// UNOFFICIAL 2026-11-07 (ptaaa-denoiser) shared RT denoiser (core\RtDenoise.h), SPECULAR mode (Settings.Specular, the path tracer's
// PTDenoiseSplit): the temporal pass of RtDenoiseTemporalRgb.frag for a specular signal, written from the published NRD ReLAX / ReBLUR
// designs (no code of theirs), as the RT reflection denoiser's RtReflDenoiseOursReproject.frag does it. Last frame's result is found
// two ways and mixed by roughness:
//   surface:  where this surface point was last frame (the camera's move + the object's own, ObjectMotion), 2 x 2 bilinear over the
//             texels of the same surface (the depth test);
//   virtual:  where the REFLECTED point was: the view ray lengthened by the specular hit distance (the virtual image behind the mirror).
//             A reflection moves with the reflected thing, not with the floor: reprojected with the surface, a reflection smears when
//             the camera moves (the mirror ghost). Only where last frame's texel there is the same kind of surface (depth within 15 %,
//             roughness, the same tracked hit distance).
// virtual share: all of it on smooth surfaces, none past roughness 0.5 (spec.y 0: surface only, the A/B). History: MaxHistory frames on
// rough surfaces, spec.x on mirrors (a wrong reprojection shows there; little noise to hide), between by roughness; people / moving /
// anti-lag caps as the RGB pass.
// In:  t_noisy (rgb: the specular light / its specular albedo, a: the hit distance when the bounce ray was the specular lobe's, 60000 a
//      miss, 0 unknown), t_rough (.g: roughness; the path tracer's accumB), the histories A (rgb, length), B (luma 2nd moment, view z, fast
//      luma mean, fast 2nd moment) and C (tracked hit distance, roughness, virtual share, 0).
// Out: A, B, C of this frame.
// Compile: glslangValidator -V --target-env vulkan1.1 -o RtDenoiseTemporalSpec.spv RtDenoiseTemporalSpec.frag
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_noisy;
layout(set = 1, binding = 1) uniform texture2D t_historyA;
layout(set = 1, binding = 2) uniform texture2D t_depth;      // TESR_DepthBuffer
layout(set = 1, binding = 3) uniform texture2D t_historyB;
layout(set = 1, binding = 4) uniform texture2D t_motion;     // TESR_ObjectMotionBuffer (a stand-in when backbone.x is 0)
layout(set = 1, binding = 5) uniform texture2D t_backbone;   // DenoiseBackbone (a stand-in when backbone.y is 0)
layout(set = 1, binding = 6) uniform texture2D t_captured;   // TESR_CapturedNormalsBuffer (a stand-in when peopleCap is 0)
layout(set = 1, binding = 7) uniform texture2D t_rough;      // .g roughness
layout(set = 1, binding = 8) uniform texture2D t_historyC;
layout(set = 1, binding = 9) uniform texture2D t_normals;    // (2026-11-09 pt-mirrors) TESR_NormalsBuffer (a curved mirror's curvature)
// UNOFFICIAL 2026-11-09 (pt-mirrors) PTDenoiseMirrorSurface (spec.z 1): the path tracer marks a smooth metal pixel whose reflected thing
// stands in for the surface with a NEGATIVE hit distance (RtPathTrace.comp gReflAlbedo: its specular light is already divided by the
// reflected colour). There the reflection is a diffuse surface seen through the mirror, and its history is that surface's:
//   - the virtual point of a CURVED mirror: a convex mirror shows its reflection close behind its surface (the mirror equation: at
//     hit / (1 + 2 x hit x curvature), the curvature from this frame's normals and depth), not a whole hit distance behind it as a flat one
//     does; the flat-mirror point made the chrome sphere's virtual history miss every frame;
//   - where that virtual history holds (the same surface there, the same tracked hit) the full history (MaxHistory), not MirrorHistory:
//     the reflected light is one noisy path a frame like any surface's, and AntiLag still restarts it where the light really changes;
//   - only history of the same kind is taken (C.w: marked or not: the two are at different scales).
// The spatial pass then smooths it (RtDenoiseAtrousRgb.frag, specular bit 2). spec.z 0: as before.

