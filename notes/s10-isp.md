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
  3648 x 2736 (0xe40 / 0xab0, binning 0x11 = none) = full 10 MP for preview/photo; 1711.951 3968 x 2232 (0xf80 /
  0x8b8) for the 16:9 video. dmesg RTA_AE "start fps(60,60)" at video start.
- Per frame the DDK/RTA rewrites FLL 0x0340, CIT 0x0202, AGAIN 0x0204, DGAIN 0x020E (1326 x each over ~22 s) =
  the 3A loop runs on the sensor every frame.
- Meaning for Windows: the 912 x 684 mode ran ~0.18 s (1701.101 -> 1701.285, INFERENCE: AE warm-up only); the
  preview and the video used the full sensor output through the ISP
  (debayer, scaling, 3A). A CPU path at 3648 x 2736 is ~16x the pixels of today's binned mode.
- 2026-11-17 review of 91eee61: 3648 x 2736 is 10 MP (not 12); "never streams 912 x 684" was an inference -> worded
  as one; ispregs-s10.sh found devmem by grepping a usage text (wording varies) -> applet lists now, and the power
  check runs every 64 registers instead of every 1024 (a camera closed mid-dump = a read of a powered-off block =
  a phone reset; the window between checks is now ~64 reads). Residual risk stated in the script header.
- 2026-11-17 15:54 DEVICE (user, ispregs-s10 v1.0 check/ids/dump): "/dev/mem: MISSING (kernel built without
  CONFIG_DEVMEM)" on the running ExtremeKernel 4.14.356 (OneUI 6.1); toybox devmem exists. Nothing was read.
  Script bug found: the runtime_status glob matched /sys/devices/platform/17030000.fimc_is_sensor (another device);
  v1.1 looks only at 17190000.fimc_is (the ISP node, DT fimc_is@17190000).
- Routes to the real register values: (a) an Android kernel with CONFIG_DEVMEM=y (the user builds S10 kernels),
  then ispregs-s10 v1.1 unchanged - gives the full live configuration, not only IDs; (b) a small kernel module
  built against the running kernel's exact source + config (vermagic / modversions) - same reads; (c) read only
  the ID registers from Windows (3AA0 sits in ISPPRE with CSIS, which S10eCam already uses; ISPLP is another
  power domain: its PMU status must be read first) - IDs only, no live configuration. Waiting for the user's pick.
- 2026-11-17 user's pick: route (a), an Android kernel with /dev/mem. Config for the user's build (Linux 4.14
  drivers/char/Kconfig, general knowledge - not read in this tree, the sparse clone has no drivers/char):
  CONFIG_DEVMEM=y; CONFIG_STRICT_DEVMEM may stay y (on arm64 it refuses System RAM only); CONFIG_IO_STRICT_DEVMEM
  must be OFF - it also refuses ranges a driver has claimed, which the ISP windows are while fimc_is runs.
  Kernel = the OneUI one in use (ExtremeKernel 4.14.356) rebuilt with these lines; Samsung-specific blocks on
  /dev/mem (RKP/uH) UNVERIFIED - "check" + "ids" show at once whether reads work.
### RTA (fimc_is_rta.bin) first look (2026-11-17)
- Entries (assembly): 0x0 start_up(NULL, table) -> bl 0x25a60 with x0 = table; 0x40 -> bl 0x287cc; 0x80 ->
  bl 0x248b0 (a table getter like the DDK's +0x80 - not run yet); 0x100 -> bl 0x24cc0 (shut-down).
- Emulated start_up with the marker table: returns 0 after three OS calls, RTA table index 80 (spin lock), 0
  (log, string in a stack buffer) and 81 (spin unlock) - the RTA layout of set_os_system_funcs_for_rta.
- How the DDK finds the RTA: image 0x2f33f4 "adrp x0, 0x520000; add x0, #0x80; ret" - a PC-relative pointer to
  RTA + 0x80 (the only DDK adrp into 0x520000..0x900000 besides one at 0x5aae4 in the VRA part). So the RTA must
  sit exactly 0x520000 after the DDK file start, as on Android: the pair moves together, never apart.
- The DDK's own section log (".data" size 12519312 ending 0xffffff80fb000000) spans the RTA's area
  (0xffffff80fa500000..): that "data" is a linker span up to the heap, not 12 MB of DDK data - the 76 MiB
  footprint from LIB_START holds DDK + RTA + DDK heap; the RTA heap is separate (HEAP_RTA_START, Stage 0).
- 2026-11-17 19:48: built ExtremeKernel OneUI 6 beyond1lte "ispdbg1" (DEVMEM, IO_STRICT_DEVMEM off, DEBUG_FS,
  DYNAMIC_DEBUG, KPROBES + KPROBE_EVENTS, FUNCTION(_GRAPH)_TRACER) - recipe tools/android/kernel-ispdbg. Config
  check: UH/RKP not set in exynos9820_defconfig (:674 "# CONFIG_UH is not set", :642 RKP_CFP off), so kprobes /
  ftrace text patching is not blocked by a hypervisor. Not yet on the phone.
- 2026-11-17 20:08 DEVICE (user, ispdbg1 kernel, Termux): "/dev/mem: present", "devmem tool: toybox devmem" - the
  kernel works and /dev/mem exists. ispregs v1.1 found no /sys/devices/platform/17190000.fimc_is -> v1.2 searches
  /sys/devices (depth 4) for a directory named *17190000* and lists the candidates / fimc nodes it sees.
- 2026-11-17 20:11 DEVICE (user, v1.2 check): no *17190000* directory; the ISP node is
  /sys/devices/platform/170e0000.fimc_is - Linux names it after the node's first reg (CSIS_DMA 0x170E0000, live
  DT fimc_is@17190000 reg list). v1.3 uses that path. Other fimc nodes seen: 17030000/17040000/17050000/17060000/
  17090000.fimc_is_sensor, 17050000.fimc_is_sensor2, fimc-is_sensor_3j1@10 / 2l4@20 / 3m3@5A / 3p9@2D (the S10's
  four sensors: front 3J1, rear 2L4 wide, 3M3 tele, 3P9 ultra-wide - names from the sysfs entries).
- 2026-11-17 20:17 DEVICE (user, v1.3): runtime_status file found (170e0000.fimc_is), "suspended" with the
  camera closed, "active" with it open - the power gate works. But every value came back EMPTY: rd() passed width
  "32" to toybox devmem, whose WIDTH is in BYTES (1/2/4/8) - invalid argument, error thrown away by 2>/dev/null.
  Nothing was read from the hardware. dump stopped at 3AA0 +6656 because runtime_status left "active" (the camera
  app was closed or paused - user to confirm). v1.4: no width (toybox default 4 B, busybox default 32 bit), errors
  kept in the output, one test read before ids/dump that stops the run if it is not a 0x value.
### REAL hardware IDs (2026-11-17 20:22, DEVICE, ispregs v1.4 "ids", front camera open, ispdbg1 kernel)
- 3AA0 +0xF0 = 0x07010000, +0xF4 = 0x0000cc22, +0xF8 = 0xdeadc0de; ISPLP +0xF0 = 0x07110000, +0xF4 = 0x0000c2ad,
  +0xF8 = 0xdeadc0de; MCSC +0xF0/F4/F8 = 0xdeadc0de (no ID there - MCSC is driven by the GPL kernel, not the DDK).
