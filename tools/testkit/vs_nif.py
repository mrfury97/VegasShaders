# Small writers for the Vegas Shaders test kit (2026-11-07): Fallout New Vegas static models (.nif 20.2.0.7, user 11, Bethesda 34) and
# uncompressed DDS textures, from geometry made here (no vanilla geometry is copied). Used by build_rtroom.py.
#
# A model is one BSFadeNode root with NiTriShape children; each shape: NiTriShapeData (positions, normals, tangents, one UV set,
# triangles), BSShaderPPLightingProperty + BSShaderTextureSet (6 paths: diffuse, normal, glow, height, environment, environment mask),
# NiMaterialProperty, and an optional NiAlphaProperty. The block layouts are the ones vanilla FNV statics use (checked field by field
# against nv_ul_wallscreen01.nif and windowsinglegglow.nif). No collision: the objects are walked through (the room uses the game's
# CollisionMarker where it needs a wall).
#   Conventions (as vanilla): z up; a triangle (a, b, c) faces where cross(b - a, c - a) points; UV v = 0 at the texture's top;
#   the "Tangents" array holds dP/dv and "Bitangents" dP/du (Bethesda's swap, as windowsinglegglow.nif stores them).
import math, struct

NIF_HEADER = b"Gamebryo File Format, Version 20.2.0.7\n"
SF_SPECULAR, SF_ALPHA_TEXTURE, SF_REMAPPABLE, SF_ZTEST = 0x1, 0x100, 0x02000000, 0x80000000
SF2_ZWRITE = 0x1
ALPHA_BLEND = 0x00ED            # blend on, src SRC_ALPHA, dst INV_SRC_ALPHA, no test (vanilla glass / soft cards)


def _sized(s):
    b = s.encode("latin-1")
    return struct.pack("<I", len(b)) + b


class Shape:
    def __init__(self, name, verts, normals, uvs, tris, textures, shader_flags=SF_SPECULAR | SF_REMAPPABLE | SF_ZTEST,
                 shader_flags2=SF2_ZWRITE, alpha=None, emissive=(0.0, 0.0, 0.0), emit_mult=1.0, glossiness=10.0,
                 specular=(0.0, 0.0, 0.0), mat_alpha=1.0):
        assert len(verts) == len(normals) == len(uvs) and len(verts) < 65536 and len(tris) < 65536
        self.name, self.verts, self.normals, self.uvs, self.tris = name, verts, normals, uvs, tris
        self.textures = (list(textures) + [""] * 6)[:6]
        self.shader_flags, self.shader_flags2, self.alpha = shader_flags, shader_flags2, alpha
        self.emissive, self.emit_mult, self.glossiness, self.specular, self.mat_alpha = emissive, emit_mult, glossiness, specular, mat_alpha