layout(push_constant) uniform Push {
    mat4 prevViewProj;
    vec4 forward;        // w farZ
    vec4 right;
    vec4 up;
    vec4 move;           // xyz camera move, w 1: reset
    vec4 params;         // x max history, y depth tolerance, zw 1 / size
    vec4 backbone;       // as RtDenoiseTemporalRgb.frag
    vec4 rect;           // as RtDenoiseTemporalRgb.frag
    vec4 spec;           // x the history on a mirror (frames), y 1: the hit-distance (virtual) reprojection on, z 1: PTDenoiseMirrorSurface
    uint samplerIndex;
    float clampK;
    float peopleCap;
    float antiLag;
    float fireflyK;      // (2026-11-07 ptaaa-denoiser) AntiFirefly k (0 off): as RtDenoiseTemporalRgb.frag
    float fastFrames;    // (2026-11-07 perf-history) AntiLag's fast history in frames (0: 4): as RtDenoiseTemporalRgb.frag
} pc;
// UNOFFICIAL 2026-11-09 (pt playground review) FireflySpread rides in fireflyK: k + 128 x cap (cap a whole number, 0 = no cap, as before).
// The clamped excess of one firefly is spread over 5 x 5 pixels (RtDenoiseFirefly.frag); a path hundreds of times its neighbours'
// brightness still left a white 5 x 5 square that the history then carried for seconds (the playground's denoised frames: dozens of
// white squares on walls and floor). With a cap the excess handed on is at most cap x what the pixel's neighbours / own past support;
// the rest is dropped (the usual, slightly biased cut).
#define NVR_FIREFLY_CAP floor(pc.fireflyK * (1.0 / 128.0))
#define NVR_FIREFLY_K (pc.fireflyK - 128.0 * NVR_FIREFLY_CAP)
vec3 FireflyExcess(vec3 excess, vec3 kept) {
    const float cap = NVR_FIREFLY_CAP;
    if (!(cap > 0.0)) return excess;
    const float le = dot(max(excess, vec3(0.0)), vec3(0.2126, 0.7152, 0.0722)), limit = cap * max(dot(kept, vec3(0.2126, 0.7152, 0.0722)), 1.0e-4);
    return le > limit ? excess * (limit / le) : excess;
}

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_a;
layout(location = 1) out vec4 o_b;
layout(location = 2) out vec4 o_c;
layout(location = 3) out vec4 o_d;   // (ptaaa-denoiser) AntiFirefly: as RtDenoiseTemporalRgb.frag's o_d (RtDenoiseFirefly.frag spreads it)

#define SMP NVR_SAMPLER(pc.samplerIndex)

// (ptaaa-denoiser) AntiFirefly's spatial support: the brightest of the 3 x 3 neighbours, the centre left out (RtDenoiseTemporalRgb.frag)
float FireflyLimit(vec2 at) {
    float m = 0.0;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
            if (dx != 0 || dy != 0) m = max(m, luma(textureLod(sampler2D(t_noisy, SMP), at + vec2(dx, dy) * pc.params.zw, 0.0).rgb));
    return m;
}

struct History { vec4 a; vec4 b; vec4 c; float weight; };

// last frame's A, B, C at the screen point prevUv: 2 x 2 bilinear over the texels that pass the test (virtual: the tracked hit too)
History Gather(vec2 prevUv, float expectedZ, float zTolerance, float rough, float hit, bool virtualTest, float mirrorKind) {
    History h; h.a = vec4(0.0); h.b = vec4(0.0); h.c = vec4(0.0); h.weight = 0.0;
    const vec2 size = 1.0 / pc.params.zw;
    const vec2 f = prevUv * pc.rect.zw * size - 0.5;
    const vec2 t = floor(f), b = f - t;
    for (int k = 0; k < 4; k++) {
        const vec2 o = vec2(k & 1, k >> 1);
        const vec2 q = t + o;
        if (any(lessThan(q, vec2(0.0))) || any(greaterThanEqual(q, size))) continue;
        const vec2 quv = (q + 0.5) / size;
        const vec4 hb = textureLod(sampler2D(t_historyB, SMP), quv, 0.0);
        if (!(hb.g > 0.0) || abs(hb.g - expectedZ) > zTolerance) continue;
        const vec4 ha = textureLod(sampler2D(t_historyA, SMP), quv, 0.0);
        if (!(ha.a > 0.0)) continue;
        const vec4 hc = textureLod(sampler2D(t_historyC, SMP), quv, 0.0);
        if (abs(hc.y - rough) > 0.05 + 0.25 * rough) continue;   // another material
        if (virtualTest && abs(hc.x - hit) > 0.25 * hit + 10.0) continue;   // another reflected thing
        if (mirrorKind >= 0.0 && abs(hc.w - mirrorKind) > 0.5) continue;   // (pt-mirrors) the other scale (marked / not)
        const float w = (o.x > 0.5 ? b.x : 1.0 - b.x) * (o.y > 0.5 ? b.y : 1.0 - b.y);
        h.a += ha * w; h.b += hb * w; h.c += hc * w; h.weight += w;
    }
    if (h.weight > 1.0e-4) { h.a /= h.weight; h.b /= h.weight; h.c /= h.weight; }
    return h;
}

