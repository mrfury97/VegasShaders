// AmbientLight pass Apply as a Vulkan pass (needs NVR DXVK): Effects/AmbientLight.fx.hlsl Apply (with SunShadowApply and SkyFactorAt),
// line for line (AmbientLight.glsl). variant bit 0: sky (technique AmbientLightSky); bit 1: a texture is on NVR_GridWave's stage (the
// D3D9 pass reads an empty stage as 0 for DebugView 8 and 12). Into the frame. effects/AmbientLight.cpp ExtDraw.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "AmbientLight.glsl"

layout(set = 1, binding = 0)  uniform texture2D t_source;            // TESR_SourceBuffer (LINEAR)
layout(set = 1, binding = 1)  uniform texture2D t_sunFar;            // NVR_SunFarAL (POINT)
layout(set = 1, binding = 2)  uniform texture2D t_depth;             // TESR_DepthBuffer
layout(set = 1, binding = 3)  uniform texture2D t_normals;           // TESR_NormalsBuffer
layout(set = 1, binding = 4)  uniform texture2D t_pointShadow;       // TESR_PointShadowBuffer (LINEAR)
layout(set = 1, binding = 5)  uniform texture2D t_skyVis;            // NVR_ALSkyVis (POINT)
layout(set = 1, binding = 6)  uniform texture2D t_viewModel;         // NVR_ALViewModel
layout(set = 1, binding = 7)  uniform texture2D t_occlusionHistory;  // NVR_ALOcclusionHistory (this frame's)
layout(set = 1, binding = 8)  uniform texture2D t_gridWave;          // NVR_GridWave's stage (DebugView 8, 12)
layout(set = 1, binding = 9)  uniform texture2D t_gridAccum;         // NVR_GridAccum (LINEAR)
layout(set = 1, binding = 10) uniform texture2D t_gridAccumColor;    // NVR_GridAccumColor (LINEAR)
layout(set = 1, binding = 11) uniform texture2D t_balance;           // NVR_ALBalance (NVR_ALHistory's stage)
layout(set = 1, binding = 12) uniform texture2D t_buffer;            // NVR_ALBuffer
layout(set = 1, binding = 13) uniform texture2D t_albedo;            // NVR_ALAlbedo (FdIndirect::ALAlbedo when active; cb.alFlags.z)
#ifdef NVR_GI_APPLY
layout(set = 1, binding = 14) uniform texture2D t_gi;                // NVR_AmbientLightHistory (this frame's)
layout(set = 1, binding = 15) uniform texture2D t_shadingNormals;    // NVR_ALShadingNormals (DeferredBumpedNormals; cb.alBump.x)
#endif

layout(location = 0) out vec4 o_color;

vec4 gridWaveLod(vec2 uv) { return (pc.variant & 2u) != 0u ? pointLod(t_gridWave, uv) : vec4(0.0); }

vec3 SunShadowApply(vec3 color, vec2 ruv, vec2 screenUV) {
    color *= mix(vec3(1.0), pointLod(t_sunFar, ruv).rgb, cb.sunFarFlags.x);
    if (cb.waterSettings.z == 1.0) {
        vec3 worldPos = cb.cameraPosition.xyz + toWorld(screenUV) * readDepthLod(t_depth, ruv);
        if (worldPos.z < (cb.waterSettings.x + 2.0) && worldPos.z > (cb.waterSettings.x - 2.0) && dot(GetWorldNormalLod(t_normals, ruv), vec3(0.0, 0.0, -1.0)) > 0.999)
            return color;
    }
    float darkness = max(0.0, 1.0 - cb.shadowData.y);
    vec2 shadow = linearLod(t_pointShadow, ruv).rg;
    shadow.r = mix(cb.shadowFade.x, 1.0, shadow.r);
    float ambient = mix(1.0, luma(cb.sunAmbient.rgb), darkness * cb.shadowFade.z);
    shadow.r = mix(0.0, ambient, shadow.r);
    shadow.r += shadow.g;
    shadow.r = saturate(mix(darkness, 1.0, shadow.r));
    vec3 linearColor = pows(color, 2.2);
    vec3 colorShadow = luma(linearColor) * shadow.r * pows(cb.skyColor.rgb, 2.2);
    colorShadow = mix(colorShadow, linearColor * shadow.r, saturate(shadow.r + 0.5));
    return pows(max(vec3(0.0), colorShadow), 1.0 / 2.2);
}

