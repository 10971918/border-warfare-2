"""M2 debugging: drive MW2 through stances with the virtual pad while BL2 hosts, and log where the
player is after each phase (sinking into the floor, stuck, can't stand...). Pairs with the guest's
`bounds` command (trace boxes per stance) - written to logs/mw_cmd.txt at the end.
  python stance_test.py
"""
import math
import os
import time

import bw2_proto as P

HERE = os.path.dirname(os.path.abspath(__file__))
B = P.XI
link = P.Link()
packet = [0]


def pad(**kw):
    packet[0] += 1
    v = {"flags": P.PAD_ACTIVE, "packet": packet[0], "buttons": 0, "thumbLX": 0, "thumbLY": 0, "thumbRX": 0, "thumbRY": 0,
         "leftTrigger": 0, "rightTrigger": 0}
    v.update(kw)
    link.write_slot(P.OFF_PAD, P.PAD, v)


def phase(name, seconds, **kw):
    g0 = link.read_slot(P.OFF_GUEST, P.GUEST)
    pad(**kw)
    end = time.time() + seconds
    zmax = g0["origin"][2]
    while time.time() < end:
        time.sleep(1 / 60)
        zmax = max(zmax, link.read_slot(P.OFF_GUEST, P.GUEST)["origin"][2])
    g1 = link.read_slot(P.OFF_GUEST, P.GUEST)
    moved = math.dist(g0["origin"][:2], g1["origin"][:2])
    st = "prone" if g1["flags"] & P.GUEST_PRONE else "crouch" if g1["flags"] & P.GUEST_CROUCHED else "stand"
    print("%-16s moved %6.1f in  z %.2f -> %.2f (max %.2f)  stance %-6s hold %d" % (
        name, moved, g0["origin"][2], g1["origin"][2], zmax, st, g1["holdTraces"]))


with open(os.path.join(HERE, "..", "logs", "mw_cmd.txt"), "a") as f:
    f.write("bounds\n")  # clears the recorder
time.sleep(0.5)
phase("idle", 1.0)
phase("walk", 1.0, thumbLY=32767)
phase("stop", 0.5)
phase("crouch tap", 0.15, buttons=B["B"])
phase("crouched idle", 1.5)
phase("walk crouched", 1.0, thumbLY=32767)
phase("stop", 0.5)
phase("stand tap", 0.15, buttons=B["B"])
phase("standing idle", 1.5)
phase("walk", 1.0, thumbLY=-32767)
phase("jump", 0.15, buttons=B["A"])
phase("airborne", 1.0)
link.write_slot(P.OFF_PAD, P.PAD, {"flags": 0, "packet": packet[0] + 1})
with open(os.path.join(HERE, "..", "logs", "mw_cmd.txt"), "a") as f:
    f.write("bounds\n")
