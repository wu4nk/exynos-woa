# Galaxy S10 (beyond1lte, SM-G973F) — port from the S10e tree

Started 2026-11-02. Device: WU4NK's own SM-G973F, bootloader `G973FXXSGHWC2`, rooted
(KernelSU-Next), running an S22U-props port ROM on `4.14.356-openela-rc1-ExtremeKernel`.
`ro.boot.em.model=SM-G973F`, live DTB model string
`Samsung BEYOND1LTE EUR OPEN 26 board based on EXYNOS9820`.

All reads below were taken by `s10-collect.sh v0.1` as root **in Android** (not TWRP)
and pulled as `s10-dump.tar.gz`. The live DTB is `/sys/firmware/fdt` (the
sboot-patched tree, i.e. what our UEFI will face), decompiled locally with the
`fdt` Python package. Nothing was written to the phone.

## 1. What the S10 shares with the S10e (read, not assumed)

| Item | S10 read | S10e value in tree | Same? |
|---|---|---|---|
| SoC / MMIO | `compatible = samsung,exynos9820-*` throughout | Exynos9820.h | yes |
| FB base | bootargs `s3cfb.bootloaderfb=0xca000000` | `EXYNOS_FB_BASE 0xCA000000` | yes |
| DRAM bank0..4 | memory@80000000 `0x3AB00000`, @C0000000 `0x20000000`, @E1900000 `0x1E700000`, @880000000 `0x80000000`, @900000000 `0x80000000` | same five | yes |
| sec_debug_magic, kaslr, el2_code, mm_debug, secure_camera, camera_contig, seclog, modem_spare, modem_if, cp_ram_logging, sec_rdx_bootdev | reserved-memory regs | same bases/sizes | yes |
| Touch | `hsi2c@104B0000/touchscreen@48`, `sec,sec_ts`, IRQ `gpa1-0` (attn-irq, func 0xF, pud 0), bus pins `gpp1-4/5` func 2, `max_coords 4096x4096`, rails `tsp_io`=LDO19M 1.8 V, `tsp_avdd`=LDO27M 3.0 V | identical (only `afe_base` 0x206 vs 0x205, `firmware_name y771_beyond1_phole.bin`) | yes |
| Audio | `spi@10470000/cs47l93@0`; `hsi2c@109A0000/cs35l40@40` + `@41` | same | yes |
| Fuel gauge | max77705, `fuelgauge,fg_resistor = 2` | `MAX77705_FG_RESISTOR_UNVERIFIED 2` | yes (now a read on the S10) |
| UFS disk | sda 31 236 096 × 4096 B, LastUsableLBA 31 236 090, AltLBA 31 236 095 | same geometry | yes |
| BOOT / RECOVERY / EFS | sda14 27264..41343, sda15 41344..57855, sda3 4096..9215 | same LBAs | yes |

## 1.2 What differs

