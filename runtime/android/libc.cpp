// libc imports the generic call can't do: va_lists, callbacks, setjmp, dl*, auxv, syscall
#include <android/log.h>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <fenv.h>
#include <link.h>
#include <pthread.h>
#include <stdarg.h>
#include <signal.h>
#include <sys/auxv.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "android/runtime.hpp"

extern "C" int __cxa_thread_atexit_impl(void (*)(void*), void*, void*);
// ours, every shared lib has one
extern "C" void* __dso_handle;

extern "C" int __cxa_atexit(void (*)(void*), void*, void*);

extern "C" [[noreturn]] void __stack_chk_fail();

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

// the kernel's own return, -errno and all
long raw_syscall(long number, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    long result;
    register uint64_t r10 asm("r10") = d;
    register uint64_t r8 asm("r8") = e;
    register uint64_t r9 asm("r9") = f;
    asm volatile("syscall" : "=a"(result) : "a"(number), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
    return result;
}

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
    case 43: return SYS_statfs;
    case 44: return SYS_fstatfs;
    case 46: return SYS_ftruncate;
    case 47: return SYS_fallocate;
    case 49: return SYS_chdir;
    case 50: return SYS_fchdir;
    case 52: return SYS_fchmod;
    case 53: return SYS_fchmodat;
    case 54: return SYS_fchownat;
    case 55: return SYS_fchown;
    case 59: return SYS_pipe2;
    case 71: return SYS_sendfile;
    case 72: return SYS_pselect6;
    case 73: return SYS_ppoll;
    case 79: return SYS_newfstatat;
    case 80: return SYS_fstat;
    case 81: return SYS_sync;
    case 82: return SYS_fsync;
    case 83: return SYS_fdatasync;
    case 88: return SYS_utimensat;
    case 99: return SYS_set_robust_list;
    case 100: return SYS_get_robust_list;
    case 132: return SYS_sigaltstack;
    case 135: return SYS_rt_sigprocmask;
    case 153: return SYS_times;
    case 160: return SYS_uname;
    case 163: return SYS_getrlimit;
    case 164: return SYS_setrlimit;
    case 165: return SYS_getrusage;
    case 166: return SYS_umask;
    case 168: return SYS_getcpu;
    case 198: return SYS_socket;
    case 199: return SYS_socketpair;
    case 200: return SYS_bind;
    case 201: return SYS_listen;
    case 202: return SYS_accept;
    case 203: return SYS_connect;
    case 204: return SYS_getsockname;
    case 205: return SYS_getpeername;
    case 206: return SYS_sendto;
    case 207: return SYS_recvfrom;
    case 208: return SYS_setsockopt;
    case 209: return SYS_getsockopt;
    case 210: return SYS_shutdown;
    case 211: return SYS_sendmsg;
    case 212: return SYS_recvmsg;
    case 242: return SYS_accept4;
    case 260: return SYS_wait4;
    case 261: return SYS_prlimit64;
    case 276: return SYS_renameat2;
    case 291: return SYS_statx;
    case 293: return SYS_rseq;
    default: return -1;
    }
}

