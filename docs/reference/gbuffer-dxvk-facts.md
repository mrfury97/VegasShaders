# G-buffer and DXVK limits (facts from the room's work, 2026-11-18 to 2026-11-22)

> Technical reference. Moved from the `vs-gbuffer-dxvk-facts` skill on 2026-11-25; proven/believed labels kept. Read it before adding push
> constants or samplers to a DXVK-fork pass, growing a land or lighting-free shader, or reading COLOR2 / albedo for grass or land.

Each fact says proven (with the evidence) or believed. Layout of the G-buffer itself: [../deferred-vs-forward.md](../deferred-vs-forward.md); wetness
design: [../deferred-wetness-design.md](../deferred-wetness-design.md); reasons behind the X4505 work: [../deferred-roadmap.md](../deferred-roadmap.md).

## DXVK push blocks: two different limits (checked against this repo 2026-11-25)
- **Proven in this repo (`src/core/NvrDxvkExt.h:55`):** the NVR extension's own passes (compute and full-screen graphics passes made through
  the extension) declare `NVR_EXT_MAX_PUSH_BYTES 240`; their push block starts at offset 0 (`NvrDxvkExt.h:27`, `:187-190`) and the
  host copies the data over the whole block. Examples in code: the RtDenoise blocks of 200 / 216 / 48 bytes (`RtDenoise.cpp:167`).
  The agent `lead-architect` checks against this 240.
- **Proven from the repo's own patch text (`tools/nvr-dxvk/patches/0004-NVR-extension-no-pipeline-library-for-a-stage-whose-.patch`,
  commit message and comment):** in DXVK's independent (graphics pipeline library) layout the fragment stage gets a fixed 48-byte slot at
  offset 192 of the context's push data; the extension's full-screen passes declare up to 240 bytes, so a stage whose block is larger
  than its slot is never fast-linked: it always gets a complete pipeline. That is why 240 is safe for the extension's own passes and
  why a replaced D3D9 game shader's fork-side fragment block must stay within 48 bytes (from offset 192; 192 + 48 = 240 of the 256).
- **Not re-read from DXVK source:** the claim of one shared 256-byte `constantData` block for all D3D9 stages (below) comes from the
  2026-11-22 debugging; the DXVK source is not in this repo (`tools/nvr-dxvk` holds only patches), so the 256 total is believed. The
  repo agrees on 192 and 48.
- Summary: extension passes (complete pipeline): up to 240 bytes from offset 0. Replaced D3D9 pixel shaders in the pipeline-library
  layout: 48 bytes from offset 192. Open: nothing about the numbers; only the 256 total is unchecked here.

### DXVK push block: 256 bytes, fragment block at 192 (D3D9 draws)
- **Proven (2026-11-22, branch `deferred-single-pass`, tip 5d4e0b1).** DXVK keeps one 256-byte `constantData` push block for all
  stages; the fragment stage's part starts at byte 192. The fork's 96-byte FRAGMENT push block ran past the end, and DXVK's uint16
  sampler heap indices overwrote the low halves of `uvmap` / `ztest`, so G-buffer reads landed up to 7x3 pixels off.
- Fix that works: move non-sampler data (uvmap, ztest) into the UBO and keep the push block to compacted sampler dwords, at most
  48 bytes (layout format 2). Same-frame parity probe (`tests/parity_probe.py`, value 14): mean and max |A-B| 0 at DebugView 5.
- **Believed, open:** VolumetricFogApply / VolumetricFogEstimate push 112 bytes with samplers at 0/4/8 and likely corrupt the low bits
  of `compositeAOTexel` the same way.
- Rule: before adding to a fragment push block, add up its bytes from offset 192; anything past 256 is silently clobbered.

## ps_3_0 temp-register wall on land (X4505)
- **Proven (2026-11-18 game log and offline compiles).** With Terrain Blending, the 5-7-layer land shaders fail to compile with X4505
  (out of temp registers) and fall back to the game's shader: that is the "pale ground" seen in game, not a prototype bug.
- **Proven (offline, 2026-11-18):** forward land records use temp r31 (all 32) at 3,842-4,600 instruction slots; the lighting-free
  (deferred) records peak at r29-r30 with 2,738-3,178 slots, before Terrain Blending adds its part. So a lighting-free record alone
  may not rescue the Terrain Blending case; shortening the shader (DeferredStandalone rescued 12 shaders in the 2026-11-18 log; puddle streaks out) or moving work
  to a deferred pass makes the room.
- Removing the puddle wall streaks saved about 175 instruction slots on the 6-layer land, with the temp peak unchanged: slots and
  temps are separate limits; check both (`template_compile`, see the `vs-build-deploy` skill).
- ps_3_0 limits (32 temps, instruction slots, no dynamic indexing) do not exist in compute or SPIR-V replacements; the D3D9 path keeps
  the split passes as the fallback.

## Grass writes no G-buffer record
- **Proven (fix 40acc53, deferred wetness; RT GI review).** Grass draws no lighting-free record, so it writes neither the material
  (COLOR2) nor the albedo buffer for its pixels: whatever the ground wrote stays under the blades. The deferred wetness film made
  grass mirror-like until 40acc53 fixed it. The RT GI review hit the same fact: a sharper blur exposed grass grain, because grass
  never writes the albedo buffer.
- Use: "no albedo written" is how to find grass pixels in a deferred pass; never treat COLOR2 / albedo under grass as the grass's own.

## Land carries its height in COLOR2.r, only between record and pass A
- **Proven (deferred wetness step 1, `TerrainTemplate.hlsl` near line 896, design doc "Height").** Every fraction of COLOR2.r is
  already read (metal as `/ 0.49` by DeferredSun, DeferredLamps, the flashlight, the path tracer, the DLSS guides; the far land's
  1/64 mark by DeferredSun), so there is no spare channel. The near land's lighting-free record writes r = 4 + 0.99 x height (the
  specular bit 16 taken out, so fp16 keeps 1/256 steps; at 20 it would be 1/64), and DeferredWetness pass A writes r = 20 (= 4 + 16)
  back for every such pixel, wet or dry (a restore-only record in dry frames, and a fallback restore on every DeferredSun return that
  skipped A).
- Consequence: any pass that reads COLOR2.r between the land records and pass A sees height, not metal. Ruled out: a separate
  water-layer height channel in the G-buffer.
