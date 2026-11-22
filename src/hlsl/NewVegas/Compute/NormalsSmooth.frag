// Normals' smoothing as Vulkan passes (needs NVR DXVK): Effects/Normals.fx.hlsl technique NormalsSmooth, line for line.
// Push alongY 0: pass X (reads the normals buffer, binding 0); 1: pass Y (reads the spare, binding 0). The depth is binding 1.
// Output: the same encoding as the normals pass; the sky (zC >= 0.99 far) and taps off the drawn part keep their values.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"
#include "Normals.glsl"

layout(location = 0) out vec4 outColor;

layout(set = 1, binding = 0) uniform texture2D t_source;	// the normals (pass X) or the spare target (pass Y)
layout(set = 1, binding = 1) uniform texture2D t_depth;	// CombineDepth's output

layout(push_constant) uniform Push {
    vec4 reciprocalResolution;	// 0: TESR_ReciprocalResolution
    vec4 smoothData;			// 16: TESR_NormalsSmoothData (x reach in texels)
    vec4 cameraData;			// 32: TESR_CameraData
    mat4 projection;			// 48: TESR_ProjectionTransform
    uint samplerIndex;			// 112: POINT, CLAMP
    uint alongY;				// 116: 0 along x, 1 along y
    uint pad[2];				// 120
    vec4 renderScale;			// 128: (scaleX, scaleY, 1/scaleX, 1/scaleY) (UNOFFICIAL 2026-11-19 RenderRect)
} pc;

float viewZAt(vec2 uv) {
    return max(texture(sampler2D(t_depth, NVR_SAMPLER(pc.samplerIndex)), uv).x * pc.cameraData.y, 1.0e-6);
}

vec4 sourceAt(vec2 uv) {
    return texture(sampler2D(t_source, NVR_SAMPLER(pc.samplerIndex)), uv);
}

void main() {
    vec2 t = pc.reciprocalResolution.xy;
    vec2 scale = pc.renderScale.x > 0.0 ? pc.renderScale.xy : vec2(1.0);
    vec2 invScale = pc.renderScale.z > 0.0 ? pc.renderScale.zw : vec2(1.0);
    vec2 uv = gl_FragCoord.xy * t;
    vec2 axis = pc.alongY != 0u ? vec2(0.0, 1.0) : vec2(1.0, 0.0);

    vec4 centreSample = sourceAt(uv);
    float zC = viewZAt(uv);
    if (zC >= 0.99 * pc.cameraData.y) {
        outColor = centreSample;
        return;
    }

    vec3 nC = normalize(centreSample.xyz * 2.0 - 1.0);
    vec3 pC = nvrViewPositionFromProjection(uv * invScale, zC, pc.projection);
    float reach = pc.smoothData.x;

    vec3 sum = nC;
    for (int i = 0; i < 6; i++) {
        int k = i < 3 ? i - 3 : i - 2;	// -3 -2 -1 1 2 3
        float distTexels = abs(float(k)) * reach / 3.0;
        vec2 uvRaw = uv + axis * (float(k) * reach / 3.0) * t;
        float onScreen = (any(lessThan(uvRaw, vec2(0.0))) || any(greaterThan(uvRaw, scale))) ? 0.0 : 1.0;
        vec2 uvT = clamp(uvRaw, 0.5 * t, scale - 0.5 * t);

        float zT = viewZAt(uvT);
        vec3 nT = normalize(sourceAt(uvT).xyz * 2.0 - 1.0);
        vec3 pT = nvrViewPositionFromProjection(uvT * invScale, zT, pc.projection);

        float tolerance = max(zC * 0.0005 * distTexels, 0.5);
        float planeDist = abs(dot(pT - pC, nC));
        float planeFactor = clamp(1.0 - planeDist / tolerance, 0.0, 1.0);
        float normalFactor = smoothstep(0.5, 0.8, dot(nT, nC));
        sum += nT * (planeFactor * normalFactor * onScreen);
    }
    float len = length(sum);
    vec3 n = len > 1.0e-12 ? sum / len : nC;
    outColor = vec4(n * 0.5 + 0.5, centreSample.a);
}
