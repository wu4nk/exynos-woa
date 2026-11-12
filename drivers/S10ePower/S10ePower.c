/*
 * S10ePower - the Windows ACPM client for CPU/GPU clocks and temperatures (notes/clocks-dvfs.md).
 *
 * C0 (always): map the ACPM SRAM and record the AP channel table (ipc_channels / ipc_ap_max at SRAM+0x7000) in the
 * service key - read-only, so the live channel layout is known before anything is sent.
 * C1/C2 (only with the service-key REG_DWORD AllowSend = 1): one serialized, polling ACPM IPC client (a port of the
 * device-validated UEFI client Beyond0LteGpuProbeDxe/GpuAcpm.c, kernel acpm_ipc.c citations kept there):
 *   - READ_TEMP on the TMU channel (ch 9, stock DT acpm_tmu; used only if the channel table shows id 9 as a queue
 *     channel) every second for tz 1 MID, 2 LITTLE, 3 G3D;
 *   - FREQ_REQ on the DVFS channel (ch 0, device-validated from UEFI) for LITTLE (idx 2), MID (idx 3), G3D (idx 10),
 *     only to rates on the DT ladders, and never above the thermal-guard cap (notes/clocks-dvfs.md "thermal guard").
 * Without temperature readings the cap stays at the UEFI boot rates (no clock increase without thermal data -
 * memory thermals-before-clocks). The ACPM is load-bearing: one request at a time, every request waited for.
 *
 * SPDX-License-Identifier: GPL-2.0
 */
#include <ntddk.h>
#include <wdmsec.h>
#include "S10ePowerIoctl.h"

/* ---- VERIFIED addresses (firmware Exynos9820.h) ---- */
#define ACPM_MBOX_BASE        0x0000000015900000ULL   /* EXYNOS_ACPM_IPC_MBOX_BASE (live DT acpm_ipc reg[0]) */
#define ACPM_MBOX_SIZE        0x1000u
#define ACPM_SRAM_BASE        0x0000000002039000ULL   /* EXYNOS_ACPM_SRAM_BASE (live DT acpm_ipc reg[1]) */
#define ACPM_SRAM_SIZE        0x00030000u
#define ACPM_INITDATA_OFFSET  0x00007000u             /* EXYNOS_ACPM_INITDATA_OFFSET */

/* ---- protocol (GpuAcpm.c, kernel citations there) ---- */
#define ACPM_MBOX_INTGR0      0x0008u
#define ACPM_MBOX_INTGR1      0x001Cu
#define ACPM_MBOX_INTCR1      0x0020u
#define ACPM_MBOX_INTSR1      0x0028u
#define ACPM_FW_IPC_CHANNELS  0x08u
#define ACPM_FW_IPC_AP_MAX    0x18u
#define ACPM_CH_STRIDE        0x48u
#define ACPM_CH_ID            0x00u
#define ACPM_CH_TYPE          0x0Cu
#define ACPM_CH_RX_REAR       0x10u
#define ACPM_CH_RX_FRONT      0x14u
#define ACPM_CH_RX_BASE       0x18u
#define ACPM_CH_TX_REAR       0x28u
#define ACPM_CH_TX_FRONT      0x2Cu
#define ACPM_CH_TX_BASE       0x30u
#define ACPM_CH_Q_LEN         0x34u
#define ACPM_CH_Q_ELEM_SIZE   0x38u
#define ACPM_TYPE_QUEUE       1u
#define ACPM_SEQ_SHIFT        16u
#define ACPM_SEQ_MASK         0x3Fu
#define ACPM_POLL_US          10u
#define ACPM_QUEUE_FULL_US    15000u
#define ACPM_REPLY_US         100000u
#define ACPM_MAX_CHANNELS     64u

#define ACPM_CH_DVFS          0u      /* VERIFIED (notes/m15-perf.md run 30, live DT clock-controller acpm-ipc-channel) */
#define ACPM_FREQ_REQ         0u      /* acpm_dvfs.h:19 */
#define ACPM_CH_TMU           9u      /* stock DT acpm_tmu acpm-ipc-channel; checked against the live table at load */
#define TMU_READ_TEMP         0x02u   /* exynos_acpm_tmu.h */

/* ---- DT ladders (kHz, descending) ---- */
static const ULONG gLadderLittle[] = { 1950000, 1846000, 1742000, 1586000, 1456000, 1300000, 1157000, 1053000,
                                       949000, 806000, 650000, 546000, 442000 };          /* notes/m15-perf.md */
/* CPU "overclock" (S10 port 2026-11-12, notes/clocks-dvfs.md): YuROM-Ultra-kernel's cluster OC - 2106000 is a level of
 * ACPM's own LITTLE fvmap table (rate + its ASV voltage) above the DT ceiling 1950000. Nothing is written to ACPM: the
 * level is only ALLOWED (CpuOc = 1 and every CpuOcCheck below passing), exactly what the kernel's max_freq raise does. */
static const ULONG gLadderLittleOc[] = { 2106000, 1950000, 1846000, 1742000, 1586000, 1456000, 1300000, 1157000,
                                         1053000, 949000, 806000, 650000, 546000, 442000 };
static BOOLEAN gLittleOc;              /* set once in DriverEntry, before the guard thread and the device exist */
#define LITTLE_TOP() (gLittleOc ? gLadderLittleOc[0] : gLadderLittle[0])
/* 2400000: the DT domain table's top MID step (notes/cpu-driver-9820.md:156), valid only with MIF >= 2093000 (mif-perf
 * constraint) - SetFreq grants it only while this driver has set MIF to 2093000 (user 2026-10-17: max the 6 cores) */
static const ULONG gLadderMid[] = { 2400000, 2314000, 2210000, 2106000, 2002000, 1898000, 1794000, 1690000, 1586000,
                                    1508000, 1404000, 1222000, 1066000, 962000, 845000, 754000, 650000, 507000 };
static ULONG gLadderG3d[] = { 702000, 650000, 572000, 433000, 377000, 325000, 260000, 200000, 156000 };
                              /* exynos9820-mali_common.dtsi gpu_dvfs_table (LineageOS lineage-23.2); [0] becomes the
                               * GpuOc rate while the fvmap patch below is applied (702000 is then gone from ACPM) */
