// Underwater pass 1, the god rays (needs NVR DXVK): Effects/Underwater.fx.hlsl Godrays line for line. effects/Underwater.cpp ExtRender.
// The rays are drawn into the upper left quarter (uv * 2); outside it the pass keeps the current image (what the D3D9 pass's
// pre-fill copied there). t_noise is Effects\bluenoise256.dds (POINT, WRAP; no mips: level 0). Depth: CombineDepth's.
#version 450
#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_samplerless_texture_functions : enable
#include "Common.glsl"
#include "Underwater.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer: the current image (texelFetch, outside the quarter)
layout(set = 1, binding = 1) uniform texture2D t_depth;      // TESR_DepthBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 2) uniform texture2D t_noise;      // TESR_BlueNoiseSampler (POINT, WRAP)
layout(set = 1, binding = 3) uniform texture2D t_caustics;   // TESR_CausticsSampler (ANISOTROPIC, WRAP)

layout(push_constant) uniform Push {
    uint linearIndex;     // 0: LINEAR, CLAMP
    uint noiseIndex;      // 4: POINT, WRAP
    uint causticsIndex;   // 8: ANISOTROPIC, WRAP
    uint pad;             // 12
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

// Underwater.fx.hlsl random: the blue noise at (seed / 256 + 0.5) / TESR_ReciprocalResolution.xy, level 0
vec3 random(vec2 seed) {
    return textureLod(sampler2D(t_noise, NVR_SAMPLER(pc.noiseIndex)), (seed / 256.0 + 0.5) / cb.reciprocalResolution.xy, 0.0).xyz;
}

void main() {
    // clip((uv <= 1) - 1) with uv = IN.UVCoord * 2: outside the quarter the D3D9 pass left the pre-filled current image
    vec2 uv2 = uv * 2.0;
    if (any(greaterThan(uv2, vec2(1.0)))) {
        o_color = texelFetch(t_rendered, ivec2(gl_FragCoord.xy), 0);
        return;
    }

    vec2 ruv2 = uv2 * cb.renderScale.xy;
    vec3 eyeVector = toWorld(uv2);
    vec2 px = cb.reciprocalResolution.xy;
    float depth = length(readDepth(clamp(ruv2, 0.5 * px, max(cb.renderScale.xy - 0.5 * px, 0.5 * px)), t_depth, pc.linearIndex) * eyeVector);

    float godraysMaxDistance = 1000.0;
    float stepSize = godraysMaxDistance / 50.0;
    vec3 stepVector = eyeVector * stepSize;

    vec4 color = vec4(0.0, 0.0, 0.0, 1.0);

    vec3 samplingUV = cb.cameraPosition.xyz;
    float sunL = sunLuma();
    for (int i = 1; i < 50; i++) {
        float fi = float(i);
        samplingUV += stepVector * mix(vec3(0.5), vec3(1.0), random(ruv2 + cb.reciprocalResolution.xx * fi));
        float rayDistance = stepSize * fi;
        float samplingDepthFade = hsmooth(800.0, 0.0, cb.waterSettings.x - samplingUV.z);
        float samplingDistanceFade = 1.0 - rayDistance / godraysMaxDistance;
        vec4 rayColor = mix(vec4(0.0, 0.0, 0.0, 1.0), vec4(1.0, 1.0, 1.0, 1.0), samplingDistanceFade) * sunL * 0.06;
        color += getCaustics(samplingUV, t_caustics, pc.causticsIndex) * rayColor * samplingDepthFade * samplingDistanceFade * float(rayDistance < depth);
    }

    o_color = vec4(smoothstep(0.1, 0.4, color).rgb, 1.0); // clamp the values to increase the strength and improve blending
}
