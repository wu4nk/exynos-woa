/* plat_posix.h - extras of the Linux test platform (plat_posix.c) */
#ifndef PLAT_POSIX_H
#define PLAT_POSIX_H
#include <stdio.h>
#include "isphost.h"
void plat_posix_log_to(FILE *f, int quiet);
void plat_posix_set_window(u32 id, void *p);
void plat_posix_crash_report(u64 lib_base);       /* diagnosis: log registers on SIGSEGV / SIGILL / ... */
#endif
