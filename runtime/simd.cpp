// the fp/simd bits too big to inline: arm's estimates, js conversion, aes, sha
#include <cmath>
#include <cstdint>
#include <cstring>

#include "tsugiki.h"

namespace {

// RecipEstimate from the arm arm, 256 <= a < 512
uint32_t recip_estimate(uint32_t a) {
    a = a * 2 + 1;
    uint32_t b = (1u << 19) / a;
    return (b + 1) / 2;
}

// RecipSqrtEstimate, 128 <= a < 512
uint32_t recip_sqrt_estimate(uint32_t a) {
    if (a < 256) {
        a = a * 2 + 1;
    } else {
        a = (a >> 1) << 1;
        a = (a + 1) * 2;
    }
    uint64_t b = 512;
    while (uint64_t(a) * (b + 1) * (b + 1) < (uint64_t(1) << 28)) {
        ++b;
    }
    return uint32_t((b + 1) / 2);
}

// both precisions through 64-bit fields: sign, biased exp, 52-bit fraction
struct Unpacked {
    uint64_t sign;
    int64_t exp;
    uint64_t fraction;
};

Unpacked unpack(uint64_t bits, int n) {
    if (n == 32) {
        return {bits >> 31 & 1, int64_t(bits >> 23 & 0xff), (bits & 0x7fffff) << 29};
    }
    return {bits >> 63, int64_t(bits >> 52 & 0x7ff), bits & 0xfffffffffffffull};
}

uint64_t pack(uint64_t sign, int64_t exp, uint64_t fraction, int n) {
    if (n == 32) {
        return (sign << 31) | (uint64_t(exp & 0xff) << 23) | (fraction >> 29);
    }
    return (sign << 63) | (uint64_t(exp & 0x7ff) << 52) | (fraction & 0xfffffffffffffull);
}

template <typename F, typename B>
F reinterpret(B bits) {
    F f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

template <typename F>
F quiet(F x) {
    return x + x;
}

template <typename F, typename B, int N>
F frecpe(F x) {
    if (std::isnan(x)) {
        return quiet(x);
    }
    if (std::isinf(x)) {
        return std::copysign(F(0), x);
    }
    if (x == 0) {
        return std::copysign(F(INFINITY), x);
    }
    // too small to have a finite reciprocal
    F tiny = N == 32 ? std::ldexp(F(1), -128) : std::ldexp(F(1), -1024);
    if (std::fabs(x) < tiny) {
        return std::copysign(F(INFINITY), x);
    }
    B bits;
    std::memcpy(&bits, &x, sizeof bits);
    Unpacked u = unpack(uint64_t(bits), N);
    uint64_t fraction = u.fraction;
    int64_t exp = u.exp;
    if (exp == 0) {
        if (!(fraction >> 51 & 1)) {
            exp = -1;
            fraction = (fraction << 2) & 0xfffffffffffffull;
        } else {
            fraction = (fraction << 1) & 0xfffffffffffffull;
        }
    }
    uint32_t scaled = uint32_t(0x100 | (fraction >> 44 & 0xff));
    int64_t result_exp = (N == 32 ? 253 : 2045) - exp;
    uint32_t estimate = recip_estimate(scaled);
    fraction = uint64_t(estimate & 0xff) << 44;
    if (result_exp == 0) {
        fraction = (uint64_t(1) << 51) | (fraction >> 1);
    } else if (result_exp == -1) {
        fraction = (uint64_t(1) << 50) | (fraction >> 2);
        result_exp = 0;
    }
    return reinterpret<F>(B(pack(u.sign, result_exp, fraction, N)));
}

template <typename F, typename B, int N>
F frsqrte(F x) {
    if (std::isnan(x)) {
        return quiet(x);
    }
    if (x == 0) {
        return std::copysign(F(INFINITY), x);
    }
    if (std::signbit(x)) {
        return F(NAN);
    }
    if (std::isinf(x)) {
        return F(0);
    }
    B bits;
    std::memcpy(&bits, &x, sizeof bits);
    Unpacked u = unpack(uint64_t(bits), N);
    uint64_t fraction = u.fraction;
    int64_t exp = u.exp;
    if (exp == 0) {
        while (!(fraction >> 51 & 1)) {
            fraction = (fraction << 1) & 0xfffffffffffffull;
            --exp;
        }
        fraction = (fraction << 1) & 0xfffffffffffffull;
    }
    uint32_t scaled = (exp & 1) == 0 ? uint32_t(0x100 | (fraction >> 44 & 0xff)) : uint32_t(0x80 | (fraction >> 45 & 0x7f));
    int64_t numerator = (N == 32 ? 380 : 3068) - exp;
    // DIV rounds down
    int64_t result_exp = numerator >= 0 ? numerator / 2 : -((-numerator + 1) / 2);
    uint32_t estimate = recip_sqrt_estimate(scaled);
    return reinterpret<F>(B(pack(0, result_exp, uint64_t(estimate & 0xff) << 44, N)));
}

template <typename F, typename B, int N>
F frecpx(F x) {
    if (std::isnan(x)) {
        return quiet(x);
    }
    B bits;
    std::memcpy(&bits, &x, sizeof bits);
    Unpacked u = unpack(uint64_t(bits), N);
    int64_t max = N == 32 ? 0xfe : 0x7fe;
    int64_t exp = u.exp == 0 ? max : (~u.exp & (N == 32 ? 0xff : 0x7ff));
    return reinterpret<F>(B(pack(u.sign, exp, 0, N)));
}

// aes
struct Aes {
    uint8_t sbox[256], inverse[256];
    Aes() {
        // from the gf(2^8) inverse and the affine map
        uint8_t p = 1, q = 1;
        do {
            p = uint8_t(p ^ (p << 1) ^ (p & 0x80 ? 0x1b : 0));
            q ^= uint8_t(q << 1);
            q ^= uint8_t(q << 2);
            q ^= uint8_t(q << 4);
            if (q & 0x80) q ^= 0x09;
            uint8_t x = uint8_t(q ^ rotl(q, 1) ^ rotl(q, 2) ^ rotl(q, 3) ^ rotl(q, 4));
            sbox[p] = uint8_t(x ^ 0x63);
        } while (p != 1);
        sbox[0] = 0x63;
        for (int i = 0; i < 256; ++i) {
            inverse[sbox[i]] = uint8_t(i);
        }
    }
    static uint8_t rotl(uint8_t x, int n) { return uint8_t((x << n) | (x >> (8 - n))); }
};

const Aes& aes() {
    static const Aes tables;
    return tables;
}

uint8_t xtime(uint8_t x) {
    return uint8_t((x << 1) ^ (x & 0x80 ? 0x1b : 0));
}

uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b) {
        if (b & 1) r ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return r;
}

void get16(const tsugiki_state* s, int r, uint8_t out[16]) {
    std::memcpy(out, &s->v[r], 16);
}

void put16(tsugiki_state* s, int r, const uint8_t in[16]) {
    std::memcpy(&s->v[r], in, 16);
}

void words(const tsugiki_state* s, int r, uint32_t out[4]) {
    std::memcpy(out, &s->v[r], 16);
}

uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
uint32_t choose(uint32_t x, uint32_t y, uint32_t z) { return ((y ^ z) & x) ^ z; }
uint32_t parity(uint32_t x, uint32_t y, uint32_t z) { return x ^ y ^ z; }
uint32_t majority(uint32_t x, uint32_t y, uint32_t z) { return (x & y) | ((x | y) & z); }

}  // namespace

