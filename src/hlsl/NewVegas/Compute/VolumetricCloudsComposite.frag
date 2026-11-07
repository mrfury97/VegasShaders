// VolumetricClouds technique Composite (0) as a Vulkan pass (needs NVR DXVK): Effects/VolumetricClouds.fx.hlsl CompositePS and
// CloudShadowAt line for line, full resolution, through the frame chain. effects/VolumetricClouds.cpp ExtRender.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer (POINT, CLAMP)
layout(set = 1, binding = 1) uniform texture2D t_depth;      // TESR_DepthBuffer (POINT, CLAMP)
layout(set = 1, binding = 2) uniform texture2D t_normals;    // TESR_NormalsBuffer (POINT, CLAMP)
layout(set = 1, binding = 3) uniform texture2D t_shadow;     // TESR_CloudShadow (LINEAR, CLAMP)
layout(set = 1, binding = 4) uniform texture2D t_history;    // TESR_CloudHistory (LINEAR, CLAMP)
layout(set = 1, binding = 5) uniform texture2D t_skycov;     // TESR_SkyCoverage (POINT, CLAMP; the depth when the game's MSAA is off; UNOFFICIAL 2026-11-05)

#define CLOUDS_HISTORY
#include "VolumetricClouds.glsl"

// Includes/Depth.hlsl readDepthLod
float readDepthLod(vec2 coord) { return POINT_LOD(t_depth, coord).x * farZ; }

#include "MsaaSkyEdge.glsl"

// The clouds of this pixel from the history (the .fx's CloudsHere, without RenderScale: the Vulkan composite never had it).
vec4 CloudsHere(vec2 at) {
    vec4 clouds;
    if (LowTexel.x < cb.reciprocalResolution.x * 0.99) {
        vec2 q = 0.25 * cb.reciprocalResolution.xy;
        clouds = 0.25 * (LINEAR_LOD(t_history, at + vec2(-q.x, -q.y)) + LINEAR_LOD(t_history, at + vec2(q.x, -q.y)) +
            LINEAR_LOD(t_history, at + vec2(-q.x, q.y)) + LINEAR_LOD(t_history, at + q));
    }
    else clouds = CatmullRom(at);
    if (Sharpness > 0.0) {
        vec4 n0 = LINEAR_LOD(t_history, at + vec2(LowTexel.x, 0.0));
        vec4 n1 = LINEAR_LOD(t_history, at - vec2(LowTexel.x, 0.0));
        vec4 n2 = LINEAR_LOD(t_history, at + vec2(0.0, LowTexel.y));
        vec4 n3 = LINEAR_LOD(t_history, at - vec2(0.0, LowTexel.y));
        vec4 lo = min(min(min(n0, n1), min(n2, n3)), clouds), hi = max(max(max(n0, n1), max(n2, n3)), clouds);
        vec4 sharpened = clouds + (clouds - 0.25 * (n0 + n1 + n2 + n3)) * (1.5 * Sharpness);
        clouds = clamp(sharpened, lo, hi);
    }
    return clouds;
}

float CloudShadowAt(vec3 world) {
    vec3 light = ShadowLight();
    vec2 q = world.xy + light.xy * ((Base - world.z) / light.z);
    vec2 suv = (q - ShadowCentre()) / ShadowRange + 0.5;
    vec2 edge = min(suv, 1.0 - suv);
    float inside = saturate(min(edge.x, edge.y) * 10.0);
    return mix(1.0, LINEAR_LOD(t_shadow, suv).r, inside);
}

vec4 Composite() {
    vec4 color = POINT_LOD(t_rendered, uv);
    float depth = readDepthLod(uv);
    if (MsaaEdgeDebugOn()) return MsaaEdgeDebug(uv, depth, color.rgb);
    bool sky = depth >= farZ * 0.996;
    // UNOFFICIAL 2026-11-05 MsaaEdgeFix: as the .fx's CompositePS.
    if (MsaaEdgeFixOn()) {
        float share = MsaaSkyCoverage(uv);
        if (share > MsaaNoSky && share < MsaaAllSky) {
            vec4 clouds = CloudsHere(uv);
            vec3 behind = MsaaSkyBehind(uv, color.rgb);
            // UNOFFICIAL 2026-11-06: as the .fx's MsaaCloudsOverShare (the resolve averaged stored values: swap the share there, no black bands).
            vec3 skyBefore = delinearize(max(behind, vec3(0.0)));
            vec3 skyAfter = delinearize(max(behind * clouds.a + clouds.rgb * cb.cloudLayout.w, vec3(0.0)));
            vec3 c = max(color.rgb, vec3(0.0));
            return vec4(c - min(share * skyBefore, c) + share * skyAfter, color.a);
        }
        sky = share >= MsaaAllSky;
    }
    if (!sky) {
        if (ShadowStrength <= 0.0) return color;
        vec3 eye = toWorld(uv);
        vec3 world = (cb.cameraPosition.xyz + eye * depth) * MetresPerUnit;
        // HLSL mul((float3x3)TESR_ViewTransform, n)
        vec3 normal = normalize(POINT_LOD(t_normals, uv).xyz * 2.0 - 1.0) * mat3(cb.viewTransform);
        float height = cb.cameraPosition.z + eye.z * depth;
        bool water = normal.z > 0.7 && abs(height - cb.waterSettings.x) < max(2.0, depth * 0.002);
        if (ShadowStrength > 0.0 && !water) {
            vec3 sunDir = normalize(cb.sunDirection.xyz);
            float day = saturate(cb.sunAmount.x);
            float facing = smoothstep(0.0, 0.25, dot(normal, sunDir));
            float strength = ShadowStrength * mix(0.3, 1.0, day) * smoothstep(0.08, 0.35, sunDir.z) * facing;
            float shadow = CloudShadowAt(world);
            if (cb.extra.w > 0.0) return vec4(vec3(shadow) * mix(0.4, 1.0, facing), 1.0);
            float factor = mix(1.0, shadow, strength);
            if (factor >= 1.0) return color;
            return vec4(delinearize(max(linearize(max(color.rgb, vec3(0.0))) * factor, vec3(0.0))), color.a);
        }
        return color;
    }
    vec4 clouds = CloudsHere(uv);
    vec3 lin = linearize(max(color.rgb, vec3(0.0))) * clouds.a + clouds.rgb * cb.cloudLayout.w;
    return vec4(delinearize(max(lin, vec3(0.0))), color.a);
}

void main() {
    o_color = Composite();
}
