// svc, system registers, cache ops and crc32 for translated code on android
#include <time.h>

#include <cstring>

#include "android/runtime.hpp"

using namespace tsugiki::rt;

extern "C" {

void tsugiki_svc(tsugiki_state* s, uint32_t) {
    s->x[0] = uint64_t(guest_syscall(s->x[8], s->x[0], s->x[1], s->x[2], s->x[3], s->x[4], s->x[5]));
}

uint64_t tsugiki_mrs(tsugiki_state*, uint32_t reg) {
    switch (reg) {
    // cntvct_el0 at 1 ghz
    case 0xdf02: {
        timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        return uint64_t(t.tv_sec) * 1000000000ull + uint64_t(t.tv_nsec);
    }
    case 0xdf00: return 1000000000;
    // ctr_el0, 64 byte lines
    case 0xd801: return 0x8444c004;
    // dczid_el0, dc zva on 64 bytes
    case 0xd807: return 4;
    // midr_el1, a cortex-a76
    case 0xc000: return 0x414fd0b1;
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
