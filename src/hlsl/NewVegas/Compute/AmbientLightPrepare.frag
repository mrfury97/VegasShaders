// AmbientLight pass Prepare as a Vulkan pass (needs NVR DXVK): Effects/AmbientLight.fx.hlsl Prepare, line for line (AmbientLight.glsl).
// Into NVR_AmbientLightPrep (half resolution). effects/AmbientLight.cpp ExtDraw.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "AmbientLight.glsl"

layout(set = 1, binding = 0) uniform texture2D t_depth;      // TESR_DepthBuffer
layout(set = 1, binding = 1) uniform texture2D t_normals;    // TESR_NormalsBuffer
layout(set = 1, binding = 2) uniform texture2D t_viewModel;  // NVR_ALViewModel

layout(location = 0) out vec4 o_color;

void main() {
    vec2 uv = frameRectUV();
    float viewZ = PositionFromLinear(uv, pointLod(t_depth, uv).x).z;
    if (IsViewModel(t_viewModel, uv)) viewZ = 1.0e6;
    vec3 normal = normalize(pointLod(t_normals, uv).xyz * 2.0 - 1.0);
    o_color = vec4(viewZ, Pack2(OctEncode(normal)), 0.0, 0.0);
}
