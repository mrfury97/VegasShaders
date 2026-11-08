# Builds NVR-TestCell.esp (2026-10-31): two interior test cells for repeatable NVR bench shots, vanilla FalloutNV.esm assets only.
#   py -3.11 -I build_testcell.py [--out <folder>]        -> <folder>\NVR-TestCell.esp + views.json (default: .\out)
# Base forms are looked up by editor ID in FalloutNV.esm (esm.py); nothing is invented except our own records:
#   NPC_ copies of vanilla NPCs (script, factions and loot removed, packages replaced), a few LIGH variants, 2 CELLs with
#   their references and a flat NAVM for the walker. The room shell is the office kit vanilla's dev cell GreenRoom is built from
#   (OffRmMid01 / OffRmWall01 / OffRmCorIn01, 256-unit tiles; local +y = the wall side, heading h turns +y to (sin h, cos h)).
import argparse, json, math, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))   # (-I leaves the script folder off the path)
from esm import Esm, subrecords, sub
from tes4 import sr, record, group, header, f32, u32

HERE = os.path.dirname(os.path.abspath(__file__))
PLUGIN = "NVR-TestCell.esp"
e = Esm()
missing = []


def F(name):
    fid = e.fid(name)
    if fid is None: missing.append(name); return 0
    return fid


class Plugin:
    def __init__(self):
        self.next = 0x01001000   # auto ids; the named refs and cells below have FIXED ids (shot lists use them)
        self.top = {}          # group label -> [record bytes]
        self.cells = []        # dicts
        self.views = []
        self.ids = {}          # our editor id -> form id

    def fid(self, edid=None):
        if edid in FIXED: f = 0x01000000 | FIXED[edid]
        else: f = self.next; self.next += 1
        assert f not in self.ids.values(), edid
        if edid: self.ids[edid] = f
        return f


# Fixed object ids (never renumber: shot lists, views.json and other sessions' lists use them)
FIXED = {"NVRTestCell": 0x800, "NVRTestCellLamps": 0x801, "NVRTestPatrolA": 0x980, "NVRTestPatrolB": 0x981, "NVRTestWalker01": 0x990,
         "NVRTestLampNPC01": 0x9B0}
FIXED.update({"NVRTestCam%02d" % k: 0x900 + k for k in range(1, 64)})
FIXED.update({"NVRTestStillNPC%02d" % k: 0x9A0 + k for k in range(1, 16)})
FIXED.update({"NVRTestLampsNPC%02d" % k: 0x9C0 + k for k in range(1, 16)})


P = Plugin()
DEG = math.pi / 180


# ---------- base records of our own ----------
def npc_copy(src, edid, name, packages):
    s = F(src); d = e.data(s); out = []; pk_done = False; keep_item = True
    for t, x in subrecords(d):
        if t == "EDID": out.append(sr("EDID", edid))
        elif t == "FULL": out.append(sr("FULL", name))
        elif t in ("SCRI", "SNAM"): continue                 # no script, no factions
        elif t == "PKID":
            if not pk_done: out += [sr("PKID", u32(p)) for p in packages]; pk_done = True
        elif t == "CNTO":
            it = struct.unpack_from("<I", x)[0]
            keep_item = it in e.recs and e.recs[it].type == "ARMO"   # clothes/armour only: no weapons, no levelled loot
            if keep_item: out.append(sr(t, x))
        elif t == "COED":
            if keep_item: out.append(sr(t, x))
        else: out.append(sr(t, x))
    assert pk_done, src
    f = P.fid(edid)
    P.top.setdefault("NPC_", []).append(record("NPC_", f, out, e.recs[s].flags & ~0x00040000))
    return f


def ligh(edid, src, radius, rgb, flags=None, fade=None):
    s = F(src); out = []
    for t, x in subrecords(e.data(s)):
        if t == "EDID": out.append(sr("EDID", edid))
        elif t == "DATA":
            v = list(struct.unpack("<iI4BIffIf", x[:32]))
            v[1] = radius; v[2:5] = rgb; v[5] = 0
            if flags is not None: v[6] = flags
            out.append(sr("DATA", struct.pack("<iI4BIffIf", *v) + x[32:]))
        elif t == "FNAM" and fade is not None: out.append(sr("FNAM", f32(fade)))
        else: out.append(sr(t, x))
    f = P.fid(edid)
    P.top.setdefault("LIGH", []).append(record("LIGH", f, out))
    return f


