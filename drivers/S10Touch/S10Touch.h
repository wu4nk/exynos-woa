/*
 * S10Touch - touchscreen driver for the Galaxy S10 (beyond1lte, SM-G973F): Samsung sec_ts "y771"
 * (S6SY761 command family; Linux driver sec_ts/y771) on HSI2C10 behind \_SB.I2C0 (Exynos9820Hsi2c), attention line gpa1-0 = ALIVE EINT8
 * -> GIC SPI 13 (GSIV 45, level). Written from scratch for the S10 kit (notes/s10-touch-driver.md).
 *
 * Shape: KMDF HID minidriver under mshidkmdf (the vhidmini2 KMDF pattern: our service is the function
 * driver, mshidkmdf the upper filter, WdfFdoInitSetFilter so unknown IRPs pass down).
 *
 * Input path (the part that is new):
 *   ISR (DIRQL)        mask EINT8, clear its pending bit, queue the DPC                       ~1 us
 *   DPC                KeSetEvent(IrqEvent)
 *   worker thread      real-time priority (LOW_REALTIME_PRIORITY + 8): reads the event stack over SPB
 *                      with a pre-allocated request, updates the contact table, builds ONE frame per
 *                      batch, completes the waiting HID read itself, unmasks EINT8
 *   repeat             while a finger is down and the controller is quiet for RepeatMs, the same thread
 *                      re-sends the current frame (the y761 only reports changes; Windows' gesture engine
 *                      expects a frame every scan, like any HID-I2C touchscreen)
 * Only the worker thread touches the contact table and the frame FIFO: no locks on the input path.
 * The read IOCTL only forwards the request to the manual queue and kicks the worker.
 *
 * SPDX-License-Identifier: GPL-2.0 (report layout and controller command set follow Morc's S6SY761Touch and
 * the Linux s6sy761 / Samsung sec_ts drivers, both GPL-2.0).
 */
#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <hidport.h>
#include <spb.h>
#include <ntstrsafe.h>
#define RESHUB_USE_HELPER_ROUTINES      /* reshub.h only defines RESOURCE_HUB_CREATE_PATH_FROM_ID with this */
#include <reshub.h>

#define S10T_TAG                    'T01S'

/* ---- ALIVE GPIO page (\_SB.TSP0 _CRS Memory32Fixed 0x15850000 + 0x1000), gpa1-0 = EINT8 ----
 * Values as the SecTsY761Touch 0.3.x driver that runs on this phone (Exynos 9820 ALIVE pinctrl layout:
 * bank stride 0x20, EXT_INT_CON 0x700 + 4n, FLTCON 0x800 + 8n, MASK 0x900 + 4n, PEND 0xA00 + 4n, gpa1 = n 1). */
#define S10T_ALIVE_PA               0x15850000ULL
#define S10T_ALIVE_LEN              0x1000UL
#define S10T_GPA1_CON               0x020UL
#define S10T_GPA1_PUD               0x028UL
#define S10T_EINT_CON               0x704UL
#define S10T_EINT_FLTCON            0x808UL
#define S10T_EINT_MASK              0x904UL
#define S10T_EINT_PEND              0xA04UL
#define S10T_PIN0_FUNC_MASK         0x0000000FUL
#define S10T_PIN0_FUNC_EINT         0x0000000FUL
#define S10T_PIN0_PUD_MASK          0x0000000FUL
#define S10T_EINT0_TRIG_MASK        0x0000000FUL
#define S10T_EINT0_LEVEL_LOW        0x00000000UL
#define S10T_EINT0_FLT_MASK         0x000000FFUL
#define S10T_EINT0_FLT_VALUE        0x000000C0UL   /* filter enable, digital filter */
#define S10T_EINT0_BIT              0x00000001UL

