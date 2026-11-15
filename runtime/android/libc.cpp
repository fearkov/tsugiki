// libc imports the generic call can't do: va_lists, callbacks, setjmp, dl*, auxv, syscall
#include <android/log.h>
#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
#include <stdarg.h>
#include <sys/auxv.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include "android/runtime.hpp"

extern "C" int __cxa_thread_atexit_impl(void (*)(void*), void*, void*);

extern "C" int __cxa_atexit(void (*)(void*), void*, void*);

namespace tsugiki::rt {

uint64_t next_int(GuestVaList& ap) {
    int32_t offs = ap.gr_offs;
    if (offs < 0) {
        ap.gr_offs = offs + 8;
        if (ap.gr_offs <= 0) {
            uint64_t v;
            std::memcpy(&v, reinterpret_cast<void*>(ap.gr_top + int64_t(offs)), 8);
            return v;
        }
    }
    uint64_t v;
    std::memcpy(&v, reinterpret_cast<void*>(ap.stack), 8);
    ap.stack += 8;
    return v;
}

double next_double(GuestVaList& ap) {
    int32_t offs = ap.vr_offs;
    if (offs < 0) {
        ap.vr_offs = offs + 16;
        if (ap.vr_offs <= 0) {
            double v;
            std::memcpy(&v, reinterpret_cast<void*>(ap.vr_top + int64_t(offs)), 8);
            return v;
        }
    }
    double v;
    std::memcpy(&v, reinterpret_cast<void*>(ap.stack), 8);
    ap.stack += 8;
    return v;
}

GuestVaList va(uint64_t pointer) {
    GuestVaList ap;
    std::memcpy(&ap, reinterpret_cast<void*>(pointer), sizeof ap);
    return ap;
}

namespace {

// back to the guest that called
void ret(tsugiki_state* s, uint64_t back, uint64_t value) {
    s->x[0] = value;
    s->pc = back;
}

template <typename T>
T* ptr(uint64_t v) {
    return reinterpret_cast<T*>(v);
}

// arm64 long double is ieee quad, good enough as a double here
double next_quad(GuestVaList& ap) {
    uint8_t q[16];
    int32_t offs = ap.vr_offs;
    if (offs < 0 && offs + 16 <= 0) {
        ap.vr_offs = offs + 16;
        std::memcpy(q, ptr<void>(ap.vr_top + int64_t(offs)), 16);
    } else {
        ap.vr_offs = offs < 0 ? offs + 16 : offs;
        ap.stack = (ap.stack + 15) & ~uint64_t(15);
        std::memcpy(q, ptr<void>(ap.stack), 16);
        ap.stack += 16;
    }
    uint64_t lo, hi;
    std::memcpy(&lo, q, 8);
    std::memcpy(&hi, q + 8, 8);
    uint64_t sign = hi >> 63;
    int64_t exp = int64_t(hi >> 48 & 0x7fff);
    uint64_t mantissa = (hi << 16) | (lo >> 48);
    uint64_t bits;
    if (exp == 0x7fff) {
        bits = (sign << 63) | (uint64_t(0x7ff) << 52) | (mantissa >> 12);
    } else if (exp == 0) {
        bits = sign << 63;
    } else {
        int64_t e = exp - 16383 + 1023;
        bits = e >= 0x7ff ? (sign << 63) | (uint64_t(0x7ff) << 52) : e <= 0 ? sign << 63 : (sign << 63) | (uint64_t(e) << 52) | (mantissa >> 12);
    }
    double d;
    std::memcpy(&d, &bits, 8);
    return d;
}

// printf's formatting from a guest va_list, one conversion at a time through the host's snprintf
std::string format(const char* fmt, GuestVaList ap) {
    std::string out;
    for (const char* p = fmt; *p;) {
        if (*p != '%') {
            out += *p++;
            continue;
        }
        const char* start = p++;
        if (*p == '%') {
            out += '%';
            ++p;
            continue;
        }
        std::string spec = "%";
        int stars[2], star_count = 0;
        while (std::strchr("-+ #0'", *p)) spec += *p++;
        if (*p == '*') {
            stars[star_count++] = int(next_int(ap));
            spec += '*';
            ++p;
        }
        while (*p >= '0' && *p <= '9') spec += *p++;
        if (*p == '.') {
            spec += *p++;
            if (*p == '*') {
                stars[star_count++] = int(next_int(ap));
                spec += '*';
                ++p;
            }
            while (*p >= '0' && *p <= '9') spec += *p++;
        }
        std::string length;
        while (std::strchr("hljztLq", *p) && *p) length += *p++;
        char conversion = *p ? *p++ : 0;
        if (conversion == 0) {
            out.append(start);
            break;
        }
        std::string host = spec;
        if (length == "L" || length == "q") {
            host += "";  // printed as a double below
        } else {
            host += length;
        }
        host += conversion;
        int n = 0;
        auto print = [&](auto value) {
            if (star_count == 2) {
                n = std::snprintf(nullptr, 0, host.c_str(), stars[0], stars[1], value);
                std::string text(size_t(n) + 1, '\0');
                std::snprintf(text.data(), text.size(), host.c_str(), stars[0], stars[1], value);
                out.append(text.c_str(), size_t(n));
            } else if (star_count == 1) {
                n = std::snprintf(nullptr, 0, host.c_str(), stars[0], value);
                std::string text(size_t(n) + 1, '\0');
                std::snprintf(text.data(), text.size(), host.c_str(), stars[0], value);
                out.append(text.c_str(), size_t(n));
            } else {
                n = std::snprintf(nullptr, 0, host.c_str(), value);
                std::string text(size_t(n) + 1, '\0');
                std::snprintf(text.data(), text.size(), host.c_str(), value);
                out.append(text.c_str(), size_t(n));
            }
        };
        switch (conversion) {
        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': case 'c':
            if (length.empty() || length == "h" || length == "hh") {
                print(int(next_int(ap)));
            } else {
                print(static_cast<long long>(next_int(ap)));
                // the host spec says l/ll/j/z/t, all 64-bit here
            }
            break;
        case 's': case 'p':
            print(ptr<void>(next_int(ap)));
            break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
            print(length == "L" || length == "q" ? next_quad(ap) : next_double(ap));
            break;
        case 'n': {
            uint64_t where = next_int(ap);
            size_t done = out.size();
            if (length == "hh") *ptr<signed char>(where) = (signed char)done;
            else if (length == "h") *ptr<short>(where) = (short)done;
            else if (length.empty()) *ptr<int>(where) = (int)done;
            else *ptr<long long>(where) = (long long)done;
            break;
        }
        case 'm':
            out += std::strerror(errno);
            break;
        default:
            out.append(start, size_t(p - start));
            break;
        }
    }
    return out;
}

void vfprintf_thunk(tsugiki_state* s, const Stub&) {
    std::string text = format(ptr<char>(s->x[1]), va(s->x[2]));
    std::fwrite(text.data(), 1, text.size(), ptr<FILE>(s->x[0]));
    ret(s, s->x[30], text.size());
}

void vprintf_thunk(tsugiki_state* s, const Stub&) {
    std::string text = format(ptr<char>(s->x[0]), va(s->x[1]));
    std::fwrite(text.data(), 1, text.size(), stdout);
    ret(s, s->x[30], text.size());
}

void vsprintf_thunk(tsugiki_state* s, const Stub&) {
    std::string text = format(ptr<char>(s->x[1]), va(s->x[2]));
    std::memcpy(ptr<char>(s->x[0]), text.c_str(), text.size() + 1);
    ret(s, s->x[30], text.size());
}

// __vsprintf_chk(buf, flags, slen, fmt, ap)
void vsprintf_chk_thunk(tsugiki_state* s, const Stub&) {
    std::string text = format(ptr<char>(s->x[3]), va(s->x[4]));
    if (text.size() >= s->x[2]) {
        fail("vsprintf overflowed its buffer");
    }
    std::memcpy(ptr<char>(s->x[0]), text.c_str(), text.size() + 1);
    ret(s, s->x[30], text.size());
}

void copy_bounded(uint64_t buffer, uint64_t size, const std::string& text) {
    if (size == 0) {
        return;
    }
    size_t n = std::min<size_t>(text.size(), size - 1);
    std::memcpy(ptr<char>(buffer), text.data(), n);
    ptr<char>(buffer)[n] = 0;
}

void vsnprintf_thunk(tsugiki_state* s, const Stub&) {
    std::string text = format(ptr<char>(s->x[2]), va(s->x[3]));
    copy_bounded(s->x[0], s->x[1], text);
    ret(s, s->x[30], text.size());
}

// __vsnprintf_chk(buf, maxlen, flags, slen, fmt, ap)
void vsnprintf_chk_thunk(tsugiki_state* s, const Stub&) {
    std::string text = format(ptr<char>(s->x[4]), va(s->x[5]));
    copy_bounded(s->x[0], s->x[1], text);
    ret(s, s->x[30], text.size());
}

void vasprintf_thunk(tsugiki_state* s, const Stub&) {
    std::string text = format(ptr<char>(s->x[1]), va(s->x[2]));
    char* copy = static_cast<char*>(std::malloc(text.size() + 1));
    std::memcpy(copy, text.c_str(), text.size() + 1);
    *ptr<char*>(s->x[0]) = copy;
    ret(s, s->x[30], text.size());
}

void vdprintf_thunk(tsugiki_state* s, const Stub&) {
    std::string text = format(ptr<char>(s->x[1]), va(s->x[2]));
    ret(s, s->x[30], uint64_t(::write(int(s->x[0]), text.data(), text.size())));
}

void android_log_vprint_thunk(tsugiki_state* s, const Stub&) {
    std::string text = format(ptr<char>(s->x[2]), va(s->x[3]));
    ret(s, s->x[30], uint64_t(__android_log_write(int(s->x[0]), ptr<char>(s->x[1]), text.c_str())));
}

// scanf family from a va_list: every argument is a pointer
void vsscanf_thunk(tsugiki_state* s, const Stub&) {
    GuestVaList ap = va(s->x[2]);
    uint64_t p[16];
    for (auto& v : p) v = next_int(ap);
    int n = std::sscanf(ptr<char>(s->x[0]), ptr<char>(s->x[1]), p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13],
                        p[14], p[15]);
    ret(s, s->x[30], uint64_t(int64_t(n)));
}

// ---- callbacks ----

void qsort_thunk(tsugiki_state* s, const Stub&) {
    uint64_t back = s->x[30];
    auto compare = reinterpret_cast<int (*)(const void*, const void*)>(host_callback(s->x[3], plain));
    std::qsort(ptr<void>(s->x[0]), s->x[1], s->x[2], compare);
    s->pc = back;
}

void bsearch_thunk(tsugiki_state* s, const Stub&) {
    uint64_t back = s->x[30];
    auto compare = reinterpret_cast<int (*)(const void*, const void*)>(host_callback(s->x[4], plain));
    void* found = std::bsearch(ptr<void>(s->x[0]), ptr<void>(s->x[1]), s->x[2], s->x[3], compare);
    ret(s, back, reinterpret_cast<uint64_t>(found));
}

void cxa_atexit_thunk(tsugiki_state* s, const Stub&) {
    auto f = reinterpret_cast<void (*)(void*)>(host_callback(s->x[0], plain));
    ret(s, s->x[30], uint64_t(int64_t(__cxa_atexit(f, ptr<void>(s->x[1]), ptr<void>(s->x[2])))));
}

void atexit_thunk(tsugiki_state* s, const Stub&) {
    auto f = reinterpret_cast<void (*)()>(host_callback(s->x[0], plain));
    ret(s, s->x[30], uint64_t(int64_t(std::atexit(f))));
}

void cxa_finalize_thunk(tsugiki_state* s, const Stub&) {
    s->pc = s->x[30];
}

void pthread_create_thunk(tsugiki_state* s, const Stub&) {
    auto start = reinterpret_cast<void* (*)(void*)>(host_callback(s->x[2], plain));
    int r = pthread_create(ptr<pthread_t>(s->x[0]), ptr<pthread_attr_t>(s->x[1]), start, ptr<void>(s->x[3]));
    ret(s, s->x[30], uint64_t(int64_t(r)));
}

void pthread_once_thunk(tsugiki_state* s, const Stub&) {
    uint64_t back = s->x[30];
    auto init = reinterpret_cast<void (*)()>(host_callback(s->x[1], plain));
    int r = pthread_once(ptr<pthread_once_t>(s->x[0]), init);
    ret(s, back, uint64_t(int64_t(r)));
}

void pthread_key_create_thunk(tsugiki_state* s, const Stub&) {
    auto destructor = s->x[1] ? reinterpret_cast<void (*)(void*)>(host_callback(s->x[1], plain)) : nullptr;
    ret(s, s->x[30], uint64_t(int64_t(pthread_key_create(ptr<pthread_key_t>(s->x[0]), destructor))));
}

// ---- setjmp and longjmp, on guest registers ----

// our own layout in the guest's jmp_buf: x19..x30, sp, d8..d15
void setjmp_thunk(tsugiki_state* s, const Stub&) {
    auto* buf = ptr<uint64_t>(s->x[0]);
    for (int i = 0; i < 12; ++i) buf[i] = s->x[19 + i];
    buf[12] = s->x[31];
    for (int i = 0; i < 8; ++i) buf[13 + i] = s->v[8 + i].lo;
    ret(s, s->x[30], 0);
}

void longjmp_thunk(tsugiki_state* s, const Stub&) {
    auto* buf = ptr<uint64_t>(s->x[0]);
    uint64_t value = s->x[1] ? s->x[1] : 1;
    for (int i = 0; i < 12; ++i) s->x[19 + i] = buf[i];
    s->x[31] = buf[12];
    for (int i = 0; i < 8; ++i) s->v[8 + i].lo = buf[13 + i], s->v[8 + i].hi = 0;
    // back where setjmp returned, the dispatcher finds its return point
    ret(s, s->x[30], value);
}

// ---- auxv ----

void getauxval_thunk(tsugiki_state* s, const Stub&) {
    uint64_t type = s->x[0], value;
    if (type == AT_HWCAP) {
        // what we translate: fp asimd aes pmull sha1 sha2 crc32 atomics jscvt lrcpc dcpop asimddp ilrcpc
        value = (1u << 0) | (1u << 1) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7) | (1u << 8) | (1u << 13) | (1u << 15) | (1u << 16) |
                (1u << 20) | (1u << 26);
    } else if (type == AT_HWCAP2) {
        value = 0;
    } else {
        value = getauxval(type);
    }
    ret(s, s->x[30], value);
}

