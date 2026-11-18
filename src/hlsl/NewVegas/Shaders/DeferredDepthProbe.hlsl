// UNOFFICIAL 2026-11-14 DeferredDepth probe (core/DeferredDepth.h Probe; test only, [Main.Main.Quality] DeferredDepthProbe): a 64 x 64
// grid of the screen read at the main opaque list's end, through the shared helper (Includes/DeferredDepth.hlsl), into two float targets
// that the C++ reads back and compares per distance band with the G-buffer's own camera distance (COLOR3.b, a half float).
//   COLOR0: r the camera distance from TESR_DeferredDepth (this frame's copy), g COLOR3.b, b the raw device depth read, a COLOR3.a
//   COLOR1: r the camera distance from TESR_DepthBufferWorld (the world's depth as the LAST frame's world scene left it: the texture a
//           D3D9 pass read as 0 under the DXVK fork, ReactiveMask.h), g its raw device depth, ba the screen pixel the texel read
//   COLOR2: r the camera distance from TESR_DeferredDepthLinear (mode 2: view z, R32F), g that view z, ba 0
// Loaded by ShaderRecord::LoadShader at startup when the probe is on (never on the render thread).

#include "Includes/DeferredDepth.hlsl"

float4 DD[6]      : register(c0);	// TESR_DeferredDepthFrame (Includes/DeferredDepth.hlsl)
float4 DDP_Map    : register(c6);	// xy the viewport's size / 64, zw its top-left corner (pixels)
float4 DDP_Size   : register(c7);	// xy 1 / the G-buffer's size

sampler2D DDP_Depth   : register(s0);	// TESR_DeferredDepth (INTZ)
sampler2D DDP_Normals : register(s1);	// TESR_CapturedNormalsBuffer: b the camera distance, a 1 written (>= 1.5 actor)
sampler2D DDP_World   : register(s2);	// TESR_DepthBufferWorld (INTZ, last frame's world resolve)
sampler2D DDP_Linear  : register(s3);	// TESR_DeferredDepthLinear (R32F view z; mode 2)

struct PS_OUT { float4 c0 : COLOR0; float4 c1 : COLOR1; float4 c2 : COLOR2; };

PS_OUT main(float2 vpos : VPOS) {
    // the screen pixel this probe texel stands for (the centre of its 1/64 cell of the viewport)
    const float2 pixel = floor((vpos + 0.5f) * DDP_Map.xy) + DDP_Map.zw;
    const float device = DD_DEVICE_DEPTH(DD, DDP_Depth, pixel);
    const float4 stored = tex2Dlod(DDP_Normals, float4((pixel + 0.5f) * DDP_Size.xy, 0.0f, 0.0f));
    const float world = DD_DEVICE_DEPTH(DD, DDP_World, pixel);
    const float viewZ = DD_LINEAR_VIEW_Z(DD, DDP_Linear, pixel);
    PS_OUT o;
    o.c0 = float4(DD_CAMERA_DISTANCE(DD, pixel, device), stored.b, device, stored.a);
    o.c1 = float4(DD_CAMERA_DISTANCE(DD, pixel, world), world, pixel);
    o.c2 = float4(length(DD_CAMERA_RELATIVE_Z(DD, pixel, viewZ)), viewZ, 0.0f, 0.0f);
    return o;
}
