// Grass PS -- vanilla GRASS23x000TMS.pso plus forward sun shadow.
// Shared by GRASS23x002.vso (LFS) and GRASS23x003.vso (LVS).
// The VS hands the lighting over already split, so only the sun is shadowed:
//   TEXCOORD4.xyz ambient    TEXCOORD5.xyz sun    TEXCOORD5.w fade    COLOR0 fog (.w amount)
// UNOFFICIAL 2026-11-04 (from Physically-Based-Reloaded; in place of PointLights, the game's 1POINT grass shaders, whose version is
// in VegasShaders-backups/before_grass_port_20261104): the grass's brightness and night brightness, the placed lamps and the
// Pip-Boy light on every tuft, and normal maps with the sun's highlight. PBR NVR's c146/c147/c160-c176 are the sun cross-fade's
// here: c80-c98 instead (nothing else in the grass PS is below c100).

// UNOFFICIAL 2026-11-16 fast-restart (testers: grass and leaves glowing under a cloud's shadow; docs/next/FAST-RESTART.md): the clouds' shadow
// on the sun term with ForwardShadows on, as every object (Includes/Shadow.hlsl SHADOW_CLOUD_IN; strength 0 with PlantShadows off). Not in
// the deferred record (FOLIAGE_DEFERRED): DeferredSun shades its sun.
#if !(defined(FOLIAGE_DEFERRED) && FOLIAGE_DEFERRED)
    #define SHADOW_CLOUD_IN 1
#endif
#include "includes/Shadow.hlsl"
#include "includes/PBRScale.hlsl"
// UNOFFICIAL 2026-11-12 grass records (docs/next/PT-COMPAT-AUDIT-2026-11-12.md item 1; Includes/CapturedNormals.hlsl CAPTURED_FOLIAGE;
// core/CapturedNormals.h GrassWriters, [Main.Main.Quality] CapturedGrass): with the AlbedoBuffer compiled in, the grass writes its own
// colour, material and normal records (COLOR1-3; see main's end). Without it this shader is exactly as before.
#if defined(CAPTURED_NORMALS_ON) && CAPTURED_NORMALS_ON >= 3
    #include "includes/CapturedNormals.hlsl"
    #if defined(FOLIAGE_DEFERRED) && FOLIAGE_DEFERRED && CAPTURED_NORMALS_ON >= 4
        #include "includes/FoliageDeferred.hlsl"	// UNOFFICIAL 2026-11-14 the deferred record (main's end)
    #endif
#endif

uniform sampler2D DiffuseMap : register(s0);
// UNOFFICIAL 2026-10-27 [Shaders.Grass.Main] w: +1 ShadowFix (the sun shadow lookup takes the world up normal, see main),
// +2 SkyLightScale (the sky light takes the tuft's light scale, as the game's ambient does).
float4 TESR_GrassScale;
#ifdef GRASS_ALPHA_TEST
// UNOFFICIAL 2026-10-27 (user: no sun shadow on grass with the game's MSAA off, needed for FSR's RenderScale): GRASS23x000.pso,
// the game's grass PS without transparency multisampling, includes this file too. The game's own differs from its TMS twin only in
// the alpha: a hard cut against AlphaTestRef (c3) for the alpha test instead of the coverage ramp (alpha x 1.75).
uniform float4 AlphaTestRef : register(c3);
#ifdef ALPHA_HASH_ON
    #include "includes/AlphaHash.hlsl"	// UNOFFICIAL 2026-11-03 SmoothCutouts: the cut hashed under TAA / FSR / DLSS (see main)
#endif
#endif

