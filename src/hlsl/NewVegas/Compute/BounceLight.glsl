// Shared by the BounceLight Vulkan passes (needs NVR DXVK): the constants and helpers of Effects/BounceLight.fx.hlsl and its
// includes (Depth.hlsl, Blur.hlsl, Helpers.hlsl), line for line. effects/BounceLight.cpp ExtPasses. The neon spill is retired
// (BounceLight.cpp sets NVR_BounceSpill to (0, 1, ...) and NVR_SpillDebug to 0 every frame), so its branches, never taken, are
// left out: GatherPlus's `* NVR_BounceSpill.y` is that 1, Blur's slope is 0 and Combine's debug view and light-pixel test are off.
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

// The constants as the D3D9 path sets them this frame, uploaded once per frame (too big for push data). Matrices: the D3DX matrix
// as stored (SetMatrix keeps the logical matrix), so cb.m[i][j] is the HLSL's m[i][j] and mul(v, M) is cb.m * v.
layout(set = 2, binding = 0, std140) uniform Constants {
    mat4 projectionTransform;      // TESR_ProjectionTransform
    mat4 invProjectionTransform;   // TESR_InvProjectionTransform
    mat4 bounceReproject;          // NVR_BounceReproject: this frame's view space -> last frame's clip space
    vec4 bounceLightData;          // TESR_BounceLightData: x strength, y radius, z draw distance, w blur radius
    vec4 reciprocalResolution;     // TESR_ReciprocalResolution
    vec4 bounceLayout;             // NVR_BounceLayout: x low / full width, zw 1 / low-resolution size
    vec4 bounceTemporal;           // NVR_BounceTemporal: x history share, y extra bounces, z frame number
    vec4 bounceAmbient;            // NVR_BounceAmbient: rgb ambient light (linear), w off-screen light
    vec4 cameraData;               // TESR_CameraData: x near, y far
    vec4 renderScale;              // RenderRect scale (xy = scale, zw = 1 / scale)
} cb;

// Per pass (32 bytes).
layout(push_constant) uniform Push {
    uint pointSampler;             // 0: POINT, CLAMP (depth, normals, the bounce buffers)
    uint linearSampler;            // 4: LINEAR, CLAMP (TESR_SourceBuffer)
    uint wrapSampler;              // 8: POINT, WRAP (TESR_BlueNoiseSampler: MIN/MAG NONE)
    uint pad;                      // 12
    vec4 pass_;                    // 16: the pass's uniform arguments (see each .frag)
} pc;

#define POINT_LOD(t, coord) textureLod(sampler2D(t, NVR_SAMPLER(pc.pointSampler)), (coord), 0.0)
#define LINEAR_LOD(t, coord) textureLod(sampler2D(t, NVR_SAMPLER(pc.linearSampler)), (coord), 0.0)
#define WRAP_LOD(t, coord) textureLod(sampler2D(t, NVR_SAMPLER(pc.wrapSampler)), (coord), 0.0)

// Depth.hlsl
#define nearZ (cb.cameraData.x)
#define farZ (cb.cameraData.y)

float projectedDepthFromLinear(float linear01) {
    float viewZ = max(linear01 * farZ, 1.0e-6);
    return nearZ * (farZ - viewZ) / (viewZ * (farZ - nearZ));
}

vec3 projectPosition(vec3 position) {
    vec4 projection = cb.projectionTransform * vec4(position, 1.0);
    projection.xyz /= projection.w;

    projection.x = projection.x * 0.5 + 0.5;
    projection.y = 0.5 - 0.5 * projection.y;

    return projection.xyz;
}

// BounceLight.fx.hlsl's statics
#define Strength (cb.bounceLightData.x)
#define Radius (max(cb.bounceLightData.y, 1.0))
#define DrawDistance (cb.bounceLightData.z)
#define BlurRadius (cb.bounceLightData.w)
const float NearFade = 40.0;
const float MaxScreenRadius = 0.06;

// HLSL smoothstep (defined for edge0 >= edge1 too) and step(y, x) = x >= y
float smoothstepHlsl(float a, float b, float x) {
    float t = saturate((x - a) / (b - a));
    return t * t * (3.0 - 2.0 * t);
}
float stepHlsl(float y, float x) { return x >= y ? 1.0 : 0.0; }

