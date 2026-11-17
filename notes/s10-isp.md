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
