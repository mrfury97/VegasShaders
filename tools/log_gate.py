"""Shader and fallback gate for a Vegas Shaders build (2026-11-28, after the 1.1.6 merge's DeferredLamps X4505 got past the bench).

Compares two VegasShaders_*.log files (the previous build's and the new build's, same scene and settings) and fails when the new one has
a compile failure, a shader made again without a feature, a record that did not load, or a [D3-FALLBACK] line the old one did not have.
Optionally lists the shaders the DLL itself recorded as failed in a game's shader cache (<binary>.failed.hlsl: the exact text the game's
D3DX9 compiler was given, with its runtime defines in <binary>.failed.defines) and compares those too.

Why not an offline compile of the templates: the game compiles with its own D3DX9 and the runtime define sets (DL_LAND_SPLIT, lamp SSS,
...); the offline tool's newer compiler fits shaders the game rejects (ShaderRecord.cpp, the .failed.hlsl comment). The game's own first
start is the faithful compile; this gate reads what it reported.

usage: py -3.10 -I tools/log_gate.py <old.log> <new.log> [--old-cache <Cache dir>] [--new-cache <Cache dir>]
exit 0 = nothing new; 1 = new problems (listed); 2 = bad arguments.
"""
import os, re, sys

PATTERNS = [
    r"did not compile", r"\bX4505\b", r"error X\d{4}", r"made again without", r"did not load", r"compiled without it",
    r"deferred record only", r"\[D3-FALLBACK\]", r"out of temp registers", r"FAILED", r"failed to compile",
]
RX = re.compile("|".join(PATTERNS), re.I)

def normalize(line):
    line = re.sub(r"^\[[^\]]*\]\s*", "", line.strip())              # [hh:mm:ss.mmm fN]
    line = re.sub(r"\(\s*\d+(\.\d+)? ms[^)]*\)", "", line)          # timings
    line = re.sub(r"\bf\d+\b", "f#", line)
    line = re.sub(r"0x[0-9A-Fa-f]+", "0x#", line)
    line = re.sub(r"\(known from an earlier start, or now\)", "", line)   # first start vs a later one: the same failure
    line = re.sub(r"known from an earlier start \(out of temp registers\): not compiled again", "(out of temp registers)", line)
    line = re.sub(r"this exact text did not compile at an earlier start \((X\d+)[^)]*\): not compiled again.*", r"did not compile (\1)", line)
    return re.sub(r"\s+", " ", line).strip()

# lines that match a pattern but report no problem: periodic stats ("marks failed 0"), "0 did not load" summaries
IGNORE = re.compile(r"DEFERRED RENDERER [0-9]+ frames|[^0-9]0 did not load|[^0-9]0 failed", re.I)

def problems(path):
    out = {}
    prev = ""
    with open(path, encoding="utf-8", errors="replace") as f:
        for raw in f:
            if RX.search(raw) and not IGNORE.search(raw):
                n = normalize(raw)
                # a bare "error X4505 ..." line names no shader: tie it to the line before it (the record that failed)
                key = (prev + " -> " + n) if re.match(r"^error X\d+", n) else n
                out[key] = out.get(key, 0) + 1
            if not re.match(r"^\s*$", raw):
                prev = normalize(raw)[:160]
    return out

def failed_records(cache):
    found = set()
    for root, _, files in os.walk(cache):
        for fn in files:
            if fn.endswith(".failed.hlsl"):
                found.add(os.path.relpath(os.path.join(root, fn[:-len(".failed.hlsl")]), cache).replace("\\", "/"))
    return found

def main(argv):
    args = [a for a in argv if not a.startswith("--")]
    opts = dict(zip(argv[::1], argv[1::1]))
    if len(args) < 2:
        print(__doc__); return 2
    old, new = problems(args[0]), problems(args[1])
    added = sorted(k for k in new if k not in old)
    gone = sorted(k for k in old if k not in new)
    bad = bool(added)
    print(f"old: {sum(old.values())} problem lines ({len(old)} kinds) | new: {sum(new.values())} ({len(new)} kinds)")
    for k in added: print("NEW   ", k)
    for k in gone: print("gone  ", k)
    oc, nc = opts.get("--old-cache"), opts.get("--new-cache")
    if nc:
        nf = failed_records(nc); of = failed_records(oc) if oc else set()
        for r in sorted(nf - of): print("NEW failed record (cache):", r); bad = True
        for r in sorted(of - nf): print("gone failed record (cache):", r)
    print("GATE:", "FAIL" if bad else "PASS")
    return 1 if bad else 0

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
