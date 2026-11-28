// SunFar as a Vulkan pass (needs NVR DXVK): Effects/SunFar.fx.hlsl line for line, with the Includes it reads (Depth, Normals,
// Shadows, SunCascades). effects/SunFar.cpp ExtRender. Matrices: cb.m[a][b] is the HLSL's m[a][b] (the D3DX matrix as stored,
// the effect framework's logical matrix, as SkinScattering.frag reads them): mul(v, M) is M * v, mul(M, v) is v * M.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

// tex2Dlod(TESR_x, float4(coord, 0, lod)) with the sampler heap index s.
#define LOD(t, s, coord, lod) textureLod(sampler2D(t, NVR_SAMPLER(s)), (coord), (lod))

// The sampler heap indices, filled in by the host at these push offsets.
layout(push_constant) uniform Push {
    uint samplerPoint;     // 0: POINT, CLAMP (TESR_DepthBuffer, TESR_NormalsBuffer)
    uint samplerLinear;    // 4: LINEAR, CLAMP, no mips (the shadow atlas, the height maps, the volumetric and water maps)
    uint samplerMip;       // 8: LINEAR with all mips, WRAP (the sky's cloud layers and the blend textures)
    uint pad;
} pc;

layout(set = 1, binding = 0) uniform texture2D t_depth;          // TESR_DepthBuffer (linear depth / farZ)
layout(set = 1, binding = 1) uniform texture2D t_normals;        // TESR_NormalsBuffer
layout(set = 1, binding = 2) uniform texture2D t_atlas;          // TESR_ShadowAtlas
layout(set = 1, binding = 3) uniform texture2D t_terrainShadow;  // TESR_TerrainShadowBuffer
layout(set = 1, binding = 4) uniform texture2D t_terrainGround;  // TESR_TerrainShadowGround
layout(set = 1, binding = 5) uniform texture2D t_layer0;         // NVR_CloudLayer0
layout(set = 1, binding = 6) uniform texture2D t_layer1;
layout(set = 1, binding = 7) uniform texture2D t_layer2;
layout(set = 1, binding = 8) uniform texture2D t_layer3;
layout(set = 1, binding = 9) uniform texture2D t_blend0;         // NVR_CloudBlend0
layout(set = 1, binding = 10) uniform texture2D t_blend1;
layout(set = 1, binding = 11) uniform texture2D t_blend2;
layout(set = 1, binding = 12) uniform texture2D t_blend3;
layout(set = 1, binding = 13) uniform texture2D t_volume;        // NVR_VolumeCloudShadow
layout(set = 1, binding = 14) uniform texture2D t_water;         // NVR_WaterDepth
layout(set = 1, binding = 15) uniform texture2D t_scene;         // NVR_Scene (2026-10-25: the world as drawn; POINT)
layout(set = 1, binding = 16) uniform texture2D t_waterMask;     // WaterMask (2026-11-21: .g > 0.45 strictly on water pixels)

