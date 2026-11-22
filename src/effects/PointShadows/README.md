# Effect spec: PointShadows


REWRITTEN 2026-11-02 (branch claude/rw3-lights): the shaders are new, written from this spec (Effects/PointShadows.fx.hlsl; Compute/PointShadows.frag, PointShadowsCommon.glsl, PointShadowsContact.frag, PointShadowsUpsample.frag). The C++ (effects/PointShadows.cpp, ShaderManager.cpp) and the settings are unchanged. Fixed bugs are marked FIXED where they are listed.

## 1. Purpose

This pass builds one grey mask for the whole screen that says how much of the nearby point lights (lamps) and the one spot light reach each visible surface. A lamp is shadowed where a lamp shadow map says something stands between the lamp and the surface, and that lamp then gives nothing there. The mask goes into a screen-sized buffer that the later shadow effects read to darken what no lamp reaches. The player sees lamp shadows: a bookcase or a character blocks a lamp and the wall behind it goes dark, indoors always and outdoors at night when point-light shadows are enabled. The pass also writes an optional small contact-shadow multiplier (dark lines where a prop meets the floor or a wall meets a table) indoors.

## 2. When it runs

Frame placement (src\core\ShaderManager.cpp, the pre-tonemap chain, function RenderEffectsPreTonemapping):
- It runs after the depth and normals are built (depth combine, normal reconstruction and smoothing, captured normals, object motion, lamp visibility ranking, the "scene in" capture, the sun depth-fit measurement, the ray tracing core point, capsule shadows and back depth). It runs before the lamp contact pass, the sun contact shadows, and the shadow apply and later effects that read the buffer.
- The whole chain is skipped when the main render toggle (RenderEffects) is off, when the player has no parent cell, or when an overlay (terminal, lockpicking) is on while the menu background is ready.

Gates, in order:
1. Indoors (not exterior): runs when the interior shadow effect is enabled ([Shaders.ShadowsInteriors.Status] Enabled, default true). Exception: when the per-object forward lamp shadows drew this frame ("forward interior") and [Shaders.ShadowsInteriors.Forward] KeepDarkening is off, the pass is skipped and the buffer is cleared once to "no shadow" instead.
2. Outdoors: runs when the exterior shadow effect is enabled ([Shaders.ShadowsExteriors.Status] Enabled, default true). The pass itself does not check [Shaders.ShadowsExteriors.Main] UsePointShadowsDay / UsePointShadowsNight (defaults false). Those set the per-frame switch that enables cube-map reads (see section 3, TESR_ShadowFade.z). With that switch off, the pass still runs with no cube reads. Whether that is intended is an open question (section 9).
3. Day or night outdoors: the switch used is UsePointShadowsDay when the game's day flag is above 0.5, otherwise UsePointShadowsNight.
4. Indoor pass with forward lamp shadows on: the cube-map reads are switched off for the pass (the switch is forced to 0 for that frame and restored after), so the mask is only the lamps' reach (the room darkening). The per-object shadows already carry the shadows.
5. Within the pass the buffer is cleared to 0 (black) before the merged technique draws. Then, in order: the outdoor batches (ExteriorLightPoints above 12, section 5), the second-pass effect PointShadows2 (only when the merged technique is missing and [Shaders.ShadowsInteriors.Main] LightPoints is above 6; a different effect, not specified here), the outdoor traced-lamp mask, and then the indoor lamp contact pass (technique 2) when ContactShadows is above 0.
6. Half-resolution mask (section 5, techniques 3 and 4): only indoors, only with forward lamp shadows, and only with [Shaders.ShadowsInteriors.Forward] MaskHalfRes on (default off), and only when both techniques exist in the loaded effect file.
7. Scissor option (section 6): the merged pass may be drawn only inside a rectangle on screen. Default on.

Cost gating in the Vulkan route: see section 7 (the Vulkan route is not used in a rect frame, see section 6).

## 3. Inputs

### 3.1 Textures and samplers (D3D9 pixel shader)

Samplers are bound by their declared register numbers (s0 to s12) and one name-bound slot (stage 13). Filter and address modes are the effect file's own states.

