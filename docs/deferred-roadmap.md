# Deferred renderer: what is and isn't deferred, and the road map (2026-11-18)

Read from the code on `vs-1.1.0-plus-fork` (upstream 1.1.0's renderer). Background and file:line detail: `docs/deferred-vs-forward.md`.
Effort is in working sessions and is an estimate. Nothing here was run in game. Upstream's own result docs (`docs/next/*`) are not in
either repo, so anything resting on them is "claimed".

## Where it stands (mode 2, the default)
| Deferred (sun and lamps in the full-screen passes) | Forward+ (forward, but every lamp from the lamp table) | Forward (the game's few lamps, or unlit) |
|---|---|---|
| Objects, armour, clothes, parallax | Glass and other blended lit objects (DeferredTransparents 1) | Water |
| Near land, far land, far (LOD) objects | Blended hair (DeferredHair) | Particles, smoke |
| Skin (DeferredSkin), alpha-tested hair | | Decals: blood (GDECAL), bullet holes (SM300x) |
| Near grass, SpeedTree leaves (sun only; their lamps: DeferredFoliageLamps, off) | | Distant trees (the game's DISTTREE shader) |
| | | First person: arms and gun (DeferredFirstPerson, off) |
| | | Fading-in / fading-out objects and faces, late grass |
| | | Shiny objects' cube-map layer (EnvTemplate), drawn over the deferred base |
| | | Anything whose forward shader failed to compile (the game's shader; see Phase 1) |
| | | Sky, clouds, menus, Pip-Boy screen, unlit objects: nothing to light |

## The order, and why
1. Measure before moving anything: the frame cost of deferred vs forward was never measured in the repo.
2. Fix what silently drops pieces back to the game's shader: it looks worst (today's pale ground) and costs least.
3. Turn on what upstream already built but left off: the code exists; it needs an in-game check and your word (each changes the picture).
4. Then new work, cheapest visible win first: forward+ for what still gets the game's few lamps, then true deferred for the last opaque bits.

## Phase 0: measure (room 06's kind of work)
- DeferredRenderer 0 vs 2, frame time alone, the same spots: a lamp-heavy interior (Vault 21), the Strip at night, open desert by day.
- `[Main.Main.Misc] GBufferCensus 2` on the same spots: how much of the screen each group covers (deferred, forward+, forward), so later
  phases are ranked by real screen share, not by guess (`core/GBufferCensus.h`).
- Effort 1 session. Risk: none to the game (test keys).

## Phase 1: stop pieces falling back to the game's shader
- **1a. Build the deferred record even when the forward one fails.** Today the lighting-free (deferred) record is only tried after the
  forward record loaded (`core/LightFreeRecords.h:70`, `core/DeferredRenderer.h:174`). At 19:13 the forward land shaders with 5-7 layers
  failed (X4505, out of temp registers, with Terrain Blending + Wetness Effects + Puddles), so the deferred ones were never tried and that
  ground was the game's shader. The deferred record leaves out the sun shadows and lamps, so it is believed to fit (not tried).
  Frames that fall back to forward (MSAA, lockpick) would then show the game's shader there.
- **1b. The fallback ladder reacts to compile failures too.** ShaderFallback drops one optional feature at a time only when a shader is
  over native D3D9's instruction cap (`core/ShaderRecord.cpp:271` tests `over` only); a shader that does not compile at all (X4505, also
  under DXVK) goes straight to the game's. Count `failed` as a reason to drop the next feature.
- What you'd see: the pale patches gone with Terrain Blending on. Effort 1 session + one full shader recompile to check. Risk: the ladder
  may drop Wetness Effects first (its order), which turns wetness off everywhere for the session; the order may need a land-only rung.

## Phase 1c: deferred wetness, the way around ps_3_0's limits (owner's choice 2026-11-18)
Chosen over the Vulkan "no-limits" replacements (`NvrDxvkReplacements`, `tools/nvr-dxvk/replace/nolimits.py`): those work only under the
DXVK fork, are built offline per exact shader variant (dump, build, copy; any setting that changes a define means a rebuild) and need a
ps_3_0 build that loads to key on. The deferred pass works on native D3D9 too and needs no per-variant builds.

**Why.** Wetness Effects is the biggest optional block in the land shaders: about 1,400 of the 6-layer land shader's 4,000-5,000
instruction slots by the code's own count (`core/ShaderFallback.h`, THE ORDER), and the land's temp-register overflow (X4505) at 19:13
came with Terrain Blending added on top of it. Taken out of the G-buffer-writing records, it leaves room for Terrain Blending and the rest.

**Proof that it can be done on screen.** `Effects/WetnessReflections.fx.hlsl` already works out "where it is wet and how glossy" for every
pixel from the depth and the normals with the surface shaders' own function (`Shaders/Includes/Wetness.hlsl` GetWetness, the same weather,
puddle field and puddle map), and GearWeather already writes weather layers into the G-buffer for first person (`core/GearWeather.h`).

**Design.**
- The lighting-free (deferred) records are compiled without the water layer: `WETNESS_EFFECTS 0` added to their overrides
  (`core/LightFreeRecords.h:46-49`; the define is set at `core/ShaderRecord.cpp:585`). Forward records keep it, so fading objects, water
  reflections and other cameras stay wet as today.
- One full-screen pass, `DeferredWetness`, between the G-buffer's end and the deferred sun (the span's end in `NewVegas/Hooks/Render.cpp`,
  after `DeferredDepth::AtSpanEnd`, before `DeferredSun`). Per pixel with a deferred record: GetWetness from the position (G-buffer
  distance or DeferredDepth), the smooth normal (COLOR3) and the bumped normal (COLOR2), then:
  - albedo (COLOR1 rgb) darkened by the porous-wet term;
  - roughness (COLOR2 b) lowered to the film's, and the specular bit set, so DeferredSun and DeferredLamps give the wet highlight with
    their own GGX (this also closes the "wet film's sun highlight" gap in DeferredSun);
  - the shading normal (COLOR2 g / a) flattened toward the smooth normal under standing water, with the ripples on top (`wet.normal`).