// The constants the HLSL reads (std140: vec4s and mat4s only, in the order of SunFarUniforms in SunFar.cpp).
layout(set = 2, binding = 0, std140) uniform Constants {
    vec4 waterSettings;             // TESR_WaterSettings
    vec4 shadowFade;                // TESR_ShadowFade
    vec4 shadowSunLight;            // TESR_ShadowSunLight
    vec4 shadowAmbientLight;        // TESR_ShadowAmbientLight
    vec4 pbrData;                   // TESR_PBRData
    vec4 pbrExtraData;              // TESR_PBRExtraData
    vec4 terrainData;               // TESR_TerrainData
    vec4 terrainSkyData;            // TESR_TerrainSkyData
    vec4 skyIrradiance[9];          // TESR_SkyIrradianceObjects (2026-10-25 name)
    vec4 skyIrradianceTerrain[9];   // TESR_SkyIrradianceTerrain
    vec4 terrainShadowData;         // TESR_TerrainShadowData
    vec4 terrainShadowRange;        // TESR_TerrainShadowRange
    vec4 terrainShadowParams;       // TESR_TerrainShadowParams
    vec4 sunFarData;                // TESR_SunFarData
    vec4 sunFarClouds;              // TESR_SunFarClouds
    vec4 sunFarCloudOffset[2];      // TESR_SunFarCloudOffset
    vec4 sunFarCloudWeight;         // TESR_SunFarCloudWeight
    vec4 sunFarCloudBlend;          // TESR_SunFarCloudBlend
    vec4 sunFarVolume;              // TESR_SunFarVolume
    vec4 sunDirection;              // TESR_SunDirection
    vec4 sunFarWater;               // TESR_SunFarWater (w: 1 OldSunModel / no scene texture)
    vec4 fogData;                   // TESR_FogData (2026-10-25)
    vec4 fogColor;                  // TESR_FogColor
    vec4 sunFarLand;                // TESR_SunFarLand
    vec4 sunFarJitter;              // TESR_SunFarJitter (w: water-mask mode)
    vec4 cameraData;                // TESR_CameraData
    vec4 cameraPosition;            // TESR_CameraPosition
    vec4 smoothedSunDir;            // TESR_SmoothedSunDir
    vec4 shadowFormatData;          // TESR_ShadowFormatData
    vec4 shadowBlur;                // TESR_ShadowBlur
    vec4 shadowNearCenter;          // TESR_ShadowNearCenter
    vec4 shadowMiddleCenter;        // TESR_ShadowMiddleCenter
    vec4 shadowFarCenter;           // TESR_ShadowFarCenter
    vec4 shadowLodCenter;           // TESR_ShadowLodCenter
    mat4 shadowToLightNear;         // TESR_ShadowCameraToLightTransformNear
    mat4 shadowToLightMiddle;       // TESR_ShadowCameraToLightTransformMiddle
    mat4 shadowToLightFar;          // TESR_ShadowCameraToLightTransformFar
    mat4 shadowToLightLod;          // TESR_ShadowCameraToLightTransformLod
    mat4 projectionTransform;       // TESR_ProjectionTransform
    mat4 viewTransform;             // TESR_ViewTransform
    vec4 renderScale;               // TESR_RenderScale (xy scale, zw 1/scale)
} cb;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

#define farZ (cb.cameraData.y)

// --- Includes/Depth.hlsl ---------------------------------------------------------------------------------------------------
float readDepthLod(vec2 coord) {
    return LOD(t_depth, pc.samplerPoint, coord, 0.0).x * farZ;
}

vec3 toWorld(vec2 tex) {
    vec3 v = vec3(cb.viewTransform[0][2], cb.viewTransform[1][2], cb.viewTransform[2][2]);
    v += (1.0 / cb.projectionTransform[0][0] * (2.0 * tex.x - 1.0)) * vec3(cb.viewTransform[0][0], cb.viewTransform[1][0], cb.viewTransform[2][0]);
    v += (-1.0 / cb.projectionTransform[1][1] * (2.0 * tex.y - 1.0)) * vec3(cb.viewTransform[0][1], cb.viewTransform[1][1], cb.viewTransform[2][1]);
    return v;
}

// --- Includes/Normals.hlsl -------------------------------------------------------------------------------------------------
vec3 GetWorldNormalLod(vec2 coord) {
    vec4 normal = vec4(LOD(t_normals, pc.samplerPoint, coord, 0.0).xyz * 2.0 - 1.0, 1.0);
    return (normal * cb.viewTransform).xyz;   // mul(TESR_ViewTransform, normal)
}

// --- Includes/Shadows.hlsl -------------------------------------------------------------------------------------------------
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

