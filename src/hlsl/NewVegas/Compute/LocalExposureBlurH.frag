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
    o_color = vec4(blurAlong(t_buffer, pc.linearSampler, uv * cb.renderScale.xy, vec2(1.0, 0.0)), 0.0, 0.0, 1.0);
}
