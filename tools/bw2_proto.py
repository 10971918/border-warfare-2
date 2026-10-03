"""Python mirror of protocol/bw2_protocol.h (the C++ header is the source of truth).

Used by the tools/fakes (Python 3.12 x64) and copied into the BL2 SDK mod (Python 3.14 x86), so it
uses only the standard library. tests/layout_test.exe prints the C++ offsets; `python bw2_proto.py`
checks them against this file.
"""
import ctypes
import mmap
import os
import struct

MAGIC = 0x4C325742  # "BW2L"
VERSION = 2
MAPPING_NAME = os.environ.get("BW2_MAPPING", "Local\\BW2_v1")  # tests use a private name

OFF_HEADER = 0x0
OFF_HOST = 0x100
OFF_PAD = 0x200
OFF_GUEST = 0x300
OFF_EVENT_RING = 0x1000
EVENT_RING_BYTES = 0x10000
OFF_CMD_RING = 0x11000
CMD_RING_BYTES = 0xF000
OFF_COLLISION_RING = 0x20000
COLLISION_RING_BYTES = 16 << 20
MAPPING_BYTES = OFF_COLLISION_RING + COLLISION_RING_BYTES
HEARTBEAT_TIMEOUT_MS = 1500

HOST_IN_GAME, HOST_MENU_OPEN, HOST_LOADING, HOST_DRIVE_PUPPET, HOST_COLLISION = 1, 2, 4, 8, 16
PAD_ACTIVE = 1
(GUEST_IN_WORLD, GUEST_ON_GROUND, GUEST_CROUCHED, GUEST_PRONE, GUEST_SPRINTING, GUEST_ADS,
 GUEST_PAD_REAL, GUEST_PAD_VIRTUAL, GUEST_FOCUSED) = (1 << i for i in range(9))
REC_PAD, REC_COMMAND, REC_TILE, REC_CLEAR_ALL, REC_SHOT, REC_GRENADE = 0, 1, 2, 3, 4, 5

# XInput button bits (XINPUT_GAMEPAD_*)
XI = dict(DPAD_UP=0x0001, DPAD_DOWN=0x0002, DPAD_LEFT=0x0004, DPAD_RIGHT=0x0008, START=0x0010, BACK=0x0020,
          LS=0x0040, RS=0x0080, LB=0x0100, RB=0x0200, A=0x1000, B=0x2000, X=0x4000, Y=0x8000)


class Layout:
    """A fixed little-endian struct: [(name, fmt)] where fmt is a struct code, optionally with a count."""

    def __init__(self, name, size, fields):
        self.name, self.size, self.fields = name, size, []
        off = 0
        for fname, fmt in fields:
            n = struct.calcsize("<" + fmt)
            self.fields.append((fname, fmt, off))
            off += n
        self.used = off
        assert off <= size, (name, hex(off))

    def offset(self, fname):
        return next(o for n, _, o in self.fields if n == fname)

    def unpack(self, buf):
        out = {}
        for fname, fmt, off in self.fields:
            v = struct.unpack_from("<" + fmt, buf, off)
            if fmt.endswith("s"):
                v = v[0].split(b"\0", 1)[0].decode("latin-1")
            elif len(v) == 1:
                v = v[0]
            out[fname] = v
        return out

    def pack_into(self, buf, base, values):
        for fname, fmt, off in self.fields:
            if fname not in values:
                continue
            v = values[fname]
            if fmt.endswith("s"):
                v = v.encode("latin-1") if isinstance(v, str) else v
                struct.pack_into("<" + fmt, buf, base + off, v)
            elif isinstance(v, (tuple, list)):
                struct.pack_into("<" + fmt, buf, base + off, *v)
            else:
                struct.pack_into("<" + fmt, buf, base + off, v)


HEADER = Layout("Header", 0x20, [("magic", "I"), ("version", "I"), ("hostPid", "I"), ("guestPid", "I"),
                                 ("hostBeatMs", "Q"), ("guestBeatMs", "Q")])
HOST = Layout("HostState", 0x100, [
    ("seq", "I"), ("flags", "I"), ("frame", "Q"), ("deltaTime", "f"), ("teleportSeq", "I"), ("teleportPos", "3d"),
    ("teleportYaw", "f"), ("worldId", "I"), ("puppetPos", "3d"), ("puppetYaw", "f"), ("collisionEpoch", "I")])
