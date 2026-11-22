// CombineDepth's separate pass as a Vulkan pass (needs NVR DXVK): Effects/CombineDepth.fx.hlsl technique 0, line for line.
// Output (target 0, R32F or G32R32F): R = viewZ / far, G = the reversed-Z depth from viewZ with the world near plane.
// effects/CombineDepth.cpp ExtRender fills the push block. No RenderScale rect clamp here (the D3D9 pass has one).
#version 450
#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_samplerless_texture_functions : enable
#include "Common.glsl"

layout(location = 0) out vec4 outColor;

layout(set = 1, binding = 0) uniform texture2D t_depthWorld;
layout(set = 1, binding = 1) uniform texture2D t_depthViewModel;

layout(push_constant) uniform Push {
    vec4 depthConstants;	// 0: TESR_DepthConstants (x view-model near, z 1 = reversed, w 1 = 2x2 plane-aware upsample)
    vec4 cameraData;		// 16: TESR_CameraData (x near, y far)
    uint samplerIndex;		// 32: POINT, CLAMP
    uint pad[3];
    vec4 upscaleRect;		// 48: xy = renderW, renderH; zw = worldJitterX, worldJitterY
    vec4 upscaleVm;			// 64: xy = vmJitterX, vmJitterY
} pc;

float viewZFromRaw(float raw, float nearZ, float farZ) {
    float span = farZ - nearZ;
    float viewZ = pc.depthConstants.z > 0.5 ?
        nearZ * farZ / (nearZ + raw * span) :
        nearZ * farZ / (farZ - raw * span);
    return max(viewZ, 1.0e-6);
}

// 2x2 plane-aware depth upsample from internal render-rect texels (mirrors TAAUpscale.cpp CompositeSource).
float upsampleRaw(texture2D tex, vec2 uv, vec2 renderSize, vec2 jitter, float nearZ, float farZ) {
    vec2 last = max(renderSize - 1.0, vec2(0.0));
    vec2 t = uv * renderSize + jitter - 0.5;
    vec2 i0 = floor(t);
    vec2 f = t - i0;
    ivec2 a = ivec2(clamp(i0, vec2(0.0), last));
    ivec2 b = ivec2(clamp(i0 + 1.0, vec2(0.0), last));
    float d00 = texelFetch(tex, ivec2(a.x, a.y), 0).x;
    float d10 = texelFetch(tex, ivec2(b.x, a.y), 0).x;
    float d01 = texelFetch(tex, ivec2(a.x, b.y), 0).x;
    float d11 = texelFetch(tex, ivec2(b.x, b.y), 0).x;
    float z00 = viewZFromRaw(d00, nearZ, farZ);
    float z10 = viewZFromRaw(d10, nearZ, farZ);
    float z01 = viewZFromRaw(d01, nearZ, farZ);
    float z11 = viewZFromRaw(d11, nearZ, farZ);
    float zMin = min(min(z00, z10), min(z01, z11));
    float spread = (max(max(z00, z10), max(z01, z11)) - zMin) / max(zMin, 1.0e-3);
    float bend = abs(d00 - d10 - d01 + d11);
    float slope = max(max(abs(d10 - d00), abs(d01 - d00)), 1.0e-7);
    if (spread < 0.02 || (spread < 0.1 && bend <= 0.25 * slope))
        return mix(mix(d00, d10, f.x), mix(d01, d11, f.x), f.y);
    return f.y < 0.5 ? (f.x < 0.5 ? d00 : d10) : (f.x < 0.5 ? d01 : d11);
}

void main() {
    vec2 uv = gl_FragCoord.xy / vec2(textureSize(t_depthWorld, 0));
    float nearZ = pc.cameraData.x;
    float farZ = pc.cameraData.y;
    float vmNearZ = max(pc.depthConstants.x, 1.0e-4);

    float worldRaw;
    float viewRaw;
    if (pc.depthConstants.w > 0.5) {
        worldRaw = upsampleRaw(t_depthWorld, uv, pc.upscaleRect.xy, pc.upscaleRect.zw, nearZ, farZ);
        viewRaw = upsampleRaw(t_depthViewModel, uv, pc.upscaleRect.xy, pc.upscaleVm.xy, vmNearZ, farZ);
    } else {
        worldRaw = texture(sampler2D(t_depthWorld, NVR_SAMPLER(pc.samplerIndex)), uv).x;
        viewRaw = texture(sampler2D(t_depthViewModel, NVR_SAMPLER(pc.samplerIndex)), uv).x;
    }

    float viewZ = viewZFromRaw(worldRaw, nearZ, farZ);
    if (pc.depthConstants.z > 0.5 ? viewRaw > 0.0 : viewRaw < 0.99999)
        viewZ = viewZFromRaw(viewRaw, vmNearZ, farZ);

    float g = nearZ * (farZ - viewZ) / (viewZ * (farZ - nearZ));
    outColor = vec4(viewZ / farZ, g, 1.0, 1.0);
}
