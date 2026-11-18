/*
 * test_front.c - the front-camera run of tools/isp-re/front_init.py, natively: Samsung's libraries executed by
 * isphost, no hardware: on AArch64 Linux (qemu-aarch64 on the build machine, or Android on the phone) with
 * trapped register windows (mmio_trap.c, every access logged), on Windows ARM64 with plain ones (mmio_plain.c),
 * both backed by this phone's live register dumps. Same order, same sensor answers, same parameter / shot structs
 * as front_init.py.
 *
 *   test_front <bin dir> <dump dir> <out dir> <sensor_itf_layout.txt> [front calibration image]
 *   env FRAMES (default 3), ISP (default 1), TRACE=1 (every OS call)
 * Writes <out>/host.log (every line), <out>/interface-calls.txt, <out>/regs-3AA0.txt / regs-ISPLP.txt (the windows'
 * final contents, "offset value"), <out>/mmio-<step>.txt (every register access), <out>/timing.txt.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#include <malloc.h>
#define mkdir(p, m) _mkdir(p)
#define aligned_alloc(a, n) _aligned_malloc((n), (a))
#ifdef ISPHOST_DRIVER
/* inside S10eIsp's Wudfhost (drivers/S10eIsp): the run is a thread of the driver host; leaving must not end the
   process - exit() ends this thread only, and main is isp_front_main(), called by S10eIsp.c */
#include <windows.h>
#define exit(c) ExitThread((DWORD)(c))
#define main isp_front_main
int isp_front_main(int argc, char **argv);
#endif
#else
#include <sys/stat.h>
#endif

#include "isphost.h"
#include "mmio_trap.h"
#include "plat_posix.h"

extern int isp_trace, isp_diag_emu_usec, isp_diag_in_irq0, isp_diag_random0;
static isp_host *H;
static const char *OUTDIR;
static FILE *itf_log;

static void *readfile(const char *dir, const char *name, size_t *n)
{
    char p[1024];
    snprintf(p, sizeof(p), "%s%s%s", dir, name ? "/" : "", name ? name : "");
    FILE *f = fopen(p, "rb");
    if (!f) { perror(p); exit(1); }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *b = malloc((size_t)sz + 1);
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) { perror(p); exit(1); }
    fclose(f);
    *n = (size_t)sz;
    return b;
}

static void *zalloc(size_t n)
{
    void *p = aligned_alloc(4096, (n + 4095) & ~(size_t)4095);
    memset(p, 0, (n + 4095) & ~(size_t)4095);
    return p;
}

static void w32(u64 a, u32 v) { if (a) memcpy((void *)(uintptr_t)a, &v, 4); }
static u32 r32(u64 a) { u32 v; memcpy(&v, (void *)(uintptr_t)a, 4); return v; }
static u64 r64(u64 a) { u64 v; memcpy(&v, (void *)(uintptr_t)a, 8); return v; }

/* the blob's return address: isp_common's frame record (x29 chain, -fno-omit-frame-pointer) holds the blob's x30 */
#if defined(_MSC_VER) && !defined(__clang__)
#define BLOB_LR() 0ull                       /* MSVC (S10eIsp build): no frame-address builtin - logs show 0x0 */
#else
#define BLOB_LR() (((u64 *)((u64 *)__builtin_frame_address(0))[0])[1])
#endif

/* ---------------- sensor interface (GPL fimc-is-interface-sensor.c) ---------------- */
#define MAXOPS 160
static char opname[MAXOPS][64];
static int nops;

struct vc_buf_info { s32 stat_type, sensor_mode; u32 width, height, element_size; };  /* interface-sensor.h:149 */

