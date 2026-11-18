// UNOFFICIAL 2026-11-15 DeferredSkin (core/DeferredSkin.h; docs/next/FULL-DEFERRED-RENDERER-PLAN.md phase 4, docs/next/FD-SKIN-RESULTS.md): the
// skin's deferred record (SkinTemplate.hlsl, LAMPS_DEFERRED 2 / 3) and the screen passes that light it (DeferredSun.hlsl, DeferredLamps.hlsl,
// FD_SKIN) share this file: the record writes what forward skin lighting (Includes/Skin.hlsl SkinLight) needs beyond the G-buffer v2 layout,
// and the passes read it back and call the same SkinLight, so the deferred skin is the forward skin by construction.
//
// What a deferred skin pixel holds (GBufferV2: CAPTURED_NORMALS_ON 4):
//   COLOR0       ambient + sky light + the sky reflection (+ the sun in LAMPS_DEFERRED 3), fogged, as forward with those lights only
//   COLOR1 rgb   the skin's albedo as SkinLight takes it (LinColor of the FaceGen-blended colour; NOT clamped to 1: the passes need it exact)
//          a     the lighting-free code 1 + keep (LAMPS_DEFERRED 2: DeferredSun and DeferredLamps light it) or the lamps code 5 + keep (3)
//   COLOR2 r     6 (skin; no specular bit, metal 0)   g / a  the bumped normal N, world, octahedral   b  skinRoughness (after SpecularAA)
//   COLOR3 rg    the smooth vertex normal (the geometric normal SkinLight's self-shadow and transmission take)   b  the camera distance
//          a     FD_SKIN_CODE: 4 and up (an actor's, a > 1.5, for every reader that tests that), holding three numbers on fp16's own grid:
//                  mask   the normal map's alpha (the highlights' mask; skinSpecScale = mask x 2 x SpecularStrength x (1 + wetness)), 32 levels
//                  thin   skinThinness (the curvature x 2, saturated: the transmission; skinCurvature = saturate(thin x Scattering), exact for
//                         Scattering >= 1, the default), 24 levels on a square-root scale (finer where the diffusion's width moves fastest)
//                  soft   how far the blurred normal (Nsoft: the normal map read 2 mips down) leans from the vertex normal toward N:
//                         Nsoft ~ normalize(lerp(Ngeo, N, soft)) (exact along N's tilt; its sideways part is lost), 16 levels
//                index i = (32-level mask x 24 + thin) x 16 + soft (0 .. 12287) written as the i-th fp16 value from 4: 4 x 2^o x (1 + m / 1024),
//                o = i / 1024 (0 .. 11), m = i mod 1024: [4, 16384), every value exact in the fp16 target.
// Readers of COLOR3.a that must know: SkinFill / SkinAlbedoFill (core/CapturedNormals.h, which take 2.02 .. 3.5 only with DeferredSkin),
// DeferredSun (its "skin drawn over this pixel forward" clip, 2.02 .. 3.5), DeferredLamps (its actor clip lets >= 3.5 through), the path
// tracer (nv.a > 3.5 = the view model: DeferredSkin is not live while the path tracer replaces the picture, core/DeferredSkin.h).
#ifndef NVR_SKIN_DEFERRED_HLSL
#define NVR_SKIN_DEFERRED_HLSL

#define FD_SKIN_CODE_BASE 4.0f
#define FD_SKIN_CODE_MIN 3.5f	// a deferred skin pixel: COLOR3.a above this (forward skin 2.05 .. 2.45, gear 2, objects 1)

// Writer: the code of (mask, thin, soft), each 0..1.
float FDSkinCode(float mask, float thin, float soft) {
    const float mq = floor(saturate(mask) * 31.0f + 0.5f);
    const float cq = floor(sqrt(saturate(thin)) * 23.0f + 0.5f);
    const float bq = floor(saturate(soft) * 15.0f + 0.5f);
    const float i = (mq * 24.0f + cq) * 16.0f + bq;
    const float o = floor(i / 1024.0f + 1.0e-4f);
    return FD_SKIN_CODE_BASE * exp2(o) * (1.0f + (i - 1024.0f * o) / 1024.0f);
}

// Reader: the three numbers back (exact for any value the writer made: the octave from log2 with a margin far above its error and below the 7.0e-4 an octave's last index sits under the next, the step by
// rounding).
void FDSkinDecode(float a, out float mask, out float thin, out float soft) {
    const float o = floor(log2(max(a, FD_SKIN_CODE_BASE) / FD_SKIN_CODE_BASE) + 1.0e-4f);	// (index 1023 of an octave sits 7.0e-4 below the next)
    const float i = 1024.0f * o + floor((a / (FD_SKIN_CODE_BASE * exp2(o)) - 1.0f) * 1024.0f + 0.5f);
    const float mq = floor((i + 0.5f) / 384.0f);
    const float rest = i - mq * 384.0f;
    const float cq = floor((rest + 0.5f) / 16.0f);
    const float bq = rest - cq * 16.0f;
    mask = mq / 31.0f;
    thin = (cq / 23.0f) * (cq / 23.0f);
    soft = bq / 15.0f;
}

