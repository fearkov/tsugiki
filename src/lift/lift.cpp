#include "lift/lift.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <optional>
#include <sstream>

#include "bytes.hpp"
#include "eh_frame.hpp"

namespace tsugiki::lift {

namespace {

using a64::Instruction;
using a64::Op;

int64_t sign_extend(uint64_t value, int bits) {
    uint64_t sign = uint64_t(1) << (bits - 1);
    return int64_t((value ^ sign) - sign);
}

// where a direct branch goes, none if it isn't one
std::optional<uint64_t> branch_target(const Instruction& in, uint64_t pc) {
    switch (in.op) {
    case Op::b:
    case Op::bl:
        return pc + uint64_t(sign_extend(in.bits(25, 0), 26) * 4);
    case Op::b_cond:
    case Op::bc_cond:
    case Op::cbz:
    case Op::cbnz:
        return pc + uint64_t(sign_extend(in.bits(23, 5), 19) * 4);
    case Op::tbz:
    case Op::tbnz:
        return pc + uint64_t(sign_extend(in.bits(18, 5), 14) * 4);
    default:
        return std::nullopt;
    }
}

bool is_call(const Instruction& in) {
    switch (in.op) {
    case Op::bl:
    case Op::blr:
    case Op::blraa:
    case Op::blrab:
    case Op::blraaz:
    case Op::blrabz:
        return true;
    default:
        return false;
    }
}

std::string hex(uint64_t value) {
    char text[32];
    std::snprintf(text, sizeof text, "0x%llxull", static_cast<unsigned long long>(value));
    return text;
}

std::string name(uint64_t address) {
    char text[32];
    std::snprintf(text, sizeof text, "%llx", static_cast<unsigned long long>(address));
    return text;
}

std::string x(int n) {
    return "s->x[" + std::to_string(n) + "]";
}

// a register as a value, 31 is zr or sp
std::string get(int n, bool wide, bool sp = false) {
    if (n == 31 && !sp) {
        return wide ? "(uint64_t)0" : "(uint32_t)0";
    }
    return wide ? x(n) : "(uint32_t)" + x(n);
}

// assignment, 31 drops the value unless it's sp, w zero-extends
std::string set(int n, bool wide, const std::string& value, bool sp = false) {
    if (n == 31 && !sp) {
        return "(void)(" + value + ");";
    }
    return x(n) + " = " + (wide ? "(uint64_t)(" + value + ")" : "(uint64_t)(uint32_t)(" + value + ")") + ";";
}

std::string condition(unsigned code) {
    switch (code & 15) {
    case 0: return "s->zf";
    case 1: return "!s->zf";
    case 2: return "s->cf";
    case 3: return "!s->cf";
    case 4: return "s->nf";
    case 5: return "!s->nf";
    case 6: return "s->vf";
    case 7: return "!s->vf";
    case 8: return "(s->cf && !s->zf)";
    case 9: return "!(s->cf && !s->zf)";
    case 10: return "(s->nf == s->vf)";
    case 11: return "(s->nf != s->vf)";
    case 12: return "(!s->zf && s->nf == s->vf)";
    case 13: return "!(!s->zf && s->nf == s->vf)";
    default: return "1";
    }
}

std::string shifted(const std::string& value, unsigned type, unsigned amount, bool wide) {
    if (amount == 0) {
        return value;
    }
    std::string a = std::to_string(amount);
    switch (type) {
    case 0: return "(" + value + " << " + a + ")";
    case 1: return "(" + value + " >> " + a + ")";
    case 2: return wide ? "(uint64_t)((int64_t)" + value + " >> " + a + ")" : "(uint32_t)((int32_t)" + value + " >> " + a + ")";
    default: return wide ? "ts_ror64(" + value + ", " + a + ")" : "ts_ror32(" + value + ", " + a + ")";
    }
}

// uxtb .. sxtx of a 64-bit register value
std::string extended(int rm, unsigned option) {
    std::string v = get(rm, true);
    switch (option) {
    case 0: return "(uint64_t)(uint8_t)" + v;
    case 1: return "(uint64_t)(uint16_t)" + v;
    case 2: return "(uint64_t)(uint32_t)" + v;
    case 3: return v;
    case 4: return "(uint64_t)(int64_t)(int8_t)" + v;
    case 5: return "(uint64_t)(int64_t)(int16_t)" + v;
    case 6: return "(uint64_t)(int64_t)(int32_t)" + v;
    default: return v;
    }
}

std::string load(unsigned bytes, const std::string& address) {
    return "ts_ld" + std::to_string(bytes * 8) + "(" + address + ")";
}

std::string store(unsigned bytes, const std::string& address, const std::string& value) {
    std::string type = "uint" + std::to_string(bytes * 8) + "_t";
    return "ts_st" + std::to_string(bytes * 8) + "(" + address + ", (" + type + ")(" + value + "));";
}

std::string utype(unsigned bytes) {
    return "uint" + std::to_string(bytes * 8) + "_t";
}

// what an integer load leaves in a register
std::string loaded(const a64::Access& a, const std::string& raw) {
    std::string bits = std::to_string(a.bytes * 8);
    if (!a.sign) {
        return "(uint64_t)" + raw;
    }
    if (a.wide) {
        return "(uint64_t)(int64_t)(int" + bits + "_t)" + raw;
    }
    return "(uint64_t)(uint32_t)(int32_t)(int" + bits + "_t)" + raw;
}

unsigned log2_bytes(unsigned bytes) {
    unsigned scale = 0;
    while ((1u << scale) < bytes) {
        ++scale;
    }
    return scale;
}

class Emitter {
public:
    Emitter(const Program& program, const Function& function, std::ostringstream& out)
        : program_(program), function_(function), out_(out) {}