// View-space position from a screen position and CombineDepth's linear depth (view z / far z).
vec3 PositionFromLinear(vec2 uv, float linear01) {
    float z = projectedDepthFromLinear(linear01);
    vec4 viewSpace = cb.invProjectionTransform * vec4(uv.x * 2.0 - 1.0, (1.0 - uv.y) * 2.0 - 1.0, z, 1.0);
    return viewSpace.xyz / viewSpace.w;
}

// Two values in [0, 1] as 12-bit integers in one 32-bit float.
float Pack2(vec2 v) {
    v = floor(clamp(v, 0.0, 1.0) * 4095.0 + 0.5);
    return v.x * 4096.0 + v.y;
}

vec2 Unpack2(float p) {
    float high = floor(p / 4096.0);
    return vec2(high, p - high * 4096.0) / 4095.0;
}

vec2 OctEncode(vec3 n) {
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    vec2 s = vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    vec2 e = n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * s;
    return e * 0.5 + 0.5;
}

vec3 OctDecode(vec2 e) {
    e = e * 2.0 - 1.0;
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    float t = saturate(-n.z);
    n.xy += vec2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);
    return normalize(n);
}

// Where a point (this frame's view space) was last frame: xy = screen texture coordinate, z = its view depth then.
vec3 LastFrame(vec3 position) {
    vec4 clip = cb.bounceReproject * vec4(position, 1.0);
    float w = max(clip.w, 0.001);
    return vec3(clip.x / w * 0.5 + 0.5, 0.5 - clip.y / w * 0.5, clip.w);
}

float DepthTolerance(vec3 position, vec3 normal) {
    float facing = max(abs(dot(normal, normalize(position))), 0.05);
    float texel = 2.0 * cb.bounceLayout.z / cb.projectionTransform[0][0];
    return position.z * (0.03 + 2.0 * texel * sqrt(1.0 - facing * facing) / facing) + 4.0;
}

// Clamp rect UVs to the active rendered region so edge taps never read unrendered garbage.
vec2 ClampFullUV(vec2 ruv) {
    vec2 px = cb.reciprocalResolution.xy;
    return clamp(ruv, 0.5 * px, max(cb.renderScale.xy - 0.5 * px, 0.5 * px));
}

vec2 ClampLowUV(vec2 ruv) {
    vec2 px = cb.bounceLayout.zw;
    return clamp(ruv, 0.5 * px, max(cb.renderScale.xy - 0.5 * px, 0.5 * px));
}

// Last frame's result at a point in NVR_BounceHistory (point sampled): how much of it matched (0: nothing usable).
float HistoryAt(texture2D historyTex, vec3 last, float tolerance, out vec3 history) {
    vec2 lastUV = last.xy * cb.renderScale.xy;
    vec2 position = lastUV / cb.bounceLayout.zw - 0.5;
    vec2 base = floor(position);
    vec2 f = position - base;
    vec4 t00 = POINT_LOD(historyTex, ClampLowUV((base + 0.5) * cb.bounceLayout.zw));
    vec4 t10 = POINT_LOD(historyTex, ClampLowUV((base + vec2(1.5, 0.5)) * cb.bounceLayout.zw));
    vec4 t01 = POINT_LOD(historyTex, ClampLowUV((base + vec2(0.5, 1.5)) * cb.bounceLayout.zw));
    vec4 t11 = POINT_LOD(historyTex, ClampLowUV((base + 1.5) * cb.bounceLayout.zw));
    vec4 w = vec4((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y);
    w *= vec4(lessThan(abs(vec4(t00.a, t10.a, t01.a, t11.a) - last.z), vec4(tolerance)));
    float sum = dot(w, vec4(1.0));
    history = (t00.rgb * w.x + t10.rgb * w.y + t01.rgb * w.z + t11.rgb * w.w) / max(sum, 1.0e-4);
    bool inside = all(greaterThan(last.xy, vec2(0.0))) && all(lessThan(last.xy, vec2(1.0))) && last.z > NearFade;
    return inside ? sum : 0.0;
}
