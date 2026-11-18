/*
 * S10eIsp.c (v0.9) - Galaxy S10 camera ISP, UMDF 2 driver (notes/s10-isp.md, Stage 4 option A, plan item 4).
 *
 * Future role: host Samsung's ISP libraries (tools/isp-host) inside this driver's Wudfhost process, with the ISP's
 * registers mapped into the process by the framework (UmdfRegisterAccessMode = RegisterAccessUsingUserModeMapping,
 * WdfDeviceGetHardwareRegisterMappedAddress) so the libraries' plain pointer accesses reach the hardware.
 *
 * v0.1 is a PROBE. It binds the DSDT node CISP (ACPI\SAMS982C, UEFI v0.2.3+, present only while PMU pd-isppre is ON)
 * and answers, in Wudfhost itself:
 *   1. which memory resources it got, and whether the framework maps them into the process (mapped address);
 *   2. CMU_ISPPRE 3AA0 aclk gate + LHM_AXI_P through both access paths (WDF_READ_REGISTER_ULONG = the framework's call,
 *      and a plain load from the mapped address = what the libraries will do);
 *   3. only if both are on: the 3AA0 ID registers +0xF0 / +0xF4 through the mapped address (S10eCam 0.11 read them
 *      from the kernel on this phone: 0x07010000 / 0x0000CC22, no abort);
 *   4. the process's dynamic-code policy (GetProcessMitigationPolicy) and whether VirtualAlloc(PAGE_EXECUTE_READWRITE)
 *      + running generated code work here (the host generates its trampolines at run time);
 *   5. x18 kept in this process: 8 threads x 100 M checks with a foreign value in x18 (x18test's code).
 *   6. (0.2) the DMA pool of S10eIspDma (\\.\S10eIspDma, IOCTL_S10EISPDMA_MAP) mapped into this process: PA /
 *      size / caching, then the access kinds the libraries use on DMA memory, each under __try: aligned and
 *      unaligned 8-byte stores / loads, memset, DC ZVA (generated code). How Windows ARM64 maps MmWriteCombined /
 *      MmNonCached (Normal-NC or Device) is not documented; Device memory would fault on the unaligned ones.
 *   7. (0.3) FRONT RUN, only when C:\S10Prov\isp\run.flg exists (deleted at once: one run per request): Samsung's
 *      libraries (C:\S10Prov\isp\fimc_is_lib.bin / fimc_is_rta.bin / setfile_3j1.bin, rom1_cal.bin optional) run
 *      INSIDE this process through tools/isp-host (isphost.c + plat_win.c + mmio_plain.c + test_front.c built in) -
 *      the front path of isphost-test.exe, 30 frames - with FAKE register windows (the Android dump, shipped with
 *      the driver) and the REAL DMA pool of S10eIspDma (dva = physical address). The ISP hardware is not touched:
 *      the dvas only land in the fake windows. Output: C:\S10Prov\isp\out (isp-s10.ps1 front grants LocalService
 *      write access there), registry value FrontRun.
 *   8. (0.4) run.flg containing "hw3aa": the same front run, but the 3AA0 window the libraries get for the INIT steps
 *      is the REAL one (the user-mode mapping of 0x17190000, only when step 3 read Android's ID) - chain_create /
 *      set_system_config / object_create, the first writes to ISP hardware from Windows; the run stops after them
 *      (no tune set, set_param, shot, DMA address or interrupt reaches the hardware). Every 3AA0 register is read
 *      before and after (32-bit reads, as Android's regdump read all 64 KiB): out\regs-3AA0-hw-before/after.txt.
 *   9. (0.5) run.flg containing "hw3aashot": item 8, then on the real 3AA0 also the tune sets, set_param, set_ctrl F1
 *      and shot F1 (DMA addresses = S10eIspDma pool PAs; CSIS is not streaming, so no frame arrives), 200 ms of
 *      interrupt-status watching (nothing connected), and the DDK's stop; registers read after shot and after stop.
 * ISPLP (ISP0) and MCSC are mapped but NEVER READ: pd-isplp's state is not known yet (S10eCam 0.12 prints it).
 * Up to 0.3.4 nothing was written to any register; 0.4 writes to 3AA0 only in the hw3aa run (item 8).
 *  10. (0.6) INTERRUPTS: with UEFI v0.2.4 the node has 3AA0's two interrupts (GSIV 351 = the libraries' id 0, 352 =
 *      id 1; _CRS order = creation order of the interrupt objects here). UMDF 2 handles them at PASSIVE_LEVEL
 *      (WDF_INTERRUPT_CONFIG docs). The ISR hands each one to the libraries' handler (isp_front_irq ->
 *      isp_irq_sync(chain 0, id), what the kernel's fimc_is_isr1_ddk / isr2 do); the handler clears the status. When
 *      nobody can take it (no hw3aa run yet, or more than IRQ_STORM of them) the ISR masks 3AA0 at the source:
 *      +0x108 = 0 and +0x118 = 0, the first two writes the libraries' own chain_create makes (Android trace).
 *      Counts go to the registry value "Irq". With UEFI v0.2.3 (no interrupt resource) the objects stay unused.
 *  11. (0.7) run.flg "hw3aaotf": item 9 with the libraries told the sensor runs S10eCam's mode 11 (912 x 684, PD_NONE);
 *      after shot F1 the run waits (OTF_WAIT 40 s, mark "armed, waiting for OTF frames") for REAL frames from
 *      CSIS2 (isp-s10.ps1 starts cam-s10 shot -Bin -Otf), queues set_ctrl / shot per frame end, then saves the 3AA's
 *      before / after-BDS buffers (out\otf-*.raw) and stops.
 *  12. (0.8, UEFI v0.2.5) the probe reads ISPLP's ID (+0xF0 / +0xF4, Android 0x07110000 / 0xC2AD; S10eCam 0.13 read
 *      them on Windows); four interrupt objects: GSIV 351 / 352 = 3AA0 (chain 0, ids 0 / 1), 328 / 329 = ISP0 (chain
 *      2, ids 6 / 7). run.flg "hw3aaotfisp": item 11 plus ISP0 on the REAL ISPLP: per 3AA frame end, ISP0 M2M from
 *      the after-BDS buffer to its YUV DMA (YUV422 1-plane YCbYCr = YUY2, 912 x 684) -> out\otf-isp-yuy2-*.raw.
 *  13. (0.9) CAMERA PIPELINE: when C:\S10Prov\isp\pipe.flg exists (persistent; isp-s10.ps1 pipe on / off) and no
 *      run.flg, the run thread starts at device start as item 12's setup, then serves S10eCamera through the
 *      S10EISP_SHM block (the S10eIspDma pool's last 64 KiB): Want -> arm the 3AA0, per real frame ISP0 into one of
 *      3 YUY2 slots, published as Seq / Latest; the libraries' sensor requests (exposure / gain) go there too.
 * The result goes to the registry, value "Probe" under
 *   HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\SAMS982C\<instance>\Device Parameters\S10eIsp
 * (PLUGPLAY_REGKEY_DEVICE | WDF_REGKEY_DEVICE_SUBKEY, KEY_READ | KEY_SET_VALUE: the only device key a UMDF driver may
 * write - WdfDeviceOpenRegistryKey docs). isp-s10.ps1 reads it.
 */
