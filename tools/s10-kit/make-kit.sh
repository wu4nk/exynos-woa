#!/usr/bin/env bash
# make-kit.sh — assemble the Galaxy S10 Windows kit zip (z3s-kit layout).
#   tools/s10-kit/make-kit.sh <kit-version> <uefi.img> [out-dir]
# Contents: this folder's scripts + the three driver source trees (+ drivers/include)
# + the UEFI image. Driver binaries are NOT included: build-s10.cmd builds them in EWDK.
set -euo pipefail
VER="${1:?usage: $0 <kit-version> <uefi.img> [out-dir]}"
UEFI="${2:?usage: $0 <kit-version> <uefi.img> [out-dir]}"
OUTDIR="${3:-$PWD}"
KIT="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$KIT/../.." && pwd)"
case "$(basename "$UEFI")" in beyond1lte-uefi-v*.img) ;; *) echo "UEFI must be named beyond1lte-uefi-v*.img"; exit 1;; esac
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
D="$TMP/s10-woa-kit-v$VER"
mkdir -p "$D"
cp "$KIT"/{README.md,build-s10.cmd,make-windows.ps1,set-esp-type.ps1,z3s-disk.cs,new-cert.ps1,get-wifi-fw.cmd,get-wifi-fw.sh,install-in-windows.ps1,update-ufs.ps1,collect-logs.ps1,push-to-windows.cmd,get-logs-twrp.cmd,s10-log.cmd,kit-watch.ps1,auto-install.ps1,gpu-s10.ps1,umd-test.ps1,gpu-watch.ps1,gpu-fill.ps1,twrp-log.sh,s10-wddm-inf.ps1} "$D/"
cp "$KIT/../S10eSpeakerTrim/S10eSpeakerTrim.cs" "$D/"
mkdir -p "$D/setup" && cp "$KIT/setup/SetupComplete.cmd" "$KIT/setup/S10-install-once.cmd" "$D/setup/"
cp "$UEFI" "$D/"
for d in Exynos9820Hsi2c Exynos9820Ufs S10Touch S10eBattery Exynos9820Wlan Exynos9820WlanCx Exynos9820Audio Exynos9820Bt S10eDiag MaliG76Wddm MaliG76Wddm-legacy MaliG76Probe include; do
  (cd "$REPO/drivers" && git ls-files -z "$d" | xargs -0 -I{} cp --parents {} "$D/")
done
# v2.0: the Linux-built user-mode driver (tools/mesa-build/linux-build-g76.sh collect) goes into umd\ when
# UMD_DIR points at its out-umd folder; build-s10.cmd puts it into the MaliG76Wddm package.
# v2.2: UMD_DIR may list several out folders (out-umd out-x86 out-gl, colon separated): every maliumd.dll /
# maliumd32.dll / maligl.dll found goes in, with one SHA256SUMS for all.
if [ -n "${UMD_DIR:-}" ]; then
  mkdir -p "$D/umd"
  IFS=: read -ra UDIRS <<< "$UMD_DIR"
  for u in "${UDIRS[@]}"; do for f in maliumd.dll maliumd32.dll maligl.dll; do [ -f "$u/$f" ] && cp "$u/$f" "$D/umd/"; done; done
  ( cd "$D/umd" && sha256sum *.dll > SHA256SUMS )
fi
# v2.4: the legacy GPU stack's UMD (0075) when UMD_LEGACY points at its folder (maliumd.dll + SHA256SUMS)
if [ -n "${UMD_LEGACY:-}" ]; then mkdir -p "$D/umd-legacy" && cp "$UMD_LEGACY/maliumd.dll" "$UMD_LEGACY/SHA256SUMS" "$D/umd-legacy/"; fi
echo "s10-woa-kit v$VER  repo $(git -C "$REPO" rev-parse --short HEAD)  uefi $(basename "$UEFI")" > "$D/VERSION.txt"
(cd "$TMP" && zip -qr "$OUTDIR/s10-woa-kit-v$VER.zip" "s10-woa-kit-v$VER")
echo "$OUTDIR/s10-woa-kit-v$VER.zip"
