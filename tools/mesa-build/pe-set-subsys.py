#!/usr/bin/env python3
"""pe-set-subsys.py - set a PE image's OS and subsystem version (default 6.2) and refresh its checksum.

Why (G2, 2026-11-03): zig 0.13 (`zig cc -target aarch64-windows-gnu`) always writes OperatingSystemVersion and
SubsystemVersion 6.0 and ignores every linker option to change them. Windows does not load ARM/ARM64 images that
claim a subsystem older than 6.2 (Windows 8, the first Windows on ARM): LoadLibrary fails with 193
ERROR_BAD_EXE_FORMAT even for a one-function C DLL (umd-bisect t1-c..t4-synch on the S10, all 193). MSVC link.exe
and stock lld-link default ARM64 images to 6.02. Only these 8 header bytes (+ the checksum) change.

Usage: pe-set-subsys.py <file.dll|exe>... [--version MAJOR.MINOR]
"""
import struct
import sys


def pe_checksum(data: bytes, csum_off: int) -> int:
    total = 0
    n = len(data)
    padded = data + (b"\0" if n % 2 else b"")
    for i in range(0, len(padded), 2):
        if csum_off <= i < csum_off + 4:
            continue
        total += struct.unpack_from("<H", padded, i)[0]
        total = (total & 0xFFFF) + (total >> 16)
    total = (total & 0xFFFF) + (total >> 16)
    return (total + n) & 0xFFFFFFFF


def fix(path: str, major: int, minor: int) -> None:
    data = bytearray(open(path, "rb").read())
    if data[:2] != b"MZ":
        raise SystemExit(f"{path}: not MZ")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise SystemExit(f"{path}: no PE signature")
    machine = struct.unpack_from("<H", data, pe + 4)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    if magic not in (0x10B, 0x20B):
        raise SystemExit(f"{path}: optional header magic {magic:#x}")
    # Same offsets in PE32 and PE32+: OS version +40/+42, image version +44/+46, subsystem version +48/+50,
    # CheckSum +64.
    old = struct.unpack_from("<HHHH", data, opt + 40)
    old_sub = struct.unpack_from("<HH", data, opt + 48)
    struct.pack_into("<HH", data, opt + 40, major, minor)
    struct.pack_into("<HH", data, opt + 48, major, minor)
    struct.pack_into("<I", data, opt + 64, 0)
    struct.pack_into("<I", data, opt + 64, pe_checksum(bytes(data), opt + 64))
    open(path, "wb").write(data)
    print(f"{path}: machine {machine:#06x}  os {old[0]}.{old[1]} -> {major}.{minor}  "
          f"subsystem {old_sub[0]}.{old_sub[1]} -> {major}.{minor}  checksum {struct.unpack_from('<I', data, opt + 64)[0]:#010x}")


def main() -> None:
    args = sys.argv[1:]
    ver = "6.2"
    if "--version" in args:
        i = args.index("--version")
        ver = args[i + 1]
        del args[i:i + 2]
    if not args:
        raise SystemExit(__doc__)
    major, minor = (int(x) for x in ver.split("."))
    for p in args:
        fix(p, major, minor)


if __name__ == "__main__":
    main()
