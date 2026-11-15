// neon intrinsics translated from arm64 against references written here
#include <cmath>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "guest.hpp"
#include "test.hpp"

extern "C" {
extern const tsugiki_entry tsugiki_entries_neon[];
extern const uint64_t tsugiki_entry_count_neon;
extern uint64_t tsugiki_base_neon;
}

namespace {

Guest& neon() {
    static Guest* g = new Guest(std::string(SAMPLE_DIR) + "/libneon.so", tsugiki_entries_neon, tsugiki_entry_count_neon, &tsugiki_base_neon);
    return *g;
}

uint64_t p(const void* v) { return reinterpret_cast<uint64_t>(v); }

struct Buffers {
    alignas(16) uint8_t a[64], b[64], c[64], out[64];
};

template <typename T>
T get(const uint8_t* p, int i) {
    T v;
    std::memcpy(&v, p + i * sizeof(T), sizeof(T));
    return v;
}

template <typename T>
void put(uint8_t* p, int i, T v) {
    std::memcpy(p + i * sizeof(T), &v, sizeof(T));
}

template <typename T>
T clamp(__int128 v) {
    __int128 hi = (__int128)std::numeric_limits<T>::max(), lo = (__int128)std::numeric_limits<T>::min();
    return T(v > hi ? hi : v < lo ? lo : v);
}

// arm's register shift: signed low byte, negative goes right
template <typename T>
__int128 shift(T x, int8_t by, bool round) {
    const int bits = sizeof(T) * 8;
    __int128 v = (__int128)x;
    if (by >= 0) {
        return by >= bits ? (v == 0 ? 0 : v > 0 ? ((__int128)1 << 100) : -((__int128)1 << 100)) : v << by;
    }
    int r = -by;
    if (round) {
        return r > bits ? 0 : (v + ((__int128)1 << (r - 1))) >> r;
    }
    return r >= bits ? (v < 0 ? -1 : 0) : v >> r;
}

template <typename F>
F round_to(F f, int mode) {
    switch (mode) {
    case 0: return std::nearbyint(f);
    case 1: return std::round(f);
    default: return std::floor(f);
    }
}

// fp to int, saturating, nan 0
template <typename I, typename F>
I to_int(F f) {
    if (std::isnan(f)) return 0;
    long double v = f;
    if (v >= (long double)std::numeric_limits<I>::max()) return std::numeric_limits<I>::max();
    if (v <= (long double)std::numeric_limits<I>::min()) return std::numeric_limits<I>::min();
    return I(v);
}

bool same_float(float a, float b) {
    return (std::isnan(a) && std::isnan(b)) || std::memcmp(&a, &b, 4) == 0;
}

using Reference = std::function<void(Buffers&)>;

struct Case {
    const char* name;
    Reference reference;
    int bytes = 16;
    bool floats = false;
};

float max_nan(float a, float b) {
    if (std::isnan(a) || std::isnan(b)) return NAN;
    if (a == 0 && b == 0) return std::signbit(a) ? b : a;
    return a > b ? a : b;
}

template <typename F>
F maxnum(F a, F b, bool max) {
    if (std::isnan(a) && !std::isnan(b)) return b;
    if (std::isnan(b) && !std::isnan(a)) return a;
    if (std::isnan(a)) return a;
    if (a == 0 && b == 0) return (std::signbit(a) == max) ? b : a;
    return max ? (a > b ? a : b) : (a < b ? a : b);
}

std::vector<Case> cases() {
    std::vector<Case> all;
    auto add = [&](const char* name, Reference r, int bytes = 16, bool floats = false) { all.push_back({name, std::move(r), bytes, floats}); };

    add("sqadd_s16", [](Buffers& x) { for (int i = 0; i < 8; ++i) put<int16_t>(x.out, i, clamp<int16_t>(get<int16_t>(x.a, i) + get<int16_t>(x.b, i))); });
    add("uqsub_u8", [](Buffers& x) { for (int i = 0; i < 16; ++i) put<uint8_t>(x.out, i, clamp<uint8_t>(int(x.a[i]) - x.b[i])); });
    add("uhadd_u8", [](Buffers& x) { for (int i = 0; i < 16; ++i) x.out[i] = uint8_t((x.a[i] + x.b[i]) >> 1); });
    add("srhadd_s16", [](Buffers& x) { for (int i = 0; i < 8; ++i) put<int16_t>(x.out, i, int16_t((get<int16_t>(x.a, i) + get<int16_t>(x.b, i) + 1) >> 1)); });
    add("sabd_s32", [](Buffers& x) { for (int i = 0; i < 4; ++i) { int64_t d = (int64_t)get<int32_t>(x.a, i) - get<int32_t>(x.b, i); put<int32_t>(x.out, i, int32_t(d < 0 ? -d : d)); } });
    add("uaba_u8", [](Buffers& x) { for (int i = 0; i < 16; ++i) x.out[i] = uint8_t(x.c[i] + (x.a[i] > x.b[i] ? x.a[i] - x.b[i] : x.b[i] - x.a[i])); });
    add("urshr_u16", [](Buffers& x) { for (int i = 0; i < 8; ++i) put<uint16_t>(x.out, i, uint16_t((get<uint16_t>(x.a, i) + 4) >> 3)); });
    add("srsra_s32", [](Buffers& x) { for (int i = 0; i < 4; ++i) put<int32_t>(x.out, i, int32_t(uint32_t(get<int32_t>(x.c, i)) + uint32_t(((int64_t)get<int32_t>(x.a, i) + 16) >> 5))); });
    add("sshl_s32", [](Buffers& x) { for (int i = 0; i < 4; ++i) put<int32_t>(x.out, i, int32_t(shift<int32_t>(get<int32_t>(x.a, i), int8_t(get<int32_t>(x.b, i)), false))); });
    add("urshl_u16", [](Buffers& x) { for (int i = 0; i < 8; ++i) put<uint16_t>(x.out, i, uint16_t(shift<uint16_t>(get<uint16_t>(x.a, i), int8_t(get<int16_t>(x.b, i)), true))); });
    add("sqshl_s8", [](Buffers& x) { for (int i = 0; i < 16; ++i) put<int8_t>(x.out, i, clamp<int8_t>(shift<int8_t>(int8_t(x.a[i]), int8_t(x.b[i]), false))); });
    add("sqrshl_s16", [](Buffers& x) { for (int i = 0; i < 8; ++i) put<int16_t>(x.out, i, clamp<int16_t>(shift<int16_t>(get<int16_t>(x.a, i), int8_t(get<int16_t>(x.b, i)), true))); });
    add("sqxtn_s32", [](Buffers& x) { for (int i = 0; i < 4; ++i) put<int16_t>(x.out, i, clamp<int16_t>(get<int32_t>(x.a, i))); }, 8);
    add("sqxtun_s32", [](Buffers& x) { for (int i = 0; i < 4; ++i) put<uint16_t>(x.out, i, clamp<uint16_t>(get<int32_t>(x.a, i))); }, 8);
    add("uqxtn2_u32", [](Buffers& x) {
        for (int i = 0; i < 4; ++i) put<uint16_t>(x.out, i, get<uint16_t>(x.c, i));
        for (int i = 0; i < 4; ++i) put<uint16_t>(x.out, 4 + i, clamp<uint16_t>(get<uint32_t>(x.a, i)));
    });
    add("xtn_u64", [](Buffers& x) { for (int i = 0; i < 2; ++i) put<uint32_t>(x.out, i, uint32_t(get<uint64_t>(x.a, i))); }, 8);
    add("sqshrn_s32", [](Buffers& x) { for (int i = 0; i < 4; ++i) put<int16_t>(x.out, i, clamp<int16_t>(get<int32_t>(x.a, i) >> 4)); }, 8);
    add("sqrshrun_s16", [](Buffers& x) { for (int i = 0; i < 8; ++i) put<uint8_t>(x.out, i, clamp<uint8_t>((get<int16_t>(x.a, i) + 4) >> 3)); }, 8);
    add("rshrn_u32", [](Buffers& x) { for (int i = 0; i < 4; ++i) put<uint16_t>(x.out, i, uint16_t(((uint64_t)get<uint32_t>(x.a, i) + 64) >> 7)); }, 8);
    add("sqdmulh_s16", [](Buffers& x) { for (int i = 0; i < 8; ++i) put<int16_t>(x.out, i, clamp<int16_t>(((__int128)get<int16_t>(x.a, i) * get<int16_t>(x.b, i) * 2) >> 16)); });
    add("sqrdmulh_s32", [](Buffers& x) {
        for (int i = 0; i < 4; ++i) put<int32_t>(x.out, i, clamp<int32_t>(((__int128)get<int32_t>(x.a, i) * get<int32_t>(x.b, i) * 2 + ((__int128)1 << 31)) >> 32));
    });
    add("sqdmull_s16", [](Buffers& x) { for (int i = 0; i < 4; ++i) put<int32_t>(x.out, i, clamp<int32_t>((__int128)get<int16_t>(x.a, i) * get<int16_t>(x.b, i) * 2)); });
    add("umull_u8", [](Buffers& x) { for (int i = 0; i < 8; ++i) put<uint16_t>(x.out, i, uint16_t(x.a[i] * x.b[i])); });
    add("smlal2_s16", [](Buffers& x) { for (int i = 0; i < 4; ++i) put<int32_t>(x.out, i, int32_t(uint32_t(get<int32_t>(x.c, i)) + uint32_t(get<int16_t>(x.a, 4 + i) * get<int16_t>(x.b, 4 + i)))); });
    add("umlsl_u32", [](Buffers& x) { for (int i = 0; i < 2; ++i) put<uint64_t>(x.out, i, get<uint64_t>(x.c, i) - (uint64_t)get<uint32_t>(x.a, i) * get<uint32_t>(x.b, i)); });
    add("addp_u16", [](Buffers& x) {
        for (int i = 0; i < 4; ++i) put<uint16_t>(x.out, i, uint16_t(get<uint16_t>(x.a, 2 * i) + get<uint16_t>(x.a, 2 * i + 1)));
        for (int i = 0; i < 4; ++i) put<uint16_t>(x.out, 4 + i, uint16_t(get<uint16_t>(x.b, 2 * i) + get<uint16_t>(x.b, 2 * i + 1)));
    });
    add("smaxp_s8", [](Buffers& x) {
        for (int i = 0; i < 8; ++i) put<int8_t>(x.out, i, std::max(int8_t(x.a[2 * i]), int8_t(x.a[2 * i + 1])));
        for (int i = 0; i < 8; ++i) put<int8_t>(x.out, 8 + i, std::max(int8_t(x.b[2 * i]), int8_t(x.b[2 * i + 1])));
    });
    add("uminp_u32", [](Buffers& x) {
        for (int i = 0; i < 2; ++i) put<uint32_t>(x.out, i, std::min(get<uint32_t>(x.a, 2 * i), get<uint32_t>(x.a, 2 * i + 1)));
        for (int i = 0; i < 2; ++i) put<uint32_t>(x.out, 2 + i, std::min(get<uint32_t>(x.b, 2 * i), get<uint32_t>(x.b, 2 * i + 1)));
    });
    add("zip1_u8", [](Buffers& x) { for (int i = 0; i < 8; ++i) x.out[2 * i] = x.a[i], x.out[2 * i + 1] = x.b[i]; });
    add("zip2_u16", [](Buffers& x) { for (int i = 0; i < 4; ++i) put<uint16_t>(x.out, 2 * i, get<uint16_t>(x.a, 4 + i)), put<uint16_t>(x.out, 2 * i + 1, get<uint16_t>(x.b, 4 + i)); });
    add("uzp2_u16", [](Buffers& x) { for (int i = 0; i < 4; ++i) put<uint16_t>(x.out, i, get<uint16_t>(x.a, 2 * i + 1)), put<uint16_t>(x.out, 4 + i, get<uint16_t>(x.b, 2 * i + 1)); });
    add("trn1_s32", [](Buffers& x) { for (int i = 0; i < 2; ++i) put<int32_t>(x.out, 2 * i, get<int32_t>(x.a, 2 * i)), put<int32_t>(x.out, 2 * i + 1, get<int32_t>(x.b, 2 * i)); });
    add("trn2_u8", [](Buffers& x) { for (int i = 0; i < 8; ++i) x.out[2 * i] = x.a[2 * i + 1], x.out[2 * i + 1] = x.b[2 * i + 1]; });
    add("ext_u8", [](Buffers& x) { for (int i = 0; i < 16; ++i) x.out[i] = i + 5 < 16 ? x.a[i + 5] : x.b[i + 5 - 16]; });
    add("tbl2_u8", [](Buffers& x) {
        for (int i = 0; i < 16; ++i) { int k = x.c[i] & 0x3f; x.out[i] = k < 16 ? x.a[k] : k < 32 ? x.b[k - 16] : 0; }
    });
    add("tbx1_u8", [](Buffers& x) { for (int i = 0; i < 16; ++i) { int k = x.c[i] & 0x1f; x.out[i] = k < 16 ? x.a[k] : x.b[i]; } });
    add("cnt_u8", [](Buffers& x) { for (int i = 0; i < 16; ++i) x.out[i] = uint8_t(__builtin_popcount(x.a[i])); });
    add("clz_u32", [](Buffers& x) { for (int i = 0; i < 4; ++i) { uint32_t v = get<uint32_t>(x.a, i) >> 5; put<uint32_t>(x.out, i, v ? __builtin_clz(v) : 32); } });
    add("cls_s16", [](Buffers& x) {
        for (int i = 0; i < 8; ++i) {
            int16_t v = get<int16_t>(x.a, i);
            int n = 0;
            for (int b = 14; b >= 0 && ((v >> b) & 1) == ((v >> 15) & 1); --b) ++n;
            put<int16_t>(x.out, i, int16_t(n));
        }
    });
    add("rbit_u8", [](Buffers& x) { for (int i = 0; i < 16; ++i) { uint8_t v = x.a[i], r = 0; for (int b = 0; b < 8; ++b) r |= ((v >> b) & 1) << (7 - b); x.out[i] = r; } });
    add("uaddlv_u8", [](Buffers& x) { uint16_t s = 0; for (int i = 0; i < 16; ++i) s += x.a[i]; put<uint16_t>(x.out, 0, s); }, 2);
    add("smaxv_s16", [](Buffers& x) { int16_t m = get<int16_t>(x.a, 0); for (int i = 1; i < 8; ++i) m = std::max(m, get<int16_t>(x.a, i)); put<int16_t>(x.out, 0, m); }, 2);
    add("uminv_u32", [](Buffers& x) { uint32_t m = get<uint32_t>(x.a, 0); for (int i = 1; i < 4; ++i) m = std::min(m, get<uint32_t>(x.a, i)); put<uint32_t>(x.out, 0, m); }, 4);
    add("fcvtns_s32", [](Buffers& x) { for (int i = 0; i < 4; ++i) put<int32_t>(x.out, i, to_int<int32_t>(round_to(get<float>(x.a, i), 0))); }, 16, true);
    add("fcvtas_s32", [](Buffers& x) { for (int i = 0; i < 4; ++i) put<int32_t>(x.out, i, to_int<int32_t>(round_to(get<float>(x.a, i), 1))); }, 16, true);
    add("fcvtmu_u32", [](Buffers& x) { for (int i = 0; i < 4; ++i) put<uint32_t>(x.out, i, to_int<uint32_t>(round_to(get<float>(x.a, i), 2))); }, 16, true);
    add("scvtf_fixed", [](Buffers& x) { for (int i = 0; i < 4; ++i) put<float>(x.out, i, float(get<int32_t>(x.a, i)) / 256.0f); });
    add("fcvtzs_fixed", [](Buffers& x) { for (int i = 0; i < 4; ++i) put<int32_t>(x.out, i, to_int<int32_t>(std::trunc(get<float>(x.a, i) * 16.0f))); }, 16, true);
    add("sli_u32", [](Buffers& x) { for (int i = 0; i < 4; ++i) put<uint32_t>(x.out, i, (get<uint32_t>(x.a, i) << 9) | (get<uint32_t>(x.c, i) & 0x1ff)); });
    add("sri_u16", [](Buffers& x) { for (int i = 0; i < 8; ++i) put<uint16_t>(x.out, i, uint16_t((get<uint16_t>(x.a, i) >> 5) | (get<uint16_t>(x.c, i) & 0xf800))); });
    add("sqshlu_s16", [](Buffers& x) { for (int i = 0; i < 8; ++i) put<uint16_t>(x.out, i, clamp<uint16_t>((__int128)get<int16_t>(x.a, i) << 3)); });
    add("fmla_lane_f32", [](Buffers& x) { for (int i = 0; i < 4; ++i) put<float>(x.out, i, std::fma(get<float>(x.a, i), get<float>(x.b, 2), get<float>(x.c, i))); }, 16, true);
    add("mul_lane_s16", [](Buffers& x) { for (int i = 0; i < 8; ++i) put<int16_t>(x.out, i, int16_t(get<int16_t>(x.a, i) * get<int16_t>(x.b, 5))); });
    add("fmax_f32", [](Buffers& x) { for (int i = 0; i < 4; ++i) put<float>(x.out, i, max_nan(get<float>(x.a, i), get<float>(x.b, i))); }, 16, true);
    add("fmaxnm_f32", [](Buffers& x) { for (int i = 0; i < 4; ++i) put<float>(x.out, i, maxnum(get<float>(x.a, i), get<float>(x.b, i), true)); }, 16, true);
    add("fminnm_f64", [](Buffers& x) { for (int i = 0; i < 2; ++i) put<double>(x.out, i, maxnum(get<double>(x.a, i), get<double>(x.b, i), false)); }, 16, true);
    add("faddp_f32", [](Buffers& x) {
        for (int i = 0; i < 2; ++i) put<float>(x.out, i, get<float>(x.a, 2 * i) + get<float>(x.a, 2 * i + 1));
        for (int i = 0; i < 2; ++i) put<float>(x.out, 2 + i, get<float>(x.b, 2 * i) + get<float>(x.b, 2 * i + 1));
    }, 16, true);
    add("fmaxv_f32", [](Buffers& x) {
        put<float>(x.out, 0, max_nan(max_nan(get<float>(x.a, 0), get<float>(x.a, 1)), max_nan(get<float>(x.a, 2), get<float>(x.a, 3))));
    }, 4, true);
    add("frinta_f32", [](Buffers& x) { for (int i = 0; i < 4; ++i) put<float>(x.out, i, std::round(get<float>(x.a, i))); }, 16, true);
    add("fdiv_sqrt_f64", [](Buffers& x) { for (int i = 0; i < 2; ++i) put<double>(x.out, i, std::sqrt(get<double>(x.a, i) / get<double>(x.b, i))); }, 16, true);
    add("facge_f32", [](Buffers& x) { for (int i = 0; i < 4; ++i) put<uint32_t>(x.out, i, std::fabs(get<float>(x.a, i)) >= std::fabs(get<float>(x.b, i)) ? ~0u : 0); }, 16, true);
    add("bsl_u32", [](Buffers& x) { for (int i = 0; i < 4; ++i) { uint32_t m = get<uint32_t>(x.c, i); put<uint32_t>(x.out, i, (get<uint32_t>(x.a, i) & m) | (get<uint32_t>(x.b, i) & ~m)); } });
    add("lanes", [](Buffers& x) {
        uint16_t v[8];
        for (int i = 0; i < 8; ++i) v[i] = get<uint16_t>(x.a, i);
        v[6] = get<uint16_t>(x.b, 3);
        for (int i = 0; i < 8; ++i) put<uint16_t>(x.out, i, uint16_t(v[i] + v[1]));
    });
    add("ld4_st4", [](Buffers& x) {
        for (int i = 0; i < 16; ++i) {
            x.out[4 * i] = x.a[4 * i + 3], x.out[4 * i + 1] = x.a[4 * i + 1], x.out[4 * i + 2] = x.a[4 * i], x.out[4 * i + 3] = x.a[4 * i + 2];
        }
    }, 64);
    add("ld1r_ld2_lane", [](Buffers& x) {
        uint16_t first[8], second[8];
        for (int i = 0; i < 8; ++i) first[i] = get<uint16_t>(x.a, i), second[i] = get<uint16_t>(x.c, i);
        first[3] = get<uint16_t>(x.b, 0), second[3] = get<uint16_t>(x.b, 1);
        uint32_t r = get<uint32_t>(x.b, 0);
        for (int i = 0; i < 4; ++i) {
            uint32_t e = uint32_t(first[2 * i] ^ second[2 * i]) | (uint32_t(first[2 * i + 1] ^ second[2 * i + 1]) << 16);
            put<uint32_t>(x.out, i, r + e);
        }
    });
    return all;
}

void fill(std::mt19937& random, uint8_t* bytes, int n, bool floats) {
    for (int i = 0; i < n; ++i) bytes[i] = uint8_t(random());
    if (!floats) {
        return;
    }
    // sane floats, with specials now and then
    static const float specials[] = {0.0f, -0.0f, INFINITY, -INFINITY, NAN, 1.5f, -2.5f, 3e9f, -3e9f, 0.5f, 2.5f, 1e-40f};
    std::uniform_real_distribution<float> range(-1e5f, 1e5f);
    for (int i = 0; i < n / 4; ++i) {
        float f = random() % 5 == 0 ? specials[random() % 12] : range(random);
        std::memcpy(bytes + 4 * i, &f, 4);
    }
}

}  // namespace

