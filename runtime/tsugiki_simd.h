// fp and neon helpers for translated code, clang vector types
#pragma once

#include <stdint.h>
#include <string.h>

typedef int8_t ts_i8 __attribute__((vector_size(16)));
typedef uint8_t ts_u8 __attribute__((vector_size(16)));
typedef int16_t ts_i16 __attribute__((vector_size(16)));
typedef uint16_t ts_u16 __attribute__((vector_size(16)));
typedef int32_t ts_i32 __attribute__((vector_size(16)));
typedef uint32_t ts_u32 __attribute__((vector_size(16)));
typedef int64_t ts_i64 __attribute__((vector_size(16)));
typedef uint64_t ts_u64 __attribute__((vector_size(16)));
typedef _Float16 ts_f16 __attribute__((vector_size(16)));
typedef float ts_f32 __attribute__((vector_size(16)));
typedef double ts_f64 __attribute__((vector_size(16)));

// 64-bit halves, for widening and narrowing
typedef int8_t ts_i8h __attribute__((vector_size(8)));
typedef uint8_t ts_u8h __attribute__((vector_size(8)));
typedef int16_t ts_i16h __attribute__((vector_size(8)));
typedef uint16_t ts_u16h __attribute__((vector_size(8)));
typedef int32_t ts_i32h __attribute__((vector_size(8)));
typedef uint32_t ts_u32h __attribute__((vector_size(8)));
typedef _Float16 ts_f16h __attribute__((vector_size(8)));
typedef float ts_f32h __attribute__((vector_size(8)));
typedef uint64_t ts_u64h __attribute__((vector_size(8)));
typedef int64_t ts_i64h __attribute__((vector_size(8)));

static inline ts_u64 ts_v(const tsugiki_state* s, int n) {
    ts_u64 v;
    memcpy(&v, &s->v[n], 16);
    return v;
}

// whole register
static inline void ts_w(tsugiki_state* s, int d, ts_u64 v) { memcpy(&s->v[d], &v, 16); }
// low half, high half zeroed
static inline void ts_w64(tsugiki_state* s, int d, ts_u64 v) {
    v[1] = 0;
    memcpy(&s->v[d], &v, 16);
}

static inline ts_u64h ts_low(ts_u64 v) { return __builtin_shufflevector(v, v, 0); }
static inline ts_u64h ts_high(ts_u64 v) { return __builtin_shufflevector(v, v, 1); }
static inline ts_u64 ts_join(ts_u64h low, ts_u64h high) { return __builtin_shufflevector(low, high, 0, 1); }

// scalar fp in v registers, writes zero the rest
static inline float ts_fs(const tsugiki_state* s, int n) { float f; memcpy(&f, &s->v[n], 4); return f; }
static inline double ts_fd(const tsugiki_state* s, int n) { double f; memcpy(&f, &s->v[n], 8); return f; }
static inline _Float16 ts_fh(const tsugiki_state* s, int n) { _Float16 f; memcpy(&f, &s->v[n], 2); return f; }
static inline void ts_ws(tsugiki_state* s, int d, float f) { uint32_t b; memcpy(&b, &f, 4); s->v[d].lo = b; s->v[d].hi = 0; }
static inline void ts_wd(tsugiki_state* s, int d, double f) { uint64_t b; memcpy(&b, &f, 8); s->v[d].lo = b; s->v[d].hi = 0; }
static inline void ts_wh(tsugiki_state* s, int d, _Float16 f) { uint16_t b; memcpy(&b, &f, 2); s->v[d].lo = b; s->v[d].hi = 0; }
static inline void ts_wbits(tsugiki_state* s, int d, uint64_t bits) { s->v[d].lo = bits; s->v[d].hi = 0; }

