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
#ifdef NVR_GI_APPLY
// UNOFFICIAL 2026-10-27 (deep/fork2-ssgi) ComputeGI (variants.txt: AmbientLightApply-gi): this frame's accumulated bounce light
// (AmbientLightGIAccumulate.frag's first target), upsampled with the occlusion's own depth-weighted taps (joint bilateral) and added as
// light x the surface colour x TESR_AmbientLightData.z (ComputeGIStrength; the retired screen bounce's slot, 0 otherwise). DebugView 13:
// the light alone (x strength, before the surface colour; the first version showed it x SurfaceColour, whose hue boost hid what the
// light itself looked like). Without the define (the shipped AmbientLightApply.spv) none of this is compiled.
layout(set = 1, binding = 13) uniform texture2D t_gi;                // NVR_AmbientLightHistory (this frame's)
#endif

layout(location = 0) out vec4 o_color;

vec4 gridWaveLod(vec2 uv) { return (pc.variant & 2u) != 0u ? pointLod(t_gridWave, uv) : vec4(0.0); }

vec3 SunShadowApply(vec3 color, vec2 uv) {
    color *= mix(vec3(1.0), pointLod(t_sunFar, uv).rgb, cb.sunFarFlags.x);
    if (cb.waterSettings.z == 1.0) {
        vec3 worldPos = cb.cameraPosition.xyz + toWorld(uv) * readDepthLod(t_depth, uv);
        if (worldPos.z < (cb.waterSettings.x + 2.0) && worldPos.z > (cb.waterSettings.x - 2.0) && dot(GetWorldNormalLod(t_normals, uv), vec3(0.0, 0.0, -1.0)) > 0.999)
            return color;
    }
    float darkness = max(0.0, 1.0 - cb.shadowData.y);
    vec2 shadow = linearLod(t_pointShadow, uv).rg;
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

vec2 SkyFactorAt(vec2 uv, float depth) {
    if (!(depth < endFade)) return vec2(1.0);
    vec2 position = uv / cb.alLayout.zw - 0.5;
    vec2 base = floor(position);
    vec2 fraction = position - base;
    vec2 sum = vec2(0.0);
    float weights = 0.0;
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 2; x++) {
            vec2 tapUV = clamp((base + vec2(float(x), float(y)) + 0.5) * cb.alLayout.zw, 0.5 * cb.alLayout.zw, 1.0 - 0.5 * cb.alLayout.zw);
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
    vec4 color = texture(sampler2D(t_source, LINEAR_S), uv);
    if (cb.alFlags.x > 0.5) color = vec4(SunShadowApply(color.rgb, uv), 1.0);
    float depth = texture(sampler2D(t_depth, POINT_S), uv).x * farZ;   // readDepth
    vec2 skyFactor = vec2(1.0);
    if (sky) skyFactor = SkyFactorAt(uv, depth);
    if (depth < NearFade || depth > DrawDistance || IsViewModel(t_viewModel, uv)) {
        if (sky) color.rgb = pows(SkyTake(pows(color.rgb, 2.2), skyFactor.x, skyFactor.y, cb.aoSky5.x, cb.aoSky5.w), 1.0 / 2.2);
        return DebugView > 0.5 ? vec4(0.0, 0.0, 0.0, 1.0) : color;
    }

    vec2 position = uv / cb.alLayout.zw - 0.5;
    vec2 base = floor(position);
    vec2 fraction = position - base;
    float tolerance = max(depth * 0.03, 2.0);
    vec4 sum = vec4(0.0);
    float weights = 1.0e-5;
    // UNOFFICIAL 2026-11-01 EdgeReject (AmbientLight.fx.hlsl Apply, line for line; alFlags.w)
    float edgeReject = cb.alFlags.w > 0.5 ? 1.0 : 0.0;
    float nearestCloseness = 1.0e9;
    vec4 nearestValue = vec4(0.0, 0.0, 0.0, 1.0);
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 2; x++) {
            vec2 tapUV = (base + vec2(float(x), float(y)) + 0.5) * cb.alLayout.zw;
            float tapDepth = readDepthLod(t_depth, tapUV);
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
    float ao = mix(DarkestAO, 1.0, ambient.a);
    ao = mix(ao, 1.0, saturate((luma(lin) - BrightFade) * 3.0));
    vec2 px = cb.reciprocalResolution.xy;
    float left = readDepthLod(t_depth, uv - vec2(px.x, 0.0)), right = readDepthLod(t_depth, uv + vec2(px.x, 0.0));
    float up = readDepthLod(t_depth, uv - vec2(0.0, px.y)), down = readDepthLod(t_depth, uv + vec2(0.0, px.y));
    float bendX = abs(left + right - 2.0 * depth);
    float bendY = abs(up + down - 2.0 * depth);
    if (cb.alFlags.y > 0.5) {
        float left2 = readDepthLod(t_depth, uv - vec2(2.0 * px.x, 0.0)), right2 = readDepthLod(t_depth, uv + vec2(2.0 * px.x, 0.0));
        float up2 = readDepthLod(t_depth, uv - vec2(0.0, 2.0 * px.y)), down2 = readDepthLod(t_depth, uv + vec2(0.0, 2.0 * px.y));
        bendX = min(bendX, min(abs(left2 + depth - 2.0 * left), abs(depth + right2 - 2.0 * right)));
        bendY = min(bendY, min(abs(up2 + depth - 2.0 * up), abs(depth + down2 - 2.0 * down)));
    }
    float busy = saturate((bendX + bendY) / depth * 20.0 - 0.1);
#ifdef NVR_GI_APPLY
    // (2026-11-07, user: "RT GI at a high strength overrides the normal maps, looks extremely weird") SurfaceColour divides the lit
    // colour by the ambient light and caps it at MaxReflectance; under a lamp nearly every pixel is over the cap, so dark grout, bright
    // tiles and the normal map's shading all got the same colour and the bounce laid one flat milky layer over them (bench shipfix,
    // demo room: GI High lifted the floor 35 % and greyed the dark tiles). The cap now applies to a small local average (4 taps, 4 px
    // out, same surface only) and each pixel's own detail (its brightness against that average, at most 2x) goes back on top, so the
    // bounce keeps the texture and the normal map's light and dark like the game's own light does.
    vec3 localLin = lin;
    float localWeight = 1.0;
    for (int i = 0; i < 4; i++) {
        vec2 o = vec2((i & 1) != 0 ? 4.0 : -4.0, (i & 2) != 0 ? 4.0 : -4.0) * cb.reciprocalResolution.xy;
        float d = readDepthLod(t_depth, uv + o);
        if (abs(d - depth) > max(depth * 0.02, 1.0)) continue;   // another surface
        vec3 tap = texture(sampler2D(t_source, LINEAR_S), uv + o).rgb;
        if (cb.alFlags.x > 0.5) tap = SunShadowApply(tap, uv + o);
        localLin += linearize(tap);
        localWeight += 1.0;
    }
    localLin /= localWeight;
    float detail = clamp(luma(lin) / max(luma(localLin), 1.0e-4), 0.0, 2.0);
    vec3 screenBounce = ambient.rgb * (cb.data.z * SurfaceColour(localLin) * detail);
    if (DebugView > 12.5) return delinearize(vec4(ambient.rgb * cb.data.z, 1.0));   // the light arriving (before the surface colour)
#endif
    if (DebugView > 11.5) {
        vec3 c = floor((toWorld(uv) * depth - cb.gridOrigin.xyz) / cb.gridOrigin.w);
        vec4 m = gridWaveLod(CellUV(clamp(c, vec3(0.0), vec3(GridX, GridY, GridZ) - 1.0)));
        return vec4(saturate(m.xyz * cb.gridStep.y), 1.0);
    }
    if (DebugView > 10.5) return vec4(vec3(busy), 1.0);
    ao = mix(ao, mix(1.0, ao, 0.25), busy);
    vec3 albedo = SurfaceColour(lin);
    vec3 gridBounce = vec3(0.0);
    if (cb.gridData.x > 0.0) {
        vec3 cameraRelative = toWorld(uv) * depth;
        vec3 normal = normalize(GetWorldNormalLod(t_normals, uv));
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
    if (DebugView > 2.5) return vec4(pointLod(t_buffer, uv).rgb, 1.0);
    if (DebugView > 0.5) return delinearize(vec4(DebugView < 1.5 ? vec3(ao) : gridBounce * albedo, 1.0));
    if (cb.alInterior.x > 0.5) lin = lin - (1.0 - ao) * min(lin, albedo * cb.alScene.rgb) + gridBounce * albedo;
    else {
        // UNOFFICIAL 2026-11-11 (sungi) ExteriorBalance (AmbientLight.fx.hlsl Apply; alInterior.w > 0 outdoors = the ambient scale)
        vec3 lit = cb.alInterior.w > 0.0 ? lin - (1.0 - ao) * min(lin, albedo * cb.alScene.rgb * cb.alInterior.w) : lin * ao;
        if (sky) lit = SkyTake(lit, skyFactor.x, skyFactor.y, cb.aoSky5.x, cb.aoSky5.w);
        lin = lit + gridBounce * albedo;
    }
#ifdef NVR_GI_APPLY
    lin += screenBounce;   // the light of the sectors the occlusion took (the grid's light above came in through the open ones)
#endif
    return delinearize(vec4(lin, color.a));
}

void main() { o_color = Apply(frameUV(), (pc.variant & 1u) != 0u); }
