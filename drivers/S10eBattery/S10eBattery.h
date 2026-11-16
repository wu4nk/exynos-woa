/*
 * S10eBattery.h - KMDF battery miniclass (battc) for the Galaxy S10e (Exynos 9820, beyond0lte).
 *
 * Reports the MAX77705 fuel gauge (7-bit 0x36) and charger status (0x69) on HSI2C45 to the Windows battery
 * class. The driver owns HSI2C45 itself as a polled master, a port of the UEFI code that is proven on the
 * device (firmware/Platform/Beyond0LtePkg/Drivers/Beyond0LteUsbHostDxe/Pmic.c + FuelGauge.c).
 * Design and every constant's source: notes/battery-driver-m1.md, notes/battery-driver.md.
 *
 * The WDF/battc plumbing follows Microsoft's Windows-driver-samples "simbatt" sample (MIT License,
 * Copyright (c) Microsoft Corporation).
 */

#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <batclass.h>
#include <wmistr.h>
#include <wmilib.h>
#include <ntstrsafe.h>

/* ---------------------------------------------------------------------------------------------------------------
 * MMIO windows (ACPI _CRS of \_SB.BAT0, in this order). VERIFIED values, copied from
 * firmware/Silicon/Exynos9820Pkg/Include/Platform/Exynos9820.h:477-502 - the driver refuses to start unless the
 * resources it is given are exactly these.
 * ------------------------------------------------------------------------------------------------------------- */
#define S10E_HSI2C45_BASE            0x15D60000ULL   /* EXYNOS_PMIC_HSI2C_BASE      (VERIFIED) */
#define S10E_HSI2C45_SIZE            0x1000u         /* EXYNOS_PMIC_HSI2C_SIZE      (VERIFIED) */
#define S10E_CMGP_USI_PAGE_BASE      0x15C22000ULL   /* EXYNOS_CMGP_SYSREG_USI_PAGE (VERIFIED) */
#define S10E_CMGP_USI_PAGE_SIZE      0x1000u         /* ..._USI_PAGE_SIZE (one page; the header does not tag it) */
#define S10E_CMGP_USI03_MODE_OFFSET  0x30u           /* EXYNOS_CMGP_USI03_MODE 0x15C22030 - page base */
#define S10E_USI_V2_MODE_I2C         0x4u            /* EXYNOS_USI_V2_MODE_I2C      (VERIFIED) */
#define S10E_CMGP_GPIO_BASE          0x15C30000ULL   /* EXYNOS_CMGP_GPIO_BASE       (VERIFIED) */
#define S10E_CMGP_GPIO_SIZE          0x1000u         /* EXYNOS_CMGP_GPIO_SIZE       (VERIFIED) */
#define S10E_CMGP_GPM12_CON_OFFSET   0x180u          /* gpm12-0 = SCL               (VERIFIED) */
#define S10E_CMGP_GPM13_CON_OFFSET   0x1A0u          /* gpm13-0 = SDA               (VERIFIED) */
#define S10E_GPIO_FUNC_HSI2C45       0x2u            /* DT samsung,pin-function <2> (VERIFIED) */

/* ---------------------------------------------------------------------------------------------------------------
 * HSI2C register map - same names/values as Beyond0LteUsbHostDxe/UsbHw.h section 5 ([K] = vendor
 * drivers/i2c/busses/i2c-exynos5.c) and the error bits from Pmic.c.
 * ------------------------------------------------------------------------------------------------------------- */
#define HSI2C_CTL                 0x00
#define   HSI2C_CTL_MASTER                  (1u << 3)
#define   HSI2C_CTL_RXCHON                  (1u << 6)
#define   HSI2C_CTL_TXCHON                  (1u << 7)
#define   HSI2C_CTL_SW_RST                  (1u << 31)
#define HSI2C_FIFO_CTL            0x04
#define   HSI2C_FIFO_CTL_RX_EN              (1u << 0)
#define   HSI2C_FIFO_CTL_TX_EN              (1u << 1)
#define   HSI2C_FIFO_CTL_RX_TRIG(x)         ((ULONG)(x) << 4)
#define   HSI2C_FIFO_CTL_TX_TRIG(x)         ((ULONG)(x) << 16)
#define HSI2C_TRAIL_CTL           0x08
#define HSI2C_INT_EN              0x20
#define HSI2C_INT_STAT            0x24
#define   HSI2C_INT_TRANSFER_DONE           (1u << 7)
#define   HSI2C_INT_TRANSFER_ABORT          (1u << 8)
#define   HSI2C_INT_NO_ACK                  (1u << 9)
#define   HSI2C_INT_NODEV                   (1u << 10)
#define   HSI2C_INT_TIMEOUT                 (1u << 11)
#define   HSI2C_INT_ERRORS                  (HSI2C_INT_TRANSFER_ABORT | HSI2C_INT_NO_ACK | HSI2C_INT_NODEV | \
                                             HSI2C_INT_TIMEOUT)
