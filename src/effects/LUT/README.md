# LUT

Source reviewed (wave 3, 2026-11-02): the new D3D9 effect src/hlsl/NewVegas/Effects/LUT.fx.hlsl and Vulkan src/hlsl/NewVegas/Compute/LUT.frag
(branch claude/rw3-colour); C++ src/effects/LUT.cpp, LUT.h, LUTFile.h, LUTIdentity.h (unchanged); settings in
resource/VegasShaders.defaults.toml; frame order in src/core/ShaderManager.cpp.

## 1. Purpose

LUT (look-up table) colour grading. Each colour of the picture is replaced by the colour stored for it in a table image
or a 3D .cube file, so a whole colour look can be swapped by changing a file. Exteriors blend a day table and a night
table by the time of day; interiors use their own table. The grade is mixed over the original by a strength setting. The
shipped table is an identity (no change), so the effect does nothing until a user picks a table.

## 2. When it runs

- Default position: post-tonemap, in the screen-space colour block, right after Coloring (and not run at all there when
  Coloring has taken it over, see the Coloring spec). Order: ... barrel haze, Coloring, LUT, depth of field, ...
- Pre-tonemap option: with [Shaders.LUT.Main] PreTonemapping = true, LUT is not run post-tonemap. It runs in the pre-tonemap
  chain (see the Bloom spec, section 2), after the lens pass and before the game's tonemap, on the HDR picture. The HDR
  compatibility option matters then (section 5).