#include <windows.h>
#include <wdf.h>
#include <strsafe.h>
#include <stdlib.h>                          /* _set_invalid_parameter_handler (UCRT) */
#include "../S10eIspDma/S10eIspDmaIoctl.h"
#include "plat_posix.h"                      /* tools/isp-host: plat_win_dma_pool, plat_win_set_thread_fatal */
int isp_front_main(int argc, char **argv);   /* tools/isp-host/test_front.c built with ISPHOST_DRIVER */
void isp_front_set_hw3aa(volatile void *va, int mode); /* 0.4/0.5/0.7: the REAL 3AA0 window (test_front.c) */
int isp_front_irq(unsigned int chain, unsigned int id); /* 0.6/0.8: a real interrupt to the libraries' handler */
void isp_front_set_hwisp(volatile void *va);  /* 0.8: the REAL ISPLP (ISP0) window (test_front.c) */
void isp_front_set_pipe(void *shm);           /* 0.9: the camera pipeline's control block (test_front.c) */

#define S10EISP_VERSION L"0.9"

/* resources, in the DSDT node's order (Dsdt.asl Device (CISP)) */
#define RES_CMU_ISPPRE 0
#define RES_3AA0 1
#define RES_ISPLP 2
#define RES_MCSC 3
#define RES_MAX 8

/* CMU_ISPPRE (S10eCam.c: K cmucal-sfr.c / cmucal-node.c references there) */
#define IP_3AA0_ACLK 0x201Cu   /* CLK_CON_GAT_GOUT_BLK_ISPPRE_UID_IS_ISPPRE_IPCLKPORT_ACLK_3AA0 */
#define IP_LHM_AXI_P 0x20B4u   /* the register path into ISPPRE */
#define CG_VAL (1u << 21)
#define CG_MAN (1u << 20)
#define TAA_ID_VERSION 0x0F0u  /* 3AA0: Android 0x07010000 */
#define TAA_ID_CODE 0x0F4u     /* 3AA0: Android 0x0000CC22 */

typedef struct _RES {
    PHYSICAL_ADDRESS pa;
    ULONG len;
    PVOID pseudo;              /* WdfDeviceMapIoSpace result: for WDF_READ_REGISTER_* */
    volatile UCHAR *mapped;    /* WdfDeviceGetHardwareRegisterMappedAddress: plain loads / stores */
} RES;

typedef struct _DEVCTX {
    RES res[RES_MAX];
    ULONG nres;
    ULONG nint;                /* 0.6: interrupt resources, raw (GSIV) + translated vector, resource-list order */
    ULONG rvec[4], tvec[4];
    WDFINTERRUPT irq[4];
} DEVCTX;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVCTX, GetCtx)

typedef struct _IRQCTX {
    ULONG index;               /* creation order */
    LONG id;                   /* the libraries' interrupt id: 0 = GSIV 351 (INTR_HWIP1), 1 = 352; -1 = not resolved */
    ULONG vec, gsiv;           /* WdfInterruptGetInfo's Vector, and the raw vector (GSIV) of the matching resource */
    BOOLEAN by_gsiv;           /* id from the GSIV (else: creation order, the fallback) */
} IRQCTX;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(IRQCTX, GetIrqCtx)
#define TAA_INT1_ENABLE 0x108u /* written 0 first by the libraries' chain_create; 0xfff0005f after shot F1 */
#define TAA_INT2_ENABLE 0x118u
#define IRQ_STORM 200000       /* interrupts per id handed to the libraries before the ISR gives up and masks */
#define NIRQ 4                 /* slots: 0 / 1 = 3AA0 GSIV 351 / 352, 2 / 3 = ISP0 GSIV 328 / 329 (0.8) */
static const ULONG g_irq_gsiv[NIRQ] = {351, 352, 328, 329};
static const ULONG g_irq_chain[NIRQ] = {0, 0, 2, 2}, g_irq_libid[NIRQ] = {0, 1, 6, 7};
static volatile LONG g_irq_n[NIRQ], g_irq_lib[NIRQ], g_irq_masked[NIRQ];
static volatile UCHAR *g_isplp_va;   /* 0.8: ISPLP's mapping, set only after its ID read = Android's */

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD EvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE EvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE EvtReleaseHardware;
EVT_WDF_INTERRUPT_ISR IspIsr;
static LONG IrqId(WDFINTERRUPT Interrupt);