#define   HSI2C_INT_ALL                     0x00000FFFu
#define HSI2C_ERROR_STATUS        0x2C
#define HSI2C_FIFO_STAT           0x30
#define   HSI2C_FIFO_STAT_TX_LEVEL(x)       ((x) & 0x7F)
#define   HSI2C_FIFO_STAT_RX_LEVEL(x)       (((x) >> 16) & 0x7F)
#define HSI2C_TX_DATA             0x34
#define HSI2C_RX_DATA             0x38
#define HSI2C_CONF                0x40
#define   HSI2C_CONF_AUTO_MODE              (1u << 31)
#define HSI2C_AUTO_CONF           0x44
#define   HSI2C_AUTO_CONF_READ_WRITE        (1u << 16)
#define   HSI2C_AUTO_CONF_STOP_AFTER_TRANS  (1u << 17)
#define   HSI2C_AUTO_CONF_MASTER_RUN        (1u << 31)
#define HSI2C_TIMEOUT             0x48
#define HSI2C_TRANS_STATUS        0x50
#define   HSI2C_MASTER_BUSY                 (1u << 17)
#define HSI2C_TIMING_FS1          0x60
#define HSI2C_TIMING_FS2          0x64
#define HSI2C_TIMING_FS3          0x68
#define HSI2C_ADDR                0x70
#define HSI2C_USI_CON             0xC4
/* FS timing: UsbHw.h HSI2C_TIMING_FS*_BOOT = the Windows HSI2C driver's 400 kHz values from the 200 MHz clock,
   device-validated on this bus in UEFI (M16). */
#define HSI2C_TIMING_FS1_BOOT     0x00F00000u
#define HSI2C_TIMING_FS2_BOOT     0x000000E0u
#define HSI2C_TIMING_FS3_BOOT     0x00210000u
#define HSI2C_FIFO_DEPTH          16
#define HSI2C_XFER_TIMEOUT_US     50000u    /* Pmic.c HSI2C_TIMEOUT_MS 50 */

/* ---------------------------------------------------------------------------------------------------------------
 * MAX77705 clients and registers. [K] = vendor source (LineageOS android_kernel_samsung_exynos9820 lineage-18.1:
 * include/linux/mfd/max77705-private.h, drivers/battery_v2/max77705_fuelgauge.c, .../max77705_charger.{c,h}).
 * ------------------------------------------------------------------------------------------------------------- */
#define MAX77705_FG_ADDR          0x36    /* [K] 0x6C >> 1 */
#define MAX77705_FG_STATUS        0x00    /* [K] low byte bit3 = battery absent */
#define   MAX77705_FG_STATUS_ABSENT         0x08
#define MAX77705_FG_REMCAP_REP    0x05    /* [K] */
#define MAX77705_FG_SOCREP        0x06    /* [K] hi byte = %, lo byte = 1/256 % */
#define MAX77705_FG_TEMPERATURE   0x08    /* [K] hi byte signed degC, lo 0.039 degC */
#define MAX77705_FG_VCELL         0x09    /* [K] */
#define MAX77705_FG_AVG_CURRENT   0x0B    /* [K] signed */
#define MAX77705_FG_DESIGNCAP     0x18    /* [K] DESIGNCAP_REG */
#define MAX77705_FG_FULLCAP_REP   0x35    /* [K] */
/* Sense-resistor factor: vendor "standard value is 2 which means 5mOhm". UNVERIFIED for beyond0lte (DT battery
   node not read). Scales mAh/mA only, never the percentage. Indirect support only: UEFI FULLCAP_REP reads of
   2460 (notes/m16-usb.md) and 3000 (task brief, log not committed) at this factor are plausible mAh for a
   3100 mAh-class cell; half or double would not be (notes/battery-driver-m1.md). */
