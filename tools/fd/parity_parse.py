"""Turns a Vegas Shaders log's parity self-test lines ("PARITY v1 ...", src/core/ParityTest.h) into one table per shot.

Usage:  py -3.11 -I tools/fd/parity_parse.py <VegasShaders.log> [--point world|final|all] [--band 1.8] [--json] [--regions]

Per shot (and per capture point) one row per measurement, then the median row:
  AB mean / >8 % / >16 %   the A-vs-B difference (A2 against B1)
  floor >16 %               the larger of the repeat floors AA (A1 vs A2) and BB (B1 vs B2)
  excess >16 %              AB >16 minus the floor
  luma A -> B               which way the difference goes (8-bit levels)
  readms / camera           the read-back stall (ms, the largest of the run's reads) and how far the camera moved (units)
--band X marks each shot's median AB >16 % against X (the plan's parity band, e.g. 1.8 for DeferredLamps indoors); --regions adds
the 3 x 3 grid of AB >16 % (median over the measurements, top row first). Stopped, refused and failed runs are listed under the table.
Exit code: 0, or 2 if the log has no PARITY lines.
"""
import argparse
import json
import re
import statistics
import sys

LINE = re.compile(r"PARITY v1 (.*)$")
WORD = re.compile(r"(\w+)=(\S+)")


def parse_line(text):
    """One PARITY line -> (fields dict, note text after ' | ' or '')."""
    m = LINE.search(text)
    if not m:
        return None
    body = m.group(1)
    head, _, note = body.partition(" | ")
    fields = dict(WORD.findall(head))
    return fields, note.strip()


def number(v, default=None):
    try:
        return float(v)
    except (TypeError, ValueError):
        return default


def numbers(v):
    try:
        return [float(x) for x in v.split(",")]
    except (AttributeError, ValueError):
        return []


def collect(lines):
    """-> (shots, notes). shots: {(run, shot): {point: {m: {'pairs': {pair: fields}, 'summary': fields}}}}; notes: [str]."""
    shots, notes, order = {}, [], []
    for text in lines:
        parsed = parse_line(text)
        if not parsed:
            continue
        f, note = parsed
        run, shot = f.get("run", "?"), f.get("shot", "?")
        key = (run, shot)
        kind = f.get("kind")
        if kind in ("stopped", "refused", "error"):
            where = f" point {f['point']}" if "point" in f else ""
            notes.append(f"{shot} (run {run}){where}: {kind}: {note}")
            continue
        if kind in ("start", "done"):
            if kind == "start" and key not in shots:
                shots[key] = {"_start": f}
                order.append(key)
            continue
        point = f.get("point")
        m_text = f.get("m", "1/1")
        try:
            m = int(m_text.split("/")[0])
        except ValueError:
            continue
        if key not in shots:
            shots[key] = {}
            order.append(key)
        slot = shots[key].setdefault(point, {}).setdefault(m, {"pairs": {}, "summary": None})
        if "pair" in f:
            slot["pairs"][f["pair"]] = f
        elif kind == "summary":
            slot["summary"] = f
    return [(k, shots[k]) for k in order], notes


def row_of(m, slot):
    p = slot["pairs"]
    ab, aa, bb = p.get("AB", {}), p.get("AA", {}), p.get("BB", {})
    floors = [number(x.get("gt16")) for x in (aa, bb) if x]
    floors = [x for x in floors if x is not None]
    floor = max(floors) if floors else None
    gt16 = number(ab.get("gt16"))
    s = slot["summary"] or {}
    return {
        "m": m,
        "ab_mean": number(ab.get("mean")), "ab_gt8": number(ab.get("gt8")), "ab_gt16": gt16,
        "aa_gt16": number(aa.get("gt16")), "bb_gt16": number(bb.get("gt16")), "floor_gt16": floor,
        "excess_gt16": (gt16 - floor) if gt16 is not None and floor is not None else None,
        "lumaA": number(ab.get("lumaA")), "lumaB": number(ab.get("lumaB")),
        "readms": number(s.get("readms_max")), "camera": number(s.get("camera_moved")),
        "size": s.get("size"), "valid": s.get("valid"),
        "regions_gt16": numbers(ab.get("r_gt16", "")),
    }


