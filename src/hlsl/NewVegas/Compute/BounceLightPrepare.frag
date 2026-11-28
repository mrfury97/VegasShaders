// BounceLight pass Prepare (first technique) and PreparePlus (every other technique) as a Vulkan pass (needs NVR DXVK):
// Effects/BounceLight.fx.hlsl line for line, into NVR_BouncePrep (or PrepQuarter). uv is HalfVS's: the low-resolution texel centre.
// pc.pass_.x: 1 = PreparePlus.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "BounceLight.glsl"

layout(set = 1, binding = 0) uniform texture2D t_depth;      // TESR_DepthBuffer (POINT, CLAMP)
layout(set = 1, binding = 1) uniform texture2D t_normals;    // TESR_NormalsBuffer (NONE: POINT, CLAMP)
layout(set = 1, binding = 2) uniform texture2D t_source;     // TESR_SourceBuffer: the current image (LINEAR, CLAMP)
layout(set = 1, binding = 3) uniform texture2D t_history;    // NVR_BounceHistory: last frame's result (POINT, CLAMP; PreparePlus)

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

vec3 NormalLod(vec2 coord) {
    return normalize(POINT_LOD(t_normals, coord).xyz * 2.0 - 1.0);
}

void main() {
    vec2 ruv = ClampFullUV(uv * cb.renderScale.xy);
    float linear01 = POINT_LOD(t_depth, ruv).x;
    vec3 normal = NormalLod(ruv);
    if (pc.pass_.x <= 0.5) {
        // Prepare. Capped so small bright glints (specular highlights, lamps) do not spray light around.
        vec3 light = min(linearize(LINEAR_LOD(t_source, ruv).rgb), vec3(1.0));
        o_color = vec4(linear01, Pack2(OctEncode(normal)), Pack2(sqrt(light.rg)), light.b);
        return;
    }
    // PreparePlus: the bounce light this surface showed last frame is light it sends on as well.
    vec3 scene = linearize(LINEAR_LOD(t_source, ruv).rgb);
    vec3 light = min(scene, vec3(1.0));
    if (cb.bounceTemporal.y > 0.0) {
        vec3 position = PositionFromLinear(uv, linear01);
        vec3 last;
        float found = HistoryAt(t_history, LastFrame(position), DepthTolerance(position, normal), last);
        if (found > 0.0) {
            // As Combine adds it: Strength * bounce * the surface's hue.
            float peak = max(max(scene.r, scene.g), max(scene.b, 0.0001));
            light = min(light + (cb.bounceTemporal.y * saturate(found * 2.0) * Strength * 0.6 / peak) * last * scene, vec3(1.0));
        }
    }
    o_color = vec4(linear01, Pack2(OctEncode(normal)), Pack2(sqrt(light.rg)), light.b);
}
