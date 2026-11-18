#!/usr/bin/env python3
r"""
build.py: compile every pass listed in passes.txt (tools/nvr-sm6) to SPIR-V for the NVR DXVK fork, one file per variant of the
compile-time defines EffectRecord::BuildMacros passes to the effects (only the defines the effect's sources test), into
src/hlsl/NewVegas/Compute/Sm6 (deployed as Data\Shaders\VegasShaders\Compute\Sm6). UNOFFICIAL 2026-10-26 (deep/fork-sm6).

  python3 tools/nvr-sm6/build.py            (Linux/WSL; glslang + spirv-val in ~/vktools/root/usr/bin or on PATH)

Re-run it after changing an effect (or an include) that has an SM6 build: NVR refuses a .spv whose recorded source hashes no
longer match the deployed .hlsl (logged; the D3D9 pass is used), so a forgotten rebuild shows up as "stale", never as a wrong picture.

Per pass: <Effect>.<Technique>.<Pass>.txt (the index: which defines matter) and per variant <...>.<key>.spv + <...>.<key>.txt
(the layout). key = FNV-1a 32 of "NAME=VALUE;NAME-;..." over the index's defines in that order (NAME- = not defined), the same
string core/Sm6Pass.cpp builds from the defines the effect was compiled with.
"""
import itertools, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
HLSL = os.path.join(REPO, "src", "hlsl", "NewVegas")
OUT = os.path.join(HLSL, "Compute", "Sm6")
sys.path.insert(0, HERE)
import hlsl2spv  # noqa: E402

# EffectRecord::BuildMacros: name -> the states it can take (None = not defined). Keep in step with EffectRecord.cpp.
MACROS = [
    ("FORWARD_SHADOWS", ["0", "1"]),
    ("PHYSICAL_SKY_ON", ["0", "1"]),
    ("REVERSED_DEPTH", [None, ""]),
    ("REFL_MATERIAL_ON", [None, "1"]),
    ("REFL_ADD_ON", [None, "1"]),
    ("REFL_SPECOCC_ON", [None, "1"]),
    ("CHARACTER_SHADOW_MAP", [None, "1"]),
    ("GBUFFER_V2", [None, "1"]),   # 2026-11-14 GBufferV2 (core/CapturedNormals.h; EffectRecord::BuildMacros)
]


def relevant(effect_path):
    used = set()
    for f in hlsl2spv.include_files(effect_path):
        for line in open(f, "rb").read().decode("latin-1").splitlines():
            if re.match(r"^\s*#\s*(if|ifdef|ifndef|elif)\b", line):
                used.update(re.findall(r"\b\w+\b", line))
    return [(n, s) for n, s in MACROS if n in used]


def key_string(states):
    return ";".join(("%s=%s" % (n, v)) if v is not None else ("%s-" % n) for n, v in states)


def main():
    os.makedirs(OUT, exist_ok=True)
    lines = [l.split("#", 1)[0].split() for l in open(os.path.join(HERE, "passes.txt"))]
    failed = 0
    for parts in lines:
        if not parts:
            continue
        effect, technique, pass_ = parts[0], parts[1], parts[2]
        pinned = dict(p.split("=", 1) for p in parts[3:])   # NAME=VALUE: only that state is built (NAME=- : not defined)
        path = os.path.join(HLSL, "Effects", effect + ".fx.hlsl")
        rel = relevant(path)
        choices = []
        for n, states in rel:
            if n in pinned:
                states = [None if pinned[n] == "-" else pinned[n]]
            choices.append([(n, s) for s in states])
        base = "%s.%s.%s" % (effect, technique, pass_)
        index = ["nvr-sm6-index 1", "macros " + " ".join(n for n, _ in rel)]
        for combo in itertools.product(*choices) if choices else [()]:
            ks = key_string(combo)
            key = "%08x" % hlsl2spv.fnv1a32(ks.encode())
            defs = []
            for n, v in combo:
                if v is not None:
                    defs += ["-D", "%s=%s" % (n, v)]
            cmd = [sys.executable, os.path.join(HERE, "hlsl2spv.py"), path, technique, pass_, "--out", OUT, "--variant", key,
                   "--hash-root", HLSL] + defs
            r = subprocess.run(cmd, capture_output=True, text=True)
            sys.stdout.write("[%s %s] %s" % (base, ks or "-", r.stdout))
            if r.stderr.strip():
                sys.stdout.write("".join("    " + l + "\n" for l in r.stderr.strip().splitlines()))
            if r.returncode != 0:
                failed += 1
                continue
            index.append("variant %s %s" % (key, ks or "-"))
        open(os.path.join(OUT, base + ".txt"), "w").write("\n".join(index) + "\n")
    print("done, %d failed" % failed)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
