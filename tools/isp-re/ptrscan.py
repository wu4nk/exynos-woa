import sys, struct, collections
b = open(sys.argv[1],'rb').read()
start = int(sys.argv[2],0)
hi = collections.Counter(); hits = []
for off in range(start - start % 8, len(b) - 7, 8):
    v = struct.unpack_from('<Q', b, off)[0]
    if (v >> 32) in (0xffffff80, 0xffffff81, 0xffffffc0, 0xffffff00):
        hi[hex(v >> 24)] += 1
        if len(hits) < 15: hits.append((hex(off), hex(v)))
print("kernel-VA-looking 64-bit words after", hex(start), ":", sum(hi.values()), hi.most_common(8))
print(hits)
