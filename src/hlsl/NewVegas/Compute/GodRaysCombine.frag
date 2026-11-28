// GodRays pass 5 (Combine) as a Vulkan pass (needs NVR DXVK): Effects/GodRays.fx.hlsl PS_Combine, line for line.
// Through the frame chain (ExtFullScreen). effects/GodRays.cpp ExtDrawPass.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "GodRays.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer (LINEAR)
layout(set = 1, binding = 1) uniform texture2D t_buffer;     // NVR_GodRaysBuffer0 (LINEAR)

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

void main() {
    vec2 scale = rectScale();
    vec4 stored = LINEAR_LOD(t_rendered, clampToRect(uv * scale, cb.reciprocalResolution.xy));
    vec4 r = LINEAR_LOD(t_buffer, clampToRect(uv * scale, cb.grLayout.zw));
    if (pc.skip.y > 0.5 && !any(notEqual(r.rgb, vec3(0.0)))) { o_color = vec4(stored.rgb, 1.0); return; }
    vec3 sunCol = GetSunColor(shade(cb.sunDirection.xyz, vec3(0.0, 0.0, 1.0)), 1.0, cb.sunAmount.x, cb.sunColor.rgb, cb.sunsetColor.rgb);
    vec3 rayCol = mix(sunCol, linearize(cb.rayColor.rgb), cb.rayColor.w);
    vec4 rays = r;
    rays.rgb *= cb.data.z * rayCol;
    rays.rgb *= rays.a;
    vec4 lin = linearize(stored);
    lin += max(rays, vec4(0.0)) * 5.0 * lin + max(rays, vec4(0.0)) * 0.2;
    o_color = vec4(delinearize(lin).rgb, 1.0);
}