// UNOFFICIAL PCF (Shadows.hlsl ShadowPCF2x2 on TESR_ShadowAtlas).
float ShadowPCF2x2(vec2 uvc, float depth, float texel) {
    vec2 p = uvc / texel - 0.5;
    vec2 base = floor(p);
    vec2 f = p - base;
    vec2 uv0 = (base + 0.5) * texel;
    vec4 d = vec4(LOD(t_atlas, pc.samplerLinear, uv0, 0.0).x,
                  LOD(t_atlas, pc.samplerLinear, uv0 + vec2(texel, 0.0), 0.0).x,
                  LOD(t_atlas, pc.samplerLinear, uv0 + vec2(0.0, texel), 0.0).x,
                  LOD(t_atlas, pc.samplerLinear, uv0 + vec2(texel), 0.0).x);
    vec4 lit = vec4(depth <= d.x ? 1.0 : 0.0, depth <= d.y ? 1.0 : 0.0, depth <= d.z ? 1.0 : 0.0, depth <= d.w ? 1.0 : 0.0);
    return mix(mix(lit.x, lit.y, f.x), mix(lit.z, lit.w, f.x), f.y);
}

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

    float posContrib = ChebyshevUpperBound(vec2(moments.x, moments.z), warpedDepth.x, minVariance.x, bleedReduction);
    float negContrib = ChebyshevUpperBound(vec2(moments.y, moments.w), warpedDepth.y, minVariance.y, bleedReduction);

    return min(posContrib, negContrib);
}

// --- Includes/SunCascades.hlsl ---------------------------------------------------------------------------------------------
vec4 SunCascadeTexCoord(vec4 coord) {
    coord.xyz /= coord.w;
    coord.x = coord.x * 0.5 + 0.5;
    coord.y = coord.y * -0.5 + 0.5;
    return coord;
}

float SunCascadeValue(mat4 lightTransform, vec4 coord, float offsetX, float offsetY, float bias, float bleedReduction) {
    vec4 lightSpace = SunCascadeTexCoord(lightTransform * coord);   // mul(coord, lightTransform)
    lightSpace.xy = lightSpace.xy * 0.5 + vec2(offsetX, offsetY);
    if (cb.shadowFormatData.x == 3.0) return ShadowPCF2x2(lightSpace.xy, lightSpace.z - 0.0002, cb.shadowBlur.x);
    vec4 moments = LOD(t_atlas, pc.samplerLinear, lightSpace.xy, 0.0);
    float shadow;
    if (cb.shadowFormatData.x == 0.0)
        shadow = GetLightAmountValueVSM(moments.xy, lightSpace.z, bias, bleedReduction);
    else if (cb.shadowFormatData.x == 1.0)
        shadow = GetLightAmountValueEVSM2(moments.xy, lightSpace.z, bias, bleedReduction, cb.shadowFormatData.y);
    else
        shadow = GetLightAmountValueEVSM4(moments, lightSpace.z, bias, bleedReduction, cb.shadowFormatData.y);
    return shadow;
}

// --- Effects/SunFar.fx.hlsl ------------------------------------------------------------------------------------------------

// The cascades' sun visibility at a point: one lookup, in the cascade the point falls in (no cross-fade between them).
float CascadeLight(vec3 positionWS, vec3 normal) {
    vec4 radii = vec4(cb.shadowNearCenter.w, cb.shadowMiddleCenter.w, cb.shadowFarCenter.w, cb.shadowLodCenter.w);
    vec4 distances = vec4(length(positionWS - cb.shadowNearCenter.xyz), length(positionWS - cb.shadowMiddleCenter.xyz),
                          length(positionWS - cb.shadowFarCenter.xyz), length(positionWS - cb.shadowLodCenter.xyz));
    float c = distances.x < radii.x ? 0.0 : distances.y < radii.y ? 1.0 : distances.z < radii.z ? 2.0 : distances.w < radii.w ? 3.0 : 4.0;
    if (c > 3.5) return 1.0;
    mat4 lightTransform = c < 0.5 ? cb.shadowToLightNear : c < 1.5 ? cb.shadowToLightMiddle :
                          c < 2.5 ? cb.shadowToLightFar : cb.shadowToLightLod;
    vec2 quadrant = c < 0.5 ? vec2(0.0, 0.0) : c < 1.5 ? vec2(0.5, 0.0) : c < 2.5 ? vec2(0.0, 0.5) : vec2(0.5, 0.5);
    float radius = c < 0.5 ? radii.x : c < 1.5 ? radii.y : c < 2.5 ? radii.z : radii.w;
    float bleed = c < 0.5 ? 0.1 : c < 1.5 ? 0.2 : c < 2.5 ? 0.6 : 0.8;
    float offsetScale = saturate(1.0 - dot(normal, cb.smoothedSunDir.xyz));
    float offset = offsetScale * 2.5 * 4.0 * radius * max(cb.shadowBlur.x, 1.0 / 16384.0);
    float bias = (cb.shadowFormatData.x == 0.0 ? 0.00001 : 0.01) * (1.0 + offsetScale);
    return SunCascadeValue(lightTransform, vec4(positionWS + offset * normal, 1.0), quadrant.x, quadrant.y, bias, bleed);
}

