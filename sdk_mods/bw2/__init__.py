"""Border Warfare 2 - BL2 host (M2: MW2 movement on BL2 collision).

`drive on`:
  1. scanning  - anchors MW2's player (moved DZ inches below MW2's own map, away from its AI, scripts and
                 triggers) to BL2's pawn, and scans BL2's collision around the pawn into tiles for MW2
  2. arriving  - once the tiles around you are in MW2: collision mode on (MW2's movement now collides with
                 BL2's world only) and MW2's player is teleported onto BL2's ground
  3. driving   - BL2's pawn and camera follow MW2's player and eye exactly; tiles keep streaming around you
`drive off` (or the guest/level going away) puts MW2's player back where it was and releases BL2's pawn.

Commands: <LogDir>\\bl_cmd.txt, or `bw2 <command>` in the console.
  status | drive on [dz] | drive off | home | lead <ms> | god on|off | budget <ms> | scanner native|python | mw <guest command> | bench <n> | con <cmd> | py <stmt>
Log: <LogDir>\\bl_host.log
"""

from __future__ import annotations

import argparse
import math
import os
import time
import traceback
from typing import Any

import unrealsdk
from mods_base import ENGINE, build_mod, command, get_pc, hook, keybind
from unrealsdk import hooks, logging

from . import bw2_coords as C
from . import bw2_proto as P
from . import gunplay as G
from . import nativescan
from . import natives as N
from . import overlay as V
from . import scan

HERE = os.path.dirname(os.path.abspath(__file__))
PHYS_NONE, PHYS_FALLING = 0, 2
DEFAULT_DZ = -12000.0     # MW2 inches: where MW2's player lives while driving (far below MW2's level)
MAX_STEP_UU = 400.0       # a single puppet move longer than this is refused (bad data)
LOST_UU = 3000.0          # MW2's player this far from the pawn: it was reset (death/checkpoint) - stop driving
SCAN_RADIUS = 1           # tiles around the pawn kept scanned (3x3)
DROP_RADIUS = 3           # tiles further than this are dropped
RESCAN_S = 30.0           # tiles near the pawn are rescanned this often (doors, movers)
ARRIVE_TIMEOUT_S = 30.0


def _log_dir() -> str:
    try:
        with open(os.path.join(HERE, "bw2.ini"), encoding="utf-8") as f:
            for line in f:
                key, _, value = line.partition("=")
                if key.strip().lower() == "logdir" and value.strip():
                    return value.strip()
    except OSError:
        pass
    return r"A:\Border Warfare 2\logs"


LOG_DIR = _log_dir()
_log_file: Any = None


def log(msg: str) -> None:
    global _log_file
    if _log_file is None:
        os.makedirs(LOG_DIR, exist_ok=True)
        _log_file = open(os.path.join(LOG_DIR, "bl_host.log"), "w", encoding="utf-8")  # noqa: SIM115
    _log_file.write(time.strftime("%H:%M:%S ") + msg + "\n")
    _log_file.flush()


class Tile:
    def __init__(self, key: tuple[int, int]) -> None:
        self.key = key
        self.job: scan.TileScan | None = None
        self.steps: Any = None
        self.sent_at = 0.0
        self.zref = 0.0
        self.boxes = 0
        self.counted = 0


class State:
    link: P.Link | None = None
    link_retry = 0.0
    frame = 0
    last_tick = 0.0
    next_cmd = 0.0
    next_report = 0.0
    mode = "off"                      # off | scanning | arriving | driving
    mode_since = 0.0
    anchor: C.Anchor | None = None
    mw_home: tuple | None = None      # MW2 player's position before driving (restored after)
    mw_map = ""                       # ... in this MW2 level (no going home across a level change)
    bl_home: tuple | None = None      # pawn Location before driving
    last_safe: tuple | None = None    # last pawn Location BL2 accepted
    epoch = 0
    teleport_seq = 0
    teleport_pos = (0.0, 0.0, 0.0)
    tiles: dict = {}
    budget_ms = 3.0
    native: Any = None                # bw2_scan.dll (None: Python scanner)
    trace_params: Any = None
    traces = 0
    applied = refused = forced = 0
    guest_lost_at = 0.0
    view: tuple | None = None
    view_height = 60.0
    block_hooks: list[str] = []
    view_hooks: list[str] = []
    pawn_addr = 0                     # the pawn we drive (a respawned pawn ends driving)
    hook_frame = -1                   # S.frame when UpdateRotation last positioned the pawn (tick is the fallback)
    lead_s = 0.0                      # `bw2 lead <ms>` (0: a 20 ms lead + view prediction felt worse, 2026-10-03): how far ahead the world is predicted (beyond the sample's age)
    dr_o: tuple | None = None         # dead reckoning (see puppet): MW2 position, when it changed (perf_counter),
    dr_t = 0.0                        # the correction being blended out, and the last position shown
    dr_off = (0.0, 0.0, 0.0)
    dr_off_t = 0.0
    dr_shown: tuple | None = None
    use_since = 0.0                   # MW2's X (use/reload) held since
    use_fired = False
    resume_at = 0.0                   # when we left MW2 control for a vehicle (0: not waiting to resume)
    resume_try = 0.0
    refused_since = 0.0               # when MW2's position started being refused (see puppet)
    third = False                     # F8: third-person camera while driving
    cam_applied = False               # BL2 camera / MW2 gun currently set for third person
    god = False                       # test god mode (`bw2 god on`): your BL2 character takes no damage while driving


