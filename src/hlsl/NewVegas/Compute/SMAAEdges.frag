// SMAA's edge detection as a Vulkan pass (needs NVR DXVK): Effects/SMAA.fx.hlsl techniques LumaEdgeDetection, ColorEdgeDetection,
// DepthEdgeDetection, LumaDepthEdgeDetection and LumaDepthEdgeDetectionStable (pc.mode 0-4) line for line, with the parts of
// Includes/SMAA.hlsl (SMAA_HLSL_3) and Includes/DitherNoise.hlsl they use inlined. effects/SMAA.cpp ExtRender.
// D3D9 clears the edges target, then each pass discards the pixels without an edge and marks the others in the stencil (REPLACE 1)
// for the weight pass. Without a stencil: a discarded pixel writes the cleared value (0) and alpha carries the mark (1 = the pass
// did not discard; the D3D9 pass writes alpha 0 and nothing reads the edges' alpha but SMAAWeights.frag). VPOS = floor(gl_FragCoord.xy).
// 2026-10-26 pc.flags 1 (NVR DXVK 1.6, NvrDxvkStencil): as D3D9, a real discard; SMAA.cpp cleared the target and binds the stencil
// (REPLACE 1 where a pixel is written).
// Every sampler of the .fx is LINEAR, CLAMP (SMAASamplePoint is tex2D through it under SMAA_HLSL_3); the targets have no mips.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer
layout(set = 1, binding = 1) uniform texture2D t_depth;      // TESR_DepthBuffer

layout(push_constant) uniform Push {
    vec4 resolution;            // 0: TESR_SMAAResolution (SMAA_RT_METRICS: 1/w, 1/h, w, h)
    vec4 data;                  // 16: TESR_SMAAData (x edge threshold, y relative depth threshold, z max search steps, w subpixel shift)
    vec4 depthData;             // 32: TESR_SMAADepthData (x depth texel offset, y local contrast adaptation factor)
    vec4 pred;                  // 48: TESR_SMAAPred (x predication, y scale, z strength, w de-dither)
    vec4 toneMappingClip;       // 64: TESR_ToneMappingClip (y tonemap dither size, z its frame)
    uint samplerIndex;          // 80: LINEAR, CLAMP
    uint pointSamplerIndex;     // 84: (weights pass only)
    uint mode;                  // 88: 0 luma, 1 color, 2 depth, 3 luma or depth, 4 the same with the aastab options
    uint flags;                 // 92: 1 = discard (the stencil route)
} pc;

// UNOFFICIAL 2026-10-26: the variant .spv fixes this (-DSMAA_EDGE_MODE, Compute/variants.txt, EffectRecord::ExtVariant), as the D3D9
// techniques are compiled one per mode; without it the push block decides (the general shader, the fallback).
#ifdef SMAA_EDGE_MODE
#define EDGE_MODE uint(SMAA_EDGE_MODE)
#else
#define EDGE_MODE pc.mode
#endif

layout(location = 0) in vec2 uv_in;
layout(location = 0) out vec4 o_color;

#define SMAA_THRESHOLD (pc.data.x > 0.0 ? pc.data.x : 0.05)
#define SMAA_LOCAL_CONTRAST_ADAPTATION_FACTOR (pc.depthData.y > 0.0 ? pc.depthData.y : 2.0)

vec4 SamplePoint(vec2 coord) { return texture(sampler2D(t_rendered, NVR_SAMPLER(pc.samplerIndex)), coord); }
vec4 SampleDepth(vec2 coord) { return textureLod(sampler2D(t_depth, NVR_SAMPLER(pc.samplerIndex)), coord, 0.0); }

// Includes/DitherNoise.hlsl
float DitherGradientNoise(vec2 p) {
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}
float TonemapDitherAt(vec2 pixel) {
    vec2 p = pixel + pc.toneMappingClip.z * vec2(5.588238, 3.235);
    // UNOFFICIAL 2026-11-09 (review 6.6.1): the second read is taken at the pixel's SWAPPED coordinates. At a constant offset it was fract(first + 0.427 or 0.444) for every pixel (the noise is linear in the pixel), so the 'triangular' sum was one number counted twice: uneven grain. Every copy of this noise changes together (SkyDither, the tonemap, Cinema, and the SMAA / DitherBuster passes that take it off again).
    return (DitherGradientNoise(p) + DitherGradientNoise(p.yx + vec2(47.0, 17.0)) - 1.0) * pc.toneMappingClip.y;
}
float LumaWithoutDither(float luma, vec2 pixel) {
    return luma - TonemapDitherAt(pixel) * saturate(luma * 128.0);
}

// discard in the D3D9 pass: the cleared value, no stencil mark
#define DISCARD { if ((pc.flags & 1u) != 0u) discard; o_color = vec4(0.0); return; }
#define MARKED(edges) o_color = vec4(edges, 0.0, 1.0)

