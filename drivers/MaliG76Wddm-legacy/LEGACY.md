# MaliG76Wddm-legacy — the S10's last known-good GPU kernel driver (1.0.36.1)

Snapshot of `drivers/MaliG76Wddm` at 8604bf2^1 (before the upstream alpha9+ merge): our WDDM 1.3 KMD with the S10
framebuffer alias (G2b-3 runs 33-42). Paired with UMD 0075 (Mesa 26.2 + our 0040-0075 series, `umd-legacy\maliumd.dll`
in the kit, sha256 in its SHA256SUMS) and panvk V1.3. Device-validated on the S10 on 2026-11-11: DWM on the Mali,
gputest 95 PASS / 0 FAIL / 5 MISSING (incl. geometry-shader), vkprobe 7/7.

Why it is back: upstream's 1.0.72.1 + its UMD give a black panel on the S10 in full-adapter mode (2026-11-11 runs,
notes/beyond1-port.md) while display-only works. `set S10_GPU_LEGACY=1` before build-s10.cmd (v1.12+) builds this one
instead; gpu-s10.ps1 v1.23 "display" removes any other MaliG76Wddm package first (1.0.72.1 > 1.0.36.1 would win).
Do not merge upstream changes into this folder.
