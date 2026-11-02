/*
 * S10Touch - Hid.c: the HID side (descriptors, IOCTLs from hidclass through mshidkmdf, delivery of frames).
 * SPDX-License-Identifier: GPL-2.0
 */
#include "S10Touch.h"

/* ---- report descriptor: one touch screen, 10 fingers per report (report 0x54), scan time, contact count,
 * contact count maximum (feature). Same layout as the S6SY761Touch family that runs on this phone, with the
 * logical range of X/Y set to the controller's real 12-bit range (0..4095) in the table itself and the
 * physical size of the S10 panel: 1440 x 3040 px at ~551 dpi = 2.61 x 5.51 in (units 0.01 in). ---- */
#define S10T_FINGER                                                                           \
    0x05, 0x0D,             /*   USAGE_PAGE (Digitizers)                       */              \
    0x09, 0x22,             /*   USAGE (Finger)                                */              \
    0xA1, 0x02,             /*   COLLECTION (Logical)                          */              \
    0x09, 0x42,             /*     USAGE (Tip Switch)                          */              \
    0x15, 0x00,             /*     LOGICAL_MINIMUM (0)                         */              \
    0x25, 0x01,             /*     LOGICAL_MAXIMUM (1)                         */              \
    0x75, 0x01,             /*     REPORT_SIZE (1)                             */              \
    0x95, 0x01,             /*     REPORT_COUNT (1)                            */              \
    0x81, 0x02,             /*     INPUT (Data,Var,Abs)                        */              \
    0x95, 0x07,             /*     REPORT_COUNT (7)                            */              \
    0x81, 0x03,             /*     INPUT (Cnst,Var,Abs) padding                */              \
    0x75, 0x08,             /*     REPORT_SIZE (8)                             */              \
    0x95, 0x01,             /*     REPORT_COUNT (1)                            */              \
    0x09, 0x51,             /*     USAGE (Contact Identifier)                  */              \
    0x25, 0x09,             /*     LOGICAL_MAXIMUM (9)                         */              \
    0x81, 0x02,             /*     INPUT (Data,Var,Abs)                        */              \
    0x05, 0x01,             /*     USAGE_PAGE (Generic Desktop)                */              \
    0x26, 0xFF, 0x0F,       /*     LOGICAL_MAXIMUM (4095)                      */              \
    0x75, 0x10,             /*     REPORT_SIZE (16)                            */              \
    0x55, 0x0E,             /*     UNIT_EXPONENT (-2)                          */              \
    0x65, 0x13,             /*     UNIT (Inch, English linear)                 */              \
    0x35, 0x00,             /*     PHYSICAL_MINIMUM (0)                        */              \
    0x46, 0x05, 0x01,       /*     PHYSICAL_MAXIMUM (261 = 2.61 in)            */              \
    0x09, 0x30,             /*     USAGE (X)                                   */              \
    0x81, 0x02,             /*     INPUT (Data,Var,Abs)                        */              \
    0x46, 0x27, 0x02,       /*     PHYSICAL_MAXIMUM (551 = 5.51 in)            */              \
    0x09, 0x31,             /*     USAGE (Y)                                   */              \
    0x81, 0x02,             /*     INPUT (Data,Var,Abs)                        */              \
    0xC0                    /*   END_COLLECTION                                */

static const UCHAR S10tReportDescriptor[] = {
    0x05, 0x0D,             /* USAGE_PAGE (Digitizers)                         */
    0x09, 0x04,             /* USAGE (Touch Screen)                            */
    0xA1, 0x01,             /* COLLECTION (Application)                        */
    0x85, S10T_REPORT_ID,   /*   REPORT_ID (0x54)                              */
    S10T_FINGER, S10T_FINGER, S10T_FINGER, S10T_FINGER, S10T_FINGER,
    S10T_FINGER, S10T_FINGER, S10T_FINGER, S10T_FINGER, S10T_FINGER,
    0x05, 0x0D,             /*   USAGE_PAGE (Digitizers)                       */
    0x55, 0x0C,             /*   UNIT_EXPONENT (-4)                            */
    0x66, 0x01, 0x10,       /*   UNIT (Seconds)                                */
    0x47, 0xFF, 0xFF, 0x00, 0x00, /* PHYSICAL_MAXIMUM (65535)                  */
    0x27, 0xFF, 0xFF, 0x00, 0x00, /* LOGICAL_MAXIMUM (65535)                   */
    0x75, 0x10,             /*   REPORT_SIZE (16)                              */
    0x95, 0x01,             /*   REPORT_COUNT (1)                              */
    0x09, 0x56,             /*   USAGE (Scan Time)                             */
    0x81, 0x02,             /*   INPUT (Data,Var,Abs)                          */
    0x55, 0x00,             /*   UNIT_EXPONENT (0)                             */
    0x65, 0x00,             /*   UNIT (None)                                   */
    0x35, 0x00,             /*   PHYSICAL_MINIMUM (0)                          */
    0x45, 0x00,             /*   PHYSICAL_MAXIMUM (0)                          */
    0x09, 0x54,             /*   USAGE (Contact Count)                         */
    0x75, 0x08,             /*   REPORT_SIZE (8)                               */
    0x25, 0x0A,             /*   LOGICAL_MAXIMUM (10)                          */
    0x81, 0x02,             /*   INPUT (Data,Var,Abs)                          */
    0x09, 0x55,             /*   USAGE (Contact Count Maximum)                 */
    0xB1, 0x02,             /*   FEATURE (Data,Var,Abs)                        */
    0xC0                    /* END_COLLECTION                                  */
};

