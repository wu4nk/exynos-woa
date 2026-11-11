#!/usr/bin/env bash
# V1 (S10) - panvk, Mesa's Vulkan driver for Mali, as a Windows ARM64 Vulkan ICD (vulkan_panfrost.dll), built on Linux.
#
# Source: the zenithblue-oss/panvk-kbase-android patch stack (Mesa PanVK 26.3.0-devel, git-795ca7ff3e, p0175 -
# 41/41 panprobe on this phone under Android) = Mesa main 5a07217f + its series (scripts/prepare-rc5-tree.sh), then
# ours (tools/mesa-build/panvk/patches, git am):
#   0001 include/win32-compat (the malikm shim, as in the 26.2 UMD tree at 0073)
#   0002 wddm-mali-g52 0003-0011/0015/0016 portability, adapted to 26.3-devel
#   0003 panvk over the shim: drmDevice enumeration ("malikm"), malikm_atoms.c (the kbase job atoms panvk's JM queue
#        uses, done with PANFROST_SUBMIT + binary syncobjs), the CPU atom sync type on _WIN32, ICD exports through
#        vulkan_api.def, Windows build fixes
# Toolchain as linux-build-g76.sh (zig 0.13 aarch64-windows-gnu, -mms-bitfields; the cross file, xlib and ddi of that
# script's work dir are reused: run linux-build-g76.sh's cross stage once first, its dir is G76_W).
#
# Usage: linux-build-panvk.sh <workdir> [stage...]   stages: source clc host cross collect (default all)
# Needs: ZIG, G76_W (linux-build-g76.sh work dir), ~/mali-g12/env (wsl-libpan.sh's conda env: LLVM 18, spirv tools).
set -euo pipefail
W=$(readlink -f "${1:?workdir}"); shift
STAGES=${*:-source clc host cross collect}
HERE=$(cd "$(dirname "$0")" && pwd)
ZIG=${ZIG:?set ZIG to the zig 0.13 binary}
G76_W=${G76_W:?set G76_W to linux-build-g76.sh work dir}
ENV=$HOME/mali-g12/env
MESA_URL=https://gitlab.freedesktop.org/mesa/mesa.git
MESA_COMMIT=5a07217f034b3e50d8c7c7794f97a2df1742613b
PV_URL=https://github.com/zenithblue-oss/panvk-kbase-android
PV_COMMIT=cf70e02b918e1864cbcd746a7fefa57b96c673ce
STUBS=$HERE/panvk/clc-stubs
want() { [[ " $STAGES " == *" $1 "* ]]; }
mkdir -p "$W"; cd "$W"

