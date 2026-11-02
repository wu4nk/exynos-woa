/*
 * S10Touch - DirectI2c.c (1.1.0): talk to the touch controller on hsi2c@104B0000 directly, polled, from the worker
 * thread - the exact transfer the UEFI's TouchChip.c does (Beyond0LteTouchPowerDxe; device log: a 1-byte register
 * read in 224 us at 400 kHz, never an error) - instead of going through SpbCx + Exynos9820Hsi2c.
 *
 * Why: s10-log 2026-11-03 04:00 (S10Touch 1.0.8 + Exynos9820Hsi2c 1.0.2): 597 failed reads (c00000b5) out of
 * ~2000 events, a batch of 3-4 reads took ~50 ms (= 12+ ms per 8-byte read through the SPB stack), the panel's
 * event stack (31 events) filled during every longer drag -> "overflow: stack cleared" -> the finger is lifted ->
 * a drag broken into taps. The bus itself is fine (UEFI); the slow, failing part is the Windows SPB path.
 *
 * Ownership: only the touch IC is on this bus. Exynos9820Hsi2c may still be installed for \_SB.I2C0; it touches
 * the controller only in its own D0Entry (reset + timing, the same values) and for requests - nobody sends it any
 * once this path is used - and its ISR ignores interrupts while it has no request (software mask 0).
 *
 * Registers: Exynos HSI2C (samsung,exynos5-hsi2c), firmware AudioProbeHw.h / TouchChip.c; bases from Exynos9820.h.
 * SPDX-License-Identifier: GPL-2.0
 */
#include "S10Touch.h"

#define DI2C_HSI2C_PA      0x104B0000ULL   /* EXYNOS_TOUCH_HSI2C_BASE */
#define DI2C_CMU_PA        0x10400000ULL   /* EXYNOS_CMU_PERIC0_BASE (0x8000) */
#define DI2C_SYSREG_PA     0x10421000ULL   /* EXYNOS_PERIC0_USI_SYSREG_BASE */
#define DI2C_GPIO_PA       0x10430000ULL   /* EXYNOS_PERIC0_GPIO_BASE */
#define DI2C_PAGE          0x1000UL
#define DI2C_CMU_LEN       0x4000UL
#define DI2C_ADDR          0x48            /* EXYNOS_TOUCH_I2C_ADDR */

/* HSI2C */
#define HS_CTL             0x00
#define   HS_CTL_RXCHON      (1UL << 6)
#define   HS_CTL_TXCHON      (1UL << 7)
#define   HS_CTL_MASTER      (1UL << 3)
#define   HS_CTL_SW_RST      (1UL << 31)
#define HS_FIFO_CTL        0x04
#define   HS_FIFO_RX_EN      (1UL << 0)
#define   HS_FIFO_TX_EN      (1UL << 1)
#define   HS_FIFO_RX_TRIG(x) (((ULONG)(x) & 0x7F) << 4)
#define   HS_FIFO_TX_TRIG(x) (((ULONG)(x) & 0x7F) << 16)
#define HS_TRAIL_CTL       0x08
#define HS_INT_EN          0x20
#define HS_INT_STAT        0x24
#define   HS_INT_DONE        (1UL << 7)
#define   HS_INT_ABORT       (1UL << 8)
#define   HS_INT_NOACK       (1UL << 9)
#define   HS_INT_NODEV       (1UL << 10)
#define   HS_INT_TIMEOUT     (1UL << 11)
#define   HS_INT_ERRORS      (HS_INT_ABORT | HS_INT_NOACK | HS_INT_NODEV | HS_INT_TIMEOUT)
#define   HS_INT_ALL         0x00000FFFUL
#define HS_FIFO_STAT       0x30
#define   HS_FIFO_RX_LEVEL(x) (((x) >> 16) & 0x7F)
#define HS_TX_DATA         0x34
#define HS_RX_DATA         0x38
#define HS_CONF            0x40
#define   HS_CONF_AUTO       (1UL << 31)
#define HS_AUTO_CONF       0x44
#define   HS_AUTO_RW         (1UL << 16)
#define   HS_AUTO_STOP       (1UL << 17)
#define   HS_AUTO_RUN        (1UL << 31)
#define HS_TIMEOUT         0x48
#define HS_TRANS_STATUS    0x50
#define   HS_MASTER_BUSY     (1UL << 17)
#define HS_TIMING_FS1      0x60
#define HS_TIMING_FS2      0x64
#define HS_TIMING_FS3      0x68
#define HS_ADDR            0x70
#define HS_USI_CON         0xC4
/* 400 kHz from a 200 MHz ipclk (the UEFI sets USI03 to PERIC0_IP / 2 = 200 MHz, v0.1.8+) */
#define HS_FS1_400K        0x00F00000UL
#define HS_FS2_400K        0x000000E0UL
#define HS_FS3_400K        0x00210000UL

