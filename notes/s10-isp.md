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

## Stage 1 - static triage of the user's binaries (2026-11-17, s10cam.zip from camtrace-s10.sh v1.0)
Scripts: tools/isp-re (README there). Offsets below are FILE offsets. The binaries stay out of the repo.

### Identity
- fimc_is_lib.bin 4432944 B (0x43a430), version string "[S5E9820 10705-2021/04/05 16:59:12]L12XLOD01GM";
  fimc_is_rta.bin 3986368 B (0x3cd3c0), "[S5E9820 b18e67877-09/26 04:52:21]L12XLOD01GM"; setfile_3j1.bin ends with
  "210402_3J1 2021/04/02/16:23 E9820". (strings; the pull also has fimc_is_fw_mcu.bin and the other setfiles.)

### Layout (confirmed in assembly)
- DDK file = CDH (0..0x20000) + VRA (0x20000..0xE0000) + DDK (0xE0000..). start_up at 0xE0000: adrp/add
  x1 = 0xE0040, x2 = 0xE0080, then bl 0xf25b8 (the real init); 0xE0040 -> bl 0xf6514, 0xE0080 -> bl 0xf1af4,
  0xE0100 -> bl 0xf29cc. RTA file starts with its entry: 0x0 "mov x0, x1; bl 0x25a60" (start_up(NULL, table) ->
  table), 0x100 -> bl 0x24cc0 (shut-down) - matches the GPL calls of Stage 0.

### Q1 position dependence - ANSWERED
- Load base, by prologue votes (basefind.py): DDK file offset 0 = 0xffffff80f9fe0000 (4277 votes; the next
  candidate 303). With it ALL 11394 VA-looking words of the file land inside the file (basecheck.py: 0 outside;
  4277 on prologues, 1016 on string starts). RTA file offset 0 = 0xffffff80fa500000: all 610 words inside.
- This is exactly Stage 0's arithmetic with VMALLOC_START = 0xffffff8008000000 (VA_BITS 39): LIB_OFFSET =
  0xffffff80f6000000, LIB_START = 0xffffff80f9fe0000, DDK_LIB_ADDR = LIB_START + 0xC0000 + 0x20000 =
  0xffffff80fa0c0000 (file 0xE0000), RTA = DDK_LIB_ADDR + 0x440000 = 0xffffff80fa500000.
  (The 0xffffff8101fe0000 in an earlier scratch computation was a wrong VMALLOC_START.)
- Where the absolute words are: DDK file 279 in the VRA part (first 0x42920), 10159 in 0xE0000..0x420000 (code
  area = .text + .rodata: tables), 956 after 0x420000 (first 0x42f870); RTA 174 below 0x340000, 436 above.
- Code itself is PC-relative (adrp/add, bl). So a loader elsewhere = copy + add (new base - old base) to those
  words. A relocation list is not in the file: the list must be the scan, which can misfire on a data value that
  only looks like a pointer - UNVERIFIED that none does; movz/movk-built absolute addresses not searched yet.
- The DDK's own memory map (its start_up log, observed in emulation, Stage 2 below): .text 2463408 B ends
  0xffffff80fa3197f0, .rodata 747432 B ends 0xffffff80fa3cff98, .bss 63600 B ends 0xffffff80fa40f870, ".data"
  12519312 B ends 0xffffff80fb000000, heap 62914560 B (60 MiB) ends 0xffffff80fec00000.
- The heap is the DDK's own (newlib-style _sbrk at 0xf4114, called through _sbrk_r 0xe31e8): start =
  adrp image + 0x1020000 (= 0xffffff80fb000000), limit adrp image + 0x4c20000; over the limit it calls os[3]
  (assert). PC-relative: it follows the image. => the DDK needs 0x4c20000 B (76 MiB) of contiguous RW VA from
  LIB_START; the RTA its own area (heap at HEAP_RTA_START per Stage 0 - not yet looked at in its code).

### Q2 x18 - ANSWERED: used as an ordinary register
- stage1.py over 0xE0000..0x420000: 664 instructions read x18, 666 write it (first: 0xe9ae0 "mov w18, #0x20",
  0xf1bb8 "adrp x18", 0xf2618 "ldr x18, [x3, #0x30]" in the OS-table copy). Built without -ffixed-x18.
- Windows ARM64 reserves x18 (Microsoft ARM64 ABI: kernel mode -> KPCR, user mode -> TEB; must not be changed).
  So the blob cannot be CALLED directly from a Windows driver: an interrupt or a callout seeing the blob's x18
  breaks Windows. Options: (a) an emulator (Stage 2 tool, slow - NEON heavy), (b) a user-mode host where every
  entry and every OS callback saves/restores x18 - still unsafe if Windows itself reads x18 between (exceptions,
  APCs: UNVERIFIED how Windows user mode reacts). Decision deferred to Stage 4; until then nothing runs on the phone.
