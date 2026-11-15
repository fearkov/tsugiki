// neon intrinsics, arm64 only: 16-byte inputs a, b, c, results to out
// references live in tests/neon_test.cpp

#include <arm_neon.h>
#include <stdint.h>

#define T(name) void t_##name(const uint8_t* a, const uint8_t* b, const uint8_t* c, uint8_t* out)

T(sqadd_s16) { vst1q_s16((int16_t*)out, vqaddq_s16(vld1q_s16((const int16_t*)a), vld1q_s16((const int16_t*)b))); }
T(uqsub_u8) { vst1q_u8(out, vqsubq_u8(vld1q_u8(a), vld1q_u8(b))); }
T(uhadd_u8) { vst1q_u8(out, vhaddq_u8(vld1q_u8(a), vld1q_u8(b))); }
T(srhadd_s16) { vst1q_s16((int16_t*)out, vrhaddq_s16(vld1q_s16((const int16_t*)a), vld1q_s16((const int16_t*)b))); }
T(sabd_s32) { vst1q_s32((int32_t*)out, vabdq_s32(vld1q_s32((const int32_t*)a), vld1q_s32((const int32_t*)b))); }
T(uaba_u8) { vst1q_u8(out, vabaq_u8(vld1q_u8(c), vld1q_u8(a), vld1q_u8(b))); }
T(urshr_u16) { vst1q_u16((uint16_t*)out, vrshrq_n_u16(vld1q_u16((const uint16_t*)a), 3)); }
T(srsra_s32) { vst1q_s32((int32_t*)out, vrsraq_n_s32(vld1q_s32((const int32_t*)c), vld1q_s32((const int32_t*)a), 5)); }
T(sshl_s32) { vst1q_s32((int32_t*)out, vshlq_s32(vld1q_s32((const int32_t*)a), vld1q_s32((const int32_t*)b))); }
T(urshl_u16) { vst1q_u16((uint16_t*)out, vrshlq_u16(vld1q_u16((const uint16_t*)a), vld1q_s16((const int16_t*)b))); }
T(sqshl_s8) { vst1q_s8((int8_t*)out, vqshlq_s8(vld1q_s8((const int8_t*)a), vld1q_s8((const int8_t*)b))); }
T(sqrshl_s16) { vst1q_s16((int16_t*)out, vqrshlq_s16(vld1q_s16((const int16_t*)a), vld1q_s16((const int16_t*)b))); }
T(sqxtn_s32) { vst1_s16((int16_t*)out, vqmovn_s32(vld1q_s32((const int32_t*)a))); }
T(sqxtun_s32) { vst1_u16((uint16_t*)out, vqmovun_s32(vld1q_s32((const int32_t*)a))); }
T(uqxtn2_u32) { vst1q_u16((uint16_t*)out, vqmovn_high_u32(vld1_u16((const uint16_t*)c), vld1q_u32((const uint32_t*)a))); }
T(xtn_u64) { vst1_u32((uint32_t*)out, vmovn_u64(vld1q_u64((const uint64_t*)a))); }
T(sqshrn_s32) { vst1_s16((int16_t*)out, vqshrn_n_s32(vld1q_s32((const int32_t*)a), 4)); }
T(sqrshrun_s16) { vst1_u8(out, vqrshrun_n_s16(vld1q_s16((const int16_t*)a), 3)); }
T(rshrn_u32) { vst1_u16((uint16_t*)out, vrshrn_n_u32(vld1q_u32((const uint32_t*)a), 7)); }
T(sqdmulh_s16) { vst1q_s16((int16_t*)out, vqdmulhq_s16(vld1q_s16((const int16_t*)a), vld1q_s16((const int16_t*)b))); }
T(sqrdmulh_s32) { vst1q_s32((int32_t*)out, vqrdmulhq_s32(vld1q_s32((const int32_t*)a), vld1q_s32((const int32_t*)b))); }
T(sqdmull_s16) { vst1q_s32((int32_t*)out, vqdmull_s16(vld1_s16((const int16_t*)a), vld1_s16((const int16_t*)b))); }
T(umull_u8) { vst1q_u16((uint16_t*)out, vmull_u8(vld1_u8(a), vld1_u8(b))); }
T(smlal2_s16) { vst1q_s32((int32_t*)out, vmlal_high_s16(vld1q_s32((const int32_t*)c), vld1q_s16((const int16_t*)a), vld1q_s16((const int16_t*)b))); }
T(umlsl_u32) { vst1q_u64((uint64_t*)out, vmlsl_u32(vld1q_u64((const uint64_t*)c), vld1_u32((const uint32_t*)a), vld1_u32((const uint32_t*)b))); }
T(addp_u16) { vst1q_u16((uint16_t*)out, vpaddq_u16(vld1q_u16((const uint16_t*)a), vld1q_u16((const uint16_t*)b))); }
T(smaxp_s8) { vst1q_s8((int8_t*)out, vpmaxq_s8(vld1q_s8((const int8_t*)a), vld1q_s8((const int8_t*)b))); }
T(uminp_u32) { vst1q_u32((uint32_t*)out, vpminq_u32(vld1q_u32((const uint32_t*)a), vld1q_u32((const uint32_t*)b))); }
T(zip1_u8) { vst1q_u8(out, vzip1q_u8(vld1q_u8(a), vld1q_u8(b))); }
T(zip2_u16) { vst1q_u16((uint16_t*)out, vzip2q_u16(vld1q_u16((const uint16_t*)a), vld1q_u16((const uint16_t*)b))); }
T(uzp2_u16) { vst1q_u16((uint16_t*)out, vuzp2q_u16(vld1q_u16((const uint16_t*)a), vld1q_u16((const uint16_t*)b))); }
T(trn1_s32) { vst1q_s32((int32_t*)out, vtrn1q_s32(vld1q_s32((const int32_t*)a), vld1q_s32((const int32_t*)b))); }
T(trn2_u8) { vst1q_u8(out, vtrn2q_u8(vld1q_u8(a), vld1q_u8(b))); }
T(ext_u8) { vst1q_u8(out, vextq_u8(vld1q_u8(a), vld1q_u8(b), 5)); }
T(tbl2_u8) {
    uint8x16x2_t t = {{vld1q_u8(a), vld1q_u8(b)}};
    vst1q_u8(out, vqtbl2q_u8(t, vandq_u8(vld1q_u8(c), vdupq_n_u8(0x3f))));
}
T(tbx1_u8) { vst1q_u8(out, vqtbx1q_u8(vld1q_u8(b), vld1q_u8(a), vandq_u8(vld1q_u8(c), vdupq_n_u8(0x1f)))); }
T(cnt_u8) { vst1q_u8(out, vcntq_u8(vld1q_u8(a))); }
T(clz_u32) { vst1q_u32((uint32_t*)out, vclzq_u32(vshrq_n_u32(vld1q_u32((const uint32_t*)a), 5))); }
T(cls_s16) { vst1q_s16((int16_t*)out, vclsq_s16(vld1q_s16((const int16_t*)a))); }
T(rbit_u8) { vst1q_u8(out, vrbitq_u8(vld1q_u8(a))); }
T(uaddlv_u8) { *(uint16_t*)out = vaddlvq_u8(vld1q_u8(a)); }
T(smaxv_s16) { *(int16_t*)out = vmaxvq_s16(vld1q_s16((const int16_t*)a)); }
T(uminv_u32) { *(uint32_t*)out = vminvq_u32(vld1q_u32((const uint32_t*)a)); }
T(fcvtns_s32) { vst1q_s32((int32_t*)out, vcvtnq_s32_f32(vld1q_f32((const float*)a))); }
T(fcvtas_s32) { vst1q_s32((int32_t*)out, vcvtaq_s32_f32(vld1q_f32((const float*)a))); }
T(fcvtmu_u32) { vst1q_u32((uint32_t*)out, vcvtmq_u32_f32(vld1q_f32((const float*)a))); }
T(scvtf_fixed) { vst1q_f32((float*)out, vcvtq_n_f32_s32(vld1q_s32((const int32_t*)a), 8)); }
T(fcvtzs_fixed) { vst1q_s32((int32_t*)out, vcvtq_n_s32_f32(vld1q_f32((const float*)a), 4)); }
T(sli_u32) { vst1q_u32((uint32_t*)out, vsliq_n_u32(vld1q_u32((const uint32_t*)c), vld1q_u32((const uint32_t*)a), 9)); }
T(sri_u16) { vst1q_u16((uint16_t*)out, vsriq_n_u16(vld1q_u16((const uint16_t*)c), vld1q_u16((const uint16_t*)a), 5)); }
T(sqshlu_s16) { vst1q_u16((uint16_t*)out, vqshluq_n_s16(vld1q_s16((const int16_t*)a), 3)); }
T(fmla_lane_f32) {
    vst1q_f32((float*)out, vfmaq_laneq_f32(vld1q_f32((const float*)c), vld1q_f32((const float*)a), vld1q_f32((const float*)b), 2));
}
T(mul_lane_s16) { vst1q_s16((int16_t*)out, vmulq_laneq_s16(vld1q_s16((const int16_t*)a), vld1q_s16((const int16_t*)b), 5)); }
T(fmax_f32) { vst1q_f32((float*)out, vmaxq_f32(vld1q_f32((const float*)a), vld1q_f32((const float*)b))); }
T(fmaxnm_f32) { vst1q_f32((float*)out, vmaxnmq_f32(vld1q_f32((const float*)a), vld1q_f32((const float*)b))); }
T(fminnm_f64) { vst1q_f64((double*)out, vminnmq_f64(vld1q_f64((const double*)a), vld1q_f64((const double*)b))); }
T(faddp_f32) { vst1q_f32((float*)out, vpaddq_f32(vld1q_f32((const float*)a), vld1q_f32((const float*)b))); }
T(fmaxv_f32) { *(float*)out = vmaxvq_f32(vld1q_f32((const float*)a)); }
T(frinta_f32) { vst1q_f32((float*)out, vrndaq_f32(vld1q_f32((const float*)a))); }
T(fdiv_sqrt_f64) { vst1q_f64((double*)out, vsqrtq_f64(vdivq_f64(vld1q_f64((const double*)a), vld1q_f64((const double*)b)))); }
T(facge_f32) { vst1q_u32((uint32_t*)out, vcageq_f32(vld1q_f32((const float*)a), vld1q_f32((const float*)b))); }
T(bsl_u32) { vst1q_u32((uint32_t*)out, vbslq_u32(vld1q_u32((const uint32_t*)c), vld1q_u32((const uint32_t*)a), vld1q_u32((const uint32_t*)b))); }
T(lanes) {
    uint16x8_t v = vld1q_u16((const uint16_t*)a);
    v = vsetq_lane_u16(vgetq_lane_u16(vld1q_u16((const uint16_t*)b), 3), v, 6);
    vst1q_u16((uint16_t*)out, vaddq_u16(v, vdupq_laneq_u16(v, 1)));
}
T(ld4_st4) {
    uint8x16x4_t v = vld4q_u8(a);
    uint8x16x4_t w = {{v.val[3], v.val[1], v.val[0], v.val[2]}};
    vst4q_u8(out, w);
}
T(ld1r_ld2_lane) {
    uint32x4_t r = vld1q_dup_u32((const uint32_t*)b);
    uint16x8x2_t two = {{vld1q_u16((const uint16_t*)a), vld1q_u16((const uint16_t*)c)}};
    two = vld2q_lane_u16((const uint16_t*)b, two, 3);
    vst1q_u32((uint32_t*)out, vaddq_u32(r, vreinterpretq_u32_u16(veorq_u16(two.val[0], two.val[1]))));
}
T(frecpe_steps) {
    float32x4_t x = vld1q_f32((const float*)a);
    float32x4_t r = vrecpeq_f32(x);
    r = vmulq_f32(r, vrecpsq_f32(x, r));
    r = vmulq_f32(r, vrecpsq_f32(x, r));
    vst1q_f32((float*)out, r);
}
T(frsqrte_steps) {
    float32x4_t x = vld1q_f32((const float*)a);
    float32x4_t r = vrsqrteq_f32(x);
    r = vmulq_f32(r, vrsqrtsq_f32(vmulq_f32(x, r), r));
    r = vmulq_f32(r, vrsqrtsq_f32(vmulq_f32(x, r), r));
    vst1q_f32((float*)out, r);
}

