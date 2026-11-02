/*
 * S10Touch - Driver.c: PnP/power, the attention interrupt, the SPB transport and the input worker.
 * See S10Touch.h for the design. SPDX-License-Identifier: GPL-2.0
 */
#include "S10Touch.h"

/* ======================================================================
 * Small helpers
 * ====================================================================== */

static ULONG AliveRead(_In_ PS10T_DEVICE Dev, _In_ ULONG Off)
{
    return READ_REGISTER_ULONG((volatile ULONG *)(Dev->Alive + Off));
}

static VOID AliveWrite(_In_ PS10T_DEVICE Dev, _In_ ULONG Off, _In_ ULONG Val)
{
    WRITE_REGISTER_ULONG((volatile ULONG *)(Dev->Alive + Off), Val);
}

/* Safe at any IRQL: the ISR and the worker never touch EINT8_MASK at the same time (the ISR masks, only the
   worker unmasks after it has drained the events; the line cannot fire while masked). */
static VOID EintMask(_In_ PS10T_DEVICE Dev, _In_ BOOLEAN Mask)
{
    ULONG v;

    if (Dev->Alive == NULL) {
        return;
    }
    v = AliveRead(Dev, S10T_EINT_MASK);
    v = Mask ? (v | S10T_EINT0_BIT) : (v & ~S10T_EINT0_BIT);
    AliveWrite(Dev, S10T_EINT_MASK, v);
}

static VOID EintAck(_In_ PS10T_DEVICE Dev)
{
    if (Dev->Alive != NULL) {
        AliveWrite(Dev, S10T_EINT_PEND, S10T_EINT0_BIT);   /* write-1-to-clear */
    }
}

static VOID EintConfigure(_In_ PS10T_DEVICE Dev)
{
    ULONG v;

    EintMask(Dev, TRUE);
    v = AliveRead(Dev, S10T_GPA1_CON);
    AliveWrite(Dev, S10T_GPA1_CON, (v & ~S10T_PIN0_FUNC_MASK) | S10T_PIN0_FUNC_EINT);
    v = AliveRead(Dev, S10T_GPA1_PUD);
    AliveWrite(Dev, S10T_GPA1_PUD, v & ~S10T_PIN0_PUD_MASK);               /* the controller drives the line */
    v = AliveRead(Dev, S10T_EINT_CON);
    AliveWrite(Dev, S10T_EINT_CON, (v & ~S10T_EINT0_TRIG_MASK) | S10T_EINT0_LEVEL_LOW);
    v = AliveRead(Dev, S10T_EINT_FLTCON);
    AliveWrite(Dev, S10T_EINT_FLTCON, (v & ~S10T_EINT0_FLT_MASK) | S10T_EINT0_FLT_VALUE);
    EintAck(Dev);
}

static VOID SleepMs(_In_ ULONG Ms)
{
    LARGE_INTEGER t;

    t.QuadPart = -10000LL * (LONGLONG)Ms;
    KeDelayExecutionThread(KernelMode, FALSE, &t);
}

static VOID RegWrite(_In_ WDFDEVICE Device, _In_ PCWSTR Name, _In_ ULONG Value)
{
    WDFKEY         key;
    UNICODE_STRING n;

    if (NT_SUCCESS(WdfDeviceOpenRegistryKey(Device, PLUGPLAY_REGKEY_DEVICE, KEY_SET_VALUE,
                                            WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        RtlInitUnicodeString(&n, Name);
        (VOID)WdfRegistryAssignULong(key, &n, Value);
        WdfRegistryClose(key);
    }
}

static ULONG RegRead(_In_ WDFDEVICE Device, _In_ PCWSTR Name, _In_ ULONG Default)
{
    WDFKEY         key;
    UNICODE_STRING n;
    ULONG          v = Default;

    if (NT_SUCCESS(WdfDeviceOpenRegistryKey(Device, PLUGPLAY_REGKEY_DEVICE, KEY_READ,
                                            WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        RtlInitUnicodeString(&n, Name);
        if (!NT_SUCCESS(WdfRegistryQueryULong(key, &n, &v))) {
            v = Default;
        }
        WdfRegistryClose(key);
    }
    return v;
}

/* ======================================================================
 * SPB (I2C through Exynos9820Hsi2c)
 * ====================================================================== */

static NTSTATUS SpbWrite(_In_ PS10T_DEVICE Dev, _In_reads_(Len) PVOID Buf, _In_ ULONG Len)
{
    WDF_MEMORY_DESCRIPTOR    mem;
    WDF_REQUEST_SEND_OPTIONS opt;
    ULONG_PTR                done = 0;
    NTSTATUS                 status;

    if (!Dev->SpbOpen) {
        return STATUS_DEVICE_NOT_READY;
    }
    if (Dev->Direct) {
        return S10tDirectWrite(Dev, Buf, Len);
    }
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&mem, Buf, Len);
    WDF_REQUEST_SEND_OPTIONS_INIT(&opt, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&opt, WDF_REL_TIMEOUT_IN_MS(SECTS_SPB_TIMEOUT_MS));
    status = WdfIoTargetSendWriteSynchronously(Dev->Spb, NULL, &mem, NULL, &opt, &done);
    if (NT_SUCCESS(status) && done != Len) {
        status = STATUS_DEVICE_DATA_ERROR;
    }
    return status;
}

static NTSTATUS SpbCommand(_In_ PS10T_DEVICE Dev, _In_ UCHAR Cmd)
{
    return SpbWrite(Dev, &Cmd, 1);
}

/* One command byte out, Len bytes back, as one I2C sequence (repeated start). Uses the pre-allocated request,
   so the input path allocates nothing. PASSIVE_LEVEL, one caller at a time (D0Entry before the worker exists,
   then only the worker). */
static NTSTATUS SpbRead(_In_ PS10T_DEVICE Dev, _In_ UCHAR Cmd, _Out_writes_(Len) PVOID Buf, _In_ ULONG Len)
{
    SPB_TRANSFER_LIST_AND_ENTRIES(2) seq;
    WDF_MEMORY_DESCRIPTOR    mem;
    WDF_REQUEST_SEND_OPTIONS opt;
    WDF_REQUEST_REUSE_PARAMS reuse;
    ULONG_PTR                done = 0;
    UCHAR                    cmd = Cmd;
    NTSTATUS                 status;

    if (Dev->SpbOpen && Dev->Direct) {
        ULONGLONG t0 = KeQueryInterruptTime();
        LONG      us;

        status = S10tDirectRead(Dev, Cmd, Buf, Len);
        us = (LONG)((KeQueryInterruptTime() - t0) / 10ULL);
        if (us > Dev->Stats.SpbMaxUs) {
            InterlockedExchange(&Dev->Stats.SpbMaxUs, us);
        }
        if (us > 20000) {
            InterlockedIncrement(&Dev->Stats.SpbSlow);
        }
        return status;
    }
    if (!Dev->SpbOpen || Dev->SpbRequest == WDF_NO_HANDLE) {
        return STATUS_DEVICE_NOT_READY;
    }
    WDF_REQUEST_REUSE_PARAMS_INIT(&reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_SUCCESS);
    status = WdfRequestReuse(Dev->SpbRequest, &reuse);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    SPB_TRANSFER_LIST_INIT(&seq.List, 2);
    seq.List.Transfers[0] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionToDevice, 0, &cmd, 1);
    seq.ExtraTransfers[0] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionFromDevice, 0, Buf, Len);

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&mem, &seq, sizeof(seq));
    WDF_REQUEST_SEND_OPTIONS_INIT(&opt, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&opt, WDF_REL_TIMEOUT_IN_MS(SECTS_SPB_TIMEOUT_MS));
    {
        ULONGLONG t0 = KeQueryInterruptTime();
        LONG      us;

        status = WdfIoTargetSendIoctlSynchronously(Dev->Spb, Dev->SpbRequest, IOCTL_SPB_EXECUTE_SEQUENCE,
                                                   &mem, NULL, &opt, &done);
        us = (LONG)((KeQueryInterruptTime() - t0) / 10ULL);
        if (us > Dev->Stats.SpbMaxUs) {
            InterlockedExchange(&Dev->Stats.SpbMaxUs, us);
        }
        if (us > 20000) {
            InterlockedIncrement(&Dev->Stats.SpbSlow);
        }
    }
    if (NT_SUCCESS(status) && done != (ULONG_PTR)(1 + Len)) {
        status = STATUS_DEVICE_DATA_ERROR;
    }
    return status;
}

