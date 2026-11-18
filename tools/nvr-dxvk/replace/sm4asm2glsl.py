"""sm4asm2glsl.py: an NVR pixel shader compiled WITHOUT ps_3_0's limits (fxc /T ps_4_0 /Gec /Lx disassembly) -> a GLSL replacement
for the NVR DXVK fork, standing in for the ps_3_0 shader the game loads (2026-10-26, docs/next/FORK-1026-LIMITS.md).

  python sm4asm2glsl.py <shader_sm4.asm> <original_ps30.asm> <out.frag> <out.txt>

Why: fxc refuses ps_3_0 shaders past 32 temporary registers (X4505) and the native driver ~3,600 instruction slots, so NVR's
HLSL carries cuts (marked NVR_NO_SM3_LIMITS). The same source compiled as ps_4_0 in backwards-compatibility mode (/Gec: tex2D,
sampler2D and register(cN) still work) has no such limits; this script turns that compiled shader, instruction for instruction,
into GLSL under the fork's replacement contract (src/core/NvrDxvkExt.h, NVR_EXT_SHADER_REPLACEMENT_DESC), the same contract
psasm2glsl.py writes for ps_3_0:
  - textures: the D3D9 sampler register of each texture comes from the ORIGINAL ps_3_0 shader's constant table (by name), so the
    replacement reads exactly the slots the game and NVR bind; its sampler from DXVK's heap (push offset 32); a slot with no
    texture (DXVK's spec type 3) reads (0, 0, 0, 1) as on D3D9.
  - float constants: $Globals members map to the original's registers by name (register(cN) globals land at slot N in /Gec
    anyway; others are remapped); the replacement's block lists every register of each member it reads (floats= in the .txt).
  - inputs by D3D9 semantic at DXVK's fixed locations (TEXCOORDn 1+n, COLORn 9+n, NORMAL 0, FOG 11), SV_Position = D3D9's VPOS
    source (the HLSL subtracts 0.5 itself, NVR_SM4), the alpha test from DXVK's spec data.
  - D3D9 arithmetic as DXVK runs a ps_3_0 shader (float emulation "Enabled"): log2 and rsq of |x|, rcp / rsq / log results
    clamped to +-FLT_MAX, sqrt of |x|, a / b as a * rcp(b). So 0 * inf stays 0 and pow(0, 0) stays 1, as in the original.
Registers are kept as uvec4 (SM4 mixes float, int and mask values in one register); every float operation converts its operands.
Not handled (stops with a message): oDepth, bool/int constants from the game, shadow-compare samples, gather, UAVs, subroutines.
"""
import re, struct, sys

FLT_MAX = "3.4028234663852886e38"
COMP = "xyzw"


class Err(Exception): pass


def full_swizzle(sw):
    if not sw: return "xyzw"
    return (sw + sw[-1] * 4)[:4]


def split_operands(rest):
    out, depth, cur = [], 0, ""
    for ch in rest:
        if ch in "([": depth += 1
        elif ch in ")]": depth -= 1
        if ch == "," and depth == 0: out.append(cur.strip()); cur = ""
        else: cur += ch
    if cur.strip(): out.append(cur.strip())
    return out


def lit_bits(s):
    s = s.strip()
    if s.lower().startswith("0x") or s.lower().startswith("-0x"):
        v = int(s, 16)
    elif re.fullmatch(r"-?\d+", s):
        v = int(s)
    else:
        v = struct.unpack("<I", struct.pack("<f", float(s)))[0]
    return v & 0xFFFFFFFF


def float_lit(bits):
    f = struct.unpack("<f", struct.pack("<I", bits))[0]
    if f != f or f in (float("inf"), float("-inf")): return "uintBitsToFloat(%du)" % bits
    r = "%.9g" % f
    if "e" not in r and "." not in r: r += ".0"
    return r


# ---------------------------------------------------------------- the original ps_3_0 shader's constant table
def parse_d3d9_ct(path):
    """name -> (register set letter, index, size) from fxc /dumpbin's listing of the original."""
    regs = {}
    in_regs = False
    for raw in open(path, encoding="utf-8", errors="replace"):
        line = raw.rstrip()
        if re.match(r"//\s+Registers:", line): in_regs = True; continue
        if in_regs:
            m = re.match(r"//\s+(\S+)\s+([csbi])(\d+)\s+(\d+)\s*$", line)
            if m: regs[m.group(1)] = (m.group(2), int(m.group(3)), int(m.group(4)))
            elif re.match(r"//\s*$", line) or line.startswith("//   ----") or "Name" in line: continue
            elif not line.startswith("//"): break
    return regs


