/*
 * plat_posix.c - isphost platform layer for AArch64 Linux user mode (the build-machine proof under qemu-aarch64,
 * notes/s10-isp.md Stage 4 option A step 2). Register windows and DMA are plain memory here: no hardware.
 */
#define _GNU_SOURCE
#include "isphost.h"
#include "plat_posix.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

static pthread_mutex_t log_m = PTHREAD_MUTEX_INITIALIZER;
static FILE *log_f;
static int log_quiet;

void plat_posix_log_to(FILE *f, int quiet)
{
    log_f = f;
    log_quiet = quiet;
}

void *plat_exec_alloc(size_t size, size_t align)
{
    size_t n = size + align;
    unsigned char *p = mmap(NULL, n, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return NULL;
    uintptr_t a = ((uintptr_t)p + align - 1) & ~(uintptr_t)(align - 1);
    return (void *)a;
}

void plat_flush_icache(void *p, size_t n)
{
    __builtin___clear_cache((char *)p, (char *)p + n);
}

void plat_log(const char *line)
{
    pthread_mutex_lock(&log_m);
    if (log_f) { fputs(line, log_f); fputc('\n', log_f); fflush(log_f); }
    if (!log_quiet) { fputs(line, stdout); fputc('\n', stdout); fflush(stdout); }
    pthread_mutex_unlock(&log_m);
}

u64 plat_usec(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (u64)t.tv_sec * 1000000u + (u64)t.tv_nsec / 1000u;
}

void plat_sleep_us(u64 us)
{
    struct timespec t = {(time_t)(us / 1000000u), (long)(us % 1000000u) * 1000};
    while (nanosleep(&t, &t)) { }
}

void *plat_mutex_new(void)
{
    pthread_mutex_t *m = malloc(sizeof(*m));
    pthread_mutex_init(m, NULL);
    return m;
}
void plat_mutex_lock(void *m) { pthread_mutex_lock(m); }
int plat_mutex_trylock(void *m) { return pthread_mutex_trylock(m) == 0; }
void plat_mutex_unlock(void *m) { pthread_mutex_unlock(m); }
void plat_mutex_free(void *m) { pthread_mutex_destroy(m); free(m); }

void *plat_cond_new(void)
{
    pthread_cond_t *c = malloc(sizeof(*c));
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(c, &a);
    return c;
}
void plat_cond_wait(void *c, void *m) { pthread_cond_wait(c, m); }
int plat_cond_wait_us(void *c, void *m, u64 us)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    u64 ns = (u64)t.tv_nsec + (us % 1000000u) * 1000u;
    t.tv_sec += (time_t)(us / 1000000u + ns / 1000000000u);
    t.tv_nsec = (long)(ns % 1000000000u);
    return pthread_cond_timedwait(c, m, &t) != 0;
}
void plat_cond_broadcast(void *c) { pthread_cond_broadcast(c); }

struct start { void (*fn)(void *); void *arg; };
static void *tramp_start(void *p)
{
    struct start s = *(struct start *)p;
    free(p);
    s.fn(s.arg);
    return NULL;
}

int plat_thread_start(void (*fn)(void *), void *arg, size_t stack_size)
{
    pthread_attr_t a;
    pthread_t t;
    struct start *s = malloc(sizeof(*s));
    s->fn = fn;
    s->arg = arg;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, stack_size);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    int r = pthread_create(&t, &a, tramp_start, s);
    pthread_attr_destroy(&a);
    return r;
}

void plat_thread_stack(u64 *lo, u64 *hi)
{
    pthread_attr_t a;
    void *addr;
    size_t size;
    pthread_getattr_np(pthread_self(), &a);
    pthread_attr_getstack(&a, &addr, &size);
    pthread_attr_destroy(&a);
    *lo = (u64)(uintptr_t)addr;
    *hi = (u64)(uintptr_t)addr + size;
}

u64 plat_thread_x18(void)
{
    u64 v;
    __asm__ volatile("mov %0, x18" : "=r"(v));
    return v;
}

u32 plat_random(void)
{
    return (u32)random();
}

/* ---- register windows: set by the test (plain memory filled from the phone's dumps) ---- */
static void *windows[8];
void plat_posix_set_window(u32 id, void *p) { if (id < 8) windows[id] = p; }
void *plat_reg_addr(u32 id) { return id < 8 ? windows[id] : NULL; }

/* ---- DMA: one 128 MiB anonymous pool, dva = 0x80000000 + offset (fake: nothing reads it but the libraries) ---- */
#define DMA_SIZE 0x08000000u
#define DMA_DVA 0x80000000u
static unsigned char *dma_pool;
static size_t dma_next;
static pthread_mutex_t dma_m = PTHREAD_MUTEX_INITIALIZER;

int plat_dma_alloc(u32 size, void **kva, u32 *dva)
{
    pthread_mutex_lock(&dma_m);
    if (!dma_pool) {
        dma_pool = mmap(NULL, DMA_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (dma_pool == MAP_FAILED) { dma_pool = NULL; pthread_mutex_unlock(&dma_m); return -1; }
    }
    size_t off = (dma_next + 0xfff) & ~(size_t)0xfff;
    if (off + size > DMA_SIZE) { pthread_mutex_unlock(&dma_m); return -1; }
    dma_next = off + size;
    *kva = dma_pool + off;
    *dva = DMA_DVA + (u32)off;
    pthread_mutex_unlock(&dma_m);
    return 0;
}
void plat_dma_free(void *kva) { (void)kva; }            /* bump allocator: freed at exit */
void plat_dma_inv(void *kva, u32 size) { (void)kva; (void)size; }
void plat_dma_clean(void *kva, u32 size) { (void)kva; (void)size; }

/* ---- crash report (diagnosis): signal, fault address, pc / lr / sp and x0..x7, with image offsets ---- */
static u64 crash_base;
static void crash(int sig, siginfo_t *si, void *ucv)
{
    ucontext_t *uc = ucv;
    mcontext_t *m = &uc->uc_mcontext;
    char b[1024];
    u64 pc = m->pc, lr = m->regs[30];
#define OFF(v) ((v) >= crash_base && (v) < crash_base + ISP_SPAN ? (unsigned long long)((v) - crash_base) : 0ull)
    snprintf(b, sizeof(b), "CRASH signal %d addr 0x%llx pc 0x%llx (lib+0x%llx) lr 0x%llx (lib+0x%llx) sp 0x%llx",
             sig, (unsigned long long)(uintptr_t)si->si_addr, (unsigned long long)pc, OFF(pc),
             (unsigned long long)lr, OFF(lr), (unsigned long long)m->sp);
    plat_log(b);
    for (int i = 0; i < 31; i += 4) {
        snprintf(b, sizeof(b), "  x%-2d %016llx %016llx %016llx %016llx", i, (unsigned long long)m->regs[i],
                 (unsigned long long)m->regs[i + 1], (unsigned long long)m->regs[i + 2],
                 i + 3 < 31 ? (unsigned long long)m->regs[i + 3] : 0ull);
        plat_log(b);
    }
#undef OFF
    _exit(128 + sig);
}

void plat_posix_crash_report(u64 lib_base)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    crash_base = lib_base;
    sa.sa_sigaction = crash;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGTRAP, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}