// The space the object shaders light in: with linear lighting the colours and scales are decoded first.
vec3 LightColor(vec3 c) { return pow(max(c, vec3(1e-6)), vec3(cb.sunFarData.z)); }
float LightScale(float s) { return pow(max(s, 1e-6), cb.sunFarData.z); }

// Mirrors SkyAmbientRadiance (mode 0) in Shaders/Includes/SkyAmbient.hlsl, encoded the same way.
vec3 SkyIrradiance(vec3 n) {
    vec3 irradiance = cb.skyIrradiance[0].rgb
        + cb.skyIrradiance[1].rgb * n.y
        + cb.skyIrradiance[2].rgb * n.z
        + cb.skyIrradiance[3].rgb * n.x
        + cb.skyIrradiance[4].rgb * (n.x * n.y)
        + cb.skyIrradiance[5].rgb * (n.y * n.z)
        + cb.skyIrradiance[6].rgb * (3.0 * n.z * n.z - 1.0)
        + cb.skyIrradiance[7].rgb * (n.x * n.z)
        + cb.skyIrradiance[8].rgb * (n.x * n.x - n.y * n.y);
    return sqrt(max(irradiance, vec3(0.0)));
}

vec3 SkyIrradianceTerrain(vec3 n) {
    vec3 irradiance = cb.skyIrradianceTerrain[0].rgb
        + cb.skyIrradianceTerrain[1].rgb * n.y
        + cb.skyIrradianceTerrain[2].rgb * n.z
        + cb.skyIrradianceTerrain[3].rgb * n.x
        + cb.skyIrradianceTerrain[4].rgb * (n.x * n.y)
        + cb.skyIrradianceTerrain[5].rgb * (n.y * n.z)
        + cb.skyIrradianceTerrain[6].rgb * (3.0 * n.z * n.z - 1.0)
        + cb.skyIrradianceTerrain[7].rgb * (n.x * n.z)
        + cb.skyIrradianceTerrain[8].rgb * (n.x * n.x - n.y * n.y);
    return sqrt(max(irradiance, vec3(0.0)));
}

// Where the height map covers (DebugView 2): 1 inside, 0 outside or off.
float TerrainShadowCoverage(vec3 worldPos) {
    if (cb.terrainShadowData.w <= 0.0) return 0.0;
    vec2 uvh = vec2((worldPos.x - cb.terrainShadowData.x) * cb.terrainShadowData.z, (cb.terrainShadowData.y - worldPos.y) * cb.terrainShadowParams.x);
    return all(greaterThanEqual(uvh, vec2(0.0))) && all(lessThanEqual(uvh, vec2(1.0))) ? 1.0 : 0.0;
}

