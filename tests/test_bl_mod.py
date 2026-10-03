"""Offline test of sdk_mods/bw2 against stand-ins for the BL2 SDK (unrealsdk, mods_base) and
tools/fake_mw2.py as the guest.

  python tests/test_bl_mod.py
Starts fake_mw2 (circle), loads the mod with stubbed SDK modules, drives for ~3 s of ticks and
checks the puppet followed: the pawn should trace a circle of radius 200 in * 2.54 = 508 uu
around where it started, stay on the stub ground, and face the walking direction.
"""
import ctypes
import math
import os
import shutil
import subprocess
import sys
import tempfile
import time
import types

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
os.environ["BW2_MAPPING"] = r"Local\BW2_test_%d" % os.getpid()  # never touch a live game link


# ---- SDK stand-ins ----------------------------------------------------------------------------
class Struct:
    def __init__(self, **kw):
        self.__dict__.update(kw)


class UFunc:
    def __init__(self, inative):
        self.buf = ctypes.create_string_buffer(0x100)
        ctypes.c_uint16.from_address(ctypes.addressof(self.buf) + 0x90).value = inative
        self.inative = inative

    def _get_address(self):
        return ctypes.addressof(self.buf)

    def cleared(self):
        return ctypes.c_uint16.from_address(ctypes.addressof(self.buf) + 0x90).value == 0


FUNCS = {"Engine.Actor:Trace": UFunc(277)}


class Bound:
    def __init__(self, func, fn):
        self.func, self.fn = func, fn

    def __call__(self, *a, **k):
        if self.func.inative and not self.func.cleared():
            return None  # what real ProcessEvent does for numbered natives
        return self.fn(*a, **k)


GROUND_Z = 1000.0
class Enemy:
    """Stands 800 uu along the last bullet, its head (top 20 uu of an 81 uu high cylinder) on the bullet's line."""

    ray = None  # (start, unit dir) of the last bullet's world trace

    def __init__(self):
        self.Class = Struct(Name="WillowAIPawn")
        self.health = 1000.0
        self.damage = []
        self.CylinderComponent = Struct(CollisionRadius=30.0, CollisionHeight=81.0)
        self.NextPawn = None

    @property
    def Location(self):
        (sx, sy, sz), (dx, dy, dz) = Enemy.ray or ((0.0, 0.0, 0.0), (1.0, 0.0, 0.0))
        return Struct(X=sx + dx * 800.0, Y=sy + dy * 800.0, Z=sz - 81.0 + 10.0)

    def _get_address(self):
        return id(self)

    def GetHealth(self):
        return self.health

    def TakeDamage(self, **kw):
        self.damage.append(kw)
        self.health -= kw["Damage"]


ENEMY = Enemy()


class Weapon:
    def __init__(self):
        self.Class = Struct(Name="WillowWeapon")
        self.ProjectilesPerShot = 1
        self.InstantHitDamage = 25.0
        self.Instigator = None  # set to the pawn below
        self.hits = []
        self.ProcessInstantHit = Bound(UFunc(0), lambda mode, impact, miss, n: self.hits.append((impact, n)))


