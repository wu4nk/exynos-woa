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