/* ---- probe log (one REG_SZ value) ---- */
static WCHAR g_log[8192];

static void Log(PCWSTR fmt, ...)
{
    va_list ap;
    size_t n = wcslen(g_log);
    va_start(ap, fmt);
    StringCchVPrintfW(g_log + n, ARRAYSIZE(g_log) - n, fmt, ap);
    va_end(ap);
    n = wcslen(g_log);
    StringCchCopyW(g_log + n, ARRAYSIZE(g_log) - n, L"\r\n");
}

static BOOLEAN ClkOn(ULONG gate) { return !((gate & CG_MAN) != 0 && (gate & CG_VAL) == 0); }

/* ---- x18 check (tools/isp-host/x18test.c, the same words) ---- */
static const ULONG g_spin[] = {
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
typedef ULONG64 (*SPIN)(ULONG64 iterations, ULONG64 magic);
typedef struct _SPINARG { SPIN fn; ULONG64 magic, seen; } SPINARG;

static DWORD WINAPI SpinThread(LPVOID p)
{
    SPINARG *a = (SPINARG *)p;
    a->seen = a->fn(100000000ull, a->magic);
    return 0;
}

static void ProbeCode(void)
{
    PROCESS_MITIGATION_DYNAMIC_CODE_POLICY dc;
    RtlZeroMemory(&dc, sizeof(dc));
    if (GetProcessMitigationPolicy(GetCurrentProcess(), ProcessDynamicCodePolicy, &dc, sizeof(dc)))
        Log(L"dynamic-code policy: ProhibitDynamicCode %u AllowThreadOptOut %u AllowRemoteDowngrade %u",
            dc.ProhibitDynamicCode, dc.AllowThreadOptOut, dc.AllowRemoteDowngrade);
    else
        Log(L"dynamic-code policy: GetProcessMitigationPolicy failed %lu", GetLastError());

    PVOID code = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!code) {
        Log(L"RWX: VirtualAlloc(PAGE_EXECUTE_READWRITE) FAILED %lu -> the host's generated trampolines cannot run "
            L"in this process", GetLastError());
        return;
    }
    RtlCopyMemory(code, g_spin, sizeof(g_spin));
    FlushInstructionCache(GetCurrentProcess(), code, sizeof(g_spin));
    Log(L"RWX: VirtualAlloc(PAGE_EXECUTE_READWRITE) ok at %p", code);

    SPINARG a[8];
    HANDLE h[8];
    ULONG bad = 0, n = 0;
    for (ULONG i = 0; i < 8; i++) {
        a[i].fn = (SPIN)code;
        a[i].magic = 0x5a5a000000000000ull | ((ULONG64)i << 32) | 0x1234abcdu;
        a[i].seen = 0;
        h[i] = CreateThread(NULL, 0, SpinThread, &a[i], 0, NULL);
        if (h[i]) n++;
    }
    for (ULONG i = 0; i < 8; i++)
        if (h[i]) { WaitForSingleObject(h[i], INFINITE); CloseHandle(h[i]); }
    for (ULONG i = 0; i < 8; i++)
        if (a[i].seen) { bad++; Log(L"x18: thread %lu found 0x%016llX instead of its value", i, a[i].seen); }
    Log(L"x18: %lu threads x 100 M checks ran generated code in this process: %s", n,
        bad ? L"X18 NOT PRESERVED" : L"X18 PRESERVED");
    VirtualFree(code, 0, MEM_RELEASE);
}

/* ---- 0.2: the DMA pool (S10eIspDma) ---- */
typedef ULONG64 (*FN1)(ULONG64);
static const ULONG g_zva[] = { 0xd50b7420, 0xd65f03c0 };     /* dc zva, x0 ; ret */
static const ULONG g_dczid[] = { 0xd53b00e0, 0xd65f03c0 };   /* mrs x0, dczid_el0 ; ret */

