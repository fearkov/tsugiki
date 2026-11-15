// on the device: translated libs through the real loader, against the same c built for x86_64
#include <dlfcn.h>

#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

extern "C" {
uint64_t native_fib(uint64_t);
int native_dispatch(int, int);
int native_through_pointer(int, int);
int64_t native_divide(int64_t, int64_t);
uint64_t native_udivide(uint64_t, uint64_t);
uint32_t native_bits(uint32_t);
int64_t native_widen(int8_t, int16_t, int32_t, uint8_t, uint16_t, uint32_t);
void native_sort(int*, int);
uint32_t native_crc(const uint8_t*, int);
uint64_t native_atomics(uint64_t*, uint64_t);
double native_scalar_math(double, double, float, float);
int64_t native_conversions(double, float, int64_t, uint32_t);
void native_saxpy(float*, const float*, float, int);
double native_dot(const double*, const double*, int);
float native_matrix(const float*, const float*, float*);
}

namespace {

int failures = 0;
int checks = 0;

#define CHECK(x)                                                         \
    do {                                                                 \
        ++checks;                                                        \
        if (!(x)) {                                                      \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x);      \
            ++failures;                                                  \
        }                                                                \
    } while (0)

void* load(const char* name) {
    void* lib = dlopen(name, RTLD_NOW);
    if (!lib) {
        std::printf("dlopen %s: %s\n", name, dlerror());
        std::exit(1);
    }
    return lib;
}

template <typename F>
F get(void* lib, const char* name) {
    void* f = dlsym(lib, name);
    if (!f) {
        std::printf("no %s\n", name);
        std::exit(1);
    }
    return reinterpret_cast<F>(f);
}

bool same(double a, double b) { return (std::isnan(a) && std::isnan(b)) || std::memcmp(&a, &b, 8) == 0; }
bool same(float a, float b) { return (std::isnan(a) && std::isnan(b)) || std::memcmp(&a, &b, 4) == 0; }