// The grass brightness settings ([Shaders.Grass.Main]): x Brightness multiplies the lit grass colour, before fog; y the night
// brightness (NightBrightness eased by the day/night transition) on the sun and ambient.
// UNOFFICIAL 2026-11-12 CapturedGrass (the records at main's end only): z 1 - CapturedGrassShade, w the material class written (0: 7).
float4 TESR_GrassBrightness : register(c80);
// UNOFFICIAL 2026-11-12 CapturedGrassTint (the colour record at main's end only; a build without the records has no such constant):
// x [Main.Main.Quality] CapturedGrassColour (multiplies the recorded colour), y CapturedGrassSaturation (1 the colour's own, 0 grey),
// z CapturedGrassTint (0-1; also 0 while CapturedGrass is off: the maths is skipped). c79: right under the grass's own c80-c98, used
// by nothing this shader includes, and never one of the low registers the game itself may write for a grass draw.
#ifdef CAPTURED_FOLIAGE
float4 TESR_GrassRecord : register(c79);
#endif
// Normal maps and specular ([Shaders.Grass.NormalMaps]; effects/Grass/Grass.cpp OnGrassShape): a grass texture's "_n" companion on
// s1, set per shape with GrassMaterial: x 1 when the map is bound, y its strength, z specular strength (0 off), w glossiness. Not a
// TESR_ constant: written per shape, not per shader.
float4 GrassMaterial : register(c81);
sampler2D NormalMap : register(s1);
// Lamps on the grass ([Shaders.Grass.Lights]; effects/Grass/Grass.cpp UpdateLights): placed lights and the Pip-Boy light, never the
// sun. x: how many of the slots below are live (0 skips them all), y: Strength. Positions camera-relative, as shadowWorldPos, w the
// radius; colours with the dimmer applied.
#define GRASS_LIGHTS 8
float4 TESR_GrassLightData                   : register(c82);
float4 TESR_GrassLightPosition[GRASS_LIGHTS] : register(c83);
float4 TESR_GrassLightColor[GRASS_LIGHTS]    : register(c91);

// The lamps' light on a blade: vanilla's falloff through LampAtt (matching objects and LampFalloff.Exterior), per-light linear
// decode (PBRLight inside the loop so overlapping lights sum linearly instead of exploding via pow(sum, gamma)), and wrapped diffuse
// without abs(dot) so lights below the ground do not glow upward through terrain.
float3 GrassLamps(float3 position, float3 normal) {
    float3 lamps = 0.0f;
    const int count = (int)min(TESR_GrassLightData.x, (float)GRASS_LIGHTS);
    [loop] for (int i = 0; i < count; i++) {
        float3 toLight = TESR_GrassLightPosition[i].xyz - position;
        float d2 = dot(toLight, toLight);
        float radius = TESR_GrassLightPosition[i].w;
        float att = saturate(1.0f - d2 / (radius * radius));
        [branch] if (att > 0.0f) {
            float3 dir = toLight * rsqrt(max(d2, 1.0f));
            float wrap = saturate((dot(normal, dir) + 0.35f) / 1.35f);
            lamps += PBRLight(TESR_GrassLightColor[i].rgb) * (LampAtt(att) * wrap);
        }
    }
    return lamps * LinScale(TESR_GrassLightData.y);
}

