/*
 * plat_win.c - isphost platform layer for Windows ARM64 user mode (notes/s10-isp.md Stage 4 option A step 3).
 * This first version is the TEST platform: register windows and DMA are plain memory (no hardware); it proves the
 * host (relocation, x18 trampolines, threads) on real Windows. The hardware version replaces plat_reg_addr and the
 * DMA functions with mappings from a kernel driver.
 * Win32 APIs used are documented ones (learn.microsoft.com): VirtualAlloc, FlushInstructionCache, SRW locks +
 * condition variables, CreateThread (dwStackSize without STACK_SIZE_PARAM_IS_A_RESERVATION = committed size),
 * GetCurrentThreadStackLimits, NtCurrentTeb, QueryPerformanceCounter.
 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "isphost.h"
#include "plat_posix.h"

static SRWLOCK log_m = SRWLOCK_INIT;
static FILE *log_f;
static int log_quiet;

void plat_posix_log_to(FILE *f, int quiet)
{
    log_f = f;
    log_quiet = quiet;
}

void *plat_exec_alloc(size_t size, size_t align)
{
    /* reserve size + align, then commit the aligned part (MEM_COMMIT inside an own reservation) */
    unsigned char *r = VirtualAlloc(NULL, size + align, MEM_RESERVE, PAGE_NOACCESS);
    if (!r) return NULL;
    uintptr_t a = ((uintptr_t)r + align - 1) & ~(uintptr_t)(align - 1);
    void *p = VirtualAlloc((void *)a, size, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    return p;
}

void plat_flush_icache(void *p, size_t n)
{
    FlushInstructionCache(GetCurrentProcess(), p, n);
}

void plat_log(const char *line)
{
    AcquireSRWLockExclusive(&log_m);
    if (log_f) { fputs(line, log_f); fputc('\n', log_f); fflush(log_f); }
    if (!log_quiet) { fputs(line, stdout); fputc('\n', stdout); fflush(stdout); }
    ReleaseSRWLockExclusive(&log_m);
}

/* progress marks written through to disk (S10eIsp 0.3.2: a blue screen left no dump and no event): appended to
   plat_win_set_mark_file()'s file, FILE_FLAG_WRITE_THROUGH + FlushFileBuffers per mark - slow, only for bring-up */