- => the Stage 2 HYPOTHESIS (3AA 7.0 / 0xcc12, ISP 7.0 / 0xd874) was WRONG. Real: 3AA v7.1 (DDK candidate at
  0x216d10's neighbour list: F4 0xcc22, F0 0x07010000) and ISPLP v7.11 (F4 0xc2ad, F0 0x07110000). The emulator now
  picks "ISP_Fimc3aaChainV7P1P0" and "ISP_FimcIspChainV7P11P0" - the classes Android runs on this phone.
- 0xdeadc0de = the value this SoC returns for unimplemented offsets inside a powered block (512 of the first 1668
  words of 3AA0).
- Partial 3AA0 dump (devmem, words 0x0000..0x1a0c before the user stopped it - 50 ms per word): with it the 3AA
  chain constructor gets further (sub-block IDs at +0x14f4/f8 0x8eb9/0xc907f815, +0x15f4 0xa95c, +0x17f4 0xc6b9,
  +0x19f4 0x91e9) and now fails reading +0x1af4/+0x1af8 = past the dumped part. ISPLP needs its own dump (its
  sub-blocks read +0x4d8.. +0xcd8 identifiers).
- tools/android/regdump-s10.c v1.0: one static aarch64 process (zig cc -target aarch64-linux-musl -static) that
  mmaps each block through /dev/mem (O_SYNC) and reads 32-bit words, re-checking runtime_status every 64 reads.
  ispregs v1.5 uses it for "dump" when regdump-s10 sits next to the script. Not yet run on the phone.
### The init path runs end to end in the emulator (2026-11-17 20:40, tools/isp-re/front_init.py)
- Input: full register dumps of 3AA0 / ISPLP / MCSC taken on the phone with the front camera previewing
  (DEVICE 20:31, ispregs v1.5 + regdump-s10: 16384 words each; non-zero non-0xdeadc0de words 1984 / 2153 / 476)
  -> notes/data/isp-dump-20261117/.
- With them every "Identify()" passes. Remaining blockers were emulator-side and are modelled from assembly:
  (1) SW reset: 3AA/ISP +0x0c write 1, poll until 0 (0x219e90, limit 10001); ISP COREX +0x24 write 1, poll +0x2c
  until 0 (0x24f408, limit 1001; copy at +0x8000) - emu.done_after; (2) sema/mutex/spin init take void ** and
  store a handle (GPL interface-library.c:821/877/1358) - os_defaults; (3) the VRA part needs its own OS struct
  (18 entries, GPL interface-vra.h:70) via VRA_LIB_ADDR (file 0x20000) - Isp.vra_os_funcs; (4) object_create
  calls the RTA (rta+0x80) -> RTA loaded at its place with its own table (GPL set_os_system_funcs_for_rta) -
  Isp.rta_start_up ("[RTA]OS service was registered").
- Result: start_up, vra_os_funcs, rta_start_up, register_sensor_interface -> 0; 3AA chain_create -> 0 (333
  register accesses, interrupts 0/1 of chain 0 registered, handler ddk+0x331e34); set_system_config -> 0;
  3AA object_create -> 0 (object ddk+0x19e8a00; RTA "RegisterRtaInterface FRONT GammaVersion 0x070B1023
  DRCVersion 0x070B0451", AE/AWB/SCD/AFC objects created, "ActivateAlgorithms ae1 awb1 af0"); ISP chain_create
  -> 0 (503 accesses, "ISP_FimcIspChainV7P11P0 ... CHAIN_REGISTER_MODE_SHADOW", interrupts 6/7 of chain 2);
  ISP object_create -> 0 (ddk+0x1d6d2f0). Logs: notes/data/isp-emu-20261117/.
- Caveats: the sensor interface answers 0 for every size / timing / gain query (only module id 38 and FRONT are
  real), so the 3A objects were created around zeros; "[ERROR] ActivateAlgorithms object is NULL, sink id(6)" x8
  is printed - unknown whether Android prints it too (no isfw-msg from Android yet; the ispdbg1 kernel has debugfs,
  so /sys/kernel/debug/fimc-is/isfw-msg can now be pulled). The register values are a LIVE snapshot (camera
  streaming), not the reset state: status registers in it are not what an idle block reads.
- Next: answer the sensor interface with the real 3J1 numbers (GPL cis driver + setfile), then set_param / shot
  for one frame, then the frame-done interrupt path (fire irq 0/1 handler ddk+0x331e34 in the emulator).
- 2026-11-17 20:41 DEVICE (user): `cat /sys/kernel/debug/fimc-is/isfw-msg` gave nothing (camera state at the time
  not stated). Source (ExtremeKernel tree): isfw_debug_read returns 0 unless FIMC_IS_DEBUG_OPEN is set
  (fimc-is-debug.c:733), set by fimc_is_debug_open from fimc-is-resourcemgr.c:1595 when the camera resources are
  taken and cleared at :1819 when they are released -> the file is readable only WHILE the camera is open; the DDK
  log_write fills that ring (interface-library.c:41-86). Also Android 14 may not mount debugfs: mount it first.
### Android's own ISP log = the oracle (2026-11-17 20:43, DEVICE, isfw-msg read with the camera open)
File: notes/data/isp-dump-20261117/isfw-msg-android.txt (602 lines, front camera, ispdbg1 kernel).
- The emulator matches it line for line up to object_create: same banner, same section map, "createChain"
  sequence, interrupt ids 0/1 (chain 0) and 6/7 (chain 2), "ISP_Fimc3aaChainV7P1P0::SwReset", the RTA FRONT Gamma
  0x070B1023 / DRC 0x070B0451 lines - and the SAME 3AA object address: Android "DICO_Object_3aa_Create
  0xffffff80fb9c8a00" = emulator ddk+0x19e8a00 (0xffffff80f9fe0000 + 0x19e8a00). The DDK heap is deterministic
  and the emulation follows the real allocation order.
- Differences = what the emulator still answers wrong:
  * sensor properties (WRAP_GetProperties): calibrated w 3968 h 2736, bayer order 0, exposure 23..33257 us
    (first mode 22..8260), again 1000..16000, dgain 1000..128000, use_sensor_dgain 1, initialAperture 190,
    3aaotfmode 1; mode 1: pclk 807240000, line_length_pck 8984, frame length 746, cur 912 x 684, max fps 120;
    mode 2: pclk 807720000, llp 9384, fll 2864/2866, cur 3648 x 2736, max fps 30. Emulator: all 0.
  * ISP object_create obj_info 0x00261000 = input(1) M2M (emulator used 0x260000 OTF); a second ISP chain
    "ISP1" (ISPHQ, 0xffffff801fc80000, ISP_FimcIspChainV7P1P0, interrupts 9/10, obj_info 0x00261111 rep 1) is
    created later for reprocessing (photo).
  * Android: "[CreateAutoFocusObjectExternal]", "ActivateAlgorithms ae1 awb1 af1", "Ext AF is selected!" (the
    actuator is available); emulator af0 + "ActivateAlgorithms object is NULL, sink id(6)" x8 = the missing AF.
- What Android does after object_create (the sequence to emulate next): VRA init ("Makalu VRA V2.0 Revision 9641"),
  DICO_CreateTuneSet index 0..28 (= the setfile, setfile_3j1.bin), FROM calibration load (AF cal pan 384 /
  macro 554), DICO_GetCalData, DICO_ApplyTuneSet, sensor mode 912 x 684 @120 "Fast AE" (TuneSetIndex 7), first
  3AA frame: ISR V SYNC -> CIN ROW -> FRAME END, "Valid Time 7613 / Frame Time 8251"; then 3aa stop, ApplyTuneSet
  I6, preview mode: sensor 3968 x 2736 calibratedCrop 3648 x 2736, 3AA bayer DMA out 2944 x 2208 (11/12 bit) ->
  BDS 2432 x 1824 -> ISP0 M2M input 2432 x 1824, 30 fps (minFps 7), PDAF stats (2PD mode3 456 x 342), TNR, DRC.
- So on Android the front preview is NOT the 912 x 684 mode: 3AA crops the 10 MP frame to 2944 x 2208 (4:3
  centre), scales to 2432 x 1824, ISP processes it, MCSC (GPL) makes the app's sizes.
- 2026-11-17 20:49 user: "chạy 120fps nó nổ máy" (running 120 fps blows the phone up / crashes it - context not
  given) and asks to keep only what was checked on this S10. Facts from the phone's own log (isfw-msg-android.txt):
  the 120 fps "Fast AE" mode (912 x 684, scene 136, TuneSetIndex 7) ran for exactly ONE frame (F1, "Frame Time
  8251" us, count 1) at 2653.93 s, then 3AA stopped and restarted at 30 fps ("Frame Time 33226"). Decision: the
  Windows side stays at what runs sustained on this phone - 30 fps; 120 fps is never a streaming mode for us, and
  FrameRate 60 (S10eCamera 0.9) stays unvalidated until the user's test.
- 2026-11-17 20:55 user: "làm đi" (the 120 fps remark was a guess). front_init.py now answers the sensor interface
  with this phone's Android values (30 fps preview mode only: cal 3968 x 2736, order 0, exp 23..33257, again
  1000..16000, dgain 1000..128000, init 2360/1000/1000 ("init 2360 1000 1000" in the log), pclk 807720000, llp
  9384, fll 2864, max_margin_cit 0 (GPL: cur_coarse_integration_time_step, not set by cis-3j1.c -> 0), cur
  3648 x 2736, 30 fps, use_dgain 1, aperture 190, actuator available) and ISP object_create obj_info 0x00261000
  (M2M) as Android. emu: stubs now see x0..x7 (get_initial_exposure_gain_of_sensor has 6 out pointers).
  Result: "[CreateAutoFocusObjectExternal]", "ActivateAlgorithms ae1 awb1 af1", "Ext AF is selected!" as on
  Android, and the ISP object at ddk+0x1d6d320 = 0xffffff80fbd4d320 = Android's "DICO_Object_Isp_Create
  0xffffff80fbd4d320". Both objects now sit at Android's addresses. Left: "3aaotfmode(0)" vs Android's 1 (which
  interface answer sets it - not found yet).
### Setfile: tune sets created and applied (2026-11-17 21:05, front_init.py)
- setfile_3j1.bin header (GPL hw-control.h:192 V3, magic 0x12345679): designed bits 0x798, "ISV7" "LL08", 64
  scenarios, 6 IPs; per-IP tune-set counts [29, 1, 1, 1, 1, 2]; IP 0 = 3AA_ISP (design bit 3), entries of
  0x21210 B each from file offset 0x74c. Scenario -> 3AA_ISP index table starts 7, 10, 14, 12, 0, 1, 25 ...
- create_tune_set x29 (struct lib_tune_set, decrypt_flag 1 for V3 as hw-3aa.c load_setfile) -> all 0; the
  handles equal Android's: index 0 0xffffff80fbfde2d0, index 6 0xffffff80fc0a4f90 (isfw-msg-android.txt).
- apply_tune_set(object, 0, 6) (Android's preview index; the one-frame Fast AE index 7 skipped) -> 0:
  "[DeserializeContext] start address : fc0a4f98, size : 135686" and "Setfile Read : SUCCESS! [135686/135686]"
  - byte for byte Android's numbers. No register access yet: tune data is applied to hardware on the first
  frame (to confirm with set_param / shot).
- Not done yet (Android does them): DICO_SystemConfig for the ISP chain, VRA frame-work init + tune copy,
  DICO_LoadCalData (EEPROM calibration at 0x1f752000 on Android - our side has no EEPROM read yet).
### FIRST FRAME: 3AA shot F1 runs and programs the hardware (2026-11-17 21:30, front_init.py)
- Struct layouts: tools/isp-re/param_layouts.txt (taa_param_set 1776 B, isp_param_set 1136 B, camera2_shot
  36592 B, clang record layout of the ExtremeKernel tree's v7_0_0 fimc-is-param.h + interface-ddk.h +
  fimc-is-metadata.h). Filled with Android's preview numbers (isfw-msg-android.txt :318-322); format / bitwidth /
  msb encodings inferred from enum names - UNVERIFIED. shot: magic 0x23456789, aa.mode AUTO(2), scene 1,
  capture intent PREVIEW, fps 7..30, frameDuration 33.3 ms.
- Two more registrations Android does that the run needed: the sensor interface is ALSO registered with the RTA
  (RTA_LIB_ADDR + 0x40, GPL fimc-is-device-sensor_v2.c:2934) - without it the RTA asserts "GetSensorIF() != NULL";
  csi_itf_ops.get_vc_dma_buf_info answers Android's PDAF buffer infos (type 1: 500/102/1756x1, type 3:
  501/102/39424x1, others INVALID).
- Calibration: Android loads the module EEPROM data (kvaddr_cal[FRONT]); we have none -> load_cal_data with a ZERO
  64 KiB buffer (PLACEHOLDER): "FROM CAL. DATA LOAD: FAIL! (0x600f)", get_cal_data type 2 gives centre 1584/1208
  (Android 1888/1400). Everything calibration-dependent below is not Android's.
- set_param -> 0; set_ctrl F1 -> 0; **shot F1 -> 0** with 5003 register accesses (4977 writes to 1564 distinct
  3AA0 registers, half of them in the upper 32 KiB = the second shadow set, CHAIN_REGISTER_MODE_SHADOW). RTA ran
  AE "CmlAeBeforeStreamOn ... start fps(7,30) end Ev=0B88 Tv=089F Sv=0400", AF boot init, PDAF config
  (pd 456 x 342 as Android), "InitFrameElement FrameElementForME (0xFBB84530)" = Android's address again.
- Check against the phone: of the 1564 registers the emulated F1 wrote, the final value of 1124 (72 %) equals the
  value read on the phone while Android previewed (notes/data/isp-dump-20261117/3AA0.txt). The rest is expected
  to differ: the phone's dump is a later frame with live AE/AWB and real calibration.
- Log: notes/data/isp-emu-20261117/mmio-3AA_shot_F1.txt. Remaining "[ERROR]CheckInputCritical DDK set B Shadow
  set B, Corex Critical set A" = the live dump's shadow/COREX state (taken mid-stream) seen at frame 1.
- Next: ISP (M2M) set_param/shot for F1, the frame interrupts (handler ddk+0x331e34: V SYNC / CIN ROW / FRAME
  END as Android's "TransitIsrState" lines), then F2..: what the blob writes per frame vs once.
- 2026-11-17 21:40: the calibration exists only in kernel RAM on Android (kvaddr_cal, /dev/mem refuses RAM).
  Samsung's own debug switch DEBUG_FORCE_DUMP_ENABLE (ExtremeKernel tree vendor/mcd/fimc-is-sec-define.h:166,
  commented out) makes fimc_is_sec_readcal_eeprom write every EEPROM read to FIMC_IS_FW_DUMP_PATH
  "/data/vendor/camera/" as "rom<rom_id>_dump.bin" (sec-define.c:1216-1228; ROM_ID_FRONT = 1 -> rom1_dump.bin) and
  the FROM cal to "from_cal.bin" (:1894). Building "ispdbg2" = ispdbg1 + that define (recipe updated in
  tools/android/kernel-ispdbg). Cal data is per-module calibration of the user's phone: kept out of the repo
  (hash only), used by the emulator from the scratch area.
### 3AA0 interrupt bits, named by the DDK itself (2026-11-17 21:50, front_init.py IRQ_SCAN=1, DIAGNOSIS run)
The handler (ddk+0x331e34, args (id, ctx)) reads 3AA +0x10c for interrupt 0 and +0x11c for interrupt 1
(assembly ddk+0x2f2bd4). One bit set at a time after shot F1 (log: notes/data/isp-emu-20261117/irq-scan-3aa.txt):
- +0x10c: bit 0 -> "TransitIsrState ... --(V SYNC)-->" (frame start); bit 1 -> "--(FRAME END)-->"; bit 2 ->
  "--(CIN ROW)-->" (line interrupt; 126 register accesses = the per-frame update); bit 3 ISR_IrqCorruptedIntr;
  bit 4 "COREX ERROR"; bits 22..29 ISR_CinFifoCoutFifoErrorIntr: 22 COUTFIFO_SIZE, 23 COUTFIFO_LINE, 24 COUTFIFO_COL,
  25 COUTFIFO_OVERFLOW, 26 CINFIFO_TOTAL_SIZE, 27 CINFIFO_LINES, 28 CINFIFO_COLUMNS, 29 CINFIFO_STREAM_OVERFLOW_SIG;
  bit 30 ISR_FimcStartBeforeFrameEndIntr; bit 31 ISR_DmaClientsErrorIrq (dumps registers). Bits 5, 7..19: no
  handler registered (the emulator jumps to NULL = these must stay masked); 6, 20, 21: no action.
- +0x11c: bits 15..24 ISR_AutoFocusInterruptHandler (AF windows, af1stIntId 15); 25 queues a task; others no
  handler.
- Android's order per frame: V SYNC (bit 0) -> CIN ROW (bit 2) -> FRAME END (bit 1) ("ISR V SYNC / CIN ROW /
  FRAME END F7 ..." lines). The error checks compare the configured size with pixel counters (bit 22: "configured
  imagesize (w3648 x h2736 /2) != pixelCount (w1014 x h1483)" - the counters come from the live dump).
- Direct use for Windows: the same register / bit meaning applies whoever drives the 3AA.
- 2026-11-17 21:15 DEVICE (user, ispdbg2): only rom0_dump.bin came out (16384 B, sha256 579e7ec72a0b90fe..., strings
  "L16XLLD01CM", "V010180809V001BEYND0XS") = ROM_ID_REAR (include/fimc-is-vender.h:50) - the REAR module, not the
  front one. Why no rom1: the front EEPROM is read once per boot (FIMC_IS_ROM_STATE_CAL_READ_DONE, sec-define.c:
  3207-3224), apparently before /data/vendor/camera is writable. Samsung's other switches: fimc_is_sec_cal_dump
  (key file 1q2w3e4r.key + dump/ folder) is compiled out by CONFIG_SAMSUNG_PRODUCT_SHIP=y (sec-define.c:3141);
  the RELOAD key /data/vendor/camera/reload/r1e2l3o4a5d.key (sec-define.c:870-877) sets force_caldata_dump = true,
  so the next fw_sel re-reads the EEPROM (-> DEBUG_FORCE_DUMP_ENABLE writes rom1_dump.bin) - it is checked once
  per ROM per boot when the dump path exists (sec-define.c:3128-3137): key file + reboot + open the front camera.
  It also sets suspend_resume_disable: remove the key afterwards.
- 2026-11-17 21:25 DEVICE (user, reload key + reboot): dmesg shows the reload worked ("forced caldata dump!!"), the
  REAR EEPROM read (rom_id 0, cal_size 65536, map V010) and the FRONT one (rom_id 1, cal_size 16384, map V004, crc
  ok, "the dumped Cal. data was applied", 3AA0 "load cal data, position: 1, addr 0xffffff801f73e000", Android AF cal
  "far: 192 ... near: 277"). But /data/vendor/camera holds only rom0_dump.bin, 16384 B although rear cal_size is
  65536 - the write path (write_data_to_file, sec-define.c:484: sys_open O_CREAT|O_TRUNC|O_SYNC from the camera
  process' context) reports errors only as "open file error" / "fail to write", which the user's grep did not
  include. Asked for those lines.
- Android's front AF calibration (useful as a check later): far 192, near 277 (FrontAfLib_BootingInit line 278).
- 2026-11-17 21:27 DEVICE: no "open file error" / "fail to write" / avc lines; /data/vendor/camera again holds only
  rom0_dump.bin (16384 B, rewritten 21:27, content = rear map "V010" at 0x50). The DEBUG_FORCE_DUMP path is not
  giving the front data - not chased further.
- tools/android/calgrab (v1.0): a READ-ONLY module built against the ispdbg2 tree (vermagic matches the running
  kernel) that calls the driver's own fimc_is_sec_get_cal_buf(&buf, rom_id) (sec-define.c:74, found with
  kallsyms_lookup_name) and exposes that buffer as /sys/kernel/debug/calgrab/rom<id> (debugfs blob). No hardware
  access. Build: make -C <kernel out> M=tools/android/calgrab modules (same flags as the kernel).
### Real front calibration in the emulator (2026-11-17 21:35)
- DEVICE 21:30 (user, calgrab.ko rom_id=1 size=16384 on ispdbg2): rom1_cal.bin, 16384 B, sha256 d19d531f15087cb8...,
  header strings "X10LLLB00AM", "V004V001", "BEYND0LF" (map V004 = dmesg's front map). Kept in the scratch area
  only (per-module data). calgrab works: the module route is the reliable one for kernel-RAM data.
- front_init.py takes it as env CAL (copied unchanged, as fimc_is_ischain_loadcalb memcpy's the buffer).
  Every calibration line now equals Android's: "FROM CAL. DATA LOAD: SUCCESS!", AF cal PanFocusPosition 384 (ROM
  0xc0), MacroPosition 554 (ROM 0x115), "No PDAF calibration", GetCalibrationData centreX/Y 1888/1400 biquadA/B
  77828/16384, RTA front AF "far: 192 ... near: 277" - all identical to isfw-msg-android.txt / dmesg.
- 3AA shot F1 with it: 1564 registers written, 1128 (72.1 %) equal to the phone's live values (zeros: 1124) - the
  first frame's register program hardly depends on calibration; the gap is frame / AE state (to check frame by
  frame next).
### Frames F1..F3 with interrupts (2026-11-17 22:00, FRAMES=3)
- emu: add_task (DDK os[27], RTA table[40]) queues work, drain() runs it after each top-level call like the
  library's kthread workers (GPL interface-library.c:1600 lib_task_trigger). Per frame: set_ctrl + shot (F2/F3:
  0 register accesses - shot only queues), then 3AA irq 0 with +0x10c bit 0 (V SYNC), bit 2 (CIN ROW), bit 1
  (FRAME END) in Android's order. All return 0; V SYNC runs tasks ddk+0x1d4b58 / 0x1d2d90, FRAME END ddk+0x1ce714;
  the per-frame register update happens at CIN ROW (128 / 259 / 245 accesses for F1 / F2 / F3).
