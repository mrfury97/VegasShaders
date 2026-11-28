// SunShadows' sharp contact shadows (technique ForwardContactSharp, second pass) as a Vulkan pass: Effects/SunShadows.fx.hlsl
// SharpContactFinish line for line.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "SunShadows.glsl"

void main() {
    vec2 ruv = toRectUV(uv);
    vec4 center = sampleShadow(ruv);
    float depth = readDepthLod(ruv);
    if (depth > SSS_MAXDEPTH) { o_color = center; return; }
    float sum = center.r * 2.0, weights = 2.0;
    float tolerance = max(depth * 0.02, 1.0);
    const vec2 offsets[4] = vec2[4](vec2(1.0, 0.0), vec2(-1.0, 0.0), vec2(0.0, 1.0), vec2(0.0, -1.0));
    for (int i = 0; i < 4; i++) {
        vec2 tapUV = clampRectUV(ruv + offsets[i] * cb.reciprocalResolution.xy);
        float w = abs(readDepthLod(tapUV) - depth) < tolerance ? 1.0 : 0.0;
        sum += sampleShadow(tapUV).r * w;
        weights += w;
    }
    center.r = pow(sum / weights, cb.screenSpaceData.w);
    o_color = center;
}
