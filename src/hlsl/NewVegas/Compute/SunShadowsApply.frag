// SunShadows' last pass of techniques 0 and ContactFast (the intensity curve and the deferred cascade lookup) as a Vulkan pass:
// Effects/SunShadows.fx.hlsl Shadow with GetLightAmount / GetLightAmountValue and Includes/Shadows.hlsl, line for line
// (SHADOW_FILTER_TAPS 1). The atlas is read with textureLod 0 everywhere, as the .fx does.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "SunShadows.glsl"

// --- Includes/Shadows.hlsl ---
float Linstep(float a, float b, float v) {
    return saturate((v - a) / (b - a));
}

float ReduceLightBleeding(float pMax, float amount) {
    return Linstep(amount, 1.0, pMax);
}

float ChebyshevUpperBound(vec2 moments, float mean, float minVariance, float lightBleedingReduction) {
    float variance = moments.y - (moments.x * moments.x);
    variance = max(variance, minVariance);

    float d = mean - moments.x;
    float pMax = variance / (variance + (d * d));

    pMax = ReduceLightBleeding(pMax, lightBleedingReduction);

    return (mean <= moments.x ? 1.0 : pMax);
}

float ShadowPCF2x2(vec2 coord, float depth, float texel) {
    vec2 p = coord / texel - 0.5;
    vec2 base = floor(p);
    vec2 f = p - base;
    vec2 uv0 = (base + 0.5) * texel;
    vec4 d = vec4(textureLod(sampler2D(t_atlas, LINEAR_S), uv0, 0.0).x, textureLod(sampler2D(t_atlas, LINEAR_S), uv0 + vec2(texel, 0.0), 0.0).x,
                  textureLod(sampler2D(t_atlas, LINEAR_S), uv0 + vec2(0.0, texel), 0.0).x, textureLod(sampler2D(t_atlas, LINEAR_S), uv0 + texel, 0.0).x);
    vec4 lit = vec4(lessThanEqual(vec4(depth), d));
    return mix(mix(lit.x, lit.y, f.x), mix(lit.z, lit.w, f.x), f.y);
}

#ifndef CHARACTER_SHADOW_MAP
#define CHARACTER_SHADOW_MAP 0
#endif
#if CHARACTER_SHADOW_MAP
// The same 2 x 2 PCF on the player's own map (TESR_CharShadowMap; POINT in the .fx: the taps sit on texel centres, where LINEAR
// returns the texel itself).
float ShadowPCF2x2Char(vec2 coord, float depth, float texel) {
    vec2 p = coord / texel - 0.5;
    vec2 base = floor(p);
    vec2 f = p - base;
    vec2 uv0 = (base + 0.5) * texel;
    vec4 d = vec4(textureLod(sampler2D(t_charShadow, LINEAR_S), uv0, 0.0).x, textureLod(sampler2D(t_charShadow, LINEAR_S), uv0 + vec2(texel, 0.0), 0.0).x,
                  textureLod(sampler2D(t_charShadow, LINEAR_S), uv0 + vec2(0.0, texel), 0.0).x, textureLod(sampler2D(t_charShadow, LINEAR_S), uv0 + texel, 0.0).x);
    vec4 lit = vec4(lessThanEqual(vec4(depth), d));
    return mix(mix(lit.x, lit.y, f.x), mix(lit.z, lit.w, f.x), f.y);
}
#endif

float GetLightAmountValueVSM(vec2 moments, float depth, float bias, float bleedReduction) {
    return ChebyshevUpperBound(moments, depth, bias, bleedReduction);
}

vec2 GetEVSMExponents(float positiveExponent, float negativeExponent, float formatBits) {
    float maxExponent = formatBits == 0.0 ? 5.54 : 42.0;

    vec2 lightSpaceExponents = vec2(positiveExponent, negativeExponent);

    return min(lightSpaceExponents, vec2(maxExponent));
}

vec2 WarpDepth(float depth, vec2 exponents) {
    depth = 2.0 * depth - 1.0;
    float pos = exp(exponents.x * depth);
    float neg = -exp(-exponents.y * depth);
    return vec2(pos, neg);
}

float GetLightAmountValueEVSM2(vec2 moments, float depth, float bias, float bleedReduction, float formatBits) {
    vec2 exponents = GetEVSMExponents(40.0, 5.0, formatBits);
    vec2 warpedDepth = WarpDepth(depth, exponents);

    vec2 depthScale = bias * exponents * warpedDepth;
    vec2 minVariance = depthScale * depthScale;

    return ChebyshevUpperBound(moments, warpedDepth.x, minVariance.x, bleedReduction);
}