- DMA format found: the phone's 3AA output strides are 12-bit PACKED (+0x410 = 0x1140 = 2944 x 1.5, +0x460 =
  0xe40 = 2432 x 1.5) -> taa dma_output format 5 = DMA_OUTPUT_FORMAT_BAYER_PACKED (format 0 gave x 2 strides).
- Match (tools/isp-re/cmp_dump.py, excluding 0xdeadc0de and DMA addresses): 1562 comparable registers, 1183
  equal = 75.7 %. What still differs: 0x29xx / 0x37xx-0x39xx / 0x40xx and their 0x8000 shadow copies -
  thresholds about 0.6 x ours (AE / AWB dependent: the emulator has no image statistics, the phone had a lit
  scene); +0x18fc, 0x19fc ... low byte equal (0x37), upper bits = hardware counters; frame / config-lock state
  (0x0c, 0x28, 0xc8, 0x118). Remaining log noise: "ISP_IoCallback_3aa: CONFIG_LOCK_DELAY" each frame and frame
  number "F0" in the ISR lines (the ISR reads the frame counter from a register: live-dump value).
### ISP0 (M2M) frames and the per-frame cost (2026-11-17 22:30)
- isp_param_set (param_layouts.txt): taa_param -> the 3AA set, dma_input = 3AA after-BDS 2432 x 1824, format 5
  packed, 12 bit, msb 11; otf_output -> MCSC YUV422 8 bit (GPL v7 mcsp default, UNVERIFIED for this path);
  input_dva = the 3AA after-BDS buffer. ISP set_system_config added (Android does it after ISP0 create).
