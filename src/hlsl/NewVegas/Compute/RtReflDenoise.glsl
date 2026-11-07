// UNOFFICIAL 2026-11-07 RT reflection denoiser ([Shaders.Reflections.RT] Denoiser; docs/next/RT-REFLECTION-DENOISER.md, core/RtReflDenoise.h).
// What the four RtReflDenoise*.frag passes share: bindings, the push block, the surface and reprojection helpers.
//
// Core 1 ("FFX") is a GLSL port of AMD's FidelityFX Reflections Denoiser (GPUOpen-Effects/FidelityFX-Denoiser d7dfecba,
// ffx-reflection-dnsr/*.h): reproject (hit-point vs surface reprojection, disocclusion), 8 x 8 average, prefilter (15 Halton taps),
// temporal resolve (AABB clip). Its maths is AMD's, line for line where it can be; the lines that had to change for this game are
// marked "NVR:" (no motion vectors: our camera reprojection; linear view z instead of the hardware depth; the signal's alpha, the
// confidence, carried with the colour; pixels that reflect nothing kept out of every average). Groupshared tiles became plain
// texture reads (a fragment pass). Core 0 ("ours", RtReflDenoise*.frag without FFX) is our own, from the published ReLAX / ReBLUR designs.
//
// AMD's notice (ffx/LICENSE-FidelityFX-Denoiser.txt next to this file):
// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files
// (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge,
// publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do
// so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE
// FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
// WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
// (ClipAABB below is AMD's copy of Playdead's, MIT, Copyright (c) 2015 Playdead, the same permission notice.)

#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_signal;   // Resolve's output: rgb the reflection x confidence, a the confidence (half size)
layout(set = 1, binding = 1) uniform texture2D t_ray;      // RT reflections' Ray image: x the surface's view z (0: nothing reflects), y roughness (+2/+4/+6 flags)
layout(set = 1, binding = 2) uniform texture2D t_hitDist;  // Resolve's Spread image: y the hit distance (world units; 0 unknown, Distance = nothing hit)
layout(set = 1, binding = 3) uniform texture2D t_normals;  // TESR_NormalsBuffer (full size, view-space normal x 0.5 + 0.5)
layout(set = 1, binding = 4) uniform texture2D t_motion;   // TESR_ObjectMotionBuffer (full size, rgb the surface's world move) or a stand-in
layout(set = 1, binding = 5) uniform texture2D t_in0;      // per stage (RtReflDenoise.h): see each pass
layout(set = 1, binding = 6) uniform texture2D t_in1;
layout(set = 1, binding = 7) uniform texture2D t_in2;
layout(set = 1, binding = 8) uniform texture2D t_in3;
layout(set = 1, binding = 9) uniform texture2D t_avg;      // the 8 x 8 average (1/8 size)

layout(push_constant) uniform Push {
    mat4 reproject;     // camera-relative world now -> last frame's clip space (Reflections' TESR_ReflectionsReproject)
    vec4 forward;       // xyz the camera's forward (world axes), w farZ
    vec4 right;         // xyz right / P00, w 1: t_motion holds ObjectMotion
    vec4 up;            // xyz up / P11
    vec4 size;          // the grid: width, height, 1 / width, 1 / height
    vec4 fullUv;        // xy: added to a grid pixel's uv to read the full-size normals / motion at its pixel (RayGen's pixelUV)
    vec4 params;        // x max samples, y history clip weight (FFX temporal stability), z mirror roughness, w 1: last frame's history is usable
    vec4 params2;       // x core (0 ours, 1 FFX), y debug view, z ours: blur radius (grid px at roughness 1), w ours: hit-distance share for contact
    vec4 params3;       // x ours: anti-lag sigma, y ours: max history at the mirror limit (frames), z ours: firefly clamp, w the frame (0-63)
    uint linearIndex;   // LINEAR / CLAMP
    uint pointIndex;    // POINT / CLAMP
    uint pad0, pad1;
} pc;

