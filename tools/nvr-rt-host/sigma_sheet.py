"""The sigma scenes' PFM dumps (tools/nvr-rt-host/out/sigma_*.pfm, docs/next/RT-DENOISER.md) as pictures.
  py -3 -I sigma_sheet.py <out folder> <prefix> <png> [scale]          one PNG strip: exact | raw | denoised | old filter | 8 rays
  py -3 -I sigma_sheet.py <out folder> <prefix> --panels [scale]       one PNG per dump (<prefix>_<name>.png), for bench/label_sheet.py
Red = the lamp does not light that pixel (or no surface)."""
import os
import struct
import sys
from PIL import Image

NAMES = ('exact', 'raw', 'denoised', 'filter', 'rays8', 'moving')


def load(path):
    d = open(path, 'rb').read()
    parts = d.split(b'\n', 3)
    w, h = map(int, parts[1].split())
    a = struct.unpack('<%df' % (w * h), parts[3][:4 * w * h])
    im = Image.new('RGB', (w, h))
    im.putdata([(160, 30, 30) if v < 0 else (int(max(0, min(1, v)) * 255),) * 3 for v in a])
    return im.transpose(Image.FLIP_TOP_BOTTOM)


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    folder, prefix = argv[0], argv[1]
    panels = argv[2] == '--panels'
    scale = int(argv[3]) if len(argv) > 3 else 3
    names = [n for n in NAMES if os.path.exists(os.path.join(folder, f'{prefix}_{n}.pfm'))]
    ims = [load(os.path.join(folder, f'{prefix}_{n}.pfm')) for n in names]
    ims = [i.resize((i.width * scale, i.height * scale), Image.NEAREST) for i in ims]
    if panels:
        for n, i in zip(names, ims):
            i.save(os.path.join(folder, f'{prefix}_{n}.png'))
        print(folder, names)
        return 0
    out = Image.new('RGB', (sum(i.width for i in ims) + 10 * (len(ims) - 1), ims[0].height), (40, 40, 40))
    x = 0
    for i in ims:
        out.paste(i, (x, 0))
        x += i.width + 10
    out.save(argv[2])
    print(argv[2], names)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
