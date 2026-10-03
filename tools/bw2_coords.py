"""MW2 (guest) <-> BL2 (host) coordinate conversion. Pure Python, standard library only: also copied
into the BL2 SDK mod.

MW2 / IW4: inches, Z up, right-handed. Yaw in degrees, counter-clockwise from +X seen from above;
pitch in degrees, positive looks DOWN. The player origin is at the feet.
BL2 / UE3: unreal units (~1 cm), Z up, left-handed (+Y is to the right of +X). Rotator ints with
65536 = 360 degrees, yaw clockwise from +X seen from above, pitch positive looks UP. A pawn's
Location is the centre of its collision cylinder (feet + CollisionHeight).

Measured in Phase 0b: MW2 eye 60 in above the feet, BL2 eye 152 uu -> SCALE = 2.54 uu per inch.

An Anchor ties one MW2 point to one BL2 point (feet); everything else is relative to it, so
neither game's absolute coordinates matter:
    bl = blA + SCALE * (dx, -dy, dz)   where d = mw - mwA
"""
import math

SCALE = 2.54
UU_PER_DEG = 65536.0 / 360.0


class Anchor:
    def __init__(self, mw=(0.0, 0.0, 0.0), bl=(0.0, 0.0, 0.0)):
        self.mw = tuple(float(v) for v in mw)
        self.bl = tuple(float(v) for v in bl)

    def to_bl(self, p):
        """MW2 point (feet) -> BL2 point (feet)."""
        return (self.bl[0] + (p[0] - self.mw[0]) * SCALE,
                self.bl[1] - (p[1] - self.mw[1]) * SCALE,
                self.bl[2] + (p[2] - self.mw[2]) * SCALE)

    def to_mw(self, p):
        """BL2 point (feet) -> MW2 point (feet)."""
        return (self.mw[0] + (p[0] - self.bl[0]) / SCALE,
                self.mw[1] - (p[1] - self.bl[1]) / SCALE,
                self.mw[2] + (p[2] - self.bl[2]) / SCALE)


def vec_to_bl(v):
    """A direction or velocity (no anchor): MW2 units -> BL2 units."""
    return (v[0] * SCALE, -v[1] * SCALE, v[2] * SCALE)


def wrap_uu(a):
    return int(round(a)) % 65536


def yaw_to_bl(yaw_deg):
    """MW2 yaw (deg, CCW) -> UE3 yaw (uu, CW)."""
    return wrap_uu(-yaw_deg * UU_PER_DEG)


def yaw_to_mw(yaw_uu):
    y = -(yaw_uu % 65536) / UU_PER_DEG
    return (y + 180.0) % 360.0 - 180.0


def pitch_to_bl(pitch_deg):
    """MW2 pitch (deg, down +) -> UE3 pitch (uu, up +), wrapped to 0..65535 like UE3 stores it."""
    return wrap_uu(-pitch_deg * UU_PER_DEG)


def forward_mw(yaw_deg, pitch_deg=0.0):
    """Unit view direction in MW2 space."""
    y, p = math.radians(yaw_deg), math.radians(pitch_deg)
    return (math.cos(p) * math.cos(y), math.cos(p) * math.sin(y), -math.sin(p))


def forward_bl(yaw_uu, pitch_uu=0):
    """Unit view direction in BL2 space from UE3 rotator values."""
    y = (yaw_uu % 65536) / UU_PER_DEG
    p = ((pitch_uu + 32768) % 65536 - 32768) / UU_PER_DEG
    y, p = math.radians(y), math.radians(p)
    return (math.cos(p) * math.cos(y), math.cos(p) * math.sin(y), math.sin(p))


if __name__ == "__main__":
    # self-test: python bw2_coords.py
    a = Anchor(mw=(-1734.8, -4053.3, -122.7), bl=(9303.5, 2024.5, 3610.6))
    for p in [(-1734.8, -4053.3, -122.7), (0, 0, 0), (100.5, -20.25, 300)]:
        back = a.to_mw(a.to_bl(p))
        assert all(abs(x - y) < 1e-6 for x, y in zip(p, back)), (p, back)
    assert a.to_bl(a.mw) == a.bl
    # walking 100 in along MW2 +X moves 254 uu along BL2 +X; +Y (MW2 left) is BL2 -Y
    b = a.to_bl((a.mw[0] + 100, a.mw[1], a.mw[2]))
    assert abs(b[0] - a.bl[0] - 254) < 1e-6
    c = a.to_bl((a.mw[0], a.mw[1] + 100, a.mw[2]))
    assert abs(c[1] - a.bl[1] + 254) < 1e-6
    # facing directions agree after conversion, for a spread of yaw/pitch
    for yaw in range(-180, 180, 15):
        for pitch in (-60, -10, 0, 25, 80):
            f_mw = vec_to_bl(forward_mw(yaw, pitch))
            n = math.sqrt(sum(x * x for x in f_mw))
            f_mw = tuple(x / n for x in f_mw)
            f_bl = forward_bl(yaw_to_bl(yaw), pitch_to_bl(pitch))
            assert all(abs(x - y) < 1e-3 for x, y in zip(f_mw, f_bl)), (yaw, pitch, f_mw, f_bl)
    for yaw in (-179.0, -90.0, 0.0, 45.5, 90.0, 179.0):
        assert abs(((yaw_to_mw(yaw_to_bl(yaw)) - yaw) + 180) % 360 - 180) < 0.01
    print("bw2_coords self-test OK")
