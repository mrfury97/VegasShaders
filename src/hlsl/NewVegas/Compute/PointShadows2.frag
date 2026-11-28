// PointShadows2 (the lamp shadows past the first six, screen space) as a Vulkan pass (needs NVR DXVK): Effects/PointShadows2.fx.hlsl
// technique 0, line for line, with the Includes it uses (Shadows, Depth, Normals, Helpers). effects/PointShadows2.cpp ExtRender.
// TESR_PointShadowBuffer is read from a copy of the buffer as it was before this pass (the pass writes the buffer itself).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

#define BIAS 0.018   // Includes/Shadows.hlsl

layout(set = 1, binding = 0) uniform texture2D t_depth;             // TESR_DepthBuffer (LINEAR, CLAMP; linear depth / farZ)
layout(set = 1, binding = 1) uniform texture2D t_normals;           // TESR_NormalsBuffer (POINT, CLAMP)
layout(set = 1, binding = 2) uniform texture2D t_pointShadow;       // TESR_PointShadowBuffer, the copy (LINEAR, CLAMP; G16R16)
layout(set = 1, binding = 3) uniform textureCube t_cube6;           // TESR_ShadowCubeMapBuffer6 (LINEAR, CLAMP)
layout(set = 1, binding = 4) uniform textureCube t_cube7;           // TESR_ShadowCubeMapBuffer7
layout(set = 1, binding = 5) uniform textureCube t_cube8;           // TESR_ShadowCubeMapBuffer8
layout(set = 1, binding = 6) uniform textureCube t_cube9;           // TESR_ShadowCubeMapBuffer9
layout(set = 1, binding = 7) uniform textureCube t_cube10;          // TESR_ShadowCubeMapBuffer10

// The constants the HLSL reads, filled by the host each frame (the light arrays are too big for push data). Matrices as the
// effect's SetMatrix uploads them: cb.viewTransform[i][j] is the HLSL's [i][j]; mul(v, M) is M * v, mul(M, v) is v * M.
layout(set = 2, binding = 0, std140) uniform Constants {
    vec4 cameraPosition;               // TESR_CameraPosition
    vec4 cameraData;                   // TESR_CameraData: y far
    mat4 viewTransform;                // TESR_ViewTransform
    mat4 projectionTransform;          // TESR_ProjectionTransform
    vec4 shadowFade;                   // TESR_ShadowFade: z 0 turns the cube maps off
    vec4 shadowLightPosition[12];      // TESR_ShadowLightPosition: xyz position, w radius
    vec4 lightColor[24];               // TESR_LightColor: rgb colour, w dimmer
    vec4 renderScale;                  // TESR_RenderScale (xy scale, zw 1/scale)
} cb;

