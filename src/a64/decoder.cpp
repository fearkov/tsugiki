#include "a64/decoder.hpp"

#include "a64/simd.hpp"

#include <array>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace tsugiki::a64 {

namespace {

struct Pattern {
    uint32_t mask;
    uint32_t value;
    Op op;
};

constexpr Pattern parse(const char* text, Op op) {
    uint32_t mask = 0, value = 0;
    int count = 0;
    for (const char* c = text; *c; ++c) {
        if (*c == ' ') {
            continue;
        }
        mask <<= 1;
        value <<= 1;
        if (*c == '0' || *c == '1') {
            mask |= 1;
            value |= *c == '1';
        }
        ++count;
    }
    if (count != 32) {
        throw std::logic_error("a64 pattern isn't 32 bits");
    }
    return {mask, value, op};
}

constexpr auto patterns = std::to_array<Pattern>({
#define TSUGIKI_OP(name, mnemonic, pattern) parse(pattern, Op::name),
    TSUGIKI_A64_OPS(TSUGIKI_OP)
#undef TSUGIKI_OP
});

constexpr const char* fixed_mnemonics[] = {
    "",
#define TSUGIKI_OP(name, mnemonic, pattern) mnemonic,
    TSUGIKI_A64_OPS(TSUGIKI_OP)
#undef TSUGIKI_OP
    "",
};

// patterns that can match each op0 (bits 28:25), in table order
struct Buckets {
    std::array<std::vector<Pattern>, 16> lists;
    Buckets() {
        for (uint32_t op0 = 0; op0 < 16; ++op0) {
            for (const Pattern& p : patterns) {
                uint32_t mask = (p.mask >> 25) & 15, value = (p.value >> 25) & 15;
                if ((op0 & mask) == value) {
                    lists[op0].push_back(p);
                }
            }
        }
    }
};

const Buckets& buckets() {
    static const Buckets all;
    return all;
}

uint64_t ones(unsigned count) {
    return count >= 64 ? ~uint64_t(0) : (uint64_t(1) << count) - 1;
}

uint64_t rotate_right(uint64_t value, unsigned amount, unsigned width) {
    amount %= width;
    if (amount == 0) {
        return value;
    }
    return ((value >> amount) | (value << (width - amount))) & ones(width);
}

uint64_t replicate(uint64_t element, unsigned esize, unsigned datasize) {
    uint64_t result = 0;
    for (unsigned i = 0; i < datasize; i += esize) {
        result |= element << i;
    }
    return result;
}

// loads/stores of one register: size, V, opc to what moves
std::optional<Access> single_access(const Instruction& in) {
    unsigned size = in.bits(31, 30), opc = in.bits(23, 22);
    bool v = in.bit(26);
    Access a;
    a.simd = v;
    if (v) {
        if (in.op == Op::single_unprivileged) {
            return std::nullopt;
        }
        if (size == 0 && opc >= 2) {
            a.bytes = 16;
        } else if (opc >= 2) {
            return std::nullopt;
        } else {
            a.bytes = uint8_t(1 << size);
        }
        a.load = opc & 1;
        return a;
    }
    a.bytes = uint8_t(1 << size);
    switch (opc) {
    case 0:
        a.wide = size == 3;
        return a;
    case 1:
        a.load = true;
        a.wide = size == 3;
        return a;
    case 2:
        if (size == 3) {
            // prfm, prfum, nothing else
            if (in.op != Op::single_unsigned && in.op != Op::single_register && in.op != Op::single_unscaled) {
                return std::nullopt;
            }
            a.prefetch = true;
            return a;
        }
        a.load = true;
        a.sign = true;
        a.wide = true;
        return a;
    default:
        if (size >= 2) {
            return std::nullopt;
        }
        a.load = true;
        a.sign = true;
        return a;
    }
}

std::optional<Access> pair_access(const Instruction& in) {
    unsigned opc = in.bits(31, 30);
    Access a;
    a.pair = true;
    a.simd = in.bit(26);
    a.load = in.bit(22);
    if (a.simd) {
        if (opc == 3) {
            return std::nullopt;
        }
        a.bytes = uint8_t(4 << opc);
        return a;
    }
    switch (opc) {
    case 0: a.bytes = 4; return a;
    case 1:
        // ldpsw, the store side is mte's stgp
        if (!a.load || in.op == Op::pair_noalloc) {
            return std::nullopt;
        }
        a.bytes = 4;
        a.sign = true;
        a.wide = true;
        return a;
    case 2: a.bytes = 8; a.wide = true; return a;
    default: return std::nullopt;
    }
}

std::optional<Access> literal_access(const Instruction& in) {
    unsigned opc = in.bits(31, 30);
    Access a;
    a.load = true;
    a.simd = in.bit(26);
    if (a.simd) {
        if (opc == 3) {
            return std::nullopt;
        }
        a.bytes = uint8_t(4 << opc);
        return a;
    }
    switch (opc) {
    case 0: a.bytes = 4; return a;
    case 1: a.bytes = 8; a.wide = true; return a;
    case 2: a.bytes = 4; a.sign = true; a.wide = true; return a;
    default: a.load = false; a.prefetch = true; return a;
    }
}

// size field at 31:30
Access sized(const Instruction& in, bool load) {
    Access a;
    a.load = load;
    a.bytes = uint8_t(1 << in.bits(31, 30));
    a.wide = in.bits(31, 30) == 3;
    return a;
}

bool allocated(Instruction& in) {
    switch (in.op) {
    case Op::minmax_imm:
        return in.bits(21, 18) <= 3;
    case Op::and_imm:
    case Op::orr_imm:
    case Op::eor_imm:
    case Op::ands_imm:
        if (!in.sf() && in.bit(22)) {
            return false;
        }
        return bit_masks(in.bit(22), in.bits(15, 10), in.bits(21, 16), true, in.sf() ? 64 : 32).has_value();
    case Op::movn:
    case Op::movz:
    case Op::movk:
        return in.sf() || !in.bit(22);
    case Op::sbfm:
    case Op::bfm:
    case Op::ubfm:
        if (in.bit(22) != in.sf()) {
            return false;
        }
        return in.sf() || (!in.bit(21) && !in.bit(15));
    case Op::extr:
        return in.bit(22) == in.sf() && (in.sf() || !in.bit(15));
    case Op::stxr:
    case Op::stlxr:
    case Op::stllr:
    case Op::stlr:
        in.access = sized(in, false);
        return true;
    case Op::ldxr:
    case Op::ldaxr:
    case Op::ldlar:
    case Op::ldar:
    case Op::ldapr:
        in.access = sized(in, true);
        return true;
    case Op::stxp:
    case Op::stlxp:
    case Op::ldxp:
    case Op::ldaxp:
        in.access.load = in.bit(22);
        in.access.pair = true;
        in.access.bytes = in.bit(30) ? 8 : 4;
        in.access.wide = in.bit(30);
        return true;
    case Op::cas:
    case Op::atomic:
    case Op::swp:
        in.access = sized(in, true);
        return true;
    case Op::casp:
        // even register pairs only
        if ((in.rs() & 1) || (in.rt() & 1)) {
            return false;
        }
        in.access.load = true;
        in.access.pair = true;
        in.access.bytes = in.bit(30) ? 8 : 4;
        in.access.wide = in.bit(30);
        return true;
    case Op::ldraa:
    case Op::ldrab:
        in.access.load = true;
        in.access.bytes = 8;
        in.access.wide = true;
        return true;
    case Op::rcpc_unscaled: {
        unsigned size = in.bits(31, 30), opc = in.bits(23, 22);
        Access a;
        a.bytes = uint8_t(1 << size);
        if (opc == 0 || opc == 1) {
            a.load = opc == 1;
            a.wide = size == 3;
        } else if (opc == 2 && size <= 2) {
            a.load = a.sign = a.wide = true;
        } else if (opc == 3 && size <= 1) {
            a.load = a.sign = true;
        } else {
            return false;
        }
        in.access = a;
        return true;
    }
    case Op::simd_multiple:
    case Op::simd_multiple_post: {
        unsigned opcode = in.bits(15, 12);
        bool q = in.bit(30), d = in.bits(11, 10) == 3;
        switch (opcode) {
        case 0b0010: case 0b0110: case 0b0111: case 0b1010:
            break;
        case 0b0000: case 0b0100: case 0b1000:
            // ld2/3/4 can't do .1d
            if (d && !q) {
                return false;
            }
            break;
        default:
            return false;
        }
        in.access.load = in.bit(22);
        in.access.simd = true;
        return true;
    }
    case Op::simd_single:
    case Op::simd_single_post: {
        unsigned opcode = in.bits(15, 13), size = in.bits(11, 10);
        bool s = in.bit(12);
        switch (opcode >> 1) {
        case 0: break;
        case 1: if (size & 1) return false; break;
        case 2: if (size > 1 || (size == 1 && s)) return false; break;
        default:
            // ld1r..ld4r
            if (!in.bit(22) || s) {
                return false;
            }
        }
        in.access.load = in.bit(22);
        in.access.simd = true;
        return true;
    }
    case Op::ldr_literal: {
        auto a = literal_access(in);
        in.access = a.value_or(Access{});
        return a.has_value();
    }
    case Op::pair_noalloc:
    case Op::pair_post:
    case Op::pair_offset:
    case Op::pair_pre: {
        auto a = pair_access(in);
        in.access = a.value_or(Access{});
        return a.has_value();
    }
    case Op::single_unscaled:
    case Op::single_post:
    case Op::single_unprivileged:
    case Op::single_pre:
    case Op::single_unsigned: {
        auto a = single_access(in);
        in.access = a.value_or(Access{});
        return a.has_value();
    }
    case Op::single_register: {
        // option<1> picks the index register width, 0 is reserved
        if (!in.bit(14)) {
            return false;
        }
        auto a = single_access(in);
        in.access = a.value_or(Access{});
        return a.has_value();
    }
    case Op::logical_shifted:
        return in.sf() || !in.bit(15);
    case Op::addsub_shifted:
        return in.bits(23, 22) != 3 && (in.sf() || !in.bit(15));
    case Op::addsub_extended:
        return in.bits(12, 10) <= 4;
    case Op::dp2: {
        unsigned opcode = in.bits(15, 10);
        switch (opcode) {
        case 2: case 3: case 8: case 9: case 10: case 11:
        case 24: case 25: case 26: case 27:
            return true;
        case 12:
            return in.sf();
        case 16: case 17: case 18: case 20: case 21: case 22:
            return !in.sf();
        case 19: case 23:
            return in.sf();
        default:
            return false;
        }
    }
    case Op::dp1: {
        unsigned opcode2 = in.bits(20, 16), opcode = in.bits(15, 10);
        if (opcode2 == 0) {
            if (opcode == 3) {
                return in.sf();
            }
            return opcode <= 8;
        }
        if (opcode2 != 1 || !in.sf()) {
            return false;
        }
        if (opcode <= 7) {
            return true;
        }
        return opcode <= 17 && in.rn() == 31;
    }
    case Op::dp3: {
        unsigned op31 = in.bits(23, 21);
        bool o0 = in.bit(15);
        switch (op31) {
        case 0: return true;
        case 1: case 5: return in.sf();
        // ra should be 31, but it's ignored either way
        case 2: case 6: return in.sf() && !o0;
        default: return false;
        }
    }
    default:
        return true;
    }
}

const char* size_suffix(unsigned size) {
    return size == 0 ? "b" : size == 1 ? "h" : "";
}

}  // namespace

