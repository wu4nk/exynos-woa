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
 * STAGE 4 (0.5, "3b"): IOCTL_S10ECAM_SHOT - the sensor streams mode 8 (1824 x 1368 RAW10 @30) through the D-PHY,
 * CSIS2 and the VC0 DMA into a 16 MiB buffer below 4 GB (SysMMU untouched = bypass); see Stage4 and notes/s10-camera.md
 * "Stage 3b facts". The setfile tables are S10eCam3j1.h (generated from Samsung's GPL setfile).
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
#include "S10eCam3j1.h"

/* ---------------------------------------------------------------------------------------------- addresses */
#define CMU_TOP_MAP         0x1A241000ULL   /* CMU_TOP 0x1A240000 + 0x1000, three pages */
#define CMU_TOP_MAP_SIZE    0x3000u
#define   TOP_CIS1_MUX        (0x1014u - 0x1000u)
#define   TOP_CIS1_DIV        (0x1810u - 0x1000u)
#define   TOP_PERIC1_BUS_GATE (0x20C4u - 0x1000u)
#define   TOP_PERIC1_IP_MUX   (0x10C4u - 0x1000u)   /* MUX_CLKCMU_PERIC1_IP [0]: PLL_SHARED0_DIV4 / PLL_SHARED2_DIV2 */
#define   TOP_PERIC1_IP_DIV   (0x18B0u - 0x1000u)   /* DIV_CLKCMU_PERIC1_IP [3:0] */
#define   TOP_PERIC1_IP_GATE  (0x20C8u - 0x1000u)   /* GATE_CLKCMU_PERIC1_IP: both I2C ipclks come from it (user mux 1) */
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

/* stage 3: the CSI receive path of the front camera (notes/s10-camera.md "Stage 3 facts"). DT: sensor id 1 =
   fimc_is_sensor2@17050000 (CSIS2, dma_ch 2 -> DMA0 0x170C0000 + common 0x170C0400), sysmmu@17210000,
   dphy_m0s4s4s4_csis2 (0x19162300, PMU isolation 0x730, reset = ISPPRE sysreg 0x17020500 bit 1). Power: pd-isppre
   (PMU +0x2500/+0x2504, parent pd-dpu +0x1E00/+0x1E04; K flexpmu_cal_local_exynos9820.h). Clocks: K cmucal-sfr.c. */
#define PMU_BASE            0x15860000ULL   /* system-controller@15860000, "samsung,exynos9820-pmu" */
#define PMU_SIZE            0x3000u
#define   PMU_MIPI_ISO_CSIS2  0x0730u       /* bit 0: 1 = isolation bypassed (PHY usable) */
#define   PMU_DPU_CONF        0x1E00u
#define   PMU_DPU_STAT        0x1E04u
#define   PMU_ISPPRE_CONF     0x2500u
#define   PMU_ISPPRE_STAT     0x2504u
#define   PD_ON               0x1u
#define   TOP_ISPPRE_BUS_MUX  (0x1094u - 0x1000u)
#define   TOP_ISPPRE_BUS_DIV  (0x1884u - 0x1000u)
#define   TOP_ISPPRE_BUS_GATE (0x20A0u - 0x1000u)
#define CMU_ISPPRE          0x17000000ULL
#define CMU_ISPPRE_SIZE     0x4000u
#define   IP_BUS_USER         0x0100u
#define   IP_BUSP_DIV         0x1804u
#define   IP_CMU_PCLK         0x2004u
#define   IP_CSIS2_ACLK       0x202Cu
#define   IP_CSISDMA_ACLK     0x2038u       /* ACLK_CSISX4_PDP_DMA */
#define   IP_PDPDMA_ACLK      0x2040u       /* ACLK_PDP_TOP_DMA */
#define   IP_SYSMMU_ACLK      0x2060u
#define   IP_LHM_AXI_P        0x20B4u       /* the register path into ISPPRE */
#define   IP_SYSREG_PCLK      0x20D8u
#define   IP_3AA0_ACLK        0x201Cu       /* 0.11: CLK_CON_GAT_GOUT_BLK_ISPPRE_UID_IS_ISPPRE_IPCLKPORT_ACLK_3AA0
                                               (ExtremeKernel cal-if/exynos9820/cmucal-sfr.c:1348) */
#define   IP_Q_3AA0           0x3034u       /* QCH_CON_IS_ISPPRE_QCH_3AA0 (cmucal-sfr.c:2380) */
/* 0.11: 3AA0 of the ISP (live DT fimc_is@17190000 reg #1; the Samsung ISP library's 3AA chain). Its ID registers
   as read on Android with the camera on (notes/s10-isp.md, ispregs v1.4 2026-11-17): +0xF0 0x07010000, +0xF4 0xcc22 */
#define TAA0_BASE           0x17190000ULL
#define   TAA_ID_VERSION      0x0F0u
#define   TAA_ID_CODE         0x0F4u
#define   IP_Q_CSIS2          0x3054u
#define   IP_Q_CSISDMA        0x305Cu
#define   IP_Q_PDPDMA         0x3064u
#define   IP_Q_SYSMMU         0x3088u
#define   IP_Q_LHM_AXI_P      0x30A4u
#define   IP_Q_SYSREG         0x30BCu
#define SYSREG_ISPPRE_PAGE  0x17020000ULL
#define   ISPPRE_PHY_RST      0x500u        /* bit 1: CSIS2 D-PHY, 1 = reset released */
#define CSIS2_BASE          0x17050000ULL
#define   CSIS_VERSION        0x000u
#define   CSIS_CMN_CTRL       0x004u
#define   CSIS_CLK_CTRL       0x008u
#define   CSIS_INT_MSK0       0x010u
#define   CSIS_INT_SRC0       0x014u
#define   CSIS_INT_MSK1       0x018u
#define   CSIS_INT_SRC1       0x01Cu
#define   CSIS_ISP_CONFIG_CH0 0x040u
#define   CSIS_ISP_RESOL_CH0  0x044u
#define   CSIS_FRM_CNT_CH0    0x100u
#define   CSIS_PHY_STATUS     0x700u
#define   CSIS_PHY_CMN_CTRL   0x704u
#define CSIS_DMA_PAGE       0x170C0000ULL   /* DMA0 (VC0) at +0x000, DMA common at +0x400 */
#define   DMA0_CTRL           0x000u
#define   DMA0_FMT            0x004u
#define   DMA0_ADDR1          0x010u
#define   DMA0_ACT_CTRL       0x090u
#define   DMA0_RESOL          0x0A4u
#define   DMA0_FCNTSEQ        0x0B0u
#define   DMA0_FCNTSEQ_STAT   0x0B4u
#define   DMAC_BASE           0x400u
#define   DMAC_DATA_CTRL      0x000u
#define   DMAC_INT_MASK       0x004u
#define   DMAC_INT_SRC        0x008u
#define   DMAC_CMN_CTRL       0x014u
#define   DMAC_ERR_CODE       0x018u
/* SysMMU ISPPRE 0x17210000: NEVER read - its non-secure frame answers a load with an external abort (bugcheck 0x124
   P1 0x11, 2026-11-16 03:44 and 04:09). The CSIS DMA is to run with it untouched (reset state = bypass, as the S9's). */

/* stage 4 (notes/s10-camera.md "Stage 3b facts"): CSIS2 + D-PHY + VC0 DMA write path */
#define   CSIS_CMN_CSI_EN       (1u << 0)
#define   CSIS_CMN_SW_RESET     (1u << 1)
#define   CSIS_CMN_LANES_SHIFT  8           /* [9:8] lanes - 1 */
#define   CSIS_CMN_ILV_SHIFT    10          /* [11:10] interleave mode */
#define   CSIS_CMN_SHADOW_SHIFT 16          /* [19:16] UPDATE_SHADOW */
#define   CSIS_CMN_QCH_EN       (1u << 20)
#define   CSIS_CMN_PHY_SEL      (1u << 21)  /* 0 = D-PHY */
#define   CSIS_PHY_EN_CLK       (1u << 0)
#define   CSIS_PHY_EN_DAT_SHIFT 1           /* [4:1] */
#define   CSIS_DEBUG_OPTION     0x500u      /* DEBUG_OPTION_SUITE = PIXEL_ALIGN_MODE */
#define   CSIS_CH_STRIDE        0x10u       /* ISP_CONFIG_CHn / ISP_RESOL_CHn = 0x40 / 0x44 + 0x10 n */
#define   CSIS_MODE_VC_ONLY     2u
#define   CSIS_PIXEL_DUAL       1u
#define   CSIS_PIXEL_QUAD       2u
#define   HW_FORMAT_RAW10       0x2Bu
#define   HW_FORMAT_UNKNOWN     0x3Fu
#define   CSIS_IRQ_MASK0_DBG    0x0FFFFF1Fu /* DBG_CSIISR set: every frame start too (only the SRC view matters here) */
#define   CSIS_IRQ_MASK1_DBG    0x0000FFFFu
#define   CSIS_FRAMESTART_SHIFT 24
#define   CSIS_FRAMEEND_SHIFT   20
#define   CSIS_INT_OVER         (1u << 4)   /* INT_SRC0 b4: CSIS overflow (seen at 100 MHz, 2026-11-16) */
#define   DMA0_EN               (1u << 0)
#define   DMA0_UPDT_PTR_EN      (1u << 1)
#define   DMA0_UPDT_FRAMEPTR(n) (1u << (2 + (n)))
#define   DMA0_RESOL_MASK       0x3FFFu
#define   DMAC_OTF_FORMAT       0x01Cu
#define   CSIS_DMA_IRQ_MASK_DBG 0x00F11FF0u
#define   DMAC_INT_DMA_ERROR    (1u << 12)
#define COMMON_DMA          0x170E0000ULL   /* fimc_is@17190000 reg[0] = IORESOURCE_CSIS_DMA */
#define   CDMA_ARB_PRI          0x00u       /* [3:2] PRI_1 = 1 (CSIS2 first), [1:0] PRI_0 = 2 */
#define   CDMA_CTRL             0x08u       /* b1 IP_PROCESSING */
#define   CDMA_SRAM_SPLIT       0x0Cu       /* [27:16] SRAM1, [11:0] SRAM0 */
#define   CDMA_MATRIX           0x10u       /* [4:0] */
#define DPHY_PAGE           0x19162000ULL   /* dphy_m0s4s4s4_csis2 0x19162300 (size 0x500): DPU block, pd-dpu on */
#define   DPHY_CSIS2            0x300u
#define   DPHY_SETTLE_1807      9u          /* fimc_is_hw_find_settle(1807): ALIGN 1810 -> 9 (8nm-lpp table) */
#define   ISPPRE_PHY_RST_CSIS2  (1u << 1)
#define CMU_TOP_DIV_BUSY        (1u << 16)
#define   IP_LHS_AXI_D          0x20C8u     /* the DMA's way out to DRAM */
#define SHOT_ALLOC              (16u * 1024u * 1024u)   /* width is fixed by DMA0_RESOL, lines are not: room for 4599 */
#define SHOT_FILL               0xEEu
#define SHOT_RING               3u          /* stream: frame n -> Ring[n % 3]; a finished frame stays 2 frames */
#define SHOT_RING_BYTES         (S10ECAM_SHOT_BYTES + 0x10000u)   /* mode 8; a ring buffer = its mode's Bytes + 64 KiB */
#define SHOT_CANARY             0xEEEEEEEEu /* the last 8 bytes of a frame: no 10-bit pixel is 0xEEEE */

/* ---------------------------------------------------------------------------------------------- state */
typedef struct _MAPS {
    volatile UCHAR *Top, *Peric1, *SysregP1, *GpioP1, *GpioP0, *I2cCam, *I2cPmic;
} MAPS;

typedef struct _REPORT {
    PCHAR  Buf;
    size_t Size;
    size_t Used;
} REPORT;

#ifndef S10ECAM_ENGINE_ONLY
static PDEVICE_OBJECT gDevice;
#endif
static KEVENT         gLock;       /* synchronization event: one stage at a time, the caller stays at PASSIVE_LEVEL */

#ifndef S10ECAM_ENGINE_ONLY
static DRIVER_UNLOAD   S10eCamUnload;
static DRIVER_DISPATCH S10eCamCreateClose, S10eCamIoctl;
DRIVER_INITIALIZE      DriverEntry;
#endif

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
    /* INT_STAT shows only ENABLED sources: Linux enables TRANSFER_DONE even when it polls (i2c-exynos5.c:775), and
       0.1 without it timed out with INT_STAT 000 (phone, 2026-11-16 03:04). */
    W32(b, HSI2C_INT_EN, HSI2C_INT_DONE | HSI2C_INT_ERRORS);
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
    *IntOut = st | (R32(b, HSI2C_TRANS_STATUS) << 12);   /* report: INT_STAT [11:0], TRANS_STATUS above */
    W32(b, HSI2C_INT_EN, 0);
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
        Log(r, "  S2MPB02 read 0x%02X failed 0x%08X (INT_STAT %03X, TRANS_STATUS %05X, FIFO %08X, ERR %08X)\r\n", Reg, s,
            st & 0xFFF, st >> 12, R32(m->I2cPmic, HSI2C_FIFO_STAT), R32(m->I2cPmic, HSI2C_ERR_STAT));
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
    Log(r, "  PERIC1_IP mux %08X div %08X (/%u) gate %08X -> %s (the I2C ipclk source when a user mux bit 4 is 1)\r\n",
        R32(m->Top, TOP_PERIC1_IP_MUX), R32(m->Top, TOP_PERIC1_IP_DIV), (R32(m->Top, TOP_PERIC1_IP_DIV) & 0xF) + 1,
        R32(m->Top, TOP_PERIC1_IP_GATE), ClkOn(R32(m->Top, TOP_PERIC1_IP_GATE)) ? "on" : "NOT on");
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

/* Power the front camera up exactly as the module file does (rails, subcam_sel, I2C pins, reset, MCLK). On a rail
   that does not take it powers down again itself. */
static NTSTATUS SensorPowerUp(MAPS *m, FOUND *f, REPORT *r)
{
    NTSTATUS s = STATUS_SUCCESS;

    if (!f->PmicOk || !f->CamBusOk) {
        Log(r, "power-up refused: S2MPB02 bus %s, sensor bus %s\r\n", f->PmicOk ? "ok" : "NOT ok", f->CamBusOk ? "ok" : "NOT ok");
        return STATUS_DEVICE_NOT_READY;
    }
    if ((f->L5 | f->L6 | f->L13 | f->L16) & PB_LDO_EN) {
        Log(r, "power-up refused: a camera rail is already on (someone else is using the camera?)\r\n");
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
    return STATUS_SUCCESS;
}

/* model ID 0x0000, revision 0x0002 (must be the DT's 0xA101), frame count 0x0005 */
static NTSTATUS SensorCheckId(MAPS *m, REPORT *r)
{
    NTSTATUS s = STATUS_SUCCESS;
    UCHAR    d[2];
    ULONG    st, model = 0, rev = 0, fc = 0, try_;

    Log(r, "== sensor 0x%02X on I2C_CAM2\r\n", SENSOR_ADDR);
    I2cReset(m->I2cCam);
    for (try_ = 0; try_ < 2; try_++) {
        s = RegRead(m->I2cCam, SENSOR_ADDR, 0x0000, 2, d, &st);
        if (NT_SUCCESS(s))
            break;
        Log(r, "  model ID read failed 0x%08X (INT_STAT %03X, TRANS_STATUS %05X, ERR %08X)%s\r\n", s, st & 0xFFF, st >> 12,
            R32(m->I2cCam, HSI2C_ERR_STAT),
            try_ == 0 ? " - 20 ms more, again" : "");
        SleepUs(20000);
    }
    if (!NT_SUCCESS(s))
        return s;
    model = ((ULONG)d[0] << 8) | d[1];
    if (NT_SUCCESS(RegRead(m->I2cCam, SENSOR_ADDR, 0x0002, 2, d, &st)))
        rev = ((ULONG)d[0] << 8) | d[1];
    if (NT_SUCCESS(RegRead(m->I2cCam, SENSOR_ADDR, 0x0005, 2, d, &st)))
        fc = d[0];
    Log(r, "  MODEL ID 0x%04X  REVISION 0x%04X (%s)  FRAME COUNT 0x%02X (%s)\r\n", model, rev,
        rev == SENSOR_REV ? "= DT rev_reg 0xA101: the S5K3J1" : "not the DT's 0xA101",
        fc, fc == 0xFF ? "not streaming, as expected" : "?");
    return rev == SENSOR_REV ? STATUS_SUCCESS : STATUS_DEVICE_DATA_ERROR;
}

static NTSTATUS Stage2(MAPS *m, FOUND *f, REPORT *r)
{
    NTSTATUS s = SensorPowerUp(m, f, r);

    if (!NT_SUCCESS(s))
        return s;
    s = SensorCheckId(m, r);
    PowerDown(m, f, r);
    return s;
}

/* ---------------------------------------------------------------------------------------------- stage 3 */

/* Read-only survey of the CSI receive path. Nothing is written anywhere. Every block is read only when the power
   domain it sits in reads ON in the PMU and its clock gate is provably on (a read of a powered-off or clock-stopped
   block can stall the bus = SError / hang).
   Without S10ECAM_FLAG_DEEP only the PMU and CMU_TOP are read (always-on / top blocks: no risk).
   0.3 then read everything inside ISPPRE in one call and BUGCHECKED (2026-11-16, no dump: the faulting read is
   unknown). Candidates: D_TZPC (Linux restores it, SMC 0x82000410, before its first CMU_ISPPRE access - K
   pmucal_local.c), or one register whose own Q-channel clock was stopped (the S9 precedent read a found-on ISPPRE
   with NO SMC, but a CMU_ISPPRE Q-channel read, 0x303C on the 9810, and SysMMU reads took external aborts there).
   0.4: one STEP per call (S10ECAM_STEP(Flags)), safest first; cam-s10.ps1 writes "step N ..." to disk write-through
   before each call, so a bugcheck names its step. Step 0 (= 0.3's all-in-one) is refused.
     1 CMU_ISPPRE BUS_USER 0x100 / BUSP 0x1804 (pmucal's isppre_save list reads exactly these)
     2 CMU_ISPPRE CLK_CON_GAT gates (7)        3 Q-channels CSIS2 / CSIS DMA / PDP DMA (the S9 wrote its CSIS QCHs)
     4 Q-channels LHM_AXI_P / SYSREG / SYSMMU  5 CSIS2 (aclk on)   6 CSIS DMA0 + common (both DMA aclks on)
     7 SYSREG_ISPPRE +0x500 (pclk on)
   Steps 5-7 re-read their gate (step 2's registers) first.
   2026-11-16 04:09 run: steps 1-7 read fine; step 8 (the SysMMU's non-secure frame 0x17210000, aclk gate on, QCH
   0x003F0002) took a synchronous external abort -> bugcheck 0x124 P1 0x11 - the 3deep crash too. Step 8 is GONE:
   the SysMMU is never read (the S9 precedent also took it as off/pass-through and its DMA worked).
   The D-PHY (0x19162300) is not read at all: it sits in the DPU block next to the running panel's DSIM PHY, and
   3b programs it from scratch anyway. Its PMU isolation bit is read (PMU = always on). */
static NTSTATUS Stage3(REPORT *r, ULONG Flags)
{
    volatile UCHAR *pmu = NULL, *top = NULL, *cmu = NULL, *sys = NULL, *csis = NULL, *dma = NULL;
    ULONG    dpuSt, ispSt, iso, tg, g, i, step;
    NTSTATUS s = STATUS_SUCCESS;
    static const struct { PCSTR Name; ULONG Reg; } gates[] = {
        { "CMU pclk     ", IP_CMU_PCLK },     { "LHM_AXI_P    ", IP_LHM_AXI_P },   { "SYSREG pclk  ", IP_SYSREG_PCLK },
        { "CSIS2 aclk   ", IP_CSIS2_ACLK },   { "CSIS DMA aclk", IP_CSISDMA_ACLK }, { "PDP DMA aclk ", IP_PDPDMA_ACLK },
        { "SYSMMU aclk  ", IP_SYSMMU_ACLK },
    };
    static const struct { PCSTR Name; ULONG Reg; } qch3[] = {
        { "CSIS2   ", IP_Q_CSIS2 }, { "CSIS DMA", IP_Q_CSISDMA }, { "PDP DMA ", IP_Q_PDPDMA },
    };
    static const struct { PCSTR Name; ULONG Reg; } qch4[] = {
        { "LHM_AXI_P", IP_Q_LHM_AXI_P }, { "SYSREG   ", IP_Q_SYSREG }, { "SYSMMU   ", IP_Q_SYSMMU },
    };

    pmu = Map(PMU_BASE, PMU_SIZE);
    top = Map(CMU_TOP_MAP, CMU_TOP_MAP_SIZE);
    if (!pmu || !top) {
        Log(r, "map failed\r\n");
        s = STATUS_INSUFFICIENT_RESOURCES;
        goto out;
    }

    Log(r, "== PMU (power domains)\r\n");
    dpuSt = R32(pmu, PMU_DPU_STAT);
    ispSt = R32(pmu, PMU_ISPPRE_STAT);
    iso = R32(pmu, PMU_MIPI_ISO_CSIS2);
    Log(r, "  pd-dpu    CONF %08X STATUS %08X -> %s\r\n", R32(pmu, PMU_DPU_CONF), dpuSt, (dpuSt & PD_ON) ? "ON" : "off");
    Log(r, "  pd-isppre CONF %08X STATUS %08X -> %s\r\n", R32(pmu, PMU_ISPPRE_CONF), ispSt, (ispSt & PD_ON) ? "ON" : "off");
    Log(r, "  CSIS2 D-PHY isolation (PMU +0x730) %08X -> %s\r\n", iso,
        (iso & 1) ? "bypassed (PHY usable)" : "isolated (PHY not powered for use)");

    Log(r, "== CMU_TOP -> ISPPRE bus clock\r\n");
    tg = R32(top, TOP_ISPPRE_BUS_GATE);
    Log(r, "  ISPPRE_BUS mux %08X div %08X (/%u) gate %08X -> %s\r\n", R32(top, TOP_ISPPRE_BUS_MUX),
        R32(top, TOP_ISPPRE_BUS_DIV), (R32(top, TOP_ISPPRE_BUS_DIV) & 0xF) + 1, tg, ClkOn(tg) ? "on" : "NOT on");

    if ((ispSt & PD_ON) == 0 || (dpuSt & PD_ON) == 0) {
        Log(r, "VERDICT: pd-isppre (or its parent pd-dpu) is OFF - nothing inside ISPPRE was touched. Stage 3b must power it\r\n"
               "  on first:"
               " PMU +0x2500 bit0 <- 1, wait +0x2504 bit0, then SMC 0x82000410 (D_TZPC restore, need_smc 0x17010204)\r\n");
        s = STATUS_DEVICE_POWERED_OFF;
        goto out;
    }
    if (!ClkOn(tg)) {
        Log(r, "VERDICT: pd-isppre is on but its bus clock is stopped in CMU_TOP - nothing inside ISPPRE was touched\r\n");
        s = STATUS_DEVICE_NOT_READY;
        goto out;
    }
    if ((Flags & S10ECAM_FLAG_DEEP) == 0) {
        Log(r, "VERDICT: pd-isppre and pd-dpu are ON, ISPPRE's bus clock runs. Stopping here (the safe part); the reads\r\n"
               "  inside ISPPRE are one step at a time: .\\cam-s10.ps1 3step\r\n");
        goto out;
    }
    step = S10ECAM_STEP(Flags);
    if (step < 1 || step > S10ECAM_STEP_MAX) {
        Log(r, "deep step %u refused: only 1..%u, one per call (0.3's all-in-one read bugchecked)\r\n", step, S10ECAM_STEP_MAX);
        s = STATUS_INVALID_PARAMETER;
        goto out;
    }

    Log(r, "== deep step %u\r\n", step);
    cmu = Map(CMU_ISPPRE, CMU_ISPPRE_SIZE);
    if (!cmu) {
        Log(r, "map failed\r\n");
        s = STATUS_INSUFFICIENT_RESOURCES;
        goto out;
    }
    switch (step) {
    case 1:
        Log(r, "  CMU_ISPPRE BUS_USER %08X (bit 4: 1 = CMU_TOP clock) BUSP div %08X\r\n", R32(cmu, IP_BUS_USER),
            R32(cmu, IP_BUSP_DIV));
        break;
    case 2:
        for (i = 0; i < RTL_NUMBER_OF(gates); i++) {
            g = R32(cmu, gates[i].Reg);
            Log(r, "  %s gate [+%04X] %08X -> %s\r\n", gates[i].Name, gates[i].Reg, g, ClkOn(g) ? "on" : "NOT on");
        }
        break;
    case 3:
        for (i = 0; i < RTL_NUMBER_OF(qch3); i++)
            Log(r, "  QCH %s [+%04X] %08X\r\n", qch3[i].Name, qch3[i].Reg, R32(cmu, qch3[i].Reg));
        break;
    case 4:
        for (i = 0; i < RTL_NUMBER_OF(qch4); i++)
            Log(r, "  QCH %s [+%04X] %08X\r\n", qch4[i].Name, qch4[i].Reg, R32(cmu, qch4[i].Reg));
        break;
    case 5:
        g = R32(cmu, IP_CSIS2_ACLK);
        if (!ClkOn(g) || !ClkOn(R32(cmu, IP_LHM_AXI_P))) {
            Log(r, "  CSIS2 not read: aclk %08X / LHM_AXI_P %08X gated\r\n", g, R32(cmu, IP_LHM_AXI_P));
            break;
        }
        csis = Map(CSIS2_BASE, PAGE_4K);
        if (!csis) { s = STATUS_INSUFFICIENT_RESOURCES; break; }
        Log(r, "  CSIS2 VERSION %08X CMN_CTRL %08X CLK_CTRL %08X\r\n", R32(csis, CSIS_VERSION), R32(csis, CSIS_CMN_CTRL),
            R32(csis, CSIS_CLK_CTRL));
        Log(r, "  INT_MSK0 %08X SRC0 %08X  INT_MSK1 %08X SRC1 %08X\r\n", R32(csis, CSIS_INT_MSK0), R32(csis, CSIS_INT_SRC0),
            R32(csis, CSIS_INT_MSK1), R32(csis, CSIS_INT_SRC1));
        Log(r, "  ISP_CONFIG_CH0 %08X ISP_RESOL_CH0 %08X FRM_CNT_CH0 %08X\r\n", R32(csis, CSIS_ISP_CONFIG_CH0),
            R32(csis, CSIS_ISP_RESOL_CH0), R32(csis, CSIS_FRM_CNT_CH0));
        Log(r, "  PHY_STATUS %08X PHY_CMN_CTRL %08X\r\n", R32(csis, CSIS_PHY_STATUS), R32(csis, CSIS_PHY_CMN_CTRL));
        break;
    case 6:
        /* which DMA gate feeds 0x170C0000 is not stated anywhere read so far (CSISX4_PDP_DMA by name; mode1's
           0x17120000 is likely PDP_TOP_DMA): both must be on, so a wrong guess can only skip the read */
        if (!ClkOn(R32(cmu, IP_CSISDMA_ACLK)) || !ClkOn(R32(cmu, IP_PDPDMA_ACLK)) || !ClkOn(R32(cmu, IP_LHM_AXI_P))) {
            Log(r, "  CSIS DMA not read: a DMA aclk or LHM_AXI_P gated\r\n");
            break;
        }
        dma = Map(CSIS_DMA_PAGE, PAGE_4K);
        if (!dma) { s = STATUS_INSUFFICIENT_RESOURCES; break; }
        Log(r, "  DMA0 CTRL %08X FMT %08X ADDR1 %08X ACT_CTRL %08X RESOL %08X FCNTSEQ %08X STAT %08X\r\n",
            R32(dma, DMA0_CTRL), R32(dma, DMA0_FMT), R32(dma, DMA0_ADDR1), R32(dma, DMA0_ACT_CTRL), R32(dma, DMA0_RESOL),
            R32(dma, DMA0_FCNTSEQ), R32(dma, DMA0_FCNTSEQ_STAT));
        Log(r, "  common DATA_CTRL %08X INT_MASK %08X INT_SRC %08X CMN_CTRL %08X ERR_CODE %08X\r\n",
            R32(dma, DMAC_BASE + DMAC_DATA_CTRL), R32(dma, DMAC_BASE + DMAC_INT_MASK), R32(dma, DMAC_BASE + DMAC_INT_SRC),
            R32(dma, DMAC_BASE + DMAC_CMN_CTRL), R32(dma, DMAC_BASE + DMAC_ERR_CODE));
        break;
    case 7:
        if (!ClkOn(R32(cmu, IP_SYSREG_PCLK)) || !ClkOn(R32(cmu, IP_LHM_AXI_P))) {
            Log(r, "  SYSREG_ISPPRE not read: pclk or LHM_AXI_P gated\r\n");
            break;
        }
        sys = Map(SYSREG_ISPPRE_PAGE, PAGE_4K);
        if (!sys) { s = STATUS_INSUFFICIENT_RESOURCES; break; }
        g = R32(sys, ISPPRE_PHY_RST);
        Log(r, "  SYSREG_ISPPRE +0x500 (D-PHY resets) %08X -> CSIS2 D-PHY %s\r\n", g,
            (g & (1u << 1)) ? "out of reset" : "IN reset");
        break;
    case 8:
        /* 0.11: two reads of the 3AA0 ID registers, only with its aclk gate, its Q-channel and LHM_AXI_P on. RISK (written
           in notes/s10-isp.md): the ISPPRE SysMMU's frame answered a read with an external abort (step 8 of 0.4) - 3AA0
           may be protected the same way; an abort = bugcheck 0x124 + reboot, nothing else */
        g = R32(cmu, IP_3AA0_ACLK);
        Log(r, "  3AA0 aclk gate [+%04X] %08X -> %s, QCH [+%04X] %08X, LHM_AXI_P %08X\r\n", IP_3AA0_ACLK, g,
            ClkOn(g) ? "on" : "NOT on", IP_Q_3AA0, R32(cmu, IP_Q_3AA0), R32(cmu, IP_LHM_AXI_P));
        if (!ClkOn(g) || !ClkOn(R32(cmu, IP_LHM_AXI_P))) {
            Log(r, "  3AA0 not read: aclk or LHM_AXI_P gated\r\n");
            break;
        }
        csis = Map(TAA0_BASE, PAGE_4K);    /* the csis pointer reused for this 4 KiB mapping: unmapped at out */
        if (!csis) { s = STATUS_INSUFFICIENT_RESOURCES; break; }
        g = R32(csis, TAA_ID_VERSION);
        i = R32(csis, TAA_ID_CODE);
        Log(r, "  3AA0 +0xF0 %08X +0xF4 %08X -> %s\r\n", g, i,
            (g == 0x07010000u && i == 0xCC22u) ? "= Android's (3AA v7.1, the library's ISP_Fimc3aaChainV7P1P0)" :
            "NOT Android's 0x07010000 / 0x0000CC22");
        break;
    }
    Log(r, "step %u done\r\n", step);

out:
    if (dma)  MmUnmapIoSpace((PVOID)dma, PAGE_4K);
    if (csis) MmUnmapIoSpace((PVOID)csis, PAGE_4K);
    if (sys)  MmUnmapIoSpace((PVOID)sys, PAGE_4K);
    if (cmu)  MmUnmapIoSpace((PVOID)cmu, CMU_ISPPRE_SIZE);
    if (top)  MmUnmapIoSpace((PVOID)top, CMU_TOP_MAP_SIZE);
    if (pmu)  MmUnmapIoSpace((PVOID)pmu, PMU_SIZE);
    return s;
}

/* ---------------------------------------------------------------------------------------------- stage 4 (3b) */

static NTSTATUS SenW16(MAPS *m, ULONG Reg, ULONG Val)
{
    UCHAR b[4] = { (UCHAR)(Reg >> 8), (UCHAR)Reg, (UCHAR)(Val >> 8), (UCHAR)Val };
    ULONG st;
    return I2cXfer(m->I2cCam, SENSOR_ADDR, FALSE, b, 4, TRUE, &st);
}

static NTSTATUS SenW8(MAPS *m, ULONG Reg, ULONG Val)
{
    UCHAR b[3] = { (UCHAR)(Reg >> 8), (UCHAR)Reg, (UCHAR)Val };
    ULONG st;
    return I2cXfer(m->I2cCam, SENSOR_ADDR, FALSE, b, 3, TRUE, &st);
}

static ULONG SenR16(MAPS *m, ULONG Reg)
{
    UCHAR d[2];
    ULONG st;
    if (!NT_SUCCESS(RegRead(m->I2cCam, SENSOR_ADDR, Reg, 2, d, &st)))
        return 0xFFFFFFFFu;
    return ((ULONG)d[0] << 8) | d[1];
}

/* A setfile table (S10eCam3j1.h). Linux sleeps 3 ms before each (sensor_cis_set_registers). Consecutive 0x6F12 words
   go out as one I2C message of up to 7 words (16-byte FIFO): what the kernel's burst does in one longer message. */
static NTSTATUS SenTable(MAPS *m, const S10E_REG16 *t, ULONG n, PCSTR Name, REPORT *r)
{
    ULONG    i = 0, k, msgs = 0, st = 0;
    UCHAR    b[HSI2C_FIFO_DEPTH];
    NTSTATUS s = STATUS_SUCCESS;

    SleepUs(3000);
    while (i < n) {
        if ((ULONG)t[i].Addr == S10E_REG_DELAY) {   /* (ULONG): MSVC C4389 on USHORT-int vs unsigned */
            SleepUs(t[i].Val);
            i++;
            continue;
        }
        b[0] = (UCHAR)(t[i].Addr >> 8);
        b[1] = (UCHAR)t[i].Addr;
        k = 0;
        do {
            b[2 + 2 * k] = (UCHAR)(t[i + k].Val >> 8);
            b[3 + 2 * k] = (UCHAR)t[i + k].Val;
            k++;
        } while (t[i].Addr == 0x6F12 && k < (HSI2C_FIFO_DEPTH - 2) / 2 && i + k < n && t[i + k].Addr == 0x6F12);
        s = I2cXfer(m->I2cCam, SENSOR_ADDR, FALSE, b, 2 + 2 * k, TRUE, &st);
        if (!NT_SUCCESS(s)) {
            Log(r, "  %s: write #%u (0x%04X) failed 0x%08X (INT_STAT %03X)\r\n", Name, i, t[i].Addr, s, st & 0xFFF);
            return s;
        }
        msgs++;
        i += k;
    }
    Log(r, "  %s: %u entries in %u I2C messages\r\n", Name, n, msgs);
    return s;
}

static VOID RmwField(volatile UCHAR *b, ULONG Off, ULONG Shift, ULONG Width, ULONG Val)
{
    ULONG mask = ((Width >= 32) ? 0xFFFFFFFFu : ((1u << Width) - 1u)) << Shift;
    W32(b, Off, (R32(b, Off) & ~mask) | ((Val << Shift) & mask));
}

/* phy-exynos-mipi.c __set_phy_cfg_0502_0002_dphy for 4 lanes, 1807 Mbps, type 0xDC (CSI_ID_C), settle 9 */
static VOID DphyConfig(volatile UCHAR *p)
{
    ULONG i;

    W32(p, 0x0000, 0x00000001);                          /* SC_GNR_CON0 */
    W32(p, 0x0004, 0x00001450);                          /* SC_GNR_CON1 */
    W32(p, 0x0008, 0x00000004);                          /* SC_ANA_CON0 (speed <= 4500) */
    W32(p, 0x000C, 0x00009000);                          /* SC_ANA_CON1 */
    W32(p, 0x0010, 0x00000005);                          /* SC_ANA_CON2 */
    RmwField(p, 0x0010, 8, 2, 0);                        /* clk_div1234_mc = max(0, 5 - ceil(903 / 26)) = 0 */
    W32(p, 0x0014, 0x00000600);                          /* SC_ANA_CON3 */
    W32(p, 0x0030, 0x00000301);                          /* SC_TIME_CON0 */
    for (i = 0; i < 4; i++) {
        ULONG o = 0x100 * i;
        W32(p, 0x0100 + o, 0x00000001);                  /* SD_GNR_CON0 */
        W32(p, 0x0104 + o, 0x00001450);                  /* SD_GNR_CON1 */
        W32(p, 0x0108 + o, 0x00000004);                  /* SD_ANA_CON0 */
        W32(p, 0x010C + o, 0x00009000);                  /* SD_ANA_CON1 (speed < 4500) */
        W32(p, 0x0110 + o, 0x00000005);                  /* SD_ANA_CON2 */
        RmwField(p, 0x0110 + o, 8, 2, 3);                /* skew_delay_sel 3 (1500 <= 1807 < 2000) */
        W32(p, 0x0114 + o, 0x00000600);                  /* SD_ANA_CON3 */
        if (i < 3)
            W32(p, 0x0124 + o, 0x00000040);              /* SD_ANA_CON7 (DC combo lanes 0-2) */
        RmwField(p, 0x0130 + o, 0, 8, DPHY_SETTLE_1807); /* SD_TIME_CON0 settle */
        RmwField(p, 0x0130 + o, 8, 1, 0);                /* settle_clk_sel 0 (>= 1500) */
        W32(p, 0x0134 + o, 0x00000003);                  /* SD_TIME_CON1 */
        RmwField(p, 0x0140 + o, 0, 1, 1);                /* SD_DESKEW_CON0 skew_cal_en (>= 1500) */
        W32(p, 0x0150 + o, 0x0000081A);                  /* SD_DESKEW_CON4 */
    }
}

static VOID CsisConfigVc(volatile UCHAR *c, ULONG Vc, ULONG Fmt, ULONG PixelMode, ULONG W, ULONG H)
{
    ULONG o = CSIS_ISP_CONFIG_CH0 + CSIS_CH_STRIDE * Vc;
    ULONG v = R32(c, o);

    v &= ~(0x3u | (0x3Fu << 2) | (1u << 8) | (1u << 9) | (1u << 11) | (0x3u << 12));
    v |= (Vc & 0x3u) | ((Fmt & 0x3Fu) << 2) | (((Fmt >> 6) & 1u) << 8) | (((Fmt >> 7) & 1u) << 9) | (PixelMode << 12);
    W32(c, o, v);                                                   /* VC map = Vc, DATAFORMAT, DECOMP, POTF 0 */
    W32(c, CSIS_ISP_RESOL_CH0 + CSIS_CH_STRIDE * Vc, (H << 16) | W);
}

/* 0.9: the two sensor modes. Mode 11 (S10ECAM_SHOT_BIN): fimc-is-device-module-3j1.c cfg "912 x 684 @120.4fps MIPI
   lane: 4, MIPI data rate 1807" = the same lanes and rate as mode 8 (so the same D-PHY settle 9), VC0 RAW10 912 x 684
   and no PDAF VC (PD_NONE); pllinfo pclk 201810000 x 4, LLP 0x2318. Its 120 fps frame length (FLL 0x02EA) is set to
   30 fps the way sensor_3j1_cis_set_frame_duration does: FLL = 807 MHz x 33333 us / 8984 = 2994. */
typedef struct _SHOT_MODE {
    ULONG             W, H, Stride, Bytes;
    const S10E_REG16 *Tab;
    ULONG             TabN;
    PCSTR             Name;
    ULONG             Fll;                               /* 0 = the setfile's */
    ULONG             PdafLines;                         /* VC1 (PD_MOD3) lines, 0 = no PDAF VC */
} SHOT_MODE;

static const SHOT_MODE gModes[2] = {
    { S10ECAM_SHOT_W, S10ECAM_SHOT_H, S10ECAM_SHOT_STRIDE, S10ECAM_SHOT_BYTES, S10eCam3j1Mode1824,
      RTL_NUMBER_OF(S10eCam3j1Mode1824), "mode 8 1824x1368@30", 0, 342 },
    { S10ECAM_BIN_W, S10ECAM_BIN_H, S10ECAM_BIN_STRIDE, S10ECAM_BIN_BYTES, S10eCam3j1Mode912,
      RTL_NUMBER_OF(S10eCam3j1Mode912), "mode 11 912x684 (120 fps setfile, FLL 2994 = 30 fps)", 2994, 0 },
};

/* 0.8: a frame ring allocated ONCE by a long-lived owner and lent to every stream. S10eCamera (0.4) fills it at
   device start (boot): 3 x 4.8 MiB physically contiguous below 4 GB was no longer to be had ~75 min after boot
   (2026-11-16 15:57, "no contiguous 5056000 bytes below 4 GB"). S10eCam itself never fills it (Ok stays FALSE). */
typedef struct _RINGPOOL {
    PUCHAR              Ring[SHOT_RING];
    PHYSICAL_ADDRESS    Pa[SHOT_RING];
    MEMORY_CACHING_TYPE Cache;
    ULONG               Bytes;                           /* 0.9: each buffer's size (its mode's Bytes + 64 KiB) */
    BOOLEAN             Ok;
} RINGPOOL;

static RINGPOOL gPool;

typedef struct _SHOT {
    volatile UCHAR *Pmu, *Cmu, *Sys, *Csis, *Dma, *Cdma, *Phy;
    PUCHAR          Buf;                                 /* shot: one 16 MiB buffer */
    PHYSICAL_ADDRESS Pa;
    PUCHAR          Ring[SHOT_RING];                     /* stream: frame buffers, ADDRn = Ring[(n - 1) % SHOT_RING] */
    PHYSICAL_ADDRESS RingPa[SHOT_RING];
    ULONG           NBuf;
    MEMORY_CACHING_TYPE Cache;                           /* the buffers' mapping (0.7: MmNonCached or MmWriteCombined) */
    BOOLEAN         Borrowed;                            /* 0.8: Ring[] is gPool's (kept by its owner, not freed here) */
    const SHOT_MODE *Mode;                               /* 0.9: gModes[0] (mode 8) or gModes[1] (mode 11) */
    ULONG           RingBytes;                           /* 0.9: each Ring[] buffer's size */
    ULONG           TopDiv, Iso, PhyRst, CdmaSaved[4], DmacMask, CsisMsk0, CsisMsk1, CsisCmn;
    BOOLEAN         DivSet, PhyOn, CsisOn, DmaOn, SensorOn, Streaming;
} SHOT;

static VOID ShotUnmap(SHOT *x)
{
    if (x->Phy)  MmUnmapIoSpace((PVOID)x->Phy, PAGE_4K);
    if (x->Cdma) MmUnmapIoSpace((PVOID)x->Cdma, PAGE_4K);
    if (x->Dma)  MmUnmapIoSpace((PVOID)x->Dma, PAGE_4K);
    if (x->Csis) MmUnmapIoSpace((PVOID)x->Csis, PAGE_4K);
    if (x->Sys)  MmUnmapIoSpace((PVOID)x->Sys, PAGE_4K);
    if (x->Cmu)  MmUnmapIoSpace((PVOID)x->Cmu, CMU_ISPPRE_SIZE);
    if (x->Pmu)  MmUnmapIoSpace((PVOID)x->Pmu, PMU_SIZE);
    if (x->Buf)  MmFreeContiguousMemorySpecifyCache(x->Buf, SHOT_ALLOC, x->Cache);
    for (ULONG i = 0; i < SHOT_RING; i++)
        if (x->Ring[i] && !x->Borrowed) MmFreeContiguousMemorySpecifyCache(x->Ring[i], x->RingBytes, x->Cache);
    RtlZeroMemory(x, sizeof(*x));
}

/* Everything back, in Linux's stream-off / power-off order. Safe to call at any stage. */
static VOID ShotTeardown(MAPS *m, FOUND *f, SHOT *x, REPORT *r)
{
    ULONG w;

    Log(r, "== teardown\r\n");
    if (x->DmaOn) {
        W32(x->Dma, DMA0_CTRL, R32(x->Dma, DMA0_CTRL) & ~(DMA0_EN | DMA0_UPDT_PTR_EN));   /* csi_hw_s_output_dma off */
        SleepUs(50000);                                  /* the frame in flight ends (33 ms at 30 fps) */
    }
    if (x->Streaming) {
        (VOID)SenW8(m, 0x0100, 0x00);                    /* sensor stream off */
        SleepUs(40000);
        x->Streaming = FALSE;
    }
    if (x->CsisOn) {
        W32(x->Csis, CSIS_PHY_CMN_CTRL, R32(x->Csis, CSIS_PHY_CMN_CTRL) & ~(CSIS_PHY_EN_CLK | (0xFu << CSIS_PHY_EN_DAT_SHIFT)));
        W32(x->Csis, CSIS_CMN_CTRL, R32(x->Csis, CSIS_CMN_CTRL) & ~CSIS_CMN_CSI_EN);
        /* csi_hw_disable (ENABLE_HWACG_CONTROL, v7_0_0 fimc-is-config.h) sets QCHANNEL_EN again: here = as found */
        W32(x->Csis, CSIS_CMN_CTRL, (R32(x->Csis, CSIS_CMN_CTRL) & ~CSIS_CMN_QCH_EN) | (x->CsisCmn & CSIS_CMN_QCH_EN));
        W32(x->Csis, CSIS_CMN_CTRL, R32(x->Csis, CSIS_CMN_CTRL) | CSIS_CMN_SW_RESET);
        for (w = 0; w < 10 && (R32(x->Csis, CSIS_CMN_CTRL) & CSIS_CMN_SW_RESET); w++) KeStallExecutionProcessor(10);
        W32(x->Csis, CSIS_INT_MSK0, x->CsisMsk0);
        W32(x->Csis, CSIS_INT_MSK1, x->CsisMsk1);
        x->CsisOn = FALSE;
    }
    if (x->DmaOn) {
        W32(x->Dma, DMA0_CTRL, 0);
        W32(x->Dma, DMAC_BASE + DMAC_INT_MASK, x->DmacMask);
        W32(x->Cdma, CDMA_ARB_PRI, x->CdmaSaved[0]);
        W32(x->Cdma, CDMA_CTRL, x->CdmaSaved[1]);
        W32(x->Cdma, CDMA_SRAM_SPLIT, x->CdmaSaved[2]);
        W32(x->Cdma, CDMA_MATRIX, x->CdmaSaved[3]);
        x->DmaOn = FALSE;
    }
    if (x->PhyOn) {                                      /* phy power_off: reset, then isolation (found values back) */
        W32(x->Sys, ISPPRE_PHY_RST, R32(x->Sys, ISPPRE_PHY_RST) & ~ISPPRE_PHY_RST_CSIS2);
        W32(x->Sys, ISPPRE_PHY_RST, (R32(x->Sys, ISPPRE_PHY_RST) & ~ISPPRE_PHY_RST_CSIS2) | (x->PhyRst & ISPPRE_PHY_RST_CSIS2));
        W32(x->Pmu, PMU_MIPI_ISO_CSIS2, x->Iso);
        x->PhyOn = FALSE;
    }
    if (x->SensorOn) {
        PowerDown(m, f, r);
        x->SensorOn = FALSE;
    }
    if (x->DivSet) {
        W32(m->Top, TOP_ISPPRE_BUS_DIV, (R32(m->Top, TOP_ISPPRE_BUS_DIV) & ~0xFu) | (x->TopDiv & 0xFu));
        for (w = 0; w < 100 && (R32(m->Top, TOP_ISPPRE_BUS_DIV) & CMU_TOP_DIV_BUSY); w++) KeStallExecutionProcessor(1);
        x->DivSet = FALSE;
    }
    if (x->Pmu && x->Sys && x->Csis && x->Dma)          /* a partial map (ShotStart's first failure) logs nothing */
        Log(r, "  CMU_TOP ISPPRE_BUS div %08X, PMU iso %08X, sysreg +0x500 %08X, CSIS2 CMN_CTRL %08X, DMA0 CTRL %08X\r\n",
            R32(m->Top, TOP_ISPPRE_BUS_DIV), R32(x->Pmu, PMU_MIPI_ISO_CSIS2), R32(x->Sys, ISPPRE_PHY_RST),
            R32(x->Csis, CSIS_CMN_CTRL), R32(x->Dma, DMA0_CTRL));
}

/* What the DMA left in the buffer: rows written (any byte not SHOT_FILL), past the frame too, and a value summary. */
static VOID ShotAnalyse(SHOT *x, REPORT *r)
{
    ULONG   y, xx, firstRow = 0xFFFFFFFFu, lastRow = 0, rowsTouched = 0, over = 0, hi = 0, px = 0;
    ULONG64 sum = 0;
    const SHOT_MODE *md = x->Mode;
    ULONG   maxRows = SHOT_ALLOC / md->Stride;

    for (y = 0; y < maxRows; y++) {
        PUCHAR row = x->Buf + (SIZE_T)y * md->Stride;
        BOOLEAN touched = FALSE;
        for (xx = 0; xx < md->Stride; xx += 64) {
            if ((ULONG)row[xx] != SHOT_FILL || (ULONG)row[xx + 1] != SHOT_FILL) { touched = TRUE; break; }
        }
        if (!touched)
            continue;
        rowsTouched++;
        if (firstRow == 0xFFFFFFFFu) firstRow = y;
        lastRow = y;
        if (y >= md->H)
            over++;
        else if ((y & 15) == 0) {
            for (xx = 0; xx < md->W; xx += 8) {
                USHORT v = ((PUSHORT)row)[xx];
                if (v > 0x3FF) hi++;
                sum += (v & 0x3FFu);
                px++;
            }
        }
    }
    for (y = SHOT_ALLOC; y >= 64; y -= 64) {             /* the DMA's extent: last 64-byte block not all fill */
        for (xx = y - 64; xx < y; xx += 4)              /* 32-bit reads: the buffer is uncached */
            if (*(volatile ULONG *)(x->Buf + xx) != SHOT_FILL * 0x01010101u) break;
        if (xx < y) break;
    }
    Log(r, "  DMA extent: %u bytes from the buffer start (a full frame = %u)\r\n", y, md->Bytes);
    if (rowsTouched == 0) {
        Log(r, "  buffer: NOTHING written (every sampled byte still 0x%02X)\r\n", SHOT_FILL);
        return;
    }
    Log(r, "  buffer: %u rows written (first %u, last %u), %u past row %u; sampled %u px: mean %u / 1023, %u above 0x3FF\r\n",
        rowsTouched, firstRow, lastRow, over, md->H - 1, px, px ? (ULONG)(sum / px) : 0, hi);
    Log(r, "  row 0: %04X %04X %04X %04X %04X %04X %04X %04X\r\n", ((PUSHORT)x->Buf)[0], ((PUSHORT)x->Buf)[1],
        ((PUSHORT)x->Buf)[2], ((PUSHORT)x->Buf)[3], ((PUSHORT)x->Buf)[4], ((PUSHORT)x->Buf)[5], ((PUSHORT)x->Buf)[6],
        ((PUSHORT)x->Buf)[7]);
}

#ifdef S10ECAM_ENGINE_ONLY   /* only S10eCamera owns a pool (S10eCam.sys would warn C4505 for the unused pair) */
static VOID RingPoolFree(VOID)
{
    ULONG i;

    for (i = 0; i < SHOT_RING; i++)
        if (gPool.Ring[i] != NULL)
            MmFreeContiguousMemorySpecifyCache(gPool.Ring[i], gPool.Bytes, gPool.Cache);
    RtlZeroMemory(&gPool, sizeof(gPool));
}

/* 3 x Bytes contiguous below 4 GB; write-combined if asked, non-cached if that (or WC) fails. TRUE = Ok.
   Bytes: a mode's Bytes + 64 KiB (SHOT_RING_BYTES for mode 8; S10ECAM_BIN_BYTES + 0x10000 for mode 11). */
static BOOLEAN RingPoolAlloc(BOOLEAN Wc, ULONG Bytes)
{
    PHYSICAL_ADDRESS lo, hi, bnd;
    ULONG            i, pass;

    RingPoolFree();
    lo.QuadPart = 0;
    hi.QuadPart = 0xFFFFFFFFLL;
    bnd.QuadPart = 0;
    for (pass = Wc ? 0u : 1u; pass < 2; pass++) {
        gPool.Cache = pass == 0 ? MmWriteCombined : MmNonCached;
        gPool.Bytes = Bytes;
        for (i = 0; i < SHOT_RING; i++) {
            gPool.Ring[i] = (PUCHAR)MmAllocateContiguousMemorySpecifyCache(Bytes, lo, hi, bnd, gPool.Cache);
            if (gPool.Ring[i] == NULL)
                break;
            gPool.Pa[i] = MmGetPhysicalAddress(gPool.Ring[i]);
        }
        if (i == SHOT_RING) {
            gPool.Ok = TRUE;
            return TRUE;
        }
        RingPoolFree();
    }
    return FALSE;
}
#endif

/* Everything up to and including sensor stream-on. On failure the caller tears down (ShotTeardown + ShotUnmap):
   x's flags say what was done. nbuf 1 = the 16 MiB single-shot buffer, SHOT_RING = the streaming ring. */
static NTSTATUS ShotStart(MAPS *m, FOUND *f, SHOT *px, const S10ECAM_SHOT_IN *in, ULONG nbuf, REPORT *r)
{
#define x (*px)
    NTSTATUS         s = STATUS_SUCCESS;
    PHYSICAL_ADDRESS lo, hi, bnd;
    ULONG            g, w, d, ow, oh, i;
    ULONG            phaseMax = S10ECAM_SHOT_PHASE(in->Flags);   /* 0 = all; 1..4 = stop after that phase (bisecting) */

    RtlZeroMemory(&x, sizeof(x));
    x.NBuf = nbuf;
    x.Mode = (in->Flags & S10ECAM_SHOT_BIN) ? &gModes[1] : &gModes[0];
    x.RingBytes = x.Mode->Bytes + 0x10000u;

    /* --- preconditions: the domain and the clocks stage 3 saw, mapped */
    x.Pmu = Map(PMU_BASE, PMU_SIZE);
    x.Cmu = Map(CMU_ISPPRE, CMU_ISPPRE_SIZE);
    x.Sys = Map(SYSREG_ISPPRE_PAGE, PAGE_4K);
    x.Csis = Map(CSIS2_BASE, PAGE_4K);
    x.Dma = Map(CSIS_DMA_PAGE, PAGE_4K);
    x.Cdma = Map(COMMON_DMA, PAGE_4K);
    x.Phy = Map(DPHY_PAGE, PAGE_4K);
    if (!x.Pmu || !x.Cmu || !x.Sys || !x.Csis || !x.Dma || !x.Cdma || !x.Phy) {
        Log(r, "map failed\r\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Log(r, "== stage 4: preconditions\r\n");
    g = R32(m->Top, TOP_ISPPRE_BUS_GATE);
    if (!(R32(x.Pmu, PMU_DPU_STAT) & PD_ON) || !(R32(x.Pmu, PMU_ISPPRE_STAT) & PD_ON) || !ClkOn(g)) {
        Log(r, "  refused: pd-dpu %08X pd-isppre %08X ISPPRE_BUS gate %08X\r\n", R32(x.Pmu, PMU_DPU_STAT),
            R32(x.Pmu, PMU_ISPPRE_STAT), g);
        return STATUS_DEVICE_POWERED_OFF;
    }
    if (!ClkOn(R32(x.Cmu, IP_LHM_AXI_P)) || !ClkOn(R32(x.Cmu, IP_CSIS2_ACLK)) || !ClkOn(R32(x.Cmu, IP_CSISDMA_ACLK)) ||
        !ClkOn(R32(x.Cmu, IP_PDPDMA_ACLK)) || !ClkOn(R32(x.Cmu, IP_SYSREG_PCLK)) || !ClkOn(R32(x.Cmu, IP_LHS_AXI_D))) {
        Log(r, "  refused: an ISPPRE gate is off (LHM %08X CSIS2 %08X DMA %08X PDP %08X SYSREG %08X LHS_D %08X)\r\n",
            R32(x.Cmu, IP_LHM_AXI_P), R32(x.Cmu, IP_CSIS2_ACLK), R32(x.Cmu, IP_CSISDMA_ACLK), R32(x.Cmu, IP_PDPDMA_ACLK),
            R32(x.Cmu, IP_SYSREG_PCLK), R32(x.Cmu, IP_LHS_AXI_D));
        return STATUS_DEVICE_NOT_READY;
    }
    if (R32(x.Csis, CSIS_CMN_CTRL) & CSIS_CMN_CSI_EN) {
        Log(r, "  refused: CSIS2 is already enabled (CMN_CTRL %08X)\r\n", R32(x.Csis, CSIS_CMN_CTRL));
        return STATUS_DEVICE_BUSY;
    }

    /* --- the buffer: physically contiguous, below 4 GB (the DMA address registers are 32-bit; the SysMMU is
       never touched = left in its reset bypass), uncached, filled so the analysis sees what the DMA wrote.
       0.7: MmNonCached (default) or MmWriteCombined (S10ECAM_SHOT_WC). Neither is cached, so the DMA's writes need no
       cache maintenance either way; 0.6's copy out of MmNonCached ran at ~80 MB/s (notes, 2026-11-16 14:49), which
       mapping reads faster on this CPU is what the flag measures. */
    x.Cache = (in->Flags & S10ECAM_SHOT_WC) ? MmWriteCombined : MmNonCached;
    lo.QuadPart = 0;
    hi.QuadPart = 0xFFFFFFFFLL;
    bnd.QuadPart = 0;
    if (nbuf == 1) {
        x.Buf = (PUCHAR)MmAllocateContiguousMemorySpecifyCache(SHOT_ALLOC, lo, hi, bnd, x.Cache);
        if (x.Buf == NULL) {
            Log(r, "  no %u MiB contiguous below 4 GB\r\n", SHOT_ALLOC >> 20);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        x.Pa = MmGetPhysicalAddress(x.Buf);
        RtlFillMemory(x.Buf, SHOT_ALLOC, SHOT_FILL);
        Log(r, "  buffer %u MiB at PA %08X%08X (%s)\r\n", SHOT_ALLOC >> 20, (ULONG)(x.Pa.QuadPart >> 32), (ULONG)x.Pa.QuadPart,
            x.Cache == MmWriteCombined ? "write-combined" : "non-cached");
    } else if (gPool.Ok && gPool.Bytes >= x.RingBytes) { /* 0.8: the owner's ring, allocated at its start */
        x.Borrowed = TRUE;
        x.Cache = gPool.Cache;
        x.RingBytes = gPool.Bytes;
        for (i = 0; i < SHOT_RING; i++) {
            x.Ring[i] = gPool.Ring[i];
            x.RingPa[i] = gPool.Pa[i];
            RtlFillMemory(x.Ring[i], x.RingBytes, SHOT_FILL);
            Log(r, "  ring %u: %u bytes at PA %08X%08X (%s, kept from device start)\r\n", i, x.RingBytes,
                (ULONG)(x.RingPa[i].QuadPart >> 32), (ULONG)x.RingPa[i].QuadPart,
                x.Cache == MmWriteCombined ? "write-combined" : "non-cached");
        }
    } else {
        for (i = 0; i < SHOT_RING; i++) {
            x.Ring[i] = (PUCHAR)MmAllocateContiguousMemorySpecifyCache(x.RingBytes, lo, hi, bnd, x.Cache);
            if (x.Ring[i] == NULL) {
                Log(r, "  no contiguous %u bytes below 4 GB for ring buffer %u\r\n", x.RingBytes, i);
                return STATUS_INSUFFICIENT_RESOURCES;
            }
            x.RingPa[i] = MmGetPhysicalAddress(x.Ring[i]);
            RtlFillMemory(x.Ring[i], x.RingBytes, SHOT_FILL);
            Log(r, "  ring %u: %u bytes at PA %08X%08X (%s)\r\n", i, x.RingBytes, (ULONG)(x.RingPa[i].QuadPart >> 32),
                (ULONG)x.RingPa[i].QuadPart, x.Cache == MmWriteCombined ? "write-combined" : "non-cached");
        }
    }

    /* --- found values */
    x.TopDiv = R32(m->Top, TOP_ISPPRE_BUS_DIV);
    x.Iso = R32(x.Pmu, PMU_MIPI_ISO_CSIS2);
    x.PhyRst = R32(x.Sys, ISPPRE_PHY_RST);
    x.DmacMask = R32(x.Dma, DMAC_BASE + DMAC_INT_MASK);
    x.CsisMsk0 = R32(x.Csis, CSIS_INT_MSK0);
    x.CsisMsk1 = R32(x.Csis, CSIS_INT_MSK1);
    Log(r, "  found: ISPPRE_BUS div %08X, PMU iso %08X, sysreg +0x500 %08X\r\n", x.TopDiv, x.Iso, x.PhyRst);

    /* --- ISPPRE bus clock (CSIS2 and its DMA): 400 MHz / (div + 1) */
    if (in->IspDiv != 0x100) {
        /* 0.5 ran /4 = 100 MHz: CSIS2 overflowed (INT_SRC0 OVER) after ~28 lines. 2 pixels a clock need >= 361 MHz
           for 4 x 1807 Mbps RAW10 -> default /1 = 400 MHz (Android: 466). in->IspDiv = the ratio 1..16. */
        d = (in->IspDiv == 0) ? 0u : ((in->IspDiv - 1u) & 0xFu);
        W32(m->Top, TOP_ISPPRE_BUS_DIV, (x.TopDiv & ~0xFu) | d);
        x.DivSet = TRUE;
        for (w = 0; w < 100 && (R32(m->Top, TOP_ISPPRE_BUS_DIV) & CMU_TOP_DIV_BUSY); w++) KeStallExecutionProcessor(1);
        Log(r, "  ISPPRE_BUS div -> %08X (mux %u: %u MHz / %u)\r\n", R32(m->Top, TOP_ISPPRE_BUS_DIV),
            R32(m->Top, TOP_ISPPRE_BUS_MUX) & 3u, (R32(m->Top, TOP_ISPPRE_BUS_MUX) & 3u) == 3u ? 400u : 0u, d + 1);
    }

    /* --- sensor: power, identity, global + mode 8 */
    s = SensorPowerUp(m, f, r);
    if (!NT_SUCCESS(s))
        return s;                                        /* SensorPowerUp powered down itself */
    x.SensorOn = TRUE;
    s = SensorCheckId(m, r);
    if (!NT_SUCCESS(s))
        return s;
    Log(r, "== sensor setfile A (fimc-is-cis-3j1-setA.h)\r\n");
    s = SenTable(m, S10eCam3j1Global, RTL_NUMBER_OF(S10eCam3j1Global), "global", r);
    if (NT_SUCCESS(s))
        s = SenTable(m, x.Mode->Tab, x.Mode->TabN, x.Mode->Name, r);
    if (NT_SUCCESS(s) && x.Mode->Fll) {
        /* frame length (not streaming yet: no group hold needed). 0.10: S10ECAM_SHOT_FPS60 on mode 11 = the same
           sensor_3j1_cis_set_frame_duration formula for 16667 us: 807 x 16667 / 8984 = 1497 (>= the setfile's 746) */
        ULONG fll = (x.Mode == &gModes[1] && (in->Flags & S10ECAM_SHOT_FPS60)) ? 1497u : x.Mode->Fll;
        s = SenW16(m, 0x0340, fll);
        Log(r, "  frame length 0x0340 <- %u (%s)\r\n", fll, fll == 1497u ? "60 fps" : "30 fps");
    }
    if (NT_SUCCESS(s) && in->Coarse)
        s = SenW16(m, 0x0202, in->Coarse & 0xFFFFu);
    if (NT_SUCCESS(s) && in->AGain)
        s = SenW16(m, 0x0204, in->AGain & 0xFFFFu);
    if (NT_SUCCESS(s) && (in->Flags & S10ECAM_SHOT_PATTERN))
        s = SenW8(m, 0x0601, 0x02);                      /* SMIA test_pattern_mode 2 = colour bars */
    if (!NT_SUCCESS(s))
        return s;
    ow = SenR16(m, 0x034C);
    oh = SenR16(m, 0x034E);
    Log(r, "  read back: coarse 0x0202 %04X, again 0x0204 %04X, FLL 0x0340 %04X, LLP 0x0342 %04X, out %u x %u (0x034C/E), pattern 0x0600 %04X\r\n",
        SenR16(m, 0x0202), SenR16(m, 0x0204), SenR16(m, 0x0340), SenR16(m, 0x0342), ow, oh, SenR16(m, 0x0600));
    /* DMA0_RESOL bounds the width only: a sensor still in another mode (3648 x 2736) would write past the buffer. */
    if (ow != x.Mode->W || oh != x.Mode->H) {
        Log(r, "  refused: the sensor's output size is not %u x %u - no DMA\r\n", x.Mode->W, x.Mode->H);
        s = STATUS_DEVICE_CONFIGURATION_ERROR;
        return s;
    }
    if (phaseMax == 1) { Log(r, "  phase limit 1 (sensor set up): stopping\r\n"); return s; }

    /* --- D-PHY power_on (isolation bypass, reset released) + 0502_0002 config */
    Log(r, "== D-PHY\r\n");
    W32(x.Pmu, PMU_MIPI_ISO_CSIS2, x.Iso | 1u);
    x.PhyOn = TRUE;
    W32(x.Sys, ISPPRE_PHY_RST, R32(x.Sys, ISPPRE_PHY_RST) & ~ISPPRE_PHY_RST_CSIS2);
    W32(x.Sys, ISPPRE_PHY_RST, R32(x.Sys, ISPPRE_PHY_RST) | ISPPRE_PHY_RST_CSIS2);
    SleepUs(10);
    Log(r, "  PMU iso %08X, sysreg +0x500 %08X; PHY SC_GNR_CON0 %08X SD0_TIME_CON0 %08X (before)\r\n",
        R32(x.Pmu, PMU_MIPI_ISO_CSIS2), R32(x.Sys, ISPPRE_PHY_RST), R32(x.Phy, DPHY_CSIS2), R32(x.Phy, DPHY_CSIS2 + 0x130));
    DphyConfig(x.Phy + DPHY_CSIS2);
    Log(r, "  PHY SC %08X %08X %08X %08X %08X  SD0 %08X %08X %08X  SD3 TIME_CON0 %08X\r\n",
        R32(x.Phy, DPHY_CSIS2), R32(x.Phy, DPHY_CSIS2 + 4), R32(x.Phy, DPHY_CSIS2 + 0x10), R32(x.Phy, DPHY_CSIS2 + 0x14),
        R32(x.Phy, DPHY_CSIS2 + 0x30), R32(x.Phy, DPHY_CSIS2 + 0x100), R32(x.Phy, DPHY_CSIS2 + 0x110),
        R32(x.Phy, DPHY_CSIS2 + 0x130), R32(x.Phy, DPHY_CSIS2 + 0x430));
    if (phaseMax == 2) { Log(r, "  phase limit 2 (D-PHY set up): stopping\r\n"); return s; }

    /* --- CSIS2: reset, 4 lanes, VC_ONLY, the cfg's 4 VCs, interrupt sources visible */
    Log(r, "== CSIS2\r\n");
    x.CsisCmn = R32(x.Csis, CSIS_CMN_CTRL);
    W32(x.Csis, CSIS_CMN_CTRL, R32(x.Csis, CSIS_CMN_CTRL) | CSIS_CMN_SW_RESET);
    for (w = 0; w < 10 && (R32(x.Csis, CSIS_CMN_CTRL) & CSIS_CMN_SW_RESET); w++) KeStallExecutionProcessor(10);
    x.CsisOn = TRUE;
    if (R32(x.Csis, CSIS_CMN_CTRL) & CSIS_CMN_SW_RESET) {
        Log(r, "  SW_RESET did not clear (CMN_CTRL %08X)\r\n", R32(x.Csis, CSIS_CMN_CTRL));
        s = STATUS_DEVICE_NOT_READY;
        return s;
    }
    RmwField(x.Csis, CSIS_CMN_CTRL, CSIS_CMN_LANES_SHIFT, 2, 3);          /* 4 lanes */
    RmwField(x.Csis, CSIS_PHY_CMN_CTRL, CSIS_PHY_EN_DAT_SHIFT, 4, 0xF);
    RmwField(x.Csis, CSIS_CMN_CTRL, CSIS_CMN_ILV_SHIFT, 2, CSIS_MODE_VC_ONLY);
    W32(x.Csis, CSIS_DEBUG_OPTION, 1);                                     /* PIXEL_ALIGN_MODE */
    CsisConfigVc(x.Csis, 0, HW_FORMAT_RAW10, CSIS_PIXEL_DUAL, x.Mode->W, x.Mode->H);
    if (x.Mode->PdafLines)
        CsisConfigVc(x.Csis, 1, HW_FORMAT_RAW10, CSIS_PIXEL_QUAD, x.Mode->W, x.Mode->PdafLines);   /* PD_MOD3 PDAF */
    else
        CsisConfigVc(x.Csis, 1, HW_FORMAT_UNKNOWN, CSIS_PIXEL_DUAL, 0, 0);                         /* PD_NONE */
    CsisConfigVc(x.Csis, 2, HW_FORMAT_UNKNOWN, CSIS_PIXEL_DUAL, 0, 0);
    CsisConfigVc(x.Csis, 3, HW_FORMAT_UNKNOWN, CSIS_PIXEL_DUAL, 0, 0);
    W32(x.Csis, CSIS_INT_MSK0, CSIS_IRQ_MASK0_DBG);
    W32(x.Csis, CSIS_INT_MSK1, CSIS_IRQ_MASK1_DBG);
    W32(x.Csis, CSIS_INT_SRC0, 0xFFFFFFFFu);
    W32(x.Csis, CSIS_INT_SRC1, 0xFFFFFFFFu);
    if (phaseMax == 3) { Log(r, "  phase limit 3 (CSIS2 configured, not enabled): stopping\r\n"); return s; }

    /* --- DMA: common (csi_hw_s_dma_common_dynamic), VC0 legacy RAW10 2D, one address. The common block 0x170E0000
       was never read before this phase (stage 3 did not): its first read is here, after phase 3, so -Phase 3
       vs 4 tells it apart if it aborts like the SysMMU did. */
    x.CdmaSaved[0] = R32(x.Cdma, CDMA_ARB_PRI);
    x.CdmaSaved[1] = R32(x.Cdma, CDMA_CTRL);
    x.CdmaSaved[2] = R32(x.Cdma, CDMA_SRAM_SPLIT);
    x.CdmaSaved[3] = R32(x.Cdma, CDMA_MATRIX);
    Log(r, "== DMA (common found %08X %08X %08X %08X)\r\n", x.CdmaSaved[0], x.CdmaSaved[1], x.CdmaSaved[2],
        x.CdmaSaved[3]);
    W32(x.Cdma, CDMA_ARB_PRI, (x.CdmaSaved[0] & ~0xFu) | (1u << 2) | 2u);
    W32(x.Cdma, CDMA_CTRL, x.CdmaSaved[1] | (1u << 1));
    W32(x.Cdma, CDMA_SRAM_SPLIT, (x.CdmaSaved[2] & ~((0xFFFu << 16) | 0xFFFu)) | (0x480u << 16) | 0x480u);
    W32(x.Cdma, CDMA_MATRIX, x.CdmaSaved[3] & ~0x1Fu);
    x.DmaOn = TRUE;
    W32(x.Dma, DMA0_FMT, R32(x.Dma, DMA0_FMT) & ~0x3Fu);                   /* RAW10, NORMAL, legacy, 2D */
    W32(x.Dma, DMA0_RESOL, (R32(x.Dma, DMA0_RESOL) & ~DMA0_RESOL_MASK) | x.Mode->W);
    W32(x.Dma, DMA0_FCNTSEQ, 0);
    W32(x.Dma, DMAC_BASE + DMAC_OTF_FORMAT, R32(x.Dma, DMAC_BASE + DMAC_OTF_FORMAT) & ~0x3u);
    W32(x.Dma, DMAC_BASE + DMAC_DATA_CTRL, R32(x.Dma, DMAC_BASE + DMAC_DATA_CTRL) & ~0x1u);   /* input path OTF */
    W32(x.Dma, DMA0_CTRL, R32(x.Dma, DMA0_CTRL) & ~DMA0_UPDT_FRAMEPTR(0));
    if (nbuf == 1) {
        W32(x.Dma, DMA0_ADDR1, (ULONG)x.Pa.QuadPart);
        W32(x.Dma, DMA0_FCNTSEQ, R32(x.Dma, DMA0_FCNTSEQ) | 1u);
    } else {
        /* frame pointer n (0..31) -> ADDR(n + 1) = Ring[n % 3]; every pointer armed. The stream thread finds the
           finished buffer by its canary, not by the pointer (FCNTSEQ's exact semantics are not documented here). */
        for (i = 0; i < 32; i++)
            W32(x.Dma, DMA0_ADDR1 + 4 * i, (ULONG)x.RingPa[i % SHOT_RING].QuadPart);
        W32(x.Dma, DMA0_FCNTSEQ, 0xFFFFFFFFu);
    }
    W32(x.Dma, DMAC_BASE + DMAC_INT_MASK, CSIS_DMA_IRQ_MASK_DBG);
    W32(x.Dma, DMAC_BASE + DMAC_INT_SRC, 0xFFFFFFFFu);
    W32(x.Dma, DMA0_CTRL, R32(x.Dma, DMA0_CTRL) | DMA0_EN | DMA0_UPDT_PTR_EN);
    Log(r, "  DMA0 CTRL %08X FMT %08X ADDR1 %08X RESOL %08X FCNTSEQ %08X; common %08X %08X %08X %08X\r\n",
        R32(x.Dma, DMA0_CTRL), R32(x.Dma, DMA0_FMT), R32(x.Dma, DMA0_ADDR1), R32(x.Dma, DMA0_RESOL),
        R32(x.Dma, DMA0_FCNTSEQ), R32(x.Cdma, CDMA_ARB_PRI), R32(x.Cdma, CDMA_CTRL), R32(x.Cdma, CDMA_SRAM_SPLIT),
        R32(x.Cdma, CDMA_MATRIX));
    if (phaseMax == 4) { Log(r, "  phase limit 4 (DMA armed, CSIS2 not enabled, no stream): stopping\r\n"); return s; }

    /* --- csi_hw_enable */
    RmwField(x.Csis, CSIS_CMN_CTRL, CSIS_CMN_SHADOW_SHIFT, 4, 0xF);
    W32(x.Csis, CSIS_CMN_CTRL, R32(x.Csis, CSIS_CMN_CTRL) & ~CSIS_CMN_PHY_SEL);
    W32(x.Csis, CSIS_PHY_CMN_CTRL, R32(x.Csis, CSIS_PHY_CMN_CTRL) | CSIS_PHY_EN_CLK);
    W32(x.Csis, CSIS_CMN_CTRL, R32(x.Csis, CSIS_CMN_CTRL) & ~CSIS_CMN_QCH_EN);   /* ENABLE_HWACG_CONTROL: Q-channel off */
    W32(x.Csis, CSIS_CMN_CTRL, R32(x.Csis, CSIS_CMN_CTRL) | CSIS_CMN_CSI_EN);
    Log(r, "  CMN_CTRL %08X PHY_CMN_CTRL %08X CONFIG0 %08X RESOL0 %08X CONFIG1 %08X PHY_STATUS %08X\r\n",
        R32(x.Csis, CSIS_CMN_CTRL), R32(x.Csis, CSIS_PHY_CMN_CTRL), R32(x.Csis, CSIS_ISP_CONFIG_CH0),
        R32(x.Csis, CSIS_ISP_RESOL_CH0), R32(x.Csis, CSIS_ISP_CONFIG_CH0 + CSIS_CH_STRIDE), R32(x.Csis, CSIS_PHY_STATUS));

    /* --- sensor stream on (sensor_3j1_cis_stream_on) */
    Log(r, "== stream on\r\n");
    SleepUs(8000);
    s = SenW16(m, 0x0A70, 0x0001);
    if (NT_SUCCESS(s)) s = SenW16(m, 0x0A72, 0x0100);
    if (NT_SUCCESS(s)) s = SenW8(m, 0x0100, 0x01);
    if (NT_SUCCESS(s)) { x.Streaming = TRUE; s = SenW8(m, 0x021E, 0x00); }
    if (!NT_SUCCESS(s))
        Log(r, "  stream-on write failed 0x%08X\r\n", s);
    return s;
#undef x
}

static NTSTATUS Stage4(MAPS *m, FOUND *f, REPORT *r, const S10ECAM_SHOT_IN *in, PUCHAR Frame, ULONG FrameCap, PULONG FrameOut)
{
    SHOT     x;
    NTSTATUS s;
    ULONG    frames, fc = 0, fc0, src0 = 0, src1 = 0, dsrc = 0, t = 0, tFirst = 0, tLast = 0, v;
    const SHOT_MODE *md = (in->Flags & S10ECAM_SHOT_BIN) ? &gModes[1] : &gModes[0];

    *FrameOut = 0;
    frames = in->Frames ? in->Frames : 4;
    if (frames > 60) frames = 60;
    s = ShotStart(m, f, &x, in, 1, r);
    if (!NT_SUCCESS(s) || !x.Streaming)
        goto out;
    Log(r, "  waiting for %u frames (2 s at most)\r\n", frames);
    fc0 = R32(x.Csis, CSIS_FRM_CNT_CH0);
    for (t = 0; t < 2000; t++) {                          /* 1 ms steps */
        SleepUs(1000);
        v = R32(x.Csis, CSIS_INT_SRC0);
        if (v) { src0 |= v; W32(x.Csis, CSIS_INT_SRC0, v); }
        v = R32(x.Csis, CSIS_INT_SRC1);
        if (v) { src1 |= v; W32(x.Csis, CSIS_INT_SRC1, v); }
        v = R32(x.Dma, DMAC_BASE + DMAC_INT_SRC);
        if (v) { dsrc |= v; W32(x.Dma, DMAC_BASE + DMAC_INT_SRC, v); }
        v = R32(x.Csis, CSIS_FRM_CNT_CH0) - fc0;
        if (v != fc) {
            if (fc == 0) tFirst = t;
            tLast = t;
            fc = v;
        }
        if (fc >= frames)
            break;
    }
    Log(r, "  %u frames counted on CH0 in %u ms (first at %u ms, last at %u ms)\r\n", fc, t, tFirst, tLast);
    Log(r, "  INT_SRC0 seen %08X (FS %X FE %X errors %03X%s), INT_SRC1 %08X, DMA cmn INT_SRC %08X%s\r\n", src0,
        (src0 >> CSIS_FRAMESTART_SHIFT) & 0xFu, (src0 >> CSIS_FRAMEEND_SHIFT) & 0xFu, (src0 >> 8) & 0xFFFu,
        (src0 & CSIS_INT_OVER) ? ", OVERFLOW" : "", src1, dsrc, (dsrc & DMAC_INT_DMA_ERROR) ? " (DMA FIFO FULL)" : "");
    Log(r, "  PHY_STATUS %08X, DMA0 ACT_CTRL %08X FCNTSEQ_STAT %08X, cmn ERR_CODE %08X\r\n", R32(x.Csis, CSIS_PHY_STATUS),
        R32(x.Dma, DMA0_ACT_CTRL), R32(x.Dma, DMA0_FCNTSEQ_STAT), R32(x.Dma, DMAC_BASE + DMAC_ERR_CODE));
    if (fc == 0)
        s = STATUS_IO_TIMEOUT;

out:
    if (x.Pmu != NULL)                                   /* mapped = ShotStart got past its preconditions' maps */
        ShotTeardown(m, f, &x, r);
    if (x.Buf != NULL) {
        ShotAnalyse(&x, r);
        if (Frame != NULL && FrameCap >= md->Bytes) {
            RtlCopyMemory(Frame, x.Buf, md->Bytes);
            *FrameOut = md->Bytes;
        }
    }
    ShotUnmap(&x);
    Log(r, "FRAME %u x %u RAW10-in-16 stride %u bytes %u Bayer GRBG returned %u\r\n", md->W, md->H, md->Stride,
        md->Bytes, *FrameOut);
    return s;
}

/* ---------------------------------------------------------------------------------------------- streaming (0.6) */

typedef struct _STREAM {
    BOOLEAN        Active;
    PFILE_OBJECT   Owner;                    /* the handle that started it: its IRP_MJ_CLEANUP stops it */
    MAPS           m;
    FOUND          f;
    SHOT           x;
    PKTHREAD       Thread;
    KEVENT         Stop;
    volatile LONG  Seq;                      /* frames finished */
    volatile LONG  Latest;                   /* ring index of the newest finished frame, -1 = none */
    volatile LONG64 LatestTime;              /* KeQueryInterruptTime of it */
    ULONG          Tail[SHOT_RING][2];       /* the last 8 bytes of each finished frame (the canary's place) */
    ULONG          Next;                     /* the ring index expected to finish next */
    ULONG          Coarse, AGain, AGainMin, AGainMax, CoarseMax;
    volatile LONG  Rearms, Stalls;
    volatile LONG  Src0, DmaSrc;
    VOID         (*OnFrame)(PVOID Ctx);          /* called from the poll thread (PASSIVE) after each finished frame */
    PVOID          OnFrameCtx;
} STREAM;

static STREAM gS;

static ULONG volatile *Canary(ULONG i)
{
    return (ULONG volatile *)(gS.x.Ring[i] + gS.x.Mode->Bytes - 8);
}

/* Polls (1 ms timer resolution while it runs): interrupt sources (accumulated + cleared), finished frames by their
   canary (in ring order, so 2 finished between polls still come out in order), FCNTSEQ re-armed. */
static VOID StreamThread(PVOID Ctx)
{
    LARGE_INTEGER t;
    ULONG         v, k, i;
    LONG64        lastDone = (LONG64)KeQueryInterruptTime();

    UNREFERENCED_PARAMETER(Ctx);
    t.QuadPart = -20000;                                 /* 2 ms */
    while (KeWaitForSingleObject(&gS.Stop, Executive, KernelMode, FALSE, &t) == STATUS_TIMEOUT) {
        v = R32(gS.x.Csis, CSIS_INT_SRC0);
        if (v) { W32(gS.x.Csis, CSIS_INT_SRC0, v); InterlockedOr(&gS.Src0, (LONG)v); }
        v = R32(gS.x.Dma, DMAC_BASE + DMAC_INT_SRC);
        if (v) { W32(gS.x.Dma, DMAC_BASE + DMAC_INT_SRC, v); InterlockedOr(&gS.DmaSrc, (LONG)v); }
        /* scanned from the expected next one around the ring: with a 2 ms poll and 33 ms frames two finishing in
           one poll is not expected, and if the DMA did not start at Ring[0] the first find re-syncs Next */
        for (k = 0, i = gS.Next; k < SHOT_RING; k++, i = (i + 1) % SHOT_RING) {
            if (Canary(i)[0] == SHOT_CANARY && Canary(i)[1] == SHOT_CANARY)
                continue;
            gS.Tail[i][0] = Canary(i)[0];
            gS.Tail[i][1] = Canary(i)[1];
            Canary(i)[0] = SHOT_CANARY;
            Canary(i)[1] = SHOT_CANARY;
            InterlockedExchange64(&gS.LatestTime, (LONG64)KeQueryInterruptTime());
            InterlockedExchange(&gS.Latest, (LONG)i);
            InterlockedIncrement(&gS.Seq);
            gS.Next = (i + 1) % SHOT_RING;
            lastDone = (LONG64)KeQueryInterruptTime();
            if (gS.OnFrame != NULL)
                gS.OnFrame(gS.OnFrameCtx);
            if (R32(gS.x.Dma, DMA0_FCNTSEQ) != 0xFFFFFFFFu) {   /* Linux re-arms per buffer: so do we */
                W32(gS.x.Dma, DMA0_FCNTSEQ, 0xFFFFFFFFu);
                InterlockedIncrement(&gS.Rearms);
            }
        }
        if ((LONG64)KeQueryInterruptTime() - lastDone > 3000000) {   /* 300 ms (100 ns units) without a frame */
            InterlockedIncrement(&gS.Stalls);
            W32(gS.x.Dma, DMA0_FCNTSEQ, 0xFFFFFFFFu);
            lastDone = (LONG64)KeQueryInterruptTime();
        }
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static VOID StreamStop(REPORT *r)
{
    if (!gS.Active)
        return;
    Log(r, "== stream stop: %ld frames, %ld re-arms, %ld stalls\r\n", gS.Seq, gS.Rearms, gS.Stalls);
    if (gS.Thread != NULL) {
        KeSetEvent(&gS.Stop, IO_NO_INCREMENT, FALSE);
        (VOID)KeWaitForSingleObject(gS.Thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(gS.Thread);
        gS.Thread = NULL;
        (VOID)ExSetTimerResolution(0, FALSE);
    }
    ShotTeardown(&gS.m, &gS.f, &gS.x, r);
    ShotUnmap(&gS.x);
    UnmapAll(&gS.m);
    gS.Active = FALSE;
    gS.Owner = NULL;
}

static NTSTATUS StreamStart(const S10ECAM_SHOT_IN *in, PFILE_OBJECT Owner, VOID (*OnFrame)(PVOID), PVOID Ctx, REPORT *r)
{
    NTSTATUS s;
    HANDLE   h;
    ULONG    i;

    if (gS.Active) {
        Log(r, "already streaming\r\n");
        return STATUS_DEVICE_BUSY;
    }
    RtlZeroMemory(&gS, sizeof(gS));
    gS.Latest = -1;
    gS.OnFrame = OnFrame;
    gS.OnFrameCtx = Ctx;
    KeInitializeEvent(&gS.Stop, NotificationEvent, FALSE);
    s = Stage1(&gS.m, &gS.f, r);
    Log(r, "stage 1: 0x%08X\r\n", s);
    if (NT_SUCCESS(s))
        s = ShotStart(&gS.m, &gS.f, &gS.x, in, SHOT_RING, r);
    if (NT_SUCCESS(s) && !gS.x.Streaming)
        s = STATUS_CANCELLED;                            /* a phase limit stopped it: nothing to stream */
    gS.Active = TRUE;                                    /* from here StreamStop undoes whatever was done */
    if (!NT_SUCCESS(s)) {
        StreamStop(r);
        return s;
    }
    for (i = 0; i < SHOT_RING; i++) {
        Canary(i)[0] = SHOT_CANARY;
        Canary(i)[1] = SHOT_CANARY;
    }
    gS.AGainMin = SenR16(&gS.m, 0x0084);
    gS.AGainMax = SenR16(&gS.m, 0x0086);
    gS.Coarse = SenR16(&gS.m, 0x0202);
    gS.AGain = SenR16(&gS.m, 0x0204);
    gS.CoarseMax = SenR16(&gS.m, 0x0340);                /* FLL - SENSOR_3J1_COARSE_INTEGRATION_TIME_MAX_MARGIN 4 */
    gS.CoarseMax = (gS.CoarseMax == 0xFFFFFFFFu || gS.CoarseMax < 8) ? 1427u : gS.CoarseMax - 4;
    Log(r, "  analog gain code range 0x0084 %04X .. 0x0086 %04X; coarse %u (max %u), again %04X\r\n", gS.AGainMin,
        gS.AGainMax, gS.Coarse, gS.CoarseMax, gS.AGain);
    (VOID)ExSetTimerResolution(10000, TRUE);             /* 1 ms ticks for the poll */
    s = PsCreateSystemThread(&h, THREAD_ALL_ACCESS, NULL, NULL, NULL, StreamThread, NULL);
    if (NT_SUCCESS(s)) {
        s = ObReferenceObjectByHandle(h, THREAD_ALL_ACCESS, *PsThreadType, KernelMode, (PVOID *)&gS.Thread, NULL);
        ZwClose(h);
    }
    if (!NT_SUCCESS(s)) {
        Log(r, "  poll thread failed 0x%08X\r\n", s);
        (VOID)ExSetTimerResolution(0, FALSE);
        StreamStop(r);
        return s;
    }
    gS.Owner = Owner;
    Log(r, "streaming: ring of %u, frames finish into it until STREAM_STOP or this handle closes\r\n", SHOT_RING);
    return STATUS_SUCCESS;
}

static NTSTATUS StreamExposure(const S10ECAM_EXPOSURE_IN *e)
{
    ULONG    c = e->Coarse, g = e->AGain;
    NTSTATUS s;

    if (!gS.Active)
        return STATUS_DEVICE_NOT_READY;
    if (c < 2) c = 2;
    if (c > gS.CoarseMax) c = gS.CoarseMax;
    if (gS.AGainMin != 0xFFFFFFFFu && g < gS.AGainMin) g = gS.AGainMin;
    if (gS.AGainMax != 0xFFFFFFFFu && g > gS.AGainMax) g = gS.AGainMax;
    s = SenW8(&gS.m, 0x0104, 0x01);                      /* grouped parameter hold: both apply on one frame */
    if (NT_SUCCESS(s)) s = SenW16(&gS.m, 0x0202, c);
    if (NT_SUCCESS(s)) s = SenW16(&gS.m, 0x0204, g);
    (VOID)SenW8(&gS.m, 0x0104, 0x00);
    if (NT_SUCCESS(s)) { gS.Coarse = c; gS.AGain = g; }
    return s;
}

/* The newest finished frame (copied out; its last 8 bytes from Tail). Returns the bytes written to Out. */
static ULONG StreamFrame(PUCHAR Out, ULONG OutLen)
{
    S10ECAM_FRAME_HDR *h = (S10ECAM_FRAME_HDR *)Out;
    LONG               i, seq;
    LARGE_INTEGER      t0, t1, fq;
    ULONG              n;

    RtlZeroMemory(h, sizeof(*h));
    h->Magic = S10ECAM_FRAME_MAGIC;
    h->Coarse = gS.Coarse; h->AGain = gS.AGain; h->AGainMin = gS.AGainMin; h->AGainMax = gS.AGainMax;
    h->CoarseMax = gS.CoarseMax;
    h->Rearms = (ULONG)gS.Rearms; h->Stalls = (ULONG)gS.Stalls;
    h->IntSrc0 = (ULONG)InterlockedExchange(&gS.Src0, 0);
    h->DmaIntSrc = (ULONG)InterlockedExchange(&gS.DmaSrc, 0);
    i = gS.Latest;
    seq = gS.Seq;
    if (!gS.Active || i < 0)
        return sizeof(*h);
    n = gS.x.Mode->Bytes;
    h->WxH = gS.x.Mode->W | (gS.x.Mode->H << 16);
    if (OutLen < sizeof(*h) + n)
        return sizeof(*h);
    h->Seq = (ULONG)seq;
    h->Ring = (ULONG)i;
    h->AgeMs = (ULONG)(((LONG64)KeQueryInterruptTime() - gS.LatestTime) / 10000);
    t0 = KeQueryPerformanceCounter(&fq);
    RtlCopyMemory(Out + sizeof(*h), gS.x.Ring[i], n - 8);
    RtlCopyMemory(Out + sizeof(*h) + n - 8, gS.Tail[i], 8);
    t1 = KeQueryPerformanceCounter(NULL);
    h->CopyUs = (ULONG)((t1.QuadPart - t0.QuadPart) * 1000000 / fq.QuadPart);
    h->Torn = (gS.Seq - seq >= 2) ? 1u : 0u;             /* the copied buffer started over meanwhile */
    return sizeof(*h) + n;
}

#ifndef S10ECAM_ENGINE_ONLY   /* S10eCamera (AVStream) includes this file for the engine and brings its own entry */
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
    switch (sp->Parameters.DeviceIoControl.IoControlCode) {
    case IOCTL_S10ECAM_STREAM_START:
    case IOCTL_S10ECAM_STREAM_STOP: {
        S10ECAM_SHOT_IN si = { 0 };                     /* (STOP has no input; MSVC C4701 otherwise) */
        BOOLEAN         start = sp->Parameters.DeviceIoControl.IoControlCode == IOCTL_S10ECAM_STREAM_START;

        if ((start && sp->Parameters.DeviceIoControl.InputBufferLength < sizeof(si)) ||
            sp->Parameters.DeviceIoControl.OutputBufferLength < 1024) {
            s = STATUS_INVALID_PARAMETER;
            goto done;
        }
        if (start)
            si = *(S10ECAM_SHOT_IN *)Irp->AssociatedIrp.SystemBuffer;
        r.Buf = (PCHAR)Irp->AssociatedIrp.SystemBuffer;
        r.Size = sp->Parameters.DeviceIoControl.OutputBufferLength;
        if (r.Size > S10ECAM_REPORT_MAX)
            r.Size = S10ECAM_REPORT_MAX;
        r.Used = 0;
        r.Buf[0] = 0;
        (VOID)KeWaitForSingleObject(&gLock, Executive, KernelMode, FALSE, NULL);
        if (start) {
            Log(&r, "S10eCam 0.11 stream start flags %08X coarse %u again %u ispdiv %X\r\n", si.Flags, si.Coarse, si.AGain,
                si.IspDiv);
            s = StreamStart(&si, sp->FileObject, NULL, NULL, &r);
            Log(&r, "stream start: 0x%08X\r\n", s);
        } else {
            if (!gS.Active)
                Log(&r, "not streaming\r\n");
            StreamStop(&r);
        }
        KeSetEvent(&gLock, IO_NO_INCREMENT, FALSE);
        Irp->IoStatus.Information = r.Used + 1;
        s = STATUS_SUCCESS;
        goto done;
    }
    case IOCTL_S10ECAM_STREAM_EXPOSURE:
        if (sp->Parameters.DeviceIoControl.InputBufferLength < sizeof(S10ECAM_EXPOSURE_IN)) {
            s = STATUS_INVALID_PARAMETER;
            goto done;
        }
        (VOID)KeWaitForSingleObject(&gLock, Executive, KernelMode, FALSE, NULL);
        s = StreamExposure((S10ECAM_EXPOSURE_IN *)Irp->AssociatedIrp.SystemBuffer);
        KeSetEvent(&gLock, IO_NO_INCREMENT, FALSE);
        goto done;
    case IOCTL_S10ECAM_STREAM_FRAME: {
        PUCHAR out;

        if (Irp->MdlAddress == NULL || sp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(S10ECAM_FRAME_HDR)) {
            s = STATUS_INVALID_PARAMETER;
            goto done;
        }
        out = (PUCHAR)MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority | MdlMappingNoExecute);
        if (out == NULL) {
            s = STATUS_INSUFFICIENT_RESOURCES;
            goto done;
        }
        (VOID)KeWaitForSingleObject(&gLock, Executive, KernelMode, FALSE, NULL);
        Irp->IoStatus.Information = StreamFrame(out, sp->Parameters.DeviceIoControl.OutputBufferLength);
        KeSetEvent(&gLock, IO_NO_INCREMENT, FALSE);
        s = STATUS_SUCCESS;
        goto done;
    }
    default:
        break;
    }
    if (gS.Active) {                                     /* stages 1-4 would re-program what the stream is using */
        s = STATUS_DEVICE_BUSY;
        goto done;
    }
    if (sp->Parameters.DeviceIoControl.IoControlCode == IOCTL_S10ECAM_SHOT) {
        S10ECAM_SHOT_IN shot;
        PUCHAR          out;
        ULONG           outLen = sp->Parameters.DeviceIoControl.OutputBufferLength, got = 0;

        if (sp->Parameters.DeviceIoControl.InputBufferLength < sizeof(shot) || outLen < S10ECAM_SHOT_HDR ||
            Irp->MdlAddress == NULL) {
            s = STATUS_INVALID_PARAMETER;
            goto done;
        }
        out = (PUCHAR)MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority | MdlMappingNoExecute);
        if (out == NULL) {
            s = STATUS_INSUFFICIENT_RESOURCES;
            goto done;
        }
        shot = *(S10ECAM_SHOT_IN *)Irp->AssociatedIrp.SystemBuffer;
        r.Buf = (PCHAR)out;
        r.Size = S10ECAM_SHOT_HDR;
        r.Used = 0;
        r.Buf[0] = 0;
        RtlZeroMemory(&m, sizeof(m));
        (VOID)KeWaitForSingleObject(&gLock, Executive, KernelMode, FALSE, NULL);
        Log(&r, "S10eCam 0.11 stage 4 (shot) flags %08X coarse %u again %u ispdiv %X frames %u\r\n", shot.Flags,
            shot.Coarse, shot.AGain, shot.IspDiv, shot.Frames);
        s = Stage1(&m, &f, &r);
        Log(&r, "stage 1: 0x%08X\r\n", s);
        if (NT_SUCCESS(s)) {
            s = Stage4(&m, &f, &r, &shot, out + S10ECAM_SHOT_HDR, outLen - S10ECAM_SHOT_HDR, &got);
            Log(&r, "stage 4: 0x%08X\r\n", s);
        }
        UnmapAll(&m);
        KeSetEvent(&gLock, IO_NO_INCREMENT, FALSE);
        Irp->IoStatus.Information = S10ECAM_SHOT_HDR + (ULONG_PTR)got;
        s = STATUS_SUCCESS;                              /* the outcome is in the report */
        goto done;
    }
    if (sp->Parameters.DeviceIoControl.IoControlCode != IOCTL_S10ECAM_STAGE ||
        sp->Parameters.DeviceIoControl.InputBufferLength < sizeof(in) ||
        sp->Parameters.DeviceIoControl.OutputBufferLength < 1024) {
        s = STATUS_INVALID_DEVICE_REQUEST;
        goto done;
    }
    in = *(S10ECAM_STAGE_IN *)Irp->AssociatedIrp.SystemBuffer;
    if (in.Stage < 1 || in.Stage > 3) {
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
    Log(&r, "S10eCam 0.11 stage %u flags %08X\r\n", in.Stage, in.Flags);
    if (in.Stage == 3) {                                 /* independent of 1/2: touches neither PERIC1 nor the PMIC */
        s = Stage3(&r, in.Flags);
        Log(&r, "stage 3: 0x%08X\r\n", s);
    } else {
        s = Stage1(&m, &f, &r);
        Log(&r, "stage 1: 0x%08X\r\n", s);
        if (in.Stage == 2 && NT_SUCCESS(s)) {
            s = Stage2(&m, &f, &r);
            Log(&r, "stage 2: 0x%08X\r\n", s);
        }
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
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);

    UNREFERENCED_PARAMETER(DeviceObject);
    if (sp->MajorFunction == IRP_MJ_CLEANUP) {           /* the handle that started the stream is going away */
        CHAR   buf[1024];
        REPORT r = { buf, sizeof(buf), 0 };

        (VOID)KeWaitForSingleObject(&gLock, Executive, KernelMode, FALSE, NULL);
        if (gS.Active && gS.Owner == sp->FileObject)
            StreamStop(&r);
        KeSetEvent(&gLock, IO_NO_INCREMENT, FALSE);
    }
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static VOID S10eCamUnload(PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING dos;

    UNREFERENCED_PARAMETER(DriverObject);
    if (gS.Active) {                                     /* (CLEANUP normally stopped it already) */
        CHAR   buf[1024];
        REPORT r = { buf, sizeof(buf), 0 };
        StreamStop(&r);
    }
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
    DriverObject->MajorFunction[IRP_MJ_CLEANUP] = S10eCamCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = S10eCamIoctl;
    gDevice->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

#endif /* S10ECAM_ENGINE_ONLY */
