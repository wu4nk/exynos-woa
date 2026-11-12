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
- User (2026-11-04): "nó lên, đúng như ảnh nhưng nó lag khủng bố luôn" - the desktop is on the panel, exactly
  as in the thumbnails, but it lags terribly. KMD presents 0xb4 -> 0x16a in 188 s = ~1 present/s while the user
  was working.

### G2b-3 lag analysis (2026-11-04, read in the source)
- Shim waits drained everything: wddm_drm WAIT_BO and SYNCOBJ_WAIT both `return wddm_idle()`, ignoring the
  timeout. Panfrost polls with timeout 0 in hot paths - panfrost_bo_cache_fetch (`panfrost_bo_wait(entry, 0)`
  for every cached BO it looks at) and the WRITE_DISCARD map in pan_resource.c (`!panfrost_bo_wait(bo, 0, true)`
  -> allocate a fresh BO instead of waiting). Each became a full GPU round trip, and wddm_lock (process-wide)
  was held through it: every other thread / device of the process stopped too.
- GEM_CLOSE drained the queue every 64 closes (WDEAD_FLUSH).
- S10_FAULTDUMP (kit v1.15 gpu-dwm) put LogonUI/explorer/SearchHost/StartMenu/TextInputHost in PAN_DBG_SYNC:
  a drmSyncobjWait(INT64_MAX) after every job (plus a chain walk).
- KMD (no rebuild possible without a PC): DoPresent is a full-frame CPU blt (1440x3040x4 = 17.5 MB) inside
  DxgkDdiSubmitCommand - DXGIDDICB_PRESENT carries no dirty rects in WDDM 1.3, SubRectCnt is the whole rect;
  the source is VidMm aperture memory (CacheCoherent FALSE: likely write-combined, slow to read). One chain
  at a time across both job slots (Kick: `while (!A->Running ...)`). Interrupts do work (m5test: irqs = jobs).
- Every submission lists every live BO (up to 256) for residency (0040): its cost in RenderCb is unknown.
- 0057 (this step): per-submission sequence numbers with their EnqueueCpuEvent events (ring of 64);
  WAIT_BO waits for the BO's newest NAMING submission within its timeout (0 = poll -> -EBUSY), syncobjs carry
  the seq of their fence (real per-descriptor table, WAIT_ALL / any, -ETIME), blocking waits release wddm_lock;
  closed allocations are destroyed once their seq is done (drain only past 2048). Always-on "malikm-perf" line
  per process every 10 s: presents/s, ms in PresentCb, submits per present, named/listed BOs, ms in RenderCb,
  waits/polls/drains, BO churn, CPU user/kernel, KMD deltas (jobs, blts, irqs, paging ops). Thumbnails only
  with S10_THUMBS=1. Kit gpu-s10.ps1 v1.16: gpu-dwm removes S10_FAULTDUMP.
- 0057 review (commit 6b68460, adversarial pass): unlocked waits keep no table pointers (WAIT_BO / SYNCOBJ_WAIT
  re-select wcur after re-locking; the event is a DuplicateHandle copy, so a concurrent retire that closes it is
  harmless); every seq > done has a ring entry with seq' >= seq (pend_pop is the only way done advances, besides
  "KMD idle" which retires all); dead allocations are taken out of wbo before any later list is built, so
  w_retire's destroys at the start of SUBMIT never hit the list being filled. Known gap, unchanged from before:
  the no-event fallback trusts the KMD's adapter-wide idle, which does not see DMA buffers still in VidSch's
  software queue. Open risk: more submissions in flight than before (no drains) - the KMD holds 16
  (MW_QUEUE_DEPTH) and SyncSubmit fails past that; VidSch is believed to feed the hardware queue only "when there
  is room" (GPUView docs), not verified for this KMD. The perf line prints the KMD's "queued".
- 0058: Mesa's CPU work in the same line - shader variants compiled (count, ms; no disk cache: -Dshader-cache=
  disabled) and bytes tiled/untiled on the CPU by pan_store/load_tiled_image (texture uploads, read-backs).
- Build: UMD 0057+0058 = maliumd.dll sha256 6a32cc30d8adb8d4aa43760d280879db7141e181216204b4b7ceaca1fb61b0bb
  (0057 alone built too: 0b2a04ce). Kit zip S10-GPU-0058-umd-kit-v1.16.zip (maliumd.dll, SHA256SUMS,
  gpu-s10.ps1 v1.16). Run 20 asks for: gpu + gpu-dwm, reboot, ~3 min of use, s10-log; read the malikm-perf
  lines of dwm.exe first (presents/s, ms in RenderCb, waits, cpu user/kernel, compiles, tiled MB).

### G2b-3 run 20 (UMD 0058 6a32cc30, 2026-11-04 06:20) - DEVICE RESULT: STILL LAGS; DWM IS CPU-BOUND IN maliumd
- User: "vẫn lag". dwm.exe malikm-perf lines: 0.3-1.0 presents/s; cpu USER 7-10.6 s per ~10 s (one core full),
  kernel 0.0-0.5 s; waits 1-16 per 10 s totalling 1-20 ms, polls almost never busy, drains 0, in flight 0;
  RenderCb 0.16-4 ms per submit (3-40 submits per present); PresentCb 0.1 ms; 0-28 compiles (<=262 ms);
  0 MB tiled on the CPU. BO churn large: +24..+87 BOs and 100-270 MB allocated per 10 s (live ~110 BOs, 140 MB).
- So 0057 removed the GPU stalls (waits are now negligible) but the lag is CPU work inside the UMD itself,
  ~1 s+ per frame, not shader compiles nor CPU tiling. Suspects: CPU copies/reads of WC aperture memory
  (copy_resource on WRITE_DISCARD reallocation, util_resource_copy_region fallbacks, AFBC header init), CPU
  fallbacks in d3d10umd.
- 0059 (UMD c501d5ee): sampling profiler in dwm.exe (S10_PROF=1 for others) - every ~5 ms the shim's last
  caller thread is suspended, its PC/LR/stack copied, the first maliumd.dll address binned (64 B); every 10 s a
  "malikm-prof" line with the 16 busiest image offsets. Symbolize with llvm-symbolizer --obj=
  build-arm64/src/gallium/targets/d3d10umd/libgallium_d3d10.dll 0x180000000+off (same build only).

### G2b-3 run 21 (UMD 0059 c501d5ee, 2026-11-04 06:39) - DEVICE RESULT: LAG = memcpy; UI PIECES MISSING
- User: lag; not drawn: UAC dialog text, taskbar battery and clock, user avatar icon.
- malikm-prof (dwm.exe, 16 lines, ~1100-1650 samples each): +0xbbbc0 x1050-1530 (65-98%) = zig compiler_rt memcpy
  (memcpy.zig: byte loop, auto-vectorized to unaligned 32 B ldp/stp), +0x9b800 x229-552 in later lines = drmIoctl
  (malikm_drm.c:311). memcpy is a leaf, so the caller is not in the sample; by elimination with the BO churn
  (+100-270 MB/10 s): pan_resource.c "Shadow resource creation" - a write map of a resource a pending batch reads
  makes a new BO and memcpy's the WHOLE old one, reading WDDM aperture memory (write-combined for the CPU).
- Same path swaps the BO of PIPE_BIND_SHARED resources (their BO is the runtime allocation other processes open,
  0046; PAN_BO_SHARED is only set on export) - suspected cause of the missing UI pieces.
- 0060 (UMD 80ab38e0): on _WIN32 no shadow copies (flush + wait instead) and BIND_SHARED resources never get a
  new BO on map. Next if still slow: profiler should attribute leaf memcpy to LR; replace zig's memcpy.

### G2b-3 run 22 (UMD 0060 80ab38e0, 2026-11-10, after a Windows reinstall for dual boot) - NO CHANGE
- gpu-report: kit v1.16 gpu-dwm applied (TdrDelay 10, EnablePreemption 0, GALLIUM_DRIVER removed); ESP not
  mountable from Windows (dual cache). dwm.exe: 0.1-0.8 presents/s, cpu user 4-10.7 s per 10 s, BO churn still
  +50-270 MB per 10 s, waits negligible.
- malikm-prof: +0xbbbc0 (zig memcpy) 56-98 % of samples; +0x9b800 x0-784 = return address 0x18009b830 in call()
  after the bl at 0x18009b82c (a lock taken in call(), malikm_drm.c:168) - the sampled thread blocked there, not
  CPU. So the shadow-copy guess (0060) was wrong; memcpy's caller is still unknown.
- 0061 (UMD 471d0b25): samples whose PC is in memcpy are filed under LR (memcpy is a leaf) with the length
  (x8 + 1 in zig's loop), printed as "(memcpy xN, avg K KB)".

### G2b-3 run 23 (UMD 0061 471d0b25, 2026-11-10 01:48) - MEMCPY CALLER FOUND
- malikm-prof (dwm.exe pid 1788, 6 lines): +0x10940 x678-1532 (memcpy, avg 16.8-17.1 MB each) = util_copy_box
  <- util_resource_copy_region (u_surface.c:346) - Panfrost's pctx->resource_copy_region IS the CPU fallback
  (pan_resource.c), so every DWM CopyResource / CopySubresourceRegion of a full-screen 1440x3040 BGRA texture
  maps both and memcpy's 17.5 MB, reading aperture (write-combined) memory. Minor: +0x8080 ResourceUpdateSubResourceUP
  (Resource.cpp:1306) memcpy ~10 MB x8; +0x975c0 small.
- 0062 (UMD cdd95de5): _WIN32 resource_copy_region -> pipe->blit (panfrost_blit, GPU, NEAREST) for same-format,
  single-sample, non-compressed, non-depth/stencil textures, src != dst, format RT+sampler supported and
  util_blitter_is_blit_supported; everything else keeps util_resource_copy_region.

### G2b-3 run 24 (UMD 0062 cdd95de5, 2026-11-10 02:07) - DEVICE RESULT: 0.7 -> 5.5-8.6 presents/s; DWM NOW BLOCKED, NOT CPU-BOUND
- dwm.exe pid 516 malikm-perf (7 lines): 56-86 presents per 10 s (one idle interval: 13), cpu user 1.0-2.0 s +
  kernel 0.7-1.4 s per 10 s (was user ~10 s), 1068-2478 submits (14.6-32.2 per present), named 11-15 + listed
  98-241 BOs, RenderCb 0.39-1.10 ms each, waits 0-15 (<=111 ms), polls 1159-3302 (20-94 busy), in flight 2-10,
  BOs +93..+211 (968-1549 MB !) -85..-115 per 10 s, live 127-371 (129-274 MB). KMD: 1147-2748 jobs, 0 failed,
  0 timed out, blts = presents, 280-654 paging ops. No job faults, no failed presents, no refused presents.
- malikm-prof: memcpy gone (+0x10940 absent; only ResourceUpdateSubResourceUP x2-8). Samples: 25-60 % "elsewhere",
  +0xa2740 (w_render: inside pfnRenderCb) 8-15 %, +0x9b800 35-67 % = return address after fd_handle's
  AcquireSRWLockShared in call() - a stale stack word of a blocked thread (the scan takes the first image word
  above SP; callee frames of a blocked thread hold old ones), i.e. "blocked somewhere", not a real site.
- Reading: presents/s stays 7-8.6 while submits vary 2.3x. Fit time = a*presents + b*jobs over the saturated
  intervals -> a ~ 90 ms per present, b ~ 1.1 ms per job (ESTIMATE from 4 intervals, not a measurement).
  ~19 MB of new BOs per present = one full-screen (17.5 MB) BO created and destroyed per present, source unknown.
  Suspects for a: KMD DoPresent (17.5 MB CPU blt in DxgkDdiSubmitCommand, cached MDL view -> write-combined FB,
  blocks the scheduler thread), VidMm work for the per-present 17.5 MB allocation, or VidMm waiting for busy
  allocations (every submission references every live BO, so none is ever idle while DWM has work queued).
- KMD facts re-read: FbVa = MmMapIoSpaceEx(PAGE_WRITECOMBINE); blt source = MmGetSystemAddressForMdlSafe (cached);
  rows by RtlCopyMemory; soft vsync 60 Hz timer; LockCb already IgnoreSync; one AsOperation(FLUSH_PT) per map.

### 0063 (UMD a83694a8) - probes for the wall time + shorter allocation lists
- perf line adds: time and count per runtime callback (escape / allocate / deallocate / lock / unlock), time
  waiting for wddm_lock (wddm_ioctl only), Panfrost flush reasons per batch really submitted (pan_job.c
  S10_REASON / S10_PUSH; implicit sites named "batch slot reused", "another batch writes/uses a resource ...").
- "malikm-bigbo" lines: top 3 call chains (maliumd frames, RtlCaptureStackBackTrace) creating BOs >= 4 MB.
- Profiler rewritten: copies up to 32 KB of the suspended thread's stack (bounded by the TEB's StackBase), resumes,
  then unwinds the COPY with RtlLookupFunctionEntry / RtlVirtualUnwind (FP redirected into the copy; 1 MB of zeros
  after it; Sp must stay inside the copy) - no loader/function-table lock is taken while the thread is suspended.
  Signature = leaf (64 B bin) + its caller + innermost/outermost maliumd frame; printed with module!export+off
  names (so a blocked thread shows the Nt*/NtGdiDdDDI* call it sits in); "running" by QueryThreadCycleTime delta.
- The profiler thread also times one present packet at a time: event b queued right behind the present
  (EnqueueCpuEvent), event a = the last render submission's; reports ms from max(call, a) to b.
- Change in behaviour: SUBMIT no longer lists every live BO. Unnamed BOs are listed until listed once (seen), and
  always if pinned: created before the context's first submission, or from the screen's mempools.bin/.desc
  (pan_mempool.c s10_unnamed -> malikm_bo_pin; preload/blend shader pools, which batches do not name - the 64 KiB
  BO of the PE run-18 fault is consistent with mempools.desc). S10_LIST_ALL=1 restores the old rule.
  RISK (unverified on device): an unnamed, used, non-pinned BO that VidMm evicts would fault its next job.
- Not verified here: nothing of 0063 has run (cross-build only). Unwinder path is new code inside dwm.exe.

### G2b-3 run 25 (UMD 0063 a83694a8, 2026-11-10 02:54) - PROBES WORK; DWM IS IDLE, NOT BLOCKED; XAML TEXT MISSING
- User: "nó vẫn lỗi, không hiện chữ ở bàn phím, cài đặt không có 1 chữ nào" (touch keyboard has no letters,
  Settings shows no text at all). Earlier (run 21): UAC text, taskbar clock/battery, avatar. All of these are XAML.
