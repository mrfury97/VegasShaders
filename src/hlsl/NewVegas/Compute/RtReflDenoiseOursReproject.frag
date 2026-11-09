// UNOFFICIAL 2026-11-07 RT reflection denoiser, core 0 ("ours"), pass 1 of 4: reproject + temporal accumulation, written from the
// published ReBLUR / ReLAX designs (no code of theirs): the noisy signal is accumulated into last frame's result found two ways:
//   surface:  where this surface point was last frame (camera move + ObjectMotion), 2 x 2 bilinear over texels of the same surface
//   virtual:  where the REFLECTED point was (the view ray lengthened by the hit distance), for glossy surfaces whose reflection moves
//             with the reflected thing, not with the floor; only where last frame's texel there is the same kind of surface
// mixed by roughness (virtual for smooth, surface for rough) and confidence. History length: up to params.x frames, fewer for smooth
// surfaces (params3.y at the mirror limit: little noise, and a wrong reprojection shows); anti-lag cuts it where the history strays
// further from this frame's 3 x 3 neighbourhood than params3.x sigmas.
// Inputs: t_in0 last frame's result, t_in1 last frame's moments (y history length, z tracking hit distance), t_in2 last frame's guide.
// Outputs: 0 the accumulated signal, 1 moments (x the accumulated hit distance (the blur's), y history length, z the tracking hit distance (3 x 3 nearest), w virtual share),
//          2 the same accumulated signal (for the 8 x 8 average), 3 this frame's guide (x view z, y roughness, zw world normal).
// Compile: glslangValidator -V --target-env vulkan1.1 -o RtReflDenoiseOursReproject.spv RtReflDenoiseOursReproject.frag
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "RtReflDenoise.glsl"

layout(location = 0) out vec4 o_accum;
layout(location = 1) out vec4 o_moments;
layout(location = 2) out vec4 o_average;
layout(location = 3) out vec4 o_guide;

