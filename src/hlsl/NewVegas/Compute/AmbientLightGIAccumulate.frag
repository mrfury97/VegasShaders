// UNOFFICIAL 2026-10-27 (NVR DXVK, deep/fork2-ssgi) [Shaders.AmbientLight.Main] ComputeGI: AmbientLightAccumulate.frag (the occlusion's
// temporal accumulation, line for line) plus the same for the bounce light AmbientLightGI.frag wrote into NVR_AmbientLight0's rgb: last
// frame's light found by the same reprojection and depth test (the occlusion history's view depth), clamped to this frame's neighbours
// (as the occlusion: 4 neighbours, a quarter of their range as margin, per channel), kept by the same share. Two targets as Accumulate:
// this frame's history (rgb bounce light, a view depth: NVR_AmbientLightHistory) and the occlusion's. All POINT. The common 32-byte
// push block (no NVR_GI: nothing of ComputeGI's own is read here).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "AmbientLight.glsl"

layout(set = 1, binding = 0) uniform texture2D t_buffer;            // NVR_ALBuffer (the blurred GI estimate)
layout(set = 1, binding = 1) uniform texture2D t_prep;              // NVR_ALPrep
layout(set = 1, binding = 2) uniform texture2D t_occlusionHistory;  // NVR_ALOcclusionHistory (last frame's)
layout(set = 1, binding = 3) uniform texture2D t_bounceHistory;     // NVR_AmbientLightHistory (last frame's: rgb bounce light, a view depth)

layout(location = 0) out vec4 o_bounce;
layout(location = 1) out vec4 o_occlusion;

void main() {
    vec2 uv = frameUV();
    vec4 current = pointLod(t_buffer, uv);
    float viewZ = pointLod(t_prep, uv).x;
    o_bounce = vec4(current.rgb, viewZ);
    o_occlusion = vec4(current.a, viewZ, 0.0, 1.0);
    if (!(cb.alTemporal.x > 0.0) || viewZ < NearFade) return;
    vec3 origin = PositionFromViewZ(uv, viewZ);
    vec4 clip = cb.reproject * vec4(origin, 1.0);   // mul(float4(origin, 1), NVR_ALReproject)
    float w = max(clip.w, 0.001);
    vec3 last = vec3(clip.x / w * 0.5 + 0.5, 0.5 - clip.y / w * 0.5, clip.w);
    vec2 position = last.xy / cb.alLayout.zw - 0.5;
    vec2 base = floor(position);
    vec2 f = position - base;
    vec2 uv00 = (base + 0.5) * cb.alLayout.zw, uv10 = (base + vec2(1.5, 0.5)) * cb.alLayout.zw;
    vec2 uv01 = (base + vec2(0.5, 1.5)) * cb.alLayout.zw, uv11 = (base + 1.5) * cb.alLayout.zw;
    vec2 t00 = pointLod(t_occlusionHistory, uv00).rg;
    vec2 t10 = pointLod(t_occlusionHistory, uv10).rg;
    vec2 t01 = pointLod(t_occlusionHistory, uv01).rg;
    vec2 t11 = pointLod(t_occlusionHistory, uv11).rg;
    vec4 weight = vec4((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y);
    weight *= vec4(lessThan(abs(vec4(t00.g, t10.g, t01.g, t11.g) - last.z), vec4(max(last.z * 0.05, 4.0))));
    float found = dot(weight, vec4(1.0));
    float occlusionHistory = dot(vec4(t00.r, t10.r, t01.r, t11.r), weight) / max(found, 1.0e-4);
    vec3 bounceHistory = (pointLod(t_bounceHistory, uv00).rgb * weight.x + pointLod(t_bounceHistory, uv10).rgb * weight.y +
        pointLod(t_bounceHistory, uv01).rgb * weight.z + pointLod(t_bounceHistory, uv11).rgb * weight.w) / max(found, 1.0e-4);
    float low = current.a, high = current.a;
    vec3 lowC = current.rgb, highC = current.rgb;
    for (int i = 0; i < 4; i++) {
        vec2 offset = (i < 2 ? vec2(float(i * 2 - 1), 0.0) : vec2(0.0, float(i * 2 - 5))) * cb.alLayout.zw;
        vec4 n = pointLod(t_buffer, uv + offset);
        low = min(low, n.a);
        high = max(high, n.a);
        lowC = min(lowC, n.rgb);
        highC = max(highC, n.rgb);
    }
    float margin = (high - low) * 0.25;
    occlusionHistory = clamp(occlusionHistory, low - margin, high + margin);
    vec3 marginC = (highC - lowC) * 0.25;
    bounceHistory = clamp(bounceHistory, lowC - marginC, highC + marginC);
    float inside = (all(greaterThan(last.xy, vec2(0.0))) && all(lessThan(last.xy, vec2(1.0))) && last.z > NearFade) ? 1.0 : 0.0;
    float moved = length((last.xy - uv) / cb.alLayout.zw);
    float keep = cb.alTemporal.x / (1.0 + moved / cb.alTemporal.y) * saturate(found * 2.0) * inside;
    o_occlusion = vec4(mix(current.a, occlusionHistory, keep), viewZ, 0.0, 1.0);
    // UNOFFICIAL 2026-11-11 (sungi) GISingleHistory (STUDY F7): variant bit 0 = the GI came through the RT denoiser (own history): keep 0
    o_bounce = vec4(mix(current.rgb, bounceHistory, (pc.variant & 1u) != 0u ? 0.0 : keep), viewZ);
}