vec2 SkyFactorAt(vec2 ruv, vec2 scale, float depth) {
    if (!(depth < endFade)) return vec2(1.0);
    vec2 position = ruv / cb.alLayout.zw - 0.5;
    vec2 base = floor(position);
    vec2 fraction = position - base;
    vec2 sum = vec2(0.0);
    float weights = 0.0;
    vec2 maxUV = max(scale - 0.5 * cb.alLayout.zw, 0.5 * cb.alLayout.zw);
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 2; x++) {
            vec2 tapUV = clamp((base + vec2(float(x), float(y)) + 0.5) * cb.alLayout.zw, 0.5 * cb.alLayout.zw, maxUV);
            vec3 tap = pointLod(t_skyVis, tapUV).rgb;
            float weight = (x != 0 ? fraction.x : 1.0 - fraction.x) * (y != 0 ? fraction.y : 1.0 - fraction.y);
            weight /= 1.0 + abs(tap.y - depth) / max(cb.aoData.z, 0.001);
            sum += tap.xz * weight;
            weights += weight;
        }
    }
    return clamp(sum / max(weights, 1.0e-6), 0.0, 1.0);
}

vec4 Apply(vec2 uv, bool sky) {
    vec2 scale = cb.renderScale.x > 0.0 && cb.renderScale.y > 0.0 ? cb.renderScale.xy : vec2(1.0);
    vec2 ruv = uv * scale;
    vec4 color = texture(sampler2D(t_source, LINEAR_S), ruv);
    bool viewModel = IsViewModel(t_viewModel, ruv);
    if (!viewModel && cb.alFlags.x > 0.5) color = vec4(SunShadowApply(color.rgb, ruv, uv), color.a);
    float depth = texture(sampler2D(t_depth, POINT_S), ruv).x * farZ;   // readDepth
    vec2 skyFactor = vec2(1.0);
    if (sky && !viewModel) skyFactor = SkyFactorAt(ruv, scale, depth);
    if (!viewModel && (depth < NearFade || depth > DrawDistance)) {
        if (sky) color.rgb = pows(SkyTake(pows(color.rgb, 2.2), max(skyFactor.x, DarkestAO), skyFactor.y, cb.aoSky5.x, cb.aoSky5.w), 1.0 / 2.2);
        return DebugView > 0.5 ? vec4(0.0, 0.0, 0.0, 1.0) : color;
    }

    vec2 position = ruv / cb.alLayout.zw - 0.5;
    vec2 base = floor(position);
    vec2 fraction = position - base;
    float tolerance = max(depth * 0.03, 2.0);
    vec4 sum = vec4(0.0);
    float weights = 1.0e-5;
    // UNOFFICIAL 2026-11-01 EdgeReject (AmbientLight.fx.hlsl Apply, line for line; alFlags.w)
    float edgeReject = cb.alFlags.w > 0.5 ? 1.0 : 0.0;
    float nearestCloseness = 1.0e9;
    vec4 nearestValue = vec4(0.0, 0.0, 0.0, 1.0);
    vec2 maxHalfUV = max(scale - 0.5 * cb.alLayout.zw, 0.5 * cb.alLayout.zw);
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 2; x++) {
            vec2 tapUV = clamp((base + vec2(float(x), float(y)) + 0.5) * cb.alLayout.zw, 0.5 * cb.alLayout.zw, maxHalfUV);
            float tapDepth = readDepthLod(t_depth, tapUV - 0.5 * max(cb.alLayout.zw - cb.reciprocalResolution.xy, vec2(0.0)));
            float weight = (x != 0 ? fraction.x : 1.0 - fraction.x) * (y != 0 ? fraction.y : 1.0 - fraction.y);
            float closeness = abs(tapDepth - depth) / tolerance;
            weight *= mix(1.0 / (1.0 + closeness), saturate(1.0 - closeness), edgeReject);
            vec4 value = vec4(0.0, 0.0, 0.0, pointLod(t_occlusionHistory, tapUV).r);
#ifdef NVR_GI_APPLY
            value.rgb = pointLod(t_gi, tapUV).rgb;
#endif
            sum += value * weight;
            weights += weight;
            if (closeness < nearestCloseness) { nearestCloseness = closeness; nearestValue = value; }
        }
    }
    vec4 ambient = sum / weights;
    if (edgeReject > 0.5 && weights < 0.01) ambient = nearestValue;

    vec3 lin = linearize(color.rgb);
    float shadedLuma = luma(lin);
    float ao = mix(DarkestAO, 1.0, ambient.a);
    bool balanced = (cb.alInterior.x > 0.5) || (cb.alInterior.w > 0.0);
    if (!balanced) ao = mix(ao, 1.0, smoothstep(BrightFade * 0.5, BrightFade * 1.5 + 0.15, shadedLuma));
    vec2 px = cb.reciprocalResolution.xy;
    vec2 minFullUV = 0.5 * px, maxFullUV = max(scale - 0.5 * px, minFullUV);
    float left = readDepthLod(t_depth, clamp(ruv - vec2(px.x, 0.0), minFullUV, maxFullUV));
    float right = readDepthLod(t_depth, clamp(ruv + vec2(px.x, 0.0), minFullUV, maxFullUV));
    float up = readDepthLod(t_depth, clamp(ruv - vec2(0.0, px.y), minFullUV, maxFullUV));
    float down = readDepthLod(t_depth, clamp(ruv + vec2(0.0, px.y), minFullUV, maxFullUV));
    float bendX = abs(left + right - 2.0 * depth);
    float bendY = abs(up + down - 2.0 * depth);
    if (cb.alFlags.y > 0.5) {
        float left2 = readDepthLod(t_depth, clamp(ruv - vec2(2.0 * px.x, 0.0), minFullUV, maxFullUV));
        float right2 = readDepthLod(t_depth, clamp(ruv + vec2(2.0 * px.x, 0.0), minFullUV, maxFullUV));
        float up2 = readDepthLod(t_depth, clamp(ruv - vec2(0.0, 2.0 * px.y), minFullUV, maxFullUV));
        float down2 = readDepthLod(t_depth, clamp(ruv + vec2(0.0, 2.0 * px.y), minFullUV, maxFullUV));
        bendX = min(bendX, min(abs(left2 + depth - 2.0 * left), abs(depth + right2 - 2.0 * right)));
        bendY = min(bendY, min(abs(up2 + depth - 2.0 * up), abs(depth + down2 - 2.0 * down)));
    }
    float busy = saturate((bendX + bendY) / depth * 20.0 - 0.1);