float GetLightAmountValueEVSM4(vec4 moments, float depth, float bias, float bleedReduction, float formatBits) {
    vec2 exponents = GetEVSMExponents(40.0, 5.0, formatBits);
    vec2 warpedDepth = WarpDepth(depth, exponents);

    vec2 depthScale = bias * exponents * warpedDepth;
    vec2 minVariance = depthScale * depthScale;

    float posContrib = ChebyshevUpperBound(moments.xz, warpedDepth.x, minVariance.x, bleedReduction);
    float negContrib = ChebyshevUpperBound(moments.yw, warpedDepth.y, minVariance.y, bleedReduction);

    return min(posContrib, negContrib);
}

// --- SunShadows.fx.hlsl ---
vec4 ScreenCoordToTexCoord(vec4 coord) {
    coord.xyz /= coord.w;
    coord.x = coord.x * 0.5 + 0.5;
    coord.y = coord.y * -0.5 + 0.5;

    return coord;
}

vec4 SampleShadowMoments(vec2 coord, vec2 quadrantOffset) {
    return textureLod(sampler2D(t_atlas, LINEAR_S), coord, 0.0);
}

float GetLightAmountValue(mat4 lightTransform, vec4 coord, float offsetX, float offsetY, float bias, float bleedReduction) {
    vec4 LightSpaceCoord = ScreenCoordToTexCoord(lightTransform * coord);

    LightSpaceCoord.xy *= 0.5;
    LightSpaceCoord.x += offsetX;
    LightSpaceCoord.y += offsetY;

    vec4 shadowBufferValue = SampleShadowMoments(LightSpaceCoord.xy, vec2(offsetX, offsetY));

    float shadow;

    if (Mode == 0.0)
        shadow = GetLightAmountValueVSM(shadowBufferValue.xy, LightSpaceCoord.z, bias, bleedReduction);
    else if (Mode == 1.0)
        shadow = GetLightAmountValueEVSM2(shadowBufferValue.xy, LightSpaceCoord.z, bias, bleedReduction, FormatBits);
    else
        shadow = GetLightAmountValueEVSM4(shadowBufferValue, LightSpaceCoord.z, bias, bleedReduction, FormatBits);
    shadow = mix(shadow, ShadowPCF2x2(LightSpaceCoord.xy, LightSpaceCoord.z - 0.0002, cb.shadowBlur.x), saturate(Mode - 2.0));

    return shadow;
}

float GetLightAmountCascades(vec4 positionWS, vec3 normal) {
    float NdotL = dot(normal, cb.smoothedSunDir.xyz);
    float offsetScale = saturate(1.0 - NdotL);

    vec4 radii = vec4(
        cb.nearCenter.w,
        cb.middleCenter.w,
        cb.farCenter.w,
        cb.lodCenter.w);
    vec4 texelWorld = 4.0 * radii * max(cb.shadowBlur.x, 1.0 / 16384.0);
    vec4 offsetDistance = offsetScale * NormalBiasTexels * texelWorld;

    float bias = (Mode == 0.0 ? 0.00001 : 0.01) * (1.0 + SlopeBias * offsetScale);

    vec4 bands = max(0.1 * radii, min(vec4(cb.forwardData.w), radii));
    vec4 starts = radii - bands;

#define TAP_NEAR(o) GetLightAmountValue(cb.cascadeNear, vec4(positionWS.xyz + (o) * normal, 1.0), 0.0, 0.0, bias, 0.1)
#define TAP_MIDDLE(o) GetLightAmountValue(cb.cascadeMiddle, vec4(positionWS.xyz + (o) * normal, 1.0), 0.5, 0.0, bias, 0.2)
#define TAP_FAR(o) GetLightAmountValue(cb.cascadeFar, vec4(positionWS.xyz + (o) * normal, 1.0), 0.0, 0.5, bias, 0.6)
#define TAP_LOD(o) GetLightAmountValue(cb.cascadeLod, vec4(positionWS.xyz + (o) * normal, 1.0), 0.5, 0.5, bias, 0.8)

    vec4 distances = vec4(
        length(positionWS.xyz - cb.nearCenter.xyz),
        length(positionWS.xyz - cb.middleCenter.xyz),
        length(positionWS.xyz - cb.farCenter.xyz),
        length(positionWS.xyz - cb.lodCenter.xyz));

    if (distances.x < radii.x) {
        float f = hlslSmoothstep(starts.x, radii.x, distances.x);
        float o = mix(offsetDistance.x, offsetDistance.y, f);
        if (f <= 0.0)
            return TAP_NEAR(o);

        return mix(TAP_NEAR(o), TAP_MIDDLE(o), f);
    }
    else if (distances.y < radii.y) {
        float f = hlslSmoothstep(starts.y, radii.y, distances.y);
        float o = mix(offsetDistance.y, offsetDistance.z, f);
        if (f <= 0.0)
            return TAP_MIDDLE(o);

        return mix(TAP_MIDDLE(o), TAP_FAR(o), f);
    }
    else if (distances.z < radii.z) {
        float f = hlslSmoothstep(starts.z, radii.z, distances.z);
        float o = mix(offsetDistance.z, offsetDistance.w, f);
        if (f <= 0.0)
            return TAP_FAR(o);

        return mix(TAP_FAR(o), TAP_LOD(o), f);
    }
    else if (distances.w < radii.w) {
        float f = hlslSmoothstep(starts.w, radii.w, distances.w);
        if (f <= 0.0)
            return TAP_LOD(offsetDistance.w);

        return mix(TAP_LOD(offsetDistance.w), 1.0, f);
    }
    else {
        return 1.0;
    }
}
#undef TAP_NEAR
#undef TAP_MIDDLE
#undef TAP_FAR
#undef TAP_LOD

