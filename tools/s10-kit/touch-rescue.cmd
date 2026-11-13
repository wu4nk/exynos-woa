@echo off
rem ---------------------------------------------------------------------------
rem touch-rescue.cmd (v1.0) - Galaxy S10: touch gone and no keyboard / mouse (no OTG). Run ON THE PC, phone in
rem TWRP, USB cable in (same adb as push-to-windows.cmd). 2026-11-13: the phone powered off during a
rem "power-s10.ps1 devices" run (pnputil /remove-device of Wi-Fi / audio / USB host / touch) and came back
rem without touch.
rem   touch-rescue.cmd        writes C:\S10Prov\touch-rescue.ps1 into the Windows volume and makes the existing
rem                           elevated logon task ("S10 kit watcher" -> C:\S10Prov\kit-watch.ps1) run it ONCE:
rem                           enable + rescan + restart the touch (and Wi-Fi / audio / USB host), record their
rem                           state and the crash events, then put kit-watch.ps1 back. Reboot to Windows, wait
rem                           ~1 minute at the desktop - touch should come back by itself.
rem   touch-rescue.cmd log    back in TWRP: pulls C:\S10Prov\touch-rescue.log to this folder - send it.
rem Needs TWRP to WRITE the Windows volume (it tries remount / ntfsfix like push-to-windows.cmd).
rem ---------------------------------------------------------------------------
setlocal EnableExtensions
cd /d "%~dp0"
adb get-state >nul 2>&1 || (echo [FAIL] adb sees no device - phone in TWRP, USB cable in & exit /b 1)
set "BL="
for /f %%B in ('adb shell "getprop ro.boot.bootloader"') do set "BL=%%B"
echo %BL%| findstr /b /c:"G973" >nul || (echo [FAIL] bootloader "%BL%" is not a Galaxy S10 ^(G973*^) & exit /b 1)
set "ROOT="
for /f %%U in ('adb shell "id -u"') do set "ROOT=%%U"
if not "%ROOT%"=="0" (echo [FAIL] adb is not root - boot the phone into TWRP & exit /b 1)
set "MODE=fix"
if /i "%~1"=="log" set "MODE=log"
set "L="
for /f "delims=:" %%N in ('findstr /n /x /c:"#__SH__" "%~f0"') do set "L=%%N"
if not defined L (echo [FAIL] embedded phone script missing & exit /b 1)
more +%L% "%~f0" > "%TEMP%\s10rescue.sh"
adb push "%TEMP%\s10rescue.sh" /tmp/s10rescue.crlf >nul || (echo [FAIL] push script & exit /b 1)
del "%TEMP%\s10rescue.sh" >nul 2>&1
adb shell "tr -d '\r' < /tmp/s10rescue.crlf > /tmp/s10rescue.sh; sh /tmp/s10rescue.sh %MODE%; R=$?; rm -f /tmp/s10rescue.sh /tmp/s10rescue.crlf; exit $R"
if errorlevel 1 (echo [FAIL] see the lines above & exit /b 1)
if "%MODE%"=="log" (
  adb pull /tmp/touch-rescue.log "%~dp0touch-rescue.log" >nul && echo [ OK ] %~dp0touch-rescue.log - send it
  exit /b 0
)
echo.
echo [ OK ] TWRP: Reboot ^> System. At the Windows desktop wait ~1 minute without doing anything.
echo        Touch back: run s10-log.cmd and send the log. Still no touch: back to TWRP, "touch-rescue.cmd log".
exit /b 0
#__SH__
#!/system/bin/sh
# touch-rescue.sh (v1.0) - phone side of touch-rescue.cmd (TWRP, root)
MODE="$1"
W=/data
say()  { echo "[ OK ] $*"; }
fail() { echo "[FAIL] $*"; exit 1; }
ci() {
  p="$1"; rest="$2"; old="$IFS"; IFS=/
  for c in $rest; do
    [ -z "$c" ] && continue
    m=$(ls -a "$p" 2>/dev/null | grep -i -x -F -- "$c" | head -n 1)
    if [ -n "$m" ]; then p="$p/$m"; else p="$p/$c"; fi
  done
  IFS="$old"; echo "$p"
}
SYS32=$(ci "$W" "Windows/System32")
if [ ! -d "$SYS32" ]; then mount /data 2>/dev/null; SYS32=$(ci "$W" "Windows/System32"); fi
[ -d "$SYS32" ] || fail "/data is not the Windows volume (no Windows\\System32) - mount Data in TWRP first"
P=$(ci "$W" "S10Prov")

if [ "$MODE" = log ]; then
  F=$(ci "$P" "touch-rescue.log")
  [ -f "$F" ] || fail "no C:\\S10Prov\\touch-rescue.log - the rescue did not run (was the desktop reached?)"
  cp "$F" /tmp/touch-rescue.log && say "touch-rescue.log ($(wc -c < /tmp/touch-rescue.log) B)"
  exit 0
fi

