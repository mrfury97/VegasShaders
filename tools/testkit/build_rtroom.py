# Builds the Vegas Shaders RT test room (2026-11-07): VS-RTRoom.esp + its own models and textures, the in-game twin of the RT playground
# (tools/nvr-rt-playground in the Vegas Shaders repo): one interior cell, VSRTRoom, flagged "behave like exterior" (CELL DATA 0x80 with
# 0x01) so the game gives it the weather's sun and sky (climate NVUrbanClearClimate), with:
#   - an office-kit room, 8 x 6 tiles (2048 x 1536, 256 high); the west 5 columns roofed, the east 3 open to the sky;
#   - a window wall on the south side (3 openings 200 x 160, our own wall model with the office wall texture), a terrace outside, a glass
#     pane in the west opening, the chain-link fence (CLFence01) inside the middle one, a fern (v22Fern) by the east one;
#   - a mirror wall on the west wall (5 vs_mirror01 panels, the model the kit already ships);
#   - chrome / gold / red spheres in front of it (our own models: smooth UV spheres, radius 64, with flat textures + _rmaos maps);
#   - a glowing panel on the north wall (our own model: a glow map in texture slot 2, as vanilla's lit windows);
#   - a pillar, a plant in the open part (NVFalseSpireaPlant);
#   - lamps: a warm ceiling lamp, a small warm lamp by the mirror, a BLUE lamp in the open part, and a big warm "sun lamp" outside the
#     windows (Vegas Shaders handles "behave like exterior" cells as interiors: no sun shadows there, so this lamp throws the window, fence
#     and fern shadows into the room through the interior lamp shadows / RT lamp shadows);
#   - camera marks VSRTRoomCam01-05 (XMarkerHeading, persistent): player.moveto VSRTRoomCam01.
# Vanilla FalloutNV.esm base forms by editor ID (nothing overridden); our models / textures are made here from scratch (vs_nif.py).
#   py -3.11 -I build_rtroom.py [--out <folder>]   -> <folder>\Data\VS-RTRoom.esp, Data\Meshes\VegasShaders\Test\*.nif,
#                                                     Data\Textures\VegasShaders\Test\*.dds, <folder>\rtroom-views.json (default .\out-rtroom)
import argparse, json, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_testcell as bt
import vs_nif as vn
from tes4 import group, header, sr, record, f32, u32

HERE = os.path.dirname(os.path.abspath(__file__))
PLUGIN = "VS-RTRoom.esp"
CELL = "VSRTRoom"
MESH_DIR = "VegasShaders\\Test\\"                 # under Meshes\ (the MODL path) and Textures\
TEX = "textures\\vegasshaders\\test\\"
FLAT_N = "textures\\shared\\flat_n.dds"          # vanilla flat normal map (the kit's mirror uses it too)
WALL_D, WALL_N = "textures\\dungeons\\office\\OffRmWall03.dds", "textures\\dungeons\\office\\OffRmWall03_n.dds"   # vanilla, by path

# fixed object ids (shot lists, aim / require onscreen use them; never renumber)
bt.FIXED = {CELL: 0x800, "VSRTRoomSphereChrome": 0x9A1, "VSRTRoomSphereGold": 0x9A2, "VSRTRoomSphereRed": 0x9A3,
            "VSRTRoomGlassPane": 0x9B1, "VSRTRoomGlowPanelRef": 0x9B2, "VSRTRoomFence": 0x9B3, "VSRTRoomFern": 0x9B4,
            "VSRTRoomSunLampRef": 0x9C1, "VSRTRoomBlueLampRef": 0x9C2, "VSRTRoomMirror03": 0x9D3}
bt.FIXED.update({"VSRTRoomCam%02d" % k: 0x900 + k for k in range(1, 16)})
bt.P = bt.Plugin()
P, F = bt.P, bt.F
W, H = 2048, 1536             # room size (8 x 6 tiles)
OPEN_FROM = 5                 # columns 5..7 have no ceiling
WINDOWS = (1, 2, 3)           # south-edge columns with a window opening
OPEN_X, OPEN_Z = (-100.0, 100.0), (64.0, 224.0)  # the opening in the window wall model (tile-local x, z)
WALL_Y = (108.0, 124.0)       # the window wall slab (tile-local y: the office walls' inner face is at ~108, their back at 124)


