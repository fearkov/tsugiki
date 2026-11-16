#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "android.hpp"
#include "apk.hpp"
#include "resources.hpp"
#include "bytes.hpp"
#include "eh_frame.hpp"
#include "elf.hpp"
#include "lift/lift.hpp"

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
    // how much of it we can translate yet
    auto program = lift::discover(library);
    size_t total = 0, simd = 0, invalid = 0;
    for (const auto& f : program.functions) {
        for (const auto& in : f.code) {
            ++total;
            simd += in.op == a64::Op::simd;
            invalid += in.op == a64::Op::invalid;
        }
    }
    std::cout << "  instructions: " << total << " in " << program.functions.size() << " functions, " << simd << " fp/simd not decoded yet, "
              << invalid << " invalid\n";
    size_t brs = 0, jump_tables = 0, targets = 0;
    std::vector<uint64_t> missed;
    for (const auto& f : program.functions) {
        for (size_t i = 0; i < f.code.size(); ++i) {
            brs += f.code[i].op == a64::Op::br;
            // shaped like a table but none found
            if (f.code[i].op == a64::Op::br && i > 0 && !f.tables.contains(f.start + 4 * i) &&
                (f.code[i - 1].op == a64::Op::addsub_shifted || f.code[i - 1].op == a64::Op::addsub_extended)) {
                missed.push_back(f.start + 4 * i);
            }
        }
        jump_tables += f.tables.size();
        for (const auto& [_, t] : f.tables) {
            targets += t.size();
        }
    }
    std::cout << "  br: " << brs << ", " << jump_tables << " jump tables with " << targets << " targets";
    if (!missed.empty()) {
        std::cout << ", " << missed.size() << " look like tables but aren't, like " << hex(missed[0]);
    }
    std::cout << "\n";
    size_t flags_in = 0, flags_out = 0, flags_after = 0;
    for (const lift::Function& f : program.functions) {
        flags_in += f.flags_in;
        flags_out += f.flags_out;
        flags_after += f.flags_after.size();
    }
    if (flags_in || flags_out) {
        std::cout << "  flags across calls: " << flags_in << " functions read the caller's, " << flags_out << " hand them back to "
                  << flags_after << " calls\n";
    }
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
            auto names = apk.names();
            if (std::find(names.begin(), names.end(), "resources.arsc") != names.end()) {
                if (auto arsc = resources::retarget_qt_libs(apk.read("resources.arsc"))) {
                    auto main = resources::manifest_meta(apk.read("AndroidManifest.xml"), "android.app.lib_name");
                    std::cout << "  qt: libs listed per abi, main lib " << (main ? *main : "unknown") << ", resources.arsc "
                              << apk.read("resources.arsc").size() << " -> " << arsc->size() << " bytes\n";
                }
            }
            for (const std::string& name : apk.libraries("arm64-v8a")) {
                describe(elf::Library(apk.read(name), name));
            }
        } else {
            describe(elf::Library(std::move(head), path));
        }
    }
    return 0;
}

int translate(const std::string& path, const std::string& id, const std::string& out) {
    elf::Library library(read_file(path), path);
    auto program = lift::discover(library);
    std::ofstream file(out);
    file << lift::emit(program, id);
    if (!file) {
        throw Error(out + ": can't write");
    }
    size_t instructions = 0;
    for (const auto& f : program.functions) {
        instructions += f.code.size();
    }
    std::cout << path << ": " << program.functions.size() << " functions, " << instructions << " instructions\n";
    return 0;
}

// x86_64 android libs from arm64 ones, plus libtsugiki.so
int build(const std::vector<std::string>& paths, const std::string& dir, int jobs) {
    android::build_runtime(dir);
    android::build_libraries(paths, dir, jobs);
    return 0;
}

void usage() {
    std::cerr << "usage: tsugiki info <file.apk | lib.so>...\n"
                 "       tsugiki translate <lib.so> <id> <out.cpp>\n"
                 "       tsugiki build [-j n] <lib.so>... -o <dir>\n"
                 "       tsugiki apk [-j n] [--force] <in.apk> -o <out.apk>\n";
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
        int jobs = android::default_jobs();
        for (size_t i = 0; i + 1 < args.size(); ++i) {
            if (args[i] == "-j") {
                jobs = std::max(1, std::atoi(args[i + 1].c_str()));
                args.erase(args.begin() + long(i), args.begin() + long(i) + 2);
                break;
            }
        }
        if (command == "info" && !args.empty()) {
            return info(args);
        }
        if (command == "translate" && args.size() == 3) {
            return translate(args[0], args[1], args[2]);
        }
        if (command == "apk") {
            bool force = !args.empty() && args[0] == "--force";
            if (force) {
                args.erase(args.begin());
            }
            if (args.size() == 3 && args[1] == "-o") {
                android::build_apk(args[0], args[2], force, jobs);
                return 0;
            }
        }
        if (command == "build" && args.size() >= 3 && args[args.size() - 2] == "-o") {
            std::string dir = args.back();
            args.resize(args.size() - 2);
            return build(args, dir, jobs);
        }
        usage();
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "tsugiki: " << e.what() << "\n";
        return 1;
    }
}
