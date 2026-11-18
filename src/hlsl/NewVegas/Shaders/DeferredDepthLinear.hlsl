// UNOFFICIAL 2026-11-14 DeferredDepth, mode 2 as a D3D9 pass (core/DeferredDepth.h Linearize: native D3D9, or DXVK without the NVR
// extension): the main list's depth copy (TESR_DeferredDepth, INTZ) turned into view z (game units, fp32) in TESR_DeferredDepthLinear
// (R32F). Compute/DeferredDepthLinear.frag is the same pass under the fork. Loaded by ShaderRecord::LoadShader at startup when mode 2 is on.

#include "Includes/DeferredDepth.hlsl"

float4 DDL_Proj : register(c0);	// x _33, y _43, zw 1 / the copy's size
sampler2D DDL_Depth : register(s0);	// TESR_DeferredDepth (INTZ), POINT

float4 main(float2 vpos : VPOS) : COLOR0 {
    const float device = tex2Dlod(DDL_Depth, float4((vpos + 0.5f) * DDL_Proj.zw, 0.0f, 0.0f)).x;
    return float4(DDViewZ(device, DDL_Proj.x, DDL_Proj.y), 0.0f, 0.0f, 1.0f);
}
