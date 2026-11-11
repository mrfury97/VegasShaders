# rt_capcheck.py (UNOFFICIAL 2026-11-10; MIT licence, as the helper): a captured game frame (.nvrcap, docs/next/RT-REPLAY.md) as a test
# scene for the path tracer, with no game and no fast GPU. docs/next/RT-CAPCHECK.md has the why and the recipe.
#
#   py -3.11 -I rt_capcheck.py <capture.nvrcap | folder of captures> [--helper NvrRtHelper.exe] [--shaders <folder with .spv>]
#                              [--out <folder>] [--mean 64] [--baseline] [--only albedo,normal,material]
#
# Per capture it replays the frame's RtPathTrace pass several times with patched constants (NvrRtHelper --replay --set / --bits):
#   1. AGREEMENT: the path tracer's debug views of the FIRST surface each pixel sees (1 colour, 2 normal, 4 roughness / metal), once
#      from the game's own screen buffers (flag 8: what the raster game drew) and once from the ray tracing scene (flag 8 off: what the
#      rays hit). Where the two differ, the ray tracing scene is not what the game shows: a wrong texture or tint (a gold head), a mesh
#      the scene lacks or has twice, hair without its colour, a wrong normal. Reported per view: the share of pixels that differ, the
#      mean difference, the biggest differing regions (their boxes), and a side-by-side picture.
#   2. LOOK: the frame as the real-time settings trace it (one run), and the same frame converged (--mean N runs, another random
#      sequence each, as a plain path trace without the denoiser's split): a picture of what the path tracer is aiming at there.
# With --baseline the numbers are stored next to the capture (<capture>.capcheck.json); later runs print what changed against them and
# exit 1 when a view got worse by more than the tolerance: a regression test over a folder of captures.
#
# The pass constants' float indices (RtPathTrace.comp `Pass`): 19 the random seed, 20 the samples already in the running mean (set to 0:
# a ReferencePT capture holds a mean of many frames, which one new sample would not move), 21 bounces, 23 flags, 47 the debug view.
import argparse, glob, json, os, re, struct, subprocess, sys
import numpy as np
from PIL import Image, ImageDraw

SEED, SAMPLES, BOUNCES, FLAGS, VIEW = 19, 20, 21, 23, 47   # SAMPLES: the paths already in the running mean (0 = this frame's alone)
F_GBUFFER, F_DEMOD, F_SPLIT, F_RESTIR, F_CACHE = 8, 512, 4194304, 262144, 524288
PASS = 'RtPathTrace'   # replaced per capture by the frame's own variant (RtPathTraceGrid with PTLightGrid, RtPathTraceGI)
VIEWS = {'albedo': 1, 'normal': 2, 'material': 4}
# a pixel "differs" over this (the views are 0-1 values); worse = the differing share grew by more than TOLERANCE (absolute)
THRESH = {'albedo': 0.10, 'normal': 0.35, 'material': 0.20}
TOLERANCE = 0.02


def read_f32(path):
    with open(path, 'rb') as f:
        magic, w, h, ch = struct.unpack('<4sIII', f.read(16))
        if magic != b'NVRF': raise SystemExit('not an .f32 image: ' + path)
        return np.frombuffer(f.read(w * h * ch * 4), dtype='<f4').reshape(h, w, ch).astype(np.float32)


def replay(helper, capture, out, extra, shaders, runs=1):
    os.makedirs(out, exist_ok=True)
    cmd = [helper, '--replay', capture, '--out', out, '--runs', str(runs)] + extra
    if shaders: cmd += ['--shaders', shaders]
    r = subprocess.run(cmd, capture_output=True, text=True, errors='replace')
    rep = os.path.join(out, 'report.txt')
    if r.returncode != 0 or not os.path.exists(rep):
        raise SystemExit('replay failed (%d): %s\n%s' % (r.returncode, ' '.join(cmd), (r.stdout + r.stderr)[-2000:]))
    return open(rep, encoding='utf-8', errors='replace').read()


def find_pass(report):
    # the frame's path trace pass: RtPathTrace or a variant of it, not its cache pass
    for line in report.splitlines():
        m = re.match(r'\s*pass\s+\d+\s+(\S+)', line)
        if m and m.group(1).lower().startswith('rtpathtrace') and 'cache' not in m.group(1).lower(): return m.group(1)
    return None


def pass_images(report, name):
    # "  pass 0  RtPathTrace   12.3 / 12.1 |  40.2  groups 160x90x1, images [26 27 28w 3 ...]"
    for line in report.splitlines():
        m = re.match(r'\s*pass\s+\d+\s+(\S+).*images \[([^\]]*)\]', line)
        if m and m.group(1).lower() == name.lower():
            return [(int(t.rstrip('w')), t.endswith('w')) for t in m.group(2).split()]
    return None


def pass_ms(report, name):
    for line in report.splitlines():
        m = re.match(r'\s*pass\s+\d+\s+(\S+)\s+([0-9.]+) /', line)
        if m and m.group(1).lower() == name.lower(): return float(m.group(2))
    return -1.0


