"""Generates the shipped effect textures under resource/Textures/ that used to come from New Vegas Reloaded.

Every texture is made procedurally from fixed seeds, so a run always gives the same pixels (the block-compressed
ones go through Microsoft's texconv, which is deterministic too). Each one keeps the original file's size, mip
count, pixel format and channel layout, because the shaders and their samplers were written against those.
See resource/Textures/README.md for what each texture is and which shader reads it.

    py -3.11 tools/gen_textures.py                     # all of them, written into resource/Textures/
    py -3.11 tools/gen_textures.py --only ripples,caust_001
    py -3.11 tools/gen_textures.py --texconv C:\\path\\texconv.exe --work H:\\scratch

Needs numpy and Pillow. texconv (https://github.com/microsoft/DirectXTex) is needed for the DXT1/DXT5 files:
pass --texconv, set TEXCONV, or have it on PATH.
"""
import argparse
import math
import os
import shutil
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import texgen_dds as dds  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEX = os.path.join(REPO, "resource", "Textures")


# ---------------------------------------------------------------------------------------------------------------
# Periodic noise helpers. Everything is built from whole-number frequencies per tile, so it tiles exactly.
# ---------------------------------------------------------------------------------------------------------------

def freqs(h, w):
    """Frequency grids in cycles per tile (ky down the rows, kx across)."""
    ky = (np.fft.fftfreq(h) * h)[:, None]
    kx = (np.fft.fftfreq(w) * w)[None, :]
    return kx, ky


def normalize(a):
    a = a - a.mean()
    return a / max(a.std(), 1e-12)


def spectral(rng, h, w, amp):
    """White noise shaped by amp(kx, ky, k) in the frequency domain; zero mean, unit deviation, tiles exactly."""
    kx, ky = freqs(h, w)
    k = np.sqrt(kx * kx + ky * ky)
    f = np.fft.fft2(rng.standard_normal((h, w)))
    with np.errstate(divide="ignore", invalid="ignore"):
        a = np.nan_to_num(amp(kx, ky, k))
    a[0, 0] = 0.0
    return normalize(np.real(np.fft.ifft2(f * a)))


def fbm(rng, h, w, beta, kmin=1.0, kmax=None):
    """Power-law noise: amplitude ~ k^-beta between kmin and a soft cut at kmax (cycles per tile)."""
    kmax = kmax or max(h, w)
    return spectral(rng, h, w, lambda kx, ky, k: (k >= kmin) * np.power(np.maximum(k, 1e-6), -beta)
                    * np.exp(-(k / kmax) ** 2))


def blur(a, sigma):
    """Periodic gaussian blur (sigma in pixels) on the first two axes."""
    h, w = a.shape[:2]
    fy = np.fft.fftfreq(h)[:, None]
    fx = np.fft.fftfreq(w)[None, :]
    g = np.exp(-2.0 * math.pi ** 2 * sigma ** 2 * (fx * fx + fy * fy))
    if a.ndim == 3:
        g = g[..., None]
    return np.real(np.fft.ifft2(np.fft.fft2(a, axes=(0, 1)) * g, axes=(0, 1)))


def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def remap(a, mean, sd, lo=0.0, hi=1.0):
    return np.clip(normalize(a) * sd + mean, lo, hi)


def encode_normal(sx, sy, target_sd):
    """Slopes -> tangent-space normal in 0..1, with the slope gain chosen so the red channel's deviation
    matches target_sd (the original map's). Returns HxWx3."""
    lo, hi = 1e-4, 1e4
    for _ in range(60):
        g = math.sqrt(lo * hi)
        n = np.stack([-g * sx, -g * sy, np.ones_like(sx)], -1)
        n /= np.linalg.norm(n, axis=-1, keepdims=True)
        if (n[..., 0] * 0.5).std() < target_sd:
            lo = g
        else:
            hi = g
    return n * 0.5 + 0.5