static NTSTATUS SpbOpen(_In_ PS10T_DEVICE Dev)
{
    WDF_IO_TARGET_OPEN_PARAMS open;
    WDF_OBJECT_ATTRIBUTES     attr;
    NTSTATUS                  status;
    DECLARE_UNICODE_STRING_SIZE(path, RESOURCE_HUB_PATH_SIZE);

    if (Dev->Direct) {
        status = S10tDirectOpen(Dev);
        Dev->SpbOpen = NT_SUCCESS(status);
        return status;
    }

    if (Dev->Spb == WDF_NO_HANDLE) {
        status = WdfIoTargetCreate(Dev->Device, WDF_NO_OBJECT_ATTRIBUTES, &Dev->Spb);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }

    status = RESOURCE_HUB_CREATE_PATH_FROM_ID(&path, Dev->SpbConnectionId.LowPart, Dev->SpbConnectionId.HighPart);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&open, &path, GENERIC_READ | GENERIC_WRITE);
    open.ShareAccess       = 0;
    open.CreateDisposition = FILE_OPEN;
    open.FileAttributes    = FILE_ATTRIBUTE_NORMAL;
    status = WdfIoTargetOpen(Dev->Spb, &open);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    /* 1.0.6: the reused request is made only once the target is OPEN. Before, it was created right after
       WdfIoTargetCreate: on a target that is not open yet that fails with STATUS_INVALID_DEVICE_STATE (the
       "SpbOpen c0000184" of 1.0.4/1.0.5), the retry then skipped the creation, opened the target and the first
       SpbRead passed a NULL request -> WDF_VIOLATION 0x10D (5 = WDF_INVALID_HANDLE, handle 0). */
    if (Dev->SpbRequest == WDF_NO_HANDLE) {
        WDF_OBJECT_ATTRIBUTES_INIT(&attr);
        attr.ParentObject = Dev->Spb;
        status = WdfRequestCreate(&attr, Dev->Spb, &Dev->SpbRequest);
        if (!NT_SUCCESS(status)) {
            Dev->SpbRequest = WDF_NO_HANDLE;
            WdfIoTargetClose(Dev->Spb);
            return status;
        }
    }
    Dev->SpbOpen = TRUE;
    return status;
}

static VOID SpbClose(_In_ PS10T_DEVICE Dev)
{
    if (Dev->Direct) {
        S10tDirectClose(Dev);
        Dev->SpbOpen = FALSE;
        return;
    }
    if (Dev->SpbOpen) {
        WdfIoTargetClose(Dev->Spb);
        Dev->SpbOpen = FALSE;
    }
}

/* sec_ts_probe order (the sequence SecTsY761Touch runs on this phone): app mode, touch functions, sense on,
   settle, clear the event stack. The rails are switched on by UEFI (Beyond0LteTouchPowerDxe). */
/* Touch function (TOUCH | PALM | WET added to what the chip reports, 2 bytes little-endian as
   sec_ts_set_touch_function), sense on, empty stack. Also what sec_ts_read_event does after a watchdog reboot. */
