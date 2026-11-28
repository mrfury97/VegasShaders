// HudRaindrops, technique 1 (KeepWetness) as a Vulkan pass (needs NVR DXVK).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_new;

layout(push_constant) uniform Push {
    uint pointSampler;
    uint pad[3];
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

void main() {
    o_color = textureLod(sampler2D(t_new, NVR_SAMPLER(pc.pointSampler)), vec2(0.5, 0.5), 0.0);
}
