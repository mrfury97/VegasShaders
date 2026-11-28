// PointShadows technique 4 (LampMaskUpsample) as a Vulkan pass (needs NVR DXVK): Effects/PointShadows.fx.hlsl's LampMaskUpsamplePS, line
// for line. The half-resolution mask (R; G holds its depth) upsampled depth-aware to the full-resolution buffer.
// effects/PointShadows.cpp ExtRender runs it.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "PointShadowsCommon.glsl"

layout(set = 1, binding = 0) uniform texture2D t_mask;    // NVR_LampMaskHalf (POINT, WRAP: the .fx's sampler state; G16R16F)
layout(set = 1, binding = 1) uniform texture2D t_depth;   // TESR_DepthBuffer (LINEAR, CLAMP)

layout(push_constant) uniform Push {
    uint linearSampler;   // 0: LINEAR, CLAMP heap index for the depth (filled by the host)
    uint pointSampler;    // 4: POINT, WRAP heap index for the mask
    uint mode;            // 8: unused here
    uint pad;
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

#define LINEAR_CLAMP NVR_SAMPLER(pc.linearSampler)
#define POINT_WRAP NVR_SAMPLER(pc.pointSampler)

void main() {
    vec2 lowRR = 2.0 * cb.reciprocalResolution.xy;
    vec2 minTap = 0.5 * lowRR;
    vec2 maxTap = max(cb.renderScale.xy - minTap, minTap);
    vec2 ruv = uv * cb.renderScale.xy;
    vec2 position = ruv / lowRR - 0.5;
    vec2 base = floor(position);
    vec2 fraction = fract(position);
    vec2 minDepthUV = 0.5 * cb.reciprocalResolution.xy;
    vec2 maxDepthUV = max(cb.renderScale.xy - minDepthUV, minDepthUV);
    float depth = readDepthAt(t_depth, LINEAR_CLAMP, clamp(ruv, minDepthUV, maxDepthUV));
    float tolerance = 1.0 + 0.01 * depth;
    float sum = 0.0, weights = 0.0;
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            vec2 tapUV = clamp((base + vec2(float(x), float(y)) + 0.5) * lowRR, minTap, maxTap);
            vec2 tap = textureLod(sampler2D(t_mask, POINT_WRAP), tapUV, 0.0).rg;
            float weight = (x != 0 ? fraction.x : 1.0 - fraction.x) * (y != 0 ? fraction.y : 1.0 - fraction.y) + 1e-4;
            float gap = abs(tap.y - depth) / tolerance;
            weight /= 1.0 + gap * gap * 16.0;
            sum += tap.x * weight;
            weights += weight;
        }
    }
    float mask = sum / max(weights, 1e-6);
    o_color = vec4(mask, mask, mask, 1.0);
}