S = State()


# ---------------------------------------------------------------------------------------- helpers
def pawn_and_pc() -> tuple[Any, Any]:
    pc = get_pc()
    return (pc.Pawn if pc is not None else None), pc


def loc(actor: Any) -> tuple[float, float, float]:
    v = actor.Location
    return (v.X, v.Y, v.Z)


def feet(pawn: Any) -> tuple[float, float, float]:
    x, y, z = loc(pawn)
    return (x, y, z - pawn.CylinderComponent.CollisionHeight)


def guest() -> dict | None:
    if S.link is None or not S.link.alive("guest"):
        return None
    return S.link.read_slot(P.OFF_GUEST, P.GUEST)


def set_mode(mode: str) -> None:
    S.mode, S.mode_since = mode, time.monotonic()
    log(f"mode -> {mode}")


def open_link() -> None:
    if S.link is not None or time.monotonic() < S.link_retry:
        return
    try:
        S.link = P.Link()
        S.link.set_header(hostPid=os.getpid())
        log("link open")
    except Exception as ex:  # noqa: BLE001
        S.link_retry = time.monotonic() + 5.0
        log(f"link failed: {ex}")


def host_flags(pawn: Any) -> int:
    flags = P.HOST_IN_GAME if pawn is not None else P.HOST_LOADING
    try:
        if ENGINE.GetCurrentWorldInfo().Pauser is not None:
            flags |= P.HOST_MENU_OPEN
    except Exception:  # noqa: BLE001
        pass
    if S.mode == "driving":
        flags |= P.HOST_DRIVE_PUPPET
    if S.mode in ("arriving", "driving"):
        flags |= P.HOST_COLLISION
    return flags


def teleport_guest(pos: tuple) -> None:
    S.teleport_seq += 1
    S.teleport_pos = tuple(float(v) for v in pos)
    log(f"teleport MW2 player #{S.teleport_seq} to ({pos[0]:.0f} {pos[1]:.0f} {pos[2]:.0f})")


# ---------------------------------------------------------------------------------------- streaming
def stream(pawn: Any, now: float) -> None:
    """Spend up to budget_ms scanning the tiles around the pawn; send finished tiles to the guest."""
    fx, fy, fz = feet(pawn)
    ctx, cty = scan.tile_of(fx, fy)
    want = [(ctx + dx, cty + dy) for dx in range(-SCAN_RADIUS, SCAN_RADIUS + 1) for dy in range(-SCAN_RADIUS, SCAN_RADIUS + 1)]
    want.sort(key=lambda k: (k[0] - ctx) ** 2 + (k[1] - cty) ** 2)
    for key in list(S.tiles):  # drop far tiles
        if max(abs(key[0] - ctx), abs(key[1] - cty)) > DROP_RADIUS:
            del S.tiles[key]
            S.link.send_tile(key[0], key[1], S.epoch, (0, 0, 0, 0, 0, 0), [])
    deadline = time.perf_counter() + S.budget_ms / 1000.0
    world = ENGINE.GetCurrentWorldInfo()  # the trace actor: lives as long as the level (a pawn can respawn mid-scan)
    if S.native is not None and S.trace_params is None:
        S.trace_params = nativescan.TraceParams(world.Trace.func)
    with N.Tracer(world) as tracer:
        for key in want:
            t = S.tiles.get(key)
            if t is None:
                t = S.tiles[key] = Tile(key)
            stale = t.sent_at and (now - t.sent_at > RESCAN_S or abs(fz - t.zref) > scan.Z_HALF * 0.6)
            if t.job is None and (not t.sent_at or stale):
                t.zref, t.counted = fz, 0
                if S.native is not None:
                    t.job = nativescan.NativeTileScan(S.native, key[0], key[1], fz, actor=world, params=S.trace_params)
                else:
                    t.job = scan.TileScan(key[0], key[1], fz, tracer)
                t.steps = t.job.run()
            if t.job is None:
                continue
            if isinstance(t.job, scan.TileScan):
                t.job.trace = tracer  # the tracer object is per tick (iNative is cleared only inside it)
            for _ in t.steps:
                if time.perf_counter() > deadline:
                    break
            S.traces += t.job.traces - t.counted
            t.counted = t.job.traces
            if t.job.done:
                send_tile(t)
            if time.perf_counter() > deadline:
                return