# ---------- our models + textures ----------
def models():
    """name -> (nif bytes, OBND), and texture name -> dds bytes."""
    out, tex = {}, {}
    def flat(rgba): return lambda x, y: rgba
    # spheres: radius 64, standing on z 0; tinted flat diffuse + material map (R roughness, G metal, B AO, A specular: as vs_mirror01_rmaos)
    for name, rgb, rough, metal in (("chrome", (245, 245, 248), 0.05, 1.0), ("gold", (255, 214, 130), 0.2, 1.0), ("red", (200, 32, 26), 0.5, 0.0)):
        stem = "vs_sphere_%s" % name
        tex[stem + "_d.dds"] = vn.dds_bytes(4, 4, flat(rgb + (255,)), mips=False)
        tex[stem + "_rmaos.dds"] = vn.dds_bytes(4, 4, flat((int(round(rough * 255)), int(round(metal * 255)), 255, 255)), mips=False)
        V, N, U, T = vn.sphere(64.0, 48, 24)
        assert vn.check_facing(V, N, T) == 0
        s = vn.Shape("VSSphere_%s:0" % name, V, N, U, T, [TEX + stem + "_d.dds", FLAT_N], glossiness=80.0 if metal else 30.0,
                     specular=(1.0, 1.0, 1.0))
        out["vs_sphere_%s01.nif" % name] = (vn.nif_bytes("VSSphere_%s" % name, [s]), (-64, -64, 0, 64, 64, 128))
    # glass pane (the west window): two faces (it is seen from both sides), alpha blended (no test), name with "glass" (material rule glass:
    # the path tracer's glass with PTGlass; VegasShaders.materials.txt keyword glass)
    tex["vs_glass01_d.dds"] = vn.dds_bytes(4, 4, flat((196, 226, 218, 56)), mips=False)
    tex["vs_glass01_rmaos.dds"] = vn.dds_bytes(4, 4, flat((5, 0, 255, 255)), mips=False)
    y = (WALL_Y[0] + WALL_Y[1]) * 0.5
    uvf = lambda p: ((p[0] - OPEN_X[0]) / (OPEN_X[1] - OPEN_X[0]), (OPEN_Z[1] - p[2]) / (OPEN_Z[1] - OPEN_Z[0]))
    V, N, U, T = vn.merge(vn.rect_x(OPEN_X[0], OPEN_X[1], y - 0.5, OPEN_Z[0], OPEN_Z[1], -1.0, uvf),
                          vn.rect_x(OPEN_X[0], OPEN_X[1], y + 0.5, OPEN_Z[0], OPEN_Z[1], +1.0, uvf))
    assert vn.check_facing(V, N, T) == 0
    s = vn.Shape("VSGlass01:0", V, N, U, T, [TEX + "vs_glass01_d.dds", FLAT_N], alpha=(vn.ALPHA_BLEND, 0),
                 shader_flags=vn.SF_SPECULAR | vn.SF_ALPHA_TEXTURE | vn.SF_REMAPPABLE | vn.SF_ZTEST, shader_flags2=0, glossiness=200.0,
                 specular=(1.0, 1.0, 1.0))
    out["vs_glass01.nif"] = (vn.nif_bytes("VSGlass01", [s]), (-100, int(y) - 1, 64, 100, int(y) + 1, 224))
    # glowing panel: 240 x 120, facing -y, z 100..220; diffuse pale with a dark frame, glow map warm white inside the frame (slot 2), the
    # material's emissive colour white (vanilla always-lit glow: emissive x glow map; no "external emittance", which follows the weather)
    def panel(inner, frame):
        return lambda x, y: frame if (x < 4 or y < 4 or x > 59 or y > 59) else inner
    tex["vs_glowpanel01_d.dds"] = vn.dds_bytes(64, 64, panel((236, 226, 206, 255), (46, 46, 48, 255)))
    tex["vs_glowpanel01_g.dds"] = vn.dds_bytes(64, 64, panel((255, 232, 192, 255), (0, 0, 0, 255)))
    V, N, U, T = vn.rect_x(-120.0, 120.0, 0.0, 100.0, 220.0, -1.0, lambda p: ((p[0] + 120.0) / 240.0, (220.0 - p[2]) / 120.0))
    s = vn.Shape("VSGlowPanel01:0", V, N, U, T, [TEX + "vs_glowpanel01_d.dds", FLAT_N, TEX + "vs_glowpanel01_g.dds"],
                 emissive=(1.0, 1.0, 1.0), emit_mult=2.0)
    out["vs_glowpanel01.nif"] = (vn.nif_bytes("VSGlowPanel01", [s]), (-120, -1, 100, 120, 1, 220))
    # the window wall: a 256 x 256 slab on the tile's +y edge (as the office kit's walls; the REFR turns it), y 108..124, with one opening;
    # office wall texture mapped as the vanilla wall maps it (u across the tile, v down the height)
    x0, x1, y0, y1 = -128.0, 128.0, WALL_Y[0], WALL_Y[1]
    ox0, ox1 = OPEN_X; oz0, oz1 = OPEN_Z
    fu = lambda p: ((p[0] + 128.0) / 256.0, 1.0 - p[2] / 256.0)               # faces along y
    su = lambda p: ((p[1] - y0) / 256.0, 1.0 - p[2] / 256.0)                  # side faces (along x)
    zu = lambda p: ((p[0] + 128.0) / 256.0, (p[1] - y0) / 256.0)              # sills (along z)
    parts = []
    for yy, fy in ((y0, -1.0), (y1, +1.0)):
        parts += [vn.rect_x(x0, x1, yy, 0.0, oz0, fy, fu), vn.rect_x(x0, x1, yy, oz1, 256.0, fy, fu),
                  vn.rect_x(x0, ox0, yy, oz0, oz1, fy, fu), vn.rect_x(ox1, x1, yy, oz0, oz1, fy, fu)]
    parts += [vn.rect_z(ox0, ox1, y0, y1, oz0, +1.0, zu), vn.rect_z(ox0, ox1, y0, y1, oz1, -1.0, zu),
              vn.rect_y(ox0, y0, y1, oz0, oz1, +1.0, su), vn.rect_y(ox1, y0, y1, oz0, oz1, -1.0, su)]
    V, N, U, T = vn.merge(*parts)
    assert vn.check_facing(V, N, T) == 0
    s = vn.Shape("VSWindowWall01:0", V, N, U, T, [WALL_D, WALL_N])
    out["vs_windowwall01.nif"] = (vn.nif_bytes("VSWindowWall01", [s]), (-128, int(y0), 0, 128, int(y1), 256))
    return out, tex


