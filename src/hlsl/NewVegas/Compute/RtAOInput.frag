// UNOFFICIAL 2026-10-28 (NVR DXVK fork, hybrid RT item 2; docs/next/FORK-RT-AO.md) [Shaders.AmbientOcclusion.RT]: what the RT helper's
// trace pass (RtAOTrace.comp) starts its rays from, per half-resolution texel: x the view distance (0 past the AO's far end: no rays),
// y the world normal (camera-relative world, OctEncode + Pack2 as AmbientLight's prep). Into NVR_RTAOInput (G32R32F), shared with the
// helper by core/RtAmbient.h. uv is the target's texel centre. effects/AmbientOcclusion.cpp RtRaw.
#version 450
#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_samplerless_texture_functions : enable
#include "AmbientOcclusion.glsl"

layout(set = 1, binding = 0) uniform texture2D t_depth;      // TESR_DepthBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 1) uniform texture2D t_normals;    // TESR_NormalsBuffer (NONE = POINT, CLAMP)

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

// (2026-11-03) the depth read at the texel (uv), the point rebuilt at its screen point s (uv / the RenderRect scale; = uv when off)
vec3 reconstructPosition(vec2 uv, vec2 s) {
    float z = projectedDepthFromLinear(LINEAR_LOD(t_depth, uv).x);
    vec4 viewSpace = cb.invProjectionTransform * vec4(s.x * 2.0 - 1.0, (1.0 - s.y) * 2.0 - 1.0, z, 1.0);
    return viewSpace.xyz / viewSpace.w;
}

// AmbientLight.glsl's
float Pack2(vec2 v) { v = floor(clamp(v, 0.0, 1.0) * 4095.0 + 0.5); return v.x * 4096.0 + v.y; }
vec2 OctEncode(vec3 n) {
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    vec2 s = vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    vec2 e = n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * s;
    return e * 0.5 + 0.5;
}