PAD = Layout("PadState", 0x100, [
    ("seq", "I"), ("flags", "I"), ("packet", "I"), ("buttons", "H"), ("leftTrigger", "B"), ("rightTrigger", "B"),
    ("thumbLX", "h"), ("thumbLY", "h"), ("thumbRX", "h"), ("thumbRY", "h")])
GUEST = Layout("GuestState", 0x200, [
    ("seq", "I"), ("flags", "I"), ("frame", "Q"), ("qpc", "q"), ("origin", "3d"), ("velocity", "3f"),
    ("viewAngles", "3f"), ("viewHeight", "f"), ("pmFlags", "I"), ("teleportAck", "I"), ("cmdButtons", "I"),
    ("forwardMove", "b"), ("rightMove", "b"), ("reserved0", "H"), ("pmoveTraces", "I"), ("mapName", "64s"),
    ("tilesHeld", "I"), ("boxesHeld", "I"), ("fieldTraces", "I"), ("holdTraces", "I"), ("collisionEpoch", "I")])
TILE_HEADER = Layout("TileHeader", 0x28, [("tx", "i"), ("ty", "i"), ("epoch", "I"), ("count", "I"), ("bounds", "6f")])
SHOT = Layout("ShotEvent", 0x20, [("shotId", "I"), ("pellet", "H"), ("reserved", "H"), ("start", "3f"), ("end", "3f")])
LAYOUTS = (HEADER, HOST, PAD, GUEST, TILE_HEADER, SHOT)

_k32 = ctypes.windll.kernel32
_k32.GetTickCount64.restype = ctypes.c_uint64


def now_ms():
    return _k32.GetTickCount64()


