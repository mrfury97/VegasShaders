// Specular (needs NVR DXVK): the functions the four Specular passes share, line for line with Effects/Specular.fx.hlsl and its
// Includes (Helpers, Depth, Shadows, SunCascades). effects/Specular.cpp fills the constants block from the values the D3D9 path
// uploads. A matrix is kept as stored: cb.m[i][j] is the HLSL's m[i][j]; HLSL mul(M, v) is transpose(cb.M) * v, HLSL mul(v, M) is
// cb.M * v.
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 2, binding = 0, std140) uniform Constants {
    mat4 viewTransform;               // TESR_ViewTransform
    mat4 projectionTransform;         // TESR_ProjectionTransform
    mat4 shadowNear;                  // TESR_ShadowCameraToLightTransformNear
    mat4 shadowMiddle;                // TESR_ShadowCameraToLightTransformMiddle
    mat4 shadowFar;                   // TESR_ShadowCameraToLightTransformFar
    mat4 shadowLod;                   // TESR_ShadowCameraToLightTransformLod
    vec4 reciprocalResolution;        // TESR_ReciprocalResolution
    vec4 specularData;                // TESR_SpecularData
    vec4 specularEffects;             // TESR_SpecularEffects
    vec4 sunDirection;                // TESR_SunDirection
    vec4 sunColor;                    // TESR_SunColor
    vec4 skyColor;                    // TESR_SkyColor
    vec4 horizonColor;                // TESR_HorizonColor
    vec4 waterSettings;               // TESR_WaterSettings
    vec4 shadowFade;                  // TESR_ShadowFade
    vec4 bounceData;                  // NVR_BounceData
    vec4 bounceLayout;                // NVR_BounceLayout
    vec4 specularExtra;               // NVR_SpecularExtra
    vec4 depthConstants;              // TESR_DepthConstants
    vec4 cameraData;                  // TESR_CameraData
    vec4 cameraPosition;              // TESR_CameraPosition
    vec4 smoothedSunDir;              // TESR_SmoothedSunDir
    vec4 shadowFormatData;            // TESR_ShadowFormatData
    vec4 shadowBlur;                  // TESR_ShadowBlur
    vec4 shadowNearCenter;            // TESR_ShadowNearCenter
    vec4 shadowMiddleCenter;          // TESR_ShadowMiddleCenter
    vec4 shadowFarCenter;             // TESR_ShadowFarCenter
    vec4 shadowLodCenter;             // TESR_ShadowLodCenter
} cb;

// Helpers.hlsl's PI (Specular.fx.hlsl's own static PI is replaced by it after the include)
const float PI_HLSL = 3.1415926538;
const float BOUNCE_NEAR_FADE = 40.0;   // BounceLight.fx.hlsl's NearFade, as Specular.fx.hlsl

// tex2Dlod(sampler, float4(uv, 0, 0)), the sampler's heap index given
vec4 sampleLod(texture2D tex, uint samp, vec2 uv) {
    return textureLod(sampler2D(tex, NVR_SAMPLER(samp)), uv, 0.0);
}

// Depth.hlsl: readDepthLod (the stored linear depth times farZ)
float readDepthLod(texture2D depthTex, uint samp, vec2 coord) {
    return sampleLod(depthTex, samp, coord).x * cb.cameraData.y;
}

// Depth.hlsl: toWorld
vec3 toWorld(vec2 tex) {
    vec3 v = vec3(cb.viewTransform[0][2], cb.viewTransform[1][2], cb.viewTransform[2][2]);
    v += (1.0 / cb.projectionTransform[0][0] * (2.0 * tex.x - 1.0)) * vec3(cb.viewTransform[0][0], cb.viewTransform[1][0], cb.viewTransform[2][0]);
    v += (-1.0 / cb.projectionTransform[1][1] * (2.0 * tex.y - 1.0)) * vec3(cb.viewTransform[0][1], cb.viewTransform[1][1], cb.viewTransform[2][1]);
    return v;
}

// Specular.fx.hlsl: ViewToWorld, mul(TESR_ViewTransform, float4(viewNormal, 1.0)).xyz
vec3 viewToWorld(vec3 viewNormal) {
    return (transpose(cb.viewTransform) * vec4(viewNormal, 1.0)).xyz;
}

