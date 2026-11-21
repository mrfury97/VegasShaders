#!/usr/bin/env python3
"""
hlsl2spv.py: one pass of an NVR effect (.fx.hlsl, the same file the D3D9 path compiles with fxc as ps_3_0) compiled to a
SPIR-V fragment shader for the NVR DXVK fork, without Shader Model 3's limits (224 float constants, 16 samplers, 32
temporaries, instruction slots). UNOFFICIAL 2026-10-26 (deep/fork-sm6); docs/next/FORK-1026-SM6.md.

Compiler: glslang's HLSL front end (DXC's SPIR-V back end is not in the Windows SDK's dxc.exe, and glslang is already the
fork's build tool). The effect source is preprocessed with the defines of one variant plus NVR_SM6=1 (so `#if NVR_SM6`
blocks add what only the fork can afford), then rewritten into what DXVK's NVR extension binds:
  - every global sampler2D/3D/CUBE becomes a separate texture (set 1, binding i, extension slot i) plus a sampler from DXVK's
    sampler heap (set 0, binding 0, a runtime array) whose heap index sits in the push block (uint per distinct sampler
    state, extension sampler slot k at push offset 4k); tex2D/tex2Dlod/... become small functions over (texture, slot);
    functions taking a sampler take (texture, slot) instead;
  - every global uniform (TESR_..., NVR_...) moves into one uniform buffer (set 2, binding 0, HLSL cbuffer packing, matrices
    row_major so a D3DXMATRIX is copied as stored: the logical matrix the effect framework's SetMatrix gives the D3D9 pass);
  - the techniques are dropped; a wrapper entry point calls the pass's pixel shader with the technique's arguments, its uv
    from location 0 (the extension's built-in full-screen triangle), POSITION/VPOS from gl_FragCoord, COLORn -> location n.
Output: <out>/<Effect>.<Technique>.<Pass>.<variant>.spv and a .txt layout NVR reads (core/Sm6Pass.cpp): source hashes
(a stale .spv is refused in game), the uniform buffer's members and offsets (+ literal defaults), the textures with their
binding and sampler slot, the sampler states, the push size and the pass's target count.

Usage (Linux/WSL, glslang + spirv-val on PATH or in ~/vktools/root/usr/bin):
  hlsl2spv.py <effect.fx.hlsl> <technique> <pass index or name> --out <dir> [-D NAME=VALUE | -D NAME= ]...
Variants and the list of passes: tools/nvr-sm6/build.sh + passes.txt.
"""
import argparse, os, re, struct, subprocess, sys

SAMPLER_TYPES = {"sampler": "2D", "sampler1D": "2D", "sampler2D": "2D", "sampler3D": "3D", "samplerCUBE": "CUBE"}
TEXTURE_TYPE = {"2D": "Texture2D", "3D": "Texture3D", "CUBE": "TextureCube"}
SCALAR_SIZE = 4


def die(msg):
    sys.stderr.write("hlsl2spv: ERROR: " + msg + "\n")
    sys.exit(1)


def warn(msg):
    sys.stderr.write("hlsl2spv: warning: " + msg + "\n")


def fnv1a64(data):
    h = 0xcbf29ce484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def fnv1a32(data):
    h = 0x811c9dc5
    for b in data:
        h ^= b
        h = (h * 0x01000193) & 0xFFFFFFFF
    return h


def include_files(path, seen=None):
    """The file and everything it #includes (quoted, relative to the including file), in first-seen order."""
    if seen is None:
        seen = []
    path = os.path.normpath(path)
    if path in seen:
        return seen
    seen.append(path)
    text = open(path, "rb").read().decode("latin-1")
    for m in re.finditer(r'^\s*#\s*include\s*"([^"]+)"', text, re.M):
        rel = m.group(1).replace("\\", "/")
        for base_dir in (os.path.dirname(path), os.path.dirname(seen[0])):   # the includer's folder, then the effect's (D3DX finds both)
            inc = os.path.join(base_dir, rel)
            if os.path.exists(inc):
                include_files(inc, seen)
                break
    return seen


