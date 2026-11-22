// Normals' pass as a Vulkan pass (needs NVR DXVK): Effects/Normals.fx.hlsl technique 0 (NormalsPS), line for line.
// Target 0: the normals buffer (A16B16G16R16F), n * 0.5 + 0.5, alpha 1. effects/Normals.cpp ExtRender fills the push block.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"
#include "Normals.glsl"

layout(location = 0) out vec4 outColor;

layout(set = 1, binding = 0) uniform texture2D t_depth;           // CombineDepth's output (R = viewZ / far)
layout(set = 1, binding = 1) uniform texture2D t_capturedNormals; // TESR_CapturedNormalsBuffer (POINT, CLAMP)
layout(set = 1, binding = 2) uniform texture2D t_material;        // MaterialBuffer (POINT, CLAMP)
layout(set = 1, binding = 3) uniform texture2D t_viewModelDepth;  // TESR_DepthBufferViewModel (POINT, CLAMP)

layout(push_constant) uniform Push {
    vec4 reciprocalResolution;	// 0: TESR_ReciprocalResolution
    vec4 cameraData;			// 16: TESR_CameraData (x near, y far)
    mat4 invProjection;			// 32: TESR_InvProjectionTransform
    uint samplerIndex;			// 96: POINT, CLAMP
    uint pad[3];				// 100
    vec4 renderScale;			// 112: (scaleX, scaleY, 1/scaleX, 1/scaleY) (UNOFFICIAL 2026-11-19 RenderRect)
    vec4 mergeTol;              // 128: (Data.y, Data.z, Data.w, 1.0 when active)
    vec4 mergeView;             // 144: (1/P._11, 1/P._22, vmDepth ? 1/ViewModelFovScale() : 0, DepthReversed() ? 1 : 0)
    vec4 mergeRow0;             // 160: (V._11.._13, 1.0 when inline merge active)
    vec4 mergeRow1;             // 176: (V._21.._23, grass 1/0)
    vec4 mergeRow2;             // 192: (V._31.._33, v2Mode 0/1/2)
} pc;

float viewZAt(vec2 uv) {
    return max(textureLod(sampler2D(t_depth, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0).x * pc.cameraData.y, 1.0e-6);
}

void main() {
    vec2 t = pc.reciprocalResolution.xy;
    vec2 scale = pc.renderScale.x > 0.0 ? pc.renderScale.xy : vec2(1.0);
    vec2 invScale = pc.renderScale.z > 0.0 ? pc.renderScale.zw : vec2(1.0);
    vec2 uv = gl_FragCoord.xy * t;
    float zC = viewZAt(uv);

    if (pc.mergeRow0.w > 0.5) {
        vec4 c = textureLod(sampler2D(t_capturedNormals, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0);
        vec4 m = pc.mergeRow1.w > 0.5 ? textureLod(sampler2D(t_material, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0) : vec4(0.0);
        float vmRaw = pc.mergeView.z > 0.0
            ? textureLod(sampler2D(t_viewModelDepth, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0).x
            : (pc.mergeView.w > 0.5 ? 0.0 : 1.0);
        vec3 enc;
        if (nvrTryCapturedNormal(uv * invScale, zC, pc.cameraData.y, c, m, vmRaw,
                pc.mergeTol, pc.mergeView, pc.mergeRow0, pc.mergeRow1, pc.mergeRow2, enc)) {
            outColor = vec4(enc, 1.0);
            return;
        }
    }

    vec2 lo = 0.5 * t;
    vec2 hi = scale - 0.5 * t;
    vec2 uvL1 = clamp(uv - vec2(t.x, 0.0), lo, hi), uvL2 = clamp(uv - vec2(2.0 * t.x, 0.0), lo, hi);
    vec2 uvR1 = clamp(uv + vec2(t.x, 0.0), lo, hi), uvR2 = clamp(uv + vec2(2.0 * t.x, 0.0), lo, hi);
    vec2 uvU1 = clamp(uv - vec2(0.0, t.y), lo, hi), uvU2 = clamp(uv - vec2(0.0, 2.0 * t.y), lo, hi);
    vec2 uvD1 = clamp(uv + vec2(0.0, t.y), lo, hi), uvD2 = clamp(uv + vec2(0.0, 2.0 * t.y), lo, hi);

    vec3 pC = nvrViewPosition(uv * invScale, zC, pc.cameraData, pc.invProjection);
    vec3 pL1 = nvrViewPosition(uvL1 * invScale, viewZAt(uvL1), pc.cameraData, pc.invProjection);
    vec3 pL2 = nvrViewPosition(uvL2 * invScale, viewZAt(uvL2), pc.cameraData, pc.invProjection);
    vec3 pR1 = nvrViewPosition(uvR1 * invScale, viewZAt(uvR1), pc.cameraData, pc.invProjection);
    vec3 pR2 = nvrViewPosition(uvR2 * invScale, viewZAt(uvR2), pc.cameraData, pc.invProjection);
    vec3 pU1 = nvrViewPosition(uvU1 * invScale, viewZAt(uvU1), pc.cameraData, pc.invProjection);
    vec3 pU2 = nvrViewPosition(uvU2 * invScale, viewZAt(uvU2), pc.cameraData, pc.invProjection);
    vec3 pD1 = nvrViewPosition(uvD1 * invScale, viewZAt(uvD1), pc.cameraData, pc.invProjection);
    vec3 pD2 = nvrViewPosition(uvD2 * invScale, viewZAt(uvD2), pc.cameraData, pc.invProjection);

    vec3 n = nvrNormal(pC, pL1, pL2, pR1, pR2, pU1, pU2, pD1, pD2);
    outColor = vec4(n * 0.5 + 0.5, 1.0);
}