// UNOFFICIAL 2026-11-04 the deferred flashlight on the grass (Grass.cpp UpdateLights, core/FlashlightDeferred.h LightParams): the light pass
// lights what the G-buffer holds, which is no grass. [0] the light, camera-relative, w its reach (0: off); [1] the beam's axis, w cos of the
// cone's outer edge; [2] rgb linear light (times [Shaders.Flashlight.Deferred] GrassStrength), w cos of the full-strength edge. The lamps'
// falloff and wrapped diffuse, times the light pass's cone; no shadow.
float4 TESR_GrassSpot[3] : register(c99);
// 2026-11-04 its shadow, as the light pass takes it (FlashlightDeferred.hlsl FLShadow): the spot map (r the distance to the light over its
// reach), the matrix from camera-relative world to its clip space (ShaderManager's SpotLightWorldToLightMatrix[0], live at the bind), and
// x 1 = the map was drawn this frame, y its texel size, z the depth bias (share of the reach), w the PCF radius (texels).
row_major float4x4 TESR_SpotLightToWorldTransform : register(c102);
float4 TESR_GrassSpotShadow : register(c106);
float4 TESR_GrassSpotFalloff : register(c107);	// x [Shaders.Flashlight.Deferred] Falloff (0: the lamps' falloff)
sampler2D TESR_ShadowSpotlightBuffer0 : register(s7) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
float GrassSpotShadow(float3 position, float distShare) {
    float4 clipPos = mul(float4(position, 1.0f), TESR_SpotLightToWorldTransform);
    float2 mapUV = clipPos.xy / max(clipPos.w, 1.0e-4f) * float2(0.5f, -0.5f) + 0.5f;
    [branch] if (clipPos.w <= 0.0f || any(mapUV < 0.0f) || any(mapUV > 1.0f)) return 1.0f;
    // 2026-11-04 PCF over the light pass's disc (FLShadow): 16 taps on a Vogel spiral, point taps here (the grass draws many layers)
    float radius = TESR_GrassSpotShadow.y * TESR_GrassSpotShadow.w;
    float bias = TESR_GrassSpotShadow.z * (1.0f + 4.0f * distShare);
    float2 dir = float2(1.0f, 0.0f);
    const float2x2 golden = float2x2(-0.7373688f, -0.6754904f, 0.6754904f, -0.7373688f);
    float lit = 0.0f;
    [unroll] for (int i = 0; i < 16; i++) {
        lit += tex2Dlod(TESR_ShadowSpotlightBuffer0, float4(mapUV + dir * (radius * sqrt((i + 0.5f) / 16.0f)), 0.0f, 0.0f)).r + bias > distShare ? 1.0f : 0.0f;
        dir = mul(golden, dir);
    }
    return lit / 16.0f;
}
float3 GrassSpot(float3 position, float3 normal) {
    float3 toLight = TESR_GrassSpot[0].xyz - position;
    float d2 = dot(toLight, toLight);
    float share2 = d2 / (TESR_GrassSpot[0].w * TESR_GrassSpot[0].w);
    float att = saturate(1.0f - share2);
    // 2026-11-04 the light pass's Falloff (FlashlightDeferred.hlsl): a windowed inverse square while > 0
    [flatten] if (TESR_GrassSpotFalloff.x > 0.0f) att = att * att / (1.0f + TESR_GrassSpotFalloff.x * share2);
    float3 dir = toLight * rsqrt(max(d2, 1.0f));
    float cone = smoothstep(TESR_GrassSpot[1].w, TESR_GrassSpot[2].w, dot(-dir, TESR_GrassSpot[1].xyz));
    float wrap = 0.5f + 0.5f * abs(dot(normal, dir));
    float shadow = 1.0f;
    [branch] if (TESR_GrassSpotShadow.x > 0.5f && att * cone > 0.0f) shadow = GrassSpotShadow(position, sqrt(d2) / TESR_GrassSpot[0].w);
    return TESR_GrassSpot[2].rgb * (att * cone * cone * wrap * shadow);
}

// The normal with the normal map: the grass meshes carry no tangents, so the frame comes from how the position and the UVs change
// across the screen (Schuler's cotangent frame), around the normal vanilla lights the grass by. The map's slope tilts that normal;
// at strength 0 or a flat map it is vanilla's normal again. w: the map's alpha, the specular mask.
float4 GrassMappedNormal(float3 N, float2 uv, float3 dp1, float3 dp2, float2 duv1, float2 duv2) {
    float3 dp2perp = cross(dp2, N);
    float3 dp1perp = cross(N, dp1);
    float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    float3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float frame = rsqrt(max(max(dot(T, T), dot(B, B)), 1e-20f));
    float4 m = tex2Dgrad(NormalMap, uv, duv1, duv2);
    m.xyz = m.xyz * 2.0f - 1.0f;
    return float4(normalize(N + (T * m.x + B * m.y) * (frame * GrassMaterial.y)), m.a);
}

struct PixVaryings {
    float2 f_uv             : TEXCOORD0;
    float4 f_shadow_world_pos : TEXCOORD1;
    float4 f_light_normal   : TEXCOORD2;   // xyz the normal the sun lights the grass by, w its N.L (without the translucency)
    float3 f_sun_dir        : TEXCOORD3;
    float3 f_ambient        : TEXCOORD4_centroid;
    float4 f_sun            : TEXCOORD5_centroid;   // .w = distance fade
    float4 f_fog            : COLOR0;               // .w = fog amount
    float4 f_amount         : TEXCOORD6_centroid;   // UNOFFICIAL: x ambient amount, y sun amount, z the sun amount before N.L, w the translucency (plain numbers: not decoded)
#ifdef CAPTURED_FOLIAGE
    float3 f_sun_colour     : TEXCOORD7;            // UNOFFICIAL 2026-11-12 CapturedGrassTint: the sun's colour without the vertex colour (GRASS23x00N.vso; undefined under the game's VS)
#endif
};