// 1 lit, 0 in the land's shadow. As CS's GetTerrainShadow, faded out toward the map's edge.
float TerrainShadow(vec3 worldPos) {
    if (cb.terrainShadowData.w <= 0.0) return 1.0;
    vec2 uvh = vec2((worldPos.x - cb.terrainShadowData.x) * cb.terrainShadowData.z, (cb.terrainShadowData.y - worldPos.y) * cb.terrainShadowParams.x);
    vec2 edge = min(uvh, 1.0 - uvh);
    float inside = saturate(min(edge.x, edge.y) * 20.0);
    if (inside <= 0.0) return 1.0;
    // The shadow heights (each sweep) and the ground (each map) in two textures, in the same 16-bit encoding.
    vec3 encoded = vec3(LOD(t_terrainShadow, pc.samplerLinear, uvh, 0.0).rg, LOD(t_terrainGround, pc.samplerLinear, uvh, 0.0).r) * cb.terrainShadowRange.y + cb.terrainShadowRange.x;
    vec2 heights = encoded.xy - cb.terrainShadowRange.z;
    // Never below the map's ground (the simplified LOD land dips under it in places); objects keep their height.
    float z = max(worldPos.z, encoded.z);
    float lower = heights.y - cb.terrainShadowRange.w;
    float transition = heights.x + cb.terrainShadowRange.w - lower;
    float lit = transition <= 0.0 ? (z >= heights.x ? 1.0 : 0.0) : saturate((z - lower) / transition);
    lit = saturate((lit - 0.5) * cb.terrainShadowParams.w + 0.5);   // Contrast: a steeper edge
    return mix(1.0, lit, cb.terrainShadowData.w * inside);
}

// One cloud layer's cover at uv, blended toward the incoming weather's texture as the sky blends it.
float CloudCover(texture2D layer, texture2D incoming, vec2 uvc, float weight, float blend) {
    float a = LOD(layer, pc.samplerMip, uvc, cb.sunFarClouds.z).a;
    a = mix(a, LOD(incoming, pc.samplerMip, uvc, cb.sunFarClouds.z).a, blend);
    return saturate(a * weight);
}

// 1 lit, toward 0 under the clouds: the light from this point (camera-relative) meets the cloud layer at the camera's height
// plus TESR_SunFarClouds.x; the four layers' cover there, stacked (each lets 1 - cover through).
float CloudShadow(vec3 rel) {
    if (cb.sunFarData.x <= 0.0) return 1.0;
    vec3 light = cb.smoothedSunDir.xyz;
    if (light.z <= 0.02) return 1.0;
    vec2 meet = rel.xy + light.xy * ((cb.sunFarClouds.x - rel.z) / light.z);
    vec2 world = meet + cb.cameraPosition.xy;
    float s = sin(cb.sunFarClouds.w);
    float c = cos(cb.sunFarClouds.w);
    vec2 uvc = vec2(world.x * c - world.y * s, world.x * s + world.y * c) * cb.sunFarClouds.y;
    float clear = (1.0 - CloudCover(t_layer0, t_blend0, uvc + cb.sunFarCloudOffset[0].xy, cb.sunFarCloudWeight.x, cb.sunFarCloudBlend.x))
                * (1.0 - CloudCover(t_layer1, t_blend1, uvc + cb.sunFarCloudOffset[0].zw, cb.sunFarCloudWeight.y, cb.sunFarCloudBlend.y))
                * (1.0 - CloudCover(t_layer2, t_blend2, uvc + cb.sunFarCloudOffset[1].xy, cb.sunFarCloudWeight.z, cb.sunFarCloudBlend.z))
                * (1.0 - CloudCover(t_layer3, t_blend3, uvc + cb.sunFarCloudOffset[1].zw, cb.sunFarCloudWeight.w, cb.sunFarCloudBlend.w));
    return saturate(1.0 - (1.0 - clear) * cb.sunFarData.x);
}

