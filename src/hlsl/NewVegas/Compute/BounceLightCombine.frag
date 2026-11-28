// BounceLight pass Combine (Combine(hard), every technique) as a Vulkan pass (needs NVR DXVK): Effects/BounceLight.fx.hlsl line for
// line, full resolution, through the frame chain. With the spill retired its slope, debug views and light-pixel test are off.
// pc.pass_.x: 1 = hard (the BounceLight*HardEdges techniques: UpsampleHardEdges).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "BounceLight.glsl"

layout(set = 1, binding = 0) uniform texture2D t_source;     // TESR_SourceBuffer: the current image (LINEAR, CLAMP)
layout(set = 1, binding = 1) uniform texture2D t_depth;      // TESR_DepthBuffer (POINT, CLAMP)
layout(set = 1, binding = 2) uniform texture2D t_bounce;     // NVR_BounceBuffer: the low-resolution result (POINT, CLAMP)

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

// Depth.hlsl: readDepth (tex2D) and readDepthLod; no mips, both level 0.
float readDepth(vec2 coord) { return POINT_LOD(t_depth, coord).x * farZ; }
float readDepthLod(vec2 coord) { return POINT_LOD(t_depth, coord).x * farZ; }

vec3 UpsampleHardEdges(vec2 coord, float depth, vec2 base, vec2 fraction) {
    float tolerance = max(depth * 0.03, 2.0);
    vec2 px = cb.reciprocalResolution.xy;
    float left = readDepthLod(ClampFullUV(coord - vec2(px.x, 0.0))), right = readDepthLod(ClampFullUV(coord + vec2(px.x, 0.0)));
    float up = readDepthLod(ClampFullUV(coord - vec2(0.0, px.y))), down = readDepthLod(ClampFullUV(coord + vec2(0.0, px.y)));
    float slopeX = (depth - left) * (right - depth) > 0.0 ? min(abs(depth - left), abs(right - depth)) : 0.0;
    float slopeY = (depth - up) * (down - depth) > 0.0 ? min(abs(depth - up), abs(down - depth)) : 0.0;
    tolerance += 1.5 * (slopeX + slopeY) / cb.bounceLayout.x;
    vec3 sum = vec3(0.0);
    float weights = 1.0e-5;
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 2; x++) {
            vec4 tap = POINT_LOD(t_bounce, ClampLowUV((base + vec2(float(x), float(y)) + 0.5) * cb.bounceLayout.zw));
            float weight = (x != 0 ? fraction.x : 1.0 - fraction.x) * (y != 0 ? fraction.y : 1.0 - fraction.y) * (abs(tap.a - depth) <= tolerance ? 1.0 : 0.0);
            sum += tap.rgb * weight;
            weights += weight;
        }
    }
    return sum / weights; // no tap on this surface: 0 / 1e-5 = no light
}

void main() {
    vec2 ruv = ClampFullUV(uv * cb.renderScale.xy);
    vec4 color = LINEAR_LOD(t_source, ruv);
    float depth = readDepth(ruv);
    if (depth < NearFade || depth > DrawDistance) { o_color = color; return; }

    // Depth-aware bilinear upsample: the four nearest low-resolution texels, the ones at another depth counted less.
    vec2 position = (uv * cb.renderScale.xy) / cb.bounceLayout.zw - 0.5;
    vec2 base = floor(position);
    vec2 fraction = position - base;
    vec3 indirect;
    if (pc.pass_.x > 0.5) indirect = UpsampleHardEdges(ruv, depth, base, fraction);
    else {
        float tolerance = max(depth * 0.03, 2.0);
        vec3 sum = vec3(0.0);
        float weights = 1.0e-5;
        for (int y = 0; y < 2; y++) {
            for (int x = 0; x < 2; x++) {
                vec4 tap = POINT_LOD(t_bounce, ClampLowUV((base + vec2(float(x), float(y)) + 0.5) * cb.bounceLayout.zw));
                float weight = (x != 0 ? fraction.x : 1.0 - fraction.x) * (y != 0 ? fraction.y : 1.0 - fraction.y);
                weight /= 1.0 + abs(tap.a - depth) / tolerance;
                sum += tap.rgb * weight;
                weights += weight;
            }
        }
        indirect = sum / weights;
    }

    vec3 lin = linearize(color.rgb);
    float peak = max(max(lin.r, lin.g), max(lin.b, 0.0001));
    vec3 albedo = lin / peak * 0.6;
    lin += Strength * indirect * albedo;
    o_color = delinearize(vec4(lin, color.a));
}
