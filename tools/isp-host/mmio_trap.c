/*
 * mmio_trap.c - test-only register windows for test_front (AArch64 Linux): the window the libraries get is an
 * inaccessible mapping; every access faults, this SIGSEGV handler decodes the load / store, performs it on a shadow
 * array (filled from the phone's dump), logs it (R/W, window, offset, value, pc) and steps over the instruction.
 * Gives the same per-access log as the emulator (front_init.py mmio-*.txt) and a deterministic model of the
 * self-clearing registers the DDK polls ("done_after": a write to A makes the next read of B return 0 once).
 * DIAGNOSIS TOOL for the build machine; nothing like it runs on the phone.
 */
#define _GNU_SOURCE
#include "mmio_trap.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>

#define MAXW 4
#define MAXRULE 16
static struct { const char *name; unsigned char *base; volatile u32 *shadow; } win[MAXW];
static int nwin;
static struct { int wa; u32 oa; int wb; u32 ob; int armed; } rule[MAXRULE];
static int nrule;
static FILE *logf_;
static u64 lib_base;
static struct sigaction prev;
static volatile int lk;
static u64 count;

static void lock(void) { while (__atomic_exchange_n(&lk, 1, __ATOMIC_ACQUIRE)) { } }
static void unlock(void) { __atomic_store_n(&lk, 0, __ATOMIC_RELEASE); }

void trap_log_to(FILE *f) { lock(); logf_ = f; unlock(); }
u64 trap_count(void) { return count; }

void trap_done_after(int wa, u32 oa, int wb, u32 ob)
{
    if (nrule < MAXRULE) { rule[nrule].wa = wa; rule[nrule].oa = oa; rule[nrule].wb = wb; rule[nrule].ob = ob; nrule++; }
}

static u64 sx(u64 v, int bits) { return bits >= 64 ? v : (v ^ (1ull << (bits - 1))) - (1ull << (bits - 1)); }

/* one access of `size` bytes at window w + off; returns the loaded value */
static u64 access_(int w, u32 off, int size, int store, u64 val, u64 pc)
{
    volatile unsigned char *s = (volatile unsigned char *)win[w].shadow;
    u64 v = 0;
    if (store) {
        for (int i = 0; i < size; i++) s[off + i] = (unsigned char)(val >> (8 * i));
        for (int r = 0; r < nrule; r++)
            if (rule[r].wa == w && rule[r].oa == off) rule[r].armed = 1;
        v = val;
    } else {
        for (int r = 0; r < nrule; r++)
            if (rule[r].wb == w && rule[r].ob == off && rule[r].armed) {
                rule[r].armed = 0;
                for (int i = 0; i < size; i++) s[off + i] = 0;
            }
        for (int i = 0; i < size; i++) v |= (u64)s[off + i] << (8 * i);
    }
    count++;
    if (logf_) {
        u64 o = pc - lib_base;
        if (pc >= lib_base + ISP_RTA_OFF && pc < lib_base + ISP_RTA_OFF + 0x800000)
            fprintf(logf_, "%s %s+0x%04x 0x%08llx rta+0x%llx\n", store ? "W" : "R", win[w].name, off,
                    (unsigned long long)v, (unsigned long long)(o - ISP_RTA_OFF));
        else
            fprintf(logf_, "%s %s+0x%04x 0x%08llx ddk+0x%llx\n", store ? "W" : "R", win[w].name, off,
                    (unsigned long long)v, (unsigned long long)o);
    }
    return v;
}

static int find(u64 a, u32 *off)
{
    for (int i = 0; i < nwin; i++)
        if (a >= (u64)(uintptr_t)win[i].base && a < (u64)(uintptr_t)win[i].base + 0x10000) {
            *off = (u32)(a - (u64)(uintptr_t)win[i].base);
            return i;
        }
    return -1;
}

