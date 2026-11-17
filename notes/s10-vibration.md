# S10 vibration motor (2026-11-15) - S10eBattery 1.0.0.8 Vibra.c

User 2026-11-15 20:13 (after the dual-boot reinstall came up): "phát triển nhiều chức năng hơn nữa, camera, rung,
sim, vân tay, gps". Vibration first (feasibility below).

## Hardware (all from the phone's live DT dump/s10-dump/dt/live.dts + LineageOS android_kernel_samsung_exynos9820
## lineage-23.0, sparse clone in /home/claude/ref/k9820-full)
- Motor: LRA ("LINEAR_1040") driven by the MAX77705 IF-PMIC haptic block. max77705@66 on hsi2c@15D60000 (=HSI2C45,
  already owned by S10eBattery) - live.dts:9677/9696. pinctrl-0 of that node = max77705-irq, vib-pwm (gpp3-1
  function 2, pud 1), motor-boost-en (gpg1-7 output 1) - live.dts:2073-2090, 9699.
- haptic node (live.dts:14612): pwm_id 1, multi_frequency 7, frequency[0] 0x63B (159.5 Hz), period[0] 0xBEBC ns
  (48828 = 20.48 kHz = 128 x 160 Hz), duty[0] 0x8F0D ns (75 %), normal_ratio 75, overdrive 95, max_timeout 10000 ms.
  regulator_name VDD_MOTOR_3P3 exists only as a string (max77705_haptic.c never enables a regulator).
- Android log (dump/s10-dump/sys/dmesg-full.txt:657-678): "max77705_vib: ratio set to 75", "[haptic engine] motor
  run", "freq:2050, intensity:10000, time:20" - the HAL's packet mode at 205 Hz.
- [K] drivers/motor/max77705_haptic.c: init MAINCTRL1 0x02 |= BIASEN b7; MCONFIG 0x10: EN b6 off, LRA b7 on,
  DIVIDER_128 b1 on; run: set PWM duty then MCONFIG |= LRA|EN; stop: MCONFIG &= ~(LRA|EN), duty = period/2 +
  pwm_disable. PMIC client = 0xCC >> 1 = 0x66 (drivers/mfd/max77705.c:57). Nothing else in the kernel writes 0x02/0x10.
- [K] sec_haptic.c: duty = period/2 + (max_duty - period/2) * intensity / 10000 (period/2 = no drive);
  MULTI_FREQ_DIVIDER 78125000 (period_ns = 78125000 / freq_0.1Hz).
- PWM pwm@10510000 "samsung,s3c6400-pwm" -> s3c64xx variant: 16-bit counters. [K] pwm-samsung.c: TCNTB(n)
  0x0C+12n, TCMPB(n) 0x10+12n, TCNTO(n) 0x14+12n (samsung_pwm_timer.c:354), TCON channel 1 at bits 8..11 (start,
  manual update, invert, autoreload), NORMAL polarity = invert SET, tcmp = tcnt - duty_ticks, both -1.
- Pins: [K] pinctrl-exynos-arm64.c exynos9820_pin_banks5 = PERIC0 @0x10430000: gpp3 at 0x060, gpg1 at 0x0A0;
  exynos9810_bank_type_off: CON +0 (4 bits/pin), DAT +4, PUD +8 (4 bits/pin).
- Clock: [K] cal-if/exynos9820/cmucal-sfr.c: CMU_PERIC0 0x10400000; PWM PCLK gate 0x2020 (CG_VAL b21, MANUAL b20,
  ENABLE_AUTOMATIC_CLKGATING b28); PWM QCH 0x3014. The PWM's input rate is not in our sources -> measured.

## Driver (drivers/S10eBattery/Vibra.c, IOCTL_S10E_VIBRATE 0x8A3 on the battery device)
- In S10eBattery because the PMIC is on the battery's polled bus (BusLock); VibLock -> BusLock order.
- PWM / PERIC0 GPIO / CMU mapped by physical address (no DSDT change).
- Order: map -> CMU gate READ (refuse unless auto-gating or manual+on: a gated SFR access can hang the bus) ->
  calibrate the PWM input clock (channel 1 one-shot from 0xFFFF, 100 us at HIGH_LEVEL vs KeQueryPerformanceCounter,
  median of 3, accepted 1..250 MHz; the motor cannot move: MCONFIG EN is off) -> pins -> PWM run -> PMIC init once +
  LRA|EN -> one-shot timer (deadline guard) -> work item stops (MCONFIG off, autoreload off). D0Exit stops the motor.
