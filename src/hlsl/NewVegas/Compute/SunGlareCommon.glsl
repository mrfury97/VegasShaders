// SunGlare (needs NVR DXVK): shared constants block and helpers for SunGlare.frag and SunGlareCover.frag,
// matching Effects/SunGlare.fx.hlsl line for line.
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 2, binding = 0, std140) uniform Constants {
    mat4 projectionTransform;       // TESR_ProjectionTransform
    vec4 reciprocalResolution;      // TESR_ReciprocalResolution
    vec4 sunColor;                  // TESR_SunColor
    vec4 sunsetColor;               // TESR_SunsetColor
    vec4 sunDirection;              // TESR_SunDirection
    vec4 sunAmount;                 // TESR_SunAmount
    vec4 viewSpaceLightDir;         // TESR_ViewSpaceLightDir
    vec4 cameraData;                // TESR_CameraData
    vec4 renderScale;               // TESR_RenderScale
    vec4 glareData;                 // TESR_SunGlareData
    vec4 glareSpikes;               // TESR_SunGlareSpikes
    vec4 glareStreak;               // TESR_SunGlareStreak
    vec4 glareSun;                  // TESR_SunGlareSun
    vec4 glareTurn;                 // TESR_SunGlareTurn
} cb;

// Includes/Depth.hlsl projectPosition returned in screen 0..1 coordinates (screenToRect omitted because
// rectToScreen(screenToRect(p)) == p). HLSL mul(v, M) is cb.M * v.
vec2 sgSunScreen() {
    vec3 pos = normalize(cb.viewSpaceLightDir.xyz) * cb.cameraData.g;
    vec4 proj = cb.projectionTransform * vec4(pos, 1.0);
    vec2 ndc = proj.xy / proj.w;
    return vec2(ndc.x * 0.5 + 0.5, 0.5 - 0.5 * ndc.y);
}
