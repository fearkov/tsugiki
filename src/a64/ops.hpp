// every a64 encoding we know: name, llvm mnemonic, bit pattern (bit 31 first)
// "" mnemonic = depends on fields, see mnemonic()
// first match wins, so specific ones go first
#pragma once

#define TSUGIKI_A64_OPS(X)                                                                  \
    /* pc-relative */                                                                       \
    X(adr, "adr", "0 xx 10000 xxxxxxxxxxxxxxxxxxx xxxxx")                                   \
    X(adrp, "adrp", "1 xx 10000 xxxxxxxxxxxxxxxxxxx xxxxx")                                 \
    /* add/sub immediate */                                                                 \
    X(add_imm, "add", "x 0 0 100010 x xxxxxxxxxxxx xxxxx xxxxx")                            \
    X(adds_imm, "adds", "x 0 1 100010 x xxxxxxxxxxxx xxxxx xxxxx")                          \
    X(sub_imm, "sub", "x 1 0 100010 x xxxxxxxxxxxx xxxxx xxxxx")                            \
    X(subs_imm, "subs", "x 1 1 100010 x xxxxxxxxxxxx xxxxx xxxxx")                          \
    /* cssc min/max immediate */                                                            \
    X(minmax_imm, "", "x 0 0 1000111 xxxx xxxxxxxx xxxxx xxxxx")                            \
    /* logical immediate */                                                                 \
    X(and_imm, "and", "x 00 100100 x xxxxxx xxxxxx xxxxx xxxxx")                            \
    X(orr_imm, "orr", "x 01 100100 x xxxxxx xxxxxx xxxxx xxxxx")                            \
    X(eor_imm, "eor", "x 10 100100 x xxxxxx xxxxxx xxxxx xxxxx")                            \
    X(ands_imm, "ands", "x 11 100100 x xxxxxx xxxxxx xxxxx xxxxx")                          \
    /* move wide */                                                                         \
    X(movn, "movn", "x 00 100101 xx xxxxxxxxxxxxxxxx xxxxx")                                \
    X(movz, "movz", "x 10 100101 xx xxxxxxxxxxxxxxxx xxxxx")                                \
    X(movk, "movk", "x 11 100101 xx xxxxxxxxxxxxxxxx xxxxx")                                \
    /* bitfield, extract */                                                                 \
    X(sbfm, "sbfm", "x 00 100110 x xxxxxx xxxxxx xxxxx xxxxx")                              \
    X(bfm, "bfm", "x 01 100110 x xxxxxx xxxxxx xxxxx xxxxx")                                \
    X(ubfm, "ubfm", "x 10 100110 x xxxxxx xxxxxx xxxxx xxxxx")                              \
    X(extr, "extr", "x 00 100111 x 0 xxxxx xxxxxx xxxxx xxxxx")                             \
    /* branches */                                                                          \
    X(b_cond, "", "0101010 0 xxxxxxxxxxxxxxxxxxx 0 xxxx")                                   \
    X(bc_cond, "", "0101010 0 xxxxxxxxxxxxxxxxxxx 1 xxxx")                                  \
    X(svc, "svc", "11010100 000 xxxxxxxxxxxxxxxx 000 01")                                   \
    X(brk, "brk", "11010100 001 xxxxxxxxxxxxxxxx 000 00")                                   \
    X(hlt, "hlt", "11010100 010 xxxxxxxxxxxxxxxx 000 00")                                   \
    X(hint, "hint", "1101010100 0 00 011 0010 xxxx xxx 11111")                              \
    X(clrex, "clrex", "1101010100 0 00 011 0011 xxxx 010 11111")                            \
    X(dsb, "dsb", "1101010100 0 00 011 0011 xxxx 100 11111")                                \
    X(dmb, "dmb", "1101010100 0 00 011 0011 xxxx 101 11111")                                \
    X(isb, "isb", "1101010100 0 00 011 0011 xxxx 110 11111")                                \
    X(sb, "sb", "1101010100 0 00 011 0011 0000 111 11111")                                  \
    X(msr_imm, "msr", "1101010100 0 00 xxx 0100 xxxx xxx 11111")                            \
    X(sys, "sys", "1101010100 0 01 xxx xxxx xxxx xxx xxxxx")                                \
    X(sysl, "sysl", "1101010100 1 01 xxx xxxx xxxx xxx xxxxx")                              \
    X(msr_reg, "msr", "1101010100 0 1 x xxx xxxx xxxx xxx xxxxx")                           \
    X(mrs, "mrs", "1101010100 1 1 x xxx xxxx xxxx xxx xxxxx")                               \
    X(br, "br", "1101011 0000 11111 000000 xxxxx 00000")                                    \
    X(blr, "blr", "1101011 0001 11111 000000 xxxxx 00000")                                  \
    X(ret, "ret", "1101011 0010 11111 000000 xxxxx 00000")                                  \
    X(braaz, "braaz", "1101011 0000 11111 00001 0 xxxxx 11111")                             \
    X(brabz, "brabz", "1101011 0000 11111 00001 1 xxxxx 11111")                             \
    X(blraaz, "blraaz", "1101011 0001 11111 00001 0 xxxxx 11111")                           \
    X(blrabz, "blrabz", "1101011 0001 11111 00001 1 xxxxx 11111")                           \
    X(retaa, "retaa", "1101011 0010 11111 00001 0 11111 11111")                             \
    X(retab, "retab", "1101011 0010 11111 00001 1 11111 11111")                             \
    X(braa, "braa", "1101011 1000 11111 00001 0 xxxxx xxxxx")                               \
    X(brab, "brab", "1101011 1000 11111 00001 1 xxxxx xxxxx")                               \
    X(blraa, "blraa", "1101011 1001 11111 00001 0 xxxxx xxxxx")                             \
    X(blrab, "blrab", "1101011 1001 11111 00001 1 xxxxx xxxxx")                             \
    X(b, "b", "0 00101 xxxxxxxxxxxxxxxxxxxxxxxxxx")                                         \
    X(bl, "bl", "1 00101 xxxxxxxxxxxxxxxxxxxxxxxxxx")                                       \
    X(cbz, "cbz", "x 011010 0 xxxxxxxxxxxxxxxxxxx xxxxx")                                   \
    X(cbnz, "cbnz", "x 011010 1 xxxxxxxxxxxxxxxxxxx xxxxx")                                 \
    X(tbz, "tbz", "x 011011 0 xxxxx xxxxxxxxxxxxxx xxxxx")                                  \
    X(tbnz, "tbnz", "x 011011 1 xxxxx xxxxxxxxxxxxxx xxxxx")                                \
    X(udf, "udf", "0000000000000000 xxxxxxxxxxxxxxxx")                                      \
    /* exclusives, acquire/release, cas */                                                  \
    X(stxr, "", "xx 001000 0 0 0 xxxxx 0 xxxxx xxxxx xxxxx")                                \
    X(stlxr, "", "xx 001000 0 0 0 xxxxx 1 xxxxx xxxxx xxxxx")                               \
    X(ldxr, "", "xx 001000 0 1 0 xxxxx 0 xxxxx xxxxx xxxxx")                                \
    X(ldaxr, "", "xx 001000 0 1 0 xxxxx 1 xxxxx xxxxx xxxxx")                               \
    X(stxp, "stxp", "1x 001000 0 0 1 xxxxx 0 xxxxx xxxxx xxxxx")                            \
    X(stlxp, "stlxp", "1x 001000 0 0 1 xxxxx 1 xxxxx xxxxx xxxxx")                          \
    X(ldxp, "ldxp", "1x 001000 0 1 1 xxxxx 0 xxxxx xxxxx xxxxx")                            \
    X(ldaxp, "ldaxp", "1x 001000 0 1 1 xxxxx 1 xxxxx xxxxx xxxxx")                          \
    X(stllr, "", "xx 001000 1 0 0 xxxxx 0 xxxxx xxxxx xxxxx")                               \
    X(stlr, "", "xx 001000 1 0 0 xxxxx 1 xxxxx xxxxx xxxxx")                                \
    X(ldlar, "", "xx 001000 1 1 0 xxxxx 0 xxxxx xxxxx xxxxx")                               \
    X(ldar, "", "xx 001000 1 1 0 xxxxx 1 xxxxx xxxxx xxxxx")                                \
    X(cas, "", "xx 001000 1 x 1 xxxxx x 11111 xxxxx xxxxx")                                 \
    X(casp, "", "0x 001000 0 x 1 xxxxx x 11111 xxxxx xxxxx")                                \
    /* rcpc unscaled: stlur, ldapur */                                                      \
    X(rcpc_unscaled, "", "xx 011001 xx 0 xxxxxxxxx 00 xxxxx xxxxx")                         \
    /* simd structures */                                                                   \
    X(simd_multiple, "", "0 x 0011000 x 000000 xxxx xx xxxxx xxxxx")                        \
    X(simd_multiple_post, "", "0 x 0011001 x 0 xxxxx xxxx xx xxxxx xxxxx")                 \
    X(simd_single, "", "0 x 0011010 x x 00000 xxx x xx xxxxx xxxxx")                        \
    X(simd_single_post, "", "0 x 0011011 x x xxxxx xxx x xx xxxxx xxxxx")                   \
    /* literal loads */                                                                     \
    X(ldr_literal, "", "xx 011 x 00 xxxxxxxxxxxxxxxxxxx xxxxx")                             \
    /* pairs */                                                                             \
    X(pair_noalloc, "", "xx 101 x 000 x xxxxxxx xxxxx xxxxx xxxxx")                         \
    X(pair_post, "", "xx 101 x 001 x xxxxxxx xxxxx xxxxx xxxxx")                            \
    X(pair_offset, "", "xx 101 x 010 x xxxxxxx xxxxx xxxxx xxxxx")                          \
    X(pair_pre, "", "xx 101 x 011 x xxxxxxx xxxxx xxxxx xxxxx")                             \
    /* single registers */                                                                  \
    X(ldapr, "", "xx 111 0 00 1 0 1 xxxxx 1 100 00 xxxxx xxxxx")                            \
    X(atomic, "", "xx 111 0 00 x x 1 xxxxx 0 xxx 00 xxxxx xxxxx")                           \
    X(swp, "", "xx 111 0 00 x x 1 xxxxx 1 000 00 xxxxx xxxxx")                              \
    X(ldraa, "ldraa", "11 111 0 00 0 x 1 xxxxxxxxx x 1 xxxxx xxxxx")                        \
    X(ldrab, "ldrab", "11 111 0 00 1 x 1 xxxxxxxxx x 1 xxxxx xxxxx")                        \
    X(single_unscaled, "", "xx 111 x 00 xx 0 xxxxxxxxx 00 xxxxx xxxxx")                     \
    X(single_post, "", "xx 111 x 00 xx 0 xxxxxxxxx 01 xxxxx xxxxx")                         \
    X(single_unprivileged, "", "xx 111 0 00 xx 0 xxxxxxxxx 10 xxxxx xxxxx")                 \
    X(single_pre, "", "xx 111 x 00 xx 0 xxxxxxxxx 11 xxxxx xxxxx")                          \
    X(single_register, "", "xx 111 x 00 xx 1 xxxxx xxx x 10 xxxxx xxxxx")                   \
    X(single_unsigned, "", "xx 111 x 01 xx xxxxxxxxxxxx xxxxx xxxxx")                       \
    /* data processing, registers */                                                        \
    X(logical_shifted, "", "x xx 01010 xx x xxxxx xxxxxx xxxxx xxxxx")                      \
    X(addsub_shifted, "", "x x x 01011 xx 0 xxxxx xxxxxx xxxxx xxxxx")                      \
    X(addsub_extended, "", "x x x 01011 00 1 xxxxx xxx xxx xxxxx xxxxx")                    \
    X(adc, "", "x x x 11010000 xxxxx 000000 xxxxx xxxxx")                                   \
    X(ccmp_register, "", "x x 1 11010010 xxxxx xxxx 0 0 xxxxx 0 xxxx")                      \
    X(ccmp_immediate, "", "x x 1 11010010 xxxxx xxxx 1 0 xxxxx 0 xxxx")                     \
    X(csel, "", "x x 0 11010100 xxxxx xxxx 0 x xxxxx xxxxx")                                \
    X(dp2, "", "x 0 0 11010110 xxxxx xxxxxx xxxxx xxxxx")                                   \
    X(dp1, "", "x 1 0 11010110 xxxxx xxxxxx xxxxx xxxxx")                                   \
    X(dp3, "", "x 00 11011 xxx xxxxx x xxxxx xxxxx xxxxx")                                 \
    /* scalar fp */                                                                         \
    X(fp_fixed, "", "x 0 0 11110 xx 0 xx xxx xxxxxx xxxxx xxxxx")                           \
    X(fp_int, "", "x 0 0 11110 xx 1 xx xxx 000000 xxxxx xxxxx")                             \
    X(fp_1src, "", "0 0 0 11110 xx 1 xxxxxx 10000 xxxxx xxxxx")                             \
    X(fp_compare, "", "0 0 0 11110 xx 1 xxxxx 00 1000 xxxxx xx000")                         \
    X(fp_imm, "", "0 0 0 11110 xx 1 xxxxxxxx 100 00000 xxxxx")                              \
    X(fp_ccmp, "", "0 0 0 11110 xx 1 xxxxx xxxx 01 xxxxx x xxxx")                           \
    X(fp_2src, "", "0 0 0 11110 xx 1 xxxxx xxxx 10 xxxxx xxxxx")                            \
    X(fp_csel, "", "0 0 0 11110 xx 1 xxxxx xxxx 11 xxxxx xxxxx")                            \
    X(fp_3src, "", "0 0 0 11111 xx x xxxxx x xxxxx xxxxx xxxxx")                            \
    /* advanced simd, crypto first */                                                       \
    X(simd_aes, "", "01001110 xx 10100 xxxxx 10 xxxxx xxxxx")                               \
    X(simd_sha3reg, "", "01011110 xx 0 xxxxx 0 xxx 00 xxxxx xxxxx")                         \
    X(simd_sha2reg, "", "01011110 xx 10100 xxxxx 10 xxxxx xxxxx")                           \
    X(simd_three_same, "", "0 x x 01110 xx 1 xxxxx xxxxx 1 xxxxx xxxxx")                    \
    X(simd_three_different, "", "0 x x 01110 xx 1 xxxxx xxxx 00 xxxxx xxxxx")               \
    X(simd_two_misc, "", "0 x x 01110 xx 10000 xxxxx 10 xxxxx xxxxx")                       \
    X(simd_across, "", "0 x x 01110 xx 11000 xxxxx 10 xxxxx xxxxx")                         \
    X(simd_copy, "", "0 x x 01110000 xxxxx 0 xxxx 1 xxxxx xxxxx")                           \
    X(simd_three_same_extra, "", "0 x x 01110 xx 0 xxxxx 1 xxxx 1 xxxxx xxxxx")             \
    X(simd_modified_imm, "", "0 x x 0111100000 xxx xxxx x 1 xxxxx xxxxx")                   \
    X(simd_shift_imm, "", "0 x x 011110 xxxx xxx xxxxx 1 xxxxx xxxxx")                      \
    X(simd_indexed, "", "0 x x 01111 xx x x xxxx xxxx x 0 xxxxx xxxxx")                     \
    X(simd_extract, "", "0 x 101110 00 0 xxxxx 0 xxxx 0 xxxxx xxxxx")                       \
    X(simd_permute, "", "0 x 001110 xx 0 xxxxx 0 xxx 10 xxxxx xxxxx")                       \
    X(simd_table, "", "0 x 001110 00 0 xxxxx 0 xx x 00 xxxxx xxxxx")                        \
    X(simd_scalar_three_same, "", "01 x 11110 xx 1 xxxxx xxxxx 1 xxxxx xxxxx")              \
    X(simd_scalar_three_different, "", "01 x 11110 xx 1 xxxxx xxxx 00 xxxxx xxxxx")         \
    X(simd_scalar_two_misc, "", "01 x 11110 xx 10000 xxxxx 10 xxxxx xxxxx")                 \
    X(simd_scalar_pairwise, "", "01 x 11110 xx 11000 xxxxx 10 xxxxx xxxxx")                 \
    X(simd_scalar_copy, "", "01 x 11110000 xxxxx 0 xxxx 1 xxxxx xxxxx")                     \
    X(simd_scalar_shift_imm, "", "01 x 111110 xxxx xxx xxxxx 1 xxxxx xxxxx")                \
    X(simd_scalar_indexed, "", "01 x 11111 xx x x xxxx xxxx x 0 xxxxx xxxxx")
