"""Constant-block layout check for the RT trace passes (tools/nvr-rt-host/README.md).

The game's C++ fills each trace pass's 512-byte constant block from a struct (RtLampShadows.h PassConstants, ...); the shader reads
it as a std140 uniform block. Nothing ties the two together at build time, and they drifted on 2026-11-06 (RtLampTrace.comp had
manyUnused[7] where the C++ has 6 vec4s: jitter read the wrong vec4 and alpha-tested casters were never tested in lamp shadows).

For every pair below this reads both declarations, computes each member's byte offset (C++: float[4] / float[N][4] / D3DXVECTOR4
members; GLSL std140: vec4 / vec4[N] / mat4 members) and checks:
  - a member with the same name on both sides sits at the same offset (renamed padding such as manyUnused is ignored by name, but
    everything after it is still compared);
  - the shader's block is no longer than the C++ struct (the shader would read bytes the game never fills);
  - both fit the protocol's 512 bytes.
The host's own mirror structs (rt_host.cpp) are checked against the game's the same way.
Exit code 1 on any mismatch. Usage: py -3 rt_layout_check.py [repo root]
"""
import os
import re
import sys

ROOT = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", ".."))
RT = "src/hlsl/NewVegas/Compute/Rt/"

# (C++ file, struct name) -> shader files whose `uniform Pass` block it fills
PAIRS = [
    (("src/core/RtLampShadows.h", "PassConstants"), [RT + "RtLampTrace.comp", RT + "RtLampTraceMany.comp", RT + "RtLampTraceRestir.comp"]),
    (("src/core/RtLampShadows.h", "PrimaryConstants"), [RT + "RtLampPrimary.comp"]),   # 2026-11-07 SameFrame
    (("src/core/RtPathTrace.cpp", "PassConstants"), [RT + "RtPathTrace.comp"]),
    (("src/core/RtPathTrace.cpp", "RestirConstants"), [RT + "RtRestirGI.comp"]),
    (("src/core/RtSunShadows.cpp", "PassConstants"), [RT + "RtSunShadow.comp"]),
    (("src/effects/Reflections/Reflections.cpp", "RtPassConstants"), [RT + "ReflectionsRTBody.glsl"]),
    # (2026-11-07) the game's own fork passes: RT reflections' RayGen / Resolve block, and the RT reflection denoiser's push block
    (("src/effects/Reflections/Reflections.cpp", "ReflRtUniforms"), ["src/hlsl/NewVegas/Compute/ReflectionsRT.glsl"]),
    (("src/core/RtReflDenoise.h", "Push"), ["src/hlsl/NewVegas/Compute/RtReflDenoise.glsl"]),
]
# the host's mirrors -> the game's structs
MIRRORS = [
    (("tools/nvr-rt-host/rt_host.cpp", "LampConstants"), ("src/core/RtLampShadows.h", "PassConstants")),
    (("tools/nvr-rt-host/rt_host_sameframe.inl", "PrimaryConstantsHost"), ("src/core/RtLampShadows.h", "PrimaryConstants")),
    (("tools/nvr-rt-host/rt_host.cpp", "PathConstants"), ("src/core/RtPathTrace.cpp", "PassConstants")),
    (("tools/nvr-rt-host/rt_host.cpp", "ReflConstants"), ("src/effects/Reflections/Reflections.cpp", "RtPassConstants")),
]


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def body_after(text, start):
    """The text between the '{' at or after start and its matching '}'."""
    i = text.index("{", start)
    depth = 0
    for j in range(i, len(text)):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                return text[i + 1:j]
    raise ValueError("unbalanced braces")


def cpp_layout(path, name):
    text = strip_comments(open(os.path.join(ROOT, path), encoding="utf-8", errors="replace").read())
    m = re.search(r"\bstruct\s+" + re.escape(name) + r"\s*\{", text)
    if not m:
        raise ValueError(f"{path}: no struct {name}")
    members, offset = [], 0
    for decl in body_after(text, m.start()).split(";"):
        decl = decl.strip()
        if not decl:
            continue
        t = re.match(r"(float|D3DXVECTOR4|uint32_t)\s+(.*)$", decl, re.S)
        if t and t.group(1) == "uint32_t":   # (2026-11-07) 4-byte scalars (a push block's sampler indices)
            for d in t.group(2).split(","):
                members.append((d.strip(), offset, 4))
                offset += 4
            continue
        if not t:
            raise ValueError(f"{path} {name}: member '{decl}' is not float[4]-based (check it by hand)")
        for d in t.group(2).split(","):
            d = d.strip()
            dm = re.match(r"(\w+)((?:\s*\[\s*\d+\s*\])*)$", d)
            if not dm:
                raise ValueError(f"{path} {name}: cannot read '{d}'")
            dims = [int(x) for x in re.findall(r"\d+", dm.group(2))]
            if t.group(1) == "float" and len(dims) == 1 and dims[0] == 16:
                vec4s = 4   # (2026-11-07) float[16]: a mat4 (the push block's reprojection)
            elif t.group(1) == "float":
                if not dims or dims[-1] != 4:
                    raise ValueError(f"{path} {name}: '{d}' is not a float[...][4] (std140 would pad it): check by hand")
                vec4s = 1
                for x in dims[:-1]:
                    vec4s *= x
            else:
                vec4s = 1
                for x in dims:
                    vec4s *= x
            members.append((dm.group(1), offset, vec4s * 16))
            offset += vec4s * 16
    return members, offset