def send_tile(t: Tile) -> None:
    job = t.job
    gb = scan.to_guest(S.anchor, job.bounds())
    flat: list[float] = []
    for b in job.boxes:
        flat += scan.to_guest(S.anchor, b)
    if not S.link.send_tile(job.tx, job.ty, S.epoch, gb, flat):
        return  # ring full: retry next tick
    t.sent_at, t.boxes, t.job, t.steps, t.counted = time.monotonic(), len(job.boxes), None, None, 0


def tiles_ready(pawn: Any) -> int:
    fx, fy, _ = feet(pawn)
    ctx, cty = scan.tile_of(fx, fy)
    n = 0
    for dx in (-1, 0, 1):
        for dy in (-1, 0, 1):
            t = S.tiles.get((ctx + dx, cty + dy))
            n += bool(t and t.sent_at)
    return n


# ---------------------------------------------------------------------------------------- drive
def start_drive(dz: float) -> str:
    pawn, _ = pawn_and_pc()
    g = guest()
    if pawn is None:
        return "no pawn"
    if not g or not (g["flags"] & P.GUEST_IN_WORLD):
        return "MW2 isn't in a mission yet (start MW2 and wait for Contingency to load)"
    if S.mode != "off":
        return f"already {S.mode}"
    if g["teleportAck"] != S.teleport_seq:  # still on its way home from the last drive: its position isn't home yet
        return "MW2's player is still moving back - press F7 again in a second"
    if g["origin"][2] < VOID_Z:  # MW2's player is lost under its level: anchoring there makes moving impossible
        S.link.send_command(f"level {g['mapName'].split('/')[-1].split('.')[0]}")
        return "MW2's player was outside its level - reloading the MW2 mission, press F7 again in about 30 seconds"
    if not (g["flags"] & P.GUEST_ON_GROUND):
        return "MW2's player is in the air - wait a second and press F7 again"
    S.mw_home = tuple(g["origin"])
    S.mw_map = g["mapName"]
    S.pawn_addr = pawn._get_address()
    S.bl_home = S.last_safe = loc(pawn)
    S.anchor = C.Anchor(mw=(g["origin"][0], g["origin"][1], g["origin"][2] + dz), bl=feet(pawn))
    S.epoch += 1
    S.tiles = {}
    S.link.send_clear_all()
    S.applied = S.refused = S.forced = S.traces = 0
    S.refused_since = 0.0
    S.dr_o = S.dr_shown = None
    S.resume_at = 0.0
    S.view = None
    set_mode("scanning")
    return f"scanning BL2 around the pawn; MW2 player will live at {tuple(round(v) for v in S.anchor.mw)}"


def stop_drive(reason: str) -> None:
    if S.mode == "off":
        return
    was = S.mode
    set_mode("off")
    pawn, pc = pawn_and_pc()
    if pawn is not None and pawn._get_address() != S.pawn_addr:
        pawn = None  # a vehicle (or a new pawn): never move or re-physics it
    try:
        if S.mw_home is not None and S.link is not None:
            g = guest()
            if g and g["mapName"] == S.mw_map:
                teleport_guest(S.mw_home)  # MW2's player back to its own level (collision flag is off now)
            S.link.send_clear_all()
        if pawn is not None and was in ("arriving", "driving"):
            if S.last_safe is not None:
                N.call(pawn.SetLocation, N.vec(*S.last_safe))
            N.call(pawn.SetPhysics, PHYS_FALLING)
        if pc is not None and was in ("arriving", "driving"):
            N.call(pc.IgnoreMoveInput, False)
        if pawn is not None and was in ("arriving", "driving"):
            pawn.bIsCrouched = False
        apply_camera(pc)  # mode is off now: back to first person
    except Exception:  # noqa: BLE001
        log(traceback.format_exc())
    S.tiles = {}  # drops unfinished native scans with the level they belong to
    if reason not in ("F7", "command", "home", "mod disabled"):
        show(f"MW2 control off: {reason}.")
    log(f"drive off ({reason}) after {was}; moves applied {S.applied}, forced {S.forced}, refused {S.refused}; traces {S.traces}")