- Same in the other parts (review fix, linear sweep): VRA 0x20000..0xE0000 170 reads / 140 writes (first
  0x2006c), RTA 0..0x340000 718 / 667 (first 0x5c4c).

### Q4 privileged state - none found in the code paths
- DDK 0xE0000..0x420000: no msr/mrs of EL1 registers, no eret/hvc/smc; only brk, mrs fpsr/fpcr/dczid_el0,
  dc zva: EL0-safe. 21324 NEON/FP instructions (fpsimd_get/put of the GPL loader confirmed needed).
- VRA part and RTA (review fix): linear sweep shows single hits of implementation-defined msr/mrs encodings
  (s2_*/s3_* ...), 25 hlt in the VRA part and one svc in the RTA. The sweep also reports 40143 / 47499
  undecodable words = data inside those areas, so these single hits are most likely data decoded as code -
  UNVERIFIED until a reachability pass (follow branches from the entries) confirms none is executed.
- Function names in this file (_sbrk, _sbrk_r, spin lock ...) are inferred from code shape and the GPL table
  order, not from symbols (the binaries are stripped of a symbol table: none seen).

### Q6 SysMMU on Android
- dmesg-camera.txt: DMA_TAAISP "dva 0x11400000" = an IOVA, so on Android the ISP SysMMUs TRANSLATE. Whether the
  ISP blocks can DMA with SysMMU in bypass under Windows (as CSIS does) is still UNVERIFIED.
- dmesg also: ISP register VAs 3AA0 0xffffff801f5a0000, ISP0 0xffffff801f9f0000, ISP1 0xffffff801fa10000
  (ioremap VAs: meaningless for Windows; the physical bases come from the DT/GPL), and AE starting at fps(120,120).

### OS table use (Unicorn, osmap.py - this is the first Stage 2 run)
- 0xf25b8 copies table indices 0..50 and 60..69 (61 entries) into globals at image 0x420xxx (os[3] -> 0x420278,
  os[29] -> 0x4203e8, os[36] -> 0x420470, os[46] -> 0x420390, os[47] -> 0x4203b8 ...). 51..59 and 70..99
  (incl. 91 get_binary_version and 99 event_write) are NOT taken: we never need to implement them for the DDK.
- With every OS function = "ret, x0 = 0", the start_up path from 0xf25b8 runs to its ret: 528 OS calls -
  os[36] spin_lock_init 173, os[42]/[43] spin lock irqsave/irqrestore 161 each, os[24]/[25] svc spin lock
  save/restore 12 each, os[29] log_write 8, os[26] get_random_int 1. No register access, no DMA, no IRQ yet.
- os[29] receives in x0 a finished string in a DDK buffer (image 0x420288): the DDK formats with its own
  formatter (the 8 lines: "start_up_ddk", the section map above). Whether x1.. carry anything is not checked,
  so Q5 is only likely moot for this call site (0xf22d0). Other variadic entries (os[0]) not seen yet.
- Caveat of the method: at an OS call the hook logs x0, and some call sites load the function pointer into x0
  itself (os[24]/[26] show their own marker value): those calls take no argument in x0.

## Progress log
- 2026-11-17: Stage 0 written from the GPL source (above). Asked the user for the three binaries.
- 2026-11-17: Stage 1 (above) from s10cam.zip: base found, x18 used, EL0-safe, heap self-managed; start_up emulated.
  Next: emulate the +0x80 ISP table getter (0xf1af4) and +0x40 sensor register (0xf6514) to list the function
  tables they return; same OS-table copy for the RTA; i2c-trace.txt (address 0x10) vs our 3J1 setfile.
- 2026-11-17 review of 6023f5e: the x18 / system-register scan covered only the DDK part -> VRA and RTA added
  with the linear-sweep caveat; "os[29] takes one argument" weakened to what was observed (x0); the hook's x0
  caveat recorded; the dropped "(c)" option was not an option.

## Stage 2 - the DDK in the emulator (2026-11-17, tools/isp-re/emu.py; user: "dịch ngược, được đến đâu thì đến")
### Function tables (observed in emulation)
- get_lib_func (+0x80, register_lib_isp(type, &table)) returns tables of 20 entries = GPL struct lib_interface_func
  order (interface-ddk.h:206): type 1 3AA -> image 0x420048, 2 ISP -> 0x4200e8, 3 TPU -> 0x420188; 4 VRA / 5 DCP
  -> NULL (this build has no DCP; VRA goes through its own path). 3AA: chain_create 0xf0378, object_create 0xf0788,
  set_param 0xf0e38, set_ctrl 0xf0ed4, shot 0xf0f88, get_meta 0xf1060, create/apply/delete_tune_set 0xf1204/
  0xf1284/0xf13c8, set_system_config 0xf1590 (all image offsets). Each call prints the banner "FIMC-ISP LIBRARY,
  DDK revision: 10705, DDK date: 2021-04-05 16:57:46".