// The record side: how far Nsoft leans from the vertex normal toward N, in the TANGENT space the forward shader lights in (z = the vertex
// normal). Fitted so normalize(lerp(z, N, soft)) tilts as far as Nsoft does along N's tilt direction: soft = p / (r q + p (1 - N.z)),
// r = |N.xy|, p = Nsoft's part along N.xy's direction, q = Nsoft.z.
float FDSkinSoftness(float3 N, float3 Nsoft) {
    const float r = length(N.xy);
    [flatten] if (r < 1.0e-3f) return 1.0f;	// N is the vertex normal: nothing to lean along
    const float p = dot(Nsoft.xy, N.xy) / r;
    const float denominator = r * Nsoft.z + p * (1.0f - N.z);
    return denominator > 1.0e-5f ? saturate(p / denominator) : 1.0f;
}

#ifdef FD_SKIN_PASS
// The screen passes' side. Includes/Skin.hlsl with SKIN_IN_SCREEN_PASS (its PBR.hlsl and c194 are the pass's own): TESR_SkinMaterial (c192),
// TESR_SkinLook (c193) and TESR_SkinDebug (c195) bound by name by the pass's SetCT, as for the skin shaders. TESR_SkinDebug.w is 1 while
// the skin blur takes the diffusion (DeferredSkinBlur: the curvature diffusion then steps aside, as forward's NVR_SkinDraw.x).
#define SKIN_IN_SCREEN_PASS
// PBRScale.hlsl's two helpers Skin.hlsl names (SkinAmbient: never called by the passes, the ambient is in COLOR0), on Object.hlsl's constants
// (PBRScale.hlsl itself declares TESR_PBRData again at another register)
float3 PBRAmbient(float3 ambient) { return LinColor(ambient * TESR_PBRData.w); }
float3 SkyAmbient(float3 worldNormal, float valid) { return LinColor(SkyAmbientRadiance(worldNormal, TESR_PBRExtraData.z)) * LinScale(TESR_PBRExtraData.y) * valid; }
#include "includes/Skin.hlsl"

static float fdSkinSoft = 1.0f;
// UNOFFICIAL 2026-11-15 SSS v2 (core/DeferredSkin.h, [Shaders.Skin.SSS]): xyz the Burley profile's scatter distance d per colour (world units:
// ScatterRed / Green / Blue x Radius), w 1 = Transmission (the thickness from the sun's shadow map; the curvature guess off). Set by name.
float4 TESR_SkinSSS : register(c196);

// One pixel: the forward SkinSetup's numbers from the G-buffer. a = COLOR3.a, roughness = COLOR2.b.
void FDSkinSetup(float a, float roughness) {
    float mask, thin, soft;
    FDSkinDecode(a, mask, thin, soft);
    const float wet = saturate(TESR_SkinLook.y);
    skinRoughness = roughness;
    skinSpecScale = mask * 2.0f * TESR_SkinMaterial.x * (1.0f + wet);
    skinThinness = thin;
    skinCurvature = TESR_SkinDebug.w > 0.5f ? 0.0f : saturate(thin * TESR_SkinMaterial.z);
    fdSkinSoft = soft;
    // (SSS v2 Transmission: the transmission from the thickness the sun's shadow map measures, FDSkinSunTransmission, in place of the guess
    // from the curvature; skinThinness feeds nothing else)
    [flatten] if (TESR_SkinSSS.w > 0.5f) skinThinness = 0.0f;
}

