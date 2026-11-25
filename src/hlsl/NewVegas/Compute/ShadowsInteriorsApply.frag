// ShadowsInteriors' dedicated last pass (CombineShadowDedicated) as a Vulkan pass (needs NVR DXVK): Effects/ShadowsInteriors.fx.hlsl
// with Includes/InteriorShadowApply.hlsl, Helpers.hlsl and Depth.hlsl, line for line. effects/ShadowsInteriors.cpp ExtRender.
// Matrices are copied as stored: m[i][j] here is the HLSL's m[i][j] (the D3DX element (i, j)), as in SkinScattering.frag.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer: the scene (LINEAR, CLAMP)
layout(set = 1, binding = 1) uniform texture2D t_mask;       // NVR_InteriorShadowIn: the blurred shadow term (LINEAR, CLAMP)
layout(set = 1, binding = 2) uniform texture2D t_depth;      // TESR_DepthBuffer (LINEAR, CLAMP)

layout(push_constant) uniform Push {
    vec4 cameraData;            // 0: TESR_CameraData (y: farZ)
    vec4 shadowData;            // 16: TESR_ShadowData (y: darkness)
    vec4 shadowFade;            // 32: TESR_ShadowFade (y: 0 when shadows are off, w: MAXDISTANCE)
    mat4 viewTransform;         // 48: TESR_ViewTransform
    mat4 projectionTransform;   // 112: TESR_ProjectionTransform
    uint samplerIndex;          // 176: LINEAR, CLAMP
    float scaleX;               // 180: RenderRect::ScaleX (1 = off)
    float scaleY;               // 184: RenderRect::ScaleY (1 = off)
    uint pad;                   // 188
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

// Includes/Depth.hlsl toWorld
vec3 toWorld(vec2 tex) {
    mat4 V = pc.viewTransform;
    mat4 P = pc.projectionTransform;
    vec3 v = vec3(V[0][2], V[1][2], V[2][2]);
    v += (1.0 / P[0][0] * (2.0 * tex.x - 1.0)) * vec3(V[0][0], V[1][0], V[2][0]);
    v += (-1.0 / P[1][1] * (2.0 * tex.y - 1.0)) * vec3(V[0][1], V[1][1], V[2][1]);
    return v;
}

// Includes/Depth.hlsl readDepth: the linear depth in game units
float readDepth(vec2 coord) {
    return texture(sampler2D(t_depth, NVR_SAMPLER(pc.samplerIndex)), coord).x * pc.cameraData.y;
}

// Helpers.hlsl invlerps
float invlerps(float a, float b, float t) {
    return clamp((t - a) / (b - a), 0.0, 1.0);
}

// Includes/InteriorShadowApply.hlsl ApplyInteriorShadowDepth (INTERIOR_SHADOW_MAXDISTANCE = TESR_ShadowFade.w)
vec4 applyInteriorShadowDepth(vec2 coord, vec4 color, float shadowSample, float depth) {
    color.rgb = pows(color.rgb, 2.2); // linearise
    vec3 eyeDir = toWorld(coord);
    float uniformDepth = length(depth * eyeDir);

    float Shadow = clamp(shadowSample, 0.0, 1.0);
    Shadow = mix(Shadow, 1.0, invlerps(300.0, pc.shadowFade.w, uniformDepth)); // fade shadows with distance
    float darkness = (pc.shadowData.y * 5.0) / (1.0 + pc.shadowData.y * 5.0);  // scaledReinhard(TESR_ShadowData.y, 5.0)
    darkness = clamp(1.0 - darkness * pc.shadowFade.y, 0.0, 1.0);             // shadowFade.y is set to 0 when shadows are disabled

    Shadow = clamp(mix(darkness, 1.0, Shadow), 0.0, 1.0); // shadow darkness to apply in the scene - 0 is full shadow 1 is full light
    float shadowPower = 1.0 - Shadow;                     // 1 is full shadow, useful for scaling effect

    // apply shadows to darkest values (< 1)
    vec4 finalColor = clamp(Shadow * pow(clamp(color, 0.0, 1.0), vec4(1.0 - shadowPower * 0.2)), 0.0, 1.0); // boost the gamma of the base image
    // add some saturation back to the darker parts of the image: lerp(luma(finalColor), finalColor, ...)
    finalColor = mix(vec4(luma(finalColor)), finalColor, 1.0 + shadowPower * 0.3);

    // readd values above 1
    finalColor += max(color - 1.0, 0.0);
    finalColor.rgb = pows(finalColor.rgb, 1.0 / 2.2); // delinearise

    return vec4(finalColor.rgb, 1.0);
}

// ShadowsInteriors.fx.hlsl CombineShadowDedicated
void main() {
    vec2 ruv = uv * vec2(pc.scaleX, pc.scaleY);
    vec4 rendered = texture(sampler2D(t_rendered, NVR_SAMPLER(pc.samplerIndex)), ruv);
    float mask = texture(sampler2D(t_mask, NVR_SAMPLER(pc.samplerIndex)), ruv).r;
    o_color = applyInteriorShadowDepth(uv, rendered, mask, readDepth(ruv));
}
