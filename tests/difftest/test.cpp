// runs every word both ways, the lifter's c++ and the interpreter, from the same random states,
// and prints the ones that end up different. usage: test [rounds per word]
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <sys/mman.h>

#include "a64/decoder.hpp"
#include "host.hpp"

using namespace tsugiki;
using a64::Op;

extern "C" const tsugiki_op tsugiki_ops_dt[];
extern "C" const uint64_t tsugiki_op_count_dt;
extern "C" uint64_t tsugiki_base_dt;

namespace {

constexpr size_t mem_size = 1 << 21;
uint8_t* mem;
uint8_t* snapshot;
uint32_t* page;

uint64_t special(std::mt19937_64& r) {
    static const uint64_t values[] = {0, 1, 2, ~uint64_t(0), 0x8000000000000000ull, 0x7fffffffffffffffull, 0xffffffff, 0x80000000,
                                      0x7fffffff, 0x100000000ull, 0x3ff0000000000000ull, 0x7ff8000000000000ull, 0x7ff0000000000000ull,
                                      0xfff0000000000000ull, 0x8000000000000000ull, 0x000fffffffffffffull, 0x3f800000, 0x7fc00000, 0xff800000};
    switch (r() % 4) {
    case 0: return values[r() % (sizeof values / sizeof values[0])];
    case 1: return r() & 0xff;
    case 2: return uint64_t(int64_t(int32_t(r())));
    default: return r();
    }
}

bool memory_op(const a64::Instruction& in) { return in.access.bytes != 0; }

void random_state(tsugiki_state& s, std::mt19937_64& r, const a64::Instruction& in) {
    std::memset(&s, 0, sizeof s);
    for (int i = 0; i < 31; ++i) s.x[i] = special(r);
    s.x[31] = reinterpret_cast<uint64_t>(mem) + mem_size / 2;
    for (int i = 0; i < 32; ++i) {
        s.v[i].lo = special(r);
        s.v[i].hi = special(r);
    }
    uint32_t nzcv = uint32_t(r());
    s.nf = nzcv & 1;
    s.zf = (nzcv >> 1) & 1;
    s.cf = (nzcv >> 2) & 1;
    s.vf = (nzcv >> 3) & 1;
    if (memory_op(in)) {
        // every register points into the sandbox, the ones used as offsets stay small
        for (int i = 0; i < 32; ++i) {
            s.x[i] = reinterpret_cast<uint64_t>(mem) + mem_size / 2 + ((r() % 4096) * 16) - 32768;
        }
        if (in.op == Op::single_register) {
            s.x[in.rm()] = r() % 512;
        }
        // exclusives: half the time a monitor that matches
        s.excl = 0;
        if (r() & 1) {
            s.excl = 1;
            s.excl_addr = s.x[in.rn()];
            s.excl_size = in.access.pair ? in.access.bytes * 2 : in.access.bytes;
            std::memcpy(&s.excl_lo, reinterpret_cast<void*>(s.excl_addr), 8);
        }
    }
}

std::string diff(const tsugiki_state& a, const tsugiki_state& b) {
    std::string out;
    char text[160];
    for (int i = 0; i < 32; ++i) {
        if (a.x[i] != b.x[i]) {
            std::snprintf(text, sizeof text, " x%d %llx vs %llx;", i, (unsigned long long)a.x[i], (unsigned long long)b.x[i]);
            out += text;
        }
    }
    for (int i = 0; i < 32; ++i) {
        if (a.v[i].lo != b.v[i].lo || a.v[i].hi != b.v[i].hi) {
            std::snprintf(text, sizeof text, " v%d %llx:%llx vs %llx:%llx;", i, (unsigned long long)a.v[i].hi, (unsigned long long)a.v[i].lo,
                          (unsigned long long)b.v[i].hi, (unsigned long long)b.v[i].lo);
            out += text;
        }
    }
    if (a.nf != b.nf || a.zf != b.zf || a.cf != b.cf || a.vf != b.vf) {
        std::snprintf(text, sizeof text, " nzcv %u%u%u%u vs %u%u%u%u;", a.nf, a.zf, a.cf, a.vf, b.nf, b.zf, b.cf, b.vf);
        out += text;
    }
    if (a.excl != b.excl) out += " excl;";
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    int rounds = argc > 1 ? std::atoi(argv[1]) : 64;
    mem = static_cast<uint8_t*>(mmap(nullptr, mem_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    snapshot = static_cast<uint8_t*>(std::malloc(mem_size));
    page = static_cast<uint32_t*>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    std::mt19937_64 fill(1);
    for (size_t i = 0; i < mem_size; i += 8) {
        uint64_t v = fill();
        std::memcpy(mem + i, &v, 8);
    }
    // no fp/simd words for the interpreter, so scalar fp goes thru its own code
    tsugiki_add_module(reinterpret_cast<uint64_t>(page), 4096, nullptr, 0, nullptr, 0);
    size_t bad = 0;
    for (uint64_t k = 0; k < tsugiki_op_count_dt; ++k) {
        uint32_t word = tsugiki_ops_dt[k].word;
        a64::Instruction in = a64::decode(word);
        page[0] = word;
        page[1] = 0xd65f03c0;
        std::mt19937_64 r(word);
        for (int round = 0; round < rounds; ++round) {
            tsugiki_state base;
            random_state(base, r, in);
            std::memcpy(snapshot, mem, mem_size);
            tsugiki_state lifted = base;
            lifted.pc = reinterpret_cast<uint64_t>(page);
            tsugiki_ops_dt[k].code(&lifted);
            std::string lifted_mem(reinterpret_cast<char*>(mem), mem_size);
            std::memcpy(mem, snapshot, mem_size);
            tsugiki_state interpreted = base;
            interpreted.pc = reinterpret_cast<uint64_t>(page);
            interpreted.x[30] = base.x[30];
            interpreted.cache = nullptr;
            // the interpreter runs the word then the ret, which puts x30 in pc and comes back
            tsugiki_interpret(&interpreted);
            std::string d = diff(lifted, interpreted);
            if (std::memcmp(lifted_mem.data(), mem, mem_size) != 0) d += " memory;";
            std::memcpy(mem, snapshot, mem_size);
            if (!d.empty()) {
                std::printf("%08x %-40s%s\n", word, a64::mnemonic(in).c_str(), d.c_str());
                ++bad;
                break;
            }
        }
    }
    std::printf("%llu words, %zu differ\n", (unsigned long long)tsugiki_op_count_dt, bad);
}