- Syntax-checked with clang against the real WDK 10.0.26100 + KMDF 1.33 headers (scratchpad kcheck.sh); struct
  layout C_ASSERTed (80 bytes). NOT built with EWDK, NOT run.
- Test: tools/s10-kit/vibra-s10.ps1 (install = pnputil 1.0.0.8; test = 3 pulses + 1 s at 50 %) -> vibra-s10.txt.
- Not yet: a Windows-facing interface (HID haptics / hardware notifications) - only the IOCTL for now.

## Feasibility of the rest of the request (research 2026-11-15, nothing built)
- GPS: the S10's GNSS is a Broadcom BCM4773 that is ALSO the sensor hub (spi@10970000, compatible "ssp,BCM4773",
  live.dts:10232; drivers/sensorhub/brcm). Position fixes come from Broadcom's closed host engine (glgps on Android);
  the hub only passes raw data. No Windows engine exists -> GPS: not feasible without reimplementing a GNSS engine.
  The same hub carries accelerometer / gyro / light / proximity over Samsung's SSP protocol (GPL kernel code
  available) -> auto-rotate / auto-brightness are a realistic later target instead.
- Fingerprint: Qualcomm QBT2000 ultrasonic (qbtspi@0 live.dts:9803, qbt2000 15468); capture + matching run in the
  Trustonic TEE with Samsung's trusted app -> not feasible from Windows.
- SIM / cellular: integrated Shannon modem (modem_if live.dts:3677, mcu_ipc_cp): CP firmware boot through the secure
  monitor, Samsung's proprietary IPC, then a Windows WWAN/MBIM driver on top -> research project of months, low odds.
- Camera: Exynos ISP (Pablo/FIMC-IS) needs Samsung's closed ISP library + firmware; a raw-sensor path (CSIS + sensor
  I2C init from the GPL sensor drivers + software debayer + a Windows AVStream camera driver) is months of work.

## 2026-11-15 20:32 camera precedent (user: github.com/danstronics/ios-s9plus-exynos)
- That repo (S9+ star2lte, iOS on HakiOS/Scion) does NOT have a camera: its README table "Camera/audio/cellular/App
  Store | Not qualified by this release"; core/docs/devices/star2lte.md: camera "no ... nothing tried on an S9+".