def _sub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def _cross(a, b): return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
def _dot(a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
def _norm(a):
    l = math.sqrt(_dot(a, a))
    return (a[0] / l, a[1] / l, a[2] / l) if l > 1e-12 else (0.0, 0.0, 1.0)


def tangents(verts, normals, uvs, tris):
    """Per vertex dP/du and dP/dv (accumulated over its triangles, made perpendicular to the normal)."""
    tu = [[0.0, 0.0, 0.0] for _ in verts]; tv = [[0.0, 0.0, 0.0] for _ in verts]
    for a, b, c in tris:
        e1, e2 = _sub(verts[b], verts[a]), _sub(verts[c], verts[a])
        du1, dv1 = uvs[b][0] - uvs[a][0], uvs[b][1] - uvs[a][1]
        du2, dv2 = uvs[c][0] - uvs[a][0], uvs[c][1] - uvs[a][1]
        det = du1 * dv2 - du2 * dv1
        if abs(det) < 1e-12: continue
        r = 1.0 / det
        pu = [(e1[k] * dv2 - e2[k] * dv1) * r for k in range(3)]
        pv = [(e2[k] * du1 - e1[k] * du2) * r for k in range(3)]
        for i in (a, b, c):
            for k in range(3): tu[i][k] += pu[k]; tv[i][k] += pv[k]
    T, B = [], []
    for i, n in enumerate(normals):
        u = tuple(tu[i]); v = tuple(tv[i])
        u = _norm(_sub(u, tuple(_dot(n, u) * x for x in n)))
        v = _norm(_sub(v, tuple(_dot(n, v) * x for x in n)))
        T.append(u); B.append(v)
    return T, B


def _bound(verts):
    lo = [min(v[k] for v in verts) for k in range(3)]; hi = [max(v[k] for v in verts) for k in range(3)]
    c = tuple((lo[k] + hi[k]) * 0.5 for k in range(3))
    return c, max(math.sqrt(_dot(_sub(v, c), _sub(v, c))) for v in verts)


def _geom_data(s):
    du, dv = tangents(s.verts, s.normals, s.uvs, s.tris)
    nv = len(s.verts)
    out = struct.pack("<iHBBB", 0, nv, 0, 0, 1)
    out += b"".join(struct.pack("<3f", *v) for v in s.verts)
    out += struct.pack("<HB", 0x1001, 1)                                   # one UV set + tangents; has normals
    out += b"".join(struct.pack("<3f", *n) for n in s.normals)
    out += b"".join(struct.pack("<3f", *t) for t in dv)                    # "Tangents" = dP/dv (Bethesda)
    out += b"".join(struct.pack("<3f", *t) for t in du)                    # "Bitangents" = dP/du
    c, r = _bound(s.verts)
    out += struct.pack("<4f", c[0], c[1], c[2], r) + b"\0"                 # bound; no vertex colours
    out += b"".join(struct.pack("<2f", *uv) for uv in s.uvs)
    out += struct.pack("<Hi", 0x4000, -1)                                  # consistency CT_STATIC, no additional data
    out += struct.pack("<HIB", len(s.tris), 3 * len(s.tris), 1) + b"".join(struct.pack("<3H", *t) for t in s.tris)
    out += struct.pack("<H", 0)                                            # no match groups
    return out


def _av(name_idx, nextra_refs, props, collision=-1, flags=0x0008000E):
    out = struct.pack("<iI", name_idx, len(nextra_refs)) + b"".join(struct.pack("<i", r) for r in nextra_refs)
    out += struct.pack("<iI", -1, flags)
    out += struct.pack("<3f9ff", 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1.0)
    out += struct.pack("<I", len(props)) + b"".join(struct.pack("<i", p) for p in props) + struct.pack("<i", collision)
    return out


def nif_bytes(root_name, shapes, bsx=None):
    for k, s in enumerate(shapes):
        if s.name == root_name: s.name = "%s:%d" % (root_name, k)      # vanilla exports name their shapes "<root>:<n>"
    strings = [root_name] + (["BSX"] if bsx is not None else []) + [s.name for s in shapes]
    blocks = []                                        # (type, bytes); refs are block indices
    nb = 1 + (1 if bsx is not None else 0) + sum(5 + (1 if s.alpha is not None else 0) for s in shapes)
    idx = 1 + (1 if bsx is not None else 0)
    shape_first = []
    for s in shapes: shape_first.append(idx); idx += 5 + (1 if s.alpha is not None else 0)
    root = _av(0, [1] if bsx is not None else [], []) + struct.pack("<I", len(shapes)) + b"".join(struct.pack("<i", f) for f in shape_first) + struct.pack("<I", 0)
    blocks.append(("BSFadeNode", root))
    if bsx is not None: blocks.append(("BSXFlags", struct.pack("<iI", 1, bsx)))
    for k, s in enumerate(shapes):
        f = shape_first[k]; name_idx = strings.index(s.name, 1 + (1 if bsx is not None else 0))
        data_i, pp_i, ts_i, mat_i = f + 1, f + 2, f + 3, f + 4
        props = ([f + 5] if s.alpha is not None else []) + [pp_i, mat_i]
        tri = _av(name_idx, [], props) + struct.pack("<ii", data_i, -1) + struct.pack("<iiB", 0, 0, 0)   # data, skin; no materials
        blocks.append(("NiTriShape", tri))
        blocks.append(("NiTriShapeData", _geom_data(s)))
        pp = struct.pack("<iIi", -1, 0, -1) + struct.pack("<HIIIf", 1, 1, s.shader_flags, s.shader_flags2, 1.0) + struct.pack("<I", 3)
        pp += struct.pack("<iffff", ts_i, 0.0, 0.0, 4.0, 1.0)              # texture set, refraction, fire period, parallax passes / scale
        blocks.append(("BSShaderPPLightingProperty", pp))
        blocks.append(("BSShaderTextureSet", struct.pack("<i", 6) + b"".join(_sized(t) for t in s.textures)))
        mat = struct.pack("<iIi", -1, 0, -1) + struct.pack("<3f3ffff", *s.specular, *s.emissive, s.glossiness, s.mat_alpha, s.emit_mult)
        blocks.append(("NiMaterialProperty", mat))
        if s.alpha is not None:
            fl, ref = s.alpha
            blocks.append(("NiAlphaProperty", struct.pack("<iIiHB", -1, 0, -1, fl, ref)))
    assert len(blocks) == nb
    types = []
    for t, _ in blocks:
        if t not in types: types.append(t)
    h = NIF_HEADER + struct.pack("<IBII", 0x14020007, 1, 11, len(blocks)) + struct.pack("<I", 34)
    for s in ("Vegas Shaders", "Do Nothing", "vs_nif.py"):
        b = s.encode() + b"\0"; h += bytes([len(b)]) + b
    h += struct.pack("<H", len(types)) + b"".join(_sized(t) for t in types)
    h += b"".join(struct.pack("<H", types.index(t)) for t, _ in blocks)
    h += b"".join(struct.pack("<I", len(b)) for _, b in blocks)
    h += struct.pack("<II", len(strings), max(len(x) for x in strings)) + b"".join(_sized(x) for x in strings)
    h += struct.pack("<I", 0)                                              # no groups
    return h + b"".join(b for _, b in blocks) + struct.pack("<Ii", 1, 0)   # one root: block 0


# ---------- geometry ----------
def sphere(r, seg=48, rings=24, z0=0.0):
    """UV sphere round (0, 0, z0 + r): its bottom touches z0. u = longitude, v = 0 at the top."""
    V, N, U, T = [], [], [], []
    for j in range(rings + 1):
        th = math.pi * j / rings
        for i in range(seg + 1):
            ph = 2 * math.pi * i / seg
            n = (math.sin(th) * math.cos(ph), math.sin(th) * math.sin(ph), math.cos(th))
            V.append((r * n[0], r * n[1], z0 + r + r * n[2])); N.append(n); U.append((i / seg, j / rings))
    w = seg + 1
    for j in range(rings):
        for i in range(seg):
            a, b, c, d = j * w + i, j * w + i + 1, (j + 1) * w + i + 1, (j + 1) * w + i
            if j != 0: T.append(_face((a, d, b), V, N))
            if j != rings - 1: T.append(_face((b, d, c), V, N))
    return V, N, U, T


def _face(t, V, N):
    """t wound so that it faces along its vertices' normals."""
    a, b, c = t
    n = _cross(_sub(V[b], V[a]), _sub(V[c], V[a]))
    avg = tuple(N[a][k] + N[b][k] + N[c][k] for k in range(3))
    return t if _dot(n, avg) >= 0 else (a, c, b)


def quad(tl, tr, br, bl, uvf=None, facing=None):
    """A flat quad given by its corners as seen from its front (top left, top right, bottom right, bottom left); it faces the viewer.
    uvf(p) -> (u, v), default (0, 0) top left .. (1, 1) bottom right. facing: the direction it must face (the corners are mirrored
    left-right when they would face away)."""
    n = _norm(_cross(_sub(bl, tl), _sub(tr, tl)))
    if facing is not None and _dot(n, facing) < 0:
        tl, tr, br, bl = tr, tl, bl, br
        n = (-n[0], -n[1], -n[2])
    P = [tl, tr, br, bl]
    U = [uvf(p) for p in P] if uvf else [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)]
    return P, [n] * 4, U, [(0, 3, 2), (0, 2, 1)]