# ---------- cells ----------
class Cell:
    def __init__(self, edid, full, xcll):
        self.edid, self.full, self.xcll = edid, full, xcll
        self.fid = P.fid(edid)
        self.pers, self.temp, self.navm = [], [], []
        P.cells.append(self)

    def ref(self, base, x, y, z, rz=0.0, rx=0.0, ry=0.0, scale=None, edid=None, extra=(), persistent=False, kind="REFR", note=None):
        f = P.fid(edid)
        subs = ([sr("EDID", edid)] if edid else []) + [sr("NAME", u32(base))] + list(extra)
        if scale is not None: subs.append(sr("XSCL", f32(scale)))
        subs.append(sr("DATA", f32(x, y, z, rx * DEG, ry * DEG, rz * DEG)))
        (self.pers if persistent else self.temp).append(record(kind, f, subs, 0x400 if persistent else 0))
        return f

    def room(self, x0, y0, nx, ny, z=0.0, windows=()):
        """Office-kit room: nx * ny tiles of 256 units from (x0, y0); walls all round. windows = north-edge tile columns
        that get a window wall (OffRmWndoFullWall01 over a plain tile) instead of the solid wall."""
        mid, wall, cor, wndo = F("OffRmMid01"), F("OffRmWall01"), F("OffRmCorIn01"), F("OffRmWndoFullWall01")
        for i in range(nx):
            for j in range(ny):
                cx, cy = x0 + 128 + 256 * i, y0 + 128 + 256 * j
                w, s, ea, n = i == 0, j == 0, i == nx - 1, j == ny - 1
                if (n and w): self.ref(cor, cx, cy, z, 0)
                elif (n and ea): self.ref(cor, cx, cy, z, 90)
                elif (s and ea): self.ref(cor, cx, cy, z, 180)
                elif (s and w): self.ref(cor, cx, cy, z, 270)
                elif n and i in windows: self.ref(mid, cx, cy, z); self.ref(wndo, cx, cy, z, 0)
                elif n: self.ref(wall, cx, cy, z, 0)
                elif ea: self.ref(wall, cx, cy, z, 90)
                elif s: self.ref(wall, cx, cy, z, 180)
                elif w: self.ref(wall, cx, cy, z, 270)
                else: self.ref(mid, cx, cy, z)

    def navmesh(self, x0, y0, x1, y1, z, step=256.0):
        """A flat navmesh over the rectangle (two triangles per square, neighbours linked; one grid cell)."""
        nx = max(1, int(round((x1 - x0) / step))); ny = max(1, int(round((y1 - y0) / step)))
        xs = [x0 + (x1 - x0) * i / nx for i in range(nx + 1)]; ys = [y0 + (y1 - y0) * j / ny for j in range(ny + 1)]
        V = [(x, y, z) for y in ys for x in xs]
        vid = lambda i, j: j * (nx + 1) + i
        tid = lambda i, j, k: 2 * (j * nx + i) + k
        T = []
        for j in range(ny):
            for i in range(nx):
                a, b, c, d = vid(i, j), vid(i + 1, j), vid(i + 1, j + 1), vid(i, j + 1)
                # A = (a, b, c): edges a-b (below: B of (i, j-1)), b-c (right: B of (i+1, j)), c-a (own B)
                T.append(((a, b, c), (tid(i, j - 1, 1) if j > 0 else -1, tid(i + 1, j, 1) if i < nx - 1 else -1, tid(i, j, 1))))
                # B = (a, c, d): edges a-c (own A), c-d (above: A of (i, j+1)), d-a (left: A of (i-1, j))
                T.append(((a, c, d), (tid(i, j, 0), tid(i, j + 1, 0) if j < ny - 1 else -1, tid(i - 1, j, 0) if i > 0 else -1)))
        if WINDING_FLIP: T = [((v[0], v[2], v[1]), (ed[2], ed[1], ed[0])) for v, ed in T]
        nvtr = b"".join(struct.pack("<3h3hHH", *v, *ed, 0x0800, 0) for v, ed in T)
        grid = struct.pack("<Iff6f", 1, x1 - x0, y1 - y0, x0, y0, z, x1, y1, z) + struct.pack("<H", len(T)) + struct.pack("<%dh" % len(T), *range(len(T)))
        f = P.fid()
        subs = [sr("NVER", u32(11)), sr("DATA", u32(self.fid, len(V), len(T), 0, 0, 0)),
                sr("NVVX", b"".join(f32(*v) for v in V)), sr("NVTR", nvtr), sr("NVGD", grid)]
        self.navm.append(record("NAVM", f, subs))

    def record(self):
        subs = [sr("EDID", self.edid), sr("FULL", self.full), sr("DATA", b"\x01"), sr("XCLL", self.xcll),
                sr("LTMP", u32(0)), sr("LNAM", u32(0)), sr("XCLW", f32(-2147483648.0)), sr("XNAM", b"\0"),
                sr("XCIM", u32(F("GSProspectorSaloonInterior") and struct.unpack("<I", sub(e.data(F("GSProspectorSaloonInterior")), "XCIM"))[0]))]
        return record("CELL", self.fid, subs)


