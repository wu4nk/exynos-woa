/*
 * S10eCamIoctl.h - user/kernel interface of S10eCam (notes/s10-camera.md). Shared by the driver and cam-s10.ps1.
 * SPDX-License-Identifier: GPL-2.0
 */
#pragma once

#define S10ECAM_DEVICE_NAME  L"\\Device\\S10eCam"
#define S10ECAM_DOS_NAME     L"\\DosDevices\\S10eCam"
#define S10ECAM_USER_PATH    L"\\\\.\\S10eCam"

#define S10ECAM_TYPE  0x8A54u
/* IOCTL_S10ECAM_STAGE: in S10ECAM_STAGE_IN, out = an ASCII report (what was read, written and seen, step by step).
 * Stage 1: read the clocks, pins, both I2C controllers and the S2MPB02's camera rails (the camera stays unpowered;
 *          the S2MPB02's bus is set up: USI10 mode, its two pins, controller reset/timing).
 * Stage 2: stage 1, then the front camera (S5K3J1) powered up exactly as Android does, its model ID / revision /
 *          frame count read, and powered down again (always, also after an error).
 * Stage 3: read-only survey of the CSI receive path (power domains pd-dpu / pd-isppre in the PMU, CMU_TOP's ISPPRE bus
 *          clock); with S10ECAM_FLAG_DEEP ONE step inside ISPPRE (1 CMU mux/div, 2 gates, 3-4 Q-channels, 5 CSIS2,
 *          6 CSIS DMA, 7 sysreg; the SysMMU is never read; S10eCam.c). Writes nothing; does not run stage 1/2. */
#define IOCTL_S10ECAM_STAGE  CTL_CODE(S10ECAM_TYPE, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _S10ECAM_STAGE_IN {
    unsigned int Stage;     /* 1, 2 or 3 */
    unsigned int Flags;     /* S10ECAM_FLAG_* (stage 3 only), else 0 */
} S10ECAM_STAGE_IN;

#define S10ECAM_FLAG_DEEP   0x1u   /* stage 3: read inside ISPPRE - one step per call, the step in bits 15:8 */
#define S10ECAM_STEP(f)     (((f) >> 8) & 0xFFu)
#define S10ECAM_STEP_MAX    7u   /* 8 (SysMMU) removed: it aborts (2026-11-16) */

#define S10ECAM_REPORT_MAX  16384u

/* IOCTL_S10ECAM_SHOT (stage 4 = "3b"): the front camera streams mode 8 (1824 x 1368 RAW10 @30, 4 lanes x 1807 Mbps)
 * through D-PHY -> CSIS2 -> VC0 DMA into a buffer below 4 GB; after S10ECAM_SHOT_IN.Frames frames everything is
 * stopped and powered down again, and the buffer comes back. METHOD_OUT_DIRECT: output = S10ECAM_SHOT_HDR bytes of
 * ASCII report (NUL-terminated), then the frame (S10ECAM_SHOT_BYTES: 16 bits per pixel, the 10-bit value in the low
 * bits as the DMA stores RAW10 with PACK12 off, S10ECAM_SHOT_STRIDE bytes a line) when the buffer is big enough. */
#define IOCTL_S10ECAM_SHOT   CTL_CODE(S10ECAM_TYPE, 0x802, METHOD_OUT_DIRECT, FILE_ANY_ACCESS)

typedef struct _S10ECAM_SHOT_IN {
    unsigned int Flags;     /* S10ECAM_SHOT_* */
    unsigned int Coarse;    /* 0 = the setfile's; else 0x0202 coarse integration (lines) */
    unsigned int AGain;     /* 0 = the setfile's; else 0x0204 analog gain (0x20 = x1) */
    unsigned int IspDiv;    /* CMU_TOP CLKCMU_ISPPRE_BUS divider field: 0 = default 3 (/4 = 100 MHz), 1..15, 0x100 = leave */
    unsigned int Frames;    /* frames to wait for before stopping: 0 = 4 */
} S10ECAM_SHOT_IN;

#define S10ECAM_SHOT_PATTERN  0x1u   /* sensor colour-bar test pattern (0x0601 = 2) instead of the scene */
#define S10ECAM_SHOT_PHASE(f) (((f) >> 8) & 0xFu)   /* 0 = everything; 1 sensor, 2 + D-PHY, 3 + CSIS2, 4 + DMA: then stop */
#define S10ECAM_SHOT_W        1824u
#define S10ECAM_SHOT_H        1368u
#define S10ECAM_SHOT_STRIDE   (S10ECAM_SHOT_W * 2u)
#define S10ECAM_SHOT_BYTES    (S10ECAM_SHOT_STRIDE * S10ECAM_SHOT_H)
#define S10ECAM_SHOT_HDR      16384u