- dwm.exe pid 1004 perf: 5.2-9.2 presents/s, cpu user 0.8-2.4 s + kernel 0.4-1.2 s per 10 s; listed BOs 240 -> 26-31
  per submit (pinned ones), no job failed/timed out, RenderCb 0.26-1.5 ms; callbacks per 10 s: allocate 20-141 ms,
  escape/deallocate/lock/unlock a few ms, wddm_lock waited <= 1.6 ms. Flushes per 10 s: 'Blit' x345-606 (6-8 per
  present), 'another batch writes a resource this one uses' x40-207, 'another batch uses a resource this one
  writes' x43-296, 'AFBC write staging blit' x25-150, 'Gallium flush' x19-59.
- Present packet timing (profiler thread): 0.0-1.1 ms from max(call, last render done) to done -> the KMD blt is NOT
  the cost (the "90 ms per present" fit in run 24 was wrong). Last render done 15-42 ms after the present call.
- Profiler (real unwinding works): 45-75 % of the render thread's samples are
  ntdll!NtWaitForMultipleObjects < KERNELBASE!WaitForMultipleObjectsEx < dwmcore+0xe1c54 = DWM's own frame loop
  waiting; NtGdiDdDDIRender (inside pfnRenderCb) ~10 %. So DWM is not blocked in the driver: the present rate is
  what DWM chooses to produce (content updates / its pacing), not a driver stall.
- malikm-bigbo chains are EMPTY (RtlCaptureStackBackTrace returns no frame inside maliumd: the image is built
  with -fomit-frame-pointer and the fast walker follows frame pointers) - 54-92 BOs >= 4 MB per 10 s in DWM
  (~one 17.5 MB BO per present) still unattributed. Fix: use the RtlVirtualUnwind walk there too.
- No "Present refused", no SetError lines, no job faults in this boot. Shared surfaces logged are all fmt 21
  (A8R8G8B8): 1024x1024 atlas pages created by DWM (x13) and opened by the apps (x12).
- Missing text: hypothesis H1 = Direct2D-rendered content (XAML text, font-glyph icons, masks) is broken with
  Panfrost independent of 0057 (run-19 thumbnails only prove Win32/GDI text). To check: A8_UNORM / R8 texture
  sampling (pan_format.c: A8_UNORM = BFMT2(A8_UNORM, R8G8B8A8, R8, 0)), D2D's blend states / dual-source blending,
  texture uploads to tiled A8.

### Missing XAML text: cause found by reading (2026-11-10) - dual-source blending never worked through tgsi_to_nir
- H1 (A8_UNORM unsupported) was WRONG: pan_format.c has FMT(A8_UNORM, A8_UNORM, 000A, L, VTR___) for Bifrost
  (my first greps were cut by `head`), and comprobe (logs30-39) reports A8_UNORM TEX2D RT SAMPLE BLEND.
- d3d10umd OutputMerger.cpp passes SRC1 blend factors through when the driver has
  max_dual_source_render_targets (Panfrost: 1). The pixel shader's SV_Target1 is TGSI COLOR[1].
  gallium/auxiliary/nir/tgsi_to_nir.c: "tgsi loses some information, so we cannot actually differentiate here
  between DSB and MRT ... so far no drivers using tgsi-to-nir support dual source blend: bool dual_src_blend =
  false" -> COLOR[1] always becomes FRAG_RESULT_DATA1 (render target 1). Panfrost's blend then has no second
  source. Direct2D draws text with dual-source blending (colour*SRC1_COLOR + dest*INV_SRC1_COLOR, coverage in
  output 1) -> "dest unchanged" = invisible text, exactly the symptom; softpipe reads TGSI itself, so the
  softpipe modes drew text. NOT yet proven on the device that D2D's blend is this one - 0064 logs when the
  variant is built.
- 0064 (UMD a5b3c176): panfrost_fs_key.s10_dual_src = bound blend is dual-source (util_blend_state_is_dual) &&
  the TGSI-born shader writes FRAG_RESULT_DATA1; that variant rewrites store_output DATA1 -> DATA0 with
  dual_source_blend_index 1 (pan_nir_lower_fs_outputs then emits blend2_pan) and re-gathers info;
  bind_blend_state re-selects the FS variant when dual-ness changes (dirty_shader = ~0 if it changed).
- 0064 review (adversarial pass): the key bool sits in a zero-initialised, memcmp'd struct like the existing
  line_smooth; variants live in a util_dynarray of structs, so a new variant can move the old ones - the bind
  code only compares the old pointer, never dereferences it; IO is already lowered when variants are compiled
  (panfrost_create_shader_state runs nir_lower_io), so the pass works on store_output; info is re-gathered so
  outputs_written no longer has DATA1 (bifrost's ">> FRAG_RESULT_DATA1" multi-RT copy is skipped) and
  color_is_dual_source is set; downstream path read: pan_nir_lower_fs_outputs data[0][1] -> blend2_pan ->
  bifrost blend_src1_type -> pan_context.c blend shader lookup. util_blend_state_is_dual ignores blend_enable
  (a disabled blend with SRC1 factors would get the variant too - harmless). Not run anywhere yet.
- 0065 (UMD 529925b6): LOG_UNSUPPORTED / LOG_UNSUPPORTED_ENTRYPOINT also write "d3d10umd: pid N: UNSUPPORTED
  func:line what" to the UMD log (first two per site); CreateBlendState/1 note dual-source blend states (3 per
  process); malikm-bigbo chains walk with RtlCaptureContext + RtlLookupFunctionEntry/RtlVirtualUnwind.
  Symbols for this build: scratchpad sym-0065/ (session-local).

### G2b-3 run 26 (UMD 0065 529925b6, 2026-11-10 12:39) - XAML TEXT STILL MISSING; dual-source was not it
- User sent a screenshot (first one): Settings shows cards, bitmap icons (Windows logo, OneDrive, Update), the PC
  thumbnail and caption buttons but not one glyph; taskbar has its bitmap icons, no clock; osk.exe (Win32) and the
  desktop watermark (GDI) have their text. So: bitmaps and rectangles of XAML arrive, glyph runs do not.
- umd-log: 11 processes create a blend state with SRC1 factors (rgb src 10 = SRC1_ALPHA, dst 26 = INV_SRC1_ALPHA,
  same for alpha) - Direct2D makes it up front; the dual-source VARIANT was compiled once in the whole boot. So the
  0064 path is real and now works when used, but XAML's text does not go through it. No UNSUPPORTED line at all
  (no unimplemented entry point / shader operand hit), no job fault, no refused present.
- Big-BO chains now have frames (0065 works), not yet symbolized (perf is not the topic of this run).
- Next: stop guessing. drivers/MaliG76Wddm/tools/d2dtext (v1.0, 25af2bb8): Direct2D/DirectWrite primitives into an
  off-screen BGRA / opaque BGRA / A8 texture, read back, changed-pixel count + bounding box + 64x8 ASCII picture per
  primitive; pass 1 hardware, pass 2 WARP (reference), pass 3 hardware in a child process with GALLIUM_TRACE (the
  trace driver is linked into maliumd: debug_screen_wrap) -> C:\S10Prov\d3dfill.txt; results ->
  C:\S10Prov\umd-test.txt (both names s10-log v1.9 already collects).

### G2b-3 run 27 (UMD 0065, d2dtext v1.0, 2026-11-10 12:54) - TEXT REPRODUCED OFF-SCREEN; CAUSE: INTEGER VERTEX ATTRIBUTES
- d2dtext results (notes/logs/g2b3-run27-d2dtext-0065.txt): on the Mali adapter (feature level 10_0) clear,
  rectangle (3072 px exact), ellipse and line arrive; EVERY text primitive is 0 px changed (grayscale, aliased,
  default, white on transparent, ClearType on an opaque target, A8 target); A8 rectangle arrives. On WARP all
  text arrives (1133 px grayscale, 798 aliased, 1244 ClearType). The traced pass gives the same results, so the
  trace wrapper does not disturb it. (Side note: ellipse 4631 px on Mali vs 4651 on WARP, first changed pixel
  00 00 00 vs eb eb eb - antialiased edge pixels differ; not looked at yet.)
- Gallium trace of the run (8.7 MB XML, 10 draw_vbo): the text draw is an INSTANCED INDEXED draw (6 indices,
  instance_count 5 = the 5 glyphs of "Hg 123", primitive_restart 1 / 0xffff on a triangle list) with 5 vertex
  elements, ALL pure integers: vb0 R16G16_SINT (quad corner), vb1 per instance (divisor 1, stride 16)
  R16G16_SINT x3 + R32_SINT. VS: I2F on IN[0..2], MOV of IN[3], IN[4] into a CONSTANT varying. FS: SAMPLE_I
  (texel fetch) on SVIEW[0] = a PIPE_BUFFER R8_UINT view of 524288 bytes (the glyph coverage; filled by
  buffer_subdata + resource_copy_region), index from the flat integer varying, then SAMPLE_I on SVIEW[1], a
  256x80 BGRA lookup texture. Blend ONE / INV_SRC_ALPHA (no dual-source), cull back, flatshade_first.
  Rectangles / ellipses / lines use R32G32_FLOAT elements, non-instanced, non-indexed.
- Cause read in the source: tgsi_to_nir declares every vertex input glsl_vec4 (float); bifrost bi_emit_load_attr
  picks the register format from the load's type: float32 -> BI_REGISTER_FORMAT_F32, integers -> AUTO. So an
  integer element is loaded as float and the shader's I2F runs on float bits -> garbage positions.
  softpipe/draw fetch by the element's format, so they were fine.
- 0066 (UMD 8461af0b): panfrost_vs_key.s10_int_attribs = pure-integer vertex elements that a TGSI-born VS reads
  (uncompiled->s10_from_tgsi; element i = IN[i] = VERT_ATTRIB_GENERIC0 + i = load_input base i); that variant
  gives those load_input an integer dest type; bind_vertex_elements_state re-selects the VS variant.
  panfrost_vertex_state_head mirrors the arch-independent head of panfrost_vertex_state for pan_shader.c.
- Still unproven after 0066 (same draw): bit-exact integers through a float32 flat varying, texel fetch from a
  512 KB buffer view (Bifrost: attribute-buffer based, limit 2^27), texel fetch from the lookup texture.

### G2b-3 run 28 (UMD 0066 8461af0b, d2dtext v1.0, 2026-11-10 13:25) - GLYPH ICONS ARRIVE, TEXT STILL 0 px; CAUSE: TEXEL BUFFERS
- Screenshot: with 0066 the font-glyph icons of XAML are there (Settings back arrow / hamburger, taskbar search /
  Start / task view, Start menu person), text labels and the taskbar clock still are not. osk / tooltips fine.
- d2dtext (notes/logs/g2b3-run28-d2dtext-0066.txt): antialiased edges now match WARP (ellipse 4657 px vs 4651,
  first changed pixel eb eb eb on both; line b7 vs bb) - that was the integer-attribute bug too. All six text
  primitives + A8 text still 0 px on Mali, in the plain and in the traced pass. umd-log has "vertex shader
  variant with integer attributes 0x1f" (explorer) / 0x2 (d2dtext geometry), so 0066 is in use.
- Cause #1, read in the source (not a guess; the path cannot work): on Bifrost (v6/v7) a texel buffer is not a
  texture, it is an ATTRIBUTE: bi_lower_txf_buf -> lea_attr_pan(texture_index) + load_global_cvt_pan, and
  pan_cmdstream.c panfrost_emit_image_texbuf_attribs builds the fragment attribute table from
  ctx->texture_buffer[stage].mask - but returns "no table" (*buffers = 0, 0) when shader->info.attribute_count
  is 0. For a fragment shader attribute_count = LAST_BIT(images_used) + LAST_BIT(nir->info.texture_buffers)
  (pan_compiler.c), and info.texture_buffers is set in exactly one place in Mesa:
  gl_nir_lower_samplers_as_deref.c (GLSL). tgsi_to_nir (legacy ttn_tex and our ttn_sample) never sets it, so
  every TGSI fragment shader that fetches from a buffer runs without an attribute table: Direct2D's glyph
  coverage (SAMPLE_I on the 512 KB R8_UINT buffer) reads nothing -> lookup-texture column 0 -> transparent.
- Cause #2 (same draw, documented hardware behaviour): docs/drivers/panfrost/varyings.rst - "mistaking a float
  for an int is ok, mistaking an int for a float seems to be lossy even without bit-size conversions"; the
  driver therefore stores every highp varying as U32 in the VS and reads flat highp ones as U32 in the FS -
  but only for linked programs (walk_varyings: !info.separate_shader). tgsi_to_nir sets separate_shader, so
  the D2D VS stores the glyph's integers (MOV OUT[2] <- IN[3], IN[4]) as float32 and the FS reads the CONSTANT
  varying as float32. TGSI is untyped, the declared type means nothing. Not proven on the device that the bits
  are actually damaged (small integers are float denormals) - fixed together with #1, so the next run cannot
  tell them apart; S10_TRUST_VARYING_TYPES=1 restores the old rule for an A/B if ever needed.
- Checked and fine by reading: buffer -> buffer resource_copy_region goes to util_resource_copy_region (0062
  only takes textures); panfrost_update_sampler_view / texel_buffer_base_ptr follow a BO change;
  attribs are re-emitted on TEXTURE | SHADER dirty; max texel buffer elements 2^27 on v6+.
- 0067 (UMD 66a415a1): (a) tgsi_to_nir sets info.texture_buffers for a BUFFER sampler view (ttn_sview_var and
  the legacy get_sampler_var), so a Bifrost shader that fetches from a buffer gets attribute_count > 0 and
  panfrost_emit_image_texbuf_attribs emits its attribute table; pan_shader.c logs "panfrost: <stage> shader
  fetches from texel buffers (slots 0x..): attribute_count N" (first 4). (b) pan_nir_collect_varyings.c
  walk_varyings, _WIN32 only: the "untrusted type" rule no longer needs !separate_shader - VS stores highp
  VAR0+ varyings as U32, FS loads flat ones as U32, smooth ones stay F32 (the path every linked GL program
  already takes). S10_TRUST_VARYING_TYPES=1 = old rule.
