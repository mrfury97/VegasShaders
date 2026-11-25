# bridge_lobe_fit.py (2026-11-05, VanillaBridge: Shaders/Includes/Object.hlsl BridgeSetup, docs/EFFECTS.md "PBR")
# py -3.11 -I tests/furnace/bridge_lobe_fit.py   (numpy)
#
# 1. The game's highlight mask x (N.H)^n against NVR's GGX highlight (PBR.hlsl: GGX D with alpha = roughness^2, Karis' separable
#    Schlick-Smith G, x N.L x pi), lit and seen head-on with the normal tilted (L = V, N.L = N.V = N.H = cos t), as a function of the
#    tilt. For alpha^2 = k / (n + 2) with the peak matched (F0 = 1 / GGX peak), it prints the half width and energy (over the
#    hemisphere of normals) relative to Blinn-Phong's, for k = 2 (used), the k that matches the energy, and the k of the least-
#    squares shape fit. Also F0 = 1 / peak against 8 / (n + 2) (identical: the peak is F0 / (4 alpha^2)).
# 2. Linear lighting: the game's gamma-space sum (albedo + mask) x light against encode(decode(albedo) + s) with the highlight's
#    strength s = mask (used), mask^2.2 or mask^1.1, over albedo 0.05-0.6 and mask 0.05-1 (light 1, surface facing it).
import numpy as np
PI = np.pi

def ggx(nh, a2):
    d = (nh * a2 - nh) * nh + 1
    return a2 / (PI * d * d)

def g1(nx, r):
    k = (r + 1) ** 2 / 8
    return nx / (nx * (1 - k) + k)

def nvr_highlight(c, r):  # per unit light, F = 1
    return ggx(c, r ** 4) * g1(c, r) ** 2 / (4 * c * c) * c * PI

t = np.linspace(0, PI / 2 - 1e-4, 100001)
w = 2 * PI * np.sin(t)

def stats(f):
    return np.degrees(t[np.argmax(f < f[0] / 2)]), np.trapezoid(f * w, t)

print("1. lobe: n | F0 for peak, 8/(n+2) | k=2: half width, energy | energy-match k, half width | L2-fit k, energy, half width")
for n in [2, 5, 10, 20, 30, 50, 100, 200]:
    v = np.cos(t) ** n
    hv, ev = stats(v)
    rows = []
    for k in np.linspace(1.0, 3.0, 81):
        g = nvr_highlight(np.cos(t), (k / (n + 2)) ** 0.25)
        f0 = 1 / g[0]
        g = g * f0
        h, e = stats(g)
        rows.append((k, e / ev, h / hv, np.trapezoid((g - v) ** 2 * w, t), f0))
    rows = np.array(rows)
    i2 = np.argmin(abs(rows[:, 0] - 2)); ie = np.argmin(abs(rows[:, 1] - 1)); il = np.argmin(rows[:, 3])
    print(f"  {n:4d} | {rows[i2, 4]:.3f} {8 / (n + 2):.3f} | {rows[i2, 2]:.2f} {rows[i2, 1]:.2f} | {rows[ie, 0]:.2f} {rows[ie, 2]:.2f} | "
          f"{rows[il, 0]:.2f} {rows[il, 1]:.2f} {rows[il, 2]:.2f}")

print("2. linear lighting, display error of the highlighted surface (0-1 scale): mean, mean |.|")
g = 2.2
a, m = np.meshgrid(np.linspace(0.05, 0.6, 56), np.linspace(0.05, 1.0, 96))
game = np.minimum(a + m, 1.0)
for name, s in [("s = mask", m), ("s = mask^2.2", m ** g), ("s = mask^1.1", m ** (g / 2))]:
    err = np.minimum((a ** g + s) ** (1 / g), 1.0) - game
    print(f"  {name:14s} {err.mean():+.3f} {abs(err).mean():.3f}")