// ---- syscalls, arm64 numbers to x86_64 ----

}  // namespace

long host_syscall_number(uint64_t arm) {
    switch (arm) {
    case 17: return SYS_getcwd;
    case 23: return SYS_dup;
    case 24: return SYS_dup3;
    case 25: return SYS_fcntl;
    case 29: return SYS_ioctl;
    case 34: return SYS_mkdirat;
    case 35: return SYS_unlinkat;
    case 48: return SYS_faccessat;
    case 56: return SYS_openat;
    case 57: return SYS_close;
    case 61: return SYS_getdents64;
    case 62: return SYS_lseek;
    case 63: return SYS_read;
    case 64: return SYS_write;
    case 65: return SYS_readv;
    case 66: return SYS_writev;
    case 67: return SYS_pread64;
    case 68: return SYS_pwrite64;
    case 78: return SYS_readlinkat;
    case 93: return SYS_exit;
    case 94: return SYS_exit_group;
    case 96: return SYS_set_tid_address;
    case 98: return SYS_futex;
    case 101: return SYS_nanosleep;
    case 113: return SYS_clock_gettime;
    case 114: return SYS_clock_getres;
    case 115: return SYS_clock_nanosleep;
    case 123: return SYS_sched_getaffinity;
    case 124: return SYS_sched_yield;
    case 129: return SYS_kill;
    case 130: return SYS_tkill;
    case 131: return SYS_tgkill;
    case 167: return SYS_prctl;
    case 169: return SYS_gettimeofday;
    case 172: return SYS_getpid;
    case 173: return SYS_getppid;
    case 174: return SYS_getuid;
    case 175: return SYS_geteuid;
    case 176: return SYS_getgid;
    case 177: return SYS_getegid;
    case 178: return SYS_gettid;
    case 179: return SYS_sysinfo;
    case 214: return SYS_brk;
    case 215: return SYS_munmap;
    case 216: return SYS_mremap;
    case 222: return SYS_mmap;
    case 226: return SYS_mprotect;
    case 227: return SYS_msync;
    case 232: return SYS_mincore;
    case 233: return SYS_madvise;
    case 278: return SYS_getrandom;
    case 279: return SYS_memfd_create;
    case 283: return SYS_membarrier;
    default: return -1;
    }
}

