#!/usr/bin/env python3
"""UNOFFICIAL 2026-11-15 smooth sun under the full deferred renderer (docs/next/FD-SMOOTHSUN-RESULTS.md): a template_compile --batch list for
the DeferredSun pass under the define list ShaderRecord::BuildDefines gives with DeferredRenderer's prerequisites on (tests/template_defines.py),
with and without the sun cross-fade (core/DeferredSun.h adds SUN_CROSSFADE 1 + SUN_CROSSFADE_PASS 1 through core/SunFadeDeferred.h), native and
fork, with the pass's optional branches (FD_SKIN, FD_HAIR, FD_FOLIAGE, DS_DEPTH_AGREE, DS_RT_SUN_FIRST) and RT sun shadows' ForwardLookup.
Usage: fd_smoothsun_compile.py OUT_LIST OUT_DIR [SHADERS_DIR [off]]    then    template_compile.exe --flow --device --batch OUT_LIST
  SHADERS_DIR: another Shaders folder (a base commit's, for the byte compare); "off": only the builds with the cross-fade off.
"""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import template_defines as td

ROOT = os.path.dirname(HERE)
SH = os.path.join(ROOT, "src", "hlsl", "NewVegas", "Shaders")
PREREQ = ["Main.Main.Quality.CapturedNormals=1", "Main.Main.Quality.MaterialBuffer=1", "Main.Main.Quality.AlbedoBuffer=1",
          "Shaders.PBR.Linear.Enabled=1", "Shaders.PBR.Linear.LinearScene=1", "Main.Main.Misc.FoldFog=1",
          "Main.Main.Misc.LampTableStandalone=1", "Main.Main.Misc.LampTableFast=1", "Shaders.ShadowsInteriors.Forward.Enabled=1",
          "Shaders.ShadowsInteriors.Atlas.Enabled=1", "Shaders.ShadowsInteriors.Deferred.Enabled=1",
          "Shaders.ShadowsInteriors.Deferred.LightFreeRecords=1"]
CLOUDS = ["Shaders.VolumetricClouds.Main.ForwardShadows=1", "Shaders.VolumetricClouds.Status.Enabled=1"]
FADE = [("SUN_CROSSFADE", "1"), ("SUN_CROSSFADE_PASS", "1")]

class Opt: pass

def opt_for(runtime, rt_sun):
    opt = Opt(); opt.reversed_depth = 1; opt.rt = 0; opt.runtime = runtime; opt.dev = False; opt.native_manylamps = False
    opt.displace_objects = False; opt.record = "default"; opt.rt_sun = rt_sun
    return opt

# (tag, runtime, GBufferV2, RT sun ForwardLookup, extra settings, the pass's own optional defines)
SETS = [
    ("nat-noV2",        "native", False, False, [],     []),
    # what DeferredRenderer 2 forces (core/SettingForce.h: GBufferV2, DeferredFoliage, DeferredDistant; DeferredSkin / DeferredHair stay off)
    ("nat-playtest",    "native", True,  False, [],     [("FD_FOLIAGE", "1"), ("DS_DEPTH_AGREE", "1")]),
    ("fork-playtest",   "dxvk",   True,  False, [],     [("FD_FOLIAGE", "1"), ("DS_DEPTH_AGREE", "1")]),
    ("nat-v2-skinhair", "native", True,  False, [],     [("FD_SKIN", "1"), ("FD_HAIR", "1")]),
    ("nat-v2-all",      "native", True,  False, [],     [("FD_FOLIAGE", "1"), ("DS_DEPTH_AGREE", "1"), ("FD_SKIN", "1"), ("FD_HAIR", "1")]),
    ("fork-v2-rt",      "dxvk",   True,  True,  CLOUDS, [("FD_SKIN", "1"), ("FD_HAIR", "1")]),
    ("fork-v2-rtfirst", "dxvk",   True,  True,  CLOUDS, [("DS_RT_SUN_FIRST", "1"), ("FD_SKIN", "1"), ("FD_HAIR", "1")]),
    ("fork-v2-rt-all",  "dxvk",   True,  True,  CLOUDS, [("FD_FOLIAGE", "1"), ("DS_DEPTH_AGREE", "1"), ("DS_RT_SUN_FIRST", "1"), ("FD_SKIN", "1"), ("FD_HAIR", "1")]),
]

def main():
    global SH
    out_list, out_dir = sys.argv[1], sys.argv[2]
    if len(sys.argv) > 3: SH = sys.argv[3]
    only_off = len(sys.argv) > 4 and sys.argv[4] == "off"
    os.makedirs(out_dir, exist_ok=True)
    lines = []
    for tag, runtime, v2, rt_sun, extra, own in SETS:
        s = td.Settings([], PREREQ + extra + (["Main.Main.Quality.GBufferV2=1"] if v2 else []))
        for fade in ((False,) if only_off else (False, True)):
            ov = [("INTERIOR_SHADOWS", "0")] + (FADE if fade else []) + own
            d = td.build(s, [], ov, opt_for(runtime, rt_sun))
            if v2: d = td.apply_forces(d, ["CAPTURED_NORMALS_ON=4"], [])
            if d.dropped: print("# %s: DROPPED past the 62 cap: %s" % (tag, " ".join(d.dropped)))
            name = "%s/%s/sun" % (tag, "fade" if fade else "off")
            lines.append("\t".join([name, "ps_3_0", os.path.join(SH, "DeferredSun.hlsl"), os.path.join(out_dir, name.replace("/", "_") + ".bin"), td.define_words(d)]))
    with open(out_list, "w", encoding="utf-8") as f: f.write("\n".join(lines) + "\n")
    print("# %d batch lines -> %s" % (len(lines), out_list))

if __name__ == "__main__": main()