/* Android's answers on this phone (isfw-msg-android.txt WRAP_GetProperties, 30 fps preview mode) */
static u64 sensor_op(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7)
{
    u64 lr = BLOB_LR();
    int k = (int)isp_cur_ctx();
    const char *n = k >= 0 && k < nops ? opname[k] : "?";
    const char *op = strchr(n, '.') ? strchr(n, '.') + 1 : n;
    u64 r = 0;
    (void)a0; (void)a7;
#define IS(s) (strcmp(op, s) == 0)
    if (IS("get_module_id")) w32(a1, 38);                       /* SENSOR_NAME_S5K3J1 */
    else if (IS("get_module_position")) w32(a1, 1);             /* FRONT */
    else if (IS("get_calibrated_size")) { w32(a1, 3968); w32(a2, 2736); }
    else if (IS("get_bayer_order")) w32(a1, 0);
    else if (IS("get_min_exposure_time")) r = 23;
    else if (IS("get_max_exposure_time")) r = 33257;
    else if (IS("get_min_analog_gain")) r = 1000;
    else if (IS("get_max_analog_gain")) r = 16000;
    else if (IS("get_min_digital_gain")) r = 1000;
    else if (IS("get_max_digital_gain")) r = 128000;
    else if (IS("get_initial_exposure_gain_of_sensor")) {
        w32(a1, 2360); w32(a2, 1000); w32(a3, 1000); w32(a4, 2360); w32(a5, 1000); w32(a6, 1000);
    } else if (IS("get_sensor_frame_timing")) { w32(a1, 807720000); w32(a2, 9384); w32(a3, 2864); w32(a4, 0); }
    else if (IS("get_sensor_cur_size")) { w32(a1, 3648); w32(a2, 2736); }
    else if (IS("get_sensor_max_fps")) w32(a1, 30);
    else if (IS("get_hdr_ratio_ctl_by_again")) w32(a1, 0);
    else if (IS("get_sensor_use_dgain")) w32(a1, 1);
    else if (IS("get_sensor_initial_aperture")) w32(a1, 190);
    else if (IS("is_actuator_available")) r = 1;                /* Android: "Ext AF is selected!" */
    else if (IS("get_vc_dma_buf_info") && a2) {
        /* Android's preview "[IspObject_InitPdafLib] requestType" lines; other types INVALID (-1), GPL :2732 */
        struct vc_buf_info v = {-1, -1, 0, 0, 0};
        if ((u32)a1 == 1) v = (struct vc_buf_info){500, 102, 1756, 1, 1};
        if ((u32)a1 == 3) v = (struct vc_buf_info){501, 102, 39424, 1, 1};
        memcpy((void *)(uintptr_t)a2, &v, sizeof(v));
    }
#undef IS
    char b[48];
    fprintf(itf_log, "%s from %s x1..3 0x%llx 0x%llx 0x%llx -> %llu\n", n, isp_img(H, lr, b, sizeof(b)),
            (unsigned long long)a1, (unsigned long long)a2, (unsigned long long)a3, (unsigned long long)r);
    return r;
}

static u64 callback_op(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7)
{
    (void)a4; (void)a5; (void)a6; (void)a7;
    char b[48];
    fprintf(itf_log, "CB %s from %s x0..3 0x%llx 0x%llx 0x%llx 0x%llx\n", isp_cur_ctx() ? "io" : "camera",
            isp_img(H, BLOB_LR(), b, sizeof(b)), (unsigned long long)a0, (unsigned long long)a1,
            (unsigned long long)a2, (unsigned long long)a3);
    return 0;
}

static u64 sensor_itf(const char *layout)
{
    unsigned char *a = zalloc(5704);
    w32((u64)(uintptr_t)a, 0xFEDCBA98);
    FILE *f = fopen(layout, "r");
    if (!f) { perror(layout); exit(1); }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        int off;
        char name[64];
        if (line[0] == '#' || sscanf(line, "%d %63s", &off, name) != 2) continue;
        if (nops >= MAXOPS) break;
        strcpy(opname[nops], name);
        u64 t = isp_host_func(H, sensor_op, (u64)nops);
        memcpy(a + off, &t, 8);
        nops++;
    }
    fclose(f);
    return (u64)(uintptr_t)a;
}

/* ---------------- fake register windows + the reset model ---------------- */
static volatile u32 *W3, *WI;

/* S10eIsp 0.4 "hw3aa" (drivers/S10eIsp, run.flg): the REAL 3AA0 registers (UMDF user-mode mapping of 0x17190000) for
   the init steps only - chain_create / set_system_config / object_create; on Android only chain_create touched the 3AA
   (notes/s10-isp.md). The run stops after them: no tune set, set_param, shot or interrupt reaches the hardware. */