def stat_new(edid, model, obnd, src="OffRmFloor01"):
    """Our own STAT: a plain vanilla STAT's record with our editor id, bounds and model (its MODT texture hashes dropped)."""
    s = F(src); out = []
    for t, x in bt.subrecords(bt.e.data(s)):
        if t == "EDID": out.append(sr("EDID", edid))
        elif t == "OBND": out.append(sr("OBND", struct.pack("<6h", *obnd)))
        elif t == "MODL": out.append(sr("MODL", model))
        elif t == "MODT": continue
        else: out.append(sr(t, x))
    f = P.fid(edid)
    P.top.setdefault("STAT", []).append(record("STAT", f, out))
    return f


def cell_record(c):
    """CELL: interior (0x01) + behave like exterior (0x80); climate NVUrbanClearClimate (clear skies), image space NVDefaultExterior,
    no water. (The pattern of Dead Money's NVDLC01stealthTest, the one vanilla 0x81 cell.)"""
    subs = [sr("EDID", c.edid), sr("FULL", c.full), sr("DATA", b"\x81"), sr("XCLL", c.xcll), sr("LTMP", u32(0)), sr("LNAM", u32(0)),
            sr("XCLW", struct.pack("<I", 0x7F7FFFFF)), sr("XNAM", b"\0"), sr("XCIM", u32(F("NVDefaultExterior"))),
            sr("XCCM", u32(F("NVUrbanClearClimate")))]
    return record("CELL", c.fid, subs)