TEST(translated_neon_intrinsics) {
    Guest& g = neon();
    std::mt19937 random(31);
    for (const Case& c : cases()) {
        bool doubles = std::string(c.name).find("f64") != std::string::npos;
        for (int round = 0; round < 300; ++round) {
            Buffers mine{}, theirs{};
            fill(random, mine.a, 64, c.floats && !doubles);
            fill(random, mine.b, 64, c.floats && !doubles);
            fill(random, mine.c, 64, c.floats && !doubles);
            if (doubles) {
                std::uniform_real_distribution<double> range(-1e5, 1e5);
                for (int i = 0; i < 8; ++i) {
                    double da = range(random), db = range(random);
                    if (round % 7 == 0 && i == 0) da = NAN;
                    std::memcpy(mine.a + 8 * i, &da, 8), std::memcpy(mine.b + 8 * i, &db, 8);
                }
            }
            std::memcpy(&theirs, &mine, sizeof mine);
            g.call(std::string("t_") + c.name, {p(mine.a), p(mine.b), p(mine.c), p(mine.out)});
            c.reference(theirs);
            bool ok = true;
            for (int i = 0; i < c.bytes && ok; ++i) {
                ok = mine.out[i] == theirs.out[i];
            }
            if (!ok && c.floats) {
                // nans only have to be nans
                ok = true;
                int width = doubles ? 8 : 4;
                for (int i = 0; i < c.bytes / width && ok; ++i) {
                    if (doubles) {
                        double x = get<double>(mine.out, i), y = get<double>(theirs.out, i);
                        ok = (std::isnan(x) && std::isnan(y)) || std::memcmp(&x, &y, 8) == 0;
                    } else {
                        ok = same_float(get<float>(mine.out, i), get<float>(theirs.out, i));
                    }
                }
            }
            if (!ok) {
                std::string hex;
                char text[8];
                for (int i = 0; i < c.bytes && i < 16; ++i) {
                    std::snprintf(text, sizeof text, "%02x", mine.out[i]);
                    hex += text;
                }
                hex += " vs ";
                for (int i = 0; i < c.bytes && i < 16; ++i) {
                    std::snprintf(text, sizeof text, "%02x", theirs.out[i]);
                    hex += text;
                }
                throw test::Failure{std::string(c.name) + ": " + hex};
            }
        }
    }
}