/* MIF: only the two device-verified ends of devfreq_mif (TWRP sysfs 421000..2093000, cur 2093000 under Linux;
 * notes/m15-perf.md:98) - the steps between are not recorded, so nothing else is requested. */
static const ULONG gLadderMif[] = { 2093000, 421000 };
/* UEFI boot rates (notes/m15-perf.md run 32, notes/gpu-m16-g3don.md): the cap when no temperature is known */
#define BOOT_LITTLE  1456000u
#define BOOT_MID     1794000u
#define BOOT_G3D     260000u

typedef struct {
    ULONG Id, Len, Size;
    volatile ULONG *TxRear, *TxFront, *RxRear, *RxFront;
    volatile UCHAR *TxBase, *RxBase;
    BOOLEAN Ok;
} ACPM_CH;

static volatile UCHAR *gMbox, *gSram;
static ACPM_CH gDvfs, gTmu;
static FAST_MUTEX gAcpmLock;
static ULONG gSeq;
static PDEVICE_OBJECT gDevice;
static WCHAR gKey[256];
static S10EPOWER_STATE gState;
static KEVENT gStopEvent;
static PVOID gGuardThread;
static volatile LONG gActive, gUnloading;   /* review fix (1): IOCTLs in flight vs. unload */

DRIVER_INITIALIZE DriverEntry;
static DRIVER_UNLOAD S10ePowerUnload;
static DRIVER_DISPATCH S10ePowerCreateClose, S10ePowerIoctl;

static VOID WriteDword(PCWSTR Name, ULONG Value)
{
    (VOID)RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, gKey, Name, REG_DWORD, &Value, sizeof(Value));
}

static ULONG ReadDword(PCWSTR Name, ULONG Default)
{
    ULONG v = Default, def = Default;
    RTL_QUERY_REGISTRY_TABLE t[2];
    RtlZeroMemory(t, sizeof(t));
    t[0].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK;
    t[0].Name = (PWSTR)Name;
    t[0].EntryContext = &v;
    t[0].DefaultType = (REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT) | REG_DWORD;
    t[0].DefaultData = &def;
    t[0].DefaultLength = sizeof(def);
    if (!NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, gKey, t, NULL, NULL)))
        return Default;
    return v;
}

static ULONG R32(volatile const void *p) { return READ_REGISTER_ULONG((volatile ULONG *)p); }
static VOID W32(volatile void *p, ULONG v) { WRITE_REGISTER_ULONG((volatile ULONG *)p, v); }

static BOOLEAN SramOk(ULONGLONG Off, ULONGLONG Len)
{
    return (Off & 3) == 0 && Len <= ACPM_SRAM_SIZE && Off <= ACPM_SRAM_SIZE - Len;
}

/* channel_init + acpm_ipc_request_channel by id (GpuAcpm.c FindChannel) */
static BOOLEAN FindChannel(ULONG Wanted, ACPM_CH *Ch)
{
    ULONG tableOff = R32(gSram + ACPM_INITDATA_OFFSET + ACPM_FW_IPC_CHANNELS);
    ULONG apMax = R32(gSram + ACPM_INITDATA_OFFSET + ACPM_FW_IPC_AP_MAX), i;

    RtlZeroMemory(Ch, sizeof(*Ch));
    if (apMax == 0 || apMax > ACPM_MAX_CHANNELS || !SramOk(tableOff, (ULONGLONG)apMax * ACPM_CH_STRIDE))
        return FALSE;
    for (i = 0; i < apMax; i++) {
        volatile UCHAR *e = gSram + tableOff + i * ACPM_CH_STRIDE;
        ULONG txr, txf, txb, rxr, rxf, rxb;
        if (R32(e + ACPM_CH_ID) != Wanted)
            continue;
        Ch->Id = Wanted;
        Ch->Len = R32(e + ACPM_CH_Q_LEN);
        Ch->Size = R32(e + ACPM_CH_Q_ELEM_SIZE);
        txr = R32(e + ACPM_CH_RX_REAR); txf = R32(e + ACPM_CH_RX_FRONT); txb = R32(e + ACPM_CH_RX_BASE);
        rxr = R32(e + ACPM_CH_TX_REAR); rxf = R32(e + ACPM_CH_TX_FRONT); rxb = R32(e + ACPM_CH_TX_BASE);
        if (R32(e + ACPM_CH_TYPE) != ACPM_TYPE_QUEUE || Ch->Len < 2 || Ch->Len > 256 || Ch->Size < 16 ||
            Ch->Size > 64 || (Ch->Size & 3) != 0 || !SramOk(txr, 4) || !SramOk(txf, 4) || !SramOk(rxr, 4) ||
            !SramOk(rxf, 4) || !SramOk(txb, (ULONGLONG)Ch->Size * Ch->Len) ||
            !SramOk(rxb, (ULONGLONG)Ch->Size * Ch->Len))
            return FALSE;
        Ch->TxRear = (volatile ULONG *)(gSram + txr); Ch->TxFront = (volatile ULONG *)(gSram + txf);
        Ch->TxBase = gSram + txb;
        Ch->RxRear = (volatile ULONG *)(gSram + rxr); Ch->RxFront = (volatile ULONG *)(gSram + rxf);
        Ch->RxBase = gSram + rxb;
        Ch->Ok = TRUE;
        return TRUE;
    }
    return FALSE;
}

/* check_response (GpuAcpm.c CheckResponse) */
static BOOLEAN CheckResponse(ACPM_CH *Ch, ULONG Seq, ULONG *Reply, BOOLEAN *Broken)
{
    ULONG front = R32(Ch->RxFront), rear = R32(Ch->RxRear), i, steps, w;

    if (front >= Ch->Len || rear >= Ch->Len) {
        *Broken = TRUE;
        return FALSE;
    }
    for (i = rear, steps = 0; i != front && steps < Ch->Len; steps++) {
        volatile UCHAR *elem = Ch->RxBase + Ch->Size * i;
        if (((R32(elem) >> ACPM_SEQ_SHIFT) & ACPM_SEQ_MASK) == Seq) {
            for (w = 0; w < 4; w++)
                Reply[w] = R32(elem + w * 4);
            if (i != rear) {
                volatile UCHAR *re = Ch->RxBase + Ch->Size * rear;
                for (w = 0; w < Ch->Size / 4; w++)
                    W32(elem + w * 4, R32(re + w * 4));
            }
            rear = (rear + 1) % Ch->Len;
            W32(Ch->RxRear, rear);
            front = R32(Ch->RxFront);
            if (rear == front) {
                W32(gMbox + ACPM_MBOX_INTCR1, 1u << Ch->Id);
                if (rear != R32(Ch->RxFront))
                    W32(gMbox + ACPM_MBOX_INTGR1, 1u << Ch->Id);
            }
            return TRUE;
        }
        i = (i + 1) % Ch->Len;
    }
    return FALSE;
}

