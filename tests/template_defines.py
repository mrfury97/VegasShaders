#!/usr/bin/env python3
"""UNOFFICIAL 2026-10-31 (docs/next/DEFERRED-RENDERING-PLAN.md stage 0c): the define list ShaderRecord::BuildDefines gives a game-shader
record, worked out offline from the settings files, so template_compile.exe compiles exactly the record the game would (slot counts, the
62-define cap, byte identity: tests/template_identity.py). It mirrors src/core/ShaderRecord.cpp BuildDefines line by line (the order matters:
an override set first wins, and the cap drops what comes past 62); keep the two in step when a define is added there.

Usage:
  template_defines.py [--toml FILE]... [--set Sec.Key=Value]... [--runtime native|dxvk] [--rt 0|1] [--reversed-depth 0|1]
                      [--record default|interior|interior-fit1|interior-fit2|interior-fit3|exterior-lamps] [--dev] [--native-manylamps]
                      [--force NAME=VALUE]... [--drop NAME]... NAME.pso [NAME...]
     prints the define list (NAME=VALUE a line) and a summary line "# N defines of 62 ..." per record
  template_defines.py --list                      every record of the template tables (name, template, collection)
  template_defines.py --batch OUT --shaders DIR --out-dir DIR [--records default,interior] [the options above] [NAMES...]
     writes a template_compile --batch list (every record, or the names given), the bytecode to <out-dir>/<record>/<name>.bin
The settings are the repo's defaults (resource/VegasShaders.defaults.toml) under the --toml files in order (the user's toml is a
full copy of every key, so it overrides all); --set wins over both. Sections are written as in the files ([_Main.Main.Misc]) or without
the underscore.
"""
import argparse, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DEFAULTS = os.path.join(ROOT, "resource", "VegasShaders.defaults.toml")
TABLES = ["src/effects/PBR/PBR.h", "src/effects/POM/POM.h", "src/effects/Terrain/Terrain.h", "src/effects/Skin/Skin.h"]  # 2026-11-04: the effect folders

# ---------------------------------------------------------------- settings
def parse_toml(path, into):
    section = None
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            s = line.strip()
            if not s or s.startswith("#"): continue
            if s.startswith("["):
                section = s[1:s.index("]")].strip()
                if section.startswith("_"): section = section[1:]
                continue
            if "=" not in s or section is None: continue
            key, _, value = s.partition("=")
            value = value.strip()
            # strip a trailing comment (values here are plain: numbers, booleans, "strings")
            if value.startswith('"'):
                end = value.find('"', 1); value = value[1:end] if end > 0 else value[1:]
            else:
                value = value.split("#", 1)[0].strip()
            into[(section, key.strip())] = value

class Settings:
    def __init__(self, tomls, sets):
        self.v = {}
        parse_toml(DEFAULTS, self.v)
        for t in tomls: parse_toml(t, self.v)
        for s in sets:
            sk, _, val = s.partition("=")
            sec, _, key = sk.rpartition(".")
            if sec.startswith("_"): sec = sec[1:]
            self.v[(sec, key)] = val
    def raw(self, sec, key): return self.v.get((sec, key))
    def i(self, sec, key):
        r = self.raw(sec, key)
        if r is None: return 0
        if r.lower() == "true": return 1
        if r.lower() == "false": return 0
        try: return int(float(r))
        except ValueError: return 0
    def f(self, sec, key):
        r = self.raw(sec, key)
        if r is None: return 0.0
        if r.lower() == "true": return 1.0
        if r.lower() == "false": return 0.0
        try: return float(r)
        except ValueError: return 0.0

# ---------------------------------------------------------------- template tables
ENTRY = re.compile(r'\{\s*"([A-Za-z0-9_]+\.[pv]so)"\s*,\s*ShaderTemplate\s*\{\s*"([A-Za-z]+)"\s*,\s*\{((?:\s*\{\s*"[^"]*"\s*,\s*"[^"]*"\s*\}\s*,?)*)\s*\}\s*\}\s*\}')
PAIR = re.compile(r'\{\s*"([^"]*)"\s*,\s*"([^"]*)"\s*\}')