- Its core/ snapshot (Scion's HakiOS fork) has the precedent: core/docs/devices/starlte.md - Galaxy S9 (Exynos 9810)
  FRONT camera S5K3H1 "partly": raw sensor path WITHOUT Samsung's ISP firmware/library, auto-exposed, demosaiced
  1632 x 1224, 30 fps into the stock Camera app preview, "seen on the phone, 3 November" (cam_s9=7 cam=5).
  Driver core/drivers/camera/hakios.exynos_camera.c (staged cam_s9=1..7: S2MPB02 LDOs/BUCK2, CIS_CLK MCLK, sensor
  I2C, ISPPRE power domain + DTZPC SMC 0x82000410 restore, CSIS v5.1 link + VC0 WDMA, D-PHY settle, setfile mode
  table, AE loop). Their own AVF camera (core/avf/CAMERA.md) is different: an Android app's Camera2 frames passed
  into an iOS VM - not hardware.
- LICENSE: their kernel side (drivers/) is APSL-2.0 (XNU derivative) -> reference reading only, not copied into our
  drivers; the facts come from Samsung's GPL kernel (LineageOS exynos9820) as theirs did from the 9810 one.
- S10 (live DT): sensors 2L4 (rear wide), 3M3 (tele), 3P9 (ultra-wide), 3J1 (front, fimc-is-3j1@10, sensor_i2c_ch 4,
  mclk_ch 1, gpio_reset, af on i2c_ch 5; sensor-module-3j1 live.dts:15614). Camera blocks at 0x1703xxxx (9820 IS),
  i.e. different addresses from the 9810's ISPPRE 0x162xxxxx - the 9810 code is a map of the steps, not of the values.
- Revised estimate: a raw front-camera frame on the S10 is a staged, realistic project (precedent exists); getting it
  into Windows' Camera app additionally needs an AVStream / camera driver + software ISP (demosaic, AE, AWB) on our
  side - the larger part.

## 2026-11-16 02:37 first run (1.0.0.8): no vibration, refused at step 2
- CMU PWM gate 0x00200000 (CG_VAL 1, MANUAL 0, AUTO 0), QCH 0x003F0002 (ENABLE 0, CLOCK_REQ 1). 1.0.0.8 required
  AUTO or MANUAL+CG_VAL and refused. [K] cal-if ra.c ra_get_gate: MANUAL 1 -> CG_VAL decides, MANUAL 0 -> pass =
  !ENABLE_AUTOMATIC_CLKGATING -> this gate PASSES (clock on). Nothing was written (stopped before calibration).
- 1.0.0.9: VibClockOn = !(MANUAL && !CG_VAL). Same fix in S10eCam's ClkOn (not run yet).
- Also: cam-s10.ps1 refused by the execution policy ("not digitally signed" = Mark-of-the-Web on a downloaded file):
  Unblock-File / Set-ExecutionPolicy -Scope Process Bypass, as README section 2 says.

## 2026-11-16 03:04 VIBRATION WORKS (S10eBattery 1.0.0.9, user: "rung ok")
- Steps 1-7 OK on every run. PWM input clock measured 26 268 862 Hz (2638 counts / 2611 QPC ticks at 26 MHz;
  = OSCCLK 26 MHz within the 100 us window's error). TCFG0 was 0x00000C19 (prescaler0 0x19, prescaler1 0x0C) with
  TCON 0x00500408 (timer 4 running with autoreload - someone's tick, untouched; timer 0 idle -> prescaler0 set 0).
  TCNT 1282 / TCMP 320 at 100 % (= 75 % duty, 48828 ns), TCMP 480 at 50 %.
- Pins after: GPP3 CON 0x00003320 (gpp3-1 = 2 TOUT1), PUD 0x10; GPG1 CON 0x10000001 (gpg1-7 output), DAT 0xD7 (bit 7).
- PMIC: MAINCTRL1 0x81 (BIASEN already set by the bootloader), MCONFIG 0x82 -> 0xC2 (LRA | DIV128 was there, EN set).
- Next for vibration: a Windows-facing interface (none yet; IOCTL only).

## 2026-11-17 user: "it vibrates with the test script, never in normal use" - why (MS docs)
- Expected, not a bug: the motor is reachable only through IOCTL_S10E_VIBRATE; Windows has no device that tells it
  a vibration motor exists.
- MS Learn "Input Device Haptics Implementation Guide" (windows-hardware/design/component-guidelines/
  input-haptics-implementation-guide): Windows 11 drives haptics for two input classes only, Haptic Touchpad (a
  SimpleHapticsController collection, Haptics page 0x0E, as a CHILD of the Precision Touchpad collection) and Haptic
  Mouse (top-level, sibling of the mouse collection); pens have their own guide. Host-initiated triggers only
  (Manual Trigger 0x21 + Intensity 0x23; Auto Trigger 0x20/0x22 "not supported by the host"); mandatory waveforms
  None/Stop/Hover/Collide/Align/Step/Grow. Nothing about touch screens, the touch keyboard, notifications or a phone
  vibration motor.
- WinRT VibrationDevice (Windows.Devices.Haptics, "a device that supports vibration notifications only"): the page
  does not say which driver interface it enumerates - UNVERIFIED; it is an API apps call, Windows itself does not.
- So a HID haptics interface would not make the shell vibrate. Vibration in daily use has to come from our side:
  e.g. a small service (notifications via the documented UserNotificationListener API, charger plug, low battery)
  calling the IOCTL.
- DECISION (user 2026-11-17 15:29): dropped - Windows never asks for vibration, a service for it is not wanted.
  Vibration stays IOCTL-only (vibra-s10.ps1 test).