static void ProbeDma(void)
{
    HANDLE h = CreateFileW(S10EISPDMA_USER_PATH, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        Log(L"DMA: open %s failed %lu (S10eIspDma not installed / not started: isp-s10.ps1 dma)", S10EISPDMA_USER_PATH,
            GetLastError());
        return;
    }
    S10EISPDMA_MAP_OUT m;
    DWORD got = 0;
    RtlZeroMemory(&m, sizeof(m));
    if (!DeviceIoControl(h, IOCTL_S10EISPDMA_MAP, NULL, 0, &m, sizeof(m), &got, NULL) || got < sizeof(m)) {
        Log(L"DMA: IOCTL_S10EISPDMA_MAP failed %lu", GetLastError());
        CloseHandle(h);
        return;
    }
    volatile UCHAR *va = (volatile UCHAR *)(ULONG_PTR)m.UserVa;
    Log(L"DMA: pool %llu MiB at PA 0x%08llX..0x%08llX, %s, mapped at %p (S10eIspDma v%lu)", m.Bytes >> 20, m.Pa,
        m.Pa + m.Bytes - 1, m.Cache ? L"write-combined" : L"non-cached", (PVOID)va, m.Version);
    if (m.Pa + m.Bytes > 0x100000000ull) Log(L"DMA: NOT below 4 GiB - unusable for the ISP's 32-bit addresses");

    PUCHAR code = (PUCHAR)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    ULONG64 z = 0;
    if (code) {
        RtlCopyMemory(code, g_zva, sizeof(g_zva));
        RtlCopyMemory(code + 64, g_dczid, sizeof(g_dczid));
        FlushInstructionCache(GetCurrentProcess(), code, 128);
        z = ((FN1)(code + 64))(0);
        Log(L"DCZID_EL0 = 0x%llX (DZP %llu, block %lu bytes)", z, (z >> 4) & 1, 4ul << (z & 15));
    }
    struct { PCWSTR what; int kind; } t[] = {
        {L"aligned 8-byte store + load", 0}, {L"unaligned 8-byte store + load (+0x1003)", 1},
        {L"memset 64 KiB + check", 2}, {L"DC ZVA on a dirty block + check", 3}, {L"last byte store + load", 4},
    };
    for (int i = 0; i < (int)ARRAYSIZE(t); i++) {
        DWORD code_ = 0;
        BOOL ok = FALSE;
        __try {
            switch (t[i].kind) {
            case 0: *(volatile ULONG64 *)(va + 0x1000) = 0x1122334455667788ull;
                    ok = *(volatile ULONG64 *)(va + 0x1000) == 0x1122334455667788ull; break;
            case 1: *(volatile ULONG64 UNALIGNED *)(va + 0x1003) = 0x8877665544332211ull;
                    ok = *(volatile ULONG64 UNALIGNED *)(va + 0x1003) == 0x8877665544332211ull; break;
            case 2: memset((void *)(va + 0x10000), 0xA5, 0x10000);
                    ok = va[0x10000] == 0xA5 && va[0x1FFFF] == 0xA5; break;
            case 3: if (!code || (z & 16)) { ok = FALSE; break; }
                    memset((void *)(va + 0x20000), 0x5A, 4096);
                    ((FN1)code)((ULONG64)(ULONG_PTR)(va + 0x20000));
                    ok = va[0x20000] == 0 && va[0x20000 + (4u << (z & 15)) - 1] == 0 &&
                         va[0x20000 + (4u << (z & 15))] == 0x5A; break;
            case 4: va[m.Bytes - 1] = 0x3C; ok = va[m.Bytes - 1] == 0x3C; break;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            code_ = GetExceptionCode();
        }
        if (code_) Log(L"DMA test: %s -> EXCEPTION 0x%08lX", t[i].what, code_);
        else Log(L"DMA test: %s -> %s", t[i].what, ok ? L"ok" : L"WRONG VALUE (or not run)");
    }
    if (code) VirtualFree(code, 0, MEM_RELEASE);
    CloseHandle(h);                                     /* unmaps (S10eIspDma CLEANUP) and re-zeroes the pool */
    Log(L"DMA: handle closed (pool unmapped)");
}

/* ---- 0.3: the front run (tools/isp-host inside this process) ---- */
static WDFDEVICE g_dev;
static LONG g_front_started;
#define ISP_DIR "C:\\S10Prov\\isp"

static void SetValue(PCWSTR name, PCWSTR text)
{
    WDFKEY key;
    UNICODE_STRING n;
    if (!g_dev) return;
    if (!NT_SUCCESS(WdfDeviceOpenRegistryKey(g_dev, PLUGPLAY_REGKEY_DEVICE | WDF_REGKEY_DEVICE_SUBKEY,
                                             KEY_READ | KEY_SET_VALUE, WDF_NO_OBJECT_ATTRIBUTES, &key)))
        return;
    RtlInitUnicodeString(&n, name);
    WdfRegistryAssignValue(key, &n, REG_SZ, (ULONG)((wcslen(text) + 1) * sizeof(WCHAR)), (PVOID)text);
    WdfRegistryClose(key);
}

/* 0.3.1 (after a blue screen during the first front run, 2026-11-18 19:07, no dump / no event): every step of the
   run thread goes to C:\S10Prov\isp\front-trace.txt, written through to disk (FILE_FLAG_WRITE_THROUGH), so the last
   line survives a crash; host.log is opened with the UCRT commit flag for the same reason (test_front.c). */
static void Trace(const char *fmt, ...)
{
    char b[512];
    va_list ap;
    DWORD n;
    va_start(ap, fmt);
    StringCchVPrintfA(b, ARRAYSIZE(b) - 2, fmt, ap);
    va_end(ap);
    StringCchCatA(b, ARRAYSIZE(b), "\r\n");
    HANDLE f = CreateFileA(ISP_DIR "\\front-trace.txt", FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    WriteFile(f, b, (DWORD)strlen(b), &n, NULL);
    FlushFileBuffers(f);
    CloseHandle(f);
}

static BOOL g_nodma, g_hw3aa, g_hw3aashot, g_hw3aaotf, g_hwisp, g_pipe;
static volatile UCHAR *g_taa_va;   /* 0.4: 3AA0's user-mode mapping, set only after its ID read = Android's */

/* 0.3.3: a CRT invalid-parameter report must not fast-fail Wudfhost (the default handler does: the 0.3.x front runs
   died in setvbuf(size 0) - test_front.c). This handler records it and returns, so the CRT call fails with EINVAL. */
static void __cdecl InvalidParameter(const wchar_t *expr, const wchar_t *func, const wchar_t *file, unsigned int line,
                                     uintptr_t reserved)
{
    UNREFERENCED_PARAMETER(expr);
    UNREFERENCED_PARAMETER(func);
    UNREFERENCED_PARAMETER(file);
    UNREFERENCED_PARAMETER(line);
    UNREFERENCED_PARAMETER(reserved);
    Trace("CRT invalid parameter reported (call returns an error)");
}

static void IrqReport(void)
{
    WCHAR m[1024];
    size_t n;
    m[0] = 0;
    for (ULONG k = 0; k < NIRQ; k++) {
        n = wcslen(m);
        StringCchPrintfW(m + n, ARRAYSIZE(m) - n, L"%sGSIV %lu (chain %lu id %lu): %ld, to the libraries %ld, masked %ld",
                         k ? L"; " : L"", g_irq_gsiv[k], g_irq_chain[k], g_irq_libid[k], g_irq_n[k], g_irq_lib[k],
                         g_irq_masked[k]);
    }
    DEVCTX *c = g_dev ? GetCtx(g_dev) : NULL;
    for (ULONG i = 0; c && i < NIRQ; i++) {
        if (!c->irq[i] || !c->nint) continue;
        LONG id = IrqId(c->irq[i]);
        IRQCTX *x = GetIrqCtx(c->irq[i]);
        n = wcslen(m);
        StringCchPrintfW(m + n, ARRAYSIZE(m) - n, L"; object %lu: vector %lu GSIV %lu -> slot %ld (%s)", i, x->vec,
                         x->gsiv, id, x->by_gsiv ? L"by GSIV" : L"by creation order");
    }
    SetValue(L"Irq", m);
    Trace("interrupts: %S", m);
}

static DWORD WINAPI FrontThread(LPVOID p)
{
    static char dir[MAX_PATH], data[MAX_PATH], layout[MAX_PATH], cal[MAX_PATH];
    WCHAR msg[512];
    HMODULE self = NULL;
    UNREFERENCED_PARAMETER(p);

    /* the driver's own folder in the driver store: the fake windows' dumps + the sensor-interface layout (INF) */
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)(ULONG_PTR)FrontThread, &self);
    GetModuleFileNameA(self, dir, MAX_PATH);
    char *slash = strrchr(dir, '\\');
    if (slash) *slash = 0;
    StringCchCopyA(data, MAX_PATH, dir);
    StringCchPrintfA(layout, MAX_PATH, "%s\\sensor_itf_layout.txt", dir);

    _set_invalid_parameter_handler(InvalidParameter);
    Trace("S10eIsp %S front thread: process %lu, data %s, nodma %d, hw3aa %d, hw3aashot %d", S10EISP_VERSION,
          GetCurrentProcessId(), data, g_nodma, g_hw3aa, g_hw3aashot);
    if ((g_hw3aashot || g_hw3aaotf) && g_nodma) {
        /* the fake pool's dvas (plat_win.c, 0x80000000 + offset) are REAL DRAM addresses on this phone: an armed 3AA
           must only ever hold addresses inside S10eIspDma's pool */
        Trace("hw3aashot with nodma refused: the fake pool's addresses are real DRAM - not run");
        SetValue(L"FrontRun", L"not run: hw3aashot needs the S10eIspDma pool (nodma refused)");
        return 1;
    }
    if (g_hw3aa) {
        if (!g_taa_va) {
            Trace("hw3aa: no verified 3AA0 mapping (probe step 3 did not read Android's ID) - not run");
            SetValue(L"FrontRun", L"not run: hw3aa asked, 3AA0 mapping not verified");
            return 1;
        }
        isp_front_set_hw3aa(g_taa_va, g_hw3aaotf ? 2 : g_hw3aashot ? 1 : 0);
        if (g_hwisp) {
            if (!g_isplp_va) {
                Trace("hw3aaotfisp: no verified ISPLP mapping - not run");
                SetValue(L"FrontRun", L"not run: ISPLP mapping not verified");
                return 1;
            }
            isp_front_set_hwisp(g_isplp_va);
            Trace("hwisp: the libraries get the REAL ISPLP window %p", (void *)g_isplp_va);
        }
        Trace("hw3aa: the libraries get the REAL 3AA0 window %p for the init steps%s", (void *)g_taa_va,
              g_hw3aashot ? " + 3AA set_param / shot F1 / stop" : "");
    }
    S10EISPDMA_MAP_OUT m;
    DWORD got = 0;
    RtlZeroMemory(&m, sizeof(m));
    if (g_nodma) {
        /* bisect switch: the libraries get plat_win's own (fake, VirtualAlloc) DMA pool - S10eIspDma is not opened */
        Trace("nodma: S10eIspDma not used, fake pool");
        plat_win_set_thread_fatal(1);
        _putenv_s("FRAMES", "30");   /* 0.3.4: the CRT's getenv() reads its own copy: SetEnvironmentVariable left 3 */
        _putenv_s("QUIET", "1");
        SetValue(L"FrontRun", L"started (nodma)");
        char *argv0[6] = {"isp_front", ISP_DIR, data, ISP_DIR "\\out", layout, NULL};
        plat_win_set_mark_file(ISP_DIR "\\front-trace.txt");   /* 0.3.2: marks inside the core too */
        Trace("calling isp_front_main (nodma)");
        int rc0 = isp_front_main(5, argv0);
        Trace("isp_front_main returned %d", rc0);
        StringCchPrintfW(msg, ARRAYSIZE(msg), L"finished (nodma): rc %d", rc0);
        SetValue(L"FrontRun", msg);
        return 0;
    }
    HANDLE h = CreateFileW(S10EISPDMA_USER_PATH, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    Trace("S10eIspDma open: %s (%lu)", h == INVALID_HANDLE_VALUE ? "FAILED" : "ok", GetLastError());
    if (h == INVALID_HANDLE_VALUE ||
        !DeviceIoControl(h, IOCTL_S10EISPDMA_MAP, NULL, 0, &m, sizeof(m), &got, NULL) || got < sizeof(m)) {
        StringCchPrintfW(msg, ARRAYSIZE(msg), L"not run: S10eIspDma pool unavailable (error %lu)", GetLastError());
        SetValue(L"FrontRun", msg);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        return 1;
    }
    /* the handle stays open for the process's lifetime: the libraries' pools live in the mapping */
    Trace("pool mapped: %llu MiB PA 0x%08llX va 0x%llX cache %u", m.Bytes >> 20, m.Pa, m.UserVa, m.Cache);
    if (g_pipe) {   /* 0.9: the last 64 KiB are the control block shared with S10eCamera, not the libraries' */
        PUCHAR shm = (PUCHAR)(ULONG_PTR)m.UserVa + m.Bytes - S10EISP_SHM_BYTES;
        RtlZeroMemory(shm, S10EISP_SHM_BYTES);
        isp_front_set_pipe(shm);
        plat_win_dma_pool((void *)(ULONG_PTR)m.UserVa, m.Pa, m.Bytes - S10EISP_SHM_BYTES);
        Trace("pipe: control block at pool offset 0x%llX", m.Bytes - S10EISP_SHM_BYTES);
    } else
        plat_win_dma_pool((void *)(ULONG_PTR)m.UserVa, m.Pa, m.Bytes);
    plat_win_set_thread_fatal(1);
    _putenv_s("FRAMES", "30");   /* 0.3.4: the CRT's getenv() reads its own copy: SetEnvironmentVariable left 3 */
    _putenv_s("QUIET", "1");
    char *argv[6] = {"isp_front", ISP_DIR, data, ISP_DIR "\\out", layout, NULL};
    int argc = 5;
    StringCchPrintfA(cal, MAX_PATH, "%s\\rom1_cal.bin", ISP_DIR);
    if (GetFileAttributesA(cal) != INVALID_FILE_ATTRIBUTES) argv[argc++] = cal;
    StringCchPrintfW(msg, ARRAYSIZE(msg),
                     L"started: pool %llu MiB at PA 0x%08llX (%s), data %S, log " ISP_DIR "\\out\\host.log",
                     m.Bytes >> 20, m.Pa, m.Cache ? L"write-combined" : L"non-cached", data);
    SetValue(L"FrontRun", msg);
    plat_win_set_mark_file(ISP_DIR "\\front-trace.txt");
    Trace("calling isp_front_main, argc %d", argc);
    int rc = isp_front_main(argc, argv);      /* an exit() / library assert inside ends this thread only */
    Trace("isp_front_main returned %d", rc);
    StringCchPrintfW(msg, ARRAYSIZE(msg), L"finished: rc %d, log " ISP_DIR "\\out\\host.log", rc);
    SetValue(L"FrontRun", msg);
    IrqReport();
    return 0;
}

static void MaybeFrontRun(void)
{
    char flg[64] = {0};
    DWORD n = 0;
    HANDLE f = CreateFileW(L"" ISP_DIR "\\run.flg", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        /* 0.9: no one-off run requested: the camera pipeline, if switched on (pipe.flg stays) */
        if (GetFileAttributesW(L"" ISP_DIR "\\pipe.flg") == INVALID_FILE_ATTRIBUTES) return;
        g_pipe = g_hw3aa = g_hw3aashot = g_hw3aaotf = g_hwisp = TRUE;
        Trace("pipe.flg found: camera pipeline");
        if (InterlockedExchange(&g_front_started, 1)) return;
        HANDLE pt = CreateThread(NULL, 1u << 20, FrontThread, NULL, 0, NULL);
        Log(L"camera pipeline: %s", pt ? L"started (value FrontRun, " ISP_DIR L"\\out\\host.log)" : L"CreateThread failed");
        if (pt) CloseHandle(pt);
        return;
    }
    ReadFile(f, flg, sizeof(flg) - 1, &n, NULL);
    CloseHandle(f);
    DeleteFileW(L"" ISP_DIR "\\run.flg");
    g_nodma = strstr(flg, "nodma") != NULL;          /* 0.3.1: run.flg containing "nodma" = without S10eIspDma */
    g_hw3aa = strstr(flg, "hw3aa") != NULL;          /* 0.4: the init steps on the REAL 3AA0 */
    g_hw3aaotf = strstr(flg, "hw3aaotf") != NULL;    /* 0.7: + real frames from CSIS2 (OTF) */
    g_hwisp = strstr(flg, "hw3aaotfisp") != NULL;    /* 0.8: + ISP0 on the real ISPLP */
    g_hw3aashot = g_hw3aaotf || strstr(flg, "hw3aashot") != NULL;  /* 0.5: + 3AA set_param / shot F1 / stop on it */
    Trace("run.flg found (\"%s\"), nodma %d, hw3aa %d, hw3aashot %d", flg, g_nodma, g_hw3aa, g_hw3aashot);
    if (InterlockedExchange(&g_front_started, 1)) {
        Log(L"front run: already ran in this process (one per Wudfhost: isp-s10.ps1 restart starts a new one)");
        return;
    }
    HANDLE t = CreateThread(NULL, 1u << 20, FrontThread, NULL, 0, NULL);
    Log(L"front run: %s", t ? L"started (FrontRun value, " ISP_DIR L"\\out\\host.log)" : L"CreateThread failed");
    if (t) CloseHandle(t);
}

/* ---- PnP ---- */
NTSTATUS EvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    DEVCTX *c = GetCtx(Device);
    c->nint = 0;
    g_log[0] = 0;
    Log(L"S10eIsp %s probe, process %lu", S10EISP_VERSION, GetCurrentProcessId());

    ULONG count = WdfCmResourceListGetCount(Translated);
    c->nres = 0;
    for (ULONG i = 0; i < count; i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR d = WdfCmResourceListGetDescriptor(Translated, i);
        if (!d) continue;
        if (d->Type == CmResourceTypeInterrupt) {
            PCM_PARTIAL_RESOURCE_DESCRIPTOR rd = WdfCmResourceListGetDescriptor(Raw, i);
            ULONG gsiv = (rd && rd->Type == CmResourceTypeInterrupt) ? rd->u.Interrupt.Vector : 0;
            Log(L"resource %lu: interrupt raw vector %lu (GSIV), translated vector %lu level %lu flags 0x%X (%s)", i,
                gsiv, d->u.Interrupt.Vector, d->u.Interrupt.Level, d->Flags,
                (d->Flags & CM_RESOURCE_INTERRUPT_LATCHED) ? L"edge" : L"level");
            if (c->nint < ARRAYSIZE(c->rvec)) {
                c->rvec[c->nint] = gsiv;
                c->tvec[c->nint] = d->u.Interrupt.Vector;
                c->nint++;
            }
            continue;
        }
        if (d->Type != CmResourceTypeMemory) {
            Log(L"resource %lu: type %u (not memory, ignored)", i, d->Type);
            continue;
        }
        if (c->nres >= RES_MAX) break;
        RES *r = &c->res[c->nres];
        r->pa = d->u.Memory.Start;
        r->len = d->u.Memory.Length;
        NTSTATUS s = WdfDeviceMapIoSpace(Device, r->pa, r->len, MmNonCached, &r->pseudo);
        if (!NT_SUCCESS(s)) {
            Log(L"memory %lu: 0x%08llX + 0x%lX: WdfDeviceMapIoSpace FAILED 0x%08lX", c->nres, r->pa.QuadPart, r->len, s);
            r->pseudo = NULL;
        } else {
            r->mapped = (volatile UCHAR *)WdfDeviceGetHardwareRegisterMappedAddress(Device, r->pseudo);
            Log(L"memory %lu: 0x%08llX + 0x%lX -> pseudo %p, user-mode mapping %p", c->nres, r->pa.QuadPart, r->len,
                r->pseudo, (PVOID)r->mapped);
        }
        c->nres++;
    }

    RES *cmu = &c->res[RES_CMU_ISPPRE], *taa = &c->res[RES_3AA0];
    if (c->nres > RES_3AA0 && cmu->pseudo && cmu->pa.QuadPart == 0x17000000 && taa->pa.QuadPart == 0x17190000) {
        ULONG g1 = WDF_READ_REGISTER_ULONG(Device, (PULONG)((PUCHAR)cmu->pseudo + IP_3AA0_ACLK));
        ULONG l1 = WDF_READ_REGISTER_ULONG(Device, (PULONG)((PUCHAR)cmu->pseudo + IP_LHM_AXI_P));
        Log(L"CMU_ISPPRE via WDF_READ_REGISTER_ULONG: 3AA0 aclk gate %08lX, LHM_AXI_P %08lX", g1, l1);
        if (cmu->mapped) {
            ULONG g2 = *(volatile ULONG *)(cmu->mapped + IP_3AA0_ACLK);
            ULONG l2 = *(volatile ULONG *)(cmu->mapped + IP_LHM_AXI_P);
            Log(L"CMU_ISPPRE via the user-mode mapping: 3AA0 aclk gate %08lX, LHM_AXI_P %08lX -> %s", g2, l2,
                (g1 == g2 && l1 == l2) ? L"same as the system call" : L"DIFFERENT");
        }
        if (!ClkOn(g1) || !ClkOn(l1)) {
            Log(L"3AA0 not read: aclk or LHM_AXI_P gated");
        } else if (taa->mapped) {
            ULONG v = *(volatile ULONG *)(taa->mapped + TAA_ID_VERSION);
            ULONG k = *(volatile ULONG *)(taa->mapped + TAA_ID_CODE);
            Log(L"3AA0 via the user-mode mapping: +0xF0 %08lX +0xF4 %08lX -> %s", v, k,
                (v == 0x07010000u && k == 0xCC22u) ? L"= Android's: the libraries can reach the 3AA from this process"
                                                   : L"NOT Android's 0x07010000 / 0x0000CC22");
            g_taa_va = (v == 0x07010000u && k == 0xCC22u) ? taa->mapped : NULL;
        } else if (taa->pseudo) {
            ULONG v = WDF_READ_REGISTER_ULONG(Device, (PULONG)((PUCHAR)taa->pseudo + TAA_ID_VERSION));
            ULONG k = WDF_READ_REGISTER_ULONG(Device, (PULONG)((PUCHAR)taa->pseudo + TAA_ID_CODE));
            Log(L"3AA0 via WDF_READ_REGISTER_ULONG (no user-mode mapping): +0xF0 %08lX +0xF4 %08lX", v, k);
        }
    } else {
        Log(L"resources not as expected (CMU_ISPPRE 0x17000000, 3AA0 0x17190000 first): no register read");
    }
    /* 0.8: ISPLP (ISP0) ID through the user-mode mapping. pd-isplp is ON as sboot leaves it and S10eCam 0.13 read
       these two words on Windows with CMU_ISPLP's gates on (cam-s10 isplp, 2026-11-18); this driver has no CMU_ISPLP
       resource to re-check the gates - recorded as the remaining risk of this read. */
    RES *lp = &c->res[RES_ISPLP];
    if (c->nres > RES_ISPLP && lp->pa.QuadPart == 0x17430000 && lp->mapped) {
        ULONG v = *(volatile ULONG *)(lp->mapped + TAA_ID_VERSION);
        ULONG k = *(volatile ULONG *)(lp->mapped + TAA_ID_CODE);
        BOOLEAN ok = (v == 0x07110000u && k == 0xC2ADu);
        Log(L"ISPLP via the user-mode mapping: +0xF0 %08lX +0xF4 %08lX -> %s", v, k,
            ok ? L"= Android's" : L"NOT Android's 0x07110000 / 0x0000C2AD");
        g_isplp_va = ok ? lp->mapped : NULL;
    }
    Log(L"MCSC: mapped only, never read");

    ProbeCode();
    ProbeDma();
    g_dev = Device;
    MaybeFrontRun();

    WDFKEY key;
    NTSTATUS s = WdfDeviceOpenRegistryKey(Device, PLUGPLAY_REGKEY_DEVICE | WDF_REGKEY_DEVICE_SUBKEY,
                                          KEY_READ | KEY_SET_VALUE, WDF_NO_OBJECT_ATTRIBUTES, &key);
    if (NT_SUCCESS(s)) {
        DECLARE_CONST_UNICODE_STRING(name, L"Probe");
        DECLARE_CONST_UNICODE_STRING(ver, L"Version");
        WdfRegistryAssignValue(key, &name, REG_SZ, (ULONG)((wcslen(g_log) + 1) * sizeof(WCHAR)), g_log);
        WdfRegistryAssignValue(key, &ver, REG_SZ, (ULONG)sizeof(S10EISP_VERSION), (PVOID)S10EISP_VERSION);
        WdfRegistryClose(key);
    }
    OutputDebugStringW(g_log);
    return STATUS_SUCCESS;
}

