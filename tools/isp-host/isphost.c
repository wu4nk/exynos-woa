/*
 * isphost.c - portable core of the native ISP-library host (isphost.h, notes/s10-isp.md Stage 4 option A).
 * OS-service semantics follow the GPL kernel the libraries were built for (ExtremeKernel = Samsung's
 * drivers/media/platform/exynos/fimc-is2/interface/fimc-is-interface-library.c; line numbers below refer to it).
 */
#include "isphost.h"
#include "isp_compat.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_THREADS 16
#define N_TASKQ 6                 /* TASK_OTF .. TASK_RTA, priority 11..16 (interface-library.h:49) */
#define TASK_PRIO_BASE 10
#define JIFFY_US 4000             /* CONFIG_HZ=250 in the kernel the libraries run under (ExtremeKernel .config) */
#define TRAMP_AREA 0x20000u       /* after ISP_SPAN: code block, thread table, trampolines */
#define TRAMP_SLOT 48

/* ---- machine code: tramp.S assembled (check_tramp.sh compares these words with the assembler's) ---- */
static const u32 code_block[] = {
    /* 0x00 isp_enter */
    0xa9be7bfd, 0x910003fd, 0xf9000bf2, 0xaa0003f0, 0xaa0103f1, 0xa9400620, 0xa9410e22, 0xa9421624,
    0xa9431e26, 0xd63f0200, 0xf9400bf2, 0xa8c27bfd, 0xd65f03c0,
    /* 0x34 isp_tramp template (literals at 0x44 / 0x4c / 0x54, pad 0x5c) */
    0x58000090, 0x580000af, 0x580000d1, 0xd61f0220, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x64 isp_common (literal: thread table at 0xc8) */
    0xa9be7bfd, 0x910003fd, 0xf9000bf2, 0x580002c9, 0xb940012a, 0x9100212b, 0x910003ec, 0x3400018a,
    0xa940396d, 0xeb0d019f, 0x540000c3, 0xeb0e019f, 0x54000082, 0xf9400972, 0xf9000d6f, 0x14000005,
    0x9100816b, 0x5100054a, 0x17fffff5, 0xd43e01a0, 0xd63f0200, 0xf9400bf2, 0xa8c27bfd, 0xd65f03c0,
    0xd503201f, 0, 0,
};
#define OFF_ENTER 0x00
#define OFF_TRAMP 0x34
#define OFF_COMMON 0x64
#define OFF_COMMON_TABLE 0xc8
#define CODE_BYTES ((u32)sizeof(code_block))

struct thr_entry { u64 lo, hi, x18, ctx; };
struct thr_table { u32 count, pad; struct thr_entry e[MAX_THREADS]; };

struct job {
    u64 fn, args[8], ret;
    int done, owned;
    struct job *next;
};

struct queue {
    void *m, *c;
    struct job *head, *tail;
    int busy;
    const char *name;
};

struct timer {                     /* stands in for struct timer_list */
    u64 due_us, func, data;
    int active;
    struct timer *next;
};

struct dma {
    void *kva;
    u32 dva, size;
    struct dma *next;
};

struct sema { void *m, *c; s32 count; };

struct isp_host {
    unsigned char *base;           /* LIB_START lands here */
    u64 delta, nreloc;
    unsigned char *code;           /* code block + tramps */
    struct thr_table *thr;
    u32 ntramp;
    void *lock;                    /* tramps, dma list, irq table, stats */
    u64 (*enter)(u64 fn, const u64 *args);
    u64 os_tab[100], rta_tab[100], vra_tab[18];
    struct queue ctl, irq, task[N_TASKQ];
    void *pend_m, *pend_c;
    int pending;                   /* queued + running task jobs */
    void *tm_m, *tm_c;
    struct timer *timers;
    struct dma *dmas;
    struct isp_irq irqs[32];
    int nirq;
    u64 gen_irq_func[4];
    void *svc_lock, *svc_lock_rta;
    char ver_ddk[64], ver_rta[64];
    void (*log_hook)(void *, const char *);
    void *log_arg;
    struct isp_stats st;
    int started_threads;
};

static isp_host *G;                /* one instance per process: the OS callbacks have no context argument */
static ISP_TLS struct thr_entry *tls_entry;
static ISP_TLS int tls_in_irq;
int isp_trace;                     /* 1: log every OS call */
int isp_diag_emu_usec, isp_diag_in_irq0, isp_diag_random0;  /* DIAGNOSIS: emulator models */             /* DIAGNOSIS: get_usec as front_init.py modelled it (notes/s10-isp.md) */

/* ---------------- small helpers ---------------- */
static void logf_(const char *fmt, ...)
{
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    plat_log(b);
}

const char *isp_img(isp_host *h, u64 a, char *buf, size_t n)
{
    u64 b = (u64)(uintptr_t)h->base;
    if (a >= b + ISP_RTA_OFF && a < b + ISP_RTA_OFF + 0x800000)
        snprintf(buf, n, "rta+0x%llx", (unsigned long long)(a - b - ISP_RTA_OFF));
    else if (a >= b && a < b + ISP_SPAN)
        snprintf(buf, n, "ddk+0x%llx", (unsigned long long)(a - b));
    else
        snprintf(buf, n, "0x%llx", (unsigned long long)a);
    return buf;
}

static void lib_line(const char *who, const char *s)
{
    char b[1024];
    size_t n;
    snprintf(b, sizeof(b), "%s%s", who, s);
    n = strlen(b);
    while (n && (b[n - 1] == '\n' || b[n - 1] == '\r'))
        b[--n] = 0;
    if (G->log_hook)
        G->log_hook(G->log_arg, b);
    plat_log(b);
}

