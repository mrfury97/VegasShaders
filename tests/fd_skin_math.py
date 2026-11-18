#!/usr/bin/env python3
"""UNOFFICIAL 2026-11-15 full deferred renderer, phase 4 skin (docs/next/FD-SKIN-RESULTS.md): the numeric check of the deferred skin.

Forward: SkinTemplate.hlsl's direct light (Includes/Skin.hlsl SkinLight for the sun and for a lamp, written out line by line in numpy), in the
TANGENT space the forward shader lights in, with the true blurred normal (Nsoft), the true curvature and the true specular mask.
Deferred: the same light as DeferredSun / DeferredLamps compute it (Includes/SkinDeferred.hlsl FDSkinLight) in WORLD space from what the
deferred record stored: the bumped normal (COLOR2.g / a, octahedral, fp16), the vertex normal (COLOR3.rg, octahedral, fp16), the roughness
(COLOR2.b, fp16), the albedo (COLOR1.rgb, fp16) and FD_SKIN_CODE (COLOR3.a: mask 32 levels, thinness 24 levels on a square root, softness 16
levels, fp16's grid; the code is decoded exactly as the shader does and checked to round-trip every index), Nsoft rebuilt as
normalize(lerp(Ngeo, N, soft)).
The frame: ambient (as COLOR0 holds it, the same both ways) + the direct light; reported in 8-bit units of the ENCODED frame (x^(1/2.2), what
the tonemapper starts from): max, share of channels over 1/255 and 4/255. Cases: the sun at shadow 0 / 0.5 / 1 / random, a lamp; with the
curvature diffusion (forward with the blur off: the parity reference) and with it off (the blur takes it); the Nsoft approximation alone (the
rest exact) and everything stored (the full deferred path); an orthonormal tangent frame (the skinned frame's shear is not modelled).
Reported in 8-bit units of the encoded frame clipped at white (an 8-bit picture), with the share of PIXELS that have a channel over 16/255:
the parity band's measure (<= 1.8 %).
Usage: py -3.11 tests/fd_skin_math.py [N]   (exit 1 when the code does not round-trip or a deferred case leaves the 1.8 % band)
"""
import sys
import numpy as np

N = int(sys.argv[1]) if len(sys.argv) > 1 else 200000
G = 2.2
rng = np.random.default_rng(20261115)
f16 = lambda x: np.asarray(x).astype(np.float16).astype(np.float64)
dec = lambda c: np.power(np.maximum(c, 0.0), G)
enc = lambda c: np.power(np.maximum(c, 0.0), 1.0 / G)
sat = lambda x: np.clip(x, 0.0, 1.0)
lerp = lambda a, b, t: a + (b - a) * t
def dot(a, b): return np.sum(a * b, axis=-1)
def norm(v): return v / np.maximum(np.linalg.norm(v, axis=-1, keepdims=True), 1e-20)
def u(lo, hi, shape=(N,)): return rng.uniform(lo, hi, shape)
PI = 3.1415926538

# --- the settings (TESR_SkinMaterial / TESR_SkinLook): the defaults, Scattering 1
SPEC_STRENGTH, ROUGH_SETTING, SCATTER, TRANSLUCENCY, SHADOW_SCATTER, VANILLA_MATCH, WET = 1.0, 0.45, 1.0, 0.5, 1.0, 0.0, 0.0
F0 = 0.028; LOBE0, LOBE1, LOBE_MIX = 0.75, 1.3, 0.15
TRANSMIT_TINT = np.array([1.0, 0.28, 0.12]); TRANSMIT_DISTORTION, TRANSMIT_POWER = 0.3, 4.0
PENUMBRA_TINT = np.array([0.3, 0.06, 0.04]); SUN_RADIUS = 0.00918043
SkinLinearColor = lambda c: dec(np.sqrt(np.maximum(c, 0.0)))   # LinColor(sqrt(c)) with linear lighting

# --- Includes/Skin.hlsl, vectorised (one pixel a row; the statics passed in)
def sg_irradiance(NdotL, lam):
    c0 = 0.36; c1 = 1.0 / (4.0 * c0)
    eml = np.exp(-lam); em2l = eml * eml; rl = 1.0 / lam
    scale = 1.0 + 2.0 * em2l - rl; bias = (eml - em2l) * rl - em2l
    x = np.sqrt(np.maximum(1.0 - scale, 1e-8)); x0 = c0 * NdotL; x1 = c1 * x; n = x0 + x1
    y = np.where(np.abs(x0) <= x1, n * n / x, sat(NdotL))
    return sat(scale * y + bias)

