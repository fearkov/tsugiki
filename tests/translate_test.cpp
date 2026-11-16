// the kernels translated from arm64 against the same c built natively
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "guest.hpp"
#include "test.hpp"

using namespace tsugiki;

extern "C" {
extern const tsugiki_entry tsugiki_entries_kernels[];
extern const uint64_t tsugiki_entry_count_kernels;
extern uint64_t tsugiki_base_kernels;

uint64_t native_fib(uint64_t);
int64_t native_flags_back(int64_t, int64_t);
int64_t native_flags_into(int64_t, int64_t);
int native_dispatch(int, int);
int native_through_pointer(int, int);
int64_t native_divide(int64_t, int64_t);
uint64_t native_udivide(uint64_t, uint64_t);
int64_t native_remainder(int64_t, int64_t);
uint32_t native_bits(uint32_t);
uint64_t native_rotate(uint64_t, unsigned);
int64_t native_mulhi(int64_t, int64_t);
uint64_t native_umulhi(uint64_t, uint64_t);
uint64_t native_sum_array(const uint32_t*, int);
void native_sort(int*, int);
uint32_t native_crc(const uint8_t*, int);
int native_compare(int64_t, int64_t);
int64_t native_widen(int8_t, int16_t, int32_t, uint8_t, uint16_t, uint32_t);
uint64_t native_bitfields(uint64_t);
int native_string_length(const char*);
uint64_t native_atomics(uint64_t*, uint64_t);
int64_t native_select(int64_t, int64_t, int64_t);
uint64_t native_collatz(uint64_t);
}

namespace {

Guest& guest() {
    static Guest* g = new Guest(std::string(SAMPLE_DIR) + "/libkernels.so", tsugiki_entries_kernels, tsugiki_entry_count_kernels, &tsugiki_base_kernels);
    return *g;
}

uint64_t u(int64_t v) { return uint64_t(v); }
uint64_t p(const void* v) { return reinterpret_cast<uint64_t>(v); }

}  // namespace

TEST(translated_scalar_kernels) {
    Guest& g = guest();
    for (uint64_t n : {0, 1, 2, 10, 20}) {
        CHECK_EQ(g.call("fib", {n}), native_fib(n));
    }
    std::mt19937_64 random(42);
    for (int i = 0; i < 2000; ++i) {
        uint64_t a = random(), b = random(), c = random();
        int64_t sa = int64_t(a) >> (a & 31), sb = int64_t(b) >> (b & 31);
        CHECK_EQ(g.call("divide", {u(sa), u(sb)}), u(native_divide(sa, sb)));
        CHECK_EQ(g.call("udivide", {a >> (c & 63), b >> (c & 31)}), native_udivide(a >> (c & 63), b >> (c & 31)));
        CHECK_EQ(g.call("remainder", {u(sa), u(sb | 1)}), u(native_remainder(sa, sb | 1)));
        CHECK_EQ(uint32_t(g.call("bits", {a})), native_bits(uint32_t(a)));
        CHECK_EQ(g.call("rotate", {a, b}), native_rotate(a, unsigned(b)));
        CHECK_EQ(g.call("mulhi", {a, b}), u(native_mulhi(int64_t(a), int64_t(b))));
        CHECK_EQ(g.call("umulhi", {a, b}), native_umulhi(a, b));
        CHECK_EQ(int(g.call("compare", {u(sa), u(sb)})), native_compare(sa, sb));
        CHECK_EQ(g.call("widen", {a, b, c, a >> 8, b >> 16, c >> 32}),
                 u(native_widen(int8_t(a), int16_t(b), int32_t(c), uint8_t(a >> 8), uint16_t(b >> 16), uint32_t(c >> 32))));
        CHECK_EQ(g.call("bitfields", {a}), native_bitfields(a));
        CHECK_EQ(g.call("select", {u(sa), u(sb), c & 7}), u(native_select(sa, sb, int64_t(c & 7))));
        int which = int(a % 11) - 1, x = int(int32_t(b) >> (c & 15));
        CHECK_EQ(int(g.call("dispatch", {u(which), u(x)})), native_dispatch(which, x));
        CHECK_EQ(int(g.call("through_pointer", {u(which), u(x)})), native_through_pointer(which, x));
        CHECK_EQ(g.call("flags_back", {u(sa), u(sb)}), u(native_flags_back(sa, sb)));
        CHECK_EQ(g.call("flags_into", {u(sa), u(sb)}), u(native_flags_into(sa, sb)));
    }
    for (uint64_t n : {1, 7, 27, 97, 871}) {
        CHECK_EQ(g.call("collatz", {n}), native_collatz(n));
    }
}

TEST(translated_memory_kernels) {
    Guest& g = guest();
    std::mt19937 random(7);
    for (int round = 0; round < 50; ++round) {
        int n = int(random() % 200);
        std::vector<uint32_t> words(size_t(n) + 1);
        for (auto& w : words) {
            w = random();
        }
        CHECK_EQ(g.call("sum_array", {p(words.data()), u(n)}), native_sum_array(words.data(), n));

        std::vector<int> a(size_t(n) + 1), b;
        for (auto& v : a) {
            v = int(random() % 1000) - 500;
        }
        b = a;
        g.call("sort", {p(a.data()), u(n)});
        native_sort(b.data(), n);
        CHECK(a == b);

        std::vector<uint8_t> bytes(size_t(n) + 1);
        for (auto& v : bytes) {
            v = uint8_t(random());
        }
        CHECK_EQ(uint32_t(g.call("crc", {p(bytes.data()), u(n)})), native_crc(bytes.data(), n));

        std::string text(size_t(n), 'x');
        CHECK_EQ(int(g.call("string_length", {p(text.c_str())})), native_string_length(text.c_str()));

        uint64_t cell_a = random(), cell_b = cell_a, v = random();
        CHECK_EQ(g.call("atomics", {p(&cell_a), v}), native_atomics(&cell_b, v));
        CHECK_EQ(cell_a, cell_b);
    }
}

TEST(speed_against_native) {
    Guest& g = guest();
    auto time = [](auto&& f) {
        auto start = std::chrono::steady_clock::now();
        f();
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    };
    uint64_t a = 0, b = 0;
    double translated = time([&] { a = g.call("fib", {30}); });
    double native = time([&] { b = native_fib(30); });
    std::vector<uint8_t> bytes(1 << 20, 0x5a);
    uint32_t c = 0, d = 0;
    double crc_translated = time([&] { c = uint32_t(g.call("crc", {p(bytes.data()), bytes.size()})); });
    double crc_native = time([&] { d = native_crc(bytes.data(), int(bytes.size())); });
    CHECK_EQ(a, b);
    CHECK_EQ(c, d);
    std::printf("     fib(30) %.1f ms vs %.1f native, crc of 1 MiB %.1f ms vs %.1f native\n", translated, native, crc_translated, crc_native);
}