static const HID_DESCRIPTOR S10tHidDescriptor = {
    0x09,                   /* bLength */
    0x21,                   /* HID */
    0x0100,                 /* HID 1.00 */
    0x00,                   /* country */
    0x01,                   /* one class descriptor */
    { { 0x22, sizeof(S10tReportDescriptor) } }
};

static const WCHAR S10tManufacturer[] = L"Samsung Electronics";
static const WCHAR S10tProduct[]      = L"Galaxy S10 Touchscreen (sec_ts y761)";
static const WCHAR S10tSerial[]       = L"S10TOUCH";

/* ======================================================================
 * Delivery (worker thread only): FIFO head -> pending IOCTL_HID_READ_REPORT
 * ====================================================================== */

VOID S10tDeliver(_In_ PS10T_DEVICE Dev)
{
    while (Dev->FifoCount != 0) {
        WDFREQUEST request;
        WDFMEMORY  mem;
        NTSTATUS   status;

        if (!NT_SUCCESS(WdfIoQueueRetrieveNextRequest(Dev->ReadQueue, &request))) {
            return;                                     /* nobody reading yet; the next read kicks us */
        }
        status = WdfRequestRetrieveOutputMemory(request, &mem);
        if (NT_SUCCESS(status)) {
            status = WdfMemoryCopyFromBuffer(mem, 0, &Dev->Fifo[Dev->FifoHead].Report, sizeof(S10T_REPORT));
        }
        if (NT_SUCCESS(status)) {
            Dev->FifoHead = (Dev->FifoHead + 1) % S10T_FIFO_DEPTH;
            Dev->FifoCount--;
            WdfRequestSetInformation(request, sizeof(S10T_REPORT));
            InterlockedIncrement(&Dev->Stats.Delivered);
        }
        WdfRequestComplete(request, status);
    }
}

/* ======================================================================
 * IOCTLs
 * ====================================================================== */

static NTSTATUS CopyOut(_In_ WDFREQUEST Request, _In_reads_bytes_(Len) const VOID *Buf, _In_ size_t Len)
{
    WDFMEMORY mem;
    size_t    size;
    NTSTATUS  status;

    status = WdfRequestRetrieveOutputMemory(Request, &mem);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    (VOID)WdfMemoryGetBuffer(mem, &size);
    if (size < Len) {
        return STATUS_INVALID_BUFFER_SIZE;
    }
    status = WdfMemoryCopyFromBuffer(mem, 0, (PVOID)Buf, Len);
    if (NT_SUCCESS(status)) {
        WdfRequestSetInformation(Request, Len);
    }
    return status;
}