#define MAX77705_FG_RESISTOR_UNVERIFIED     2

#define MAX77705_CHG_ADDR         0x69    /* [K] 0xD2 >> 1 */
/* G18 H1 (notes/g18-hdmi-dp-alt.md): the USB-C / PD controller block, READ-ONLY. [K] = vendor
 * include/linux/mfd/max77705-private.h (LineageOS lineage-23.2): USBC/MUIC client 0x4A (8-bit), registers
 * UIC_HW_REV 0x00, UIC_FW_REV 0x01, UIC_INT..VDM_INT 0x02-0x05 (interrupt latches - NOT read here, a read would
 * clear them), USBC_STATUS1 0x06, USBC_STATUS2 0x07, BC_STATUS 0x08, CC_STATUS0 0x0A, CC_STATUS1 0x0B,
 * PD_STATUS0 0x0C, PD_STATUS1 0x0D. */
#define MAX77705_USBC_ADDR        0x25    /* [K] 0x4A >> 1 */
#define MAX77705_USBC_HW_REV      0x00
#define MAX77705_USBC_STATUS1     0x06    /* 0x06..0x0D read as one 8-byte block */

/* G18 H1b (notes/g18-hdmi-dp-alt.md): read-only window on the USB-C block for tools/phone/usbc/usbcwatch - a private
   IOCTL on the battery device (preprocessed before BatteryClassIoctl): input S10E_USBC_READ_IN, output Length bytes
   from Reg. Reading 0x02..0x05 clears those interrupt latches (nothing else consumes them under Windows). */
#define IOCTL_S10E_USBC_READ      CTL_CODE(FILE_DEVICE_UNKNOWN, 0x8A0, METHOD_BUFFERED, FILE_READ_ACCESS)
typedef struct _S10E_USBC_READ_IN { UCHAR Reg; UCHAR Length; } S10E_USBC_READ_IN;
#define S10E_USBC_READ_MAX        128

/* G18 H2 (notes/g18-h2-altmode-research.md): the max77705 opcode interface, ALLOWLISTED to the DP alt-mode entry
   (no VBUS / role / charging writes - research section G): 0x55 SET_ALTERNATEMODE data 0x00/0x01/0x03, 0x48 SEND_VDM
   with exactly the Discover Identity / Enter Mode / DP Configure payloads, 0x4B GET_VDM_RESP. The driver writes
   [opcode, data] from reg 0x21, commits with 0x00 at 0x41 (when shorter than 32), polls UIC_INT 0x02 b7 (3 s) and
   reads ReadLen + 1 reply bytes from 0x51 (byte 0 = the opcode). WRITE_REG: only the INT mask regs 0x0E..0x11. */
#define IOCTL_S10E_USBC_OPCODE    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x8A1, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_S10E_USBC_WRITE_REG CTL_CODE(FILE_DEVICE_UNKNOWN, 0x8A2, METHOD_BUFFERED, FILE_WRITE_ACCESS)
typedef struct _S10E_USBC_OPCODE_IN { UCHAR Opcode; UCHAR DataLen; UCHAR ReadLen; UCHAR Data[32]; } S10E_USBC_OPCODE_IN;
typedef struct _S10E_USBC_WRITE_IN { UCHAR Reg; UCHAR Value; } S10E_USBC_WRITE_IN;
/* Charger watchdog (kernel max77705_charger.c set_wdtmr_kick / get_wdtmr_status, max77705_charger.h:124-245):
   CNFG_00 bit4 WDTEN; kick = CNFG_06 bits 1:0 <- 01 (WDTCLR); CHG_DTLS 0x0B = watchdog expired -> charging off.
   Linux kicks it on every health poll; under Windows nobody did, so the phone stopped charging (run batt1). */
