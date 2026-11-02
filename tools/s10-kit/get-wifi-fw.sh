#!/system/bin/sh
# get-wifi-fw.sh - run ON THE PHONE by get-wifi-fw.cmd (TWRP: as root; Android: through su).
# Collects the BCM4375 WiFi blobs from the vendor partition into a world-readable folder and
# prints that folder as the last line. Read-only on every partition.
if [ -n "$(getprop ro.twrp.version)" ] || [ ! -d /data/local/tmp ]; then OUT=/tmp/bcm4375; else OUT=/data/local/tmp/bcm4375; fi
rm -rf "$OUT"; mkdir -p "$OUT"
MNT=""
if [ ! -d /vendor/firmware ]; then
  mkdir -p /tmp/s10v && mount -o ro /dev/block/by-name/vendor /tmp/s10v 2>/dev/null && MNT=/tmp/s10v
fi
for d in /vendor/firmware /vendor/etc/wifi $MNT/firmware $MNT/etc/wifi; do
  [ -d "$d" ] || continue
  for f in "$d"/bcmdhd_* "$d"/nvram.txt_*; do [ -f "$f" ] && cp -f "$f" "$OUT"/; done
done
[ -n "$MNT" ] && umount "$MNT" 2>/dev/null
chmod 755 "$OUT"; chmod 644 "$OUT"/* 2>/dev/null
ls -l "$OUT" 1>&2
echo "$OUT"
