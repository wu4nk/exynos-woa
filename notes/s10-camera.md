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

## 2026-11-16 03:14 stage 1 PASS (S10eCam 0.2)
- INT_EN fix confirmed: S2MPB02 answers on USI10 (mode 4, gpp6 f2, FS3 div 0x7D).
- CMU_TOP PERIC1_IP mux 1 (PLL_SHARED2_DIV2) div /2 gate on -> both I2C ipclks come from it (user mux 0x10, div 0);
  the real ipclk is not 200 MHz necessarily (PLL rate not read) - the bus works, any rate up to 2x only means
  <= ~200 kHz SCL with the standard-mode timings.
- S2MPB02: ID 0x00, B2CTRL1 0x18 (off), B2CTRL2 0x90 (1.30 V), LDO5 0x24 (1.05 V), LDO6 0x30 (1.80 V), LDO13 0x5E
  (2.95 V), LDO16 0x58 (2.80 V) - all off, every vsel EXACTLY the value computed from the DT and the regulator
  driver's min/step (= the register map and encodings are right). LDO17 0xDE (on, 2.95 V: VDD_GRIP_3P3 2.8-3.0 V).
- Ready for stage 2 (all camera rails off, sensor bus clocks on).

## 2026-11-16 03:16 STAGE 2 PASS - the front sensor answers under Windows
- Power-up writes all read back: B2CTRL1 0x18 -> 0xD8 (BUCK2 on), L16 0xD8, L13 0xDE, L6 0xB0, L5 0xA4.
- CIS_CLK1 gate 0x00200000 -> 0x00300000 (MANUAL + CG_VAL), mux/div 0 = 26 MHz; pins gpc0-4/5 f2, gpc1-5 f2,
  gpg2-5 output high, gpg4-3 output 0.
- Sensor 0x10 on I2C_CAM2: MODEL ID 0x30A1, REVISION 0xA101 (= DT rev_reg), FRAME COUNT 0xFF (not streaming).
- Power-down: every rail back to its vsel with enable off, BUCK2 0x18, CIS_CLK1 gate restored 0x00200000; gpc1-5
  left f1 output low (sensor-mclk1-out), gpg2-5 output low, gpg4-3 output 0.

## Stage 3 facts (2026-11-16, before any code) - the CSI receive path for the front camera
- DT: the 3J1 is sensor id 1 = node fimc_is_sensor2@17050000 (alias fimc_is_sensor1): csi "reg" 0x17050000 (CSIS2,
  csi_ch 2) + "phy" 0x19160000/0x10000; dma_ch 2 (mode0) -> sensor1-ch-mode0 VC0 regs 0x170C0000 (DMA0) + 0x170C0400
  (DMA common); mode1 VC1-3 at 0x17120100.. (PDP_TOP_DMA). iommus = sysmmu@17210000 (port "3AA0, 3AA1, CSIS,
  PDP_STAT", secure_base 0x17200000, samsung,power-domain = pd-isppre). phys = dphy_m0s4s4s4_csis2: regs
  0x19162300/0x500, PMU isolation 0x730, reset = ISPPRE sysreg 0x17020500 bit 1 ("samsung,exynos9810-isppre"
  syscon). (fimc_is_sensor@17050000 = id 2 shares the same CSIS2.)
- Power: pd-isppre@15862500 cal_id 0xB1380008, need_smc 0x17010204, parent = phandle 0x3 = pd-dpu@15861E00
  (need_smc 0x191B0204). K flexpmu_cal_local_exynos9820.h isppre_on: PMU 0x15860000 + 0x2500 bit0 <- 1, wait +0x2504
  bit0 == 1 (DPU: +0x1E00 / +0x1E04). After a need_smc domain powers on, Linux issues SMC_CMD_PREAPRE_PD_ONOFF
  0x82000410 (include/linux/smc.h) so EL3 re-programs that block's TZPC. PMU = system-controller@15860000.
- The MIPI PHY block 0x19160000 is shared with DSIM0 (dphy_m4s4_dsim0@0x19160000; dsim reg includes 0x19160000) =
  inside the DPU block -> readable only with pd-dpu on (the panel runs, so it should be).
- CMU_ISPPRE 0x17000000 (K cmucal-sfr.c): BUS_USER mux 0x100, DIV BUSP 0x1804, gates (21 CG_VAL, 20 MANUAL):
  CMU pclk 0x2004, ACLK_CSIS2 0x202C, ACLK_CSISX4_PDP_DMA 0x2038, ACLK_PDP_TOP_DMA 0x2040, ACLK_SYSMMU 0x2060,
  LHM_AXI_P 0x20B4, SYSREG pclk 0x20D8; QCH CSIS2 0x3054, CSISX4_PDP_DMA 0x305C, PDP_TOP_DMA 0x3064, SYSMMU 0x3088,
  LHM_AXI_P 0x30A4, SYSREG 0x30BC. CMU_TOP feeding it: MUX_CLKCMU_ISPPRE_BUS 0x1094, DIV 0x1884, GATE 0x20A0.
- CSIS v5.2 (fimc-is-hw-csi-v5_2.h): VERSION 0x0, CMN_CTRL 0x4, CLK_CTRL 0x8, INT_MSK0/SRC0 0x10/0x14, MSK1/SRC1
  0x18/0x1C, ISP_CONFIG_CH0 0x40, ISP_RESOL_CH0 0x44, FRM_CNT_CH0 0x100, PHY_STATUS 0x700, PHY_CMN_CTRL 0x704.
  DMA: DMA0_CTRL 0x0, FMT 0x4, ADDR1 0x10, ACT_CTRL 0x90, RESOL 0xA4, FCNTSEQ 0xB0, FCNTSEQ_STAT 0xB4; DMA common:
  DATA_CTRL 0x0, INT_MASK 0x4, INT_SRC 0x8, CMN_CTRL 0x14, ERR_CODE 0x18.
- SysMMU (K exynos-iommu.h): MMU_CTRL 0x0 (5 enable, 7 block, 0 disable), MMU_CFG 0x4, MMU_STATUS 0x8, CAPA 0x30,
  VERSION 0x34. Only the non-secure 0x17210000 frame is read (never secure_base).
- D-PHY (phy-exynos-mipi.c): isolation = PMU bit0 (1 bypass = PHY usable); reset = sysreg bit, 1 = released;
  0502_x config: SC_GNR_CON0 0x0, SC_GNR_CON1 0x4, ANA_CON0..2 0x8..0x10, SD_TIME_CON0 0x30 (settle), per lane +0x100.

### Stage 3a plan (read-only, S10eCam 0.3)
Read PMU DPU/ISPPRE CONFIGURATION+STATUS and MIPI PHY isolation. Domain off -> report, touch nothing inside it (an
access to a powered-off block = SError/hang). Domain on -> CMU_ISPPRE (incl. CMU_TOP ISPPRE_BUS), then each block
only if its gate is provably on: CSIS2, CSIS DMA0 + common, SysMMU, ISPPRE sysreg; D-PHY only if pd-dpu is on.