- 0067 review (adversarial pass): texture_buffers is written nowhere else (nir_gather_info / nir_lower_samplers
  do not touch it; the end of tgsi_to_nir zeroes only textures_used_by_txf), is copied by nir_shader_clone, and
  the bit index is the SVIEW index = the pipe sampler-view slot = ctx->texture_buffer[stage].mask bit =
  texture_index after nir_lower_samplers (mid_lower_tex adds 0 for a FS without images), so attribute i is the
  one emit_texbuf_attribs fills. A shader that declares a buffer with none bound gets a CONSTANT-format
  attribute (existing loop). Varyings: FS descriptor format = FS base type + VS bit size + VS ncomps
  (panfrost_emit_varying_descriptor), so VS U32 / FS F32 for smooth varyings is the designed mismatch; flat
  loads go LD_VAR_FLAT with regfmt U32; locations < VAR0 (position etc.) untouched; walk_varyings is run on
  the uncompiled NIR (VS layout) and again per variant with the same result. Risk accepted: (b) changes every
  shader's varying store type on Windows at once - if rendering regresses broadly, suspect it first.
  Not run anywhere yet. Symbols: scratchpad sym-0067/ (session-local).

### G2b-3 run 29 (UMD 0067 66a415a1, 2026-11-10 14:00) - TEXT FIXED; "5-6 fps": RotateResourceIdentities COPIES
- Screenshot: Settings with every label, the taskbar clock, tray icons - XAML text is there. d2dtext
  (notes/logs/g2b3-run29-d2dtext-0067.txt): every text primitive on the Mali has the SAME pixel count and first
  pixel as WARP (grayscale 1133 px c3 c3 c3, aliased 798, ClearType 1244 px 70 be ff, A8 1133). umd-log:
  "MESA_SHADER_FRAGMENT shader fetches from texel buffers (slots 0x1): attribute_count 1" - cause #1 of run 28
  confirmed; a VERTEX shader with texel buffers (slots 0x3, attribute_count 11) exists too and would have been
  broken the same way. Cause #2 (varying types) was fixed in the same build: not told apart.
- User: "only about 5-6 fps". DWM perf lines: 2.2-7.7 presents/s, 16-48 submissions per present, flushes per
  present ~8 'Blit' + ~13 batch-dependency + ~2 Gallium; "last render done 20-40 ms after the present call";
  1 BO of 17.5 MB created per present (allocate callbacks only ~25 ms / 10 s, but ~1-1.5 GB of fresh pages per
  10 s). Top non-idle profile signature: NtGdiDdDDIRender under maliumd entry +0x3b90.
- Symbolized (sym-0067): entry +0x3b0c/+0x3b90/+0x3be0 = _RotateResourceIdentities (DxgiFns.cpp:340/373), the
  big-BO chain = panfrost_resource_create <- _RotateResourceIdentities. Upstream d3d10umd rotates a swap chain by
  resource_create(temp) + Resources + 1 full copies ("XXX: Copying is not very efficient"); DWM's chain is 3
  kernel primaries of 1440x3040, so every present = a new 17.5 MB texture + 3-4 full-screen GPU copies, each
  through panfrost_blit, which flushes ALL batches before and after itself (pan_blit.c, upstream).
  Other entries seen: +0x822c ResourceCopyRegion, +0x2efc DrawIndexed, +0x842c ResourceUpdateSubResourceUP
  (memcpy 3-5 KB), +0x7d04 ResourceMap (waits in w_wait_seq), u_vbuf_get_minmax_index under DrawIndexed.
