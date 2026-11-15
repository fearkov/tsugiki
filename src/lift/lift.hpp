// arm64 functions to c++
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "a64/decoder.hpp"
#include "elf.hpp"

namespace tsugiki::lift {

struct Function {
    uint64_t start = 0;
    uint64_t end = 0;
    // code[i] sits at start + 4 * i
    std::vector<a64::Instruction> code;
    // jumped to from inside
    std::set<uint64_t> labels;
    // where the dispatcher can come in: the start and every return point
    std::set<uint64_t> entries;
};

struct Program {
    std::vector<Function> functions;
    // start -> index into functions
    std::map<uint64_t, size_t> starts;
};

Program discover(const elf::Library& library);

// c++ for the whole library, id goes into the exported names
std::string emit(const Program& program, const std::string& id);

}  // namespace tsugiki::lift