static NTSTATUS ControllerReinit(_In_ PS10T_DEVICE Dev)
{
    UCHAR    func[2] = { 0, 0 };
    UCHAR    cmd[3];
    NTSTATUS status;
    CHAR     t[48];

    InterlockedIncrement(&Dev->Stats.Reinits);

    if (NT_SUCCESS(SpbRead(Dev, SECTS_CMD_TOUCHFUNCTION, func, sizeof(func)))) {
        cmd[0] = SECTS_CMD_TOUCHFUNCTION;
        cmd[1] = (UCHAR)(func[0] | SECTS_SETFUNC_DEFAULT);
        cmd[2] = func[1];
        (VOID)SpbWrite(Dev, cmd, sizeof(cmd));
    }

    status = SpbCommand(Dev, SECTS_CMD_SENSE_ON);
    (VOID)RtlStringCbPrintfA(t, sizeof(t), "reinit func %02x%02x sense_on %08x", func[1], func[0], (ULONG)status);
    S10tPramRoll(t);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    SleepMs(50);
    return SpbCommand(Dev, SECTS_CMD_CLEAR_EVENT_STACK);
}

/* 1.0.4: runs on the worker thread (not in D0Entry, where a bus that never answers held up the PnP start with
   no trace); every attempt goes to PRAM, and a stop request ends the wait. */
static BOOLEAN StopWait(_In_ PS10T_DEVICE Dev, _In_ ULONG Ms)
{
    LARGE_INTEGER t;

    t.QuadPart = -10000LL * (LONGLONG)Ms;
    return KeWaitForSingleObject(&Dev->StopEvent, Executive, KernelMode, FALSE, &t) == STATUS_SUCCESS;
}

static NTSTATUS ControllerStart(_In_ PS10T_DEVICE Dev)
{
    UCHAR    boot = 0;
    ULONG    tries;
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    CHAR     t[48];

    for (tries = 0; tries < 50; tries++) {
        boot   = 0;
        status = SpbRead(Dev, SECTS_CMD_BOOT_STATUS, &boot, 1);
        if (tries < 3 || tries % 10 == 9 || (NT_SUCCESS(status) && boot == SECTS_STATUS_APP_MODE)) {
            (VOID)RtlStringCbPrintfA(t, sizeof(t), "boot try %u st %08x val %02x", tries, (ULONG)status, boot);
            S10tPramRoll(t);
        }
        if (NT_SUCCESS(status) && boot == SECTS_STATUS_APP_MODE) {
            break;
        }
        if (StopWait(Dev, 20)) {
            return STATUS_CANCELLED;
        }
    }
    RegWrite(Dev->Device, L"S10tBootStatus", NT_SUCCESS(status) ? boot : 0xFFFFFFFFUL);
    RegWrite(Dev->Device, L"S10tBootTries", tries);
    Dev->BootStatus = NT_SUCCESS(status) ? boot : 0xFFFFFFFFUL;
    Dev->BootTries  = tries;
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (boot != SECTS_STATUS_APP_MODE) {
        return STATUS_DEVICE_NOT_READY;
    }

    return ControllerReinit(Dev);
}

/* ======================================================================
 * Frames (worker thread only)
 * ====================================================================== */

static BOOLEAN AnyDown(_In_ PS10T_DEVICE Dev)
{
    ULONG i;

    for (i = 0; i < S10T_MAX_CONTACTS; i++) {
        if (Dev->Contacts[i].Down) {
            return TRUE;
        }
    }
    return FALSE;
}

/* Current contacts -> one frame. Lifted contacts go out once with tip 0, then are forgotten. */
static BOOLEAN BuildFrame(_In_ PS10T_DEVICE Dev, _Out_ S10T_FRAME *F, _In_ BOOLEAN StateChange)
{
    ULONG i;
    ULONG n = 0;

    RtlZeroMemory(F, sizeof(*F));
    F->Report.ReportId = S10T_REPORT_ID;
    F->Report.ScanTime = (USHORT)(KeQueryInterruptTime() / 1000ULL);     /* 100 ns -> 100 us */
    for (i = 0; i < S10T_MAX_CONTACTS; i++) {
        S10T_CONTACT *c = &Dev->Contacts[i];

        if (c->Down || c->Lifted) {
            F->Report.Points[n].Tip       = c->Down ? 1 : 0;
            F->Report.Points[n].ContactId = (UCHAR)i;
            F->Report.Points[n].X         = c->X;
            F->Report.Points[n].Y         = c->Y;
            n++;
            if (c->Lifted) {
                StateChange = TRUE;
                c->Lifted = FALSE;
            }
        }
    }
    F->Report.ContactCount = (UCHAR)n;
    F->MoveOnly = !StateChange;
    return n != 0;
}

/* Into the FIFO. A move-only frame replaces a move-only frame still waiting at the tail (the reader gets the
   newest position, never a backlog); press/release frames always get their own slot. */
static VOID PushFrame(_In_ PS10T_DEVICE Dev, _In_ const S10T_FRAME *F)
{
    ULONG tail;

    if (Dev->FifoCount != 0) {
        tail = (Dev->FifoHead + Dev->FifoCount - 1) % S10T_FIFO_DEPTH;
        if (F->MoveOnly && Dev->Fifo[tail].MoveOnly) {
            Dev->Fifo[tail] = *F;
            InterlockedIncrement(&Dev->Stats.Coalesced);
            return;
        }
    }
    if (Dev->FifoCount == S10T_FIFO_DEPTH) {
        /* nobody is reading: drop the oldest frame */
        Dev->FifoHead = (Dev->FifoHead + 1) % S10T_FIFO_DEPTH;
        Dev->FifoCount--;
        InterlockedIncrement(&Dev->Stats.FifoFull);
    }
    Dev->Fifo[(Dev->FifoHead + Dev->FifoCount) % S10T_FIFO_DEPTH] = *F;
    Dev->FifoCount++;
    InterlockedIncrement(&Dev->Stats.Frames);
}