def merge(*parts):
    V, N, U, T = [], [], [], []
    for v, n, u, t in parts:
        o = len(V); V += v; N += n; U += u; T += [(a + o, b + o, c + o) for a, b, c in t]
    return V, N, U, T


def rect_x(x0, x1, y, z0, z1, facing_y, uvf=None):
    """A rectangle in the plane y = const (x0..x1, z0..z1), facing -y (facing_y < 0) or +y."""
    return quad((x0, y, z1), (x1, y, z1), (x1, y, z0), (x0, y, z0), uvf, (0.0, facing_y, 0.0))


def rect_z(x0, x1, y0, y1, z, facing_z, uvf=None):
    """A rectangle in the plane z = const, facing -z or +z."""
    return quad((x0, y1, z), (x1, y1, z), (x1, y0, z), (x0, y0, z), uvf, (0.0, 0.0, facing_z))


def rect_y(x, y0, y1, z0, z1, facing_x, uvf=None):
    """A rectangle in the plane x = const, facing -x or +x."""
    return quad((x, y0, z1), (x, y1, z1), (x, y1, z0), (x, y0, z0), uvf, (facing_x, 0.0, 0.0))


def check_facing(V, N, T):
    """Every triangle faces along its vertices' normals (the winding check vanilla meshes pass); returns the count that do not."""
    bad = 0
    for a, b, c in T:
        n = _cross(_sub(V[b], V[a]), _sub(V[c], V[a]))
        if _dot(n, tuple(N[a][k] + N[b][k] + N[c][k] for k in range(3))) <= 0: bad += 1
    return bad


# ---------- DDS (uncompressed A8R8G8B8, optional box-filtered mip chain) ----------
def dds_bytes(w, h, pixel, mips=True):
    """pixel(x, y) -> (r, g, b, a) 0-255. Rows top to bottom."""
    levels = [[[tuple(pixel(x, y)) for x in range(w)] for y in range(h)]]
    while mips and (len(levels[-1]) > 1 or len(levels[-1][0]) > 1):
        src = levels[-1]; sh, sw = len(src), len(src[0]); nh, nw = max(1, sh // 2), max(1, sw // 2)
        lvl = []
        for y in range(nh):
            row = []
            for x in range(nw):
                ps = [src[min(sh - 1, 2 * y + dy)][min(sw - 1, 2 * x + dx)] for dy in (0, 1) for dx in (0, 1)]
                row.append(tuple(int(round(sum(p[k] for p in ps) / 4.0)) for k in range(4)))
            lvl.append(row)
        levels.append(lvl)
    flags = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000 | (0x20000 if len(levels) > 1 else 0)
    caps = 0x1000 | (0x400008 if len(levels) > 1 else 0)
    hdr = b"DDS " + struct.pack("<7I", 124, flags, h, w, w * 4, 0, len(levels)) + b"\0" * 44
    hdr += struct.pack("<2I4s5I", 32, 0x41, b"\0\0\0\0", 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)
    hdr += struct.pack("<5I", caps, 0, 0, 0, 0)
    body = b"".join(bytes((p[2], p[1], p[0], p[3])) for lvl in levels for row in lvl for p in row)
    return hdr + body