    void function() {
        out_ << "static void f_" << name(function_.start) << "(tsugiki_state* __restrict s) {\n";
        out_ << "    switch (s->pc - TB) {\n";
        for (uint64_t entry : function_.entries) {
            out_ << "    case " << hex(entry) << ": goto L_" << name(entry) << ";\n";
        }
        out_ << "    default: tsugiki_trap(s, s->pc, 0, \"no entry there\");\n    }\n";
        for (size_t i = 0; i < function_.code.size(); ++i) {
            uint64_t pc = function_.start + 4 * i;
            if (function_.labels.contains(pc) || function_.entries.contains(pc)) {
                out_ << "L_" << name(pc) << ":\n";
            }
            pc_ = pc;
            next_ = pc + 4;
            const Instruction& in = function_.code[i];
            out_ << "    // " << a64::mnemonic(in) << "\n";
            instruction(in);
        }
        // ran off the end
        out_ << "    s->pc = TB + " << hex(function_.end) << ";\n    return;\n}\n\n";
    }

private:
    void line(const std::string& text) { out_ << "    " << text << "\n"; }

    void trap(const Instruction& in, const char* why) {
        char text[128];
        std::snprintf(text, sizeof text, "tsugiki_trap(s, TB + %s, 0x%08x, \"%s\");", hex(pc_).c_str(), in.raw, why);
        line(text);
    }

    bool inside(uint64_t target) const { return target >= function_.start && target < function_.end; }

    // jump somewhere, s->pc set for anything not in here
    std::string jump(uint64_t target) {
        if (inside(target)) {
            return "goto L_" + name(target) + ";";
        }
        if (program_.starts.contains(target)) {
            return "{ s->pc = TB + " + hex(target) + "; f_" + name(target) + "(s); return; }";
        }
        return "{ s->pc = TB + " + hex(target) + "; return; }";
    }

    void call(const std::string& how) {
        line(how);
        line("if (s->pc != TB + " + hex(next_) + ") return;");
    }

