#!/usr/bin/env bash
# G2 (S10) - Mesa panfrost + d3d10umd UMD (maliumd.dll, Windows ARM64) built entirely on Linux, no MSVC.
# Same inputs as build-mesa-g76.ps1 (Mesa 26.2.4 @9a0169f, wddm-mali-g52 @16f7e7c6 0001-0019 + shim, our 0020+),
# different toolchain:
#   libpan   wsl-libpan.sh (conda-forge LLVM 18), unchanged
#   host     panfrost_compile for Linux x86_64, clang 18, -mms-bitfields
#   cross    zig 0.13 cc/c++ -target aarch64-windows-gnu (mingw-w64 + libc++, static), -mms-bitfields
#   ddi      the WDK DDI headers (Mesa include/winddk/README.txt list) from the user's EWDK
#            (get-ddi-headers.ps1 -> ddi-headers.zip), the only Microsoft input
# ABI (notes/gpu-m16-g12-mesa.md step 9): panfrost_compile memcpys struct bifrost_precompiled_kernel_info into the
# generated C, so host and target must lay structs out alike. Checked with -fdump-record-layouts-simple on the real
# compile commands (host pan_compile.c vs target pan_precomp.c): 240 common records, only struct stat and
# _util_cpu_caps_state_t differ (libc / per-platform, never serialized). maliwddm_abi.h / malikm_ioctl.h (shared
# with the MSVC-built KMD) use fixed-width integers only. -mms-bitfields = MSVC bitfield layout for the DDI structs.
#
# Usage: linux-build-g76.sh <workdir> <ddi-headers.zip|dir> [stage...]   stages: source libpan host cross collect (default all)
# Needs: git, python3, clang-18, a zig 0.13 tree (ZIG=/path/to/zig), network to github.com + conda.anaconda.org.
set -euo pipefail
W=$(readlink -f "${1:?workdir}"); DDI_IN=${2:?ddi-headers.zip or dir}; shift 2
STAGES=${*:-source libpan host cross collect}
HERE=$(cd "$(dirname "$0")" && pwd)
ZIG=${ZIG:?set ZIG to the zig 0.13 binary}
MESA_URL=https://github.com/chaotic-cx/mesa-mirror
MESA_COMMIT=9a0169f2a72d8d844124801b7abbdc75cc37f874     # branch 26.2, VERSION 26.2.4
REF_URL=${REF_URL_OVERRIDE:-https://github.com/gahingwoo/wddm-mali-g52}
REF_COMMIT=16f7e7c6
ENV=$HOME/mali-g12/env
want() { [[ " $STAGES " == *" $1 "* ]]; }
mkdir -p "$W"; cd "$W"

if want source; then
  [ -d mesa-26.2 ] || { git init -q mesa-26.2; git -C mesa-26.2 remote add origin $MESA_URL; }
  git -C mesa-26.2 fetch -q --depth 400 origin 26.2
  git -C mesa-26.2 -c advice.detachedHead=false checkout -q $MESA_COMMIT
  [ -d wddm-mali-g52 ] || git clone -q "$REF_URL" wddm-mali-g52
  rm -rf mesa-26.2-g76 ref-pin; mkdir ref-pin
  git clone -q -c core.autocrlf=false --no-local mesa-26.2 mesa-26.2-g76
  git -C wddm-mali-g52 archive --format=tar $REF_COMMIT ci win32-compat include mesa-patches | tar -x -C ref-pin
  M=$W/mesa-26.2-g76
  cp -r ref-pin/win32-compat $M/include/win32-compat
  cp ref-pin/include/malikm_ioctl.h ref-pin/include/maliwddm_abi.h $M/include/win32-compat/
  for p in ref-pin/mesa-patches/*.patch; do tr -d '\r' < "$p" > "$W/p.lf"; git -C $M apply --whitespace=nowarn "$W/p.lf"; echo "applied $(basename $p)"; done
  git -C $M add -A; git -C $M -c user.name=g12-build -c user.email=g12@local commit -qm 'wddm-mali-g52 0001-0019 + shim'
  for p in "$HERE"/patches/*.patch; do
    git -C $M apply --whitespace=nowarn "$p"; git -C $M add -A
    git -C $M -c user.name=exynos-woa -c user.email=build@local commit -qm "exynos-woa $(basename $p)"; echo "applied $(basename $p)"
  done
  chmod +x ref-pin/ci/clc-stubs/*
fi

if want libpan; then
  bash "$HERE/wsl-libpan.sh" "$W/mesa-26.2-g76" "$W/libpan-prebuilt"
fi

if want host; then
  ( export PATH=$W/ref-pin/ci/clc-stubs:$ENV/bin:$PATH PKG_CONFIG_PATH=$ENV/lib/pkgconfig:$ENV/share/pkgconfig \
           LIBPAN_PREBUILT=$W/libpan-prebuilt CC=clang-18 CXX=clang++-18
    rm -rf build-host
    meson setup build-host mesa-26.2-g76 -Dbuildtype=release -Dgallium-drivers=panfrost -Dvulkan-drivers= -Dplatforms= \
      -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dllvm=disabled -Dmesa-clc=system -Dprecomp-compiler=enabled \
      -Dvideo-codecs= -Dbuild-tests=false -Dzstd=disabled -Dshader-cache=disabled \
      "-Dc_args=-mms-bitfields -DHAVE_FUNC_ATTRIBUTE_PACKED" "-Dcpp_args=-mms-bitfields -DHAVE_FUNC_ATTRIBUTE_PACKED" > host-setup.log
    ninja -C build-host src/panfrost/clc/panfrost_compile > host-ninja.log )
  echo "host: $(ls -la build-host/src/panfrost/clc/panfrost_compile)"
fi

if want cross; then
  TC=$W/tc; mkdir -p $TC xlib ddi
  for t in cc c++; do printf '#!/bin/sh\nexec %s %s -target aarch64-windows-gnu "$@"\n' "$ZIG" $t > $TC/zig-$t; done
  for t in ar rc dlltool; do printf '#!/bin/sh\nexec %s %s "$@"\n' "$ZIG" $t > $TC/zig-$t; done
  chmod +x $TC/zig-*
  # DDI headers: only the ones mingw-w64 lacks (Mesa include/winddk/README.txt), flat in one -I dir. The WDK's
  # sal.h / d3d11.h / dxgi*.h stay out: mingw's own are used. Compress-Archive (PowerShell 5.1) stores names
  # with backslashes ("um\d3d10umddi.h"), so match on the basename.
  if [ -f "$DDI_IN" ]; then rm -rf ddi-zip; mkdir ddi-zip; (cd ddi-zip && python3 -I -m zipfile -e "$DDI_IN" .); DDI_SRC=ddi-zip; else DDI_SRC=$DDI_IN; fi
  rm -rf ddi; mkdir ddi
  for h in d3d10tokenizedprogramformat.hpp d3d11tokenizedprogramformat.hpp d3d10umddi.h d3dkmddi.h d3dkmdt.h \
           d3dkmthk.h d3dukmdt.h d3dumddi.h dxgiddi.h dxmini.h wmidata.h; do
    f=$(find "$DDI_SRC" -type f | awk -v h="$h" '{n=$0; gsub(/.*[\/\\]/, "", n); if (tolower(n) == h) {print; exit}}')
    [ -n "$f" ] || { echo "ddi: missing $h"; exit 1; }
    cp "$f" ddi/$h
  done
  # d3d10umddi.h includes them with this case (Windows is case-insensitive, Linux is not).
  ln -s d3d10tokenizedprogramformat.hpp ddi/d3d10TokenizedProgramFormat.hpp
  ln -s d3d11tokenizedprogramformat.hpp ddi/d3d11TokenizedProgramFormat.hpp
  # SAL annotation the WDK 10.0.28000 d3dkmddi.h uses that mingw-w64's sal.h lacks.
  printf '/* exynos-woa G2: SAL the WDK DDI headers use that mingw-w64 lacks (no-ops). */\n#pragma once\n#ifndef _Maybenull_\n#define _Maybenull_\n#endif\n' > ddi/ddi_mingw_compat.h
  echo "ddi: $(ls ddi | wc -l) files"
  # mingw has no libsynchronization.a (MSVC name): import lib from api-ms-win-core-synch-l1-2-0.
  ZLIB_DEF=$(dirname "$ZIG")/lib/libc/mingw/lib-common/api-ms-win-core-synch-l1-2-0.def
  $TC/zig-dlltool -m arm64 -d "$ZLIB_DEF" -l xlib/libsynchronization.a
  # zlib (wrapdb is not reachable): static, from github madler/zlib.
  [ -d zlib-src ] || git -c advice.detachedHead=false clone -q --depth 1 --branch v1.3.1 https://github.com/madler/zlib zlib-src
  Z=$W/zlib-arm64; rm -rf $Z; mkdir -p $Z/lib/pkgconfig $Z/include zobj
  for f in adler32 compress crc32 deflate gzclose gzlib gzread gzwrite infback inffast inflate inftrees trees uncompr zutil; do
    $TC/zig-cc -O2 -mms-bitfields -c zlib-src/$f.c -o zobj/$f.o; done
  $TC/zig-ar rcs $Z/lib/libz.a zobj/*.o; cp zlib-src/zlib.h zlib-src/zconf.h $Z/include/
  printf 'prefix=%s\nlibdir=${prefix}/lib\nincludedir=${prefix}/include\nName: zlib\nDescription: zlib\nVersion: 1.3.1\nLibs: -L${libdir} -lz\nCflags: -I${includedir}\n' $Z > $Z/lib/pkgconfig/zlib.pc
  cat > $TC/arm64-mingw-zig.cross <<EOF
[binaries]
c = '$TC/zig-cc'
cpp = '$TC/zig-c++'
ar = '$TC/zig-ar'
windres = '$TC/zig-rc'
dlltool = '$TC/zig-dlltool'
pkg-config = '$ENV/bin/pkg-config'

[properties]
pkg_config_libdir = ['$Z/lib/pkgconfig']

[built-in options]
c_args = ['-include', '$W/ddi/ddi_mingw_compat.h', '-Wno-error=missing-prototypes', '-mms-bitfields', '-DHAVE_FUNC_ATTRIBUTE_PACKED', '-I$W/ddi']
cpp_args = ['-include', '$W/ddi/ddi_mingw_compat.h', '-Wno-error=missing-prototypes', '-mms-bitfields', '-DHAVE_FUNC_ATTRIBUTE_PACKED', '-I$W/ddi']
c_link_args = ['-L$W/xlib']
cpp_link_args = ['-L$W/xlib']

[host_machine]
system = 'windows'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
EOF
  ( export PATH=$W/build-host/src/panfrost/clc:$W/ref-pin/ci/clc-stubs:$ENV/bin:$PATH LIBPAN_PREBUILT=$W/libpan-prebuilt
    unset CC CXX PKG_CONFIG_PATH
    rm -rf build-arm64
    meson setup build-arm64 mesa-26.2-g76 --backend=ninja --cross-file $TC/arm64-mingw-zig.cross -Dbuildtype=debugoptimized \
      -Dprecomp-compiler=system -Dplatforms=windows -Dgallium-drivers=softpipe,panfrost -Dgallium-d3d10umd=true \
      -Dmesa-clc=system -Dvulkan-drivers= -Dllvm=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled \
      -Degl=disabled -Dglx=disabled -Dgbm=disabled -Dvideo-codecs= -Dmicrosoft-clc=disabled -Dspirv-to-dxil=false \
      -Dzstd=disabled -Dshader-cache=disabled -Dbuild-tests=false > cross-setup.log
    ninja -C build-arm64 -k 0 > cross-ninja.log 2>&1 || { grep -E 'error:|FAILED' cross-ninja.log | head -40; exit 1; } )
fi

if want collect; then
  D=build-arm64/src/gallium/targets/d3d10umd/libgallium_d3d10.dll
  O=$W/out-umd; rm -rf $O; mkdir -p $O
  cp $D $O/libgallium_d3d10.dll; cp $D $O/maliumd.dll
  # zig writes OS/subsystem version 6.0; Windows refuses ARM64 images below 6.2 (LoadLibrary 193). Fix the header.
  python3 -I "$HERE/pe-set-subsys.py" $O/libgallium_d3d10.dll $O/maliumd.dll
  ( cd $O; sha256sum maliumd.dll > SHA256SUMS )
  llvm-readobj-18 --coff-imports $D | grep -E 'Name:' | sort -u > $O/imports.txt || true
  llvm-readobj-18 --coff-exports $D | grep -E 'Name:' > $O/exports.txt || true
  file $D; cat $O/SHA256SUMS; echo "imports:"; cat $O/imports.txt
fi
