// Dream, techniques DreamPost / DreamPostDirect (needs NVR DXVK): Effects/Dream.fx.hlsl Post line for line.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "DreamCommon.glsl"

layout(set = 1, binding = 0) uniform texture2D t_source;

layout(push_constant) uniform Push {
    uint linearSampler;
    uint pad[3];
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

vec3 meltPass(vec2 uvCoord, float f, float variant, float zoom, float t) {
    vec2 aspect = vec2(cb.grid.x / cb.grid.y, 1.0) * 2.4;
    float a = t * 0.11 + variant * 1.7;
    vec2 c;
    if (variant < 1.5)      c = 0.7885 * vec2(cos(a), sin(a));
    else if (variant < 2.5) c = vec2(-0.8, 0.156) + 0.05 * vec2(sin(a * 1.3), cos(a));
    else if (variant < 3.5) c = vec2(-0.123, 0.745) + 0.03 * vec2(sin(a), cos(a * 0.7));
    else                    c = vec2(0.285, 0.01) + 0.02 * vec2(cos(a * 2.0), sin(a * 2.0));

    vec2 m = vec2(1.0, 0.0) - 4.0 * c;
    float ml = length(m);
    vec2 root = vec2(sqrt(0.5 * (ml + m.x)), (m.y >= 0.0 ? 1.0 : -1.0) * sqrt(max(0.0, 0.5 * (ml - m.x))));
    vec2 fixedPoint = 0.5 * (vec2(1.0, 0.0) + root);

    vec2 z = (uvCoord - 0.5) * aspect / zoom + fixedPoint * (1.0 - 1.0 / zoom);
    float steps = 0.0;
    float escaped = 0.0;
    for (int k = 0; k < 48; k++) {
        z = vec2(z.x * z.x - z.y * z.y, 2.0 * z.x * z.y) + c;
        if (dot(z, z) > 64.0) { escaped = 1.0; break; }
        steps += 1.0;
    }
    float smoothSteps = steps + 1.0 - log2(max(0.5 * log2(dot(z, z)), 1.0e-3));
    vec2 target = escaped > 0.5 ? vec2(smoothSteps / 10.0 + t * 0.03, 0.5 + atan(z.y, z.x) / 6.2831) : 0.5 + z / aspect;
    vec3 s = fetch(t_source, pc.linearSampler, fold(mix(uvCoord, target, f)));
    float grey = dot(s, vec3(0.3, 0.59, 0.11));
    vec3 palette = 0.5 + 0.5 * cos(6.2831 * (smoothSteps / 24.0 + grey + vec3(0.0, 0.33, 0.67) + t * 0.05));
    return mix(s, s * 0.55 + palette * 0.6, f * f * f * (0.3 + 0.7 * escaped));
}

void main() {
    float t = cb.level.y;
    float warp = cb.level.w;
    vec2 grid = cb.grid.xy;

    vec2 c = uv - 0.5;
    float r = length(c);
    float turn = warp * 0.35 * sin(t * 0.4) * (1.0 - smoothstep(0.0, 0.6, r));
    float s = sin(turn);
    float k = cos(turn);
    vec2 uvp = 0.5 + vec2(c.x * k + c.y * s, c.y * k - c.x * s);
    uvp += warp * 0.006 * vec2(sin(uvp.y * 40.0 + t * 2.0), cos(uvp.x * 34.0 + t * 1.6));

    vec2 block = floor(uvp * grid / 8.0);
    float roll = hash(block + cb.grid.z);
    float blockChance = cb.trails.w;
    bool macro = roll < blockChance;
    if (macro) uvp = (block + 0.5) * 8.0 / grid;

    vec2 o = vec2(cb.melt.w, 0.0);
    vec3 col = vec3(fetch(t_source, pc.linearSampler, uvp + o).r,
                    fetch(t_source, pc.linearSampler, uvp).g,
                    fetch(t_source, pc.linearSampler, uvp - o).b);
    if (macro) {
        col = floor(col * 4.0) / 4.0 + (roll < blockChance * 0.2727 ? vec3(-0.15, 0.2, -0.15) : vec3(0.0));
    }

    if (cb.melt.x > 0.001) {
        col = meltPass(uvp, cb.melt.x, cb.melt.y, cb.melt.z, t);
    }

    if (cb.level.z != 0.0) col = hueTurn(col, cb.level.z);
    float grey = dot(col, vec3(0.299, 0.587, 0.114));
    col = mix(vec3(grey), col, 1.0 + cb.finish.z);
    col *= 1.0 - cb.finish.x * smoothstep(0.35, 0.85, r);

    float threshold = bayer4(floor(uvp * grid)) - 0.5;
    vec3 crushed = floor(col * 31.0 + 0.5 + threshold) / 31.0;
    o_color = vec4(mix(col, crushed, cb.finish.y), 1.0);
}