// 1 lit, toward 0 under the volumetric clouds, in metres (VolumetricClouds.fx CloudShadowAt).
float VolumeCloudShadow(vec3 world) {
    if (cb.sunFarVolume.x <= 0.0) return 1.0;
    vec3 light = normalize(cb.sunDirection.xyz);
    light.z = max(light.z, 0.25);
    light = normalize(light);
    vec3 p = world / 70.0;
    float texel = cb.sunFarVolume.y / cb.sunFarVolume.z;
    vec2 centre = floor(cb.cameraPosition.xy / 70.0 / texel) * texel;
    vec2 q = p.xy + light.xy * ((cb.sunFarVolume.w - p.z) / light.z);
    vec2 suv = (q - centre) / cb.sunFarVolume.y + 0.5;
    vec2 edge = min(suv, 1.0 - suv);
    float inside = saturate(min(edge.x, edge.y) * 10.0);
    return mix(1.0, LOD(t_volume, pc.samplerLinear, suv, 0.0).r, inside * cb.sunFarVolume.x);
}

// The water's surface: facing up at the water's height.
bool OnWater(vec3 world, vec3 normal, float depth) {
    return normal.z > 0.7 && abs(world.z - cb.waterSettings.x) < max(2.0, depth * 0.002);
}

bool IsWaterPixel(vec2 uvc, vec3 world, vec3 normal, float depth) {
    float mode = cb.sunFarJitter.w;
    if (mode > 1.5) return LOD(t_waterMask, pc.samplerPoint, uvc, 0.0).g > 0.45;
    if (mode > 0.5) return false;
    return OnWater(world, normal, depth);
}

// How deep the water is here, 0 at the shore to 1 deep, from the game's water depth map; far off 1.
float WaterDeep(vec2 uvc, vec3 rel) {
    float deep = saturate(LOD(t_water, pc.samplerLinear, uvc, 0.0).x);
    return mix(deep, 1.0, smoothstep(4096.0, 8192.0, length(rel.xy)));
}