/* seq 1..63, skipping seqs still pending in the AP RX queue (GpuAcpm.c NextSeq) */
static ULONG NextSeq(ACPM_CH *Ch)
{
    ULONGLONG pending = 0;
    ULONG front = R32(Ch->RxFront), rear = R32(Ch->RxRear), steps, t;

    if (front < Ch->Len && rear < Ch->Len)
        for (steps = 0; rear != front && steps < Ch->Len; steps++) {
            pending |= 1ull << ((R32(Ch->RxBase + Ch->Size * rear) >> ACPM_SEQ_SHIFT) & ACPM_SEQ_MASK);
            rear = (rear + 1) % Ch->Len;
        }
    for (t = 0; t < 63; t++) {
        if (++gSeq >= 64)
            gSeq = 1;
        if ((pending & (1ull << gSeq)) == 0)
            break;
    }
    return gSeq;
}

/* __acpm_ipc_send_data, polling, response expected (GpuAcpm.c AcpmSend). Caller holds gAcpmLock. */
static NTSTATUS AcpmSend(ACPM_CH *Ch, ULONG *Cmd)
{
    ULONG front, next, seq, w, waited;
    BOOLEAN broken = FALSE;

    if (!Ch->Ok)
        return STATUS_DEVICE_NOT_READY;
    front = R32(Ch->TxFront);
    if (front >= Ch->Len || R32(Ch->TxRear) >= Ch->Len)
        return STATUS_DEVICE_DATA_ERROR;
    next = front + 1 >= Ch->Len ? 0 : front + 1;
    for (waited = 0; next == R32(Ch->TxRear); waited += ACPM_POLL_US) {
        if (waited >= ACPM_QUEUE_FULL_US)
            return STATUS_IO_TIMEOUT;
        KeStallExecutionProcessor(ACPM_POLL_US);
    }
    seq = NextSeq(Ch);
    Cmd[0] |= (seq & ACPM_SEQ_MASK) << ACPM_SEQ_SHIFT;
    for (w = 0; w < Ch->Size / 4; w++)
        W32(Ch->TxBase + Ch->Size * front + w * 4, w < 4 ? Cmd[w] : 0);
    W32(Ch->TxFront, next);
    W32(gMbox + ACPM_MBOX_INTGR0, (1u << Ch->Id) << 16);
    InterlockedIncrement((volatile LONG *)&gState.Sends);
    for (waited = 0;; waited += ACPM_POLL_US) {
        if ((R32(gMbox + ACPM_MBOX_INTSR1) & (1u << Ch->Id)) != 0 && CheckResponse(Ch, seq, Cmd, &broken))
            return STATUS_SUCCESS;
        if (broken)
            return STATUS_DEVICE_DATA_ERROR;
        if (waited >= ACPM_REPLY_US) {
            if (CheckResponse(Ch, seq, Cmd, &broken))
                return STATUS_SUCCESS;
            InterlockedIncrement((volatile LONG *)&gState.Timeouts);
            return STATUS_IO_TIMEOUT;
        }
        if (waited < 1000) {
            KeStallExecutionProcessor(ACPM_POLL_US);
        } else {
            /* review fix (2): past 1 ms do not burn the CPU - sleep 1 ms per check */
            LARGE_INTEGER ms;
            ms.QuadPart = -10000;
            KeDelayExecutionThread(KernelMode, FALSE, &ms);
            waited += 1000 - ACPM_POLL_US;
        }
    }
}

static NTSTATUS ReadTemp(ULONG Tz, ULONG *TempC)
{
    ULONG cmd[4] = { 0, TMU_READ_TEMP | (Tz << 16), 0, 0 };   /* w1: byte0 type, byte2 tzid */
    NTSTATUS st;
    ExAcquireFastMutex(&gAcpmLock);
    st = AcpmSend(&gTmu, cmd);
    ExReleaseFastMutex(&gAcpmLock);
    if (!NT_SUCCESS(st))
        return st;
    if ((cmd[1] & 0xFF) != TMU_READ_TEMP || ((cmd[1] >> 16) & 0xFF) != Tz || (CHAR)((cmd[1] >> 8) & 0xFF) < 0)
        return STATUS_DEVICE_DATA_ERROR;   /* reply: byte0 type, byte1 ret (s8), byte2 tzid, byte3 temp */
    *TempC = (cmd[1] >> 24) & 0xFF;
    return STATUS_SUCCESS;
}

static const ULONG *Ladder(ULONG Dom, ULONG *Count, ULONG *Slot)
{
    switch (Dom) {
    case S10EPOWER_DOM_LITTLE:
        if (gLittleOc) { *Count = ARRAYSIZE(gLadderLittleOc); *Slot = 0; return gLadderLittleOc; }
        *Count = ARRAYSIZE(gLadderLittle); *Slot = 0; return gLadderLittle;
    case S10EPOWER_DOM_MID:    *Count = ARRAYSIZE(gLadderMid);    *Slot = 1; return gLadderMid;
    case S10EPOWER_DOM_G3D:    *Count = ARRAYSIZE(gLadderG3d);    *Slot = 2; return gLadderG3d;
    default: return NULL;
    }
}

/* the highest ladder step <= Khz (the lowest step if Khz is below the ladder) */
static ULONG RoundDown(const ULONG *L, ULONG N, ULONG Khz)
{
    ULONG i;
    for (i = 0; i < N; i++)
        if (L[i] <= Khz)
            return L[i];
    return L[N - 1];
}

