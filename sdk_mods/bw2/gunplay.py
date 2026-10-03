"""Border Warfare 2 - M4 gunplay: MW2 fires, BL2 hits.

MW2 decides when you fire and where every bullet goes (its fire rate, spread, recoil, pellets, ADS);
the guest sends each bullet's start/end. Here each bullet is traced through BL2's world (actors too)
and the hit is handed to your equipped BL2 gun's own ProcessInstantHit, so BL2 computes the damage,
elements, crits, impact effects, kills, XP and loot. A MW2 frag makes BL2 throw your BL2 grenade.

Pellets: a MW2 shot of n pellets is matched to the BL2 gun's ProjectilesPerShot p - the first p MW2
pellets are used, and if p > n each of them counts p/n hits - so a BL2 shotgun stays a shotgun's worth of
damage whatever MW2 weapon you hold.
"""

from __future__ import annotations

import math
import time
from typing import Any

import unrealsdk

from mods_base import ENGINE

from . import bw2_proto as P
from . import natives as N


class Stats:
    shots = bullets = hits = misses = grenades = errors = pawn_hits = 0
    last_hit = ""
    grenade_until = 0.0
    last_pawn: Any = None      # the last enemy a bullet hit (debugging)
    mode = "takedamage"        # how a hit becomes damage: takedamage | pipeline (ProcessInstantHit: hits, no damage)
    by_class: dict = {}        # what bullets hit in BL2, by class name (why some enemies survive)
    synthetic = 0              # enemy hits found by the cylinder test without a bone from the short trace
    synthetic_head = False
    passed_by: dict = {}       # pass-through actors by class name
    passed = 0                 # bullets that went through a trigger/volume on the way
    survivors = 0              # enemy hits that left the enemy alive
    damage_type: Any = None  # BL2's own ThrowGrenade is let through until then (it fires after the throw animation)


ST = Stats()


def _norm(v: tuple) -> tuple:
    n = math.sqrt(sum(c * c for c in v)) or 1.0
    return tuple(c / n for c in v)


def handle_events(link: P.Link, driving: bool, pawn: Any, pc: Any, anchor: Any, log: Any) -> None:
    """Drain the guest's event ring every tick (so it never fills); act on it only while driving."""
    events = link.drain_events()
    if not driving or not events:
        return
    shots: dict[int, list[dict]] = {}
    for rtype, ev in events:
        if rtype == P.REC_SHOT:
            shots.setdefault(ev["shotId"], []).append(ev)
        elif rtype == P.REC_GRENADE:
            throw_grenade(pc, log)
    if not shots:
        return
    weapon = pawn.Weapon
    if weapon is None:
        return
    for bullets in shots.values():
        fire(pawn, weapon, sorted(bullets, key=lambda b: b["pellet"]), anchor, log)


def fire(pawn: Any, weapon: Any, bullets: list[dict], anchor: Any, log: Any) -> None:
    ST.shots += 1
    p = max(1, int(getattr(weapon, "ProjectilesPerShot", 1) or 1))
    n = len(bullets)
    hits_each = max(1, round(p / n)) if p > n else 1
    zero = N.vec(0, 0, 0)
    for ev in bullets[:p]:
        ST.bullets += 1
        s = anchor.to_bl(ev["start"])
        e = anchor.to_bl(ev["end"])
        try:
            actor, hl, hn, hi = _trace(pawn, s, e)
            if actor is None:
                ST.misses += 1
                continue
            d = _norm(tuple(b - a for a, b in zip(s, e)))
            impact = unrealsdk.make_struct("ImpactInfo", HitActor=actor, HitLocation=hl, HitNormal=hn, RayDir=N.vec(*d),
                                           StartTrace=N.vec(*s), HitInfo=hi, EndTrace=N.vec(*e))
            name = actor.Class.Name
            ST.by_class[name] = ST.by_class.get(name, 0) + 1
            pawn_hit = "Pawn" in name
            before = _health(actor) if pawn_hit else None
            dist = sum((a - b) ** 2 for a, b in zip(s, (hl.X, hl.Y, hl.Z))) ** 0.5
            if pawn_hit and ST.mode == "takedamage":
                ST.last_pawn = actor
                take_damage(actor, weapon, hl, d, hi, hits_each)
            else:
                N.call(weapon.ProcessInstantHit, 0, impact, False, hits_each)
            ST.hits += 1
            if pawn_hit:
                ST.pawn_hits += 1
                after = _health(actor)
                if after > 0:
                    ST.survivors += 1
                if ST.pawn_hits <= 20 or (after > 0 and ST.survivors <= 100):  # survivors: why some enemies don't die
                    log(f"hit {name} {getattr(actor, 'Name', '?')} bone {getattr(hi, 'BoneName', None)}{' (cylinder)' if ST.synthetic_head or getattr(hi, 'BoneName', None) is None else ''} at {dist:.0f} uu: health {before:.0f} -> {after:.0f}"
                        f" (dmg {float(weapon.InstantHitDamage) * hits_each:.0f})")
            elif ST.hits <= 300 and ST.hits % 10 == 0:
                log(f"bullet hit {name} at {dist:.0f} uu (not an enemy)")
        except Exception as ex:  # noqa: BLE001
            ST.errors += 1
            if ST.errors < 5:
                log(f"shot failed: {ex}")


