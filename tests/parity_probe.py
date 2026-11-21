"""TEST ONLY: reads the DeferredSun parity probe's files (NvrDxvkDeferredResolve 14, src/core/DeferredSun.h ParityProbe) and compares
the D3D9 draw (A) with the Vulkan draw (B) of the same frame.

    py -3.10 -I tests/parity_probe.py <folder with parity_*.bin> [frame] [--legacy] [--png out.png]

Without a frame, the newest frame in the folder is used. Prints:
  - per-channel |A - B| (mean, max, 99th percentile) over the pixels both sides drew, and the sentinel (-1) counts;
  - with DebugView 5 (the shader writes N * 0.5 + 0.5): the normal recomputed from the G-buffer (m.ga, octahedral, as
    DeferredSun.hlsl DSShadingNormal / Includes/Shadow.hlsl ShadowDecodeNormal; COLOR3's n.rg where m.g says "no bumped normal"),
    and how closely A and B each match it, also against the G-buffer read one pixel off (a texel offset shows there);
  - a PNG of |A - B| x 16 (magenta: only one side drew the pixel).
--legacy: the G-buffer without GBufferV2 (m.g oct.x, m.a 1.01 + 0.49 (oct.y + 1)).
"""
import argparse
import glob
import os
import re
import struct
import sys
import zlib

import numpy as np

# D3DFORMAT -> (numpy dtype, channels); A8R8G8B8 / X8R8G8B8 are B G R A bytes in memory
FORMATS = {
    116: (np.float32, 4),  # A32B32G32R32F: r g b a
    113: (np.float16, 4),  # A16B16G16R16F: r g b a
    115: (np.float32, 2),  # G32R32F
    112: (np.float16, 2),  # G16R16F
    114: (np.float32, 1),  # R32F
    111: (np.float16, 1),  # R16F
    21: (np.uint8, 4),     # A8R8G8B8
    22: (np.uint8, 4),     # X8R8G8B8
}


def read_bin(path):
    """(rgba float64 array H x W x 4, header dict). Missing channels are 0 (alpha 1)."""
    with open(path, "rb") as f:
        head = f.read(28)
        magic, version, width, height, fmt, bpp, debug_view = struct.unpack("<4s6I", head)
        if magic != b"NVRP" or version != 1:
            raise ValueError(f"{path}: not a parity probe file")
        if fmt not in FORMATS:
            raise ValueError(f"{path}: format {fmt} is not decoded here")
        dtype, ch = FORMATS[fmt]
        data = np.frombuffer(f.read(width * height * bpp), dtype=dtype)
    px = data.reshape(height, width, ch).astype(np.float64)
    if dtype == np.uint8:
        px = px[..., [2, 1, 0, 3]] / 255.0
        if fmt == 22:
            px[..., 3] = 1.0
    out = np.zeros((height, width, 4))
    out[..., 3] = 1.0
    out[..., :ch] = px
    return out, {"width": width, "height": height, "format": fmt, "debug_view": debug_view}


def oct_decode(e, dtype=np.float64):
    """Includes/Shadow.hlsl ShadowDecodeNormal, element-wise: n = (e, 1 - |e.x| - |e.y|), t = saturate(-n.z),
    n.xy += (n.x >= 0 ? -t : t, n.y >= 0 ? -t : t), normalize(n)."""
    e = e.astype(dtype)
    n = np.empty(e.shape[:-1] + (3,), dtype=dtype)
    n[..., 0] = e[..., 0]
    n[..., 1] = e[..., 1]
    n[..., 2] = 1.0 - np.abs(e[..., 0]) - np.abs(e[..., 1])
    t = np.clip(-n[..., 2], 0.0, 1.0)
    n[..., 0] += np.where(n[..., 0] >= 0.0, -t, t)
    n[..., 1] += np.where(n[..., 1] >= 0.0, -t, t)
    length = np.sqrt(np.sum(n * n, axis=-1, keepdims=True))
    return n / np.where(length > 0.0, length, 1.0)


def shading_normal(m, n, legacy, dtype=np.float64):
    """DeferredSun.hlsl DSShadingNormal: (N, bumped mask)."""
    if legacy:
        bumped = (m[..., 3] > 1.005) & (m[..., 3] < 1.995)
        e = np.stack([m[..., 1], (m[..., 3] - 1.01) / 0.49 - 1.0], axis=-1)
    else:
        bumped = np.abs(m[..., 1]) <= 1.001
        e = np.stack([m[..., 1], m[..., 3]], axis=-1)
    nb = oct_decode(e, dtype)
    ns = oct_decode(n[..., 0:2], dtype)
    return np.where(bumped[..., None], nb, ns), bumped


def shifted(img, dx, dy):
    """img read one pixel off: out[y, x] = img[y + dy, x + dx], edges clamped (as the G-buffer's CLAMP sampler)."""
    h, w = img.shape[:2]
    ys = np.clip(np.arange(h) + dy, 0, h - 1)
    xs = np.clip(np.arange(w) + dx, 0, w - 1)
    return img[ys][:, xs]


def write_png(path, rgb8):
    h, w, _ = rgb8.shape
    raw = b"".join(b"\x00" + rgb8[y].tobytes() for y in range(h))

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


