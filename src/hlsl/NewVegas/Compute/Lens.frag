// Lens as a Vulkan pass (NVR DXVK). Effects/Lens.fx.hlsl, line for line.
// Textures: set 1 binding 0 = source frame copy, binding 1 = first bloom level (may be absent), binding 2 = dirt (Effects\dirtlens.png).
// Samplers: LINEAR CLAMP at push offset 16 (source, bloom), LINEAR WRAP at 20 (dirt).
// The dirt is read at the frame's own uv: the D3D9 render-rect mapping is the identity without RenderScale, and this path is only
// used when the C++ allows it (see the spec).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_source;   // TESR_SourceBuffer
layout(set = 1, binding = 1) uniform texture2D t_bloom;    // TESR_BloomBuffer
layout(set = 1, binding = 2) uniform texture2D t_dirt;     // TESR_LensSampler

layout(push_constant) uniform Push {
    vec4 lensData;        // 0   TESR_LensData (x strength, y bloom exponent, z smudginess)
    uint samplerIndex;    // 16  LINEAR CLAMP
    uint wrapIndex;       // 20  LINEAR WRAP
    uint pad0;            // 24
    uint pad1;            // 28
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

vec4 sampleLod(texture2D t, uint samplerIndex, vec2 at) {
    return textureLod(sampler2D(t, NVR_SAMPLER(samplerIndex)), at, 0.0);
}

void main() {
    vec2 ruv = gl_FragCoord.xy / vec2(textureSize(sampler2D(t_source, NVR_SAMPLER(pc.samplerIndex)), 0));
    float strength = pc.lensData.x;
    float bloomExponent = pc.lensData.y;
    float smudginess = pc.lensData.z;

    vec3 source = sampleLod(t_source, pc.samplerIndex, ruv).rgb;

    vec3 bloom = sampleLod(t_bloom, pc.samplerIndex, ruv).rgb;
    float bloomLuma = max(dot(bloom, vec3(0.2126, 0.7152, 0.0722)), 1.0e-6);
    vec3 shapedBloom = (bloom / bloomLuma) * pow(bloomLuma, bloomExponent);

    float dirt = sampleLod(t_dirt, pc.wrapIndex, uv).r;
    dirt = pow(max(dirt, 0.0), max(3.0 - smudginess, 0.0));

    vec3 added = dirt * shapedBloom * strength;
    vec3 linearColor = linearize(source) + added;
    o_color = vec4(delinearize(linearColor), 1.0);
}
