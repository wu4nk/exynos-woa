# emu.py - Stage 2 harness (notes/s10-isp.md): Samsung's DDK / RTA images in Unicorn at their Android VAs, with an
# OS-services table of marker functions. Every OS call is logged and answered by a Python handler (default: return 0).
# Nothing here touches hardware: register windows handed out by get_reg_addr are plain RAM that is logged on access.
#
#   from emu import Isp;  e = Isp(ddk_path);  e.start_up();  e.call(e.DDK + 0x80, 1, out_ptr)
import struct
from unicorn import (Uc, UcError, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_UNMAPPED,
                     UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE)
from unicorn.arm64_const import *
from unicorn.unicorn_const import UC_MEM_WRITE

LIB_START = 0xffffff80f9fe0000          # DDK file offset 0 (basefind.py, 2026-11-17)
DDK = LIB_START + 0xE0000                 # start_up (DDK_LIB_ADDR)
DDK_SPAN = 0x4c20000                      # image + data + heap (DDK's own log: heap_end 0xffffff80fec00000)
RTA = 0xffffff80fa500000                  # RTA file offset 0
MK = 0x10000000                           # OS marker functions: MK + 4*index, each "ret"
SCRATCH = 0x20000000                      # our own allocations (stack, tables, structs, fake MMIO)
RET_ADDR = 0x30000000                     # top-level calls return here
STUB = 0x11000000                         # sensor-interface ops and other functions we hand in
MMIO = 0x40000000                         # fake register windows (plain RAM, every access logged)
CB_CAMERA, CB_IO = 200, 201               # lib_callback_func entries live in the marker page too

OS_NAMES = {0: 'log_write_console', 3: 'assert', 4: 'sema_init', 5: 'sema_finish', 6: 'sema_up', 7: 'sema_down',
    8: 'mutex_init', 9: 'mutex_finish', 10: 'mutex_lock', 11: 'mutex_trylock', 12: 'mutex_unlock',
    13: 'timer_create', 14: 'timer_delete', 15: 'timer_reset', 16: 'timer_query', 17: 'timer_enable',
    18: 'timer_disable', 19: 'irq_register', 20: 'irq_unregister', 21: 'irq_enable', 22: 'irq_disable',
    23: 'irq_clear', 24: 'svc_spin_lock_save', 25: 'svc_spin_unlock_restore', 26: 'get_random_int', 27: 'add_task',
    28: 'get_usec', 29: 'log_write', 30: 'dva_taaisp', 31: 'kva_taaisp', 32: 'sleep', 33: 'inv_taaisp',
    34: 'alloc_taaisp', 35: 'free_taaisp', 36: 'spin_init', 37: 'spin_finish', 38: 'spin_lock', 39: 'spin_unlock',
    40: 'spin_lock_irq', 41: 'spin_unlock_irq', 42: 'spin_lock_irqsave', 43: 'spin_unlock_irqrestore',
    46: 'get_reg_addr', 47: 'in_irq', 48: 'flush_task_handler', 49: 'fd_data_a', 50: 'fd_data_b',
    CB_CAMERA: 'CB camera_callback', CB_IO: 'CB io_callback'}


