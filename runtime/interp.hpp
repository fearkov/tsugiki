// what the interpreter needs from whichever runtime it's linked into
#pragma once

#include "tsugiki.h"

namespace tsugiki::interp {

// a guest pc as the runtime sees it
struct Place {
    // translated code starting right there, null if none
    tsugiki_code code = nullptr;
    // its lib's fp/simd words
    const tsugiki_op* ops = nullptr;
    uint64_t op_count = 0;
    // inside some guest lib's image, so we can run it ourselves
    bool guest = false;
};

// each runtime has its own of these two
Place place(uint64_t pc);
// we're about to interpret from pc, for logs
void started(uint64_t pc);

}  // namespace tsugiki::interp
