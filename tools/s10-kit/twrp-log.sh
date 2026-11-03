#!/system/bin/sh
# twrp-log.sh (v1.1) - collect the S10 Windows-port logs ON THE PHONE in TWRP, no PC needed.
# Run in TWRP: Advanced > Terminal:   sh /cache/twrp-log.sh        (or wherever you put it, e.g. /usb_otg/twrp-log.sh)
#   sh twrp-log.sh nogpu   also creates \S10EDIAG\nogpu.flg (next Windows boot hides the GPU = normal screen + keyboard)
# Output: /cache/S10LOG/s10log-<date>.tar.gz on the ESP (FAT, writable in TWRP). Windows reads it after
#   mountvol S: /s  ->  copy S:\S10LOG\*.tar.gz %USERPROFILE%\Desktop\  ->  mountvol S: /d   and the browser uploads it.
# Read-only on the Windows volume (/data, NTFS). Skips the 9 MB SYSTEM hive to keep the ESP tidy.
# Note: a TWRP boot destroys the PRAM (0x8FE00000) - the per-boot GPU log is C:\S10Prov\gpuwatch.txt, copied here.

D=$(date +%Y%m%d-%H%M%S)
O=/tmp/s10log-$D
mkdir -p "$O"
say() { echo "$@"; echo "$@" >> "$O/twrp-log.txt"; }

# case-insensitive path walk (NTFS names keep their case, TWRP's ntfs driver may not fold it)
ci() {
  p="$1"; rest="$2"; old="$IFS"; IFS=/
  for c in $rest; do
    [ -z "$c" ] && continue
    m=$(ls -a "$p" 2>/dev/null | grep -i -x -F -- "$c" | head -n 1)
    if [ -n "$m" ]; then p="$p/$m"; else p="$p/$c"; fi
  done
  IFS="$old"; echo "$p"
}
grab() { [ -f "$1" ] && cp -f "$1" "$2"/ && say "[ OK ] $(basename "$1") ($(wc -c < "$1") B)"; }

say "twrp-log v1.1  $D  $(getprop ro.boot.bootloader)"

# ESP = cache partition (FAT32). TWRP mounts it at /cache.
E=/cache
if ! grep -q " /cache " /proc/mounts; then mount /cache 2>/dev/null; fi
mkdir -p "$O/esp"
for f in UEFILOG.TXT WINPRAM.BIN WINPRAM-RESTART.BIN; do grab "$(ci "$E" "$f")" "$O/esp"; done
ls -laR "$E/S10EDIAG" > "$O/esp/s10ediag-listing.txt" 2>&1

# Windows volume on /data (read-only is fine)
W=/data
[ -d "$(ci "$W" "Windows/System32")" ] || mount /data 2>/dev/null
S32=$(ci "$W" "Windows/System32")
if [ -d "$S32" ]; then
  mkdir -p "$O/win/LogFiles" "$O/win/S10Prov" "$O/win/wer"
  P=$(ci "$W" "S10Prov")
  for f in "$P"/*.txt "$P"/*.log; do grab "$f" "$O/win/S10Prov"; done
  # v1.1: the UMD's own log (Mesa stderr + abort stacks, patch 0042)
  grab "$(ci "$W" "ProgramData/S10e/umd/umd-log.txt")" "$O/win/S10Prov"
  for f in "$(ci "$S32" "LogFiles")"/*; do
    case "$(basename "$f" | tr 'A-Z' 'a-z')" in exynos*|s10*|mali*) grab "$f" "$O/win/LogFiles" ;; esac
  done
  grab "$(ci "$S32" "winevt/Logs/System.evtx")" "$O/win"
  grab "$(ci "$S32" "winevt/Logs/Application.evtx")" "$O/win"
  WER=$(ci "$W" "ProgramData/Microsoft/Windows/WER")
  find "$WER" -iname 'Report.wer' 2>/dev/null | while read -r r; do
    cp -f "$r" "$O/win/wer/$(basename "$(dirname "$r")").wer.txt" 2>/dev/null
  done
  say "[ OK ] WER: $(ls "$O/win/wer" | wc -l) reports"
else
  say "[WARN] /data is not the Windows volume"
fi

if [ "$1" = "nogpu" ]; then
  mkdir -p "$E/S10EDIAG" && touch "$E/S10EDIAG/nogpu.flg" && say "[ OK ] nogpu.flg created - next Windows boot: GPU hidden, normal screen"
fi

mkdir -p "$E/S10LOG"
T="$E/S10LOG/s10log-$D.tar.gz"
(cd /tmp && tar -czf "$T" "s10log-$D") && say "[ OK ] $T ($(wc -c < "$T") B)" || say "[FAIL] tar"
rm -rf "$O"
echo
echo "Windows (cmd as admin):  mountvol S: /s"
echo "                         copy S:\\S10LOG\\s10log-$D.tar.gz %USERPROFILE%\\Desktop\\"
echo "                         mountvol S: /d"
