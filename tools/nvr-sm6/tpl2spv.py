#!/usr/bin/env python3
r"""
tpl2spv.py: "template mode" of tools/nvr-sm6. One of the game-shader records' (ShaderRecord) preprocessed cache texts, the exact
text D3DX compiled to ps_3_0 in game (<game>\Data\Shaders\VegasShaders\Shaders\Cache\<sub>\<Name>.pso.hlsl, or CacheShipped\),
compiled to a SPIR-V fragment shader for the NVR DXVK fork's full-screen Draw, plus a .layout file the C++ "record mode" reads
(docs\deferred-single-pass.md, step 1). The pass's C++ sets every constant, texture and sampler state on the D3D9 device as for its
D3D9 draw; record mode then reads them back and hands them to one Vulkan Draw:
  - constants: one uniform buffer that IS the D3D9 float register file: register(cN) sits at byte 16 x N (packoffset(cN)); the
    C++ uploads GetPixelShaderConstantF(0, registers), then uvmap and ztest in the two registers after them (c<registers>, +1).
    Names sharing registers (D3D9 aliases: they read the same register) are collapsed: same register and type -> one member and
    #defines; anything else overlapping -> one float4 array over the union of their registers and each name an accessor into it.
    b / i registers and unpinned constants are refused (no register to mirror);
  - textures: D3D9 stage sN -> set 1, binding N, extension slot N (aliases on one stage share it);
  - samplers: DXVK's sampler heap (set 0, binding 0, runtime array); the push block holds only their heap indices, one dword per
    USED stage in stage order (the layout's sampler lines give each offset), filled by the extension from the sampler slot N the C++
    binds; at most 48 bytes (PUSH_LIMIT: DXVK's fragment push budget; past 64 it overflows into its sampler indices). The states come
    from the device at runtime, not from this file;
  - inputs: VPOS = FragCoord.xy - 0.5 (D3D9's whole-number pixel centres; Includes\DeferredDepth.hlsl relies on them), TEXCOORD0 =
    FragCoord.xy x uvmap.xy + uvmap.zw (the D3D9 quad's uv as an affine map of the pixel, whatever viewport the Draw gets). The
    extension's built-in triangle uv (location 0) is not read;
  - outputs: COLORn -> SV_Targetn, DEPTH -> SV_Depth, plus an optional in-shader depth test (ztest, texture zref) for a Draw
    without a depth attachment.
Output: <out>\<Pass>-<key>.spv and <Pass>-<key>.layout; key = FNV-1a 64 of the cache file's bytes with one trailing NUL removed, 16
lower-case hex digits (the same bytes ShaderProgram::PortableSource gives at runtime). Format of .layout: tools\nvr-sm6\README.md.

Usage (Windows: py -3.10 -I; glslangValidator + spirv-val from the Vulkan SDK, found on PATH, in %VULKAN_SDK%\Bin or F:\Tools):
  tpl2spv.py <cache .pso.hlsl> --out <dir> [--pass NAME] [--keep] [--key-only]
"""
import argparse, json, os, re, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import hlsl2spv as H  # noqa: E402  (split_items, balanced, split_commas, fnv1a64, tool)

FORMAT = "nvr-sm6-record 2"
TEX_SET, UBO_SET, UBO_BINDING, HEAP_SET, HEAP_BINDING = 1, 2, 0, 0, 0
STAGES = 16                      # ps_3_0 samplers s0..s15: texture binding / slot = stage
ZREF_BINDING = 16                # the depth-test reference texture (set 1), slot 16
# A fragment shader's push data starts at byte 192 of DXVK's 256-byte push constant array (v3.0.1 DxvkPushDataBlock): 64 bytes before
# it overflows into the array DXVK writes the sampler heap indices to; 48 (256 - 16 reserved - 192) is the stage's budget, and all an
# "Independent" (pipeline-library) layout gives it. The limit is 48: every pass built so far fits (README "Push block").
PUSH_LIMIT = 48
MAX_REGISTERS = 224              # ps_3_0 float constants
VIEW = {"sampler": "2D", "sampler1d": "2D", "sampler2d": "2D", "sampler3d": "3D", "samplercube": "CUBE"}
TEXTURE_TYPE = {"2D": "Texture2D", "3D": "Texture3D", "CUBE": "TextureCube"}
SWZ = "xyzw"


def die(msg):
    raise SystemExit("tpl2spv: ERROR: " + msg)


