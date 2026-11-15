/*
 * S10eCam.c - Galaxy S10 (beyond1lte) camera bring-up, stage by stage (notes/s10-camera.md). A legacy kernel service
 * like S10ePower (sc create ... type= kernel start= demand), driven by tools/s10-kit/cam-s10.ps1; nothing happens
 * until an IOCTL asks for a stage, and the driver touches nothing when it is loaded or unloaded.
 *
 * Every address and value below is this phone's: its live device tree (dump/s10-dump/dt/live.dts, "DT") and the
 * LineageOS exynos9820 kernel lineage-23.0 ("K"; fimc-is2 camera framework, cal-if exynos9820, s2mpb02 regulator,
 * i2c-exynos5, pinctrl-exynos-arm64). The staged approach follows the Galaxy S9 (Exynos 9810) front-camera bring-up
 * of Scion's HakiOS fork (hakios.exynos_camera.c, APSL - read as a map of steps, no code taken; the 9820's
 * addresses are all different).
 *
 * THE FRONT CAMERA: S5K3J1 (DT fimc-is-3j1@10, "samsung,exynos5-fimc-is-cis-3j1", rev_reg <0x2 0x2 0xA101>)
 *   sensor I2C     0x10 on hsi2c@10880000 ("PERI1 CAM2" = I2C_CAM2, no USI, samsung,usi-i2c-v2 -> USI_CON reset),
 *                  pins gpc0-4 SCL / gpc0-5 SDA of PERIC1 (0x10830000 + 0x060): function 2 "on_i2c" (hsi2c2-bus),
 *                  function 0 "off_i2c" (hsi2c2-bus-in)
 *   reset          gpg2-5 of PERIC0 (0x10430000 + 0x0C0), output, high = running (gpio_reset <0x96 5 1>)
 *   subcam_sel     gpg4-3 of PERIC0 (0x10430000 + 0x0E0), output 0 (gpio_subcam_sel <0x17B 3 1>; the module file
 *                  writes 0 although its label says "high")
 *   MCLK           CIS_CLK1 (mclk_ch 1) of CMU_TOP 0x1A240000 (K cmucal-sfr.c): MUX_CLKCMU_CIS_CLK1 0x1014 [0]
 *                  (0 OSCCLK 26 MHz, 1 PLL_SHARED2_DIV2), DIV_CLKCMU_CIS_CLK1 0x1810 [4:0], GATE_CLKCMU_CIS_CLK1 0x202C
 *                  (21 CG_VAL, 20 MANUAL, 28 auto), DMYQCH DFTMUX_TOP_QCH_CIS_CLK1 0x300C (0 ENABLE, 1 CLOCK_REQ).
 *                  Android asks 26 MHz (exynos9820_fimc_is_sensor_mclk_on: set_rate 26 MHz) = OSCCLK undivided.
 *                  Out on gpc1-5 (PERIC1 + 0x080): function 2 "sensor-mclk1-fn" (pud 0), off: function 1
 *                  "sensor-mclk1-out" (pud 1)
 *   supplies       S2MPB02 (0x59 on hsi2c@10920000 = USI10_USI, SYSREG_PERIC1 USI10 mode 0x10821038 = I2C, pins
 *                  gpp6-0/1 function 2 "hsi2c24-bus"). K s2mpb02-regulator.h: register = enum index; LDO control:
 *                  bit 7 on, [6:0] = (uV - 600000) / step (12.5 mV LDO1-5, 25 mV LDO6-18); BUCK2 B2CTRL1 0x0D [7:6]
 *                  enable (mask 0xC0), B2CTRL2 0x0E = (uV - 400000) / 6250. DT names (min = max):
 *                    VDD_EXT_1P3_PB02  BUCK2  1.30 V  B2CTRL2 0x90
 *                    VDDD_1.05V_VT     LDO5   1.05 V  L5CTRL  0x22 = 0x80 | 0x24
 *                    VDDIO_1.8V_VT     LDO6   1.80 V  L6CTRL  0x23 = 0x80 | 0x30
 *                    VDDA_2.95V_VT     LDO13  2.95 V  L13CTRL 0x2A = 0x80 | 0x5E
 *                    VDDAF_2.8V_VT     LDO16  2.80 V  L16CTRL 0x2D = 0x80 | 0x58
 *   power on       K fimc-is-device-module-3j1.c, beyond1 = CONFIG_CAMERA_BSS_V01 -> USE_BUCK2_REGULATOR_CONTROL,
 *                  no TOF IO, power_seq_id 0 (absent from the DT): BUCK2 on; subcam_sel 0, 2 ms; VDDAF; VDDA; VDDIO;
 *                  VDDD, 1 ms; on_i2c, 10 us; reset high; MCLK pin function 2; MCLK on, 9 ms
 *   power off      subcam_sel 0, 9 us; MCLK off, 1 us; MCLK pin function 1 (out); reset low; off_i2c; VDDAF off;
 *                  VDDD off; VDDA off; VDDIO off; BUCK2 off. CIS_CLK1's registers put back as found.
 *   identity       16-bit register addresses: model ID 0x0000 (16 bit), revision 0x0002 (16 bit: 0xA101 per DT),
 *                  frame count 0x0005 (0xFF = not streaming) (K fimc-is-cis-3j1.c sensor_3j1_cis_check_rev)
 *
 * Bus clocks are only READ (CMU_PERIC1 0x10800000: I2C_CAM2 pclk 0x2048 / ipclk 0x2044, USI10 pclk 0x20EC / ipclk
 * 0x20E8; CMU_TOP PERIC1_BUS gate 0x20C4): a controller whose clocks are not provably on is not touched (a gated SFR
 * access can hang the bus). The I2C runs in standard mode (100 kHz) with timings computed for a 200 MHz input
 * clock (the DT's default-clk): a slower real clock only makes the bus slower.
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#include <ntddk.h>
#include <wdmsec.h>
/* NTSTRSAFE_LIB: the RtlString* functions come from km ntstrsafe.lib (linked by build.bat). Inline, they would call
   _vsnprintf through the ucrt stdio.h that build.bat's INCLUDE also carries -> __stdio_common_vsprintf, which a
   kernel driver cannot link (EWDK build 2026-11-16: LNK2019). */
