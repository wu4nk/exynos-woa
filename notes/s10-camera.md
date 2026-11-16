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

## S10eCam 0.3 - stage 3 (3a survey) written (2026-11-16)
- Stage 3 is independent of 1/2 (no PERIC1 / PMIC access). Always: PMU pd-dpu / pd-isppre CONF+STATUS, CSIS2 D-PHY
  isolation (PMU +0x730), CMU_TOP ISPPRE_BUS mux/div/gate. Stops there unless S10ECAM_FLAG_DEEP (cam-s10.ps1 '3deep').
- Deep (only with both domains ON and the CMU_TOP gate on): CMU_ISPPRE BUS_USER/BUSP + 7 gates/QCHs; LHM_AXI_P gate
  must be on, else stop; then SYSREG +0x500 (pclk gate), CSIS2 (aclk), CSIS DMA0 + common (CSISX4_PDP_DMA aclk),
  SysMMU non-secure frame (SYSMMU aclk). Each block skipped if its gate is MANUAL+!CG_VAL.
- Why 'deep' is a separate run: Linux restores D_TZPC (SMC 0x82000410) after power-on BEFORE its first CMU_ISPPRE
  access (K pmucal_local.c pmucal_local_enable: on-seq -> exynos_pd_tz_restore -> restore_seq). If the domain is on
  with a secure TZPC, a non-secure read = bus error -> SError -> bugcheck (reboot, nothing written).
- The D-PHY registers (0x19162300) are NOT read: DPU block, next to the panel's DSIM PHY; 3b programs them anyway.
- cam-s10.ps1 v1.1: actions 3 / 3deep (Flags at input offset 4), log per action C:\S10Prov\cam-s10-<action>.txt.
- Checks: clang -fsyntax-only vs WDK 26100 km (-Wall -Wextra -Wconversion) clean; pwsh parser 0 errors; the embedded
  C# compiles (Run(Int32, Int32)). NOT built with EWDK, NOT run.
- Review of the 0.3 commit: offsets re-derived (CMU_TOP map base 0x1A241000 + 0x94/0x884/0x10A0 = 0x1094/0x1884/
  0x20A0; DIV ratio 4 bits, ISPPRE_BUS select 2 bits per cmucal-sfr.c); escapes in the VERDICT strings correct;
  stage 3 never touches FOUND/MAPS. Found: the CSIS DMA read was gated only on ACLK_CSISX4_PDP_DMA although nothing
  read so far ties 0x170C0000 to that gate (vs PDP_TOP_DMA). Fixed in a follow-up: both DMA aclks must be on.

## 2026-11-16 03:44 stage 3deep -> BUGCHECK (user report: "sau khi chạy 3deep thì màn xanh luôn")
- 3deep only reads inside ISPPRE when PMU says pd-dpu AND pd-isppre are ON and CMU_TOP's ISPPRE_BUS gate is on, so
  the domain was ON and one of the reads inside ISPPRE faulted (first candidate: the very first CMU_ISPPRE read).
  Matches the risk written in 0.3: Linux restores D_TZPC (SMC 0x82000410) before its first CMU_ISPPRE access; an
  NS read of a block still marked secure = bus error -> SError -> bugcheck. Not yet proven: the stage-3 log
  (cam-s10-3.txt) and the bugcheck code (System log event 1001 / C:\Windows\Minidump) were asked for.
- Lesson: no more reads inside ISPPRE from Windows until the TZPC question is settled. Next options: (a) per-read
  step mode with the "about to read X" line flushed to disk before each IOCTL, to name the faulting register; (b) the
  UEFI (EL2/EL1 before ExitBootServices, ArmCallSmc like G3D) powers ISPPRE and issues 0x82000410 itself.

### 2026-11-16 03:51 follow-up: logs after the bugcheck
- No System event 1001 and no Kernel-Power event 41 at all, no C:\Windows\Minidump: our UFS driver has no crash-dump
  path, and the event log / file data written just before the crash was evidently not flushed either.
- cam-s10-3.txt (03:43:51, S10eCam 0.3): pd-dpu CONF 1 STATUS 1 (ON); pd-isppre CONF 1 STATUS 1 (ON - left on since
  reset/sboot, nobody under Windows powered it); CSIS2 D-PHY isolation PMU+0x730 = 0 (isolated); CMU_TOP ISPPRE_BUS
  mux 3, div 0xF (/16), gate 0x00200000 (on per ra_get_gate).
- cam-s10-3deep.txt is dated 03:36:02 (BEFORE the 03:43 '3' run) and holds only "driver: running" + "log:" = no
  report. Reading: that run was in the old PowerShell window where the v1.0 S10Cam type (Run(int)) was already
  loaded -> "[S10Cam]::Run($stage, $flags)" failed on the console, $rep empty, the driver never called. The 3deep
  that bugchecked came after 03:43:51 and its file writes were lost in the crash (same as the event log). INFERENCE.
