// SunGlare as a Vulkan pass (needs NVR DXVK): Effects/SunGlare.fx.hlsl PS_SunGlare line for line.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "SunGlareCommon.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 1) uniform texture2D t_cover;      // TESR_SunGlareCover (POINT, CLAMP)

layout(push_constant) uniform Push {
    uint linearSampler;     // 0: LINEAR, CLAMP
    uint pointSampler;      // 4: POINT, CLAMP
    uint pad[2];            // 8..15
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

const int SG_MAX_LINES = 8;

vec3 getSunColor(float sunHeight, float atmosphere, float dayTime, vec3 sunCol, vec3 sunsetCol) {
    sunCol = linearize(sunCol);
    sunsetCol = linearize(sunsetCol);
    vec3 rgbAcc = (1.0 + sunHeight) * sunCol;
    float sunSet = saturate(pows(1.0 - sunHeight, 8.0)) * dayTime;
    rgbAcc += sunsetCol * sunSet * atmosphere;
    return rgbAcc;
}

void main() {
    // ExtFullScreen sets viewport to the active RenderRect when active, so vertex uv spans 0..1 across the screen
    // while texture samples need uv * cb.renderScale.xy.
    vec2 ruv = uv * cb.renderScale.xy;
    vec4 stored = textureLod(sampler2D(t_rendered, NVR_SAMPLER(pc.linearSampler)), ruv, 0.0);
    const float strength = cb.glareData.x;
    const float aspect = 1.0 / cb.reciprocalResolution.z;
    vec2 sun = sgSunScreen();
    vec2 d = (uv - sun) * vec2(aspect, 1.0);
    float r = length(d);

    float halo = cb.glareData.z * (exp(-r / cb.glareData.y) + 0.12 * exp(-r / (cb.glareData.y * 5.0)));
    float spikeReach = cb.glareSpikes.w * exp(-r * 0.7071 / cb.glareSpikes.y);
    float streak = cb.glareStreak.z * exp(-abs(d.y) / cb.glareStreak.y) / (1.0 + (d.x * d.x) / (cb.glareStreak.x * cb.glareStreak.x) * 9.0);
    if ((halo + spikeReach + streak) * strength < 0.004 && cb.glareData.w < 0.5) {
        o_color = vec4(stored.rgb, 1.0);
        return;
    }

    vec4 cover = textureLod(sampler2D(t_cover, NVR_SAMPLER(pc.pointSampler)), vec2(0.5, 0.5), 0.0);
    float visible = cover.x;

    float spikes = 0.0;
    vec2 axis = cb.glareTurn.xy;
    for (int k = 0; k < SG_MAX_LINES; k++) {
        if (float(k) >= cb.glareSpikes.x) break;
        float along = dot(d, axis);
        float across = abs(d.x * axis.y - d.y * axis.x);
        float pattern = fract(float(k) * 0.6180340 + (along > 0.0 ? 0.31 : 0.77));
        float len = cb.glareSpikes.y * (0.45 + 0.55 * pattern);
        float width = cb.glareSpikes.z * (1.0 + abs(along) / len);
        spikes += exp(-(across * across) / (width * width)) * exp(-abs(along) / len) / (1.0 + abs(along) / (len * 0.35));
        axis = vec2(axis.x * cb.glareTurn.z - axis.y * cb.glareTurn.w, axis.x * cb.glareTurn.w + axis.y * cb.glareTurn.z);
    }
    spikes *= cb.glareSpikes.w;

    vec3 spectrum = 1.0 + cb.glareStreak.w * cos(6.2831853 * (r / max(cb.glareSpikes.y, 1.0e-4) * 1.5 + vec3(0.0, 0.33, 0.67))) * saturate(r / cb.glareData.y);
    vec3 sunCol = getSunColor(saturate(cb.sunDirection.z), 1.0, cb.sunAmount.x, cb.sunColor.rgb, cb.sunsetColor.rgb);
    vec3 glare = sunCol * (halo + (spikes + streak) * spectrum) * (strength * visible);

    if (cb.glareData.w > 1.5) {
        o_color = vec4(delinearize(glare), 1.0);
        return;
    }
    if (cb.glareData.w > 0.5) {
        vec3 tint = mix(vec3(1.0, 0.2, 0.2), vec3(0.2, 1.0, 0.2), visible);
        o_color = vec4(stored.rgb * mix(vec3(1.0), tint, r < 0.25 ? 0.6 : 0.15) + delinearize(glare), 1.0);
        return;
    }
    o_color = vec4(delinearize(linearize(stored.rgb) + glare), 1.0);
}
