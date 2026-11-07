// UNOFFICIAL 2026-11-07 RT reflection denoiser, pass 2 of 4: the 8 x 8 average (AMD FidelityFX Reflections Denoiser's downsample at the
// end of FFX_DNSR_Reflections_Reproject, ported; RtReflDenoise.glsl). One texel per 8 x 8 grid pixels, luminance-weighted (bright
// outliers count less). NVR: pixels that reflect nothing are left out (AMD's sample counts them as black).
// Input: t_in0 the reproject pass' output 2. Target: 1/8 size (rounded up).
// Compile: glslangValidator -V --target-env vulkan1.1 -o RtReflDenoiseAverage.spv RtReflDenoiseAverage.frag
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "RtReflDenoise.glsl"

layout(location = 0) out vec4 o_average;

void main() {
    ivec2 base = ivec2(gl_FragCoord.xy) * 8;
    vec4 sum = vec4(0.0);
    float weights = 0.0;
    for (int j = 0; j < 8; j++)
        for (int i = 0; i < 8; i++) {
            ivec2 q = base + ivec2(i, j);
            if (any(greaterThanEqual(q, GRID)) || Fetch(t_ray, q).x <= 0.0) continue;
            vec4 radiance = Fetch(t_in0, q);
            float weight = max(exp(-FFX_Luminance(radiance.rgb) * FFX_DNSR_REFLECTIONS_AVG_RADIANCE_LUMINANCE_WEIGHT), 1.0e-2);
            if (!Finite(radiance) || weight > 1.0e3) continue;
            sum += radiance * weight;
            weights += weight;
        }
    o_average = sum / max(weights, 1.0e-3);
}
