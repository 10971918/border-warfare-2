import sys, collections, struct
from pe64 import Image
img = Image(sys.argv[1]); lo=int(sys.argv[2],16); hi=int(sys.argv[3],16)
tables=collections.Counter()
for b,e in img.funcs():
    if not (lo<=b<hi): continue
    last={}
    for ins in img.md.disasm(bytes(img.mem[b:e]), img.base+b):
        if ins.mnemonic=="lea" and "rip" in ins.op_str:
            last[ins.op_str.split(",")[0]] = ins.address-img.base+ins.size+ins.operands[1].mem.disp
        if ins.mnemonic=="call" and "*8]" in ins.op_str:
            reg=ins.op_str.split("[")[1].split(" ")[0]
            if reg in last: tables[(last[reg])]+=1
for t,n in tables.most_common():
    print("table %x used %d times" % (t,n))
    for i in range(0,12):
        q=struct.unpack_from("<Q",img.mem,t+8*i)[0]
        r=q-img.base if q else 0
        print("   [%2d] %x callers=%d" % (i,r,len(img.xrefs(r)) if 0<r<len(img.mem) else -1))
