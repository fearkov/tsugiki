// guest code w/o a translation, one instruction at a time
// runs til its function returns or it lands on code we did translate
// same semantics as the lifter's c++, keep them in sync
#include "interp.hpp"

#include <algorithm>
#include <cstring>
#include <memory>

#include "a64/decoder.hpp"
#include "a64/simd.hpp"

namespace {

namespace a64 = tsugiki::a64;
namespace interp = tsugiki::interp;
using a64::Instruction;
using a64::Op;
using u128 = unsigned __int128;

int64_t sext(uint64_t value, int bits) {
    uint64_t sign = uint64_t(1) << (bits - 1);
    return int64_t((value ^ sign) - sign);
}

bool structure_op(Op op) {
    return op == Op::simd_multiple || op == Op::simd_multiple_post || op == Op::simd_single || op == Op::simd_single_post;
}

// decoded words by pc so loops don't decode every lap
struct Slot {
    uint64_t pc = ~uint64_t(0);
    Instruction in;
    // the lib's code for an fp/simd word
    tsugiki_code op = nullptr;
    // translated code starting right here
    tsugiki_code here = nullptr;
};

constexpr size_t slot_count = 4096;
thread_local std::unique_ptr<Slot[]> slots;

const Slot& fetch(uint64_t pc) {
    if (!slots) {
        slots.reset(new Slot[slot_count]);
    }
    Slot& slot = slots[(pc >> 2) & (slot_count - 1)];
    uint32_t word = ts_ld32(pc);
    if (slot.pc != pc || slot.in.raw != word) {
        interp::Place p = interp::place(pc);
        slot.pc = pc;
        slot.in = a64::decode(word);
        slot.here = p.code;
        slot.op = nullptr;
        if (p.ops && (a64::is_simd(slot.in.op) || structure_op(slot.in.op))) {
            const tsugiki_op* end = p.ops + p.op_count;
            const tsugiki_op* o = std::lower_bound(p.ops, end, word, [](const tsugiki_op& a, uint32_t w) { return a.word < w; });
            if (o != end && o->word == word) {
                slot.op = o->code;
            }
        }
    }
    return slot;
}

bool holds(const tsugiki_flags& fl, unsigned code) {
    switch (code & 15) {
    case 0: return fl.zf;
    case 1: return !fl.zf;
    case 2: return fl.cf;
    case 3: return !fl.cf;
    case 4: return fl.nf;
    case 5: return !fl.nf;
    case 6: return fl.vf;
    case 7: return !fl.vf;
    case 8: return fl.cf && !fl.zf;
    case 9: return !(fl.cf && !fl.zf);
    case 10: return fl.nf == fl.vf;
    case 11: return fl.nf != fl.vf;
    case 12: return !fl.zf && fl.nf == fl.vf;
    case 13: return !(!fl.zf && fl.nf == fl.vf);
    default: return true;
    }
}

uint64_t width_mask(bool wide) { return wide ? ~uint64_t(0) : 0xffffffffull; }

uint64_t shifted(uint64_t value, unsigned type, unsigned amount, bool wide) {
    if (amount == 0) {
        return value;
    }
    if (wide) {
        switch (type) {
        case 0: return value << amount;
        case 1: return value >> amount;
        case 2: return uint64_t(int64_t(value) >> amount);
        default: return ts_ror64(value, amount);
        }
    }
    uint32_t w = uint32_t(value);
    switch (type) {
    case 0: return uint32_t(w << amount);
    case 1: return w >> amount;
    case 2: return uint32_t(int32_t(w) >> amount);
    default: return ts_ror32(w, amount);
    }
}

// uxtb thru sxtx of a 64 bit value
uint64_t extended(uint64_t v, unsigned option) {
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

uint64_t load(unsigned bytes, uint64_t a) {
    switch (bytes) {
    case 1: return ts_ld8(a);
    case 2: return ts_ld16(a);
    case 4: return ts_ld32(a);
    default: return ts_ld64(a);
    }
}

void store(unsigned bytes, uint64_t a, uint64_t v) {
    switch (bytes) {
    case 1: ts_st8(a, uint8_t(v)); break;
    case 2: ts_st16(a, uint16_t(v)); break;
    case 4: ts_st32(a, uint32_t(v)); break;
    default: ts_st64(a, v); break;
    }
}

// what an integer load leaves in a register
uint64_t loaded(const a64::Access& a, uint64_t raw) {
    if (!a.sign) {
        return raw;
    }
    int shift = 64 - 8 * a.bytes;
    int64_t v = int64_t(raw << shift) >> shift;
    return a.wide ? uint64_t(v) : uint64_t(uint32_t(v));
}

tsugiki_vreg vload(unsigned bytes, uint64_t a) {
    tsugiki_vreg v{0, 0};
    if (bytes == 16) {
        v.lo = ts_ld64(a);
        v.hi = ts_ld64(a + 8);
    } else {
        v.lo = load(bytes, a);
    }
    return v;
}

void vstore(unsigned bytes, uint64_t a, const tsugiki_vreg& v) {
    if (bytes == 16) {
        ts_st64(a, v.lo);
        ts_st64(a + 8, v.hi);
    } else {
        store(bytes, a, v.lo);
    }
}

template <typename T>
bool cas(uint64_t a, T expected, T desired) {
    return __atomic_compare_exchange_n(reinterpret_cast<T*>(a), &expected, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

// compare and swap of 1 to 16 bytes, the old value back in expected
template <typename T>
T swap_in(uint64_t a, T expected, T desired) {
    __atomic_compare_exchange_n(reinterpret_cast<T*>(a), &expected, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
}

template <typename T>
uint64_t fetch_op(unsigned which, uint64_t a, uint64_t value, bool swap) {
    T* p = reinterpret_cast<T*>(a);
    T v = T(value);
    if (swap) {
        return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST);
    }
    switch (which) {
    case 0: return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST);
    case 1: return __atomic_fetch_and(p, T(~v), __ATOMIC_SEQ_CST);
    case 2: return __atomic_fetch_xor(p, v, __ATOMIC_SEQ_CST);
    case 3: return __atomic_fetch_or(p, v, __ATOMIC_SEQ_CST);
    case 4: return ts_atomic_smax(p, v);
    case 5: return ts_atomic_smin(p, v);
    case 6: return ts_atomic_umax(p, v);
    default: return ts_atomic_umin(p, v);
    }
}

class Interpreter {
public:
    explicit Interpreter(tsugiki_state* s) : s_(s), fl_(ts_flags_in(s)) {}

    void run() {
        uint64_t pc = s_->pc;
        for (;;) {
            const Slot& slot = fetch(pc);
            Instruction in = slot.in;
            op_ = slot.op;
            pc_ = pc;
            next_ = pc + 4;
            if (!step(in)) {
                return;
            }
            pc = next_;
        }
    }

private:
    // x registers, 31 is zr unless sp says otherwise
    uint64_t get(int n, bool wide, bool sp = false) const {
        uint64_t v = n == 31 && !sp ? 0 : s_->x[n];
        return wide ? v : uint32_t(v);
    }

    void set(int n, bool wide, uint64_t v, bool sp = false) {
        if (n == 31 && !sp) {
            return;
        }
        s_->x[n] = wide ? v : uint32_t(v);
    }

    void trap(const Instruction& in, const char* why) { tsugiki_trap(s_, pc_, in.raw, why); }

    // a taken branch, false once translated code took over and returned
    bool jump(uint64_t target) {
        if (tsugiki_code code = fetch(target).here) {
            ts_flags_out(s_, fl_);
            s_->pc = target;
            code(s_);
            return false;
        }
        next_ = target;
        return true;
    }

    void dispatch() {
        if (s_->cache) {
            tsugiki_dispatch(s_);
        } else {
            tsugiki_call(s_);
        }
    }

    // bl and blr, false if the guest unwound past us
    bool call(uint64_t target) {
        s_->x[30] = next_;
        s_->pc = target;
        ts_flags_out(s_, fl_);
        dispatch();
        if (s_->pc != next_) {
            return false;
        }
        fl_ = ts_flags_in(s_);
        // back in some function we translated after all
        if (tsugiki_code code = fetch(next_).here) {
            code(s_);
            return false;
        }
        return true;
    }

    // an fp/simd word, run by the lib's own code for it, scalar fp by hand when there's none (jit code)
    bool lib_op(const Instruction& in) {
        if (op_ == nullptr) {
            if (scalar_fp(in)) {
                return true;
            }
            trap(in, "no code for this fp/simd word");
        }
        ts_flags_out(s_, fl_);
        op_(s_);
        fl_ = ts_flags_in(s_);
        return true;
    }

    template <typename F>
    F fget(int n) const {
        F f;
        std::memcpy(&f, &s_->v[n], sizeof(F));
        return f;
    }

    // scalar writes zero the rest of the register
    template <typename F>
    void fset(int d, F f) {
        s_->v[d] = {0, 0};
        std::memcpy(&s_->v[d], &f, sizeof(F));
    }

    // same as the lifter's fp code, the helpers come from tsugiki_simd.h
    bool scalar_fp(const Instruction& in) {
        switch (in.op) {
        case Op::fp_1src:
        case Op::fp_2src:
        case Op::fp_3src:
        case Op::fp_compare:
        case Op::fp_ccmp:
        case Op::fp_csel:
        case Op::fp_imm:
            switch (in.bits(23, 22)) {
            case 0: return fp_ops<float>(in, 2);
            case 1: return fp_ops<double>(in, 3);
            case 3: return fp_ops<_Float16>(in, 1);
            default: return false;
            }
        case Op::fp_int:
        case Op::fp_fixed:
            switch (in.bits(23, 22)) {
            case 0: return fp_int<float>(in);
            case 1: return fp_int<double>(in);
            case 3: return fp_int<_Float16>(in);
            default: return false;
            }
        default:
            return false;
        }
    }

    template <typename F>
    bool fp_ops(const Instruction& in, int size) {
        int d = in.rd(), n = in.rn(), m = in.rm();
        switch (in.op) {
        case Op::fp_1src: {
            F a = fget<F>(n);
            switch (in.bits(20, 15)) {
            case 0: fset(d, a); return true;
            case 1: fset(d, __builtin_elementwise_abs(a)); return true;
            case 2: fset(d, F(-a)); return true;
            case 3: fset(d, __builtin_elementwise_sqrt(a)); return true;
            case 4: fset(d, float(a)); return true;
            case 5: fset(d, double(a)); return true;
            case 7: fset(d, _Float16(a)); return true;
            case 8: case 14: case 15: fset(d, __builtin_elementwise_roundeven(a)); return true;
            case 9: fset(d, __builtin_elementwise_ceil(a)); return true;
            case 10: fset(d, __builtin_elementwise_floor(a)); return true;
            case 11: fset(d, __builtin_elementwise_trunc(a)); return true;
            case 12: fset(d, __builtin_elementwise_round(a)); return true;
            default: return false;
            }
        }
        case Op::fp_2src: {
            F a = fget<F>(n), b = fget<F>(m), v;
            switch (in.bits(15, 12)) {
            case 0: v = a * b; break;
            case 1: v = a / b; break;
            case 2: v = a + b; break;
            case 3: v = a - b; break;
            case 4: v = __builtin_elementwise_maximum(a, b); break;
            case 5: v = __builtin_elementwise_minimum(a, b); break;
            case 6: v = ts_maxnum(a, b); break;
            case 7: v = ts_minnum(a, b); break;
            case 8: v = -(a * b); break;
            default: return false;
            }
            fset(d, v);
            return true;
        }
        case Op::fp_3src: {
            F x = fget<F>(n), y = fget<F>(m), a = fget<F>(in.ra());
            switch (in.bit(21) * 2 + in.bit(15)) {
            case 0: fset(d, __builtin_elementwise_fma(x, y, a)); break;
            case 1: fset(d, __builtin_elementwise_fma(F(-x), y, a)); break;
            case 2: fset(d, __builtin_elementwise_fma(F(-x), y, F(-a))); break;
            default: fset(d, __builtin_elementwise_fma(x, y, F(-a))); break;
            }
            return true;
        }
        case Op::fp_compare:
            ts_fcmp(&fl_, fget<F>(n), in.bit(3) ? F(0) : fget<F>(m));
            return true;
        case Op::fp_ccmp:
            if (holds(fl_, in.bits(15, 12))) {
                ts_fcmp(&fl_, fget<F>(n), fget<F>(m));
            } else {
                ts_nzcv(&fl_, in.bits(3, 0));
            }
            return true;
        case Op::fp_csel:
            fset(d, holds(fl_, in.bits(15, 12)) ? fget<F>(n) : fget<F>(m));
            return true;
        case Op::fp_imm: {
            // VFPExpandImm
            uint64_t imm8 = in.bits(20, 13), sign = imm8 >> 7, b = (imm8 >> 6) & 1, cd = (imm8 >> 4) & 3, efgh = imm8 & 15;
            int e = size == 1 ? 5 : size == 2 ? 8 : 11, f = size == 1 ? 10 : size == 2 ? 23 : 52;
            uint64_t exp = ((b ^ 1) << (e - 1)) | (b ? ((uint64_t(1) << (e - 3)) - 1) << 2 : 0) | cd;
            s_->v[d] = {(sign << (e + f)) | (exp << f) | (efgh << (f - 4)), 0};
            return true;
        }
        default:
            return false;
        }
    }

    template <typename I, typename F>
    I to_int(F f, unsigned rmode, bool away) {
        if (away) {
            return ts_to_int<I, ts_away>(f);
        }
        switch (rmode) {
        case 0: return ts_to_int<I, ts_nearest>(f);
        case 1: return ts_to_int<I, ts_plus>(f);
        case 2: return ts_to_int<I, ts_minus>(f);
        default: return ts_to_int<I, ts_zero>(f);
        }
    }

    template <typename F>
    uint64_t to_int(F f, bool sf, bool is_signed, unsigned rmode, bool away) {
        if (is_signed) {
            return sf ? uint64_t(to_int<int64_t>(f, rmode, away)) : uint64_t(uint32_t(to_int<int32_t>(f, rmode, away)));
        }
        return sf ? to_int<uint64_t>(f, rmode, away) : to_int<uint32_t>(f, rmode, away);
    }

    // fmov between registers, int conversions, fixed point
    template <typename F>
    bool fp_int(const Instruction& in) {
        unsigned ftype = in.bits(23, 22);
        bool sf = in.sf();
        int d = in.rd(), n = in.rn();
        uint64_t mask = ftype == 3 ? 0xffff : ftype == 0 ? 0xffffffffull : ~uint64_t(0);
        if (in.op == Op::fp_fixed) {
            unsigned opcode = in.bits(20, 16);
            int fbits = 64 - int(in.bits(15, 10));
            F unscale = F(__builtin_ldexp(1.0, -fbits)), scale = F(__builtin_ldexp(1.0, fbits));
            if (opcode == 0b00010 || opcode == 0b00011) {
                uint64_t x = get(n, true);
                F v = opcode == 0b00010 ? (sf ? F(int64_t(x)) : F(int32_t(x))) : (sf ? F(x) : F(uint32_t(x)));
                fset(d, F(v * unscale));
                return true;
            }
            set(d, sf, to_int(F(fget<F>(n) * scale), sf, opcode == 0b11000, 3, false));
            return true;
        }
        unsigned rmode = in.bits(20, 19), opcode = in.bits(18, 16);
        if (opcode == 7) {
            if (rmode == 1) {
                s_->v[d].hi = get(n, true);
            } else {
                s_->v[d] = {get(n, true) & mask, 0};
            }
            return true;
        }
        if (opcode == 6) {
            if (rmode == 1) {
                set(d, true, s_->v[n].hi);
            } else if (rmode == 3) {
                set(d, false, uint32_t(tsugiki_fjcvtzs(s_, fget<double>(n))));
            } else {
                set(d, sf, s_->v[n].lo & mask);
            }
            return true;
        }
        if (opcode == 2 || opcode == 3) {
            uint64_t x = get(n, true);
            fset(d, opcode == 2 ? (sf ? F(int64_t(x)) : F(int32_t(x))) : (sf ? F(x) : F(uint32_t(x))));
            return true;
        }
        set(d, sf, to_int(fget<F>(n), sf, !(opcode & 1), rmode, opcode >= 4));
        return true;
    }

    bool step(const Instruction& in) {
        bool wide = in.sf();
        switch (in.op) {
        case Op::adr:
        case Op::adrp: {
            uint64_t imm = uint64_t(sext((in.bits(23, 5) << 2) | in.bits(30, 29), 21));
            set(in.rd(), true, in.op == Op::adr ? pc_ + imm : (pc_ & ~uint64_t(0xfff)) + (imm << 12));
            return true;
        }
        case Op::add_imm:
        case Op::adds_imm:
        case Op::sub_imm:
        case Op::subs_imm: {
            uint64_t imm = uint64_t(in.bits(21, 10)) << (in.bit(22) ? 12 : 0);
            uint64_t n = get(in.rn(), wide, true);
            bool sub = in.op == Op::sub_imm || in.op == Op::subs_imm;
            if (in.op == Op::add_imm || in.op == Op::sub_imm) {
                set(in.rd(), wide, sub ? n - imm : n + imm, true);
            } else if (wide) {
                set(in.rd(), true, sub ? ts_subs64(&fl_, n, imm) : ts_adds64(&fl_, n, imm, 0));
            } else {
                set(in.rd(), false, sub ? ts_subs32(&fl_, uint32_t(n), uint32_t(imm)) : ts_adds32(&fl_, uint32_t(n), uint32_t(imm), 0));
            }
            return true;
        }
        case Op::and_imm:
        case Op::orr_imm:
        case Op::eor_imm:
        case Op::ands_imm: {
            auto masks = a64::bit_masks(in.bit(22), in.bits(15, 10), in.bits(21, 16), true, wide ? 64 : 32);
            if (!masks) {
                trap(in, "bad logical immediate");
            }
            uint64_t imm = masks->wmask & width_mask(wide), n = get(in.rn(), wide);
            uint64_t r = in.op == Op::orr_imm ? n | imm : in.op == Op::eor_imm ? n ^ imm : n & imm;
            if (in.op == Op::ands_imm) {
                wide ? ts_nz64(&fl_, r) : ts_nz32(&fl_, uint32_t(r));
                set(in.rd(), wide, r);
            } else {
                set(in.rd(), wide, r, true);
            }
            return true;
        }
        case Op::movn:
        case Op::movz:
        case Op::movk: {
            unsigned shift = in.bits(22, 21) * 16;
            uint64_t imm = uint64_t(in.bits(20, 5)) << shift;
            if (in.op == Op::movz) {
                set(in.rd(), wide, imm);
            } else if (in.op == Op::movn) {
                set(in.rd(), wide, ~imm);
            } else {
                set(in.rd(), wide, (get(in.rd(), true) & ~(uint64_t(0xffff) << shift)) | imm);
            }
            return true;
        }
        case Op::sbfm:
        case Op::bfm:
        case Op::ubfm: {
            unsigned r = in.bits(21, 16), imms = in.bits(15, 10);
            auto masks = a64::bit_masks(in.bit(22), imms, r, false, wide ? 64 : 32);
            if (!masks) {
                trap(in, "bad bitfield");
            }
            uint64_t all = width_mask(wide), wmask = masks->wmask & all, tmask = masks->tmask & all;
            uint64_t src = get(in.rn(), wide);
            uint64_t rotated = wide ? ts_ror64(src, r) : ts_ror32(uint32_t(src), r);
            if (in.op == Op::ubfm) {
                set(in.rd(), wide, rotated & wmask & tmask);
            } else if (in.op == Op::sbfm) {
                uint64_t bot = rotated & wmask, top = (src >> imms) & 1 ? all : 0;
                set(in.rd(), wide, (top & ~tmask & all) | (bot & tmask));
            } else {
                uint64_t d = get(in.rd(), wide);
                uint64_t bot = (d & ~wmask & all) | (rotated & wmask);
                set(in.rd(), wide, (d & ~tmask & all) | (bot & tmask));
            }
            return true;
        }
        case Op::extr: {
            unsigned lsb = in.bits(15, 10);
            uint64_t m = get(in.rm(), wide), n = get(in.rn(), wide);
            set(in.rd(), wide, lsb == 0 ? m : ((m >> lsb) | (n << ((wide ? 64 : 32) - lsb))) & width_mask(wide));
            return true;
        }
        case Op::b:
            return jump(pc_ + uint64_t(sext(in.bits(25, 0), 26) * 4));
        case Op::bl:
            return call(pc_ + uint64_t(sext(in.bits(25, 0), 26) * 4));
        case Op::b_cond:
        case Op::bc_cond:
            return holds(fl_, in.bits(3, 0)) ? jump(pc_ + uint64_t(sext(in.bits(23, 5), 19) * 4)) : true;
        case Op::cbz:
        case Op::cbnz:
            return (get(in.rt(), wide) == 0) == (in.op == Op::cbz) ? jump(pc_ + uint64_t(sext(in.bits(23, 5), 19) * 4)) : true;
        case Op::tbz:
        case Op::tbnz: {
            unsigned bit = (in.bit(31) << 5) | in.bits(23, 19);
            bool clear = ((get(in.rt(), true) >> bit) & 1) == 0;
            return clear == (in.op == Op::tbz) ? jump(pc_ + uint64_t(sext(in.bits(18, 5), 14) * 4)) : true;
        }
        case Op::br:
        case Op::braa:
        case Op::brab:
        case Op::braaz:
        case Op::brabz: {
            uint64_t target = get(in.rn(), true);
            interp::Place p = interp::place(target);
            // more guest code w/o a translation, same loop, no nesting
            if (p.guest && !p.code) {
                next_ = target;
                return true;
            }
            ts_flags_out(s_, fl_);
            s_->pc = target;
            dispatch();
            return false;
        }
        case Op::ret:
        case Op::retaa:
        case Op::retab:
            ts_flags_out(s_, fl_);
            s_->pc = get(in.op == Op::ret ? in.rn() : 30, true);
            return false;
        case Op::blr:
        case Op::blraa:
        case Op::blrab:
        case Op::blraaz:
        case Op::blrabz:
            return call(get(in.rn(), true));
        case Op::svc:
            tsugiki_svc(s_, in.bits(20, 5));
            return true;
        case Op::brk:
        case Op::hlt:
        case Op::udf:
            trap(in, "breakpoint");
            return false;
        case Op::hint:
        case Op::isb:
        case Op::sb:
        case Op::msr_imm:
            return true;
        case Op::clrex:
            s_->excl = 0;
            return true;
        case Op::dsb:
        case Op::dmb:
            if ((in.bits(11, 8) & 3) == 3) {
                __atomic_thread_fence(__ATOMIC_SEQ_CST);
            } else {
                __atomic_signal_fence(__ATOMIC_SEQ_CST);
            }
            return true;
        case Op::mrs:
        case Op::msr_reg:
            system_register(in);
            return true;
        case Op::sys:
            tsugiki_sys(s_, in.bits(18, 5), get(in.rt(), true));
            return true;
        case Op::stxr:
        case Op::stlxr:
        case Op::ldxr:
        case Op::ldaxr:
        case Op::stxp:
        case Op::stlxp:
        case Op::ldxp:
        case Op::ldaxp:
            exclusive(in);
            return true;
        case Op::stllr:
        case Op::stlr: {
            uint64_t a = get(in.rn(), true, true), v = get(in.rt(), true);
            switch (in.access.bytes) {
            case 1: __atomic_store_n(reinterpret_cast<uint8_t*>(a), uint8_t(v), __ATOMIC_SEQ_CST); break;
            case 2: __atomic_store_n(reinterpret_cast<uint16_t*>(a), uint16_t(v), __ATOMIC_SEQ_CST); break;
            case 4: __atomic_store_n(reinterpret_cast<uint32_t*>(a), uint32_t(v), __ATOMIC_SEQ_CST); break;
            default: __atomic_store_n(reinterpret_cast<uint64_t*>(a), v, __ATOMIC_SEQ_CST); break;
            }
            return true;
        }
        case Op::ldlar:
        case Op::ldar:
        case Op::ldapr: {
            uint64_t a = get(in.rn(), true, true), v;
            switch (in.access.bytes) {
            case 1: v = __atomic_load_n(reinterpret_cast<uint8_t*>(a), __ATOMIC_ACQUIRE); break;
            case 2: v = __atomic_load_n(reinterpret_cast<uint16_t*>(a), __ATOMIC_ACQUIRE); break;
            case 4: v = __atomic_load_n(reinterpret_cast<uint32_t*>(a), __ATOMIC_ACQUIRE); break;
            default: v = __atomic_load_n(reinterpret_cast<uint64_t*>(a), __ATOMIC_ACQUIRE); break;
            }
            set(in.rt(), true, v);
            return true;
        }
        case Op::cas:
        case Op::casp:
        case Op::atomic:
        case Op::swp:
            atomic(in);
            return true;
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
            return true;
        case Op::simd_multiple:
        case Op::simd_multiple_post:
        case Op::simd_single:
        case Op::simd_single_post:
            return lib_op(in);
        case Op::logical_shifted: {
            unsigned opc = in.bits(30, 29);
            uint64_t m = shifted(get(in.rm(), wide), in.bits(23, 22), in.bits(15, 10), wide);
            if (in.bit(21)) {
                m = ~m;
            }
            m &= width_mask(wide);
            uint64_t n = get(in.rn(), wide);
            uint64_t r = opc == 1 ? n | m : opc == 2 ? n ^ m : n & m;
            if (opc == 3) {
                wide ? ts_nz64(&fl_, r) : ts_nz32(&fl_, uint32_t(r));
            }
            set(in.rd(), wide, r);
            return true;
        }
        case Op::addsub_shifted:
        case Op::addsub_extended: {
            bool sub = in.bit(30), flags = in.bit(29), ext = in.op == Op::addsub_extended;
            uint64_t m = ext ? extended(get(in.rm(), true), in.bits(15, 13)) << in.bits(12, 10)
                             : shifted(get(in.rm(), wide), in.bits(23, 22), in.bits(15, 10), wide);
            m &= width_mask(wide);
            uint64_t n = get(in.rn(), wide, ext);
            if (flags) {
                uint64_t r = wide ? (sub ? ts_subs64(&fl_, n, m) : ts_adds64(&fl_, n, m, 0))
                                  : (sub ? ts_subs32(&fl_, uint32_t(n), uint32_t(m)) : ts_adds32(&fl_, uint32_t(n), uint32_t(m), 0));
                set(in.rd(), wide, r);
            } else {
                set(in.rd(), wide, sub ? n - m : n + m, ext);
            }
            return true;
        }
        case Op::adc: {
            bool sub = in.bit(30), flags = in.bit(29);
            uint64_t m = get(in.rm(), wide);
            if (sub) {
                m = ~m & width_mask(wide);
            }
            uint64_t n = get(in.rn(), wide);
            if (flags) {
                set(in.rd(), wide, wide ? ts_adds64(&fl_, n, m, fl_.cf) : ts_adds32(&fl_, uint32_t(n), uint32_t(m), fl_.cf));
            } else {
                set(in.rd(), wide, n + m + fl_.cf);
            }
            return true;
        }
        case Op::ccmp_register:
        case Op::ccmp_immediate: {
            uint64_t m = in.op == Op::ccmp_immediate ? in.bits(20, 16) : get(in.rm(), wide);
            uint64_t n = get(in.rn(), wide);
            if (holds(fl_, in.bits(15, 12))) {
                if (wide) {
                    in.bit(30) ? (void)ts_subs64(&fl_, n, m) : (void)ts_adds64(&fl_, n, m, 0);
                } else {
                    in.bit(30) ? (void)ts_subs32(&fl_, uint32_t(n), uint32_t(m)) : (void)ts_adds32(&fl_, uint32_t(n), uint32_t(m), 0);
                }
            } else {
                ts_nzcv(&fl_, in.bits(3, 0));
            }
            return true;
        }
        case Op::csel: {
            uint64_t m = get(in.rm(), wide);
            switch (in.bit(30) * 2 + in.bit(10)) {
            case 1: m = m + 1; break;
            case 2: m = ~m; break;
            case 3: m = 0 - m; break;
            default: break;
            }
            set(in.rd(), wide, holds(fl_, in.bits(15, 12)) ? get(in.rn(), wide) : m);
            return true;
        }
        case Op::dp2:
            data2(in);
            return true;
        case Op::dp1:
            data1(in);
            return true;
        case Op::dp3:
            data3(in);
            return true;
        case Op::minmax_imm: {
            unsigned opc = in.bits(19, 18), imm = in.bits(17, 10);
            bool is_signed = !(opc & 1), is_max = !(opc & 2);
            uint64_t n = get(in.rn(), wide), r;
            if (is_signed) {
                int64_t a = wide ? int64_t(n) : int64_t(int32_t(n)), b = sext(imm, 8);
                r = uint64_t(is_max ? std::max(a, b) : std::min(a, b));
            } else {
                r = is_max ? std::max<uint64_t>(n, imm) : std::min<uint64_t>(n, imm);
            }
            set(in.rd(), wide, r);
            return true;
        }
        case Op::invalid:
            trap(in, "invalid instruction");
            return false;
        default:
            if (a64::is_simd(in.op)) {
                return lib_op(in);
            }
            trap(in, "the interpreter can't run this yet");
            return false;
        }
    }

    void system_register(const Instruction& in) {
        unsigned reg = in.bits(20, 5);
        constexpr unsigned tpidr = 0xde82, nzcv = 0xda10, fpcr = 0xda20, fpsr = 0xda21;
        if (in.op == Op::mrs) {
            uint64_t v;
            switch (reg) {
            case tpidr: v = s_->tpidr; break;
            case nzcv: v = (uint64_t(fl_.nf) << 31) | (uint64_t(fl_.zf) << 30) | (uint64_t(fl_.cf) << 29) | (uint64_t(fl_.vf) << 28); break;
            case fpcr: v = s_->fpcr; break;
            case fpsr: v = s_->fpsr; break;
            default: v = tsugiki_mrs(s_, reg); break;
            }
            set(in.rt(), true, v);
            return;
        }
        uint64_t v = get(in.rt(), true);
        switch (reg) {
        case tpidr: s_->tpidr = v; break;
        case nzcv: ts_nzcv(&fl_, uint32_t(v >> 28)); break;
        case fpcr: ts_set_fpcr(s_, v); break;
        case fpsr: s_->fpsr = uint32_t(v); break;
        default: tsugiki_msr(s_, reg, v); break;
        }
    }

    void exclusive(const Instruction& in) {
        bool pair = in.access.pair;
        unsigned bytes = in.access.bytes, total = pair ? bytes * 2 : bytes;
        uint64_t a = get(in.rn(), true, true);
        if (in.access.load) {
            u128 v = 0;
            std::memcpy(&v, reinterpret_cast<const void*>(a), total);
            s_->excl = 1;
            s_->excl_addr = a;
            s_->excl_size = total;
            s_->excl_lo = uint64_t(v);
            s_->excl_hi = total == 16 ? uint64_t(v >> 64) : 0;
            if (pair) {
                uint64_t mask = bytes == 8 ? ~uint64_t(0) : 0xffffffffull;
                set(in.rt(), true, uint64_t(v) & mask);
                set(in.rt2(), true, uint64_t(v >> (bytes * 8)) & mask);
            } else {
                set(in.rt(), true, uint64_t(v));
            }
            return;
        }
        u128 value;
        if (pair) {
            uint64_t low = get(in.rt(), true), high = get(in.rt2(), true);
            value = bytes == 8 ? (u128(high) << 64) | low : (uint64_t(uint32_t(high)) << 32) | uint32_t(low);
        } else {
            value = get(in.rt(), true);
        }
        uint32_t fail = 1;
        if (s_->excl && s_->excl_addr == a && s_->excl_size == total) {
            u128 expected = (u128(s_->excl_hi) << 64) | s_->excl_lo;
            switch (total) {
            case 1: fail = !cas<uint8_t>(a, uint8_t(expected), uint8_t(value)); break;
            case 2: fail = !cas<uint16_t>(a, uint16_t(expected), uint16_t(value)); break;
            case 4: fail = !cas<uint32_t>(a, uint32_t(expected), uint32_t(value)); break;
            case 8: fail = !cas<uint64_t>(a, uint64_t(expected), uint64_t(value)); break;
            default: fail = !cas<u128>(a, expected, value); break;
            }
        }
        s_->excl = 0;
        set(in.rs(), false, fail);
    }

    void atomic(const Instruction& in) {
        unsigned bytes = in.access.bytes;
        uint64_t a = get(in.rn(), true, true);
        if (in.op == Op::cas) {
            uint64_t expected = get(in.rs(), true), desired = get(in.rt(), true), old;
            switch (bytes) {
            case 1: old = swap_in<uint8_t>(a, uint8_t(expected), uint8_t(desired)); break;
            case 2: old = swap_in<uint16_t>(a, uint16_t(expected), uint16_t(desired)); break;
            case 4: old = swap_in<uint32_t>(a, uint32_t(expected), uint32_t(desired)); break;
            default: old = swap_in<uint64_t>(a, expected, desired); break;
            }
            set(in.rs(), true, old);
            return;
        }
        if (in.op == Op::casp) {
            bool wide = in.bit(30);
            auto both = [&](int r) -> u128 {
                return wide ? (u128(get(r + 1, true)) << 64) | get(r, true) : (uint64_t(uint32_t(get(r + 1, true))) << 32) | uint32_t(get(r, true));
            };
            u128 old = wide ? swap_in<u128>(a, both(in.rs()), both(in.rt())) : swap_in<uint64_t>(a, uint64_t(both(in.rs())), uint64_t(both(in.rt())));
            uint64_t mask = wide ? ~uint64_t(0) : 0xffffffffull;
            set(in.rs(), true, uint64_t(old) & mask);
            set(in.rs() + 1, true, uint64_t(old >> (wide ? 64 : 32)) & mask);
            return;
        }
        bool swap = in.op == Op::swp;
        unsigned which = in.bits(14, 12);
        uint64_t v = get(in.rs(), true), old;
        switch (bytes) {
        case 1: old = fetch_op<uint8_t>(which, a, v, swap); break;
        case 2: old = fetch_op<uint16_t>(which, a, v, swap); break;
        case 4: old = fetch_op<uint32_t>(which, a, v, swap); break;
        default: old = fetch_op<uint64_t>(which, a, v, swap); break;
        }
        set(in.rt(), true, old);
    }

    void memory(const Instruction& in) {
        const a64::Access& a = in.access;
        if (a.prefetch) {
            return;
        }
        unsigned scale = 0;
        while ((1u << scale) < a.bytes) {
            ++scale;
        }
        uint64_t base = get(in.rn(), true, true), address = 0;
        bool writeback = false;
        uint64_t written = 0;
        switch (in.op) {
        case Op::ldr_literal:
            address = pc_ + uint64_t(sext(in.bits(23, 5), 19) * 4);
            break;
        case Op::single_unsigned:
            address = base + (uint64_t(in.bits(21, 10)) << scale);
            break;
        case Op::single_unscaled:
        case Op::single_unprivileged:
        case Op::rcpc_unscaled:
            address = base + uint64_t(sext(in.bits(20, 12), 9));
            break;
        case Op::single_pre:
            address = written = base + uint64_t(sext(in.bits(20, 12), 9));
            writeback = true;
            break;
        case Op::single_post:
            address = base;
            written = base + uint64_t(sext(in.bits(20, 12), 9));
            writeback = true;
            break;
        case Op::single_register:
            address = base + (extended(get(in.rm(), true), in.bits(15, 13)) << (in.bit(12) ? scale : 0));
            break;
        case Op::pair_noalloc:
        case Op::pair_offset:
            address = base + uint64_t(sext(in.bits(21, 15), 7) * a.bytes);
            break;
        case Op::pair_pre:
            address = written = base + uint64_t(sext(in.bits(21, 15), 7) * a.bytes);
            writeback = true;
            break;
        case Op::pair_post:
            address = base;
            written = base + uint64_t(sext(in.bits(21, 15), 7) * a.bytes);
            writeback = true;
            break;
        case Op::ldraa:
        case Op::ldrab:
            address = written = base + uint64_t(sext((in.bit(22) << 9) | in.bits(20, 12), 10) * 8);
            writeback = in.bit(11);
            break;
        default:
            trap(in, "bad memory op");
            return;
        }
        uint64_t second = address + a.bytes;
        tsugiki_vreg v1{0, 0}, v2{0, 0};
        uint64_t i1 = 0, i2 = 0;
        if (a.load) {
            if (a.simd) {
                v1 = vload(a.bytes, address);
                if (a.pair) {
                    v2 = vload(a.bytes, second);
                }
            } else {
                i1 = loaded(a, load(a.bytes, address));
                if (a.pair) {
                    i2 = loaded(a, load(a.bytes, second));
                }
            }
        } else if (a.simd) {
            vstore(a.bytes, address, s_->v[in.rt()]);
            if (a.pair) {
                vstore(a.bytes, second, s_->v[in.rt2()]);
            }
        } else {
            store(a.bytes, address, get(in.rt(), true));
            if (a.pair) {
                store(a.bytes, second, get(in.rt2(), true));
            }
        }
        if (writeback) {
            s_->x[in.rn()] = written;
        }
        if (a.load) {
            if (a.simd) {
                s_->v[in.rt()] = v1;
                if (a.pair) {
                    s_->v[in.rt2()] = v2;
                }
            } else {
                set(in.rt(), true, i1);
                if (a.pair) {
                    set(in.rt2(), true, i2);
                }
            }
        }
    }

    void data2(const Instruction& in) {
        bool wide = in.sf();
        uint64_t n = get(in.rn(), wide), m = get(in.rm(), wide);
        unsigned bits = wide ? 63 : 31, opcode = in.bits(15, 10);
        switch (opcode) {
        case 2: set(in.rd(), wide, wide ? ts_udiv64(n, m) : ts_udiv32(uint32_t(n), uint32_t(m))); return;
        case 3:
            set(in.rd(), wide, wide ? uint64_t(ts_sdiv64(int64_t(n), int64_t(m))) : uint64_t(uint32_t(ts_sdiv32(int32_t(n), int32_t(m)))));
            return;
        case 8: set(in.rd(), wide, n << (m & bits)); return;
        case 9: set(in.rd(), wide, n >> (m & bits)); return;
        case 10:
            set(in.rd(), wide, wide ? uint64_t(int64_t(n) >> (m & bits)) : uint64_t(uint32_t(int32_t(uint32_t(n)) >> (m & bits))));
            return;
        case 11: set(in.rd(), wide, wide ? ts_ror64(n, unsigned(m)) : ts_ror32(uint32_t(n), unsigned(m))); return;
        case 12: set(in.rd(), true, (get(in.rn(), true) ^ get(in.rm(), true)) & 0xffffffff00000000ull); return;
        case 16: case 17: case 18: case 19: case 20: case 21: case 22: case 23:
            set(in.rd(), false, tsugiki_crc32(uint32_t(get(in.rn(), true)), get(in.rm(), true), 1 << (opcode & 3), (opcode & 4) ? 1 : 0));
            return;
        case 24: case 25: case 26: case 27: {
            bool is_signed = !(opcode & 1), is_max = !(opcode & 2);
            uint64_t r;
            if (is_signed) {
                int64_t a = wide ? int64_t(n) : int64_t(int32_t(n)), b = wide ? int64_t(m) : int64_t(int32_t(m));
                r = uint64_t(is_max ? std::max(a, b) : std::min(a, b));
            } else {
                r = is_max ? std::max(n, m) : std::min(n, m);
            }
            set(in.rd(), wide, r);
            return;
        }
        default:
            trap(in, "bad dp2");
        }
    }

    void data1(const Instruction& in) {
        bool wide = in.sf();
        uint64_t n = get(in.rn(), wide);
        // pac is off for us, signing and checking leave pointers alone
        if (in.bits(20, 16) == 1) {
            return;
        }
        switch (in.bits(15, 10)) {
        case 0: set(in.rd(), wide, wide ? __builtin_bitreverse64(n) : __builtin_bitreverse32(uint32_t(n))); return;
        case 1:
            set(in.rd(), wide, wide ? ((n & 0x00ff00ff00ff00ffull) << 8) | ((n >> 8) & 0x00ff00ff00ff00ffull)
                                    : uint32_t(((n & 0x00ff00ffu) << 8) | ((n >> 8) & 0x00ff00ffu)));
            return;
        case 2: set(in.rd(), wide, wide ? ts_ror64(__builtin_bswap64(n), 32) : __builtin_bswap32(uint32_t(n))); return;
        case 3: set(in.rd(), wide, __builtin_bswap64(n)); return;
        case 4: set(in.rd(), wide, wide ? ts_clz64(n) : ts_clz32(uint32_t(n))); return;
        case 5: set(in.rd(), wide, wide ? ts_cls64(n) : ts_cls32(uint32_t(n))); return;
        case 6: set(in.rd(), wide, wide ? ts_ctz64(n) : ts_ctz32(uint32_t(n))); return;
        case 7: set(in.rd(), wide, wide ? uint64_t(__builtin_popcountll(n)) : uint64_t(__builtin_popcount(uint32_t(n)))); return;
        case 8:
            set(in.rd(), wide, wide ? (int64_t(n) < 0 ? 0 - n : n) : (int32_t(n) < 0 ? uint32_t(0 - uint32_t(n)) : n));
            return;
        default: trap(in, "bad dp1");
        }
    }

    void data3(const Instruction& in) {
        bool wide = in.sf();
        uint64_t n = get(in.rn(), wide), m = get(in.rm(), wide), a = get(in.ra(), wide);
        switch (in.bits(23, 21) * 2 + in.bit(15)) {
        case 0: set(in.rd(), wide, a + n * m); return;
        case 1: set(in.rd(), wide, a - n * m); return;
        case 2: case 3: case 10: case 11: {
            bool is_signed = in.bits(23, 21) == 1, sub = in.bit(15);
            uint64_t x = get(in.rn(), true), y = get(in.rm(), true);
            uint64_t product = is_signed ? uint64_t(int64_t(int32_t(x))) * uint64_t(int64_t(int32_t(y))) : uint64_t(uint32_t(x)) * uint32_t(y);
            uint64_t acc = get(in.ra(), true);
            set(in.rd(), true, sub ? acc - product : acc + product);
            return;
        }
        case 4:
            set(in.rd(), true, uint64_t((__int128(int64_t(get(in.rn(), true))) * int64_t(get(in.rm(), true))) >> 64));
            return;
        case 12:
            set(in.rd(), true, uint64_t((u128(get(in.rn(), true)) * get(in.rm(), true)) >> 64));
            return;
        default:
            trap(in, "bad dp3");
        }
    }

    tsugiki_state* s_;
    tsugiki_flags fl_;
    tsugiki_code op_ = nullptr;
    uint64_t pc_ = 0;
    uint64_t next_ = 0;
};

}  // namespace

extern "C" void tsugiki_interpret(tsugiki_state* s) {
    interp::started(s->pc);
    Interpreter(s).run();
}