NTSTATUS EvtReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    DEVCTX *c = GetCtx(Device);
    UNREFERENCED_PARAMETER(Translated);
    g_isplp_va = NULL;
    g_taa_va = NULL;   /* a hw3aa run still in its init steps would now fault: the run lasts ~1 s after start */
    for (ULONG i = 0; i < c->nres; i++) {
        if (c->res[i].pseudo) WdfDeviceUnmapIoSpace(Device, c->res[i].pseudo, c->res[i].len);
        c->res[i].pseudo = NULL;
        c->res[i].mapped = NULL;
    }
    c->nres = 0;
    return STATUS_SUCCESS;
}

/* which interrupt an object got: the docs fetched do not say how the framework matches resources to objects, so the
   object's own vector (WdfInterruptGetInfo, valid after EvtDevicePrepareHardware) is looked up in the translated list
   and the raw entry next to it gives the GSIV -> slot (g_irq_gsiv). Creation order only as the fallback (recorded). */
static LONG IrqId(WDFINTERRUPT Interrupt)
{
    IRQCTX *x = GetIrqCtx(Interrupt);
    if (x->id >= 0) return x->id;
    WDF_INTERRUPT_INFO info;
    WDF_INTERRUPT_INFO_INIT(&info);
    WdfInterruptGetInfo(Interrupt, &info);
    DEVCTX *c = GetCtx(WdfInterruptGetDevice(Interrupt));
    x->vec = info.Vector;
    LONG id = (LONG)x->index;
    for (ULONG k = 0; k < c->nint; k++)
        if (c->tvec[k] == info.Vector) {
            x->gsiv = c->rvec[k];
            for (ULONG j = 0; j < NIRQ; j++)
                if (x->gsiv == g_irq_gsiv[j]) { id = (LONG)j; x->by_gsiv = TRUE; }
        }
    x->id = id;
    return id;
}

