// a64 decoding, one 32-bit word at a time
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "a64/ops.hpp"

namespace tsugiki::a64 {

enum class Op : uint16_t {
    invalid,
#define TSUGIKI_OP(name, mnemonic, pattern) name,
    TSUGIKI_A64_OPS(TSUGIKI_OP)
#undef TSUGIKI_OP
    // fp/simd data processing, not decoded yet
    simd,
};

// what a load/store moves
struct Access {
    bool load = false;
    bool simd = false;
    // 1, 2, 4, 8, 16
    uint8_t bytes = 0;
    bool sign = false;
    // integer loads: x or w register
    bool wide = false;
    bool prefetch = false;
    bool pair = false;
};

struct Instruction {
    uint32_t raw = 0;
    Op op = Op::invalid;
    Access access;

    uint32_t bits(int high, int low) const { return (raw >> low) & ((uint32_t(2) << (high - low)) - 1); }
    bool bit(int n) const { return (raw >> n) & 1; }

    // the usual field spots
    int rd() const { return raw & 31; }
    int rt() const { return raw & 31; }
    int rn() const { return (raw >> 5) & 31; }
    int rm() const { return (raw >> 16) & 31; }
    int rs() const { return (raw >> 16) & 31; }
    int ra() const { return (raw >> 10) & 31; }
    int rt2() const { return (raw >> 10) & 31; }
    bool sf() const { return bit(31); }

    bool valid() const { return op != Op::invalid; }
};

Instruction decode(uint32_t raw);

// every pattern, for tests
struct Encoding {
    Op op;
    uint32_t mask;
    uint32_t value;
};
std::vector<Encoding> encodings();

// llvm's name for it, no aliases
std::string mnemonic(const Instruction& instruction);

// eq, ne, ..., al, nv
const char* condition(unsigned code);

// DecodeBitMasks from the arm arm, none if reserved
struct Masks {
    uint64_t wmask;
    uint64_t tmask;
};
std::optional<Masks> bit_masks(bool n, unsigned imms, unsigned immr, bool immediate, unsigned datasize);

}  // namespace tsugiki::a64
