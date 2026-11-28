// HudRaindrops, technique 2 (ScreenDrops) as a Vulkan pass (needs NVR DXVK).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_state;
layout(set = 1, binding = 1) uniform texture2D t_screenCopy;
layout(set = 1, binding = 2) uniform texture2D t_dropTex;

layout(push_constant) uniform Push {
    vec4 screen;        // 0: TESR_HudRainScreen (x threshold, y clock, z refraction, w drop tiles across)
    uint pointSampler;  // 16
    uint linearClamp;   // 20
    uint mipWrap;       // 24
    uint pad;           // 28
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

vec3 screenDropLayer(vec4 texel, float speed, float threshold) {
    vec2 slope = texel.gb * 2.0 - 1.0;
    float life = 1.0 - fract(texel.a + pc.screen.y * speed);
    float strength = saturate(life * texel.r - threshold);
    return vec3(slope * strength, strength);
}

void main() {
    float wet = textureLod(sampler2D(t_state, NVR_SAMPLER(pc.pointSampler)), vec2(0.5, 0.5), 0.0).r;
    vec2 aspect = vec2(1.0, 0.75);
    vec2 p = uv * aspect * pc.screen.w;
    float threshold = mix(1.0, pc.screen.x, saturate(wet * 1.5));
    vec3 drops = screenDropLayer(texture(sampler2D(t_dropTex, NVR_SAMPLER(pc.mipWrap)), p), 0.2, threshold)
               + screenDropLayer(texture(sampler2D(t_dropTex, NVR_SAMPLER(pc.mipWrap)), (p + vec2(0.23, 0.46)) * 1.87), 0.1, threshold);
    drops *= saturate(wet * 2.0);
    drops.xy = clamp(drops.xy, -1.0, 1.0);

    vec2 shift = drops.xy * pc.screen.z / (pc.screen.w * aspect);
    vec4 color = texture(sampler2D(t_screenCopy, NVR_SAMPLER(pc.linearClamp)), uv + shift);

    vec3 N = normalize(vec3(drops.xy * 2.5, 1.0));
    float glint = pow(saturate(dot(N, vec3(-0.37, 0.46, 0.81))), 60.0) * saturate(drops.z * 2.0);
    color.rgb = color.rgb * (1.0 - 0.35 * saturate(length(drops.xy))) + glint * 0.5;
    o_color = color;
}
