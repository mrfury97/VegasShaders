// UNOFFICIAL 2026-11-13 RT sun shadows, Mode 1 with ForwardLookup ([Shaders.ShadowsExteriors.RT]; core\RtSunShadows.cpp PackForward;
// Shaders\Includes\Shadow.hlsl RT_SUN_FORWARD): the traced (and denoised) sun visibility packed with the frame's view depth into
// TESR_RtSunForward, which the object, skin and land shaders of the NEXT frame read in place of their cascade lookup:
//   red   the visibility (1 the sun reaches the pixel, 0 blocked)
//   green the pixel's view depth in game units (TESR_DepthBuffer's red x farZ; at most 60000: a half float), 0 for the sky
// A reader takes the visibility only where this depth is its own point's depth in this frame's view (the same test as the RT lamp
// shadows' lookup), so a texel is never used for another surface.
// Drawn over the whole target with no blending, red and green written. Both sources are screen size with the frame in their
// top-left RenderScale part (the visibility half size with HalfResolution: then its sampler is linear), so the same uv reads both.
// Compile: glslangValidator -V --target-env vulkan1.1 -I. -o RtSunForward.spv RtSunForward.frag
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_visibility;   // TESR_RtSunShadow (or its denoised / penumbra copy)
layout(set = 1, binding = 1) uniform texture2D t_depth;        // TESR_DepthBuffer

layout(push_constant) uniform Push {
    float farZ;              // 0: the depth buffer's unit (view z / farZ)
    uint visibilitySampler;  // 4: POINT, CLAMP (LINEAR with HalfResolution)
    uint depthSampler;       // 8: POINT, CLAMP
    uint pad;
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

void main() {
    const float visibility = clamp(textureLod(sampler2D(t_visibility, NVR_SAMPLER(pc.visibilitySampler)), uv, 0.0).r, 0.0, 1.0);
    const float linear01 = textureLod(sampler2D(t_depth, NVR_SAMPLER(pc.depthSampler)), uv, 0.0).r;
    const float viewZ = linear01 > 0.0 && linear01 < 0.9999 ? min(linear01 * pc.farZ, 60000.0) : 0.0;
    o_color = vec4(visibility, viewZ, 0.0, 1.0);
}
