// names for fp/simd encodings, nullptr = unallocated
#include "a64/simd.hpp"

namespace tsugiki::a64 {

namespace {

// fp ftype: 00 s, 01 d, 11 h, 10 nothing
bool good_ftype(const Instruction& in) {
    return in.bits(23, 22) != 2;
}

const char* fp_fixed(const Instruction& in) {
    if (!good_ftype(in) || (!in.sf() && !in.bit(15))) {
        return nullptr;
    }
    switch (in.bits(20, 16)) {
    case 0b00010: return "scvtf";
    case 0b00011: return "ucvtf";
    case 0b11000: return "fcvtzs";
    case 0b11001: return "fcvtzu";
    default: return nullptr;
    }
}

const char* fp_int(const Instruction& in) {
    unsigned ftype = in.bits(23, 22), rmode = in.bits(20, 19), opcode = in.bits(18, 16);
    bool sf = in.sf();
    if (opcode == 6 || opcode == 7) {
        // fmov between general and fp registers
        if (rmode == 0) {
            if (ftype == 3 || (ftype == 0 && !sf) || (ftype == 1 && sf)) {
                return "fmov";
            }
            return nullptr;
        }
        if (rmode == 1 && ftype == 2 && sf) {
            return "fmov";
        }
        if (rmode == 3 && opcode == 6 && ftype == 1 && !sf) {
            return "fjcvtzs";
        }
        return nullptr;
    }
    if (ftype == 2) {
        return nullptr;
    }
    static const char* to_int[2][4] = {{"fcvtns", "fcvtps", "fcvtms", "fcvtzs"}, {"fcvtnu", "fcvtpu", "fcvtmu", "fcvtzu"}};
    switch (opcode) {
    case 0: case 1: return to_int[opcode][rmode];
    case 2: return rmode == 0 ? "scvtf" : nullptr;
    case 3: return rmode == 0 ? "ucvtf" : nullptr;
    case 4: return rmode == 0 ? "fcvtas" : nullptr;
    case 5: return rmode == 0 ? "fcvtau" : nullptr;
    default: return nullptr;
    }
}

const char* fp_1src(const Instruction& in) {
    unsigned ftype = in.bits(23, 22), opcode = in.bits(20, 15);
    if (ftype == 2) {
        return nullptr;
    }
    switch (opcode) {
    case 0: return "fmov";
    case 1: return "fabs";
    case 2: return "fneg";
    case 3: return "fsqrt";
    case 4: case 5: case 7:
        // fcvt to another precision
        return (opcode & 3) != ftype ? "fcvt" : nullptr;
    case 6: return ftype == 1 ? "bfcvt" : nullptr;
    case 8: return "frintn";
    case 9: return "frintp";
    case 10: return "frintm";
    case 11: return "frintz";
    case 12: return "frinta";
    case 14: return "frintx";
    case 15: return "frinti";
    case 16: return ftype < 2 ? "frint32z" : nullptr;
    case 17: return ftype < 2 ? "frint32x" : nullptr;
    case 18: return ftype < 2 ? "frint64z" : nullptr;
    case 19: return ftype < 2 ? "frint64x" : nullptr;
    default: return nullptr;
    }
}

const char* fp_2src(const Instruction& in) {
    static const char* names[] = {"fmul", "fdiv", "fadd", "fsub", "fmax", "fmin", "fmaxnm", "fminnm", "fnmul"};
    unsigned opcode = in.bits(15, 12);
    return good_ftype(in) && opcode <= 8 ? names[opcode] : nullptr;
}

const char* fp_3src(const Instruction& in) {
    static const char* names[] = {"fmadd", "fmsub", "fnmadd", "fnmsub"};
    return good_ftype(in) ? names[in.bit(21) * 2 + in.bit(15)] : nullptr;
}

// size:Q == 110, no 1d vectors
bool one_d(const Instruction& in) {
    return in.bits(23, 22) == 3 && !in.bit(30);
}

const char* three_same(const Instruction& in) {
    unsigned opcode = in.bits(15, 11), size = in.bits(23, 22);
    bool u = in.bit(29), q = in.bit(30);
    if (opcode == 3) {
        static const char* logic[2][4] = {{"and", "bic", "orr", "orn"}, {"eor", "bsl", "bit", "bif"}};
        return logic[u][size];
    }
    if (opcode >= 0b11000) {
        // fp, size<1> picks the op, size<0> the precision
        bool a = size >> 1, sz = size & 1;
        // fhm widening ones, half to single
        if (!u && opcode == 0b11101) {
            return sz ? nullptr : a ? "fmlsl" : "fmlal";
        }
        if (u && opcode == 0b11001) {
            return sz ? nullptr : a ? "fmlsl2" : "fmlal2";
        }
        if (sz && !q) {
            return nullptr;
        }
        static const char* fp[2][8][2] = {
            {{"fmaxnm", "fminnm"}, {"fmla", "fmls"}, {"fadd", "fsub"}, {"fmulx", nullptr}, {"fcmeq", nullptr}, {nullptr, nullptr}, {"fmax", "fmin"}, {"frecps", "frsqrts"}},
            {{"fmaxnmp", "fminnmp"}, {nullptr, nullptr}, {"faddp", "fabd"}, {"fmul", nullptr}, {"fcmge", "fcmgt"}, {"facge", "facgt"}, {"fmaxp", "fminp"}, {"fdiv", nullptr}},
        };
        return fp[u][opcode - 0b11000][a];
    }
    static const char* names[2][23] = {
        {"shadd", "sqadd", "srhadd", nullptr, "shsub", "sqsub", "cmgt", "cmge", "sshl", "sqshl", "srshl", "sqrshl", "smax", "smin", "sabd", "saba", "add", "cmtst",
         "mla", "mul", "smaxp", "sminp", "sqdmulh"},
        {"uhadd", "uqadd", "urhadd", nullptr, "uhsub", "uqsub", "cmhi", "cmhs", "ushl", "uqshl", "urshl", "uqrshl", "umax", "umin", "uabd", "uaba", "sub", "cmeq",
         "mls", "pmul", "umaxp", "uminp", "sqrdmulh"},
    };
    if (opcode == 0b10111) {
        return u || one_d(in) ? nullptr : "addp";
    }
    if (one_d(in)) {
        return nullptr;
    }
    switch (opcode) {
    case 0: case 2: case 4: case 12: case 13: case 14: case 15: case 18: case 20: case 21:
        if (size == 3) {
            return nullptr;
        }
        break;
    case 19:
        if (u ? size != 0 : size == 3) {
            return nullptr;
        }
        break;
    case 22:
        if (size == 0 || size == 3) {
            return nullptr;
        }
        break;
    default:
        break;
    }
    return names[u][opcode];
}

const char* three_different(const Instruction& in) {
    unsigned opcode = in.bits(15, 12), size = in.bits(23, 22);
    bool u = in.bit(29);
    static const char* names[2][16] = {
        {"saddl", "saddw", "ssubl", "ssubw", "addhn", "sabal", "subhn", "sabdl", "smlal", "sqdmlal", "smlsl", "sqdmlsl", "smull", "sqdmull", "pmull", nullptr},
        {"uaddl", "uaddw", "usubl", "usubw", "raddhn", "uabal", "rsubhn", "uabdl", "umlal", nullptr, "umlsl", nullptr, "umull", nullptr, nullptr, nullptr},
    };
    const char* name = names[u][opcode];
    if (name == nullptr) {
        return nullptr;
    }
    if (opcode == 14) {
        return size == 0 || size == 3 ? name : nullptr;
    }
    if (opcode == 9 || opcode == 11 || opcode == 13) {
        return size == 1 || size == 2 ? name : nullptr;
    }
    return size == 3 ? nullptr : name;
}

const char* two_misc(const Instruction& in) {
    unsigned opcode = in.bits(16, 12), size = in.bits(23, 22);
    bool u = in.bit(29), q = in.bit(30), a = size >> 1, sz = size & 1;
    // fp ones, picked by size<1>
    struct Fp {
        unsigned opcode;
        const char* name[2][2];
    };
    static const Fp fp[] = {
        {0b11000, {{"frintn", "frintp"}, {"frinta", nullptr}}},
        {0b11001, {{"frintm", "frintz"}, {"frintx", "frinti"}}},
        {0b11010, {{"fcvtns", "fcvtps"}, {"fcvtnu", "fcvtpu"}}},
        {0b11011, {{"fcvtms", "fcvtzs"}, {"fcvtmu", "fcvtzu"}}},
        {0b11100, {{"fcvtas", "urecpe"}, {"fcvtau", "ursqrte"}}},
        {0b11101, {{"scvtf", "frecpe"}, {"ucvtf", "frsqrte"}}},
        {0b01100, {{nullptr, "fcmgt"}, {nullptr, "fcmge"}}},
        {0b01101, {{nullptr, "fcmeq"}, {nullptr, "fcmle"}}},
        {0b01110, {{nullptr, "fcmlt"}, {nullptr, nullptr}}},
        {0b01111, {{nullptr, "fabs"}, {nullptr, "fneg"}}},
        {0b11111, {{"frint64z", nullptr}, {"frint64x", "fsqrt"}}},
        {0b11110, {{"frint32z", nullptr}, {"frint32x", nullptr}}},
    };
    for (const Fp& f : fp) {
        if (f.opcode == opcode) {
            const char* name = f.name[u][a];
            if (name == nullptr || (sz && !q)) {
                return nullptr;
            }
            // urecpe and ursqrte are 32-bit only
            if ((opcode == 0b11100) && a && sz) {
                return nullptr;
            }
            return name;
        }
    }
    switch (opcode) {
    case 0b10110:
        if (!u) {
            return a ? (sz ? nullptr : "bfcvtn") : "fcvtn";
        }
        return !a && sz ? "fcvtxn" : nullptr;
    case 0b10111:
        return !u && !a ? "fcvtl" : nullptr;
    case 0b00000:
        if (u) {
            return size <= 1 ? "rev32" : nullptr;
        }
        return size != 3 ? "rev64" : nullptr;
    case 0b00001:
        return !u && size == 0 ? "rev16" : nullptr;
    case 0b00010: return size != 3 ? (u ? "uaddlp" : "saddlp") : nullptr;
    case 0b00110: return size != 3 ? (u ? "uadalp" : "sadalp") : nullptr;
    case 0b00011: return one_d(in) ? nullptr : u ? "usqadd" : "suqadd";
    case 0b00111: return one_d(in) ? nullptr : u ? "sqneg" : "sqabs";
    case 0b00100: return size != 3 ? (u ? "clz" : "cls") : nullptr;
    case 0b00101:
        if (!u) {
            return size == 0 ? "cnt" : nullptr;
        }
        return size == 0 ? "not" : size == 1 ? "rbit" : nullptr;
    case 0b01000: return one_d(in) ? nullptr : u ? "cmge" : "cmgt";
    case 0b01001: return one_d(in) ? nullptr : u ? "cmle" : "cmeq";
    case 0b01010: return !u && !one_d(in) ? "cmlt" : nullptr;
    case 0b01011: return one_d(in) ? nullptr : u ? "neg" : "abs";
    case 0b10010: return size != 3 ? (u ? "sqxtun" : "xtn") : nullptr;
    case 0b10011: return u && size != 3 ? "shll" : nullptr;
    case 0b10100: return size != 3 ? (u ? "uqxtn" : "sqxtn") : nullptr;
    default: return nullptr;
    }
}

const char* across(const Instruction& in) {
    unsigned opcode = in.bits(16, 12), size = in.bits(23, 22);
    bool u = in.bit(29), q = in.bit(30);
    if (opcode == 0b01100 || opcode == 0b01111) {
        // fp: u=1 single, u=0 half
        bool a = size >> 1, sz = size & 1;
        if (sz || (u && !q)) {
            return nullptr;
        }
        if (opcode == 0b01100) {
            return a ? "fminnmv" : "fmaxnmv";
        }
        return a ? "fminv" : "fmaxv";
    }
    if (size == 3 || (size == 2 && !q)) {
        return nullptr;
    }
    switch (opcode) {
    case 0b00011: return u ? "uaddlv" : "saddlv";
    case 0b01010: return u ? "umaxv" : "smaxv";
    case 0b11010: return u ? "uminv" : "sminv";
    case 0b11011: return u ? nullptr : "addv";
    default: return nullptr;
    }
}

const char* copy(const Instruction& in) {
    unsigned imm5 = in.bits(20, 16), imm4 = in.bits(14, 11);
    bool op = in.bit(29), q = in.bit(30);
    if ((imm5 & 15) == 0) {
        return nullptr;
    }
    unsigned size = __builtin_ctz(imm5);
    if (op) {
        return q ? "ins" : nullptr;
    }
    switch (imm4) {
    case 0b0000: return size == 3 && !q ? nullptr : "dup";
    case 0b0001: return size == 3 && !q ? nullptr : "dup";
    case 0b0011: return q ? "ins" : nullptr;
    case 0b0101: return size < (q ? 3u : 2u) ? "smov" : nullptr;
    case 0b0111: return q ? (size == 3 ? "umov" : nullptr) : (size < 3 ? "umov" : nullptr);
    default: return nullptr;
    }
}

const char* modified_imm(const Instruction& in) {
    unsigned cmode = in.bits(15, 12);
    bool op = in.bit(29), q = in.bit(30), o2 = in.bit(11);
    if (o2) {
        return cmode == 15 && !op ? "fmov" : nullptr;
    }
    if (cmode == 15) {
        return !op || q ? "fmov" : nullptr;
    }
    if (cmode == 14) {
        return "movi";
    }
    if ((cmode & 1) == 0 || (cmode & 0b1110) == 0b1100) {
        return op ? "mvni" : "movi";
    }
    return op ? "bic" : "orr";
}

// element size from immh, 0 = b
int immh_size(const Instruction& in) {
    unsigned immh = in.bits(22, 19);
    return immh == 0 ? -1 : 31 - __builtin_clz(immh);
}

const char* shift_imm(const Instruction& in) {
    int size = immh_size(in);
    unsigned opcode = in.bits(15, 11);
    bool u = in.bit(29), q = in.bit(30);
    if (size < 0) {
        return nullptr;
    }
    bool narrow = opcode >= 0b10000 && opcode <= 0b10100;
    if (narrow ? size == 3 : (size == 3 && !q)) {
        return nullptr;
    }
    if (opcode == 0b11100 || opcode == 0b11111) {
        if (size == 0) {
            return nullptr;
        }
        return opcode == 0b11100 ? (u ? "ucvtf" : "scvtf") : (u ? "fcvtzu" : "fcvtzs");
    }
    static const char* names[2][21] = {
        {"sshr", nullptr, "ssra", nullptr, "srshr", nullptr, "srsra", nullptr, nullptr, nullptr, "shl", nullptr, nullptr, nullptr, "sqshl", nullptr, "shrn", "rshrn",
         "sqshrn", "sqrshrn", "sshll"},
        {"ushr", nullptr, "usra", nullptr, "urshr", nullptr, "ursra", nullptr, "sri", nullptr, "sli", nullptr, "sqshlu", nullptr, "uqshl", nullptr, "sqshrun",
         "sqrshrun", "uqshrn", "uqrshrn", "ushll"},
    };
    return opcode <= 20 ? names[u][opcode] : nullptr;
}

const char* indexed(const Instruction& in) {
    unsigned opcode = in.bits(15, 12), size = in.bits(23, 22);
    bool u = in.bit(29), q = in.bit(30);
    // the odd extensions: dots, fhm, bf16, fcma
    if (!u && opcode == 0b1111) {
        static const char* names[] = {"sudot", "bfdot", "usdot", nullptr};
        return size == 3 ? (q ? "bfmlalt" : "bfmlalb") : names[size];
    }
    if (size == 2 && ((!u && (opcode == 0 || opcode == 4)) || (u && (opcode == 8 || opcode == 12)))) {
        static const char* names[2][2] = {{"fmlal", "fmlsl"}, {"fmlal2", "fmlsl2"}};
        return names[u][opcode >> 2 & 1];
    }
    if (u && (opcode & 0b1001) == 0b0001) {
        // fcmla by element: h indexes with h:l, s with h alone
        if (size == 1) {
            return q || !in.bit(11) ? "fcmla" : nullptr;
        }
        return size == 2 && q && !in.bit(21) ? "fcmla" : nullptr;
    }
    bool fp = (!u && (opcode == 1 || opcode == 5 || opcode == 9)) || (u && opcode == 9);
    if (fp) {
        // 10 s, 11 d, 00 h
        if (size == 1 || (size == 3 && (in.bit(21) || !in.bit(30)))) {
            return nullptr;
        }
        if (u) {
            return "fmulx";
        }
        return opcode == 1 ? "fmla" : opcode == 5 ? "fmls" : "fmul";
    }
    if (opcode == 14) {
        return size == 2 ? (u ? "udot" : "sdot") : nullptr;
    }
    if (size == 0 || size == 3) {
        return nullptr;
    }
    static const char* names[2][16] = {
        {nullptr, nullptr, "smlal", "sqdmlal", nullptr, nullptr, "smlsl", "sqdmlsl", "mul", nullptr, "smull", "sqdmull", "sqdmulh", "sqrdmulh", nullptr, nullptr},
        {"mla", nullptr, "umlal", nullptr, "mls", nullptr, "umlsl", nullptr, nullptr, nullptr, "umull", nullptr, nullptr, "sqrdmlah", nullptr, "sqrdmlsh"},
    };
    return names[u][opcode];
}

const char* three_same_extra(const Instruction& in) {
    unsigned opcode = in.bits(14, 11), size = in.bits(23, 22);
    bool u = in.bit(29), q = in.bit(30);
    if (!u) {
        switch (opcode) {
        case 0b0010: return size == 2 ? "sdot" : nullptr;
        case 0b0011: return size == 2 ? "usdot" : nullptr;
        case 0b0100: return size == 2 && q ? "smmla" : nullptr;
        case 0b0101: return size == 2 && q ? "usmmla" : nullptr;
        default: return nullptr;
        }
    }
    switch (opcode) {
    case 0b0000: return size == 1 || size == 2 ? "sqrdmlah" : nullptr;
    case 0b0001: return size == 1 || size == 2 ? "sqrdmlsh" : nullptr;
    case 0b0010: return size == 2 ? "udot" : nullptr;
    case 0b0100: return size == 2 && q ? "ummla" : nullptr;
    case 0b1000: case 0b1001: case 0b1010: case 0b1011:
        return size != 0 && !(size == 3 && !q) ? "fcmla" : nullptr;
    case 0b1100: case 0b1110:
        return size != 0 && !(size == 3 && !q) ? "fcadd" : nullptr;
    case 0b1101:
        return size == 1 ? (q ? "bfmmla" : nullptr) : size == 0 ? nullptr : nullptr;
    case 0b1111:
        return size == 1 ? "bfdot" : size == 3 ? (q ? "bfmlalt" : "bfmlalb") : nullptr;
    default:
        return nullptr;
    }
}

const char* permute(const Instruction& in) {
    static const char* names[] = {nullptr, "uzp1", "trn1", "zip1", nullptr, "uzp2", "trn2", "zip2"};
    return one_d(in) ? nullptr : names[in.bits(14, 12)];
}

const char* scalar_three_same(const Instruction& in) {
    unsigned opcode = in.bits(15, 11), size = in.bits(23, 22);
    bool u = in.bit(29), a = size >> 1;
    if (opcode >= 0b11000) {
        struct Fp {
            unsigned opcode;
            const char* name[2][2];
        };
        static const Fp fp[] = {
            {0b11011, {{"fmulx", nullptr}, {nullptr, nullptr}}},
            {0b11100, {{"fcmeq", nullptr}, {"fcmge", "fcmgt"}}},
            {0b11101, {{nullptr, nullptr}, {"facge", "facgt"}}},
            {0b11111, {{"frecps", "frsqrts"}, {nullptr, nullptr}}},
            {0b11010, {{nullptr, nullptr}, {nullptr, "fabd"}}},
        };
        for (const Fp& f : fp) {
            if (f.opcode == opcode) {
                return f.name[u][a];
            }
        }
        return nullptr;
    }
    switch (opcode) {
    case 0b00001: return u ? "uqadd" : "sqadd";
    case 0b00101: return u ? "uqsub" : "sqsub";
    case 0b01001: return u ? "uqshl" : "sqshl";
    case 0b01011: return u ? "uqrshl" : "sqrshl";
    case 0b10110: return size == 1 || size == 2 ? (u ? "sqrdmulh" : "sqdmulh") : nullptr;
    default: break;
    }
    if (size != 3) {
        return nullptr;
    }
    switch (opcode) {
    case 0b00110: return u ? "cmhi" : "cmgt";
    case 0b00111: return u ? "cmhs" : "cmge";
    case 0b01000: return u ? "ushl" : "sshl";
    case 0b01010: return u ? "urshl" : "srshl";
    case 0b10000: return u ? "sub" : "add";
    case 0b10001: return u ? "cmeq" : "cmtst";
    default: return nullptr;
    }
}

const char* scalar_three_different(const Instruction& in) {
    unsigned size = in.bits(23, 22);
    if (in.bit(29) || size == 0 || size == 3) {
        return nullptr;
    }
    switch (in.bits(15, 12)) {
    case 0b1001: return "sqdmlal";
    case 0b1011: return "sqdmlsl";
    case 0b1101: return "sqdmull";
    default: return nullptr;
    }
}

const char* scalar_two_misc(const Instruction& in) {
    unsigned opcode = in.bits(16, 12), size = in.bits(23, 22);
    bool u = in.bit(29), a = size >> 1, sz = size & 1;
    struct Fp {
        unsigned opcode;
        const char* name[2][2];
    };
    static const Fp fp[] = {
        {0b11010, {{"fcvtns", "fcvtps"}, {"fcvtnu", "fcvtpu"}}},
        {0b11011, {{"fcvtms", "fcvtzs"}, {"fcvtmu", "fcvtzu"}}},
        {0b11100, {{"fcvtas", nullptr}, {"fcvtau", nullptr}}},
        {0b11101, {{"scvtf", "frecpe"}, {"ucvtf", "frsqrte"}}},
        {0b11111, {{nullptr, "frecpx"}, {nullptr, nullptr}}},
        {0b01100, {{nullptr, "fcmgt"}, {nullptr, "fcmge"}}},
        {0b01101, {{nullptr, "fcmeq"}, {nullptr, "fcmle"}}},
        {0b01110, {{nullptr, "fcmlt"}, {nullptr, nullptr}}},
    };
    for (const Fp& f : fp) {
        if (f.opcode == opcode) {
            return f.name[u][a];
        }
    }
    switch (opcode) {
    case 0b00011: return u ? "usqadd" : "suqadd";
    case 0b00111: return u ? "sqneg" : "sqabs";
    case 0b10100: return size != 3 ? (u ? "uqxtn" : "sqxtn") : nullptr;
    case 0b10010: return u && size != 3 ? "sqxtun" : nullptr;
    case 0b10110: return u && !a && sz ? "fcvtxn" : nullptr;
    default: break;
    }
    if (size != 3) {
        return nullptr;
    }
    switch (opcode) {
    case 0b01000: return u ? "cmge" : "cmgt";
    case 0b01001: return u ? "cmle" : "cmeq";
    case 0b01010: return u ? nullptr : "cmlt";
    case 0b01011: return u ? "neg" : "abs";
    default: return nullptr;
    }
}

const char* scalar_pairwise(const Instruction& in) {
    unsigned opcode = in.bits(16, 12), size = in.bits(23, 22);
    bool u = in.bit(29), a = size >> 1, sz = size & 1;
    if (opcode == 0b11011) {
        return !u && size == 3 ? "addp" : nullptr;
    }
    // u=1 single/double, u=0 half (sz must be 0 then)
    if (!u && sz) {
        return nullptr;
    }
    switch (opcode) {
    case 0b01100: return a ? "fminnmp" : "fmaxnmp";
    case 0b01101: return a ? nullptr : "faddp";
    case 0b01111: return a ? "fminp" : "fmaxp";
    default: return nullptr;
    }
}

const char* scalar_shift_imm(const Instruction& in) {
    int size = immh_size(in);
    unsigned opcode = in.bits(15, 11);
    bool u = in.bit(29);
    if (size < 0) {
        return nullptr;
    }
    bool d_only = opcode <= 0b01010;
    if (d_only && size != 3) {
        return nullptr;
    }
    if (opcode >= 0b10000 && opcode <= 0b10011 && size == 3) {
        return nullptr;
    }
    switch (opcode) {
    case 0b00000: return u ? "ushr" : "sshr";
    case 0b00010: return u ? "usra" : "ssra";
    case 0b00100: return u ? "urshr" : "srshr";
    case 0b00110: return u ? "ursra" : "srsra";
    case 0b01000: return u ? "sri" : nullptr;
    case 0b01010: return u ? "sli" : "shl";
    case 0b01100: return u ? "sqshlu" : nullptr;
    case 0b01110: return u ? "uqshl" : "sqshl";
    case 0b10000: return u ? "sqshrun" : nullptr;
    case 0b10001: return u ? "sqrshrun" : nullptr;
    case 0b10010: return u ? "uqshrn" : "sqshrn";
    case 0b10011: return u ? "uqrshrn" : "sqrshrn";
    case 0b11100: return size ? (u ? "ucvtf" : "scvtf") : nullptr;
    case 0b11111: return size ? (u ? "fcvtzu" : "fcvtzs") : nullptr;
    default: return nullptr;
    }
}

const char* scalar_indexed(const Instruction& in) {
    unsigned opcode = in.bits(15, 12), size = in.bits(23, 22);
    bool u = in.bit(29);
    bool fp = (!u && (opcode == 1 || opcode == 5 || opcode == 9)) || (u && opcode == 9);
    if (fp) {
        if (size == 1 || (size == 3 && in.bit(21))) {
            return nullptr;
        }
        return u ? "fmulx" : opcode == 1 ? "fmla" : opcode == 5 ? "fmls" : "fmul";
    }
    if (size == 0 || size == 3) {
        return nullptr;
    }
    switch (opcode) {
    case 0b0011: return u ? nullptr : "sqdmlal";
    case 0b0111: return u ? nullptr : "sqdmlsl";
    case 0b1011: return u ? nullptr : "sqdmull";
    case 0b1100: return u ? nullptr : "sqdmulh";
    case 0b1101: return u ? "sqrdmlah" : "sqrdmulh";
    case 0b1111: return u ? "sqrdmlsh" : nullptr;
    default: return nullptr;
    }
}

bool second_half(Op op, const char* name);

}  // namespace

bool has_second_half(Op op, const std::string& base) {
    return second_half(op, base.c_str());
}

namespace {

bool second_half(Op op, const char* name) {
    // narrowing, widening and long ops take a 2 with q
    static const char* const twos[] = {"saddl", "saddw", "ssubl", "ssubw", "addhn", "sabal", "subhn", "sabdl", "smlal", "sqdmlal", "smlsl", "sqdmlsl", "smull",
                                       "sqdmull", "pmull", "uaddl", "uaddw", "usubl", "usubw", "raddhn", "uabal", "rsubhn", "uabdl", "umlal", "umlsl", "umull",
                                       "xtn", "sqxtn", "sqxtun", "uqxtn", "fcvtn", "fcvtl", "fcvtxn", "shll", "bfcvtn", "shrn", "rshrn", "sqshrn", "sqrshrn",
                                       "sqshrun", "sqrshrun", "uqshrn", "uqrshrn", "sshll", "ushll"};
    switch (op) {
    case Op::simd_three_different:
    case Op::simd_two_misc:
    case Op::simd_shift_imm:
    case Op::simd_indexed:
        break;
    default:
        return false;
    }
    for (const char* two : twos) {
        if (__builtin_strcmp(two, name) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

std::string simd_name(const Instruction& in) {
    const char* name = nullptr;
    switch (in.op) {
    case Op::fp_fixed: name = fp_fixed(in); break;
    case Op::fp_int: name = fp_int(in); break;
    case Op::fp_1src: name = fp_1src(in); break;
    case Op::fp_compare: name = good_ftype(in) ? (in.bit(4) ? "fcmpe" : "fcmp") : nullptr; break;
    case Op::fp_imm: name = good_ftype(in) ? "fmov" : nullptr; break;
    case Op::fp_ccmp: name = good_ftype(in) ? (in.bit(4) ? "fccmpe" : "fccmp") : nullptr; break;
    case Op::fp_2src: name = fp_2src(in); break;
    case Op::fp_csel: name = good_ftype(in) ? "fcsel" : nullptr; break;
    case Op::fp_3src: name = fp_3src(in); break;
    case Op::simd_aes: {
        static const char* names[] = {"aese", "aesd", "aesmc", "aesimc"};
        unsigned opcode = in.bits(16, 12);
        name = in.bits(23, 22) == 0 && opcode >= 4 && opcode <= 7 ? names[opcode - 4] : nullptr;
        break;
    }
    case Op::simd_sha3reg: {
        static const char* names[] = {"sha1c", "sha1p", "sha1m", "sha1su0", "sha256h", "sha256h2", "sha256su1", nullptr};
        name = in.bits(23, 22) == 0 ? names[in.bits(14, 12)] : nullptr;
        break;
    }
    case Op::simd_sha2reg: {
        static const char* names[] = {"sha1h", "sha1su1", "sha256su0"};
        unsigned opcode = in.bits(16, 12);
        name = in.bits(23, 22) == 0 && opcode <= 2 ? names[opcode] : nullptr;
        break;
    }
    case Op::simd_three_same: name = three_same(in); break;
    case Op::simd_three_different: name = three_different(in); break;
    case Op::simd_two_misc: name = two_misc(in); break;
    case Op::simd_across: name = across(in); break;
    case Op::simd_copy: name = copy(in); break;
    case Op::simd_three_same_extra: name = three_same_extra(in); break;
    case Op::simd_modified_imm: name = modified_imm(in); break;
    case Op::simd_shift_imm: name = shift_imm(in); break;
    case Op::simd_indexed: name = indexed(in); break;
    case Op::simd_extract: name = !in.bit(30) && in.bit(14) ? nullptr : "ext"; break;
    case Op::simd_permute: name = permute(in); break;
    case Op::simd_table: name = in.bit(12) ? "tbx" : "tbl"; break;
    case Op::simd_scalar_three_same: name = scalar_three_same(in); break;
    case Op::simd_scalar_three_different: name = scalar_three_different(in); break;
    case Op::simd_scalar_two_misc: name = scalar_two_misc(in); break;
    case Op::simd_scalar_pairwise: name = scalar_pairwise(in); break;
    case Op::simd_scalar_copy: name = !in.bit(29) && in.bits(14, 11) == 0 && (in.bits(20, 16) & 15) != 0 ? "dup" : nullptr; break;
    case Op::simd_scalar_shift_imm: name = scalar_shift_imm(in); break;
    case Op::simd_scalar_indexed: name = scalar_indexed(in); break;
    default: return {};
    }
    if (name == nullptr) {
        return {};
    }
    std::string result = name;
    if (in.bit(30) && second_half(in.op, name)) {
        result += "2";
    }
    return result;
}

bool is_simd(Op op) {
    return op >= Op::fp_fixed && op <= Op::simd_scalar_indexed;
}

}  // namespace tsugiki::a64