// SMAALumaEdgeDetectionPS (SMAA_PREDICATION 0)
void LumaEdgeDetection(vec2 texcoord, vec4 offset[3]) {
    vec2 threshold = vec2(SMAA_THRESHOLD, SMAA_THRESHOLD);
    vec3 weights = vec3(0.2126, 0.7152, 0.0722);
    float L = dot(SamplePoint(texcoord).rgb, weights);
    float Lleft = dot(SamplePoint(offset[0].xy).rgb, weights);
    float Ltop = dot(SamplePoint(offset[0].zw).rgb, weights);
    vec4 delta;
    delta.xy = abs(L - vec2(Lleft, Ltop));
    vec2 edges = step(threshold, delta.xy);
    if (dot(edges, vec2(1.0, 1.0)) == 0.0)
        DISCARD
    float Lright = dot(SamplePoint(offset[1].xy).rgb, weights);
    float Lbottom = dot(SamplePoint(offset[1].zw).rgb, weights);
    delta.zw = abs(L - vec2(Lright, Lbottom));
    vec2 maxDelta = max(delta.xy, delta.zw);
    float Lleftleft = dot(SamplePoint(offset[2].xy).rgb, weights);
    float Ltoptop = dot(SamplePoint(offset[2].zw).rgb, weights);
    delta.zw = abs(vec2(Lleft, Ltop) - vec2(Lleftleft, Ltoptop));
    maxDelta = max(maxDelta.xy, delta.zw);
    float finalDelta = max(maxDelta.x, maxDelta.y);
    edges.xy *= step(finalDelta, SMAA_LOCAL_CONTRAST_ADAPTATION_FACTOR * delta.xy);
    MARKED(edges);
}

// SMAAColorEdgeDetectionPS (SMAA_PREDICATION 0)
void ColorEdgeDetection(vec2 texcoord, vec4 offset[3]) {
    vec2 threshold = vec2(SMAA_THRESHOLD, SMAA_THRESHOLD);
    vec4 delta;
    vec3 C = SamplePoint(texcoord).rgb;
    vec3 Cleft = SamplePoint(offset[0].xy).rgb;
    vec3 t = abs(C - Cleft);
    delta.x = max(max(t.r, t.g), t.b);
    vec3 Ctop = SamplePoint(offset[0].zw).rgb;
    t = abs(C - Ctop);
    delta.y = max(max(t.r, t.g), t.b);
    vec2 edges = step(threshold, delta.xy);
    if (dot(edges, vec2(1.0, 1.0)) == 0.0)
        DISCARD
    vec3 Cright = SamplePoint(offset[1].xy).rgb;
    t = abs(C - Cright);
    delta.z = max(max(t.r, t.g), t.b);
    vec3 Cbottom = SamplePoint(offset[1].zw).rgb;
    t = abs(C - Cbottom);
    delta.w = max(max(t.r, t.g), t.b);
    vec2 maxDelta = max(delta.xy, delta.zw);
    vec3 Cleftleft = SamplePoint(offset[2].xy).rgb;
    t = abs(C - Cleftleft);
    delta.z = max(max(t.r, t.g), t.b);
    vec3 Ctoptop = SamplePoint(offset[2].zw).rgb;
    t = abs(C - Ctoptop);
    delta.w = max(max(t.r, t.g), t.b);
    maxDelta = max(maxDelta.xy, delta.zw);
    float finalDelta = max(maxDelta.x, maxDelta.y);
    edges.xy *= step(finalDelta, SMAA_LOCAL_CONTRAST_ADAPTATION_FACTOR * delta.xy);
    MARKED(edges);
}

// SMAADepthEdges
vec2 SMAADepthEdges(vec2 texcoord, vec4 offset[3]) {
    vec2 o = pc.depthData.x * pc.resolution.xy;
    float P     = SampleDepth(texcoord + o).r;
    float Pleft = SampleDepth(offset[0].xy + o).r;
    float Ptop  = SampleDepth(offset[0].zw + o).r;

    vec2 delta = abs(vec2(P) - vec2(Pleft, Ptop));
    float depthThreshold = (pc.data.y > 0.0) ? pc.data.y : 0.005;
    return step(vec2(depthThreshold * max(P, 0.000001)), delta);
}

// DX9_SMAADepthEdgeDetectionPS
void DepthEdgeDetection(vec2 texcoord, vec4 offset[3]) {
    vec2 edges = SMAADepthEdges(texcoord, offset);

    if (dot(edges, vec2(1.0, 1.0)) == 0.0)
        DISCARD

    MARKED(edges);
}

