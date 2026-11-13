"""rt_sun_set.py: (re)build the no-limits replacements that give the forward RT sun to the shaders with no ps_3_0 sampler left
(2026-11-14 rt-shadows room; F:\\Claude Brain\\RTShadowAudit\\AUDIT.md, Shaders/Includes/Shadow.hlsl SHADOW_SUN_CONTACT_EXTRA).

A replacement is matched by the hash of the ps_3_0 bytecode the game compiled, and that bytecode depends on the toml's compile-time
settings: after a settings change that recompiles shaders (or a template edit) the old set silently stops applying (those shaders
fall back to the cascades: nothing breaks). Rebuild then:
  1. [Main.Main.Performance] NvrDxvkReplacements = true, create Data\\Shaders\\VegasShaders\\Compute\\Replace\\dump\\, start the game
     once (every pixel shader is written there with its defines), quit, and delete the dump folder again.
  2. py -3 rt_sun_set.py <dump folder> <tree's src\\hlsl\\NewVegas\\Shaders> <out folder>
     -> nolimits.py over the templates that can take it, keeping only the variants that need it: outdoors (no INTERIOR_SHADOWS),
        the ps_3_0 original without the lookup (no TESR_ContactForward in its table) and the no-limits build with it ("extras=" in
        its manifest: lamp-only passes never read the sun).
  3. Copy <out folder>\\*.spv + *.txt to Data\\Shaders\\VegasShaders\\Compute\\Replace\\ (replace the old set).
"""
import glob, os, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
TEMPLATES = "TerrainTemplate,ObjectTemplate,ParallaxTemplate,SkinTemplate,SM3002.pso,SM3003.pso,SM3005.pso,SM3007.pso"


def main():
    if len(sys.argv) < 4: sys.exit(__doc__)
    dump, src, out = (os.path.abspath(a) for a in sys.argv[1:4])
    work = os.environ.get("NOLIMITS_WORK") or os.path.join(os.environ.get("TEMP", out), "nvr-nolimits-work")
    os.makedirs(work, exist_ok=True)
    tmp = out + ".all"
    if os.path.exists(tmp): shutil.rmtree(tmp)
    env = dict(os.environ, NOLIMITS_WORK=work)
    subprocess.run([sys.executable, "-I", os.path.join(HERE, "nolimits.py"), "dump", dump, src, tmp, "--templates", TEMPLATES], env=env, check=False)
    if os.path.exists(out): shutil.rmtree(out)
    os.makedirs(out)
    kept = skipped = 0
    for spv in sorted(glob.glob(os.path.join(tmp, "*.spv"))):
        key = os.path.basename(spv)[:-4]
        txt = spv[:-4] + ".txt"
        defines = open(os.path.join(dump, key + ".defines"), encoding="utf-8").read()
        asm = os.path.join(work, key + ".ps30.asm")
        has_lookup = os.path.exists(asm) and "TESR_ContactForward" in open(asm, errors="replace").read()
        wants = "extras=" in open(txt, encoding="utf-8").read()
        if "INTERIOR_SHADOWS=1" in defines or has_lookup or not wants:
            skipped += 1
            continue
        shutil.copy2(spv, out); shutil.copy2(txt, out); kept += 1
    shutil.rmtree(tmp)
    # (2026-11-14) the set's own list: the DLL checks it against what matched at load (DxvkCompute ReplacementSetStatus) and
    # says so in the log and the menu when shaders changed since (add hand-made ones, e.g. SLS2027's full-puddle build, by hand)
    keys = sorted(os.path.basename(f)[:-4] for f in glob.glob(os.path.join(out, "*.spv")))
    NL = chr(10)
    open(os.path.join(out, "rt_sun_set.txt"), "w", newline=NL).write("# rt_sun_set.py: the replacements of this set (Name@hash)" + NL + NL.join(keys) + NL)
    print("rt_sun_set: %d replacements kept (%d built but not needed) -> %s" % (kept, skipped, out))


if __name__ == "__main__":
    main()
