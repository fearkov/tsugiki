// the c++ side of entry.S: exports and callbacks into guest code
#include <cstring>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include "android/runtime.hpp"

extern "C" char tsugiki_callbacks[];
extern "C" char tsugiki_callbacks_end[];

namespace tsugiki::rt {

namespace {

struct Frame {
    uint64_t ints[6];
    uint64_t fps[8];
    tsugiki_module* module;
    uint64_t vaddr;
    const uint64_t* stack;
    uint64_t kind;
    uint64_t fp_result;
};

struct Slot {
    uint64_t guest;
    int kind;
};

std::mutex slot_lock;
std::vector<Slot> slots;
std::map<std::pair<uint64_t, int>, size_t> slot_of;

}  // namespace

uint64_t host_callback(uint64_t guest_function, int kind) {
    if (guest_function == 0) {
        return 0;
    }
    std::lock_guard guard(slot_lock);
    auto key = std::make_pair(guest_function, kind);
    auto it = slot_of.find(key);
    if (it == slot_of.end()) {
        size_t most = size_t(tsugiki_callbacks_end - tsugiki_callbacks) / 16;
        if (slots.size() >= most) {
            fail("out of callback trampolines");
        }
        slots.push_back({guest_function, kind});
        it = slot_of.emplace(key, slots.size() - 1).first;
    }
    return reinterpret_cast<uint64_t>(tsugiki_callbacks + 16 * it->second);
}

// a plain callback slot's guest function, 0 if it isn't one
uint64_t callback_target(uint64_t address) {
    uint64_t begin = reinterpret_cast<uint64_t>(tsugiki_callbacks);
    if (address - begin >= uint64_t(tsugiki_callbacks_end - tsugiki_callbacks) || (address - begin) % 16) {
        return 0;
    }
    std::lock_guard guard(slot_lock);
    size_t i = (address - begin) / 16;
    return i < slots.size() && slots[i].kind == plain ? slots[i].guest : 0;
}

}  // namespace tsugiki::rt

using namespace tsugiki::rt;

extern "C" uint64_t tsugiki_enter(Frame* f) {
    uint64_t function;
    int kind = int(f->kind);
    if (kind == 3) {
        Slot slot;
        {
            std::lock_guard guard(slot_lock);
            slot = slots.at(f->vaddr);
        }
        function = slot.guest;
        kind = slot.kind;
    } else if (kind == 4) {
        // a raw guest pointer the host called, see on_segv
        function = f->vaddr;
        kind = plain;
    } else {
        tsugiki_load(f->module);
        function = *f->module->base + f->vaddr;
    }
    Call call;
    for (int i = 0; i < 6; ++i) call.ints[i] = f->ints[i];
    // the host's 7th and 8th ints are on its stack, the guest's in x6 and x7
    call.ints[6] = f->stack[0];
    call.ints[7] = f->stack[1];
    for (int i = 0; i < 8; ++i) call.fps[i] = f->fps[i];
    call.stack = f->stack + 2;
    call.stack_count = 8;
    if (kind == jni_env) {
        call.ints[0] = guest_env(reinterpret_cast<void*>(call.ints[0]));
    } else if (kind == jni_vm) {
        call.ints[0] = guest_vm(reinterpret_cast<void*>(call.ints[0]));
    }
    return run_guest(function, call, &f->fp_result);
}
