// AmbientLight pass Estimate as a Vulkan pass (needs NVR DXVK): Effects/AmbientLight.fx.hlsl Estimate, line for line (AmbientLight.glsl).
// Into NVR_AmbientLight0 (half resolution). effects/AmbientLight.cpp ExtDraw.
//
// UNOFFICIAL 2026-10-27 (deep/fork2-ssgi) [Shaders.AmbientLight.Main] ComputeGI: with NVR_GI defined (variants.txt AmbientLightEstimate-gi0/1/2:
// GI_SLICES x GI_STEPS = 2 x 4 as Estimate, 3 x 6, 4 x 8) the same search also gathers one bounce of light from what is on screen: a
// visibility bitmask per slice (Therrien, Levesque, Gilet 2023, "Screen Space Indirect Lighting with Visibility Bitmask"), 32 sectors of
// the slice's hemisphere spaced so each holds 1/32 of its cosine-weighted measure (t = (sin(theta - n) + 1) / 2; theta from the view
// vector, n the normal's). Each tap covers the sectors from its surface's front to its front pushed back by ComputeGIThickness (pc.gi.x),
// and the sectors it is the FIRST to cover (nearest taps first) add its light (t_light, AmbientLightGILight.frag) if its surface faces
// this pixel (the prep's normal, already read). The falloff with distance (the occlusion's weight) is a chance per tap (blue noise z,
// new each frame). The occlusion itself is this file's GTAO untouched (ComputeGIThinOcclusion, pc.gi.w: the bitmask's open sectors
// instead). Output rgb = the bounce light (linear, x the fade), a = the visibility as always. Integer bit operations: not in ps_3_0.
// Without NVR_GI (the shipped AmbientLightEstimate.spv) none of that is compiled.
#version 450
#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_control_flow_attributes : enable
#include "AmbientLight.glsl"

#ifdef NVR_GI
#define AL_SLICES GI_SLICES
#define AL_STEPS GI_STEPS
#else
#define AL_SLICES SLICES
#define AL_STEPS STEPS
#endif

layout(set = 1, binding = 0) uniform texture2D t_prep;       // NVR_ALPrep (POINT)
layout(set = 1, binding = 1) uniform texture2D t_blueNoise;  // TESR_BlueNoiseSampler (POINT, WRAP)
#ifdef NVR_GI
layout(set = 1, binding = 2) uniform texture2D t_light;      // NVR_AmbientLightGILight: the frame's light per prep texel, sqrt (POINT)
#endif

layout(location = 0) out vec4 o_color;

float SliceArc(float horizonCos0, float horizonCos1, float n, float cosNormal, float sinN) {
    float h0 = n + clamp(-acos(clamp(horizonCos1, -1.0, 1.0)) - n, -1.5707963, 1.5707963);
    float h1 = n + clamp(acos(clamp(horizonCos0, -1.0, 1.0)) - n, -1.5707963, 1.5707963);
    return (cosNormal + 2.0 * h0 * sinN - cos(2.0 * h0 - n)) * 0.25 + (cosNormal + 2.0 * h1 * sinN - cos(2.0 * h1 - n)) * 0.25;
}

#ifdef NVR_GI
// The sectors from t0 to t1 (0..1 across the slice's hemisphere), rounded to whole sectors: a surface that covers less than half a
// sector covers none (a flat floor's own taps lie on its tangent plane, at the hemisphere's edge, and must not light it).
uint Sectors(float t0, float t1) {
    uint a = uint(clamp(t0, 0.0, 1.0) * 32.0 + 0.5);
    uint b = uint(clamp(t1, 0.0, 1.0) * 32.0 + 0.5);
    uint count = b > a ? b - a : 0u;
    return count == 0u ? 0u : ((0xFFFFFFFFu >> (32u - count)) << a);
}

// sin(theta - n) for a direction with cos(theta) = c (to the view vector) on the given side, clamped to the hemisphere round the normal.
// theta stays on its side's half of the slice (0..pi on side +1, -pi..0 on side -1), so a direction outside the hemisphere (below the
// surface) can only lie beyond that side's edge: +1 on side +1, -1 on side -1. (2026-10-27 fix: the first version took the sign of
// sin(theta - n), which flips past theta - n = -pi: a floor tap near the pixel on the camera's side, pushed back by the thickness to
// below the floor, then covered the whole hemisphere with the floor's own light: the even lift over the carpet in the first v2 run.)
float SliceSin(float c, float side, float cosN, float sinN) {
    float s = side * sqrt(max(1.0 - c * c, 0.0));
    float sinD = s * cosN - c * sinN;
    float cosD = c * cosN + s * sinN;
    return cosD < 0.0 ? side : sinD;
}
#endif