/* platform (exynos9820hsi2c.h, cal-if) */
#define CMU_GATE_SRC       0x2008UL
#define CMU_GATE_RSTSYNC   0x2038UL
#define CMU_GATE_IPCLK     0x209CUL
#define CMU_GATE_PCLK      0x20A0UL
#define CMU_QCH            0x303CUL
#define CMU_GATE_ON        ((1UL << 20) | (1UL << 21))
#define CMU_QCH_ON         ((1UL << 0) | (1UL << 1))
#define CMU_USER_MUX       0x01E0UL        /* bit 4: 1 = CLKCMU_PERIC0_IP */
#define CMU_DIV            0x1810UL        /* [3:0] ratio */
#define SYSREG_USI03       0x001CUL
#define SYSREG_MODE_I2C    0x4UL
#define GPP1_CON           0x020UL
#define GPP1_PUD           0x028UL
#define GPP1_DRV           0x02CUL
#define GPP1_MASK          ((0xFUL << 16) | (0xFUL << 20))
#define GPP1_I2C           ((0x2UL << 16) | (0x2UL << 20))

#define DI2C_TIMEOUT_US    5000UL          /* an 8-byte read is ~300 us */

static ULONG Rd(_In_ volatile UCHAR *B, _In_ ULONG Off)               { return READ_REGISTER_ULONG((volatile ULONG *)(B + Off)); }
static VOID  Wr(_In_ volatile UCHAR *B, _In_ ULONG Off, _In_ ULONG V) { WRITE_REGISTER_ULONG((volatile ULONG *)(B + Off), V); }

static VOID DiReset(_In_ PS10T_DEVICE Dev)
{
    volatile UCHAR *h = Dev->Hs;

    Wr(h, HS_INT_EN, 0);
    Wr(h, HS_CTL, Rd(h, HS_CTL) | HS_CTL_SW_RST);
    Wr(h, HS_CTL, Rd(h, HS_CTL) & ~HS_CTL_SW_RST);
    Wr(h, HS_USI_CON, 0);
    Wr(h, HS_TIMING_FS1, HS_FS1_400K);
    Wr(h, HS_TIMING_FS2, HS_FS2_400K);
    Wr(h, HS_TIMING_FS3, HS_FS3_400K);
    Wr(h, HS_CTL, HS_CTL_MASTER);
    Wr(h, HS_TRAIL_CTL, 0xFFFFFF);
    Wr(h, HS_TIMEOUT, 0);
    Wr(h, HS_CONF, HS_CONF_AUTO);
    Wr(h, HS_FIFO_CTL, 0);
    Wr(h, HS_AUTO_CONF, 0);
    Wr(h, HS_ADDR, 0);
    Wr(h, HS_INT_STAT, HS_INT_ALL);
}

