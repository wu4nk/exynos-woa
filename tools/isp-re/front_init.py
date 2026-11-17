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


# The sensor's answers = what this phone's Android reported (isfw-msg-android.txt WRAP_GetProperties, 30 fps
# preview mode; the one-frame 120 fps AE warm-up mode is deliberately not used). Signatures: GPL
# fimc-is-interface-sensor.c (getters with u32 * outputs write them, u32 getters return the value).
S = dict(cal=(3968, 2736), bayer=0, exp=(23, 33257), again=(1000, 16000), dgain=(1000, 128000),
         init=(2360, 1000, 1000), timing=(807720000, 9384, 2864, 0), cur=(3648, 2736), fps=30, use_dgain=1,
         aperture=190, module_id=38, position=1)     # module 38 = SENSOR_NAME_S5K3J1, position 1 = FRONT


def wr(*vals):
    def h(e, itf, *ptrs):
        for p, v in zip(ptrs, vals):
            w32(e, p, v)
        return 0
    return h


itf = e.sensor_itf(LAYOUT, {
    'cis_itf_ops.get_module_id': wr(S['module_id']),
    'cis_itf_ops.get_module_position': wr(S['position']),
    'cis_itf_ops.get_calibrated_size': wr(*S['cal']),
    'cis_itf_ops.get_bayer_order': wr(S['bayer']),
    'cis_itf_ops.get_min_exposure_time': lambda e, *a: S['exp'][0],
    'cis_itf_ops.get_max_exposure_time': lambda e, *a: S['exp'][1],
    'cis_itf_ops.get_min_analog_gain': lambda e, *a: S['again'][0],
    'cis_itf_ops.get_max_analog_gain': lambda e, *a: S['again'][1],
    'cis_itf_ops.get_min_digital_gain': lambda e, *a: S['dgain'][0],
    'cis_itf_ops.get_max_digital_gain': lambda e, *a: S['dgain'][1],
    'cis_itf_ops.get_initial_exposure_gain_of_sensor': wr(*(S['init'] * 2)),
    'cis_itf_ops.get_sensor_frame_timing': wr(*S['timing']),
    'cis_itf_ops.get_sensor_cur_size': wr(*S['cur']),
    'cis_itf_ops.get_sensor_max_fps': wr(S['fps']),
    'cis_itf_ops.get_hdr_ratio_ctl_by_again': wr(0),
    'cis_itf_ops.get_sensor_use_dgain': wr(S['use_dgain']),
    'cis_itf_ops.get_sensor_initial_aperture': wr(S['aperture']),
    'cis_itf_ops.is_actuator_available': lambda e, *a: 1,      # Android: "Ext AF is selected!", AF cal 384/554
    # struct vc_buf_info_t {stat_type, sensor_mode, width, height, element_size} (interface-sensor.h:149); values
    # = Android's preview "[IspObject_InitPdafLib] requestType ..." lines; other types INVALID (-1) as GPL memset+:2732
    'csi_itf_ops.get_vc_dma_buf_info': lambda e, itf, t, p, *a: (e.u.mem_write(p, struct.pack(
        '<iiIII', *({1: (500, 102, 1756, 1, 1), 3: (501, 102, 39424, 1, 1)}.get(t, (-1, -1, 0, 0, 0))))), 0)[1],
})
print('register_sensor_interface ->', e.call(e.DDK + 0x40, itf) & 0xffffffff)
# the same interface is also registered with the RTA (GPL fimc-is-device-sensor_v2.c:2934, RTA_LIB_ADDR + 0x40)
print('register_sensor_interface (RTA) ->', e.call(e.RTA + 0x40, itf) & 0xffffffff)
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
info_isp = (38 << 16) | (1 << 12)      # Android: DICO_Object_Isp_Create (0x00261000) input(1) = M2M
step('ISP object_create', tabs['ISP']['object_create'], obji, info_isp, hw)
print('ISP object =', e.img(e.q(obji)))
# --- setfile (GPL hardware/fimc-is-hw-control.c parse_setfile_header / parse_setfile_info, V3 header
# hw-control.h:192): IP 0 = 3AA_ISP (designed bit 3), 29 tune sets on this phone's setfile_3j1.bin
SF = open(os.path.join(BIN, 'setfile_3j1.bin'), 'rb').read()
magic, designed, ver, rev, nscen, nips, ent_off = struct.unpack_from('<II4s4sIII', SF, 0)
num_base = 28 + nips * nscen * 4
tab_base = num_base + nips * 4
count0 = struct.unpack_from('<I', SF, num_base)[0]
print('setfile: magic 0x%x designed 0x%x %s %s scenarios %d ips %d, 3AA_ISP tune sets %d' %
      (magic, designed, ver.decode(), rev.decode(), nscen, nips, count0))