static inline uint32_t ts_bits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
static inline uint64_t ts_bits(double f) { uint64_t b; memcpy(&b, &f, 8); return b; }
static inline uint16_t ts_bits(_Float16 f) { uint16_t b; memcpy(&b, &f, 2); return b; }
static inline float ts_float(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
static inline double ts_double(uint64_t b) { double f; memcpy(&f, &b, 8); return f; }
static inline _Float16 ts_half(uint16_t b) { _Float16 f; memcpy(&f, &b, 2); return f; }

// fcmp: unordered 0011, equal 0110, less 1000, greater 0010
template <typename F>
static inline void ts_fcmp(tsugiki_state* __restrict s, F a, F b) {
    if (a != a || b != b) {
        s->nf = 0, s->zf = 0, s->cf = 1, s->vf = 1;
    } else if (a == b) {
        s->nf = 0, s->zf = 1, s->cf = 1, s->vf = 0;
    } else if (a < b) {
        s->nf = 1, s->zf = 0, s->cf = 0, s->vf = 0;
    } else {
        s->nf = 0, s->zf = 0, s->cf = 1, s->vf = 0;
    }
}

// fp to int, rounded one of five ways, saturating, nan is 0
enum { ts_nearest, ts_plus, ts_minus, ts_zero, ts_away };

template <int Mode, typename F>
static inline F ts_round(F f) {
    switch (Mode) {
    case ts_nearest: return __builtin_elementwise_roundeven(f);
    case ts_plus: return __builtin_elementwise_ceil(f);
    case ts_minus: return __builtin_elementwise_floor(f);
    case ts_zero: return __builtin_elementwise_trunc(f);
    default: return __builtin_elementwise_round(f);
    }
}

template <typename I, int Mode, typename F>
static inline I ts_to_int(F f) {
    if (f != f) {
        return 0;
    }
    double r = (double)ts_round<Mode>(f);
    const int bits = sizeof(I) * 8;
    if ((I)-1 < 0) {
        double top = (double)((uint64_t)1 << (bits - 1));
        if (r >= top) return (I)(((uint64_t)1 << (bits - 1)) - 1);
        if (r < -top) return (I)((uint64_t)1 << (bits - 1));
        return (I)r;
    }
    double top = bits == 64 ? 18446744073709551616.0 : (double)((uint64_t)1 << bits);
    if (r < 0) return 0;
    if (r >= top) return (I)~(I)0;
    return (I)r;
}

// the same lane by lane, E the int lane type
template <typename IV, typename E, int Mode, typename FV>
static inline IV ts_to_int_v(FV f) {
    IV out;
    for (unsigned i = 0; i < sizeof(IV) / sizeof(E); ++i) {
        out[i] = ts_to_int<E, Mode>(f[i]);
    }
    return out;
}

// fmulx: like fmul but 0 * inf is 2
template <typename F>
static inline F ts_fmulx(F a, F b) {
    if ((a == 0 && __builtin_isinf(b)) || (b == 0 && __builtin_isinf(a))) {
        return (__builtin_signbit(a) != __builtin_signbit(b)) ? (F)-2.0 : (F)2.0;
    }
    return a * b;
}

// frecps: 2 - a * b, fused
template <typename F>
static inline F ts_frecps(F a, F b) {
    if ((a == 0 && __builtin_isinf(b)) || (b == 0 && __builtin_isinf(a))) {
        return (F)2.0;
    }
    return __builtin_elementwise_fma(-a, b, (F)2.0);
}

// frsqrts: (3 - a * b) / 2, fused
template <typename F>
static inline F ts_frsqrts(F a, F b) {
    if ((a == 0 && __builtin_isinf(b)) || (b == 0 && __builtin_isinf(a))) {
        return (F)1.5;
    }
    return __builtin_elementwise_fma(-a, b, (F)3.0) / (F)2.0;
}

extern "C" {
// the arm estimate tables, bit exact
float tsugiki_frecpe_s(float x);
double tsugiki_frecpe_d(double x);
float tsugiki_frsqrte_s(float x);
double tsugiki_frsqrte_d(double x);
float tsugiki_frecpx_s(float x);
double tsugiki_frecpx_d(double x);
uint32_t tsugiki_urecpe(uint32_t x);
uint32_t tsugiki_ursqrte(uint32_t x);
int32_t tsugiki_fjcvtzs(tsugiki_state* s, double x);
void tsugiki_aes(tsugiki_state* s, int op, int d, int n);
void tsugiki_sha(tsugiki_state* s, int op, int d, int n, int m);
}

template <typename V, typename E>
static inline V ts_vfmulx(V a, V b) {
    V out;
    for (unsigned i = 0; i < sizeof(V) / sizeof(E); ++i) out[i] = ts_fmulx<E>(a[i], b[i]);
    return out;
}

template <typename V, typename E>
static inline V ts_vfrecps(V a, V b) {
    V out;
    for (unsigned i = 0; i < sizeof(V) / sizeof(E); ++i) out[i] = ts_frecps<E>(a[i], b[i]);
    return out;
}

template <typename V, typename E>
static inline V ts_vfrsqrts(V a, V b) {
    V out;
    for (unsigned i = 0; i < sizeof(V) / sizeof(E); ++i) out[i] = ts_frsqrts<E>(a[i], b[i]);
    return out;
}

// value of a lane as a wide signed number, unsigned lanes zero-extended
template <typename E>
static inline __int128 ts_wide(E x) {
    return (__int128)x;
}

template <typename E>
static inline E ts_clamp(__int128 r) {
    const bool is_signed = (E)-1 < 0;
    const int bits = sizeof(E) * 8;
    __int128 hi = is_signed ? ((__int128)1 << (bits - 1)) - 1 : ((__int128)1 << bits) - 1;
    __int128 lo = is_signed ? -((__int128)1 << (bits - 1)) : 0;
    return (E)(r > hi ? hi : r < lo ? lo : r);
}

// sshl, ushl and the rounding and saturating ones: shift by the signed low byte of each lane
template <typename V, typename E, bool Round, bool Saturate>
static inline V ts_vshl(V a, V b) {
    V out;
    const int bits = sizeof(E) * 8;
    for (unsigned i = 0; i < sizeof(V) / sizeof(E); ++i) {
        int shift = (int8_t)b[i];
        __int128 x = ts_wide<E>(a[i]), r;
        if (shift >= 0) {
            // anything past the width just saturates or vanishes
            if (shift >= bits) {
                r = x == 0 ? 0 : Saturate ? (x > 0 ? ((__int128)1 << 100) : -((__int128)1 << 100)) : 0;
            } else {
                r = x << shift;
            }
        } else {
            int right = -shift;
            if (Round) {
                r = right > bits ? 0 : (x + ((__int128)1 << (right - 1))) >> right;
            } else {
                r = right >= bits ? (x < 0 ? -1 : 0) : x >> right;
            }
        }
        out[i] = Saturate ? ts_clamp<E>(r) : (E)r;
    }
    return out;
}

// halving and rounding halving
template <typename V, typename E>
static inline V ts_vhadd(V a, V b, int round) {
    V out;
    for (unsigned i = 0; i < sizeof(V) / sizeof(E); ++i) out[i] = (E)((ts_wide<E>(a[i]) + ts_wide<E>(b[i]) + round) >> 1);
    return out;
}

template <typename V, typename E>
static inline V ts_vhsub(V a, V b) {
    V out;
    for (unsigned i = 0; i < sizeof(V) / sizeof(E); ++i) out[i] = (E)((ts_wide<E>(a[i]) - ts_wide<E>(b[i])) >> 1);
    return out;
}

template <typename V>
static inline V ts_vabd(V a, V b) {
    return __builtin_elementwise_max(a, b) - __builtin_elementwise_min(a, b);
}

// rounding shift right by an immediate, wide enough not to overflow
template <typename V, typename E>
static inline V ts_vrshr(V a, int shift) {
    V out;
    for (unsigned i = 0; i < sizeof(V) / sizeof(E); ++i) out[i] = (E)((ts_wide<E>(a[i]) + ((__int128)1 << (shift - 1))) >> shift);
    return out;
}

// saturating shift left by an immediate, into To's range
template <typename V, typename E, typename To>
static inline V ts_vqshl(V a, int shift) {
    V out;
    for (unsigned i = 0; i < sizeof(V) / sizeof(E); ++i) out[i] = (E)ts_clamp<To>(ts_wide<E>(a[i]) << shift);
    return out;
}

// sqdmulh, sqrdmulh: doubling multiply high, saturating
template <typename V, typename E, bool Round>
static inline V ts_vqdmulh(V a, V b) {
    V out;
    const int bits = sizeof(E) * 8;
    for (unsigned i = 0; i < sizeof(V) / sizeof(E); ++i) {
        __int128 p = ts_wide<E>(a[i]) * ts_wide<E>(b[i]) * 2;
        if (Round) p += (__int128)1 << (bits - 1);
        out[i] = ts_clamp<E>(p >> bits);
    }
    return out;
}

// lanes clamped into a narrower type
template <typename ToV, typename To, typename FromV, typename From>
static inline ToV ts_vsat(FromV x) {
    ToV out;
    for (unsigned i = 0; i < sizeof(ToV) / sizeof(To); ++i) out[i] = ts_clamp<To>(ts_wide<From>(x[i]));
    return out;
}

// tbl and tbx over up to four table registers
static inline ts_u8 ts_tbl(const tsugiki_state* s, int first, int count, ts_u8 index, ts_u8 fallback) {
    uint8_t table[64];
    for (int i = 0; i < count; ++i) memcpy(table + 16 * i, &s->v[(first + i) & 31], 16);
    ts_u8 out;
    for (int i = 0; i < 16; ++i) out[i] = index[i] < 16 * count ? table[index[i]] : fallback[i];
    return out;
}

template <typename V, typename E>
static inline V ts_vcls(V a) {
    V out;
    const int bits = sizeof(E) * 8;
    for (unsigned i = 0; i < sizeof(V) / sizeof(E); ++i) {
        int64_t x = (int64_t)a[i];
        int n = 0;
        for (int b = bits - 2; b >= 0 && (((x >> b) & 1) == ((x >> (bits - 1)) & 1)); --b) ++n;
        out[i] = (E)n;
    }
    return out;
}

template <typename V, typename E>
static inline V ts_vclz(V a) {
    V out;
    const int bits = sizeof(E) * 8;
    for (unsigned i = 0; i < sizeof(V) / sizeof(E); ++i) {
        uint64_t x = (uint64_t)a[i] & (~(uint64_t)0 >> (64 - bits));
        out[i] = (E)(x == 0 ? bits : __builtin_clzll(x) - (64 - bits));
    }
    return out;
}

// pairwise over a then b, adjacent pairs, used lanes only
template <typename V, typename E, typename F>
static inline V ts_vpair(V a, V b, F op, bool q) {
    const unsigned lanes = sizeof(V) / sizeof(E);
    const unsigned used = q ? lanes : lanes / 2;
    E all[2 * lanes];
    for (unsigned i = 0; i < used; ++i) all[i] = a[i], all[used + i] = b[i];
    V out = {};
    for (unsigned i = 0; i < used; ++i) out[i] = (E)op(all[2 * i], all[2 * i + 1]);
    return out;
}

// the unsigned estimate and polynomial bits
static inline uint64_t ts_pmul8(uint8_t a, uint8_t b) {
    uint64_t r = 0;
    for (int i = 0; i < 8; ++i) if (b >> i & 1) r ^= (uint64_t)a << i;
    return r;
}

static inline unsigned __int128 ts_pmul64(uint64_t a, uint64_t b) {
    unsigned __int128 r = 0;
    for (int i = 0; i < 64; ++i) if (b >> i & 1) r ^= (unsigned __int128)a << i;
    return r;
}

// every lane x, no arithmetic so -0.0 stays -0.0
template <typename V, typename E>
static inline V ts_splat(E x) {
    V v;
    for (unsigned i = 0; i < sizeof(V) / sizeof(E); ++i) v[i] = x;
    return v;
}