#define NTSTRSAFE_LIB
#include <ntstrsafe.h>
#include "S10eCamIoctl.h"

/* ---------------------------------------------------------------------------------------------- addresses */
#define CMU_TOP_MAP         0x1A241000ULL   /* CMU_TOP 0x1A240000 + 0x1000, three pages */
#define CMU_TOP_MAP_SIZE    0x3000u
#define   TOP_CIS1_MUX        (0x1014u - 0x1000u)
#define   TOP_CIS1_DIV        (0x1810u - 0x1000u)
#define   TOP_PERIC1_BUS_GATE (0x20C4u - 0x1000u)
#define   TOP_CIS1_GATE       (0x202Cu - 0x1000u)
#define   TOP_CIS1_QCH        (0x300Cu - 0x1000u)
#define CMU_PERIC1          0x10800000ULL
#define CMU_PERIC1_SIZE     0x4000u
#define   P1_BUS_USER         0x0100u
#define   P1_I2C_CAM2_USER    0x0160u
#define   P1_USI10_USER       0x0260u
#define   P1_I2C_CAM2_DIV     0x1808u
#define   P1_USI10_DIV        0x1828u
#define   P1_I2C_CAM2_RST     0x2010u
#define   P1_I2C_CAM2_IPCLK   0x2044u
#define   P1_I2C_CAM2_PCLK    0x2048u
#define   P1_USI10_RST        0x2078u
#define   P1_USI10_IPCLK      0x20E8u
#define   P1_USI10_PCLK       0x20ECu
#define   P1_I2C_CAM2_QCH     0x301Cu
#define   P1_USI10_QCH        0x3060u
#define SYSREG_PERIC1_PAGE  0x10821000ULL
#define   USI10_MODE          0x038u
#define   USI_V2_MODE_I2C     0x4u          /* as S10eBattery's EXYNOS_USI_V2_MODE_I2C (VERIFIED there) */
#define GPIO_PERIC1         0x10830000ULL
#define   GPP6                0x040u        /* S2MPB02 bus: gpp6-0 SCL, gpp6-1 SDA */
#define   GPC0                0x060u        /* sensor bus:  gpc0-4 SCL, gpc0-5 SDA */
#define   GPC1                0x080u        /* MCLK:        gpc1-5 */
#define GPIO_PERIC0         0x10430000ULL
#define   GPG2                0x0C0u        /* reset:       gpg2-5 */
#define   GPG4                0x0E0u        /* subcam_sel:  gpg4-3 */
#define   G_CON 0x0u
#define   G_DAT 0x4u
#define   G_PUD 0x8u
#define HSI2C_CAM2          0x10880000ULL   /* sensor bus */
#define HSI2C_USI10         0x10920000ULL   /* S2MPB02 bus */
#define PAGE_4K             0x1000u

#define CG_VAL   (1u << 21)
#define CG_MAN   (1u << 20)
#define CG_AUTO  (1u << 28)

/* HSI2C (K i2c-exynos5.c) */
#define HSI2C_CTL           0x00
#define   HSI2C_CTL_SW_RST    (1u << 31)
#define   HSI2C_CTL_MASTER    (1u << 3)
#define   HSI2C_CTL_RXCHON    (1u << 6)
#define   HSI2C_CTL_TXCHON    (1u << 7)
#define HSI2C_FIFO_CTL      0x04
#define HSI2C_TRAIL_CTL     0x08
#define HSI2C_INT_EN        0x20
#define HSI2C_INT_STAT      0x24
#define   HSI2C_INT_DONE      (1u << 7)
#define   HSI2C_INT_ERRORS    ((1u << 8) | (1u << 9) | (1u << 10) | (1u << 11))   /* abort, no-ack, no-device, timeout */
#define   HSI2C_INT_NOACKDEV  ((1u << 9) | (1u << 10))
#define   HSI2C_INT_ALL       0xFFFu
#define HSI2C_ERR_STAT      0x2C
#define HSI2C_FIFO_STAT     0x30
#define   FIFO_TX_LEVEL(v)    ((v) & 0x7Fu)
#define   FIFO_RX_LEVEL(v)    (((v) >> 16) & 0x7Fu)
#define HSI2C_TX_DATA       0x34
#define HSI2C_RX_DATA       0x38
#define HSI2C_CONF          0x40
#define   HSI2C_AUTO_MODE     (1u << 31)
#define HSI2C_AUTO_CONF     0x44
#define   AUTO_STOP           (1u << 17)
#define   AUTO_READ           (1u << 16)
#define   AUTO_RUN            (1u << 31)
#define HSI2C_TIMEOUT       0x48
#define HSI2C_TRANS_STATUS  0x50
#define   MASTER_BUSY         (1u << 17)
#define HSI2C_TIMING_FS1    0x60
#define HSI2C_TIMING_FS2    0x64
#define HSI2C_TIMING_FS3    0x68
#define HSI2C_ADDR          0x70
#define HSI2C_USI_CON       0xC4
#define HSI2C_FIFO_DEPTH    16u
#define HSI2C_IPCLK_ASSUMED 200000000u
#define HSI2C_SCL_HZ        100000u

/* S2MPB02 (K s2mpb02-regulator.h) */
#define PMIC_ADDR    0x59
#define PB_ID        0x00
#define PB_B2CTRL1   0x0D
#define PB_B2CTRL2   0x0E
#define PB_L5CTRL    0x22
#define PB_L6CTRL    0x23
#define PB_L13CTRL   0x2A
#define PB_L16CTRL   0x2D
#define PB_L17CTRL   0x2E   /* VDD_GRIP_3P3, always on: read for the log, never written */
#define PB_LDO_EN    0x80u
#define PB_BUCK_EN   0xC0u
#define V_BUCK2      0x90u  /* 1.30 V */
#define V_LDO5       0x24u  /* 1.05 V */
#define V_LDO6       0x30u  /* 1.80 V */
#define V_LDO13      0x5Eu  /* 2.95 V */
#define V_LDO16      0x58u  /* 2.80 V */

