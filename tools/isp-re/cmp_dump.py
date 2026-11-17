# cmp_dump.py OUT DUMP WINDOW [N] - last value the emulator wrote per register (all mmio-*.txt of a front_init run,
# in file order) vs the phone dump; skips 0xdeadc0de (write-only / unimplemented) and DMA addresses (ours 0x8xxxxxxx).
# compare the emulator's last written 3AA0 values with the phone's live dump, excluding registers that cannot match:
# dump value 0xdeadc0de (write-only / unimplemented) and DMA addresses (ours 0x8xxxxxxx, Android IOVA)
import os,sys,collections
O=sys.argv[1]; dump=sys.argv[2]
D={}
for l in open(dump):
    f=l.split()
    if l[0]!='#' and len(f)==2: D[int(f[0],16)]=int(f[1],16)
files=sorted(os.listdir(O), key=lambda n: os.path.getmtime(os.path.join(O,n)))
last={}
for n in files:
    if not n.startswith('mmio-'): continue
    for l in open(os.path.join(O,n)):
        rw,loc,v,pc=l.split()
        if rw=='W' and loc.startswith(sys.argv[3]): last[int(loc.split('+')[1],16)]=int(v,16)
cmp=[(o,v) for o,v in last.items() if D.get(o) not in (None,0xdeadc0de) and (v>>28)!=8]
same=[o for o,v in cmp if D[o]==v]
print('written %d, comparable %d, equal %d = %.1f%%'%(len(last),len(cmp),len(same),100*len(same)/len(cmp)))
diff=[(o,v,D[o]) for o,v in cmp if D[o]!=v]
print('diff by 256-B block:',[(hex(k),n) for k,n in collections.Counter((o>>8)<<8 for o,_,_ in diff).most_common(20)])
for o,v,a in sorted(diff)[:int(sys.argv[4]) if len(sys.argv)>4 else 30]: print('  +0x%04x ours 0x%08x phone 0x%08x'%(o,v,a))
