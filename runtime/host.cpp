// the runtime on a plain linux host, for tests
#include "host.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

namespace {

struct Module {
    uint64_t base;
    uint64_t size;
    const tsugiki_entry* entries;
    uint64_t count;
};

std::vector<Module>& modules() {
    static std::vector<Module> all;
    return all;
}

std::map<uint64_t, tsugiki_code>& hosts() {
    static std::map<uint64_t, tsugiki_code> all;
    return all;
}

tsugiki_code find(uint64_t address) {
    for (const Module& m : modules()) {
        if (address - m.base < m.size) {
            uint64_t offset = address - m.base;
            const tsugiki_entry* end = m.entries + m.count;
            const tsugiki_entry* e = std::lower_bound(m.entries, end, offset, [](const tsugiki_entry& a, uint64_t o) { return a.address < o; });
            return e != end && e->address == offset ? e->code : nullptr;
        }
    }
    auto host = hosts().find(address);
    return host != hosts().end() ? host->second : nullptr;
}

}  // namespace

void tsugiki_add_module(uint64_t base, uint64_t size, const tsugiki_entry* entries, uint64_t count) {
    modules().push_back({base, size, entries, count});
}

void tsugiki_add_host(uint64_t address, tsugiki_code code) {
    hosts()[address] = code;
}

uint64_t tsugiki_run(tsugiki_state* s, uint64_t function, std::initializer_list<uint64_t> args) {
    size_t i = 0;
    for (uint64_t a : args) {
        s->x[i++] = a;
    }
    s->x[30] = tsugiki_stop;
    s->pc = function;
    while (s->pc != tsugiki_stop) {
        tsugiki_call(s);
    }
    return s->x[0];
}

extern "C" {

void tsugiki_call(tsugiki_state* s) {
    tsugiki_code code = find(s->pc);
    if (code == nullptr) {
        tsugiki_trap(s, s->pc, 0, "nothing to run there");
    }
    if (s->cache) {
        uint64_t i = tsugiki_cache_slot(s->pc);
        s->cache->pc[i] = s->pc;
        s->cache->code[i] = code;
    }
    code(s);
}

void tsugiki_svc(tsugiki_state* s, uint32_t) {
    tsugiki_trap(s, s->pc, 0, "svc");
}

void tsugiki_trap(tsugiki_state* s, uint64_t pc, uint32_t raw, const char* why) {
    std::fprintf(stderr, "tsugiki: %s at 0x%llx (0x%08x), x30 0x%llx\n", why, (unsigned long long)pc, raw, (unsigned long long)s->x[30]);
    std::abort();
}

uint64_t tsugiki_mrs(tsugiki_state*, uint32_t reg) {
    switch (reg) {
    // cntvct_el0, 1 ghz
    case 0xdf02:
        return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    // cntfrq_el0
    case 0xdf00: return 1000000000;
    // ctr_el0, 64 byte lines
    case 0xd801: return 0x8444c004;
    // dczid_el0, dc zva on 64 bytes
    case 0xd807: return 4;
    default: return 0;
    }
}

void tsugiki_msr(tsugiki_state*, uint32_t, uint64_t) {}

void tsugiki_sys(tsugiki_state*, uint32_t op, uint64_t value) {
    // dc zva
    if (op == 0x1ba1) {
        std::memset(reinterpret_cast<void*>(value & ~uint64_t(63)), 0, 64);
    }
}

uint32_t tsugiki_crc32(uint32_t crc, uint64_t value, int bytes, int castagnoli) {
    uint32_t polynomial = castagnoli ? 0x82f63b78 : 0xedb88320;
    for (int i = 0; i < bytes; ++i) {
        crc ^= uint8_t(value >> (8 * i));
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (polynomial & (0u - (crc & 1)));
        }
    }
    return crc;
}

}  // extern "C"
