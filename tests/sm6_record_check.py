#!/usr/bin/env python3
r"""
sm6_record_check.py: offline check of tools\nvr-sm6\tpl2spv.py (record mode, docs\deferred-single-pass.md step 1).

  py -3.10 -I tests\sm6_record_check.py [--lamps] [--cache DIR]... [--built DIR]

Converts every Deferred\Sun and Deferred\SunFade cache text present (build_deferred.py's default caches, or --cache; --lamps adds the
DeferredLamps family) into a scratch folder, then checks each result with tools that share no code with the converter:
  - spirv-val (Vulkan 1.1) passes;
  - the file name's key = FNV-1a 64 of the cache bytes without the trailing NUL (computed here again);
  - spirv-cross --reflect: every uniform-buffer member sits at 16 x the register the CACHE TEXT pins it to (read here with its own
    regex), arrays at a 16-byte stride, matrices at a 16-byte stride with each 16 bytes one D3D9 register (HLSL row_major = SPIR-V
    column-major and the reverse); members shared by several names (a group) cover each name's registers; uvmap and ztest in the uniform buffer at
    the offsets the layout gives, after every register the text pins; the push block ONLY the used stages' sampler indices (uint
    s[n], one dword per used stage in stage order), at most 48 bytes (tpl2spv.PUSH_LIMIT: DXVK's fragment push budget; past 64 the
    push data overflows into DXVK's sampler indices); textures at set 1 binding = stage; the sampler heap at set 0 binding 0;
    colour outputs at locations 0..targets-1;
  - the layout's used textures are exactly the images the SPIR-V declares.
--built DIR also checks the .spv / .layout pairs already in DIR (src\hlsl\NewVegas\Compute\Deferred), keys excepted (no source).
Exit code 0 = all passed.
"""
import argparse, json, os, re, subprocess, sys, tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(REPO, "tools", "nvr-sm6")
sys.path.insert(0, TOOLS)
import build_deferred, tpl2spv  # noqa: E402
import hlsl2spv  # noqa: E402  (tool lookup only)

FAILS = []


def check(cond, what):
    if not cond:
        FAILS.append(what)
        print("   FAIL: " + what)
    return cond


def fnv1a64(data):
    h = 0xcbf29ce484222325
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


REG_RE = re.compile(r"(?:^|;)\s*(?:(?:uniform|extern|const)\s+)*(row_major\s+|column_major\s+)?(float|half)([1-4](?:x[1-4])?)?\s+(\w+)\s*"
                    r"(?:\[\s*(\d+)\s*\])?\s*:\s*register\s*\(\s*c(\d+)\s*\)", re.M)


def pinned_registers(text):
    """name -> (register, register count) from the cache text's own register(cN) declarations."""
    out = {}
    for m in REG_RE.finditer(text):
        major, _, dims, name, count, reg = m.groups()
        n = int(count) if count else 1
        if dims and "x" in dims:
            per = int(dims[0]) if (major or "").strip() == "row_major" else int(dims[2])
        else:
            per = 1
        out[name] = (int(reg), per * n, (major or "").strip() or ("column_major" if dims and "x" in dims else None))
    return out


def read_layout(path):
    lay = {"const": {}, "group": {}, "texture": {}, "sampler": {}}
    for line in open(path):
        t = line.split()
        if not t or t[0].startswith("#"):
            continue
        if t[0] == "const":
            lay["const"][t[1]] = (int(t[2]), int(t[3]), t[4], t[5], t[6])
        elif t[0] == "group":
            lay["group"][t[1]] = (int(t[2]), int(t[3]))
        elif t[0] == "texture":
            lay["texture"][int(t[1])] = {"set": int(t[2]), "binding": int(t[3]), "slot": int(t[4]), "view": t[5], "used": t[6] == "used"}
        elif t[0] == "sampler":
            lay["sampler"][int(t[1])] = (int(t[2]), int(t[3]))
        elif t[0] == FORMAT_TAG:
            lay["format"] = int(t[1])
        else:
            lay[t[0]] = t[1:]
    return lay


FORMAT_TAG = "nvr-sm6-record"
PUSH_LIMIT = tpl2spv.PUSH_LIMIT