def tool(name):
    r"""A Vulkan SDK tool: ~/vktools (Linux/WSL), else on Windows %VULKAN_SDK%\Bin or the SDK under F:\Tools (glslang is
    glslangValidator.exe there), else the bare name (PATH)."""
    local = os.path.expanduser("~/vktools/root/usr/bin/" + name)
    if os.path.exists(local):
        return local
    if os.name == "nt":
        import shutil
        exe = name if name.lower().endswith(".exe") else name + ".exe"
        for d in (os.path.join(os.environ.get("VULKAN_SDK", ""), "Bin"), r"F:\Tools\VulkanSDK\1.3.296.0\Bin"):
            for n in ((exe, "glslangValidator.exe") if name == "glslang" else (exe,)):
                if d and os.path.exists(os.path.join(d, n)):
                    return os.path.join(d, n)
        return shutil.which(exe) or name
    return name


# ---------------------------------------------------------------------------------------------------------------------
# Top-level structure of the preprocessed source
# ---------------------------------------------------------------------------------------------------------------------
def split_items(text):
    """Top-level items: '#...' lines, declarations ending in ';' (also 'x = sampler_state {...};', 'struct S {...};'),
    and function definitions ending in their closing brace."""
    items, i, n = [], 0, len(text)
    while i < n:
        while i < n and text[i] in " \t\r\n":
            i += 1
        if i >= n:
            break
        if text[i] == "#":
            j = text.find("\n", i)
            j = n if j < 0 else j
            items.append(text[i:j])
            i = j
            continue
        start, depth, paren = i, 0, 0
        while i < n:
            c = text[i]
            if c == "(":
                paren += 1
            elif c == ")":
                paren -= 1
            elif c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    k = i + 1
                    while k < n and text[k] in " \t\r\n":
                        k += 1
                    if k < n and text[k] == ";":
                        i = k
                        items.append(text[start:i + 1])
                        i += 1
                        break
                    # a function body ends here unless this was an initializer list inside a declaration (then a ';' follows later)
                    head = text[start:i]
                    if "=" in head.split("{", 1)[0] and "(" not in head.split("=", 1)[0]:
                        i += 1
                        continue
                    items.append(text[start:i + 1])
                    i += 1
                    break
            elif c == ";" and depth == 0 and paren == 0:
                head = text[start:i]
                # a ";" inside an annotation (<string ResourceName = "x.dds";>) of a non-static declaration does not end it
                if head.count("<") > head.count(">") and re.match(r"\s*(uniform\s+|extern\s+)?(sampler\w*|texture\w*|float\w*|half\w*|int\w*|bool\w*)\s", head):
                    i += 1
                    continue
                items.append(text[start:i + 1])
                i += 1
                break
            i += 1
        else:
            items.append(text[start:])
    return items


def balanced(text, open_pos):
    """Index of the bracket closing the one at open_pos."""
    pairs = {"(": ")", "{": "}", "[": "]", "<": ">"}
    o, c, depth = text[open_pos], pairs[text[open_pos]], 0
    for i in range(open_pos, len(text)):
        if text[i] == o:
            depth += 1
        elif text[i] == c:
            depth -= 1
            if depth == 0:
                return i
    die("unbalanced " + o)


def split_commas(text):
    out, depth, cur = [], 0, ""
    for c in text:
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        if c == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += c
    if cur.strip():
        out.append(cur)
    return [s.strip() for s in out]


