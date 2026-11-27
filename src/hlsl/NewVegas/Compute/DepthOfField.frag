// DepthOfField, technique DistantBlurOnly, as a Vulkan pass (needs NVR DXVK): Effects/DepthOfField.fx.hlsl DistantOnly line for
// line. effects/DepthOfField.cpp ExtRender. The matrices are the D3DXMATRIX memory the D3D9 constants are uploaded from: view[i][j]
// is HLSL TESR_ViewTransform[i][j]. t_rendered is TESR_RenderedBuffer, t_depth the CombineDepth texture (TESR_DepthBuffer).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 1) uniform texture2D t_depth;      // TESR_DepthBuffer (LINEAR, CLAMP)

layout(push_constant) uniform Push {
    vec4 viewTransform[4];        // 0: TESR_ViewTransform
    vec4 projectionTransform[4];  // 64: TESR_ProjectionTransform
    vec4 reciprocalResolution;    // 128: TESR_ReciprocalResolution
    vec4 blur;                    // 144: TESR_DepthOfFieldBlur (x distant blur, y start, z end, w base blur radius)
    vec4 data;                    // 160: TESR_DepthOfFieldData (not read by this technique)
    vec4 cameraPosition;          // 176: TESR_CameraPosition
    vec4 cameraData;              // 192: TESR_CameraData (y far Z)
    uint samplerIndex;            // 208: LINEAR, CLAMP
    float useBokeh;               // 212: (not read here)
    vec2 jitter;                  // 216: UNOFFICIAL 2026-11-01 JitterFix (TESR_DepthOfFieldJitter.xy)
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

// Depth.hlsl: readDepth and readDepthLod (one mip level, so LOD 0 is what tex2D picks anyway)
float readDepth(vec2 coord) {
    return textureLod(sampler2D(t_depth, NVR_SAMPLER(pc.samplerIndex)), coord - pc.jitter, 0.0).x * pc.cameraData.y;	// (JitterFix)
}
float readDepthLod(vec2 coord) { return readDepth(coord); }

// Depth.hlsl: toWorld
vec3 toWorld(vec2 tex) {
    vec3 v = vec3(pc.viewTransform[0][2], pc.viewTransform[1][2], pc.viewTransform[2][2]);
    v += (1.0 / pc.projectionTransform[0][0] * (2.0 * tex.x - 1.0)) * vec3(pc.viewTransform[0][0], pc.viewTransform[1][0], pc.viewTransform[2][0]);
    v += (-1.0 / pc.projectionTransform[1][1] * (2.0 * tex.y - 1.0)) * vec3(pc.viewTransform[0][1], pc.viewTransform[1][1], pc.viewTransform[2][1]);
    return v;
}

// Helpers.hlsl: invlerps (a macro in HLSL, a function here)
float invlerps(float a, float b, float t) { return saturate((t - a) / (b - a)); }

void main() {
    const float DistantStrength = max(pc.blur.x, 0.0);
    const float DistantStart = pc.blur.y;
    const float DistantEnd = pc.blur.z;
    const float BaseBlurRadius = pc.blur.w;

    float depth = readDepth(uv);
    vec3 cameraVector = toWorld(uv) * depth;
    float worldHeight = pc.cameraPosition.z + cameraVector.z;
    float distRamp = invlerps(DistantStart, DistantEnd, depth) * invlerps(100000.0, 5000.0, worldHeight);
    float blur = saturate(distRamp * min(DistantStrength, 1.0));

    // Reads the current image (TESR_RenderedBuffer), as the D3D9 pass does.
    vec4 center = textureLod(sampler2D(t_rendered, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0);
    if (blur <= 0.0) {
        o_color = vec4(center.rgb, 1.0);
        return;
    }

    vec2 radius = pc.reciprocalResolution.xy * BaseBlurRadius * max(DistantStrength, 1.0) * (1.0 + blur);
    const vec2 corners[4] = vec2[4](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0));
    vec4 blurred = vec4(0.0);
    float weights = 0.0;
    for (int k = 0; k < 4; k++) {
        vec2 tapUV = uv + radius * corners[k];
        float weight = saturate(1.5 - abs(readDepthLod(tapUV) - depth) / (0.15 * depth));
        blurred += textureLod(sampler2D(t_rendered, NVR_SAMPLER(pc.samplerIndex)), tapUV, 0.0) * weight;
        weights += weight;
    }
    blurred = weights > 0.0001 ? blurred / weights : center;
    o_color = vec4(mix(center.rgb, blurred.rgb, blur), 1.0);
}
