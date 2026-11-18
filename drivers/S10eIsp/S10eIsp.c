/*
 * S10eIsp.c (v0.3.3) - Galaxy S10 camera ISP, UMDF 2 driver (notes/s10-isp.md, Stage 4 option A, plan item 4).
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
 * ISPLP (ISP0) and MCSC are mapped but NEVER READ: pd-isplp's state is not known yet (S10eCam 0.12 prints it).
 * Nothing is written to any register. No interrupt is connected (none is declared in the DSDT node).
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

#define S10EISP_VERSION L"0.3.3"

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
} DEVCTX;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVCTX, GetCtx)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD EvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE EvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE EvtReleaseHardware;

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

static BOOL g_nodma;

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
    Trace("S10eIsp %S front thread: process %lu, data %s, nodma %d", S10EISP_VERSION, GetCurrentProcessId(), data,
          g_nodma);
    S10EISPDMA_MAP_OUT m;
    DWORD got = 0;
    RtlZeroMemory(&m, sizeof(m));
    if (g_nodma) {
        /* bisect switch: the libraries get plat_win's own (fake, VirtualAlloc) DMA pool - S10eIspDma is not opened */
        Trace("nodma: S10eIspDma not used, fake pool");
        plat_win_set_thread_fatal(1);
        SetEnvironmentVariableA("FRAMES", "30");
        SetEnvironmentVariableA("QUIET", "1");
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
    plat_win_dma_pool((void *)(ULONG_PTR)m.UserVa, m.Pa, m.Bytes);
    plat_win_set_thread_fatal(1);
    SetEnvironmentVariableA("FRAMES", "30");
    SetEnvironmentVariableA("QUIET", "1");
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
    return 0;
}

static void MaybeFrontRun(void)
{
    char flg[64] = {0};
    DWORD n = 0;
    HANDLE f = CreateFileW(L"" ISP_DIR "\\run.flg", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    ReadFile(f, flg, sizeof(flg) - 1, &n, NULL);
    CloseHandle(f);
    DeleteFileW(L"" ISP_DIR "\\run.flg");
    g_nodma = strstr(flg, "nodma") != NULL;          /* 0.3.1: run.flg containing "nodma" = without S10eIspDma */
    Trace("run.flg found (\"%s\"), nodma %d", flg, g_nodma);
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
    UNREFERENCED_PARAMETER(Raw);
    g_log[0] = 0;
    Log(L"S10eIsp %s probe, process %lu", S10EISP_VERSION, GetCurrentProcessId());

    ULONG count = WdfCmResourceListGetCount(Translated);
    c->nres = 0;
    for (ULONG i = 0; i < count; i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR d = WdfCmResourceListGetDescriptor(Translated, i);
        if (!d) continue;
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
        } else if (taa->pseudo) {
            ULONG v = WDF_READ_REGISTER_ULONG(Device, (PULONG)((PUCHAR)taa->pseudo + TAA_ID_VERSION));
            ULONG k = WDF_READ_REGISTER_ULONG(Device, (PULONG)((PUCHAR)taa->pseudo + TAA_ID_CODE));
            Log(L"3AA0 via WDF_READ_REGISTER_ULONG (no user-mode mapping): +0xF0 %08lX +0xF4 %08lX", v, k);
        }
    } else {
        Log(L"resources not as expected (CMU_ISPPRE 0x17000000, 3AA0 0x17190000 first): no register read");
    }
    Log(L"ISPLP / MCSC: mapped only, never read (pd-isplp state unknown)");

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
    for (ULONG i = 0; i < c->nres; i++) {
        if (c->res[i].pseudo) WdfDeviceUnmapIoSpace(Device, c->res[i].pseudo, c->res[i].len);
        c->res[i].pseudo = NULL;
        c->res[i].mapped = NULL;
    }
    c->nres = 0;
    return STATUS_SUCCESS;
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
    return WdfDeviceCreate(&Init, &attr, &dev);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG cfg;
    WDF_DRIVER_CONFIG_INIT(&cfg, EvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &cfg, WDF_NO_HANDLE);
}
