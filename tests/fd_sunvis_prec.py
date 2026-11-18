#!/usr/bin/env python3
"""UNOFFICIAL 2026-11-16 fd-sunvis (docs/next/FD-SUN-RESULTS.md section 4.1): the COLOR3.a sun-visibility code of
src/hlsl/NewVegas/Shaders/Includes/DeferredPixel.hlsl (FDSunVisCode / FDSunVisDecode, keep in step) through an fp16 target.
Checks that every code is on fp16's grid, the decode error, and that the code keeps the other readers' tests (written > 0.99,
actor > 1.5, below the forward skin's 2.02). Usage: py -3 tests/fd_sunvis_prec.py   (exit 1 on a failure)"""
import math, struct, sys

def half(x): return struct.unpack("<e", struct.pack("<e", x))[0]
def sat(x): return min(max(x, 0.0), 1.0)

def code(a, vis, f0, mark):
    v = math.floor((1 - sat(vis)) * 47 + 0.5)
    s = 0 if f0 < 0 else 1 + math.floor(sat(f0 * 12.5) * 8 + 0.5)
    q = 0 if (mark > 1.5 or mark < -0.5) else 48 * s + v
    return a + q / 1024 if abs(a - 1) < 0.01 else a - q / 1024 if abs(a - 2) < 0.01 else a

def decode(a):
    q = math.floor(abs(a - (2 if a > 1.5 else 1)) * 1024 + 0.5) if 0.99 < a < 2.005 else 0
    s = math.floor((q + 0.5) / 48)
    return 1 - (q - 48 * s) / 47, ((s - 1) * 0.01 if s > 0.5 else 0.04)

off_grid = bad = 0; max_v = max_f = 0.0; lo = {1: 9.0, 2: 9.0}; hi = {1: 0.0, 2: 0.0}
for a in (1.0, 2.0):
    for vi in range(1001):
        vis = vi / 1000
        for f0 in [-1.0] + [i / 1000 for i in range(121)]:
            c = code(a, vis, f0, 1.0); h = half(c)
            off_grid += h != c
            lo[a] = min(lo[a], h); hi[a] = max(hi[a], h)
            dv, df = decode(h)
            max_v = max(max_v, abs(dv - vis)); max_f = max(max_f, abs(df - (0.04 if f0 < 0 else min(f0, 0.08))))
            bad += not (h > 0.99 and (h > 1.5) == (a > 1.5) and h < 2.02)
print("codes off fp16's grid: %d | readers' tests broken: %d | vis max error %.4f (half a level: %.4f) | F0 max error %.4f"
      % (off_grid, bad, max_v, 0.5 / 47, max_f))
print("world codes %.6f .. %.6f, gear codes %.6f .. %.6f" % (lo[1], hi[1], lo[2], hi[2]))
neutral = [code(1.0, 0.3, 0.05, 3.0), code(2.0, 0.3, 0.05, -1.0), code(0.0, 0.3, 0.05, 1.0), code(2.2, 0.3, 0.05, 1.0)]
print("blended 1 / 2, unwritten 0, skin 2.2 written as:", neutral, "| decode of 1, 2, 2.25, 4.5:", [decode(x) for x in (1.0, 2.0, 2.25, 4.5)])
ok = off_grid == 0 and bad == 0 and max_v <= 0.5 / 47 + 1e-9 and max_f <= 0.005 + 1e-9 and neutral == [1.0, 2.0, 0.0, 2.2]
print("FD SUNVIS PREC: PASS" if ok else "FD SUNVIS PREC: FAIL")
sys.exit(0 if ok else 1)
