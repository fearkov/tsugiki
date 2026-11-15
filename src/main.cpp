#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "apk.hpp"
#include "bytes.hpp"
#include "eh_frame.hpp"
#include "elf.hpp"

using namespace tsugiki;

namespace {

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw Error(path + ": can't open");
    }
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

std::string kib(uint64_t bytes) {
    return std::to_string((bytes + 1023) / 1024) + " KiB";
}

std::string hex(uint64_t value) {
    char text[32];
    std::snprintf(text, sizeof text, "0x%llx", static_cast<unsigned long long>(value));
    return text;
}

void describe(const elf::Library& library) {
    std::cout << library.name() << " (" << kib(library.bytes().size()) << ", "
              << (library.type() == 3 ? "shared object" : "executable") << ")\n";
    if (!library.soname.empty()) {
        std::cout << "  soname " << library.soname << "\n";
    }
    if (!library.needed.empty()) {
        std::cout << "  needs";
        for (const std::string& name : library.needed) {
            std::cout << " " << name;
        }
        std::cout << "\n";
    }

    for (const elf::Segment& s : library.segments()) {
        if (s.type == elf::segment::load && s.executable()) {
            std::cout << "  code " << hex(s.vaddr) << ".." << hex(s.vaddr + s.memsz) << " (" << kib(s.memsz) << ")\n";
        }
    }
    if (auto tls = library.segment(elf::segment::tls)) {
        std::cout << "  tls " << tls->memsz << " bytes\n";
    }

    int functions = 0, objects = 0, imports = 0, tls = 0;
    for (size_t i = 1; i < library.dynamic_symbols.size(); ++i) {
        const elf::Symbol& s = library.dynamic_symbols[i];
        if (!s.defined()) {
            imports += s.bind != elf::symbol::local;
        } else if (s.code()) {
            ++functions;
        } else if (s.type == elf::symbol::tls) {
            ++tls;
        } else if (s.type == elf::symbol::object) {
            ++objects;
        }
    }
    const char* counted[] = {"section headers", "DT_HASH", "DT_GNU_HASH"};
    std::cout << "  symbols: " << functions << " functions, " << objects << " objects, " << tls << " tls, " << imports << " imports"
              << " (count from " << counted[int(library.symbol_count_from())] << ")\n";
    if (!library.static_symbols.empty()) {
        std::cout << "  not stripped: " << library.static_symbols.size() << " in .symtab\n";
    }

    std::map<uint32_t, int> by_type;
    std::map<elf::Table, int> by_table;
    for (const elf::Relocation& x : library.relocations) {
        ++by_type[x.type];
        ++by_table[x.table];
    }
    std::cout << "  relocations:";
    for (auto [type, count] : by_type) {
        std::cout << " " << count << " " << elf::relocation::name(type);
    }
    std::cout << "\n   from";
    const char* tables[] = {"rela", "jmprel", "relr", "android aps2"};
    for (auto [table, count] : by_table) {
        std::cout << " " << count << " " << tables[int(table)];
    }
    std::cout << "\n";

    auto fdes = eh_frame::read(library);
    int with_lsda = 0;
    std::set<uint64_t> starts;
    for (const eh_frame::Fde& f : fdes) {
        with_lsda += f.lsda.has_value();
        starts.insert(f.start);
    }
    int exported_outside = 0;
    for (const elf::Symbol& s : library.dynamic_symbols) {
        if (s.defined() && s.code() && !starts.contains(s.value)) {
            ++exported_outside;
        }
    }
    std::cout << "  functions: " << fdes.size() << " in eh_frame, " << with_lsda << " with landing pads, " << exported_outside
              << " exported ones without an fde\n";
    if (library.init || !library.init_array.empty() || !library.preinit_array.empty()) {
        std::cout << "  init: " << library.init_array.size() << " in init_array" << (library.init ? ", DT_INIT" : "") << "\n";
    }
}

int info(const std::vector<std::string>& paths) {
    for (const std::string& path : paths) {
        auto head = read_file(path);
        if (apk::looks_like_zip(head)) {
            apk::Apk apk(path);
            auto abis = apk.abis();
            std::cout << path << ": abis";
            for (const std::string& abi : abis) {
                std::cout << " " << abi;
            }
            std::cout << (abis.empty() ? " none, no native code" : "") << "\n";
            for (const std::string& name : apk.libraries("arm64-v8a")) {
                describe(elf::Library(apk.read(name), name));
            }
        } else {
            describe(elf::Library(std::move(head), path));
        }
    }
    return 0;
}

void usage() {
    std::cerr << "usage: tsugiki info <file.apk | lib.so>...\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) {
        usage();
        return 2;
    }
    try {
        std::string command = args[0];
        args.erase(args.begin());
        if (command == "info" && !args.empty()) {
            return info(args);
        }
        usage();
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "tsugiki: " << e.what() << "\n";
        return 1;
    }
}
