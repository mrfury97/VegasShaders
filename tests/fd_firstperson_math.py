#!/usr/bin/env python3
"""UNOFFICIAL 2026-11-15 DeferredFirstPerson + GearWeather (docs/next/FD-FIRSTPERSON-RESULTS.md): the CPU checks, no game, no GPU.
Each check mirrors the C++ / HLSL line for line (core/DeferredFirstPerson.h MakeFrame, Shaders/DeferredLamps.hlsl DLPixel, core/GearWeather.h
Step / Rows / Sphere / SphereBox, Shaders/GearWeather.hlsl's bone blend) and prints its worst case:
  1. the lamp pass's ray mapping: the screen share rescaled through DL_View with the world camera's tangents rebuilds exactly the first-person
     ray, and its 64-pixel tile is the world camera's tile of the same direction (the froxel lists' tiles);
  2. the lamps' rows: lamp + kLightingOffset - eye (what the forward first-person draws get) is the lamp's world-camera-relative position while
     the first-person eye sits at the world eye + the offset (the eye check), and the error is exactly the eye's distance otherwise;
  3. the first-person projection's depth terms (_33, _43) against a D3D projection of the frustum, both depth conventions;
  4. GearWeather's build-up and fade times (each layer's documented times) and their independence from the frame rate;
  5. the gear-local coordinate: a surface point of a part keeps its gear-local position whatever the part's pose (a reload, a walk), while
     its screen / camera-relative position moves; a skinned part's two-bone blend is continuous;
  6. a part's screen box holds every point of its bound sphere.
Usage: py -3.11 tests/fd_firstperson_math.py    ->  FD FIRSTPERSON MATH: PASS (exit 0) or FAIL (exit 1)
"""
import math, random, sys

random.seed(1115)
fails = []
def check(name, ok, detail):
    print("%-70s %s  %s" % (name, "ok  " if ok else "FAIL", detail))
    if not ok: fails.append(name)

def norm(v):
    l = math.sqrt(sum(x * x for x in v)); return [x / l for x in v]
def dot(a, b): return sum(x * y for x, y in zip(a, b))
def add(a, b): return [x + y for x, y in zip(a, b)]
def sub(a, b): return [x - y for x, y in zip(a, b)]
def mul(a, s): return [x * s for x in a]
def cross(a, b): return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]

def random_rot():
    f = norm([random.uniform(-1, 1) for _ in range(3)])
    t = norm(cross(f, norm([random.uniform(-1, 1) for _ in range(3)])))
    u = cross(t, f)
    return f, u, t	# forward, up, side (the camera's columns 0, 1, 2)

