#!/usr/bin/env python3
"""UNOFFICIAL 2026-11-15 SSS v2 (docs/next/FD-SKIN-RESULTS.md "SSS v2"): the offline check of the Burley profile and its sampled disc.

Christensen & Burley 2015 (normalized diffusion): R(r) = (e^(-r/d) + e^(-r/3d)) / (8 pi d r), CDF(r) = 1 - e^(-r/d)/4 - 3 e^(-r/3d)/4.
1. Energy: the integral of R over the plane (2 pi r dr) is 1 for each colour's d (numerically, to r = 60 d).
2. The CDF the taps are drawn from is R's own (its derivative against 2 pi r R).
3. The disc (core/DeferredSkin.h SssTaps + Shaders/DeferredSkinSSS.hlsl, written out here): 24 radii from red's distribution at
   u = (i + 0.5) / 24, angles i x the golden angle (+ the frame's turn), weights R_c / R_red, normalised by their sum. Against the exact
   convolution of R_c with test pictures of the light: a radial bump, and a hard edge (lit half-plane) at several distances from the pixel:
   one frame (still, no TAA) and the average over 64 turned frames (what TAA / FSR / DLSS resolve).
Usage: py -3.11 tests/fd_skin_sss_math.py   (exit 1 if the energy is off by more than 1e-3 or the 64-frame edge error passes 0.03)
"""
import sys
import numpy as np

D_MM = np.array([3.5, 0.9, 0.32])	# [Shaders.Skin.SSS] ScatterRed / Green / Blue (mm), Radius 1
N = 24
GOLDEN = 2.39996323

def R(r, d): return (np.exp(-r / d) + np.exp(-r / (3 * d))) / (8 * np.pi * d * np.maximum(r, 1e-12))
def CDF(r, d): return 1 - 0.25 * np.exp(-r / d) - 0.75 * np.exp(-r / (3 * d))

def taps(d):	# DeferredSkin::SssTaps
    out = []
    for i in range(N):
        u = (i + 0.5) / N
        lo, hi = 0.0, 64.0
        for _ in range(60):
            x = 0.5 * (lo + hi)
            if 1 - 0.25 * np.exp(-x) - 0.75 * np.exp(-x / 3) < u: lo = x
            else: hi = x
        x = 0.5 * (lo + hi); r = x * d[0]
        red = (np.exp(-r / d[0]) + np.exp(-r / (3 * d[0]))) / d[0]
        out.append([x] + [((np.exp(-r / dc) + np.exp(-r / (3 * dc))) / dc) / red for dc in d])
    return np.array(out)

def disc(f, d, angle):	# the shader's estimate of (R_c * f) at the origin for one frame's turn
    t = taps(d)
    r = t[:, 0] * d[0]; a = angle + GOLDEN * np.arange(N)
    v = f(r * np.cos(a), r * np.sin(a))
    w = t[:, 1:]
    return (w * v[:, None]).sum(0) / w.sum(0)

def exact(f, d, rmax_d=40.0, nr=4000, na=720):	# the convolution by quadrature (polar, r^1/2-spaced radii for the 1/r peak)
    out = []
    for dc in d:
        s = np.linspace(0, np.sqrt(rmax_d * dc), nr + 1); rr = 0.5 * (s[1:] ** 2 + s[:-1] ** 2); dr = s[1:] ** 2 - s[:-1] ** 2
        aa = (np.arange(na) + 0.5) * 2 * np.pi / na
        X = rr[:, None] * np.cos(aa)[None]; Y = rr[:, None] * np.sin(aa)[None]
        val = f(X, Y).mean(1)	# angular average
        out.append(np.sum(val * R(rr, dc) * 2 * np.pi * rr * dr) / CDF(rmax_d * dc, dc))
    return np.array(out)

def main():
    d = D_MM / 14.2857	# world units
    ok = True
    print("1. energy, the integral of R over the plane (to 60 d), per colour (d = %.3f %.3f %.3f mm):" % tuple(D_MM))
    for c, dc in zip("RGB", D_MM):
        s = np.linspace(0, np.sqrt(60 * dc), 200001); rr = 0.5 * (s[1:] ** 2 + s[:-1] ** 2); dr = s[1:] ** 2 - s[:-1] ** 2
        e = np.sum(R(rr, dc) * 2 * np.pi * rr * dr)
        ok &= abs(e - 1) < 1e-3 + np.exp(-20)
        print("   %s: %.6f (the tail past 60 d: %.2e)" % (c, e, 0.75 * np.exp(-20)))
    print("2. the CDF against R: max |dCDF/dr - 2 pi r R| over r in (0, 30 d]:", end=" ")
    rr = np.linspace(1e-4, 30, 100000); dd = 1.0
    print("%.2e" % np.max(np.abs(np.gradient(CDF(rr, dd), rr) - 2 * np.pi * rr * R(rr, dd))))
    t = taps(d)
    print("3. the 24 taps: radii %.3f .. %.3f x d_red (CDF %.3f .. %.3f); weights at the first tap R %.2f G %.2f B %.2f, at the last %.2e %.2e %.2e" %
          (t[0, 0], t[-1, 0], CDF(t[0, 0], 1), CDF(t[-1, 0], 1), t[0, 1], t[0, 2], t[0, 3], t[-1, 1], t[-1, 2], t[-1, 3]))
    sig = d[0] * 3
    bump = lambda X, Y: np.exp(-(X * X + Y * Y) / (2 * sig * sig))
    ex = exact(bump, d); est = disc(bump, d, 0.0)
    print("   radial bump (sigma 3 d_red): exact %s  disc %s  error %s" % (np.round(ex, 4), np.round(est, 4), np.round(est - ex, 4)))
    worst = 0.0
    for off in (0.25, 0.5, 1.0, 2.0, 4.0):
        a = off * d[0]
        edge = lambda X, Y, a=a: (X > a).astype(float)
        ex = exact(edge, d)
        one = disc(edge, d, 0.0)
        avg = np.mean([disc(edge, d, ang) for ang in 2 * np.pi * ((0.61803399 * np.arange(64)) % 1.0)], axis=0)
        worst = max(worst, np.max(np.abs(avg - ex)))
        print("   edge at %.2f d_red: exact %s  one frame %s  64 turned frames %s" % (off, np.round(ex, 4), np.round(one, 4), np.round(avg, 4)))
    ok &= worst <= 0.03
    print("   worst 64-frame error on the edges: %.4f (limit 0.03)" % worst)
    print("FD SKIN SSS MATH: %s" % ("PASS" if ok else "FAIL"))
    sys.exit(0 if ok else 1)

if __name__ == "__main__": main()
