#!/usr/bin/env python3
"""UNOFFICIAL 2026-11-15 fd-lightpass (docs/next/FD-LIGHTPASS-RESULTS.md): a template_compile --batch list for the deferred light passes this
branch changed (the half-pixel follow-up: DeferredLamps.hlsl's half-float branch and depth line, FlashlightDeferred.hlsl), under the define
list ShaderRecord::BuildDefines gives with DeferredRenderer's prerequisites on (tests/template_defines.py, as tests/fd_firstperson_compile.py),
GBufferV2 (CAPTURED_NORMALS_ON 4), native and fork, with DL_LINEAR_DEPTH (DeferredDistant) off and on: the lamp pass, the shadow pass, the
skin lamp pass, the fog resolve (core/DeferredLamps.h EnsurePass / EnsureShadowShader) and the flashlight pass (core/FlashlightDeferred.h).
Usage: fd_lightpass_compile.py OUT_LIST OUT_DIR [SHADER_ROOT]    then    template_compile.exe --flow --device --batch OUT_LIST
SHADER_ROOT (default: this tree's src/hlsl/NewVegas/Shaders) compiles another copy of the shaders with the same lines (the base's, for a
before / after).
"""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import template_defines as td

ROOT = os.path.dirname(HERE)
PREREQ = ["Main.Main.Quality.CapturedNormals=1", "Main.Main.Quality.MaterialBuffer=1", "Main.Main.Quality.AlbedoBuffer=1",
          "Main.Main.Quality.GBufferV2=1", "Shaders.PBR.Linear.Enabled=1", "Shaders.PBR.Linear.LinearScene=1", "Main.Main.Misc.FoldFog=1",
          "Main.Main.Misc.LampTableStandalone=1", "Main.Main.Misc.LampTableFast=1", "Shaders.ShadowsInteriors.Forward.Enabled=1",
          "Shaders.ShadowsInteriors.Atlas.Enabled=1", "Shaders.ShadowsInteriors.Deferred.Enabled=1",
          "Shaders.ShadowsInteriors.Deferred.LightFreeRecords=1"]

class Opt: pass
def opt_for(runtime):
    opt = Opt(); opt.reversed_depth = 1; opt.rt = 0; opt.runtime = runtime; opt.dev = False; opt.native_manylamps = False
    opt.displace_objects = False; opt.record = "default"
    return opt

def main():
    out_list, out_dir = sys.argv[1], sys.argv[2]
    sh = sys.argv[3] if len(sys.argv) > 3 else os.path.join(ROOT, "src", "hlsl", "NewVegas", "Shaders")
    os.makedirs(out_dir, exist_ok=True)
    s = td.Settings([], PREREQ)
    lines = []
    def add(tag, src, words):
        lines.append("\t".join([tag, "ps_3_0", src, os.path.join(out_dir, tag.replace("/", "_") + ".bin"), words]))
    for runtime in ("native", "dxvk"):
        opt = opt_for(runtime)
        lamp = [("INTERIOR_SHADOWS", "1"), ("FORWARD_SHADOWS", "0"), ("SUN_CROSSFADE", "0"), ("CONTACT_HARDENING", "0")]
        # DeferredRenderer 2 as played (5464b565a): GBufferV2, DeferredDistant (DL_LINEAR_DEPTH) and DeferredFoliage forced on, DeferredSkin
        # off (FD_SKIN 0); the other combinations too
        for skin in ("0", "1"):
            extra = [("POINT_SHADOW_ATLAS", "1"), ("FD_LIGHT_FREE", "1"), ("LIGHT_COMMON_LIST_TEXELS", "16.0f")] + ([("FD_SKIN", "1")] if skin == "1" else [])
            for lz in ("0", "1"):
                z = [("DL_LINEAR_DEPTH", "1")] if lz == "1" else []
                passes = [("dlpass", "DeferredLamps", lamp + [("POINT_SHADOW_FIT", "3")] + extra + z),
                          ("dlshadow", "DeferredLamps", lamp + [("DL_SHADOW_PASS", "1"), ("POINT_SHADOW_FIT", "0")] + extra + z),
                          ("dlfog", "DeferredLamps", lamp + [("POINT_SHADOW_FIT", "3")] + extra + z + [("DL_FOG_RESOLVE", "1")])]
                if skin == "1": passes.append(("dlskin", "DeferredLamps", lamp + [("POINT_SHADOW_FIT", "3")] + extra + z + [("DL_SKIN_ONLY", "1")]))
                for tag, file, ov in passes:
                    d = td.apply_forces(td.build(s, [], ov, opt), ["CAPTURED_NORMALS_ON=4"], [])
                    if d.dropped: print("# %s: DROPPED past the 62 cap: %s" % (tag, " ".join(d.dropped)))
                    add("%s/%s-skin%s-lz%s" % (runtime, tag, skin, lz), os.path.join(sh, file + ".hlsl"), td.define_words(d))
        for fol in ("0", "1"):
            fl = [("INTERIOR_SHADOWS", "0"), ("FORWARD_SHADOWS", "0"), ("SUN_CROSSFADE", "0"), ("CONTACT_HARDENING", "0"), ("FD_LIGHT_FREE", "1")] + ([("FD_FOLIAGE", "1")] if fol == "1" else [])
            d = td.apply_forces(td.build(s, [], fl, opt), ["CAPTURED_NORMALS_ON=4"], [])
            add("%s/flashlight-foliage%s" % (runtime, fol), os.path.join(sh, "FlashlightDeferred.hlsl"), td.define_words(d))
    with open(out_list, "w", encoding="utf-8") as f: f.write("\n".join(lines) + "\n")
    print("# %d batch lines -> %s" % (len(lines), out_list))

if __name__ == "__main__": main()
