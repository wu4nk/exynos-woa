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
