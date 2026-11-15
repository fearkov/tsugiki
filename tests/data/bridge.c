// arm64 code that calls into the real libc, built with the ndk
// the device check runs it translated, through every bridge we have

#define _BSD_SOURCE
#include <SLES/OpenSLES.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/stat.h>
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

static __thread int tls_counter = 5;
// a pointer in .tdata, relocated before threads copy it
static __thread const char* tls_name = "main";
static __thread char tls_big[64];

static void* tls_worker(void* arg) {
    int n = (int)(intptr_t)arg;
    for (int i = 0; i < n; ++i) tls_counter++;
    tls_name = "worker";
    tls_big[0] = 'w';
    return (void*)(intptr_t)(tls_counter * 1000 + tls_name[0] + tls_big[0]);
}

// every thread its own copy, starting from .tdata and .tbss
int bridge_tls(void) {
    tls_counter += 1;
    pthread_t t;
    void* back = NULL;
    pthread_create(&t, NULL, tls_worker, (void*)(intptr_t)10);
    pthread_join(t, &back);
    int worker = (int)(intptr_t)back;
    if (worker != 15000 + 'w' + 'w') return -1;
    if (tls_name[0] != 'm' || tls_big[0] != 0) return -2;
    return tls_counter;
}

// struct stat is laid out differently on the two sides, a wrong size trips the canary
long bridge_stat(const char* path) {
    struct stat a, b;
    if (stat(path, &a) != 0) return -1;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -2;
    int r = fstat(fd, &b);
    close(fd);
    if (r != 0) return -3;
    if (a.st_size != b.st_size || a.st_ino != b.st_ino || a.st_mode != b.st_mode || !S_ISREG(a.st_mode)) return -4;
    return (long)a.st_size;
}

// so is epoll_event
int bridge_epoll(void) {
    int p[2];
    if (pipe(p) != 0) return -1;
    int ep = epoll_create1(0);
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events = EPOLLIN;
    ev.data.u64 = 0x1122334455667788ull;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) != 0) return -2;
    if (write(p[1], "x", 1) != 1) return -3;
    struct epoll_event out[4];
    memset(out, 0, sizeof out);
    int n = epoll_wait(ep, out, 4, 1000);
    close(ep);
    close(p[0]);
    close(p[1]);
    if (n != 1) return -4;
    return out[0].data.u64 == 0x1122334455667788ull && (out[0].events & EPOLLIN) ? 1 : -5;
}

static volatile int got_signal;

static void on_usr1(int sig) { got_signal = sig; }

// a guest handler the kernel calls on a host thread
int bridge_signal(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_usr1;
    if (sigaction(SIGUSR1, &sa, NULL) != 0) return -1;
    raise(SIGUSR1);
    struct sigaction back;
    sigaction(SIGUSR1, NULL, &back);
    return got_signal == SIGUSR1 && back.sa_handler == on_usr1 ? 1 : 0;
}
