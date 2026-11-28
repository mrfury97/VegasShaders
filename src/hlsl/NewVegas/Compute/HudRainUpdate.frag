// HudRaindrops, technique 0 (UpdateWetness) as a Vulkan pass (needs NVR DXVK).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_ortho;
layout(set = 1, binding = 1) uniform texture2D t_state;

layout(set = 2, binding = 0, std140) uniform Constants {
    mat4 shadowOrtho;   // TESR_ShadowCameraToLightTransformOrtho
    vec4 data;          // TESR_HudRainData
    vec4 time;          // TESR_HudRainTime
    vec4 shade;         // TESR_HudRainShade
    vec4 debug;         // TESR_HudRainDebug
} cb;

layout(push_constant) uniform Push {
    uint pointSampler;
    uint pad[3];
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

const float COVER_HEIGHT = 48.0;
const float COVER_THICKNESS = 0.003;

void main() {
    float wet = textureLod(sampler2D(t_state, NVR_SAMPLER(pc.pointSampler)), vec2(0.5, 0.5), 0.0).r;

    float covered = cb.shade.z;
    if (covered < 0.5 && cb.data.x > 0.0) {
        vec4 o = cb.shadowOrtho * vec4(0.0, 0.0, COVER_HEIGHT, 1.0);
        o.xyz /= o.w;
        vec2 ouv = vec2(o.x * 0.5 + 0.5, o.y * -0.5 + 0.5);
        float top = textureLod(sampler2D(t_ortho, NVR_SAMPLER(pc.pointSampler)), ouv, 0.0).r;
        bool inside = all(greaterThan(ouv, vec2(0.0))) && all(lessThan(ouv, vec2(1.0)));
        covered = (inside && o.z > top + COVER_THICKNESS) ? 1.0 : 0.0;
    }

    float exposure = cb.data.x * (1.0 - covered);
    float dt = cb.time.y;
    wet = exposure > 0.0 ? min(wet + dt * exposure / cb.time.z, 1.0) : max(wet - dt / cb.time.z, 0.0);
    if (cb.shade.w > 0.5) wet = 0.0;
    if (cb.debug.x > 0.5) wet = 1.0;
    o_color = vec4(wet, covered, 0.0, 1.0);
}
