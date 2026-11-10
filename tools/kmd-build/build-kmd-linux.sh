#!/bin/bash
# build-kmd-linux.sh (v0.1) - exynos-woa G2b-3: build MaliG76Wddm.sys on Linux (no PC with an EWDK needed).
#
#   build-kmd-linux.sh <work-dir> <out-dir> [MwBuild]
#
# <work-dir> caches the WDK: the NuGet packages Microsoft.Windows.WDK.ARM64 10.0.26100.6584 (km headers, km\ARM64
# libs: ntoskrnl, hal, displib, bufferoverflowfastfailk) and Microsoft.Windows.SDK.CPP 10.0.26100.6584 (shared
# headers), downloaded from api.nuget.org once. Compiler/linker: the host's clang-cl 18 + lld-link
# (--target=aarch64-pc-windows-msvc), with the switches the WDK's WindowsKernelModeDriver10.0 toolset uses for an
# ARM64 WDM driver (/kernel /GS /Zp8, entry GsDriverEntry, /DRIVER /SUBSYSTEM:NATIVE). The result is UNSIGNED:
# sign-pe.py signs it with a test certificate (test mode only).
set -euo pipefail
W=$1; O=$2; MWB=${3:-1}
R=$(cd "$(dirname "$0")/../.." && pwd)
SRC=$R/drivers/MaliG76Wddm
V=10.0.26100.6584
I=$W/x/c/Include/10.0.26100.0
L=$W/x/c/Lib/10.0.26100.0/km/ARM64
mkdir -p "$W" "$O"
if [ ! -f "$L/displib.lib" ] || [ ! -f "$I/shared/ntdef.h" ]; then
  (cd "$W" &&
   curl -sS -L -o wdk.nupkg "https://api.nuget.org/v3-flatcontainer/microsoft.windows.wdk.arm64/$V/microsoft.windows.wdk.arm64.$V.nupkg" &&
   curl -sS -L -o sdk.nupkg "https://api.nuget.org/v3-flatcontainer/microsoft.windows.sdk.cpp/$V/microsoft.windows.sdk.cpp.$V.nupkg" &&
   mkdir -p x && cd x &&
   unzip -q -o ../wdk.nupkg 'c/Include/10.0.26100.0/km/*' 'c/Lib/10.0.26100.0/km/ARM64/*' &&
   unzip -q -o ../sdk.nupkg 'c/Include/10.0.26100.0/shared/*')
fi
CL=${CLANG_CL:-clang-cl-18}
LINK=${LLD_LINK:-lld-link}
DEFS="/D_ARM64_ /DARM64 /D_WIN64 /D_USE_DECLSPECS_FOR_SAL=1 /DSTD_CALL /DDEPRECATE_DDK_FUNCTIONS=1 /DMSC_NOOPT
      /D_KERNEL_MODE /DWINNT=1 /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00 /DNTDDI_VERSION=0x0A000008 /DPOOL_NX_OPTIN=1
      /DMW_BUILD=$MWB /DEXYNOS9820_BOARD_BEYOND1=1 /DNDEBUG"
FLAGS="--target=aarch64-pc-windows-msvc /c /nologo /kernel /GS /Gy /GF /Zp8 /O1 /Oi /W4 /Brepro
       /clang:-mgeneral-regs-only /clang:-funwind-tables -fms-compatibility-version=19.40
       -Wno-unused-parameter -Wno-microsoft-anon-tag -Wno-pragma-pack -Wno-ignored-pragma-intrinsic
       -Wno-unknown-pragmas -Wno-ignored-attributes -Wno-nonportable-include-path"
[ -d "$W/casefix" ] || python3 -I "$(dirname "$0")/casefix.py" "$W/casefix" "$I/km/crt" "$I/km" "$I/shared" "$SRC"
INC="/imsvc $I/km/crt /imsvc $I/km /imsvc $I/shared /imsvc $W/casefix /I $SRC/include /I $SRC"
rm -f "$O"/*.obj
for f in adapter display memory command maligpu pram; do
  $CL $FLAGS $DEFS $INC "/Fo$O/$f.obj" "$SRC/$f.c"
done
$LINK /nologo /machine:arm64 /driver /subsystem:native,10.00 /entry:GsDriverEntry /nodefaultlib /Brepro \
  /opt:ref /opt:icf /release /merge:_TEXT=.text /merge:.impcall=.rdata /section:INIT,d /guard:no /dynamicbase /nxcompat \
  /tsaware:no /include:_load_config_used \
  /map:"$O/MaliG76Wddm.map" /out:"$O/MaliG76Wddm.sys" "$O"/*.obj \
  "$L/displib.lib" "$L/bufferoverflowfastfailk.lib" "$L/ntoskrnl.lib" "$L/hal.lib" "$L/libcntpr.lib"
ls -la "$O/MaliG76Wddm.sys"
