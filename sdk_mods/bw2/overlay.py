"""Border Warfare 2 - M3 view: MW2's hands and gun instead of BL2's.

bw2_overlay.dll (src/bl_overlay/overlay.cpp) draws MW2's keyed first-person frame over BL2 at Present;
here we start it, switch it on while driving, and hide BL2's own first-person arms and gun meanwhile
(re-applied every second: switching weapons brings the gun mesh back). BL2's HUD is switched off too
(its ToggleHUD exec, only when its state is wrong); MW2's HUD (ammo, grenades, crosshair) arrives with its frame,
and the DLL draws BL2's shield and health as two bars with a red flash on damage (M5).
"""

from __future__ import annotations

import ctypes
import os
import struct
import time
from typing import Any

from . import natives as N

HERE = os.path.dirname(os.path.abspath(__file__))


class State:
    dll: Any = None
    on = False
    next_hide = 0.0
    hidden: list = []  # components we hid (restored when driving stops)
    hud_shown = False
    hud_pc: Any = None  # the controller whose HUD we switched off


O = State()


def start(log: Any) -> bool:
    path = os.path.join(HERE, "bw2_overlay.dll")
    if struct.calcsize("P") != 4 or not os.path.exists(path):
        log("overlay: bw2_overlay.dll not available")
        return False
    O.dll = ctypes.CDLL(path)
    O.dll.bw2_overlay_start.restype = ctypes.c_int
    O.dll.bw2_overlay_enable.argtypes = [ctypes.c_int]
    O.dll.bw2_overlay_stats.argtypes = [ctypes.POINTER(ctypes.c_uint32)]
    O.dll.bw2_overlay_hud.argtypes = [ctypes.c_int] + [ctypes.c_float] * 4
    r = O.dll.bw2_overlay_start()
    log(f"overlay: start -> {r} ({'hooked' if r == 1 else 'FAILED'})")
    return r == 1


def _set_hidden(comp: Any, hidden: bool) -> None:
    if comp is not None:
        N.call(comp.SetHidden, hidden)


def _hud(pc: Any, show: bool) -> bool:
    """BL2's HUD on/off; returns True if the controller has a HUD."""
    try:
        hud = pc.myHUD if pc is not None else None
        if hud is None:
            return False
        if bool(hud.bShowHUD) != show:
            pc.ConsoleCommand("ToggleHUD")
        return True
    except Exception:  # noqa: BLE001  (the controller may be gone)
        return False


def update(driving: bool, pawn: Any, log: Any) -> None:
    """Every tick: MW2's view on while driving, BL2's arms and gun hidden; everything back otherwise."""
    if O.dll is None:
        return
    if driving != O.on:
        O.on = driving
        O.dll.bw2_overlay_enable(1 if driving else 0)
        if not driving:
            for comp in O.hidden:
                try:
                    _set_hidden(comp, False)
                except Exception:  # noqa: BLE001  (the pawn may be gone)
                    pass
            O.hidden = []
            _hud(O.hud_pc, True)
            O.hud_pc = None
        log(f"overlay: {'on' if driving else 'off'}")
    if driving and pawn is not None:  # M5 HUD: BL2's shield and health (BL2's own HUD is off while driving)
        try:
            O.dll.bw2_overlay_hud(1, float(pawn.GetHealth()), float(pawn.GetMaxHealth()),
                                  float(pawn.GetShieldStrength()), float(pawn.GetMaxShieldStrength()))
        except Exception:  # noqa: BLE001
            O.dll.bw2_overlay_hud(0, 0.0, 0.0, 0.0, 0.0)
    elif driving != O.hud_shown:
        O.dll.bw2_overlay_hud(0, 0.0, 0.0, 0.0, 0.0)
    O.hud_shown = driving
    if driving and pawn is not None and time.monotonic() >= O.next_hide:
        O.next_hide = time.monotonic() + 1.0
        try:
            for comp in (pawn.Arms, pawn.Weapon.FirstPersonMesh if pawn.Weapon is not None else None):
                if comp is not None and not comp.HiddenGame:
                    _set_hidden(comp, True)
                    if comp not in O.hidden:
                        O.hidden.append(comp)
            pc = pawn.Controller
            if pc is not None and _hud(pc, False):
                O.hud_pc = pc
        except Exception as ex:  # noqa: BLE001
            log(f"overlay: hiding BL2's arms/gun/HUD failed: {ex}")


def report() -> str:
    if O.dll is None:
        return "overlay off"
    out = (ctypes.c_uint32 * 8)()  # the DLL writes 8 values
    O.dll.bw2_overlay_stats(out)
    g = (ctypes.c_uint32 * 4)()
    try:
        O.dll.bw2_overlay_guard(g)
    except AttributeError:  # an older DLL
        pass
    return (f"overlay drawn {out[0]} uploaded {out[1]} errors {out[2]} mw2 frame {out[3]}"
            f" | hook guard: loops {g[0]}, d3d9 present {'known' if g[1] else 'unknown'}, reset {'known' if g[3] else 'unknown'}")