/* G17: MIF has no thermal cap here (Linux holds it at 2093000 all the time) and no slot in the 3-domain arrays. */
static NTSTATUS SetMif(ULONG Khz)
{
    ULONG cmd[4];
    NTSTATUS st;

    if (!gState.AllowSend || !gDvfs.Ok)
        return STATUS_INVALID_DEVICE_REQUEST;
    gState.MifReqKhz = Khz;
    cmd[0] = S10EPOWER_DOM_MIF; cmd[1] = RoundDown(gLadderMif, ARRAYSIZE(gLadderMif), Khz);
    cmd[2] = ACPM_FREQ_REQ; cmd[3] = 0;
    ExAcquireFastMutex(&gAcpmLock);
    st = AcpmSend(&gDvfs, cmd);
    if (NT_SUCCESS(st))
        gState.MifCurKhz = cmd[1];
    ExReleaseFastMutex(&gAcpmLock);
    if (!NT_SUCCESS(st))
        InterlockedIncrement((volatile LONG *)&gState.SendFails);
    return st;
}

static NTSTATUS SetFreq(ULONG Dom, ULONG Khz)
{
    ULONG n, slot, cmd[4];
    const ULONG *l = Ladder(Dom, &n, &slot);
    NTSTATUS st;

    if (Dom == S10EPOWER_DOM_MIF)
        return SetMif(Khz);
    if (l == NULL || !gState.AllowSend || !gDvfs.Ok)
        return STATUS_INVALID_DEVICE_REQUEST;
    gState.ReqKhz[slot] = Khz;
    Khz = RoundDown(l, n, min(Khz, gState.CapKhz[slot]));
    if (Dom == S10EPOWER_DOM_MID && Khz > 2314000 && gState.MifCurKhz < 2093000)
        Khz = 2314000;   /* MID 2400000 needs MIF >= 2093000 (DT mif-perf) */
    cmd[0] = Dom; cmd[1] = Khz; cmd[2] = ACPM_FREQ_REQ; cmd[3] = 0;
    ExAcquireFastMutex(&gAcpmLock);
    st = AcpmSend(&gDvfs, cmd);
    if (NT_SUCCESS(st))
        gState.CurKhz[slot] = cmd[1];   /* the reply echoes the granted rate (FREQ_GET returns 0 on this firmware);
                                         * review fix (3): updated under the lock */
    ExReleaseFastMutex(&gAcpmLock);
    if (!NT_SUCCESS(st))
        InterlockedIncrement((volatile LONG *)&gState.SendFails);
    return st;
}

/* ---- thermal guard: caps from the hottest CPU zone (MID/LITTLE) and the G3D zone; DT trips: CPU control 83 C,
 * GPU control 88 C, alerts 95..110 C, hot 115 C (notes/thermal-research.md). Hysteresis 5 C. ---- */
static VOID GuardApply(ULONG Cpu, ULONG Gpu)
{
    static ULONG level;
    ULONG want, hot = max(Cpu, Gpu > 5 ? Gpu - 5 : 0);   /* GPU trips sit 5 C above the CPU ones */

    want = hot >= 105 ? 3 : hot >= 95 ? 2 : hot >= 83 ? 1 : 0;
    if (want < level && hot + 5 > (level == 3 ? 105u : level == 2 ? 95u : 83u))
        want = level;                                   /* hysteresis on the way down */
    level = want;
    gState.GuardLevel = level;
    switch (level) {
    case 0: gState.CapKhz[0] = LITTLE_TOP(); gState.CapKhz[1] = gLadderMid[0]; gState.CapKhz[2] = gLadderG3d[0]; break;
    case 1: gState.CapKhz[0] = BOOT_LITTLE; gState.CapKhz[1] = BOOT_MID; gState.CapKhz[2] = 325000; break;
    case 2: gState.CapKhz[0] = 1053000; gState.CapKhz[1] = 1222000; gState.CapKhz[2] = 260000; break;
    default: gState.CapKhz[0] = 442000; gState.CapKhz[1] = 507000; gState.CapKhz[2] = 156000; break;
    }
    /* bring anything above its new cap down at once */
    if (gState.CurKhz[0] > gState.CapKhz[0]) (VOID)SetFreq(S10EPOWER_DOM_LITTLE, gState.CapKhz[0]);
    if (gState.CurKhz[1] > gState.CapKhz[1]) (VOID)SetFreq(S10EPOWER_DOM_MID, gState.CapKhz[1]);
    if (gState.CurKhz[2] > gState.CapKhz[2]) (VOID)SetFreq(S10EPOWER_DOM_G3D, gState.CapKhz[2]);
}

