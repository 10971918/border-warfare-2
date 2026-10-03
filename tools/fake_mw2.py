"""Stand-in for the MW2 guest: publishes a scripted player through the link so the BL2 side can be
tested without MW2 running.

  python fake_mw2.py [--script circle|square|line|still] [--seconds N] [--origin X Y Z]

The fake player walks the script around --origin (MW2 units, inches) at ~190 in/s (MW2's run speed),
faces where it walks, answers teleports and prints commands sent to the guest.
Log: logs/fake_mw2.log
"""
import argparse
import math
import os
import time
import ctypes

_f = ctypes.c_int64()
ctypes.windll.kernel32.QueryPerformanceFrequency(ctypes.byref(_f))
QPC_FREQ = _f.value  # the real guest stamps samples with QueryPerformanceCounter

import bw2_proto as P

HERE = os.path.dirname(os.path.abspath(__file__))
LOGS = os.path.join(HERE, "..", "logs")


def log(msg):
    line = time.strftime("%H:%M:%S ") + msg
    print(line, flush=True)
    with open(os.path.join(LOGS, "fake_mw2.log"), "a") as f:
        f.write(line + "\n")


def path(script, t):
    """Offset (x, y) in inches and facing yaw (deg) at time t."""
    if script == "circle":  # radius 200 in, one lap per ~6.6 s
        w = 190.0 / 200.0
        a = t * w
        return (200 * math.cos(a) - 200, 200 * math.sin(a)), math.degrees(a) + 90  # starts at the origin
    if script == "square":  # 400 in sides
        side, speed = 400.0, 190.0
        d = (t * speed) % (4 * side)
        k, r = int(d // side), d % side
        pts = [((r, 0), 0), ((side, r), 90), ((side - r, side), 180), ((0, side - r), -90)]
        return pts[k]
    if script == "line":  # 600 in back and forth along +X
        d = (t * 190.0) % 1200
        return ((d, 0), 0) if d < 600 else ((1200 - d, 0), 180)
    return (0, 0), 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--script", default="circle", choices=("circle", "square", "line", "still"))
    ap.add_argument("--seconds", type=float, default=0, help="0 = until Ctrl+C")
    ap.add_argument("--origin", type=float, nargs=3, default=(0.0, 0.0, 0.0))
    ap.add_argument("--shoot", action="store_true", help="fire a bullet along the view every 0.5 s, and a frag every 4th shot")
    a = ap.parse_args()
    os.makedirs(LOGS, exist_ok=True)

    link = P.Link()
    link.set_header(guestPid=os.getpid())
    link.ring_drain(P.OFF_CMD_RING, P.CMD_RING_BYTES)  # drop anything from before we started
    origin = list(a.origin)
    ack = (link.read_slot(P.OFF_HOST, P.HOST) or {}).get("teleportSeq", 0)
    link.ring_drain(P.OFF_COLLISION_RING, P.COLLISION_RING_BYTES)
    tiles, epoch = {}, 0  # collision tiles from the host: key -> box count
    log("fake MW2 guest: script=%s origin=%s" % (a.script, origin))
    t0 = time.time()
    frame, prev, next_report = 0, None, 0
    next_shot, shot_id, nade_sent = time.time() + 1.0, 0, False
    while not a.seconds or time.time() - t0 < a.seconds:
        t = time.time() - t0
        (dx, dy), yaw = path(a.script, t)
        pos = (origin[0] + dx, origin[1] + dy, origin[2])
        vel = (0.0, 0.0, 0.0) if prev is None else tuple((p - q) * 60 for p, q in zip(pos, prev))
        prev = pos
        frame += 1
        h = link.read_slot(P.OFF_HOST, P.HOST)
        if h and h["teleportSeq"] != ack:
            ack = h["teleportSeq"]
            origin = [h["teleportPos"][0], h["teleportPos"][1], h["teleportPos"][2]]
            t0 = time.time()
            log("teleport #%d to %s" % (ack, origin))
        for rtype, payload in link.ring_drain(P.OFF_COLLISION_RING, P.COLLISION_RING_BYTES):
            if rtype == P.REC_CLEAR_ALL:
                tiles.clear()
            elif rtype == P.REC_TILE:
                th = P.TILE_HEADER.unpack(payload)
                epoch = th["epoch"]
                if th["count"] == 0 and th["bounds"][3] <= th["bounds"][0]:
                    tiles.pop((th["tx"], th["ty"]), None)
                else:
                    tiles[(th["tx"], th["ty"])] = th["count"]
        link.write_slot(P.OFF_GUEST, P.GUEST, {
            "tilesHeld": len(tiles), "boxesHeld": sum(tiles.values()), "collisionEpoch": epoch,
            "flags": P.GUEST_IN_WORLD | P.GUEST_ON_GROUND, "frame": frame, "qpc": int(time.perf_counter() * QPC_FREQ), "origin": pos, "velocity": vel,
            "viewAngles": (0.0, (yaw + 180) % 360 - 180, 0.0), "viewHeight": 60.0, "teleportAck": ack,
            "mapName": "fake_mw2"})
        link.beat("guest")
        if a.shoot and time.time() >= next_shot:
            next_shot = time.time() + 0.5
            shot_id += 1
            eye = (pos[0], pos[1], pos[2] + 60.0)
            yr = math.radians(yaw)
            end = (eye[0] + 2000 * math.cos(yr), eye[1] + 2000 * math.sin(yr), eye[2])
            link.send_shot(shot_id, 0, eye, end)
            if shot_id % 4 == 0:  # and a frag every 4th shot
                link.ring_write(P.OFF_EVENT_RING, P.EVENT_RING_BYTES, P.REC_GRENADE, b"")
        for rtype, payload in link.ring_drain(P.OFF_CMD_RING, P.CMD_RING_BYTES):
            log("command from host: %r" % payload.decode("ascii", "replace"))
        if time.time() >= next_report:
            next_report = time.time() + 1.0
            log("pos (%.0f %.0f %.0f) yaw %.0f | host alive=%s flags=%s | tiles %d boxes %d" % (
                pos[0], pos[1], pos[2], yaw % 360, link.alive("host"), h and hex(h["flags"]), len(tiles), sum(tiles.values())))
        time.sleep(1 / 60)
    log("done")


if __name__ == "__main__":
    main()