def diffusion(N, Ns, L, curv):
    ng = norm(lerp(N, Ns, 0.6)); nb = norm(lerp(N, Ns, 0.3))
    ndl = np.stack([dot(Ns, L), dot(ng, L), dot(nb, L)], -1)
    c = np.maximum(curv, 1e-6)[:, None]
    lam = np.concatenate([2.4 * c ** -1.5, 28.0 / (c * c), 71.0 / (c * c)], -1)
    return np.where((curv > 0.002)[:, None], sg_irradiance(ndl, lam), sat(ndl))

def skin_shadow(s):
    pen = s * (1.0 - s) * 4.0
    return sat(s[:, None] + pen[:, None] * PENUMBRA_TINT[None] * SHADOW_SCATTER)

def ggx(nh, r): a = r ** 4; d = np.maximum((nh * a - nh) * nh + 1, 1e-5); return a / (PI * d * d)
def sb(nx, r): kk = (r + 1) ** 2 / 8.0; return nx / np.maximum(nx * (1 - kk) + kk, 1e-8)
def brdf(r, F, nv, nl, nh): return (ggx(nh, r) * sb(nv, r) * sb(nl, r))[:, None] * F / (4.0 * nv * nl)[:, None]

def vanilla_match():
    a0 = np.clip(ROUGH_SETTING * LOBE0, 0.04, 1.0) ** 2; a1 = np.clip(ROUGH_SETTING * LOBE1, 0.04, 1.0) ** 2
    peak = lerp(F0 / (4 * a0 * a0), F0 / (4 * a1 * a1), LOBE_MIX)
    return lerp(1.0, np.clip(1.0 / (2.0 * peak), 1.0, 12.0), sat(VANILLA_MATCH))

def specular(N, V, L, light, sun, rough, specScale):
    r0 = np.clip(rough * LOBE0, 0.04, 1.0); r1 = np.clip(rough * LOBE1, 0.04, 1.0)
    n0 = np.ones(len(N)); n1 = np.ones(len(N))
    if sun:
        rr = 2.0 * dot(N, V)[:, None] * N - V   # reflect(-V, N)
        ctr = dot(L, rr)[:, None] * rr - L
        L = norm(L + ctr * sat(SUN_RADIUS / np.maximum(np.linalg.norm(ctr, axis=-1), 1e-5))[:, None])
        def snorm(r): a = r * r; ratio = a / sat(a + SUN_RADIUS * 0.5); return ratio * ratio
        n0 = snorm(r0); n1 = snorm(r1)
    H = norm(V + L)
    nl = np.maximum(sat(dot(N, L)), 1e-5); nv = np.maximum(sat(dot(N, V)), 1e-5); nh = sat(dot(N, H)); lh = sat(dot(L, H))
    fres = (F0 + (1 - F0) * (1 - lh) ** 5)[:, None] * np.ones((1, 3))
    lobes = lerp(brdf(r0, fres, nv, nl, nh) * n0[:, None], brdf(r1, fres, nv, nl, nh) * n1[:, None], LOBE_MIX)
    return lobes * (nl * PI * specScale * vanilla_match())[:, None] * light

def skin_light(albedo, N, Ns, gNdotL, V, L, light, shadow, sun, rough, specScale, curv, thin):
    L = norm(L)
    scat = diffusion(N, Ns, L, curv)
    direct = np.minimum(scat, sat(dot(N, L))[:, None]); wrapped = scat - direct
    ss = sat((0.05 - gNdotL) / 0.15); ss = ss * ss * (3 - 2 * ss) * sat(SHADOW_SCATTER)
    sc = skin_shadow(shadow)
    diffuse = albedo * (direct * sc + wrapped * lerp(sc, 1.0, ss[:, None]))
    t = sat((thin - 0.3) / 0.6); thinS = t * t * (3 - 2 * t)
    far = sat(0.3 - gNdotL)
    tdir = norm(L + Ns * TRANSMIT_DISTORTION)
    back = sat(dot(V, -tdir)) ** TRANSMIT_POWER
    transmitted = albedo * SkinLinearColor(TRANSMIT_TINT)[None] * (back * thinS * far * TRANSLUCENCY * shadow)[:, None]
    spec = specular(N, V, L, light, sun, rough, specScale) * shadow[:, None]
    return (diffuse + transmitted) * light + spec

# --- the G-buffer codes, as the shaders write and read them
def oct_enc(n):
    n = n / np.sum(np.abs(n), -1, keepdims=True)
    e = n[:, :2].copy()
    neg = n[:, 2] < 0
    sgn = np.where(e >= 0, 1.0, -1.0)
    e[neg] = (1.0 - np.abs(n[neg][:, [1, 0]])) * sgn[neg]
    return e
