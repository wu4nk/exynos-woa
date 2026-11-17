#!/system/bin/sh
# run-isphost-s10.sh (v1.0) - Galaxy S10 on ANDROID (root): runs tools/isp-host/test_front on the phone's own CPU
# (notes/s10-isp.md, Stage 4 option A). Samsung's ISP libraries from this phone's firmware partition are loaded into
# an ordinary process and driven through the whole front-camera path (init, setfile, calibration, N frames) against
# FAKE register windows filled from the earlier register dump. Nothing touches the camera hardware: no /dev/mem, no
# driver, the camera does not need to be open. Purpose: real-silicon proof of the native host + time per frame.
#
#   unpack isphost-s10.tar.gz to /sdcard/s10cam/isphost (any folder works), then
#   su -c "sh /sdcard/s10cam/isphost/run-isphost-s10.sh"
#   optional: su -c "sh .../run-isphost-s10.sh /path/to/rom1_cal.bin"     (default: searched in /sdcard)
#   optional env FRAMES (default 30)
# Result: /sdcard/s10cam/isphost-out.tar.gz (host.log, timing.txt, mmio-*.txt, regs-*.txt, interface-calls.txt).
# The libraries are copied from /vendor into /data/local/tmp/isphost only for the run; nothing is sent anywhere.
D=$(cd "$(dirname "$0")" && pwd)
W=/data/local/tmp/isphost
OUT=/sdcard/s10cam
echo "run-isphost-s10 v1.0"
if [ "$(id -u)" != 0 ]; then echo "needs root: su -c \"sh $0\""; exit 1; fi
for f in test_front sensor_itf_layout.txt dump/3AA0.txt dump/ISPLP.txt; do
    [ -f "$D/$f" ] || { echo "missing $D/$f - unpack the whole tarball"; exit 1; }
done
rm -rf "$W"
mkdir -p "$W/bin" "$W/dump" "$W/out" "$OUT" || exit 1
cp "$D/test_front" "$D/sensor_itf_layout.txt" "$W/" && cp "$D/dump/3AA0.txt" "$D/dump/ISPLP.txt" "$W/dump/" || exit 1
chmod 755 "$W/test_front"
for f in fimc_is_lib.bin fimc_is_rta.bin setfile_3j1.bin; do
    p=$(find /vendor /system /odm -xdev -name "$f" 2>/dev/null | head -n 1)
    [ -n "$p" ] || { echo "not found on this phone: $f"; exit 1; }
    cp "$p" "$W/bin/" || exit 1
    echo "library: $p ($(wc -c < "$p") bytes)"
done
CAL=$1
if [ -z "$CAL" ]; then
    for c in /sdcard/s10cam/rom1_cal.bin /sdcard/rom1_cal.bin /sdcard/Download/rom1_cal.bin "$D/rom1_cal.bin"; do
        [ -f "$c" ] && { CAL=$c; break; }
    done
fi
if [ -n "$CAL" ]; then echo "calibration: $CAL"; cp "$CAL" "$W/rom1_cal.bin"; CALARG=$W/rom1_cal.bin
else echo "calibration: none found (a zero buffer is used; the run still works)"; CALARG=; fi
echo "running $W/test_front, FRAMES=${FRAMES:-30} ..."
cd "$W" || exit 1
FRAMES=${FRAMES:-30} QUIET=1 ./test_front "$W/bin" "$W/dump" "$W/out" "$W/sensor_itf_layout.txt" $CALARG
RC=$?
echo "exit code $RC"
grep -E '^== |CRASH|steady|trapped register|time-out' "$W/out/host.log" | tail -n 12
rm -f "$W/rom1_cal.bin"
rm -rf "$W/bin"
cd "$W" && tar czf "$OUT/isphost-out.tar.gz" out && echo "result: $OUT/isphost-out.tar.gz"