def to_png(a, scale=1.0, gamma=True):
    x = np.nan_to_num(a[..., :3] if a.shape[2] >= 3 else np.repeat(a[..., :1], 3, axis=2), nan=0.0, posinf=0.0, neginf=0.0)
    x = np.clip(x * scale, 0.0, 1.0)
    if gamma: x = x ** (1.0 / 2.2)
    return Image.fromarray((x * 255.0 + 0.5).astype(np.uint8))


def regions(mask, cell=16, top=5):
    # the worst cells of a coarse grid, merged into boxes: no scipy here
    h, w = mask.shape
    gh, gw = (h + cell - 1) // cell, (w + cell - 1) // cell
    grid = np.zeros((gh, gw), dtype=np.float32)
    for y in range(gh):
        for x in range(gw):
            grid[y, x] = mask[y * cell:(y + 1) * cell, x * cell:(x + 1) * cell].mean()
    hot = grid > 0.5
    seen = np.zeros_like(hot)
    boxes = []
    for y in range(gh):
        for x in range(gw):
            if not hot[y, x] or seen[y, x]: continue
            stack, x0, y0, x1, y1, n = [(y, x)], x, y, x, y, 0
            seen[y, x] = True
            while stack:
                cy, cx = stack.pop(); n += 1
                x0, y0, x1, y1 = min(x0, cx), min(y0, cy), max(x1, cx), max(y1, cy)
                for ny, nx in ((cy + 1, cx), (cy - 1, cx), (cy, cx + 1), (cy, cx - 1)):
                    if 0 <= ny < gh and 0 <= nx < gw and hot[ny, nx] and not seen[ny, nx]: seen[ny, nx] = True; stack.append((ny, nx))
            boxes.append((n, x0 * cell, y0 * cell, min((x1 + 1) * cell, w), min((y1 + 1) * cell, h)))
    boxes.sort(reverse=True)
    return boxes[:top]


def sheet(path, title, panels):
    # panels: [(caption, PIL image)], side by side, a caption under each, the title on top (every picture labelled)
    w = sum(p[1].width for p in panels) + 8 * (len(panels) + 1)
    h = max(p[1].height for p in panels) + 54
    out = Image.new('RGB', (w, h), (24, 24, 24))
    d = ImageDraw.Draw(out)
    d.text((8, 6), title, fill=(255, 255, 255))
    x = 8
    for cap, im in panels:
        out.paste(im, (x, 26)); d.text((x, 30 + im.height), cap, fill=(220, 220, 220)); x += im.width + 8
    out.save(path)


