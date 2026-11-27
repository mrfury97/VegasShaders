// UNOFFICIAL 2026-11-25 fd-complete batch 6 (docs/next/FD-COMPLETE.md; core/DeferredTransparents.h namespace Decals; [Main.Main.Quality]
// DeferredDecals): forward+ for the game's lit decals, SM3005 / SM3007.pso (bullet holes, blood and impact marks: BSSM_3XLIGHTING_VcPxo(Spc)).
// The game draws them in BSShaderAccumulator::RenderDecals, after the deferred light passes, with its own four lights (the sun + up to 3
// lamps of the draw's LightData), so a hole in a wall lit by every lamp of the room took three of them, unshadowed by the atlas, and the
// flashlight only as the game's lamp. This record (DECAL_FORWARD_PLUS 1, made only with DeferredDecals on at startup and chosen only inside
// the decal groups' forward+ scope with this frame's lamp table) keeps the game's sun pair and replaces the game's lamps by EVERY lamp of the
// pixel's froxel (Includes/ForwardPlus.hlsl: the table and lists the deferred lamp pass reads, each shadowed from its atlas tile) and the
// deferred flashlight's beam, lit as the shader's loop lights a lamp (Lambert, the loop's falloff through LampAttOther, PBRLight's strength).
// The decal's lighting is in its tangent space (object space from SM3004.vso); the table is camera-relative world, so the normal-mapped
// normal is rebuilt in world axes from the screen derivatives (the cotangent frame CapturedNormals.hlsl CapturedDerivFrame uses, same signs).
// SM3007's highlight stays the sun's alone. Included only by the DECAL_FORWARD_PLUS records.
#ifndef NVR_DECAL_FORWARD_PLUS_HLSL
#define NVR_DECAL_FORWARD_PLUS_HLSL

#define FP_OWN_SHADOW 1
#define FP_ATLAS_REG s12	// (s8 is the clouds' shadow, s9 / s10 the sun's, s11 the character map; s3 the table; s12 free)
#define FP_ATT(toLamp, reach) (1.0f - saturate(dot(toLamp, toLamp) / ((reach) * (reach))))	// 1 - (d / r)^2, the loop's falloff before LampAttOther
// The flashlight: the light pass's own beam (Includes/FlashBeam.hlsl: its reach, Falloff, cone, NearFade, HotspotLimit and shadow map, as the
// hair and the eyes take it), not ForwardPlus.hlsl's plain cone term, which lit a bullet hole at about a third of the ground round it (bench
// 2026-11-25, Goodsprings 22:00). Registers free in SM3005 / SM3007: c9-c13 and c14-c17 (they use c0, c1, c3, c7, c20-c28), s6.
#define FP_NO_BEAM
#define FLASH_BEAM_REG c9
#define FLASH_BEAM_MATRIX_REG c14
#define FLASH_BEAM_SAMPLER s6
#include "includes/FlashBeam.hlsl"

static float3 decalFpN;	// the normal-mapped normal, world axes

// One table lamp, as the shader's loop lights a point lamp. color: the table's (linear, the lamp's dimmers in); the loop's PBRLight takes the
// game's colour x the PBR light strength decoded, i.e. the decoded colour x LinScale(strength).
float3 DecalFPLamp(float3 toLamp, float att, float3 color) {
    return color * LinScale(TESR_PBRData.z) * (saturate(dot(decalFpN, normalize(toLamp))) * LampAttOther(att));
}
#define FP_LAMP_TERM(toLamp, att, color, V, n, albedo, rough, metal) DecalFPLamp(toLamp, att, color)
#include "includes/ForwardPlus.hlsl"

// The beam on the decal: one more lamp's Lambert, with the beam's strength in place of a lamp's falloff ([2].rgb has the lamps' light scale in).
float3 DecalFPBeam(float3 p, float valid) {
    float3 light = 0.0f;
    [branch] if (FLASH_BEAM_ON && valid > 0.0f) {
        float3 toLight;
        const float beam = FlashBeamStrength(p, toLight);
        light = TESR_FlashBeam[2].rgb * (beam * saturate(dot(decalFpN, toLight * rsqrt(max(dot(toLight, toLight), 1.0e-6f)))));
    }
    return light;
}

// The tangent-space normal tn in world axes: the cotangent frame of p (camera-relative world) and uv about nFace (the facing geometric
// normal, unit). Top level only (ddx / ddy).
float3 DecalFPWorldNormal(float3 p, float2 uv, float3 nFace, float3 tn) {
    const float3 dp1 = ddx(p), dp2 = ddy(p);
    const float2 duv1 = ddx(uv), duv2 = ddy(uv);
    const float3 dp2perp = cross(dp2, nFace), dp1perp = cross(nFace, dp1);
    const float3 t = dp2perp * duv1.x + dp1perp * duv2.x;
    const float3 b = dp2perp * duv1.y + dp1perp * duv2.y;
    const float s = -rsqrt(max(max(dot(t, t), dot(b, b)), 1.0e-30f));
    const float3 n = (t * tn.x + b * tn.y) * s + nFace * tn.z;
    return n * rsqrt(max(dot(n, n), 1.0e-20f));
}

#endif