// ---- Shadows.hlsl ----
float shadowPCF2x2(texture2D atlas, uint samp, vec2 uv, float depth, float texel) {
    vec2 p = uv / texel - 0.5;
    vec2 base = floor(p);
    vec2 f = p - base;
    vec2 uv0 = (base + 0.5) * texel;
    vec4 d = vec4(sampleLod(atlas, samp, uv0).x, sampleLod(atlas, samp, uv0 + vec2(texel, 0.0)).x,
                  sampleLod(atlas, samp, uv0 + vec2(0.0, texel)).x, sampleLod(atlas, samp, uv0 + texel).x);
    vec4 lit = vec4(lessThanEqual(vec4(depth), d));
    return mix(mix(lit.x, lit.y, f.x), mix(lit.z, lit.w, f.x), f.y);
}

float linstep(float a, float b, float v) {
    return clamp((v - a) / (b - a), 0.0, 1.0);
}

float chebyshevUpperBound(vec2 moments, float mean, float minVariance, float lightBleedingReduction) {
    float variance = moments.y - (moments.x * moments.x);
    variance = max(variance, minVariance);
    float d = mean - moments.x;
    float pMax = variance / (variance + (d * d));
    pMax = linstep(lightBleedingReduction, 1.0, pMax);   // ReduceLightBleeding
    return (mean <= moments.x ? 1.0 : pMax);
}

vec2 evsmExponents(float positiveExponent, float negativeExponent, float formatBits) {
    float maxExponent = formatBits == 0.0 ? 5.54 : 42.0;
    return min(vec2(positiveExponent, negativeExponent), vec2(maxExponent));
}

vec2 warpDepth(float depth, vec2 exponents) {
    depth = 2.0 * depth - 1.0;
    float pos = exp(exponents.x * depth);
    float neg = -exp(-exponents.y * depth);
    return vec2(pos, neg);
}

float evsm2(vec2 moments, float depth, float bias, float bleedReduction, float formatBits) {
    vec2 exponents = evsmExponents(40.0, 5.0, formatBits);
    vec2 warpedDepth = warpDepth(depth, exponents);
    vec2 depthScale = bias * exponents * warpedDepth;
    vec2 minVariance = depthScale * depthScale;
    return chebyshevUpperBound(moments, warpedDepth.x, minVariance.x, bleedReduction);
}

float evsm4(vec4 moments, float depth, float bias, float bleedReduction, float formatBits) {
    vec2 exponents = evsmExponents(40.0, 5.0, formatBits);
    vec2 warpedDepth = warpDepth(depth, exponents);
    vec2 depthScale = bias * exponents * warpedDepth;
    vec2 minVariance = depthScale * depthScale;
    float posContrib = chebyshevUpperBound(moments.xz, warpedDepth.x, minVariance.x, bleedReduction);
    float negContrib = chebyshevUpperBound(moments.yw, warpedDepth.y, minVariance.y, bleedReduction);
    return min(posContrib, negContrib);
}

// ---- SunCascades.hlsl ----
vec4 sunCascadeTexCoord(vec4 coord) {
    coord.xyz /= coord.w;
    coord.x = coord.x * 0.5 + 0.5;
    coord.y = coord.y * -0.5 + 0.5;
    return coord;
}

// lightTransform is a cb matrix; HLSL mul(coord, lightTransform) is cb matrix * coord
float sunCascadeValue(mat4 lightTransform, vec4 coord, float offsetX, float offsetY, float bias, float bleedReduction, texture2D atlas, uint samp) {
    vec4 lightSpace = sunCascadeTexCoord(lightTransform * coord);
    lightSpace.xy = lightSpace.xy * 0.5 + vec2(offsetX, offsetY);
    if (cb.shadowFormatData.x == 3.0) return shadowPCF2x2(atlas, samp, lightSpace.xy, lightSpace.z - 0.0002, cb.shadowBlur.x);
    vec4 moments = sampleLod(atlas, samp, lightSpace.xy);
    float shadow;
    if (cb.shadowFormatData.x == 0.0)
        shadow = chebyshevUpperBound(moments.xy, lightSpace.z, bias, bleedReduction);
    else if (cb.shadowFormatData.x == 1.0)
        shadow = evsm2(moments.xy, lightSpace.z, bias, bleedReduction, cb.shadowFormatData.y);
    else
        shadow = evsm4(moments, lightSpace.z, bias, bleedReduction, cb.shadowFormatData.y);
    return shadow;
}

