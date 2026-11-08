# Builds VS-AlphaLampTest.esp (2026-11-05, bench slots only): one small interior to prove RT lamp shadows from ALPHA-TESTED casters
# (RT-PATH-TRACING.md section 33 item 2). A chain-link fence (CLFence01, alpha-tested) stands in the middle of an office-kit room; one
# steady lamp (no flicker) stands behind it at chest height; the camera mark VSAlphaLampCam01 looks over the floor in front of the fence,
# where the lamp throws the fence's shadow. (2026-11-06) A polished marble disc under it all, for reflection demonstrations. [Shaders.ShadowsInteriors.RT] AlphaTest 0 / 1 (live) then shows the mesh's diamonds or a
# solid card / nothing.
#   py -3.11 -I build_alphalamp.py [--out <folder>]   -> <folder>\VS-AlphaLampTest.esp (default .\out-alphalamp)
# Vanilla FalloutNV.esm assets only. Never for the user's game.
import argparse, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_testcell as bt
from tes4 import group, header, sr, record

HERE = os.path.dirname(os.path.abspath(__file__))
PLUGIN = "VS-AlphaLampTest.esp"
CELL = "VSAlphaLampTest"

bt.FIXED = {CELL: 0x800}
bt.FIXED.update({"VSAlphaLampCam%02d" % k: 0x900 + k for k in range(1, 8)})
bt.P = bt.Plugin()
P, F = bt.P, bt.F


def stat_copy(edid, src, model):
    """(2026-11-06) our own STAT: a vanilla one's record with another model (MODT, the old model's texture hashes, dropped)."""
    s = F(src); out = []
    for t, x in bt.subrecords(bt.e.data(s)):
        if t == "EDID": out.append(sr("EDID", edid))
        elif t == "MODL": out.append(sr("MODL", model))
        elif t == "MODT": continue
        else: out.append(sr(t, x))
    f = P.fid(edid)
    P.top.setdefault("STAT", []).append(record("STAT", f, out))
    return f


def build():
    lamp = bt.ligh("VSAlphaLampSteady", "BasementLightKeyNeutral", 700, (255, 240, 215))
    c = bt.Cell(CELL, "Vegas Shaders Alpha Lamp Test", bt.xcll((20, 20, 22), (14, 14, 16), (10, 10, 12), fog_far=6000.0))
    W, H = 2048, 1536
    c.room(0, 0, 8, 6)
    c.navmesh(48, 48, W - 48, H - 48, 0.0)
    FX, FY = 1024, 820
    c.ref(F("CLFence01"), FX, FY, 0, 0)          # the chain-link fence, across the room (its length along x)
    c.ref(lamp, FX, FY + 160, 110)                # the lamp behind it, chest height: the shadow falls toward the camera
    c.ref(F("PBFence01"), FX + 520, FY, 0, 0)     # an opaque wooden fence beside it (its shadow is there with AlphaTest 0 too)
    c.ref(lamp, FX + 520, FY + 160, 110)
    # (2026-11-06, the user: a near-mirror floor for demonstrations) the Ultra-Luxe polished marble disc (ULInnerFloor is one quarter,
    # +x / -y of its origin, radius 713; four turns make the disc), 1 unit over the office floor, under the fences and the cameras' view.
    # With the bench toml rtfull	oml-alpha-mirror.toml (RT reflections + the Mirror look) it reflects the fences, the lamps and you.
    MX, MY = 1200, 760
    for rz in (0, 90, 180, 270):
        c.ref(F("ULInnerFloor"), MX, MY, 1, rz)
    # (2026-11-06, the user: "a true mirror object just for testing", then "a flat plane, not a broken piece of glass") a STANDING
    # MIRROR near the south wall: the Ultra-Luxe wall screen (NVULWallScreen01: a flat 112 x 208 screen, two quads, in a gold frame,
    # upright, facing +-x), turned to face +-y and scaled 1.5 (168 x 312). Its texture nv_ul_wallscreen_d gets a material map
    # (pbr\...\nv_ul_wallscreen_rmaos.dds, a copy of the marble's) so RoughnessOverrides "nv_ul_wallscreen:0.02" makes it a mirror.
    # The lamp above the space in front of it lights you.
    # (later) the screen is a cut-out lattice: our own model vs_mirror01.nif = the screen with its alpha test switched off (NiAlphaProperty
    # flags 0x12EC -> 0x10EC): one solid flat quad in the gold frame. Ships beside the ESP: Meshes/VegasShaders/Test/vs_mirror01.nif.
    MRX, MRY = 1040, 150
    mirror = stat_copy("VSMirrorPanel01", "NVULWallScreen01", "VegasShaders/Test/vs_mirror01.nif".replace("/", chr(92)))
    # (later, the user: "make several and have them connected: an almost full mirror wall") five panels side by side, 162 apart (each
    # is 168 wide at scale 1.5; the frames overlap a little so the seams are single thin gold lines): a wall 816 wide, 312 tall.
    for k in range(-2, 3):
        c.ref(mirror, MRX + 162 * k, MRY, 0.0, 90, scale=1.5)
    c.ref(lamp, MRX, MRY + 260, 230)
    for k, (x, y, yaw) in enumerate([(FX + 260, FY - 520, 0), (FX, FY - 300, 0), (MRX, MRY + 170, 180)]):
        f = c.ref(0x34, x, y, 0.0, yaw, edid="VSAlphaLampCam%02d" % (k + 1), persistent=True)   # XMarkerHeading
        P.views.append({"marker": "VSAlphaLampCam%02d" % (k + 1), "object_id": "%06X" % (f & 0xFFFFFF)})
    if bt.missing: sys.exit("missing editor IDs in FalloutNV.esm: %s" % ", ".join(bt.missing))
    return c


def write(out_dir):
    body = b""; nrec = 0; ngrp = 0
    for label in ("STAT", "LIGH"):
        recs = P.top.get(label, [])
        if recs: body += group(label, 0, b"".join(recs)); nrec += len(recs); ngrp += 1
    c = P.cells[0]; oid = c.fid & 0xFFFFFF
    kids = group(c.fid, 8, b"".join(c.pers)) + group(c.fid, 9, b"".join(c.temp + c.navm)); ngrp += 2
    cg = group(oid % 10, 2, group((oid // 10) % 10, 3, c.record() + group(c.fid, 6, kids))); ngrp += 4
    nrec += 1 + len(c.pers) + len(c.temp) + len(c.navm)
    body += group("CELL", 0, cg)
    hdr = header(nrec + ngrp, P.next, "Vegas Shaders (Claude Code)",
                 "Vegas Shaders bench test: RT lamp shadows from alpha-tested casters (vanilla assets). coc VSAlphaLampTest; player.moveto VSAlphaLampCam01.",
                 ["FalloutNV.esm"])
    os.makedirs(out_dir, exist_ok=True)
    p = os.path.join(out_dir, PLUGIN)
    with open(p + ".tmp", "wb") as f: f.write(hdr + body)
    os.replace(p + ".tmp", p)
    print("wrote %s (%d bytes, %d records, %d groups), views %s" % (p, len(hdr + body), nrec, ngrp, P.views))


if __name__ == "__main__":
    ap = argparse.ArgumentParser(); ap.add_argument("--out", default=os.path.join(HERE, "out-alphalamp"))
    a = ap.parse_args()
    build(); write(a.out)