#define SENSOR_ADDR  0x10
#define SENSOR_REV   0xA101u

/* ---------------------------------------------------------------------------------------------- state */
typedef struct _MAPS {
    volatile UCHAR *Top, *Peric1, *SysregP1, *GpioP1, *GpioP0, *I2cCam, *I2cPmic;
} MAPS;

typedef struct _REPORT {
    PCHAR  Buf;
    size_t Size;
    size_t Used;
} REPORT;

static PDEVICE_OBJECT gDevice;
static KEVENT         gLock;       /* synchronization event: one stage at a time, the caller stays at PASSIVE_LEVEL */

static DRIVER_UNLOAD   S10eCamUnload;
static DRIVER_DISPATCH S10eCamCreateClose, S10eCamIoctl;
DRIVER_INITIALIZE      DriverEntry;

static ULONG R32(volatile UCHAR *b, ULONG off) { return READ_REGISTER_ULONG((volatile ULONG *)(b + off)); }
static VOID  W32(volatile UCHAR *b, ULONG off, ULONG v) { WRITE_REGISTER_ULONG((volatile ULONG *)(b + off), v); }

static VOID Log(REPORT *r, PCSTR Fmt, ...)
{
    va_list a;
    size_t  left = r->Size - r->Used;
    PSTR    end = NULL;

    if (left < 2)
        return;
    va_start(a, Fmt);
    (VOID)RtlStringCbVPrintfExA(r->Buf + r->Used, left, &end, NULL, STRSAFE_IGNORE_NULLS, Fmt, a);
    va_end(a);
    if (end != NULL)
        r->Used = (size_t)(end - r->Buf);
}

static VOID SleepUs(ULONG Us)
{
    LARGE_INTEGER t;

    if (Us <= 50) {
        KeStallExecutionProcessor(Us);
        return;
    }
    t.QuadPart = -(LONGLONG)Us * 10;
    (VOID)KeDelayExecutionThread(KernelMode, FALSE, &t);
}

/* K cal-if ra.c ra_get_gate: MANUAL 1 -> CG_VAL decides; MANUAL 0 -> the clock passes unless automatic gating is on,
   which (HWACG) wakes it on access. Only MANUAL 1 + CG_VAL 0 is off (the PWM gate read 0x00200000 on 2026-11-16). */
static BOOLEAN ClkOn(ULONG Gate) { return !((Gate & CG_MAN) != 0 && (Gate & CG_VAL) == 0); }

static volatile UCHAR *Map(ULONGLONG Pa, ULONG Size)
{
    PHYSICAL_ADDRESS p;
    p.QuadPart = (LONGLONG)Pa;
    return (volatile UCHAR *)MmMapIoSpaceEx(p, Size, PAGE_READWRITE | PAGE_NOCACHE);
}

static VOID UnmapAll(MAPS *m)
{
    if (m->Top)      MmUnmapIoSpace((PVOID)m->Top, CMU_TOP_MAP_SIZE);
    if (m->Peric1)   MmUnmapIoSpace((PVOID)m->Peric1, CMU_PERIC1_SIZE);
    if (m->SysregP1) MmUnmapIoSpace((PVOID)m->SysregP1, PAGE_4K);
    if (m->GpioP1)   MmUnmapIoSpace((PVOID)m->GpioP1, PAGE_4K);
    if (m->GpioP0)   MmUnmapIoSpace((PVOID)m->GpioP0, PAGE_4K);
    if (m->I2cCam)   MmUnmapIoSpace((PVOID)m->I2cCam, PAGE_4K);
    if (m->I2cPmic)  MmUnmapIoSpace((PVOID)m->I2cPmic, PAGE_4K);
    RtlZeroMemory(m, sizeof(*m));
}

/* pin n of a bank: 4-bit fields in CON and PUD, 1 bit in DAT */
static VOID PinFunc(volatile UCHAR *Bank, ULONG Pin, ULONG Func)
{
    W32(Bank, G_CON, (R32(Bank, G_CON) & ~(0xFu << (4 * Pin))) | (Func << (4 * Pin)));
}
static VOID PinPud(volatile UCHAR *Bank, ULONG Pin, ULONG Pud)
{
    W32(Bank, G_PUD, (R32(Bank, G_PUD) & ~(0xFu << (4 * Pin))) | (Pud << (4 * Pin)));
}
static VOID PinOut(volatile UCHAR *Bank, ULONG Pin, ULONG High)
{
    W32(Bank, G_DAT, High ? (R32(Bank, G_DAT) | (1u << Pin)) : (R32(Bank, G_DAT) & ~(1u << Pin)));
    PinFunc(Bank, Pin, 1);                       /* output */
}
static VOID LogBank(REPORT *r, PCSTR Name, volatile UCHAR *Bank)
{
    Log(r, "  %-6s CON %08X DAT %08X PUD %08X\r\n", Name, R32(Bank, G_CON), R32(Bank, G_DAT), R32(Bank, G_PUD));
}

/* ---------------------------------------------------------------------------------------------- HSI2C */