# ---- 1. the lamp pass's ray mapping (DLPixel) with DeferredFirstPerson::MakeFrame's constants
worst_ray = 0.0; worst_tile = 0
for case in range(400):
    fwd, up, right = random_rot()
    vp = (random.choice([0, 0, 8]), random.choice([0, 0, 4]), random.choice([1920, 1280, 960, 2560]), None)
    vpW = vp[2]; vpH = int(vpW * random.choice([9 / 16, 10 / 16, 3 / 4])); vpX, vpY = vp[0], vp[1]
    aspect = vpW / vpH
    world_vfov = math.radians(random.uniform(55, 100)); fp_vfov = math.radians(random.uniform(40, 90))
    tanWY = math.tan(world_vfov / 2); tanWX = tanWY * aspect
    tanFY = math.tan(fp_vfov / 2); tanFX = tanFY * aspect	# (the frustum's half width = tan of half the fov)
    kx, ky = tanFX / tanWX, tanFY / tanWY
    c0 = (kx / vpW, ky / vpH, vpX - 0.5 * (1 - kx) * vpW / kx, vpY - 0.5 * (1 - ky) * vpH / ky)
    tilesX, tilesY = math.ceil(vpW / 64), math.ceil(vpH / 64)	# (DL_Debug.zw: the lists' viewport, here the same size)
    for _ in range(50):
        px, py = vpX + random.uniform(0, vpW - 1), vpY + random.uniform(0, vpH - 1)
        px, py = math.floor(px), math.floor(py)
        # the first-person ray (DDRay with the first-person frame, symmetric: no shift)
        lfp = ((px - vpX + 0.5) / vpW, (py - vpY + 0.5) / vpH)
        ray_fp = add(add(fwd, mul(right, (lfp[0] * 2 - 1) * tanFX)), mul(up, (1 - lfp[1] * 2) * tanFY))
        # the lamp pass with the view-model constants: local = (vpos - c0.zw + 0.5) * c0.xy, then the world's tangents
        loc = ((px - c0[2] + 0.5) * c0[0], (py - c0[3] + 0.5) * c0[1])
        ray = add(add(fwd, mul(right, (loc[0] * 2 - 1) * tanWX)), mul(up, (1 - loc[1] * 2) * tanWY))
        worst_ray = max(worst_ray, max(abs(a - b) for a, b in zip(ray, ray_fp)))
        # the tile the pass reads against the world camera's tile of the same direction (a point along the ray, projected)
        p = mul(ray_fp, random.uniform(5, 60))
        z = dot(p, fwd); wx = dot(p, right) / (z * tanWX); wy = dot(p, up) / (z * tanWY)
        lw = ((wx + 1) * 0.5, (1 - wy) * 0.5)
        sat = lambda v: min(max(v, 0.0), 1.0)
        tile_pass = (math.floor(sat(loc[0]) * tilesX), math.floor(sat(loc[1]) * tilesY))
        tile_world = (math.floor(sat(lw[0]) * tilesX), math.floor(sat(lw[1]) * tilesY))
        if tile_pass != tile_world:
            # only a float tie exactly on a tile edge may differ
            edge = min(abs(sat(lw[0]) * tilesX - round(sat(lw[0]) * tilesX)), abs(sat(lw[1]) * tilesY - round(sat(lw[1]) * tilesY)))
            if edge > 1e-6: worst_tile += 1
check("1. ray mapping: the rebuilt ray = the first-person ray (max |diff|)", worst_ray < 1e-9, "%.2e" % worst_ray)
check("1. ray mapping: tile = the world camera's tile of the direction (misses)", worst_tile == 0, "%d of 20000" % worst_tile)

# ---- 2. the lamps' rows (DL_Rows translation = kLightingOffset - eye)
worst_same = 0.0; worst_err = 0.0
for _ in range(2000):
    world_eye = [random.uniform(-1e5, 1e5) for _ in range(3)]
    offset = mul(world_eye, -1.0)	# 0x00874C10: -(the player's position); any value works
    offset = add(offset, [random.uniform(-200, 200) for _ in range(3)])
    lamp = add(world_eye, [random.uniform(-800, 800) for _ in range(3)])
    eye = add(world_eye, offset)	# the first-person eye in its own space
    rel = add(lamp, sub(offset, eye))	# LightCommonLamp: (lamp + 0) . rows, rows' w = offset - eye
    truth = sub(lamp, world_eye)
    worst_same = max(worst_same, max(abs(a - b) for a, b in zip(rel, truth)))
    d = [random.uniform(-2, 2) for _ in range(3)]	# an eye off the world eye (the check allows 2 units)
    rel_d = add(lamp, sub(offset, add(eye, d)))
    worst_err = max(worst_err, abs(math.sqrt(dot(sub(rel_d, truth), sub(rel_d, truth))) - math.sqrt(dot(d, d))))
check("2. lamp rows: lamp + offset - eye = lamp - world eye (max |diff|, units)", worst_same < 1e-6, "%.2e" % worst_same)
check("2. lamp rows: with the eye off by d the error is |d| (max deviation)", worst_err < 1e-6, "%.2e" % worst_err)

# ---- 3. the depth terms (MakeFrame proj[0], proj[1]) against the D3D projection of the frustum
worst_depth = 0.0
for reversed_ in (True, False):
    for _ in range(500):
        n = random.uniform(1, 10); f = random.uniform(1000, 400000)
        step = 1 / (f - n)
        p33 = -(n * step) if reversed_ else f * step
        p43 = n * f * step if reversed_ else -(n * f * step)
        # D3D (row vector): clip z = z p33 + p43, w = z  ->  depth = p33 + p43 / z
        for z in (n, f, random.uniform(n, 100), random.uniform(n, f)):
            depth = p33 + p43 / z
            want = (f / z - 1) * n / (f - n) if reversed_ else (1 - n / z) * f / (f - n)	# reversed: 1 at near, 0 at far
            worst_depth = max(worst_depth, abs(depth - want))
