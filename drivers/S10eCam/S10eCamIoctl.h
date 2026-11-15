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
 *          frame count read, and powered down again (always, also after an error). */
#define IOCTL_S10ECAM_STAGE  CTL_CODE(S10ECAM_TYPE, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _S10ECAM_STAGE_IN {
    unsigned int Stage;     /* 1 or 2 */
    unsigned int Flags;     /* 0 */
} S10ECAM_STAGE_IN;

#define S10ECAM_REPORT_MAX  16384u
