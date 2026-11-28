#!/usr/bin/env python3
"""5-scene automated slot visual canary runner and baseline comparator for Vegas Shaders."""
import argparse
import glob
import hashlib
import os
import re
import shutil
import subprocess
import sys

SH_EXE = r"C:\Program Files\Git\bin\sh.exe"
MULTIRUN_DIR = "H:/VegasShaders/work/bench/deep-roadmap/multirun"
LOGCHECK_PY = r"H:\VegasShaders\work\bench\deep-roadmap\multirun\logcheck.py"
COMPARE_RUNS_PY = r"H:\VegasShaders\work\bench\deep-roadmap\multirun\compare_runs.py"
FLIP_PYTHON = r"H:\VegasShaders\work\bench\foundations\dl\flip-venv\Scripts\python.exe"

SCENES = [
    {
        "name": "fp-rmaos",
        "scene": "noon",
        "list": "canary-fp-rmaos",
        "label": "canary-fp-rmaos",
        "toml": "G:/_NVR-build-scratch/fp-rmaos/fp-rmaos.toml",
        "extra": "",
        "key_shots": ["cr-gun", "cr-dv2", "amr-gun"],
    },
    {
        "name": "fp-vanilla-weapon",
        "scene": "noon",
        "list": "canary-fp-vanilla-weapon",
        "label": "canary-fp-vanilla-weapon",
        "toml": "G:/_NVR-build-scratch/fp-rmaos/fp-rmaos.toml",
        "extra": "",
        "key_shots": ["smg-gun", "plasma-gun"],
    },
    {
        "name": "int-glow",
        "scene": "lucky22",
        "list": "canary-int-glow",
        "label": "canary-int-glow",
        "toml": "G:/_NVR-build-scratch/perf-audit-1127/toml-native-115.toml",
        "extra": "",
        "key_shots": ["int-default"],
    },
    {
        "name": "ext-flora",
        "scene": "og-desert-near",
        "list": "canary-ext-flora",
        "label": "canary-ext-flora",
        "toml": "G:/_NVR-build-scratch/perf-audit-1127/toml-native-115.toml",
        "extra": "",
        "key_shots": ["ext-default"],
    },
    {
        "name": "night-lamps",
        "scene": "og-night-near",
        "list": "canary-night-lamps",
        "label": "canary-night-lamps",
        "toml": "G:/_NVR-build-scratch/perf-audit-1127/toml-native-115.toml",
        "extra": "",
        "key_shots": ["night-default"],
    },
]


def to_fwd(path):
    return path.replace("\\", "/")


def dll_sha256(tree):
    dll = os.path.join(r"H:\VegasShaders", tree, "build", "optimized", "VegasShaders.dll")
    if not os.path.exists(dll):
        dll = os.path.join(r"H:\VegasShaders\code", "build", "optimized", "VegasShaders.dll")
    return hashlib.sha256(open(dll, "rb").read()).hexdigest()


