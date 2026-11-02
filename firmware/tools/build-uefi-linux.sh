#!/usr/bin/env bash
# build-uefi-linux.sh — build a 9820-family UEFI FD on Linux (clang + lld, CLANGDWARF).
#
#   firmware/tools/build-uefi-linux.sh [Beyond0LtePkg|Beyond1LtePkg] [DEBUG|RELEASE]
#
# Needs: firmware/edk2 (git clone --depth=1 https://github.com/tianocore/edk2 &&
# git submodule update --init --depth=1 --recursive), BaseTools built (make -C BaseTools),
# clang, ld.lld, llvm-objcopy, iasl, python3. The authoritative S10e build stays the
# Windows CLANGPDB one (build-uefi.ps1); this is the S10 port's build path.
set -euo pipefail

PLATFORM="${1:-Beyond1LtePkg}"
TARGET="${2:-DEBUG}"
WS="$(cd "$(dirname "$0")/.." && pwd)"
EDK2="$WS/edk2"
[ -d "$EDK2/BaseTools" ] || { echo "edk2 missing at $EDK2"; exit 1; }

export WORKSPACE="$EDK2"
export EDK_TOOLS_PATH="$EDK2/BaseTools"
export PACKAGES_PATH="$EDK2:$WS"
export PYTHON_COMMAND=python3
export CLANG_BIN="$(dirname "$(command -v clang)")/"
export CLANGDWARF_BIN="$CLANG_BIN"
export PATH="$EDK2/BaseTools/BinWrappers/PosixLike:$PATH"

mkdir -p "$EDK2/Conf"
for f in tools_def build_rule target; do
  [ -f "$EDK2/Conf/$f.txt" ] || sed 's/\r$//' "$EDK2/BaseTools/Conf/$f.template" > "$EDK2/Conf/$f.txt"
done

cd "$EDK2"
# shellcheck disable=SC1091
set +u; . ./edksetup.sh BaseTools >/dev/null; set -u
PYTHONPATH="$EDK2/BaseTools/Source/Python${PYTHONPATH:+:$PYTHONPATH}" python3 "$EDK2/BaseTools/Source/Python/build/build.py" \
  -a AARCH64 -t CLANGDWARF -b "$TARGET" \
  -p "Platform/$PLATFORM/$PLATFORM.dsc" -n "$(nproc)"
