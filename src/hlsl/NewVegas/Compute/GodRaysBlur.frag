// GodRays passes 2-4 (Blur1, Blur2, Blur3) as a Vulkan pass (needs NVR DXVK): Effects/GodRays.fx.hlsl GRBlur, line for line.
// effects/GodRays.cpp ExtDrawPass (pc.blurStep, pc.storeAttenuation).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "GodRays.glsl"

layout(set = 1, binding = 0) uniform texture2D t_buffer;     // NVR_GodRaysBuffer (LINEAR)
layout(set = 1, binding = 1) uniform texture2D t_max;        // NVR_GodRaysMax (POINT)
layout(set = 1, binding = 2) uniform texture2D t_depth;      // TESR_DepthBuffer (LINEAR)

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

bool GRAllEmpty() { return pc.skip.x > 0.5 && POINT_LOD(t_max, vec2(0.5)).r <= 0.0; }

float GRAttenuation(vec2 ruv) {
    float depth = LINEAR_LOD(t_depth, clampToRect(ruv, cb.reciprocalResolution.xy)).x;
    vec3 eye = normalize(GRPositionAt(ruv, depth));
    float timeF = cb.data.w != 0.0 ? mix(0.2, 4.0, pows(GRLow(), 4.0)) : 1.0;
    return pow(compress(shade(GRSunView(), eye)), 2.5) * timeF * (GRLow() < 1.0 ? 1.0 : 0.0);
}

void main() {
    if (GRAllEmpty()) { o_color = vec4(0.0, 0.0, 0.0, 1.0); return; }
    vec2 ruv = uv * rectScale();
    float aspect = 1.0 / cb.reciprocalResolution.z;
    vec2 toSun = (GRSunScreen() - uv) * vec2(1.0, aspect);
    float reach = length(toSun);
    vec2 dir = toSun / max(reach, 0.000001);
    float stepLen = pc.blurStep * cb.ray.y;
    vec3 sum = vec3(0.0);
    float count = 1.0;
    for (int i = 0; i < 10; i++) {
        float along = stepLen * float(i);
        vec2 tap = uv + dir * along / vec2(1.0, aspect);
        if (along <= reach && tap.x > 0.0 && tap.y > 0.0 && tap.x < 1.0 && tap.y < 1.0) {
            sum += LINEAR_LOD(t_buffer, clampToRect(screenToRect(tap), cb.grLayout.zw)).rgb;
            count += 1.0;
        }
    }
    o_color = vec4(sum / count, mix(1.0, GRAttenuation(ruv), pc.storeAttenuation));
}
