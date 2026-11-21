#!/usr/bin/env python3
r"""
build_deferred.py: the deferred light passes' SPIR-V builds for record mode (docs\deferred-single-pass.md step 1), from the game's own
preprocessed cache texts, into src\hlsl\NewVegas\Compute\Deferred (deployed as Data\Shaders\VegasShaders\Compute\Deferred).

  py -3.10 -I tools\nvr-sm6\build_deferred.py [--cache DIR]... [--lamps] [--out DIR] [--dry-run]

Each --cache DIR is a Shaders\VegasShaders\Shaders\Cache or CacheShipped folder (default: the MO2 overwrite's live Cache and the
VegasShaders-1.1.0 mod's CacheShipped on this PC). Every Deferred\Sun and Deferred\SunFade text found there is converted (tpl2spv.py);
--lamps adds the DeferredLamps family (step 2's input), --dry-run writes to a scratch folder instead of --out. Only the variants those
caches hold are built: a player whose settings preprocess the pass differently gets a key with no .spv, and the C++ keeps D3D9 then.
"""
import argparse, os, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import tpl2spv  # noqa: E402

MO2 = r"E:\Mod Organizer 2 (Archive)-6194-2-1-5 (1)"
DEFAULT_CACHES = [MO2 + r"\overwrite\Shaders\VegasShaders\Shaders\Cache",
                  MO2 + r"\mods\VegasShaders-1.1.0\Shaders\VegasShaders\Shaders\CacheShipped"]
SUN = [("Deferred", "Sun"), ("Deferred", "SunFade")]     # sub-folders (ShaderRecord::LoadShader's CacheSubPath) holding DeferredSun.pso.hlsl
LAMP_NAMES = ("DeferredLamps", "DeferredLampFog", "DeferredLampShadows", "DeferredLampSkin", "DeferredLampLand", "DeferredLampLandFilm")


def sources(caches, lamps):
    found = []
    for c in caches:
        for parts in SUN:
            p = os.path.join(c, *parts, "DeferredSun.pso.hlsl")
            if os.path.exists(p):
                found.append(p)
        if lamps:
            for root, _, files in os.walk(os.path.join(c, "Deferred")):
                for f in sorted(files):
                    if f.endswith(".pso.hlsl") and f[:-len(".pso.hlsl")] in LAMP_NAMES:
                        found.append(os.path.join(root, f))
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cache", action="append", default=None)
    ap.add_argument("--lamps", action="store_true")
    ap.add_argument("--out", default=os.path.join(REPO, "src", "hlsl", "NewVegas", "Compute", "Deferred"))
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    out = tempfile.mkdtemp(prefix="nvr-deferred-") if a.dry_run else a.out
    ok, failed, keys = 0, [], set()
    for src in sources(a.cache or DEFAULT_CACHES, a.lamps):
        print("== " + src)
        try:
            info = tpl2spv.convert(src, out)
            ok += 1
            if (info["pass"], info["key"]) in keys:
                print("   (same text as an earlier cache: the same file)")
            keys.add((info["pass"], info["key"]))
        except SystemExit as e:
            failed.append(src)
            print("   FAILED: %s" % e)
    print("%d converted (%d distinct), %d failed, into %s" % (ok, len(keys), len(failed), out))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