// Last frame's result, moments and the summed weight at uv, from the 2 x 2 texels that hold the same kind of surface
struct History { vec4 value; vec4 moments; float weight; };
History Gather(vec2 uv, Surface s, float expectedZ, float zTolerance, float hit, bool virtualTest) {
    History h; h.value = vec4(0.0); h.moments = vec4(0.0); h.weight = 0.0;
    vec2 f = pc.size.xy * uv - 0.5;
    ivec2 t = ivec2(floor(f));
    vec2 b = f - vec2(t);
    for (int k = 0; k < 4; k++) {
        ivec2 o = ivec2(k & 1, k >> 1);
        ivec2 q = t + o;
        if (any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, GRID))) continue;
        vec4 g = Fetch(t_in2, q);
        if (g.x <= 0.0) continue;
        float w = (o.x == 1 ? b.x : 1.0 - b.x) * (o.y == 1 ? b.y : 1.0 - b.y);
        bool same = abs(g.x - expectedZ) < zTolerance && dot(OctDecodeN(g.zw), s.n) > 0.9 && abs(g.y - s.roughness) < 0.05 + 0.25 * s.roughness;
        vec4 m = Fetch(t_in1, q);
        if (virtualTest) same = same && abs(m.z - hit) < 0.25 * hit + 10.0;   // the same reflected thing (its hit distance)
        if (!same) continue;
        vec4 v = Fetch(t_in0, q);
        if (!Finite(v)) continue;
        h.value += v * w; h.moments += m * w; h.weight += w;
    }
    if (h.weight > 1.0e-4) { h.value /= h.weight; h.moments /= h.weight; }
    return h;
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec2 uv = (vec2(p) + 0.5) * pc.size.zw;
    Surface s = SurfaceAt(p);
    vec4 signal = Fetch(t_signal, p);
    if (!Finite(signal)) signal = vec4(0.0);
    // the hit distance for tracking: the nearest hit of the 3 x 3 neighbourhood (one ray a pixel mixes the reflected object with sky
    // misses at its edges; their mean would point the virtual reprojection between the two)
    float hit = 1.0e9;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++) {
            ivec2 q = p + ivec2(x, y);
            float hq = Fetch(t_hitDist, q).y;
            if (Fetch(t_ray, q).x > 0.0 && hq > 0.0) hit = min(hit, hq);
        }
    if (hit > 1.0e8) hit = 0.0;
    o_guide = vec4(s.z, s.roughness, OctEncodeN(s.n));
    o_accum = signal; o_average = signal;
    float rawHit = max(Fetch(t_hitDist, p).y, 0.0);
    o_moments = vec4(rawHit, 1.0, max(hit, 0.0), 0.0);
    if (s.z <= 0.0) { o_guide = vec4(0.0); o_accum = vec4(0.0); o_average = vec4(0.0); o_moments = vec4(0.0); return; }
    if (IsMirror(s.roughness) || !HistoryValid) return;

    // surface reprojection
    vec3 point = PointAt(uv, s.z);
    vec3 last = ToLast(point - ObjectMotionAt(uv));
    if (s.weapon) last = vec3(uv, s.z);
    History hs = Gather(last.xy, s, last.z, 0.03 * last.z + 2.0, 0.0, false);
    // virtual (hit-point) reprojection
    float hitUsed = hit;
    History hv; hv.weight = 0.0;
    if (hitUsed > 0.0 && !s.weapon) {
        float d = length(point);
        vec3 lastV = ToLast(point / d * (d + hitUsed));
        // last frame's surface under the virtual point: not known before the lookup; a floor of this frame's depth +- 15 %
        hv = Gather(lastV.xy, s, s.z, 0.15 * s.z + 4.0, hitUsed, true);
    }
    float sOk = hs.weight > 0.25 ? 1.0 : 0.0, vOk = hv.weight > 0.25 ? 1.0 : 0.0;
    // virtual share: all of it on smooth surfaces, none past roughness 0.5 (a wide lobe's image moves more with the surface)
    float vShare = vOk * (sOk > 0.0 ? 1.0 - smoothstep(0.1, 0.5, s.roughness) : 1.0);
    if (sOk + vOk <= 0.0) return;   // disoccluded: this frame's sample alone
    vec4 history = mix(hs.value, hv.value, vShare);
    vec4 hm = mix(hs.moments, hv.moments, vShare);

    // history length cap by roughness; anti-lag against this frame's 3 x 3 neighbourhood
    float maxN = mix(pc.params3.y, pc.params.x, smoothstep(MirrorRoughness, 0.25, s.roughness));
    float n = min(hm.y + 1.0, maxN);
    vec4 m1 = vec4(0.0), m2 = vec4(0.0);
    float c = 0.0;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++) {
            ivec2 q = p + ivec2(x, y);
            if (Fetch(t_ray, q).x <= 0.0) continue;
            vec4 v = Fetch(t_signal, q);
            if (!Finite(v)) continue;
            m1 += v; m2 += v * v; c += 1.0;
        }
    m1 /= c; m2 /= c;
    vec4 sigma = sqrt(max(m2 - m1 * m1, vec4(0.0)));
    // UNOFFICIAL 2026-11-08 (perf-reflfix4) MirrorJitter: a mirror pixel's history (only reached with MirrorJitter: MirrorRoughness 0) is
    // clipped into this frame's 3 x 3 neighbourhood, mean +- 1.25 sigma (TAA's variance clip): the jittered samples' area average stays,
    // what moved is cut at once (the RT test host's mirroraa scene: motion's extra error 0.0024 -> 0.0019, the stair-steps gone)
    if (s.roughness < 0.03) history = clamp(history, m1 - 1.25 * sigma, m1 + 1.25 * sigma);
    float stray = luma(abs(history - m1).rgb), allowed = pc.params3.x * luma(sigma.rgb) + 0.05 * luma(m1.rgb) + 0.01;
    if (stray > allowed) n = max(1.0, n * allowed / stray);
    float a = 1.0 / n;
    o_accum = mix(history, signal, a);
    o_average = o_accum;
    o_moments = vec4(hm.x > 0.0 ? mix(hm.x, rawHit, a) : rawHit, n, hit, vShare);
}
