// function ranges from .eh_frame, basically every function the ndk builds
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "elf.hpp"

namespace tsugiki::eh_frame {

struct Fde {
    uint64_t start = 0;
    uint64_t size = 0;
    // c++ landing pads live in here
    std::optional<uint64_t> lsda;
    // personality routine, or the GOT slot holding it if indirect
    std::optional<uint64_t> personality;
    bool personality_indirect = false;
    // has 'S', signal trampolines
    bool signal_frame = false;
};

// all FDEs, sorted by start, empty if there's no PT_GNU_EH_FRAME
std::vector<Fde> read(const elf::Library& library);

// where an exception can land in that function, from its lsda's call sites
std::vector<uint64_t> landing_pads(const elf::Library& library, const Fde& fde);

}  // namespace tsugiki::eh_frame
