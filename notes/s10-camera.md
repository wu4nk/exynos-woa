# S10 camera bring-up (started 2026-11-16) - S10eCam.sys + cam-s10.ps1

Plan (user 2026-11-15 "chuẩn bị cho camera"; precedent = Scion HakiOS S9 front camera, notes/s10-vibration.md end):
stage 1 read-only survey -> stage 2 sensor power + chip ID -> stage 3 CSIS link + one raw frame -> stage 4 debayer /
AE -> stage 5 a Windows camera driver. Front camera first (S5K3J1), like the S9 port.

## Facts (all from the live DT dump/s10-dump/dt/live.dts and LineageOS exynos9820 lineage-23.0, ref/k9820-full)
- Sensors (DT compatibles): 2L4 rear wide, 3M3 tele, 3P9 ultra-wide, 3J1 front. Front: fimc-is-3j1@10 on
  hsi2c@10880000 ("PERI1 CAM2" = I2C_CAM2, samsung,usi-i2c-v2, 1 MHz fast-plus in Android), rev_reg <0x2 0x2 0xA101>.
- beyond1lte defconfig: CONFIG_CAMERA_BSS_V01 -> fimc-is-vendor-config_bss_v01.h: USE_BUCK2_REGULATOR_CONTROL (no TOF
  IO denoise); CONFIG_FIMC_IS_V7_0_0 (ischain/fimc-is-v7_0_0); CONFIG_CAMERA_CIS_3J1_OBJ / 2L4.
- Module (sensor-module-3j1, live.dts:15614): mclk_ch 1, sensor_i2c_ch 4, gpio_mclk gpc1-5, gpio_reset gpg2-5,
  gpio_subcam_sel gpg4-3, pinctrl pin1 = sensor-mclk1-out (gpc1-5 f1 pud1), pin2 = sensor-mclk1-fn (gpc1-5 f2 pud0);
  no power_seq_id (-> 0). Power on/off order: fimc-is-device-module-3j1.c (copied into S10eCam.c's header).
- Rails on the S2MPB02 (s2mpb02_pmic@59 on hsi2c@10920000 = USI10_USI, usi@10821038 mode i2c, pins gpp6-0/1 f2):
  BUCK2 VDD_EXT_1P3_PB02 1.30 V, LDO5 VDDD_1.05V_VT, LDO6 VDDIO_1.8V_VT, LDO13 VDDA_2.95V_VT, LDO16 VDDAF_2.8V_VT.
  Register map = enum s2mpb02_reg (B2CTRL1 0x0D, B2CTRL2 0x0E, L5CTRL 0x22, L6 0x23, L13 0x2A, L16 0x2D, L17 0x2E);
  LDO: bit 7 enable, vsel (uV-600000)/12500 (LDO1-5) or /25000 (LDO6-18); BUCK2 enable [7:6], vsel (uV-400000)/6250.
  Same register numbers as the S9's (HakiOS) - independent agreement. Android dump regulators.txt: all five off.
  LDO17 VDD_GRIP_3P3 is always-on on the same chip: read, never written.
- MCLK: CIS_CLK1 of CMU_TOP (0x1A240000): mux 0x1014 [0] (OSCCLK / PLL_SHARED2_DIV2), div 0x1810 [4:0], gate 0x202C,
  DMYQCH 0x300C; exynos9820_fimc_is_sensor_mclk_on sets 26 MHz -> OSCCLK /1.
- Pin banks (pinctrl-exynos-arm64.c): PERIC1 0x10830000 gpp6 0x40, gpc0 0x60, gpc1 0x80; PERIC0 0x10430000 gpg2 0xC0,
  gpg4 0xE0. Sensor bus pins gpc0-4/5 f2 (hsi2c2-bus) / f0 (hsi2c2-bus-in).
- Bus clocks (CMU_PERIC1 0x10800000): I2C_CAM2 ipclk 0x2044 pclk 0x2048 rst 0x2010 QCH 0x301C div 0x1808 user mux
  0x160; USI10 ipclk 0x20E8 pclk 0x20EC rst 0x2078 QCH 0x3060 div 0x1828 user mux 0x260; CMU_TOP PERIC1_BUS 0x20C4.
- HSI2C: i2c-exynos5.c standard-mode timing (fs_div = ipclk / (100k * 16), FS2 = 0xFF << tSCL_H, FS1 = 0xFF <<
  tSTART_HD), USI_CON = 0 (USI_RESET released), auto mode. ipclk assumed 200 MHz (DT default-clk): a lower real
  clock only slows the bus.

## Driver (drivers/S10eCam, legacy service, demand start, IOCTL 0x8A54/0x801 -> ASCII report)
- Stage 1: CMU/pin/controller reads; PERIC1 bus gate checked first (refuses everything if not provably on); per-bus
  ipclk+pclk gate check (a bus whose clocks are not on is not touched); S2MPB02 bus set up (USI10 mode I2C, gpp6
  f2, controller reset) and the ID / BUCK2 / LDO5/6/13/16 / LDO17 read.
- Stage 2: refuses if a camera LDO is already on; power-up per the module file; ID reads (0x0000 model, 0x0002 rev
  -> 0xA101 expected, 0x0005 frame count -> 0xFF); power-down ALWAYS (also on error) + CIS_CLK1 registers restored.
- Syntax-checked against WDK 26100 km headers (clang, -Wall -Wextra -Wconversion clean). NOT built with EWDK, NOT run.

## 2026-11-16 03:04 stage 1 run (S10eCam 0.1)
- CMU_TOP CIS_CLK1 mux 0 (OSCCLK) div 0 gate 0x00200000 qch 0x2; PERIC1_BUS gate 0x00200000 (on per ra_get_gate).
- CMU_PERIC1 user muxes BUS/I2C_CAM2/USI10 = 0x10 (CMU_TOP clocks), dividers 0, all gates 0x00200000, QCH 0x003F0002.
- Pins: gpp6 CON 0x11 (gpp6-0/1 = outputs, DAT 3), gpc0 CON 0 (gpc0-4/5 inputs), gpc1 CON 0, gpg2 CON 0x1 DAT 0xD,
  gpg4 CON 0.
- I2C_CAM2 as found: CTL 0x8 (master) CONF 0x980110FF FS1 0x0180FF00 FS2 0x030003F8 FS3 0x00FF0000 USI_CON 1.
- USI10 mode was 0 (set 4), USI_CON 1; S2MPB02 ID read: STATUS_IO_TIMEOUT with INT_STAT 000.
- Cause: INT_STAT shows only enabled sources; Linux writes INT_ENABLE = TRANSFER_DONE even in polling mode
  (i2c-exynos5.c:775), S10eBattery sets DONE|ERRORS, 0.1 left INT_EN 0. 0.2: INT_EN = DONE | ERRORS around the
  transfer, timeouts log TRANS_STATUS / FIFO / ERR, and CMU_TOP PERIC1_IP mux/div/gate (0x10C4/0x18B0/0x20C8, the
  ipclk source with user mux 1) is logged.
