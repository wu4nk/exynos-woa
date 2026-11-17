#!/bin/sh
# build-win.sh - the Windows ARM64 test executables, cross-built with zig (mingw-w64 target, UCRT):
#   isphost-test.exe = isphost.c + plat_win.c + mmio_plain.c + test_front.c, x18test.exe = x18test.c
#   sh build-win.sh <out dir>          (ZIG=path overrides)
set -e
cd "$(dirname "$0")"
ZIG=${ZIG:-zig}
OUT=${1:-.}
mkdir -p "$OUT"
$ZIG cc -target aarch64-windows-gnu -O2 -fno-omit-frame-pointer -Wall -Wextra -Wshadow \
    -o "$OUT/isphost-test.exe" isphost.c plat_win.c mmio_plain.c test_front.c
$ZIG cc -target aarch64-windows-gnu -O2 -Wall -Wextra -o "$OUT/x18test.exe" x18test.c
rm -f "$OUT"/*.pdb
python3 -I check_tramp.py
