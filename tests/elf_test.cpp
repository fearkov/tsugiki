#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "eh_frame.hpp"
#include "elf.hpp"
#include "test.hpp"

using namespace tsugiki;

namespace {

const std::vector<std::string> packings = {"rela", "relr", "android", "android_relr", "android_tags"};
const std::vector<std::string> all_samples = {"rela", "relr", "android", "android_relr", "android_tags", "gnu_hash", "sysv_hash"};

std::string sample_path(const std::string& variant) {
    return std::string(SAMPLE_DIR) + "/libsample_" + variant + ".so";
}

elf::Library sample(const std::string& variant) {
    std::ifstream file(sample_path(variant), std::ios::binary);
    CHECK(file.good());
    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    return elf::Library(std::move(bytes), variant);
}

const elf::Symbol& find(const std::vector<elf::Symbol>& symbols, const std::string& name) {
    for (const elf::Symbol& s : symbols) {
        if (s.name == name) {
            return s;
        }
    }
    throw test::Failure{"no symbol " + name};
}

std::string run(const std::string& command) {
    FILE* pipe = popen(command.c_str(), "r");
    CHECK(pipe != nullptr);
    std::string output;
    char buffer[4096];
    while (size_t got = fread(buffer, 1, sizeof buffer, pipe)) {
        output.append(buffer, got);
    }
    pclose(pipe);
    return output;
}

bool is_hex(const std::string& word) {
    return !word.empty() && word.find_first_not_of("0123456789abcdef") == std::string::npos;
}

// (offset, type name) from llvm-readelf -r, relr lines decoded too
std::vector<std::pair<uint64_t, std::string>> readelf_relocations(const std::string& path) {
    std::vector<std::pair<uint64_t, std::string>> found;
    std::istringstream lines(run(std::string(LLVM_READELF) + " -r --wide " + path));
    std::string line;
    bool relr = false;
    while (std::getline(lines, line)) {
        if (line.starts_with("Relocation section")) {
            relr = line.find(".relr") != std::string::npos;
            continue;
        }
        std::istringstream words(line);
        std::vector<std::string> w{std::istream_iterator<std::string>(words), std::istream_iterator<std::string>()};
        if (w.empty()) {
            continue;
        }
        if (relr) {
            // "0001:  entry address  sym" or "      address  sym"
            if (w[0].ends_with(":") && w.size() >= 3 && is_hex(w[2])) {
                found.emplace_back(std::stoull(w[2], nullptr, 16), "RELATIVE");
            } else if (is_hex(w[0]) && line.starts_with(" ")) {
                found.emplace_back(std::stoull(w[0], nullptr, 16), "RELATIVE");
            }
        } else if (w.size() >= 3 && is_hex(w[0]) && w[2].starts_with("R_AARCH64_")) {
            found.emplace_back(std::stoull(w[0], nullptr, 16), w[2].substr(10));
        }
    }
    std::sort(found.begin(), found.end());
    return found;
}

}  // namespace

TEST(relocations_match_llvm_readelf) {
    if (std::string(LLVM_READELF).empty() || std::string(LLVM_READELF).ends_with("NOTFOUND")) {
        return;
    }
    for (const std::string& variant : all_samples) {
        elf::Library library = sample(variant);
        std::vector<std::pair<uint64_t, std::string>> ours;
        for (const elf::Relocation& x : library.relocations) {
            ours.emplace_back(x.offset, elf::relocation::name(x.type));
        }
        std::sort(ours.begin(), ours.end());
        auto theirs = readelf_relocations(sample_path(variant));
        CHECK(!theirs.empty());
        if (ours != theirs) {
            throw test::Failure{variant + ": relocations differ from llvm-readelf"};
        }
    }
}

TEST(packings_give_the_same_relocations) {
    // layouts move between packings, so compare what they point at
    auto summary = [](const elf::Library& library) {
        std::multiset<std::string> kinds;
        for (const elf::Relocation& x : library.relocations) {
            std::string target = x.symbol ? library.dynamic_symbols[x.symbol].name : "";
            kinds.insert(elf::relocation::name(x.type) + " " + target);
        }
        return kinds;
    };
    auto expected = summary(sample("rela"));
    for (const std::string& variant : packings) {
        CHECK(summary(sample(variant)) == expected);
    }
    // every relative one went to relr, and aps2 got them too
    int relative = 0, relr = 0, android = 0;
    for (const elf::Relocation& x : sample("rela").relocations) {
        relative += x.type == elf::relocation::relative;
    }
    for (const elf::Relocation& x : sample("relr").relocations) {
        relr += x.table == elf::Table::relr;
    }
    for (const elf::Relocation& x : sample("android").relocations) {
        android += x.table == elf::Table::android;
    }
    CHECK(relative >= 7);
    CHECK_EQ(relr, relative);
    CHECK(android > relative);
}

