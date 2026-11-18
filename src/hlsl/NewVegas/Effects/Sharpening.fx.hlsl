// Sharpening: AMD FidelityFX Contrast Adaptive Sharpening (CAS), the non-scaling filter with CAS_BETTER_DIAGONALS, plus the mod's
// depth falloff (near surfaces sharpened, far land and sky left alone). Vulkan twin: Compute/Sharpening.frag (keep the two equal).
// UNOFFICIAL 2026-11-04: replaces the old "contrast adaptive" box-average shader.
//
// CAS: a 3x3 neighbourhood; the soft minimum and maximum of the cross (b d e f h) and of the full 3x3 give how much headroom the
// pixel has before it would clip ("amp"); the four cross neighbours get a negative weight w = amp * peak, so low-contrast detail is
// sharpened more and edges that are already strong are left nearly alone (no halos, no ringing). One weight for all three channels
// (green's, as AMD's default path: no hue shifts).
//
// Ported from ffx_cas.h (FidelityFX CAS, https://github.com/GPUOpen-Effects/FidelityFX-CAS):
//   Copyright (c) 2017-2019 Advanced Micro Devices, Inc. All rights reserved.
//   Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation
//   files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy,
//   modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software
//   is furnished to do so, subject to the following conditions:
//   The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
//   OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
//   LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
//   CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#define falloffExponent 4   // the depth falloff's curve (1 at the camera, 0 at Offset x 1000 units and beyond)

float4 TESR_SharpeningData; // x Strength = CAS sharpness (0..1), y Clamp (unused), z Offset (falloff distance / 1000)
float4 TESR_ReciprocalResolution;

sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_DepthBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };

#include "Includes/Helpers.hlsl"
#include "Includes/Depth.hlsl"

struct VSOUT
{
    float4 vertPos : POSITION;
    float2 UVCoord : TEXCOORD0;
};

struct VSIN
{
    float4 vertPos : POSITION0;
    float2 UVCoord : TEXCOORD0;
};

VSOUT FrameVS(VSIN IN)
{
    VSOUT OUT = (VSOUT) 0.0f;
    OUT.vertPos = IN.vertPos;
    OUT.UVCoord = IN.UVCoord;
    return (OUT);
}

float3 CasTap(float2 uv, float2 offset)
{
    return tex2Dlod(TESR_RenderedBuffer, float4(uv + offset * TESR_ReciprocalResolution.xy, 0.0, 0.0)).rgb;
}

float4 ContrastAdaptiveSharpening(VSOUT IN) : COLOR
{
    const float2 uv = IN.UVCoord;
    float4 color = tex2Dlod(TESR_RenderedBuffer, float4(uv, 0.0, 0.0));

    // depth falloff (Offset <= 0: no sharpening)
    const float maxDepth = TESR_SharpeningData.z * 1000.0;
    if (maxDepth <= 0.0) return color;
    const float falloff = pow(saturate((maxDepth - readDepthLod(uv)) / maxDepth), falloffExponent);
    if (falloff <= 0.0) return color;

    //  a b c
    //  d e f
    //  g h i
    const float3 a = CasTap(uv, float2(-1.0, -1.0));
    const float3 b = CasTap(uv, float2( 0.0, -1.0));
    const float3 c = CasTap(uv, float2( 1.0, -1.0));
    const float3 d = CasTap(uv, float2(-1.0,  0.0));
    const float3 e = color.rgb;
    const float3 f = CasTap(uv, float2( 1.0,  0.0));
    const float3 g = CasTap(uv, float2(-1.0,  1.0));
    const float3 h = CasTap(uv, float2( 0.0,  1.0));
    const float3 i = CasTap(uv, float2( 1.0,  1.0));

    // soft min and max: the cross's plus the full 3x3's (CAS_BETTER_DIAGONALS), so each lies in 0..2
    float3 mn = min(min(min(d, e), min(f, b)), h);
    float3 mx = max(max(max(d, e), max(f, b)), h);
    mn += min(min(min(mn, a), min(c, g)), i);
    mx += max(max(max(mx, a), max(c, g)), i);

    // headroom to the signal limits over the max, then shaped (sqrt); green's for all channels (AMD's default)
    const float amp = sqrt(saturate(min(mn.g, 2.0 - mx.g) / max(mx.g, 1.0e-5)));
    const float peak = -1.0 / lerp(8.0, 5.0, saturate(TESR_SharpeningData.x));
    const float w = amp * peak;
    const float3 sharpened = saturate(((b + d + f + h) * w + e) / (1.0 + 4.0 * w));

    color.rgb = lerp(color.rgb, sharpened, falloff);
    return color;
}

technique
{
    pass
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader = compile ps_3_0 ContrastAdaptiveSharpening();
    }
}
