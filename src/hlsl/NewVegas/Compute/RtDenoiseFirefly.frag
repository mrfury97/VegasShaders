// UNOFFICIAL 2026-11-07 (ptaaa-denoiser) shared RT denoiser (core\RtDenoise.h), Settings.AntiFirefly (the path tracer's
// PTDenoiseAntiFirefly), after the temporal pass (RGB or specular): the temporal pass clamped each firefly to its neighbours' / its own
// recent support and wrote the excess x its blend weight (t_excess); here every pixel takes 1 / 25 of the excess of the 5 x 5 round
// it is in, into the history (A), and the same of its luma x the fast history's weight into B's fast mean (z): the anti-lag compares
// the long history with the fast one, and with the spread light in the long one only it pulled the long one back down (the light lost).
// Each source's excess lands on the 25 pixels round it once each: its light is kept (in the running mean, as the unclamped sample would
// have added it), as a soft 5 x 5 spread instead of a white dot. (Pixels with no surface take none: their share, and the share past
// the screen's edge, is the only light lost.)
// t_excess: rgb the excess x the long history's blend weight, a its luma x the fast history's weight.
// Compile: glslangValidator -V --target-env vulkan1.1 -o RtDenoiseFirefly.spv RtDenoiseFirefly.frag
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_a;        // the temporal pass' A (rgb, history length)
layout(set = 1, binding = 1) uniform texture2D t_excess;   // its excess (above)
layout(set = 1, binding = 2) uniform texture2D t_b;        // its B (.g view z: 0 = no surface; .b the fast mean)

layout(push_constant) uniform Push {
    vec4 size;     // x 1 / width, y 1 / height
    uint samplerIndex;
    uint pad[3];
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_a;
layout(location = 1) out vec4 o_b;

void main() {
    const vec4 ca = textureLod(sampler2D(t_a, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0);
    const vec4 cb = textureLod(sampler2D(t_b, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0);
    o_b = cb;
    if (cb.g <= 0.0) { o_a = ca; return; }
    vec4 sum = vec4(0.0);
    for (int y = -2; y <= 2; y++)
        for (int x = -2; x <= 2; x++) {
            const vec2 at = uv + vec2(x, y) * pc.size.xy;
            if (any(lessThan(at, vec2(0.0))) || any(greaterThan(at, vec2(1.0)))) continue;   // (the clamping sampler would count an edge texel twice)
            sum += textureLod(sampler2D(t_excess, NVR_SAMPLER(pc.samplerIndex)), at, 0.0);
        }
    o_a = vec4(ca.rgb + sum.rgb * (1.0 / 25.0), ca.a);
    o_b = vec4(cb.r, cb.g, cb.b + sum.a * (1.0 / 25.0), cb.a);
}