- ISP shot F1 -> 0 (1537 register accesses): "[ISP0][ConfigureISPInput]: fro(1) inputImageArea(0,0 / 2432x1824
  12/12bits)", "ISP CellResolution: 38 x 28", "HW: (0, 16) 2432 x 1792", "normalizeShift = 9", "uThreshClipCnt_ =
  532(128)", "TNR_MODE_BYPASS -> TNR_MODE_FIRST_RUN", "ControlClock clock 1 -> 1", "EOTF gamma disabled" - each
  identical to Android's lines :546-557. Different: "DrcMode change Prev:0 -> Cur:1" (Android Cur:3) and "Unit gain
  is set due to an empty histogram" (no image statistics in the emulator).
- ISP interrupt 6 reads ISP +0x10c: bit 0 V SYNC, bit 1 FRAME END (IRQ_SCAN_ISP=1 run); every other bit of +0x10c
  and all of interrupt 7's +0x11c have no handler in this mode (keep masked). Android's ISP order V SYNC -> FRAME
  END ("TransitIsrState I0 ... I1").
- Match after F1..F3 (cmp_dump.py): ISPLP 1032 comparable registers, 824 equal = 79.8 % (differences mostly in the
  0x9800-0xa800 shadow copy: tone / DRC / AE-dependent); 3AA0 now 1202 / 1562 = 77.0 %.
- Cost per steady frame (FRAMES=5, EMU_COUNT=1 block counter, timing.txt): 3AA set_ctrl 7.8k + shot 5.3k +
  V SYNC (with its tasks = RTA AE/AWB/AF) 2.62M + CIN ROW 0.16M + FRAME END 0.05M; ISP set_ctrl 3.2k + shot 2.06M
  + V SYNC 0.9k + FRAME END 29k instructions = about 5.0 M AArch64 instructions per frame -> ~150 M/s at 30 fps.
  Init (start_up .. object_create, once): ~3.6 M. (Wall times in timing.txt are Python-hooked Unicorn on the build
  machine - not a measure of anything on the phone.)
- Microsoft's ARM64 ABI page (learn.microsoft.com/en-us/cpp/build/arm64-windows-abi-conventions) on x18: "Reserved
  platform register: in kernel mode, points to KPCR for the current processor; In user mode, points to TEB";
  volatility "N/A"; it does not spell out a "must not modify" rule beyond "Reserved".
## Stage 4 decision point (2026-11-17 22:35) - how the ISP runs under Windows
Facts now known: the libraries run their real front-camera path (init, setfile, calibration, 3AA + ISP frames)
when given (1) register windows, (2) DMA buffers with 32-bit addresses, (3) interrupts, (4) the GPL OS services and
(5) the GPL sensor interface; ~5 M AArch64 instructions per frame (~150 M/s at 30 fps); code is EL0-safe but uses
x18 freely.
- A. Native host in a user-mode process (UMDF 2 driver or a service + our kernel driver for MMIO / IRQ / DMA):
  the blob runs at full speed; every entry into the blob and every OS callback out of it swaps x18 (blob's value
  <-> TEB). Open points: no Windows code may run on that thread while the blob's x18 is live (no APC, no
  exception, no profiler walk - our thunks must cover every exit), register access from user mode (UMDF user-mode
  register mapping), interrupt latency at PASSIVE level, SysMMU state for the ISP DMA (still UNVERIFIED).
- B. The emulator on the phone: no x18 issue, but ~150 M instructions/s to emulate - not credible on the S10 in
  real time.
