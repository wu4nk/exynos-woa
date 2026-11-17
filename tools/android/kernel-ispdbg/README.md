# ExtremeKernel OneUI 6 for beyond1lte with /dev/mem + debug (ispdbg1)

For tools/android/ispregs-s10.sh (notes/s10-isp.md). Source: github.com/Kernels-by-ravindu644/
samsung_kernel_exynos9820_extremerom @6cf3bf465 (main = OneUI 6/7), submodule KernelSU = BakaSU.
Build = that repo's build.sh steps for `--model beyond1lte --oneui 6`, plus isp-debug.config (this folder):

    make O=out ARCH=arm64 LLVM=1 LLVM_IAS=1 HOSTCC=gcc HOSTCXX=g++ CC=clang CLANG_TRIPLE=aarch64-linux-gnu- \
      LOCALVERSION= exynos9820_defconfig beyond1lte.config custom.config version.config droidspaces.config \
      oneui6.config isp-debug.config
    make ... Image          (then build.sh's pack_boot_image: mkbootimg board SRPRI28B016KU, repo ramdisk, twrp_zip)

- version.config: CONFIG_LOCALVERSION="-ExtremeKernel-ravindu644-6cf3bf465-ispdbg1".
- Compiler: Ubuntu clang/LLD 18.1.3 (the repo expects AOSP clang-r510928, also clang 18).
- Result 2026-11-17: Image 55 867 408 B, boot.img 56 565 760 B; boot partition sda14 112640 sectors =
  57 671 680 B (gpt dump before the dual-boot split) -> 1 105 920 B to spare. The function tracer makes the image
  larger: if a later build does not fit, drop FUNCTION_GRAPH_TRACER / FUNCTION_TRACER first.
- Host packages needed besides clang/lld: cpio (kernel/gen_kheaders.sh and the ramdisk). LTO link needs ~4 GB free.