// (2026-10-23 CharacterShadowMap, ported 2026-10-26) the cascades' light amount, and with CHARACTER_SHADOW_MAP the darker of it and
// the player's own map's inside that map's box, fading to the cascades alone over the box's outer band (SunShadows.fx GetLightAmount).
float GetLightAmount(vec4 positionWS, vec3 normal) {
    float shadow = GetLightAmountCascades(positionWS, normal);
#if CHARACTER_SHADOW_MAP
    float texel = max(cb.charShadowData.x, 1.0 / 16384.0);
    float halfExtent = max(cb.charShadowCenter.w, 1.0);
    float NdotL = dot(normal, cb.smoothedSunDir.xyz);
    float offsetScale = saturate(1.0 - NdotL);
    vec3 position = positionWS.xyz + normal * (offsetScale * 1.5 * 2.0 * halfExtent * texel);
    vec4 lightSpace = cb.charShadowCameraToLight * vec4(position, 1.0);   // mul(float4(position, 1), TESR_CharShadowCameraToLight)
    float edge = max(abs(lightSpace.x), abs(lightSpace.y));
    float band = max(cb.charShadowData.y, 0.01);
    float weight = (1.0 - hlslSmoothstep(1.0 - band, 1.0, edge)) * cb.charShadowData.w;
    if (weight > 0.0) {
        vec2 cuv = vec2(lightSpace.x * 0.5 + 0.5, lightSpace.y * -0.5 + 0.5);
        float slope = min(sqrt(saturate(1.0 - NdotL * NdotL)) / max(NdotL, 0.05), 5.0);
        float depth = min(lightSpace.z, 0.9999) - cb.charShadowData.z - texel * slope;
        float lit = ShadowPCF2x2Char(clamp(cuv, texel, 1.0 - texel), depth, texel);
        shadow = mix(shadow, min(shadow, lit), weight);
    }
#endif
    return shadow;
}

// Shadow: a shadow value from the darkness setting (full shadow) to 1 (full light)
void main() {
    vec2 ruv = toRectUV(uv);
    vec4 Shadow = sampleShadow(ruv);
    Shadow = pow(Shadow, vec4(cb.screenSpaceData.w));
    if (cb.shadowFade.y == 0.0) { o_color = Shadow; return; }

    // FORWARD_SHADOWS as compiled: the forward lookups running (TESR_ShadowForwardData.x 0) leave the cascades to the object shaders.
    if (cb.flags.x != 0.0 && cb.forwardData.x == 0.0) { o_color = Shadow; return; }

    float viewDepth;
    vec4 worldPos = reconstructWorldPositionLod(uv, viewDepth);
    vec3 normal = GetWorldNormalLod(ruv);
    float cascades = GetLightAmount(worldPos, normal);
    // UNOFFICIAL 2026-10-28 RT sun shadows, Mode 1 (replace; flags.w = the RT range, 0 = off): the cascades only from where the
    // traced shadows fade out (the last 15% of the range) on; the traced visibility is multiplied in afterwards (RtSunCombine).
    if (cb.flags.w > 0.0) cascades = mix(1.0, cascades, saturate((viewDepth - 0.85 * cb.flags.w) / (0.15 * cb.flags.w)));
    Shadow.r = min(Shadow.r, cascades);

    o_color = Shadow;
}
