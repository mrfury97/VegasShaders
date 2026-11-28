// ShadowsExteriors (the sun shadow apply) as a Vulkan pass (needs NVR DXVK): Effects/ShadowsExteriors.fx.hlsl, technique Shadow,
// line for line. effects/ShadowsExterior.cpp ExtRender.
// The matrix is TESR_ViewTransform as the HLSL reads it (ID3DXEffect SetMatrix: M[i][j] is the logical element i, j): the push
// block's viewColumn[j] is the HLSL column j (M[0..3][j]), so the GLSL mat4 built from them has viewT[j][i] = M[i][j], and
// HLSL mul(M, v) is viewT * v, HLSL M[i][j] is viewT[j][i].
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;      // TESR_RenderedBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 1) uniform texture2D t_sunFar;        // TESR_SunFarBuffer (POINT, CLAMP, LOD 0)
layout(set = 1, binding = 2) uniform texture2D t_pointShadow;   // TESR_PointShadowBuffer (LINEAR, CLAMP; G16R16)
layout(set = 1, binding = 3) uniform texture2D t_depth;         // TESR_DepthBuffer (R32F, linear depth / farZ; LINEAR, LOD 0)
layout(set = 1, binding = 4) uniform texture2D t_normals;       // TESR_NormalsBuffer (LINEAR, LOD 0)

layout(push_constant) uniform Push {
    vec4 viewColumn0;       // 0: TESR_ViewTransform, HLSL column 0
    vec4 viewColumn1;       // 16: column 1
    vec4 viewColumn2;       // 32: column 2
    vec4 viewColumn3;       // 48: column 3
    vec4 cameraPosition;    // 64: TESR_CameraPosition
    vec4 projectionFar;     // 80: x TESR_ProjectionTransform[0][0], y [1][1], z TESR_CameraData.y (farZ)
    vec4 shadowFade;        // 96: TESR_ShadowFade
    vec4 shadowData;        // 112: TESR_ShadowData
    vec4 waterSettings;     // 128: TESR_WaterSettings
    vec4 sunFarApply;       // 144: TESR_SunFarApply
    vec4 sunAmbient;        // 160: TESR_SunAmbient
    vec4 skyColor;          // 176: TESR_SkyColor
    uint samplerIndex;      // 192: LINEAR, CLAMP
    uint pointSamplerIndex; // 196: POINT, CLAMP
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

mat4 viewT() { return mat4(pc.viewColumn0, pc.viewColumn1, pc.viewColumn2, pc.viewColumn3); }

// Depth.hlsl toWorld
vec3 toWorld(vec2 tex) {
    vec3 v = pc.viewColumn2.xyz;
    v += (1.0 / pc.projectionFar.x * (2.0 * tex.x - 1.0)) * pc.viewColumn0.xyz;
    v += (-1.0 / pc.projectionFar.y * (2.0 * tex.y - 1.0)) * pc.viewColumn1.xyz;
    return v;
}

// Depth.hlsl readDepthLod
float readDepthLod(vec2 coord) {
    return textureLod(sampler2D(t_depth, NVR_SAMPLER(pc.samplerIndex)), coord, 0.0).x * pc.projectionFar.z;
}

// Normals.hlsl GetWorldNormalLod: mul(TESR_ViewTransform, normal).xyz
vec3 getWorldNormalLod(vec2 coord) {
    vec4 normal = vec4(textureLod(sampler2D(t_normals, NVR_SAMPLER(pc.samplerIndex)), coord, 0.0).xyz * 2.0 - 1.0, 1.0);
    return (viewT() * normal).xyz;
}

void main() {
    vec2 uvc = gl_FragCoord.xy / vec2(textureSize(sampler2D(t_rendered, NVR_SAMPLER(pc.samplerIndex)), 0));
    vec4 color = texture(sampler2D(t_rendered, NVR_SAMPLER(pc.samplerIndex)), uvc);
    // HLSL lerp(1.0f, tex2Dlod(SunFar).rgb, saturate(SunFarApply.x)): the composites' sun factor (0 or 1)
    color.rgb *= mix(vec3(1.0), textureLod(sampler2D(t_sunFar, NVR_SAMPLER(pc.pointSamplerIndex)), uvc, 0.0).rgb, saturate(pc.sunFarApply.x));

    // Underwater: the water plane itself is not shadowed (the HLSL returns color unchanged there).
    if (pc.waterSettings.z == 1.0) {
        float depth = readDepthLod(uvc);
        vec3 cameraVector = toWorld(uv) * depth;
        vec4 worldPos = vec4(pc.cameraPosition.xyz + cameraVector, 1.0);
        vec3 worldNormal = getWorldNormalLod(uvc);
        if (worldPos.z < (pc.waterSettings.x + 2.0) && worldPos.z > (pc.waterSettings.x - 2.0) && dot(worldNormal, vec3(0.0, 0.0, -1.0)) > 0.999) {
            o_color = color;
            return;
        }
    }

    vec2 shadow = texture(sampler2D(t_pointShadow, NVR_SAMPLER(pc.samplerIndex)), uvc).rg;
    shadow.r = mix(pc.shadowFade.x, 1.0, shadow.r); // fade shadows to light when sun is low

    // scale shadows strength to ambient before adding attenuation for pointlights (ShadowFade.z means point Lights are on)
    float darkness = max(0.0, 1.0 - pc.shadowData.y); // DARKNESS
    float ambient = mix(1.0, luma(pc.sunAmbient.rgb), darkness * pc.shadowFade.z); // linearise
    shadow.r = mix(0.0, ambient, shadow.r); // scale brightest areas to the ambient so it can be lit further with attenuation
    shadow.r += shadow.g; // Apply poing light attenuation (includes point light shadows)

    shadow.r = mix(darkness, 1.0, shadow.r); // brighten shadow value from 0 to darkness from config value
    shadow.r = saturate(shadow.r);

    color.rgb = pows(color.rgb, 2.2); // linearise
    vec4 skyColor = vec4(pows(pc.skyColor.rgb, 2.2), pc.skyColor.w); // linearise
    // tint shadowed areas with Sky color before blending
    vec4 colorShadow = luma(color.rgb) * shadow.r * skyColor;
    colorShadow.rgb = mix(colorShadow.rgb, color.rgb * shadow.r, saturate(shadow.r + 0.5)); // bias the transition between the 2 colors to make it less noticeable
    colorShadow.rgb = pows(max(vec3(0.0), colorShadow.rgb), 1.0 / 2.2); // delinearise
    o_color = vec4(colorShadow.rgb, 1.0);
}