def run_scene(spec, args, expected_sha):
    cmd_parts = [
        f"LOCKTAG={args.locktag}",
        f"SLOT={args.slot}",
        f"RUN_TOML={spec['toml']}",
    ]
    if spec["extra"]:
        cmd_parts.append(f"SLOT_EXTRA={spec['extra']}")
    cmd_parts.append(
        f"sh {MULTIRUN_DIR}/run_slot_when_free.sh {spec['scene']} {spec['list']} {spec['label']} {args.tree}"
    )
    sh_cmd = " ".join(cmd_parts)
    proc = subprocess.run([SH_EXE, "-lc", sh_cmd], capture_output=True, text=True)
    out = proc.stdout + "\n" + proc.stderr
    m_pass = re.search(r"RESULT:\s+([A-Za-z]:\S+|/\S+)\s+(\S+)", out)
    run_dir = m_pass.group(1) if m_pass else ""
    shots_dir = m_pass.group(2) if m_pass else ""

    verdict_ok = False
    verdict_line = f"exit {proc.returncode}"
    stop_detected = "STOP-multirun" in out or os.path.exists(os.path.join(MULTIRUN_DIR, "STOP-multirun.txt"))
    m_fail = re.search(r"(RESULT:\s+FAILED.*|REFUSED:.*)", out)
    if m_fail and not run_dir:
        verdict_line = m_fail.group(1).strip()
    elif stop_detected and not run_dir:
        verdict_line = "STOP-multirun.txt present"
    run_sha = ""
    x4505_cnt = compile_fail_cnt = flow_cnt = standalone_cnt = -1
    logcheck_ok = False

    if run_dir and os.path.isdir(run_dir):
        vp = os.path.join(run_dir, "verdict.txt")
        if os.path.exists(vp):
            vlines = open(vp, errors="replace").read().splitlines()
            verdict_line = vlines[0] if vlines else ""
            verdict_ok = verdict_line.startswith("VERDICT: PASS")
        rp = os.path.join(run_dir, "run-info.txt")
        if os.path.exists(rp):
            for l in open(rp, errors="replace"):
                if l.startswith("dll_sha256 "):
                    run_sha = l.split(" ", 1)[1].strip()
        lc = subprocess.run([sys.executable, LOGCHECK_PY, run_dir], capture_output=True, text=True)
        logcheck_ok = lc.returncode == 0
        logs = sorted(glob.glob(os.path.join(run_dir, "VegasShaders_*.log")))
        if logs:
            text = open(logs[0], encoding="utf-8", errors="replace").read()
            x4505_cnt = text.count("X4505")
            compile_fail_cnt = len(re.findall(r"failed to compile", text, re.I))
            flow_cnt = len(re.findall(r"PREFER_FLOW_CONTROL", text))
            standalone_cnt = len(re.findall(r"DeferredStandalone:", text))
            # Check HEARTBEAT zero line if present; note log headers themselves contain the literal word PREFER_FLOW_CONTROL in "0 PREFER_FLOW_CONTROL retries"
            hb = re.findall(r"HEARTBEAT counts.*?\n", text)
            if hb:
                last_hb = hb[-1]
                m_cf = re.search(r"(\d+)\s+compile failures,\s+(\d+)\s+PREFER_FLOW_CONTROL retries", last_hb)
                if m_cf:
                    compile_fail_cnt = int(m_cf.group(1))
                    flow_cnt = int(m_cf.group(2))
            else:
                flow_cnt = len(re.findall(r"out of temp registers at the default flags; compiled again with PREFER_FLOW_CONTROL", text))

    sha_ok = bool(run_sha) and run_sha.lower() == expected_sha.lower()

    flip_summary = "-"
    flip_ok = True
    if args.baseline and shots_dir and os.path.isdir(shots_dir):
        base_scene = os.path.join(args.baseline, spec["name"])
        if os.path.isdir(base_scene) and os.path.exists(FLIP_PYTHON):
            cp = subprocess.run([FLIP_PYTHON, "-I", COMPARE_RUNS_PY, base_scene, shots_dir], capture_output=True, text=True)
            m_sum = re.search(r"SUMMARY:.*", cp.stdout)
            flip_summary = m_sum.group(0) if m_sum else f"compare rc={cp.returncode}"
            if cp.returncode != 0:
                flip_ok = False

    if args.save_baseline and shots_dir and os.path.isdir(shots_dir):
        dst = os.path.join(args.save_baseline, spec["name"])
        if os.path.exists(dst):
            shutil.rmtree(dst)
        shutil.copytree(shots_dir, dst)

    all_ok = (
        verdict_ok
        and logcheck_ok
        and sha_ok
        and x4505_cnt == 0
        and compile_fail_cnt == 0
        and flow_cnt == 0
        and standalone_cnt == 0
        and flip_ok
    )
    return {
        "name": spec["name"],
        "ok": all_ok,
        "verdict": verdict_line,
        "logcheck": logcheck_ok,
        "sha_ok": sha_ok,
        "run_sha": run_sha[:12],
        "x4505": x4505_cnt,
        "compile_fail": compile_fail_cnt,
        "flow": flow_cnt,
        "standalone": standalone_cnt,
        "flip": flip_summary,
        "run_dir": run_dir,
        "shots_dir": shots_dir,
        "stop_detected": stop_detected,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scenes", default="all", help="Comma-separated subset or 'all'")
    ap.add_argument("--tree", default="VSBENCH-trunk")
    ap.add_argument("--slot", default="C")
    ap.add_argument("--locktag", default="vscanary")
    ap.add_argument("--baseline", default="", help="Baseline directory containing <scene>/ shot folders")
    ap.add_argument("--save-baseline", default="", help="Save shot folders into <dir>/<scene>/ after passing")
    ap.add_argument("--report", default="", help="Write Markdown summary report")
    args = ap.parse_args()

    wanted = SCENES if args.scenes == "all" else [s for s in SCENES if s["name"] in args.scenes.split(",")]
    if not wanted:
        print("No matching canary scenes.", file=sys.stderr)
        return 2

    exp_sha = dll_sha256(args.tree)
    print(f"Visual Canary: {len(wanted)} scene(s), tree={args.tree}, slot={args.slot}, dll_sha256={exp_sha[:16]}")
    results = []
    for spec in wanted:
        print(f"--- Running {spec['name']} ({spec['scene']} / {spec['list']}) ---")
        res = run_scene(spec, args, exp_sha)
        results.append(res)
        status = "PASS" if res["ok"] else "FAIL"
        print(
            f"    {status} | {res['verdict']} | logcheck={res['logcheck']} sha={res['sha_ok']} "
            f"X4505={res['x4505']} fail={res['compile_fail']} flow={res['flow']} standalone={res['standalone']}"
        )
        if res.get("stop_detected"):
            print("    Aborting remaining scenes because STOP-multirun.txt was detected.", file=sys.stderr)
            break

    lines = [
        "# Vegas Shaders Visual Canary Report",
        f"- **Tree:** `{args.tree}` | **Slot:** `{args.slot}` | **Expected DLL SHA256:** `{exp_sha}`",
        "",
        "| Scene | Status | Verdict | logcheck | SHA OK | X4505 / Fail / Flow / Standalone | FLIP / Notes | Run | Shots |",
        "|---|---|---|---|---|---|---|---|---|",
    ]
    for r in results:
        lines.append(
            f"| `{r['name']}` | {'PASS' if r['ok'] else 'FAIL'} | {r['verdict']} | {r['logcheck']} | {r['sha_ok']} "
            f"| {r['x4505']} / {r['compile_fail']} / {r['flow']} / {r['standalone']} | {r['flip']} | `{r['run_dir']}` | `{r['shots_dir']}` |"
        )
    report_txt = "\n".join(lines) + "\n"
    if args.report:
        os.makedirs(os.path.dirname(os.path.abspath(args.report)), exist_ok=True)
        open(args.report, "w", encoding="utf-8").write(report_txt)
        print(f"Report written to {args.report}")
    print("\n" + report_txt)
    return 0 if all(r["ok"] for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
