# Small read-only FalloutNV.esm (TES4 format) reader for the NVR test cell builder (2026-10-31).
# Same walk as bench\deep-roadmap\evidence\hero\esm_refs.py, but it keeps every record (type, form id, flags, raw data) so the
# builder can look up base forms by editor ID, read OBND / LIGH / CELL data and copy a vanilla room's references.
# Game data is treated as data (struct unpacking only).
import struct, zlib, pickle, os

# (2026-11-07) the game's FalloutNV.esm: set FNV_ESM to its full path (default: Steam's usual folder); the index cache goes to
# FNV_ESM_CACHE or the temp folder (nothing is written into the repo)
import tempfile
ESM = os.environ.get("FNV_ESM") or r"C:\Program Files (x86)\Steam\steamapps\common\Fallout New Vegas\Data\FalloutNV.esm"
CACHE = os.environ.get("FNV_ESM_CACHE") or os.path.join(tempfile.gettempdir(), "vs-testkit-esm-index.pkl")


def subrecords(data):
    i, n, big = 0, len(data), None
    while i + 6 <= n:
        t = data[i:i + 4]; size = struct.unpack_from("<H", data, i + 4)[0]; i += 6
        if t == b"XXXX": big = struct.unpack_from("<I", data, i)[0]; i += size; continue
        if big is not None: size = big; big = None
        yield t.decode("latin-1"), data[i:i + size]; i += size


def sub(data, name):
    for t, d in subrecords(data):
        if t == name: return d
    return None


def zstr(d):
    return d.rstrip(b"\0").decode("latin-1") if d is not None else ""


class Rec:
    __slots__ = ("type", "fid", "flags", "off", "size", "cell", "world", "group")
    def __init__(self, t, fid, flags, off, size, cell, world, group):
        self.type, self.fid, self.flags, self.off, self.size, self.cell, self.world, self.group = t, fid, flags, off, size, cell, world, group


class Esm:
    def __init__(self, path=ESM):
        self.path = path
        self.buf = open(path, "rb").read()
        if os.path.isfile(CACHE) and os.path.getmtime(CACHE) > os.path.getmtime(path):
            self.recs, self.edid = pickle.load(open(CACHE, "rb"))
        else:
            self.recs, self.edid = {}, {}
            self._walk(0, len(self.buf), None, None, None)
            os.makedirs(os.path.dirname(CACHE), exist_ok=True)
            pickle.dump((self.recs, self.edid), open(CACHE, "wb"), protocol=4)
        self.by_edid = {v.lower(): k for k, v in self.edid.items()}
        self.children = {}
        for r in self.recs.values():
            if r.cell is not None and r.type in ("REFR", "ACHR", "ACRE", "PGRE", "NAVM", "LAND", "PMIS"):
                self.children.setdefault(r.cell, []).append(r.fid)

    def _walk(self, start, end, cell, world, group):
        buf, i = self.buf, start
        while i < end:
            t = buf[i:i + 4]
            if t == b"GRUP":
                size, label, gtype = struct.unpack_from("<IIi", buf, i + 4)
                c, w = cell, world
                if gtype == 1: w = label
                elif gtype in (6, 8, 9, 10): c = label
                self._walk(i + 24, i + size, c, w, gtype)
                i += size
            else:
                size, flags, fid = struct.unpack_from("<III", buf, i + 4)
                ty = t.decode("latin-1")
                rc = fid if ty == "CELL" else cell
                self.recs[fid] = Rec(ty, fid, flags, i, size, None if ty == "CELL" else cell, world, group)
                if ty not in ("REFR", "ACHR", "ACRE", "NAVM", "LAND", "PGRE", "INFO"):
                    d = self.data(self.recs[fid])
                    e = sub(d, "EDID")
                    if e: self.edid[fid] = zstr(e)
                i += 24 + size

    def data(self, r):
        if isinstance(r, int): r = self.recs[r]
        d = self.buf[r.off + 24:r.off + 24 + r.size]
        if r.flags & 0x00040000: d = zlib.decompress(d[4:])
        return d

    def header(self, r):
        if isinstance(r, int): r = self.recs[r]
        return self.buf[r.off:r.off + 24]

    def fid(self, edid):
        return self.by_edid.get(edid.lower())

    def ofType(self, ty):
        return [r for r in self.recs.values() if r.type == ty]

    def obnd(self, fid):
        d = sub(self.data(fid), "OBND")
        return struct.unpack("<6h", d) if d else None

    def ref_info(self, fid):
        d = self.data(fid)
        base = sub(d, "NAME"); pos = sub(d, "DATA")
        return (struct.unpack("<I", base)[0] if base else None, struct.unpack("<6f", pos) if pos else None)
