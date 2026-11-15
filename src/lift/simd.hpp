// fp and neon to c++
#pragma once

#include <cstdint>
#include <string>

#include "a64/decoder.hpp"

namespace tsugiki::lift {

// statements for one fp/simd instruction, empty if we can't yet
std::string simd(const a64::Instruction& in, uint64_t pc);

}  // namespace tsugiki::lift
