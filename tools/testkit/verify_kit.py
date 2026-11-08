# verify_kit.py <Data folder> [nif_mesh.py]: checks a test-kit Data folder (2026-11-07): every .esp with verify_esp.py, every .nif read back
# block by block (header, block sizes add up to the file, strings, texture paths, geometry: vertex / triangle counts, indices in range,
# triangles facing their normals), every .dds header against its file size; optionally nif_mesh.py (the GunFX reader) on each .nif too.
import glob, os, struct, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vs_nif as vn

HERE = os.path.dirname(os.path.abspath(__file__))


def read_nif(path):
    data = open(path, "rb").read()
    p = data.index(b"\n") + 1
    ver, endian, uv, nb, bsver = struct.unpack_from("<IBIII", data, p); p += 17
    assert (ver, uv, bsver) == (0x14020007, 11, 34), (hex(ver), uv, bsver)
    for _ in range(3): p += 1 + data[p]
    nt = struct.unpack_from("<H", data, p)[0]; p += 2; types = []
    for _ in range(nt):
        n = struct.unpack_from("<I", data, p)[0]; p += 4; types.append(data[p:p + n].decode()); p += n
    ti = struct.unpack_from("<%dH" % nb, data, p); p += 2 * nb
    sizes = struct.unpack_from("<%dI" % nb, data, p); p += 4 * nb
    ns, ml = struct.unpack_from("<II", data, p); p += 8; strings = []
    for _ in range(ns):
        n = struct.unpack_from("<I", data, p)[0]; p += 4; strings.append(data[p:p + n].decode("latin1")); p += n
    assert ml == max(len(s) for s in strings)
    ng = struct.unpack_from("<I", data, p)[0]; p += 4 + 4 * ng
    blocks = []
    for s in sizes: blocks.append((types[ti[len(blocks)]], data[p:p + s])); p += s
    assert data[p:] == struct.pack("<Ii", 1, 0), "footer"
    report = []
    for i, (t, b) in enumerate(blocks):
        if t == "BSShaderTextureSet":
            n = struct.unpack_from("<i", b, 0)[0]; q = 4; paths = []
            for _ in range(n):
                L = struct.unpack_from("<I", b, q)[0]; q += 4; paths.append(b[q:q + L].decode("latin1")); q += L
            assert q == len(b); report.append("textures %s" % [x for x in paths if x])
        elif t == "NiTriShapeData":
            gid, nv, keep, comp, hasv = struct.unpack_from("<iHBBB", b, 0); q = 9
            V = [struct.unpack_from("<3f", b, q + 12 * k) for k in range(nv)]; q += 12 * nv
            vflags, hasn = struct.unpack_from("<HB", b, q); q += 3
            N = [struct.unpack_from("<3f", b, q + 12 * k) for k in range(nv)]; q += 12 * nv
            if vflags & 0x1000: q += 24 * nv
            q += 16; hasc = b[q]; q += 1 + 16 * nv * hasc
            q += 8 * nv * (vflags & 1)
            q += 6
            ntri, npts, hast = struct.unpack_from("<HIB", b, q); q += 7
            T = [struct.unpack_from("<3H", b, q + 6 * k) for k in range(ntri)]; q += 6 * ntri
            nmg = struct.unpack_from("<H", b, q)[0]; q += 2
            assert q == len(b), "NiTriShapeData size"
            assert npts == 3 * ntri and all(max(t3) < nv for t3 in T)
            bad = vn.check_facing(V, N, T)
            assert bad == 0, "%d triangles face away from their normals" % bad
            report.append("geometry %d vertices %d triangles" % (nv, ntri))
        elif t == "NiAlphaProperty":
            report.append("alpha %04X ref %d" % struct.unpack_from("<HB", b, 12))
    return [t for t, _ in blocks], strings, report


def read_dds(path):
    d = open(path, "rb").read()
    assert d[:4] == b"DDS "
    _, flags, h, w, pitch, depth, mips = struct.unpack_from("<7I", d, 4)
    bits = struct.unpack_from("<I", d, 88)[0]
    size = 0; ww, hh = w, h
    for _ in range(max(mips, 1)):
        size += ww * hh * bits // 8; ww, hh = max(1, ww // 2), max(1, hh // 2)
    assert len(d) == 128 + size, (len(d), 128 + size)
    return "%dx%d %d-bit, %d mip(s)" % (w, h, bits, max(mips, 1))


def main(data, nif_mesh=None):
    ok = True
    for esp in sorted(glob.glob(os.path.join(data, "*.esp"))):
        r = subprocess.run([sys.executable, "-I", os.path.join(HERE, "verify_esp.py"), esp], capture_output=True, text=True)
        print(os.path.basename(esp), r.stdout.strip()); ok &= r.returncode == 0
    for nif in sorted(glob.glob(os.path.join(data, "Meshes", "**", "*.nif"), recursive=True)):
        try:
            types, strings, report = read_nif(nif)
            print(os.path.basename(nif), "OK:", len(types), "blocks", "; ".join(report))
        except Exception as ex:
            print(os.path.basename(nif), "FAILED:", repr(ex)); ok = False
        if nif_mesh:
            r = subprocess.run([sys.executable, "-I", nif_mesh, nif, os.devnull], capture_output=True, text=True)
            print("   nif_mesh.py:", (r.stdout.strip().splitlines() or ["(no output)"])[-1], "" if r.returncode == 0 else r.stderr.strip()[-300:])
            ok &= r.returncode == 0
    for dds in sorted(glob.glob(os.path.join(data, "Textures", "**", "*.dds"), recursive=True)):
        try:
            print(os.path.basename(dds), "OK:", read_dds(dds))
        except Exception as ex:
            print(os.path.basename(dds), "FAILED:", repr(ex)); ok = False
    print("ALL OK" if ok else "FAILURES")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None))