#define MAX77705_CHG_CNFG_00      0xB7
#define   MAX77705_MODE_MASK                0xF
#define   MAX77705_MODE_OTG_ON              0xE   /* [K] BUCK+BOOST+OTG, charger off (UsbHw.h:320) */
#define   MAX77705_MODE_CHG_OTG_ON          0xF   /* [K] same + charger engine (UsbHw.h:321) */
#define MAX77705_CHG_CNFG_06      0xBD
/* Charging-current limits (vendor max77705_charger.c :590-648, max77705-private.h:77/84, max77705_charger.h:194/275):
   CHG_CNFG_02 0xB9 bits 5:0 CHG_CC fast-charge current, 50 mA steps (<= 2 -> 100 mA); CHG_CNFG_09 0xC0 bits 6:0
   CHGIN_ILIM input current limit, (field + 1) x 25 mA. Read only (notes/battery-driver.md). */
#define MAX77705_CHG_CNFG_02      0xB9
#define MAX77705_CHG_CNFG_09      0xC0
#define   MAX77705_CHG_CC_MASK              0x3F  /* [K] max77705_charger.h:189 MAX77705_CHG_CC (bits 7:6 OTG_ILIM kept) */
#define   MAX77705_CHG_CHGIN_LIM_MASK       0x7F  /* [K] max77705_charger.h:270 MAX77705_CHG_CHGIN_LIM */
#define   MAX77705_CHG_CNFG_06_CHGPROT      0x0C  /* [K] bits 3:2 = 11 unlocks the charger settings
                                                     (max77705_charger_unlock / _initialize) */

/* 1.0.0.10 ChargeTune: CHGIN_ILIM / CHG_CC per BC1.2 type, from the vendor S10 (beyondlte) cable table
   arch/arm64/boot/dts/samsung/battery_data_beyondlte_common.dtsi "cable-info" (LineageOS
   android_kernel_samsung_exynos9820, fetched 2026-11-16): TA (3) = default_input_current 1800 /
   default_charging_current 2100; USB (4) = current_group_12 475 / 550; USB_CDP (5) = current_group_3 1500 / 1500.
   Register encodings = vendor max77705_set_input_current (I/25 - 1) / max77705_set_charge_current (I/50).
   Hot guard: temp_high_threshold_normal 500 / temp_high_recovery_normal 480 (0.1 degC) - the vendor stops charging
   there; this driver does not touch the charger mode, it drops to CC 100 mA / ILIM 500 mA instead. */
#define S10E_TUNE_TA_ILIM_MA      1800u
#define S10E_TUNE_TA_CC_MA        2100u
#define S10E_TUNE_USB_ILIM_MA     475u
#define S10E_TUNE_USB_CC_MA       550u
#define S10E_TUNE_CDP_ILIM_MA     1500u
#define S10E_TUNE_CDP_CC_MA       1500u
#define S10E_TUNE_HOT_ILIM_MA     500u
#define S10E_TUNE_HOT_CC_MA       100u
#define S10E_TUNE_HOT_MILLIC      50000
#define S10E_TUNE_COOL_MILLIC     48000
/* USB-C block BC_STATUS 0x08 [K] max77705-muic.h:224-231 / :293-299: bits 1:0 ChgTyp 0 none, 1 SDP (USB),
   2 CDP, 3 DCP (dedicated charger); bit 2 DCDTmo; bits 5:3 PrChgTyp; bit 7 VBUSDet. UsbcStatus[] starts at 0x06. */
#define MAX77705_USBC_BC_STATUS   0x08
#define   MAX77705_BC_CHGTYP(x)             ((x) & 0x03)
#define   MAX77705_CHG_CNFG_06_WDTCLR_MASK  0x03
#define   MAX77705_CHG_CNFG_06_WDTCLR       0x01
#define MAX77705_CHG_DETAILS_00   0xB3    /* [K] bits 6:5 CHGIN_DTLS (3 = VBUS valid) */
#define   MAX77705_CHGIN_DTLS(x)            (((x) & 0x60) >> 5)
#define   MAX77705_CHGIN_DTLS_VALID         3
/* MAX77705_CHG_DETAILS_01 0xB4 [K]: bits 3:0 CHG_DTLS, read as the 2nd byte of the 0xB3 word */
#define   MAX77705_CHG_DTLS(x)              ((x) & 0x0F)

/* Battery nominal voltage for mAh -> mWh (Windows reports energy). UNVERIFIED: close it from the battery label
   (EB-BG970ABU) or the vendor DT battery node. Scales the Wh figures only; the percentage is the gauge's SOCREP. */
#define S10E_BATT_NOMINAL_MV_UNVERIFIED     3850u

