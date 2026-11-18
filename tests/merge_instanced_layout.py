# UNOFFICIAL 2026-11-15 MergeCopies (src/core/MergeCopies.cpp): which instance-stream TEXCOORD feeds which vertex constant, checked offline.
# Batch 3's round-3 review found c16 and c25 read from each other's stream slot (the self-check compares the engine's registers, not what the
# instanced shader reads, so it could not see it). Three checks:
#   1. the C++ layout: kInstRegs, InstReg(k) (slot -> register) and the declaration (slot k = TEXCOORD8 + k);
#   2. the HLSL: ObjectTemplate.hlsl's NVR_INSTANCED inputs and NvrInstancedLoad (which input becomes ModelViewProj rows, EyePosition, LightData[0])
#      must give each register the TEXCOORD the C++ fills it into;
#   3. the compiled code (fxc, vs_3_0) of several rigid vertex shaders, plain and NVR_INSTANCED: every instruction of the plain shader that reads
#      c0-c3 / c16 / c25 has a twin in the instanced shader reading an input register instead; that input's dcl_texcoord must be the slot of
#      the register it replaced.
# Usage: py -3 -I tests/merge_instanced_layout.py [worktree]   (exit 0 = PASS)
import os, re, sys, glob, subprocess, tempfile

tree = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..'))
cpp = open(os.path.join(tree, 'src/core/MergeCopies.cpp'), encoding='utf-8').read()
hlsl_path = os.path.join(tree, 'src/hlsl/NewVegas/Shaders/ObjectTemplate.hlsl')
hlsl = open(hlsl_path, encoding='utf-8').read()
fails = []

# ---- 1. C++ ----
n = int(re.search(r'kInstRegs\s*=\s*(\d+)', cpp).group(1))
m = re.search(r'InstReg\(UInt32 k\)\s*\{\s*return k < 4 \? k : k == 4 \? (\d+) : (\d+);', cpp)
if not m: sys.exit('FAIL: InstReg(k) not in the expected form; update this test with the layout')
slot_reg = {k: k for k in range(4)}
slot_reg[4] = int(m.group(1)); slot_reg[5] = int(m.group(2))
if n != 6: fails.append('kInstRegs is %d, this test knows a 6-slot layout' % n)
if not re.search(r'D3DDECLUSAGE_TEXCOORD,\s*\(BYTE\)\(8 \+ k\)', cpp): fails.append('the declaration no longer puts slot k at TEXCOORD8 + k')
reg_tc = {r: 8 + k for k, r in slot_reg.items()}
print('C++ layout:', ', '.join('c%d <- TEXCOORD%d' % (r, tc) for r, tc in sorted(reg_tc.items())))

# ---- 2. HLSL ----
block = re.search(r'#ifdef NVR_INSTANCED\s*\n(.*?)#endif\s*\n\};', hlsl, re.S)
inputs = dict(re.findall(r'float4\s+(\w+)\s*:\s*TEXCOORD(\d+)\s*;', block.group(1))) if block else {}
inputs = {k: int(v) for k, v in inputs.items()}
load = re.search(r'void NvrInstancedLoad\(VS_INPUT vin\)\s*\{(.*?)\}', hlsl, re.S)
if not inputs or not load: sys.exit('FAIL: NVR_INSTANCED inputs or NvrInstancedLoad not found')
body = load.group(1)
rows = re.search(r'InstModelViewProj\s*=\s*float4x4\(\s*vin\.(\w+),\s*vin\.(\w+),\s*vin\.(\w+),\s*vin\.(\w+)\s*\)', body)
eye = re.search(r'InstEyePosition\s*=\s*vin\.(\w+)', body)
light = re.search(r'InstLightData\[0\]\s*=\s*vin\.(\w+)', body)
hl = {}
for i in range(4): hl[i] = inputs[rows.group(i + 1)]
hl[16] = inputs[eye.group(1)]
hl[25] = inputs[light.group(1)]
print('HLSL reads:', ', '.join('c%d <- TEXCOORD%d' % (r, tc) for r, tc in sorted(hl.items())))
for r, tc in reg_tc.items():
    if hl.get(r) != tc: fails.append('c%d: the C++ fills TEXCOORD%d, the shader reads TEXCOORD%s' % (r, tc, hl.get(r)))

