/*
 * S10eIspDma.c (v0.1) - DMA memory for the camera ISP's user-mode host (notes/s10-isp.md, Stage 4 option A, item 5).
 * SPDX-License-Identifier: GPL-2.0
 *
 * Why a kernel driver: UMDF 2 has no DMA ("the following features are available only to KMDF drivers ... Direct
 * memory access (DMA)", wdf/comparing-umdf-2-0-functionality-to-kmdf). Samsung's ISP libraries (running in S10eIsp's
 * Wudfhost) need device-visible buffers: their own taaisp / tnr / medrc pools and the 3AA / ISP image buffers.
 *
 * Documented route only:
 *  - MmAllocateContiguousMemorySpecifyCache, below 4 GiB (the ISP's DMA addresses are 32-bit), once in DriverEntry
 *    ("A driver that must use contiguous memory should allocate this memory during driver initialization"), zeroed
 *    ("A kernel-mode driver must first zero this memory if it is going to make it visible to user-mode software");
 *  - MmBuildMdlForNonPagedPool ("memory that is allocated by ... MmAllocateContiguousMemorySpecifyCache"; "For
 *    mappings to user space, MDLs that are built by the MmBuildMdlForNonPagedPool routine can be used");
 *  - MmMapLockedPagesSpecifyCache(UserMode) inside try/except, from the IOCTL handler = in the caller's process
 *    context; the same caching type as the kernel allocation (MEMORY_CACHING_TYPE remarks: "only one caching behavior
 *    is allowed for any given physical address translation"); MmUnmapLockedPages at IRP_MJ_CLEANUP of that handle
 *    (runs in the closing process's context, also at process exit).
 * Caching: MmWriteCombined first, MmNonCached if that allocation fails (S10eCam's experience on this phone). Neither
 * is cached, so neither CPU nor ISP needs cache maintenance. How Windows ARM64 maps them (Normal-NC vs Device) is NOT
 * documented: S10eIsp 0.2 tests unaligned and DC ZVA-style accesses on the mapping (the libraries do both).
 * Pool size: HKLM\SYSTEM\CurrentControlSet\Services\S10eIspDma\Parameters PoolMiB (DWORD, 16..512, default 128):
 * the front path's own allocations were 45 MiB in the native run (notes), the 3AA output buffers ~22 MiB more.
 * Security: device ACL SYSTEM + Administrators + LocalService (the account Wudfhost runs as,
 * wdf/umdf-driver-host-process). Nothing here touches a hardware register.
 */
#include <ntddk.h>
#include <wdmsec.h>
#include "S10eIspDmaIoctl.h"

#define POOL_MIB_DEFAULT 128u
#define POOL_MIB_MIN 16u
#define POOL_MIB_MAX 512u

static PDEVICE_OBJECT gDevice;
static PVOID gPool;                /* kernel VA */
static SIZE_T gBytes;
static PHYSICAL_ADDRESS gPa;
static MEMORY_CACHING_TYPE gCache;
static PMDL gMdl;
static FAST_MUTEX gLock;
static PFILE_OBJECT gOwner;        /* the handle holding the user mapping */
static PEPROCESS gOwnerProcess;
static PVOID gUserVa;

DRIVER_INITIALIZE DriverEntry;
DRIVER_UNLOAD S10eIspDmaUnload;
_Dispatch_type_(IRP_MJ_CREATE) _Dispatch_type_(IRP_MJ_CLOSE) _Dispatch_type_(IRP_MJ_CLEANUP)
DRIVER_DISPATCH S10eIspDmaCreateClose;
_Dispatch_type_(IRP_MJ_DEVICE_CONTROL) DRIVER_DISPATCH S10eIspDmaIoctl;