/* Every finger up, sent at once (I2C failure, controller overflow, stop): a release lost to a bus error would
   otherwise leave a finger "down" forever and turn the next touch into a two-finger gesture. */
static VOID AllUp(_In_ PS10T_DEVICE Dev)
{
    S10T_FRAME f;
    ULONG      i;
    BOOLEAN    any = FALSE;

    for (i = 0; i < S10T_MAX_CONTACTS; i++) {
        if (Dev->Contacts[i].Down) {
            Dev->Contacts[i].Down = FALSE;
            Dev->Contacts[i].Lifted = TRUE;
            any = TRUE;
        }
    }
    if (any && BuildFrame(Dev, &f, TRUE)) {
        PushFrame(Dev, &f);
    }
}

/* Read one batch from the controller and turn it into frames, in event order:
     - a press or a release produces its own frame right away (after flushing the moves seen before it), so a
       quick lift + touch inside one batch is never merged into one moving finger;
     - moves are collected and go out as one frame at the end of the batch.
   Bus errors and stack overflows lift every finger (what SecTsY761Touch did - its 1.0.0.0 rewrite did not, and a
   lost release then made later touches look like a second finger). */
static VOID ReadEvents(_In_ PS10T_DEVICE Dev)
{
    UCHAR     *ev = Dev->Events;
    ULONG      left, i;
    BOOLEAN    moved = FALSE;
    S10T_FRAME f;
    NTSTATUS   status;

    Dev->BatchEmpty = TRUE;
    status = SpbRead(Dev, SECTS_CMD_READ_ONE_EVENT, ev, SECTS_EVENT_SIZE);
    if (!NT_SUCCESS(status)) {
        CHAR t[40];

        (VOID)RtlStringCbPrintfA(t, sizeof(t), "READ_ONE failed %08x streak %u", (ULONG)status, Dev->ErrStreak + 1);
        S10tPramRoll(t);
        InterlockedIncrement(&Dev->Stats.I2cErrors);
        /* 1.0.7: one failed read is not a lift. 1.0.1-1.0.6 lifted every finger on ANY error; the next move event of
           the same finger then came in as a new touch-down -> a tap seen twice, a drag cut into pieces, a double tap
           where the drag started (user report, 2026-11-03). sec_ts_read_event simply drops the batch. Lift only when
           the bus keeps failing, so a finger cannot stay stuck down. */
        if (++Dev->ErrStreak == S10T_ERROR_LIFT_STREAK) {
            InterlockedIncrement(&Dev->Stats.ErrorLifts);
            S10tPramRoll("bus keeps failing: all up");
            AllUp(Dev);
        }
        return;
    }
    Dev->ErrStreak = 0;
    if (ev[0] == 0) {
        return;
    }
    Dev->BatchEmpty = FALSE;
    left = ev[7] & SECTS_LEFT_EVENTS_MASK;
    if (left > SECTS_MAX_EVENTS - 1) {
        S10tPramRoll("overflow: stack cleared");
        InterlockedIncrement(&Dev->Stats.InvalidEvents);
        AllUp(Dev);
        (VOID)SpbCommand(Dev, SECTS_CMD_CLEAR_EVENT_STACK);
        return;
    }
    if (left != 0) {
        /* 1.0.7: the rest of the stack one event (8 bytes) at a time with READ_ONE_EVENT, which pops an event
           ("repeat READ_ONE_EVENT until buffer is empty", sec_ts_read_event), instead of one READ_ALL of left x 8
           bytes. Every multi-finger batch needs the long read - one finger moving rarely does - and that is exactly
           where touch fell apart (3 fingers dragging = a tap at every step): reads longer than the HSI2C RX trigger
           (8 bytes) go through the controller's FIFO refill path, the 8-byte read is the one proven in UEFI and
           Windows. */
        ULONG k;

        for (k = 1; k <= left; k++) {
            status = SpbRead(Dev, SECTS_CMD_READ_ONE_EVENT, ev + k * SECTS_EVENT_SIZE, SECTS_EVENT_SIZE);
            if (!NT_SUCCESS(status) || ev[k * SECTS_EVENT_SIZE] == 0) {
                if (!NT_SUCCESS(status)) {
                    CHAR t[40];

                    (VOID)RtlStringCbPrintfA(t, sizeof(t), "READ_ONE %u/%u failed %08x", k, left, (ULONG)status);
                    S10tPramRoll(t);
                    InterlockedIncrement(&Dev->Stats.I2cErrors);
                }
                break;                              /* keep what was read, no lift */
            }
        }
        left = k - 1;
    }

    for (i = 0; i <= left; i++) {
        UCHAR *e = ev + i * SECTS_EVENT_SIZE;
        ULONG  eid = e[0] & SECTS_EID_MASK;
        CHAR   t[48];

        if (e[0] == 0) {
            break;
        }
        InterlockedIncrement(&Dev->Stats.Events);
        /* 1.0.7: every press / release / status event, but only 1 move in 16, so the 60 lines span whole gestures */
        if (eid != SECTS_EID_COORDINATE || (e[0] >> SECTS_STATE_SHIFT) != SECTS_STATE_MOVE || (Dev->Stats.Events & 15) == 0) {
            (VOID)RtlStringCbPrintfA(t, sizeof(t), "ev %02x %02x %02x %02x %02x %02x %02x %02x L%u/%u",
                                     e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7], i, left);
            S10tPramRoll(t);
        }
        if (eid == SECTS_EID_COORDINATE) {
            ULONG tid   = (e[0] & SECTS_TID_MASK) >> 2;
            ULONG state = e[0] >> SECTS_STATE_SHIFT;
            S10T_CONTACT *c;
            BOOLEAN wasDown;
            ULONG ttype = ((ULONG)(e[6] >> 6) << 2) | (e[7] >> 6);

            if (tid == 0 || tid > S10T_MAX_CONTACTS || state == 0) {
                continue;
            }
            /* sec_ts_read_event only reports NORMAL / GLOVE / PALM / WET contacts; HOVER, FLIP COVER, STYLUS
               and PROXIMITY coordinates are not fingers on the glass (a hovering hand made ghost touches). */
            if (state != SECTS_STATE_RELEASE && ttype != 0 && ttype != 3 && ttype != 5 && ttype != 6) {
                InterlockedIncrement(&Dev->Stats.InvalidEvents);
                continue;
            }
            c = &Dev->Contacts[tid - 1];
            wasDown = c->Down;
            Dev->LastEventTime = KeQueryInterruptTime();

            if (state == SECTS_STATE_RELEASE) {
                if (!wasDown) {
                    continue;
                }
                if (moved && BuildFrame(Dev, &f, FALSE)) {      /* the moves that came before this lift */
                    PushFrame(Dev, &f);
                }
                moved = FALSE;
                c->Down = FALSE;
                c->Lifted = TRUE;
                if (BuildFrame(Dev, &f, TRUE)) {
                    PushFrame(Dev, &f);
                }
                (VOID)RtlStringCbPrintfA(t, sizeof(t), "F- id%u %u,%u", tid - 1, c->X, c->Y);
                S10tPramRoll(t);
                continue;
            }

            /* press or move */
            if (!wasDown) {
                if (moved && BuildFrame(Dev, &f, FALSE)) {
                    PushFrame(Dev, &f);
                }
                moved = FALSE;
            }
            c->Down   = TRUE;
            c->Lifted = FALSE;
            c->X = (USHORT)(((ULONG)e[1] << 4) | (e[3] >> 4));
            c->Y = (USHORT)(((ULONG)e[2] << 4) | (e[3] & 0x0F));
            if (!wasDown) {
                if (BuildFrame(Dev, &f, TRUE)) {               /* the touch-down gets its own frame */
                    PushFrame(Dev, &f);
                }
                (VOID)RtlStringCbPrintfA(t, sizeof(t), "F+ id%u %u,%u st%u tt%u", tid - 1, c->X, c->Y, state, ttype);
                S10tPramRoll(t);
            } else {
                moved = TRUE;
            }
        } else if (eid == SECTS_EID_STATUS) {
            ULONG stype = (e[0] >> SECTS_STYPE_SHIFT) & SECTS_STYPE_MASK;

            if (stype == SECTS_STYPE_INFO && e[1] == SECTS_ACK_BOOT_COMPLETE && e[2] == SECTS_STATUS_APP_MODE) {
                /* the chip rebooted (watchdog/ESD): every finger is gone and it needs sense on again */
                InterlockedIncrement(&Dev->Stats.InvalidEvents);
                if (moved && BuildFrame(Dev, &f, FALSE)) {
                    PushFrame(Dev, &f);
                }
                moved = FALSE;
                S10tPramRoll("status: chip rebooted -> all up, reinit");
                AllUp(Dev);
                Dev->NeedReinit = TRUE;
            } else if (stype == SECTS_STYPE_ERR && e[1] == SECTS_ERR_QUEUE_FULL) {
                if (moved && BuildFrame(Dev, &f, FALSE)) {
                    PushFrame(Dev, &f);
                }
                moved = FALSE;
                S10tPramRoll("status: queue full -> all up");
                AllUp(Dev);
            }
        } else if (eid != SECTS_EID_GESTURE) {
            /* EID 3 = invalid: sec_ts_read_event clears the stack and drops the rest of the batch */
            InterlockedIncrement(&Dev->Stats.InvalidEvents);
            (VOID)SpbCommand(Dev, SECTS_CMD_CLEAR_EVENT_STACK);
            break;
        }
    }
    if (moved && BuildFrame(Dev, &f, FALSE)) {
        PushFrame(Dev, &f);
    }
}