TEST(translated_neon_estimates) {
    Guest& g = neon();
    // known answers: 1/1 and 1/sqrt(1) estimate to 0.998046875
    alignas(16) float in[4] = {1.0f, 2.0f, 4.0f, 0.5f};
    alignas(16) uint32_t out[8];
    g.call("estimates", {p(in), p(out)});
    CHECK_EQ(out[0], 0x3f7f8000u);
    CHECK_EQ(out[1], 0x3eff8000u);
    CHECK_EQ(out[4], 0x3f7f8000u);
    CHECK_EQ(out[6], 0x3eff8000u);
    // two newton steps get within a few ulps
    std::mt19937 random(3);
    std::uniform_real_distribution<float> range(0.001f, 1e6f);
    for (int round = 0; round < 500; ++round) {
        alignas(16) float x[16], r[16];
        for (int i = 0; i < 4; ++i) x[i] = range(random);
        g.call("t_frecpe_steps", {p(x), 0, 0, p(r)});
        for (int i = 0; i < 4; ++i) CHECK(std::fabs(r[i] * x[i] - 1.0f) < 1e-5f);
        g.call("t_frsqrte_steps", {p(x), 0, 0, p(r)});
        for (int i = 0; i < 4; ++i) CHECK(std::fabs(r[i] * r[i] * x[i] - 1.0f) < 1e-4f);
    }
}

