// jni for guest code: its own JNIEnv and JavaVM, every entry a stub into the real ones
#include <jni.h>

#include <cstddef>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "android/runtime.hpp"

namespace tsugiki::rt {

namespace {

constexpr size_t env_slots = sizeof(JNINativeInterface) / sizeof(void*);
constexpr size_t vm_slots = sizeof(JNIInvokeInterface) / sizeof(void*);

// the guest's views, the host one riding along
struct GuestEnv {
    const uint64_t* functions;
    JNIEnv* host;
};

struct GuestVm {
    const uint64_t* functions;
    JavaVM* host;
};

uint64_t env_table[env_slots];
uint64_t vm_table[vm_slots];

#define SLOT(name) (offsetof(JNINativeInterface, name) / sizeof(void*))
#define VM_SLOT(name) (offsetof(JNIInvokeInterface, name) / sizeof(void*))

// method ids to their signatures, for the variadic calls
std::mutex signature_lock;
std::unordered_map<jmethodID, std::string> signatures;

std::string signature_of(jmethodID id) {
    std::lock_guard guard(signature_lock);
    auto it = signatures.find(id);
    return it != signatures.end() ? it->second : std::string();
}

// the host jni function in a slot, called like an import with the env swapped
void call_slot(tsugiki_state* s, size_t slot) {
    auto* guest = reinterpret_cast<GuestEnv*>(s->x[0]);
    JNIEnv* host = guest->host;
    void* f = reinterpret_cast<void* const*>(host->functions)[slot];
    s->x[0] = reinterpret_cast<uint64_t>(host);
    Stub stub{generic_call, f, 0, "jni"};
    generic_call(s, stub);
}

void plain_slot(tsugiki_state* s, const Stub& stub) {
    call_slot(s, stub.data);
}

void method_id_slot(tsugiki_state* s, const Stub& stub) {
    std::string signature = reinterpret_cast<const char*>(s->x[3]);
    call_slot(s, stub.data);
    if (s->x[0]) {
        std::lock_guard guard(signature_lock);
        signatures[reinterpret_cast<jmethodID>(s->x[0])] = signature;
    }
}

// the parameter types of a method signature, one letter each
std::string parameters(const std::string& signature) {
    std::string types;
    size_t i = signature.find('(');
    if (i == std::string::npos) {
        return types;
    }
    for (++i; i < signature.size() && signature[i] != ')';) {
        char c = signature[i];
        if (c == '[') {
            while (i < signature.size() && signature[i] == '[') ++i;
            if (i < signature.size() && signature[i] == 'L') i = signature.find(';', i);
            types += 'L';
            ++i;
        } else if (c == 'L') {
            i = signature.find(';', i) + 1;
            types += 'L';
        } else {
            types += c;
            ++i;
        }
    }
    return types;
}

// where the next variadic arg comes from: registers, then the stack
struct ArgReader {
    const tsugiki_state* s;
    int ints;
    int fps = 0;
    uint64_t stack;

    uint64_t next_int() {
        if (ints < 8) return s->x[ints++];
        uint64_t v;
        std::memcpy(&v, reinterpret_cast<void*>(stack), 8);
        stack += 8;
        return v;
    }

