/*
 * mmio_plain.c - the mmio_trap.h API without traps, for platforms where an access fault inside the libraries cannot
 * be used (Windows: the exception dispatcher would run with the libraries' x18). The libraries get the shadow memory
 * itself; nothing is logged; the self-clearing registers are modelled by a polling thread:
 *  - rule (A -> A): when A reads non-zero, write 0 (SW reset bit)
 *  - rule (A -> B): when A reads non-zero, write 0 to B and to A (so the next write to A is seen)
 * The thread can lose the race against a short poll loop (COREX: 1001 reads) - then the DDK logs "sw_reset is
 * time-out" and goes on. TEST ONLY.
 */
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif
#include "mmio_trap.h"

#define MAXW 4
#define MAXRULE 16
static volatile u32 *shadow[MAXW];
static int nwin;
static struct { int wa; u32 oa; int wb; u32 ob; } rule[MAXRULE];
static volatile int nrule;

void trap_log_to(FILE *f)
{
    if (f) fprintf(f, "# register accesses are not logged on this platform (mmio_plain.c)\n");
}

u64 trap_count(void) { return 0; }

#ifdef _WIN32
static DWORD WINAPI model(LPVOID p)
#else
static void *model(void *p)
#endif
{
    (void)p;
    for (;;) {
        for (int r = 0; r < nrule; r++) {
            volatile u32 *a = &shadow[rule[r].wa][rule[r].oa / 4], *b = &shadow[rule[r].wb][rule[r].ob / 4];
            if (*a) { *b = 0; if (a != b) *a = 0; }
        }
#ifdef _WIN32
        YieldProcessor();
#else
        sched_yield();
#endif
    }
#ifndef _WIN32
    return NULL;
#endif
}

void trap_done_after(int wa, u32 oa, int wb, u32 ob)
{
    if (nrule >= MAXRULE) return;
    rule[nrule].wa = wa; rule[nrule].oa = oa; rule[nrule].wb = wb; rule[nrule].ob = ob;
    nrule++;
    if (nrule == 1) {
#ifdef _WIN32
        HANDLE h = CreateThread(NULL, 0, model, NULL, 0, NULL);
        SetThreadPriority(h, THREAD_PRIORITY_TIME_CRITICAL);
        CloseHandle(h);
#else
        pthread_t t;
        pthread_create(&t, NULL, model, NULL);
#endif
    }
}

void *trap_window(const char *name, volatile u32 *sh, u64 lib_base)
{
    (void)name;
    (void)lib_base;
    if (nwin >= MAXW) return NULL;
    shadow[nwin++] = sh;
    return (void *)sh;
}
