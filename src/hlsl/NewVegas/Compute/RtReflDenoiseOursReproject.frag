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

// Last frame's result, moments and the summed weight at rectUv, from the 2 x 2 texels that hold the same kind of surface
struct History { vec4 value; vec4 moments; float weight; };
History Gather(vec2 rectUv, bool inFrustum, Surface s, float expectedZ, float zTolerance, float hit, bool virtualTest) {
    History h; h.value = vec4(0.0); h.moments = vec4(0.0); h.weight = 0.0;
    if (!inFrustum) return h;
    ivec2 lastGrid = max(ivec2(round(pc.size.xy * pc.rect.zw)), ivec2(1));
    lastGrid = min(lastGrid, ACTIVE_GRID);
    vec2 f = pc.size.xy * rectUv - 0.5;
    ivec2 t = ivec2(floor(f));
    vec2 b = f - vec2(t);
    for (int k = 0; k < 4; k++) {
        ivec2 o = ivec2(k & 1, k >> 1);
        ivec2 q = t + o;
        if (any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, lastGrid))) continue;
        vec4 g = Fetch(t_in2, q);
        if (g.x == 0.0) continue;
        bool gWeapon = g.x < 0.0;
        float gz = abs(g.x);
        if (gWeapon != s.weapon) continue;
        float w = (o.x == 1 ? b.x : 1.0 - b.x) * (o.y == 1 ? b.y : 1.0 - b.y);
        float nDotMin = s.weapon ? 0.75 : 0.9;
        bool same = abs(gz - expectedZ) < zTolerance && dot(OctDecodeN(g.zw), s.n) > nDotMin && abs(g.y - s.roughness) < 0.05 + 0.25 * s.roughness;
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
    ivec2 px = ivec2(gl_FragCoord.xy);
    if (any(greaterThanEqual(px, ACTIVE_GRID))) {
        o_guide = vec4(0.0); o_accum = vec4(0.0); o_average = vec4(0.0); o_moments = vec4(0.0);
        return;
    }
    vec2 rectUv = (vec2(px) + 0.5) * pc.size.zw;
    Surface s = SurfaceAt(px);
    vec4 signal = Fetch(t_signal, px);
    if (!Finite(signal)) signal = vec4(0.0);
    // the hit distance for tracking: the nearest hit of the 3 x 3 neighbourhood (one ray a pixel mixes the reflected object with sky
    // misses at its edges; their mean would point the virtual reprojection between the two)
    float hit = 1.0e9;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++) {
            ivec2 q = clamp(px + ivec2(x, y), ivec2(0), max(ACTIVE_GRID - 1, ivec2(0)));
            float hq = Fetch(t_hitDist, q).y;
            if (Fetch(t_ray, q).x > 0.0 && hq > 0.0) hit = min(hit, hq);
        }
    if (hit > 1.0e8) hit = 0.0;
    o_guide = vec4(s.weapon ? -s.z : s.z, s.roughness, OctEncodeN(s.n));
    o_accum = signal; o_average = signal;
    float rawHit = max(Fetch(t_hitDist, px).y, 0.0);
    o_moments = vec4(rawHit, 1.0, max(hit, 0.0), 0.0);
    if (s.z <= 0.0) { o_guide = vec4(0.0); o_accum = vec4(0.0); o_average = vec4(0.0); o_moments = vec4(0.0); return; }
    if (IsMirror(s.roughness)) return;

    // 3x3 same-surface neighborhood (un-premultiplied Tame color + alpha) for firefly clamping, anti-lag, and variance clip
    const vec3 lumaW = vec3(0.2126, 0.7152, 0.0722);
    const float minTameDenom = 1.0 / 17.0;
    vec4 m1 = vec4(0.0), m2 = vec4(0.0);
    float c = 0.0;
    vec3 cM1 = vec3(0.0), cM2 = vec3(0.0);
    float cCount = 0.0;
    float nbrLin1 = 0.0, nbrLin2 = 0.0, nbrN = 0.0;
    float aMin = signal.a, aMax = signal.a;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++) {
            ivec2 q = clamp(px + ivec2(x, y), ivec2(0), max(ACTIVE_GRID - 1, ivec2(0)));
            vec2 rq = Fetch(t_ray, q).xy;
            if (rq.x <= 0.0) continue;
            bool qw;
            StripRoughness(rq.y, qw);
            if (qw != s.weapon) continue;
            vec4 v = Fetch(t_signal, q);
            if (!Finite(v)) continue;
            aMin = min(aMin, v.a);
            aMax = max(aMax, v.a);
            if (x == 0 && y == 0) continue;
            m1 += v; m2 += v * v; c += 1.0;
            if (v.a > 1.0e-4) {
                vec3 tc = max(v.rgb / v.a, vec3(0.0));
                cM1 += tc; cM2 += tc * tc; cCount += 1.0;
                float tl = dot(tc / max(1.0 - dot(tc, lumaW), minTameDenom), lumaW);
                nbrLin1 += tl; nbrLin2 += tl * tl; nbrN += 1.0;
            }
        }
    if (pc.params3.z > 0.0 && signal.a > 1.0e-4 && nbrN >= 3.0) {
        vec3 curC = max(signal.rgb / signal.a, vec3(0.0));
        vec3 curLin = curC / max(1.0 - dot(curC, lumaW), minTameDenom);
        float curLinL = dot(curLin, lumaW);
        float meanL = nbrLin1 / nbrN;
        float sigL = sqrt(max(nbrLin2 / nbrN - meanL * meanL, 0.0));
        float capL = meanL + pc.params3.z * max(sigL, 0.25);
        if (curLinL > capL) {
            curLin *= capL / max(curLinL, 1.0e-4);
            curC = curLin / (1.0 + dot(curLin, lumaW));
            signal.rgb = curC * signal.a;
        }
    }
    m1 += signal; m2 += signal * signal; c += 1.0;
    if (signal.a > 1.0e-4) {
        vec3 tc = max(signal.rgb / signal.a, vec3(0.0));
        cM1 += tc; cM2 += tc * tc; cCount += 1.0;
    }
    o_accum = signal; o_average = signal;
    if (!HistoryValid) return;

    // surface reprojection (with ObjectMotion; weapon pixels use ViewModelMotionBuffer when available, else screen-locked)
    vec3 uvSurf;
    bool inSurf = true;
    if (s.weapon) {
        if (pc.vmMotionValid != 0u) {
            vec2 ruvFull = clamp(rectUv + pc.fullUv.xy, 0.5 * pc.size.zw, pc.rect.xy - 0.5 * pc.size.zw);
            vec4 vmm = textureLod(sampler2D(t_vmMotion, POINT_S), ruvFull, 0.0);
            if (vmm.b > 0.5) {
                vec2 sCur = (rectUv + pc.fullUv.xy) / max(pc.rect.xy, vec2(1.0e-4));
                vec2 sPrev = sCur + vmm.rg;
                inSurf = all(greaterThanEqual(sPrev, vec2(0.0))) && all(lessThanEqual(sPrev, vec2(1.0)));
                uvSurf = vec3(sPrev * pc.rect.zw - pc.fullUv.xy, s.z);
            } else {
                uvSurf = vec3(rectUv, s.z);
            }
        } else {
            uvSurf = vec3(rectUv, s.z);
        }
    } else {
        vec3 pSurf = PointAt(rectUv, s.z);
        uvSurf = ToLastWithMotion(pSurf, rectUv, inSurf);
    }
    History hs = Gather(uvSurf.xy, inSurf, s, uvSurf.z, 0.03 * uvSurf.z + 2.0, 0.0, false);

    // virtual (hit-point) reprojection: camera-only ToLast(pVirt) (never subtract reflector ObjectMotion!), including RtMirrorJitter sub-texel ray shift
    float hitUsed = hit;
    History hv; hv.value = vec4(0.0); hv.moments = vec4(0.0); hv.weight = 0.0;
    if (hitUsed > 0.0 && !s.weapon) {
        vec2 mj = (pc.activeSize.z > 0.5 && s.roughness < pc.up.w) ? RtMirrorJitter(pc.activeSize.z, s.roughness) * pc.size.zw : vec2(0.0);
        vec3 pSurfVirt = PointAt(rectUv + mj, s.z);
        float d = max(length(pSurfVirt), 1.0e-4);
        bool inVirt = false;
        vec3 lastV = ToLast(pSurfVirt * ((d + hitUsed) / d), inVirt);
        // last frame's surface under the virtual point: not known before the lookup; a floor of this frame's depth +- 15 %
        hv = Gather(lastV.xy, inVirt, s, s.z, 0.15 * s.z + 4.0, hitUsed, true);
    }
    float sOk = hs.weight > 0.25 ? 1.0 : 0.0, vOk = hv.weight > 0.25 ? 1.0 : 0.0;
    // virtual share: all of it on smooth surfaces, none past roughness 0.5 (a wide lobe's image moves more with the surface)
    float vShare = vOk * (sOk > 0.0 ? 1.0 - smoothstep(0.1, 0.5, s.roughness) : 1.0);
    if (sOk + vOk <= 0.0) return;   // disoccluded: this frame's sample alone
    vec4 history = mix(hs.value, hv.value, vShare);
    vec4 hm = mix(hs.moments, hv.moments, vShare);

    // history length cap by roughness; un-premultiplied Tame history clamp + anti-lag against this frame's 3 x 3 neighbourhood
    float maxN = mix(pc.params3.y, pc.params.x, smoothstep(MirrorRoughness, 0.25, s.roughness));
    if (s.weapon) maxN = min(maxN, 6.0);
    float n = min(hm.y + 1.0, maxN);
    m1 /= max(c, 1.0); m2 /= max(c, 1.0);
    vec4 sigma = sqrt(max(m2 - m1 * m1, vec4(0.0)));
    if (cCount > 0.5 && history.a > 1.0e-4) {
        vec3 cMean = cM1 / cCount;
        vec3 cSigma = sqrt(max(cM2 / cCount - cMean * cMean, vec3(0.0)));
        vec3 histC = max(history.rgb / history.a, vec3(0.0));
        if (s.roughness < 0.03) histC = clamp(histC, max(cMean - 1.25 * cSigma, vec3(0.0)), cMean + 1.25 * cSigma);
        float hl = dot(histC, lumaW);
        float maxTameL = 16.0 / 17.0;
        if (hl > maxTameL) histC *= maxTameL / hl;
        float histA = s.roughness < 0.03 ? clamp(history.a, aMin, aMax) : history.a;
        history = vec4(histC * histA, histA);
    } else {
        history = vec4(0.0);
    }
    float stray = luma(abs(history - m1).rgb), allowed = pc.params3.x * luma(sigma.rgb) + 0.05 * luma(m1.rgb) + 0.01;
    if (stray > allowed) n = max(1.0, n * allowed / stray);
    float a = 1.0 / n;
    o_accum = mix(history, signal, a);
    o_average = o_accum;
    o_moments = vec4(hm.x > 0.0 ? mix(hm.x, rawHit, a) : rawHit, n, hit, vShare);
}