namespace {

void syscall_thunk(tsugiki_state* s, const Stub&) {
    long r = guest_syscall(s->x[0], s->x[1], s->x[2], s->x[3], s->x[4], s->x[5], s->x[6]);
    if (r < 0 && r > -4096) {
        errno = int(-r);
        r = -1;
    }
    ret(s, s->x[30], uint64_t(r));
}

// guest jits: rwx anon memory gets EACCES so pcre2 and friends go back to their interpreters,
// the ones flipping rw pages to rx (luajit, which can't live w/o it) get their code interpreted.
// mappings never really get exec, arm64's PROT_BTI and PROT_MTE mean nothing here
constexpr uint64_t guest_prot_extra = 0x30;

void mmap_thunk(tsugiki_state* s, const Stub&) {
    uint64_t prot = s->x[2] & ~guest_prot_extra;
    int fd = int(s->x[4]);
    if ((prot & PROT_EXEC) && (s->x[3] & MAP_ANONYMOUS)) {
        errno = EACCES;
        ret(s, s->x[30], uint64_t(MAP_FAILED));
        return;
    }
    void* r = mmap(ptr<void>(s->x[0]), size_t(s->x[1]), int(prot & ~uint64_t(PROT_EXEC)), int(s->x[3]), fd, off_t(s->x[5]));
    if (r != MAP_FAILED && (prot & PROT_EXEC)) {
        add_guest_code(reinterpret_cast<uint64_t>(r), s->x[1]);
    }
    ret(s, s->x[30], reinterpret_cast<uint64_t>(r));
}

void mprotect_thunk(tsugiki_state* s, const Stub&) {
    uint64_t prot = s->x[2] & ~guest_prot_extra;
    int r = mprotect(ptr<void>(s->x[0]), size_t(s->x[1]), int(prot & ~uint64_t(PROT_EXEC)));
    if (r == 0 && (prot & PROT_EXEC)) {
        add_guest_code(s->x[0], s->x[1]);
    }
    ret(s, s->x[30], uint64_t(int64_t(r)));
}

// the guest reading one of our .so files gets the arm64 original we carry inside it,
// qt checks plugins' elf headers and metadata that way; -1 if it isn't one of ours
int open_original(const char* path, uint64_t flags) {
    size_t n = std::strlen(path);
    if (n < 4 || std::strcmp(path + n - 3, ".so") != 0 || (flags & O_ACCMODE) != O_RDONLY) {
        return -1;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    auto read_at = [&](void* to, size_t size, uint64_t at) { return pread(fd, to, size, off_t(at)) == ssize_t(size); };
    int result = -1;
    Elf64_Ehdr eh;
    if (read_at(&eh, sizeof eh, 0) && std::memcmp(eh.e_ident, ELFMAG, SELFMAG) == 0 && eh.e_machine == EM_X86_64 &&
        eh.e_shentsize == sizeof(Elf64_Shdr) && eh.e_shstrndx < eh.e_shnum) {
        std::vector<Elf64_Shdr> sections(eh.e_shnum);
        if (read_at(sections.data(), sections.size() * sizeof(Elf64_Shdr), eh.e_shoff)) {
            const Elf64_Shdr& names = sections[eh.e_shstrndx];
            std::vector<char> text(names.sh_size + 1, 0);
            if (read_at(text.data(), names.sh_size, names.sh_offset)) {
                for (const Elf64_Shdr& sh : sections) {
                    if (sh.sh_name < names.sh_size && std::strcmp(text.data() + sh.sh_name, ".tsugiki_image") == 0) {
                        std::vector<uint8_t> image(sh.sh_size);
                        int memfd = read_at(image.data(), image.size(), sh.sh_offset)
                                        ? int(syscall(SYS_memfd_create, path + n - std::min<size_t>(n, 40), (flags & O_CLOEXEC) ? 1u : 0u))
                                        : -1;
                        if (memfd >= 0 && write(memfd, image.data(), image.size()) == ssize_t(image.size()) && lseek(memfd, 0, SEEK_SET) == 0) {
                            result = memfd;
                        } else if (memfd >= 0) {
                            close(memfd);
                        }
                        break;
                    }
                }
            }
        }
    }
    close(fd);
    return result;
}

// a thread running guest code has its stack where the guest's sp is, not on the host stack;
// qml's v4, rust and gcs measure recursion against these bounds
void pthread_getattr_np_thunk(tsugiki_state* s, const Stub&) {
    auto thread = pthread_t(s->x[0]);
    auto* attr = ptr<pthread_attr_t>(s->x[1]);
    int r = pthread_getattr_np(thread, attr);
    void* base = nullptr;
    size_t size = 0;
    if (r == 0 && guest_stack_of(thread, &base, &size)) {
        pthread_attr_setstack(attr, base, size);
    }
    ret(s, s->x[30], uint64_t(int64_t(r)));
}

// open and friends, with arm64's flags
void open_thunk(tsugiki_state* s, const Stub&) {
    int r = open_original(ptr<char>(s->x[0]), s->x[1]);
    if (r < 0) {
        r = open(ptr<char>(s->x[0]), int(host_open_flags(s->x[1])), mode_t(s->x[2]));
    }
    ret(s, s->x[30], uint64_t(int64_t(r)));
}

void openat_thunk(tsugiki_state* s, const Stub&) {
    const char* path = ptr<char>(s->x[1]);
    int r = path[0] == '/' || int(s->x[0]) == AT_FDCWD ? open_original(path, s->x[2]) : -1;
    if (r < 0) {
        r = openat(int(s->x[0]), path, int(host_open_flags(s->x[2])), mode_t(s->x[3]));
    }
    ret(s, s->x[30], uint64_t(int64_t(r)));
}

void fcntl_thunk(tsugiki_state* s, const Stub&) {
    int cmd = int(s->x[1]);
    uint64_t arg = cmd == F_SETFL ? host_open_flags(s->x[2]) : s->x[2];
    int r = fcntl(int(s->x[0]), cmd, arg);
    if (cmd == F_GETFL && r >= 0) {
        r = int(guest_open_flags(uint64_t(r)));
    }
    ret(s, s->x[30], uint64_t(int64_t(r)));
}

void pipe2_thunk(tsugiki_state* s, const Stub&) {
    ret(s, s->x[30], uint64_t(int64_t(pipe2(ptr<int>(s->x[0]), int(host_open_flags(s->x[1]))))));
}

// ---- dl*, aware of translated modules ----

// an export wrapper of a translated lib, as glue_asm writes it:
// movabs $vaddr, %r10; lea module(%rip), %r11; jmp entry. 0 if it isn't one
uint64_t guest_of_wrapper(const void* host) {
    const auto* b = static_cast<const uint8_t*>(host);
    if (!host_code(const_cast<void*>(host)) || b[0] != 0x49 || b[1] != 0xba || b[10] != 0x4c || b[11] != 0x8d || b[12] != 0x1d) {
        return 0;
    }
    uint64_t vaddr;
    int32_t rel;
    std::memcpy(&vaddr, b + 2, 8);
    std::memcpy(&rel, b + 13, 4);
    auto* module = reinterpret_cast<const tsugiki_module*>(b + 17 + rel);
    return module->loaded ? *module->base + vaddr : 0;
}

void dlsym_thunk(tsugiki_state* s, const Stub&) {
    std::string name = ptr<char>(s->x[1]);
    uint64_t back = s->x[30];
    // the host's lookup honours the handle; a translated lib answers with its wrapper
    void* host = dlsym(ptr<void>(s->x[0]), name.c_str());
    uint64_t result = 0;
    if (uint64_t guest = host ? guest_of_wrapper(host) : 0) {
        result = guest;
    } else if (uint64_t guest = host ? 0 : lookup_guest(name)) {
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

// thread_local destructors; the linker wants a dso it knows, the guest's handle is inside its image
void cxa_thread_atexit_thunk(tsugiki_state* s, const Stub&) {
    auto f = reinterpret_cast<void (*)(void*)>(host_callback(s->x[0], plain));
    ret(s, s->x[30], uint64_t(int64_t(__cxa_thread_atexit_impl(f, ptr<void>(s->x[1]), &__dso_handle))));
}

void pthread_atfork_thunk(tsugiki_state* s, const Stub&) {
    auto wrap = [](uint64_t f) { return f ? reinterpret_cast<void (*)()>(host_callback(f, plain)) : nullptr; };
    ret(s, s->x[30], uint64_t(int64_t(pthread_atfork(wrap(s->x[0]), wrap(s->x[1]), wrap(s->x[2])))));
}

// ---- structs laid out differently on arm64 ----

// arm64's struct stat, the asm-generic one; x86_64's is 144 bytes in another order
struct GuestStat {
    uint64_t dev;
    uint64_t ino;
    uint32_t mode;
    uint32_t nlink;
    uint32_t uid;
    uint32_t gid;
    uint64_t rdev;
    uint64_t pad1;
    int64_t size;
    int32_t blksize;
    int32_t pad2;
    int64_t blocks;
    timespec atim, mtim, ctim;
    uint32_t unused4, unused5;
};
static_assert(sizeof(GuestStat) == 128);

void stat_out(int r, const struct stat& h, uint64_t out) {
    if (r != 0 || out == 0) {
        return;
    }
    GuestStat g{};
    g.dev = h.st_dev;
    g.ino = h.st_ino;
    g.mode = h.st_mode;
    g.nlink = uint32_t(h.st_nlink);
    g.uid = h.st_uid;
    g.gid = h.st_gid;
    g.rdev = h.st_rdev;
    g.size = h.st_size;
    g.blksize = int32_t(h.st_blksize);
    g.blocks = h.st_blocks;
    g.atim = h.st_atim;
    g.mtim = h.st_mtim;
    g.ctim = h.st_ctim;
    std::memcpy(ptr<void>(out), &g, sizeof g);
}

void stat_thunk(tsugiki_state* s, const Stub&) {
    struct stat h;
    int r = stat(ptr<const char>(s->x[0]), &h);
    stat_out(r, h, s->x[1]);
    ret(s, s->x[30], uint64_t(int64_t(r)));
}

void lstat_thunk(tsugiki_state* s, const Stub&) {
    struct stat h;
    int r = lstat(ptr<const char>(s->x[0]), &h);
    stat_out(r, h, s->x[1]);
    ret(s, s->x[30], uint64_t(int64_t(r)));
}

void fstat_thunk(tsugiki_state* s, const Stub&) {
    struct stat h;
    int r = fstat(int(s->x[0]), &h);
    stat_out(r, h, s->x[1]);
    ret(s, s->x[30], uint64_t(int64_t(r)));
}

void fstatat_thunk(tsugiki_state* s, const Stub&) {
    struct stat h;
    int r = fstatat(int(s->x[0]), ptr<const char>(s->x[1]), &h, int(s->x[3]));
    stat_out(r, h, s->x[2]);
    ret(s, s->x[30], uint64_t(int64_t(r)));
}

// arm64 aligns the data word, x86_64 packs it right after events
struct GuestEpollEvent {
    uint32_t events;
    uint32_t pad;
    uint64_t data;
};
static_assert(sizeof(GuestEpollEvent) == 16);

void epoll_ctl_thunk(tsugiki_state* s, const Stub&) {
    epoll_event h{};
    if (s->x[3]) {
        auto* g = ptr<GuestEpollEvent>(s->x[3]);
        h.events = g->events;
        h.data.u64 = g->data;
    }
    int r = epoll_ctl(int(s->x[0]), int(s->x[1]), int(s->x[2]), s->x[3] ? &h : nullptr);
    ret(s, s->x[30], uint64_t(int64_t(r)));
}

void epoll_out(int r, const std::vector<epoll_event>& h, uint64_t out) {
    auto* g = ptr<GuestEpollEvent>(out);
    for (int i = 0; i < r; ++i) {
        g[i].events = h[size_t(i)].events;
        g[i].pad = 0;
        g[i].data = h[size_t(i)].data.u64;
    }
}

void epoll_wait_thunk(tsugiki_state* s, const Stub&) {
    std::vector<epoll_event> h(size_t(std::max(1, int(s->x[2]))));
    int r = epoll_wait(int(s->x[0]), h.data(), int(s->x[2]), int(s->x[3]));
    epoll_out(r, h, s->x[1]);
    ret(s, s->x[30], uint64_t(int64_t(r)));
}

void epoll_pwait_thunk(tsugiki_state* s, const Stub&) {
    std::vector<epoll_event> h(size_t(std::max(1, int(s->x[2]))));
    int r = epoll_pwait(int(s->x[0]), h.data(), int(s->x[2]), int(s->x[3]), ptr<const sigset_t>(s->x[4]));
    epoll_out(r, h, s->x[1]);
    ret(s, s->x[30], uint64_t(int64_t(r)));
}

// ---- signals: guest handlers behind callback slots ----

std::mutex signal_lock;
// sigsegv stays ours, the guest's handler gets what we don't want
struct sigaction guest_segv_action {};

uint64_t wrap_handler(uint64_t handler) {
    if (handler == uint64_t(SIG_DFL) || handler == uint64_t(SIG_IGN) || handler == uint64_t(SIG_ERR)) {
        return handler;
    }
    return host_callback(handler, plain);
}

uint64_t unwrap_handler(uint64_t handler) {
    uint64_t guest = callback_target(handler);
    return guest ? guest : handler;
}

void sigaction_thunk(tsugiki_state* s, const Stub&) {
    int sig = int(s->x[0]);
    // the same struct on both 64-bit sides of bionic
    auto* in = ptr<const struct sigaction>(s->x[1]);
    auto* out = ptr<struct sigaction>(s->x[2]);
    struct sigaction act {}, old {};
    if (in) {
        act = *in;
        act.sa_handler = reinterpret_cast<sighandler_t>(wrap_handler(reinterpret_cast<uint64_t>(in->sa_handler)));
    }
    int r = 0;
    if (sig == SIGSEGV) {
        std::lock_guard guard(signal_lock);
        old = guest_segv_action;
        if (in) {
            guest_segv_action = act;
        }
    } else {
        r = sigaction(sig, in ? &act : nullptr, out ? &old : nullptr);
    }
    if (out && r == 0) {
        old.sa_handler = reinterpret_cast<sighandler_t>(unwrap_handler(reinterpret_cast<uint64_t>(old.sa_handler)));
        *out = old;
    }
    ret(s, s->x[30], uint64_t(int64_t(r)));
}

void signal_thunk(tsugiki_state* s, const Stub&) {
    int sig = int(s->x[0]);
    uint64_t handler = wrap_handler(s->x[1]);
    uint64_t previous;
    if (sig == SIGSEGV) {
        std::lock_guard guard(signal_lock);
        previous = reinterpret_cast<uint64_t>(guest_segv_action.sa_handler);
        guest_segv_action.sa_handler = reinterpret_cast<sighandler_t>(handler);
        guest_segv_action.sa_flags = SA_RESTART;
    } else {
        previous = reinterpret_cast<uint64_t>(signal(sig, reinterpret_cast<sighandler_t>(handler)));
    }
    ret(s, s->x[30], unwrap_handler(previous));
}

}  // namespace

// a sigsegv that isn't ours, for the guest's own handler if it set one
bool guest_segv(int sig, siginfo_t* info, void* context) {
    struct sigaction action;
    {
        std::lock_guard guard(signal_lock);
        action = guest_segv_action;
    }
    uint64_t handler = reinterpret_cast<uint64_t>(action.sa_handler);
    if (handler == uint64_t(SIG_DFL) || handler == uint64_t(SIG_IGN) || handler == 0) {
        return false;
    }
    if (action.sa_flags & SA_SIGINFO) {
        action.sa_sigaction(sig, info, context);
    } else {
        action.sa_handler(sig);
    }
    return true;
}

namespace {

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

// fenv: arm64's fenv_t is fpcr and fpsr, 8 bytes, x86's is 32, and the constants differ;
// works on the guest's fpcr/fpsr like bionic's arm64 libm, with mxcsr flags folded in
struct GuestFenv {
    uint32_t control, status;
};
constexpr uint32_t fe_all = 0x9f;

uint32_t mxcsr_as_fpsr() {
    unsigned m = __builtin_ia32_stmxcsr();
    uint32_t f = 0;
    if (m & 0x01) f |= 0x01;  // invalid
    if (m & 0x04) f |= 0x02;  // div by zero
    if (m & 0x08) f |= 0x04;  // overflow
    if (m & 0x10) f |= 0x08;  // underflow
    if (m & 0x20) f |= 0x10;  // inexact
    if (m & 0x02) f |= 0x80;  // denormal
    return f;
}

uint32_t get_fpsr(tsugiki_state* s) {
    return s->fpsr | mxcsr_as_fpsr();
}

void set_fpsr(tsugiki_state* s, uint32_t f) {
    s->fpsr = f;
    unsigned m = __builtin_ia32_stmxcsr() & ~0x3fu;
    if (f & 0x01) m |= 0x01;
    if (f & 0x02) m |= 0x04;
    if (f & 0x04) m |= 0x08;
    if (f & 0x08) m |= 0x10;
    if (f & 0x10) m |= 0x20;
    if (f & 0x80) m |= 0x02;
    __builtin_ia32_ldmxcsr(m);
}

GuestFenv guest_fenv(uint64_t at) {
    // FE_DFL_ENV is the import, the host's own
    if (at == 0 || at == reinterpret_cast<uint64_t>(FE_DFL_ENV)) {
        return {0, 0};
    }
    return *ptr<GuestFenv>(at);
}

void fegetenv_thunk(tsugiki_state* s, const Stub&) {
    *ptr<GuestFenv>(s->x[0]) = {s->fpcr, get_fpsr(s)};
    ret(s, s->x[30], 0);
}

void fesetenv_thunk(tsugiki_state* s, const Stub&) {
    GuestFenv e = guest_fenv(s->x[0]);
    ts_set_fpcr(s, e.control);
    set_fpsr(s, e.status);
    ret(s, s->x[30], 0);
}

void feholdexcept_thunk(tsugiki_state* s, const Stub&) {
    *ptr<GuestFenv>(s->x[0]) = {s->fpcr, get_fpsr(s)};
    set_fpsr(s, get_fpsr(s) & ~fe_all);
    ret(s, s->x[30], 0);
}

void feupdateenv_thunk(tsugiki_state* s, const Stub&) {
    uint32_t raised = get_fpsr(s) & fe_all;
    GuestFenv e = guest_fenv(s->x[0]);
    ts_set_fpcr(s, e.control);
    set_fpsr(s, e.status | raised);
    ret(s, s->x[30], 0);
}

void fegetround_thunk(tsugiki_state* s, const Stub&) {
    ret(s, s->x[30], (s->fpcr >> 22) & 3);
}

void fesetround_thunk(tsugiki_state* s, const Stub&) {
    int64_t mode = int32_t(s->x[0]);
    if (mode < 0 || mode > 3) {
        ret(s, s->x[30], uint64_t(-1));
        return;
    }
    ts_set_fpcr(s, (s->fpcr & ~(3u << 22)) | uint32_t(mode << 22));
    ret(s, s->x[30], 0);
}

void feclearexcept_thunk(tsugiki_state* s, const Stub&) {
    set_fpsr(s, get_fpsr(s) & ~(uint32_t(s->x[0]) & fe_all));
    ret(s, s->x[30], 0);
}

void feraiseexcept_thunk(tsugiki_state* s, const Stub&) {
    set_fpsr(s, get_fpsr(s) | (uint32_t(s->x[0]) & fe_all));
    ret(s, s->x[30], 0);
}

void fetestexcept_thunk(tsugiki_state* s, const Stub&) {
    ret(s, s->x[30], get_fpsr(s) & uint32_t(s->x[0]) & fe_all);
}

// fexcept_t is 4 bytes on arm64, 2 on x86
void fegetexceptflag_thunk(tsugiki_state* s, const Stub&) {
    *ptr<uint32_t>(s->x[0]) = get_fpsr(s) & uint32_t(s->x[1]) & fe_all;
    ret(s, s->x[30], 0);
}

void fesetexceptflag_thunk(tsugiki_state* s, const Stub&) {
    uint32_t which = uint32_t(s->x[1]) & fe_all;
    set_fpsr(s, (get_fpsr(s) & ~which) | (*ptr<uint32_t>(s->x[0]) & which));
    ret(s, s->x[30], 0);
}

// arm64 bionic can't trap on these
void feenableexcept_thunk(tsugiki_state* s, const Stub&) {
    ret(s, s->x[30], uint64_t(-1));
}

void fe_zero_thunk(tsugiki_state* s, const Stub&) {
    ret(s, s->x[30], 0);
}

// a guest canary died, say where before bionic aborts
void stack_chk_fail_thunk(tsugiki_state* s, const Stub&) {
    uint64_t at = s->x[30];
    Dl_info info{};
    if (guest_dladdr(at, &info) && info.dli_fname) {
        __android_log_print(ANDROID_LOG_FATAL, "tsugiki", "guest stack corruption, back to %s+0x%llx (%s)", info.dli_fname,
                            (unsigned long long)(at - reinterpret_cast<uint64_t>(info.dli_fbase)), info.dli_sname ? info.dli_sname : "?");
    } else {
        __android_log_print(ANDROID_LOG_FATAL, "tsugiki", "guest stack corruption, back to 0x%llx", (unsigned long long)at);
    }
    dump_recent_calls();
    log_guest_backtrace(s);
    __stack_chk_fail();
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
    {"stat", stat_thunk},
    {"stat64", stat_thunk},
    {"lstat", lstat_thunk},
    {"lstat64", lstat_thunk},
    {"fstat", fstat_thunk},
    {"fstat64", fstat_thunk},
    {"fstatat", fstatat_thunk},
    {"fstatat64", fstatat_thunk},
    {"epoll_ctl", epoll_ctl_thunk},
    {"epoll_wait", epoll_wait_thunk},
    {"epoll_pwait", epoll_pwait_thunk},
    {"sigaction", sigaction_thunk},
    {"sigaction64", sigaction_thunk},
    {"signal", signal_thunk},
    {"__stack_chk_fail", stack_chk_fail_thunk},
    {"open", open_thunk},
    {"open64", open_thunk},
    {"__open_2", open_thunk},
    {"openat", openat_thunk},
    {"openat64", openat_thunk},
    {"__openat_2", openat_thunk},
    {"fcntl", fcntl_thunk},
    {"fcntl64", fcntl_thunk},
    {"pipe2", pipe2_thunk},
    {"pthread_getattr_np", pthread_getattr_np_thunk},
    {"mmap", mmap_thunk},
    {"mmap64", mmap_thunk},
    {"mprotect", mprotect_thunk},
    {"fegetenv", fegetenv_thunk},
    {"fesetenv", fesetenv_thunk},
    {"feholdexcept", feholdexcept_thunk},
    {"feupdateenv", feupdateenv_thunk},
    {"fegetround", fegetround_thunk},
    {"fesetround", fesetround_thunk},
    {"feclearexcept", feclearexcept_thunk},
    {"feraiseexcept", feraiseexcept_thunk},
    {"fetestexcept", fetestexcept_thunk},
    {"fegetexceptflag", fegetexceptflag_thunk},
    {"fesetexceptflag", fesetexceptflag_thunk},
    {"feenableexcept", feenableexcept_thunk},
    {"fedisableexcept", fe_zero_thunk},
    {"fegetexcept", fe_zero_thunk},
    {"bsd_signal", signal_thunk},
};

}  // namespace

// arm64 has O_DIRECTORY and O_NOFOLLOW where x86 has O_DIRECT and O_LARGEFILE, and back
uint64_t host_open_flags(uint64_t g) {
    uint64_t h = g & ~uint64_t(0x3c000);
    if (g & 0x4000) h |= 0x10000;
    if (g & 0x8000) h |= 0x20000;
    if (g & 0x10000) h |= 0x4000;
    if (g & 0x20000) h |= 0x8000;
    return h;
}

uint64_t guest_open_flags(uint64_t h) {
    uint64_t g = h & ~uint64_t(0x3c000);
    if (h & 0x10000) g |= 0x4000;
    if (h & 0x20000) g |= 0x8000;
    if (h & 0x4000) g |= 0x10000;
    if (h & 0x8000) g |= 0x20000;
    return g;
}

long guest_syscall(uint64_t arm, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    long number = host_syscall_number(arm);
    if (number < 0) {
        log("syscall %llu has no x86_64 number here", (unsigned long long)arm);
        return -ENOSYS;
    }
    switch (arm) {
    case 25:  // fcntl
        if (b == F_SETFL) c = host_open_flags(c);
        break;
    case 56:  // openat
        c = host_open_flags(c);
        break;
    case 59:  // pipe2
        b = host_open_flags(b);
        break;
    case 222:  // mmap and mprotect, jits get what mmap_thunk gives them
    case 226: {
        c &= ~uint64_t(0x30);
        if ((c & PROT_EXEC) && arm == 222 && (d & MAP_ANONYMOUS)) {
            return -EACCES;
        }
        bool exec = c & PROT_EXEC;
        c &= ~uint64_t(PROT_EXEC);
        long r = raw_syscall(number, a, b, c, d, e, f);
        if (exec && r >= 0) {
            add_guest_code(arm == 222 ? uint64_t(r) : a, b);
        }
        return r;
    }
    case 79:  // newfstatat and fstat, x86's struct stat is bigger
    case 80: {
        struct stat h;
        long r = arm == 79 ? raw_syscall(number, a, b, reinterpret_cast<uint64_t>(&h), d, 0, 0)
                           : raw_syscall(number, a, reinterpret_cast<uint64_t>(&h), 0, 0, 0, 0);
        stat_out(r == 0 ? 0 : -1, h, arm == 79 ? c : b);
        return r;
    }
    default:
        break;
    }
    long r = raw_syscall(number, a, b, c, d, e, f);
    if (arm == 25 && b == F_GETFL && r >= 0) {
        r = long(guest_open_flags(uint64_t(r)));
    }
    return r;
}

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