std::optional<Masks> bit_masks(bool n, unsigned imms, unsigned immr, bool immediate, unsigned datasize) {
    unsigned combined = (unsigned(n) << 6) | (~imms & 0x3f);
    if (combined == 0) {
        return std::nullopt;
    }
    unsigned len = 31 - __builtin_clz(combined);
    if (len < 1) {
        return std::nullopt;
    }
    unsigned esize = 1u << len;
    if (esize > datasize) {
        return std::nullopt;
    }
    unsigned levels = esize - 1;
    if (immediate && (imms & levels) == levels) {
        return std::nullopt;
    }
    unsigned s = imms & levels, r = immr & levels;
    unsigned d = (s - r) & levels;
    uint64_t welem = ones(s + 1), telem = ones(d + 1);
    return Masks{replicate(rotate_right(welem, r, esize), esize, datasize), replicate(telem, esize, datasize)};
}

Instruction decode(uint32_t raw) {
    Instruction in;
    in.raw = raw;
    unsigned op0 = (raw >> 25) & 15;
    for (const Pattern& p : buckets().lists[op0]) {
        if ((raw & p.mask) == p.value) {
            in.op = p.op;
            bool ok = is_simd(in.op) ? !simd_name(in).empty() : allocated(in);
            if (!ok) {
                in.op = Op::invalid;
                in.access = {};
            }
            return in;
        }
    }
    // fp/simd we don't decode yet, fp16 vectors and such
    if ((op0 & 7) == 7) {
        in.op = Op::simd;
    }
    return in;
}

