#!/system/bin/sh
# android-gpu-info.sh (v1.0) - exynos-woa S10: collect what Android knows about the Mali-G76's clock, voltage and
# thermal tables, so the Windows side (UEFI G3D clock, now fixed at 260 MHz) can be raised with real numbers.
# Run as root on Android (LineageOS / stock), e.g. in Termux:   su -c sh /sdcard/android-gpu-info.sh
# Result: /sdcard/gpu-info.txt  (send that file). Reads only; changes nothing.
O=/sdcard/gpu-info.txt
{
echo "android-gpu-info v1.0  $(date)"
echo "== build"; getprop ro.product.device; getprop ro.build.fingerprint; uname -a
echo "== mali device nodes"
for d in /sys/devices/platform/*mali* /sys/kernel/gpu /sys/class/misc/mali0/device /sys/devices/platform/*g3d*; do
  [ -d "$d" ] || continue
  echo "--- $d"; ls -la "$d" 2>/dev/null
  for f in "$d"/*; do
    [ -f "$f" ] || continue
    case "$f" in *trace*|*mem_pool*|*uevent) continue;; esac
    echo "== $f"; head -c 6000 "$f" 2>/dev/null; echo
  done
done
echo "== devfreq"
for d in /sys/class/devfreq/*; do
  echo "--- $d"
  for f in cur_freq min_freq max_freq available_frequencies available_governors governor trans_stat; do
    [ -f "$d/$f" ] && { echo "== $f"; head -c 4000 "$d/$f"; echo; }
  done
done
echo "== cpufreq (for comparison with gpubench's CPU numbers)"
for p in /sys/devices/system/cpu/cpufreq/policy*; do
  echo "--- $p: related $(cat $p/related_cpus 2>/dev/null) cur $(cat $p/scaling_cur_freq 2>/dev/null) min $(cat $p/cpuinfo_min_freq 2>/dev/null) max $(cat $p/cpuinfo_max_freq 2>/dev/null)"
  cat $p/scaling_available_frequencies 2>/dev/null
done
echo "== thermal zones"
for z in /sys/class/thermal/thermal_zone*; do echo "$(cat $z/type 2>/dev/null) $(cat $z/temp 2>/dev/null)"; done
echo "== ECT / ASV / cal-if debugfs (needs debugfs mounted)"
mount | grep -q debugfs || mount -t debugfs none /sys/kernel/debug 2>/dev/null
for f in /sys/kernel/debug/ect/* /sys/kernel/debug/asv/* /sys/kernel/debug/cal-if/* /sys/kernel/debug/exynos-asv/*; do
  [ -f "$f" ] && { echo "== $f"; head -c 20000 "$f" 2>/dev/null; echo; }
done
echo "== regulators with g3d / gpu in the name"
for r in /sys/class/regulator/regulator.*; do
  n=$(cat $r/name 2>/dev/null)
  case "$n" in *[gG]3[dD]*|*gpu*|*GPU*|*BUCK*|*buck*) echo "$n: $(cat $r/microvolts 2>/dev/null) uV, min $(cat $r/min_microvolts 2>/dev/null) max $(cat $r/max_microvolts 2>/dev/null), state $(cat $r/state 2>/dev/null)";; esac
done
echo "== clk summary lines for g3d / gpu"
grep -i -E "g3d|gpu|mali" /sys/kernel/debug/clk/clk_summary 2>/dev/null | head -60
echo "== dmesg (mali / g3d / dvfs / asv / acpm)"
dmesg 2>/dev/null | grep -i -E "mali|g3d|dvfs|asv|acpm|ect" | head -300
} > "$O" 2>&1
echo "done: $O ($(wc -c < "$O") bytes)"
