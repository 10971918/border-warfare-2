"""Border Warfare 2: scanning Borderlands 2's collision into boxes (M2, SkyCraft-style "Stage A").

BL2 is scanned in square tiles of TILE_CELLS x TILE_CELLS columns, CELL units wide, with BL2's own
traces. Each column becomes vertical solid spans; spans become axis-aligned boxes, which the MW2 guest
answers Pmove's traces against. Pure Python with an injected trace function, so it runs offline in tests:

    trace(start, end) -> None, or (hit_location (x, y, z), hit_normal (x, y, z))

Per column:
  * down multi-hit from the top of the tile's height range: every surface facing up is the top of a span
  * for each top except the lowest, an up trace from the next surface below finds the underside
    (floors, bridges, ceilings); the lowest span reaches down to the bottom of the range
  * a column with no hits at all is solid (inside rock, or a pit nobody should fall into)
Walls that start above the scan range or have no visible top are caught by short horizontal probes from
each column at knee and chest height; the cell behind a wall face becomes solid over the player's height.

Tile scans are generators (one trace per step) so the caller can spend a fixed time budget per frame.
"""

from __future__ import annotations

from collections.abc import Callable, Iterator

CELL = 32.0            # BL2 units per column (~12.6 MW2 inches; the MW2 player is 30 in wide)
TILE_CELLS = 24        # columns per tile side -> 768 uu tiles
TILE = CELL * TILE_CELLS
Z_HALF = 1200.0        # scanned height range: reference +- this
MAX_LAYERS = 4         # surfaces per column
THIN_FLOOR = 24.0      # a floor whose underside can't be traced is this thick
PROBE_HEIGHTS = (55.0, 100.0)    # above a column's top: knee (2 directions) and chest (4 directions)
WALL_BAND = (-20.0, 190.0)       # solid added behind a wall face, relative to the probing column's top
STEP = 3.0             # nudge past a surface before tracing on
INSIDE_EPS = 0.6       # a hit this close to the trace start means the start is inside a solid
EXIT_STEP = 64.0       # probe step down through solids: below any gap a player fits through (prone ~71 uu)

Trace = Callable[[tuple, tuple], "tuple | None"]