    void instruction(const Instruction& in) {
        bool wide = in.sf();
        switch (in.op) {
        case Op::adr:
        case Op::adrp: {
            int64_t immediate = sign_extend((in.bits(23, 5) << 2) | in.bits(30, 29), 21);
            uint64_t target = in.op == Op::adr ? pc_ + uint64_t(immediate) : (pc_ & ~uint64_t(0xfff)) + uint64_t(immediate << 12);
            line(set(in.rd(), true, "TB + " + hex(target)));
            return;
        }
        case Op::add_imm:
        case Op::adds_imm:
        case Op::sub_imm:
        case Op::subs_imm: {
            uint64_t imm = uint64_t(in.bits(21, 10)) << (in.bit(22) ? 12 : 0);
            std::string n = get(in.rn(), wide, true), i = wide ? hex(imm) : "(uint32_t)" + hex(imm);
            bool sub = in.op == Op::sub_imm || in.op == Op::subs_imm;
            if (in.op == Op::add_imm || in.op == Op::sub_imm) {
                line(set(in.rd(), wide, n + (sub ? " - " : " + ") + i, true));
            } else {
                std::string w = wide ? "64" : "32";
                std::string flags = sub ? "ts_subs" + w + "(s, " + n + ", " + i + ")" : "ts_adds" + w + "(s, " + n + ", " + i + ", 0)";
                line(set(in.rd(), wide, flags));
            }
            return;
        }
        case Op::and_imm:
        case Op::orr_imm:
        case Op::eor_imm:
        case Op::ands_imm: {
            auto masks = a64::bit_masks(in.bit(22), in.bits(15, 10), in.bits(21, 16), true, wide ? 64 : 32);
            std::string imm = wide ? hex(masks->wmask) : "(uint32_t)" + hex(masks->wmask & 0xffffffff);
            std::string n = get(in.rn(), wide);
            const char* op = in.op == Op::orr_imm ? " | " : in.op == Op::eor_imm ? " ^ " : " & ";
            if (in.op == Op::ands_imm) {
                std::string t = wide ? "uint64_t" : "uint32_t";
                line("{ " + t + " r = " + n + op + imm + "; ts_nz" + (wide ? "64" : "32") + "(s, r); " + set(in.rd(), wide, "r") + " }");
            } else {
                line(set(in.rd(), wide, n + op + imm, true));
            }
            return;
        }
        case Op::movn:
        case Op::movz:
        case Op::movk: {
            unsigned shift = in.bits(22, 21) * 16;
            uint64_t imm = uint64_t(in.bits(20, 5)) << shift;
            if (in.op == Op::movz) {
                line(set(in.rd(), wide, hex(imm)));
            } else if (in.op == Op::movn) {
                line(set(in.rd(), wide, hex(wide ? ~imm : ~imm & 0xffffffff)));
            } else {
                uint64_t keep = ~(uint64_t(0xffff) << shift);
                line(set(in.rd(), wide, "(" + get(in.rd(), true) + " & " + hex(keep) + ") | " + hex(imm)));
            }
            return;
        }
        case Op::sbfm:
        case Op::bfm:
        case Op::ubfm: {
            unsigned r = in.bits(21, 16), imms = in.bits(15, 10);
            auto masks = a64::bit_masks(in.bit(22), imms, r, false, wide ? 64 : 32);
            uint64_t all = wide ? ~uint64_t(0) : 0xffffffff;
            uint64_t wmask = masks->wmask & all, tmask = masks->tmask & all;
            std::string src = get(in.rn(), wide);
            std::string rotated = (wide ? "ts_ror64(" : "ts_ror32(") + src + ", " + std::to_string(r) + ")";
            std::string t = wide ? "uint64_t" : "uint32_t";
            if (in.op == Op::ubfm) {
                line(set(in.rd(), wide, "(" + t + ")(" + rotated + " & " + hex(wmask & tmask) + ")"));
            } else if (in.op == Op::sbfm) {
                std::string top = "((" + src + " >> " + std::to_string(imms) + ") & 1 ? (" + t + ")" + hex(all) + " : (" + t + ")0)";
                line("{ " + t + " bot = " + rotated + " & " + hex(wmask) + "; " + t + " top = " + top + "; " +
                     set(in.rd(), wide, "(top & " + hex(~tmask & all) + ") | (bot & " + hex(tmask) + ")") + " }");
            } else {
                std::string dst = get(in.rd(), wide);
                line("{ " + t + " d = " + dst + "; " + t + " bot = (d & " + hex(~wmask & all) + ") | (" + rotated + " & " + hex(wmask) + "); " +
                     set(in.rd(), wide, "(d & " + hex(~tmask & all) + ") | (bot & " + hex(tmask) + ")") + " }");
            }
            return;
        }
        case Op::extr: {
            unsigned lsb = in.bits(15, 10);
            std::string m = get(in.rm(), wide), n = get(in.rn(), wide);
            if (lsb == 0) {
                line(set(in.rd(), wide, m));
            } else {
                line(set(in.rd(), wide, "(" + m + " >> " + std::to_string(lsb) + ") | (" + n + " << " + std::to_string((wide ? 64 : 32) - lsb) + ")"));
            }
            return;
        }
        case Op::b:
            line(jump(*branch_target(in, pc_)));
            return;
        case Op::bl: {
            uint64_t target = *branch_target(in, pc_);
            line("s->x[30] = TB + " + hex(next_) + ";");
            line("s->pc = TB + " + hex(target) + ";");
            call(program_.starts.contains(target) ? "f_" + name(target) + "(s);" : "tsugiki_call(s);");
            return;
        }
        case Op::b_cond:
        case Op::bc_cond:
            line("if (" + condition(in.bits(3, 0)) + ") " + jump(*branch_target(in, pc_)));
            return;
        case Op::cbz:
        case Op::cbnz:
            line("if (" + get(in.rt(), wide) + (in.op == Op::cbz ? " == 0) " : " != 0) ") + jump(*branch_target(in, pc_)));
            return;
        case Op::tbz:
        case Op::tbnz: {
            unsigned bit = (in.bit(31) << 5) | in.bits(23, 19);
            line("if (((" + get(in.rt(), true) + " >> " + std::to_string(bit) + ") & 1) " + (in.op == Op::tbz ? "== 0) " : "!= 0) ") +
                 jump(*branch_target(in, pc_)));
            return;
        }
        case Op::br:
        case Op::braa:
        case Op::brab:
        case Op::braaz:
        case Op::brabz:
            // a tail call, plt stubs and such, the target returns for us
            line("s->pc = " + get(in.rn(), true) + ";");
            line("tsugiki_call(s);");
            line("return;");
            return;
        case Op::ret:
        case Op::retaa:
        case Op::retab:
            line("s->pc = " + get(in.op == Op::ret ? in.rn() : 30, true) + ";");
            line("return;");
            return;
        case Op::blr:
        case Op::blraa:
        case Op::blrab:
        case Op::blraaz:
        case Op::blrabz:
            line("s->pc = " + get(in.rn(), true) + ";");
            line("s->x[30] = TB + " + hex(next_) + ";");
            call("tsugiki_call(s);");
            return;
        case Op::svc:
            line("tsugiki_svc(s, " + std::to_string(in.bits(20, 5)) + ");");
            return;
        case Op::brk:
        case Op::hlt:
        case Op::udf:
            trap(in, "breakpoint");
            return;
        case Op::hint:
        case Op::isb:
        case Op::sb:
        case Op::msr_imm:
            return;
        case Op::clrex:
            line("s->excl = 0;");
            return;
        case Op::dsb:
        case Op::dmb:
            // x86 only reorders store then load, so full barriers only
            if ((in.bits(11, 8) & 3) == 3) {
                line("__atomic_thread_fence(__ATOMIC_SEQ_CST);");
            } else {
                line("__atomic_signal_fence(__ATOMIC_SEQ_CST);");
            }
            return;
        case Op::mrs:
        case Op::msr_reg:
            system_register(in);
            return;
        case Op::sys:
            line("tsugiki_sys(s, " + std::to_string(in.bits(18, 5)) + ", " + get(in.rt(), true) + ");");
            return;
        case Op::stxr:
        case Op::stlxr:
        case Op::ldxr:
        case Op::ldaxr:
        case Op::stxp:
        case Op::stlxp:
        case Op::ldxp:
        case Op::ldaxp:
            exclusive(in);
            return;
        case Op::stllr:
        case Op::stlr: {
            unsigned bytes = in.access.bytes;
            line("__atomic_store_n((" + utype(bytes) + "*)" + get(in.rn(), true, true) + ", (" + utype(bytes) + ")" + get(in.rt(), true) +
                 ", __ATOMIC_SEQ_CST);");
            return;
        }
        case Op::ldlar:
        case Op::ldar:
        case Op::ldapr: {
            unsigned bytes = in.access.bytes;
            line(set(in.rt(), true, "(uint64_t)__atomic_load_n((" + utype(bytes) + "*)" + get(in.rn(), true, true) + ", __ATOMIC_ACQUIRE)"));
            return;
        }
        case Op::cas:
        case Op::casp:
        case Op::atomic:
        case Op::swp:
            atomic(in);
            return;
        case Op::ldr_literal:
        case Op::pair_noalloc:
        case Op::pair_post:
        case Op::pair_offset:
        case Op::pair_pre:
        case Op::single_unscaled:
        case Op::single_post:
        case Op::single_unprivileged:
        case Op::single_pre:
        case Op::single_register:
        case Op::single_unsigned:
        case Op::rcpc_unscaled:
        case Op::ldraa:
        case Op::ldrab:
            memory(in);
            return;
        case Op::logical_shifted: {
            unsigned opc = in.bits(30, 29);
            std::string m = shifted(get(in.rm(), wide), in.bits(23, 22), in.bits(15, 10), wide);
            if (in.bit(21)) {
                m = "~" + m;
            }
            std::string n = get(in.rn(), wide);
            const char* op = opc == 1 ? " | " : opc == 2 ? " ^ " : " & ";
            std::string t = wide ? "uint64_t" : "uint32_t";
            if (opc == 3) {
                line("{ " + t + " r = " + n + op + "(" + t + ")(" + m + "); ts_nz" + (wide ? "64" : "32") + "(s, r); " + set(in.rd(), wide, "r") + " }");
            } else {
                line(set(in.rd(), wide, n + op + "(" + t + ")(" + m + ")"));
            }
            return;
        }
        case Op::addsub_shifted:
        case Op::addsub_extended: {
            bool sub = in.bit(30), flags = in.bit(29), extended_form = in.op == Op::addsub_extended;
            std::string t = wide ? "uint64_t" : "uint32_t";
            std::string m = extended_form ? "(" + t + ")(" + extended(in.rm(), in.bits(15, 13)) + " << " + std::to_string(in.bits(12, 10)) + ")"
                                          : "(" + t + ")" + shifted(get(in.rm(), wide), in.bits(23, 22), in.bits(15, 10), wide);
            std::string n = get(in.rn(), wide, extended_form);
            std::string w = wide ? "64" : "32";
            if (flags) {
                std::string r = sub ? "ts_subs" + w + "(s, " + n + ", " + m + ")" : "ts_adds" + w + "(s, " + n + ", " + m + ", 0)";
                line(set(in.rd(), wide, r));
            } else {
                line(set(in.rd(), wide, n + (sub ? " - " : " + ") + m, extended_form));
            }
            return;
        }
        case Op::adc: {
            bool sub = in.bit(30), flags = in.bit(29);
            std::string w = wide ? "64" : "32", t = wide ? "uint64_t" : "uint32_t";
            std::string m = sub ? "(" + t + ")~" + get(in.rm(), wide) : get(in.rm(), wide);
            std::string n = get(in.rn(), wide);
            if (flags) {
                line(set(in.rd(), wide, "ts_adds" + w + "(s, " + n + ", " + m + ", s->cf)"));
            } else {
                line(set(in.rd(), wide, n + " + " + m + " + (" + t + ")s->cf"));
            }
            return;
        }
        case Op::ccmp_register:
        case Op::ccmp_immediate: {
            std::string w = wide ? "64" : "32", t = wide ? "uint64_t" : "uint32_t";
            std::string m = in.op == Op::ccmp_immediate ? "(" + t + ")" + std::to_string(in.bits(20, 16)) : get(in.rm(), wide);
            std::string n = get(in.rn(), wide);
            std::string compare = in.bit(30) ? "(void)ts_subs" + w + "(s, " + n + ", " + m + ");" : "(void)ts_adds" + w + "(s, " + n + ", " + m + ", 0);";
            line("if (" + condition(in.bits(15, 12)) + ") { " + compare + " } else { ts_nzcv(s, " + std::to_string(in.bits(3, 0)) + "); }");
            return;
        }
        case Op::csel: {
            std::string t = wide ? "uint64_t" : "uint32_t";
            std::string m = get(in.rm(), wide);
            switch (in.bit(30) * 2 + in.bit(10)) {
            case 1: m = "(" + t + ")(" + m + " + 1)"; break;
            case 2: m = "(" + t + ")~" + m; break;
            case 3: m = "(" + t + ")-" + m; break;
            default: break;
            }
            line(set(in.rd(), wide, condition(in.bits(15, 12)) + " ? " + get(in.rn(), wide) + " : " + m));
            return;
        }
        case Op::dp2:
            data2(in);
            return;
        case Op::dp1:
            data1(in);
            return;
        case Op::dp3:
            data3(in);
            return;
        case Op::minmax_imm: {
            unsigned opc = in.bits(19, 18), imm = in.bits(17, 10);
            bool is_signed = !(opc & 1), is_max = !(opc & 2);
            std::string t = wide ? (is_signed ? "int64_t" : "uint64_t") : (is_signed ? "int32_t" : "uint32_t");
            std::string value = is_signed ? std::to_string(sign_extend(imm, 8)) : std::to_string(imm);
            std::string n = "(" + t + ")" + get(in.rn(), wide);
            std::string pick = is_max ? "(" + n + " > (" + t + ")" + value + " ? " + n + " : (" + t + ")" + value + ")"
                                      : "(" + n + " < (" + t + ")" + value + " ? " + n + " : (" + t + ")" + value + ")";
            line(set(in.rd(), wide, pick));
            return;
        }
        case Op::invalid:
            trap(in, "invalid instruction");
            return;
        default:
            trap(in, "not translated yet");
            return;
        }
    }