static VOID GuardThread(PVOID Context)
{
    LARGE_INTEGER period;
    UNREFERENCED_PARAMETER(Context);
    period.QuadPart = -10LL * 1000 * 1000;   /* 1 s */
    for (;;) {
        ULONG tz, cpu = 0, gpu = 0, okCount = 0;
        for (tz = 1; tz <= 3 && gTmu.Ok; tz++) {
            ULONG t;
            if (NT_SUCCESS(ReadTemp(tz, &t)) && t < 150) {
                gState.TempC[tz] = t;
                okCount++;
                if (tz == 3) gpu = t; else cpu = max(cpu, t);
            } else {
                InterlockedIncrement((volatile LONG *)&gState.TempFails);
            }
        }
        if (okCount == (gTmu.Ok ? 3u : 0u) && okCount != 0) {
            InterlockedIncrement((volatile LONG *)&gState.TempReads);
            GuardApply(cpu, gpu);
        } else {
            /* no trustworthy temperature: never above the UEFI boot rates */
            gState.CapKhz[0] = BOOT_LITTLE; gState.CapKhz[1] = BOOT_MID; gState.CapKhz[2] = BOOT_G3D;
            gState.GuardLevel = 0xFF;
        }
        if ((gState.TempReads % 10) == 1) {
            WriteDword(L"TempMid", gState.TempC[1]); WriteDword(L"TempLittle", gState.TempC[2]);
            WriteDword(L"TempG3d", gState.TempC[3]); WriteDword(L"TempReads", gState.TempReads);
            WriteDword(L"TempFails", gState.TempFails); WriteDword(L"GuardLevel", gState.GuardLevel);
        }
        if (KeWaitForSingleObject(&gStopEvent, Executive, KernelMode, FALSE, &period) == STATUS_SUCCESS)
            break;
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* ---- G3D overclock (S10 port, notes/clocks-dvfs.md 2026-11-12): a port of YuROM-Ultra-kernel (github.com/yuefdev,
 * drivers/soc/samsung/cal-if/fvmap.c fvmap_init_gpu_oc + exynos9820-oc.h), same checks, same three writes.
 * The ACPM G3D fvmap entry has 12 levels; level 0 = 702000 kHz uses PLL_G3D PMS slot 1 (0x006c0400 = M 108), while
 * PMS slot 0 (0x00740400 = M 116, 754 MHz) is in the table but no level uses it. GpuOc = 754000 points level 0 at slot
 * 0; GpuOc = 806000 also rewrites slot 0 to M 124 (0x007c0400, P 4, S 0 unchanged). The level keeps ITS VOLTAGE (702
 * MHz's) - nothing about the rail changes; whether the silicon runs 754/806 MHz at that voltage is the experiment.
 * Every check below must pass or nothing is written; the original three values are restored on unload. ---- */
#define ACPM_FW_PLUGINS       0x00u
#define ACPM_FW_NUM_PLUGINS   0x04u
#define PLUGIN_STRIDE         48u      /* struct plugin (fw_header/common.h, CONFIG_EXYNOS_ACPM): u32 id, base_addr,
                                        * acpm_ops, plugin_ops, secure/extern masks, timer{2 u32}, u8 is_attached,
                                        * u32 size, u8 stay_attached, u32 fw_name - natural alignment = 48 bytes */
#define PLUGIN_BASE_ADDR      0x04u
#define PLUGIN_IS_ATTACHED    0x20u
#define PLUGIN_STAY_ATTACHED  0x28u
#define PLUGIN_FW_NAME        0x2Cu
#define FVMAP_OFFSET          0xA400u  /* live DT acpm fvmap_offset (scratchpad b1.dts:7768, = kernel exynos9820.dts) */
#define FVMAP_SIZE            0x2000u  /* fvmap.c FVMAP_SIZE SZ_8K */
#define FVH_SIZE              24u      /* struct fvmap_header */
#define FVH_NUM_LV            1u
#define FVH_NUM_MEMBERS       2u
#define FVH_NUM_PLL           3u
#define FVH_O_MEMBERS         18u
#define FVH_O_RATEVOLT        20u
#define FVH_O_TABLES          22u
#define PLLH_LEVEL            6u       /* struct pll_header: u32 addr, u16 o_lock, u16 level, u32 pms[] */
#define PLLH_PMS              8u
#define G3D_PMS_702           0x006c0400u
#define G3D_PMS_754           0x00740400u
#define G3D_PMS_806           0x007c0400u

static ULONG gFvmap;                    /* SRAM offset of the fvmap, 0 = not found */
static struct { BOOLEAN On; ULONG PmsOff, Pms0, RateOff, Rate0, TabOff; UCHAR Tab0; } gOc;

static UCHAR R8(ULONG Off) { return READ_REGISTER_UCHAR((volatile UCHAR *)(gSram + Off)); }
static USHORT R16(ULONG Off) { return READ_REGISTER_USHORT((volatile USHORT *)(gSram + Off)); }
static BOOLEAN SramIn(ULONG Off, ULONG Len) { return Len <= ACPM_SRAM_SIZE && Off <= ACPM_SRAM_SIZE - Len; }

/* plugins_init (kernel acpm.c): the attached DVFS plugin's base + fvmap_offset */
static VOID FvmapLocate(VOID)
{
    ULONG plugins = R32(gSram + ACPM_INITDATA_OFFSET + ACPM_FW_PLUGINS);
    ULONG n = R32(gSram + ACPM_INITDATA_OFFSET + ACPM_FW_NUM_PLUGINS), i, k;

    WriteDword(L"AcpmPlugins", n);
    if (n == 0 || n > 64 || !SramIn(plugins, n * PLUGIN_STRIDE))
        return;
    for (i = 0; i < n; i++) {
        ULONG e = plugins + i * PLUGIN_STRIDE, name = R32(gSram + e + PLUGIN_FW_NAME), base;
        CHAR nm[33];
        BOOLEAN dvfs = FALSE;
        if (R8(e + PLUGIN_IS_ATTACHED) != 1 || R8(e + PLUGIN_STAY_ATTACHED) != 1 || name == 0 || !SramIn(name, 32))
            continue;
        for (k = 0; k < 32; k++) {
            nm[k] = (CHAR)R8(name + k);
            if (nm[k] == 0)
                break;
        }
        nm[k] = 0;
        for (k = 0; nm[k] != 0 && nm[k + 1] != 0 && nm[k + 2] != 0 && nm[k + 3] != 0; k++)
            if ((nm[k] | 0x20) == 'd' && (nm[k + 1] | 0x20) == 'v' && (nm[k + 2] | 0x20) == 'f' && (nm[k + 3] | 0x20) == 's')
                dvfs = TRUE;
        if (!dvfs)
            continue;
        base = (R32(gSram + e + PLUGIN_BASE_ADDR) & ~1u) + FVMAP_OFFSET;
        WriteDword(L"FvmapPlugin", i);
        if (!SramIn(base, FVMAP_SIZE))
            return;
        gFvmap = base;
        WriteDword(L"FvmapOff", base);
        {
            /* the whole fvmap, read-only, for the record (notes: verify the layout offline) */
            ULONG *copy = (ULONG *)ExAllocatePool2(POOL_FLAG_PAGED, FVMAP_SIZE, 'rPeS');
            if (copy != NULL) {
                for (k = 0; k < FVMAP_SIZE / 4; k++)
                    copy[k] = R32(gSram + base + k * 4);
                (VOID)RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, gKey, L"FvmapDump", REG_BINARY, copy, FVMAP_SIZE);
                ExFreePoolWithTag(copy, 'rPeS');
            }
        }
        return;
    }
}

/* fvmap_init_gpu_oc's layout checks. Returns 0 when every one passes, else the first failed check's number
 * (G3dOcCheck in the service key). Fills the offsets the patch writes. */
static ULONG G3dOcCheck(ULONG *PmsOff, ULONG *RateOff, ULONG *TabOff)
{
    ULONG h = gFvmap + S10EPOWER_DOM_G3D * FVH_SIZE, rv, tab, pll, lv, i, j, prev = 0xFFFFFFFFu;

    if (gFvmap == 0) return 1;
    if (R8(h + FVH_NUM_LV) != 12 || R8(h + FVH_NUM_MEMBERS) != 1 || R8(h + FVH_NUM_PLL) != 1) return 2;
    rv = gFvmap + R16(h + FVH_O_RATEVOLT);
    tab = gFvmap + R16(h + FVH_O_TABLES);
    if (R16(h + FVH_O_RATEVOLT) + 12 * 8 > FVMAP_SIZE || R16(h + FVH_O_TABLES) + 12 > FVMAP_SIZE ||
        R16(h + FVH_O_MEMBERS) + 2 > FVMAP_SIZE || (rv & 3) != 0) return 3;
    WriteDword(L"G3dTopRate", R32(gSram + rv)); WriteDword(L"G3dTopVolt", R32(gSram + rv + 4));
    if (R32(gSram + rv) != 702000 || R32(gSram + rv + 4) < 600000 || R32(gSram + rv + 4) > 800000) return 4;
    for (i = 0; i < 12; i++) {                          /* rates strictly descending */
        ULONG rate = R32(gSram + rv + i * 8);
        if (rate >= prev || rate == 0) return 5;
        prev = rate;
        if (R8(tab + i) != i + 1) return 6;              /* level i -> PLL slot i + 1 (slot 0 unused) */
    }
    for (j = 0; j < ARRAYSIZE(gLadderG3d); j++) {       /* every DT step is an ACPM level */
        BOOLEAN found = FALSE;
        for (i = 0; i < 12; i++)
            if (R32(gSram + rv + i * 8) == gLadderG3d[j]) found = TRUE;
        if (!found) return 7;
    }
    if ((R16(h + FVH_O_MEMBERS) & 1) != 0) return 8;
    pll = R16(gFvmap + R16(h + FVH_O_MEMBERS));         /* the member's pll_header offset */
    if (pll + PLLH_PMS + 13 * 4 > FVMAP_SIZE || ((gFvmap + pll) & 3) != 0) return 8;
    pll += gFvmap;
    lv = R16(pll + PLLH_LEVEL);
    WriteDword(L"G3dPllLevels", lv);
    WriteDword(L"G3dPms0", R32(gSram + pll + PLLH_PMS)); WriteDword(L"G3dPms1", R32(gSram + pll + PLLH_PMS + 4));
    if (lv != 13) return 9;
    if (R32(gSram + pll + PLLH_PMS + 4) != G3D_PMS_702) return 10;
    if (R32(gSram + pll + PLLH_PMS) != G3D_PMS_754 && R32(gSram + pll + PLLH_PMS) != G3D_PMS_806) return 11;
    *PmsOff = pll + PLLH_PMS; *RateOff = rv; *TabOff = tab;
    return 0;
}

/* GpuOc = 754000 / 806000 in the service key (AllowSend and the TMU guard required) */
static VOID G3dOcApply(VOID)
{
    ULONG want = ReadDword(L"GpuOc", 0), pmsOff = 0, rateOff = 0, tabOff = 0, check, pms;

    WriteDword(L"GpuOcActive", 0);
    check = G3dOcCheck(&pmsOff, &rateOff, &tabOff);
    WriteDword(L"G3dOcCheck", check);
    if (want == 0)
        return;
    if (check != 0 || (want != 754000 && want != 806000) || !gState.AllowSend || !gDvfs.Ok || !gTmu.Ok) {
        WriteDword(L"GpuOcStatus", check != 0 ? 0x100 | check : !gTmu.Ok ? 3 : 2);   /* not applied */
        return;
    }
    pms = want == 806000 ? G3D_PMS_806 : G3D_PMS_754;
    gOc.PmsOff = pmsOff; gOc.Pms0 = R32(gSram + pmsOff);
    gOc.RateOff = rateOff; gOc.Rate0 = R32(gSram + rateOff);
    gOc.TabOff = tabOff; gOc.Tab0 = R8(tabOff);
    ExAcquireFastMutex(&gAcpmLock);                     /* no DVFS request while the table changes */
    if (gOc.Pms0 != pms)
        W32(gSram + pmsOff, pms);
    W32(gSram + rateOff, want);
    WRITE_REGISTER_UCHAR((volatile UCHAR *)(gSram + tabOff), 0);
    KeMemoryBarrier();
    if (R32(gSram + pmsOff) != pms || R32(gSram + rateOff) != want || R8(tabOff) != 0) {
        W32(gSram + pmsOff, gOc.Pms0); W32(gSram + rateOff, gOc.Rate0);
        WRITE_REGISTER_UCHAR((volatile UCHAR *)(gSram + tabOff), gOc.Tab0);
        KeMemoryBarrier();
        ExReleaseFastMutex(&gAcpmLock);
        WriteDword(L"GpuOcStatus", 4);                   /* read-back mismatch, restored */
        return;
    }
    gOc.On = TRUE;
    gLadderG3d[0] = want;
    ExReleaseFastMutex(&gAcpmLock);
    WriteDword(L"GpuOcActive", want);
    WriteDword(L"GpuOcStatus", 1);                       /* applied */
}

/* unload: off the patched level, then the original three values back */
static VOID G3dOcRestore(VOID)
{
    if (!gOc.On)
        return;
    (VOID)SetFreq(S10EPOWER_DOM_G3D, 650000);
    ExAcquireFastMutex(&gAcpmLock);
    W32(gSram + gOc.PmsOff, gOc.Pms0); W32(gSram + gOc.RateOff, gOc.Rate0);
    WRITE_REGISTER_UCHAR((volatile UCHAR *)(gSram + gOc.TabOff), gOc.Tab0);
    KeMemoryBarrier();
    gOc.On = FALSE;
    gLadderG3d[0] = 702000;
    ExReleaseFastMutex(&gAcpmLock);
    WriteDword(L"GpuOcActive", 0);
}

/* ---- CPU levels from the fvmap (read-only) + the LITTLE 2106000 allowance. fvmap_validate_oc_opp +
 * exynos9820_cluster_oc_opp_valid (YuROM exynos9820-oc.h): the stock top and the target are both firmware levels, rates
 * strictly descending, stock voltage 500..1250 mV, target voltage >= stock and <= the ceiling (LITTLE 2106000: 1175 mV,
 * MID 2400000: 1050 mV). YuROM also compares against ECT, which Windows cannot read; instead every DT ladder step must
 * be a level of the same header (proves the domain index). Returns 0 = OK, else the failed check (registry). ---- */
static ULONG CpuOcCheck(ULONG Dom, const ULONG *Dt, ULONG DtCount, ULONG Stock, ULONG Target, ULONG CeilUv,
                        PCWSTR LevelsName, ULONG *TargetUv)
{
    ULONG h = gFvmap + Dom * FVH_SIZE, n, rvRel, rv, i, j, prev = 0xFFFFFFFFu, stockUv = 0, tgtUv = 0;
    BOOLEAN stock = FALSE, tgt = FALSE;
    ULONG pairs[64];

    *TargetUv = 0;
    if (gFvmap == 0) return 1;
    n = R8(h + FVH_NUM_LV);
    rvRel = R16(h + FVH_O_RATEVOLT);
    if (n == 0 || n > 32) return 2;
    if (rvRel + n * 8 > FVMAP_SIZE || ((gFvmap + rvRel) & 3) != 0) return 3;
    rv = gFvmap + rvRel;
    for (i = 0; i < n; i++) {
        ULONG rate = R32(gSram + rv + i * 8), uv = R32(gSram + rv + i * 8 + 4);
        pairs[i * 2] = rate; pairs[i * 2 + 1] = uv;
        if (rate == 0 || rate >= prev) {
            (VOID)RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, gKey, LevelsName, REG_BINARY, pairs, (i + 1) * 8);
            return 5;
        }
        prev = rate;
        if (rate == Stock) { stock = TRUE; stockUv = uv; }
        if (rate == Target) { tgt = TRUE; tgtUv = uv; }
    }
    (VOID)RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, gKey, LevelsName, REG_BINARY, pairs, n * 8);
    for (j = 0; j < DtCount; j++) {
        BOOLEAN found = FALSE;
        for (i = 0; i < n; i++)
            if (R32(gSram + rv + i * 8) == Dt[j]) found = TRUE;
        if (!found) return 7;
    }
    if (!tgt) return 6;
    if (!stock || stockUv < 500000 || stockUv > 1250000) return 8;
    *TargetUv = tgtUv;
    if (tgtUv < stockUv || tgtUv > CeilUv) return 9;
    return 0;
}