// DX9_SMAALumaDepthEdgeDetectionPS
void LumaDepthEdgeDetection(vec2 texcoord, vec4 offset[3]) {
    vec3 weights = vec3(0.2126, 0.7152, 0.0722);
    float L = dot(SamplePoint(texcoord).rgb, weights);
    float Lleft = dot(SamplePoint(offset[0].xy).rgb, weights);
    float Ltop = dot(SamplePoint(offset[0].zw).rgb, weights);
    vec2 delta = abs(L - vec2(Lleft, Ltop));
    vec2 lumaEdges = step(vec2(SMAA_THRESHOLD, SMAA_THRESHOLD), delta);
    vec2 depthEdges = SMAADepthEdges(texcoord, offset);

    if (dot(lumaEdges, vec2(1.0, 1.0)) == 0.0) {
        if (dot(depthEdges, vec2(1.0, 1.0)) == 0.0)
            DISCARD
        MARKED(depthEdges);
        return;
    }

    float Lright = dot(SamplePoint(offset[1].xy).rgb, weights);
    float Lbottom = dot(SamplePoint(offset[1].zw).rgb, weights);
    vec2 maxDelta = max(delta, abs(L - vec2(Lright, Lbottom)));
    float Lleftleft = dot(SamplePoint(offset[2].xy).rgb, weights);
    float Ltoptop = dot(SamplePoint(offset[2].zw).rgb, weights);
    maxDelta = max(maxDelta, abs(vec2(Lleft, Ltop) - vec2(Lleftleft, Ltoptop)));
    lumaEdges *= step(vec2(max(maxDelta.x, maxDelta.y)), SMAA_LOCAL_CONTRAST_ADAPTATION_FACTOR * delta);

    MARKED(max(lumaEdges, depthEdges));
}

// SMAALumaTap
float SMAALumaTap(vec2 coord, vec2 pixel) {
    float l = dot(SamplePoint(coord).rgb, vec3(0.2126, 0.7152, 0.0722));
    if (pc.pred.w > 0.5)
        l = LumaWithoutDither(l, pixel);
    return l;
}

// DX9_SMAALumaDepthEdgeDetectionStablePS
void LumaDepthEdgeDetectionStable(vec2 vpos, vec2 texcoord, vec4 offset[3]) {
    float L = SMAALumaTap(texcoord, vpos);
    float Lleft = SMAALumaTap(offset[0].xy, vpos + vec2(-1.0, 0.0));
    float Ltop = SMAALumaTap(offset[0].zw, vpos + vec2(0.0, -1.0));
    vec2 delta = abs(L - vec2(Lleft, Ltop));
    vec2 depthEdges = SMAADepthEdges(texcoord, offset);
    vec2 threshold = vec2(SMAA_THRESHOLD, SMAA_THRESHOLD);
    if (pc.pred.x > 0.5)
        threshold *= pc.pred.y * (1.0 - pc.pred.z * depthEdges);
    vec2 lumaEdges = step(threshold, delta);

    if (dot(lumaEdges, vec2(1.0, 1.0)) == 0.0) {
        if (dot(depthEdges, vec2(1.0, 1.0)) == 0.0)
            DISCARD
        MARKED(depthEdges);
        return;
    }

    float Lright = SMAALumaTap(offset[1].xy, vpos + vec2(1.0, 0.0));
    float Lbottom = SMAALumaTap(offset[1].zw, vpos + vec2(0.0, 1.0));
    vec2 maxDelta = max(delta, abs(L - vec2(Lright, Lbottom)));
    float Lleftleft = SMAALumaTap(offset[2].xy, vpos + vec2(-2.0, 0.0));
    float Ltoptop = SMAALumaTap(offset[2].zw, vpos + vec2(0.0, -2.0));
    maxDelta = max(maxDelta, abs(vec2(Lleft, Ltop) - vec2(Lleftleft, Ltoptop)));
    lumaEdges *= step(vec2(max(maxDelta.x, maxDelta.y)), SMAA_LOCAL_CONTRAST_ADAPTATION_FACTOR * delta);

    MARKED(max(lumaEdges, depthEdges));
}

void main() {
    // DX9_SMAAEdgeDetectionVS: texcoord += SMAA_SUBPIXEL_SHIFT; SMAAEdgeDetectionVS
    vec2 texcoord = uv_in + pc.data.w * pc.resolution.xy;
    vec4 offset[3];
    offset[0] = pc.resolution.xyxy * vec4(-1.0, 0.0, 0.0, -1.0) + texcoord.xyxy;
    offset[1] = pc.resolution.xyxy * vec4( 1.0, 0.0, 0.0,  1.0) + texcoord.xyxy;
    offset[2] = pc.resolution.xyxy * vec4(-2.0, 0.0, 0.0, -2.0) + texcoord.xyxy;

    if (EDGE_MODE == 0u) LumaEdgeDetection(texcoord, offset);
    else if (EDGE_MODE == 1u) ColorEdgeDetection(texcoord, offset);
    else if (EDGE_MODE == 2u) DepthEdgeDetection(texcoord, offset);
    else if (EDGE_MODE == 3u) LumaDepthEdgeDetection(texcoord, offset);
    else LumaDepthEdgeDetectionStable(floor(gl_FragCoord.xy), texcoord, offset);
}