def tile_of(x: float, y: float) -> tuple[int, int]:
    return int(x // TILE), int(y // TILE)


class TileScan:
    """One tile. `run()` is a generator yielding after each trace; `boxes` holds the result when done."""

    def __init__(self, tx: int, ty: int, zref: float, trace: Trace) -> None:
        self.tx, self.ty = tx, ty
        self.x0, self.y0 = tx * TILE, ty * TILE
        self.zlo, self.zhi = zref - Z_HALF, zref + Z_HALF
        self.trace = trace
        self.traces = 0
        self.done = False
        self.spans: list[list[list[float]]] = [[] for _ in range(TILE_CELLS * TILE_CELLS)]
        self.tops: list[float | None] = [None] * (TILE_CELLS * TILE_CELLS)  # main walkable top per column
        self.boxes: list[tuple[float, float, float, float, float, float]] = []

    def bounds(self) -> tuple[float, float, float, float, float, float]:
        return (self.x0, self.y0, self.zlo, self.x0 + TILE, self.y0 + TILE, self.zhi)

    def _t(self, a: tuple, b: tuple):
        self.traces += 1
        return self.trace(a, b)

    def cell_center(self, i: int, j: int) -> tuple[float, float]:
        return self.x0 + (i + 0.5) * CELL, self.y0 + (j + 0.5) * CELL

    def run(self) -> Iterator[None]:
        n = TILE_CELLS
        for j in range(n):
            for i in range(n):
                yield from self._column(i, j)
        for j in range(n):
            for i in range(n):
                yield from self._walls(i, j)
        self._build_boxes()
        self.done = True

    # ---- columns ----
    def _inside(self, cx: float, cy: float, z: float) -> Iterator[None]:
        """Generator; its return value says whether (cx, cy, z) is inside a solid that reports hits at the
        trace start (UE3 convex/box/BSP collision does; plain triangle meshes don't)."""
        hit = self._t((cx, cy, z), (cx, cy, z - 1.0))
        yield
        return hit is not None and abs(hit[0][2] - z) < INSIDE_EPS

    def _exit_below(self, cx: float, cy: float, z: float) -> Iterator[None]:
        """Generator for a column point z inside a solid. Returns (bottom, free): the underside of the solid
        and a free point just below it, or (zlo, None) when it reaches the bottom of the range.
        Probes go down in fixed EXIT_STEP steps, smaller than any gap a player fits through, so no free
        space is skipped (doubling steps jumped over a player standing under an arch in Sanctuary); then an
        up trace from the first free probe hits the underside exactly (traces from outside see faces)."""
        probe = z
        while True:
            probe -= EXIT_STEP
            if probe <= self.zlo:
                return self.zlo, None
            if not (yield from self._inside(cx, cy, probe)):
                break
        up = self._t((cx, cy, probe), (cx, cy, probe + EXIT_STEP + 1.0))
        yield
        bottom = up[0][2] if up is not None else probe
        return bottom, probe

    def _column(self, i: int, j: int) -> Iterator[None]:
        cx, cy = self.cell_center(i, j)
        spans = self.spans[j * TILE_CELLS + i]
        pending: list[float] = []  # tops of one-sided surfaces whose underside is still unknown
        main_top: float | None = None
        z, layers = self.zhi, 0
        while layers < MAX_LAYERS and z > self.zlo:
            hit = self._t((cx, cy, z), (cx, cy, self.zlo))
            yield
            if hit is None:
                if layers == 0:  # nothing at all below free space: a pit or the edge of the world - solid
                    spans.append([self.zlo, z])
                break
            hz = hit[0][2]
            layers += 1
            if hz >= z - INSIDE_EPS:  # started inside a solid: it extends from here down to its underside
                bottom, free = yield from self._exit_below(cx, cy, z)
                spans.append([bottom, z])
                if free is None:
                    break
                z = free
                continue
            # a surface facing up at hz with free space above it
            if main_top is None:
                main_top = hz
            if (yield from self._inside(cx, cy, hz - STEP)):  # a closed solid: find its underside directly
                bottom, free = yield from self._exit_below(cx, cy, hz - STEP)
                spans.append([bottom, hz])
                if free is None:
                    break
                z = free
            else:  # a one-sided surface (terrain, plain mesh): the next surface below bounds it
                pending.append(hz)
                z = hz - STEP
        # undersides of one-sided surfaces: up from the surface below, or down to the bottom of the range
        for k, top in enumerate(pending):
            below = max([s[1] for s in spans if s[1] < top] + ([pending[k + 1]] if k + 1 < len(pending) else []), default=None)
            if below is None:
                spans.append([self.zlo, top])
                continue
            up = self._t((cx, cy, below + STEP), (cx, cy, top - STEP))
            yield
            bottom = up[0][2] if up is not None else top - THIN_FLOOR
            spans.append([max(bottom, below), top])
        self.tops[j * TILE_CELLS + i] = main_top

    # ---- walls ----
    def _walls(self, i: int, j: int) -> Iterator[None]:
        top = self.tops[j * TILE_CELLS + i]
        if top is None:
            return
        cx, cy = self.cell_center(i, j)
        for h, dirs in ((PROBE_HEIGHTS[0], ((1, 0), (0, 1))), (PROBE_HEIGHTS[1], ((1, 0), (0, 1), (-1, 0), (0, -1)))):
            z = top + h
            if z >= self.zhi:
                continue
            for dx, dy in dirs:
                hit = self._t((cx, cy, z), (cx + dx * CELL, cy + dy * CELL, z))
                yield
                if hit is None:
                    continue
                (hx, hy, _), (nx, ny, _) = hit
                # the cell just behind the face (the inside of the wall)
                bx, by = hx - nx * 4.0, hy - ny * 4.0
                ci, cj = int((bx - self.x0) // CELL), int((by - self.y0) // CELL)
                if 0 <= ci < TILE_CELLS and 0 <= cj < TILE_CELLS:
                    self.spans[cj * TILE_CELLS + ci].append([top + WALL_BAND[0], min(self.zhi, top + WALL_BAND[1])])

    # ---- boxes ----
    def _build_boxes(self) -> None:
        n = TILE_CELLS
        merged: list[tuple] = []
        for idx in range(n * n):
            spans = sorted(self.spans[idx])
            out: list[list[float]] = []
            for lo, hi in spans:
                lo, hi = round(max(lo, self.zlo)), round(min(hi, self.zhi))
                if hi <= lo:
                    continue
                if out and lo <= out[-1][1] + 1:
                    out[-1][1] = max(out[-1][1], hi)
                else:
                    out.append([lo, hi])
            merged.append(tuple(tuple(s) for s in out))
        # runs along x of identical span lists become one box per span
        for j in range(n):
            i = 0
            while i < n:
                key = merged[j * n + i]
                k = i + 1
                while k < n and merged[j * n + k] == key:
                    k += 1
                if key:
                    x0, x1 = self.x0 + i * CELL, self.x0 + k * CELL
                    y0, y1 = self.y0 + j * CELL, self.y0 + (j + 1) * CELL
                    for lo, hi in key:
                        self.boxes.append((x0, y0, float(lo), x1, y1, float(hi)))
                i = k


def to_guest(anchor, box: tuple) -> list[float]:
    """BL2 box -> guest box (min xyz, max xyz) through the coords Anchor (y is mirrored)."""
    a = anchor.to_mw((box[0], box[1], box[2]))
    b = anchor.to_mw((box[3], box[4], box[5]))
    return [min(a[0], b[0]), min(a[1], b[1]), min(a[2], b[2]), max(a[0], b[0]), max(a[1], b[1]), max(a[2], b[2])]
