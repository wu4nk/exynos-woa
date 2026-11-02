#!/usr/bin/env bash
# pack-uefi-beyond1lte.sh — wrap BEYOND1LTE_UEFI.fd into a flashable BOOT image for the
# Galaxy S10 (beyond1lte, SM-G973F).
#
#   firmware/tools/pack-uefi-beyond1lte.sh <version> [DEBUG|RELEASE] [output.img]
#     e.g. pack-uefi-beyond1lte.sh 0.1.0
#
# Steps (the S10e recipe from pack-uefi-beyond0lte.ps1 + tasks/T09, ported to Linux):
#   1. assemble the 9820 shim (tools/beyond0lte-bootshim/bootshim.S) for the S10 board
#      (-DEXYNOS9820_BOARD_BEYOND1=1) at UEFI_BASE 0x90000000;
#   2. kernel = [shim][FD];
#   3. Android boot image header v2 with a real DTB section (build-bootimg-v2.py,
#      edk2-exynos s10.sh.inc values), DTB = Platform/Beyond1LtePkg/FdtBlob/beyond1lte.dtb.
# The version must match PcdFirmwareVersionString in Beyond1LtePkg.dsc; it goes into
# the boot image's board name and cmdline and into the output file name.
set -euo pipefail

VERSION="${1:?usage: $0 <version> [DEBUG|RELEASE] [output.img]}"
TARGET="${2:-DEBUG}"
WS="$(cd "$(dirname "$0")/.." && pwd)"
FV="$WS/edk2/Build/Beyond1LtePkg/${TARGET}_CLANGDWARF/FV"
FD="$FV/BEYOND1LTE_UEFI.fd"
DTB="$WS/Platform/Beyond1LtePkg/FdtBlob/beyond1lte.dtb"
OUT="${3:-$FV/beyond1lte-uefi-v${VERSION}.img}"
UEFI_BASE=0x90000000
BOOT_PARTITION_BYTES=$((14080 * 4096))   # sda14 boot: LBA 27264..41343 on the S10 (same as S10e)

[ -f "$FD" ]  || { echo "FD not found: $FD (run tools/build-uefi-linux.sh Beyond1LtePkg $TARGET)"; exit 1; }
[ -f "$DTB" ] || { echo "DTB not found: $DTB"; exit 1; }

DSC_VER=$(grep -o 'beyond1lte-uefi v[0-9.]*' "$WS/Platform/Beyond1LtePkg/Beyond1LtePkg.dsc" | head -1 | sed 's/.* v//')
[ "$DSC_VER" = "$VERSION" ] || { echo "version $VERSION != Beyond1LtePkg.dsc PcdFirmwareVersionString v$DSC_VER"; exit 1; }
python3 -c "import sys;d=open(sys.argv[1],'rb').read();sys.exit(0 if ('beyond1lte-uefi v'+sys.argv[2]).encode('utf-16le') in d else 1)" "$FD" "$VERSION" || \
  { echo "FD does not carry 'beyond1lte-uefi v$VERSION' - rebuild after bumping the PCD"; exit 1; }

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
FD_LEN=$(stat -c %s "$FD")
UEFI_SIZE=$(printf '0x%X' $(( (FD_LEN + 15) / 16 * 16 )))

clang -target aarch64-none-elf -ffreestanding -nostdlib -mgeneral-regs-only \
  -DUEFI_BASE=$UEFI_BASE -DUEFI_SIZE=$UEFI_SIZE -DEXYNOS9820_BOARD_BEYOND1=1 \
  -c "$WS/tools/beyond0lte-bootshim/bootshim.S" -o "$TMP/shim.o"
ld.lld -o "$TMP/shim.elf" "$TMP/shim.o" -Ttext=$UEFI_BASE -e _head --no-relax
llvm-objcopy -O binary "$TMP/shim.elf" "$TMP/shim.bin"

# Same sanity checks as pack-uefi-beyond0lte.ps1: entry is `adr x1,#imm` (imm != 0)
# and the ARM64 Image magic sits at +56.
python3 - "$TMP/shim.bin" <<'EOF'
import struct, sys
b = open(sys.argv[1], 'rb').read()
code0 = struct.unpack_from('<I', b, 0)[0]
assert (code0 & 0x9F00001F) == 0x10000001, f"shim code0 not adr x1 (0x{code0:08X})"
assert (code0 & 0x00FFFFE0) != 0, "shim adr x1 immediate is 0"
assert b[56:59] == b'ARM', "ARM64 header magic missing"
print(f"shim OK: {len(b)} B, code0=0x{code0:08X}")
EOF

cat "$TMP/shim.bin" "$FD" > "$TMP/kernel.bin"

python3 "$WS/tools/build-bootimg-v2.py" \
  --kernel "$TMP/kernel.bin" --dtb "$DTB" --output "$OUT" \
  --board "b1uefi-v$VERSION" \
  --cmdline "buildvariant=userdebug exynos-woa=beyond1lte-uefi-v$VERSION"

LEN=$(stat -c %s "$OUT")
[ "$LEN" -le "$BOOT_PARTITION_BYTES" ] || { echo "image $LEN B > BOOT $BOOT_PARTITION_BYTES B"; exit 1; }
echo "==> $OUT ($LEN B, fits BOOT $BOOT_PARTITION_BYTES B)"
sha256sum "$OUT"
