# cmp_emu.py - native host run (test_front) vs emulator run (tools/isp-re/front_init.py) of the same front path.
#   python3 -I cmp_emu.py <dump dir> <emulator out dir (front_init.log + mmio-*.txt)> <host out dir>
# 1) per step: return value and the libraries' log lines (as multisets: tasks run concurrently in the host)
# 2) register windows: emulator final = phone dump + every logged write in step order; host final = regs-*.txt
import collections
import os
import re
import sys

DUMP, EMU, HOST = sys.argv[1:4]


def load(p):
    d = {}
    for line in open(p):
        f = line.split()
        if line[0] != '#' and len(f) == 2 and f[1].startswith('0x'):
            d[int(f[0], 16)] = int(f[1], 16)
    return d


def norm(s):
    # addresses differ between the runs (image base, heap, fake windows): mask hex numbers >= 0x10000
    s = re.sub(r'0x0x', '0x', s)
    return re.sub(r'0x[0-9a-fA-F]{5,}', '<addr>', s).strip()


emu_steps, cur = [], None
for line in open(os.path.join(EMU, 'front_init.log')):
    m = re.match(r'== (.*?) -> (\S+)', line)
    if m:
        cur = (m.group(1), m.group(2), [])
        emu_steps.append(cur)
    elif line.startswith('   log: ') and cur:
        cur[2].append(norm(line[8:]))
host_steps, buf = [], []
for line in open(os.path.join(HOST, 'host.log')):
    m = re.match(r'== (.*?) -> (\S+)', line)
    if m:
        host_steps.append((m.group(1), m.group(2), buf))
        buf = []
    elif line.strip() and not line.startswith(('isphost:', 'DIAG', 'interrupt registered', 'ISP object', '3AA object',
                                               'setfile:', 'calibration:', 'cal_info', 'DMA buffers',
                                               'register_sensor_interface', '[@][DRV]Register interrupt')):
        buf.append(norm(line))
hs = {s[0]: s for s in host_steps}
same = 0
for name, ret, logs in emu_steps:
    if name not in hs:
        print('%-26s only in the emulator run' % name)
        continue
    _, hret, hlogs = hs[name]
    a, b = collections.Counter(logs), collections.Counter(hlogs)
    only_e, only_h = a - b, b - a
    ok = (int(ret, 16) == int(hret, 16)) and not only_e and not only_h
    same += ok
    print('%-26s ret emu %s host %s, log lines %d / %d%s' % (name, ret, hret, len(logs), len(hlogs),
                                                            '' if ok else '  DIFF'))
    for k in list(only_e)[:6]:
        print('     emu only : %s' % k[:150])
    for k in list(only_h)[:6]:
        print('     host only: %s' % k[:150])
print('steps identical (return + log lines): %d / %d' % (same, len(emu_steps)))

for win, dump in (('3AA0', '3AA0'), ('ISP0', 'ISPLP')):
    ref = load(os.path.join(DUMP, dump + '.txt'))
    for name, _, _ in emu_steps:
        p = os.path.join(EMU, 'mmio-%s.txt' % name.replace(' ', '_'))
        if not os.path.exists(p):
            continue
        for line in open(p):
            f = line.split()
            if f[0] == 'W' and f[1].startswith(win + '+'):
                ref[int(f[1].split('+')[1], 16)] = int(f[2], 16)
    host = load(os.path.join(HOST, 'regs-%s.txt' % ('3AA0' if win == '3AA0' else 'ISPLP')))
    diff = [o for o in sorted(set(ref) | set(host)) if ref.get(o, 0) != host.get(o, 0)]
    print('%s: %d words, %d differ' % (win, len(set(ref) | set(host)), len(diff)))
    for o in diff[:40]:
        print('   +0x%04x emulator 0x%08x host 0x%08x' % (o, ref.get(o, 0), host.get(o, 0)))

# 3) register ACCESS sequences per step (mmio-<step>.txt of both runs): exact order, then as multisets
print('register accesses per step (emulator / host): exact-order equal, multiset equal')
tot = [0, 0, 0]
for name, _, _ in emu_steps:
    fn = 'mmio-%s.txt' % name.replace(' ', '_')
    pe, ph = os.path.join(EMU, fn), os.path.join(HOST, fn)
    if not (os.path.exists(pe) and os.path.exists(ph)):
        continue
    a = [l.split()[:4] for l in open(pe)]
    b = [l.split()[:4] for l in open(ph)]
    tot[0] += 1
    tot[1] += a == b
    ms = collections.Counter(map(tuple, a)) == collections.Counter(map(tuple, b))
    tot[2] += ms
    if a != b:
        k = next((i for i in range(min(len(a), len(b))) if a[i] != b[i]), min(len(a), len(b)))
        print('  %-24s %5d / %5d  order %s multiset %s; first difference #%d: emu %s | host %s' % (
            name, len(a), len(b), 'same' if a == b else 'DIFF', 'same' if ms else 'DIFF', k,
            ' '.join(a[k]) if k < len(a) else '-', ' '.join(b[k]) if k < len(b) else '-'))
print('steps: %d, identical order %d, identical multiset %d' % tuple(tot))