def update_drive(pawn: Any, pc: Any, now: float) -> None:
    g = guest()
    if g is None or not (g["flags"] & P.GUEST_IN_WORLD):
        if not S.guest_lost_at:
            S.guest_lost_at = now
        elif now - S.guest_lost_at > 2.0:
            stop_drive("MW2 closed or reloading")
        return
    S.guest_lost_at = 0.0
    if pawn._get_address() != S.pawn_addr:
        if "Vehicle" in pawn.Class.Name:  # you got into a vehicle: BL2 drives it; MW2 control comes back when you get out
            S.resume_at = time.monotonic()
            stop_drive("in a vehicle - MW2 control comes back when you get out")
        else:
            stop_drive("BL2 pawn changed (death or respawn)")
        return
    if S.mode == "driving" and _injured(pawn):  # Fight For Your Life: BL2's own controls take over
        stop_drive("down (Fight For Your Life) - press F7 again once you're back up")
        return
    stream(pawn, now)
    if S.mode == "scanning":
        if tiles_ready(pawn) == 9 and g["collisionEpoch"] == S.epoch and g["tilesHeld"] >= 9:
            N.call(pc.IgnoreMoveInput, True)
            N.call(pawn.SetPhysics, PHYS_NONE)
            # the pawn kept moving while BL2 was scanned (you may have been running when you pressed F7):
            # put it back on the anchor, where MW2's player is about to arrive
            bx, by, bz = S.anchor.bl
            N.call(pawn.SetLocation, N.vec(bx, by, bz + pawn.CylinderComponent.CollisionHeight + 1.0 * C.SCALE))
            pawn.Velocity = N.vec(0, 0, 0)
            teleport_guest((S.anchor.mw[0], S.anchor.mw[1], S.anchor.mw[2] + 1.0))
            set_mode("arriving")
        elif now - S.mode_since > ARRIVE_TIMEOUT_S:
            stop_drive(f"scan too slow ({tiles_ready(pawn)}/9 tiles, guest holds {g['tilesHeld']})")
    elif S.mode == "arriving":
        if g["teleportAck"] == S.teleport_seq and math.dist(g["origin"], S.anchor.mw) < 40.0:
            set_mode("driving")
            apply_camera(pc)
        elif now - S.mode_since > ARRIVE_TIMEOUT_S:
            stop_drive("MW2 player didn't arrive")
    elif S.mode == "driving":
        # normally done in UpdateRotation (before BL2 builds the camera); this is the fallback if that hook
        # didn't run this frame (e.g. a cutscene)
        if S.hook_frame != S.frame - 1:  # S.frame was bumped by this viewport tick; the hook ran before it
            puppet(pawn, pc, g)


MAX_PREDICT_S = 0.1         # never predict MW2's position further ahead than this
# (tried 2026-10-03: +20 ms lead and view prediction from MW2's turn rate made looking/moving feel laggier)
BLEND_S = 0.08              # a new MW2 position's correction is spread over this long (tested on a recorded run)
BLEND_MAX = 50.0            # MW2 inches: a bigger correction (teleport, unstick) is applied at once
MW2_USE_BIT = 0x20          # usercmd use/reload (the guest ORs it in while X is held)
USE_HOLD_S = 0.35           # MW2's X held this long = use (a tap is a reload)
RESUME_FOR_S = 20.0         # after getting out of a vehicle, MW2 control is re-enabled within this time
REFUSE_FOR_S = 1.0          # a big jump MW2's player keeps for this long is real, not a glitch
MAX_PREDICT_SPEED = 600.0   # MW2 inches/s (sprint ~ 300): faster than this is not walking - no prediction

VOID_Z = -30000.0  # MW2 inches: below this, MW2's player has fallen out of its level (Contingency's ground is ~100)


def show(msg: str, seconds: float = 4.0) -> None:
    """An on-screen BL2 message box (the same one BL2's tutorial hints use)."""
    try:
        pc = get_pc()
        movie = pc.myHUD.HUDMovie
        movie.ClearTrainingText()
        movie.AddTrainingText(msg, "Border Warfare 2", seconds, unrealsdk.make_struct("Color", R=255, G=255, B=255, A=255),
                              "", False, 0, pc.PlayerReplicationInfo, True)
    except Exception:  # noqa: BLE001  (no HUD during loading)
        pass


def _injured(pawn: Any) -> bool:
    try:
        return bool(pawn.IsInjured()) or float(pawn.GetHealth()) <= 0.0
    except Exception:  # noqa: BLE001
        return False


