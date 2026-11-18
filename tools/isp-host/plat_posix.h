/* plat_posix.h - extras of the Linux test platform (plat_posix.c) */
#ifndef PLAT_POSIX_H
#define PLAT_POSIX_H
#include <stdio.h>
#include "isphost.h"
void plat_posix_log_to(FILE *f, int quiet);
void plat_posix_set_window(u32 id, void *p);
void plat_posix_crash_report(u64 lib_base);       /* diagnosis: log registers on SIGSEGV / SIGILL / ... */
#ifdef _WIN32
void plat_win_dma_pool(void *va, u64 pa, u64 bytes);   /* plat_win.c: S10eIspDma's pool */
void plat_win_set_thread_fatal(int on);
void plat_win_set_mark_file(const char *path);
#endif
#endif
