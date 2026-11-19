// host runtime bits for tests
#pragma once

#include <initializer_list>

#include "tsugiki.h"

// x30 when we call in, the run stops when code returns here
constexpr uint64_t tsugiki_stop = 0x7ffffffff000ull;

void tsugiki_add_module(uint64_t base, uint64_t size, const tsugiki_entry* entries, uint64_t count, const tsugiki_op* ops = nullptr,
                        uint64_t op_count = 0);
// everything in guest modules goes thru the interpreter while on
void tsugiki_interpret_all(bool on);
void tsugiki_add_host(uint64_t address, tsugiki_code code);

// calls guest code until it returns, x0 back
uint64_t tsugiki_run(tsugiki_state* s, uint64_t function, std::initializer_list<uint64_t> args);
