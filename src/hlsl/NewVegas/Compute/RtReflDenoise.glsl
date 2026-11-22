// UNOFFICIAL 2026-11-07 RT reflection denoiser ([Shaders.Reflections.RT] Denoiser; docs/next/RT-REFLECTION-DENOISER.md, core/RtReflDenoise.h).
// What the four RtReflDenoiseOurs*.frag / RtReflDenoiseAverage.frag passes share: bindings, push block, sub-rect surface & reprojection helpers.
// (AMD MIT notice kept for RtReflDenoiseAverage.frag's luminance weight: Copyright (c) 2021 Advanced Micro Devices, Inc., MIT License.)

#include "Common.glsl"
#include "RtMirrorJitter.glsl"

layout(set = 1, binding = 0) uniform texture2D t_signal;   // Resolve's output: rgb the reflection x confidence, a the confidence (half size)
layout(set = 1, binding = 1) uniform texture2D t_ray;      // RT reflections' Ray image: x the surface's view z (0: nothing reflects), y roughness (+2/+4/+6 flags)
layout(set = 1, binding = 2) uniform texture2D t_hitDist;  // Resolve's Spread image: y the hit distance (world units; 0 unknown, Distance = nothing hit)
layout(set = 1, binding = 3) uniform texture2D t_normals;  // TESR_NormalsBuffer (full size, view-space normal x 0.5 + 0.5)
layout(set = 1, binding = 4) uniform texture2D t_motion;   // TESR_ObjectMotionBuffer (full size, rgb the surface's world move) or a stand-in
layout(set = 1, binding = 5) uniform texture2D t_in0;      // per stage (RtReflDenoise.h): see each pass
layout(set = 1, binding = 6) uniform texture2D t_in1;
layout(set = 1, binding = 7) uniform texture2D t_in2;
layout(set = 1, binding = 8) uniform texture2D t_in3;
layout(set = 1, binding = 9) uniform texture2D t_avg;      // the 8 x 8 average (1/8 size)
layout(set = 1, binding = 10) uniform texture2D t_vmMotion; // TESR_ViewModelMotionBuffer (full size) or a stand-in

layout(push_constant) uniform Push {
    mat4 reproject;     // camera-relative world now -> last frame's clip space (shifted by -lastJitter on CPU)
    vec4 forward;       // xyz the camera's forward (world axes), w farZ
    vec4 right;         // xyz right / P00, w 1: t_motion holds ObjectMotion
    vec4 up;            // xyz up / P11, w mirrorJitterRoughness
    vec4 size;          // the full grid: fullW, fullH, 1 / fullW, 1 / fullH
    vec4 fullUv;        // xy: added to a grid pixel's rectUv to read full-size normals / motion at its pixel; zw: cur DepthJitterUV (screen [0,1] UV)
    vec4 params;        // x max samples, y clip weight, z mirror roughness, w 1: last frame's history is usable
    vec4 params2;       // x core, y debug view, z ours: blur radius (grid px at roughness 1), w ours: hit-distance share for contact
    vec4 params3;       // x ours: anti-lag sigma, y ours: max history at the mirror limit (frames), z ours: firefly clamp, w the frame (0-63)
    vec4 rect;          // xy curScale, zw lastScale (RenderRect sub-rect scales; 1, 1 when native)
    vec4 activeSize;    // xy activeW, activeH (grid pixels drawn), z MirrorJitter code (0 off), w weaponFovScale
    uint linearIndex;   // LINEAR / CLAMP
    uint pointIndex;    // POINT / CLAMP
    uint vmMotionValid; // 1 = t_vmMotion holds ViewModelMotionBuffer
    uint pad1;
} pc;

#define LINEAR_S NVR_SAMPLER(pc.linearIndex)
#define POINT_S NVR_SAMPLER(pc.pointIndex)
#define GRID ivec2(pc.size.xy)
#define ACTIVE_GRID ivec2(pc.activeSize.xy)
#define MirrorRoughness pc.params.z
#define HistoryValid (pc.params.w > 0.5)

vec4 Fetch(texture2D t, ivec2 p) { return texelFetch(sampler2D(t, POINT_S), clamp(p, ivec2(0), max(ACTIVE_GRID - 1, ivec2(0))), 0); }

