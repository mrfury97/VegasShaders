// UNOFFICIAL 2026-10-29 shared RT denoiser (core\RtDenoise.h), RGB mode (Settings.Channels 3: GI / sky colour), pass 1: temporal
// accumulation as RtDenoiseTemporal.frag, on rgb. Two outputs: 0 = (rgb, history length), 1 = (luma second moment, view depth, 0, 0).
// Compile: glslangValidator -V --target-env vulkan1.1 -o RtDenoiseTemporalRgb.spv RtDenoiseTemporalRgb.frag
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_noisy;      // rgb
layout(set = 1, binding = 1) uniform texture2D t_historyA;   // last frame's output 0
layout(set = 1, binding = 2) uniform texture2D t_depth;      // TESR_DepthBuffer
layout(set = 1, binding = 3) uniform texture2D t_historyB;   // last frame's output 1
layout(set = 1, binding = 4) uniform texture2D t_motion;     // UNOFFICIAL 2026-10-31 DenoiseBackbone: as RtDenoiseTemporal.frag
layout(set = 1, binding = 5) uniform texture2D t_backbone;
layout(set = 1, binding = 6) uniform texture2D t_captured;   // (2026-11-07) TESR_CapturedNormalsBuffer (a stand-in when peopleCap is 0)
// UNOFFICIAL 2026-11-16 (rtgi-review, Settings.Soft bit 1: backbone.w + 2): the albedo buffer (full size; alpha 1 where a template wrote the
// pixel, 0 on grass and whatever else the game's own shaders draw). A texel half or more of whose pixels have none is SOFT: grass blades sit
// each at their own depth, sway, and their cut-out is hashed under TAA / DLSS, so the disocclusion mask called them newly seen every frame
// and the depth test threw their history away: one raw sample a texel, for ever (the sparkles on grass in the shade). A soft texel keeps its
// history: the mask does not restart it and the depth may differ by 25 % (the blade in front last frame, the ground behind it now).
layout(set = 1, binding = 7) uniform texture2D t_soft;

layout(push_constant) uniform Push {
    mat4 prevViewProj;
    vec4 forward;        // w farZ
    vec4 right;
    vec4 up;
    vec4 move;           // xyz camera move, w 1: reset
    vec4 params;         // x max history, y depth tolerance
    vec4 backbone;       // x 1: subtract the object motion, y 1: the shared disocclusion mask restarts, z the history cap on moving
                         // pixels (0 none), w 1: half size (the mask's 2 x 2), + 2 (2026-11-16): Settings.Soft (t_soft)
    vec4 rect;           // (2026-11-03) RenderScale < 1: xy this frame's RenderRect scale (the signal is in the top-left xy part),
                         // zw last frame's (its history's part); 1 when off
    uint samplerIndex;
    float clampK;        // (2026-11-06) HistoryClamp: the history clamped to the new frame's 3 x 3 mean +- clampK x its deviation (0 off)
    float peopleCap;     // (2026-11-07) PeopleHistory: people's pixels (captured normal alpha > 1.5) keep at most this many frames (0 off)
    float antiLag;       // (2026-11-07) AntiLag: k, the fast history's window in standard errors (0 off)
    float fireflyK;      // (2026-11-07 ptaaa-denoiser) AntiFirefly: k (0 off; FireflyClamp below)
    float fastFrames;    // (2026-11-07 perf-history) AntiLag: the fast history in frames (RtHistoryTime.h: 4 at 60 fps, the same seconds at
                         // other frame rates, at least 2); 0: 4 (the value it had when hard-coded)
#ifdef NVR_CONFIDENCE
    float confOn, confTile, confW, confH;   // (2026-11-15 rt-denoise-inputs) 1: on, the tile size, the confidence grid's size (tiles); scalars:
                                           // a vec4 would be aligned to byte 208 (std430), RtDenoise.cpp ConfPush writes them at 200
#endif
} pc;
#ifdef NVR_CONFIDENCE
// UNOFFICIAL 2026-11-15 (rt-denoise-inputs, docs/next/RT-DENOISE-INPUTS.md) the -conf variant (variants.txt; Settings.Confidence,
// PTDenoiseConfidence / GIDenoiseConfidence): RtDenoiseConfidence.frag's history confidence (1/5 resolution, r), bilinear over the tile
// centres. Where it is low the history is cut to confidence^2 x MaxHistory frames (0: this frame's sample only), before the fast history
// and the blend, so a light that went out or moved shows at once. The variant only: the plain RtDenoiseTemporalRgb.spv is unchanged.
layout(set = 1, binding = 8) uniform texture2D t_conf;   // (test-after-110 merge: 8, after the soft mask on 7)
float ConfidenceAt(vec2 at) {
    const vec2 grid = vec2(pc.confW, pc.confH);
    const vec2 f = at * (1.0 / pc.params.zw) / pc.confTile - 0.5;
    const vec2 t = floor(f), b = f - t;
    float c = 0.0;
    for (int k = 0; k < 4; k++) {
        const vec2 o = vec2(k & 1, k >> 1);
        const vec2 q = clamp(t + o, vec2(0.0), grid - 1.0);
        c += textureLod(sampler2D(t_conf, NVR_SAMPLER(pc.samplerIndex)), (q + 0.5) / grid, 0.0).r *
            (o.x > 0.5 ? b.x : 1.0 - b.x) * (o.y > 0.5 ? b.y : 1.0 - b.y);
    }
    return clamp(c, 0.0, 1.0);
}
#endif
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

