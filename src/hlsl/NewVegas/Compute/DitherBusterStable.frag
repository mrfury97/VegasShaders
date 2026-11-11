// DitherBuster technique 1 as a Vulkan pass (needs NVR DXVK): Effects/DitherBuster.fx.hlsl DitherOnlyStablePS line for line.
// Soft gate, de-dither judgement and debug view. Pixel = floor(fragment coordinate): the tonemap's whole-pixel convention.
// Push block: TESR_ReciprocalResolution, TESR_DitherBusterData, TESR_DitherBusterGate, TESR_ToneMappingClip, the sampler's index.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer (LINEAR, CLAMP)

layout(push_constant) uniform Push {
    vec4 reciprocalResolution;  // 0: TESR_ReciprocalResolution
    vec4 ditherBusterData;      // 16: TESR_DitherBusterData
    vec4 ditherBusterGate;      // 32: TESR_DitherBusterGate (x softness, y de-dither, z debug view)
    vec4 toneMappingClip;       // 48: TESR_ToneMappingClip (y noise size, z frame counter)
    uint samplerIndex;          // 64: LINEAR, CLAMP
    uint pad[3];                // 68..79
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

#define DB_SAMPLE(p) texture(sampler2D(t_rendered, NVR_SAMPLER(pc.samplerIndex)), p).rgb

// DitherNoise.hlsl: the tonemap's dither, recomputed at a whole pixel.
float ditherGradientNoise(vec2 p) { return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }

float tonemapDitherAt(vec2 pixel) {
    vec2 p = pixel + pc.toneMappingClip.z * vec2(5.588238, 3.235);
    // UNOFFICIAL 2026-11-09 (review 6.6.1): the second read is taken at the pixel's SWAPPED coordinates. At a constant offset it was fract(first + 0.427 or 0.444) for every pixel (the noise is linear in the pixel), so the 'triangular' sum was one number counted twice: uneven grain. Every copy of this noise changes together (SkyDither, the tonemap, Cinema, and the SMAA / DitherBuster passes that take it off again).
    return (ditherGradientNoise(p) + ditherGradientNoise(p.yx + vec2(47.0, 17.0)) - 1.0) * pc.toneMappingClip.y;
}

float lumaWithoutDither(float l, vec2 pixel) {
    return l - tonemapDitherAt(pixel) * clamp(l * 128.0, 0.0, 1.0);
}

void main() {
    vec2 t = pc.reciprocalResolution.xy;
    vec3 c = DB_SAMPLE(uv);
    vec3 up = DB_SAMPLE(uv + vec2(0.0, -t.y));
    vec3 down = DB_SAMPLE(uv + vec2(0.0, t.y));
    vec3 left = DB_SAMPLE(uv + vec2(-t.x, 0.0));
    vec3 right = DB_SAMPLE(uv + vec2(t.x, 0.0));

    vec2 pixel = floor(gl_FragCoord.xy);
    float softness = pc.ditherBusterGate.x;
    bool deDither = pc.ditherBusterGate.y > 0.5;
    bool debugView = pc.ditherBusterGate.z > 0.5;

    float lc = luma(c);
    if (debugView) {
        vec3 clean = c - tonemapDitherAt(pixel) * clamp(lc * 128.0, 0.0, 1.0);
        o_color = vec4(clamp(clean, 0.0, 1.0), 1.0);
        return;
    }

    float lu = luma(up), ld = luma(down), ll = luma(left), lr = luma(right);
    float lcJudge = lc;
    if (deDither) {
        lcJudge = lumaWithoutDither(lc, pixel);
        lu = lumaWithoutDither(lu, pixel + vec2(0.0, -1.0));
        ld = lumaWithoutDither(ld, pixel + vec2(0.0, 1.0));
        ll = lumaWithoutDither(ll, pixel + vec2(-1.0, 0.0));
        lr = lumaWithoutDither(lr, pixel + vec2(1.0, 0.0));
    }
    float differences = abs(lcJudge - lu) + abs(lcJudge - ld) + abs(lcJudge - ll) + abs(lcJudge - lr);
    float iso = max(0.0, differences - 4.0 * length(vec2(lr - ll, lu - ld) * 0.5));

    float threshold = pc.ditherBusterData.z;
    float gate = 0.0;
    if (threshold > 0.0) {
        gate = (softness > 0.0) ? smoothstep(threshold * (1.0 - softness), threshold * (1.0 + softness), iso)
                                : (iso > threshold ? 1.0 : 0.0);
    }
    vec3 mean = (c + up + down + left + right) * 0.2;
    float weight = clamp(pc.ditherBusterData.x, 0.0, 1.0) * gate;
    o_color = vec4(mix(c, mean, weight), 1.0);
}
