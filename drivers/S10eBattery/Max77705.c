/*
 * Max77705.c - one complete, READ-ONLY reading of the MAX77705 fuel gauge (0x36) and charger status (0x69).
 *
 * Conversions are the vendor's, verbatim (drivers/battery_v2/max77705_fuelgauge.c), exactly as the device-run
 * UEFI PoC does them (Beyond0LteUsbHostDxe/FuelGauge.c). No gauge register is ever written here; the charger gets
 * the watchdog kick (CNFG_06 WDTCLR) and, with ChargeTune 1 (1.0.0.10), CHGIN_ILIM / CHG_CC (S10eMax77705Tune).
 * Every register is read as a 2-byte transfer (the 1-byte-read bug on this bus, Pmic.c:Max77705RegRead).
 * Caller holds Fdo->BusLock.
 */

#include "S10eBattery.h"

/* Vendor max77705_bulk_read(reg, 2): w = (data[1] << 8) | data[0]. */
static NTSTATUS FgReadWord(_Inout_ PS10E_BATT_FDO Fdo, _In_ UCHAR Reg, _Out_ PUSHORT Value)
{
    NTSTATUS Status;
    UCHAR    Data[2];

    *Value = 0;
    Status = S10eMax77705ReadN(Fdo, MAX77705_FG_ADDR, Reg, Data, sizeof(Data));
    if (NT_SUCCESS(Status)) {
        *Value = (USHORT)((Data[1] << 8) | Data[0]);
    }

    return Status;
}

/* max77705_fg_read_soc: ((hi * 100) + (lo * 100 / 256)) / 10, clamped to 1000 (0.1 % units). */
static ULONG FgSocTenths(_In_ USHORT w)
{
    ULONG Soc;

    Soc = (((ULONG)(w >> 8) * 100) + ((ULONG)(w & 0xFF) * 100 / 256)) / 10;
    return (Soc > 1000) ? 1000 : Soc;
}

/* max77705_fg_read_vcell, verbatim (FuelGauge.c FgReadVcell). */
static ULONG FgVcellMv(_In_ USHORT w)
{
    ULONGLONG Temp;
    ULONG     Mv;

    Temp = (ULONGLONG)(w & 0xFFF) * 78125;
    Mv   = (ULONG)(Temp / 1000000);
    Temp = (ULONGLONG)((w & 0xF000) >> 4) * 78125;
    Temp /= 1000000;
    Mv  += (ULONG)(Temp << 4);
    return Mv;
}

/* max77705_fg_read_avg_current: signed 16-bit, magnitude * 15625 * fg_resistor / 100000 mA. */
static LONG FgAvgCurrentMa(_In_ USHORT w)
{
    ULONG   Temp;
    BOOLEAN Negative;

    Temp     = w;
    Negative = (Temp & 0x8000) != 0;
    if (Negative) {
        Temp = (~Temp & 0xFFFF) + 1;
    }

    Temp = Temp * 15625 * MAX77705_FG_RESISTOR_UNVERIFIED / 100000;
    return Negative ? -(LONG)Temp : (LONG)Temp;
}

/* max77705_fg_read_temp: milli-degC (hi signed whole degC * 1000 + lo * 39 / 10). */
static LONG FgTempMilliC(_In_ USHORT w)
{
    LONG Hi = (LONG)(w >> 8);
    LONG Lo = (LONG)(w & 0xFF);
    LONG Temper;

    if ((Hi & 0x80) != 0) {
        Temper  = ((~Hi) & 0xFF) + 1;
        Temper *= -1000;
        Temper -= ((~Lo) + 1) * 39 / 10;
    } else {
        Temper  = (Hi & 0x7F) * 1000;
        Temper += Lo * 39 / 10;
    }

    return Temper;
}

/* Capacity registers: mAh = word * fg_resistor / 2 (vendor max77705_fg_read_*cap*). */
static ULONG FgCapMah(_In_ USHORT w)
{
    return (ULONG)w * MAX77705_FG_RESISTOR_UNVERIFIED / 2;
}

