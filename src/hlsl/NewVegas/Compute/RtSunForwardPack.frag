// UNOFFICIAL 2026-11-14 (rt-shadows F1) the denoised RT sun visibility into the forward contact texture (TESR_ContactForward, core/BendContact.h)
// for NEXT frame's lit shaders (Shaders/Includes/Shadow.hlsl SunForwardLookup): drawn with only green and blue written (ColorWriteMask
// 0x6), so Bend's red (its contact visibility, or 1 where it did not run) stays. g = the pixel's view depth (units: the lit shaders
// compare their point's depth in this frame with it), b = 1 + visibility (1 shadowed .. 2 lit), 0 = no traced value here (sky, past
// the RT range, outside the traced rect): the cascades there. core/RtSunShadows.cpp PackForward.
// Compile: glslangValidator -V --target-env vulkan1.1 -o RtSunForwardPack.spv RtSunForwardPack.frag
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_visibility;   // the denoised visibility (traced size; top-left rect in a RenderScale frame)
layout(set = 1, binding = 1) uniform texture2D t_depth;        // TESR_DepthBuffer: view z / farZ (screen size)

layout(push_constant) uniform Push {
    vec4 params;         // x farZ, y Range (view units: no traced value past it), z uv scale x (traced rect / visibility texture), w uv scale y
    vec4 rect;           // xy the rect's share of the screen (RenderScale: 1 native), z strength (0..1), w unused
    uint samplerIndex;   // 32: POINT (native) or LINEAR (half resolution), CLAMP
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

void main() {
    const float linear01 = textureLod(sampler2D(t_depth, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0).r;
    const float viewZ = linear01 * pc.params.x;
    // the screen uv in the rect: a RenderScale frame draws the world into the top-left rect of every screen-size buffer
    const bool insideRect = uv.x <= pc.rect.x && uv.y <= pc.rect.y;
    const bool valid = insideRect && linear01 > 0.0 && linear01 < 0.9999 && viewZ < pc.params.y;
    float b = 0.0;
    if (valid) {
        const vec2 vuv = (uv / pc.rect.xy) * pc.params.zw;
        const float v = clamp(textureLod(sampler2D(t_visibility, NVR_SAMPLER(pc.samplerIndex)), vuv, 0.0).r, 0.0, 1.0);
        b = 1.0 + mix(1.0, v, clamp(pc.rect.z, 0.0, 1.0));
    }
    o_color = vec4(1.0, valid ? viewZ : 0.0, b, 0.0);
}