| Item | S10 | S10e |
|---|---|---|
| Panel | 1440 × 3040 (`dumpsys display` real size; fb0 stride 5760 B @ 32 bpp; lcd_type `SDC_A10013`) | 1080 × 2280 |
| RAM | 8 GB: extra `memory@980000000 reg = <0x9 0x80000000 0x80000000>` | 6 GB |
| el2_earlymem | `0x9FD800000 + 0x2800000` | `0x97E000000 + 0x2000000` |
| camera (ION) | `0x99000000 + 0x1B800000` (custom kernel's DT; not used by any code) | `+0xE400000` |
| Battery | `fuelgauge,capacity 3273` (3300 mAh cell, `charge_full_design 3300000`) | 3100 mAh |
| Partitions | 34 on sda (see §2) | 31 |
| Backup GPT | **present**: header at LBA 31236095, entries 31236091..31236092 non-zero | none (tail all zero) |
| Free tail | **none**: USERDATA ends exactly at LastUsableLBA 31236090 | 31234816..31236090 unclaimed |

## 2. The S10's GPT (parsed from sda LBA 0..5, 4096-byte LBAs)

```
sda1  bota0      1024..2175      sda18 persistent  70912..71039
sda2  bota1      2176..4095      sda19 misc        71040..71295
sda3  efs        4096..9215      sda20 keyrefuge   71296..75391
sda4  param      9216..11263     sda21 keydata     75392..79487
sda5  up_param  11264..13311     sda22 dqmdbg      79488..83583
sda6  keystorage 13312..13439    sda23 vbmeta      83584..83599
sda7  sec_efs   13440..18559     sda24 bota3       83600..83967
sda8  bota2     18560..20607     sda25 system      83968..2131967
sda9  steady    20608..21631     sda26 vendor    2131968..2541567
sda10 uh        21632..22143     sda27 product   2541568..2951167
sda11 uhcfg     22144..23167     sda28 odm       2951168..2952191
sda12 dtb       23168..25215     sda29 prism     2952192..3110911
sda13 dtbo      25216..27263     sda30 optics    3110912..3118591
sda14 boot      27264..41343     sda31 cache     3118592..3272191   <- ESP (same 153 600 LBAs as the S10e's)
sda15 recovery  41344..57855     sda32 omr       3272192..3284991   refused
sda16 radio     57856..70655     sda33 cp_debug  3284992..3286271   refused
sda17 nad_refer 70656..70911     sda34 userdata  3286272..31236090  <- Windows
```

sdb (8192 × 512 B), sdc (8192 × 512 B) carry no partitions; sdd1 = `cpefs`.

**Partition NUMBERS differ from the S10e** (S10e: CACHE sda28, USERDATA sda31). On the
S10, sda28 is `odm` and sda31 is `cache`. Every S10e tool addresses partitions by
**name** (`/dev/block/by-name/cache`, `.../userdata`, `.../boot`) — checked with
`rg 'sda[0-9]' firmware/tools` — so the names carry over; anything that hard-codes an
`sdaN` number must not be reused on the S10.

## 3. Progress log

- 2026-11-02 — dump received and parsed (§1, §2).
- 2026-11-02 — firmware board switch: `Exynos9820Board.h` + `Beyond1LtePkg` (board
  variant DSC/FDF over the Beyond0LtePkg modules). S10e regression: built Beyond0LtePkg
  before/after with the Linux CLANGDWARF flow; every module byte-identical except
  `PeilessSec.efi`, whose only difference is one `ASSERT` line-number immediate
  (438 → 458, `llvm-objdump -d` diff). Bootshim: S10e binaries byte-identical with
  diag on and off.
- 2026-11-02 — `Beyond0LteFbSerialPortLib` `STATIC_ASSERT (ROWS * CELL_H < HEIGHT)`
  fired for 3040 (exact multiple of 32): ROWS is now `(HEIGHT - 1) / CELL_H`
  (S10e unchanged at 71, S10 94).
- 2026-11-02 — GPT headers on the S10: primary AND backup both `HeaderSize = 92`, CRCs
  valid, backup entries byte-identical to the primary (CRC 0x8959027D). The UFS driver's
  in-flight 512→92 translation (`UfsTranslateGptHeaderBlock`) ignores a 92-byte header,
  so it is a no-op here.
- 2026-11-02 — UFS driver: `-Board Beyond1` (build-ufs.ps1) → `/D EXYNOS9820_BOARD_BEYOND1=1`.
  Fence 3118592..31236090 (cache start .. LastUsableLBA), protected A = omr
  3272192..3284991, B = cp_debug 3284992..3286271, refusing-build run vendor..cp_debug
  2131968..3286271. **No unclaimed tail on the S10**: UfsDiag scratch / readout window
  are aimed inside cp_debug (refused by range B, C_ASSERTed), and `UFS_READOUT_BOOT=1`
  is an `#error` on this board. Asserts compile-checked with clang `_Static_assert` on
  the extracted block for both boards, plus a negative test (cp_debug end +1 → fails).
  MSVC/WDK build NOT run here (Windows only).
- 2026-11-02 — not changed, with reasons: touch driver (same bus/IRQ/IC, raw 12-bit
  coords; the HID physical extent is still the S9+'s on the S10e too — S10 measured
  2.60 × 5.51 in from dumpsys dpi, vs 2.71 × 5.58 in the descriptor), battery driver
  (`fg_resistor = 2` is now a read on the S10; design capacity comes from the gauge).
- 2026-11-02 — S10 boot image: `tools/pack-uefi-beyond1lte.sh 0.1.0` → header v2, page
  2048, base 0x10000000, kernel = shim (512 B, S10 board) + FD (7 340 032 B), dummy
  ramdisk, DTB section = `Platform/Beyond1LtePkg/FdtBlob/beyond1lte.dtb` (edk2-exynos
  s10.dtb, sha256 5c1853…39d5, BEYOND1LTE EUR OPEN 26 — same board rev as the phone).
  board name `b1uefi-v0.1.0`, cmdline carries `exynos-woa=beyond1lte-uefi-v0.1.0`.
  Parsed back independently: FD at kernel+512 byte-equal, DTB at its page, FDT magic.
  7 716 864 B, fits BOOT (57 671 680 B).
- 2026-11-02 — RAM map simulated on the host from Exynos9820.h for both boards (same
  carve algorithm as PlatformMemoryMapLib): S10e 10 pieces / 5.25 GiB (Windows reported
  5.2 GiB on the S10e), S10 11 pieces / 7.23 GiB. Every S10 DT reservation the map
  treats as monitor/modem-owned is excluded, EXCEPT `kaslr`: the S10 DT has it at
  0x80001000 + 0x1000, the map carves `EXYNOS_RS_KASLR` 0x80100000 + 0x100000. The S10e
  tree carries both values (RS_ vs RSVD_) and its validated boots publish 0x80001000 as
  RAM too, so the S10 keeps parity — OPEN: settle which kaslr value is right.
- 2026-11-02 — S10e regression re-run after the ROWS change: still only the PeilessSec
  ASSERT line immediate differs.
- 2026-11-02 — installer: `PartitionMap.Beyond1Lte` (disk 127 943 049 216 B, USERDATA
  offset 13 460 570 112 B / 114 482 458 624 B = LBA 3286272..31236090, CACHE 629 145 600 B,
  BOOT/RECOVERY as S10e) and `S10eHarness --board beyond1lte` (geometry, SM-G973F model,
  S10 unattend names). **This phone's cache/userdata unique GUIDs are random**
  ({21ca1362-…0251}, {712bcd12-…ca08}), not Samsung's name-derived ones — they are
  per-device and go into the BCD. Compiled on Linux (.NET 8, net8.0 TFM in a scratch copy,
  Odin serial flasher excluded, CA1416 silenced — the real target is net8.0-windows) and
  ran: `--board beyond1lte` → geometry LBAs 3286272..31236090, cache 153 600 LBAs; a bad
  board name is refused. xUnit tests NOT run (NuGet blocked here).
  GptTypeService already handles a present backup GPT (entries at LastLBA-4) — the S10's
  layout satisfies its adjacency check.
- 2026-11-02 — UFS driver carries `UfsBoardIdentity` ("UFSBRD01", board 1/2, fence first
  LBA, protected run), kept by `/INCLUDE:UfsBoardIdentity` in build-ufs.ps1, so the flash
  kit can refuse a driver built for the other board. Encoding checked with clang
  (32-bit ULONG): S10e → 1, 1983488, 2137088, 2149887; S10 → 2, 3118592, 3272192, 3284991.
- 2026-11-02 — build-ufs.ps1 works inside an EWDK window: vcvarsall from VSINSTALLDIR,
  kit root from WDKContentRoot, version from Version_Number or the newest kit with
  km\ntddk.h (WU4NK's EWDK compiles against 10.0.28000.0, seen in their S20U build
  logs). Touch vcxproj targets `$(LatestTargetPlatformVersion)` like their S20U projects.
  Neither run here (no Windows / no PowerShell on this machine).
- 2026-11-02 — `tools/s10-kit/`: the S10 edition of WU4NK's own S20 Ultra (z3s) kit
  (make-windows v2.1 → v2.1-s10, set-esp-type v1.3 → v1.3-s10, z3s-disk.cs unchanged
  except one message, build-all.cmd → build-s10.cmd). Found while porting: the z3s export
  (`Z3s.Export.Plan`) requires a MiB-aligned partition, and the S10's userdata
  (114 482 458 624 B) is not — the NTFS volume is now 109178 MiB and the flash script's
  size check still uses the whole partition; the last 251 LBAs of userdata stay unused
  (a real free scratch run for UfsDiag later: LBA 31235840..31236090). Guards added:
  UFSBRD01 board record must say board 2 / fence 3118592; flash-windows.cmd and
  set-esp-type.ps1 refuse a bootloader that is not G973*; set-esp-type re-checks the
  cache/userdata GUIDs on the phone. z3s-disk.cs compiled with .NET 8; the PowerShell
  scripts were NOT parsed or run here (no PowerShell on this machine).
- 2026-11-02 — FIRST S10 WINDOWS BOOT (WU4NK): Windows boots; horizontal streaks over the
  regions the boot spinner redraws. Suspected: the framebuffer is a hole in the EFI memory
  map (UEFI maps it Device, the OS picks its own attributes) — Mu-Silicium/edk2-msm publish
  the panel buffer as "Display Reserved", write-through, and WU4NK's S20U (Mu-Silicium based)
  shows no streaks. v0.1.1: `BEYOND0LTE_FB_DISPLAY_RESERVED_WT` (default = S10 board):
  page table WT + resource HOB EFI_RESOURCE_MEMORY_RESERVED (attrs 0x1007, base 0xCA000000,
  0x10B3000) + EfiReservedMemoryType allocation HOB — checked in the PeilessSec disassembly.
  S10e PeilessSec unchanged except ASSERT line immediates. UNVERIFIED until booted; if the
  streaks only move with the animation and leave no residue, the cause is command-mode
  tearing instead (no TE sync) and this does not fix it.
- 2026-11-02 — WU4NK: the streaks start already over sboot's "Galaxy S10" logo, i.e. while
  our shim/UEFI runs, before Windows. Mu-Silicium has a beyond1lte target
  (Project-Silicium/Mu-Silicium, Platforms/Samsung/beyond1ltePkg): its S5E9820 PlatformSecLib
  writes 0x1281 to DECON + 0x70 (HW_SW_TRIG_CONTROL) and its memory map carries "Display
  Reserved" 0xCA000000 + 0x1400000, Reserv, WRITE_THROUGH. Our shim wrote the same 0x1281 to
  DECON + 0 (from the older edk2-exynos) — the prime suspect. v0.1.2: S10 shim writes +0x70
  (S10e shim byte-identical), plus TSP0 interrupt Level/ActiveHigh on the S10 (AML checked
  with iasl -d for both boards) for the new touch driver.
- 2026-11-02 — SecTsY761Touch (drivers/): WU4NK's z3s SecTsY79ATouch v0.2 (DIRQL ISR + work
  item, app-mode/touch-function/sense-on start, EID 3 handling) with the y761 event format
  (8-byte events, left_event mask 0x3F, 64-event FIFO — the S10e driver's values), ACPI\SCSY0761,
  S10 HID extent 2.60 x 5.51 in. Reason: the S10e driver connects a PASSIVE-level ISR to a GIC
  SPI (gpa1-0 = SPI 13), which the S20U work had already replaced. The parser reads only bytes
  0-3 and 7 of an event, identical in both formats. Not compiled here (WDK).
- 2026-11-02 — kit v1.1: S10's key GPIOs equal the S10e's (live DT gpio_keys: gpa0-3/4/6,
  gpa2-4), MAX77705 on hsi2c@15D60000, WLAN enable gpg0-5 / PERST gpf2-2 / CLKREQ gpf2-0 —
  so S10eDiag+S10eAgent, S10eBattery and Exynos9820WlanCx are used unchanged. build-s10.cmd
  builds them (S10eDiag build.bat made EWDK-aware), get-wifi-fw.cmd pulls the phone's own
  BCM4375 blobs, make-windows v2.2-s10 places blobs + SetupComplete.cmd (key agent),
  install-in-windows.ps1 adds everything to an installed Windows.
- 2026-11-02 — audio: S10 live DT = S10e values (cs47l93@0 on spi@10470000, reset gpg1-0, supplies
  LDO17M vdd_pmic_codec_1p8 / LDO18M vdd_codec_1p2; cs35l40@40 "cs35l40-bot" reset gph1-1,
  cs35l40@41 "cs35l40-rcv" reset gph1-2, boost-peak 4100 mA, on hsi2c@109A0000). Exynos9820Audio
  added unchanged to kit v1.2 (vcxproj -> latest SDK). On the S10e: speaker works, headphone
  jack detected without sound, microphones not started (driver_status.md).

## GPU stage G1 (2026-11-02) - UEFI v0.1.3, kit v1.3
- Where the S10e left the GPU (notes/gpu-m16-g9b.md, gpu-full-adapter-hang.md): MaliG76Probe ran a real
  WRITE_VALUE job on the G76 (PASS, boot #106); MaliG76Wddm display-only works; full adapter reaches the desktop
  but every pixel is Mesa softpipe on the CPU (slower than Basic Display + WARP); GpuEnable 1 (panfrost on the
  Mali) never done. So "GPU acceleration" does not exist yet on either phone.
- S10 live DT mali@18500000 == S10e: reg 0x18500000+0x5000, interrupts SPI 0x120/0x11F/0x121, pmu status
  0x2104 mask 1, gpu_dvfs_start_clock 260000 (VERIFIED, dump/s10-dump/dt/live.dts). DSDT GPU0 reused as-is.
- UEFI v0.1.3: Beyond1LtePkg.dsc builds Beyond0LteGpuProbeDxe with -DBEYOND0LTE_GPU_POWER_ON=1 (module-scoped
  BuildOptions; S10e build unchanged). If sboot leaves G3D on (S10e did) nothing is touched; otherwise the S10e's
  G9 pmucal + DTZPC SMC + ACPM 260 MHz path runs - that path itself was never exercised on hardware (UNVERIFIED).
  Rescue: \S10EDIAG\nogpu.flg on the ESP or flash v0.1.2.
- MaliG76Wddm.h: MW_GOP_* per board (only FbMatchesGop uses them; the driver takes dxgkrnl's geometry).
  Both Mali vcxproj -> $(LatestTargetPlatformVersion), /p:MwNoWx=1 drops /WX, /p:ExynosBoardBeyond1=1.
- Kit v1.3: build-s10.cmd builds both (optional, warnings on failure); gpu-s10.ps1 probe|display|status|off.
- Next (G2, after a probe PASS on the S10): the real work is GpuEnable 1 = Mesa panfrost UMD + the KMD's GPU half
  (tools/mesa-build, Windows + WSL host build) - weeks of device iteration, not started on either phone.

## UEFI exception in ArmCpuDxe (user photo 2026-11-02 16:38, v0.1.3) -> v0.1.4
- Screen: "Synchronous Exception at 0x9FD595FD4", ESR 0x02000000 (EC 0x00 Unknown, IL 1, ISS 0), FAR garbage,
  ELR = PC. "Sometimes" - not every boot.
- Symbolised against this tree's DEBUG build (llvm-addr2line on ArmCpuDxe.debug / DxeCore.debug):
  ArmCpuDxe+0x4FD4 = UpdateRegionMappingRecursive, the instruction right after `bl ArmUpdateTranslationTableEntry`
  (ArmMmuLibCore.c:195 inlined ReplaceTableEntry, leaf path :474-480) - a VALID `cmp x20, x25`. Caller chain:
  ArmSetMemoryAttributes <- CpuSetMemoryAttributes <- DxeCore CoreConvertSpace <- SetUefiImageMemoryAttributes <-
  ProtectUefiImage <- MemoryProtectionCpuArchProtocolNotify (fires when ArmCpuDxe installs the CPU arch protocol).
  Entry X22 0xBAAE3F50 = L2 index 490 -> the 2 MB block 0x9FD400000 that holds ArmCpuDxe's own code.
- Where: ArmCpuDxe loaded at 0x9FD591000 = top of the S10-only bank5, directly under uH's el2_earlymem
  (0x9FD800000). DXE allocates top-down; on the S10e the top of RAM is bank4 (proven layout), so UEFI never ran
  from bank5 before. EC 0 / ISS 0 on a valid instruction = a corrupted fetch or an exception injected by EL2
  (that is the ESR a hypervisor injects). UNVERIFIED which.
- v0.1.4: PlatformMemoryMapLib BEYOND1LTE_UEFI_AVOID_BANK5 (default = board has bank5): bank5 below el2_earlymem
  goes to DXE as an EfiBootServicesData allocation HOB (0x980000000 + 0x7D800000, checked in PeilessSec's
  disassembly: BuildMemoryAllocationHob(0x980000000, 0x7d800000, 4)). UEFI then runs from the top of bank4 like
  the S10e; Windows still gets bank5 (BootServicesData is free after ExitBootServices). STATIC_ASSERT that
  el2_earlymem is exactly the top of bank5. S10e build unchanged (no bank5).
- If v0.1.4 still throws: the cause is not the location -> next suspect is the DXE memory-protection split of a
  live 2 MB code block (PcdDxeNxMemoryProtectionPolicy / image protection), A/B by turning image protection off.

## Touch: hold not recognised, drags break up (user, 2026-11-02) -> SecTsY761Touch 0.3.1
- Checked first: the runtime descriptor patch (offsets 40/41, 57/58, stride 58) really rewrites every contact's
  X/Y LOGICAL_MAXIMUM 1080/2250 -> 4095 (parsed the array: bytes 39..41 / 56..58 are 0x26 lo hi for all 10
  contacts); the IC range is 12-bit (live DT sec,max_coords <0x1000 0x1000>; Android dmesg swipe dd:-1895 in x).
  Scan time unit 100 us (0x55 0x0C, 0x66 0x01 0x10) matches KeQueryInterruptTime/1000. So not geometry.
- Cause addressed: the y761 reports only on change; a stationary finger produces no frames, a slow drag produces
  sparse frames. HID touchscreens report every contact every scan; Windows' interaction engine (press-and-hold,
  drag, inertia) assumes that. INFERENCE (no device trace yet).
- 0.3.1: PASSIVE one-shot WDFTIMER (re-armed) re-queues the current frame as a coalescable move every
  HoldRepeatMs (default 10, registry, 0 = off, max 100) while a contact is Active and the IC has been quiet that
  long; stops 3 s after the last IC coordinate event; stopped in TouchStopInput; counter TouchHoldRepeatCount.
- Not compiled here (WDF headers); EWDK build is the check.

## Round 3 (user 2026-11-02 16:50): "audio shows the S10e device but no sound, WiFi not detected, park the GPU"
- UEFI v0.1.5: GPU_POWER_ON back to 0 for the S10 (GPU0 hidden; power-on path dead-stripped: "power-on: done"
  string absent from the .efi). Bank5 fix of v0.1.4 kept.
- Audio "no sound" = by design of the package: Exynos9820Audio.inf A8 shipped AmpInit/AmpEnable/AboxEnable = 0
  (silent endpoint; the S10e harness switched them on per run via esp/extra-audio-stage.cmd). The S10e played
  through the loudspeaker with all three = 1 (d8-audio-win-a5.md "DEVICE RESULT a12 stage D"). 1.0.2.0: INF and
  code defaults = 1; strings "Galaxy S10 Audio". INF AddReg is NOCLOBBER (0x00010003) -> an installed 0 survives an
  upgrade, so install-in-windows.ps1 writes 1 explicitly.
- WiFi "not detected": the driver side is the S10e's working cx5 (native WiFi validated there, d. m12-wifi-wificx.md
  "USER RESULT cx5"); S10 PCIe0/WLAN pins are the S10e's (live DT: pcie0@13ED0000 same reg/IRQ 0xFD, PERST gpf2-2,
  CLKREQ gpf2-0, WLAN_EN gpg0-5 cfg_wlanen, host wake gpa0-7) = Exynos9820.h. Blob names the driver loads exist on
  the S10 (LineageOS beyond1lte proprietary-files: vendor/firmware/bcmdhd_sta.bin_b1, bcmdhd_clm.blob,
  nvram.txt_1rh_es43_b1, nvram.txt_CS01_semco_b1; S10e list identical). The S10 dump only listed /vendor/etc/wifi
  (no blobs there) - the blobs are in /vendor/firmware. Weak spot found: get-wifi-fw.sh skipped mounting the real
  vendor partition whenever a /vendor/firmware dir existed (TWRP ramdisks can have one). v1.1 always mounts vendor
  + system read-only on scratch dirs and searches every candidate dir.
