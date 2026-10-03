import sys
from pe64 import Image
img = Image(sys.argv[1]); root = int(sys.argv[2],16); depth = int(sys.argv[3]) if len(sys.argv)>3 else 3
seen=set()
def callers(f):
    out=set()
    for site,txt in img.xrefs(f):
        if txt.startswith(("call","jmp")):
            fr=img.func_of(site); out.add(fr[0] if fr else site)
    return sorted(out)
def walk(f,d):
    cs=callers(f); fr=img.func_of(f)
    print("  "*d+"%x (size %x, %d callers)%s"%(f,(fr[1]-fr[0]) if fr else 0,len(cs)," *" if f in seen else ""))
    if f in seen or d>=depth: return
    seen.add(f)
    if len(cs)>25: print("  "*(d+1)+"... %d callers"%len(cs)); return
    for c in cs: walk(c,d+1)
walk(root,0)
