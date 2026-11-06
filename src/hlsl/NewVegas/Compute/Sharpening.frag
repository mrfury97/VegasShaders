// Sharpening as a Vulkan pass (needs NVR DXVK): AMD FidelityFX CAS + the depth falloff, Effects/Sharpening.fx.hlsl line for line
// (keep the two equal). effects/Sharpening.cpp ExtRender. readDepth: linear depth / farZ in TESR_DepthBuffer's first channel, times farZ.
// UNOFFICIAL 2026-11-04: replaces the old box-average sharpening.
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
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer
layout(set = 1, binding = 1) uniform texture2D t_depth;      // TESR_DepthBuffer

layout(push_constant) uniform Push {
    vec4 sharpeningData;        // 0: TESR_SharpeningData (x Strength = CAS sharpness 0..1, y Clamp unused, z Offset / depth falloff)
    vec4 reciprocalResolution;  // 16: TESR_ReciprocalResolution
    vec4 cameraData;            // 32: TESR_CameraData (y farZ)
    uint samplerIndex;          // 48: LINEAR, CLAMP (the colour taps land on texel centres: exact)
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

const float falloffExponent = 4.0;

float readDepth(vec2 coord) {
    return textureLod(sampler2D(t_depth, NVR_SAMPLER(pc.samplerIndex)), coord, 0.0).x * pc.cameraData.y;
}

vec3 casTap(vec2 offset) {
    return textureLod(sampler2D(t_rendered, NVR_SAMPLER(pc.samplerIndex)), uv + offset * pc.reciprocalResolution.xy, 0.0).rgb;
}

void main() {
    vec4 color = textureLod(sampler2D(t_rendered, NVR_SAMPLER(pc.samplerIndex)), uv, 0.0);
    o_color = color;

    // depth falloff (Offset <= 0: no sharpening)
    const float maxDepth = pc.sharpeningData.z * 1000.0;
    if (maxDepth <= 0.0) return;
    const float falloff = pow(saturate((maxDepth - readDepth(uv)) / maxDepth), falloffExponent);
    if (falloff <= 0.0) return;

    //  a b c
    //  d e f
    //  g h i
    const vec3 a = casTap(vec2(-1.0, -1.0));
    const vec3 b = casTap(vec2( 0.0, -1.0));
    const vec3 c = casTap(vec2( 1.0, -1.0));
    const vec3 d = casTap(vec2(-1.0,  0.0));
    const vec3 e = color.rgb;
    const vec3 f = casTap(vec2( 1.0,  0.0));
    const vec3 g = casTap(vec2(-1.0,  1.0));
    const vec3 h = casTap(vec2( 0.0,  1.0));
    const vec3 i = casTap(vec2( 1.0,  1.0));

    // soft min and max: the cross's plus the full 3x3's (CAS_BETTER_DIAGONALS), so each lies in 0..2
    vec3 mn = min(min(min(d, e), min(f, b)), h);
    vec3 mx = max(max(max(d, e), max(f, b)), h);
    mn += min(min(min(mn, a), min(c, g)), i);
    mx += max(max(max(mx, a), max(c, g)), i);

    // headroom to the signal limits over the max, then shaped (sqrt); green's for all channels (AMD's default)
    const float amp = sqrt(saturate(min(mn.g, 2.0 - mx.g) / max(mx.g, 1.0e-5)));
    const float peak = -1.0 / mix(8.0, 5.0, saturate(pc.sharpeningData.x));
    const float w = amp * peak;
    const vec3 sharpened = saturate(((b + d + f + h) * w + e) / (1.0 + 4.0 * w));

    o_color = vec4(mix(color.rgb, sharpened, falloff), color.a);
}
