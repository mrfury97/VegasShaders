# One deferred reflection pass: design (2026-11-20)

Read from the code of the current test build (`test-p1a-bump` e31a5ed: deferred wetness steps 1-3, bumped normals, Reflections with the
film). Paths are under `src\` unless they start with `docs\` or `resource\`. Nothing here was built or run. Costs are estimates, not
measured. Background: `docs/deferred-roadmap.md`, `docs/deferred-vs-forward.md`, `docs/bumped-normals-study.md`,
`docs/deferred-wetness-design.md`, `effects/Reflections/README.md`.

## Short answer
- **Doable, and most of it exists already.** The Reflections effect's Vulkan path is already the source ladder the request describes:
  screen ray (or RT hit) -> live cube -> light grid -> sky, mixed by split-sum Fresnel and blurred by roughness
  (`hlsl/NewVegas/Compute/ReflectionsTrace.frag:75-99`, `ReflectionsRTResolve.frag:247-264`). What makes reflections "many systems" today
  is everything around it: four other screen passes trace or fake their own reflections, it takes its material from a side channel
  (MaterialAware) instead of the G-buffer, it only covers floors, and the forward shaders still add their own cube / sky / metal sheen.
- **The plan:** grow the Reflections effect into the one pass rather than write a new one. It reads normal, roughness, metal and the wet
  film from the G-buffer, covers every surface by roughness, and the other reflection passes and forward sheens retire one by one.
- **In game:** one reflection look everywhere (wet roads, tiles, metal, puddles agree with each other); glossy walls and props reflect
  too, not only floors; no doubled sheen on env-mapped props; one "Reflections" switch with a quality level.
- **Effort:** about 6-9 working sessions in five steps. Biggest risk: double-counted or missing reflections while forward sheens are
  retired (the energy has to move, not be added).
- **Recommended first step:** measure today's reflection cost in rain and dry (0.5 session, needs one in-game run with the profiler),
  then step 1: Reflections reads its material from the G-buffer (1-2 sessions, test switch, off).

## 1. Every reflection path today

| Path | Traces / samples | Normal | Roughness / F0 | Miss fallback | Covers | API |
|---|---|---|---|---|---|---|
| **Reflections effect** (`effects/Reflections/`, chain step Reflections `core/EffectChain.cpp:990-1011`; off by default, `[Shaders.Reflections.Status]`) | half-res march over depth, sampling the lit frame (`Compute/ReflectionsTrace.frag:42-73`); GGX spread, exact ray under roughness 0.03 (:20-31); optional Hi-Z | `FdReflNormals(TESR_NormalsBuffer)` (`Reflections.cpp:36-41,1397`): smooth, or the G-buffer shading normal with DeferredReflections / DeferredBumpedNormals; wet pixels lean back to smooth (`Compute/Reflections.glsl:196-204`) | constants Dry/WetRoughness (glsl:104-107); MaterialAware copy (glsl:322,390-414); film when `FilmWhole` (glsl:273-277); specular AA on bumped normals (glsl:421-458); metal F0 `ReflMetalF0.glsl` | live cube -> light grid -> sky, hand-over `smoothstep(0.15,0.35,roughness)` (Trace.frag:82) | upward floors `N.z > 0.75` (glsl:389), or MaterialAware b > 0.75 any facing (:392), glass by GlassMask, not water | Vulkan (fork) or D3D9 `Effects/Reflections.fx.hlsl` (film, puddles, AA, sky pano are Vulkan only) |
| **RT reflections** (same effect, `[Shaders.Reflections.RT] Enabled` off) | 64-bit helper, compute ray queries on the scene TLAS (`Compute/Rt/ReflectionsRTBody.glsl`), sun / key-lamp shadow ray, MetalBounce | as above | as above, MaxRoughness 0.5 | Resolve: screen -> cube -> hit estimate -> grid -> sky (`ReflectionsRTResolve.frag:35,59,247-264`) | as above | Vulkan + helper |
| **Path tracer** (`ReferencePT`, internal) | its own GGX bounce (`RtPathTrace.comp:1967-1979`, roughness from COLOR2.b) | G-buffer | COLOR2.b | sky | whole picture; Reflections skipped (`Reflections.cpp:819`) | Vulkan |
| **WetnessReflections** (`effects/WetnessEffects/WetnessReflections.cpp`, step after `DeferredWetness::Fallback`, `EffectChain.cpp:934-939`) | full-res march over depth, 24 steps (`Effects/WetnessReflections.fx.hlsl:252-310`); replaces the forward wet sky term with the hit | TESR_NormalsBuffer; film / COLOR2 oct normal (fx:186-203) | GetWetness, or the film (r gloss, g roughness) | keeps the forward sky (fx:289) | wet pixels, outdoors, Wetness Effects on | D3D9 only |
| **WetWorld** (`effects/WetWorld/`, on by default, drawn only when Wetness Effects is off, `EffectChain.cpp:946-951`) | its own puddle SSR (`WetWorld.fx.hlsl:494,603`) | shading normals s15 | its own puddle mask, MaterialBuffer r/a | analytic sky (:1127-1135) | wet ground | D3D9 |
| **DeferredWetness sky** (`Shaders/DeferredWetness.hlsl:177,291`; fallback :164-180) | sky along the wet normal (`Includes/Wetness.hlsl:681-695` WetSkyReflection) | film normal | wet gloss, F0 0.02 | - | wet pixels | D3D9 full-screen |
| **Forward wet film** (`ObjectTemplate.hlsl:571-581`, Terrain :737-747, Parallax :893-899) | sky SH along the wet normal + sun highlight | GetWetness | F0 0.02 x gloss | - | forward records (and lighting-free ones without DeferredWetness) | D3D9 game shaders |
| **DeferredReflections** (`[Main.Main.Quality]`, off; `core/FdIndirect.h:30,72`) | not a pass: swaps Reflections' normals to the shading normals and its material copy to the G-buffer one (`core/FdIndirectShaders.h:48-56`) | G-buffer shading normal | spec-bit models 1-3 reflect, 5-9 rough | - | - | both |
| **Env-map pass** ("chrome", `Shaders/EnvTemplate.hlsl`, replaces SLS2057/2058, `effects/PBR/PBR.h:112-117`) | the game's painted cube (s1) x mask (normal alpha or s3); LiveCube replaces it when ready (:189-194) | normal map, bumps flattened x 0.1 (:77-81,167-169) | none (no roughness, no Fresnel) | - | env-mapped meshes, glass windows; drawn ADDED over the deferred base, never in the G-buffer (`core/LightFreeRecords.h:85-97`, `core/CapturedNormals.h:12-15`) | D3D9 forward |
| **Metal sheen** (`Includes/Object.hlsl:643-659` getMetalAmbient) | ambient + sky along the reflected elevation, or the live cube | geometric | EnvBRDFApprox(F0 = albedo, roughness) | - | metal in lit ObjectTemplate passes; stays in COLOR0 of the lighting-free record (`ObjectTemplate.hlsl:1541-1547`) | D3D9 forward, baked into the deferred base |
| **Skin sky** (`Includes/Skin.hlsl:248-262`) | sky SH | skin | skin F0 | - | skin (deferred skin keeps it in COLOR0) | forward / in the record |
| **Specular effect** (off; `EffectChain.cpp:954-979`) | analytic GGX sun + sky, quarter res | depth normals | constants | - | everything | D3D9 |
| **Water** (`NewVegas/Hooks/Render.cpp:5238-5306`, ForceReflections on) | the game's planar reflection camera (`WATER000:85`, `WATER017:84`, `WATER033:63`); placed / interior water: horizon or fog colour; RT Water reads last frame's Reflections buffer, reprojected (`Includes/Water.hlsl:112-128`) | wave normal map + rain ripples | Schlick, F0 0.08, no roughness (`Water.hlsl:167-183`) | planar -> horizon | water | D3D9 forward, its own camera |
| **Glass** | only the window env-map pass above; GlassMask lets Reflections reflect on glass (GlassReflect 0) | - | - | - | blended, forward+ (`core/DeferredTransparents.h`) | D3D9 forward |

So a wet, metal-trimmed street in the rain can get up to five reflections at once: the forward / DeferredWetness sky term, WetnessReflections'
trace over it, Reflections' trace, the env-map pass, and the metal sheen in the base. They are mixed or added by different rules.

## 2. The unified pass

### Where it sits and what it is
The Reflections effect, kept at its place in the chain (after the lit frame exists, before the fog; `effects/Reflections/Reflections.h:4-6`).
A true "right after the G-buffer" pass is not possible for the screen trace: it samples the lit colour. Modern deferred engines put SSR
after lighting and before transparents; moving it before the blended list here is a later option (step 5), not needed for one system.

Per pixel, one ladder (as today's Vulkan trace, extended):
1. Inputs from the G-buffer: shading normal (COLOR2 g/a, smooth COLOR3 for the ray start), roughness (COLOR2 b), metal (COLOR2 r fraction /
   0.49), specular bit and model (COLOR2 r), albedo (COLOR1 rgb: metal F0), the wet film (DeferredWetness target: gloss, roughness, normal).
2. Effective surface: wet share w from the film; roughness = lerp(dry, film roughness, w), widened by specular AA where the normal is bumped;
   normal leans to the smooth / film normal by w; F0 = lerp(0.04, albedo, metal), toward 0.02 by w on dielectrics.
3. Source, by roughness: under MaxRoughness the trace (RT hit when RT is on, else SSR; Hi-Z at high quality). Miss or too rough: live
   cube -> light grid -> sky (today's hand-over). Above the cut-off no ray at all: probe only (cheap; this is the "ambient specular" every
   surface needs and what the forward sheens fake).
4. Mix: today's composite (`ReflectionsComposite.frag`: `lin = mix(lin, reflected, share)`, share = split-sum Fresnel x coverage),
   denoised by roughness (RT denoiser, or temporal + blur).

### What the G-buffer has and lacks (mode 2)
- **Has:** shading normal, smooth normal, roughness, metal, model, spec bit, albedo, distance, the film. Nothing new is needed for steps 1-3.
- **Lacks:**
  - *"The artist made this shiny":* the env-map flag and mask never reach the G-buffer (the env pass is masked off). Two ways, no new
    channel: (a) `[Shaders.PBR.EnvMaps] FromGameData` (exists, off; `Includes/Object.hlsl:119-126,517-557`) already turns the env data
    into metal in the object records, so it lands in COLOR2 r; (b) the record lowers its written roughness by the env mask. (a) first.
  - *Specular occlusion:* none stored; AO and Skylighting's sky visibility are screen buffers the pass can read (already does for the sky).
  - *Pixels the G-buffer does not hold:* game shaders (fallbacks, decals, distant trees), water, glass, particles, first person. They get
    what they get today (water / glass: section "Forward surfaces"; the rest: no reflection), never a wrong material: COLOR1 a = 0 means skip.
- **Channel reuse:** none proposed, so no reader changes. If wet roughness were ever written into COLOR2 b instead of the film target (so
  DeferredSun's own GGX gives the wet highlight), every reader of COLOR2 b must be checked: `DeferredSun.hlsl:205,259`,
  `DeferredLamps.hlsl:419`, `FlashlightDeferred.hlsl:168`, `GearWeather.hlsl:85`, `Compute/Reflections.glsl:379-380,412-413`,
  `DlssRRGuides.comp:113-117`, `RtPathTrace.comp:1967-1982`, `core/FdIndirectShaders.h:51-55`, `core/CapturedNormals.h:471`. Not proposed now.
- **Mode 0** (`DeferredRenderer = 0`) has no lighting-free records but still has CapturedNormals / MaterialBuffer by default. MaterialAware
  (the scene alpha trick, `core/ReflMaterial.h`, `core/ReflMaterialRule.h`) stays as the mode-0 material source; mode 2 reads the G-buffer.

### What it replaces, and what stays
| Today | Unified |
|---|---|
| MaterialAware / `kReflMaterialPS` material copy (mode 2) | G-buffer read in the trace (no copy pass, no scene-alpha rule) |
| DeferredReflections + DeferredBumpedNormals (two test keys) | always the G-buffer normals, with specular AA (one behaviour, no key) |
| "floors only" (`Reflections.glsl:389`) | every surface by roughness; floors-only stays as the Low quality |
| WetnessReflections screen pass (full-res D3D9 trace) | removed while Reflections is on: the film feeds the one trace (already done for the Vulkan path, step 3) |
| WetWorld's puddle SSR | retired with the wetness step 4 (WetWorld off when Wetness Effects is the one system) |
| DeferredWetness's own sky add (`DeferredWetness.hlsl:177,291`) | the pass's sky / probe fallback; DeferredWetness keeps albedo, normal and film only |
| Env-map pass on top of the deferred base | skipped for G-buffer pixels when the pass answers (ReplaceForward exists, `EnvTemplate.hlsl:203`); env data becomes metal / roughness (FromGameData) |
| Metal sheen in the base (getMetalAmbient) | dropped from the lighting-free records (shorter shaders: helps the X4505 wall), added by the probe step |
| Specular effect's sky term (off by default) | retired: the probe step is the same thing, done right |
| **Stays separate** | water (planar camera, forward), glass (blended, forward+), skin's sky term (skin's own model), the path tracer (replaces the picture) |

**Forward surfaces.** Water already reads last frame's reflection buffer for RT Water (`Includes/Water.hlsl:112-128`); the same read can take
the SSR result for water near the camera, with the planar map as the fallback (the planar camera stays: SSR misses most of what water
mirrors). Glass can read the same buffer at its pixel (the env pass's ReplaceForward read already does exactly this).

### Wetness
The film target already says where and how wet (step 3). In the pass it is only a material change: lower roughness, F0 toward 0.02, the
normal toward smooth. That is what step 3 built in `Reflections.glsl` FilmWetness for the Vulkan path. Missing: the D3D9 technique has none
of it (film, puddles, specular AA are Vulkan only, `Effects/Reflections.fx.hlsl`): to be ported for one system on native D3D9.

### D3D9 and Vulkan / RT
- One effect, two back ends as today (`core/EffectRecord.cpp:504` picks Vulkan when `DxvkCompute::PassesOn()`). The rule becomes: both
  back ends read the same inputs and give the same answer without RT; Vulkan adds RT and Hi-Z.
- RT is not a separate effect now either: RayGen -> helper -> Resolve replaces the trace inside the Reflections step and shares the
  fallbacks and the composite. In the unified version RT is simply the top quality level's first source.

### Settings
One switch, `[Shaders.Reflections.Status] Enabled`, and one `Quality`:
| Quality | What |
|---|---|
| 0 Probes | no trace: cube / grid / sky by roughness on every surface (replaces the forward sheens cheaply) |
| 1 Low | half-res SSR on floors and wet ground, probes elsewhere |
| 2 Medium (default when on) | half-res SSR on every surface under MaxRoughness |
| 3 High | + Hi-Z trace, temporal + blur at full settings |
| 4 RT | RT hits first (fork + helper), denoiser |
Retired keys (kept readable for old tomls, then removed with a migration): WetnessEffects.Reflections Enabled, DeferredReflections,
DeferredBumpedNormals, MaterialAware, Specular's sky. The tuning keys stay under `[Shaders.Reflections.Main]`. Turning reflections on by
default, or folding keys players have saved, changes the picture: owner's word, with a step in `Configuration::UpdateOldDefaults()`.

### Frame cost (estimate, NOT measured)
- Today in rain with both on: Reflections (cube 6 + 42 filter draws, half-res trace, temporal, two blurs, full-res composite) plus
  WetnessReflections' full-res 24-step march (believed the larger of the two traces: 4x the pixels of a half-res trace) plus the
  DeferredWetness fallback.
- Unified: the full-res wet march goes (believed about 0.4-1 ms at 1440p on a mid card); the trace covers more pixels (walls, props) but at
  half resolution and only under MaxRoughness; the probe-only pixels cost one cube / grid read; the lighting-free records lose the metal
  sheen and the env pass is skipped on answered pixels.
- Net believed: lower than today in rain with both on; up to about +0.3-0.6 ms in dry interiors full of glossy walls at Medium (more
  pixels traced). Quality 0 / 1 cost less than today's effect. Phase 0 measuring decides.

## 3. Risks, effort, order

**Does a mod already do this?** Not for New Vegas that we know of. Community Shaders (Skyrim) is the reference: believed (not checked this
session) it keeps the engine's own deferred SSR for water and wet surfaces and adds Dynamic Cubemaps (a cube made from the screen, used by
env-mapped materials and its wetness) and Skylighting (sky visibility for specular, which this build already ported). That is the same split
this design ends: screen trace first, a live cube as the probe, sky visibility on top. No single CS pass covers everything either.

**Risks**
- Double or missing reflection while forward sheens move (env pass, metal sheen, wet sky): each retirement must remove one term and add it
  in the pass in the same change. Check by A/B on the same spot (proven method: the owner's toggles found the grain cause).
- Wider coverage shows SSR's limits on walls (holes at screen edges, disocclusion streaks): the probe fallback hides most; Quality 1 keeps
  floors only.
- Noise on bumped surfaces: specular AA exists (step 3) but is only proven offline; the owner's look is pending.
- D3D9 parity: the .fx path lacks the film and the AA; porting them meets ps_3_0 limits in a full-screen pass (no 62-define issue, temps
  believed fine).
- Frame cost on walls at Medium (not measured).
- Mode 0 keeps MaterialAware: two material sources to maintain, by design (deferred first).

**Steps** (each under a test switch, off, until the owner's look; then one migration for the defaults)
| # | Step | What you'd see | Sessions |
|---|---|---|---|
| 0 | Measure: Reflections and WetnessReflections cost, rain and dry, F10 profiler, alone | nothing | 0.5 (one in-game run by you) |
| 1 | Reflections reads normal, roughness, metal, albedo F0 from the G-buffer in mode 2 (replaces MaterialAware and DeferredReflections' copy; bumped normals + specular AA always) | same floors; metal and smooth props reflect by their material | 1-2 |
| 2 | Wet into it on both back ends: D3D9 technique gets the film; WetnessReflections pass and DeferredWetness's sky add skipped while Reflections is on | one wet reflection, no double sky on wet ground | 1-2 |
| 3 | Every surface by roughness + probe-only above the cut-off; lighting-free records drop getMetalAmbient; env pass skipped where answered (ReplaceForward, FromGameData) | glossy walls / props reflect; no painted sheen over real reflections | 2-3 |
| 4 | One switch + Quality; retire the old keys with a migration (owner's word) | one menu entry | 0.5-1 |
| 5 | Optional: water and glass read the result; pass moved before the blended list | near water shows SSR detail; glass reflects the room | 1-2 |
Total about 6-9 sessions (6.5-10.5 with step 5).


## Step 1 as built (2026-11-20, branch `claude/unified-refl-step1-009a94`; NOT yet run in a game)
- Switch: `[Main.Main.Quality] UnifiedReflections` (live, test only, off, in the hidden Internal list). To be folded into one Reflections
  Quality setting in step 4 (owner's word).
- In mode 2 with this frame's COLOR1-3, the Reflections passes decode the material per pixel from the G-buffer (`Compute/Reflections.glsl`
  GBufferMaterial; `Effects/Reflections.fx.hlsl` the same under `REFL_GBUFFER_ON`) into the old copy's layout, so MaterialReflectivity and the
  floor rule are unchanged: specular-bit objects / gear / cloth (models 1-3) reflect any way they face by MaterialReflect, up to
  MaterialMaxRoughness (chosen over RT MaxRoughness: it is the existing "which materials reflect" knob, MaterialAware's; RT MaxRoughness only
  decides which reflecting pixels get rays; both default 0.5); other written materials give the floor rule their roughness and metal; models 5-9
  rough. Metal F0 = COLOR1 albedo through ReflMetalF0; dielectric 0.04.
- A record counts only where COLOR3's distance matches the depth (the CapturedNormals readers' rule): anything drawn over the G-buffer keeps
  today's rules. The first-person weapon keeps MaterialAware's lookup (the copy is still made while the RT weapon rays run).
- Normals: the shading normals in every frame it is used (as DeferredBumpedNormals), so the Vulkan specular AA and the film's lean run.
- Review (lead-architect, approve with changes, applied): EffectPrecompile's Job::Macros was 8 while BuildMacros fills 10 (latent, now
  reachable with the ninth define: fixed to 10); the weapon's copy; the normals gated on the frame's decision; the per-technique D3D9
  fallback documented as a known gap. Metal F0 from COLOR1's raw base colour makes rough coloured metal darker than the old normalised hue
  (by 1 / its brightest channel): intended, to be judged by eye.
- MaterialAware's copy (technique 7) and DeferredReflections' / MaterialBufferRoughness's merge are not drawn while it is on. Mode 0 / 1:
  untouched.
- D3D9: compiled in only when on at startup (one define more: nine at most, the macro arrays hold ten with the terminator); COLOR3 / COLOR1
  ride on the capture cubes' stages 8 / 9 (only Capture / Filter read those). Offline: off, the disassembly is identical to before
  (plain and MaterialAware builds); all four on/off x MaterialAware variants compile. Instruction slots with it on: Composite 555 -> 719,
  Trace 816 -> 889, Blur 535 -> 609 (MaterialAware builds).
- Vulkan: bindings 30 / 31, `cb.gbuffer`; all twelve Reflections*.spv rebuilt and validated.
- Owner's first look (2026-11-20, install-test13): "reflections dont seem to be using the normal bump at all". Cause (from the code, believed):
  step 3's specular AA measured the bumped normal, so every bump became roughness past the trace's hand-over. Fix (install-test14): AA on the
  smooth normal's curvature (kappa 0.015), bumps kept whole near the camera and faded to the smooth normal by pixel footprint (0.5 -> 2.0
  units / pixel), damp ground keeps 20 % of its bump (puddles are flat in the G-buffer); DebugView 12; DeferredReflections, MaterialAware and
  MaterialBufferRoughness hidden in the menu while UnifiedReflections is on in mode 2 (DeferredBumpedNormals stays: Wetness Effects and
  WetWorld still use it). Reviewed (approve with changes, applied). README "The wet floors from DeferredWetness's film" has the numbers.
- Not done here: the D3D9 film and specular AA (step 2), coverage of every surface / probe-only (step 3), the env-map pass and metal sheen
  (step 3), the one switch (step 4).

## Proven / believed / not run
- **Proven from the code** (file:line above): every path in section 1, the G-buffer layout and COLOR2 b readers, that the Vulkan trace already
  has the source ladder and Fresnel mix, that WetnessReflections and the env pass add their own reflection on top.
- **Believed:** the cost estimates, the relative weight of the two traces, Community Shaders' split, that wider coverage looks better than
  floors-only, that the D3D9 port fits.
- **Not run:** all of it; nothing built, nothing measured.