static VOID UpdateLatency(_In_ PS10T_DEVICE Dev)
{
    LONGLONG t0 = InterlockedCompareExchange64(&Dev->IrqTime, 0, 0);
    LONG     us;

    if (t0 == 0) {
        return;
    }
    us = (LONG)((KeQueryInterruptTime() - (ULONGLONG)t0) / 10ULL);
    if (us > Dev->Stats.MaxIrqToReportUs && us < 10000000) {
        InterlockedExchange(&Dev->Stats.MaxIrqToReportUs, us);
    }
}

/* ======================================================================
 * The worker thread
 * ====================================================================== */

static VOID Heartbeat(_In_ PS10T_DEVICE Dev)
{
    CHAR      t[56];
    ULONGLONG now = KeQueryInterruptTime();

    if (now - Dev->LastHeartbeat < 10000000ULL) {
        return;
    }
    Dev->LastHeartbeat = now;
    (VOID)RtlStringCbPrintfA(t, sizeof(t), "irq%u ev%u fr%u dl%u rd%u i2c%u inv%u dr%u",
                             Dev->Stats.Irqs, Dev->Stats.Events, Dev->Stats.Frames, Dev->Stats.Delivered,
                             Dev->Stats.ReadRequests, Dev->Stats.I2cErrors, Dev->Stats.InvalidEvents,
                             Dev->Stats.DirectRetries);
    S10tPramLine(2, t);
    (VOID)RtlStringCbPrintfA(t, sizeof(t), "ff%u co%u rp%u re%u el%u lv%u m%x p%x %uus s%u/%u",
                             Dev->Stats.FifoFull, Dev->Stats.Coalesced, Dev->Stats.Repeats, Dev->Stats.Reinits,
                             Dev->Stats.ErrorLifts, Dev->Alive ? (AliveRead(Dev, S10T_GPA1_CON + 4) & 1) : 9,
                             Dev->Alive ? AliveRead(Dev, S10T_EINT_MASK) & 0xFF : 0,
                             Dev->Alive ? AliveRead(Dev, S10T_EINT_PEND) & 0xFF : 0, Dev->Stats.MaxIrqToReportUs,
                             Dev->Stats.SpbMaxUs, Dev->Stats.SpbSlow);
    S10tPramLine(3, t);
}