static volatile u32 *HW3;
void isp_front_set_hw3aa(volatile void *va) { HW3 = (volatile u32 *)va; }

static volatile u32 *window(const char *dump_dir, const char *name)
{
    volatile u32 *w = zalloc(0x10000);
    char p[1024];
    snprintf(p, sizeof(p), "%s/%s.txt", dump_dir, name);
    FILE *f = fopen(p, "r");
    if (!f) { perror(p); exit(1); }
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        unsigned off, v;
        if (line[0] != '#' && sscanf(line, "%x 0x%x", &off, &v) == 2 && off < 0x10000) w[off / 4] = v;
    }
    fclose(f);
    return w;
}

static void dump_window(volatile u32 *w, const char *name)
{
    char p[1024];
    snprintf(p, sizeof(p), "%s/regs-%s.txt", OUTDIR, name);
    FILE *f = fopen(p, "w");
    fprintf(f, "# %s final contents after the native run (isphost test_front)\n", name);
    for (unsigned o = 0; o < 0x10000; o += 4) fprintf(f, "%04x 0x%08x\n", o, w[o / 4]);
    fclose(f);
}

static void snap(volatile u32 *w, u32 *dst)
{
    for (unsigned o = 0; o < 0x10000; o += 4) dst[o / 4] = w[o / 4];   /* one 32-bit read each (Android's regdump) */
}

static void write_regs(const u32 *v, const char *name, const char *title)
{
    char p[1024];
    snprintf(p, sizeof(p), "%s/regs-%s.txt", OUTDIR, name);
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "# %s\n", title);
    for (unsigned o = 0; o < 0x10000; o += 4) fprintf(f, "%04x 0x%08x\n", o, v[o / 4]);
    fclose(f);
}

/* ---------------- steps ---------------- */
static FILE *timing;
static int marks_on = 1;      /* plat_mark per step until frame 1 is done (write-through marks cost ms) */
static double trap_cost_us;
static u64 frame_us, frame_acc;
static u64 FN3[20], FNI[20];
enum { CHAIN_CREATE, OBJECT_CREATE, CHAIN_DESTROY, OBJECT_DESTROY, STOP, RECOVERY, SET_PARAM, SET_CTRL, SHOT, GET_META,
       CREATE_TUNE_SET, APPLY_TUNE_SET, DELETE_TUNE_SET, SET_FLASH, SET_AF, LOAD_CAL, GET_CAL, MODE_CHG, UPDATE_CTL,
       SET_SYSTEM_CONFIG };   /* struct lib_interface_func order, GPL interface-ddk.h:206 */

static FILE *mmio_open(const char *label)
{
    char p[1024];
    snprintf(p, sizeof(p), "%s/mmio-%s.txt", OUTDIR, label);
    for (char *c = p + strlen(OUTDIR) + 1; *c; c++) if (*c == ' ') *c = '_';
    FILE *f = fopen(p, "w");
    trap_log_to(f);
    return f;
}

static void mmio_close(FILE *f)
{
    trap_log_to(NULL);
    if (f) fclose(f);
}

static u64 step(const char *label, u64 fn, int n, const u64 *a)
{
    char b[64];
    plat_log("");
    if (marks_on) plat_mark(label);
    FILE *mf = mmio_open(label);
    u64 t0 = plat_usec();
    u64 r = isp_call(H, fn, n, a);
    isp_drain(H);
    mmio_close(mf);
    u64 dt = plat_usec() - t0;
    frame_us += dt;
    snprintf(b, sizeof(b), "== %s -> 0x%x", label, (u32)r);
    plat_log(b);
    fprintf(timing, "%-28s %10.1f ms\n", label, dt / 1000.0);
    return r;
}
#define STEP(label, fn, ...) step((label), (fn), (int)(sizeof((u64[]){__VA_ARGS__}) / sizeof(u64)), (u64[]){__VA_ARGS__})

