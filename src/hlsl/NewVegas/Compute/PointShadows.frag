// PointShadows as Vulkan passes (needs NVR DXVK): Effects/PointShadows.fx.hlsl, line for line. Three of its techniques, by push mode:
// 0 MergedPointShadows (Shadow(false)), 1 AddShadowedLamps (Shadow(true), added onto the buffer by blending), 2 LampMaskHalf
// (the half-resolution mask in R, the depth in G, at the top-left full-resolution texel of each half-resolution pixel).
// effects/PointShadows.cpp ExtRender runs it.
#version 450
#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_control_flow_attributes : enable
#include "PointShadowsCommon.glsl"

layout(set = 1, binding = 0) uniform textureCube t_cube0;     // TESR_ShadowCubeMapBuffer0 (LINEAR, CLAMP)
layout(set = 1, binding = 1) uniform textureCube t_cube1;
layout(set = 1, binding = 2) uniform textureCube t_cube2;
layout(set = 1, binding = 3) uniform textureCube t_cube3;
layout(set = 1, binding = 4) uniform textureCube t_cube4;
layout(set = 1, binding = 5) uniform textureCube t_cube5;
layout(set = 1, binding = 6) uniform textureCube t_cube6;
layout(set = 1, binding = 7) uniform textureCube t_cube7;
layout(set = 1, binding = 8) uniform textureCube t_cube8;
layout(set = 1, binding = 9) uniform textureCube t_cube9;
layout(set = 1, binding = 10) uniform textureCube t_cube10;   // TESR_ShadowCubeMapBuffer10
layout(set = 1, binding = 11) uniform texture2D t_depth;      // TESR_DepthBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 12) uniform texture2D t_normals;    // TESR_NormalsBuffer (POINT, CLAMP)

layout(push_constant) uniform Push {
    uint linearSampler;   // 0: LINEAR, CLAMP heap index (filled by the host)
    uint pointSampler;    // 4: POINT, CLAMP heap index
    uint mode;            // 8: 0 merged, 1 batch (AddShadowedLamps), 2 half-resolution mask (LampMaskHalf)
    uint pad;
} pc;

// UNOFFICIAL 2026-10-26: the variant .spv fixes this (-DPOINT_SHADOW_MODE, Compute/variants.txt, EffectRecord::ExtVariant), as the D3D9
// techniques are compiled one per mode; without it the push block decides (the general shader, the fallback).
#ifdef POINT_SHADOW_MODE
#define SHADOW_MODE uint(POINT_SHADOW_MODE)
#else
#define SHADOW_MODE pc.mode
#endif

#define LINEAR_CLAMP NVR_SAMPLER(pc.linearSampler)
#define POINT_CLAMP NVR_SAMPLER(pc.pointSampler)

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