static ULONG PoolMiB(PUNICODE_STRING RegistryPath)
{
    RTL_QUERY_REGISTRY_TABLE q[2];
    ULONG v = POOL_MIB_DEFAULT, def = POOL_MIB_DEFAULT;
    UNICODE_STRING path;
    WCHAR buf[256];
    RtlZeroMemory(q, sizeof(q));
    RtlInitEmptyUnicodeString(&path, buf, sizeof(buf));
    if (!NT_SUCCESS(RtlAppendUnicodeStringToString(&path, RegistryPath)) ||
        !NT_SUCCESS(RtlAppendUnicodeToString(&path, L"\\Parameters")))
        return POOL_MIB_DEFAULT;
    q[0].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK;
    q[0].Name = L"PoolMiB";
    q[0].EntryContext = &v;
    q[0].DefaultType = (REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT) | REG_DWORD;
    q[0].DefaultData = &def;
    q[0].DefaultLength = sizeof(def);
    if (!NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, path.Buffer, q, NULL, NULL)))
        v = POOL_MIB_DEFAULT;
    if (v < POOL_MIB_MIN) v = POOL_MIB_MIN;
    if (v > POOL_MIB_MAX) v = POOL_MIB_MAX;
    return v;
}

static VOID PoolAlloc(ULONG MiB)
{
    PHYSICAL_ADDRESS lo, hi, bnd;
    ULONG pass;
    lo.QuadPart = 0;
    hi.QuadPart = 0xFFFFFFFFLL;
    bnd.QuadPart = 0;
    /* the asked size first, then halves down to the minimum; write-combined, then non-cached, at each size */
    for (; MiB >= POOL_MIB_MIN && gPool == NULL; MiB /= 2) {
        for (pass = 0; pass < 2 && gPool == NULL; pass++) {
            gCache = pass == 0 ? MmWriteCombined : MmNonCached;
            gBytes = (SIZE_T)MiB << 20;
            gPool = MmAllocateContiguousMemorySpecifyCache(gBytes, lo, hi, bnd, gCache);
        }
    }
    if (gPool == NULL) {
        gBytes = 0;
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "S10eIspDma: no contiguous memory below 4 GiB\n");
        return;
    }
    RtlZeroMemory(gPool, gBytes);
    gPa = MmGetPhysicalAddress(gPool);
    gMdl = IoAllocateMdl(gPool, (ULONG)gBytes, FALSE, FALSE, NULL);
    if (gMdl == NULL) {
        MmFreeContiguousMemory(gPool);
        gPool = NULL;
        gBytes = 0;
        return;
    }
    MmBuildMdlForNonPagedPool(gMdl);
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "S10eIspDma: pool %Iu bytes at PA %I64X (%s)\n", gBytes,
               gPa.QuadPart, gCache == MmWriteCombined ? "write-combined" : "non-cached");
}

/* in the owner process's context (IOCTL / CLEANUP of the owning handle) */
static VOID Unmap(VOID)
{
    if (gUserVa != NULL) {
        MmUnmapLockedPages(gUserVa, gMdl);
        gUserVa = NULL;
        RtlZeroMemory(gPool, gBytes);        /* the next host never sees this one's data */
    }
    if (gOwnerProcess != NULL) {
        ObDereferenceObject(gOwnerProcess);
        gOwnerProcess = NULL;
    }
    gOwner = NULL;
}

NTSTATUS S10eIspDmaCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    UNREFERENCED_PARAMETER(DeviceObject);
    if (sp->MajorFunction == IRP_MJ_CLEANUP) {
        ExAcquireFastMutex(&gLock);
        if (gOwner == sp->FileObject && gOwnerProcess == PsGetCurrentProcess())
            Unmap();
        ExReleaseFastMutex(&gLock);
    }
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

