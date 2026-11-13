// Grass PS -- vanilla GRASS23x000TMS.pso plus forward sun shadow.
// Shared by GRASS23x002.vso (LFS) and GRASS23x003.vso (LVS).
// The VS hands the lighting over already split, so only the sun is shadowed:
//   TEXCOORD4.xyz ambient    TEXCOORD5.xyz sun    TEXCOORD5.w fade    COLOR0 fog (.w amount)
// UNOFFICIAL 2026-11-04 (from Physically-Based-Reloaded; in place of PointLights, the game's 1POINT grass shaders, whose version is
// in VegasShaders-backups/before_grass_port_20261104): the grass's brightness and night brightness, the placed lamps and the
// Pip-Boy light on every tuft, and normal maps with the sun's highlight. PBR NVR's c146/c147/c160-c176 are the sun cross-fade's
// here: c80-c98 instead (nothing else in the grass PS is below c100).

// UNOFFICIAL 2026-11-14 (rt-shadows F1) the forward RT sun (Includes/Shadow.hlsl SunForward): last frame's traced visibility instead of
// the cascades where it is there; the default registers (s10, c160-c164: free here; not with the sun cross-fade). Not Bend's contact.
#define SHADOW_SUN_CONTACT_IN 1
#define SHADOW_SUN_CONTACT_RT_ONLY 1
#include "includes/Shadow.hlsl"
#include "includes/PBRScale.hlsl"

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
float4 TESR_GrassBrightness : register(c80);
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

// The lamps' light on a blade: vanilla's falloff (1 - d^2 / r^2, as the object shaders' vanillaAtt), and a wrapped diffuse, as a
// thin blade lets light through to its back face.
float3 GrassLamps(float3 position, float3 normal) {
    float3 lamps = 0.0f;
    [loop] for (int i = 0; i < GRASS_LIGHTS; i++) {
        [branch] if (i >= TESR_GrassLightData.x) break;
        float3 toLight = TESR_GrassLightPosition[i].xyz - position;
        float radius = TESR_GrassLightPosition[i].w;
        float att = saturate(1.0f - dot(toLight, toLight) / (radius * radius));
        float wrap = 0.5f + 0.5f * abs(dot(normal, toLight * rsqrt(max(dot(toLight, toLight), 1.0f))));
        lamps += TESR_GrassLightColor[i].rgb * (att * wrap);
    }
    return lamps * TESR_GrassLightData.y;
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
};

struct PixResult {
    float4 f_color : COLOR0;
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
        light_term += PBRLight(GrassLamps(pa.f_shadow_world_pos.xyz, normal.xyz));
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
    // UNOFFICIAL 2026-11-03 SmoothCutouts (Includes/AlphaHash.hlsl): on a hashed draw the cut takes noise; mode 2 the coverage the
    // game's transparency multisampling gives grass (its TMS build: alpha x 1.75). The pattern is shifted off the fade dither's.
    const float grass_width = max(fwidth(base_rgb.a) * TESR_AlphaHash.z, 1.0f / 255.0f);
    if (NVR_AlphaHashDraw) {
        if (TESR_AlphaHash.x > 0.5f) {
            const float coverage = TESR_AlphaHash.x > 1.5f ? saturate(base_rgb.a * 1.75f) : saturate((base_rgb.a - AlphaTestRef.x) / grass_width + 0.5f);
            pr.f_color.a = AlphaHashKeep(coverage, vpos_s, 37.0f) > 0.0f ? 1.0f : 0.0f;
        }
    }
#endif
#else
    pr.f_color.a = saturate(base_rgb.a * 1.75f);
#endif

    return pr;
};