/* vsnprintf stand-in for the libraries' printf-style calls. The libraries pass AAPCS64 (Linux) variadic arguments:
   integers in x1..x7, floating point in v0..v7 - a Windows-compiled variadic function would read them differently, so
   the host side is declared non-variadic and formats integer conversions itself; %f/%e/%g print as "<fp>". */
static void fmt_lib(char *out, size_t n, const char *f, const u64 *a, int na)
{
    size_t o = 0;
    int k = 0;
    while (*f && o + 1 < n) {
        if (*f != '%') { out[o++] = *f++; continue; }
        const char *s = f++;
        if (*f == '%') { out[o++] = '%'; f++; continue; }
        while (*f && strchr("-+ #0", *f)) f++;
        while (*f >= '0' && *f <= '9') f++;
        if (*f == '.') { f++; while (*f >= '0' && *f <= '9') f++; }
        int lng = 0;
        while (*f && strchr("hlzjt", *f)) { if (*f != 'h') lng = 1; f++; }
        char conv = *f ? *f++ : 0;
        char spec[32];
        size_t sl = (size_t)(f - s);
        if (sl >= sizeof(spec) - 4) sl = sizeof(spec) - 4;
        u64 v = k < na ? a[k] : 0;
        char tmp[256];
        tmp[0] = 0;
        if (conv == 'f' || conv == 'e' || conv == 'g' || conv == 'F' || conv == 'E' || conv == 'G') {
            snprintf(tmp, sizeof(tmp), "<fp>");               /* in v-registers, not passed here */
        } else {
            k++;
            if (conv == 's') {
                memcpy(spec, s, sl); spec[sl] = 0;
                /* drop length modifiers, keep flags / width / precision */
                snprintf(tmp, sizeof(tmp), "%s", v ? (const char *)(uintptr_t)v : "(null)");
            } else if (conv == 'p') {
                snprintf(tmp, sizeof(tmp), "0x%llx", (unsigned long long)v);
            } else if (conv == 'c') {
                tmp[0] = (char)v; tmp[1] = 0;
            } else if (strchr("diuxXo", conv)) {
                /* rebuild the spec with "ll" so a 64-bit value is printed; 32-bit values are sign/zero extended */
                size_t j = 0;
                for (const char *p = s; p < f - 1 && j < sizeof(spec) - 4; p++)
                    if (!strchr("hlzjt", *p)) spec[j++] = *p;
                spec[j++] = 'l'; spec[j++] = 'l'; spec[j++] = conv; spec[j] = 0;
                if (conv == 'd' || conv == 'i')
                    snprintf(tmp, sizeof(tmp), spec, lng ? (long long)v : (long long)(int32_t)v);
                else
                    snprintf(tmp, sizeof(tmp), spec, lng ? (unsigned long long)v : (unsigned long long)(u32)v);
            } else {
                snprintf(tmp, sizeof(tmp), "<%%%c?>", conv ? conv : '?');
            }
        }
        for (const char *p = tmp; *p && o + 1 < n; p++) out[o++] = *p;
    }
    out[o] = 0;
}

/* ---------------- queues and threads ---------------- */
static void q_init(struct queue *q, const char *name)
{
    q->m = plat_mutex_new();
    q->c = plat_cond_new();
    q->name = name;
}

static void q_push(struct queue *q, struct job *j)
{
    plat_mutex_lock(q->m);
    j->next = NULL;
    if (q->tail) q->tail->next = j; else q->head = j;
    q->tail = j;
    plat_cond_broadcast(q->c);
    plat_mutex_unlock(q->m);
}

static void register_thread(isp_host *h)
{
    u64 lo, hi;
    plat_thread_stack(&lo, &hi);
    plat_mutex_lock(h->lock);
    u32 i = h->thr->count;
    if (i >= MAX_THREADS) plat_fatal("isphost: thread table full");
    struct thr_entry *e = &h->thr->e[i];
    e->lo = lo;
    e->hi = hi;
    e->x18 = plat_thread_x18();
    e->ctx = 0;
    ISP_STORE32_REL(&h->thr->count, i + 1);
    tls_entry = e;
    h->started_threads++;
    plat_mutex_unlock(h->lock);
    logf_("isphost: thread %u stack 0x%llx..0x%llx x18 0x%llx", i, (unsigned long long)lo, (unsigned long long)hi,
          (unsigned long long)e->x18);
}

u64 isp_cur_ctx(void)
{
    return tls_entry ? tls_entry->ctx : 0;
}

static u64 run(isp_host *h, u64 fn, const u64 *args)
{
    u64 a[8] = {0};
    if (args) memcpy(a, args, sizeof(a));
    return h->enter(fn, a);
}

struct thr_arg { isp_host *h; struct queue *q; int kind; };

static void worker(void *p)
{
    struct thr_arg *ta = p;
    isp_host *h = ta->h;
    struct queue *q = ta->q;
    int kind = ta->kind;      /* 0 control, 1 irq, 2 task */
    free(ta);
    register_thread(h);
    if (kind == 1) tls_in_irq = 1;
    for (;;) {
        plat_mutex_lock(q->m);
        while (!q->head) plat_cond_wait(q->c, q->m);
        struct job *j = q->head;
        q->head = j->next;
        if (!q->head) q->tail = NULL;
        q->busy = 1;
        plat_mutex_unlock(q->m);

        u64 r;
        if (kind == 1) {           /* irq: fn = chain << 32 | id */
            u32 chain = (u32)(j->fn >> 32), id = (u32)j->fn;
            struct isp_irq ir = {0};
            int found = 0;
            plat_mutex_lock(h->lock);
            for (int i = 0; i < h->nirq; i++)
                if (h->irqs[i].chain == chain && h->irqs[i].id == id && h->irqs[i].valid) { ir = h->irqs[i]; found = 1; }
            plat_mutex_unlock(h->lock);
            if (found) {
                u64 a[8] = {ir.id, ir.ctx};
                r = run(h, ir.handler, a);   /* hwip_handler(u32 id, void *context) (interface-ischain.h:26) */
                ISP_ADD64(&h->st.irqs_run, 1);
            } else {
                logf_("isphost: interrupt chain %u id %u has no handler", chain, id);
                r = (u64)-1;
            }
        } else {
            r = run(h, j->fn, j->args);
            if (kind == 2) ISP_ADD64(&h->st.tasks_run, 1);
        }

        plat_mutex_lock(q->m);
        q->busy = 0;
        j->ret = r;
        j->done = 1;
        int owned = j->owned;
        plat_cond_broadcast(q->c);
        plat_mutex_unlock(q->m);
        if (owned) free(j);
        if (kind == 2) {
            plat_mutex_lock(h->pend_m);
            if (--h->pending == 0) plat_cond_broadcast(h->pend_c);
            plat_mutex_unlock(h->pend_m);
        }
    }
}

