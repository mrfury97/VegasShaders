// UNOFFICIAL 2026-11-02 the reference path tracer's picture (core\RtPathTrace.cpp RenderDebug, [Main.Main.RayTracing] ReferencePT): the
// accumulated mean (linear, the game's units: NVR_RtPathTrace from the RT helper's RtPathTrace.comp) over the finished frame, x PTExposure,
// clamped and gamma-encoded like the game's LDR picture. PTSplit: the left part of the screen keeps the game's own frame (discard).
// Compile: glslangValidator -V --target-env vulkan1.1 -o RtPathTraceShow.spv RtPathTraceShow.frag
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_traced;   // NVR_RtPathTrace
layout(set = 1, binding = 1) uniform texture2D t_albedo;   // (2026-11-03) PTDenoise: the first surface's colour (demodulation), a 0 = sky / frame
layout(set = 1, binding = 2) uniform texture2D t_raw;      // (2026-11-03) PTDenoise: the trace before the denoiser (the sky / frame pixels: exact)
layout(set = 1, binding = 3) uniform texture2D t_viewModel; // (2026-11-07) TESR_DepthBufferViewModel (a stand-in with keepViewModel 0)
layout(set = 1, binding = 4) uniform texture2D t_frame;     // (2026-11-07) PTDetail: the game's frame before the replace (x^(1/2.2)), a stand-in when off
layout(set = 1, binding = 5) uniform texture2D t_depth;     // (2026-11-07) TESR_DepthBuffer (view z / farZ): the detail's same-surface test
// (2026-11-07 ptaaa-denoiser) PTDenoiseSplit (modulate bit 2): the specular light (denoised, and its raw trace) divided by its specular
// albedo, and that albedo (r, g | b, roughness): the picture = the diffuse light x the colour + the specular light x its albedo
layout(set = 1, binding = 6) uniform texture2D t_spec;
layout(set = 1, binding = 7) uniform texture2D t_specRaw;
layout(set = 1, binding = 8) uniform texture2D t_specAlbedoRG;
layout(set = 1, binding = 9) uniform texture2D t_specAlbedoB;

