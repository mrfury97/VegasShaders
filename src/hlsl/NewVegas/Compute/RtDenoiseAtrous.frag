// UNOFFICIAL 2026-10-29 shared RT denoiser (core\RtDenoise.h), pass 2 of 2, run 3 times (step 1, 2, 4): an edge-stopping a-trous
// wavelet (SVGF): a 5 x 5 B3 kernel at this step, each tap weighted by depth (relative to the local depth slope), the view normal and
// the value's difference against its temporal standard deviation (from the moments; fresh history gets a wider window).
// In / out RGBA16F: r = value, g = second moment, b = history length, a = linear view depth (the chain's layout); the last iteration
// may write straight into the caller's R16F target (.r = the value).
// Compile: glslangValidator -V --target-env vulkan1.1 -o RtDenoiseAtrous.spv RtDenoiseAtrous.frag
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_in;
layout(set = 1, binding = 1) uniform texture2D t_normals;   // TESR_NormalsBuffer: view-space normal * 0.5 + 0.5
layout(set = 1, binding = 2) uniform texture2D t_ids;       // Settings.Ids (.r: a source id, e.g. the lamp a pixel's signal is from), or a stand-in

layout(push_constant) uniform Push {
    vec4 params;   // x step (texels), y depth sigma, z normal power, w value sigma
    vec4 size;     // x 1 / width, y 1 / height, z 1: only taps with the centre's id (t_ids) count, w 1: the 3 x 3 kernel
    uint samplerIndex;
    uint pad[3];
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

const float kernel[3] = float[3](3.0 / 8.0, 1.0 / 4.0, 1.0 / 16.0);
const float kernel3[2] = float[2](1.0 / 2.0, 1.0 / 4.0);   // the 3 x 3 kernel (Settings.Kernel 3): 9 taps instead of 25

void main() {
    const vec4 center = textureLod(sampler2D(t_in, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0);
    const vec3 n = normalize(textureLod(sampler2D(t_normals, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0).xyz * 2.0 - 1.0);
    const float z = center.a;
    const float id = pc.size.z > 0.5 ? textureLod(sampler2D(t_ids, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0).r : 0.0;
    if (z <= 0.0) { o_color = center; return; }
    const float variance = max(center.g - center.r * center.r, 0.0);
    // little history: trust the neighbours more (the variance estimate is not there yet)
    const float valueSigma = pc.params.w * sqrt(variance + (center.b < 4.0 ? 0.05 : 0.0)) + 1.0e-3;
    const float rawSlope = max(abs(dFdx(z)), abs(dFdy(z)));
    const float zSlope = min(rawSlope, 0.05 * z) * pc.params.x + 1.0e-2 * z;

    vec4 sum = vec4(0.0);
    float weights = 0.0;
    const int reach = pc.size.w > 0.5 ? 1 : 2;
    for (int y = -reach; y <= reach; y++) {
        for (int x = -reach; x <= reach; x++) {
            const vec2 tapUv = uv + vec2(x, y) * pc.params.x * pc.size.xy;
            const vec4 tap = textureLod(sampler2D(t_in, NVR_SAMPLER(pc.samplerIndex)), tapUv, 0.0);
            if (tap.a <= 0.0 || abs(tap.a - z) > max(0.15 * z, 12.0)) continue;
            if (pc.size.z > 0.5 && abs(textureLod(sampler2D(t_ids, NVR_SAMPLER(pc.samplerIndex)), tapUv, 0.0).r - id) > 0.5) continue;
            const vec3 tn = normalize(textureLod(sampler2D(t_normals, NVR_SAMPLER(pc.samplerIndex)), tapUv, 0.0).xyz * 2.0 - 1.0);
            float w = reach == 1 ? kernel3[abs(x)] * kernel3[abs(y)] : kernel[abs(x)] * kernel[abs(y)];
            w *= exp(-abs(tap.a - z) / (pc.params.y * zSlope * length(vec2(x, y)) + 1.0e-3));
            w *= pow(max(dot(n, tn), 0.0), pc.params.z);
            w *= exp(-abs(tap.r - center.r) / valueSigma);
            sum += vec4(tap.r, tap.g, 0.0, 0.0) * w;
            weights += w;
        }
    }
    const vec2 filtered = weights > 0.0 ? sum.rg / weights : center.rg;
    o_color = vec4(filtered.x, filtered.y, center.b, center.a);
}