/* CpuOc = 1 in the service key (AllowSend and the TMU guard required, like GpuOc) */
static VOID CpuOcApply(VOID)
{
    ULONG want = ReadDword(L"CpuOc", 0), uv = 0, check;

    gLittleOc = FALSE;
    WriteDword(L"CpuOcActive", 0);
    check = CpuOcCheck(S10EPOWER_DOM_LITTLE, gLadderLittle, ARRAYSIZE(gLadderLittle), 1950000, 2106000, 1175000,
                       L"LittleLevels", &uv);
    WriteDword(L"LittleOcCheck", check);
    WriteDword(L"LittleOcUv", uv);
    /* MID 2400000 is already in the ladder (MIF interlock, notes 2026-10-17); its YuROM check is recorded only */
    check = CpuOcCheck(S10EPOWER_DOM_MID, gLadderMid + 1, ARRAYSIZE(gLadderMid) - 1, 2314000, 2400000, 1050000,
                       L"MidLevels", &uv);
    WriteDword(L"MidOcCheck", check);
    WriteDword(L"MidOcUv", uv);
    if (want != 1)
        return;
    check = CpuOcCheck(S10EPOWER_DOM_LITTLE, gLadderLittle, ARRAYSIZE(gLadderLittle), 1950000, 2106000, 1175000,
                       L"LittleLevels", &uv);
    if (check != 0 || !gState.AllowSend || !gDvfs.Ok || !gTmu.Ok) {
        WriteDword(L"CpuOcStatus", check != 0 ? 0x100 | check : !gTmu.Ok ? 3 : 2);   /* not allowed */
        return;
    }
    gLittleOc = TRUE;
    WriteDword(L"CpuOcActive", 2106000);
    WriteDword(L"CpuOcStatus", 1);
}

