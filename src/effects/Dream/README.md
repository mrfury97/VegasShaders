# Dream

A "fun" look, off by default. **Ported from the author's own DreamSim shaders** (DreamSim `godot/shaders/post.gdshader` and
`godot/shaders/mix.gdshader`, with the timings and curves of its `src/main.gd`). The HLSL was written fresh from that GLSL; it uses only
the normal effect boilerplate (the full-screen `FrameVS`, the `TESR_` constants and samplers) and none of the shared Vegas Shaders
helper code (no includes, no other effect's functions). The port is one way: nothing from Vegas Shaders goes back into DreamSim.

Left out on purpose: DreamSim's bodycam part (Vegas Shaders has its own `Bodycam` effect) and its geometry tricks (PS1 vertex
wobble, corridor twist, edge texture melt), which need the world's shaders rather than a post pass.

## 1. Purpose

The picture drifts into a trip. One dream level (0-1) drives everything: the middle of the picture twists slowly and the whole image
wobbles, red and blue split apart, colours drift round the colour wheel and grow stronger, anything that moves leaves tracers, and the
edges darken with a coarse 15-bit dithered colour. The level breathes by itself around the value set. Now and then the scene melts
into a Julia fractal for a few seconds; glitch bursts break the picture into posterised macroblocks and smeared datamosh blocks; and
holding the view still for a few seconds makes it fall into itself (a swirl, then a hexagonal kaleidoscope zooming in for ever).
Optionally, low health deepens the dream.

## 2. When it runs

- **Switch:** `[_Shaders.Dream.Status] Enabled`, default **false**.
- **Menu:** the sidebar's effects list, group **Extras** (second last, above Developer), entry **Dream**. Every setting is live.
  With Advanced off the page shows a short list (dream level, breathing, tracers, stare tunnel, fractal melts, deeper when hurt;
  `kSimpleRows` in `src/core/ImGuiManager.cpp`); Advanced shows every key below.
- **Frame position:** post chain, after Bodycam and before SkyDither (`src/core/EffectChain.cpp`, step `Dream`). It sees the finished
  picture (after anti-aliasing, Cinema, ImageAdjust and the body camera). Cinema's fused sky dither is not used while Dream draws
  (it sits between them).
- **Skip rule (`ShouldRender`):** nothing is drawn while the dream level this frame is 0 (Level 0 with Breathing off, or the
  breath below 0 at a low level) unless a melt is still running. With LowHealth on, low health alone can start it.
- **Clock:** the effect's own seconds, advanced once a frame while the switch is on (`UpdateConstants` is called twice a frame; the
  second call returns at once).

## 3. Inputs

### Textures (declaration order = binding order)

| Name | Holds | Filtering / addressing |
|---|---|---|
| `TESR_RenderedBuffer` | the picture arriving at Dream | linear, clamp |
| `TESR_DreamHistory` | the effect's own history: last frame's mix (in the mix pass), this frame's mix (in the post pass) | linear, clamp |

The history is two screen-size `A16B16G16R16F` render targets, made the first time the mix pass runs (none before: a 4 x 4 stand-in
`NVR_DreamStandIn` is bound). Half floats so a fading tracer does not leave its last 8-bit step stuck on screen. It is not the TAA
history. Remade if the picture's size changes; if it cannot be made, tracers, tunnel and datamosh stay off (logged once).

### Constants

| Name | Components |
|---|---|
| `TESR_DreamLevel` | x dream level now (0-1), y seconds, z hue angle (radians), w warp (level² x Warp) |
| `TESR_DreamTrails` | x tracer mix (0-0.95), y tunnel (0-1), z datamosh block chance, w macroblock chance |
| `TESR_DreamMelt` | x melt (0-1), y fractal variant (1-4), z fractal zoom, w colour split (uv) |
| `TESR_DreamFinish` | x vignette, y dither, z extra saturation, w unused |
| `TESR_DreamGrid` | xy the low-res grid (screen size / pixel size), z macroblock seed, w datamosh seed (frame-rate-free counters, 13 and 9 a second) |

## 4. Settings (`[_Shaders.Dream.Main]`, all live)

