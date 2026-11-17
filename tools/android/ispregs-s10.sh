#!/system/bin/sh
# ispregs-s10.sh (v1.2) - Galaxy S10 on ANDROID (root): read the camera ISP's registers WHILE Samsung's camera stack
# drives them, as ground truth for the ISP emulator and the Windows port (notes/s10-isp.md, 2026-11-17).
# READS ONLY (32-bit loads through /dev/mem). Nothing is written to any register.
#
#   adb push ispregs-s10.sh /data/local/tmp/
#   adb shell su -c "sh /data/local/tmp/ispregs-s10.sh check"
#       -> says whether this kernel lets root read registers at all (/dev/mem + devmem). Nothing else happens.
#   then: open the Camera app on the FRONT camera and leave it on the preview, and run
#   adb shell su -c "sh /data/local/tmp/ispregs-s10.sh ids"     the version registers only (a few reads, seconds)
#   adb shell su -c "sh /data/local/tmp/ispregs-s10.sh dump"    3AA0 + ISPLP + MCSC, 64 KiB each (a few minutes)
#   adb pull /sdcard/s10cam/ispregs
#
# Safety: a register block whose power domain is off cannot be read - the read would stop the phone (it reboots).
# So every read happens only while the fimc_is device reports runtime_status "active" (= the camera is open), and the
# script stops as soon as that changes. Keep the Camera app open on the front camera until it says "done".
# Register bases (phone's device tree, fimc_is@17190000 reg list; GPL fimc-is-v7_0_0/fimc-is-hw-chain.h:140 order):
#   3AA0 0x17190000, ISPLP (ISP0) 0x17430000, MCSC 0x17440000, each 0x10000.
MODE=${1:-check}
OUT=/sdcard/s10cam/ispregs
mkdir -p $OUT
LOG=$OUT/ispregs-$MODE.txt
echo "ispregs-s10.sh v1.2 $MODE $(date)" > $LOG
say() { echo "$*"; echo "$*" >> $LOG; }

DM=""
if [ -c /dev/mem ]; then say "/dev/mem: present"; else say "/dev/mem: MISSING (kernel built without CONFIG_DEVMEM) - registers cannot be read this way"; fi
# applet lists, not usage texts (their wording differs between toybox / busybox versions)
if toybox 2>/dev/null | tr ' ' '\n' | grep -qx devmem; then DM="toybox devmem"; fi
if [ -z "$DM" ]; then
  for b in /data/adb/ksu/bin/busybox /data/adb/magisk/busybox /system/xbin/busybox; do
    [ -x $b ] && $b --list 2>/dev/null | grep -qx devmem && DM="$b devmem" && break
  done
fi
say "devmem tool: ${DM:-none}"

# v1.2: the ISP device itself (DT fimc_is@17190000). v1.0's glob took 17030000.fimc_is_sensor (a sensor/CSIS
# node, another power state); v1.1 looked only at /sys/devices/platform/17190000.fimc_is, which does not exist
# on the user's phone (2026-11-17 20:08) - search every sysfs device directory named after 17190000.
ST=""
for d in $(find /sys/devices -maxdepth 4 -type d -name '*17190000*' 2>/dev/null); do
  say "candidate: $d ($(cat $d/power/runtime_status 2>/dev/null || echo 'no runtime_status'))"
  [ -z "$ST" ] && [ -r $d/power/runtime_status ] && ST=$d/power/runtime_status
done
[ -z "$ST" ] && say "fimc nodes seen: $(find /sys/devices -maxdepth 4 -type d -name '*fimc*' 2>/dev/null | tr '\n' ' ')"
say "fimc_is runtime_status file: ${ST:-not found}"
[ -n "$ST" ] && say "fimc_is runtime_status now: $(cat $ST)"

if [ "$MODE" = check ] || [ ! -c /dev/mem ] || [ -z "$DM" ] || [ -z "$ST" ]; then
  say "check only - nothing read. Send $LOG back."
  exit 0
fi

active() { [ "$(cat $ST)" = active ]; }
if ! active; then say "the camera is not open (runtime_status $(cat $ST)): open the front camera first"; exit 1; fi

rd() { $DM $1 32 2>/dev/null; }

if [ "$MODE" = ids ]; then
  for blk in 3AA0:0x17190000 ISPLP:0x17430000 MCSC:0x17440000; do
    n=${blk%%:*}; b=${blk##*:}
    for o in 0x00f0 0x00f4 0x00f8; do
      active || { say "camera closed - stopped"; exit 1; }
      a=$(printf '0x%08x' $((b + o)))
      say "$n +$o ($a) = $(rd $a)"
    done
  done
  say "done"
  exit 0
fi

if [ "$MODE" = dump ]; then
  for blk in 3AA0:0x17190000 ISPLP:0x17430000 MCSC:0x17440000; do
    n=${blk%%:*}; b=${blk##*:}
    F=$OUT/$n.txt
    echo "# $n base $b, offset value (32-bit reads), $(date)" > $F
    o=0
    while [ $o -lt 65536 ]; do
      # power check every 64 registers: closing the camera mid-dump must not reach a powered-off block
      if [ $((o % 256)) -eq 0 ]; then
        active || { say "camera closed at $n +$o - stopped (partial file kept)"; exit 1; }
        [ $((o % 4096)) -eq 0 ] && printf '\r%s +0x%04x ' $n $o
      fi
      printf '%04x %s\n' $o "$(rd $(printf '0x%08x' $((b + o))))" >> $F
      o=$((o + 4))
    done
    echo
    say "$n: $F"
  done
  say "done"
  exit 0
fi
say "unknown mode $MODE (check | ids | dump)"