class Pawn:
    def __init__(self):
        self.Location = Struct(X=5000.0, Y=-3000.0, Z=GROUND_Z + 83.0)
        self.CylinderComponent = Struct(CollisionHeight=81.0)
        self.Physics = 1
        self.bIsCrouched = False
        self.Class = Struct(Name="WillowPlayerPawn")
        self.moves = []
        self.Trace = Bound(FUNCS["Engine.Actor:Trace"], self._trace)
        self.SetLocation = Bound(UFunc(267), self._setloc)
        self.SetPhysics = Bound(UFunc(3970), self._setphys)
        self.Weapon = Weapon()
        self.Weapon.Instigator = Struct(Controller="PC")

    def _get_address(self):
        return id(self)

    def _trace(self, hl, hn, end, start, actors, *rest, **kw):
        if not actors and abs(start.Z - end.Z) < 1e-6 and math.dist((start.X, start.Y), (end.X, end.Y)) > 1000.0:
            n = math.dist((start.X, start.Y, start.Z), (end.X, end.Y, end.Z))  # a bullet's world trace
            Enemy.ray = ((start.X, start.Y, start.Z), ((end.X - start.X) / n, (end.Y - start.Y) / n, 0.0))
            return (None, hl, hn, None)
        if actors:  # a bullet: a trigger around the player first (must be passed through), then an enemy
            # the player stands inside a trigger volume (radius 40 uu): a trace starting inside it reports it right at
            # the start, every time, until the start is outside (2026-10-03: bullets gave up after 8 tiny steps)
            if getattr(self, "_vol_end", None) != (end.X, end.Y, end.Z):  # a new bullet: the volume is where it starts
                self._vol_end, self._vol = (end.X, end.Y, end.Z), (start.X, start.Y, start.Z)
            c = self._vol
            if math.dist(c, (start.X, start.Y, start.Z)) < 40.0:
                return (Struct(Class=Struct(Name="WillowTrigger"), bBlockActors=False), Struct(X=start.X, Y=start.Y, Z=start.Z),
                        Struct(X=0, Y=0, Z=1), Struct(BoneName="None"))
            mid = Struct(X=(start.X + end.X) / 2, Y=(start.Y + end.Y) / 2, Z=(start.Z + end.Z) / 2)
            return (ENEMY, mid, Struct(X=0, Y=0, Z=1), Struct(BoneName="Head"))
        if start.Z >= GROUND_Z >= end.Z:
            return (Struct(Class=Struct(Name="Terrain")), Struct(X=start.X, Y=start.Y, Z=GROUND_Z), Struct(X=0, Y=0, Z=1), None)
        return (None, hl, hn, None)

    def _setloc(self, v):
        self.Location = Struct(X=v.X, Y=v.Y, Z=v.Z)
        self.moves.append((v.X, v.Y, v.Z))
        return True

    def _setphys(self, p):
        self.Physics = p


class PC:
    def __init__(self, pawn):
        self.Pawn = pawn
        self.Rotation = Struct(Pitch=0, Yaw=0, Roll=0)
        self.ignore = 0
        self.IgnoreMoveInput = Bound(UFunc(0), lambda b: setattr(self, "ignore", self.ignore + (1 if b else -1)))
        self.IgnoreLookInput = Bound(UFunc(0), lambda b: None)
        self.grenades = 0
        self.StartThrowGrenadeSkill = Bound(UFunc(0), lambda: setattr(self, "grenades", self.grenades + 1))


def install_stubs(pc):
    unrealsdk = types.ModuleType("unrealsdk")
    unrealsdk.make_struct = lambda name, **kw: Struct(**kw)

    def find_object(cls, path):
        if path in FUNCS:
            return FUNCS[path]
        raise ValueError(path)

    unrealsdk.find_object = find_object
    unrealsdk.find_class = lambda name: Struct(Name=name)
    hooks = types.ModuleType("unrealsdk.hooks")
    hooks.Block = object()
    hooks.Type = Struct(PRE=0, POST=1)
    hooks.add_hook = lambda *a: None
    hooks.remove_hook = lambda *a: None
    logging = types.ModuleType("unrealsdk.logging")
    logging.info = print
    unrealsdk.hooks, unrealsdk.logging = hooks, logging
    mods_base = types.ModuleType("mods_base")
    world = Struct(Pauser=None, Trace=pc.Pawn.Trace, PawnList=ENEMY)  # the streamer traces from WorldInfo
    mods_base.ENGINE = Struct(GetCurrentWorldInfo=lambda: world)
    mods_base.get_pc = lambda: pc
    mods_base.hook = lambda *a, **k: (lambda f: f)

    class Cmd:
        def __init__(self, f):
            self.f = f

        def add_argument(self, *a, **k):
            pass

    mods_base.command = lambda *a, **k: (lambda f: Cmd(f))
    mods_base.build_mod = lambda **k: Struct(**k)
    mods_base.keybind = lambda *a, **k: (lambda f: f)
    sys.modules.update({"unrealsdk": unrealsdk, "unrealsdk.hooks": hooks, "unrealsdk.logging": logging, "mods_base": mods_base})


