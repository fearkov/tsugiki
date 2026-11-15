#include "lift/simd.hpp"

#include <vector>

#include "a64/simd.hpp"
#include "lift/emit.hpp"

namespace tsugiki::lift {

namespace {

using a64::Instruction;
using a64::Op;
using namespace text;

// lane types by log2 of the lane bytes
const char* const uv[] = {"ts_u8", "ts_u16", "ts_u32", "ts_u64"};
const char* const sv[] = {"ts_i8", "ts_i16", "ts_i32", "ts_i64"};
const char* const ue[] = {"uint8_t", "uint16_t", "uint32_t", "uint64_t"};
const char* const se[] = {"int8_t", "int16_t", "int32_t", "int64_t"};
const char* const uh[] = {"ts_u8h", "ts_u16h", "ts_u32h", "ts_u64h"};
const char* const sh[] = {"ts_i8h", "ts_i16h", "ts_i32h", "ts_i64h"};
const char* const fv[] = {nullptr, "ts_f16", "ts_f32", "ts_f64"};
const char* const fe[] = {nullptr, "_Float16", "float", "double"};
const char* const fh[] = {nullptr, "ts_f16h", "ts_f32h", nullptr};

std::string str(int64_t v) {
    return std::to_string(v);
}

std::string reg(int r, const std::string& type) {
    return "((" + type + ")ts_v(s, " + str(r) + "))";
}

// 64-bit half of a register as a half vector type
std::string half(int r, bool upper, const std::string& type) {
    return std::string("((") + type + ")" + (upper ? "ts_high" : "ts_low") + "(ts_v(s, " + str(r) + ")))";
}

std::string store(int d, bool q, const std::string& value) {
    return std::string(q ? "ts_w" : "ts_w64") + "(s, " + str(d) + ", (ts_u64)(" + value + "));";
}

// a narrow 64-bit result, into the low half or, for the 2 forms, the high one
std::string store_narrow(int d, bool upper, const std::string& value) {
    if (upper) {
        return "ts_w(s, " + str(d) + ", ts_join(ts_low(ts_v(s, " + str(d) + ")), (ts_u64h)(" + value + ")));";
    }
    return "ts_w(s, " + str(d) + ", ts_join((ts_u64h)(" + value + "), (ts_u64h){0}));";
}

// scalar fp by log2 bytes: 1 h, 2 s, 3 d
std::string fget(int r, int size) {
    const char* f = size == 2 ? "ts_fs" : size == 3 ? "ts_fd" : "ts_fh";
    return std::string(f) + "(s, " + str(r) + ")";
}

std::string fset(int r, int size, const std::string& value) {
    const char* f = size == 2 ? "ts_ws" : size == 3 ? "ts_wd" : "ts_wh";
    return std::string(f) + "(s, " + str(r) + ", (" + fe[size] + ")(" + value + "));";
}

int ftype_size(unsigned ftype) {
    return ftype == 0 ? 2 : ftype == 1 ? 3 : 1;
}

// VFPExpandImm
uint64_t fp_immediate(unsigned imm8, int size) {
    uint64_t sign = imm8 >> 7, b = (imm8 >> 6) & 1, cd = (imm8 >> 4) & 3, efgh = imm8 & 15;
    int e = size == 1 ? 5 : size == 2 ? 8 : 11, f = size == 1 ? 10 : size == 2 ? 23 : 52;
    uint64_t exp = ((b ^ 1) << (e - 1)) | (b ? ((uint64_t(1) << (e - 3)) - 1) << 2 : 0) | cd;
    return (sign << (e + f)) | (exp << f) | (efgh << (f - 4));
}

uint64_t replicate(uint64_t value, int bits) {
    uint64_t r = 0;
    for (int i = 0; i < 64; i += bits) {
        r |= value << i;
    }
    return r;
}

// AdvSIMDExpandImm
uint64_t simd_immediate(bool op, unsigned cmode, uint64_t imm8) {
    switch (cmode >> 1) {
    case 0: return replicate(imm8, 32);
    case 1: return replicate(imm8 << 8, 32);
    case 2: return replicate(imm8 << 16, 32);
    case 3: return replicate(imm8 << 24, 32);
    case 4: return replicate(imm8, 16);
    case 5: return replicate(imm8 << 8, 16);
    case 6: return (cmode & 1) ? replicate((imm8 << 16) | 0xffff, 32) : replicate((imm8 << 8) | 0xff, 32);
    default:
        if (!(cmode & 1)) {
            if (!op) {
                return replicate(imm8, 8);
            }
            uint64_t r = 0;
            for (int i = 0; i < 8; ++i) {
                if (imm8 >> i & 1) {
                    r |= uint64_t(0xff) << (8 * i);
                }
            }
            return r;
        }
        return op ? fp_immediate(unsigned(imm8), 3) : replicate(fp_immediate(unsigned(imm8), 2), 32);
    }
}

std::string vector_constant(uint64_t low, uint64_t high) {
    return "((ts_u64){" + hex(low) + ", " + hex(high) + "})";
}

// shufflevector over two operands with constant lanes
std::string shuffle(const std::string& a, const std::string& b, const std::vector<int>& lanes) {
    std::string text = "__builtin_shufflevector(" + a + ", " + b;
    for (int l : lanes) {
        text += ", " + str(l);
    }
    return text + ")";
}

const char* rounding_mode(unsigned rmode, bool away) {
    if (away) {
        return "ts_away";
    }
    static const char* modes[] = {"ts_nearest", "ts_plus", "ts_minus", "ts_zero"};
    return modes[rmode];
}

class Lifter {
public:
    Lifter(const Instruction& in, uint64_t pc) : in_(in), pc_(pc), name_(a64::simd_name(in)) {}

    std::string run() {
        switch (in_.op) {
        case Op::fp_1src: return fp_1src();
        case Op::fp_2src: return fp_2src();
        case Op::fp_3src: return fp_3src();
        case Op::fp_compare: return fp_compare();
        case Op::fp_ccmp: return fp_ccmp();
        case Op::fp_csel: return fp_csel();
        case Op::fp_imm: return fset(in_.rd(), ftype_size(in_.bits(23, 22)), half_bits(fp_immediate(in_.bits(20, 13), ftype_size(in_.bits(23, 22)))));
        case Op::fp_int: return fp_int();
        case Op::fp_fixed: return fp_fixed();
        case Op::simd_three_same: return three_same();
        case Op::simd_three_different: return three_different();
        case Op::simd_two_misc: return two_misc();
        case Op::simd_across: return across();
        case Op::simd_copy: return copy();
        case Op::simd_modified_imm: return modified_imm();
        case Op::simd_shift_imm: return shift_imm(false);
        case Op::simd_scalar_shift_imm: return shift_imm(true);
        case Op::simd_indexed: return indexed(false);
        case Op::simd_scalar_indexed: return indexed(true);
        case Op::simd_extract: return extract();
        case Op::simd_permute: return permute();
        case Op::simd_table: return table();
        case Op::simd_three_same_extra: return three_same_extra();
        case Op::simd_scalar_three_same: return scalar_three_same();
        case Op::simd_scalar_three_different: return scalar_three_different();
        case Op::simd_scalar_two_misc: return scalar_two_misc();
        case Op::simd_scalar_pairwise: return scalar_pairwise();
        case Op::simd_scalar_copy: return scalar_copy();
        case Op::simd_aes: return "tsugiki_aes(s, " + str(in_.bits(16, 12)) + ", " + str(in_.rd()) + ", " + str(in_.rn()) + ");";
        case Op::simd_sha3reg: return "tsugiki_sha(s, " + str(in_.bits(14, 12)) + ", " + str(in_.rd()) + ", " + str(in_.rn()) + ", " + str(in_.rm()) + ");";
        case Op::simd_sha2reg: return "tsugiki_sha(s, " + str(16 + in_.bits(16, 12)) + ", " + str(in_.rd()) + ", " + str(in_.rn()) + ", 0);";
        default: return {};
        }
    }

private:
    // fp constants as typed bits
    std::string half_bits(uint64_t bits) {
        int size = ftype_size(in_.bits(23, 22));
        return size == 2 ? "ts_float(" + hex(bits) + ")" : size == 3 ? "ts_double(" + hex(bits) + ")" : "ts_half(" + hex(bits) + ")";
    }

    const std::string& name() const { return name_; }
    bool is(const char* n) const { return name_ == n; }

