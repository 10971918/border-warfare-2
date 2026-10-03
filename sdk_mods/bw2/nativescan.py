"""Native backend for scan.TileScan (bw2_scan.dll, src/bl_scan/scan.cpp): the same algorithm, ~10x faster
in BL2 because the traces don't go through Python.

NativeTileScan has the TileScan interface the streamer uses: run() (generator), done, traces, boxes, bounds().
In BL2 it traces through Actor.Trace via the SDK's process_event (Python must keep Trace's iNative cleared
while run() steps, which the streamer's Tracer context does). Tests pass a Python trace callback instead.
"""

from __future__ import annotations

import ctypes
import os
import struct
from collections.abc import Iterator
from typing import Any

from . import scan

HERE = os.path.dirname(os.path.abspath(__file__))
TRACE_CB = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.POINTER(ctypes.c_float), ctypes.POINTER(ctypes.c_float),
                            ctypes.POINTER(ctypes.c_float), ctypes.POINTER(ctypes.c_float))
SLICE_MS = 0.5  # each run() step scans about this long natively


def load(path: str | None = None) -> ctypes.CDLL | None:
    """The DLL next to this file (bw2_scan.dll, matching the Python's bitness), or None."""
    if path is None:
        name = "bw2_scan.dll" if struct.calcsize("P") == 4 else "bw2_scan_x64.dll"
        path = os.path.join(HERE, name)
    if not os.path.exists(path):
        return None
    dll = ctypes.CDLL(path)
    dll.bw2_scan_new.restype = ctypes.c_void_p
    dll.bw2_scan_new.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_double]
    dll.bw2_scan_set_callback.argtypes = [ctypes.c_void_p, TRACE_CB, ctypes.c_void_p]
    dll.bw2_scan_set_unreal.restype = ctypes.c_int
    dll.bw2_scan_set_unreal.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p] + [ctypes.c_int] * 6
    dll.bw2_scan_run.restype = ctypes.c_int
    dll.bw2_scan_run.argtypes = [ctypes.c_void_p, ctypes.c_double]
    dll.bw2_scan_traces.restype = ctypes.c_int
    dll.bw2_scan_traces.argtypes = [ctypes.c_void_p]
    dll.bw2_scan_boxes.restype = ctypes.c_int
    dll.bw2_scan_boxes.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_float), ctypes.c_int]
    dll.bw2_scan_free.argtypes = [ctypes.c_void_p]
    return dll


class TraceParams:
    """Actor.Trace's parameter layout, read once from the UFunction."""

    def __init__(self, func: Any) -> None:
        off = {p.Name: p.Offset_Internal for p in func._properties()}
        self.func = func._get_address()
        self.size = func.ParamsSize
        self.hit_loc, self.hit_norm = off["HitLocation"], off["HitNormal"]
        self.end, self.start, self.ret = off["TraceEnd"], off["TraceStart"], off["ReturnValue"]


class NativeTileScan:
    def __init__(self, dll: ctypes.CDLL, tx: int, ty: int, zref: float, *, actor: Any = None, params: TraceParams | None = None,
                 trace: scan.Trace | None = None) -> None:
        self.dll = dll
        self.tx, self.ty = tx, ty
        self.x0, self.y0 = tx * scan.TILE, ty * scan.TILE
        self.zlo, self.zhi = zref - scan.Z_HALF, zref + scan.Z_HALF
        self.done = False
        self.boxes: list[tuple] = []
        self.h = dll.bw2_scan_new(tx, ty, zref)
        self._cb = None
        if trace is not None:
            def cb(_user, s, e, hit, n):  # noqa: ANN001
                r = trace((s[0], s[1], s[2]), (e[0], e[1], e[2]))
                if r is None:
                    return 0
                hit[0], hit[1], hit[2] = r[0]
                n[0], n[1], n[2] = r[1]
                return 1
            self._cb = TRACE_CB(cb)
            dll.bw2_scan_set_callback(self.h, self._cb, None)
        else:
            ok = dll.bw2_scan_set_unreal(self.h, actor._get_address(), params.func, params.size, params.hit_loc, params.hit_norm,
                                         params.end, params.start, params.ret)
            if not ok:
                raise RuntimeError("unrealsdk.dll has no _unrealsdk_export__process_event")

    @property
    def traces(self) -> int:
        return self.dll.bw2_scan_traces(self.h) if self.h else self._traces

    def bounds(self) -> tuple:
        return (self.x0, self.y0, self.zlo, self.x0 + scan.TILE, self.y0 + scan.TILE, self.zhi)

    def run(self) -> Iterator[None]:
        while not self.dll.bw2_scan_run(self.h, SLICE_MS):
            yield
        n = self.dll.bw2_scan_boxes(self.h, None, 0)
        buf = (ctypes.c_float * (6 * n))()
        self.dll.bw2_scan_boxes(self.h, buf, n)
        self.boxes = [tuple(buf[6 * k:6 * k + 6]) for k in range(n)]
        self._traces = self.dll.bw2_scan_traces(self.h)
        self.dll.bw2_scan_free(self.h)
        self.h = None
        self.done = True

    def __del__(self) -> None:
        if getattr(self, "h", None):
            self.dll.bw2_scan_free(self.h)
            self.h = None