extern "C" {

float tsugiki_frecpe_s(float x) { return frecpe<float, uint32_t, 32>(x); }
double tsugiki_frecpe_d(double x) { return frecpe<double, uint64_t, 64>(x); }
float tsugiki_frsqrte_s(float x) { return frsqrte<float, uint32_t, 32>(x); }
double tsugiki_frsqrte_d(double x) { return frsqrte<double, uint64_t, 64>(x); }
float tsugiki_frecpx_s(float x) { return frecpx<float, uint32_t, 32>(x); }
double tsugiki_frecpx_d(double x) { return frecpx<double, uint64_t, 64>(x); }

uint32_t tsugiki_urecpe(uint32_t x) {
    if (!(x >> 31)) {
        return 0xffffffff;
    }
    return (recip_estimate(x >> 23) & 0x1ff) << 23;
}

uint32_t tsugiki_ursqrte(uint32_t x) {
    if ((x >> 30) == 0) {
        return 0xffffffff;
    }
    return (recip_sqrt_estimate(x >> 23) & 0x1ff) << 23;
}

int32_t tsugiki_fjcvtzs(tsugiki_state* s, double x) {
    s->nf = 0, s->cf = 0, s->vf = 0;
    if (std::isnan(x) || std::isinf(x)) {
        s->zf = 0;
        return 0;
    }
    double t = std::trunc(x);
    double wrapped = std::fmod(t, 4294967296.0);
    if (wrapped < 0) {
        wrapped += 4294967296.0;
    }
    uint32_t r = uint32_t(wrapped);
    bool exact = t == x && x >= -2147483648.0 && x <= 2147483647.0 && !(x == 0 && std::signbit(x));
    s->zf = exact;
    return int32_t(r);
}

void tsugiki_aes(tsugiki_state* s, int op, int d, int n) {
    const Aes& t = aes();
    uint8_t a[16], b[16], r[16];
    get16(s, d, a);
    get16(s, n, b);
    switch (op) {
    case 4:  // aese: xor, shift rows, sub bytes
    case 5: {  // aesd: xor, inverse shift rows, inverse sub bytes
        for (int i = 0; i < 16; ++i) a[i] ^= b[i];
        for (int c = 0; c < 4; ++c) {
            for (int row = 0; row < 4; ++row) {
                if (op == 4) {
                    r[c * 4 + row] = t.sbox[a[((c + row) % 4) * 4 + row]];
                } else {
                    r[((c + row) % 4) * 4 + row] = t.inverse[a[c * 4 + row]];
                }
            }
        }
        break;
    }
    case 6:  // aesmc
    case 7:  // aesimc
        for (int c = 0; c < 4; ++c) {
            uint8_t* x = b + 4 * c;
            if (op == 6) {
                r[4 * c + 0] = uint8_t(gmul(x[0], 2) ^ gmul(x[1], 3) ^ x[2] ^ x[3]);
                r[4 * c + 1] = uint8_t(x[0] ^ gmul(x[1], 2) ^ gmul(x[2], 3) ^ x[3]);
                r[4 * c + 2] = uint8_t(x[0] ^ x[1] ^ gmul(x[2], 2) ^ gmul(x[3], 3));
                r[4 * c + 3] = uint8_t(gmul(x[0], 3) ^ x[1] ^ x[2] ^ gmul(x[3], 2));
            } else {
                r[4 * c + 0] = uint8_t(gmul(x[0], 14) ^ gmul(x[1], 11) ^ gmul(x[2], 13) ^ gmul(x[3], 9));
                r[4 * c + 1] = uint8_t(gmul(x[0], 9) ^ gmul(x[1], 14) ^ gmul(x[2], 11) ^ gmul(x[3], 13));
                r[4 * c + 2] = uint8_t(gmul(x[0], 13) ^ gmul(x[1], 9) ^ gmul(x[2], 14) ^ gmul(x[3], 11));
                r[4 * c + 3] = uint8_t(gmul(x[0], 11) ^ gmul(x[1], 13) ^ gmul(x[2], 9) ^ gmul(x[3], 14));
            }
        }
        break;
    default:
        return;
    }
    put16(s, d, r);
}

void tsugiki_sha(tsugiki_state* s, int op, int d, int n, int m) {
    uint32_t x[4], y[4], w[4], r[4];
    words(s, d, x);
    words(s, n, y);
    words(s, m, w);
    switch (op) {
    case 0: case 1: case 2: {
        // sha1c, sha1p, sha1m: x is abcd, y's low word e
        uint32_t e = y[0];
        for (int i = 0; i < 4; ++i) {
            uint32_t f = op == 0 ? choose(x[1], x[2], x[3]) : op == 1 ? parity(x[1], x[2], x[3]) : majority(x[1], x[2], x[3]);
            e = e + rol(x[0], 5) + f + w[i];
            x[1] = rol(x[1], 30);
            // rotate e:x left by 32
            uint32_t top = x[3];
            x[3] = x[2], x[2] = x[1], x[1] = x[0], x[0] = e;
            e = top;
        }
        std::memcpy(r, x, 16);
        break;
    }
    case 3:  // sha1su0
        r[0] = x[2] ^ x[0] ^ w[0];
        r[1] = x[3] ^ x[1] ^ w[1];
        r[2] = y[0] ^ x[2] ^ w[2];
        r[3] = y[1] ^ x[3] ^ w[3];
        break;
    case 4: case 5: {
        // sha256h, sha256h2
        uint32_t a[4], b[4];
        if (op == 4) {
            std::memcpy(a, x, 16), std::memcpy(b, y, 16);
        } else {
            std::memcpy(a, y, 16), std::memcpy(b, x, 16);
        }
        for (int i = 0; i < 4; ++i) {
            uint32_t chs = choose(b[0], b[1], b[2]);
            uint32_t maj = majority(a[0], a[1], a[2]);
            uint32_t t = b[3] + (ror(b[0], 6) ^ ror(b[0], 11) ^ ror(b[0], 25)) + chs + w[i];
            a[3] = t + a[3];
            b[3] = t + (ror(a[0], 2) ^ ror(a[0], 13) ^ ror(a[0], 22)) + maj;
            // rotate b:a left by 32
            uint32_t top_a = a[3], top_b = b[3];
            a[3] = a[2], a[2] = a[1], a[1] = a[0], a[0] = top_b;
            b[3] = b[2], b[2] = b[1], b[1] = b[0], b[0] = top_a;
        }
        std::memcpy(r, op == 4 ? a : b, 16);
        break;
    }
    case 6: {  // sha256su1
        uint32_t t0[4] = {y[1], y[2], y[3], w[0]};
        uint32_t t1[2] = {w[2], w[3]};
        for (int i = 0; i < 2; ++i) {
            uint32_t e = t1[i];
            r[i] = (ror(e, 17) ^ ror(e, 19) ^ (e >> 10)) + x[i] + t0[i];
        }
        for (int i = 2; i < 4; ++i) {
            uint32_t e = r[i - 2];
            r[i] = (ror(e, 17) ^ ror(e, 19) ^ (e >> 10)) + x[i] + t0[i];
        }
        break;
    }
    case 16:  // sha1h
        r[0] = rol(y[0], 30), r[1] = r[2] = r[3] = 0;
        break;
    case 17: {  // sha1su1
        uint32_t t[4] = {x[0] ^ y[1], x[1] ^ y[2], x[2] ^ y[3], x[3]};
        r[0] = rol(t[0], 1), r[1] = rol(t[1], 1), r[2] = rol(t[2], 1), r[3] = rol(t[3], 1) ^ rol(t[0], 2);
        break;
    }
    case 18: {  // sha256su0
        uint32_t t[4] = {x[1], x[2], x[3], y[0]};
        for (int i = 0; i < 4; ++i) r[i] = (ror(t[i], 7) ^ ror(t[i], 18) ^ (t[i] >> 3)) + x[i];
        break;
    }
    default:
        return;
    }
    std::memcpy(&s->v[d], r, 16);
}

}  // extern "C"