- Enabled: [Shaders.LUT.Status] Enabled (bool, default true).
- Gates: [Main.Main] RenderEffects on, a player cell present, overlay off, and ShouldRender (below).
- ShouldRender (the effect's own test, checked before any drawing):
  - Strength is 0 or less: does not run.
  - Exterior: runs unless both the day and the night table are identity tables.
  - Interior: runs unless the interior table is an identity table.
- Exterior and interior: chosen per frame from the game state. Exteriors blend day and night by the day/night transition
  curve. Interiors use the interior table only (a hard switch when entering or leaving a cell).

## 3. Inputs

### Textures

| Slot | Name | Holds |
|---|---|---|
| s0 | TESR_RenderedBuffer | the current picture |
| s1 | TESR_LUTDayBuffer | the day table (a strip texture, see section 6) |
| s2 | TESR_LUTNightBuffer | the night table |
| s3 | TESR_LUTInteriorBuffer | the interior table |

All four are LINEAR, CLAMP, no mips.

### Constants (registered by name; no fixed registers)

- TESR_LUTData (float4): x day table cell count N (0 means no usable table); y Strength (not clamped); z night N; w interior N.
- TESR_LUTBlend (float4): x day/night blend in exteriors (1 is full day, 0 is full night; 0 in interiors); y interior flag
  (0 exterior, 1 interior); z HDR compatibility flag (1 when the HDRCompat setting is on); w unused.

Units: colour values 0..1 for LDR input; HDR input can be above 1 (section 5).

## 4. Settings

[Shaders.LUT.Main]:
- DayLUT (string, default 'neutral_lut.png'): file name in Data/Textures/VegasShaders/LUTs/. Accepts a .cube file, or a PNG,
  DDS or BMP image that is a horizontal strip N cells wide and N high (N * N wide by N high, for example 256 by 16, 1024 by 32,
  4096 by 64). Other shapes are ignored and logged.
- NightLUT, InteriorLUT: the same rules.
- Strength (float, default 1.0): blend between the original picture (0) and the full grade (1).
- PreTonemapping (bool, default false): apply the table before the game's tonemap. Needs HDR tables, or HDRCompat on.
- HDRCompat (bool, default true): normalise HDR input before the table lookup (section 5).

[Shaders.LUT.Status]:
- Enabled (bool, default true): "Applies a color lookup table. Ships with a neutral LUT so the default is a no-op until you swap in
  your own."

Each slot name is saved back to the settings when a table is picked from the in-game menu. Unchanged by the rewrite.

## 5. Passes and outputs

One technique (index 0, the only one), one pass. Full-screen quad through the frame vertex buffer, writing the frame chain's target
(or the game target when this is the chain's final effect). Output is the same size as the render, alpha 1, opaque.

1. HDR compatibility (only when TESR_LUTBlend.z is 1): the picture is divided by the larger of 1 and its largest channel, so the
   table is always sampled with values at or below 1. The scale is undone after the lookup. For LDR input the scale is 1.
2. Lookup in each table (section 6). A table with a cell count below 2 is not sampled at all; its colour is the input colour, so
   the input passes through for that table.
3. Exteriors: day and night are blended by TESR_LUTBlend.x (day when 1). Interiors use the interior colour. The two are combined
   by TESR_LUTBlend.y (0 keeps the exterior colour, 1 gives the interior colour).
4. The HDR scale is restored (only when the HDR compatibility flag is 1).
5. Output = the original picture blended toward the graded colour by TESR_LUTData.y (Strength), channel by channel. Not clamped.

## 6. Behaviour details

### Table layout and loading (unchanged)

- Strip layout: the table is N square cells in a row. Within a cell, red runs along x and green along y (N - 1 steps each, with a
  half-texel centre offset). Blue selects the cell: the lookup takes the two cells on either side of the blue position and blends
  them by its fraction. A 256 by 16 strip has 16 cells of 16 by 16.
- .cube files: a 3D table of N cubed lines, red fastest, then green, then blue. A 1D table, an input range other than 0..1, or a wrong
  entry count is rejected with a logged reason. A valid .cube is converted to a strip of N cells in 8-bit (A8R8G8B8).
- Image tables (PNG, DDS, BMP) are loaded at their exact size.
- Each slot keeps its own cell count.
- A texture that is not a valid strip (width not height squared, or height below 2) is not used at all, and a log line says so.
- Identity detection: a table in which every texel matches the identity mapping within one 8-bit step is flagged neutral. ShouldRender
  uses the flags (section 2).

### Runtime behaviour

- Sampling is linear inside a cell and between the two neighbouring cells.
- Values above 1 in the input (HDR, PreTonemapping on, HDRCompat off): the lookup clamps to the table edge, so the output is the edge
  colour, not an extrapolation. HDRCompat on avoids this.
- Strength of 0 gives the original picture exactly; ShouldRender also skips the pass.
- Interiors: the interior table alone is used.
- Sky, depth, first-person weapon, TAA jitter: none handled.
- RenderScale (rect frames): no rect handling in the D3D9 pass (it reads the whole target, as other full-screen effects do; see section 9).
- No dithering in the pass itself. The 8-bit table can band in smooth gradients; the dither effects handle that separately.

## 7. Vulkan path

- Choice: [Main.Main.Performance] NvrDxvkPasses (default true) with the Vegas Shaders DXVK extension present. Technique 0 only; any other index
  is refused and falls back to the D3D9 technique for the session.
- File: src/hlsl/NewVegas/Compute/LUT.frag (the same maths, line for line, as the D3D9 pass).
- Bindings (set 1): binding 0 = the picture; bindings 1, 2, 3 = the day, night and interior tables. An unusable slot is bound to the
  picture and not read.
- One LINEAR CLAMP sampler, its heap index in the push block.
- Push block (36 bytes plus padding): 0 TESR_LUTData (vec4); 16 TESR_LUTBlend (vec4); 32 sampler heap index (uint); 36..47 padding.

## 8. Look and cost

- Correct result: with the neutral table the picture is unchanged (and the pass is skipped). With a real table, the picture takes the
  table's look; at Strength 0.5 it is halfway. In exteriors the look moves from the night table to the day table with the time of day.
- Typical wrong results:
  - Banding or steps in gradients: the 8-bit table precision, or a table of too few cells.
  - A table that scrambles colours: a wrong layout (a strip that is not N cells of N by N, which is ignored), or a .cube in the wrong order.
  - A look in the wrong place: a day table used for the night, or an interior table that is not neutral used by mistake.
  - Odd colours at high brightness: PreTonemapping on with HDRCompat off.
- Cost class: single pass, texture-read heavy but cheap. Per pixel: one read of the picture and two reads per usable table (up to seven
  reads for three tables). Unusable tables are no longer sampled (wave 3).

## 9. Open questions

Changes made in wave 3 (2026-11-02):
- CHANGED (no visible change): a table with a cell count below 2 is no longer sampled; its colour is the input, as before.
- The shader no longer samples a table that is not used in the frame (exterior uses day and night; interior uses the interior table).

Still open:
1. Strength is not clamped in the C++ or the shader, so a value above 1 extrapolates the grade beyond the table. Whether that is wanted
   was not established from the code (the toml says 0 to 1 in words). Kept as it was.
2. The pre-tonemapping path runs on HDR values before the tonemap compresses them; the effect on the picture was not checked.
3. A table picked in the menu that fails to load keeps or drops the previously assigned table: not traced.
4. RenderScale: no rect handling in the D3D9 pass or the Vulkan pass. Not checked in a rect frame. (Other effects clamp their taps to
   the drawn part; this one reads one texel per pixel, so it has no neighbour reads that could leave the drawn part.)
5. The interaction with Coloring's combined technique depends on the LUT's constants being set before Coloring's pass; the update order
   is in src/core/ShaderManager.cpp and was not traced again here (see the Coloring spec).
6. (2026-11-17) A table name from a setting, a preset or the NVRSetSettingS command is a file in the LUT folder or below it: a name
   with "..", a drive letter or a leading slash is not loaded (`LUTEffect::SafeName`). Before, such a name opened any file on the drive.
