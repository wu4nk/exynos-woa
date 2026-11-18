/*
 * S10eIsp.c (v0.1) - Galaxy S10 camera ISP, UMDF 2 driver (notes/s10-isp.md, Stage 4 option A, plan item 4).
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

#define S10EISP_VERSION L"0.1"

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