sf_mem = e.alloc(len(SF), 4096)
e.u.mem_write(sf_mem, SF)
ts = e.alloc(24)
e.logs.clear()
for idx in range(count0):
    off, size = struct.unpack_from('<II', SF, tab_base + 8 * idx)
    # struct lib_tune_set {u32 index; ulong addr; u32 size; int decrypt_flag} (interface-ddk.h:181), flag = V3
    e.u.mem_write(ts, struct.pack('<IIQII', idx, 0, sf_mem + ent_off + off, size, 1))
    r = e.call(tabs['3AA']['create_tune_set'], e.q(obj3), 0, ts) & 0xffffffff
    if r:
        print('create_tune_set %d -> 0x%x' % (idx, r))
print('create_tune_set x%d done; log lines %d (first: %s)' % (count0, len(e.logs), e.logs[:2]))
# Android applied 3AA tune set 7 (Fast AE, one frame) then 6 for the preview (isfw-msg-android.txt :200 / :316):
# only the preview one here
step('3AA apply_tune_set 6', tabs['3AA']['apply_tune_set'], e.q(obj3), 0, 6)
# Android then loads the module calibration (EEPROM, kernel kvaddr_cal[FRONT]; hw-3aa.c apply_setfile) and reads
# the LSC centre back (isfw-msg-android.txt: "DICO_LoadCalData addr:0x1f752000", "GetCalibrationData ... type(2)
# centerX/Y(1888/1400)"). We do not have this phone's front EEPROM contents yet: a ZERO buffer stands in -
# PLACEHOLDER, every calibration-dependent result after this point is not Android's.
CAL = e.alloc(64 * 1024, 4096)
step('3AA load_cal_data (zeros)', tabs['3AA']['load_cal_data'], e.q(obj3), 0, CAL)
CI = e.alloc(64)
step('3AA get_cal_data type 2', tabs['3AA']['get_cal_data'], e.q(obj3), 0, CI, 2)
print('cal_info data[0..3]:', [e.d(CI + 4 * k) for k in range(4)])

# --- first preview frame: set_param / set_ctrl / shot for the 3AA (GPL hw-3aa.c shot path: lib set_param when
# params change, lib set_ctrl(object, instance, fcount, shot), lib shot(object, param_set, shot, num_buffers)).
# Struct layouts: param_layouts.txt (clang record layout of the GPL v7_0_0 headers). Values = Android's own log of
# this phone's preview (isfw-msg-android.txt :318-322): otf input 3648 x 2736 (calibrated 3968 x 2736, crop 160),
# bayer crop (352, 264) 2944 x 2208, DMA out before BDS 2944 x 2208 "11/12bits", after BDS 2432 x 1824, 7..30 fps.
# The ENCODING of format / bitwidth / msb is inferred from the enum names (fimc-is-param.h) - UNVERIFIED.
P = e.alloc(1776)


def put(off, *vals):
    e.u.mem_write(P + off, struct.pack('<%dI' % len(vals), *vals))