vec4 Estimate(vec2 uv) {
    vec4 center = pointLod(t_prep, uv);
    vec3 origin = PositionFromViewZ(uv, center.x);
    float fade = smoothstep(NearFade, NearFade * 2.0, origin.z) * (1.0 - smoothstep(DrawDistance * 0.7, DrawDistance, origin.z));
    if (fade <= 0.0) return vec4(0.0, 0.0, 0.0, 1.0);

    vec3 normal = OctDecode(Unpack2(center.y));
    vec3 viewVec = normalize(-origin);
    vec3 noise = textureLod(sampler2D(t_blueNoise, NVR_SAMPLER(pc.pointWrap)), uv / (256.0 * cb.alLayout.zw), 0.0).xyz;
    noise.xy = fract(noise.xy + (cb.alTemporal.x > 0.0 ? cb.alTemporal.z : 0.0) * vec2(0.618034, 0.754878));
    vec2 screenRadius = Radius * vec2(cb.projectionTransform[0][0], cb.projectionTransform[1][1]) / (2.0 * origin.z);
    screenRadius *= min(1.0, MaxScreenRadius / screenRadius.x);
    float falloffStart = 0.4 * Radius;
    // UNOFFICIAL 2026-10-26: the loop's constant divisors as reciprocals, once (fxc's rcp + mul; D3DX takes the first to its
    // preshader): GLSL's division is a full-precision one per sample and per component.
    float invFalloff = 1.0 / (Radius - falloffStart);
    vec2 invTexel = 1.0 / cb.alLayout.zw;
#ifdef NVR_GI
    float chance = fract(noise.z + (cb.alTemporal.x > 0.0 ? cb.alTemporal.z : 0.0) * 0.569840);
    float thickness = max(pc.gi.x, 1.0);
    float openSum = 0.0;
    vec3 light = vec3(0.0);
#endif

    if (DebugView > 2.5 && DebugView < 3.5) return vec4(normal * 0.5 + 0.5, 1.0);
    if (DebugView > 3.5 && DebugView < 4.5) return vec4(origin.z / 2000.0, fract(origin.z / 100.0), 0.0, 1.0);
    if (DebugView > 4.5 && DebugView < 5.5) return vec4(screenRadius * 10.0, Radius / 1000.0, 1.0);
    float visibility = 0.0, weightSum = 0.0;
    vec3 bounce = vec3(0.0);
    float firstCos = 0.0, firstLow = 0.0;
    [[unroll]] for (int slice = 0; slice < AL_SLICES; slice++) {   // (2026-10-26: [unroll] as the .fx; glslang keeps loops)
        float phi = (float(slice) + noise.x) * (3.14159265 / float(AL_SLICES));
        vec2 omega = vec2(cos(phi), -sin(phi));
        vec3 direction = vec3(cos(phi), sin(phi), 0.0);
        vec3 orthoDirection = direction - dot(direction, viewVec) * viewVec;
        vec3 axis = normalize(cross(orthoDirection, viewVec));
        vec3 projectedNormal = normal - axis * dot(normal, axis);
        float projectedLength = length(projectedNormal) + 1e-5;
        float signNormal = dot(orthoDirection, projectedNormal) >= 0.0 ? 1.0 : -1.0;
        float cosNormal = saturate(dot(projectedNormal, viewVec) / projectedLength);
        float n = signNormal * acos(cosNormal);
        float lowCos0 = cos(n + 1.5707963 - cb.alEdges.y), lowCos1 = cos(n - 1.5707963 + cb.alEdges.y);
        float horizonCos0 = lowCos0, horizonCos1 = lowCos1;
#ifdef NVR_GI
        float sinNGI = sin(n);
        uint covered = 0u;
        vec3 sliceLight = vec3(0.0);
#endif
        [[unroll]] for (int k = 0; k < AL_STEPS; k++) {
            float s = (float(k) + fract(noise.y + float(k) * 0.618034)) / float(AL_STEPS);
            s *= s;
            vec2 offset = omega * screenRadius * max(s, 2.0 * cb.alLayout.z);
            // UNOFFICIAL 2026-11-08 (alsky) MinReach, as the D3D9 Estimate's since 2026-11-01 (NVR_ALReach x / y, here cb.sunFarFlags.y / z;
            // tester at native 4K on these passes: AmbientLight darkened the far ground with a hard line): the max above compares a share of
            // the radius with a texel size, so far away the nearest reads fell on the pixel's own texel, a read at distance 0 that counts as
            // a 90-degree horizon, and the ground darkened itself from that distance on. On: every read at least MinReachTexels texels out.
            if (cb.sunFarFlags.y > 0.5) {
                vec2 reach = screenRadius * s;
                offset = omega * reach * max(1.0, cb.sunFarFlags.z * cb.alLayout.z / max(reach.x, 1.0e-6));
            }
            [[unroll]] for (int side = 0; side < 2; side++) {
                vec2 sampleUV = uv + (side == 0 ? offset : -offset);
                float onScreen = step(0.0, sampleUV.x) * step(sampleUV.x, 1.0) * step(0.0, sampleUV.y) * step(sampleUV.y, 1.0);
                sampleUV = (floor(clamp(sampleUV, 0.0, 1.0) * invTexel) + 0.5) * cb.alLayout.zw;
                vec4 texel = pointLod(t_prep, sampleUV);
                vec3 delta = PositionFromViewZ(sampleUV, texel.x) - origin;
                float dist = length(delta) + 1e-4;
                vec3 dir = delta * (1.0 / dist);
                float weight = saturate((Radius - dist) * invFalloff) * onScreen * step(NearFade, origin.z + delta.z);
                float lowCos = side == 0 ? lowCos0 : lowCos1;
                float sampleCos = mix(lowCos, dot(dir, viewVec), weight);
                if (side == 0) horizonCos0 = max(horizonCos0, sampleCos); else horizonCos1 = max(horizonCos1, sampleCos);
                if (slice == 0 && side == 0 && k == 1) { firstCos = dot(dir, viewVec); firstLow = weight; }
#ifdef NVR_GI
                if (weight > fract(chance + float(k * 2 + side) * 0.618034)) {
                    float sideSign = side == 0 ? 1.0 : -1.0;
                    float front = SliceSin(dot(dir, viewVec), sideSign, cosNormal, sinNGI);
                    float back = SliceSin(dot(normalize(delta - viewVec * thickness), viewVec), sideSign, cosNormal, sinNGI);
                    uint bits = Sectors(0.5 + 0.5 * min(front, back), 0.5 + 0.5 * max(front, back));
                    uint fresh = bits & ~covered;
                    if (fresh != 0u) {
                        // Its surface sends light this way only if it faces this pixel (its normal against the direction to it).
                        float facing = saturate(-dot(OctDecode(Unpack2(texel.y)), dir) * 8.0);
                        vec3 l = pointLod(t_light, sampleUV).rgb;
                        sliceLight += (float(bitCount(fresh)) * facing) * (l * l);
                    }
                    covered |= bits;
                }
#endif
            }
        }
        float sinN = sin(n);
        visibility += projectedLength * SliceArc(horizonCos0, horizonCos1, n, cosNormal, sinN);
        weightSum += projectedLength * SliceArc(lowCos0, lowCos1, n, cosNormal, sinN);
#ifdef NVR_GI
        // The bitmask's shares, each slice weighed as GTAO weighs it (its whole hemisphere's arc).
        float full = projectedLength * SliceArc(lowCos0, lowCos1, n, cosNormal, sinN);
        openSum += full * (1.0 - float(bitCount(covered)) * (1.0 / 32.0));
        light += full * sliceLight * (1.0 / 32.0);
#endif
    }
#ifdef NVR_GI
    if (pc.gi.w > 0.5) visibility = openSum;   // ComputeGIThinOcclusion: the open sectors (both / weightSum below)
    bounce = light / max(weightSum, 1e-4);
    // DebugView 6 only: Estimate's raw values go out for every view from 6 up (its rgb was always 0 and no later view reads it), but
    // the GI's rgb is the light DebugView 13 shows (the first v2 run showed these raw values there: the pink "normal" picture).
    if (DebugView > 5.5 && DebugView < 6.5) return vec4(visibility / max(weightSum, 1e-4), firstCos * 0.5 + 0.5, firstLow, 1.0);
#else
    if (DebugView > 5.5) return vec4(visibility / max(weightSum, 1e-4), firstCos * 0.5 + 0.5, firstLow, 1.0);
#endif
    visibility = pow(saturate(visibility / max(weightSum, 1e-4)), 1.5 * OcclusionStrength);
    visibility = mix(1.0, visibility, fade);
#ifdef NVR_GI
    return vec4(bounce * fade, saturate(visibility));
#else
    return vec4(bounce * (BounceScale * fade / float(SLICES * STEPS * 2)), saturate(visibility));
#endif
}

void main() { o_color = Estimate(frameUV()); }
