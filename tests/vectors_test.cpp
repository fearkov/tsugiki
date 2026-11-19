// fp and vectorized loops: translated arm64 against the same c built natively
#include <cmath>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "guest.hpp"
#include "test.hpp"

extern "C" {
extern const tsugiki_entry tsugiki_entries_vectors[];
extern const uint64_t tsugiki_entry_count_vectors;
extern uint64_t tsugiki_base_vectors;
extern const tsugiki_op tsugiki_ops_vectors[];
extern const uint64_t tsugiki_op_count_vectors;

double native_scalar_math(double, double, float, float);
int64_t native_conversions(double, float, int64_t, uint32_t);
void native_saxpy(float*, const float*, float, int);
double native_dot(const double*, const double*, int);
uint32_t native_byte_sum(const uint8_t*, int);
void native_widen_narrow(const int16_t*, const int16_t*, int32_t*, int8_t*, int);
void native_clamp_bytes(const int32_t*, uint8_t*, int);
uint32_t native_max_min(const uint16_t*, const int32_t*, int);
uint32_t native_abs_diff(const uint8_t*, const uint8_t*, int);
void native_select_bits(const uint32_t*, const uint32_t*, uint32_t*, int);
void native_rgb_planes(const uint8_t*, uint8_t*, uint8_t*, uint8_t*, int);
void native_interleave(const uint16_t*, const uint16_t*, uint16_t*, int);
void native_float_ints(const float*, int32_t*, float*, const int32_t*, int);
void native_mix_precision(const double*, float*, double*, int);
void native_reverse_words(const uint32_t*, uint32_t*, int);
float native_matrix(const float*, const float*, float*);
int native_compare_floats(const float*, const float*, int);
uint64_t native_lookup(const uint8_t*, uint8_t*, int);
}