class Link:
    """Opens (or creates) the shared mapping."""

    def __init__(self):
        self.mm = mmap.mmap(-1, MAPPING_BYTES, tagname=MAPPING_NAME, access=mmap.ACCESS_WRITE)
        h = HEADER.unpack(self.mm[OFF_HEADER:OFF_HEADER + HEADER.size])
        if h["magic"] == 0:
            HEADER.pack_into(self.mm, OFF_HEADER, {"version": VERSION})
            HEADER.pack_into(self.mm, OFF_HEADER, {"magic": MAGIC})
        h = HEADER.unpack(self.mm[OFF_HEADER:OFF_HEADER + HEADER.size])
        if h["magic"] != MAGIC or h["version"] != VERSION:
            raise RuntimeError("shared memory holds protocol %#x v%d, expected v%d" % (h["magic"], h["version"], VERSION))
        self._seq = {}

    def close(self):
        self.mm.close()

    # ---- header ----
    def header(self):
        return HEADER.unpack(self.mm[OFF_HEADER:OFF_HEADER + HEADER.size])

    def set_header(self, **values):
        HEADER.pack_into(self.mm, OFF_HEADER, values)

    def beat(self, side):
        """side: 'host' or 'guest'."""
        HEADER.pack_into(self.mm, OFF_HEADER, {side + "BeatMs": now_ms()})

    def alive(self, side):
        b = self.header()[side + "BeatMs"]
        return b != 0 and now_ms() - b < HEARTBEAT_TIMEOUT_MS

    # ---- seqlock slots ----
    def read_slot(self, off, layout, tries=8):
        for _ in range(tries):
            s1 = struct.unpack_from("<I", self.mm, off)[0]
            if s1 & 1:
                continue
            buf = self.mm[off:off + layout.size]
            if struct.unpack_from("<I", self.mm, off)[0] == s1:
                return layout.unpack(buf)
        return None

    def write_slot(self, off, layout, values):
        """Updates the named fields (others keep their value) under the seqlock."""
        s = struct.unpack_from("<I", self.mm, off)[0]
        struct.pack_into("<I", self.mm, off, s | 1)
        layout.pack_into(self.mm, off, {k: v for k, v in values.items() if k != "seq"})
        struct.pack_into("<I", self.mm, off, (s | 1) + 1)

    # ---- SPSC rings ----
    def ring_write(self, off, total, rtype, payload):
        cap = total - 0x80
        need = (8 + len(payload) + 7) & ~7
        head = struct.unpack_from("<Q", self.mm, off)[0]
        tail = struct.unpack_from("<Q", self.mm, off + 0x40)[0]
        pos = head % cap
        to_end = cap - pos
        pad = to_end if need > to_end else 0
        if head + pad + need - tail > cap:
            return False
        data = off + 0x80
        if pad:
            if pad >= 8:
                struct.pack_into("<II", self.mm, data + pos, REC_PAD, pad - 8)
            head += pad
            pos = 0
        struct.pack_into("<II", self.mm, data + pos, rtype, len(payload))
        self.mm[data + pos + 8:data + pos + 8 + len(payload)] = payload
        struct.pack_into("<Q", self.mm, off, head + need)
        return True

    def ring_drain(self, off, total, limit=1 << 30):
        cap = total - 0x80
        data = off + 0x80
        out = []
        tail = struct.unpack_from("<Q", self.mm, off + 0x40)[0]
        head = struct.unpack_from("<Q", self.mm, off)[0]
        while tail < head and len(out) < limit:
            pos = tail % cap
            if cap - pos < 8:
                tail += cap - pos
                continue
            rtype, n = struct.unpack_from("<II", self.mm, data + pos)
            if rtype != REC_PAD:
                out.append((rtype, bytes(self.mm[data + pos + 8:data + pos + 8 + n])))
            tail += (8 + n + 7) & ~7
            struct.pack_into("<Q", self.mm, off + 0x40, tail)
        return out

    def send_tile(self, tx, ty, epoch, bounds, boxes):
        """boxes: flat list/array of floats, 6 per box (min xyz, max xyz), guest space."""
        n = len(boxes) // 6
        payload = struct.pack("<iiII6f", tx, ty, epoch, n, *bounds) + struct.pack("<%df" % (6 * n), *boxes)
        return self.ring_write(OFF_COLLISION_RING, COLLISION_RING_BYTES, REC_TILE, payload)

    def send_clear_all(self):
        return self.ring_write(OFF_COLLISION_RING, COLLISION_RING_BYTES, REC_CLEAR_ALL, b"")

    def send_shot(self, shot_id, pellet, start, end):
        """(fake guest) one bullet into the event ring."""
        return self.ring_write(OFF_EVENT_RING, EVENT_RING_BYTES, REC_SHOT, struct.pack("<IHH3f3f", shot_id, pellet, 0, *start, *end))

    def drain_events(self):
        """(host) [(type, payload dict or None)] from the guest's event ring."""
        out = []
        for rtype, payload in self.ring_drain(OFF_EVENT_RING, EVENT_RING_BYTES):
            out.append((rtype, SHOT.unpack(payload) if rtype == REC_SHOT else None))
        return out

    def send_command(self, line):
        return self.ring_write(OFF_CMD_RING, CMD_RING_BYTES, REC_COMMAND, line.encode("ascii"))


def check_layout(cpp_lines):
    """cpp_lines: 'Struct.field offset' lines from layout_test.exe. Returns a list of mismatches."""
    bad = []
    known = {(l.name, n): o for l in LAYOUTS for n, _, o in l.fields}
    sizes = {l.name: l.size for l in LAYOUTS}
    for line in cpp_lines:
        parts = line.split()
        if len(parts) != 2:
            continue
        key, val = parts[0], int(parts[1], 0)
        if key.endswith(".sizeof"):
            if sizes.get(key[:-7]) != val:
                bad.append("%s: C++ %#x, Python %#x" % (key, val, sizes.get(key[:-7], -1)))
        else:
            s, f = key.split(".")
            if known.get((s, f)) != val:
                bad.append("%s: C++ %#x, Python %s" % (key, val, hex(known[(s, f)]) if (s, f) in known else "missing"))
    return bad


if __name__ == "__main__":
    import subprocess
    import sys
    import os
    here = os.path.dirname(os.path.abspath(__file__))
    ok = True
    for exe in ("layout_test_x86.exe", "layout_test_x64.exe"):
        path = os.path.join(here, "..", "bin", exe)
        out = subprocess.run([path], capture_output=True, text=True, check=True).stdout.splitlines()
        bad = check_layout(out)
        print("%s: %d fields checked, %s" % (exe, len(out), "OK" if not bad else "MISMATCH"))
        for b in bad:
            print("   ", b)
        ok &= not bad
    sys.exit(0 if ok else 1)