// (2026-11-07 ptaaa-denoiser) AntiFirefly (Settings.AntiFirefly, PTDenoiseAntiFirefly), NRD-style: a new sample brighter than both its
// 3 x 3 neighbours (the brightest, the centre left out) and k x its own recent past (the mean of its last ~4 RAW samples, kept in B.a while
// AntiFirefly is on: a glint that is there every frame is its own support from the second frame on) is clamped to that; the clamped
// excess is not thrown away: o_d carries it (x this frame's blend weight) and RtDenoiseFirefly.frag spreads it over the 5 x 5 round,
// into the history. So the light of rare bright paths (a glow panel only bounces find) is kept as a soft spread instead of white dots.
// The anti-lag's fast history then takes the clamped sample (fed the raw one, a firefly pulled the long history up to it: a dot).
float FireflyLimit(vec2 at, out bool actorNear) {
    float m = 0.0;
    actorNear = false;
    const vec2 lo = 0.5 * pc.params.zw, hi = max(pc.rect.xy - 0.5 * pc.params.zw, lo);
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
            if (dx != 0 || dy != 0) {
                const vec4 q = textureLod(sampler2D(t_noisy, NVR_SAMPLER(pc.samplerIndex)), clamp(at + vec2(dx, dy) * pc.params.zw, lo, hi), 0.0);
                m = max(m, luma(q.rgb));
                if (q.a < 0.0) actorNear = true;
            }
    return m;
}

// (2026-11-07 ptaaa-denoiser) last frame's A and B at the screen point prevUv: 2 x 2 bilinear over the texels of the same surface (each
// passes the depth test on its own; the weights renormalised). Was the nearest texel: under motion its half-texel rounding drifted the
// history by up to half a pixel a frame, and the drift adds up over the history (a moving NPC's light smeared even with the object
// motion right: host scene ptobjmotion). On a still camera prevUv sits on texel centres: one texel, weight 1, as before.
bool HistoryAt(vec2 prevUv, float expectedZ, float tolerance, out vec4 ha, out vec4 hb) {
    ha = vec4(0.0); hb = vec4(0.0);
    float sum = 0.0;
    const vec2 size = 1.0 / pc.params.zw;
    const vec2 f = prevUv * pc.rect.zw * size - 0.5;
    const vec2 t = floor(f), b = f - t;
    for (int k = 0; k < 4; k++) {
        const vec2 o = vec2(k & 1, k >> 1);
        const float w = (o.x > 0.5 ? b.x : 1.0 - b.x) * (o.y > 0.5 ? b.y : 1.0 - b.y);
        const vec2 q = t + o;
        if (w <= 0.0 || any(lessThan(q, vec2(0.0))) || any(greaterThanEqual(q, size))) continue;
        const vec2 quv = (q + 0.5) / size;
        const vec4 tb = textureLod(sampler2D(t_historyB, NVR_SAMPLER(pc.samplerIndex)), quv, 0.0);
        if (!(abs(tb.g - expectedZ) <= tolerance)) continue;
        const vec4 ta = textureLod(sampler2D(t_historyA, NVR_SAMPLER(pc.samplerIndex)), quv, 0.0);
        if (!(ta.a > 0.0)) continue;
        ha += ta * w; hb += tb * w; sum += w;
    }
    if (sum < 1.0e-3) return false;
    ha /= sum; hb /= sum;
    return true;
}

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_a;
layout(location = 1) out vec4 o_b;
layout(location = 2) out vec4 o_d;   // (ptaaa-denoiser) AntiFirefly: rgb the clamped excess x its blend weight, a its luma x the fast
                                     // history's weight (RtDenoiseFirefly.frag spreads both: the anti-lag's fast mean must see the light too)