def records():
    out = []
    for rel in TABLES:
        text = open(os.path.join(ROOT, rel), encoding="utf-8", errors="replace").read()
        for m in ENTRY.finditer(text):
            name, template, pairs = m.group(1), m.group(2), [(k, v) for k, v in PAIR.findall(m.group(3))]
            out.append((name, template, pairs, os.path.basename(rel)))
    return out

# ---------------------------------------------------------------- BuildDefines, mirrored
CAP = 62

class Defines:
    def __init__(self, template_pairs):
        self.items = list(template_pairs)   # the template's own come first (ShaderTemplate.Defines)
        self.dropped = []
        self.template_count = len(template_pairs)
        self.override_count = 0
    def append(self, name, value):
        for k, _ in self.items:
            if k == name: return          # an override already set it
        if len(self.items) >= CAP: self.dropped.append(name); return
        self.items.append((name, value))

def build(settings, template_pairs, overrides, opt):
    d = Defines(template_pairs)
    for k, v in overrides: d.append(k, v)
    d.override_count = len(d.items) - d.template_count
    s = settings
    on = lambda sec, key: "1" if s.i(sec, key) else "0"
    if opt.reversed_depth: d.append("REVERSED_DEPTH", "")
    d.append("FORWARD_SHADOWS", on("Shaders.ShadowsExteriors.Main", "ForwardShadows"))
    # 2026-11-06 Bend forward contact shadows: appended only when on (Bend && InLighting && ForwardShadows), default true for both keys
    ss = "Shaders.ShadowsExteriors.ScreenSpace"
    flag = lambda key: s.raw(ss, key) is None or s.i(ss, key)
    if flag("Bend") and flag("InLighting") and s.i("Shaders.ShadowsExteriors.Main", "ForwardShadows"): d.append("SUN_CONTACT_FORWARD", "1")
    d.append("CONTACT_HARDENING", on("Shaders.ContactHardening.Status", "Enabled"))
    d.append("SHADOW_SLACK_CAP_ALL", on("Shaders.ShadowsExteriors.Main", "CasterReachEverywhere"))
    d.append("POINT_SHADOW_SMOOTH", on("Shaders.ShadowsInteriors.Forward", "SmoothEdges"))
    if s.i("Shaders.ShadowsInteriors.Forward", "EvenTaps"): d.append("POINT_SHADOW_EVEN", "1")
    if s.i("Shaders.ShadowsInteriors.Forward", "TexelFilter"): d.append("POINT_SHADOW_TEXEL", "1")
    elif s.f("Shaders.ShadowsInteriors.Forward", "MagnifiedSoftness") > 0.0: d.append("POINT_SHADOW_MAG", "1")
    if opt.rt: d.append("POINT_SHADOW_RT", "1")       # RtLampShadows::CompiledIn: on at startup AND NVR DXVK can trace
    d.append("LIVE_CUBE", "1" if (s.f("Shaders.PBR.Metal", "CubeReflections") > 0.0 or s.f("Shaders.PBR.EnvMaps", "CubeReflections") > 0.0) else "0")
    d.append("MATERIAL_RULES_ON", on("Shaders.PBR.MaterialRules", "Enabled"))
    d.append("RMAOS_ON", on("Shaders.PBR.Rmaos", "Enabled"))
    if s.i("Shaders.Reflections.Main", "MaterialAware") > 0 and s.i("Shaders.Reflections.Status", "Enabled") and s.i("Shaders.PBR.Rmaos", "Enabled"):
        d.append("REFL_MATERIAL_ON", "1")
    d.append("HUD_RAIN_ON", "1" if s.f("Shaders.WetnessEffects.HudRaindrops", "Enabled") else "0")
    d.append("LINEAR_LIGHTING_ON", on("Shaders.PBR.Linear", "Enabled"))
    d.append("LINEAR_SCENE_ON", "1" if (s.i("Shaders.PBR.Linear", "Enabled") and s.i("Shaders.PBR.Linear", "LinearScene")) else "0")
    captured = s.i("Shaders.SnowAccumulation.Main", "CapturedNormals") or s.i("Main.Main.Quality", "CapturedNormals")
    material = captured and s.i("Main.Main.Quality", "MaterialBuffer")
    albedo = material and s.i("Main.Main.Quality", "AlbedoBuffer")
    if captured: d.append("CAPTURED_NORMALS_ON", "3" if albedo else "2" if material else "1")
    if s.i("Main.Main.Quality", "CapturedNormals"): d.append("CAPTURED_NORMALS_FORWARD", "1")
    if opt.dev and s.i("Main.Main.Misc", "LampCapsCensus"): d.append("LAMP_CAPS_VIEW", "1")
    d.append("PHYSICAL_SKY_ON", on("Shaders.Sky.Physical", "Enabled"))
    d.append("SPECULAR_LAMPS6", on("Main.Main.Misc", "MoreLampsPerObject"))
    d.append("LAMP_REACH_SKIP", on("Shaders.PBR.Main", "LampReachSkip"))
    dxvk = opt.runtime == "dxvk"
    d.append("NO_SLOT_LIMIT", "1" if (s.i("Shaders.PBR.Main", "DxvkLampBranches") and dxvk) else "0")
    many = s.i("Main.Main.Misc", "ManyLampsOnePass") and s.i("Main.Main.Misc", "MoreLampsPerObject") and (dxvk or opt.native_manylamps)
    table = s.i("Main.Main.Misc", "LightCommonLamps")
    cluster = table and s.i("Main.Main.Misc", "ClusteredLamps")
    d.append("MANY_LAMPS_ON", ("3" if cluster else "2" if table else "1") if many else "0")
    # 2026-10-31 LampTableStandalone / TableLamps (ManyLamps::RowsOverride / TileRowsAtStartup)
    deferred = many and table and s.i("Shaders.ShadowsInteriors.Deferred", "Enabled") and s.i("Shaders.ShadowsInteriors.Atlas", "Enabled") and dxvk and \
        s.i("Shaders.ShadowsInteriors.Forward", "Enabled")	# 2026-10-31 DeferredLamps (ManyLamps::DeferredAtStartup)
    froxel_rows = cluster or s.i("Main.Main.Misc", "LampTableStandalone") or deferred
    tiles = many and table and s.i("Shaders.ShadowsInteriors.Atlas", "Enabled") and dxvk and s.i("Shaders.ShadowsInteriors.Atlas", "TableLamps")
    froxel_table_rows = 61 if deferred else 31	# 2026-10-31: 16-texel lists with DeferredLamps (ManyLamps::FroxelTableRows)
    rows = (froxel_table_rows if froxel_rows else 1) + (2 if (tiles or deferred) else 0)
    if many and table and rows != (31 if cluster else 1): d.append("LIGHT_COMMON_ROWS_N", "%d.0f" % rows)
    if tiles: d.append("TABLE_LAMP_SHADOWS", "%d.0f" % (froxel_table_rows if froxel_rows else 1))
    if deferred: d.append("LIGHT_COMMON_LIST_TEXELS", "16.0f")
    if s.i("Main.Main.Misc", "LandAllLamps"): d.append("LAND_ALL_LAMPS", "1")
    if s.i("Shaders.ShadowsInteriors.Atlas", "Enabled") and dxvk: d.append("POINT_SHADOW_ATLAS", "1")	# P85: DXVK only (AtlasWanted)
    d.append("UNLIT_SHADOW_SKIP", on("Shaders.PBR.Main", "UnlitShadowSkip"))
    d.append("SMITH_CORRELATED_ON", "1" if s.i("Shaders.PBR.Main", "SmithCorrelated") == 1 else "0")
    d.append("SPEC_FADE_ON", on("Shaders.PBR.Main", "SpecularFade"))
    d.append("FOLD_HIGHLIGHTS_ON", on("Shaders.PBR.Main", "FoldHighlights"))
    d.append("LAMP_VECTOR_ATT_ON", on("Shaders.PBR.Main", "LampDistanceFix"))
    d.append("LAMP_SUMS_ON", on("Shaders.PBR.Main", "LampSums"))
    d.append("HIGHLIGHT_SUMS_ON", on("Shaders.PBR.Main", "HighlightSums"))
    d.append("LIN_PREDECODE_ON", on("Shaders.PBR.Linear", "PreDecode"))
    ext, inte, match = (s.i("Shaders.PBR.LampFalloff", k) for k in ("Exterior", "Interior", "MatchEverywhere"))
    shape = ext or inte
    if shape or match: d.append("LAMP_FALLOFF_ON", "2" if shape else "1")
    if s.f("Shaders.ShadowsInteriors.Forward", "ShadowContrast") > 0.0: d.append("SHADOW_CONTRAST_ON", "1")
    d.append("TERRAIN_BLEND", on("Shaders.Terrain.Blending", "Enabled"))
    d.append("TERRAIN_VARIATION", on("Shaders.Terrain.Variation", "Enabled"))
    d.append("TERRAIN_VARIATION_LAYERS", str(min(max(s.i("Shaders.Terrain.Variation", "Layers"), 1), 7)))
    d.append("HAND_BLOOD", on("Shaders.HandBlood.Status", "Enabled"))
    d.append("CLOTH_WIND_ON", on("Shaders.Cloth.Main", "Enabled"))
    d.append("PLANT_WIND_ON", on("Shaders.Plants.Wind", "Enabled"))
    d.append("SUN_CROSSFADE", "1" if (s.i("Shaders.ShadowsExteriors.SunSmoothing", "CrossFade") and s.i("Shaders.ShadowsExteriors.Main", "ForwardShadows")) else "0")
    d.append("SKYLIGHTING_MODE", on("Shaders.PBR.Main", "SkylightingMode"))
    d.append("SHADOW_FIXED_MODE", "3")
    if s.i("Shaders.ShadowsExteriors.Main", "CharacterObjectReach"): d.append("SHADOW_CHARACTER_OBJECTS", "1")
    if s.i("Shaders.ShadowsExteriors.ShadowMaps", "CharacterShadowMap"): d.append("CHARACTER_SHADOW_MAP", "1")
    d.append("OBJECT_SMOOTH_NORMALS", on("Shaders.PBR.Main", "SmoothObjectNormals"))
    if s.i("Main.Main.Performance", "NvrDxvkDisplacement"): d.append("NVR_DISPLACE_DEPTH", "1")
    if opt.displace_objects: d.append("NVR_DISPLACE_OBJECTS", "1")
    return d

