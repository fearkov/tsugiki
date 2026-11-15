// runtime internals shared by its files
#pragma once

#include <dlfcn.h>

#include <vector>

#include <cstdint>
#include <string>

#include "tsugiki.h"

namespace tsugiki::rt {

// a guest-visible address for something on the host
struct Stub {
    void (*call)(tsugiki_state* s, const Stub& stub);
    void* host;
    uint64_t data;
    const char* name;
};

uint64_t make_stub(const Stub& stub);

// an import's host function, called with guest registers in place
void generic_call(tsugiki_state* s, const Stub& stub);

// whatever a guest import resolves to by name, 0 if nothing
uint64_t special_import(const std::string& name);
uint64_t jni_import(const std::string& name);

// host-callable pointer that runs a guest function, kind as below
enum Kind : int { plain = 0, jni_env = 1, jni_vm = 2 };
uint64_t host_callback(uint64_t guest_function, int kind);

// runs a guest function on this thread, nested or not
struct Call {
    uint64_t ints[8] = {};
    uint64_t fps[8] = {};
    // more int args, pushed on the guest stack
    const uint64_t* stack = nullptr;
    int stack_count = 0;
};
uint64_t run_guest(uint64_t function, const Call& call, uint64_t* fp_result = nullptr);

// the guest's view of jni
uint64_t guest_env(void* host_env);
uint64_t guest_vm(void* host_vm);
void* host_env_of(uint64_t guest_env);

// guest arg readers for thunks: the nth int or fp arg of a call, past x0..x7 and v0..v7
// a plain callback slot's guest function, 0 if it isn't one
uint64_t callback_target(uint64_t address);

// a loaded guest lib, for dl_iterate_phdr
struct GuestModule {
    uint64_t base;
    uint64_t size;
    const char* soname;
    // Elf64_Phdr, the same on both sides
    const void* phdrs;
    uint16_t phnum;
};
std::vector<GuestModule> guest_modules();
// dladdr on a guest address, false if it isn't one
bool guest_dladdr(uint64_t address, Dl_info* info);

// this thread's guest registers, made on first use
tsugiki_state* guest_state();
uint64_t int_arg(const tsugiki_state* s, int n);

// the stop address code returns to when the host called it
uint64_t stop_address();

// a translated lib's export, 0 if none has it
uint64_t lookup_guest(const std::string& name);

// aarch64 va_list, which arm64 passes as a pointer to a copy
struct GuestVaList {
    uint64_t stack;
    uint64_t gr_top;
    uint64_t vr_top;
    int32_t gr_offs;
    int32_t vr_offs;
};

uint64_t next_int(GuestVaList& ap);
double next_double(GuestVaList& ap);
GuestVaList va(uint64_t pointer);

// x86_64 number for an arm64 syscall, -1 if we don't know it
long host_syscall_number(uint64_t arm);

[[noreturn]] void fail(const char* what, ...);
void log(const char* what, ...);

}  // namespace tsugiki::rt