- register_sensor_interface (+0x40) with a struct fimc_is_sensor_interface (layout: tools/isp-re/
  sensor_itf_layout.txt, 5704 B, from the GPL header with the beyond1 defines) returns 0; it calls
  cis_itf_ops.get_module_position.
### Which camera blocks the front camera uses on Android (dmesg-camera.txt)
- 3AA0 + ISP0 (= ISPLP) + MCSC0, chain 0: "[HW:3AA0] chain_create done [reg_base:0xffffff801f5a0000]", "[HW:ISP0]
  ... 0xffffff801f9f0000", "[HW:MCS0] mcsc_open". DT fimc_is@17190000 reg list in the GPL order
  (fimc-is-v7_0_0/fimc-is-hw-chain.h:140 IORESOURCE_*): CSIS_DMA 0x170E0000, 3AA0 0x17190000, 3AA1 0x172A0000,
  ISPLP 0x17430000, ISPHQ 0x17730000, MCSC 0x17440000, VRA 0x17630000, STAT_DMA 0x17140000.
- module id: front sensor S5K3J1 = SENSOR_NAME_S5K3J1 38 (include/exynos-fimc-is-sensor.h:110); the DDK echoes
  "moduleId(38) sensorPlace(1)" when the sensor interface answers get_module_id 38 / position 1. (That the kernel
  passes module_id = sensor_id is INFERENCE from the log shape, not read in the GPL call chain.)
### chain_create probes the hardware version (read in assembly + emulation)
- The chain factory reads +0xF4 (and +0xF0) of the block and compares against 17 candidates (image 0x20f6a8 ..
  0x23a5ac), e.g. 0x20f6a8: F4 == 0xdbc4 && F0 == 0x06000000. With F0/F4 = 0 every candidate fails and the
  DDK asserts "IpChainObject (SIRC_IspChainFactory.cpp:93)".
- HYPOTHESIS used in the emulator only (UNVERIFIED on hardware): 3AA0 F0 0x07000000 / F4 0xcc12 (candidate at
  0x216d10) and ISPLP F0 0x07000000 / F4 0xd874 (0x22b524). With them the DDK picks classes named
  "ISP_Fimc3aaChainV7P0P0" and "ISP_FimcIspChainV7P0P0" - the same 7.0.0 generation as the GPL kernel's
  fimc-is-v7_0_0 for the 9820, which supports (does not prove) the guess.
- Next wall: every sub-block of the chain checks its own ID registers too ("Identify()", e.g. 3AA0 +0x14f4/+0x14f8,
  ISPLP +0x07d8/+0x07dc ... via ISP_DmaClientWithDescriptorV04.hpp:23), then the chain/objects are NULL.
  Guessing hundreds of IDs is not the way: the real values come from the phone -> tools/android/ispregs-s10.sh.
### object_create learns the sensor from the interface (observed)
- DICO_Object_3aa_Create asks the sensor interface: get_module_id, is_flash/actuator/companion/ois/aperture_
  available, get_calibrated_size, get_bayer_order, min/max exposure and analog/digital gain, get_initial_exposure_
  gain_of_sensor, get_sensor_frame_timing, get_sensor_cur_size, get_sensor_max_fps, get_hdr_ratio_ctl_by_again,
  get_sensor_use_dgain, get_sensor_initial_aperture, set_sensor_3a_mode = the sensor facts our Windows side must
  answer (all are GPL fimc-is-interface-sensor.c functions backed by the CIS driver - portable).
- OS services it uses so far: DMA medrc pool (os 68 alloc 10048 B x n, 65 dva), mutex_init, assert.
### Android's front-camera sensor sequence (i2c-trace.txt, bus i2c-2 address 0x10, 8640 register writes)
- Three sensor modes in one session: 1701.101 912 x 684 (X_OUT 0x390 / Y_OUT 0x2ac, binning 0x0900 = 0x244,
  LLP 0x2318) = our mode 11 (setfile 120 fps, the DDK's AE warm-up: dmesg "Frame Time 8245" us); 1701.285
  3648 x 2736 (0xe40 / 0xab0, binning 0x11 = none) = full 12 MP for preview/photo; 1711.951 3968 x 2232 (0xf80 /
  0x8b8) for the 16:9 video. dmesg RTA_AE "start fps(60,60)" at video start.
- Per frame the DDK/RTA rewrites FLL 0x0340, CIT 0x0202, AGAIN 0x0204, DGAIN 0x020E (1326 x each over ~22 s) =
  the 3A loop runs on the sensor every frame.
- Meaning for Windows: Android never streams 912 x 684 to the user - the full sensor output goes through the ISP
  (debayer, scaling, 3A). A CPU path at 3648 x 2736 is ~16x the pixels of today's binned mode.
