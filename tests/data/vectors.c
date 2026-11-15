// fp and loops the compiler vectorizes, built for arm64 and natively
// -ffp-contract=off on both sides so only fmaf fuses

#include <stdint.h>

#ifdef NATIVE
#define K(name) native_##name
#else
#define K(name) name
#endif

double K(scalar_math)(double a, double b, float c, float d) {
    double r = a * b + a / (b == 0 ? 1 : b) - __builtin_sqrt(__builtin_fabs(a)) + __builtin_floor(b) + __builtin_ceil(a) + __builtin_trunc(b) + __builtin_round(a) + __builtin_rint(b) + __builtin_nearbyint(a);
    float f = c * d - c / (d == 0 ? 1 : d) + __builtin_sqrtf(__builtin_fabsf(c)) + __builtin_floorf(d) + __builtin_ceilf(c) + __builtin_truncf(d) + __builtin_roundf(c) + __builtin_fmaf(c, d, c);
    r += __builtin_fma(a, b, -a) + __builtin_fmin(a, b) + __builtin_fmax(a, b) + (double)__builtin_fminf(c, d) + (double)__builtin_fmaxf(c, d);
    r += __builtin_copysign(a, b) + (double)__builtin_copysignf(c, d) + (a < b ? 1.5 : -2.5) + (c >= d ? 3.0f : 4.0f);
    return r + f + (double)(float)a;
}

int64_t K(conversions)(double a, float b, int64_t i, uint32_t u) {
    // in range so c defines them
    double clamp_a = a > 1e15 ? 1e15 : a < -1e15 ? -1e15 : a;
    float clamp_b = b > 2e9f ? 2e9f : b < -2e9f ? -2e9f : b;
    int64_t r = (int64_t)clamp_a + (int32_t)clamp_b + (int64_t)(double)i + (int64_t)(float)u + (int64_t)__builtin_lround(clamp_a) + (int64_t)__builtin_lrintf(clamp_b);
    r += (int64_t)(uint32_t)(clamp_b < 0 ? -clamp_b : clamp_b) + (int64_t)(uint64_t)(clamp_a < 0 ? -clamp_a : clamp_a);
    r += (int64_t)((double)i * 0.25) + (int64_t)(float)(int16_t)u;
    return r;
}

void K(saxpy)(float* y, const float* x, float a, int n) {
    for (int i = 0; i < n; i++) y[i] = a * x[i] + y[i];
}

double K(dot)(const double* a, const double* b, int n) {
    double s = 0;
    for (int i = 0; i < n; i++) s += a[i] * b[i];
    return s;
}

uint32_t K(byte_sum)(const uint8_t* p, int n) {
    uint32_t s = 0;
    for (int i = 0; i < n; i++) s += p[i];
    return s;
}

void K(widen_narrow)(const int16_t* a, const int16_t* b, int32_t* wide, int8_t* narrow, int n) {
    for (int i = 0; i < n; i++) {
        wide[i] = (int32_t)a[i] * b[i];
        narrow[i] = (int8_t)(a[i] >> 4);
    }
}

void K(clamp_bytes)(const int32_t* in, uint8_t* out, int n) {
    for (int i = 0; i < n; i++) {
        int32_t v = in[i];
        out[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
}

uint32_t K(max_min)(const uint16_t* p, const int32_t* q, int n) {
    uint16_t hi = 0;
    int32_t lo = 0x7fffffff;
    for (int i = 0; i < n; i++) {
        hi = p[i] > hi ? p[i] : hi;
        lo = q[i] < lo ? q[i] : lo;
    }
    return hi + (uint32_t)lo;
}

uint32_t K(abs_diff)(const uint8_t* a, const uint8_t* b, int n) {
    uint32_t s = 0;
    for (int i = 0; i < n; i++) s += a[i] > b[i] ? a[i] - b[i] : b[i] - a[i];
    return s;
}

void K(select_bits)(const uint32_t* a, const uint32_t* b, uint32_t* out, int n) {
    for (int i = 0; i < n; i++) {
        uint32_t x = a[i], y = b[i];
        out[i] = (x > y ? x ^ y : x & ~y) | ((x << 3) ^ (y >> 5)) | (uint32_t)((int32_t)x >> 7);
    }
}

void K(rgb_planes)(const uint8_t* rgb, uint8_t* r, uint8_t* g, uint8_t* b, int n) {
    for (int i = 0; i < n; i++) {
        r[i] = rgb[3 * i];
        g[i] = rgb[3 * i + 1];
        b[i] = rgb[3 * i + 2];
    }
}

void K(interleave)(const uint16_t* a, const uint16_t* b, uint16_t* out, int n) {
    for (int i = 0; i < n; i++) {
        out[2 * i] = a[i];
        out[2 * i + 1] = b[i];
    }
}

void K(float_ints)(const float* f, int32_t* i, float* back, const int32_t* j, int n) {
    for (int k = 0; k < n; k++) {
        i[k] = (int32_t)f[k];
        back[k] = (float)j[k] * 0.5f;
    }
}

void K(mix_precision)(const double* d, float* f, double* e, int n) {
    for (int k = 0; k < n; k++) {
        f[k] = (float)d[k];
        e[k] = (double)f[k] * 2.0;
    }
}

void K(reverse_words)(const uint32_t* in, uint32_t* out, int n) {
    for (int i = 0; i < n; i++) out[i] = __builtin_bswap32(in[n - 1 - i]);
}

float K(matrix)(const float* m, const float* v, float* out) {
    float s = 0;
    for (int r = 0; r < 4; r++) {
        float acc = 0;
        for (int c = 0; c < 4; c++) acc += m[4 * r + c] * v[c];
        out[r] = acc;
        s += acc;
    }
    return s;
}

int K(compare_floats)(const float* a, const float* b, int n) {
    int count = 0;
    for (int i = 0; i < n; i++) count += (a[i] < b[i]) + 2 * (a[i] == b[i]) + 4 * (a[i] >= b[i]);
    return count;
}

uint64_t K(lookup)(const uint8_t* index, uint8_t* out, int n) {
    static const uint8_t table[16] = {9, 3, 7, 1, 15, 11, 0, 4, 2, 14, 6, 10, 8, 13, 5, 12};
    uint64_t s = 0;
    for (int i = 0; i < n; i++) {
        out[i] = table[index[i] & 15];
        s += out[i];
    }
    return s;
}
