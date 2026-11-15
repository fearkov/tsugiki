// arm64 .so files, read like android's linker does
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace tsugiki::elf {

namespace relocation {
constexpr uint32_t none = 0;
constexpr uint32_t abs64 = 257;
constexpr uint32_t copy = 1024;
constexpr uint32_t glob_dat = 1025;
constexpr uint32_t jump_slot = 1026;
constexpr uint32_t relative = 1027;
constexpr uint32_t tls_dtpmod64 = 1028;
constexpr uint32_t tls_dtprel64 = 1029;
constexpr uint32_t tls_tprel64 = 1030;
constexpr uint32_t tlsdesc = 1031;
constexpr uint32_t irelative = 1032;

// llvm's name, without R_AARCH64_
std::string name(uint32_t type);
}  // namespace relocation

namespace segment {
constexpr uint32_t load = 1;
constexpr uint32_t dynamic = 2;
constexpr uint32_t interp = 3;
constexpr uint32_t note = 4;
constexpr uint32_t phdr = 6;
constexpr uint32_t tls = 7;
constexpr uint32_t gnu_eh_frame = 0x6474e550;
constexpr uint32_t gnu_stack = 0x6474e551;
constexpr uint32_t gnu_relro = 0x6474e552;
constexpr uint32_t gnu_property = 0x6474e553;

constexpr uint32_t executable = 1;
constexpr uint32_t writable = 2;
constexpr uint32_t readable = 4;
}  // namespace segment

namespace symbol {
constexpr uint8_t no_type = 0;
constexpr uint8_t object = 1;
constexpr uint8_t function = 2;
constexpr uint8_t section = 3;
constexpr uint8_t file = 4;
constexpr uint8_t tls = 6;
constexpr uint8_t ifunc = 10;

constexpr uint8_t local = 0;
constexpr uint8_t global = 1;
constexpr uint8_t weak = 2;
constexpr uint8_t unique = 10;

constexpr uint16_t undefined = 0;
constexpr uint16_t absolute = 0xfff1;
}  // namespace symbol

struct Segment {
    uint32_t type = 0;
    uint32_t flags = 0;
    uint64_t offset = 0;
    uint64_t vaddr = 0;
    uint64_t filesz = 0;
    uint64_t memsz = 0;
    uint64_t align = 0;

    bool executable() const { return flags & segment::executable; }
    bool writable() const { return flags & segment::writable; }
    bool holds(uint64_t address) const { return address >= vaddr && address - vaddr < memsz; }
};

struct Symbol {
    std::string name;
    uint64_t value = 0;
    uint64_t size = 0;
    uint8_t type = 0;
    uint8_t bind = 0;
    uint8_t visibility = 0;
    uint16_t section = 0;

    bool defined() const { return section != symbol::undefined; }
    bool code() const { return type == symbol::function || type == symbol::ifunc; }
};

// which table it came from
enum class Table : uint8_t { rela, jmprel, relr, android };

struct Relocation {
    uint64_t offset = 0;
    uint32_t type = 0;
    // index into dynamic_symbols, 0 = none
    uint32_t symbol = 0;
    // relr: the word in place
    int64_t addend = 0;
    Table table = Table::rela;

    bool operator==(const Relocation&) const = default;
};

class Library {
public:
    // name is just for errors
    explicit Library(std::vector<uint8_t> bytes, std::string name = {});

    const std::string& name() const { return name_; }
    std::span<const uint8_t> bytes() const { return bytes_; }
    uint16_t type() const { return type_; }
    uint64_t entry() const { return entry_; }

    const std::vector<Segment>& segments() const { return segments_; }
    // first one of that type
    std::optional<Segment> segment(uint32_t type) const;
    // the PT_LOAD holding it
    std::optional<Segment> loaded(uint64_t address) const;

    // file bytes at an address, none if not all in the file (bss etc)
    std::optional<std::span<const uint8_t>> at(uint64_t address, uint64_t size) const;
    // from an address to the end of its segment's file part
    std::optional<std::span<const uint8_t>> from(uint64_t address) const;
    bool executable(uint64_t address) const;
    // in .text and such, not just the r-x segment, old linkers put .rodata in there too
    bool code(uint64_t address) const;

    // a pointer slot with relocations applied at base 0
    // none if it points at an import
    std::optional<uint64_t> pointer(uint64_t address) const;
    const Relocation* relocation_at(uint64_t address) const;

    std::string soname;
    std::vector<std::string> needed;
    std::vector<Symbol> dynamic_symbols;
    // .symtab if not stripped
    std::vector<Symbol> static_symbols;
    std::vector<Relocation> relocations;
    uint64_t init = 0;
    uint64_t fini = 0;
    // targets as pointer() reads them, 0 = unknown
    std::vector<uint64_t> preinit_array;
    std::vector<uint64_t> init_array;
    std::vector<uint64_t> fini_array;
    bool bind_now = false;

    // where the dynsym count came from, tests check all of them
    enum class Count : uint8_t { sections, hash, gnu_hash };
    Count symbol_count_from() const { return counted_; }
    // 0 if the library doesn't have that one
    uint64_t count_symbols(Count way) const;

private:
    void read_segments();
    void read_sections();
    void read_dynamic();
    void read_rela(uint64_t address, uint64_t size, Table table);
    void read_relr(uint64_t address, uint64_t size);
    void read_android(uint64_t address, uint64_t size);
    std::vector<uint64_t> read_array(uint64_t address, uint64_t size) const;
    std::string string_at(uint64_t offset) const;
    std::vector<Symbol> read_symbols(std::span<const uint8_t> table, std::span<const uint8_t> strings) const;

    std::vector<uint8_t> bytes_;
    std::string name_;
    uint16_t type_ = 0;
    uint64_t entry_ = 0;
    uint64_t phoff_ = 0;
    uint16_t phnum_ = 0;
    uint64_t shoff_ = 0;
    uint16_t shnum_ = 0;
    uint16_t shstrndx_ = 0;
    std::vector<Segment> segments_;
    // executable sections, start and end, empty if there are no section headers
    std::vector<std::pair<uint64_t, uint64_t>> code_sections_;

    uint64_t strtab_ = 0;
    uint64_t strsz_ = 0;
    uint64_t symtab_ = 0;
    uint64_t hash_ = 0;
    uint64_t gnu_hash_ = 0;
    uint64_t dynsym_size_ = 0;
    Count counted_ = Count::sections;
    // indices into relocations, sorted by offset
    std::vector<uint32_t> by_offset_;
    // preinit, init, fini: address and size
    std::pair<uint64_t, uint64_t> arrays_[3] = {};
};

}  // namespace tsugiki::elf