static u64 q_sync(struct queue *q, struct job *j)
{
    j->done = 0;
    j->owned = 0;
    q_push(q, j);
    plat_mutex_lock(q->m);
    while (!j->done) plat_cond_wait(q->c, q->m);
    plat_mutex_unlock(q->m);
    return j->ret;
}

u64 isp_call(isp_host *h, u64 fn, int n, const u64 *args)
{
    struct job j;
    memset(&j, 0, sizeof(j));
    j.fn = fn;
    for (int i = 0; i < n && i < 8; i++) j.args[i] = args[i];
    return q_sync(&h->ctl, &j);
}

void isp_irq_post(isp_host *h, u32 chain, u32 id)
{
    struct job *j = calloc(1, sizeof(*j));
    j->fn = (u64)chain << 32 | id;
    j->owned = 1;
    q_push(&h->irq, j);
}

u64 isp_irq_sync(isp_host *h, u32 chain, u32 id)
{
    struct job j;
    memset(&j, 0, sizeof(j));
    j.fn = (u64)chain << 32 | id;
    return q_sync(&h->irq, &j);
}

const struct isp_irq *isp_irqs(isp_host *h, int *n)
{
    *n = h->nirq;
    return h->irqs;
}

void isp_drain(isp_host *h)
{
    plat_mutex_lock(h->pend_m);
    while (h->pending) plat_cond_wait(h->pend_c, h->pend_m);
    plat_mutex_unlock(h->pend_m);
}

static void timer_thread(void *p)
{
    isp_host *h = p;
    register_thread(h);
    plat_mutex_lock(h->tm_m);
    for (;;) {
        struct timer *due = NULL;
        u64 now = plat_usec(), wait = 1000000;
        for (struct timer *t = h->timers; t; t = t->next) {
            if (!t->active) continue;
            if (t->due_us <= now) { due = t; break; }
            if (t->due_us - now < wait) wait = t->due_us - now;
        }
        if (!due) { plat_cond_wait_us(h->tm_c, h->tm_m, wait); continue; }
        due->active = 0;                       /* a timer_list fires once */
        u64 fn = due->func, data = due->data;
        plat_mutex_unlock(h->tm_m);
        u64 a[8] = {data};
        run(h, fn, a);                         /* void (*func)(ulong) (fimc_is_timer_create :1002) */
        ISP_ADD64(&h->st.timers_fired, 1);
        plat_mutex_lock(h->tm_m);
    }
}

/* ---------------- OS services: DDK table (set_os_system_funcs :1973) ---------------- */
#define A8 u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7
#define UNUSED8 (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7
#define P(x) ((void *)(uintptr_t)(x))

static void trace(const char *name, u64 a0, u64 a1, u64 a2, u64 ret)
{
    ISP_ADD64(&G->st.traced_calls, 1);
    if (isp_trace) logf_("  os %-22s %llx %llx %llx -> %llx", name, (unsigned long long)a0, (unsigned long long)a1,
                         (unsigned long long)a2, (unsigned long long)ret);
}

static u64 os_log_console(A8)            /* int fimc_is_log_write_console(char *str) :35 */
{
    UNUSED8;
    lib_line(isp_cur_ctx() == 1 ? "[RTA] " : "", (const char *)P(a0));
    return 0;
}

static u64 os_log_write(A8)              /* int fimc_is_log_write(const char *str, ...) :41 */
{
    char b[1024];
    u64 a[7] = {a1, a2, a3, a4, a5, a6, a7};
    (void)a0;
    fmt_lib(b, sizeof(b), (const char *)P(a0), a, 7);
    lib_line(isp_cur_ctx() == 1 ? "[RTA] " : "", b);
    return 0;
}

static u64 os_assert(A8)                 /* void fimc_is_assert(void) :809 = BUG_ON(1) */
{
    UNUSED8;
    plat_fatal("isphost: library ASSERT (kernel BUG_ON) - stopping");
}

static u64 os_sema_init(A8)              /* int fimc_is_sema_init(void **sema, int sema_count) :817 */
{
    UNUSED8;
    void **pp = P(a0);
    struct sema *s = *pp;
    if (!s) {
        s = calloc(1, sizeof(*s));
        s->m = plat_mutex_new();
        s->c = plat_cond_new();
        *pp = s;
    }
    s->count = (s32)a1;
    trace("sema_init", a0, a1, 0, 0);
    return 0;
}

static u64 os_sema_finish(A8)
{
    UNUSED8;
    struct sema *s = P(a0);
    if (!s) return (u64)-1;
    plat_mutex_free(s->m);
    free(s);
    return 0;
}

static u64 os_sema_up(A8)
{
    UNUSED8;
    struct sema *s = P(a0);
    if (!s) return (u64)-1;
    plat_mutex_lock(s->m);
    s->count++;
    plat_cond_broadcast(s->c);
    plat_mutex_unlock(s->m);
    return 0;
}