check("3. depth terms: _33 + _43 / z against the frustum's depth (max |diff|)", worst_depth < 1e-9, "%.2e" % worst_depth)

# ---- 4. GearWeather::Step (core/GearWeather.h, line for line)
def clamp01(v): return min(max(v, 0.0), 1.0)
def step(l, inp, dt, speed):
    t = (min(dt, 0.5) if dt > 0 else 0.0) * max(speed, 0.0)
    out = inp["exterior"] and not inp["underwater"]
    rain = clamp01(inp["rain"]) if out else 0.0
    wet, frost, sand, mud = l
    if inp["underwater"]: wet += t * 2.0
    elif rain > 0.01: wet += t * rain / 20.0
    else: wet -= t * (1 / 180.0 if out else 1 / 90.0)
    snow = clamp01(inp["snow"]) if out else 0.0
    if snow > 0.01: frost += t * snow / 60.0
    else: frost -= t * (1 / 5.0 if inp["underwater"] else 1 / 45.0 if not inp["exterior"] else 1 / 30.0 if rain > 0.01 else 1 / 240.0)
    storm = clamp01(inp["storm"]) if out else 0.0
    dust = clamp01((inp["wind"] - 0.5) * 2.0) if out and rain <= 0.01 else 0.0
    if storm > 0.01 or dust > 0.01: sand += t * (storm / 30.0 + dust / 180.0)
    else: sand -= t * (1 / 5.0 if inp["underwater"] else 1 / 40.0 if rain > 0.01 else 1 / 600.0 if not inp["exterior"] else 1 / 300.0)
    splash = clamp01(inp["ground"]) * clamp01(inp["moving"]) if out else 0.0
    if splash > 0.01: mud += t * splash / 45.0
    else: mud -= t * (1 / 5.0 if inp["underwater"] else 1 / 360.0 if not inp["exterior"] else 1 / 600.0)
    return (clamp01(wet), clamp01(frost), clamp01(sand), clamp01(mud))

def base(**k):
    d = dict(rain=0.0, ground=0.0, snow=0.0, storm=0.0, wind=0.0, moving=0.0, exterior=True, underwater=False); d.update(k); return d

def time_to(level0, inp, layer, target, fps=60.0, speed=1.0, limit=3600.0):
    l = level0; t = 0.0; dt = 1.0 / fps
    while t < limit:
        l = step(l, inp, dt, speed); t += dt
        if (target >= 1.0 and l[layer] >= 1.0) or (target <= 0.0 and l[layer] <= 0.0): return t
    return float("inf")

cases = [  # (name, start, inputs, layer, target, documented seconds)
    ("wet: up in full rain outdoors", (0, 0, 0, 0), base(rain=1.0, ground=1.0), 0, 1, 20),
    ("wet: dries outdoors", (1, 0, 0, 0), base(), 0, 0, 180),
    ("wet: dries indoors", (1, 0, 0, 0), base(exterior=False), 0, 0, 90),
    ("frost: up in a snow weather", (0, 0, 0, 0), base(snow=1.0), 1, 1, 60),
    ("frost: melts indoors", (0, 1, 0, 0), base(exterior=False), 1, 0, 45),
    ("frost: melts outdoors without snow", (0, 1, 0, 0), base(), 1, 0, 240),
    ("frost: melts in rain", (0, 1, 0, 0), base(rain=1.0), 1, 0, 30),
    ("sand: up in a full storm", (0, 0, 0, 0), base(storm=1.0), 2, 1, 30),
    ("sand: up in full dry wind alone", (0, 0, 0, 0), base(wind=1.0), 2, 1, 180),
    ("sand: fades outdoors", (0, 0, 1, 0), base(), 2, 0, 300),
    ("sand: fades indoors", (0, 0, 1, 0), base(exterior=False), 2, 0, 600),
    ("sand: rain washes it", (0, 0, 1, 0), base(rain=1.0), 2, 0, 40),
    ("mud: up running through the wet", (0, 0, 0, 0), base(rain=1.0, ground=1.0, moving=1.0), 3, 1, 45),
    ("mud: dries outdoors", (0, 0, 0, 1), base(), 3, 0, 600),
    ("mud: dries indoors", (0, 0, 0, 1), base(exterior=False), 3, 0, 360),
    ("underwater washes the mud", (0, 0, 0, 1), base(underwater=True), 3, 0, 5),
]
worst_rel = 0.0
for name, l0, inp, layer, target, want in cases:
    got = time_to(l0, inp, layer, target)
    rel = abs(got - want) / want
    worst_rel = max(worst_rel, rel)
    check("4. " + name + " (s)", rel < 0.01, "%.1f s (documented %d s)" % (got, want))
