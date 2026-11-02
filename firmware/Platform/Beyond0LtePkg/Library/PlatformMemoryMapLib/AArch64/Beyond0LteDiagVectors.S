//
// Beyond0LteDiagVectors.S - boot-bring-up exception catcher (notes/m8-uefi-image.md B51).
//
// Boots #4-#12 ended every fault in a SOFT reset: with no EL1 vectors of our own the
// exception goes to whatever sboot left in VBAR_EL1. This installs a vector table in the
// SEC image (XIP in the FD, mapped as DRAM before and after MMU-on) whose every entry
// paints the fault registers as 32-cell binary strips on the panel and then parks the CPU,
// so the phone FREEZES on the evidence instead of rebooting.
//
// Panel geometry: per board from Exynos9820Board.h (S10e 1080 x 2280, line 4320 B; S10
// 1440 x 3040, line 5760 B), FB 0xCA000000 on both. The rows below fit either panel. Rows:
//   1900  ESR_EL1           (EC = bits 31:26)
//   1990  ELR_EL1 [31:0]    (faulting PC)
//   2080  FAR_EL1 [31:0]    (faulting data address, if any)
//   2170  vector index << 24 | SPSR_EL1 [15:0]
// Strips: 32 cells x 33 px (28 painted), MSB left; 1 = 80-row block, 0 = 4-row underline.
//
// Stays active until DXE's CpuDxe installs its own table (whose handler dead-loops
// instead of resetting, so a reset after that point is not an EL1 exception).
// Remove once the boot path is understood (BEYOND0LTE_DIAG_VECTORS in ArmPlatformLib.c).
//

#include <AsmMacroLib.h>
#include <Platform/Exynos9820Board.h>   // panel line length per board (plain integers, asm-safe)

#define FB_ROW(r)  (0xCA000000 + (r) * EXYNOS_BOARD_FB_LINE_BYTES)   // S10e 4320 B, S10 5760 B

  .macro LOADFB reg, row
  movz  \reg, #((FB_ROW(\row)) & 0xFFFF)
  movk  \reg, #((FB_ROW(\row)) >> 16), lsl #16
  .endm

  .macro VECTOR idx
  .balign 0x80
  mov   x19, #\idx
  b     DiagCommon
  .endm

  .text

ASM_FUNC(Beyond0LteInstallDiagVectors)
  adr   x0, DiagVectors
  msr   vbar_el1, x0
  isb
  ret

  .balign 0x800
DiagVectors:
  VECTOR 0
  VECTOR 1
  VECTOR 2
  VECTOR 3
  VECTOR 4
  VECTOR 5
  VECTOR 6
  VECTOR 7
  VECTOR 8
  VECTOR 9
  VECTOR 10
  VECTOR 11
  VECTOR 12
  VECTOR 13
  VECTOR 14
  VECTOR 15

DiagCommon:
  mrs   x20, esr_el1
  mrs   x21, elr_el1
  mrs   x22, far_el1
  mrs   x23, spsr_el1
  LOADFB x9, 1900
  mov   w12, w20
  bl    DiagBits
  LOADFB x9, 1990
  mov   w12, w21
  bl    DiagBits
  LOADFB x9, 2080
  mov   w12, w22
  bl    DiagBits
  and   w12, w23, #0xFFFF
  orr   w12, w12, w19, lsl #24
  LOADFB x9, 2170
  bl    DiagBits
1:
  wfe
  b     1b

// x9 = FB address of the strip's top-left, w12 = value. Leaf, no stack.
DiagBits:
  mov   w13, #31
  mov   w8, #-1
2:
  lsr   w14, w12, w13
  mov   w15, #31
  sub   w15, w15, w13
  mov   w16, #132
  umaddl x17, w15, w16, x9
  tbnz  w14, #0, 3f
  movz  x16, #((76 * EXYNOS_BOARD_FB_LINE_BYTES) & 0xFFFF)
  movk  x16, #((76 * EXYNOS_BOARD_FB_LINE_BYTES) >> 16), lsl #16   // 76 lines (S10e 0x50280, S10 0x6AE00)
  add   x17, x17, x16
  mov   w10, #4
  b     4f
3:
  mov   w10, #80
4:
  mov   w11, #28
  mov   x7, x17
5:
  str   w8, [x7], #4
  subs  w11, w11, #1
  b.ne  5b
  mov   x16, #EXYNOS_BOARD_FB_LINE_BYTES
  add   x17, x17, x16
  subs  w10, w10, #1
  b.ne  4b
  subs  w13, w13, #1
  b.ge  2b
  dsb   sy
  ret
