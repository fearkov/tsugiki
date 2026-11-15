// integer code built twice, arm64 to translate and native to compare
// no fp/simd yet, the arm64 build uses -mgeneral-regs-only

#include <stdint.h>

#ifdef NATIVE
#define K(name) native_##name
#else
#define K(name) name
#endif

#define NOINLINE __attribute__((noinline))

uint64_t K(fib)(uint64_t n) { return n < 2 ? n : K(fib)(n - 1) + K(fib)(n - 2); }

NOINLINE static int twice(int x) { return x * 2; }
NOINLINE static int square(int x) { return x * x; }
NOINLINE static int negate(int x) { return -x; }

// a real jump table, the cases call out
int K(dispatch)(int which, int x) {
    switch (which) {
    case 0: return twice(x);
    case 1: return square(x) + 1;
    case 2: return negate(x) - 3;
    case 3: return twice(x) ^ 0x55;
    case 4: return square(x) >> 2;
    case 5: return negate(twice(x));
    case 6: return x % 7;
    case 7: return x / 3;
    case 8: return twice(square(x));
    default: return -1;
    }
}

typedef int (*operation)(int);
static operation const operations[] = {twice, square, negate};

int K(through_pointer)(int which, int x) { return operations[(unsigned)which % 3](x); }

int64_t K(divide)(int64_t a, int64_t b) { return b ? a / b : 0; }
uint64_t K(udivide)(uint64_t a, uint64_t b) { return b ? a / b : 0; }
int64_t K(remainder)(int64_t a, int64_t b) { return b ? a % b : 0; }

uint32_t K(bits)(uint32_t x) {
    return (uint32_t)__builtin_popcount(x) + (uint32_t)__builtin_clz(x | 1) + (uint32_t)__builtin_ctz(x | 0x80000000u) + ((x >> 3) ^ (x << 7));
}

uint64_t K(rotate)(uint64_t x, unsigned n) { return (x >> (n & 63)) | (x << ((64 - n) & 63)); }

int64_t K(mulhi)(int64_t a, int64_t b) { return (int64_t)(((__int128)a * b) >> 64); }
uint64_t K(umulhi)(uint64_t a, uint64_t b) { return (uint64_t)(((unsigned __int128)a * b) >> 64); }

uint64_t K(sum_array)(const uint32_t* p, int n) {
    uint64_t s = 0;
    for (int i = 0; i < n; i++) {
        s += (uint64_t)p[i] * (uint64_t)(i + 1);
    }
    return s;
}

void K(sort)(int* p, int n) {
    for (int i = 1; i < n; i++) {
        int v = p[i], j = i - 1;
        while (j >= 0 && p[j] > v) {
            p[j + 1] = p[j];
            j--;
        }
        p[j + 1] = v;
    }
}

uint32_t K(crc)(const uint8_t* p, int n) {
    uint32_t c = 0xffffffffu;
    for (int i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
        }
    }
    return ~c;
}

int K(compare)(int64_t a, int64_t b) { return (a > b) - (a < b); }

int64_t K(widen)(int8_t a, int16_t b, int32_t c, uint8_t d, uint16_t e, uint32_t f) { return a * 3 + b * 5 + c * 7 + d * 11 + e * 13 + (int64_t)f * 17; }

struct fields {
    unsigned a : 3, b : 7, c : 11, d : 5;
    signed e : 6;
};

uint64_t K(bitfields)(uint64_t x) {
    struct fields f;
    f.a = (unsigned)x;
    f.b = (unsigned)(x >> 3);
    f.c = (unsigned)(x >> 10);
    f.d = (unsigned)(x >> 21);
    f.e = (int)(x >> 26);
    return (uint64_t)f.a * 1000003u + f.b * 10007u + f.c * 101u + f.d * 7u + (uint64_t)(int64_t)f.e;
}

int K(string_length)(const char* s) {
    int n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

uint64_t K(atomics)(uint64_t* p, uint64_t v) {
    uint64_t a = __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST);
    uint64_t b = __atomic_exchange_n(p, a ^ v, __ATOMIC_SEQ_CST);
    uint64_t expected = a ^ v;
    __atomic_compare_exchange_n(p, &expected, b + 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return a * 31 + b * 7 + *p;
}

int64_t K(select)(int64_t a, int64_t b, int64_t c) { return a > b ? (b > c ? a - c : a + c) : (a == c ? b : -b); }

uint64_t K(collatz)(uint64_t n) {
    uint64_t steps = 0;
    while (n != 1 && steps < 10000) {
        n = (n & 1) ? 3 * n + 1 : n / 2;
        steps++;
    }
    return steps;
}
