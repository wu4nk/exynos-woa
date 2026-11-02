/*
 * S10Touch - Pram.c: what the driver is doing, written as text lines into reserved RAM that the firmware saves as
 * \WINPRAM.BIN (ESP) on the next boot after Windows ended abnormally (force restart, hang, bugcheck). With no touch
 * the only way out of Windows is a force restart, which loses the registry counters (D0Exit never runs) and the event
 * log; this survives it. get-logs-twrp.cmd copies WINPRAM*.BIN off the ESP.
 *
 * Where: BEYOND0LTE_WINPRAM_BASE 0x8FE00000 (firmware/Platform/Beyond0LtePkg/Library/Beyond0LteFbSerialPortLib/
 * Beyond0LteBootLog.h:36) + 0xE000, 4 KiB. The firmware saves the first 0x10000 bytes and zeroes them after the save.
 * Inventory (notes/gpu-full-adapter-hang.md 2026-10-13): +0x0000 UFS ring, +0x4000 MaliG76Wddm, +0x8000 UFS early
 * marks, +0xA000..0xDFFF S10eDiag ring, +0xF000 progress words - +0xE000..0xEFFF is used by nothing else.
 *
 * Layout: 64 lines of 64 bytes, "<uptime ms, 10 digits> <text>" padded with spaces, ended by '\n' (the S10eDiag /
 * MaliG76Wddm format, read with `tr -c '[:print:]\n' '\n'`):
 *   line 0   "S10TOUCH <version>" + boot uptime
 *   line 1   start (worker): boot status, tries, ControllerStart status
 *   line 2   heartbeat (worker, once a second): counters
 *   line 3   heartbeat 2: attention line level, EINT registers, read requests, reinits
 *   lines 4..63  rolling: every raw controller event (8 bytes) and notable things, "#seq" first
 * Writers: D0Entry/D0Exit and the worker thread, all PASSIVE_LEVEL, never at the same time for the same line.
 * SPDX-License-Identifier: GPL-2.0
 */
#include "S10Touch.h"

#define S10T_PRAM_PA        0x8FE0E000ULL
#define S10T_PRAM_SIZE      0x1000u
#define S10T_PRAM_LINE      64u
#define S10T_PRAM_LINES     (S10T_PRAM_SIZE / S10T_PRAM_LINE)
#define S10T_PRAM_ROLL0     4u

static volatile UCHAR *Pram;
static ULONG           PramRoll = S10T_PRAM_ROLL0;
static ULONG           PramSeq;

VOID S10tPramInit(VOID)
{
    PHYSICAL_ADDRESS pa;

    if (Pram != NULL) {
        return;
    }
    pa.QuadPart = (LONGLONG)S10T_PRAM_PA;
    Pram = (volatile UCHAR *)MmMapIoSpaceEx(pa, S10T_PRAM_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (Pram != NULL) {
        ULONG i;

        for (i = 0; i < S10T_PRAM_SIZE; i++) {
            Pram[i] = (i % S10T_PRAM_LINE == S10T_PRAM_LINE - 1) ? '\n' : ' ';
        }
        PramRoll = S10T_PRAM_ROLL0;
        S10tPramLine(0, "S10TOUCH 1.0.7.0 (pram +0xE000, 64 x 64 B)");
    }
}

VOID S10tPramFree(VOID)
{
    if (Pram != NULL) {
        MmUnmapIoSpace((PVOID)Pram, S10T_PRAM_SIZE);
        Pram = NULL;
    }
}

VOID S10tPramLine(_In_ ULONG Line, _In_z_ PCSTR Text)
{
    CHAR      buf[S10T_PRAM_LINE];
    ULONGLONG ms;
    ULONG     n, i;

    if (Pram == NULL || Line >= S10T_PRAM_LINES) {
        return;
    }
    ms = KeQueryInterruptTime() / 10000ULL;
    RtlZeroMemory(buf, sizeof(buf));
    (VOID)RtlStringCbPrintfA(buf, sizeof(buf), "%010I64u %s", ms, Text);
    n = (ULONG)strlen(buf);
    for (i = 0; i < S10T_PRAM_LINE - 1; i++) {
        UCHAR c = (i < n) ? (UCHAR)buf[i] : ' ';
        Pram[Line * S10T_PRAM_LINE + i] = (c >= 0x20 && c < 0x7F) ? c : '.';
    }
    Pram[Line * S10T_PRAM_LINE + S10T_PRAM_LINE - 1] = '\n';
}

VOID S10tPramRoll(_In_z_ PCSTR Text)
{
    CHAR buf[S10T_PRAM_LINE];

    if (Pram == NULL) {
        return;
    }
    (VOID)RtlStringCbPrintfA(buf, sizeof(buf), "#%05u %s", PramSeq++ % 100000u, Text);
    S10tPramLine(PramRoll, buf);
    PramRoll = (PramRoll + 1 < S10T_PRAM_LINES) ? PramRoll + 1 : S10T_PRAM_ROLL0;
}
