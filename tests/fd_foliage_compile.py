#!/usr/bin/env python3
"""UNOFFICIAL 2026-11-14 full deferred renderer, phase 3 foliage (docs/next/FD-FOLIAGE-RESULTS.md): a template_compile --batch list for every
shader the foliage branch touches, under the define list ShaderRecord::BuildDefines gives with DeferredRenderer 2's prerequisites on
(tests/template_defines.py build(), no template pairs: these shaders are their own files), GBufferV2 (CAPTURED_NORMALS_ON 4), native and fork:
  - the leaf and grass pixel shaders, forward (as today) and their deferred records (FOLIAGE_DEFERRED 1)
  - DeferredSun.hlsl (INTERIOR_SHADOWS 0) and FlashlightDeferred.hlsl (its overrides) with and without FD_FOLIAGE (+ DS_DEPTH_AGREE)
Usage: fd_foliage_compile.py OUT_LIST OUT_DIR [SHADERS_DIR [off]]    then    template_compile.exe --flow --device --batch OUT_LIST
  SHADERS_DIR: another Shaders folder (a base commit's, for the byte compare); "off": only the builds with every new define off
  (the leaf / grass forward builds, DeferredSun / FlashlightDeferred without FD_FOLIAGE): compare their .bin with the base's.
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

class Opt: pass

def defines(runtime, extra):
    opt = Opt(); opt.reversed_depth = 1; opt.rt = 0; opt.runtime = runtime; opt.dev = False; opt.native_manylamps = False
    opt.displace_objects = False; opt.record = "default"
    s = td.Settings([], PREREQ)
    d = td.build(s, [], [(k, v) for k, v in extra], opt)
    d = td.apply_forces(d, ["CAPTURED_NORMALS_ON=4"], [])
    return d

def main():
    global SH
    out_list, out_dir = sys.argv[1], sys.argv[2]
    if len(sys.argv) > 3: SH = sys.argv[3]
    only_off = len(sys.argv) > 4 and sys.argv[4] == "off"
    os.makedirs(out_dir, exist_ok=True)
    lines = []
    for runtime in ("native", "dxvk"):
        for name in ("STLEAF2000", "STLEAF2000TMS", "STLEAF2001", "STLEAF2001TMS", "GRASS23x000", "GRASS23x000TMS"):
            for kind, extra in (("fwd", []),) + ((("def", [("FOLIAGE_DEFERRED", "1")]),) if not only_off else ()):
                d = defines(runtime, extra)
                lines.append("\t".join(["%s/%s/%s" % (runtime, kind, name), "ps_3_0", os.path.join(SH, name + ".pso.hlsl"),
                                        os.path.join(out_dir, "%s_%s_%s.bin" % (runtime, kind, name)), td.define_words(d)]))
        for kind, extra in (("off", []),) + ((("fol", [("FD_FOLIAGE", "1")]), ("folagree", [("FD_FOLIAGE", "1"), ("DS_DEPTH_AGREE", "1")])) if not only_off else ()):
            d = defines(runtime, [("INTERIOR_SHADOWS", "0")] + extra)
            lines.append("\t".join(["%s/%s/DeferredSun" % (runtime, kind), "ps_3_0", os.path.join(SH, "DeferredSun.hlsl"),
                                    os.path.join(out_dir, "%s_%s_DeferredSun.bin" % (runtime, kind)), td.define_words(d)]))
            d = defines(runtime, [("INTERIOR_SHADOWS", "0"), ("FORWARD_SHADOWS", "0"), ("SUN_CROSSFADE", "0"), ("CONTACT_HARDENING", "0"), ("FD_LIGHT_FREE", "1")] + extra)
            lines.append("\t".join(["%s/%s/FlashlightDeferred" % (runtime, kind), "ps_3_0", os.path.join(SH, "FlashlightDeferred.hlsl"),
                                    os.path.join(out_dir, "%s_%s_FlashlightDeferred.bin" % (runtime, kind)), td.define_words(d)]))
    with open(out_list, "w", encoding="utf-8") as f: f.write("\n".join(lines) + "\n")
    print("# %d batch lines -> %s" % (len(lines), out_list))

if __name__ == "__main__": main()