def median_row(rows):
    out = {"m": "median"}
    for k in ("ab_mean", "ab_gt8", "ab_gt16", "aa_gt16", "bb_gt16", "floor_gt16", "excess_gt16", "lumaA", "lumaB", "readms", "camera"):
        vals = [r[k] for r in rows if r[k] is not None]
        out[k] = statistics.median(vals) if vals else None
    regs = [r["regions_gt16"] for r in rows if len(r["regions_gt16"]) == 9]
    out["regions_gt16"] = [statistics.median(col) for col in zip(*regs)] if regs else []
    out["size"] = rows[0]["size"] if rows else None
    readms = [r["readms"] for r in rows if r["readms"] is not None]
    out["readms"] = max(readms) if readms else None
    return out


def fmt(v, digits=3):
    return "-" if v is None else f"{v:.{digits}f}"


def build(lines, point_filter="all"):
    shots, notes = collect(lines)
    result = []
    for (run, shot), data in shots:
        start = data.get("_start", {})
        for point in sorted((p for p in data if p != "_start"), key=lambda p: ({"world": 0, "final": 1}.get(p, 2), p)):
            if point_filter != "all" and point != point_filter:
                continue
            rows = [row_of(m, slot) for m, slot in sorted(data[point].items())]
            if not rows:
                continue
            result.append({
                "run": run, "shot": shot, "point": point,
                "switch": start.get("switch"), "A": start.get("A"), "B": start.get("B"), "frames": start.get("frames"),
                "rows": rows, "median": median_row(rows),
            })
    return result, notes


def render(tables, notes, band=None, regions=False):
    out = []
    for t in tables:
        out.append(f"### {t['shot']} ({t['point']}, run {t['run']}): {t['switch']} A={t['A']} B={t['B']}, {t['frames']} frames a block, "
                   f"{t['median']['size'] or '?'}")
        out.append("")
        out.append("| m | AB mean | AB >8 % | AB >16 % | AA >16 % | BB >16 % | excess >16 % | luma A -> B | readms | camera |")
        out.append("|---|---|---|---|---|---|---|---|---|---|")
        for r in t["rows"] + [t["median"]]:
            out.append(f"| {r['m']} | {fmt(r['ab_mean'])} | {fmt(r['ab_gt8'])} | {fmt(r['ab_gt16'])} | {fmt(r['aa_gt16'])} | {fmt(r['bb_gt16'])} | "
                       f"{fmt(r['excess_gt16'])} | {fmt(r['lumaA'], 2)} -> {fmt(r['lumaB'], 2)} | {fmt(r['readms'], 1)} | {fmt(r['camera'], 1)} |")
        med = t["median"]
        if band is not None and med["ab_gt16"] is not None:
            verdict = "WITHIN" if med["ab_gt16"] <= band else "OVER"
            out.append("")
            out.append(f"Band: median AB >16 {med['ab_gt16']:.3f} % vs {band} %: **{verdict}**; floor {fmt(med['floor_gt16'])} %.")
        if regions and len(med["regions_gt16"]) == 9:
            g = med["regions_gt16"]
            out.append("")
            out.append("AB >16 % by region (median): " + " / ".join(" ".join(f"{v:.2f}" for v in g[i:i + 3]) for i in (0, 3, 6)))
        out.append("")
    if notes:
        out.append("Not measured:")
        out.extend(f"- {n}" for n in notes)
        out.append("")
    return "\n".join(out)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("log")
    ap.add_argument("--point", default="all", choices=["world", "final", "all"])
    ap.add_argument("--band", type=float, default=None)
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--regions", action="store_true")
    a = ap.parse_args(argv)
    with open(a.log, encoding="utf-8", errors="replace") as fh:
        lines = fh.readlines()
    tables, notes = build(lines, a.point)
    if a.json:
        print(json.dumps({"tables": tables, "notes": notes}, indent=1))
    else:
        print(render(tables, notes, a.band, a.regions))
    return 0 if tables or notes else 2


if __name__ == "__main__":
    sys.exit(main())
