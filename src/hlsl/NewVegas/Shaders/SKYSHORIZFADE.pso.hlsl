// Night sky

uniform sampler2D TexMap : register(s0);

uniform float4 TESR_GameTime;
uniform float4 TESR_DebugVar;
uniform float4 TESR_SkyData;
uniform float4 TESR_ReciprocalResolution;
uniform float4 TESR_CloudData;

struct VtxAttrs {
    float2 f_tex_uv : TEXCOORD0;
    float2 f_position : VPOS;
    float f_texcoord_2 : TEXCOORD2;
    float3 f_location : TEXCOORD1;
    float4 f_color_0 : COLOR0;
};

struct VtxVaryings {
    float4 f_color_0 : COLOR0;
};

// 3D value noise: a random value in [0, 1) at each whole-number lattice point, blended between the 8 corners of the cell
// with a smoothstep curve. Output in [0, 1), mean 0.5, one lattice cell per unit. Written for Vegas Shaders (2026-11-15);
// it replaces upstream's copy of a Shadertoy noise under CC BY-NC-SA 3.0 (not GPL-compatible), with the same range and scale.
// Lattice hash: Dave Hoskins' "Hash without Sine" hash13, MIT, Copyright (c) 2014 David Hoskins (shadertoy.com/view/4djSRW).
float hash(float3 p3)
{
    p3 = frac(p3 * 0.1031);
    p3 += dot(p3, p3.zyx + 31.32);
    return frac((p3.x + p3.y) * p3.z);
}

float noise(float3 xx)
{
    float3 cell = floor(xx);
    float3 ff = frac(xx);
    ff = ff * ff * (3.0 - 2.0 * ff);
    float z0 = lerp(lerp(hash(cell), hash(cell + float3(1, 0, 0)), ff.x),
                    lerp(hash(cell + float3(0, 1, 0)), hash(cell + float3(1, 1, 0)), ff.x), ff.y);
    float z1 = lerp(lerp(hash(cell + float3(0, 0, 1)), hash(cell + float3(1, 0, 1)), ff.x),
                    lerp(hash(cell + float3(0, 1, 1)), hash(cell + float3(1, 1, 1)), ff.x), ff.y);
    return lerp(z0, z1, ff.z);
}

#include "includes/Helpers.hlsl"
#include "includes/Position.hlsl"

VtxVaryings main(VtxAttrs pa) {
    VtxVaryings pr;

    float4 stars_c = linearize(tex2D(TexMap, pa.f_tex_uv.xy));
    float4 loc_skytint = linearize(pa.f_color_0);

    float star_flicker = 0.05;
    float noise_scale = 4;
    float flicker_speed = 0.1 * TESR_CloudData.y;

    float3 eye_dir = normalize(pa.f_location);
  
    // create animated noise to modulate star brightness
    float stars_luma = smoothstep(0, 1, luma(stars_c.rgb)); // only pick the brightest areas of the sky for the flicker

    float nn = noise(noise_scale * eye_dir);
    nn *= noise((TESR_GameTime.x * flicker_speed).xxx + noise_scale * eye_dir) * 2;

    pr.f_color_0.a = (stars_c.a * loc_skytint.a) * pa.f_texcoord_2.x * TESR_SkyData.w ;//* (n * starsLuma + 1);
    stars_c.rgb = stars_c.rgb * (nn * 100 * stars_luma + 1) * loc_skytint.rgb;
    stars_c.rgb = delinearize(stars_c).rgb; //delinearise
    pr.f_color_0.rgb = stars_c.rgb;
    return pr;
};