def xcll(amb, dirc, fog, fog_near=64.0, fog_far=6000.0, fade=1.0, clip=8000.0, power=1.0):
    return bytes(amb) + b"\0" + bytes(dirc) + b"\0" + bytes(fog) + b"\0" + struct.pack("<ffiifff", fog_near, fog_far, 0, 0, fade, clip, power)


def winding_of_vanilla():
    """+1 if vanilla navmesh triangles wind counter-clockwise seen from above (GreenRoom's navmesh), else -1."""
    d = e.data(e.children and [k for k in e.children[F("GreenRoom")] if e.recs[k].type == "NAVM"][0])
    V = [struct.unpack_from("<3f", sub(d, "NVVX"), i) for i in range(0, len(sub(d, "NVVX")), 12)]
    T = sub(d, "NVTR"); s = 0
    for i in range(0, len(T), 16):
        a, b, c = struct.unpack_from("<3h", T, i)
        s += (V[b][0] - V[a][0]) * (V[c][1] - V[a][1]) - (V[b][1] - V[a][1]) * (V[c][0] - V[a][0])
    return 1 if s > 0 else -1


WINDING_FLIP = winding_of_vanilla() < 0   # ours are counter-clockwise as written


def view(edid, cell, x, y, yaw, pitch, purpose, frames=300, subject=None, moving=False):
    f = cell.ref(0x34, x, y, 0.0, yaw, edid=edid, persistent=True)   # XMarkerHeading
    P.views.append({"marker": edid, "object_id": "%06X" % (f & 0xFFFFFF), "cell": cell.edid, "pos": [x, y, 0.0],
                    "yaw": yaw, "pitch": pitch, "settle_frames": frames, "purpose": purpose,
                    "subject": subject, "moving": moving})
    return f


def yaw_to(x, y, tx, ty):
    return round(math.degrees(math.atan2(tx - x, ty - y)) % 360, 2)