vec4 SunFarPS(vec2 uvc) {
    vec2 ruv = uvc * cb.renderScale.xy;
    // Not on the sky (at the far plane) nor on nothing at all.
    float depth = readDepthLod(ruv);
    if (depth <= 0.0 || depth >= farZ * 0.996) return vec4(1.0);
    vec3 rel = toWorld(uvc + cb.sunFarJitter.xy) * depth;
    vec3 world = cb.cameraPosition.xyz + rel;

    float terrain = TerrainShadow(world);
    float cloud = CloudShadow(rel) * VolumeCloudShadow(world);
    float blocked = terrain * cloud;
    // DebugView 4 (test, 2026-10-25 as the .fx): as a fully clouded sky over the whole screen at Strength 1.
    blocked = cb.sunFarData.y > 3.5 ? 0.0 : blocked;
    // [TerrainShadows] Darkness: in the land's shadow, a share of the ambient and sky light goes too.
    float shade = (1.0 - terrain) * cb.terrainShadowParams.z;

    // Debug views: 1 the land's shadow in green and the clouds' in blue, through the composite (the picture tinted);
    // 2 also the height map's area in a faint blue; 3 also the water's depth (white at the shore to red deep).
    if (cb.sunFarData.y > 0.5 && cb.sunFarData.y < 3.5) {
        vec3 tint = mix(vec3(1.0), vec3(0.25, 1.0, 0.25), (1.0 - terrain) * 0.8) * mix(vec3(1.0), vec3(0.35, 0.5, 1.0), (1.0 - cloud) * 0.8);
        if (cb.sunFarData.y > 1.5) tint *= mix(vec3(1.0), vec3(0.85, 0.9, 1.0), TerrainShadowCoverage(world));
        float water = cb.sunFarWater.z * step(2.5, cb.sunFarData.y) * (IsWaterPixel(ruv, world, GetWorldNormalLod(ruv), depth) ? 1.0 : 0.0);
        tint *= mix(vec3(1.0), mix(vec3(1.0), vec3(1.0, 0.15, 0.15), WaterDeep(ruv, rel)), water);
        return vec4(tint, 1.0);
    }
    if (blocked >= 0.999 && shade <= 0.001) return vec4(1.0);   // most of the screen
    vec3 normal = GetWorldNormalLod(ruv);

    // Water is left alone (WaterShadows: shallow water fades in by its depth, the share of deep water is WaterDeepShare).
    float keep = 1.0;   // the share of the change this pixel takes (1 off the water)
    if (IsWaterPixel(ruv, world, normal, depth)) {
        keep = cb.sunFarWater.x * mix(1.0, cb.sunFarWater.y, WaterDeep(ruv, rel));
        if (keep <= 0.0) return vec4(1.0, 1.0, 1.0, blocked);
    }

    // What the cascades already took (faded as GetSunShadow fades them). Not on water (keep < 1).
    float cascade = 1.0;
    if (cb.shadowFade.y > 0.0 && keep >= 1.0 && cb.sunFarJitter.z < 0.5) cascade = saturate(mix(CascadeLight(rel, normal), 1.0, cb.shadowFade.x));

    // Terrain lights with its own scales: surfaces facing up take the terrain scales, walls and objects the PBR ones.
    float terrainWeight = (cb.terrainData.z + cb.terrainData.w > 0.0) ? smoothstep(0.5, 0.8, normal.z) : 0.0;
    vec3 scales = mix(vec3(cb.pbrData.z, cb.pbrData.w, cb.pbrExtraData.y), vec3(cb.terrainData.z, cb.terrainData.w, cb.terrainSkyData.x), terrainWeight);
    // (2026-10-25, as the .fx) the light vector the surface was lit with: the land's own (TESR_SunFarLand, w 1 when read) for the
    // surfaces taken for land, the directional light for the rest; TESR_SunFarWater.w 1 (OldSunModel / no scene texture): as before.
    vec3 lightDir = cb.sunFarWater.w > 0.5 ? cb.smoothedSunDir.xyz : normalize(cb.sunDirection.xyz);
    vec3 landDir = cb.sunFarLand.w > 0.5 && cb.sunFarWater.w < 0.5 ? cb.sunFarLand.xyz : lightDir;
    float NdotL = mix(saturate(dot(normal, lightDir)), saturate(dot(normal, landDir)), terrainWeight);
    vec3 sunLight = LightColor(cb.shadowSunLight.rgb) * (NdotL * LightScale(scales.x));
    vec3 skyIrr = mix(SkyIrradiance(normal), SkyIrradianceTerrain(normal), terrainWeight);
    vec3 ambientLight = LightColor(cb.shadowAmbientLight.rgb) * LightScale(scales.y) + LightColor(skyIrr) * LightScale(scales.z);
    vec3 litNow = ambientLight + sunLight * cascade;
    vec3 factor = saturate((ambientLight * (1.0 - shade) + sunLight * (cascade * blocked)) / max(litNow, vec3(0.0001)));
    factor = keep < 1.0 ? mix(vec3(1.0), factor, keep) : factor;
    // Into the stored space: with linear lighting the shaders encoded their result with 1 / gamma (w; 1 with it off).
    factor = pow(max(factor, vec3(1e-6)), vec3(cb.sunFarData.w));
    // (2026-10-25, as the .fx) the share of the pixel the shaders filled with the game's fog keeps its brightness (NVR_Scene: the
    // world as drawn). sunFarWater.w 1 (OldSunModel / no scene texture): as before.
    float fogAmount = pow(1.0 - saturate((cb.fogData.y - length(rel)) / max(cb.fogData.y - cb.fogData.x, 1.0)), max(cb.fogData.w, 0.0001));
    vec3 fogShare = saturate(fogAmount * cb.fogColor.rgb / max(LOD(t_scene, pc.samplerPoint, ruv, 0.0).rgb, vec3(0.0001)));
    factor = cb.sunFarWater.w > 0.5 ? factor : factor + (1.0 - factor) * fogShare;
    // a: what reaches the sun (1 lit).
    return vec4(factor, blocked);
}

void main() {
    o_color = SunFarPS(uv);
}
