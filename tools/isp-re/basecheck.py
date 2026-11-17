import sys,struct
b=open(sys.argv[1],'rb').read(); n=len(b); base=int(sys.argv[2],0)
ins=out=strs=pro=0; outs=[]
for o in range(0,n-7,8):
    v=struct.unpack_from('<Q',b,o)[0]
    if (v>>32)!=0xffffff80 or (v&0xffffffff)<0xf0000000: continue
    t=v-base
    if 0<=t<n:
        ins+=1
        if t+4<=n and (struct.unpack_from('<I',b,t)[0]&0xffc07fff) in (0xa9807bfd,0xa9007bfd): pro+=1
        elif t+3<=n and all(32<=c<127 for c in b[t:t+3]) and (t==0 or b[t-1]==0): strs+=1
    else: out+=1; outs.append(v)
print("size 0x%x base %x: in-file %d (prologue %d, string %d), outside %d"%(n,base,ins,pro,strs,out))
if outs: print("  outside range %x..%x"%(min(outs),max(outs)))
