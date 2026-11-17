// tramp.S - REFERENCE SOURCE of the machine code isphost.c emits at run time (notes/s10-isp.md, Stage 4 option A).
// Not linked: isphost.c carries the same instruction words (check_tramp.sh reassembles this file and compares), so the
// host needs no assembler syntax of its own on either platform (GNU as on Linux, armasm64 / clang-cl on Windows).
//
// Samsung's DDK / RTA use x18 as an ordinary register; Windows ARM64 user mode keeps the TEB in x18. So:
//  - isp_enter: host -> blob. Saves the host x18 in its frame, calls fn with x0..x7 from args[8], restores x18.
//  - isp_tramp (one per host function handed to the blob): loads the host target into x16, its context word into x15
//    and branches to isp_common.
//  - isp_common: blob -> host. Saves the blob's x18, finds the calling thread's entry in the thread table by SP
//    (stack_lo <= sp < stack_hi), loads that thread's host x18, stores x15 in entry.ctx, calls the target with x0..x7
//    untouched, restores the blob's x18. Uses only x9..x17 (caller-saved in AAPCS64 and in the Windows ARM64 ABI).
//    Stack-passed arguments would be off by the 32-byte frame: every interface used has <= 8 integer arguments.
// thread table: u32 count, u32 pad, then entries of {u64 stack_lo, u64 stack_hi, u64 x18, u64 ctx}.
        .text
        .globl isp_enter
isp_enter:                              // u64 isp_enter(u64 fn, const u64 *args)
        stp     x29, x30, [sp, #-32]!
        mov     x29, sp
        str     x18, [sp, #16]
        mov     x16, x0
        mov     x17, x1
        ldp     x0, x1, [x17]
        ldp     x2, x3, [x17, #16]
        ldp     x4, x5, [x17, #32]
        ldp     x6, x7, [x17, #48]
        blr     x16
        ldr     x18, [sp, #16]
        ldp     x29, x30, [sp], #32
        ret

        .globl isp_tramp
isp_tramp:
        ldr     x16, 1f
        ldr     x15, 2f
        ldr     x17, 3f
        br      x17
1:      .quad   0x1111111111111111      // host target
2:      .quad   0x2222222222222222      // context word
3:      .quad   0x3333333333333333      // isp_common
        .quad   0                       // pad to 48 bytes

        .globl isp_common
isp_common:
        stp     x29, x30, [sp, #-32]!
        mov     x29, sp
        str     x18, [sp, #16]
        ldr     x9, 9f                  // thread table
        ldr     w10, [x9]
        add     x11, x9, #8
        mov     x12, sp
4:      cbz     w10, 6f
        ldp     x13, x14, [x11]
        cmp     x12, x13
        b.lo    5f
        cmp     x12, x14
        b.hs    5f
        ldr     x18, [x11, #16]
        str     x15, [x11, #24]
        b       7f
5:      add     x11, x11, #32
        sub     w10, w10, #1
        b       4b
6:      brk     #0xf00d                 // a thread the host never registered called out of the blob
7:      blr     x16
        ldr     x18, [sp, #16]
        ldp     x29, x30, [sp], #32
        ret
        nop
9:      .quad   0x4444444444444444      // thread table address
