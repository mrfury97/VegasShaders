# Screen Space Shaders (STALKER Anomaly) rain and wet reflections vs ours

Owner's question (2026-11-21): can the outdoor rain reflection code of github.com/dsh2dsh/screen_space_shaders be ported?
Read from a shallow clone at `F:\VS-scratch\ext\screen_space_shaders` (commit a9fe1aa, "Update 18.1", 2023-10-19). It is Ascii1457's
Screen Space Shaders for S.T.A.L.K.E.R. Anomaly (X-Ray engine, D3D11 `r3` shaders), mirrored by dsh2dsh.

## Proven (read in the code)
- **No licence.** No licence file and no licence or permission text in any file; headers name the author and the ModDB page only.
  So: ideas may be re-written in our own words, no code is copied, and their textures (ripple and water-fall normal maps, the per-map
  puddle masks) never go into our repo or install.
- What its rain wetness and rain reflections are (files under `96 - IMPROVED GLOSS & WET SURFACES`, `05 - Rain - Puddles`, `03 - SSR`):
  - `rain_patch_normal.ps`: one deferred pass over the G-buffer. A rain cover map seen from above (the engine's own rain shadow
    map) decides what is open; open surfaces get rain ripples from a texture (up-facing), a scrolling "water fall" normal texture in
    50 columns on X- and Z-facing walls, and a gloss raise by rain wetness. Flora is skipped, undersides get no gloss, gloss fades out
    200-250 m, the cover counts as open past the rain map's far edge.
  - `deffer_impl_flat.ps` (terrain): puddles = a Perlin texture threshold x a hand-painted puddle mask per STALKER map x a flatness
    mask; inside, the normal is set straight up plus two scrolling ripple samples and rain ripples; the ground's detail texture is read
    with its UV pushed by the ripple normal (a fake refraction); a tint; the puddle writes its gloss into the G-buffer.
  - `screenspace_reflections.h`: a plain linear screen-space ray march (8-64 steps, one binary step at high quality), the sky or
    an environment colour where it misses, Fresnel = pow(cos, 3), strength = gloss. Terrain keeps the sky reflection near the screen
    edge; the first-person weapon gets extra reflection in rain.

## Compared with ours (Puddles, DeferredWetness, Reflections on `test-p1a-bump`)
| Theirs | Ours |
|---|---|
| Rain cover from a top-down map | Have it (ortho map cover test, 40-unit look-out on walls; `includes/Wetness.hlsl`) |
| Rain ripples (texture) | Have it (procedural rings `WetRainDrops`, stronger with rain) |
| Puddles: noise x painted mask x flatness | Have more: world noise + the land's height crevices + slope + cover, fill and dry over game hours, soaked ring, murk |
| Wet gloss raise, undersides dry | Have it; undersides keep `MinRainWetness` instead of 0 (a look choice) |
| Wall water streaks | We had them; removed 2026-11-18 on the owner's request |
| Linear SSR + sky | Ours is well past it: SSR or RT, live cube, light grid, sky ladder, rough blur, film-aware (Reflections + DeferredWetness) |
| Fake refraction inside puddles (ground wobbles under ripples) | **Missing in ours** |
| Extra first-person weapon reflection in rain | Not in ours (the first-person model is forward and dry; DeferredFirstPerson off) |

## Believed (not run)
- Worth taking, as an idea written fresh: **puddle refraction**. In `DeferredWetness.hlsl`, inside puddles, read the albedo a few
  pixels off along the ripple normal (scaled by water depth), so the ground under the water wobbles with the rain. Deferred path,
  one extra G-buffer read per puddle pixel; about one session including the offline compile check. Player-visible but only adds
  to puddles; frame cost believed small, not measured.
- Not worth porting: their SSR (weaker than ours), the water-fall streaks (removed by owner's choice), the painted masks (per STALKER
  map, their asset), the ripple texture (ours is procedural, no free sampler in the object shaders).

## Ruled out
- A direct port of the code: no licence, different engine (X-Ray D3D11 `r3`, Lua-script driven settings), and every piece but the two
  rows above already exists in ours in a stronger form.

## Open
- Owner's call: add puddle refraction (above)?
