// VolumetricFog pass Estimate as a Vulkan pass (needs NVR DXVK): Effects/VolumetricFog.fx.hlsl DedicatedFogEstimate (HalfVS's uv:
// the half-resolution texel centre) line for line, into NVR_FogBuffer0 (multiply + depth) and NVR_FogBuffer1 (add) at once.
// The first pass of DedicatedFog, CompositeFog, CompositeFogInterior and their Legacy and Classic forms (push: legacy, classic,
// interior). effects/VolumetricFog.cpp ExtDedicated.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"
#include "VolumetricFog.glsl"

layout(location = 0) out vec4 o_mulDepth;
layout(location = 1) out vec4 o_add;

void main() {
    vec2 ruv = uv * fogRectScale();
    vec2 texel = cb.reciprocalResolution.xy;
    vec2 h = 0.5 * (pc.fogLayout.zw - texel);
    vec2 best = fogClampToRect(ruv - h, texel);
    float bestDepth = readDepthLod(best);
    const vec2 signs[3] = vec2[3](vec2(1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0));	// others: (h.x, -h.y), (-h.x, h.y), (h.x, h.y)
    for (int i = 0; i < 3; i++) {
        vec2 candidate = fogClampToRect(ruv + signs[i] * h, texel);
        float candidateDepth = readDepthLod(candidate);
        if (candidateDepth < bestDepth) { best = candidate; bestDepth = candidateDepth; }
    }
    vec3 fogMul, fogAdd;
    float nightAmount;
    FogTerms(best, fogMul, fogAdd, nightAmount, FOG_LEGACY_ON, FOG_INTERIOR_ON, FOG_CLASSIC_ON);
    o_mulDepth = vec4(fogMul, saturate(bestDepth / farZ));
    o_add = vec4(fogAdd, 1.0);
}
