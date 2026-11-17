#!/bin/sh
# build.sh - test_front for AArch64 Linux (static musl): runs under qemu-aarch64 on the build machine and as-is on
# the phone's Android (Termux / adb shell). Needs zig (pip ziglang); ZIG=path overrides.
#   sh build.sh [out file]
set -e
cd "$(dirname "$0")"
ZIG=${ZIG:-zig}
OUT=${1:-test_front}
$ZIG cc -target aarch64-linux-musl -O2 -g -static -fno-omit-frame-pointer -Wall -Wextra -Wshadow \
    -o "$OUT" isphost.c plat_posix.c mmio_trap.c test_front.c
python3 -I check_tramp.py
