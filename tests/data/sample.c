// test lib, built for arm64 with no libc
// exports, imports, code pointers in data, a ctor, tls, a jump table

#include <stdint.h>

extern int imported_function(int);
extern int imported_counter;
extern void (*imported_hook)(void);

static int add(int a, int b) { return a + b; }
static int sub(int a, int b) { return a - b; }
static int mul(int a, int b) { return a * b; }

typedef int (*operation)(int, int);
operation operations[] = {add, sub, mul};
const char* names[] = {"add", "sub", "mul"};

__thread int per_thread = 7;

int initialized;
// calls out so clang can't fold it into the data
__attribute__((constructor)) static void setup(void) { initialized = imported_function(1); }

int apply(int which, int a, int b) { return operations[which](a, b); }
int (*exported_pointer)(int, int, int) = apply;

int call_out(int x) {
    if (imported_hook) {
        imported_hook();
    }
    return imported_function(x) + imported_counter;
}

int thread_local_value(void) { return per_thread++; }

int classify(int x) {
    switch (x) {
    case 0: return 10;
    case 1: return 21;
    case 2: return 33;
    case 3: return 47;
    case 4: return 52;
    case 5: return 68;
    case 6: return 71;
    case 7: return 89;
    default: return -1;
    }
}

uint64_t fib(uint64_t n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