put(0, 33333, 7, 30, 3648, 2736, 1000, 1000, 1000, 1000, 0, 0, 3648, 2736, 3968, 2736)   # sensor_config
put(128, 1, 0, 10, 0, 3648, 2736, 352, 264, 2944, 2208)        # otf_input: cmd, format, bitwidth, order, w, h, crop
put(640, 1, 0, 12, 0, 1, 2944, 2208, 0, 0, 2944, 2208, 0, 11)  # dma_output_before_bds .. crop_enable, msb
put(768, 1, 0, 12, 0, 1, 2432, 1824, 0, 0, 2432, 1824, 0, 11)  # dma_output_after_bds
bufs = {}
for name, size, off in (('before_bds', 2944 * 2208 * 2, 1348), ('after_bds', 2432 * 1824 * 2, 1416)):
    k = e.handlers[68](e, size)                                  # fake DMA pool, dva = 0x8xxxxxxx
    dv = e.alloc(4)
    e.handlers[65](e, k, dv)
    bufs[name] = (k, e.d(dv))
    put(off, e.d(dv))
put(1760, 0, 1)                                                 # instance_id 0, fcount 1
SHOT = e.alloc(36592)
e.u.mem_write(SHOT + 36584, struct.pack('<I', 0x23456789))     # magicNumber (SHOT_MAGIC_NUMBER)
e.u.mem_write(SHOT + 148, struct.pack('<II', 7, 30))           # ctl.aa.aeTargetFpsRange
e.u.mem_write(SHOT + 216, struct.pack('<I', 1))                # ctl.aa.captureIntent PREVIEW
e.u.mem_write(SHOT + 224, struct.pack('<II', 2, 1))            # ctl.aa.mode AUTO, sceneMode DISABLED (Android 2 / 1)
e.u.mem_write(SHOT + 648, struct.pack('<QQ', 33333000, 33333333))  # ctl.sensor exposureTime, frameDuration (ns)
print('DMA buffers (kva, dva):', {k: (hex(a), hex(b)) for k, (a, b) in bufs.items()})
step('3AA set_param', tabs['3AA']['set_param'], e.q(obj3), P)
step('3AA set_ctrl F1', tabs['3AA']['set_ctrl'], e.q(obj3), 0, 1, SHOT)
step('3AA shot F1', tabs['3AA']['shot'], e.q(obj3), P, SHOT, 1)

# --- frame interrupts: Android's ISR order per 3AA frame is V SYNC -> CIN ROW -> FRAME END (isfw-msg-android.txt
# "TransitIsrState"). The handler gets (interrupt id, ctx) (GPL struct hwip_intr_handler / fimc_is_register_interrupt);
# which status bits mean which event is not known yet: the handler reads them from the (live-dump) registers.
for irq in [i for i in e.irqs if i['chain'] == 0]:
    step('3AA irq %d' % irq['id'], irq['handler'], irq['id'], irq['ctx'])
# status registers read by the handler (assembly ddk+0x2f2bd4): 3AA +0x10c (irq 0), +0x11c (irq 1). Discovery: set
# one bit at a time and record what the DDK does with it (logs + register accesses) - DIAGNOSIS ONLY
if os.environ.get('IRQ_SCAN'):
    for irq in [i for i in e.irqs if i['chain'] == 0]:
        st = 0x10c if irq['id'] == 0 else 0x11c
        for bit in range(32):
            e.u.mem_write(w3 + st, struct.pack('<I', 1 << bit))
            e.logs.clear()
            n0 = len(e.mmio)
            try:
                e.call(irq['handler'], irq['id'], irq['ctx'])
                r = 'ok'
            except RuntimeError as x:
                r = 'EXC ' + str(x)[:80]
            acc = [a for a in e.mmio[n0:] if not (a[0] == 'R' and a[2] == st)]
            print('IRQ%d bit %2d: %s, %d other accesses; %s' % (irq['id'], bit, r, len(acc),
                  ' | '.join(l for l in e.logs if 'null interrupt' not in l)[:300]))
print('interrupts registered:', [(i['id'], i['chain'], e.img(i['handler'])) for i in e.irqs])
with open(os.path.join(OUT, 'interface-calls.txt'), 'w') as f:
    for name, lr, x, ret in e.stub_calls:
        f.write('%s from %s x1..3 %s -> %s\n' % (name, e.img(lr), ' '.join('0x%x' % v for v in x[1:4]), ret))