NTSTATUS S10eIspDmaIoctl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS st = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR info = 0;
    UNREFERENCED_PARAMETER(DeviceObject);

    if (sp->Parameters.DeviceIoControl.IoControlCode == IOCTL_S10EISPDMA_MAP) {
        if (Irp->RequestorMode != UserMode) {
            st = STATUS_ACCESS_DENIED;
        } else if (sp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(S10EISPDMA_MAP_OUT)) {
            st = STATUS_BUFFER_TOO_SMALL;
        } else if (gPool == NULL) {
            st = STATUS_NO_MEMORY;
        } else {
            S10EISPDMA_MAP_OUT *o = (S10EISPDMA_MAP_OUT *)Irp->AssociatedIrp.SystemBuffer;
            ExAcquireFastMutex(&gLock);
            if (gOwner != NULL && (gOwner != sp->FileObject || gOwnerProcess != PsGetCurrentProcess())) {
                st = STATUS_DEVICE_BUSY;
            } else {
                st = STATUS_SUCCESS;
                if (gUserVa == NULL) {
                    __try {
                        /* "A non-executable mapping is always created when AccessMode is UserMode" */
                        gUserVa = MmMapLockedPagesSpecifyCache(gMdl, UserMode, gCache, NULL, FALSE,
                                                               NormalPagePriority | MdlMappingNoExecute);
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                        gUserVa = NULL;
                        st = GetExceptionCode();
                    }
                    if (gUserVa != NULL) {
                        gOwner = sp->FileObject;
                        gOwnerProcess = PsGetCurrentProcess();
                        ObReferenceObject(gOwnerProcess);
                    } else if (NT_SUCCESS(st)) {
                        st = STATUS_INSUFFICIENT_RESOURCES;
                    }
                }
                if (NT_SUCCESS(st)) {
                    RtlZeroMemory(o, sizeof(*o));
                    o->Version = S10EISPDMA_VERSION;
                    o->Cache = gCache == MmWriteCombined ? 1u : 0u;
                    o->UserVa = (unsigned long long)(ULONG_PTR)gUserVa;
                    o->Pa = (unsigned long long)gPa.QuadPart;
                    o->Bytes = (unsigned long long)gBytes;
                    info = sizeof(*o);
                }
            }
            ExReleaseFastMutex(&gLock);
        }
    }
    Irp->IoStatus.Status = st;
    Irp->IoStatus.Information = info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return st;
}

VOID S10eIspDmaUnload(PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING dos;
    UNREFERENCED_PARAMETER(DriverObject);
    /* every handle is closed before unload, so CLEANUP has unmapped */
    if (gMdl != NULL) IoFreeMdl(gMdl);
    if (gPool != NULL) MmFreeContiguousMemory(gPool);
    gMdl = NULL;
    gPool = NULL;
    RtlInitUnicodeString(&dos, S10EISPDMA_DOS_NAME);
    IoDeleteSymbolicLink(&dos);
    if (gDevice != NULL) IoDeleteDevice(gDevice);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    UNICODE_STRING name, dos, sddl;
    NTSTATUS st;
    static const GUID cls = { 0x7c1d2e55, 0x0b6a, 0x4e19, { 0x8f, 0x3d, 0x55, 0x9a, 0x21, 0x0e, 0x4b, 0x6c } };

    ExInitializeFastMutex(&gLock);
    DriverObject->DriverUnload = S10eIspDmaUnload;
    PoolAlloc(PoolMiB(RegistryPath));         /* a failure is reported per IOCTL (STATUS_NO_MEMORY), not here */
    RtlInitUnicodeString(&name, S10EISPDMA_DEVICE_NAME);
    RtlInitUnicodeString(&sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;LS)");
    st = IoCreateDeviceSecure(DriverObject, 0, &name, FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN, FALSE, &sddl,
                              &cls, &gDevice);
    if (!NT_SUCCESS(st)) {
        if (gMdl != NULL) IoFreeMdl(gMdl);
        if (gPool != NULL) MmFreeContiguousMemory(gPool);
        gMdl = NULL;
        gPool = NULL;
        return st;
    }
    RtlInitUnicodeString(&dos, S10EISPDMA_DOS_NAME);
    (VOID)IoCreateSymbolicLink(&dos, &name);
    DriverObject->MajorFunction[IRP_MJ_CREATE] = S10eIspDmaCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = S10eIspDmaCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP] = S10eIspDmaCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = S10eIspDmaIoctl;
    gDevice->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}