- Confound for the user's impression: cpu.txt of this run has ALL 6 processors ~85-100 % busy (TiWorker 152 s,
  svchost 123 s, MsMpEng 117 s of CPU in a ~10 min boot - Windows Update + Defender after the reinstall) and
  processor 0 at 68 % DPC time, 15.6k interrupts/s (source unknown; 0 % / 480 per s in run 21's log). Only 6
  processors are online. Half of the profiler's samples were in neither the 12 printed signatures nor idle.
- MS Learn (DXGI_DDI_BASE_FUNCTIONS, pfnRotateResourceIdentities), fetched today: "must exchange identities ...
  X, Y, Z -> Y, Z, X ... the user-mode display driver should swap the kernel handles of the corresponding
  resources. However, the driver should not swap the corresponding runtime (RT) handles ... views can refer to
  resources ... the driver must re-create such views. Also, the driver might be required to reapply currently
  bound views."
- 0068 (UMD 93d4486c): panfrost_s10_rotate_storage (pan_resource.c): n same-shaped, same-modifier textures
  exchange {bo, plane (layout + GPU address), valid levels, CRC valid, label ownership, constant stencil} after
  every batch that names one of them is flushed; the pipe_resource objects stay, so views stay valid
  (panfrost_update_sampler_view notices the new BO; batches read rsrc->bo when made); dirty-all afterwards.
  d3d10_gdi.c d3d10_rotate_storage wraps it; _RotateResourceIdentities uses it for plain textures (no buffer,
  no mapped subresource, no opened-shared copy) and rotates kmt_primary / kmt_shared / gpu_primary of the
  Resource structs the same way; otherwise the old copies. Logs "RotateResourceIdentities: N x WxH exchanged
  storage (no copy)" / "... rotated by COPYING (why)" (2 each per process).
  malikm-prof: new "split of N samples" line - outside / inside the driver x waiting / kernel calls / running,
  and the top 10 driver entry points (function start offsets) as samples/waiting/kernel.
- 0068 review (adversarial pass): rotation direction = the old copy loop's (temp<-0, 0<-1, ..., last<-temp).
  Not a Panfrost context (softpipe, trace wrapper: resource_copy_region is not panfrost_s10_copy_region) ->
  false -> copies. Shim state is per BO (busy seq, seen/pinned, WDDM handle), so it moves with the BO. KMD
  present (command.c MwPresent/DoPresent) takes whatever source allocation the present names and maps it by its
  MDL - alternating primaries are fine; before, DWM always presented the same allocation. Known wrinkle,
  accepted: runtime allocations (WBO_RUNTIME) are freed by the runtime with the RT resource they were created
  under, which after a rotation is not the Resource whose pipe_resource holds that BO; swap-chain buffers are
  destroyed together and GEM_CLOSE of such a BO only unlocks, so the worst case read from the code is an unlock
  of an already freed handle. NOT verified on the device: app flip-model swap chains (kmt_shared) now present
  alternating allocations to DWM instead of always the same one - watch for "Present refused" and frozen app
  windows. Not run anywhere yet. Symbols: scratchpad sym-0068/ (session-local).
- Not done (next candidates if still slow): panfrost_blit's flush-all before and after every blit;
  u_vbuf_get_minmax_index per DrawIndexed; KMD one-job-at-a-time.

### G2b-3 run 30 (UMD 0068 93d4486c, 2026-11-10 16:11) - ROTATION WORKS, STILL 2-9 PRESENTS/S, DWM WAITS OUTSIDE THE DRIVER
- "RotateResourceIdentities: 3 x 1440x3040 exchanged storage (no copy)"; no big BOs any more (BOs +0..4 per
  10 s), idle DWM frame = 4-5 submissions (was 16), 0 failed jobs, no "Present refused", user reports no
  rendering fault - 0068 is correct on the device. User: "fps still extremely low".
- DWM busiest intervals: 8-9.4 presents/s, 12-33 submissions per present ('Blit' ~3 + ~10 batch-dependency
  flushes per present remain), cpu 1-2.3 s per 10 s, waits ~0. New split line, busiest interval: 781 samples =
  501 waiting OUTSIDE the driver + 49 kernel + 160 running outside | in the driver 4 waiting + 39 kernel + 28
  running. So the driver takes < 10 % of DWM's thread and DWM idles in its own frame loop 65-95 % of the time:
  DWM is neither CPU- nor driver-bound. "Last render done 6-53 ms after the present call."
- Apps in the same log: Explorer 5 presents/s, SystemSettings 2.6-4/s (cpu 4-5 s per 10 s, waits 44-275 ms),
  StartMenuExperienceHost 2.8/s. Older boots' logon DWMs sit at a constant 13.2-13.5 presents/s with ~13
  submissions "in flight" - a ceiling that does not move with CPU use.
- cpu.txt this time: 20 % total (processor 4 at 88 %), DPC 11.8 % on processor 0 - the Windows Update load of
  run 29 is gone, so that was not the cause.
- NOT KNOWN, and nothing logged can tell: what one GPU job costs submit -> done (KMD runs one chain at a time;
  G3D clock is the firmware's 260 MHz of 702, notes/gpu-m16-g2g3.md F1/F5), the real software-vsync rate
  (16 ms KTIMER on a 15.6 ms tick, VsyncTicks is not exported), the cores' speed under Windows.
- gpubench v1.0 (drivers/MaliG76Wddm/tools/gpubench, built by build-gpu-tools.sh, sha a3dbcded): D3D11 only -
  50 round trips (clear 16x16 + 1-pixel staging map), 200 small copies back to back, 20 full-screen copies
  (-> Mpixel/s), 5 full-screen uploads, a full-screen read back, WaitForVBlank x 120 (-> Hz), a 200 M-step
  multiply-add chain pinned to each processor; then the GPU part on WARP. -> C:\S10Prov\umd-test.txt.

### G2b-3 run 31 (gpubench v1.0, UMD 0068, 2026-11-10 16:23) - THE BOTTLENECK IS THE KMD'S FLIP LATCH
- gpubench (notes/logs/g2b3-run31-gpubench.txt), Mali vs WARP: round trip 1.57 ms (worst 56); 200 small copies
  0.43 ms each; 20 full-screen 1440x3040 copies 5.62 ms each = 779 Mpixel/s (WARP 1007); a full-screen upload
  20.4 ms (WARP 1.7); read back 6.0 ms. The GPU and the job path are NOT what caps DWM at 2-13 frames/s.
  WaitForVBlank x 120: 45.9 ms apart = 21.8 Hz, shortest 0.2 ms, longest 3256 ms - the 60 Hz software vsync is
  starved. CPU chain: 1.3-1.5 G steps/s on all 6 processors (no slow core).
- Why (read in command.c): with FlipMmio (default) every DWM flip is latched INSIDE the vsync DPC (VsDpc -> Latch ->
  DoPresent) as a CPU copy of the whole 1440x3040 primary (17.5 MB) from its VidMm pages (mapped uncached /
  write-combined - the UMD's own upload of that size into such memory took 20.4 ms) into the WC-mapped GOP fb, at
  DISPATCH_LEVEL with ScanLock held. Consistent with: DPC time on processor 0 = 68 % (run 29, busy) / 11-12 %
  (runs 30-31, ~2 presents/s) = 50-130 ms per latch; the vsync DPC cannot fire while a latch copies (21.8 Hz,
  back-to-back ticks after a long one); DWM's thread waits outside the driver 65-95 % of the time (for flips); the
  logon DWM's constant 13.2-13.5 frames/s ceiling (runs 21-26). My run-24 conclusion "DoPresent is 0-1.1 ms, not a
  bottleneck" measured BLT presents through SubmitCommand, not the MMIO-flip latch - wrong part.
- No UMD-side fix exists (the copy and its source mapping are the KMD's). User has no PC with an EWDK.
- KMD change (ScanoutDirect, Parameters\ScanoutDirect default 1, MwBuild 35, INF DriverVer 1.0.35.1):
  memory.c MAP_APERTURE_SEGMENT maps a primary that has exactly the fb's width / height / pitch, 32 bpp, onto the
  GOP framebuffer's own physical pages (Fb.PhysicAddress = 0xCA000000, FbSize must be whole pages: 17 510 400 B =
  4275 pages) in the Mali's page tables (MapScanoutDirect, runs of 64 PFNs through MkMmuMapPages; pages past the fb
  from VidMm's MDL; undone on failure) and sets MW_ALLOCATION.ScanDirect; UNMAP clears it. Latch: a ScanDirect
  primary needs no copy (DirectLatches++). DoPresent: a ScanDirect SOURCE is read from FbVa (its VidMm pages are
  stale); into the fb at the same origin it is a no-op. PRAM: "Switches" bit 24 = ScanoutDirect, "ScanDirect pages
  N" for the first 4 mapped. So the GPU renders DWM's frame straight into what the panel shows; a flip is free.
- Review (adversarial pass): eligibility needs FlipMmio + fb mapped + fb page-aligned and whole pages + identical
  geometry, so any other primary keeps the copy. All of DWM's 3 flip-chain primaries alias the fb: content stays
  correct (each frame redraws at least what changed since the previous one, which is what the fb holds; Mali writes
  a tile once, at the end of a render pass), but a frame split into several render passes ('Blit' / dependency
  flushes, acrylic blur) can show its intermediate state for a moment - possible flicker, accepted; tearing as
  before. CPU views of a ScanDirect primary (shim w_lock -> VidMm pages) are stale - no GPU-mode path reads them
  (malikm_primary_present is the softpipe path). GPU MMU attributes as for any page; the job chain ends with
  FLUSH_CLEAN_INVALIDATE so DECON reads what the GPU wrote. Race flag-vs-latch benign (set under MmuLock before
  Mapped). Not verified: that DECON shows GPU writes the same way it shows CPU writes (same DRAM; no reason not to).
  Rescue: Parameters\ScanoutDirect = 0 (old copy), or kit kmd-restore, or nogpu.flg.
- BUILT ON LINUX (new, tools/kmd-build): build-kmd-linux.sh v0.1 fetches the NuGet packages
  Microsoft.Windows.WDK.ARM64 + Microsoft.Windows.SDK.CPP 10.0.26100.6584 (km headers; km/ARM64 ntoskrnl, hal,
  displib, bufferoverflowfastfailk, libcntpr libs), casefix.py symlinks the WDK's wrong-case #includes, clang-cl 18
  --target=aarch64-pc-windows-msvc /kernel /GS /Zp8 /O1 -mgeneral-regs-only -funwind-tables (NOT -ffreestanding:
  it drops .pdata), lld-link /driver /subsystem:native /entry:GsDriverEntry /include:_load_config_used (GsDriverEntry
  fast-fails if the loader did not set __security_cookie, which it finds through the load config)
  /merge:.impcall=.rdata /tsaware:no. Checked: ARM64 PE32+, subsystem native, imports ntoskrnl 53 + HAL 2
  (KeGetCurrentIrql, KeStallExecutionProcessor - as the EWDK build's notes say), load config with the cookie,
  176 .pdata entries (the functions without one are frameless leaves / thunks), MMIO accessors = WDK inlines with
  dsb sy. sign-pe.py v0.1: Authenticode SHA-256 PKCS#7 with a self-made test certificate (key stays in the work dir,
  .gitignore), self-verified (hash, RSA signature, embedded cert, PE checksum). Loads only in test mode (the phone
  shows "Test Mode"). Never loaded anywhere yet - first clang-built kernel driver of the project.
- Kit gpu-s10.ps1 v1.17: `kmd` (test mode check, testsign.cer -> LocalMachine Root + TrustedPublisher, original
  kept once in C:\S10Prov\kmd-backup, takeown/icacls, rename + copy over the service's ImagePath file, else
  MoveFileEx delay-until-reboot), `kmd-restore`. v1.18 (21:15, user: "Access ... MaliG76Wddm.sys.new denied"): takeown/icacls
  the driver-store folder too, not only the file - the original stayed in place (rename had failed first). Package S10-KMD-1.0.35-build35-kit-v1.17.zip (sys sha dd80a783).
- 21:55 user: the clang/Linux-built KMD (build 35) BUGCHECKED at boot ("màn xanh"), stop code not given. Cause
  unknown (clang codegen vs MSVC, the self-made test signature, or ScanoutDirect itself - nothing to tell them
  apart yet). User now builds himself: sent the MaliG76Wddm source folder (ScanoutDirect, DriverVer 1.0.35.1) for
  build-s10.cmd (S10_GPU=1) in his EWDK; rescue = nogpu.flg -> kmd-restore. Do NOT ship Linux-built kernel drivers
  again until one has been seen to load (compare against an EWDK build of the same source first).
- 2026-11-11 01:09 log (7d14aaa4): Windows boots on Basic Display. setupapi: out-gpu\MaliG76Wddm 1.0.34.1 (the OLD
  build, not 1.0.35) was re-added as oem7.inf, then a later boot ran "pnputil /delete-driver oem7.inf /uninstall
  /force" - no MaliG76Wddm package left, ACPI\SAMS9828\0 problem 28 (0xC0000490), Services\MaliG76Wddm\Parameters
  empty, mali-pram.txt stale garbage. gpu-report.txt is from kit v1.16 (old script on the phone). No bugcheck event /
  dump in the log (stop code of the build-35 crash still unknown). maliumd 0068 (93d4486c) still in System32.

### G2b-3 run 32 (KMD 1.0.35.1 ScanoutDirect, built by the user with his EWDK, UMD 0068; log 2026-11-11 01:33)
- setupapi: out-gpu\MaliG76Wddm 1.0.35.1 installed (SUCCESS). That GPU boot ran with no bugcheck: the EWDK build of
  the same source works, so the build-35 bluescreen was the Linux/clang build (or its signature), not ScanoutDirect.
- DWM in that boot: 20-34 presents/s for minutes (was 2-13; logon DWM ceiling 13.2 -> gone), 0 failed jobs, 0
  timed out, KMD "0 blts" (latches copy nothing), profiler: DWM waits outside the driver ~92 % (idle/paced).
- The log itself was collected in a later boot with \S10EDIAG\nogpu.flg present (GPU0 hidden, phantom) - why the
  user went back to the rescue flag is not in the log (asked). gpubench in umd-test.txt is the old run-31 file.
- 01:43 user (run 32 continued): "smoother, but flickers while dragging windows, leaves blurred edges, tearing" -
  the aliasing risk named in the 0068/ScanoutDirect review: all three flip buffers were the panel's memory, so every
  render pass of a frame (and DWM reading its "previous" buffer for blur/backdrop) happened on screen.
- KMD 1.0.36 (source to the user, compile-checked here with the Linux toolchain, NOT shipped as a binary): only an
  allocation flagged MW_ALLOC_FBALIAS (new ABI flag 0x4, fb geometry) is mapped onto the fb (ScanDirect);
  FbAliasMapped counts mapped ones (map / unmap / free-while-mapped); Latch skips the CPU copy for an fb-sized
  primary while FbAliasMapped > 0. DWM's primaries are ordinary VidMm pages again.
- UMD 0069 (f49591d5): shim malikm_fbalias_bo_begin/end -> CREATE_BO with MW_ALLOC_FBALIAS + w/h/pitch/A8R8G8B8 as a
  device allocation; d3d10umd _Present: for a GPU primary (DWM) s10_frame_to_fb() creates once per device a LINEAR
  RT texture of the primary's size/format (stride must be w*4, logged "framebuffer alias ... frames are copied into
  it") and resource_copy_region()s the frame into it (GPU blit, ~5.6 ms full screen) before the flush and the flip.
  S10_FB_ALIAS=0 = off. Old KMD + 0069: the alias is an ordinary BO, nothing changes on screen; KMD 1.0.36 + old UMD:
  no alias, latch copies on the CPU (slow, correct).
- Review: flip packet follows the blit in the same context, so dxgkrnl latches only after the blit completed; the
  blit is one render pass per tile, so the panel sees old or new tiles, never a half-composed frame (tearing at the
  tile front still possible - not synchronised to the scan). Alias evicted -> FbAliasMapped 0 -> latch copies again.
  Cost: one full-screen GPU pass per DWM frame (~17 % of the GPU at 30 frames/s); dirty-region copies later if needed.

### G2b-3 run 33 (KMD 1.0.36 built by the user, UMD 0069 f49591d5; 2026-11-11 02:10)
- User: "laggier, but no more glitches"; on-screen keyboard (TabTip / TextInputHost) hangs after a little use, and
  Win+Shift+S does not work - both "since always" (no details yet).
- umd-log: "framebuffer alias 1440x3040 ... (alloc 0x0, stride 5760): NOT POSSIBLE - no copy" -> the KMD latch copies
  on the CPU again (KMD 58-67 blts / 10 s = DWM's flips), DWM 2-10 presents/s. Cause: Panfrost's BO cache served the
  alias a recycled 17.5 MB BO (panfrost_bo_create fetches from the cache before allocating), so CREATE_BO never ran
  with MW_ALLOC_FBALIAS. Cache entries are never SHAREABLE (pan_bo.c), so 0070 adds PIPE_BIND_SHARED to the alias.
- gpubench (notes/logs/g2b3-run33-gpubench.txt): WaitForVBlank 54.7 Hz (was 21.8; shortest 0.1, longest 71.6 ms) with
  DWM mostly idle; GPU full-screen copy 769 Mpixel/s.
- UMD 0070 (3f7aef59): alias template + PIPE_BIND_SHARED. Not run yet.

### G2b-3 run 34 (KMD 1.0.36 + UMD 0070 3f7aef59; 2026-11-11 02:35) - FRAMEBUFFER ALIAS WORKS
- "framebuffer alias 1440x3040 ... (alloc 0x40001380, stride 5760): frames are copied into it". DWM 15-40 presents/s
  (typ. 22-35) for the whole boot, KMD 0 blts, 0 failed / timed-out jobs, no "Present refused". gpubench:
  WaitForVBlank 62.4 Hz (shortest 2.7, longest 41 ms). User: "tạm ổn" (no flicker / ghosting report any more).
- G2b-3 goal (DWM + apps render on the Mali, usable speed) is MET on the device. Open: touch keyboard hangs after a
  while, Win+Shift+S does nothing useful (no details yet); BC1-3; KMD one job at a time; G3D clock 260 MHz.
- User wants an app to test broadly towards daily use -> gputest (differential D3D11 test Mali vs WARP).
- gputest v1.0 (drivers/MaliG76Wddm/tools/gputest, sha 8d12a634): D3D11 FL 10_0, HLSL compiled on the phone
  (d3dcompiler_47), each test drawn on the hardware adapter and on WARP into 256x256 RGBA8 and compared (PASS / FAIL
  with area, first differing pixel, a 32x16 difference map, BMPs in C:\S10Prov\gputest / MISSING = the Mali refused
  what WARP takes / BROKEN = the tool's own bug). 53 feature tests (filters, address modes, 11 blend set-ups incl.
  dual source, depth/stencil/scissor/viewport/cull, topologies + strip cut, instanced integer attributes, flat
  integer varying, Buffer.Load, vertex texture fetch, geometry shader, MRT, MSAA resolve/load, cube/array/3D,
  comparison sampler, CopySubresourceRegion/UpdateSubresource, dynamic VB, discard/derivatives/integer ops/loop) +
  33 texture formats + 14 render-target formats. NOT run anywhere yet: expect BROKEN lines on the first run.

### G2c run 35 (gputest v1.0 first run, UMD 0070, KMD 1.0.36; 2026-11-11 02:49)
- 38 of the first 39 tests PASS with 0 differing pixels (filters, address modes, all 11 blend set-ups incl. dual
  source, depth/stencil/scissor/viewport/cull, topologies, strip cut, instanced integer attributes, Buffer.Load,
  vertex texture fetch). No BROKEN lines - the tool's HLSL is fine so far.
- FAIL flat-integer-varying: every triangle got key 0 (Mali 8000ff05 everywhere) = SV_VertexID is 0..2 for every
  vertex. d3d10umd emits TGSI VERTEXID_NOBASE -> nir load_vertex_id_zero_base, which pan_nir_lower_vertex_id does
  not lower and Bifrost's emit has no case for (release build: falls through to nothing).
- gputest then DIED at geometry-shader: "ACCESS VIOLATION: execute at 0x0, lr maliumd+0x9064" =
  CreateGeometryShader calling pipe->create_gs_state, which Panfrost does not have (no geometry shaders at all,
  although the UMD advertises feature level 10_0). The remaining 61 tests did not run.
- UMD 0071 (f0c6fdd1): pan_nir_lower_vertex_id lowers load_vertex_id_zero_base = raw_vertex_id + raw_vertex_offset
  - first_vertex (new sysval word: PAN_SYSVAL_VERTEX_INSTANCE_OFFSETS u[3] = index bias or the start of a
  non-indexed draw, via load_first_vertex); d3d10umd CreateDevice installs stand-in create/bind/delete_gs_state when
  the driver has none (logged; such draws run without the GS). Real geometry shaders = open item.
- User: brightness cannot be changed - no backlight/brightness driver exists in this port yet (AMOLED: DSI
  brightness command via DECON); roadmap item, not GPU.

### G2c run 36 (gputest v1.0 complete, UMD 0071; 2026-11-11 03:08): 86 PASS, 2 FAIL, 12 MISSING, 0 BROKEN
- flat-integer-varying PASS (0071 SV_VertexID fix confirmed), no crash at the geometry shader.
- FAIL geometry-shader: expected (stand-in, no GS in Panfrost). Open.
- FAIL two-render-targets: target 1 stays at its clear value. Cause: Panfrost enables blend info only for RTs
  0..pipe_blend_state.max_rt (panfrost_create_blend_state rt_count = max_rt + 1; emit: !so->info[i].enabled -> off)
  and d3d10umd never set max_rt. 0072 sets it to 7 in both CreateBlendState paths.
- MISSING BC1/BC2/BC3/BC4/BC5 (+sRGB): CreateTexture2D E_OUTOFMEMORY - the GPU has no S3TC/RGTC, but D3D10 makes
  them mandatory, so any app loading DDS/BC assets fails. 0072: CreateResource keeps such a texture as
  R8G8B8A8_UNORM/_SRGB (Resource.bc_format), initial data and UpdateSubResourceUP are decoded with
  util_format_unpack_rgba_8unorm_rect, SRVs of it use the RGBA8 format. Not handled: Map of such a texture
  (dynamic/staging BC), SNORM variants.
- MISSING 4x MSAA (CheckMultisampleQualityLevels 0), B5G6R5 / B5G5R5A1 (optional at FL 10_0). Open.
- PASS but note: cube-map 208 px differ up to 247 (face edges - seamless cube filtering / face selection). Open.
- UMD 0072 (c32555a1). Not run yet.

### G2c run 37 (gputest v1.0, UMD 0072 c32555a1; 2026-11-11 03:28): 94 PASS, 1 FAIL, 5 MISSING, 0 BROKEN
- two-render-targets PASS (max_rt), BC1/BC2/BC3/BC4/BC5 + sRGB PASS (decoded-on-upload emulation, worst diff 1-2).
- Left: FAIL geometry-shader (none in Panfrost); MISSING msaa4-resolve / msaa4-load-samples (4x MSAA not offered),
  B5G6R5 / B5G5R5A1 (optional); cube-map passes with 208 px up to 247 off at face edges.
- Next: real apps (Edge, Photos, Store, Terminal), touch keyboard hang, Win+Shift+S, MSAA, GS, brightness driver.

### Android-side data (2026-11-11 06:40, user's S10 on Android 14 "r0s" ROM, ExtremeKernel 4.14.356)
- notes/logs/android-gpu-info-2026-11-11.txt (android-gpu-info.sh v1.0). Mali-G76 MP12 r0p0 (gpu_id 0x7211),
  core mask 0x770077. asv_table of THIS chip (kHz, uV): 702000 681250 | 650000 662500 | 572000 650000 |
  433000 625000 | 377000 612500 | 325000 587500 | 260000 568750 | 200000 568750 | 156000 543750. dvfs_table tops at
  702 MHz (no 803 MHz step in this kernel). time_in_state: mostly 156 MHz idle, 572 / 702 MHz under load. G3D
  thermal zone 50 C idle. Regulator names do not say which buck feeds G3D (ACPM does it on Exynos) - still needed
  before the UEFI raises G3D above the firmware's 260 MHz. CPU policies: 4 x 1.95 GHz, 2 x 2.31 GHz, 2 x 2.73 GHz;
  under Windows only 6 processors are online.
- User also ran "panprobe" (Android app dev.zenithblue.panvktest 1.3.2) with an imported panvk .so (Mesa 26.3.0-devel
  git-795ca7ff3e "p0175", a fork with a kbase JM backend for v7): 41/41 PASS on this phone, incl. geometry,
  tessellation, xfb, pipeline stats, BC emulation; d3d_fl: DXVK 3.1.1 D3D11 up to FL 11_1, vkd3d-proton D3D12 11_1.
  So Bifrost v7 here can do GS / tessellation / FL11 through panvk. User asks to "integrate it" - open question
  whether he means a panprobe-style harness for our Gallium driver or porting that panvk to the Windows shim.
- 06:45 user: github.com/JimVulkan/mali-panfrost ("Panfrost for Mali Bifrost on kbase", Mesa 26.3.0-devel based, MIT,
  "XclipseTools", tested on Mali-G76 / S10e and G72). It is the SAME Gallium panfrost driver we use, with geometry
  shaders and tessellation emulated by COMPUTE jobs on the v6/v7 job manager (pan_gs.h, pan_nir_lower_gs.c 723 lines,
  GS hooks in pan_cmdstream.c ~400 lines (panfrost_gs_prepare/emit_vs_varyings/emit_out_varyings/viewports),
  pan_jm.c (jm_launch_draw_gs / jm_launch_draw_tess), pan_shader.c (create/bind_gs_state, gs key), pan_context.h
  (panfrost_gs_draw, PAN_SYSVAL_GS_PARAMS 22 / TESS_PARAMS 23), pan_screen.c caps; tess also libpan tess.cl kernels).
  Not done there: GS + tess together, TES without TCS, xfb from GS/TES, layered rendering from GS. Cloned read-only to
  scratchpad ext/mali-panfrost. Our tree already has src/poly (same files, 2 small diffs) and the needed intrinsics;
  the rest of src/panfrost has drifted (26.2 vs 26.3-devel: pan_blit.c renamed pan_blitter.c, compiler changes).
- Plan: port ONLY the geometry-shader emulation (JM path, v7) into our 26.2 tree as a series of UMD patches; tess is
  unreachable from the D3D10 DDI (needs the D3D11 DDI, FL11), so it waits. Then gputest geometry-shader should PASS
  and the d3d10umd stand-in GS hooks (0071) go away when the driver has create_gs_state. Attribution: their files keep
  their MIT header + a note in the patch. gputest v2 (panprobe-style report) half-written, not built.

### G2c UMD 0073 - geometry shaders (2026-11-11 07:10), port of JimVulkan/mali-panfrost's GS emulation
- tools/mesa-build/patches/0073-g2c-geometry-shader-emulation.patch. Copied verbatim (MIT header "Copyright 2026
  XclipseTools" kept): pan_gs.h, pan_nir_lower_gs.c. Ported by hand into our 26.2 tree: pan_context.h
  (emu_params[], viewports[16]/scissors[16], PAN_SYSVAL_GS_PARAMS 22, vs key keep_clip_space, fs key
  viewport_clip/emulated_producer, panfrost_gs_draw, panfrost_gs_key, uncompiled .gs info); compiler
  inputs.vs_keep_clip_space (bifrost_nir.c skips nir_lower_viewport_transform + noperspective lowering);
  pan_nir_lower_sysvals (load_geometry_param_buffer_poly -> GS_PARAMS), pan_helpers (GS_PARAMS dirty = PARAMS),
  sysval upload; pan_shader.c (GS lowered to a 64-wide compute shader at variant compile, GS key, FS moves
  viewport/layer inputs + viewport clip, bind_gs re-keys VS+FS, create/delete/bind_gs_state on v6/v7 only);
  pan_cmdstream.c (panfrost_gs_topology / prims_per_instance / emit_vs_varyings / emit_out_varyings /
  emit_viewports / prepare, GS branch in panfrost_single_draw_direct, active prim = the GS output primitive,
  indirect draws with a GS go to util_draw_indirect, adjacency dropped without a GS, prim restart split on the CPU);
  pan_jm.c GENX(jm_launch_draw_gs) (vertex job -> COMPUTE job (barrier) -> tiler job, indexed UINT32 list over the
  emulation's buffers); pan_context.c (viewport/scissor arrays, point size from the GS); pan_screen.c (GEOMETRY shader
  caps on v6/v7, adjacency prim modes, max_geometry_output_vertices 256, total components 1024, invocations 32,
  max_viewports 16).
- Ours on top: a VS feeding the GS is compiled with no_idvs (the emulation launches it as a plain vertex job; the
  fork turns IDVS off for all of Bifrost, we keep it for normal draws); d3d10umd's token-less GS
  (CreateGeometryShaderWithStreamOutput without code = SO from the VS) becomes a sentinel meaning "no GS" (as the
  0071 stand-in did; SO from it still not wired); a log line for the first 4 GS draws ("panfrost: geometry shader
  draw (emulated): ..."). The 0071 d3d10umd stand-ins stay but are inert (create_gs_state exists now).
- Not ported: tessellation (needs the D3D11 DDI), GS + xfb, multi-stream GS, layered rendering, the fork's
  "no IDVS on Bifrost" (we have no evidence of the INDEXED_VERTEX failure they saw - our desktop draws with IDVS).
- Review (self, adversarial): index records - VS runs from offset_start = min_index + bias, GS record = index -
  (offset_start - bias) = index - min_index: matches params->index_bias. Integer varyings: GS copies raw 32-bit words,
  FS attribute format is u32/f32 from the FS's own slot type (0067 makes flat TGSI varyings ints) - bit exact.
  Risk: the GS compute job reads the vertex job's varyings with plain loads after a job barrier (fork's measured
  path); the whole NIR path for a TGSI-born GS (tgsi_to_nir -> nir_lower_io -> pan_preprocess_nir -> lower_gs) was
  only compile-tested on Linux - first device run decides. If DWM breaks: UMD 0072 back (GS draws do not occur in
  DWM, so it should not).
- UMD 0073 maliumd.dll sha256 79adcdc4. Not run yet. Target: gputest geometry-shader PASS.

### V1 - panvk (Vulkan) on Windows, first build (2026-11-11 07:56)
- User gave the source of the panvk he ran on Android: github.com/zenithblue-oss/panvk-kbase-android @cf70e02b
  (Mesa PanVK 26.3.0-devel git-795ca7ff3e build 122i / p0175). It is a patch stack, not a Mesa tree: Mesa main
  5a07217f (gitlab) + 228 series patches + 170 jm-v7-wip patches; scripts/prepare-rc5-tree.sh reproduces it
  exactly here ("OK base=228 jm-v7-wip=170", .panvk-patchlevel p0175). License: MIT scaffolding, Mesa code keeps
  Mesa's licenses, vendored kbase UAPI headers GPL-2.0 WITH syscall note (NOTICE.md).
- Finding: p0175's JM (v6/v7) queue is kbase-ONLY - jm/panvk_vX_gpu_queue.c submits kbase atoms
  (kbase_kmod_job_submit_dep / _retry, kbase_kmod_wait_atom) and tracks them with a CPU vk_sync type
  (kbase_cpu_sync_type, panvk_kbase_sync_set_pending); upstream's DRM/syncobj JM path is gone. Rather than rewrite
  their (device-tested) queue, V1 implements those atom calls over our shim: src/panfrost/lib/kmod/malikm_atoms.c,
  an atom = one PANFROST_SUBMIT whose out_sync is a binary syncobj from a 1024-slot ring (slot reused only after
  its atom completed), deps -> in_syncs, wait -> drmSyncobjWait. The physical device opens through the DRM path
  (drmGetDevices2 in the shim returns one platform device "malikm"; create_kmod_dev calls malikm_open() = the
  D3DKMT path of maliwddm) but uses the kbase CPU sync types on _WIN32.
- Our patches (tools/mesa-build/panvk/patches, on the p0175 tree): 0001 include/win32-compat (shim @0073);
  0002 wddm-mali-g52 0003-0011/0015/0016 adapted (4 hunks redone by hand: kmod meson, pan_bi_tex_flags bitfields,
  PATH_MAX); 0003 the panvk port: drmDevice API + drmGetCap in the shim, malikm_atoms.c, kbase sync code built on
  _WIN32 minus sync files, libcl uint typedef, vk_image drm_format_mod on Windows, no ELF build-id (blake3 of
  version+git sha), strndup/sysmacros/usleep/sched_yield/clock (os_time) fixes, BC raw spill file off on Windows,
  ICD exports via vs_module_defs vulkan_api.def. Build: tools/mesa-build/linux-build-panvk.sh (source clc host cross
  collect); clc stubs for the cross build in tools/mesa-build/panvk/clc-stubs (now also libpoly).
- Result: vulkan_panfrost.dll 33.9 MB (all arches v6..v14), exports vk_icdGetInstanceProcAddr /
  GetPhysicalDeviceProcAddr / NegotiateLoaderICDInterfaceVersion, sha256 37f5f6a8. Link warnings: vk_drm_syncobj_*
  undefined - referenced only by the CSF (v10+) queue and the sparse-binding queue (not exposed on v7:
  has_sparse = PAN_ARCH >= 10). Not loaded on any device yet.
- vkprobe v1.0 (drivers/MaliG76Wddm/tools/vkprobe): loads the ICD directly (no vulkan-1.dll - the phone has no
  Vulkan loader), tests instance/device/fill/copy/compute/triangle/stress with 5 s fence timeouts, hand-written
  SPIR-V (spv/*.spvasm, spirv-as + spirv-val vulkan1.0/1.1). Kit S10-panvk-V1.zip. Not run yet.
- Risks for the first run: the shim's D3DKMT path in a non-D3D process (worked for m5test/fill tools); our KMD and
  the tiler heap growth panvk expects (jm-v7/003 grows it on kbase); the series' v7 code assuming kbase
  behaviour elsewhere (SAME_VA / AUTO_VA, imports). Next: vkcube-style present (WSI win32 = GDI blit), then DXVK.

### G2c run 38 (UMD 0073 79adcdc4 installed; 2026-11-11 08:23 log zip): DWM crash loop
- 17 dwm.exe + 2 LogonUI.exe AppCrash, all maliumd.dll+0x18f468, exception 0x80000002
  (STATUS_DATATYPE_MISALIGNMENT), one every ~5 s, each right after the first blend state. Symbolized (pdb):
  simple_mtx_lock in panfrost_update_shader_variant (pan_shader.c:690), ldaxr on ctx->uncompiled[type] + 0x2cc with a
  misaligned pointer. Cause: 0073 advertises GEOMETRY shader caps, so util_blitter_create sets
  has_geometry_shader and every blit/clear restores pipe->bind_gs_state(saved_gs) - but panfrost_blitter_save
  never saved the GS, so saved_gs = INVALID_PTR got bound as the GS and the next re-key locked "its" mutex.
  My review of 0073 missed u_blitter's caps-driven save/restore (lesson: when a driver gains a stage, grep
  u_blitter / cso for has_<stage>).
- UMD 0074 (ea8bd2b5): panfrost_blitter_save also saves ctx->uncompiled[GEOMETRY]. Every panfrost blitter path goes
  through panfrost_blitter_save (pan_blit.c x2, pan_context.c clear, pan_resource.c x2). Not run yet.
- vkprobe (panvk V1) not run in this log.

### G2c run 39 (UMD 0074 ea8bd2b5 + panvk V1; 2026-11-11 08:42)
- 0074 confirmed: DWM up again, no new dwm/LogonUI crash (the 19 WER reports in the zip are run 38's).
- gputest: 39 tests PASS (triangle .. vertex-texture-fetch), then the process died at geometry-shader, 3 runs alike:
  0xC000001D (illegal instruction = UNREACHABLE's brk) at maliumd+0x2683a0 = ttn_compile_init, right after
  "tgsi_to_nir: unhandled TGSI property 1 = 5" (GS_OUTPUT_PRIM = TRIANGLE_STRIP). tgsi_to_nir has NO geometry
  shader support at all (EMIT/ENDPRIM "XXX", no GS properties, no IN[vertex][i]); before 0073 d3d10umd's stand-in
  never created the shader so it never got there. 0073's review assumed TTN handled GS - wrong.
- UMD 0075 (f66ae84a): tgsi_to_nir GS: GS_INPUT_PRIM/OUTPUT_PRIM/MAX_OUTPUT_VERTICES/INVOCATIONS properties,
  GS inputs as per-vertex arrays at their semantic slot (d3d10umd: GENERIC[reg] / POSITION) loaded with the vertex
  dimension (immediate or ADDR-indirect), PRIMID / INVOCATIONID system values, VIEWPORT_INDEX scalar int output,
  EMIT = output stores + emit_vertex, ENDPRIM = end_primitive. pan_nir_lower_gs.c: two phis built with sources
  emitted after nir_pop_if (invalid NIR - the same bug the panvk series fixed in jm-v7 0011), and
  nir_lower_continue_constructs before nir_lower_returns. NEW host test s10_gs_hosttest (pan_shader.c hook
  s10_hosttest_compile = Panfrost's create-time lowering + panfrost_shader_compile with a fake G76 screen): debug
  build + NIR_DEBUG=validate, 5 shaders compile (GS points->strip 7.9 KB, GS triangles with a loop + indirect vertex
  10 KB, VS keep_clip_space, VS plain, FS behind the GS). Build: ninja -C build-hostdbg
  src/gallium/drivers/panfrost/s10_gs_hosttest. Runtime (the three jobs) still untested.
- vkprobe: "loaded in 8333 ms", 0 physical devices, no message. Cause found in the code: panvk refuses v6/v7/v11/v14
  unless PAN_I_WANT_A_BROKEN_VULKAN_DRIVER is set (INCOMPATIBLE_DRIVER = silently skipped); the Android launcher
  sets it. V1.1: gate off on _WIN32 (panvk patch 0004); vkprobe v1.1 also sets the variable, PANVK_DEBUG=startup and
  MALIKM_TRACE=1, and writes stderr to vkprobe-stderr.txt. vulkan_panfrost.dll 2a7be5c5, vkprobe 83cd3ae5.

### G2c run 40 (UMD 0075 f66ae84a + panvk V1.1; 2026-11-11 09:19): gputest 95 PASS, 0 FAIL
- gputest v1.0: 95 PASS, 0 FAIL, 5 MISSING (msaa4 x2, B5G6R5 / B5G5R5A1 texture + B5G6R5 RT), 0 BROKEN.
  geometry-shader PASS with 0 px differ: the emulation (0073) + TTN GS (0075) work on the G76 end to end
  (VS vertex job -> GS COMPUTE job -> indexed tiler job). cube-map still 208 px edge diffs (open).
- vkprobe v1.1: instance created; shim found the maliwddm adapter (adapter 0x40000000, 256 allocation slots), VERSION
  panfrost 1.1, "Found compatible device 'malikm'", 17 GET_PARAMs (the last -22 = a param the KMD does not know,
  tolerated), then "panvk is not a conformant Vulkan implementation" (panvk_physical_device_init, after sync types)
  and the process ended silently inside vkEnumeratePhysicalDevices (no line after "loader interface version 5").
  So the crash is after vk_warn_non_conformant_implementation: extension/feature/property tables, disk cache,
  vk_physical_device_init or panvk_wsi_init (win32 WSI).
- vkprobe v1.2: unhandled-exception filter printing the code, module+offset and an RtlVirtualUnwind call stack
  (vulkan_panfrost.dll has .pdata), "enumerating physical devices..." marker. DLL unchanged (2a7be5c5).

### panvk run 41 (vkprobe v1.2, V1.1 DLL; 2026-11-11 09:23)
- Crash report worked: 0xC000001D at vulkan_panfrost.dll+0x3fe7d4 = get_ctr_el0 (util/cache_ops_aarch64.c, `mrs
  ctr_el0`) <- util_cache_granularity <- panvk_v7_get_physical_device_properties (nonCoherentAtomSize) <-
  panvk_physical_device_init <- vkEnumeratePhysicalDevices. Windows does not let EL0 read CTR_EL0 (Linux traps and
  emulates it). util_has_cache_ops() already said false for MSVC; mingw/zig took the aarch64 path.
- Fix (panvk patch 0005): util_has_cache_ops() false on _WIN32 (no user-mode data cache maintenance assumed; only
  WB_MMAP BOs would need it), get_ctr_el0() = 64-byte lines on _WIN32. Disassembly of the new DLL: no `mrs
  ctr_el0` left, the only mrs is FPCR. V1.3: vulkan_panfrost.dll 573504d3, vkprobe 0c626027.
- Note: second load of the DLL took 9 ms (first 8.3-8.9 s = cold read of 34 MB from storage).

### panvk run 42 (V1.3 573504d3, vkprobe v1.2; 2026-11-11 09:31): 7 PASS, 0 FAIL - Vulkan works on the G76 under Windows
- instance 37 ms: "Mali-G76 MC12" (vendor 0x13b5, device 0x72110000), Vulkan 1.3.363, driver panvk / Mesa 26.3.0-devel
  (git-83df52d604, p0175). geometryShader 1, tessellationShader 1, multiViewport 1, textureCompressionBC 1,
  shaderFloat64 0. One heap 5552 MiB DEVICE_LOCAL; types: 0x1 and 0x7 (device-local + host-visible + coherent).
- device 10 ms; fill 1 MiB OK (209 ms first submit = meta shader compile + warm-up); copy 256 KiB 1.25 ms; compute
  4096 invocations OK (compile 1.7 ms, run 0.99 ms); triangle 256x256 OK (compile 4.2 ms, run 12 ms, 20808 px lit,
  red/green/blue corners interpolated, black clear - picture checked); stress 200 fill submits 171 ms (0.85 ms each,
  worst 8.6 ms). DLL load 328 ms.
- Shim trace: BOs created through CREATE_BO (3 HEAP BOs for the tiler heap, flags 0x3), MMAP_BO, each atom = SYNCOBJ
  CREATE + SUBMIT ("0 BOs named, 11-14 listed for residency") + SYNCOBJ_WAIT; no errors. malikm_atoms.c's ring works.
- Logs: notes/logs/run42-vkprobe.log. Next: V3 = a window (WSI win32 GDI path), V4 = a Vulkan loader for the phone
  (vulkan-1.dll ARM64) + DXVK.

### Upstream check (2026-11-11 12:21): ilyatbn/exynos-woa moved far ahead of our fork point
- Our fork point: 52b14d0 (2026-10-13, G13 gfxperf, DWM on softpipe). Upstream main now b5696c1 (2026-10-23), tag
  alpha9 (2026-10-20): 938 commits, 820 files, +97k lines. Ours: 152 commits since the fork point.
- Same GPU base (Mesa 26.2 + d3d10umd + panfrost + the malikm shim), developed in parallel: their Mesa series runs to
  0222 (ours 0041-0075 are a different line - same numbers, different patches). Their KMD MaliG76Wddm 1.0.72.1 is
  WDDM 2.0 (tracked fences -> Windows Terminal / XAML), ours 1.0.36 WDDM 1.3.
- Theirs and not ours: D3D11.1 / WDDM 2.0 DDI in d3d10umd, FL 11_0 (SM5, cs_5_0, CPU tessellation, GS emulated as
  compute - GsEmu.cpp, a different design from our port of JimVulkan's), BC1-5 + ETC2 handling, x64/x86 apps,
  threaded context (u_threaded_context in panfrost), per-program shader cache, a native OpenGL ICD (WGL), MFC H.264
  decode (Media Foundation + browsers), 720p mode, brightness, thermal driver + clocks governor (GPU 702 MHz, CPU 1.95 /
  2.4 GHz with a 83 C guard), RAM 5.9 GB, UFS ~500 MB/s, audio (speakers, jack, mics), WiFi 5 GHz, BT, rotation,
  touch keyboard investigation (notes/touch-keyboard.md), dock/USB.
- Ours and not theirs: Beyond1LtePkg (S10 firmware), S10Touch, Exynos9820Hsi2c, panvk Vulkan 1.3 on Windows (their
  status: "DirectX 12 / Vulkan are not supported"), our GS port (gputest 95/0/5), KMD fb-alias present path.
- Decision for the user: stay on our line, or move the S10 port onto upstream alpha9+ and carry only the S10-specific
  pieces + panvk across.

## Merge onto upstream (2026-11-11 13:17, user: "mang của họ sang")
- Merge commit 8604bf2: upstream main b5696c1 (alpha9+) merged into beyond1lte-port; 11 conflicts resolved (see the
  merge message). MaliG76Wddm = upstream 1.0.72 + the S10 board switch (MW_GOP_* 1440 x 3040, MW_FB_RESERVED 18 MiB,
  panel commands / brightness OFF by default on the S10 build - upstream's DECON command-mode switch and AOR (B1)
  brightness were only ever exercised on the S10e's s6e3fa7; the S10 panel is another IC, nothing read on it yet;
  Parameters\PanelCmdMode / PanelBrightness = 1 enable them for a test). vcxproj: /p:ExynosBoardBeyond1, MwNoWx,
  LatestTargetPlatformVersion (EWDK). Compile-checked on Linux with board=1 (tools/kmd-build now takes the source
  list from the vcxproj): 0 errors. Our KMD 1.0.36 (fb alias, WDDM 1.3) is retired; its GPU-half fixes f91fbc8
  (coherency NONE after reset, FLUSH_MEM before AS0 reprogram, JS_COMMAND_NEXT busy check) are NOT in upstream's
  maligpu.c - to re-check against it later.
- Exynos9820Audio = upstream's 1.0.3.7 (they fixed the same choppiness with an 8-slot ring, LEAD 3; defaults off,
  the S10 kit's install script writes AmpInit/AmpEnable/AboxEnable = 1).
- Firmware: FB reservation is a board value (Exynos9820Board.h EXYNOS_BOARD_FB_RESERVED_SIZE: S10e 10 MiB, S10 18 MiB);
  the WT "Display Reserved" HOB now covers the whole reservation. Beyond1LtePkg dsc/fdf synced with upstream's
  Beyond0 changes (SMBIOS 3 entry point, boot timeout 0, GraphicsConsoleDxe dropped, BtUart / SmbiosDxe / Smbios /
  Mfc DXEs). Boot splash: BootSplashBeyond1.h, 1440 x 3040 = the S10e splash x 4/3 (rescale_splash.py, checked by
  eye). SMBIOS: "Galaxy S10" / SM-G973F, 8192 MB, banks 3-5 mapped. TouchPower: S10 DVFS + touch bring-up kept,
  upstream's SCI LLC / M4 soak blocks added (both off by default).
- UEFI v0.2.0 built (Beyond1 DEBUG rc 0; the S10e build rc 0 too): beyond1lte-uefi-v0.2.0.img 7716864 B
  sha256 0f2413de4e3e5b1cc73677dc58d02796bb4090a328e0eb0c16d92f6ba63de1f9. FVMAIN_COMPACT 99 % full (208 B free;
  the S10e build: 1096 B) - the next module needs FV room.
- Mesa: our 0040-0075 line archived in tools/mesa-build/patches-s10-g2 (README). Upstream builds its series with
  clang-cl + MSVC libs on Windows (build-mesa-g76.ps1: ARM64, x86, ARM64EC/ARM64X, GL ICD); no prebuilt releases
  exist. Next: build upstream 0001-0222 on Linux (zig/mingw like our tree, or clang-cl + downloaded MSVC libs).

- 2026-11-11 — UFS on the upstream base: our tree already carries upstream's w43..w47 (async completion, 4
  slots, NEXUS cache) plus the S10 board switch; `git diff HEAD upstream/main -- drivers/Exynos9820Ufs` is only
  the S10 additions. The S10 fence (cache start .. LastUsableLBA, omr/cp_debug refused) does not depend on how
  userdata is split, so a dual-boot layout needs no LBA edits as long as the Windows volume and the ESP sit in
  3118592..31236090 outside omr/cp_debug. PAIRING: UEFI v0.2.0's DSDT has the real UFS line GSIV 281 (upstream
  default since w45); the S10's previous driver (pre-merge) is older than w43 → its ISR returns FALSE → level
  interrupt storm. Order: new UFS driver first (it works on the old parked 259 line: backstop timer, then
  AsyncDisabled → polled), reboot, then flash v0.2.0. Upstream's INF says DriverVer 9.0.0.0 for every build,
  so pnputil would not replace the old one: build-s10.cmd v1.9 stamps 11/11/2026,9.0.47.1. New
  tools/s10-kit/update-ufs.ps1 v1.0 (in-Windows swap: board record check, live-layout fence check, pnputil).
  Parsed with pwsh 7.4; fence/overlap logic checked on 4 synthetic layouts. NOT run on the phone.
- 2026-11-11 — GPU builds on the upstream base (all Linux, zig 0.13, release):
  - `linux-build-g76.sh gl` → out-gl/maligl.dll (upstream's -Stage gl: wgl target, D3D10 UMD off), ARM64, 19 Drv*
    ICD exports, imports GDI32/KERNEL32/USER32/d3d11/dxgi; sha256 bc4c5214…3c57. First try failed linking host
    SPIRV-Tools → `-Dspirv-tools=disabled` in the extra stages.
  - `linux-build-g76.sh x86` → out-x86/maliumd32.dll (WOW64 D3D UMD), i386, image base 0x10000000, OpenAdapter10 /
    _2 exported undecorated from `_OpenAdapter10@4` (disassembly: `ret $4`, stdcall), same export/import set as the
    ARM64 UMD; sha256 d9f84864…3923. Uses the x86_64 host panfrost_compile: new layout-check.py compares the
    serialized records (bifrost_precompiled_kernel_info + the 19 records nested in it, from the host's full layout
    dump) between host pan_compile.c and the target's pan_precomp.c: all equal for x86 and ARM64; the 69 that
    differ on x86 all hold pointers and are never serialized.
  - panvk V2: pv tree branch win-up222 = p0175 + 0001..0005 + 0006 (include/win32-compat replaced by upstream's
    Mesa-0222 shim, drmGetDevices2/drmGetCap/drmDevice re-added); builds and links clean, ICD exports present;
    vulkan_panfrost.dll 712a87e4…1468. The shim's G32 submit queue is flushed by every locked entry incl. syncobj
    waits, which is what malikm_atoms relies on. V1 builds stay tied to the old KMD.
  - Kit: make-kit.sh v2.2 takes UMD_DIR=a:b:c, build-s10.cmd v1.10 copies maliumd/maliumd32/maligl.dll;
    s10-wddm-inf.ps1 with those three present keeps their INF lines and drops only *_arm64 / *_ec (tested pwsh).
  - NOT built: ARM64EC (zig has no arm64ec) → x64-emulated apps still have no Mali D3D/GL. KMD fixes of f91fbc8
    (COHERENCY_NONE, FLUSH_MEM before AS0 reprogram, JS_COMMAND_NEXT check) are not in upstream's maligpu.c; left
    out (upstream's KMD is validated on the S10e without them; revisit if the S10 shows faults/hangs).
  None of this run on the phone yet.
- 2026-11-11 — WU4NK sent their Exynos9820Ufs.h ("don't touch this file"): byte-identical to this branch before the
  upstream merge (8604bf2^1 / 3d2bc20); the current header = theirs + 40 added lines (w40-w47 driver fields:
  UFS_SLOT, async/IRQ state, large-PRDT asserts), no line of theirs changed - every LBA / fence / protected-range
  constant is the same. Their .h cannot build the w47 .c, so they keep the installed (pre-w40) driver:
  build-s10.cmd v1.11 `set S10_KEEP_UFS=1` skips the UFS build.
- UEFI v0.2.1 (sha256 5f9d6f5e…e201): S10 DSDT UFS0 interrupt back on the PARKED line 259 (disassembled: 0x103) -
  upstream's 281 storms with the pre-w43 driver WU4NK keeps; w43+ stays polled on 259. BT role latch ON
  (BEYOND0LTE_BT_UART_ROLE=1 on Beyond0LteBtUartDxe only): DSDT BTH0 present when the DXE latched. S10 DT
  (FdtBlob/beyond1lte.dtb, python fdt) = S10e: bluetooth pinctrl bt-hostwake gpa2-3, bt-btwake gpg2-0, bt-en
  gpm8-0, uart1 = uart@10840000. FVMAIN_COMPACT 3520 B free.
- Audio (user: "secondary speaker not working and still quiet"): by design of the package defaults - Stereo 0
  (earpiece amp @0x41 not powered, (L+R)/2 to the loudspeaker only) and SpkAtten 40 (-5 dB on the loudspeaker; the
  S10e by-ear balance). install-in-windows v1.2 sets Stereo 1 + SpkAtten 0 and restarts the device; analog gains
  unchanged (Rcv 12 / Spk 10; stock music is 17/17 WITH Samsung's DSP protection, which we do not have) - the
  trim app (S10eSpeakerTrim.cs, compiled on the phone with the in-box csc) raises them to 17 at the user's choice.
- Bluetooth (user: "nothing"): Exynos9820Bt was never in the S10 kit and the role latch was off. Now: build-s10.cmd
  builds it when bcm4375\bt\bcm4375B1_semco.hcd exists; get-wifi-fw v1.2/v1.1 pulls bcm4375*.hcd + cid.info and
  copies murata/semco B1 by the module id (INFERENCE: BT patch file follows the WiFi module vendor in cid.info;
  no cid.info -> semco + warning). BD address still the chip default (upstream open item). Not run on the S10.
- 2026-11-11 — gpu-s10 full/gpu on kit v2.x: picked a stale out-gpu\maliumd.dll (2026-11-10, older kit) and
  Rename-Item of System32\maliumd.dll failed "Access denied" (the 1.0.72 package installed it -> TrustedInstaller).
  gpu-s10.ps1 v1.19: with out-gpu\MaliG76Wddm\maliumd.dll present, maliumd/maliumd32/maligl are checked against the
  package and a differing one replaced from it (takeown + icacls Administrators F, rename aside, copy); loose-file
  path keeps the same takeown. Parsed with pwsh 7.4; not run on the phone.
- 2026-11-11 15:55 run (log 3f58783e): "GPU not working". gpubench/gputest ran on 'Microsoft Basic Render Driver'
  (gputest's "100 PASS on the Mali" = WARP vs WARP); vkprobe 0 devices. ACPI\SAMS9828 CM_PROB_FAILED_INSTALL:
  setupapi shows boot 23:16 (phone clock) -> `pnputil /delete-driver oem13.inf /uninstall /force` followed by the
  out\ packages = install-in-windows.ps1 step 1, which still removed malig76wddm/malig76probe (from the parked-GPU
  round). Fixed in install-in-windows v1.3. 16 DWM crashes in that boot: maliumd.dll timestamp 0x6af3b1b2 (NOT the
  kit's 603a7885 = 0x6AF40C7E) +0x18f468 0x80000002 - the stale out-gpu\maliumd.dll gpu-s10 <= v1.18 copied, old
  UMD on the 1.0.72 KMD. Earlier boots with the package's UMD: DWM on the Mali, KMD 50607 jobs 0 failed 0 timed out.
- 2026-11-11 16:09 log 49c3df9b: package back (display 23:27 SUCCESS, UMD 603a7885, FullAdapter 1 GpuEnable 1) but
  GPU0 CM_PROB_PHANTOM: UEFILOG "\S10EDIAG\nogpu.flg present - DSDT GPU0 hidden" (UEFI v0.2.1 booted; GpuProbe
  G76 r0p0 read fine). esp-listing: S:\S10EDIAG\nogpu.flg (created 2026-11-11 09:04 UTC) + an old S:\nogpu.flg.
  gpu-s10 gpu-dwm said "no nogpu.flg on the ESP (\\?\Volume{3b87cd3e…})": Clear-NoGpuFlag returned at the first
  FAT volume that had an S10EDIAG folder - dual boot has more than one FAT partition. v1.21 deletes the flag on
  every lettered drive and FAT volume (simulated with a fake volume in pwsh). No new DWM crash in this log.
- 2026-11-11 16:18 log f981cf7c ("black after Please wait"): the same nogpu boot state as 49c3df9b (flag
  S:\S10EDIAG\nogpu.flg 09:04 UTC still there, WINPRAM-RESTART identical). Nothing from the black boot itself:
  the rescue went through TWRP/Android, so the Mali PRAM was overwritten before UEFI saved it, and upstream's UMD
  never writes C:\ProgramData\S10e\umd\umd-log.txt (that log + its "screen: driver ... maliumd built" lines belong to
  OUR old 0040-0075 series). CORRECTION of the 15:55 entry: the "DWM on the Mali, 50607 jobs 0 failed" lines were
  from those old UMD builds (Nov 4-11 01:50..09:00 stamps), not from the kit's 603a7885 - upstream UMD + 1.0.72 KMD
  in GPU mode on the S10 is NOT shown to work yet. The only 1.0.72 trace: display-only (mode 0, Params 0x311), 945
  PresentDisplayOnly, no error.
  gpu-s10 v1.22 sets machine MALIKM_TGSI_DIR=C:\ProgramData\S10e\umd for full/gpu/gpu-dwm (upstream shim per-process
  logs + crash evidence), removes it for display/off; s10-log v1.11 collects malikm-*. Next run: gpu (DWM softpipe)
  first, then gpu-dwm; rescue without TWRP if possible so the PRAM survives.
- 2026-11-11 17:27 log de246bbc ("still broken"): gpu mode (FullAdapter 1, GpuEnable 1, DWM softpipe) with upstream
  1.0.72.1 + UMD 603a7885: Windows reached the desktop (explorer/StartMenu/Brave running, gpuwatch passes), DWM
  presents (malikm-dwm log: frontbuffer 1440x3040, present primary ok), KMD registry log healthy (Mode 2,
  FbMatchesGop 1, GpuUp 1, ScanoutMap 0 = success, ScanoutGpuVa 0x2000000) - but the panel stays black. The KMD
  PRAM was not readable live (gpuwatch "pram:" empty) and the rescue went through TWRP again, so no latch trace.
  Upstream full-adapter scan-out on the S10 is the open item.
- Fallback: drivers/MaliG76Wddm-legacy = KMD 1.0.36.1 (git 8604bf2^1), the stack validated on the S10 this morning
  with UMD 0075 (build stamp Nov 11 09:00:12 = the umd-log lines of the working DWM runs). build-s10 v1.12
  S10_GPU_LEGACY=1, gpu-s10 v1.23 (display removes every MaliG76Wddm package first; out-gpu\umd UMD with matching
  SHA256SUMS wins), make-kit UMD_LEGACY. Kit v2.4.
- 2026-11-11 18:10 log 9965158f ("still black"): the legacy stack was NOT installed - gpu-report = gpu-s10 v1.23
  "gpu" from the nogpu rescue boot, package UMD 603a7885, KMD registry Build 1 / Mode 2 (upstream), no MaliG76Wddm
  install in setupapi. Same picture as de246bbc (desktop runs, DWM presents, panel black). Cause of the mix-up:
  "display" in a rescue boot (GPU0 hidden) only wrote the mode, never installed the kit's package, and "gpu" did not
  check which KMD was installed. gpu-s10 v1.24: rescue "display" removes every MaliG76Wddm/Probe package and stages
  the kit's (GPU0 binds it next boot); "full"/"gpu" fail when the installed version != out-gpu's.
- 2026-11-11 18:29 log 16391447: legacy KMD 1.0.36.1 now installed (display-only boot fine: FbMatchesGop 1, Presents
  0x400). Then "gpu" (v1.24) took the PACKAGE path: out-gpu\MaliG76Wddm\ still held the 1.0.72 kit's maliumd.dll
  (603a7885; out-gpu copied over, not replaced) -> KMD 1.0.36 + upstream UMD = black. gpu-s10 v1.25: package path only
  when the package INF lists maliumd.dll in [SourceDisksFiles]; a legacy package needs out-gpu\umd\maliumd.dll (fails
  clearly otherwise).
- 2026-11-11 18:51 log 3db2a1c3: back on upstream 1.0.72.1 in DISPLAY-ONLY mode (FullAdapter 0 / GpuEnable 0, Mode 0,
  panel works, 1440x3040, PRAM readable live: HB p514) - out-gpu was a non-legacy build again, so v1.24 "display"
  replaced the legacy package with 1.0.72.1, and the later "gpu" ran from the nogpu boot on 1.0.72.1 + 603a7885.
  Note: in display-only the live PRAM peek works (gpuwatch HB lines); in the full-mode boots it read nothing.
- 2026-11-11 19:12 log c0f0890b: LEGACY STACK RUNNING - KMD 1.0.36.1 (Mode 2) + UMD 0075 f66ae84a in System32, gpu mode
  (DWM on softpipe): panel shows the desktop; user "terribly laggy" = expected for softpipe at 1440x3040 (dwm 18-28 s
  CPU per 20 s pass). KMD HB: vsync ~0x1d88, 0x74 submits, presents rising, no faults. SearchHost restarts every
  ~20 s (pid changes each pass). Next: gpu-dwm (DWM on the Mali), as in the morning's working runs.
- 2026-11-11 19:22 log e6d34eca: gpu-dwm on the legacy stack (KMD 1.0.36.1 + UMD 0075): DWM ON THE MALI again -
  dwm 19 presents/s, 0 failed submits, KMD 0 failed / 0 timed-out jobs, no fault lines; gputest run (umd-test.txt).
- 2026-11-11 19:35 user: UFS (their kept driver), Bluetooth (kit v2.3: Exynos9820Bt + UEFI v0.2.1 role latch) and
  audio (stereo, SpkAtten 0) WORK on the S10; audio still a bit quiet, louder later. Next: S10ePower (clocks/thermal).
- 2026-11-11 19:38 user: focus on the GPU (S10ePower port paused: build.bat EWDK-aware + build-s10 v1.13 :power +
  power-s10.ps1 v1.0 written, not run; DT check done - ACPM 0x15900000/0x2039000, DVFS ch 0, TMU ch 9, LITTLE/MID/G3D
  cal-ids and ladders identical to the S10e).
- Upstream 1.0.72 black-panel hunt, instrumentation: gpu-watch v1.4 logs per pass MaliG76Wddm's stats escape
  (tools\kmdstats.exe, built with zig from the upstream source: presents / latch CPU copies / GPU-presented / skipped),
  a raw PRAM peek line when the trace is missing, 3 framebuffer pixels (0xCA000000, rows 100/1520/2940, centre
  column) and the DPU G0 (0x19071000) / DECON (0x19030000) registers - i.e. is the FB black, is the scan-out still on
  the GOP base. gpu-s10 v1.26 -GpuDir (legacy and upstream builds side by side). Kit v2.5.
- 2026-11-11 20:39 log 0f6e9193 (upstream 1.0.72 gpu mode, black; first run with gpu-watch v1.4 + a usable WINPRAM):
  KMD trace LOAD mode 2, flips SVA addr 0x02000000 (= ScanoutGpuVa), latches run (first ones px 0 = black boot frames,
  at 647 s a desktop frame ff99bdd5/ffeeeeee copied), "fb skips 01000000" (FbVa mapped, 0 skips). Live: framebuffer
  0xCA000000 holds real pixels (lock-screen blues), DPU G0 base 0xCA000000 / 1440x3040, DECON 0x113/0x133 (command
  mode, HW trigger 0x1001), frame counter +0x78 running ~61 fps - all IDENTICAL to the Basic-Display rescue boot whose
  panel works. kmdstats: ~0 presents/s for minutes, then 1.5/s cpu copies. So the scan-out path is not what differs.
- FOUND: upstream's KMD is validated only as WDDM 2.0 - the S10e phone runs Parameters\Wddm2 = 5 (mixed GpuMmu) +
  DirectFlip = 1 (HANDOFF 47-49). Default Wddm2 0 = the old WDDM 1.3 adapter; the upstream UMD expects 2.0 (DWM log
  "CB FAIL CreatePagingQueue 0x80004005"). Every S10 upstream run used the default. gpu-s10 v1.27 sets Wddm2 5 +
  DirectFlip 1 for the upstream package (full/gpu/gpu-dwm), removes them for display/legacy.
- 2026-11-11 20:46 user CORRECTION: the upstream runs were never black - the desktop renders, incomplete, at a
  near-zero frame rate ("0.0000001 fps"). Matches kmdstats (~0 presents/s for minutes) and the KMD near idle: DWM is
  blocked, not the scan-out. Same WDDM 1.3-vs-2.0 suspect (CreatePagingQueue fails on the 1.3 adapter); upstream also
  never runs DWM on softpipe - after "gpu" the real check is "gpu-dwm" (DWM on Panfrost) with Wddm2 5.
- 2026-11-11 21:11 user ran display, gpu, gpu-dwm in ONE boot: gpu wrote Wddm2 5 etc., then gpu-dwm read GpuEnable empty
  (the display reinstall completes at restart and its service key replaced the values). gpu-s10 v1.28: display leaves
  C:\S10Prov\gpu-reboot-pending.txt (boot time); full/gpu/gpu-dwm refuse until the phone restarted.
- 2026-11-11 21:24 user: still "GpuEnable is not 1" after a restart. Cause = my v1.27 bug: Set-Wddm2 ran
  New-Item -Force on the existing Parameters key right after FullAdapter/GpuEnable were written - on the registry
  provider that recreates the key empty, so only Wddm2/DirectFlip survived and the KMD booted display-only. v1.29
  creates keys only when missing (gpu-s10, install-in-windows v1.4 - its audio key lost MicGain etc. the same way -,
  power-s10).
- 2026-11-11 21:48 log 58e5e1a4 (user: "nó bị đen màn rồi") - first S10 boot with Wddm2 5 + DirectFlip 1 + gpu-dwm
  (gpuwatch boot "23:17:55", registry FullAdapter 1 / GpuEnable 1 / Wddm2 5 / DirectFlip 1, collected in a nogpu
  rescue boot). Evidence:
  - Apps are fine on the 2.0 adapter: this boot's LogonUI-376, SearchHost-1648/5524/7356/8280, StartMenu-5552,
    TextInputHost-7372, msedge-3464 logs show "residency: runtime paging queue hr 0x00000000" (the 1.3 adapter's
    CreatePagingQueue 0x80004005 is gone), Panfrost, presents hr 0, 0 CB FAIL.
  - DWM stalls in the first minute: PRAM HB p (presents) frozen at 0x13e = 318, ST l (latches) frozen at 0x75 = 117
    from pass 0 (73 s) on; submits/fences crawl (426 -> 441 in 80 s), queued 0 running 0 (GPU idle, nothing hung);
    kmdstats 0.0 presents/s. Framebuffer 0xCA000000 all ff000000 at rows 100/1520/2940 while DPU G0 still scans
    0xCA000000 1440x3040 and DECON runs (0x133) - the last latched DWM frames were black, then no more.
  - DWM's own UMD log is NOT in the zip: s10-log took the "newest 60" malikm files by time, and the phone's clock
    restarts at the same time every boot (every gpuwatch boot starts ~23:17), so a DWM log that stopped growing early
    lost to the rescue boot's files. The 16 WER AppCrash_dwm (fault maliumd 0x18f468, 0x80000002 misalignment) are
    from an OLDER UMD: fault module timestamp 6af3b1b2 = umd0073; System32 now holds 603a7885 (timestamp 6af40c7e).
    Not this boot. WINPRAM.BIN has no MALIG76T trace (the TWRP/nogpu rescue path replaced it).
  - Tooling: gpu-watch v1.5 logs the running DWM's log size + last 12 lines per pass and copies it to
    C:\S10Prov\dwmlog; s10-log v1.12 always collects malikm-dwm*/LogonUI* + that folder; gpu-s10 v1.30 clears old
    shim logs, dwmlog and dwm/LogonUI/explorer WER reports at every mode switch.
  - Next run: "gpu" (DWM on softpipe on the 2.0 adapter: separates the KMD's 2.0 flip path from DWM-on-Panfrost),
    reboot, look + s10-log; then "gpu-dwm", reboot, s10-log.
- 2026-11-11 22:43 logs c05d240f (gpu) + 68eeafcf (gpu-dwm; user: gpu-dwm black, rescued with nogpu.flg):
  - gpu (DWM on softpipe, Wddm2 5): the desktop reaches the panel (fb rows 100/1520/2940 = desktop colours, latches
    0x12 -> 0x28), DWM alive but ~0.5 presents/s at most (softpipe at 1440x3040).
  - gpu-dwm (DWM on Panfrost, Wddm2 5): dwm#556 log (gpu-watch v1.5 copy) stops at 2821 lines, the same in every pass.
    80 "present primary ... hr 0" up to 33843 ms, then DWM creates a new 1440x3040 render target + CBs, "idle 31 ms
    via event", one more present, "tc: driver thread tid 3676" (the FIRST time the threaded context's driver thread
    runs) and at 35875 "tc: driver thread waits > 2 s for the DDI thread to run callback op 4 (DDI thread last seen
    2016 ms ago, submit queue 0)". op 4 = MK_RENDER (patch 0133 enum: ESCAPE CREATE RESIDENT LOCK RENDER ...): the
    Render callback is marshalled to DWM's DDI thread (MS Learn: runtime callbacks only on the DDI thread unless
    pfnSetAsyncCallbacksCb), and that thread never entered the UMD again = deadlock, DWM frozen, panel black (KMD:
    presents 0x145 / latches 0x54 frozen, GPU idle). The 0175 DDI-thread snapshot printed nothing (log ends there).
    upstream S10e notes (perf-threaded-context.md) show the same message once in a game (nfs-tc1), fixed then by
    T5c (Windows futex timeout); this one is new.
  - gpu-s10 v1.31: "gpu-dwm -NoTc" = machine MALIUMD_NO_TC=1 (Device.cpp switch) for one boot, DIAGNOSIS ONLY (AGENTS
    12): does DWM on Panfrost run without the threaded context? Every other mode removes it.
- 2026-11-11 22:54 log d7538ee5 (user: "nó lên rồi"): gpu-dwm -NoTc (MALIUMD_NO_TC=1), Wddm2 5 - the desktop is up on
  the Mali: kmdstats presents 1.5-2.0/s all "gpu" (no CPU copy), renders/submits 3-6/s, PRAM p 0x63a -> 0x972 and
  latches 0x2cd -> 0x469 keep moving, fb holds desktop pixels, dwm#496 log grows (4538 -> 4787 lines). CONFIRMED: the
  black panel was the threaded-context deadlock.
- Fix (UMD 0222-s10.1): tools/mesa-build/patches-s10/s10-0001-tc-off-by-default.patch - Device.cpp creates the
  threaded context only with MALIUMD_TC=1 (and no MALIUMD_NO_TC); linux-build-g76.sh applies patches-s10/ after
  patches-s10-mingw/. Built incrementally (build-arm64 + build-x86 ninja, layout-check OK): maliumd.dll 1c2871f3,
  maliumd32.dll 0a237e99, both contain the MALIUMD_TC string. maligl.dll unchanged (no d3d10umd). Open: the real cause
  of the DDI thread not coming back (S10e runs DWM threaded) - T-series root cause, not needed for the S10 desktop.
  NOT validated on the device yet.
- 2026-11-11 23:50 user: UMD 0222-s10.1 works ("nó hoạt động rồi") - DWM on the Mali with the threaded context off by
  default (no -NoTc). User directive: finish ALL remaining GPU work before anything else.
- GPU completion plan (in this order): (1) gpu-validate.ps1 battery on the device -> list of real failures;
  (2) "final" everyday mode (gpu-s10 v1.32: shim logging MALIKM_TGSI_DIR + watch/fill tasks off - upstream HANDOFF 50
  "never leave it on"); (3) GPU clocks: S10ePower/S10eGovernor (GPU stays at the boot rate without it; upstream's
  video/mpv judder fix was clocks); (4) Edge edge://gpu / WebGL / YouTube by hand; (5) video decode (Exynos9820Mfc +
  D3D11 video DDI, upstream G17); (6) threaded-context deadlock root cause (perf only).
- gpu-validate.ps1 v1.0 + tools/mesa-build/build-validate-tools.sh: 21 ARM64 tools (d3dcaps msaa cs sm41 sm5 gs so
  tess mrt bc copysrgb cbdyn vbring fence fs, gltest, gpuinfo, dwmrate, vblank, flipseq, gpuclock) + 5 x86 (caps msaa
  cs cbdyn vbring), zig 0.13, d3dcompiler_47 import libs from zig's mingw .def. Not built: d3dtri (overlay source),
  gpucheck (WinMain GUI), d3dswap (goto over init). NOT run on the device yet.
- 2026-11-12 00:50 first gpu-validate run (UMD 0222-s10.1, KMD 1.0.72.1 Wddm2 5, MALIKM_TGSI_DIR still on):
  dxdiag WDDM 2.0, DDI 11.3, FL 11_0..9_1, 1440x3040 60 Hz. PASS: d3dcaps, d3dmsaa, d3dcs 7/7, d3dsm5 34/34, d3dgs 6/6,
  d3dso, d3dtess 9/9, d3dmrt 7/7, d3dcopysrgb, d3dcbdyn, d3dvbring, d3dfence; x86 d3dcaps32 + d3dmsaa32. KNOWN d3dsm41
  7/8 (lod, as S10e). Edge edge://gpu: Canvas/Compositing/WebGL/WebGPU/Video Decode+Encode hardware accelerated,
  Rasterization blocklisted (upstream same), ANGLE D3D11 ES 3.0, D3D11 FL 11_0.
  FAILS and causes:
  - d3dbc (plain) 63/63 DIFF = by design since G26 (BC4/BC5 -> ETC2/EAC re-compression; upstream g27: "plain 63/63
    DIFF = expected"); the right check is "d3dbc lossy" -> gpu-validate v1.1.
  - x86 d3dcs32/d3dcbdyn32/d3dvbring32 0xC0000139 = MY tool build: zig dlltool without -k imported "D3DCompile@44"
    from d3dcompiler_47. build-validate-tools.sh now uses -k (import name D3DCompile, checked with llvm-readobj).
  - gputest 90 PASS / 10 FAIL (legacy stack was 95-100): names cut off by the 25-line tail; B5G6R5 RT 11 px (worst 9).
    v1.1 keeps the whole output.
  - gltest 0/5: GL 3.1 Mesa Panfrost context OK, every draw GL_INVALID_FRAMEBUFFER_OPERATION, "mmap(..., size=262144)
    failed: Invalid argument" = shim IOCTL_MALIKM_MAP -> w_lock -> D3DKMTLock (the ICD's non-runtime D3DKMT path)
    returned an error. Upstream S10e: GLTEST 5/5 on the same path. Diagnostic: patches-s10/s10-0002 logs the Lock
    NTSTATUS (+ the D3DKMT paging-queue line already there); maligl-diag.dll swapped in by gpu-validate v1.1 for gltest.
  - GPU clock 156 MHz (lowest DVFS step: no S10ePower/governor) -> power-s10.ps1 test is the clocks step.
  - 52 app hangs since boot, mostly SearchHost 1002 in the first minute (shim logging on) - recheck after "final".
  - gpu-validate v1.0 bug: "$known" vs "$Known" (case-insensitive names) printed the hashtable.
- 2026-11-12 01:16 gpu-validate v1.1 (zip 65bb14c3): 21 PASS, 2 FAIL, 1 known. x86 tools fixed -> all 5 PASS; d3dbc
  lossy PASS. Remaining:
  - gltest: tlog "kmt Lock 0x400009c0 (device 0x40000180) failed 0xc000000d" (STATUS_INVALID_PARAMETER), 7x, only for
    0x400009c0 = the D3D11 shared texture the ICD renders into ("kmt open shared 0x40000142 -> BO", GL7 DXGI
    presenter); every other BO maps fine. glGetError 0x505 = GL_OUT_OF_MEMORY from the failed map (glReadPixels),
    present runs 56.5 fps. Fix try (s10-0003): D3DKMTLock2 (WDDM 2.0 lock, d3dkmthk.h D3DKMT_LOCK2) when Lock
    refuses, paired D3DKMTUnlock2; both results logged. NOT validated.
  - gputest 10 FAIL: 7x texture-format-BC1/2/3/4/5(+SRGB) = BCn re-compressed to ETC2/EAC by design (G26; gputest's
    tolerance 8 is for native formats); msaa4-resolve / msaa4-load-samples 1.7 % px at triangle edges (worst 115) -
    open, low; blend-dual-source 56 %: pictures show RT0 blended with a CONSTANT src1 (no uv gradient). Cause:
    tgsi_to_nir hard-codes dual_src_blend = false ("no drivers using tgsi-to-nir support dual source blend"), so
    SV_Target1 only goes to DATA1 while Panfrost reports max_dual_source_render_targets 1 and d3d10umd passes SRC1
    factors. Fix (s10-0004): COLOR index 1 is also stored to a DATA0/index-1 output (Panfrost lowers it into RT0's
    blend2_pan, read only when the blend names SRC1). Edge advertises dual-source-blending (WebGPU) -> matters.
  - UMD 0222-s10.2 = s10-0001..0004: maliumd f57416b8, maliumd32 9f588b50, maligl b8b98694 (x86 layout-check OK).
    gpu-validate v1.2 -TryUmd swaps them in for the test run only (DWM keeps its boot copy), -Keep keeps them.
- 2026-11-12 01:32 gpu-validate v1.2 -TryUmd umd-0222-s10.2 (zip 4d7093aa): gputest 91/100 - blend-dual-source now
  PASSES (s10-0004 validated); left = 7 BCn (by design) + 2 msaa4 edge. d3dsm5 33/34: "bits" (countbits /
  firstbit / reversebits CS) all zeros once - the change (FS outputs only) cannot reach a compute shader; flaky or
  real is open (v1.3 repeats "d3dsm5 bits" 3x). gltest still 0/5 BUT it ran on v1.1's maligl-diag.dll (the kit
  folder still had it, the v1.2 script still swapped it in) -> Lock2 untested.
  BUG (mine): v1.2's $Swapped list and v1.1's $swapped flag are the same PowerShell variable -> "RESTORE FAILED for
  True": the phone's System32 maliumd/maligl + SysWOW64 maliumd32 are s10.2, originals in *.s10prev. v1.3: diag swap
  removed, $TrySwapped, -Restore, never overwrites an existing .s10prev, restores by renaming (in-use safe).
- 2026-11-12 01:39 gpu-validate v1.3 -TryUmd s10.2 (zip e048a3f2): 21 PASS, 2 FAIL, 1 known; TryUmd restore OK (all
  three "restored"). d3dsm5 34/34 (the "bits" zero run was a one-off). gputest 91/100 (dual-source PASS; 7 BCn by
  design + 2 msaa4 edge). gltest 0/5: "kmt Lock 0x400009c0 failed 0xc000000d, Lock2 0xc000000d" - the opened
  shared allocation refuses BOTH locks on the D3DKMT device; all other BOs lock.
- s10-0005 (UMD 0222-s10.3): Panfrost panfrost_ptr_map - a linear non-buffer BO whose mmap fails takes the AFBC
  staging path (GPU blit into an own linear staging resource, which maps; unmap blits writes back). Imported
  resources are modifier_constant, so the unmap never swaps the BO. maliumd a6850349, maliumd32 621b3027, maligl
  712ecdb2; x86 layout-check OK. Open (why S10e's lock worked): not needed for the fix. NOT validated.
- 2026-11-12 01:47 gpu-validate v1.3 -TryUmd umd-0222-s10.3 (zip 97738670): 22 PASS, 1 FAIL (gputest 91/100 = 7 BCn by
  design + 2 msaa4 edge), 1 known (d3dsm41 7/8). **GLTEST 5/5 PASS** (59.6 fps swaps) - the Lock/Lock2 refusals are
  still logged, the staging path (s10-0005) carries the readback. No regression vs s10.2. UMD 0222-s10.3 VALIDATED
  (tests; desktop/DWM with it after a reboot still to see). Next: install it for good (-Keep now, kit umd\ +
  build-s10 package later), "final", then clocks (power-s10 test).
- 2026-11-12 02:19 user: "full xong lại lag" (log 970baf77). gpu-report: "gpu-s10.ps1 v1.32 full" -> Parameters
  GpuEnable 0 (GPU half OFF, Mesa on the CPU = the lag), and "full" reinstalled the package's maliumd 1c2871f3
  (s10.1) over the -Keep'd s10.3 (now System32\maliumd.dll.old). "full" is the CPU mode, not "final".
  gpu-s10 v1.33: Install-UmdFiles (the package loop as a function) + -UmdDir <folder> for full/gpu/final; "final"
  works from any full-adapter state (sets GpuEnable 1, removes GALLIUM_DRIVER) and installs -UmdDir; an in-use
  *.old gets a random suffix instead of failing the rename. Fix for the user: final -UmdDir umd-0222-s10.3 + reboot.
- 2026-11-12 03:31 user: drag/resize OK now ("tôi thấy ok rồi"). GPU state on the S10: KMD 1.0.72.1 as WDDM 2.0
  (Wddm2 5 + DirectFlip 1), UMD 0222-s10.3 (TC off, GL Lock2+staging, dual-source), gpu-s10 final, S10ePower +
  governor with G3D 806 MHz (YuROM fvmap slot), S10Touch 1.1.1.0 touch boost. Open GPU items: msaa4 edge diff
  (1.7 % px), threaded-context deadlock root cause (perf only), hardware video decode on the S10 (MFC), long-run
  stability at 806 MHz / 681 mV. Then: louder audio (user, earlier), mic / headphone, brightness.
- 2026-11-12 13:00 gpu-validate v1.3 -TryUmd umd-0222-s10.4 (zip 433945dd): NOT swapped - "*.s10prev exists" for all
  three (s10.3's -Keep left them, by design of v1.3's never-overwrite rule). The run tested the installed s10.3
  (a6850349 / 621b3027 / 712ecdb2): 22 PASS, gputest 91, GLTEST 5/5 - s10.3 re-confirmed. gpu-validate v1.4:
  over a kept install the run backs the current files up to *.s10try (originals stay in *.s10prev).
- 2026-11-12 13:04 gpu-validate v1.4 -TryUmd umd-0222-s10.4 (zip e38c14ea): swapped (572f69d7 / 45b21b15 / 0bd4de69)
  and restored from .s10try - v1.4 works. 22 PASS, 1 known, gputest 91/100 (same 9 as before), GLTEST 5/5; test
  times 5-25 % shorter than s10.3 (threaded context on). UMD 0222-s10.4 VALIDATED by tests; desktop after -Keep +
  reboot next.