static void handler(int sig, siginfo_t *si, void *ucv)
{
    ucontext_t *uc = ucv;
    mcontext_t *m = &uc->uc_mcontext;
    u32 off;
    int w = find((u64)(uintptr_t)si->si_addr, &off);
    if (w < 0) goto chain;
    u32 in = *(u32 *)(uintptr_t)m->pc;
    u64 *x = (u64 *)m->regs;
#define XR(r) ((r) == 31 ? 0 : x[r])                 /* data register: 31 = xzr */
#define XN(r) ((r) == 31 ? m->sp : x[r])             /* base register: 31 = sp */
    int rt = in & 31, rn = (in >> 5) & 31;
    lock();
    if ((in & 0x3b000000) == 0x39000000 || (in & 0x3b200000) == 0x38000000 || (in & 0x3b200c00) == 0x38200800) {
        /* LDR/STR (unsigned imm), LDUR/STUR / pre / post index (imm9), register offset; V = 0 only */
        int size = 1 << (in >> 30), opc = (in >> 22) & 3;
        u64 addr, base = XN(rn);
        int wb = 0;
        if ((in & 0x3b000000) == 0x39000000) {
            addr = base + (((in >> 10) & 0xfff) << (in >> 30));
        } else if ((in & 0x3b200000) == 0x38000000) {
            int mode = (in >> 10) & 3;
            u64 imm = sx((in >> 12) & 0x1ff, 9);
            addr = mode == 1 ? base : base + imm;    /* 1 post, 3 pre, 0 unscaled */
            if (mode == 1 || mode == 3) wb = 1, base += imm;
        } else {
            int rm = (in >> 16) & 31, opt = (in >> 13) & 7, S = (in >> 12) & 1;
            u64 r = XR(rm);
            if (opt == 2) r = (u32)r;
            else if (opt == 6) r = sx((u32)r, 32);
            addr = base + (S ? r << (in >> 30) : r);
        }
        if (find(addr, &off) != w) { unlock(); goto chain; }
        if (opc == 0) {
            access_(w, off, size, 1, XR(rt), m->pc);
        } else {
            u64 v = access_(w, off, size, 0, 0, m->pc);
            if (opc == 2) v = sx(v, 8 * size);                       /* to 64 bit */
            else if (opc == 3) v = (u32)sx(v, 8 * size);             /* to 32 bit */
            if (rt != 31) x[rt] = v;
        }
        if (wb) { if (rn == 31) m->sp = base; else x[rn] = base; }
    } else if ((in & 0x3e000000) == 0x28000000 && ((in >> 23) & 3)) {
        /* LDP/STP (post 01, offset 10, pre 11), integer; opc 00 = 32-bit, 10 = 64-bit, 01 = LDPSW */
        int opc = in >> 30, L = (in >> 22) & 1, mode = (in >> 23) & 3, rt2 = (in >> 10) & 31;
        int size = opc == 2 ? 8 : 4;
        u64 imm = sx((in >> 15) & 0x7f, 7) * (u64)size, base = XN(rn);
        u64 addr = mode == 1 ? base : base + imm;
        if (find(addr, &off) != w) { unlock(); goto chain; }
        if (L) {
            u64 v1 = access_(w, off, size, 0, 0, m->pc), v2 = access_(w, off + size, size, 0, 0, m->pc);
            if (opc == 1) { v1 = sx(v1, 32); v2 = sx(v2, 32); }
            if (rt != 31) x[rt] = v1;
            if (rt2 != 31) x[rt2] = v2;
        } else {
            access_(w, off, size, 1, XR(rt), m->pc);
            access_(w, off + size, size, 1, XR(rt2), m->pc);
        }
        if (mode != 2) { base += imm; if (rn == 31) m->sp = base; else x[rn] = base; }
    } else {
        unlock();
        fprintf(stderr, "mmio_trap: undecoded instruction 0x%08x at pc 0x%llx (lib+0x%llx), window %s+0x%x\n", in,
                (unsigned long long)m->pc, (unsigned long long)(m->pc - lib_base), win[w].name, off);
        goto chain;
    }
    unlock();
    m->pc += 4;
    return;
chain:
    if (prev.sa_flags & SA_SIGINFO) prev.sa_sigaction(sig, si, ucv);
    else { signal(sig, SIG_DFL); raise(sig); }
}

void *trap_window(const char *name, volatile u32 *shadow, u64 libbase)
{
    if (nwin >= MAXW) return NULL;
    unsigned char *b = mmap(NULL, 0x10000, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (b == MAP_FAILED) return NULL;
    win[nwin].name = name;
    win[nwin].base = b;
    win[nwin].shadow = shadow;
    nwin++;
    if (nwin == 1) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        lib_base = libbase;
        sa.sa_sigaction = handler;
        sa.sa_flags = SA_SIGINFO | SA_NODEFER;
        sigaction(SIGSEGV, &sa, &prev);
    }
    return b;
}