- So: ISPPRE is ON with a TZPC state nobody restored since reset, and a read inside it bugchecks. Same shape as G3D
  (gpu-m16-g3don.md H4: first NS CMU_G3D read depends on the DTZPC restore). Linux never touches a need_smc domain
  without exynos_pd_tz_restore after power-on; the S9 HakiOS camera also did ISPPRE on + 0x82000410 restore.
- ISPPRE pmucal (flexpmu_cal_local_exynos9820.h:600-699): on = CONF(+0x2500) bit0 1, wait STATUS(+0x2504) bit0 1;
  off = CONF 0, wait STATUS 0; save list = CMU_ISPPRE BUSP div 0x1804 + BUS_USER 0x100 (only restored after a save).
- Candidate fix, Linux's runtime cycle in the UEFI (as GpuPower.c does for G3D): tz_save SMC(0x82000410, 0,
  0x17010204, 2) -> ISPPRE off -> on -> tz_restore SMC(0x82000410, 1, 0x17010204, 2). Not written yet.

### 2026-11-16 ~04:00 re-think + S10eCam 0.4 (step mode)
- The S9 precedent weakens the TZPC-only reading: hakios.exynos_camera.c used a FOUND-ON ISPPRE "as it is, no SMC"
  and read CMU_ISPPRE / CSIS / WDMA fine, but its CMU_ISPPRE Q-channel sweep read of 0x303C (9810) took a synchronous
  external abort, and it kept SysMMU reads off ("its clock or Q-channel may be off, and a read then aborts"). Our 0.3
  read QCH registers AND the SysMMU. So the faulting read is unknown: TZPC or one register; a UEFI SMC could be
  unneeded or insufficient. Plan changed from "UEFI first" to "find the read first".
