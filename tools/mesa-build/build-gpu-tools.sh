#!/usr/bin/env bash
# G2b: m5test.exe / d3dfill.exe (drivers/MaliG76Wddm/tools) for Windows ARM64, built on Linux with zig 0.13 like the
# UMD (linux-build-g76.sh). Needs the ddi/ dir that linux-build-g76.sh's cross stage made (d3dkmthk.h + compat).
# --image-base=0x140000000: zig's defaults are below 4 GB and Windows ARM64 refuses those (LoadLibrary/CreateProcess
# 193, notes/beyond1-port.md 15:44). d3dprobe is not built: it uses MSVC __try (no SEH in mingw C).
# Usage: build-gpu-tools.sh <workdir of linux-build-g76.sh> <out dir>      (ZIG=/path/to/zig)
set -euo pipefail
W=$(readlink -f "${1:?workdir}"); O=${2:?out dir}; mkdir -p "$O"
T=$(cd "$(dirname "$0")/../../drivers/MaliG76Wddm" && pwd)
CC="$W/tc/zig-cc"
F=(-O2 -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -Wl,--image-base=0x140000000 -include "$W/ddi/ddi_mingw_compat.h"
   -I"$W/ddi" -I"$T/include")
"$CC" "${F[@]}" -o "$O/m5test.exe" "$T/tools/m5test/m5test.c"
"$CC" "${F[@]}" -o "$O/d3dfill.exe" "$T/tools/d3dfill/d3dfill.c" -ld3d11 -ldxgi -luser32
# comprobe (C++: D2D / DComp headers): the D3D11 / DXGI / D2D / DirectComposition steps the XAML shell hosts make
"$W/tc/zig-c++" -O2 -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -Wl,--image-base=0x140000000 -o "$O/comprobe.exe" \
   "$T/tools/comprobe/comprobe.cpp" -ld3d11 -ldxgi -ld2d1 -ldcomp -luser32
# d2dtext (C++: D2D / DirectWrite): draws rectangles and text off-screen and reads them back (G2b-3 missing XAML text)
"$W/tc/zig-c++" -O2 -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -Wl,--image-base=0x140000000 -o "$O/d2dtext.exe" \
   "$T/tools/d2dtext/d2dtext.cpp" -ld3d11 -ldxgi -ld2d1 -ldwrite -luser32
rm -f "$O"/*.pdb
# gpubench (C++: D3D11 only): GPU job round trip, fill rate, vsync rate, per-processor speed (G2b-3 low frame rate)
"$W/tc/zig-c++" -O2 -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -Wl,--image-base=0x140000000 -o "$O/gpubench.exe" \
   "$T/tools/gpubench/gpubench.cpp" -ld3d11 -ldxgi -luser32
python3 -I "$(dirname "$0")/pe-set-subsys.py" "$O/m5test.exe" "$O/d3dfill.exe" "$O/comprobe.exe" "$O/d2dtext.exe" \
   "$O/gpubench.exe"
(cd "$O" && sha256sum m5test.exe d3dfill.exe comprobe.exe d2dtext.exe gpubench.exe > SHA256SUMS && cat SHA256SUMS)