- C. No blob at runtime: the emulator generates the register programs offline (init + per-frame set for a chosen
  set of exposures / scenes); a Windows driver replays them and runs a small AE of its own on the sensor. Android-
  grade pipeline (debayer, denoise, DRC, scaler), static tuning, no Samsung code on the phone at runtime.
Every option needs the hardware facts first (ISP clocks / power, SysMMU bypass, 3AA DMA working under Windows),
so the next hardware step is the same for all three.
## Stage 4, step 1: can Windows reach the ISP? (S10eCam 0.11 / cam-s10.ps1 v1.7 "isp", 2026-11-17 22:50)
Common to options A / B / C: the 3AA0 must answer Windows' reads. Known (notes/s10-camera.md 2026-11-16): inside
pd-isppre Windows reads CMU_ISPPRE, CSIS2, CSIS DMA and SYSREG with the domain as sboot left it (no SMC), but the
ISPPRE SysMMU's frame (0x17210000) answered with an external abort (bugcheck 0x124 P1 0x11).
- New deep step 8 (driver 0.11): reads CMU_ISPPRE's 3AA0 aclk gate +0x201C (cmucal-sfr.c:1348) and QCH +0x3034
  (:2380), and only if the gate and LHM_AXI_P are on, the 3AA0 (0x17190000, live DT fimc_is reg #1) ID registers
  +0xF0 / +0xF4, compared with Android's 0x07010000 / 0x0000CC22. Two reads, no writes.
- RISK stated to the user: if 3AA0 is protected like the SysMMU, the read aborts -> bugcheck 0x124 + reboot (no
  other effect; WINPRAM ring + the write-through log name the step).
- cam-s10.ps1 v1.7: action "isp" = that one step (the 3step loop stays 1..7). kcheck-wdm.sh S10eCam.c: no errors;
  psparse cam-s10.ps1: 0 parse errors. NOT run on the phone yet.
- 2026-11-17 22:55 DECISION (user): option A - run the real libraries natively in a user-mode host on Windows,
  swapping x18 at every entry into / exit from them. Plan: (1) a portable host core in C (loader + pointer
  relocation, the GPL OS-service table, sensor interface, task threads, DMA / register / interrupt through a
  platform layer); (2) prove it on AArch64 Linux user mode (qemu-aarch64 on the build machine) with fake register
  windows = the same run as front_init.py, natively compiled; (3) the Windows platform layer (UMDF 2 or service +
  kernel helper for MMIO / IRQ / contiguous DMA) - Microsoft docs first for the register / interrupt / DMA parts.
## Stage 4 option A, step 1+2: tools/isp-host - the libraries run NATIVELY (AArch64 Linux user mode, 2026-11-17)
- Core isphost.c (portable C) + plat_posix.c (Linux test platform) + test_front.c (= front_init.py, natively).
  Built static with zig cc -target aarch64-linux-musl, run under qemu-aarch64 on the build machine. No emulator.
- Loader: one RWX block (0x7000000 + 0x20000, 2 MiB aligned), DDK file at +0, RTA file at +0x520000; every 8-byte
  aligned word in [LIB_START, LIB_START + 0x7000000) gets + (base - LIB_START): 12005 words relocated. 698 other
  "kernel-half" words (0xffffff8..) outside the span exist (577 DDK, 121 RTA): all look like small negative numbers /
  masks (0xffffffffffff0000, 0xfffffffe01000000 ...), left untouched.
- x18: tramp.S (reference source; isphost.c emits the same words at run time): isp_enter saves the host x18 around
  every call into the libraries; each host function handed in gets a 48-byte trampoline (target, context word,
  isp_common); isp_common saves the library's x18, finds the calling thread's host x18 by stack range in a thread
  table, calls the host function with x0..x7, restores. All library code runs on 9 core-created threads (control,
  interrupt, timer, 6 task workers = GPL fimc_is_init_ddk_thread priorities 11..16), each registered.
- OS services: GPL semantics, incl. two points the emulator had wrong / missing: get_usec(uint64_t *time) WRITES
  *time (GPL :1467; the DDK passes a stack pointer, seen at ddk+0xf48d8 - the emulator returned the value instead),
  get_binary_version(char **buf, type, hint) (DDK [91] / RTA [91]) = the 44-byte version strings carved like GPL
  carve_binary_version (DDK "...2021/04/05 16:59:12]L12XLOD01GM", RTA "...09/26 04:52:21]L12XLOD01GM").
- First native run (FRAMES=3, real front calibration): DDK start_up, VRA, RTA start_up, both sensor-interface
  registrations, 3AA / ISP chain_create, set_system_config, object_create, 29 create_tune_set, apply 6, load_cal_data,
  get_cal_data (centerX/Y 1888/1400 biquadA/B 77828/16384 = Android's numbers), set_param / set_ctrl / shot and the
  V SYNC / CIN ROW / FRAME END (3AA) and V SYNC / FRAME END (ISP) handlers for 3 frames: every call -> 0, 17 tasks
  run on the workers, 15 interrupts. The only crash on the way was in the test itself (a local "SHOT" shadowing the
  enum index - now -Wshadow).
- Native vs emulator, same inputs (FRAMES=3, real calibration; tools/isp-host/cmp_emu.py on test_front out vs a
  fresh front_init.py run): all 38 steps return 0 in both; register ACCESS sequences (every R/W, offset, value, in
  order) identical in 35 / 38 steps - 3AA chain_create (333 accesses), object_create, all shots, ISP chain_create,
  all ISP frames. The 3 others (F1..F3 irq CIN ROW) differ only by 3 writes the emulator repeats and the host does not:
  W 3AA0 +0x8070 = 1, +0x8068 = 0, +0x8064 = 2 at ddk+0x2a442c..34 - the SAME values both runs already wrote in
  3AA shot F1. The writer (ddk+0x2a43a4, called through a vtable entry at file 0x3d48d8) skips when a 16-byte
  parameter equals its cached copy (bl 0xe0ec4, memcmp-like); inferred: the parameter carries uninitialised (stack)
  bytes, so "equal" depends on leftover stack contents. Ruled out by diagnosis switches (env, test only):
  DIAG_EMU_USEC (get_usec as the emulator modelled it), DIAG_IN_IRQ0 (in_irq always 0), DIAG_RANDOM0 - none changes
  it. Final register-window contents: identical except the modelled self-clearing registers (+0x0c, +0x3d2c).
- Log lines: identical except time-dependent ones. The emulator never wrote get_usec's *time, so its "ISR V SYNC
  F0 0~0 (0 us)" / "FT 0" lines were artefacts; natively "[S0][3AA0] Valid Time .. / Frame Time .." and the ISR
  durations appear like on Android ("Valid Time 7613 / Frame Time 8251", isfw-msg-android.txt). One more line differs
  with no time involved: "Error SIspStandAloneObject!!! nextFrameAnalogGain(4825) / (50962)" (emulator / host) - it is
  an error line in both runs and absent on Android (our standalone ISP M2M parameters, not a host issue).
- Register windows in the test = tools/isp-host/mmio_trap.c: an inaccessible mapping + SIGSEGV handler that decodes
  LDR/STR (imm12, imm9 unscaled / pre / post, register offset) and LDP/STP, performs the access on the dump-filled
  shadow, logs it like the emulator and models the polled self-clearing registers deterministically (a first try with
  a polling thread lost the race on the 2-CPU build machine: "[COREX]sw_reset is time-out", its poll limit is 1001
  reads). No undecoded instruction was hit in the whole run. Test-only diagnosis tool.
- Review of 916de6f / 14fda9d (fixed in ff7f0c5): mmio_trap's load/store masks did not test bit 26 (V) - a SIMD
  access would have been decoded as an integer one (none occurred: every access matched the emulator) -> masks
  0x3f..., PRFM skipped; README + build.sh were missing; "os calls" counted only traced calls -> traced_calls; a
  leftover variable. Relocation completeness, checked: the 12005 relocated words = 11395 (lib) + 610 (rta) 8-aligned
  words in the span, and there are 0 such words at 4-mod-8 offsets in either file (no misaligned pointer missed);
  an instruction pair can not produce one (high word 0xffffff80 is not an allocated A64 encoding).
- Timing (qemu-aarch64 on the 2-CPU x86 build machine, FRAMES=30; meaningless for the phone, recorded as a baseline):
  trapped access 6.5 us; steady frame (3AA set_ctrl + shot + 3 interrupts, ISP set_ctrl + shot + 2 interrupts, all
  tasks drained) 76 ms wall, 1151 register accesses (7.5 ms of it trapping).
- Phone run prepared: tools/android/run-isphost-s10.sh v1.0 + the same static test_front (stripped) in
  isphost-s10.tar.gz: copies the three libraries from /vendor into /data/local/tmp/isphost, runs FRAMES=30 against
  the dump-filled fake windows (no hardware, no /dev/mem), packs out/ to /sdcard/s10cam/isphost-out.tar.gz and
  deletes the copied libraries. Gives: the host on real silicon (not qemu) + the real per-frame cost. NOT RUN YET.
## Stage 4 option A, step 3: Windows (2026-11-17 22:20)
- Microsoft docs first (AGENTS.md 13):
  - WdfDeviceGetHardwareRegisterMappedAddress (learn.microsoft.com/.../wdfdevice/nf-wdfdevice-
    wdfdevicegethardwareregistermappedaddress): "A driver calls WdfDeviceGetHardwareRegisterMappedAddress to get the
    user-mode mapped address of the memory resource it mapped previously using WdfDeviceMapIoSpace" ... "it can
    access the user-mode address directly" ... "you must set the UmdfRegisterAccessMode INF directive to
    RegisterAccessUsingUserModeMapping"; "Not the recommended approach" (no UMDF validation); UMDF 2.0, PASSIVE_LEVEL.
    -> the libraries' plain pointer accesses to registers are possible from a UMDF 2 host process.
  - "Creating an interrupt object" (wdf/creating-an-interrupt-object): framework 1.11+ on Windows 8+ "KMDF and UMDF
    drivers can create interrupt objects requiring passive-level handling"; UMDF supports level-triggered,
    edge-triggered (exclusive) and MSI interrupts.
  - "Comparing UMDF 2 Functionality to KMDF" (wdf/comparing-umdf-2-0-functionality-to-kmdf): "the following features
    are available only to KMDF drivers ... Direct memory access (DMA)". -> the ISP's DMA buffers (3AA output, ISP
    input, the libraries' own taaisp / tnr / medrc pools) need a kernel-mode helper whatever hosts the libraries.
  - UNKNOWN that decides option A on Windows: whether Windows' kernel preserves a user thread's x18 when it is not
    the TEB (preemption, interrupts, migration). Not answered by the ABI page ("Reserved platform register ... In user
    mode, points to TEB", volatility "N/A"). Per AGENTS.md 11 no Windows binary is examined: a behavioural test
    instead -> tools/isp-host/x18test.c (16 threads keep a magic value in x18 for 2e9 checks each, more threads than
    cores so they are preempted and migrated; any change is reported and compared with the TEB). POSIX sanity run
    under qemu: preserved (as expected on Linux).
- plat_win.c (Win32, documented APIs only: VirtualAlloc RWX, FlushInstructionCache, SRW locks + condition
  variables, CreateThread with a COMMITTED stack - the libraries are kernel code without stack probes -,
  GetCurrentThreadStackLimits for the trampolines' thread table, NtCurrentTeb() as the host x18, QPC) + mmio_plain.c
  (plain-memory windows + a TIME_CRITICAL thread modelling the self-clearing registers: on Windows an access fault
  inside the libraries cannot be used for trapping, the exception dispatcher would run with the libraries' x18).
  Cross-built with zig -target aarch64-windows-gnu (UCRT only, no extra DLLs): isphost-test.exe, x18test.exe.
  The same plain variant on Linux/qemu: runs through, one "[ERROR][3AA]sw_reset is time-out" (the model thread lost a
  race on 2 CPUs - the run goes on), as expected for that model.
- tools/s10-kit/isphost-s10.ps1 v1.0 (CRLF, psparse 0 errors): x18 | host | all; results C:\S10Prov\isphost.zip.
  The libraries must be copied by the user into C:\S10Prov\isp (from s10cam.zip\files): never shipped by us.
  NOT RUN YET.
- Review of 9492da5: (1) isphost-s10.ps1 'all' threw when the libraries were missing, before zipping the x18
  result -> v1.1 skips the host test with a message and still zips (fixed). (2) OPEN, for the hardware version:
  plat_win.c sleeps / timed waits use Sleep / SleepConditionVariableSRW (ms, default timer resolution ~15.6 ms) where
  the kernel's msleep is 1-2 jiffies (4-8 ms at HZ=250) - irrelevant for the fake-window test, to be replaced
  (high-resolution waitable timer, documented CREATE_WAITABLE_TIMER_HIGH_RESOLUTION) when real frames are timed.
  (3) mmio_plain's model thread spins at TIME_CRITICAL on one core for the whole run (test only, 8 cores).
- S10eCam 0.12 (2026-11-18): every stage-3 call (3, 3step, isp) also prints PMU pd-isplp (CONF +0x2480 / STATUS
  +0x2484) and pd-isphq (+0x2400 / +0x2404) - K cal-if/exynos9820/flexpmu_cal_local_exynos9820.h isphq_on :703,
  isplp_on :757 (exynos9820, not _evt0: CONFIG_SOC_EXYNOS9820_EVT0 is not set in the phone's kernel .config); DT
  pd-isplp@15862480 need_smc 0x17410204. Why: ISP0 (0x17430000) and MCSC (0x17440000), the libraries' front-camera
  ISP M2M + scaler, sit in pd-isplp; if Windows leaves it off, running them needs power-on + the TZPC SMC like
  pd-isppre. Read-only, PMU = always-on block already read by every stage-3 call. kcheck-wdm: S10eCam.c clean,
  S10eCamera.c (includes S10eCam.c) the same 25 pre-existing warnings before and after.

## Stage 4 option A, step 4: the Windows hardware path - design (2026-11-18 01:30)
Microsoft docs read for it (AGENTS.md 13):
- MmMapLockedPagesSpecifyCache (ddi/wdm/nf-wdm-mmmaplockedpagesspecifycache): AccessMode UserMode "returns a user
  address that is valid in the context of the process in which the driver is running"; "Callers that specify
  UserMode must wrap the call ... in a try/except block"; "A non-executable mapping is always created when AccessMode
  is UserMode"; IRQL <= APC_LEVEL; buffers mapped to user space must be zeroed and page-multiple. "For mappings to
  user space, MDLs that are built by the MmBuildMdlForNonPagedPool routine can be used."
- MmBuildMdlForNonPagedPool (ddi/wdm/nf-wdm-mmbuildmdlfornonpagedpool): "the specified MDL must describe a buffer in
  nonpaged system memory ... such as memory that is allocated by ... MmAllocateContiguousMemorySpecifyCache". The
  page says nothing about I/O space -> mapping DEVICE REGISTERS into a process this way is NOT documented; contiguous
  DMA memory IS.
- So, documented split: registers + interrupts through UMDF 2 (WdfDeviceMapIoSpace +
  WdfDeviceGetHardwareRegisterMappedAddress with UmdfRegisterAccessMode = RegisterAccessUsingUserModeMapping;
  passive-level interrupt objects), DMA memory through a kernel helper (MmAllocateContiguousMemorySpecifyCache below
  4 GiB -> MmBuildMdlForNonPagedPool -> MmMapLockedPagesSpecifyCache(UserMode) in the host process's context, i.e.
  from its IOCTL handler; physical address = the 32-bit address the ISP DMA uses while its SysMMU is bypassed).
- UMDF driver host (wdf/umdf-driver-host-process): Wudfhost.exe runs as LocalService; the page says nothing about
  process mitigation policies -> whether VirtualAlloc(PAGE_EXECUTE_READWRITE) works inside Wudfhost is UNKNOWN and
  must be tested there (a UMDF probe), as must x18 (x18test covers ordinary processes only).
Hardware facts for it, verified:
- Resources: DT fimc_is@17190000 (live.dts:4747) reg: CSIS_DMA 0x170E0000+0x100, 3AA0 0x17190000+0x10000, 3AA1
  0x172A0000, ISPLP (= DEV_HW_ISP0) 0x17430000, ISPHQ 0x17730000, MCSC 0x17440000, VRA 0x17630000 (+0x10000 each),
  STAT_DMA 0x17140000+0x100. interrupts (SPI): 0x13F 0x140 0x141 0x142 0x128 0x129 0x126 0x127 0x12B 0x12C 0x1AD;
  K fimc-is-v7_0_0/fimc-is-hw-chain.c:1536.. (CONFIG_FIMC_IS_V7_0_0=y) maps them by index: 0/1 3AA0 INTR_HWIP1/2,
  2/3 3AA1, 4/5 ISP0, 6/7 ISP1, 8 MCSC0, 9 MCSC1, 10 VRA. GSIV = SPI + 32: 3AA0 351 / 352, ISP0 328 / 329,
  MCSC0 331.
- The library registers (chain 0, id 0 / 1) for 3AA0 and (chain 2, id 6 / 7) for ISP0 (native run's log); the
  kernel ISR (K fimc_is_isr1_ddk / isr2) only calls handler(id, ctx) between enter/exit_lib_isr: the library's
  handler reads and clears the status itself -> a UMDF passive ISR = isp_irq_sync(chain, id). Until that runs, an
  interrupt must NOT be connected (a level interrupt nobody clears would storm).
Plan (each a task, each waits for the result before it):
  1. x18test + isphost-test on Windows (sent, pending)          4. UMDF probe S10eIsp 0.1: DSDT device with the
  2. run-isphost-s10.sh on Android (sent, pending)                 memory resources only, maps them, reads nothing;
  3. S10eCam 0.12 isp: 3AA0 readable? pd-isplp on? (pending)       tests RWX + x18 inside Wudfhost
  5. kernel helper: contiguous DMA buffers mapped into the host  6. host on hardware: 3AA0 only (pd-isppre is on),
  7. pd-isplp power + TZPC SMC if needed, then ISP0 + MCSC        interrupts connected after the handler exists

## Stage 4 option A: the native host ON THE S10 (Android, 2026-11-18 13:58) - plan item 2 done
- run-isphost-s10.sh v1.0 by the user, FRAMES=30, real front calibration, fake (trapped) windows; result kept in
  notes/data/isphost-android-20261118/ (host.log, timing.txt; the mmio logs are identical to the qemu ones, below).
- Real silicon, not qemu: image at 0x761d400000, 12005 pointers relocated, DDK / RTA start_up 0, all 282 steps -> 0,
  no crash, no sw_reset time-out; 179 tasks, 150 interrupt handler runs, 0 timers fired.
- Same result as qemu, register for register: final 3AA0 / ISPLP windows after 30 frames identical to the qemu
  FRAMES=30 run (out4: 0 words differ); mmio logs of 3AA shot F30 and F30 irq CIN ROW identical; vs the emulator
  the same 35 / 38 steps identical, the same 3 redundant CIN ROW rewrites.
- Cost on the S10's CPU (wall time per frame = 3AA set_ctrl + shot + V SYNC / CIN ROW / FRAME END, ISP set_ctrl +
  shot + V SYNC / FRAME END, every task drained; Android running beside it, no core pinning): median 9.7 ms, p90
  13.7 ms, max 19.8 ms, min 5.8 ms over 30 frames; ~1300 register accesses per frame cost ~2.7 ms of it here
  (trapping: 2.12 us per access) -> about 7.6 ms per frame for the libraries + host = ~23 % of a 33.3 ms frame at
  30 fps. Heaviest steps: 3AA shot F1 26.3 ms, ISP object_create 21.7 ms (one-time); steady CIN ROW ~5.5 ms.
  Android's own "[S0][3AA0] Valid Time / Frame Time" lines appear with real numbers here too.
- Conclusion: the libraries run as ordinary AArch64 user code on this phone and are fast enough for 30 fps with
  margin. Remaining unknowns for Windows: x18 (x18test), Wudfhost RWX, hardware access (S10eCam 0.12 isp).

## Stage 4 option A: x18 on Windows ARM64 (2026-11-18 14:15) - plan item 1a done
- x18test.exe v1.0 on the S10 under Windows (user, isphost-s10.ps1 x18; notes/data/isphost-win-20261118/
  x18test.txt): 16 threads x 2e9 checks on 6 logical processors (Windows reports 6), ~6.8 s per thread, all running
  at once, so every thread was preempted and rescheduled many times: "X18 PRESERVED (0 of 16 threads saw x18
  change)". Observed behaviour: Windows' kernel keeps a user thread's x18 across interrupts / preemption /
  migration even when it does not hold the TEB. The main unknown of option A on Windows is closed (behavioural
  test only, AGENTS.md 11; what it does NOT cover: exceptions or APCs delivered while the libraries' x18 is live -
  the host never lets that happen on purpose).
- The host test (isphost-test.exe) was not in this zip: the libraries were not yet in C:\S10Prov\isp (s10cam.zip
  sent back to the user for that).

## Stage 4 option A: the native host on WINDOWS ARM64 (2026-11-18 14:18) - plan item 1b done
- isphost-test.exe (zig aarch64-windows-gnu, plat_win.c + mmio_plain.c) on the S10 under Windows, user run of
  isphost-s10.ps1 host, FRAMES=30, libraries from s10cam.zip\files, NO calibration file (rom1_cal.bin was not in
  C:\S10Prov\isp -> zero buffer); notes/data/isphost-win-20261118/ (host.log, timing.txt).
- Image at 0x1D910000000, 12005 pointers relocated, DDK / RTA start_up 0, all 282 steps -> 0, no crash, no
  sw_reset time-out (the model thread kept up), 179 tasks, 150 interrupt handler runs - same counts as Android.
- Against the Android run (same binary logic, calibration present there): every log line identical except the ones
  that come from the missing calibration ("FROM CAL. DATA LOAD: FAIL! (0x600f)", get_cal_data centerX/Y 1584/1208
  default instead of 1888/1400, AF cal Macro/Pan defaults, "Sensor_Revision = 0", AE "nextFrameAnalogGain"
  values) and addresses. Final registers: 3AA0 21 words differ, all in +0x1a3c..+0x1a70 (follow the calibration),
  ISP0 3: +0x3d24 (the model thread clears it, the trap model does not), +0xa208, +0xb038.
- Cost on Windows (no trapping here, so this IS the host + libraries): median 10.9 ms, p90 16.1, max 22.1, min 7.4
  ms per frame over 30 frames on 6 CPUs; one-time ISP object_create 35.8 ms. Fits 30 fps (33.3 ms).
- => Option A works on Windows ARM64 in an ordinary process: relocation, x18 trampolines, threads, timers, tasks.
  Next: hardware access (S10eCam 0.12 isp pending), then the UMDF probe (plan item 4).

## Windows reaches the ISP: 3AA0 ID read (2026-11-18 14:21) - plan item 3 (first half) done
- cam-s10.ps1 v1.7 isp with S10eCam 0.11 (the 0.12 build with the pd-isplp line was not installed yet);
  notes/data/isphost-win-20261118/cam-s10-isp.txt: pd-isppre ON (as sboot leaves it), ISPPRE_BUS gate on,
  3AA0 aclk gate [+201C] 00200000 on, QCH [+3034] 003F0002, LHM_AXI_P on;
  3AA0 +0xF0 = 0x07010000, +0xF4 = 0x0000CC22 = exactly Android's values (3AA v7.1, ISP_Fimc3aaChainV7P1P0). No
  abort, no bugcheck: the 3AA0 register block is readable from Windows with the clocks as sboot leaves them, no SMC.
  (The phone's clock said 2026-11-01 in the log header: RTC, irrelevant.)
- Still open: pd-isplp state (ISP0 + MCSC) -> S10eCam 0.12.

## Plan item 4: S10eIsp 0.1, the UMDF 2 probe (2026-11-18 14:30)
- drivers/S10eIsp: S10eIsp.c (UMDF 2.15, C), S10eIsp.inf (ACPI\SAMS982C; UmdfDirectHardwareAccess =
  AllowDirectHardwareAccess, UmdfRegisterAccessMode = RegisterAccessUsingUserModeMapping, UmdfHostProcessSharing =
  ProcessSharingDisabled, UmdfLibraryVersion 2.15.0, ServiceBinary %13% - directive text from
  wdf/specifying-wdf-directives-in-inf-files), S10eIsp.vcxproj (WindowsUserModeDriver10.0 like IddSampleDriver).
  build-s10.cmd v1.19 :isp (msbuild -> dll + inf -> kit catalog + signature), make-kit.sh ships the source,
  tools/s10-kit/isp-s10.ps1 v1.0 (install / status / restart / remove; reads the report from the registry).
- What the probe does in PrepareHardware (inside its own Wudfhost): maps every memory resource (WdfDeviceMapIoSpace
  + WdfDeviceGetHardwareRegisterMappedAddress); reads CMU_ISPPRE +0x201C (3AA0 aclk gate) and +0x20B4 (LHM_AXI_P)
  both with WDF_READ_REGISTER_ULONG (wdfhwaccess.h: WdfDeviceReadFromHardware) and with a plain load from the user
  mapping; if both gates are on, reads 3AA0 +0xF0 / +0xF4 from the user mapping; never reads ISPLP / MCSC; reports
  GetProcessMitigationPolicy(ProcessDynamicCodePolicy), tries VirtualAlloc(PAGE_EXECUTE_READWRITE) and runs the
  x18 check code on 8 threads x 1e8 there. Report: REG_SZ "Probe" under the device's hardware key subkey
  (PLUGPLAY_REGKEY_DEVICE | WDF_REGKEY_DEVICE_SUBKEY, KEY_READ | KEY_SET_VALUE - the only writable one for UMDF,
  WdfDeviceOpenRegistryKey docs). No interrupt, no register write.
- Syntax check (no Windows build here): clang aarch64-pc-windows-msvc against the WDK 26100 um + UMDF 2.15 headers
  (scratch ucheck-umdf.sh, a lower-case symlink overlay for the headers' mixed-case includes, km/crt as CRT): clean.
  First version used READ_REGISTER_ULONG - not declared for UMDF; the UMDF form is WDF_READ_REGISTER_ULONG(Device,
  reg). The real build (msbuild, linking WdfDriverStubUm) happens on the user's PC: NOT BUILT YET.
- UEFI beyond1lte v0.2.3 (2026-11-18 14:30): DSDT Device (CISP) _HID SAMS982C, S10 only (#if
  EXYNOS9820_BOARD_BEYOND1): _CRS = CMU_ISPPRE 0x17000000/0x4000, 3AA0 0x17190000, ISPLP 0x17430000, MCSC 0x17440000
  (0x10000 each), no interrupt; _STA = PMU 0x15862504 bit 0 (pd-isppre ON) read through an AML OperationRegion (no
  resource claimed - GPU0 owns the PMU page in its _CRS). The addresses are now in Exynos9820.h (EXYNOS_CMU_ISPPRE_*,
  EXYNOS_ISP_*, LIVE-READ / SOURCE-ONLY marked). Build Beyond1 DEBUG rc 0 (FVMAIN_COMPACT 99 %, 3408 B free),
  iasl -d of the built Dsdt.aml shows the node as written; Beyond0 (S10e) DEBUG rc 0, its Dsdt.aml has no SAMS982C.
  Image beyond1lte-uefi-v0.2.3.img 7716864 B sha256 6b083fc08fdc69b6ded2bf5a268f01d11baf6e07bc3b27cd9515459a48dd2ef3
  (also carries the WINDOWS_WATCHDOG 0 change, next commit). NOT FLASHED.
- Review of 352bd98 / de7cc6f / e6dd990: (1) the AML OperationRegion reads 0x15862504, inside GPU0's _CRS page
  0x15862000 - whether Windows' ACPI driver lets AML read a range a loaded driver owns is NOT documented in what was
  read; if it refuses, ISPS reads 0 and CISP stays hidden (fails safe), the symptom would be "device NOT PRESENT"
  with pd-isppre ON in cam-s10 - then gate _STA through a UEFI progress word like GPU0. (2) install-in-windows /
  make-windows take everything in out\: S10eIsp would be installed by them too; harmless (binds only CISP, reads only
  the gates and the 3AA0 IDs; ~0.2 s x18 check at each start). (3) DSDT/_CRS order and S10eIsp.c RES_* agree
  (CMU_ISPPRE 0, 3AA0 1, ISPLP 2, MCSC 3), and S10eIsp.c also checks the two physical addresses before reading.
- 14:36 user build (EWDK, build-s10.cmd v1.19): S10eIsp.vcxproj failed MSB4019 "The imported project
  C:\Microsoft.Cpp.Redirect.18.props was not found" ($(CurrentVCTargetsPath) empty): the IddSample-style header
  (ToolsVersion 12.0, no Microsoft.Cpp.Default.props before the configuration group). Fix: the shape of the kit's
  KMDF projects that build in the same EWDK (S10eBattery.vcxproj: ToolsVersion Current, WindowsTargetPlatformVersion
  $(LatestTargetPlatformVersion), Microsoft.Cpp.Default.props, then PlatformToolset WindowsUserModeDriver10.0 /
  DynamicLibrary / DriverType UMDF).

## S10eIsp 0.1 on the phone (2026-11-18 14:42) - plan item 4 DONE
- User: UEFI v0.2.3 flashed, kit built with the fixed vcxproj, isp-s10.ps1 install
  (notes/data/isphost-win-20261118/isp-s10-install.txt): ACPI\SAMS982C\0 present (so the AML read of the PMU word
  inside GPU0's page works and pd-isppre was ON), status OK, oem10.inf 0.1.0.0, one Wudfhost (process 1332).
- Every memory resource is mapped into the process: CMU_ISPPRE -> 0x1FA7B0D0000, 3AA0 -> 0x1FA7B430000, ISPLP ->
  ...440000, MCSC -> ...450000 (kernel pseudo addresses FFFFE401...).
- CMU_ISPPRE 3AA0 aclk gate 00200000 / LHM_AXI_P 00200000 through WDF_READ_REGISTER_ULONG and through the user
  mapping: the same. 3AA0 +0xF0 07010000 / +0xF4 0000CC22 read with a plain load from the user mapping = Android.
- Wudfhost: ProhibitDynamicCode 0; VirtualAlloc(PAGE_EXECUTE_READWRITE) ok; generated code ran on 8 threads,
  "X18 PRESERVED".
- => Everything option A needs from Windows for the registers is there, documented path only (UMDF 2 user-mode
  register mapping). Left: DMA memory (plan item 5, kernel helper), interrupts (after the handler exists),
  pd-isplp (S10eCam 0.12 not run yet). ("firmware:" empty in the log: isp-s10.ps1 reads HARDWARE\...\BIOS
  BIOSVersion, absent here - cosmetic.)

## 2026-11-18 17:22 cam-s10 isp again: still S10eCam 0.11
- notes/data/isphost-win-20261118/cam-s10-isp-2.txt: the header still says "S10eCam 0.11" and no pd-isplp line:
  cam-s10.ps1 runs the already registered C:\S10Prov\cam\S10eCam.sys; a new build only takes effect after
  `.\cam-s10.ps1 install` (copies out\S10eCam\S10eCam.sys, sc config). pd-isplp still unknown.

## Plan item 5: S10eIspDma 0.1 - DMA pool for the host (2026-11-18 17:40)
- Docs (in addition to the item-4 ones): MmAllocateContiguousMemorySpecifyCache (ddi/wdm/nf-wdm-
  mmallocatecontiguousmemoryspecifycache): nonpaged, physically contiguous; "uninitialized. A kernel-mode driver
  must first zero this memory if it is going to make it visible to user-mode software"; "A driver that must use
  contiguous memory should allocate this memory during driver initialization"; freed with MmFreeContiguousMemory;
  IRQL <= DISPATCH_LEVEL. MEMORY_CACHING_TYPE (ddi/wdm/ne-wdm-_memory_caching_type): MmWriteCombined "should not be
  cached by the processor, but writes ... can be combined"; "if a driver maps two different virtual address ranges
  to the same physical address, it must ensure that it specifies the same caching behavior for both"; NOTHING about
  how ARM64 maps the types (Normal-NC vs Device) -> tested on the phone (S10eIsp 0.2).
- Size from the native run (TRACE=1, FRAMES=2): the libraries' own dma_alloc calls total ~45 MiB (2 x 0x1171200,
  2 x 0x2e8300, 0x3b020, 0x8ca10, 0xe2220, 32 x 0x2740, 2 x 0xfb0), no dma_free; the 3AA output buffers of the test
  add ~22 MiB. Default pool 128 MiB (registry Parameters\PoolMiB 16..512), halved on failure.
- drivers/S10eIspDma: S10eIspDma.c (legacy WDM like S10eCam; pool in DriverEntry, WC then NC, below 4 GiB, zeroed;
  MDL by MmBuildMdlForNonPagedPool; IOCTL_S10EISPDMA_MAP maps it UserMode (non-executable, MdlMappingNoExecute) in
  the caller's context under __try; one owner handle; IRP_MJ_CLEANUP of that handle in the owner process unmaps and
  re-zeroes; ACL SY + BA + LS = the Wudfhost account), S10eIspDmaIoctl.h, build.bat (S10eCam's toolchain).
  build-s10.cmd v1.20 :ispdma, make-kit ships it, isp-s10.ps1 v1.1 dma / dmaremove (sc create start= system).
  kcheck-wdm clean. Known gap: if the last handle to the owning file object is closed in ANOTHER process
  (duplicated handle), CLEANUP does not unmap (wrong context) - the host never duplicates it.
- S10eIsp 0.2 (same day): after the 0.1 probe, opens \\.\S10eIspDma, maps the pool, logs PA / size / caching, and
  runs under __try the access kinds the libraries use on DMA memory: aligned and unaligned (+0x1003) 8-byte store +
  load, memset 64 KiB, DC ZVA (generated code; DCZID_EL0 read the same way, DZP checked), last-byte access; then
  closes the handle (unmaps). Device memory would fault on the unaligned / DC ZVA ones; Normal-NC would not.
  ucheck-umdf clean. INF DriverVer 0.2.0.0.