TEST(translated_aes_and_sha256) {
    Guest& g = neon();
    // fips-197 c.1
    const uint8_t key[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
    const uint8_t plain[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    const uint8_t cipher[16] = {0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30, 0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a};
    // key expansion here, it isn't what's tested
    static const uint8_t sbox[256] = {
        0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf,
        0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15, 0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a,
        0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed,
        0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf, 0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
        0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d,
        0x64, 0x5d, 0x19, 0x73, 0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c,
        0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08, 0xba, 0x78, 0x25, 0x2e,
        0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a, 0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
        0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf, 0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f,
        0xb0, 0x54, 0xbb, 0x16};
    alignas(16) uint8_t keys[176];
    std::memcpy(keys, key, 16);
    uint8_t rcon = 1;
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4] = {keys[i - 4], keys[i - 3], keys[i - 2], keys[i - 1]};
        if (i % 16 == 0) {
            uint8_t first = t[0];
            t[0] = uint8_t(sbox[t[1]] ^ rcon), t[1] = sbox[t[2]], t[2] = sbox[t[3]], t[3] = sbox[first];
            rcon = uint8_t((rcon << 1) ^ (rcon & 0x80 ? 0x1b : 0));
        }
        for (int k = 0; k < 4; ++k) keys[i + k] = uint8_t(keys[i - 16 + k] ^ t[k]);
    }
    alignas(16) uint8_t out[16], back[16];
    g.call("aes_encrypt", {p(keys), p(plain), p(out)});
    CHECK(std::memcmp(out, cipher, 16) == 0);
    g.call("aes_decrypt", {p(keys), p(out), p(back)});
    CHECK(std::memcmp(back, plain, 16) == 0);

    // sha-256("abc")
    alignas(16) uint32_t state[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    alignas(16) uint8_t block[64] = {'a', 'b', 'c', 0x80};
    block[63] = 24;
    g.call("sha256_block", {p(state), p(block)});
    const uint32_t digest[8] = {0xba7816bf, 0x8f01cfea, 0x414140de, 0x5dae2223, 0xb00361a3, 0x96177a9c, 0xb410ff61, 0xf20015ad};
    for (int i = 0; i < 8; ++i) CHECK_EQ(state[i], digest[i]);
}
