// UNOFFICIAL 2026-11-15 DeferredTransparents' maths check (tests/fd_glass_math.cpp; docs/next/FD-GLASS-RESULTS.md): pseudo-random surface
// points in the view of a synthetic lamp table (built on the CPU by the game's own code: LampTableCore FillRow / BinFast and
// ForwardPlusHeader::Put), lit by Includes/ForwardPlus.hlsl FPLampSum (the pixel's froxel list) and, for reference, by EVERY lamp of the
// table in index order (no froxel lists). FG_Mode: x 0 = FPLampSum, 1 = every lamp, 2 = the point itself (camera-relative position, for the
// CPU reference); y 1 = each lamp's term is colour x falloff only (the CPU can sum that exactly), 0 = the draw's PBR term (getPointLightLightingAtt).
#define SPECULAR
#ifndef LIGHT_COMMON_LIST_TEXELS
    #define LIGHT_COMMON_LIST_TEXELS 16.0f
#endif
#define LAMPS_DEFERRED 5	// (the unshadowed form: the test table has no atlas tiles)
#include "includes/Helpers.hlsl"
#include "includes/Object.hlsl"

float4 FG_Mode : register(c0);		// x mode, y simple term
float4 FG_View : register(c1);		// x viewport width, y height, z lamps in the table

float3 FGTerm(float3 toLamp, float att, float3 color, float3 v, float3 n, float3 albedo, float rough, float metal) {
    return FG_Mode.y > 0.5f ? color * att : getPointLightLightingAtt(toLamp, att, color, v, n, albedo, rough, metal);
}
#ifndef FG_REAL
#define getPointLightLightingAtt FGTerm	// (FG_REAL: FPLampSum with the real term, as the templates compile it)
#endif
#include "includes/ForwardPlus.hlsl"
#ifndef FG_REAL
#undef getPointLightLightingAtt
#endif

float Hash(float2 p, float k) { return frac(sin(dot(p, float2(12.9898f, 78.233f)) + k * 37.719f) * 43758.5453f); }
float3 Dir(float2 p, float k) {
    const float z = Hash(p, k) * 2.0f - 1.0f, a = Hash(p, k + 0.5f) * 6.2831853f, s = sqrt(saturate(1.0f - z * z));
    return float3(s * cos(a), s * sin(a), z);
}

float4 main(float2 vpos : VPOS) : COLOR0 {
    const float4 fwd = FPHeader(1.0f), right = FPHeader(2.0f), up = FPHeader(3.0f);
    const float2 local = (vpos + 0.5f) / FG_View.xy;
    const float3 ray = fwd.xyz + right.xyz * ((local.x * 2.0f - 1.0f) * fwd.w) + up.xyz * ((1.0f - local.y * 2.0f) * right.w);	// forward share 1
    const float viewZ = exp2(lerp(3.0f, 12.0f, Hash(vpos, 1.0f)));	// 8 .. 4096 units: every slice
    const float3 p = ray * viewZ;
    if (FG_Mode.x > 1.5f) return float4(p, 1.0f);
    float3 n = Dir(vpos, 2.0f);
    n = dot(n, p) > 0.0f ? -n : n;	// facing the camera
    const float3 albedo = float3(Hash(vpos, 4.0f), Hash(vpos, 5.0f), Hash(vpos, 6.0f));
    const float rough = lerp(0.05f, 1.0f, Hash(vpos, 7.0f));
    SetDiffuseEnergyKeep(n, -normalize(p), rough);
    if (FG_Mode.x < 0.5f) return float4(FPLampSum(p, n, 1.0f, albedo, rough, 0.0f), 1.0f);
    if (FG_Mode.z > 0.5f) {	// every lamp, calling the lamp term directly (with FG_REAL: the term FPLampSum calls too)
        const float3 q2 = FPHeader(0.0f).xyz + p;
        float3 s2 = 0.0f;
        [loop] for (int j = 0; j < 1024; j++) {
            [branch] if ((float)j >= FG_View.z) break;
            const float x2 = ((float)j * 2.0f + 0.5f) / LIGHT_COMMON_TEXELS;
            const float4 l2 = tex2Dlod(NVR_LampTable, float4(x2, 0.5f / LIGHT_COMMON_ROWS, 0.0f, 0.0f));
            const float3 t2 = l2.xyz - q2;
            const float a2 = l2.w > 0.0f ? vanillaAtt(t2, max(l2.w, 1.0e-3f)) : 0.0f;
            [branch] if (a2 > 0.0f) {
                const float3 c2 = tex2Dlod(NVR_LampTable, float4(x2 + 1.0f / LIGHT_COMMON_TEXELS, 0.5f / LIGHT_COMMON_ROWS, 0.0f, 0.0f)).rgb;
                s2 += getPointLightLightingAtt(t2, a2, c2, -p * rsqrt(max(dot(p, p), 1.0e-6f)), n, albedo, rough, 0.0f);
            }
        }
        return float4(s2, 1.0f);
    }
    // every lamp of the table, in index order
    const float3 q = FPHeader(0.0f).xyz + p;
    const float3 v = -p * rsqrt(max(dot(p, p), 1.0e-6f));
    float3 sum = 0.0f;
    [loop] for (int i = 0; i < 1024; i++) {
        [branch] if ((float)i >= FG_View.z) break;
        const float x = ((float)i * 2.0f + 0.5f) / LIGHT_COMMON_TEXELS;
        const float4 lamp = tex2Dlod(NVR_LampTable, float4(x, 0.5f / LIGHT_COMMON_ROWS, 0.0f, 0.0f));
        const float3 toLamp = lamp.xyz - q;
        const float att = lamp.w > 0.0f ? vanillaAtt(toLamp, max(lamp.w, 1.0e-3f)) : 0.0f;
        [branch] if (att > 0.0f) {
            const float3 color = tex2Dlod(NVR_LampTable, float4(x + 1.0f / LIGHT_COMMON_TEXELS, 0.5f / LIGHT_COMMON_ROWS, 0.0f, 0.0f)).rgb;
            sum += FGTerm(toLamp, att, color, v, n, albedo, rough, 0.0f);
        }
    }
    return float4(sum, 1.0f);
}
