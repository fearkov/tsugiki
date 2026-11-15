// jump tables behind a br: where in the function it can land
#pragma once

#include <cstdint>
#include <vector>

#include "elf.hpp"
#include "lift/lift.hpp"

namespace tsugiki::lift {

// targets of the br at f.code[index], empty if it doesn't look like a table
// a guess is fine: they only become cases, anything else still goes to the dispatcher
std::vector<uint64_t> jump_table(const elf::Library& library, const Function& f, size_t index);

}  // namespace tsugiki::lift