def slopes_of(hgt):
    """Central differences with wrap (per pixel)."""
    sx = 0.5 * (np.roll(hgt, -1, 1) - np.roll(hgt, 1, 1))
    sy = 0.5 * (np.roll(hgt, -1, 0) - np.roll(hgt, 1, 0))
    return sx, sy


def ocean_spectrum(rng, n, peak, cut, wind=(0.8, 0.6), spread=0.35, m=None):
    """Directional Phillips-style ocean height spectrum h0(k) on an n x m grid (frequencies in cycles per tile).
    peak: frequency of the longest waves, cut: soft upper cut, spread: share of energy off the wind axis."""
    m = m or n
    kx, ky = freqs(n, m)
    k = np.sqrt(kx * kx + ky * ky)
    ks = np.maximum(k, 1e-6)
    cosw = (kx * wind[0] + ky * wind[1]) / ks
    p = np.exp(-(peak / ks) ** 2) / ks ** 4 * (spread + (1.0 - spread) * cosw ** 2) * np.exp(-(k / cut) ** 2)
    p[0, 0] = 0.0
    g = (rng.standard_normal((n, m)) + 1j * rng.standard_normal((n, m))) / math.sqrt(2.0)
    return g * np.sqrt(p), kx, ky, k


# ---------------------------------------------------------------------------------------------------------------
# Splat helpers (drops, bokeh discs). Each draws into a local window with wrap-around, so the result tiles.
# ---------------------------------------------------------------------------------------------------------------

def splat(img, cx, cy, rad, fn, wrap=True):
    """img[y, x] = combine with fn(dx, dy) over a window of radius rad around (cx, cy). fn returns (value, weight)."""
    h, w = img.shape[:2]
    r = int(math.ceil(rad)) + 2
    xs = np.arange(int(cx) - r, int(cx) + r + 1)
    ys = np.arange(int(cy) - r, int(cy) + r + 1)
    if not wrap:
        xs = xs[(xs >= 0) & (xs < w)]
        ys = ys[(ys >= 0) & (ys < h)]
        if not len(xs) or not len(ys):
            return
    dx = (xs[None, :] + 0.5) - cx
    dy = (ys[:, None] + 0.5) - cy
    yy, xx = np.ix_(ys % h, xs % w)
    fn(img, yy, xx, dx, dy)


# ---------------------------------------------------------------------------------------------------------------
# The textures
# ---------------------------------------------------------------------------------------------------------------

def gen_dirtlens():
    """Lens dirt: soft bokeh discs of different sizes and a faint smear haze on black. Lens.fx reads red only,
    raised to (3 - smudginess) and multiplied with the bloom. 2048 x 1365 RGB, like the original."""
    rng = np.random.default_rng(1001)
    h, w = 1365, 2048
    img = np.zeros((h, w, 3))
    haze = smoothstep(-0.5, 2.5, fbm(rng, h, w, 1.6, 2, 60)) * 0.05
    img += haze[..., None]

    def disc(cx, cy, r, val, tint, rim, soft):
        def fn(im, yy, xx, dx, dy):
            d = np.sqrt(dx * dx + dy * dy)
            body = smoothstep(r, r - soft, d)
            ring = 1.0 + rim * smoothstep(0.55 * r, r - 0.5 * soft, d)
            im[yy, xx] = np.maximum(im[yy, xx], (body * ring * val)[..., None] * tint)
        splat(img, cx, cy, r, fn)

    def add_layer(count, rmin, rmax, vmin, vmax, soft_frac, rim):
        for _ in range(count):
            r = math.exp(rng.uniform(math.log(rmin), math.log(rmax)))
            v = vmin + (vmax - vmin) * rng.random() ** 1.6
            tint = np.array([0.93, 0.95, 1.05]) + rng.normal(0, 0.03, 3)
            disc(rng.uniform(0, w), rng.uniform(0, h), r, v, tint, rim * rng.uniform(0.3, 1.0),
                 max(1.5, soft_frac * r))

    add_layer(150, 3.0, 8.0, 0.04, 0.30, 0.6, 0.0)       # fine specks
    add_layer(220, 12.0, 30.0, 0.04, 0.30, 0.45, 0.15)   # medium smudges
    add_layer(50, 26.0, 52.0, 0.12, 0.85, 0.30, 0.25)    # big bokeh discs

    # a few faint wipe streaks: elongated soft blobs strung along gentle arcs
    for _ in range(9):
        x0, y0 = rng.uniform(0, w), rng.uniform(0, h)
        ang, bend, length = rng.uniform(0, 2 * math.pi), rng.uniform(-0.004, 0.004), rng.uniform(150, 450)
        val = rng.uniform(0.03, 0.06)
        for s in np.arange(0, length, 10.0):
            a = ang + bend * s
            disc(x0 + s * math.cos(a), y0 + s * math.sin(a), rng.uniform(12, 20), val, np.array([0.95, 0.96, 1.04]),
                 0.0, 10.0)
    img = blur(img, 1.5)                                    # out of focus: no hard pixel edges anywhere
    return np.clip(img, 0.0, 0.9)


