#version 450
#extension GL_GOOGLE_include_directive : enable
#include "LocalExposureCommon.glsl"

layout(set = 1, binding = 0) uniform texture2D t_buffer;

layout(push_constant) uniform Push {
    uint linearSampler;
    uint pad[3];
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

void main() {
    float sum = 0.0;
    for (int j = 0; j < 8; j++) {
        for (int i = 0; i < 8; i++) {
            vec2 sampleUv = ((vec2(float(i), float(j)) + 0.5) / 8.0) * cb.renderScale.xy;
            sum += textureLod(sampler2D(t_buffer, NVR_SAMPLER(pc.linearSampler)), sampleUv, 0.0).r;
        }
    }
    o_color = vec4(sum / 64.0, 0.0, 0.0, 1.0);
}
