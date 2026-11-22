// UNOFFICIAL 2026-10-31 RT sun Penumbra ([Shaders.ShadowsExteriors.RT] Penumbra, core\RtSunShadows.cpp; docs\next\RT-SUN-SOFT-AND-BUILD.md):
// contact-hardening filter for the soft RT sun shadows. In: the jittered sun-disc visibility after the shared denoiser's temporal pass
// (no spatial passes) and each traced pixel's blocker distance. A blocker search averages the blocker distances around the pixel; the
// penumbra's radius on screen is that distance x tan(the sun's half-angle) / the world size of a pixel at this depth (the idea of
// PCSS, Fernando 2005, with the real traced distances as in NVIDIA SIGMA / AMD FidelityFX Shadow Denoiser); the visibility is then
// averaged over a disc of that radius, kept on the same surface by depth and normal. Contact (blocker a few units away): no blur;
// far blockers: wide soft edges. Traced size (half with HalfResolution).
// Compile: glslangValidator -V --target-env vulkan1.1 -o RtSunPenumbra.spv RtSunPenumbra.frag
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_visibility;   // R16F: the temporally accumulated visibility (traced size)
layout(set = 1, binding = 1) uniform texture2D t_blocker;      // R16F: the blocker distance (units), 0 = free (traced size)
layout(set = 1, binding = 2) uniform texture2D t_depth;        // TESR_DepthBuffer: view z / farZ (full size)
layout(set = 1, binding = 3) uniform texture2D t_normals;      // TESR_NormalsBuffer: view-space normal * 0.5 + 0.5 (full size)

layout(push_constant) uniform Push {
    vec4 params;   // x tan(the sun's half-angle), y farZ, z the largest radius (traced pixels), w debug (1: radius / largest as grey, 2: a flat 0.25)
    vec4 size;     // x 1 / width, y 1 / height (the textures), z the traced frame's height (traced pixels), w projection[1][1]
    vec4 extra;    // x the smallest radius that filters (pixels); UNOFFICIAL 2026-11-04 RenderScale < 1: yz the rect's last texel centre
                   // (uv), w 1 = a rect frame (the trace is the textures' top-left part: taps clamped to it, texels past it write 0)
    uint samplerIndex;
    uint pad[3];
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

const int kTaps = 16;

// (2026-11-04) a tap's uv kept inside the traced rect in rect frames (past it: older frames' values); native frames unchanged
vec2 inRect(vec2 t) { return pc.extra.w > 0.5 ? min(t, pc.extra.yz) : t; }

// A Vogel (golden angle) disc: tap i of n at radius sqrt((i + 0.5) / n), turned by `turn`
vec2 vogel(int i, float turn) {
    const float r = sqrt((float(i) + 0.5) / float(kTaps));
    const float a = float(i) * 2.39996323 + turn;
    return vec2(cos(a), sin(a)) * r;
}

void main() {
    if (pc.params.w > 1.5) { o_color = vec4(0.25, 0.0, 0.0, 0.0); return; }   // DebugView 12: the pass' own output, flat
    float center = textureLod(sampler2D(t_visibility, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0).r;
    // 2026-10-31 anti-firefly (ReBLUR's): a lone pixel brighter or darker than all 8 neighbours is clamped into their range (the stray
    // specks in sunlit ground: a rare jittered ray that met something, kept by the history; a real shadow is never one pixel alone)
    float lo = 1.0e9, hi = -1.0e9;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++) {
            if (x == 0 && y == 0) continue;
            const float v = textureLod(sampler2D(t_visibility, NVR_SAMPLER(pc.samplerIndex)), inRect(uv + vec2(x, y) * pc.size.xy), 0.0).r;
            lo = min(lo, v); hi = max(hi, v);
        }
    center = clamp(center, lo, hi);
    const float d01 = textureLod(sampler2D(t_depth, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0).r;
    const float z = d01 * pc.params.y;
    const float zSlope = max(abs(dFdx(z)), abs(dFdy(z)));   // before any branch (derivatives need every pixel of the quad)
    if (pc.extra.w > 0.5 && any(greaterThan(uv, pc.extra.yz + 0.5 * pc.size.xy))) { o_color = vec4(0.0); return; }   // (2026-11-04) past the rect
    if (!(d01 > 0.0 && d01 < 0.9999)) { o_color = vec4(center, 0.0, 0.0, 0.0); return; }
    const vec3 n = normalize(textureLod(sampler2D(t_normals, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0).xyz * 2.0 - 1.0);
    // the world size of one traced pixel at this depth (size.z: the traced frame's height, the rect's in a rect frame)
    const float pixelWorld = 2.0 * z / (pc.size.w * pc.size.z);
    const vec2 pixel = gl_FragCoord.xy;
    // interleaved gradient noise: a fixed turn per pixel (no frame term: the result is not averaged over frames after this)
    const float turn = 6.2831853 * fract(52.9829189 * fract(0.06711056 * pixel.x + 0.00583715 * pixel.y));

    // 1. blocker search: the average distance of the blocked samples within the largest penumbra
    float blockers = 0.0, found = 0.0;
    const float own = abs(textureLod(sampler2D(t_blocker, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0).r);
    if (own > 0.0) { blockers += own; found += 1.0; }
    for (int i = 0; i < kTaps; i++) {
        const vec2 tapUv = inRect(uv + vogel(i, turn) * pc.params.z * pc.size.xy);
        const float b = abs(textureLod(sampler2D(t_blocker, NVR_SAMPLER(pc.samplerIndex)), tapUv, 0.0).r);
        if (b > 0.0) { blockers += b; found += 1.0; }
    }
    if (found <= 0.0) { o_color = vec4(pc.params.w > 0.5 ? 0.0 : center, 0.0, 0.0, 0.0); return; }   // nothing blocks near here: lit
    const float blocker = blockers / found;
    const float radius = min(blocker * pc.params.x / pixelWorld, pc.params.z);
    if (pc.params.w > 0.5) { o_color = vec4(radius / pc.params.z, 0.0, 0.0, 0.0); return; }
    if (radius < pc.extra.x) { o_color = vec4(center, 0.0, 0.0, 0.0); return; }   // contact: as hard as traced

    // 2. the visibility over a disc of that radius, on the same surface (depth relative to its slope, view normal)
    float sum = center, weights = 1.0;
    for (int i = 0; i < kTaps; i++) {
        const vec2 o = vogel(i, turn + 1.3) * radius;
        const vec2 tapUv = inRect(uv + o * pc.size.xy);
        const float td = textureLod(sampler2D(t_depth, NVR_SAMPLER(pc.samplerIndex)), tapUv, 0.0).r;
        if (!(td > 0.0 && td < 0.9999)) continue;
        const float tz = td * pc.params.y;
        const float tolerance = 1.5 * zSlope * length(o) + 0.01 * z;
        if (abs(tz - z) > 3.0 * tolerance) continue;
        const vec3 tn = normalize(textureLod(sampler2D(t_normals, NVR_SAMPLER(pc.samplerIndex)), tapUv, 0.0).xyz * 2.0 - 1.0);
        float w = exp(-abs(tz - z) / tolerance) * pow(max(dot(n, tn), 0.0), 16.0);
        w *= exp(-2.0 * dot(o, o) / (radius * radius));
        sum += textureLod(sampler2D(t_visibility, NVR_SAMPLER(pc.samplerIndex)), tapUv, 0.0).r * w;
        weights += w;
    }
    o_color = vec4(sum / weights, 0.0, 0.0, 0.0);
}