def gen_bloodlens():
    """Blood on the lens. BloodLens.fx: alpha = blotch pattern (only the part above a random threshold turns to
    blood), red/green = a two-channel warp offset (x2-1), blue unused. Ours: clustered drops and smears in alpha;
    red/green = a refraction field around the drops plus a faint wobble, centred on 0.5."""
    rng = np.random.default_rng(1002)
    n = 1024
    # where the blood lands: a few dozen clusters of different spread (a density field that tiles)
    dens = np.zeros((n, n))
    for _ in range(30):
        cx, cy = rng.uniform(0, n, 2)
        dens[int(cy), int(cx)] += rng.uniform(0.6, 1.0)
    dens = blur(dens, 34.0) * 0.55 + blur(dens, 80.0) * 0.45
    dens /= dens.max()
    # irregular splatter inside the clusters: fine noise cut by a threshold that drops where the density is high
    spots = 0.7 * fbm(rng, n, n, 0.7, 30, 160) + 0.3 * fbm(rng, n, n, 1.0, 10, 60)
    field = spots + 2.6 * dens
    t = np.quantile(field, 0.977)
    a = smoothstep(t - 0.10, t + 0.10, field)

    def drop(cx, cy, rx, ry, ang, val):
        ca, sa = math.cos(ang), math.sin(ang)

        def fn(im, yy, xx, dx, dy):
            u = (dx * ca + dy * sa) / rx
            v = (-dx * sa + dy * ca) / ry
            d = np.sqrt(u * u + v * v)
            edge = smoothstep(1.0, 1.0 - 1.2 / max(min(rx, ry), 1.0), d)
            im[yy, xx] = np.maximum(im[yy, xx], edge * val)
        splat(a, cx, cy, max(rx, ry), fn)

    for _ in range(260):  # loose round droplets and fine spray
        r = float(np.clip(rng.lognormal(math.log(1.6), 0.45), 0.8, 5.0))
        drop(rng.uniform(0, n), rng.uniform(0, n), r * rng.uniform(1.0, 1.6), r, rng.uniform(0, math.pi),
             rng.uniform(0.8, 1.0))

    thick = blur(a, 2.0)
    sx, sy = slopes_of(thick)
    wob = 0.03
    fx = sx * 1.6 + wob * fbm(rng, n, n, 1.5, 2, 24)
    fy = sy * 1.6 + wob * fbm(rng, n, n, 1.5, 2, 24)
    out = np.zeros((n, n, 4))
    out[..., 0] = np.clip(0.5 + fx, 0, 1)
    out[..., 1] = np.clip(0.5 + fy, 0, 1)
    out[..., 3] = np.clip(a, 0, 1)
    return out