/* 0.6: PASSIVE_LEVEL (UMDF 2 always). Each interrupt goes to the libraries' handler, which clears the 3AA0 status;
   if it cannot (nothing to hand it to, or a storm), the source is masked so the level line drops. */
BOOLEAN IspIsr(WDFINTERRUPT Interrupt, ULONG MessageID)
{
    LONG id = IrqId(Interrupt);
    UNREFERENCED_PARAMETER(MessageID);
    if (id < 0 || id >= NIRQ) return FALSE;
    LONG n = InterlockedIncrement(&g_irq_n[id]);
    if (n <= IRQ_STORM && isp_front_irq(g_irq_chain[id], g_irq_libid[id]) == 0) {
        InterlockedIncrement(&g_irq_lib[id]);
        return TRUE;
    }
    /* mask at the source: 3AA0 or ISP0 (both: +0x108 / +0x118 = 0 are the first writes of their chain_create) */
    DEVCTX *c = GetCtx(WdfInterruptGetDevice(Interrupt));
    ULONG ri = id < 2 ? RES_3AA0 : RES_ISPLP;
    RES *taa = &c->res[ri];
    if (c->nres > ri && taa->pa.QuadPart == (id < 2 ? 0x17190000 : 0x17430000)) {
        if (taa->mapped) {
            *(volatile ULONG *)(taa->mapped + TAA_INT1_ENABLE) = 0;
            *(volatile ULONG *)(taa->mapped + TAA_INT2_ENABLE) = 0;
        } else if (taa->pseudo) {
            WDF_WRITE_REGISTER_ULONG(WdfInterruptGetDevice(Interrupt), (PULONG)((PUCHAR)taa->pseudo + TAA_INT1_ENABLE), 0);
            WDF_WRITE_REGISTER_ULONG(WdfInterruptGetDevice(Interrupt), (PULONG)((PUCHAR)taa->pseudo + TAA_INT2_ENABLE), 0);
        }
    }
    InterlockedIncrement(&g_irq_masked[id]);
    return TRUE;
}

