/*
 * S10eCamera.c - Galaxy S10 (beyond1lte) front camera as a Windows camera: an AVStream capture minidriver.
 *
 * The capture engine (sensor power + setfile, D-PHY, CSIS2, VC0 DMA into a 3-buffer ring, finished frames found by a
 * canary, exposure in a group hold) is S10eCam's, validated on the phone 2026-11-16 (notes/s10-camera.md): this file
 * includes S10eCam.c with S10ECAM_ENGINE_ONLY and adds the AVStream side only.
 *
 * One filter (KSCATEGORY_VIDEO + CAPTURE + VIDEO_CAMERA), one capture pin, one format: YUY2 @30 fps, 684 x 912
 * (portrait: rotation 90 / 270) or 912 x 684 (0 / 180). A frame = the newest finished raw frame (1824 x 1368 GRBG
 * RAW10-in-16), 2 x 2 Bayer binned, black level 64 subtracted, gray-world white balance, gamma 1/2.2, rotated
 * clockwise + optionally mirrored left-right (after rotating), BT.601 limited range. Auto exposure (lines first, then
 * analog gain) runs on the same sums. Frames are pushed: the engine's poll thread calls KsPinAttemptProcessing after
 * each finished raw frame; the pin does not let AVStream initiate processing (KSPIN_FLAG_DO_NOT_INITIATE_PROCESSING),
 * so a frame without an empty buffer waiting is dropped (counted in KS_FRAME_INFO.DropCount).
 * The camera runs only in KSSTATE_RUN: PAUSE -> RUN starts the engine (~1 s: the sensor setfile over I2C), RUN ->
 * PAUSE and pin close stop it and power everything down.
 * 0.2 settings (Services\S10eCamera\Parameters, DWORDs, read at each device start; camera-s10.ps1 set):
 *   Rotation 0/90/180/270 (default 270: upright on the phone held upright, 2026-11-16), Mirror 0/1 (default 0),
 *   RingWc 0/1 (default 1: the frame ring write-combined instead of non-cached - 0.6's non-cached copy ran at
 *   ~80 MB/s, 60 ms a frame).
 * 0.3: every KS call into the driver (device start, filter create/close, data intersection, pin create / format /
 *   state / close) is traced to C:\S10Prov\camera-avs.txt with its result: the Camera app's DXGI_ERROR_UNSUPPORTED
 *   (2026-11-16) left no start report, so how far a client gets is the first thing to know.
 * 0.4: the trace showed every client reaching PAUSE -> RUN and CamStart failing with "no contiguous 5056000 bytes
 *   below 4 GB" (~75 min after boot). The 3 frame buffers are now allocated ONCE at device start (boot, memory still
 *   unfragmented) into S10eCam's gPool and lent to every stream; freed at device stop / remove. If that allocation
 *   fails, each start still tries its own, as before.
 * 0.5: setting RingKeep (default 1; 0 = 0.3's per-start allocation) - a DIAGNOSIS switch: with 0.4 the clients closed
 *   the filter right after the data intersection, before any pin (2026-11-16 18:18), where with 0.3 they reached
 *   RUN; RingKeep 0 tells whether the 15 MiB held below 4 GB is what they missed.
 *
 * Written from the WDK documentation (learn.microsoft.com/windows-hardware/drivers/stream/...); the avshws sample
 * was read for the descriptor layout only, no code is taken from it.
 * SPDX-License-Identifier: GPL-2.0
 */

#define S10ECAM_ENGINE_ONLY
#pragma warning(disable : 4505)   /* engine functions this driver does not call (stages 2-4, StreamFrame) */
#include "..\S10eCam\S10eCam.c"

#include <windef.h>
#include <unknown.h>
#include <ks.h>
#include <ksmedia.h>
#include "S10eCameraLut.h"

#define CAM_SW          (S10ECAM_SHOT_W / 2)             /* 912: the binned image, sensor orientation */
#define CAM_SH          (S10ECAM_SHOT_H / 2)             /* 684 */
#define CAM_BYTES       (CAM_SW * CAM_SH * 2)            /* YUY2: 2 bytes a pixel, whichever way round */
#define CAM_INTERVAL    333333                           /* 100 ns units: 30 fps (the sensor's mode 8) */
#define CAM_AE_TARGET   110u                             /* black-subtracted green mean (of 959): mid grey after gamma */
#define CAM_TAG         'mCeS'

/* YUY2 = FOURCC 'YUY2' as a media subtype GUID */
#define STATIC_S10_SUBTYPE_YUY2 0x32595559L, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71
#define S10_FOURCC_YUY2         0x32595559u

typedef struct _CAMPIN {
    LONG    LastSeq;        /* engine frame number last delivered (or dropped) */
    ULONG64 Frames;         /* delivered */
    ULONG64 Drops;          /* raw frames with no buffer waiting */
    ULONG64 Torn;           /* delivered frames whose ring slot the DMA started over during the conversion */
    ULONG64 ConvUs;         /* conversion time: sum ... */
    ULONG   ConvUsMax;      /* ... and worst */
    ULONG   WbR, WbB;       /* white balance gains, Q8 (256 = x1) */
    ULONG   AeWait;         /* frames to wait before the next AE decision */
    BOOLEAN Running;
} CAMPIN;

