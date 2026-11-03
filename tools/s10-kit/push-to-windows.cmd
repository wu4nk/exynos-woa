@echo off
rem ---------------------------------------------------------------------------
rem push-to-windows.cmd (v1.6) - put the built kit straight into the Windows install on the S10
rem   push-to-windows.cmd       (v1.4 default) the kit goes to /cache = the EFI partition (ESP:\S10KIT +
rem                             INSTALL.CMD); the Windows volume is not touched at all
rem   push-to-windows.cmd c     straight into C: (only when TWRP can write the Windows volume)
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
set "MODE=esp"
if /i "%~1"=="c" set "MODE=c"
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
for %%D in (out setup bcm4375 out-gpu) do if exist "%%D\" (adb push "%%D" %S%/ >nul || (echo [FAIL] push %%D & exit /b 1))
for %%F in (install-in-windows.ps1 auto-install.ps1 collect-logs.ps1 kit-watch.ps1 s10-log.cmd gpu-s10.ps1 umd-test.ps1 gpu-watch.ps1 gpu-fill.ps1) do if exist "%%F" (adb push "%%F" %S%/ >nul || (echo [FAIL] push %%F & exit /b 1))
echo [ OK ] kit staged on the phone
set "L="
for /f "delims=:" %%N in ('findstr /n /x /c:"#__SH__" "%~f0"') do set "L=%%N"
if not defined L (echo [FAIL] embedded phone script missing & exit /b 1)
more +%L% "%~f0" > "%TEMP%\s10push.sh"
adb push "%TEMP%\s10push.sh" /tmp/s10push.crlf >nul || (echo [FAIL] push script & exit /b 1)
del "%TEMP%\s10push.sh" >nul 2>&1
adb shell "tr -d '\r' < /tmp/s10push.crlf > /tmp/s10push.sh; sh /tmp/s10push.sh %S% %MODE%; R=$?; rm -rf %S% /tmp/s10push.sh /tmp/s10push.crlf; exit $R"
if errorlevel 4 (echo [FAIL] see the lines above & exit /b 1)
if errorlevel 3 goto :esp
if errorlevel 1 (echo [FAIL] see the lines above & exit /b 1)
echo.
echo [ OK ] Reboot to Windows ^(TWRP: Reboot ^> System^), log in, tap Yes on the admin prompt.
exit /b 0
:esp
echo.
echo [ OK ] Kit written to /cache = the EFI partition ^(ESP:\S10KIT^).
echo        If the kit watcher is installed: just boot Windows and answer Yes - it installs without a restart.
echo        First time only ^(no watcher yet^):
echo        Reboot to Windows, open cmd AS ADMINISTRATOR and type:
echo.
echo            mountvol S: /s
echo            S:\S10KIT\INSTALL.CMD
echo.
echo        Then: rmdir /s /q S:\S10KIT  ^&  mountvol S: /d  ^&  shutdown /r /t 0
echo        INSTALL.CMD copies the kit to C:\S10Prov\kit, installs the drivers, turns off hibernation ^(the usual
echo        reason TWRP mounts Windows read-only^) and tells you when to reboot.
exit /b 0
#__SH__
#!/system/bin/sh
# push-to-windows.sh (v1.6) - run ON THE PHONE (TWRP, root) by push-to-windows.cmd, AFTER the PC has
# pushed the kit to $STAGE. Places it inside the Windows volume that TWRP mounted on /data:
#   C:\S10Prov\kit\                      the kit (out\, bcm4375\, setup\, scripts)
#   C:\Windows\System32\drivers\bcm4375\ the WiFi firmware (works offline, no install step needed)
#   C:\ProgramData\Microsoft\Windows\Start Menu\Programs\StartUp\S10-install-once.cmd
#                                        runs auto-install.ps1 elevated at the next logon (one UAC "Yes"),
#                                        removes itself when the install succeeded
# NTFS under Linux is case-sensitive for lookups, Windows names are not: every path is resolved
# case-insensitively against what is really on the volume.
STAGE="${1:-/tmp/s10kit}"
MODE="$2"
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
if [ "$MODE" = c ]; then
  SYS32=$(ci "$W" "Windows/System32")
  if [ ! -d "$SYS32" ]; then mount /data 2>/dev/null; SYS32=$(ci "$W" "Windows/System32"); fi
  [ -d "$SYS32" ] || fail "/data is not the Windows volume (no Windows\\System32) - mount it in TWRP first"
fi
# v1.3: "writable" only counts when /data IS the Windows volume. v1.1/v1.2 tested the bare directory too:
# after a failed re-mount /data is TWRP's own ramdisk folder - writable, empty, gone at reboot - so the kit
# "landed" in RAM. Now: a mount point, Windows\System32 present, and the test file written there.
writable() {
  grep -q " $W " /proc/mounts || return 1
  [ -d "$(ci "$W" "Windows/System32")" ] || return 1
  T="$(ci "$W" "Windows")/.s10-write-test.$$"; ( : > "$T" ) 2>/dev/null && rm -f "$T"
}

