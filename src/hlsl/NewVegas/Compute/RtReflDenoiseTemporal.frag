// UNOFFICIAL 2026-11-07 RT reflection denoiser, pass 4 of 4: the temporal resolve (AMD FidelityFX Reflections Denoiser's
// FFX_DNSR_Reflections_ResolveTemporal, ported; RtReflDenoise.glsl). The reprojected history, clipped to this frame's 9 x 9
// neighbourhood (+ the 8 x 8 average), blended with the prefiltered radiance by the sample count.
// Inputs: t_in0 prefiltered radiance, t_in1 its variance, t_in2 the reprojected history, t_in3 the reproject pass' moments.
// Outputs: 0 the result (= next frame's history), 1 next frame's moments (x variance, y samples).
// Compile: glslangValidator -V --target-env vulkan1.1 -o RtReflDenoiseTemporal.spv RtReflDenoiseTemporal.frag
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "RtReflDenoise.glsl"

layout(location = 0) out vec4 o_result;
layout(location = 1) out vec4 o_moments;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    Surface c = SurfaceAt(p);
    vec4 new_signal = Fetch(t_in0, p);
    float new_variance = Fetch(t_in1, p).x;
    vec4 m = Fetch(t_in3, p);
    o_moments = vec4(new_variance, m.y, 0.0, 0.0);
    if (c.z <= 0.0) { o_result = vec4(0.0); o_moments = vec4(0.0); return; }
    if (IsMirror(c.roughness)) { o_result = Fetch(t_signal, p); return; }   // NVR: mirrors pass through (traced noise-free)

    float num_samples = m.y;
    vec2 uv8 = (vec2(p) + 0.5) / (8.0 * vec2(textureSize(sampler2D(t_avg, POINT_S), 0)));
    vec4 avg_radiance = textureLod(sampler2D(t_avg, LINEAR_S), uv8, 0.0);
    vec4 old_signal = Fetch(t_in2, p);
    vec4 local_mean, local_variance;
    LocalMoments(t_in0, p, local_mean, local_variance);
    // Clip history based on the current local statistics (AMD)
    vec4 color_std = (sqrt(local_variance) + length(local_mean - avg_radiance)) * pc.params.y * 1.4;
    local_mean = mix(local_mean, avg_radiance, 0.2);
    vec4 clipped_old_signal = FFX_ClipAABB(local_mean - color_std, local_mean + color_std, old_signal);
    float accumulation_speed = 1.0 / max(num_samples, 1.0);
    float weight = 1.0 - accumulation_speed;
    // Blend with average for small sample count
    new_signal = mix(new_signal, avg_radiance, 1.0 / max(num_samples + 1.0, 1.0));
    // Clip outliers
    new_signal = FFX_ClipAABB(avg_radiance - color_std, avg_radiance + color_std, new_signal);
    // Blend with history
    new_signal = mix(new_signal, clipped_old_signal, weight);
    new_variance = mix(FFX_ComputeTemporalVariance(new_signal.rgb, clipped_old_signal.rgb), new_variance, weight);
    if (!Finite(new_signal) || isnan(new_variance) || isinf(new_variance)) { new_signal = vec4(0.0); new_variance = 0.0; }
    o_result = new_signal;
    o_moments = vec4(new_variance, num_samples, 0.0, 0.0);
}