/* ---- C0: the channel table dump ---- */
static VOID DumpChannels(VOID)
{
    ULONG tableOff = R32(gSram + ACPM_INITDATA_OFFSET + ACPM_FW_IPC_CHANNELS);
    ULONG apMax = R32(gSram + ACPM_INITDATA_OFFSET + ACPM_FW_IPC_AP_MAX), i, status = 0;
    WriteDword(L"AcpmTableOff", tableOff);
    WriteDword(L"AcpmApMax", apMax);
    if (apMax == 0 || apMax > ACPM_MAX_CHANNELS || !SramOk(tableOff, (ULONGLONG)apMax * ACPM_CH_STRIDE)) {
        status = 2;
    } else {
        ULONG *copy = (ULONG *)ExAllocatePool2(POOL_FLAG_PAGED, apMax * ACPM_CH_STRIDE, 'rPeS');
        if (copy == NULL) {
            status = 3;
        } else {
            for (i = 0; i < apMax * ACPM_CH_STRIDE / 4; i++)
                copy[i] = R32(gSram + tableOff + i * 4);
            (VOID)RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, gKey, L"AcpmChannels", REG_BINARY, copy,
                                        apMax * ACPM_CH_STRIDE);
            ExFreePoolWithTag(copy, 'rPeS');
        }
    }
    WriteDword(L"AcpmStatus", status);
}

static NTSTATUS S10ePowerCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS S10ePowerIoctl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(Irp);
    ULONG inLen = s->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outLen = s->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID buf = Irp->AssociatedIrp.SystemBuffer;
    NTSTATUS st = STATUS_SUCCESS;
    ULONG_PTR info = 0;

    UNREFERENCED_PARAMETER(DeviceObject);
    InterlockedIncrement(&gActive);
    if (gUnloading) {
        InterlockedDecrement(&gActive);
        Irp->IoStatus.Status = STATUS_DELETE_PENDING;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_DELETE_PENDING;
    }
    switch (s->Parameters.DeviceIoControl.IoControlCode) {
    case IOCTL_S10EPOWER_SET_FREQ:
        if (inLen < sizeof(S10EPOWER_SET_FREQ)) { st = STATUS_BUFFER_TOO_SMALL; break; }
        st = SetFreq(((S10EPOWER_SET_FREQ *)buf)->Domain, ((S10EPOWER_SET_FREQ *)buf)->Khz);
        if (!NT_SUCCESS(st))
            break;
        __fallthrough;
    case IOCTL_S10EPOWER_GET_STATE:
        if (outLen < S10EPOWER_STATE_V1_SIZE) { st = STATUS_BUFFER_TOO_SMALL; break; }
        info = min(outLen, sizeof(gState));   /* version-1 callers get the version-1 prefix */
        RtlCopyMemory(buf, &gState, info);
        break;
    default:
        st = STATUS_INVALID_DEVICE_REQUEST;
    }
    Irp->IoStatus.Status = st;
    Irp->IoStatus.Information = info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    InterlockedDecrement(&gActive);
    return st;
}