/* Plausibility gate, NOT a hardware constant: a single Li-ion cell cannot read outside this window, so a VCELL
   outside it means the transfer returned garbage (e.g. the all-zero 1-byte-read failure mode of this bus). Such a
   reading is dropped instead of being reported - a bogus 0 % would trigger Windows' critical-battery action. */
#define S10E_PLAUSIBLE_VCELL_MIN_MV         2500u
#define S10E_PLAUSIBLE_VCELL_MAX_MV         5000u

#define S10E_BATT_POLL_MS         2000    /* 1.0.0.10: tick period - a 1-register charger check (CHGIN_DTLS) */
#define S10E_BATT_FULL_EVERY      10      /* full reading every 10th tick (20 s, as before), or at once when
                                             CHGIN_DTLS changed (plug / unplug seen within 2 s, was up to 20 s) */
#define S10E_TUNE_PENDING_TICKS   10      /* charger seen but BC1.2 type not yet known: full reading each tick */
#define S10E_BATT_TAG             'tBeS'

/* ---------------------------------------------------------------------------------------------------------------
 * VIBRATION (Vibra.c, notes/s10-vibration.md). The S10's LRA motor is driven by the MAX77705's haptic block (same
 * chip, same HSI2C45 bus as the battery -> one owner, BusLock) from SoC PWM channel 1. Every value below is from the
 * phone's own live DT (dump/s10-dump/dt/live.dts) or the LineageOS exynos9820 kernel (lineage-23.0) [K]:
 *   max77705@66 on hsi2c@15D60000, pinctrl-0 = max77705-irq, vib-pwm (gpp3-1 func 2, pud 1), motor-boost-en
 *   (gpg1-7 output 1); haptic { pwm_id 1, multi_frequency 7, frequency[0] 0x63B (159.5 Hz), period[0] 0xBEBC ns,
 *   duty[0] 0x8F0D ns, max_timeout 0x2710 ms } (live.dts:9696-9699, 2073-2090, 14612-14631);
 *   pwm@10510000 "samsung,s3c6400-pwm" (16-bit counters, live.dts:11386);
 *   [K] drivers/motor/max77705_haptic.c: MAINCTRL1 (0x02) |= BIASEN b7; MCONFIG (0x10): init EN b6 off, LRA b7 on,
 *   DIVIDER_128 b1 on; run = LRA|EN on, stop = LRA|EN off; PMIC client 0xCC >> 1 (drivers/mfd/max77705.c:57).
 *   [K] drivers/motor/sec_haptic.c: duty = period/2 + (max_duty - period/2) * intensity / 10000; period/2 = stop.
 *   [K] drivers/pwm/pwm-samsung.c: TCNTB(n) 0x0C+n*0xC, TCMPB(n) 0x10+n*0xC, TCON bits of channel 1 at 8..11
 *   (start, manual update, invert, autoreload), NORMAL polarity = invert bit SET, tcmp = tcnt - duty ticks, both -1.
 *   [K] drivers/pinctrl/samsung/pinctrl-exynos-arm64.c exynos9820_pin_banks5 (PERIC0 @0x10430000, live.dts:1342):
 *   gpp3 0x060, gpg1 0x0A0; exynos9810_bank_type_off: CON +0 (4 bit/pin), DAT +4, PUD +8 (4 bit/pin).
 *   [K] drivers/soc/samsung/cal-if/exynos9820/cmucal-sfr.c: CMU_PERIC0 0x10400000, PWM PCLK gate 0x2020
 *   (CG_VAL b21, MANUAL b20, ENABLE_AUTOMATIC_CLKGATING b28), PWM QCH 0x3014. Read only - the driver never writes
 *   the CMU and refuses to touch the PWM when its clock is not provably running.
 * The PWM input clock is NOT taken from a table: Vibra.c measures it against KeQueryPerformanceCounter with the
 * output pin still unmuxed (no motor drive) and refuses rates outside 1..250 MHz.
 * ------------------------------------------------------------------------------------------------------------- */