    void system_register(const Instruction& in) {
        unsigned reg = in.bits(20, 5);
        bool read = in.op == Op::mrs;
        constexpr unsigned tpidr = 0xde82, nzcv = 0xda10, fpcr = 0xda20, fpsr = 0xda21;
        if (read) {
            switch (reg) {
            case tpidr: line(set(in.rt(), true, "s->tpidr")); return;
            case nzcv: line(set(in.rt(), true, "((uint64_t)s->nf << 31) | ((uint64_t)s->zf << 30) | ((uint64_t)s->cf << 29) | ((uint64_t)s->vf << 28)")); return;
            case fpcr: line(set(in.rt(), true, "s->fpcr")); return;
            case fpsr: line(set(in.rt(), true, "s->fpsr")); return;
            default: line(set(in.rt(), true, "tsugiki_mrs(s, " + std::to_string(reg) + ")")); return;
            }
        }
        std::string value = get(in.rt(), true);
        switch (reg) {
        case tpidr: line("s->tpidr = " + value + ";"); return;
        case nzcv: line("ts_nzcv(s, (uint32_t)(" + value + " >> 28));"); return;
        case fpcr: line("s->fpcr = (uint32_t)" + value + ";"); return;
        case fpsr: line("s->fpsr = (uint32_t)" + value + ";"); return;
        default: line("tsugiki_msr(s, " + std::to_string(reg) + ", " + value + ");"); return;
        }
    }

