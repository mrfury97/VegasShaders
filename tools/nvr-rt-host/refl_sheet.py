"""The refldenoise scene's colour PFM dumps (tools/nvr-rt-host/out/refl_*.pfm, docs/next/RT-REFLECTION-DENOISER.md) as pictures.
  py -3 -I refl_sheet.py <out folder> <prefix> [scale]     one PNG per dump (<prefix>_<name>.png), for bench/label_sheet.py
Colours: x / (1 + x) per channel, then sRGB (the boxes, the panel and the sky stay apart)."""
import glob
import os
import struct
import sys
from PIL import Image


def load(path):
    d = open(path, 'rb').read()
    parts = d.split(b'\n', 3)
    w, h = map(int, parts[1].split())
    a = struct.unpack('<%df' % (w * h * 3), parts[3][:12 * w * h])

    def tone(v):
        v = max(0.0, v)
        v = v / (1.0 + v) * 1.6
        return int(max(0.0, min(1.0, v)) ** (1 / 2.2) * 255 + 0.5)
    im = Image.new('RGB', (w, h))
    im.putdata([(tone(a[3 * i]), tone(a[3 * i + 1]), tone(a[3 * i + 2])) for i in range(w * h)])
    return im.transpose(Image.FLIP_TOP_BOTTOM)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    folder, prefix = argv[0], argv[1]
    scale = int(argv[2]) if len(argv) > 2 else 3
    done = []
    for p in sorted(glob.glob(os.path.join(folder, prefix + '_*.pfm'))):
        im = load(p)
        im = im.resize((im.width * scale, im.height * scale), Image.NEAREST)
        out = p[:-4] + '.png'
        im.save(out)
        done.append(os.path.basename(out))
    print(folder, done)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
