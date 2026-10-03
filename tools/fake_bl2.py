"""Stand-in for the BL2 host: keeps the link alive and watches what the MW2 guest publishes, and can
play scripted controller input into MW2 through the virtual pad (PadState).

  python fake_bl2.py monitor [--seconds N]     heartbeat only; log the MW2 player once a second
  python fake_bl2.py drive  [--seconds N]      virtual pad: walk forward, strafe, turn, jump, crouch,
                                               fire, ADS, reload - and check MW2 actually moved/turned
  python fake_bl2.py drive --pad-only          same, but the real BL2 is the host (only the pad is written)
  python fake_bl2.py send "<command>"          one command into the guest's command ring
Records: logs/fake_bl2.log, logs/fake_bl2_trace.csv
"""
import argparse
import math
import os
import time

import bw2_proto as P

HERE = os.path.dirname(os.path.abspath(__file__))
LOGS = os.path.join(HERE, "..", "logs")
B = P.XI


def log(msg):
    line = time.strftime("%H:%M:%S ") + msg
    print(line, flush=True)
    with open(os.path.join(LOGS, "fake_bl2.log"), "a") as f:
        f.write(line + "\n")


class Host:
    def __init__(self, pad_only=False):
        self.link = P.Link()
        self.pad_only = pad_only
        if not pad_only:
            self.link.set_header(hostPid=os.getpid())
        self.frame, self.packet = 0, 0
        self.trace = open(os.path.join(LOGS, "fake_bl2_trace.csv"), "w")
        self.trace.write("t,phase,flags,x,y,z,vx,vy,vz,pitch,yaw,buttons,fwd,right\n")
        self.t0 = time.time()
        self.phase = ""

    def pad(self, lx=0, ly=0, rx=0, ry=0, lt=0, rt=0, buttons=0, active=True):
        self.packet += 1
        self.link.write_slot(P.OFF_PAD, P.PAD, {
            "flags": P.PAD_ACTIVE if active else 0, "packet": self.packet, "buttons": buttons, "leftTrigger": lt,
            "rightTrigger": rt, "thumbLX": lx, "thumbLY": ly, "thumbRX": rx, "thumbRY": ry})

    def tick(self):
        self.frame += 1
        if not self.pad_only:
            self.link.write_slot(P.OFF_HOST, P.HOST, {"flags": P.HOST_IN_GAME, "frame": self.frame, "deltaTime": 1 / 60})
            self.link.beat("host")
        g = self.link.read_slot(P.OFF_GUEST, P.GUEST)
        if g and self.link.alive("guest"):
            o, v, a = g["origin"], g["velocity"], g["viewAngles"]
            self.trace.write("%.3f,%s,%d,%.2f,%.2f,%.2f,%.1f,%.1f,%.1f,%.2f,%.2f,%d,%d,%d\n" % (
                time.time() - self.t0, self.phase, g["flags"], *o, *v, a[0], a[1], g["cmdButtons"], g["forwardMove"], g["rightMove"]))
            return g
        return None

    def hold(self, seconds, phase, **pad):
        self.phase = phase
        self.pad(**pad)
        first = last = None
        end = time.time() + seconds
        while time.time() < end:
            g = self.tick()
            if g:
                first = first or g
                last = g
            time.sleep(1 / 60)
        return first, last

    def wait_guest(self, timeout=300):
        log("waiting for the MW2 guest (load a mission in MW2 single player)...")
        end = time.time() + timeout
        while time.time() < end:
            g = self.tick()
            if g and g["flags"] & P.GUEST_IN_WORLD:
                log("guest in world: map %s, origin (%.0f %.0f %.0f)" % (g["mapName"], *g["origin"]))
                return True
            time.sleep(1 / 60)
        log("no guest")
        return False


def dist(a, b):
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


def angdiff(a, b):
    return (a - b + 180) % 360 - 180