namespace {

Guest& vectors() {
    static Guest* g = new Guest(std::string(SAMPLE_DIR) + "/libvectors.so", tsugiki_entries_vectors, tsugiki_entry_count_vectors, &tsugiki_base_vectors, tsugiki_ops_vectors,
                                tsugiki_op_count_vectors);
    return *g;
}

uint64_t p(const void* v) { return reinterpret_cast<uint64_t>(v); }
uint64_t u(int64_t v) { return uint64_t(v); }

// same value, nans count as equal
bool same(double a, double b) {
    return (std::isnan(a) && std::isnan(b)) || std::memcmp(&a, &b, 8) == 0;
}

bool same(float a, float b) {
    return (std::isnan(a) && std::isnan(b)) || std::memcmp(&a, &b, 4) == 0;
}

template <typename T>
bool same(const std::vector<T>& a, const std::vector<T>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if constexpr (std::is_floating_point_v<T>) {
            if (!same(a[i], b[i])) return false;
        } else if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST(translated_scalar_fp) {
    Guest& g = vectors();
    std::mt19937_64 random(5);
    std::uniform_real_distribution<double> wide(-1e6, 1e6);
    for (int i = 0; i < 3000; ++i) {
        double a = wide(random), b = wide(random) / 7;
        float c = float(wide(random) / 3), d = float(wide(random) / 11);
        if (i % 50 == 0) a = 0.5, b = -2.5, c = 1.5f, d = -0.5f;
        g.call("scalar_math", {}, {bits_of(a), bits_of(b), bits_of(c), bits_of(d)});
        if (!same(g.d0(), native_scalar_math(a, b, c, d))) {
            throw test::Failure{"scalar_math(" + std::to_string(a) + ", " + std::to_string(b) + ", " + std::to_string(c) + ", " + std::to_string(d) + ") = " +
                                std::to_string(g.d0()) + " vs " + std::to_string(native_scalar_math(a, b, c, d))};
        }
        int64_t n = int64_t(random()) >> 12;
        uint32_t w = uint32_t(random());
        double big = wide(random) * 1e9;
        CHECK_EQ(int64_t(g.call("conversions", {u(n), w}, {bits_of(big), bits_of(c)})), native_conversions(big, c, n, w));
    }
}

TEST(translated_vector_loops) {
    Guest& g = vectors();
    std::mt19937 random(9);
    for (int round = 0; round < 40; ++round) {
        int n = int(random() % 300) + 1;
        std::uniform_real_distribution<float> fl(-100, 100);

        std::vector<float> x(n), y(n), y2;
        for (auto& v : x) v = fl(random);
        for (auto& v : y) v = fl(random);
        y2 = y;
        float a = fl(random);
        g.call("saxpy", {p(y.data()), p(x.data()), u(n)}, {bits_of(a)});
        native_saxpy(y2.data(), x.data(), a, n);
        CHECK(same(y, y2));

        std::vector<double> da(n), db(n);
        for (auto& v : da) v = fl(random);
        for (auto& v : db) v = fl(random);
        g.call("dot", {p(da.data()), p(db.data()), u(n)});
        CHECK(same(g.d0(), native_dot(da.data(), db.data(), n)));

        std::vector<uint8_t> bytes(n), other(n);
        for (auto& v : bytes) v = uint8_t(random());
        for (auto& v : other) v = uint8_t(random());
        CHECK_EQ(uint32_t(g.call("byte_sum", {p(bytes.data()), u(n)})), native_byte_sum(bytes.data(), n));
        CHECK_EQ(uint32_t(g.call("abs_diff", {p(bytes.data()), p(other.data()), u(n)})), native_abs_diff(bytes.data(), other.data(), n));

        std::vector<int16_t> sa(n), sb(n);
        for (auto& v : sa) v = int16_t(random());
        for (auto& v : sb) v = int16_t(random());
        std::vector<int32_t> wide1(n), wide2(n);
        std::vector<int8_t> narrow1(n), narrow2(n);
        g.call("widen_narrow", {p(sa.data()), p(sb.data()), p(wide1.data()), p(narrow1.data()), u(n)});
        native_widen_narrow(sa.data(), sb.data(), wide2.data(), narrow2.data(), n);
        CHECK(wide1 == wide2 && narrow1 == narrow2);

        std::vector<int32_t> ints(n);
        for (auto& v : ints) v = int32_t(random() % 700) - 200;
        std::vector<uint8_t> c1(n), c2(n);
        g.call("clamp_bytes", {p(ints.data()), p(c1.data()), u(n)});
        native_clamp_bytes(ints.data(), c2.data(), n);
        CHECK(c1 == c2);

        std::vector<uint16_t> us(n);
        for (auto& v : us) v = uint16_t(random());
        for (auto& v : ints) v = int32_t(random());
        CHECK_EQ(uint32_t(g.call("max_min", {p(us.data()), p(ints.data()), u(n)})), native_max_min(us.data(), ints.data(), n));

        std::vector<uint32_t> wa(n), wb(n), o1(n), o2(n);
        for (auto& v : wa) v = uint32_t(random());
        for (auto& v : wb) v = uint32_t(random());
        g.call("select_bits", {p(wa.data()), p(wb.data()), p(o1.data()), u(n)});
        native_select_bits(wa.data(), wb.data(), o2.data(), n);
        CHECK(o1 == o2);
        g.call("reverse_words", {p(wa.data()), p(o1.data()), u(n)});
        native_reverse_words(wa.data(), o2.data(), n);
        CHECK(o1 == o2);

        std::vector<uint8_t> rgb(3 * n), r1(n), g1(n), b1(n), r2(n), g2(n), b2(n);
        for (auto& v : rgb) v = uint8_t(random());
        g.call("rgb_planes", {p(rgb.data()), p(r1.data()), p(g1.data()), p(b1.data()), u(n)});
        native_rgb_planes(rgb.data(), r2.data(), g2.data(), b2.data(), n);
        CHECK(r1 == r2 && g1 == g2 && b1 == b2);

        std::vector<uint16_t> ia(n), ib(n), io1(2 * n), io2(2 * n);
        for (auto& v : ia) v = uint16_t(random());
        for (auto& v : ib) v = uint16_t(random());
        g.call("interleave", {p(ia.data()), p(ib.data()), p(io1.data()), u(n)});
        native_interleave(ia.data(), ib.data(), io2.data(), n);
        CHECK(io1 == io2);

        std::vector<float> fs(n), back1(n), back2(n);
        for (auto& v : fs) v = fl(random) * 1000;
        std::vector<int32_t> fi1(n), fi2(n);
        g.call("float_ints", {p(fs.data()), p(fi1.data()), p(back1.data()), p(ints.data()), u(n)});
        native_float_ints(fs.data(), fi2.data(), back2.data(), ints.data(), n);
        CHECK(fi1 == fi2 && same(back1, back2));

        std::vector<double> dd(n), e1(n), e2(n);
        std::vector<float> f1(n), f2(n);
        for (auto& v : dd) v = double(fl(random)) * 1.0000001;
        g.call("mix_precision", {p(dd.data()), p(f1.data()), p(e1.data()), u(n)});
        native_mix_precision(dd.data(), f2.data(), e2.data(), n);
        CHECK(same(f1, f2) && same(e1, e2));

        std::vector<float> fa(n), fb(n);
        for (int i = 0; i < n; ++i) fa[i] = float(int(random() % 10)), fb[i] = float(int(random() % 10));
        CHECK_EQ(int(g.call("compare_floats", {p(fa.data()), p(fb.data()), u(n)})), native_compare_floats(fa.data(), fb.data(), n));

        std::vector<uint8_t> l1(n), l2(n);
        CHECK_EQ(g.call("lookup", {p(bytes.data()), p(l1.data()), u(n)}), native_lookup(bytes.data(), l2.data(), n));
        CHECK(l1 == l2);
    }
    std::vector<float> m(16), v(4), out1(4), out2(4);
    for (int i = 0; i < 16; ++i) m[i] = float(i) * 0.25f - 1;
    for (int i = 0; i < 4; ++i) v[i] = float(i) + 0.5f;
    g.call("matrix", {p(m.data()), p(v.data()), p(out1.data())});
    float r = g.s0();
    CHECK(same(r, native_matrix(m.data(), v.data(), out2.data())));
    CHECK(same(out1, out2));
}

// fp words go thru the lib's own lil functions here
TEST(interpreted_vectors) {
    Interpreted on;
    translated_scalar_fp();
    translated_vector_loops();
}

// no lib functions for fp words, the interpreter does scalar fp itself like it does for jit code
TEST(interpreted_scalar_fp_by_hand) {
    Interpreted on(true);
    translated_scalar_fp();
}