    std::string fp_1src() {
        int size = ftype_size(in_.bits(23, 22));
        std::string a = fget(in_.rn(), size);
        unsigned opcode = in_.bits(20, 15);
        auto out = [&](const std::string& v) { return fset(in_.rd(), size, v); };
        switch (opcode) {
        case 0: return out(a);
        case 1: return out("__builtin_elementwise_abs(" + a + ")");
        case 2: return out("-" + a);
        case 3: return out("__builtin_elementwise_sqrt(" + a + ")");
        case 4: case 5: case 7: {
            int to = ftype_size(opcode & 3);
            return fset(in_.rd(), to, "(" + std::string(fe[to]) + ")" + a);
        }
        case 8: return out("__builtin_elementwise_roundeven(" + a + ")");
        case 9: return out("__builtin_elementwise_ceil(" + a + ")");
        case 10: return out("__builtin_elementwise_floor(" + a + ")");
        case 11: return out("__builtin_elementwise_trunc(" + a + ")");
        case 12: return out("__builtin_elementwise_round(" + a + ")");
        case 14: case 15: return out("__builtin_elementwise_roundeven(" + a + ")");
        default: return {};
        }
    }

    std::string fp_2src() {
        int size = ftype_size(in_.bits(23, 22));
        std::string a = fget(in_.rn(), size), b = fget(in_.rm(), size), v;
        switch (in_.bits(15, 12)) {
        case 0: v = a + " * " + b; break;
        case 1: v = a + " / " + b; break;
        case 2: v = a + " + " + b; break;
        case 3: v = a + " - " + b; break;
        case 4: v = "__builtin_elementwise_maximum(" + a + ", " + b + ")"; break;
        case 5: v = "__builtin_elementwise_minimum(" + a + ", " + b + ")"; break;
        case 6: v = "__builtin_elementwise_maximumnum(" + a + ", " + b + ")"; break;
        case 7: v = "__builtin_elementwise_minimumnum(" + a + ", " + b + ")"; break;
        case 8: v = "-(" + a + " * " + b + ")"; break;
        default: return {};
        }
        return fset(in_.rd(), size, v);
    }

    std::string fp_3src() {
        int size = ftype_size(in_.bits(23, 22));
        std::string n = fget(in_.rn(), size), m = fget(in_.rm(), size), a = fget(in_.ra(), size), v;
        switch (in_.bit(21) * 2 + in_.bit(15)) {
        case 0: v = "__builtin_elementwise_fma(" + n + ", " + m + ", " + a + ")"; break;
        case 1: v = "__builtin_elementwise_fma(-" + n + ", " + m + ", " + a + ")"; break;
        case 2: v = "__builtin_elementwise_fma(-" + n + ", " + m + ", -" + a + ")"; break;
        default: v = "__builtin_elementwise_fma(" + n + ", " + m + ", -" + a + ")"; break;
        }
        return fset(in_.rd(), size, v);
    }

    std::string fp_compare() {
        int size = ftype_size(in_.bits(23, 22));
        std::string b = in_.bit(3) ? "(" + std::string(fe[size]) + ")0" : fget(in_.rm(), size);
        return "ts_fcmp(s, " + fget(in_.rn(), size) + ", " + b + ");";
    }

    std::string fp_ccmp() {
        int size = ftype_size(in_.bits(23, 22));
        return "if (" + condition(in_.bits(15, 12)) + ") ts_fcmp(s, " + fget(in_.rn(), size) + ", " + fget(in_.rm(), size) + "); else ts_nzcv(s, " +
               str(in_.bits(3, 0)) + ");";
    }

    std::string fp_csel() {
        int size = ftype_size(in_.bits(23, 22));
        return fset(in_.rd(), size, condition(in_.bits(15, 12)) + " ? " + fget(in_.rn(), size) + " : " + fget(in_.rm(), size));
    }

    std::string fp_int() {
        unsigned ftype = in_.bits(23, 22), rmode = in_.bits(20, 19), opcode = in_.bits(18, 16);
        bool sf = in_.sf();
        int d = in_.rd(), n = in_.rn();
        if (opcode == 7) {
            // general to fp bits
            if (rmode == 1) {
                return "s->v[" + str(d) + "].hi = " + get(n, true) + ";";
            }
            std::string mask = ftype == 3 ? " & 0xffff" : ftype == 0 ? " & 0xffffffffull" : "";
            return "ts_wbits(s, " + str(d) + ", " + get(n, true) + mask + ");";
        }
        if (opcode == 6) {
            if (rmode == 1) {
                return set(d, true, "s->v[" + str(n) + "].hi");
            }
            if (rmode == 3) {
                return set(d, false, "(uint32_t)tsugiki_fjcvtzs(s, ts_fd(s, " + str(n) + "))");
            }
            std::string mask = ftype == 3 ? " & 0xffff" : ftype == 0 ? " & 0xffffffffull" : "";
            return set(d, sf, "s->v[" + str(n) + "].lo" + mask);
        }
        int size = ftype_size(ftype);
        if (opcode == 2 || opcode == 3) {
            bool is_signed = opcode == 2;
            std::string source = std::string(is_signed ? (sf ? "(int64_t)" : "(int32_t)") : (sf ? "(uint64_t)" : "(uint32_t)")) + get(n, true);
            return fset(d, size, "(" + std::string(fe[size]) + ")(" + source + ")");
        }
        bool is_signed = !(opcode & 1);
        const char* type = is_signed ? (sf ? "int64_t" : "int32_t") : (sf ? "uint64_t" : "uint32_t");
        return set(d, sf, std::string("ts_to_int<") + type + ", " + rounding_mode(rmode, opcode >= 4) + ">(" + fget(n, size) + ")");
    }

    std::string fp_fixed() {
        unsigned ftype = in_.bits(23, 22), opcode = in_.bits(20, 16);
        bool sf = in_.sf();
        int size = ftype_size(ftype), fbits = 64 - int(in_.bits(15, 10));
        std::string scale = "(" + std::string(fe[size]) + ")0x1p" + str(fbits), unscale = "(" + std::string(fe[size]) + ")0x1p-" + str(fbits);
        if (opcode == 0b00010 || opcode == 0b00011) {
            bool is_signed = opcode == 0b00010;
            std::string source = std::string(is_signed ? (sf ? "(int64_t)" : "(int32_t)") : (sf ? "(uint64_t)" : "(uint32_t)")) + get(in_.rn(), true);
            return fset(in_.rd(), size, "(" + std::string(fe[size]) + ")(" + source + ") * " + unscale);
        }
        bool is_signed = opcode == 0b11000;
        const char* type = is_signed ? (sf ? "int64_t" : "int32_t") : (sf ? "uint64_t" : "uint32_t");
        return set(in_.rd(), sf, std::string("ts_to_int<") + type + ", ts_zero>(" + fget(in_.rn(), size) + " * " + scale + ")");
    }

    // ---- vectors ----

