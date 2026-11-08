// VolumetricLight technique GridLight (5) as a Vulkan pass (needs NVR DXVK): Effects/VolumetricLight.fx.hlsl GridLightPS,
// FogHeightWeight and FogNoiseAt line for line, into the froxel grid. effects/VolumetricLight.cpp ExtRender.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_atlas;      // TESR_ShadowAtlas (LINEAR, CLAMP)
layout(set = 1, binding = 1) uniform texture3D t_noise;      // NVR_FogNoise (LINEAR, WRAP; read at level 0)
layout(set = 1, binding = 2) uniform texture2D t_rtvis;      // TESR_RtFogVisibility (LINEAR, CLAMP; UNOFFICIAL 2026-11-07 ptaaa-fog)

#define VL_SHADOWS
#define VL_GRID
#define VL_RT
#include "VolumetricLight.glsl"

float FogHeightWeight(float height, float nightFactor) {
    float fogPower = mix(1.0, cb.fogData.w, saturate(cb.fogDensity.y));
    float heightFalloff = max(0.0001, cb.fogShape.x) * mix(1.0, max(0.0, cb.fogNight.z), nightFactor);
    float falloff = 1.5e-4 / (fogPower * heightFalloff);
    // UNOFFICIAL 2026-11-01 HeightLayer (the .fx line for line)
    if (cb.fogShape.x < 0.0)
        return mix(1.0, exp(-max(cb.cameraPosition.z + height - cb.fogShape.y, 0.0) * (-cb.fogShape.x)), saturate(cb.fogScatter.w));
    return mix(1.0, exp(clamp(-falloff * height, -20.0, 10.0)), saturate(cb.fogScatter.w));
}

float FogNoiseAt(vec3 position, float nightFactor) {
    float windSpeed = cb.fogWind.z * mix(1.0, max(0.0, cb.fogNightScatter.y), nightFactor);
    vec3 windOffset = vec3(cb.fogWind.xy * windSpeed * cb.gameTime.x * 0.002, 0.0);
    vec3 noisePos = (cb.cameraPosition.xyz + position + windOffset) / (1500.0 * max(0.0001, cb.fogWind.w));
    return textureLod(sampler3D(t_noise, NVR_SAMPLER(pc.wrapIndex)), noisePos / 4.0, 0.0).g;
}

void main() {
    vec2 vpos = floor(gl_FragCoord.xy);
    vec2 tile = floor(vpos / GridSize);
    vec2 cell = vpos - tile * GridSize;
    float z = tile.y * 8.0 + tile.x;
    float near = GridDepth(z), far = GridDepth(z + 1.0);
    vec3 positionA = toWorld((cell + 0.25) / GridSize) * mix(near, far, 0.25);
    vec3 positionB = toWorld((cell + 0.75) / GridSize) * mix(near, far, 0.75);
    // UNOFFICIAL 2026-11-07 (ptaaa-fog) the traced visibility inside the RT range (the .fx line for line)
    vec3 centre = toWorld((cell + 0.5) / GridSize) * GridDepth(z + 0.5);
    float rtShare = RtShare(centre);
    float traced = 1.0;
    if (rtShare > 0.0) traced = cb.rt.x < 1.5 ? LINEAR_LOD(t_rtvis, (vpos + 0.5) * GridAtlasTexel).r : RtFogVisAt(centre);
    float shadowA = traced, shadowB = traced;
    if (rtShare < 1.0) {
        shadowA = mix(MarchShadow(positionA), traced, rtShare);
        shadowB = mix(MarchShadow(positionB), traced, rtShare);
    }
    float sigma = Density * length(toWorld((cell + 0.5) / GridSize));
    vec4 cellValue = vec4(0.5 * (shadowA + shadowB) * (exp(-sigma * near) - exp(-sigma * far)) * cb.gridData.y, 0.0, 0.0, 0.0);
    if (cb.gridData.x > 0.5) {
        float nightFactor = cb.fogWeather.y * (1.0 - smoothstep(0.4, 0.8, cb.sunAmount.x));
        float share = 0.5 * (far - near) / MarchDistance;
        float hA = FogHeightWeight(positionA.z, nightFactor), hB = FogHeightWeight(positionB.z, nightFactor);
        float nA = FogNoiseAt(positionA, nightFactor), nB = FogNoiseAt(positionB, nightFactor);
        cellValue.gba = share * vec3(hA + hB, hA * nA + hB * nB, hA * shadowA + hB * shadowB);
    }
    o_color = cellValue;
}