namespace {

void syscall_thunk(tsugiki_state* s, const Stub&) {
    long number = host_syscall_number(s->x[0]);
    long r;
    if (number < 0) {
        log("syscall %llu has no x86_64 number here", (unsigned long long)s->x[0]);
        errno = ENOSYS;
        r = -1;
    } else {
        r = syscall(number, s->x[1], s->x[2], s->x[3], s->x[4], s->x[5], s->x[6]);
    }
    ret(s, s->x[30], uint64_t(r));
}

// ---- dl*, aware of translated modules ----

void dlsym_thunk(tsugiki_state* s, const Stub&) {
    std::string name = ptr<char>(s->x[1]);
    uint64_t back = s->x[30];
    void* host = dlsym(ptr<void>(s->x[0]), name.c_str());
    uint64_t result = 0;
    if (uint64_t guest = lookup_guest(name)) {
        result = guest;
    } else if (uint64_t special = special_import(name)) {
        result = special;
    } else if (host) {
        result = reinterpret_cast<uint64_t>(host);
        static std::mutex lock;
        static std::unordered_map<void*, uint64_t> wrapped;
        std::lock_guard guard(lock);
        auto it = wrapped.find(host);
        if (it != wrapped.end()) {
            result = it->second;
        } else {
            struct Search {
                uint64_t address;
                bool code;
            } search{result, false};
            dl_iterate_phdr(
                [](dl_phdr_info* info, size_t, void* data) {
                    auto* x = static_cast<Search*>(data);
                    for (int i = 0; i < info->dlpi_phnum; ++i) {
                        const auto& ph = info->dlpi_phdr[i];
                        if (ph.p_type == PT_LOAD && x->address - (info->dlpi_addr + ph.p_vaddr) < ph.p_memsz) {
                            x->code = (ph.p_flags & PF_X) != 0;
                            return 1;
                        }
                    }
                    return 0;
                },
                &search);
            if (search.code) {
                result = make_stub({generic_call, host, 0, strdup(name.c_str())});
            }
            wrapped.emplace(host, result);
        }
    }
    ret(s, back, result);
}

// guest libs first, so the guest's unwinder finds its own eh_frame, then the host's
void dl_iterate_phdr_thunk(tsugiki_state* s, const Stub&) {
    uint64_t back = s->x[30], callback = s->x[0], data = s->x[1];
    static std::atomic<unsigned long long> adds{0};
    int result = 0;
    for (const GuestModule& m : guest_modules()) {
        dl_phdr_info info{};
        info.dlpi_addr = m.base;
        info.dlpi_name = m.soname;
        info.dlpi_phdr = static_cast<const Elf64_Phdr*>(m.phdrs);
        info.dlpi_phnum = m.phnum;
        info.dlpi_adds = adds.load();
        Call call;
        call.ints[0] = reinterpret_cast<uint64_t>(&info);
        call.ints[1] = sizeof info;
        call.ints[2] = data;
        result = int(run_guest(callback, call));
        if (result != 0) {
            ret(s, back, uint64_t(int64_t(result)));
            return;
        }
    }
    struct Walk {
        uint64_t callback, data;
    } walk{callback, data};
    result = dl_iterate_phdr(
        [](dl_phdr_info* info, size_t size, void* p) {
            auto* w = static_cast<Walk*>(p);
            Call call;
            call.ints[0] = reinterpret_cast<uint64_t>(info);
            call.ints[1] = size;
            call.ints[2] = w->data;
            return int(run_guest(w->callback, call));
        },
        &walk);
    ret(s, back, uint64_t(int64_t(result)));
}

void dladdr_thunk(tsugiki_state* s, const Stub&) {
    auto* info = ptr<Dl_info>(s->x[1]);
    int found = guest_dladdr(s->x[0], info) ? 1 : dladdr(ptr<void>(s->x[0]), info);
    ret(s, s->x[30], uint64_t(found));
}

// thread_local destructors
void cxa_thread_atexit_thunk(tsugiki_state* s, const Stub&) {
    auto f = reinterpret_cast<void (*)(void*)>(host_callback(s->x[0], plain));
    ret(s, s->x[30], uint64_t(int64_t(__cxa_thread_atexit_impl(f, ptr<void>(s->x[1]), ptr<void>(s->x[2])))));
}

void pthread_atfork_thunk(tsugiki_state* s, const Stub&) {
    auto wrap = [](uint64_t f) { return f ? reinterpret_cast<void (*)()>(host_callback(f, plain)) : nullptr; };
    ret(s, s->x[30], uint64_t(int64_t(pthread_atfork(wrap(s->x[0]), wrap(s->x[1]), wrap(s->x[2])))));
}

// an executable's _start lands here: run main, then exit like bionic does
void libc_init_thunk(tsugiki_state* s, const Stub&) {
    auto* raw = ptr<uint64_t>(s->x[0]);
    uint64_t slingshot = s->x[2];
    int argc = int(raw[0]);
    Call call;
    call.ints[0] = uint64_t(argc);
    call.ints[1] = reinterpret_cast<uint64_t>(raw + 1);
    call.ints[2] = reinterpret_cast<uint64_t>(raw + 1 + argc + 1);
    exit(int(run_guest(slingshot, call)));
}

struct Special {
    const char* name;
    void (*call)(tsugiki_state*, const Stub&);
};

const Special specials[] = {
    {"vfprintf", vfprintf_thunk},
    {"vprintf", vprintf_thunk},
    {"vsprintf", vsprintf_thunk},
    {"__vsprintf_chk", vsprintf_chk_thunk},
    {"vsnprintf", vsnprintf_thunk},
    {"__vsnprintf_chk", vsnprintf_chk_thunk},
    {"vasprintf", vasprintf_thunk},
    {"vdprintf", vdprintf_thunk},
    {"__android_log_vprint", android_log_vprint_thunk},
    {"vsscanf", vsscanf_thunk},
    {"qsort", qsort_thunk},
    {"bsearch", bsearch_thunk},
    {"__cxa_atexit", cxa_atexit_thunk},
    {"atexit", atexit_thunk},
    {"__cxa_finalize", cxa_finalize_thunk},
    {"pthread_create", pthread_create_thunk},
    {"pthread_once", pthread_once_thunk},
    {"pthread_key_create", pthread_key_create_thunk},
    {"setjmp", setjmp_thunk},
    {"_setjmp", setjmp_thunk},
    {"sigsetjmp", setjmp_thunk},
    {"longjmp", longjmp_thunk},
    {"_longjmp", longjmp_thunk},
    {"siglongjmp", longjmp_thunk},
    {"getauxval", getauxval_thunk},
    {"syscall", syscall_thunk},
    {"dlsym", dlsym_thunk},
    {"__libc_init", libc_init_thunk},
    {"dl_iterate_phdr", dl_iterate_phdr_thunk},
    {"dladdr", dladdr_thunk},
    {"__cxa_thread_atexit_impl", cxa_thread_atexit_thunk},
    {"pthread_atfork", pthread_atfork_thunk},
};

}  // namespace

uint64_t special_import(const std::string& name) {
    static std::mutex lock;
    static std::unordered_map<std::string, uint64_t> made;
    for (const Special& s : specials) {
        if (name == s.name) {
            std::lock_guard guard(lock);
            auto it = made.find(name);
            if (it == made.end()) {
                it = made.emplace(name, make_stub({s.call, nullptr, 0, s.name})).first;
            }
            return it->second;
        }
    }
    return 0;
}

}  // namespace tsugiki::rt