/* K exynos5_i2c_reset + exynos_usi_init + set_timing(HSI2C_STAND_SPD) + exynos5_i2c_init */
static VOID I2cReset(volatile UCHAR *b)
{
    ULONG ipclk = HSI2C_IPCLK_ASSUMED;
    ULONG div = ipclk / (HSI2C_SCL_HZ * 16);
    ULONG h;

    W32(b, HSI2C_CTL, R32(b, HSI2C_CTL) | HSI2C_CTL_SW_RST);
    W32(b, HSI2C_CTL, R32(b, HSI2C_CTL) & ~HSI2C_CTL_SW_RST);
    W32(b, HSI2C_USI_CON, 0);                                    /* USI_RESET released */
    div &= 0xFF;
    W32(b, HSI2C_TIMING_FS3, (R32(b, HSI2C_TIMING_FS3) & ~0x00FF0000u) | (div << 16));
    h = (25 * (ipclk / 1000000)) / ((div + 1) * 10);
    W32(b, HSI2C_TIMING_FS2, (R32(b, HSI2C_TIMING_FS2) & ~0xFFu) | ((h < 8) ? ((0xFFu << h) & 0xFF) : 0));
    h = (25 * (ipclk / 1000000)) / ((div + 1) * 10) - 1;
    W32(b, HSI2C_TIMING_FS1, (R32(b, HSI2C_TIMING_FS1) & ~0x00FF0000u) | (((h < 8) ? ((0xFFu << h) & 0xFF) : 0) << 16));
    W32(b, HSI2C_CTL, HSI2C_CTL_MASTER);
    W32(b, HSI2C_TRAIL_CTL, 0xFFFFFF);
    W32(b, HSI2C_TIMEOUT, 0);
    W32(b, HSI2C_CONF, HSI2C_AUTO_MODE);
    W32(b, HSI2C_FIFO_CTL, 0);
    W32(b, HSI2C_AUTO_CONF, 0);
    W32(b, HSI2C_ADDR, 0);
    W32(b, HSI2C_INT_STAT, HSI2C_INT_ALL);
    W32(b, HSI2C_INT_EN, 0);
}

/* One auto-mode transfer, polled (as S10eBattery's Hsi2c.c S10eHsi2cXfer). */
static NTSTATUS I2cXfer(volatile UCHAR *b, UCHAR Addr, BOOLEAN Read, PUCHAR Data, ULONG Len, BOOLEAN Stop, PULONG IntOut)
{
    ULONG ctl, ac, a, st = 0, i, w;

    *IntOut = 0;
    if (Len == 0 || Len > HSI2C_FIFO_DEPTH)
        return STATUS_INVALID_PARAMETER;
    W32(b, HSI2C_TIMEOUT, R32(b, HSI2C_TIMEOUT) & ~(1u << 31));
    W32(b, HSI2C_FIFO_CTL, 0x3u | (Len << 4) | (Len << 16));
    ctl = R32(b, HSI2C_CTL) | HSI2C_CTL_MASTER;
    ctl = Read ? ((ctl | HSI2C_CTL_RXCHON) & ~HSI2C_CTL_TXCHON) : ((ctl | HSI2C_CTL_TXCHON) & ~HSI2C_CTL_RXCHON);
    W32(b, HSI2C_CTL, ctl);
    ac = (R32(b, HSI2C_AUTO_CONF) & ~0xFFFFu & ~AUTO_READ & ~AUTO_STOP) | Len | (Read ? AUTO_READ : 0) | (Stop ? AUTO_STOP : 0);
    a = R32(b, HSI2C_ADDR) & ~(0x3FFu << 10) & ~0x3FFu & ~(0xFFu << 24);
    W32(b, HSI2C_ADDR, a | ((ULONG)(Addr & 0x7F) << 10) | (0x7u << 24));
    for (w = 0; w < 100 && (R32(b, HSI2C_TRANS_STATUS) & MASTER_BUSY) != 0; w++)
        SleepUs(1000);
    if ((R32(b, HSI2C_TRANS_STATUS) & MASTER_BUSY) != 0) {
        I2cReset(b);
        return STATUS_DEVICE_BUSY;
    }
    W32(b, HSI2C_INT_STAT, HSI2C_INT_ALL);
    W32(b, HSI2C_AUTO_CONF, ac | AUTO_RUN);
    if (!Read) {
        for (i = 0; i < Len; i++) {
            if (FIFO_TX_LEVEL(R32(b, HSI2C_FIFO_STAT)) >= HSI2C_FIFO_DEPTH)
                return STATUS_DEVICE_DATA_ERROR;
            WRITE_REGISTER_UCHAR((volatile UCHAR *)(b + HSI2C_TX_DATA), Data[i]);
        }
    }
    for (w = 0; w < 500; w++) {                     /* 50 ms in 100 us steps */
        st = R32(b, HSI2C_INT_STAT);
        if ((st & (HSI2C_INT_DONE | HSI2C_INT_ERRORS)) != 0)
            break;
        KeStallExecutionProcessor(100);
    }
    *IntOut = st;
    if ((st & (HSI2C_INT_DONE | HSI2C_INT_ERRORS)) == 0) {
        I2cReset(b);
        return STATUS_IO_TIMEOUT;
    }
    if ((st & HSI2C_INT_ERRORS) != 0) {
        I2cReset(b);
        return (st & HSI2C_INT_NOACKDEV) ? STATUS_NO_SUCH_DEVICE : STATUS_DEVICE_DATA_ERROR;
    }
    W32(b, HSI2C_INT_STAT, st);
    if (Read) {
        for (i = 0; i < Len; i++) {
            for (w = 0; w < 100 && FIFO_RX_LEVEL(R32(b, HSI2C_FIFO_STAT)) == 0; w++)
                SleepUs(1000);
            if (FIFO_RX_LEVEL(R32(b, HSI2C_FIFO_STAT)) == 0)
                return STATUS_IO_TIMEOUT;
            Data[i] = READ_REGISTER_UCHAR((volatile UCHAR *)(b + HSI2C_RX_DATA));
        }
    }
    return STATUS_SUCCESS;
}

/* register pointer (1 or 2 bytes) without stop, then a read with restart + stop. Two bytes are read even for one
   (S10eBattery: on these controllers 1-byte auto-mode reads came back 0). */