// UNOFFICIAL 2026-11-15 SSS v2 Transmission (DeferredSun.hlsl, FD_SKIN; needs Includes/Shadow.hlsl): the sun through thin skin from behind. The
// thickness t is the distance from the point back toward the sun to the nearest surface the sun's Near cascade holds (its PCF atlas stores
// the light-space depth: SHADOW_FIXED_MODE 3), in world units; the light that comes through falls off with the profile's own share past t,
// per colour (Burley: 1 - CDF(t) = 1/4 e^(-t/d) + 3/4 e^(-t/3d), red reaching furthest), on the side turned away from the sun (Jimenez's
// saturate(0.3 - N.L)), times Translucency. Outside the Near cascade, indoors or with the sun's maps off: none.
#ifdef FD_SKIN_SUN
float3 FDSkinSunTransmission(float3 P, float3 Ngeo, float3 L, float3 albedo, float3 light) {
    [branch] if (TESR_SkinSSS.w < 0.5f || !TESR_ShadowFormatData.z || !TESR_ShadowFade.y || length(P - TESR_ShadowNearCenter.xyz) >= TESR_ShadowNearCenter.w)
        return 0.0f;
    const float farSide = saturate(0.3f - dot(Ngeo, L));
    [branch] if (farSide <= 0.0f) return 0.0f;
    float4 ls = mul(float4(P, 1.0f), TESR_ShadowCameraToLightTransformNear);
    ls.xyz /= ls.w;
    const float2 uv = float2(ls.x * 0.5f + 0.5f, ls.y * -0.5f + 0.5f) * 0.5f;	// the Near quadrant (0, 0)
    const float occluder = tex2Dlod(TESR_ShadowAtlas, float4(uv, 0.0f, 0.0f)).x;
    const float worldPerDepth = 1.0f / length(float3(TESR_ShadowCameraToLightTransformNear[0][2], TESR_ShadowCameraToLightTransformNear[1][2], TESR_ShadowCameraToLightTransformNear[2][2]));
    const float t = max(ls.z - occluder, 0.0f) * worldPerDepth;
    const float3 d = max(TESR_SkinSSS.xyz, 1.0e-3f);
    const float3 through = 0.25f * exp(-t / d) + 0.75f * exp(-t / (3.0f * d));
    // (2026-11-16 fd-fwdgroups, batch 8's hard orange edge across the backlit scalp) Where the point is itself the nearest surface toward the
    // sun (t ~ 0: it is lit directly, its light is the diffuse term's), "through" was 1; one texel further round the head the map's nearest
    // surface jumps to the far side and t to the head's width: full transmission next to none, a hard line. Light comes through only past a
    // thickness: a ramp over the red scatter distance from t = 0, so both sides of that line meet at 0 (the ear, ~0.5-1 unit thin, keeps it).
    const float entered = saturate(t / max(d.r, 1.0e-3f));
    return albedo * through * (entered * farSide * TESR_SkinMaterial.w) * light;
}
#endif

// One light on a skin pixel (world space): SkinLight as forward calls it. N the bumped normal, Ngeo the vertex normal, L toward the light
// (any length), light decoded (with its falloff), shadow 0..1 (the sun's, inside the skin's model; 1 for a lamp, whose shadow multiplies after).
float3 FDSkinLight(float3 albedo, float3 N, float3 Ngeo, float3 V, float3 L, float3 light, float shadow, bool sun, out float3 specular) {
    const float3 Nsoft = normalize(lerp(Ngeo, N, fdSkinSoft));
    return SkinLight(albedo, N, Nsoft, dot(Ngeo, normalize(L)), V, L, light, shadow, sun, specular);
}

// The skin lamp pass's two halves (DeferredLamps.hlsl DL_SKIN_ONLY: SkinLight whole inside the lamp loop runs out of temp registers, X4505,
// so the loop runs twice, the diffuse half then the highlight half; their sum is FDSkinLight's). FDSkinLampDiffuse is SkinLight's diffusion,
// self-shadow escape and transmission line for line (Includes/Skin.hlsl: keep in step), FDSkinLampSpecular its SkinSpecular; a lamp's shadow
// multiplies after (shadow 1 here, as SkinLamp).
float3 FDSkinLampDiffuse(float3 albedo, float3 N, float3 Ngeo, float3 V, float3 L, float3 light) {
    L = normalize(L);
    const float3 Nsoft = normalize(lerp(Ngeo, N, fdSkinSoft));
    const float geometricNdotL = dot(Ngeo, L);
    const float3 scattered = SkinDiffusion(N, Nsoft, L);
    const float3 direct = min(scattered, saturate(dot(N, L)));
    const float3 wrapped = scattered - direct;
    float selfShadowed = saturate((0.05f - geometricNdotL) / 0.15f);
    selfShadowed = selfShadowed * selfShadowed * (3.0f - 2.0f * selfShadowed) * saturate(TESR_SkinLook.z);
    const float3 shadowColor = SkinShadow(1.0f);
    const float3 diffuse = albedo * (direct * shadowColor + wrapped * lerp(shadowColor, 1.0f, selfShadowed));
    const float thin = smoothstep(0.3f, 0.9f, skinThinness);
    const float farSide = saturate(0.3f - geometricNdotL);
    const float3 transmitDir = normalize(L + Nsoft * SKIN_TRANSMIT_DISTORTION);
    const float back = pow(saturate(dot(V, -transmitDir)), SKIN_TRANSMIT_POWER);
    const float3 transmitted = albedo * SkinLinearColor(SKIN_TRANSMIT_TINT) * (back * thin * farSide * TESR_SkinMaterial.w);
    return (diffuse + transmitted) * light;
}
float3 FDSkinLampSpecular(float3 N, float3 V, float3 L, float3 light) {
    return SkinSpecular(N, V, normalize(L), light, false);
}
#endif

#endif