| Name | Register | Holds | Filter / address |
|---|---|---|---|
| TESR_DepthBuffer | s0 | Vegas Shaders linear depth: stored value multiplied by the far plane distance (TESR_CameraData y) gives view distance in game units | linear, clamp |
| TESR_NormalsBuffer | s1 | per-pixel normal, encoded 0..1 (x2-1 gives a view-space normal, then multiplied by TESR_ViewTransform to get world space) | point, clamp |
| TESR_ShadowCubeMapBuffer0 .. TESR_ShadowCubeMapBuffer10 | s2 .. s12 (slot k is register s(k+2)) | the lamp's shadow cube map of lamp slot k: the nearest caster's distance from the lamp, divided by the lamp's radius, per direction. A texel of 0 or 1 means nothing was drawn there (no test) | linear, clamp, cube |
| NVR_LampMaskHalf | s13 (bound by name, not declaration order) | the half-resolution mask in R and the full-resolution depth in G (format G16R16F, half size rounded up) | point, clamp in the effect file (see section 9) |

Slot 11 (the 12th shadow lamp, index 11 in TESR_ShadowLightPosition) has no cube map: it is lit without a shadow lookup. The cube map for lamp slot 11 is never sampled.

### 3.2 Constants

No constant has a fixed register in this effect file; the compiler assigns them. The table gives each constant's meaning. "Units" are game units; the settings file says about 70 units make one metre.

