/*
 * Vibra.c - the Galaxy S10's vibration motor (LRA) through the MAX77705 haptic block + SoC PWM channel 1.
 * notes/s10-vibration.md; every register and value: S10eBattery.h "VIBRATION" block (live DT + LineageOS
 * exynos9820 kernel drivers/motor/max77705_haptic.c, sec_haptic.c, drivers/pwm/pwm-samsung.c).
 *
 * Lives in S10eBattery because the MAX77705 sits on HSI2C45, which this driver owns as a polled master: a second
 * driver on the same bus would race the battery poll. The PWM block, the PERIC0 pins and the CMU gate are mapped
 * here by their VERIFIED physical addresses (no ACPI resources - the UEFI's DSDT stays as it is).
 *
 * Safety:
 *  - the CMU is only READ: if the PWM's bus clock is not provably on (automatic gating, or manual + enabled), the
 *    PWM block is not touched at all (a gated SFR access can hang the bus);
 *  - the PWM input clock is measured, not assumed, with gpp3-1 still on its old function (no drive);
 *  - the motor runs at most S10E_VIB_MAX_MS and stops by a timer even if the caller goes away, and on D0Exit;
 *  - PMIC writes are exactly the kernel's (MAINCTRL1 BIASEN, MCONFIG LRA/EN/DIVIDER_128), read-modify-write.
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#include "S10eBattery.h"

EVT_WDF_TIMER         S10eVibTimerFn;
EVT_WDF_WORKITEM      S10eVibWorkFn;

static __forceinline ULONG VRd(_In_ volatile UCHAR *Base, _In_ ULONG Off)
{
    return READ_REGISTER_ULONG((volatile ULONG *)(Base + Off));
}

static __forceinline VOID VWr(_In_ volatile UCHAR *Base, _In_ ULONG Off, _In_ ULONG Val)
{
    WRITE_REGISTER_ULONG((volatile ULONG *)(Base + Off), Val);
}

NTSTATUS S10eVibCreate(_Inout_ PS10E_BATT_FDO Fdo)
{
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_WORKITEM_CONFIG   WorkConfig;
    WDF_TIMER_CONFIG      TimerConfig;
    NTSTATUS              Status;

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Fdo->Device;
    Status = WdfWaitLockCreate(&Attributes, &Fdo->VibLock);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    WDF_WORKITEM_CONFIG_INIT(&WorkConfig, S10eVibWorkFn);
    WorkConfig.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Fdo->Device;
    Status = WdfWorkItemCreate(&WorkConfig, &Attributes, &Fdo->VibWork);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    WDF_TIMER_CONFIG_INIT(&TimerConfig, S10eVibTimerFn);     /* one-shot */
    TimerConfig.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject   = Fdo->Device;
    Attributes.ExecutionLevel = WdfExecutionLevelDispatch;
    return WdfTimerCreate(&TimerConfig, &Attributes, &Fdo->VibTimer);
}

/* ------------------------------------------------------------------------------------------------ PMIC (I2C) */

/* Two-byte read: this bus's 1-byte reads come back all-zero (Hsi2c.c S10eMax77705ReadN). Caller holds BusLock. */
static NTSTATUS PmicRead(_Inout_ PS10E_BATT_FDO Fdo, _In_ UCHAR Reg, _Out_ PUCHAR Val)
{
    UCHAR    Buf[2] = { 0, 0 };
    NTSTATUS Status = S10eMax77705ReadN(Fdo, MAX77705_PMIC_ADDR, Reg, Buf, sizeof(Buf));

    *Val = Buf[0];
    return Status;
}

/* [K] max77705_update_reg(client, reg, val, mask): new = (old & ~mask) | (val & mask); written only on change,
   then read back. Caller holds BusLock. */
