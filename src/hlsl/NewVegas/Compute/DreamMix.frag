// Dream, technique DreamMix (needs NVR DXVK): Effects/Dream.fx.hlsl MixPS line for line.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "DreamCommon.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;
layout(set = 1, binding = 1) uniform texture2D t_history;

layout(push_constant) uniform Push {
    uint linearSampler;
    uint pad[3];
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

vec3 splitNow(vec2 q, float s) {
    vec2 o = vec2(0.003 * s, 0.0);
    return vec3(fetch(t_rendered, pc.linearSampler, q + o).r,
                fetch(t_rendered, pc.linearSampler, q).g,
                fetch(t_rendered, pc.linearSampler, q - o).b);
}

vec3 tunnel(vec2 uv0, float stare, float t) {
    vec2 aspect = vec2(cb.grid.x / cb.grid.y, 1.0);
    vec2 c = (uv0 - 0.5) * aspect;
    float r = max(length(c), 1.0e-4);
    float a = atan(c.y, c.x);

    float twist = stare * 1.6 * exp(-r * 2.5) + 0.05 * stare * sin(r * 14.0 - t * 2.0);
    vec2 swirled = vec2(cos(a + twist), sin(a + twist)) * r * (1.0 - 0.12 * stare * sin(t * 0.8 + r * 10.0));
    vec3 swirl = splitNow(0.5 + swirled / aspect, stare);

    const float outer = 0.62;
    const float ln2 = 0.693147;
    float hexR = max(max(abs(c.x) * 0.8660254 + abs(c.y) * 0.5, abs(c.y)), 1.0e-4);
    float depth = log(hexR / outer) / ln2 + t * 0.15 * stare;
    float ring = floor(depth);
    float inRing = depth - ring;
    const float wedge = 6.2831 / 6.0;
    float wa = a + ring * 0.5 + t * 0.05;
    wa = abs(posMod(wa, wedge) - wedge * 0.5) + 0.35;
    vec2 dir = vec2(cos(wa), sin(wa));
    dir /= max(abs(dir.x) * 0.8660254 + abs(dir.y) * 0.5, abs(dir.y));
    vec3 nested = hueTurn(splitNow(0.5 + dir * outer * exp(ln2 * (inRing - 1.0)) / aspect, stare), ring * 0.9 + t * 0.2);
    nested *= smoothstep(0.0, 0.05, inRing) * (1.0 - smoothstep(0.95, 1.0, inRing)) * 0.85 + 0.15;

    vec3 d = mix(swirl, nested, smoothstep(0.45, 1.0, stare));
    d = hueTurn(d, stare * 1.4 * sin(t * 0.3));
    float grey = dot(d, vec3(0.3, 0.59, 0.11));
    return mix(vec3(grey), d, 1.0 + 0.9 * stare);
}

void main() {
    float t = cb.level.y;
    float stare = cb.trails.y;

    vec3 col = mix(fetch(t_rendered, pc.linearSampler, uv), fetch(t_history, pc.linearSampler, uv), cb.trails.x);
    if (stare > 0.0) {
        col = mix(col, tunnel(uv, stare, t), smoothstep(0.0, 0.25, stare));
    }

    vec2 block = floor(uv * cb.grid.xy / 8.0);
    if (hash(block + cb.grid.w) < cb.trails.z) {
        col = fetch(t_history, pc.linearSampler, uv + vec2((hash(block * 1.7) - 0.5) * 0.04, 0.004));
    }
    o_color = vec4(col, 1.0);
}
