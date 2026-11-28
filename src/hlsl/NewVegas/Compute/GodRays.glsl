// GodRays (needs NVR DXVK): shared constants and helpers for DedicatedGodRays (passes 0-5) and GodRaysMaxReduce,
// matching Effects/GodRays.fx.hlsl line for line. effects/GodRays.cpp (ExtPrepare, ExtDrawPass, RunReduce) fills the
// uniform buffer and push block. Matrices arrive as stored (SetMatrix): cb.M * v = HLSL mul(v, M).
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 2, binding = 0, std140) uniform Constants {
    mat4 projectionTransform;    // TESR_ProjectionTransform
    mat4 invProjectionTransform; // TESR_InvProjectionTransform
    vec4 reciprocalResolution;   // TESR_ReciprocalResolution
    vec4 sunColor;               // TESR_SunColor
    vec4 ray;                    // TESR_GodRaysRay (x RayIntensity, y RayLength, w SunsetGlareFix)
    vec4 rayColor;               // TESR_GodRaysRayColor (rgb custom color, w Saturate)
    vec4 data;                   // TESR_GodRaysData (y Luminance threshold, z day/night strength, w TimeEnabled)
    vec4 viewSpaceLightDir;      // TESR_ViewSpaceLightDir
    vec4 sunDirection;           // TESR_SunDirection
    vec4 sunPosition;            // TESR_SunPosition
    vec4 sunAmount;              // TESR_SunAmount
    vec4 sunsetColor;            // TESR_SunsetColor
    vec4 cameraData;             // TESR_CameraData (x nearZ, y farZ)
    vec4 grLayout;               // NVR_GodRaysLayout (xy ray buffer / full extent, zw 1 / ray buffer size)
    vec4 renderScale;            // xy: RenderRect scale (1 = off), zw: 1 / xy
} cb;

layout(push_constant) uniform Push {
    vec4 skip;                   // 0: NVR_GodRaysSkip (x SkipEmpty reduction ran, y SkipIdle on)
    vec4 reduce;                 // 16: NVR_GodRaysReduce (xy 1 / source size of reduction step)
    float blurStep;              // 32: RadialBlur's step
    float storeAttenuation;      // 36: RadialBlur's storeAttenuation (1 on Blur3, else 0)
    uint linearSampler;          // 40: LINEAR, CLAMP
    uint pointSampler;           // 44: POINT, CLAMP
} pc;

#define LINEAR_LOD(t, coord) textureLod(sampler2D(t, NVR_SAMPLER(pc.linearSampler)), (coord), 0.0)
#define POINT_LOD(t, coord)  textureLod(sampler2D(t, NVR_SAMPLER(pc.pointSampler)), (coord), 0.0)

#define compress(x) ((x) * 0.5 + 0.5)
#define shade(nrm, lgt) max(dot(nrm, lgt), 0.0)
#define nearZ (cb.cameraData.x)
#define farZ  (cb.cameraData.y)

vec2 rectScale() { return (cb.renderScale.x > 0.0 && cb.renderScale.y > 0.0) ? cb.renderScale.xy : vec2(1.0); }
vec2 invRectScale() { return (cb.renderScale.z > 0.0 && cb.renderScale.w > 0.0) ? cb.renderScale.zw : (1.0 / rectScale()); }
vec2 rectToScreen(vec2 ruv) { return ruv * invRectScale(); }
vec2 screenToRect(vec2 s01) { return s01 * rectScale(); }
vec2 clampToRect(vec2 ruv, vec2 texel) {
    vec2 lo = 0.5 * texel;
    return clamp(ruv, lo, max(rectScale() - lo, lo));
}

float projectedDepthFromLinear(float linear01) {
    float viewZ = max(linear01 * farZ, 1.0e-6);
    return nearZ * (farZ - viewZ) / (viewZ * (farZ - nearZ));
}

vec3 projectPosition(vec3 posn) {
    vec4 projection = cb.projectionTransform * vec4(posn, 1.0);
    projection.xyz /= projection.w;
    projection.x = projection.x * 0.5 + 0.5;
    projection.y = 0.5 - 0.5 * projection.y;
    projection.xy = screenToRect(projection.xy);
    return projection.xyz;
}

float GRLow() { return 1.0 - max(cb.sunPosition.z, 0.0); }
float GRSunset() { return pows(GRLow(), 8.0); }
vec3 GRSunView() { return cb.viewSpaceLightDir.xyz; }

vec3 GRPositionAt(vec2 ruv, float linearDepth) {
    vec2 s = rectToScreen(ruv);
    vec4 v = cb.invProjectionTransform * vec4(s.x * 2.0 - 1.0, (1.0 - s.y) * 2.0 - 1.0, projectedDepthFromLinear(linearDepth), 1.0);
    return v.xyz / v.w;
}

vec2 GRSunScreen() { return rectToScreen(projectPosition(GRSunView() * farZ).xy); }

vec3 GetSunColor(float sun_height, float athmosphere_v, float day_time, vec3 sun_color, vec3 sunset_color) {
    sun_color = linearize(sun_color);
    sunset_color = linearize(sunset_color);
    vec3 rgb_acc = (1.0 + sun_height) * sun_color;
    float sun_set = saturate(pows(1.0 - sun_height, 8.0)) * day_time;
    rgb_acc += sunset_color * sun_set * athmosphere_v;
    return rgb_acc;
}
