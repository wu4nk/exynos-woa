@echo off
rem ---------------------------------------------------------------------------
rem push-to-windows.cmd (v1.0) - put the built kit straight into the Windows install on the S10
rem from TWRP, no USB stick: TWRP mounts the Windows volume (userdata, NTFS) on /data.
rem   - drivers + scripts      -> C:\S10Prov\kit\
rem   - WiFi firmware          -> C:\Windows\System32\drivers\bcm4375\ (active right away)
rem   - one-shot logon script  -> All Users StartUp\S10-install-once.cmd
rem Next Windows logon: tap "Yes" on the admin prompt; it runs install-in-windows.ps1 (log
rem C:\S10Prov\install.log), then reboots once. Run build-s10.cmd (and get-wifi-fw.cmd) first.
rem One file: the phone-side part is the sh script after the #__SH__ line at the end of this file.
rem Phone in TWRP, Windows volume mounted on /data, USB cable in.
rem ---------------------------------------------------------------------------
setlocal EnableExtensions
cd /d "%~dp0"
if not exist "out\S10Touch\S10Touch.sys" (echo [FAIL] no out\ - run build-s10.cmd first & exit /b 1)
adb get-state >nul 2>&1 || (echo [FAIL] adb sees no device & exit /b 1)
set "BL="
for /f %%B in ('adb shell "getprop ro.boot.bootloader"') do set "BL=%%B"
echo %BL%| findstr /b /c:"G973" >nul || (echo [FAIL] bootloader "%BL%" is not a Galaxy S10 ^(G973*^) & exit /b 1)
set "ROOT="
for /f %%U in ('adb shell "id -u"') do set "ROOT=%%U"
if not "%ROOT%"=="0" (echo [FAIL] adb is not root - boot the phone into TWRP & exit /b 1)
echo [ OK ] Galaxy S10 in TWRP, bootloader %BL%
if not exist "bcm4375\bcmdhd_sta.bin_b1" echo [WARN] no bcm4375\ - WiFi needs get-wifi-fw.cmd first ^(continuing^)

set "S=/tmp/s10kit"
adb shell "rm -rf %S% && mkdir -p %S%" || (echo [FAIL] adb shell & exit /b 1)
for %%D in (out setup bcm4375) do if exist "%%D\" (adb push "%%D" %S%/ >nul || (echo [FAIL] push %%D & exit /b 1))
for %%F in (install-in-windows.ps1 auto-install.ps1 collect-logs.ps1) do (adb push "%%F" %S%/ >nul || (echo [FAIL] push %%F & exit /b 1))
echo [ OK ] kit staged on the phone
set "L="
for /f "delims=:" %%N in ('findstr /n /x /c:"#__SH__" "%~f0"') do set "L=%%N"
if not defined L (echo [FAIL] embedded phone script missing & exit /b 1)
more +%L% "%~f0" > "%TEMP%\s10push.sh"
adb push "%TEMP%\s10push.sh" /tmp/s10push.crlf >nul || (echo [FAIL] push script & exit /b 1)
del "%TEMP%\s10push.sh" >nul 2>&1
adb shell "tr -d '\r' < /tmp/s10push.crlf > /tmp/s10push.sh; sh /tmp/s10push.sh %S%; R=$?; rm -rf %S% /tmp/s10push.sh /tmp/s10push.crlf; exit $R"
if errorlevel 1 (echo [FAIL] see the lines above & exit /b 1)
echo.
echo [ OK ] Reboot to Windows ^(TWRP: Reboot ^> System^), log in, tap Yes on the admin prompt.
exit /b 0
#__SH__
#!/system/bin/sh
# push-to-windows.sh (v1.0) - run ON THE PHONE (TWRP, root) by push-to-windows.cmd, AFTER the PC has
# pushed the kit to $STAGE. Places it inside the Windows volume that TWRP mounted on /data:
#   C:\S10Prov\kit\                      the kit (out\, bcm4375\, setup\, scripts)
#   C:\Windows\System32\drivers\bcm4375\ the WiFi firmware (works offline, no install step needed)
#   C:\ProgramData\Microsoft\Windows\Start Menu\Programs\StartUp\S10-install-once.cmd
#                                        runs auto-install.ps1 elevated at the next logon (one UAC "Yes"),
#                                        removes itself when the install succeeded
# NTFS under Linux is case-sensitive for lookups, Windows names are not: every path is resolved
# case-insensitively against what is really on the volume.
STAGE="${1:-/tmp/s10kit}"
W=/data
say()  { echo "[ OK ] $*"; }
fail() { echo "[FAIL] $*"; exit 1; }

# ci BASE REL -> BASE/REL with every existing component spelled as it is on disk
ci() {
  p="$1"; rest="$2"; old="$IFS"; IFS=/
  for c in $rest; do
    [ -z "$c" ] && continue
    m=$(ls -a "$p" 2>/dev/null | grep -i -x -F -- "$c" | head -n 1)
    if [ -n "$m" ]; then p="$p/$m"; else p="$p/$c"; fi
  done
  IFS="$old"; echo "$p"
}

[ -d "$STAGE/out" ] || fail "nothing staged in $STAGE (push-to-windows.cmd pushes it first)"
SYS32=$(ci "$W" "Windows/System32")
if [ ! -d "$SYS32" ]; then mount /data 2>/dev/null; SYS32=$(ci "$W" "Windows/System32"); fi
[ -d "$SYS32" ] || fail "/data is not the Windows volume (no Windows\\System32) - mount it in TWRP first"
T="$W/.s10-write-test.$$"
( : > "$T" ) 2>/dev/null && rm -f "$T" || fail "/data is read-only (Windows hibernated / fast startup?) - boot Windows, shut down fully, retry"
say "Windows volume on /data: $SYS32"

KIT=$(ci "$W" "S10Prov/kit")
mkdir -p "$KIT" || fail "mkdir $KIT"
rm -rf "$KIT/out" "$KIT/bcm4375" "$KIT/setup"
cp -r "$STAGE"/. "$KIT"/ || fail "copy the kit to $KIT"
rm -f "$KIT/install.done" "$KIT/install.tries"
say "kit -> C:\\S10Prov\\kit ($(ls "$KIT/out" | tr '\n' ' '))"

if [ -f "$STAGE/bcm4375/bcmdhd_sta.bin_b1" ]; then
  FW=$(ci "$SYS32" "drivers/bcm4375")
  mkdir -p "$FW" && cp -f "$STAGE"/bcm4375/* "$FW"/ || fail "copy WiFi firmware"
  say "WiFi firmware -> C:\\Windows\\System32\\drivers\\bcm4375 ($(ls "$FW" | wc -l) files)"
else
  echo "[WARN] no bcm4375/bcmdhd_sta.bin_b1 staged - WiFi will not start (run get-wifi-fw.cmd first)"
fi

SU=$(ci "$W" "ProgramData/Microsoft/Windows/Start Menu/Programs/StartUp")
mkdir -p "$SU" || fail "mkdir $SU"
cp -f "$STAGE/setup/S10-install-once.cmd" "$SU/S10-install-once.cmd" || fail "copy the logon launcher"
say "logon launcher -> ...\\Start Menu\\Programs\\StartUp\\S10-install-once.cmd"

sync
say "done - boot Windows, log in, tap Yes on the admin prompt; it installs and reboots once"