def build(meshes):
    st = {}
    for name, (_, obnd) in meshes.items():
        edid = {"vs_sphere_chrome01.nif": "VSRTRoomSphereChromeStat", "vs_sphere_gold01.nif": "VSRTRoomSphereGoldStat",
                "vs_sphere_red01.nif": "VSRTRoomSphereRedStat", "vs_glass01.nif": "VSRTRoomGlass01",
                "vs_glowpanel01.nif": "VSRTRoomGlowPanel01", "vs_windowwall01.nif": "VSRTRoomWindowWall01"}[name]
        st[name] = stat_new(edid, "VegasShaders\\Test\\" + name, obnd)
    mirror = stat_new("VSRTRoomMirrorPanel01", "VegasShaders\\Test\\vs_mirror01.nif", (-1, -56, 0, 1, 56, 208), src="NVULWallScreen01")
    L = {
        "ceiling": bt.ligh("VSRTRoomLightCeiling", "BasementLightKeyNeutral", 650, (255, 226, 190)),
        "mirror": bt.ligh("VSRTRoomLightMirror", "BasementLightKeyWarm", 380, (255, 190, 130)),
        "blue": bt.ligh("VSRTRoomLightBlue", "BasementLightKeyNeutral", 560, (90, 150, 255)),
        "sun": bt.ligh("VSRTRoomSunLamp", "BasementLightKeyNeutral", 1600, (255, 240, 212), fade=2.0),
    }
    c = bt.Cell(CELL, "Vegas Shaders RT Test Room", bt.xcll((40, 40, 44), (64, 62, 58), (70, 76, 84), fog_near=2000.0, fog_far=20000.0))
    # ---- the room ----
    mid, floor = F("OffRmMid01"), F("OffRmFloor01")
    wall, wall_open = F("OffRmWall01"), F("OffRmWallNoCeiling01")
    cor, cor_open = F("OffRmCorIn01"), F("OffRmCorIn01NoCeiling01")
    nx, ny = W // 256, H // 256
    for i in range(nx):
        for j in range(ny):
            cx, cy = 128 + 256 * i, 128 + 256 * j
            op = i >= OPEN_FROM
            w, s, ea, n = i == 0, j == 0, i == nx - 1, j == ny - 1
            if n and w: c.ref(cor, cx, cy, 0, 0)
            elif n and ea: c.ref(cor_open if op else cor, cx, cy, 0, 90)
            elif s and ea: c.ref(cor_open if op else cor, cx, cy, 0, 180)
            elif s and w: c.ref(cor, cx, cy, 0, 270)
            elif s and i in WINDOWS:
                c.ref(mid, cx, cy, 0)
                c.ref(st["vs_windowwall01.nif"], cx, cy, 0, 180)
            elif n: c.ref(wall_open if op else wall, cx, cy, 0, 0)
            elif ea: c.ref(wall_open if op else wall, cx, cy, 0, 90)
            elif s: c.ref(wall_open if op else wall, cx, cy, 0, 180)
            elif w: c.ref(wall, cx, cy, 0, 270)
            else: c.ref(floor if op else mid, cx, cy, 0)
    # the terrace outside the windows (two rows of floor) and invisible walls (the game's CollisionMarker) in the openings
    for i in range(0, OPEN_FROM):
        for cy in (-128, -384):
            c.ref(floor, 128 + 256 * i, cy, 0)
    for x in (384, 640, 896):
        c.ref(F("CollisionMarker"), x, -12, 128)
    c.navmesh(48, 48, W - 48, H - 48, 0.0)
    # ---- the window side: glass in the west opening, the fence inside the middle one, a fern by the east one ----
    # (the window wall REFR turns +y to -y: the slab sits at world y 4..20, the openings at x 284..484, 540..740, 796..996, z 64..224)
    c.ref(st["vs_glass01.nif"], 256 + 128, 128, 0, 180, edid="VSRTRoomGlassPane")
    c.ref(F("CLFence01"), 512 - 2, 150, 0, 0, edid="VSRTRoomFence")
    c.ref(F("v22Fern"), 768 + 128, 230, 1, 30, edid="VSRTRoomFern", scale=1.6)
    c.ref(L["sun"], 640, -420, 360, edid="VSRTRoomSunLampRef")
    # ---- the mirror wall (west wall, facing +x) and the spheres in front of it ----
    for k in range(-2, 3):
        c.ref(mirror, 24, 768 + 130 * k, 2, 0, scale=1.2, edid="VSRTRoomMirror03" if k == 0 else None)
    for k, (name, y) in enumerate((("red", 560), ("gold", 768), ("chrome", 976))):
        c.ref(st["vs_sphere_%s01.nif" % name], 420, y, 1, 0, edid="VSRTRoomSphere" + name.capitalize())
    c.ref(L["mirror"], 300, 768, 200)
    # ---- the glowing panel (north wall), the pillar, the ceiling lamp ----
    c.ref(st["vs_glowpanel01.nif"], 900, 1512, 0, 0, edid="VSRTRoomGlowPanelRef")
    c.ref(F("OffRmFreeCol01"), 1100, 1080, 0)
    c.ref(F("OffRmLight02"), 640, 1100, 224); c.ref(L["ceiling"], 640, 1100, 190)
    # ---- the open part: a plant in the sun, the blue lamp ----
    c.ref(F("NVFalseSpireaPlant"), 1700, 420, 0, 0, scale=1.4)
    c.ref(L["blue"], 1720, 980, 150, edid="VSRTRoomBlueLampRef")
    # ---- camera marks ----
    def view(k, x, y, yaw, pitch, purpose):
        f = c.ref(0x34, x, y, 0.0, yaw, edid="VSRTRoomCam%02d" % k, persistent=True)   # XMarkerHeading
        P.views.append({"marker": "VSRTRoomCam%02d" % k, "object_id": "%06X" % (f & 0xFFFFFF), "pos": [x, y, 0.0], "yaw": yaw,
                        "pitch": pitch, "purpose": purpose})
    view(1, 1900, 1380, bt.yaw_to(1900, 1380, 500, 420), 8, "the whole room from the open (sky) corner: mirror wall, spheres, window wall")
    view(2, 820, 1000, 180, 6, "the window wall: glass (right), fence (middle), fern (left), the sun lamp's light and shadows on the floor")
    view(3, 1000, 768, 270, 8, "the mirror wall and the chrome / gold / red spheres in front of it (you in the mirror)")
    view(4, 900, 640, 0, 2, "the glowing panel on the north wall, the pillar, the ceiling lamp")
    view(5, 1150, 250, bt.yaw_to(1150, 250, 1750, 760), -10, "the open part: the sky, the plant in the sun, the blue lamp")
    if bt.missing: sys.exit("missing editor IDs in FalloutNV.esm: %s" % ", ".join(bt.missing))
    return c


