#include "elf.hpp"

#include <algorithm>
#include <cstring>

#include "bytes.hpp"

namespace tsugiki::elf {

namespace {

constexpr uint16_t et_exec = 2;
constexpr uint16_t et_dyn = 3;
constexpr uint16_t em_aarch64 = 183;

constexpr uint32_t sht_symtab = 2;
constexpr uint32_t sht_dynsym = 11;

enum : int64_t {
    dt_null = 0,
    dt_needed = 1,
    dt_pltrelsz = 2,
    dt_hash = 4,
    dt_strtab = 5,
    dt_symtab = 6,
    dt_rela = 7,
    dt_relasz = 8,
    dt_relaent = 9,
    dt_strsz = 10,
    dt_syment = 11,
    dt_init = 12,
    dt_fini = 13,
    dt_soname = 14,
    dt_rel = 17,
    dt_pltrel = 20,
    dt_jmprel = 23,
    dt_bind_now = 24,
    dt_init_array = 25,
    dt_fini_array = 26,
    dt_init_arraysz = 27,
    dt_fini_arraysz = 28,
    dt_flags = 30,
    dt_preinit_array = 32,
    dt_preinit_arraysz = 33,
    dt_relrsz = 35,
    dt_relr = 36,
    dt_relrent = 37,
    dt_android_rel = 0x6000000f,
    dt_android_relsz = 0x60000010,
    dt_android_rela = 0x60000011,
    dt_android_relasz = 0x60000012,
    dt_android_relr = 0x6fffe000,
    dt_android_relrsz = 0x6fffe001,
    dt_android_relrent = 0x6fffe003,
    dt_gnu_hash = 0x6ffffef5,
    dt_flags_1 = 0x6ffffffb,
};

constexpr uint64_t df_bind_now = 0x8;
constexpr uint64_t df_1_now = 0x1;

constexpr size_t symbol_size = 24;
constexpr size_t rela_size = 24;

}  // namespace

std::string relocation::name(uint32_t type) {
    switch (type) {
    case none: return "NONE";
    case abs64: return "ABS64";
    case copy: return "COPY";
    case glob_dat: return "GLOB_DAT";
    case jump_slot: return "JUMP_SLOT";
    case relative: return "RELATIVE";
    case tls_dtpmod64: return "TLS_DTPMOD64";
    case tls_dtprel64: return "TLS_DTPREL64";
    case tls_tprel64: return "TLS_TPREL64";
    case tlsdesc: return "TLSDESC";
    case irelative: return "IRELATIVE";
    default: return "unknown " + std::to_string(type);
    }
}

Library::Library(std::vector<uint8_t> bytes, std::string name) : bytes_(std::move(bytes)), name_(std::move(name)) {
    if (bytes_.size() < 64 || std::memcmp(bytes_.data(), "\x7f" "ELF", 4) != 0) {
        throw Error(name_ + ": not an ELF file");
    }
    if (bytes_[4] != 2 || bytes_[5] != 1) {
        throw Error(name_ + ": not a 64-bit little-endian ELF");
    }
    Reader header(bytes_);
    header.seek(16);
    type_ = header.u16();
    uint16_t machine = header.u16();
    header.skip(4);
    entry_ = header.u64();
    phoff_ = header.u64();
    shoff_ = header.u64();
    header.skip(4 + 2);
    uint16_t phentsize = header.u16();
    phnum_ = header.u16();
    uint16_t shentsize = header.u16();
    shnum_ = header.u16();
    shstrndx_ = header.u16();
    if (machine != em_aarch64) {
        throw Error(name_ + ": not arm64 (machine " + std::to_string(machine) + ")");
    }
    if (type_ != et_dyn && type_ != et_exec) {
        throw Error(name_ + ": not a shared object or an executable");
    }
    if (phentsize != 56 || (shnum_ != 0 && shentsize != 64)) {
        throw Error(name_ + ": weird header sizes");
    }

    read_segments();
    read_sections();
    read_dynamic();

    by_offset_.resize(relocations.size());
    for (uint32_t i = 0; i < by_offset_.size(); ++i) {
        by_offset_[i] = i;
    }
    std::stable_sort(by_offset_.begin(), by_offset_.end(), [&](uint32_t a, uint32_t b) { return relocations[a].offset < relocations[b].offset; });

    // the arrays are relocated slots, so after the index
    preinit_array = read_array(arrays_[0].first, arrays_[0].second);
    init_array = read_array(arrays_[1].first, arrays_[1].second);
    fini_array = read_array(arrays_[2].first, arrays_[2].second);
}

void Library::read_segments() {
    if (phoff_ > bytes_.size() || (bytes_.size() - phoff_) / 56 < phnum_) {
        throw Error(name_ + ": program headers past the end");
    }
    Reader table(std::span(bytes_).subspan(phoff_, size_t(phnum_) * 56));
    for (uint16_t i = 0; i < phnum_; ++i) {
        Segment s;
        s.type = table.u32();
        s.flags = table.u32();
        s.offset = table.u64();
        s.vaddr = table.u64();
        table.skip(8);
        s.filesz = table.u64();
        s.memsz = table.u64();
        s.align = table.u64();
        if (s.type == segment::load && (s.offset > bytes_.size() || bytes_.size() - s.offset < s.filesz)) {
            throw Error(name_ + ": a segment runs past the end of the file");
        }
        segments_.push_back(s);
    }
}

std::optional<Segment> Library::segment(uint32_t type) const {
    for (const Segment& s : segments_) {
        if (s.type == type) {
            return s;
        }
    }
    return std::nullopt;
}

std::optional<Segment> Library::loaded(uint64_t address) const {
    for (const Segment& s : segments_) {
        if (s.type == segment::load && s.holds(address)) {
            return s;
        }
    }
    return std::nullopt;
}

std::optional<std::span<const uint8_t>> Library::at(uint64_t address, uint64_t size) const {
    for (const Segment& s : segments_) {
        if (s.type != segment::load || address < s.vaddr) {
            continue;
        }
        uint64_t into = address - s.vaddr;
        if (into <= s.filesz && s.filesz - into >= size) {
            return std::span(bytes_).subspan(s.offset + into, size);
        }
    }
    return std::nullopt;
}

std::optional<std::span<const uint8_t>> Library::from(uint64_t address) const {
    for (const Segment& s : segments_) {
        if (s.type == segment::load && address >= s.vaddr && address - s.vaddr < s.filesz) {
            uint64_t into = address - s.vaddr;
            return std::span(bytes_).subspan(s.offset + into, s.filesz - into);
        }
    }
    return std::nullopt;
}

bool Library::executable(uint64_t address) const {
    auto s = loaded(address);
    return s && s->executable();
}

bool Library::code(uint64_t address) const {
    if (code_sections_.empty()) {
        return executable(address);
    }
    for (auto [start, end] : code_sections_) {
        if (address >= start && address < end) {
            return true;
        }
    }
    return false;
}

void Library::read_sections() {
    if (shoff_ == 0 || shnum_ == 0) {
        return;
    }
    // sections are optional, a broken table just gets ignored
    if (shoff_ > bytes_.size() || (bytes_.size() - shoff_) / 64 < shnum_) {
        return;
    }
    struct Section {
        uint32_t type;
        uint64_t addr, offset, size;
        uint32_t link;
    };
    std::vector<Section> sections;
    Reader table(std::span(bytes_).subspan(shoff_, size_t(shnum_) * 64));
    for (uint16_t i = 0; i < shnum_; ++i) {
        Section s;
        table.skip(4);
        s.type = table.u32();
        uint64_t flags = table.u64();
        s.addr = table.u64();
        s.offset = table.u64();
        s.size = table.u64();
        s.link = table.u32();
        table.skip(4 + 8 + 8);
        sections.push_back(s);
        // alloc + execinstr
        if ((flags & 6) == 6 && s.size) {
            code_sections_.emplace_back(s.addr, s.addr + s.size);
        }
    }
    auto contents = [&](const Section& s) -> std::optional<std::span<const uint8_t>> {
        if (s.offset > bytes_.size() || bytes_.size() - s.offset < s.size) {
            return std::nullopt;
        }
        return std::span(bytes_).subspan(s.offset, s.size);
    };
    for (const Section& s : sections) {
        if (s.type == sht_dynsym) {
            dynsym_size_ = s.size;
        }
        if (s.type == sht_symtab && s.link < sections.size()) {
            auto symbols = contents(s);
            auto strings = contents(sections[s.link]);
            if (symbols && strings) {
                static_symbols = read_symbols(*symbols, *strings);
            }
        }
    }
}

std::vector<Symbol> Library::read_symbols(std::span<const uint8_t> table, std::span<const uint8_t> strings) const {
    std::vector<Symbol> symbols;
    Reader r(table);
    while (r.left() >= symbol_size) {
        Symbol s;
        uint32_t name = r.u32();
        uint8_t info = r.u8();
        s.visibility = r.u8() & 3;
        s.section = r.u16();
        s.value = r.u64();
        s.size = r.u64();
        s.bind = info >> 4;
        s.type = info & 0xf;
        if (name < strings.size()) {
            Reader text(strings.subspan(name));
            s.name = text.cstring();
        }
        symbols.push_back(std::move(s));
    }
    return symbols;
}

std::string Library::string_at(uint64_t offset) const {
    if (offset >= strsz_) {
        throw Error(name_ + ": string past the end of the string table");
    }
    auto strings = at(strtab_, strsz_);
    if (!strings) {
        throw Error(name_ + ": string table not in the file");
    }
    Reader r(strings->subspan(offset));
    return r.cstring();
}

uint64_t Library::count_symbols(Count way) const {
    switch (way) {
    case Count::sections:
        return dynsym_size_ / symbol_size;
    case Count::hash: {
        // nchain is the symbol count
        auto header = hash_ ? at(hash_, 8) : std::nullopt;
        if (!header) {
            return 0;
        }
        Reader r(*header);
        r.skip(4);
        return r.u32();
    }
    case Count::gnu_hash: {
        auto header = gnu_hash_ ? at(gnu_hash_, 16) : std::nullopt;
        if (!header) {
            return 0;
        }
        Reader r(*header);
        uint32_t buckets = r.u32();
        uint32_t first = r.u32();
        uint32_t bloom = r.u32();
        uint64_t at_buckets = gnu_hash_ + 16 + uint64_t(bloom) * 8;
        auto bucket_bytes = at(at_buckets, uint64_t(buckets) * 4);
        if (!bucket_bytes) {
            return 0;
        }
        Reader b(*bucket_bytes);
        uint32_t last = 0;
        for (uint32_t i = 0; i < buckets; ++i) {
            last = std::max(last, b.u32());
        }
        if (last < first) {
            return first;
        }
        // walk the last chain to its end bit
        uint64_t at_chains = at_buckets + uint64_t(buckets) * 4;
        while (true) {
            auto chain = at(at_chains + uint64_t(last - first) * 4, 4);
            if (!chain) {
                return 0;
            }
            uint32_t value;
            std::memcpy(&value, chain->data(), 4);
            if (value & 1) {
                return uint64_t(last) + 1;
            }
            ++last;
        }
    }
    }
    return 0;
}

void Library::read_dynamic() {
    auto dynamic = segment(segment::dynamic);
    if (!dynamic) {
        return;
    }
    if (dynamic->offset > bytes_.size() || bytes_.size() - dynamic->offset < dynamic->filesz) {
        throw Error(name_ + ": dynamic table past the end");
    }
    Reader r(std::span(bytes_).subspan(dynamic->offset, dynamic->filesz));
    std::vector<uint64_t> needed_at;
    uint64_t soname_at = ~uint64_t(0);
    uint64_t rela = 0, relasz = 0, relaent = rela_size, jmprel = 0, pltrelsz = 0, pltrel = dt_rela;
    uint64_t relr = 0, relrsz = 0, relrent = 8;
    uint64_t android_rela = 0, android_relasz = 0;
    bool android_rel = false, plain_rel = false;
    while (r.left() >= 16) {
        int64_t tag = int64_t(r.u64());
        uint64_t value = r.u64();
        if (tag == dt_null) {
            break;
        }
        switch (tag) {
        case dt_needed: needed_at.push_back(value); break;
        case dt_soname: soname_at = value; break;
        case dt_strtab: strtab_ = value; break;
        case dt_strsz: strsz_ = value; break;
        case dt_symtab: symtab_ = value; break;
        case dt_syment:
            if (value != symbol_size) {
                throw Error(name_ + ": weird symbol size");
            }
            break;
        case dt_hash: hash_ = value; break;
        case dt_gnu_hash: gnu_hash_ = value; break;
        case dt_rela: rela = value; break;
        case dt_relasz: relasz = value; break;
        case dt_relaent: relaent = value; break;
        case dt_rel: plain_rel = true; break;
        case dt_jmprel: jmprel = value; break;
        case dt_pltrelsz: pltrelsz = value; break;
        case dt_pltrel: pltrel = value; break;
        case dt_relr:
        case dt_android_relr: relr = value; break;
        case dt_relrsz:
        case dt_android_relrsz: relrsz = value; break;
        case dt_relrent:
        case dt_android_relrent: relrent = value; break;
        case dt_android_rela: android_rela = value; break;
        case dt_android_relasz: android_relasz = value; break;
        case dt_android_rel:
        case dt_android_relsz: android_rel = true; break;
        case dt_init: init = value; break;
        case dt_fini: fini = value; break;
        case dt_preinit_array: arrays_[0].first = value; break;
        case dt_preinit_arraysz: arrays_[0].second = value; break;
        case dt_init_array: arrays_[1].first = value; break;
        case dt_init_arraysz: arrays_[1].second = value; break;
        case dt_fini_array: arrays_[2].first = value; break;
        case dt_fini_arraysz: arrays_[2].second = value; break;
        case dt_bind_now: bind_now = true; break;
        case dt_flags: bind_now |= (value & df_bind_now) != 0; break;
        case dt_flags_1: bind_now |= (value & df_1_now) != 0; break;
        default: break;
        }
    }
    if (plain_rel || android_rel) {
        throw Error(name_ + ": REL relocations, arm64 only uses RELA");
    }
    if (relaent != rela_size || relrent != 8) {
        throw Error(name_ + ": weird relocation entry sizes");
    }
    if (pltrel != dt_rela) {
        throw Error(name_ + ": plt relocations aren't RELA");
    }

    for (uint64_t at : needed_at) {
        needed.push_back(string_at(at));
    }
    if (soname_at != ~uint64_t(0)) {
        soname = string_at(soname_at);
    }

    if (symtab_) {
        uint64_t count = count_symbols(Count::sections);
        counted_ = Count::sections;
        if (count == 0) {
            count = count_symbols(Count::gnu_hash);
            counted_ = Count::gnu_hash;
        }
        if (count == 0) {
            count = count_symbols(Count::hash);
            counted_ = Count::hash;
        }
        auto table = at(symtab_, count * symbol_size);
        auto strings = at(strtab_, strsz_);
        if (!table || !strings) {
            throw Error(name_ + ": symbol table not in the file");
        }
        dynamic_symbols = read_symbols(*table, *strings);
    }

    if (rela && relasz) {
        read_rela(rela, relasz, Table::rela);
    }
    if (android_rela && android_relasz) {
        read_android(android_rela, android_relasz);
    }
    if (relr && relrsz) {
        read_relr(relr, relrsz);
    }
    if (jmprel && pltrelsz) {
        read_rela(jmprel, pltrelsz, Table::jmprel);
    }
    for (const Relocation& x : relocations) {
        if (x.symbol != 0 && x.symbol >= dynamic_symbols.size()) {
            throw Error(name_ + ": relocation with a symbol past the table");
        }
    }

}

void Library::read_rela(uint64_t address, uint64_t size, Table table) {
    auto bytes = at(address, size);
    if (!bytes) {
        throw Error(name_ + ": relocations not in the file");
    }
    Reader r(*bytes);
    while (r.left() >= rela_size) {
        Relocation x;
        x.offset = r.u64();
        uint64_t info = r.u64();
        x.symbol = uint32_t(info >> 32);
        x.type = uint32_t(info);
        x.addend = int64_t(r.u64());
        x.table = table;
        relocations.push_back(x);
    }
}

void Library::read_relr(uint64_t address, uint64_t size) {
    auto bytes = at(address, size);
    if (!bytes) {
        throw Error(name_ + ": relr table not in the file");
    }
    auto add = [&](uint64_t where) {
        // implicit addend, it's whatever sits there
        auto slot = at(where, 8);
        if (!slot) {
            throw Error(name_ + ": relr points outside the file");
        }
        Relocation x;
        x.offset = where;
        x.type = relocation::relative;
        std::memcpy(&x.addend, slot->data(), 8);
        x.table = Table::relr;
        relocations.push_back(x);
    };
    Reader r(*bytes);
    uint64_t where = 0;
    while (r.left() >= 8) {
        uint64_t entry = r.u64();
        if ((entry & 1) == 0) {
            add(entry);
            where = entry + 8;
            continue;
        }
        // bitmap, bit n = where + (n - 1) * 8
        for (unsigned bit = 1; bit < 64; ++bit) {
            if ((entry >> bit) & 1) {
                add(where + (bit - 1) * 8);
            }
        }
        where += 63 * 8;
    }
}

void Library::read_android(uint64_t address, uint64_t size) {
    auto bytes = at(address, size);
    if (!bytes || size < 4 || std::memcmp(bytes->data(), "APS2", 4) != 0) {
        throw Error(name_ + ": packed relocations that aren't APS2");
    }
    // same order as bionic's packed_reloc_iterator
    constexpr uint64_t by_info = 1, by_offset_delta = 2, by_addend = 4, has_addend = 8;
    Reader r(bytes->subspan(4));
    uint64_t count = uint64_t(r.sleb128());
    uint64_t offset = uint64_t(r.sleb128());
    uint64_t info = 0;
    int64_t addend = 0;
    uint64_t done = 0;
    while (done < count) {
        uint64_t group = uint64_t(r.sleb128());
        uint64_t flags = uint64_t(r.sleb128());
        uint64_t delta = 0;
        if (flags & by_offset_delta) {
            delta = uint64_t(r.sleb128());
        }
        if (flags & by_info) {
            info = uint64_t(r.sleb128());
        }
        if ((flags & has_addend) && (flags & by_addend)) {
            addend += r.sleb128();
        } else if (!(flags & has_addend)) {
            addend = 0;
        }
        for (uint64_t i = 0; i < group && done < count; ++i, ++done) {
            offset += (flags & by_offset_delta) ? delta : uint64_t(r.sleb128());
            if (!(flags & by_info)) {
                info = uint64_t(r.sleb128());
            }
            if ((flags & has_addend) && !(flags & by_addend)) {
                addend += r.sleb128();
            }
            Relocation x;
            x.offset = offset;
            x.type = uint32_t(info);
            x.symbol = uint32_t(info >> 32);
            x.addend = addend;
            x.table = Table::android;
            relocations.push_back(x);
        }
    }
}

const Relocation* Library::relocation_at(uint64_t address) const {
    auto it = std::lower_bound(by_offset_.begin(), by_offset_.end(), address, [&](uint32_t i, uint64_t a) { return relocations[i].offset < a; });
    if (it == by_offset_.end() || relocations[*it].offset != address) {
        return nullptr;
    }
    return &relocations[*it];
}

std::optional<uint64_t> Library::pointer(uint64_t address) const {
    if (const Relocation* x = relocation_at(address)) {
        switch (x->type) {
        case relocation::relative:
        case relocation::irelative:
            return uint64_t(x->addend);
        case relocation::abs64:
        case relocation::glob_dat:
        case relocation::jump_slot: {
            if (x->symbol == 0) {
                return uint64_t(x->addend);
            }
            const Symbol& s = dynamic_symbols[x->symbol];
            if (!s.defined()) {
                return std::nullopt;
            }
            // preemptible but it's ours unless something else exports it
            return s.value + uint64_t(x->addend);
        }
        default:
            return std::nullopt;
        }
    }
    auto slot = at(address, 8);
    if (!slot) {
        return std::nullopt;
    }
    uint64_t value;
    std::memcpy(&value, slot->data(), 8);
    return value;
}

std::vector<uint64_t> Library::read_array(uint64_t address, uint64_t size) const {
    std::vector<uint64_t> entries;
    for (uint64_t i = 0; i + 8 <= size; i += 8) {
        entries.push_back(pointer(address + i).value_or(0));
    }
    return entries;
}

}  // namespace tsugiki::elf
