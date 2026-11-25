# UNOFFICIAL 2026-11-18 checks src/core/DdsTranscode.h with tests/dds_transcode.cpp against Pillow's decoders (py -3.11 -I, Pillow + numpy).
# usage: dds_transcode_check.py <dds_transcode.exe> <workdir> <bc7 file.dds>...
# Per file: DecodeBC7 must equal Pillow's BC7 decode exactly; the BC3 file ToLegacy writes is decoded by Pillow and compared (PSNR).
import sys, os, subprocess, io
import numpy as np
from PIL import Image

exe, work = sys.argv[1], sys.argv[2]
bad = 0
for f in sys.argv[3:]:
    base = os.path.join(work, os.path.splitext(os.path.basename(f))[0])
    out = subprocess.run([exe, f, base + '.rgba', base + '.legacy.dds'], capture_output=True, text=True).stdout.strip()
    ref = np.asarray(Image.open(f).convert('RGBA'))
    h, w = ref.shape[:2]
    mine = np.fromfile(base + '.rgba', np.uint8).reshape(h, w, 4)
    diff = int((mine != ref).any(axis=2).sum())
    leg = Image.open(base + '.legacy.dds')
    bc3 = np.asarray(leg.convert('RGBA')).astype(np.float64)
    mse = ((bc3 - ref.astype(np.float64)) ** 2).mean(axis=(0, 1))
    psnr = [99.0 if m == 0 else 10 * np.log10(255 * 255 / m) for m in mse]
    print(f"{os.path.basename(f)}: {out} | decode vs Pillow: {diff} pixels differ | legacy file {leg.format} {leg.mode} {leg.size} "
          f"| BC3 PSNR R {psnr[0]:.1f} G {psnr[1]:.1f} B {psnr[2]:.1f} A {psnr[3]:.1f} dB")
    if diff: bad += 1
print('FAIL' if bad else 'PASS', f'({bad} files with decode differences)')
sys.exit(1 if bad else 0)