// The surface at a grid pixel: view z (0: nothing reflects), roughness without RayGen's flags, weapon (RayGen's +6).
struct Surface { float z; float roughness; bool weapon; vec3 n; };
float StripRoughness(float y, out bool weapon) {
    weapon = y > 5.5;
    return y - (weapon ? 6.0 : y > 3.5 ? 4.0 : y > 1.5 ? 2.0 : 0.0);
}
vec3 ViewToWorldN(vec3 v) { return normalize(v.x * normalize(pc.right.xyz) + v.y * normalize(pc.up.xyz) + v.z * normalize(pc.forward.xyz)); }
Surface SurfaceAt(ivec2 p) {
    Surface s;
    ivec2 cp = clamp(p, ivec2(0), max(ACTIVE_GRID - 1, ivec2(0)));
    vec2 r = Fetch(t_ray, cp).xy;
    s.z = r.x;
    s.roughness = StripRoughness(r.y, s.weapon);
    vec2 ruv = clamp((vec2(cp) + 0.5) * pc.size.zw + pc.fullUv.xy, 0.5 * pc.size.zw, pc.rect.xy - 0.5 * pc.size.zw);
    s.n = ViewToWorldN(textureLod(sampler2D(t_normals, POINT_S), ruv, 0.0).xyz * 2.0 - 1.0);
    return s;
}
// The camera-relative world point through grid rectUv at view z: rectUv -> normalized [0, 1] screen UV (+ TAA DepthJitterUV) -> world ray * z
vec3 PointAt(vec2 rectUv, float z) {
    vec2 s = (rectUv + pc.fullUv.xy) / max(pc.rect.xy, vec2(1.0e-4)) + pc.fullUv.zw;
    return (pc.forward.xyz + pc.right.xyz * (2.0 * s.x - 1.0) - pc.up.xyz * (2.0 * s.y - 1.0)) * z;
}
vec3 PointAtSurface(vec2 rectUv, float z, bool weapon) {
    vec2 s = (rectUv + pc.fullUv.xy) / max(pc.rect.xy, vec2(1.0e-4)) + pc.fullUv.zw;
    float invFov = (weapon && pc.activeSize.w > 1.0e-3) ? (1.0 / pc.activeSize.w) : 1.0;
    return (pc.forward.xyz + (pc.right.xyz * (2.0 * s.x - 1.0) - pc.up.xyz * (2.0 * s.y - 1.0)) * invFov) * z;
}
// Camera-only reprojection into last frame's sub-rect grid UV; checks frustum bounds against normalized [0, 1] sPrev
vec3 ToLast(vec3 p, out bool inFrustum) {
    vec4 clip = pc.reproject * vec4(p, 1.0);
    if (clip.w <= 1.0e-3) { inFrustum = false; return vec3(-1.0, -1.0, clip.w); }
    vec2 sPrev = vec2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);
    inFrustum = all(greaterThanEqual(sPrev, vec2(0.0))) && all(lessThanEqual(sPrev, vec2(1.0)));
    return vec3(sPrev * pc.rect.zw - pc.fullUv.xy, clip.w);
}
vec3 ObjectMotionAt(vec2 rectUv) {
    if (pc.right.w <= 0.5) return vec3(0.0);
    vec2 ruv = clamp(rectUv + pc.fullUv.xy, 0.5 * pc.size.zw, pc.rect.xy - 0.5 * pc.size.zw);
    return textureLod(sampler2D(t_motion, POINT_S), ruv, 0.0).xyz;
}
// Surface-only reprojection with ObjectMotion world delta (never used for virtual hit points)
vec3 ToLastWithMotion(vec3 pSurf, vec2 rectUv, out bool inFrustum) {
    return ToLast(pSurf - ObjectMotionAt(rectUv), inFrustum);
}

vec2 OctEncodeN(vec3 n) {
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    vec2 e = n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return e;
}
vec3 OctDecodeN(vec2 e) {
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}

bool Finite(vec4 v) { return !any(isnan(v)) && !any(isinf(v)); }

#define FFX_DNSR_REFLECTIONS_AVG_RADIANCE_LUMINANCE_WEIGHT 0.3
float FFX_Luminance(vec3 color) { return max(dot(color, vec3(0.299, 0.587, 0.114)), 0.001); }
bool IsMirror(float roughness) { return roughness < MirrorRoughness; }
