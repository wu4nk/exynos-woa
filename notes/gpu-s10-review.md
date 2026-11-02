# GPU source review against references (S10 stage G1 follow-up, 2026-11-02)

Task: user asked to pull more reference material and fix the GPU sources where they are wrong.
Scope: drivers/MaliG76Wddm/maligpu.{c,h} (GPU half), MaliG76Probe.c, UEFI GpuPower.c/GpuAcpm.c.
No device access (S10 owner tests). Builds: UEFI only (Linux); Windows drivers cannot be compiled here.

## References (scratchpad/refs, outside the repo)
- wddm-mali-g52 (github gahingwoo, depth 1, HEAD of 2026-11-02) - origin of maligpu/maliwddm/m1probe.
- linux torvalds master 6628e7d (2026-11-01), sparse: drivers/gpu/drm/panfrost, include/uapi/drm/panfrost_drm.h,
  drivers/iommu/io-pgtable-arm.c.
- LineageOS android_kernel_samsung_exynos9820 lineage-23.2, sparse: drivers/gpu/arm (b_r10p0..bv_r32p1 kbase +
  exynos glue), drivers/soc/samsung/cal-if, exynos-pd*, arch/arm64/boot/dts/exynos/exynos9820*.

## Progress log
- maligpu.c vs Linux panfrost (master 6628e7d) + io-pgtable-arm.c:
  - AS0 setup MATCHES panfrost's legacy (ARM_MALI_LPAE) path: TRANSTAB = L0 | READ_INNER(4) | ADRMODE_TABLE(3) = |7
    (io-pgtable-arm.c:1259-1262); MEMATTR 0x00888D88 = IMP_DEF/WRITE_ALLOC/IMP_DEF at idx 0/1/2 (:1244-1250);
    TRANSCFG 0 = AS_TRANSCFG_ADRMODE_LEGACY (panfrost_regs.h:331, panfrost_mmu.c mmu_cfg_init_mali_lpae).
  - Leaf PTE 0x2C5 MATCHES: Mali LPAE uses TYPE_BLOCK(1) at level 3 too (io-pgtable-arm.c:380-383), HAP read|write
    (bits 6,7 - stage-2-style permissions, :522-533), ATTRINDX 1 cache (:556), SH_OS (:566-569), no AF (:577).
  - XN differs: panfrost ARM_LPAE_PTE_XN = 3<<53 (bits 53+54, :77), ours 1<<54 only. kbase aarch64 mode uses bit 54
    alone. Both bits is what the legacy-format reference sets -> align (low risk).
  - panfrost_mmu_enable does FLUSH_MEM over the whole VA (lock 0..~0) BEFORE writing TRANSTAB (:200); ours does
    not. After a soft reset nothing is cached, but MkGpuBringup after a fault re-enables a live AS -> add it.
  - lock_region width rule identical (max(fls64(start^end-1), 15) - 1).
  - Power-on order/timeouts identical (panfrost_gpu.c panfrost_gpu_power_on); soft reset identical, except panfrost
    unmasks GPU_IRQ_MASK_ERROR|PERFCNT|CLEAN_CACHES, ours FAULT|MULTIPLE_FAULT = MASK_ERROR (fine, no perfcnt).
  - Quirks: G76 features TLS_HASHING + IDVS_GROUP_SIZE, issues 0 (panfrost_features.h:85-97, issues.h:259) ->
    SHADER_CONFIG = SC_TLS_HASH_ENABLE, JM_CONFIG = 0xF<<16: MATCHES. kbase (Samsung bv_r32p1
    mali_kbase_pm_driver.c:2337-2420) ORs into the read value instead of overwriting; reset values are 0 on the G76
    (S10e G9b log: SHADER_CONFIG 0x20000 / JM_CONFIG 0xF0000 after our writes) -> same result.
  - L2_MMU_CONFIG: kbase writes AID limits KBASE_3BIT_AID_32 = 0 = "no limit" for TNOX (l2_mmu_config.c, defaults.h:63)
    = the reset value -> nothing to do (panfrost does not write it either).
  - COHERENCY_ENABLE: kbase writes system_coherency (COHERENCY_NONE 31, no system-coherency in the 9820 DT) after
    every reset (cache_policy_backend.c:27-38, pm_driver.c:2732); panfrost never writes it. Add the kbase write
    (G76 has COHERENCY_REG) - removes a dependency on the reset value.
  - Job submit vs panfrost_job_hw_submit (panfrost_job.c:205-270): affinity = shader_present, cfg = AS | PRI(8) |
    START/END FLUSH_CLEAN_INVALIDATE | chain flag: MATCHES. Panfrost also checks JS_COMMAND_NEXT == 0 before
    writing the _NEXT registers (WARN + refuse); ours overwrites a pending NEXT silently -> add the check.
    FLUSH_REDUCTION (G76 has it) is an optimisation only (not needed without JS_FLUSH_ID_NEXT).
- Samsung 9820 kernel config (beyond0lte == beyond1lte Mali options): bv_r32p1, no LLC, no coherency, BTS_MO
  (bus QoS, optional). The Exynos glue's coherency/LLC hooks are dummies on the 9820.
