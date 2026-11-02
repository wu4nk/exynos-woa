# beyond1lte.dtb — the DTB section of the S10 boot image

sboot's Android-12 `load_kernel` refuses a boot image without a header-v2 DTB
section (`dt table header check fail`, tasks/T09-boot-payload-dtb.md). Our UEFI never
reads this tree; sboot only checks it and patches it. It is the board's own DT so that
sboot's patching (memory nodes, bootargs) lands on a tree it recognises.

- Source: edk2-exynos (`sonic011gamer/edk2-exynos`),
  `Platform/Samsung/exynos9820/FdtBlob_compat/s10.dtb`, last changed in commit
  `c00035413a9df469414b31529640bf1da28859db` (2024-02-08) — the blob that project's
  `s10.sh.inc` packs into its working S10 boot images.
- SHA-256: `5c1853884f477b8c842a07d0bb3e61c619b555743129dc40a9eabbe0935d39d5`
- Content: `model = "Samsung BEYOND1LTE EUR OPEN 26 board based on EXYNOS9820"`, six
  memory nodes (the 8 GB board) — the same board revision string as the user's own
  SM-G973F live tree (notes/beyond1-port.md §1).
- License: the tree is compiled from Samsung's GPL-2.0 kernel DTS (exynos9820 / beyond1).
