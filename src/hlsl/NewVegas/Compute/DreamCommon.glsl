// Dream (needs NVR DXVK): shared constants and helpers matching Effects/Dream.fx.hlsl line for line.
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 2, binding = 0, std140) uniform Constants {
    vec4 level;     // x dream level, y seconds, z hue angle, w warp
    vec4 trails;    // x tracer mix, y tunnel, z datamosh chance, w macroblock chance
    vec4 melt;      // x melt, y fractal variant, z fractal zoom, w colour split
    vec4 finish;    // x vignette, y dither, z extra saturation, w unused
    vec4 grid;      // xy low-res grid, z macroblock seed, w datamosh seed
} cb;

float hash(vec2 p) {
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

float posMod(float x, float y) {
    return x - y * floor(x / y);
}

vec2 fold(vec2 u) {
    return 1.0 - abs(u - 2.0 * floor(u * 0.5) - 1.0);
}

vec3 hueTurn(vec3 c, float h) {
    float y = dot(c, vec3(0.299, 0.587, 0.114));
    float i = dot(c, vec3(0.596, -0.274, -0.322));
    float q = dot(c, vec3(0.211, -0.523, 0.312));
    float s = sin(h);
    float k = cos(h);
    float i2 = i * k - q * s;
    float q2 = i * s + q * k;
    return vec3(y + 0.956 * i2 + 0.621 * q2, y - 0.272 * i2 - 0.647 * q2, y - 1.106 * i2 + 1.703 * q2);
}

vec3 fetch(texture2D tex, uint samp, vec2 uv) {
    return textureLod(sampler2D(tex, NVR_SAMPLER(samp)), uv, 0.0).rgb;
}

float bayer2(vec2 a) {
    a = floor(a);
    return fract(a.x * 0.5 + a.y * a.y * 0.75);
}

float bayer4(vec2 a) {
    return bayer2(0.5 * a) * 0.25 + bayer2(a);
}
