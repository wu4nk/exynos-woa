# patches-s10-g2 (archived)

The S10 port's own Mesa line, 0040-0075, made 2026-10-13 .. 2026-11-11 in parallel with upstream's (same base:
Mesa 26.2.4 + wddm-mali-g52 0001-0019 + upstream 0020-0039). Upstream's series (tools/mesa-build/patches, 0040-0222:
WDDM 2.0 DDI, FL 11_0, threaded context, OpenGL ICD, video) replaced it on 2026-11-11 when the S10 branch was merged
onto upstream alpha9+ (notes/beyond1-port.md "Merge onto upstream").

Kept for reference and for pieces upstream does not have: the KMD fb-alias present path (0069/0070), the geometry
shader port from JimVulkan/mali-panfrost with tgsi_to_nir GS support and the s10_gs_hosttest host test (0073-0075),
max_rt / BC fixes (0072). Same numbers as upstream's 0040-0075 but different patches - never apply both.