def oct_dec(e):
    n = np.concatenate([e, (1.0 - np.abs(e[:, 0]) - np.abs(e[:, 1]))[:, None]], -1)
    t = sat(-n[:, 2])
    n[:, 0] += np.where(n[:, 0] >= 0, -t, t); n[:, 1] += np.where(n[:, 1] >= 0, -t, t)
    return norm(n)

def code_of(mask, thin, soft):	# Includes/SkinDeferred.hlsl FDSkinCode
    mq = np.floor(sat(mask) * 31 + 0.5); cq = np.floor(np.sqrt(sat(thin)) * 23 + 0.5); bq = np.floor(sat(soft) * 15 + 0.5)
    i = (mq * 24 + cq) * 16 + bq
    o = np.floor(i / 1024 + 1e-4)
    return 4.0 * np.exp2(o) * (1 + (i - 1024 * o) / 1024)
def code_dec(a):	# FDSkinDecode (log2 with the shader's margin)
    o = np.floor(np.log2(np.maximum(a, 4.0) / 4.0) + 1e-4)
    i = 1024 * o + np.floor((a / (4.0 * np.exp2(o)) - 1) * 1024 + 0.5)
    mq = np.floor((i + 0.5) / 384); rest = i - mq * 384; cq = np.floor((rest + 0.5) / 16); bq = rest - cq * 16
    return mq / 31, (cq / 23) ** 2, bq / 15
def softness(N, Ns):	# FDSkinSoftness (tangent space)
    r = np.linalg.norm(N[:, :2], axis=-1)
    p = dot(Ns[:, :2], N[:, :2]) / np.maximum(r, 1e-12)
    den = r * Ns[:, 2] + p * (1 - N[:, 2])
    out = np.where(den > 1e-5, sat(p / np.maximum(den, 1e-12)), 1.0)
    return np.where(r < 1e-3, 1.0, out)

def check_code():
    idx = np.arange(12288)
    o = np.floor(idx / 1024 + 1e-4); a = 4.0 * np.exp2(o) * (1 + (idx - 1024 * o) / 1024)
    ok = np.all(f16(a) == a)
    o2 = np.floor(np.log2(a / 4.0) + 1e-4); back = 1024 * o2 + np.floor((a / (4.0 * np.exp2(o2)) - 1) * 1024 + 0.5)
    ok = ok and np.all(back == idx) and a.max() < 16384.0
    print("FD_SKIN_CODE: 12288 indices, every value on fp16's grid and decoded back exactly: %s (largest %.1f)" % ("yes" if ok else "NO", a.max()))
    return ok

# --- pixels
def tilted(maxdeg):	# random unit vectors within maxdeg of +z
    ct = np.cos(np.radians(u(0, maxdeg))); st = np.sqrt(1 - ct * ct); ph = u(0, 2 * PI)
    return np.stack([st * np.cos(ph), st * np.sin(ph), ct], -1)
def rotation():	# random orthonormal frames (rows T, B, N in world)
    q = norm(rng.normal(size=(N, 4))); w, x, y, z = q.T
    R = np.stack([np.stack([1-2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w)], -1), np.stack([2*(x*y+z*w), 1-2*(x*x+z*z), 2*(y*z-x*w)], -1),
                  np.stack([2*(x*z-y*w), 2*(y*z+x*w), 1-2*(x*x+y*y)], -1)], 1)
    return R
def to_world(R, v): return np.einsum('nij,nj->ni', R, v)

def main():
    ok = check_code()
    for pores in (8.0, 25.0):
        print("=== the normal map's fine detail (what 2 mips down loses) tilted up to %.0f deg%s" % (pores, " (a face's pores and stubble)" if pores < 10 else " (a harsh case: wrinkles as fine as pores)"))
        ok &= run(pores)
    print("FD SKIN MATH: %s" % ("PASS" if ok else "FAIL"))
    sys.exit(0 if ok else 1)

