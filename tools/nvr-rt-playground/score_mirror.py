# UNOFFICIAL 2026-11-09 (pt-mirrors) scores the playground's denoised mirror / chrome / glass shots against their matched references.
#   py -3.11 -I tools\nvr-rt-playground\score_mirror.py [--out <folder>] <tag> [<tag> ...]
# For each tag it reads out\05_mirror_spheres<tag>.png against out\09_reference_of_05<tag>.png and out\08_glass_leaf_skin<tag>.png against
# out\10_reference_of_08<tag>.png (the demo's matched references: the same camera and the same frozen moment, ~450 samples; README).
# The shots are the window (1280x720, the 320x180 internal picture scaled up x4 + the HUD): each region is box-averaged back to the
# internal size first. Per region:
#   mae    mean absolute difference from the reference (0-255, all channels), the reference first 3 x 3 median-filtered (its own rare
#          bright paths: the caustics off the spheres)
#   noise  high-frequency energy: mean |luma - its 3 x 3 box mean| (0-255) of the shot, and the same of the (filtered) reference
#   ratio  noise / the reference's noise (1 = as smooth as the reference; a grainy mirror is 2-4)
#   bias   the region's mean luma minus the (filtered) reference's (0-255: + brighter, - darker)
# The regions are fixed window rectangles for the demo's cameras (x0, y0, x1, y1 in window pixels, below the HUD).
import sys, os
import numpy as np
from PIL import Image

REGIONS = {
    "05": [("wall mirror", (692, 184, 1060, 344)), ("chrome sphere", (20, 420, 300, 700)), ("gold sphere", (360, 360, 500, 480)),
           ("floor", (560, 560, 1240, 700))],
    "08": [("glass pane", (600, 156, 830, 296)), ("leaf", (440, 200, 560, 370)), ("sun patch (floor)", (300, 470, 1000, 610))],
}
PAIRS = {"05": ("05_mirror_spheres", "09_reference_of_05"), "08": ("08_glass_leaf_skin", "10_reference_of_08")}


def load(path):
    a = np.asarray(Image.open(path).convert("RGB"), dtype=np.float32)
    h, w = a.shape[0] // 4 * 4, a.shape[1] // 4 * 4
    return a[:h, :w].reshape(h // 4, 4, w // 4, 4, 3).mean(axis=(1, 3))   # back to the internal size


def median3(a):
    p = np.pad(a, ((1, 1), (1, 1), (0, 0)), mode="edge")
    stack = [p[1 + dy:1 + dy + a.shape[0], 1 + dx:1 + dx + a.shape[1]] for dy in (-1, 0, 1) for dx in (-1, 0, 1)]
    return np.median(np.stack(stack), axis=0)


def hf(a):
    l = a @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)
    p = np.pad(l, 1, mode="edge")
    box = sum(p[1 + dy:1 + dy + l.shape[0], 1 + dx:1 + dx + l.shape[1]] for dy in (-1, 0, 1) for dx in (-1, 0, 1)) / 9.0
    return np.abs(l - box)[1:-1, 1:-1].mean()


def score(shot, ref, seg):
    a, r = load(shot), median3(load(ref))
    rows = []
    for name, (x0, y0, x1, y1) in REGIONS[seg]:
        sa, sr = a[y0 // 4:y1 // 4, x0 // 4:x1 // 4], r[y0 // 4:y1 // 4, x0 // 4:x1 // 4]
        n, nr = hf(sa), hf(sr)
        w = np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)
        bias = float((sa @ w).mean() - (sr @ w).mean())
        rows.append((name, float(np.abs(sa - sr).mean()), float(n), float(nr), float(n / max(nr, 1e-3)), bias))
    return rows


def main():
    args = sys.argv[1:]
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out")
    if len(args) >= 2 and args[0] == "--out":
        out, args = args[1], args[2:]
    if not args:
        print(__doc__ or "usage: score_mirror.py [--out <folder>] <tag> ...")
        return 2
    print("| run | segment | region | MAE vs ref | noise | ref noise | noise / ref | bias |")
    print("|---|---|---|---|---|---|---|---|")
    for tag in args:
        for seg, (shot, ref) in PAIRS.items():
            sp, rp = os.path.join(out, shot + tag + ".png"), os.path.join(out, ref + tag + ".png")
            if not (os.path.exists(sp) and os.path.exists(rp)):
                print(f"| {tag} | {seg} | (missing {os.path.basename(sp) if not os.path.exists(sp) else os.path.basename(rp)}) | | | | | |")
                continue
            for name, mae, n, nr, ratio, bias in score(sp, rp, seg):
                print(f"| {tag} | {seg} | {name} | {mae:.2f} | {n:.2f} | {nr:.2f} | {ratio:.2f} | {bias:+.1f} |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