    std::string three_same() {
        unsigned opcode = in_.bits(15, 11), size = in_.bits(23, 22);
        bool q = in_.bit(30), u = in_.bit(29);
        int d = in_.rd(), n = in_.rn(), m = in_.rm();
        if (opcode == 3) {
            std::string a = reg(n, "ts_u64"), b = reg(m, "ts_u64"), dd = reg(d, "ts_u64"), v;
            switch (u * 4 + size) {
            case 0: v = a + " & " + b; break;
            case 1: v = a + " & ~" + b; break;
            case 2: v = a + " | " + b; break;
            case 3: v = a + " | ~" + b; break;
            case 4: v = a + " ^ " + b; break;
            case 5: v = "(" + dd + " & " + a + ") | (~" + dd + " & " + b + ")"; break;
            case 6: v = "(" + dd + " & ~" + b + ") | (" + a + " & " + b + ")"; break;
            default: v = "(" + dd + " & " + b + ") | (" + a + " & ~" + b + ")"; break;
            }
            return store(d, q, v);
        }
        if (opcode >= 0b11000) {
            return fp_three_same(opcode, size, q, u, d, n, m);
        }
        bool is_signed = !u;
        // ops where the signedness is fixed regardless of u
        if (is("add") || is("sub") || is("mul") || is("mla") || is("mls") || is("cmtst") || is("cmeq") || is("addp") || is("pmul")) {
            is_signed = false;
        }
        if (is("sqdmulh") || is("sqrdmulh")) {
            is_signed = true;
        }
        std::string t = is_signed ? sv[size] : uv[size], e = is_signed ? se[size] : ue[size];
        std::string a = reg(n, t), b = reg(m, t), dd = reg(d, t), v;
        const std::string& op = name();
        if (op == "shadd" || op == "uhadd") v = "ts_vhadd<" + t + ", " + e + ">(" + a + ", " + b + ", 0)";
        else if (op == "srhadd" || op == "urhadd") v = "ts_vhadd<" + t + ", " + e + ">(" + a + ", " + b + ", 1)";
        else if (op == "shsub" || op == "uhsub") v = "ts_vhsub<" + t + ", " + e + ">(" + a + ", " + b + ")";
        else if (op == "sqadd" || op == "uqadd") v = "__builtin_elementwise_add_sat(" + a + ", " + b + ")";
        else if (op == "sqsub" || op == "uqsub") v = "__builtin_elementwise_sub_sat(" + a + ", " + b + ")";
        else if (op == "cmgt" || op == "cmhi") v = "(" + a + " > " + b + ")";
        else if (op == "cmge" || op == "cmhs") v = "(" + a + " >= " + b + ")";
        else if (op == "sshl" || op == "ushl") v = "ts_vshl<" + t + ", " + e + ", false, false>(" + a + ", " + b + ")";
        else if (op == "sqshl" || op == "uqshl") v = "ts_vshl<" + t + ", " + e + ", false, true>(" + a + ", " + b + ")";
        else if (op == "srshl" || op == "urshl") v = "ts_vshl<" + t + ", " + e + ", true, false>(" + a + ", " + b + ")";
        else if (op == "sqrshl" || op == "uqrshl") v = "ts_vshl<" + t + ", " + e + ", true, true>(" + a + ", " + b + ")";
        else if (op == "smax" || op == "umax") v = "__builtin_elementwise_max(" + a + ", " + b + ")";
        else if (op == "smin" || op == "umin") v = "__builtin_elementwise_min(" + a + ", " + b + ")";
        else if (op == "sabd" || op == "uabd") v = "ts_vabd(" + a + ", " + b + ")";
        else if (op == "saba" || op == "uaba") v = dd + " + ts_vabd(" + a + ", " + b + ")";
        else if (op == "add") v = a + " + " + b;
        else if (op == "sub") v = a + " - " + b;
        else if (op == "cmtst") v = "((" + a + " & " + b + ") != 0)";
        else if (op == "cmeq") v = "(" + a + " == " + b + ")";
        else if (op == "mla") v = dd + " + " + a + " * " + b;
        else if (op == "mls") v = dd + " - " + a + " * " + b;
        else if (op == "mul") v = a + " * " + b;
        else if (op == "pmul") {
            return "{ ts_u8 a = " + a + ", b = " + b + ", r; for (int i = 0; i < 16; ++i) r[i] = (uint8_t)ts_pmul8(a[i], b[i]); " + store(d, q, "r") + " }";
        } else if (op == "smaxp" || op == "umaxp") v = "ts_vpair<" + t + ", " + e + ">(" + a + ", " + b + ", [](" + e + " x, " + e + " y) { return x > y ? x : y; }, " + (q ? "true" : "false") + ")";
        else if (op == "sminp" || op == "uminp") v = "ts_vpair<" + t + ", " + e + ">(" + a + ", " + b + ", [](" + e + " x, " + e + " y) { return x < y ? x : y; }, " + (q ? "true" : "false") + ")";
        else if (op == "addp") v = "ts_vpair<" + t + ", " + e + ">(" + a + ", " + b + ", [](" + e + " x, " + e + " y) { return (" + e + ")(x + y); }, " + (q ? "true" : "false") + ")";
        else if (op == "sqdmulh") v = "ts_vqdmulh<" + t + ", " + e + ", false>(" + a + ", " + b + ")";
        else if (op == "sqrdmulh") v = "ts_vqdmulh<" + t + ", " + e + ", true>(" + a + ", " + b + ")";
        else return {};
        return store(d, q, v);
    }

    std::string fp_three_same(unsigned opcode, unsigned size, bool q, bool u, int d, int n, int m) {
        (void)opcode;
        (void)u;
        int lane = (size & 1) ? 3 : 2;
        std::string t = fv[lane], e = fe[lane];
        std::string a = reg(n, t), b = reg(m, t), dd = reg(d, t), v;
        const std::string& op = name();
        std::string qs = q ? "true" : "false";
        auto pair = [&](const std::string& body) { return "ts_vpair<" + t + ", " + e + ">(" + a + ", " + b + ", [](" + e + " x, " + e + " y) { return " + body + "; }, " + qs + ")"; };
        if (op == "fmaxnm") v = "__builtin_elementwise_maximumnum(" + a + ", " + b + ")";
        else if (op == "fminnm") v = "__builtin_elementwise_minimumnum(" + a + ", " + b + ")";
        else if (op == "fmla") v = "__builtin_elementwise_fma(" + a + ", " + b + ", " + dd + ")";
        else if (op == "fmls") v = "__builtin_elementwise_fma(-" + a + ", " + b + ", " + dd + ")";
        else if (op == "fadd") v = a + " + " + b;
        else if (op == "fsub") v = a + " - " + b;
        else if (op == "fmulx") v = "ts_vfmulx<" + t + ", " + e + ">(" + a + ", " + b + ")";
        else if (op == "fcmeq") v = "(" + a + " == " + b + ")";
        else if (op == "fmax") v = "__builtin_elementwise_maximum(" + a + ", " + b + ")";
        else if (op == "fmin") v = "__builtin_elementwise_minimum(" + a + ", " + b + ")";
        else if (op == "frecps") v = "ts_vfrecps<" + t + ", " + e + ">(" + a + ", " + b + ")";
        else if (op == "frsqrts") v = "ts_vfrsqrts<" + t + ", " + e + ">(" + a + ", " + b + ")";
        else if (op == "fmaxnmp") v = pair("__builtin_elementwise_maximumnum(x, y)");
        else if (op == "fminnmp") v = pair("__builtin_elementwise_minimumnum(x, y)");
        else if (op == "faddp") v = pair("x + y");
        else if (op == "fmaxp") v = pair("__builtin_elementwise_maximum(x, y)");
        else if (op == "fminp") v = pair("__builtin_elementwise_minimum(x, y)");
        else if (op == "fabd") v = "__builtin_elementwise_abs(" + a + " - " + b + ")";
        else if (op == "fmul") v = a + " * " + b;
        else if (op == "fcmge") v = "(" + a + " >= " + b + ")";
        else if (op == "fcmgt") v = "(" + a + " > " + b + ")";
        else if (op == "facge") v = "(__builtin_elementwise_abs(" + a + ") >= __builtin_elementwise_abs(" + b + "))";
        else if (op == "facgt") v = "(__builtin_elementwise_abs(" + a + ") > __builtin_elementwise_abs(" + b + "))";
        else if (op == "fdiv") v = a + " / " + b;
        else return {};
        return store(d, q, v);
    }

    // widen a half register into the double-width lane type
    std::string widen(int r, bool upper, unsigned size, bool is_signed) {
        std::string narrow = is_signed ? sh[size] : uh[size];
        std::string wide = is_signed ? sv[size + 1] : uv[size + 1];
        return "__builtin_convertvector(" + half(r, upper, narrow) + ", " + wide + ")";
    }

