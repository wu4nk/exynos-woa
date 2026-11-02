# S10Touch - new touchscreen driver (user 2026-11-02 17:25: "still laggy, delete it, write a new one")

## Progress log
- Read SecTsY761Touch 0.3.1 (the running one) for the hardware facts only: ALIVE EINT8 register offsets
  (Device.h B1_*), controller start order (TouchStartSensing = sec_ts_probe), event format/commands, SPB
  sequence pattern, HID descriptor layout (10 fingers, report 0x54, 64 B), resource parsing.
- Design reasons (lag): the old path ran the I2C read in a system work item (normal priority) while the
  desktop is drawn by the CPU (Basic Display + WARP at 1440x3040) -> input processing competes with rendering;
  it also allocated a WDFREQUEST per transfer and coalesced via lock + timer. New: ISR -> DPC -> dedicated
  worker thread at LOW_REALTIME_PRIORITY + 8, reused SPB request, worker-owned FIFO (no locks), stationary
  repeat every 8 ms while a finger is down (stops 3 s after the last controller event), move-only frames
  coalesce at the FIFO tail, press/release frames never. Back-off (5..50 ms) after >3 empty/error attentions in
  a row so a stuck-low line cannot spin a real-time thread.
- Descriptor checked by a mini HID parser: 654 B, input 63 B + report id = 64 B (S10T_REPORT C_ASSERT),
  feature 1 B + id. X/Y logical 0..4095 (live DT sec,max_coords 0x1000), physical 2.61 x 5.51 in.
- Syntax: Driver.c + Hid.c compile clean with clang -fsyntax-only against stub WDF/WDM headers
  (scratchpad/stubwdf). Real EWDK build + device run pending. INF binds ACPI\SCSY0761 (installer removes
  SecTsY761Touch / S6SY761TouchBeyond0 packages first).
- Review: the first worker draft blocked on KickEvent after 3 s idle (would have ignored attentions) - replaced by
  the repeatDue condition; ReleaseHardware now clears the interrupt handle (framework deletes interrupts created
  in PrepareHardware); ntstrsafe.h before reshub.h (RESOURCE_HUB_CREATE_PATH_FROM_ID uses RtlUnicodeStringPrintf).
- EWDK build 1 (user 17:40): C4013 RESOURCE_HUB_CREATE_PATH_FROM_ID undefined - the SDK reshub.h defines it only under RESHUB_USE_HELPER_ROUTINES (the old SecTsY761Touch defined it before the include). Fixed in S10Touch.h.

## 1.0.1.0 (2026-11-02) - "loạn cảm ứng" after 1.0.0.0
User report: touch worse than SecTsY761Touch, chaotic. Causes found in 1.0.0.0:
- one HID frame per I2C batch: release + press of the same tid inside one batch overwrote the lift
  (Windows saw a finger teleport / a second finger -> random zoom/scroll);
- no all-up on I2C error, stack overflow or failed READ_ALL (old driver had TouchQueueAllUpReport) -> stuck
  phantom contact;
- HOVER / PROXIMITY / FLIP COVER / STYLUS coordinate events were reported as fingers (sec_ts only reports
  NORMAL, GLOVE, PALM, WET; ttype = ev[6]>>6 <<2 | ev[7]>>6);
- SPB timeout 100 ms (old: 250 ms); repeat frames every 8 ms interleaved with the chip's own moves.
Fix: per-event press/release frames in event order (pending moves flushed first, one move frame at batch end),
AllUp on every read failure/overflow, ttype filter, timeout 250 ms, repeat only after 2 x RepeatMs quiet
(default RepeatMs 10).

## 1.0.2.0 (2026-11-02) - the panel is a y771
LineageOS android_kernel_samsung_exynos9820: beyond1lte_defconfig (and beyond0lte) set CONFIG_TOUCHSCREEN_SEC_TS_Y771=y
(drivers/input/touchscreen/sec_ts/y771). Differences that matter (sec_ts.h / sec_ts.c):
- struct sec_ts_event_coordinate byte 7 = left_event:5, max_energy_flag:1, ttype_1_0:2; sec_ts_read_event uses
  `& 0x1F`, MAX_EVENT_COUNT 31 -> 1.0.0/1.0.1 used 0x3F: max_energy_flag added 32 to the count -> READ_ALL of 32
  extra stale events -> ghost touches. Fixed (mask 0x1F, max 31).