def apply_camera(pc: Any) -> None:
    """Third person (F8) while driving: BL2's behind-the-back camera shows your character, and MW2 stops drawing
    its gun and arms (its crosshair and ammo counter stay). Everything back to first person otherwise."""
    want = S.third and S.mode == "driving"
    if want == S.cam_applied or pc is None:
        return
    S.cam_applied = want
    try:
        pc.ConsoleCommand("camera 3rd" if want else "camera 1st")
    except Exception:  # noqa: BLE001
        pass
    if S.link is not None:
        S.link.send_command(f"con cg_drawGun {0 if want else 1}")
    log(f"camera: {'third' if want else 'first'} person")


def toggle_third() -> None:
    S.third = not S.third
    pc = get_pc()
    apply_camera(pc)
    if S.mode != "driving":
        show(f"Third person {'on' if S.third else 'off'} - it applies while MW2 control (F7) is on.")


@keybind("Toggle third person", "F8", description="Border Warfare 2: third-person camera while MW2 control is on.")
def _third_key() -> None:
    toggle_third()


def toggle_drive() -> None:
    """F7 (rebindable in the mod's keybinds): MW2 control on / off."""
    if S.mode == "off":
        r = start_drive(DEFAULT_DZ)
        log(f"F7: {r}")
        if S.mode == "off":  # didn't start: say why
            show(r[0].upper() + r[1:] + ".")
    else:
        stop_drive("F7")


@keybind("Toggle MW2 control", "F7", description="Border Warfare 2: play with MW2's movement and guns (press again to stop).")
def _toggle_key() -> None:
    toggle_drive()


def puppet(pawn: Any, pc: Any, g: dict) -> None:
    """BL2's pawn goes exactly where MW2's player is (MW2 already collides with BL2's world)."""
    S.view = g["viewAngles"]
    S.view_height = g["viewHeight"]
    height = pawn.CylinderComponent.CollisionHeight
    # MW2 publishes every frame (~140/s) but its player's position only changes every ~30-40 ms (in 7-13 inch
    # steps). Predicting from the publish time made the pawn run ahead, snap back on every re-publish of the same
    # position, then lurch forward ("micro lag back, then rubber band"; measured 2147 backward steps in 90 s,
    # tools/jitter_probe.py). Dead reckoning instead: from the moment the position last changed, along MW2's
    # velocity; the small error at each new position is blended out over BLEND_S instead of snapping.
    vel = g["velocity"]
    if math.hypot(*vel) > MAX_PREDICT_SPEED:  # falling, knockback, a teleport: don't guess
        vel = (0.0, 0.0, 0.0)
    now = time.perf_counter()
    o = tuple(g["origin"])

    def reckon(t: float) -> tuple:
        dt = min(max(t - S.dr_t, 0.0) + S.lead_s, MAX_PREDICT_S)
        return tuple(a + v * dt for a, v in zip(S.dr_o, vel))

    if S.dr_o is None or o != S.dr_o:
        S.dr_o, S.dr_t = o, now
        fresh = reckon(now)
        prev = S.dr_shown
        S.dr_off = tuple(a - b for a, b in zip(prev, fresh)) if prev is not None and math.dist(prev, fresh) < BLEND_MAX else (0.0, 0.0, 0.0)
        S.dr_off_t = now
    k = max(0.0, 1.0 - (now - S.dr_off_t) / BLEND_S)
    origin = tuple(p + off * k for p, off in zip(reckon(now), S.dr_off))
    S.dr_shown = origin
    fx, fy, fz = S.anchor.to_bl(origin)
    target = (fx, fy, fz + height)
    if pawn.Physics != PHYS_NONE:  # BL2 resets it (knockback, damage): its physics would move the pawn on its own
        N.call(pawn.SetPhysics, PHYS_NONE)
    jump = math.dist(loc(pawn), target)
    if jump > LOST_UU:
        stop_drive(f"MW2's player jumped {jump:.0f} uu away (MW2 checkpoint reload?)")
        return
    now = time.monotonic()
    if jump > MAX_STEP_UU:
        # One odd sample (an unstick nudge) is refused; MW2 staying there for a second is where it really is.
        if not S.refused_since:
            S.refused_since = now
        if now - S.refused_since < REFUSE_FOR_S:
            S.refused += 1
            if S.refused % 60 == 1:
                log(f"refused a {jump:.0f} uu jump to {tuple(round(v) for v in target)}")
            pawn.Velocity = N.vec(0, 0, 0)
            return
        log(f"accepted a {jump:.0f} uu jump after {REFUSE_FOR_S:.0f} s (MW2's player stayed there)")
    S.refused_since = 0.0
    if N.call(pawn.SetLocation, N.vec(*target)):
        S.applied += 1
        S.last_safe = target
    else:
        # BL2's own collision objects (scanner error next to a wall): follow anyway, but don't mark it safe
        pawn.Location = N.vec(*target)
        S.forced += 1
    # BL2's animations (third person) follow the pawn's velocity: MW2's, converted (PHYS_NONE never integrates it)
    if math.hypot(*vel) <= MAX_PREDICT_SPEED:
        a = S.anchor.to_bl(g["origin"])
        b = S.anchor.to_bl(tuple(o + v for o, v in zip(g["origin"], vel)))
        pawn.Velocity = N.vec(b[0] - a[0], b[1] - a[1], b[2] - a[2])
    else:
        pawn.Velocity = N.vec(0, 0, 0)
    eye = S.view_height * C.SCALE - height
    pawn.EyeHeight = eye
    pawn.BaseEyeHeight = eye
    # third-person pose: BL2 has no prone, so MW2's crouch and prone both show as BL2's crouch
    # (setting the flag alone switches the animation; the collision cylinder stays as it is)
    crouched = bool(g["flags"] & (P.GUEST_CROUCHED | P.GUEST_PRONE))
    if bool(pawn.bIsCrouched) != crouched:
        pawn.bIsCrouched = crouched


