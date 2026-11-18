#!/usr/bin/env python3
"""UNOFFICIAL 2026-11-15 full deferred renderer, phase 4 skin (docs/next/FD-SKIN-RESULTS.md): a template_compile --batch list for every shader
DeferredSkin touches, under the define list ShaderRecord::BuildDefines gives with DeferredRenderer's prerequisites on (tests/template_defines.py),
GBufferV2 (CAPTURED_NORMALS_ON 4), native and fork:
  - the skin pixel shaders SKIN2000-2003 and SKIN2010-2012: forward (the Interior record where the name has one, else the default), the deferred
    records LAMPS_DEFERRED 2 (DeferredRenderer 2) and 3 (DeferredRenderer 1), as core/DeferredSkin.h loads them (the default overrides + that)
  - DeferredSun.hlsl (INTERIOR_SHADOWS 0) and DeferredLamps.hlsl (lamp pass FIT 3 and shadow pass FIT 0, as DeferredLamps.h loads them) with
    and without FD_SKIN, and the skin lamp pass (FD_SKIN + DL_SKIN_ONLY)
Usage: fd_skin_compile.py OUT_LIST OUT_DIR [SHADERS_DIR [off]]    then    template_compile.exe --flow --device --batch OUT_LIST
  SHADERS_DIR: another Shaders folder (a base commit's, for the byte compare); "off": only the builds with every new define off.
"""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import template_defines as td

ROOT = os.path.dirname(HERE)
SH = os.path.join(ROOT, "src", "hlsl", "NewVegas", "Shaders")
PREREQ = ["Main.Main.Quality.CapturedNormals=1", "Main.Main.Quality.MaterialBuffer=1", "Main.Main.Quality.AlbedoBuffer=1",
          "Main.Main.Quality.GBufferV2=1", "Shaders.PBR.Linear.Enabled=1", "Shaders.PBR.Linear.LinearScene=1", "Main.Main.Misc.FoldFog=1",
          "Main.Main.Misc.LampTableStandalone=1", "Main.Main.Misc.LampTableFast=1", "Shaders.ShadowsInteriors.Forward.Enabled=1",
          "Shaders.ShadowsInteriors.Atlas.Enabled=1", "Shaders.ShadowsInteriors.Deferred.Enabled=1",
          "Shaders.ShadowsInteriors.Deferred.LightFreeRecords=1"]
NAMES = ("SKIN2000", "SKIN2001", "SKIN2002", "SKIN2003", "SKIN2010", "SKIN2011", "SKIN2012")
HAIR = ("SLS2009", "SLS2010", "SLS2013", "SLS2016", "SLS2019", "SLS2022")	# ObjectTemplate HAIR (DeferredHair)

class Opt: pass

def opt_for(runtime):
    opt = Opt(); opt.reversed_depth = 1; opt.rt = 0; opt.runtime = runtime; opt.dev = False; opt.native_manylamps = False
    opt.displace_objects = False; opt.record = "default"
    return opt