static KSTART_ROUTINE WorkerMain;

static VOID WorkerMain(_In_ PVOID Context)
{
    PS10T_DEVICE  Dev = (PS10T_DEVICE)Context;
    PVOID         objs[3];
    LARGE_INTEGER timeout;
    NTSTATUS      w;

    (VOID)KeSetPriorityThread(KeGetCurrentThread(), LOW_REALTIME_PRIORITY + 8);

    objs[0] = &Dev->StopEvent;
    objs[1] = &Dev->IrqEvent;
    objs[2] = &Dev->KickEvent;

    {
        NTSTATUS st;
        CHAR     t[56];
        ULONG    tries;

        for (tries = 0;; tries++) {
            st = SpbOpen(Dev);
            if (NT_SUCCESS(st) || tries < 3 || tries % 20 == 19) {
                (VOID)RtlStringCbPrintfA(t, sizeof(t), "%s open try %u st %08x clk %x", Dev->Direct ? "direct" : "spb",
                                         tries, (ULONG)st, Dev->DirectClock);
                S10tPramRoll(t);
            }
            if (NT_SUCCESS(st)) {
                break;
            }
            /* 100 ms apart for 10 s, then once a second for as long as the device is up */
            if (StopWait(Dev, tries < 100 ? 100 : 1000)) {
                S10tPramRoll("worker: stopped before SPB open");
                PsTerminateSystemThread(STATUS_SUCCESS);
            }
            Heartbeat(Dev);
        }
        RegWrite(Dev->Device, L"S10tSpbOpenStatus", (ULONG)st);
        RegWrite(Dev->Device, L"S10tSpbOpenTries", tries);

        S10tPramRoll("worker: controller start");
        st = ControllerStart(Dev);
        RegWrite(Dev->Device, L"S10tStartStatus", (ULONG)st);
        (VOID)RtlStringCbPrintfA(t, sizeof(t), "start boot %02x tries %u st %08x rpt %u",
                                 Dev->BootStatus, Dev->BootTries, (ULONG)st, Dev->RepeatMs);
        S10tPramLine(1, t);
        S10tPramRoll(t);
        /* events that came in while starting: read them now (the EINT is unmasked since PostInterruptsEnabled) */
        KeSetEvent(&Dev->IrqEvent, IO_NO_INCREMENT, FALSE);
    }

    for (;;) {
        /* the repeat clock runs while a finger is down and the controller spoke within S10T_REPEAT_LIMIT_MS;
           after that the contact simply stays down (no phantom refresh) until the controller reports again */
        BOOLEAN repeatDue = AnyDown(Dev) && Dev->RepeatMs != 0 &&
                            (KeQueryInterruptTime() - Dev->LastEventTime) < (ULONGLONG)S10T_REPEAT_LIMIT_MS * 10000ULL;

        /* 1.0.3: never wait forever - a 1 s tick keeps the PRAM heartbeat alive */
        timeout.QuadPart = -10000LL * (LONGLONG)(repeatDue ? Dev->RepeatMs : 1000);
        w = KeWaitForMultipleObjects(3, objs, WaitAny, Executive, KernelMode, FALSE, &timeout, NULL);
        Heartbeat(Dev);
        if (w == STATUS_WAIT_0) {
            break;
        }

        if (w == STATUS_WAIT_1) {
            /* attention: drain the controller, one frame per batch, then unmask (level line: if the controller
               still holds it low, the ISR fires again right away and we come back here) */
            ReadEvents(Dev);
            if (Dev->NeedReinit) {
                Dev->NeedReinit = FALSE;
                (VOID)ControllerReinit(Dev);
            }
            S10tDeliver(Dev);
            UpdateLatency(Dev);
            /* A line that stays low with nothing to read (or a bus that keeps failing) would bring us straight
               back here at real-time priority: back off, up to 50 ms, after three such attentions in a row. */
            if (Dev->BatchEmpty) {
                if (++Dev->EmptyStreak > 3) {
                    SleepMs(Dev->EmptyStreak > 13 ? 50 : 5 * (Dev->EmptyStreak - 3));
                }
            } else {
                Dev->EmptyStreak = 0;
            }
            EintAck(Dev);
            EintMask(Dev, FALSE);
        } else if (w == STATUS_TIMEOUT) {
            /* a finger is down and the controller has been quiet for RepeatMs: same frame, new scan time */
            ULONGLONG quiet = KeQueryInterruptTime() - Dev->LastEventTime;
            S10T_FRAME f;

            if (quiet >= 2ULL * Dev->RepeatMs * 10000ULL &&
                quiet < (ULONGLONG)S10T_REPEAT_LIMIT_MS * 10000ULL &&
                BuildFrame(Dev, &f, FALSE)) {
                PushFrame(Dev, &f);
                InterlockedIncrement(&Dev->Stats.Repeats);
            }
            S10tDeliver(Dev);
        } else {
            /* a read request arrived: hand it whatever is waiting */
            S10tDeliver(Dev);
        }
    }

    AllUp(Dev);
    S10tDeliver(Dev);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

VOID S10tKickWorker(_In_ PS10T_DEVICE Dev)
{
    KeSetEvent(&Dev->KickEvent, IO_NO_INCREMENT, FALSE);
}

static NTSTATUS WorkerStart(_In_ PS10T_DEVICE Dev)
{
    HANDLE   h;
    NTSTATUS status;

    KeClearEvent(&Dev->StopEvent);
    KeClearEvent(&Dev->IrqEvent);
    KeClearEvent(&Dev->KickEvent);
    RtlZeroMemory(Dev->Contacts, sizeof(Dev->Contacts));
    Dev->FifoHead = Dev->FifoCount = 0;
    Dev->LastEventTime = KeQueryInterruptTime();

    status = PsCreateSystemThread(&h, THREAD_ALL_ACCESS, NULL, NULL, NULL, WorkerMain, Dev);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = ObReferenceObjectByHandle(h, THREAD_ALL_ACCESS, *PsThreadType, KernelMode, (PVOID *)&Dev->Worker, NULL);
    ZwClose(h);
    if (!NT_SUCCESS(status)) {
        /* the thread runs but cannot be waited for: stop it and give up */
        KeSetEvent(&Dev->StopEvent, IO_NO_INCREMENT, FALSE);
        SleepMs(50);
        Dev->Worker = NULL;
    }
    return status;
}

static VOID WorkerStop(_In_ PS10T_DEVICE Dev)
{
    if (Dev->Worker != NULL) {
        KeSetEvent(&Dev->StopEvent, IO_NO_INCREMENT, FALSE);
        (VOID)KeWaitForSingleObject(Dev->Worker, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(Dev->Worker);
        Dev->Worker = NULL;
    }
}

/* ======================================================================
 * Interrupt
 * ====================================================================== */

BOOLEAN S10tEvtIsr(_In_ WDFINTERRUPT Interrupt, _In_ ULONG MessageID)
{
    PS10T_DEVICE Dev = S10tGet(WdfInterruptGetDevice(Interrupt));

    UNREFERENCED_PARAMETER(MessageID);
    if (Dev->Alive == NULL) {
        return FALSE;
    }
    /* Level line: mask EINT8 until the worker has drained the controller. */
    EintMask(Dev, TRUE);
    EintAck(Dev);
    InterlockedIncrement(&Dev->Stats.Irqs);
    (VOID)InterlockedExchange64(&Dev->IrqTime, (LONGLONG)KeQueryInterruptTime());
    WdfInterruptQueueDpcForIsr(Interrupt);
    return TRUE;
}

VOID S10tEvtDpc(_In_ WDFINTERRUPT Interrupt, _In_ WDFOBJECT AssociatedObject)
{
    PS10T_DEVICE Dev = S10tGet(WdfInterruptGetDevice(Interrupt));

    UNREFERENCED_PARAMETER(AssociatedObject);
    KeSetEvent(&Dev->IrqEvent, IO_NO_INCREMENT, FALSE);
}

/* ======================================================================
 * PnP / power
 * ====================================================================== */

NTSTATUS S10tEvtPrepareHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Raw, _In_ WDFCMRESLIST Translated)
{
    PS10T_DEVICE Dev = S10tGet(Device);
    ULONG        i, n = WdfCmResourceListGetCount(Translated);
    BOOLEAN      spb = FALSE, alive = FALSE;
    LONG         irq = -1;
    PHYSICAL_ADDRESS alivePa;
    WDF_INTERRUPT_CONFIG ic;
    NTSTATUS     status;

    S10tPramInit();
    alivePa.QuadPart = 0;
    for (i = 0; i < n; i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR d = WdfCmResourceListGetDescriptor(Translated, i);

        if (d == NULL) {
            continue;
        }
        switch (d->Type) {
        case CmResourceTypeConnection:
            if (!spb && d->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_SERIAL &&
                d->u.Connection.Type == CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C) {
                Dev->SpbConnectionId.LowPart  = d->u.Connection.IdLowPart;
                Dev->SpbConnectionId.HighPart = d->u.Connection.IdHighPart;
                spb = TRUE;
            }
            break;
        case CmResourceTypeInterrupt:
            if (irq < 0) {
                irq = (LONG)i;
            }
            break;
        case CmResourceTypeMemory:
            if ((ULONGLONG)d->u.Memory.Start.QuadPart == S10T_ALIVE_PA && d->u.Memory.Length >= S10T_ALIVE_LEN) {
                alivePa = d->u.Memory.Start;
                alive = TRUE;
            }
            break;
        default:
            break;
        }
    }
    RegWrite(Device, L"S10tResources", (spb ? 1 : 0) | (irq >= 0 ? 2 : 0) | (alive ? 4 : 0));
    {
        CHAR t[48];

        (VOID)RtlStringCbPrintfA(t, sizeof(t), "PrepareHardware res %u (need 7) n %u", (spb ? 1 : 0) | (irq >= 0 ? 2 : 0) | (alive ? 4 : 0), n);
        S10tPramRoll(t);
    }
    if (!spb || irq < 0 || !alive) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;   /* needs the S10 DSDT TSP0 (UEFI v0.1.2+) */
    }

    Dev->Alive = (PUCHAR)MmMapIoSpaceEx(alivePa, S10T_ALIVE_LEN, PAGE_READWRITE | PAGE_NOCACHE);
    if (Dev->Alive == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    EintMask(Dev, TRUE);

    if (Dev->Interrupt == WDF_NO_HANDLE) {
        WDF_INTERRUPT_CONFIG_INIT(&ic, S10tEvtIsr, S10tEvtDpc);
        ic.InterruptRaw        = WdfCmResourceListGetDescriptor(Raw, (ULONG)irq);
        ic.InterruptTranslated = WdfCmResourceListGetDescriptor(Translated, (ULONG)irq);
        status = WdfInterruptCreate(Device, &ic, WDF_NO_OBJECT_ATTRIBUTES, &Dev->Interrupt);
        if (!NT_SUCCESS(status)) {
            S10tPramRoll("WdfInterruptCreate FAILED");
            RegWrite(Device, L"S10tInterruptStatus", (ULONG)status);
            return status;
        }
    }

    Dev->Direct   = RegRead(Device, L"DirectI2c", 1) != 0;   /* 1.1.0: 0 = through SpbCx / Exynos9820Hsi2c */
    Dev->RepeatMs = RegRead(Device, L"RepeatMs", S10T_REPEAT_MS_DEFAULT);
    if (Dev->RepeatMs > 100) {
        Dev->RepeatMs = 100;
    }
    return STATUS_SUCCESS;
}

NTSTATUS S10tEvtReleaseHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Translated)
{
    PS10T_DEVICE Dev = S10tGet(Device);

    UNREFERENCED_PARAMETER(Translated);
    if (Dev->Alive != NULL) {
        EintMask(Dev, TRUE);
        MmUnmapIoSpace(Dev->Alive, S10T_ALIVE_LEN);
        Dev->Alive = NULL;
    }
    /* an interrupt created in PrepareHardware is deleted by the framework after this callback */
    Dev->Interrupt = WDF_NO_HANDLE;
    S10tPramFree();
    return STATUS_SUCCESS;
}