def warn(msg):
    sys.stderr.write("tpl2spv: warning: " + msg + "\n")


def cache_key(raw):
    """The variant key: FNV-1a 64 over the cache text's bytes, one trailing NUL removed (D3DX's buffer keeps its terminator)."""
    body = raw[:-1] if raw.endswith(b"\0") else raw
    return "%016x" % H.fnv1a64(body), body


def pass_name(path):
    base = os.path.basename(path)
    for suffix in (".pso.hlsl", ".hlsl"):
        if base.lower().endswith(suffix):
            return base[:-len(suffix)]
    return base.split(".")[0]


UNIFORM_RE = re.compile(
    r"^(?:(?:uniform|extern|const)\s+)*(?:(row_major|column_major)\s+)?"
    r"(float|half|int|uint|bool|dword)([1-4](?:x[1-4])?)?\s+(\w+)\s*(?:\[\s*(\d+)\s*\])?\s*"
    r"(?::\s*register\s*\(\s*([A-Za-z])(\d+)\s*\))?\s*(?:<[^>]*>)?\s*(?:=\s*(.*?))?\s*;$", re.S)
SAMPLER_RE = re.compile(
    r"^(?:uniform\s+)?(sampler|sampler1D|sampler2D|sampler3D|samplerCUBE)\s+(\w+)\s*(?::\s*register\s*\(\s*([A-Za-z])(\d+)\s*\))?\s*"
    r"(?:<[^>]*>)?\s*(?:=\s*sampler_state\s*\{(.*)\})?\s*;$", re.S | re.I)
PROTO_RE = re.compile(r"^[\w<>]+(?:\s+[\w<>]+)*?\s+\w+\s*\(.*\)\s*;$", re.S)


class Uniform:
    def __init__(self, major, base, dims, name, count, reg):
        self.base, self.dims, self.name, self.count, self.reg = base, dims, name, count, reg
        if dims and "x" in dims:
            self.rows, self.cols = int(dims[0]), int(dims[2])
            self.major = major or "column_major"     # fxc's default for D3DXCompileShader without PACKMATRIX_ROWMAJOR
            per = self.rows if self.major == "row_major" else self.cols
        else:
            self.rows, self.cols, self.major = 0, int(dims) if dims else 1, None
            per = 1
        self.nregs = per * max(1, count)
        self.how = "member"

    @property
    def type(self):
        return "%s%s%s" % (self.base, self.dims, "[%d]" % self.count if self.count else "")

    def decl(self):
        major = (self.major + " ") if self.major else ""
        return "%s%s%s %s%s" % (major, self.base, self.dims, self.name, "[%d]" % self.count if self.count else "")

    def accessor(self, array, base_reg, index=None):
        """This name (an array: its element [index]) as an expression over float4 array[] whose element 0 is register base_reg."""
        o = self.reg - base_reg
        per = self.nregs // max(1, self.count)
        start = ("%d + (%s)%s" % (o, index, " * %d" % per if per != 1 else "")) if self.count else "%d" % o

        def r(k):
            return "%s[%s]" % (array, start if k == 0 else "%s + %d" % (start, k))
        swz = "" if self.cols == 4 else "." + SWZ[:self.cols]
        if not self.rows:
            return "(%s%s)" % (r(0), swz)
        if self.major == "row_major":
            parts = [r(i) + swz for i in range(self.rows)]
        else:   # register o + j holds column j
            parts = ["%s.%s" % (r(j), SWZ[i]) for i in range(self.rows) for j in range(self.cols)]
        return "(%s%s(%s))" % (self.base, self.dims, ", ".join(parts))


