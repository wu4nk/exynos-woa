#!/system/bin/sh
# get-wifi-fw.sh (v1.1) - run ON THE PHONE by get-wifi-fw.cmd (TWRP: as root; Android: through su).
# Collects the BCM4375 WiFi blobs into a world-readable folder and prints that folder as the last
# line. Read-only on every partition.
#
# Where the S10's blobs live (LineageOS android_device_samsung_beyond1lte proprietary-files.txt and
# exynos9820-common: vendor/firmware/bcmdhd_sta.bin_b1, bcmdhd_clm.blob, nvram.txt_*; Samsung stock
# keeps the same names under /vendor/etc/wifi or /system/etc/wifi on some firmwares).
# v1.1: TWRP's own ramdisk can carry an unrelated /vendor/firmware, so the REAL vendor (and system)
# partitions are always mounted read-only on a scratch dir as well, and every candidate dir is searched.
if [ -n "$(getprop ro.twrp.version)" ] || [ ! -d /data/local/tmp ]; then OUT=/tmp/bcm4375; else OUT=/data/local/tmp/bcm4375; fi
rm -rf "$OUT"; mkdir -p "$OUT"

MNTS=""
try_mount() {   # $1 = partition name, $2 = scratch dir
  [ -e "/dev/block/by-name/$1" ] || return
  mkdir -p "$2"
  if mount -o ro "/dev/block/by-name/$1" "$2" 2>/dev/null; then MNTS="$MNTS $2"; fi
}
try_mount vendor /tmp/s10fw_vendor
try_mount system /tmp/s10fw_system

for d in /vendor/firmware /vendor/etc/wifi /system/etc/wifi /system/vendor/firmware /system/vendor/etc/wifi \
         /system_root/system/etc/wifi /odm/firmware /odm/etc/wifi \
         /tmp/s10fw_vendor/firmware /tmp/s10fw_vendor/etc/wifi \
         /tmp/s10fw_system/etc/wifi /tmp/s10fw_system/system/etc/wifi \
         /tmp/s10fw_system/vendor/firmware /tmp/s10fw_system/system/vendor/firmware; do
  [ -d "$d" ] || continue
  for f in "$d"/bcmdhd_* "$d"/nvram.txt_* "$d"/nvram_net.txt*; do
    [ -f "$f" ] || continue
    [ -f "$OUT/$(basename "$f")" ] || { cp -f "$f" "$OUT"/ && echo "from $d: $(basename "$f")" 1>&2; }
  done
done

for m in $MNTS; do umount "$m" 2>/dev/null; done
chmod 755 "$OUT"; chmod 644 "$OUT"/* 2>/dev/null
ls -l "$OUT" 1>&2
echo "$OUT"
