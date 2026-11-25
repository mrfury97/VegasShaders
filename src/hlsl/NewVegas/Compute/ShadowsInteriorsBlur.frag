// ShadowsInteriors' dedicated blur passes (InteriorShadowBlur, both directions) as Vulkan passes (needs NVR DXVK):
// Effects/ShadowsInteriors.fx.hlsl with Includes/Blur.hlsl and Depth.hlsl, line for line. effects/ShadowsInteriors.cpp ExtRender.
// The mask is the shadow term (NVR_InteriorShadowIn, G16R16); the offset mask is (1,0) for the first pass, (0,1) for the second.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_mask;    // NVR_InteriorShadowIn: the shadow term (LINEAR, CLAMP)
layout(set = 1, binding = 1) uniform texture2D t_depth;   // TESR_DepthBuffer: linear depth / farZ (LINEAR, CLAMP)

layout(push_constant) uniform Push {
    vec4 cameraData;            // 0: TESR_CameraData (y: farZ)
    vec4 reciprocalResolution;  // 16: TESR_ReciprocalResolution
    vec2 offsetMask;            // 32: OffsetMaskH (1, 0) or OffsetMaskV (0, 1)
    float maxDistance;          // 40: MAXDISTANCE, TESR_ShadowFade.w: beyond it the pass writes black
    uint samplerIndex;          // 44: LINEAR, CLAMP
    vec2 renderScale;           // 48: xy render size / screen size (1 = off)
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

const int cKernelSize = 12;   // Blur.hlsl cKernelSize

// Blur.hlsl BlurWeights
const float blurWeights[cKernelSize] = float[cKernelSize](
    0.057424882,
    0.058107773,
    0.061460144,
    0.071020611,
    0.088092873,
    0.106530916,
    0.106530916,
    0.088092873,
    0.071020611,
    0.061460144,
    0.058107773,
    0.057424882
);

// Blur.hlsl BlurOffsets: -6..-1 then +1..+6 times TESR_ReciprocalResolution (no zero tap)
vec2 blurOffset(int i) {
    float k = i < 6 ? float(i - 6) : float(i - 5);
    return vec2(k * pc.reciprocalResolution.x, k * pc.reciprocalResolution.y);
}

// Includes/Depth.hlsl readDepthLod: the linear depth in game units
float readDepthLod(vec2 coord) {
    return textureLod(sampler2D(t_depth, NVR_SAMPLER(pc.samplerIndex)), coord, 0.0).x * pc.cameraData.y;
}

// NVR_InteriorShadowIn read with tex2Dlod
vec4 sampleMask(vec2 coord) {
    return textureLod(sampler2D(t_mask, NVR_SAMPLER(pc.samplerIndex)), coord, 0.0);
}

// ShadowsInteriors.fx.hlsl InteriorShadowBlur
void main() {
    vec2 ruv = uv * pc.renderScale;
    vec2 minUV = 0.5 * pc.reciprocalResolution.xy;
    vec2 maxUV = pc.renderScale - minUV;
    vec4 center = sampleMask(ruv);
    float depth1 = readDepthLod(ruv);
    if (depth1 > pc.maxDistance) {
        o_color = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec4 taps[cKernelSize];
    vec4 spread = vec4(0.0);
    for (int t = 0; t < cKernelSize; t++) {
        vec2 tapUV = clamp(ruv + (blurOffset(t) * pc.offsetMask) * 1.0, minUV, maxUV);
        taps[t] = sampleMask(tapUV);
        spread = max(spread, abs(taps[t] - center));
    }
    if (max(spread.r, spread.g) < 0.0001) {
        o_color = vec4(center.rgb, 1.0);
        return;
    }

    float weightSum = 0.114725602;
    vec4 color1 = center * weightSum;
    float depthDrop = 3500.0 * (depth1 / pc.cameraData.y);

    for (int i = 0; i < cKernelSize; i++) {
        vec2 tapUV = clamp(ruv + (blurOffset(i) * pc.offsetMask) * 1.0, minUV, maxUV);
        float depth2 = readDepthLod(tapUV);
        float diff = abs(depth1 - depth2);

        float useForBlur = diff <= depthDrop ? 1.0 : 0.0;
        color1 += blurWeights[i] * taps[i] * useForBlur;
        weightSum += blurWeights[i] * useForBlur;
    }
    color1 /= weightSum;
    o_color = vec4(color1.rgb, 1.0);
}