static u64 os_sema_down(A8)
{
    UNUSED8;
    struct sema *s = P(a0);
    if (!s) return (u64)-1;
    plat_mutex_lock(s->m);
    while (s->count <= 0) plat_cond_wait(s->c, s->m);
    s->count--;
    plat_mutex_unlock(s->m);
    return 0;
}

static u64 os_mutex_init(A8)             /* int fimc_is_mutex_init(void **mutex) :873 */
{
    UNUSED8;
    void **pp = P(a0);
    if (!*pp) *pp = plat_mutex_new();
    return 0;
}

static u64 os_mutex_finish(A8) { UNUSED8; if (a0) plat_mutex_free(P(a0)); return 0; }
static u64 os_mutex_lock(A8) { UNUSED8; plat_mutex_lock(P(a0)); return 0; }
static u64 os_mutex_trylock(A8) { UNUSED8; return plat_mutex_trylock(P(a0)) ? 1 : 0; }   /* bool :922 */
static u64 os_mutex_unlock(A8) { UNUSED8; plat_mutex_unlock(P(a0)); return 0; }

/* timers: struct timer_list semantics of :1002-:1096 (expires in jiffies, relative) */
static u64 os_timer_create(A8)           /* (void **timer, ulong expires, void (*func)(ulong), ulong data) */
{
    UNUSED8;
    void **pp = P(a0);
    plat_mutex_lock(G->tm_m);
    struct timer *t = *pp;
    if (!t) {
        t = calloc(1, sizeof(*t));
        t->next = G->timers;
        G->timers = t;
        *pp = t;
    }
    t->active = 0;
    t->due_us = plat_usec() + a1 * JIFFY_US;
    t->func = a2;
    t->data = a3;
    plat_mutex_unlock(G->tm_m);
    trace("timer_create", a0, a1, a2, 0);
    return 0;
}

static u64 os_timer_delete(A8)
{
    UNUSED8;
    struct timer *t = P(a0);
    if (!t) return (u64)-1;
    plat_mutex_lock(G->tm_m);
    for (struct timer **pp = &G->timers; *pp; pp = &(*pp)->next)
        if (*pp == t) { *pp = t->next; break; }
    plat_mutex_unlock(G->tm_m);
    free(t);
    return 0;
}

static u64 os_timer_reset(A8)            /* mod_timer(timer, jiffies + expires) */
{
    UNUSED8;
    struct timer *t = P(a0);
    if (!t) return (u64)-1;
    plat_mutex_lock(G->tm_m);
    t->due_us = plat_usec() + a1 * JIFFY_US;
    t->active = 1;
    plat_cond_broadcast(G->tm_c);
    plat_mutex_unlock(G->tm_m);
    return 0;
}

static u64 os_timer_query(A8) { UNUSED8; return 0; }        /* "TODO: will be IMPLEMENTED" :1060 */

static u64 os_timer_enable(A8)           /* add_timer: the expiry set at create time */
{
    UNUSED8;
    struct timer *t = P(a0);
    if (!t) return (u64)-1;
    plat_mutex_lock(G->tm_m);
    t->active = 1;
    plat_cond_broadcast(G->tm_c);
    plat_mutex_unlock(G->tm_m);
    return 0;
}

static u64 os_timer_disable(A8)          /* if (data) { del_timer; data = NULL } :1078 */
{
    UNUSED8;
    struct timer *t = P(a0);
    if (!t) return (u64)-1;
    plat_mutex_lock(G->tm_m);
    if (t->data) { t->active = 0; t->data = 0; }
    plat_mutex_unlock(G->tm_m);
    return 0;
}

static u64 os_irq_register(A8)           /* int fimc_is_register_interrupt(struct hwip_intr_handler info) :1112 -
                                            40-byte struct by value = passed by reference (AAPCS64) */
{
    UNUSED8;
    const unsigned char *p = P(a0);
    struct isp_irq ir;
    memcpy(&ir.valid, p + 0, 4);
    memcpy(&ir.priority, p + 4, 4);
    memcpy(&ir.id, p + 8, 4);
    memcpy(&ir.ctx, p + 16, 8);
    memcpy(&ir.handler, p + 24, 8);
    memcpy(&ir.chain, p + 32, 4);
    ir.valid = 1;
    plat_mutex_lock(G->lock);
    int i;
    for (i = 0; i < G->nirq; i++)
        if (G->irqs[i].chain == ir.chain && G->irqs[i].id == ir.id) break;
    if (i < 32) {
        G->irqs[i] = ir;
        if (i == G->nirq) G->nirq++;
    }
    plat_mutex_unlock(G->lock);
    char b[48];
    logf_("[@][DRV]Register interrupt:ID(%u), handler(%s) chain %u", ir.id, isp_img(G, ir.handler, b, sizeof(b)),
          ir.chain);
    return 0;
}

static u64 os_irq_unregister(A8)         /* (u32 intr_id, u32 chain_id) */
{
    UNUSED8;
    plat_mutex_lock(G->lock);
    for (int i = 0; i < G->nirq; i++)
        if (G->irqs[i].id == (u32)a0 && G->irqs[i].chain == (u32)a1) G->irqs[i].valid = 0;
    plat_mutex_unlock(G->lock);
    return 0;
}

static u64 os_irq_nop(A8) { UNUSED8; return 0; }            /* enable / disable / clear: "TODO" :1303-:1320 */

static u64 os_svc_lock(A8) { UNUSED8; plat_mutex_lock(G->svc_lock); return 0; }
static u64 os_svc_unlock(A8) { UNUSED8; plat_mutex_unlock(G->svc_lock); return 0; }
static u64 os_svc_lock_rta(A8) { UNUSED8; plat_mutex_lock(G->svc_lock_rta); return 0; }
static u64 os_svc_unlock_rta(A8) { UNUSED8; plat_mutex_unlock(G->svc_lock_rta); return 0; }
static u64 os_random(A8) { UNUSED8; return isp_diag_random0 ? 0 : plat_random(); }