def main():
    tmp = tempfile.mkdtemp(prefix="bw2test_")
    pkg = os.path.join(tmp, "bw2")
    shutil.copytree(os.path.join(ROOT, "sdk_mods", "bw2"), pkg, ignore=shutil.ignore_patterns("__pycache__"))
    shutil.copy(os.path.join(ROOT, "tools", "bw2_proto.py"), pkg)
    shutil.copy(os.path.join(ROOT, "tools", "bw2_coords.py"), pkg)
    with open(os.path.join(pkg, "bw2.ini"), "w") as f:
        f.write("LogDir=%s\n" % tmp)
    pawn = Pawn()
    pc = PC(pawn)
    install_stubs(pc)
    sys.path.insert(0, tmp)

    fake = subprocess.Popen([sys.executable, os.path.join(ROOT, "tools", "fake_mw2.py"), "--script", "circle", "--seconds", "40",
                             "--origin", "-1734", "-4053", "-122", "--shoot"], stdout=subprocess.DEVNULL)
    try:
        import bw2  # noqa: E402
        start = (pawn.Location.X, pawn.Location.Y, pawn.Location.Z)
        for _ in range(60):  # link + guest come up
            bw2.tick()
            time.sleep(1 / 60)
        bw2.run_command("drive on")
        assert bw2.S.mode == "scanning", "drive did not start: " + bw2.S.mode
        # you keep running while BL2 is scanned: the pawn is 600 uu away by the time MW2's player arrives
        # (2026-10-03: every move was then refused, and BL2's physics floated the pawn around instead)
        pawn.Location = Struct(X=pawn.Location.X + 600.0, Y=pawn.Location.Y, Z=pawn.Location.Z)
        t0 = time.time()
        while bw2.S.mode in ("scanning", "arriving") and time.time() - t0 < 30:
            bw2.tick()
            time.sleep(0.004)
        scan_s = time.time() - t0
        assert bw2.S.mode == "driving", "never reached driving (mode %s)" % bw2.S.mode
        assert pc.ignore == 1 and pawn.Physics == 0
        g = bw2.guest()
        print("reached driving after %.1f s: %d traces, guest holds %d tiles / %d boxes" % (scan_s, bw2.S.traces, g["tilesHeld"], g["boxesHeld"]))
        assert g["tilesHeld"] == 9 and g["boxesHeld"] > 0
        assert abs(g["origin"][2] - (-122 - 12000 + 1)) < 0.01, "MW2 player not moved below its level: %r" % (g["origin"],)
        pawn.moves.clear()
        for _ in range(180):
            bw2.tick()
            time.sleep(1 / 60)
        # in game the pawn is moved inside UpdateRotation (before BL2 builds the camera), then the viewport ticks:
        # exactly one move per frame, from the hook
        n0, hook_frames = len(pawn.moves), 120
        for _ in range(hook_frames):
            bw2._update_rotation(pc)
            bw2.tick()
            time.sleep(1 / 60)
        hook_moves = len(pawn.moves) - n0
        assert hook_frames - 2 <= hook_moves <= hook_frames, "hook path moved %d times in %d frames" % (hook_moves, hook_frames)
        drive_moves = list(pawn.moves)  # the path checks below are about this first drive
        # into a vehicle: MW2 control hands over without touching the car; out again: it comes back by itself
        car_moves = []
        car = Struct(Class=Struct(Name="WillowVehicle_WheeledVehicle"), _get_address=lambda: 4242,
                     Location=Struct(X=pawn.Location.X, Y=pawn.Location.Y, Z=pawn.Location.Z), CylinderComponent=Struct(CollisionHeight=60.0),
                     SetLocation=Bound(UFunc(267), lambda v: car_moves.append(v) or True), Physics=0)
        pc.Pawn = car
        bw2.tick()
        assert bw2.S.mode == "off" and not car_moves and car.Physics == 0, "the vehicle was moved or MW2 control stayed on"
        assert bw2.S.resume_at, "not waiting to resume after the vehicle"
        pc.Pawn = pawn
        t0 = time.time()
        while bw2.S.mode == "off" and time.time() - t0 < 5:  # retried once a second, once MW2's player is home
            bw2.tick()
            time.sleep(1 / 60)
        assert bw2.S.mode != "off", "MW2 control did not come back after the vehicle"
        t0 = time.time()
        while bw2.S.mode != "driving" and time.time() - t0 < 30:
            bw2.tick()
            time.sleep(0.004)
        assert bw2.S.mode == "driving", "never reached driving again after the vehicle (%s)" % bw2.S.mode
        print("vehicle: handed over and resumed")
        bw2.run_command("status")
        bw2.run_command("drive off")
        assert bw2.S.mode == "off" and pc.ignore == 0 and pawn.Physics == 2
        assert FUNCS["Engine.Actor:Trace"].inative == 277 and not FUNCS["Engine.Actor:Trace"].cleared(), "iNative not restored"
        bw2.tick()  # publishes the "go home" teleport
        h = bw2.S.link.read_slot(bw2.P.OFF_HOST, bw2.P.HOST)
        assert math.dist(h["teleportPos"], bw2.S.mw_home) < 0.01 and abs(h["teleportPos"][2] + 122) < 0.01, "MW2 player not sent home: %r" % (h["teleportPos"],)
        assert not h["flags"] & bw2.P.HOST_COLLISION, "collision flag still set after drive off"
        xs = [m[0] for m in drive_moves]
        ys = [m[1] for m in drive_moves]
        zs = [m[2] for m in drive_moves]
        span = max(math.dist((x, y), (xs[0], ys[0])) for x, y in zip(xs, ys))
        want_z = (GROUND_Z + 2.0) + 1.0 * 2.54 + 81.0  # anchor feet (1002) + 1 in arrival lift, + cylinder half height
        print("moves %d, forced %d, refused %d, span %.0f uu (expect ~1016), z %.2f..%.2f (expect %.2f), yaw now %d, eye %.1f" % (
            len(drive_moves), bw2.S.forced, bw2.S.refused, span, min(zs), max(zs), want_z, pc.Rotation.Yaw, pawn.EyeHeight))
        print(open(os.path.join(tmp, "bl_host.log")).read())
        assert len(drive_moves) > 120, "puppet barely moved"
        assert bw2.S.refused == 0, "moves refused after the pawn drifted during the scan"
        assert 900 < span < 1030, "path size wrong"
        assert all(abs(z - want_z) < 0.05 for z in zs), "puppet height wrong"
        assert abs(pawn.EyeHeight - (60 * 2.54 - 81)) < 0.01, "eye height not MW2's"
        dmg = ENEMY.damage
        print("gunplay: %d hits damaged the enemy (health 1000 -> %.0f, head x2), %d grenade(s); %s" % (
            len(dmg), ENEMY.health, pc.grenades, bw2.G.report()))
        assert len(dmg) >= 4 and all(d["Damage"] == 50.0 and d["InstigatedBy"] == "PC" for d in dmg), "hits did no damage"
        assert bw2.G.ST.misses == 0 and bw2.G.ST.pawn_hits == bw2.G.ST.bullets, "a trigger around the player stopped bullets"
        assert pc.grenades >= 1, "MW2 frag did not throw a BL2 grenade"
        print("test_bl_mod OK")
    finally:
        fake.wait()
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