- status event (eid 1): stype (b0>>2)&0xF; INFO(2) id 0x00 data1 0x20 = watchdog reboot -> release all, reinit,
  sense on; ERR(1) id 0x01 = queue full -> release all. Both handled now.
- SEC_TS_CMD_SW_RESET is 0x12 on y771; touch function 0x30 written as 2 bytes LE (already so).
- Init (probe): power, 70 ms, wait_for_ready (READ_ONE_EVENT until INFO/VENDOR_INFO ack 0x00, 20 ms x
  SEC_TS_WAIT_RETRY_CNT), device id 0x22 (5 B), integrity 0x21 (0x80 ok), boot status 0x55, ts status 0xAF (4 B),
  touch function |= 0x61, sense on.

## 1.0.3.0 (2026-11-02) - no touch at all after installing 1.0.2.0; PRAM telemetry
Logs (get-logs-twrp 20261102-224113): setupapi: S10Touch 1.0.2.0 imported + device started OK (signer score
Authenticode, cert not in root but test signing). System.evtx ends 21 s after the install (record 1256), no later
boot recorded -> the phone was force-reset right after; registry counters (written in D0Exit) never landed; the
SYSTEM hive did not copy. UEFILOG v0.1.8: USI03 clock found at PERIC0_IP / 1 = ~400 MHz (USER mux 0x10, div 0) ->
every HSI2C here and in Windows ran at ~800 kHz SCL (timing for 200 MHz); v0.1.8 set div 1 -> 1-byte read 122 us
-> 225 us (= 400 kHz). Bus speed is NOT what made the UEFI test lag. Chip: device id 53 45 37 71 00 (y771),
integrity 0x80, fw 17.55.02.01, img 17.55.02.34, touch function already 0x0061, sense on OK.
1.0.3.0: the driver writes its state into WINPRAM +0xE000 (free slice per the 2026-10-13 inventory), 64 x 64 B
lines: D0Entry result, a 1 s heartbeat with all counters + EINT mask/pend + attention level + hidclass read
requests, and the last 60 raw controller events. The worker now always waits with a 1 s timeout (heartbeat).

## 1.0.4.0 (2026-11-03) - 1.0.3 PRAM: header only
Logs 20261103-000930 (fresh image from make-windows v2.3): UEFILOG = **v0.1.7** (the flash kit picked it, not
v0.1.8): USI03 still at PERIC0_IP/1 (~800 kHz SCL); TouchChip: boot status 0x20, id 53 45 37 71, ts status
00 02 01 00, **sense on: Device Error** (write failed at 800 kHz) -> "controller NOT started". WINPRAM-RESTART.BIN
+0xE000: only line 0 "S10TOUCH 1.0.3.0" at uptime 6484 ms - no D0Entry line (written after ControllerStart), no
heartbeat for the whole session -> stuck in SpbOpen / ControllerStart inside D0Entry, or PrepareHardware failed
after PramInit. 1.0.4: ControllerStart runs on the worker (stop-aware waits), D0Entry returns at once, PRAM marks
for PrepareHardware (resource mask), D0Entry, SpbOpen status, interrupts on, boot-status attempts, start result.
Also noted: Exynos9820Hsi2c idles to D3 after 1 s (SystemManagedIdleTimeoutWithHint 1000 ms): first transfer
after idle waits for its D0 - a latency candidate for later.

## 1.0.5.0 + Exynos9820Hsi2c 1.0.1.0 (2026-11-03) - "SpbOpen c0000184"
Logs 20261103-005515 (UEFI v0.1.8 now: clock fixed, TouchChip sense on Success, chip sensing, stack empty).
WINPRAM-RESTART +0xE000: "PrepareHardware res 7 n 3", "D0Entry", "SpbOpen c0000184" (STATUS_INVALID_DEVICE_STATE),
all at 6468 ms, nothing after -> WdfIoTargetOpen on the resource-hub path refused because the HSI2C controller
was not in a usable state at that instant (not started yet, or idle in D3: Exynos9820Hsi2c assigns S0 idle
1000 ms), and the failed D0Entry kills the touch device for the session (1.0.0 had the same single attempt; it
worked when the controller happened to be up).
Fix: S10Touch opens the target from the worker with retries (100 ms x 100, then 1 s, PRAM marks, registry
S10tSpbOpenStatus/Tries); Exynos9820Hsi2c idleSettings.Enabled = WdfFalse (stays in D0 - also removes a D0
transition from the first transfer of every touch).