static NTSTATUS PmicUpdate(_Inout_ PS10E_BATT_FDO Fdo, _In_ UCHAR Reg, _In_ UCHAR Val, _In_ UCHAR Mask,
                           _Out_opt_ PUCHAR Before, _Out_opt_ PUCHAR After)
{
    UCHAR    Old;
    UCHAR    New;
    UCHAR    Back = 0;
    UCHAR    Write[2];
    NTSTATUS Status = PmicRead(Fdo, Reg, &Old);

    if (Before != NULL) { *Before = Old; }
    if (After != NULL)  { *After = Old; }
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    New = (UCHAR)((Old & ~Mask) | (Val & Mask));
    if (New != Old) {
        Write[0] = Reg;
        Write[1] = New;
        Status = S10eHsi2cXfer(Fdo, MAX77705_PMIC_ADDR, FALSE, Write, sizeof(Write), TRUE);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    }
    Status = PmicRead(Fdo, Reg, &Back);
    if (After != NULL) { *After = Back; }
    if (NT_SUCCESS(Status) && Back != New) {
        Status = STATUS_DEVICE_DATA_ERROR;
    }
    return Status;
}

/* ------------------------------------------------------------------------------------------------ PWM */

static NTSTATUS VibMap(_Inout_ PS10E_BATT_FDO Fdo)
{
    PHYSICAL_ADDRESS Pa;

    if (Fdo->VibCmu == NULL) {
        Pa.QuadPart = (LONGLONG)S10E_CMU_PERIC0_PAGE;
        Fdo->VibCmu = (volatile UCHAR *)MmMapIoSpaceEx(Pa, S10E_CMU_PERIC0_PAGE_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    }
    if (Fdo->VibPwm == NULL) {
        Pa.QuadPart = (LONGLONG)S10E_PWM_BASE;
        Fdo->VibPwm = (volatile UCHAR *)MmMapIoSpaceEx(Pa, S10E_PWM_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    }
    if (Fdo->VibGpio == NULL) {
        Pa.QuadPart = (LONGLONG)S10E_PERIC0_GPIO_BASE;
        Fdo->VibGpio = (volatile UCHAR *)MmMapIoSpaceEx(Pa, S10E_PERIC0_GPIO_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    }
    return (Fdo->VibCmu != NULL && Fdo->VibPwm != NULL && Fdo->VibGpio != NULL) ? STATUS_SUCCESS
                                                                                 : STATUS_INSUFFICIENT_RESOURCES;
}

VOID S10eVibUnmap(_Inout_ PS10E_BATT_FDO Fdo)
{
    if (Fdo->VibCmu != NULL)  { MmUnmapIoSpace((PVOID)Fdo->VibCmu, S10E_CMU_PERIC0_PAGE_SIZE); }
    if (Fdo->VibPwm != NULL)  { MmUnmapIoSpace((PVOID)Fdo->VibPwm, S10E_PWM_SIZE); }
    if (Fdo->VibGpio != NULL) { MmUnmapIoSpace((PVOID)Fdo->VibGpio, S10E_PERIC0_GPIO_SIZE); }
    Fdo->VibCmu = Fdo->VibPwm = Fdo->VibGpio = NULL;
    Fdo->VibTinHz = 0;
}

/* The PWM's bus clock must not be gated off. [K] cal-if ra.c ra_get_gate: "MANUAL 1 : CG_VALUE control, 0 :
   ENABLE_AUTOMATIC_CLKGATING control" - with MANUAL 0 the clock passes unless automatic gating is on, and automatic
   gating (HWACG) wakes it on access. Only MANUAL 1 + CG_VAL 0 is off. (1.0.0.8 also refused MANUAL 0 + AUTO 0 -
   the phone's state, gate 0x00200000, 2026-11-16 - which ra_get_gate reports as passing.) */
static BOOLEAN VibClockOn(_In_ ULONG Gate)
{
    return !(((Gate & S10E_CMU_GATE_MANUAL) != 0) && ((Gate & S10E_CMU_GATE_CG_VAL) == 0));
}

/* Channel 1 counts down from 0xFFFF once (no autoreload) while the pin is still on its old function; the counts
   in a ~100 us window against KeQueryPerformanceCounter give the input clock. Median of three. */
static NTSTATUS VibCalibrate(_Inout_ PS10E_BATT_FDO Fdo, _Inout_ S10E_VIBRATE_OUT *Out)
{
    volatile UCHAR *Pwm = Fdo->VibPwm;
    ULONG           Tcon;
    ULONG           Rate[3];
    ULONG           Counts = 0;
    ULONG           Ticks = 0;
    LARGE_INTEGER   Freq;
    ULONG           i;

    Out->Tcfg0      = VRd(Pwm, PWM_TCFG0);
    Out->Tcfg1      = VRd(Pwm, PWM_TCFG1);
    Out->TconBefore = VRd(Pwm, PWM_TCON);

    /* prescaler 0 (timers 0 and 1) to 1:1 only while timer 0 is idle; channel 1 mux to 1/1 (pwm-samsung div 0) */
    if ((Out->TconBefore & PWM_TCON0_START) == 0 && (Out->Tcfg0 & 0xFFu) != 0) {
        VWr(Pwm, PWM_TCFG0, Out->Tcfg0 & ~0xFFu);
    }
    VWr(Pwm, PWM_TCFG1, VRd(Pwm, PWM_TCFG1) & ~(0xFu << 4));

    KeQueryPerformanceCounter(&Freq);
    Out->QpcHz = (ULONG)Freq.QuadPart;
    for (i = 0; i < 3; i++) {
        KIRQL         OldIrql;
        LARGE_INTEGER Q0;
        LARGE_INTEGER Q1;
        ULONG         C0;
        ULONG         C1;

        Tcon = VRd(Pwm, PWM_TCON) & ~PWM_TCON1_ALL;
        VWr(Pwm, PWM_TCNTB1, PWM_COUNTER_MAX);
        VWr(Pwm, PWM_TCMPB1, 0);
        VWr(Pwm, PWM_TCON, Tcon | PWM_TCON1_MANUAL);
        VWr(Pwm, PWM_TCON, Tcon | PWM_TCON1_START);

        KeRaiseIrql(HIGH_LEVEL, &OldIrql);
        C0 = VRd(Pwm, PWM_TCNTO1);
        Q0 = KeQueryPerformanceCounter(NULL);
        KeStallExecutionProcessor(100);
        C1 = VRd(Pwm, PWM_TCNTO1);
        Q1 = KeQueryPerformanceCounter(NULL);
        KeLowerIrql(OldIrql);

        VWr(Pwm, PWM_TCON, Tcon);                     /* channel 1 stopped */
        if (C1 == 0 || C1 >= C0 || Q1.QuadPart <= Q0.QuadPart) {
            Out->CalCounts = C0;
            Out->CalTicks  = C1;
            return STATUS_DEVICE_NOT_READY;           /* not counting, or faster than 65535 per 100 us */
        }
        Counts = C0 - C1;
        Ticks  = (ULONG)(Q1.QuadPart - Q0.QuadPart);
        Rate[i] = (ULONG)(((ULONGLONG)Counts * (ULONGLONG)Freq.QuadPart) / Ticks);
    }
    Out->CalCounts = Counts;
    Out->CalTicks  = Ticks;
    /* median of three */
    if (Rate[0] > Rate[1]) { ULONG t = Rate[0]; Rate[0] = Rate[1]; Rate[1] = t; }
    if (Rate[1] > Rate[2]) { ULONG t = Rate[1]; Rate[1] = Rate[2]; Rate[2] = t; }
    if (Rate[0] > Rate[1]) { ULONG t = Rate[0]; Rate[0] = Rate[1]; Rate[1] = t; }
    Out->TinHz = Rate[1];
    if (Rate[1] < 1000000u || Rate[1] > 250000000u) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    Fdo->VibTinHz = Rate[1];
    return STATUS_SUCCESS;
}

/* [K] pwm-samsung __pwm_samsung_config + enable (manual update, autoreload, start); NORMAL polarity = invert bit. */
static NTSTATUS VibPwmRun(_Inout_ PS10E_BATT_FDO Fdo, _In_ ULONG DutyNs, _Inout_ S10E_VIBRATE_OUT *Out)
{
    volatile UCHAR *Pwm = Fdo->VibPwm;
    ULONGLONG       Tin = Fdo->VibTinHz;
    ULONG           Tcnt = (ULONG)(((ULONGLONG)S10E_VIB_PERIOD_NS * Tin + 500000000ull) / 1000000000ull);
    ULONG           DutyTicks = (ULONG)(((ULONGLONG)DutyNs * Tin + 500000000ull) / 1000000000ull);
    ULONG           Tcmp;
    ULONG           Tcon;

    if (Tcnt < 100 || Tcnt > PWM_COUNTER_MAX || DutyTicks > Tcnt) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;     /* period not representable with this input clock */
    }
    Tcmp = Tcnt - DutyTicks;
    if (Tcmp == Tcnt) {
        Tcmp--;
    }
    Tcnt--;
    Tcmp--;
    Out->Tcnt = Tcnt;
    Out->Tcmp = Tcmp;

    VWr(Pwm, PWM_TCNTB1, Tcnt);
    VWr(Pwm, PWM_TCMPB1, Tcmp);
    Tcon = (VRd(Pwm, PWM_TCON) & ~PWM_TCON1_ALL) | PWM_TCON1_INVERT;
    VWr(Pwm, PWM_TCON, Tcon | PWM_TCON1_MANUAL);
    VWr(Pwm, PWM_TCON, Tcon | PWM_TCON1_AUTORELOAD | PWM_TCON1_START);
    Out->TconAfter = VRd(Pwm, PWM_TCON);
    return STATUS_SUCCESS;
}

/* [K] set_intensity(0) -> duty = period/2 -> pwm_disable: autoreload off, the channel stops after this period. */
static VOID VibPwmStop(_Inout_ PS10E_BATT_FDO Fdo)
{
    if (Fdo->VibPwm != NULL && Fdo->VibTinHz != 0) {
        VWr(Fdo->VibPwm, PWM_TCON, VRd(Fdo->VibPwm, PWM_TCON) & ~PWM_TCON1_AUTORELOAD);
    }
}

/* gpp3-1 -> TOUT1 (pull-down), gpg1-7 -> output high (motor boost enable): the max77705 node's pinctrl-0. */
static VOID VibPins(_Inout_ PS10E_BATT_FDO Fdo, _Inout_ S10E_VIBRATE_OUT *Out)
{
    volatile UCHAR *G = Fdo->VibGpio;
    ULONG           V;

    V = VRd(G, GPP3_PUD);
    V = (V & ~(0xFu << (4 * VIB_PWM_PIN))) | (VIB_PWM_PUD << (4 * VIB_PWM_PIN));
    VWr(G, GPP3_PUD, V);
    V = VRd(G, GPP3_CON);
    V = (V & ~(0xFu << (4 * VIB_PWM_PIN))) | (VIB_PWM_FUNC << (4 * VIB_PWM_PIN));
    VWr(G, GPP3_CON, V);

    VWr(G, GPG1_DAT, VRd(G, GPG1_DAT) | (1u << VIB_BOOST_PIN));
    V = VRd(G, GPG1_CON);
    V = (V & ~(0xFu << (4 * VIB_BOOST_PIN))) | (0x1u << (4 * VIB_BOOST_PIN));
    VWr(G, GPG1_CON, V);

    Out->Gpp3Con = VRd(G, GPP3_CON);
    Out->Gpp3Pud = VRd(G, GPP3_PUD);
    Out->Gpg1Con = VRd(G, GPG1_CON);
    Out->Gpg1Dat = VRd(G, GPG1_DAT);
}

/* Motor off: MCONFIG LRA|EN off first, then the PWM to rest ([K] sec_haptic enable(false), set_intensity(0)).
   Caller holds VibLock. */
static VOID VibStopLocked(_Inout_ PS10E_BATT_FDO Fdo)
{
    if (!Fdo->VibOn) {
        return;
    }
    WdfWaitLockAcquire(Fdo->BusLock, NULL);
    if (Fdo->BusReady) {
        (VOID)PmicUpdate(Fdo, MAX77705_PMIC_MCONFIG, 0, MAX77705_MOTOR_LRA | MAX77705_MOTOR_EN, NULL, NULL);
    }
    WdfWaitLockRelease(Fdo->BusLock);
    VibPwmStop(Fdo);
    Fdo->VibOn = FALSE;
}

static NTSTATUS VibStart(_Inout_ PS10E_BATT_FDO Fdo, _In_ ULONG DurationMs, _In_ LONG Intensity,
                         _Inout_ S10E_VIBRATE_OUT *Out)
{
    ULONG    Half = S10E_VIB_PERIOD_NS / 2;
    ULONG    DutyNs;
    NTSTATUS Status;

    Out->Step = 1;
    Status = VibMap(Fdo);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    Out->Step   = 2;
    Out->CmuGate = VRd(Fdo->VibCmu, S10E_CMU_PWM_GATE_OFF);
    Out->CmuQch  = VRd(Fdo->VibCmu, S10E_CMU_PWM_QCH_OFF);
    if (!VibClockOn(Out->CmuGate)) {
        return STATUS_DEVICE_POWERED_OFF;              /* PWM bus clock gated: do not touch the block */
    }

    Out->Step = 3;
    if (Fdo->VibTinHz == 0) {
        Status = VibCalibrate(Fdo, Out);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    } else {
        Out->TinHz = Fdo->VibTinHz;
    }

    Out->Step = 4;
    VibPins(Fdo, Out);

    Out->Step = 5;
    DutyNs = Half + (ULONG)(((ULONGLONG)(S10E_VIB_MAX_DUTY_NS - Half) * (ULONG)Intensity) / S10E_VIB_MAX_INTENSITY);
    Status = VibPwmRun(Fdo, DutyNs, Out);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    Out->Step = 6;
    WdfWaitLockAcquire(Fdo->BusLock, NULL);
    if (!Fdo->BusReady) {
        Status = STATUS_DEVICE_NOT_READY;
    } else {
        if (!Fdo->VibPmicInit) {
            /* [K] max77705_haptic_init_reg(init_en = true) */
            Status = PmicUpdate(Fdo, MAX77705_PMIC_MAINCTRL1, 0xFF, MAX77705_BIASEN, &Out->MainCtrl1Before,
                                &Out->MainCtrl1);
            if (NT_SUCCESS(Status)) {
                Status = PmicUpdate(Fdo, MAX77705_PMIC_MCONFIG, 0x00, MAX77705_MOTOR_EN, &Out->MconfigBefore, NULL);
            }
            if (NT_SUCCESS(Status)) {
                Status = PmicUpdate(Fdo, MAX77705_PMIC_MCONFIG, 0xFF, MAX77705_MOTOR_LRA, NULL, NULL);
            }
            if (NT_SUCCESS(Status)) {
                Status = PmicUpdate(Fdo, MAX77705_PMIC_MCONFIG, 0xFF, MAX77705_DIVIDER_128, NULL, NULL);
            }
            Fdo->VibPmicInit = NT_SUCCESS(Status);
        }
        if (NT_SUCCESS(Status)) {
            /* [K] max77705_haptic_i2c(en = true) */
            Status = PmicUpdate(Fdo, MAX77705_PMIC_MCONFIG, 0xFF, MAX77705_MOTOR_LRA | MAX77705_MOTOR_EN, NULL,
                                &Out->Mconfig);
        }
    }
    WdfWaitLockRelease(Fdo->BusLock);
    Fdo->VibOn = TRUE;                                 /* even on a PMIC error: the stop path cleans both up */
    if (!NT_SUCCESS(Status)) {
        VibStopLocked(Fdo);
        return Status;
    }

    Out->Step = 7;
    Fdo->VibDeadline = KeQueryInterruptTime() + (ULONGLONG)DurationMs * 10000ull;
    WdfTimerStart(Fdo->VibTimer, WDF_REL_TIMEOUT_IN_MS(DurationMs));
    return STATUS_SUCCESS;
}

NTSTATUS S10eVibIoctl(_Inout_ PS10E_BATT_FDO Fdo, _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION Sp = IoGetCurrentIrpStackLocation(Irp);
    S10E_VIBRATE_IN    In;
    S10E_VIBRATE_OUT   Out;
    NTSTATUS           Status;

    PAGED_CODE();
    Irp->IoStatus.Information = 0;
    if (Sp->Parameters.DeviceIoControl.InputBufferLength < sizeof(In) ||
        Sp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(Out) || Irp->AssociatedIrp.SystemBuffer == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    In = *(S10E_VIBRATE_IN *)Irp->AssociatedIrp.SystemBuffer;
    if (In.DurationMs > S10E_VIB_MAX_MS || In.Intensity < 0 || In.Intensity > S10E_VIB_MAX_INTENSITY ||
        (In.DurationMs != 0 && In.Intensity == 0)) {
        return STATUS_INVALID_PARAMETER;
    }
    RtlZeroMemory(&Out, sizeof(Out));
    Out.Version = 1;

    WdfTimerStop(Fdo->VibTimer, FALSE);
    WdfWaitLockAcquire(Fdo->VibLock, NULL);
    VibStopLocked(Fdo);
    Status = (In.DurationMs == 0) ? STATUS_SUCCESS : VibStart(Fdo, In.DurationMs, In.Intensity, &Out);
    WdfWaitLockRelease(Fdo->VibLock);

    Out.Status = Status;
    RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, &Out, sizeof(Out));
    Irp->IoStatus.Information = sizeof(Out);
    return STATUS_SUCCESS;                             /* the result is in Out.Status, so the caller sees the steps */
}

VOID S10eVibShutdown(_Inout_ PS10E_BATT_FDO Fdo)
{
    if (Fdo->VibLock == NULL) {
        return;
    }
    WdfTimerStop(Fdo->VibTimer, TRUE);
    WdfWorkItemFlush(Fdo->VibWork);
    WdfWaitLockAcquire(Fdo->VibLock, NULL);
    VibStopLocked(Fdo);
    Fdo->VibPmicInit = FALSE;                          /* re-init after a power transition */
    WdfWaitLockRelease(Fdo->VibLock);
}

_Use_decl_annotations_
VOID S10eVibTimerFn(WDFTIMER Timer)
{
    PS10E_BATT_FDO Fdo = S10eBattGetFdo(WdfTimerGetParentObject(Timer));

    WdfWorkItemEnqueue(Fdo->VibWork);
}

_Use_decl_annotations_
VOID S10eVibWorkFn(WDFWORKITEM WorkItem)
{
    PS10E_BATT_FDO Fdo = S10eBattGetFdo(WdfWorkItemGetParentObject(WorkItem));

    WdfWaitLockAcquire(Fdo->VibLock, NULL);
    /* a timer that fired for an earlier run (stopped too late in the IOCTL) must not cut the new run short:
       stop only once the current run's end is reached (2 ms timer slack) */
    if (KeQueryInterruptTime() + 20000ull >= Fdo->VibDeadline) {
        VibStopLocked(Fdo);
    } else {
        WdfTimerStart(Fdo->VibTimer, WDF_REL_TIMEOUT_IN_US((Fdo->VibDeadline - KeQueryInterruptTime()) / 10 + 1));
    }
    WdfWaitLockRelease(Fdo->VibLock);
}
