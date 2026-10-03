import sys, collections
from pe64 import Image
img = Image(sys.argv[1]); CM = 0x131f800
F = {0x10:"planes",0x40:"brushsides",0x58:"numNodes",0x60:"nodes",0x68:"numLeafs",0x70:"leafs",0x80:"leafbrushNodes",0x90:"leafbrushes",0xa0:"leafsurfaces",0xb0:"verts",0xc0:"triIndices",0xc8:"triWalk",0xe8:"partitions",0xf8:"aabbTrees",0x100:"numSubModels",0x108:"cmodels",0x110:"numBrushes",0x118:"brushes",0x120:"brushBounds",0x128:"brushContents",0x140:"smodelNodes",0x0:"name",0x8:"isInUse",0x20:"staticModelList",0x30:"materials"}
byf = collections.defaultdict(set)
for off,name in F.items():
    for site,_ in img.xrefs(CM+off):
        f = img.func_of(site); byf[f[0] if f else site].add(name)
for f in sorted(byf):
    fr = img.func_of(f); n = len(img.xrefs(f))
    print("%x size=%-5x callers=%-3d %s" % (f, (fr[1]-fr[0]) if fr else 0, n, ",".join(sorted(byf[f]))))
