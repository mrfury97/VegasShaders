// DepthOfField, technique 0 (autofocus), pass 1 (DoF) as a Vulkan pass (needs NVR DXVK): Effects/DepthOfField.fx.hlsl DoF line for
// line; effects/DepthOfField.cpp ExtRender. Writes the blur amounts (.r near, .g far) into the frame chain. showDepth is 0 there, so
// the debug output is not ported. t_depth is TESR_DepthBuffer (CombinedDepth), t_avgLuma is TESR_AvgLumaBuffer (1x1).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_depth;      // TESR_DepthBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 1) uniform texture2D t_avgLuma;    // TESR_AvgLumaBuffer (LINEAR, CLAMP)

#include "DepthOfField.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 o_color;

// Depth.hlsl: readDepth (one mip level, so LOD 0 is what tex2D picks anyway)
float readDepth(vec2 coord) {
    // UNOFFICIAL 2026-11-01 JitterFix: pad holds TESR_DepthOfFieldJitter.xy as floats (effects/DepthOfField.cpp ExtRender)
    vec2 jitter = vec2(uintBitsToFloat(pc.pad[0]), uintBitsToFloat(pc.pad[1]));
    return textureLod(sampler2D(t_depth, NVR_SAMPLER(pc.samplerIndex)), coord - jitter, 0.0).x * pc.cameraData.y;
}

// Depth.hlsl: toWorld
vec3 toWorld(vec2 tex) {
    vec3 v = vec3(pc.viewTransform[0][2], pc.viewTransform[1][2], pc.viewTransform[2][2]);
    v += (1.0 / pc.projectionTransform[0][0] * (2.0 * tex.x - 1.0)) * vec3(pc.viewTransform[0][0], pc.viewTransform[1][0], pc.viewTransform[2][0]);
    v += (-1.0 / pc.projectionTransform[1][1] * (2.0 * tex.y - 1.0)) * vec3(pc.viewTransform[0][1], pc.viewTransform[1][1], pc.viewTransform[2][1]);
    return v;
}

void main() {
    vec2 uv = inUV;

    const float DistantStrength = max(pc.blur.x, 0.0);
    const float DistantStart = pc.blur.y;
    const float DistantEnd = pc.blur.z;
    const float HyperFocalDistance = max(0.0000001, pc.data.x * 1000.0);
    const float NearBlurCutoff = min(HyperFocalDistance, pc.data.w);
    // static const float focalDistance: blue channel of TESR_AvgLumaBuffer at its centre, a fraction of HyperFocalDistance
    const float focalDistance = textureLod(sampler2D(t_avgLuma, NVR_SAMPLER(pc.samplerIndex)), vec2(0.5, 0.5), 0.0).b * HyperFocalDistance;

    float depth = readDepth(uv);
    vec3 camera_vector = toWorld(uv) * depth;
    vec4 world_pos = vec4(pc.cameraPosition.xyz + camera_vector, 1.0);

    float focalLength = 0.01; // mostly negligible and used here to avoid dividing by 0

    // near and far plane from the hyperfocal distance (http://www.waloszek.de/gen_dof_e.php)
    float nearPlane = focalDistance * (HyperFocalDistance - focalLength) / (HyperFocalDistance + focalDistance - 2.0 * focalLength);
    float farPlane = focalDistance * (HyperFocalDistance - focalLength) / (HyperFocalDistance - focalDistance);

    float nearblur = invlerps(nearPlane, 0.0, depth);
    float farBlur = invlerps(farPlane, farPlane * 2.0, depth) * 0.8;

    nearblur *= 0.5 * smoothstepHLSL(NearBlurCutoff / 2.0, 0.0, depth) + smoothstepHLSL(NearBlurCutoff / 2.0, NearBlurCutoff, depth); // attenuate near blur to reveal gun
    farBlur = saturate(farBlur + invlerps(DistantStart, DistantEnd, depth) * DistantStrength * invlerps(100000.0, 5000.0, world_pos.z)); // distance blur, sky excluded

    o_color = vec4(nearblur, farBlur, 1.0, 1.0);
}
