#!/system/bin/sh
# camtrace-s10.sh (v1.0) - Galaxy S10 on ANDROID (root): collect what the Samsung camera stack loads and record how it
# talks to the front sensor, as a reference for the Windows driver (notes/s10-camera.md, 2026-11-17).
# READ-ONLY for the phone: copies files, turns kernel trace events on for one window, reads logs. Writes nothing to
# /vendor or /system, touches no device node.
#
#   adb push camtrace-s10.sh /data/local/tmp/
#   adb shell su -c "sh /data/local/tmp/camtrace-s10.sh 25"      (25 = seconds to record; default 25)
#   -> while it counts down: open the Camera app, switch to the FRONT camera, wait ~5 s, record ~5 s of video,
#      take a photo, close the app.
#   adb pull /sdcard/s10cam
#
# Output in /sdcard/s10cam/:
#   files/          fimc_is_lib.bin (ISP "DDK"), fimc_is_rta.bin (3A), setfile_3j1.bin (front tuning), the camera HAL
#                   libraries - Samsung's binaries from YOUR phone: for study only, never redistribute them
#   i2c-trace.txt   every I2C transfer while the camera ran (sensor 3J1 = address 0x10 -> "a=010"; PMIC S2MPB02 = 0x59 -> "a=059")
#   isfw-msg.txt    the ISP library's own log (debugfs fimc-is/isfw-msg), if the kernel has debugfs
#   dmesg-camera.txt kernel messages of the camera drivers
#   info.txt        kernel version, file list with sizes / sha256, camera properties
SECS=${1:-25}
OUT=/sdcard/s10cam
mkdir -p $OUT/files
INFO=$OUT/info.txt
echo "camtrace-s10.sh v1.0  $(date)" > $INFO
uname -a >> $INFO

echo "== files" | tee -a $INFO
for f in $(find /vendor /system /odm /product -xdev \( -name 'fimc_is_*.bin' -o -name 'setfile_*.bin' \
          -o -name 'libexynoscamera*.so' -o -name 'camera.exynos*.so' -o -name 'libhwjpeg*.so' \) 2>/dev/null); do
  # keep 64-bit and 32-bit copies apart
  case $f in */lib/*) d=$OUT/files/lib32;; */lib64/*) d=$OUT/files/lib64;; *) d=$OUT/files;; esac
  mkdir -p $d
  cp -p "$f" "$d/" && echo "$(sha256sum "$f" | cut -c1-16)  $(wc -c < "$f")  $f" | tee -a $INFO
done

echo "== camera properties" >> $INFO
getprop | grep -i -E 'camera|cam\.' >> $INFO

# tracefs: newer kernels mount it at /sys/kernel/tracing, older ones only under debugfs
T=/sys/kernel/tracing
[ -d $T/events ] || T=/sys/kernel/debug/tracing
[ -d $T/events ] || { mount -t debugfs none /sys/kernel/debug 2>/dev/null; T=/sys/kernel/debug/tracing; }
if [ -d $T/events/i2c ]; then
  echo 0 > $T/tracing_on
  echo 16384 > $T/buffer_size_kb
  for e in i2c_write i2c_read i2c_reply i2c_result; do echo 1 > $T/events/i2c/$e/enable; done
  echo > $T/trace
  echo 1 > $T/tracing_on
  echo "TRACING for $SECS s: open the Camera app NOW, front camera, ~5 s preview, ~5 s video, one photo, close it"
  i=$SECS; while [ $i -gt 0 ]; do printf "\r%3d s " $i; sleep 1; i=$((i - 1)); done; echo
  echo 0 > $T/tracing_on
  cat $T/trace > $OUT/i2c-trace.txt
  for e in i2c_write i2c_read i2c_reply i2c_result; do echo 0 > $T/events/i2c/$e/enable; done
  echo "i2c trace: $(grep -c i2c_write $OUT/i2c-trace.txt) writes, $(grep -c 'a=010' $OUT/i2c-trace.txt) lines for address 0x10" | tee -a $INFO
else
  echo "no i2c trace events in this kernel ($T): needs CONFIG_FTRACE / CONFIG_EVENT_TRACING - skipped" | tee -a $INFO
  echo "still: open the Camera app now (front camera) for the logs below; $SECS s"
  sleep $SECS
fi

if [ -r /sys/kernel/debug/fimc-is/isfw-msg ]; then
  cat /sys/kernel/debug/fimc-is/isfw-msg > $OUT/isfw-msg.txt 2>/dev/null
  echo "isfw-msg: $(wc -l < $OUT/isfw-msg.txt) lines" | tee -a $INFO
else
  echo "no /sys/kernel/debug/fimc-is/isfw-msg (debugfs or the driver's debug off)" | tee -a $INFO
fi
dmesg | grep -i -E 'fimc|is-|cis|csi|3j1|sensor|ischain|mcsc|3aa|isp' > $OUT/dmesg-camera.txt
echo "dmesg: $(wc -l < $OUT/dmesg-camera.txt) camera lines" | tee -a $INFO
echo "done: adb pull $OUT"