- install-in-windows.ps1 v1.1: removes Mali packages, checks the blob set by name, forces the audio Parameters to 1
  (NOCLOBBER), treats pnputil 259 (staged, no device) as a warning, prints touch/audio/WiFi/battery device state.
- Without logs the remaining WiFi suspects are: WLAN hidden by the firmware (PCIe link not up -> UEFILOG "[PCIe]"),
  or blobs missing (driver run log "DL 3"). collect-logs.ps1 (next task) gathers both.
- Kit v1.6: build-s10.cmd v1.4 skips MaliG76Probe/MaliG76Wddm unless S10_GPU=1; make-kit drops gpu-s10.ps1 and the
  Mali sources, adds collect-logs.ps1 (devices + Device Parameters for every ACPI ID we expose, problem devices,
  driver store, audio Parameters/endpoints, netsh wlan + bcm4375 listing, battery report, driver LogFiles,
  last 300 System events, UEFILOG.TXT/WINPRAM*.BIN from the ESP via mountvol /s, zipped to C:\S10Prov).
  PowerShell scripts not syntax-checked here (no pwsh); reviewed by reading.
- push-to-windows (user 17:03 "TWRP mounts Windows on /data, push from there"; 17:04 "cmd"): ONE file,
  push-to-windows.cmd (PC). It checks adb root + G973, stages out\ setup\ bcm4375\ + scripts in /tmp/s10kit, then
  extracts the sh script embedded after its own `#__SH__` line (findstr /n + more +N), pushes it, strips CR on the
  phone and runs it. Phone part: /data must be the Windows volume (Windows\System32, case-insensitive resolution -
  NTFS lookups are case-sensitive under Linux) and writable (hibernated NTFS mounts read-only); copies the kit to
  C:\S10Prov\kit, WiFi blobs to System32\drivers\bcm4375, setup\S10-install-once.cmd to the All Users StartUp
  folder. At logon that launcher starts auto-install.ps1 elevated (one UAC prompt) -> install-in-windows.ps1 with a
  transcript (C:\S10Prov\install.log) -> install.done -> one reboot; the launcher deletes itself after success and
  stops after 3 failed tries. install-in-windows.ps1 now ends with `exit 0`. Embedded sh: sh -n clean after
  extraction; the .cmd/.ps1 are not executable here.

