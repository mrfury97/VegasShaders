// UNOFFICIAL 2026-11-15 (rt-denoise-inputs, docs/next/RT-DENOISE-INPUTS.md) shared RT denoiser (core\RtDenoise.h), RGB mode, Settings.Confidence:
// HISTORY CONFIDENCE at 1/5 resolution, the input NVIDIA's NRD documentation asks for (IN_DIFF_CONFIDENCE: "essential for responsiveness",
// from a gradient between the stored and freshly traced light at about 1/5 resolution). Written from the published designs only (NRD's
// README; Schied et al. 2018, "Gradient Estimation for Real-Time Adaptive Temporal Filtering", A-SVGF), no code of theirs.
// Why not a true re-trace with last frame's random numbers (A-SVGF's forward-projected gradient samples): the path tracer's light is NOT a
// function of the seed alone (ReSTIR DI's temporal reservoirs, the radiance cache, the GI reuse carry history of their own), so a replayed
// path does not give last frame's value on an unchanged scene. Instead each 5 x 5 tile compares this frame's 25 fresh samples with the
// reprojected long history of the same 25 surface points (RtDenoiseTemporalRgb.frag's camera + object motion; nearest texel; no view-model
// rule and no DenoiseBackbone mask): their means
// differ by more than the fresh mean's own noise (k standard errors, + the history's) only where the light really changed (a lamp went
// out or moved, a door opened, an object turned). The relative excess is the tile's gradient; confidence = 1 - 2 x gradient.
// RtDenoiseTemporalRgb.frag's -conf variant reads it (bilinear over the tiles) and cuts the history to confidence^2 x MaxHistory there,
// so the change shows in one frame instead of after the fast history (AntiLag) catches up; the a-trous pass' history fix then smooths the
// fresh pixels. One fragment a tile (the target is ceil(w / 5) x ceil(h / 5), RG16F: r confidence, g the gradient).
// Compile: tools/nvr-dxvk/compile-compute.sh RtDenoiseConfidence (glslangValidator -V --target-env vulkan1.1)
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_noisy;      // rgb (this frame's samples)
layout(set = 1, binding = 1) uniform texture2D t_historyA;   // last frame's temporal output 0 (rgb, history length)
layout(set = 1, binding = 2) uniform texture2D t_depth;      // TESR_DepthBuffer
layout(set = 1, binding = 3) uniform texture2D t_historyB;   // last frame's output 1 (luma second moment, view depth, ...)
layout(set = 1, binding = 4) uniform texture2D t_motion;     // TESR_ObjectMotionBuffer (a stand-in when backbone.x is 0)

layout(push_constant) uniform Push {   // RtDenoiseTemporalRgb.frag's block (the same camera), + conf
    mat4 prevViewProj;
    vec4 forward;        // w farZ
    vec4 right;
    vec4 up;
    vec4 move;           // xyz camera move, w 1: reset
    vec4 params;         // x max history, y depth tolerance, zw 1 / the signal's size
    vec4 backbone;       // x 1: subtract the object motion
    vec4 rect;           // xy this frame's RenderRect scale, zw last frame's
    uint samplerIndex;
    float clampK, peopleCap, antiLag, fireflyK, fastFrames;   // (unused here)
    // (scalars, not a vec4: std430 would align a vec4 to byte 208; RtDenoise.cpp ConfPush writes them at 200-215)
    float confK;         // the fresh mean's noise allowance in standard errors
    float confTile;      // the tile size (5)
    float confGain;      // the gain (2)
    float confPad;
} pc;

#define SMP NVR_SAMPLER(pc.samplerIndex)

// last frame's A and B at the NEAREST texel of the same surface (the temporal pass blends 2 x 2; a tile mean does not need it: 2 reads
// instead of 8 a sample, the pass' cost)
bool HistoryAt(vec2 prevUv, float expectedZ, float tolerance, out vec4 ha, out vec4 hb) {
    const vec2 size = 1.0 / pc.params.zw;
    const vec2 q = clamp(floor(prevUv * pc.rect.zw * size), vec2(0.0), size - 1.0);
    const vec2 quv = (q + 0.5) / size;
    hb = textureLod(sampler2D(t_historyB, SMP), quv, 0.0);
    ha = vec4(0.0);
    if (!(abs(hb.g - expectedZ) <= tolerance)) return false;
    ha = textureLod(sampler2D(t_historyA, SMP), quv, 0.0);
    return ha.a > 0.0;
}

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_conf;

void main() {
    const int T = int(pc.confTile + 0.5);
    const ivec2 tile = ivec2(gl_FragCoord.xy);
    const vec2 size = 1.0 / pc.params.zw;
    float n = 0.0, sumN = 0.0, sumN2 = 0.0, sumH = 0.0, invLen = 0.0;
    if (pc.move.w < 0.5) {
        for (int y = 0; y < T; y++)
            for (int x = 0; x < T; x++) {
                const vec2 q = vec2(tile * T + ivec2(x, y));
                if (any(greaterThanEqual(q, size))) continue;
                const vec2 at = (q + 0.5) * pc.params.zw;
                const vec2 s = at / pc.rect.xy;
                if (s.x > 1.0 || s.y > 1.0) continue;
                const float viewZ = textureLod(sampler2D(t_depth, SMP), at, 0.0).r * pc.forward.w;
                if (!(viewZ > 0.0)) continue;
                vec3 p = (pc.forward.xyz + pc.right.xyz * (2.0 * s.x - 1.0) - pc.up.xyz * (2.0 * s.y - 1.0)) * viewZ + pc.move.xyz;
                if (pc.backbone.x > 0.5) p -= textureLod(sampler2D(t_motion, SMP), at, 0.0).rgb;
                const vec4 clip = pc.prevViewProj * vec4(p, 1.0);
                if (!(clip.w > 0.0)) continue;
                const vec2 prevUv = vec2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);
                if (any(lessThan(prevUv, vec2(0.0))) || any(greaterThan(prevUv, vec2(1.0)))) continue;
                vec4 ha, hb;
                if (!HistoryAt(prevUv, clip.w, pc.params.y * max(clip.w, 1.0), ha, hb)) continue;
                const float l = luma(textureLod(sampler2D(t_noisy, SMP), at, 0.0).rgb);
                const float hl = luma(ha.rgb);
                if (!(abs(l) < 6.0e4) || !(abs(hl) < 6.0e4)) continue;
                n += 1.0; sumN += l; sumN2 += l * l; sumH += hl;
                invLen += 1.0 / max(ha.a, 1.0);   // the history's own noise: the fresh per-sample variance / its length (hb.r is 0 after a cut)
            }
    }
    float conf = 1.0, grad = 0.0;
    if (n >= 4.0) {
        const float meanN = sumN / n, meanH = sumH / n;
        const float varN = max(sumN2 / n - meanN * meanN, 0.0);
        const float noise = sqrt(varN / n + varN * invLen / (n * n));
        const float excess = max(abs(meanN - meanH) - pc.confK * noise, 0.0);
        grad = excess / max(max(meanN, meanH), 1.0e-4);
        conf = clamp(1.0 - pc.confGain * grad, 0.0, 1.0);
    }
    o_conf = vec4(conf, grad, 0.0, 0.0);
}