def lamp_falloff_interior_define(s):
    """PBRShaders::LampFalloffInteriorDefine: the interior lamp-shadow records' LAMP_FALLOFF_ON (None = as the global one)."""
    ext, inte, match = (s.i("Shaders.PBR.LampFalloff", k) for k in ("Exterior", "Interior", "MatchEverywhere"))
    if not (ext or inte) or inte: return None
    return "1" if match else "0"

def overrides_for(record, s):
    """ShaderManager::LoadShader's override sets (the interior records, their FIT step-downs, the RT ExteriorForward records)."""
    if record == "default": return []
    base = [("INTERIOR_SHADOWS", "1"), ("FORWARD_SHADOWS", "0"), ("SUN_CROSSFADE", "0"), ("CONTACT_HARDENING", "0")]
    if record.startswith("interior"):
        if record != "interior": base.append(("POINT_SHADOW_FIT", record[-1]))
        lf = lamp_falloff_interior_define(s)
        if lf is not None: base.append(("LAMP_FALLOFF_ON", lf))
        return base
    if record == "exterior-lamps": return [("INTERIOR_SHADOWS", "1"), ("POINT_SHADOW_RT_ONLY", "1")]
    raise SystemExit("unknown record kind " + record)

def eligible_interior(name, template):
    """PointShadowForward::Eligible: which pixel shaders get an Interior (lamp shadow) record."""
    if not name.endswith(".pso"): return False
    if template == "SkinTemplate": return name not in ("SKIN2000.pso", "SKIN2001.pso")
    return template in ("ObjectTemplate", "SkinVPSTemplate")

