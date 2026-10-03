# Calls (direct + indirect) made from code in [lo,hi): which targets, how often.
import sys, collections
from pe64 import Image
img = Image(sys.argv[1]); lo=int(sys.argv[2],16); hi=int(sys.argv[3],16)
direct=collections.Counter(); indirect=collections.Counter()
for b,e in img.funcs():
    if not (lo<=b<hi): continue
    for ins in img.md.disasm(bytes(img.mem[b:e]), img.base+b):
        if ins.mnemonic=="call":
            if ins.op_str.startswith("0x"): direct[int(ins.op_str,16)-img.base]+=1
            else: indirect[ins.op_str]+=1
print("indirect:", indirect.most_common(15))
for t,n in direct.most_common(60):
    fr=img.func_of(t); print("%x x%d size=%x" % (t,n,(fr[1]-fr[0]) if fr else 0), end=" | ")