// (pt-mirrors) a screen point's position (from this eye) and normal (view space; only their differences are used: |dn| / |dp|)
vec3 PointAt(vec2 at, out float z) {
    z = textureLod(sampler2D(t_depth, SMP), at, 0.0).r * pc.forward.w;
    const vec2 q = at / pc.rect.xy;
    return (pc.forward.xyz + pc.right.xyz * (2.0 * q.x - 1.0) - pc.up.xyz * (2.0 * q.y - 1.0)) * z;
}
vec3 NormalAt(vec2 at) { return normalize(textureLod(sampler2D(t_normals, SMP), at, 0.0).xyz * 2.0 - 1.0); }
// (pt-mirrors) the surface's curvature (1 / radius) here: how fast its normal turns per unit of surface, from the neighbours of the same
// surface (depth within 5 %), both screen axes. 0 on a flat mirror; a sphere of radius R gives 1 / R. (Its sign is not known: a concave
// mirror is taken as convex; its virtual history then fails the hit test and the surface's is used, as before.)
float Curvature(vec2 at, vec3 p0, float z0) {
    const vec3 n0 = NormalAt(at);
    float k = 0.0, w = 0.0;
    for (int axis = 0; axis < 2; axis++) {
        const vec2 d = axis == 0 ? vec2(pc.params.z, 0.0) : vec2(0.0, pc.params.w);
        float za, zb;
        const vec3 pa = PointAt(at + d, za), pb = PointAt(at - d, zb);
        const bool okA = za > 0.0 && abs(za - z0) < 0.05 * z0, okB = zb > 0.0 && abs(zb - z0) < 0.05 * z0;
        if (okA && okB) { k += length(NormalAt(at + d) - NormalAt(at - d)) / max(length(pa - pb), 1.0e-3); w += 1.0; }
        else if (okA) { k += length(NormalAt(at + d) - n0) / max(length(pa - p0), 1.0e-3); w += 1.0; }
        else if (okB) { k += length(NormalAt(at - d) - n0) / max(length(pb - p0), 1.0e-3); w += 1.0; }
    }
    return w > 0.0 ? k / w : 0.0;
}