def apply_forces(d, forces, drops):
    for f in forces:
        k, _, v = f.partition("=")
        for i, (n, _) in enumerate(d.items):
            if n == k: d.items[i] = (k, v); break
        else: d.items.append((k, v))
    d.items = [(n, v) for n, v in d.items if n not in drops]
    return d

def define_words(d): return " ".join(n if v == "" else "%s=%s" % (n, v) for n, v in d.items)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--toml", action="append", default=[])
    ap.add_argument("--set", action="append", default=[])
    ap.add_argument("--runtime", choices=["native", "dxvk"], default="native")
    ap.add_argument("--rt", type=int, default=0, help="POINT_SHADOW_RT compiled in (RT lamp shadows on at startup under NVR DXVK)")
    ap.add_argument("--reversed-depth", type=int, default=1, help="the game clears depth to 0 (the live log says 'reversed depth yes')")
    ap.add_argument("--record", default="default")
    ap.add_argument("--records", default="default,interior", help="--batch: record kinds")
    ap.add_argument("--dev", action="store_true", help="a development build (NVR_TEST_TOOLS: LampCapsCensus)")
    ap.add_argument("--native-manylamps", action="store_true", help="ManyLampsOnePassNative: the runtime gate lifted (the plan's decision rule)")
    ap.add_argument("--displace-objects", action="store_true")
    ap.add_argument("--force", action="append", default=[])
    ap.add_argument("--drop", action="append", default=[])
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--batch")
    ap.add_argument("--shaders", help="--batch: the Shaders folder the templates are compiled from")
    ap.add_argument("--out-dir", help="--batch: where the bytecode goes (<out-dir>/<record>/<name>.bin)")
    ap.add_argument("names", nargs="*")
    opt = ap.parse_args()
    recs = records()
    if opt.list:
        for name, template, pairs, where in recs: print("%-14s %-20s %-10s %s" % (name, template, where, " ".join(k if not v else "%s=%s" % (k, v) for k, v in pairs)))
        print("# %d records" % len(recs)); return
    s = Settings(opt.toml, opt.set)
    by_name = {r[0]: r for r in recs}
    if opt.batch:
        if not opt.shaders or not opt.out_dir: raise SystemExit("--batch needs --shaders and --out-dir")
        wanted = opt.names or [r[0] for r in recs]
        lines = []
        for kind in opt.records.split(","):
            os.makedirs(os.path.join(opt.out_dir, kind), exist_ok=True)
            for name in wanted:
                if name not in by_name: raise SystemExit("no record " + name)
                _, template, pairs, _ = by_name[name]
                if kind != "default" and not eligible_interior(name, template): continue
                src = os.path.join(opt.shaders, template + ".hlsl")
                if not os.path.exists(src): continue
                d = apply_forces(build(s, pairs, overrides_for(kind, s), opt), opt.force, opt.drop)
                profile = "vs_3_0" if name.endswith(".vso") else "ps_3_0"
                out = os.path.join(opt.out_dir, kind, name + ".bin")
                lines.append("\t".join(["%s/%s" % (kind, name), profile, src, out, define_words(d)]))
        with open(opt.batch, "w", encoding="utf-8") as f: f.write("\n".join(lines) + "\n")
        print("# %d batch lines -> %s" % (len(lines), opt.batch)); return
    for name in opt.names:
        if name not in by_name: raise SystemExit("no record " + name)
        _, template, pairs, where = by_name[name]
        d = apply_forces(build(s, pairs, overrides_for(opt.record, s), opt), opt.force, opt.drop)
        print("# %s (%s, %s record)" % (name, template, opt.record))
        for n, v in d.items: print(n if v == "" else "%s=%s" % (n, v))
        print("# %d defines of %d (template %d, overrides %d, appended %d%s)" % (len(d.items), CAP, d.template_count, d.override_count,
            len(d.items) - d.template_count - d.override_count, (", DROPPED past the cap: " + " ".join(d.dropped)) if d.dropped else ""))

if __name__ == "__main__": main()