void main() {
    // UNOFFICIAL 2026-11-03 RenderScale < 1: the frame is the top-left cb.rtRect.xy part (the trace covers only that); past it no rays
    // (x 0: the RT sky pass's neighbour taps there never match a surface's distance)
    vec2 s = uv / cb.rtRect.xy;
    if (s.x > 1.0 || s.y > 1.0) { o_color = vec4(0.0); return; }
    vec3 P = reconstructPosition(uv, s);
    if (!(P.z < endFade) || P.z <= 0.0) { o_color = vec4(0.0); return; }
    // UNOFFICIAL 2026-11-09 NormalGuard (cb.skyLayout.w 1, [Shaders.AmbientOcclusion.RT]; docs/next/RT-PT-REVIEW-2026-11-09.md 1.2 and 11).
    // PROVEN on a tester's RTX 5090 (NVIDIA 621.1, DXVK "Binding model: Descriptor heap"): the point-sampled read below returned a blank
    // texel there on every pixel, while the buffer itself held real normals. A blank texel decodes to (-1, -1, -1): every AO / GI ray
    // started from one bogus normal, and the face-forward flip below turned it over on one side of the view plane X + Y + Z = 0: a
    // screen-fixed 45-degree triangle in the bounce light, plus trails (the reuse's normal test passed everywhere).
    // On: the normals are fetched by texel, with no sampler (the same texel a point sampler picks); and a texel that is still not a
    // unit normal (an undrawn or cleared buffer) takes the surface's own normal, rebuilt from the depth here and at its neighbours.
    // Off: the point-sampled read as before, whatever it returns.
    vec3 N;
    if (cb.skyLayout.w > 0.5) {
        const ivec2 size = textureSize(t_normals, 0);
        N = texelFetch(t_normals, clamp(ivec2(uv * vec2(size)), ivec2(0), size - 1), 0).xyz * 2.0 - 1.0;
        const float nn = dot(N, N);
        if (!(nn > 0.5625 && nn < 1.5625)) {
            const vec2 px = cb.aoLayout.zw;   // one target texel in uv
            const vec3 Px = reconstructPosition(uv + vec2(px.x, 0.0), (uv + vec2(px.x, 0.0)) / cb.rtRect.xy);
            const vec3 Py = reconstructPosition(uv + vec2(0.0, px.y), (uv + vec2(0.0, px.y)) / cb.rtRect.xy);
            const vec3 Qx = reconstructPosition(uv - vec2(px.x, 0.0), (uv - vec2(px.x, 0.0)) / cb.rtRect.xy);
            const vec3 Qy = reconstructPosition(uv - vec2(0.0, px.y), (uv - vec2(0.0, px.y)) / cb.rtRect.xy);
            // the nearer neighbour on each axis (the one on this surface at a depth edge)
            const vec3 dx = abs(Px.z - P.z) < abs(Qx.z - P.z) ? Px - P : P - Qx;
            const vec3 dy = abs(Py.z - P.z) < abs(Qy.z - P.z) ? Py - P : P - Qy;
            N = normalize(cross(dy, dx) + vec3(0.0, 0.0, -1.0e-20));
        }
    } else N = POINT_LOD(t_normals, uv).xyz * 2.0 - 1.0;
    N = dot(N, -P) < 0.0 ? -N : N;
    // UNOFFICIAL 2026-11-13 (rtgi-review R4) InputSamePixel (cb.skyLayout.y 1, with NormalGuard): the depth above is a LINEAR read at this texel's centre,
    // the corner of 4 full-size pixels: their average. Along a silhouette that is a point in mid air between two surfaces, with the normal of one of
    // them (the texel fetched above): its rays trace nonsense. Now the point is on the surface the normal came from: that pixel's own depth, and
    // the texel centre's view ray met with the plane through it (the pixel's point and normal), so a flat surface gives exactly the centre's point
    // (no half-pixel depth error at grazing angles); a plane the ray barely meets, or a result over 3 % off the pixel's depth, keeps the pixel's depth.
    if (cb.skyLayout.y > 0.5) {
        const ivec2 dsize = textureSize(t_depth, 0);
        const ivec2 dpx = clamp(ivec2(uv * vec2(dsize)), ivec2(0), dsize - 1);
        const float zp = projectedDepthFromLinear(texelFetch(t_depth, dpx, 0).x);
        const vec2 sp = (vec2(dpx) + 0.5) / vec2(dsize) / cb.rtRect.xy;
        vec4 a = cb.invProjectionTransform * vec4(sp.x * 2.0 - 1.0, (1.0 - sp.y) * 2.0 - 1.0, zp, 1.0);
        const vec3 Pp = a.xyz / a.w;                      // the fetched pixel's point
        a = cb.invProjectionTransform * vec4(s.x * 2.0 - 1.0, (1.0 - s.y) * 2.0 - 1.0, zp, 1.0);
        const vec3 C = a.xyz / a.w;                       // the texel centre's ray at that depth
        if (!(Pp.z < endFade) || Pp.z <= 0.0) { o_color = vec4(0.0); return; }
        const float denom = dot(N, C);
        const float t = abs(denom) > 0.05 * length(C) ? dot(N, Pp) / denom : 1.0;
        P = C * (t > 0.97 && t < 1.03 ? t : 1.0);
        N = dot(N, -P) < 0.0 ? -N : N;
    }
    // UNOFFICIAL 2026-11-10 LegacyTilt (cb.skyLayout.x, 0-1, [Shaders.AmbientOcclusion.RT]; 0 = off): a tester preferred the much darker
    // AO / sky light / bounce the blank-normals bug above gave, without its wedge and trails. What the bug did over most of the screen: every
    // ray started from the view-space direction (-1, -1, -1) (left, down, toward the camera) instead of the surface's normal, so on open
    // ground most rays went into the ground or along it. This bends the real normal toward that direction by the amount set. No flip (the
    // wedge was the flip), and below 1 the normals still differ from surface to surface, so the reuse's normal test keeps working (at 1 it
    // passes everywhere again: the trails can come back).
    if (cb.skyLayout.x > 0.0) N = normalize(mix(N, vec3(-0.57735), clamp(cb.skyLayout.x, 0.0, 1.0)) + vec3(0.0, 0.0, -1.0e-6));
    vec3 n = normalize((cb.invViewTransform * vec4(N, 0.0)).xyz);
    o_color = vec4(P.z, Pack2(OctEncode(n)), 0.0, 0.0);
}
