// UNOFFICIAL 2026-11-07 RT reflection denoiser, core 0 ("ours"), pass 3 of 4: the spatial blur (ReBLUR-style, written from the
// published design). Over the accumulated signal, 12 golden-angle taps (turned every frame) in a disc of the specular lobe's size on
// screen: (hit distance / (view z + hit distance)) x the lobe's angle (2 roughness^2) / the angle of a grid pixel, capped at
// params2.z, times params2.w + (1 - params2.w) x the hit's share (sharp where the reflected thing touches the surface), and shrunk as
// the history grows (x sqrt(4 / history length), at least 0.25: a settled history needs little blur). Weights: the surface's plane,
// normal and roughness, and the hit distance (a reflection of something else blurs no further).
// Inputs: t_in0 this frame's moments (x accumulated hit distance, y history length), t_in1 the accumulated signal, t_in2 this frame's guide.
// Outputs: 0 the blurred signal, 1 (x the radius in grid pixels, for the debug view).
// Compile: glslangValidator -V --target-env vulkan1.1 -o RtReflDenoiseOursBlur.spv RtReflDenoiseOursBlur.frag
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "RtReflDenoise.glsl"

layout(location = 0) out vec4 o_blurred;
layout(location = 1) out vec4 o_radius;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec2 uv = (vec2(p) + 0.5) * pc.size.zw;
    vec4 centre = Fetch(t_in1, p);
    o_blurred = centre;
    o_radius = vec4(0.0);
    vec4 g = Fetch(t_in2, p);
    if (g.x <= 0.0 || IsMirror(g.y)) return;
    vec4 m = Fetch(t_in0, p);
    float z = g.x, r = g.y, hit = max(m.x, 0.0), n = max(m.y, 1.0);
    vec3 normal = OctDecodeN(g.zw);
    vec3 point = PointAt(uv, z);
    float a = r * r;
    float pixelAngle = 2.0 * length(pc.right.xyz) * pc.size.z;
    float lobe = hit / (z + hit) * 2.0 * a / pixelAngle;
    float radius = min(lobe, pc.params2.z) * clamp(sqrt(4.0 / n), 0.25, 1.0);
    o_radius = vec4(radius, lobe, 0.0, 0.0);
    if (radius < 0.5) return;
    float rotation = 6.2831853 * fract(52.9829189 * fract(dot(vec2(p) + pc.params3.w * 5.588238, vec2(0.06711056, 0.00583715))));
    vec4 sum = centre;
    float weights = 1.0;
    float planeTolerance = 0.01 * z + 2.0;
    for (int k = 0; k < 12; k++) {
        float t = sqrt((float(k) + 0.5) / 12.0);
        float ang = rotation + float(k) * 2.39996323;
        vec2 o = vec2(cos(ang), sin(ang)) * t * radius;
        ivec2 q = p + ivec2(round(o));
        if (q == p || any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, GRID))) continue;
        vec4 gq = Fetch(t_in2, q);
        if (gq.x <= 0.0) continue;
        vec3 pq = PointAt((vec2(q) + 0.5) * pc.size.zw, gq.x);
        float plane = abs(dot(pq - point, normal)) / planeTolerance;
        float w = exp(-2.0 * t * t) * max(0.0, 1.0 - plane)
                * pow(max(dot(OctDecodeN(gq.zw), normal), 0.0), 64.0)
                * exp(-abs(gq.y - r) / (0.05 + 0.2 * r))
                * exp(-abs(Fetch(t_in0, q).x - hit) / (0.3 * hit + 0.1 * z + 5.0));
        vec4 v = Fetch(t_in1, q);
        if (!Finite(v) || w <= 0.0) continue;
        sum += v * w; weights += w;
    }
    o_blurred = sum / weights;
}