    std::string three_different() {
        unsigned size = in_.bits(23, 22);
        bool q = in_.bit(30), u = in_.bit(29), is_signed = !u;
        int d = in_.rd(), n = in_.rn(), m = in_.rm();
        std::string op = name();
        if (op.back() == '2') {
            op.pop_back();
        }
        if (size == 3 || op == "pmull") {
            if (op != "pmull") {
                return {};
            }
            if (size == 3) {
                std::string lane = q ? ".hi" : ".lo";
                return "{ unsigned __int128 r = ts_pmul64(s->v[" + str(n) + "]" + lane + ", s->v[" + str(m) + "]" + lane + "); s->v[" + str(d) +
                       "].lo = (uint64_t)r; s->v[" + str(d) + "].hi = (uint64_t)(r >> 64); }";
            }
            return "{ ts_u8h a = " + half(n, q, "ts_u8h") + ", b = " + half(m, q, "ts_u8h") + "; ts_u16 r; for (int i = 0; i < 8; ++i) r[i] = (uint16_t)ts_pmul8(a[i], b[i]); " +
                   store(d, true, "r") + " }";
        }
        std::string wide = is_signed ? sv[size + 1] : uv[size + 1];
        std::string wide_e = is_signed ? se[size + 1] : ue[size + 1];
        std::string a = widen(n, q, size, is_signed), b = widen(m, q, size, is_signed);
        std::string aw = reg(n, wide), dd = reg(d, wide);
        if (op == "saddl" || op == "uaddl") return store(d, true, a + " + " + b);
        if (op == "saddw" || op == "uaddw") return store(d, true, aw + " + " + b);
        if (op == "ssubl" || op == "usubl") return store(d, true, a + " - " + b);
        if (op == "ssubw" || op == "usubw") return store(d, true, aw + " - " + b);
        if (op == "sabdl" || op == "uabdl") return store(d, true, "ts_vabd(" + a + ", " + b + ")");
        if (op == "sabal" || op == "uabal") return store(d, true, dd + " + ts_vabd(" + a + ", " + b + ")");
        if (op == "smull" || op == "umull") return store(d, true, a + " * " + b);
        if (op == "smlal" || op == "umlal") return store(d, true, dd + " + " + a + " * " + b);
        if (op == "smlsl" || op == "umlsl") return store(d, true, dd + " - " + a + " * " + b);
        if (op == "addhn" || op == "raddhn" || op == "subhn" || op == "rsubhn") {
            std::string uw = uv[size + 1], bits = str(8 << size);
            std::string sum = op.find("add") != std::string::npos ? reg(n, uw) + " + " + reg(m, uw) : reg(n, uw) + " - " + reg(m, uw);
            std::string round = op[0] == 'r' ? " + (" + std::string(ue[size + 1]) + ")" + hex(uint64_t(1) << ((8 << size) - 1)) : "";
            return store_narrow(d, q, "__builtin_convertvector((" + sum + round + ") >> " + bits + ", " + std::string(uh[size]) + ")");
        }
        if (op == "sqdmull" || op == "sqdmlal" || op == "sqdmlsl") {
            std::string acc = op == "sqdmull" ? "0" : op == "sqdmlal" ? "(__int128)acc[i]" : "(__int128)acc[i]";
            std::string sign = op == "sqdmlsl" ? " - " : " + ";
            return "{ " + wide + " x = " + a + ", y = " + b + ", acc = " + dd + ", r; for (unsigned i = 0; i < sizeof(r) / sizeof(" + wide_e +
                   "); ++i) { __int128 p = ts_clamp<" + wide_e + ">((__int128)x[i] * y[i] * 2); r[i] = ts_clamp<" + wide_e + ">(" + acc + sign + "p); } " +
                   store(d, true, "r") + " }";
        }
        return {};
    }

    std::string two_misc() {
        unsigned size = in_.bits(23, 22), opcode = in_.bits(16, 12);
        bool q = in_.bit(30), u = in_.bit(29);
        int d = in_.rd(), n = in_.rn();
        std::string op = name();
        bool second = q && op.back() == '2' && a64::has_second_half(in_.op, op.substr(0, op.size() - 1));
        if (second) {
            op.pop_back();
        }
        if (opcode >= 0b01100 && opcode != 0b10010 && opcode != 0b10011 && opcode != 0b10100 && !(opcode == 0b10110 && op == "fcvtn") &&
            op != "fcvtn" && op != "fcvtl" && op != "fcvtxn" && op != "bfcvtn") {
            if (op == "abs" || op == "neg" || op == "cmlt" || op == "cmge" || op == "cmgt" || op == "cmle" || op == "cmeq") {
                // integer compares live here too
            } else {
                return fp_two_misc(size, q, u, d, n);
            }
        }
        std::string t = sv[size], e = se[size], ut = uv[size];
        std::string a = reg(n, t), ua = reg(n, ut), dd = reg(d, t);
        if (op == "rev64" || op == "rev32" || op == "rev16") {
            int container = op == "rev64" ? 8 : op == "rev32" ? 4 : 2;
            int lane = 1 << size;
            std::vector<int> lanes;
            int count = 16 / lane;
            for (int i = 0; i < count; ++i) {
                int per = container / lane, group = i / per, within = i % per;
                lanes.push_back(group * per + (per - 1 - within));
            }
            return store(d, q, shuffle(ua, ua, lanes));
        }
        if (op == "cls") return store(d, q, "ts_vcls<" + t + ", " + e + ">(" + a + ")");
        if (op == "clz") return store(d, q, "ts_vclz<" + ut + ", " + std::string(ue[size]) + ">(" + ua + ")");
        if (op == "cnt") return store(d, q, "__builtin_elementwise_popcount(" + reg(n, "ts_u8") + ")");
        if (op == "not") return store(d, q, "~" + reg(n, "ts_u8"));
        if (op == "rbit") return store(d, q, "__builtin_elementwise_bitreverse(" + reg(n, "ts_u8") + ")");
        if (op == "abs") return store(d, q, "__builtin_elementwise_abs(" + a + ")");
        if (op == "neg") return store(d, q, "-" + a);
        if (op == "sqabs") return store(d, q, "ts_vsat<" + t + ", " + e + ", " + t + ", " + e + ">(" + a + ")" + " == " + a + " ? __builtin_elementwise_abs(" + a + ") : " + a);
        if (op == "cmgt") return store(d, q, "(" + a + " > 0)");
        if (op == "cmge") return store(d, q, "(" + a + " >= 0)");
        if (op == "cmeq") return store(d, q, "(" + a + " == 0)");
        if (op == "cmle") return store(d, q, "(" + a + " <= 0)");
        if (op == "cmlt") return store(d, q, "(" + a + " < 0)");
        if (op == "saddlp" || op == "uaddlp" || op == "sadalp" || op == "uadalp") {
            bool is_signed = op[0] == 's';
            std::string from = is_signed ? sv[size] : uv[size], from_e = is_signed ? se[size] : ue[size];
            std::string to = is_signed ? sv[size + 1] : uv[size + 1], to_e = is_signed ? se[size + 1] : ue[size + 1];
            std::string acc = op.find("adal") != std::string::npos ? "(" + to_e + ")acc[i] + " : "";
            return "{ " + from + " x = " + reg(n, from) + "; " + to + " acc = " + reg(d, to) + ", r = {}; for (unsigned i = 0; i < " + str((q ? 16 : 8) >> (size + 1)) +
                   "; ++i) r[i] = (" + to_e + ")(" + acc + "(" + to_e + ")x[2 * i] + (" + to_e + ")x[2 * i + 1]); " + store(d, q, "r") + " }";
        }
        if (op == "xtn") {
            return store_narrow(d, second, "__builtin_convertvector(" + reg(n, uv[size + 1]) + ", " + std::string(uh[size]) + ")");
        }
        if (op == "sqxtn" || op == "uqxtn" || op == "sqxtun") {
            bool from_signed = op != "uqxtn", to_signed = op == "sqxtn";
            std::string from = from_signed ? sv[size + 1] : uv[size + 1], from_e = from_signed ? se[size + 1] : ue[size + 1];
            std::string to = to_signed ? sh[size] : uh[size], to_e = to_signed ? se[size] : ue[size];
            return store_narrow(d, second, "ts_vsat<" + to + ", " + to_e + ", " + from + ", " + from_e + ">(" + reg(n, from) + ")");
        }
        if (op == "shll") {
            return store(d, true, widen(n, second, size, false) + " << " + str(8 << size));
        }
        if (op == "suqadd" || op == "usqadd") {
            // accumulate the other signedness, saturating to ours
            bool ours_signed = op == "suqadd";
            std::string mine = ours_signed ? t : ut, mine_e = ours_signed ? e : ue[size];
            std::string other_e = ours_signed ? ue[size] : se[size];
            return "{ " + mine + " acc = " + reg(d, mine) + ", x = " + reg(n, mine) + ", r; for (unsigned i = 0; i < sizeof(r) / sizeof(" + mine_e +
                   "); ++i) r[i] = ts_clamp<" + mine_e + ">((__int128)acc[i] + (__int128)(" + other_e + ")x[i]); " + store(d, q, "r") + " }";
        }
        if (op == "sqneg") {
            return "{ " + t + " x = " + a + ", r; for (unsigned i = 0; i < sizeof(r) / sizeof(" + e + "); ++i) r[i] = ts_clamp<" + e + ">(-(__int128)x[i]); " +
                   store(d, q, "r") + " }";
        }
        if (op == "fcvtn") {
            // d to s or s to h
            int to = size & 1 ? 2 : 1;
            return store_narrow(d, second, "__builtin_convertvector(" + reg(n, fv[to + 1]) + ", " + std::string(fh[to]) + ")");
        }
        if (op == "fcvtl") {
            int from = size & 1 ? 2 : 1;
            return store(d, true, "__builtin_convertvector(" + half(n, second, fh[from]) + ", " + std::string(fv[from + 1]) + ")");
        }
        return {};
    }