NTSTATUS EvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT Init)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES attr;
    WDFDEVICE dev;
    UNREFERENCED_PARAMETER(Driver);
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = EvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = EvtReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(Init, &pnp);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, DEVCTX);
    NTSTATUS s = WdfDeviceCreate(&Init, &attr, &dev);
    if (!NT_SUCCESS(s)) return s;
    /* 0.6: one interrupt object per interrupt (WdfInterruptCreate docs: "once for each interrupt vector"); which one
       each gets is resolved by its vector (IrqId); 0.8: four (UEFI v0.2.5), unused ones stay unused */
    for (ULONG i = 0; i < NIRQ; i++) {
        WDF_INTERRUPT_CONFIG ic;
        WDF_OBJECT_ATTRIBUTES ia;
        WDFINTERRUPT irq;
        WDF_INTERRUPT_CONFIG_INIT(&ic, IspIsr, NULL);
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&ia, IRQCTX);
        s = WdfInterruptCreate(dev, &ic, &ia, &irq);
        if (!NT_SUCCESS(s)) return s;
        GetIrqCtx(irq)->index = i;
        GetIrqCtx(irq)->id = -1;
        GetCtx(dev)->irq[i] = irq;
    }
    return STATUS_SUCCESS;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG cfg;
    WDF_DRIVER_CONFIG_INIT(&cfg, EvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &cfg, WDF_NO_HANDLE);
}
