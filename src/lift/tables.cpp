#include "lift/tables.hpp"

#include <cstring>
#include <optional>
#include <set>

#include "a64/simd.hpp"
#include "lift/emit.hpp"

namespace tsugiki::lift {

namespace {

using a64::Instruction;
using a64::Op;

// how far back to look for a register's value, compilers hoist table bases out of loops
constexpr size_t window = 256;

enum class Write { no, yes, unknown };

Write writes(const Instruction& in, int reg) {
    if (reg == 31) {
        return Write::no;
    }
    switch (in.op) {
    case Op::bl:
    case Op::blr:
    case Op::blraa:
    case Op::blrab:
    case Op::blraaz:
    case Op::blrabz:
        // calls trash the caller-saved ones
        return reg <= 18 || reg == 30 ? Write::unknown : Write::no;
    case Op::b:
    case Op::b_cond:
    case Op::bc_cond:
    case Op::cbz:
    case Op::cbnz:
    case Op::tbz:
    case Op::tbnz:
    case Op::br:
    case Op::ret:
    case Op::hint:
    case Op::dmb:
    case Op::dsb:
    case Op::isb:
        return Write::no;
    default:
        break;
    }
    // fp/simd mostly writes v registers, a miss here only costs a table
    if (in.op == Op::simd || a64::is_simd(in.op)) {
        return Write::no;
    }
    if (in.access.bytes && !in.access.load) {
        return Write::no;
    }
    if (in.access.bytes && in.access.simd) {
        return Write::no;
    }
    if (in.rd() == reg) {
        return Write::yes;
    }
    if (in.access.pair && in.rt2() == reg) {
        return Write::unknown;
    }
    return Write::no;
}

// the nearest earlier instruction writing reg, by address order, no cfg
std::optional<size_t> writer(const Function& f, size_t before, int reg) {
    size_t stop = before > window ? before - window : 0;
    for (size_t j = before; j-- > stop;) {
        switch (writes(f.code[j], reg)) {
        case Write::no: continue;
        case Write::yes: return j;
        case Write::unknown: return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<uint64_t> value(const Function& f, size_t before, int reg, int depth = 0);

// [base, #offset] of an integer ldr/str with an immediate, for stack slots
struct Slot {
    int base;
    int64_t offset;
    bool operator==(const Slot&) const = default;
};

std::optional<Slot> slot(const Instruction& in) {
    if (in.bit(26)) {
        return std::nullopt;
    }
    if (in.op == Op::single_unsigned) {
        return Slot{in.rn(), int64_t(in.bits(21, 10)) << in.bits(31, 30)};
    }
    if (in.op == Op::single_unscaled) {
        return Slot{in.rn(), text::sign_extend(in.bits(20, 12), 9)};
    }
    return std::nullopt;
}

// the x register an str or stp puts in that slot
std::optional<int> stored_to(const Instruction& in, Slot want) {
    if (in.bit(26)) {
        return std::nullopt;
    }
    if (in.bits(31, 30) == 3 && in.bits(23, 22) == 0 && slot(in) == want) {
        return in.rt();
    }
    if (in.op == Op::pair_offset && in.bits(31, 30) == 2 && !in.bit(22) && in.rn() == want.base) {
        int64_t offset = text::sign_extend(in.bits(21, 15), 7) * 8;
        if (offset == want.offset) return in.rt();
        if (offset + 8 == want.offset) return in.rt2();
    }
    return std::nullopt;
}

// what the instruction at j leaves in its rd
std::optional<uint64_t> produced(const Function& f, size_t j, int depth) {
    if (depth > 8) {
        return std::nullopt;
    }
    const Instruction& in = f.code[j];
    uint64_t pc = f.start + 4 * j;
    switch (in.op) {
    case Op::adr:
        return pc + uint64_t(text::sign_extend((in.bits(23, 5) << 2) | in.bits(30, 29), 21));
    case Op::adrp:
        return (pc & ~uint64_t(0xfff)) + uint64_t(text::sign_extend((in.bits(23, 5) << 2) | in.bits(30, 29), 21) << 12);
    case Op::add_imm:
    case Op::sub_imm: {
        if (in.rn() == 31) {
            return std::nullopt;
        }
        auto base = value(f, j, in.rn(), depth + 1);
        if (!base) {
            return std::nullopt;
        }
        uint64_t imm = uint64_t(in.bits(21, 10)) << (in.bit(22) ? 12 : 0);
        uint64_t r = in.op == Op::add_imm ? *base + imm : *base - imm;
        return in.sf() ? r : uint32_t(r);
    }
    case Op::logical_shifted:
        // mov is orr rd, zr, rm
        if (in.bits(30, 29) == 1 && in.rn() == 31 && in.bits(15, 10) == 0 && !in.bit(21)) {
            auto v = value(f, j, in.rm(), depth + 1);
            if (v && !in.sf()) {
                return uint32_t(*v);
            }
            return v;
        }
        return std::nullopt;
    case Op::movz:
        return uint64_t(in.bits(20, 5)) << (16 * in.bits(22, 21));
    case Op::single_unsigned:
    case Op::single_unscaled: {
        // a table base spilled to the stack, it's whatever got stored there last
        auto from = slot(in);
        if (!from || in.bits(31, 30) != 3 || in.bits(23, 22) != 1) {
            return std::nullopt;
        }
        for (size_t k = j; k-- > 0;) {
            if (auto stored = stored_to(f.code[k], *from)) {
                return value(f, k, *stored, depth + 1);
            }
        }
        return std::nullopt;
    }
    default:
        return std::nullopt;
    }
}

std::optional<uint64_t> value(const Function& f, size_t before, int reg, int depth) {
    auto j = writer(f, before, reg);
    if (!j) {
        return std::nullopt;
    }
    return produced(f, *j, depth);
}

// the nearest write if we can work it out, else every write in the function we can,
// hoisted table bases hide behind writes on other paths
std::vector<uint64_t> candidates(const Function& f, size_t before, int reg) {
    if (auto v = value(f, before, reg)) {
        return {*v};
    }
    std::set<uint64_t> all;
    for (size_t j = 0; j < f.code.size(); ++j) {
        if (writes(f.code[j], reg) == Write::yes) {
            if (auto v = produced(f, j, 0)) {
                all.insert(*v);
            }
        }
    }
    return {all.begin(), all.end()};
}

uint64_t extend(uint64_t v, unsigned option) {
    switch (option) {
    case 0: return uint8_t(v);
    case 1: return uint16_t(v);
    case 2: return uint32_t(v);
    case 4: return uint64_t(int64_t(int8_t(v)));
    case 5: return uint64_t(int64_t(int16_t(v)));
    case 6: return uint64_t(int64_t(int32_t(v)));
    default: return v;
    }
}

}  // namespace

std::vector<uint64_t> jump_table(const elf::Library& library, const Function& f, size_t index) {
    const Instruction& br = f.code[index];
    auto add_at = writer(f, index, br.rn());
    if (!add_at) {
        return {};
    }
    const Instruction& add = f.code[*add_at];
    // target = anchor + (entry << amount), entry loaded from the table
    int anchor_reg = add.rn(), entry_reg = add.rm();
    unsigned amount = 0;
    unsigned option = 3;
    if (add.op == Op::addsub_shifted && add.bits(30, 29) == 0 && add.bits(23, 22) == 0) {
        amount = add.bits(15, 10);
    } else if (add.op == Op::addsub_extended && add.bits(30, 29) == 0) {
        amount = add.bits(12, 10);
        option = add.bits(15, 13);
    } else {
        return {};
    }
    auto load_at = writer(f, *add_at, entry_reg);
    auto is_table_load = [&](std::optional<size_t> at) {
        if (!at) return false;
        const Instruction& in = f.code[*at];
        return in.op == Op::single_register && in.access.load && !in.access.simd;
    };
    if (!is_table_load(load_at) && amount == 0 && add.op == Op::addsub_shifted) {
        // table-relative offsets put the table first
        std::swap(anchor_reg, entry_reg);
        load_at = writer(f, *add_at, entry_reg);
    }
    if (!is_table_load(load_at)) {
        return {};
    }
    auto anchors = candidates(f, *add_at, anchor_reg);
    const Instruction& load = f.code[*load_at];
    auto tables = candidates(f, *load_at, load.rn());
    if (anchors.empty() || tables.empty()) {
        return {};
    }
    unsigned bytes = load.access.bytes;

    // the bound: a cmp on the index a few instructions up, then b.hi or b.hs
    std::optional<uint64_t> count;
    for (size_t j = *load_at; j-- > (*load_at > 32 ? *load_at - 32 : 0);) {
        const Instruction& in = f.code[j];
        if (in.op == Op::subs_imm && in.rd() == 31) {
            uint64_t n = uint64_t(in.bits(21, 10)) << (in.bit(22) ? 12 : 0);
            for (size_t k = j + 1; k < f.code.size() && k <= index; ++k) {
                if (f.code[k].op == Op::b_cond || f.code[k].op == Op::bc_cond) {
                    unsigned cond = f.code[k].bits(3, 0);
                    if (cond == 8 || cond == 9) count = n + 1;
                    if (cond == 2 || cond == 3) count = n;
                    break;
                }
            }
            break;
        }
        if (in.op == Op::b || in.op == Op::br || in.op == Op::ret || in.op == Op::bl || in.op == Op::blr) {
            break;
        }
    }

    std::set<uint64_t> targets;
    uint64_t most = count ? std::min<uint64_t>(*count, 4096) : 1024;
    for (uint64_t anchor : anchors) {
        for (uint64_t table : tables) {
            std::vector<uint64_t> found;
            bool fits = true;
            for (uint64_t k = 0; k < most; ++k) {
                auto raw = library.at(table + k * bytes, bytes);
                if (!raw) {
                    fits = !count;
                    break;
                }
                uint64_t entry = 0;
                std::memcpy(&entry, raw->data(), bytes);
                if (load.access.sign) {
                    entry = uint64_t(text::sign_extend(entry, int(bytes * 8)));
                }
                uint64_t target = anchor + (extend(entry, option) << amount);
                if (target < f.start || target >= f.end || (target & 3)) {
                    // with a bound every entry has to land in here, else it's the wrong table
                    fits = !count;
                    break;
                }
                found.push_back(target);
            }
            if (fits) {
                targets.insert(found.begin(), found.end());
            }
        }
    }
    return {targets.begin(), targets.end()};
}

}  // namespace tsugiki::lift