    std::string fp_two_misc(unsigned size, bool q, bool u, int d, int n) {
        (void)u;
        int lane = (size & 1) ? 3 : 2;
        std::string t = fv[lane], e = fe[lane];
        std::string it = sv[lane], ie = se[lane], ut = uv[lane], ue_ = ue[lane];
        std::string a = reg(n, t);
        const std::string& op = name();
        auto to_int = [&](const char* mode, bool is_signed) {
            return store(d, q, "ts_to_int_v<" + (is_signed ? it : ut) + ", " + (is_signed ? ie : ue_) + ", " + mode + ">(" + a + ")");
        };
        if (op == "fabs") return store(d, q, "__builtin_elementwise_abs(" + a + ")");
        if (op == "fneg") return store(d, q, "-" + a);
        if (op == "fsqrt") return store(d, q, "__builtin_elementwise_sqrt(" + a + ")");
        if (op == "frintn" || op == "frintx" || op == "frinti") return store(d, q, "__builtin_elementwise_roundeven(" + a + ")");
        if (op == "frintp") return store(d, q, "__builtin_elementwise_ceil(" + a + ")");
        if (op == "frintm") return store(d, q, "__builtin_elementwise_floor(" + a + ")");
        if (op == "frintz") return store(d, q, "__builtin_elementwise_trunc(" + a + ")");
        if (op == "frinta") return store(d, q, "__builtin_elementwise_round(" + a + ")");
        if (op == "fcvtns") return to_int("ts_nearest", true);
        if (op == "fcvtnu") return to_int("ts_nearest", false);
        if (op == "fcvtps") return to_int("ts_plus", true);
        if (op == "fcvtpu") return to_int("ts_plus", false);
        if (op == "fcvtms") return to_int("ts_minus", true);
        if (op == "fcvtmu") return to_int("ts_minus", false);
        if (op == "fcvtzs") return to_int("ts_zero", true);
        if (op == "fcvtzu") return to_int("ts_zero", false);
        if (op == "fcvtas") return to_int("ts_away", true);
        if (op == "fcvtau") return to_int("ts_away", false);
        if (op == "scvtf") return store(d, q, "__builtin_convertvector(" + reg(n, it) + ", " + t + ")");
        if (op == "ucvtf") return store(d, q, "__builtin_convertvector(" + reg(n, ut) + ", " + t + ")");
        if (op == "fcmgt") return store(d, q, "(" + a + " > 0)");
        if (op == "fcmge") return store(d, q, "(" + a + " >= 0)");
        if (op == "fcmeq") return store(d, q, "(" + a + " == 0)");
        if (op == "fcmle") return store(d, q, "(" + a + " <= 0)");
        if (op == "fcmlt") return store(d, q, "(" + a + " < 0)");
        if (op == "frecpe" || op == "frsqrte") {
            std::string f = std::string(op == "frecpe" ? "tsugiki_frecpe_" : "tsugiki_frsqrte_") + (lane == 3 ? "d" : "s");
            return "{ " + t + " x = " + a + ", r; for (unsigned i = 0; i < sizeof(r) / sizeof(" + e + "); ++i) r[i] = " + f + "(x[i]); " + store(d, q, "r") + " }";
        }
        if (op == "urecpe" || op == "ursqrte") {
            std::string f = op == "urecpe" ? "tsugiki_urecpe" : "tsugiki_ursqrte";
            return "{ ts_u32 x = " + reg(n, "ts_u32") + ", r; for (int i = 0; i < 4; ++i) r[i] = " + f + "(x[i]); " + store(d, q, "r") + " }";
        }
        return {};
    }

    std::string across() {
        unsigned size = in_.bits(23, 22);
        bool q = in_.bit(30);
        int d = in_.rd(), n = in_.rn();
        const std::string& op = name();
        if (op[0] == 'f') {
            // single precision pairwise tree, like the arm arm
            if (!in_.bit(29)) {
                return {};
            }
            std::string f = op == "fmaxv" ? "__builtin_elementwise_maximum" : op == "fminv" ? "__builtin_elementwise_minimum"
                          : op == "fmaxnmv" ? "__builtin_elementwise_maximumnum" : "__builtin_elementwise_minimumnum";
            return "{ ts_f32 x = " + reg(n, "ts_f32") + "; ts_ws(s, " + str(d) + ", " + f + "(" + f + "(x[0], x[1]), " + f + "(x[2], x[3]))); }";
        }
        bool is_signed = op[0] == 's' || op == "addv";
        if (op == "addv") {
            is_signed = false;
        }
        std::string t = q ? (is_signed ? sv[size] : uv[size]) : (is_signed ? sh[size] : uh[size]);
        std::string source = q ? reg(n, t) : half(n, false, t);
        if (op == "addv") return "ts_wbits(s, " + str(d) + ", (" + std::string(ue[size]) + ")__builtin_reduce_add(" + source + "));";
        if (op == "smaxv" || op == "umaxv") return "ts_wbits(s, " + str(d) + ", (" + std::string(ue[size]) + ")__builtin_reduce_max(" + source + "));";
        if (op == "sminv" || op == "uminv") return "ts_wbits(s, " + str(d) + ", (" + std::string(ue[size]) + ")__builtin_reduce_min(" + source + "));";
        if (op == "saddlv" || op == "uaddlv") {
            std::string wide = is_signed ? se[size + 1] : ue[size + 1];
            return "{ " + t + " x = " + source + "; " + wide + " r = 0; for (unsigned i = 0; i < sizeof(x) / sizeof(x[0]); ++i) r += (" + wide + ")x[i]; ts_wbits(s, " + str(d) +
                   ", (" + std::string(ue[size + 1]) + ")r); }";
        }
        return {};
    }

    std::string copy() {
        unsigned imm5 = in_.bits(20, 16), imm4 = in_.bits(14, 11);
        bool q = in_.bit(30), op = in_.bit(29);
        int d = in_.rd(), n = in_.rn();
        unsigned size = __builtin_ctz(imm5), index = imm5 >> (size + 1);
        std::string t = uv[size], e = ue[size];
        int lanes = (q ? 16 : 8) >> size;
        if (op) {
            unsigned from = imm4 >> size;
            return "{ " + t + " r = " + reg(d, t) + "; r[" + str(index) + "] = " + reg(n, t) + "[" + str(from) + "]; " + store(d, true, "r") + " }";
        }
        switch (imm4) {
        case 0: return store(d, q, shuffle(reg(n, t), reg(n, t), std::vector<int>(16 >> size, int(index))));
        case 1: return store(d, q, "ts_splat<" + t + ", " + e + ">((" + e + ")" + get(n, true) + ")");
        case 3: return "{ " + t + " r = " + reg(d, t) + "; r[" + str(index) + "] = (" + e + ")" + get(n, true) + "; " + store(d, true, "r") + " }";
        case 5: return set(d, q, "(int64_t)(" + std::string(se[size]) + ")" + reg(n, t) + "[" + str(index) + "]");
        case 7: return set(d, q, "(uint64_t)" + reg(n, t) + "[" + str(index) + "]");
        default: (void)lanes; return {};
        }
    }

    std::string modified_imm() {
        unsigned cmode = in_.bits(15, 12);
        bool q = in_.bit(30), op = in_.bit(29);
        int d = in_.rd();
        uint64_t imm8 = (in_.bits(18, 16) << 5) | in_.bits(9, 5);
        if (in_.bit(11)) {
            // fmov .h
            uint64_t h = fp_immediate(unsigned(imm8), 1);
            uint64_t all = replicate(h, 16);
            return store(d, q, vector_constant(all, q ? all : 0));
        }
        uint64_t imm = simd_immediate(op, cmode, imm8);
        const std::string& name_ = name();
        if (name_ == "mvni") {
            imm = ~imm;
        }
        if (name_ == "movi" || name_ == "mvni" || name_ == "fmov") {
            return store(d, q, vector_constant(imm, q ? imm : 0));
        }
        std::string c = vector_constant(imm, imm);
        if (name_ == "orr") return store(d, q, reg(d, "ts_u64") + " | " + c);
        return store(d, q, reg(d, "ts_u64") + " & ~" + c);
    }

