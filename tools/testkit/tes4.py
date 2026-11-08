# Minimal TES4 (Fallout: New Vegas) plugin writer for the NVR test cell (2026-10-31).
# Writes uncompressed records and groups; form ids of the plugin's own records use load index 01 (one master: FalloutNV.esm).
import struct

VERSION = 15          # record header "form version" of FalloutNV.esm records
HEDR_VERSION = 1.34   # FNV plugin header version


def sr(tag, data):
    """One subrecord; > 65535 bytes go behind an XXXX size record."""
    if isinstance(data, str): data = data.encode("latin-1") + b"\0"
    if len(data) > 0xFFFF:
        return b"XXXX" + struct.pack("<HI", 4, len(data)) + tag.encode() + struct.pack("<H", 0) + data
    return tag.encode() + struct.pack("<H", len(data)) + data


def record(rtype, fid, subs, flags=0):
    body = b"".join(subs)
    return rtype.encode() + struct.pack("<IIIIHH", len(body), flags, fid, 0, VERSION, 0) + body


def group(label, gtype, content):
    if isinstance(label, str): label = label.encode()
    elif isinstance(label, int): label = struct.pack("<I" if label >= 0 else "<i", label)
    return b"GRUP" + struct.pack("<I", 24 + len(content)) + label + struct.pack("<iHHHH", gtype, 0, 0, 0, 0) + content


def header(nrec, next_id, author, desc, masters):
    subs = [sr("HEDR", struct.pack("<fiI", HEDR_VERSION, nrec, next_id)), sr("CNAM", author), sr("SNAM", desc)]
    for m in masters: subs += [sr("MAST", m), sr("DATA", struct.pack("<Q", 0))]
    return record("TES4", 0, subs)


def f32(*v): return struct.pack("<%df" % len(v), *v)
def u32(*v): return struct.pack("<%dI" % len(v), *v)
