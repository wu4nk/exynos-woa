# tools/isp-re - static and emulated analysis of the Samsung ISP libraries (notes/s10-isp.md)

Build-machine scripts (Python 3, `pip install capstone unicorn`). They read the user's own fimc_is_lib.bin /
fimc_is_rta.bin (pulled with tools/android/camtrace-s10.sh). The binaries are Samsung's: never commit them.
Run Python on them with `-I`, from outside the directory that holds the binaries.

| script | what it does |
| --- | --- |
| stage1.py FILE A:E[,..] E1[,..] | instruction census of code areas: x18 reads/writes, system registers, NEON count, entry disassembly |
| ptrscan.py FILE START | histogram of 64-bit words that look like kernel VAs, from START |
| basefind.py FILE | finds the load base: VA - offset votes over function prologues |
| basecheck.py FILE BASE | with a base: how many pointer words land in the file, on prologues, on strings |
| disasm.py FILE OFF[,..] [N] | disassembles N instructions at file offsets |
| front_init.py BIN DUMP OUT | the front-camera init path end to end in the emulator (start_up, VRA, RTA, sensor interface, 3AA + ISP chain_create with the phone's register dump, object_create); writes mmio-*.txt, interface-calls.txt |
| cmp_dump.py OUT DUMP WINDOW [N] | emulator vs phone: last written value per register against the live dump |
| osmap.py FILE [STOP] | Unicorn: runs the DDK start_up path from 0xf25b8 at its Android VA with marker OS functions (each a `ret`); prints where each OS-table index is stored, every OS call, and the DDK's log lines |
| emu.py | Stage 2 harness (import it): DDK/RTA at their Android VAs in Unicorn, OS table of logging markers with GPL-semantics defaults (DMA pools, get_usec, get_reg_addr, irq/timer registration), fake register windows (every access logged), sensor-interface stubs |
| param_layouts.txt | clang record layouts of struct taa_param_set, isp_param_set, camera2_shot (GPL v7_0_0 headers of the ExtremeKernel tree) |
| sensor_itf_layout.txt | struct fimc_is_sensor_interface function-pointer offsets (beyond1 config, clang record layout of the GPL header) - emu.py fills it with logging stubs |
