// what translated code includes
#pragma once

#include <stdint.h>
#include <string.h>

struct tsugiki_vreg {
    uint64_t lo, hi;
};

// one guest thread
struct tsugiki_state {
    // x[31] is sp
    uint64_t x[32];
    // where to go next after a return or a jump we couldn't follow
    uint64_t pc;
    uint64_t tpidr;
    // nzcv, one per word so writes stay cheap
    uint32_t nf, zf, cf, vf;
    // ldxr/stxr monitor
    uint64_t excl_addr;
    uint64_t excl_lo, excl_hi;
    uint32_t excl_size;
    uint32_t excl;
    uint32_t fpcr, fpsr;
    // this thread's dispatch cache, see tsugiki_dispatch
    struct tsugiki_cache* cache;
    alignas(16) tsugiki_vreg v[32];
};

typedef void (*tsugiki_code)(tsugiki_state*);

// guest pc -> translated code, direct mapped, one per thread so no locking
#define TSUGIKI_CACHE 4096
struct tsugiki_cache {
    uint64_t pc[TSUGIKI_CACHE];
    tsugiki_code code[TSUGIKI_CACHE];
};

static inline uint64_t tsugiki_cache_slot(uint64_t pc) {
    return (pc >> 2) & (TSUGIKI_CACHE - 1);
}

struct tsugiki_entry {
    uint64_t address;
    tsugiki_code code;
};

// one fp/simd word as its own lil function, the interpreter runs these
struct tsugiki_op {
    uint32_t word;
    tsugiki_code code;
};

// one translated library, handed to the runtime when it loads
struct tsugiki_module {
    const char* soname;
    const tsugiki_entry* entries;
    uint64_t count;
    // where the code finds the guest image
    uint64_t* base;
    // the arm64 .so, embedded
    const uint8_t* image;
    uint64_t image_size;
    // the runtime's, null until loaded
    void* loaded;
    // bss in the translated .so the image goes into, so cfi sees guest code inside a library
    uint8_t* space;
    uint64_t space_size;
    // its fp/simd words, sorted
    const tsugiki_op* ops;
    uint64_t op_count;
};

// the runtime builds with hidden visibility, only this api is exported
#pragma GCC visibility push(default)
extern "C" {
// maps the image, relocates it, runs its constructors
void tsugiki_load(tsugiki_module* m);
// a translated executable's main: loads it and runs its _start
int tsugiki_main(tsugiki_module* m, int argc, char** argv, char** envp);
// runs whatever is at s->pc, one function's worth
void tsugiki_call(tsugiki_state* s);
// same but for code w/o a translation, one instruction at a time
void tsugiki_interpret(tsugiki_state* s);
void tsugiki_svc(tsugiki_state* s, uint32_t imm);
// unimplemented or bad stuff, doesn't return
[[noreturn]] void tsugiki_trap(tsugiki_state* s, uint64_t pc, uint32_t raw, const char* why);
uint64_t tsugiki_mrs(tsugiki_state* s, uint32_t reg);
void tsugiki_msr(tsugiki_state* s, uint32_t reg, uint64_t value);
void tsugiki_sys(tsugiki_state* s, uint32_t op, uint64_t value);
uint32_t tsugiki_crc32(uint32_t crc, uint64_t value, int bytes, int castagnoli);
}
#pragma GCC visibility pop

// guest memory is our memory
static inline uint8_t ts_ld8(uint64_t a) { return *(const volatile uint8_t*)a; }
static inline uint16_t ts_ld16(uint64_t a) { uint16_t v; memcpy(&v, (const void*)a, 2); return v; }
static inline uint32_t ts_ld32(uint64_t a) { uint32_t v; memcpy(&v, (const void*)a, 4); return v; }
static inline uint64_t ts_ld64(uint64_t a) { uint64_t v; memcpy(&v, (const void*)a, 8); return v; }
static inline void ts_st8(uint64_t a, uint8_t v) { *(volatile uint8_t*)a = v; }
static inline void ts_st16(uint64_t a, uint16_t v) { memcpy((void*)a, &v, 2); }
static inline void ts_st32(uint64_t a, uint32_t v) { memcpy((void*)a, &v, 4); }
static inline void ts_st64(uint64_t a, uint64_t v) { memcpy((void*)a, &v, 8); }

// flags, kept in a local by translated code
struct tsugiki_flags {
    uint32_t nf, zf, cf, vf;
};

static inline tsugiki_flags ts_flags_in(const tsugiki_state* s) {
    return {s->nf, s->zf, s->cf, s->vf};
}

static inline void ts_flags_out(tsugiki_state* s, tsugiki_flags f) {
    s->nf = f.nf;
    s->zf = f.zf;
    s->cf = f.cf;
    s->vf = f.vf;
}

// fpcr writes: rounding and flush to zero go to mxcsr too, the host does the math
static inline void ts_set_fpcr(tsugiki_state* s, uint64_t v) {
    s->fpcr = (uint32_t)v;
#if defined(__x86_64__)
    unsigned rm = (unsigned)(v >> 22) & 3;
    // arm64 has up then down, x86 down then up
    unsigned x86 = rm == 1 ? 2 : rm == 2 ? 1 : rm;
    unsigned m = __builtin_ia32_stmxcsr();
    m = (m & ~0x6000u) | (x86 << 13);
    m = (v & (1u << 24)) ? (m | 0x8040u) : (m & ~0x8040u);
    __builtin_ia32_ldmxcsr(m);
#endif
}