    void exclusive(const Instruction& in) {
        bool pair = in.access.pair, load = in.access.load;
        unsigned bytes = in.access.bytes, total = pair ? bytes * 2 : bytes;
        std::string a = get(in.rn(), true, true);
        if (load) {
            std::string t = total == 16 ? "unsigned __int128" : utype(total);
            line("{ uint64_t a = " + a + "; " + t + " v; memcpy(&v, (const void*)a, " + std::to_string(total) + ");");
            line("  s->excl = 1; s->excl_addr = a; s->excl_size = " + std::to_string(total) + "; s->excl_lo = (uint64_t)v; s->excl_hi = " +
                 (total == 16 ? "(uint64_t)(v >> 64)" : "0") + ";");
            if (pair) {
                std::string mask = bytes == 8 ? "" : " & 0xffffffffull";
                line("  " + set(in.rt(), true, "(uint64_t)v" + mask) + " " + set(in.rt2(), true, "(uint64_t)(v >> " + std::to_string(bytes * 8) + ")" + mask) + " }");
            } else {
                line("  " + set(in.rt(), true, "(uint64_t)v") + " }");
            }
            return;
        }
        std::string t = total == 16 ? "unsigned __int128" : utype(total);
        std::string value;
        if (pair) {
            std::string low = get(in.rt(), true), high = get(in.rt2(), true);
            value = bytes == 8 ? "((unsigned __int128)" + high + " << 64) | " + low : "((uint64_t)(uint32_t)" + high + " << 32) | (uint32_t)" + low;
        } else {
            value = "(" + t + ")" + get(in.rt(), true);
        }
        std::string expected = total == 16 ? "((unsigned __int128)s->excl_hi << 64) | s->excl_lo" : "(" + t + ")s->excl_lo";
        line("{ uint64_t a = " + a + "; uint32_t fail = 1;");
        line("  if (s->excl && s->excl_addr == a && s->excl_size == " + std::to_string(total) + ") { " + t + " expected = " + expected + "; " + t +
             " desired = " + value + "; fail = !__atomic_compare_exchange_n((" + t + "*)a, &expected, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }");
        line("  s->excl = 0; " + set(in.rs(), false, "fail") + " }");
    }

    void atomic(const Instruction& in) {
        unsigned bytes = in.access.bytes;
        std::string t = utype(bytes);
        std::string a = "(" + t + "*)" + get(in.rn(), true, true);
        if (in.op == Op::cas) {
            line("{ " + t + " expected = (" + t + ")" + get(in.rs(), true) + "; __atomic_compare_exchange_n(" + a + ", &expected, (" + t + ")" +
                 get(in.rt(), true) + ", false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); " + set(in.rs(), true, "(uint64_t)expected") + " }");
            return;
        }
        if (in.op == Op::casp) {
            bool wide = in.bit(30);
            std::string pt = wide ? "unsigned __int128" : "uint64_t";
            auto both = [&](int r) {
                return wide ? "((unsigned __int128)" + get(r + 1, true) + " << 64) | " + get(r, true)
                            : "((uint64_t)(uint32_t)" + get(r + 1, true) + " << 32) | (uint32_t)" + get(r, true);
            };
            std::string mask = wide ? "" : " & 0xffffffffull";
            line("{ " + pt + " expected = " + both(in.rs()) + "; __atomic_compare_exchange_n((" + pt + "*)" + get(in.rn(), true, true) +
                 ", &expected, " + both(in.rt()) + ", false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); " + set(in.rs(), true, "(uint64_t)expected" + mask) +
                 " " + set(in.rs() + 1, true, std::string("(uint64_t)(expected >> ") + (wide ? "64" : "32") + ")" + mask) + " }");
            return;
        }
        std::string v = "(" + t + ")" + get(in.rs(), true);
        std::string old;
        if (in.op == Op::swp) {
            old = "__atomic_exchange_n(" + a + ", " + v + ", __ATOMIC_SEQ_CST)";
        } else {
            switch (in.bits(14, 12)) {
            case 0: old = "__atomic_fetch_add(" + a + ", " + v + ", __ATOMIC_SEQ_CST)"; break;
            case 1: old = "__atomic_fetch_and(" + a + ", (" + t + ")~" + v + ", __ATOMIC_SEQ_CST)"; break;
            case 2: old = "__atomic_fetch_xor(" + a + ", " + v + ", __ATOMIC_SEQ_CST)"; break;
            case 3: old = "__atomic_fetch_or(" + a + ", " + v + ", __ATOMIC_SEQ_CST)"; break;
            case 4: old = "ts_atomic_smax(" + a + ", " + v + ")"; break;
            case 5: old = "ts_atomic_smin(" + a + ", " + v + ")"; break;
            case 6: old = "ts_atomic_umax(" + a + ", " + v + ")"; break;
            default: old = "ts_atomic_umin(" + a + ", " + v + ")"; break;
            }
        }
        line(set(in.rt(), true, "(uint64_t)" + old));
    }