def apply_view(pc: Any) -> None:
    if S.view is not None:
        pitch, yaw, _ = S.view
        pc.Rotation = unrealsdk.make_struct("Rotator", Pitch=C.pitch_to_bl(pitch), Yaw=C.yaw_to_bl(yaw), Roll=0)


# ---------------------------------------------------------------------------------------- tick
def tick() -> None:
    open_link()
    if S.link is None:
        return
    now = time.monotonic()
    dt = now - S.last_tick if S.last_tick else 0.0
    S.last_tick = now
    S.frame += 1
    pawn, pc = pawn_and_pc()
    if pawn is None and S.mode != "off":
        stop_drive("pawn gone (level change?)")
    puppet_pos, puppet_yaw = (0.0, 0.0, 0.0), 0.0
    if pawn is not None and S.anchor is not None:
        puppet_pos = S.anchor.to_mw(feet(pawn))
        puppet_yaw = C.yaw_to_mw(pc.Rotation.Yaw)
    S.link.write_slot(P.OFF_HOST, P.HOST, {
        "flags": host_flags(pawn), "frame": S.frame, "deltaTime": dt, "teleportSeq": S.teleport_seq,
        "teleportPos": S.teleport_pos, "puppetPos": puppet_pos, "puppetYaw": puppet_yaw, "collisionEpoch": S.epoch})
    S.link.beat("host")
    if S.mode != "off" and pawn is not None:
        update_drive(pawn, pc, now)
    if S.mode == "driving" and pawn is not None:
        use_button(pc)
    elif S.mode == "off" and S.resume_at and pawn is not None:
        resume_after_vehicle(pawn, now)
    G.handle_events(S.link, S.mode == "driving" and pawn is not None, pawn, pc, S.anchor, log)
    V.update(S.mode == "driving" and pawn is not None, pawn, log)


def use_button(pc: Any) -> None:
    """MW2's X: tap = reload, hold = use. A hold of USE_HOLD_S also presses BL2's Use (vehicle stations, doors,
    vending machines, chests): BL2 doesn't see MW2's use, and its own input is ignored while driving."""
    g = guest()
    held = bool(g and g["cmdButtons"] & MW2_USE_BIT)
    now = time.monotonic()
    if not held:
        S.use_since, S.use_fired = 0.0, False
        return
    if not S.use_since:
        S.use_since = now
    if not S.use_fired and now - S.use_since >= USE_HOLD_S:
        S.use_fired = True
        try:
            pc.Use()
            log("use (MW2 X held)")
        except Exception as ex:  # noqa: BLE001
            log(f"use failed: {ex}")


def resume_after_vehicle(pawn: Any, now: float) -> None:
    """Out of the vehicle again: MW2 control back on (tried once a second for RESUME_FOR_S)."""
    if "Vehicle" in pawn.Class.Name or now < S.resume_try:
        return
    if now - S.resume_at > RESUME_FOR_S:
        S.resume_at = 0.0
        show("Press F7 for MW2 control.")
        return
    S.resume_try = now + 1.0
    r = start_drive(DEFAULT_DZ)
    if S.mode != "off":
        S.resume_at = 0.0
        log(f"back from the vehicle: {r}")