class Isp:
    def __init__(self, ddk_path=None, rta_path=None, trace_os=True):
        self.u = u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        self.DDK, self.RTA = DDK, RTA
        self.trace_os = trace_os
        self.calls, self.logs = [], []
        self.handlers = {}
        if ddk_path:
            img = open(ddk_path, 'rb').read()
            u.mem_map(LIB_START, DDK_SPAN)
            u.mem_write(LIB_START, img)
        if rta_path:
            img = open(rta_path, 'rb').read()
            if ddk_path:
                # RTA lies inside the DDK span (fixed 0x520000 from LIB_START); its heap HEAP_RTA_START = LIB_START +
                # 80 MiB (GPL fimc-is-binary.h) is beyond the span: map LIB_START + span .. + 0x7000000
                u.mem_map(LIB_START + DDK_SPAN, 0x7000000 - DDK_SPAN)
            else:
                u.mem_map(RTA, (len(img) + 0x400000 + 0xfffff) & ~0xfffff)
            u.mem_write(RTA, img)
        u.mem_map(MK, 0x1000)
        u.mem_write(MK, b'\xc0\x03\x5f\xd6' * 0x400)
        u.mem_map(SCRATCH, 0x04000000)
        u.mem_map(RET_ADDR, 0x1000)
        self.brk = SCRATCH + 0x00100000
        self.sp_top = SCRATCH + 0x000f0000
        self.table = self.alloc(8 * 100)
        for i in range(100):
            u.mem_write(self.table + 8 * i, struct.pack('<Q', MK + 4 * i))
        u.hook_add(UC_HOOK_CODE, self._os, begin=MK, end=MK + 0xfff)
        u.hook_add(UC_HOOK_MEM_UNMAPPED, self._bad)
        u.reg_write(UC_ARM64_REG_CPACR_EL1, 0x300000)          # FP/SIMD on
        self.bad = None
        self.windows = {}                       # name -> (base, size)
        # behaviour model: a write to (window, off) arms registers that then read 0 once ("done"), e.g. a
        # self-clearing reset bit. Only what the DDK is seen to poll is modelled (notes/s10-isp.md).
        self.done_after = {}                    # (window, off) -> [(window, off), ...]
        self.armed = set()
        self.mmio = []                          # (R/W, window, offset, value, pc)
        self.cb = self.alloc(16)
        u.mem_write(self.cb, struct.pack('<QQ', MK + 4 * CB_CAMERA, MK + 4 * CB_IO))

    def window(self, name, size=0x10000, values=None):
        """A fake register window: RAM at MMIO.., reads/writes logged. values = {offset: initial u32}."""
        base = MMIO + 0x100000 * len(self.windows)
        if not self.windows:
            self.u.mem_map(MMIO, 0x1000000)
            self.u.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, self._mmio, begin=MMIO, end=MMIO + 0xffffff)
        self.windows[name] = (base, size)
        for off, v in (values or {}).items():
            self.u.mem_write(base + off, struct.pack('<I', v))
        return base

    def _mmio(self, uc, access, addr, size, value, data):
        for name, (b, n) in self.windows.items():
            if b <= addr < b + 0x100000:
                off = addr - b
                if access == UC_MEM_WRITE:
                    rw, v = 'W', value
                    for tgt in self.done_after.get((name, off), ()):
                        self.armed.add(tgt)
                else:
                    if (name, off) in self.armed:          # hardware finished: the polled register reads 0
                        self.armed.discard((name, off))
                        uc.mem_write(addr, b'\0' * size)
                    rw, v = 'R', int.from_bytes(uc.mem_read(addr, size), 'little')
                self.mmio.append((rw, name, addr - b, v, size, uc.reg_read(UC_ARM64_REG_PC)))
                return

    # --- memory helpers
    def alloc(self, n, align=64):
        self.brk = (self.brk + align - 1) & ~(align - 1)
        a = self.brk
        self.brk += n
        self.u.mem_write(a, b'\0' * n)
        return a

    def q(self, a): return struct.unpack('<Q', self.u.mem_read(a, 8))[0]
    def d(self, a): return struct.unpack('<I', self.u.mem_read(a, 4))[0]
    def s(self, a, n=256): return bytes(self.u.mem_read(a, n)).split(b'\0')[0].decode('latin1')
    def img(self, a):
        if RTA <= a < RTA + 0x800000: return 'rta+0x%x' % (a - RTA)    # first: it lies inside the DDK span
        if LIB_START <= a < LIB_START + DDK_SPAN: return 'ddk+0x%x' % (a - LIB_START)
        return '0x%x' % a

    # --- OS calls
    def _os(self, uc, addr, size, data):
        i = (addr - MK) // 4
        x = [uc.reg_read(r) for r in (UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3)]
        lr = uc.reg_read(UC_ARM64_REG_X30)
        ret = 0
        if i in self.handlers:
            ret = self.handlers[i](self, *x)
        elif i in (0, 29):
            self.logs.append(self.s(x[0]).rstrip('\n'))
        self.calls.append((i, lr, x))
        if self.trace_os and i not in (24, 25, 36, 42, 43, 0, 29):
            print('  os %-24s from %s  x0..3 %s -> %s' % (OS_NAMES.get(i, str(i)), self.img(lr),
                                                     ' '.join(self.img(v) for v in x), ret))
        uc.reg_write(UC_ARM64_REG_X0, ret & 0xffffffffffffffff)

    def _bad(self, uc, t, a, s, v, d):
        self.bad = 'unmapped access type %d at 0x%x, pc %s, lr %s' % (t, a, self.img(uc.reg_read(UC_ARM64_REG_PC)),
                                                                   self.img(uc.reg_read(UC_ARM64_REG_X30)))
        return False

    # --- calling into the image
    def call(self, fn, *args, limit=50_000_000):
        u = self.u
        for k, v in enumerate(args):
            u.reg_write(UC_ARM64_REG_X0 + k, v & 0xffffffffffffffff)
        u.reg_write(UC_ARM64_REG_SP, self.sp_top)
        u.reg_write(UC_ARM64_REG_X30, RET_ADDR)
        self.bad = None
        try:
            u.emu_start(fn, RET_ADDR, count=limit)
        except UcError as e:
            raise RuntimeError('%s (%s; pc %s lr %s)' % (e, self.bad, self.img(u.reg_read(UC_ARM64_REG_PC)),
                                                     self.img(u.reg_read(UC_ARM64_REG_X30))))
        if u.reg_read(UC_ARM64_REG_PC) != RET_ADDR:
            raise RuntimeError('did not return (instruction limit), pc %s' % self.img(u.reg_read(UC_ARM64_REG_PC)))
        return u.reg_read(UC_ARM64_REG_X0)

    # --- default OS services (GPL interface-library.c semantics, no hardware)
    def os_defaults(self):
        """DMA pools (alloc/dva/kva/inv/free for taaisp 30-35, tnr 60-64, medrc 65-69), get_usec, get_reg_addr,
        interrupt and timer registration recorded for later firing."""
        DMA_KVA, DMA_DVA, DMA_SZ = 0x50000000, 0x80000000, 0x08000000
        self.u.mem_map(DMA_KVA, DMA_SZ)
        self.dma = {'next': DMA_KVA, 'allocs': []}
        def alloc(e, size, *a):
            k = (self.dma['next'] + 0xfff) & ~0xfff
            self.dma['next'] = k + size
            assert self.dma['next'] < DMA_KVA + DMA_SZ, 'fake DMA pool full'
            self.dma['allocs'].append((k, size))
            return k
        def dva(e, kva, pdva, *a):
            if not (DMA_KVA <= kva < DMA_KVA + DMA_SZ): return -22
            self.u.mem_write(pdva, struct.pack('<I', DMA_DVA + kva - DMA_KVA)); return 0
        def kva(e, dv, pkva, *a):
            dv &= 0xffffffff
            if not (DMA_DVA <= dv < DMA_DVA + DMA_SZ): return -22
            self.u.mem_write(pkva, struct.pack('<Q', DMA_KVA + dv - DMA_DVA)); return 0
        for b in (30, 60, 65): self.handlers[b] = dva
        for b in (31, 61, 66): self.handlers[b] = kva
        for b in (34, 63, 68): self.handlers[b] = alloc
        self.usec = 1_000_000
        def usec(e, *a):
            self.usec += 10; return self.usec
        self.handlers[28] = usec
        REG_IDS = {0: '3AA0', 1: '3AA1', 2: 'ISP0', 3: 'ISP1', 4: 'TPU0', 5: 'TPU1', 6: 'DCP'}
        def reg_addr(e, i, *a):
            w = self.windows.get(REG_IDS.get(i & 0xffffffff, '?'))
            return w[0] if w else 0
        self.handlers[46] = reg_addr
        self.irqs = []
        def irq_reg(e, pinfo, *a):
            v = bytes(self.u.mem_read(pinfo, 40))
            valid, prio, iid = struct.unpack_from('<III', v, 0)
            ctx, handler = struct.unpack_from('<QQ', v, 16)
            chain = struct.unpack_from('<I', v, 32)[0]
            self.irqs.append(dict(valid=valid, prio=prio, id=iid, ctx=ctx, handler=handler, chain=chain))
            return 0
        self.handlers[19] = irq_reg
        self.timers = []
        def timer_create(e, ptimer, expires, func, data):
            self.timers.append(dict(ptimer=ptimer, expires=expires, func=func, data=data))
            h = self.alloc(64); self.u.mem_write(ptimer, struct.pack('<Q', h)); return 0
        self.handlers[13] = timer_create
        # sema_init(void **, count) / mutex_init(void **) / spin_lock_init(void **) allocate a handle and store it
        # (GPL interface-library.c:821 / :877 / :1358)
        def handle_init(e, pp, *a):
            h = self.alloc(64); self.u.mem_write(pp, struct.pack('<Q', h)); return 0
        for k in (4, 8, 36): self.handlers[k] = handle_init

    # --- stub functions handed to the image (sensor interface ops, callbacks): STUB + 4*n, each "ret"
    def stub(self, name, handler=None):
        if not hasattr(self, 'stubs'):
            self.stubs, self.stub_calls = [], []
            self.u.mem_map(STUB, 0x10000)
            self.u.mem_write(STUB, b'\xc0\x03\x5f\xd6' * 0x4000)
            self.u.hook_add(UC_HOOK_CODE, self._stub, begin=STUB, end=STUB + 0xffff)
        self.stubs.append((name, handler))
        return STUB + 4 * (len(self.stubs) - 1)

    def _stub(self, uc, addr, size, data):
        name, handler = self.stubs[(addr - STUB) // 4]
        x = [uc.reg_read(r) for r in (UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3,
                                       UC_ARM64_REG_X4, UC_ARM64_REG_X5)]
        ret = handler(self, *x) if handler else 0
        lr = uc.reg_read(UC_ARM64_REG_X30)
        self.stub_calls.append((name, lr, x, ret))
        if self.trace_os:
            print('  itf %-45s from %s  x1..3 %s -> %s' % (name, self.img(lr), ' '.join('0x%x' % v for v in x[1:4]), ret))
        uc.reg_write(UC_ARM64_REG_X0, (ret or 0) & 0xffffffffffffffff)

    def sensor_itf(self, layout_path, handlers=None):
        """struct fimc_is_sensor_interface (sensor_itf_layout.txt) with every op a logging stub; returns its address."""
        a = self.alloc(5704)
        self.u.mem_write(a, struct.pack('<I', 0xFEDCBA98))
        for line in open(layout_path):
            if line.startswith('#'): continue
            off, name = line.split()
            self.u.mem_write(a + int(off), struct.pack('<Q', self.stub(name, (handlers or {}).get(name))))
        return a

    def vra_os_funcs(self):
        """VRA part of the DDK file: ((vra_set_os_funcs_t)VRA_LIB_ADDR)(&funcs) with the 18-entry
        struct fimc_is_lib_vra_os_system_funcs (GPL interface-vra.h:70, filled at interface-vra.c:1277)."""
        names = ['control_task_set_event', 'fw_algs_task_set_event', 'set_dram_adr_from_core_to_vdma',
                 'clean_cache_region', 'invalidate_cache_region', 'data_write_back_cache_region',
                 'log_write_console', 'log_write', 'spin_lock_init', 'spin_lock_finish', 'spin_lock', 'spin_unlock',
                 'spin_lock_irq', 'spin_unlock_irq', 'spin_lock_irqsave', 'spin_unlock_irqrestore', 'lib_assert',
                 'lib_in_irq']
        def log(e, p, *a):
            self.logs.append('[VRA] ' + self.s(p).rstrip('\n')); return 0
        def hinit(e, pp, *a):
            h = self.alloc(64); self.u.mem_write(pp, struct.pack('<Q', h)); return 0
        hs = {'log_write_console': log, 'log_write': log, 'spin_lock_init': hinit}
        f = self.alloc(8 * len(names))
        for k, n in enumerate(names):
            self.u.mem_write(f + 8 * k, struct.pack('<Q', self.stub('vra.' + n, hs.get(n))))
        return self.call(LIB_START + 0x20000, f) & 0xffffffff

    def rta_start_up(self):
        """RTA: ((rta_start_up_func_t)RTA_LIB_ADDR)(NULL, table) with its own table layout
        (GPL set_os_system_funcs_for_rta, interface-library.c:2052)."""
        names = {0: 'log_write', 1: 'log_write_console', 2: 'assert', 9: 'fwrite', 30: 'register_general_interrupt',
                 31: 'unregister_general_interrupt', 40: 'add_task_rta', 50: 'timer_create', 51: 'timer_delete',
                 52: 'timer_reset', 53: 'timer_query', 54: 'timer_enable', 55: 'timer_disable', 56: 'udelay',
                 57: 'usleep', 58: 'sleep', 60: 'sema_init', 61: 'sema_finish', 62: 'sema_up', 63: 'sema_down',
                 80: 'svc_spin_lock_save', 81: 'svc_spin_unlock_restore', 90: 'get_usec', 91: 'get_binary_version'}
        def log(e, p, *a):
            self.logs.append('[RTA] ' + self.s(p).rstrip('\n')); return 0
        def hinit(e, pp, *a):
            h = self.alloc(64); self.u.mem_write(pp, struct.pack('<Q', h)); return 0
        def usec(e, *a):
            self.usec = getattr(self, 'usec', 1_000_000) + 10; return self.usec
        def tcreate(e, pt, *a):
            return hinit(e, pt)
        hs = {0: log, 1: log, 60: hinit, 50: tcreate, 90: usec}
        t = self.alloc(8 * 100)
        for k, n in names.items():
            self.u.mem_write(t + 8 * k, struct.pack('<Q', self.stub('rta.' + n, hs.get(k))))
        self.rta_table = t
        return self.call(RTA, 0, t) & 0xffffffff

    def start_up(self):
        return self.call(DDK, self.table) & 0xffffffff