def gen_flashlight():
    """Flashlight cookie (red read, CLAMP). Bright hotspot, darker shoulder, a broad bright band, a thin outer
    ring and a fade to black at the corners, with a faint lens pattern (fine rings, slight angular unevenness)."""
    rng = np.random.default_rng(1003)
    n = 2048
    y, x = (np.mgrid[0:n, 0:n] + 0.5 - n / 2) / (n / 2)
    r = np.sqrt(x * x + y * y)
    th = np.arctan2(y, x)
    hot = 0.46 * np.exp(-(r / 0.19) ** 4)
    band = 0.27 * np.exp(-((r - 0.46) / 0.18) ** 2)
    ring = 0.07 * np.exp(-((r - 0.76) / 0.018) ** 2)
    glow = 0.20 * np.exp(-(r / 0.85) ** 4)
    f = hot + band + ring + glow
    # lens pattern: faint concentric ripples and slow angular unevenness, both small
    f *= 1.0 + 0.025 * np.sin(r * 90.0) * np.exp(-r * 1.5)
    ang = sum(rng.uniform(0.01, 0.025) * np.cos(k * th + rng.uniform(0, 2 * math.pi)) for k in (3, 5, 7, 11))
    f *= 1.0 + ang * smoothstep(0.05, 0.35, r)              # no pinch at the centre
    speck = blur(rng.standard_normal((n, n)), 40.0)
    f *= 1.0 + 0.04 * normalize(speck) * smoothstep(1.0, 0.2, r)
    f *= smoothstep(1.12, 0.92, r)                         # dark corners; edges stay near the original's level
    f = np.clip(f, 0.0, 0.71)
    out = np.ones((n, n, 4))
    out[..., :3] = f[..., None]
    return out


def gen_water_nrm_lens():
    """WaterLens noise (256, wrap). Read as (x2-1): red/green = screen tilt, blue = z of the refraction direction
    (low blue bends strongly and makes glints), alpha = a second height added to that z from another sample.
    Ours: running-water streaks + droplets as the normal, a large soft patch mask in blue, smooth noise in alpha."""
    rng = np.random.default_rng(1004)
    n = 256
    # streaks along a diagonal: noise stretched along (1, 1)
    streak = spectral(rng, n, n, lambda kx, ky, k: (k >= 1) * np.exp(-(((kx + ky) / 2.0) / 3.0) ** 2)
                      * np.exp(-(((kx - ky) / 2.0) / 14.0) ** 2) * np.power(np.maximum(k, 1e-6), -0.8))
    drops = np.zeros((n, n))
    for _ in range(40):
        cx, cy, r = rng.uniform(0, n), rng.uniform(0, n), rng.uniform(4.0, 10.0)

        def fn(im, yy, xx, dx, dy, r=r):
            d2 = (dx * dx + dy * dy) / (r * r)
            im[yy, xx] += np.sqrt(np.clip(1.0 - d2, 0.0, 1.0))
        splat(drops, cx, cy, r, fn)
    hgt = 1.0 * streak + 0.6 * normalize(blur(drops, 2.0)) + 0.15 * fbm(rng, n, n, 1.4, 8, 60)
    sx, sy = slopes_of(hgt)
    nrm = encode_normal(sx, sy, 0.145)
    mask = smoothstep(-0.7, 0.7, fbm(rng, n, n, 2.0, 1, 10) + 0.1)
    out = np.zeros((n, n, 4))
    out[..., :2] = nrm[..., :2]
    out[..., 2] = np.clip(mask * 0.97 + 0.0, 0, 1)
    out[..., 3] = remap(fbm(rng, n, n, 1.6, 1, 64), 0.43, 0.22, 0.0, 0.94)
    return out


def gen_noise():
    """Plain per-channel white noise (255 x 255 RGB): gaussian around 0.5, deviation 0.2, like the original.
    No current shader reads it; it is kept so the package keeps the same files."""
    rng = np.random.default_rng(1005)
    return np.clip(rng.normal(0.5, 0.2, (255, 255, 3)), 0, 1)


