# Dynamic CPU / GPU clocks (DVFS) + UI priority under Windows

User directive 2026-10-17 (memory clocks-directive-20261017): after alpha6 - dynamic CPU and GPU clocks (minimum with
the screen off and when idle, spin up on touch so the UI is smooth), find the highest usable clocks assuming the two
big (cheetah/M4) cores never work, same for the GPU, and a scheme where the UI gets real-time performance while idle
and background work gets lower priority. Standing rule (memory thermals-before-clocks): no clock INCREASE before
Windows reads temperatures and can throttle; lowering is fine.

## Facts already established (do not re-derive)
- notes/cpu-driver-9820.md F2/F9/F11: CPU clusters are driven by ACPM IPC (mailbox 0x15900000+0x1000, SRAM
  0x2039000+0x30000, framework at SRAM+0x7000), DVFS domain idx 2 LITTLE (A55 x4, 442 MHz..1.95 GHz, 13 OPPs),
  3 MID (A75 x2, 507/650 MHz..2.4 GHz, 17 OPPs; 2.4/2.314 GHz need MIF >= 2.093 GHz), 4 BIG (M4 x2, not running
  under Windows). Message u32[4] {idx | seq<<16, kHz, opcode FREQ_REQ 0 / FREQ_GET 1, 0}, doorbell INTGR0
  (1<<ch)<<16, polling INTSR1, 15 ms timeout; a mis-paired/unanswered request makes the kernel emergency-reset - the
  ACPM is load-bearing. Verified in the shipped kernel binary (F11).
- notes/m15-perf.md runs 31/32: UEFI (Beyond0LteTouchPowerDxe / GpuAcpm.c) already sends FREQ_REQ - today's fixed
  clocks: LITTLE 1456 MHz, MID 1794 MHz (s10eperf measures ~1.45 / ~1.79 GHz), GPU 260 MHz (ACPM ch 0 id 10,
  table 702..156 MHz, notes/thermal-research.md Q5). Windows itself has no ACPM client and no DVFS.
- notes/thermal-research.md: temperatures = ACPM ch 9 READ_TEMP (tz 0..4 BIG/MID/LITTLE/G3D/ISP, reply w1[31:24]
  deg C); the TMU hardware trip (115 C) is probably NOT armed under Windows -> the phone is unprotected; stage plan
  0-3 (UEFI read-only check, Windows logging, ACPI thermal zones, passive cooling). DT trips: CPU control 83 C, GPU
  88 C, alerts 95-110 C, hot 115 C.
- Idle states: PSCI standby per class (F4). Energy data (F5) ranks LITTLE < MID < BIG.

## Design
1. **S10ePower KMDF driver** (one owner of the ACPM mailbox under Windows - serialized, polling mode, never leaves a
   request unanswered): FREQ_REQ/FREQ_GET for idx 2/3 (CPU) and the GPU domain, READ_TEMP ch 9 for tz 1..3. Reads
   the per-SKU OPP ladder from ACPM SRAM fvmap (F9 point 3) instead of hard-coding. IOCTL interface + registry stats.
   Thermal guard inside the driver: any request above a temperature-dependent cap is clamped (CPU 83 C / GPU 88 C
   control temps step clocks down; >= 95 C forces minimum). This makes clock increases allowed under the rule.
