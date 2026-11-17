/*
 * x18test.c - behavioural test for Stage 4 option A (notes/s10-isp.md): does Windows ARM64 user mode leave x18 alone
 * while code that is not Windows code holds a foreign value in it? Samsung's ISP libraries use x18 as a scratch
 * register; isphost swaps it at every entry / exit, but between those points the libraries' value lives in x18
 * through timer interrupts, preemption and migration to other cores. If the kernel reloaded x18 (= TEB) on its way
 * back to user mode, the libraries' computations would break at random. This test answers that by observation only.
 *
 * T threads (more than cores, so they are preempted and migrated) each run generated machine code that puts a magic
 * value in x18 and checks it for N iterations without making any call. Result per thread: 0 = x18 kept, otherwise the
 * value found in its place (compared with the thread's TEB).
 *   x18test.exe [threads (16)] [iterations per thread in millions (2000)]
 * Build (Windows ARM64, from the build machine): zig cc -target aarch64-windows-gnu -O2 -o x18test.exe x18test.c
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <sys/mman.h>
#include <time.h>
#endif

/* x0 = iterations, x1 = magic -> 0 if x18 stayed == magic, else the value seen (x18 restored before returning) */
static const uint32_t spin_code[] = {
    0xaa1203e9, /* mov x9, x18 */
    0xaa0103f2, /* mov x18, x1 */
    0xeb01025f, /* 1: cmp x18, x1 */
    0x540000c1, /* b.ne 2f */
    0xf1000400, /* subs x0, x0, #1 */
    0x54ffffa1, /* b.ne 1b */
    0xaa0903f2, /* mov x18, x9 */
    0xd2800000, /* mov x0, #0 */
    0xd65f03c0, /* ret */
    0xaa1203e0, /* 2: mov x0, x18 */
    0xaa0903f2, /* mov x18, x9 */
    0xd65f03c0, /* ret */
};

typedef uint64_t (*spin_fn)(uint64_t iterations, uint64_t magic);
static spin_fn spin;
static uint64_t iters;

struct res { uint64_t magic, seen, teb, ms; int idx; };

static uint64_t now_ms(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
#endif
}

#ifdef _WIN32
static DWORD WINAPI worker(LPVOID p)
#else
static void *worker(void *p)
#endif
{
    struct res *r = p;
#ifdef _WIN32
    r->teb = (uint64_t)(uintptr_t)NtCurrentTeb();
#endif
    uint64_t t0 = now_ms();
    r->seen = spin(iters, r->magic);
    r->ms = now_ms() - t0;
    return 0;
}

int main(int argc, char **argv)
{
    int threads = argc > 1 ? atoi(argv[1]) : 16;
    iters = (argc > 2 ? strtoull(argv[2], NULL, 0) : 2000) * 1000000ull;
    if (threads < 1 || threads > 256) threads = 16;
#ifdef _WIN32
    void *code = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!code) { printf("VirtualAlloc RWX failed: %lu\n", GetLastError()); return 1; }
    memcpy(code, spin_code, sizeof(spin_code));
    FlushInstructionCache(GetCurrentProcess(), code, sizeof(spin_code));
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    printf("x18test v1.0: %d threads x %llu iterations, %lu logical processors\n", threads,
           (unsigned long long)iters, si.dwNumberOfProcessors);
#else
    void *code = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    memcpy(code, spin_code, sizeof(spin_code));
    __builtin___clear_cache((char *)code, (char *)code + sizeof(spin_code));
    printf("x18test v1.0 (POSIX sanity run): %d threads x %llu iterations\n", threads, (unsigned long long)iters);
#endif
    spin = (spin_fn)(uintptr_t)code;
    struct res *r = calloc((size_t)threads, sizeof(*r));
    uint64_t t0 = now_ms();
#ifdef _WIN32
    HANDLE *h = calloc((size_t)threads, sizeof(HANDLE));
    for (int i = 0; i < threads; i++) {
        r[i].idx = i;
        r[i].magic = 0x5a5a000000000000ull | ((uint64_t)i << 32) | 0x1234abcdu;
        h[i] = CreateThread(NULL, 0, worker, &r[i], 0, NULL);
    }
    for (int i = 0; i < threads; i++) { WaitForSingleObject(h[i], INFINITE); CloseHandle(h[i]); }
#else
    pthread_t *h = calloc((size_t)threads, sizeof(pthread_t));
    for (int i = 0; i < threads; i++) {
        r[i].idx = i;
        r[i].magic = 0x5a5a000000000000ull | ((uint64_t)i << 32) | 0x1234abcdu;
        pthread_create(&h[i], NULL, worker, &r[i]);
    }
    for (int i = 0; i < threads; i++) pthread_join(h[i], NULL);
#endif
    int bad = 0;
    for (int i = 0; i < threads; i++) {
        if (r[i].seen) {
            bad++;
            printf("thread %2d: x18 CHANGED after some iterations: found 0x%016llx (magic 0x%016llx, TEB 0x%016llx%s)\n",
                   i, (unsigned long long)r[i].seen, (unsigned long long)r[i].magic, (unsigned long long)r[i].teb,
                   r[i].seen == r[i].teb ? " = the TEB: the kernel reloads x18" : "");
        } else {
            printf("thread %2d: x18 kept for all iterations (%llu ms)\n", i, (unsigned long long)r[i].ms);
        }
    }
    printf("RESULT: %s (%d of %d threads saw x18 change), %llu ms total\n", bad ? "X18 NOT PRESERVED" : "X18 PRESERVED",
           bad, threads, (unsigned long long)(now_ms() - t0));
    return bad ? 1 : 0;
}
