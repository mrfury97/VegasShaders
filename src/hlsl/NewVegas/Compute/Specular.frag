// Specular, technique 0 (needs NVR DXVK): adds the sun's highlight and the sky's reflection to the image, as Effects/
// Specular.fx.hlsl's SpecularPS line for line. effects/Specular.cpp ExtRender (technique 0) runs it through the frame chain.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "SpecularCommon.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;         // TESR_RenderedBuffer
layout(set = 1, binding = 1) uniform texture2D t_depth;            // TESR_DepthBuffer
layout(set = 1, binding = 2) uniform texture2D t_normals;          // TESR_NormalsBuffer
layout(set = 1, binding = 3) uniform texture2D t_pointShadow;      // TESR_PointShadowBuffer
layout(set = 1, binding = 4) uniform texture2D t_depthViewModel;   // TESR_DepthBufferViewModel
layout(set = 1, binding = 5) uniform texture2D t_specularBuffer;   // TESR_SpecularBuffer

layout(push_constant) uniform Push {
    uint pointSampler;       // 0: POINT/CLAMP heap index (filled by the host)
    uint linearSampler;      // 4: LINEAR/CLAMP heap index
    uint pad[2];
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

void main() {
    vec2 ruv = uv * cb.renderScale.xy;
    vec4 color = sampleLod(t_rendered, pc.pointSampler, ruv);
    vec3 lin = linearize(color.rgb);
    if (!addSpecular(ruv, readDepthLod(t_depth, pc.pointSampler, ruv), pc.pointSampler, pc.linearSampler,
                     t_depthViewModel, t_normals, t_pointShadow, t_specularBuffer, lin)) {
        o_color = color;
        return;
    }
    o_color = vec4(delinearize(lin), color.a);
}
