@echo off
rem ---------------------------------------------------------------------------
rem get-logs-twrp.cmd (v1.1) - collect the Galaxy S10 Windows-port logs WITHOUT booting Windows.
rem Phone in TWRP, USB cable in, run on the PC:   get-logs-twrp.cmd
rem Reads only (works when TWRP mounts Windows read-only):
rem   ESP (cache): UEFILOG.TXT (firmware log: TouchChip / TouchPower / WLAN / audio), WINPRAM*.BIN
rem   Windows:     driver logs (System32\LogFiles), the SYSTEM registry hive + its .LOG1/.LOG2 (the touch
rem                driver's S10t* counters live there), System.evtx, setupapi.dev.log, C:\S10Prov logs
rem Result: s10-logs-twrp-<date>.zip next to this file - send that.
rem The phone-side part is the sh script after the #__SH__ line at the end of this file.
rem ---------------------------------------------------------------------------
setlocal EnableExtensions
cd /d "%~dp0"
adb get-state >nul 2>&1 || (echo [FAIL] adb sees no device & exit /b 1)
set "ROOT="
for /f %%U in ('adb shell "id -u"') do set "ROOT=%%U"
if not "%ROOT%"=="0" (echo [FAIL] adb is not root - boot the phone into TWRP & exit /b 1)
set "L="
for /f "delims=:" %%N in ('findstr /n /x /c:"#__SH__" "%~f0"') do set "L=%%N"
if not defined L (echo [FAIL] embedded phone script missing & exit /b 1)
more +%L% "%~f0" > "%TEMP%\s10logs.sh"
adb push "%TEMP%\s10logs.sh" /tmp/s10logs.crlf >nul || (echo [FAIL] push script & exit /b 1)
del "%TEMP%\s10logs.sh" >nul 2>&1
adb shell "tr -d '\r' < /tmp/s10logs.crlf > /tmp/s10logs.sh; sh /tmp/s10logs.sh; R=$?; rm -f /tmp/s10logs.sh /tmp/s10logs.crlf; exit $R"
if errorlevel 1 (echo [FAIL] see the lines above & exit /b 1)
for /f %%T in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set "STAMP=%%T"
set "D=s10-logs-twrp-%STAMP%"
adb pull /tmp/s10logs "%D%" >nul || (echo [FAIL] adb pull & exit /b 1)
adb shell "rm -rf /tmp/s10logs" >nul 2>&1
powershell -NoProfile -Command "Compress-Archive -Path '%D%\*' -DestinationPath '%D%.zip' -Force" || (echo [FAIL] zip & exit /b 1)
rmdir /s /q "%D%" >nul 2>&1
echo [ OK ] %CD%\%D%.zip - send this file
exit /b 0
#__SH__
#!/system/bin/sh
# get-logs-twrp.sh (v1.1) - run ON THE PHONE (TWRP, root) by get-logs-twrp.cmd. Read-only on every partition.
O=/tmp/s10logs
rm -rf "$O"; mkdir -p "$O/esp" "$O/windows"
ci() {   # case-insensitive path resolution (NTFS / FAT names under Linux)
  p="$1"; rest="$2"; old="$IFS"; IFS=/
  for c in $rest; do
    [ -z "$c" ] && continue
    m=$(ls -a "$p" 2>/dev/null | grep -i -x -F -- "$c" | head -n 1)
    if [ -n "$m" ]; then p="$p/$m"; else p="$p/$c"; fi
  done
  IFS="$old"; echo "$p"
}
grab() { [ -f "$1" ] && cp -f "$1" "$2"/ && echo "[ OK ] $(basename "$1") ($(wc -c < "$1") B)"; }

{ echo "get-logs-twrp v1.1  $(date)"; getprop ro.boot.bootloader; uname -a; cat /proc/mounts; } > "$O/twrp.txt" 2>&1

# --- ESP = cache partition (FAT32) ---
ED=$(readlink -f /dev/block/by-name/cache 2>/dev/null)
E=$(grep "^$ED " /proc/mounts | head -n 1 | cut -d' ' -f2); MINE=""
if [ -z "$E" ]; then E=/tmp/s10esp; mkdir -p "$E"; mount -t vfat -o ro "$ED" "$E" 2>/dev/null && MINE=1 || E=""; fi
if [ -n "$E" ]; then
  for f in UEFILOG.TXT WINPRAM.BIN WINPRAM-RESTART.BIN; do grab "$(ci "$E" "$f")" "$O/esp"; done
  ls -laR "$E" > "$O/esp/listing.txt" 2>&1
  [ -n "$MINE" ] && umount "$E"
else
  echo "[WARN] ESP ($ED) did not mount - no UEFILOG.TXT"
fi

# --- Windows volume on /data (read-only is fine) ---
W=/data
[ -d "$(ci "$W" "Windows/System32")" ] || mount /data 2>/dev/null
S32=$(ci "$W" "Windows/System32")
if [ -d "$S32" ]; then
  mkdir -p "$O/windows/LogFiles" "$O/windows/S10Prov"
  for f in "$(ci "$S32" "LogFiles")"/*; do
    case "$(basename "$f" | tr 'A-Z' 'a-z')" in exynos*|s10*|mali*) grab "$f" "$O/windows/LogFiles" ;; esac
  done
  for f in SYSTEM SYSTEM.LOG1 SYSTEM.LOG2; do grab "$(ci "$S32" "config/$f")" "$O/windows"; done
  grab "$(ci "$S32" "winevt/Logs/System.evtx")" "$O/windows"
  # v1.1: who crashed in user mode (dwm / LogonUI / the Mesa UMD) - Application log + the WER report texts (no dumps)
  grab "$(ci "$S32" "winevt/Logs/Application.evtx")" "$O/windows"
  WER=$(ci "$W" "ProgramData/Microsoft/Windows/WER")
  if [ -d "$WER" ]; then
    mkdir -p "$O/windows/wer"
    find "$WER" -iname 'Report.wer' 2>/dev/null | while read -r r; do
      cp -f "$r" "$O/windows/wer/$(basename "$(dirname "$r")").wer.txt" 2>/dev/null
    done
    ls -laR "$WER" > "$O/windows/wer/listing.txt" 2>&1
    echo "[ OK ] WER: $(ls "$O/windows/wer" | wc -l) files"
  fi
  grab "$(ci "$W" "Windows/INF/setupapi.dev.log")" "$O/windows"
  P=$(ci "$W" "S10Prov")
  for f in "$P"/*.log "$P"/*.txt; do grab "$f" "$O/windows/S10Prov"; done
  Z=$(ls -t "$P"/s10-logs-*.zip 2>/dev/null | head -n 1); [ -n "$Z" ] && grab "$Z" "$O/windows/S10Prov"
  # v1.1: the newest s10-log.cmd run folder (C:\S10Prov\logs\<stamp>), even if its zip never reached C:\log.zip
  L=$(ls -td "$(ci "$P" "logs")"/*/ 2>/dev/null | head -n 1)
  [ -n "$L" ] && cp -rf "$L" "$O/windows/S10Prov/" && echo "[ OK ] s10-log run $(basename "$L")"
  ls -la "$(ci "$P" "kit/out")" > "$O/windows/kit-out-listing.txt" 2>&1
else
  echo "[WARN] /data is not the Windows volume - only the ESP part was collected"
fi
chmod -R a+r "$O"
echo "[ OK ] collected $(du -sk "$O" | cut -f1) KiB"
