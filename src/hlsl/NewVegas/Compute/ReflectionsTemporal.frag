// Reflections technique 3 (Temporal, half resolution) as a Vulkan pass: Effects/Reflections.fx.hlsl TemporalPS line for line.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Reflections.glsl"

void main() {
    vec2 vpos = floor(gl_FragCoord.xy);
    vec2 uv = (vpos + 0.5) * LowTexel;
    vec4 current = textureLod(sampler2D(t_trace, POINT_S), uv, 0.0);
    if (any(isnan(current)) || any(isinf(current))) current = vec4(0.0);   // (UNOFFICIAL 2026-10-30: no non-finite value into the history)
    float depth = readDepthLod(uv - 0.5 * (LowTexel - cb.reciprocalResolution.xy));

    vec4 low = current, high = current;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            if (x == 0 && y == 0) continue;
            vec4 tap = textureLod(sampler2D(t_trace, POINT_S), uv + vec2(x, y) * LowTexel, 0.0);
            if (any(isnan(tap)) || any(isinf(tap))) continue;   // (2026-10-30)
            low = min(low, tap); high = max(high, tap);
        }
    }

    vec3 world = toWorld(uv) * depth;
    // UNOFFICIAL 2026-10-29 ObjectMotion (the HLSL's): pass 2 = t_ortho holds TESR_ObjectMotionBuffer (Reflections.cpp ExtRender, technique 3)
    if (pc.pass == 2u) world -= textureLod(sampler2D(t_ortho, POINT_S), uv - 0.5 * (LowTexel - cb.reciprocalResolution.xy), 0.0).xyz;
    vec4 clip = cb.reproject * vec4(world, 1.0);
    float w = max(clip.w, 0.001);
    vec2 last = vec2(clip.x / w * 0.5 + 0.5, 0.5 - clip.y / w * 0.5);
    // UNOFFICIAL 2026-10-30 RT weapon reflections: the weapon moves with the camera: its history is where it is on the screen.
    if (ViewModelAt(uv - 0.5 * (LowTexel - cb.reciprocalResolution.xy))) last = uv;   // (2026-11-09: ViewModelAt: the hands move with the camera too)
    float inside = float(all(greaterThan(last, vec2(0.0))) && all(lessThan(last, vec2(1.0))) && current.a + high.a > 0.0);
    vec4 history = textureLod(sampler2D(t_history, POINT_S), last, 0.0);
    if (any(isnan(history)) || any(isinf(history))) history = current;   // (2026-10-30: a bad texel never survives)
    history = clamp(history, low, high);
    // (2026-11-06) mirrors (roughness under 0.03) take no history: RayGen / Trace trace them the same every frame (no noise), and the
    // history is re-found where the MIRROR's surface was, not where the reflected thing was, so it smeared what a mirror shows.
    // Rough surfaces keep it all (their rays scatter). Vulkan pass only.
    vec2 pixelUV = uv - 0.5 * (LowTexel - cb.reciprocalResolution.xy);
    float roughness;
    float amount = Reflectivity(pixelUV, depth, ViewNormal(pixelUV), roughness);
    float keep = amount > 0.0 && roughness < 0.03 ? 0.0 : 1.0;
    o_color = mix(current, history, HistoryWeight * inside * keep);
}