float scNear(texture2D atlas, uint samp, vec3 p, vec3 n, float dist, float bias) {
    return sunCascadeValue(cb.shadowNear, vec4(p + dist * n, 1.0), 0.0, 0.0, bias, 0.1, atlas, samp);
}
float scMiddle(texture2D atlas, uint samp, vec3 p, vec3 n, float dist, float bias) {
    return sunCascadeValue(cb.shadowMiddle, vec4(p + dist * n, 1.0), 0.5, 0.0, bias, 0.2, atlas, samp);
}
float scFar(texture2D atlas, uint samp, vec3 p, vec3 n, float dist, float bias) {
    return sunCascadeValue(cb.shadowFar, vec4(p + dist * n, 1.0), 0.0, 0.5, bias, 0.6, atlas, samp);
}
float scLod(texture2D atlas, uint samp, vec3 p, vec3 n, float dist, float bias) {
    return sunCascadeValue(cb.shadowLod, vec4(p + dist * n, 1.0), 0.5, 0.5, bias, 0.8, atlas, samp);
}

// 1 lit .. 0 in the sun's shadow (camera-relative world position, world normal)
float sunCascadeLight(texture2D atlas, uint samp, vec3 positionWS, vec3 normal) {
    float offsetScale = saturate(1.0 - dot(normal, cb.smoothedSunDir.xyz));
    vec4 radii = vec4(cb.shadowNearCenter.w, cb.shadowMiddleCenter.w, cb.shadowFarCenter.w, cb.shadowLodCenter.w);
    vec4 offsetDistance = offsetScale * 2.5 * 4.0 * radii * max(cb.shadowBlur.x, 1.0 / 16384.0);
    float bias = (cb.shadowFormatData.x == 0.0 ? 0.00001 : 0.01) * (1.0 + offsetScale);
    const float blend = 0.9;
    vec4 distances = vec4(length(positionWS - cb.shadowNearCenter.xyz), length(positionWS - cb.shadowMiddleCenter.xyz),
                          length(positionWS - cb.shadowFarCenter.xyz), length(positionWS - cb.shadowLodCenter.xyz));
    float light = 1.0;
    if (distances.x < radii.x) {
        light = distances.x < radii.x * blend ? scNear(atlas, samp, positionWS, normal, offsetDistance.x, bias)
            : mix(scNear(atlas, samp, positionWS, normal, offsetDistance.x, bias), scMiddle(atlas, samp, positionWS, normal, offsetDistance.y, bias),
                  smoothstep(radii.x * blend, radii.x, distances.x));
    }
    else if (distances.y < radii.y) {
        light = distances.y < radii.y * blend ? scMiddle(atlas, samp, positionWS, normal, offsetDistance.y, bias)
            : mix(scMiddle(atlas, samp, positionWS, normal, offsetDistance.y, bias), scFar(atlas, samp, positionWS, normal, offsetDistance.z, bias),
                  smoothstep(radii.y * blend, radii.y, distances.y));
    }
    else if (distances.z < radii.z) {
        light = distances.z < radii.z * blend ? scFar(atlas, samp, positionWS, normal, offsetDistance.z, bias)
            : mix(scFar(atlas, samp, positionWS, normal, offsetDistance.z, bias), scLod(atlas, samp, positionWS, normal, offsetDistance.w, bias),
                  smoothstep(radii.z * blend, radii.z, distances.z));
    }
    else if (distances.w < radii.w) {
        light = distances.w < radii.w * blend ? scLod(atlas, samp, positionWS, normal, offsetDistance.w, bias)
            : mix(scLod(atlas, samp, positionWS, normal, offsetDistance.w, bias), 1.0, smoothstep(radii.w * blend, radii.w, distances.w));
    }
    return light;
}