    void memory(const Instruction& in) {
        const a64::Access& a = in.access;
        if (a.prefetch) {
            return;
        }
        unsigned scale = log2_bytes(a.bytes);
        std::string base = get(in.rn(), true, true);
        std::string address;
        // base written back after the access, if at all
        std::optional<std::string> writeback;
        switch (in.op) {
        case Op::ldr_literal:
            address = "TB + " + hex(pc_ + uint64_t(sign_extend(in.bits(23, 5), 19) * 4));
            break;
        case Op::single_unsigned:
            address = base + " + " + hex(uint64_t(in.bits(21, 10)) << scale);
            break;
        case Op::single_unscaled:
        case Op::single_unprivileged:
        case Op::rcpc_unscaled:
            address = base + " + (uint64_t)" + std::to_string(sign_extend(in.bits(20, 12), 9)) + "ll";
            break;
        case Op::single_pre:
            address = base + " + (uint64_t)" + std::to_string(sign_extend(in.bits(20, 12), 9)) + "ll";
            writeback = "a";
            break;
        case Op::single_post:
            address = base;
            writeback = "a + (uint64_t)" + std::to_string(sign_extend(in.bits(20, 12), 9)) + "ll";
            break;
        case Op::single_register: {
            unsigned amount = in.bit(12) ? scale : 0;
            address = base + " + (" + extended(in.rm(), in.bits(15, 13)) + " << " + std::to_string(amount) + ")";
            break;
        }
        case Op::pair_noalloc:
        case Op::pair_offset:
            address = base + " + (uint64_t)" + std::to_string(sign_extend(in.bits(21, 15), 7) * a.bytes) + "ll";
            break;
        case Op::pair_pre:
            address = base + " + (uint64_t)" + std::to_string(sign_extend(in.bits(21, 15), 7) * a.bytes) + "ll";
            writeback = "a";
            break;
        case Op::pair_post:
            address = base;
            writeback = "a + (uint64_t)" + std::to_string(sign_extend(in.bits(21, 15), 7) * a.bytes) + "ll";
            break;
        case Op::ldraa:
        case Op::ldrab: {
            int64_t offset = sign_extend((in.bit(22) << 9) | in.bits(20, 12), 10) * 8;
            address = base + " + (uint64_t)" + std::to_string(offset) + "ll";
            if (in.bit(11)) {
                writeback = "a";
            }
            break;
        }
        default:
            trap(in, "bad memory op");
            return;
        }
        out_ << "    { uint64_t a = " << address << ";";
        if (a.pair) {
            std::string second = "a + " + std::to_string(a.bytes);
            if (a.load) {
                if (a.simd) {
                    out_ << " tsugiki_vreg v1 = {0, 0}, v2 = {0, 0};" << simd_load("v1", a.bytes, "a") << simd_load("v2", a.bytes, second);
                } else {
                    out_ << " uint64_t v1 = " << loaded(a, load(a.bytes, "a")) << "; uint64_t v2 = " << loaded(a, load(a.bytes, second)) << ";";
                }
            } else if (a.simd) {
                out_ << simd_store(in.rt(), a.bytes, "a") << simd_store(in.rt2(), a.bytes, second);
            } else {
                out_ << " " << store(a.bytes, "a", get(in.rt(), true)) << " " << store(a.bytes, second, get(in.rt2(), true));
            }
        } else if (a.load) {
            if (a.simd) {
                out_ << " tsugiki_vreg v1 = {0, 0};" << simd_load("v1", a.bytes, "a");
            } else {
                out_ << " uint64_t v1 = " << loaded(a, load(a.bytes, "a")) << ";";
            }
        } else if (a.simd) {
            out_ << simd_store(in.rt(), a.bytes, "a");
        } else {
            out_ << " " << store(a.bytes, "a", get(in.rt(), true));
        }
        if (writeback) {
            out_ << " " << x(in.rn()) << " = " << *writeback << ";";
        }
        if (a.load) {
            if (a.simd) {
                out_ << " s->v[" << in.rt() << "] = v1;";
                if (a.pair) {
                    out_ << " s->v[" << in.rt2() << "] = v2;";
                }
            } else {
                out_ << " " << set(in.rt(), true, "v1");
                if (a.pair) {
                    out_ << " " << set(in.rt2(), true, "v2");
                }
            }
        }
        out_ << " }\n";
    }

    static std::string simd_load(const std::string& into, unsigned bytes, const std::string& address) {
        if (bytes == 16) {
            return " " + into + ".lo = ts_ld64(" + address + "); " + into + ".hi = ts_ld64(" + address + " + 8);";
        }
        return " " + into + ".lo = (uint64_t)" + load(bytes, address) + ";";
    }

    static std::string simd_store(int reg, unsigned bytes, const std::string& address) {
        std::string v = "s->v[" + std::to_string(reg) + "]";
        if (bytes == 16) {
            return " ts_st64(" + address + ", " + v + ".lo); ts_st64(" + address + " + 8, " + v + ".hi);";
        }
        return " " + store(bytes, address, v + ".lo");
    }