def build():
    # ---- our base records ----
    stay = F("DefaultStayAtEditorLocation"); patrol = F("DefaultPatrolCasual")
    npc = {
        "trudy": npc_copy("GSTrudy", "NVRTestNPCCloth01", "NVR Test Person (cloth)", [stay]),
        "chet": npc_copy("GSChet", "NVRTestNPCCloth02", "NVR Test Person (cloth 2)", [stay]),
        "joe": npc_copy("GSJoeCobb", "NVRTestNPCArmor01", "NVR Test Person (armour)", [stay]),
        "sunny": npc_copy("GSSunnySmiles", "NVRTestNPCLeather01", "NVR Test Person (leather)", [stay]),
        "jean": npc_copy("VFSJeanBaptisteCutting", "NVRTestNPCCombat01", "NVR Test Person (combat armour)", [stay]),
        "walker": npc_copy("GSEasyPete", "NVRTestNPCWalker01", "NVR Test Walker", [patrol]),
    }
    L = {
        "ceiling": ligh("NVRTestLightCeiling", "BasementLightKeyNeutral", 600, (240, 226, 200)),
        "bright": ligh("NVRTestLightBrightOverhead", "BasementLightKeyNeutral", 420, (255, 250, 240), fade=2.6),
        "floor": ligh("NVRTestLightFloorLamp", "BasementLightKeyWarm", 380, (255, 200, 130)),
        "sconce": ligh("NVRTestLightSconce", "BasementLightKeyWarm", 320, (255, 180, 110)),
        "neon": ligh("NVRTestLightNeonRed", "NVLightNeonRed", 340, (251, 104, 79)),
        "flicker": ligh("NVRTestLightFlicker", "OfficeLightKeyCoolFlicker", 380, (189, 204, 221)),
        "fire": ligh("NVRTestLightFire", "FireBrightOrange", 380, (255, 142, 17)),
    }
    pool_colors = [(255, 236, 200), (200, 220, 255), (255, 90, 70), (90, 255, 120), (110, 140, 255), (255, 180, 60)]
    L["pool"] = [ligh("NVRTestLightPool%02d" % k, "BasementLightKeyNeutral", 300, c) for k, c in enumerate(pool_colors)]

    # ---- cell 1: the hall (14 x 10 tiles = 3584 x 2560 units, floor z 0) ----
    hall = Cell("NVRTestCell", "NVR Test Cell", xcll((70, 67, 62), (48, 47, 46), (40, 40, 44), fog_far=7000.0))
    W, H = 3584, 2560
    hall.room(0, 0, 14, 10, windows=(10, 11))
    hall.navmesh(48, 48, W - 48, H - 48, 0.0)
    fixture, flick_fix = F("OffRmLight02"), F("OffRmLightBlink01")
    for x in (512, 1280, 2048, 2816):
        for y in (384, 1152, 1920):
            if (x, y) == (2816, 1152): continue          # the flickering one goes here instead
            hall.ref(fixture, x, y, 224); hall.ref(L["ceiling"], x, y, 190)
    hall.ref(flick_fix, 2816, 1152, 224); hall.ref(L["flicker"], 2816, 1152, 190)

    # material strip along the north wall (fronts face south = heading 0 for props; y about 2300)
    S = 2300
    hall.ref(F("Locker01Empty"), 260, S + 150, 0)                                   # bare metal
    hall.ref(F("TableVault01"), 300, S - 60, 0)                                     # bare metal (vault table)
    hall.ref(F("SlotMachineTable01"), 620, S, 0); hall.ref(F("SlotMachine01StaticTOPS"), 620, S, 49)   # chrome trim
    hall.ref(F("Motorcycle01"), 930, S - 40, 0, 90)                                 # chrome + paint
    hall.ref(F("RefrigeratorClean"), 1250, S + 100, 0)                              # painted metal
    hall.ref(F("NukaVendingMachineSTATIC"), 1420, S + 100, 0)                       # painted metal
    hall.ref(F("CrateLarge01"), 1760, S + 70, 0)                                    # wood
    hall.ref(F("NVGSCrateContainerEmpty"), 1720, S - 120, 0)                        # wood (small crate)
    hall.ref(F("BedMattress01Static"), 2080, S - 40, 6, 90)                         # cloth
    hall.ref(F("GomorrahVelvetCurtain04"), 2120, S + 180, 0)                        # cloth (hanging)
    # windows: north-wall tiles 10 and 11 (x 2560-3072) are window walls; a mirror stands on the polished floor quarter
    hall.ref(F("ULInnerFloor"), 2900, 2420, 1, 0)                                   # mirror-like polished floor (quarter disc, 713 units)
    hall.ref(F("RestroomMirror01"), 3250, 2150, 130, 270)                           # mirror (faces -y after the turn)
    hall.ref(F("DisplayCaseGlass02"), 3050, 2050, 60)                               # glass
    hall.ref(F("TableVault01"), 3050, 2050, 0)

    # south-west: still NPCs on rugs and tiles, facing south (heading 180) toward camera 05
    for (x, y, b) in ((400, 350, "CasLoRmFloorONLY"), (656, 350, "CasLoRmFloorONLY"), (1150, 350, "BLD11Floor01")):
        hall.ref(F(b), x, y, 1)
    hall.ref(F("TenpennyRug02"), 500, 1000, 1); hall.ref(F("NVTOPSCasinoRug"), 820, 1000, 1); hall.ref(F("NVLucky38Rug01"), 1160, 1000, 1)
    still = []
    for k, (x, b) in enumerate(((500, "trudy"), (800, "chet"), (1100, "joe"), (1400, "sunny"))):
        still.append(hall.ref(npc[b], x, 1000, 0, 180, kind="ACHR", persistent=True, edid="NVRTestStillNPC%02d" % (k + 1)))
    # bright lamp right over a standing NPC (capsule / contact shadows)
    lampnpc = hall.ref(npc["jean"], 1560, 560, 0, 225, kind="ACHR", persistent=True, edid="NVRTestLampNPC01")
    hall.ref(L["bright"], 1560, 560, 232)

    # lamps of each kind: floor lamp (SW), wall sconce (west wall), neon (east wall), flicker (ceiling, above), ceiling (grid)
    hall.ref(F("FloorLamp"), 300, 1560, 0); hall.ref(L["floor"], 300, 1560, 130)
    hall.ref(F("NeoclassicalSconce"), 100, 1200, 20, 270); hall.ref(L["sconce"], 70, 1200, 170)
    hall.ref(F("NVNoSoNeonSign02A"), 3545, 1250, 100, 90); hall.ref(L["neon"], 3470, 1250, 150)

    # south-east: fire, puddle, plants, emissive signs
    hall.ref(F("NVFirePitA"), 2350, 1350, 0); hall.ref(F("FXFireSmall02"), 2350, 1350, 40); hall.ref(L["fire"], 2350, 1350, 110)
    hall.ref(F("NVCleanWater1x1"), 2850, 1050, 1, scale=0.4)                         # placed water puddle, ~205 units
    for (x, y, b) in ((3150, 1450, "NVTOPSRecPlanter04Small"), (3320, 1450, "v22Fern"), (3200, 1600, "NVFalseSpireaPlant"),
                      (3380, 1050, "NVbarrelcactus01"), (3300, 850, "pricklypearcactus01"), (3420, 1250, "NVTOPSRecPlanter03")):
        hall.ref(F(b), x, y, 0)
    hall.ref(F("NVSSNeonSign"), 3560, 900, 150, 90)                                  # small emissive sign
    hall.ref(F("WindowSingleGGlow"), 3560, 1600, 150, 90)                            # glowing window

    # the walker: a patrol loop across the south lane (y 350), x 2000 <-> 3300
    schr = bytes.fromhex("0000000000000000000000000000000000000100")
    ma, mb = 0x01000000 | FIXED["NVRTestPatrolA"], 0x01000000 | FIXED["NVRTestPatrolB"]   # two patrol markers linked to each other
    def patrol_marker(edid, x, y, nxt):
        return hall.ref(0x34, x, y, 0, 90, edid=edid, persistent=True,
                        extra=[sr("XPRD", f32(1.0)), sr("XPPA", b""), sr("INAM", u32(0)), sr("SCHR", schr), sr("TNAM", u32(0)), sr("XLKR", u32(nxt))])
    assert patrol_marker("NVRTestPatrolA", 2000, 350, mb) == ma
    assert patrol_marker("NVRTestPatrolB", 3300, 350, ma) == mb
    walker = hall.ref(npc["walker"], 2000, 350, 0, 90, kind="ACHR", persistent=True, edid="NVRTestWalker01", extra=[sr("XLKR", u32(ma))])

    # camera marks (player.moveto <mark>, then setangle x <pitch>)
    view("NVRTestCam01", hall, 760, 1900, 0, 8, "material strip west: bare metal, chrome, painted metal (fronts)", subject="NVRTestStillNPC01")
    view("NVRTestCam02", hall, 1800, 1900, 0, 8, "material strip middle: painted metal, wood, cloth")
    view("NVRTestCam03", hall, 2900, 1600, 10, 12, "glass + window wall + mirror on the polished (mirror-like) floor, ceiling lamps reflected")
    view("NVRTestCam04", hall, 950, 1480, 180, 20, "still NPCs from behind-above over rugs; sconce + floor lamp light")
    view("NVRTestCam05", hall, 950, 480, 0, 3, "four still NPCs facing the camera on rugs (skin, cloth, armour, leather)", subject="NVRTestStillNPC02")
    view("NVRTestCam06", hall, 800, 830, 0, 4, "close-up: one still NPC face (skin / SSS / face shadows)", subject="NVRTestStillNPC02")
    view("NVRTestCam07", hall, 1250, 300, yaw_to(1250, 300, 1560, 560), 12, "bright lamp right over a standing NPC: capsule + contact shadows", subject="NVRTestLampNPC01")
    view("NVRTestCam08", hall, 400, 650, 340, 4, "lamps of each kind: floor lamp, wall sconce, ceiling grid (shadowed interior lamps)")
    view("NVRTestCam09", hall, 2450, 750, yaw_to(2450, 750, 2950, 1250), 16, "fire, placed water puddle, plants, neon + emissive signs, flickering ceiling lamp")
    view("NVRTestCam10", hall, 2650, 1000, 180, 8, "WALKER: the patrol crosses the view in front of a static wall (ghosting / motion vectors)", subject="NVRTestWalker01", moving=True)
    view("NVRTestCam11", hall, 1792, 1280, 90, 0, "wide hall east, long view (fog, many objects, lamp falloff)")

    # ---- cell 2: many lamps (8 x 8 tiles = 2048 x 2048) ----
    lamps = Cell("NVRTestCellLamps", "NVR Test Cell (many lamps)", xcll((30, 30, 32), (20, 20, 22), (30, 30, 34), fog_far=6000.0))
    lamps.room(0, 0, 8, 8)
    small_fix = F("OffRmLight01"); k = 0
    for x in range(256, 2048, 256):
        for y in range(256, 2048, 256):
            lamps.ref(small_fix, x, y, 224); lamps.ref(L["pool"][k % len(L["pool"])], x, y, 200); k += 1
    for (x, y) in ((640, 640), (1408, 640), (640, 1408), (1408, 1408)):
        lamps.ref(F("OffRmFreeCol01"), x, y, 0)
    lamps.ref(F("CrateLarge01"), 1024, 1500, 0); lamps.ref(F("TableVault01"), 1024, 700, 0); lamps.ref(F("TenpennyRug01"), 1024, 1024, 1)
    for k, (x, b) in enumerate(((880, "trudy"), (1024, "chet"), (1168, "joe"))):
        lamps.ref(npc[b], x, 1100, 0, 180, kind="ACHR", persistent=True, edid="NVRTestLampsNPC%02d" % (k + 1))
    view("NVRTestCam12", lamps, 1024, 250, 0, 8, "many lamps (49 coloured ceiling lamps): ManyLights / lamp pools, wide", subject="NVRTestLampsNPC02")
    view("NVRTestCam13", lamps, 300, 300, 45, 10, "many lamps, diagonal through the pillars")
    view("NVRTestCam14", lamps, 1024, 400, 0, 55, "many lamps: floor pools looking down (lamp falloff, pool edges)")

    if missing: sys.exit("missing editor IDs in FalloutNV.esm: %s" % ", ".join(missing))