static u64 os_add_task(A8)               /* bool fimc_is_add_task(int priority, void *func, void *param) :1600 */
{
    UNUSED8;
    int prio = (s32)a0;
    int idx = prio - TASK_PRIO_BASE - 1;
    if (idx < 0 || idx >= N_TASKQ || !a1 || !a2) {   /* GPL panics on a bad priority, BUG on NULL func / param */
        logf_("isphost: add_task bad priority %d / func %llx / param %llx", prio, (unsigned long long)a1,
              (unsigned long long)a2);
        return 0;
    }
    struct job *j = calloc(1, sizeof(*j));
    j->fn = a1;
    j->args[0] = a2;
    j->owned = 1;
    plat_mutex_lock(G->pend_m);
    G->pending++;
    plat_mutex_unlock(G->pend_m);
    q_push(&G->task[idx], j);
    if (isp_trace) logf_("  os add_task prio %d func %llx param %llx", prio, (unsigned long long)a1,
                         (unsigned long long)a2);
    return 1;
}

static u64 os_get_usec(A8)               /* int fimc_is_get_usec(uint64_t *time) :1467 */
{
    UNUSED8;
    if (isp_diag_emu_usec) {            /* DIAGNOSIS ONLY: the emulator's (wrong) model - value returned, *time untouched */
        static u64 t = 1000000;
        return t += 10;
    }
    if (a0) *(u64 *)P(a0) = plat_usec();
    return 0;
}

static struct dma *dma_find_kva(u64 kva)
{
    for (struct dma *d = G->dmas; d; d = d->next)
        if (kva >= (u64)(uintptr_t)d->kva && kva < (u64)(uintptr_t)d->kva + d->size) return d;
    return NULL;
}

static u64 os_dma_dva(A8)                /* int (ulong kva, u32 *dva) :644 */
{
    UNUSED8;
    plat_mutex_lock(G->lock);
    struct dma *d = dma_find_kva(a0);
    u64 r = (u64)-22;
    if (d) { *(u32 *)P(a1) = d->dva + (u32)(a0 - (u64)(uintptr_t)d->kva); r = 0; }
    plat_mutex_unlock(G->lock);
    trace("dma_dva", a0, a1, 0, r);
    return r;
}

static u64 os_dma_kva(A8)                /* int (u32 dva, ulong *kva) :672 */
{
    UNUSED8;
    u32 dva = (u32)a0;
    u64 r = (u64)-22;
    plat_mutex_lock(G->lock);
    for (struct dma *d = G->dmas; d; d = d->next)
        if (dva >= d->dva && dva < d->dva + d->size) {
            *(u64 *)P(a1) = (u64)(uintptr_t)d->kva + (dva - d->dva);
            r = 0;
            break;
        }
    plat_mutex_unlock(G->lock);
    trace("dma_kva", a0, a1, 0, r);
    return r;
}

static u64 os_dma_inv(A8) { UNUSED8; plat_dma_inv(P(a0), (u32)a1); return 0; }   /* (ulong kva, u32 size) */

int isp_dma_alloc(isp_host *h, u32 size, void **kva, u32 *dva)
{
    if (plat_dma_alloc(size, kva, dva)) return -1;
    struct dma *d = calloc(1, sizeof(*d));
    d->kva = *kva;
    d->dva = *dva;
    d->size = size;
    plat_mutex_lock(h->lock);
    d->next = h->dmas;
    h->dmas = d;
    plat_mutex_unlock(h->lock);
    return 0;
}

static u64 os_dma_alloc(A8)              /* void *(u32 size) :442 */
{
    UNUSED8;
    void *k;
    u32 dva;
    u64 r = isp_dma_alloc(G, (u32)a0, &k, &dva) ? 0 : (u64)(uintptr_t)k;
    trace("dma_alloc", a0, 0, 0, r);
    return r;
}

static u64 os_dma_free(A8)               /* void (void *kva) */
{
    UNUSED8;
    struct dma *d = NULL;
    plat_mutex_lock(G->lock);
    for (struct dma **pp = &G->dmas; *pp; pp = &(*pp)->next)
        if ((u64)(uintptr_t)(*pp)->kva == a0) { d = *pp; *pp = d->next; break; }
    plat_mutex_unlock(G->lock);
    if (d) { plat_dma_free(d->kva); free(d); }
    return 0;
}

static u64 os_sleep(A8) { UNUSED8; plat_sleep_us((u64)(u32)a0 * 1000); return 0; }       /* msleep(u32 msec) */
static u64 os_usleep(A8) { UNUSED8; plat_sleep_us(a0); return 0; }
static u64 os_udelay(A8)                 /* udelay: busy wait */
{
    UNUSED8;
    u64 end = plat_usec() + a0;
    while (plat_usec() < end) { }
    return 0;
}

static u64 os_spin_init(A8)              /* int fimc_is_spin_lock_init(void **slock) :1354 */
{
    UNUSED8;
    void **pp = P(a0);
    if (!*pp) *pp = plat_mutex_new();
    return 0;
}

/* spin_lock_irq*: interrupts are a host thread here, so mutual exclusion = a mutex; flags = 0 */
static u64 os_spin_finish(A8) { UNUSED8; if (a0) plat_mutex_free(P(a0)); return 0; }
static u64 os_spin_lock(A8) { UNUSED8; plat_mutex_lock(P(a0)); return 0; }
static u64 os_spin_unlock(A8) { UNUSED8; plat_mutex_unlock(P(a0)); return 0; }

