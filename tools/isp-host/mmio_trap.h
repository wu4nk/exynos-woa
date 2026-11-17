/* mmio_trap.h - test-only trapped register windows (mmio_trap.c) */
#ifndef MMIO_TRAP_H
#define MMIO_TRAP_H
#include <stdio.h>
#include "isphost.h"
/* returns the address the libraries get; accesses go to shadow[] (64 KiB). Window index = creation order. */
void *trap_window(const char *name, volatile u32 *shadow, u64 lib_base);
/* a store to (wa, oa) makes the next load of (wb, ob) return 0 once (and leaves 0 in the shadow) */
void trap_done_after(int wa, u32 oa, int wb, u32 ob);
void trap_log_to(FILE *f);       /* "R/W window+off value pc" lines; NULL = off */
u64 trap_count(void);
#endif
