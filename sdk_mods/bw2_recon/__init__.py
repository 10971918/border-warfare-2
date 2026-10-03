"""Border Warfare 2 - Phase 0b BL2 recon probe (read-only).

Answers the BL2-side runtime questions for the GO/NO-GO:
  * player pawn units: collision cylinder, eye height, speeds, location/rotation conventions
  * camera: location, rotation, FOV
  * can BL2's collision near the player be exported? -> how fast are Actor.Trace calls from the SDK
    (`bench`), and what do they hit
It never changes the game unless you send it a `con` or `py` command.

Log: <LogDir>\\bl_recon.log. Live commands: write lines to <LogDir>\\bl_cmd.txt
  status | bench <n> [extent] | con <console command> | py <python statement>
"""

from __future__ import annotations

import math
import os
import time
import traceback
from typing import Any

import unrealsdk
from mods_base import ENGINE, build_mod, get_pc, hook
from unrealsdk import logging

HERE = os.path.dirname(os.path.abspath(__file__))


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
_next_report = 0.0
_next_cmd = 0.0


def log(msg: str) -> None:
    global _log_file
    if _log_file is None:
        os.makedirs(LOG_DIR, exist_ok=True)
        _log_file = open(os.path.join(LOG_DIR, "bl_recon.log"), "w", encoding="utf-8")  # noqa: SIM115
    _log_file.write(time.strftime("%H:%M:%S ") + msg + "\n")
    _log_file.flush()


def vec(v: Any) -> str:
    return f"({v.X:.1f} {v.Y:.1f} {v.Z:.1f})"


def rot(r: Any) -> str:
    return f"(p={r.Pitch} y={r.Yaw} r={r.Roll})"


def pawn_status() -> None:
    pc = get_pc()
    pawn = pc.Pawn if pc is not None else None
    if pawn is None:
        log("status: no pawn")
        return
    try:
        world = str(ENGINE.GetCurrentWorldInfo().GetStreamingPersistentMapName())
    except Exception:  # noqa: BLE001
        world = "?"
    cyl = pawn.CylinderComponent
    log(
        f"status: map={world} pawn={pawn.Class.Name} loc={vec(pawn.Location)} rot={rot(pawn.Rotation)} "
        f"vel={vec(pawn.Velocity)} physics={pawn.Physics} cyl r={cyl.CollisionRadius:.1f} h={cyl.CollisionHeight:.1f} "
        f"eye={pawn.EyeHeight:.1f} baseEye={pawn.BaseEyeHeight:.1f} ground={pawn.GroundSpeed:.0f} jumpZ={pawn.JumpZ:.0f}",
    )
    cam = pc.PlayerCamera
    if cam is not None:
        pov = cam.CameraCache.POV
        log(f"camera: {cam.Class.Name} loc={vec(pov.Location)} rot={rot(pov.Rotation)} fov={pov.FOV:.2f}")


def bench(n: int, extent: float) -> None:
    """n downward traces on a grid (100 uu spacing) around the player: timing + what they hit."""
    pawn = get_pc().Pawn
    if pawn is None:
        log("bench: no pawn")
        return
    side = max(1, int(math.sqrt(n)))
    o = pawn.Location
    ext = unrealsdk.make_struct("Vector", X=extent, Y=extent, Z=extent)
    zero = unrealsdk.make_struct("Vector", X=0.0, Y=0.0, Z=0.0)
    hits, kinds, zs = 0, {}, []
    t0 = time.perf_counter()
    for i in range(side):
        for j in range(side):
            x = o.X + (i - side / 2) * 100.0
            y = o.Y + (j - side / 2) * 100.0
            start = unrealsdk.make_struct("Vector", X=x, Y=y, Z=o.Z + 1000.0)
            end = unrealsdk.make_struct("Vector", X=x, Y=y, Z=o.Z - 2000.0)
            # Actor.Trace(out HitLocation, out HitNormal, TraceEnd, TraceStart, bTraceActors, Extent, ...)
            # returns (hit actor, HitLocation, HitNormal, ...)
            kw = {"Extent": ext} if extent > 0 else {}
            res = pawn.Trace(HitLocation=zero, HitNormal=zero, TraceEnd=end, TraceStart=start, bTraceActors=False, **kw)
            actor, hit_loc = res[0], res[1]
            if actor is not None:
                hits += 1
                kinds[actor.Class.Name] = kinds.get(actor.Class.Name, 0) + 1
                zs.append(hit_loc.Z)
    dt = time.perf_counter() - t0
    count = side * side
    log(
        f"bench: {count} traces (extent {extent}) in {dt * 1000:.1f} ms = {dt / count * 1e6:.1f} us/trace; "
        f"hits {hits}; actors {kinds}; hitZ min/max {min(zs, default=0):.1f}/{max(zs, default=0):.1f}",
    )


def run_command(line: str) -> None:
    log(f"cmd> {line}")
    try:
        if line == "status":
            pawn_status()
        elif line.startswith("bench"):
            parts = line.split()
            bench(int(parts[1]) if len(parts) > 1 else 400, float(parts[2]) if len(parts) > 2 else 0.0)
        elif line.startswith("con "):
            get_pc().ConsoleCommand(line[4:])
        elif line.startswith("py "):
            exec(line[3:], {"unrealsdk": unrealsdk, "get_pc": get_pc, "ENGINE": ENGINE, "log": log})  # noqa: S102
        else:
            log("unknown command (status | bench <n> [extent] | con <cmd> | py <stmt>)")
    except Exception:  # noqa: BLE001
        log(traceback.format_exc())


def take_commands() -> list[str]:
    path = os.path.join(LOG_DIR, "bl_cmd.txt")
    try:
        with open(path, "r+", encoding="utf-8") as f:
            lines = [ln.strip() for ln in f.read().splitlines()]
            f.seek(0)
            f.truncate()
    except OSError:
        return []
    return [ln for ln in lines if ln and not ln.startswith("#")]


@hook("WillowGame.WillowGameViewportClient:Tick")
def viewport_tick(*_: Any) -> None:
    global _next_report, _next_cmd
    now = time.monotonic()
    if now >= _next_cmd:
        _next_cmd = now + 0.25
        for line in take_commands():
            run_command(line)
    if now >= _next_report:
        _next_report = now + 5.0
        try:
            pawn_status()
        except Exception:  # noqa: BLE001
            log(traceback.format_exc())


def on_enable() -> None:
    log(f"Border Warfare 2 BL2 recon probe enabled. LogDir={LOG_DIR}")
    logging.info("[BW2 recon] enabled, logging to " + LOG_DIR)


mod = build_mod(on_enable=on_enable)