def check(capture, a):
    name = os.path.splitext(os.path.basename(capture))[0]
    out = os.path.join(a.out or os.path.dirname(os.path.abspath(capture)), name + '-capcheck')
    clear = F_DEMOD | F_SPLIT | F_RESTIR | F_CACHE
    result = {'capture': name, 'views': {}}
    # the real-time frame as captured (one run): where the display image is, the pass time
    rep = replay(a.helper, capture, os.path.join(out, 'realtime'), [], a.shaders)
    global PASS
    PASS = find_pass(rep) or 'RtPathTrace'
    imgs = pass_images(rep, PASS)
    if not imgs or len(imgs) < 3:
        print('%s: no RtPathTrace pass in this capture: skipped' % name); return None
    display = imgs[2][0]
    result['realtime_ms'] = pass_ms(rep, PASS)
    lines = ['%s: %s %.1f ms here; display = image %d' % (name, PASS, result['realtime_ms'], display)]
    worst = False
    for view in [v for v in VIEWS if v in a.only]:
        pics = {}
        for src, bits in (('screen', '%d,%d' % (clear, F_GBUFFER)), ('rays', '%d,0' % (clear | F_GBUFFER))):
            d = os.path.join(out, view + '-' + src)
            replay(a.helper, capture, d, ['--set', '%s:%d=%d' % (PASS, VIEW, VIEWS[view]), '--set', '%s:%d=0' % (PASS, SAMPLES), '--bits', '%s:%d=%s' % (PASS, FLAGS, bits)], a.shaders)
            pics[src] = read_f32(os.path.join(d, 'img%02d.f32' % display))[..., :3]
        s, r = np.nan_to_num(pics['screen']), np.nan_to_num(pics['rays'])
        both = (np.abs(s).sum(axis=2) > 1.0e-6) & (np.abs(r).sum(axis=2) > 1.0e-6)   # a surface in both (not sky / a miss)
        only_screen = (np.abs(s).sum(axis=2) > 1.0e-6) & ~(np.abs(r).sum(axis=2) > 1.0e-6)
        diff = np.abs(s - r).max(axis=2)
        if view == 'albedo':   # the two differ in sharpness (the scene's small textures): compare a 5 x 5 mean
            k = 5
            def box(x):
                p = np.pad(x, ((k // 2, k // 2), (k // 2, k // 2), (0, 0)), mode='edge')
                c = np.cumsum(np.cumsum(p, axis=0), axis=1)
                c = np.pad(c, ((1, 0), (1, 0), (0, 0)))
                return (c[k:, k:] - c[:-k, k:] - c[k:, :-k] + c[:-k, :-k]) / (k * k)
            diff = np.abs(box(s) - box(r)).max(axis=2)
        bad = both & (diff > THRESH[view])
        n = max(int(both.sum()), 1)
        v = {'differ': float(bad.sum()) / n, 'mean_diff': float(diff[both].mean()) if both.any() else 0.0,
             'screen_only': float(only_screen.sum()) / max(s.shape[0] * s.shape[1], 1),
             'boxes': [list(map(int, b)) for b in regions(bad | only_screen)]}
        result['views'][view] = v
        lines.append('  %-8s differ %5.1f %% of shared pixels (mean diff %.3f); on screen but not hit by a ray %4.1f %%; worst boxes (cells, x0 y0 x1 y1): %s' % (
            view, 100.0 * v['differ'], v['mean_diff'], 100.0 * v['screen_only'], ' '.join('%d:%d,%d-%d,%d' % tuple(b) for b in v['boxes']) or 'none'))
        g = view != 'normal'
        dm = to_png(np.repeat((bad | only_screen)[..., None].astype(np.float32), 3, axis=2), gamma=False)
        sheet(os.path.join(out, view + '.png'), '%s: %s, the first surface each pixel sees' % (name, view),
              [('the game\'s screen buffers', to_png(s, gamma=g)), ('the ray tracing scene (what the rays hit)', to_png(r, gamma=g)), ('white = differs / not in the scene', dm)])
    # the look: the captured real-time frame's raw light, and the converged plain path trace
    if a.mean > 0:
        d = os.path.join(out, 'reference')
        rep = replay(a.helper, capture, d, ['--mean', '--seed', '%s:%d' % (PASS, SEED), '--set', '%s:%d=0' % (PASS, VIEW), '--set', '%s:%d=0' % (PASS, SAMPLES),
                                           '--bits', '%s:%d=%d,0' % (PASS, FLAGS, clear)], a.shaders, runs=a.mean)
        ref = read_f32(os.path.join(d, 'mean_img%02d.f32' % display))[..., :3]
        one = read_f32(os.path.join(d, 'img%02d.f32' % display))[..., :3]
        scale = 1.0 / max(float(np.percentile(np.nan_to_num(ref), 99.5)), 1.0e-3)
        sheet(os.path.join(out, 'look.png'), '%s: the path tracer on this frame (plain paths, no denoiser), brightness scaled alike' % name,
              [('one path a pixel (a single frame)', to_png(one, scale)), ('the mean of %d frames (what it converges to)' % a.mean, to_png(ref, scale))])
        result['reference_mean'] = [float(x) for x in np.nan_to_num(ref).mean(axis=(0, 1))]
        lines.append('  look     reference mean rgb %.4f %.4f %.4f (%d runs) -> look.png' % (*result['reference_mean'], a.mean))
    base_path = os.path.splitext(capture)[0] + '.capcheck.json'
    if os.path.exists(base_path) and not a.baseline:
        base = json.load(open(base_path))
        for view, v in result['views'].items():
            b = base.get('views', {}).get(view)
            if not b: continue
            delta = v['differ'] - b['differ']
            mark = 'WORSE' if delta > TOLERANCE else 'better' if delta < -TOLERANCE else 'same'
            worst = worst or mark == 'WORSE'
            lines.append('  vs baseline %-8s %5.1f %% -> %5.1f %%: %s' % (view, 100.0 * b['differ'], 100.0 * v['differ'], mark))
    if a.baseline:
        json.dump(result, open(base_path, 'w'), indent=1); lines.append('  baseline written: ' + base_path)
    json.dump(result, open(os.path.join(out, 'result.json'), 'w'), indent=1)
    print('\n'.join(lines)); print('  pictures: ' + out)
    return worst


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('capture')
    ap.add_argument('--helper', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), 'bin', 'NvrRtHelper.exe'))
    ap.add_argument('--shaders', default='')
    ap.add_argument('--out', default='')
    ap.add_argument('--mean', type=int, default=64)
    ap.add_argument('--baseline', action='store_true')
    ap.add_argument('--only', default='albedo,normal,material')
    a = ap.parse_args()
    a.only = a.only.split(',')
    caps = sorted(glob.glob(os.path.join(a.capture, '*.nvrcap'))) if os.path.isdir(a.capture) else [a.capture]
    if not caps: raise SystemExit('no .nvrcap in ' + a.capture)
    worse = [c for c in caps if check(c, a)]
    print('CAPCHECK: %d capture(s), %d worse than their baseline' % (len(caps), len(worse)))
    sys.exit(1 if worse else 0)


if __name__ == '__main__':
    main()
