/*
 * S10eIspDmaIoctl.h - interface of S10eIspDma (notes/s10-isp.md, Stage 4 option A plan item 5), shared by the driver
 * and its user (S10eIsp's Wudfhost: tools/isp-host plat layer).
 *
 * One physically contiguous pool below 4 GiB, allocated ONCE when the driver starts (system start: contiguous memory
 * is scarce later - notes/s10-camera.md: "~75 min after boot MmAllocateContiguousMemorySpecifyCache found none"),
 * zeroed, and mapped on request into the CALLING process (MmMapLockedPagesSpecifyCache UserMode). The caller
 * sub-allocates it; the ISP's DMA uses physical addresses (its SysMMU is in bypass on Windows, as for CSIS DMA).
 * One mapping at a time (one ISP host); closing the handle (or the process ending) unmaps and re-zeroes the pool.
 */
#pragma once

#define S10EISPDMA_DEVICE_NAME L"\\Device\\S10eIspDma"
#define S10EISPDMA_DOS_NAME    L"\\DosDevices\\S10eIspDma"
#define S10EISPDMA_USER_PATH   L"\\\\.\\S10eIspDma"

#define S10EISPDMA_TYPE 0x8A55u
#define S10EISPDMA_VERSION 0x00000001u   /* 0.1 */

/* out: S10EISPDMA_MAP_OUT. Maps the pool into the calling process (once per handle; again on the same handle returns
   the same mapping). STATUS_DEVICE_BUSY while another handle holds the mapping, STATUS_NO_MEMORY if the pool could
   not be allocated at start. */
#define IOCTL_S10EISPDMA_MAP CTL_CODE(S10EISPDMA_TYPE, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _S10EISPDMA_MAP_OUT {
    unsigned int Version;        /* S10EISPDMA_VERSION */
    unsigned int Cache;          /* 1 = MmWriteCombined, 0 = MmNonCached (the same type for kernel + user mapping) */
    unsigned long long UserVa;   /* the pool in the calling process (read/write, never executable) */
    unsigned long long Pa;       /* physical address of byte 0 (< 4 GiB; contiguous) */
    unsigned long long Bytes;    /* pool size */
} S10EISPDMA_MAP_OUT;