if want source; then
  [ -d panvk-kbase ] || git clone -q "$PV_URL" panvk-kbase
  git -C panvk-kbase -c advice.detachedHead=false checkout -q $PV_COMMIT
  rm -rf mesa; git init -q mesa
  git -C mesa fetch -q --depth 1 $MESA_URL $MESA_COMMIT
  git -C mesa -c advice.detachedHead=false checkout -q FETCH_HEAD
  git -C mesa checkout -q -b p0175
  git -C mesa config user.name exynos-woa; git -C mesa config user.email build@local
  sh panvk-kbase/scripts/prepare-rc5-tree.sh --mesa "$W/mesa"
  git -C mesa tag -f base-p0175 >/dev/null
  git -C mesa checkout -q -b win
  git -C mesa am -q "$HERE"/panvk/patches/*.patch
  echo "source: $(git -C mesa log --oneline -1)"
fi

if want clc; then
  # libpan_v<N> and libpoly SPIR-V + bindings (platform independent), made here with the real mesa_clc.
  ( export PATH=$ENV/bin:$PATH PKG_CONFIG_PATH=$ENV/lib/pkgconfig:$ENV/share/pkgconfig LD_LIBRARY_PATH=$ENV/lib CC=gcc CXX=g++
    rm -rf build-clc
    meson setup build-clc mesa -Dbuildtype=release -Dgallium-drivers= -Dvulkan-drivers=panfrost -Dpanfrost-kmds=panfrost \
      -Dplatforms= -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dllvm=enabled -Dmesa-clc=enabled \
      -Dprecomp-compiler=enabled -Dvideo-codecs= -Dbuild-tests=false > clc-setup.log
    t=""; for v in 4 5 6 7 9 10 11 12 13 14; do
      t="$t src/panfrost/libpan/libpan_v$v.spv src/panfrost/libpan/libpan_v$v.cpp src/panfrost/libpan/libpan_v$v.h"; done
    ninja -C build-clc $t src/poly/cl/libpoly.spv src/poly/cl/libpoly.cpp src/poly/cl/libpoly.h > clc-ninja.log )
  rm -rf libpan-prebuilt; mkdir libpan-prebuilt
  cp build-clc/src/panfrost/libpan/libpan_v*.{spv,cpp,h} build-clc/src/poly/cl/libpoly.{spv,cpp,h} libpan-prebuilt/
  echo "clc: $(ls libpan-prebuilt | wc -l) files"
fi

if want host; then
  ( export PATH=$STUBS:$ENV/bin:$PATH PKG_CONFIG_PATH=$ENV/lib/pkgconfig:$ENV/share/pkgconfig \
           LIBPAN_PREBUILT=$W/libpan-prebuilt CC=clang-18 CXX=clang++-18
    rm -rf build-host
    meson setup build-host mesa -Dbuildtype=release -Dgallium-drivers=panfrost -Dvulkan-drivers= -Dpanfrost-kmds=panfrost \
      -Dplatforms= -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dllvm=disabled -Dmesa-clc=system \
      -Dprecomp-compiler=enabled -Dvideo-codecs= -Dbuild-tests=false -Dzstd=disabled -Dshader-cache=disabled \
      "-Dc_args=-mms-bitfields -DHAVE_FUNC_ATTRIBUTE_PACKED" "-Dcpp_args=-mms-bitfields -DHAVE_FUNC_ATTRIBUTE_PACKED" > host-setup.log
    ninja -C build-host src/panfrost/clc/panfrost_compile > host-ninja.log )
  echo "host: $(ls -la build-host/src/panfrost/clc/panfrost_compile)"
fi

if want cross; then
  cp "$G76_W/tc/arm64-mingw-zig.cross" cross.txt
  ( export PATH=$W/build-host/src/panfrost/clc:$STUBS:$ENV/bin:$PATH LIBPAN_PREBUILT=$W/libpan-prebuilt
    unset CC CXX PKG_CONFIG_PATH
    rm -rf build-arm64
    meson setup build-arm64 mesa --backend=ninja --cross-file cross.txt -Dbuildtype=release -Db_ndebug=true \
      -Dprecomp-compiler=system -Dplatforms=windows -Dgallium-drivers= -Dvulkan-drivers=panfrost -Dpanfrost-kmds=panfrost \
      -Dmesa-clc=system -Dllvm=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Degl=disabled -Dglx=disabled \
      -Dgbm=disabled -Dvideo-codecs= -Dmicrosoft-clc=disabled -Dspirv-to-dxil=false -Dspirv-tools=disabled \
      -Dzstd=disabled -Dshader-cache=disabled -Dbuild-tests=false > cross-setup.log
    ninja -C build-arm64 -k 0 > cross-ninja.log 2>&1 || { grep -E 'error:|FAILED' cross-ninja.log | head -40; exit 1; } )
  # Only the CSF (v10+) queue and the sparse-binding queue reference vk_drm_syncobj (not built without libdrm);
  # neither exists on the G76 (v7). Anything else undefined is an error.
  if grep 'undefined symbol' cross-ninja.log | grep -v 'vk_drm_syncobj_'; then echo "unexpected undefined symbols"; exit 1; fi
fi

if want collect; then
  O=$W/out-panvk; rm -rf $O; mkdir -p $O
  cp build-arm64/src/panfrost/vulkan/libvulkan_panfrost.dll $O/vulkan_panfrost.dll
  python3 -I "$HERE/pe-set-subsys.py" $O/vulkan_panfrost.dll
  printf '{\n    "file_format_version": "1.0.1",\n    "ICD": {\n        "library_path": ".\\\\vulkan_panfrost.dll",\n        "api_version": "1.4.0",\n        "library_arch": "64"\n    }\n}\n' > $O/panfrost_icd.json
  P=$HERE/../../drivers/MaliG76Wddm/tools/vkprobe
  "$ZIG" cc -target aarch64-windows-gnu -O2 -I"$W/mesa/include" "$P/vkprobe.c" -o $O/vkprobe.exe
  cp "$HERE/panvk/README-kit.txt" $O/README.txt
  ( cd $O; sha256sum vulkan_panfrost.dll vkprobe.exe > SHA256SUMS )
  llvm-readobj-18 --coff-exports $O/vulkan_panfrost.dll | grep -E 'Name: vk_icd' || { echo "no ICD exports"; exit 1; }
  cat $O/SHA256SUMS
fi
