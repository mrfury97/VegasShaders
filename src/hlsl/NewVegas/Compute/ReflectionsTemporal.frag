// Reflections technique 3 (Temporal, half resolution) as a Vulkan pass: Effects/Reflections.fx.hlsl TemporalPS line for line.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Reflections.glsl"

void main() {
    vec2 vpos = floor(gl_FragCoord.xy);
    vec2 uv = (vpos + 0.5) * LowTexel;
    vec4 current = textureLod(sampler2D(t_trace, POINT_S), uv, 0.0);
    if (any(isnan(current)) || any(isinf(current))) current = vec4(0.0);   // (UNOFFICIAL 2026-10-30: no non-finite value into the history)
    vec2 pixelUV = uv - 0.5 * (LowTexel - cb.reciprocalResolution.xy);
    float depth = readDepthLod(pixelUV);
    bool centerWeapon = WeaponAt(pixelUV);

    bool hasValid = current.a > 1.0e-4;
    vec3 curC = hasValid ? max(current.rgb / current.a, vec3(0.0)) : vec3(0.0);
    vec3 lowC = curC, highC = curC;
    float lowA = current.a, highA = current.a;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            if (x == 0 && y == 0) continue;
            vec2 tapUV = uv + vec2(x, y) * LowTexel;
            vec2 tapPixelUV = tapUV - 0.5 * (LowTexel - cb.reciprocalResolution.xy);
            if (WeaponAt(tapPixelUV) != centerWeapon) continue;
            vec4 tap = textureLod(sampler2D(t_trace, POINT_S), tapUV, 0.0);
            if (any(isnan(tap)) || any(isinf(tap))) continue;   // (2026-10-30)
            lowA = min(lowA, tap.a); highA = max(highA, tap.a);
            if (tap.a > 1.0e-4) {
                vec3 tc = max(tap.rgb / tap.a, vec3(0.0));
                if (!hasValid) { lowC = tc; highC = tc; hasValid = true; }
                else { lowC = min(lowC, tc); highC = max(highC, tc); }
            }
        }
    }

    vec3 world = toWorld(uv) * depth;
    // UNOFFICIAL 2026-10-29 ObjectMotion (the HLSL's): pass 2 = t_ortho holds TESR_ObjectMotionBuffer (Reflections.cpp ExtRender, technique 3)
    if (pc.pass == 2u) world -= textureLod(sampler2D(t_ortho, POINT_S), uv - 0.5 * (LowTexel - cb.reciprocalResolution.xy), 0.0).xyz;
    vec4 clip = cb.reproject * vec4(world, 1.0);
    float w = max(clip.w, 0.001);
    // (2026-11-24) the history is in the RenderScale rect too: read and edge-tested there, as the HLSL's TemporalPS (screenToRect, the
    // rect's size as the edge). Native frames: renderScale is 1, unchanged. (Today a rect frame runs the D3D9 Temporal instead, Reflections.cpp
    // RtPassesWanted, so this only keeps the port right for when that changes.)
    vec2 last = screenToRect(vec2(clip.x / w * 0.5 + 0.5, 0.5 - clip.y / w * 0.5));
    // UNOFFICIAL 2026-10-30 RT weapon reflections: the weapon moves with the camera: its history is where it is on the screen.
    if (ViewModelAt(uv - 0.5 * (LowTexel - cb.reciprocalResolution.xy))) last = uv;   // (2026-11-09: ViewModelAt: the hands move with the camera too)
    float inside = float(all(greaterThan(last, vec2(0.0))) && all(lessThan(last, cb.renderScale.xy)) && highA > 0.0);
    vec4 history = textureLod(sampler2D(t_history, POINT_S), last, 0.0);
    if (any(isnan(history)) || any(isinf(history))) history = current;   // (2026-10-30: a bad texel never survives)
    if (hasValid && history.a > 1.0e-4) {
        vec3 hc = clamp(max(history.rgb / history.a, vec3(0.0)), lowC, highC);
        float hl = dot(hc, vec3(0.2126, 0.7152, 0.0722));
        float maxTameL = BrightestReflection / (1.0 + BrightestReflection);
        if (hl > maxTameL) hc *= maxTameL / hl;
        float ha = clamp(history.a, lowA, highA);
        history = vec4(hc * ha, ha);
    } else {
        history = vec4(0.0);
    }
    // (2026-11-06) mirrors (roughness under 0.03) take no history: RayGen / Trace trace them the same every frame (no noise), and the
    // history is re-found where the MIRROR's surface was, not where the reflected thing was, so it smeared what a mirror shows.
    // Rough surfaces keep it all (their rays scatter). Vulkan pass only.
    float roughness;
    float amount = Reflectivity(pixelUV, depth, ViewNormal(pixelUV), roughness);
    float keep = amount > 0.0 && roughness < 0.03 ? 0.0 : 1.0;
    o_color = mix(current, history, HistoryWeight * inside * keep);
}
