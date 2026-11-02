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
cp "$KIT"/{README.md,build-s10.cmd,make-windows.ps1,set-esp-type.ps1,z3s-disk.cs,new-cert.ps1,get-wifi-fw.cmd,get-wifi-fw.sh,install-in-windows.ps1} "$D/"
mkdir -p "$D/setup" && cp "$KIT/setup/SetupComplete.cmd" "$D/setup/"
cp "$UEFI" "$D/"
for d in Exynos9820Hsi2c Exynos9820Ufs SecTsY761Touch S10eBattery Exynos9820Wlan Exynos9820WlanCx Exynos9820Audio S10eDiag include; do
  (cd "$REPO/drivers" && git ls-files -z "$d" | xargs -0 -I{} cp --parents {} "$D/")
done
echo "s10-woa-kit v$VER  repo $(git -C "$REPO" rev-parse --short HEAD)  uefi $(basename "$UEFI")" > "$D/VERSION.txt"
(cd "$TMP" && zip -qr "$OUTDIR/s10-woa-kit-v$VER.zip" "s10-woa-kit-v$VER")
echo "$OUTDIR/s10-woa-kit-v$VER.zip"
