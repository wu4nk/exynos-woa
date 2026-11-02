/** @file
  Exynos9820Board.h — the per-BOARD values of the Exynos 9820 family.

  The SoC map (Exynos9820.h) is shared by the Galaxy S10e (`beyond0lte`, SM-G970F)
  and the Galaxy S10 (`beyond1lte`, SM-G973F): same SoC, same MMIO, same UFS
  controller, same touch bus, same PMIC, same audio parts. What differs is the
  board around it, and only those values live here, selected by one switch:

    EXYNOS9820_BOARD_BEYOND1 = 0   Galaxy S10e  (default — the S10e build is unchanged)
    EXYNOS9820_BOARD_BEYOND1 = 1   Galaxy S10   (set by Platform/Beyond1LtePkg/Beyond1LtePkg.dsc)

  This header is included by C AND by .S files (the bootshim and the diag vectors),
  so it holds plain integers only: no ULL suffixes, no casts, no typedefs.

  Every S10 value is a read from the user's own SM-G973F (bootloader
  G973FXXSGHWC2), collected 2026-11-02 by s10-collect.sh v0.1 as root in Android
  (notes/beyond1-port.md §1 holds the raw reads and how each was taken).
**/

#ifndef EXYNOS9820_BOARD_H_
#define EXYNOS9820_BOARD_H_

#ifndef EXYNOS9820_BOARD_BEYOND1
#define EXYNOS9820_BOARD_BEYOND1  0
#endif

#if EXYNOS9820_BOARD_BEYOND1

//
// Galaxy S10 (beyond1lte). Panel: /sys/class/lcd/panel/lcd_type = SDC_A10013,
// dumpsys display "real 1440 x 3040", /sys/class/graphics/fb0 stride = 5760 B at
// 32 bpp. The bootloader scanout base is the S10e's (s3cfb.bootloaderfb=0xca000000
// in the S10's own bootargs), so only the geometry moves.
//
#define EXYNOS_BOARD_FB_WIDTH            1440      // VERIFIED (dumpsys display real 1440 x 3040; fb0 stride 5760 = 1440 x 4)
#define EXYNOS_BOARD_FB_HEIGHT           3040      // VERIFIED (same reads)

//
// RAM: the S10 is the 8 GB board. Its live DTB (sboot-patched /sys/firmware/fdt)
// carries the S10e's five memory nodes with identical reg values PLUS
//   memory@980000000  reg = <0x9 0x80000000 0x80000000>
// and el2_earlymem moves to the top of that new bank:
//   reserved-memory/el2_earlymem reg = <0x9 0xFD800000 0x2800000>
// (S10e: 0x97E000000 + 0x2000000, the top of ITS last bank).
//
#define EXYNOS_BOARD_HAS_DRAM_BANK5      1
#define EXYNOS_BOARD_EL2_EARLY_BASE_HI   0x9       // 0x9FD800000, split for asm-safe use
#define EXYNOS_BOARD_EL2_EARLY_BASE_LO   0xFD800000
#define EXYNOS_BOARD_EL2_EARLY_SIZE      0x2800000 // VERIFIED (live reserved-memory/el2_earlymem reg)

#else

//
// Galaxy S10e (beyond0lte) — the values Exynos9820.h carried before the split,
// unchanged; their provenance stays documented there.
//
#define EXYNOS_BOARD_FB_WIDTH            1080
#define EXYNOS_BOARD_FB_HEIGHT           2280
#define EXYNOS_BOARD_HAS_DRAM_BANK5      0
#define EXYNOS_BOARD_EL2_EARLY_BASE_HI   0x9       // 0x97E000000
#define EXYNOS_BOARD_EL2_EARLY_BASE_LO   0x7E000000
#define EXYNOS_BOARD_EL2_EARLY_SIZE      0x2000000

#endif

#define EXYNOS_BOARD_FB_LINE_BYTES       (EXYNOS_BOARD_FB_WIDTH * 4)

#endif // EXYNOS9820_BOARD_H_
