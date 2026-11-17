import sys, struct, collections
from capstone import *
from capstone.arm64 import *
path = sys.argv[1]; areas = [tuple(int(x,0) for x in a.split(':')) for a in sys.argv[2].split(',')]; entries = [int(x,0) for x in sys.argv[3].split(',')]
b = open(path, 'rb').read()
print("size", hex(len(b)), "code areas", [(hex(a),hex(e)) for a,e in areas])
md = Cs(CS_ARCH_ARM64, CS_MODE_ARM); md.detail = True
def dis(off, n=12):
    for i in md.disasm(b[off:off+4*n], off): print("   %06x: %-8s %s" % (i.address, i.mnemonic, i.op_str))
for off in entries:
    print("entry +%#x:" % off); dis(off, 10)
# scan code area: x18 use, fp/simd use, system instructions, svc/hvc/smc, unknown words
x18r = x18w = 0; x18sites = []; simd = 0; sysins = collections.Counter(); bad = 0; n = 0
X18 = {ARM64_REG_X18, ARM64_REG_W18}
import itertools
offs = itertools.chain.from_iterable(range(a, min(e, len(b)), 4) for a, e in areas)
for off in offs:
    w = b[off:off+4]
    ins = list(md.disasm(w, off))
    if not ins: bad += 1; continue
    i = ins[0]; n += 1
    try:
        r, wr = i.regs_access()
    except CsError:
        r, wr = [], []
    if X18 & set(r): x18r += 1; x18sites.append(i.address)
    if X18 & set(wr): x18w += 1; x18sites.append(i.address)
    if any(op.type == ARM64_OP_REG and (ARM64_REG_V0 <= op.reg <= ARM64_REG_V31 or ARM64_REG_Q0 <= op.reg <= ARM64_REG_Q31 or ARM64_REG_D0 <= op.reg <= ARM64_REG_D31 or ARM64_REG_S0 <= op.reg <= ARM64_REG_S31) for op in i.operands):
        simd += 1
    if i.mnemonic in ('svc','hvc','smc','msr','mrs','eret','dc','ic','tlbi','isb','dsb','dmb','wfi','wfe','sev','brk','hlt'):
        sysins[i.mnemonic + (' ' + (i.op_str.split(',')[0] if i.mnemonic in ('msr','dc','ic','tlbi') else i.op_str.split(',')[-1]) if i.mnemonic in ('msr','mrs','dc','ic','tlbi') else '')] += 1
print("decoded", n, "undecodable words", bad)
print("x18 reads", x18r, "writes", x18w, "first sites", [hex(a) for a in x18sites[:12]])
print("fp/simd register instructions", simd)
print("system-ish instructions", sysins.most_common(30))
