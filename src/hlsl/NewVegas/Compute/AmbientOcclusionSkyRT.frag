// UNOFFICIAL 2026-10-28 (NVR DXVK fork, hybrid RT item 2; docs/next/FORK-RT-AO.md) [Shaders.AmbientOcclusion.RT] Sky: Skylighting v2's
// sky factor (SkyVisibilityV2, AmbientOcclusionSky.frag) from the RT helper's traced sky visibility (RtAOTrace.comp, NVR_RTAORaw .y,
// 'covered' .z) instead of the top-down ortho map: overhangs, bridges, porches and the inside of buildings block the sky by their real
// shape. Same target, same output (x the factor, y the depth, z the visibility), same settings (Strength, OpenThreshold, ShareApply,
// AmbientScale, the sun mask; LightCap and Skylighting's DebugView 1-4 downstream), so AmbientLight's apply and the fog's composite take
// it unchanged. effects/AmbientOcclusion.cpp RenderSkyVisibility. FrameVS's uv in the sky target's viewport, as the v2 pass.
// 2026-10-29: the trace turns its sky rings by a fixed 4 x 4 per-texel interleave (16 turns, no noise over time); this pass averages
// the 4 x 4 traced texels round its own (any 4 neighbours in a row cover all 4 turns of that axis), each kept only where its view
// distance matches this texel's (5 %, at least 4 units: no sky light bleeding across an edge). So the interleave never shows.
#version 450
#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_samplerless_texture_functions : require
#include "AmbientOcclusion.glsl"

layout(set = 1, binding = 0) uniform texture2D t_depth;      // TESR_DepthBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 1) uniform texture2D t_normals;    // TESR_NormalsBuffer (NONE = POINT, CLAMP)
layout(set = 1, binding = 2) uniform texture2D t_sunMask;    // NVR_SunMask (POINT, CLAMP); a stand-in unless cb.sky4.z > 0.5
layout(set = 1, binding = 3) uniform texture2D t_skyPrep;    // NVR_SkyPrep (POINT, CLAMP); a stand-in unless cb.skyLayout.z > 0.5
layout(set = 1, binding = 4) uniform texture2D t_rt;         // NVR_RTAORaw (half resolution; texelFetch)
layout(set = 1, binding = 5) uniform texture2D t_rtInput;    // NVR_RTAOInput (the trace's input: x view distance; texelFetch)

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

vec2 SkyUnpack2(float p) { float high = floor(p / 4096.0); return vec2(high, p - high * 4096.0) / 4095.0; }
vec3 SkyOctDecode(vec2 e) {
    e = e * 2.0 - 1.0;
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    float t = saturate(-n.z);
    n.xy += mix(vec2(t), vec2(-t), greaterThanEqual(n.xy, vec2(0.0)));
    return normalize(n);
}

void main() {
    // FrameVS's uv in this viewport (as AmbientOcclusionSky.frag): texel i at i / size + the quad's 0.5 / full resolution.
    vec2 inUV = uv + 0.5 * (cb.reciprocalResolution.xy - cb.skyLayout.xy);
    vec2 uv = (floor(inUV / cb.reciprocalResolution.xy) + 0.5) * cb.reciprocalResolution.xy;
    float depth = LINEAR_LOD(t_depth, uv).x * farZ;
    vec3 viewNormal = POINT_LOD(t_normals, uv).xyz * 2.0 - 1.0;
    if (cb.skyLayout.z > 0.5) viewNormal = SkyOctDecode(SkyUnpack2(POINT_LOD(t_skyPrep, inUV).y));
    float result = 1.0, skyVisibility = 1.0;
    float sunMask = cb.sky4.z > 0.5 ? mix(cb.shadowFade.x, 1.0, POINT_LOD(t_sunMask, uv).r) : 1.0;
    if (depth < endFade) {
        vec3 n = normalize((cb.invViewTransform * vec4(viewNormal, 0.0)).xyz);
        ivec2 size = textureSize(t_rt, 0);
        ivec2 centre = clamp(ivec2(inUV * vec2(size)), ivec2(0), size - 1);
        float centreZ = texelFetch(t_rtInput, centre, 0).x;
        float tolerance = max(centreZ * 0.05, 4.0);
        vec2 sum = vec2(0.0);
        float weight = 0.0;
        for (int y = -1; y <= 2; y++) {
            for (int x = -1; x <= 2; x++) {
                ivec2 t = clamp(centre + ivec2(x, y), ivec2(0), size - 1);
                float w = abs(texelFetch(t_rtInput, t, 0).x - centreZ) < tolerance ? 1.0 : 0.0;
                sum += w * texelFetch(t_rt, t, 0).yz;
                weight += w;
            }
        }
        vec2 rt = weight > 0.0 ? sum / weight : texelFetch(t_rt, centre, 0).yz;   // x sky visibility, y covered (a share now)
        // UNOFFICIAL 2026-11-08 (alsky) SkySoftEdges (cb.sky5.z, [Shaders.AmbientOcclusion.RT]): open sky over the last quarter before
        // endFade, where this pass, the trace's input and the composites stop. The ortho version is open there already (its map ends
        // long before); the traced one is not, so its factor stopped with a hard line at 8000 units. 0: as before.
        float farOpen = cb.sky5.z > 0.5 ? smoothstep(0.75 * endFade, endFade, depth) : 0.0;
        rt = mix(rt, vec2(1.0, 0.0), farOpen);
        float visibility = saturate(rt.x * cb.sky3.y);
        result = SkyFactor(visibility, n, sunMask, rt.y);
        skyVisibility = visibility;
    }
    o_color = vec4(result, min(depth, endFade), skyVisibility, 1.0);
}