NTSTATUS S10tEvtD0Entry(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE Previous)
{
    PS10T_DEVICE Dev = S10tGet(Device);
    NTSTATUS     status;

    UNREFERENCED_PARAMETER(Previous);
    S10tPramRoll("D0Entry");
    EintConfigure(Dev);                         /* masked until D0EntryPostInterruptsEnabled */

    /* 1.0.5: the SPB target is opened by the worker, with retries: 1.0.4 showed "SpbOpen c0000184"
       (STATUS_INVALID_DEVICE_STATE) - the I2C controller was not ready (still starting, or idle in D3) at the
       one moment D0Entry asked, and a failed D0Entry left the touch device dead for the whole session. */

    status = WorkerStart(Dev);
    RegWrite(Device, L"S10tWorkerStatus", (ULONG)status);
    return status;
}

NTSTATUS S10tEvtD0EntryPostInterruptsEnabled(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE Previous)
{
    PS10T_DEVICE Dev = S10tGet(Device);

    UNREFERENCED_PARAMETER(Previous);
    S10tPramRoll("interrupts on");
    EintAck(Dev);
    EintMask(Dev, FALSE);
    return STATUS_SUCCESS;
}

NTSTATUS S10tEvtD0ExitPreInterruptsDisabled(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE Target)
{
    PS10T_DEVICE Dev = S10tGet(Device);

    UNREFERENCED_PARAMETER(Target);
    EintMask(Dev, TRUE);
    EintAck(Dev);
    return STATUS_SUCCESS;
}

