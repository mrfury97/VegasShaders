#version 450
#extension GL_GOOGLE_include_directive : enable
#include "LocalExposureCommon.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;

layout(push_constant) uniform Push {
    uint linearSampler;
    uint pad[3];
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

void main() {
    vec2 ruv = uv * cb.renderScale.xy;
    float sum = 0.0;
    for (int j = 0; j < 4; j++) {
        for (int i = 0; i < 4; i++) {
            vec2 sampleUv = clampToRectIfOn(clamp(ruv + (vec2(float(i), float(j)) - 1.5) * 4.0 * cb.size.zw, vec2(0.0), vec2(1.0)), cb.size.zw);
            vec3 c = textureLod(sampler2D(t_rendered, NVR_SAMPLER(pc.linearSampler)), sampleUv, 0.0).rgb;
            float y = luma(pows(max(c, vec3(0.0)), cb.toneMapping.w));
            y = (abs(y) < 65504.0 && !isnan(y)) ? y : 0.0;
            sum += log2(max(y, 1.0e-4));
        }
    }
    o_color = vec4(sum / 16.0, 0.0, 0.0, 1.0);
}