static u64 os_get_reg_addr(A8)           /* ulong get_reg_addr(u32 id) :1495 */
{
    UNUSED8;
    u64 r = (u64)(uintptr_t)plat_reg_addr((u32)a0);
    logf_("get_reg_addr: [ID:%u](0x%llx)", (u32)a0, (unsigned long long)r);
    return r;
}

static u64 os_in_irq(A8) { UNUSED8; return tls_in_irq && !isp_diag_in_irq0 ? 1 : 0; }   /* bool fimc_is_lib_in_irq(void) :1810 */

static u64 os_flush_task(A8)             /* void fimc_is_lib_flush_task_handler(int priority) :1779 */
{
    UNUSED8;
    int idx = (s32)a0 - TASK_PRIO_BASE - 1;
    if (idx < 0 || idx >= N_TASKQ) return 0;
    struct queue *q = &G->task[idx];
    plat_mutex_lock(q->m);
    while (q->head || q->busy) plat_cond_wait(q->c, q->m);
    plat_mutex_unlock(q->m);
    return 0;
}

static u64 os_fd_data(A8)                /* fimc_is_get_fd_data / hybrid (:1818 / :1881): face data for FDAE/FDAF;
                                            no VRA running here - leave the caller's buffer as it is */
{
    UNUSED8;
    trace("get_fd_data", a0, a1, a2, 0);
    return 0;
}

static u64 os_bin_version(A8)            /* void fimc_is_get_binary_version(char **buf, type, hint) :1960 */
{
    UNUSED8;
    static char empty[1];
    char *v = empty;
    if ((u32)a1 == 0)                   /* IS_BIN_LIBRARY: hint 0 DDK, 1 RTA; the part after ']' */
        v = (u32)a2 == 0 ? G->ver_ddk : G->ver_rta;
    if ((u32)a1 == 0) { char *p = strrchr(v, ']'); if (p) v = p + 1; }
    *(char **)P(a0) = v;
    trace("get_binary_version", a0, a1, a2, 0);
    return 0;
}

static u64 os_event_write(A8)            /* int fimc_is_event_write(const char *str, ...) :90 */
{
    char b[1024];
    u64 a[7] = {a1, a2, a3, a4, a5, a6, a7};
    fmt_lib(b, sizeof(b), (const char *)P(a0), a, 7);
    lib_line("[event] ", b);
    return 0;
}

/* ---------------- RTA-only entries (set_os_system_funcs_for_rta :2056) ---------------- */
static u64 os_fwrite(A8)                 /* int fimc_is_fwrite(const char *pfname, void *pdata, int size) */
{
    UNUSED8;
    logf_("isphost: RTA fwrite(%s, %d bytes) ignored", (const char *)P(a0), (s32)a2);
    return 0;
}

static u64 os_gen_irq_register(A8)       /* (struct general_intr_handler {int irq; int id; int (*f)(void)}) by
                                            value, 16 bytes = x0 (irq | id << 32), x1 (intr_func) */
{
    UNUSED8;
    u32 id = (u32)(a0 >> 32);
    if (id < 4) G->gen_irq_func[id] = a1;
    char b[48];
    logf_("isphost: RTA general interrupt %u -> %s (PDAF / PDP: not wired)", id, isp_img(G, a1, b, sizeof(b)));
    return 0;
}

static u64 os_gen_irq_unregister(A8) { UNUSED8; return 0; }

/* ---------------- VRA os funcs (interface-vra.h:70) ---------------- */
static u64 vra_nop(A8) { UNUSED8; trace("vra", isp_cur_ctx(), a0, a1, 0); return 0; }
static u64 vra_dram(A8)                  /* int set_dram_adr_from_core_to_vdma(ulong src_adr, u32 *target_adr) */
{
    return os_dma_dva(a0, a1, a2, a3, a4, a5, a6, a7);
}
static u64 vra_log(A8)
{
    char b[1024];
    u64 a[7] = {a1, a2, a3, a4, a5, a6, a7};
    fmt_lib(b, sizeof(b), (const char *)P(a0), a, 7);
    lib_line("[VRA] ", b);
    return 0;
}

/* ---------------- loading ---------------- */
static u64 tramp(isp_host *h, isp_host_fn fn, u64 ctx)
{
    plat_mutex_lock(h->lock);
    u32 off = 0x1000;                                     /* page 0: code block + thread table */
    unsigned char *s = h->code + off + TRAMP_SLOT * h->ntramp;
    if (off + TRAMP_SLOT * (h->ntramp + 1) > TRAMP_AREA) { plat_mutex_unlock(h->lock); return 0; }
    h->ntramp++;
    memcpy(s, (const unsigned char *)code_block + OFF_TRAMP, TRAMP_SLOT);
    u64 tgt = (u64)(uintptr_t)fn, com = (u64)(uintptr_t)(h->code + OFF_COMMON);
    memcpy(s + 16, &tgt, 8);
    memcpy(s + 24, &ctx, 8);
    memcpy(s + 32, &com, 8);
    plat_flush_icache(s, TRAMP_SLOT);
    plat_mutex_unlock(h->lock);
    return (u64)(uintptr_t)s;
}

u64 isp_host_func(isp_host *h, isp_host_fn fn, u64 ctx)
{
    return tramp(h, fn, ctx);
}

static u64 relocate(isp_host *h, u32 off, size_t size)
{
    u64 n = 0, other = 0;
    for (size_t i = 0; i + 8 <= size; i += 8) {
        u64 w;
        memcpy(&w, h->base + off + i, 8);
        if (w >= ISP_LIB_START && w < ISP_LIB_START + ISP_SPAN) {
            w += h->delta;
            memcpy(h->base + off + i, &w, 8);
            n++;
        } else if ((w >> 39) == 0x1ffffffull) {         /* other kernel-half values: reported, never touched */
            if (other++ < 5) logf_("isphost: kernel-half word 0x%llx at file +0x%zx (not in the image span, left)",
                                   (unsigned long long)w, i);
        }
    }
    if (other) logf_("isphost: %llu kernel-half words outside the span (left as they are)", (unsigned long long)other);
    return n;
}

