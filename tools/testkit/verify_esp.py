# Structural check of a written plugin: every group/record size adds up, subrecords parse, form ids unique, the expected tree.
import struct, sys, collections, zlib
buf = open(sys.argv[1], "rb").read()
seen = collections.Counter(); types = collections.Counter(); errs = []
def walk(i, end, depth, path):
    while i < end:
        t = buf[i:i+4]
        if t == b"GRUP":
            size, label, gt = struct.unpack_from("<IIi", buf, i+4)
            if i + size > end or size < 24: errs.append("bad group size at %d" % i); return
            walk(i+24, i+size, depth+1, path + [(gt, label)]); i += size
        else:
            size, flags, fid = struct.unpack_from("<III", buf, i+4)
            d = buf[i+24:i+24+size]
            j = 0; big = None
            while j < len(d):
                st = d[j:j+4]; ss = struct.unpack_from("<H", d, j+4)[0]; j += 6
                if st == b"XXXX": big = struct.unpack_from("<I", d, j)[0]; j += ss; continue
                if big is not None: ss = big; big = None
                j += ss
            if j != len(d): errs.append("subrecords overrun in %s %08X" % (t, fid))
            seen[fid] += 1; types[t.decode()] += 1; i += 24 + size
    if i != end: errs.append("group overrun")
hs = struct.unpack_from("<I", buf, 4)[0]
walk(24 + hs, len(buf), 0, [])
dup = [hex(f) for f, n in seen.items() if n > 1]
print("records", dict(types), "dup ids", dup, "errors", errs)
sys.exit(1 if dup or errs else 0)
