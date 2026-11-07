// UNOFFICIAL 2026-11-07 RT reflection denoiser, pass 1 of 4: reproject (AMD FidelityFX Reflections Denoiser's
// FFX_DNSR_Reflections_Reproject + PickReprojection, ported; RtReflDenoise.glsl has AMD's notice and the NVR changes).
// Last frame's result is found where the REFLECTED thing was (the hit point seen "through" the mirror: the view ray lengthened by
// the hit distance, reprojected), or where the surface was, whichever matches this pixel better; a disocclusion drops it.
// Inputs: t_in0 last frame's result, t_in1 last frame's moments (x variance, y samples), t_in2 last frame's guide.
// Outputs: 0 the reprojected history (rgba), 1 moments (x variance, y samples, z disocclusion, w 1 = the hit point was used),
//          2 the radiance for the 8 x 8 average, 3 this frame's guide (x view z, y roughness, zw the world normal, octahedral).
// Compile: glslangValidator -V --target-env vulkan1.1 -o RtReflDenoiseReproject.spv RtReflDenoiseReproject.frag
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "RtReflDenoise.glsl"

layout(location = 0) out vec4 o_reprojected;
layout(location = 1) out vec4 o_moments;
layout(location = 2) out vec4 o_average;
layout(location = 3) out vec4 o_guide;

vec4 SampleRadianceHistory(vec2 uv) { return textureLod(sampler2D(t_in0, LINEAR_S), uv, 0.0); }
vec4 SampleMomentsHistory(vec2 uv) { return textureLod(sampler2D(t_in1, LINEAR_S), uv, 0.0); }
// (NVR: the guide is RGBA32F, read point-sampled: its nearest texel)
vec4 GuideHistory(vec2 uv) { return textureLod(sampler2D(t_in2, POINT_S), uv, 0.0); }
vec4 GuideHistoryAt(ivec2 p) { return Fetch(t_in2, p); }
vec3 HistoryNormal(vec4 g) { return OctDecodeN(g.zw); }

float GetDisocclusionFactor(vec3 normal, vec3 history_normal, float linear_depth, float history_linear_depth) {
    return exp(-abs(1.0 - max(0.0, dot(normal, history_normal))) * FFX_DNSR_REFLECTIONS_DISOCCLUSION_NORMAL_WEIGHT)
         * exp(-abs(history_linear_depth - linear_depth) / max(linear_depth, 1.0e-3) * FFX_DNSR_REFLECTIONS_DISOCCLUSION_DEPTH_WEIGHT);
}

