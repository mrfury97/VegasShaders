// UNOFFICIAL 2026-11-07 RT reflection denoiser, pass 3 of 4: prefilter (AMD FidelityFX Reflections Denoiser's
// FFX_DNSR_Reflections_Prefilter + Resolve, ported; RtReflDenoise.glsl). 15 Halton(2,3) taps in +-3 grid pixels, weighted by the
// normal, the depth, the radiance against the 8 x 8 average and the variance. Mirrors and pixels without variance pass through.
// Input: t_in0 the reproject pass' moments (x variance). Outputs: 0 the filtered radiance, 1 x its variance.
// Compile: glslangValidator -V --target-env vulkan1.1 -o RtReflDenoisePrefilter.spv RtReflDenoisePrefilter.frag
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "RtReflDenoise.glsl"

layout(location = 0) out vec4 o_radiance;
layout(location = 1) out vec4 o_variance;

float GetEdgeStoppingNormalWeight(vec3 normal_p, vec3 normal_q) { return pow(max(dot(normal_p, normal_q), 0.0), FFX_DNSR_REFLECTIONS_PREFILTER_NORMAL_SIGMA); }
// NVR: AMD's depth term works on the hardware depth (0..1); on linear view z it becomes a relative difference
float GetEdgeStoppingDepthWeight(float center_depth, float neighbor_depth) {
    return exp(-abs(center_depth - neighbor_depth) / max(center_depth, 1.0e-3) * FFX_DNSR_REFLECTIONS_PREFILTER_DEPTH_SIGMA * 8.0);
}
float GetRadianceWeight(vec4 center_radiance, vec4 neighbor_radiance, float variance) {
    return max(exp(-(FFX_DNSR_REFLECTIONS_RADIANCE_WEIGHT_BIAS + variance * FFX_DNSR_REFLECTIONS_RADIANCE_WEIGHT_VARIANCE_K)
                   * length(center_radiance.rgb - neighbor_radiance.rgb)), 1.0e-2);
}

const ivec2 sample_offsets[15] = ivec2[](ivec2(0, 1), ivec2(-2, 1), ivec2(2, -3), ivec2(-3, 0), ivec2(1, 2), ivec2(-1, -2), ivec2(3, 0), ivec2(-3, 3),
                                         ivec2(0, -3), ivec2(-1, -1), ivec2(2, 1), ivec2(-2, -2), ivec2(1, 0), ivec2(0, 2), ivec2(3, -1));

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    Surface c = SurfaceAt(p);
    vec4 radiance = Fetch(t_signal, p);
    if (!Finite(radiance)) radiance = vec4(0.0);
    float variance = Fetch(t_in0, p).x;
    o_radiance = radiance;
    o_variance = vec4(variance, 0.0, 0.0, 0.0);
    if (c.z <= 0.0 || !(variance > 0.0) || IsMirror(c.roughness)) return;

    vec2 uv8 = (vec2(p) + 0.5) / (8.0 * vec2(textureSize(sampler2D(t_avg, POINT_S), 0)));
    vec4 avg_radiance = textureLod(sampler2D(t_avg, LINEAR_S), uv8, 0.0);
    // Initial weight is important to remove fireflies (AMD)
    float accumulated_weight = GetRadianceWeight(avg_radiance, radiance, variance);
    vec4 accumulated_radiance = radiance * accumulated_weight;
    float accumulated_variance = variance * accumulated_weight * accumulated_weight;
    float variance_weight = max(FFX_DNSR_REFLECTIONS_PREFILTER_VARIANCE_BIAS, 1.0 - exp(-(variance * FFX_DNSR_REFLECTIONS_PREFILTER_VARIANCE_WEIGHT)));
    for (int i = 0; i < 15; ++i) {
        ivec2 q = p + sample_offsets[i];
        if (any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, GRID))) continue;
        Surface n = SurfaceAt(q);
        if (n.z <= 0.0) continue;   // NVR: nothing reflects there
        vec4 nr = Fetch(t_signal, q);
        if (!Finite(nr)) continue;
        float weight = GetEdgeStoppingNormalWeight(c.n, n.n) * GetEdgeStoppingDepthWeight(c.z, n.z)
                     * GetRadianceWeight(avg_radiance, nr, variance) * variance_weight;
        accumulated_weight += weight;
        accumulated_radiance += weight * nr;
        accumulated_variance += weight * weight * Fetch(t_in0, q).x;
    }
    o_radiance = accumulated_radiance / accumulated_weight;
    o_variance = vec4(accumulated_variance / (accumulated_weight * accumulated_weight), 0.0, 0.0, 0.0);
}
