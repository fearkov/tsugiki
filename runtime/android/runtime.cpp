// the runtime on android: modules, dispatcher, threads, entries, imports
#include "android/runtime.hpp"

#include <android/log.h>
#include <dlfcn.h>
#include <link.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <sys/auxv.h>
#include <sys/mman.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "elf.hpp"

extern "C" char tsugiki_raw_entry[];

namespace tsugiki::rt {

void log(const char* what, ...) {
    va_list args;
    va_start(args, what);
    __android_log_vprint(ANDROID_LOG_INFO, "tsugiki", what, args);
    va_end(args);
}

void fail(const char* what, ...) {
    char text[512];
    va_list args;
    va_start(args, what);
    std::vsnprintf(text, sizeof text, what, args);
    va_end(args);
    __android_log_print(ANDROID_LOG_FATAL, "tsugiki", "%s", text);
    std::fprintf(stderr, "tsugiki: %s\n", text);
    abort();
}

// ---- stubs: guest addresses for host things ----

namespace {

constexpr uint64_t stub_size = 16;
constexpr uint64_t most_stubs = 1 << 16;

struct Stubs {
    uint64_t base = 0;
    std::vector<Stub> all;
    std::mutex lock;
    Stubs() {
        // never mapped readable, just unique addresses no code lives at
        void* region = mmap(nullptr, most_stubs * stub_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (region == MAP_FAILED) {
            fail("can't reserve stub addresses");
        }
        base = reinterpret_cast<uint64_t>(region);
        all.reserve(most_stubs);
        // stub 0 is where returns to the host land
        all.push_back({nullptr, nullptr, 0, "stop"});
    }
};

Stubs& stubs() {
    static Stubs* s = new Stubs;
    return *s;
}

}  // namespace

uint64_t make_stub(const Stub& stub) {
    Stubs& s = stubs();
    std::lock_guard guard(s.lock);
    if (s.all.size() >= most_stubs) {
        fail("out of stubs");
    }
    s.all.push_back(stub);
    return s.base + (s.all.size() - 1) * stub_size;
}

uint64_t stop_address() {
    return stubs().base;
}

// ---- modules ----

namespace {

struct Loaded {
    tsugiki_module* module = nullptr;
    std::unique_ptr<elf::Library> library;
    uint64_t base = 0;
    uint64_t size = 0;
    std::unordered_map<std::string, uint64_t> exports;
    // for dl_iterate_phdr, the guest's unwinder finds eh_frame through these
    std::vector<Elf64_Phdr> phdrs;
};

struct Modules {
    std::mutex lock;
    // never shrinks, readers walk it without the lock
    std::atomic<Loaded**> list{nullptr};
    std::atomic<size_t> count{0};
    std::vector<Loaded*> owned;
};

Modules& modules() {
    static Modules* m = new Modules;
    return *m;
}

tsugiki_code find(uint64_t address) {
    Modules& all = modules();
    Loaded** list = all.list.load(std::memory_order_acquire);
    size_t count = all.count.load(std::memory_order_acquire);
    for (size_t i = 0; i < count; ++i) {
        Loaded* l = list[i];
        if (address - l->base < l->size) {
            uint64_t offset = address - l->base;
            const tsugiki_entry* begin = l->module->entries;
            const tsugiki_entry* end = begin + l->module->count;
            const tsugiki_entry* e = std::lower_bound(begin, end, offset, [](const tsugiki_entry& a, uint64_t o) { return a.address < o; });
            return e != end && e->address == offset ? e->code : nullptr;
        }
    }
    return nullptr;
}

void add_module(Loaded* l) {
    Modules& all = modules();
    std::lock_guard guard(all.lock);
    all.owned.push_back(l);
    // a fresh array each time, old ones leak on purpose so readers stay safe
    Loaded** list = new Loaded*[all.owned.size()];
    std::copy(all.owned.begin(), all.owned.end(), list);
    all.list.store(list, std::memory_order_release);
    all.count.store(all.owned.size(), std::memory_order_release);
}

uint64_t guest_export(const std::string& name) {
    Modules& all = modules();
    Loaded** list = all.list.load(std::memory_order_acquire);
    size_t count = all.count.load(std::memory_order_acquire);
    for (size_t i = 0; i < count; ++i) {
        auto it = list[i]->exports.find(name);
        if (it != list[i]->exports.end()) {
            return it->second;
        }
    }
    return 0;
}

}  // namespace

uint64_t lookup_guest(const std::string& name) {
    return guest_export(name);
}

std::vector<GuestModule> guest_modules() {
    Modules& all = modules();
    Loaded** list = all.list.load(std::memory_order_acquire);
    size_t count = all.count.load(std::memory_order_acquire);
    std::vector<GuestModule> out;
    for (size_t i = 0; i < count; ++i) {
        const Loaded* l = list[i];
        out.push_back({l->base, l->size, l->module->soname, l->phdrs.data(), uint16_t(l->phdrs.size())});
    }
    return out;
}

bool guest_dladdr(uint64_t address, Dl_info* info) {
    Modules& all = modules();
    Loaded** list = all.list.load(std::memory_order_acquire);
    size_t count = all.count.load(std::memory_order_acquire);
    for (size_t i = 0; i < count; ++i) {
        const Loaded* l = list[i];
        if (address - l->base >= l->size) {
            continue;
        }
        uint64_t offset = address - l->base;
        info->dli_fname = l->module->soname;
        info->dli_fbase = reinterpret_cast<void*>(l->base);
        info->dli_sname = nullptr;
        info->dli_saddr = nullptr;
        // the closest symbol at or below it
        const elf::Symbol* best = nullptr;
        for (const elf::Symbol& sym : l->library->dynamic_symbols) {
            if (sym.defined() && !sym.name.empty() && sym.value <= offset && (!best || sym.value > best->value) &&
                (sym.size == 0 || offset < sym.value + sym.size)) {
                best = &sym;
            }
        }
        if (best) {
            info->dli_sname = best->name.c_str();
            info->dli_saddr = reinterpret_cast<void*>(l->base + best->value);
        }
        return true;
    }
    return false;
}

namespace {

// whether a host address is code, by its library's segments
bool host_code(void* address) {
    struct Search {
        uint64_t address;
        bool code;
    } search{reinterpret_cast<uint64_t>(address), false};
    dl_iterate_phdr(
        [](dl_phdr_info* info, size_t, void* data) {
            auto* s = static_cast<Search*>(data);
            for (int i = 0; i < info->dlpi_phnum; ++i) {
                const auto& ph = info->dlpi_phdr[i];
                if (ph.p_type == PT_LOAD && s->address - (info->dlpi_addr + ph.p_vaddr) < ph.p_memsz) {
                    s->code = (ph.p_flags & PF_X) != 0;
                    return 1;
                }
            }
            return 0;
        },
        &search);
    return search.code;
}

bool host_code_cached(uint64_t address) {
    thread_local std::unordered_map<uint64_t, bool> known;
    auto it = known.find(address);
    if (it == known.end()) {
        it = known.emplace(address, host_code(reinterpret_cast<void*>(address))).first;
    }
    return it->second;
}

void missing_call(tsugiki_state* s, const Stub& stub) {
    (void)s;
    fail("guest called %s, which nothing provides", stub.name);
}

// an import by name: other translated libs, our special thunks, then the host
uint64_t resolve(const Loaded& l, const std::string& name, bool weak) {
    if (uint64_t guest = guest_export(name)) {
        return guest;
    }
    if (uint64_t special = special_import(name)) {
        return special;
    }
    if (uint64_t jni = jni_import(name)) {
        return jni;
    }
    void* host = dlsym(RTLD_DEFAULT, name.c_str());
    if (host == nullptr) {
        // libs it needs that nothing loaded yet
        for (const std::string& needed : l.library->needed) {
            if (void* handle = dlopen(needed.c_str(), RTLD_NOW | RTLD_NOLOAD)) {
                host = dlsym(handle, name.c_str());
            } else if (void* opened = dlopen(needed.c_str(), RTLD_NOW)) {
                host = dlsym(opened, name.c_str());
            }
            if (host) {
                break;
            }
        }
    }
    if (host == nullptr) {
        if (weak) {
            return 0;
        }
        log("%s: nothing provides %s", l.module->soname, name.c_str());
        return make_stub({missing_call, nullptr, 0, strdup(name.c_str())});
    }
    if (!host_code(host)) {
        // data, the guest reads it where it is
        return reinterpret_cast<uint64_t>(host);
    }
    return make_stub({generic_call, host, 0, strdup(name.c_str())});
}

}  // namespace

// ---- threads ----

namespace {

constexpr size_t guest_stack_size = 2 << 20;

struct Thread {
    alignas(16) tsugiki_state state{};
    uint8_t* stack = nullptr;
    int depth = 0;
};

uint64_t thread_pointer() {
    uint64_t tp;
    asm("mov %%fs:0, %0" : "=r"(tp));
    return tp;
}

thread_local Thread* current = nullptr;

Thread& thread() {
    if (current == nullptr) {
        auto* t = new Thread;
        void* stack = mmap(nullptr, guest_stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
        if (stack == MAP_FAILED) {
            fail("no memory for a guest stack");
        }
        // a guard page at the bottom
        mprotect(stack, 4096, PROT_NONE);
        t->stack = static_cast<uint8_t*>(stack);
        // some room above, host calls read 16 stack words whether they're there or not
        t->state.x[31] = reinterpret_cast<uint64_t>(t->stack + guest_stack_size - 512);
        // bionic keeps the stack guard in tls slot 5 on both, so share the host's
        t->state.tpidr = thread_pointer();
        current = t;
    }
    return *current;
}

}  // namespace

tsugiki_state* guest_state() {
    return &thread().state;
}

uint64_t int_arg(const tsugiki_state* s, int n) {
    if (n < 8) {
        return s->x[n];
    }
    uint64_t v;
    std::memcpy(&v, reinterpret_cast<const void*>(s->x[31] + 8 * uint64_t(n - 8)), 8);
    return v;
}

uint64_t run_guest(uint64_t function, const Call& call, uint64_t* fp_result) {
    Thread& t = thread();
    tsugiki_state* s = &t.state;
    // nested: the guest is in a host call further out, keep its registers
    std::unique_ptr<tsugiki_state> saved;
    if (t.depth > 0) {
        saved = std::make_unique<tsugiki_state>(*s);
    }
    ++t.depth;
    uint64_t sp = s->x[31] & ~uint64_t(15);
    int pushed = (call.stack_count + 1) & ~1;
    sp -= 8 * uint64_t(pushed);
    for (int i = 0; i < call.stack_count; ++i) {
        std::memcpy(reinterpret_cast<void*>(sp + 8 * uint64_t(i)), &call.stack[i], 8);
    }
    s->x[31] = sp;
    for (int i = 0; i < 8; ++i) {
        s->x[i] = call.ints[i];
        s->v[i].lo = call.fps[i];
        s->v[i].hi = 0;
    }
    s->x[30] = stop_address();
    s->pc = function;
    while (s->pc != stop_address()) {
        tsugiki_call(s);
    }
    uint64_t result = s->x[0];
    if (fp_result) {
        *fp_result = s->v[0].lo;
    }
    --t.depth;
    if (saved) {
        *s = *saved;
    } else {
        s->x[31] = sp + 8 * uint64_t(pushed);
    }
    return result;
}

// ---- the generic import call ----

namespace {

struct HostCall {
    uint64_t ints[6];
    uint64_t fps[8];
    const uint64_t* stack;
    uint64_t count;
    uint64_t rax, rdx;
    uint64_t xmm0[2], xmm1[2];
};
static_assert(offsetof(HostCall, stack) == 112 && offsetof(HostCall, rax) == 128 && offsetof(HostCall, xmm1) == 160);

}  // namespace

}  // namespace tsugiki::rt

extern "C" void tsugiki_host_call(void* function, tsugiki::rt::HostCall* call);

namespace tsugiki::rt {

// six ints in registers, the guest's x6 x7 and stack words on the host stack, doubles in xmm0..7;
// rax:rdx and xmm0:xmm1 come back, for 16-byte structs too
void generic_call(tsugiki_state* s, const Stub& stub) {
    uint64_t back = s->x[30];
    HostCall c;
    for (int i = 0; i < 6; ++i) {
        c.ints[i] = s->x[i];
    }
    for (int i = 0; i < 8; ++i) {
        c.fps[i] = s->v[i].lo;
    }
    uint64_t extra[16];
    for (int i = 0; i < 16; ++i) {
        extra[i] = int_arg(s, 6 + i);
    }
    c.stack = extra;
    c.count = 16;
    tsugiki_host_call(stub.host, &c);
    s->x[0] = c.rax;
    s->x[1] = c.rdx;
    s->v[0].lo = c.xmm0[0];
    s->v[0].hi = 0;
    s->v[1].lo = c.xmm1[0];
    s->v[1].hi = 0;
    s->pc = back;
}

// ---- host code jumping into guest code through plain pointers ----

namespace {

struct sigaction previous_segv;

bool in_guest_image(uint64_t address) {
    Modules& all = modules();
    Loaded** list = all.list.load(std::memory_order_acquire);
    size_t count = all.count.load(std::memory_order_acquire);
    for (size_t i = 0; i < count; ++i) {
        if (address - list[i]->base < list[i]->size) {
            return true;
        }
    }
    return false;
}

// the image isn't executable, so a host call into it faults right at the target:
// point rip at an entry that runs the guest function instead, args and return address untouched
void on_segv(int signal, siginfo_t* info, void* context) {
    auto* uc = static_cast<ucontext_t*>(context);
    uint64_t at = uint64_t(uc->uc_mcontext.gregs[REG_RIP]);
    if (reinterpret_cast<uint64_t>(info->si_addr) == at) {
        if (in_guest_image(at)) {
            uc->uc_mcontext.gregs[REG_R10] = greg_t(at);
            uc->uc_mcontext.gregs[REG_RIP] = greg_t(reinterpret_cast<uint64_t>(tsugiki_raw_entry));
            return;
        }
        // a stub handed to the host, like &strlen: run what's behind it
        Stubs& st = stubs();
        uint64_t offset = at - st.base;
        if (offset < most_stubs * stub_size && offset % stub_size == 0 && offset / stub_size < st.all.size()) {
            const Stub& stub = st.all[offset / stub_size];
            if (stub.call == generic_call && stub.host) {
                uc->uc_mcontext.gregs[REG_RIP] = greg_t(reinterpret_cast<uint64_t>(stub.host));
                return;
            }
        }
    }
    // not ours
    if (previous_segv.sa_flags & SA_SIGINFO) {
        if (previous_segv.sa_sigaction) {
            previous_segv.sa_sigaction(signal, info, context);
            return;
        }
    } else if (previous_segv.sa_handler != SIG_DFL && previous_segv.sa_handler != SIG_IGN) {
        previous_segv.sa_handler(signal);
        return;
    }
    // the fault comes back and kills us the normal way
    struct sigaction normal {};
    normal.sa_handler = SIG_DFL;
    sigaction(signal, &normal, nullptr);
}

void watch_segv() {
    static std::once_flag once;
    std::call_once(once, [] {
        struct sigaction action {};
        action.sa_sigaction = on_segv;
        action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&action.sa_mask);
        sigaction(SIGSEGV, &action, &previous_segv);
    });
}

}  // namespace

// ---- loading ----

namespace {

void run_init(uint64_t function) {
    if (function == 0) {
        return;
    }
    Call call;
    run_guest(function, call);
}

}  // namespace

}  // namespace tsugiki::rt

using namespace tsugiki;
using namespace tsugiki::rt;

extern "C" void tsugiki_load(tsugiki_module* m) {
    if (m->loaded) {
        return;
    }
    watch_segv();
    auto* l = new Loaded;
    l->module = m;
    try {
        l->library = std::make_unique<elf::Library>(std::vector<uint8_t>(m->image, m->image + m->image_size), m->soname);
    } catch (const std::exception& e) {
        fail("%s: %s", m->soname, e.what());
    }
    const elf::Library& lib = *l->library;
    uint64_t end = 0;
    for (const elf::Segment& s : lib.segments()) {
        if (s.type == elf::segment::load) {
            end = std::max(end, s.vaddr + s.memsz);
        }
    }
    constexpr uint64_t align = 0x10000;
    uint64_t size = (end + align - 1) & ~(align - 1);
    void* raw = mmap(nullptr, size + align, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) {
        fail("%s: no memory for the image", m->soname);
    }
    uint64_t base = (reinterpret_cast<uint64_t>(raw) + align - 1) & ~(align - 1);
    for (const elf::Segment& s : lib.segments()) {
        if (s.type == elf::segment::load && s.filesz) {
            std::memcpy(reinterpret_cast<void*>(base + s.vaddr), lib.bytes().data() + s.offset, s.filesz);
        }
    }
    l->base = base;
    l->size = size;
    *m->base = base;
    for (const elf::Segment& seg : lib.segments()) {
        Elf64_Phdr ph{};
        ph.p_type = seg.type;
        ph.p_flags = seg.flags;
        ph.p_offset = seg.offset;
        ph.p_vaddr = seg.vaddr;
        ph.p_paddr = seg.vaddr;
        ph.p_filesz = seg.filesz;
        ph.p_memsz = seg.memsz;
        ph.p_align = seg.align;
        l->phdrs.push_back(ph);
    }
    for (size_t i = 1; i < lib.dynamic_symbols.size(); ++i) {
        const elf::Symbol& s = lib.dynamic_symbols[i];
        if (s.defined() && s.bind != elf::symbol::local && !s.name.empty()) {
            l->exports.emplace(s.name, base + s.value);
        }
    }
    // imports, each name once
    std::unordered_map<uint32_t, uint64_t> resolved;
    for (const elf::Relocation& x : lib.relocations) {
        auto* slot = reinterpret_cast<uint64_t*>(base + x.offset);
        uint64_t target = 0;
        if (x.symbol) {
            const elf::Symbol& s = lib.dynamic_symbols[x.symbol];
            if (s.defined()) {
                target = base + s.value;
            } else {
                auto known = resolved.find(x.symbol);
                if (known == resolved.end()) {
                    known = resolved.emplace(x.symbol, resolve(*l, s.name, s.bind == elf::symbol::weak)).first;
                }
                target = known->second;
            }
        }
        switch (x.type) {
        case elf::relocation::relative:
            *slot = base + uint64_t(x.addend);
            break;
        case elf::relocation::abs64:
        case elf::relocation::glob_dat:
        case elf::relocation::jump_slot:
            *slot = target + uint64_t(x.addend);
            break;
        case elf::relocation::none:
            break;
        default:
            log("%s: relocation %s at 0x%llx not handled yet", m->soname, elf::relocation::name(x.type).c_str(), (unsigned long long)x.offset);
            break;
        }
    }
    m->loaded = l;
    add_module(l);
    log("%s: loaded at 0x%llx, %llu entries", m->soname, (unsigned long long)base, (unsigned long long)m->count);
    // its constructors, in order
    run_init(lib.init ? base + lib.init : 0);
    for (uint64_t f : lib.init_array) {
        run_init(f ? base + f : 0);
    }
}

extern "C" int tsugiki_main(tsugiki_module* m, int argc, char** argv, char** envp) {
    tsugiki_load(m);
    auto* l = static_cast<Loaded*>(m->loaded);
    // what the kernel leaves on the stack: argc, argv, envp, auxv
    std::vector<uint64_t> block;
    block.push_back(uint64_t(argc));
    for (int i = 0; i < argc; ++i) {
        block.push_back(reinterpret_cast<uint64_t>(argv[i]));
    }
    block.push_back(0);
    for (char** e = envp; e && *e; ++e) {
        block.push_back(reinterpret_cast<uint64_t>(*e));
    }
    block.push_back(0);
    for (uint64_t type : {AT_PAGESZ, AT_HWCAP, AT_HWCAP2, AT_RANDOM, AT_SECURE}) {
        block.push_back(type);
        block.push_back(type == AT_PAGESZ ? 4096 : type == AT_RANDOM ? getauxval(AT_RANDOM) : 0);
    }
    block.push_back(AT_NULL);
    block.push_back(0);
    // _start does mov x0, sp, so the block sits right at sp
    tsugiki_state* s = guest_state();
    uint64_t sp = (s->x[31] - 8 * block.size()) & ~uint64_t(15);
    std::memcpy(reinterpret_cast<void*>(sp), block.data(), 8 * block.size());
    s->x[31] = sp;
    Call call;
    run_guest(l->base + l->library->entry(), call);
    // __libc_init exits, so not here normally
    return 0;
}

extern "C" void tsugiki_call(tsugiki_state* s) {
    Stubs& st = stubs();
    uint64_t offset = s->pc - st.base;
    if (offset < most_stubs * stub_size) {
        const Stub& stub = st.all[offset / stub_size];
        if (stub.call == nullptr) {
            fail("returned to the host through tsugiki_call");
        }
        stub.call(s, stub);
        return;
    }
    tsugiki_code code = find(s->pc);
    if (code == nullptr) {
        // one of our callback slots handed back: straight to its guest function
        if (uint64_t guest = callback_target(s->pc)) {
            s->pc = guest;
            tsugiki_call(s);
            return;
        }
        // host code through a plain pointer: vtables of host apis, eglGetProcAddress and such
        if (host_code_cached(s->pc)) {
            Stub stub{generic_call, reinterpret_cast<void*>(s->pc), 0, "host"};
            generic_call(s, stub);
            return;
        }
        fail("nothing to run at 0x%llx (lr 0x%llx)", (unsigned long long)s->pc, (unsigned long long)s->x[30]);
    }
    code(s);
}

extern "C" void tsugiki_trap(tsugiki_state* s, uint64_t pc, uint32_t raw, const char* why) {
    fail("%s at 0x%llx (0x%08x), lr 0x%llx", why, (unsigned long long)pc, raw, (unsigned long long)s->x[30]);
}
