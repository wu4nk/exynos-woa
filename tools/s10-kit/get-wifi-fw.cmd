@echo off
rem ---------------------------------------------------------------------------
rem get-wifi-fw.cmd (v1.2) - copy the S10's own BCM4375 WiFi firmware off the phone into
rem bcm4375\ next to this file. make-windows.ps1 puts it in Windows\System32\drivers\bcm4375\,
rem where Exynos9820WlanCx loads it (bcmdhd_sta.bin_b1, bcmdhd_clm.blob, nvram.txt_*_b1; the
rem nvram is picked by the module's OTP vendor id). The blobs are Broadcom/Samsung's and are
rem never shipped in the kit: they come from THIS phone's vendor partition.
rem
rem v1.2: also the MFC video codec firmware (/vendor/firmware/mfc_fw.bin) -> mfc\mfc_fw.bin next to this file;
rem build-s10.cmd v1.14 builds the Exynos9820Mfc (hardware video decode) package only when it is there.
rem
rem Phone in TWRP, or in Android with root (KernelSU), USB cable in. Read-only on the phone.
rem ---------------------------------------------------------------------------
setlocal EnableExtensions
cd /d "%~dp0"
set "D=%~dp0bcm4375"
adb get-state >nul 2>&1 || (echo [FAIL] adb sees no device & exit /b 1)
set "BL="
for /f %%B in ('adb shell "getprop ro.boot.bootloader"') do set "BL=%%B"
echo %BL%| findstr /b /c:"G973" >nul || (echo [FAIL] bootloader "%BL%" is not a Galaxy S10 ^(G973*^) & exit /b 1)
echo [ OK ] Galaxy S10, bootloader %BL%

adb push get-wifi-fw.sh /data/local/tmp/get-wifi-fw.sh >nul 2>&1 || adb push get-wifi-fw.sh /tmp/get-wifi-fw.sh >nul 2>&1
set "RUN=sh /data/local/tmp/get-wifi-fw.sh"
adb shell "[ -f /data/local/tmp/get-wifi-fw.sh ]" || set "RUN=sh /tmp/get-wifi-fw.sh"
set "ROOT="
for /f %%U in ('adb shell "id -u"') do set "ROOT=%%U"
if not "%ROOT%"=="0" set "RUN=su -c '%RUN%'"
set "SRC="
for /f "usebackq" %%P in (`adb shell "%RUN%"`) do set "SRC=%%P"
if not defined SRC (echo [FAIL] the phone-side script did not run ^(root?^) & exit /b 1)
if exist "%D%" rmdir /s /q "%D%"
mkdir "%D%"
adb pull "%SRC%/." "%D%" >nul || (echo [FAIL] adb pull %SRC% & exit /b 1)

set MISS=0
for %%F in (bcmdhd_sta.bin_b1 bcmdhd_clm.blob) do if not exist "%D%\%%F" (echo [FAIL] %%F not found on the phone & set MISS=1)
if not exist "%D%\nvram.txt_1rh_es43_b1" if not exist "%D%\nvram.txt_CS01_semco_b1" (echo [FAIL] neither nvram.txt_1rh_es43_b1 nor nvram.txt_CS01_semco_b1 & set MISS=1)
if "%MISS%"=="1" (echo        found:& dir /b "%D%" & exit /b 1)
echo [ OK ] WiFi firmware in %D%
rem v1.1: Bluetooth patch file for Exynos9820Bt. Its INF installs it under the name bcm4375B1_semco.hcd (the S10e's
rem file); the one copied there matches THIS phone's module: cid.info (written by Android's WiFi driver) says murata
rem or semco. No cid.info: semco (the S10e default) with a warning - Bluetooth may then not start (UNVERIFIED on S10).
set "MOD="
if exist "%D%\cid.info" (findstr /i "murata" "%D%\cid.info" >nul && set "MOD=murata")
if exist "%D%\cid.info" if not defined MOD (findstr /i "semco" "%D%\cid.info" >nul && set "MOD=semco")
set "HCD=%D%\bcm4375B1_semco.hcd"
if "%MOD%"=="murata" set "HCD=%D%\bcm4375B1_murata.hcd"
if not defined MOD echo [WARN] module unknown ^(no cid.info: run this from Android with root, or TWRP with /data decrypted^) - using the semco patch file
if exist "%HCD%" (mkdir "%D%\bt" & copy /y "%HCD%" "%D%\bt\bcm4375B1_semco.hcd" >nul & echo [ OK ] Bluetooth patch file: %HCD% ^(module %MOD%^) -^> bcm4375\bt\) else (echo [WARN] %HCD% not on the phone - Bluetooth will be skipped)
rem v1.2: MFC firmware -> mfc\ (not a WiFi file: make-windows copies bcm4375\ to System32\drivers\bcm4375)
if exist "%D%\mfc_fw.bin" (
  if not exist "%~dp0mfc" mkdir "%~dp0mfc"
  move /y "%D%\mfc_fw.bin" "%~dp0mfc\mfc_fw.bin" >nul
  for %%S in ("%~dp0mfc\mfc_fw.bin") do echo [ OK ] MFC firmware: mfc\mfc_fw.bin ^(%%~zS bytes^)
) else (echo [WARN] mfc_fw.bin not on the phone - hardware video decode will be skipped)
dir /b "%D%"
exit /b 0