/* hidclass passes HID_XFER_PACKET in Irp->UserBuffer for the feature IOCTLs (vhidmini2 KMDF). */
static NTSTATUS GetFeature(_In_ WDFREQUEST Request)
{
    WDF_REQUEST_PARAMETERS p;
    HID_XFER_PACKET       *packet;

    WDF_REQUEST_PARAMETERS_INIT(&p);
    WdfRequestGetParameters(Request, &p);
    if (p.Parameters.DeviceIoControl.OutputBufferLength < sizeof(HID_XFER_PACKET)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    packet = (HID_XFER_PACKET *)WdfRequestWdmGetIrp(Request)->UserBuffer;
    if (packet == NULL || packet->reportBuffer == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (packet->reportId != S10T_REPORT_ID) {
        return STATUS_INVALID_PARAMETER;
    }
    if (packet->reportBufferLen < 2) {
        return STATUS_INVALID_BUFFER_SIZE;
    }
    packet->reportBuffer[0] = S10T_REPORT_ID;
    packet->reportBuffer[1] = S10T_MAX_CONTACTS;      /* Contact Count Maximum */
    WdfRequestSetInformation(Request, 2);
    return STATUS_SUCCESS;
}

static NTSTATUS GetString(_In_ WDFREQUEST Request)
{
    WDF_REQUEST_PARAMETERS p;
    ULONG                  id;
    const WCHAR           *str;
    size_t                 len;

    WDF_REQUEST_PARAMETERS_INIT(&p);
    WdfRequestGetParameters(Request, &p);
    /* the string id is the low word of the Type3InputBuffer VALUE (not a pointer) */
    id = PtrToUlong(p.Parameters.DeviceIoControl.Type3InputBuffer) & 0xFFFF;
    switch (id) {
    case HID_STRING_ID_IMANUFACTURER: str = S10tManufacturer; len = sizeof(S10tManufacturer); break;
    case HID_STRING_ID_IPRODUCT:      str = S10tProduct;      len = sizeof(S10tProduct);      break;
    case HID_STRING_ID_ISERIALNUMBER: str = S10tSerial;       len = sizeof(S10tSerial);       break;
    default: return STATUS_INVALID_PARAMETER;
    }
    return CopyOut(Request, str, len);
}

static VOID EvtInternalIoctl(_In_ WDFQUEUE Queue, _In_ WDFREQUEST Request, _In_ size_t OutLen, _In_ size_t InLen,
                             _In_ ULONG Code)
{
    PS10T_DEVICE         Dev = S10tGet(WdfIoQueueGetDevice(Queue));
    HID_DEVICE_ATTRIBUTES attr;
    NTSTATUS             status;

    UNREFERENCED_PARAMETER(OutLen);
    UNREFERENCED_PARAMETER(InLen);

    switch (Code) {
    case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
        status = CopyOut(Request, &S10tHidDescriptor, sizeof(S10tHidDescriptor));
        break;
    case IOCTL_HID_GET_REPORT_DESCRIPTOR:
        status = CopyOut(Request, S10tReportDescriptor, sizeof(S10tReportDescriptor));
        break;
    case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
        RtlZeroMemory(&attr, sizeof(attr));
        attr.Size          = sizeof(attr);
        attr.VendorID      = S10T_VID;
        attr.ProductID     = S10T_PID;
        attr.VersionNumber = S10T_VERSION;
        status = CopyOut(Request, &attr, sizeof(attr));
        break;
    case IOCTL_HID_READ_REPORT:
        status = WdfRequestForwardToIoQueue(Request, Dev->ReadQueue);
        if (NT_SUCCESS(status)) {
            S10tKickWorker(Dev);                        /* the worker owns the FIFO: it completes the read */
            return;
        }
        break;
    case IOCTL_HID_GET_FEATURE:
        status = GetFeature(Request);
        break;
    case IOCTL_HID_SET_FEATURE:
        status = STATUS_SUCCESS;                        /* nothing to set on a touch screen */
        break;
    case IOCTL_HID_GET_STRING:
        status = GetString(Request);
        break;
    case IOCTL_HID_ACTIVATE_DEVICE:
    case IOCTL_HID_DEACTIVATE_DEVICE:
        status = STATUS_SUCCESS;
        break;
    default:
        /* WRITE_REPORT, GET/SET input/output reports, indexed strings, idle notification, physical
           descriptor: not supported by a touch screen minidriver */
        status = STATUS_NOT_SUPPORTED;
        break;
    }
    WdfRequestComplete(Request, status);
}

NTSTATUS S10tQueuesCreate(_In_ WDFDEVICE Device, _In_ PS10T_DEVICE Dev)
{
    WDF_IO_QUEUE_CONFIG qc;
    WDFQUEUE            q;
    NTSTATUS            status;

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&qc, WdfIoQueueDispatchParallel);
    qc.EvtIoInternalDeviceControl = EvtInternalIoctl;
    status = WdfIoQueueCreate(Device, &qc, WDF_NO_OBJECT_ATTRIBUTES, &q);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_IO_QUEUE_CONFIG_INIT(&qc, WdfIoQueueDispatchManual);
    qc.PowerManaged = WdfFalse;                         /* reads stay pending across D0 transitions */
    return WdfIoQueueCreate(Device, &qc, WDF_NO_OBJECT_ATTRIBUTES, &Dev->ReadQueue);
}
