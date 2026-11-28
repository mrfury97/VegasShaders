// GodRays technique GodRaysMaxReduce as a Vulkan pass (needs NVR DXVK): Effects/GodRays.fx.hlsl PS_Reduce, line for line.
// 8 x 8 block maximum per output pixel. effects/GodRays.cpp RunReduce.
#version 450
#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_control_flow_attributes : enable
#include "GodRays.glsl"

layout(set = 1, binding = 0) uniform texture2D t_buffer;     // NVR_GodRaysBuffer (LINEAR)

layout(location = 0) out vec4 o_color;

void main() {
    vec2 origin = floor(gl_FragCoord.xy) * 8.0 + 0.5;
    float m = 0.0;
    [[unroll]] for (int j = 0; j < 8; j++) {
        [[unroll]] for (int i = 0; i < 8; i++) {
            vec3 c = LINEAR_LOD(t_buffer, (origin + vec2(float(i), float(j))) * pc.reduce.xy).rgb;
            m = max(m, max(c.r, max(c.g, c.b)));
        }
    }
    o_color = vec4(m, m, m, 1.0);
}