#define LINEAR_S NVR_SAMPLER(pc.linearIndex)
#define POINT_S NVR_SAMPLER(pc.pointIndex)
#define GRID ivec2(pc.size.xy)
#define MirrorRoughness pc.params.z
#define HistoryValid (pc.params.w > 0.5)

vec4 Fetch(texture2D t, ivec2 p) { return texelFetch(sampler2D(t, POINT_S), clamp(p, ivec2(0), GRID - 1), 0); }

// The surface at a grid pixel: view z (0: nothing reflects), roughness without RayGen's flags, weapon (RayGen's +6).
struct Surface { float z; float roughness; bool weapon; vec3 n; };
float StripRoughness(float y, out bool weapon) {
    weapon = y > 5.5;
    return y - (weapon ? 6.0 : y > 3.5 ? 4.0 : y > 1.5 ? 2.0 : 0.0);
}
vec3 ViewToWorldN(vec3 v) { return normalize(v.x * normalize(pc.right.xyz) + v.y * normalize(pc.up.xyz) + v.z * normalize(pc.forward.xyz)); }
Surface SurfaceAt(ivec2 p) {
    Surface s;
    vec2 r = Fetch(t_ray, p).xy;
    s.z = r.x;
    s.roughness = StripRoughness(r.y, s.weapon);
    vec2 uv = (vec2(clamp(p, ivec2(0), GRID - 1)) + 0.5) * pc.size.zw + pc.fullUv.xy;
    s.n = ViewToWorldN(textureLod(sampler2D(t_normals, POINT_S), uv, 0.0).xyz * 2.0 - 1.0);
    return s;
}
// The camera-relative world point through grid uv at view z (Reflections.glsl toWorld(pixelUV) * depth: the grid pixel's full-size pixel)
vec3 PointAt(vec2 uv, float z) { uv += pc.fullUv.xy; return (pc.forward.xyz + pc.right.xyz * (2.0 * uv.x - 1.0) - pc.up.xyz * (2.0 * uv.y - 1.0)) * z; }
// Into last frame's grid (uv) through the reprojection; z: last frame's view z of that point
vec3 ToLast(vec3 p) {
    vec4 clip = pc.reproject * vec4(p, 1.0);
    float w = max(clip.w, 1.0e-3);
    return vec3(vec2(clip.x / w * 0.5 + 0.5, 0.5 - clip.y / w * 0.5) - pc.fullUv.xy, clip.w);
}
vec3 ObjectMotionAt(vec2 uv) { return pc.right.w > 0.5 ? textureLod(sampler2D(t_motion, POINT_S), uv + pc.fullUv.xy, 0.0).xyz : vec3(0.0); }

vec2 OctEncodeN(vec3 n) {
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    vec2 e = n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return e;
}
vec3 OctDecodeN(vec2 e) {
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}

bool Finite(vec4 v) { return !any(isnan(v)) && !any(isinf(v)); }

// ---- AMD's helpers (ffx_denoiser_reflections_common.h / _config.h) ----
#define FFX_DNSR_REFLECTIONS_GAUSSIAN_K 3.0
#define FFX_DNSR_REFLECTIONS_RADIANCE_WEIGHT_BIAS 0.6
#define FFX_DNSR_REFLECTIONS_RADIANCE_WEIGHT_VARIANCE_K 0.1
#define FFX_DNSR_REFLECTIONS_AVG_RADIANCE_LUMINANCE_WEIGHT 0.3
#define FFX_DNSR_REFLECTIONS_PREFILTER_VARIANCE_WEIGHT 4.4
#define FFX_DNSR_REFLECTIONS_REPROJECT_SURFACE_DISCARD_VARIANCE_WEIGHT 1.5
#define FFX_DNSR_REFLECTIONS_PREFILTER_VARIANCE_BIAS 0.1
#define FFX_DNSR_REFLECTIONS_PREFILTER_NORMAL_SIGMA 512.0
#define FFX_DNSR_REFLECTIONS_PREFILTER_DEPTH_SIGMA 4.0
#define FFX_DNSR_REFLECTIONS_DISOCCLUSION_NORMAL_WEIGHT 1.4
#define FFX_DNSR_REFLECTIONS_DISOCCLUSION_DEPTH_WEIGHT 1.0
#define FFX_DNSR_REFLECTIONS_DISOCCLUSION_THRESHOLD 0.9
#define FFX_DNSR_REFLECTIONS_REPROJECTION_NORMAL_SIMILARITY_THRESHOLD 0.9999
#define FFX_DNSR_REFLECTIONS_SAMPLES_FOR_ROUGHNESS(r) (1.0 - exp(-(r) * 100.0))
#define FFX_DNSR_REFLECTIONS_LOCAL_NEIGHBORHOOD_RADIUS 4