# ---------------------------------------------------------------------------------------------------------------------
# Sampler states -> the extension's sampler descriptions
# ---------------------------------------------------------------------------------------------------------------------
def sampler_state(body, name):
    st = {}
    for m in re.finditer(r"(\w+)\s*=\s*([\w<>]+)\s*;", body or ""):
        st[m.group(1).upper()] = m.group(2).upper()
    mn, mg, mp = st.get("MINFILTER", "POINT"), st.get("MAGFILTER", "POINT"), st.get("MIPFILTER", "NONE")
    if "ANISOTROPIC" in (mn, mg, mp):
        filt = "ANISOTROPIC"
    elif "LINEAR" in (mn, mg):
        filt = "LINEAR_MIPMAPS" if mp in ("LINEAR", "POINT") else "LINEAR"
        if mn != mg:
            warn("%s: MINFILTER %s / MAGFILTER %s differ; both linear here" % (name, mn, mg))
    else:
        filt = "NEAREST"
        if mp not in ("NONE",):
            warn("%s: point sampling with MIPFILTER %s: no mips here" % (name, mp))
    au = st.get("ADDRESSU", "WRAP")
    for k in ("ADDRESSV", "ADDRESSW"):
        if k in st and st[k] != au and not (k == "ADDRESSW" and "ADDRESSW" not in st):
            warn("%s: %s %s differs from ADDRESSU %s; ADDRESSU used for all" % (name, k, st[k], au))
    if au == "BORDER" and st.get("BORDERCOLOR", "0") not in ("0", "0X00000000"):
        warn("%s: BORDERCOLOR %s: the extension's border is transparent black" % (name, st.get("BORDERCOLOR")))
    if st.get("SRGBTEXTURE", "FALSE") not in ("FALSE", "0"):
        warn("%s: SRGBTEXTURE: not mapped (no sRGB view)" % name)
    return filt, {"CLAMP": "CLAMP", "WRAP": "WRAP", "MIRROR": "MIRROR", "BORDER": "BORDER"}.get(au, "CLAMP")


# ---------------------------------------------------------------------------------------------------------------------
# Uniform declarations
# ---------------------------------------------------------------------------------------------------------------------
UNIFORM_RE = re.compile(
    r"^(?:(?:uniform|extern|const)\s+)*(?:(row_major|column_major)\s+)?"
    r"(float|half|int|uint|bool|dword)([1-4](?:x[1-4])?)?\s+(\w+)\s*(?:\[\s*(\d+)\s*\])?\s*"
    r"(?::\s*register\s*\([^)]*\))?\s*(?:<[^>]*>)?\s*(?:=\s*(.*?))?\s*;$", re.S)


def type_size(base, dims, count):
    """Bytes the member takes in the uniform buffer (HLSL packing; arrays: 16-byte elements but the last one tight)."""
    if dims and "x" in dims:
        r, c = int(dims[0]), int(dims[2])
        elem = (r - 1) * 16 + c * 4     # row_major: r rows of c floats, 16-byte row stride
    else:
        elem = (int(dims) if dims else 1) * SCALAR_SIZE
    if count:
        stride = (elem + 15) // 16 * 16
        return stride * (count - 1) + elem, stride
    return elem, 0


def literal_floats(init):
    if init is None:
        return None
    s = init.strip()
    m = re.match(r"^(?:float[1-4]?|half[1-4]?|int[1-4]?)\s*\((.*)\)$", s, re.S)
    vals = split_commas(m.group(1)) if m else [s]
    out = []
    for v in vals:
        v = v.strip().rstrip("fFhH")
        if v.lower() in ("true", "false"):
            out.append(1.0 if v.lower() == "true" else 0.0)
            continue
        try:
            out.append(float(v))
        except ValueError:
            return None
    return out


