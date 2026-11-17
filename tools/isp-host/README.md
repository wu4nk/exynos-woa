# isp-host - Samsung's camera ISP libraries, run natively (notes/s10-isp.md, Stage 4 option A)

The S10's camera pipeline (3AA / ISP / MCSC) is programmed by two closed libraries from the phone's firmware
partition: `fimc_is_lib.bin` (DDK + VRA) and `fimc_is_rta.bin` (RTA: AE / AWB / AF). On Android they run inside the
kernel at a fixed address. This host loads them into a user-mode process, relocates their absolute pointers, gives
them the OS services the GPL kernel gives them, and runs them on host threads. The libraries are never in this repo:
they are read from the phone at run time.

| file | what |
| --- | --- |
| `isphost.h` / `isphost.c` | portable core: loader + relocation, x18 trampolines, GPL OS-service tables, task / timer / interrupt threads, DMA bookkeeping |
| `tramp.S` | reference source of the trampoline machine code (`isphost.c` emits the same words; `check_tramp.py` proves it) |
| `plat_posix.c` | platform layer for AArch64 Linux (build machine under qemu-aarch64, or Android itself) |
| `mmio_trap.c` | TEST ONLY: register windows as trapped mappings (every access decoded, logged, performed on a dump) |
| `test_front.c` | the front-camera run of `tools/isp-re/front_init.py`, natively |
| `cmp_emu.py` | compares a `test_front` run with a `front_init.py` run (returns, log lines, register accesses per step) |

## Why the trampolines
The libraries use x18 as an ordinary register; Windows ARM64 user mode keeps the TEB in x18. Every call into the
libraries saves the host's x18 (`isp_enter`); every call out of them (an OS service, a sensor-interface op, a
callback) goes through a trampoline that restores the calling thread's host x18 (looked up by stack range) and
puts the library's x18 back on return (`isp_common`). All library code runs on threads the core creates, each
registered in that table. Limits: interfaces with more than 8 integer arguments or floating-point arguments are not
supported by the generic host signature (none is used on the front path).

## Build and run (build machine)
```
sh build.sh /tmp/test_front                         # zig cc -target aarch64-linux-musl, static
qemu-aarch64 /tmp/test_front <dir: fimc_is_lib.bin fimc_is_rta.bin setfile_3j1.bin> \
    notes/data/isp-dump-20261117 <out dir> tools/isp-re/sensor_itf_layout.txt [front calibration image]
python3 -I cmp_emu.py notes/data/isp-dump-20261117 <front_init.py out dir> <out dir>
```
Env: `FRAMES` (3), `ISP` (1), `TRACE=1`, `QUIET=1`. Diagnosis switches that reproduce the emulator's old OS models
(never for real use): `DIAG_EMU_USEC`, `DIAG_IN_IRQ0`, `DIAG_RANDOM0`.

Outputs: `host.log`, `interface-calls.txt`, `mmio-<step>.txt`, `regs-3AA0.txt` / `regs-ISPLP.txt`, `timing.txt`.