## 1.0.6.0 (2026-11-03) - WDF_VIOLATION from 1.0.5
Logs 20261103-015018: WINPRAM.BIN (crash dump path): =UFSBUGCHECK BC=0x10D B1=0x5 (WDF_INVALID_HANDLE) B2=0 (handle
NULL) B3=0x1008. Touch PRAM: PrepareHardware res 7, D0Entry, "SpbOpen try 0 st c0000184", interrupts on,
"SpbOpen try 1 st 0", "worker: controller start", then the crash. Cause: SpbOpen created the reused request
(WdfRequestCreate with IoTarget = the not-yet-opened target) right after WdfIoTargetCreate; that create returned
STATUS_INVALID_DEVICE_STATE (= the c0000184 of 1.0.4 too - it was never the HSI2C), the retry skipped creation
(Spb != NULL), opened the target, and ControllerStart's first SpbRead called WdfRequestReuse(NULL).
Fix: request created after a successful WdfIoTargetOpen (target closed again if that fails); SpbRead/SpbWrite
return STATUS_DEVICE_NOT_READY unless open. Exynos9820Hsi2c 1.0.1.0 (no D3 idle) kept: harmless, removes a D0
transition from the first transfer after a pause.

## 1.0.7.0 (2026-11-03) - works, but taps doubled, drags broken, 3-finger drag = a tap per step
User after 1.0.6: touch works; single tap often seen twice; drag: double tap at start/end, or cut then a late tap at
the end; 3 fingers moving -> a stream of taps along the path. Reading: contacts are lifted and re-pressed mid-gesture.
Two paths did that: (1) any SPB error -> AllUp (1.0.1+; Linux drops the batch only); (2) READ_ALL (left x 8 bytes)
is used by every multi-finger batch, rarely with one finger -> its failure = AllUp every batch with 3 fingers.
Exynos9820Hsi2c reads > EXYNOS_HSI2C_FIFO_TRIGGER (8) go through the RX_ALMOST_FULL/TRAILING refill path (GIC SPI
declared Edge in the DSDT) - not proven on hardware; the 8-byte read is.
Fix: lift only after S10T_ERROR_LIFT_STREAK (5) failed reads in a row; the rest of a batch is read with
READ_ONE_EVENT x left (8 bytes each); PRAM F+/F- marks, lift reasons, SpbMaxUs / SpbSlow(>20 ms) in heartbeat 2,
1 move event in 16 logged.

## 1.0.8.0 + Exynos9820Hsi2c 1.0.2.0 (2026-11-03) - SPB reads time out
Logs 20261103-030905. WINPRAM-RESTART (clean session, 1.0.7): heartbeat irq249 ev424 fr661 dl661 rd663 i2c50 inv9,
ff0 rp416 re1 el0, MaxIrqToReport 281249 us, SpbMax 272524 us, SpbSlow(>20 ms) 53; roll: repeated
"READ_ONE failed c00000b5" (STATUS_IO_TIMEOUT = our 250 ms send timeout), then "overflow: stack cleared". Events
decode fine (tid 1 press 0x44 / move 0x84 / release 0xC4, ttype 0). WINPRAM.BIN of the force-reset session is
bit-rotted (RAM not refreshed across that reset) - the RESTART dump is the usable one.
=> ~1 in 10 short HSI2C transfers never completes: the controller finishes but the completion interrupt is lost
(DSDT declares SPI 420 Edge; Linux DT gives no type = level). Each loss stalls the worker 250 ms -> backlog ->
overflow -> all up -> doubled taps / broken drags / "delay".
UEFI test: 34 events in 10 s (3/s), backlog 13 - the UEFI reader is throttled by GOP Blt per event (slow uncached
framebuffer: also why boot drawing is slow); not a touch fault.
Fix: Exynos9820Hsi2c PollTimer (high-resolution, 2 ms) while a request is in flight: under the interrupt lock,
enabled status set -> disable + ack + run OnInterruptDpc (rescue count TelemetryRescueCount); stopped with the
request timer. S10Touch SPB timeout 250 -> 50 ms. Candidate for later: DSDT I2C0 interrupt Level (UEFI).