def report() -> str:
    pawn, _ = pawn_and_pc()
    g = guest()
    parts = [f"mode={S.mode}", f"link={'ok' if S.link else 'none'}", f"guest={'alive' if g else 'none'}",
             f"tiles sent={sum(1 for t in S.tiles.values() if t.sent_at)}/{len(S.tiles)}", f"traces={S.traces}",
             f"moves={S.applied}/forced {S.forced}/refused {S.refused}", G.report(), V.report()]
    if g:
        parts.append(f"mw2 {g['mapName']} flags={g['flags']:#x} origin=({g['origin'][0]:.0f} {g['origin'][1]:.0f} {g['origin'][2]:.0f}) "
                     f"tiles={g['tilesHeld']} boxes={g['boxesHeld']} field={g['fieldTraces']} hold={g['holdTraces']}")
    if pawn is not None:
        x, y, z = loc(pawn)
        parts.append(f"bl2 pawn=({x:.0f} {y:.0f} {z:.0f}) physics={pawn.Physics}")
    return " ".join(parts)


# ---------------------------------------------------------------------------------------- commands
def run_command(line: str) -> None:
    log(f"cmd> {line}")
    try:
        words = line.split()
        if not words:
            return
        w0, rest = words[0], line[len(words[0]):].strip()
        if w0 == "status":
            log(report())
        elif w0 == "drive" and rest.startswith("on"):
            arg = rest[2:].strip()
            log(start_drive(float(arg) if arg else DEFAULT_DZ))
        elif w0 == "drive" and rest == "off":
            stop_drive("command")
        elif w0 == "home":
            stop_drive("home")
            pawn, _ = pawn_and_pc()
            if pawn is not None and S.bl_home is not None:
                log(f"home: SetLocation {S.bl_home} -> {N.call(pawn.SetLocation, N.vec(*S.bl_home))}")
        elif w0 == "lead":
            if rest:
                S.lead_s = max(0.0, min(float(rest), 80.0)) / 1000.0
            log(f"world lead {S.lead_s * 1000:.0f} ms (prediction beyond the MW2 sample's age, max {MAX_PREDICT_S * 1000:.0f} ms)")
        elif w0 == "god":
            S.god = rest != "off"
            log(f"test god mode {'on' if S.god else 'off'} (while driving)")
        elif w0 == "scanner":
            S.native = nativescan.load() if rest == "native" else None
            S.trace_params = None
            log(f"scanner: {'native (bw2_scan.dll)' if S.native else 'python'}")
        elif w0 == "budget":
            S.budget_ms = float(rest)
            log(f"scan budget {S.budget_ms} ms per frame")
        elif w0 == "mw":
            log(f"to guest: {rest!r} queued={S.link.send_command(rest) if S.link else False}")
        elif w0 == "bench":
            n = int(rest or 400)
            pawn, _ = pawn_and_pc()
            x, y, z = loc(pawn)
            side = max(1, int(math.sqrt(n)))
            t0, hits = time.perf_counter(), 0
            with N.Tracer(pawn) as tr:
                for i in range(side):
                    for j in range(side):
                        px, py = x + (i - side / 2) * 100, y + (j - side / 2) * 100
                        hits += tr((px, py, z + 1000), (px, py, z - 2000)) is not None
            dt = time.perf_counter() - t0
            log(f"bench: {side * side} traces {dt * 1000:.1f} ms ({dt / (side * side) * 1e6:.1f} us each), {hits} hits")
        elif w0 == "con":
            get_pc().ConsoleCommand(rest)
        elif w0 == "py":
            exec(rest, {"unrealsdk": unrealsdk, "get_pc": get_pc, "ENGINE": ENGINE, "log": log, "S": S, "N": N, "P": P, "C": C, "scan": scan})  # noqa: S102
        else:
            log("commands: status | drive on [dz] | drive off | home | budget <ms> | mw <cmd> | bench <n> | con <cmd> | py <stmt>")
    except Exception:  # noqa: BLE001
        log(traceback.format_exc())


def take_commands() -> list[str]:
    try:
        with open(os.path.join(LOG_DIR, "bl_cmd.txt"), "r+", encoding="utf-8") as f:
            lines = [ln.strip() for ln in f.read().splitlines()]
            f.seek(0)
            f.truncate()
    except OSError:
        return []
    return [ln for ln in lines if ln and not ln.startswith("#")]


