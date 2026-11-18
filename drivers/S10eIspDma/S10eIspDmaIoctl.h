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
#define S10EISPDMA_VERSION 0x00000002u   /* 0.2 (KVA + SHM) */

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

/* 0.2: IRP_MJ_INTERNAL_DEVICE_CONTROL, kernel callers only (S10eCamera): the pool's kernel mapping. out:
   S10EISPDMA_KVA_OUT (METHOD_BUFFERED). */
#define IOCTL_S10EISPDMA_KVA CTL_CODE(S10EISPDMA_TYPE, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _S10EISPDMA_KVA_OUT {
    unsigned int Version;
    unsigned int Cache;
    unsigned long long KernelVa; /* the driver's own mapping of byte 0 (valid while S10eIspDma is loaded) */
    unsigned long long Pa;
    unsigned long long Bytes;
} S10EISPDMA_KVA_OUT;

/* 0.2: the pool's LAST 64 KiB = the control block between S10eIsp's camera pipeline (Wudfhost, Samsung's libraries:
   3AA0 + ISP0 on real frames) and S10eCamera (AVStream). S10eIsp sub-allocates only the rest. The pool is zeroed when
   S10eIsp's mapping goes away (Magic 0 = no pipeline). */
#define S10EISP_SHM_BYTES 0x10000u
#define S10EISP_SHM_MAGIC 0x50495053u    /* 'SPIP' */
#define S10EISP_SHM_SLOTS 3u
typedef struct _S10EISP_SHM {
    unsigned int Magic;
    unsigned int Version;                /* 1 */
    volatile long Want;                  /* S10eCamera: 1 = frames wanted (a pin is running), 0 = stop */
    volatile long State;                 /* S10eIsp: 1 ready (libraries up), 2 armed (3AA0 waits for OTF), 3 frames */
    volatile long Seq;                   /* finished YUY2 frames (since the pipeline started) */
    volatile long Latest;                /* slot of the newest finished frame */
    unsigned int W, H, Stride, NSlots;   /* 912, 684, 1824, 3: YUY2 (Y0 U Y1 V), sensor orientation */
    unsigned int SlotPa[S10EISP_SHM_SLOTS];
    volatile long AeSeq;                 /* the libraries' latest sensor request (cis request_exposure / _gain) */
    unsigned int AeExpUs, AeAGain1000, AeDGain1000;
    volatile long Sessions;              /* Want 0 -> 1 cycles served */
    volatile long Errors;                /* library steps that did not return 0 */
    char Status[128];
} S10EISP_SHM;
