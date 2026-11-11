S10 panvk kit (exynos-woa) - Mesa PanVK 26.3.0-devel (panvk-kbase-android p0175) as a Windows ARM64 Vulkan driver.

Needs: MaliG76Wddm 1.0.72.1 (upstream base, kit v2.x) with its GPU half on. V2 (patch 0006): built against upstream's
shim (Mesa 0222); the V1 builds (S10-panvk-V1.x) only talk to the old 1.0.3x KMD - do not mix them.
Nothing is installed; no Vulkan loader needed.

1. Unzip anywhere, e.g. C:\S10Prov\panvk
2. cmd in that folder:  vkprobe.exe
3. Send back: vkprobe.log, vkprobe-stderr.txt (the driver's own messages), vkprobe-triangle.bmp (if made),
   and an s10-log zip if the phone hung.

vkprobe loads vulkan_panfrost.dll from its own folder and runs: instance, device, fill, copy, compute, triangle,
stress. It stops at the first test whose GPU job never finishes (5 s timeout).
panfrost_icd.json is for later (registering the driver with a Vulkan loader); vkprobe does not use it.
