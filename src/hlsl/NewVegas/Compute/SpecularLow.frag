// Specular, technique SpecularLow (needs NVR DXVK): the light each quarter-resolution block's surface reflects, as
// Effects/Specular.fx.hlsl's SpecularLowPS line for line. effects/Specular.cpp ExtRender (technique 2) draws it into
// TESR_SpecularBuffer; NVR_SpecularSkyVis and NVR_SpecularSunFar are the extras BindExtras sets for that step.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "SpecularCommon.glsl"

layout(set = 1, binding = 0) uniform texture2D t_specularNormals;   // TESR_SpecularNormals
layout(set = 1, binding = 1) uniform texture2D t_shadowAtlas;       // TESR_ShadowAtlas
layout(set = 1, binding = 2) uniform texture2D t_skyVis;            // NVR_SpecularSkyVis (or a placeholder, see the host)
layout(set = 1, binding = 3) uniform texture2D t_sunFar;            // NVR_SpecularSunFar (or a placeholder, see the host)

layout(push_constant) uniform Push {
    uint pointSampler;       // 0: POINT/CLAMP heap index (filled by the host)
    uint linearSampler;      // 4: LINEAR/CLAMP heap index
    uint pad[2];
} pc;

layout(location = 0) out vec4 o_color;

void main() {
    vec2 vpos = floor(gl_FragCoord.xy);                       // VPOS
    vec2 px = cb.reciprocalResolution.xy;
    float lowScale = cb.specularEffects.z > 0.0 ? cb.specularEffects.z : 4.0;
    vec2 lowPx = px * lowScale;
    vec2 uv = (vpos + 0.5) * lowPx;                           // this block (its centre, in screen UV)
    vec4 centre = sampleLod(t_specularNormals, pc.pointSampler, uv);
    float depth = centre.a;
    if (depth >= cb.specularData.w || depth >= cb.cameraData.y * 0.98) {
        o_color = vec4(0.0, 0.0, 0.0, depth);
        return;
    }

    vec3 eyeVector = toWorld(uv) * depth;
    vec3 V = -normalize(eyeVector);

    // The normal over a wide patch (blocks one and three out; those whose depth is off this one's down-weighted smoothly)
    vec3 sum = centre.xyz;
    float weight = 1.0;
    float tolerance = 0.03 * depth + 3.0;
    const vec2 ring[16] = vec2[16](vec2(1, 0), vec2(1, 1), vec2(0, 1), vec2(-1, 1), vec2(-1, 0), vec2(-1, -1), vec2(0, -1), vec2(1, -1),
                                   vec2(3, 1), vec2(1, 3), vec2(-1, 3), vec2(-3, 1), vec2(-3, -1), vec2(-1, -3), vec2(1, -3), vec2(3, -1));
    for (int k = 0; k < 16; k++) {
        vec4 s = sampleLod(t_specularNormals, pc.pointSampler, clampToRectIfOn(uv + ring[k] * lowPx, lowPx));
        float rel = abs(s.a - depth) / tolerance;
        rel *= rel;
        float w = 1.0 / (1.0 + rel * rel);
        sum += w * s.xyz;
        weight += w;
    }
    vec3 meanNormal = sum / weight;
    float agreement = saturate(length(meanNormal));              // 1: all the same; less where they disagree
    vec3 N = normalize(viewToWorld(meanNormal));

    // Water has its own reflections.
    vec3 worldPos = cb.cameraPosition.xyz + eyeVector;
    if (N.z > 0.9 && abs(worldPos.z - cb.waterSettings.x) < (depth / cb.cameraData.y) * 200.0) {
        o_color = vec4(0.0, 0.0, 0.0, depth);
        return;
    }

    // The roughness, widened by the normals' spread (Toksvig)
    float alpha = cb.specularData.y * cb.specularData.y;
    float variance = (1.0 - agreement) / max(agreement, 0.001);
    float alpha2 = saturate(alpha * alpha + cb.specularEffects.x * 2.0 * variance);
    float roughAA = sqrt(sqrt(alpha2));

    vec3 L = normalize(cb.sunDirection.xyz);
    vec3 Hv = normalize(L + V);
    float NdotL = saturate(dot(N, L));
    float NdotV = max(dot(N, V), 0.0001);
    float NdotH = saturate(dot(N, Hv));
    float VdotH = saturate(dot(V, Hv));
    float F0 = cb.specularEffects.y;
    float dn = NdotH * NdotH * (alpha2 - 1.0) + 1.0;
    float D = alpha2 / (PI_HLSL * dn * dn);
    float vis = 0.5 / (NdotL * sqrt(NdotV * NdotV * (1.0 - alpha2) + alpha2) + NdotV * sqrt(NdotL * NdotL * (1.0 - alpha2) + alpha2) + 0.00001);
    float F = F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);
    float sunSpec = min(D * vis * F * NdotL * PI_HLSL, 2.0) * cb.specularData.x;
    // The sun's shadow from the cascades (not SPECULAR_SIM_SHADOWS: that define is not set by NVR)
    if (sunSpec > 0.00001 && cb.shadowFade.y > 0.0) sunSpec *= sunCascadeLight(t_shadowAtlas, pc.linearSampler, eyeVector, N);

    // The sky along the reflected view; below the horizon, dimmer.
    vec3 R = reflect(-V, N);
    float skyAt = sqrt(saturate(R.z));
    float below = R.z >= 0.0 ? 1.0 : mix(0.6, 0.25, saturate(-R.z * 4.0));
    vec2 env = envBRDFApprox(roughAA, NdotV);
    float skyAmount = (F0 * env.x + env.y) * cb.specularData.z * below;
    // UNOFFICIAL 2026-10-19 (no branches on these constants, as the D3D9 technique): x and z are 0 or 1.
    skyAmount *= mix(1.0, mix(cb.specularExtra.y, 1.0, sampleLod(t_skyVis, pc.linearSampler, uv).b), cb.specularExtra.x);
    sunSpec *= mix(1.0, sampleLod(t_sunFar, pc.linearSampler, uv).a, cb.specularExtra.z);

    float fogAmount = cb.fogData.y > cb.fogData.x + 1.0
        ? pow(1.0 - saturate((cb.fogData.y - length(eyeVector)) / max(cb.fogData.y - cb.fogData.x, 1.0)), max(cb.fogData.w, 0.0001))
        : 0.0;
    float fade = (1.0 - smoothstep(0.7 * cb.specularData.w, cb.specularData.w, depth)) * (1.0 - fogAmount);
    o_color = vec4(skyAmount * fade, skyAt, sunSpec * fade, depth);
}