# same read-write steps as push-to-windows.sh v1.6 (a crash / forced restart leaves NTFS dirty -> read-only)
writable() {
  grep -q " $W " /proc/mounts || return 1
  [ -d "$(ci "$W" "Windows/System32")" ] || return 1
  T="$(ci "$W" "Windows")/.s10-write-test.$$"; ( : > "$T" ) 2>/dev/null && rm -f "$T"
}
make_rw() {
  writable && return 0
  echo "[WARN] /data is read-only - trying to fix it"
  DEV=$(grep " $W " /proc/mounts | head -n 1 | cut -d' ' -f1)
  case "$DEV" in /dev/fuse|fuse*|"") DEV=$(readlink -f /dev/block/by-name/userdata 2>/dev/null) ;; esac
  [ -n "$DEV" ] || DEV=/dev/block/by-name/userdata
  mount -o remount,rw "$W" 2>/dev/null; writable && { say "remounted /data read-write"; return 0; }
  umount "$W" 2>/dev/null || umount -l "$W" 2>/dev/null
  command -v ntfsfix >/dev/null 2>&1 && ntfsfix -d "$DEV" 2>&1 | sed 's/^/       ntfsfix: /'
  for try in ntfs-3g mount.ntfs-3g mount.ntfs; do
    command -v $try >/dev/null 2>&1 || continue
    $try -o rw,remove_hiberfile "$DEV" "$W" 2>&1 | sed "s/^/       $try: /"
    writable && { say "mounted /data read-write with $try"; return 0; }
    umount "$W" 2>/dev/null
  done
  for opt in "-t ntfs3 -o rw,force" "-t ntfs -o rw" "-o rw"; do
    mount $opt "$DEV" "$W" 2>/dev/null
    writable && { say "mounted /data read-write (mount $opt)"; return 0; }
    umount "$W" 2>/dev/null
  done
  grep -q " $W " /proc/mounts || mount "$DEV" "$W" 2>/dev/null || mount /data 2>/dev/null
  return 1
}
make_rw || fail "TWRP cannot write the Windows volume - nothing changed (send this output)"
P=$(ci "$W" "S10Prov")
KW=$(ci "$P" "kit-watch.ps1")
[ -f "$KW" ] || fail "no C:\\S10Prov\\kit-watch.ps1 - the elevated logon task is not installed, nothing changed"
if [ ! -f "$KW.s10orig" ]; then cp "$KW" "$KW.s10orig" || fail "backup of kit-watch.ps1"; fi
say "kit-watch.ps1 kept as kit-watch.ps1.s10orig"

# the one-shot rescue (runs elevated, hidden, at the next logon) - PowerShell, CRLF
printf '%s\r\n' \
'# touch-rescue.ps1 (v1.0) - written by touch-rescue.cmd from TWRP; runs ONCE as C:\S10Prov\kit-watch.ps1' \
'$ErrorActionPreference = "Continue"' \
'$log = "C:\S10Prov\touch-rescue.log"' \
'function L($m) { "$(Get-Date -Format s)  $m" | Add-Content -Path $log -Encoding utf8 }' \
'L "touch-rescue v1.0 start (uptime $([int]((Get-Date) - (Get-CimInstance Win32_OperatingSystem).LastBootUpTime).TotalSeconds) s)"' \
'try {' \
'  Start-Sleep -Seconds 10' \
'  $ids = "ACPI\SCSY0761\0", "ACPI\SAMS9825\0", "ACPI\SAMS9827\0", "ACPI\PNP0D10\0", "ACPI\SAMS9829\0"' \
'  foreach ($id in $ids) { L ("before " + ((pnputil /enum-devices /instanceid $id 2>&1 | Out-String) -replace "\s+", " ")) }' \
'  foreach ($id in $ids) { L ("enable ${id}: " + ((pnputil /enable-device $id 2>&1 | Out-String) -replace "\s+", " ")) }' \
'  L ("scan: " + ((pnputil /scan-devices 2>&1 | Out-String) -replace "\s+", " "))' \
'  Start-Sleep -Seconds 8' \
'  $t = Get-PnpDevice -InstanceId "ACPI\SCSY0761\0" -ErrorAction SilentlyContinue' \
'  if (-not $t -or $t.Status -ne "OK") { L ("touch not OK -> restart: " + ((pnputil /restart-device "ACPI\SCSY0761\0" 2>&1 | Out-String) -replace "\s+", " ")); Start-Sleep -Seconds 8 }' \
'  foreach ($id in $ids) { $d = Get-PnpDevice -InstanceId $id -ErrorAction SilentlyContinue; $p = (Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_ProblemCode -ErrorAction SilentlyContinue).Data; L ("after  {0}: {1} problem {2} ({3})" -f $id, $d.Status, $p, $d.FriendlyName) }' \
'  L "--- system events (newest 60: bugcheck / kernel-power / pnp / errors)"' \
'  Get-WinEvent -LogName System -MaxEvents 400 -ErrorAction SilentlyContinue | Where-Object { $_.Level -le 2 -or $_.Id -in 41, 1001, 6008, 219, 400, 410, 411, 420, 430 } | Select-Object -First 60 | ForEach-Object { L ("{0:s} {1} {2} {3}" -f $_.TimeCreated, $_.ProviderName, $_.Id, (($_.Message -split "`n")[0])) }' \
'  Get-ChildItem C:\Windows\Minidump, C:\Windows\MEMORY.DMP -ErrorAction SilentlyContinue | ForEach-Object { L ("dump {0} {1} B {2:s}" -f $_.FullName, $_.Length, $_.LastWriteTime) }' \
'} catch { L "error: $_" }' \
'finally {' \
'  if (Test-Path "C:\S10Prov\kit-watch.ps1.s10orig") { Copy-Item "C:\S10Prov\kit-watch.ps1.s10orig" "C:\S10Prov\kit-watch.ps1" -Force; Remove-Item "C:\S10Prov\kit-watch.ps1.s10orig" -Force; L "kit-watch.ps1 restored" }' \
'  L "done"' \
'}' \
> "$KW" || fail "write the rescue script"
sync
say "rescue script in place (C:\\S10Prov\\kit-watch.ps1, runs once at the next logon, then restores itself)"
exit 0
