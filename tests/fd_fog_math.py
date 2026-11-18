#!/usr/bin/env python3
"""UNOFFICIAL 2026-11-14 the deferred passes' stored-curve fog (Shaders/Includes/DeferredPixel.hlsl FDAddFogged; docs/next/FD-SUN-RESULTS.md
section 9). The object / parallax / land templates fog in the STORED curve: forward writes dec(lerp(enc(A + D), F, f)) into the linear frame,
a deferred record dec(lerp(enc(A), F, f)). This compares, against forward, the plan's plain identity (frame + D x (1 - f)) and the helper
(the frame read back, enc(A) rebuilt, the forward lerp redone), with what the GPU really holds: the frame and its copy in fp16 (A16B16G16R16F),
the keep decoded from COLOR1.a's fp16 code (base 3 / 5 without GBufferV2, 1 + keep with it), and a chain (the sun pass, then the lamp pass on
its result). Errors in 1/255 of the displayed (encoded) value, the way the parity test counts them. numpy only; prints a table.
Usage: py -3.11 -I tests/fd_fog_math.py
"""
import numpy as np

rng = np.random.default_rng(20261114)
N = 400000
G = 2.2
f16 = lambda x: np.asarray(x, np.float32).astype(np.float16).astype(np.float64)
enc = lambda x: np.power(np.maximum(x, 0.0), 1.0 / G)
dec = lambda x: np.power(np.maximum(x, 0.0), G)
lerp = lambda a, b, t: a + (b - a) * t
u = lambda lo, hi, shape=(N, 1): rng.uniform(lo, hi, shape)

def helper(frame, D, keep, F):
    """FDAddFogged (frameLinear 1), as written in DeferredPixel.hlsl; returns what the pass adds."""
    out = D * keep
    on = keep < 0.9999
    v = enc(frame)
    f = 1.0 - keep
    encA = np.maximum(v - f * F, 0.0) / np.maximum(keep, 1e-4)
    v2 = f * F + keep * enc(dec(encA) + np.maximum(D, 0.0))
    exact = dec(v2) - np.maximum(frame, 0.0)
    return np.where(on, exact, out)

A = u(0.0, 0.6, (N, 3)) * u(0.2, 1.0)                     # the record's light (ambient, glow) on its albedo, linear
D1 = u(0.0, 1.5, (N, 3)) * np.where(u(0, 1) < 0.25, 0.0, 1.0)   # the sun (none on a quarter: shadow)
D2 = u(0.0, 0.8, (N, 3)) * np.where(u(0, 1) < 0.5, 0.0, 1.0)    # the lamps
F = np.clip(u(0.2, 0.8) * np.array([0.85, 0.95, 1.1]) * u(0.9, 1.1, (N, 3)), 0, 1)   # the weather's fog colour (stored curve)
fog = np.where(u(0, 1) < 0.2, 0.0, u(0.0, 0.95))

def keep_code(keep, layout):
    if layout == "v1": return f16(3.0 + keep) - 3.0       # base 3 (PBRSun); base 5 rounds twice as coarse, below
    if layout == "v1 base 5": return f16(5.0 + keep) - 5.0
    return f16(1.0 + keep) - 1.0                          # GBufferV2

fwd1 = enc(A + D1) * (1 - fog) + F * fog                  # forward, sun only (stored)
fwd2 = enc(A + D1 + D2) * (1 - fog) + F * fog             # forward, sun + lamps
rec = f16(dec(enc(A) * (1 - fog) + F * fog))              # what the lighting-free record leaves in the linear fp16 frame

def err(out_lin, ref_stored):
    return np.abs(enc(out_lin) - ref_stored).max(axis=1) * 255.0

bands = (("no fog", fog[:, 0] == 0.0), ("fog 0-0.3", (fog[:, 0] > 0) & (fog[:, 0] <= 0.3)), ("fog 0.3-0.6", (fog[:, 0] > 0.3) & (fog[:, 0] <= 0.6)),
         ("fog 0.6-0.95", fog[:, 0] > 0.6))
print("%-12s %-10s %-34s %10s %10s %10s %12s" % ("layout", "band", "method", "mean/255", "p99/255", "max/255", "> 1/255 share"))
for layout in ("v1", "v1 base 5", "v2"):
    keep = keep_code(1.0 - fog, layout)
    plain1 = f16(rec + D1 * keep)
    exact1 = f16(rec + helper(rec, D1, keep, F))
    plain2 = f16(plain1 + D2 * keep)
    exact2 = f16(exact1 + helper(f16(exact1), D2, keep, F))   # the lamp pass reads the copy of the sun pass's result
    for name, sel in bands:
        for meth, out, ref in (("plain x (1 - f), sun", plain1, fwd1), ("FDAddFogged, sun", exact1, fwd1),
                               ("plain, sun then lamps", plain2, fwd2), ("FDAddFogged, sun then lamps", exact2, fwd2)):
            e = err(out[sel], ref[sel])
            print("%-12s %-10s %-34s %10.3f %10.3f %10.3f %12.5f" % (layout, name, meth, e.mean(), np.percentile(e, 99), e.max(), (e > 1.0).mean()))
