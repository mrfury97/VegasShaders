// UNOFFICIAL 2026-11-14 DeferredSun's maths check (tests/fd_sun_math.cpp; docs/next/FD-SUN-RESULTS.md): the same pseudo-random inputs per
// pixel through the object templates' sun term (Includes/Object.hlsl getSunLighting, its variant fixed at compile time by SPECULAR, after
// SetDiffuseEnergyKeep as the templates call it) and through the pass's (getSunLightingModel, the variant chosen per pixel), compiled
// from the real include files. FD_PASS 1 = the pass's form (FD_SPEC its per-pixel choice), else the template's.
// (2026-11-16 fd-sunvis) FD_FADE 1: SpecularFade's highlight fade, the template's PBR_SPEC_FADE (NVR_SpecFade.x, c211) against the pass's
// FD_SUN_SPEC_FADE (Includes/Object.hlsl getSunLightingModel), both at FD_Spec.y.
#define LINEAR_LIGHTING
float4 FD_Spec : register(c0);	// x the pass's specular choice (1 PBRSun, 0 PBRDiffuse), y the fade (FD_FADE)
#if defined(FD_FADE) && FD_FADE
    #if FD_PASS
        #define FD_SUN_SPEC_FADE FD_Spec.y
    #else
        #define PBR_SPEC_FADE
    #endif
#endif
#include "includes/Helpers.hlsl"
#include "includes/Object.hlsl"

float Hash(float2 p, float k) { return frac(sin(dot(p, float2(12.9898f, 78.233f)) + k * 37.719f) * 43758.5453f); }
float3 Dir(float2 p, float k) {
    const float z = Hash(p, k) * 2.0f - 1.0f, a = Hash(p, k + 0.5f) * 6.2831853f, s = sqrt(saturate(1.0f - z * z));
    return float3(s * cos(a), s * sin(a), z);
}

float4 main(float2 vpos : VPOS) : COLOR0 {
    const float3 N = Dir(vpos, 1.0f);
    float3 L = Dir(vpos, 2.0f);
    float3 V = Dir(vpos, 3.0f);
    L = dot(L, N) < 0.0f && Hash(vpos, 9.0f) > 0.2f ? -L : L;	// mostly lit, some back-facing
    V = dot(V, N) < 0.0f ? -V : V;
    const float3 albedo = float3(Hash(vpos, 4.0f), Hash(vpos, 5.0f), Hash(vpos, 6.0f));
    const float r = lerp(0.02f, 1.0f, Hash(vpos, 7.0f));
    const float metal = Hash(vpos, 8.0f) > 0.7f ? Hash(vpos, 10.0f) : 0.0f;
    const float3 sun = float3(0.9f, 0.8f, 0.6f) * (0.5f + 2.0f * Hash(vpos, 11.0f));
    SetDiffuseEnergyKeep(N, V, r);
#if FD_PASS
    return float4(getSunLightingModel(FD_Spec.x, L, sun, V, N, albedo, r, metal), 1.0f);
#else
    return float4(getSunLighting(L, sun, V, N, albedo, r, metal), 1.0f);
#endif
}
