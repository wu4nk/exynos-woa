/*
 * isphost.h - native user-mode host for Samsung's Exynos 9820 camera ISP libraries (fimc_is_lib.bin = DDK + VRA,
 * fimc_is_rta.bin = RTA), notes/s10-isp.md "Stage 4 option A". The libraries are the phone's own files, loaded at
 * run time from the phone; nothing of them is in this repo.
 *
 * Layers:
 *   isphost.c  - portable core: loader + pointer relocation, x18-swapping trampolines (tramp.S), the GPL OS-service
 *                tables (DDK set_os_system_funcs, RTA set_os_system_funcs_for_rta, VRA os funcs), task workers,
 *                timers, interrupt dispatch, DMA address bookkeeping.
 *   plat_*.c   - platform: executable memory, threads / locks / time, register windows, DMA memory, log output.
 *
 * Every piece of blob code runs on a thread the core created (control, interrupt, timer, 6 task workers), each
 * registered in the trampolines' thread table (stack range + host x18). Callers use isp_call() from any thread.
 * Types: everything crossing into the blob is a fixed-width integer (Linux "ulong" = 64 bit; Windows long = 32).
 */
#ifndef ISPHOST_H
#define ISPHOST_H
#include <stddef.h>
#include <stdint.h>

typedef uint64_t u64;
typedef uint32_t u32;
typedef int32_t s32;

/* Android load addresses (basefind.py / Stage 1; GPL fimc-is-binary.h): DDK file offset 0, RTA at a fixed distance */
#define ISP_LIB_START 0xffffff80f9fe0000ull
#define ISP_VRA_OFF 0x20000u          /* VRA set_os_funcs (GPL VRA_LIB_ADDR) */
#define ISP_DDK_OFF 0xE0000u          /* DDK start_up (GPL DDK_LIB_ADDR) */
#define ISP_RTA_OFF 0x520000u         /* RTA start_up (GPL RTA_LIB_ADDR) */
#define ISP_SPAN 0x7000000u           /* DDK heap ends +0x4c20000, RTA heap rta+0x4b00000..+0x5100000 (notes) */

/* ---------------- platform layer (implemented by plat_posix.c / plat_win.c) ---------------- */
void *plat_exec_alloc(size_t size, size_t align);   /* zeroed, read/write/execute, aligned */
void plat_flush_icache(void *p, size_t n);
void plat_log(const char *line);                     /* one line, no newline */
void plat_fatal(const char *why);                    /* never returns: process (test) or thread (driver) ends */
u64 plat_usec(void);                                 /* monotonic microseconds */
void plat_sleep_us(u64 us);
void *plat_mutex_new(void);
void plat_mutex_lock(void *m);
int plat_mutex_trylock(void *m);                     /* 1 = acquired */
void plat_mutex_unlock(void *m);
void plat_mutex_free(void *m);
void *plat_cond_new(void);
void plat_cond_wait(void *c, void *m);
int plat_cond_wait_us(void *c, void *m, u64 us);     /* 0 = signalled (or spurious), 1 = timed out */
void plat_cond_broadcast(void *c);
int plat_thread_start(void (*fn)(void *), void *arg, size_t stack_size);
void plat_thread_stack(u64 *lo, u64 *hi);            /* current thread's stack range */
u64 plat_thread_x18(void);                           /* this thread's host x18 (Windows: the TEB) */
u32 plat_random(void);
/* register windows: GPL get_reg_addr ids (ID_3AA_0 = 0, ID_3AA_1, ID_ISP_0, ID_ISP_1, ID_TPU_0, ID_TPU_1, ID_DCP) */
void *plat_reg_addr(u32 id);
/* DMA memory: physically contiguous / device-visible; *dva = the 32-bit address the ISP uses */
int plat_dma_alloc(u32 size, void **kva, u32 *dva);
void plat_dma_free(void *kva);
void plat_dma_inv(void *kva, u32 size);              /* CPU cache invalidate before the CPU reads device data */
void plat_dma_clean(void *kva, u32 size);            /* CPU cache clean before the device reads CPU data */

/* ---------------- core API ---------------- */
typedef u64 (*isp_host_fn)(u64, u64, u64, u64, u64, u64, u64, u64);

struct isp_irq {                   /* GPL struct hwip_intr_handler as registered (interface-ischain.h:28) */
    u32 valid, priority, id, chain;
    u64 ctx, handler;
};

struct isp_host;
typedef struct isp_host isp_host;

/* load: ddk / rta = file contents (fimc_is_lib.bin / fimc_is_rta.bin). Returns NULL on error (logged). */
isp_host *isp_load(const void *ddk, size_t ddk_size, const void *rta, size_t rta_size);
u64 isp_base(isp_host *h);                           /* where LIB_START landed */
u64 isp_reloc_count(isp_host *h);
/* a host function the blob may call: returns the blob-callable trampoline address. ctx is readable in fn via
   isp_cur_ctx() (for shared logging stubs). */
u64 isp_host_func(isp_host *h, isp_host_fn fn, u64 ctx);
u64 isp_cur_ctx(void);
/* run blob function fn(args...) on the control thread and wait for its x0 */
u64 isp_call(isp_host *h, u64 fn, int n, const u64 *args);
#define ISP_CALL(h, fn, ...) isp_call((h), (fn), (int)(sizeof((u64[]){__VA_ARGS__}) / sizeof(u64)), (u64[]){__VA_ARGS__})
/* start-up sequence of the GPL loader: DDK start_up(os table), VRA set_os_funcs, RTA start_up(NULL, rta table) */
int isp_start(isp_host *h);
/* interrupts: the platform (or a test) raises (chain, id); the registered handler runs on the interrupt thread.
   isp_irq_sync waits for the handler to return. */
const struct isp_irq *isp_irqs(isp_host *h, int *n);
void isp_irq_post(isp_host *h, u32 chain, u32 id);
u64 isp_irq_sync(isp_host *h, u32 chain, u32 id);
/* wait until every task worker is idle with an empty queue (tasks that queue tasks included) */
void isp_drain(isp_host *h);
/* log hook: every line the libraries print (also sent to plat_log) */
void isp_set_log_hook(isp_host *h, void (*hook)(void *, const char *), void *arg);
/* DMA helpers for callers (same pool the libraries use) */
int isp_dma_alloc(isp_host *h, u32 size, void **kva, u32 *dva);
/* counters for the report */
struct isp_stats { u64 traced_calls, tasks_run, timers_fired, irqs_run; };  /* traced_calls: the subset
                                                                                         of OS calls that pass trace() */
void isp_get_stats(isp_host *h, struct isp_stats *s);
const char *isp_img(isp_host *h, u64 addr, char *buf, size_t n);   /* "ddk+0x..." / "rta+0x..." / "0x..." */

#endif