- S10eCam 0.4: deep = one step per IOCTL (Flags bit0 + step in bits 15:8, S10ECAM_STEP_MAX 8): 1 BUS_USER/BUSP
  (pmucal's save list reads them), 2 the 7 gates, 3 QCH CSIS2/CSISDMA/PDPDMA, 4 QCH LHM/SYSREG/SYSMMU, 5 CSIS2,
  6 CSIS DMA, 7 SYSREG +0x500, 8 SysMMU. Steps 5-8 re-check their gate + LHM_AXI_P. Step 0 (v1.1's 3deep) refused.
- cam-s10.ps1 v1.2: '3step [N]' runs N..8, writes "step N (...): calling the driver ..." write-through + FlushFileBuffers
  before each call (log appends across runs), stops at a step without "step N done"; refuses in a window that still
  has the v1.0 type. '3deep' removed. Checks: clang WDK clean; pwsh parse 0; the loop run on Linux with a fake
  S10Cam type (steps 2..5, stop at an incomplete 5) - output as intended. NOT built with EWDK, NOT run.
- Review of the 0.4 commit: rg 'lhm' -> no leftover; every step's gate re-read uses a step-2 register; old v1.1 3deep (Flags 1) -> step 0 -> refused before any ISPPRE map; the '3' VERDICT names 3step; ps1 report filter keeps the first step's PMU lines only. Nothing found.

### 2026-11-16 04:09 3step -> blue screen -> phone came up in TWRP (user)
- TWRP regardless of stay.flg: S10eDiag's bugcheck callback (notes/m11-s10ediag.md run 5) records BC + P1..P4 in the
  PRAM mark ring (WINPRAM +0xA000) and resets with INFORM3 = recovery -> sboot boots TWRP directly. The ring stays in
  RAM; a firmware bounce (reboot with \WINRUN.FLG still on the ESP) dumps it to \WINPRAM.BIN.
- Asked for: /data/S10Prov/cam-s10-3step.txt (write-through log: last "step N ... calling" line = the step), ESP
  listing, then `adb reboot` (bounce) and \WINPRAM.BIN (bugcheck code).

### 2026-11-16 04:13 evidence from TWRP
- ESP (/cache = sda39): S10EDIAG/stay.flg 34 B (= stay-s10.ps1 v1.0's one line: the user ran it), WINRUN.FLG,
  WINPRAM.BIN, UEFILOG.TXT (FAT dates all 2026-11-01 17:16 = the firmware's fixed timestamp, useless).
- /data/S10Prov/cam-s10-3step.txt NOT visible in TWRP: /data = sda45 NTFS via fuseblk (ntfs-3g) read-only, which
  does not replay $LogFile; after a blue screen the new file's MFT update can sit only in the log. Read it from
  Windows (it replays the log) instead.
- WINPRAM.BIN (notes/data/winpram-cam3deep-20261116.bin): S10eDiag mark ring, last session uptime 5459 s, then
  "BUGCHECK callback", BC 0x124 (WHEA_UNCORRECTABLE_ERROR), P1 0x11 (WHEA source = SEA, synchronous external abort),
  P2 = WHEA record ptr, "reset to recovery". A 91-min session cannot end at 04:09 (the 03:44 3deep crash is inside
  it), and the firmware zeroes the ring after each dump -> this dump is the 3deep crash (03:44). The 3step crash's
  ring should still be in RAM: asked for a bounce (adb reboot; stay.flg -> dump + continue into Windows).
- Meaning: a synchronous external abort = the slave (or the interconnect / TZPC filter) answered the load with an
  ERROR response - not a hang of a clock-stopped block. Fits a D_TZPC deny (DECERR) best; a Q-channel-gated slave
  may also answer with an error on this bus. The 3step log will say which read.
- 04:17 WINPRAM-3step.BIN (notes/data/winpram-cam3step-20261116.bin): fresh ring (S10eDiag loaded once, uptime
  1308 s = boot ~03:47, right after the 3deep crash) -> BUGCHECK BC 0x124, P1 0x11 (SEA), "reset to recovery".
  Same signature as 3deep. The step is in C:\S10Prov\cam-s10-3step.txt (asked again).

## 2026-11-16 04:20 STAGE 3 (3step) RESULT - the faulting read is the SysMMU
cam-s10-3step.txt (write-through log survived; Windows clock said 2026-11-01T23:38 - the RTC/time is wrong, separate):
- step 1 CMU_ISPPRE BUS_USER 0x10 (CMU_TOP clock), BUSP div 0x1 (/2)
- step 2 gates 0x2004/20B4/20D8/202C/2038/2040/2060 all 0x00200000 (MANUAL 0 = HWACG, on)
- step 3/4 QCH CSIS2 / CSIS DMA / PDP DMA / LHM_AXI_P / SYSREG / SYSMMU all 0x003F0002 (ENABLE 0, CLOCK_REQ 1)
- step 5 CSIS2: VERSION 0x05020000 (= v5.2, matches CONFIG_CSIS_V5_2), CMN_CTRL 0x4000, CLK_CTRL 0x10, INT masks/srcs
  0, ISP_CONFIG_CH0 0xFC, ISP_RESOL_CH0 0x80008000, FRM_CNT_CH0 0, PHY_STATUS 0xF1, PHY_CMN_CTRL 0 (= reset state)
- step 6 CSIS DMA0: CTRL 0 FMT 0 ADDR1 0 ACT_CTRL 0x1007C RESOL 0 FCNTSEQ 0xFFFFFFFF STAT 0; common all 0
- step 7 SYSREG_ISPPRE +0x500 = 0 -> every D-PHY of ISPPRE in reset
- step 8 SysMMU 0x17210000 MMU_CTRL read -> "calling the driver ..." was the last line -> bugcheck 0x124 P1 0x11 (SEA)
  (WINPRAM-3step ring). The 3deep crash had the same signature and also read the SysMMU last.
Conclusions:
- NO D_TZPC problem for CMU_ISPPRE / CSIS2 / CSIS DMA / SYSREG: Windows reads them with the domain as sboot left it
  - no UEFI SMC needed for 3b (matches the S9: found-on ISPPRE used as is, no SMC).
- The SysMMU's NS frame aborts even with its aclk gate and QCH on (secure-only until Linux's restore? not proven).
  Never read it; 3b runs the DMA with the SysMMU untouched (reset = bypass, the S9's assumption, its DMA worked).
- ISPPRE bus clock is CMU_TOP mux 3 / div 16 - slow, as the S9 found ("about 50 MHz, too slow: CSIS overflows") ->
  3b must raise CLKCMU_ISPPRE_BUS (source of mux 3 and the target rate: to be read from cmucal-vclklut / DT).
- 0.4.1: step 8 removed (S10ECAM_STEP_MAX 7, cam-s10 v1.2.1 runs 1..7), SysMMU defines dropped.

## Stage 3b (= driver stage 4, "shot") facts - 2026-11-16 ~05:00, all from K (LineageOS exynos9820) + live DT
Sensor (fimc-is-cis-3j1.c, -setA.h; module fimc-is-device-module-3j1.c):
- setfile "default" = setA. Table = u32 triples {addr, data, size}; size 2 = 16-bit write, 1 = 8-bit; markers
  I2C_MODE_BASE 0xF0000000: BURST_ADDR +1 (addr word = 0x6F12), BURST_DATA +2 (one 16-bit word to that address, sent
  in one I2C message = the same as single writes to 0x6F12, whose pointer auto-increments), DELAY +3 (data = us).
  sensor_cis_set_registers sleeps 3 ms before every table.
- Order (fimc-is-cis-3j1.c): global sensor_3j1_setfile_A_Global (then DT post_init_setting_list: none in our DT) ->
  mode table sensor_3j1_setfiles_A[mode] -> stream_on: group hold 0, 8 ms, 0x0A70 = 0x0001, 0x0A72 = 0x0100 (16-bit),
  0x0100 = 0x01 (8-bit), 0x021E = 0x00 (8-bit, no WDR). stream_off: 0x0100 = 0x00 (8-bit).
- Mode 8 = 1824 x 1368 @30, 4 lanes, 1807 Mbps/lane, MCLK 26 MHz (setA header + module cfg #8: VC0 RAW10
  1824x1368, VC1 RAW10 1824x342 PDAF, CSI_MODE_VC_ONLY, PD_MOD3). Bayer GR_BG (cis->bayer_order). Test pattern:
  0x0601 (8-bit, CIS_TEST_PATTERN_MODE; SMIA: 2 = colour bars). Exposure regs 0x0202 coarse, 0x0204 again, 0x0340 FLL.
CSIS2 v5.2 (fimc-is-hw-csi-v5_2.c/.h, fimc-is-device-csi_v4.c csi_stream_on):
- fields: CMN_CTRL 0x4: CSI_EN b0, SW_RESET b1, LANE_NUMBER [9:8], INTERLEAVE_MODE [11:10], UPDATE_SHADOW [19:16],
  QCHANNEL_EN b20, PHY_SEL b21 (0 = D-PHY). PHY_CMN_CTRL 0x704: ENABLE_CLK b0, ENABLE_DAT [4:1].
  ISP_CONFIG_CHn 0x40+0xC*n: VC [1:0], DATAFORMAT [7:2] (RAW10 0x2B), DECOMP_EN b8, DECOMP_PREDICT b9, POTF_ONLY b11,
  PIXEL_MODE [13:12] (VC0 dual=1 unless PD_MOD1/MSPD). ISP_RESOL_CHn 0x44: VRESOL [31:16], HRESOL [15:0].
  DEBUG_OPTION_SUITE 0x500 (= PIXEL_ALIGN_MODE 1). LINE_INTR_CH0 0x110. INT_MSK0/1 = 0x011FFF1F / 0x0000FFF0
  (non-EARLYBUF variant: mask 1 = enabled); FRAMESTART [27:24], FRAMEEND [23:20].
- sequence: reset (SW_RESET 1, wait clear, 10 x 10 us) at s_format; stream on: [PHY] -> s_lane (LANE_NUMBER 3,
  ENABLE_DAT 0xF) -> INTERLEAVE_MODE = CSI_MODE_VC_ONLY -> DEBUG_OPTION_SUITE 1 -> s_config per VC (VC_ONLY: all 4 VCs
  from the cfg) -> DMA config -> csi_hw_enable: UPDATE_SHADOW 0xF, PHY_SEL 0, ENABLE_CLK 1, CSI_EN 1.
  stream off: ENABLE_CLK 0, ENABLE_DAT 0, CSI_EN 0, reset, DMA0 CTRL ENABLE 0 for every VC.
- VC DMA (vc_reg 0x170C0000 + cmn 0x170C0400): DMA0_CTRL 0x0: ENABLE b0, UPDT_PTR_EN b1, UPDT_FRAMEPTR [6:2];
  DMA0_FMT 0x4: DATAFORMAT [1:0] (0 RAW10), PACK12 b3, STORAGE b4 (0 legacy), DIM b5 (0 2D); DMA0_ADDR1..32
  0x10..0x8C; ACT_CTRL 0x90 (ACTIVE ENABLE b0, FRAMEPTR [6:2]); DMA0_RESOL 0xA4 [13:0] = width; FCNTSEQ 0xB0 (bit n =
  ADDR n+1 in use), FCNTSEQ_STAT 0xB4. cmn: DMA_DATA_CTRL 0x0 (INPUT_PATH_CH0 b0: 0 OTF), INT_MASK 0x4, INT_SRC 0x8
  (DMA_ERROR b12 = FIFO full), CMN_CTRL 0x14 (DMA_ABORT_REQ b0), OTF_FORMAT 0x1C [1:0] (0 RAW10).
  Order: FMT (legacy RAW10, pack12 per pixelformat: our NORMAL = a pixel in 16 bits), RESOL = width, FCNTSEQ = 0;
  cmn OTF_FORMAT 0, INPUT_PATH 0; addr: clear UPDT_FRAMEPTR bit n, ADDR(n+1) = phys, FCNTSEQ |= 1 << n; output on:
  CTRL ENABLE 1 + UPDT_PTR_EN 1. The S9 precedent's WDMA wrote RAW10 as 16 bits per pixel (stride = 2 x width).
- Common DMA = fimc_is@17190000 reg[0] = 0x170E0000 (+0x100) (IORESOURCE_CSIS_DMA 0): csi_hw_s_dma_common_dynamic:
  ARB_PRI 0x0 [3:2]=1 [1:0]=2; CTRL 0x8 IP_PROCESSING b1 = 1; SRAM_SPLIT 0xC [27:16] = [11:0] = 0x480; MATRIX 0x10
  [4:0] = 0. (STAT DMA 0x17140000 only with ENABLE_PDP_STAT_DMA - skipped.)
D-PHY (phy-exynos-mipi.c, csi_hw_s_phy_set): power_on = PMU +0x730 bit0 <- 1 (isolation bypass; 0x730 is shared by
  the m0s4s4s4 PHYs csis1/2/3, not the display's) then sysreg 0x17020500 bit1 <- 0 then 1 (reset release).
  cfg = {0x05020002 (9820 non-EVT0, beyond1 defconfig "# CONFIG_SOC_EXYNOS9820_EVT0 is not set", 4 lanes),
  0xD<<16 | 0xDC (instance CSI_ID_C = 2), lanes 3, 1807, settle}. settle = fimc_is_hw_find_settle(1807):
  ALIGN 1810 -> 9 (fimc-is-hw-settle-8nm-lpp.h, v7_0_0). __set_phy_cfg_0502_0002_dphy at 0x19162300: SC 0x0=1, 0x4=0x1450,
  0x8=4, 0xC=0x9000, 0x10=5 then [9:8]=clk_div1234_mc=max(0, 5-ceil(((3-1)*1807>>2)/26)) = 0, 0x14=0x600, 0x30=0x301;
  per lane i=0..3 at +0x100*i: 0x100=1, 0x104=0x1450, 0x108=4, 0x10C=0x9000, 0x110=5 then [9:8]=skew_delay_sel 3
  (1500<=1807<2000), 0x114=0x600, (type 0xDC, i<3) 0x124=0x40, 0x130 [7:0]=9 [8]=settle_clk_sel 0, 0x134=3,
  0x140 b0 = skew_cal_en 1, 0x150 = 0x81A.
ISPPRE bus clock: CMU_TOP MUX_CLKCMU_ISPPRE_BUS parents {PLL_SHARED4 666 MHz, SHARED0_DIV4, SHARED1_DIV4
  (1865.5/4 = 466 MHz), SHARED2_DIV2 (800/2 = 400 MHz)}; found mux 3 / div 16 = 25 MHz (pmucal rate tables). Android
  sets it through ACPM DVFS dvfs_cam (dfs_id 0xB04000C, levels 690000/650000) - clock AND voltage. Plan: divider only,
  /4 = 100 MHz (well under any level), restored at the end. INFERENCE: 100 MHz x 2 pixels/clock > the ~77 Mpixel/s a
  1824-wide line at 30 fps needs on average.
- correction to the facts above: ISP_CONFIG_CHn / ISP_RESOL_CHn step is 0x10 per VC (0x40/0x44, 0x50/0x54, ...),
  not 0xC (fimc-is-hw-csi-v5_2.h:110-121). ENABLE_HWACG_CONTROL is defined for v7_0_0 (fimc-is-config.h:195):
  csi_hw_enable clears CMN_CTRL QCHANNEL_EN b20, csi_hw_disable sets it. CMU_TOP ISPPRE_BUS DIV BUSY = b16.

## S10eCam 0.5 - stage 4 "shot" written (2026-11-16 ~05:30)
- IOCTL_S10ECAM_SHOT 0x802 METHOD_OUT_DIRECT, in S10ECAM_SHOT_IN {Flags (b0 colour bars, [11:8] phase limit),
  Coarse, AGain, IspDiv (0 -> 3 = /4 = 100 MHz, 0x100 leave), Frames (0 -> 4)}; out = 16 KiB report + 1824x1368x2.
- Order: stage 1 -> preconditions (pd-dpu/isppre ON, CMU_TOP gate, ISPPRE gates LHM/CSIS2/CSISDMA/PDPDMA/SYSREG/
  LHS_AXI_D 0x20C8 on, CSIS2 not enabled) -> 16 MiB contiguous < 4 GB uncached, filled 0xEE -> ISPPRE div ->
  SensorPowerUp + SensorCheckId -> global (227) + mode 8 (326) tables (0x6F12 runs batched 7 words / message) ->
  optional 0x0202/0x0204/0x0601=2 -> read back + GUARD: 0x034C/0x034E must be 1824/1368 (DMA0_RESOL bounds only the
  width) [phase 1] -> PMU iso b0=1, sysreg b1 0->1, D-PHY 0502_0002 writes [phase 2] -> CSIS2 reset, 4 lanes,
  VC_ONLY, PIXEL_ALIGN 1, VC0 RAW10 dual 1824x1368, VC1 RAW10 quad 1824x342, VC2/3 0x3F, masks DBG [phase 3] ->
  common DMA (ARB 1/2, IP_PROCESSING, SRAM 0x480/0x480, MATRIX 0), VC0 FMT 0, RESOL 1824, OTF_FORMAT 0, input OTF,
  ADDR1 = PA, FCNTSEQ 1, DMA mask, CTRL EN|UPDT_PTR_EN [phase 4] -> csi_hw_enable (+QCH_EN off) -> sensor
  stream on (8 ms, 0x0A70 1, 0x0A72 0x100, 0x0100=1, 0x021E=0) -> poll FRM_CNT_CH0 / INT_SRC0/1 / DMA INT_SRC 1 ms
  steps up to 2 s for N frames -> teardown (DMA off + 50 ms, stream off + 40 ms, CSIS off/QCH as found/reset, masks,
  DMA regs, common DMA regs back, PHY reset + isolation as found, sensor PowerDown, ISPPRE div back) -> analysis of
  the buffer (rows written, rows past 1367, mean of sampled 10-bit values) -> frame copied out.
- cam-s10.ps1 v1.3: 'shot' / 'shot bars' (+ -Coarse -AGain -IspDiv -Frames -Phase), type S10Cam13 (new name: old
  windows keep the old S10Cam), saves cam-shot.raw + cam-shot.bmp (2x2 GRBG binning 912x684, white point 99.5th
  percentile, gamma 1/2.2). Preview verified on Linux with a synthetic GRBG 8-bar frame: the 8 colours come out right.
- Checks: clang vs WDK 26100 km clean (-Wall -Wextra -Wconversion); pwsh parse 0; MSVC C4389 spots cast. NOT built
  with EWDK, NOT run.
- Review of the 0.5 commits: (1) the common DMA block 0x170E0000 was first READ in the "found values" before phase 1 -
  a block stage 3 never read (could abort like the SysMMU) ahead of every phase -> moved to the DMA phase (after
  phase 3), so -Phase 3 vs 4 isolates it. (2) MSVC C4389 USHORT/UCHAR vs unsigned compares cast. (3) HWACG
  QCHANNEL_EN handling added (Linux clears it at enable). (4) DMA0_RESOL bounds only the width -> 0x034C/E guard.
  First-ever accesses in a full shot, in order: CMU_ISPPRE 0x20C8 (CMU, as the gates read fine), PMU 0x730 write,
  sysreg 0x500 write, D-PHY 0x19162300.. (DPU block, phase 2), CSIS2 writes (phase 3), common DMA (phase 4).

## 2026-11-16 04:55 FIRST SHOT (S10eCam 0.5, `shot bars`) - frames arrive, CSIS overflows at 100 MHz
Log: notes/data/cam-s10-shot-1-20261116.txt (raw frame kept off-repo: 4.99 MB, only the first 70912 bytes written).
- Everything up to the stream WORKED: rails, ID, global (227 entries / 54 msgs) + mode 8 (326 / 257) written, read
  back coarse 0x0100, again 0x0020 (x1), FLL 0x0597 (1431 lines), LLP 0x4940, OUTPUT 1824 x 1368, pattern 0x0002.
  D-PHY: iso 1, reset released (sysreg 0x2), SC/SD regs read back as written (SD0 TIME_CON0 9). No abort anywhere:
  PHY block 0x19162300 and common DMA 0x170E0000 are NS-accessible. Common DMA found 0/4/0/0.
- CSIS2: CMN_CTRL 0x4B01 (CSI_EN, 4 lanes, VC_ONLY, shadow F... ), PHY_CMN_CTRL 0x1F, CONFIG0 0x10AC (RAW10, dual),
  RESOL0 0x05580720, CONFIG1 0x20AD (VC1, RAW10, quad). PHY_STATUS 0xF1 -> 0xF0 while streaming.
- Stream: "4 frames counted on CH0 in 54 ms" (FRM_CNT_CH0 likely counts VC0 + VC1 frame starts: 2 sensor frames at
  33 ms), INT_SRC0 0x03300010 = FS VC0+VC1, FE VC0+VC1, and bit 4 = OVER (CSIS overflow; S9 bit map "OVER 4"). DMA cmn
  INT_SRC 0x01011011 incl. b12 DMA_ERROR (FIFO full). ACT_CTRL 1, FCNTSEQ_STAT 0x101.
- Buffer: 70912 bytes written then nothing. Byte autocorrelation: period 2496 bytes (0.99 match) = consecutive
  "lines" packed back to back; as 16-bit words (1248 per period) they form clean vertical bars (white run of 252
  words, black runs) = the colour-bar pattern, 16 bits per pixel, the 10-bit value low (ff 03 = 0x3FF), but each line
  lost ~1/3 of its pixels and values like 0x3FC/0xCC/0x330 appear = data dropped/corrupted by the overflow.
- Same as the S9 precedent: "The boot leaves it at about 50 MHz, too slow: CSIS1 overflows and the WDMA's FIFO fills a
  few lines into the frame". Need: CSIS2 at 2 pixels/clock >= 4 x 1807 Mbps / 10 bit / 2 = 361 MHz. Next: ISPPRE
  bus 400 MHz (mux 3 SHARED2_DIV2, div /1) - under Android's 466 (SHARED1_DIV4). Voltage NOT raised (no ACPM
  dvfs_cam request): INFERENCE that the boot voltage carries 400 MHz; a failure would be a hang/abort, not damage.
- The 0.5 IspDiv field could not express divider field 0 (0 meant "default") -> 0.5.1: IspDiv = the RATIO 1..16.

## 2026-11-16 05:02 FULL FRAME - colour bars through D-PHY -> CSIS2 -> DMA -> RAM under Windows (S10eCam 0.5.1)
Log: notes/data/cam-s10-shot-2-bars-20261116.txt. ISPPRE bus 400 MHz (mux 3, div 0).
- INT_SRC0 0x03300000: FS + FE on VC0/VC1, NO overflow; DMA cmn INT_SRC 0x111, no FIFO-full; PHY_STATUS F1 -> F0.
- DMA extent exactly 4990464 bytes = 1824 x 1368 x 2; 1368 rows, none past; every value in rows 0/1/700/1367 is
  0x000 or 0x3FF (the bars are pure) -> layout CONFIRMED: 16 bits per pixel, 10-bit value low, stride 3648, Bayer
  GRBG (the BMP preview shows the 8 SMPTE-order bars white/yellow/cyan/green/magenta/red/blue/black in order).
- 400 MHz without an ACPM voltage request ran fine for this shot (one run: not a stability proof).
- Teardown left everything as found (ISPPRE div 0xF, iso 0, sysreg 0, CMN_CTRL 0x4000, DMA0 CTRL 0, rails off).
- Stage 3b = DONE for the test pattern. Next: the scene (pattern off), exposure by hand (coarse max = FLL 1431 - 4 =
  1427 lines at 30 fps; again 0x20 = x1), then an AE loop, then a Windows camera driver (AVStream / frame server).

## 2026-11-16 05:07 FIRST SCENE (pattern off, -Coarse 1400 -AGain 128) - the sensor images, the room was dark
Log: notes/data/cam-s10-shot-3-scene-20261116.txt. Read back coarse 0x0578, again 0x0080 (x4), pattern 0.
- Full frame again (4990464 bytes, no overflow, 4 counts in 68 ms). Values: min 58, max 131, mean 64.4, std 1.3;
  per channel G1 64.48 R 64.36 B 64.37 G2 64.49 -> black level (pedestal) = 64, the scene ~1 LSB above it.
- The 0x3F-0x41 row-0 values + v1.3.1's preview (white point = p99.5 = 68 -> noise stretched to white) gave a flat
  white BMP. Averaged in 8x8 blocks and stretched (notes/data/cam-first-scene-20261116-stretched.png) the frame
  shows a real scene: a lit round shape (a head/face) in the middle-left, edges above - FIRST REAL IMAGE. At 05:07
  the room was almost dark; exposure 1400 lines = ~32.6 ms (line = 18752 / 806 MHz = 23.3 us), gain x4.
- cam-s10.ps1 v1.3.2: preview subtracts black 64 and keeps the white point >= 96 (a dark frame now looks dark).
  Next: a lit scene; read 0x0084/0x0086 (analog gain code min/max, the kernel's get_min/max_analog_gain) for the gain
  range; longer exposure = FLL 0x0340 up (lower fps); then AE + white balance.

## 2026-11-16 05:10 SECOND SCENE - a recognisable picture (laptop keyboard + screen), same settings
Log: notes/data/cam-s10-shot-4-scene-20261116.txt (cam-s10.ps1 v1.3.1 still on the phone: old preview).
- Mean 73 / 1023 (9 above black), white point 207. Black-subtracted channel means R 6.0, G 9.6, B 5.4 (gray world:
  R x1.60, B x1.78). Rendered offline with black 64 + gray-world WB + p99.7 white + gamma 1/2.2:
  notes/data/cam-scene-2-20261116-wb.png - backlit Dell keyboard (key legends readable), a laptop screen showing a
  web page; sharp enough, colours plausible. Orientation as the sensor reads out (rotated vs the phone).
- The raw path (sensor -> D-PHY -> CSIS2 -> DMA) is proven on real content. Remaining for a usable camera: AE (coarse/
  gain from the frame mean, a few shots), white balance (gray world is fine to start), demosaic beyond 2x2 binning,
  orientation, then the Windows-facing driver (AVStream/frame server; MS docs first) - a big separate milestone.

## 2026-11-16 05:15 user: "all" (AE + WB + demosaic + a Windows camera). Windows camera path - MS docs first (rule 13)
- MFCreateVirtualCamera (learn.microsoft.com/windows/win32/api/mfvirtualcamera/nf-mfvirtualcamera-mfcreatevirtualcamera):
  "In the current release, only MFVirtualCameraType_SoftwareCameraSource is supported"; sourceId = "The unique
  CLSID of the custom media source to be activated for this virtual camera"; MFVirtualCameraLifetime_System to
  "persist across sessions and/or across reboots"; AllUsers access "must have administrator permissions"; minimum
  client Windows Build 22000; lib mfsensorgroup.lib. -> a user-mode COM media source + a registering step.
- AVStream samples (learn.microsoft.com/windows-hardware/drivers/stream/avstream-samples): Avshws "provides a
  pin-centric AVStream capture driver for a simulated piece of hardware ... video captures at 320 x 240 pixels in
  either RGB24 or YUV422 format using direct memory access (DMA) into capture buffers"; Avssamp = filter-centric.
- Choice: an AVStream capture minidriver (kernel, a real KSCATEGORY_VIDEO_CAMERA device - what the Camera app /
  Frame Server enumerate; no user-mode registration; fits "native only" rule 12 best). It reuses S10eCam's capture
  engine (power, setfile, D-PHY, CSIS2, DMA) and converts the Bayer frame in kernel (2x2 binning -> 912x684, cheap)
  into YUY2/NV12. The virtual camera stays the fallback if the AVStream path stalls.
- Order: (1) S10eCam 0.6 streaming engine (start / latest frame / exposure / stop, 3-buffer DMA ring) + a user-mode
  'stream' test with AE + gray-world WB + full demosaic + rotation (cam-s10.ps1); (2) the engine moved into a shared
  file; (3) the AVStream driver (S10eCamera) on it; (4) INF + root-enumerated device + kit.

## S10eCam 0.6 - streaming engine (2026-11-16, written, NOT built/run)
- IOCTLs 0x803 STREAM_START (in S10ECAM_SHOT_IN, out report), 0x804 STREAM_FRAME (OUT_DIRECT: 64-byte
  S10ECAM_FRAME_HDR + frame), 0x805 STREAM_EXPOSURE ({Coarse, AGain}, written inside a 0x0104 group hold, clamped to
  2..FLL-4 and the sensor's 0x0084..0x0086 gain codes), 0x806 STREAM_STOP. IRP_MJ_CLEANUP of the starting handle and
  DriverUnload stop it; stages 1-4 return STATUS_DEVICE_BUSY while it runs.
- Ring: 3 contiguous uncached buffers < 4 GB (frame + 64 KiB); DMA ADDR1..32 = Ring[n % 3], FCNTSEQ all ones.
  Finished-frame detection does not rely on the undocumented FCNTSEQ / frame-pointer semantics: each buffer's last 8
  bytes hold a canary 0xEEEEEEEE (no 10-bit pixel is 0xEEEE); a 2 ms poll thread (ExSetTimerResolution 1 ms) sees it
  overwritten = that frame finished, saves those 8 bytes (Tail) and re-plants the canary; scans from the expected
  next index (re-syncs if the DMA did not start at Ring[0]). FCNTSEQ re-armed when found not all ones (counted), a
  300 ms gap = a "stall" (counted, re-armed). FRAME copies the newest finished buffer (+ Tail), reports CopyUs and
  Torn (2+ frames finished during the copy = the buffer may have started over).
- cam-s10.ps1 v1.4 'stream' [bars] (-Seconds, -Rotate, -Mirror): one handle for the run (closing it stops the
  camera), frames every ~120 ms with header + stats (black-subtracted G/R/B means on a 16-px grid, clipped %), AE on
  E = lines x gain/32 toward G 110 (ratio clamped 0.25..4, clip > 5 % halves, settled within 12 %; lines first up to
  CoarseMax, then gain within 0x0084..0x0086; next decision only 3 frames after a write), last frame developed:
  bilinear GRBG demosaic at full 1824 x 1368, gray-world WB (R/B gains = G/R, G/B of the stats, clamped), p99.5
  white, gamma 1/2.2, rotation; saves cam-stream.bmp + .raw. Mock-tested on Linux (fake S10Cam14 driving the second
  scene's raw scaled by exposure): AE walked 256/x1 -> 1024 -> 1427/x2.9 -> x11.5 -> x16 (dark scene, gain-limited),
  the BMP came out; Develop of the real raw: 1824x1368 in ~0.5 s, rot 90 correct. Mirror: NOT verified (the text in
  the scene is too blurred to read) -> opt-in -Mirror.