static NTSTATUS RegRead(volatile UCHAR *b, UCHAR Addr, ULONG Reg, ULONG RegBytes, PUCHAR Out2, PULONG Int)
{
    UCHAR    p[2];
    NTSTATUS s;

    if (RegBytes == 2) { p[0] = (UCHAR)(Reg >> 8); p[1] = (UCHAR)Reg; }
    else               { p[0] = (UCHAR)Reg; }
    Out2[0] = Out2[1] = 0;
    s = I2cXfer(b, Addr, FALSE, p, RegBytes, FALSE, Int);
    if (!NT_SUCCESS(s))
        return s;
    return I2cXfer(b, Addr, TRUE, Out2, 2, TRUE, Int);
}

static NTSTATUS PbRead(MAPS *m, UCHAR Reg, PUCHAR Val, REPORT *r)
{
    UCHAR    v[2];
    ULONG    st;
    NTSTATUS s = RegRead(m->I2cPmic, PMIC_ADDR, Reg, 1, v, &st);

    *Val = v[0];
    if (!NT_SUCCESS(s))
        Log(r, "  S2MPB02 read 0x%02X failed 0x%08X (INT_STAT %03X)\r\n", Reg, s, st);
    return s;
}

/* write and read back */
static NTSTATUS PbWrite(MAPS *m, UCHAR Reg, UCHAR Val, REPORT *r)
{
    UCHAR    w[2] = { Reg, Val };
    UCHAR    back = 0;
    ULONG    st;
    NTSTATUS s = I2cXfer(m->I2cPmic, PMIC_ADDR, FALSE, w, 2, TRUE, &st);

    if (NT_SUCCESS(s))
        s = PbRead(m, Reg, &back, r);
    if (NT_SUCCESS(s) && back != Val)
        s = STATUS_DEVICE_DATA_ERROR;
    Log(r, "  S2MPB02 0x%02X <- 0x%02X (read back 0x%02X) %s\r\n", Reg, Val, back, NT_SUCCESS(s) ? "ok" : "FAILED");
    return s;
}

/* ---------------------------------------------------------------------------------------------- stage 1 */

typedef struct _FOUND {
    BOOLEAN PmicOk, CamBusOk;
    UCHAR   Id, B2c1, B2c2, L5, L6, L13, L16;
    ULONG   CisMux, CisDiv, CisGate, CisQch;
} FOUND;

