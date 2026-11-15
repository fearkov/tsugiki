// arm64 code that calls into the real libc, built with the ndk
// the device check runs it translated, through every bridge we have

#define _BSD_SOURCE
#include <SLES/OpenSLES.h>
#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <unistd.h>

static int by_value(const void* a, const void* b) {
    int x = *(const int*)a, y = *(const int*)b;
    return (x > y) - (x < y);
}

// host qsort calling a guest comparator
int bridge_qsort(int* p, int n) {
    qsort(p, (size_t)n, sizeof(int), by_value);
    int sorted = 1;
    for (int i = 1; i < n; ++i) sorted &= p[i - 1] <= p[i];
    return sorted;
}

// variadic into the host, ints doubles and strings mixed
int bridge_format(char* out, int size, int i, double d, const char* s, long long l, float f) {
    return snprintf(out, (size_t)size, "%d|%.3f|%s|%lld|%g|%x|%c|%5.1e|%s", i, d, s, l, (double)f, i * 3, 'z', d * 1000, "end");
}

static int vformat(char* out, int size, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, (size_t)size, fmt, ap);
    va_end(ap);
    return n;
}

// a guest va_list handed to the host
int bridge_vformat(char* out, int size, int a, double b, int c, double d, int e, int f, int g, int h, int i, double j) {
    return vformat(out, size, "%d %.2f %d %.2f %d %d %d %d %d %.2f", a, b, c, d, e, f, g, h, i, j);
}

struct work {
    int n;
    long sum;
};

static void* worker(void* arg) {
    struct work* w = arg;
    for (int i = 1; i <= w->n; ++i) w->sum += i;
    return w;
}

// a guest thread made by the host's pthread
long bridge_thread(int n) {
    pthread_t t;
    struct work w = {n, 0};
    if (pthread_create(&t, NULL, worker, &w) != 0) return -1;
    void* back = NULL;
    pthread_join(t, &back);
    return back == &w ? w.sum : -2;
}

static jmp_buf jump;

static void deep(int x) {
    if (x > 3) longjmp(jump, x * 10);
    deep(x + 1);
}

int bridge_jump(int x) {
    int r = setjmp(jump);
    if (r) return r + 1;
    deep(x);
    return -1;
}

// errno lives in the host's tls
int bridge_errno(void) {
    errno = 0;
    if (close(-1) == 0) return -1;
    return errno;
}

unsigned long bridge_hwcap(void) { return getauxval(AT_HWCAP); }

static int counter;

static void bump(void) { counter += 1; }

int bridge_once(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, bump);
    pthread_once(&once, bump);
    return counter;
}

// malloc and the str* functions
int bridge_strings(const char* a, const char* b) {
    size_t n = strlen(a) + strlen(b) + 1;
    char* s = malloc(n);
    strcpy(s, a);
    strcat(s, b);
    int r = (int)strlen(s) * 100 + (strstr(s, b) - s);
    free(s);
    return r;
}

static int constructed;

__attribute__((constructor)) static void construct(void) { constructed = 42; }

int bridge_constructed(void) { return constructed; }

static char sink[256];
static int sink_length;

static int sink_write(void* cookie, const char* data, int n) {
    (void)cookie;
    for (int i = 0; i < n && sink_length < 255; ++i) sink[sink_length++] = data[i];
    sink[sink_length] = 0;
    return n;
}

// host stdio calling a guest function it only has as a plain pointer
int bridge_funopen(char* out, int size) {
    sink_length = 0;
    FILE* f = funopen(NULL, NULL, sink_write, NULL, NULL);
    if (!f) return -1;
    fprintf(f, "cookie %d %s %.1f", 42, "jar", 2.5);
    fclose(f);
    snprintf(out, (size_t)size, "%s", sink);
    return sink_length;
}

// guest code calling host code it only has as plain pointers, opensl's vtables
int bridge_opensl(void) {
    SLObjectItf engine = NULL, mix = NULL;
    SLEngineItf itf = NULL;
    int r = 0;
    if (slCreateEngine(&engine, 0, NULL, 0, NULL, NULL) != SL_RESULT_SUCCESS) return 1;
    if ((*engine)->Realize(engine, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) r |= 2;
    if ((*engine)->GetInterface(engine, SL_IID_ENGINE, &itf) != SL_RESULT_SUCCESS) {
        r |= 4;
    } else if ((*itf)->CreateOutputMix(itf, &mix, 0, NULL, NULL) != SL_RESULT_SUCCESS) {
        r |= 8;
    } else {
        if ((*mix)->Realize(mix, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) r |= 16;
        (*mix)->Destroy(mix);
    }
    (*engine)->Destroy(engine);
    return r;
}

// 16-byte struct back in two registers
lldiv_t bridge_lldiv(long long a, long long b) { return lldiv(a, b); }