def glsl_layout(path):
    text = strip_comments(open(os.path.join(ROOT, path), encoding="utf-8", errors="replace").read())
    blocks = list(re.finditer(r"std140\)\s*uniform\s+\w+\s*\{", text))
    if not blocks:   # (2026-11-07) or the push block (vec4 / mat4 / uint members: no std140 padding differences then)
        blocks = list(re.finditer(r"push_constant\)\s*uniform\s+\w+\s*\{", text))
    if len(blocks) != 1:
        raise ValueError(f"{path}: {len(blocks)} std140 blocks (variants by #ifdef: check by hand)")
    members, offset = [], 0
    for decl in body_after(text, blocks[0].start()).split(";"):
        decl = decl.strip()
        if not decl:
            continue
        u = re.match(r"uint\s+([\w\s,]+)$", decl)
        if u:
            for n in u.group(1).split(","):
                members.append((n.strip(), offset, 4))
                offset += 4
            continue
        t = re.match(r"(vec4|mat4)\s+(\w+)\s*(?:\[\s*(\d+)\s*\])?$", decl)
        if not t:
            raise ValueError(f"{path}: member '{decl}' is not vec4 / mat4 based (check it by hand)")
        size = (16 if t.group(1) == "vec4" else 64) * (int(t.group(3)) if t.group(3) else 1)
        members.append((t.group(2), offset, size))
        offset += size
    return members, offset


def compare(label_a, a, label_b, b, b_may_be_shorter=True):
    """a: the filling side (C++), b: the reading side. Returns error lines."""
    errors = []
    (ma, size_a), (mb, size_b) = a, b
    off_a = {n: o for n, o, _ in ma}
    for n, o, _ in mb:
        if n in off_a and off_a[n] != o:
            errors.append(f"{label_b} reads '{n}' at byte {o} (vec4 {o // 16}), {label_a} writes it at byte {off_a[n]} (vec4 {off_a[n] // 16})")
    if size_b > size_a:
        errors.append(f"{label_b} is {size_b} bytes, longer than {label_a} ({size_a}): it reads bytes the game never fills")
    if not b_may_be_shorter and size_b != size_a:
        errors.append(f"{label_b} is {size_b} bytes, {label_a} {size_a}")
    for label, size in ((label_a, size_a), (label_b, size_b)):
        if size > 512:
            errors.append(f"{label} is {size} bytes, over the protocol's 512")
    return errors


def main():
    failures = 0
    checked = 0
    for (cpp, name), shaders in PAIRS:
        try:
            c = cpp_layout(cpp, name)
        except OSError as e:
            print(f"FAIL {cpp}: {e}")
            failures += 1
            continue
        except ValueError as e:
            print(f"SKIP {cpp} {name}: {e}")
            continue
        for sh in shaders:
            try:
                g = glsl_layout(sh)
            except OSError as e:
                print(f"FAIL {sh}: {e}")
                failures += 1
                continue
            except ValueError as e:
                print(f"SKIP {sh}: {e}")
                continue
            errs = compare(f"{os.path.basename(cpp)} {name}", c, os.path.basename(sh), g)
            checked += 1
            common = len({n for n, _, _ in c[0]} & {n for n, _, _ in g[0]})
            if errs:
                failures += 1
                print(f"FAIL {os.path.basename(sh)} <- {os.path.basename(cpp)} {name}")
                for e in errs:
                    print("     " + e)
            else:
                print(f"ok   {os.path.basename(sh)} <- {os.path.basename(cpp)} {name} ({c[1]} / {g[1]} bytes, {common} named members at the same offsets)")
    for (mf, mn), (gf, gn) in MIRRORS:
        try:
            m, g = cpp_layout(mf, mn), cpp_layout(gf, gn)
        except (ValueError, OSError) as e:
            print(f"FAIL mirror {mn}: {e}")
            failures += 1
            continue
        errs = compare(f"{os.path.basename(gf)} {gn}", g, f"host {mn}", m, b_may_be_shorter=False)
        checked += 1
        names_g, names_m = [n for n, _, _ in g[0]], [n for n, _, _ in m[0]]
        if names_g != names_m:
            errs.append(f"host {mn} members {names_m} differ from the game's {names_g}")
        if errs:
            failures += 1
            print(f"FAIL host {mn} vs {os.path.basename(gf)} {gn}")
            for e in errs:
                print("     " + e)
        else:
            print(f"ok   host {mn} = {os.path.basename(gf)} {gn}")
    print(f"layout: {checked} pairs checked, {failures} failed -> {'FAIL' if failures else 'PASS'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