    void data2(const Instruction& in) {
        bool wide = in.sf();
        std::string w = wide ? "64" : "32", t = wide ? "uint64_t" : "uint32_t", st = wide ? "int64_t" : "int32_t";
        std::string n = get(in.rn(), wide), m = get(in.rm(), wide);
        std::string bits = wide ? "63" : "31";
        unsigned opcode = in.bits(15, 10);
        switch (opcode) {
        case 2: line(set(in.rd(), wide, "ts_udiv" + w + "(" + n + ", " + m + ")")); return;
        case 3: line(set(in.rd(), wide, "(" + t + ")ts_sdiv" + w + "((" + st + ")" + n + ", (" + st + ")" + m + ")")); return;
        case 8: line(set(in.rd(), wide, "(" + t + ")(" + n + " << (" + m + " & " + bits + "))")); return;
        case 9: line(set(in.rd(), wide, "(" + t + ")(" + n + " >> (" + m + " & " + bits + "))")); return;
        case 10: line(set(in.rd(), wide, "(" + t + ")((" + st + ")" + n + " >> (" + m + " & " + bits + "))")); return;
        case 11: line(set(in.rd(), wide, "ts_ror" + w + "(" + n + ", (unsigned)" + m + ")")); return;
        case 12: line(set(in.rd(), true, "(" + get(in.rn(), true) + " ^ " + get(in.rm(), true) + ") & 0xffffffff00000000ull")); return;
        case 16: case 17: case 18: case 19: case 20: case 21: case 22: case 23: {
            unsigned size = opcode & 3;
            bool castagnoli = opcode & 4;
            line(set(in.rd(), false, "tsugiki_crc32((uint32_t)" + get(in.rn(), true) + ", " + get(in.rm(), true) + ", " + std::to_string(1 << size) + ", " +
                                         (castagnoli ? "1" : "0") + ")"));
            return;
        }
        case 24: case 25: case 26: case 27: {
            bool is_signed = !(opcode & 1), is_max = !(opcode & 2);
            std::string ty = is_signed ? st : t;
            std::string a = "(" + ty + ")" + n, b = "(" + ty + ")" + m;
            line(set(in.rd(), wide, "(" + a + (is_max ? " > " : " < ") + b + " ? " + a + " : " + b + ")"));
            return;
        }
        default:
            trap(in, "bad dp2");
            return;
        }
    }

    void data1(const Instruction& in) {
        bool wide = in.sf();
        std::string w = wide ? "64" : "32", t = wide ? "uint64_t" : "uint32_t", st = wide ? "int64_t" : "int32_t";
        std::string n = get(in.rn(), wide);
        if (in.bits(20, 16) == 1) {
            // pac is off for us, signing and checking leave pointers alone
            return;
        }
        switch (in.bits(15, 10)) {
        case 0: line(set(in.rd(), wide, (wide ? "__builtin_bitreverse64(" : "__builtin_bitreverse32(") + n + ")")); return;
        case 1:
            line(set(in.rd(), wide, wide ? "((" + n + " & 0x00ff00ff00ff00ffull) << 8) | ((" + n + " >> 8) & 0x00ff00ff00ff00ffull)"
                                         : "(uint32_t)(((" + n + " & 0x00ff00ffu) << 8) | ((" + n + " >> 8) & 0x00ff00ffu))"));
            return;
        case 2:
            line(set(in.rd(), wide, wide ? "ts_ror64(__builtin_bswap64(" + n + "), 32)" : "__builtin_bswap32(" + n + ")"));
            return;
        case 3: line(set(in.rd(), wide, "__builtin_bswap64(" + n + ")")); return;
        case 4: line(set(in.rd(), wide, "ts_clz" + w + "(" + n + ")")); return;
        case 5: line(set(in.rd(), wide, "ts_cls" + w + "(" + n + ")")); return;
        case 6: line(set(in.rd(), wide, "ts_ctz" + w + "(" + n + ")")); return;
        case 7: line(set(in.rd(), wide, (wide ? "(uint64_t)__builtin_popcountll(" : "(uint32_t)__builtin_popcount(") + n + ")")); return;
        case 8: line(set(in.rd(), wide, "(" + t + ")((" + st + ")" + n + " < 0 ? -(" + t + ")" + n + " : " + n + ")")); return;
        default: trap(in, "bad dp1"); return;
        }
    }

    void data3(const Instruction& in) {
        bool wide = in.sf();
        std::string t = wide ? "uint64_t" : "uint32_t";
        std::string n = get(in.rn(), wide), m = get(in.rm(), wide), a = get(in.ra(), wide);
        switch (in.bits(23, 21) * 2 + in.bit(15)) {
        case 0: line(set(in.rd(), wide, "(" + t + ")(" + a + " + " + n + " * " + m + ")")); return;
        case 1: line(set(in.rd(), wide, "(" + t + ")(" + a + " - " + n + " * " + m + ")")); return;
        case 2: case 3: case 10: case 11: {
            bool is_signed = in.bits(23, 21) == 1, sub = in.bit(15);
            std::string ext = is_signed ? "(uint64_t)(int64_t)(int32_t)" : "(uint64_t)(uint32_t)";
            std::string product = ext + get(in.rn(), true) + " * " + ext + get(in.rm(), true);
            line(set(in.rd(), true, get(in.ra(), true) + (sub ? " - " : " + ") + product));
            return;
        }
        case 4:
            line(set(in.rd(), true, "(uint64_t)(((__int128)(int64_t)" + get(in.rn(), true) + " * (int64_t)" + get(in.rm(), true) + ") >> 64)"));
            return;
        case 12:
            line(set(in.rd(), true, "(uint64_t)(((unsigned __int128)" + get(in.rn(), true) + " * " + get(in.rm(), true) + ") >> 64)"));
            return;
        default:
            trap(in, "bad dp3");
            return;
        }
    }

