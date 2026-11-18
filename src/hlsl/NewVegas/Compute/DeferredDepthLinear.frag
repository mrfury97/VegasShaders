// UNOFFICIAL 2026-11-14 DeferredDepth, mode 2 under the DXVK fork (core/DeferredDepth.h Linearize; needs NVR DXVK): the main list's depth
// copy (TESR_DeferredDepth, INTZ) turned into view z (game units, fp32) in TESR_DeferredDepthLinear (R32F), which D3D9 passes then read
// as any float texture. The depth is fetched without a sampler (texelFetch: no sampler heap involved), the image read as CombineDepth.frag
// reads the world depth. Push: x, y the projection's _33, _43 (device depth d = _33 + _43 / view z). Same maths as
// Shaders/Includes/DeferredDepth.hlsl DDViewZ and Shaders/DeferredDepthLinear.hlsl (the D3D9 pass, native D3D9).
#version 450
#extension GL_EXT_samplerless_texture_functions : enable

layout(location = 0) out vec4 outColor;

layout(set = 1, binding = 0) uniform texture2D t_depth;

layout(push_constant) uniform Push {
    vec4 proj;	// x _33, y _43
} pc;

void main() {
    float d = texelFetch(t_depth, ivec2(gl_FragCoord.xy), 0).x;
    float den = d - pc.proj.x;
    if (abs(den) < 1.0e-12) den = pc.proj.y < 0.0 ? -1.0e-12 : 1.0e-12;
    outColor = vec4(pc.proj.y / den, 0.0, 0.0, 1.0);
}
