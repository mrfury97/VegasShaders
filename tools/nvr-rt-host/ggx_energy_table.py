# UNOFFICIAL 2026-11-07 the GGX single-scattering energy table for RtPathTrace.comp's specular BRDF (PTSpecular; RT-PATH-TRACING.md section 49):
# E(nv, rough) = the integral over the hemisphere of D G1(l) G1(v) / (4 nl nv) x nl (Fresnel 1), the BRDF's albedo for a view at cosine nv,
# rough the perceptual roughness (alpha = max(rough^2, 0.002), SmithG1 as the shader's). Sampled with the visible-normal distribution
# (Heitz 2018), where the estimator is G1(l): 1/E is the multiple-scattering energy compensation (Turquin 2019 / Fdez-Aguera 2019).
# Prints the table (32 nv x 16 rough nodes, nv = i / 31 (0 -> 1e-3), rough = j / 15) for src/core/RtRestirRow.h kGgxEnergy (the CPU writes
# it into the path tracer's frame row: as a GLSL constant array it cost ~12 s of pipeline compile per variant on the AMD driver).
#   py -3.11 -I tools/nvr-rt-host/ggx_energy_table.py > table.txt
import numpy as np

NV, NR, SAMPLES = 32, 16, 400000
rng = np.random.default_rng(20261107)

def g1(x, a2):
    return 2.0 * x / (x + np.sqrt(a2 + (1.0 - a2) * x * x))

def energy(nv, rough):
    a = max(rough * rough, 0.002)
    a2 = a * a
    v = np.array([np.sqrt(max(1.0 - nv * nv, 0.0)), 0.0, nv])
    vh = np.array([a * v[0], a * v[1], v[2]]); vh /= np.linalg.norm(vh)
    lensq = vh[0] ** 2 + vh[1] ** 2
    t1 = np.array([-vh[1], vh[0], 0.0]) / np.sqrt(lensq) if lensq > 0 else np.array([1.0, 0.0, 0.0])
    t2 = np.cross(vh, t1)
    u1, u2 = rng.random(SAMPLES), rng.random(SAMPLES)
    r, phi = np.sqrt(u1), 2.0 * np.pi * u2
    p1, p2 = r * np.cos(phi), r * np.sin(phi)
    s = 0.5 * (1.0 + vh[2])
    p2 = (1.0 - s) * np.sqrt(np.maximum(1.0 - p1 * p1, 0.0)) + s * p2
    pz = np.sqrt(np.maximum(1.0 - p1 * p1 - p2 * p2, 0.0))
    nh = p1[:, None] * t1 + p2[:, None] * t2 + pz[:, None] * vh
    h = np.stack([a * nh[:, 0], a * nh[:, 1], np.maximum(nh[:, 2], 0.0)], 1)
    h /= np.linalg.norm(h, axis=1, keepdims=True)
    vdh = h @ v
    l = 2.0 * vdh[:, None] * h - v
    nl = l[:, 2]
    return float(np.where(nl > 0, g1(np.maximum(nl, 1e-6), a2), 0.0).mean())

rows = []
for j in range(NR):
    rough = j / (NR - 1)
    rows.append([energy(max(i / (NV - 1), 1e-3), rough) for i in range(NV)])
print("const float kGgxEnergy[%d] = float[%d](   // [rough j / %d][nv i / %d]" % (NV * NR, NV * NR, NR - 1, NV - 1))
for j, row in enumerate(rows):
    print("\t" + ", ".join("%.4f" % e for e in row) + ("," if j < NR - 1 else ""))
print(");")