# no mud standing still in the rain, none indoors, none in the dry
l = (0, 0, 0, 0)
for _ in range(60 * 120): l = step(l, base(rain=1.0, ground=1.0, moving=0.0), 1 / 60.0, 1.0)
check("4. standing still in the rain makes no mud", l[3] == 0.0, "mud %.3f after 120 s" % l[3])
# frame-rate independence: 30 / 144 fps and a 2 s hitch every 10 s (clamped to 0.5 s: the hitch counts as half a second)
a = time_to((0, 0, 0, 0), base(rain=1.0), 0, 1, fps=30); b = time_to((0, 0, 0, 0), base(rain=1.0), 0, 1, fps=144)
check("4. wet build-up at 30 and 144 fps (s)", abs(a - b) < 0.1, "%.2f / %.2f" % (a, b))
l = (0, 0, 0, 0); t = 0.0
for i in range(10000):
    dt = 2.0 if i % 600 == 599 else 1 / 60.0
    l = step(l, base(rain=0.5), dt, 1.0); t += dt
    if l[0] >= 1.0: break
check("4. a long frame counts as half a second (no jump)", 30.0 < t < 45.0, "%.1f s at half rain with hitches" % t)

# ---- 5. the gear-local coordinate (GearWeather::Rows / Sphere, the shader's dot(rows, (P, 1)))
def mat_from(f, u, s): return [[f[0], u[0], s[0]], [f[1], u[1], s[1]], [f[2], u[2], s[2]]]	# columns
def mat_vec(R, v): return [R[0][0] * v[0] + R[0][1] * v[1] + R[0][2] * v[2], R[1][0] * v[0] + R[1][1] * v[1] + R[1][2] * v[2], R[2][0] * v[0] + R[2][1] * v[1] + R[2][2] * v[2]]
def rows(R, pos, scale, eye):
    inv = 1.0 / scale if scale else 1.0
    shift = sub(eye, pos)
    return [[R[0][i] * inv, R[1][i] * inv, R[2][i] * inv, (R[0][i] * shift[0] + R[1][i] * shift[1] + R[2][i] * shift[2]) * inv] for i in range(3)]
def apply_rows(r, P): return [r[i][0] * P[0] + r[i][1] * P[1] + r[i][2] * P[2] + r[i][3] for i in range(3)]

worst_local = 0.0; min_moved = 1e9
for _ in range(2000):
    eye = [random.uniform(-50, 50) for _ in range(3)]
    l = [random.uniform(-20, 20) for _ in range(3)]	# a point of the part's mesh (its own space)
    poses = []
    for k in range(2):	# two poses: before and during a reload
        R = mat_from(*random_rot()); pos = add(eye, [random.uniform(-40, 40) for _ in range(3)]); s = random.uniform(0.8, 1.2)
        world = add(mat_vec(R, mul(l, s)), pos)	# NiTransform: R (s l) + pos
        P = sub(world, eye)	# camera-relative, as the pass rebuilds it from the G-buffer's distance
        got = apply_rows(rows(R, pos, s, eye), P)
        worst_local = max(worst_local, max(abs(a - b) for a, b in zip(got, l)))
        poses.append(P)
    min_moved = min(min_moved, math.sqrt(dot(sub(poses[0], poses[1]), sub(poses[0], poses[1]))))