- command.c / memory.c (GPU-half callers):
  - BuildPagingBuffer MAP_APERTURE_SEGMENT takes the PFNs from pMdl + MdlOffset but always maps them at the
    allocation's GpuVa (offset 0); a partial map (MdlOffset != 0) would land on the wrong VA, and the
    NumberOfPages clamp ignores the offset. Fix: map at GpuVa + MdlOffset pages, clamp to Pages - MdlOffset.
    (Whole-allocation maps - the common case - are unchanged.)
  - Reap() (ISR / SynchronizeExecution, DIRQL) calls MkGpuBringup after a job fault: soft reset + power-up
    + AS update polls, worst case ~70 ms at DIRQL. Panfrost defers the reset to a workqueue
    (panfrost_device_schedule_reset). Not fixed now (needs a reset work item + queue drain design); recorded.
  - AS0 LOCK/FLUSH_PT from BuildPagingBuffer (PASSIVE, MmuLock) can interleave with a Bringup at DIRQL; panfrost
    serialises AS commands with as_lock. Recorded, same follow-up as the reset item.
  - Segment CacheCoherent FALSE -> VidMm maps allocations uncached/WC for the CPU; GPU side flushes L2 at job
    start/end (JS_CONFIG). Consistent for a non-coherent G76 (kbase: COHERENCY_NONE on the 9820).
- XN re-checked against Arm's own legacy-LPAE mode (Samsung tree b_r12p0..b_r19p0 mali_kbase_mmu_mode_lpae.c:38
  ENTRY_NX_BIT = 1<<54): ours MATCHES Arm; panfrost's 3<<53 is the io-pgtable generic XN. No change.
- UEFI GpuPower.c vs flexpmu_cal_local_exynos9820.h:555-566 embedded_g3d_on: all 11 steps, values, the 0xC000
  atomic-set alias (pmucal_rae.c:145-151), 0x118 us delays (udelay, :356) and the 2000 x 1 us waits (:102-116)
  MATCH; tz_restore after the sequence as pmucal_local.c:45-52. No defect.
- Upstream wddm-mali-g52 after our base 16f7e7c6 (2026-10-03 .. HEAD e8bcd36):
  - d064464 + 555a120 (2026-10-17, VALIDATED on their G52: "jobs done 24, failed 0, resets 0", was 2 failed):
    the libdrm shim only put the BOs a submit names into the WDDM allocation list, but Mesa/panfrost expects every
    BO mapped from creation -> VidMm never paged in the others -> read translation faults 0x4002c3. Fix: list
    every live BO. Applies cleanly on top of our 0020-0037 shim hunks (replayed in scratchpad/shim-port).
    -> our Mesa patch 0040 (GpuEnable 1 path only; softpipe path unaffected).
  - Upstream added mesa-patches/0020-d3d10umd-default-to-panfrost-as-maliumd.patch: build-mesa-g76.ps1 applies
    EVERY $Ref\mesa-patches file -> with a newer clone it would apply upstream 0020 + ours (same change) and fail.
    -> pin $Ref to 16f7e7c6 in the build script.
  - KMD: SectionBackedPrimary + CpuVisible + vsync emulation + stats fields - we already have CpuVisible and our
    own soft vsync / FlipMmio; our full mode reached a desktop without SectionBackedPrimary -> not taken.

## Fix 1 - maligpu.c / command.c (commit below)
- SoftReset: GPU_COHERENCY_ENABLE = COHERENCY_NONE (31) after every reset (kbase behaviour on the 9820).
- AsEnable: AS_COMMAND_FLUSH_MEM over the whole VA (lock 0..~0, width 63) before reprogramming AS0 (panfrost).
  FlushRange and AsEnable now share AsOperation (the lock_region + op + wait sequence, unchanged maths).
- MkJobStart returns FALSE without writing if JS_COMMAND_NEXT != 0 (panfrost); Kick counts JobStartRefused and
  leaves Running set so Reap's timeout path (hard stop, reset, drop the submission) recovers. Reported in the
  StopDevice registry trace and the GetEngineTrace-style string (command.c).
- Check: maligpu.c compiles clean (clang -fsyntax-only -Wall -Wextra, stub ntddk.h in scratchpad/stub, -fshort-wchar).
  command.c/adapter.c not compilable here (dispmprt.h) - edits are a field, a counter and a format argument.
- Review of Fix 1 (f91fbc8, adversarial): the refused-start recovery claim checked - Reap(TRUE) runs from SyncPoll
  (command.c:425-436, the 1 ms poll) with MW_JOB_TIMEOUT_MS 2000 (command.c:23) -> hard stop + MkGpuBringup + drop:
  real path. FLUSH_MEM on a freshly reset AS0 (MkGpuInit) = what panfrost_mmu_as_get -> panfrost_mmu_enable does on
  a reset GPU; a stuck AS_ACTIVE only costs the 10 ms poll (AsWaitReady result ignored, as before). COHERENCY_NONE
  is valid whatever COHERENCY_FEATURES says (kbase writes it unconditionally with COHERENCY_REG). ChainFlag still
  advances on a refused start - harmless (the slot is reset before the next start). No defect found.

## Fix 2 - partial aperture maps: counted, not changed
- MS docs leave open whether MdlOffset is relative to the allocation; the full-mode present path (AllocationVa uses
  MdlPage = MdlOffset) is device-validated with the current reading, so remapping at GpuVa + MdlOffset is a guess.
  Instead PartialMaps / LastPartialMdlOffset are counted (BuildPagingBuffer) and logged at StopDevice; a GpuEnable 1
  run with PartialMaps 0 closes it.