/* ---- sec_ts command set (y761 and y771 share it) (Linux s6sy761.c / Samsung sec_ts.h) ---- */
#define SECTS_CMD_SENSE_ON          0x10
#define SECTS_CMD_TOUCHFUNCTION     0x30
#define SECTS_CMD_BOOT_STATUS       0x55
#define SECTS_CMD_READ_ONE_EVENT    0x60
#define SECTS_CMD_READ_ALL_EVENTS   0x61
#define SECTS_CMD_CLEAR_EVENT_STACK 0x62
#define SECTS_STATUS_APP_MODE       0x20
#define SECTS_SETFUNC_DEFAULT       0x61           /* TOUCH | PALM | WET, as sec_ts_probe */
#define SECTS_EVENT_SIZE            8
/* The S10 panel is a y771 (beyond1lte_defconfig: CONFIG_TOUCHSCREEN_SEC_TS_Y771=y), not a y761: byte 7 of an
   event is left_event:5 | max_energy_flag:1 | ttype_1_0:2 (sec_ts/y771/sec_ts.h struct sec_ts_event_coordinate),
   so the count is 5 bits (sec_ts_read_event: & 0x1F) and bit 5 is a flag. 1.0.0/1.0.1 masked 0x3F: every event
   with max_energy_flag set asked READ_ALL for 32 extra events -> stale stack contents replayed as touches. */
#define SECTS_LEFT_EVENTS_MASK      0x1F
#define SECTS_MAX_EVENTS            31             /* sec_ts.c MAX_EVENT_COUNT */
#define SECTS_STYPE_SHIFT           2              /* status event: eid:2 stype:4 sf:2 */
#define SECTS_STYPE_MASK            0x0F
#define SECTS_STYPE_ERR             1              /* TYPE_STATUS_EVENT_ERR */
#define SECTS_STYPE_INFO            2              /* TYPE_STATUS_EVENT_INFO */
#define SECTS_ACK_BOOT_COMPLETE     0x00           /* status_id; status_data_1 0x20 = rebooted into app mode (watchdog) */
#define SECTS_ERR_QUEUE_FULL        0x01
#define SECTS_EID_MASK              0x03
#define SECTS_EID_COORDINATE        0x00
#define SECTS_EID_STATUS            0x01
#define SECTS_EID_GESTURE           0x02
#define SECTS_TID_MASK              0x3C
#define SECTS_STATE_SHIFT           6
#define SECTS_STATE_PRESS           1
#define SECTS_STATE_MOVE            2
#define SECTS_STATE_RELEASE         3
#define SECTS_COORD_MAX             0x0FFF         /* live DT sec,max_coords = <0x1000 0x1000> */
#define SECTS_SPB_TIMEOUT_MS        250            /* as SecTsY761Touch */

/* ---- HID ---- */
#define S10T_REPORT_ID              0x54
#define S10T_MAX_CONTACTS           10
#define S10T_VID                    0x04E8         /* Samsung */
#define S10T_PID                    0x0761
#define S10T_VERSION                0x0100
#define S10T_FIFO_DEPTH             32
#define S10T_REPEAT_MS_DEFAULT      10             /* repeat period once the controller has been quiet 2 periods */
#define S10T_REPEAT_LIMIT_MS        3000           /* stop repeating after 3 s without controller events */

#include <pshpack1.h>
typedef struct _S10T_POINT {
    UCHAR  Tip;                 /* bit0 tip switch, 7 bits padding */
    UCHAR  ContactId;
    USHORT X;
    USHORT Y;
} S10T_POINT;

typedef struct _S10T_REPORT {
    UCHAR      ReportId;
    S10T_POINT Points[S10T_MAX_CONTACTS];
    USHORT     ScanTime;        /* 100 us units */
    UCHAR      ContactCount;
} S10T_REPORT;
#include <poppack.h>

C_ASSERT(sizeof(S10T_POINT) == 6);
C_ASSERT(sizeof(S10T_REPORT) == 64);

typedef struct _S10T_CONTACT {
    BOOLEAN Down;
    BOOLEAN Lifted;             /* released since the last frame: reported once with tip 0 */
    USHORT  X;
    USHORT  Y;
} S10T_CONTACT;

typedef struct _S10T_FRAME {
    S10T_REPORT Report;
    BOOLEAN     MoveOnly;       /* may be overwritten by a newer frame (no press/release in it) */
} S10T_FRAME;

