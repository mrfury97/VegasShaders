// PointShadows technique 2 (LampContact) as a Vulkan pass (needs NVR DXVK): Effects/PointShadows.fx.hlsl's LampContactPS, line for
// line. The pass multiplies the lamp mask in the buffer by its result (blend: multiply). effects/PointShadows.cpp ExtRender runs it.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "PointShadowsCommon.glsl"

layout(set = 1, binding = 0) uniform texture2D t_depth;      // TESR_DepthBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 1) uniform texture2D t_normals;    // TESR_NormalsBuffer (POINT, CLAMP)

layout(push_constant) uniform Push {
    uint linearSampler;   // 0: LINEAR, CLAMP heap index (filled by the host)
    uint pointSampler;    // 4: POINT, CLAMP heap index
    uint mode;            // 8: unused here
    uint pad;
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

// PointShadows.fx: ContactProject (undoes toWorld: a camera-relative point -> its screen position and view depth)
vec3 contactProject(vec3 r) {
    vec3 v = mat3(cb.viewTransform) * r;   // HLSL mul(r, (float3x3)TESR_ViewTransform)
    return vec3((v.x / v.z * cb.projectionTransform[0][0]) * 0.5 + 0.5, (v.y / v.z * cb.projectionTransform[1][1]) * -0.5 + 0.5, v.z);
}

// PointShadows.fx: ContactOpen (how open the way from this pixel toward a lamp is; 1 open, 0 blocked right at the pixel)
float contactOpen(texture2D depthTex, sampler samp, vec3 r0, vec3 worldPos, vec3 n, vec3 lampWorld, vec2 ruv, float pixelDepth) {
    vec3 toLamp = lampWorld - worldPos;
    float dist = length(toLamp);
    vec3 dir = toLamp / dist;
    if (dot(dir, n) < 0.1) return 1.0;

    float len = min(cb.lampContact.y, dist * 0.5);
    float jitter = mix(0.5, fract(cb.lampContactNoise.z * fract(dot(ruv, cb.lampContactNoise.xy))), cb.lampContactNoise.w);
    vec3 start = r0 + n * (cb.lampContact.w + pixelDepth * cb.lampContactBias.z);
    vec2 minUV = 0.5 * cb.reciprocalResolution.xy;
    vec2 maxUV = max(cb.renderScale.xy - minUV, minUV);
    float occ = 0.0;
    for (int i = 0; i < 6; i++) {
        float t = (float(i) + jitter) / 6.0;
        vec3 s = contactProject(start + dir * (len * t));
        if (s.z < 1.0 || any(lessThan(s.xy, vec2(0.0))) || any(greaterThan(s.xy, vec2(1.0)))) break;
        float delta = s.z - textureLod(sampler2D(depthTex, samp), clamp(s.xy * cb.renderScale.xy, minUV, maxUV), 0.0).x * cb.cameraData.y;	// (readDepthLod)
        occ = max(occ, (delta > cb.lampContactBias.x + s.z * cb.lampContactBias.y && delta < cb.lampContact.z) ? 1.0 - t : 0.0);
    }
    return 1.0 - occ;
}

#define LINEAR_CLAMP NVR_SAMPLER(pc.linearSampler)
#define POINT_CLAMP NVR_SAMPLER(pc.pointSampler)

void main() {
    vec2 minUV = 0.5 * cb.reciprocalResolution.xy;
    vec2 maxUV = max(cb.renderScale.xy - minUV, minUV);
    vec2 ruv = clamp(uv * cb.renderScale.xy, minUV, maxUV);
    float depth = readDepthAt(t_depth, LINEAR_CLAMP, ruv);
    float fade = clamp((depth - cb.lampContactFade.x) * cb.lampContactFade.y, 0.0, 1.0) * clamp((cb.lampContactFade.z - depth) * cb.lampContactFade.w, 0.0, 1.0);
    float keep = 1.0;
    if (fade > 0.0) {
        vec3 cameraVector = toWorld(uv) * depth;
        vec3 worldPos = cb.cameraPosition.xyz + cameraVector;
        float total = 0.0, b1 = 0.0;
        vec3 L1 = vec3(0.0);
        for (int i = 0; i < 11; i++) {
            vec4 lp = cb.shadowLightPosition[i];
            vec3 lightDir = lp.xyz - worldPos;
            float s2 = clamp(dot(lightDir, lightDir) / max(lp.w * lp.w, 1.0), 0.0, 1.0);
            float a = lp.w > 0.0 ? (1.0 - s2) * (1.0 - s2) * luma(cb.lightColor[i].rgb) * cb.lightColor[i].w : 0.0;
            total += a;
            if (a > b1) { b1 = a; L1 = lp.xyz; }
        }
        if (b1 > cb.lampContactBias.w * total && b1 > 0.0) {
            float blocked = b1 * (1.0 - contactOpen(t_depth, LINEAR_CLAMP, cameraVector, worldPos, worldNormalAt(t_normals, POINT_CLAMP, ruv), L1, ruv, depth));
            keep = 1.0 - cb.lampContact.x * fade * clamp(blocked / total, 0.0, 1.0);
        }
    }
    o_color = vec4(keep, keep, keep, 1.0);
}