static u64 fire(const char *label, volatile u32 *w, u32 reg, int bit, u32 chain, u32 id)
{
    char b[96];
    plat_log("");
    if (marks_on) plat_mark(label);
    FILE *mf = mmio_open(label);
    u64 t0 = plat_usec();
    w[reg / 4] = 1u << bit;
    u64 r = isp_irq_sync(H, chain, id);
    isp_drain(H);
    w[reg / 4] = 0;
    mmio_close(mf);
    frame_us += plat_usec() - t0;
    snprintf(b, sizeof(b), "== %s -> 0x%x", label, (u32)r);
    plat_log(b);
    fprintf(timing, "%-28s %10.1f ms\n", label, (plat_usec() - t0) / 1000.0);
    return r;
}

static void put(unsigned char *base, u32 off, int n, const u32 *v) { memcpy(base + off, v, 4u * (u32)n); }
#define PUT(base, off, ...) put((base), (off), (int)(sizeof((u32[]){__VA_ARGS__}) / 4), (u32[]){__VA_ARGS__})

static int finish(FILE *lf);

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: %s <bin dir> <dump dir> <out dir> <sensor_itf_layout.txt> [cal image]\n", argv[0]);
        return 2;
    }
    const char *BIN = argv[1], *DUMP = argv[2], *layout = argv[4], *calp = argc > 5 ? argv[5] : NULL;
    OUTDIR = argv[3];
    plat_mark("front: isp_front_main entered");
    mkdir(OUTDIR, 0755);
    char p[1024];
    snprintf(p, sizeof(p), "%s/host.log", OUTDIR);
    FILE *lf = fopen(p, "w");     /* 0.3.4: no "c" (commit per line) any more - plat_mark covers crashes, and a
                                     disk commit per log line distorted the frame times */
    plat_posix_log_to(lf, getenv("QUIET") != NULL);
    snprintf(p, sizeof(p), "%s/interface-calls.txt", OUTDIR);
    itf_log = fopen(p, "w");
    /* size 4096, not 0: the UCRT's setvbuf takes 2 <= size <= INT_MAX for _IOFBF / _IOLBF and invokes the invalid
       parameter handler otherwise - with MSVC's default handler that fast-fails the process (S10eIsp 0.3 / 0.3.1 / 0.3.2
       front runs died right here; mingw's setvbuf accepted 0) */
    setvbuf(itf_log, NULL, _IOLBF, 4096);
    snprintf(p, sizeof(p), "%s/timing.txt", OUTDIR);
    timing = fopen(p, "w");
    isp_trace = getenv("TRACE") != NULL;
    isp_diag_emu_usec = getenv("DIAG_EMU_USEC") != NULL;     /* diagnosis: the emulator's OS models */
    isp_diag_in_irq0 = getenv("DIAG_IN_IRQ0") != NULL;
    isp_diag_random0 = getenv("DIAG_RANDOM0") != NULL;

    size_t nd, nr;
    plat_mark("front: logs open, reading the libraries");
    void *ddk = readfile(BIN, "fimc_is_lib.bin", &nd), *rta = readfile(BIN, "fimc_is_rta.bin", &nr);
    plat_mark("front: libraries read");
    H = isp_load(ddk, nd, rta, nr);
    if (!H) return 1;
    u64 B = isp_base(H), DDK = B + ISP_DDK_OFF, RTA = B + ISP_RTA_OFF;
#ifndef ISPHOST_DRIVER
    plat_posix_crash_report(B);               /* not in a driver host: its own exception handling stays */
