import sys,struct,collections
b=open(sys.argv[1],'rb').read(); n=len(b)
ptrs=[]
for o in range(0,n-7,8):
    v=struct.unpack_from('<Q',b,o)[0]
    if (v>>32)==0xffffff80 and (v&0xffffffff)>=0xf0000000: ptrs.append((o,v))
print("ptrs",len(ptrs),"min %x max %x"%(min(v for _,v in ptrs),max(v for _,v in ptrs)))
# score candidate bases on 4 KiB granularity: target must be in file and be a printable string start or a stp x29,x30 prologue
def score(base):
    s=0
    for o,v in ptrs:
        t=v-base
        if 0<=t<n-4:
            w=struct.unpack_from('<I',b,t)[0]
            if (w&0xffc07fff)==0xa9807bfd or (w&0xffc07fff)==0xa9007bfd: s+=2
            elif 32<=b[t]<127 and 32<=b[t+1]<127 and 32<=b[t+2]<127 and (t==0 or b[t-1]==0): s+=1
    return s
lo=min(v for _,v in ptrs)-n; hi=min(v for _,v in ptrs)
cands=collections.Counter()
for base in range((lo>>12)<<12, hi+0x1000, 0x1000): 
    pass
# cheaper: candidates from ptr - offsets of prologues
pro=[t for t in range(0,n-4,4) if (struct.unpack_from('<I',b,t)[0]&0xffc07fff) in (0xa9807bfd,0xa9007bfd)]
pros=set(pro)
for o,v in ptrs[:3000]:
    for t in pro[::1]:
        pass
# histogram of (v - t) for v in sample ptrs, t in prologues, only for low 12-bit match
bylow=collections.defaultdict(list)
for t in pro: bylow[t&0xfff].append(t)
for o,v in ptrs:
    for t in bylow.get(v&0xfff,[]): cands[v-t]+=1
for base,c in cands.most_common(5): print("base %x hits %d score %d"%(base,c,score(base)))
