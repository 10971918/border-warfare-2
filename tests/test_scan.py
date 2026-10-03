"""Offline test of sdk_mods/bw2/scan.py against a synthetic BL2-like world.

The tracer behaves like UE3 line checks: surfaces are hit only from outside (a ray starting inside a
solid passes through it), terrain is one-sided. Checks that the boxes the scanner produces are solid
where the world is solid and free where a player could stand.
  python tests/test_scan.py
"""
import math
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "sdk_mods", "bw2"))
import scan  # noqa: E402

# ---- synthetic world (BL2 units) ----
BOXES = [
    # building: 4 walls, 2000 tall (taller than the scan range above ground), 40 thick
    (100, 100, 0, 600, 140, 2000), (100, 560, 0, 600, 600, 2000),
    (100, 100, 0, 140, 600, 2000), (560, 100, 0, 600, 360, 2000),  # gap in the east wall = door at y 360..600? no: door 360..560
    (560, 560, 0, 600, 600, 2000),
    (250, 250, 70, 330, 330, 80),        # table top (underside at 70)
    (-400, -400, 300, -100, -100, 320),  # second floor slab outside
    (300, -300, 0, 340, -260, 150),      # pillar outside
]
# UE3 simple (convex/box) collision: a trace that starts inside reports a hit at its start (found in BL2's
# Sanctuary: the space above the player was inside a static mesh's collision hull)
CONVEX = [
    (-700, 200, 0, -500, 400, 2000),     # tower taller than the scan range
    (-300, 200, 250, 0, 500, 1500),      # canopy over open ground, reaching above the scan range
    (200, -700, 270, 500, -500, 930),    # Sanctuary case: thick arch, free space under it ...
    (200, -700, -500, 500, -500, 2),     # ... above a convex ground hull (top at 2)
]


def terrain_z(x, y):
    return 0.0 if x < 700 else (x - 700) * 0.3  # flat, then a 17-degree hill to the east


def ray_box(o, d, b):
    tmin, tmax, axis_hit = -1e30, 1e30, None
    for a in range(3):
        if abs(d[a]) < 1e-12:
            if o[a] < b[a] or o[a] > b[a + 3]:
                return None
            continue
        t1, t2 = (b[a] - o[a]) / d[a], (b[a + 3] - o[a]) / d[a]
        n1 = -1 if d[a] > 0 else 1
        if t1 > t2:
            t1, t2 = t2, t1
        if t1 > tmin:
            tmin, axis_hit = t1, (a, n1)
        tmax = min(tmax, t2)
    if tmin > tmax or tmax < 0 or tmin < 0:  # starts inside (tmin < 0): no front face hit
        return None
    n = [0.0, 0.0, 0.0]
    n[axis_hit[0]] = float(axis_hit[1])
    return tmin, tuple(n)


def trace(start, end):
    d = tuple(e - s for s, e in zip(start, end))
    length = math.sqrt(sum(v * v for v in d))
    best = None
    for b in BOXES:
        h = ray_box(start, d, b)
        if h and h[0] <= 1.0 and (best is None or h[0] < best[0]):
            best = h
    for b in CONVEX:
        if all(b[a] < start[a] < b[a + 3] for a in range(3)):
            return tuple(start), (0.0, 0.0, 1.0)  # inside: hit at the start
        h = ray_box(start, d, b)
        if h and h[0] <= 1.0 and (best is None or h[0] < best[0]):
            best = h
    # terrain: one-sided heightfield, marched coarsely then refined
    if start[2] > terrain_z(start[0], start[1]):
        steps = max(2, int(length / 4))
        prev = 0.0
        for k in range(1, steps + 1):
            t = k / steps
            p = [s + v * t for s, v in zip(start, d)]
            if p[2] <= terrain_z(p[0], p[1]):
                lo, hi = prev, t
                for _ in range(20):
                    m = (lo + hi) / 2
                    q = [s + v * m for s, v in zip(start, d)]
                    if q[2] <= terrain_z(q[0], q[1]):
                        hi = m
                    else:
                        lo = m
                n = (0.0, 0.0, 1.0) if p[0] < 700 else (-0.287, 0.0, 0.958)
                if best is None or hi < best[0]:
                    best = (hi, n)
                break
            prev = t
    if best is None:
        return None
    t, n = best
    return tuple(s + v * t for s, v in zip(start, d)), n


