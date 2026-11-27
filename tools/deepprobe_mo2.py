"""Performance probe live report -> cost per MO2 mod.

Reads one live-mode report folder written by Vegas Shaders' Help > Performance probe (VegasShaders-Logs\\DeepProbe_Live_<time>\\:
modules.csv, script_plugins.csv, spikes.csv, frames.csv) and an MO2 instance, works out which enabled MO2 mod supplies each DLL and
each .esp/.esm (the winning one in the profile's priority order; MO2's overwrite folder wins over all) and prints a table per MO2 mod.
Standard library only. See docs/TOOLS.md "Performance probe".

    py -3.10 -I tools\\deepprobe_mo2.py [<report folder>] --mo2 <MO2 folder> [--profile <name>] [--logs <folder>] [--top N] [--out file.txt]

--mo2 may also come from the environment variable VS_MO2. Without --profile, MO2's selected profile (ModOrganizer.ini) is used.
Without a report folder, the newest DeepProbe_Live_* folder in --logs (default: <game folder from ModOrganizer.ini>\\VegasShaders-Logs)
is used.
"""
import argparse
import csv
import os
import re
import sys


def read_csv(path):
    if not os.path.isfile(path):
        return []
    with open(path, newline="", encoding="utf-8", errors="replace") as f:
        return list(csv.DictReader(line for line in f if not line.startswith("#")))


def mo2_ini_value(mo2, key):
    """A [General] value of ModOrganizer.ini, with MO2's @ByteArray(...) wrapper removed."""
    path = os.path.join(mo2, "ModOrganizer.ini")
    if not os.path.isfile(path):
        return None
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith(key + "="):
                value = line.split("=", 1)[1].strip()
                m = re.match(r"@ByteArray\((.*)\)$", value)
                value = m.group(1) if m else value
                return value.replace("\\\\", "\\").replace("/", "\\")
    return None


def enabled_mods(mo2, profile):
    """Enabled mods, lowest priority first (modlist.txt lists the highest priority first)."""
    path = os.path.join(mo2, "profiles", profile, "modlist.txt")
    mods = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\r\n")
            if line.startswith("+"):
                mods.append(line[1:])
    mods.reverse()
    return mods


class ModIndex:
    def __init__(self, mo2, profile):
        self.root = os.path.join(mo2, "mods")
        self.overwrite = os.path.join(mo2, "overwrite")
        self.mods = enabled_mods(mo2, profile)
        self.cache = {}

    def owner(self, rel_paths):
        """Highest-priority enabled mod that has any of rel_paths; the overwrite folder wins over all."""
        key = tuple(p.lower() for p in rel_paths)
        if key in self.cache:
            return self.cache[key]
        hit = None
        if any(os.path.exists(os.path.join(self.overwrite, rel)) for rel in rel_paths):
            hit = "(MO2 overwrite)"
        else:
            for mod in reversed(self.mods):
                base = os.path.join(self.root, mod)
                if any(os.path.exists(os.path.join(base, rel)) for rel in rel_paths):
                    hit = mod
                    break
        self.cache[key] = hit
        return hit

    def for_module(self, name, path):
        low = path.lower()
        marker = os.path.join(self.root, "").lower()
        if low.startswith(marker):			# the real path inside the mods folder
            return path[len(marker):].split("\\", 1)[0]
        if "\\windows\\" in low:
            return None
        rel = None
        m = re.search(r"\\data\\(.*)$", path, re.IGNORECASE)	# the game's virtual path: Data\NVSE\Plugins\x.dll and the like
        if m:
            rel = m.group(1)
        candidates = [os.path.join("NVSE", "Plugins", name), name, os.path.join("root", name)]
        if rel:
            candidates.insert(0, rel)
        return self.owner(candidates)

    def for_plugin(self, name):
        if not name or name.startswith("[") or name.startswith("("):
            return None
        return self.owner([name])


def f(x, default=0.0):
    try:
        return float(x)
    except (TypeError, ValueError):
        return default


def newest_session(logs):
    if not logs or not os.path.isdir(logs):
        return None
    dirs = [os.path.join(logs, d) for d in os.listdir(logs) if d.startswith("DeepProbe_Live_") and os.path.isdir(os.path.join(logs, d))]
    return max(dirs, key=os.path.getmtime) if dirs else None


