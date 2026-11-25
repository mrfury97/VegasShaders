// UNOFFICIAL 2026-10-29 shared RT denoiser (core\RtDenoise.h; docs\next\FORK-RT-SUN.md "Denoiser"), pass 1 of 2: temporal
// accumulation (SVGF / ReBLUR-style). The noisy signal (.r) is blended with last frame's history reprojected through the depth and last
// frame's camera (camera-relative view-projection + the camera's move); disoccluded pixels (depth plane mismatch) restart.
// Output (RGBA16F history): r = value, g = second moment, b = history length (frames), a = linear view depth (for the next frame).
// Compile: glslangValidator -V --target-env vulkan1.1 -o RtDenoiseTemporal.spv RtDenoiseTemporal.frag
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_noisy;     // the RT pass' output (.r)
layout(set = 1, binding = 1) uniform texture2D t_history;   // last frame's output of this pass
layout(set = 1, binding = 2) uniform texture2D t_depth;     // TESR_DepthBuffer (.r = view z / farZ)
// UNOFFICIAL 2026-10-31 DenoiseBackbone (RtBackbone.frag): the objects' own move (B1) and the shared disocclusion mask, or stand-ins
layout(set = 1, binding = 3) uniform texture2D t_motion;    // TESR_ObjectMotionBuffer (rgb = the surface's world move since last frame)
layout(set = 1, binding = 4) uniform texture2D t_backbone;  // RtBackbone (b = 1: disoccluded)

layout(push_constant) uniform Push {
    mat4 prevViewProj;   // last frame's camera-relative view-projection (D3D rows uploaded as columns: clip = M * p)
    vec4 forward;        // xyz camera forward, w farZ
    vec4 right;          // xyz right / P00
    vec4 up;             // xyz up / P11
    vec4 move;           // xyz this frame's camera position - last frame's (world units), w 1: reset (no history)
    vec4 params;         // x max history frames, y depth tolerance (relative), z 1 / width, w 1 / height
    vec4 backbone;       // x 1: subtract the object motion (t_motion), y 1: the shared disocclusion mask restarts (t_backbone),
                         // z (RGB pass only), w 1: half size (the mask over the pixel's 2 x 2 full-size ones)
    vec4 rect;           // (2026-11-03) RenderScale < 1: xy this frame's RenderRect scale (the signal is in the top-left xy part),
                         // zw last frame's (its history's part); 1 when off
    uint samplerIndex;   // the point-clamp sampler (written by the binding)
    uint pad[3];
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

void main() {
    // (2026-11-03) the screen point of this texel (the rect's uv / its scale); past the rect nothing (a = 0: the a-trous passes skip it)
    const vec2 s = uv / pc.rect.xy;
    if (s.x > 1.0 || s.y > 1.0) { o_color = vec4(0.0); return; }
    const float value = textureLod(sampler2D(t_noisy, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0).r;
    const float linear01 = textureLod(sampler2D(t_depth, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0).r;
    const float viewZ = (linear01 > 0.0 && linear01 < 0.999) ? min(linear01 * pc.forward.w, 60000.0) : 0.0;
    vec4 result = vec4(value, value * value, 1.0, viewZ);
    // (2026-10-31, rt-ao-sky) at half size (backbone.w 1) the mask over this pixel's 2 x 2 full-size pixels (uv sits on their corner):
    // a thin newly seen edge is not missed by the one tap a point sample takes
    const bool fresh = pc.backbone.y > 0.5 && (pc.backbone.w > 0.5
        ? any(greaterThan(textureGather(sampler2D(t_backbone, NVR_SAMPLER(pc.samplerIndex)), uv, 2), vec4(0.5)))
        : textureLod(sampler2D(t_backbone, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0).b > 0.5);
    if (pc.move.w < 0.5 && viewZ > 0.0 && !fresh) {
        // this pixel last frame: camera-relative now, + the camera's move = camera-relative then (- the surface's own move: B1)
        vec3 p = (pc.forward.xyz + pc.right.xyz * (2.0 * s.x - 1.0) - pc.up.xyz * (2.0 * s.y - 1.0)) * viewZ + pc.move.xyz;
        if (pc.backbone.x > 0.5) p -= textureLod(sampler2D(t_motion, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0).rgb;
        const vec4 clip = pc.prevViewProj * vec4(p, 1.0);
        if (clip.w > 0.0) {
            const vec2 prevUv = vec2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);
            if (all(greaterThanEqual(prevUv, vec2(0.0))) && all(lessThanEqual(prevUv, vec2(1.0)))) {
                const vec4 history = textureLod(sampler2D(t_history, NVR_SAMPLER(pc.samplerIndex)), prevUv * pc.rect.zw, 0.0);
                // the same surface: last frame's depth there matches this point's depth as seen from last frame
                if (abs(history.a - clip.w) <= pc.params.y * max(clip.w, 1.0) && history.b > 0.0) {
                    const float length = min(history.b + 1.0, pc.params.x);
                    const float alpha = 1.0 / length;
                    result.r = mix(history.r, value, alpha);
                    result.g = mix(history.g, value * value, alpha);
                    result.b = length;
                }
            }
        }
    }
    o_color = result;
}
