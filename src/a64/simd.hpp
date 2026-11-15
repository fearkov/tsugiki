// fp/simd names, empty if unallocated
#pragma once

#include <string>

#include "a64/decoder.hpp"

namespace tsugiki::a64 {

std::string simd_name(const Instruction& in);
bool is_simd(Op op);
// long or narrow ops with a q=1 form named with a 2
bool has_second_half(Op op, const std::string& base);

}  // namespace tsugiki::a64