def check_pair(spv, layout, cache_text=None, cache_raw=None):
    name = os.path.basename(spv)
    print("== %s" % name)
    v = subprocess.run([hlsl2spv.tool("spirv-val"), "--target-env", "vulkan1.1", spv], capture_output=True)
    out = (v.stdout + v.stderr).decode(errors="replace").strip()
    print("   spirv-val: %s" % ("ok" if v.returncode == 0 and not out else out or "exit %d" % v.returncode))
    check(v.returncode == 0, "%s: spirv-val" % name)
    lay = read_layout(layout)
    check(lay.get("format") == 2, "%s: layout format" % name)
    key = lay["key"][0]
    check(name == "%s-%s.spv" % (lay["pass"][0], key), "%s: file name vs layout pass/key" % name)
    if cache_raw is not None:
        body = cache_raw[:-1] if cache_raw.endswith(b"\0") else cache_raw
        check("%016x" % fnv1a64(body) == key, "%s: key is not FNV-1a 64 of the cache text" % name)
    r = subprocess.run([hlsl2spv.tool("spirv-cross"), spv, "--reflect"], capture_output=True)
    if not check(r.returncode == 0, "%s: spirv-cross --reflect" % name):
        return
    ref = json.loads(r.stdout.decode())
    types = ref.get("types", {})
    registers = int(lay["registers"][0])
    ubo_line = lay["ubo"]
    ubo_used = ubo_line[4] == "used"
    ubos = ref.get("ubos", [])
    check(len(ubos) == (1 if ubo_used else 0), "%s: %d uniform buffers, layout says %s" % (name, len(ubos), ubo_line[4]))
    pinned = pinned_registers(cache_text) if cache_text is not None else None
    checked = 0
    if ubos:
        u = ubos[0]
        check((u["set"], u["binding"]) == (int(ubo_line[0]), int(ubo_line[1])), "%s: ubo set/binding" % name)
        extras = {"nvr_uvmap": int(lay["uvmap"][0])}
        if "ztest" in lay:
            extras["nvr_ztest"] = int(lay["ztest"][0])
        check(u["block_size"] <= int(ubo_line[3]) == 16 * registers + 16 * len(extras), "%s: ubo size %d, layout %s, registers %d + %d" % (
            name, u["block_size"], ubo_line[3], registers, len(extras)))
        for xn, xoff in extras.items():     # after the D3D9 registers, inside the buffer, a register each
            check(xoff % 16 == 0 and 16 * registers <= xoff and xoff + 16 <= int(ubo_line[3]), "%s: %s at %d (registers %d, ubo %s)" % (
                name, xn, xoff, registers, ubo_line[3]))
        seen = set()
        for m in types[u["type"]]["members"]:
            mn, off = m["name"], m["offset"]
            if mn in extras:
                check(off == extras[mn] and m["type"] == "vec4", "%s: %s at %d (%s), layout %d" % (name, mn, off, m["type"], extras[mn]))
                seen.add(mn)
                continue
            if mn in lay["group"]:
                reg, n = lay["group"][mn]
                check(off == 16 * reg, "%s: group %s at %d, register c%d" % (name, mn, off, reg))
                check(m.get("array_stride") == 16 and m.get("array") == [n], "%s: group %s stride/size" % (name, mn))
                members = [(k, c) for k, c in lay["const"].items() if c[4] == "group=" + mn]
                for k, c in members:   # every name of the group inside it
                    check(reg <= c[0] and c[0] + c[1] <= reg + n, "%s: %s c%d+%d outside group %s" % (name, k, c[0], c[1], mn))
                    if pinned is not None:
                        check(pinned.get(k, (None,))[0] == c[0], "%s: %s layout c%d, text %s" % (name, k, c[0], pinned.get(k)))
                checked += 1
                continue
            c = lay["const"].get(mn)
            if not check(c is not None and c[4] == "member", "%s: block member %s not a layout member" % (name, mn)):
                continue
            reg, nregs, typ, major = c[0], c[1], c[2], c[3]
            if pinned is not None:
                p = pinned.get(mn)
                check(p is not None and p[0] == reg and p[1] == nregs, "%s: %s layout c%d+%d, cache text %s" % (name, mn, reg, nregs, p))
            check(off == 16 * reg, "%s: %s offset %d != 16 x c%d" % (name, mn, off, reg))
            if "array" in m:
                check(m.get("array_stride") == 16, "%s: %s array stride %s" % (name, mn, m.get("array_stride")))
            if m["type"].startswith("mat"):
                check(m.get("matrix_stride") == 16, "%s: %s matrix stride %s" % (name, mn, m.get("matrix_stride")))
                # glslang's HLSL front end flips the qualifier: HLSL row_major (a row per register) = SPIR-V ColMajor
                check(bool(m.get("row_major")) == (major == "column_major"), "%s: %s majorness %s vs SPIR-V row_major=%s" % (name, mn, major, m.get("row_major")))
            checked += 1
        for k, c in lay["const"].items():   # aliases of one member: same register as it
            if c[4].startswith("alias="):
                tgt = lay["const"].get(c[4][6:])
                check(tgt is not None and tgt[0] == c[0] and tgt[2] == c[2], "%s: alias %s -> %s" % (name, k, c[4]))
                if pinned is not None:
                    check(pinned.get(k, (None,))[0] == c[0], "%s: alias %s register" % (name, k))
        check(seen == set(extras), "%s: uniform buffer has %s of %s" % (name, sorted(seen), sorted(extras)))
    if pinned is not None:
        check(set(pinned) == set(lay["const"]), "%s: constants in the text %s vs layout %s" % (name, sorted(set(pinned) ^ set(lay["const"])), ""))
    # push block: the sampler heap indices only, compacted, within DXVK's fragment budget
    push = int(lay["push"][0])
    check(0 < push <= PUSH_LIMIT and push % 4 == 0, "%s: push %d bytes (limit %d)" % (name, push, PUSH_LIMIT))
    check(push == 4 * max(1, len(lay["sampler"])), "%s: push %d bytes for %d samplers" % (name, push, len(lay["sampler"])))
    pcs = ref.get("push_constants", [])
    if lay["sampler"] and check(len(pcs) == 1, "%s: one push block" % name):
        pm = types[pcs[0]["type"]]["members"]
        check([m["name"] for m in pm] == ["s"], "%s: push members %s, only s[] expected" % (name, [m["name"] for m in pm]))
        check(pm[0]["offset"] == 0 and pm[0].get("array_stride") == 4 and pm[0].get("array") == [push // 4], "%s: push s[%d] at 0 stride 4" % (name, push // 4))
        check(types[pcs[0]["type"]].get("size", push) <= push, "%s: push block size %s > %d" % (name, types[pcs[0]["type"]].get("size"), push))
    for k, (st, (slot, off)) in enumerate(sorted(lay["sampler"].items())):
        check(off == 4 * k and slot == st, "%s: sampler stage %d slot %d at push %d, expected %d" % (name, st, slot, off, 4 * k))
    # images and the sampler heap
    imgs = {i["name"]: i for i in ref.get("separate_images", [])}
    want = {"nvr_t%d" % st: t for st, t in lay["texture"].items() if t["used"]}
    if "zref" in lay and lay["zref"][4] == "used":
        want["nvr_zref"] = {"set": int(lay["zref"][0]), "binding": int(lay["zref"][1])}
    check(set(imgs) == set(want), "%s: images %s vs layout %s" % (name, sorted(imgs), sorted(want)))
    for n, t in want.items():
        if n in imgs:
            check((imgs[n]["set"], imgs[n]["binding"]) == (t["set"], t["binding"]), "%s: %s set/binding" % (name, n))
    for st, t in lay["texture"].items():
        check(t["binding"] == st and t["slot"] == st and t["set"] == 1, "%s: texture stage %d binding/slot" % (name, st))
        check(t["used"] == (st in lay["sampler"]), "%s: stage %d used texture without a sampler line (or the reverse)" % (name, st))
    smp = ref.get("separate_samplers", [])
    check(len(smp) == (1 if lay["sampler"] else 0) and all((s["set"], s["binding"]) == (0, 0) for s in smp), "%s: sampler heap" % name)
    # outputs
    locs = sorted(o["location"] for o in ref.get("outputs", []) if "location" in o)
    check(locs == list(range(int(lay["targets"][0]))), "%s: colour outputs %s vs targets %s" % (name, locs, lay["targets"][0]))
    dis = subprocess.run([hlsl2spv.tool("spirv-dis"), spv], capture_output=True).stdout.decode(errors="replace")
    check(("DepthReplacing" in dis) == (lay["depth"][0] == "1"), "%s: depth output vs layout" % name)
    check("FragCoord" in dis, "%s: no FragCoord (VPOS / uv)" % name)
    print("   %d uniform-buffer members checked against their registers, %d textures, %d target(s), depth %s" % (
        checked, len(want), int(lay["targets"][0]), lay["depth"][0]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cache", action="append", default=None)
    ap.add_argument("--lamps", action="store_true")
    ap.add_argument("--built", default=None)
    a = ap.parse_args()
    out = tempfile.mkdtemp(prefix="sm6-record-check-")
    srcs = build_deferred.sources(a.cache or build_deferred.DEFAULT_CACHES, a.lamps)
    check(len(srcs) > 0 or a.built, "no cache texts found")
    for src in srcs:
        print("-- " + src)
        raw = open(src, "rb").read()
        try:
            info = tpl2spv.convert(src, out, verbose=False)
        except SystemExit as e:
            check(False, "%s: conversion failed: %s" % (src, e))
            continue
        base = os.path.join(out, "%s-%s" % (info["pass"], info["key"]))
        check_pair(base + ".spv", base + ".layout", (raw[:-1] if raw.endswith(b"\0") else raw).decode("latin-1"), raw)
    if a.built:
        for f in sorted(os.listdir(a.built)):
            if f.endswith(".spv"):
                check_pair(os.path.join(a.built, f), os.path.join(a.built, f[:-4] + ".layout"))
    if not FAILS:
        import shutil
        shutil.rmtree(out, ignore_errors=True)    # kept after a failure, for a look at the .spv / .layout
    print("%s: %d failure(s)%s" % ("PASS" if not FAILS else "FAIL", len(FAILS), "" if not FAILS else " (outputs kept in %s)" % out))
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()