def _trace(pawn: Any, s: tuple, e: tuple) -> tuple:
    """What a bullet from s to e hits: (actor, HitLocation, HitNormal, TraceHitInfo), actor None = nothing.

    Triggers, waypoints and other invisible volumes report a hit wherever a trace starts inside them, and the
    player often stands inside one (a mission waypoint was ~700 uu across); stepping over them either never got
    out or jumped past enemies (2026-10-03). So: the world (walls, rocks, terrain) is traced without actors,
    which ignores those volumes entirely; enemies are found by intersecting the bullet with the collision
    cylinder of each pawn in BL2's live pawn list; the nearest one in front of the wall is hit, and a short
    actor trace right in front of it supplies the bone (head) when it can."""
    ST.synthetic_head = False
    zero = N.vec(0, 0, 0)
    seg = tuple(b - a for a, b in zip(s, e))
    length = math.hypot(*seg)
    if length < 1.0:
        return None, None, None, None
    d = tuple(c / length for c in seg)
    r = N.call(pawn.Trace, zero, zero, N.vec(*e), N.vec(*s), False, zero, unrealsdk.make_struct("TraceHitInfo"))
    wall, whl, whn, whi = r[0], r[1], r[2], r[3]
    wall_t = math.dist(s, (whl.X, whl.Y, whl.Z)) if wall is not None else length
    hits = []
    for p in _pawns(pawn):
        t = _ray_cylinder(s, d, p)
        if t is not None and t < wall_t:
            hits.append((t, p))
    for t, target in sorted(hits, key=lambda h: h[0]):
        if _health(target) <= 0:  # a corpse still in the list
            ST.passed += 1
            ST.passed_by["dead pawn"] = ST.passed_by.get("dead pawn", 0) + 1
            continue
        radius = float(target.CylinderComponent.CollisionRadius)
        a = tuple(c + dc * max(t - 30.0, 0.0) for c, dc in zip(s, d))
        b = tuple(c + dc * min(t + 2.0 * radius + 60.0, wall_t) for c, dc in zip(s, d))
        r = N.call(pawn.Trace, zero, zero, N.vec(*b), N.vec(*a), True, zero, unrealsdk.make_struct("TraceHitInfo"))
        if r[0] is not None and _same(r[0], target):
            return r[0], r[1], r[2], r[3]
        hit = tuple(c + dc * t for c, dc in zip(s, d))  # the cylinder hit; head = its top part
        top = target.Location.Z + float(target.CylinderComponent.CollisionHeight)
        info = unrealsdk.make_struct("TraceHitInfo")
        ST.synthetic += 1
        ST.synthetic_head = top - hit[2] < HEAD_UU
        return target, N.vec(*hit), N.vec(-d[0], -d[1], -d[2]), info
    return wall, whl, whn, whi


HEAD_UU = 22.0  # cylinder hits this close to the top count as headshots when no bone is known


def _same(a: Any, b: Any) -> bool:
    try:
        return a._get_address() == b._get_address()
    except Exception:  # noqa: BLE001
        return a is b