template <typename F>
double ms(F&& f) {
    auto start = std::chrono::steady_clock::now();
    f();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

void kernels() {
    void* lib = load("libkernels.so");
    auto fib = get<uint64_t (*)(uint64_t)>(lib, "fib");
    auto dispatch = get<int (*)(int, int)>(lib, "dispatch");
    auto through_pointer = get<int (*)(int, int)>(lib, "through_pointer");
    auto divide = get<int64_t (*)(int64_t, int64_t)>(lib, "divide");
    auto udivide = get<uint64_t (*)(uint64_t, uint64_t)>(lib, "udivide");
    auto bits = get<uint32_t (*)(uint32_t)>(lib, "bits");
    auto widen = get<int64_t (*)(int8_t, int16_t, int32_t, uint8_t, uint16_t, uint32_t)>(lib, "widen");
    auto sort = get<void (*)(int*, int)>(lib, "sort");
    auto crc = get<uint32_t (*)(const uint8_t*, int)>(lib, "crc");
    auto atomics = get<uint64_t (*)(uint64_t*, uint64_t)>(lib, "atomics");

    std::mt19937_64 random(42);
    for (int i = 0; i < 2000; ++i) {
        uint64_t a = random(), b = random(), c = random();
        int64_t sa = int64_t(a) >> (a & 31), sb = int64_t(b) >> (b & 31);
        CHECK(divide(sa, sb) == native_divide(sa, sb));
        CHECK(udivide(a >> (c & 63), b >> (c & 31)) == native_udivide(a >> (c & 63), b >> (c & 31)));
        CHECK(bits(uint32_t(a)) == native_bits(uint32_t(a)));
        CHECK(widen(int8_t(a), int16_t(b), int32_t(c), uint8_t(a >> 8), uint16_t(b >> 16), uint32_t(c >> 32)) ==
              native_widen(int8_t(a), int16_t(b), int32_t(c), uint8_t(a >> 8), uint16_t(b >> 16), uint32_t(c >> 32)));
        int which = int(a % 11) - 1, x = int(int32_t(b) >> (c & 15));
        CHECK(dispatch(which, x) == native_dispatch(which, x));
        CHECK(through_pointer(which, x) == native_through_pointer(which, x));
    }
    for (int round = 0; round < 50; ++round) {
        int n = int(random() % 200);
        std::vector<int> a(size_t(n) + 1), b;
        for (auto& v : a) v = int(random() % 1000) - 500;
        b = a;
        sort(a.data(), n);
        native_sort(b.data(), n);
        CHECK(a == b);
        std::vector<uint8_t> bytes(size_t(n) + 1);
        for (auto& v : bytes) v = uint8_t(random());
        CHECK(crc(bytes.data(), n) == native_crc(bytes.data(), n));
        uint64_t cell_a = random(), cell_b = cell_a, v = random();
        CHECK(atomics(&cell_a, v) == native_atomics(&cell_b, v));
        CHECK(cell_a == cell_b);
    }
    uint64_t f1 = 0, f2 = 0;
    double translated = ms([&] { f1 = fib(30); });
    double native = ms([&] { f2 = native_fib(30); });
    CHECK(f1 == f2);
    std::vector<uint8_t> big(1 << 20, 0x5a);
    uint32_t c1 = 0, c2 = 0;
    double crc_translated = ms([&] { c1 = crc(big.data(), int(big.size())); });
    double crc_native = ms([&] { c2 = native_crc(big.data(), int(big.size())); });
    CHECK(c1 == c2);
    std::printf("kernels: fib(30) %.1f ms vs %.1f native, crc of 1 MiB %.1f ms vs %.1f native\n", translated, native, crc_translated, crc_native);
}

void vectors() {
    void* lib = load("libvectors.so");
    auto scalar_math = get<double (*)(double, double, float, float)>(lib, "scalar_math");
    auto conversions = get<int64_t (*)(double, float, int64_t, uint32_t)>(lib, "conversions");
    auto saxpy = get<void (*)(float*, const float*, float, int)>(lib, "saxpy");
    auto dot = get<double (*)(const double*, const double*, int)>(lib, "dot");
    auto matrix = get<float (*)(const float*, const float*, float*)>(lib, "matrix");

    std::mt19937_64 random(5);
    std::uniform_real_distribution<double> wide(-1e6, 1e6);
    for (int i = 0; i < 3000; ++i) {
        double a = wide(random), b = wide(random) / 7;
        float c = float(wide(random) / 3), d = float(wide(random) / 11);
        CHECK(same(scalar_math(a, b, c, d), native_scalar_math(a, b, c, d)));
        int64_t n = int64_t(random()) >> 12;
        uint32_t w = uint32_t(random());
        double big = wide(random) * 1e9;
        CHECK(conversions(big, c, n, w) == native_conversions(big, c, n, w));
    }
    std::uniform_real_distribution<float> fl(-100, 100);
    for (int round = 0; round < 40; ++round) {
        int n = int(random() % 300) + 1;
        std::vector<float> x(n), y(n), y2;
        for (auto& v : x) v = fl(random);
        for (auto& v : y) v = fl(random);
        y2 = y;
        float a = fl(random);
        saxpy(y.data(), x.data(), a, n);
        native_saxpy(y2.data(), x.data(), a, n);
        bool all = true;
        for (int i = 0; i < n; ++i) all &= same(y[i], y2[i]);
        CHECK(all);
        std::vector<double> da(n), db(n);
        for (auto& v : da) v = fl(random);
        for (auto& v : db) v = fl(random);
        CHECK(same(dot(da.data(), db.data(), n), native_dot(da.data(), db.data(), n)));
        float m[16], v[4], o1[4], o2[4];
        for (auto& e : m) e = fl(random);
        for (auto& e : v) e = fl(random);
        CHECK(same(matrix(m, v, o1), native_matrix(m, v, o2)));
        CHECK(std::memcmp(o1, o2, sizeof o1) == 0);
    }
    std::printf("vectors: done\n");
}

void bridge() {
    void* lib = load("libbridge.so");
    CHECK(get<int (*)()>(lib, "bridge_constructed")() == 42);
    std::vector<int> p = {5, -3, 9, 0, 9, -100, 7, 2};
    CHECK(get<int (*)(int*, int)>(lib, "bridge_qsort")(p.data(), int(p.size())) == 1);
    CHECK(p.front() == -100 && p.back() == 9);

    char out[256], want[256];
    int n = get<int (*)(char*, int, int, double, const char*, long long, float)>(lib, "bridge_format")(out, sizeof out, -7, 3.14159, "hi", 1LL << 40, 0.25f);
    int m = std::snprintf(want, sizeof want, "%d|%.3f|%s|%lld|%g|%x|%c|%5.1e|%s", -7, 3.14159, "hi", 1LL << 40, 0.25, -21, 'z', 3141.59, "end");
    std::printf("bridge_format: %s\n", out);
    CHECK(n == m && std::strcmp(out, want) == 0);

    n = get<int (*)(char*, int, int, double, int, double, int, int, int, int, int, double)>(lib, "bridge_vformat")(out, sizeof out, 1, 2.5, 3, 4.25, 5, 6, 7, 8, 9, 10.75);
    m = std::snprintf(want, sizeof want, "%d %.2f %d %.2f %d %d %d %d %d %.2f", 1, 2.5, 3, 4.25, 5, 6, 7, 8, 9, 10.75);
    std::printf("bridge_vformat: %s\n", out);
    CHECK(n == m && std::strcmp(out, want) == 0);

    CHECK(get<long (*)(int)>(lib, "bridge_thread")(1000) == 500500);
    CHECK(get<int (*)(int)>(lib, "bridge_jump")(0) == 41);
    CHECK(get<int (*)()>(lib, "bridge_errno")() == EBADF);
    std::printf("bridge_hwcap: %lx\n", get<unsigned long (*)()>(lib, "bridge_hwcap")());
    CHECK(get<int (*)()>(lib, "bridge_once")() == 1);
    CHECK(get<int (*)(const char*, const char*)>(lib, "bridge_strings")("hello ", "world") == 11 * 100 + 6);
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string only = argc > 1 ? argv[1] : "";
    if (only.empty() || only == "kernels") kernels();
    if (only.empty() || only == "vectors") vectors();
    if (only.empty() || only == "bridge") bridge();
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures != 0;
}
