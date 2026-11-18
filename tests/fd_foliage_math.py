#!/usr/bin/env python3
"""UNOFFICIAL 2026-11-14 full deferred renderer, phase 3 foliage (docs/next/FD-FOLIAGE-RESULTS.md): the numeric check of the foliage split.

The forward leaf and grass shaders (STLEAF200x.pso.hlsl + Includes/Leaf.hlsl, GRASS23x000TMS.pso.hlsl) against their deferred records
(FOLIAGE_DEFERRED 1: COLOR0 without the sun; COLOR1.rgb albedo; COLOR2.b = 2 + w, Includes/FoliageDeferred.hlsl) plus DeferredSun's foliage
branch (A x sunColour / luma(sunColour) x w x shadow), written out line by line in numpy from those files. The frame is what LinearScene
decodes after the draw (dec = x^gamma, the colours' own decode). Every G-buffer value and the frame are rounded to fp16, as the targets are
(A16B16G16R16F). Pseudo-random inputs: textures, vertex colours, the sun's colour and amount (N.L with the translucency, light scale, sun
scale: one number the vertex shader made), ambient, sky, the grass's lamps and highlight, fog colour (grey-blue, as the weathers') and amount (0-0.9), the shadow 0, 0.5, 1.
Reported in 8-bit units of the ENCODED frame (what the tonemapper starts from): max and the share of channels over 1/255 and 4/255.
Exact by construction where the record's sun hue is the pass's (both the game's directional light) at shadow 0 and 1; in the penumbra the
stored-curve fog lerp makes the forward light not linear in the shadow (reported). BEFORE = the first version (w fitted to the fogged difference, no keep); AFTER = the keep in COLOR2.g and DeferredSun's
FDAddFogged (2026-11-14 fog).
Usage: py -3.11 tests/fd_foliage_math.py [N]    (exit 1 if the shadow 0 / 1 bands fail)
"""
import sys
import numpy as np

N = int(sys.argv[1]) if len(sys.argv) > 1 else 200000
G = 2.2                     # [Shaders.PBR.Linear] Gamma (TESR_LinearLighting.x)
rng = np.random.default_rng(20261114)
f16 = lambda x: x.astype(np.float16).astype(np.float64)
dec = lambda c: np.power(np.maximum(c, 0.0), G)
enc = lambda c: np.power(np.maximum(c, 0.0), 1.0 / G)
luma = lambda c: c @ np.array([0.2126, 0.7152, 0.0722])
lerp = lambda a, b, t: a + (b - a) * t

def u(lo, hi, shape=(N,)): return rng.uniform(lo, hi, shape)

def report(name, fwd, dfr, limit):
    e = np.abs(enc(f16(fwd)) - enc(f16(dfr))) * 255.0
    over1, over4 = (e > 1.0).mean() * 100.0, (e > 4.0).mean() * 100.0
    ok = e.max() <= limit
    print("  %-46s max %6.3f /255   > 1/255: %6.3f %%   > 4/255: %6.3f %%   %s" % (name, e.max(), over1, over4, "ok" if ok else "OVER %.1f" % limit))
    return ok

def pass_add(albedo, b, sun_pass, shadow):	# DeferredSun.hlsl, FD_FOLIAGE branch (FoliageSunWeightOf: dec(b - 2))
    hue = sun_pass / np.maximum(luma(sun_pass), 1e-8)[:, None]
    return albedo * hue * dec(np.maximum(b - 2.0, 0.0))[:, None] * shadow
code = lambda w: f16(2.0 + enc(w))	# FoliageSunCode, stored in fp16

def fit(delta, basis):	# Includes/FoliageDeferred.hlsl FoliageFit
    bb = (basis * basis).sum(1)
    return np.where(bb > 1e-12, np.maximum((delta * basis).sum(1), 0.0) / np.maximum(bb, 1e-30), 0.0)
def hue_of(c):
    l = luma(c)
    return np.where((l > 1e-8)[:, None], c / np.maximum(l, 1e-30)[:, None], 0.0)
def weight(with_sun, without, albedo, sun_light):	# FoliageSunWeight
    return fit(np.maximum(dec(with_sun) - dec(without), 0.0), albedo * hue_of(sun_light))
# BEFORE (eb52ce809): w fitted to the fogged difference, the highlight in the 4.5 mark band, no keep
mark = lambda h: f16(4.421875 + 0.15625 * enc(np.clip(h / 1.0, 0, 1)))
mark_of = lambda g: 1.0 * dec(np.clip((g - 4.421875) / 0.15625, 0, 1))
# AFTER (2026-11-14 fog): Includes/DeferredPixel.hlsl FDFoliage*Code / FDFoliageKeep / FDFoliageHighlightEnc, FDAddFogged
leaf_keepq = lambda k: np.floor(np.clip(k, 0, 1) * 1023.0 + 0.5) / 1023.0
grass_keepq = lambda k: np.floor(np.clip(k, 0, 1) * 127.0 + 0.5) / 127.0
leaf_code = lambda kq: f16(16.0 + np.floor(kq * 1023.0 + 0.5) / 64.0)
def grass_code(kq, henc):
    i = 40.0 * np.floor(kq * 127.0 + 0.5) + np.floor(np.clip(henc, 0, 1) * 39.0 + 0.5)
    o = np.floor(i / 1024.0 + 1e-4)
    return f16(32.0 * np.power(2.0, o) * (1.0 + (i - 1024.0 * o) / 1024.0))
