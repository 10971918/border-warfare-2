# For each dvar name: registration site -> global dvar_t* it is stored in -> functions reading it.
import sys, capstone
from pe64 import Image
img = Image(sys.argv[1])
for name in sys.argv[2:]:
    for srva, s in img.strings("^" + name + "$"):
        for site, _ in img.xrefs(srva):
            glob = None
            for ins in img.md.disasm(bytes(img.mem[site:site+160]), img.base+site):
                if ins.mnemonic == "mov" and ins.op_str.startswith("qword ptr [rip") and ins.op_str.endswith(", rax"):
                    glob = ins.address - img.base + ins.size + ins.operands[0].mem.disp; break
            if glob is None: print(name, "reg %x: no store" % site); continue
            users = sorted(set((img.func_of(x)[0] if img.func_of(x) else x) for x,t in img.xrefs(glob) if t.startswith("mov") and not t.split(",")[0].endswith("]")))
            print("%-24s dvar@%x  readers: %s" % (name, glob, " ".join("%x" % u for u in users)))