- The pass reads and writes the same targets, so it works from copies of COLOR1 and COLOR2 (or writes through blend states where the
  maths allows); the codes in COLOR1's alpha and COLOR2's model / metal bits are passed through untouched.
- Skin, hair, eyes, grass and leaves keep their own models: the pass skips their shading models (COLOR2 r), as the forward shaders give
  them no water film either (to be checked per family).
- Under its own switch first (`[Main.Main.Quality] DeferredWetness`, test only, off), with an A/B against the forward wetness.

**Cost (not measured).** One full-screen pass reading about four fp16 targets and depth, writing two: about 50-60 bytes a pixel, roughly
0.15-0.3 ms at 1080p, only while something is wet (Wetness Effects' own flag). The deferred land and object shaders get shorter, so the
G-buffer fill should get cheaper in the rain, believed to cover most of it.

**What you'd see.** The same wet ground, puddles and ripples, now also on the land pieces that fell back to the game's shader, with
Terrain Blending on. A one-time full shader recompile when the switch turns on.

**Effort and risk.** 3-4 sessions plus a recompile and a rain test. Risks: matching today's wet look (darkening curve, puddle edges);
puddle height on objects (`OBJ_PUDDLE_HEIGHT` reads the object's own texture, which the screen pass does not have: puddles on objects
may need the object's height carried in or dropped); wet surfaces seen in water reflections stay on the forward path (unchanged).
Needs Phase 1a first so the deferred record loads even if a forward one still fails.

## Phase 2: built upstream, off by default (each: an in-game look, then your word)
| Switch | What it moves | Look check | Effort |
|---|---|---|---|
| DeferredFoliageLamps | leaves lit by every lamp of the froxel, not the game's one | trees near street lamps at night | 0.5 |
| DeferredFirstPerson | arms, hands and gun through the deferred passes (aiming, scopes, Pip-Boy, VATS stay forward) | gun in a lamp-lit room, then aim | 0.5 |
| DeferredReflections, DeferredRRGuides | reflections / DLSS RR guides read the normal-mapped normal and the G-buffer material | wet roads, tiled floors | 0.5 |
| DeferredALAlbedo | bounce light takes the G-buffer colour minus metal | coloured walls near light | 0.5 |
| DeferredRtSunFirst | RT sun shadow as the deferred sun's own visibility | RT sun shadows on, outdoors | 0.5 |
| DeferredTiledLight | lamp pass only over the tiles that have lamps (speed) | frame time in Vault 21 | 0.5 |
| DeferredADFog | a fog fix for the light-only base passes | foggy interiors | 0.5 |
Risk: low per switch (the code exists and has its own A/B key), but none has been looked at on this PC. Several are claimed in upstream's
missing result docs only.

## Phase 3: forward+ for what still gets the game's few lamps
- **Water** (1-2 sessions): every lamp of the froxel and the flashlight on the water surface from the lamp table, as glass does
  (`Includes/ForwardPlus.hlsl`). True deferred does not fit water (blended, refracts, its own reflection). See: lamps reflected in water at night.
- **Fading objects and faces** (1 session): the blended base pass of an object fading in or out takes the forward+ record instead of the
  forward one, so it is lit like its neighbours while it fades (today it keeps the game's lamps, `DeferredRenderer.h:250-277`).
- **Late grass** (1 session): the fading tufts drawn after the main list get the leaves' forward+ lamp model.
- **Particles and smoke** (2 sessions, believed): the game's particle shaders are not this mod's templates, so they need replacement shaders
  first; whether the vanilla ones light at all was not checked.

## Phase 4: true deferred for the last opaque pieces
- **Decals** (2-3 sessions): blood and bullet holes write their colour, normal and roughness into the G-buffer inside the span, so the
  deferred passes light them (the standard "deferred decals"). Needs them drawn early inside the span, as DeferredDistant already does for
  the far world (`core/DeferredDistant.h`). Risk: draw order, blended writes into fp16 targets.
- **Distant trees** (1-2 sessions): a replacement for the game's DISTTREE shader that writes the G-buffer. Small screen share (Phase 0 will say).
- **Cube-map layer** (1-2 sessions): fold the EnvTemplate pass into the deferred reflections (cube map as the fallback when a ray misses).
  Risk: double reflections during the change.
- **DeferredSun's missing terms** (1 session): metal's tint, the wet film's sun highlight, the land's highlight with UsePBR off, parallax's
  sun with POM shadows off (`Shaders/DeferredSun.hlsl:17-18`).

## Stays forward on purpose
Sky and clouds, menus and the Pip-Boy screen, other cameras, unlit objects, first person while aiming, and glass (forward+ is the right end
state for see-through things). The path tracer replaces the picture and keeps foliage forward under it.

## Effort in total
Phase 0: 1. Phase 1: 1. Phase 1c (deferred wetness): 3-4. Phase 2: 3-4 (looks). Phase 3: 5-6. Phase 4: 5-8. About 18-24 sessions for
everything; Phases 0-1c (5-6 sessions) fix what breaks today and lift the shader-size limit.

## Questions for you
1. Start with Phase 0 (measure) or Phase 1 (the fallback fixes) first?
2. Phase 2 switches change the picture once on by default: look at them one at a time, or as a batch in one session?
3. Is water (Phase 3) the next big one, or decals / first person?

## Proven / believed / not checked
- Proven from the code: the table above, the Phase 1 causes (file:line given), the existing switches and their defaults.
- Believed: the deferred land record fits where the forward one did not; the effort numbers; screen shares.
- Not checked: vanilla particle lighting, DISTTREE's screen share, any frame time, anything in game.
