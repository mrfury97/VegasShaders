#version 450
// UNOFFICIAL 2026-10-30 DLSS Ray Reconstruction (needs NVR DXVK; effects/TAAFSR.cpp RayReconstruct, docs/next/FORK2-DLSS-RR.md): the last
// step of the RR point. DLSS-D's output (linear HDR) into the frame chain, encoded back into the stored curve the chain holds before the
// tonemap (pow(o, 1 / Linearization), the inverse of DlssRRGuides' decode), except on the first-person weapon: it is drawn without jitter
// (its own camera), so it is put back exactly as it was before RR, as FSRComposite does after DLSS SR.
// (2026-11-07 perf-rr) In a RenderScale rect frame RR's output is the whole screen but the frame before RR (and the weapon's depth) is the
// top-left render rect: those are read at the screen pixel's centre x scale (push rect = 1; the weapon then as the rect's bilinear stretch).
#extension GL_GOOGLE_include_directive : require
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rr;          // RR's output (RGBA16F, linear)
layout(set = 1, binding = 1) uniform texture2D t_source;      // the frame chain's image before RR
layout(set = 1, binding = 2) uniform texture2D t_viewmodel;   // TESR_DepthBufferViewModel (> 0 on the weapon)

layout(push_constant) uniform Push {
    uint pointIndex;            // 0: POINT, CLAMP (the extension writes the heap index)
    float invLinearization;     // 4: 1 / TESR_ToneMapping.w
    uint rawFrame;              // 8: (review 2026-10-30) this frame's RT effects skipped their own denoisers (WantsRaw): the weapon
                                //    takes RR's output too (slightly soft, it is not jittered, but not the raw 1-ray noise on it)
    uint rect;                  // 12: (2026-11-07 perf-rr) 1 = a RenderScale rect frame: t_source and t_viewmodel hold the top-left render rect
    vec2 scale;                 // 16: render size / screen size (that rect's share of the screen)
    vec2 renderSize;            // 24: the render rect in pixels
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

#define POINT_S NVR_SAMPLER(pc.pointIndex)

// (perf-rr) the rect's texel (clamped into it)
vec4 rectTexel(texture2D t, ivec2 p) {
    return texelFetch(sampler2D(t, POINT_S), clamp(p, ivec2(0), ivec2(pc.renderSize) - 1), 0);
}

void main() {
    ivec2 pixel = ivec2(floor(gl_FragCoord.xy));
    vec3 rr = pow(max(texelFetch(sampler2D(t_rr, POINT_S), pixel, 0).rgb, vec3(0.0)), vec3(pc.invLinearization));
    if (pc.rect == 0u) {
        vec4 source = texelFetch(sampler2D(t_source, POINT_S), pixel, 0);
        bool weapon = pc.rawFrame == 0u && texelFetch(sampler2D(t_viewmodel, POINT_S), pixel, 0).x > 0.0;
        o_color = vec4(weapon ? source.rgb : rr, source.a);
        return;
    }
    // (2026-11-07 perf-rr) RR upscaled the render rect to the whole screen; the frame before RR is still the rect. This screen pixel's centre in
    // the rect's pixel centres (core/RrRectPlan.h RectPoint): the nearest texel gives the alpha (a material mark some later passes read) and the
    // weapon test, and the weapon (put back as drawn, not jittered: as at native size) is the rect's bilinear stretch there.
    vec2 t = (vec2(pixel) + 0.5) * pc.scale - 0.5;
    ivec2 nearest = ivec2(floor(t + 0.5));
    vec4 source = rectTexel(t_source, nearest);
    bool weapon = pc.rawFrame == 0u && rectTexel(t_viewmodel, nearest).x > 0.0;
    vec3 color = rr;
    if (weapon) {
        ivec2 i0 = ivec2(floor(t));
        vec2 f = t - vec2(i0);
        vec3 a = mix(rectTexel(t_source, i0).rgb, rectTexel(t_source, i0 + ivec2(1, 0)).rgb, f.x);
        vec3 b = mix(rectTexel(t_source, i0 + ivec2(0, 1)).rgb, rectTexel(t_source, i0 + ivec2(1, 1)).rgb, f.x);
        color = mix(a, b, f.y);
    }
    o_color = vec4(color, source.a);
}
