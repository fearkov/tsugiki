#include "loader.hpp"

#include <sys/mman.h>

#include <cstring>
#include <stdexcept>

using namespace tsugiki;

Image load_image(const elf::Library& library, const std::function<uint64_t(const std::string&)>& resolve) {
    uint64_t end = 0;
    for (const elf::Segment& s : library.segments()) {
        if (s.type == elf::segment::load) {
            end = std::max(end, s.vaddr + s.memsz);
        }
    }
    // 64k aligned, like android's 16k pages and then some
    constexpr uint64_t align = 0x10000;
    uint64_t size = (end + align - 1) & ~(align - 1);
    void* raw = mmap(nullptr, size + align, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) {
        throw std::runtime_error("mmap failed");
    }
    uint64_t base = (reinterpret_cast<uint64_t>(raw) + align - 1) & ~(align - 1);
    for (const elf::Segment& s : library.segments()) {
        if (s.type == elf::segment::load && s.filesz) {
            std::memcpy(reinterpret_cast<void*>(base + s.vaddr), library.bytes().data() + s.offset, s.filesz);
        }
    }
    for (const elf::Relocation& x : library.relocations) {
        uint64_t* slot = reinterpret_cast<uint64_t*>(base + x.offset);
        uint64_t target = 0;
        if (x.symbol) {
            const elf::Symbol& symbol = library.dynamic_symbols[x.symbol];
            target = symbol.defined() ? base + symbol.value : resolve(symbol.name);
        }
        switch (x.type) {
        case elf::relocation::relative:
            *slot = base + uint64_t(x.addend);
            break;
        case elf::relocation::abs64:
        case elf::relocation::glob_dat:
        case elf::relocation::jump_slot:
            *slot = target + uint64_t(x.addend);
            break;
        default:
            // tls and friends, not needed by the tests yet
            break;
        }
    }
    return {base, size};
}
