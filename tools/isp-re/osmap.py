# Stage 1 helper: emulate the DDK start_up path from 0xf25b8 up to a stop offset with marker OS-table values;
# report where each table index is stored and which OS functions get called. Image loaded at its Android VA.
import sys,struct
from unicorn import *
from unicorn.arm64_const import *
b=open(sys.argv[1],'rb').read(); STOP=int(sys.argv[2],0) if len(sys.argv)>2 else 0xf28e0
BASE=0xffffff80f9fe0000
u=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
SZ=0x05000000; u.mem_map(BASE,SZ); u.mem_write(BASE,b)          # image + its bss / heap neighbourhood
TAB=0x900000; u.mem_map(TAB,0x1000)
MK=0xAB000000
for i in range(100): u.mem_write(TAB+8*i,struct.pack('<Q',MK+4*i))
u.mem_map(MK,0x1000); u.mem_write(MK,b'\xc0\x03\x5f\xd6'*0x400)  # every OS function = ret, w0 = whatever
calls=[]
def oscall(uc,a,s,d):
    calls.append(((a-MK)//4, uc.reg_read(UC_ARM64_REG_X30)-BASE, uc.reg_read(UC_ARM64_REG_X0)))
    if (a-MK)//4 in (0,29): print('LOG:', bytes(uc.mem_read(uc.reg_read(UC_ARM64_REG_X0),200)).split(b'\0')[0])
    uc.reg_write(UC_ARM64_REG_X0,0)
u.hook_add(UC_HOOK_CODE,oscall,begin=MK,end=MK+0xfff)
def bad(uc,t,a,s,v,d):
    print("UNMAPPED %d at 0x%x  pc=img+0x%x"%(t,a,uc.reg_read(UC_ARM64_REG_PC)-BASE)); return False
u.hook_add(UC_HOOK_MEM_UNMAPPED,bad)
STK=0xA00000; u.mem_map(STK,0x10000)
u.reg_write(UC_ARM64_REG_SP,STK+0x8000); u.reg_write(UC_ARM64_REG_X0,TAB); u.reg_write(UC_ARM64_REG_X30,0)
u.reg_write(UC_ARM64_REG_CPACR_EL1,0x300000)
try: u.emu_start(BASE+0xf25b8,BASE+STOP)
except UcError as e: print("stopped:",e)
data=u.mem_read(BASE+0x420000,0x40000); hits={}
for o in range(0,len(data),8):
    v=struct.unpack_from('<Q',data,o)[0]
    if MK<=v<MK+400: hits[(v-MK)//4]=0x420000+o
for k in sorted(hits): print("os[%2d] -> global img+0x%x"%(k,hits[k]))
for k,r,x0 in calls: print("call os[%d] from img+0x%x x0=0x%x"%(k,r,x0))
