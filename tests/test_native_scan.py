"""The native scanner (bin/bw2_scan_x64.dll, src/bl_scan/scan.cpp) must produce the same boxes as the
Python reference (sdk_mods/bw2/scan.py) on the synthetic world of tests/test_scan.py.
  python tests/test_native_scan.py
"""
import os
import shutil
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import test_scan  # noqa: E402  (synthetic world + tracer)

# a throwaway package holding just scan.py + nativescan.py (bw2/__init__ needs the BL2 SDK)
tmp = tempfile.mkdtemp(prefix="bw2ns_")
pkg = os.path.join(tmp, "bw2ns")
os.makedirs(pkg)
for name in ("scan.py", "nativescan.py"):
    shutil.copy(os.path.join(ROOT, "sdk_mods", "bw2", name), pkg)
open(os.path.join(pkg, "__init__.py"), "w").close()
sys.path.insert(0, tmp)
from bw2ns import nativescan, scan  # noqa: E402


def main():
    dll = nativescan.load(os.path.join(ROOT, "bin", "bw2_scan_x64.dll"))
    assert dll, "build bin/bw2_scan_x64.dll first (build.bat)"
    worst, total, tp, tn, t_py, t_c = 0.0, 0, 0, 0, 0.0, 0.0
    for tx in (-1, 0, 1):
        for ty in (-1, 0):
            t0 = time.perf_counter()
            ref = scan.TileScan(tx, ty, 0.0, test_scan.trace)
            for _ in ref.run():
                pass
            t1 = time.perf_counter()
            nat = nativescan.NativeTileScan(dll, tx, ty, 0.0, trace=test_scan.trace)
            for _ in nat.run():
                pass
            t2 = time.perf_counter()
            t_py, t_c = t_py + t1 - t0, t_c + t2 - t1
            assert len(ref.boxes) == len(nat.boxes), "tile %d,%d: %d python boxes vs %d native" % (tx, ty, len(ref.boxes), len(nat.boxes))
            for a, b in zip(ref.boxes, nat.boxes):
                d = max(abs(x - y) for x, y in zip(a, b))
                worst = max(worst, d)
                assert d < 0.01, "tile %d,%d box differs: %r vs %r" % (tx, ty, a, b)
            total += len(ref.boxes)
            tp += ref.traces
            tn += nat.traces
    print("native == python: %d boxes, worst difference %.5f; traces %d vs %d; time %.2f s vs %.2f s (callback tracer)" % (
        total, worst, tp, tn, t_py, t_c))
    assert tp == tn, "trace counts differ"
    print("test_native_scan OK")


if __name__ == "__main__":
    try:
        main()
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
