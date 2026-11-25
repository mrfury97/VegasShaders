// CombineDepth's merged pass as a Vulkan pass (needs NVR DXVK): Effects/CombineDepth.fx.hlsl technique DepthNormals, line for line.
// Two targets: 0 the combined depth (R32F or G32R32F, as CombineDepth), 1 the normals (A16B16G16R16F, as Normals). Normals.glsl has
// the normal maths. effects/CombineDepth.cpp ExtDepthNormals (MergedDepthNormals, off by default).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"
#include "Normals.glsl"

layout(set = 1, binding = 0) uniform texture2D t_worldDepth;      // TESR_DepthBufferWorld (POINT, CLAMP)
layout(set = 1, binding = 1) uniform texture2D t_viewModelDepth;  // TESR_DepthBufferViewModel (POINT, CLAMP)
layout(set = 1, binding = 2) uniform texture2D t_capturedNormals; // TESR_CapturedNormalsBuffer (POINT, CLAMP)
layout(set = 1, binding = 3) uniform texture2D t_material;        // MaterialBuffer (POINT, CLAMP)

layout(push_constant) uniform Push {
    vec4 reciprocalResolution;  // 0: TESR_ReciprocalResolution
    vec4 cameraData;            // 16: TESR_CameraData (x near Z, y far Z)
    vec4 depthConstants;        // 32: TESR_DepthConstants (x view model near Z, z 1 = reversed)
    mat4 invProjection;         // 48: TESR_InvProjectionTransform
    uint samplerIndex;          // 112: POINT, CLAMP
    uint pad[3];                // 116
    vec4 renderScale;           // 128: (scaleX, scaleY, 1/scaleX, 1/scaleY) (UNOFFICIAL 2026-11-19 RenderRect)
    vec4 mergeTol;              // 144: (Data.y, Data.z, Data.w, 1.0 when active)
    vec4 mergeView;             // 160: (1/P._11, 1/P._22, vmDepth ? 1/ViewModelFovScale() : 0, DepthReversed() ? 1 : 0)
    vec4 mergeRow0;             // 176: (V._11.._13, 1.0 when inline merge active)
    vec4 mergeRow1;             // 192: (V._21.._23, grass 1/0)
    vec4 mergeRow2;             // 208: (V._31.._33, v2Mode 0/1/2)
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_depth;
layout(location = 1) out vec4 o_normal;

// Convert raw hardware depth to view space Z (handles reversed-Z and standard-Z like CombineDepth.frag).
float viewZFromRaw(float raw, float nearZ, float farZ) {
    float span = farZ - nearZ;
    float viewZ = pc.depthConstants.z > 0.5 ?
        nearZ * farZ / (nearZ + raw * span) :
        nearZ * farZ / (farZ - raw * span);
    return max(viewZ, 1.0e-6);
}

// Convert view space Z to post-projection depth.
float toPS(float viewZ, float nearZ, float farZ) {
    return nearZ * (farZ - viewZ) / (viewZ * (farZ - nearZ));
}

// CombineDepth.fx.hlsl's CombinedDepthAt: (linear01, post-projection depth) of the combined depth at a point.
vec2 combinedDepthFromRaw(float worldDepth, float viewModelDepth) {
    float nearZ = pc.cameraData.x;
    float farZ = pc.cameraData.y;
    float combinedViewZ = viewZFromRaw(worldDepth, nearZ, farZ);
    if (viewModelDepth > 0.0)
        combinedViewZ = viewZFromRaw(viewModelDepth, max(pc.depthConstants.x, 1.0e-4), farZ);
    return vec2(combinedViewZ / farZ, toPS(combinedViewZ, nearZ, farZ));
}

vec2 combinedDepthAt(vec2 p) {
    float worldDepth = textureLod(sampler2D(t_worldDepth, NVR_SAMPLER(pc.samplerIndex)), p, 0.0).x;
    float viewModelDepth = textureLod(sampler2D(t_viewModelDepth, NVR_SAMPLER(pc.samplerIndex)), p, 0.0).x;
    return combinedDepthFromRaw(worldDepth, viewModelDepth);
}

void main() {
    vec2 texel = pc.reciprocalResolution.xy;
    float farZ = pc.cameraData.y;
    vec2 scale = pc.renderScale.x > 0.0 ? pc.renderScale.xy : vec2(1.0);
    vec2 invScale = pc.renderScale.z > 0.0 ? pc.renderScale.zw : vec2(1.0);
    vec2 ruv = uv * scale;
    vec2 lo = 0.5 * texel;
    vec2 hi = scale - 0.5 * texel;
    vec2 crv = clamp(ruv, lo, hi);

    float worldDepth = textureLod(sampler2D(t_worldDepth, NVR_SAMPLER(pc.samplerIndex)), crv, 0.0).x;
    float vmRaw = textureLod(sampler2D(t_viewModelDepth, NVR_SAMPLER(pc.samplerIndex)), crv, 0.0).x;
    vec2 centerDepth = combinedDepthFromRaw(worldDepth, vmRaw);
    o_depth = vec4(centerDepth.x, centerDepth.y, 1.0, 1.0);

    if (pc.mergeRow0.w > 0.5) {
        vec4 c = textureLod(sampler2D(t_capturedNormals, NVR_SAMPLER(pc.samplerIndex)), crv, 0.0);
        vec4 m = pc.mergeRow1.w > 0.5 ? textureLod(sampler2D(t_material, NVR_SAMPLER(pc.samplerIndex)), crv, 0.0) : vec4(0.0);
        vec3 enc;
        if (nvrTryCapturedNormal(uv, centerDepth.x * farZ, farZ, c, m, vmRaw,
                pc.mergeTol, pc.mergeView, pc.mergeRow0, pc.mergeRow1, pc.mergeRow2, enc)) {
            o_normal = vec4(enc, 1.0);
            return;
        }
    }

    vec3 viewNormal = viewNormalFromTaps(uv, texel * invScale, pc.invProjection, farZ,
        centerDepth,
        combinedDepthAt(clamp(ruv + vec2(1.0, 0.0) * texel, lo, hi)), combinedDepthAt(clamp(ruv + vec2(2.0, 0.0) * texel, lo, hi)),
        combinedDepthAt(clamp(ruv + vec2(-1.0, 0.0) * texel, lo, hi)), combinedDepthAt(clamp(ruv + vec2(-2.0, 0.0) * texel, lo, hi)),
        combinedDepthAt(clamp(ruv + vec2(0.0, -1.0) * texel, lo, hi)), combinedDepthAt(clamp(ruv + vec2(0.0, -2.0) * texel, lo, hi)),
        combinedDepthAt(clamp(ruv + vec2(0.0, 1.0) * texel, lo, hi)), combinedDepthAt(clamp(ruv + vec2(0.0, 2.0) * texel, lo, hi)));

    o_normal = vec4(viewNormal * 0.5 + 0.5, 1.0);
}