def convert(src_path, out_dir, name=None, keep=False, verbose=True):
    raw = open(src_path, "rb").read()
    key, body = cache_key(raw)
    pname = name or pass_name(src_path)
    base = "%s-%s" % (pname, key)
    text = body.decode("latin-1")
    text = re.sub(r"^\s*#\s*line\b.*$", "", text, flags=re.M)
    # glslang's preprocessor reads backslashes in strings (annotations) as escapes
    text = re.sub(r'"[^"\n]*"', lambda m: m.group(0).replace("\\", "/"), text)

    # 1. classify the top-level items
    structs, functions, uniforms, out_items = {}, {}, [], []
    stages = {}       # stage -> {"view", "names"}
    sampler_alias = {}
    for it in H.split_items(text):
        flat = " ".join(it.split())
        if it.startswith("#"):
            if not re.match(r"#\s*pragma\b", it):
                warn("dropped a directive: " + flat[:80])
            continue
        if re.match(r"^technique\w*\b", flat):
            die("a technique in a template's text: not a ShaderRecord cache file?")
        m = SAMPLER_RE.match(flat)
        if m:
            view = VIEW[m.group(1).lower()]
            if not m.group(3):
                die("sampler %s has no register(sN): its stage is not known" % m.group(2))
            if m.group(3).lower() != "s":
                die("sampler %s in register %s%s" % (m.group(2), m.group(3), m.group(4)))
            st = int(m.group(4))
            if st >= STAGES:
                die("sampler %s on stage %d" % (m.group(2), st))
            s = stages.setdefault(st, {"view": view, "names": []})
            if s["view"] != view:
                die("stage s%d declared as %s (%s) and %s (%s)" % (st, s["view"], s["names"][0], view, m.group(2)))
            if m.group(2) not in s["names"]:
                s["names"].append(m.group(2))
            sampler_alias[m.group(2)] = st
            continue
        if re.match(r"^texture\w*\s+\w+\s*(<[^>]*>)?\s*;$", flat, re.I):
            warn("dropped an effect texture object: " + flat)
            continue
        if flat.startswith("struct "):
            sm = re.match(r"^struct\s+(\w+)\s*\{(.*)\}\s*;$", flat, re.S)
            if sm:
                members = []
                for decl in sm.group(2).split(";"):
                    decl = decl.strip()
                    if decl:
                        dm = re.match(r"^(?:\w+\s+)*?(\w+)\s+(\w+)\s*(?:\[\s*\d+\s*\])?\s*(?::\s*(\w+))?$", decl)
                        if dm:
                            members.append((dm.group(1), dm.group(2), (dm.group(3) or "").upper()))
                structs[sm.group(1)] = members
            out_items.append(("raw", it))
            continue
        head = re.sub(r":\s*register\s*\([^)]*\)", "", flat).split("=", 1)[0]
        if not flat.startswith("static") and not flat.startswith("typedef") and "(" not in head:
            um = UNIFORM_RE.match(flat)
            if um:
                major, btype, dims, uname, count, rclass, rnum, init = um.groups()
                if rclass is None:
                    die("constant %s has no register(cN): fxc chose its register, which this text does not say" % uname)
                if rclass.lower() in ("b", "i"):
                    die("constant %s in register %s%s: bool / int registers are refused (record mode mirrors c registers only)" % (uname, rclass, rnum))
                if rclass.lower() != "c":
                    die("constant %s in register %s%s" % (uname, rclass, rnum))
                if btype not in ("float", "half"):
                    die("constant %s is %s in a float register (its bits would not read as D3D9's float)" % (uname, btype))
                if init is not None:
                    warn("constant %s: initializer dropped (the register file holds what the C++ set)" % uname)
                uniforms.append(Uniform(major, "float", dims or "", uname, int(count) if count else 0, int(rnum)))
                continue
            if "," in flat and not "(" in flat:
                die("cannot read a global declaration: " + flat[:100])
        fm = re.match(r"^([\w<>]+(?:\s+[\w<>]+)*?)\s+(\w+)\s*\(", flat)
        if fm and flat.endswith("}") and "{" in flat:
            functions[fm.group(2)] = it
            out_items.append(("function", it))
            continue
        if PROTO_RE.match(flat) and "=" not in flat.split("(", 1)[0]:   # (fix 5) prototypes take samplers like definitions
            out_items.append(("function", it))
            continue
        out_items.append(("raw", it))
    if "main" not in functions:
        die("no main")

    # 2. registers: components of overlapping ranges
    for u in uniforms:
        if u.reg + u.nregs > MAX_REGISTERS:
            die("constant %s: c%d..c%d is past ps_3_0's %d" % (u.name, u.reg, u.reg + u.nregs - 1, MAX_REGISTERS))
    order = sorted(range(len(uniforms)), key=lambda k: (uniforms[k].reg, -uniforms[k].nregs))
    groups, cur, cur_end = [], [], -1
    for k in order:
        u = uniforms[k]
        if cur and u.reg < cur_end:
            cur.append(k)
            cur_end = max(cur_end, u.reg + u.nregs)
        else:
            if cur:
                groups.append(cur)
            cur, cur_end = [k], u.reg + u.nregs
    if cur:
        groups.append(cur)
    members, defines, group_arrays = [], [], []      # members: (decl, register, uniform or None)
    for g in groups:
        us = [uniforms[k] for k in sorted(g)]           # declaration order
        if len(us) == 1:
            members.append((us[0].decl(), us[0].reg, us[0]))
            continue
        first = us[0]
        if all(u.reg == first.reg and u.type == first.type and u.major == first.major for u in us):
            members.append((first.decl(), first.reg, first))
            for u in us[1:]:
                u.how = "alias=" + first.name
                defines.append("#define %s %s" % (u.name, first.name))
            continue
        lo = min(u.reg for u in us)
        hi = max(u.reg + u.nregs for u in us)
        arr = "nvr_c%d" % lo
        members.append(("float4 %s[%d]" % (arr, hi - lo), lo, None))
        for u in us:
            u.how = "group=" + arr
            if u.count:     # every NAME[i] in the code becomes an element of the group array (rewrite_arrays)
                group_arrays.append((u, arr, lo))
            else:
                defines.append("#define %s %s" % (u.name, u.accessor(arr, lo)))
    registers = max([u.reg + u.nregs for u in uniforms] or [0])

    # 3. samplers: stage textures, names as (texture, slot) pairs
    sampler_names = set(sampler_alias)

    def expand_names(code, names):
        if not names:
            return code
        pat = re.compile(r"(?<![\w.])(" + "|".join(sorted(map(re.escape, names), key=len, reverse=True)) + r")\b(?!\s*__)")
        return pat.sub(lambda m: m.group(1) + "__t, " + m.group(1) + "__s", code)

    def rewrite_arrays(code):
        for u, arr, lo in group_arrays:
            pat = re.compile(r"(?<![\w.])" + re.escape(u.name) + r"\b")
            m = pat.search(code)
            while m:
                j = m.end()
                while j < len(code) and code[j].isspace():
                    j += 1
                if j >= len(code) or code[j] != "[":
                    die("constant array %s shares registers and is used without an index" % u.name)
                k = H.balanced(code, j)
                rep_text = u.accessor(arr, lo, rewrite_arrays(code[j + 1:k]).strip())
                code = code[:m.start()] + rep_text + code[k + 1:]
                m = pat.search(code, m.start() + len(rep_text))
        return code

    def rewrite_function(code):
        p0 = code.index("(")
        p1 = H.balanced(code, p0)
        params = H.split_commas(code[p0 + 1:p1])
        local, new_params = set(), []
        for p in params:
            pm = re.match(r"^(?:in\s+|uniform\s+)?(sampler|sampler1D|sampler2D|sampler3D|samplerCUBE)\s+(\w+)$", " ".join(p.split()), re.I)
            if pm:
                new_params.append("%s %s__t, uint %s__s" % (TEXTURE_TYPE[VIEW[pm.group(1).lower()]], pm.group(2), pm.group(2)))
                local.add(pm.group(2))
            else:
                new_params.append(p)
        return code[:p0 + 1] + ", ".join(new_params) + ")" + expand_names(code[p1 + 1:], sampler_names | local)

    # 4. the entry: main renamed (fix 2: an HLSL function called main next to the SPIR-V entry "main" gives invalid SPIR-V)
    entry = functions["main"]
    fm = re.match(r"^\s*([\w<>]+(?:\s+[\w<>]+)*?)\s+main\s*\(", entry)
    ret_type = fm.group(1).split()[-1]
    p0 = entry.index("(")
    p1 = H.balanced(entry, p0)
    params = H.split_commas(entry[p0 + 1:p1])
    ret_sem = re.match(r"^\s*:\s*(\w+)", entry[p1 + 1:])
    prelude, call, outs, writes_depth = [], [], [], False
    targets = 1
    for p in params:
        pf = " ".join(p.split())
        om = re.match(r"^out\s+(\w+)\s+(\w+)\s*:\s*(\w+)$", pf)     # (fix 3) out DEPTH / out COLORn
        if om:
            ty, nm, sem = om.group(1), om.group(2), om.group(3).upper()
            if sem == "DEPTH" or sem == "DEPTH0":
                sv = "SV_Depth"
                writes_depth = True
                dname = "nvr_out_" + nm
            elif re.match(r"^COLOR\d?$", sem):
                n = int(sem[5:] or 0)
                sv = "SV_Target%d" % n
                targets = max(targets, n + 1)
            else:
                die("main: out parameter %s : %s" % (nm, sem))
            prelude.append("%s nvr_out_%s = (%s)0;" % (ty, nm, ty))
            call.append("nvr_out_" + nm)
            outs.append((ty, nm, sv))
            continue
        dm = re.match(r"^(?:in\s+)?(\w+)\s+(\w+)\s*(?::\s*(\w+))?$", pf)
        if not dm or pf.startswith("uniform") or pf.startswith("inout"):
            die("main: cannot feed parameter '%s'" % pf)
        ptype, pnm, psem = dm.group(1), dm.group(2), (dm.group(3) or "").upper()

        def source(sem, ty):
            if sem in ("TEXCOORD0", "TEXCOORD"):
                return {"float2": "nvr_uv", "float4": "float4(nvr_uv, 0.0f, 1.0f)", "float3": "float3(nvr_uv, 0.0f)"}.get(ty) or die("TEXCOORD0 as %s" % ty)
            if sem == "VPOS":
                return {"float2": "nvr_vpos", "float4": "float4(nvr_vpos, 0.0f, 0.0f)"}.get(ty) or die("VPOS as %s" % ty)
            die("main: input %s has no source in a full-screen pass" % sem)
        if ptype in structs:
            prelude.append("%s nvr_in_%s = (%s)0;" % (ptype, pnm, ptype))
            for mtype, mname, msem in structs[ptype]:
                prelude.append("nvr_in_%s.%s = %s;" % (pnm, mname, source(msem, mtype)))
            call.append("nvr_in_" + pnm)
        else:
            call.append(source(psem, ptype))
    struct_outs = []
    if ret_type in structs:
        for mtype, mname, msem in structs[ret_type]:
            if re.match(r"^COLOR\d?$", msem):
                n = int(msem[5:] or 0)
                struct_outs.append((mname, n))
                targets = max(targets, n + 1)
            elif msem in ("DEPTH", "DEPTH0"):
                die("main returns DEPTH in a struct (not handled)")
            else:
                die("main's output %s : %s" % (mname, msem))
    elif not ret_sem or not re.match(r"^COLOR0?$", ret_sem.group(1).upper()):
        die("main must return COLOR0 or a struct of COLORn")

    # 5. the source, for a map stage -> push dword (the sampler's heap index). The push block holds ONLY those indices: DXVK puts a
    # fragment shader's push data at byte 192 of its 256-byte constant array, so bytes 64 and up overflow into the array where it writes
    # the sampler heap indices, which replace their low 16 bits (2026-11-22 parity probe: the old uvmap at push 64 read the G-buffer up to
    # 7 x 3 pixels off). uvmap and ztest are two more registers in the uniform buffer, right after the D3D9 ones.
    uv_reg = registers                                   # c<registers>: uvmap; c<registers + 1>: ztest (with depth)
    extras = [("float4 nvr_uvmap", uv_reg)] + ([("float4 nvr_ztest", uv_reg + 1)] if writes_depth else [])

    def source(push_index):
        n_push = max(1, len(set(push_index.values())))
        src = ["// generated by tools/nvr-sm6/tpl2spv.py from %s (key %s); do not edit" % (os.path.basename(src_path), key),
               "[[vk::binding(%d, %d)]] SamplerState nvr_samplers[];" % (HEAP_BINDING, HEAP_SET),
               "struct NvrRecPush { uint s[%d]; };   // the used stages' sampler heap indices, in stage order" % n_push,
               "[[vk::push_constant]] ConstantBuffer<NvrRecPush> nvr_push;",
               "SamplerState nvr_sampler(uint s) { return nvr_samplers[nvr_push.s[s] & 0xFFFF]; }"]
        for kind, tt in TEXTURE_TYPE.items():
            uv = {"2D": "float2", "3D": "float3", "CUBE": "float3"}[kind]
            sw = "xy" if uv == "float2" else "xyz"
            nm = {"2D": "tex2D", "3D": "tex3D", "CUBE": "texCUBE"}[kind]
            src.append("float4 %s(%s t, uint s, %s uv) { return t.Sample(nvr_sampler(s), uv); }" % (nm, tt, uv))
            src.append("float4 %slod(%s t, uint s, float4 uv) { return t.SampleLevel(nvr_sampler(s), uv.%s, uv.w); }" % (nm, tt, sw))
            src.append("float4 %sbias(%s t, uint s, float4 uv) { return t.SampleBias(nvr_sampler(s), uv.%s, uv.w); }" % (nm, tt, sw))
            src.append("float4 %sgrad(%s t, uint s, %s uv, %s dx, %s dy) { return t.SampleGrad(nvr_sampler(s), uv, dx, dy); }" % (nm, tt, uv, uv, uv))
        src.append("float4 tex2Dproj(Texture2D t, uint s, float4 uv) { return t.Sample(nvr_sampler(s), uv.xy / uv.w); }")
        src.append("[[vk::binding(%d, %d)]] cbuffer NvrRegisters {" % (UBO_BINDING, UBO_SET))
        for decl, reg in [(d, r) for d, r, _ in members] + extras:
            src.append("  %s : packoffset(c%d);" % (decl, reg))     # (fix 7) the D3D9 register file at 16 x N
        src.append("};")
        src.extend(defines)
        for st in sorted(stages):
            src.append("[[vk::binding(%d, %d)]] %s nvr_t%d;" % (st, TEX_SET, TEXTURE_TYPE[stages[st]["view"]], st))
            for n in stages[st]["names"]:
                src.append("#define %s__t nvr_t%d" % (n, st))
                src.append("#define %s__s %du" % (n, push_index.get(st, 0)))   # (an unused stage's index is never read)
        if writes_depth:
            src.append("[[vk::binding(%d, %d)]] Texture2D<float4> nvr_zref;" % (ZREF_BINDING, TEX_SET))
        for kind, it in out_items:
            if kind == "function":
                if it is entry:
                    it = re.sub(r"^(\s*[\w<>]+(?:\s+[\w<>]+)*?\s+)main(\s*\()", r"\1nvr_tpl_main\2", it, count=1)
                src.append(rewrite_function(rewrite_arrays(it)))
            else:
                src.append(expand_names(rewrite_arrays(it), sampler_names))
        decl = ["float4 o%d : SV_Target%d;" % (i, i) for i in range(targets)]
        if writes_depth:
            decl.append("float od : SV_Depth;")
        src.append("struct NvrRecOut { " + " ".join(decl) + " };")
        src.append("NvrRecOut nvr_main(float4 nvr_pos : SV_Position) {")
        src.append("  const float2 nvr_vpos = nvr_pos.xy - 0.5f;   // D3D9 VPOS: whole-number pixel centres")
        src.append("  const float2 nvr_uv = nvr_pos.xy * nvr_uvmap.xy + nvr_uvmap.zw;   // the D3D9 quad's TEXCOORD0")
        src.extend("  " + l for l in prelude)
        src.append("  NvrRecOut o = (NvrRecOut)0;")
        if ret_type in structs:
            src.append("  %s r = nvr_tpl_main(%s);" % (ret_type, ", ".join(call)))
            src.extend("  o.o%d = r.%s;" % (n, m) for m, n in struct_outs)
        else:
            src.append("  o.o0 = nvr_tpl_main(%s);" % ", ".join(call))
        for ty, nm, sv in outs:
            if sv.startswith("SV_Target"):
                src.append("  o.o%s = nvr_out_%s;" % (sv[9:], nm))
        if writes_depth:
            src.extend(["  // the D3D9 pass's depth test, in the shader (ztest: x 0 off, 1 zref holds view z (reference = y + z / it),",
                        "  // 2 zref holds device depth; w 1 GREATEREQUAL, 2 LESSEQUAL); a Draw with a depth attachment sets x 0 and tests SV_Depth",
                        "  [branch] if (nvr_ztest.x > 0.5f) {",
                        "    const float zr = nvr_zref.Load(int3(int2(nvr_vpos), 0)).x;",
                        "    const float ref = nvr_ztest.x > 1.5f ? zr : nvr_ztest.y + nvr_ztest.z / max(zr, 1.0e-6f);",
                        "    if (nvr_ztest.w > 1.5f ? !(%s <= ref) : !(%s >= ref)) discard;" % (dname, dname),
                        "  }",
                        "  o.od = %s;" % dname])
        src.append("  return o;\n}")
        return "\n".join(src) + "\n", n_push

    # 6. compile
    os.makedirs(out_dir, exist_ok=True)
    hlsl_path = os.path.join(out_dir, base + ".rec.hlsl")
    spv_path = os.path.join(out_dir, base + ".spv")

    def compile_reflect(push_index):
        source_text, n_push = source(push_index)
        with open(hlsl_path, "w", newline="\n") as f:
            f.write(source_text)
        r = subprocess.run([H.tool("glslangValidator"), "-D", "-V", "-S", "frag", "-e", "main", "--source-entrypoint", "nvr_main",
                            "--target-env", "vulkan1.1", "--hlsl-offsets", "-o", spv_path, hlsl_path], capture_output=True)
        if r.returncode != 0:
            die("glslang failed on %s (rewritten source kept in %s):\n%s" % (src_path, hlsl_path,
                (r.stdout + r.stderr).decode(errors="replace")[-4000:]))

        # 7. SPIR-V: the sampler heap as a runtime array (as hlsl2spv), reflection of what is used
        words = list(struct.unpack("<%dI" % (os.path.getsize(spv_path) // 4), open(spv_path, "rb").read()))
        insts, i = [], 5
        while i < len(words):
            wc, op = words[i] >> 16, words[i] & 0xFFFF
            insts.append([op] + words[i + 1:i + wc])
            i += wc

        def sp_string(ws):
            b = struct.pack("<%dI" % len(ws), *ws)
            return b[:b.index(b"\0")].decode()
        names, var_type, ptr_type, loaded = {}, {}, {}, set()
        member_names, member_offsets = {}, {}
        for ins in insts:
            if ins[0] == 5:
                names[ins[1]] = sp_string(ins[2:])
            elif ins[0] == 6:
                member_names[(ins[1], ins[2])] = sp_string(ins[3:])
            elif ins[0] == 72 and ins[3] == 35:
                member_offsets[(ins[1], ins[2])] = ins[4]
            elif ins[0] == 59:
                var_type[ins[2]] = ins[1]
            elif ins[0] == 32:
                ptr_type[ins[1]] = ins[3]
            elif ins[0] in (61, 65):     # OpLoad / OpAccessChain of a variable
                loaded.add(ins[3])
        by_name = {v: k for k, v in names.items() if k in var_type}
        heap = by_name.get("nvr_samplers")
        if heap is not None:
            arr = ptr_type[var_type[heap]]
            for ins in insts:
                if ins[0] == 28 and ins[1] == arr:
                    ins[:] = [29, ins[1], ins[2]]
            last_cap = max(k for k, ins in enumerate(insts) if ins[0] == 17)
            if not any(ins[0] == 17 and ins[1] == 5302 for ins in insts):
                insts.insert(last_cap + 1, [17, 5302])
            ext = b"SPV_EXT_descriptor_indexing\0"
            ext += b"\0" * (-len(ext) % 4)
            if not any(ins[0] == 10 and sp_string(ins[1:]) == "SPV_EXT_descriptor_indexing" for ins in insts):
                insts.insert(last_cap + 2, [10] + list(struct.unpack("<%dI" % (len(ext) // 4), ext)))
        out_words = words[:5]
        for ins in insts:
            out_words.append((len(ins) << 16) | ins[0])
            out_words += ins[1:]
        open(spv_path, "wb").write(struct.pack("<%dI" % len(out_words), *out_words))
        v = subprocess.run([H.tool("spirv-val"), "--target-env", "vulkan1.1", spv_path], capture_output=True)
        if v.returncode != 0:
            die("spirv-val %s: %s" % (spv_path, (v.stdout + v.stderr).decode(errors="replace")))

        # self-check: every uniform-buffer member at 16 x its register (uvmap / ztest included)
        block_struct = next((k for k, v in names.items() if v == "NvrRegisters" and k not in var_type), None)
        ubo_used = block_struct is not None
        if "$Global" in names.values():
            die("%s: a global glslang put in its own $Global buffer (a uniform this converter did not recognise)" % src_path)
        if ubo_used:
            for idx, (decl, reg) in enumerate([(d, r) for d, r, _ in members] + extras):
                mname = decl.split()[-1].split("[")[0]
                got = member_offsets.get((block_struct, idx))
                if member_names.get((block_struct, idx)) != mname or got != 16 * reg:
                    die("uniform member %d %s at %s, expected %d" % (idx, mname, got, 16 * reg))
        used = {st: (by_name.get("nvr_t%d" % st) in loaded) for st in stages}
        zref_used = writes_depth and by_name.get("nvr_zref") in loaded
        return used, zref_used, ubo_used, n_push

    # first with every declared stage in the push block, then (when some go unused) again with the used ones only
    declared = {st: k for k, st in enumerate(sorted(stages))}
    used, zref_used, ubo_used, n_push = compile_reflect(declared)
    push_index = {st: k for k, st in enumerate(st for st in sorted(stages) if used[st])}
    if len(push_index) != len(declared):
        used2, zref_used, ubo_used, n_push = compile_reflect(push_index)
        if used2 != used:
            die("the used stages changed when the push block was compacted (%s, then %s)" % (
                sorted(s for s in used if used[s]), sorted(s for s in used2 if used2[s])))
    if not keep:
        os.remove(hlsl_path)
    push_bytes = 4 * n_push
    if push_bytes > PUSH_LIMIT:
        die("%s: %d used samplers = a %d-byte push block, over DXVK's %d-byte fragment push budget (past 64 bytes it overflows into the "
            "sampler indices DXVK writes; tools/nvr-sm6/README.md 'Push block')" % (src_path, len(push_index), push_bytes, PUSH_LIMIT))
    ubo_registers = uv_reg + len(extras)

    # 8. the layout
    L = [FORMAT,
         "# tools/nvr-sm6/tpl2spv.py; format: tools/nvr-sm6/README.md",
         "pass %s" % pname,
         "key %s" % key,
         "from %s" % src_path.replace("\\", "/").split("/Cache/")[-1].split("/CacheShipped/")[-1],
         "registers %d" % registers,
         "ubo %d %d %d %d %s" % (UBO_SET, UBO_BINDING, 0, 16 * ubo_registers, "used" if ubo_used else "unused"),
         "heap %d %d" % (HEAP_SET, HEAP_BINDING),
         "push %d" % push_bytes,
         "uvmap %d" % (16 * uv_reg)]
    if writes_depth:
        L.append("ztest %d" % (16 * (uv_reg + 1)))
    for st in sorted(stages):
        L.append("texture %d %d %d %d %s %s %s" % (st, TEX_SET, st, st, stages[st]["view"], "used" if used[st] else "unused", ",".join(stages[st]["names"])))
    for st in sorted(stages):
        if used[st]:
            L.append("sampler %d %d %d" % (st, st, 4 * push_index[st]))
    if writes_depth:
        L.append("zref %d %d %d 2D %s" % (TEX_SET, ZREF_BINDING, ZREF_BINDING, "used" if zref_used else "unused"))
    for u in uniforms:
        L.append("const %s %d %d %s %s %s" % (u.name, u.reg, u.nregs, u.type, u.major or "-", u.how))
    for decl, reg, u in members:
        if u is None:
            L.append("group %s %d %d" % (decl.split()[-1].split("[")[0], reg, int(decl.split("[")[1].rstrip("]"))))
    L.append("targets %d" % targets)
    L.append("depth %d" % (1 if writes_depth else 0))
    with open(os.path.join(out_dir, base + ".layout"), "w", newline="\n") as f:
        f.write("\n".join(L) + "\n")
    info = {"pass": pname, "key": key, "spv": spv_path, "bytes": os.path.getsize(spv_path), "registers": registers,
            "constants": len(uniforms), "aliases": sum(1 for u in uniforms if u.how != "member"),
            "groups": sum(1 for m in members if m[2] is None), "textures": sum(used.values()), "stages": len(stages),
            "targets": targets, "depth": writes_depth, "push": push_bytes, "samplers": len(push_index)}
    if verbose:
        print("ok %s: %d bytes SPIR-V, c0..c%d (%d constants, %d sharing registers, %d groups), %d of %d stages used, %d-byte push, %d target(s)%s" % (
            base, info["bytes"], registers - 1, info["constants"], info["aliases"], info["groups"], info["textures"], info["stages"],
            push_bytes, targets, ", depth" if writes_depth else ""))
    return info


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("cache", help="a ShaderRecord's preprocessed cache text (<Name>.pso.hlsl)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--pass", dest="pass_", default=None, help="pass name in the file name (default: the record name)")
    ap.add_argument("--keep", action="store_true", help="keep the rewritten source (<Pass>-<key>.rec.hlsl)")
    ap.add_argument("--key-only", action="store_true", help="print the variant key and stop")
    a = ap.parse_args()
    if a.key_only:
        print(cache_key(open(a.cache, "rb").read())[0])
        return
    convert(a.cache, a.out, a.pass_, a.keep)


if __name__ == "__main__":
    main()