// ---- Specular.fx.hlsl ----
// Karis, "Physically Based Shading on Mobile" (2014)
vec2 envBRDFApprox(float roughness, float NdotV) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
    return vec2(-1.04, 1.04) * a004 + r.zw;
}

// AddSpecular: adds the light to a pixel's linear colour; false, leaving it alone, where there is none.
bool addSpecular(vec2 uv, float depth, uint pointS, uint linearS, texture2D depthViewModel, texture2D normals, texture2D pointShadow,
                 texture2D specularBuffer, inout vec3 lin) {
    float viewmodelDepth = sampleLod(depthViewModel, pointS, uv).x;
    bool weapon = cb.depthConstants.z == 0.0 ? viewmodelDepth < 0.99999 : viewmodelDepth > 0.0;
    if (depth >= cb.specularData.w || depth >= cb.cameraData.y * 0.98 || weapon) return false;

    vec3 pixelPos = cb.cameraPosition.xyz + toWorld(uv) * depth;
    vec3 pixelNormal = viewToWorld(sampleLod(normals, linearS, uv).xyz * 2.0 - 1.0);
    if (normalize(pixelNormal).z > 0.7 && abs(pixelPos.z - cb.waterSettings.x) < max(2.0, depth * 0.002)) return false;

    vec2 px = cb.reciprocalResolution.xy;
    float lowScale = cb.specularEffects.z > 0.0 ? cb.specularEffects.z : 4.0;
    vec2 lowPx = px * lowScale;
    vec2 p = uv / lowPx - 0.5;
    vec2 f = fract(p);
    vec2 at = (floor(p) + 0.5) * lowPx;
    vec4 w = vec4((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y);
    vec4 s00 = sampleLod(specularBuffer, pointS, at);
    vec4 s10 = sampleLod(specularBuffer, pointS, at + vec2(lowPx.x, 0.0));
    vec4 s01 = sampleLod(specularBuffer, pointS, at + vec2(0.0, lowPx.y));
    vec4 s11 = sampleLod(specularBuffer, pointS, at + lowPx);
    vec4 off = abs(vec4(s00.a, s10.a, s01.a, s11.a) - depth) / (0.05 * depth + 4.0);
    off *= off;
    w *= 1.0 / (1.0 + off * off) + 0.0001;
    vec3 light = (s00.rgb * w.x + s10.rgb * w.y + s01.rgb * w.z + s11.rgb * w.w) / dot(w, vec4(1.0));

    float shadow = sampleLod(pointShadow, pointS, uv).r;
    float sunUp = 1.0 - cb.shadowFade.x;
    vec3 sky = mix(linearize(cb.horizonColor.rgb), linearize(cb.skyColor.rgb), light.g);
    lin = lin + light.r * sky + light.b * shadow * sunUp * linearize(cb.sunColor.rgb);
    return true;
}

// AddBounce: BounceLight's combine, as SpecularBouncePS does it before AddSpecular.
bool addBounce(vec2 uv, float depth, uint pointS, texture2D bounce, inout vec3 lin) {
    if (depth < BOUNCE_NEAR_FADE || depth > cb.bounceData.z) return false;
    vec2 position = uv / cb.bounceLayout.zw - 0.5;
    vec2 base = floor(position);
    vec2 fraction = position - base;
    float tolerance = max(depth * 0.03, 2.0);
    vec3 sum = vec3(0.0);
    float weights = 1.0e-5;
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 2; x++) {
            vec4 tap = sampleLod(bounce, pointS, (base + vec2(float(x), float(y)) + 0.5) * cb.bounceLayout.zw);
            float weight = (x != 0 ? fraction.x : 1.0 - fraction.x) * (y != 0 ? fraction.y : 1.0 - fraction.y);
            weight /= 1.0 + abs(tap.a - depth) / tolerance;
            sum += tap.rgb * weight;
            weights += weight;
        }
    }
    vec3 indirect = sum / weights;
    float peak = max(max(lin.r, lin.g), max(lin.b, 0.0001));
    vec3 albedo = lin / peak * 0.6;
    lin += cb.bounceData.x * indirect * albedo;
    return true;
}
