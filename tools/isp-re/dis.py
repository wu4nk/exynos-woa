import sys
from capstone import *
b=open(sys.argv[1],'rb').read(); md=Cs(CS_ARCH_ARM64, CS_MODE_ARM)
for a in sys.argv[2].split(','):
    off=int(a,0); n=int(sys.argv[3]) if len(sys.argv)>3 else 12
    print("--", hex(off))
    for i in md.disasm(b[off:off+4*n], off): print("   %06x: %-8s %s"%(i.address,i.mnemonic,i.op_str))