def _pawns(me: Any) -> list:
    """The live pawns other than ours (BL2's PawnList; only reads properties, no sweep of all objects)."""
    out = []
    try:
        p = ENGINE.GetCurrentWorldInfo().PawnList
    except Exception:  # noqa: BLE001
        return out
    for _ in range(512):
        if p is None:
            break
        if not _same(p, me) and "Pawn" in p.Class.Name and "PlayerPawn" not in p.Class.Name:
            out.append(p)
        p = p.NextPawn
    return out


def _ray_cylinder(s: tuple, d: tuple, p: Any) -> float | None:
    """Distance along the ray (s, unit d) to pawn p's collision cylinder, or None."""
    try:
        c = p.CylinderComponent
        r, h = float(c.CollisionRadius), float(c.CollisionHeight)
        L = p.Location
        cx, cy, cz = L.X, L.Y, L.Z
    except Exception:  # noqa: BLE001
        return None
    if r <= 0 or h <= 0:
        return None
    ox, oy = s[0] - cx, s[1] - cy
    a = d[0] * d[0] + d[1] * d[1]
    ts = []
    if a > 1e-9:  # the side
        b = 2 * (ox * d[0] + oy * d[1])
        k = ox * ox + oy * oy - r * r
        disc = b * b - 4 * a * k
        if disc >= 0:
            for t in ((-b - math.sqrt(disc)) / (2 * a), (-b + math.sqrt(disc)) / (2 * a)):
                if t >= 0 and abs(s[2] + d[2] * t - cz) <= h:
                    ts.append(t)
    if abs(d[2]) > 1e-9:  # the caps
        for zc in (cz - h, cz + h):
            t = (zc - s[2]) / d[2]
            if t >= 0:
                x, y = ox + d[0] * t, oy + d[1] * t
                if x * x + y * y <= r * r:
                    ts.append(t)
    if ox * ox + oy * oy <= r * r and abs(s[2] - cz) <= h:
        ts.append(0.0)  # starts inside it
    return min(ts) if ts else None


def take_damage(actor: Any, weapon: Any, hit_loc: Any, ray: tuple, hit_info: Any, hits: int) -> None:
    """Damage the hit enemy directly: your BL2 gun's per-shot damage (x2 on a head bone, standing in for BL2's
    crit), you as the instigator so the kill, XP and loot are yours. (ProcessInstantHit from outside BL2's own
    firing sequence registers hits but applies no damage - found in game 2026-10-03.)"""
    dmg = float(weapon.InstantHitDamage) * hits
    bone = str(getattr(hit_info, "BoneName", "") or "")
    if "head" in bone.lower() or (bone in ("", "None") and ST.synthetic_head):
        dmg *= 2.0
    if ST.damage_type is None:
        ST.damage_type = unrealsdk.find_class("DamageType")
    pc = weapon.Instigator.Controller if weapon.Instigator is not None else None
    actor.TakeDamage(Damage=dmg, InstigatedBy=pc, HitLocation=hit_loc, Momentum=N.vec(ray[0] * 500, ray[1] * 500, ray[2] * 500),
                     DamageType=ST.damage_type, HitInfo=hit_info, DamageCauser=weapon)


def _health(actor: Any) -> float:
    """Health of the one live pawn a bullet just hit (never called on other objects: a broad sweep of
    pawns through GetHealth crashed BL2 on 2026-10-03)."""
    try:
        return float(actor.GetHealth())
    except Exception:  # noqa: BLE001
        return -1.0


def throw_grenade(pc: Any, log: Any) -> None:
    ST.grenades += 1
    ST.grenade_until = time.monotonic() + 2.0
    try:
        N.call(pc.StartThrowGrenadeSkill)
    except Exception as ex:  # noqa: BLE001
        log(f"grenade failed: {ex}")


def allow_grenade() -> bool:
    return time.monotonic() < ST.grenade_until


def report() -> str:
    return (f"shots {ST.shots} bullets {ST.bullets} hits {ST.hits} misses {ST.misses} grenades {ST.grenades}"
            + f" enemy hits {ST.pawn_hits} survived {ST.survivors} passed {ST.passed}"
            + (f" errors {ST.errors}" if ST.errors else "")
            + " | passed " + ", ".join(f"{k} {v}" for k, v in sorted(ST.passed_by.items(), key=lambda kv: -kv[1])[:3])
            + " | hit " + ", ".join(f"{k} {v}" for k, v in sorted(ST.by_class.items(), key=lambda kv: -kv[1])[:6]))