layout(push_constant) uniform Push {
    uint samplerIndex;                 // 0: LINEAR, CLAMP (filled in by the host)
    uint pointSamplerIndex;            // 4: POINT, CLAMP (filled in by the host)
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

// Includes/Depth.hlsl readDepth
float readDepth(vec2 coord) {
    return texture(sampler2D(t_depth, NVR_SAMPLER(pc.samplerIndex)), coord).x * cb.cameraData.y;
}

// Includes/Depth.hlsl toWorld
vec3 toWorld(vec2 tex) {
    vec3 v = vec3(cb.viewTransform[0][2], cb.viewTransform[1][2], cb.viewTransform[2][2]);
    v += (1.0 / cb.projectionTransform[0][0] * (2.0 * tex.x - 1.0)) * vec3(cb.viewTransform[0][0], cb.viewTransform[1][0], cb.viewTransform[2][0]);
    v += (-1.0 / cb.projectionTransform[1][1] * (2.0 * tex.y - 1.0)) * vec3(cb.viewTransform[0][1], cb.viewTransform[1][1], cb.viewTransform[2][1]);
    return v;
}

// Includes/Normals.hlsl GetWorldNormal: mul(TESR_ViewTransform, normal).xyz
vec3 getWorldNormal(vec2 coord) {
    vec4 normal = vec4(texture(sampler2D(t_normals, NVR_SAMPLER(pc.pointSamplerIndex)), coord).xyz * 2.0 - 1.0, 1.0);
    return (normal * cb.viewTransform).xyz;
}

// Includes/Shadows.hlsl GetPointLightAmountValue
float GetPointLightAmountValue(textureCube cube, uint samplerIndex, vec3 lightDir, float dist) {
    if (cb.shadowFade.z == 0.0) return 1.0;

    float lightDepth = texture(samplerCube(cube, NVR_SAMPLER(samplerIndex)), lightDir).r;
    float shadow = lightDepth + BIAS * dist > dist ? 1.0 : 0.0; // increase BIAS with distance

    // ignore shadow sample if lightDepth is not within range of cube map
    return mix(1.0, shadow, (lightDepth > 0.0 && lightDepth < 1.0) ? 1.0 : 0.0);
}

// Includes/Shadows.hlsl GetPointLightAtten
float GetPointLightAtten(vec3 lightDir, float dist, vec4 normal) {
    // radius based attenuation based on https://lisyarus.github.io/blog/graphics/2022/07/30/point-light-attenuation.html
    float s = saturate(dist * dist);
    float atten = saturate(((1.0 - s) * (1.0 - s)) / (1.0 + 5.0 * s));

    lightDir = normalize(lightDir);
    float diffuse = mix(1.0, dot(lightDir, normal.xyz), smoothstep(0.0, 0.2, dist)); // add some light bleeding at very short distance

    return saturate(diffuse * atten);
}

// Includes/Shadows.hlsl GetPointLightAmount
float GetPointLightAmount(textureCube cube, vec4 worldPos, vec4 lightPos, vec4 normal) {
    if (lightPos.w == 0.0) return 0.0; // w is light radius.

    vec3 lightDir = lightPos.xyz - worldPos.xyz;
    vec3 lightUV = lightDir * vec3(-1.0, -1.0, 1.0);

    float dist = length(lightDir) / lightPos.w; // normalize distance over light range

    float lightAmount = GetPointLightAmountValue(cube, pc.samplerIndex, lightUV, dist) * GetPointLightAtten(lightDir, dist, normal);
    return saturate(lightAmount);
}

// Includes/Shadows.hlsl GetPointLightContribution (no shadow map)
float GetPointLightContribution(vec4 worldPos, vec4 lightPos, vec4 normal) {
    vec3 lightDir = lightPos.xyz - worldPos.xyz;
    float dist = length(lightDir) / lightPos.w;
    return GetPointLightAtten(lightDir, dist, normal);
}

void main() {
    vec2 ruv = clamp(uv * cb.renderScale.xy, vec2(0.0), cb.renderScale.xy);
    float depth = readDepth(ruv);
    vec3 cameraVector = toWorld(uv) * depth;
    vec4 worldPos = vec4(cb.cameraPosition.xyz + cameraVector, 1.0);
    vec4 normal = vec4(getWorldNormal(ruv), 1.0);

    float shadow = texture(sampler2D(t_pointShadow, NVR_SAMPLER(pc.samplerIndex)), ruv).r;
    shadow += GetPointLightAmount(t_cube6, worldPos, cb.shadowLightPosition[6], normal) * luma(cb.lightColor[6].rgb) * cb.lightColor[6].w;
    shadow += GetPointLightAmount(t_cube7, worldPos, cb.shadowLightPosition[7], normal) * luma(cb.lightColor[7].rgb) * cb.lightColor[7].w;
    shadow += GetPointLightAmount(t_cube8, worldPos, cb.shadowLightPosition[8], normal) * luma(cb.lightColor[8].rgb) * cb.lightColor[8].w;
    shadow += GetPointLightAmount(t_cube9, worldPos, cb.shadowLightPosition[9], normal) * luma(cb.lightColor[9].rgb) * cb.lightColor[9].w;
    shadow += GetPointLightAmount(t_cube10, worldPos, cb.shadowLightPosition[10], normal) * luma(cb.lightColor[10].rgb) * cb.lightColor[10].w;
    // (HLSL: TESR_ShadowCubeMapBuffer11 is commented out in the .fx)
    shadow += GetPointLightContribution(worldPos, cb.shadowLightPosition[11], normal);

    shadow = saturate(shadow);
    o_color = vec4(shadow, shadow, shadow, 1.0);
}