NTSTATUS S10eMax77705Read(_Inout_ PS10E_BATT_FDO Fdo, _Out_ PS10E_BATT_READING Reading)
{
    NTSTATUS Status;
    USHORT   w;
    UCHAR    Chg[2];

    RtlZeroMemory(Reading, sizeof(*Reading));

    Status = FgReadWord(Fdo, MAX77705_FG_STATUS, &w);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Reading->FgStatus = w;
    if ((w & MAX77705_FG_STATUS_ABSENT) != 0) {
        return STATUS_NO_MEDIA_IN_DEVICE;
    }

    Status = FgReadWord(Fdo, MAX77705_FG_SOCREP, &w);
    if (!NT_SUCCESS(Status)) { return Status; }
    Reading->SocTenths = FgSocTenths(w);

    Status = FgReadWord(Fdo, MAX77705_FG_VCELL, &w);
    if (!NT_SUCCESS(Status)) { return Status; }
    Reading->VcellMv = FgVcellMv(w);

    Status = FgReadWord(Fdo, MAX77705_FG_AVG_CURRENT, &w);
    if (!NT_SUCCESS(Status)) { return Status; }
    Reading->AvgCurrentMa = FgAvgCurrentMa(w);

    Status = FgReadWord(Fdo, MAX77705_FG_TEMPERATURE, &w);
    if (!NT_SUCCESS(Status)) { return Status; }
    Reading->TempMilliC = FgTempMilliC(w);

    Status = FgReadWord(Fdo, MAX77705_FG_REMCAP_REP, &w);
    if (!NT_SUCCESS(Status)) { return Status; }
    Reading->RemCapMah = FgCapMah(w);

    Status = FgReadWord(Fdo, MAX77705_FG_FULLCAP_REP, &w);
    if (!NT_SUCCESS(Status)) { return Status; }
    Reading->FullCapMah = FgCapMah(w);

    Status = FgReadWord(Fdo, MAX77705_FG_DESIGNCAP, &w);
    if (!NT_SUCCESS(Status)) { return Status; }
    Reading->DesignCapMah = FgCapMah(w);

    /* Plausibility gate (S10eBattery.h): garbage must never reach Windows as a real percentage. */
    if ((Reading->VcellMv < S10E_PLAUSIBLE_VCELL_MIN_MV) || (Reading->VcellMv > S10E_PLAUSIBLE_VCELL_MAX_MV) ||
        ((Reading->FullCapMah == 0) && (Reading->DesignCapMah == 0))) {
        return STATUS_DEVICE_DATA_ERROR;
    }

    /* Charger: CHG_DETAILS_00 (0xB3) and CHG_DETAILS_01 (0xB4) in one 2-byte auto-increment read. A charger
       failure does not fail the reading: the gauge data stays valid, AC is then reported as unknown/offline. */
    if (NT_SUCCESS(S10eMax77705ReadN(Fdo, MAX77705_CHG_ADDR, MAX77705_CHG_DETAILS_00, Chg, sizeof(Chg)))) {
        Reading->ChgDetails00 = Chg[0];
        Reading->ChgDetails01 = Chg[1];
        Reading->ChargerRead  = TRUE;
    }

    /* Kick the charger watchdog every full reading (20 s; the WDT is tens of seconds).
       Read-modify-write of WDTCLR only; a failure leaves the reading intact. */
    if (NT_SUCCESS(S10eMax77705ReadN(Fdo, MAX77705_CHG_ADDR, MAX77705_CHG_CNFG_00, Chg, sizeof(Chg)))) {
        Reading->ChgCnfg00  = Chg[0];
        Reading->Cnfg00Read = TRUE;
    }
    Reading->ChgCnfg02 = 0xFF;
    Reading->ChgCnfg09 = 0xFF;
    if (NT_SUCCESS(S10eMax77705ReadN(Fdo, MAX77705_CHG_ADDR, MAX77705_CHG_CNFG_02, Chg, sizeof(Chg)))) {
        Reading->ChgCnfg02 = Chg[0];
    }
    if (NT_SUCCESS(S10eMax77705ReadN(Fdo, MAX77705_CHG_ADDR, MAX77705_CHG_CNFG_09, Chg, sizeof(Chg)))) {
        Reading->ChgCnfg09 = Chg[0];
    }
    if (NT_SUCCESS(S10eMax77705ReadN(Fdo, MAX77705_CHG_ADDR, MAX77705_CHG_CNFG_06, Chg, sizeof(Chg)))) {
        UCHAR Write[2];

        Reading->ChgCnfg06 = Chg[0];
        Write[0] = MAX77705_CHG_CNFG_06;
        Write[1] = (UCHAR)((Chg[0] & ~MAX77705_CHG_CNFG_06_WDTCLR_MASK) | MAX77705_CHG_CNFG_06_WDTCLR);
        Reading->WdtKicked = NT_SUCCESS(S10eHsi2cXfer(Fdo, MAX77705_CHG_ADDR, FALSE, Write, sizeof(Write), TRUE));
    }

    /* G18 H1: USB-C / PD block snapshot, read-only, revisions + status registers only (no interrupt latches). A
       failure leaves the battery reading intact. */
    if (NT_SUCCESS(S10eMax77705ReadN(Fdo, MAX77705_USBC_ADDR, MAX77705_USBC_HW_REV, Reading->UsbcRev,
                                     sizeof(Reading->UsbcRev))) &&
        NT_SUCCESS(S10eMax77705ReadN(Fdo, MAX77705_USBC_ADDR, MAX77705_USBC_STATUS1, Reading->UsbcStatus,
                                     sizeof(Reading->UsbcStatus)))) {
        Reading->UsbcRead = TRUE;
    }

    return STATUS_SUCCESS;
}

