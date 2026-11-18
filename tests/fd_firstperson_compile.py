#!/usr/bin/env python3
"""UNOFFICIAL 2026-11-15 DeferredFirstPerson + GearWeather (docs/next/FD-FIRSTPERSON-RESULTS.md): a template_compile --batch list for every
shader the first-person deferred path draws with, under the define list ShaderRecord::BuildDefines gives with DeferredRenderer's prerequisites
on (tests/template_defines.py), GBufferV2 (CAPTURED_NORMALS_ON 4), native and fork:
  - Shaders/GearWeather.hlsl (new: GearWeather::Load, the global define list, no override);
  - the two C++-compiled passes of core/DeferredFirstPerson.h (the resolve and the mark), written out to OUT_DIR as .hlsl first;
  - the light passes the view model is lit by again, their sources unchanged on this branch (DeferredSun with FD_SKIN / FD_HAIR, the lamp
    pass, the shadow pass, the skin lamp pass, as core/DeferredLamps.h loads them): the same bytecode the world's passes use.
Usage: fd_firstperson_compile.py OUT_LIST OUT_DIR    then    template_compile.exe --flow --device --batch OUT_LIST
"""
import os, re, sys
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
def opt_for(runtime):
    opt = Opt(); opt.reversed_depth = 1; opt.rt = 0; opt.runtime = runtime; opt.dev = False; opt.native_manylamps = False
    opt.displace_objects = False; opt.record = "default"
    return opt

def cpp_shaders(out_dir):
    """The two strings of core/DeferredFirstPerson.h Load, out of the source (so the check compiles exactly what the DLL compiles)."""
    src = open(os.path.join(ROOT, "src", "core", "DeferredFirstPerson.h"), encoding="utf-8").read()
    out = []
    for name in ("kResolve", "kNull"):
        m = re.search(r'static const char\* ' + name + r' =\s*((?:"(?:[^"\\]|\\.)*"\s*)+);', src)
        if not m: raise SystemExit("could not find %s in DeferredFirstPerson.h" % name)
        text = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1)))
        path = os.path.join(out_dir, "fp_%s.hlsl" % name)
        with open(path, "w", encoding="utf-8") as f: f.write(text + "\n")
        out.append((name, path))
    return out

def main():
    out_list, out_dir = sys.argv[1], sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    s = td.Settings([], PREREQ)
    lines = []
    def add(tag, src, words):
        lines.append("\t".join([tag, "ps_3_0", src, os.path.join(out_dir, tag.replace("/", "_") + ".bin"), words]))
    for name, path in cpp_shaders(out_dir): add("cpp/%s" % name, path, "NVR_CPP_PASS=1")	# (a define word: the batch line needs one)
    for runtime in ("native", "dxvk"):
        opt = opt_for(runtime)
        d = td.apply_forces(td.build(s, [], [], opt), ["CAPTURED_NORMALS_ON=4"], [])
        if d.dropped: print("# gear: DROPPED past the 62 cap: %s" % " ".join(d.dropped))
        add("%s/gear" % runtime, os.path.join(SH, "GearWeather.hlsl"), td.define_words(d))
        lamp = [("INTERIOR_SHADOWS", "1"), ("FORWARD_SHADOWS", "0"), ("SUN_CROSSFADE", "0"), ("CONTACT_HARDENING", "0")]
        extra = [("POINT_SHADOW_ATLAS", "1"), ("FD_LIGHT_FREE", "1"), ("LIGHT_COMMON_LIST_TEXELS", "16.0f"), ("FD_SKIN", "1")]
        passes = [("sun", "DeferredSun", [("INTERIOR_SHADOWS", "0"), ("FD_SKIN", "1"), ("FD_HAIR", "1")]),
                  ("dlpass", "DeferredLamps", lamp + [("POINT_SHADOW_FIT", "3")] + extra),
                  ("dlshadow", "DeferredLamps", lamp + [("DL_SHADOW_PASS", "1"), ("POINT_SHADOW_FIT", "0")] + extra),
                  ("dlskin", "DeferredLamps", lamp + [("POINT_SHADOW_FIT", "3")] + extra + [("DL_SKIN_ONLY", "1")])]
        for tag, file, ov in passes:
            d = td.apply_forces(td.build(s, [], ov, opt), ["CAPTURED_NORMALS_ON=4"], [])
            if d.dropped: print("# %s: DROPPED past the 62 cap: %s" % (tag, " ".join(d.dropped)))
            add("%s/%s" % (runtime, tag), os.path.join(SH, file + ".hlsl"), td.define_words(d))
    with open(out_list, "w", encoding="utf-8") as f: f.write("\n".join(lines) + "\n")
    print("# %d batch lines -> %s" % (len(lines), out_list))

if __name__ == "__main__": main()