def main():
    global SH
    out_list, out_dir = sys.argv[1], sys.argv[2]
    if len(sys.argv) > 3: SH = sys.argv[3]
    only_off = len(sys.argv) > 4 and sys.argv[4] == "off"
    os.makedirs(out_dir, exist_ok=True)
    s = td.Settings([], PREREQ)
    recs = {r[0]: r for r in td.records() if r[1] == "SkinTemplate"}
    lines = []
    def add(tag, src, d):
        if d.dropped: print("# %s: DROPPED past the 62 cap: %s" % (tag, " ".join(d.dropped)))
        lines.append("\t".join([tag, "ps_3_0", src, os.path.join(out_dir, tag.replace("/", "_") + ".bin"), td.define_words(d)]))
    for runtime in ("native", "dxvk"):
        opt = opt_for(runtime)
        for n in NAMES:
            name, template, pairs, _ = recs[n + ".pso"]
            interior = td.eligible_interior(name, template)
            kinds = [("fwd", td.overrides_for("interior", s) if interior else [])]
            if interior: kinds.append(("fwdext", []))
            if not only_off: kinds += [("ld2", [("LAMPS_DEFERRED", "2")]), ("ld3", [("LAMPS_DEFERRED", "3")])]
            for kind, ov in kinds:
                d = td.apply_forces(td.build(s, pairs, ov, opt), ["CAPTURED_NORMALS_ON=4"], [])
                add("%s/%s/%s" % (runtime, kind, n), os.path.join(SH, "SkinTemplate.hlsl"), d)
        # DeferredHair: the object template's HAIR records, forward and their deferred records (DeferredRenderer 2: LAMPS_DEFERRED 2; 1: the
        # Interior overrides + LAMPS_DEFERRED 3, else LAMPS_DEFERRED 3 alone)
        orecs = {r[0]: r for r in td.records() if r[1] == "ObjectTemplate"}
        for n in HAIR:
            name, template, pairs, _ = orecs[n + ".pso"]
            interior = td.eligible_interior(name, template)
            kinds = [("fwd", td.overrides_for("interior", s) if interior else [])]
            if not only_off: kinds += [("hlf", [("LAMPS_DEFERRED", "2")]), ("hld3", (td.overrides_for("interior", s) if interior else []) + [("LAMPS_DEFERRED", "3")])]
            for kind, ov in kinds:
                d = td.apply_forces(td.build(s, pairs, ov, opt), ["CAPTURED_NORMALS_ON=4"], [])
                add("%s/%s/%s" % (runtime, kind, n), os.path.join(SH, "ObjectTemplate.hlsl"), d)
        if not only_off:
            for tag, sk in (("hair", [("FD_HAIR", "1")]), ("skinhair", [("FD_SKIN", "1"), ("FD_HAIR", "1")])):
                d = td.apply_forces(td.build(s, [], [("INTERIOR_SHADOWS", "0")] + sk, opt), ["CAPTURED_NORMALS_ON=4"], [])
                add("%s/%s/sun" % (runtime, tag), os.path.join(SH, "DeferredSun.hlsl"), d)
        lamp = [("INTERIOR_SHADOWS", "1"), ("FORWARD_SHADOWS", "0"), ("SUN_CROSSFADE", "0"), ("CONTACT_HARDENING", "0")]
        extra = [("POINT_SHADOW_ATLAS", "1"), ("FD_LIGHT_FREE", "1"), ("LIGHT_COMMON_LIST_TEXELS", "16.0f")]
        for kind, sk in (("off", []),) + ((("skin", [("FD_SKIN", "1")]),) if not only_off else ()):
            passes = [("sun", "DeferredSun", [("INTERIOR_SHADOWS", "0")]),
                      ("dlpass", "DeferredLamps", lamp + [("POINT_SHADOW_FIT", "3")] + extra),
                      ("dlshadow", "DeferredLamps", lamp + [("DL_SHADOW_PASS", "1"), ("POINT_SHADOW_FIT", "0")] + extra)]
            if sk: passes.append(("dlskin", "DeferredLamps", lamp + [("POINT_SHADOW_FIT", "3")] + extra + [("DL_SKIN_ONLY", "1")]))	# the skin lamp pass
            if sk:	# SSS v2's two passes (Shaders/DeferredSkinSSS.hlsl), as DeferredSkin::SssLoad loads them
                passes += [("sssprep", "DeferredSkinSSS", [("SSS_PREP", "1")]), ("sssblur", "DeferredSkinSSS", [])]
            for tag, file, ov in passes:
                d = td.apply_forces(td.build(s, [], ov + sk, opt), ["CAPTURED_NORMALS_ON=4"], [])
                add("%s/%s/%s" % (runtime, kind, tag), os.path.join(SH, file + ".hlsl"), d)
    with open(out_list, "w", encoding="utf-8") as f: f.write("\n".join(lines) + "\n")
    print("# %d batch lines -> %s" % (len(lines), out_list))

if __name__ == "__main__": main()