#endif
    if (isp_start(H)) { plat_log("start failed"); return 1; }

    u64 itf = sensor_itf(layout);
    char b[256];
    snprintf(b, sizeof(b), "register_sensor_interface -> 0x%x", (u32)ISP_CALL(H, DDK + 0x40, itf));
    plat_log(b);
    /* the same interface is also registered with the RTA (GPL fimc-is-device-sensor_v2.c:2934, RTA + 0x40) */
    snprintf(b, sizeof(b), "register_sensor_interface (RTA) -> 0x%x", (u32)ISP_CALL(H, RTA + 0x40, itf));
    plat_log(b);
    u64 out = (u64)(uintptr_t)zalloc(8);
    ISP_CALL(H, DDK + 0x80, 1, out);                        /* get_lib_func(LIB_FUNC_3AA, &table) */
    for (int k = 0; k < 20; k++) FN3[k] = r64(r64(out) + 8u * k);
    ISP_CALL(H, DDK + 0x80, 2, out);                        /* LIB_FUNC_ISP */
    for (int k = 0; k < 20; k++) FNI[k] = r64(r64(out) + 8u * k);

    W3 = window(DUMP, "3AA0");
    WI = window(DUMP, "ISPLP");
    /* the libraries get trapped windows (mmio_trap.c): every access logged, performed on W3 / WI */
    void *T3 = trap_window("3AA0", W3, B), *TI = trap_window("ISP0", WI, B);
    {   /* cost of one trapped access on this machine (to subtract from the step times) */
        volatile u32 *probe = trap_window("PROBE", zalloc(0x10000), B);
        u64 t0 = plat_usec();
        u32 sum = 0;
        for (int i = 0; i < 2000; i++) sum += probe[i & 0xff];
        trap_cost_us = (double)(plat_usec() - t0) / 2000.0;
        snprintf(b, sizeof(b), "trapped register access: %.2f us each (2000 reads, sum %u)", trap_cost_us, sum);
        plat_log(b);
    }
    u32 *hw_before = NULL;
#ifndef ISPHOST_DRIVER
    if (getenv("HW3AA_SELFTEST")) HW3 = window(DUMP, "3AA0");   /* build-machine check of the hw3aa path: plain
                                     memory, so the SW reset poll (+0x0c) times out here - real hardware clears it */
