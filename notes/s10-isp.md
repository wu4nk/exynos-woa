# S10 hardware ISP under Windows: reverse-engineering plan and log

Goal: run the Exynos 9820 camera ISP chain (3AA / ISP / MCSC ...) under Windows the way Android does, by loading
Samsung's own ISP libraries (from the user's phone, never redistributed) behind an OS-services table we implement.
Decided 2026-11-17 (user: "dịch ngược dần" = reverse it step by step). Companion of notes/s10-camera.md (the CPU
pipeline that works today stays the fallback).

Ground rules for this file (AGENTS.md): every finding written here as soon as it is known; constants only from a
source (GPL file:line, a dump, a disassembly offset) or marked UNVERIFIED; nothing runs on the phone before a
sandbox run says what it will touch.

## Stage 0 - the blob boundary as the GPL kernel defines it (2026-11-17, from k9820-full, LineageOS
## android_kernel_samsung_exynos9820, drivers/media/platform/exynos/fimc-is2)

### Files
- vendor/mcd/fimc-is-sec-define.h:101-102: FIMC_IS_DDK "fimc_is_lib.bin" (ISP "DDK", VRA included: USE_ONE_BINARY
  path of fimc-is-binary.h:42-47), FIMC_IS_RTA "fimc_is_rta.bin" (3A); front setfile "setfile_3j1.bin" (:111).
- include/fimc-is-binary.h:18-20: FIMC_IS_FW_PATH "/system/vendor/firmware/", dump/setfile path "/data/vendor/camera/".

### Where they run (include/fimc-is-binary.h:69-110)
- A FIXED kernel virtual address: LIB_OFFSET = VMALLOC_START + 0xF6000000 - 0x8000000; __LIB_START = LIB_OFFSET +
  0x04000000 - CDH_SIZE (128 KiB); VRA_LIB_ADDR = LIB_START + CDH; DDK_LIB_ADDR = LIB_START + VRA_LIB_SIZE (768 KiB)
  + CDH; DDK_LIB_SIZE = 3.25 MiB + 1 MiB; RTA_LIB_ADDR follows, RTA_LIB_SIZE 4 MiB; HEAP_START = LIB_START + 16 MiB;
  HEAP_RTA_START = HEAP_START + 64 MiB. Backed by memblock_alloc(LIB_SIZE, 2 MiB) (fimc-is-resourcemgr.c:790-830).
- Loader (interface/fimc-is-interface-library.c:2240-2385, fimc_is_load_ddk_bin): request_binary -> mark the area
  NX+RW -> memcpy(lib_addr, bin.data, bin.size) (flat image, no relocation step) -> flush D-cache -> mark the first
  LIB_ISP_CODE_SIZE (0x340000, interface-library.h:34) bytes RO+X -> set_os_system_funcs(table) ->
  fpsimd_get(); ((start_up_func_t)DDK_LIB_ADDR)(table); fpsimd_put(). Entry = the image's FIRST instruction;
  start_up_func_t is u32 (*)(void **func). RTA: ((rta_start_up_func_t)RTA_LIB_ADDR)(NULL, table) (:2556).
- Other fixed entry points: DDK + 0x40 sensor register func (sensor/module_framework/fimc-is-interface-sensor.h:75),
  LIB_ISP_ADDR = DDK + LIB_ISP_OFFSET 0x80 (interface-ddk.h:188, register_lib_isp = the ISP function table getter),
  DDK + 0x100 / RTA + 0x100 shut-down (interface-library.h:105-107).

### The OS-services table the DDK gets (set_os_system_funcs, interface-library.c:1977-2050)
Index: 0 log_write_console, 3 assert, 4-7 sema init/finish/up/down, 8-12 mutex init/finish/lock/tryLock/unlock,
13-18 timer create/delete/reset/query/enable/disable, 19-23 interrupt register/unregister/enable/disable/clear,
24-25 svc spin lock save / unlock restore, 26 get_random_int, 27 add_task, 28 get_usec, 29 log_write (VARIADIC),
30-35 dma_taaisp dva/kva/sleep(32)/inv/alloc/free, 36-43 spin lock init/finish/lock/unlock/_irq/_irqsave variants,
46 get_reg_addr (ISP block register base by id: the blob touches the hardware THROUGH POINTERS WE GIVE),
47 in_irq, 48 flush_task_handler, 49-50 FD data, 60-64 dma_tnr, 65-69 dma_medrc, 91 get_binary_version,
99 event_write. RTA has its own layout (set_os_system_funcs_for_rta, :2052+: 0-2 log/assert, 30-31 general irq,
40 add_task, 50-58 timers/delays, 60-63 sema, 80-81 spin, 90 get_usec ...).
- DMA addresses are u32 ("int fimc_is_dva_dma_taaisp(ulong kva, u32 *dva)", :648): 32-bit device addresses.

### What this means for Windows (questions Stage 1 must answer from the binaries)
- Q1 position dependence: the image is copied, never relocated, to a fixed kernel VA. AArch64 code is PC-relative
  (ADRP/ADD, BL) so code may run anywhere, but absolute pointers in its data (function-pointer tables, string
  tables) would be fixed-VA values. Scan the data part for 64-bit words inside [LIB_START, LIB_START + LIB_SIZE).
- Q2 x18: Windows ARM64 reserves x18 (kernel: KPCR, user: TEB); a Linux 4.14 kernel blob may use x18 as a scratch
  register. Disassemble and count x18 reads/writes. If used: run the blob where x18 is not Windows' (an
  emulator / a user-mode harness that saves-restores x18 around every call, or a dedicated thread).
- Q3 types: Linux LP64 vs Windows LLP64 - every "ulong"/"long" in the table is 64-bit; our implementations must
  use 64-bit types there.
- Q4 FP/SIMD: the loader brackets start_up with fpsimd_get/put = the blob uses NEON in kernel.
- Q5 varargs: log_write is variadic with the AAPCS64 va_list (a struct); a Windows ARM64 C callee sees va_list
  as char * - callee must be written for AAPCS64 variadics (or only the fixed args used).
- Q6 SysMMU: the ISP DMAs use 32-bit DVAs from our table. If the ISP SysMMUs are left in their reset BYPASS state
  (as the CSIS one is, the camera DMA works with physical addresses today), DVA = PA below 4 GiB from contiguous
  buffers and no SysMMU driver is needed. UNVERIFIED for the ISP blocks; reading a SysMMU register aborted
  (S10eCam stage 3 step 8, notes/s10-camera.md) - its state must be learnt another way (DT / GPL driver, no reads).

### Order of work
1. Stage 1 (needs the user's fimc_is_lib.bin, fimc_is_rta.bin, setfile_3j1.bin): static triage - sizes vs the
   areas above, version strings (carve_binary_version), the start_up prologue, Q1 / Q2 / Q4 scans, the table getter
   at +0x80 and which OS-table indices the code actually loads.
2. Stage 2: run start_up and the ISP init path in an EMULATOR on the build machine (Unicorn, AArch64) with stub
   OS functions that log every call - learns the call sequence with zero risk to the phone.
3. Stage 3: Android oracle (tools/android/camtrace-s10.sh, camhal-ioctl.js): what the vendor stack does per frame.
4. Stage 4: only then a Windows loader (user-mode harness first if Q2 allows nothing better), SysMMU question settled.

## Progress log
- 2026-11-17: Stage 0 written from the GPL source (above). Asked the user for the three binaries.
