#!/usr/bin/env python3
"""UNOFFICIAL 2026-11-16 fd-fwdgroups (docs/next/FD-FORWARD-GROUPS.md): a template_compile --batch list for every shader the forward-kept groups'
fixes touch, under the define list ShaderRecord::BuildDefines gives with the deferred renderer's prerequisites + GBufferV2 (tests/template_defines.py),
native and fork:
  - skin: DeferredSkinSSS prep / blur; DeferredSun with FD_SKIN (Includes/SkinDeferred.hlsl's transmission)
  - glass: every eligible Object name's forward+ record indoors (Interior overrides + LAMPS_DEFERRED 4, and 5) and outdoors (LAMPS_DEFERRED 4 /
    5 alone) with ForwardPlus.hlsl's flashlight
  - hair: SM3002 / SM3003.pso forward (byte compare) and HAIR_FORWARD_PLUS 1; SM3003.vso
  - leaves: STLEAF2000/2000TMS/2001/2001TMS.pso forward, FOLIAGE_DEFERRED 1 (byte compare) and + LEAF_FORWARD_PLUS 1; STLEAF001/003.vso
Usage: fd_fwdgroups_compile.py OUT_LIST OUT_DIR [SHADERS_DIR [off]]   then   template_compile.exe --flow --device --batch OUT_LIST
  "off": only the builds that must be byte-identical to the base (no new define): compare their .bin with a base Shaders folder's.
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
TABLE = ["CAPTURED_NORMALS_ON=4", "LIGHT_COMMON_LIST_TEXELS=16.0f"]

class Opt: pass

def opt_for(runtime):
    o = Opt(); o.reversed_depth = 1; o.rt = 0; o.runtime = runtime; o.dev = False; o.native_manylamps = False
    o.displace_objects = False; o.record = "default"
    return o

def main():
    global SH
    out_list, out_dir = sys.argv[1], sys.argv[2]
    if len(sys.argv) > 3: SH = sys.argv[3]
    only_off = len(sys.argv) > 4 and sys.argv[4] == "off"
    os.makedirs(out_dir, exist_ok=True)
    s = td.Settings([], PREREQ)
    lines = []
    def add(tag, model, src, d):
        if d.dropped: print("# %s: DROPPED past the 62 cap: %s" % (tag, " ".join(d.dropped)))
        lines.append("\t".join([tag, model, src, os.path.join(out_dir, tag.replace("/", "_") + ".bin"), td.define_words(d)]))
    def plain(opt, ov, forces=TABLE):
        return td.apply_forces(td.build(s, [], ov, opt), forces, [])
    for runtime in ("native", "dxvk"):
        opt = opt_for(runtime)
        # hair
        for n in ("SM3002", "SM3003"):
            add("%s/fwd/%s" % (runtime, n), "ps_3_0", os.path.join(SH, n + ".pso.hlsl"), plain(opt, []))
            add("%s/fwdint/%s" % (runtime, n), "ps_3_0", os.path.join(SH, n + ".pso.hlsl"), plain(opt, td.overrides_for("interior", s)))
            if not only_off: add("%s/fp/%s" % (runtime, n), "ps_3_0", os.path.join(SH, n + ".pso.hlsl"), plain(opt, [("HAIR_FORWARD_PLUS", "1")]))
        add("%s/nov2/SM3003vs" % runtime, "vs_3_0", os.path.join(SH, "SM3003.vso.hlsl"), plain(opt, [], ["CAPTURED_NORMALS_ON=3"]))
        if not only_off: add("%s/v2/SM3003vs" % runtime, "vs_3_0", os.path.join(SH, "SM3003.vso.hlsl"), plain(opt, []))
        # leaves
        for n in ("STLEAF2000", "STLEAF2000TMS", "STLEAF2001", "STLEAF2001TMS"):
            add("%s/fwd/%s" % (runtime, n), "ps_3_0", os.path.join(SH, n + ".pso.hlsl"), plain(opt, []))
            add("%s/def/%s" % (runtime, n), "ps_3_0", os.path.join(SH, n + ".pso.hlsl"), plain(opt, [("FOLIAGE_DEFERRED", "1")]))
            if not only_off: add("%s/deflamps/%s" % (runtime, n), "ps_3_0", os.path.join(SH, n + ".pso.hlsl"), plain(opt, [("FOLIAGE_DEFERRED", "1"), ("LEAF_FORWARD_PLUS", "1")]))
        for n in ("STLEAF001", "STLEAF003"):
            add("%s/nov2/%svs" % (runtime, n), "vs_3_0", os.path.join(SH, n + ".vso.hlsl"), plain(opt, [], ["CAPTURED_NORMALS_ON=3"]))
            if not only_off: add("%s/v2/%svs" % (runtime, n), "vs_3_0", os.path.join(SH, n + ".vso.hlsl"), plain(opt, []))
        if only_off: continue
        # skin
        add("%s/sssprep" % runtime, "ps_3_0", os.path.join(SH, "DeferredSkinSSS.hlsl"), plain(opt, [("SSS_PREP", "1")]))
        add("%s/sssblur" % runtime, "ps_3_0", os.path.join(SH, "DeferredSkinSSS.hlsl"), plain(opt, []))
        add("%s/skinsun" % runtime, "ps_3_0", os.path.join(SH, "DeferredSun.hlsl"), plain(opt, [("INTERIOR_SHADOWS", "0"), ("FD_SKIN", "1"), ("FD_HAIR", "1")]))
        # glass (DeferredTransparents::Eligible: ObjectTemplate lit, not ONLY_LIGHT / DIFFUSE / ONLY_SPECULAR / NO_LIGHT / HAIR, with an Interior record)
        for name, template, pairs, _ in td.records():
            if template != "ObjectTemplate" or not td.eligible_interior(name, template): continue
            keys = [k for k, _v in pairs]
            if any(k in keys for k in ("ONLY_LIGHT", "DIFFUSE", "ONLY_SPECULAR", "NO_LIGHT", "HAIR")): continue
            base = name.replace(".pso", "")
            for kind, ov in (("fpi", td.overrides_for("interior", s) + [("LAMPS_DEFERRED", "4")]), ("fpx", [("LAMPS_DEFERRED", "4")]), ("fpx5", [("LAMPS_DEFERRED", "5")])):
                d = td.apply_forces(td.build(s, pairs, ov, opt), TABLE, [])
                add("%s/%s/%s" % (runtime, kind, base), "ps_3_0", os.path.join(SH, "ObjectTemplate.hlsl"), d)
    with open(out_list, "w", encoding="utf-8") as f: f.write("\n".join(lines) + "\n")
    print("# %d batch lines -> %s" % (len(lines), out_list))

if __name__ == "__main__": main()