def solid_at(boxes, p):
    return any(b[0] <= p[0] <= b[3] and b[1] <= p[1] <= b[4] and b[2] <= p[2] <= b[5] for b in boxes)


def main():
    t0 = time.perf_counter()
    boxes, traces = [], 0
    for tx in (-1, 0, 1):
        for ty in (-1, 0):
            ts = scan.TileScan(tx, ty, 0.0, trace)
            for _ in ts.run():
                pass
            boxes += ts.boxes
            traces += ts.traces
    dt = time.perf_counter() - t0
    print("6 tiles: %d traces, %d boxes, %.2f s (synthetic tracer)" % (traces, len(boxes), dt))
    checks = [
        ("ground is solid just below z 0", (50, 50, -5), True),
        ("air above ground is free", (50, 50, 60), False),
        ("inside the tall north wall (chest height)", (350, 120, 120), True),
        ("inside the tall west wall (knee height)", (120, 350, 50), True),
        ("inside the building is free", (350, 450, 60), False),
        ("door gap in the east wall is free", (580, 450, 60), False),
        ("table top is solid", (290, 290, 75), True),
        ("under the table is free", (290, 290, 40), False),
        ("second floor slab is solid", (-250, -250, 310), True),
        ("between ground and the slab is free", (-250, -250, 150), False),
        ("pillar is solid", (320, -280, 100), True),
        ("hill surface below is solid", (800, 50, 20), True),
        ("air above the hill is free", (800, 50, 70), False),
        ("convex tower solid at ground level", (-600, 300, 50), True),
        ("convex tower solid high up", (-600, 300, 1000), True),
        ("under the convex canopy is free", (-150, 350, 100), False),
        ("convex canopy is solid", (-150, 350, 600), True),
        ("ground under the canopy is solid", (-150, 350, -5), True),
        ("beside the tower is free", (-450, 300, 100), False),
        ("standing room under the thick arch is free", (350, -600, 150), False),
        ("the thick arch is solid", (350, -600, 600), True),
        ("convex ground under the arch is solid", (350, -600, -40), True),
    ]
    bad = 0
    for name, p, want in checks:
        got = solid_at(boxes, p)
        print("%-45s %s" % (name, "ok" if got == want else "FAIL (solid=%s)" % got))
        bad += got != want
    if bad:
        sys.exit("test_scan: %d failures" % bad)
    print("test_scan OK")


if __name__ == "__main__" and "--export" not in sys.argv:
    main()


def export_guest(path):
    """Scan the synthetic world and write guest-space boxes (anchor: BL2 origin = MW2 origin) for
    tests/walk_test.exe: first line = tile bounds (guest), then one box per line."""
    sys.path.insert(0, os.path.join(HERE, "..", "tools"))
    import bw2_coords as C
    a = C.Anchor()
    boxes = []
    for tx in (-1, 0, 1):
        for ty in (-1, 0):
            ts = scan.TileScan(tx, ty, 0.0, trace)
            for _ in ts.run():
                pass
            boxes += [scan.to_guest(a, b) for b in ts.boxes]
    lo = scan.to_guest(a, (-scan.TILE, -scan.TILE, -scan.Z_HALF, 2 * scan.TILE, scan.TILE, scan.Z_HALF))
    with open(path, "w") as f:
        f.write(" ".join("%.3f" % v for v in lo) + "\n")
        for b in boxes:
            f.write(" ".join("%.3f" % v for v in b) + "\n")
    print("exported %d guest boxes to %s" % (len(boxes), path))


if __name__ == "__main__" and "--export" in sys.argv:
    export_guest(sys.argv[sys.argv.index("--export") + 1])