/* One charger register read-modify-write: only the bits in Mask change. */
static NTSTATUS ChgUpdate(_Inout_ PS10E_BATT_FDO Fdo, _In_ UCHAR Reg, _In_ UCHAR Value, _In_ UCHAR Mask)
{
    NTSTATUS Status;
    UCHAR    Cur[2];
    UCHAR    Write[2];

    Status = S10eMax77705ReadN(Fdo, MAX77705_CHG_ADDR, Reg, Cur, sizeof(Cur));
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Write[0] = Reg;
    Write[1] = (UCHAR)((Cur[0] & ~Mask) | (Value & Mask));
    if (Write[1] == Cur[0]) {
        return STATUS_SUCCESS;
    }
    Status = S10eHsi2cXfer(Fdo, MAX77705_CHG_ADDR, FALSE, Write, sizeof(Write), TRUE);
    if (NT_SUCCESS(Status)) {
        Fdo->TuneWrites++;
    }
    return Status;
}

/* ChargeTune (1.0.0.10): what the vendor charger stack does per cable type, for the three BC1.2 types the MAX77705
   USB-C block reports (S10eBattery.h S10E_TUNE_*). Only with ChargeTune 1, only while CHGIN is valid, never the
   charger mode (CNFG_00). A charger without a BC1.2 result (type 0) keeps whatever the chip has. */
VOID S10eMax77705Tune(_Inout_ PS10E_BATT_FDO Fdo, _In_ PS10E_BATT_READING R)
{
    UCHAR Bc;
    ULONG Ilim;
    ULONG Cc;
    UCHAR IlimReg;
    UCHAR CcReg;

    Fdo->TuneIlimMa = 0;
    Fdo->TuneCcMa   = 0;
    if (!R->ChargerRead || (MAX77705_CHGIN_DTLS(R->ChgDetails00) != MAX77705_CHGIN_DTLS_VALID) || !R->UsbcRead) {
        Fdo->TunePending = 0;
        Fdo->TuneArmed   = FALSE;
        Fdo->TuneHot     = FALSE;
        return;
    }
    Bc = R->UsbcStatus[MAX77705_USBC_BC_STATUS - MAX77705_USBC_STATUS1];
    Fdo->TuneBc = Bc;
    switch (MAX77705_BC_CHGTYP(Bc)) {
    case 1:  Ilim = S10E_TUNE_USB_ILIM_MA; Cc = S10E_TUNE_USB_CC_MA; break;
    case 2:  Ilim = S10E_TUNE_CDP_ILIM_MA; Cc = S10E_TUNE_CDP_CC_MA; break;
    case 3:  Ilim = S10E_TUNE_TA_ILIM_MA;  Cc = S10E_TUNE_TA_CC_MA;  break;
    default:                                     /* detection still running (or none): look again next tick, */
        if (!Fdo->TuneArmed) {                   /* for at most 10 ticks per plug-in */
            Fdo->TuneArmed   = TRUE;
            Fdo->TunePending = S10E_TUNE_PENDING_TICKS;
        }
        return;
    }
    Fdo->TunePending = 0;

    if (R->TempMilliC >= S10E_TUNE_HOT_MILLIC) {
        Fdo->TuneHot = TRUE;
    } else if (R->TempMilliC < S10E_TUNE_COOL_MILLIC) {
        Fdo->TuneHot = FALSE;
    }
    if (Fdo->TuneHot) {
        Ilim = S10E_TUNE_HOT_ILIM_MA;
        Cc   = S10E_TUNE_HOT_CC_MA;
    }
    Fdo->TuneIlimMa = Ilim;
    Fdo->TuneCcMa   = Cc;
    if (Fdo->TuneSetting == 0 || R->ChgCnfg02 == 0xFF || R->ChgCnfg09 == 0xFF) {
        return;                                  /* report the decision only */
    }

    IlimReg = (UCHAR)(Ilim / 25u - 1u);          /* vendor: (I / 25) - 1, I >= 100 */
    CcReg   = (UCHAR)(Cc / 50u);                 /* vendor: I / 50, I >= 100 */
    if (((R->ChgCnfg09 & MAX77705_CHG_CHGIN_LIM_MASK) == IlimReg) && ((R->ChgCnfg02 & MAX77705_CHG_CC_MASK) == CcReg)) {
        return;
    }
    /* Unlock the settings first (CHGPROT = 11), as max77705_charger_unlock does (update of bits 3:2 only). */
    if (!NT_SUCCESS(ChgUpdate(Fdo, MAX77705_CHG_CNFG_06, MAX77705_CHG_CNFG_06_CHGPROT, MAX77705_CHG_CNFG_06_CHGPROT))) {
        return;
    }
    (VOID)ChgUpdate(Fdo, MAX77705_CHG_CNFG_09, IlimReg, MAX77705_CHG_CHGIN_LIM_MASK);
    (VOID)ChgUpdate(Fdo, MAX77705_CHG_CNFG_02, CcReg, MAX77705_CHG_CC_MASK);
}