ENGINE_ROLES = ("Game", "OS / CRT", "Windows", "Driver / DXVK", "Trampolines / JIT")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("session", nargs="?")
    ap.add_argument("--mo2", default=os.environ.get("VS_MO2"))
    ap.add_argument("--profile")
    ap.add_argument("--logs")
    ap.add_argument("--top", type=int, default=30)
    ap.add_argument("--out")
    a = ap.parse_args()

    if not a.mo2 or not os.path.isdir(a.mo2):
        sys.exit("Give the MO2 folder with --mo2 (or the VS_MO2 environment variable).")
    profile = a.profile or mo2_ini_value(a.mo2, "selected_profile")
    if not profile:
        sys.exit("No --profile given and none selected in ModOrganizer.ini.")
    logs = a.logs
    if not logs:
        game = mo2_ini_value(a.mo2, "gamePath")
        logs = os.path.join(game, "VegasShaders-Logs") if game else None
    session = a.session or newest_session(logs)
    if not session or not os.path.isdir(session):
        sys.exit("No report folder given or found (DeepProbe_Live_* in %s)." % logs)
    idx = ModIndex(a.mo2, profile)

    modules = read_csv(os.path.join(session, "modules.csv"))
    script_plugins = read_csv(os.path.join(session, "script_plugins.csv"))
    spikes = read_csv(os.path.join(session, "spikes.csv"))
    frames = read_csv(os.path.join(session, "frames.csv"))

    agg = {}

    def row(mod):
        return agg.setdefault(mod, {"near": 0.0, "own": 0.0, "script": 0.0, "parts": set(), "spikes": 0})

    other = {}
    dll_owner = {}
    for m in modules:
        name, role = m["name"], m["role"]
        near, own = f(m["near_ms_per_frame"]), f(m["own_ms_per_frame"])
        if role in ENGINE_ROLES:
            other[f"{name} [{role}]"] = other.get(f"{name} [{role}]", 0.0) + near
            continue
        owner = m.get("mo2_mod") or idx.for_module(name, m.get("path", ""))
        dll_owner[name.lower()] = owner or name
        r = row(owner or name)
        r["near"] += near
        r["own"] += own
        r["parts"].add(name)
    for s in script_plugins:
        owner = idx.for_plugin(s["plugin"])
        r = row(owner or s["plugin"])
        r["script"] += f(s["self_ms_per_frame"])
        r["parts"].add(s["plugin"])
    for s in spikes:
        top = (s.get("top_near_dlls") or "").split(";")[0].strip()
        name = top.rsplit(" ", 1)[0] if top else ""
        if name.lower() in dll_owner:
            row(dll_owner[name.lower()])["spikes"] += 1

    out = []
    p = out.append
    p(f"Report folder: {session}")
    p(f"MO2: {a.mo2} (profile {profile})")
    ms_list = sorted(f(x["ms"]) for x in frames if not (int(f(x.get("flags"), 0)) & 2)) if frames else []
    if ms_list:
        avg = sum(ms_list) / len(ms_list)
        p(f"{len(ms_list)} frames (loading left out), avg {avg:.2f} ms ({1000 / avg:.1f} fps), "
          f"99% {ms_list[int(0.99 * (len(ms_list) - 1))]:.2f} ms, {len(spikes)} spikes kept")
    p("")
    p(f"TOP {a.top} MO2 MODS BY MAIN-THREAD COST")
    p("  near = sampled share x mean frame time, engine work a DLL's hooks call included (heuristic); own = the DLL's own code;")
    p("  script = exact script time per frame; spikes = kept spike frames whose top near DLL is this mod's.")
    p(f"  {'MO2 mod':<52} {'near':>8} {'own':>8} {'script':>8} {'spikes':>6}  files")
    ranked = sorted(agg.items(), key=lambda kv: max(kv[1]["near"], kv[1]["script"]), reverse=True)
    for name, r in ranked[: a.top]:
        parts = ", ".join(sorted(r["parts"]))[:70]
        p(f"  {name[:52]:<52} {r['near']:8.3f} {r['own']:8.3f} {r['script']:8.3f} {r['spikes']:6d}  {parts}")
    p("")
    p("NOT CHARGED TO ANY MOD (near ms/frame)")
    for name, ms in sorted(other.items(), key=lambda kv: kv[1], reverse=True)[:10]:
        p(f"  {name[:70]:<70} {ms:8.3f}")
    text = "\n".join(out)
    print(text)
    if a.out:
        with open(a.out, "w", encoding="utf-8") as fo:
            fo.write(text + "\n")


if __name__ == "__main__":
    main()
