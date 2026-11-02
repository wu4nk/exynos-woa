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
