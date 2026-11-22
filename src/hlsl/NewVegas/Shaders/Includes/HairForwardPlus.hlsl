// UNOFFICIAL 2026-11-16 fd-fwdgroups: forward+ for the game's own blended hair, brows and beards (SM3002 / SM3003.pso; docs/next/FD-FORWARD-GROUPS.md;
// core/DeferredTransparents.h namespace Hair; [Main.Main.Quality] DeferredHair).
// The game draws its hair in the sorted blended list, after the deferred light passes, with its own one-pass lamps (the sun + up to 3, the
// draw's LightData), so under the deferred renderer a face lit by every lamp of the room sat under hair lit by three of them, with no atlas
// shadows. This record (HAIR_FORWARD_PLUS 1, made only with DeferredHair on at startup and chosen only in the blended list with this frame's
// lamp table) keeps the game's sun pair and replaces the game's point lamps by EVERY lamp of the pixel's froxel (Includes/ForwardPlus.hlsl:
// the table and lists the deferred lamp pass reads, each shadowed from its atlas tile), lit with the hair's own model, lamp for lamp as the
// shader's loop lights a LightData lamp: PBR diffuse, and (SM3003) the Marschner R + TRT lobes from AnisoMap with the point lamps' tint, the
// game's look's highlight from the light's stored-curve colour.
// The table's lamps are camera-relative world; the hair lights in its tangent space, built from object space. The vertex shader (SM3003.vso,
// with G-buffer v2 compiled in) hands over the world-space tangent and normal beside the object-space ones; their two frames give the
// world -> object rotation (the hair is rigid: one ModelViewProj, no bones). Included only by the HAIR_FORWARD_PLUS records.
#ifndef NVR_HAIR_FORWARD_PLUS_HLSL
#define NVR_HAIR_FORWARD_PLUS_HLSL

#define FP_OWN_SHADOW 1
#define FP_ATLAS_REG s12	// (s8 is the clouds' shadow here, s9 / s10 the sun's, s11 the character map; s3 the table; s12 free)
#define FP_NO_BEAM			// (the hair takes the flashlight's beam itself: Includes/FlashBeam.hlsl)
#define FP_ATT(toLamp, reach) (1.0f - saturate(dot(toLamp, toLamp) / ((reach) * (reach))))	// 1 - (d / r)^2, the loop's falloff before LampAttOther

static float3x3 hairFpM;	// camera-relative world -> the hair's tangent space
static float3 hairFpN, hairFpEye, hairFpTint;
static float3 hairFpSpec = 0.0f;	// the highlight, in the look TESR_HairData.y picks (one sum: a temp register less, SM3003 ran out: X4505)

// One table lamp, as the shader's loop lights a point lamp. color: the table's (linear, the lamp's dimmers in); the loop's PBRLight takes the
// game's colour x the PBR light strength decoded, i.e. the decoded colour x LinScale(strength).
float3 HairFPLamp(float3 toLamp, float att, float3 color) {
    const float3 lv = normalize(mul(hairFpM, toLamp));
    const float a = LampAttOther(att);
    const float3 lc = color * LinScale(TESR_PBRData.z);
#ifdef HAIR_FP_SPEC
    const float3 hs = hairFpEye + lv;
    const float h2 = dot(hs, hs);
    const float3 hv = h2 > 1.0e-6f ? hs * rsqrt(h2) : hairFpN;	// (a lamp straight behind the head: as the loop)
    // the two shifts as the shader makes them, from the normal (made here, not held across the loop: temp registers)
    const float3 trt = normalize(hairFpN * 0.5f + float3(0.0f, 0.0f, 1.0f));
    const float3 r = normalize(trt + float3(-0.05f, 0.0f, 0.0f));
    const float la = tex2Dlod(AnisoMap, float4(dot(r, lv), dot(r, hv), 0.0f, 0.0f)).a;
    const float lb = tex2Dlod(AnisoMap, float4(dot(trt, lv), dot(trt, hv), 0.0f, 0.0f)).a;
    const float3 lobes = (hairFpTint * la + lb * 0.7f) * (saturate(dot(hairFpN, lv)) * a);
    hairFpSpec += lobes * (TESR_HairData.y > 0.5f ? color : lc);
    return PBRDiffuse(0.0f, HAIR_ROUGHNESS, 1.0f, hairFpN, hairFpEye, lv, lc) * a;
#else
    return PBRDiffuse(0.0f, HAIR_ROUGHNESS, 1.0f, hairFpN, hairFpN, lv, lc) * a;
#endif
}
#define FP_LAMP_TERM(toLamp, att, color, V, n, albedo, rough, metal) HairFPLamp(toLamp, att, color)
#include "includes/ForwardPlus.hlsl"

// World -> tangent: tbn (the shader's rows, object space) after world -> object, from the tangent and normal in both spaces (Gram-Schmidt).
float3x3 HairFPFrame(float3x3 tbn, float3 tObj, float3 nObj, float3 tWorld, float3 nWorld) {
    const float3 No = normalize(nObj);
    const float3 To = normalize(tObj - No * dot(tObj, No));
    const float3 Nw = normalize(nWorld);
    const float3 Tw = normalize(tWorld - Nw * dot(tWorld, Nw));
    return mul(tbn, mul(transpose(float3x3(To, cross(No, To), No)), float3x3(Tw, cross(Nw, Tw), Nw)));
}

#endif
