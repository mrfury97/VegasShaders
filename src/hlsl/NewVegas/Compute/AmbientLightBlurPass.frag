// AmbientLight passes BlurX / BlurY as a Vulkan pass (needs NVR DXVK): Effects/AmbientLight.fx.hlsl Blur, line for line (AmbientLight.glsl).
// variant bit 0: the y axis (BlurY). Both textures POINT. effects/AmbientLight.cpp ExtDraw. (AmbientLightBlur.comp is the compute
// version of both passes, AmbientLightBlurCompute; it takes precedence when on.)
#version 450
#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_control_flow_attributes : enable
#include "AmbientLight.glsl"

layout(set = 1, binding = 0) uniform texture2D t_depth;      // TESR_DepthBuffer
layout(set = 1, binding = 1) uniform texture2D t_buffer;     // NVR_ALBuffer

layout(location = 0) out vec4 o_color;

const float weights[4] = float[4](0.30, 0.23, 0.11, 0.05);

vec4 Blur(vec2 uvIn, vec2 axis) {
    float depth = readDepthLod(t_depth, uvIn);
    float tolerance = max(depth * 0.03, 2.0);
    float looseTolerance = max(depth * 0.25, 16.0);
    vec2 invTolerance = 1.0 / vec2(tolerance, looseTolerance);   // UNOFFICIAL 2026-10-26: once, not two divisions per tap (fxc: rcp + mul)
    vec4 center = pointLod(t_buffer, uvIn);
    if (DebugView > 2.5 && DebugView < 6.5) return center;
    vec3 bounce = center.rgb * weights[0];
    float visibility = center.a * weights[0];
    vec2 total = vec2(weights[0]);
    float tapDepth[7];   // UNOFFICIAL 2026-11-13 (GIGrassBlur) the taps' depths along the axis, [3] this texel's
    tapDepth[3] = depth;
    [[unroll]] for (int i = 1; i < 4; i++) {   // (2026-10-26: [unroll] as the .fx; glslang keeps loops)
        [[unroll]] for (int side = -1; side <= 1; side += 2) {
            vec2 tapUV = uvIn + axis * cb.alLayout.zw * (float(i * side) * BlurStep);
            tapDepth[3 + i * side] = readDepthLod(t_depth, tapUV);
            float dz = abs(tapDepth[3 + i * side] - depth);
            vec2 w = weights[i] * clamp(1.0 - dz * invTolerance, 0.0, 1.0);
            vec4 tap = pointLod(t_buffer, tapUV);
            visibility += tap.a * w.x;
            bounce += tap.rgb * w.y;
            total += w;
        }
    }
    // UNOFFICIAL 2026-11-13 (rtgi-review R1) GIDenoisedNoBlur (variant bit 1): the RT-denoised bounce passes through (AmbientLight.fx.hlsl Blur)
    // UNOFFICIAL 2026-11-13 GIGrassBlur (variant bit 2): where the depth is busy along the axis (grass, bushes) the loose blur is back (AmbientLight.fx.hlsl Blur)
    const float bend = min(abs(tapDepth[2] + tapDepth[4] - 2.0 * depth), min(abs(tapDepth[1] + depth - 2.0 * tapDepth[2]), abs(depth + tapDepth[5] - 2.0 * tapDepth[4])));
    const float busy = (pc.variant & 4u) != 0u ? clamp(bend / depth * 40.0 - 0.1, 0.0, 1.0) : 0.0;
    const vec3 blurred = bounce / total.y;
    return vec4((pc.variant & 2u) != 0u ? mix(center.rgb, blurred, busy) : blurred, visibility / total.x);
}

void main() { o_color = Blur(frameUV(), (pc.variant & 1u) != 0u ? vec2(0.0, 1.0) : vec2(1.0, 0.0)); }
