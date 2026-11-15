// fp/simd names, empty if unallocated
#pragma once

#include <string>

#include "a64/decoder.hpp"

namespace tsugiki::a64 {

std::string simd_name(const Instruction& in);
bool is_simd(Op op);

}  // namespace tsugiki::a64
