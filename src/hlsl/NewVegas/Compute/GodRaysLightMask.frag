// GodRays pass 1 (LightMask) as a Vulkan pass (needs NVR DXVK): Effects/GodRays.fx.hlsl PS_LightMask, line for line.
// Into NVR_GodRaysBuffer1 (half/quarter resolution). effects/GodRays.cpp ExtDrawPass.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "GodRays.glsl"

layout(set = 1, binding = 0) uniform texture2D t_buffer;     // NVR_GodRaysBuffer0 (LINEAR)

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

void main() {
    vec2 t = cb.grLayout.zw;
    vec2 ruv = clampToRect(uv * rectScale(), t);
    vec3 avg = (LINEAR_LOD(t_buffer, ruv).rgb
              + LINEAR_LOD(t_buffer, clampToRect(ruv + vec2(-t.x, -t.y), t)).rgb
              + LINEAR_LOD(t_buffer, clampToRect(ruv + vec2(-t.x,  t.y), t)).rgb
              + LINEAR_LOD(t_buffer, clampToRect(ruv + vec2( t.x, -t.y), t)).rgb
              + LINEAR_LOD(t_buffer, clampToRect(ruv + vec2( t.x,  t.y), t)).rgb) / 5.0;
    float thr = mix(2.0, 0.0, pow(abs(GRLow()), 8.0));
    float ramp = smoothstep(thr, thr + cb.data.y * 15.0, luma(avg));
    o_color = vec4(saturate(ramp * avg * 100.0 * cb.ray.x), 1.0);
}
