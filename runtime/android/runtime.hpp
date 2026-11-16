// runtime internals shared by its files
#pragma once

#include <dlfcn.h>
#include <signal.h>

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
// logs the last imports this thread went through generic_call with
void dump_recent_calls();

// whatever a guest import resolves to by name, 0 if nothing
uint64_t special_import(const std::string& name);
uint64_t jni_import(const std::string& name);

// host-callable pointer that runs a guest function, kind as below
enum Kind : int { plain = 0, jni_env = 1, jni_vm = 2 };
uint64_t host_callback(uint64_t guest_function, int kind);

// runs a guest function on this thread, nested or not
struct Call {
    uint64_t ints[8] = {};
    // whole vector registers, lo then hi
    uint64_t fps[8][2] = {};
    // more int args, pushed on the guest stack
    const uint64_t* stack = nullptr;
    int stack_count = 0;
};
// fp_result gets all of v0, two words
uint64_t run_guest(uint64_t function, const Call& call, uint64_t* fp_result = nullptr);

// the guest's view of jni
uint64_t guest_env(void* host_env);
uint64_t guest_vm(void* host_vm);
void* host_env_of(uint64_t guest_env);

// guest arg readers for thunks: the nth int or fp arg of a call, past x0..x7 and v0..v7
// the host env or vm behind a guest one, anything else as it is
uint64_t host_view(uint64_t value);
// which slot of the guest env or vm table a stub address is, -1 if neither
int jni_stub_slot(uint64_t address, bool* vm);

// a sigsegv we don't handle, passed to the guest's handler; false if it has none
bool guest_segv(int sig, siginfo_t* info, void* context);

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

// whether a host address is in some lib's code
bool host_code(void* address);

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
// a syscall by its arm64 number, -errno like the kernel, flags and structs that differ converted
long guest_syscall(uint64_t arm, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f);
// open flags: arm64 has O_DIRECTORY and O_NOFOLLOW where x86 has O_DIRECT and O_LARGEFILE
uint64_t host_open_flags(uint64_t guest);
uint64_t guest_open_flags(uint64_t host);

[[noreturn]] void fail(const char* what, ...);
void log(const char* what, ...);

}  // namespace tsugiki::rt
