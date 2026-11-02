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
