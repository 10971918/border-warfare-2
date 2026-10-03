"""Records MW2's published position (GuestState) and BL2's pawn position (HostState.puppetPos) from the live
link at ~1 kHz, then reports backward steps ("micro lag back, then rubber band forward").

    python tools/jitter_probe.py [seconds] [out.csv] [--wait]

--wait: start recording only once MW2's player starts moving (up to 10 minutes).

Read-only: it never writes to the mapping. Run it while running forward in a straight line.
"""
from __future__ import annotations

import csv
import math
import sys
import time

import bw2_proto as P


def backward_steps(rows: list[tuple], key: int) -> tuple[int, float, int]:
    """Steps against the direction of travel (over the last ~100 ms) for column `key` (an xyz tuple):
    count, worst (MW2 inches), and how many samples moved at all."""
    n = worst = moved = 0
    for i in range(1, len(rows)):
        p0, p1 = rows[i - 1][key], rows[i][key]
        d = (p1[0] - p0[0], p1[1] - p0[1])
        if math.hypot(*d) < 1e-3:
            continue
        moved += 1
        j = max(0, i - 100)
        travel = (p1[0] - rows[j][key][0], p1[1] - rows[j][key][1])
        t = math.hypot(*travel)
        if t < 5.0:  # not really travelling
            continue
        along = (d[0] * travel[0] + d[1] * travel[1]) / t
        if along < -0.05:
            n += 1
            worst = max(worst, -along)
    return n, worst, moved


def main() -> None:
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 60.0
    out = sys.argv[2] if len(sys.argv) > 2 and not sys.argv[2].startswith("--") else None
    link = P.Link()
    if "--wait" in sys.argv:
        g0 = link.read_slot(P.OFF_GUEST, P.GUEST)
        t0 = time.perf_counter()
        while time.perf_counter() - t0 < 600:
            g = link.read_slot(P.OFF_GUEST, P.GUEST)
            if g and g0 and math.dist(g["origin"], g0["origin"]) > 20.0:
                break
            time.sleep(0.01)
        print(f"recording starts after {time.perf_counter() - t0:.0f} s", flush=True)
    rows: list[tuple] = []
    last = None
    t_end = time.perf_counter() + seconds
    while time.perf_counter() < t_end:
        g = link.read_slot(P.OFF_GUEST, P.GUEST)
        h = link.read_slot(P.OFF_HOST, P.HOST)
        if g and h:
            key = (g["frame"], h["frame"])
            if key != last:
                last = key
                rows.append((time.perf_counter(), g["frame"], tuple(g["origin"]), tuple(g["velocity"]), tuple(h["puppetPos"]), h["frame"],
                             g["viewAngles"][1], h["puppetYaw"]))
        time.sleep(0.0005)
    if out:
        with open(out, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["t", "mw_frame", "mw_x", "mw_y", "mw_z", "vx", "vy", "vz", "bl_x", "bl_y", "bl_z", "bl_frame", "mw_yaw", "bl_yaw"])
            for t, mf, o, v, b, bf, my, by in rows:
                w.writerow([f"{t:.5f}", mf, *(f"{x:.3f}" for x in o), *(f"{x:.1f}" for x in v), *(f"{x:.3f}" for x in b), bf,
                            f"{my:.3f}", f"{by:.3f}"])
    mw = [r for i, r in enumerate(rows) if i == 0 or r[1] != rows[i - 1][1]]
    bl = [r for i, r in enumerate(rows) if i == 0 or r[5] != rows[i - 1][5]]
    for name, sel, key in (("MW2 position (as published)", mw, 2), ("BL2 pawn (as placed)", bl, 4)):
        n, worst, moved = backward_steps(sel, key)
        print(f"{name}: {len(sel)} samples, {moved} moving, {n} backward steps, worst {worst:.2f} in")


if __name__ == "__main__":
    main()