| Key | Default | Range | What it does |
|---|---|---|---|
| `Level` | 0.5 | 0-1 | the dream level |
| `Breathing` | true | | the level breathes: `(0.05 + 0.1 Level) (0.65 sin(0.23 p) + 0.35 sin(0.61 p + 1.7))`, p = seconds x BreathingSpeed |
| `BreathingSpeed` | 1 | 0-5 | how fast it breathes |
| `Warp` | 1 | 0-3 | twist and wobble (x level²) |
| `ColourSplit` | 1 | 0-3 | the split: `0.0008 + 0.004 level²` of the screen width, times this |
| `HueDrift` | 1 | 0-3 | hue angle `1.2 level^1.5 sin(0.07 t)`, times this |
| `Saturation` | 1 | 0-3 | extra colour `0.8 level`, times this |
| `Tracers` | 1 | 0-1.5 | history mix `0.6 level^1.5`, times this (at most 0.95) |
| `Tunnel` | 1 | 0-1 | the stare tunnel's strength |
| `TunnelDelay` | 4 | 0-120 s | stillness before it starts; it opens over 20 s and eases out at 0.4 a second |
| `Melt` | 1 | 0-1 | the fractal melt's strength |
| `MeltInterval` | 7 | 0-60 min | mean time between melts (DreamSim: 4-10 min, divided by 1 + level); 0 = never |
| `Macroblocks` | 1 | 0-3 | block chance during a glitch: 0.22 x this (green flash: 0.06 x this) |
| `Datamosh` | 1 | 0-3 | block chance during a glitch: 0.3 x this |
| `GlitchInterval` | 85 | 0-3600 s | mean time between glitch bursts (45-125 s divided by 1 + level, as DreamSim's lag spikes); 0 = never |
| `Vignette` | 1 | 0-1.5 | darkening `0.55 smoothstep(0.35, 0.85, r)`, times this |
| `Dither` | 1 | 0-1 | blend towards 31 steps a channel with a 4 x 4 Bayer dither |
| `PixelSize` | 0 | 0-32 | screen pixels per DreamSim pixel for the blocks and the dither; 0 = screen height / 240 |
| `LowHealth` | 0 | 0-1 | added to the level: this x smoothstep from half health (0) to a tenth (1); needs the player alive |

The constant parts (vignette, dither, the 0.0008 base split) fade in over the first quarter of the level, so a level near 0 is
close to the untouched picture (in DreamSim they are always on).

## 5. Passes

- **Technique 0 `DreamMix`** (only when tracers, the tunnel or datamosh are live): this frame mixed with the history (tracers), the
  tunnel blended over it, datamosh blocks from the history; into the other history target, which then becomes `TESR_DreamHistory`.
  After a gap (not drawn for 0.25 s, a menu, a load) or a camera cut, and on the history's first frame, the tracer mix and datamosh
  are 0 for that frame.
- **Technique 1 `DreamPost`**: warp, macroblocks, colour split, melt, hue, saturation, vignette, dither; from the mix just written
  into the chain (it may be the last effect and write the game target).
- **Technique 2 `DreamPostDirect`**: the same from `TESR_RenderedBuffer`, when no mix pass runs (the history is then marked stale).
- D3D9 only: no Vulkan (DXVK extension) path and no SM6 build; `ExtRender` is not overridden.

## 6. Behaviour details

- **Melt:** in 4 s, held 2-4 s, out 3.5 s, eased twice; one of four Julia sets (variant drawn at random); the view zooms towards the
  set's repelling fixed point at `exp(0.42 (t - 2))`. Pixels slide from their place to their fractal place, then take the set's
  palette (more where the orbit escaped). The first melt comes one interval after the effect starts drawing.
- **Glitch bursts:** 0.4-1.8 s each; DreamSim triggers its glitch by hand, here they come at random.
- **Stare:** "still" = turning under about 7 degrees a second and moving under 20 units a second (player rotation and position).
  Reset under the game's menus (Pip-Boy, VATS, dialogue, terminals and other full-screen menus); the Vegas Shaders menu does not stop it.
- **Not ported:** DreamSim's stare also raises the level by itself, and its field of view breathes; both left out (one is a setting the
  player owns, the other is camera geometry).
- **RenderScale / RenderRect frames:** no special handling (post-tonemap, after any upscale).
- **HDR output:** works on the display values like the rest of the post chain; the dither assumes 0-1.

## 7. Look and cost

- **Correct:** at Level 0.5 the picture twists gently, colours swim and are richer, motion leaves soft trails, edges are darker and the
  colour slightly banded with a fine pattern. Stand still 4 s: the middle starts to swirl and after ~15 s turns into nested hexagons.
- **Wrong:** trails that never clear (history not flipped or continuity broken), a frozen picture (post pass reading a stale history),
  the tunnel starting while walking (stillness test), black blocks (history stand-in read by datamosh).
- **Cost:** post pass ~380 instructions (8 texture reads; the melt's 48-step loop only during a melt); the mix pass ~300 (18 reads,
  the tunnel's work only while staring). Two extra half-float screen targets once tracers or datamosh have run.