#define S10E_CMU_PERIC0_PAGE         0x10402000ULL   /* CMU_PERIC0 + 0x2000, two pages: gate 0x2020, QCH 0x3014 */
#define S10E_CMU_PERIC0_PAGE_SIZE    0x2000u
#define   S10E_CMU_PWM_GATE_OFF               0x020u    /* 0x10402020 */
#define   S10E_CMU_PWM_QCH_OFF                0x1014u   /* 0x10403014 */
#define   S10E_CMU_GATE_CG_VAL                (1u << 21)
#define   S10E_CMU_GATE_MANUAL                (1u << 20)
#define   S10E_CMU_GATE_AUTO                  (1u << 28)
#define S10E_PWM_BASE                0x10510000ULL
#define S10E_PWM_SIZE                0x1000u
#define   PWM_TCFG0                           0x00
#define   PWM_TCFG1                           0x04
#define   PWM_TCON                            0x08
#define   PWM_TCNTB1                          0x18
#define   PWM_TCMPB1                          0x1C
#define   PWM_TCNTO1                          0x20
#define   PWM_TCON0_START                     (1u << 0)
#define   PWM_TCON1_START                     (1u << 8)
#define   PWM_TCON1_MANUAL                    (1u << 9)
#define   PWM_TCON1_INVERT                    (1u << 10)
#define   PWM_TCON1_AUTORELOAD                (1u << 11)
#define   PWM_TCON1_ALL                       (0xFu << 8)
#define   PWM_COUNTER_MAX                     0xFFFFu   /* s3c6400 variant: 16-bit */
#define S10E_PERIC0_GPIO_BASE        0x10430000ULL
#define S10E_PERIC0_GPIO_SIZE        0x1000u
#define   GPP3_CON                            0x060u
#define   GPP3_PUD                            0x068u
#define   GPG1_CON                            0x0A0u
#define   GPG1_DAT                            0x0A4u
#define   VIB_PWM_PIN                         1         /* gpp3-1 */
#define   VIB_PWM_FUNC                        0x2u      /* TOUT1 */
#define   VIB_PWM_PUD                         0x1u      /* pin-pud <1> */
#define   VIB_BOOST_PIN                       7         /* gpg1-7 */
#define MAX77705_PMIC_ADDR           0x66    /* [K] I2C_ADDR_PMIC 0xCC >> 1; live DT max77705@66 */
#define   MAX77705_PMIC_MAINCTRL1             0x02
#define     MAX77705_BIASEN                   0x80
#define   MAX77705_PMIC_MCONFIG               0x10
#define     MAX77705_MOTOR_LRA                0x80
#define     MAX77705_MOTOR_EN                 0x40
#define     MAX77705_DIVIDER_128              0x02
#define S10E_VIB_PERIOD_NS           48828u  /* haptic,period[0] 0xBEBC */
#define S10E_VIB_MAX_DUTY_NS         36621u  /* haptic,duty[0]   0x8F0D (75 %) */
#define S10E_VIB_MAX_MS              10000u  /* haptic,max_timeout */
#define S10E_VIB_MAX_INTENSITY       10000

/* IOCTL_S10E_VIBRATE (tools/s10-kit/vibra-s10.ps1): run the motor DurationMs (1..10000, 0 = stop now) at Intensity
   (1..10000, Samsung's scale), auto-stop by timer. Output: what the driver saw and did (always filled). */
#define IOCTL_S10E_VIBRATE        CTL_CODE(FILE_DEVICE_UNKNOWN, 0x8A3, METHOD_BUFFERED, FILE_WRITE_ACCESS)
typedef struct _S10E_VIBRATE_IN { ULONG DurationMs; LONG Intensity; } S10E_VIBRATE_IN;
typedef struct _S10E_VIBRATE_OUT {
    ULONG Version;        /* 1 */
    ULONG Step;           /* last step reached: 1 map, 2 clock check, 3 calibrate, 4 pins, 5 pwm, 6 pmic, 7 running */
    LONG  Status;
    ULONG CmuGate, CmuQch;
    ULONG Tcfg0, Tcfg1, TconBefore, TconAfter;
    ULONG TinHz;          /* measured PWM input clock */
    ULONG CalCounts, CalTicks, QpcHz;
    ULONG Tcnt, Tcmp;
    ULONG Gpp3Con, Gpp3Pud, Gpg1Con, Gpg1Dat;   /* after */
    UCHAR MainCtrl1, Mconfig, MainCtrl1Before, MconfigBefore;
} S10E_VIBRATE_OUT;