void main() {
    // HalfMaskVS: the half-resolution pixel reads the full-resolution texel at its top-left
    vec2 minUV = 0.5 * cb.reciprocalResolution.xy;
    vec2 maxUV = max(cb.renderScale.xy - minUV, minUV);
    vec2 screenUV = uv + (SHADOW_MODE == 2u ? 0.5 * cb.reciprocalResolution.xy * cb.renderScale.zw : vec2(0.0));
    vec2 ruv = clamp(uv * cb.renderScale.xy + (SHADOW_MODE == 2u ? 0.5 * cb.reciprocalResolution.xy : vec2(0.0)), minUV, maxUV);
    bool batch = SHADOW_MODE == 1u;

    float depth = readDepthAt(t_depth, LINEAR_CLAMP, ruv);
    vec3 cameraVector = toWorld(screenUV) * depth;
    vec3 worldPos = cb.cameraPosition.xyz + cameraVector;
    vec3 normal = worldNormalAt(t_normals, POINT_CLAMP, ruv);

    // TEST variants (variants.txt PointShadows m0x/m0y/m0z/m0e, chosen live by NvrDxvkTestSuffix): parts left out to see what costs
#ifdef PS_TEST_EMPTY
    o_color = vec4(saturate(depth * 1e-6 + normal.x * 1e-6), 0.0, 0.0, 1.0);
    return;
#endif
#ifdef PS_TEST_NO_SHADOWED
    float shadow = 0.0;
#else
    float shadow = shadowedLight(t_cube0, LINEAR_CLAMP, worldPos, cb.shadowLightPosition[0], cb.shadowLightPre[0], normal);
    shadow += shadowedLight(t_cube1, LINEAR_CLAMP, worldPos, cb.shadowLightPosition[1], cb.shadowLightPre[1], normal);
    shadow += shadowedLight(t_cube2, LINEAR_CLAMP, worldPos, cb.shadowLightPosition[2], cb.shadowLightPre[2], normal);
    shadow += shadowedLight(t_cube3, LINEAR_CLAMP, worldPos, cb.shadowLightPosition[3], cb.shadowLightPre[3], normal);
    shadow += shadowedLight(t_cube4, LINEAR_CLAMP, worldPos, cb.shadowLightPosition[4], cb.shadowLightPre[4], normal);
    shadow += shadowedLight(t_cube5, LINEAR_CLAMP, worldPos, cb.shadowLightPosition[5], cb.shadowLightPre[5], normal);
    shadow += shadowedLight(t_cube6, LINEAR_CLAMP, worldPos, cb.shadowLightPosition[6], cb.shadowLightPre[6], normal);
    shadow += shadowedLight(t_cube7, LINEAR_CLAMP, worldPos, cb.shadowLightPosition[7], cb.shadowLightPre[7], normal);
    shadow += shadowedLight(t_cube8, LINEAR_CLAMP, worldPos, cb.shadowLightPosition[8], cb.shadowLightPre[8], normal);
    shadow += shadowedLight(t_cube9, LINEAR_CLAMP, worldPos, cb.shadowLightPosition[9], cb.shadowLightPre[9], normal);
    shadow += shadowedLight(t_cube10, LINEAR_CLAMP, worldPos, cb.shadowLightPosition[10], cb.shadowLightPre[10], normal);
#endif
#ifdef PS_TEST_NO_LAMPS
    batch = true;   // the lamps without shadows left out (the loops and both ifs test it)
#endif

    if (!batch && cb.shadowLightPosition[11].w != 0.0) {
        vec4 light11 = pointLightDistance(worldPos, cb.shadowLightPosition[11].xyz, cb.shadowLightPre[11].x);
        shadow += saturate(lampAmount(light11.xyz, light11.w, normal)) * edgeWindow(light11.w);
    }

    if (!batch && cb.spotLightPosition.w != 0.0)
        shadow += spotLightAmount(worldPos, cb.spotLightPosition, cb.spotLightDirection, normal) * cb.nearPre.w;

    // (fxc unrolls this loop: constant indices into the constants, a uniform branch per lamp)
    [[unroll]] for (int i = 0; i < 12; i++) {
        if (!batch && cb.lightPosition[i].w != 0.0) {
            vec4 light = pointLightDistance(worldPos, cb.lightPosition[i].xyz, cb.lightPre[i].x);
            if (light.w < 1.0 || cb.pointShadowNear.w < 0.5)
                shadow += saturate(lampAmount(light.xyz, light.w, normal) * cb.lightPre[i].y) * edgeWindow(light.w);
        }
    }

    for (int e = 0; e < 16 && !batch; e++) {
        if (cb.extraLights[e].w == 0.0 || shadow >= 1.0) break;
        vec4 light = pointLightDistance(worldPos, cb.extraLights[e].xyz, cb.extraPre[e].x);   // radius and weight unpacked once a draw
        if (light.w < 1.0 || cb.pointShadowNear.w < 0.5)
            shadow += saturate(lampAmount(light.xyz, light.w, normal) * cb.extraPre[e].y) * edgeWindow(light.w);
    }

    shadow = saturate(shadow);

    if (SHADOW_MODE == 2u)
        o_color = vec4(shadow, depth, 0.0, 1.0);	// LampMaskHalfPS
    else
        o_color = vec4(shadow, shadow, shadow, 1.0);
}
