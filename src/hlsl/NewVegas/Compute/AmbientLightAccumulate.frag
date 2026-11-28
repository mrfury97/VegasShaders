// AmbientLight pass Accumulate as a Vulkan pass (needs NVR DXVK): Effects/AmbientLight.fx.hlsl Accumulate, line for line
// (AmbientLight.glsl). Two targets: this frame's history (bounce) and the occlusion's. All POINT. effects/AmbientLight.cpp ExtDraw.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "AmbientLight.glsl"

layout(set = 1, binding = 0) uniform texture2D t_buffer;            // NVR_ALBuffer (the blurred estimate)
layout(set = 1, binding = 1) uniform texture2D t_prep;              // NVR_ALPrep
layout(set = 1, binding = 2) uniform texture2D t_occlusionHistory;  // NVR_ALOcclusionHistory (last frame's)
layout(set = 1, binding = 3) uniform texture2D t_objmotion;         // UNOFFICIAL 2026-10-29 TESR_ObjectMotionBuffer (read with variant bit 2)

layout(location = 0) out vec4 o_bounce;
layout(location = 1) out vec4 o_occlusion;

void main() {
    vec2 uv = frameRectUV();
    vec4 current = pointLod(t_buffer, uv);
    float viewZ = pointLod(t_prep, uv).x;
    o_bounce = vec4(current.rgb, viewZ);
    o_occlusion = vec4(current.a, viewZ, 0.0, 1.0);
    if (!(cb.alTemporal.x > 0.0) || viewZ < NearFade) return;
    vec3 origin = PositionFromViewZ(uv, viewZ);
    // UNOFFICIAL 2026-10-29 ObjectMotion (the HLSL's mul(motion, (float3x3)TESR_ViewTransform)): variant bit 2 = this frame's buffer is there
    if ((pc.variant & 4u) != 0u) origin -= (cb.viewTransform * vec4(pointLod(t_objmotion, uv).xyz, 0.0)).xyz;
    vec4 clip = cb.reproject * vec4(origin, 1.0);   // mul(float4(origin, 1), NVR_ALReproject)
    float w = max(clip.w, 0.001);
    vec3 last = vec3(clip.x / w * 0.5 + 0.5, 0.5 - clip.y / w * 0.5, clip.w);
    vec2 lastUV = screenToRect(last.xy + vec2(cb.alTemporal.w, cb.alEdges.w));   // (UNOFFICIAL 2026-11-01 TemporalJitterFix: the .fx's)
    vec2 position = lastUV / cb.alLayout.zw - 0.5;
    vec2 base = floor(position);
    vec2 f = position - base;
    vec2 t00 = pointLod(t_occlusionHistory, clampToRect((base + 0.5) * cb.alLayout.zw, cb.alLayout.zw)).rg;
    vec2 t10 = pointLod(t_occlusionHistory, clampToRect((base + vec2(1.5, 0.5)) * cb.alLayout.zw, cb.alLayout.zw)).rg;
    vec2 t01 = pointLod(t_occlusionHistory, clampToRect((base + vec2(0.5, 1.5)) * cb.alLayout.zw, cb.alLayout.zw)).rg;
    vec2 t11 = pointLod(t_occlusionHistory, clampToRect((base + 1.5) * cb.alLayout.zw, cb.alLayout.zw)).rg;
    vec4 weight = vec4((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y);
    weight *= vec4(lessThan(abs(vec4(t00.g, t10.g, t01.g, t11.g) - last.z), vec4(max(last.z * 0.05, 4.0))));
    float found = dot(weight, vec4(1.0));
    float occlusionHistory = dot(vec4(t00.r, t10.r, t01.r, t11.r), weight) / max(found, 1.0e-4);
    float low = current.a, high = current.a;
    for (int i = 0; i < 4; i++) {
        vec2 offset = (i < 2 ? vec2(float(i * 2 - 1), 0.0) : vec2(0.0, float(i * 2 - 5))) * cb.alLayout.zw;
        float n = pointLod(t_buffer, clampToRect(uv + offset, cb.alLayout.zw)).a;
        low = min(low, n);
        high = max(high, n);
    }
    float margin = (high - low) * 0.25;
    occlusionHistory = clamp(occlusionHistory, low - margin, high + margin);
    float inside = (all(greaterThan(last.xy, vec2(0.0))) && all(lessThan(last.xy, vec2(1.0))) && last.z > NearFade) ? 1.0 : 0.0;
    float moved = length((lastUV - uv) / cb.alLayout.zw);
    float keep = cb.alTemporal.x / (1.0 + moved / cb.alTemporal.y) * saturate(found * 2.0) * inside;
    o_occlusion = vec4(mix(current.a, occlusionHistory, keep), viewZ, 0.0, 1.0);
}