// raw estimates of the given values, for known answers
void estimates(const float* in, uint32_t* out) {
    float32x4_t x = vld1q_f32(in);
    vst1q_u32(out, vreinterpretq_u32_f32(vrecpeq_f32(x)));
    vst1q_u32(out + 4, vreinterpretq_u32_f32(vrsqrteq_f32(x)));
}

// aes-128, round keys already expanded
void aes_encrypt(const uint8_t* keys, const uint8_t* in, uint8_t* out) {
    uint8x16_t s = vld1q_u8(in);
    for (int r = 0; r < 9; ++r) {
        s = vaesmcq_u8(vaeseq_u8(s, vld1q_u8(keys + 16 * r)));
    }
    s = vaeseq_u8(s, vld1q_u8(keys + 16 * 9));
    s = veorq_u8(s, vld1q_u8(keys + 16 * 10));
    vst1q_u8(out, s);
}

void aes_decrypt(const uint8_t* keys, const uint8_t* in, uint8_t* out) {
    // keys are the encryption ones, inverted mix columns for the middle rounds
    uint8x16_t s = vld1q_u8(in);
    s = vaesdq_u8(s, vld1q_u8(keys + 16 * 10));
    for (int r = 9; r >= 1; --r) {
        s = vaesdq_u8(vaesimcq_u8(s), vaesimcq_u8(vld1q_u8(keys + 16 * r)));
    }
    s = veorq_u8(s, vld1q_u8(keys));
    vst1q_u8(out, s);
}

// one sha-256 block, state in and out
void sha256_block(uint32_t* state, const uint8_t* block) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
        0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
        0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
        0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
        0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32x4_t abcd = vld1q_u32(state), efgh = vld1q_u32(state + 4);
    uint32x4_t m[4];
    for (int i = 0; i < 4; ++i) m[i] = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(block + 16 * i)));
    uint32x4_t abcd0 = abcd, efgh0 = efgh;
    for (int i = 0; i < 16; ++i) {
        uint32x4_t w = m[i % 4];
        if (i >= 4) {
            w = vsha256su1q_u32(vsha256su0q_u32(m[i % 4], m[(i + 1) % 4]), m[(i + 2) % 4], m[(i + 3) % 4]);
            m[i % 4] = w;
        }
        uint32x4_t wk = vaddq_u32(w, vld1q_u32(k + 4 * i));
        uint32x4_t old = abcd;
        abcd = vsha256hq_u32(abcd, efgh, wk);
        efgh = vsha256h2q_u32(efgh, old, wk);
    }
    vst1q_u32(state, vaddq_u32(abcd, abcd0));
    vst1q_u32(state + 4, vaddq_u32(efgh, efgh0));
}
