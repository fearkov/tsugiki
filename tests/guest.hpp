// an arm64 lib loaded and its translation registered, for tests
#pragma once

#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "elf.hpp"
#include "host.hpp"
#include "loader.hpp"
#include "test.hpp"

// while one of these lives, guest code runs thru the interpreter, translations ignored
inline bool& interpreted() {
    static bool on = false;
    return on;
}

struct Interpreted {
    explicit Interpreted(bool without_ops = false) {
        interpreted() = true;
        tsugiki_interpret_all(true, without_ops);
    }
    ~Interpreted() {
        interpreted() = false;
        tsugiki_interpret_all(false);
    }
};

struct Guest {
    std::unique_ptr<tsugiki::elf::Library> library;
    Image image;
    std::vector<uint64_t> stack = std::vector<uint64_t>(1 << 17);
    tsugiki_state state{};
    std::unique_ptr<tsugiki_cache> cache = fresh_cache();

    static std::unique_ptr<tsugiki_cache> fresh_cache() {
        auto c = std::make_unique<tsugiki_cache>();
        for (auto& pc : c->pc) {
            pc = ~uint64_t(0);
        }
        return c;
    }

    Guest(const std::string& path, const tsugiki_entry* entries, uint64_t count, uint64_t* base, const tsugiki_op* ops, uint64_t op_count) {
        std::ifstream file(path, std::ios::binary);
        std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        library = std::make_unique<tsugiki::elf::Library>(std::move(bytes), path);
        image = load_image(*library, [](const std::string&) { return uint64_t(0); });
        *base = image.base;
        tsugiki_add_module(image.base, image.size, entries, count, ops, op_count);
        // bionic's tls slots, the stack guard in slot 5 like on arm64 and x86_64
        tls[5] = 0x5ca1ab1e0ddba11ull;
        state.tpidr = reinterpret_cast<uint64_t>(tls);
        state.cache = cache.get();
    }

    uint64_t tls[16] = {};

    uint64_t address(const std::string& name) const {
        for (const tsugiki::elf::Symbol& s : library->dynamic_symbols) {
            if (s.name == name && s.defined()) {
                return image.base + s.value;
            }
        }
        throw test::Failure{"no guest function " + name};
    }

    // x args, then fp args as raw bits into v0..v7
    uint64_t call(const std::string& name, std::initializer_list<uint64_t> args, std::initializer_list<uint64_t> fp = {}) {
        state.x[31] = reinterpret_cast<uint64_t>(stack.data() + stack.size()) & ~uint64_t(15);
        int i = 0;
        for (uint64_t bits : fp) {
            state.v[i].lo = bits;
            state.v[i].hi = 0;
            ++i;
        }
        // the dispatch cache holds translated code, the interpreter must never see it
        state.cache = interpreted() ? nullptr : cache.get();
        return tsugiki_run(&state, address(name), args);
    }

    double d0() const { double d; std::memcpy(&d, &state.v[0].lo, 8); return d; }
    float s0() const { float f; std::memcpy(&f, &state.v[0].lo, 4); return f; }
};

inline uint64_t bits_of(double d) { uint64_t b; std::memcpy(&b, &d, 8); return b; }
inline uint64_t bits_of(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }
