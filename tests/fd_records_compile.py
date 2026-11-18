#!/usr/bin/env python3
"""UNOFFICIAL 2026-11-14 (docs/next/FULL-DEFERRED-RENDERER-PLAN.md phase 0, question 1; docs/next/FD-RECORDS-RESULTS.md): do the
lighting-free records (LAMPS_DEFERRED 2: no sun, no lamp, no shadow lookups; ambient, glow, fog and the G-buffer kept) fit native D3D9's
instruction-slot cap at the full shadow filter (POINT_SHADOW_FIT 0)? Compiles, with the game's D3DX and the stage-0c method
(tests/stage0c_compile.py: tests/template_compile.exe --flow --batch [--device], define lists from tests/template_defines.py), each record
kind below for every lit pixel shader of the Object, Parallax and Terrain templates (or the names given), under one option set:

  fwd      today's forward record: the Interior record (interior overrides) for the names that get one, else the default record
  fwd-ext  the default (exterior) record, for the names that also have an Interior one
  ld1      the DeferredLamps record (Interior overrides + LAMPS_DEFERRED 1: lamps out, the sun kept)  [interior names only]
  ld3      the lamps-deferred record of every lit family (Interior overrides + LAMPS_DEFERRED 3; DeferredRenderer 1)  [interior names only]
  ld3x     LAMPS_DEFERRED 3 alone (DeferredRenderer 1 for a name with no Interior record, e.g. parallax)
  lf       the lighting-free record (LAMPS_DEFERRED 2 alone: one record indoors and out)
  lf-int   the lighting-free record built on the Interior overrides (INTERIOR_SHADOWS 1 ...) for comparison  [interior names only]

Option sets (--set-name): release (the repo's defaults, native), heavy (release + POINT_SHADOW_ATLAS, HandBlood, POINT_SHADOW_RT,
MaterialBuffer + AlbedoBuffer, the many-lamp shaders with the lamp table: MANY_LAMPS_ON 2, i.e. the stage-0c worst case), user (--toml FILE,
native), plus any --force / --set.

Usage: fd_records_compile.py --exe ...template_compile.exe --shaders src/hlsl/NewVegas/Shaders --out DIR --set-name release|heavy|user
                             [--toml FILE] [--kinds fwd,lf,...] [--names SLS2035.pso ...] [--no-device]
Prints one line a record: kind/name OK slots=N ms=M flow=0|1 defines=K device=ok|refused(...)  (template_compile's own line)
"""
import argparse, os, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import template_defines as td

TEMPLATES = ("ObjectTemplate", "ParallaxTemplate", "TerrainTemplate")
HEAVY_FORCES = ["POINT_SHADOW_ATLAS=1", "HAND_BLOOD=1", "POINT_SHADOW_RT=1"]
HEAVY_SETS = ["Main.Main.Quality.CapturedNormals=true", "Main.Main.Quality.MaterialBuffer=true", "Main.Main.Quality.AlbedoBuffer=true",
              "Main.Main.Misc.ManyLampsOnePass=true", "Main.Main.Misc.MoreLampsPerObject=true", "Main.Main.Misc.LightCommonLamps=true",
              "Shaders.HandBlood.Status.Enabled=true"]

def overrides(kind, s):
    if kind in ("fwd-int", "ld1", "ld3", "lf-int", "fp", "fp5"):
        base = td.overrides_for("interior", s)
        if kind == "ld1": base = base + [("LAMPS_DEFERRED", "1")]
        if kind == "ld3": base = base + [("LAMPS_DEFERRED", "3")]	# 2026-11-14 phase 2: the lamps-deferred record of every lit family (DeferredRenderer 1)
        if kind == "fp": base = base + [("LAMPS_DEFERRED", "4")]	# 2026-11-15 phase 5 (DeferredTransparents): the forward+ record of a blended draw
        if kind == "fp5": base = base + [("LAMPS_DEFERRED", "5")]	# ... its unshadowed fallback
        if kind == "lf-int": base = base + [("LAMPS_DEFERRED", "2")]
        return base
    if kind == "lf": return [("LAMPS_DEFERRED", "2")]
    if kind == "ld3x": return [("LAMPS_DEFERRED", "3")]	# 2026-11-14: DeferredRenderer 1 for a name without an Interior record (parallax)
    return []	# fwd-ext / default

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True); ap.add_argument("--shaders", required=True); ap.add_argument("--out", required=True)
    ap.add_argument("--set-name", default="release"); ap.add_argument("--toml", action="append", default=[])
    ap.add_argument("--set", action="append", default=[]); ap.add_argument("--force", action="append", default=[])
    ap.add_argument("--kinds", default="fwd,fwd-ext,ld1,lf,lf-int"); ap.add_argument("--names", nargs="*", default=[])
    ap.add_argument("--no-device", action="store_true"); ap.add_argument("--runtime", default="native")
    ap.add_argument("--passes", action="store_true")	# also the DeferredLamps lamp / shadow passes and the DeferredSun pass
    # 2026-11-14 (fd-lod): other templates, e.g. --templates TerrainLODTemplate --force DISTANT_GBUFFER=1 (DeferredDistant's far land)
    ap.add_argument("--templates", default=",".join(TEMPLATES))
    a = ap.parse_args(); a.exe = os.path.abspath(a.exe)
    os.makedirs(a.out, exist_ok=True)
    sets, forces = list(a.set), list(a.force)
    class Opt: pass
    opt = Opt(); opt.reversed_depth = 1; opt.rt = 0; opt.runtime = a.runtime; opt.native_manylamps = False; opt.dev = False; opt.displace_objects = False
    if a.set_name == "heavy":
        sets = HEAVY_SETS + sets; forces = HEAVY_FORCES + forces; opt.native_manylamps = True
    s = td.Settings(a.toml, sets)
    templates = tuple(t for t in a.templates.split(",") if t)
    recs = [r for r in td.records() if r[0].endswith(".pso") and r[1] in templates]
    if a.names: recs = [r for r in recs if r[0] in a.names]
    lines = []
    for name, template, pairs, _ in recs:
        interior = td.eligible_interior(name, template)
        for kind in a.kinds.split(","):
            if kind in ("ld1", "ld3", "lf-int", "fwd-ext", "fp", "fp5") and not interior: continue
            # 2026-11-15 DeferredTransparents::Eligible: the Object template's lit one-pass / base records (no added-light, light-only or hair pass)
            if kind.startswith("fp") and (template != "ObjectTemplate" or any(k in ("DIFFUSE", "ONLY_SPECULAR", "NO_LIGHT", "HAIR", "ONLY_LIGHT") for k, _ in pairs)): continue
            if (kind.startswith("lf") or kind.startswith("ld3")) and any(k in ("DIFFUSE", "ONLY_SPECULAR", "NO_LIGHT") for k, _ in pairs): continue
            if kind.startswith("ld3") and any(k == "HAIR" for k, _ in pairs): continue	# (DeferredRenderer::Eligible: hair stays forward)	# (LightFreeRecords / DeferredRenderer::Eligible: the added-light passes a deferred frame skips, and hair, have no deferred record)
            k = kind
            if kind == "fwd": k = "fwd-int" if interior else "fwd-ext"
            d = td.apply_forces(td.build(s, pairs, overrides(k, s), opt), forces, [])
            if d.dropped: print("# %s/%s: DROPPED past the 62 cap: %s" % (kind, name, " ".join(d.dropped)))
            lines.append("\t".join(["%s/%s" % (kind, name), "ps_3_0", os.path.join(a.shaders, template + ".hlsl"), "-", td.define_words(d)]))
    if a.passes:	# 2026-11-14 (fd-interiors): the light passes as DeferredLamps.h / DeferredSun.h load them (the deferred renderer on: atlas for the pass, FD_LIGHT_FREE)
        lamp = [("INTERIOR_SHADOWS", "1"), ("FORWARD_SHADOWS", "0"), ("SUN_CROSSFADE", "0"), ("CONTACT_HARDENING", "0")]
        extra = [("POINT_SHADOW_ATLAS", "1"), ("FD_LIGHT_FREE", "1"), ("LIGHT_COMMON_LIST_TEXELS", "16.0f")]
        for kind, file, ov in [("dlpass", "DeferredLamps", lamp + [("POINT_SHADOW_FIT", "3")] + extra),
                               ("dlshadow", "DeferredLamps", lamp + [("DL_SHADOW_PASS", "1"), ("POINT_SHADOW_FIT", "0")] + extra),
                               ("sunpass", "DeferredSun", [("INTERIOR_SHADOWS", "0")])]:
            d = td.apply_forces(td.build(s, [], ov, opt), forces, [])
            lines.append("\t".join(["%s/%s.pso" % (kind, file), "ps_3_0", os.path.join(a.shaders, file + ".hlsl"), "-", td.define_words(d)]))
    batch = os.path.join(a.out, "fdrecords-%s.batch.txt" % a.set_name)
    open(batch, "w", encoding="utf-8").write("\n".join(lines) + "\n")
    log = os.path.join(a.out, "fdrecords-%s.log" % a.set_name)
    with open(log, "w", encoding="utf-8") as f:
        subprocess.run([a.exe, "--flow", "--batch", batch] + ([] if a.no_device else ["--device"]), stdout=f, stderr=subprocess.STDOUT, text=True)
    print(open(log, encoding="utf-8", errors="replace").read())
    print("set:", a.set_name, "| records:", len(lines), "| log:", log)

if __name__ == "__main__": main()