static NTSTATUS Stage1(MAPS *m, FOUND *f, REPORT *r)
{
    ULONG g, ipc, pc;

    RtlZeroMemory(f, sizeof(*f));
    m->Top = Map(CMU_TOP_MAP, CMU_TOP_MAP_SIZE);
    m->Peric1 = Map(CMU_PERIC1, CMU_PERIC1_SIZE);
    m->SysregP1 = Map(SYSREG_PERIC1_PAGE, PAGE_4K);
    m->GpioP1 = Map(GPIO_PERIC1, PAGE_4K);
    m->GpioP0 = Map(GPIO_PERIC0, PAGE_4K);
    m->I2cCam = Map(HSI2C_CAM2, PAGE_4K);
    m->I2cPmic = Map(HSI2C_USI10, PAGE_4K);
    if (!m->Top || !m->Peric1 || !m->SysregP1 || !m->GpioP1 || !m->GpioP0 || !m->I2cCam || !m->I2cPmic) {
        Log(r, "map failed\r\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Log(r, "== CMU_TOP\r\n");
    f->CisMux = R32(m->Top, TOP_CIS1_MUX);
    f->CisDiv = R32(m->Top, TOP_CIS1_DIV);
    f->CisGate = R32(m->Top, TOP_CIS1_GATE);
    f->CisQch = R32(m->Top, TOP_CIS1_QCH);
    g = R32(m->Top, TOP_PERIC1_BUS_GATE);
    Log(r, "  CIS_CLK1 mux %08X (sel %u: %s) div %08X (/%u) gate %08X qch %08X\r\n", f->CisMux, f->CisMux & 1,
        (f->CisMux & 1) ? "PLL_SHARED2_DIV2" : "OSCCLK 26 MHz", f->CisDiv, (f->CisDiv & 0x1F) + 1, f->CisGate, f->CisQch);
    Log(r, "  PERIC1_BUS gate %08X -> %s\r\n", g, ClkOn(g) ? "on" : "NOT on");
    if (!ClkOn(g)) {
        Log(r, "PERIC1's bus clock is not provably on: nothing in PERIC1 is touched\r\n");
        return STATUS_DEVICE_POWERED_OFF;
    }

    Log(r, "== CMU_PERIC1\r\n");
    Log(r, "  user muxes: BUS %08X I2C_CAM2 %08X USI10 %08X (bit 4: 1 = CMU_TOP clock, 0 = OSCCLK)\r\n",
        R32(m->Peric1, P1_BUS_USER), R32(m->Peric1, P1_I2C_CAM2_USER), R32(m->Peric1, P1_USI10_USER));
    Log(r, "  dividers: I2C_CAM2 %08X USI10 %08X\r\n", R32(m->Peric1, P1_I2C_CAM2_DIV), R32(m->Peric1, P1_USI10_DIV));
    ipc = R32(m->Peric1, P1_I2C_CAM2_IPCLK); pc = R32(m->Peric1, P1_I2C_CAM2_PCLK);
    f->CamBusOk = ClkOn(ipc) && ClkOn(pc);
    Log(r, "  I2C_CAM2 ipclk %08X pclk %08X rst %08X qch %08X -> %s\r\n", ipc, pc, R32(m->Peric1, P1_I2C_CAM2_RST),
        R32(m->Peric1, P1_I2C_CAM2_QCH), f->CamBusOk ? "on" : "NOT on (sensor bus not touched)");
    ipc = R32(m->Peric1, P1_USI10_IPCLK); pc = R32(m->Peric1, P1_USI10_PCLK);
    f->PmicOk = ClkOn(ipc) && ClkOn(pc);
    Log(r, "  USI10    ipclk %08X pclk %08X rst %08X qch %08X -> %s\r\n", ipc, pc, R32(m->Peric1, P1_USI10_RST),
        R32(m->Peric1, P1_USI10_QCH), f->PmicOk ? "on" : "NOT on (S2MPB02 bus not touched)");

    Log(r, "== pins\r\n");
    LogBank(r, "gpp6", m->GpioP1 + GPP6);
    LogBank(r, "gpc0", m->GpioP1 + GPC0);
    LogBank(r, "gpc1", m->GpioP1 + GPC1);
    LogBank(r, "gpg2", m->GpioP0 + GPG2);
    LogBank(r, "gpg4", m->GpioP0 + GPG4);

    if (f->CamBusOk) {
        Log(r, "== I2C_CAM2 (sensor bus) as found: CTL %08X CONF %08X USI_CON %08X FS1 %08X FS2 %08X FS3 %08X\r\n",
            R32(m->I2cCam, HSI2C_CTL), R32(m->I2cCam, HSI2C_CONF), R32(m->I2cCam, HSI2C_USI_CON),
            R32(m->I2cCam, HSI2C_TIMING_FS1), R32(m->I2cCam, HSI2C_TIMING_FS2), R32(m->I2cCam, HSI2C_TIMING_FS3));
    }
    if (!f->PmicOk)
        return STATUS_DEVICE_POWERED_OFF;

    Log(r, "== S2MPB02 bus (USI10): mode %08X, CTL %08X USI_CON %08X as found\r\n", R32(m->SysregP1, USI10_MODE),
        R32(m->I2cPmic, HSI2C_CTL), R32(m->I2cPmic, HSI2C_USI_CON));
    if (R32(m->SysregP1, USI10_MODE) != USI_V2_MODE_I2C)
        W32(m->SysregP1, USI10_MODE, USI_V2_MODE_I2C);
    PinPud(m->GpioP1 + GPP6, 0, 0); PinPud(m->GpioP1 + GPP6, 1, 0);
    PinFunc(m->GpioP1 + GPP6, 0, 2); PinFunc(m->GpioP1 + GPP6, 1, 2);
    I2cReset(m->I2cPmic);
    Log(r, "  set: mode %08X, gpp6 CON %08X, FS1 %08X FS2 %08X FS3 %08X\r\n", R32(m->SysregP1, USI10_MODE),
        R32(m->GpioP1 + GPP6, G_CON), R32(m->I2cPmic, HSI2C_TIMING_FS1), R32(m->I2cPmic, HSI2C_TIMING_FS2),
        R32(m->I2cPmic, HSI2C_TIMING_FS3));

    if (!NT_SUCCESS(PbRead(m, PB_ID, &f->Id, r))) {
        f->PmicOk = FALSE;
        return STATUS_NO_SUCH_DEVICE;
    }
    (VOID)PbRead(m, PB_B2CTRL1, &f->B2c1, r);
    (VOID)PbRead(m, PB_B2CTRL2, &f->B2c2, r);
    (VOID)PbRead(m, PB_L5CTRL, &f->L5, r);
    (VOID)PbRead(m, PB_L6CTRL, &f->L6, r);
    (VOID)PbRead(m, PB_L13CTRL, &f->L13, r);
    (VOID)PbRead(m, PB_L16CTRL, &f->L16, r);
    Log(r, "  S2MPB02 ID 0x%02X  B2CTRL1 0x%02X (on %s) B2CTRL2 0x%02X (%u mV)\r\n", f->Id, f->B2c1,
        (f->B2c1 & PB_BUCK_EN) ? "yes" : "no", f->B2c2, (400000u + 6250u * f->B2c2) / 1000u);
    Log(r, "  LDO5 0x%02X (%u mV %s) LDO6 0x%02X (%u mV %s) LDO13 0x%02X (%u mV %s) LDO16 0x%02X (%u mV %s)\r\n",
        f->L5, (600000u + 12500u * (f->L5 & 0x7F)) / 1000u, (f->L5 & 0x80) ? "on" : "off",
        f->L6, (600000u + 25000u * (f->L6 & 0x7F)) / 1000u, (f->L6 & 0x80) ? "on" : "off",
        f->L13, (600000u + 25000u * (f->L13 & 0x7F)) / 1000u, (f->L13 & 0x80) ? "on" : "off",
        f->L16, (600000u + 25000u * (f->L16 & 0x7F)) / 1000u, (f->L16 & 0x80) ? "on" : "off");
    {
        UCHAR v = 0;
        (VOID)PbRead(m, PB_L17CTRL, &v, r);
        Log(r, "  (LDO17 VDD_GRIP_3P3, never written: 0x%02X)\r\n", v);
    }
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------------------------- stage 2 */

static VOID MclkOn(MAPS *m, REPORT *r)
{
    ULONG w;

    W32(m->Top, TOP_CIS1_QCH, R32(m->Top, TOP_CIS1_QCH) | (1u << 1));            /* CLOCK_REQ */
    W32(m->Top, TOP_CIS1_MUX, R32(m->Top, TOP_CIS1_MUX) & ~1u);                  /* OSCCLK */
    for (w = 0; w < 100 && (R32(m->Top, TOP_CIS1_MUX) & (1u << 16)); w++) KeStallExecutionProcessor(1);
    W32(m->Top, TOP_CIS1_DIV, R32(m->Top, TOP_CIS1_DIV) & ~0x1Fu);               /* /1 = 26 MHz */
    for (w = 0; w < 100 && (R32(m->Top, TOP_CIS1_DIV) & (1u << 16)); w++) KeStallExecutionProcessor(1);
    W32(m->Top, TOP_CIS1_GATE, R32(m->Top, TOP_CIS1_GATE) | CG_MAN | CG_VAL);    /* on */
    Log(r, "  CIS_CLK1 on: mux %08X div %08X gate %08X qch %08X\r\n", R32(m->Top, TOP_CIS1_MUX),
        R32(m->Top, TOP_CIS1_DIV), R32(m->Top, TOP_CIS1_GATE), R32(m->Top, TOP_CIS1_QCH));
}

static VOID MclkRestore(MAPS *m, FOUND *f)
{
    W32(m->Top, TOP_CIS1_GATE, f->CisGate);
    W32(m->Top, TOP_CIS1_DIV, f->CisDiv & 0x1Fu);
    W32(m->Top, TOP_CIS1_MUX, f->CisMux & 1u);
    W32(m->Top, TOP_CIS1_QCH, f->CisQch);
}

static VOID PowerDown(MAPS *m, FOUND *f, REPORT *r)
{
    UCHAR v;

    Log(r, "== power down (module file's order)\r\n");
    PinOut(m->GpioP0 + GPG4, 3, 0);  SleepUs(9);                                   /* subcam_sel low */
    W32(m->Top, TOP_CIS1_GATE, (R32(m->Top, TOP_CIS1_GATE) | CG_MAN) & ~CG_VAL);   /* MCLK off */
    SleepUs(1);
    PinPud(m->GpioP1 + GPC1, 5, 1);  PinOut(m->GpioP1 + GPC1, 5, 0);               /* sensor-mclk1-out */
    PinOut(m->GpioP0 + GPG2, 5, 0);                                                /* reset low */
    PinFunc(m->GpioP1 + GPC0, 4, 0); PinFunc(m->GpioP1 + GPC0, 5, 0);              /* off_i2c */
    if (PbRead(m, PB_L16CTRL, &v, r) == STATUS_SUCCESS) (VOID)PbWrite(m, PB_L16CTRL, (UCHAR)(v & ~PB_LDO_EN), r);   /* VDDAF */
    if (PbRead(m, PB_L5CTRL, &v, r) == STATUS_SUCCESS)  (VOID)PbWrite(m, PB_L5CTRL, (UCHAR)(v & ~PB_LDO_EN), r);    /* VDDD */
    if (PbRead(m, PB_L13CTRL, &v, r) == STATUS_SUCCESS) (VOID)PbWrite(m, PB_L13CTRL, (UCHAR)(v & ~PB_LDO_EN), r);   /* VDDA */
    if (PbRead(m, PB_L6CTRL, &v, r) == STATUS_SUCCESS)  (VOID)PbWrite(m, PB_L6CTRL, (UCHAR)(v & ~PB_LDO_EN), r);    /* VDDIO */
    if (PbRead(m, PB_B2CTRL1, &v, r) == STATUS_SUCCESS) (VOID)PbWrite(m, PB_B2CTRL1, (UCHAR)(v & ~PB_BUCK_EN), r);  /* BUCK2 */
    MclkRestore(m, f);
    Log(r, "  CIS_CLK1 back: mux %08X div %08X gate %08X qch %08X\r\n", R32(m->Top, TOP_CIS1_MUX),
        R32(m->Top, TOP_CIS1_DIV), R32(m->Top, TOP_CIS1_GATE), R32(m->Top, TOP_CIS1_QCH));
    LogBank(r, "gpc0", m->GpioP1 + GPC0);
    LogBank(r, "gpc1", m->GpioP1 + GPC1);
    LogBank(r, "gpg2", m->GpioP0 + GPG2);
    LogBank(r, "gpg4", m->GpioP0 + GPG4);
}

static NTSTATUS Stage2(MAPS *m, FOUND *f, REPORT *r)
{
    NTSTATUS s = STATUS_SUCCESS;
    UCHAR    d[2];
    ULONG    st, model = 0, rev = 0, fc = 0, try_;

    if (!f->PmicOk || !f->CamBusOk) {
        Log(r, "stage 2 refused: S2MPB02 bus %s, sensor bus %s\r\n", f->PmicOk ? "ok" : "NOT ok", f->CamBusOk ? "ok" : "NOT ok");
        return STATUS_DEVICE_NOT_READY;
    }
    if ((f->L5 | f->L6 | f->L13 | f->L16) & PB_LDO_EN) {
        Log(r, "stage 2 refused: a camera rail is already on (someone else is using the camera?)\r\n");
        return STATUS_DEVICE_BUSY;
    }

    Log(r, "== power up (fimc-is-device-module-3j1.c, BSS_V01)\r\n");
    PinOut(m->GpioP0 + GPG2, 5, 0);                                                /* setpin: reset low */
    PinPud(m->GpioP1 + GPC1, 5, 1);  PinOut(m->GpioP1 + GPC1, 5, 0);               /* setpin: MCLK pin low */
    if (f->B2c2 != V_BUCK2)
        s = PbWrite(m, PB_B2CTRL2, V_BUCK2, r);                                    /* 1.30 V (min = max) */
    if (NT_SUCCESS(s)) s = PbWrite(m, PB_B2CTRL1, (UCHAR)(f->B2c1 | PB_BUCK_EN), r);   /* BUCK2 on */
    SleepUs(200);
    PinOut(m->GpioP0 + GPG4, 3, 0);  SleepUs(2000);                                /* subcam_sel 0, 2 ms */
    if (NT_SUCCESS(s)) s = PbWrite(m, PB_L16CTRL, PB_LDO_EN | V_LDO16, r);         /* VDDAF 2.8 V */
    SleepUs(200);
    if (NT_SUCCESS(s)) s = PbWrite(m, PB_L13CTRL, PB_LDO_EN | V_LDO13, r);         /* VDDA 2.95 V */
    SleepUs(200);
    if (NT_SUCCESS(s)) s = PbWrite(m, PB_L6CTRL, PB_LDO_EN | V_LDO6, r);           /* VDDIO 1.8 V */
    SleepUs(200);
    if (NT_SUCCESS(s)) s = PbWrite(m, PB_L5CTRL, PB_LDO_EN | V_LDO5, r);           /* VDDD 1.05 V */
    SleepUs(1000);
    if (!NT_SUCCESS(s)) {
        Log(r, "a rail did not take: powering down\r\n");
        PowerDown(m, f, r);
        return s;
    }
    PinPud(m->GpioP1 + GPC0, 4, 0);  PinPud(m->GpioP1 + GPC0, 5, 0);
    PinFunc(m->GpioP1 + GPC0, 4, 2); PinFunc(m->GpioP1 + GPC0, 5, 2);              /* on_i2c */
    SleepUs(10);
    PinOut(m->GpioP0 + GPG2, 5, 1);                                                /* reset high */
    PinPud(m->GpioP1 + GPC1, 5, 0);  PinFunc(m->GpioP1 + GPC1, 5, 2);              /* sensor-mclk1-fn */
    MclkOn(m, r);
    SleepUs(9000);
    LogBank(r, "gpc0", m->GpioP1 + GPC0);
    LogBank(r, "gpc1", m->GpioP1 + GPC1);
    LogBank(r, "gpg2", m->GpioP0 + GPG2);
    LogBank(r, "gpg4", m->GpioP0 + GPG4);

    Log(r, "== sensor 0x%02X on I2C_CAM2\r\n", SENSOR_ADDR);
    I2cReset(m->I2cCam);
    for (try_ = 0; try_ < 2; try_++) {
        s = RegRead(m->I2cCam, SENSOR_ADDR, 0x0000, 2, d, &st);
        if (NT_SUCCESS(s))
            break;
        Log(r, "  model ID read failed 0x%08X (INT_STAT %03X, ERR %08X)%s\r\n", s, st, R32(m->I2cCam, HSI2C_ERR_STAT),
            try_ == 0 ? " - 20 ms more, again" : "");
        SleepUs(20000);
    }
    if (NT_SUCCESS(s)) {
        model = ((ULONG)d[0] << 8) | d[1];
        if (NT_SUCCESS(RegRead(m->I2cCam, SENSOR_ADDR, 0x0002, 2, d, &st)))
            rev = ((ULONG)d[0] << 8) | d[1];
        if (NT_SUCCESS(RegRead(m->I2cCam, SENSOR_ADDR, 0x0005, 2, d, &st)))
            fc = d[0];
        Log(r, "  MODEL ID 0x%04X  REVISION 0x%04X (%s)  FRAME COUNT 0x%02X (%s)\r\n", model, rev,
            rev == SENSOR_REV ? "= DT rev_reg 0xA101: the S5K3J1" : "not the DT's 0xA101",
            fc, fc == 0xFF ? "not streaming, as expected" : "?");
    }
    PowerDown(m, f, r);
    return s;
}

/* ---------------------------------------------------------------------------------------------- dispatch */

static NTSTATUS S10eCamIoctl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    S10ECAM_STAGE_IN   in;
    MAPS               m;
    FOUND              f;
    REPORT             r;
    NTSTATUS           s;

    UNREFERENCED_PARAMETER(DeviceObject);
    Irp->IoStatus.Information = 0;
    if (sp->Parameters.DeviceIoControl.IoControlCode != IOCTL_S10ECAM_STAGE ||
        sp->Parameters.DeviceIoControl.InputBufferLength < sizeof(in) ||
        sp->Parameters.DeviceIoControl.OutputBufferLength < 1024) {
        s = STATUS_INVALID_DEVICE_REQUEST;
        goto done;
    }
    in = *(S10ECAM_STAGE_IN *)Irp->AssociatedIrp.SystemBuffer;
    if (in.Stage < 1 || in.Stage > 2) {
        s = STATUS_INVALID_PARAMETER;
        goto done;
    }
    r.Buf = (PCHAR)Irp->AssociatedIrp.SystemBuffer;
    r.Size = sp->Parameters.DeviceIoControl.OutputBufferLength;
    if (r.Size > S10ECAM_REPORT_MAX)
        r.Size = S10ECAM_REPORT_MAX;
    r.Used = 0;
    r.Buf[0] = 0;
    RtlZeroMemory(&m, sizeof(m));

    (VOID)KeWaitForSingleObject(&gLock, Executive, KernelMode, FALSE, NULL);
    Log(&r, "S10eCam 0.1 stage %u\r\n", in.Stage);
    s = Stage1(&m, &f, &r);
    Log(&r, "stage 1: 0x%08X\r\n", s);
    if (in.Stage == 2 && NT_SUCCESS(s)) {
        s = Stage2(&m, &f, &r);
        Log(&r, "stage 2: 0x%08X\r\n", s);
    }
    UnmapAll(&m);
    KeSetEvent(&gLock, IO_NO_INCREMENT, FALSE);
    Irp->IoStatus.Information = r.Used + 1;
    s = STATUS_SUCCESS;                                  /* the outcome is in the report */
done:
    Irp->IoStatus.Status = s;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return s;
}

static NTSTATUS S10eCamCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static VOID S10eCamUnload(PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING dos;

    UNREFERENCED_PARAMETER(DriverObject);
    RtlInitUnicodeString(&dos, S10ECAM_DOS_NAME);
    (VOID)IoDeleteSymbolicLink(&dos);
    if (gDevice != NULL)
        IoDeleteDevice(gDevice);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    UNICODE_STRING name, dos, sddl;
    NTSTATUS       st;
    static const GUID cls = { 0x5a10e9a2, 0x3c2d, 0x4f6e, { 0x9b, 0x21, 0x7e, 0x55, 0x1d, 0x0c, 0x8a, 0x43 } };

    UNREFERENCED_PARAMETER(RegistryPath);
    KeInitializeEvent(&gLock, SynchronizationEvent, TRUE);
    DriverObject->DriverUnload = S10eCamUnload;
    RtlInitUnicodeString(&name, S10ECAM_DEVICE_NAME);
    RtlInitUnicodeString(&sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");   /* SYSTEM + administrators */
    st = IoCreateDeviceSecure(DriverObject, 0, &name, FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN, FALSE, &sddl,
                              &cls, &gDevice);
    if (!NT_SUCCESS(st))
        return st;
    RtlInitUnicodeString(&dos, S10ECAM_DOS_NAME);
    (VOID)IoCreateSymbolicLink(&dos, &name);
    DriverObject->MajorFunction[IRP_MJ_CREATE] = S10eCamCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = S10eCamCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = S10eCamIoctl;
    gDevice->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}