def write(out_dir, meshes, tex):
    data = os.path.join(out_dir, "Data")
    md = os.path.join(data, "Meshes", "VegasShaders", "Test"); td = os.path.join(data, "Textures", "VegasShaders", "Test")
    os.makedirs(md, exist_ok=True); os.makedirs(td, exist_ok=True)
    for name, (b, _) in meshes.items():
        open(os.path.join(md, name), "wb").write(b)
    for name, b in tex.items():
        open(os.path.join(td, name), "wb").write(b)
    body = b""; nrec = 0; ngrp = 0
    for label in ("STAT", "LIGH"):
        recs = P.top.get(label, [])
        if recs: body += group(label, 0, b"".join(recs)); nrec += len(recs); ngrp += 1
    c = P.cells[0]; oid = c.fid & 0xFFFFFF
    kids = group(c.fid, 8, b"".join(c.pers)) + group(c.fid, 9, b"".join(c.temp + c.navm)); ngrp += 2
    cg = group(oid % 10, 2, group((oid // 10) % 10, 3, cell_record(c) + group(c.fid, 6, kids))); ngrp += 4
    nrec += 1 + len(c.pers) + len(c.temp) + len(c.navm)
    body += group("CELL", 0, cg)
    hdr = header(nrec + ngrp, P.next, "Vegas Shaders (Claude Code)",
                 "Vegas Shaders RT test room: mirror wall, chrome / gold / red spheres, glass, glow panel, fence, plants, lamps, sky. "
                 "coc VSRTRoom; player.moveto VSRTRoomCam01 .. 05.", ["FalloutNV.esm"])
    p = os.path.join(data, PLUGIN)
    with open(p + ".tmp", "wb") as f: f.write(hdr + body)
    os.replace(p + ".tmp", p)
    meta = {"plugin": PLUGIN, "cell": CELL, "about": "camera marks of VS-RTRoom.esp; full form id = load index (hex) + object_id",
            "refs": {k: "%06X" % (v & 0xFFFFFF) for k, v in P.ids.items()}, "views": P.views}
    with open(os.path.join(out_dir, "rtroom-views.json"), "w", encoding="utf-8") as f: json.dump(meta, f, indent=1)
    print("wrote %s (%d bytes, %d records, %d groups), %d models, %d textures, %d views" % (p, len(hdr + body), nrec, ngrp, len(meshes), len(tex), len(P.views)))


if __name__ == "__main__":
    ap = argparse.ArgumentParser(); ap.add_argument("--out", default=os.path.join(HERE, "out-rtroom"))
    a = ap.parse_args()
    meshes, tex = models()
    build(meshes); write(a.out, meshes, tex)