check("5. gear-local: the point's own position back from (P, 1) (max |diff|, units)", worst_local < 1e-9, "%.2e" % worst_local)
check("5. gear-local: the same point in two poses reads the same noise input", worst_local < 1e-9, "camera-relative moved >= %.2f units" % min_moved)

def blend_w1(d0, d1): return 0.0 if d1 > 1e8 else clamp01(0.5 - (d1 - d0) / max(2.0 * (d0 + d1), 0.5))
jump = 0.0
for _ in range(2000):	# walk across the halfway between two bones: the weight of the second never jumps
    a = random.uniform(0, 10); prev = None
    for i in range(201):
        x = i / 200.0
        d0, d1 = (a * x, a * (1 - x)) if x < 0.5 else (a * (1 - x), a * x)	# the nearer is always d0
        w = blend_w1(d0, d1) if x < 0.5 else 1.0 - blend_w1(d0, d1)		# the share of bone "B" (the right one)
        if prev is not None: jump = max(jump, abs(w - prev))
        prev = w
check("5. skinned: the two-bone blend is continuous across bones (largest step of 1/200)", jump < 0.05, "%.4f" % jump)

# ---- 6. a part's screen box (GearWeather::SphereBox) holds its bound sphere
def sphere_box(s, frame, vp):
    right, up, fwd, tx, ty = frame
    c = s[:3]; r = s[3]
    x, y, z = dot(c, right), dot(c, up), dot(c, fwd)
    vpX, vpY, vpW, vpH = vp
    if z - r < 0.5: return (vpX, vpY, vpX + vpW, vpY + vpH)
    near, far = z - r, z + r
    nx0, nx1 = min((x - r) / near, (x - r) / far) / tx, max((x + r) / near, (x + r) / far) / tx
    ny0, ny1 = min((y - r) / near, (y - r) / far) / ty, max((y + r) / near, (y + r) / far) / ty
    lx0, lx1 = (nx0 + 1) * 0.5, (nx1 + 1) * 0.5
    ly0, ly1 = (1 - ny1) * 0.5, (1 - ny0) * 0.5
    if lx1 < 0 or lx0 > 1 or ly1 < 0 or ly0 > 1: return None
    return (math.floor(vpX + clamp01(lx0) * vpW) - 1, math.floor(vpY + clamp01(ly0) * vpH) - 1,
            math.ceil(vpX + clamp01(lx1) * vpW) + 1, math.ceil(vpY + clamp01(ly1) * vpH) + 1)
outside = 0; tested = 0
for _ in range(3000):
    fwd, up, right = random_rot()
    vp = (0, 0, 1920, 1080); ty = math.tan(math.radians(random.uniform(40, 80)) / 2); tx = ty * 1920 / 1080
    frame = (right, up, fwd, tx, ty)
    r = random.uniform(0.5, 15)
    c = add(mul(fwd, random.uniform(r + 1, 80)), add(mul(right, random.uniform(-30, 30)), mul(up, random.uniform(-20, 20))))
    box = sphere_box(c + [r], frame, vp)
    for _ in range(40):
        p = add(c, mul(norm([random.uniform(-1, 1) for _ in range(3)]), r * random.uniform(0, 1)))
        z = dot(p, fwd)
        sx = (dot(p, right) / (z * tx) + 1) * 0.5 * 1920; sy = (1 - dot(p, up) / (z * ty)) * 0.5 * 1080
        if not (0 <= sx < 1920 and 0 <= sy < 1080): continue
        tested += 1
        if box is None or not (box[0] <= sx <= box[2] and box[1] <= sy <= box[3]): outside += 1
check("6. screen box: every on-screen point of the sphere inside it", outside == 0, "%d outside of %d" % (outside, tested))

print("FD FIRSTPERSON MATH: %s" % ("PASS" if not fails else "FAIL (" + ", ".join(fails) + ")"))
sys.exit(1 if fails else 0)
