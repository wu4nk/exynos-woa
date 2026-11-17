# front_init.py - Stage 2 (notes/s10-isp.md): the front-camera init path of Samsung's ISP libraries in the emulator,
# in the order the GPL kernel uses: DDK start_up, VRA os funcs, RTA start_up, register_sensor_interface,
# get_lib_func(3AA / ISP), chain_create (3AA0 / ISPLP with the REGISTER VALUES READ ON THE PHONE), set_system_config,
# object_create. Logs every OS call, sensor-interface call, register access (mmio-<step>.txt) and the libraries' log.
#
#   python3 -I front_init.py <dir with fimc_is_lib.bin + fimc_is_rta.bin> <dir with 3AA0.txt + ISPLP.txt> <out dir>
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from emu import Isp  # noqa: E402

BIN, DUMP, OUT = sys.argv[1], sys.argv[2], sys.argv[3]
os.makedirs(OUT, exist_ok=True)
LAYOUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'sensor_itf_layout.txt')
FN = ['chain_create', 'object_create', 'chain_destroy', 'object_destroy', 'stop', 'recovery', 'set_param', 'set_ctrl',
      'shot', 'get_meta', 'create_tune_set', 'apply_tune_set', 'delete_tune_set', 'set_parameter_flash',
      'set_parameter_af', 'load_cal_data', 'get_cal_data', 'sensor_info_mode_chg', 'sensor_update_ctl',
      'set_system_config']       # struct lib_interface_func order, GPL interface-ddk.h:206


def load(name):
    d = {}
    for line in open(os.path.join(DUMP, name + '.txt')):
        f = line.split()
        if line[0] != '#' and len(f) == 2 and f[1].startswith('0x'):
            d[int(f[0], 16)] = int(f[1], 16)
    return d


e = Isp(os.path.join(BIN, 'fimc_is_lib.bin'), os.path.join(BIN, 'fimc_is_rta.bin'), trace_os=False)
e.os_defaults()
print('start_up ->', e.start_up())
print('vra_os_funcs ->', e.vra_os_funcs())
print('rta_start_up ->', e.rta_start_up())


def w32(e, p, v):
    e.u.mem_write(p, struct.pack('<I', v))
    return 0


# the sensor answers we know so far: S5K3J1 = module id 38 (include/exynos-fimc-is-sensor.h:110), FRONT = 1
itf = e.sensor_itf(LAYOUT, {'cis_itf_ops.get_module_id': lambda e, itf, p, *a: w32(e, p, 38),
                            'cis_itf_ops.get_module_position': lambda e, itf, p, *a: w32(e, p, 1)})
print('register_sensor_interface ->', e.call(e.DDK + 0x40, itf) & 0xffffffff)
out = e.alloc(8)
tabs = {}
for t, n in ((1, '3AA'), (2, 'ISP')):
    e.call(e.DDK + 0x80, t, out)
    p = e.q(out)
    tabs[n] = {FN[k]: e.q(p + 8 * k) for k in range(len(FN))}
w3 = e.window('3AA0', values=load('3AA0'))
wi = e.window('ISP0', values=load('ISPLP'))
# reset handshakes seen in assembly: 3AA/ISP +0x0c write 1 then poll until 0 (0x219e90); ISP COREX +0x24 write 1
# then poll +0x2c until 0 (0x24f408; second COREX copy at +0x8000)
e.done_after = {('3AA0', 0x0c): [('3AA0', 0x0c)], ('ISP0', 0x0c): [('ISP0', 0x0c)],
                ('ISP0', 0x3d24): [('ISP0', 0x3d2c)], ('ISP0', 0xbd24): [('ISP0', 0xbd2c)]}


def step(label, fn, *a):
    e.logs.clear()
    n0, c0, s0 = len(e.mmio), len(e.calls), len(e.stub_calls)
    try:
        r = '0x%x' % (e.call(fn, *a) & 0xffffffff)
    except RuntimeError as x:
        r = 'EXC ' + str(x)
    acc = e.mmio[n0:]
    with open(os.path.join(OUT, 'mmio-%s.txt' % label.replace(' ', '_')), 'w') as f:
        for rw, w, off, v, sz, pc in acc:
            f.write('%s %s+0x%04x 0x%08x %s\n' % (rw, w, off, v, e.img(pc)))
    print('== %s -> %s  (register accesses %d, OS calls %d, interface calls %d)' %
          (label, r, len(acc), len(e.calls) - c0, len(e.stub_calls) - s0))
    for line in e.logs:
        print('   log:', line)


step('3AA chain_create', tabs['3AA']['chain_create'], 0, w3, 0, e.cb)
cfg = e.alloc(40)
e.u.mem_write(cfg, struct.pack('<I', 1))
step('3AA set_system_config', tabs['3AA']['set_system_config'], cfg)
obj3, hw = e.alloc(8), e.alloc(0x2000)
info = 38 << 16              # obj_info: module id 38 at bit 16, chain 0, instance 0, OTF (interface-ddk.c:546+)
step('3AA object_create', tabs['3AA']['object_create'], obj3, info, hw)
print('3AA object =', e.img(e.q(obj3)))
step('ISP chain_create', tabs['ISP']['chain_create'], 0, wi, 0, e.cb)
obji = e.alloc(8)
step('ISP object_create', tabs['ISP']['object_create'], obji, info, hw)
print('ISP object =', e.img(e.q(obji)))
print('interrupts registered:', [(i['id'], i['chain'], e.img(i['handler'])) for i in e.irqs])
with open(os.path.join(OUT, 'interface-calls.txt'), 'w') as f:
    for name, lr, x, ret in e.stub_calls:
        f.write('%s from %s x1..3 %s -> %s\n' % (name, e.img(lr), ' '.join('0x%x' % v for v in x[1:4]), ret))
