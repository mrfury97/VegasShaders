# Particles - lit and soft particles

UNOFFICIAL. Written 2026-11-09 (roadmap track 3, steps 1 and 2); the code is older (lit particles 2026-10, soft particles 2026-11-04).

## 1. Purpose

The game draws smoke, dust, blood and debris particle systems unlit: they keep full brightness in shade, at night and indoors. This
collection replaces their shaders so they take the scene's light, and makes every particle and unlit effect mesh fade where it meets
the scene behind it (soft particles) instead of cutting a hard line.

## 2. When it runs

- `[Shaders.Particles.Status] Enabled` (default false). Read at startup for the soft-particle parts (the depth copy texture,
  `core/ParticleSize.h`'s vertex hook); the lighting follows the switch live (`ShaderCollection::SwitchShader`).
- `Strength` above 0 for the lighting; `SoftScale` / `MeshSoftScale` above 0 for the fade.
- With "Vanilla Plus Particles" loaded (`VanillaPlusParticles.dll`), `ParticleSize` is not installed: particle systems get no fade
  (meshes still do, and the lighting works).

## 3. How it works

Shaders (all game-shader replacements, loaded at runtime from `Shaders\VegasShaders\Shaders\`; see SHADERS.txt):
- `NOLIGHT016.vso` / `NOLIGHT017.vso` (particle systems, plain and texture-sheet): the game's vs_2_0 shaders ported, plus the
  camera-relative world position (with the shadow sentinel), the vertex light (`ParticleVertexLight`: ambient + the 24 tracked lamps,
  unshadowed, radius falloff) and the view depth + particle size for the fade.
- `NOLIGHT006.vso` (falloff cards): the same outputs, vertexLight.w = 2.
- `NOLIGHTTEXVC.pso`: texture x vertex colour, the game's fog modes, then `ParticleLight`: vertex light + the sun through the forward sun
  shadow (`GetSunShadow`) x `SunShare` x the sun phase (below). Lit only behind the particle vertex shaders (the sentinel), and falloff
  cards only when normally blended: additive draws (muzzle and impact flashes, sparks, glows: the game marks them with the
  fade-to-black fog mode) stay exactly as the game draws them. `SetShadersHook` gives draws with the game's own vs_2_0 vertex shaders the
  game's pixel shader back (D3D9 cannot pair ps_3_0 with vs_2_0).
- `GDECAL.vso` / `GDECALS.vso` / `GDECAL.pso` (blood decals on characters): the same light, without the sun phase.
- `Includes/SoftParticle.hlsl`: alpha x saturate((scene depth - particle depth) / (size x SoftScale)), the scene depth from a copy of
  the world's depth taken at the first alpha-blended NoLighting draw of the world scene (`ParticleShaders::ForDraw`).

### Sun phase (2026-11-09, `SunPhase`)

A thin particle scatters the sun toward the camera more when the sun is behind it. `ParticleSunPhase` multiplies the sun term by the
Henyey-Greenstein phase on the angle between the sun's light and the view ray, divided by its isotropic value 1/(4 pi):

    P(cos) = (1 - g^2) / (1 + g^2 - 2 g cos)^1.5,   cos = dot(toward the sun, camera -> particle)

Its average over all view directions is exactly 1, so the average brightness stays the flat `SunShare`'s. g > 0: smoke and dust glow
looking toward the sun (peak (1+g)/(1-g)^2: 2.65 at g 0.3, 10 at 0.6) and read darker from the sun's side (minimum (1-g)/(1+g)^2:
0.41 at g 0.3). g = 0 is skipped by a branch: exactly the old shader path. The direction is `ShaderConst.SunDir` (the sun disc by
day, the game's directional light, i.e. the moon, at night). Only in `NOLIGHTTEXVC.pso` (`PARTICLE_SUN_PHASE`); the blood decals
compile byte-identical to before.

## 4. Settings ([Shaders.Particles.Main])

| key | default | what |
|---|---|---|
| Strength | 1.0 | 0 = the game's unlit look, 1 = fully lit |
| SunShare | 0.6 | share of the sun a particle catches outdoors |
| SunPhase | 0.3 | Henyey-Greenstein g of the sun on particles, [-0.9, 0.9]; 0 = the old flat share |
| SoftScale | 0.25 | particle systems: fade over their size x this; 0 = off |
| MeshSoftScale | 0.25 | unlit effect meshes: fade over their bound radius x this; 0 = off |

## 5. Constants

| name | register (ps / vs) | components |
|---|---|---|
| TESR_ParticleData | c168 / c200 | x Strength, y 1, z SunShare, w 1 outdoors |
| TESR_ParticleAmbient | - / c201 | rgb ambient (the sky's outdoors, the cell's indoors) |
| TESR_ParticlePhase | c193 (NOLIGHTTEXVC only) | xyz toward the sun (world), w SunPhase |
| TESR_SunColor | c192 (ps) | the weather's sun colour (c170 until 2026-11-09: overwritten per pass, see section 9) |
| TESR_ParticleSoft | c175 | x soft scale for this draw (0 off), yz 1 / resolution |
| NVR_SoftSize | vs c41, set per draw | x mesh size, y 1 when alpha-blended |

## 6. Vulkan path

None of its own: the game shaders run through DXVK as any D3D9 shader. RT / PT do not trace particles.

## 7. What right and wrong look like

Right: smoke and dust darker in shade and at night, warm near fires and lamps, brighter and glowing when looking toward a low sun
(SunPhase > 0), dimmer with the sun behind the camera; no hard line where smoke meets the ground; flashes, sparks, glows and flames
exactly as with the switch off.
Wrong: particles black or pure white (a missing constant), additive flashes dimmed (the pairing guard broke), a halo or speckled outline
of object edges through smoke with game MSAA on (the depth copy), smoke vanishing near the ground over a long distance (SoftScale too
high for big particles).

## 8. Measured

See docs/next/PARTICLES-BENCH-2026-11-09.md (pictures off / on / phase, MSAA 0 and 4, frame totals).

## 9. Known problems (bench 2026-11-09)

- Fixed: the sun never reached particles or blood decals. `TESR_SunColor` sat in pixel c170, which `HudRaindrops::SetDrawConstants`
  writes on every geometry pass (zeros when dry) after the shader's constants are uploaded, so the sun term was always black. Moved to
  c192 (the phase to c193: BarrelHeat writes c171-c173 per draw). Pick pinned pixel registers for these shaders outside every per-draw
  write: c168-c175, c180-c191, c194, c197-c198, c204, c206-c222 are written per draw by some hook.
- Open (why it is still off by default): normally blended glow meshes take the lit path like smoke. The Strip's night light dome goes
  dark, and its steam turns purple. The mesh soft fade (MeshSoftScale x bound radius) cuts big glow meshes where geometry stands behind them,
  and dims window glow cards that sit flush on walls. Both are gone with Strength 0 + MeshSoftScale 0.
