// every unique word in some libs' code, as the lifter's c++ for each, for test.cpp to run
// run as gen <out dir> <lib.so>...
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include "a64/decoder.hpp"
#include "a64/simd.hpp"
#include "elf.hpp"
#include "lift/lift.hpp"

using namespace tsugiki;
using a64::Op;

// what the harness can run in a sandbox, no control flow or system stuff
static bool testable(const a64::Instruction& in) {
    switch (in.op) {
    case Op::invalid:
    case Op::simd:
    case Op::b:
    case Op::bl:
    case Op::b_cond:
    case Op::bc_cond:
    case Op::cbz:
    case Op::cbnz:
    case Op::tbz:
    case Op::tbnz:
    case Op::br:
    case Op::blr:
    case Op::ret:
    case Op::braaz:
    case Op::brabz:
    case Op::blraaz:
    case Op::blrabz:
    case Op::retaa:
    case Op::retab:
    case Op::braa:
    case Op::brab:
    case Op::blraa:
    case Op::blrab:
    case Op::svc:
    case Op::brk:
    case Op::hlt:
    case Op::udf:
    case Op::sys:
    case Op::sysl:
    case Op::mrs:
    case Op::msr_reg:
    case Op::msr_imm:
    case Op::adr:
    case Op::adrp:
    case Op::ldr_literal:
        return false;
    default:
        break;
    }
    // vector ops run the lifter's own code in the interpreter too, nothing to compare
    if (a64::is_simd(in.op)) {
        return in.op == Op::fp_1src || in.op == Op::fp_2src || in.op == Op::fp_3src || in.op == Op::fp_compare || in.op == Op::fp_ccmp ||
               in.op == Op::fp_csel || in.op == Op::fp_imm || in.op == Op::fp_int || in.op == Op::fp_fixed;
    }
    return in.op != Op::simd_multiple && in.op != Op::simd_multiple_post && in.op != Op::simd_single && in.op != Op::simd_single_post;
}

int main(int argc, char** argv) {
    std::set<uint32_t> words;
    for (int i = 2; i < argc; ++i) {
        std::ifstream f(argv[i], std::ios::binary);
        std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(f), {}};
        elf::Library lib(std::move(bytes), argv[i]);
        for (auto [start, end] : lib.code_ranges()) {
            start = (start + 3) & ~uint64_t(3);
            auto b = lib.at(start, (end - start) & ~uint64_t(3));
            if (!b) continue;
            for (size_t k = 0; k + 4 <= b->size(); k += 4) {
                uint32_t w;
                std::memcpy(&w, b->data() + k, 4);
                if (testable(a64::decode(w))) words.insert(w);
            }
        }
    }
    lift::Program p;
    // one lil function so the entries table isn't empty
    lift::Function f;
    f.start = 0;
    f.end = 4;
    f.code.push_back(a64::decode(0xd65f03c0));
    f.entries.insert(0);
    p.starts[0] = 0;
    p.functions.push_back(f);
    p.ops.assign(words.begin(), words.end());
    auto parts = lift::emit_parts(p, "dt", SIZE_MAX);
    std::string dir = argv[1];
    for (size_t i = 0; i < parts.size(); ++i) {
        std::ofstream(dir + "/part" + std::to_string(i) + ".cpp") << parts[i];
    }
    std::printf("%zu words, %zu parts\n", words.size(), parts.size());
}