def write(out_dir):
    body = b""; nrec = 0; ngrp = 0
    for label in ("LIGH", "NPC_"):
        recs = P.top.get(label, [])
        if recs: body += group(label, 0, b"".join(recs)); nrec += len(recs); ngrp += 1
    blocks = {}
    for c in P.cells:
        oid = c.fid & 0xFFFFFF
        blocks.setdefault(oid % 10, {}).setdefault((oid // 10) % 10, []).append(c)
    cg = b""
    for b in sorted(blocks):
        bg = b""
        for s in sorted(blocks[b]):
            sg = b""
            for c in blocks[b][s]:
                kids = b""
                if c.pers: kids += group(c.fid, 8, b"".join(c.pers)); ngrp += 1
                kids += group(c.fid, 9, b"".join(c.temp + c.navm)); ngrp += 1
                sg += c.record() + group(c.fid, 6, kids); ngrp += 1
                nrec += 1 + len(c.pers) + len(c.temp) + len(c.navm)
            bg += group(s, 3, sg); ngrp += 1
        cg += group(b, 2, bg); ngrp += 1
    body += group("CELL", 0, cg); ngrp += 1
    hdr = header(nrec + ngrp, P.next, "NVR bench (Claude Code)",
                 "NVR test cells for repeatable bench shots (vanilla FalloutNV.esm assets only). coc NVRTestCell / NVRTestCellLamps; camera marks NVRTestCam01-14.",
                 ["FalloutNV.esm"])
    os.makedirs(out_dir, exist_ok=True)
    p = os.path.join(out_dir, PLUGIN)
    with open(p + ".tmp", "wb") as f: f.write(hdr + body)
    os.replace(p + ".tmp", p)
    meta = {"plugin": PLUGIN, "about": "camera marks of NVR-TestCell.esp; full form id = load index (hex, 2 digits) + object_id",
            "cells": {c.edid: "%06X" % (c.fid & 0xFFFFFF) for c in P.cells},
            "refs": {k: "%06X" % (v & 0xFFFFFF) for k, v in P.ids.items()}, "views": P.views}
    with open(os.path.join(out_dir, "views.json"), "w", encoding="utf-8") as f: json.dump(meta, f, indent=1)
    print("wrote %s (%d bytes, %d records, %d groups), %d views" % (p, len(hdr + body), nrec, ngrp, len(P.views)))


if __name__ == "__main__":
    ap = argparse.ArgumentParser(); ap.add_argument("--out", default=os.path.join(HERE, "out"))
    a = ap.parse_args()
    build(); write(a.out)
