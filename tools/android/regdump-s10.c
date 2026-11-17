/*
 * regdump-s10.c (v1.0) - read-only register dump for ispregs-s10.sh (notes/s10-isp.md, 2026-11-17).
 * One process maps each block through /dev/mem once (O_SYNC = uncached device mapping) and reads it as 32-bit
 * words: toybox devmem costs one exec per word (~50 ms each on the phone = 13 min per 64 KiB block).
 *
 *   regdump-s10 <runtime_status file> <out dir> NAME:BASE:SIZE [NAME:BASE:SIZE ...]
 *
 * Writes <out dir>/NAME.txt as "offset value" lines. Before every 64 reads it re-reads the runtime_status file and
 * stops if it is not "active" (a powered-off block would stop the phone). Never writes to a register.
 * Build (static, no Android libc needed): zig cc -target aarch64-linux-musl -O2 -static -o regdump-s10 regdump-s10.c
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int active(const char *st)
{
    char b[32] = {0};
    int fd = open(st, O_RDONLY);
    if (fd < 0) return 0;
    ssize_t n = read(fd, b, sizeof(b) - 1);
    close(fd);
    return n >= 6 && strncmp(b, "active", 6) == 0;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <runtime_status file> <out dir> NAME:BASE:SIZE ...\n", argv[0]);
        return 2;
    }
    const char *st = argv[1], *dir = argv[2];
    int mem = open("/dev/mem", O_RDONLY | O_SYNC);
    if (mem < 0) { perror("/dev/mem"); return 1; }
    for (int a = 3; a < argc; a++) {
        char name[64];
        unsigned long base, size;
        if (sscanf(argv[a], "%63[^:]:%lx:%lx", name, &base, &size) != 3 || (base & 0xfff) || (size & 0xfff) || !size) {
            fprintf(stderr, "bad block '%s' (NAME:BASE:SIZE, hex, 4 KiB aligned)\n", argv[a]);
            return 2;
        }
        if (!active(st)) { printf("%s: camera not active - stopped\n", name); return 1; }
        volatile uint32_t *p = mmap(NULL, size, PROT_READ, MAP_SHARED, mem, (off_t)base);
        if (p == MAP_FAILED) { perror("mmap"); return 1; }
        char path[512];
        snprintf(path, sizeof(path), "%s/%s.txt", dir, name);
        FILE *f = fopen(path, "w");
        if (!f) { perror(path); return 1; }
        fprintf(f, "# %s base 0x%08lx, offset value (32-bit reads, regdump-s10 v1.0)\n", name, base);
        for (unsigned long o = 0; o < size; o += 4) {
            if ((o & 0xff) == 0 && !active(st)) {
                fclose(f);
                printf("%s: camera closed at +0x%04lx - stopped (partial file kept)\n", name, o);
                return 1;
            }
            fprintf(f, "%04lx 0x%08x\n", o, p[o / 4]);
        }
        fclose(f);
        munmap((void *)p, size);
        printf("%s: %s\n", name, path);
    }
    close(mem);
    return 0;
}