#ifdef NVR_GI_APPLY
    vec3 giLight = ambient.rgb * cb.data.z;
    if (DebugView > 12.5) return delinearize(vec4(giLight, 1.0));   // the light arriving (before the surface colour)
#endif
    if (DebugView > 11.5) {
        vec3 c = floor((toWorld(uv) * depth - cb.gridOrigin.xyz) / cb.gridOrigin.w);
        vec4 m = gridWaveLod(CellUV(clamp(c, vec3(0.0), vec3(GridX, GridY, GridZ) - 1.0)));
        return vec4(saturate(m.xyz * cb.gridStep.y), 1.0);
    }
    if (DebugView > 10.5) return vec4(vec3(busy), 1.0);
    ao = viewModel ? 1.0 : mix(ao, mix(1.0, ao, 0.25), busy);
    vec3 albedo = SurfaceAlbedo(t_albedo, ruv, lin);
    vec3 gridBounce = vec3(0.0);
    if (cb.gridData.x > 0.0) {
        vec3 cameraRelative = viewModel ? vec3(0.0) : toWorld(uv) * depth;
        vec3 normal = normalize(GetWorldNormalLod(t_normals, ruv));
        gridBounce = GridBounce(t_gridAccum, t_gridAccumColor, cameraRelative, normal) * (cb.gridData.x * ao);
    }
    if (DebugView > 8.5) return delinearize(vec4(DebugView < 9.5 ? gridBounce : albedo, 1.0));
    if (cb.alInterior.x > 0.5) gridBounce *= pointLod(t_balance, vec2(0.5, 0.5)).x;
    if (DebugView > 7.5) {
        vec3 cameraRelative = toWorld(uv) * depth;
        vec3 c = floor((cameraRelative - cb.gridOrigin.xyz) / cb.gridOrigin.w);
        vec4 m = gridWaveLod(CellUV(clamp(c, vec3(0.0), vec3(GridX, GridY, GridZ) - 1.0)));
        return vec4(saturate(m.x * 4.0), m.x != 0.0 ? 0.5 : 0.0, saturate(gridBounce.b * 4.0), 1.0);
    }
    if (DebugView > 6.5) return delinearize(vec4(gridBounce * albedo, 1.0));
    if (DebugView > 2.5) return vec4(pointLod(t_buffer, ruv).rgb, 1.0);
    if (DebugView > 0.5) return delinearize(vec4(DebugView < 1.5 ? vec3(ao) : gridBounce * albedo, 1.0));
    // UNOFFICIAL 2026-11-12 AmbientReplaceInterior (RoomGI; AmbientLight.fx.hlsl Apply, line for line; cb.alBump.y mirrors NVR_ALRoom.x)
    if (viewModel) {
        lin = lin + gridBounce * albedo;
    } else if (cb.alInterior.x > 0.5) {
        lin = lin - ((1.0 - ao) + ao * max(cb.alInterior.x - 1.0, 0.0)) * min(lin, albedo * cb.alScene.rgb * (1.0 - cb.alBump.y)) + gridBounce * albedo;
    } else {
        // UNOFFICIAL 2026-11-11 (sungi) ExteriorBalance + AmbientReplace (AmbientLight.fx.hlsl Apply, line for line)
        vec3 ambShare = min(lin, albedo * cb.alScene.rgb * cb.alInterior.w);
        float keepShare = 1.0 - cb.alInterior.y * (1.0 - cb.alInterior.z);
        vec3 lit = cb.alInterior.w > 0.0 ? lin - ambShare + keepShare * ambShare * ao : lin * ao;
        if (sky) lit = SkyTake(lit, max(skyFactor.x, DarkestAO), skyFactor.y, cb.aoSky5.x, cb.aoSky5.w);
        lin = lit + gridBounce * albedo;
    }
