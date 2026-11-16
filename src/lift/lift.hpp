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
    // br address -> where its jump table goes, all inside
    std::map<uint64_t, std::vector<uint64_t>> tables;
    // nzcv across calls, which aapcs64 says never happens but llvm's outliner does:
    // reads its caller's flags, hands flags back on ret, and bl sites reading flags after
    bool flags_in = false;
    bool flags_out = false;
    std::set<uint64_t> flags_after;
};

struct Program {
    std::vector<Function> functions;
    // start -> index into functions
    std::map<uint64_t, size_t> starts;
};

Program discover(const elf::Library& library);

// c++ for the whole library, id goes into the exported names
std::string emit(const Program& program, const std::string& id);

// the same as several files of about per_part instructions, for parallel builds;
// the first one has the dispatcher's table
std::vector<std::string> emit_parts(const Program& program, const std::string& id, size_t per_part);

}  // namespace tsugiki::lift
