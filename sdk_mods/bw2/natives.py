"""Calling numbered UE3 natives from Python.

UE3's ProcessEvent silently skips functions that have a native index (`native(277) function Trace`),
so the SDK gets default return values back (found in Phase 0b). Clearing UFunction::iNative for the
duration of the call makes ProcessEvent run them. Everything runs on the game thread, so nothing
else sees the cleared index.
"""

from __future__ import annotations

import ctypes
from contextlib import contextmanager
from typing import Any

import unrealsdk

INATIVE_OFFSET = 0x90  # u16 UFunction::iNative in BL2 (Phase 0b: Trace=277, FastTrace=548)
_verified: bool | None = None


def _inative(func: Any) -> ctypes.c_uint16:
    return ctypes.c_uint16.from_address(func._get_address() + INATIVE_OFFSET)


def verify() -> bool:
    """True if the offset is right for this game (Actor.Trace must read 277)."""
    global _verified
    if _verified is None:
        _verified = _inative(unrealsdk.find_object("Function", "Engine.Actor:Trace")).value == 277
    return _verified


@contextmanager
def unlocked(bound: Any):
    """Inside the block, `bound` (and any call of the same UFunction) runs although it is numbered."""
    if not verify():
        raise RuntimeError("UFunction::iNative offset not verified for this build")
    n = _inative(bound.func)
    old = n.value
    n.value = 0
    try:
        yield bound
    finally:
        n.value = old


def call(bound: Any, *args: Any, **kwargs: Any) -> Any:
    """Call a bound UFunction, temporarily clearing its native index if it has one."""
    with unlocked(bound):
        return bound(*args, **kwargs)


def vec(x: float, y: float, z: float) -> Any:
    return unrealsdk.make_struct("Vector", X=x, Y=y, Z=z)


class Tracer:
    """Fast repeated world traces (no pawns) from one actor. Use inside `with tracer:`.
    tracer(start, end) -> None or ((x, y, z), (nx, ny, nz)) - the scan.Trace signature."""

    def __init__(self, actor: Any) -> None:
        self.fn = actor.Trace
        self.zero = vec(0, 0, 0)
        self._ctx = None

    def __enter__(self) -> "Tracer":
        self._ctx = unlocked(self.fn)
        self._ctx.__enter__()
        return self

    def __exit__(self, *exc: Any) -> None:
        self._ctx.__exit__(*exc)

    def __call__(self, start: tuple, end: tuple):
        r = self.fn(self.zero, self.zero, vec(*end), vec(*start), False)
        if r[0] is None:
            return None
        return (r[1].X, r[1].Y, r[1].Z), (r[2].X, r[2].Y, r[2].Z)


def trace(actor: Any, start: tuple, end: tuple) -> tuple[Any, tuple | None, tuple | None]:
    """Single world trace. Returns (hit actor or None, hit location, hit normal)."""
    with unlocked(actor.Trace):
        r = actor.Trace(vec(0, 0, 0), vec(0, 0, 0), vec(*end), vec(*start), False)
    if r[0] is None:
        return None, None, None
    return r[0], (r[1].X, r[1].Y, r[1].Z), (r[2].X, r[2].Y, r[2].Z)
