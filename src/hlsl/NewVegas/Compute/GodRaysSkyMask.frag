// GodRays pass 0 (SkyMask) as a Vulkan pass (needs NVR DXVK): Effects/GodRays.fx.hlsl PS_SkyMask, line for line.
// Into NVR_GodRaysBuffer0 (half/quarter resolution). effects/GodRays.cpp ExtDrawPass.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "GodRays.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer (LINEAR)
layout(set = 1, binding = 1) uniform texture2D t_depth;      // TESR_DepthBuffer (LINEAR)

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

void main() {
    vec2 ruv = clampToRect(uv * rectScale(), cb.reciprocalResolution.xy);
    float depthRaw = LINEAR_LOD(t_depth, ruv).x;
    float sky = depthRaw > 0.9 ? 1.0 : 0.0;
    float horizon = smoothstep(0.0, 0.01, GRLow());
    if (sky * horizon == 0.0) { o_color = vec4(0.0, 0.0, 0.0, 1.0); return; }

    float sunset = GRSunset();
    float fix = step(0.5, cb.ray.w);
    float boostW = mix(1.0 - sunset, sunset, fix);
    vec3 glareCol = linearize(cb.sunColor.rgb) + linearize(cb.sunsetColor.rgb) * boostW;
    float glarePow = mix(0.1, 8.0, sunset);
    float glare = pows(dot(GRSunView(), normalize(GRPositionAt(ruv, depthRaw))), 180.0) * glarePow;
    vec3 scene = linearize(LINEAR_LOD(t_rendered, ruv).rgb);
    o_color = vec4((scene + glare * glareCol) * sky * horizon, 1.0);
}