    std::string shift_imm(bool scalar) {
        unsigned immh = in_.bits(22, 19), immb = in_.bits(18, 16);
        unsigned size = 31 - __builtin_clz(immh);
        int esize = 8 << size;
        int immhb = int((immh << 3) | immb);
        int right = 2 * esize - immhb, left = immhb - esize;
        bool q = scalar ? true : in_.bit(30);
        int d = in_.rd(), n = in_.rn();
        std::string op = name();
        bool second = !scalar && op.back() == '2';
        if (second) {
            op.pop_back();
        }
        std::string t = sv[size], e = se[size], ut = uv[size], ue_ = ue[size];
        // scalar ones use lane 0 and zero the rest
        auto out = [&](const std::string& v) {
            if (scalar) {
                return "ts_wbits(s, " + str(d) + ", (uint64_t)(" + ue_ + ")((" + ut + ")(" + v + "))[0]);";
            }
            return store(d, q, v);
        };
        std::string a = reg(n, t), ua = reg(n, ut), dd = reg(d, t), ud = reg(d, ut);
        auto sshr = [&]() { return "(" + a + " >> " + str(right >= esize ? esize - 1 : right) + ")"; };
        auto ushr = [&]() { return right >= esize ? "(" + ut + "){}" : "(" + ua + " >> " + str(right) + ")"; };
        if (op == "sshr") return out(sshr());
        if (op == "ushr") return out(ushr());
        if (op == "ssra") return out(dd + " + " + sshr());
        if (op == "usra") return out(ud + " + " + ushr());
        if (op == "srshr") return out("ts_vrshr<" + t + ", " + e + ">(" + a + ", " + str(right) + ")");
        if (op == "urshr") return out("ts_vrshr<" + ut + ", " + ue_ + ">(" + ua + ", " + str(right) + ")");
        if (op == "srsra") return out(dd + " + ts_vrshr<" + t + ", " + e + ">(" + a + ", " + str(right) + ")");
        if (op == "ursra") return out(ud + " + ts_vrshr<" + ut + ", " + ue_ + ">(" + ua + ", " + str(right) + ")");
        if (op == "shl") return out(ua + " << " + str(left));
        if (op == "sli") {
            uint64_t mask = left >= 64 ? 0 : (~uint64_t(0) << left) & (esize == 64 ? ~uint64_t(0) : (uint64_t(1) << esize) - 1);
            std::string m = "(" + ut + "){} + (" + ue_ + ")" + hex(mask);
            return out("(" + ud + " & ~(" + m + ")) | (" + ua + " << " + str(left) + ")");
        }
        if (op == "sri") {
            if (right >= esize) {
                return out(ud);
            }
            uint64_t mask = (esize == 64 ? ~uint64_t(0) : (uint64_t(1) << esize) - 1) >> right;
            std::string m = "(" + ut + "){} + (" + ue_ + ")" + hex(mask);
            return out("(" + ud + " & ~(" + m + ")) | (" + ua + " >> " + str(right) + ")");
        }
        if (op == "sqshl") return out("ts_vqshl<" + t + ", " + e + ", " + e + ">(" + a + ", " + str(left) + ")");
        if (op == "uqshl") return out("ts_vqshl<" + ut + ", " + ue_ + ", " + ue_ + ">(" + ua + ", " + str(left) + ")");
        if (op == "sqshlu") return out("ts_vqshl<" + t + ", " + e + ", " + ue_ + ">(" + a + ", " + str(left) + ")");
        if (op == "sshll" || op == "ushll") {
            bool is_signed = op == "sshll";
            return store(d, true, widen(n, second, size, is_signed) + " << " + str(left));
        }
        if (op == "scvtf" || op == "ucvtf" || op == "fcvtzs" || op == "fcvtzu") {
            int fbits = right;
            if (size < 1) {
                return {};
            }
            std::string ft = fv[size], fe_ = fe[size];
            if (op == "scvtf" || op == "ucvtf") {
                std::string from = op == "scvtf" ? t : ut;
                return out("__builtin_convertvector(" + reg(n, from) + ", " + ft + ") * (" + fe_ + ")0x1p-" + str(fbits));
            }
            bool is_signed = op == "fcvtzs";
            return out("ts_to_int_v<" + (is_signed ? t : ut) + ", " + (is_signed ? e : ue_) + ", ts_zero>(" + reg(n, ft) + " * (" + fe_ + ")0x1p" + str(fbits) + ")");
        }
        // narrowing: the source lanes are twice as wide
        if (op == "shrn" || op == "rshrn" || op == "sqshrn" || op == "uqshrn" || op == "sqrshrn" || op == "uqrshrn" || op == "sqshrun" || op == "sqrshrun") {
            int shift = right;
            bool round = op.find("rshr") != std::string::npos;
            bool from_signed = op[0] == 's', saturate = op[1] == 'q', to_signed = op == "sqshrn" || op == "sqrshrn";
            std::string from = from_signed ? sv[size + 1] : uv[size + 1], from_e = from_signed ? se[size + 1] : ue[size + 1];
            std::string shifted = round ? "ts_vrshr<" + from + ", " + from_e + ">(" + reg(n, from) + ", " + str(shift) + ")" : "(" + reg(n, from) + " >> " + str(shift) + ")";
            if (op == "shrn" || op == "rshrn") {
                shifted = round ? "ts_vrshr<" + std::string(uv[size + 1]) + ", " + ue[size + 1] + ">(" + reg(n, uv[size + 1]) + ", " + str(shift) + ")"
                                : "(" + reg(n, uv[size + 1]) + " >> " + str(shift) + ")";
            }
            std::string to = to_signed ? sh[size] : uh[size], to_e = to_signed ? se[size] : ue[size];
            std::string narrow = saturate ? "ts_vsat<" + to + ", " + to_e + ", " + from + ", " + from_e + ">(" + shifted + ")"
                                          : "__builtin_convertvector(" + shifted + ", " + std::string(uh[size]) + ")";
            if (scalar) {
                return "ts_wbits(s, " + str(d) + ", (uint64_t)(" + std::string(ue[size]) + ")((" + std::string(uh[size]) + ")(" + narrow + "))[0]);";
            }
            return store_narrow(d, second, narrow);
        }
        return {};
    }

    std::string indexed(bool scalar) {
        unsigned size = in_.bits(23, 22), opcode = in_.bits(15, 12);
        bool q = scalar ? true : in_.bit(30), u = in_.bit(29);
        bool h = in_.bit(11), l = in_.bit(21), mbit = in_.bit(20);
        int d = in_.rd(), n = in_.rn();
        std::string op = name();
        bool second = !scalar && op.back() == '2';
        if (second) {
            op.pop_back();
        }
        bool fp = (!u && (opcode == 1 || opcode == 5 || opcode == 9)) || (u && opcode == 9);
        // which lane of which register
        int m, index;
        int lane = fp ? (size == 0 ? 1 : size == 2 ? 2 : 3) : int(size);
        if (lane == 1) {
            m = int(in_.bits(19, 16));
            index = (h << 2) | (l << 1) | mbit;
        } else if (lane == 2) {
            m = int(in_.bits(20, 16));
            index = (h << 1) | l;
        } else {
            m = int(in_.bits(20, 16));
            index = h;
        }
        if (fp) {
            if (lane == 1) {
                return {};
            }
            std::string t = fv[lane], e = fe[lane];
            std::string a = reg(n, t), dd = reg(d, t);
            std::string b = "ts_splat<" + t + ", " + e + ">(" + reg(m, t) + "[" + str(index) + "])";
            std::string v;
            if (op == "fmul") v = a + " * " + b;
            else if (op == "fmla") v = "__builtin_elementwise_fma(" + a + ", " + b + ", " + dd + ")";
            else if (op == "fmls") v = "__builtin_elementwise_fma(-" + a + ", " + b + ", " + dd + ")";
            else if (op == "fmulx") v = "ts_vfmulx<" + t + ", " + e + ">(" + a + ", " + b + ")";
            else return {};
            if (scalar) {
                return fset(d, lane, "(" + v + ")[0]");
            }
            return store(d, q, v);
        }
        std::string t = sv[size], e = se[size], ut = uv[size];
        if (op == "mul" || op == "mla" || op == "mls") {
            std::string a = reg(n, ut), dd = reg(d, ut), b = "ts_splat<" + ut + ", " + std::string(ue[size]) + ">(" + reg(m, ut) + "[" + str(index) + "])";
            std::string v = op == "mul" ? a + " * " + b : op == "mla" ? dd + " + " + a + " * " + b : dd + " - " + a + " * " + b;
            return store(d, q, v);
        }
        if (op == "sqdmulh" || op == "sqrdmulh") {
            std::string b = "ts_splat<" + t + ", " + e + ">(" + reg(m, t) + "[" + str(index) + "])";
            std::string v = "ts_vqdmulh<" + t + ", " + e + ", " + (op == "sqrdmulh" ? "true" : "false") + ">(" + reg(n, t) + ", " + b + ")";
            if (scalar) {
                return "ts_wbits(s, " + str(d) + ", (uint64_t)(" + std::string(ue[size]) + ")((" + ut + ")(" + v + "))[0]);";
            }
            return store(d, q, v);
        }
        if (op == "smull" || op == "umull" || op == "smlal" || op == "umlal" || op == "smlsl" || op == "umlsl") {
            bool is_signed = op[0] == 's';
            std::string wide = is_signed ? sv[size + 1] : uv[size + 1];
            std::string narrow_e = is_signed ? se[size] : ue[size];
            std::string a = widen(n, second, size, is_signed);
            std::string b = "((" + wide + "){} + (" + std::string(is_signed ? se[size + 1] : ue[size + 1]) + ")(" + narrow_e + ")" + reg(m, is_signed ? t : ut) + "[" +
                            str(index) + "])";
            std::string dd = reg(d, wide);
            std::string v = op.find("mull") != std::string::npos ? a + " * " + b : op.find("mlal") != std::string::npos ? dd + " + " + a + " * " + b : dd + " - " + a + " * " + b;
            return store(d, true, v);
        }
        if (op == "sdot" || op == "udot") {
            bool is_signed = op == "sdot";
            std::string bt = is_signed ? "ts_i8" : "ts_u8", be = is_signed ? "int8_t" : "uint8_t", rt = is_signed ? "ts_i32" : "ts_u32";
            return "{ " + bt + " a = " + reg(n, bt) + ", b = " + reg(m, bt) + "; " + rt + " r = " + reg(d, rt) + "; for (int i = 0; i < " + str(q ? 4 : 2) +
                   "; ++i) for (int k = 0; k < 4; ++k) r[i] += (int32_t)(" + be + ")a[4 * i + k] * (int32_t)(" + be + ")b[4 * " + str(index) + " + k]; " + store(d, q, "r") + " }";
        }
        return {};
    }

