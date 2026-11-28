// AmbientLight pass StatsSample (indoors) as a Vulkan pass (needs NVR DXVK): Effects/AmbientLight.fx.hlsl StatsAt / StatsSample, line for
// line (AmbientLight.glsl). Into NVR_AmbientLightStats (16 x 9). effects/AmbientLight.cpp ExtDraw.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "AmbientLight.glsl"

layout(set = 1, binding = 0) uniform texture2D t_source;            // TESR_SourceBuffer (LINEAR)
layout(set = 1, binding = 1) uniform texture2D t_depth;             // TESR_DepthBuffer
layout(set = 1, binding = 2) uniform texture2D t_viewModel;         // NVR_ALViewModel
layout(set = 1, binding = 3) uniform texture2D t_occlusionHistory;  // NVR_ALOcclusionHistory (this frame's)
layout(set = 1, binding = 4) uniform texture2D t_normals;           // TESR_NormalsBuffer
layout(set = 1, binding = 5) uniform texture2D t_gridAccum;         // NVR_GridAccum (LINEAR)
layout(set = 1, binding = 6) uniform texture2D t_gridAccumColor;    // NVR_GridAccumColor (LINEAR)

layout(location = 0) out vec4 o_color;

const vec2 StatsSize = vec2(16.0, 9.0);

vec3 StatsAt(vec2 s01) {
    vec2 uv = screenToRect(s01);
    vec3 lin = linearize(linearLod(t_source, uv).rgb);
    float depth = readDepthLod(t_depth, uv);
    if (depth < NearFade || depth > DrawDistance || IsViewModel(t_viewModel, uv)) return vec3(luma(lin), 0.0, 0.0);
    float ao = mix(DarkestAO, 1.0, pointLod(t_occlusionHistory, uv).r);
    ao = mix(ao, 1.0, saturate((luma(lin) - BrightFade) * 3.0));
    vec3 albedo = SurfaceColour(lin);
    vec3 normal = normalize(GetWorldNormalLod(t_normals, uv));
    vec3 bounce = GridBounce(t_gridAccum, t_gridAccumColor, toWorld(s01) * depth, normal) * (cb.gridData.x * ao) * albedo;
    vec3 taken = (1.0 - ao) * min(lin, albedo * cb.alScene.rgb);
    return vec3(luma(lin), luma(taken), luma(bounce));
}

void main() {
    vec2 cell = floor(frameUV() * StatsSize);
    vec3 sum = vec3(0.0);
    for (int j = 0; j < 2; j++) {
        for (int i = 0; i < 2; i++) sum += StatsAt((cell * 2.0 + vec2(float(i), float(j)) + 0.5) / (StatsSize * 2.0));
    }
    o_color = vec4(sum, 1.0);
}