/* ------------------------------------------------------------------------------------------------------------- */

typedef struct _S10E_BATT_READING {
    ULONG   FgStatus;        /* raw STATUS word */
    ULONG   SocTenths;       /* 0..1000 */
    ULONG   VcellMv;
    LONG    AvgCurrentMa;    /* + = into the battery */
    LONG    TempMilliC;
    ULONG   RemCapMah;
    ULONG   FullCapMah;
    ULONG   DesignCapMah;
    UCHAR   ChgDetails00;
    UCHAR   ChgDetails01;
    BOOLEAN ChargerRead;     /* charger 0x69 answered this poll */
    UCHAR   ChgCnfg00;       /* CHG_CNFG_00 0xB7 (mode, bit4 WDTEN) */
    BOOLEAN Cnfg00Read;      /* ChgCnfg00 is valid */
    UCHAR   ChgCnfg06;       /* CHG_CNFG_06 0xBD before the watchdog kick - diagnostics */
    UCHAR   ChgCnfg02;       /* CHG_CNFG_02 0xB9 (CHG_CC) - diagnostics, 0xFF = not read */
    UCHAR   ChgCnfg09;       /* CHG_CNFG_09 0xC0 (CHGIN_ILIM) - diagnostics, 0xFF = not read */
    BOOLEAN WdtKicked;       /* the charger watchdog kick write succeeded this poll */
    BOOLEAN UsbcRead;        /* G18 H1: the USB-C block answered */
    UCHAR   UsbcRev[2];      /* UIC_HW_REV, UIC_FW_REV */
    UCHAR   UsbcStatus[8];   /* 0x06 USBC_STATUS1 .. 0x0D PD_STATUS1 */
} S10E_BATT_READING, *PS10E_BATT_READING;

typedef struct _S10E_BATT_GLOBAL {
    UNICODE_STRING RegistryPath;
} S10E_BATT_GLOBAL, *PS10E_BATT_GLOBAL;

typedef struct _S10E_BATT_FDO {
    WDFDEVICE           Device;

    /* MMIO (PrepareHardware / ReleaseHardware) */
    volatile UCHAR     *I2c;
    volatile UCHAR     *UsiPage;
    volatile UCHAR     *GpioPage;

    /* bus (BusLock) */
    WDFWAITLOCK         BusLock;
    BOOLEAN             BusReady;
    BOOLEAN             BusSaved;
    ULONG               SavedMode;
    ULONG               SavedScl;
    ULONG               SavedSda;
    ULONG               SavedUsiCon;

    /* polling */
    WDFTIMER            PollTimer;
    WDFWORKITEM         PollWork;
    ULONG               PollOk;
    ULONG               PollFail;
    NTSTATUS            LastPollStatus;
    ULONG               Tick;                /* timer ticks (PollWork) */
    UCHAR               LastChgin;           /* CHGIN_DTLS of the last full reading (0xFF = none yet) */

    /* ChargeTune (BusLock): Services\S10eBattery\Parameters\ChargeTune, read at every full reading */
    ULONG               TuneSetting;         /* 0 = read only (as 1.0.0.9), 1 = set ILIM / CC per BC1.2 type */
    ULONG               TunePending;         /* ticks left with a charger present and no BC1.2 type yet */
    BOOLEAN             TuneHot;             /* hot guard latched (hysteresis) */
    BOOLEAN             TuneArmed;           /* the <= 10 fast ticks were granted for this plug-in already */
    UCHAR               TuneBc;              /* BC_STATUS of the last full reading with the charger present */
    ULONG               TuneIlimMa;          /* target of the last decision (0 = none: no charger / unknown type) */
    ULONG               TuneCcMa;
    ULONG               TuneWrites;          /* register writes done (counted per CNFG_02 / 09 / 06 write) */

    /* battery class registration (ClassInitLock) */
    WDFWAITLOCK         ClassInitLock;
    PVOID               ClassHandle;
    WMILIB_CONTEXT      WmiLibContext;

    /* reported state (StateLock) */
    WDFWAITLOCK         StateLock;
    ULONG               BatteryTag;          /* BATTERY_TAG_INVALID until a reading succeeded */
    ULONG               NextTag;
    S10E_BATT_READING   Last;                /* last good reading */
    BATTERY_INFORMATION Info;
    BATTERY_STATUS      Status;
    ULONG               TemperatureDeciK;

    /* unplug diagnosis (StateLock): did the poll see the change, and did the class driver re-query? */
    ULONG               NotifyCount;         /* BatteryClassStatusNotify calls */
    ULONG               QueryStatusCount;    /* QueryStatus calls from the class driver */
    ULONG               QueriedPowerState;   /* PowerState returned by the last QueryStatus */

    /* vibration (Vibra.c, VibLock; I2C under BusLock taken inside VibLock) */
    WDFWAITLOCK         VibLock;
    WDFTIMER            VibTimer;
    WDFWORKITEM         VibWork;
    volatile UCHAR     *VibCmu;
    volatile UCHAR     *VibPwm;
    volatile UCHAR     *VibGpio;
    ULONG               VibTinHz;            /* 0 = not calibrated */
    ULONGLONG           VibDeadline;         /* KeQueryInterruptTime of the current run's end (stale timer guard) */
    BOOLEAN             VibOn;
    BOOLEAN             VibPmicInit;
} S10E_BATT_FDO, *PS10E_BATT_FDO;