    std::string extract() {
        bool q = in_.bit(30);
        int imm = int(in_.bits(14, 11));
        std::vector<int> lanes;
        if (q) {
            for (int i = 0; i < 16; ++i) lanes.push_back(i + imm);
            return store(in_.rd(), true, shuffle(reg(in_.rn(), "ts_u8"), reg(in_.rm(), "ts_u8"), lanes));
        }
        // 8 bytes of n then 8 of m
        for (int i = 0; i < 16; ++i) {
            int k = i + imm;
            lanes.push_back(i < 8 ? (k < 8 ? k : 16 + k - 8) : 0);
        }
        return store(in_.rd(), false, shuffle(reg(in_.rn(), "ts_u8"), reg(in_.rm(), "ts_u8"), lanes));
    }

    std::string permute() {
        unsigned size = in_.bits(23, 22), opcode = in_.bits(14, 12);
        bool q = in_.bit(30);
        int count = (q ? 16 : 8) >> size;
        std::string t = uv[size];
        int total = 16 >> size;
        std::vector<int> lanes(total, 0);
        bool part2 = opcode & 4;
        for (int i = 0; i < count; ++i) {
            switch (opcode & 3) {
            case 1:  // uzp
                lanes[i] = 2 * i + part2 < count ? 2 * i + part2 : total + (2 * i + part2 - count);
                break;
            case 2:  // trn
                lanes[i] = (i & 1) ? total + (i & ~1) + part2 : (i & ~1) + part2;
                break;
            default: {  // zip
                int base = part2 ? count / 2 : 0;
                lanes[i] = (i & 1) ? total + base + i / 2 : base + i / 2;
                break;
            }
            }
        }
        return store(in_.rd(), q, shuffle(reg(in_.rn(), t), reg(in_.rm(), t), lanes));
    }

    std::string table() {
        bool q = in_.bit(30), tbx = in_.bit(12);
        int len = int(in_.bits(14, 13)) + 1;
        std::string fallback = tbx ? reg(in_.rd(), "ts_u8") : "(ts_u8){}";
        return store(in_.rd(), q, "ts_tbl(s, " + str(in_.rn()) + ", " + str(len) + ", " + reg(in_.rm(), "ts_u8") + ", " + fallback + ")");
    }

    std::string three_same_extra() {
        bool q = in_.bit(30);
        const std::string& op = name();
        if (op == "sdot" || op == "udot") {
            bool is_signed = op == "sdot";
            std::string bt = is_signed ? "ts_i8" : "ts_u8", be = is_signed ? "int8_t" : "uint8_t", rt = is_signed ? "ts_i32" : "ts_u32";
            return "{ " + bt + " a = " + reg(in_.rn(), bt) + ", b = " + reg(in_.rm(), bt) + "; " + rt + " r = " + reg(in_.rd(), rt) + "; for (int i = 0; i < " +
                   str(q ? 4 : 2) + "; ++i) for (int k = 0; k < 4; ++k) r[i] += (int32_t)(" + be + ")a[4 * i + k] * (int32_t)(" + be + ")b[4 * i + k]; " +
                   store(in_.rd(), q, "r") + " }";
        }
        return {};
    }

    // ---- scalar advanced simd: lane 0, the rest zeroed ----

    std::string lane0(unsigned size, const std::string& vector_value) {
        return "ts_wbits(s, " + str(in_.rd()) + ", (uint64_t)(" + std::string(ue[size]) + ")((" + std::string(uv[size]) + ")(" + vector_value + "))[0]);";
    }

    std::string scalar_three_same() {
        unsigned size = in_.bits(23, 22);
        int n = in_.rn(), m = in_.rm();
        const std::string& op = name();
        if (op[0] == 'f') {
            int lane = (size & 1) ? 3 : 2;
            std::string a = fget(n, lane), b = fget(m, lane), e = fe[lane];
            std::string ones = lane == 3 ? "~0ull" : "0xffffffffull";
            auto mask = [&](const std::string& c) { return "ts_wbits(s, " + str(in_.rd()) + ", (" + c + ") ? " + ones + " : 0);"; };
            if (op == "fmulx") return fset(in_.rd(), lane, "ts_fmulx<" + e + ">(" + a + ", " + b + ")");
            if (op == "frecps") return fset(in_.rd(), lane, "ts_frecps<" + e + ">(" + a + ", " + b + ")");
            if (op == "frsqrts") return fset(in_.rd(), lane, "ts_frsqrts<" + e + ">(" + a + ", " + b + ")");
            if (op == "fabd") return fset(in_.rd(), lane, "__builtin_elementwise_abs(" + a + " - " + b + ")");
            if (op == "fcmeq") return mask(a + " == " + b);
            if (op == "fcmge") return mask(a + " >= " + b);
            if (op == "fcmgt") return mask(a + " > " + b);
            if (op == "facge") return mask("__builtin_elementwise_abs(" + a + ") >= __builtin_elementwise_abs(" + b + ")");
            if (op == "facgt") return mask("__builtin_elementwise_abs(" + a + ") > __builtin_elementwise_abs(" + b + ")");
            return {};
        }
        std::string t = sv[size], ut = uv[size], e = se[size], ue_ = ue[size];
        std::string a = reg(n, t), b = reg(m, t), ua = reg(n, ut), ub = reg(m, ut);
        if (op == "add") return lane0(size, ua + " + " + ub);
        if (op == "sub") return lane0(size, ua + " - " + ub);
        if (op == "cmgt") return lane0(size, "(" + a + " > " + b + ")");
        if (op == "cmge") return lane0(size, "(" + a + " >= " + b + ")");
        if (op == "cmhi") return lane0(size, "(" + ua + " > " + ub + ")");
        if (op == "cmhs") return lane0(size, "(" + ua + " >= " + ub + ")");
        if (op == "cmeq") return lane0(size, "(" + ua + " == " + ub + ")");
        if (op == "cmtst") return lane0(size, "((" + ua + " & " + ub + ") != 0)");
        if (op == "sqadd") return lane0(size, "__builtin_elementwise_add_sat(" + a + ", " + b + ")");
        if (op == "uqadd") return lane0(size, "__builtin_elementwise_add_sat(" + ua + ", " + ub + ")");
        if (op == "sqsub") return lane0(size, "__builtin_elementwise_sub_sat(" + a + ", " + b + ")");
        if (op == "uqsub") return lane0(size, "__builtin_elementwise_sub_sat(" + ua + ", " + ub + ")");
        if (op == "sshl") return lane0(size, "ts_vshl<" + t + ", " + e + ", false, false>(" + a + ", " + b + ")");
        if (op == "ushl") return lane0(size, "ts_vshl<" + ut + ", " + ue_ + ", false, false>(" + ua + ", " + ub + ")");
        if (op == "srshl") return lane0(size, "ts_vshl<" + t + ", " + e + ", true, false>(" + a + ", " + b + ")");
        if (op == "urshl") return lane0(size, "ts_vshl<" + ut + ", " + ue_ + ", true, false>(" + ua + ", " + ub + ")");
        if (op == "sqshl") return lane0(size, "ts_vshl<" + t + ", " + e + ", false, true>(" + a + ", " + b + ")");
        if (op == "uqshl") return lane0(size, "ts_vshl<" + ut + ", " + ue_ + ", false, true>(" + ua + ", " + ub + ")");
        if (op == "sqrshl") return lane0(size, "ts_vshl<" + t + ", " + e + ", true, true>(" + a + ", " + b + ")");
        if (op == "uqrshl") return lane0(size, "ts_vshl<" + ut + ", " + ue_ + ", true, true>(" + ua + ", " + ub + ")");
        if (op == "sqdmulh") return lane0(size, "ts_vqdmulh<" + t + ", " + e + ", false>(" + a + ", " + b + ")");
        if (op == "sqrdmulh") return lane0(size, "ts_vqdmulh<" + t + ", " + e + ", true>(" + a + ", " + b + ")");
        return {};
    }