def stats_line(name, d):
    if d.size == 0:
        return f"  {name}: no pixels"
    return (f"  {name}: mean {d.mean():.6f}  max {d.max():.6f}  p99 {np.percentile(d, 99):.6f}  "
            f"within 1/255 {np.mean(d <= 1.0 / 255.0) * 100:.2f} %  within 1e-3 {np.mean(d <= 1e-3) * 100:.2f} %")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folder")
    ap.add_argument("frame", nargs="?", type=int)
    ap.add_argument("--legacy", action="store_true", help="the G-buffer without GBufferV2")
    ap.add_argument("--png", help="where the |A-B| x16 picture goes (default: parity_<frame>_diff16.png in the folder)")
    args = ap.parse_args()

    frames = sorted({int(m.group(1)) for p in glob.glob(os.path.join(args.folder, "parity_*_A.bin"))
                     for m in [re.search(r"parity_(\d+)_A\.bin$", p)] if m})
    if not frames:
        sys.exit(f"no parity_<frame>_A.bin in {args.folder}")
    frame = args.frame if args.frame is not None else frames[-1]
    path = lambda tag: os.path.join(args.folder, f"parity_{frame}_{tag}.bin")
    a, ha = read_bin(path("A"))
    b, hb = read_bin(path("B"))
    dv = ha["debug_view"]
    print(f"frame {frame}: {ha['width']}x{ha['height']}, A format {ha['format']}, B format {hb['format']}, DebugView {dv}"
          f" (frames in the folder: {frames})")
    if a.shape != b.shape:
        sys.exit("A and B differ in size")

    sent_a = np.all(a == -1.0, axis=-1)
    sent_b = np.all(b == -1.0, axis=-1)
    both = ~sent_a & ~sent_b & np.all(np.isfinite(a), axis=-1) & np.all(np.isfinite(b), axis=-1)
    print(f"sentinel pixels: A {sent_a.sum()}, B {sent_b.sum()}, only A {np.sum(sent_a & ~sent_b)}, only B {np.sum(sent_b & ~sent_a)}; "
          f"compared {both.sum()} of {a.shape[0] * a.shape[1]}")
    diff = np.abs(a - b)
    print("|A - B| per channel (pixels both sides drew):")
    for c, name in enumerate("rgba"):
        print(stats_line(name, diff[..., c][both]))
    signed = (a - b)[both]
    if signed.size:
        print("  mean A - B (signed): " + "  ".join(f"{n} {signed[:, c].mean():+.6f}" for c, n in enumerate("rgba")))

    # the recomputed normal (DebugView 5 writes N * 0.5 + 0.5)
    try:
        m, hm = read_bin(path("m"))
        n, hn = read_bin(path("n"))
    except (OSError, ValueError) as e:
        m = n = None
        print(f"G-buffer files not read: {e}")
    if m is not None and m.shape[:2] != a.shape[:2]:
        print(f"the G-buffer is {m.shape[1]}x{m.shape[0]}, not the targets' size: no recomputation")
        m = None
    if m is not None:
        if dv != 5:
            print(f"DebugView {dv}: the normal comparison needs DebugView 5 (the probe wrote the shader's view {dv}); skipped")
        else:
            for dtype in (np.float64, np.float32):
                N, bumped = shading_normal(m, n, args.legacy, dtype)
                expect = N.astype(np.float64) * 0.5 + 0.5
                print(f"recomputed N ({np.dtype(dtype).name}; {'legacy' if args.legacy else 'GBufferV2'} decode; bumped pixels "
                      f"{np.sum(bumped & both)} of {both.sum()} compared):")
                for side, img, mask in (("A (D3D9)", a, ~sent_a), ("B (Vulkan)", b, ~sent_b)):
                    err = np.max(np.abs(img[..., :3] - expect), axis=-1)
                    print(stats_line(f"{side} max-channel error, all drawn", err[mask]))
                    print(stats_line(f"{side} max-channel error, bumped only", err[mask & bumped]))
            # which G-buffer texel each side read: the error against m / n read one pixel off
            print("error against the G-buffer read one pixel off (mean max-channel error, bumped pixels both drew; lowest = the texel read):")
            for side, img in (("A (D3D9)", a), ("B (Vulkan)", b)):
                cells = []
                for dy in (-1, 0, 1):
                    for dx in (-1, 0, 1):
                        N, bumped = shading_normal(shifted(m, dx, dy), shifted(n, dx, dy), args.legacy)
                        mask = both & bumped
                        err = np.max(np.abs(img[..., :3] - (N * 0.5 + 0.5)), axis=-1)[mask]
                        cells.append(f"({dx:+d},{dy:+d}) {err.mean() if err.size else float('nan'):.5f}")
                print(f"  {side}: " + "  ".join(cells))

    out = args.png or os.path.join(args.folder, f"parity_{frame}_diff16.png")
    img = np.clip(diff[..., :3] * 16.0, 0.0, 1.0)
    img[~both] = 0.0
    img[sent_a ^ sent_b] = (1.0, 0.0, 1.0)
    write_png(out, (img * 255.0 + 0.5).astype(np.uint8))
    print(f"|A - B| x16: {out}")


if __name__ == "__main__":
    main()