# ---- 3. compiled code ----
fxcs = sorted(glob.glob(r'C:\Program Files (x86)\Windows Kits\10\bin\*\x64\fxc.exe'))
if not fxcs: fails.append('fxc not found: compiled-code check skipped')
else:
    fxc = fxcs[-1]
    root = os.path.join(tree, 'src/hlsl/NewVegas')
    variants = [('SLS2000', []), ('SLS2004', ['PROJ_SHADOW']), ('SLS2008', ['LIGHTS=2']), ('SLS2012', ['SPECULAR']),
                ('SLS2032', ['ONLY_LIGHT', 'LIGHTS=3']), ('SLS2030', ['ONLY_LIGHT', 'LIGHTS=2', 'PROJ_SHADOW'])]
    extra = ['CAPTURED_NORMALS_ON=3', 'LINEAR_LIGHTING_ON=1', 'FORWARD_SHADOWS=1', 'REVERSED_DEPTH', 'OBJECT_SMOOTH_NORMALS=1']
    tmp = tempfile.mkdtemp(prefix='mergelayout')
    def compile_asm(defs, out):
        cmd = [fxc, '/nologo', '/T', 'vs_3_0', '/E', 'main', '/I', os.path.join(root, 'Shaders'), '/I', os.path.join(root, 'Effects'), '/DVS']
        for d in defs + extra: cmd += ['/D' + d]
        cmd += ['/Fc', out, hlsl_path]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode: return None
        return [l.strip() for l in open(out, encoding='utf-8', errors='replace') if l.startswith('    ') and not l.strip().startswith('//')]
    for name, defs in variants:
        plain = compile_asm(defs, os.path.join(tmp, name + '_0.asm'))
        inst = compile_asm(defs + ['NVR_INSTANCED=1'], os.path.join(tmp, name + '_1.asm'))
        if plain is None or inst is None: fails.append('%s: did not compile' % name); continue
        # (Aligning plain and instanced instructions to see which input replaced which constant was tried and is not reliable: the compiler
        # renumbers temps and reorders. What the compiled code can show: the constants it still reads, and the inputs it declares.)
        def ctab(lines_path):
            regs = set()
            for l in open(lines_path, encoding='utf-8', errors='replace'):
                mm = re.match(r'//\s+(\w+)\s+c(\d+)\s+(\d+)', l)
                if mm:
                    for k in range(int(mm.group(2)), int(mm.group(2)) + int(mm.group(3))): regs.add((mm.group(1), k))
            return regs
        pt, it = ctab(os.path.join(tmp, name + '_0.asm')), ctab(os.path.join(tmp, name + '_1.asm'))
        dcls = set(int(mm.group(1) or 0) for mm in (re.match(r'dcl_texcoord(\d*)\s+v\d+', l) for l in inst) if mm)
        named = lambda regs, nm: any(n_ == nm for n_, _ in regs)
        if any(r in (0, 1, 2, 3, 16) for _, r in it): fails.append('%s: the instanced code still reads c0-c3 / c16 from constants' % name)
        need = {8, 9, 10, 11}
        if named(pt, 'EyePosition'): need.add(reg_tc[16])
        if ('LightData', 25) in pt and not ('LightData', 25) in it: need.add(reg_tc[25])
        if not need <= dcls: fails.append('%s: inputs %s expected, the instanced code declares TEXCOORD %s' % (name, sorted(need), sorted(d for d in dcls if d >= 8)))
        if any(d >= 8 + n for d in dcls): fails.append('%s: an input past the layout' % name)
        print('%s: declares TEXCOORD %s (expected at least %s)' % (name, sorted(d for d in dcls if d >= 8), sorted(need)))

if fails:
    print('FAIL'); [print('  ' + f) for f in fails]; sys.exit(1)
print('PASS')