    std::string scalar_three_different() {
        unsigned size = in_.bits(23, 22);
        const std::string& op = name();
        std::string wide_e = se[size + 1], narrow_e = se[size];
        std::string a = "(__int128)(" + narrow_e + ")s->v[" + str(in_.rn()) + "].lo", b = "(__int128)(" + narrow_e + ")s->v[" + str(in_.rm()) + "].lo";
        std::string p = "ts_clamp<" + wide_e + ">(" + a + " * " + b + " * 2)";
        std::string acc = "(__int128)(" + wide_e + ")s->v[" + str(in_.rd()) + "].lo";
        std::string v = op == "sqdmull" ? p : op == "sqdmlal" ? "ts_clamp<" + wide_e + ">(" + acc + " + " + p + ")" : "ts_clamp<" + wide_e + ">(" + acc + " - " + p + ")";
        return "ts_wbits(s, " + str(in_.rd()) + ", (uint64_t)(" + std::string(ue[size + 1]) + ")(" + v + "));";
    }

    std::string scalar_two_misc() {
        unsigned size = in_.bits(23, 22);
        int n = in_.rn();
        const std::string& op = name();
        bool fp = op[0] == 'f' || op == "scvtf" || op == "ucvtf";
        if (fp) {
            int lane = (size & 1) ? 3 : 2;
            std::string a = fget(n, lane), e = fe[lane];
            std::string it = lane == 3 ? "int64_t" : "int32_t", ut = lane == 3 ? "uint64_t" : "uint32_t";
            std::string ones = lane == 3 ? "~0ull" : "0xffffffffull";
            auto mask = [&](const std::string& c) { return "ts_wbits(s, " + str(in_.rd()) + ", (" + c + ") ? " + ones + " : 0);"; };
            auto to_int = [&](const char* mode, bool is_signed) {
                return "ts_wbits(s, " + str(in_.rd()) + ", (" + ut + ")ts_to_int<" + (is_signed ? it : ut) + ", " + mode + ">(" + a + "));";
            };
            if (op == "fcvtns") return to_int("ts_nearest", true);
            if (op == "fcvtnu") return to_int("ts_nearest", false);
            if (op == "fcvtps") return to_int("ts_plus", true);
            if (op == "fcvtpu") return to_int("ts_plus", false);
            if (op == "fcvtms") return to_int("ts_minus", true);
            if (op == "fcvtmu") return to_int("ts_minus", false);
            if (op == "fcvtzs") return to_int("ts_zero", true);
            if (op == "fcvtzu") return to_int("ts_zero", false);
            if (op == "fcvtas") return to_int("ts_away", true);
            if (op == "fcvtau") return to_int("ts_away", false);
            if (op == "scvtf") return fset(in_.rd(), lane, "(" + e + ")(" + it + ")s->v[" + str(n) + "].lo");
            if (op == "ucvtf") return fset(in_.rd(), lane, "(" + e + ")(" + ut + ")s->v[" + str(n) + "].lo");
            if (op == "fcmgt") return mask(a + " > 0");
            if (op == "fcmge") return mask(a + " >= 0");
            if (op == "fcmeq") return mask(a + " == 0");
            if (op == "fcmle") return mask(a + " <= 0");
            if (op == "fcmlt") return mask(a + " < 0");
            std::string suffix = lane == 3 ? "d" : "s";
            if (op == "frecpe") return fset(in_.rd(), lane, "tsugiki_frecpe_" + suffix + "(" + a + ")");
            if (op == "frsqrte") return fset(in_.rd(), lane, "tsugiki_frsqrte_" + suffix + "(" + a + ")");
            if (op == "frecpx") return fset(in_.rd(), lane, "tsugiki_frecpx_" + suffix + "(" + a + ")");
            if (op == "fcvtxn") return {};
            return {};
        }
        std::string t = sv[size], ut = uv[size], e = se[size];
        std::string a = reg(n, t);
        if (op == "abs") return lane0(size, "__builtin_elementwise_abs(" + a + ")");
        if (op == "neg") return lane0(size, "-" + a);
        if (op == "cmgt") return lane0(size, "(" + a + " > 0)");
        if (op == "cmge") return lane0(size, "(" + a + " >= 0)");
        if (op == "cmeq") return lane0(size, "(" + a + " == 0)");
        if (op == "cmle") return lane0(size, "(" + a + " <= 0)");
        if (op == "cmlt") return lane0(size, "(" + a + " < 0)");
        if (op == "sqabs") return "ts_wbits(s, " + str(in_.rd()) + ", (uint64_t)(" + std::string(ue[size]) + ")ts_clamp<" + e + ">(__builtin_elementwise_abs((__int128)" + a + "[0])));";
        if (op == "sqneg") return "ts_wbits(s, " + str(in_.rd()) + ", (uint64_t)(" + std::string(ue[size]) + ")ts_clamp<" + e + ">(-(__int128)" + a + "[0]));";
        if (op == "sqxtn" || op == "uqxtn" || op == "sqxtun") {
            bool from_signed = op != "uqxtn", to_signed = op == "sqxtn";
            std::string from_e = from_signed ? se[size + 1] : ue[size + 1], to_e = to_signed ? se[size] : ue[size];
            return "ts_wbits(s, " + str(in_.rd()) + ", (uint64_t)(" + std::string(ue[size]) + ")ts_clamp<" + to_e + ">((__int128)(" + from_e + ")s->v[" + str(n) + "].lo));";
        }
        if (op == "suqadd" || op == "usqadd") {
            bool ours_signed = op == "suqadd";
            std::string mine = ours_signed ? e : ue[size], other = ours_signed ? ue[size] : e;
            return "ts_wbits(s, " + str(in_.rd()) + ", (uint64_t)(" + std::string(ue[size]) + ")ts_clamp<" + mine + ">((__int128)(" + mine + ")s->v[" + str(in_.rd()) +
                   "].lo + (__int128)(" + other + ")s->v[" + str(n) + "].lo));";
        }
        return {};
    }

    std::string scalar_pairwise() {
        unsigned size = in_.bits(23, 22);
        int n = in_.rn(), d = in_.rd();
        const std::string& op = name();
        if (op == "addp") {
            return "ts_wbits(s, " + str(d) + ", s->v[" + str(n) + "].lo + s->v[" + str(n) + "].hi);";
        }
        if (!in_.bit(29)) {
            // half precision pairs
            return {};
        }
        int lane = (size & 1) ? 3 : 2;
        std::string t = fv[lane];
        std::string a = reg(n, t) + "[0]", b = reg(n, t) + "[1]";
        std::string v;
        if (op == "faddp") v = a + " + " + b;
        else if (op == "fmaxp") v = "__builtin_elementwise_maximum(" + a + ", " + b + ")";
        else if (op == "fminp") v = "__builtin_elementwise_minimum(" + a + ", " + b + ")";
        else if (op == "fmaxnmp") v = "__builtin_elementwise_maximumnum(" + a + ", " + b + ")";
        else if (op == "fminnmp") v = "__builtin_elementwise_minimumnum(" + a + ", " + b + ")";
        else return {};
        return fset(d, lane, v);
    }

    std::string scalar_copy() {
        unsigned imm5 = in_.bits(20, 16);
        unsigned size = __builtin_ctz(imm5), index = imm5 >> (size + 1);
        return "ts_wbits(s, " + str(in_.rd()) + ", (uint64_t)" + reg(in_.rn(), uv[size]) + "[" + str(index) + "]);";
    }

    const Instruction& in_;
    uint64_t pc_;
    std::string name_;
};

}  // namespace

std::string simd(const a64::Instruction& in, uint64_t pc) {
    return Lifter(in, pc).run();
}

}  // namespace tsugiki::lift
