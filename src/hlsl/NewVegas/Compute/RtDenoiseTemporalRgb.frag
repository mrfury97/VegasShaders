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

layout(push_constant) uniform Push {
    mat4 prevViewProj;
    vec4 forward;        // w farZ
    vec4 right;
    vec4 up;
    vec4 move;           // xyz camera move, w 1: reset
    vec4 params;         // x max history, y depth tolerance
    vec4 backbone;       // x 1: subtract the object motion, y 1: the shared disocclusion mask restarts, z the history cap on moving
                         // pixels (0 none), w 1: half size (the mask's 2 x 2)
    vec4 rect;           // (2026-11-03) RenderScale < 1: xy this frame's RenderRect scale (the signal is in the top-left xy part),
                         // zw last frame's (its history's part); 1 when off
    uint samplerIndex;
    float clampK;        // (2026-11-06) HistoryClamp: the history clamped to the new frame's 3 x 3 mean +- clampK x its deviation (0 off)
    uint pad[2];
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_a;
layout(location = 1) out vec4 o_b;

void main() {
    // (2026-11-03) the screen point of this texel (the rect's uv / its scale); past the rect nothing (the a-trous passes skip it)
    const vec2 s = uv / pc.rect.xy;
    if (s.x > 1.0 || s.y > 1.0) { o_a = vec4(0.0); o_b = vec4(0.0); return; }
    const vec3 value = textureLod(sampler2D(t_noisy, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0).rgb;
    const float l = luma(value);
    const float viewZ = textureLod(sampler2D(t_depth, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0).r * pc.forward.w;
    vec4 a = vec4(value, 1.0);
    vec4 b = vec4(l * l, viewZ, 0.0, 0.0);
    // (2026-10-31, rt-ao-sky) at half size (backbone.w 1) the mask over this pixel's 2 x 2 full-size pixels (uv sits on their corner):
    // a thin newly seen edge is not missed by the one tap a point sample takes
    const bool fresh = pc.backbone.y > 0.5 && (pc.backbone.w > 0.5
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
        const vec4 clip = pc.prevViewProj * vec4(p, 1.0);
        if (clip.w > 0.0) {
            const vec2 prevUv = vec2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);
            if (all(greaterThanEqual(prevUv, vec2(0.0))) && all(lessThanEqual(prevUv, vec2(1.0)))) {
                vec4 ha = textureLod(sampler2D(t_historyA, NVR_SAMPLER(pc.samplerIndex)), prevUv * pc.rect.zw, 0.0);
                const vec4 hb = textureLod(sampler2D(t_historyB, NVR_SAMPLER(pc.samplerIndex)), prevUv * pc.rect.zw, 0.0);
                if (abs(hb.g - clip.w) <= pc.params.y * max(clip.w, 1.0) && ha.a > 0.0) {
                    // (2026-11-06) HistoryClamp (PTDenoiseClamp): a moving camera / light leaves the history describing light that is no
                    // longer there (a trailing shadow, a lamp's pool lagging); clamp it to what this frame's 3 x 3 neighbourhood allows,
                    // wide (clampK deviations: one noisy path a pixel has a wide spread) so a still picture keeps accumulating.
                    if (pc.clampK > 0.0) {
                        vec3 m1 = vec3(0.0), m2 = vec3(0.0);
                        for (int dy = -1; dy <= 1; dy++)
                            for (int dx = -1; dx <= 1; dx++) {
                                const vec3 c = textureLod(sampler2D(t_noisy, NVR_SAMPLER(pc.samplerIndex)), uv + vec2(dx, dy) * pc.params.zw, 0.0).rgb;
                                m1 += c; m2 += c * c;
                            }
                        m1 /= 9.0; m2 /= 9.0;
                        const vec3 dev = sqrt(max(m2 - m1 * m1, vec3(0.0)));
                        ha.rgb = clamp(ha.rgb, max(m1 - pc.clampK * dev, vec3(0.0)), m1 + pc.clampK * dev);
                    }
                    const float length = min(min(ha.a, cap) + 1.0, pc.params.x);
                    const float alpha = 1.0 / length;
                    a = vec4(mix(ha.rgb, value, alpha), length);
                    b.r = mix(hb.r, l * l, alpha);
                }
            }
        }
    }
    o_a = a;
    o_b = b;
}