float dot2(vec4 a) { return dot(a, a); }

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec2 uv = (vec2(p) + 0.5) * pc.size.zw;
    Surface s = SurfaceAt(p);
    vec4 radiance = Fetch(t_signal, p);
    if (!Finite(radiance)) radiance = vec4(0.0);
    o_guide = vec4(s.z, s.roughness, OctEncodeN(s.n));
    o_reprojected = vec4(0.0);
    o_moments = vec4(1.0, 1.0, 0.0, 0.0);
    o_average = radiance;
    if (s.z <= 0.0) { o_guide = vec4(0.0); o_moments = vec4(0.0); o_average = vec4(0.0); return; }
    if (IsMirror(s.roughness) || !HistoryValid) return;

    float ray_length = Fetch(t_hitDist, p).y;
    vec4 local_mean, local_variance;
    LocalMoments(t_signal, p, local_mean, local_variance);

    // NVR: no motion vectors in this game: the surface's point through the camera's reprojection (and the object's own move)
    vec3 point = PointAt(uv, s.z);
    vec3 surfaceLast = ToLast(point - ObjectMotionAt(uv));
    vec2 surface_reprojection_uv = s.weapon ? uv : surfaceLast.xy;   // (the weapon moves with the camera)
    // AMD's GetHitPositionReprojection: the view ray lengthened by the hit distance ("straight through" the surface), reprojected
    float surface_depth = length(point);
    vec3 hitLast = ToLast(point / surface_depth * (surface_depth + max(ray_length, 0.0)));
    vec2 hit_reprojection_uv = hitLast.xy;

    vec4 sg = GuideHistory(surface_reprojection_uv), hg = GuideHistory(hit_reprojection_uv);
    vec3 surface_normal = HistoryNormal(sg), hit_normal = HistoryNormal(hg);
    vec4 surface_history = SampleRadianceHistory(surface_reprojection_uv), hit_history = SampleRadianceHistory(hit_reprojection_uv);
    float hit_normal_similarity = dot(hit_normal, s.n);
    float surface_normal_similarity = dot(surface_normal, s.n);
    float hit_roughness = hg.y, surface_roughness = sg.y;

    float disocclusion_factor;
    vec2 reprojection_uv;
    vec4 reprojection;
    vec3 history_normal;
    float history_linear_depth, expected_depth;
    bool hitPicked = false;
    if (!s.weapon && ray_length > 0.0
        && hit_normal_similarity > FFX_DNSR_REFLECTIONS_REPROJECTION_NORMAL_SIMILARITY_THRESHOLD
        && hit_normal_similarity + 1.0e-3 > surface_normal_similarity
        && abs(hit_roughness - s.roughness) < abs(surface_roughness - s.roughness) + 1.0e-3) {
        history_normal = hit_normal;
        history_linear_depth = hg.x;
        expected_depth = s.z;   // (AMD: the depth now)
        reprojection_uv = hit_reprojection_uv;
        reprojection = hit_history;
        hitPicked = true;
    } else if (dot2(surface_history - local_mean) < FFX_DNSR_REFLECTIONS_REPROJECT_SURFACE_DISCARD_VARIANCE_WEIGHT * length(local_variance)) {
        history_normal = surface_normal;
        history_linear_depth = sg.x;
        expected_depth = s.weapon ? s.z : surfaceLast.z;   // NVR: the point's view z in last frame's camera (AMD: the depth now)
        reprojection_uv = surface_reprojection_uv;
        reprojection = surface_history;
    } else {
        o_moments = vec4(1.0, 1.0, 0.0, 0.0);
        return;
    }
    disocclusion_factor = GetDisocclusionFactor(s.n, history_normal, expected_depth, history_linear_depth);

    if (disocclusion_factor < FFX_DNSR_REFLECTIONS_DISOCCLUSION_THRESHOLD) {
        // the closest sample in the 3 x 3 vicinity
        vec2 dudv = pc.size.zw;
        vec2 best = reprojection_uv;
        for (int y = -1; y <= 1; y++)
            for (int x = -1; x <= 1; x++) {
                vec2 u = reprojection_uv + vec2(x, y) * dudv;
                vec4 g = GuideHistory(u);
                float w = GetDisocclusionFactor(s.n, HistoryNormal(g), expected_depth, g.x);
                if (w > disocclusion_factor) { disocclusion_factor = w; best = u; }
            }
        reprojection_uv = best;
        reprojection = SampleRadianceHistory(reprojection_uv);
    }
    if (disocclusion_factor < FFX_DNSR_REFLECTIONS_DISOCCLUSION_THRESHOLD) {
        // the 2 x 2 bilinear neighbourhood, texels of another surface left out
        float uvx = fract(pc.size.x * reprojection_uv.x + 0.5);
        float uvy = fract(pc.size.y * reprojection_uv.y + 0.5);
        ivec2 t = ivec2(floor(pc.size.xy * reprojection_uv - 0.5));
        vec4 g00 = GuideHistoryAt(t), g10 = GuideHistoryAt(t + ivec2(1, 0)), g01 = GuideHistoryAt(t + ivec2(0, 1)), g11 = GuideHistoryAt(t + ivec2(1, 1));
        vec4 w;
        w.x = GetDisocclusionFactor(s.n, HistoryNormal(g00), expected_depth, g00.x) > FFX_DNSR_REFLECTIONS_DISOCCLUSION_THRESHOLD / 2.0 ? 1.0 : 0.0;
        w.y = GetDisocclusionFactor(s.n, HistoryNormal(g10), expected_depth, g10.x) > FFX_DNSR_REFLECTIONS_DISOCCLUSION_THRESHOLD / 2.0 ? 1.0 : 0.0;
        w.z = GetDisocclusionFactor(s.n, HistoryNormal(g01), expected_depth, g01.x) > FFX_DNSR_REFLECTIONS_DISOCCLUSION_THRESHOLD / 2.0 ? 1.0 : 0.0;
        w.w = GetDisocclusionFactor(s.n, HistoryNormal(g11), expected_depth, g11.x) > FFX_DNSR_REFLECTIONS_DISOCCLUSION_THRESHOLD / 2.0 ? 1.0 : 0.0;
        w *= vec4((1.0 - uvx) * (1.0 - uvy), uvx * (1.0 - uvy), (1.0 - uvx) * uvy, uvx * uvy);
        float ws = max(w.x + w.y + w.z + w.w, 1.0e-3);
        w /= ws;
        reprojection = Fetch(t_in0, t) * w.x + Fetch(t_in0, t + ivec2(1, 0)) * w.y + Fetch(t_in0, t + ivec2(0, 1)) * w.z + Fetch(t_in0, t + ivec2(1, 1)) * w.w;
        history_linear_depth = g00.x * w.x + g10.x * w.y + g01.x * w.z + g11.x * w.w;
        history_normal = HistoryNormal(g00) * w.x + HistoryNormal(g10) * w.y + HistoryNormal(g01) * w.z + HistoryNormal(g11) * w.w;
        disocclusion_factor = GetDisocclusionFactor(s.n, history_normal, expected_depth, history_linear_depth);
    }
    disocclusion_factor = disocclusion_factor < FFX_DNSR_REFLECTIONS_DISOCCLUSION_THRESHOLD ? 0.0 : disocclusion_factor;

    if (all(greaterThan(reprojection_uv, vec2(0.0))) && all(lessThan(reprojection_uv, vec2(1.0))) && Finite(reprojection)) {
        vec4 hm = SampleMomentsHistory(reprojection_uv);
        float prev_variance = hm.x;
        float num_samples = hm.y * disocclusion_factor;
        float s_max_samples = max(8.0, pc.params.x * FFX_DNSR_REFLECTIONS_SAMPLES_FOR_ROUGHNESS(s.roughness));
        num_samples = min(s_max_samples, num_samples + 1.0);
        float new_variance = FFX_ComputeTemporalVariance(radiance.rgb, reprojection.rgb);
        if (disocclusion_factor < FFX_DNSR_REFLECTIONS_DISOCCLUSION_THRESHOLD) {
            o_reprojected = vec4(0.0);
            o_moments = vec4(1.0, 1.0, disocclusion_factor, hitPicked ? 1.0 : 0.0);
        } else {
            float variance_mix = mix(new_variance, prev_variance, 1.0 / num_samples);
            o_reprojected = reprojection;
            o_moments = vec4(variance_mix, num_samples, disocclusion_factor, hitPicked ? 1.0 : 0.0);
            o_average = mix(radiance, reprojection, 0.3);   // AMD: the reprojection mixed in for the average
        }
    }
}