static FAST_MUTEX gCamMutex;                             /* Process vs start / stop (the ring must not go away) */
static UCHAR      gRows[4 * S10ECAM_SHOT_STRIDE];        /* 4 raw lines, cached (the ring is uncached); image = nonpaged */
static UCHAR      gY[2][CAM_SW], gU[2][CAM_SW], gV[2][CAM_SW];   /* 2 binned rows as Y, U, V (under gCamMutex) */
static CHAR       gLog[8192];                            /* the engine's report of the last start / stop */
static ULONG      gRot = 270, gMirror, gRingWc = 1, gRingKeep = 1;   /* settings (Parameters), read at device start */
static ULONG      gOutW = CAM_SH, gOutH = CAM_SW;        /* the output picture: rotation 90/270 = portrait */
static WCHAR      gParamPath[260];                       /* <service key>\Parameters */

/* ---------------------------------------------------------------------------------------------- the report file */

/* C:\S10Prov\camera-avs.txt: the engine's start/stop report (what cam-s10.ps1 shows for S10eCam). PASSIVE_LEVEL. */
static VOID WriteReport(PCSTR Text, size_t Len)
{
    UNICODE_STRING    name;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK   io;
    HANDLE            h;

    RtlInitUnicodeString(&name, L"\\??\\C:\\S10Prov\\camera-avs.txt");
    InitializeObjectAttributes(&oa, &name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (NT_SUCCESS(ZwCreateFile(&h, FILE_APPEND_DATA | SYNCHRONIZE, &oa, &io, NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ,
                                FILE_OPEN_IF, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0))) {
        (VOID)ZwWriteFile(h, NULL, NULL, NULL, &io, (PVOID)Text, (ULONG)Len, NULL, NULL);
        ZwClose(h);
    }
}

/* One traced KS call: "[ms since boot] text". PASSIVE_LEVEL (every KS dispatch traced here runs there). */
static VOID Trace(PCSTR Fmt, ...)
{
    CHAR    b[512];
    REPORT  r = { b, sizeof(b), 0 };
    PSTR    end = NULL;
    va_list a;

    b[0] = 0;
    Log(&r, "[%I64u ms] ", (ULONG64)KeQueryInterruptTime() / 10000);
    va_start(a, Fmt);
    (VOID)RtlStringCbVPrintfExA(b + r.Used, sizeof(b) - r.Used, &end, NULL, STRSAFE_IGNORE_NULLS, Fmt, a);
    va_end(a);
    if (end != NULL)                                     /* also when truncated (STATUS_BUFFER_OVERFLOW) */
        r.Used = (size_t)(end - b);
    WriteReport(b, r.Used);
}

/* ---------------------------------------------------------------------------------------------- conversion */

static UCHAR Clamp8(LONG v)
{
    return (UCHAR)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

/* Binned source pixel (sx, sy) (912 x 684, sensor orientation) -> output (ox, oy): clockwise rotation, then the
   left-right mirror. Same convention as cam-s10.ps1 v1.5 (its Develop: rotate 270 takes the source's top-left corner to
   the picture's bottom-left, checked 2026-11-16). */
static VOID MapOut(ULONG sx, ULONG sy, ULONG *ox, ULONG *oy)
{
    switch (gRot) {
    case 90:  *ox = CAM_SH - 1 - sy; *oy = sx;               break;
    case 180: *ox = CAM_SW - 1 - sx; *oy = CAM_SH - 1 - sy;  break;
    case 270: *ox = sy;              *oy = CAM_SW - 1 - sx;  break;
    default:  *ox = sx;              *oy = sy;               break;
    }
    if (gMirror)
        *ox = gOutW - 1 - *ox;
}

/* Two source pixels that land side by side on one output row = one YUY2 macropixel (Y0 U Y1 V): A and B are
   horizontal neighbours (sx even, sx + 1) for rotation 0/180 and vertical ones (sy even, sy + 1) for 90/270; the
   output widths (912 / 684) are even, so either way they fill one macropixel, also mirrored. */
static VOID PutPair(PUCHAR Out, ULONG sxa, ULONG sya, ULONG ka, ULONG sxb, ULONG syb, ULONG kb)
{
    ULONG  oxa, oya, oxb, oyb;
    PUCHAR p;

    MapOut(sxa, sya, &oxa, &oya);
    MapOut(sxb, syb, &oxb, &oyb);                            /* oyb == oya, oxb == oxa ^ 1 */
    p = Out + ((SIZE_T)oya * gOutW + (oxa & ~1u)) * 2;
    p[(oxa & 1u) * 2] = gY[ka][sxa];
    p[(oxb & 1u) * 2] = gY[kb][sxb];
    p[1] = (UCHAR)((gU[ka][sxa] + gU[kb][sxb] + 1u) / 2u);
    p[3] = (UCHAR)((gV[ka][sxa] + gV[kb][sxb] + 1u) / 2u);
}

/* Ring[i] (+ its saved tail) -> YUY2, 4 raw lines (= 2 binned rows) at a time. Sums of the black-subtracted channels
   for AE / WB. */
static VOID Convert(ULONG i, PUCHAR Out, ULONG64 *SumG, ULONG64 *SumR, ULONG64 *SumB, ULONG WbR, ULONG WbB)
{
    ULONG   j, k, bx;
    ULONG64 sg = 0, sr = 0, sb = 0;

    for (j = 0; j < CAM_SH / 2; j++) {
        RtlCopyMemory(gRows, gS.x.Ring[i] + (SIZE_T)(4 * j) * S10ECAM_SHOT_STRIDE, 4 * S10ECAM_SHOT_STRIDE);
        if (j == CAM_SH / 2 - 1)                             /* the canary's 8 bytes: the frame's own values */
            RtlCopyMemory(gRows + 4 * S10ECAM_SHOT_STRIDE - 8, gS.Tail[i], 8);
        for (k = 0; k < 2; k++) {
            PUSHORT r0 = (PUSHORT)(gRows + (2 * k) * S10ECAM_SHOT_STRIDE);
            PUSHORT r1 = (PUSHORT)(gRows + (2 * k + 1) * S10ECAM_SHOT_STRIDE);

            for (bx = 0; bx < CAM_SW; bx++) {
                ULONG x = 2 * bx;
                LONG  g = (LONG)(((r0[x] & 0x3FFu) + (r1[x + 1] & 0x3FFu)) / 2u) - 64;   /* G R / B G */
                LONG  r = (LONG)(r0[x + 1] & 0x3FFu) - 64;
                LONG  b = (LONG)(r1[x] & 0x3FFu) - 64;
                LONG  R, G, B;

                if (g < 0) g = 0;
                if (r < 0) r = 0;
                if (b < 0) b = 0;
                sg += (ULONG)g; sr += (ULONG)r; sb += (ULONG)b;
                r = (LONG)(((ULONG)r * WbR) >> 8);
                b = (LONG)(((ULONG)b * WbB) >> 8);
                R = gGamma[r > 1023 ? 1023 : r];
                G = gGamma[g > 1023 ? 1023 : g];
                B = gGamma[b > 1023 ? 1023 : b];
                gY[k][bx] = Clamp8(((66 * R + 129 * G + 25 * B + 128) >> 8) + 16);     /* BT.601, limited range */
                gU[k][bx] = Clamp8(((-38 * R - 74 * G + 112 * B + 128) >> 8) + 128);
                gV[k][bx] = Clamp8(((112 * R - 94 * G - 18 * B + 128) >> 8) + 128);
            }
        }
        if (gRot == 90 || gRot == 270) {
            for (bx = 0; bx < CAM_SW; bx++)
                PutPair(Out, bx, 2 * j, 0, bx, 2 * j + 1, 1);
        } else {
            for (k = 0; k < 2; k++)
                for (bx = 0; bx < CAM_SW; bx += 2)
                    PutPair(Out, bx, 2 * j + k, k, bx + 1, 2 * j + k, k);
        }
    }
    *SumG = sg; *SumR = sr; *SumB = sb;
}

/* E = lines x gain / 32 toward CAM_AE_TARGET (factor clamped 1/4 .. 4); lines up to the maximum first, then gain. */
static VOID AutoExposure(CAMPIN *c, ULONG MeanG)
{
    ULONG64 e, e2;
    ULONG   lines, gain;
    S10ECAM_EXPOSURE_IN x;

    if (c->AeWait) {                                     /* a change shows 1-2 frames after it is written */
        c->AeWait--;
        return;
    }
    if (MeanG * 100 > CAM_AE_TARGET * 88 && MeanG * 100 < CAM_AE_TARGET * 112)
        return;                                          /* within 12 % */
    e = (ULONG64)gS.Coarse * gS.AGain;                   /* x32 */
    if (MeanG == 0)
        e2 = e * 4;
    else {
        e2 = e * CAM_AE_TARGET / MeanG;
        if (e2 > e * 4) e2 = e * 4;
        if (e2 < e / 4) e2 = e / 4;
    }
    lines = (ULONG)(e2 / 32 > gS.CoarseMax ? gS.CoarseMax : (e2 / 32 < 2 ? 2 : e2 / 32));
    gain = (ULONG)(e2 / lines);
    x.Coarse = lines;
    x.AGain = gain;
    if (NT_SUCCESS(StreamExposure(&x)))                  /* clamps to the sensor's gain range itself */
        c->AeWait = 3;
}

/* ---------------------------------------------------------------------------------------------- pin */

static VOID OnFrame(PVOID Ctx)                           /* the engine's poll thread, PASSIVE_LEVEL */
{
    KsPinAttemptProcessing((PKSPIN)Ctx, TRUE);
}

static NTSTATUS PinProcess(PKSPIN Pin)
{
    CAMPIN            *c = (CAMPIN *)Pin->Context;
    PKSSTREAM_POINTER  lead;
    PKSSTREAM_HEADER   hdr;
    PIKSREFERENCECLOCK clock;
    LONG               seq, i;
    ULONG64            sg, sr, sb, n = (ULONG64)CAM_SW * CAM_SH;
    LARGE_INTEGER      t0, t1, fq;
    ULONG              us;

    ExAcquireFastMutex(&gCamMutex);
    if (!c->Running || !gS.Active) {
        ExReleaseFastMutex(&gCamMutex);
        return STATUS_PENDING;
    }
    seq = gS.Seq;
    i = gS.Latest;
    if (seq == c->LastSeq || i < 0) {
        ExReleaseFastMutex(&gCamMutex);
        return STATUS_PENDING;
    }
    lead = KsPinGetLeadingEdgeStreamPointer(Pin, KSSTREAM_POINTER_STATE_LOCKED);
    if (lead == NULL) {                                  /* nobody has a buffer waiting: this frame is dropped */
        c->Drops += (ULONG)(seq - c->LastSeq);
        c->LastSeq = seq;
        ExReleaseFastMutex(&gCamMutex);
        return STATUS_PENDING;
    }
    if (lead->OffsetOut.Remaining < CAM_BYTES) {         /* a buffer smaller than a frame: return it empty */
        KsStreamPointerAdvanceOffsetsAndUnlock(lead, 0, 0, TRUE);
        c->LastSeq = seq;
        ExReleaseFastMutex(&gCamMutex);
        return STATUS_PENDING;
    }
    t0 = KeQueryPerformanceCounter(&fq);
    Convert((ULONG)i, (PUCHAR)lead->OffsetOut.Data, &sg, &sr, &sb, c->WbR, c->WbB);
    t1 = KeQueryPerformanceCounter(NULL);
    us = (ULONG)((t1.QuadPart - t0.QuadPart) * 1000000 / fq.QuadPart);
    c->ConvUs += us;
    if (us > c->ConvUsMax)
        c->ConvUsMax = us;
    if (gS.Seq - seq >= 2)                               /* as S10eCam's StreamFrame: the slot started over meanwhile */
        c->Torn++;
    if (seq - c->LastSeq > 1)
        c->Drops += (ULONG)(seq - c->LastSeq - 1);
    c->LastSeq = seq;
    c->Frames++;

    hdr = lead->StreamHeader;
    hdr->Duration = CAM_INTERVAL;
    hdr->OptionsFlags |= KSSTREAM_HEADER_OPTIONSF_DURATIONVALID;
    if (NT_SUCCESS(KsPinGetReferenceClockInterface(Pin, &clock))) {
        hdr->PresentationTime.Time = clock->lpVtbl->GetTime(clock);
        hdr->PresentationTime.Numerator = hdr->PresentationTime.Denominator = 1;
        hdr->OptionsFlags |= KSSTREAM_HEADER_OPTIONSF_TIMEVALID;
        clock->lpVtbl->Release(clock);
    }
    if (hdr->Size >= sizeof(KSSTREAM_HEADER) + sizeof(KS_FRAME_INFO)) {
        PKS_FRAME_INFO fi = (PKS_FRAME_INFO)(hdr + 1);
        fi->ExtendedHeaderSize = sizeof(KS_FRAME_INFO);
        fi->dwFrameFlags = KS_VIDEO_FLAG_FRAME;
        fi->PictureNumber = (LONGLONG)c->Frames;
        fi->DropCount = (LONGLONG)c->Drops;
    }
    KsStreamPointerAdvanceOffsetsAndUnlock(lead, 0, CAM_BYTES, TRUE);

    /* gray-world white balance (smoothed, x1/4 .. x4) and auto exposure from this frame */
    if (sr > 0 && sb > 0) {
        ULONG64 wr = sg * 256 / sr, wb = sg * 256 / sb;
        wr = wr < 64 ? 64 : (wr > 1024 ? 1024 : wr);
        wb = wb < 64 ? 64 : (wb > 1024 ? 1024 : wb);
        c->WbR = (ULONG)((c->WbR * 3 + wr) / 4);
        c->WbB = (ULONG)((c->WbB * 3 + wb) / 4);
    }
    AutoExposure(c, (ULONG)(sg / n));
    ExReleaseFastMutex(&gCamMutex);
    return STATUS_PENDING;
}

static NTSTATUS CamStart(PKSPIN Pin, CAMPIN *c)
{
    S10ECAM_SHOT_IN in;
    REPORT          r = { gLog, sizeof(gLog), 0 };
    NTSTATUS        s;

    RtlZeroMemory(&in, sizeof(in));                      /* setfile exposure, ISPPRE 400 MHz (the defaults) */
    if (gRingWc)
        in.Flags |= S10ECAM_SHOT_WC;
    gLog[0] = 0;
    Log(&r, "S10eCamera 0.5 start: output %u x %u, rotation %u%s, ring %s\r\n", gOutW, gOutH, gRot,
        gMirror ? " + mirror" : "", gRingWc ? "write-combined" : "non-cached");
    ExAcquireFastMutex(&gCamMutex);
    c->LastSeq = 0;
    c->Frames = c->Drops = c->Torn = c->ConvUs = 0;
    c->ConvUsMax = 0;
    c->WbR = c->WbB = 256;
    c->AeWait = 2;
    ExReleaseFastMutex(&gCamMutex);
    /* not under gCamMutex: StreamStart sleeps for ~1 s and calls back into OnFrame -> Process, which takes it */
    s = StreamStart(&in, NULL, OnFrame, Pin, &r);
    Log(&r, "start: 0x%08X\r\n", s);
    if (NT_SUCCESS(s)) {
        ExAcquireFastMutex(&gCamMutex);
        c->Running = TRUE;
        ExReleaseFastMutex(&gCamMutex);
    }
    WriteReport(gLog, r.Used);
    return s;
}

static VOID CamStop(CAMPIN *c)
{
    REPORT r = { gLog, sizeof(gLog), 0 };
    BOOLEAN active;

    ExAcquireFastMutex(&gCamMutex);
    c->Running = FALSE;                                  /* Process returns at once from here on */
    active = gS.Active;
    ExReleaseFastMutex(&gCamMutex);
    if (!active)
        return;
    gLog[0] = 0;
    Log(&r, "S10eCamera 0.5 stop: %I64u frames delivered, %I64u dropped, %I64u torn; conversion avg %I64u us, max %u us\r\n",
        c->Frames, c->Drops, c->Torn, c->Frames ? c->ConvUs / c->Frames : 0, c->ConvUsMax);
    /* StreamStop waits for the poll thread (whose OnFrame only queues work) and then frees the ring: take the mutex so
       a Process that already got in finishes first */
    ExAcquireFastMutex(&gCamMutex);
    ExReleaseFastMutex(&gCamMutex);
    StreamStop(&r);
    WriteReport(gLog, r.Used);
}

static NTSTATUS PinCreate(PKSPIN Pin, PIRP Irp)
{
    PKS_DATAFORMAT_VIDEOINFOHEADER f = (PKS_DATAFORMAT_VIDEOINFOHEADER)Pin->ConnectionFormat;
    CAMPIN                         *c;

    UNREFERENCED_PARAMETER(Irp);
    if (Pin->ConnectionFormat->FormatSize < sizeof(KS_DATAFORMAT_VIDEOINFOHEADER) ||
        f->VideoInfoHeader.bmiHeader.biWidth != (LONG)gOutW || f->VideoInfoHeader.bmiHeader.biHeight != (LONG)gOutH ||
        f->VideoInfoHeader.bmiHeader.biCompression != S10_FOURCC_YUY2) {
        Trace("pin create: format size %u, subtype %08X, %d x %d, compression %08X -> NO_MATCH\r\n",
              Pin->ConnectionFormat->FormatSize, Pin->ConnectionFormat->SubFormat.Data1,
              Pin->ConnectionFormat->FormatSize >= sizeof(KS_DATAFORMAT_VIDEOINFOHEADER) ? f->VideoInfoHeader.bmiHeader.biWidth : 0,
              Pin->ConnectionFormat->FormatSize >= sizeof(KS_DATAFORMAT_VIDEOINFOHEADER) ? f->VideoInfoHeader.bmiHeader.biHeight : 0,
              Pin->ConnectionFormat->FormatSize >= sizeof(KS_DATAFORMAT_VIDEOINFOHEADER) ? f->VideoInfoHeader.bmiHeader.biCompression : 0);
        return STATUS_NO_MATCH;
    }
    c = (CAMPIN *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(CAMPIN), CAM_TAG);
    if (c == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    Pin->Context = c;
    Trace("pin create: YUY2 %u x %u ok\r\n", gOutW, gOutH);
    return STATUS_SUCCESS;
}

static NTSTATUS PinClose(PKSPIN Pin, PIRP Irp)
{
    CAMPIN *c = (CAMPIN *)Pin->Context;

    UNREFERENCED_PARAMETER(Irp);
    Trace("pin close\r\n");
    if (c != NULL) {
        CamStop(c);
        ExFreePoolWithTag(c, CAM_TAG);
        Pin->Context = NULL;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS PinSetDeviceState(PKSPIN Pin, KSSTATE To, KSSTATE From)
{
    CAMPIN  *c = (CAMPIN *)Pin->Context;
    NTSTATUS s = STATUS_SUCCESS;

    if (To == KSSTATE_RUN && From == KSSTATE_PAUSE)
        s = CamStart(Pin, c);
    else if (From == KSSTATE_RUN)
        CamStop(c);
    Trace("pin state %d -> %d: 0x%08X\r\n", (int)From, (int)To, s);   /* 0 STOP 1 ACQUIRE 2 PAUSE 3 RUN */
    return s;
}

static NTSTATUS PinSetDataFormat(PKSPIN Pin, PKSDATAFORMAT OldFormat, PKSMULTIPLE_ITEM OldAttributeList,
                                 const KSDATARANGE *DataRange, const KSATTRIBUTE_LIST *AttributeRange)
{
    PKS_DATAFORMAT_VIDEOINFOHEADER f = (PKS_DATAFORMAT_VIDEOINFOHEADER)Pin->ConnectionFormat;

    UNREFERENCED_PARAMETER(OldFormat);
    UNREFERENCED_PARAMETER(OldAttributeList);
    UNREFERENCED_PARAMETER(DataRange);
    UNREFERENCED_PARAMETER(AttributeRange);
    if (Pin->ConnectionFormat->FormatSize < sizeof(KS_DATAFORMAT_VIDEOINFOHEADER) ||
        f->VideoInfoHeader.bmiHeader.biWidth != (LONG)gOutW || f->VideoInfoHeader.bmiHeader.biHeight != (LONG)gOutH ||
        f->VideoInfoHeader.bmiHeader.biCompression != S10_FOURCC_YUY2) {
        Trace("pin set format: refused (size %u)\r\n", Pin->ConnectionFormat->FormatSize);
        return STATUS_NO_MATCH;
    }
    Trace("pin set format: ok\r\n");
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------------------------- descriptors */

/* Built for the portrait default; SetRangeSize writes the size the settings give at device start (no pin exists then). */
static KS_DATARANGE_VIDEO gRangeYuy2 = {
    {   sizeof(KS_DATARANGE_VIDEO), 0, CAM_BYTES, 0,
        { STATIC_KSDATAFORMAT_TYPE_VIDEO }, { STATIC_S10_SUBTYPE_YUY2 }, { STATIC_KSDATAFORMAT_SPECIFIER_VIDEOINFO } },
    TRUE,                                                /* bFixedSizeSamples */
    FALSE,                                               /* bTemporalCompression */
    KS_VIDEOSTREAM_CAPTURE,                              /* StreamDescriptionFlags */
    0,                                                   /* MemoryAllocationFlags */
    {   { STATIC_KSDATAFORMAT_SPECIFIER_VIDEOINFO },
        KS_AnalogVideo_None,
        { CAM_SH, CAM_SW }, { CAM_SH, CAM_SW }, { CAM_SH, CAM_SW },   /* input, min / max cropping */
        1, 1, 1, 1,                                      /* crop granularity / alignment */
        { CAM_SH, CAM_SW }, { CAM_SH, CAM_SW },          /* min / max output */
        1, 1, 0, 0, 0, 0,                                /* output granularity, stretch / shrink taps */
        CAM_INTERVAL, CAM_INTERVAL,                      /* min / max frame interval */
        CAM_BYTES * 8 * 30, CAM_BYTES * 8 * 30 },        /* min / max bits per second */
    {   { 0, 0, 0, 0 }, { 0, 0, 0, 0 },                  /* rcSource, rcTarget */
        CAM_BYTES * 8 * 30, 0, CAM_INTERVAL,             /* bit rate, bit error rate, AvgTimePerFrame */
        { sizeof(KS_BITMAPINFOHEADER), CAM_SH, CAM_SW, 1, 16, S10_FOURCC_YUY2, CAM_BYTES, 0, 0, 0, 0 } }
};

static VOID SetRangeSize(ULONG W, ULONG H)
{
    KS_VIDEO_STREAM_CONFIG_CAPS *c = &gRangeYuy2.ConfigCaps;
    SIZE                         z;

    z.cx = (LONG)W;
    z.cy = (LONG)H;
    c->InputSize = c->MinCroppingSize = c->MaxCroppingSize = c->MinOutputSize = c->MaxOutputSize = z;
    gRangeYuy2.VideoInfoHeader.bmiHeader.biWidth = (LONG)W;
    gRangeYuy2.VideoInfoHeader.bmiHeader.biHeight = (LONG)H;
}

static const PKSDATARANGE gRanges[] = { (PKSDATARANGE)&gRangeYuy2 };

/* AVStream asks the pin to turn our (single) range into the format a client connects with: KSDATAFORMAT + header. */
static NTSTATUS PinIntersect(PVOID Context, PIRP Irp, PKSP_PIN Pin, PKSDATARANGE DataRange, PKSDATARANGE MatchingDataRange,
                             ULONG DataBufferSize, PVOID Data, PULONG DataSize)
{
    const KS_DATARANGE_VIDEO       *ours = (const KS_DATARANGE_VIDEO *)MatchingDataRange;
    PKS_DATAFORMAT_VIDEOINFOHEADER  f = (PKS_DATAFORMAT_VIDEOINFOHEADER)Data;

    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Irp);
    UNREFERENCED_PARAMETER(Pin);
    /* AVStream passes ranges that already "match in major type, subformat, and specifier" (Data Range Intersections in
       AVStream); 0.3 also takes the wildcard specifier (0.1/0.2 refused it). Our range is one fixed format, so the
       client range's own values are not needed. */
    Trace("intersect: client specifier %08X size %u, buffer %u\r\n", DataRange->Specifier.Data1, DataRange->FormatSize,
          DataBufferSize);
    if (MatchingDataRange != (PKSDATARANGE)&gRangeYuy2 ||
        (!IsEqualGUID(&DataRange->Specifier, &KSDATAFORMAT_SPECIFIER_VIDEOINFO) &&
         !IsEqualGUID(&DataRange->Specifier, &KSDATAFORMAT_SPECIFIER_WILDCARD)))
        return STATUS_NO_MATCH;
    *DataSize = sizeof(KS_DATAFORMAT_VIDEOINFOHEADER);
    if (DataBufferSize == 0)
        return STATUS_BUFFER_OVERFLOW;
    if (DataBufferSize < sizeof(KS_DATAFORMAT_VIDEOINFOHEADER))
        return STATUS_BUFFER_TOO_SMALL;
    RtlCopyMemory(&f->DataFormat, &ours->DataRange, sizeof(KSDATAFORMAT));
    f->DataFormat.FormatSize = sizeof(KS_DATAFORMAT_VIDEOINFOHEADER);
    f->DataFormat.SampleSize = CAM_BYTES;
    RtlCopyMemory(&f->VideoInfoHeader, &ours->VideoInfoHeader, sizeof(KS_VIDEOINFOHEADER));
    return STATUS_SUCCESS;
}

static const KSPIN_DISPATCH gPinDispatch = {
    PinCreate, PinClose, PinProcess, NULL /* Reset */, PinSetDataFormat, PinSetDeviceState,
    NULL /* Connect */, NULL /* Disconnect */, NULL /* Clock */, NULL /* Allocator */
};

DECLARE_SIMPLE_FRAMING_EX(gFraming, STATICGUIDOF(KSMEMORY_TYPE_KERNEL_NONPAGED),
                          KSALLOCATOR_REQUIREMENTF_SYSTEM_MEMORY | KSALLOCATOR_REQUIREMENTF_PREFERENCES_ONLY,
                          2, 0, CAM_BYTES, CAM_BYTES);

/* PINNAME_VIDEO_CAPTURE (ksmedia.h) and DirectShow's PIN_CATEGORY_CAPTURE (uuids.h, user-mode) are the same GUID,
   FB6C4281-0353-11d1-905F-0000C0CC16BA: the pin's category and its name. */
static const GUID gPinName = { STATIC_PINNAME_VIDEO_CAPTURE };

static const KSPIN_DESCRIPTOR_EX gPins[] = {
    {   &gPinDispatch, NULL,
        {   0, NULL, 0, NULL,
            SIZEOF_ARRAY(gRanges), gRanges,
            KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_BOTH,
            &gPinName, &gPinName, 0 },
        KSPIN_FLAG_PROCESS_IN_RUN_STATE_ONLY | KSPIN_FLAG_DO_NOT_INITIATE_PROCESSING,
        1, 1, &gFraming, (PFNKSINTERSECTHANDLEREX)PinIntersect }
};

static const GUID gCategories[] = {
    { STATICGUIDOF(KSCATEGORY_VIDEO) }, { STATICGUIDOF(KSCATEGORY_CAPTURE) }, { STATICGUIDOF(KSCATEGORY_VIDEO_CAMERA) }
};

static NTSTATUS FilterCreate(PKSFILTER Filter, PIRP Irp)
{
    UNREFERENCED_PARAMETER(Filter);
    UNREFERENCED_PARAMETER(Irp);
    Trace("filter create (a client opened the camera)\r\n");
    return STATUS_SUCCESS;
}

static NTSTATUS FilterClose(PKSFILTER Filter, PIRP Irp)
{
    UNREFERENCED_PARAMETER(Filter);
    UNREFERENCED_PARAMETER(Irp);
    Trace("filter close\r\n");
    return STATUS_SUCCESS;
}

static const KSFILTER_DISPATCH gFilterDispatch = { FilterCreate, FilterClose, NULL, NULL };

static const KSFILTER_DESCRIPTOR gFilter = {
    &gFilterDispatch, NULL, KSFILTER_DESCRIPTOR_VERSION, 0, &KSNAME_Filter,
    DEFINE_KSFILTER_PIN_DESCRIPTORS(gPins),
    DEFINE_KSFILTER_CATEGORIES(gCategories),
    0, sizeof(KSNODE_DESCRIPTOR), NULL, 0, NULL, NULL
};

static const KSFILTER_DESCRIPTOR *const gFilters[] = { &gFilter };

/* ---------------------------------------------------------------------------------------------- settings */

static ULONG RegDword(HANDLE Key, PCWSTR Name, ULONG Default)
{
    UNICODE_STRING                 n;
    ULONG                          buf[(sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG) + 3) / 4], got;
    PKEY_VALUE_PARTIAL_INFORMATION v = (PKEY_VALUE_PARTIAL_INFORMATION)buf;

    RtlInitUnicodeString(&n, Name);
    if (Key != NULL && NT_SUCCESS(ZwQueryValueKey(Key, &n, KeyValuePartialInformation, v, sizeof(buf), &got)) &&
        v->Type == REG_DWORD && v->DataLength == sizeof(ULONG))
        return *(ULONG UNALIGNED *)v->Data;
    return Default;
}

/* Parameters -> gRot / gMirror / gRingWc and the output size; at each device start (PnP start: also after
   camera-s10.ps1 set's restart), before any filter or pin of this start exists. */
static NTSTATUS DeviceStart(PKSDEVICE Device, PIRP Irp, PCM_RESOURCE_LIST Translated, PCM_RESOURCE_LIST Untranslated)
{
    UNICODE_STRING    p;
    OBJECT_ATTRIBUTES oa;
    HANDLE            h = NULL;
    ULONG             rot;

    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(Irp);
    UNREFERENCED_PARAMETER(Translated);
    UNREFERENCED_PARAMETER(Untranslated);
    RtlInitUnicodeString(&p, gParamPath);
    InitializeObjectAttributes(&oa, &p, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (p.Length == 0 || !NT_SUCCESS(ZwOpenKey(&h, KEY_READ, &oa)))
        h = NULL;
    rot = RegDword(h, L"Rotation", 270);
    gRot = (rot == 0 || rot == 90 || rot == 180 || rot == 270) ? rot : 270;
    gMirror = RegDword(h, L"Mirror", 0) ? 1u : 0u;
    gRingWc = RegDword(h, L"RingWc", 1) ? 1u : 0u;
    gRingKeep = RegDword(h, L"RingKeep", 1) ? 1u : 0u;
    if (h != NULL)
        ZwClose(h);
    gOutW = (gRot == 90 || gRot == 270) ? CAM_SH : CAM_SW;
    gOutH = (gRot == 90 || gRot == 270) ? CAM_SW : CAM_SH;
    SetRangeSize(gOutW, gOutH);
    if (!gRingKeep) {
        RingPoolFree();
        Trace("frame ring: not kept (RingKeep 0): each start allocates its own\r\n");
    } else if (RingPoolAlloc(gRingWc ? TRUE : FALSE))
        Trace("frame ring kept for the device's lifetime: 3 x %u bytes at PA %08X %08X %08X (%s)\r\n", SHOT_RING_BYTES,
              (ULONG)gPool.Pa[0].QuadPart, (ULONG)gPool.Pa[1].QuadPart, (ULONG)gPool.Pa[2].QuadPart,
              gPool.Cache == MmWriteCombined ? "write-combined" : "non-cached");
    else
        Trace("frame ring: no 3 x %u bytes contiguous below 4 GB now; each start will try its own\r\n", SHOT_RING_BYTES);
    Trace("S10eCamera 0.5 device start: YUY2 %u x %u, rotation %u%s, ring %s\r\n", gOutW, gOutH, gRot,
          gMirror ? " + mirror" : "", gRingWc ? "write-combined" : "non-cached");
    return STATUS_SUCCESS;
}

/* PnP stop / remove: no pin exists then (KS closes them first), so no stream holds the ring. */
static VOID DeviceStop(PKSDEVICE Device, PIRP Irp)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(Irp);
    RingPoolFree();
    Trace("device stop: frame ring freed\r\n");
}

static const KSDEVICE_DISPATCH gDeviceDispatch = {
    NULL /* Add */, DeviceStart, NULL /* PostStart */, NULL /* QueryStop */, NULL /* CancelStop */, DeviceStop,
    NULL /* QueryRemove */, NULL /* CancelRemove */, DeviceStop /* Remove */
};

static const KSDEVICE_DESCRIPTOR gDevice = { &gDeviceDispatch, SIZEOF_ARRAY(gFilters), gFilters };

DRIVER_INITIALIZE DriverEntry;

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    UNICODE_STRING p;

    p.Buffer = gParamPath;
    p.Length = 0;
    p.MaximumLength = sizeof(gParamPath) - sizeof(WCHAR);   /* stays NUL-terminated (gParamPath is zeroed) */
    if (!NT_SUCCESS(RtlAppendUnicodeStringToString(&p, RegistryPath)) ||
        !NT_SUCCESS(RtlAppendUnicodeToString(&p, L"\\Parameters")))
        gParamPath[0] = 0;                               /* -> the defaults */
    ExInitializeFastMutex(&gCamMutex);
    KeInitializeEvent(&gLock, SynchronizationEvent, TRUE);
    return KsInitializeDriver(DriverObject, RegistryPath, &gDevice);
}