static char mark_path[MAX_PATH];
void plat_win_set_mark_file(const char *path) { lstrcpynA(mark_path, path, MAX_PATH); }
void plat_mark(const char *what)
{
    char b[300];
    DWORD n;
    if (!mark_path[0]) return;
    snprintf(b, sizeof(b), "%10llu us  %s\r\n", (unsigned long long)plat_usec(), what);
    HANDLE f = CreateFileA(mark_path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    WriteFile(f, b, (DWORD)strlen(b), &n, NULL);
    FlushFileBuffers(f);
    CloseHandle(f);
}

/* inside a driver host process (S10eIsp) a library assert must not take the whole Wudfhost down with it: the calling
   thread ends instead (the run is lost, the device stays up) - plat_win_set_thread_fatal(1) */
static int fatal_thread_only;
void plat_win_set_thread_fatal(int on) { fatal_thread_only = on; }
void plat_fatal(const char *why)
{
    plat_log(why);
    if (fatal_thread_only) ExitThread(0xDEAD);
    abort();
}

u64 plat_usec(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (u64)((double)c.QuadPart * 1e6 / (double)f.QuadPart);
}

void plat_sleep_us(u64 us)
{
    if (us >= 1000) { Sleep((DWORD)((us + 999) / 1000)); return; }
    u64 end = plat_usec() + us;
    while (plat_usec() < end) SwitchToThread();
}

void *plat_mutex_new(void)
{
    SRWLOCK *m = malloc(sizeof(*m));
    InitializeSRWLock(m);
    return m;
}
void plat_mutex_lock(void *m) { AcquireSRWLockExclusive(m); }
int plat_mutex_trylock(void *m) { return TryAcquireSRWLockExclusive(m) != 0; }
void plat_mutex_unlock(void *m) { ReleaseSRWLockExclusive(m); }
void plat_mutex_free(void *m) { free(m); }

void *plat_cond_new(void)
{
    CONDITION_VARIABLE *c = malloc(sizeof(*c));
    InitializeConditionVariable(c);
    return c;
}
void plat_cond_wait(void *c, void *m) { SleepConditionVariableSRW(c, m, INFINITE, 0); }
int plat_cond_wait_us(void *c, void *m, u64 us)
{
    DWORD ms = (DWORD)((us + 999) / 1000);
    return SleepConditionVariableSRW(c, m, ms, 0) ? 0 : 1;
}
void plat_cond_broadcast(void *c) { WakeAllConditionVariable(c); }

struct start { void (*fn)(void *); void *arg; };
static DWORD WINAPI tramp_start(LPVOID p)
{
    struct start s = *(struct start *)p;
    free(p);
    s.fn(s.arg);
    return 0;
}

int plat_thread_start(void (*fn)(void *), void *arg, size_t stack_size)
{
    struct start *s = malloc(sizeof(*s));
    s->fn = fn;
    s->arg = arg;
    /* dwStackSize without STACK_SIZE_PARAM_IS_A_RESERVATION = initial COMMITTED size: the libraries are kernel code
       and never probe the stack page by page (no __chkstk), so the whole stack must be committed up front */
    HANDLE h = CreateThread(NULL, stack_size, tramp_start, s, 0, NULL);
    if (!h) return -1;
    CloseHandle(h);
    return 0;
}

void plat_thread_stack(u64 *lo, u64 *hi)
{
    ULONG_PTR l, h;
    GetCurrentThreadStackLimits(&l, &h);
    *lo = l;
    *hi = h;
}

u64 plat_thread_x18(void)
{
    return (u64)(uintptr_t)NtCurrentTeb();     /* Windows ARM64 user mode: x18 = TEB (ARM64 ABI conventions) */
}

u32 plat_random(void)
{
    return (u32)rand() << 16 ^ (u32)rand();
}

/* ---- test register windows / DMA: plain memory ---- */
static void *windows[8];
void plat_posix_set_window(u32 id, void *p) { if (id < 8) windows[id] = p; }
void *plat_reg_addr(u32 id) { return id < 8 ? windows[id] : NULL; }

#define DMA_SIZE 0x08000000u
#define DMA_DVA 0x80000000u
static unsigned char *dma_pool;
static size_t dma_next;
static SRWLOCK dma_m = SRWLOCK_INIT;

static u64 dma_pa = DMA_DVA, dma_bytes = DMA_SIZE;

/* S10eIsp: the real pool of S10eIspDma (contiguous, below 4 GiB, mapped into this process) instead of the fake one:
   dva = its physical address + offset (the ISP's SysMMU is bypassed on Windows) */
void plat_win_dma_pool(void *va, u64 pa, u64 bytes)
{
    AcquireSRWLockExclusive(&dma_m);
    dma_pool = va;
    dma_pa = pa;
    dma_bytes = bytes;
    dma_next = 0;
    ReleaseSRWLockExclusive(&dma_m);
}

int plat_dma_alloc(u32 size, void **kva, u32 *dva)
{
    AcquireSRWLockExclusive(&dma_m);
    if (!dma_pool) dma_pool = VirtualAlloc(NULL, DMA_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    size_t off = (dma_next + 0xfff) & ~(size_t)0xfff;
    if (!dma_pool || off + size > dma_bytes) { ReleaseSRWLockExclusive(&dma_m); return -1; }
    dma_next = off + size;
    *kva = dma_pool + off;
    *dva = (u32)(dma_pa + off);
    ReleaseSRWLockExclusive(&dma_m);
    return 0;
}
void plat_dma_free(void *kva) { (void)kva; }
void plat_dma_inv(void *kva, u32 size) { (void)kva; (void)size; }
void plat_dma_clean(void *kva, u32 size) { (void)kva; (void)size; }

/* ---- crash report (diagnosis). An exception raised while the libraries' x18 is live may not even reach this
   filter (the dispatcher runs with whatever x18 holds) - UNVERIFIED; it is only for host-side faults. ---- */
static u64 crash_base;
static LONG WINAPI crash(EXCEPTION_POINTERS *e)
{
    char b[512];
    CONTEXT *c = e->ContextRecord;
    u64 pc = c->Pc, lr = c->Lr;
#define OFF(v) ((v) >= crash_base && (v) < crash_base + ISP_SPAN ? (unsigned long long)((v) - crash_base) : 0ull)
    snprintf(b, sizeof(b), "CRASH exception 0x%08lx addr 0x%llx pc 0x%llx (lib+0x%llx) lr 0x%llx (lib+0x%llx) sp 0x%llx",
             (unsigned long)e->ExceptionRecord->ExceptionCode,
             (unsigned long long)(e->ExceptionRecord->NumberParameters > 1 ? e->ExceptionRecord->ExceptionInformation[1]
                                                                           : 0),
             (unsigned long long)pc, OFF(pc), (unsigned long long)lr, OFF(lr), (unsigned long long)c->Sp);
    plat_log(b);
    for (int i = 0; i < 29; i += 4) {
        snprintf(b, sizeof(b), "  x%-2d %016llx %016llx %016llx %016llx", i, (unsigned long long)c->X[i],
                 (unsigned long long)c->X[i + 1], (unsigned long long)c->X[i + 2],
                 i + 3 < 29 ? (unsigned long long)c->X[i + 3] : (unsigned long long)c->Fp);
        plat_log(b);
    }
#undef OFF
    return EXCEPTION_CONTINUE_SEARCH;
}

void plat_posix_crash_report(u64 lib_base)
{
    crash_base = lib_base;
    SetUnhandledExceptionFilter(crash);
}