static void version(char *dst, const unsigned char *img, size_t size)
{
    /* GPL carve_binary_version: LIBRARY_VER_LEN 44 bytes at size - (44 + IS_SIGNATURE_LEN 528)
       (fimc-is-binary.h:136-142, USE_BINARY_PADDING_DATA_ADDED on this phone) */
    dst[0] = 0;
    if (size < 44 + 528) return;
    memcpy(dst, img + size - 44 - 528, 44);
    dst[44] = 0;
}

isp_host *isp_load(const void *ddk, size_t ddk_size, const void *rta, size_t rta_size)
{
    if (G) { plat_log("isphost: already loaded"); return NULL; }
    if (ddk_size > ISP_RTA_OFF || (rta && rta_size > ISP_SPAN - ISP_RTA_OFF)) {
        plat_log("isphost: image sizes do not fit the Android layout");
        return NULL;
    }
    isp_host *h = calloc(1, sizeof(*h));
    G = h;
    /* 2 MiB alignment: LIB_START is 128 KiB aligned on Android; any alignment the image assumes up to that holds */
    plat_mark("isp_load: exec alloc");
    h->base = plat_exec_alloc(ISP_SPAN + TRAMP_AREA, 0x200000);
    plat_mark(h->base ? "isp_load: exec alloc ok" : "isp_load: exec alloc FAILED");
    if (!h->base) { plat_log("isphost: no memory for the image span"); return NULL; }
    h->delta = (u64)(uintptr_t)h->base - ISP_LIB_START;
    memcpy(h->base, ddk, ddk_size);
    if (rta) memcpy(h->base + ISP_RTA_OFF, rta, rta_size);
    h->nreloc = relocate(h, 0, ddk_size);
    if (rta) h->nreloc += relocate(h, ISP_RTA_OFF, rta_size);
    version(h->ver_ddk, ddk, ddk_size);
    if (rta) version(h->ver_rta, rta, rta_size);
    plat_mark("isp_load: copied + relocated");
    /* only the two files hold code: flush their ranges (0.3.2; was the whole 112 MiB span) */
    plat_flush_icache(h->base, ddk_size);
    if (rta) plat_flush_icache(h->base + ISP_RTA_OFF, rta_size);
    plat_mark("isp_load: icache flushed");

    h->code = h->base + ISP_SPAN;
    memcpy(h->code, code_block, CODE_BYTES);
    u32 tab_off = ((CODE_BYTES + 63) & ~63u);
    h->thr = (struct thr_table *)(h->code + tab_off);
    u64 tab = (u64)(uintptr_t)h->thr;
    memcpy(h->code + OFF_COMMON_TABLE, &tab, 8);
    plat_flush_icache(h->code, 0x1000);
    h->enter = (u64 (*)(u64, const u64 *))(uintptr_t)(h->code + OFF_ENTER);
    h->lock = plat_mutex_new();
    h->svc_lock = plat_mutex_new();
    h->svc_lock_rta = plat_mutex_new();
    h->pend_m = plat_mutex_new();
    h->pend_c = plat_cond_new();
    h->tm_m = plat_mutex_new();
    h->tm_c = plat_cond_new();

    logf_("isphost: image at %p (delta 0x%llx), %llu pointers relocated; DDK '%s' RTA '%s'", (void *)h->base,
          (unsigned long long)h->delta, (unsigned long long)h->nreloc, h->ver_ddk, h->ver_rta);

    /* threads: control, interrupt, timer, 6 task workers (GPL fimc_is_init_ddk_thread :1696) */
    static const char *tn[N_TASKQ] = {"lib_0_otf", "lib_1_af", "lib_2_isp_dma", "lib_3_3aa_dma", "lib_4_aa",
                                      "lib_5_rta"};
    q_init(&h->ctl, "control");
    q_init(&h->irq, "irq");
    for (int i = 0; i < N_TASKQ; i++) q_init(&h->task[i], tn[i]);
    const size_t STK = 1u << 20;     /* kernel code: 16 KiB kernel stacks on Android; 1 MiB committed here */
    struct { struct queue *q; int kind; } th[2 + N_TASKQ];
    th[0].q = &h->ctl; th[0].kind = 0;
    th[1].q = &h->irq; th[1].kind = 1;
    for (int i = 0; i < N_TASKQ; i++) { th[2 + i].q = &h->task[i]; th[2 + i].kind = 2; }
    for (int i = 0; i < 2 + N_TASKQ; i++) {
        struct thr_arg *ta = malloc(sizeof(*ta));
        ta->h = h; ta->q = th[i].q; ta->kind = th[i].kind;
        if (plat_thread_start(worker, ta, STK)) { plat_log("isphost: thread start failed"); return NULL; }
    }
    if (plat_thread_start(timer_thread, h, STK)) { plat_log("isphost: thread start failed"); return NULL; }
    plat_mark("isp_load: threads starting");
    for (;;) {                       /* every blob thread must be in the table before blob code runs */
        plat_mutex_lock(h->lock);
        int n = h->started_threads;
        plat_mutex_unlock(h->lock);
        if (n == 3 + N_TASKQ) break;
        plat_sleep_us(1000);
    }

    /* the tables */
#define H(f) ((isp_host_fn)(f))
    plat_mark("isp_load: threads registered");
    u64 *t = h->os_tab;
    t[0] = tramp(h, H(os_log_console), 0);
    t[3] = tramp(h, H(os_assert), 0);
    t[4] = tramp(h, H(os_sema_init), 0);
    t[5] = tramp(h, H(os_sema_finish), 0);
    t[6] = tramp(h, H(os_sema_up), 0);
    t[7] = tramp(h, H(os_sema_down), 0);
    t[8] = tramp(h, H(os_mutex_init), 0);
    t[9] = tramp(h, H(os_mutex_finish), 0);
    t[10] = tramp(h, H(os_mutex_lock), 0);
    t[11] = tramp(h, H(os_mutex_trylock), 0);
    t[12] = tramp(h, H(os_mutex_unlock), 0);
    t[13] = tramp(h, H(os_timer_create), 0);
    t[14] = tramp(h, H(os_timer_delete), 0);
    t[15] = tramp(h, H(os_timer_reset), 0);
    t[16] = tramp(h, H(os_timer_query), 0);
    t[17] = tramp(h, H(os_timer_enable), 0);
    t[18] = tramp(h, H(os_timer_disable), 0);
    t[19] = tramp(h, H(os_irq_register), 0);
    t[20] = tramp(h, H(os_irq_unregister), 0);
    t[21] = t[22] = t[23] = tramp(h, H(os_irq_nop), 0);
    t[24] = tramp(h, H(os_svc_lock), 0);
    t[25] = tramp(h, H(os_svc_unlock), 0);
    t[26] = tramp(h, H(os_random), 0);
    t[27] = tramp(h, H(os_add_task), 0);
    t[28] = tramp(h, H(os_get_usec), 0);
    t[29] = tramp(h, H(os_log_write), 0);
    t[30] = t[60] = t[65] = tramp(h, H(os_dma_dva), 0);
    t[31] = t[61] = t[66] = tramp(h, H(os_dma_kva), 0);
    t[32] = tramp(h, H(os_sleep), 0);
    t[33] = t[62] = t[67] = tramp(h, H(os_dma_inv), 0);
    t[34] = t[63] = t[68] = tramp(h, H(os_dma_alloc), 0);
    t[35] = t[64] = t[69] = tramp(h, H(os_dma_free), 0);
    t[36] = tramp(h, H(os_spin_init), 0);
    t[37] = tramp(h, H(os_spin_finish), 0);
    t[38] = t[40] = t[42] = tramp(h, H(os_spin_lock), 0);
    t[39] = t[41] = t[43] = tramp(h, H(os_spin_unlock), 0);
    t[46] = tramp(h, H(os_get_reg_addr), 0);
    t[47] = tramp(h, H(os_in_irq), 0);
    t[48] = tramp(h, H(os_flush_task), 0);
    t[49] = t[50] = tramp(h, H(os_fd_data), 0);
    t[91] = tramp(h, H(os_bin_version), 0);
    t[99] = tramp(h, H(os_event_write), 0);

    u64 *r = h->rta_tab;                 /* ctx 1 = RTA (log prefix) */
    r[0] = tramp(h, H(os_log_write), 1);
    r[1] = tramp(h, H(os_log_console), 1);
    r[2] = tramp(h, H(os_assert), 1);
    r[9] = tramp(h, H(os_fwrite), 1);
    r[30] = tramp(h, H(os_gen_irq_register), 1);
    r[31] = tramp(h, H(os_gen_irq_unregister), 1);
    r[40] = tramp(h, H(os_add_task), 1);  /* fimc_is_add_task_rta: priority must be TASK_PRIORITY_6TH (:1610) */
    r[50] = t[13]; r[51] = t[14]; r[52] = t[15]; r[53] = t[16]; r[54] = t[17]; r[55] = t[18];
    r[56] = tramp(h, H(os_udelay), 1);
    r[57] = tramp(h, H(os_usleep), 1);
    r[58] = t[32];
    r[60] = t[4]; r[61] = t[5]; r[62] = t[6]; r[63] = t[7];
    r[80] = tramp(h, H(os_svc_lock_rta), 1);
    r[81] = tramp(h, H(os_svc_unlock_rta), 1);
    r[90] = t[28];
    r[91] = t[91];

    u64 *v = h->vra_tab;                 /* ctx 100 + index for the trace */
    for (int i = 0; i < 6; i++) v[i] = tramp(h, H(vra_nop), 100 + i);
    v[2] = tramp(h, H(vra_dram), 102);
    v[6] = tramp(h, H(vra_log), 106);
    v[7] = v[6];
    v[8] = t[36]; v[9] = t[37]; v[10] = t[38]; v[11] = t[39]; v[12] = t[40]; v[13] = t[41]; v[14] = t[42];
    v[15] = t[43];
    v[16] = t[3];
    v[17] = t[47];
#undef H
    return h;
}

