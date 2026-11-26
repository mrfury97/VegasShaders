// UNOFFICIAL 2026-10-31 (NVR DXVK, RT reflections round 17; [Shaders.Reflections.RT] SkyPanorama): the sky panorama, a 512 x 512 (128 before 2026-11-26)
// hemi-octahedral map of the upper sky (z up; the cloud sky probe's layout) that reflections take for the sky where a ray meets
// nothing. Every texel whose direction is on screen this frame AND lands on a sky pixel (depth at the far plane, not the weapon)
// takes the screen's colour there: the game's sky dome as drawn (its gradient, sun glow, the flat cloud layers, the sun) with NVR's
// volumetric clouds already on it. Every other texel keeps what it last saw, fading back over about a minute to the estimate: the
// sky gradient, with the cloud sky probe's clouds on it when that runs (pc.pass 1). Bindings as every Reflections pass; in this pass
// t_trace is LAST frame's panorama and t_cloudSky the cloud probe (Reflections.cpp RenderSkyPano). pc.cube.w 1: start over.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Reflections.glsl"

void main() {
    vec2 p = uv * 2.0 - 1.0;
    vec3 v = vec3((p.x + p.y) * 0.5, (p.x - p.y) * 0.5, 0.0);
    v.z = max(1.0 - abs(v.x) - abs(v.y), 0.0);
    vec3 d = normalize(v + vec3(0.0, 0.0, 1.0e-4));

    // The estimate: the gradient (no horizon fade: the panorama is the sky itself) and the probe's clouds.
    vec3 estimate = mix(linearize(cb.horizonColor.rgb), linearize(cb.skyColor.rgb), sqrt(saturate(d.z)));
    if (pc.pass == 1u) {
        vec4 clouds = textureLod(sampler2D(t_cloudSky, LINEAR_S), uv, 0.0);
        estimate = estimate * clouds.a + clouds.rgb * cb.material2.z;
    }

    vec4 last = CubeReset > 0.5 ? vec4(0.0) : textureLod(sampler2D(t_trace, POINT_S), uv, 0.0);
    // (2026-11-26 mirror puddles) a texel not seen lately (most often high above the view: a puddle seen from above mirrors the sky over
    // the player) takes the sky last seen lower on the same bearing (4 steps down toward the horizon) before the gradient: the gradient is
    // a simpler model than the sky shader's and came out paler and brighter than the drawn sky (bench run 20261126-195058-slotA).
    if (CubeReset < 0.5 && last.a < 0.5) {
        vec2 h = length(d.xy) > 1.0e-4 ? normalize(d.xy) : vec2(1.0, 0.0);
        float el = asin(clamp(d.z, 0.0, 1.0));
        // (filtered and weighted, nearest first: single texel reads made hard blocks of cloud in the mirror, bench run 20261126-200330-slotA)
        vec3 sum = vec3(0.0);
        float weight = 0.0;
        for (int k = 1; k <= 4; k++) {
            float e = el * (1.0 - 0.2 * float(k));
            vec3 dk = vec3(h * cos(e), sin(e));
            vec3 vk = dk / (abs(dk.x) + abs(dk.y) + dk.z);
            vec4 lower = textureLod(sampler2D(t_trace, LINEAR_S), vec2(vk.x + vk.y, vk.x - vk.y) * 0.5 + 0.5, 0.0);
            float w = smoothstep(0.25, 0.75, lower.a) * (1.0 - saturate(weight));
            sum += lower.rgb * w;
            weight += w;
        }
        if (weight > 1.0e-3) estimate = mix(estimate, sum / weight, saturate(weight));
    }
    float seen = max(last.a - 1.0 / 3600.0, 0.0);   // what was seen fades back to the estimate (~1 minute at 60 fps)
    vec3 colour = last.rgb;

    vec3 view = WorldToViewDir(d);
    if (view.z > 0.05) {
        vec2 at = ToScreen(view);
        vec2 s01 = rectToScreen(at);   // (UNOFFICIAL 2026-11-03 RenderScale < 1: at is in the rect; the edge test on the screen point)
        if (all(greaterThan(s01, vec2(0.002))) && all(lessThan(s01, vec2(0.998))) && readDepthLod(at) >= farZ * 0.98 && !IsWeapon(at)) {
            vec3 screen = linearize(textureLod(sampler2D(t_rendered, LINEAR_S), at, 0.0).rgb);
            colour = seen > 0.0 ? mix(colour, screen, 0.5) : screen;   // a little smoothing against flicker (rain streaks, dither)
            seen = 1.0;
        }
    }
    // Seen within the last half minute or so: as seen; older, it leans toward the estimate (the estimate alone once never seen or faded out).
    o_color = vec4(mix(estimate, colour, smoothstep(0.0, 0.5, seen)), seen);
    if (any(isnan(o_color)) || any(isinf(o_color))) o_color = vec4(estimate, 0.0);
}
