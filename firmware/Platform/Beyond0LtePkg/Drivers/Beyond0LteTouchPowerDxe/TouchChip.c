/** @file
  TouchChip.c - bring the S10's touch controller up in UEFI, before Windows (beyond1lte, v0.1.7).

  The S20 Ultra port (z3s) showed that Windows touch is reliable only when the firmware has already
  found and started the panel IC: the Windows driver then attaches to a controller that is known to
  answer, in application mode, with its event stack empty. This file does that on the S10, at
  ReadyToBoot, right after Beyond0LteTouchPowerDxe has switched the tsp_io / tsp_avdd rails on:

    1. HSI2C bus of the panel (hsi2c@104B0000 = USI03 of PERIC0, the same bus \_SB.I2C0 gives
       Windows): USI03 clock gates + Q-channel, USI mode = I2C, gpp1-4/5 = HSI2C function, no pulls,
       controller reset, 400 kHz master - the exact order of Exynos9820Hsi2c ControllerConfigurePlatform
       + ControllerReset (device-validated in Windows).
    2. sec_ts y771 probe order (LineageOS android_kernel_samsung_exynos9820, drivers/input/
       touchscreen/sec_ts/y771/sec_ts.c sec_ts_probe / sec_ts_wait_for_ready): boot status 0x55 until
       0x20 (application mode); if it never gets there, SW reset 0x12 and wait for the
       "boot complete" status event again; then device id 0x22, firmware integrity 0x21,
       TS status 0xAF, firmware / image version 0xA3 / 0xA5 - all logged to UEFILOG.TXT.
    3. touch function |= TOUCH | PALM | WET (0x30, 2 bytes LE), sense on (0x10), empty event stack.
    4. Touch test (the z3s one): a bar at the top of the screen for TOUCH_TEST_ARM_MS - green = the
       controller answered, red = it did not. Touch the screen while it is shown and the next
       TOUCH_TEST_RUN_MS every finger draws white squares where the firmware sees it. This proves the
       panel works with no Windows driver involved.
  At the end the event stack is emptied again and the controller is left sensing, in application
  mode, for the Windows driver (S10Touch re-runs touch function + sense on itself).

  Every I2C wait is bounded; any failure is logged and the boot continues.
  v0.1.8: the v0.1.7 test drew, but far behind the finger (user, 2026-11-02): the reader was slower than the
  panel. The USI03 kernel clock is now set to the 200 MHz the HSI2C timing assumes (TsClockFix, with a
  measured before/after and a fallback), transfers poll every 5 us, the drawing loop only sleeps when the
  event stack is empty, and the log reports events/s and the deepest backlog (left_event) seen.
  NOT VALIDATED ON HARDWARE (stage Guided) until the UEFILOG of v0.1.8 is read.
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Protocol/GraphicsOutput.h>
#include <Platform/Exynos9820.h>

#include "../Beyond0LteAudioProbeDxe/AudioProbeHw.h"   // Exynos HSI2C register set (device-validated, UsbHw.h)
#include "TouchChip.h"

//
// Bus. MMIO bases only from Exynos9820.h (AGENTS.md rule 6); register offsets and bit values from the
// Windows Exynos9820Hsi2c driver's exynos9820hsi2c.h (cal-if cmucal-sfr.c citations there).
//
#define TS_I2C            ((UINTN)EXYNOS_TOUCH_HSI2C_BASE)
#define TS_CMU            ((UINTN)EXYNOS_CMU_PERIC0_BASE)
#define TS_SYSREG         ((UINTN)EXYNOS_PERIC0_USI_SYSREG_BASE)
#define TS_GPIO           ((UINTN)EXYNOS_PERIC0_GPIO_BASE)
#define TS_ALIVE          ((UINTN)EXYNOS_ALIVE_GPIO_BASE)
#define TS_ADDR           ((UINT8)EXYNOS_TOUCH_I2C_ADDR)

#define CMU_GATE_USI03_SOURCE      0x2008u   // RSTNSYNC_CLK_PERIC0_OSCCLK_IPCLKPORT_CLK
#define CMU_GATE_USI03_RESET_SYNC  0x2038u   // RSTNSYNC_CLK_PERIC0_USI03_USI_IPCLKPORT_CLK
#define CMU_GATE_USI03_IPCLK       0x209Cu   // USI03_USI_IPCLKPORT_IPCLK
#define CMU_GATE_USI03_PCLK        0x20A0u   // USI03_USI_IPCLKPORT_PCLK
#define CMU_QCH_USI03              0x303Cu   // QCH_CON_USI03_USI_QCH
#define CMU_QCH_REQUIRED           (BIT0 | BIT1)   // ENABLE | CLOCK_REQUEST
#define SYSREG_USI_MODE_I2C        0x4u

//
// v0.1.8: the USI03 kernel clock itself (cal-if exynos9820 cmucal-sfr.c / cmucal-node.c):
//   PLL_CON0_MUX_CLKCMU_PERIC0_USI03_USI_USER 0x1E0: MUX_SEL bit 4 (0 = OSCCLK_PERIC0 26 MHz,
//     1 = CLKCMU_PERIC0_IP), BUSY bit 7
//   CLK_CON_DIV_DIV_CLK_PERIC0_USI03_USI 0x1810: DIVRATIO [3:0] (divide by ratio + 1), BUSY bit 16
// The HSI2C timing (here, and in the Windows Exynos9820Hsi2c driver) is computed for a 200 MHz ipclk = the DT
// default-clk, which i2c-exynos5.c reaches with clk_set_rate(): CLKCMU_PERIC0_IP 400 MHz (cmucal-vclk.c UUD LUT
// cmucal_vclk_mux_clkcmu_peric0_ip_lut 400000 kHz) / 2. Nothing outside Linux sets that rate, so v0.1.8 does
// it: USER mux -> CLKCMU_PERIC0_IP, DIVRATIO 1. The 400 MHz parent is UNVERIFIED (CMU_TOP is only mapped
// 0x1000 here, its PERIC0_IP mux/div at 0x10BC/0x18A8 cannot be read) - so the result is checked by timing
// real transfers with the 26 MHz arch timer, and the old setting is put back if the bus stops answering.
//
#define CMU_USI03_USER_MUX         0x01E0u
#define   CMU_USER_MUX_SEL           BIT4
#define   CMU_USER_MUX_BUSY          BIT7
#define CMU_USI03_DIV              0x1810u
#define   CMU_DIV_RATIO_MASK         0xFu
#define   CMU_DIV_BUSY               BIT16
#define OSCCLK_HZ                  26000000u    // EXYNOS_ARCH_TIMER_FREQ_HZ source, OSCCLK_PERIC0
#define PERIC0_IP_HZ               400000000u   // UNVERIFIED (cal-if UUD LUT)

#define GPP1_CON                   0x020u
#define GPP1_PUD                   0x028u
#define GPP1_DRV                   0x02Cu
#define GPP1_I2C_MASK              ((0xFu << 16) | (0xFu << 20))   // pins 4 (SCL) and 5 (SDA)
#define GPP1_I2C_FUNC              ((0x2u << 16) | (0x2u << 20))

#define GPA1_DAT                   0x024u    // ALIVE gpa1, bit 0 = touch attention line (low = event pending)

#define TS_I2C_TIMEOUT_US          50000u

//
// sec_ts y771 (drivers/input/touchscreen/sec_ts/y771/sec_ts.h).
//
#define SEC_TS_CMD_SENSE_ON              0x10
#define SEC_TS_CMD_SW_RESET              0x12
#define SEC_TS_READ_FIRMWARE_INTEGRITY   0x21
#define SEC_TS_READ_DEVICE_ID            0x22
#define SEC_TS_CMD_SET_TOUCHFUNCTION     0x30
#define SEC_TS_READ_BOOT_STATUS          0x55
#define SEC_TS_READ_ONE_EVENT            0x60
#define SEC_TS_CMD_CLEAR_EVENT_STACK     0x62
#define SEC_TS_READ_FW_VERSION           0xA3
#define SEC_TS_READ_IMG_VERSION          0xA5
#define SEC_TS_READ_TS_STATUS            0xAF
#define SEC_TS_STATUS_APP_MODE           0x20
#define SEC_TS_ACK_BOOT_COMPLETE         0x00
#define SEC_TS_SETFUNC_DEFAULT           0x0061   // TOUCH | PALM | WET (SEC_TS_DEFAULT_ENABLE_BIT_SETFUNC)
#define SEC_TS_EVENT_SIZE                8
#define SEC_TS_WAIT_TRIES                50       // x 20 ms, as sec_ts_wait_for_ready
#define SEC_TS_COORD_RANGE               4096     // sec,max_coords = <4096 4096>

//
// v0.1.9 (user, 2026-11-03: touch is smooth in Windows now, "remove the green bar and the touch wait screen"):
// the boot touch test is off - no bar, no 3 s wait. 1 brings it back for bring-up work.
//
#ifndef BEYOND1LTE_TOUCH_TEST
#define BEYOND1LTE_TOUCH_TEST            0
#endif
#define TOUCH_TEST_ARM_MS                3000
#define TOUCH_TEST_RUN_MS                10000
#define TOUCH_TEST_BAR_HEIGHT            48
#define TOUCH_TEST_DOT                   24

// -------------------------------------------------------------------------------------------------
// HSI2C master (polled). Same transfer as Beyond0LteAudioProbeDxe I2cXfer, on the touch bus.
// -------------------------------------------------------------------------------------------------

STATIC
VOID
TsI2cReset (
  VOID
  )
{
  MmioWrite32 (TS_I2C + HSI2C_INT_EN, 0);
  MmioOr32 (TS_I2C + HSI2C_CTL, HSI2C_CTL_SW_RST);
  MmioAnd32 (TS_I2C + HSI2C_CTL, ~HSI2C_CTL_SW_RST);
  MmioWrite32 (TS_I2C + HSI2C_USI_CON, 0);
  MmioWrite32 (TS_I2C + HSI2C_TIMING_FS1, HSI2C_TIMING_FS1_400K);
  MmioWrite32 (TS_I2C + HSI2C_TIMING_FS2, HSI2C_TIMING_FS2_400K);
  MmioWrite32 (TS_I2C + HSI2C_TIMING_FS3, HSI2C_TIMING_FS3_400K);
  MmioWrite32 (TS_I2C + HSI2C_CTL, HSI2C_CTL_MASTER);
  MmioWrite32 (TS_I2C + HSI2C_TRAIL_CTL, 0xFFFFFF);
  MmioWrite32 (TS_I2C + HSI2C_TIMEOUT, 0);
  MmioWrite32 (TS_I2C + HSI2C_CONF, HSI2C_CONF_AUTO_MODE);
  MmioWrite32 (TS_I2C + HSI2C_FIFO_CTL, 0);
  MmioWrite32 (TS_I2C + HSI2C_AUTO_CONF, 0);
  MmioWrite32 (TS_I2C + HSI2C_ADDR, 0);
  MmioWrite32 (TS_I2C + HSI2C_INT_STAT, HSI2C_INT_ALL);
}

STATIC
VOID
TsBusInit (
  VOID
  )
{
  CONST UINT32  Gate = BIT20 | BIT21;   // EXYNOS_CMU_GATE_MANUAL | EXYNOS_CMU_GATE_VALUE

  DEBUG ((DEBUG_ERROR, "TouchChip: bus before: gates %x %x %x %x qch %x usi03 %x gpp1 con %x pud %x\n",
          MmioRead32 (TS_CMU + CMU_GATE_USI03_SOURCE), MmioRead32 (TS_CMU + CMU_GATE_USI03_RESET_SYNC),
          MmioRead32 (TS_CMU + CMU_GATE_USI03_IPCLK), MmioRead32 (TS_CMU + CMU_GATE_USI03_PCLK),
          MmioRead32 (TS_CMU + CMU_QCH_USI03), MmioRead32 (TS_SYSREG + EXYNOS_USI03_MODE_OFFSET),
          MmioRead32 (TS_GPIO + GPP1_CON), MmioRead32 (TS_GPIO + GPP1_PUD)));

  MmioOr32 (TS_CMU + CMU_GATE_USI03_SOURCE, Gate);
  MmioOr32 (TS_CMU + CMU_GATE_USI03_RESET_SYNC, Gate);
  MmioOr32 (TS_CMU + CMU_GATE_USI03_IPCLK, Gate);
  MmioOr32 (TS_CMU + CMU_GATE_USI03_PCLK, Gate);
  MmioOr32 (TS_CMU + CMU_QCH_USI03, CMU_QCH_REQUIRED);
  MmioWrite32 (TS_SYSREG + EXYNOS_USI03_MODE_OFFSET, SYSREG_USI_MODE_I2C);
  MmioAndThenOr32 (TS_GPIO + GPP1_CON, ~GPP1_I2C_MASK, GPP1_I2C_FUNC);
  MmioAnd32 (TS_GPIO + GPP1_PUD, ~GPP1_I2C_MASK);
  MmioAnd32 (TS_GPIO + GPP1_DRV, ~GPP1_I2C_MASK);
  __asm__ __volatile__ ("dsb sy" ::: "memory");
  TsI2cReset ();
}

STATIC
EFI_STATUS
TsXfer (
  IN     BOOLEAN  Read,
  IN OUT UINT8    *Data,
  IN     UINTN    Length,
  IN     BOOLEAN  Stop
  )
{
  UINT32  Ctl;
  UINT32  AutoConf;
  UINT32  Addr;
  UINT32  IntStat;
  UINTN   Index;
  UINTN   Us;

  if ((Length == 0) || (Length > 16)) {
    return EFI_INVALID_PARAMETER;
  }

  MmioAnd32 (TS_I2C + HSI2C_TIMEOUT, ~BIT31);
  MmioWrite32 (TS_I2C + HSI2C_FIFO_CTL, HSI2C_FIFO_CTL_RX_EN | HSI2C_FIFO_CTL_TX_EN |
               HSI2C_FIFO_CTL_RX_TRIG (Length) | HSI2C_FIFO_CTL_TX_TRIG (Length));

  Ctl  = MmioRead32 (TS_I2C + HSI2C_CTL) | HSI2C_CTL_MASTER;
  Ctl &= ~(HSI2C_CTL_RXCHON | HSI2C_CTL_TXCHON);
  Ctl |= Read ? HSI2C_CTL_RXCHON : HSI2C_CTL_TXCHON;
  MmioWrite32 (TS_I2C + HSI2C_CTL, Ctl);

  AutoConf  = MmioRead32 (TS_I2C + HSI2C_AUTO_CONF) & ~(0xFFFFu | HSI2C_AUTO_CONF_READ_WRITE | HSI2C_AUTO_CONF_STOP_AFTER_TRANS);
  AutoConf |= (UINT32)Length;
  if (Read) {
    AutoConf |= HSI2C_AUTO_CONF_READ_WRITE;
  }

  if (Stop) {
    AutoConf |= HSI2C_AUTO_CONF_STOP_AFTER_TRANS;
  }

  Addr  = MmioRead32 (TS_I2C + HSI2C_ADDR) & ~((0x3FFu << 10) | 0x3FFu | (0xFFu << 24));
  Addr |= ((UINT32)(TS_ADDR & 0x7F) << 10) | (0x7u << 24);
  MmioWrite32 (TS_I2C + HSI2C_ADDR, Addr);

  for (Us = 0; (MmioRead32 (TS_I2C + HSI2C_TRANS_STATUS) & HSI2C_MASTER_BUSY) != 0; Us += 100) {
    if (Us >= TS_I2C_TIMEOUT_US) {
      TsI2cReset ();
      return EFI_DEVICE_ERROR;
    }

    gBS->Stall (100);
  }

  MmioWrite32 (TS_I2C + HSI2C_INT_STAT, HSI2C_INT_ALL);
  MmioWrite32 (TS_I2C + HSI2C_INT_EN, HSI2C_INT_TRANSFER_DONE | HSI2C_INT_ERRORS);
  MmioWrite32 (TS_I2C + HSI2C_AUTO_CONF, AutoConf | HSI2C_AUTO_CONF_MASTER_RUN);

  if (!Read) {
    for (Index = 0; Index < Length; Index++) {
      MmioWrite8 (TS_I2C + HSI2C_TX_DATA, Data[Index]);
    }
  }

  IntStat = 0;
  for (Us = 0; Us < TS_I2C_TIMEOUT_US; Us += 5) {
    IntStat = MmioRead32 (TS_I2C + HSI2C_INT_STAT);
    if ((IntStat & (HSI2C_INT_TRANSFER_DONE | HSI2C_INT_ERRORS)) != 0) {
      break;
    }

    gBS->Stall (5);
  }

  if ((IntStat & HSI2C_INT_ERRORS) != 0) {
    TsI2cReset ();
    return ((IntStat & (HSI2C_INT_NODEV | HSI2C_INT_NO_ACK)) != 0) ? EFI_NOT_FOUND : EFI_DEVICE_ERROR;
  }

  if ((IntStat & HSI2C_INT_TRANSFER_DONE) == 0) {
    TsI2cReset ();
    return EFI_TIMEOUT;
  }

  MmioWrite32 (TS_I2C + HSI2C_INT_STAT, IntStat);

  if (Read) {
    for (Index = 0; Index < Length; Index++) {
      for (Us = 0; HSI2C_FIFO_STAT_RX_LEVEL (MmioRead32 (TS_I2C + HSI2C_FIFO_STAT)) == 0; Us += 100) {
        if (Us >= TS_I2C_TIMEOUT_US) {
          TsI2cReset ();
          return EFI_TIMEOUT;
        }

        gBS->Stall (100);
      }

      Data[Index] = MmioRead8 (TS_I2C + HSI2C_RX_DATA);
    }
  }

  return EFI_SUCCESS;
}

// sec_ts_i2c_read: command byte, repeated start, Length bytes.
STATIC
EFI_STATUS
TsRead (
  IN  UINT8  Cmd,
  OUT UINT8  *Data,
  IN  UINTN  Length
  )
{
  EFI_STATUS  Status;

  ZeroMem (Data, Length);
  Status = TsXfer (FALSE, &Cmd, 1, FALSE);
  if (!EFI_ERROR (Status)) {
    Status = TsXfer (TRUE, Data, Length, TRUE);
  }

  return Status;
}

STATIC
EFI_STATUS
TsWrite (
  IN UINT8        Cmd,
  IN CONST UINT8  *Data   OPTIONAL,
  IN UINTN        Length
  )
{
  UINT8  Buf[8];

  if (Length > sizeof (Buf) - 1) {
    return EFI_INVALID_PARAMETER;
  }

  Buf[0] = Cmd;
  if (Length != 0) {
    CopyMem (&Buf[1], Data, Length);
  }

  return TsXfer (FALSE, Buf, Length + 1, TRUE);
}

STATIC
UINT64
TsTicks (
  VOID
  )
{
  UINT64  V;

  __asm__ __volatile__ ("isb\n\tmrs %0, cntpct_el0" : "=r" (V) :: "memory");
  return V;
}

// Microseconds since Start, from the 26 MHz arch counter (CNTFRQ_EL0 verified 26 MHz).
STATIC
UINT32
TsUsSince (
  IN UINT64  Start
  )
{
  return (UINT32)(((TsTicks () - Start) * 1000000ULL) / EXYNOS_ARCH_TIMER_FREQ_HZ);
}

STATIC
UINT32
TsIpclkHz (
  IN UINT32  Mux,
  IN UINT32  Div
  )
{
  return (((Mux & CMU_USER_MUX_SEL) != 0) ? PERIC0_IP_HZ : OSCCLK_HZ) / ((Div & CMU_DIV_RATIO_MASK) + 1);
}

STATIC
VOID
TsWaitClkIdle (
  VOID
  )
{
  UINTN  Us;

  for (Us = 0; Us < 1000; Us++) {
    if (((MmioRead32 (TS_CMU + CMU_USI03_USER_MUX) & CMU_USER_MUX_BUSY) == 0) &&
        ((MmioRead32 (TS_CMU + CMU_USI03_DIV) & CMU_DIV_BUSY) == 0)) {
      return;
    }

    gBS->Stall (1);
  }
}

// HSI2C fast-mode timing for an ipclk, exynos5_i2c_set_timing (HSI2C_FAST_SPD, 400 kHz) - the formula the
// FS*_400K constants were derived from for 200 MHz.
STATIC
VOID
TsSetTiming (
  IN UINT32  IpclkHz
  )
{
  UINT32  FsDiv;
  UINT32  High;
  UINT32  Start;

  FsDiv = (IpclkHz / (400000u * 15u)) & 0xFF;
  High  = (4u * (IpclkHz / 1000000u)) / ((FsDiv + 1) * 10);
  Start = (High > 0) ? High - 1 : 0;
  High  = (High > 7) ? 0 : ((0xFFu << High) & 0xFF);
  Start = (Start > 7) ? 0 : ((0xFFu << Start) & 0xFF);
  MmioAndThenOr32 (TS_I2C + HSI2C_TIMING_FS3, ~0x00FF0000u, FsDiv << 16);
  MmioAndThenOr32 (TS_I2C + HSI2C_TIMING_FS2, ~0x000000FFu, High);
  MmioAndThenOr32 (TS_I2C + HSI2C_TIMING_FS1, ~0x00FF0000u, Start << 16);
  DEBUG ((DEBUG_ERROR, "TouchChip: HSI2C timing for ipclk %u Hz: FS1 %x FS2 %x FS3 %x\n", IpclkHz,
          MmioRead32 (TS_I2C + HSI2C_TIMING_FS1), MmioRead32 (TS_I2C + HSI2C_TIMING_FS2),
          MmioRead32 (TS_I2C + HSI2C_TIMING_FS3)));
}

// Average microseconds of one 1-byte register read (cmd write + repeated start + 1 byte): ~40 SCL periods,
// ~100-150 us at 400 kHz. 0 = the bus does not answer.
STATIC
UINT32
TsMeasureReadUs (
  VOID
  )
{
  UINT64  T0;
  UINT8   B;
  UINTN   I;

  T0 = TsTicks ();
  for (I = 0; I < 20; I++) {
    if (EFI_ERROR (TsRead (SEC_TS_READ_BOOT_STATUS, &B, 1))) {
      return 0;
    }
  }

  return TsUsSince (T0) / 20;
}

//
// Put the USI03 kernel clock at the 200 MHz everything here is timed for; keep it only if the bus still answers.
//
STATIC
VOID
TsClockFix (
  VOID
  )
{
  UINT32  Mux0;
  UINT32  Div0;
  UINT32  UsBefore;
  UINT32  UsAfter;

  Mux0     = MmioRead32 (TS_CMU + CMU_USI03_USER_MUX);
  Div0     = MmioRead32 (TS_CMU + CMU_USI03_DIV);
  UsBefore = TsMeasureReadUs ();
  DEBUG ((DEBUG_ERROR, "TouchChip: USI03 clock as found: USER mux %x (%a) div %x (/%u) -> ipclk ~%u Hz (timing assumes 200 MHz),"
                       " 1-byte read %u us\n", Mux0, (Mux0 & CMU_USER_MUX_SEL) ? "PERIC0_IP" : "OSCCLK 26 MHz",
          Div0, (Div0 & CMU_DIV_RATIO_MASK) + 1, TsIpclkHz (Mux0, Div0), UsBefore));

  if (((Mux0 & CMU_USER_MUX_SEL) != 0) && ((Div0 & CMU_DIV_RATIO_MASK) == 1)) {
    DEBUG ((DEBUG_ERROR, "TouchChip: USI03 clock already PERIC0_IP / 2 - unchanged\n"));
    return;
  }

  // Divider first (a larger division while the faster parent is selected), then the mux (cal-if order irrelevant here).
  MmioAndThenOr32 (TS_CMU + CMU_USI03_DIV, ~CMU_DIV_RATIO_MASK, 1);
  TsWaitClkIdle ();
  MmioOr32 (TS_CMU + CMU_USI03_USER_MUX, CMU_USER_MUX_SEL);
  TsWaitClkIdle ();
  TsI2cReset ();
  UsAfter = TsMeasureReadUs ();
  DEBUG ((DEBUG_ERROR, "TouchChip: USI03 clock set: USER mux %x div %x -> 1-byte read %u us (was %u)\n",
          MmioRead32 (TS_CMU + CMU_USI03_USER_MUX), MmioRead32 (TS_CMU + CMU_USI03_DIV), UsAfter, UsBefore));

  if ((UsAfter == 0) && (UsBefore != 0)) {
    // The new clock does not drive the bus: back to what sboot left, with the timing computed for that clock.
    MmioWrite32 (TS_CMU + CMU_USI03_USER_MUX, Mux0 & ~CMU_USER_MUX_BUSY);
    TsWaitClkIdle ();
    MmioAndThenOr32 (TS_CMU + CMU_USI03_DIV, ~CMU_DIV_RATIO_MASK, Div0 & CMU_DIV_RATIO_MASK);
    TsWaitClkIdle ();
    TsI2cReset ();
    TsSetTiming (TsIpclkHz (Mux0, Div0));
    DEBUG ((DEBUG_ERROR, "TouchChip: bus dead on PERIC0_IP / 2 - clock restored, timing for the old clock: read %u us\n",
            TsMeasureReadUs ()));
  }
}

// -------------------------------------------------------------------------------------------------
// Controller bring-up
// -------------------------------------------------------------------------------------------------

STATIC
BOOLEAN
TsWaitAppMode (
  OUT UINT8  *Boot,
  OUT UINTN  *Tries
  )
{
  EFI_STATUS  Status;
  UINTN       Try;

  *Boot  = 0;
  Status = EFI_NOT_READY;
  for (Try = 0; Try < SEC_TS_WAIT_TRIES; Try++) {
    Status = TsRead (SEC_TS_READ_BOOT_STATUS, Boot, 1);
    if (!EFI_ERROR (Status) && (*Boot == SEC_TS_STATUS_APP_MODE)) {
      break;
    }

    gBS->Stall (20000);
  }

  *Tries = Try;
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "TouchChip: boot status read failed after %u tries: %r (no answer at 0x%02x)\n",
            Try, Status, TS_ADDR));
    return FALSE;
  }

  return *Boot == SEC_TS_STATUS_APP_MODE;
}

// sec_ts_wait_for_ready: READ_ONE_EVENT until an INFO (2) or VENDOR_INFO (7) status event with this ack.
STATIC
BOOLEAN
TsWaitForAck (
  IN UINT8  Ack
  )
{
  UINT8  Ev[SEC_TS_EVENT_SIZE];
  UINTN  Try;
  UINT8  Stype;

  for (Try = 0; Try < SEC_TS_WAIT_TRIES; Try++) {
    if (!EFI_ERROR (TsRead (SEC_TS_READ_ONE_EVENT, Ev, sizeof (Ev)))) {
      Stype = (Ev[0] >> 2) & 0xF;
      if ((Ev[0] != 0) && ((Stype == 2) || (Stype == 7)) && (Ev[1] == Ack)) {
        DEBUG ((DEBUG_ERROR, "TouchChip: ready ack %02x after %u tries: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                Ack, Try, Ev[0], Ev[1], Ev[2], Ev[3], Ev[4], Ev[5], Ev[6], Ev[7]));
        return TRUE;
      }
    }

    gBS->Stall (20000);
  }

  DEBUG ((DEBUG_ERROR, "TouchChip: no ready ack %02x in %u ms\n", Ack, SEC_TS_WAIT_TRIES * 20));
  return FALSE;
}

STATIC
VOID
TsLogInfo (
  VOID
  )
{
  UINT8  Id[5];
  UINT8  Integrity;
  UINT8  TsStatus[4];
  UINT8  Fw[4];
  UINT8  Img[4];

  if (!EFI_ERROR (TsRead (SEC_TS_READ_DEVICE_ID, Id, sizeof (Id)))) {
    DEBUG ((DEBUG_ERROR, "TouchChip: device id %02x %02x %02x %02x %02x\n", Id[0], Id[1], Id[2], Id[3], Id[4]));
  }

  if (!EFI_ERROR (TsRead (SEC_TS_READ_FIRMWARE_INTEGRITY, &Integrity, 1))) {
    DEBUG ((DEBUG_ERROR, "TouchChip: firmware integrity %02x (%a)\n", Integrity,
            (Integrity & 0x80) ? "valid" : (Integrity & 0x40) ? "INVALID firmware" : "unknown result"));
  }

  if (!EFI_ERROR (TsRead (SEC_TS_READ_TS_STATUS, TsStatus, sizeof (TsStatus)))) {
    DEBUG ((DEBUG_ERROR, "TouchChip: ts status %02x %02x %02x %02x\n", TsStatus[0], TsStatus[1], TsStatus[2], TsStatus[3]));
  }

  if (!EFI_ERROR (TsRead (SEC_TS_READ_FW_VERSION, Fw, sizeof (Fw)))) {
    DEBUG ((DEBUG_ERROR, "TouchChip: fw version %02x.%02x.%02x.%02x\n", Fw[0], Fw[1], Fw[2], Fw[3]));
  }

  if (!EFI_ERROR (TsRead (SEC_TS_READ_IMG_VERSION, Img, sizeof (Img)))) {
    DEBUG ((DEBUG_ERROR, "TouchChip: img version %02x.%02x.%02x.%02x\n", Img[0], Img[1], Img[2], Img[3]));
  }
}

STATIC
EFI_STATUS
TsStart (
  VOID
  )
{
  UINT8       Func[2];
  EFI_STATUS  Status;
  UINT16      F;

  F = 0;
  if (!EFI_ERROR (TsRead (SEC_TS_CMD_SET_TOUCHFUNCTION, Func, sizeof (Func)))) {
    F = (UINT16)(Func[0] | ((UINT16)Func[1] << 8));
  }

  DEBUG ((DEBUG_ERROR, "TouchChip: touch function %04x -> %04x\n", F, F | SEC_TS_SETFUNC_DEFAULT));
  F     |= SEC_TS_SETFUNC_DEFAULT;
  Func[0] = (UINT8)F;
  Func[1] = (UINT8)(F >> 8);
  Status  = TsWrite (SEC_TS_CMD_SET_TOUCHFUNCTION, Func, sizeof (Func));
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "TouchChip: touch function write: %r\n", Status));
  }

  Status = TsWrite (SEC_TS_CMD_SENSE_ON, NULL, 0);
  DEBUG ((DEBUG_ERROR, "TouchChip: sense on: %r\n", Status));
  gBS->Stall (50000);
  (VOID)TsWrite (SEC_TS_CMD_CLEAR_EVENT_STACK, NULL, 0);
  return Status;
}

// -------------------------------------------------------------------------------------------------
// Touch test on the boot framebuffer
// -------------------------------------------------------------------------------------------------

#if BEYOND1LTE_TOUCH_TEST
STATIC
VOID
TsFill (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop,
  IN UINT32                        Color,     // 0x00RRGGBB
  IN UINTN                         X,
  IN UINTN                         Y,
  IN UINTN                         W,
  IN UINTN                         H
  )
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Px;
  UINTN                          Hr;
  UINTN                          Vr;

  if (Gop == NULL) {
    return;
  }

  Hr = Gop->Mode->Info->HorizontalResolution;
  Vr = Gop->Mode->Info->VerticalResolution;
  if ((X >= Hr) || (Y >= Vr)) {
    return;
  }

  W = MIN (W, Hr - X);
  H = MIN (H, Vr - Y);
  Px.Blue     = (UINT8)Color;
  Px.Green    = (UINT8)(Color >> 8);
  Px.Red      = (UINT8)(Color >> 16);
  Px.Reserved = 0;
  (VOID)Gop->Blt (Gop, &Px, EfiBltVideoFill, 0, 0, X, Y, W, H, 0);
}

STATIC UINT32  mMaxLeft;

// Pops every pending event; draws a dot per press/move when Draw. Returns the number of fingers seen.
STATIC
UINTN
TsPoll (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop,
  IN BOOLEAN                       Draw,
  IN OUT UINTN                     *Events
  )
{
  UINT8   Ev[SEC_TS_EVENT_SIZE];
  UINTN   N;
  UINTN   Fingers;
  UINT32  X;
  UINT32  Y;
  UINTN   Hr;
  UINTN   Vr;

  Fingers = 0;
  for (N = 0; N < 32; N++) {
    if (EFI_ERROR (TsRead (SEC_TS_READ_ONE_EVENT, Ev, sizeof (Ev))) || (Ev[0] == 0)) {
      break;
    }

    (*Events)++;
    if ((UINT32)(Ev[7] & 0x1F) > mMaxLeft) {   // y771: left_event:5 - how far behind the reader is
      mMaxLeft = Ev[7] & 0x1F;
    }

    if ((Ev[0] & 0x3) != 0) {               // not a coordinate event
      continue;
    }

    if (((Ev[0] >> 6) != 1) && ((Ev[0] >> 6) != 2)) {   // press or move only
      continue;
    }

    Fingers++;
    X = ((UINT32)Ev[1] << 4) | (Ev[3] >> 4);
    Y = ((UINT32)Ev[2] << 4) | (Ev[3] & 0x0F);
    if (*Events <= 4) {
      DEBUG ((DEBUG_ERROR, "TouchChip: event %02x %02x %02x %02x %02x %02x %02x %02x -> tid %u x %u y %u\n",
              Ev[0], Ev[1], Ev[2], Ev[3], Ev[4], Ev[5], Ev[6], Ev[7], (Ev[0] >> 2) & 0xF, X, Y));
    }

    if (Draw && (Gop != NULL)) {
      Hr = Gop->Mode->Info->HorizontalResolution;
      Vr = Gop->Mode->Info->VerticalResolution;
      X  = (UINT32)((X * Hr) / SEC_TS_COORD_RANGE);
      Y  = (UINT32)((Y * Vr) / SEC_TS_COORD_RANGE);
      TsFill (Gop, 0xFFFFFF, (X > TOUCH_TEST_DOT / 2) ? X - TOUCH_TEST_DOT / 2 : 0,
              (Y > TOUCH_TEST_DOT / 2) ? Y - TOUCH_TEST_DOT / 2 : 0, TOUCH_TEST_DOT, TOUCH_TEST_DOT);
    }
  }

  return Fingers;
}

STATIC
VOID
TsTouchTest (
  IN BOOLEAN  ChipOk
  )
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;
  UINTN                         Ms;
  UINTN                         Events;
  BOOLEAN                       Armed;

  Gop = NULL;
  if (EFI_ERROR (gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop))) {
    Gop = NULL;
  }

  // Status bar: green = controller up, red = no answer.
  TsFill (Gop, ChipOk ? 0x00C853 : 0xD50000, 0, 0, MAX_UINTN, TOUCH_TEST_BAR_HEIGHT);
  if (!ChipOk) {
    gBS->Stall (1500 * 1000);
    TsFill (Gop, 0x000000, 0, 0, MAX_UINTN, TOUCH_TEST_BAR_HEIGHT);
    return;
  }

  Events = 0;
  Armed  = FALSE;
  for (Ms = 0; Ms < TOUCH_TEST_ARM_MS; Ms += 10) {
    if (TsPoll (Gop, FALSE, &Events) != 0) {
      Armed = TRUE;
      break;
    }

    gBS->Stall (10000);
  }

  DEBUG ((DEBUG_ERROR, "TouchChip: test window %u ms, %u events, %a\n", Ms, Events,
          Armed ? "touched - drawing" : "not touched"));
  if (Armed) {
    UINT64  T0;
    UINTN   Before;

    TsFill (Gop, 0x2962FF, 0, 0, MAX_UINTN, TOUCH_TEST_BAR_HEIGHT);   // blue bar = drawing mode
    Events   = 0;
    mMaxLeft = 0;
    T0       = TsTicks ();
    while (TsUsSince (T0) < TOUCH_TEST_RUN_MS * 1000u) {
      Before = Events;
      (VOID)TsPoll (Gop, TRUE, &Events);
      if (Events == Before) {
        gBS->Stall (1000);                  // stack empty: look again in 1 ms (the panel reports at ~120 Hz)
      }
    }

    DEBUG ((DEBUG_ERROR, "TouchChip: drawing done: %u events in %u ms (%u/s), deepest backlog %u events\n",
            Events, TOUCH_TEST_RUN_MS, (UINT32)((Events * 1000) / TOUCH_TEST_RUN_MS), mMaxLeft));
  }

  TsFill (Gop, 0x000000, 0, 0, MAX_UINTN, TOUCH_TEST_BAR_HEIGHT);
}

#endif // BEYOND1LTE_TOUCH_TEST

VOID
TouchChipBringUp (
  IN BOOLEAN  RailsJustEnabled
  )
{
  UINT8    Boot;
  UINTN    Tries;
  BOOLEAN  Ok;

  DEBUG ((DEBUG_ERROR, "TouchChip: start (rails %a, attention line gpa1-0 = %u)\n",
          RailsJustEnabled ? "just enabled" : "already on", MmioRead32 (TS_ALIVE + GPA1_DAT) & 1));

  TsBusInit ();

  Ok = TsWaitAppMode (&Boot, &Tries);
  if (Ok) {
    TsClockFix ();
  }
  DEBUG ((DEBUG_ERROR, "TouchChip: boot status %02x after %u tries (%a)\n", Boot, Tries,
          Ok ? "application mode" : "NOT in application mode"));
  if (!Ok) {
    //
    // Not answering or stuck (bootloader mode, flash mode, or left half-asleep by sboot): software
    // reset and the probe's wait for "boot complete", then ask again.
    //
    DEBUG ((DEBUG_ERROR, "TouchChip: SW reset: %r\n", TsWrite (SEC_TS_CMD_SW_RESET, NULL, 0)));
    gBS->Stall (70000);
    (VOID)TsWaitForAck (SEC_TS_ACK_BOOT_COMPLETE);
    Ok = TsWaitAppMode (&Boot, &Tries);
    DEBUG ((DEBUG_ERROR, "TouchChip: after reset boot status %02x after %u tries (%a)\n", Boot, Tries,
            Ok ? "application mode" : "STILL NOT in application mode"));
  }

  if (Ok) {
    TsLogInfo ();
    Ok = !EFI_ERROR (TsStart ());
  }

#if BEYOND1LTE_TOUCH_TEST
  TsTouchTest (Ok);
#endif

  if (Ok) {
    (VOID)TsWrite (SEC_TS_CMD_CLEAR_EVENT_STACK, NULL, 0);
  }

  DEBUG ((DEBUG_ERROR, "TouchChip: done - controller %a, attention line %u\n",
          Ok ? "sensing, stack empty" : "NOT started", MmioRead32 (TS_ALIVE + GPA1_DAT) & 1));
}