struct PixResult {
    float4 f_color : COLOR0;
#ifdef CAPTURED_FOLIAGE
    CAPTURED_FOLIAGE_OUTPUTS	// UNOFFICIAL 2026-11-12 grass records (discarded where nothing is bound there)
#endif
};

PixResult main(PixVaryings pa, float2 vpos_s : VPOS) {
    PixResult pr;

    const bool nvr_vs = SHADOW_VS_PRESENT(pa.f_shadow_world_pos.w);
    // The VS's sun amount (N.L included), or per pixel through the normal map. The derivatives stay at top level, outside dynamic
    // flow control.
    float3 dp1 = ddx(pa.f_shadow_world_pos.xyz), dp2 = ddy(pa.f_shadow_world_pos.xyz);
    float2 duv1 = ddx(pa.f_uv), duv2 = ddy(pa.f_uv);
    float4 normal = float4(normalize(pa.f_light_normal.xyz), 1.0f);   // w the specular mask
    float sun_base = pa.f_amount.y;
    [branch] if (GrassMaterial.x > 0.0f && nvr_vs) {
        normal = GrassMappedNormal(normal.xyz, pa.f_uv, dp1, dp2, duv1, duv2);
        sun_base = pa.f_amount.z * (saturate(dot(pa.f_sun_dir, normal.xyz)) + pa.f_amount.w);
    }

    // Outside the guard: the skylight needs this normal whether or not forward shadows
    // are compiled in, and ForwardShadows is a live setting that can switch them off.
    float3 shadow_normal = GetShadowGeometricNormal(pa.f_shadow_world_pos.xyz);
    float sun_shadow = 1.0f;
#if FORWARD_SHADOWS
    // UNOFFICIAL 2026-10-27 ShadowFix (tester: zebra stripes on each tuft, shaded as if the sun came from the other side): the
    // derivative normal is turned to face the CAMERA, and a grass card is two-sided, so seen with the sun behind it the normal
    // points away from the sun and the normal offset pushes the lookup into the tuft's own shadow (mirrored), and the per-triangle
    // derivatives of thin, swaying blades flip from blade to blade (stripes). Grass has no surface of its own to bias from: take the
    // ground's up normal, the same for the whole tuft, as the ground it stands on. Off: as before.
    float3 lookup_normal = fmod(TESR_GrassScale.w, 2.0f) > 0.5f ? float3(0.0f, 0.0f, 1.0f) : shadow_normal;
    // ddx/ddy must stay at top level, outside dynamic flow control.
    sun_shadow = nvr_vs ? GetSunShadow(pa.f_shadow_world_pos.xyz, lookup_normal) : 1.0f;
#endif
    float sun_amount = sun_base * sun_shadow;

    // Same split getSunLighting/getAmbientLighting apply on the object path.
    // UNOFFICIAL linear lighting (2026-10-14): only the colours are decoded; the amounts multiply after (see the VS).
    // UNOFFICIAL 2026-10-27 SkyLightScale (tester: grass in shadow a little too bright): the game darkens each tuft's ambient by its
    // own light scale (0.25-1, IN.f_amount.x, packed per tuft), but the sky light NVR adds took the full amount. In shadow the sky is
    // most of what is left, so shaded tufts came out brighter than the game's own balance. Off: the full sky light, as before.
    float sky_scale = TESR_GrassScale.w > 1.5f ? pa.f_amount.x : 1.0f;
    float3 light_term = PBRLight(pa.f_sun.xyz) * sun_amount + PBRAmbient(pa.f_ambient.xyz) * pa.f_amount.x + SkyAmbient(shadow_normal, nvr_vs ? sky_scale : 0.0f);

    // The sun's highlight (Blinn-Phong), masked by the normal map's alpha where there is one. The camera sits at the origin of the
    // camera-relative position, which only NVR's VS hands over.
    float3 specular = 0.0f;
    [branch] if (GrassMaterial.z > 0.0f && nvr_vs) {
        float3 L = normalize(pa.f_sun_dir);
        float3 H = normalize(L - normalize(pa.f_shadow_world_pos.xyz));
        float highlight = pow(saturate(dot(normal.xyz, H)), GrassMaterial.w) * saturate(dot(normal.xyz, L));
        specular = PBRLight(pa.f_sun.xyz) * (pa.f_amount.z * sun_shadow * highlight * GrassMaterial.z * normal.w);
    }

    // The night brightness ([Shaders.Grass.Main] NightBrightness), on the sun/moon and ambient only: lamps keep theirs.
    light_term *= TESR_GrassBrightness.y;
    specular *= TESR_GrassBrightness.y;
    [branch] if (TESR_GrassLightData.x > 0.0f && nvr_vs)
        light_term += GrassLamps(pa.f_shadow_world_pos.xyz, normal.xyz) * pa.f_amount.x;
    [branch] if (TESR_GrassSpot[0].w > 0.0f && nvr_vs)	// (linear light already: no PBRLight)
        light_term += GrassSpot(pa.f_shadow_world_pos.xyz, normal.xyz);

    float4 base_rgb = tex2D(DiffuseMap, pa.f_uv.xy);
    float3 lit_color = LinEncode((light_term * LinColor(base_rgb.rgb) + specular) * TESR_GrassBrightness.x);	// UNOFFICIAL linear lighting: decoded albedo, encoded result (PBRScale.hlsl decodes the light)

    pr.f_color.rgb = lerp(lit_color, pa.f_fog.rgb, pa.f_fog.w);
    // UNOFFICIAL grass fade: the game fades grass by scaling its alpha with distance (TEXCOORD5.w from the VS,
    // fGrassStartFadeDistance / fGrassFadeRange), but grass is alpha-TESTED, so a tuft keeps its full cut-out until
    // the scaled alpha crosses the test threshold and then vanishes in one step: the pop at the fade distance.
    // Dissolve with a screen-space dither instead: the share of pixels that survive follows the fade, so a tuft
    // thins out across the whole fade range. The cut-out alpha itself is unchanged.
    float dither_v = frac(52.9829189f * frac(dot(vpos_s, float2(0.06711056f, 0.00583715f))));
    clip(pa.f_sun.w - dither_v);
#ifdef GRASS_ALPHA_TEST
    pr.f_color.a = base_rgb.a > AlphaTestRef.x ? 1.0f : 0.0f;	// the game's cut (its fade is the dither above, as in the TMS build)
#ifdef ALPHA_HASH_ON
    // UNOFFICIAL 2026-11-03 SmoothCutouts (Includes/AlphaHash.hlsl): on a hashed draw the cut takes noise; mode 2 softens the
    // blade edge around AlphaTestRef while zeroing transparent DXT5 background fringe texels (prevents off-color chroma fireflies).
    const float grass_style = AlphaHashStyle();
    const float grass_width = max(fwidth(base_rgb.a) * (TESR_AlphaHash.z - 16.0f * grass_style), 1.0f / 255.0f);
    if (NVR_AlphaHashDraw) {
        if (TESR_AlphaHash.x > 0.5f) {
            const float grass_band = grass_style > 5.5f ? 3.0f : grass_style > 4.5f ? 1.5f : grass_style > 2.5f ? 2.0f : 1.0f;
            const float grass_soft = saturate((base_rgb.a - AlphaTestRef.x) * grass_band + AlphaTestRef.x);
            const float coverage = TESR_AlphaHash.x > 1.5f
                ? (base_rgb.a < AlphaTestRef.x * 0.25f ? 0.0f : grass_soft)
                : saturate((base_rgb.a - AlphaTestRef.x) / grass_width + 0.5f);
            pr.f_color.a = AlphaHashKeep(coverage, vpos_s, 37.0f) > 0.0f ? 1.0f : 0.0f;
        }
    }
#endif
#else
    pr.f_color.a = saturate(base_rgb.a * 1.75f);
#endif

#ifdef CAPTURED_FOLIAGE
    // UNOFFICIAL 2026-11-12 grass records (audit item 1: under the path tracer each blade was a per-pixel mix of the ground's record, the
    // traced tufts and the game's pixel, since the grass wrote depth and nothing else). The colour the light is multiplied by (the texture,
    // decoded, times [Shaders.Grass.Main] Brightness, which multiplies the lit colour), class 5 (plants) with a matte roughness, and the
    // normal the game lights the tuft by (before the normal map, as the templates' smooth normal) with the camera distance. Written only
    // where this pixel counts as grass: Vegas Shaders' vertex shader (the position is its) and the cut-out's alpha at 0.5 or more (1 or 0
    // with the alpha test build; pixels the alpha test or the fade's clip removes write nothing anywhere). Grass is drawn with the
    // SRCALPHA / INVSRCALPHA blend: alpha 0 leaves the three texels as they were.
    // UNOFFICIAL 2026-11-12 CapturedGrass, second pass ("grass records that are right"; an RTX run of the lines above: bright orange-yellow
    // speckles on the tufts that changed every frame). What was wrong, and what is written now:
    //  - WHERE: the record was written at alpha 0.5 or more only, the depth wherever the pixel survives (the alpha test's own, lower
    //    reference; alpha to coverage): a blade's soft rim had the grass's depth over the GROUND's record. Now every pixel that survives
    //    writes it (1 / 255: never a texel with no alpha at all, should a draw come without the alpha test).
    //  - COLOUR: the texture alone, so every tuft counted as fully lit ground cover. The game darkens each tuft by its own light scale
    //    (0.25-1, f_amount.x, a plain amount: on the sun and the ambient alike), which is the tuft's share of the light: part of the colour
    //    the light is multiplied by. TESR_GrassBrightness.z = 1 - [Main.Main.Quality] CapturedGrassShade (0: all of it, the default).
    //  - MATERIAL: class 5 (plants) turned the path tracer's thin-leaf model on (light through the card, a second light sample from
    //    behind, 29 % of the bounce rays sent through it): a model for a card lit by its own face, not for a tuft lit as the ground it
    //    stands on. Class 7 now (ground cover: every reader takes an unknown class as a plain matte surface), 5 again with
    //    CapturedGrassPlant (TESR_GrassBrightness.w: the class; never set = 7). g = 0.5 marks grass (the templates write whole numbers
    //    there, the draw's radius): core/CapturedNormals.h MergeIntoNormals takes this normal for the marked pixels.
    //  - NORMAL: the tuft's lighting normal was right (one per tuft, its axis: about the ground's normal), but CapturedNormalOut turns a
    //    normal that faces away from the camera over: a tuft above the eye (a slope, tall grass close by) was recorded pointing DOWN, and
    //    the line where that happens crosses the tufts. It is bent toward the eye instead, just enough to face it (0.15): no flip, and
    //    the same for the whole tuft. (That nobody READ this normal was the larger fault: see MergeIntoNormals.)
    //  - DISTANCE: the pixel's own (the vertex shader rebuilds the position from the clip position, wind and bending included): as before.
    const float grass_written = nvr_vs && pr.f_color.a > (1.0f / 255.0f) ? 1.0f : 0.0f;
    const float grass_shade = lerp(pa.f_amount.x, 1.0f, saturate(TESR_GrassBrightness.z));
    const float grass_class = TESR_GrassBrightness.w > 0.5f ? TESR_GrassBrightness.w : 7.0f;
    float3 grass_n = pa.f_light_normal.xyz * rsqrt(max(dot(pa.f_light_normal.xyz, pa.f_light_normal.xyz), 1.0e-12f));
    const float3 grass_v = -pa.f_shadow_world_pos.xyz * rsqrt(max(dot(pa.f_shadow_world_pos.xyz, pa.f_shadow_world_pos.xyz), 1.0e-12f));
    grass_n += grass_v * max(0.15f - dot(grass_n, grass_v), 0.0f);
    // UNOFFICIAL 2026-11-12 CapturedGrassTint, third pass ("the grass records' colour"; an RTX run of the second, Goodsprings at noon: the
    // tufts steady, but bright yellow-orange under the path tracer where the game shows a muted brown; the self-check: grass 0.181 0.121
    // 0.056 against the land's 0.125 0.107 0.081). What was missing is the grass mesh's VERTEX COLOUR. The land's and the objects' records
    // are texture x vertex colour; the grass's was the texture alone. The game multiplies the grass's vertex colour into the SUN only
    // (f_sun = vertex colour x sun colour; the ambient and the sky light are untinted), and it is mostly a darkening: the game's own tufts
    // are white at the tips and 0.2-0.6 grey-brown at the roots (decoded: about 0.3-0.45 of the sun over a card), a darker-grass mod's
    // 0.18 down to 0.03 (decoded 0.02: no sun to speak of, the tuft is lit by the blue ambient alone: the muted brown). The path tracer
    // has one colour per pixel for all its light, so the record is the colour that gives the game's result under the same sun and sky:
    //     texture x (vertex colour x sun x N.L + ambient + sky) / (sun x N.L + ambient + sky)        per channel, unshadowed
    // the share of the light an untinted surface gets there that the game leaves the tuft: the vertex colour itself in full sun with no
    // ambient, 1 (the texture alone, as before) with no sun: at night's end, indoors, under the game's own vertex shader. Per channel, so
    // the warm sun's missing share also takes the orange out, as it does in the game (no blind desaturation). N.L and the grass dimmer
    // as the vertex shader worked them out (f_amount.y over the tuft's light scale: one value per tuft, no normal map, no shadow: a
    // shadow here would bake the raster shadow map's edge into the colour). The sun's colour without the vertex colour comes from the
    // vertex shader (f_sun_colour; dividing f_sun by the vertex colour fails at 0). Known limit: in the tracer's own shade the tuft is
    // darker than the game's (which gives its shaded grass the untinted ambient). TESR_GrassRecord.z = [Main.Main.Quality]
    // CapturedGrassTint (0-1, how much of it; 0 = the second pass's colour), then .y CapturedGrassSaturation and .x CapturedGrassColour
    // for what is left to tune by eye. The branch is not taken while CapturedGrass is off (z 0).
    float3 grass_colour = LinColor(base_rgb.rgb) * (TESR_GrassBrightness.x * grass_shade);
    [branch] if (TESR_GrassRecord.z > 0.0f && nvr_vs) {
        const float tuft_scale = max(pa.f_amount.x, 0.25f);                    // the tuft's light scale (0.25-1): on every term of the game's light
        const float sun_n = pa.f_amount.y / tuft_scale;                        // N.L (with the translucency) x the grass dimmer
        // the three lights exactly as light_term takes them above (written out again: that line stays as it was for the builds without records)
        const float3 untinted = PBRAmbient(pa.f_ambient.xyz) + SkyAmbient(shadow_normal, nvr_vs ? sky_scale : 0.0f) / tuft_scale;   // ambient + sky, per unit of light scale (SkyLightScale off: the sky is not scaled)
        const float3 light_share = (PBRLight(pa.f_sun.xyz) * sun_n + untinted + 1.0e-4f) / (PBRLight(pa.f_sun_colour) * sun_n + untinted + 1.0e-4f);
        grass_colour *= lerp(1.0f, saturate(light_share), saturate(TESR_GrassRecord.z));
    }
    grass_colour = lerp(dot(grass_colour, float3(0.2126f, 0.7152f, 0.0722f)), grass_colour, saturate(TESR_GrassRecord.y)) * TESR_GrassRecord.x;
    pr.capturedPad1 = float4(saturate(grass_colour), grass_written);
    #if CAPTURED_NORMALS_ON >= 4
        // UNOFFICIAL 2026-11-14 GBufferV2 (Includes/CapturedNormals.hlsl): the grass mark is 4.5 there (g 4 and up: no shading normal; a stays
        // the written 1 / 0 the grass's blend needs)
        pr.capturedPad2 = float4(grass_class * grass_written, 4.5f, 0.9f, grass_written);
    #else
        pr.capturedPad2 = float4(grass_class * grass_written, 0.5f, 0.9f, grass_written);
    #endif
    pr.capturedNormal = CapturedFoliageNormal(grass_n, pa.f_shadow_world_pos.xyz, grass_written);
    #if defined(FOLIAGE_DEFERRED) && FOLIAGE_DEFERRED && CAPTURED_NORMALS_ON >= 4
    {
        // UNOFFICIAL 2026-11-14 full deferred renderer, phase 3: the grass's DEFERRED record (FOLIAGE_DEFERRED 1, made by core/DeferredFoliage.h
        // only with [Main.Main.Quality] DeferredFoliage and GBufferV2 at startup; the model: Includes/FoliageDeferred.hlsl). The same light as
        // above, the sun's part kept apart and left unshadowed (the shadow lookup above is then unused and compiled away): COLOR0 without the
        // sun, COLOR2.b the sun's light for DeferredSun (g its keep and highlight: Includes/DeferredPixel.hlsl FDFoliageGrassCode). The flashlight's beam (TESR_GrassSpot) is left to FlashlightDeferred, which lights these
        // records like any other (the forward grass lit itself by it because nothing else could). The grass's own lamps stay here, as drawn.
        float3 fdSpec = 0.0f;	// the sun's highlight as above, without the shadow
        [branch] if (GrassMaterial.z > 0.0f && nvr_vs) {
            const float3 fdL = normalize(pa.f_sun_dir);
            const float3 fdH = normalize(fdL - normalize(pa.f_shadow_world_pos.xyz));
            fdSpec = PBRLight(pa.f_sun.xyz) * (pa.f_amount.z * pow(saturate(dot(normal.xyz, fdH)), GrassMaterial.w) * saturate(dot(normal.xyz, fdL)) * GrassMaterial.z * normal.w);
        }
        float3 fdRest = (PBRAmbient(pa.f_ambient.xyz) * pa.f_amount.x + SkyAmbient(shadow_normal, nvr_vs ? sky_scale : 0.0f)) * TESR_GrassBrightness.y;
        [branch] if (TESR_GrassLightData.x > 0.0f && nvr_vs)
            fdRest += GrassLamps(pa.f_shadow_world_pos.xyz, normal.xyz) * pa.f_amount.x;
        const float3 fdSun = PBRLight(pa.f_sun.xyz) * (sun_base * TESR_GrassBrightness.y);
        const float3 fdBase = LinColor(base_rgb.rgb);
        const float fdKeep = FDFoliageGrassKeepQ(1.0f - pa.f_fog.w);	// (2026-11-14 fog: the keep the code holds, so DeferredSun's FDAddFogged is exact)
        const float3 fdWithout = lerp(LinEncode(fdRest * fdBase * TESR_GrassBrightness.x), pa.f_fog.rgb, 1.0f - fdKeep);
        const float3 fdWith = lerp(LinEncode(((fdRest + fdSun) * fdBase + fdSpec * TESR_GrassBrightness.y) * TESR_GrassBrightness.x), pa.f_fog.rgb, pa.f_fog.w);
        // the albedo: the texture x Brightness x the vertex colour's share of the sun (f_sun / f_sun_colour, decoded: exact, the decode is a
        // power) x the tuft's shade; the sun's hue is the vertex shader's sun colour
        const float3 fdHue = FoliageHue(PBRLight(pa.f_sun_colour));
        const float3 fdAlbedo = saturate(fdBase * TESR_GrassBrightness.x * saturate(PBRLight(pa.f_sun.xyz) / max(PBRLight(pa.f_sun_colour), 1.0e-6f)) * grass_shade);
        pr.f_color.rgb = grass_written > 0.5f ? fdWithout : fdWith;
        pr.capturedPad1.rgb = grass_written > 0.5f ? fdAlbedo : pr.capturedPad1.rgb;
        // the sun's light (unshadowed, unfogged) per unit of A x hue, and its highlight (the sun's hue) per unit of hue: least-squares numbers
        pr.capturedPad2.b = grass_written > 0.5f ? FoliageSunCode(FoliageFit(fdSun * fdBase * TESR_GrassBrightness.x, fdAlbedo * fdHue)) : pr.capturedPad2.b;
        pr.capturedPad2.g = grass_written > 0.5f ? FDFoliageGrassCode(fdKeep, FoliageHighlightEnc(FoliageFit(fdSpec * (TESR_GrassBrightness.y * TESR_GrassBrightness.x), fdHue))) : pr.capturedPad2.g;
    }
    #endif
#endif

    return pr;
};
