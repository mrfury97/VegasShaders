// BounceLight pass Gather (first technique) and GatherPlus(samples, temporal, motionPixels) (every other technique) as a Vulkan pass
// (needs NVR DXVK): Effects/BounceLight.fx.hlsl line for line, into the low-resolution buffer 0 (Temporal: this frame's history
// buffer). uv is HalfVS's: the low-resolution texel centre.
// pc.pass_: x 1 = GatherPlus, y its samples (8 or 4), z 1 = temporal, w motionPixels.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "BounceLight.glsl"

layout(set = 1, binding = 0) uniform texture2D t_prep;       // NVR_BouncePrep (POINT, CLAMP)
layout(set = 1, binding = 1) uniform texture2D t_noise;      // TESR_BlueNoiseSampler (NONE: POINT, WRAP)
layout(set = 1, binding = 2) uniform texture2D t_history;    // NVR_BounceHistory: last frame's gather (POINT, CLAMP; temporal)

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

const int SAMPLES = 8;

vec4 Gather() {
    vec2 ruv = ClampLowUV(uv * cb.renderScale.xy);
    vec4 center = POINT_LOD(t_prep, ruv);
    vec3 origin = PositionFromLinear(uv, center.x);
    float fade = smoothstepHlsl(NearFade, NearFade * 2.0, origin.z) * (1.0 - smoothstepHlsl(DrawDistance * 0.7, DrawDistance, origin.z));
    if (fade <= 0.0) return vec4(0.0, 0.0, 0.0, origin.z);

    vec3 normal = OctDecode(Unpack2(center.y));
    vec3 noise = WRAP_LOD(t_noise, (uv * cb.renderScale.xy) / (256.0 * cb.bounceLayout.zw)).xyz;
    vec3 up = abs(normal.z) < 0.99 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(up, normal));
    vec3 bitangent = cross(normal, tangent);
    // A disc of r world units at depth z spans r * P11 / (2 z) of the screen width.
    float radius = min(Radius, MaxScreenRadius * 2.0 * origin.z / cb.projectionTransform[0][0]);

    vec3 indirect = vec3(0.0);
    for (int i = 0; i < SAMPLES; i++) {
        float angle = (float(i) + noise.x) * (6.2831853 / float(SAMPLES));
        float r = radius * (0.15 + 0.85 * fract(noise.y + float(i) * 0.618034));
        vec3 p = origin + (tangent * cos(angle) + bitangent * sin(angle)) * r + normal * (r * 0.25);
        vec2 sampleUV = projectPosition(p).xy;
        float onScreen = stepHlsl(0.0, sampleUV.x) * stepHlsl(sampleUV.x, 1.0) * stepHlsl(0.0, sampleUV.y) * stepHlsl(sampleUV.y, 1.0);
        // Snapped to the centre of the low-resolution pixel whose depth, normal and colour are read.
        vec2 sampleRUV = ClampLowUV((floor(clamp(sampleUV, 0.0, 1.0) * cb.renderScale.xy / cb.bounceLayout.zw) + 0.5) * cb.bounceLayout.zw);

        vec4 texel = POINT_LOD(t_prep, sampleRUV);
        vec3 s = PositionFromLinear(sampleRUV * cb.renderScale.zw, texel.x);
        vec3 toSample = s - origin;
        float dist = length(toSample) + 0.001;
        vec3 dir = toSample / dist;
        float nearAim = saturate(1.0 - length(s - p) / radius);
        float weight = saturate(dot(normal, dir)) * saturate(dot(OctDecode(Unpack2(texel.y)), -dir)) * nearAim * onScreen * stepHlsl(NearFade, s.z);
        vec2 rg = Unpack2(texel.z);
        indirect += vec3(rg * rg, texel.w) * weight;
    }
    return vec4(indirect * (2.0 / float(SAMPLES)) * fade, origin.z);
}

