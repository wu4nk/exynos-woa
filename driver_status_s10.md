# Galaxy S10 (beyond1lte, SM-G973F) Windows driver status

As of 2026-11-17, branch `beyond1lte-port` (fork wu4nk/exynos-woa). The S10e's own table is driver_status.md.
Dual boot (David42069's Samsung Dualboot): Android on slot A, Windows on slot B. UEFI v0.2.2.

🔴 nothing · 🟠 built / detected, not usable or not run · 🟡 works, not perfect · 🟢 working (confirmed by the user on the S10) · ❔ not tried on the S10

Every 🟢 / 🟡 line has a device result in the notes; anything only built is 🟠.

| Device | Status | In short | Source |
|---|---|---|---|
| Booting Windows | 🟡 | boots in dual boot; after a shutdown / crash the next boot goes to TWRP once (fix stay-s10.ps1 written, not run) | beyond1-port.md 2026-11-15, 11-16 |
| Dual boot with Android | 🟢 | Android (slot A) and Windows (slot B) both boot (UFS fence for the dual layout, board 3) | user 2026-11-17 |
| CPU | 🟡 | 6 of 8 cores (the 2 M4 cores fail to start, as on the S10e); clock raise (CpuOc) confirmed | beyond1-port.md Round 4, 2026-11-15 |
| Storage (UFS) | 🟢 | the user's own build of the driver | 2026-11-11 19:35 |
| Display + brightness | 🟢 | own display driver; brightness slider works (S6E3HA9 panel curve) | 2026-11-12 15:47 |
| GPU | 🟡 | Mali G76 renders the desktop and apps; "tạm ổn". MSAA fix (UMD 0222-s10.6) not confirmed on the phone | 2026-11-11, 11-15 |
| Touch | 🟢 | smooth (S10Touch 1.1.0) | Round 8 |
| On-screen keyboard | 🟡 | hangs after a short use (user 2026-11-11); S10e workaround UseSWRender = 1 not tried on the S10 | touch-keyboard.md |
| Screenshots | 🟢 | Win+Shift+S and Win+PrtScr work (did not on 2026-11-11) | user 2026-11-17 |
| Buttons | 🟢 | hardware keys work; Reboot to TWRP | user 2026-11-17 |
| Battery / charging | 🟢 | plug seen in 2-3 s, wall charger 1800 / 2100 mA, % rises while plugged (S10eBattery 1.0.0.10) | battery-driver.md 2026-11-17 |
| Battery life | 🟡 | ~2 W idle floor (no SoC idle states yet) | 2026-11-15 |
| Vibration | 🟢 | MAX77705 haptic (S10eBattery 1.0.0.9) | s10-vibration.md 2026-11-16 |
| Speakers | 🟡 | both, stereo; a bit quiet (+3 dB in SetupComplete v1.3, not confirmed) | 2026-11-11 19:35, 11-15 |
| Microphones | 🟢 | built-in mics after CodecEnable 1 (mic-s10.ps1) | 2026-11-16 03:14 |
| Headphone jack | ❔ | works on the S10e; not tried on the S10 | - |
| WiFi | 🟡 | connects (2026-11-03); speed not measured on the S10 | Round 5 |
| Bluetooth | 🟡 | works (Exynos9820Bt, kit v2.3); same chip as the S10e, so the 2.4 GHz WiFi coexistence problem is expected (not measured) | 2026-11-11 19:35 |
| Front camera | 🟡 | Camera app, browsers, Settings: 684 x 912 at 30 fps, NV12, CPU path (S10eCam 0.8 / S10eCamera 0.8). 60 fps + mirror (0.10 / 0.9) built, not run; video recording lags | s10-camera.md |
| Rear cameras | 🔴 | nothing; ISP (Samsung library) reverse engineering at Stage 1 | s10-isp.md |
| Hardware video decode (MFC) | 🟠 | Exynos9820Mfc built in kit v2.6 (needs mfc_fw.bin), not confirmed on the S10 | 2026-11-12 |
| USB host / docks | ❔ | works on the S10e (plugged before boot); not tried on the S10 | - |
| Screen rotation | ❔ | works on the S10e (button / tool); not tried on the S10 | - |
| External display | 🔴 | researched only (S10e) | - |
| Cellular / SIM, fingerprint, GPS, sensors, NFC, wireless charging | 🔴 | not started | - |

## Open items in order
1. Camera: run S10eCam 0.10 + S10eCamera 0.9 (60 fps, mirror) and measure the recording lag.
2. Boot: run stay-s10.ps1 on; next UEFI build with BEYOND0LTE_WINDOWS_WATCHDOG=0 for Beyond1.
3. On-screen keyboard hang (composition frame completion on the Mali path - S10e touch-keyboard.md).
4. ISP Stage 2 (emulator) - weeks.
