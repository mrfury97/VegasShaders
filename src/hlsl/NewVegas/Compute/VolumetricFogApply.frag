// VolumetricFog's full-resolution passes as one Vulkan pass shader (needs NVR DXVK), line for line from Effects/VolumetricFog.fx.hlsl;
// push mode picks the pixel shader: 0 DedicatedFogCombine, 1 DedicatedFogComposite, 2 DedicatedFogCompositeInterior (the second pass
// of DedicatedFog, CompositeFog and CompositeFogInterior, and of their Legacy and Classic forms), 3 ShadowAOApply (technique
// ApplyShadowAO), 4 VolumetricFog (the full-resolution techniques: the unnamed one, VolumetricFogLegacy, VolumetricFogClassic).
// effects/VolumetricFog.cpp ExtDedicated, ExtShadowAO, ExtRender.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"
#define FOG_APPLY
#include "VolumetricFog.glsl"

layout(location = 0) out vec4 o_color;

void main() {
    vec2 ruv = uv * fogRectScale();
    bool legacy = FOG_LEGACY_ON, classic = FOG_CLASSIC_ON;
    if (FOG_MODE_IS(4u)) {
        // VolumetricFog
        vec4 color = linearize(LINEAR_LOD(t_scene, ruv));
        vec3 fogMul, fogAdd;
        float nightAmount;
        FogTerms(ruv, fogMul, fogAdd, nightAmount, legacy, false, classic);
        vec4 finalColor = max(vec4(color.rgb * fogMul + fogAdd, mix(color.a, 1.0, nightAmount)), vec4(0.0));
        o_color = delinearize(finalColor);
        return;
    }
    vec3 color;
    if (FOG_MODE_IS(2u)) {
        // DedicatedFogCompositeInterior
        color = InteriorStoreRound(ApplyInteriorShadowDepth(ruv, LINEAR_LOD(t_scene, ruv), LINEAR_LOD(t_pointShadow, ruv).r, readDepthLod(ruv)).rgb);
        color = pows(color, 2.2);
        if (pc.compositeFlags.y > 0.5) color = CompositeAO(color, ruv);
        color = pows(color, 1.0 / 2.2);
        color = FogApply(color, ruv, legacy, true, classic);
    }
    else if (FOG_MODE_IS(0u)) {
        // DedicatedFogCombine
        color = FogApply(LINEAR_LOD(t_scene, ruv).rgb, ruv, legacy, false, classic);
    }
    else {
        // DedicatedFogComposite (1), ShadowAOApply (3)
        color = LINEAR_LOD(t_scene, ruv).rgb;
        color = pows(color, 2.2);
        if (pc.compositeFlags.x > 0.5) color = CompositeSunShadow(color, ruv);
        if (pc.compositeFlags.y > 0.5) color = CompositeAO(color, ruv);
        color = pows(color, 1.0 / 2.2);
        if (FOG_MODE_IS(3u)) { o_color = vec4(color, 1.0); return; }
        color = FogApply(color, ruv, legacy, false, classic);
    }
    if (pc.gridFlags.y > 0.5) color = FogGridLight(color, ruv);	// UNOFFICIAL froxel grid
    o_color = vec4(color, 1.0);
}