vec4 GatherPlus(int samples, bool temporal, float motionPixels) {
    vec2 ruv = ClampLowUV(uv * cb.renderScale.xy);
    vec4 center = POINT_LOD(t_prep, ruv);
    vec3 origin = PositionFromLinear(uv, center.x);
    float fade = smoothstepHlsl(NearFade, NearFade * 2.0, origin.z) * (1.0 - smoothstepHlsl(DrawDistance * 0.7, DrawDistance, origin.z));
    if (fade <= 0.0) return vec4(0.0, 0.0, 0.0, origin.z);

    vec3 normal = OctDecode(Unpack2(center.y));
    vec3 noise = WRAP_LOD(t_noise, (uv * cb.renderScale.xy) / (256.0 * cb.bounceLayout.zw)).xyz;
    // A new pattern every frame (golden-ratio steps keep each frame's pattern blue noise), so the frames average out.
    if (temporal) noise.xy = fract(noise.xy + cb.bounceTemporal.z * vec2(0.618034, 0.754878));
    vec3 up = abs(normal.z) < 0.99 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(up, normal));
    vec3 bitangent = cross(normal, tangent);
    float radius = min(Radius, MaxScreenRadius * 2.0 * origin.z / cb.projectionTransform[0][0]);

    vec3 indirect = vec3(0.0);
    float seenWeight = 0.0, seen = 0.0; // the seen samples' weights added up, and how many were seen
    for (int i = 0; i < samples; i++) {
        float angle = (float(i) + noise.x) * (6.2831853 / float(samples));
        float r = radius * (0.15 + 0.85 * fract(noise.y + float(i) * 0.618034));
        vec3 p = origin + (tangent * cos(angle) + bitangent * sin(angle)) * r + normal * (r * 0.25);
        vec2 sampleUV = projectPosition(p).xy;
        float onScreen = stepHlsl(0.0, sampleUV.x) * stepHlsl(sampleUV.x, 1.0) * stepHlsl(0.0, sampleUV.y) * stepHlsl(sampleUV.y, 1.0);
        vec2 sampleRUV = ClampLowUV((floor(clamp(sampleUV, 0.0, 1.0) * cb.renderScale.xy / cb.bounceLayout.zw) + 0.5) * cb.bounceLayout.zw);

        vec4 texel = POINT_LOD(t_prep, sampleRUV);
        vec3 s = PositionFromLinear(sampleRUV * cb.renderScale.zw, texel.x);
        vec3 toSample = s - origin;
        float dist = length(toSample) + 0.001;
        vec3 dir = toSample / dist;
        float nearAim = saturate(1.0 - length(s - p) / radius);
        float visible = onScreen * stepHlsl(NearFade, s.z); // a surface on screen, not the weapon in front of it
        float weight = saturate(dot(normal, dir)) * saturate(dot(OctDecode(Unpack2(texel.y)), -dir)) * nearAim * visible;
        vec2 rg = Unpack2(texel.z);
        indirect += vec3(rg * rg, texel.w) * weight;
        seenWeight += weight;
        seen += visible;
    }
    // The samples whose surface is unknown (off screen, behind the weapon): weighted as the seen ones on average, with
    // light between the ambient's and the seen ones' average, the more of them seen the nearer theirs.
    vec3 seenLight = indirect / max(seenWeight, 1.0e-4);
    float unknownWeight = (float(samples) - seen) * seenWeight / max(seen, 1.0);
    indirect += mix(cb.bounceAmbient.rgb, seenLight, seen / float(samples)) * (unknownWeight * cb.bounceAmbient.w);
    vec3 result = indirect * (2.0 / float(samples) * 1.0) * fade; // * NVR_BounceSpill.y, 1 (the spill is retired)
    if (temporal) {
        // Last frame's at the same spot. Less of it is kept the further the spot moved on screen.
        vec3 last;
        vec3 where = LastFrame(origin);
        float found = HistoryAt(t_history, where, DepthTolerance(origin, normal), last);
        float moved = length((where.xy - uv) * cb.renderScale.xy / cb.bounceLayout.zw);
        result = mix(result, last, cb.bounceTemporal.x / (1.0 + moved / motionPixels) * saturate(found * 2.0));
    }
    return vec4(result, origin.z);
}

void main() {
    if (pc.pass_.x <= 0.5) o_color = Gather();
    else o_color = GatherPlus(int(pc.pass_.y + 0.5), pc.pass_.z > 0.5, pc.pass_.w);
}