def void_and_cluster(rng, n=256, sigma=1.9, initial=0.1):
    """Ulichney's void-and-cluster: a dither array whose every threshold is a blue-noise point set.
    Returns ranks 0..n*n-1."""
    yy, xx = np.mgrid[0:n, 0:n]
    dy = np.minimum(yy, n - yy)
    dx = np.minimum(xx, n - xx)
    kern = np.exp(-(dx * dx + dy * dy) / (2.0 * sigma * sigma))
    kf = np.fft.fft2(kern)

    def energy(mask):
        return np.real(np.fft.ifft2(np.fft.fft2(mask) * kf))

    total = n * n
    ones = rng.random((n, n)) < initial
    # make the initial pattern evenly spread: swap tightest cluster into largest void until stable
    e = energy(ones.astype(float))
    for _ in range(total):
        c = np.argmax(np.where(ones, e, -np.inf))
        cy, cx = divmod(c, n)
        ones[cy, cx] = False
        e -= np.roll(np.roll(kern, cy, 0), cx, 1)
        v = np.argmin(np.where(ones, np.inf, e))
        vy, vx = divmod(v, n)
        ones[vy, vx] = True
        e += np.roll(np.roll(kern, vy, 0), vx, 1)
        if v == c:
            break
    rank = np.zeros((n, n), np.int64)
    start = int(ones.sum())
    # phase 1: remove tightest clusters, ranks below the initial count
    p1 = ones.copy()
    e1 = energy(p1.astype(float))
    for r in range(start - 1, -1, -1):
        c = np.argmax(np.where(p1, e1, -np.inf))
        cy, cx = divmod(c, n)
        p1[cy, cx] = False
        e1 -= np.roll(np.roll(kern, cy, 0), cx, 1)
        rank[cy, cx] = r
    # phases 2 and 3: fill largest voids
    p2 = ones.copy()
    e2 = energy(p2.astype(float))
    e2[p2] = np.inf
    for r in range(start, total):
        v = np.argmin(e2)
        vy, vx = divmod(v, n)
        p2[vy, vx] = True
        e2 += np.roll(np.roll(kern, vy, 0), vx, 1)
        e2[vy, vx] = np.inf
        rank[vy, vx] = r
    return rank