float FFX_Luminance(vec3 color) { return max(dot(color, vec3(0.299, 0.587, 0.114)), 0.001); }
float FFX_ComputeTemporalVariance(vec3 history_radiance, vec3 radiance) {
    float history_luminance = FFX_Luminance(history_radiance);
    float luminance = FFX_Luminance(radiance);
    float diff = abs(history_luminance - luminance) / max(max(history_luminance, luminance), 0.5);
    return diff * diff;
}
// NVR: four channels (the confidence is clipped with the colour)
vec4 FFX_ClipAABB(vec4 aabb_min, vec4 aabb_max, vec4 prev_sample) {
    vec4 aabb_center = 0.5 * (aabb_max + aabb_min);
    vec4 extent_clip = 0.5 * (aabb_max - aabb_min) + 0.001;
    vec4 color_vector = prev_sample - aabb_center;
    vec4 color_vector_clip = abs(color_vector / extent_clip);
    float max_abs_unit = max(max(color_vector_clip.x, color_vector_clip.y), max(color_vector_clip.z, color_vector_clip.w));
    return max_abs_unit > 1.0 ? aabb_center + color_vector / max_abs_unit : prev_sample;
}
float FFX_LocalNeighborhoodKernelWeight(float i) {
    const float radius = float(FFX_DNSR_REFLECTIONS_LOCAL_NEIGHBORHOOD_RADIUS) + 1.0;
    return exp(-FFX_DNSR_REFLECTIONS_GAUSSIAN_K * (i * i) / (radius * radius));
}
// NVR: AMD's IsGlossyReflection(roughness < the SSSR threshold) / IsMirrorReflection: every surface that reflects is glossy here
// (Resolve already left surfaces past MaxRoughness to the smooth fallbacks); mirrors (under MirrorRoughness, traced noise-free)
// pass through untouched, as in the old path.
bool IsMirror(float roughness) { return roughness < MirrorRoughness; }

// The local neighbourhood (9 x 9 Gaussian) of a texture, over pixels that reflect (NVR: the others are left out, not black)
void LocalMoments(texture2D t, ivec2 p, out vec4 mean, out vec4 variance) {
    mean = vec4(0.0); variance = vec4(0.0);
    float acc = 0.0;
    for (int j = -FFX_DNSR_REFLECTIONS_LOCAL_NEIGHBORHOOD_RADIUS; j <= FFX_DNSR_REFLECTIONS_LOCAL_NEIGHBORHOOD_RADIUS; ++j)
        for (int i = -FFX_DNSR_REFLECTIONS_LOCAL_NEIGHBORHOOD_RADIUS; i <= FFX_DNSR_REFLECTIONS_LOCAL_NEIGHBORHOOD_RADIUS; ++i) {
            ivec2 q = p + ivec2(i, j);
            if (any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, GRID)) || Fetch(t_ray, q).x <= 0.0) continue;
            vec4 radiance = Fetch(t, q);
            if (!Finite(radiance)) continue;
            float w = FFX_LocalNeighborhoodKernelWeight(float(i)) * FFX_LocalNeighborhoodKernelWeight(float(j));
            acc += w; mean += radiance * w; variance += radiance * radiance * w;
        }
    if (acc <= 0.0) return;
    mean /= acc; variance /= acc;
    variance = abs(variance - mean * mean);
}