int isp_start(isp_host *h)
{
    u64 b = (u64)(uintptr_t)h->base;
    plat_mark("isp_start: DDK start_up");
    u64 r = ISP_CALL(h, b + ISP_DDK_OFF, (u64)(uintptr_t)h->os_tab);
    logf_("isphost: DDK start_up -> 0x%llx", (unsigned long long)r);
    if ((u32)r) return -1;
    r = ISP_CALL(h, b + ISP_VRA_OFF, (u64)(uintptr_t)h->vra_tab);
    logf_("isphost: VRA set_os_funcs -> 0x%llx", (unsigned long long)r);
    plat_mark("isp_start: RTA start_up");
    r = ISP_CALL(h, b + ISP_RTA_OFF, 0, (u64)(uintptr_t)h->rta_tab);
    logf_("isphost: RTA start_up -> 0x%llx", (unsigned long long)r);
    return (u32)r ? -1 : 0;
}

u64 isp_base(isp_host *h) { return (u64)(uintptr_t)h->base; }
u64 isp_reloc_count(isp_host *h) { return h->nreloc; }

void isp_set_log_hook(isp_host *h, void (*hook)(void *, const char *), void *arg)
{
    h->log_hook = hook;
    h->log_arg = arg;
}

void isp_get_stats(isp_host *h, struct isp_stats *s) { *s = h->st; }