NTSTATUS S10tEvtD0Exit(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE Target)
{
    PS10T_DEVICE Dev = S10tGet(Device);

    UNREFERENCED_PARAMETER(Target);
    WorkerStop(Dev);
    SpbClose(Dev);
    S10tPramRoll("D0Exit");

    RegWrite(Device, L"S10tIrqs", (ULONG)Dev->Stats.Irqs);
    RegWrite(Device, L"S10tI2cErrors", (ULONG)Dev->Stats.I2cErrors);
    RegWrite(Device, L"S10tInvalidEvents", (ULONG)Dev->Stats.InvalidEvents);
    RegWrite(Device, L"S10tFrames", (ULONG)Dev->Stats.Frames);
    RegWrite(Device, L"S10tRepeats", (ULONG)Dev->Stats.Repeats);
    RegWrite(Device, L"S10tCoalesced", (ULONG)Dev->Stats.Coalesced);
    RegWrite(Device, L"S10tDelivered", (ULONG)Dev->Stats.Delivered);
    RegWrite(Device, L"S10tFifoFull", (ULONG)Dev->Stats.FifoFull);
    RegWrite(Device, L"S10tMaxIrqToReportUs", (ULONG)Dev->Stats.MaxIrqToReportUs);
    return STATUS_SUCCESS;
}

NTSTATUS S10tEvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT Init)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES        attr;
    WDFDEVICE                    device;
    PS10T_DEVICE                 Dev;
    NTSTATUS                     status;

    UNREFERENCED_PARAMETER(Driver);

    WdfFdoInitSetFilter(Init);                  /* mshidkmdf above us; unknown IRPs go down */

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware              = S10tEvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware              = S10tEvtReleaseHardware;
    pnp.EvtDeviceD0Entry                      = S10tEvtD0Entry;
    pnp.EvtDeviceD0Exit                       = S10tEvtD0Exit;
    pnp.EvtDeviceD0EntryPostInterruptsEnabled = S10tEvtD0EntryPostInterruptsEnabled;
    pnp.EvtDeviceD0ExitPreInterruptsDisabled  = S10tEvtD0ExitPreInterruptsDisabled;
    WdfDeviceInitSetPnpPowerEventCallbacks(Init, &pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, S10T_DEVICE);
    status = WdfDeviceCreate(&Init, &attr, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    Dev = S10tGet(device);
    RtlZeroMemory(Dev, sizeof(*Dev));
    Dev->Device     = device;
    Dev->Interrupt  = WDF_NO_HANDLE;
    Dev->Spb        = WDF_NO_HANDLE;
    Dev->SpbRequest = WDF_NO_HANDLE;
    Dev->RepeatMs   = S10T_REPEAT_MS_DEFAULT;
    KeInitializeEvent(&Dev->IrqEvent, SynchronizationEvent, FALSE);
    KeInitializeEvent(&Dev->KickEvent, SynchronizationEvent, FALSE);
    KeInitializeEvent(&Dev->StopEvent, NotificationEvent, FALSE);

    return S10tQueuesCreate(device, Dev);
}

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;

    WDF_DRIVER_CONFIG_INIT(&config, S10tEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}