def run(pores):
    ok = True
    low = tilted(25.0)	# the normal map's broad shape (what 2 mips down keeps)
    detail = tilted(pores)
    N_ts = norm(np.concatenate([low[:, :2] + detail[:, :2] * u(0, 1)[:, None], low[:, 2:]], -1))	# + pores
    Ns_ts = norm(lerp(low, N_ts, u(0, 0.35)[:, None]))	# the blurred read: mostly the broad shape
    V_ts = tilted(85.0)
    R = rotation()
    Zw = to_world(R, np.tile([0.0, 0.0, 1.0], (N, 1)))
    Nw, Nsw, Vw = to_world(R, N_ts), to_world(R, Ns_ts), to_world(R, V_ts)
    albedo = u(0.05, 1.0, (N, 3)); mask = u(0, 1); rough = u(0.08, 1.0)
    x = u(0, 1.4)	# curvature x 2 (SkinSetup's curvature x SKIN_SCATTER_BASE)
    thin = sat(x)
    specScale = mask * 2 * SPEC_STRENGTH * (1 + WET)
    ambient = albedo * u(0.0, 0.3, (N, 1))
    sunColor = dec(u(0.5, 1.0, (N, 3)) * 1.2)
    # stored
    oN = f16(oct_enc(Nw)); oZ = f16(oct_enc(Zw))
    Nw_d, Zw_d = oct_dec(oN), oct_dec(oZ)
    rough_d = f16(rough); albedo_d = f16(albedo)
    code = f16(code_of(mask, thin, softness(N_ts, Ns_ts)))
    m_d, thin_d, soft_d = code_dec(code)
    specScale_d = m_d * 2 * SPEC_STRENGTH * (1 + WET)
    Ns_d = norm(lerp(Zw_d, Nw_d, soft_d[:, None]))
    Ns_only = norm(lerp(Zw, Nw, softness(N_ts, Ns_ts)[:, None]))	# the softness approximation alone, the rest exact

    def frame(direct): return f16(ambient + direct)
    def report(name, fwd, dfr, limit_over16=None):
        # 8-bit units of the encoded frame, clipped at white as an 8-bit picture is (skin highlights at roughness 0.06 reach many times white)
        e = np.abs(sat(enc(frame(fwd))) - sat(enc(frame(dfr)))) * 255.0
        o1, o4 = (e > 1).mean() * 100, (e > 4).mean() * 100
        px16 = (e.max(axis=-1) > 16).mean() * 100	# pixels with a channel over 16/255: the parity band's measure (ParityTest)
        flag = "" if limit_over16 is None else ("ok" if px16 <= limit_over16 else "OVER %.1f %%" % limit_over16)
        print("  %-52s max %6.2f   >1: %6.3f %%   >4: %6.3f %%   pixels >16: %6.3f %%   %s" % (name, e.max(), o1, o4, px16, flag))
        return limit_over16 is None or px16 <= limit_over16

    for blur in (False, True):
        curv = np.zeros(N) if blur else sat(x * SCATTER)
        curv_d = np.zeros(N) if blur else sat(thin_d * SCATTER)
        print("curvature diffusion %s:" % ("OFF (the blur takes it)" if blur else "ON (forward without the blur: the parity reference)"))
        for sname, shadow in (("shadow 1", np.ones(N)), ("shadow 0.5", np.full(N, 0.5)), ("shadow 0", np.zeros(N)), ("shadow random", u(0, 1))):
            L_ts = norm(rng.normal(size=(N, 3))); Lw = to_world(R, L_ts)
            fwd = skin_light(albedo, N_ts, Ns_ts, L_ts[:, 2], V_ts, L_ts, sunColor, shadow, True, rough, specScale, curv, thin)
            soft_only = skin_light(albedo, Nw, Ns_only, dot(Zw, Lw), Vw, Lw, sunColor, shadow, True, rough, specScale, curv, thin)
            dfr = skin_light(albedo_d, Nw_d, Ns_d, dot(Zw_d, Lw), Vw, Lw, sunColor, shadow, True, rough_d, specScale_d, curv_d, thin_d)
            report("sun, %s: Nsoft approximation alone" % sname, fwd, soft_only)
            ok &= report("sun, %s: everything stored (the deferred path)" % sname, fwd, dfr, 1.8)
        # a lamp: d = its tangent-space vector / radius (|d| < 1), falloff 1 - |d|^2, colour decoded
        d_ts = norm(rng.normal(size=(N, 3))) * u(0.05, 0.95)[:, None]; dw = to_world(R, d_ts)
        lampColor = dec(u(0.3, 1.0, (N, 3))) * (1 - dot(d_ts, d_ts))[:, None]
        one = np.ones(N)
        fwd = skin_light(albedo, N_ts, Ns_ts, norm(d_ts)[:, 2], V_ts, d_ts, lampColor, one, False, rough, specScale, curv, thin)
        dfr = skin_light(albedo_d, Nw_d, Ns_d, dot(Zw_d, norm(dw)), Vw, dw, lampColor, one, False, rough_d, specScale_d, curv_d, thin_d)
        ok &= report("a lamp: everything stored", fwd, dfr, 1.8)
    return ok

if __name__ == "__main__": main()