def gen_bluenoise():
    """Blue noise, 256 x 256, four independent void-and-cluster channels. Each channel holds every 8-bit value
    exactly 256 times (uniform), sampled point/wrap by AO, AmbientLight, clouds, shadows, Cinema and others."""
    out = np.zeros((256, 256, 4))
    for c in range(4):
        rank = void_and_cluster(np.random.default_rng(2000 + c))
        out[..., c] = (rank // 256) / 255.0
    return out


def gen_water_nrm():
    """Water normal map (1024, wrap). getWaveTexture samples it at three scales and sums xy, so a broad,
    isotropic-enough wave field works best: an ocean spectrum turned into slopes."""
    rng = np.random.default_rng(1007)
    n = 1024
    h0, kx, ky, k = ocean_spectrum(rng, n, peak=7.0, cut=55.0, spread=0.45)
    sx = np.real(np.fft.ifft2(1j * kx * h0))
    sy = np.real(np.fft.ifft2(1j * ky * h0))
    # add a little short chop so the close-up water is not too smooth
    chop = fbm(rng, n, n, 1.5, 30, 140)
    cx, cy = slopes_of(chop)
    sx = normalize(sx) + 0.18 * normalize(cx)
    sy = normalize(sy) + 0.18 * normalize(cy)
    nrm = encode_normal(sx, sy, 0.125)
    return np.concatenate([nrm, np.ones((n, n, 1))], -1)


def gen_water_nrm_lod():
    """32 animation frames of 128 x 128 in a 4096 x 128 strip, each frame tiling, frame 31 looping to frame 0.
    Red/green: gentle wave normal xy. Blue: crest height (mostly low, high on crests) like the original.
    No current shader reads it; it is kept so the package keeps the same files."""
    rng = np.random.default_rng(1008)
    n, frames = 128, 32
    h0, kx, ky, k = ocean_spectrum(rng, n, peak=3.0, cut=40.0, spread=0.4)
    h0m = np.conj(np.roll(np.flip(h0, (0, 1)), (1, 1), (0, 1)))  # conj(h0(-k))
    w0 = 2.0 * math.pi / frames
    omega = w0 * np.maximum(1.0, np.round(np.sqrt(k) * 1.2))  # whole cycles per loop: the strip loops exactly
    strip = np.zeros((n, n * frames, 3))
    slopes, heights = [], []
    for f in range(frames):
        hk = h0 * np.exp(1j * omega * f) + h0m * np.exp(-1j * omega * f)
        slopes.append((np.real(np.fft.ifft2(1j * kx * hk)), np.real(np.fft.ifft2(1j * ky * hk))))
        heights.append(np.real(np.fft.ifft2(hk)))
    sd_s = np.std([s[0] for s in slopes])
    hs = np.array(heights)
    hs = (hs - hs.mean()) / hs.std()
    for f in range(frames):
        nrm = encode_normal(slopes[f][0] / sd_s, slopes[f][1] / sd_s, 0.055)
        strip[:, f * n:(f + 1) * n, :2] = nrm[..., :2]
        strip[:, f * n:(f + 1) * n, 2] = np.clip(0.13 * np.exp(0.95 * hs[f]), 0, 1)
    return strip


def gen_watercalm_nrm():
    """Calm-water normal map (4096, wrap, DXT5) for WATER001/WATER018: long low swells with fine ripples."""
    rng = np.random.default_rng(1009)
    n = 4096
    h0, kx, ky, k = ocean_spectrum(rng, n, peak=6.0, cut=160.0, spread=0.5)
    del k
    sx = np.real(np.fft.ifft2(1j * kx * h0))
    sy = np.real(np.fft.ifft2(1j * ky * h0))
    del h0
    sx, sy = normalize(sx), normalize(sy)
    nrm = encode_normal(sx.astype(np.float32), sy.astype(np.float32), 0.075)
    return np.concatenate([nrm, np.ones((n, n, 1), np.float32)], -1)


def gen_caustics():
    """Underwater caustics (256, wrap, grey; red is read). Made the physical way: light through a tiling
    wave surface, bent by its slope, gathered on the floor. Gives the bright net of focus lines."""
    rng = np.random.default_rng(1010)
    n, m = 256, 2048       # output size, photon grid
    kx, ky = freqs(m, m)
    k = np.sqrt(kx * kx + ky * ky)
    amp = (k >= 3) * np.exp(-((k - 8.0) / 3.5) ** 2)

    f = np.fft.fft2(rng.standard_normal((m, m))) * amp
    tau = 2.0 * math.pi
    gx = np.real(np.fft.ifft2(tau * 1j * kx * f))          # surface slope, height per tile width
    gy = np.real(np.fft.ifft2(tau * 1j * ky * f))
    curv = np.real(np.fft.ifft2(-(tau * kx) ** 2 * f)).std()
    depth = 0.80 / curv                                     # floor depth near the focus of the steeper crests
    ys, xs = (np.mgrid[0:m, 0:m] + 0.5) / m
    px = ((xs + depth * gx) % 1.0) * n
    py = ((ys + depth * gy) % 1.0) * n
    idx = (py.astype(np.int64) % n) * n + (px.astype(np.int64) % n)
    hist = np.bincount(idx.ravel(), minlength=n * n).reshape(n, n).astype(float)
    hist = blur(hist, 1.0)
    c = hist / hist.mean()
    out = np.clip(0.04 + 0.15 * c ** 0.7, 0.04, 1.0)

    out = out - out.mean() + 0.19
    out = np.clip(out, 0.04, 1.0)
    return np.concatenate([np.repeat(out[..., None], 3, -1), np.ones((n, n, 1))], -1)


def gen_ripples():
    """Rain ripples (256, wrap). Per drop: red = 1 at the centre falling linearly to 0 at the rim, green/blue =
    the unit direction from the centre (x right, y up) as 0.5 + 0.5 * dir, alpha = the drop's random phase.
    ComputeRipple (water shaders) and WetRipples (WetWorld) read exactly these. Drops do not overlap."""
    rng = np.random.default_rng(1012)
    n = 256
    out = np.zeros((n, n, 4))
    out[..., 1:3] = 0.5
    taken = np.zeros((n, n), bool)
    yy, xx = np.mgrid[0:n, 0:n] + 0.5
    for rmax in (26, 20, 15, 11, 8, 6):
        for _ in range(900):
            r = rng.uniform(0.7 * rmax, rmax)
            cx, cy = rng.uniform(0, n, 2)
            dx = (xx - cx + n / 2) % n - n / 2
            dy = (yy - cy + n / 2) % n - n / 2
            d = np.sqrt(dx * dx + dy * dy)
            inside = d < r
            if (taken & (d < r + 1.0)).any():
                continue
            taken |= inside
            dd = np.maximum(d[inside], 1e-6)
            out[..., 0][inside] = 1.0 - d[inside] / r
            out[..., 1][inside] = 0.5 + 0.5 * dx[inside] / dd
            out[..., 2][inside] = 0.5 - 0.5 * dy[inside] / dd
            out[..., 3][inside] = rng.random()
    return out


def gen_snow_nrm():
    """Snow normal map (512, wrap, no mips) for SnowAccumulation: fine grain plus soft wind-drift edges."""
    rng = np.random.default_rng(1014)
    n = 512
    grain = fbm(rng, n, n, 1.1, 12, 300)
    lowf = fbm(rng, n, n, 2.0, 2, 20)
    drifts = smoothstep(0.2, 0.45, lowf) + 0.6 * smoothstep(-0.9, -0.6, -lowf)
    hgt = 0.6 * grain + 2.5 * blur(drifts, 3.0) + 0.5 * fbm(rng, n, n, 1.8, 2, 60)

    sx, sy = slopes_of(hgt)
    nrm = encode_normal(sx, sy, 0.041)
    return nrm


def gen_neutral_lut():
    """Identity LUT strip, 256 x 16: 16 slices of 16 x 16; red = x within a slice, green = y, blue = slice.
    Exactly the identity (LUT.cpp skips the pass when a slot holds it)."""
    x = np.arange(256)[None, :]
    y = np.arange(16)[:, None]
    lut = np.zeros((16, 256, 3), np.uint8)
    lut[..., 0] = np.round((x % 16) * 255 / 15)
    lut[..., 1] = np.round(np.broadcast_to(y, (16, 256)) * 255 / 15)
    lut[..., 2] = np.round((x // 16) * 255 / 15)
    return lut


# name: (path under resource/Textures, generator, output kind, mip levels, normal-map mips, header flags, caps)
# kind: png (RGB or RGBA), a8r8g8b8 / r8g8b8 (written here), bc1 / bc3 (texconv from an A8R8G8B8 chain).
# flags/caps repeat the original files' header values so the headers stay as they were.
SPECS = {
    "dirtlens":       ("Effects/dirtlens.png", gen_dirtlens, "png_rgb", 1, False, None, None),
    "bloodlens":      ("Effects/bloodlens.dds", gen_bloodlens, "bc3", 11, False, None, None),
    "Flashlight":     ("Effects/Flashlight.png", gen_flashlight, "png_rgba", 1, False, None, None),
    "water_NRM_lens": ("Effects/water_NRM_lens.dds", gen_water_nrm_lens, "a8r8g8b8", 1, False, 0x100F, 0x1000),
    "noise":          ("Effects/noise.dds", gen_noise, "r8g8b8", 1, False, 0x100F, 0x1000),
    "bluenoise256":   ("Effects/bluenoise256.dds", gen_bluenoise, "a8r8g8b8", 1, False, 0x100F, 0x1000),
    "water_NRM":      ("Water/water_NRM.dds", gen_water_nrm, "r8g8b8", 11, True, 0xA1007, 0x401008),
    "water_NRM_LOD":  ("Water/water_NRM_LOD.dds", gen_water_nrm_lod, "r8g8b8", 1, False, 0x81007, 0x1000),
    "watercalm_NRM":  ("Water/watercalm_NRM.dds", gen_watercalm_nrm, "bc3", 13, True, None, None),
    "caust_001":      ("Water/caust_001.dds", gen_caustics, "bc1", 9, False, None, None),
    "ripples":        ("Precipitations/ripples.dds", gen_ripples, "a8r8g8b8", 9, False, 0xA1007, 0x401008),
    "snow_NRM":       ("Precipitations/snow_NRM.dds", gen_snow_nrm, "r8g8b8", 1, False, 0x100F, 0x1000),
    "neutral_lut":    ("VegasShaders/LUTs/neutral_lut.png", gen_neutral_lut, "lut", 1, False, None, None),
}


def find_texconv(arg):
    for cand in (arg, os.environ.get("TEXCONV"), shutil.which("texconv"), shutil.which("texconv.exe")):
        if cand and os.path.isfile(cand):
            return cand
    return None


def write_one(name, spec, out_root, work, texconv):
    rel, gen, kind, mips, normal, flags, caps = spec
    dst = os.path.join(out_root, *rel.split("/"))
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    img = gen()
    if kind == "lut":
        Image.fromarray(img, "RGB").save(dst, optimize=True)
    elif kind == "png_rgb":
        Image.fromarray(dds.to_u8(img[..., :3]), "RGB").save(dst, optimize=True)
    elif kind == "png_rgba":
        Image.fromarray(dds.to_u8(img), "RGBA").save(dst, optimize=True)
    elif kind in ("a8r8g8b8", "r8g8b8"):
        if img.shape[2] == 3 and kind == "a8r8g8b8":
            img = np.concatenate([img, np.ones(img.shape[:2] + (1,))], -1)
        chain = dds.mip_chain(img, mips, normal)
        dds.write_uncompressed(dst, chain, 32 if kind == "a8r8g8b8" else 24, flags, caps)
    elif kind in ("bc1", "bc3"):
        if not texconv:
            raise SystemExit(f"{name}: texconv not found (pass --texconv or set TEXCONV)")
        chain = dds.mip_chain(img, mips, normal)
        src = os.path.join(work, os.path.basename(dst))
        dds.write_uncompressed(src, chain, 32)
        del chain
        outdir = os.path.join(work, "bc")
        os.makedirs(outdir, exist_ok=True)
        fmt = "BC1_UNORM" if kind == "bc1" else "BC3_UNORM"
        subprocess.run([texconv, "-nologo", "-y", "-dx9", "-f", fmt, "-m", str(mips), "-o", outdir, src],
                       check=True, stdout=subprocess.DEVNULL)
        shutil.move(os.path.join(outdir, os.path.basename(dst)), dst)
        dds.clear_depth(dst)

        os.remove(src)
    else:
        raise ValueError(kind)
    if dst.endswith(".dds"):
        print(f"{name:15s} {dds.describe(dds.read_header(dst))}  -> {rel}")
    else:
        with Image.open(dst) as im:
            print(f"{name:15s} PNG {im.size[0]}x{im.size[1]} {im.mode}  -> {rel}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--only", help="comma-separated names (default: all): " + ", ".join(SPECS))
    ap.add_argument("--out", default=TEX, help="output root (default: resource/Textures)")
    ap.add_argument("--texconv", help="path to texconv.exe")
    ap.add_argument("--work", help="folder for temporary files (default: the system temp folder)")
    args = ap.parse_args()
    names = args.only.split(",") if args.only else list(SPECS)
    for nm in names:
        if nm not in SPECS:
            raise SystemExit(f"unknown texture {nm}")
    texconv = find_texconv(args.texconv)
    work = tempfile.mkdtemp(prefix="gen_textures_", dir=args.work)
    try:
        for nm in names:
            write_one(nm, SPECS[nm], args.out, work, texconv)
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