# ---------------------------------------------------------------- the SM4 listing
class Shader:
    def __init__(self, path):
        self.members = []        # (name, offset, size, used)
        self.resources = []      # (name, type, dim, bind letter, bind index, count)
        self.isig = []           # (semantic, index, mask, register, sysvalue)
        self.body = []
        section = None
        lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
        i = 0
        while i < len(lines):
            line = lines[i]
            if re.match(r"^(ps|vs)_[45]_[01]\b", line.strip()):
                self.version = line.strip()
                self.body = lines[i + 1:]
                break
            if "cbuffer $Globals" in line or re.match(r"//\s*cbuffer\s", line): section = "cb"
            elif "Resource Bindings:" in line: section = "res"
            elif "Input signature:" in line: section = "in"
            elif "Output signature:" in line: section = "out"
            if section == "cb":
                m = re.match(r"//\s+(.*?)\s+([A-Za-z_]\w*)(\[\d+\])?;\s*// Offset:\s*(\d+)\s+Size:\s*(\d+)(.*)$", line)
                if m: self.members.append((m.group(2), int(m.group(4)), int(m.group(5)), "[unused]" not in m.group(6), m.group(1)))
            elif section == "res":
                m = re.match(r"//\s+(\S+)\s+(sampler|texture|cbuffer|tbuffer|UAV\S*)\s+(\S+)\s+(\S+)\s+([a-z]+)(\d+)\s+(\d+)", line)
                if m: self.resources.append((m.group(1), m.group(2), m.group(4), m.group(5), int(m.group(6)), int(m.group(7))))
            elif section == "in":
                m = re.match(r"//\s+(\w+)\s+(\d+)\s+([xyzw]+)\s+(\d+)\s+(\w+)\s+(\w+)", line)
                if m: self.isig.append((m.group(1).upper(), int(m.group(2)), m.group(3), int(m.group(4)), m.group(5).upper()))
            i += 1
        else:
            raise Err("no ps_4_0 / ps_5_0 line in " + path)
        if not self.version.startswith("ps_"): raise Err("not a pixel shader: " + self.version)