# ---------------------------------------------------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("effect")
    ap.add_argument("technique")
    ap.add_argument("pass_", metavar="pass")
    ap.add_argument("--out", required=True)
    ap.add_argument("-D", dest="defines", action="append", default=[])
    ap.add_argument("--variant", default="", help="variant key (build.sh): the file name's last part")
    ap.add_argument("--hash-root", default=None, help="paths in the layout's hash lines are relative to this folder")
    ap.add_argument("--keep", action="store_true", help="keep the rewritten source next to the output (.sm6.hlsl)")
    a = ap.parse_args()

    effect = os.path.abspath(a.effect)
    effect_name = os.path.basename(effect).split(".")[0]
    defines = ["NVR_SM6=1"] + a.defines
    hash_root = os.path.abspath(a.hash_root) if a.hash_root else os.path.dirname(os.path.dirname(effect))

    # 1. preprocess (glslang's preprocessor, HLSL mode, the variant's defines), on a copy of the sources whose annotation strings
    # ("Precipitations\splash.dds") have their backslashes turned round (glslang's preprocessor rejects them as escapes)
    import tempfile, shutil
    tmp = tempfile.mkdtemp(prefix="nvr-sm6-")
    root = os.path.dirname(os.path.dirname(effect))
    for f in include_files(effect):
        dst = os.path.join(tmp, os.path.relpath(f, root))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        src_text = open(f, "rb").read().decode("latin-1")
        src_text = re.sub(r'"[^"\n]*"', lambda m: m.group(0).replace("\\", "/"), src_text)
        open(dst, "wb").write(src_text.encode("latin-1"))
    cmd = [tool("glslang"), "-E", "-D", "-S", "frag", "-I" + os.path.join(tmp, os.path.relpath(os.path.dirname(effect), root))] + ["-D" + d for d in defines] + [os.path.join(tmp, os.path.relpath(effect, root))]
    pp = subprocess.run(cmd, capture_output=True)
    if pp.returncode != 0:
        die("preprocess failed:\n" + pp.stdout.decode(errors="replace") + pp.stderr.decode(errors="replace"))
    text = pp.stdout.decode("latin-1")
    shutil.rmtree(tmp, ignore_errors=True)
    text = re.sub(r"^\s*#\s*line\b.*$", "", text, flags=re.M)

    items = split_items(text)

    # 2. classify
    structs, functions, samplers, uniforms, out_items = {}, {}, [], [], []
    technique_items = []
    for it in items:
        flat = " ".join(it.split())
        if it.startswith("#"):
            out_items.append(("raw", it))
            continue
        if re.match(r"^technique\w*\b", flat):
            technique_items.append(it)
            continue
        m = re.match(r"^(?:uniform\s+)?(sampler|sampler1D|sampler2D|sampler3D|samplerCUBE)\s+(\w+)\s*(?::\s*register\s*\(\s*\w+\s*\))?\s*(?:<[^>]*>)?\s*"
                     r"(?:=\s*sampler_state\s*\{(.*)\})?\s*;$", flat, re.S | re.I)
        if m:
            kind = SAMPLER_TYPES.get(m.group(1), None) or SAMPLER_TYPES[[k for k in SAMPLER_TYPES if k.lower() == m.group(1).lower()][0]]
            samplers.append({"name": m.group(2), "view": kind, "state": sampler_state(m.group(3), m.group(2))})
            out_items.append(("sampler", m.group(2)))
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
                    if not decl:
                        continue
                    dm = re.match(r"^(?:\w+\s+)*?(\w+)\s+(\w+)\s*(?:\[\s*\d+\s*\])?\s*(?::\s*(\w+))?$", decl)
                    if dm:
                        members.append((dm.group(1), dm.group(2), (dm.group(3) or "").upper()))
                structs[sm.group(1)] = members
            out_items.append(("raw", it))
            continue
        if not flat.startswith("static") and "(" not in flat.split("=", 1)[0]:
            um = UNIFORM_RE.match(flat)
            if um:
                if um.group(1) == "column_major":
                    die("column_major uniform %s: the uniform buffer is row_major" % um.group(4))
                base, dims, name, count, init = um.group(2), um.group(3), um.group(4), um.group(5), um.group(6)
                uniforms.append({"base": base, "dims": dims or "", "name": name, "count": int(count) if count else 0,
                                 "default": literal_floats(init)})
                if init is not None and literal_floats(init) is None:
                    warn("uniform %s: initializer '%s' is not a literal; it starts as 0 unless NVR sets it" % (name, init))
                continue
        fm = re.match(r"^([\w<>]+(?:\s+[\w<>]+)*?)\s+(\w+)\s*\(", flat)
        if fm and flat.endswith("}") and "{" in flat:
            functions[fm.group(2)] = it
            out_items.append(("function", it))
            continue
        out_items.append(("raw", it))

    # 3. the pass: technique -> pass -> PixelShader = compile ps_x FUNC(args)
    tech = None
    for t in technique_items:
        tm = re.match(r"^\s*technique\w*\s*(\w*)\s*\{", t)
        tname = tm.group(1) if tm else ""
        if tname == a.technique or (a.technique in ("-", "", "default") and tname == ""):
            tech = t
            break
    if tech is None:
        die("technique %s not found (have: %s)" % (a.technique, ", ".join(re.match(r"^\s*technique\w*\s*(\w*)", t).group(1) or "<unnamed>" for t in technique_items)))
    body = tech[tech.index("{") + 1:tech.rindex("}")]
    passes = [(pm.group(1), pm.group(2)) for pm in re.finditer(r"pass\s*(\w*)\s*\{([^{}]*)\}", body)]
    if a.pass_.isdigit():
        pidx = int(a.pass_)
    else:
        names = [p[0] for p in passes]
        if a.pass_ not in names:
            die("pass %s not in technique %s" % (a.pass_, a.technique))
        pidx = names.index(a.pass_)
    if pidx >= len(passes):
        die("technique %s has %d passes" % (a.technique, len(passes)))
    pass_body = passes[pidx][1]
    psm = re.search(r"PixelShader\s*=\s*compile\s+\w+\s+(\w+)\s*\((.*?)\)\s*;", pass_body, re.S)
    if not psm:
        die("no PixelShader in %s pass %d" % (a.technique, pidx))
    vsm = re.search(r"VertexShader\s*=\s*compile\s+\w+\s+(\w+)\s*\(", pass_body)
    entry, entry_args = psm.group(1), split_commas(psm.group(2))
    states = [s.strip() for s in re.sub(r"(VertexShader|PixelShader)\s*=[^;]*;", "", pass_body).split(";") if s.strip()]
    for s in states:
        warn("pass render state not applied by the SPIR-V pass (the caller's job): " + s)
    if entry not in functions:
        die("pixel shader %s not found" % entry)

    # 4. sampler slots (one per distinct state) and texture bindings
    slot_of_state, tex_bind = {}, {}
    for i, s in enumerate(samplers):
        s["binding"] = i
        s["slot"] = slot_of_state.setdefault(s["state"], len(slot_of_state))
        tex_bind[s["name"]] = s
    sampler_names = set(tex_bind)
    nslots = max(1, len(slot_of_state))

    def expand_names(code, names):
        if not names:
            return code
        pat = re.compile(r"(?<![\w.])(" + "|".join(sorted(map(re.escape, names), key=len, reverse=True)) + r")\b(?!\s*__)")
        return pat.sub(lambda m: m.group(1) + "__t, " + m.group(1) + "__s", code)

    def rewrite_function(code):
        p0 = code.index("(")
        p1 = balanced(code, p0)
        params = split_commas(code[p0 + 1:p1])
        local = set()
        new_params = []
        for p in params:
            pm = re.match(r"^(?:in\s+|uniform\s+)?(sampler|sampler1D|sampler2D|sampler3D|samplerCUBE)\s+(\w+)$", " ".join(p.split()), re.I)
            if pm:
                kind = [v for k, v in SAMPLER_TYPES.items() if k.lower() == pm.group(1).lower()][0]
                new_params.append("%s %s__t, uint %s__s" % (TEXTURE_TYPE[kind], pm.group(2), pm.group(2)))
                local.add(pm.group(2))
            else:
                # HLSL lets a function change its uniform parameters (a technique's arguments); here they are plain inputs
                new_params.append(re.sub(r"^\s*uniform\s+", "", p))
        rest = code[p1 + 1:]
        return code[:p0 + 1] + ", ".join(new_params) + ")" + expand_names(rest, sampler_names | local)

    # 5. the rewritten source
    src = []
    src.append("// generated by tools/nvr-sm6/hlsl2spv.py from %s, technique %s pass %d (%s); do not edit" % (os.path.basename(effect), a.technique, pidx, entry))
    src.append("#pragma pack_matrix(row_major)")
    src.append("[[vk::binding(0, 0)]] SamplerState nvr_samplers[];")
    src.append("struct NvrSm6Push { uint s[%d]; };" % nslots)
    src.append("[[vk::push_constant]] ConstantBuffer<NvrSm6Push> nvr_push;")
    for kind, tt in TEXTURE_TYPE.items():
        uv2 = {"2D": "float2", "3D": "float3", "CUBE": "float3"}[kind]
        nm = {"2D": "tex2D", "3D": "tex3D", "CUBE": "texCUBE"}[kind]
        src.append("float4 %s(%s t, uint s, %s uv) { return t.Sample(nvr_samplers[nvr_push.s[s]], uv); }" % (nm, tt, uv2))
        src.append("float4 %slod(%s t, uint s, float4 uv) { return t.SampleLevel(nvr_samplers[nvr_push.s[s]], uv.%s, uv.w); }" % (nm, tt, uv2[-1] == "2" and "xy" or "xyz"))
        src.append("float4 %sbias(%s t, uint s, float4 uv) { return t.SampleBias(nvr_samplers[nvr_push.s[s]], uv.%s, uv.w); }" % (nm, tt, uv2[-1] == "2" and "xy" or "xyz"))
        src.append("float4 %sgrad(%s t, uint s, %s uv, %s dx, %s dy) { return t.SampleGrad(nvr_samplers[nvr_push.s[s]], uv, dx, dy); }" % (nm, tt, uv2, uv2, uv2))
    src.append("float4 tex2Dproj(Texture2D t, uint s, float4 uv) { return t.Sample(nvr_samplers[nvr_push.s[s]], uv.xy / uv.w); }")
    if uniforms:
        src.append("[[vk::binding(0, 2)]] cbuffer NvrSm6Globals {")
        for u in uniforms:
            src.append("  %s%s %s%s;" % (u["base"] if u["base"] != "dword" else "uint", u["dims"], u["name"], "[%d]" % u["count"] if u["count"] else ""))
        src.append("};")
    for s in samplers:
        src.append("[[vk::binding(%d, 1)]] %s %s__t;" % (s["binding"], TEXTURE_TYPE[s["view"]], s["name"]))
        src.append("static const uint %s__s = %d;" % (s["name"], s["slot"]))
    for kind, it in out_items:
        if kind == "sampler":
            continue
        if kind == "function":
            src.append(rewrite_function(it))
        elif kind == "raw":
            src.append(it if it.startswith("#pragma") or it.startswith("#extension") else expand_names(it, sampler_names) if not it.startswith("#") else "")

    # wrapper entry point
    fcode = functions[entry]
    fm = re.match(r"^\s*([\w<>]+(?:\s+[\w<>]+)*?)\s+(\w+)\s*\(", fcode)
    ret_type = fm.group(1).split()[-1]
    p0 = fcode.index("(")
    params = split_commas(fcode[p0 + 1:balanced(fcode, p0)])
    call, prelude, ai = [], [], 0
    for p in params:
        pf = " ".join(p.split())
        dm = re.match(r"^(uniform\s+)?(?:in\s+)?(\w+)\s+(\w+)\s*(?::\s*(\w+))?\s*(?:=\s*(.+))?$", pf)
        if not dm:
            die("cannot read parameter '%s' of %s" % (pf, entry))
        uni, ptype, pname, psem, pdef = dm.groups()
        if uni:
            if ai < len(entry_args):
                call.append(expand_names(entry_args[ai], sampler_names))
                ai += 1
            elif pdef is not None:
                call.append(pdef)
            else:
                die("%s: no value for uniform parameter %s" % (entry, pname))
            continue
        sem = (psem or "").upper()
        if ptype in structs:
            prelude.append("%s nvr_in_%s = (%s)0;" % (ptype, pname, ptype))
            for mtype, mname, msem in structs[ptype]:
                if msem in ("TEXCOORD0", "TEXCOORD"):
                    prelude.append("nvr_in_%s.%s = %s;" % (pname, mname, "nvr_uv" if mtype == "float2" else "float4(nvr_uv, 0, 0)"))
                elif msem in ("POSITION", "POSITION0", "SV_POSITION"):
                    prelude.append("nvr_in_%s.%s = nvr_pos;" % (pname, mname))
                elif msem == "VPOS":
                    prelude.append("nvr_in_%s.%s = floor(nvr_pos.xy);" % (pname, mname))
                else:
                    warn("%s: input %s.%s (%s) has no source in a full-screen pass: 0" % (entry, ptype, mname, msem))
            call.append("nvr_in_" + pname)
        elif sem in ("TEXCOORD0", "TEXCOORD"):
            call.append("nvr_uv")
        elif sem == "VPOS":
            call.append("floor(nvr_pos.xy)")
        elif sem in ("POSITION", "POSITION0", "SV_POSITION"):
            call.append("nvr_pos")
        else:
            die("%s: parameter %s (%s) has no source in a full-screen pass" % (entry, pname, sem))
    if ai < len(entry_args):
        die("%s: technique passes %d arguments, %d used" % (entry, len(entry_args), ai))
    targets = 1
    if ret_type in structs:
        outs = [(m[1], int(re.sub(r"\D", "", m[2]) or 0)) for m in structs[ret_type] if m[2].startswith("COLOR") or m[2].startswith("SV_TARGET")]
        targets = max(o[1] for o in outs) + 1
        src.append("struct NvrSm6Out {" + " ".join("float4 o%d : SV_Target%d;" % (i, i) for i in range(targets)) + " };")
        src.append("NvrSm6Out nvr_main(float2 nvr_uv : TEXCOORD0, float4 nvr_pos : SV_Position) {")
        src += ["  " + l for l in prelude]
        src.append("  %s r = %s(%s);" % (ret_type, entry, ", ".join(call)))
        src.append("  NvrSm6Out o = (NvrSm6Out)0;")
        src += ["  o.o%d = r.%s;" % (idx, name) for name, idx in outs]
        src.append("  return o;\n}")
    else:
        src.append("float4 nvr_main(float2 nvr_uv : TEXCOORD0, float4 nvr_pos : SV_Position) : SV_Target0 {")
        src += ["  " + l for l in prelude]
        src.append("  return %s(%s);\n}" % (entry, ", ".join(call)))
    source = "\n".join(src) + "\n"

    # 6. compile
    os.makedirs(a.out, exist_ok=True)
    base = "%s.%s.%d%s" % (effect_name, a.technique if a.technique not in ("-", "") else "default", pidx, ("." + a.variant) if a.variant else "")
    hlsl_path = os.path.join(a.out, base + ".sm6.hlsl") if a.keep else os.path.join(a.out, "tmp-%d.hlsl" % os.getpid())
    open(hlsl_path, "w").write(source)
    spv_path = os.path.join(a.out, base + ".spv")
    r = subprocess.run([tool("glslang"), "-D", "-V", "-S", "frag", "-e", "main", "--source-entrypoint", "nvr_main", "--target-env", "vulkan1.1",
                        "--hlsl-offsets", "-o", spv_path, hlsl_path], capture_output=True)
    if r.returncode != 0:
        log = r.stdout.decode(errors="replace") + r.stderr.decode(errors="replace")
        if not a.keep:
            keep = os.path.join(a.out, base + ".sm6.hlsl")
            open(keep, "w").write(source)
            hlsl_path = keep
        die("glslang failed (rewritten source kept in %s):\n%s" % (hlsl_path, log))
    if not a.keep:
        os.remove(hlsl_path)

    # 7. SPIR-V: the sampler heap as a runtime array (DXVK's heap, as the GLSL ports declare it); reflection
    words = list(struct.unpack("<%dI" % (os.path.getsize(spv_path) // 4), open(spv_path, "rb").read()))
    if words[0] != 0x07230203:
        die("not SPIR-V")
    insts, i = [], 5
    while i < len(words):
        wc, op = words[i] >> 16, words[i] & 0xFFFF
        insts.append([op] + words[i + 1:i + wc])
        i += wc

    def sp_string(ws):
        b = struct.pack("<%dI" % len(ws), *ws)
        return b[:b.index(b"\0")].decode()

    names, member_names, member_offsets, decor, var_type, ptr_type = {}, {}, {}, {}, {}, {}
    for ins in insts:
        op = ins[0]
        if op == 5:
            names[ins[1]] = sp_string(ins[2:])
        elif op == 6:
            member_names[(ins[1], ins[2])] = sp_string(ins[3:])
        elif op == 72 and ins[3] == 35:
            member_offsets[(ins[1], ins[2])] = ins[4]
        elif op == 71:
            decor.setdefault(ins[1], {})[ins[2]] = ins[3:] if len(ins) > 3 else []
        elif op == 59:
            var_type[ins[2]] = ins[1]
        elif op == 32:
            ptr_type[ins[1]] = ins[3]
    by_name = {v: k for k, v in names.items()}
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
        out_words.append(((len(ins)) << 16) | ins[0])
        out_words += ins[1:]
    open(spv_path, "wb").write(struct.pack("<%dI" % len(out_words), *out_words))
    v = subprocess.run([tool("spirv-val"), "--target-env", "vulkan1.1", spv_path], capture_output=True)
    if v.returncode != 0:
        die("spirv-val: " + (v.stdout + v.stderr).decode(errors="replace"))
    # the uv must be location 0 (the built-in full-screen vertex shader's output)
    uvvar = by_name.get("nvr_uv") or by_name.get("nvr_main_nvr_uv")
    for vid, dd in decor.items():
        if names.get(vid, "").endswith("nvr_uv") and 30 in dd and dd[30][0] != 0:
            die("uv input landed on location %d, not 0" % dd[30][0])

    # 8. layout for NVR (core/Sm6Pass.cpp)
    lines = ["nvr-sm6 1",
             "source %s technique %s pass %d entry %s(%s) vs %s" % (os.path.basename(effect), a.technique or "-", pidx, entry,
                                                                   ",".join(entry_args), vsm.group(1) if vsm else "-"),
             "defines " + " ".join(defines)]
    for f in include_files(effect):
        rel = os.path.relpath(f, hash_root).replace("\\", "/")
        lines.append("hash %016x %s" % (fnv1a64(open(f, "rb").read()), rel))
    block = by_name.get("NvrSm6Globals")
    ubo_size = 0
    if uniforms and block is not None:
        for k, u in enumerate(uniforms):
            mname = member_names.get((block, k))
            if mname != u["name"]:
                die("uniform block member %d is %s, expected %s" % (k, mname, u["name"]))
            off = member_offsets[(block, k)]
            size, stride = type_size(u["base"], u["dims"], u["count"])
            ubo_size = max(ubo_size, off + size)
            lines.append("const %s %d %d %s%s%s" % (u["name"], off, size, u["base"], u["dims"], "[%d]" % u["count"] if u["count"] else ""))
            if u["default"]:
                lines.append("default %s %s" % (u["name"], " ".join("%.9g" % x for x in u["default"])))
        ubo_size = (ubo_size + 15) // 16 * 16
        lines.append("ubo 2 0 %d" % ubo_size)
    used = 0
    for s in samplers:
        present = by_name.get(s["name"] + "__t") is not None
        if present:
            used += 1
        lines.append("texture %s %d %s %d %s" % (s["name"], s["binding"], s["view"], s["slot"], "used" if present else "unused"))
    for state, slot in sorted(slot_of_state.items(), key=lambda x: x[1]):
        lines.append("sampler %d %s %s" % (slot, state[0], state[1]))
    lines.append("vs %s" % (vsm.group(1) if vsm else "-"))
    lines.append("states %d%s" % (len(states), "".join(" | " + " ".join(s.split()) for s in states)))
    lines.append("push %d" % (4 * nslots))
    lines.append("targets %d" % targets)
    open(os.path.join(a.out, base + ".txt"), "w").write("\n".join(lines) + "\n")
    print("ok %s: %d bytes SPIR-V, %d uniforms (%d bytes), %d textures (%d used), %d sampler states, %d target(s)" % (
        base, os.path.getsize(spv_path), len(uniforms), ubo_size, len(samplers), used, len(slot_of_state), targets))


if __name__ == "__main__":
    main()