#define S10E_BATT_HIST_SLOTS  16             /* Device Parameters Hist00..Hist15, written on every notify */

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(S10E_BATT_GLOBAL, S10eBattGetGlobal);
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(S10E_BATT_FDO, S10eBattGetFdo);

#define S10eBattPrint(Level, Fmt, ...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, (Level), "S10eBattery: " Fmt, __VA_ARGS__)

/* Hsi2c.c - port of Pmic.c (bus setup / reset / polled transfer / release). Caller holds BusLock. */
VOID     S10eHsi2cBusInit(_Inout_ PS10E_BATT_FDO Fdo);
VOID     S10eHsi2cBusRelease(_Inout_ PS10E_BATT_FDO Fdo);
NTSTATUS S10eHsi2cXfer(_Inout_ PS10E_BATT_FDO Fdo, _In_ UCHAR SlaveAddr, _In_ BOOLEAN Read,
                       _Inout_updates_(Length) PUCHAR Data, _In_ ULONG Length, _In_ BOOLEAN Stop);
NTSTATUS S10eMax77705ReadN(_Inout_ PS10E_BATT_FDO Fdo, _In_ UCHAR ClientAddr, _In_ UCHAR Reg,
                           _Out_writes_(Length) PUCHAR Buf, _In_ ULONG Length);

/* Max77705.c - one complete reading. Caller holds BusLock. */
NTSTATUS S10eMax77705Read(_Inout_ PS10E_BATT_FDO Fdo, _Out_ PS10E_BATT_READING Reading);
VOID     S10eMax77705Tune(_Inout_ PS10E_BATT_FDO Fdo, _In_ PS10E_BATT_READING Reading);   /* BusLock held */

/* Vibra.c */
NTSTATUS S10eVibCreate(_Inout_ PS10E_BATT_FDO Fdo);                 /* AddDevice: lock, timer, work item */
NTSTATUS S10eVibIoctl(_Inout_ PS10E_BATT_FDO Fdo, _Inout_ PIRP Irp);
VOID     S10eVibShutdown(_Inout_ PS10E_BATT_FDO Fdo);               /* D0Exit: motor off */
VOID     S10eVibUnmap(_Inout_ PS10E_BATT_FDO Fdo);                  /* ReleaseHardware */

/* Miniclass.c */
VOID     S10eBattPoll(_In_ WDFDEVICE Device);
VOID     S10eBattTick(_In_ WDFDEVICE Device);
VOID     S10eBattInitState(_Inout_ PS10E_BATT_FDO Fdo);
BCLASS_QUERY_TAG_CALLBACK             S10eBattQueryTag;
BCLASS_QUERY_INFORMATION_CALLBACK     S10eBattQueryInformation;
BCLASS_SET_INFORMATION_CALLBACK       S10eBattSetInformation;
BCLASS_QUERY_STATUS_CALLBACK          S10eBattQueryStatus;
BCLASS_SET_STATUS_NOTIFY_CALLBACK     S10eBattSetStatusNotify;
BCLASS_DISABLE_STATUS_NOTIFY_CALLBACK S10eBattDisableStatusNotify;
