// SkinScattering as a Vulkan pass (needs NVR DXVK): Effects/SkinScattering.fx.hlsl line for line, both passes (push firstPass
// picks the direction, as BlurHorizontal and BlurVertical do). effects/SkinScattering.cpp ExtRender.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

#define SAMPLES 21   // SkinScatteringEffect::KernelSamples
#define farZ (cb.cameraData.y)
#define SAMPLE_LOD(t, coord) textureLod(sampler2D(t, NVR_SAMPLER(pc.samplerIndex)), (coord), 0.0)

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer
layout(set = 1, binding = 1) uniform texture2D t_depth;      // TESR_DepthBuffer
layout(set = 1, binding = 2) uniform texture2D t_scatter;    // TESR_SkinScatterBuffer
layout(set = 1, binding = 3) uniform texture2D t_albedo;     // TESR_SkinAlbedoBuffer
layout(set = 1, binding = 4) uniform texture2D t_source;     // TESR_SourceBuffer

// The constants the HLSL reads, filled by the host each frame (the kernel is too big for push data). Matrices: cb.m[i][j]
// is the HLSL's m[i][j] (the D3DX matrix as stored).
layout(set = 2, binding = 0, std140) uniform Constants {
    vec4 scatterData;              // TESR_SkinScatterData: x game units per kernel unit, y depth follow, z distance tolerance, w albedo detail
    vec4 scatterKernel[SAMPLES];   // TESR_SkinScatterKernel: rgb weight, a offset; [0] is the centre
    vec4 reciprocalResolution;     // TESR_ReciprocalResolution: xy 1 / screen size
    vec4 scatterDebug;             // TESR_SkinScatterDebug: x debug view, y 1 to turn the kernel per pixel, z this frame's turn
    vec4 cameraData;               // TESR_CameraData: x near, y far
    mat4 projectionTransform;      // TESR_ProjectionTransform
    mat4 viewTransform;            // TESR_ViewTransform
    vec4 renderScale;              // TESR_RenderScale
} cb;

vec2 clampToRectIfOn(vec2 coord, vec2 px) {
    vec2 hi = cb.renderScale.xy - 0.5 * px;
    return (cb.renderScale.x < 0.9999 || cb.renderScale.y < 0.9999) ? clamp(coord, 0.5 * px, max(hi, 0.5 * px)) : coord;
}