def grass_index(g):
    o = (g >= 63.99) * 1.0 + (g >= 127.99) + (g >= 255.99) + (g >= 511.99)
    return 1024.0 * o + np.floor((g / (32.0 * np.power(2.0, o)) - 1.0) * 1024.0 + 0.5)
def keep_of(g):
    return np.where(g < 31.99, np.clip(np.floor((g - 16.0) * 64.0 + 0.5) / 1023.0, 0, 1), np.floor(grass_index(g) / 40.0 + 1e-3) / 127.0)
def henc_of(g):
    i = grass_index(g)
    return (i - 40.0 * np.floor(i / 40.0 + 1e-3)) / 39.0
def add_fogged(frame, Dl, keep, F):	# FDAddFogged (frameLinear 1)
    v = enc(frame); f = 1.0 - keep
    encA = np.maximum(v - f * F, 0.0) / np.maximum(keep, 1e-4)
    v2 = f * F + keep * enc(dec(encA) + np.maximum(Dl, 0.0))
    return dec(v2) - frame
def after_add(frame, Dl, keep, F, fogged):	# DeferredSun's last lines: FDAddFogged where the game's fog is drawn and keep < 1
    return np.where((fogged & (keep < 0.9999))[:, None], add_fogged(frame, Dl, keep[:, None], F), Dl * keep[:, None])

ok = True
# ---------------------------------------------------------------- leaves
print("LEAVES (STLEAF200x + Leaf.hlsl LeafLighting / LeafDeferredApply)")
tex = u(0, 1, (N, 3)); D = u(0.3, 1.0, (N, 3)) * np.array([1.0, 0.9, 0.75])   # the directional light (stored), warm
k = u(0, 1.4); amb_s = u(0, 0.45, (N, 3)); point_s = np.where(u(0, 1) < 0.2, u(0, 0.3), 0.0)[:, None] * u(0.5, 1, (N, 3))
sun_s = k[:, None] * D                       # f_sun = light_scale x (N.L + translucency) x SunScale x DiffuseColor (one number x the colour)
fogc = np.clip(u(0.2, 0.8)[:, None] * np.array([0.85, 0.95, 1.1]) * u(0.9, 1.1, (N, 3)), 0, 1)   # the weather's fog: grey-blue
fog = np.where(u(0, 1) < 0.3, 0.0, u(0, 0.9))[:, None]
A = dec(tex); rest = dec(amb_s + point_s); sunL = dec(sun_s); A1 = np.clip(A, 0, 1)
# before
without_b = lerp(enc(A * rest), fogc, fog); with_ = lerp(enc(A * (rest + sunL)), fogc, fog)
alb16, b16_b = f16(A1), code(weight(with_, without_b, A1, sunL))
# after
kq = leaf_keepq(1.0 - fog[:, 0]); without_a = lerp(enc(A * rest), fogc, (1.0 - kq)[:, None])
b16_a, g16_a = code(luma(sunL)), leaf_code(kq)
fogged = fog[:, 0] > 0.0
bands = (("no fog", fog[:, 0] == 0.0, 1.5), ("fog 0-0.3", (fog[:, 0] > 0.0) & (fog[:, 0] <= 0.3), 1.5), ("fog 0.3-0.9", fog[:, 0] > 0.3, 1.5))
for s_ in (0.0, 1.0, 0.5):
    fwd = dec(lerp(enc(A * (rest + sunL * s_)), fogc, fog))                    # LeafLighting's linear split, the shader's fog lerp, LinearScene's decode
    before = dec(f16(without_b)) + pass_add(alb16, b16_b, dec(D), s_)
    fr = dec(f16(without_a))
    after = fr + after_add(fr, pass_add(alb16, b16_a, dec(D), s_), keep_of(g16_a), fogc, fogged)
    for band, sel, limit in bands:
        report("shadow %.1f, %s, BEFORE (fit)" % (s_, band), fwd[sel], before[sel], 1e9)
        ok &= report("shadow %.1f, %s, AFTER (keep + FDAddFogged)" % (s_, band), fwd[sel], after[sel], limit)