typedef struct _S10T_STATS {
    volatile LONG Irqs;
    volatile LONG I2cErrors;
    volatile LONG InvalidEvents;
    volatile LONG Frames;
    volatile LONG Repeats;
    volatile LONG Coalesced;
    volatile LONG Delivered;
    volatile LONG FifoFull;
    volatile LONG MaxIrqToReportUs;
    volatile LONG ReadRequests;     /* 1.0.3: IOCTL_HID_READ_REPORTs from hidclass (0 = nobody reads us) */
    volatile LONG Reinits;
    volatile LONG Events;           /* raw controller events read */
} S10T_STATS;

typedef struct _S10T_DEVICE {
    WDFDEVICE       Device;
    WDFQUEUE        ReadQueue;          /* manual: pending IOCTL_HID_READ_REPORT */
    WDFINTERRUPT    Interrupt;
    WDFIOTARGET     Spb;
    WDFREQUEST      SpbRequest;         /* reused for every write-read (worker thread only) */
    LARGE_INTEGER   SpbConnectionId;
    PUCHAR          Alive;              /* ALIVE GPIO page, uncached */
    BOOLEAN         SpbOpen;

    /* worker */
    PKTHREAD        Worker;
    KEVENT          IrqEvent;           /* synchronization: DPC -> worker */
    KEVENT          KickEvent;          /* synchronization: new read request -> worker */
    KEVENT          StopEvent;          /* notification */
    volatile LONGLONG IrqTime;          /* KeQueryInterruptTime at the ISR (latency stat) */

    /* worker-owned state (no locks) */
    S10T_CONTACT    Contacts[S10T_MAX_CONTACTS];
    S10T_FRAME      Fifo[S10T_FIFO_DEPTH];
    ULONG           FifoHead, FifoCount;
    ULONGLONG       LastEventTime;
    UCHAR           Events[SECTS_EVENT_SIZE * SECTS_MAX_EVENTS];
    BOOLEAN         BatchEmpty;         /* last attention gave an I2C error or an empty event stack */
    ULONG           EmptyStreak;        /* consecutive such attentions: back-off against a stuck-low line */

    /* configuration (device key) */
    ULONG           RepeatMs;
    BOOLEAN         NeedReinit;
    ULONGLONG       LastHeartbeat;  /* 1.0.3: PRAM heartbeat clock */
    ULONG           BootStatus;
    ULONG           BootTries;     /* the controller reported a watchdog reboot: touch function + sense on again */
    S10T_STATS      Stats;
} S10T_DEVICE, *PS10T_DEVICE;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(S10T_DEVICE, S10tGet)

/* Driver.c */
DRIVER_INITIALIZE                       DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD               S10tEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE         S10tEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE         S10tEvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY                 S10tEvtD0Entry;
EVT_WDF_DEVICE_D0_EXIT                  S10tEvtD0Exit;
EVT_WDF_DEVICE_D0_ENTRY_POST_INTERRUPTS_ENABLED S10tEvtD0EntryPostInterruptsEnabled;
EVT_WDF_DEVICE_D0_EXIT_PRE_INTERRUPTS_DISABLED  S10tEvtD0ExitPreInterruptsDisabled;
EVT_WDF_INTERRUPT_ISR                   S10tEvtIsr;
EVT_WDF_INTERRUPT_DPC                   S10tEvtDpc;
VOID S10tKickWorker(_In_ PS10T_DEVICE Dev);

/* Hid.c */
NTSTATUS S10tQueuesCreate(_In_ WDFDEVICE Device, _In_ PS10T_DEVICE Dev);
VOID S10tDeliver(_In_ PS10T_DEVICE Dev);

/* Pram.c (1.0.3): text lines in firmware-saved reserved RAM (WINPRAM +0xE000), readable after a force restart */
VOID S10tPramInit(VOID);
VOID S10tPramFree(VOID);
VOID S10tPramLine(_In_ ULONG Line, _In_z_ PCSTR Text);
VOID S10tPramRoll(_In_z_ PCSTR Text);
