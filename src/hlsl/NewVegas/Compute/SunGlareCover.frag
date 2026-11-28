// SunGlareCover as a Vulkan pass (needs NVR DXVK): Effects/SunGlare.fx.hlsl PS_Cover line for line.
// Drawn into 1x1 TESR_SunGlareCover with NVR_EXT_BLEND_ALPHA (SrcAlpha / InvSrcAlpha).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "SunGlareCommon.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 1) uniform texture2D t_depth;      // TESR_DepthBuffer (POINT, CLAMP)

layout(push_constant) uniform Push {
    uint linearSampler;     // 0: LINEAR, CLAMP
    uint pointSampler;      // 4: POINT, CLAMP
    uint pad[2];            // 8..15
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

const int SG_COVER_TAPS = 16;
const int SG_RING_TAPS = 8;

vec2 sgCover(vec2 sun) {
    const float aspect = cb.reciprocalResolution.z;
    const mat2 golden = mat2(-0.7373688, 0.6754904, -0.6754904, -0.7373688);
    vec2 dir = vec2(1.0, 0.0);
    float open = 0.0;
    float patchSum = 0.0;
    for (int i = 0; i < SG_COVER_TAPS; i++) {
        vec2 at = sun + dir * (cb.glareSun.x * sqrt((float(i) + 0.5) / float(SG_COVER_TAPS))) * vec2(aspect, 1.0);
        float inside = (at.x > 0.0 && at.y > 0.0 && at.x < 1.0 && at.y < 1.0) ? 1.0 : 0.0;
        vec2 rect = clamp(at, 0.0, 1.0) * cb.renderScale.xy;
        float sky = textureLod(sampler2D(t_depth, NVR_SAMPLER(pc.pointSampler)), rect, 0.0).x > 0.9 ? inside : 0.0;
        open += sky;
        patchSum += luma(linearize(textureLod(sampler2D(t_rendered, NVR_SAMPLER(pc.linearSampler)), rect, 0.0).rgb)) * sky;
        dir = golden * dir;
    }
    float ring = 0.0;
    float ringCount = 0.0;
    dir = vec2(0.9238795, 0.3826834);
    const mat2 eighth = mat2(0.7071068, 0.7071068, -0.7071068, 0.7071068);
    for (int j = 0; j < SG_RING_TAPS; j++) {
        vec2 at = sun + dir * (cb.glareSun.x * 7.0) * vec2(aspect, 1.0);
        vec2 rect = clamp(at, 0.0, 1.0) * cb.renderScale.xy;
        float sky = (at.x > 0.0 && at.y > 0.0 && at.x < 1.0 && at.y < 1.0 &&
                     textureLod(sampler2D(t_depth, NVR_SAMPLER(pc.pointSampler)), rect, 0.0).x > 0.9) ? 1.0 : 0.0;
        ring += luma(linearize(textureLod(sampler2D(t_rendered, NVR_SAMPLER(pc.linearSampler)), rect, 0.0).rgb)) * sky;
        ringCount += sky;
        dir = eighth * dir;
    }
    float share = open / float(SG_COVER_TAPS);
    float contrast = (patchSum / max(open, 1.0)) / max(ringCount > 0.5 ? ring / ringCount : 0.25, 1.0e-4);
    return vec2(share, smoothstep(cb.glareSun.y, cb.glareSun.z, contrast));
}

void main() {
    vec2 sun = sgSunScreen();
    vec2 edge = min(sun, 1.0 - sun);
    float onScreen = saturate(min(edge.x, edge.y) / 0.04 + 1.0);
    onScreen *= (cb.viewSpaceLightDir.z > 0.0) ? 1.0 : 0.0;
    vec2 cover = sgCover(sun);
    o_color = vec4(cover.x * cover.y * onScreen, cover.x, cover.y, cb.glareSun.w);
}
