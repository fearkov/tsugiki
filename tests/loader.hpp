// maps an arm64 .so into our memory and relocates it, tests only
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "elf.hpp"

struct Image {
    uint64_t base = 0;
    uint64_t size = 0;
};

// imports go through resolve, 0 = unresolved
Image load_image(const tsugiki::elf::Library& library, const std::function<uint64_t(const std::string&)>& resolve);