void main() {
    const vec2 s = uv / pc.rect.xy;
    if (s.x > 1.0 || s.y > 1.0) { o_a = vec4(0.0); o_b = vec4(0.0); o_c = vec4(0.0); o_d = vec4(0.0); return; }
    const vec4 noisy = textureLod(sampler2D(t_noisy, SMP), uv, 0.0);
    const vec3 value = noisy.rgb;
    const float l = luma(value);
    const float viewZ = textureLod(sampler2D(t_depth, SMP), uv, 0.0).r * pc.forward.w;
    const float rough = clamp(textureLod(sampler2D(t_rough, SMP), uv, 0.0).g, 0.0, 1.0);
    // the hit distance to track: the nearest of the 3 x 3 neighbourhood's known ones (one ray a pixel mixes the reflected object with
    // misses at its edges, and only the pixels whose ray was the specular lobe's know one)
    const bool mirrorOn = pc.spec.z > 0.5, mirror = mirrorOn && noisy.a < 0.0;   // (pt-mirrors) PTDenoiseMirrorSurface
    const float known = mirrorOn ? 0.9 : 0.0;   // (pt-mirrors: a marked pixel's -0.5 = its hit not known this frame)
    float hit = 1.0e9;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++) {
            const float hq = abs(textureLod(sampler2D(t_noisy, SMP), uv + vec2(x, y) * pc.params.zw, 0.0).a);   // (pt-mirrors: < 0 marked)
            if (hq > known) hit = min(hit, hq);
        }
    if (hit > 1.0e8) hit = 0.0;
    const float mirrorKind = mirrorOn ? (mirror ? 1.0 : 0.0) : -1.0;
    const float spatialLimit = NVR_FIREFLY_K > 0.0 ? FireflyLimit(uv) : 1.0e30;   // (ptaaa-denoiser) AntiFirefly
    vec3 used = l > spatialLimit ? value * (spatialLimit / l) : value;
    float excessWeight = 1.0, fastWeight = 1.0;
    vec4 a = vec4(used, 1.0);
    vec4 b = vec4(luma(used) * luma(used), viewZ, NVR_FIREFLY_K > 0.0 ? luma(used) : l, NVR_FIREFLY_K > 0.0 ? l : l * l);   // (AntiFirefly: as the RGB pass)
    vec4 c = vec4(hit, rough, 0.0, mirror ? 1.0 : 0.0);
    const bool fresh = pc.backbone.y > 0.5 && (pc.backbone.w > 0.5
        ? any(greaterThan(textureGather(sampler2D(t_backbone, SMP), uv, 2), vec4(0.5)))
        : textureLod(sampler2D(t_backbone, SMP), uv, 0.0).b > 0.5);
    if (pc.move.w < 0.5 && viewZ > 0.0 && !fresh) {
        const vec3 point = (pc.forward.xyz + pc.right.xyz * (2.0 * s.x - 1.0) - pc.up.xyz * (2.0 * s.y - 1.0)) * viewZ;   // from this eye
        float cap = pc.params.x;
        vec3 own = vec3(0.0);
        if (pc.backbone.x > 0.5) {
            own = textureLod(sampler2D(t_motion, SMP), uv, 0.0).rgb;
            if (pc.backbone.z > 0.5 && dot(own, own) > 0.0625) cap = min(cap, pc.backbone.z);
        }
        if (pc.peopleCap > 0.5 && textureLod(sampler2D(t_captured, SMP), uv, 0.0).a > 1.5) cap = min(cap, pc.peopleCap);
        // surface reprojection (as the RGB pass, bilinear)
        History hs; hs.a = vec4(0.0); hs.b = vec4(0.0); hs.c = vec4(0.0); hs.weight = 0.0;
        {
            const vec4 clip = pc.prevViewProj * vec4(point + pc.move.xyz - own, 1.0);
            if (clip.w > 0.0) {
                const vec2 prevUv = vec2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);
                if (all(greaterThanEqual(prevUv, vec2(0.0))) && all(lessThanEqual(prevUv, vec2(1.0))))
                    hs = Gather(prevUv, clip.w, pc.params.y * max(clip.w, 1.0), rough, 0.0, false, mirrorKind);
            }
        }
        // virtual (hit-point) reprojection: the reflected point = this view ray lengthened by the hit distance
        if (hit <= 0.0 && hs.weight > 0.25) hit = hs.c.x;   // none known here: the surface history's
        c.x = hit;
        History hv; hv.a = vec4(0.0); hv.b = vec4(0.0); hv.c = vec4(0.0); hv.weight = 0.0;
        float virtualPx = 1.0e9;   // (pt-mirrors) how far the virtual point moved on screen since last frame (pixels)
        if (pc.spec.y > 0.5 && hit > 0.0) {
            const float d = length(point);
            // (pt-mirrors) a marked curved mirror's virtual point lies hit / (1 + 2 hit k) behind it (a flat one's: the whole hit)
            const float behind = mirror ? hit / max(1.0 + 2.0 * hit * Curvature(uv, point, viewZ), 1.0) : hit;
            const vec4 clip = pc.prevViewProj * vec4(point / d * (d + behind) + pc.move.xyz, 1.0);
            if (clip.w > 0.0) {
                const vec2 prevUv = vec2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);
                virtualPx = length((prevUv - s) / pc.params.zw);
                // last frame's surface under the virtual point: its depth is not known before the lookup; this frame's +- 15 %
                if (all(greaterThanEqual(prevUv, vec2(0.0))) && all(lessThanEqual(prevUv, vec2(1.0))))
                    hv = Gather(prevUv, viewZ, 0.15 * viewZ + 4.0, rough, hit, true, mirrorKind);
            }
        }
        const float sOk = hs.weight > 0.25 ? 1.0 : 0.0, vOk = hv.weight > 0.25 ? 1.0 : 0.0;
        const float vShare = vOk * (sOk > 0.0 ? 1.0 - smoothstep(0.1, 0.5, rough) : 1.0);
        c.z = vShare;
        if (sOk + vOk > 0.0) {
            vec4 ha = vShare > 0.0 ? mix(hs.a, hv.a, vShare) : hs.a;
            const vec4 hb = vShare > 0.0 ? mix(hs.b, hv.b, vShare) : hs.b;
            // the history on a mirror: spec.x frames (a reprojection error shows there, and its one sample is nearly the answer), all
            // of MaxHistory from roughness 0.4 up
            // (pt-mirrors) a marked mirror whose virtual history held: the full history (the reflected surface's own) while the reflection
            // stands still on screen, down to MirrorHistory as it moves 0.05 -> 0.3 pixels a frame: each frame's bilinear reprojection
            // blurs a little, and a long history of a sliding reflection piles that blur up (RT host ptspecmove, 12 units a frame: the full
            // history at ~0.3 px a frame was 1.37x the old path's error: a walking camera keeps the old length, a still one the full)
            const float mirrorCap = mix(min(pc.spec.x, pc.params.x), pc.params.x, smoothstep(0.05, 0.4, rough));
            if (!(mirror && vOk > 0.0)) cap = min(cap, mirrorCap);
            else cap = min(cap, mix(pc.params.x, mirrorCap, smoothstep(0.05, 0.3, virtualPx)));
            if (pc.clampK > 0.0) {
                vec3 m1 = vec3(0.0), m2 = vec3(0.0);
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        const vec3 v = textureLod(sampler2D(t_noisy, SMP), uv + vec2(dx, dy) * pc.params.zw, 0.0).rgb;
                        m1 += v; m2 += v * v;
                    }
                m1 /= 9.0; m2 /= 9.0;
                const vec3 dev = sqrt(max(m2 - m1 * m1, vec3(0.0)));
                ha.rgb = clamp(ha.rgb, max(m1 - pc.clampK * dev, vec3(0.0)), m1 + pc.clampK * dev);
            }
            // AntiLag as the RGB pass (NRD ReBLUR's fast history)
            const float kFast = pc.fastFrames > 0.0 ? pc.fastFrames : 4.0;   // (perf-history: was 4.0)
            if (NVR_FIREFLY_K > 0.0) {   // (ptaaa-denoiser) AntiFirefly with a history: + k x this pixel's recent raw mean (B.a, as the RGB pass)
                const float limit = max(spatialLimit, NVR_FIREFLY_K * hb.a);
                used = l > limit ? value * (limit / l) : value;
            }
            const float fastLen = min(ha.a, kFast - 1.0) + 1.0;
            fastWeight = 1.0 / fastLen;
            const float fastL = mix(hb.b, NVR_FIREFLY_K > 0.0 ? luma(used) : l, 1.0 / fastLen);
            const float fastL2 = mix(hb.a, NVR_FIREFLY_K > 0.0 ? l : l * l, 1.0 / fastLen);
            b.b = fastL; b.a = fastL2;
            if (pc.antiLag > 0.0) {
                const float hl = luma(ha.rgb);
                const float sigma = sqrt(max(hb.r - hl * hl, 0.0));
                const float window = pc.antiLag * sigma * inversesqrt(kFast) + 0.02 * fastL + 1.0e-4;
                if (abs(hl - fastL) > window) {
                    const float target = clamp(hl, fastL - window, fastL + window);
                    ha.rgb = hl > 1.0e-6 ? ha.rgb * (target / hl) : vec3(target);
                    cap = min(cap, kFast);
                }
            }
            const float len = min(min(ha.a, cap) + 1.0, pc.params.x);
            const float alpha = 1.0 / len;
            a = vec4(mix(ha.rgb, used, alpha), len);
            excessWeight = alpha;
            b.r = mix(hb.r, luma(used) * luma(used), alpha);
        }
    }
    o_a = a;
    o_b = b;
    o_c = c;
    const vec3 excess = FireflyExcess(value - used, used);   // (2026-11-09: FireflySpread's cap)
    o_d = vec4(excess * excessWeight, luma(excess) * fastWeight);
}