# ---------------------------------------------------------------- grass
print("GRASS (GRASS23x000TMS.pso.hlsl main / its FOLIAGE_DEFERRED block)")
tex = u(0, 1, (N, 3)); vc = np.clip(u(0.1, 1.0, (N, 1)) * u(0.8, 1.0, (N, 3)), 0, 1)   # vertex colour: grey-brown roots .. white tips
Dg = u(0.3, 1.0, (N, 3)) * np.array([1.0, 0.92, 0.8]); z = 1.0                # DiffuseColor; TESR_PBRData.z (LightingScale)
PBRLight = lambda c: dec(c * z)
f_sun = vc * Dg                                                               # GRASS23x00N.vso, Vegas Shaders' pixel shader paired
light_scale = u(0.25, 1.0); ndl = u(0, 1.2); sun_base = light_scale * ndl     # f_amount.y (N.L + translucency) x light scale (AddlParams 1)
amb = dec(u(0, 0.4, (N, 3))) * light_scale[:, None]; sky = dec(u(0, 0.3, (N, 3))) * 0.5
NB = u(0.5, 1.0)[:, None]; Bx = u(0.8, 1.0)[:, None]   # [Shaders.Grass.Main] Brightness 1 by default (above 1 the albedo record clips)
lamps = np.where(u(0, 1) < 0.15, 1.0, 0.0)[:, None] * dec(u(0, 0.5, (N, 3)))
spec = np.where(u(0, 1) < 0.3, 1.0, 0.0)[:, None] * PBRLight(f_sun) * (light_scale * u(0, 0.3))[:, None]
shade = light_scale[:, None]                                                   # CapturedGrassShade 0: the tuft's full shade
fogc = np.clip(u(0.2, 0.8)[:, None] * np.array([0.85, 0.95, 1.1]) * u(0.9, 1.1, (N, 3)), 0, 1)   # the weather's fog: grey-blue
fog = np.where(u(0, 1) < 0.5, 0.0, u(0, 0.6))[:, None]
base = dec(tex)
rest = (amb + sky) * NB + lamps
sunT = PBRLight(f_sun) * (sun_base[:, None] * NB)
hue = PBRLight(Dg)
alb = np.clip(base * Bx * np.clip(PBRLight(f_sun) / np.maximum(hue, 1e-6), 0, 1) * shade, 0, 1)
alb16 = f16(alb)
# before
without_b = lerp(enc(rest * base * Bx), fogc, fog)
with_d = lerp(enc((rest + sunT) * base * Bx), fogc, fog)
with_ = lerp(enc(((rest + sunT) * base + spec * NB) * Bx), fogc, fog)
b16_b, g16_b = code(weight(with_d, without_b, alb, hue)), mark(fit(np.maximum(dec(with_) - dec(with_d), 0.0), hue_of(hue)))
# after
kq = grass_keepq(1.0 - fog[:, 0]); without_a = lerp(enc(rest * base * Bx), fogc, (1.0 - kq)[:, None])
b16_a = code(fit(sunT * base * Bx, alb * hue_of(hue)))
g16_a = grass_code(kq, enc(np.clip(fit(spec * NB * Bx, hue_of(hue)), 0, 1)))
hq = np.floor(np.clip(enc(np.clip(fit(spec * NB * Bx, hue_of(hue)), 0, 1)), 0, 1) * 39.0 + 0.5) / 39.0
assert ((g16_a >= 32.0) & (g16_a < 1024.0)).all() and np.array_equal(keep_of(g16_a), kq) and np.allclose(henc_of(g16_a), hq), "the grass code does not read back"
assert np.array_equal(keep_of(leaf_code(np.linspace(0, 1, 1024))), leaf_keepq(np.linspace(0, 1, 1024))), "the leaf code does not read back"
fogged = fog[:, 0] > 0.0
nospec = spec[:, 0] == 0.0
bands = (("no fog, no highlight", (fog[:, 0] == 0.0) & nospec, 1.5), ("no fog, highlight", (fog[:, 0] == 0.0) & ~nospec, 16.0),
         ("fog 0-0.6, no highlight", fogged & nospec, 1.5), ("fog 0-0.6, highlight", fogged & ~nospec, 16.0))
for s_ in (0.0, 1.0, 0.5):
    fwd = dec(lerp(enc(((rest + sunT * s_) * base + spec * NB * s_) * Bx), fogc, fog))
    before = dec(f16(without_b)) + pass_add(alb16, b16_b, dec(Dg), s_) + hue_of(dec(Dg)) * mark_of(g16_b)[:, None] * s_
    fr = dec(f16(without_a))
    Dl = pass_add(alb16, b16_a, dec(Dg), s_) + hue_of(dec(Dg)) * dec(henc_of(g16_a))[:, None] * s_
    after = fr + after_add(fr, Dl, keep_of(g16_a), fogc, fogged)
    for band, sel, limit in bands:
        report("shadow %.1f, %s, BEFORE" % (s_, band), fwd[sel], before[sel], 1e9)
        ok &= report("shadow %.1f, %s, AFTER" % (s_, band), fwd[sel], after[sel], limit)
print("FD FOLIAGE MATH: %s" % ("PASS" if ok else "FAIL"))
sys.exit(0 if ok else 1)