# v1.1: a Windows that crashed, was force-restarted (Vol Down + Power) or shut down with fast startup
# leaves NTFS "dirty" / hibernated, and TWRP then mounts it read-only. Try to get it writable here:
# remount; then ntfsfix -d (clear the dirty flag) and mount again with ntfs-3g remove_hiberfile
# (drops only the fast-startup hibernation image: Windows does a normal cold boot) or ntfs3 force.
make_rw() {
  writable && return 0
  echo "[WARN] /data is read-only - trying to fix it"
  grep " $W " /proc/mounts | head -n 1 | sed 's/^/       mount: /'
  DEV=$(grep " $W " /proc/mounts | head -n 1 | cut -d' ' -f1)
  case "$DEV" in /dev/fuse|fuse*|"") DEV=$(readlink -f /dev/block/by-name/userdata 2>/dev/null) ;; esac
  [ -n "$DEV" ] || DEV=/dev/block/by-name/userdata
  echo "       device: $DEV"

  mount -o remount,rw "$W" 2>/dev/null; writable && { say "remounted /data read-write"; return 0; }

  umount "$W" 2>/dev/null || umount -l "$W" 2>/dev/null
  if command -v ntfsfix >/dev/null 2>&1; then
    ntfsfix -d "$DEV" 2>&1 | sed 's/^/       ntfsfix: /'
  fi
  for try in ntfs-3g mount.ntfs-3g mount.ntfs; do
    command -v $try >/dev/null 2>&1 || continue
    $try -o rw,remove_hiberfile "$DEV" "$W" 2>&1 | sed "s/^/       $try: /"
    writable && { say "mounted /data read-write with $try (remove_hiberfile)"; return 0; }
    umount "$W" 2>/dev/null
  done
  for opt in "-t ntfs3 -o rw,force" "-t ntfs -o rw" "-o rw"; do
    mount $opt "$DEV" "$W" 2>/dev/null
    writable && { say "mounted /data read-write (mount $opt)"; return 0; }
    umount "$W" 2>/dev/null
  done
  grep -q " $W " /proc/mounts || mount "$DEV" "$W" 2>/dev/null || mount /data 2>/dev/null   # back as TWRP had it
  return 1
}
# v1.2: when NTFS stays read-only, put the kit on the ESP instead (FAT32 = the cache partition, always
# writable from TWRP) with S10KIT\INSTALL.CMD; exit 3 tells push-to-windows.cmd to print the Windows steps.
esp_fallback() {
  ED=$(readlink -f /dev/block/by-name/cache 2>/dev/null)
  [ -n "$ED" ] || fail "no cache partition"
  # /cache as TWRP mounts it; mount it there if TWRP did not
  E=$(grep "^$ED " /proc/mounts | head -n 1 | cut -d' ' -f2)
  if [ -z "$E" ]; then
    E=/cache; mkdir -p "$E"
    mount -t vfat -o rw "$ED" "$E" 2>/dev/null || fail "/cache ($ED) does not mount as FAT32"
  fi
  mount -o remount,rw "$E" 2>/dev/null
  [ -d "$(ci "$E" "EFI")" ] || fail "$ED mounted on $E has no EFI\\ folder - not the Windows ESP, nothing written"
  K="$E/S10KIT"
  rm -rf "$K"; mkdir -p "$K" || fail "mkdir $K"
  cp -r "$STAGE"/. "$K"/ || fail "copy the kit to the ESP (full?)"
  date +%Y%m%d-%H%M%S > "$K/kit.stamp"     # v1.5: kit-watch.ps1 (logon) offers each new stamp once
  printf '%s\r\n' \
    '@echo off' \
    'rem S10KIT\INSTALL.CMD - written by push-to-windows v1.6 (TWRP could only read the Windows volume).' \
    'rem In Windows, an ADMINISTRATOR cmd:   mountvol S: /s   then   S:\S10KIT\INSTALL.CMD' \
    'net session >nul 2>&1 || (echo [FAIL] open cmd as Administrator first & exit /b 1)' \
    'robocopy "%~dp0." C:\S10Prov\kit /E /R:1 /W:1 /NFL /NDL /NJH /NJS /NP >nul' \
    'if errorlevel 8 (echo [FAIL] copy to C:\S10Prov\kit & exit /b 1)' \
    'del /q C:\S10Prov\kit\INSTALL.CMD >nul 2>&1' \
    'echo [ OK ] kit copied to C:\S10Prov\kit' \
    'powercfg /h off' \
    'echo [ OK ] hibernation / fast startup off (TWRP can write C: after the next full shutdown)' \
    'fsutil dirty query C:' \
    'cd /d C:\S10Prov\kit' \
    'powershell -NoProfile -ExecutionPolicy Bypass -File C:\S10Prov\kit\install-in-windows.ps1' \
    'echo.' \
    'echo Done. Type:  mountvol S: /d   then reboot (shutdown /r /t 0).' \
    > "$K/INSTALL.CMD" || fail "write INSTALL.CMD"
  sync
  say "kit -> $E/S10KIT = ESP:\\S10KIT ($(du -sk "$K" | cut -f1) KiB) + INSTALL.CMD"
  exit 3
}
if [ "$MODE" = esp ]; then
  echo "[ OK ] writing to /cache (the EFI partition); the Windows volume is not touched"
  esp_fallback
fi
make_rw || { echo "[WARN] /data stays read-only - using the ESP instead"; esp_fallback; }
SYS32=$(ci "$W" "Windows/System32")
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