    double next_double() {
        if (fps < 8) {
            double d;
            std::memcpy(&d, &s->v[fps++].lo, 8);
            return d;
        }
        double d;
        std::memcpy(&d, reinterpret_cast<void*>(stack), 8);
        stack += 8;
        return d;
    }
};

template <typename Reader>
std::vector<jvalue> collect(const std::string& types, Reader&& read_int, auto&& read_double) {
    std::vector<jvalue> values(types.size() + 1);
    for (size_t i = 0; i < types.size(); ++i) {
        jvalue& v = values[i];
        std::memset(&v, 0, sizeof v);
        switch (types[i]) {
        case 'Z': v.z = jboolean(read_int()); break;
        case 'B': v.b = jbyte(read_int()); break;
        case 'C': v.c = jchar(read_int()); break;
        case 'S': v.s = jshort(read_int()); break;
        case 'I': v.i = jint(read_int()); break;
        case 'J': v.j = jlong(read_int()); break;
        case 'F': v.f = float(read_double()); break;
        case 'D': v.d = read_double(); break;
        default: v.l = reinterpret_cast<jobject>(read_int()); break;
        }
    }
    return values;
}

// CallXMethod(env, obj, [clazz], id, ...): the A variant with a jvalue array
void variadic_slot(tsugiki_state* s, const Stub& stub) {
    int fixed = int(stub.data >> 32);
    size_t a_slot = size_t(stub.data & 0xffffffff);
    auto id = reinterpret_cast<jmethodID>(s->x[fixed - 1]);
    std::string types = parameters(signature_of(id));
    ArgReader reader{s, fixed, 0, s->x[31]};
    auto values = collect(types, [&] { return reader.next_int(); }, [&] { return reader.next_double(); });
    // env obj [clazz] id values
    auto* state = const_cast<tsugiki_state*>(s);
    state->x[fixed] = reinterpret_cast<uint64_t>(values.data());
    call_slot(state, a_slot);
}

// CallXMethodV(env, obj, [clazz], id, va_list)
void va_list_slot(tsugiki_state* s, const Stub& stub) {
    int fixed = int(stub.data >> 32);
    size_t a_slot = size_t(stub.data & 0xffffffff);
    auto id = reinterpret_cast<jmethodID>(s->x[fixed - 1]);
    std::string types = parameters(signature_of(id));
    GuestVaList ap = va(s->x[fixed]);
    auto values = collect(types, [&] { return next_int(ap); }, [&] { return next_double(ap); });
    s->x[fixed] = reinterpret_cast<uint64_t>(values.data());
    call_slot(s, a_slot);
}

void register_natives_slot(tsugiki_state* s, const Stub& stub) {
    auto* methods = reinterpret_cast<const JNINativeMethod*>(s->x[2]);
    int count = int(s->x[3]);
    std::vector<JNINativeMethod> host(methods, methods + count);
    for (auto& m : host) {
        m.fnPtr = reinterpret_cast<void*>(host_callback(reinterpret_cast<uint64_t>(m.fnPtr), jni_env));
    }
    s->x[2] = reinterpret_cast<uint64_t>(host.data());
    call_slot(s, stub.data);
}

void get_java_vm_slot(tsugiki_state* s, const Stub& stub) {
    auto* out = reinterpret_cast<JavaVM**>(s->x[1]);
    call_slot(s, stub.data);
    if (s->x[0] == JNI_OK && out && *out) {
        *out = reinterpret_cast<JavaVM*>(guest_vm(*out));
    }
}

// ---- JavaVM ----

void vm_slot_call(tsugiki_state* s, size_t slot) {
    auto* guest = reinterpret_cast<GuestVm*>(s->x[0]);
    JavaVM* host = guest->host;
    void* f = reinterpret_cast<void* const*>(host->functions)[slot];
    s->x[0] = reinterpret_cast<uint64_t>(host);
    Stub stub{generic_call, f, 0, "jni vm"};
    generic_call(s, stub);
}

void vm_plain(tsugiki_state* s, const Stub& stub) {
    vm_slot_call(s, stub.data);
}

// GetEnv and AttachCurrentThread hand back an env: the guest's one
void vm_env_out(tsugiki_state* s, const Stub& stub) {
    auto* out = reinterpret_cast<void**>(s->x[1]);
    vm_slot_call(s, stub.data);
    if (s->x[0] == JNI_OK && out && *out) {
        *out = reinterpret_cast<void*>(guest_env(*out));
    }
}

struct Tables {
    Tables() {
        for (size_t i = 0; i < env_slots; ++i) {
            env_table[i] = make_stub({plain_slot, nullptr, i, "jni"});
        }
        auto special = [](size_t slot, void (*call)(tsugiki_state*, const Stub&), uint64_t data) {
            env_table[slot] = make_stub({call, nullptr, data, "jni"});
        };
        special(SLOT(GetMethodID), method_id_slot, SLOT(GetMethodID));
        special(SLOT(GetStaticMethodID), method_id_slot, SLOT(GetStaticMethodID));
        special(SLOT(RegisterNatives), register_natives_slot, SLOT(RegisterNatives));
        special(SLOT(GetJavaVM), get_java_vm_slot, SLOT(GetJavaVM));
        // each variadic call, its V and A forms follow it
        const size_t calls[] = {SLOT(CallObjectMethod), SLOT(CallBooleanMethod), SLOT(CallByteMethod), SLOT(CallCharMethod), SLOT(CallShortMethod),
                                SLOT(CallIntMethod), SLOT(CallLongMethod), SLOT(CallFloatMethod), SLOT(CallDoubleMethod), SLOT(CallVoidMethod),
                                SLOT(CallStaticObjectMethod), SLOT(CallStaticBooleanMethod), SLOT(CallStaticByteMethod), SLOT(CallStaticCharMethod),
                                SLOT(CallStaticShortMethod), SLOT(CallStaticIntMethod), SLOT(CallStaticLongMethod), SLOT(CallStaticFloatMethod),
                                SLOT(CallStaticDoubleMethod), SLOT(CallStaticVoidMethod), SLOT(NewObject)};
        for (size_t slot : calls) {
            special(slot, variadic_slot, (uint64_t(3) << 32) | (slot + 2));
            special(slot + 1, va_list_slot, (uint64_t(3) << 32) | (slot + 2));
        }
        const size_t nonvirtual[] = {SLOT(CallNonvirtualObjectMethod), SLOT(CallNonvirtualBooleanMethod), SLOT(CallNonvirtualByteMethod),
                                     SLOT(CallNonvirtualCharMethod), SLOT(CallNonvirtualShortMethod), SLOT(CallNonvirtualIntMethod),
                                     SLOT(CallNonvirtualLongMethod), SLOT(CallNonvirtualFloatMethod), SLOT(CallNonvirtualDoubleMethod),
                                     SLOT(CallNonvirtualVoidMethod)};
        for (size_t slot : nonvirtual) {
            special(slot, variadic_slot, (uint64_t(4) << 32) | (slot + 2));
            special(slot + 1, va_list_slot, (uint64_t(4) << 32) | (slot + 2));
        }
        for (size_t i = 0; i < vm_slots; ++i) {
            vm_table[i] = make_stub({vm_plain, nullptr, i, "jni vm"});
        }
        vm_table[VM_SLOT(GetEnv)] = make_stub({vm_env_out, nullptr, VM_SLOT(GetEnv), "jni vm"});
        vm_table[VM_SLOT(AttachCurrentThread)] = make_stub({vm_env_out, nullptr, VM_SLOT(AttachCurrentThread), "jni vm"});
        vm_table[VM_SLOT(AttachCurrentThreadAsDaemon)] = make_stub({vm_env_out, nullptr, VM_SLOT(AttachCurrentThreadAsDaemon), "jni vm"});
    }
};

void tables() {
    static Tables made;
}

}  // namespace

uint64_t guest_env(void* host_env) {
    if (host_env == nullptr) {
        return 0;
    }
    tables();
    // one per host env, they never go away
    static std::mutex lock;
    static std::unordered_map<void*, GuestEnv*> envs;
    std::lock_guard guard(lock);
    auto it = envs.find(host_env);
    if (it == envs.end()) {
        it = envs.emplace(host_env, new GuestEnv{env_table, static_cast<JNIEnv*>(host_env)}).first;
    }
    return reinterpret_cast<uint64_t>(it->second);
}

uint64_t guest_vm(void* host_vm) {
    if (host_vm == nullptr) {
        return 0;
    }
    tables();
    static std::mutex lock;
    static std::unordered_map<void*, GuestVm*> vms;
    std::lock_guard guard(lock);
    auto it = vms.find(host_vm);
    if (it == vms.end()) {
        it = vms.emplace(host_vm, new GuestVm{vm_table, static_cast<JavaVM*>(host_vm)}).first;
    }
    return reinterpret_cast<uint64_t>(it->second);
}

void* host_env_of(uint64_t guest) {
    return reinterpret_cast<GuestEnv*>(guest)->host;
}

uint64_t jni_import(const std::string&) {
    return 0;
}

}  // namespace tsugiki::rt