layout(push_constant) uniform Push {
    uint samplerIndex;             // 0: the point sampler's heap index (filled by the host)
    uint firstPass;                // 4: 1 for the horizontal pass, 0 for the vertical
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

// Includes/Depth.hlsl toWorld
vec3 toWorld(vec2 tex) {
    vec2 s = tex * cb.renderScale.zw;
    vec3 v = vec3(cb.viewTransform[0][2], cb.viewTransform[1][2], cb.viewTransform[2][2]);
    v += (1.0 / cb.projectionTransform[0][0] * (2.0 * s.x - 1.0)) * vec3(cb.viewTransform[0][0], cb.viewTransform[1][0], cb.viewTransform[2][0]);
    v += (-1.0 / cb.projectionTransform[1][1] * (2.0 * s.y - 1.0)) * vec3(cb.viewTransform[0][1], cb.viewTransform[1][1], cb.viewTransform[2][1]);
    return v;
}

// The colour test: the scene about as bright as the skin's own pass left it, or brighter by up to 60%.
bool ColourStillSkin(float sceneLuma, float drawnLuma) {
    return sceneLuma > drawnLuma * 0.97 - 0.01 && sceneLuma < drawnLuma * 1.6 + 0.02;
}

// The distance test: the scene's depth is the skin's, or something sits just in front of it (within 1.5 units).
bool DistanceStillSkin(float skinDistance, float sceneDistance) {
    float gap = skinDistance - sceneDistance;
    return abs(gap) < sceneDistance * cb.scatterData.z + 0.02 || (gap > 0.0 && gap < 1.5);
}

// The highlights test: each channel of the scene holds at least the highlights' share (a quarter of it allowed below, for fog).
bool HighlightsStillThere(vec3 share, vec3 sceneColor) {
    return all(lessThanEqual(share, sceneColor + share * 0.25 + vec3(0.02)));
}

// Skin where all tests pass.
bool IsSkin(vec4 scatter, float drawnLuma, vec3 sceneColor, float viewDepth, vec2 coord) {
    float dist = viewDepth * length(toWorld(coord));
    return scatter.a > 0.0
        && DistanceStillSkin(scatter.a, dist)
        && ColourStillSkin(luma(sceneColor), drawnLuma)
        && HighlightsStillThere(scatter.rgb, sceneColor);
}

// The scene is gamma encoded (the skin shaders encode with sqrt); the blur works on light.
vec3 Decode(vec3 c) { return c * c; }
vec3 Encode(vec3 c) { return sqrt(max(c, vec3(0.0))); }

// Dark texels take no part, smoothly.
vec3 Participation(vec3 albedo) { return smoothstep(vec3(0.01), vec3(0.04), albedo); }
vec3 AlbedoFactor(vec3 albedo) { return pow(max(albedo, vec3(1e-4)), vec3(cb.scatterData.w)); }

// Light per unit (albedo ^ detail), the quantity that diffuses.
vec3 ToLight(vec3 color, vec3 highlights, vec3 albedoGamma) {
    vec3 albedo = Decode(albedoGamma);
    vec3 light = Decode(max(color - highlights, vec3(0.0))) * Participation(albedo) / AlbedoFactor(albedo);
    return min(light, vec3(16.0));
}

// Pass 1's output can exceed 1; stored compressed.
vec3 Pack(vec3 x) { return x / (1.0 + x); }
vec3 Unpack(vec3 x) { return x / max(1.0 - x, vec3(1e-4)); }

vec4 SkinBlur(vec2 coord, vec2 dir, bool firstPass) {
    vec4 colorM = SAMPLE_LOD(t_rendered, coord);
    vec4 scatterM = SAMPLE_LOD(t_scatter, coord);
    // Port (2026-10-15): no skin drawn here: nothing else to read.
    if (!(scatterM.a > 0.0)) return colorM;
    vec4 albedoM = SAMPLE_LOD(t_albedo, coord);
    float depthM = SAMPLE_LOD(t_depth, coord).r * farZ;
    // The scene as drawn: pass 1 reads it directly, pass 2 from the copy taken before pass 1.
    vec4 sceneM = firstPass ? colorM : SAMPLE_LOD(t_source, coord);

    // Debug views (pass 2 only).
    if (!firstPass && cb.scatterDebug.x > 0.0 && cb.scatterDebug.x < 2.5 && scatterM.a > 0.0) {
        vec3 tint;
        if (cb.scatterDebug.x > 1.5) {
            float ratio = luma(sceneM.rgb) / max(albedoM.a, 1e-3);
            tint = ratio < 1.0 ? mix(vec3(0.0, 1.0, 0.0), vec3(0.0, 0.2, 1.0), saturate((1.0 - ratio) * 5.0))
                               : mix(vec3(0.0, 1.0, 0.0), vec3(1.0, 0.0, 0.0), saturate((ratio - 1.0) * 5.0));
        }
        else {
            float dist = depthM * length(toWorld(coord));
            bool distanceOK = DistanceStillSkin(scatterM.a, dist);
            bool colourOK = ColourStillSkin(luma(sceneM.rgb), albedoM.a) && HighlightsStillThere(scatterM.rgb, sceneM.rgb);
            tint = !distanceOK ? vec3(1.0, 0.0, 0.0) : (!colourOK ? vec3(0.0, 0.2, 1.0) : vec3(0.0, 1.0, 0.0));
        }
        return vec4(mix(sceneM.rgb, tint, 0.6), sceneM.a);
    }

    if (!IsSkin(scatterM, albedoM.a, sceneM.rgb, depthM, coord)) return colorM;

    vec3 lightM = firstPass ? ToLight(colorM.rgb, scatterM.rgb, albedoM.rgb) : Unpack(colorM.rgb);

    // The kernel turned by a random angle per pixel (TAA on), the same in both passes.
    if (cb.scatterDebug.y > 0.5) {
        vec2 pixel = floor(coord / cb.reciprocalResolution.xy) + 5.588238 * cb.scatterDebug.z;
        float angle = fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715)))) * 6.2831853;
        float c = cos(angle), s = sin(angle);
        dir = vec2(dir.x * c - dir.y * s, dir.x * s + dir.y * c);
    }

    // One kernel unit on screen: the projection of scatterData.x game units at this depth.
    vec2 stepUV = dir * vec2(cb.projectionTransform[0][0], cb.projectionTransform[1][1]) * (0.5 * cb.scatterData.x / max(depthM, 1.0));
    stepUV *= cb.renderScale.xy;

    vec3 blurred = lightM * cb.scatterKernel[0].rgb;
    for (int i = 1; i < SAMPLES; i++) {
        vec2 sampleUV = clampToRectIfOn(coord + cb.scatterKernel[i].a * stepUV, cb.reciprocalResolution.xy);
        vec4 color = SAMPLE_LOD(t_rendered, sampleUV);
        vec4 scatter = SAMPLE_LOD(t_scatter, sampleUV);
        vec4 albedo = SAMPLE_LOD(t_albedo, sampleUV);
        float depth = SAMPLE_LOD(t_depth, sampleUV).r * farZ;
        vec3 scene = firstPass ? color.rgb : SAMPLE_LOD(t_source, sampleUV).rgb;

        vec3 light = firstPass ? ToLight(color.rgb, scatter.rgb, albedo.rgb) : Unpack(color.rgb);

        // Light does not scatter across a depth jump, nor in from anything that is not skin.
        float follow = saturate(cb.scatterData.y * cb.projectionTransform[1][1] * abs(depthM - depth));
        light = mix(light, lightM, follow * follow);
        light = IsSkin(scatter, albedo.a, scene, depth, sampleUV) ? light : lightM;

        blurred += cb.scatterKernel[i].rgb * light;
    }

    if (firstPass) return vec4(Pack(blurred), colorM.a);

    // Back to colour: the blurred light times the albedo factor, then the highlights.
    vec4 original = sceneM;
    vec3 albedo = Decode(albedoM.rgb);
    vec3 participation = Participation(albedo);
    // Port 2026-10-19: never more highlights back than the pixel held.
    vec3 highlights = min(scatterM.rgb, original.rgb);
    vec3 light = blurred * AlbedoFactor(albedo) * participation
               + Decode(original.rgb - highlights) * (1.0 - participation * participation);
    vec3 result = Encode(light) + highlights;

    // View 3, on skin: how much the blur changed each pixel.
    if (cb.scatterDebug.x > 2.5) {
        float change = luma(result) - luma(original.rgb);
        vec3 tint = change > 0.0 ? vec3(1.0, 0.35, 0.25) : vec3(0.25, 0.45, 1.0);
        float amount = saturate(length(result - original.rgb) * 8.0);
        return vec4(luma(original.rgb) * 0.25 + tint * amount, original.a);
    }
    return vec4(result, original.a);
}

void main() {
    bool firstPass = pc.firstPass != 0u;
    vec2 ruv = uv * cb.renderScale.xy;
    o_color = SkinBlur(ruv, firstPass ? vec2(1.0, 0.0) : vec2(0.0, 1.0), firstPass);
}