static inline uint64_t ts_adds64(tsugiki_flags* __restrict s, uint64_t a, uint64_t b, uint64_t carry) {
    unsigned __int128 wide = (unsigned __int128)a + b + carry;
    uint64_t r = (uint64_t)wide;
    s->nf = r >> 63;
    s->zf = r == 0;
    s->cf = (uint32_t)(wide >> 64);
    s->vf = ((~(a ^ b) & (a ^ r)) >> 63) & 1;
    return r;
}

static inline uint32_t ts_adds32(tsugiki_flags* __restrict s, uint32_t a, uint32_t b, uint32_t carry) {
    uint64_t wide = (uint64_t)a + b + carry;
    uint32_t r = (uint32_t)wide;
    s->nf = r >> 31;
    s->zf = r == 0;
    s->cf = (uint32_t)(wide >> 32);
    s->vf = ((~(a ^ b) & (a ^ r)) >> 31) & 1;
    return r;
}

// a - b is a + ~b + 1
static inline uint64_t ts_subs64(tsugiki_flags* __restrict s, uint64_t a, uint64_t b) { return ts_adds64(s, a, ~b, 1); }
static inline uint32_t ts_subs32(tsugiki_flags* __restrict s, uint32_t a, uint32_t b) { return ts_adds32(s, a, ~b, 1); }

static inline void ts_nz64(tsugiki_flags* __restrict s, uint64_t r) {
    s->nf = r >> 63;
    s->zf = r == 0;
    s->cf = 0;
    s->vf = 0;
}

static inline void ts_nz32(tsugiki_flags* __restrict s, uint32_t r) {
    s->nf = r >> 31;
    s->zf = r == 0;
    s->cf = 0;
    s->vf = 0;
}

static inline void ts_nzcv(tsugiki_flags* __restrict s, uint32_t nzcv) {
    s->nf = (nzcv >> 3) & 1;
    s->zf = (nzcv >> 2) & 1;
    s->cf = (nzcv >> 1) & 1;
    s->vf = nzcv & 1;
}

static inline uint64_t ts_ror64(uint64_t x, unsigned n) { n &= 63; return n ? (x >> n) | (x << (64 - n)) : x; }
static inline uint32_t ts_ror32(uint32_t x, unsigned n) { n &= 31; return n ? (x >> n) | (x << (32 - n)) : x; }

static inline uint64_t ts_clz64(uint64_t x) { return x ? __builtin_clzll(x) : 64; }
static inline uint32_t ts_clz32(uint32_t x) { return x ? __builtin_clz(x) : 32; }
static inline uint64_t ts_ctz64(uint64_t x) { return x ? __builtin_ctzll(x) : 64; }
static inline uint32_t ts_ctz32(uint32_t x) { return x ? __builtin_ctz(x) : 32; }

// leading copies of the sign bit, minus that bit
static inline uint64_t ts_cls64(uint64_t x) { return ts_clz64(x ^ (uint64_t)((int64_t)x >> 1)) - 1; }
static inline uint32_t ts_cls32(uint32_t x) { return ts_clz32(x ^ (uint32_t)((int32_t)x >> 1)) - 1; }

static inline int64_t ts_sdiv64(int64_t a, int64_t b) {
    if (b == 0) return 0;
    if (a == INT64_MIN && b == -1) return INT64_MIN;
    return a / b;
}
static inline int32_t ts_sdiv32(int32_t a, int32_t b) {
    if (b == 0) return 0;
    if (a == INT32_MIN && b == -1) return INT32_MIN;
    return a / b;
}
static inline uint64_t ts_udiv64(uint64_t a, uint64_t b) { return b ? a / b : 0; }
static inline uint32_t ts_udiv32(uint32_t a, uint32_t b) { return b ? a / b : 0; }

// atomic min/max, cas loops
template <typename T, typename Compare>
static inline T ts_atomic_pick(T* p, T value, Compare better) {
    T old = __atomic_load_n(p, __ATOMIC_RELAXED);
    while (true) {
        T next = better(value, old) ? value : old;
        if (__atomic_compare_exchange_n(p, &old, next, false, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) {
            return old;
        }
    }
}

template <typename T>
static inline T ts_atomic_smax(T* p, T v) {
    return ts_atomic_pick(p, v, [](T a, T b) {
        if (sizeof(T) == 1) return (int8_t)a > (int8_t)b;
        if (sizeof(T) == 2) return (int16_t)a > (int16_t)b;
        if (sizeof(T) == 4) return (int32_t)a > (int32_t)b;
        return (int64_t)a > (int64_t)b;
    });
}

template <typename T>
static inline T ts_atomic_smin(T* p, T v) {
    return ts_atomic_pick(p, v, [](T a, T b) {
        if (sizeof(T) == 1) return (int8_t)a < (int8_t)b;
        if (sizeof(T) == 2) return (int16_t)a < (int16_t)b;
        if (sizeof(T) == 4) return (int32_t)a < (int32_t)b;
        return (int64_t)a < (int64_t)b;
    });
}

template <typename T>
static inline T ts_atomic_umax(T* p, T v) {
    return ts_atomic_pick(p, v, [](T a, T b) { return a > b; });
}

template <typename T>
static inline T ts_atomic_umin(T* p, T v) {
    return ts_atomic_pick(p, v, [](T a, T b) { return a < b; });
}

#include "tsugiki_simd.h"

// jump to s->pc: the thread's cache first, the runtime's lookup when it misses
static inline void tsugiki_dispatch(tsugiki_state* s) {
    struct tsugiki_cache* c = s->cache;
    uint64_t i = tsugiki_cache_slot(s->pc);
    if (__builtin_expect(c->pc[i] == s->pc, 1)) {
        c->code[i](s);
    } else {
        tsugiki_call(s);
    }
}
