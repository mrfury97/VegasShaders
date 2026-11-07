// Reflections techniques 2 / 4 (Blur X / Blur Y, half resolution) as a Vulkan pass: Effects/Reflections.fx.hlsl BlurAxis
// line for line. pc.pass 0: Blur X (reads TESR_ReflectionsTemp), 1: Blur Y (reads TESR_ReflectionsTrace).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Reflections.glsl"

// UNOFFICIAL 2026-10-29 RT reflections: ReflectionsRTResolve's hit-distance share (0 contact .. 1 far), on frames RT ran (pc.cube.w =
// the narrowest radius share, 0 = not this frame: the blur exactly as before).
layout(set = 1, binding = 22) uniform texture2D t_rtSpread;

vec4 Source(vec2 coord) {
    return pc.pass == 0u ? textureLod(sampler2D(t_temp, POINT_S), coord, 0.0) : textureLod(sampler2D(t_trace, POINT_S), coord, 0.0);
}

void main() {
    vec2 vpos = floor(gl_FragCoord.xy);
    vec2 axis = pc.pass == 0u ? vec2(1.0, 0.0) : vec2(0.0, 1.0);
    vec2 uv = (vpos + 0.5) * LowTexel;
    vec4 centre = Source(uv);
    vec2 pixelUV = uv - 0.5 * (LowTexel - cb.reciprocalResolution.xy);
    float depth = readDepthLod(pixelUV);
    float roughness;
    float amount = Reflectivity(pixelUV, depth, ViewNormal(pixelUV), roughness);
    if (amount <= 0.0) { o_color = centre; return; }

    // Wider on rough ground, narrow on wet ground, so puddles stay sharp.
    // (2026-11-06) mirrors get no blur: below roughness 0.03 none, full from 0.15 (a mirror needs no denoising: its rays do not scatter;
    // the old 0.5 floor blurred a perfect mirror ~6 half-res pixels each way). Rough floors keep the full radius.
    float radius = BlurRadius * saturate(0.5 + roughness * 2.0) * smoothstep(0.03, 0.15, roughness);
    if (pc.cube.w > 0.0) radius *= mix(pc.cube.w, 1.0, textureLod(sampler2D(t_rtSpread, POINT_S), uv, 0.0).r);   // RT: sharp at contact
    float sigma = max(radius * 0.5, 0.5);
    float falloff = -0.5 / (sigma * sigma);
    float tolerance = depth * 0.03 + 4.0;
    vec4 sum = centre;
    float weights = 1.0;
    for (int n = 1; n <= 8; n++) {
        float i = float(n);
        float gauss = i <= radius ? exp(falloff * i * i) : 0.0;
        vec2 ua = uv + axis * i * LowTexel, ub = uv - axis * i * LowTexel;
        float da = readDepthLod(ua - 0.5 * (LowTexel - cb.reciprocalResolution.xy)), db = readDepthLod(ub - 0.5 * (LowTexel - cb.reciprocalResolution.xy));
        float wa = gauss / (1.0 + abs(da - depth) / tolerance);
        float wb = gauss / (1.0 + abs(db - depth) / tolerance);
        sum += Source(ua) * wa + Source(ub) * wb;
        weights += wa + wb;
    }
    o_color = sum / weights;
}