@hook("WillowGame.WillowGameViewportClient:Tick")
def viewport_tick(*_: Any) -> None:
    try:
        now = time.monotonic()
        if now >= S.next_cmd:
            S.next_cmd = now + 0.25
            for line in take_commands():
                run_command(line)
        tick()
        if S.mode != "off" and now >= S.next_report:
            S.next_report = now + 5.0
            log(report())
    except Exception:  # noqa: BLE001
        log(traceback.format_exc())
        stop_drive("error")


@command("bw2", description="Border Warfare 2: status | drive on [dz] | drive off | home | budget <ms> | mw <cmd> | bench | con | py")
def bw2_cmd(args: argparse.Namespace) -> None:
    run_command(" ".join(args.words))
    logging.info("[BW2] " + report())


bw2_cmd.add_argument("words", nargs="*")


# ---------------------------------------------------------------------------------------- hooks
# While driving, BL2's own actions from the controller are swallowed (MW2 gets the controller).
BLOCK_WHILE_DRIVING = (
    "Engine.PlayerController:StartFire",
    "WillowGame.WillowPlayerController:StartFire",
    "Engine.Pawn:DoJump",
    "WillowGame.WillowPlayerPawn:DoJump",
    "WillowGame.WillowPlayerController:DuckPressed",
    "WillowGame.WillowPlayerController:ReloadWeapon",
    "WillowGame.WillowPlayerController:Reload",
    "WillowGame.WillowPlayerController:NextWeapon",
    "WillowGame.WillowPlayerController:PrevWeapon",
    "WillowGame.WillowPlayerController:ThrowGrenade",
    "WillowGame.WillowPlayerController:Melee",
    "Engine.Pawn:UpdateEyeHeight",          # MW2's eye height wins
    "WillowGame.WillowPlayerPawn:UpdateEyeHeight",
    "WillowGame.WillowPlayerPawn:TakeDamage",     # test god mode (`bw2 god off` lets damage through)
)
VIEW_FUNCS = ("Engine.PlayerController:UpdateRotation", "WillowGame.WillowPlayerController:UpdateRotation")


def _block(_obj: Any, _args: Any, _ret: Any, func: Any) -> Any:
    if S.mode not in ("arriving", "driving"):
        return None
    if func.func.Name == "TakeDamage" and not S.god:
        return None
    if func.func.Name == "ThrowGrenade" and G.allow_grenade():
        return None  # the BL2 grenade a MW2 frag asked for (gunplay.throw_grenade)
    return hooks.Block


def _update_rotation(obj: Any, *_: Any) -> Any:
    """PlayerController.UpdateRotation: while driving, MW2's view replaces BL2's look input in the same
    frame the camera is built."""
    if S.mode != "driving" or obj != get_pc():
        return None
    g = guest()
    if g:
        S.view = g["viewAngles"]
        # Position here too, not in the viewport tick: BL2 builds the camera after the world tick and before
        # the viewport ticks, so a move made there showed one frame late (running felt out of sync).
        pawn = obj.Pawn
        if pawn is not None and pawn._get_address() == S.pawn_addr and g["flags"] & P.GUEST_IN_WORLD:
            try:
                S.hook_frame = S.frame
                puppet(pawn, obj, g)
            except Exception:  # noqa: BLE001
                log(traceback.format_exc())
    apply_view(obj)
    return hooks.Block


def _add(paths: tuple, ident: str, fn: Any) -> list[str]:
    added = []
    for path in paths:
        try:
            unrealsdk.find_object("Function", path)
        except Exception:  # noqa: BLE001
            continue
        hooks.add_hook(path, hooks.Type.PRE, ident, fn)
        added.append(path)
    return added


def on_enable() -> None:
    S.native = nativescan.load()
    V.start(log)
    log(f"Border Warfare 2 host enabled (M2). LogDir={LOG_DIR}; iNative fix verified={N.verify()}; "
        f"scanner {'native (bw2_scan.dll)' if S.native else 'python'}")
    S.block_hooks = _add(BLOCK_WHILE_DRIVING, "bw2_block", _block)
    S.view_hooks = _add(VIEW_FUNCS, "bw2_view", _update_rotation)
    log(f"blocking while driving: {', '.join(S.block_hooks)}; view from MW2 via {', '.join(S.view_hooks)}")


def on_disable() -> None:
    stop_drive("mod disabled")
    for path in S.block_hooks:
        hooks.remove_hook(path, hooks.Type.PRE, "bw2_block")
    for path in S.view_hooks:
        hooks.remove_hook(path, hooks.Type.PRE, "bw2_view")
    S.block_hooks, S.view_hooks = [], []
    log("disabled")


mod = build_mod(on_enable=on_enable, on_disable=on_disable)