| Name | Size | Meaning per component | Units / notes |
|---|---|---|---|
| TESR_ShadowLightPosition | 12 vec4 | xyz world position of the lamp in shadow slot k (slots 0..10 have cube maps, slot 11 does not). w = that lamp's radius (w 0 = no lamp in the slot) | game units; the radius is the reach the cube map covers |
| TESR_LightPosition | 12 vec4 | xyz world position of tracked lamps (no shadow slot). w = radius; w 0 ends the list | game units |
| TESR_LightColor | 24 vec4 | entries 0..11 are the colour of shadow slots 0..11; entries 12..23 are the colour of the tracked lamps (lamp i uses entry i+12). rgb = colour, w = dimmer | the lamp's light weight = Rec.709 luma of rgb times w |
| TESR_ExtraLights | 16 vec4 | xyz position of an extra lamp (lamps past the shadow and tracked sets, nearest first). w packs radius and weight: radius = floor(w / 8), weight = w - 8 x radius. w 0 ends the list | weight is luma x dimmer, clamped 0..7.99 when packed. Merged technique only |
| TESR_ShadowFade | 1 vec4 | x: sun fade (not used here). y: interior shadow flag (not used here). z: cube-map switch (1 = read cube maps; 0 = no cube reads, every lamp counts unshadowed). w: point-light draw distance (not used here) | z is 1 indoors (0 for the forward-interior pass), and outdoors it is UsePointShadowsDay or UsePointShadowsNight |
| TESR_PointShadowNear | 1 vec4 | x: NearFade (game units, 0 = off). y: EdgeFade (fraction of a lamp's radius, 0 = off). z: reach-mode value. w: MaskSkipFar switch (1 = skip lamps past their radius; exact either way) | z > 0: reach mode, z = 1 / LightRadiusMult (never below 1 for the multiplier). z < 0: forward mode 2 (attenuation without its angle term). z = 0: normal attenuation with the surface angle |
| TESR_SpotLightPosition | 1 vec4 | xyz world position of the spot light; w = its radius (0 = no spot) | game units |
| TESR_SpotLightDirection | 1 vec4 | xyz direction the spot points; w = full cone angle in degrees (half this angle is full strength) | degrees |
| TESR_SpotLightColor | 1 vec4 | rgb colour; w = dimmer | spot weight = luma(rgb) x w |
| TESR_LampContact | 1 vec4 | x: contact strength (0..1, 0 = off). y: ray length toward the lamp. z: thickness (a surface thinner than this in front does not block). w: normal offset for the start of the ray (0.5) | y, z, w in game units |
| TESR_LampContactFade | 1 vec4 | x: fade-in start, distance from the camera (25). y: 1 / fade-in range (1/20). z: fade-out end distance (the ContactDistance setting). w: 1 / fade-out range (1 / (0.3 x ContactDistance)) | game units |
| TESR_LampContactBias | 1 vec4 | x: depth bias (0.3). y: bias per unit of depth (0.002). z: normal offset per unit of depth (0.001). w: smallest share of the total lamp light that the traced lamp must give (0.2) | units as stated |
| TESR_LampContactNoise | 1 vec4 | x, y: factors that turn a pixel's uv into the noise position (0.06711056 x screen width, 0.00583715 x screen height). z: 52.9829189. w: 1 = noise on (darkening blurred), 0 = even steps | |
| TESR_ReciprocalResolution | 1 vec4 | xy = 1 / full-resolution width and height | |
| TESR_CameraPosition | 1 vec4 | xyz world position of the camera | game units |
| TESR_CameraData | 1 vec4 | y = far plane distance (scale for the stored depth); x = near plane | game units |
| TESR_ViewTransform | 4x4 | the view matrix as stored (HLSL row-vector convention); columns 0..2 give the camera's right, up and forward axes | |
| TESR_ProjectionTransform | 4x4 | the projection matrix as stored | |

Values fed by the engine per frame (src\core\ShaderManager.cpp and src\effects\ShadowsExterior.cpp):
- Lamps are chosen nearest first. A lamp keeps the shadow slot it had last frame when possible. Slot 11 keeps the farthest shadow caster, and is lit without a shadow lookup. Tracked lamps (TESR_LightPosition) are the lamps beyond the shadow slots. Extra lamps (TESR_ExtraLights) come from the forward-shadow slots past 24 and the tracked lamps they push off (indoors, with forward shadows).
- In the outdoor batch passes, the shadow slots of one batch are swapped in for each pass, then restored (section 5).

## 4. Settings

The effect has no settings section of its own. Every value below is read by the engine and fed into the constants in section 3. The effect file itself has no switches. "Read by" names where the value is consumed; the source is the defaults file resource\VegasShaders.defaults.toml and the reader in src\effects\ShadowsExterior.cpp unless stated.

### 4.1 [Shaders.ShadowsInteriors.Main]

| Key | Type | Range (clamp in code) | Default | Visual effect |
|---|---|---|---|---|
| NearFade | float | 0 to 128 (floored at 0) | 0.0 | Blockers closer than this (game units) to a lamp cast less shadow from it, none at the lamp itself. 0 = as before |
| EdgeFade | float | 0 to 0.9 | 0.0 | Each lamp's lit disc, and the shadows in it, fades to zero over this fraction of its radius, instead of ending on a hard circle. 0 = as before |
| MaskSkipFar | bool | on/off | true | Skips lamps past their radius (same picture, cheaper). Off gives the same picture |
| LampReachMask | bool | on/off | false | Reach mode: a lamp counts in full out to its game radius and fades over the rest of its reach (the game's own lamp light does the falloff). Changes the picture |
| ForwardReachMask | int | 0 to 2 (clamped) | 2 | Indoors with lamps: 2 = attenuation without the surface angle (no facets on characters, room as bright as before); 1 = reach mode (LampReachMask's rule); 0 = old surface-angle attenuation (facets). LampReachMask on overrides it |
| ContactShadows | float | 0 to 1 (clamped) | 0.0 | Strength of the indoor lamp contact shadows (technique 2). 0 = off, and the contact pass is not run |
| ContactLength | float | 4 to 200 (clamped) | 40.0 | How far toward the lamp the contact ray looks, game units |
| ContactThickness | float | 1 to 100 (clamped) | 12.0 | Thickness of a surface taken to block a contact ray, game units |
| ContactDistance | float | 200 to 20000 (clamped) | 3000.0 | Distance from the camera where the contact shadows fade out (fade-out ends here, starts at 70% of it) |
| LightRadiusMult | float | at least 1 | 1.5 | Multiplier that sets the reach of the lamp shadows; the reach mode uses its reciprocal |
| LightPoints | int | (see toml) | 24 | Number of lamps with cube-map shadows indoors (up to 64 with forward shadows) and the slot count the pass sees; outdoors at most 12 |
| ExteriorLightPoints | int | 0 to 44 | 12 | Outdoors with point shadows on: how many lamps cast shadows (above 12 adds batches of 11, section 5) |
| ShadowCubeMapSize | int | (see toml) | 512 | Resolution of one face of each lamp's cube map (the map this pass reads) |
| Enabled | bool | on/off | (no key in the defaults file) | Read by the exterior-constant code as Settings.Interiors.Enabled. See open questions |

### 4.2 [Shaders.ShadowsInteriors.Forward]

| Key | Type | Default | Visual effect |
|---|---|---|---|
| Enabled | bool | true | Per-object lamp shadows indoors (needs restart). When on and drawn, the pass is the reach-only mask (section 2) |
| KeepDarkening | bool | true | Keeps the darkening of what no tracked lamp reaches. Off: the pass is skipped indoors (section 2, gate 1) and the unlit corners are brighter |
| DarkeningBlur | bool | false | Blurs the darkening as the old pass did. Also turns on the noise for the contact shadows (section 6) |
| MaskHalfRes | bool | false | Draws the reach mask at half resolution and upsamples it (techniques 3 and 4) |

### 4.3 [Shaders.ShadowsExteriors.Main] and status switches

| Key | Type | Default | Visual effect |
|---|---|---|---|
| UsePointShadowsDay | bool | false | Outdoors in daylight: lamp cube-map shadows on (sets TESR_ShadowFade z). Off: no cube reads; the mask is unshadowed lamp light |
| UsePointShadowsNight | bool | false | The same, at night |
| [Shaders.ShadowsExteriors.Status] Enabled | bool | true | Whether the exterior shadow effect runs (pass gate outdoors) |
| [Shaders.ShadowsInteriors.Status] Enabled | bool | true | Whether the interior shadow effect runs (pass gate indoors) |

### 4.4 [Main.Main.Performance]

| Key | Type | Default | Visual effect |
|---|---|---|---|
| LampMaskScissor | bool | true | Draws the merged pass only inside the screen rectangle its lamps can reach; outside it stays 0. Same picture. Does not apply when extra lamps exist, or with unusual camera matrices (then the whole pass is drawn) |
| LampMaskScissorCheck | int | 0 | Development-only diagnostic. Leave 0. Does not change the picture |
| NvrDxvkPasses | bool | toml true (the code's own default is false) | Runs this effect's passes as Vulkan passes when the Vegas Shaders DXVK extension is present (section 7) |
| NvrDxvkVariants | bool | true | Uses the compile-time mode variant files where they exist (section 7) |
| NvrDxvkKeepD3D9 | string | "" | Comma list of effect names that keep their D3D9 passes. Naming PointShadows here forces the D3D9 passes |
| NvrDxvkTestSuffix | string | "" | Test only: tries a variant file with this suffix added to the mode name first (section 7) |
| NvrSm6Shaders | bool | false | Runs technique 0 from its SM6 build when present (section 7) |

## 5. Passes and outputs

The effect file has five techniques. The engine asks for them by index and by name. Each full-screen draw uses the same full-screen vertex pass (except technique 3, which offsets the uv by half a full-resolution texel) with pixel shader profile ps_3_0.

| Index | Name | Blend | Target | Clear first | What it writes |
|---|---|---|---|---|---|
| 0 | MergedPointShadows | none | the point-shadow buffer (TESR_PointShadowBuffer: G16R16, full resolution; the engine's ShadowPassSurface) | yes, to black (0) | the mask: the sum of every lamp's share of light reaching this pixel, saturated to 0..1, written to R, G and B (alpha 1). Covers the cube-mapped lamps 0..10, the unshadowed lamp 11, the tracked lamps, the spot light and the extra lamps |
| 1 | AddShadowedLamps | additive: source One, destination One | the same buffer | no | the outdoor batch: the cube-mapped lamps 0..10 only (the batch's swapped-in lamps), added onto what was there. Extra, tracked and spot lamps are not added here |
| 2 | LampContact | multiply: destination = destination x source | the same buffer | no | the contact multiplier: 1 where no contact shadow, below 1 where a lamp is blocked by a nearby surface |
| 3 | LampMaskHalf | none | NVR_LampMaskHalf (G16R16F, half size rounded up) | yes, to black | R: the mask (the same sum as technique 0, read at the top-left full-resolution texel of each half-resolution pixel). G: the full-resolution depth at that texel |
| 4 | LampMaskUpsample | none | the point-shadow buffer | no | the mask upsampled from the half-resolution buffer, depth-aware (section 6), written to R, G and B |

Which technique the engine picks:
- Normal outdoor or indoor pass: technique 0 (MergedPointShadows) if the effect file has it; if not, the second-pass effect runs instead (not specified here).
- Outdoor batches (ExteriorLightPoints above 12): technique 1, once per batch of 11 lamps after technique 0.
- Indoor contact pass: technique 2, after the mask, when ContactShadows above 0 (indoors only).
- Half-resolution path: technique 3 into NVR_LampMaskHalf (cleared), then technique 4 into the buffer (not cleared, it overwrites every pixel). Before technique 4 the stage 13 texture is bound to NVR_LampMaskHalf.
- Scissor path: the buffer is cleared to 0 over the whole target with the scissor test off, then technique 0 draws only inside the rectangle. If no lamp reaches the screen the draw is skipped and only the clear is done.

## 6. Behaviour details

Lamp arithmetic, per lamp, per pixel (world position from the depth and the view, normal from the normals buffer):
- Distance d = distance from the pixel to the lamp, divided by the lamp's radius. A lamp gives nothing when d is 1 or more (not less than 1), or when its weight is 0, or when its radius is 0 (the division gives infinity or NaN, which the test also rejects).
- Attenuation (standard radius falloff, as in the published "point light attenuation" construction): with s = clamp(d squared, 0, 1), atten = (1 - s) squared / (1 + 5 s), clamped to 0..1.
- Angle term (normal mode, reach-mode value z = 0): diffuse = mix(1, dot(unit direction to the lamp, normal), smoothstep(0, 0.2, d)). The mix gives the light a little bleed at very short distance. Under forward mode 2 (z < 0) the normal is set to the direction to the lamp, so the angle term is 1 and only the attenuation remains. Reach mode (z > 0) replaces the whole attenuation with 1 - smoothstep(z, 1, d).
- Shadow test: the cube map is read in the direction to the lamp with the x and y components negated (z kept). The stored value is compared with d plus 0.018 x d: lit when stored + bias x d is greater than d. The test is only used when the stored value is strictly between 0 and 1; otherwise the lamp counts as lit. When the lamp's near fade is set, the shadow result is blended toward 1 by smoothstep(0, NearFade / radius, stored): blockers within NearFade of the lamp cast a weaker shadow, none right at the lamp.
- Cube switch: when TESR_ShadowFade z is 0 the test returns lit for every lamp.
- Lamp amount = clamp(lamp factor, 0, 1) x lamp weight, then clamped to 0..1. The edge window multiplies it: when EdgeFade > 0, the amount is multiplied by 1 - smoothstep(1 - EdgeFade, 1, d), which takes the outer EdgeFade of each lamp's radius to zero. Without EdgeFade, the window is 1.
- The sum of all lamps is saturated to 0..1 at the end.

Spot light (merged technique only, not in batches): radius from TESR_SpotLightPosition w; attenuation as above; cone = (clamp(dot(spot direction, direction from the surface to the spot), 0, 1) with the cone limits cos(full angle) to cos(half angle)), inverse-lerped to 0..1 and then squared. Diffuse term = max(dot(direction to the spot, normal), 0). Weight = luma(colour) x dimmer. Spot light is never shadowed.

Extra lamps (merged technique only): the loop stops early once the running sum is 1 or more, or at the first empty entry. Each extra lamp's radius and weight are the packed values from TESR_ExtraLights w.

Sky and far pixels: the pass has no sky test. Sky or far depth lies beyond every lamp's radius, so it gets 0 from lamps. Anything the consumers do about sky happens in those effects.

First-person weapon: the mask pass has no weapon test. The contact pass fades in only past 25 game units from the camera, so the weapon and hands get no contact shadow.

Contact pass (technique 2):
- Fade: 0 nearer than 25 units from the camera, rising to 1 over the next 20 units, falling from 1 at 70% of ContactDistance to 0 at ContactDistance. Multiplied together.
- Lamp selection: the brightest shadowed lamp among slots 0..10, by (1 - s2) squared times the lamp's light (s2 = squared distance over squared radius, clamped), and only when that lamp gives more than 20% (TESR_LampContactBias w) of the total of all the slots' light. Lamps with a 0 radius are excluded.
- Facing: when the direction to the lamp makes less than 0.1 with the surface normal, the lamp is not blocked (the surface faces away).
- March: up to 6 steps along the direction toward the lamp, over min(ContactLength, half the distance to the lamp). Each step starts at the surface offset along the normal (TESR_LampContact w plus a per-unit-depth share). A step lies on a blocker when its depth is behind the stored depth by more than the bias (a constant plus a share of its depth) and less than the thickness. A blocker found at step fraction t contributes (1 - t), keeping the largest. The march ends at the first step that leaves the screen or lies in front of the camera.
- Jitter: with DarkeningBlur off, steps sit at the middle of each sixth (no grain). With it on, the step start is offset by interleaved gradient noise (the factors in TESR_LampContactNoise).
- Output: 1 minus strength x fade x (blocked light share), so the multiply darkens only where the traced lamp is blocked near the pixel.

Half-resolution mask upsampling (technique 4): each full-resolution pixel reads the four nearest half-resolution texels (point samples at their centres). Each has bilinear weight, divided by 1 + 16 x (gap squared), where gap = |sample depth - pixel depth| / (1 + 0.01 x pixel depth). The output is the weighted average. Not exact at silhouettes and creases.

Scissor rectangle (merged, plain path): the rectangle is the screen box where the lamps' spheres project (the camera's right, up and forward axes come from the view matrix; the projection must be a plain perspective, or the whole pass is drawn). It is skipped entirely when the list of lamps is empty. With extra lamps present, the whole pass is drawn. In a rect frame (RenderScale), the rectangle is scaled by the rect's scale factors and clamped to the rect.

Rect frames (RenderRect, TESR_RenderScale): the Vulkan route is not used in a rect frame. The contact pass maps its screen positions into the rect before it reads depth, so the rect's depth is read. The D3D9 pass itself is unaware of the rect.

Jitter (TAA): the pass uses the uv it is given and does not remove any jitter itself. Whether the uv it gets is jittered is not checked here (section 9).

Reversed depth: the depth is the linear Vegas Shaders depth (not a reversed buffer). The pass does not handle reversed depth.

HDR vs LDR: the scene is HDR and this pass does not touch scene colour. The buffer is G16R16 (unsigned normalised, 0 to 1), so the mask is clamped to 0..1 on write, and the additive batches are clamped too. The contact multiply can only darken.

NaN and infinity guards: the lamp-radius division gives infinity for a zero radius; the test "not less than 1" rejects it. The final sum is saturated. The contact pass divides by the distance to the lamp without a guard (section 9).

Things that look wrong if missed:
- A lamp's shadow filling the room when you stand next to it (NearFade 0 leaves it sharp and total at the lamp).
- A hard circle at the edge of a bright lamp's reach (EdgeFade 0).
- Facets on characters (forward mode 0 or the surface-angle term left on).
- The room darkened twice (reach mode and the game's own falloff both applied).
- Contact shadows on the first-person weapon (the 25-unit fade must stay).
- The buffer not cleared before the merged technique (a stale mask shows through).
- Outdoor batch slots not swapped back (the lamp list is restored after the batches).

## 7. Vulkan path

### 7.1 Files

| File | Status | Role |
|---|---|---|
| src\hlsl\NewVegas\Compute\PointShadows.frag | source | Modes 0, 1 and 2 of MergedPointShadows, AddShadowedLamps and LampMaskHalf. The mode comes from the push block (general shader), or from a compile-time define (variant) |
| src\hlsl\NewVegas\Compute\PointShadowsCommon.glsl | source | Shared constants block and the shadow, attenuation, spot, and depth and normal helpers (the same maths as the effect file) |
| src\hlsl\NewVegas\Compute\PointShadowsContact.frag | source | Technique 2 (LampContact) |
| src\hlsl\NewVegas\Compute\PointShadowsUpsample.frag | source | Technique 4 (LampMaskUpsample) |
| Compute\PointShadows.spv | binary, built from PointShadows.frag | general shader (mode from push block) |
| Compute\PointShadows-m0.spv, -m1.spv, -m2.spv | binary, built from PointShadows.frag with the mode defined | mode variants |
| Compute\PointShadows-m0x.spv, -m0y.spv, -m0z.spv, -m0e.spv | binary, built from PointShadows.frag with test defines | test variants listed in Compute\variants.txt (no cube reads; no shadowed lamps; no lamps; empty). Not for play |
| Compute\PointShadowsContact.spv | binary, from PointShadowsContact.frag | technique 2 |
| Compute\PointShadowsUpsample.spv | binary, from PointShadowsUpsample.frag | technique 4 |
| Compute\Sm6\PointShadows.MergedPointShadows.0.811c9dc5.spv (with .txt and the index .txt) | SM6 build of technique 0 | used only with NvrSm6Shaders (default off). Its source is the effect file's technique 0 |

PointShadows2.frag and PointShadows2.spv belong to the separate PointShadows2 effect and are not part of this spec. No variant files exist for the contact and upsample passes.

Wave 3 (2026-11-02): PointShadows.spv, the four -m variants and the five test variants, PointShadowsContact.spv and PointShadowsUpsample.spv were rebuilt from the new sources with tools/nvr-dxvk/compile-compute.sh (spirv-val passes). The Sm6 build of technique 0 is NOT rebuilt: it is from the old technique 0 and must not be used until it is rebuilt from the new .fx (open; NvrSm6Shaders is off by default).

### 7.2 How the C++ chooses D3D9 or Vulkan

Checked in the order the engine does it (src\core\EffectRecord.cpp, the effect's Render path; src\core\DxvkCompute.cpp; src\core\Sm6Pass.cpp):
1. The Vulkan draw is tried first when the passes are on: NvrDxvkPasses on, not a rect frame, and the Vegas Shaders DXVK extension available. Those are the conditions of DxvkCompute's passes-on check.
2. The effect is skipped from Vulkan when NvrDxvkKeepD3D9 names it ("PointShadows").
3. The Vulkan draw is skipped for a technique whose passes all have SM6 builds while NvrSm6Shaders is on (technique 0 only has one).
4. The effect's Vulkan draw returns false (so the D3D9 technique runs for that draw) when: technique index above 4; the extension or the linear and point samplers are missing; the scissor test is on (the Vulkan passes ignore the scissor); a texture is missing; a required constant is missing.
5. If a pass fails to build or draw, the effect falls back to its D3D9 technique for the rest of the session (logged).

### 7.3 Variant choice

For each Vulkan draw the file name is chosen in this order (EffectRecord's variant lookup):
1. When NvrDxvkTestSuffix is not empty: name with the suffix, e.g. PointShadows-m0 plus the suffix, if that file exists.
2. When NvrDxvkVariants is on and the draw has a mode suffix: the mode variant (PointShadows-m0, -m1 or -m2), if the file exists.
3. Otherwise the general file (PointShadows.spv, PointShadowsContact.spv or PointShadowsUpsample.spv).

The merged, batch and half-mask draws use mode 0, 1 and 2 respectively. The contact and upsample draws look for a mode-0 variant file, which does not exist, so they use the general file.

### 7.4 Bindings and push block (what the engine fills)

Texture bindings, set 1, binding number equal to the list position (the cube maps are the first entries):
- Merged, batch and half-mask draws: binding 0 to 10 = the cube maps for lamp slots 0 to 10 (cube view). Binding 11 = depth (2D). Binding 12 = normals (2D).
- Contact draw: binding 0 = depth, binding 1 = normals.
- Upsample draw: binding 0 = NVR_LampMaskHalf (its half-resolution mask), binding 1 = depth.

Constants: set 2, binding 0, one uniform block with std140 layout, all vec4 unless stated. Byte offsets, in order:

| Offset (bytes) | Name (as in section 3) | Count |
|---|---|---|
| 0 | TESR_ShadowLightPosition | 12 |
| 192 | TESR_LightPosition | 12 |
| 384 | TESR_LightColor | 24 |
| 768 | TESR_ExtraLights | 16 |
| 1024 | TESR_ShadowFade | 1 |
| 1040 | TESR_PointShadowNear | 1 |
| 1056 | TESR_SpotLightPosition | 1 |
| 1072 | TESR_SpotLightDirection | 1 |
| 1088 | TESR_SpotLightColor | 1 |
| 1104 | TESR_LampContact | 1 |
| 1120 | TESR_LampContactFade | 1 |
| 1136 | TESR_LampContactBias | 1 |
| 1152 | TESR_LampContactNoise | 1 |
| 1168 | TESR_ReciprocalResolution | 1 |
| 1184 | TESR_CameraPosition | 1 |
| 1200 | TESR_CameraData | 1 |
| 1216 | TESR_ViewTransform | mat4 (64 bytes) |
| 1280 | TESR_ProjectionTransform | mat4 (64 bytes) |
| 1344 | per-shadow-lamp derived values | 12 |
| 1536 | per-tracked-lamp derived values | 12 |
| 1728 | per-extra-lamp derived values | 16 |
| 1984 | near-fade derived values | 1 |
| 2000 | spot cone derived values | 1 |

Total block size 2016 bytes. The engine fills the derived values once per draw from the constants above:
- Per shadow lamp k (offset 1344): x = 1 / radius; y = weight (luma of TESR_LightColor entry k times its w); z = radius / NearFade (0 when NearFade is 0).
- Per tracked lamp i (offset 1536): x = 1 / radius; y = weight (luma of entry i+12 times its w).
- Per extra lamp e (offset 1728): x = 1 / radius, where radius = floor(packed w / 8); y = weight = packed w minus 8 x radius.
- Near-fade block (offset 1984): x = 1 / EdgeFade (0 when EdgeFade is 0); y = 1 / (1 - reach value z); z = 1 / spot radius; w = luma of spot colour times its w.
- Spot block (offset 2000): x = cos(full cone angle); y = 1 / (cos(half angle) - cos(full angle)).

Push constants (16 bytes, read by the shader as four unsigned ints):
- Byte 0: heap index of the linear clamp sampler (all draws use it for depth; merged, batch and half-mask draws also for the cube maps).
- Byte 4: heap index of the point clamp sampler (normals; contact draw). For the upsample draw this slot holds a point WRAP sampler (for the half-resolution mask).
- Byte 8: mode: 0 = merged (also used by contact and upsample), 1 = batch, 2 = half-resolution mask.
- Byte 12: padding.

Render target formats: point-shadow buffer G16R16; half-resolution mask G16R16F. Clear to black before the merged, half-mask and scissored draws, as the D3D9 pass does.

Blend: technique 1 = additive, technique 2 = multiply (destination x source), others none.

Sampler states (the effect file's): depth linear clamp; normals point clamp; cube maps linear clamp; NVR_LampMaskHalf point (see section 9 for the address mode).

Vulkan route must draw the same picture as the D3D9 technique. Known test variants exist only to measure cost.

## 8. Look and cost

Correct look:
- Indoors: a lamp's light is cut where props and walls block it, giving soft-edged shadows that follow the lamp's position. Rooms keep their mood (KeepDarkening) and the far part of a lit room is not darkened twice.
- Outdoors at night with point shadows on: street lamps and campfires cast shadows where things stand between them and the surface; open country by day gets no mask at all (scissor skips it).
- Characters keep their smooth shading, with no facets (forward mode 2 or the surface-angle term off).
- No hard circle at the edge of a bright lamp's reach.
- Lamp contact (when ContactShadows is above 0): small dark lines where a prop meets the floor or a wall meets a table, faded out with distance, none on the weapon.

Typical wrong results:
- A shadow that fills the room when standing at a lamp (NearFade 0).
- A hard disc edge around a bright lamp (EdgeFade 0).
- Triangle-shaped facets on skin or clothes near a lamp (surface angle on).
- Dark gaps where no lamp is blocked (a stale buffer, or an unset clear).
- Grain on the contact lines (noise with DarkeningBlur on, unfiltered).
- A contact shadow on the weapon (fade start changed).

Cost class: multi-pass, per-pixel loop. The merged technique reads up to 11 cube maps and loops over up to 12 tracked lamps and 16 extra lamps, with branches on each. The batch technique adds about 0.3 ms at 1440p per batch of 11 lamps (from the settings file's note). The contact pass is a second per-pixel pass with a 6-step march. Measured figures in the code notes: LampMaskScissor saves about 0.15 to 0.37 ms at 1440p; the derived per-draw values in the Vulkan route were measured at 0.3 to 0.5 ms cheaper than per-pixel divisions on a GTX 1070 in the Vulkan merged pass.

## 9. Open questions

1. Sampler address mode of NVR_LampMaskHalf: the effect file declares CLAMP on both axes, but the C++ comment and the Vulkan upsample shader (point WRAP) say WRAP. Still open: wave 3 keeps CLAMP (the .fx is the reference). The difference is only at the screen edge.
2. [Shaders.ShadowsInteriors.Main] Enabled is read (as Settings.Interiors.Enabled) but has no key in resource\VegasShaders.defaults.toml. Its effect on this pass was not traced.
3. Outdoors, the pass runs whenever the exterior shadow effect is enabled, even with UsePointShadowsDay and Night both off (cube reads then off). It is not clear whether the mask then gives unshadowed lamp light outdoors on purpose, and whether that costs anything with nothing to show.
4. Disabled effect: when the effect's own Enabled flag is off, its Render returns before the clear. The buffer then keeps the previous frame's contents, so the mask may not be cleared. Not checked in a run.
5. FIXED (wave 3): contact pass: the distance to the lamp is clamped to 1e-6 before dividing (no NaN at a zero distance). The lamp selection also skips zero-radius lamps before any division.
6. Meaning of the buffer's value for consumers: this spec describes it as the sum of lamp light reaching the pixel, 0 to 1. Consumers (the exterior and interior shadow effects, ambient light, snow, blood flow and others) darken by it; their exact use is outside this spec.
7. TAA jitter: whether the uv this pass receives is jittered before it runs was not checked.
8. Vulkan matrix order: the shared Vulkan helper says its matrix products equal the effect file's row-vector products. That was read, not run.
9. The contact and upsample draws look for a mode-0 variant file that does not exist. Harmless, but it is a wasted lookup. Still open (a C++ detail, not changed in wave 3).
10. FIXED (wave 3): the Vulkan binaries are rebuilt from the current sources (spirv-val passes). The Sm6 build is still old (see section 7.1).
13. Wave 3 readings (the spec was not exact here; the shader decides): (a) the contact blocker's contribution is the largest (1 - t) times the traced lamp's share of all slots' light, times fade and strength; (b) the spot cone uses the direction from the spot to the surface against the spot direction, clamped at 0; (c) tracked and extra lamps use the same factor as the normal lamps (reach, forward mode 2 or normal, as the value z says); (d) the near fade blends the shadow toward 1 near the lamp: result = lerp(1, shadow, smoothstep(0, NearFade / radius, stored)), the reading that gives "none right at the lamp".
14. Wave 3 choice: the contact pass is computed in view space (the depth and normals are view space). The Vulkan passes use the same matrix convention: D3D mul(v, M) is M * v in GLSL, and D3D mul(M, v) is v * M.
11. The LightRadiusMult reach: the settings file says the lamp shadows cover LightRadiusMult times the game radius, and the reach mode fades over the rest of that. Where the multiplication happens (the radius sent in TESR_ShadowLightPosition or TESR_LightPosition) was not traced.
12. Engine default for NvrDxvkPasses is false in code but true in the defaults file; the defaults file wins in normal play. Not a problem for a new writer, but worth knowing.
