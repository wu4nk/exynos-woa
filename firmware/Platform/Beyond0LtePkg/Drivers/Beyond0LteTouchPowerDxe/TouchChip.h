/** @file
  TouchChip.h - S10 (beyond1lte) touch controller bring-up in UEFI (TouchChip.c).
**/

#ifndef TOUCH_CHIP_H_
#define TOUCH_CHIP_H_

//
// 1 on the S10 build: after the rails are on, start the sec_ts y771 over HSI2C and run the boot touch test.
// The S10e build keeps its previous behaviour (rails only).
//
#ifndef BEYOND1LTE_TOUCH_CHIP_INIT
#if EXYNOS9820_BOARD_BEYOND1
#define BEYOND1LTE_TOUCH_CHIP_INIT  1
#else
#define BEYOND1LTE_TOUCH_CHIP_INIT  0
#endif
#endif

VOID
TouchChipBringUp (
  IN BOOLEAN  RailsJustEnabled
  );

#endif