std::vector<Encoding> encodings() {
    std::vector<Encoding> all;
    for (const Pattern& p : patterns) {
        all.push_back({p.op, p.mask, p.value});
    }
    return all;
}

const char* condition(unsigned code) {
    static const char* names[] = {"eq", "ne", "hs", "lo", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le", "al", "nv"};
    return names[code & 15];
}

std::string mnemonic(const Instruction& in) {
    if (is_simd(in.op)) {
        return simd_name(in);
    }
    const char* fixed = fixed_mnemonics[size_t(in.op)];
    if (fixed[0] != 0) {
        return fixed;
    }
    unsigned size = in.bits(31, 30);
    std::string acquire = in.bit(22) ? "a" : "", release = in.bit(15) ? "l" : "";
    switch (in.op) {
    case Op::invalid: return "invalid";
    case Op::simd: return "simd";
    case Op::b_cond: return std::string("b.") + condition(in.bits(3, 0));
    case Op::bc_cond: return std::string("bc.") + condition(in.bits(3, 0));
    case Op::minmax_imm: {
        static const char* names[] = {"smax", "umax", "smin", "umin"};
        return names[in.bits(19, 18)];
    }
    case Op::stxr: return std::string("stxr") + size_suffix(size);
    case Op::stlxr: return std::string("stlxr") + size_suffix(size);
    case Op::ldxr: return std::string("ldxr") + size_suffix(size);
    case Op::ldaxr: return std::string("ldaxr") + size_suffix(size);
    case Op::stllr: return std::string("stllr") + size_suffix(size);
    case Op::stlr: return std::string("stlr") + size_suffix(size);
    case Op::ldlar: return std::string("ldlar") + size_suffix(size);
    case Op::ldar: return std::string("ldar") + size_suffix(size);
    case Op::ldapr: return std::string("ldapr") + size_suffix(size);
    case Op::cas: return "cas" + acquire + release + size_suffix(size);
    case Op::casp: return "casp" + acquire + release;
    case Op::atomic: {
        static const char* names[] = {"ldadd", "ldclr", "ldeor", "ldset", "ldsmax", "ldsmin", "ldumax", "ldumin"};
        return names[in.bits(14, 12)] + std::string(in.bit(23) ? "a" : "") + (in.bit(22) ? "l" : "") + size_suffix(size);
    }
    case Op::swp:
        return "swp" + std::string(in.bit(23) ? "a" : "") + (in.bit(22) ? "l" : "") + size_suffix(size);
    case Op::ldr_literal:
        return in.access.prefetch ? "prfm" : in.access.sign ? "ldrsw" : "ldr";
    case Op::rcpc_unscaled: {
        std::string name = in.access.load ? "ldapur" : "stlur";
        if (in.access.sign) {
            name += "s";
        }
        return name + (in.access.bytes == 1 ? "b" : in.access.bytes == 2 ? "h" : (in.access.sign && in.access.bytes == 4) ? "w" : "");
    }
    case Op::simd_multiple:
    case Op::simd_multiple_post: {
        std::string name = in.bit(22) ? "ld" : "st";
        switch (in.bits(15, 12)) {
        case 0b0000: return name + "4";
        case 0b0100: return name + "3";
        case 0b1000: return name + "2";
        default: return name + "1";
        }
    }
    case Op::simd_single:
    case Op::simd_single_post: {
        std::string name = in.bit(22) ? "ld" : "st";
        unsigned opcode = in.bits(15, 13);
        unsigned count = ((opcode & 1) << 1 | in.bit(21)) + 1;
        if ((opcode >> 1) == 3) {
            return name + std::to_string(count) + "r";
        }
        return name + std::to_string(count);
    }
    case Op::pair_noalloc:
        return in.access.load ? "ldnp" : "stnp";
    case Op::pair_post:
    case Op::pair_offset:
    case Op::pair_pre:
        return in.access.load ? (in.access.sign ? "ldpsw" : "ldp") : "stp";
    case Op::single_unscaled:
    case Op::single_post:
    case Op::single_unprivileged:
    case Op::single_pre:
    case Op::single_register:
    case Op::single_unsigned: {
        const Access& a = in.access;
        if (a.prefetch) {
            return in.op == Op::single_unscaled ? "prfum" : "prfm";
        }
        std::string name = a.load ? "ld" : "st";
        name += in.op == Op::single_unscaled ? "ur" : in.op == Op::single_unprivileged ? "tr" : "r";
        if (a.simd) {
            return name;
        }
        if (a.sign) {
            name += "s";
        }
        name += a.bytes == 1 ? "b" : a.bytes == 2 ? "h" : (a.sign && a.bytes == 4) ? "w" : "";
        return name;
    }
    case Op::logical_shifted: {
        static const char* names[] = {"and", "bic", "orr", "orn", "eor", "eon", "ands", "bics"};
        return names[in.bits(30, 29) * 2 + in.bit(21)];
    }
    case Op::addsub_shifted:
    case Op::addsub_extended: {
        static const char* names[] = {"add", "adds", "sub", "subs"};
        return names[in.bits(30, 29)];
    }
    case Op::adc: {
        static const char* names[] = {"adc", "adcs", "sbc", "sbcs"};
        return names[in.bits(30, 29)];
    }
    case Op::ccmp_register:
    case Op::ccmp_immediate:
        return in.bit(30) ? "ccmp" : "ccmn";
    case Op::csel: {
        static const char* names[] = {"csel", "csinc", "csinv", "csneg"};
        return names[in.bit(30) * 2 + in.bit(10)];
    }
    case Op::dp2: {
        switch (in.bits(15, 10)) {
        case 2: return "udiv";
        case 3: return "sdiv";
        case 8: return "lslv";
        case 9: return "lsrv";
        case 10: return "asrv";
        case 11: return "rorv";
        case 12: return "pacga";
        case 16: return "crc32b";
        case 17: return "crc32h";
        case 18: return "crc32w";
        case 19: return "crc32x";
        case 20: return "crc32cb";
        case 21: return "crc32ch";
        case 22: return "crc32cw";
        case 23: return "crc32cx";
        case 24: return "smax";
        case 25: return "umax";
        case 26: return "smin";
        case 27: return "umin";
        }
        return "invalid";
    }
    case Op::dp1: {
        if (in.bits(20, 16) == 0) {
            static const char* names[] = {"rbit", "rev16", "rev", "rev", "clz", "cls", "ctz", "cnt", "abs"};
            if (in.bits(15, 10) == 2 && in.sf()) {
                return "rev32";
            }
            return names[in.bits(15, 10)];
        }
        static const char* names[] = {"pacia", "pacib", "pacda", "pacdb", "autia", "autib", "autda", "autdb",
                                      "paciza", "pacizb", "pacdza", "pacdzb", "autiza", "autizb", "autdza", "autdzb",
                                      "xpaci", "xpacd"};
        return names[in.bits(15, 10)];
    }
    case Op::dp3: {
        switch (in.bits(23, 21) * 2 + in.bit(15)) {
        case 0: return "madd";
        case 1: return "msub";
        case 2: return "smaddl";
        case 3: return "smsubl";
        case 4: return "smulh";
        case 10: return "umaddl";
        case 11: return "umsubl";
        case 12: return "umulh";
        }
        return "invalid";
    }
    default:
        return "?";
    }
}

}  // namespace tsugiki::a64