## Round 4 (user 17:15): audio + WiFi work; the boot exception still happens sometimes; asks about the M4 cores
- Boot exception survived v0.1.4/v0.1.5 (UEFI out of bank5) -> location was not (the whole) cause. v0.1.6:
  PcdImageProtectionPolicy 0 (AutoGen.h of DxeMain: 0x00000000U) - the crashing path
  (MemoryProtectionCpuArchProtocolNotify -> ProtectUefiImage -> live page-table split of the 2 MB block holding
  ArmCpuDxe's code) no longer runs. If it still crashes: need the new photo (PC/ESR/backtrace) to see where.
- M4 (cores 6/7): S10e facts (cpu-ram-plan-20261012.md): CPU_ON of an M4 works; Windows dies once an M4 is
  offered; Linux's own SANITY CHECK shows ID_AA64ISAR1 LRCPC 1 on A55/A75 but 0 on the M4 (no LDAPR) and
  MMFR1 VMIDBits/HPDS lower. Windows sizes features from the boot CPU (A55) -> RCPC code on an M4 = UNDEF.
  Options for the user: (1) probe-only stub (ID regs, ITARGETSR, clock) - safe; (2) BIG-cluster ACPM FREQ_REQ;
  (3) M4 as the BOOT CPU (bootshim CPU_ONs core 6 to run UEFI+Windows, so the boot CPU has the smallest feature
  set and every secondary is a superset); (4) loader-time patch LDAPR->LDAR (KernelErrataPatcher style; user mode
  apps keyed on PF_ARM_V83_LRCPC would still fault) . Recommendation: (1) then (3); not started (user decision).

## Round 5 (user 17:25): WiFi sees networks but cannot connect; touch still laggy -> new driver; audio crackles
- Audio 1.0.2.1 (Render.c): the packet timer copied the packet that had JUST become current into the ring with
  one period of lead and advanced CurrentPacket first. Two crackle mechanisms under load (CPU-rendered desktop):
  a client still writing that packet -> half-written audio; a timer pass > 1 period late -> the RDMA replays a
  4-period-old slot. Now: copy each packet when it COMPLETES, before CurrentPacket advances, into slot
  (period-1+AUD_DMA_LEAD), LEAD 3, ring 6 slots; the slot just played is zeroed (a later miss = silence, not stale
  audio); 3 silent periods at Run, position excludes them. Client tolerance +1 period, timer tolerance 2 periods,
  +20 ms latency. UNVERIFIED that this is the crackle (the run log's "late timer passes" / LateWrites decide);
  amp-side distortion would not change with it.

## Round 6 (2026-11-02) - v0.1.7: touch controller started by the firmware
User: on the z3s port touch only became reliable once the UEFI itself found and started the panel IC, then the
Windows driver. Done the same here (Beyond0LteTouchPowerDxe/TouchChip.c, S10 build only via
BEYOND1LTE_TOUCH_CHIP_INIT = EXYNOS9820_BOARD_BEYOND1):
- after the rails (and DVFS) at ReadyToBoot: USI03 gates + QCH, USI mode I2C, gpp1-4/5 func 2 no pull, HSI2C reset
  400 kHz (Exynos9820Hsi2c ControllerConfigurePlatform/ControllerReset order) on hsi2c@104B0000, slave 0x48;
- sec_ts y771 probe order: boot status 0x55 -> 0x20 (50 x 20 ms); else SW reset 0x12 + wait for INFO ack 0x00 +
  boot status again; log device id 0x22, integrity 0x21, ts status 0xAF, fw 0xA3, img 0xA5; touch function |= 0x61
  (2 B), sense on, clear stack;
- boot touch test (z3s style): top bar green (chip OK) / red (no answer) for 3 s; touch -> blue bar, 10 s of white
  dots where the firmware sees each finger; then the stack is cleared and the chip left sensing for Windows.
Panel IC family: beyond1lte_defconfig CONFIG_TOUCHSCREEN_SEC_TS_Y771=y (Exynos9820.h: S10 blob y771_beyond1_phole.bin).
Build: Beyond1LtePkg DEBUG rc 0, Beyond0LtePkg DEBUG rc 0 (S10e unchanged: init compiled out).
Image: beyond1lte-uefi-v0.1.7.img 7716864 B sha256 7ef5b61b5dac68ca21bba8477a93c86255977015af5234f0813a288f44da293d.
NOT VALIDATED ON HARDWARE: read the TouchChip: lines of UEFILOG.TXT.

## Round 7 (2026-11-02) - v0.1.8: touch test lags far behind the finger
User on v0.1.7: bar green (chip answers), dots follow the finger but with a very long delay; in Windows touch
is jerky, broken up and jumps elsewhere. A delay that grows = the reader is slower than the panel (y771 event
stack backlog; at overflow the stack is cleared -> jumps). Same bus, same timing math in UEFI and Windows.
Suspect: the USI03 kernel clock. cal-if 9820: PLL_CON0_MUX_CLKCMU_PERIC0_USI03_USI_USER (CMU_PERIC0+0x1E0, bit4:
0 OSCCLK 26 MHz / 1 CLKCMU_PERIC0_IP), DIV_CLK_PERIC0_USI03_USI (+0x1810, ratio [3:0]); CLKCMU_PERIC0_IP UUD LUT
400 MHz; i2c-exynos5.c clk_set_rate(default-clk 200 MHz) -> /2. Nobody but Linux sets it; every HSI2C timing here
and in Exynos9820Hsi2c assumes 200 MHz. If sboot leaves the USER mux on OSCCLK, SCL is ~52 kHz (8x slow).
CMU_TOP (PERIC0_IP mux/div at 0x10BC/0x18A8) is not readable: only 0x1000 of it is mapped -> 400 MHz UNVERIFIED.
v0.1.8 TouchChip: logs mux/div + a timed 1-byte read (arch counter 26 MHz), sets div 1 + mux PERIC0_IP, re-times,
restores the old clock (and computes timing for it) if the bus stops answering; xfer polling 5 us; drawing loop
sleeps only when the stack is empty; logs events/s and the deepest left_event backlog.
Build rc 0. beyond1lte-uefi-v0.1.8.img sha256 3271639e1d781f8172be75815884e5d3542a4dc2eaee622152aa32b9273af2af.

## Round 8 (2026-11-03) - v0.1.9: boot touch test off
Touch is device-validated (S10Touch 1.1.0, direct polled HSI2C) and Reboot to TWRP works (S10eAgent RequestsThread,
SetupComplete stops the agent before replacing it). User: remove the green bar / touch wait screen.
v0.1.9: BEYOND1LTE_TOUCH_TEST 0 (TsTouchTest/TsPoll/TsFill compiled out); chip bring-up, USI03 /2 clock fix and the
UEFILOG lines stay. Build Beyond1 rc 0, Beyond0 rc 0. beyond1lte-uefi-v0.1.9.img sha256
770c57407849b4d569306a9635a6f4e153ccf292fd733ac9d110ceb4271d135f.

## Round 9 (2026-11-03) - v0.1.10: faster clocks
User: boot exception now rare (a failed boot ends in TWRP via the crash path); asks for more speed.
S10 build only: DVFS FREQ_REQ LITTLE 1950000 (max), MID 2106000 (2210/2314 need MIF >= 2093000 per mif-perf; MIF
left at sboot's value), INT 534000 (max) - all exact TWRP-kernel ladder steps; S10e keeps 1456000/1794000/400000.
Progress-0x66 guard still skips the request after a boot that hung in it. No thermal management in Windows: MID kept
below max. NOT VALIDATED: read the "DVFS FREQ_REQ ... reply" lines + s10eperf clocks.

## Round 10 (2026-11-03) - GPU stage G1 again: UEFI v0.1.11, kit v1.7
User: clocks better; "switch to the GPU now". v0.1.11 = v0.1.10 + GpuProbeDxe BEYOND0LTE_GPU_POWER_ON=1 (G3D left on /
powered, DSDT GPU0 shown). Rescue: /cache/S10EDIAG/nogpu.flg from TWRP. build-s10.cmd v1.7: S10_GPU=1 builds
MaliG76Probe/MaliG76Wddm into out-gpu\ (both bind ACPI\SAMS9828 - must not reach make-windows v2.3, which installs
everything in out\); gpu-s10.ps1 reads out-gpu\. s10-log v1.2: SAMS9828 device + MaliG76*.txt + gpu-report.txt.
Plan: G1a probe job (MaliG76Probe PASS = the G76 runs a job on the S10), G1b display-only MaliG76Wddm, then G2 =
GpuEnable 1 (Mesa panfrost UMD, tools/mesa-build, built on the PC) - never reached on the S10e either.
beyond1lte-uefi-v0.1.11.img sha256 5393d8016e184c996633f4713d04e8c6bd3f4f3e3d0285b63986f09f450f2990. Build rc 0.

## DEVICE RESULT 2026-11-03 05:15 - GPU stage G1a PASS on the S10 (UEFI v0.1.11, MaliG76Probe)
gpu-report: GPU0 (ACPI\SAMS9828) present (problem 28 before = no driver), MaliG76Probe installed, status OK.
G3D powered (EMBEDDED_G3D_STATUS 1, G3D_STATUS 1); GPU_ID 0x72110000 (G76 r1p1?), SHADER_PRESENT 0x00770077 = 12 cores,
L2_FEATURES 07120206, MMU_FEATURES 00002830, AS_PRESENT ff, JS_PRESENT 7, COHERENCY_FEATURES 0; soft reset OK; L2/tiler/
shader power-up OK; AS0 Mali-LPAE TRANSTAB at PA 9:fd7c9007; WRITE_VALUE job on JS0 -> DONE, JOB IRQ on GSIV 320,
no MMU/GPU fault, target = c0ffee42 -> RESULT: PASS. Same result as the S10e G9b boot #106.
Next: G1b display-only MaliG76Wddm (gpu-s10.ps1 display), then G2 (GpuEnable 1 + Mesa panfrost UMD build).

## DEVICE RESULT 2026-11-03 05:29 - GPU stage G1b PASS on the S10 (display-only MaliG76Wddm, GpuEnable 0)
gpu-report: GPU0 status OK, service MaliG76Wddm, problem 0. DDI trace: QueryInterface -> StartDevice (Started 1,
Mode 0) -> QueryChildRelations -> QueryAdapterInfo -> QueryDeviceDescriptor (C01D0001 = no EDID, expected) ->
RecommendMonitorModes -> IsSupportedVidPn/EnumCofuncModality -> CommitVidPn (status 0) -> SetVidPnSourceVisibility ->
PresentDisplayOnly (Presents 0x800 -> 3072 between two reads = the desktop runs through our driver).
Fb 1440x3040 pitch 5760 format 0x16 at PA 0xCA000000, FbMatchesGop = 1 (the S10 panel, not the S10e one).
Cosmetic: FriendlyName still says "(Galaxy S10e, G10/G11)" - shared INF string.
Other: CPUs 6/7 (M4) CM_PROB_FAILED_START 0xC0000225 - the known open item, unrelated.
G2 (Mesa UMD) on the Linux build box: Mesa 26.2.4 @9a0169f from github chaotic-cx/mesa-mirror, ref 0001-0019 +
our 0020-0040 all apply; libpan SPIR-V v4-v14 built (conda-forge LLVM 18, 0 warnings); host panfrost_compile built
with clang 18 -mms-bitfields; ARM64 UMD via zig 0.13 (aarch64-windows-gnu, -mms-bitfields) needs the WDK DDI headers
(tools/mesa-build/get-ddi-headers.ps1 on the user's PC; Mesa include/winddk/README.txt list).

## G2 build 2026-11-03 05:50 - maliumd.dll (Mesa d3d10umd + panfrost, ARM64) built on Linux
- ddi-headers.zip from the user's extracted EWDK: SDK 10.0.28000.0, 34 files (Compress-Archive names with
  backslashes). Used only the 11 mingw lacks: d3d10/11tokenizedprogramformat.hpp, d3d10umddi.h, d3dkmddi.h,
  d3dkmdt.h, d3dkmthk.h, d3dukmdt.h, d3dumddi.h, dxgiddi.h, dxmini.h, wmidata.h; mingw's sal/d3d11/dxgi stay.
- Fixes needed (none in Mesa itself beyond 0041): case symlinks d3d1xTokenizedProgramFormat.hpp; `_Maybenull_`
  missing from mingw sal.h (-include ddi_mingw_compat.h); -Wno-error=missing-prototypes (the shim's exported
  helpers malikm_* / d3d10_bind_runtime have no prototypes; MSVC never enables that warning).
- Result: libgallium_d3d10.dll 14.6 MB, PE32+ AArch64 DLL, exports OpenAdapter10 / OpenAdapter10_2 (+ Mesa's
  D3DKMT.cpp stubs, exported because d3dkmthk.h declares them dllimport - MSVC does the same; the shim resolves the
  real D3DKMT* through GetProcAddress(win32u/gdi32), malikm_drm.c:642). Imports: GDI32, KERNEL32, USER32,
  api-ms-win-core-synch, api-ms-win-crt-* (UCRT) - no VCRUNTIME140, so no vc_redist needed (libc++ static).
- buildtype debugoptimized (asserts on), same as the S10e g54 UMD.
- gpu-s10.ps1 v1.1: `full` = G2a (copies maliumd.dll to System32, FullAdapter 1, GpuEnable 0). Expectation from
  S10e g52-g54: desktop via softpipe on the CPU, slower than display-only; GpuEnable 1 never worked on the S10e.

## DEVICE RESULT 2026-11-03 06:17 - G2a (gpu-s10.ps1 full, maliumd.dll mingw build) FAILS: black screen + cursor
User: after `full` + reboot only the mouse cursor on black, no sign-in screen. No logs yet.
Reading: the KMD came up as the full adapter (the pointer is drawn - SetPointerPosition path) but nothing the UMD
composes reaches the panel, or LogonUI/dwm crash in the UMD. Same family as S10e g39 ("DWM composes black") /
g45-g51 (LogonUI fail-fast in CreateCompositionBuffer) - but our UMD has 0020-0040 (those fixes), so first
suspects are what differs from the S10e UMD: the toolchain (zig mingw vs clang-cl MSVC), WDK 10.0.28000 DDI headers
(vs the S10e's EWDK), and the S10 panel 1440x3040 vs 1080x2280.
Rescue sent: TWRP -> /cache/S10EDIAG/nogpu.flg -> boot (Basic Display) -> s10-log v1.3 (adds Application log, WER
report texts, gpu-mode.txt) -> gpu-s10.ps1 display -> delete the flag -> reboot.
install-in-windows: copies a newer s10-log.cmd to the Public Desktop and gpu-s10.ps1 to C:\S10Prov\kit (5b).
- 06:39 log.zip (f0890078) is byte-identical to the 05:29 one (49bc7f30, 89512 B, gpu-report 23:20:27): C:\log.zip
  was not rewritten - s10-log v1.3 did not run (or did not reach its zip step) in that boot. No evidence for G2a yet.
- Side finding: every boot's System-log time starts at 2026-11-01 23:17:08 (OS start event 12) - the Windows clock
  is reset to the same value each boot (no RTC / time sync).
- 15:09 TWRP logs (get-logs-twrp v1.0): no evidence of the full-mode boot. WINPRAM-RESTART +0x4000 = a display-only
  boot (LOAD mode 0, Presents ~3.5k); WINPRAM.BIN slice empty. System.evtx: boots rec 2494 / 2620 ended abnormally
  (6008 on the next boot) = the two black-screen full boots; both reached Winlogon 7001 (user logon notification at
  ~20 s) - so logon itself ran, like S10e g52 ("logon completes, panel dark"). No dxgkrnl/Display events; Win32k 263
  (generic) in every boot. User-mode crash evidence (Application.evtx, WER) was not collected by v1.0.
- get-logs-twrp v1.1: + Application.evtx, + WER Report.wer texts (ReportArchive/ReportQueue, no dumps), + the newest
  C:\S10Prov\logs\<stamp> s10-log run folder.
- 15:16 TWRP logs (get-logs-twrp v1.1) - CAUSE NARROWED: 47 WER AppCrash_dwm.exe reports, all the same: dwmcore.dll
  10.0.22621.1635, exception 0x8898008D (UCEERR_RENDERTHREADFAILURE), offset 0xdc6a0; Application log Dwminit
  "0x8898008d ... Arm Mali-G76 MP12" (DWM did pick our adapter). DWM restarts every few seconds (crash loop =
  black screen + cursor). In ALL 47 reports DWM's loaded modules have D3D10Warp.dll but NOT maliumd.dll - on the
  S10e (gpu-full-adapter-hang.md:540) DWM had both. So the runtime never kept our UMD: either LoadLibrary fails
  (mingw build: import / CRT DllMain / TLS) or it loads and something after OpenAdapter fails and it is unloaded
  (OpenAdapterCommon is only a calloc - cannot fail). This error code never occurred on the S10e.
- Ruled out by reading: MW_GOP_* S10 values exist (only FbMatchesGop uses them); no 1080/2280 constants in the
  UMD/shim (comments only); DDI *_SUPPORTED versions are fixed per DDI, not per WDK header build.
- umd-test.ps1 v1.0 (display-only mode, no reboot): LoadLibraryEx as datafile + real load, GetLastError,
  OpenAdapter10/_2 exports -> C:\S10Prov\umd-test.txt. Next decision depends on it.
- 15:30 umd-test on the phone (display-only): process ARM64/64-bit; maliumd.dll sha256 46e5620f... (= the build);
  "map as data" OK; **LOAD: FAIL error 193 (ERROR_BAD_EXE_FORMAT)**. So the loader rejects the image (or one it
  pulls in) - the D3D runtime never had a UMD; DWM fell back and died. Not a Mesa/KMD logic problem.
- Header review (llvm-readobj + pefile): AArch64 0xAA64, DLL|EXEC|LAA, DYNAMIC_BASE|HIGH_ENTROPY|NX, sections
  page-aligned, raw ranges contiguous up to EOF 0xDE8800, SizeOfImage 0xE20000 consistent, relocs DIR64 only,
  TLS dir 37 KB, debug dir = .buildid; pefile: no warnings. Import directory RVA odd (0xDB3995, right after the
  exports) - but the tiny zig test DLLs have the same, so it is lld's normal layout, not proof.
- Bisect set umd-bisect.zip (same zig 0.13 toolchain): t1-c (plain C), t2-tls (36 KB __thread), t3-cxx (libc++),
  t4-synch (WaitOnAddress through the dlltool-made libsynchronization.a + gdi32). Each through umd-test.ps1.
- 15:36 bisect on the phone: t1-c, t2-tls, t3-cxx, t4-synch ALL "LOAD: FAIL 193" -> the toolchain, not Mesa.
  Common header fact: zig 0.13 writes OperatingSystemVersion/SubsystemVersion 6.0 for aarch64-windows-gnu and
  ignores --major/minor-subsystem-version, --subsystem console:6.02 (rejected) and win10 target versions.
  ARM/ARM64 images need subsystem >= 6.2 (MSVC / stock lld-link default ARM64 to 6.02) - working hypothesis.
- tools/mesa-build/pe-set-subsys.py: sets OS + subsystem version (default 6.2) and recomputes the PE checksum
  (matches pefile.generate_checksum). linux-build-g76.sh collect runs it on the UMD. Patched maliumd sha256
  dacaf32c0658ff56ab91d08ccfa649df2d4038a0788b17f3ef00023883ceaf34; f1-c/f3-cxx patched for the device check.
- 15:40 subsystem 6.2 patch: f1-c, f3-cxx, maliumd (patched) still 193 -> hypothesis WRONG (patch harmless, kept
  in the build for MSVC parity but not the fix).
- Reference: an MSVC-built ARM64 DLL from PyPI (markupsafe 3.0.4 win_arm64 _speedups.pyd). Header diff vs zig:
  ImageBase 0x180000000 vs 0x10000000, MSVC has a LoadConfig (0x140, GuardFlags 0x100) / zig none, zig has a TLS
  directory (mingw CRT) and .buildid, zig import dir RVA odd, OS/subsystem 6.2 vs 6.0.
- Bisect set 2 (umd-bisect2.zip): m1-msvc-ref (expect 126 = header accepted, python312.dll missing),
  r1-nocrt (zig, -nostdlib, no imports/TLS/buildid, 3 sections), r2-nocrt-base (r1 + base 0x180000000),
  r3-nocrt-imp (r1 + KERNEL32 import, import RVA 0x208C), r4-nocrt-62 (r1 + subsystem 6.2), c2-crt-base (CRT +
  base 0x180000000). Plus a positive control: a copy of System32\dbghelp.dll.
- 15:44 bisect 2 RESULT: ctl-system (System32 dbghelp copy) LOAD OK; m1-msvc-ref 126 (header accepted, python312
  missing - as expected); r1-nocrt 193; **r2-nocrt-base (r1 + --image-base=0x180000000) LOAD OK**; r3-nocrt-imp 193;
  r4-nocrt-62 193; **c2-crt-base (normal CRT DLL + base 0x180000000) LOAD OK**.
  => ROOT CAUSE: zig 0.13's default DLL image base 0x10000000 (below 4 GB). This Windows 11 22621 ARM64 refuses
  to map ARM64 images based below 4 GB with ERROR_BAD_EXE_FORMAT (MSVC's ARM64 default is 0x180000000; LNK4281 warns
  for low bases). Subsystem version, TLS, imports, import-dir alignment, .buildid: all irrelevant.
- Fix: linux-build-g76.sh cross file c/cpp_link_args += -Wl,--image-base=0x180000000. Rebuild running.
- 15:50 rebuilt maliumd.dll: ImageBase 0x180000000, OS/subsystem 6.2, exports OpenAdapter10/_2, sha256 9fb3fe45fff80432b961d164f254265365e625c0eb82d5a818d44b96c6bff306. Next: umd-test on the phone, then gpu-s10 full.
- 15:53 umd-test on the phone, maliumd 9fb3fe45 (base 0x180000000): **LOAD OK** at 0x7FFBA61C0000 (relocated), OpenAdapter10 / OpenAdapter10_2 exports OK, its CRT deps resolve (ucrtbase). The 193 is fixed. Next: gpu-s10 full (G2a).

## DEVICE RESULT 2026-11-03 16:01 - G2a desktop UP on the S10 (FullAdapter 1, GpuEnable 0, maliumd 9fb3fe45)
User: boots to the desktop in full mode; "still ok, not very laggy" (16:03). Better than the S10e g54 user boots
(extreme lag). Not yet confirmed from logs that DWM really runs on our UMD (expect: KMD trace Mode 1, maliumd.dll
in dwm.exe's modules, no AppCrash_dwm). s10-log v1.4 (PowerShell-extracted, CRLF) sent for that.
- 16:03 log.zip CORRECTION: that "desktop up" boot was NOT full mode. gpu-report (gpu-s10 v1.3 full): it copied the
  OLD maliumd 46e5620f (base 0x10000000) - an old copy was found before the new one - and "could not mount the
  ESP", so nogpu.flg stayed: GPU0 CM_PROB_PHANTOM, Basic Display drew the desktop (hence "not laggy"). Registry
  FullAdapter 1 / GpuEnable 0 is set; KMD trace in the registry is stale (Mode 1 from the black-screen boot).
- gpu-s10.ps1 v1.4: refuses UMD candidates with ImageBase < 4 GB (reads PE+48), newest good one wins, also looks in
  fix\; Clear-NoGpuFlag first searches every mounted drive for \S10EDIAG\nogpu.flg (ESP already mounted case).

## DEVICE RESULT 2026-11-03 16:12 - G2a PASS on the S10 (real full mode, maliumd 9fb3fe45)
gpu-s10 v1.4: skipped nothing bad, copied 9fb3fe45 (base 0x180000000), nogpu.flg removed (S:\, the ESP was still
mounted). Next boot: Win32_VideoController = "Arm Mali-G76 MP12", status OK, InstalledDisplayDrivers maliumd.dll x4,
1440x3040; KMD Mode 1, Started 1; registry FullAdapter 1 GpuEnable 0; NO new WER AppCrash_dwm (s10-log v1.4 found
no WER dir entries). User: desktop works but laggy (right-click slow) - expected: GpuEnable 0 = Mesa softpipe (TGSI
interpreter, CPU) draws everything DWM composes, vs Basic Display + WARP (JIT, multi-threaded) in display-only.
So the S10 now matches the S10e g54 state, with a UMD built on Linux (zig + 0x180000000 base).
Advice given: use display-only for daily use; full mode is the base for G2b (GpuEnable 1, Mali jobs).

## G2b plan (2026-11-03 16:15) - GpuEnable 1 in three isolated steps
Basis: KMD GPU half (maligpu.c, G11 retarget) reviewed vs panfrost/kbase in notes/gpu-s10-review.md (Fix 1-3, in the
S10 kit build - committed 11-02 15:15, kit built after; INF version still 1.0.34.1, so the proof is the
JobStartRefused/PartialMaps trace keys that only the fixed build writes). Never run on any device (S10e stopped at G13).
- G2b-1 `gpu-s10.ps1 gpu`: FullAdapter 1 + GpuEnable 1, machine GALLIUM_DRIVER=softpipe (DWM keeps softpipe, desktop
  as G2a), reboot; `gpu-s10.ps1 m5test`: m5test.exe = one WRITE_VALUE job through D3DKMT only (no Mesa) - the KMD job
  path (CreateContext/Allocation/Render/Lock -> MkJobStart -> IRQ/poll -> fence). PASS line + KMD trace.
- G2b-2 `gpu-s10.ps1 fill`: d3dfill.exe 3 with GALLIUM_DRIVER=panfrost in its own process (Mesa Panfrost on the
  Mali, DWM untouched): 4 colours, readback per phase.
- G2b-3 `gpu-s10.ps1 gpu-dwm`: drop the machine variable -> DWM on Panfrost (0020 default when the GPU is up).
- gpu-s10.ps1 v1.5 (Set-FullMode shared by full/gpu, Set-GalliumDriver, Find-Tool, Show-KmdTrace; display/full clear
  the variable; native calls run with ErrorActionPreference Continue so stderr does not throw in PS 5.1).
  All edited .ps1 (+ s10-log's PS part) parse clean with pwsh 7 (conda-forge `powershell`, ~/pwsh-env).
- tools/mesa-build/build-gpu-tools.sh: m5test.exe 1815a3dc..., d3dfill.exe 8e4c8d13... (zig, base 0x140000000,
  subsystem 6.2). d3dprobe skipped (MSVC __try).
- 16:28 G2b-1 boot (gpu-s10 gpu): user - lag so bad the taskbar shows no app icons and Search never opens.
  Read: MkIsr clears JOB/MMU/GPU INT_STAT it reads (no obvious storm); PollDpc 1 ms only while QCount != 0;
  job timeout MW_JOB_TIMEOUT_MS 2000. Suspects: (a) DWM did not inherit the machine GALLIUM_DRIVER=softpipe and runs
  Panfrost -> jobs time out 2 s each; (b) an interrupt storm on a line MkIsr does not own (returns FALSE);
  (c) the HB/vsync DPCs. s10-log v1.5: + MaliG76Wddm PRAM slice (0x8FE04000, HB i/d/v/s/p), + cpu.txt
  (per-CPU % processor / interrupt / DPC time, interrupts/s, top processes).
- 16:32 TWRP logs: registry GpuEnable 1 / FullAdapter 1 / machine GALLIUM_DRIVER=softpipe (written 16:19:47 local).
  The saved WINPRAM.BIN Mali slice is an OLDER mode-1 boot (Params 00010311 = GpuEnable 0; HB i0 d219f v2136 s6b
  p15b at 152 s = 347 presents / 152 s = 2.3 fps softpipe) - entering TWRP straight from a forced reset skips our
  UEFI, so the G2b-1 boot's PRAM is saved only at the NEXT UEFI boot (if TWRP leaves 0x8FE00000 alone).
  G2b-1 boot = System rec 3642: logon (7001) at +21 s, events until +170 s, NO WER, no dwm crash, no display/TDR
  event, ended by the forced reset. => not crashing, starved/stalled. Need its PRAM (HB i/d/s/p, job counters).
  Note: Windows clock is synced by NTP during a boot (rec 3621 at 09:20:39 UTC = real time), boots start 2026-11-01.
- 17:03 log.zip (nogpu boot, s10-log v1.5): GPU0 hidden, live Mali PRAM all zero (= UEFI saved+cleared it ->
  the G2b-1 boot's PRAM is in ESP \WINPRAM.BIN), but s10-log copied nothing from the ESP: same "ESP already
  mounted on S:" failure as gpu-s10 v1.3 -> s10-log v1.6 uses a mounted ESP first. Asked for adb pull /cache/WINPRAM.BIN.
  cpu.txt (nogpu boot, idle): interrupt/DPC time 0, 1432 interrupts/s total; dwm 157 CPU-s, MsMpEng 103 CPU-s.
- 17:10 ESP WINPRAM.BIN + UEFILOG from TWRP: UEFILOG "BootLog: progress prev=0x0" (no valid PRAM -> no save) and the
  WINPRAM.BIN on the ESP is 64 KiB of noise (no MALIG76T / S10TOUCH anywhere). LESSON: a TWRP boot between the
  interesting Windows boot and the next UEFI boot DESTROYS the PRAM (TWRP's Linux reuses 0x8FE00000). To capture a
  boot's PRAM: forced reset -> let it boot through OUR UEFI (it saves \WINPRAM.BIN) -> only then TWRP.
  The G2b-1 counters are lost; re-run needed.
- 17:20 gpu-watch.ps1 v1.0 + gpu-s10.ps1 v1.6: task "S10 GPU watch" (SYSTEM, AtStartup, 15 min limit) registered
  by `gpu`, removed by `full`/`display` (also the nogpu rescue branch). Every 15 s x 40: Mali PRAM HB/ST + last 6
  trace lines (s10ediag peek 0x8FE04000), _Total % processor/interrupt/DPC + interrupts/s, top 6 CPU deltas; m5test
  once at uptime >= 60 s (60 s limit, killed on timeout). Appends to C:\S10Prov\gpuwatch.txt - NTFS, survives a
  forced reset, get-logs-twrp grabs S10Prov\*.txt. No keyboard needed on the slow boot, no PRAM/UEFI ordering.
  Both scripts parse clean (pwsh). Kit lists (push-to-windows, make-kit) include gpu-watch.ps1.

## DEVICE RESULT 2026-11-03 17:34 - G2b-1: m5test PASS (first WDDM-submitted Mali job, any device)
gpuwatch.txt (gpu-watch v1.0, boot with FullAdapter 1 GpuEnable 1, GALLIUM_DRIVER=softpipe):
- m5test at uptime ~77 s: adapter GPU_ID 0x7211, stats before "gpu 1 renders 0 presents 151 submits 90 fence 90/90",
  allocations 29, aperture maps 5 (fails 0), paging ops [5]=7 [6]=2; job: device/context/allocation at GPU VA
  0x56f6000 -> rendered -> "jobs done 1 failed 0 timedout 0 js 0x0 fault 0x0 irqs 1 resets 0", target 0xc0ffee52 =
  want, job header status 1, after 219 ms -> **PASS: the Mali ran a job submitted through WDDM**. The KMD GPU half
  (soft reset, power, AS0/MMU, aperture map, JS0 submit, IRQ, fence) works on the S10 - the S10e never ran it.
  (exit code line empty: Start-Process -NoNewWindow ExitCode not populated - cosmetic.)
- Lag: HB p 0xac@77 s -> 0xe2@126 s = 54 presents / 49 s ~ 1.1 fps (G2a 2.3 fps); CPU _Total 26-44 %, interrupt
  <=0.5 %, DPC <=8.5 %, 4-8k interrupts/s, ISR i=1 (only m5test's job) - no storm, no starvation by the KMD.
  Cause (by reading memory.c:33-42): in GPU mode allocations live in the aperture segment with CacheCoherent FALSE
  -> CPU mappings uncached/WC; DWM's softpipe renders on the CPU straight into them (reads included) -> crawls.
  Expected to go away when the Mali renders (Panfrost). The KMD's CPU present blt (presents: 155 blts) also reads
  uncached memory: a later performance item.
- Next (no typing possible in this mode): gpu-fill.ps1 + gpu-s10 v1.7 register task "S10 GPU fill" (AtLogOn,
  BUILTIN\Users, Limited): +45 s d3dfill.exe 3 with GALLIUM_DRIVER=panfrost (process env only) ->
  C:\S10Prov\d3dfill-auto.txt incl. per-phase readback. Then G2b-3 (gpu-dwm).
- 17:58 second G2b-1 boot (gpu-watch): m5test PASS again (235 ms, jobs done 1, irqs 1) - reproducible.
  gpu-report.txt is still "gpu-s10.ps1 v1.6 gpu" -> v1.7 (fill task) was not run, so no d3dfill-auto.txt yet.
  The gpuwatch.txt head (passes 13-16, empty pram = MaliG76Wddm not loaded = a nogpu boot where the watch task
  still ran) shows 11-17k interrupts/s with 50-70 % CPU while the user worked in Basic Display; idle 707/s.

## DEVICE RESULT 2026-11-03 18:25 - G2b-2 PASS: Mesa Panfrost renders on the Mali through WDDM (d3dfill)
d3dfill-auto.txt (task "S10 GPU fill", GALLIUM_DRIVER=panfrost in d3dfill's env only, DWM on softpipe):
- adapter "Arm Mali-G76 MP12", D3D11CreateDevice S_OK FL 0xa000, 1440x3040 window, swap chain S_OK.
- Run A (boot @23:18:07): SetFullscreenState(TRUE) S_OK = EXCLUSIVE FULLSCREEN (S10e g53/g54: always 0x887A0022).
  readback (0,0): red 0xffff0000, green 0xff00ff00, blue 0xff0000ff, white 0xffffffff - all exact; 3 frames per
  ~3.7-4.1 s per phase (~0.8 fps); KMD ISR count 1 -> 0x19 (25) during the run = Panfrost jobs; exit 0.
- Run B (another boot): windowed (SetFullscreenState 0x887a0022), same 4 exact readbacks, 11-13 frames / ~3.1 s
  (~4 fps), exit 0. Run C: a nogpu boot - only Basic Render adapters, "no Mali adapter" (expected).
- m5test PASS in every GPU boot (3 boots, 219-250 ms).
=> The whole stack works on hardware: D3D11 runtime -> maliumd.dll (Linux/zig build) -> Panfrost (Bifrost v7
   compiler, libpan precomp) -> libdrm shim -> MaliG76Wddm escapes/render/paging -> Mali G76 jobs -> IRQ -> fences.
- Slow frames: present = CPU copy of the Panfrost back buffer (malikm_present_d3dddi / d3d10_gdi: map for reading
  + GDI/DXGI blit) out of the aperture segment, which the CPU maps uncached/WC (memory.c CacheCoherent FALSE):
  17.5 MB uncached reads per 1440x3040 frame. Performance item for after G2b-3 (cached CPU view + explicit cache
  maintenance, or scanout/flip of the Mali-rendered surface instead of copying).
- WerFault.exe shows in a pass-1 CPU top list of run A's boot - something crashed once; not investigated (no WER copy).
Next: G2b-3 gpu-s10 gpu-dwm (DWM on Panfrost).
- 18:34 G2b-3 attempt: the boot had machine GALLIUM_DRIVER '' (gpu-dwm applied) but MaliG76Wddm never loaded (empty
  PRAM, m5test/d3dfill see only Basic Render) - gpu-dwm was run in a nogpu rescue boot and, unlike gpu/full/display,
  did not remove \S10EDIAG\nogpu.flg. gpu-s10.ps1 v1.8: gpu-dwm also Clear-NoGpuFlag + Set-GpuWatch $true.
- 20:22 G2b-3 (gpu-dwm v1.8): user - the screen comes up, Windows boots, then BLACK. User has no PC now.
  twrp-log.sh v1.0 (TWRP terminal, no PC): ESP UEFILOG/WINPRAM, C:\S10Prov\*.txt/*.log (gpuwatch, d3dfill-auto,
  gpu-report), Exynos/S10/Mali LogFiles, System+Application.evtx, WER Report.wer texts -> /cache/S10LOG/
  s10log-<date>.tar.gz; `nogpu` arg creates the flag. Simulated on a fake tree (bash): archive complete.
  s10-log v1.7: + gpuwatch.txt, d3dfill-auto.txt, m5test.txt, d3dfill.txt, umd-test.txt. Easiest path without a
  PC: TWRP `touch /cache/S10EDIAG/nogpu.flg` -> Windows (Basic Display) -> s10-log -> upload C:\log.zip by browser.

## G2b-3 run 1 (2026-11-03 20:26): DWM on Panfrost - maliumd LOADED, DWM aborts on a Mesa assert
- User: Windows boots, then black. log.zip (s10-log v1.6 from the nogpu boot after): 19 WER AppCrash_dwm.exe, new
  bucket 6ffe1143..., every ~4-5 s 16:34:52 -> 16:36:18: fault module ucrtbase.dll offset 0x76d8c, c0000409
  (= abort() -> __fastfail), and maliumd.dll IS in DWM's modules (index 61, next to D3D10Warp) - first time DWM
  runs on our UMD with the GPU up. Debugoptimized build = asserts on; message lost (DWM has no console).
  Likely: nir_validate (on in debug builds) or a d3d10umd/Panfrost assert on DWM's shaders/formats.
- Mesa patch 0042 (d3d10_gdi.c): s10_diag_init() at screen creation: freopen(stderr) to
  C:\ProgramData\S10e\umd\umd-log.txt, _set_error_mode(_OUT_TO_STDERR) (assert text to stderr), SIGABRT handler
  with RtlCaptureStackBackTrace module+offset lines; "screen" line with driver + build type. Compiled clean.
- linux-build-g76.sh MESA_BUILDTYPE=release -> -Dbuildtype=release -Db_ndebug=true (asserts off). Building.
- gpu-s10.ps1 v1.9 Initialize-UmdLog (folder + icacls Everyone:M, DWM runs as Window Manager\DWM-n) in
  full/gpu/gpu-dwm; s10-log v1.8 + twrp-log v1.1 collect umd-log.txt.
- 20:34 release UMD built: buildtype release, optimization 3, b_ndebug true (-DNDEBUG on 1214 build lines), base 0x180000000, subsystem 6.2, contains the 0042 diag ('release (asserts off)'), sha256 a2805d23f7af517689035b4f7b460890d8480dc0dd3125c8c7962ed1d4e52369. Debug 9fb3fe45 kept in scratchpad/mali/umd-debug-9fb3fe45.
- 21:44 gpu-s10 v1.9 gpu-dwm: Clear-NoGpuFlag found no flag on lettered drives, mountvol /s -> 'The parameter is incorrect'. v1.10: fallback over Get-Volume FAT* volumes via \\?\Volume{GUID}\ paths ([IO.File] Exists/Delete) before mountvol.

## G2b-3 run 2 (2026-11-03 22:29, release UMD a2805d23 + 0042 diag) - ROOT CAUSE: tgsi_to_nir lacks SM4 SAMPLE
umd-log.txt: every dwm.exe (97 x) "screen: driver 'panfrost' ... release (asserts off)" then "unknown TGSI opcode:
SAMPLE" -> SIGNAL 22 (stack maliumd+0x16e8 -> ucrtbase abort); LogonUI.exe (3 x) "unknown TGSI opcode: SAMPLE_I".
= tgsi_to_nir.c ttn_emit_instruction default case: fprintf + abort(). d3d10umd (ShaderTGSI.c sample_ureg_emit)
emits the Shader Model 4 opcodes (separate SVIEW + SAMP operands, TGSI_TEXTURE_UNKNOWN, target in the SVIEW decl);
softpipe interprets TGSI directly so the S10e softpipe runs never hit it; Panfrost goes through NIR.
Not an assert at all - the release build changed nothing (abort() is unconditional).
Mesa patch 0043 (tgsi_to_nir.c): ttn_sample() for SAMPLE/SAMPLE_B/SAMPLE_C/SAMPLE_C_LZ/SAMPLE_D/SAMPLE_L/SAMPLE_I/
SAMPLE_I_MS, ttn_sviewinfo() for SVIEWINFO; separate "sview" (binding = SVIEW) and bare "samp" (binding = SAMP)
variables -> nir_lower_samplers gives texture_index/sampler_index; sview_targets[] recorded from DCL SVIEW;
SAMPLER_VIEW operands return NULL in ttn_get_src (as SAMPLER); result swizzled by the SVIEW operand (tgsi_exec
semantics); ddx/ddy/offset sizes = dim coordinate components (explicit - nir_tex_instr_src_size needs src_type).
Cross-compiles clean; native debug test (nir_validate on) being built.
- 0043 verified natively (build-ttn: panfrost config, buildtype debug -> nir_validate on): tools/mesa-build/tests/
  ttn-sm4-sample-test.c translates two D3D10-style TGSI shaders (SAMPLE / SAMPLE_C / SAMPLE_C_LZ / SAMPLE_B /
  SAMPLE_L / SAMPLE_D on 2D + 2D_ARRAY with SVIEW != SAMP indices and an SVIEW swizzle; SAMPLE_I on 2D UINT and
  BUFFER, SVIEWINFO on both) -> nir_validate after ttn and after nir_lower_samplers -> RESULT: PASS. Lowered indices:
  (tex 1, samp 0), (tex 3, samp 2), shadow ops shadow=1 with comparator, txd 3 srcs, txf 2D coord+lod, txf BUF coord
  only, txs 2D with lod + query_levels, txs BUF no lod. No deref left.
- 22:55 release UMD with 0042+0043: sha256 e87c4f4d02320030a84e4e36402afb4c27edde51f19872e915f60588d5add940 (base 0x180000000, subsystem 6.2).

## G2b-3 run 3 (2026-11-03 23:02, UMD e87c4f4d = 0042+0043) - DWM survives, submits fail with EIO
- umd-log (new build 22:43:22): ONE dwm.exe screen (no crash loop - 0043 fixed the SAMPLE abort), LogonUI, TabTip,
  Explorer, SearchHost x6, TextInputHost x12 on Panfrost; then 364 x "MESA: error: panfrost_batch_submit failed: 5"
  (EIO = the shim's MALIKM_NR_PANFROST_SUBMIT when w_render/D3DKMTRender fails - status printed only with
  MALIKM_TRACE). gpuwatch (that boot, 65 s): KMD "renders 550, jobs done 549 failed 1, js 0x8 fault 0x223002c2
  @0x10, irqs 550, resets 1", allocations 318, aperture maps 231 (fails 0); m5test still PASS after the fault ->
  the GPU itself recovered. AS fault 0xC2 = translation fault, access READ, VA 0x10 = NULL + 16 (a NULL descriptor
  pointer); not explained yet. No Display/TDR event in the System log.
- UWP crashes TextInputHost / SearchHost / StartMenuExperienceHost: ucrtbase+0x7550c c0000409 = invalid-parameter
  fail-fast. Cause = OUR 0042: freopen(stderr) closes stderr even when it then fails (AppContainer cannot write
  C:\ProgramData\S10e\umd), next fprintf(stderr) -> invalid parameter. TextInputHost is the touch keyboard.
- Ruled out by reading Panfrost: txf without a bound sampler (panfrost_upload_wa_sampler), texture table shorter
  than the shader's texture_count (alloc MAX2(actual, needed), null textures).
- Mesa patch 0044: shim prints every Render failure (first 8, then every 100th; NTSTATUS, jc, slot, named/listed/
  slots, live BOs, pid) and every newly failed job (js, fault status/address, jobs done, pid) without MALIKM_TRACE;
  d3d10_gdi.c diag: test-open the log before freopen(stderr) (fixes the UWP crashes).
- 23:20 release UMD 0042-0044: sha256 3b8533e55886cd62d9d57be3937637cad7d5ccabaa8986b5e52c00ff2a413d5d (base 0x180000000).

### G2b-3 run 4 (UMD 3b8533e5, 2026-11-03) - DEVICE RESULT
- Panel: black for a long time, then red/green/blue/white = d3dfill exclusive fullscreen drawn by the Mali (first GPU
  image on the S10 panel). DWM pid 708 on Panfrost: no Render failures, KMD 179 presents (183 blts), still black.
- Cause 1 (black desktop): OpenResource imported the CDD shared primaries / shadow surface with
  resource_from_handle(WINSYS_HANDLE_TYPE_SHARED, CPU pointer) - softpipe's external-memory path (0034). Panfrost
  takes the handle for a dma-buf fd, the import fails, and a blank texture was composed instead.
- Cause 2 (Explorer 382 x Render 0xC0000001): its second CreateDevice re-bound the shim (single global `ddi`) to the
  new device while u_pipe_screen_lookup_or_create handed that device the first one's screen
  ("os_same_file_description couldn't determine ..."): BOs owned by device A submitted on device B.
- One job fault per boot (0x243002c2 @0x10, before jobs done 75) is KMD-global, seen by every process: still open.
- Patch 0045 (UMD 12a5e28b): GPU screen -> shared resources are ordinary textures filled from the locked allocation
  at open and re-filled when sampled / copied from after a Present (d3d10_frame_gen); the first GPU device stays the
  kernel device, later ones share it, DestroyDevice unbinds (malikm_unbind_d3dddi); per-process present log
  (runtime primary / flip / blt ok/failed) and OpenResource log.

### G2b-3 run 5 (UMD 0045 12a5e28b, 2026-11-03 ~23:45) - DEVICE RESULT
- User: a blue screen flashed by, too fast to read. Three boots on 0045 in umd-log (DWM pids 1080, 1748, 636), the
  log collected from a 4th boot in Basic Render (nogpu rescue).
- BUGCHECK (ESP WINPRAM.BIN, UFS bugcheck callback): BC 0x7E SYSTEM_THREAD_EXCEPTION_NOT_HANDLED, c0000005 at
  fffff80962261944 (module unknown - no base map in PRAM). MaliG76Wddm PRAM trace at the end: QueryCurrentFence,
  CollectDbgInfo (= dxgkrnl TDR handling), then SystemDisplayEnable (bugcheck screen); ResetFromTimeout never
  entered. So: a TDR, and the crash inside Windows' TDR path, 67 ms after CollectDbgInfo. The KMD's own job timeout
  (command.c MW_JOB_TIMEOUT_MS 2000) equals the default TdrDelay (2 s), so the TDR wins the race.
  Mitigation without a KMD rebuild (no PC): kit v1.11 gpu-dwm sets GraphicsDrivers TdrDelay 10 / TdrDdiDelay 20.
- 0045 worked as designed where it applied: Explorer's second device "shares the kernel device" (no Render
  failures any more), LogonUI's device unbound cleanly ("kernel device ... destroyed (last bound device)"), DWM
  presents "runtime primary 1440x3040 ok", apps run (Explorer, WindowsTerminal, Taskmgr, SearchHost, TextInputHost).
- But EVERY OpenResource in DWM: "not lockable - blank texture" (1024x1024, 992x2048, 704x704, 1440x3072 ...):
  in GPU mode no opened allocation can be CPU-locked, so the 0045 CPU copy never ran. And the creators' MISC_SHARED
  textures (0037 path) were Panfrost textures unrelated to the shared allocation (resource_from_handle with a CPU
  pointer fails on Panfrost: "WSI pitch not properly aligned" for the terminal's 1372-wide surface, bogus fd else).
  This also explains G2a's missing taskbar icons / search / touch keyboard: shared surfaces never worked.
- Job faults grew: 1 at boot -> 65 by the end of boot C, all fault 0x..3002c2 (translation, READ) @0x10, mostly
  DWM after logon. Still unexplained (candidates: an attribute record for an unbound vertex buffer - panfrost
  skipped it and left the attribute on the next record / none; a NULL constant buffer).
- Explorer restarted every ~10 s in boot C (new pid each time, "65 job(s) failed" lines per pid) - cause unknown.
- WindowsTerminal: present blt 1372x1260 FAILED 0x88760873 (x3).
- WER AppCrash dwm.exe x20 in the zip are OLD (2026-11-03 14:56-14:58 UTC, the 0042 "unknown TGSI opcode" run).
- Plan 0046: (1) shared resources on the GPU - the creator's texture BO IS the runtime allocation (created with the
  resource handle, linear, pitch align 64, described in MW_ALLOCATION_INFO), and DWM imports an opened allocation
  as a BO at its fixed GPU VA (the KMD's VA space is global) via a fake dma-buf fd (drmPrimeFDToHandle +
  malikm_prime_size); (2) panfrost: unbound vertex buffer -> zero record; shareable BOs never cached;
  (3) S10_FAULTDUMP=<process>: PAN_DBG_SYNC without abort, first 2 faulted chains pandecoded into umd-log.
- 0046 built: UMD e3697685 (release, base 0x180000000), kit gpu-s10.ps1 v1.11. Review before shipping: shim lock
  not taken recursively (w_escape / wddm_idle take none; drmPrimeFDToHandle and malikm_prime_size run outside the
  ioctl lock); shim reports DRM 1.1 so pan_kmod never sends QUERY_BO_INFO (unsupported); import pitch must be
  64-byte aligned for Panfrost's strict linear import (CDD 1440 -> 5760 ok; odd pitches are refused and logged).
  Test: gpu-dwm (sets TdrDelay + S10_FAULTDUMP=dwm.exe), reboot, s10-log. Look for "imported (GPU reads it in
  place)", "GPU BO = shared allocation", "FAULTED chain" + decode in umd-log.

### G2b-3 run 6 (UMD 0046 e3697685, 2026-11-04 ~00:45) - DEVICE RESULT
- GPU sharing works: every creator logs "GPU BO = shared allocation" (pitch == Panfrost stride for all, incl. the
  terminal's 1372 -> 5504), DWM logs "imported (GPU reads it in place)" for every OpenResource (1024x1024, 32x32,
  704x704, 992x2048, 1440x3072 ...). No "not lockable" any more.
- S10_FAULTDUMP decoded DWM's faulting chains (vertex/tiler chain, reqs 0): in every faulting IDVS job the
  fragment shader's UBO 0 (= D3D cb0; panfrost puts sysvals at UBO 1) is "XXX: null pointer deref, ubuf_0[0] = 0x0"
  -> the @0x10 translation faults are reads of an UNBOUND constant buffer (panfrost leaves the descriptor zero,
  Bifrost does not bounds-check UBO loads). Fix 0047: a 64 KiB zero BO per context for every referenced UBO slot
  that is not bound.
- DWM pid 712 crashed at 23:18:13 in dwmcore.dll+0x7d52c (c0000005), DWM 7560 took over. 18 WER dwmcore reports
  with code 0x8898008D in the same boot (~4.5-6.5 min uptime) without new "screen" lines.
- XAML hosts crashed 30x in Windows.UI.Xaml.dll+0x253ce4 with 0xC000027B (stowed exception): explorer (restarted ~9x),
  TextInputHost 11x, SearchHost 6x, StartMenuExperienceHost; 3x d3d11.dll c0000005. No D3D error was visible:
  the frontend's SetError was silent.
- Found while reading: CreateResource tested `!pResource` instead of `!pResource->resource` after resource_create,
  so a failed create went on with a NULL texture (fixed in 0047).
- 0047 (UMD b7328d16): zero UBO; SetError logs DDI + line + HRESULT; unsupported-format, failed resource_create and
  failed Map logged with the template; vectored handler logs the first 4 access violations per process (pc/lr/stack
  as module+offset). Kit v1.12: UMD log folder also writable for ALL APPLICATION PACKAGES (AppContainer XAML hosts).

### G2b-3 run 7 (2026-11-04 01:15) - NOT a 0047 run
- gpu-report.txt: "gpu-s10.ps1 v1.11 gpu-dwm", System32\maliumd.dll sha256 e3697685 (0046, mtime 2026-11-01 23:32 by
  the device's wrong clock): g2b3f-umd.zip (0047 + v1.12) never reached the kit folder that was run
  (C:\S10Prov\kit). Same picture as run 6: black, explorer restarting every ~10 s (with SearchHost/TextInputHost),
  @0x10 faults in DWM. User: phone warmer than usual (XAML crash loop + S10_FAULTDUMP's per-job waits).
- Note for gpu-s10 Set-FullMode: the newest-LastWriteTime rule is fragile on this device (RTC reset to
  2026-11-01 23:17 every boot, extracted files may carry either the zip's or the device's time).

### G2b-3 run 8 (UMD 0047 b7328d16, kit v1.12, 2026-11-04 01:28) - DEVICE RESULT
- Black again; the boot lasted ~37 s: BUGCHECK 0x7E c0000005 at ...e31944 (run 5: ...261944 - same offset in a
  page, same instruction, other KASLR base). MaliG76Wddm PRAM trace: the first DxgkDdiPreemptCommand at 26.960 s,
  QueryCurrentFence + CollectDbgInfo (TDR) at 36.961 s = exactly TdrDelay (10 s, kit v1.11+) later; HB at 36.633 s
  had submits == completed fences (0xd8), i.e. no render packet outstanding. => The TDRs are PREEMPTION timeouts:
  command.c MwPreemptCommand returns success and never reports DXGK_INTERRUPT_DMA_PREEMPTED ("Preemption is off
  (EnablePreemption=0 in the INF)" - not where dxgkrnl reads it). Run 5 fits too (TdrDelay 2 s then).
  Fix without a KMD rebuild: kit v1.13 sets GraphicsDrivers\Scheduler EnablePreemption = 0 (MS Learn, "Changing the
  behavior of the GPU scheduler for debugging"). KMD follow-up (needs the PC/EWDK): report DMA_PREEMPTED or set
  PreemptionAware 0.
- AppContainer XAML hosts (SearchHost, StartMenuExperienceHost) now write to the log (v1.12 ACL) and say
  "d3d10umd: Panfrost found no Mali device, using softpipe" - their device is bound to the GPU, so malikm_open's
  _open("NUL") is the suspect (UNVERIFIED). 0048: token fds fall back to an anonymous pipe, with a log line.
- One fault @0x10 at jobs done 83 again, first reported by LogonUI's wait; DWM's S10_FAULTDUMP saw no faulted chain
  of its own -> it is another process's job. 0048: S10_FAULTDUMP takes a list; kit v1.13 sets
  dwm.exe,LogonUI.exe,explorer.exe. Kit v1.13 also prefers the maliumd.dll next to the script (wrong RTC).
- 0048 UMD a5c3d572.

### G2b-3 run 9 (2026-11-04 01:49) - NOT a 0048 run
- gpu-report "v1.11 gpu-dwm", System32 maliumd e3697685 (0046): g2b3e-umd.zip unpacked instead of g2b3g (e/g look
  alike). BUGCHECK 0x7E again at ...011944 (same in-page offset 0x1944 as runs 5 and 8), TDR (QueryCurrentFence
  110.502 s, CollectDbgInfo 110.540 s) with HB submits == fences (0x33b) 0.9 s before: again no render packet
  outstanding - fits the preemption-timeout reading (EnablePreemption was not yet set: kit v1.11).
- Re-sent the 0048 kit as UMD-0048-a5c3d572-kit-v1.13.zip.

### G2b-3 run 10 (UMD 0048 a5c3d572, kit v1.13, 2026-11-04 02:07) - DEVICE RESULT
- User: ran "very long", and FRAGMENTS of a window appeared (the "update driver?" prompt) - first desktop content
  composed by DWM on the Mali. No bugcheck (EnablePreemption 0 works; the TDR/0x7E is gone).
- GPU job faults: ONE per boot (LogonUI, jobs done 80) - DWM's are gone with the 0047 zero UBO. LogonUI's decoded
  chain still shows "ubuf_0[0] = 0x0" in a fragment shader whose ubo_mask evidently lacks bit 0 (pushed?), so the
  null UBO is not necessarily what faults there; open.
- "_open(NUL) failed (errno 13)" confirmed in 10 AppContainer processes; the pipe fallback made them Panfrost.
- No SetError at all in the whole boot. XAML hosts still crash-loop (explorer 13, SearchHost 15, StartMenu 15,
  TextInputHost 13 instances): Windows.UI.Xaml.dll+0x253ce4 0xC000027B, WER P4 combase.dll, P7 80070057 =
  E_INVALIDARG originated in combase - from a runtime/DXGI/DComp call, not from the UMD.
- WindowsTerminal: d3d11.dll+0x46f4 c0000005 (read of 0x8), from dxgi+0xa77c / d2d1 - 4x.
- DWM: one "maliumd.dll_unloaded" c0000005 +0x184c = the 0047 vectored handler after the UMD was unloaded
  (fixed: 0049 pins maliumd.dll); one dwmcore c00001ad (out of memory) - watch DWM memory (gpu-watch v1.1).
- Next: comprobe.exe (drivers/MaliG76Wddm/tools/comprobe) at logon via gpu-fill v1.1 - D3D11 FL, format support,
  shared / keyed-mutex / NT-handle textures, D2D draw, DComp surfaces, composition swap chain, each HRESULT.
  Suspect: WDDM 1.3 KMD (adapter.c DXGKDDI_WDDMv1_3) lacks what Win11's composition expects (NT handles etc.).
- 0049 UMD d9a93d39; kit v1.14 (S10_FAULTDUMP=LogonUI.exe only - DWM no longer waits per job), gpu-fill v1.1,
  gpu-watch v1.1. Zip: S10-GPU-0049-kit-v1.14.zip.

### G2b-3 run 11 (UMD 0049 d9a93d39, kit v1.14, 2026-11-04 02:34) - DEVICE RESULT
- No bugcheck, m5test PASS mid-boot (GPU_ID 0x7211, renders 1288, presents 220, fence 1438/1438).
- comprobe never ran: no "gpu-fill v1.1" header in d3dfill-auto.txt in any boot - the logon task "S10 GPU fill"
  did not fire (last entries are older v1.0 ones). gpu-watch v1.2 now runs comprobe itself (SYSTEM, session 0).
- MEMORY (gpu-watch v1.1 mem lines): in the GPU boot every Panfrost process is huge right after start -
  explorer 362-382 MB private / ~490 MB WS within 8-9 s, SearchHost ~338 MB, TextInputHost 296 MB, StartMenu 180 MB,
  DWM ~370 MB; in the software boot explorer 56, SearchHost 85, TextInputHost 34, DWM 95 MB. Cause found in
  pan_device.c: a 128 MB PAN_BO_GROWABLE tiler heap per process - on WDDM the shim allocates it whole (no growable
  BOs), and the KMD's aperture segment is 1 GB (MW_SEGMENT_SIZE). 0050: 32 MB on _WIN32, S10_TILER_HEAP_MB 4..128.
- XAML hosts still crash-loop (explorer restarts every ~9 s). 13 job faults @0x10 during the boot, none decoded
  (only LogonUI dumped); 0050: one decode per executable (marker dumped-<exe>.flag), kit v1.15 dumps LogonUI +
  the 4 XAML hosts.
- 0050 UMD eefdf678; kit v1.15, gpu-watch v1.2. Zip S10-GPU-0050-kit-v1.15.zip.

### G2b-3 run 12 (UMD 0050 eefdf678, kit v1.15, 2026-11-04 02:55) - DEVICE RESULT
- Tiler heap 32 MB halves the per-process memory: explorer 189 MB private (was 380), SearchHost ~150 (338),
  TextInputHost 104 (296), StartMenu 83 (180), DWM 267-287 (370). Only ONE job fault in the boot (LogonUI, decoded)
  - the 13 of run 11 went with the memory pressure.
- XAML hosts still crash-loop (WER: 92x explorer APPCRASH, stowed E_INVALIDARG originated at combase+0x51a94;
  WindowsTerminal MoAppCrash E_INVALIDARG + d3d11 c0000005).
- comprobe (gpu-watch v1.2, SYSTEM, session 0): Mali adapter "Arm Mali-G76 MP12 (Galaxy S10e, G10/G11)", shared
  1024 MB; D3D11 device FL 10_0 (BGRA support); B8G8R8A8/R8G8B8A8/A8/R8/R8G8/R16G16B16A16F/R10G10B10A2 TEX2D RT
  SAMPLE BLEND (+DISPLAY for 32bpp); B5G6R5 and NV12 not supported (E_FAIL); BC1-3 reported TEX2D SAMPLE but
  CreateTexture2D BC1/BC3 -> E_OUTOFMEMORY (Panfrost: no DXT on the G76; the UMD's CheckFormatSupport claims SAMPLE -
  FL10 requires BC1-3: emulate later); shared / keyed-mutex / NT-handle textures, CreateSharedHandle, staging Map,
  dynamic Map, D2D device + draw: all OK. DCompositionCreateDevice E_ACCESSDENIED and CreateSwapChainForComposition
  DXGI_ERROR_NOT_CURRENTLY_AVAILABLE - expected in session 0.
- gpu-watch v1.3 runs comprobe v1.1 a second time in the console user's session (one-shot interactive task):
  + OpenSharedResource (legacy) / OpenSharedResource1 (NT handle) on a second device, DComp desktop device on a
  WS_EX_NOREDIRECTIONBITMAP window (target, visual, surface draw, commit - magenta 400x300 for 1.5 s), and a flip
  swap chain for a window (green 400x300 for 1.5 s).

### G2b-3 run 13 (UMD 0050, gpu-watch v1.3 / comprobe v1.1, 2026-11-04 03:12) - DEVICE RESULT
- User: the phone got very hot.
- comprobe in the user session (session 1): same format / sharing results as session 0, plus OpenSharedResource
  (legacy) and OpenSharedResource1 (NT handle) on a second device OK, DCompositionCreateDevice OK, DComp surface
  BeginDraw OK (atlas 1024x1024 fmt 87 bind 0x20 misc 0x8200, offset 1,1) - then comprobe DIED (task result
  0xC000013A) with an access violation inside maliumd: pan_resource_afbcp_update (pan_resource.c:2271) from
  panfrost_batch_read_rsrc (pan_job.c:370), reading 0x6c252064697023e6 = the text "#pid %l".
- Cause (read, not guessed): d3d10umd IaSetVertexBuffers makes every unbound slot a USER buffer pointing at a static
  `dummy` float[4]; the device's cso is CSO_NO_VBUF and Panfrost has no user vertex buffers, so
  util_set_vertex_buffers_mask marks the slot bound (the buffer union is non-NULL) and panfrost_emit_vertex_data
  casts `dummy` to a panfrost_resource: afbcp / plane.base are whatever .rdata follows `dummy` (our format
  strings). On the GPU side that becomes a garbage attribute address. Every D3D10/11 app with an input layout
  naming an empty slot hits it. 0051: without caps.user_vertex_buffers an unbound slot is a NULL resource (0046
  then binds a zero record).
- Also 0051: S10_GPU_DENY=<exe list> refuses OpenAdapter to those processes (WARP fallback experiment, off by
  default); the vectored handler now unwinds the faulting thread from its CONTEXT (RtlVirtualUnwind) - the
  RtlCaptureStackBackTrace stack stopped at ntdll's dispatcher. UMD a99f832b.

### G2b-3 run 14 (UMD 0051 a99f832b, 2026-11-04 03:33) - DEVICE RESULT
- Still black; XAML hosts still restart (SearchHost 16, explorer 15, StartMenu 15 instances). Only ONE GPU job
  fault in the boot. No access violation in comprobe any more (the fake-user-VB crash is gone).
- comprobe (user session) output stopped after "CreateRenderTargetView" again, but its umd-log lines show it went on:
  DComp atlas 1024x1024 (x2), "present blt 1024x1024 ok", a composition swap chain (256x256 x2) whose present
  FAILED 0x88760873, the hwnd flip swap chain (400x300 x2) "present blt ok". Task result 0xC000013A. stdout lost
  (buffering suspected) -> comprobe v1.2: unbuffered stdout, progress lines before Clear / EndDraw.
- 0x88760873 = D3DDDIERR_INVALIDHANDLE (d3dumddi.h MAKE_D3DDDIHRESULT(2163)). Same as WindowsTerminal's
  "present blt 1372x1260 FAILED" (its XAML SwapChainPanel = a composition swap chain) - followed by d3d11.dll+0x46f4
  read of 0x8 and an E_INVALIDARG MoAppCrash. The UMD presented those buffers by copying them into the shim's own
  private surface; dxgkrnl refuses that handle for composition presents. The buffers are MISC_SHARED resources whose
  BO IS a runtime allocation (0046) -> 0052: such sources are presented directly (hSrcAllocation = that allocation,
  no read-back), logged as "present shared allocation". UMD 2eff63ee.

### G2b-3 run 15 (UMD 0052 2eff63ee, 2026-11-04 03:49) - DEVICE RESULT
- Still black. 0052 works where the swap chain lives on the process's first device: "present shared allocation
  1024x1024 / 1376x1376 ok" x20 each (no read-back any more). Windows Terminal's 1372x1260 composition swap chain
  still FAILED 0x88760873 (D3DDDIERR_INVALIDHANDLE) when presented as its own allocation - Terminal (like the
  XAML hosts) runs TWO devices, and since 0045 every kernel call went through the first one: the buffer's
  allocation was created through a device that does not own the swap-chain resource.
- 0053 (UMD 1c8d577d): one shim context per runtime device (struct wctx: callbacks, context, command / allocation
  / patch lists, BO table, dead list, primaries), selected by the descriptor's kernel object (CompareObjectHandles;
  a duplicate of the screen's original handle is kept, d3d10_create_screen closes its descriptor) or, for the
  frontend's calls, by malikm_use_device() (CreateResource / OpenResource / DestroyResource / Present /
  SetDisplayMode). util/os_file.c os_same_file_description compares handles on Windows, so every device gets its
  own Panfrost screen (no more u_pipe_screen_lookup_or_create sharing). DestroyDevice frees the device's context.

### G2b-3 run 16 (UMD 0053 1c8d577d, 2026-11-04 04:22) - DEVICE RESULT: XAML CRASH LOOP GONE
- With one shim context per device ("device ... has its own shim context (2 in this process)" x8) the shell hosts
  start ONCE and stay up: in the GPU boot explorer#3856 up 106 s, SearchHost, StartMenuExperienceHost,
  TextInputHost all one instance each (runs 10-15: 13-30 restarts each). CPU calm (dwm 6 s / 26 s interval,
  explorer 0.7 s) - no more heat. No bugcheck.
- comprobe (user session): every step OK except BC1/BC3 textures - DComp windowed tree commits, composition and
  hwnd swap chains Present S_OK.
- Still refused with D3DDDIERR_INVALIDHANDLE: composition swap-chain presents (Terminal 1372x1260 x7, comprobe
  256x256), both as their own allocation and through the blt fallback.
- DWM presented its primary only a few times ("runtime primary" < 300; KMD p 0xb4 -> 0xbc in 26 s).
- 0054 (UMD a39b0a1f): DXGI primaries on the GPU screen are linear Panfrost resources whose BO is the kernel
  primary allocation (Flags.Primary, VidPN source, MW_ALLOC_PRIMARY, pitch w*4) and are presented directly -
  no read-back + convert + memcpy of 17.5 MB per DWM frame (S10_PRIMARY_COPY=1 restores the copy path); refused
  presents log their DXGI_DDI_ARG_PRESENT (flags, dst, subresources, interval).

### G2b-3 run 17 (UMD 0054 a39b0a1f, 2026-11-04 04:41) - DEVICE RESULT
- Black. GPU primaries work: "primary 1440x3040 B8G8R8A8 source 0 (stride 5760): GPU BO = kernel primary" x3 and
  "present shared allocation 1440x3040 ok"; KMD: 182 presents, all blts, by 70 s - then DWM goes idle (30 s later
  submits +1, presents +0): it believes the screen is up to date. XAML hosts stable (one instance each).
- Composition swap chain refused presents logged: "src 1372x1260 alloc 0x40000900, dst 0, subresources 0/0,
  flags 0x1 (Blt), interval 0" - a plain Blt present with no destination, refused D3DDDIERR_INVALIDHANDLE.
- Unknown now: is DWM's composed frame black, or does the KMD's blt not reach the panel? 0055 (UMD fe480a1d):
  1/8-scale thumbnails of DWM's 1st/5th/20th/60th/150th/400th presented primary (GPU blit into a small linear
  texture) -> C:\ProgramData\S10e\umd\frame-<pid>-<n>.ppm + "% non-black, mean rgb" in the UMD log;
  s10-log v1.9 / twrp-log v1.2 collect them.

### G2b-3 run 18 (UMD 0055 fe480a1d, 2026-11-04 05:07) - DEVICE RESULT: DWM'S FRAME IS BLACK
- Thumbnails of DWM's presented primaries #1/#5/#20/#60 (frame-1060-*.ppm): 0.0% non-black, mean rgb 0/0/0. The
  composed frame itself is black (not a KMD / panel problem).
- Cause (read in the source): pan_shader.c lets the Bifrost compiler push UBO 0 into the FAU, because in GL UBO 0
  is the CPU-side default uniform block (a user buffer). Under d3d10umd UBO 0 is D3D cb0, a real buffer;
  panfrost_emit_const_buf's push loop then took cb->user_buffer (NULL) and RETURNED 0 - the draw had no UBO
  descriptors at all (pandecode "XXX: UBOs referenced but not specified", runs 6/10) and its shaders read zeros:
  transforms of 0 -> nothing drawn. d3dfill only cleared, so it was unaffected.
- 0056 (UMD 30e08d7d): on _WIN32 nothing but the sysval UBO is pushable; cb0 is read with LD_UBO from its buffer.

### G2b-3 run 19 (UMD 0056 30e08d7d, 2026-11-04 05:37) - DEVICE RESULT: DWM COMPOSES THE DESKTOP ON THE MALI
- DWM pid 672 thumbnails (notes/img/g2b3-run19-dwm-frames.png): #1 black (boot), #5 the lock screen (100%
  non-black, mean rgb 12/34/128), #20 / #60 the desktop - Bloom wallpaper, taskbar with icons, desktop icons,
  a Windows Terminal window (its content black), a dialog - #150 Brave with a page, an Explorer file dialog over
  it, taskbar. The user worked in this boot (brave, consent.exe x4 = UAC prompts, WindowsTerminal x9).
- 0056 was the missing piece: with cb0 read through LD_UBO instead of a failed push, DWM's draws have their
  transforms and textures. Shell hosts stable (one instance each, up 200+ s). No bugcheck.
- KMD: presents 0xb4 -> 0x16a between 69 s and 257 s (DWM presents on change), all blts, fences = submits.
- Still open: composition swap-chain presents refused (Terminal 1372x1260, 23x INVALIDHANDLE, the window content
  stays black); BC1/BC3 textures; performance (DWM 15-28 s CPU per ~25 s interval).