void main() {
    // (2026-11-03) the screen point of this texel (the rect's uv / its scale); past the rect nothing (the a-trous passes skip it)
    const vec2 s = uv / pc.rect.xy;
    if (s.x > 1.0 || s.y > 1.0) { o_a = vec4(0.0); o_b = vec4(0.0); o_d = vec4(0.0); return; }
    const vec4 noisySample = textureLod(sampler2D(t_noisy, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0);
    const vec3 value = noisySample.rgb;
    const float l = luma(value);
    const float viewZ = textureLod(sampler2D(t_depth, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0).r * pc.forward.w;
    // (ptaaa-denoiser) AntiFirefly: the neighbours' support (a newly seen pixel has only that); c the sample used, its excess to o_d
    // UNOFFICIAL 2026-11-20: also detect moving-actor shadow blockers (t_noisy.a < 0) in the 3x3 neighbourhood with zero extra fetches when AntiFirefly is on
    bool actorShadowNear = noisySample.a < 0.0;
    float spatialLimit = 1.0e30;
    if (NVR_FIREFLY_K > 0.0) {
        bool neighActor = false;
        spatialLimit = FireflyLimit(uv, neighActor);
        actorShadowNear = actorShadowNear || neighActor;
    } else if (!actorShadowNear) {
        const vec2 lo = 0.5 * pc.params.zw, hi = max(pc.rect.xy - 0.5 * pc.params.zw, lo);
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++)
                if ((dx != 0 || dy != 0) && textureLod(sampler2D(t_noisy, NVR_SAMPLER(pc.samplerIndex)), clamp(uv + vec2(dx, dy) * pc.params.zw, lo, hi), 0.0).a < 0.0)
                    actorShadowNear = true;
    }
    vec3 c = l > spatialLimit ? value * (spatialLimit / l) : value;
    float excessWeight = 1.0, fastWeight = 1.0;
    vec4 a = vec4(c, 1.0);
    // b: z / w (2026-11-07) the FAST history (AntiLag): its luminance mean and second moment over the last kFast frames; with AntiFirefly
    // (ptaaa-denoiser) z the mean of the clamped samples, w the mean of the raw ones (the firefly limit's own support)
    vec4 b = vec4(luma(c) * luma(c), viewZ, NVR_FIREFLY_K > 0.0 ? luma(c) : l, NVR_FIREFLY_K > 0.0 ? l : l * l);
    // (2026-10-31, rt-ao-sky) at half size (backbone.w 1) the mask over this pixel's 2 x 2 full-size pixels (uv sits on their corner):
    // a thin newly seen edge is not missed by the one tap a point sample takes
    const bool halfSize = mod(pc.backbone.w, 2.0) > 0.5;
    bool soft = false;
    if (pc.backbone.w > 1.5) {   // (Settings.Soft) at half size the texel's 2 x 2 full-size pixels (uv sits on their corner), else its own
        const vec4 written = halfSize ? textureGather(sampler2D(t_soft, NVR_SAMPLER(pc.samplerIndex)), uv, 3)
            : vec4(textureLod(sampler2D(t_soft, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0).a);
        soft = dot(step(written, vec4(0.5)), vec4(1.0)) >= 2.0;
    }
    const bool fresh = !soft && pc.backbone.y > 0.5 && (halfSize
        ? any(greaterThan(textureGather(sampler2D(t_backbone, NVR_SAMPLER(pc.samplerIndex)), uv, 2), vec4(0.5)))
        : textureLod(sampler2D(t_backbone, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0).b > 0.5);
    if (pc.move.w < 0.5 && viewZ > 0.0 && !fresh) {
        vec3 p = (pc.forward.xyz + pc.right.xyz * (2.0 * s.x - 1.0) - pc.up.xyz * (2.0 * s.y - 1.0)) * viewZ + pc.move.xyz;
        float cap = pc.params.x;
        if (pc.backbone.x > 0.5) {
            const vec3 own = textureLod(sampler2D(t_motion, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0).rgb;
            p -= own;
            // (2026-10-31, rt-ao-sky) RT GI: a surface that moves itself (a walking person) has its bounce light change as it moves:
            // a short history there (backbone.z frames, GIMovingHistory) instead of 16 frames trailing behind it
            if (pc.backbone.z > 0.5 && dot(own, own) > 0.0625) cap = min(cap, pc.backbone.z);
        }
        // (2026-11-07) a face talks and changes expression with no motion the reprojection knows of: on people's pixels (the skin
        // templates write captured normal alpha 2: skin, skinned armour, clothes) a short history, so the old mouth and the hat's old
        // shadow do not trail behind (the path tracer's dialogue ghosting; PTDenoisePeopleHistory)
        const float people = pc.peopleCap > 0.5 ? textureLod(sampler2D(t_captured, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0).a : 0.0;
        if (people > 1.5) cap = min(cap, pc.peopleCap);
        // UNOFFICIAL 2026-11-20: ground/wall pixels where a moving actor blocks a direct shadow ray (t_noisy.a < 0 in 3x3) keep short history
        if (actorShadowNear) cap = min(cap, 2.0);
        vec4 clip = pc.prevViewProj * vec4(p, 1.0);
        // (2026-11-07 ptaaa-firstperson) the first-person view model (the mask > 3.5: RtPathTrace.cpp ViewModelPrep, Settings.People) moves
        // with the camera: its history is at the same screen point and depth, not where the camera's motion would carry a world point
        vec2 prevUv = clip.w > 0.0 ? vec2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5) : vec2(-1.0);
        if (people > 3.5) { prevUv = s; clip.w = viewZ; }
        if (clip.w > 0.0) {
            if (all(greaterThanEqual(prevUv, vec2(0.0))) && all(lessThanEqual(prevUv, vec2(1.0)))) {
                vec4 ha, hb;
                if (HistoryAt(prevUv, clip.w, (soft ? max(pc.params.y, 0.25) : pc.params.y) * max(clip.w, 1.0), ha, hb)) {
#ifdef NVR_CONFIDENCE
                    if (pc.confOn > 0.5) { const float k = ConfidenceAt(uv); ha.a = min(ha.a, k * k * pc.params.x); }
#endif
                    // (2026-11-06) HistoryClamp (PTDenoiseClamp): a moving camera / light leaves the history describing light that is no
                    // longer there (a trailing shadow, a lamp's pool lagging); clamp it to what this frame's 3 x 3 neighbourhood allows,
                    // wide (clampK deviations: one noisy path a pixel has a wide spread) so a still picture keeps accumulating.
                    if (pc.clampK > 0.0) {
                        vec3 m1 = vec3(0.0), m2 = vec3(0.0);
                        const vec2 lo = 0.5 * pc.params.zw, hi = max(pc.rect.xy - 0.5 * pc.params.zw, lo);
                        for (int dy = -1; dy <= 1; dy++)
                            for (int dx = -1; dx <= 1; dx++) {
                                const vec3 q3 = textureLod(sampler2D(t_noisy, NVR_SAMPLER(pc.samplerIndex)), clamp(uv + vec2(dx, dy) * pc.params.zw, lo, hi), 0.0).rgb;
                                m1 += q3; m2 += q3 * q3;
                            }
                        m1 /= 9.0; m2 /= 9.0;
                        const vec3 dev = sqrt(max(m2 - m1 * m1, vec3(0.0)));
                        ha.rgb = clamp(ha.rgb, max(m1 - pc.clampK * dev, vec3(0.0)), m1 + pc.clampK * dev);
                    }
                    // (2026-11-07) AntiLag (as NVIDIA NRD ReBLUR's): a fast history (kFast frames) beside the long one. Where the long
                    // history's luminance leaves the fast mean's window (k standard errors of that mean: the LONG history's per-sample
                    // spread / sqrt(kFast), + 2 %), the light really changed there (a shadow moved, a lamp went on, something walked off):
                    // the long history is pulled to the window's edge and restarts at kFast frames, so a trail lasts a few frames, not
                    // PTDenoiseHistory.
                    const float kFast = pc.fastFrames > 0.0 ? pc.fastFrames : 4.0;   // (perf-history: was 4.0)
                    // (ptaaa-denoiser) AntiFirefly with a history: the limit is also k x this pixel's recent raw mean
                    if (NVR_FIREFLY_K > 0.0) {
                        const float limit = max(spatialLimit, NVR_FIREFLY_K * hb.a);
                        c = l > limit ? value * (limit / l) : value;
                    }
                    const float fastLen = min(ha.a, kFast - 1.0) + 1.0;
                    fastWeight = 1.0 / fastLen;
                    const float fastL = mix(hb.b, NVR_FIREFLY_K > 0.0 ? luma(c) : l, 1.0 / fastLen);
                    const float fastL2 = mix(hb.a, NVR_FIREFLY_K > 0.0 ? l : l * l, 1.0 / fastLen);   // (AntiFirefly: the raw mean)
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
                    const float length = min(min(ha.a, cap) + 1.0, pc.params.x);
                    const float alpha = 1.0 / length;
                    a = vec4(mix(ha.rgb, c, alpha), length);
                    b.r = mix(hb.r, luma(c) * luma(c), alpha);
                    excessWeight = alpha;
                }
            }
        }
    }
    o_a = a;
    o_b = b;
    const vec3 excess = FireflyExcess(value - c, c);   // (2026-11-09: FireflySpread's cap)
    o_d = vec4(excess * excessWeight, luma(excess) * fastWeight);
}