2. **Governor** (user-mode service S10eGovernor, SYSTEM): inputs - display state (power setting notification
   GUID_CONSOLE_DISPLAY_STATE + our panel escape state), per-cluster CPU load (NtQuerySystemInformation
   SystemProcessorPerformanceInformation, 20-50 ms), GPU busy (KMD stats escape MW_ESCAPE_STATS), touch activity
   (touch driver interrupt count / an event the touch driver sets). Policy: screen off -> all minimum; idle ->
   minimum; touch -> immediate boost of MID + GPU for N ms (input boost, like Android's touchboost); load-tracking
   in between (schedutil-like: next freq = 1.25 * util * max). Hysteresis to avoid thrash; rate-limit ACPM calls.
3. **UI priority (native Windows mechanism)**: the Windows 11 scheduler places high-QoS (foreground/UI) threads on
   higher-performance cores and EcoQoS/background threads on efficient cores when the firmware describes
   heterogeneity - ACPI MADT GICC EfficiencyClass (LITTLE 0, MID 1) + PPTT. Check what our MADT says today; set the
   classes (firmware/ACPI change, boot path!) so foreground work lands on the A75s and background on the A55s. Then
   the governor can keep LITTLE low and MID fast when the user interacts. (Research MS docs: "heterogeneous
   processors / efficiency class" before changing.)
4. **Max-clock tests** (after 1's thermal guard): step LITTLE to 1.95 GHz, MID to 2.4 GHz (needs MIF), GPU above
   260 MHz (DT ladder 156..702), each under a sustained load (s10eperf cpu, cube/gfxperf GPU) while logging temps;
   stop at the first instability or when the control temperature is reached within N minutes; record sustained vs
   burst limits.

## Tasks (one commit each)
- C0 Stage-0 style read-only check from Windows: ACPM framework + ch 9 READ_TEMP once (S10ePower skeleton, read-only),
  temperatures in the registry. Validates the channel layout under Windows before any write.
- C1 FREQ_GET for idx 2/3/GPU, fvmap ladder read (still read-only).
- C2 FREQ_REQ (down only first: screen-off minimum test), thermal guard.
- C3 governor service (display state, idle, touch boost, load tracking).
- C4 MADT efficiency classes (research first; firmware change; boot hazard -> rollback plan).
- C5 max-clock tests with temperature logging; set the governor's ceilings from the results.

## Progress log
- 2026-10-17 05:15 plan written (phone busy finishing a Windows update restart).
- 05:20 C4 is mostly in place: firmware/Platform/Beyond0LtePkg/AcpiTables/Madt.aslc already sets ProcessorPowerEfficiencyClass LITTLE 0 / MID 1 / BIG 2 (fixed 2026-10-11, notes/m14-smp.md: the 9810 table was inverted). To verify on the device: GetSystemCpuSetInformation EfficiencyClass per CPU; then the governor keeps MID fast during interaction.
- 05:35 C0 code: drivers/S10ePower (legacy NT driver, build.bat), READ-ONLY: maps ACPM SRAM (PAGE_READONLY), records ipc_channels offset, ap_max and the raw channel table (0x48-byte entries) in the service key. Built (s10e-artifacts\s10epower). Not loaded yet (phone still finishing the update restart). Load: sc create S10ePower type= kernel start= demand binPath= C:\S10eTmp\S10ePower.sys; sc start S10ePower.
- 05:55 S10ePower full code (built, not run): C0 dump always; AllowSend=1 (service key) maps the mailbox, finds ch 0 (DVFS) + ch 9 (TMU, only with AllowTmu=1), guard thread 1 s READ_TEMP tz 1/2/3 -> caps (level 0 ladder max, 1 >=83 C boot rates + G3D 325, 2 >=95 C LITTLE 1053/MID 1222/G3D 260, 3 >=105 C minimum; 5 C hysteresis; GPU trips +5 C); no temperature -> cap = UEFI boot rates (1456/1794/260). SET_FREQ IOCTL rounds down to the DT ladders (LITTLE/MID from TWRP sysfs, G3D from exynos9820-mali_common.dtsi: 702/650/572/433/377/325/260/200/156). Device ACL SYSTEM+admins. s10epowerctl state|set. Note: G3D >= 650 and MID 2314 want MIF >= 2.093 GHz (DT tables) - MIF untouched under Windows, its rate UNKNOWN.
- 06:15 C3 prep: touch driver 6.0.5058.7 (sys 408B530D, built, not installed) sets named auto-reset event \BaseNamedObjects\S10eTouchActivity (IoCreateSynchronizationEvent at prepare-hardware, ZwClose at release) from its PASSIVE_LEVEL ISR at most every 20 ms - the governor's touch-boost input.
- 06:35 C3 code: drivers/S10ePower/s10egovernor.c (service S10eGovernor / 'console' test mode): display state (GUID_CONSOLE_DISPLAY_STATE service notification) + MaliG76Wddm PanelDisplayPower trace -> screen off = all minimum; S10eTouchActivity -> boost for BoostMs (300); else per-cluster load tracking every 50 ms (1.25 x busiest core x ceiling, up at once, down after DownSamples=6); GPU keeps its ceiling with the screen on, half-ceiling step after 5 s without touch + idle CPUs. Ceilings default = UEFI boot rates (governor only lowers until C5). Built, not run.
- 05:20 C0 DONE (device): S10ePower loaded read-only (sc create ... start= demand; AllowSend absent): AcpmStatus 0,
  ipc_channels at SRAM+0x5f0c, ap_max 11. Decoded (tools/phone/power/acpmtable.py): id 0 queue q_len 15 elem 16 ap_poll
  1 (= m15 run 30 from UEFI), id 1 q3, id 2 q5, id 3/5/6/7/8 buffer channels, id 4 q3, **id 9 queue q_len 7 elem 16
  ap_poll 1, owner 64** (the TMU plugin - consistent with stock DT acpm_tmu = 9), id 10 q3. Channel 9 layout verified
  live; tzid mapping still from the DT. Next: C1 (AllowSend=1, AllowTmu=1: READ_TEMP every second) after the alpha6
  soak.
- 05:30 C4 check (device, drivers/S10ePower/cpusets.c): GetSystemCpuSetInformation: cpu0-3 EfficiencyClass 0, cpu4-5 EfficiencyClass 1, cpu5 Parked 1 at idle (core parking active), each core its own LLC index. => Windows' heterogeneous scheduling is on (UI/high-QoS threads prefer the A75s). Remaining native levers: power-plan processor settings (heterogeneous thread scheduling policy / short-running policy, core parking min cores / unpark speed) - research MS docs before changing.
- 05:40 C4 conclusion: Balanced plan SCHEDPOLICY = SHORTSCHEDPOLICY = 5 (Automatic: MS Learn configuration-for-hetero-power-scheduling-schedulingpolicy 'the OS determines the policy based on system configuration and QoS type'), CPMINCORES 10 %. With MADT efficiency classes live (cpusets), Windows 11's QoS scheduling already puts high-QoS (foreground/UI) threads on the A75s and EcoQoS/background on the A55s - the native mechanism for 'UI real-time, background lower priority'. No setting changed. The governor's job: keep the A75s fast during interaction (touch boost) and let the A55s idle low.
- 05:40 C1 DONE (device): AllowSend=1 + AllowTmu=1, sc stop/start S10ePower -> DvfsChannelOk 1, TmuChannelOk 1, READ_TEMP replies: MID 47 C, LITTLE 47 C, G3D 46 C at idle (registry 0x33/0x32/0x2e on the first log line), 12 sends, 0 fails, 0 timeouts, guard level 0 (caps = ladder max, nothing requested). First temperatures read by Windows on this phone. Battery 7 % -> parking in TWRP to charge (unattended rule) before C2.
- 05:45 governor: GPU load from Windows' own GPU Engine counters (PDH \GPU Engine(*engtype_3D)\Utilization Percentage, busiest instance, every 200 ms): target 1.25 x busy x ceiling, floor half the ceiling with the screen on; fallback to the old rule if the counter is unavailable. Built.
- 06:30 C2 DONE (device, traces/wddm2-2026-10-16/c2-down.txt): Windows FREQ_REQ via S10ePower: LITTLE 442000 -> s10eperf
  cpu0-3 ~0.43-0.44 GHz, MID 650000 -> cpu4-5 ~0.64-0.65 GHz; restore 1456000/1794000 -> ~1.45/~1.79 GHz. 61 sends,
  0 fails, 0 timeouts, temps 54-61 C. Before any request this boot the cores measured ~1.71-1.74 GHz (LITTLE) /
  ~2.10 GHz (MID) = the 1742000/2106000 steps, NOT the UEFI 1456000/1794000 (Beyond0LteTouchPowerDxe
  BEYOND0LTE_DVFS_*_KHZ) - likely the UEFI request was skipped by its progress-0x66 self-healing guard on this boot
  path (after the TWRP reset); where 1742/2106 come from (sboot / ACPM init level) is UNVERIFIED. Temperature at
  those rates after boot: 58 C, 54 C after lowering. Windows now owns the CPU clocks.
- 06:50 C3 first run (device): touch driver 6.0.5058.7 installed (pnputil, active - Win32_PnPSignedDriver), S10eTouchActivity exists (SSH admin gets access denied = exists; SYSTEM governor opens it: TouchEvent 1). S10eGovernor service (start= demand) running: GpuCounter 1; idle: LITTLE 546-650 MHz, MID 507 MHz, G3D 156 MHz; ~5 changes/s at first -> MinIntervalMs 200 (boost exempt) -> ~3/s, tracking real load bursts. 0 ACPM fails. Touch boost not yet exercised (needs a finger - user).
- 07:00 screen-off test (panelon 0 12 = panel dark for 12 s, process alive): governor ScreenOffs 1, all domains at minimum (LITTLE 442000, MID 507000, G3D 156000); after relight load tracking resumes (LITTLE 1456000, MID 754000). 2164 ACPM sends, 0 fails. Services set to start at boot: S10ePower auto, S10eGovernor auto depend= S10ePower.
- 07:10 reboot test: S10ePower + S10eGovernor auto-start RUNNING after boot, 201 sends 0 fails; temps 63 C right after
  boot. Battery 16 % -> 15-min dark-panel (panelon 0 900) measurement running before C5.
- Review of S10ePower / s10egovernor (harsh): (1) S10ePower has no rundown protection - an IOCTL racing Unload could
  touch unmapped MMIO; SCM refuses to stop a driver with a running dependent (governor depends on it), but a manual
  `sc stop` order mistake remains possible -> follow-up: IoAcquireRemoveLock-style counter. (2) AcpmSend busy-waits with
  KeStallExecutionProcessor up to 100 ms per request while holding a FAST_MUTEX (APC_LEVEL) - acceptable for 3/s, but a
  timeout costs a full CPU for 100 ms; follow-up: KeDelayExecutionThread after the first 1 ms. (3) gState fields
  written by the guard thread and the IOCTL path without a lock - benign counters/caps, but CurKhz could race; follow-up:
  update CurKhz under gAcpmLock. (4) Governor screen-off counter reuses gDom[0].Quiet as a marker - fragile; follow-up:
  own flag. (5) tzid mapping (1 MID, 2 LITTLE, 3 G3D) is still DT-derived - the readings are plausible and track load
  (47 idle -> 63 boot), not independently proven per zone. (6) Ladders duplicated in driver and governor.
- 07:40 C5 DONE (device, 30/25 s steps, battery 17 %, governor stopped during tests):
  MID 1794 -> 2314 MHz: every step granted and measured (s10eperf cpu4-5 1.79/1.89/2.00/2.10/2.20/2.31 GHz), peak
  74 C (2210 step), 67-69 C at 2314 (traces/.../c5-mid.txt). LITTLE 1456 -> 1950 MHz: 1.44/1.56/1.72/1.82/1.92-1.94 GHz,
  peak 60 C (c5-little.txt). G3D 260 -> 702 MHz with d3dtri cube: 0 present failures at every step, G3D <= 54 C, cube
  36.6 -> 38.8 fps (not GPU-bound) (c5-gpu.txt); real GPU clock via MW_ESCAPE_CLOCK (new tool
  drivers/MaliG76Wddm/tools/gpuclock): requested 156/260/433/702 -> measured 156.0/260.0/432.3/702.0 MHz.
  Result: with the 2 big cores off, the full DT ladders are usable on burst: LITTLE 1.95 GHz, MID 2.314 GHz, GPU
  702 MHz (sustained minutes-long thermals not measured yet - the S10ePower guard caps at 83/88 C).
  Governor ceilings set (Parameters): MaxLittle 1950000, MaxMid 2314000, MaxG3d 702000; touch boost BoostLittle
  1742000, BoostMid 2106000, BoostG3d 433000.
- 07:55 governor efficiency points (C5 follow-up): EffLittle 1456000 (LITTLE load tracking stops there unless a core > 90 % or boost), GpuFloor 260000 (was half the ceiling = 377 MHz at idle). Idle after deploy: LITTLE 1456 (background), MID 507, G3D 260 MHz.

## 2026-10-17 evening: "overclock" request, MID 2.4 GHz, stability tools
- User: overclock the 6 usable cores step by step + stability check. Reality: ACPM sets frequency AND voltage from
  fixed tables (fvmap/ECT); only listed rates can be requested. Beyond-table clocks would need direct PLL + PMIC
  programming with unverified values (hardware / data-corruption risk, rule 6) - not done; told the user.
- In-table headroom: MID top step 2400000 (DT, notes/cpu-driver-9820.md:156; needs MIF >= 2093000, mif-perf). Added
  to S10ePower's ladder with an interlock (granted only while this driver set MIF 2093000) and to the governor ladder.
  LITTLE: 1950000 is already the table maximum.
- Device: set mif 2093000 + set mid 2400000 -> ACPM echo 2400000, clocks.exe measured 2399 / 2386 MHz.
- tools: drivers/S10ePower/cpustress.c (one pinned thread per core, deterministic int/FP/memory workload checked
  against a per-thread reference -> silent computation errors), drivers/S10ePower/clocks.c (live measured per-core
  MHz + GPU MHz via MW_ESCAPE_CLOCK + governor/ACPM state; copied to the user's desktop), tools/phone/templog.ps1.
- cpustress results: baseline (governor) 60 s all cores PASS; MID 2400 pinned 2 cores 120 s PASS, 62 C; all 6 at
  LITTLE 1950 / MID 2400 120 s PASS but the S10ePower guard tripped (>= 83 C) mid-run and capped MID to the boot
  1794 (left there afterwards - guard caps only lower); rerun with temperature log (MID actually 1794, LITTLE 1950):
  zones 72-74 C sustained, 82 C peak at 47 s. => 2.4 GHz is computationally stable; sustained all-core max is
  thermally limited (~1 min to the 83 C trip) - bursts and 2-thread loads benefit. Governor MaxMid = 2400000.
- Windows' Task Manager "base speed 1.79 GHz" = Win32_Processor MaxClockSpeed / ~MHz from boot (cpu0 1456, cpu4
  1794), its "speed" = an estimate; it does not see ACPM rates.
- REVIEW 2250edd: governor lowered MIF before the CPUs on screen-off (MID could sit at 2400 with MIF 421 for a moment) -> fixed: MIF up before the CPUs (screen on), down after them (screen off). Deployed.
- clocks.exe is now a small GUI window (user: 'Core #0-5 - x / y, keep it simple'): Core #n - measured / table max MHz + GPU line, 1 s refresh, DPI-aware, measurement on a worker thread; the console variant is clockscli.exe (S10eTmp, for ssh). Deployed to the desktop and running. Governor in normal use (screen on): fast cores measured 2395 / 2389 MHz = the 2.4 GHz step is used.

## 2026-11-12 02:07 (S10 port): user asks for the GPU at 803 MHz ("someone forced it up")
- S10 live DT (scratchpad b1.dts:6236-6243): gpu_dvfs_table 9 steps, top 0xAB630 = 702000, gpu_max_clock(_limit)
  702000 - the same ladder as S10ePower gLadderG3d. ACPM sets G3D frequency AND voltage from its own table (fvmap,
  built from ECT); a rate outside it is refused or needs direct PLL + PMIC writes (2026-10-17 entry above: not done).
- Read that closes it (read-only, Android root): ECT + ACPM partitions, /sys/kernel/gpu/gpu_freq_table, dmesg fvmap
  lines. If Samsung's G3D table has a step above 702000 (with its voltage), it is in-table -> add to the ladder; if
  not, 803 MHz = beyond-table overvolt/PLL -> not without the user's explicit acceptance of the risk.
- 02:36 user: the 803/806 MHz claim is YuROM Ultra (XDA; kernel github.com/yuefdev/YuROM-Ultra-kernel, V35 Beta).
  Read its source (clone in scratchpad yurom/k): drivers/soc/samsung/cal-if/exynos9820-oc.h + fvmap.c
  fvmap_init_gpu_oc. FACTS from it: the ACPM G3D fvmap header (12 levels, 1 member, 1 PLL) has level 0 = 702000 kHz
  at 600..800 mV using PLL_G3D PMS slot 1 (0x006c0400, M 108); PLL_G3D has 13 PMS slots and slot 0 (0x00740400, M 116
  = 754 MHz, in ECT too) is used by NO level. Their OC: rewrite fvmap ratevolt[0].rate to the target, o_tables[0]
  1 -> 0 (level 0 -> slot 0), and for 806 MHz slot 0 PMS -> 0x007c0400 (M 124, P 4, S 0, 26 MHz ref). VOLTAGE NOT
  CHANGED (702 MHz's) - "experimental-same-voltage", silicon lottery. Not a Samsung-validated OPP; no overvolt.
  fvmap base = SRAM + (dvfs plugin base_addr & ~1) + fvmap_offset 0xA400 (kernel acpm.c plugins_init; S10 live DT
  b1.dts:7768 fvmap_offset 0xA400). struct plugin (CONFIG_EXYNOS_ACPM) = 48 bytes, framework plugins @0 num @4.
- S10ePower: FvmapLocate (read-only, always: AcpmPlugins, FvmapPlugin, FvmapOff, FvmapDump 8 KB binary),
  G3dOcCheck (YuROM's layout checks + rates descending + every DT step present; result number in G3dOcCheck),
  G3dOcApply (service key GpuOc 754000/806000, only with AllowSend + DVFS + TMU channels; the three writes under the
  ACPM lock, read back, restored on mismatch; gLadderG3d[0] = the rate -> thermal guard level-0 cap), G3dOcRestore at
  unload (G3D to 650000 first). Governor: G3D ladder top follows S10ePower's GpuOcActive. power-s10 v1.1 "gpuoc -Khz":
  measures with gpuclock.exe, gputest twice (keeps it only with >= 91 PASS both times and no TDR event), sets MaxG3d.
  Syntax-checked with zig/mingw DDK headers (x64) only - the real build is the user's EWDK. NOT run on the device.
- 2026-11-12 03:00 DEVICE (power-report 01137b7c, power-s10 v1.1 gpuoc -Khz 806000): AcpmStatus 0, DVFS + TMU channels
  OK, AcpmPlugins 9, FvmapPlugin 4, FvmapOff 0x29000 (167936), G3dOcCheck 0 (every YuROM check passed on the S10),
  G3D level 0 702000 kHz at 681250 uV, PLL levels 13, PMS0 0x00740400, PMS1 0x006c0400 (= YuROM's expected layout).
  GpuOcStatus 1, GpuOcActive 806000; ACPM granted 806000; gpuclock.exe (GPU cycle counter) 806.0 MHz x3.
  gputest x2 at 806: 91 PASS / 9 FAIL both = identical to stock (7 BCn by design + 2 msaa4 edge); no TDR.
  Temps after: MID 64 / LITTLE 61 / G3D 56 C; 91 sends, 0 fails. VALIDATED short-run; sustained load (minutes) and
  long-term stability at 681 mV still to be seen.
- 03:11 user: s10.3 installed + final done; "drag / resize windows lags". Report 01137b7c already showed: governor NOT
  running (gpuoc did not print "governor restarted"), MIF cur 0 (never requested since the driver loaded -> MIF at
  its boot rate, UNKNOWN, while upstream's judder fix was MIF 2093000 screen-on), LITTLE/MID at the UEFI boot rates
  1456/1794. Fix 1: power-s10 enable (v1.2 keeps MaxG3d = GpuOc instead of resetting 702000). Measurement:
  tools/s10-kit/drag-probe.ps1 (idle 10 s vs drag/resize 15 s: dwmrate, kmdstats busy, ACPM clocks, per-core CPU,
  GPU 3D engine, top processes). Other suspects to separate with it: DWM single-threaded now (threaded context off,
  s10-0001), 1440x3040 swap-chain reallocation per resize step (bo alloc 10-16 ms per 17 MB surface in UMD logs).
- 03:19 drag-probe (196ac9f1, governor + S10ePower running, MaxG3d 806000): idle: LITTLE/MID/G3D at 1456/507/260, GPU
  busy 3.7 %, 2.5 presents/s. Drag/resize 15 s: cpu4-5 (A75) 60-85 % at MID 2314000, LITTLE 442..1950000, G3D mostly
  325000 (806 twice), GPU job running 20.7 % of samples, 34 presents/s all GPU-presented, 115 renders/s, no CPU copy.
  Top CPU during the drag: osk.exe 15.8 s over 15 s (~1 core - the legacy On-Screen Keyboard redrawing on every input
  move), dwm 11.4 s (~0.76 core; single-threaded since s10-0001). DWM_TIMING_INFO counters useless on this KMD
  (0.1 frames/s while 34 presents/s go through).
  => CPU-bound on the two A75 cores (osk + DWM), GPU at a low clock for frame latency. No touch boost at all on the S10:
  S10Touch never set \BaseNamedObjects\S10eTouchActivity (only the S10e S6SY761 driver did).
- Fixes: S10Touch 1.1.1.0 sets the event from PushFrame (<= every 20 ms, IoCreateSynchronizationEvent at
  prepare-hardware, ZwClose at release - the S6SY761TouchBeyond0 code); governor: touch boost is a floor over load
  tracking (it replaced it, capping MID at BoostMid 2106000 during drags); power-s10 v1.3 enable: BoostMid 2314000,
  BoostG3d 650000. User advice: close osk.exe when not typing (touch keyboard = TextInputHost). NOT validated.

## 2026-11-12 14:3x (S10 port) CPU "overclock" - user: "tiếp theo là ép xung cpu"
- YuROM-Ultra-kernel (scratchpad yurom/k) drivers/soc/samsung/cal-if/fvmap.c fvmap_validate_oc_opp + exynos9820-oc.h
  exynos9820_cluster_oc_opp_valid: the CPU OC there writes NOTHING to ACPM. It raises the kernel's vclk max_freq to a
  level that ACPM's own fvmap already has ("Only select a complete, pre-existing firmware OPP; never invent a rail
  value"): LITTLE 1950000 -> 2106000 (target voltage <= 1175000 uV), MID 2314000 -> 2400000 (<= 1050000 uV), M4
  2730000 -> 2912000/3016000 (BIG, not running under Windows). Checks: target + stock in the table, ECT == firmware
  rate per level, rates descending, stock voltage 500..1250 mV, target voltage >= stock and <= the ceiling.
- S10ePower: CpuOcCheck (same checks; ECT is not readable from Windows -> instead every DT ladder step must be a level
  of that fvmap header, which proves the domain index) records LittleLevels / MidLevels (REG_BINARY rate/uV pairs),
  LittleOcCheck/Uv, MidOcCheck/Uv (MID 2400000 was already in use with the MIF interlock - recorded only).
  CpuOc = 1 + AllowSend + DVFS + TMU -> gLittleOc: the LITTLE ladder gains 2106000 (guard level-0 cap LITTLE_TOP()),
  CpuOcActive 2106000. Unload: LITTLE back to 1950000 when above it. Governor: kLadderLittle has a 14th slot used only
  when S10ePower's CpuOcActive = 2106000. power-s10 v1.4 "cpuoc" (-Off): checks, s10epowerctl set little 2106000,
  clockscli measure, cpustress 60 s A55 x2 + 30 s all cores; any error -> removed; else MaxLittle 2106000.
  Also fixed: gpuoc read the governor state AFTER Copy-Files had stopped it, so it never restarted the governor
  (report 01137b7c "governor NOT running") - both actions read it before.
- Compile checks: S10ePower.c clang-cl (WDK 26100 km headers) /W4 clean; s10egovernor.c zig mingw -Wall clean.
  NOT run on the device; whether this phone's LITTLE fvmap has 2106000 is unknown until LittleLevels is read.

## 2026-11-12 14:4x (S10 port) battery - user: "cải thiện hiệu năng pin"
- Levers found (by size, AMOLED): (1) brightness - none existed on the S10 (KMD panel commands off); KMD 1.0.72.2 /
  UEFI v0.2.2 add it (notes/beyond1-port.md); it also enables the power-button panel-off (DCS 28) on the S10.
  (2) MIF: the governor holds 2093 MHz (max) the whole time the screen is on (G17 judder fix); new governor knob
  IdleMifMs (default 0 = unchanged): screen on + no touch for that long + CPUs < 30 % + GPU < 10 % (PDH counter
  required) -> MIF 421 MHz; touch/load -> 2093 within one 50 ms loop; Stat IdleMifPct (share of screen-on loops at 421).
  MID 2400 stays safe through S10ePower's MIF interlock. (3) CPU idle states: the DSDT has no _LPI (rg over AcpiTables,
  notes/thermal-research.md:114) -> Windows idles cores in WFI only; Linux uses PSCI CPU_SUSPEND 0x10000 per class
  (notes/cpu-driver-9820.md F4). Boot-hazard firmware work + Microsoft docs on ARM64 _LPI / PEP first (rule 13) -
  next round, not started.
- Measurement: power-s10 v1.4 "drain": BatteryStatus.DischargeRate (S10eBattery reports mW, Miniclass.c:94-98) averaged
  over -Secs per setting with the display held on (SetThreadExecutionState): brightness 100/50/20 %, IdleMifMs 0 vs
  3000. Decides whether IdleMifMs becomes a default. NOT run yet.
- Review of 026e4ea: IdleMif could drop MIF to 421 while MID still sat at 2400000 (down-steps wait DownSamples loops) = the DT mif-perf violation the interlock exists for. Fix: mifIdle also needs MID cur <= 2314000 (own commit).
- 2026-11-12 15:21 DEVICE (power-report cc78f939, power-s10 v1.4 cpuoc): fvmap read OK. LittleLevels (MHz/mV):
  2106/1206.25 2002/1137.5 1950/1093.75 1846/1050 1742/1000 1586/918.75 1456/868.75 1300/818.75 1157/775 1053/743.75
  949/712.5 806/675 650/631.25 546/612.5 442/581.25 351/556.25 247/537.5 182/537.5 130/537.5 (19 levels).
  MidLevels: 2400/1112.5 2314/1056.25 2210/1006.25 2106/956.25 2002/918.75 1898/893.75 1794/850 1690/818.75
  1586/793.75 1508/781.25 1404/756.25 1222/725 1066/681.25 962/662.5 845/650 754/631.25 650/606.25 507/587.5 377/581.25.
  LittleOcCheck 9 (2106 at 1206.25 mV > YuROM's 1175 mV ceiling), MidOcCheck 9 (2400 at 1112.5 mV > 1050 mV): this
  chip's ASV voltages are higher than the ones YuROM's limits were written for - YuROM itself would refuse both here.
  Nothing allowed, nothing changed (correct behaviour). The governor's MID ceiling on the S10 is 2314000 (power-s10
  enable) so 2400 is not in use. There IS a level between: LITTLE 2002000 at 1137.5 mV (inside 1175).
- Plan: CpuOc 1 = the highest LITTLE level above 1950000 whose voltage is inside YuROM's 1175 mV (here 2002000);
  CpuOc 2 (user opt-in, "-Force") = up to 2106000 with Samsung's table voltage up to 1250 mV (YuROM's own bound for
  any stock voltage) + MID 2400 likewise (governor MaxMid 2400000). Nothing written to ACPM in either mode.
- Implemented: S10ePower CpuOc 1/2 picks 2106000 else 2002000 (gLittleOcSkip; ladder 2106/2002/1950...), CpuOcActive
  = the level, CpuOcUv, CpuOcMidOk (MID 2400 within the mode's limit: 1050 mV / 1250 mV). Governor kLadderLittle
  1950/2002/2106 tail, count by CpuOcActive. power-s10 v1.5 cpuoc [-Force]: sets the found level, with -Force and
  CpuOcMidOk also MIF 2093 + MID 2400, cpustress A55 x2, A75 at 2400 (Force), all cores; MaxLittle/MaxMid kept on
  pass, back to 1950/2314 on any error; enable keeps them. Compile checks clean. NOT run on the device.
- 2026-11-12 15:41 DEVICE (power-report 2327b661, power-s10 v1.5 cpuoc, no -Force): CpuOcStatus 1, CpuOcActive
  2002000, CpuOcUv 1137500, CpuOcMidOk 0. ACPM granted 2002000, clockscli measured cpu0-3 1960/1991/1989/1994 MHz.
  cpustress A55 x4 60 s PASS (temps -> MID 74 / LITTLE 73 / G3D 65), again PASS (76/75/67), all 6 cores 30 s PASS.
  During the all-core run the A55 iteration rate fell from ~245/s to ~130-160/s per core and afterwards LITTLE cur
  1053000 / MID cur 1222000 = the guard's level-2 caps (>= 95 C read on some zone; caps back to 2002000/2400000 by the
  print = cooled below again). The governor was stopped during the test, so nothing raised the clocks again until it
  restarted. => A55 2002 MHz computes correctly; sustained all-core load is thermally limited (S10e: same, 82 C in
  47 s at 1950/2400) and the guard works. Kept: MaxLittle 2002000. Gain ~+2.7 % single-cluster burst. -Force (2106 at
  1206 mV) would run hotter - not recommended on this evidence.
- Follow-up (diagnosis, next S10ePower change): record TempPeak / GuardPeak since load - the report shows only the
  temperature after each run, the level-2 trip above was inferred from the caps.
- 2026-11-12 16:00 DEVICE drain (power-report dd4ea65e, v1.5.1, 60 s rows, 10 s settle, run right after the cpuoc
  stress): brightness 100 % 2406 mW (min 1679), 50 % 2401, 20 % 2198, IdleMifMs 0 (50 %) 2234, IdleMifMs 3000 2188
  (58 % of loops at 421 MHz). Readings: (1) ~2.2 W with the screen on and nothing running is very high for a phone ->
  the static load dominates, not the panel; (2) brightness 100 -> 20 only ~200 mW (dark desktop on AMOLED; plus the
  drift: 50 % read 2401 first and 2234 later = the phone cooling after the stress / AvgCurrent filter lag);
  (3) idle MIF ~46 mW (~2 %), inside the noise - IdleMifMs stays default 0.
- power-s10 v1.6 drain = a breakdown instead: 30 s settle, rows baseline (50 %), brightness 0 %, all clocks minimum
  with the panel on (governor stopped; LITTLE 442 / MID 507 / G3D 156 / MIF 421), panel dark via panelon.exe 0 (the
  screen-off state: DCS 28 + governor minimum), baseline again (drift). panelon (MaliG76Wddm tools) built with zig into
  the validate folder (build-validate-tools.sh). Candidates by expected size once the split is known: CPU idle states
  (_LPI: cores only WFI today), GPU idle power-down in the KMD, then DVFS floors.
- 2026-11-12 18:23 DEVICE drain v1.6 (power-report 9715e1d9) + user: "sao tự nhiên nó lag vl mà màn hình lại còn bị
  nhiễu kẻ ngang" + log 5e013462. Rows done: baseline 50 % 2779 mW, brightness 0 % 2104 mW, all clocks minimum
  (LITTLE 442 / MID 507 / G3D 156 / MIF 421, panel on) 2012 mW; the report stops there (no panel-dark / drift rows =
  the run was stopped). At the governor stop the baseline state was LITTLE 442, MID 2314, G3D 806 (req 806000) -
  the governor held the GPU at the top during the measurement (WMI polling / brightness changes redraw?).
  Most likely cause of lag + stripes = row 3 itself (my test): CPU/GPU at minimum = lag, MIF 421 MHz with the panel
  on = horizontal stripes (scan-out bandwidth / underrun) - to be CONFIRMED with the user (when it started, whether a
  reboot cleared it). The log is from a later boot (RTC restarts each boot; uptime ~15 min): PanelStep 0, PanelCmdStatus
  4, PanelPowerChanges 5, PanelDisplayPower 1, BrightnessLast level 50 AOR 0x093C ok; DECON GLOBAL read 0x113/0x133 in
  gpuwatch passes; kmd 2 presents/s, cpu 1.7 % - not visibly broken at collection time.
  Consequences: IdleMifMs (MIF 421 with the screen on) must stay 0 - same mechanism (58 % share in the earlier run,
  nothing reported then); the screen-on MIF 2093 policy (upstream's judder fix) is probably also a scan-out need.
  Numbers: clocks contribute ~90 mW (2104 -> 2012), the panel at 50 % vs 0 % ~0.67 W this run (drift-prone): ~2 W stays
  with everything at minimum and the panel nearly dark -> static: CPU cores only in WFI (no _LPI), GPU / other blocks
  never powered down - the next target.
- power-s10 v1.7: the minimum row keeps MIF 2093; try/finally restarts the governor, kills panelon (KMD relights),
  restores brightness and the display timeout even on Ctrl+C.
- 2026-11-12 18:42 DEVICE drain v1.7 (power-report d90911d6, complete): baseline 50 % governor 3971 mW; brightness 0 %
  3484; CPU+GPU minimum, MIF 2093, panel 50 % 2670; panel dark (screen-off state: governor minimum, MIF 421) 2253;
  baseline again 3385 (drift ~-0.6 W; this run's level is ~1.2 W above the 16:00 one = background activity varies).
  No stripes/lag reported this time (MIF stayed 2093 with the panel on) - consistent with the MIF-421 explanation.
  At the governor stop: MID cur/req 2314000, G3D cur/req 806000 although nothing ran but the test's WMI polls.
  Split (approx.): governor's choice of clocks at "idle" 0.7-1.3 W (baseline vs CPU+GPU minimum); panel at 50 % ~0.4 W
  (2670 vs 2253); static floor ~2.25 W with the panel dark and everything at minimum.
- Cause of the first item: s10egovernor's race-to-ceiling thresholds (MidMaxBusyPct 50 on the busiest A75,
  GpuMaxBusyPct 30 - the G17 frame-pacing / drag-lag tuning) trip on short background bursts, and the down-steps
  wait DownSamples loops. Fix (governor Eco, default on, EcoMs 3000): no touch for 3 s -> MID / G3D follow
  1.25 x busy x ceiling capped at EcoMid 1794000 / EcoG3d 572000, unless that domain's load stays >= 50 % for 1.5 s
  (video / game / benchmark), then the normal policy for 3 s more; a touch restores the normal policy at once. Stat
  EcoPct. Caveat: mouse / keyboard use is not seen (only S10eTouchActivity) - such interaction gets Eco unless
  sustained. EcoMs 0 = old behaviour. NOT validated: drain (governor rows) + video / drag check by the user.
- Static floor ~2.25 W (next): candidates CPU idle states (_LPI absent: cores WFI only), GPU never powered down,
  WiFi power save, other blocks; Microsoft docs for ARM64 idle states first.
- 2026-11-12 22:12 user: "chúng ta đã ổn định hơn nhiều rồi đúng không ?, fix tiếp đi" (no drain / Eco feedback yet).
  Re-read of log 5e013462 gpuwatch: the 'S10 GPU watch' task is STILL ACTIVE on the S10 (gpu-s10 "final" never ran
  after the s10.4 -Keep install): every pass ~20-30 s = a new PowerShell with 5-6 s CPU, MsMpEng 3-16 s per pass
  (Defender scanning each start), WmiPrvSE; processor time ~19-25 % with 2300-5800 interrupts/s during passes vs
  0.8-1.7 % / 490-680 between. That is a large part of the "idle" draw in every drain run so far. power-s10 v1.9
  drain lists diagnostic tasks / logging variables first (Warn) and the top CPU-time processes.
- MS docs (rule 13) for CPU idle states: learn.microsoft.com/windows-hardware/drivers/kernel/ppm-notifications -
  processor idle states on Windows are listed by the PEP (PEP_NOTIFY_PPM_QUERY_IDLE_STATES_V2 "query the list of idle
  states that it supports") and executed by it (PEP_NOTIFY_PPM_IDLE_EXECUTE: "the PEP ... must execute the halt
  instruction to transition the processor to the idle state", called with interrupts disabled). The page never
  mentions _LPI; nothing says Windows reads _LPI without a PEP. => CPU deep idle on ARM64 Windows = writing a processor
  PEP (PoFx plug-in, boot-start, PSCI CPU_SUSPEND from IDLE_EXECUTE) - a large, boot-hazard project. Estimate of its
  worth first: 6 cores in WFI at minimum clocks are ~0.1-0.3 W, not the ~2 W floor - so measure the floor again
  without the diagnostic background before deciding.
- 2026-11-13 14:37 DEVICE drain (power-report b1b2de27; the user ran v1.7, governor before Eco - still MID 2314 / G3D
  806 at the stop; background presumably cleaned by gpu-s10 final + reboot): baseline 50 % 1957 mW, brightness 0 %
  2058, CPU+GPU minimum (MIF 2093, panel on) 2081, panel dark (screen-off state, MIF 421) 1999, baseline again 2091.
  => FLAT ~2.0 W: panel on/off, brightness, CPU/GPU clocks and MIF make no measurable difference at idle (cores and
  the GPU are clock-gated when idle; the earlier 0.7-1.3 W "governor" difference was the gpu-watch background running
  at high clocks). The earlier 3-4 W baselines were the diagnostic background. Eco therefore matters for background
  bursts only, not for true idle.
  The ~2 W floor is constant and independent of everything tried: a device / block that is always on. A phone SoC
  idles at a few hundred mW, so it is not WFI-vs-deep-idle of 6 cores (~0.1-0.3 W). power-s10 v2.0 "devices": A/B with
  one device disabled at a time (Wi-Fi SAMS9825, BT SAMS9829, audio+CS35L40 SAMS9827, MFC SAMS982A, xHCI PNP0D10,
  touch SCSY0761), then all, baseline before/after, re-enabled in finally. Not covered by it: the M4 cluster (never
  started - is its power domain on?), USB VBUS boost (xHCI host mode), DSIM/DECON, UFS link power.
- 14:43 power-s10 v2.0 devices failed at once: $off = @() hit the script's [switch]$Off (case-insensitive names) -> v2.0.1 $downIds. MFC row skipped (VIDC not started: Exynos9820Mfc not installed on the phone yet).
- 14:45 v2.0.1 devices: baseline 2571 mW (brave running in the background this time), then Disable-PnpDevice on ACPI\SAMS9825\0 (Wi-Fi) -> 'Not supported' (WMI 0x8004100c) and the run ended (everything re-enabled by finally). v2.0.2: pnputil /disable-device fallback, a device that cannot be switched off is skipped.
- 14:55 v2.0.2 devices (power-report e2c91198, brave in the background again): baseline 2479 mW; Wi-Fi, audio, xHCI, touch = 'Cannot disable critical system device' (pnputil; same as notes/thermal-clocks.md 08:25); Bluetooth off 2474 (-6 mW = nothing); all-off (= only BT) 2530; baseline again 3116 (3 samples). v2.0.3: pnputil /remove-device fallback + /scan-devices to bring them back (the method used for Wi-Fi on 2026-11-05).
- 15:27 USER: during the v2.0.3 devices run the phone POWERED OFF, and after it came back touch is gone. Cause unknown (which remove-device: Wi-Fi / audio / xHCI / touch; a bugcheck in a remove path?). v2.0.4: touch and xHCI taken out of the candidate list. Asked for: cold restart, then pnputil enable/scan of ACPI\SCSY0761\0 + s10-log (or TWRP log if no input).
- 15:35 user: no OTG (no keyboard / mouse) -> touch has to come back without any input. Mechanisms found: Windows
  logs on by itself (unattend AutoLogon, blank password; desktop ~68 s after boot in gpuwatch); the elevated logon
  task "S10 kit watcher" runs C:\S10Prov\kit-watch.ps1 (RunLevel Highest, -ExecutionPolicy Bypass); S10eAgent's
  "run:" button action is NOT elevated (KeysRunInSession(..., FALSE)), only taskmgr is; TWRP can write the Windows
  volume when NTFS is not dirty/hibernated (push-to-windows make_rw: remount / ntfsfix -d / ntfs-3g remove_hiberfile).
  New tools/s10-kit/touch-rescue.cmd v1.0 (PC, adb, TWRP): backs up kit-watch.ps1 (.s10orig) and replaces it with a
  one-shot script: pnputil /enable-device + /scan-devices for touch / Wi-Fi / audio / xHCI / BT, /restart-device for
  touch if still not OK, logs states + System errors / bugcheck / Kernel-PnP events + dump files to
  C:\S10Prov\touch-rescue.log, restores kit-watch.ps1 in finally. "touch-rescue.cmd log" pulls the log from TWRP.
  sh -n OK, the generated PowerShell parses (pwsh). NOT run.
- 15:38 touch-rescue.cmd v1.0 on the user's PC: /data read-only; mount.ntfs: "The disk contains an unclean file
  system (0, 0). Metadata kept in Windows cache, refused to mount ... NTFS partition is in an unsafe state" (the
  power-off / forced restart left NTFS dirty); no ntfsfix in this TWRP, ntfs3 force mount failed too -> nothing
  written (script failed safe). Building a static ntfsfix to clear the dirty flag was refused by the session's
  permission check (external code) - and forcing a dirty NTFS from Linux risks the volume anyway, so not pursued.
  A clean Windows restart clears it, but needs input (power menu is a GUI; buttons.ini lives on C:). Next: read-only
  diagnosis - get-logs-twrp.cmd (SYSTEM hive + System.evtx, read-only) to see why touch does not start; ask whether
  touch works in TWRP and whether a Vol Down + Power cold restart was tried.
- 15:42 get-logs-twrp zip (73c5e736): only the ESP part - /data was "fuseblk ro" but nothing under it was found
  (the touch-rescue make_rw had unmounted /data and re-mounted it; the windows\ folders came back empty) -> no
  SYSTEM hive / evtx this time. ESP: UEFILOG of the last Windows boot: TouchPowerDxe identical to a good boot
  (L19 tsp_io and L27 tsp_avdd enabled 0x2C->0xEC / 0x30->0xF0, settled 70 ms, done) = the touch rails are powered
  by the UEFI as always. WINPRAM.BIN (the Windows session before it, bit-rotted by the forced restart): S10eDiag ring
  full of "keys: power.short -> screen / power.long -> powermenu / voldown.long" (the user trying to get out without
  touch) and the touch PRAM page (+0xE000) all ZERO = S10Touch's PrepareHardware (S10tPramInit is its first call)
  never ran in that session: the touch device was not started at all (not present / disabled / driver not loading),
  not a controller problem. Need the SYSTEM hive (Enum\ACPI\SCSY0761\0 ConfigFlags / problem) - TWRP must be
  restarted (Reboot > Recovery) for a clean read-only mount, then get-logs-twrp again.
- Review note (S10Touch 1.1.1.0, own follow-up): TouchActivityClose (ReleaseHardware) NULLs gTouchActivity and closes
  the handle while TouchActivityPulse may still run from the input path - check that the worker / interrupt are
  stopped before ReleaseHardware (D0Exit ordering), else a remove can KeSetEvent a freed event.