class Translator:
    def __init__(self, sh, ct):
        self.sh, self.ct = sh, ct
        self.out = []
        self.depth = 1
        self.temps = 0
        self.indexable = {}      # x index -> count
        self.tex_dims = {}       # SM4 t index -> "2d" / "cube" / "3d"
        self.used_tex = {}       # D3D9 slot -> dim
        self.used_regs = set()   # D3D9 float registers to upload
        self.inputs = {}         # v register -> list of (component, glsl expr) or special
        self.input_decls = {}    # glsl name -> (location, qualifier)
        self.special = {}        # v register -> "position" / "front"
        self.icb = None
        self.has_output = set()
        self.slot_map = self.build_slot_map()
        self.tex_map = self.build_tex_map()

    # ---- constants: SM4 slot -> (D3D9 register base, member slot base, member slot count)
    def build_slot_map(self):
        m = {}
        self.missing = []
        self.bool_map = {}       # (2026-11-14) $Globals slot -> D3D9 bool register
        for name, off, size, used, typ in self.sh.members:
            if off % 16 and "bool" in typ and used and size == 4:
                # (2026-11-14) a bool packed inside a register: component (off % 16) / 4 of slot off // 16
                if name not in self.ct or self.ct[name][0] != "b": raise Err("bool constant %s is not a bool register of the original" % name)
                self.bool_map[(off // 16, (off % 16) // 4)] = self.ct[name][1]
                continue
            if off % 16:
                if used: raise Err("$Globals member %s starts inside a register (offset %d): not handled" % (name, off))
                continue
            base, count = off // 16, max(1, (size + 15) // 16)
            # (2026-11-14 rt-shadows) a D3D9 bool constant (bN in the original): DXVK hands the pixel shader's bools over as the spec data's
            # psBools word, bit N (masked by the bools the original reads: one it never read is 0 here too)
            if "bool" in typ:
                if not used: continue
                if name not in self.ct or self.ct[name][0] != "b": raise Err("bool constant %s is not a bool register of the original" % name)
                if count != 1: raise Err("bool array %s: not handled" % name)
                self.bool_map[(base, 0)] = self.ct[name][1]
                continue
            if typ.startswith("int") and used: raise Err("int constant %s: not handled" % name)
            reg = None
            if name in self.ct:
                kind, idx, cnt = self.ct[name]
                if kind != "c":
                    if used: raise Err("%s is %s%d in the original, not a float register" % (name, kind, idx))
                    continue
                reg = idx
            elif used:
                self.missing.append(name)
                reg = base   # register(cN) globals sit at slot N: the best guess; reported
            if reg is None: continue
            for k in range(count): m[base + k] = (reg, base, count, name)
        return m

    def build_tex_map(self):
        """SM4 texture index -> D3D9 sampler register (from the original's table, by name and array element)."""
        tm = {}
        for name, typ, dim, letter, idx, count in self.sh.resources:
            if typ != "texture": continue
            if name not in self.ct or self.ct[name][0] != "s":
                # the original never read this texture (it was cut away there): a new slot would lie to DXVK's analysis
                raise Err("texture %s is not a sampler of the original ps_3_0 shader" % name)
            base = self.ct[name][1]
            for k in range(count): tm[idx + k] = base + k
        return tm

    # ---- output helpers
    def emit(self, s): self.out.append("  " * self.depth + s)

    def c_expr(self, slot_tok, rel=None):
        """cb0[...] -> c[...] (vec4 expression)."""
        k = int(slot_tok)
        if k not in self.slot_map: raise Err("constant slot %d is not a known $Globals member" % k)
        reg, base, count, name = self.slot_map[k]
        for r in range(reg, reg + count): self.used_regs.add(r)
        if rel is None: return "C(%d)" % (reg + k - base)
        return "c[CIDX(%d) + %s]" % (reg + k - base, rel)

    def rel_index(self, inner):
        """'r1.x + 39' / 'r1.x' / '39' -> (glsl int expr or None, constant part)."""
        inner = inner.strip()
        m = re.fullmatch(r"(?:([rx]\d+(?:\[[^\]]+\])?\.[xyzw]))?\s*(?:\+\s*)?(-?\d+)?", inner)
        if not m or (m.group(1) is None and m.group(2) is None): raise Err("index not understood: " + inner)
        reg = m.group(1)
        const = int(m.group(2)) if m.group(2) else 0
        return ("int(%s)" % self.reg_bits(reg)) if reg else None, const

    def reg_bits(self, tok):
        """A one-component register reference as a uint expression (for indices)."""
        m = re.fullmatch(r"r(\d+)\.([xyzw])", tok)
        if m: return "r%s.%s" % (m.group(1), m.group(2))
        raise Err("index register not handled: " + tok)

    def src(self, tok, ty):
        """A source operand, swizzled to four components, as vec4 ('f'), ivec4 ('i') or uvec4 ('u')."""
        tok = tok.strip()
        neg = False
        if tok.startswith("-"): neg = True; tok = tok[1:].strip()
        absm = False
        if tok.startswith("|") and tok.endswith("|"): absm = True; tok = tok[1:-1].strip()
        # literal
        m = re.fullmatch(r"l\((.*)\)", tok)
        if m:
            vals = [lit_bits(v) for v in m.group(1).split(",")]
            if len(vals) == 1: vals = vals * 4
            if ty == "f": e = "vec4(%s)" % ", ".join(float_lit(v) for v in vals)
            elif ty == "i": e = "ivec4(%s)" % ", ".join(str(struct.unpack("<i", struct.pack("<I", v))[0]) for v in vals)
            else: e = "uvec4(%s)" % ", ".join("%du" % v for v in vals)
            return self.mods(e, ty, neg, absm)
        m = re.fullmatch(r"(r|v|o)(\d+)(?:\.([xyzw]+))?", tok)
        x = re.fullmatch(r"x(\d+)\[([^\]]+)\](?:\.([xyzw]+))?", tok)
        cb = re.fullmatch(r"cb0\[([^\]]+)\](?:\.([xyzw]+))?", tok)
        icb = re.fullmatch(r"icb\[([^\]]+)\](?:\.([xyzw]+))?", tok)
        if m:
            kind, n, sw = m.group(1), int(m.group(2)), full_swizzle(m.group(3))
            if kind == "v":
                if n in self.special:
                    base = self.special[n]          # already a uvec4 expression
                elif n in self.inputs: base = "floatBitsToUint(v%d)" % n
                else: raise Err("input v%d used but not declared" % n)
            elif kind == "o": base = "o%d" % n
            else: base = "r%d" % n
            e = "%s.%s" % (base, sw)
            return self.mods(self.conv_u(e, ty), ty, neg, absm)
        if x:
            n = int(x.group(1)); rel, const = self.rel_index(x.group(2))
            idx = "%s + %d" % (rel, const) if rel else str(const)
            e = "x%d[%s].%s" % (n, idx, full_swizzle(x.group(3)))
            return self.mods(self.conv_u(e, ty), ty, neg, absm)
        if cb and re.fullmatch(r"\d+", cb.group(1).strip()) and any(k[0] == int(cb.group(1)) for k in self.bool_map):
            slot = int(cb.group(1))
            comps = []
            for ch in full_swizzle(cb.group(2)):
                b = self.bool_map.get((slot, "xyzw".index(ch)))
                if b is None: raise Err("cb0[%d].%s mixes a bool with another member: not handled" % (slot, ch))
                comps.append("(bitfieldExtract(specPsBools, %d, 1) != 0u ? 0xFFFFFFFFu : 0u)" % b)
            e = "uvec4(%s)" % ", ".join(comps)
            return self.mods(self.conv_u(e, ty), ty, neg, absm)
        if cb:
            rel, const = self.rel_index(cb.group(1))
            e = "%s.%s" % (self.c_expr(const, rel), full_swizzle(cb.group(2)))
            if ty == "f": pass
            elif ty == "i": e = "floatBitsToInt(%s)" % e
            else: e = "floatBitsToUint(%s)" % e
            return self.mods(e, ty, neg, absm)
        if icb:
            if self.icb is None: raise Err("icb used without dcl_immediateConstantBuffer")
            rel, const = self.rel_index(icb.group(1))
            idx = "%s + %d" % (rel, const) if rel else str(const)
            e = "ICB[%s].%s" % (idx, full_swizzle(icb.group(2)))
            return self.mods(self.conv_u(e, ty), ty, neg, absm)
        raise Err("source operand not understood: " + tok)

    @staticmethod
    def conv_u(e, ty):
        if ty == "f": return "uintBitsToFloat(%s)" % e
        if ty == "i": return "ivec4(%s)" % e
        return e

    @staticmethod
    def mods(e, ty, neg, absm):
        if absm: e = "abs(%s)" % e
        if neg: e = "(-%s)" % e
        return e

    def dst(self, tok):
        tok = tok.strip()
        if tok == "null": return None, None
        m = re.fullmatch(r"(r|o)(\d+)(?:\.([xyzw]+))?", tok)
        x = re.fullmatch(r"x(\d+)\[([^\]]+)\](?:\.([xyzw]+))?", tok)
        if m:
            kind, n, mask = m.group(1), int(m.group(2)), m.group(3) or "xyzw"
            if kind == "o": self.has_output.add(n)
            return "%s%d" % (kind, n), mask
        if x:
            n = int(x.group(1)); rel, const = self.rel_index(x.group(2))
            idx = "%s + %d" % (rel, const) if rel else str(const)
            return "x%d[%s]" % (n, idx), x.group(3) or "xyzw"
        if tok.startswith("oDepth"): raise Err("oDepth is not handled")
        raise Err("destination not understood: " + tok)

    def assign(self, dtok, expr4, ty, sat=False, scalar=False):
        name, mask = self.dst(dtok)
        if name is None: return
        if sat:
            if ty != "f": raise Err("_sat on a non-float result")
            expr4 = "clamp(%s, 0.0, 1.0)" % expr4
        n = len(mask)
        if scalar: sel = expr4 if n == 1 else "%s(%s)" % ({"f": "vec", "i": "ivec", "u": "uvec"}[ty] + str(n), expr4)
        else: sel = "(%s).%s" % (expr4, mask)
        if ty == "f": sel = "floatBitsToUint(%s)" % sel
        elif ty == "i": sel = ("uint(%s)" if n == 1 else "uvec%d(%%s)" % n) % sel
        self.emit("%s.%s = %s;" % (name, mask, sel))

    # ---- declarations
    def dcl(self, op, args, line):
        if op == "dcl_temps": self.temps = int(args[0]); return
        if op == "dcl_indexableTemp":
            m = re.fullmatch(r"x(\d+)\[(\d+)\]", args[0].strip()); self.indexable[int(m.group(1))] = int(m.group(2)); return
        if op.startswith("dcl_resource_texture"):
            dim = op[len("dcl_resource_texture"):]
            t = int(re.search(r"t(\d+)", line).group(1))
            if dim not in ("2d", "cube", "3d"): raise Err("texture kind not handled: " + op)
            self.tex_dims[t] = dim; return
        if op in ("dcl_sampler", "dcl_constantbuffer", "dcl_globalFlags", "dcl_output"):
            if op == "dcl_output" and not args[0].startswith("o"): raise Err("output not handled: " + line)
            return
        if op in ("dcl_input_ps", "dcl_input_ps_siv", "dcl_input_ps_sgv"):
            # "dcl_input_ps linear centroid v1.xyz" / "dcl_input_ps_siv linear noperspective v11.xy, position"
            words = line.split(None, 1)[1].replace(",", " ").split()
            regtok = [w for w in words if re.fullmatch(r"v\d+(\.[xyzw]+)?", w)][0]
            n = int(re.match(r"v(\d+)", regtok).group(1))
            quals = [w for w in words if w in ("constant", "linear", "centroid", "noperspective", "sample")]
            sysv = words[-1] if op != "dcl_input_ps" else None
            if sysv == "position": self.special[n] = "floatBitsToUint(vec4(gl_FragCoord.xyz, 1.0 / gl_FragCoord.w))"; return
            if sysv == "is_front_face": self.special[n] = "uvec4(gl_FrontFacing ? 0xFFFFFFFFu : 0u)"; return
            if sysv: raise Err("system value input not handled: " + line)
            self.declare_input(n, quals)
            return
        if op == "dcl_input": raise Err("input not handled: " + line)
        raise Err("declaration not handled: " + line)

    def declare_input(self, n, quals):
        rows = [r for r in self.sh.isig if r[3] == n and r[4] == "NONE"]
        if not rows: raise Err("v%d has no input signature row" % n)
        comps = ["0.0"] * 4
        for sem, idx, mask, reg, sysv in rows:
            loc, name = self.location(sem, idx)
            q = ("centroid " if "centroid" in quals else "") + ("noperspective " if "noperspective" in quals else "") + \
                ("flat " if "constant" in quals else "")
            self.input_decls[name] = (loc, q)
            first = COMP.index(mask[0])
            for c in mask: comps[COMP.index(c)] = "%s.%s" % (name, COMP[COMP.index(c) - first])
        self.inputs[n] = "vec4(%s)" % ", ".join(comps)

    @staticmethod
    def location(sem, idx):
        # DXVK (dxbc-spirv sm3_io_map.cpp s_ffLocations): only NORMAL0, TEXCOORD0-7, COLOR0-1 and FOG have fixed locations; any
        # other semantic (TEXCOORD8, ...) gets the next free location in the shader's own declaration order, which a
        # replacement cannot know: such a shader stays as the game's ps_3_0 shader.
        if sem == "TEXCOORD" and idx <= 7: return 1 + idx, "texcoord%d" % idx
        if sem == "COLOR" and idx <= 1: return 9 + idx, "color%d" % idx
        if sem == "NORMAL" and idx == 0: return 0, "normal0"
        if sem == "FOG": return 11, "fog%d" % idx
        # (2026-11-14 rt-shadows) TEXCOORD8 / TEXCOORD9: no fixed place, numbered 12, 13 by the VERTEX shader's declaration order (a
        # replacement is linked to it by location, DXVK's semantic linking needs both stages compiled by DXVK). NVR's object and
        # parallax vertex shaders declare TEXCOORD8 before TEXCOORD9 and no other non-fixed output (ParallaxTemplate.hlsl's layout
        # note), so 12 and 13. A vertex shader that writes TEXCOORD9 alone would break this: none of NVR's do.
        if sem == "TEXCOORD" and idx in (8, 9): return 12 + (idx - 8), "texcoord%d" % idx
        raise Err("input semantic %s%d has no fixed location in DXVK's D3D9 linkage" % (sem, idx))

    # ---- instructions
    FLOAT2 = {"add": "%s + %s", "mul": "%s * %s", "div": "divE(%s, %s)", "min": "min(%s, %s)", "max": "max(%s, %s)"}
    FLOAT1 = {"frc": "fract(%s)", "round_ne": "roundEven(%s)", "round_ni": "floor(%s)", "round_pi": "ceil(%s)",
              "round_z": "trunc(%s)", "exp": "exp2(%s)", "log": "logE(%s)", "sqrt": "sqrt(abs(%s))", "rsq": "rsqE(%s)",
              "rcp": "rcpE(%s)", "deriv_rtx": "dFdx(%s)", "deriv_rty": "dFdy(%s)", "deriv_rtx_coarse": "dFdxCoarse(%s)",
              "deriv_rty_coarse": "dFdyCoarse(%s)", "deriv_rtx_fine": "dFdxFine(%s)", "deriv_rty_fine": "dFdyFine(%s)"}
    FCMP = {"lt": "lessThan", "ge": "greaterThanEqual", "eq": "equal", "ne": "notEqual"}
    ICMP = {"ilt": ("i", "lessThan"), "ige": ("i", "greaterThanEqual"), "ieq": ("i", "equal"), "ine": ("i", "notEqual"),
            "ult": ("u", "lessThan"), "uge": ("u", "greaterThanEqual")}
    INT2 = {"iadd": ("i", "%s + %s"), "and": ("u", "%s & %s"), "or": ("u", "%s | %s"), "xor": ("u", "%s ^ %s"),
            "ishl": ("i", "%s << (%s & 31)"), "ishr": ("i", "%s >> (%s & 31)"), "ushr": ("u", "%s >> (%s & 31u)"),
            "imin": ("i", "min(%s, %s)"), "imax": ("i", "max(%s, %s)"), "umin": ("u", "min(%s, %s)"), "umax": ("u", "max(%s, %s)")}

    def instr(self, line):
        line = line.strip()
        if not line or line.startswith("//"): return
        op, _, rest = line.partition(" ")
        args = split_operands(rest) if rest.strip() else []
        sat = op.endswith("_sat")
        if sat: op = op[:-4]
        S = self.src
        if op.startswith("dcl_immediateConstantBuffer"): return   # handled in run()
        if op.startswith("dcl_"): return self.dcl(op, args, line)
        if op in self.FLOAT2: return self.assign(args[0], self.FLOAT2[op] % (S(args[1], "f"), S(args[2], "f")), "f", sat)
        if op in self.FLOAT1: return self.assign(args[0], self.FLOAT1[op] % S(args[1], "f"), "f", sat)
        if op == "mad": return self.assign(args[0], "%s * %s + %s" % (S(args[1], "f"), S(args[2], "f"), S(args[3], "f")), "f", sat)
        if op == "mov":
            if sat: return self.assign(args[0], S(args[1], "f"), "f", True)
            return self.assign(args[0], S(args[1], "u"), "u")
        if op == "movc":
            if sat:   # (2026-11-14) the float result saturated
                return self.assign(args[0], "mix(%s, %s, notEqual(%s, uvec4(0u)))" % (S(args[3], "f"), S(args[2], "f"), S(args[1], "u")), "f", True)
            return self.assign(args[0], "mix(%s, %s, notEqual(%s, uvec4(0u)))" % (S(args[3], "u"), S(args[2], "u"), S(args[1], "u")), "u")
        if op in ("dp2", "dp3", "dp4"):
            n = int(op[2]); sw = "xyzw"[:n]
            return self.assign(args[0], "dot((%s).%s, (%s).%s)" % (S(args[1], "f"), sw, S(args[2], "f"), sw), "f", sat, scalar=True)
        if op in self.FCMP: return self.assign(args[0], "M(%s(%s, %s))" % (self.FCMP[op], S(args[1], "f"), S(args[2], "f")), "u")
        if op in self.ICMP:
            ty, fn = self.ICMP[op]; return self.assign(args[0], "M(%s(%s, %s))" % (fn, S(args[1], ty), S(args[2], ty)), "u")
        if op in self.INT2:
            ty, f = self.INT2[op]
            b = S(args[2], ty)
            if op in ("ishl", "ishr"): b = S(args[2], "i")
            if op == "ushr": b = S(args[2], "u")
            return self.assign(args[0], f % (S(args[1], ty), b), ty)
        if op == "imad": return self.assign(args[0], "%s * %s + %s" % (S(args[1], "i"), S(args[2], "i"), S(args[3], "i")), "i")
        if op == "imul":
            if args[0] != "null": raise Err("imul with a high result")
            return self.assign(args[1], "%s * %s" % (S(args[2], "i"), S(args[3], "i")), "i")
        if op == "udiv":
            a, b = S(args[2], "u"), S(args[3], "u")
            if args[0] != "null": self.assign(args[0], "%s / %s" % (a, b), "u")
            if args[1] != "null": self.assign(args[1], "%s %% %s" % (a, b), "u")
            return
        if op == "ineg": return self.assign(args[0], "-%s" % S(args[1], "i"), "i")
        if op == "not": return self.assign(args[0], "~%s" % S(args[1], "u"), "u")
        if op == "ftoi": return self.assign(args[0], "ivec4(%s)" % S(args[1], "f"), "i")
        if op == "ftou": return self.assign(args[0], "uvec4(max(%s, vec4(0.0)))" % S(args[1], "f"), "u")
        if op == "itof": return self.assign(args[0], "vec4(%s)" % S(args[1], "i"), "f", sat)
        if op == "utof": return self.assign(args[0], "vec4(%s)" % S(args[1], "u"), "f", sat)
        if op == "sincos":
            x = S(args[2], "f")
            if args[0] != "null": self.assign(args[0], "sin(%s)" % x, "f", sat)
            if args[1] != "null": self.assign(args[1], "cos(%s)" % x, "f", sat)
            return
        if op.startswith("sample") or op == "ld":
            return self.sample(op, args, sat)
        # flow control
        if op in ("if_nz", "if_z"):
            self.emit("if (%s.x %s 0u) {" % (S(args[0], "u"), "!=" if op == "if_nz" else "=="))
            self.depth += 1; return
        if op == "else": self.depth -= 1; self.emit("} else {"); self.depth += 1; return
        if op in ("endif", "endloop", "endswitch"): self.depth -= 1; self.emit("}"); return
        if op == "loop": self.emit("while (true) {"); self.depth += 1; return
        if op == "break": self.emit("break;"); return
        if op == "continue": self.emit("continue;"); return
        if op in ("breakc_nz", "breakc_z", "continuec_nz", "continuec_z", "discard_nz", "discard_z", "retc_nz", "retc_z"):
            what = {"breakc": "break;", "continuec": "continue;", "discard": "discard;", "retc": "{ finish(); return; }"}[op.rsplit("_", 1)[0]]
            self.emit("if (%s.x %s 0u) %s" % (S(args[0], "u"), "!=" if op.endswith("_nz") else "==", what)); return
        if op == "ret":
            self.emit("finish(); return;" if self.depth > 1 else "finish();"); return
        if op == "switch": self.emit("switch (int(%s.x)) {" % S(args[0], "u")); self.depth += 1; return
        if op == "case": self.emit("case %d:" % struct.unpack("<i", struct.pack("<I", lit_bits(re.fullmatch(r"l\((.*)\)", args[0]).group(1))))[0]); return
        if op == "default": self.emit("default:"); return
        if op == "nop": return
        raise Err("instruction not handled: " + line)

    def sample(self, op, args, sat):
        if op not in ("sample", "sample_l", "sample_d", "sample_b", "ld"): raise Err(op + " is not handled")
        t = re.fullmatch(r"t(\d+)(?:\.([xyzw]+))?", args[2].strip())
        if not t: raise Err("resource operand not understood: " + args[2])
        ti = int(t.group(1))
        if ti not in self.tex_map: raise Err("t%d has no D3D9 slot" % ti)
        slot = self.tex_map[ti]
        dim = self.tex_dims.get(ti)
        if dim is None: raise Err("t%d not declared" % ti)
        prev = self.used_tex.get(slot)
        if prev and prev != dim: raise Err("D3D9 slot %d read as %s and %s" % (slot, prev, dim))
        self.used_tex[slot] = dim
        n = {"2d": 2, "cube": 3, "3d": 3}[dim]
        co = "(%s).%s" % (self.src(args[1], "f" if op != "ld" else "i"), "xyz"[:n])
        if op == "sample": call = "texture(SMP%d, %s)" % (slot, co)
        elif op == "sample_l": call = "textureLod(SMP%d, %s, (%s).x)" % (slot, co, self.src(args[4], "f"))
        elif op == "sample_b": call = "texture(SMP%d, %s, (%s).x)" % (slot, co, self.src(args[4], "f"))
        elif op == "sample_d":
            call = "textureGrad(SMP%d, %s, (%s).%s, (%s).%s)" % (slot, co, self.src(args[4], "f"), "xyz"[:n], self.src(args[5], "f"), "xyz"[:n])
        else:
            if dim != "2d": raise Err("ld on a non-2D texture")
            call = "texelFetch(T%d, %s, (%s).w)" % (slot, co, self.src(args[1], "i"))
        call = "(isNull(%du) ? vec4(0.0, 0.0, 0.0, 1.0) : %s)" % (slot, call)
        if t.group(2): call = "(%s).%s" % (call, full_swizzle(t.group(2)))
        self.assign(args[0], call, "f", sat)

    # ---- the whole shader
    def run(self):
        body = self.sh.body
        i = 0
        while i < len(body):
            line = body[i].strip()
            if line.startswith("dcl_immediateConstantBuffer"):
                text = line
                while "}" not in text.replace("{", "", 1).rsplit("}", 1)[0] + text[-1:] and not text.rstrip().endswith("}"):
                    i += 1; text += " " + body[i].strip()
                rows = re.findall(r"\{([^{}]*)\}", text[text.index("{") + 1:])
                self.icb = [[lit_bits(v) for v in r.split(",")] for r in rows]
            else:
                self.instr(line)
            i += 1
        if self.missing: print("WARNING: not in the original's constant table (assumed at their /Gec slot): " + ", ".join(self.missing))
        return self.glsl()

    def glsl(self):
        regs = sorted(self.used_regs)
        index = {r: k for k, r in enumerate(regs)}
        body = "\n".join(self.out)
        body = re.sub(r"C\((\d+)\)", lambda m: "c[%d]" % index[int(m.group(1))], body)
        body = re.sub(r"CIDX\((\d+)\)", lambda m: "%d" % index[int(m.group(1))], body)
        o = ["#version 450",
             "// Generated by tools/nvr-dxvk/replace/sm4asm2glsl.py from an NVR shader compiled without ps_3_0's limits (ps_4_0 /Gec).",
             "#extension GL_EXT_nonuniform_qualifier : enable",
             "#extension GL_EXT_scalar_block_layout : enable",
             "",
             "layout(set = 0, binding = 0) uniform sampler s_samplers[];"]
        for s, dim in sorted(self.used_tex.items()):
            o.append("layout(set = 1, binding = %d) uniform %s T%d;" % (s, {"2d": "texture2D", "cube": "textureCube", "3d": "texture3D"}[dim], s))
        if regs: o.append("layout(set = 2, binding = 6, std140) uniform Consts { vec4 c[%d]; };" % len(regs))
        o += ["// D3D9's spec data: DXVK turns each member into a specialization constant, so they must be plain uint members",
              "layout(set = 3, binding = 0, scalar) uniform SpecConsts {",
              "  uint specAlphaTest; uint specFog; uint specFixedFunction; uint specVsSamplers;",
              "  uint specPsSamplerTypes; uint specPsSamplerModes; uint specPsBools; uint specReserved;",
              "};",
              "layout(push_constant) uniform Push {",
              "  uint packedFogColorAndAlphaRef; float fogDistanceScale; float fogDistanceEnd; float fogDensity;",
              "  layout(offset = 32) uint d3d9Samplers[8];",
              "};"]
        for name, (loc, q) in sorted(self.input_decls.items(), key=lambda kv: kv[1][0]):
            o.append("layout(location = %d) %sin vec4 %s;" % (loc, q, name))
        for n in sorted(self.has_output):
            o.append("layout(location = %d) out vec4 oC%d;" % (n, n))
        o += ["",
              "const float FLT_MAX = %s;" % FLT_MAX,
              "float rcpE(float x) { return min(1.0 / x, FLT_MAX); }",
              "vec4 rcpE(vec4 x) { return min(1.0 / x, vec4(FLT_MAX)); }",
              "vec4 rsqE(vec4 x) { return min(inversesqrt(abs(x)), vec4(FLT_MAX)); }",
              "vec4 logE(vec4 x) { return max(log2(abs(x)), vec4(-FLT_MAX)); }",
              "vec4 divE(vec4 a, vec4 b) { return a * rcpE(b); }",
              "uvec4 M(bvec4 b) { return uvec4(b) * 0xFFFFFFFFu; }",
              "bool isNull(uint s) { return bitfieldExtract(specPsSamplerTypes, int(s + s), 2) == 3u; }",
              "uint smp(uint s) { return bitfieldExtract(d3d9Samplers[s >> 1], int((s & 1u) * 16u), 16); }"]
        for s, dim in sorted(self.used_tex.items()):
            ctor = {"2d": "sampler2D", "cube": "samplerCube", "3d": "sampler3D"}[dim]
            o.append("#define SMP%d %s(T%d, s_samplers[smp(%du)])" % (s, ctor, s, s))
        if self.icb:
            o.append("const uvec4 ICB[%d] = uvec4[%d](%s);" % (len(self.icb), len(self.icb),
                     ", ".join("uvec4(%s)" % ", ".join("%du" % v for v in (r + [0] * 4)[:4]) for r in self.icb)))
        o += ["",
              "bool alphaTest(float alpha) {   // DXVK 3.0.1 d3d9_fixed_function_frag.glsl, alphaTest()",
              "  uint compareOp = bitfieldExtract(specAlphaTest, 0, 3);",
              "  uint precisionBits = bitfieldExtract(specAlphaTest, 4, 4);",
              "  uint alphaRefInitial = bitfieldExtract(packedFogColorAndAlphaRef, 24, 8);",
              "  float alphaRef;",
              "  if (compareOp < 7u) {",
              "    if (precisionBits <= 8u) {",
              "      alphaRef = float((alphaRefInitial << precisionBits) | (alphaRefInitial >> (8u - precisionBits)));",
              "      alpha = roundEven(alpha * float((256u << precisionBits) - 1u));",
              "    } else alphaRef = float(alphaRefInitial) / 255.0;",
              "    switch (compareOp) {",
              "      case 1u: return alpha <  alphaRef;",
              "      case 2u: return alpha == alphaRef;",
              "      case 3u: return alpha <= alphaRef;",
              "      case 4u: return alpha >  alphaRef;",
              "      case 5u: return alpha != alphaRef;",
              "      case 6u: return alpha >= alphaRef;",
              "      default: return false;",
              "    }",
              "  }",
              "  return true;",
              "}",
              ""]
        for n in sorted(self.has_output): o.append("uvec4 o%d = uvec4(0u);" % n)
        o.append("void finish() {")
        for n in sorted(self.has_output): o.append("  oC%d = uintBitsToFloat(o%d);" % (n, n))
        if 0 in self.has_output: o.append("  if (!alphaTest(oC0.w)) discard;")
        o.append("}")
        o += ["", "void main() {"]
        for n in range(self.temps): o.append("  uvec4 r%d = uvec4(0u);" % n)
        for n, cnt in sorted(self.indexable.items()): o.append("  uvec4 x%d[%d];" % (n, cnt))
        for n, e in sorted(self.inputs.items()): o.append("  vec4 v%d = %s;" % (n, e))
        o.append(body)
        if not self.out or "finish()" not in self.out[-1]: o.append("  finish();")
        o.append("}")
        manifest = ["# generated by sm4asm2glsl.py (no-limits build)",
                    "textures=" + ",".join(str(s) for s in sorted(self.used_tex)),
                    "floats=" + ",".join(str(r) for r in regs)]
        return "\n".join(o) + "\n", "\n".join(manifest) + "\n"


def main():
    sm4, d3d9, frag, txt = sys.argv[1:5]
    try:
        t = Translator(Shader(sm4), parse_d3d9_ct(d3d9))
        code, manifest = t.run()
    except Err as e:
        print("NOT TRANSLATED: %s" % e); sys.exit(2)
    open(frag, "w", newline="\n").write(code)
    open(txt, "w", newline="\n").write(manifest)
    print("ok: %d lines -> %s (%d textures, %d float registers, %d temps)" % (len(t.out), frag, len(t.used_tex), len(t.used_regs), t.temps))


if __name__ == "__main__":
    main()