TEST(pointers_resolve_into_the_library) {
    for (const std::string& variant : packings) {
        elf::Library library = sample(variant);
        const auto& symbols = library.static_symbols;
        uint64_t operations = find(symbols, "operations").value;
        CHECK_EQ(library.pointer(operations).value(), find(symbols, "add").value);
        CHECK_EQ(library.pointer(operations + 8).value(), find(symbols, "sub").value);
        CHECK_EQ(library.pointer(operations + 16).value(), find(symbols, "mul").value);
        CHECK_EQ(library.pointer(find(symbols, "exported_pointer").value).value(), find(symbols, "apply").value);
        CHECK_EQ(library.init_array.size(), 1u);
        CHECK_EQ(library.init_array[0], find(symbols, "setup").value);
        // strings in names[] point into rodata, not code
        uint64_t name = library.pointer(find(symbols, "names").value).value();
        CHECK(!library.executable(name));
        auto text = library.at(name, 4);
        CHECK(text && std::string(text->begin(), text->end() - 1) == "add");
    }
}

TEST(imports_and_tls) {
    for (const std::string& variant : packings) {
        elf::Library library = sample(variant);
        const auto& dynamic = library.dynamic_symbols;
        CHECK(!find(dynamic, "imported_function").defined());
        CHECK(!find(dynamic, "imported_counter").defined());
        CHECK(!find(dynamic, "imported_hook").defined());
        std::set<std::string> kinds;
        for (const elf::Relocation& x : library.relocations) {
            kinds.insert(elf::relocation::name(x.type) + " " + dynamic[x.symbol].name);
        }
        CHECK(kinds.contains("JUMP_SLOT imported_function"));
        CHECK(kinds.contains("GLOB_DAT imported_counter"));
        CHECK(kinds.contains("TLSDESC per_thread"));
        CHECK(library.segment(elf::segment::tls).has_value());
        // an import is nothing we can point at
        for (const elf::Relocation& x : library.relocations) {
            if (x.symbol && dynamic[x.symbol].name == "imported_counter") {
                CHECK(!library.pointer(x.offset).has_value());
            }
        }
    }
}

TEST(symbol_counts_agree) {
    using Count = elf::Library::Count;
    elf::Library both = sample("rela");
    uint64_t count = both.count_symbols(Count::sections);
    CHECK(count > 10);
    CHECK_EQ(both.count_symbols(Count::hash), count);
    CHECK_EQ(both.count_symbols(Count::gnu_hash), count);

    elf::Library gnu = sample("gnu_hash");
    CHECK_EQ(gnu.count_symbols(Count::hash), 0u);
    CHECK_EQ(gnu.count_symbols(Count::gnu_hash), gnu.count_symbols(Count::sections));

    elf::Library sysv = sample("sysv_hash");
    CHECK_EQ(sysv.count_symbols(Count::gnu_hash), 0u);
    CHECK_EQ(sysv.count_symbols(Count::hash), sysv.count_symbols(Count::sections));
}

TEST(every_function_has_an_fde) {
    for (const std::string& variant : all_samples) {
        elf::Library library = sample(variant);
        auto fdes = eh_frame::read(library);
        CHECK(!fdes.empty());
        std::map<uint64_t, uint64_t> sizes;
        for (const eh_frame::Fde& f : fdes) {
            CHECK(library.executable(f.start));
            sizes[f.start] = f.size;
        }
        int functions = 0;
        for (const elf::Symbol& s : library.static_symbols) {
            if (s.type == elf::symbol::function && s.defined() && s.size > 0) {
                ++functions;
                CHECK(sizes.contains(s.value));
                CHECK_EQ(sizes[s.value], s.size);
            }
        }
        CHECK(functions >= 9);
    }
}