static VOID S10ePowerUnload(PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING dos;
    LARGE_INTEGER ms;
    UNREFERENCED_PARAMETER(DriverObject);
    InterlockedExchange(&gUnloading, 1);
    ms.QuadPart = -10000;
    while (gActive != 0)
        KeDelayExecutionThread(KernelMode, FALSE, &ms);   /* review fix (1): no IOCTL left touching the MMIO */
    if (gGuardThread != NULL) {
        KeSetEvent(&gStopEvent, IO_NO_INCREMENT, FALSE);
        KeWaitForSingleObject(gGuardThread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(gGuardThread);
        gGuardThread = NULL;
    }
    if (gMbox != NULL && gSram != NULL) {
        G3dOcRestore();
        if (gLittleOc && gState.CurKhz[0] > 1950000)
            (VOID)SetFreq(S10EPOWER_DOM_LITTLE, 1950000);   /* leave a stock rate for whoever comes next */
    }
    RtlInitUnicodeString(&dos, S10EPOWER_DOS_NAME);
    (VOID)IoDeleteSymbolicLink(&dos);
    if (gDevice != NULL)
        IoDeleteDevice(gDevice);
    if (gSram != NULL)
        MmUnmapIoSpace((PVOID)gSram, ACPM_SRAM_SIZE);
    if (gMbox != NULL)
        MmUnmapIoSpace((PVOID)gMbox, ACPM_MBOX_SIZE);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    PHYSICAL_ADDRESS phys;
    UNICODE_STRING name, dos, sddl;
    NTSTATUS st;
    ULONG i;
    static const GUID cls = { 0x5a10e9a1, 0x3c2d, 0x4f6e, { 0x9b, 0x21, 0x7e, 0x55, 0x1d, 0x0c, 0x8a, 0x42 } };

    DriverObject->DriverUnload = S10ePowerUnload;
    if (RegistryPath->Length >= sizeof(gKey))
        return STATUS_INVALID_PARAMETER;
    RtlCopyMemory(gKey, RegistryPath->Buffer, RegistryPath->Length);
    gKey[RegistryPath->Length / sizeof(WCHAR)] = 0;
    ExInitializeFastMutex(&gAcpmLock);
    KeInitializeEvent(&gStopEvent, NotificationEvent, FALSE);
    RtlZeroMemory(&gState, sizeof(gState));
    gState.Version = 2;
    for (i = 0; i < S10EPOWER_TZ_COUNT; i++)
        gState.TempC[i] = 0xFF;
    gState.CurKhz[0] = BOOT_LITTLE; gState.CurKhz[1] = BOOT_MID; gState.CurKhz[2] = BOOT_G3D;
    gState.CapKhz[0] = BOOT_LITTLE; gState.CapKhz[1] = BOOT_MID; gState.CapKhz[2] = BOOT_G3D;

    phys.QuadPart = (LONGLONG)ACPM_SRAM_BASE;
    gSram = (volatile UCHAR *)MmMapIoSpaceEx(phys, ACPM_SRAM_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (gSram == NULL) {
        WriteDword(L"AcpmStatus", 1);
        return STATUS_SUCCESS;   /* loaded, inert */
    }
    DumpChannels();
    FvmapLocate();   /* read-only */
    gState.AllowSend = ReadDword(L"AllowSend", 0) == 1;
    WriteDword(L"AllowSendActive", gState.AllowSend);

    if (gState.AllowSend) {
        phys.QuadPart = (LONGLONG)ACPM_MBOX_BASE;
        gMbox = (volatile UCHAR *)MmMapIoSpaceEx(phys, ACPM_MBOX_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
        if (gMbox == NULL) {
            gState.AllowSend = 0;
        } else {
            (VOID)FindChannel(ACPM_CH_DVFS, &gDvfs);
            (VOID)FindChannel(ACPM_CH_TMU, &gTmu);
            if (ReadDword(L"AllowTmu", 0) != 1)
                gTmu.Ok = FALSE;   /* temperature reads are enabled separately (C1 step) */
        }
        WriteDword(L"DvfsChannelOk", gDvfs.Ok);
        WriteDword(L"TmuChannelOk", gTmu.Ok);
        G3dOcApply();   /* before the guard thread: its caps come from gLadderG3d[0] */
        CpuOcApply();   /* likewise LITTLE_TOP() */
        if (gState.AllowSend) {
            HANDLE h;
            st = PsCreateSystemThread(&h, THREAD_ALL_ACCESS, NULL, NULL, NULL, GuardThread, NULL);
            if (NT_SUCCESS(st)) {
                (VOID)ObReferenceObjectByHandle(h, SYNCHRONIZE, *PsThreadType, KernelMode, &gGuardThread, NULL);
                ZwClose(h);
            }
        }
    }

    RtlInitUnicodeString(&name, S10EPOWER_DEVICE_NAME);
    RtlInitUnicodeString(&sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");   /* SYSTEM + administrators */
    st = IoCreateDeviceSecure(DriverObject, 0, &name, FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN, FALSE, &sddl,
                              &cls, &gDevice);
    if (NT_SUCCESS(st)) {
        RtlInitUnicodeString(&dos, S10EPOWER_DOS_NAME);
        (VOID)IoCreateSymbolicLink(&dos, &name);
        DriverObject->MajorFunction[IRP_MJ_CREATE] = S10ePowerCreateClose;
        DriverObject->MajorFunction[IRP_MJ_CLOSE] = S10ePowerCreateClose;
        DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = S10ePowerIoctl;
        gDevice->Flags &= ~DO_DEVICE_INITIALIZING;
    }
    return STATUS_SUCCESS;
}