#endif
    if (HW3) {
        hw_before = malloc(0x10000);
        plat_mark("hw3aa: reading all 3AA0 registers (before)");   /* a bus hang here leaves this as the last mark */
        snap(HW3, hw_before);
        plat_mark("hw3aa: 3AA0 read (before)");
        write_regs(hw_before, "3AA0-hw-before", "3AA0 0x17190000 REAL registers before chain_create (S10eIsp hw3aa)");
        snprintf(b, sizeof(b), "hw3aa: REAL 3AA0 window %p for the init steps: +0xF0 0x%08x +0xF4 0x%08x", (void *)HW3,
                 hw_before[0xF0 / 4], hw_before[0xF4 / 4]);
        plat_log(b);
        T3 = (void *)HW3;            /* the reset model below keeps polling the fake W3 (window 0): nothing is
                                        written to the hardware but by the libraries */
    }
    plat_posix_set_window(0, T3);                           /* ID_3AA_0 */
    plat_posix_set_window(2, TI);                           /* ID_ISP_0 */
    /* self-clearing registers the DDK polls (assembly, notes/s10-isp.md; = front_init.py done_after): 3AA / ISP
       +0x0c SW reset (write 1, poll until 0), ISP COREX +0x24 -> poll +0x2c (second copy +0x8000) - TEST MODEL */
    trap_done_after(0, 0x0c, 0, 0x0c);
    trap_done_after(1, 0x0c, 1, 0x0c);
    trap_done_after(1, 0x3d24, 1, 0x3d2c);
    trap_done_after(1, 0xbd24, 1, 0xbd2c);
    u64 cb = (u64)(uintptr_t)zalloc(16);                    /* struct lib_callback_func {camera_callback, io_callback} */
    u64 cbs[2] = {isp_host_func(H, callback_op, 0), isp_host_func(H, callback_op, 1)};
    memcpy((void *)(uintptr_t)cb, cbs, 16);

    u64 w3 = (u64)(uintptr_t)T3, wi = (u64)(uintptr_t)TI;
    STEP("3AA chain_create", FN3[CHAIN_CREATE], 0, w3, 0, cb);
    u64 cfg = (u64)(uintptr_t)zalloc(40);
    w32(cfg, 1);
    STEP("3AA set_system_config", FN3[SET_SYSTEM_CONFIG], cfg);
    u64 obj3 = (u64)(uintptr_t)zalloc(8), hw = (u64)(uintptr_t)zalloc(0x2000);
    STEP("3AA object_create", FN3[OBJECT_CREATE], obj3, 38u << 16, hw);   /* moduleId 38, chain 0, OTF */
    snprintf(b, sizeof(b), "3AA object = %s", isp_img(H, r64(obj3), (char[48]){0}, 48));
    plat_log(b);
    STEP("ISP chain_create", FNI[CHAIN_CREATE], 0, wi, 0, cb);
    STEP("ISP set_system_config", FNI[SET_SYSTEM_CONFIG], cfg);
    u64 obji = (u64)(uintptr_t)zalloc(8);
    STEP("ISP object_create", FNI[OBJECT_CREATE], obji, (38u << 16) | (1u << 12), hw);   /* 0x00261000 = M2M */
    snprintf(b, sizeof(b), "ISP object = %s", isp_img(H, r64(obji), (char[48]){0}, 48));
    plat_log(b);
    if (HW3) {
        u32 *after = malloc(0x10000);
        plat_mark("hw3aa: reading all 3AA0 registers (after)");
        snap(HW3, after);
        write_regs(after, "3AA0-hw-after", "3AA0 0x17190000 REAL registers after the init steps (S10eIsp hw3aa)");
        int nch = 0;
        for (unsigned o = 0; o < 0x10000; o += 4)
            if (hw_before[o / 4] != after[o / 4]) {
                snprintf(b, sizeof(b), "hw 3AA0 +0x%04x 0x%08x -> 0x%08x", o, hw_before[o / 4], after[o / 4]);
                plat_log(b);
                nch++;
            }
        snprintf(b, sizeof(b), "hw3aa: %d REAL 3AA0 registers changed by the init steps; stopping here (no tune set, "
                 "set_param, shot or interrupt on the hardware)", nch);
        plat_log(b);
        plat_mark("front: hw3aa init steps done");
        return finish(lf);
    }

    /* setfile: V3 header (GPL hw-control.h:192), IP 0 = 3AA_ISP */
    size_t nsf;
    unsigned char *sf = readfile(BIN, "setfile_3j1.bin", &nsf);
    unsigned char *sfm = zalloc(nsf);
    memcpy(sfm, sf, nsf);
    u32 hdr[7];
    memcpy(hdr, sf, 28);   /* magic, designed, ver[4], rev[4], nscen, nips, ent_off */
    u32 nscen = hdr[4], nips = hdr[5], ent_off = hdr[6];
    u32 num_base = 28 + nips * nscen * 4, tab_base = num_base + nips * 4, count0;
    memcpy(&count0, sf + num_base, 4);
    snprintf(b, sizeof(b), "setfile: magic 0x%x designed 0x%x scenarios %u ips %u, 3AA_ISP tune sets %u", hdr[0],
             hdr[1], nscen, nips, count0);
    plat_log(b);
    unsigned char *ts = zalloc(24);
    int bad = 0;
    for (u32 i = 0; i < count0; i++) {
        u32 off, size;
        memcpy(&off, sf + tab_base + 8 * i, 4);
        memcpy(&size, sf + tab_base + 8 * i + 4, 4);
        /* struct lib_tune_set {u32 index; ulong addr; u32 size; int decrypt_flag} (interface-ddk.h:181) */
        u64 addr = (u64)(uintptr_t)(sfm + ent_off + off);
        memset(ts, 0, 24);
        memcpy(ts, &i, 4);
        memcpy(ts + 8, &addr, 8);
        memcpy(ts + 16, &size, 4);
        u32 one = 1;
        memcpy(ts + 20, &one, 4);
        u32 r = (u32)ISP_CALL(H, FN3[CREATE_TUNE_SET], r64(obj3), 0, (u64)(uintptr_t)ts);
        if (r) { snprintf(b, sizeof(b), "create_tune_set %u -> 0x%x", i, r); plat_log(b); bad++; }
    }
    snprintf(b, sizeof(b), "== 3AA create_tune_set x%u -> 0x%x", count0, bad);
    plat_log(b);
    STEP("3AA apply_tune_set 6", FN3[APPLY_TUNE_SET], r64(obj3), 0, 6);
    unsigned char *cal = zalloc(64 * 1024);
    if (calp) {
        size_t nc;
        void *c = readfile(calp, NULL, &nc);
        memcpy(cal, c, nc < 65536 ? nc : 65536);
        snprintf(b, sizeof(b), "calibration: %s (%zu bytes)", calp, nc);
        plat_log(b);
    }
    STEP("3AA load_cal_data", FN3[LOAD_CAL], r64(obj3), 0, (u64)(uintptr_t)cal);
    u64 ci = (u64)(uintptr_t)zalloc(64);
    STEP("3AA get_cal_data type 2", FN3[GET_CAL], r64(obj3), 0, ci, 2);
    snprintf(b, sizeof(b), "cal_info data[0..3]: %u %u %u %u", r32(ci), r32(ci + 4), r32(ci + 8), r32(ci + 12));
    plat_log(b);

    /* first preview frame: taa_param_set (param_layouts.txt), values = Android's own preview log */
    unsigned char *P = zalloc(1776);
    PUT(P, 0, 33333, 7, 30, 3648, 2736, 1000, 1000, 1000, 1000, 0, 0, 3648, 2736, 3968, 2736);   /* sensor_config */
    PUT(P, 128, 1, 0, 10, 0, 3648, 2736, 352, 264, 2944, 2208);                                 /* otf_input */
    PUT(P, 640, 1, 5, 12, 0, 1, 2944, 2208, 0, 0, 2944, 2208, 0, 11);    /* dma_output_before_bds, packed */
    PUT(P, 768, 1, 5, 12, 0, 1, 2432, 1824, 0, 0, 2432, 1824, 0, 11);    /* dma_output_after_bds */
    void *k1, *k2;
    u32 d1, d2;
    isp_dma_alloc(H, 2944 * 2208 * 2, &k1, &d1);
    isp_dma_alloc(H, 2432 * 1824 * 2, &k2, &d2);
    PUT(P, 1348, d1);
    PUT(P, 1416, d2);
    PUT(P, 1760, 0, 1);                                     /* instance 0, fcount 1 */
    unsigned char *SH = zalloc(36592);
    PUT(SH, 36584, 0x23456789);                             /* SHOT_MAGIC_NUMBER */
    PUT(SH, 148, 7, 30);                                    /* ctl.aa.aeTargetFpsRange */
    PUT(SH, 216, 1);                                        /* captureIntent PREVIEW */
    PUT(SH, 224, 2, 1);                                     /* aa.mode AUTO, sceneMode DISABLED */
    u64 et = 33333000, fd = 33333333;
    memcpy(SH + 648, &et, 8);
    memcpy(SH + 656, &fd, 8);
    snprintf(b, sizeof(b), "DMA buffers (dva): before_bds 0x%x after_bds 0x%x", d1, d2);
    plat_log(b);
    u64 PP = (u64)(uintptr_t)P, SHP = (u64)(uintptr_t)SH;
    STEP("3AA set_param", FN3[SET_PARAM], r64(obj3), PP);
    STEP("3AA set_ctrl F1", FN3[SET_CTRL], r64(obj3), 0, 1, SHP);
    STEP("3AA shot F1", FN3[SHOT], r64(obj3), PP, SHP, 1);

    /* ISP0 M2M from the 3AA's after-BDS output */
    unsigned char *PI = zalloc(1136);
    memcpy(PI, &PP, 8);                                     /* taa_param */
    PUT(PI, 136, 1, 5, 12, 0, 1, 2432, 1824, 0, 2432, 1824, 0, 0, 2432, 1824, 0, 11);   /* dma_input */
    PUT(PI, 392, 1, 2, 8, 0, 2432, 1824, 0, 0, 2432, 1824, 0);                          /* otf_output */
    PUT(PI, 776, d2);
    int nirq;
    const struct isp_irq *irqs = isp_irqs(H, &nirq);
    u32 isp_irq_id = 0xffffffff;
    for (int i = 0; i < nirq; i++)
        if (irqs[i].chain == 2 && irqs[i].id < isp_irq_id) isp_irq_id = irqs[i].id;
    for (int i = 0; i < nirq; i++) {
        snprintf(b, sizeof(b), "interrupt registered: id %u chain %u handler %s", irqs[i].id, irqs[i].chain,
                 isp_img(H, irqs[i].handler, (char[48]){0}, 48));
        plat_log(b);
    }

    int frames = getenv("FRAMES") ? atoi(getenv("FRAMES")) : 3;
    int use_isp = getenv("ISP") ? atoi(getenv("ISP")) : 1;
    u64 sum_us = 0, sum_acc = 0, nsum = 0;
    for (u32 fc = 1; fc <= (u32)frames; fc++) {
        char l[64];
        frame_us = 0;
        frame_acc = trap_count();
        if (fc > 1) {
            PUT(P, 1764, fc);
            snprintf(l, sizeof(l), "3AA set_ctrl F%u", fc);
            STEP(l, FN3[SET_CTRL], r64(obj3), 0, fc, SHP);
            snprintf(l, sizeof(l), "3AA shot F%u", fc);
            STEP(l, FN3[SHOT], r64(obj3), PP, SHP, 1);
        }
        /* 3AA +0x10c: bit 0 V SYNC, 2 CIN ROW, 1 FRAME END (named by the DDK, irq-scan-3aa.txt) */
        snprintf(l, sizeof(l), "F%u irq V SYNC", fc);    fire(l, W3, 0x10c, 0, 0, 0);
        snprintf(l, sizeof(l), "F%u irq CIN ROW", fc);   fire(l, W3, 0x10c, 2, 0, 0);
        snprintf(l, sizeof(l), "F%u irq FRAME END", fc); fire(l, W3, 0x10c, 1, 0, 0);
        if (!use_isp) continue;
        PUT(PI, 1120, 0, fc);
        if (fc == 1) STEP("ISP set_param", FNI[SET_PARAM], r64(obji), (u64)(uintptr_t)PI);
        snprintf(l, sizeof(l), "ISP set_ctrl F%u", fc);
        STEP(l, FNI[SET_CTRL], r64(obji), 0, fc, SHP);
        snprintf(l, sizeof(l), "ISP shot F%u", fc);
        STEP(l, FNI[SHOT], r64(obji), (u64)(uintptr_t)PI, SHP, 1);
        snprintf(l, sizeof(l), "F%u ISP irq V SYNC", fc);   fire(l, WI, 0x10c, 0, 2, isp_irq_id);
        snprintf(l, sizeof(l), "F%u ISP irq FRAME END", fc); fire(l, WI, 0x10c, 1, 2, isp_irq_id);
        if (fc == 1) { plat_mark("frame 1 done - no more per-step marks"); marks_on = 0; }
        u64 acc = trap_count() - frame_acc;
        fprintf(timing, "FRAME %-3u %10.1f ms, %llu register accesses (~%.1f ms of it trapping)\n", fc,
                frame_us / 1000.0, (unsigned long long)acc, acc * trap_cost_us / 1000.0);
        if (fc >= 3) { sum_us += frame_us; sum_acc += acc; nsum++; }
    }
    if (nsum) {
        snprintf(b, sizeof(b), "steady frames 3..%d: %.2f ms per frame wall (3AA + ISP calls, interrupts, tasks), "
                 "%llu register accesses = ~%.2f ms trapping -> ~%.2f ms without the test's trapping", frames,
                 sum_us / 1000.0 / nsum, (unsigned long long)(sum_acc / nsum), sum_acc * trap_cost_us / 1000.0 / nsum,
                 (sum_us - sum_acc * trap_cost_us) / 1000.0 / nsum);
        plat_log(b);
        fprintf(timing, "%s\n", b);
    }
    trap_finish();                /* the reset model stops before the windows are dumped (finish() again: no-op) */
    dump_window(W3, "3AA0");
    dump_window(WI, "ISPLP");
    return finish(lf);
}

static int finish(FILE *lf)
{
    char b[256];
    trap_finish();
    struct isp_stats st;
    isp_get_stats(H, &st);
    snprintf(b, sizeof(b), "done: traced OS calls %llu, tasks %llu, timers %llu, irqs %llu",
             (unsigned long long)st.traced_calls, (unsigned long long)st.tasks_run, (unsigned long long)st.timers_fired,
             (unsigned long long)st.irqs_run);
    plat_log(b);
    plat_mark("front: done");
    fclose(timing);
    fclose(itf_log);
    plat_posix_log_to(NULL, 1);   /* host.log closed: the run's files can be zipped while the host lives on */
    fclose(lf);
    return 0;
}
