@echo off
rem ---------------------------------------------------------------------------
rem get-wifi-fw.cmd (v1.0) - copy the S10's own BCM4375 WiFi firmware off the phone into
rem bcm4375\ next to this file. make-windows.ps1 puts it in Windows\System32\drivers\bcm4375\,
rem where Exynos9820WlanCx loads it (bcmdhd_sta.bin_b1, bcmdhd_clm.blob, nvram.txt_*_b1; the
rem nvram is picked by the module's OTP vendor id). The blobs are Broadcom/Samsung's and are
rem never shipped in the kit: they come from THIS phone's vendor partition.
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
dir /b "%D%"
exit /b 0