def drive(h):
    """Scripted input; each step checks MW2's reaction and logs PASS/FAIL."""
    results = []

    def check(name, ok, detail):
        results.append((name, ok))
        log("%-22s %s  %s" % (name, "PASS" if ok else "FAIL", detail))

    h.hold(1.0, "idle")
    f, l = h.hold(2.0, "forward", ly=32767)
    d = dist(f["origin"][:2], l["origin"][:2])
    yaw = math.radians(f["viewAngles"][1])
    fwd = ((l["origin"][0] - f["origin"][0]) * math.cos(yaw) + (l["origin"][1] - f["origin"][1]) * math.sin(yaw))
    check("left stick forward", d > 100 and fwd > 0.8 * d, "moved %.0f in, %.0f along view (cmd fwd %d)" % (d, fwd, l["forwardMove"]))
    h.hold(0.5, "stop")
    f, l = h.hold(2.0, "strafe right", lx=32767)
    yaw = math.radians(f["viewAngles"][1])
    right = ((l["origin"][0] - f["origin"][0]) * math.sin(yaw) - (l["origin"][1] - f["origin"][1]) * math.cos(yaw))
    check("left stick right", right > 80, "moved %.0f in to the right (cmd right %d)" % (right, l["rightMove"]))
    f, l = h.hold(0.6, "turn right", rx=32767)  # ~130 deg at full deflection (no wrap past 180)
    turned = angdiff(l["viewAngles"][1], f["viewAngles"][1])
    check("right stick turn", -170 < turned < -60, "yaw changed %.0f deg (right turn = negative)" % turned)
    f, l = h.hold(1.0, "look up", ry=32767)
    check("right stick look up", l["viewAngles"][0] < f["viewAngles"][0] - 20, "pitch %.0f -> %.0f (up = negative)" % (f["viewAngles"][0], l["viewAngles"][0]))
    h.hold(1.0, "look down", ry=-32767)
    h.hold(0.3, "release")
    f, l = h.hold(0.15, "jump", buttons=B["A"])
    _, l2 = h.hold(0.6, "jump release")
    zmax = max(l["origin"][2], l2["origin"][2])
    check("A jump", zmax > f["origin"][2] + 10 or abs(l2["velocity"][2]) > 50, "z %.0f -> %.0f (vz %.0f)" % (f["origin"][2], l2["origin"][2], l2["velocity"][2]))
    h.hold(1.0, "settle")
    h.hold(0.15, "crouch tap", buttons=B["B"])
    _, l = h.hold(0.8, "crouched")
    check("B tap crouch", bool(l["flags"] & P.GUEST_CROUCHED), "flags %#x" % l["flags"])
    h.hold(0.15, "stand", buttons=B["B"])
    _, l = h.hold(0.8, "standing")
    check("B tap again stands", not l["flags"] & (P.GUEST_CROUCHED | P.GUEST_PRONE), "flags %#x" % l["flags"])
    _, l = h.hold(0.6, "ADS", lt=255)
    check("LT aims", bool(l["cmdButtons"] & 0x800), "usercmd buttons %#x (ADS = 0x800)" % l["cmdButtons"])
    _, l = h.hold(0.4, "ADS+fire", lt=255, rt=255)
    check("RT fires", bool(l["cmdButtons"] & 0x1), "usercmd buttons %#x (attack = 0x1)" % l["cmdButtons"])
    h.hold(0.5, "release all")
    h.hold(0.15, "reload", buttons=B["X"])
    h.hold(1.0, "release")
    h.pad(active=False)
    h.tick()
    log("drive: %d/%d checks passed" % (sum(ok for _, ok in results), len(results)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=("monitor", "drive", "send"))
    ap.add_argument("arg", nargs="?")
    ap.add_argument("--seconds", type=float, default=0)
    ap.add_argument("--pad-only", action="store_true", help="the real BL2 is the host: write only the virtual pad")
    a = ap.parse_args()
    os.makedirs(LOGS, exist_ok=True)
    h = Host(pad_only=a.pad_only)
    if a.mode == "send":
        log("sent %r: %s" % (a.arg, h.link.send_command(a.arg or "status")))
        return
    if a.mode == "drive":
        if h.wait_guest():
            drive(h)
        return
    end = time.time() + a.seconds if a.seconds else float("inf")
    nxt = 0
    while time.time() < end:
        g = h.tick()
        if time.time() >= nxt:
            nxt = time.time() + 1
            if g:
                log("guest: map=%s flags=%#x origin=(%.0f %.0f %.0f) vel=(%.0f %.0f %.0f) view=(%.1f %.1f) buttons=%#x move=%d/%d traces=%d" % (
                    g["mapName"], g["flags"], *g["origin"], *g["velocity"], g["viewAngles"][0], g["viewAngles"][1],
                    g["cmdButtons"], g["forwardMove"], g["rightMove"], g["pmoveTraces"]))
            else:
                log("no guest (alive=%s)" % h.link.alive("guest"))
        time.sleep(1 / 60)


if __name__ == "__main__":
    main()