NTSTATUS S10tDirectOpen(_In_ PS10T_DEVICE Dev)
{
    PHYSICAL_ADDRESS pa;
    PUCHAR cmu, sys, gpio;
    ULONG  before;

    if (Dev->Hs != NULL) {
        return STATUS_SUCCESS;
    }
    pa.QuadPart = (LONGLONG)DI2C_HSI2C_PA;  Dev->Hs = (PUCHAR)MmMapIoSpaceEx(pa, DI2C_PAGE, PAGE_READWRITE | PAGE_NOCACHE);
    pa.QuadPart = (LONGLONG)DI2C_CMU_PA;    cmu     = (PUCHAR)MmMapIoSpaceEx(pa, DI2C_CMU_LEN, PAGE_READWRITE | PAGE_NOCACHE);
    pa.QuadPart = (LONGLONG)DI2C_SYSREG_PA; sys     = (PUCHAR)MmMapIoSpaceEx(pa, DI2C_PAGE, PAGE_READWRITE | PAGE_NOCACHE);
    pa.QuadPart = (LONGLONG)DI2C_GPIO_PA;   gpio    = (PUCHAR)MmMapIoSpaceEx(pa, DI2C_PAGE, PAGE_READWRITE | PAGE_NOCACHE);
    if (Dev->Hs == NULL || cmu == NULL || sys == NULL || gpio == NULL) {
        if (cmu)  MmUnmapIoSpace(cmu, DI2C_CMU_LEN);
        if (sys)  MmUnmapIoSpace(sys, DI2C_PAGE);
        if (gpio) MmUnmapIoSpace(gpio, DI2C_PAGE);
        if (Dev->Hs) { MmUnmapIoSpace(Dev->Hs, DI2C_PAGE); Dev->Hs = NULL; }
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* clocks, USI mode, pins: what UEFI and Exynos9820Hsi2c both do (same values, idempotent) */
    Wr(cmu, CMU_GATE_SRC, Rd(cmu, CMU_GATE_SRC) | CMU_GATE_ON);
    Wr(cmu, CMU_GATE_RSTSYNC, Rd(cmu, CMU_GATE_RSTSYNC) | CMU_GATE_ON);
    Wr(cmu, CMU_GATE_IPCLK, Rd(cmu, CMU_GATE_IPCLK) | CMU_GATE_ON);
    Wr(cmu, CMU_GATE_PCLK, Rd(cmu, CMU_GATE_PCLK) | CMU_GATE_ON);
    Wr(cmu, CMU_QCH, Rd(cmu, CMU_QCH) | CMU_QCH_ON);
    before = Rd(cmu, CMU_DIV);
    Dev->DirectClock = ((Rd(cmu, CMU_USER_MUX) & (1UL << 4)) ? 0x100UL : 0) | (before & 0xF);   /* for the log */
    /* PERIC0_IP (400 MHz) / 1 = an older UEFI (< v0.1.8): the 400 kHz timing below needs / 2 = 200 MHz */
    if ((Rd(cmu, CMU_USER_MUX) & (1UL << 4)) && (before & 0xF) == 0) {
        Wr(cmu, CMU_DIV, (before & ~0xFUL) | 1);
        KeStallExecutionProcessor(10);
        Dev->DirectClock |= 0x1000UL;                      /* "fixed here" */
    }
    Wr(sys, SYSREG_USI03, SYSREG_MODE_I2C);
    Wr(gpio, GPP1_CON, (Rd(gpio, GPP1_CON) & ~GPP1_MASK) | GPP1_I2C);
    Wr(gpio, GPP1_PUD, Rd(gpio, GPP1_PUD) & ~GPP1_MASK);
    Wr(gpio, GPP1_DRV, Rd(gpio, GPP1_DRV) & ~GPP1_MASK);
    KeMemoryBarrier();
    MmUnmapIoSpace(cmu, DI2C_CMU_LEN);
    MmUnmapIoSpace(sys, DI2C_PAGE);
    MmUnmapIoSpace(gpio, DI2C_PAGE);

    DiReset(Dev);
    return STATUS_SUCCESS;
}

VOID S10tDirectClose(_In_ PS10T_DEVICE Dev)
{
    if (Dev->Hs != NULL) {
        Wr(Dev->Hs, HS_INT_EN, 0);
        MmUnmapIoSpace(Dev->Hs, DI2C_PAGE);
        Dev->Hs = NULL;
    }
}

/* One HSI2C transfer, polled. Length <= 16 (FIFO). Stop = STOP after it (else a repeated start follows). */
static NTSTATUS DiXfer(_In_ PS10T_DEVICE Dev, _In_ BOOLEAN Read, _Inout_updates_(Len) PUCHAR Data, _In_ ULONG Len, _In_ BOOLEAN Stop)
{
    volatile UCHAR *h = Dev->Hs;
    ULONG ctl, auto_, addr, st = 0, i, us;

    if (h == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }
    if (Len == 0 || Len > 16) {
        return STATUS_INVALID_PARAMETER;
    }

    Wr(h, HS_TIMEOUT, Rd(h, HS_TIMEOUT) & ~(1UL << 31));
    Wr(h, HS_FIFO_CTL, HS_FIFO_RX_EN | HS_FIFO_TX_EN | HS_FIFO_RX_TRIG(Len) | HS_FIFO_TX_TRIG(Len));
    ctl = (Rd(h, HS_CTL) | HS_CTL_MASTER) & ~(HS_CTL_RXCHON | HS_CTL_TXCHON);
    ctl |= Read ? HS_CTL_RXCHON : HS_CTL_TXCHON;
    Wr(h, HS_CTL, ctl);

    auto_ = (Rd(h, HS_AUTO_CONF) & ~(0xFFFFUL | HS_AUTO_RW | HS_AUTO_STOP)) | Len;
    if (Read) auto_ |= HS_AUTO_RW;
    if (Stop) auto_ |= HS_AUTO_STOP;
    addr = (Rd(h, HS_ADDR) & ~((0x3FFUL << 10) | 0x3FFUL | (0xFFUL << 24))) | ((ULONG)DI2C_ADDR << 10) | (0x7UL << 24);
    Wr(h, HS_ADDR, addr);

    for (us = 0; Rd(h, HS_TRANS_STATUS) & HS_MASTER_BUSY; us += 5) {
        if (us >= DI2C_TIMEOUT_US) {
            DiReset(Dev);
            return STATUS_DEVICE_BUSY;
        }
        KeStallExecutionProcessor(5);
    }

    Wr(h, HS_INT_STAT, HS_INT_ALL);
    Wr(h, HS_INT_EN, HS_INT_DONE | HS_INT_ERRORS);   /* status bits are latched for enabled sources (as in UEFI) */
    Wr(h, HS_AUTO_CONF, auto_ | HS_AUTO_RUN);
    if (!Read) {
        for (i = 0; i < Len; i++) {
            WRITE_REGISTER_UCHAR((volatile UCHAR *)(h + HS_TX_DATA), Data[i]);
        }
    }

    for (us = 0; us < DI2C_TIMEOUT_US; us += 2) {
        st = Rd(h, HS_INT_STAT);
        if (st & (HS_INT_DONE | HS_INT_ERRORS)) {
            break;
        }
        KeStallExecutionProcessor(2);
    }
    Wr(h, HS_INT_EN, 0);
    if (st & HS_INT_ERRORS) {
        DiReset(Dev);
        return (st & (HS_INT_NOACK | HS_INT_NODEV)) ? STATUS_NO_SUCH_DEVICE : STATUS_IO_DEVICE_ERROR;
    }
    if (!(st & HS_INT_DONE)) {
        DiReset(Dev);
        return STATUS_IO_TIMEOUT;
    }
    Wr(h, HS_INT_STAT, st);

    if (Read) {
        for (i = 0; i < Len; i++) {
            for (us = 0; HS_FIFO_RX_LEVEL(Rd(h, HS_FIFO_STAT)) == 0; us += 2) {
                if (us >= DI2C_TIMEOUT_US) {
                    DiReset(Dev);
                    return STATUS_IO_TIMEOUT;
                }
                KeStallExecutionProcessor(2);
            }
            Data[i] = READ_REGISTER_UCHAR((volatile UCHAR *)(h + HS_RX_DATA));
        }
    }
    return STATUS_SUCCESS;
}

/* sec_ts_i2c_read: command byte, repeated start, Len bytes (Len > 16 is split into 16-byte reads of the same
   command - only READ_ONE_EVENT (8) and short registers are used). One retry after a bus reset. */
NTSTATUS S10tDirectRead(_In_ PS10T_DEVICE Dev, _In_ UCHAR Cmd, _Out_writes_(Len) PVOID Buf, _In_ ULONG Len)
{
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    UCHAR    c;
    ULONG    attempt;

    if (Len > 16) {
        return STATUS_INVALID_PARAMETER;
    }
    for (attempt = 0; attempt < 2; attempt++) {
        c = Cmd;
        status = DiXfer(Dev, FALSE, &c, 1, FALSE);
        if (NT_SUCCESS(status)) {
            status = DiXfer(Dev, TRUE, (PUCHAR)Buf, Len, TRUE);
        }
        if (NT_SUCCESS(status)) {
            break;
        }
        InterlockedIncrement(&Dev->Stats.DirectRetries);
    }
    return status;
}

NTSTATUS S10tDirectWrite(_In_ PS10T_DEVICE Dev, _In_reads_(Len) PVOID Buf, _In_ ULONG Len)
{
    UCHAR    tmp[16];
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    ULONG    attempt;

    if (Len == 0 || Len > sizeof(tmp)) {
        return STATUS_INVALID_PARAMETER;
    }
    for (attempt = 0; attempt < 2; attempt++) {
        RtlCopyMemory(tmp, Buf, Len);
        status = DiXfer(Dev, FALSE, tmp, Len, TRUE);
        if (NT_SUCCESS(status)) {
            break;
        }
        InterlockedIncrement(&Dev->Stats.DirectRetries);
    }
    return status;
}
