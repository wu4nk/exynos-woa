#!/usr/bin/env bash
# S10 GPU validation tools (tools/s10-kit/gpu-validate.ps1 -> kit tools\validate\): upstream's D3D11 / GL / timing test
# programs (drivers/MaliG76Wddm/tools) built on Linux with zig 0.13 like the UMD (linux-build-g76.sh cross stage).
# d3dcompiler_47 import libs come from zig's mingw .def files. Not built: d3dtri (needs its overlay source),
# gpucheck (GUI WinMain), d3dswap (MSVC-only goto over initialisation).
# Usage: build-validate-tools.sh <workdir of linux-build-g76.sh> <out dir>      (ZIG=/path/to/zig)
set -euo pipefail
W=$(readlink -f "${1:?workdir}"); O=$(readlink -f "${2:?out dir}"); mkdir -p "$O/x86" "$O/lib" "$O/lib86"
T=$(cd "$(dirname "$0")/../../drivers/MaliG76Wddm" && pwd)
M=$(dirname "${ZIG:?set ZIG}")/lib/libc/mingw
"$W/tc/zig-dlltool" -m arm64 -d "$M/lib-common/d3dcompiler_47.def" -l "$O/lib/libd3dcompiler.a"
"$W/tc/zig-dlltool" -m i386 -d "$M/lib32/d3dcompiler_47.def" -l "$O/lib86/libd3dcompiler.a"
F=(-O2 -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -include "$W/ddi/ddi_mingw_compat.h" -I"$W/ddi" -I"$T/include")
L=(-ld3dcompiler -ld3d11 -ldxgi -luser32 -lgdi32)
src() { (shopt -s nullglob; f=("$T/tools/$1"/*.c "$T/tools/$1"/*.cpp); echo "${f[0]}"); }
cc_for() { case "$1" in *.cpp) echo "$2-c++" ;; *) echo "$2-cc" ;; esac; }
for d in d3dcaps d3dmsaa d3dcs d3dsm41 d3dsm5 d3dgs d3dso d3dtess d3dmrt d3dbc d3dcopysrgb d3dcbdyn d3dvbring d3dfence \
         d3dfs gltest gpuinfo dwmrate vblank flipseq gpuclock; do
  f=$(src $d); extra=$(grep -ohE '\b[a-z0-9_]+\.lib\b' "$T/tools/$d"/*.cmd 2>/dev/null | sed 's/\.lib//' | sort -u |
                       grep -vE '^(ntoskrnl|hal|d3dcompiler)$' | sed 's/^/-l/' | tr '\n' ' ' || true)
  $(cc_for "$f" "$W/tc/zig") "${F[@]}" -Wl,--image-base=0x140000000 -L"$O/lib" -o "$O/$d.exe" "$f" $extra "${L[@]}"
done
for d in d3dcaps d3dmsaa d3dcs d3dcbdyn d3dvbring; do
  f=$(src $d)
  $(cc_for "$f" "$W/tc/zig86") "${F[@]}" -Wl,--image-base=0x10000000 -L"$O/lib86" -o "$O/x86/${d}32.exe" "$f" "${L[@]}"
done
rm -rf "$O/lib" "$O/lib86" "$O"/*.pdb "$O"/x86/*.pdb
python3 -I "$(dirname "$0")/pe-set-subsys.py" "$O"/*.exe
(cd "$O" && sha256sum *.exe x86/*.exe > SHA256SUMS && cat SHA256SUMS)
