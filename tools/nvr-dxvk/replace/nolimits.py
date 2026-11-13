"""nolimits.py: build the NVR DXVK fork's no-limits replacements for NVR's own pixel shaders (2026-10-26, FORK-1026-LIMITS.md).

  py -3 nolimits.py dump <dump folder> <Shaders source folder> <out folder> [--only NAME,NAME] [--templates T,T] [--plain]
      For every <Name>@<hash>.defines in the dump folder (NVR writes it next to <Name>@<hash>.pso when
      Data\\Shaders\\VegasShaders\\Compute\\Replace\\dump exists, the fork is present and NvrDxvkReplacements is on):
      compiles that exact variant again WITHOUT ps_3_0's limits (fxc /T ps_4_0 /Gec, NVR_SM4=1 NVR_NO_SM3_LIMITS=1),
      translates it (sm4asm2glsl.py) against the original .pso's constant table and writes <Name>@<hash>.spv + .txt to
      <out folder> (copy them to Data\\Shaders\\VegasShaders\\Compute\\Replace\\). --plain: without NVR_NO_SM3_LIMITS
      (the same cuts as the ps_3_0 shader: a check that the route itself gives the same picture).
  py -3 nolimits.py variant <Shaders source folder> <template> <out prefix> NAME=VALUE ...
      Offline check of one variant: the ps_3_0 build (the original) and the no-limits build, translated and validated.

Needs: fxc.exe (Windows SDK), WSL with glslangValidator + spirv-val (~/vktools/root/usr/bin, see compile.sh).
A shader the script cannot build is skipped with the reason (it keeps running as the game's ps_3_0 shader).
"""
import glob, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import sm4asm2glsl  # noqa: E402

FXC_CANDIDATES = glob.glob(r"C:\Program Files (x86)\Windows Kits\10\bin\10.*\x64\fxc.exe")
FXC = sorted(FXC_CANDIDATES)[-1] if FXC_CANDIDATES else "fxc.exe"


def wsl_path(p):
    p = os.path.abspath(p)
    return "/mnt/%s%s" % (p[0].lower(), p[2:].replace("\\", "/"))


def fxc(args, cwd):
    r = subprocess.run([FXC, "/nologo"] + args, cwd=cwd, capture_output=True, text=True)
    errs = [l for l in (r.stdout + r.stderr).splitlines() if "error" in l]
    return r.returncode, "\n".join(errs[:6])


def define_args(defines):
    out = []
    for k, v in defines:
        out.append("/D%s=%s" % (k, v) if v != "" else "/D%s" % k)
    return out


def spirv(frag, spv):
    """GLSL -> SPIR-V. (2026-11-14 rt-shadows) Windows glslang when found (NOLIMITS_GLSLANG, or tools\dl\glslang-* of Claude Brain),
    spirv-val after it when NOLIMITS_SPIRV_VAL names one; else WSL as before."""
    win = os.environ.get("NOLIMITS_GLSLANG") or next(iter(sorted(glob.glob(r"F:\Claude Brain\tools\dl\glslang-*\x\bin\glslang.exe"))), None)
    if win and os.path.exists(win):
        r = subprocess.run([win, "-V", "--target-env", "vulkan1.1", "-o", spv, frag], capture_output=True, text=True)
        if r.returncode: return 1, (r.stdout + r.stderr).strip()[-1500:]
        val = os.environ.get("NOLIMITS_SPIRV_VAL")
        if val and os.path.exists(val):
            r = subprocess.run([val, "--target-env", "vulkan1.1", "--scalar-block-layout", spv], capture_output=True, text=True)
            if r.returncode: return 1, (r.stdout + r.stderr).strip()[-1500:]
        return 0, ""
    tools = "$HOME/vktools/root/usr/bin"
    cmd = "export PATH=%s:$PATH; glslangValidator -V --target-env vulkan1.1 -o '%s' '%s' > /tmp/nolimits_glslang.log 2>&1 || { tail -20 /tmp/nolimits_glslang.log; exit 1; }; spirv-val --target-env vulkan1.1 --scalar-block-layout '%s'" % (
        tools, wsl_path(spv), wsl_path(frag), wsl_path(spv))
    r = subprocess.run(["wsl", "-e", "bash", "-lc", cmd], capture_output=True, text=True)
    return r.returncode, (r.stdout + r.stderr).strip()


