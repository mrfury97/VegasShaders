#!/usr/bin/env python3
# refl-hashes.py (2026-11-24): rewrites the hash table of src/effects/Reflections/Reflections.cpp LogReflShaderFiles from the .spv files
# in src/hlsl/NewVegas/Compute, so the game log's "Reflections shader files" line calls this build's own files "this build's".
# The table was kept by hand and went stale whenever one of those shaders was rebuilt (1.1.3 and 1.1.4 shipped with 10 of its 11 hashes
# old, so every log said OLD (copy the new one) and the menu's Install check showed "1 to fix" on a clean install).
# compile-compute.sh runs it after every build. By hand: python tools/nvr-dxvk/refl-hashes.py [--check]
#   --check: change nothing; exit 1 (and name them) when a hash in the table differs from its file.
import os, re, sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
COMPUTE = os.path.join(ROOT, "src", "hlsl", "NewVegas", "Compute")
SOURCE = os.path.join(ROOT, "src", "effects", "Reflections", "Reflections.cpp")
ENTRY = re.compile(r'\{ "((?:[^"\\]|\\\\)+)", 0x([0-9A-Fa-f]{8})u \}')


def fnv1a(path):  # as Reflections.cpp ReflFileHash
    h = 2166136261
    with open(path, "rb") as f:
        for b in f.read():
            h ^= b
            h = (h * 16777619) & 0xFFFFFFFF
    return h


def main():
    check = "--check" in sys.argv[1:]
    with open(SOURCE, "r", encoding="utf-8", newline="") as f:
        text = f.read()
    start = text.find("static void LogReflShaderFiles()")
    start = text.find("wants[] = {", start) if start >= 0 else -1
    end = text.find("};", start)
    if start < 0 or end < 0:
        sys.exit("refl-hashes: LogReflShaderFiles' table not found in " + SOURCE)
    table = text[start:end]
    stale = []

    def fix(m):
        name = m.group(1).replace("\\\\", "\\")
        path = os.path.join(COMPUTE, name.replace("\\", os.sep) + ".spv")
        if not os.path.exists(path):
            sys.exit("refl-hashes: %s.spv is missing" % name)
        now = fnv1a(path)
        if now != int(m.group(2), 16):
            stale.append("%s 0x%s -> 0x%08X" % (name, m.group(2).upper(), now))
        return '{ "%s", 0x%08Xu }' % (m.group(1), now)

    new_table, count = ENTRY.subn(fix, table)
    if count == 0:
        sys.exit("refl-hashes: no entries found in LogReflShaderFiles' table")
    if check:
        for s in stale:
            print("stale: " + s)
        sys.exit(1 if stale else 0)
    if stale:
        with open(SOURCE, "w", encoding="utf-8", newline="") as f:
            f.write(text[:start] + new_table + text[end:])
        print("refl-hashes: %d of %d updated in Reflections.cpp: %s" % (len(stale), count, "; ".join(stale)))
    else:
        print("refl-hashes: all %d in step" % count)


if __name__ == "__main__":
    main()