layout(push_constant) uniform Push {
    float exposure;      // 0
    uint samplerIndex;   // 4: POINT, CLAMP
    float split;         // 8: 0 = the whole screen; 0.5 = the right half only
    float gamma;         // 12: 1 / 2.2; 1 = the debug views (albedo, normals) as they are
    uint modulate;       // 16: bit 0 = x t_albedo (the traced light came demodulated through the denoiser); bit 1 = into the HDR scene
                         // (PTReplace: not clamped above 1); bit 2 (2026-11-07 ptaaa-denoiser) + the specular light x its albedo
    float uvScaleX;      // 20: (2026-11-03) the trace covers the top-left uvScale part (RenderScale < 1, the overlay after the upscale)
    float uvScaleY;      // 24
    uint keepViewModel;  // 28: (2026-11-07) 1 = the first-person model's pixels (view-model depth < 0.9) keep the frame's own; 2 = the same
                         // with reversed depth (> 0.01); 0 = none
    float detail;        // 32: (2026-11-07) PTDetail: how much of the game's own fine shading the traced light takes (0 off, 1 all)
    float detailRadius;  // 36: its local average's reach (pixels)
    vec2 texel;          // 40: 1 / the target's size
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

void main() {
    if (uv.x < pc.split) discard;
    const vec2 tuv = uv * vec2(pc.uvScaleX, pc.uvScaleY);
    // (2026-11-07) the first-person model is not in the traced scene (the trace saw the world behind it): it keeps the game's pixel
    if (pc.keepViewModel != 0u) {
        const float vm = textureLod(sampler2D(t_viewModel, NVR_SAMPLER(pc.samplerIndex)), tuv, 0.0).r;
        if (pc.keepViewModel == 1u ? vm < 0.9 : vm > 0.01) discard;
    }
    vec3 c = textureLod(sampler2D(t_traced, NVR_SAMPLER(pc.samplerIndex)), tuv, 0.0).rgb * pc.exposure;
    if ((pc.modulate & 1u) != 0u) {
        const vec4 a = textureLod(sampler2D(t_albedo, NVR_SAMPLER(pc.samplerIndex)), tuv, 0.0);
        const vec3 raw = textureLod(sampler2D(t_raw, NVR_SAMPLER(pc.samplerIndex)), tuv, 0.0).rgb * pc.exposure;   // (demodulated by 1 there)
        vec3 lit = c * a.rgb, rawLit = raw;
        if ((pc.modulate & 4u) != 0u) {   // (ptaaa-denoiser) the two signals back together
            const vec3 sa = vec3(textureLod(sampler2D(t_specAlbedoRG, NVR_SAMPLER(pc.samplerIndex)), tuv, 0.0).rg,
                                 textureLod(sampler2D(t_specAlbedoB, NVR_SAMPLER(pc.samplerIndex)), tuv, 0.0).r);
            lit += textureLod(sampler2D(t_spec, NVR_SAMPLER(pc.samplerIndex)), tuv, 0.0).rgb * pc.exposure * sa;
            rawLit += textureLod(sampler2D(t_specRaw, NVR_SAMPLER(pc.samplerIndex)), tuv, 0.0).rgb * pc.exposure * sa;   // (0 off surfaces)
        }
        c = mix(rawLit, lit, clamp(a.a, 0.0, 1.0));
    }
    // (2026-11-07, testers: "we're losing a lot of texture quality when RT is on") the path tracer shades the first surface with its
    // smooth vertex normals (the G-buffer has no normal maps) and its denoiser smooths the light over a few pixels, so a road lost its
    // cracks, bumps and glints. The game's own frame has them: its shading (its picture / the surface colour) against the same
    // surface's average a few pixels round (4 taps, same depth) is that fine detail; the traced light takes it (clamped 0.5 - 2), its
    // own large-scale light (bounces, soft shadows, sky) unchanged. As the RT GI apply's detail (AmbientLight.fx.hlsl, 2026-11-07).
    if (pc.detail > 0.0 && (pc.modulate & 1u) != 0u) {
        const vec4 a0 = textureLod(sampler2D(t_albedo, NVR_SAMPLER(pc.samplerIndex)), tuv, 0.0);
        const float z0 = textureLod(sampler2D(t_depth, NVR_SAMPLER(pc.samplerIndex)), tuv, 0.0).r;
        if (a0.a >= 0.5 && z0 > 0.0 && z0 < 0.9999) {
            const float s0 = luma(pow(max(textureLod(sampler2D(t_frame, NVR_SAMPLER(pc.samplerIndex)), tuv, 0.0).rgb, vec3(0.0)), vec3(2.2))) / max(luma(a0.rgb), 0.02);
            float sum = 0.0, n = 0.0;
            for (int k = 0; k < 4; k++) {
                const vec2 o = (k == 0 ? vec2(1.0, 0.0) : k == 1 ? vec2(-1.0, 0.0) : k == 2 ? vec2(0.0, 1.0) : vec2(0.0, -1.0)) * pc.detailRadius * pc.texel;
                const vec2 tt = tuv + o;
                const vec4 at = textureLod(sampler2D(t_albedo, NVR_SAMPLER(pc.samplerIndex)), tt, 0.0);
                const float zt = textureLod(sampler2D(t_depth, NVR_SAMPLER(pc.samplerIndex)), tt, 0.0).r;
                if (at.a < 0.5 || abs(zt - z0) > 0.03 * z0) continue;
                sum += luma(pow(max(textureLod(sampler2D(t_frame, NVR_SAMPLER(pc.samplerIndex)), tt, 0.0).rgb, vec3(0.0)), vec3(2.2))) / max(luma(at.rgb), 0.02);
                n += 1.0;
            }
            if (n > 0.5 && s0 > 0.0 && sum > 0.0) c *= mix(1.0, clamp(s0 / (sum / n), 0.5, 2.0), clamp(pc.detail, 0.0, 1.0));
        }
    }
    c = (pc.modulate & 2u) != 0u ? max(c, vec3(0.0)) : clamp(c, 0.0, 1.0);
    o_color = vec4(pow(c, vec3(pc.gamma)), 1.0);
}