def build(src_dir, template, defines, original_asm, prefix, plain=False):
    """-> (ok, message). Writes <prefix>.sm4.asm, .frag, .txt, .spv."""
    src_dir, prefix = os.path.abspath(src_dir), os.path.abspath(prefix)
    extra = [("NVR_SM4", "1")] + ([] if plain else [("NVR_NO_SM3_LIMITS", "1")])
    hlsl = os.path.join(src_dir, template + ".hlsl")
    if not os.path.exists(hlsl): return False, "no source " + hlsl
    sm4 = prefix + ".sm4.asm"
    code, err = fxc(["/T", "ps_4_0", "/Gec", "/Lx", "/O3", "/E", "main", "/I", "."] + define_args(defines + extra) + ["/Fc", sm4, hlsl], src_dir)
    if code: return False, "fxc ps_4_0: " + err
    frag, txt, spv = prefix + ".frag", prefix + ".txt", prefix + ".spv"
    try:
        t = sm4asm2glsl.Translator(sm4asm2glsl.Shader(sm4), sm4asm2glsl.parse_d3d9_ct(original_asm))
        glsl, manifest = t.run()
    except sm4asm2glsl.Err as e:
        return False, "translate: %s" % e
    open(frag, "w", newline="\n").write(glsl)
    open(txt, "w", newline="\n").write(manifest)
    code, msg = spirv(frag, spv)
    if code: return False, "glslang/spirv-val: " + msg
    return True, "%d temps, %d textures, %d float registers, %d bytes SPIR-V%s" % (
        t.temps, len(t.used_tex), len(t.used_regs), os.path.getsize(spv), (" (WARNING: %s not in the original's table)" % ",".join(t.missing)) if t.missing else "")


def cmd_dump(dump_dir, src_dir, out_dir, only=None, plain=False, templates=None):
    os.makedirs(out_dir, exist_ok=True)
    # Intermediate files (.asm, .frag) outside the output folder: that one is copied into the game (game_swap copies all of Compute).
    work = os.environ.get("NOLIMITS_WORK") or os.path.join(os.environ.get("TEMP", out_dir), "nvr-nolimits-work")
    os.makedirs(work, exist_ok=True)
    ok = bad = 0
    for dpath in sorted(glob.glob(os.path.join(dump_dir, "*.defines"))):
        key = os.path.basename(dpath)[:-len(".defines")]
        name = key.split("@")[0]
        if only and name not in only: continue
        pso = os.path.join(dump_dir, key + ".pso")
        if not os.path.exists(pso): print("SKIP %s: no .pso" % key); continue
        lines = open(dpath, encoding="utf-8").read().splitlines()
        template = lines[0].split("=", 1)[1]
        if templates and template not in templates: continue
        defines = [tuple(l.split("=", 1)) for l in lines[1:] if "=" in l]
        orig = os.path.join(work, key + ".ps30.asm")
        code, err = fxc(["/dumpbin", "/Fc", orig, pso], work)
        if code: print("SKIP %s: fxc /dumpbin: %s" % (key, err)); bad += 1; continue
        good, msg = build(src_dir, template, defines, orig, os.path.join(work, key), plain)
        if good:
            for ext in (".spv", ".txt"):
                with open(os.path.join(work, key + ext), "rb") as f, open(os.path.join(out_dir, key + ext), "wb") as g: g.write(f.read())
            ok += 1
        else: bad += 1
        print("%s %s: %s" % ("OK  " if good else "SKIP", key, msg))
    print("done: %d replacements, %d skipped -> %s" % (ok, bad, out_dir))


def cmd_variant(src_dir, template, prefix, defs):
    defines = [tuple(d.split("=", 1)) if "=" in d else (d, "") for d in defs]
    orig_bin, orig_asm = prefix + ".ps30.bin", prefix + ".ps30.asm"
    code, err = fxc(["/T", "ps_3_0", "/E", "main", "/I", "."] + define_args(defines) + ["/Fo", orig_bin, "/Fc", orig_asm, os.path.join(src_dir, template + ".hlsl")], src_dir)
    print("ps_3_0: %s" % ("ok" if not code else err))
    if code: return
    for plain in (True, False):
        good, msg = build(src_dir, template, defines, orig_asm, prefix + (".plain" if plain else ".nolimits"), plain)
        print("%s: %s %s" % ("plain" if plain else "no-limits", "OK" if good else "FAILED", msg))


def main():
    a = sys.argv[1:]
    if len(a) >= 4 and a[0] == "dump":
        only = None
        if "--only" in a: only = set(a[a.index("--only") + 1].split(","))
        templates = set(a[a.index("--templates") + 1].split(",")) if "--templates" in a else None
        cmd_dump(a[1], a[2], a[3], only, "--plain" in a, templates)
    elif len(a) >= 4 and a[0] == "variant":
        cmd_variant(a[1], a[2], a[3], a[4:])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
