# tools/nvr-sm6: SPIR-V builds of NVR's ps_3_0 passes for the NVR DXVK fork

Two modes, one SPIR-V back end (glslang's HLSL front end, `--target-env vulkan1.1`, then spirv-val):

| Mode | Script | Input | Output | Read by |
|---|---|---|---|---|
| Effect | `hlsl2spv.py`, `build.py` (+ `passes.txt`) | an effect's `.fx.hlsl`, technique and pass | `Compute/Sm6/<Effect>.<Technique>.<Pass>.<key>.spv` + `.txt` | `src/core/Sm6Pass.cpp` |
| Template ("record mode") | `tpl2spv.py`, `build_deferred.py` | a game-shader record's preprocessed cache text, `Shaders\Cache\<sub>\<Name>.pso.hlsl` | `Compute/Deferred/<Name>-<key>.spv` + `.layout` | `src/core/Sm6Record.h` (record mode; `DeferredSun.h` VkDraw, switch `NvrDxvkDeferredResolve`; `docs/deferred-single-pass.md` step 1) |

Tools on Windows: Python 3.10 (`py -3.10 -I`) and the Vulkan SDK's `glslangValidator.exe`, `spirv-val.exe`, `spirv-cross.exe`,
`spirv-dis.exe`, found on PATH, in `%VULKAN_SDK%\Bin` or in `F:\Tools\VulkanSDK\1.3.296.0\Bin` (`hlsl2spv.tool`). On Linux/WSL as before.
glslang, not dxc: it is the fork's own build tool, the effect mode already uses it, and the scratch feasibility run proved it on all 9
deferred passes (dxc 1.8 with `-HV 2018` also runs, but would mean a second back end to keep in step).

## Template mode (record mode)

```
py -3.10 -I tools\nvr-sm6\build_deferred.py [--cache DIR]... [--lamps] [--out DIR] [--dry-run]
py -3.10 -I tools\nvr-sm6\tpl2spv.py <cache .pso.hlsl> --out DIR [--keep] [--key-only]
py -3.10 -I tests\sm6_record_check.py [--lamps] [--built src\hlsl\NewVegas\Compute\Deferred]
```

`build_deferred.py` converts every `Deferred\Sun` and `Deferred\SunFade` cache text in the given caches (default: this PC's MO2
overwrite `...\Shaders\VegasShaders\Shaders\Cache` and the `VegasShaders-1.1.0` mod's `CacheShipped`) into
`src\hlsl\NewVegas\Compute\Deferred` (deployed as `Data\Shaders\VegasShaders\Compute\Deferred\`). Only the variants those caches hold
are built: a player whose settings preprocess the pass to a different text has no file for that key and keeps the D3D9 pass.
A change to the pass's `.hlsl`, an include, a define or the shaders folder's name changes the text, so the key, so an old `.spv` is
never used for a new text (it falls back instead): rebuild from a cache the new build has written.

### The variant key (the C++ must compute the same)

`key` = FNV-1a 64 (offset basis `0xcbf29ce484222325`, prime `0x100000001b3`, byte by byte: `h = (h ^ byte) * prime`) over the bytes of
the cache text **with one trailing NUL removed** (D3DX's preprocess buffer keeps its terminator and `LoadShader` writes it to the file),
printed as 16 lower-case hex digits. Those bytes are exactly `ShaderProgram::PortableSource(source)` in `ShaderRecord::LoadShader`
(`src/core/ShaderRecord.cpp`: compared with the cache file at :682, written at :744-746), minus its last byte when that is `'\0'`. Every
load path has `source` (D3DX preprocess, or `ShaderPrecompile::Take`), so the record can store the key at load. `Sm6FileHash`
(`src/core/Sm6Pass.cpp`) is the same hash. File: `Compute\Deferred\<Name>-<key>.spv` and `.layout`, `<Name>` the record's name without
`.pso` (`DeferredSun` for both the Sun and the SunFade records: their texts, so keys, differ).

### What the SPIR-V expects

- **Uniform buffer** set 2, binding 0 (`ubo` line, `bytes` long): the D3D9 pixel-shader float register file, `cN` at byte `16 N`, then
  `float4 uvmap` and (with depth) `float4 ztest` in the registers after it, at the `uvmap` / `ztest` byte offsets (as built:
  `c<registers>` and `c<registers + 1>`). Fill `GetPixelShaderConstantF(0, data, registers)` as is, write uvmap / ztest at their
  offsets, upload `bytes`. Names that share registers in the D3D9 shader read the same bytes here (`alias=` / `group=` in the `const`
  lines), so the device's last write wins in both paths. `unused` (nothing read): declare and bind no buffer.
- **Textures** set 1, binding = D3D9 stage `N` = extension slot `N`, the stage's view type. Bind the device's texture on stage `N`, or a
  1x1 dummy when the stage is empty, holds a Draw target or a texture of another kind, or the pass did not set it for this draw (the
  caller says so: DeferredSun's s0 without the fog copy). `unused` stages: no binding. Only a 2D dummy exists: a cube or 3D stage
  that needs one keeps that frame on D3D9 (`Sm6Record.h`).
- **Samplers**: DXVK's sampler heap, set 0 binding 0 (`heap`). For every `sampler <stage> <slot> <pushOffset>` line, declare an
  `NVR_EXT_BIND_SAMPLER` binding (Slot = slot, PushOffset = pushOffset) and bind the extension sampler matching the device's sampler
  states on that stage (MIN/MAG/MIPFILTER -> `NVR_EXT_FILTER`, ADDRESSU -> `NVR_EXT_ADDRESS`; SRGBTEXTURE -> the texture binding's
  `Srgb`). The shader masks the heap index with `0xFFFF`. As built (`Sm6Record.h` DeviceSampler): ANISOTROPIC on min or mag ->
  ANISOTROPIC; else LINEAR (or above) on min or mag -> LINEAR, or LINEAR_MIPMAPS with a MIPFILTER (both LINEAR before extension 1.3);
  else NEAREST. ADDRESSU must equal ADDRESSV, BORDER needs a zero border colour, MIRRORONCE has no match: those keep that frame on
  D3D9. Samplers are made through `DxvkCompute::CreateSamplerNoFail` (never `Fail`).
- **Push block** (`push` bytes, offset 0): `uint s[n]`, the used stages' sampler heap indices only (written by the extension), one
  dword per used stage in stage order (each `sampler` line's `pushOffset`); `n` = 1 when no sampler is used. At most **48 bytes**:
  `tpl2spv.py` refuses a longer one and `Sm6Record.h` refuses to load it.
  - Why (2026-11-22, proven by the parity probe and DXVK v3.0.1's source): DXVK keeps push data in a 256-byte array and puts a fragment
    shader's at byte 192 (`DxvkPushDataBlock::computeBlockOffsetForStage`), so a fragment push block has 64 bytes before it runs past
    that array into the next one, where DXVK writes each sampler binding's uint16 heap index at its push offset. Layout 1 pushed 96
    bytes (`uint s[16]; float4 uvmap; float4 ztest`): stages 0-7's heap indices replaced the low 16 bits of uvmap.xy and ztest.xyz,
    and the sun pass read the G-buffer up to 7 x 3 pixels off its own pixel.
  - 48 vs 64: 64 is where the overflow starts in the fork's merged ("packed") layouts; 48 (`256 - 16 reserved - 192`) is what DXVK
    gives the fragment stage in an "Independent" (pipeline-library) layout, which truncates the block there. The fork's full-screen
    pipelines are believed not to use Independent layouts (patch 0004), but every pass built so far fits 48 (most: DeferredLampLandFilm,
    12 used samplers), so 48 is the limit. A future pass using 13 or more samplers needs either 64 (and proof the layout is merged)
    or its samplers merged.
- **uvmap / ztest** (uniform buffer, above):
  - `uvmap`: the pass's `TEXCOORD0` is `FragCoord.xy * uvmap.xy + uvmap.zw`; the built-in triangle's uv is not read. For a D3D9
    `XYZRHW` quad with corners `(x0, y0, u0, v0)` and `(x1, y1, u1, v1)`: `uvmap.x = (u1 - u0) / (x1 - x0)`,
    `uvmap.z = u0 - (x0 + 0.5) * uvmap.x` (y / w the same). DeferredSun's quad (`DeferredSun.h:518-519`): `(1/W, 1/H, 0, 0)`, W x H
    the target's size, whatever the viewport.
  - `VPOS` is `FragCoord.xy - 0.5`: D3D9's whole-number pixel centres in target pixels. Nothing to supply.
  - `ztest` (only with `depth 1`): the D3D9 pass's depth test done in the shader against texture `zref`. `x`: 0 off, 1 `zref` holds
    view z (R32F, `DeferredDepth::LinearTexture`; reference depth = `y + z / zref`, with `y` = projection `_33`, `z` = `_43`), 2 `zref`
    holds device depth (the INTZ copy); `w`: 1 pass where the pass's depth `>=` the reference (D3D9 GREATEREQUAL, reversed depth),
    2 `<=` (LESSEQUAL). With x = 0 the pass writes `SV_Depth` and the C++ attaches the device's depth-stencil surface through `DrawGeometryScene` (the fork's 1.7-grass; not `DrawDepthStencil`, whose viewport range is squashed to its single `Depth` value, patch 0010).
- **Outputs**: `targets` colour targets (`COLORn` -> location n), `depth 1` = also `SV_Depth`. Every channel of every target is written
  (the extension has one blend and no per-target mask); a pass whose D3D9 draw masks alpha must return alpha 0 under an ADD blend.
- **Viewport**: the Draw's viewport (and so its scissor) is the device viewport; the shader's uv and VPOS do not depend on it.

### The .layout format (`nvr-sm6-record 2`)

Text, one record per line, whitespace-separated tokens, the tag first; `#` starts a comment line; unknown tags are to be ignored.

| Line | Meaning |
|---|---|
| `nvr-sm6-record 2` | format and version (first line; 1 had uvmap / ztest in the push block, refused now) |
| `pass <Name>` / `key <16 hex>` / `from <cache path>` | what it was built from (the key also in the file name) |
| `registers <N>` | float registers the pass declares: c0..c(N-1) |
| `ubo <set> <binding> <slot> <bytes> <used\|unused>` | the register-file buffer (`bytes` = 16 x (N + 1, or + 2 with depth): the registers, uvmap, ztest) |
| `heap <set> <binding>` | the sampler heap |
| `push <bytes>` | push block size: 4 x the used samplers (at least 4), at most 48 |
| `uvmap <offset>` / `ztest <offset>` | uniform-buffer byte offsets of those float4s, after the registers (`ztest` only with depth) |
| `texture <stage> <set> <binding> <slot> <2D\|3D\|CUBE> <used\|unused> <name>[,<name>...]` | one per D3D9 sampler stage the text declares |
| `sampler <stage> <slot> <pushOffset>` | one per used texture stage: its device sampler state's heap index, at push dword 0, 1, ... in stage order |
| `zref <set> <binding> <slot> 2D <used\|unused>` | the depth-test reference texture (only with depth) |
| `const <name> <register> <count> <type> <row_major\|column_major\|-> <member\|alias=<name>\|group=<array>>` | every float constant: first register, registers taken |
| `group <array> <register> <count>` | a float4 array standing for names whose registers overlap |
| `targets <1-4>` / `depth <0\|1>` | colour outputs; SV_Depth written |

Refused at conversion (the pass keeps D3D9): constants or samplers without a `register()`, `b`/`i` registers, int/bool constants in
`c` registers, an aliased constant array used without an index, techniques, `main` inputs other than TEXCOORD0 and VPOS.
