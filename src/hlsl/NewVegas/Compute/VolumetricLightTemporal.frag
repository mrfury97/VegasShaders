// VolumetricLight technique Temporal (3) as a Vulkan pass (needs NVR DXVK): Effects/VolumetricLight.fx.hlsl TemporalPS and
// HistoryAt line for line, half resolution, into the effect's own buffer. effects/VolumetricLight.cpp ExtRender.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_march;      // TESR_VolumetricLightMarch (POINT, CLAMP)
layout(set = 1, binding = 1) uniform texture2D t_history;    // TESR_VolumetricLightHistory (POINT, CLAMP)

#include "VolumetricLight.glsl"

float HistoryAt(vec2 position, float expected01, out vec2 history) {
    position = position / LowTexel - 0.5;
    vec2 base = floor(position);
    vec2 f = position - base;
    vec2 lo = 0.5 * LowTexel;
    vec2 hi = max(cb.renderScale.xy - 0.5 * LowTexel, lo);
    vec4 t00 = POINT_LOD(t_history, clamp((base + 0.5) * LowTexel, lo, hi));
    vec4 t10 = POINT_LOD(t_history, clamp((base + vec2(1.5, 0.5)) * LowTexel, lo, hi));
    vec4 t01 = POINT_LOD(t_history, clamp((base + vec2(0.5, 1.5)) * LowTexel, lo, hi));
    vec4 t11 = POINT_LOD(t_history, clamp((base + 1.5) * LowTexel, lo, hi));
    vec4 w = vec4((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y);
    float tolerance = max(expected01 * 0.05, 0.0002);
    w *= vec4(lessThan(abs(vec4(t00.b, t10.b, t01.b, t11.b) - expected01), vec4(tolerance)));
    float sum = dot(w, vec4(1.0));
    history = (t00.rg * w.x + t10.rg * w.y + t01.rg * w.z + t11.rg * w.w) / max(sum, 1.0e-4);
    return sum;
}

void main() {
    vec2 vpos = floor(gl_FragCoord.xy);
    vec2 coord = (vpos + 0.5) * LowTexel;
    vec2 lo = 0.5 * LowTexel;
    vec2 hi = max(cb.renderScale.xy - 0.5 * LowTexel, lo);
    vec4 current = POINT_LOD(t_march, clamp(coord, lo, hi));

    float tolerance = max(current.b * 0.05, 0.00002);
    vec2 low = current.rg, high = current.rg;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            if (x == 0 && y == 0) continue;
            vec4 tap = POINT_LOD(t_march, clamp(coord + vec2(x, y) * LowTexel, lo, hi));
            if (abs(tap.b - current.b) < tolerance) { low = min(low, tap.rg); high = max(high, tap.rg); }
        }
    }

    vec3 position = toWorld(coord * cb.renderScale.zw) * (current.b * farZ);
    vec4 clip = cb.reproject * vec4(position, 1.0);
    float w = max(clip.w, 0.001);
    vec2 last = vec2(clip.x / w * 0.5 + 0.5, 0.5 - clip.y / w * 0.5);
    vec2 history;
    float found = HistoryAt(last * cb.renderScale.xy, clip.w / farZ, history);
    found *= (all(greaterThan(last, vec2(0.0))) && all(lessThan(last, vec2(1.0)))) ? 1.0 : 0.0;

    vec2 margin = 0.1 * (high - low) + 1.0e-4;
    history = clamp(history, low - margin, high + margin);
    float weight = HistoryWeight * saturate(found);
    o_color = vec4(mix(current.rg, history, weight), current.b, 1.0);
}