    const Program& program_;
    const Function& function_;
    std::ostringstream& out_;
    uint64_t pc_ = 0;
    uint64_t next_ = 0;
};

}  // namespace

Program discover(const elf::Library& library) {
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    for (const eh_frame::Fde& f : eh_frame::read(library)) {
        if (f.size > 0 && library.executable(f.start)) {
            ranges.emplace_back(f.start, f.start + f.size);
        }
    }
    // exported code without an fde, hand-written asm mostly
    auto covered = [&](uint64_t address) {
        for (auto& [start, end] : ranges) {
            if (address >= start && address < end) {
                return true;
            }
        }
        return false;
    };
    for (const auto* symbols : {&library.dynamic_symbols, &library.static_symbols}) {
        for (const elf::Symbol& s : *symbols) {
            if (s.defined() && s.code() && s.size > 0 && library.executable(s.value) && !covered(s.value)) {
                ranges.emplace_back(s.value, s.value + s.size);
            }
        }
    }
    std::sort(ranges.begin(), ranges.end());

    // drop overlaps, keep the first
    std::map<uint64_t, uint64_t> known;
    uint64_t last_end = 0;
    for (auto [start, end] : ranges) {
        if (start >= last_end && (start & 3) == 0) {
            known[start] = start + (end - start) / 4 * 4;
            last_end = end;
        }
    }

    // code without an fde that something branches to, plt stubs mostly:
    // decode until an unconditional jump or the next function
    std::vector<uint64_t> pending;
    auto inside_known = [&](uint64_t address) {
        auto it = known.upper_bound(address);
        return it != known.begin() && address < std::prev(it)->second;
    };
    auto scan = [&](uint64_t start, uint64_t end) {
        auto bytes = library.at(start, end - start);
        if (!bytes) {
            return;
        }
        Reader r(*bytes);
        for (uint64_t pc = start; r.left() >= 4; pc += 4) {
            Instruction in = a64::decode(r.u32());
            if (auto target = branch_target(in, pc); target && !inside_known(*target) && library.executable(*target) && (*target & 3) == 0) {
                pending.push_back(*target);
            }
        }
    };
    for (auto [start, end] : known) {
        scan(start, end);
    }
    while (!pending.empty()) {
        uint64_t start = pending.back();
        pending.pop_back();
        if (inside_known(start)) {
            continue;
        }
        auto next = known.upper_bound(start);
        uint64_t limit = next != known.end() ? next->first : library.loaded(start)->vaddr + library.loaded(start)->filesz;
        uint64_t end = start;
        while (end < limit) {
            auto word = library.at(end, 4);
            if (!word) {
                break;
            }
            uint32_t raw;
            std::memcpy(&raw, word->data(), 4);
            Instruction in = a64::decode(raw);
            end += 4;
            if (!in.valid() || in.op == Op::b || in.op == Op::br || in.op == Op::ret || in.op == Op::braa || in.op == Op::brab ||
                in.op == Op::braaz || in.op == Op::brabz || in.op == Op::retaa || in.op == Op::retab) {
                break;
            }
        }
        known[start] = end;
        scan(start, end);
    }

    Program program;
    for (auto [start, end] : known) {
        auto bytes = library.at(start, end - start);
        if (!bytes) {
            continue;
        }
        Function f;
        f.start = start;
        f.end = end;
        Reader r(*bytes);
        while (r.left() >= 4) {
            f.code.push_back(a64::decode(r.u32()));
        }
        f.entries.insert(start);
        for (size_t i = 0; i < f.code.size(); ++i) {
            uint64_t pc = start + 4 * i;
            if (auto target = branch_target(f.code[i], pc); target && f.code[i].op != Op::bl && *target >= f.start && *target < f.end) {
                f.labels.insert(*target);
            }
            if (is_call(f.code[i]) && pc + 4 < f.end) {
                f.entries.insert(pc + 4);
            }
        }
        program.starts[start] = program.functions.size();
        program.functions.push_back(std::move(f));
    }
    return program;
}

std::string emit(const Program& program, const std::string& id) {
    std::ostringstream out;
    out << "// made by tsugiki, don't edit\n";
    out << "#include \"tsugiki.h\"\n\n";
    out << "extern \"C\" uint64_t tsugiki_base_" << id << ";\n";
    out << "#define TB tsugiki_base_" << id << "\n\n";
    out << "#pragma clang diagnostic ignored \"-Wunused-label\"\n\n";
    for (const Function& f : program.functions) {
        out << "static void f_" << name(f.start) << "(tsugiki_state* __restrict s);\n";
    }
    out << "\n";
    for (const Function& f : program.functions) {
        Emitter(program, f, out).function();
    }
    // the dispatcher's table, sorted by address
    std::vector<std::pair<uint64_t, uint64_t>> entries;
    for (const Function& f : program.functions) {
        for (uint64_t entry : f.entries) {
            entries.emplace_back(entry, f.start);
        }
    }
    std::sort(entries.begin(), entries.end());
    out << "extern \"C\" uint64_t tsugiki_base_" << id << " = 0;\n";
    out << "extern \"C\" const tsugiki_entry tsugiki_entries_" << id << "[] = {\n";
    for (auto [entry, function] : entries) {
        out << "    {" << hex(entry) << ", f_" << name(function) << "},\n";
    }
    out << "};\n";
    out << "extern \"C\" const uint64_t tsugiki_entry_count_" << id << " = " << entries.size() << ";\n";
    return out.str();
}

}  // namespace tsugiki::lift
