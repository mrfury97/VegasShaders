// AmbientOcclusion pass Combine of the Dedicated techniques (DedicatedCombine) as a Vulkan pass (needs NVR DXVK):
// Effects/AmbientOcclusion.fx.hlsl line for line (viewao 0), through the frame chain (effects/AmbientOcclusion.cpp Render).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "AmbientOcclusion.glsl"

layout(set = 1, binding = 0) uniform texture2D t_ao;         // NVR_AOBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 1) uniform texture2D t_depth;      // TESR_DepthBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 2) uniform texture2D t_source;     // TESR_SourceBuffer slot = the current image (LINEAR, CLAMP)
layout(set = 1, binding = 3) uniform texture2D t_skyVis;     // NVR_SkyVis (POINT, CLAMP); a stand-in unless cb.sky4.w > 0.5 (never read then)
layout(set = 1, binding = 4) uniform texture2D t_viewmodel;  // TESR_DepthBufferViewModel (POINT, CLAMP); a stand-in when pc.gtaoEstimate == 0u

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

float readDepth(vec2 coord) {
    return LINEAR_LOD(t_depth, coord).x * farZ;
}

void main() {
    vec2 ruv = uv * aoRectScale();
    if (pc.gtaoEstimate != 0u) {
        float vmD = POINT_LOD(t_viewmodel, ruv).x;
        if (invertedDepth ? vmD > 0.0 : vmD < 0.99999) { o_color = vec4(LINEAR_LOD(t_source, ruv).rgb, 1.0); return; }
    }
    vec2 position = ruv / cb.aoLayout.zw - 0.5;
    vec2 base = floor(position);
    vec2 fraction = fract(position);
    float depth = readDepth(ruv);
    if (depth >= endFade) { o_color = vec4(LINEAR_LOD(t_source, ruv).rgb, 1.0); return; }
    float sum = 0.0, weights = 0.0;
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            vec2 tapUV = (base + vec2(x, y) + 0.5) * cb.aoLayout.zw;
            vec2 tap = LINEAR_LOD(t_ao, DedicatedUV(tapUV)).rg;
            float weight = (x != 0 ? fraction.x : 1.0 - fraction.x) * (y != 0 ? fraction.y : 1.0 - fraction.y);
            weight /= 1.0 + abs(tap.y - depth) / max(blurDrop, 0.001);
            sum += tap.x * weight;
            weights += weight;
        }
    }
    float ao = mix(AOclamp, 1.0, sum / max(weights, 1.0e-6));
    vec3 source = LINEAR_LOD(t_source, ruv).rgb;
    vec3 linearColor = pows(source, 2.2);
    ao = mix(ao, 1.0, smoothstep(AOlumThreshold * 0.5, AOlumThreshold * 1.5 + 0.15, luma(linearColor)));
    vec3 result = source * pow(abs(ao), 1.0 / 2.2);
    if (cb.sky4.w > 0.5) {	// Skylighting v2: the sky factor, with the AO's upsample weights
        vec2 skySum = vec2(0.0);	// x the factor, y the visibility (LightCap)
        float skyWeights = 0.0;
        for (int sy = 0; sy < 2; ++sy) {
            for (int sx = 0; sx < 2; ++sx) {
                vec2 tapUV = (base + vec2(sx, sy) + 0.5) * cb.aoLayout.zw;
                vec2 tap = LINEAR_LOD(t_ao, DedicatedUV(tapUV)).rg;
                float weight = (sx != 0 ? fraction.x : 1.0 - fraction.x) * (sy != 0 ? fraction.y : 1.0 - fraction.y);
                weight /= 1.0 + abs(tap.y - depth) / max(blurDrop, 0.001);
                skySum += POINT_LOD(t_skyVis, DedicatedUV(tapUV)).rb * weight;
                skyWeights += weight;
            }
        }
        vec2 sky = clamp(skySum / max(skyWeights, 1.0e-6), 0.0, 1.0);
        result = pows(SkyTake(pows(result, 2.2), max(sky.x, AOclamp), sky.y, cb.sky5.x, cb.sky5.w), 1.0 / 2.2);
    }
    o_color = vec4(result, 1.0);
}