#ifdef NVR_GI_APPLY
    vec3 c0 = linearize(pointLod(t_source, ruv).rgb);
    vec3 localSum = c0;
    float localWeight = 1.0;
    for (int i = 0; i < 4; i++) {
        vec2 o = vec2((i & 1) != 0 ? 4.0 : -4.0, (i & 2) != 0 ? 4.0 : -4.0) * px;
        vec2 tap = clamp(ruv + o, minFullUV, maxFullUV);
        float w = step(abs(readDepthLod(t_depth, tap) - depth), max(depth * 0.02, 1.0));
        localSum += linearize(pointLod(t_source, tap).rgb) * w;
        localWeight += w;
    }
    vec3 localLin = localSum / localWeight;
    float shade = shadedLuma / max(luma(c0), 1.0e-4);
    float detail = clamp(luma(c0) / max(luma(localLin), 1.0e-4), 0.0, 2.0);
    vec3 guess = SurfaceColourCap(localLin * shade, FallbackCap()) * detail;
    vec4 a = pointLod(t_albedo, ruv);
    float hasAlbedo = step(0.5, cb.alFlags.z) * step(0.5, a.a);
    lin += giLight * mix(guess, clamp(a.rgb, vec3(0.0), vec3(0.85)), hasAlbedo);
#endif
    return delinearize(vec4(lin, color.a));
}

void main() { o_color = Apply((floor(gl_FragCoord.xy) + 0.5) * pc.frame.xy, (pc.variant & 1u) != 0u); }
