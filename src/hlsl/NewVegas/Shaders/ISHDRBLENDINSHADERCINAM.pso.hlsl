// Tonemap & apply bloom/cinematic filter from weather for scene with alpha mask
//#define	ScreenSpace	Src0
uniform sampler2D Src0 : register(s0);
uniform sampler2D DestBlend : register(s1);
uniform sampler2D TESR_BloomBuffer : register(s8) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };

uniform float4 HDRParam : register(c1);
uniform float3 BlurScale : register(c2);
uniform float4 Cinematic : register(c19);
uniform float4 Tint : register(c20);
uniform float4 Fade : register(c22);
uniform float4 UseAlphaMask : register(c23);
uniform float4 TESR_DebugVar : register(c24);
uniform float4 TESR_HDRBloomData : register(c25);
//float4 TESR_SunAmount : register(c26);
uniform float4 TESR_HDRData : register(c27);
uniform float4 TESR_LotteData : register(c28);
uniform float4 TESR_ToneMapping : register(c29);
uniform float4 TESR_BloomExtraData : register(c30); // .x - NVR bloom on/off.
uniform float4 TESR_ReciprocalResolution : register(c31); //
uniform float4 TESR_ToneMappingClip : register(c32); // UNOFFICIAL .x: 1 = HueSafeClip (2026-10-30, as ISHDRBLENDINSHADERCIN), y: dither size (0 off), z: dither frame (sky dither at the tonemap)

#include "includes/Helpers.hlsl"
#include "includes/Tonemapping.hlsl"

struct VtxAttrs {
    float2 f_screen_offset : TEXCOORD0;
    float2 f_texcoord_1 : TEXCOORD1;
    float2 f_pos : VPOS;
};

struct VtxVaryings {
    float4 f_color_0 : COLOR0;
};

static const float3 box_taps[5] = {
	float3( 0,  0, 1),
	float3(-1, -1, 1),
	float3( 1, -1, 1),
	float3(-1,  1, 1),
	float3( 1,  1, 1)
};

static const float cine_k = TESR_HDRData.z;

float4 sampleBox(sampler2D buf, float2 tex_uv, float ofs){
    float4 pix = float4(0, 0, 0, 0);
    float total_f = 0;

    for (int k = 0; k < 5; k ++){
        float2 crd = tex_uv + TESR_ReciprocalResolution.xy * box_taps[k].xy * ofs;
        float2 safety_f = TESR_ReciprocalResolution.xy * float2(0.5, 0.5);
        float is_valid = (crd.x > safety_f.x && crd.x < 1 - safety_f.x && crd.y > safety_f.y && crd.y < 1 - safety_f.y) * box_taps[k].z;
        pix += tex2D(buf, crd) * is_valid;
        total_f += is_valid;
    }
    pix /= total_f;

    return pix;
}

VtxVaryings main(VtxAttrs pa) {
    VtxVaryings pr;
    // Whether we linearize before applying most post process effects that were already present in the vanilla game.
    // If we do, it will look less like vanilla; sometimes it will look better, some other times it will look worse (tint and fade especially make a big difference);
    // generally giving a darker and less saturated look in linear space.
    bool gamma_space_post_process = (TESR_ToneMapping.w < 0 || TESR_HDRData.w < 0); // apply if gamma or linearization settings are below 0
    //TODO: TESR_ToneMapping.w isn't used anymore (it was in gamma), TESR_HDRData.w isn't used anymore (it was out gamma)

    float4 final_c = tex2D(DestBlend, pa.f_texcoord_1.xy);
    float3 tint_rgb = Tint.rgb;
    float3 fade_f = Fade.rgb;

    if (!gamma_space_post_process){
        // linearize
        final_c.rgb = pows(final_c.rgb, TESR_ToneMapping.w);
        tint_rgb = pows(tint_rgb, TESR_ToneMapping.w);
        fade_f = pows(fade_f, TESR_ToneMapping.w);
    }

    final_c.rgb = lerp(luma(final_c.rgb).xxx, final_c.rgb, Cinematic.x * cine_k); // saturation
    final_c.rgb = lerp(final_c.rgb, tint_rgb * luma(final_c.rgb), saturate(Tint.a * TESR_ToneMapping.z)); // apply tint
    
    if (TESR_BloomExtraData.x)
    {
        // NVR bloom
        float4 nvrbloom = linearize(tex2D(TESR_BloomBuffer, pa.f_texcoord_1.xy)); // already linear

        if (gamma_space_post_process){ // Always do the new bloom in linear space as it's designed for that
            final_c.rgb = linearize(final_c.rgb);
        }
        final_c.rgb = lerp(final_c.rgb, nvrbloom.rgb * TESR_HDRBloomData.y, saturate(TESR_HDRBloomData.x));
    }else{
        // vanilla bloom
        // scale bloom while maintaining color
        float4 bloom_c = tex2D(Src0, pa.f_screen_offset.xy);
        //if (!gammaSpacePostProcess){
        //    // linearize bloom
        //    bloom.rgb = pows(bloom.rgb, 1/TESR_ToneMapping.w);
        //}
        bloom_c.rgb = TESR_HDRBloomData.x * pows(bloom_c.rgb, TESR_HDRBloomData.y);

        float tmp_q0 = 1.0 / max(bloom_c.w, HDRParam.x); // HDRParam.x is brights cutoff (clamp)
        final_c.rgb = ((tmp_q0 * HDRParam.x) * final_c.rgb) + bloom_c.rgb * (tmp_q0 * 0.5); // blend image and bloom
        if (gamma_space_post_process){
            final_c.rgb = linearize(final_c.rgb);
        }
    }
    
    final_c.rgb = lerp(final_c.rgb, final_c.rgb * Cinematic.w, cine_k); // apply brightness from Cinematic
    // exposure, local exposure and the night colour shift (2026-11-04, both off by default) & tonemap
    final_c.rgb = tonemap(nightShift(localExposure(final_c.rgb * TESR_HDRData.y, pa.f_texcoord_1.xy, TESR_HDRData.y)));
    // UNOFFICIAL 2026-10-30 HueSafeClip here too (as ISHDRBLENDINSHADERCIN): this alpha-mask variant tonemaps the frames with a
    // menu over the world (the Pip-Boy), and it cut a too-bright channel where the world's keeps the hue, so the Pip-Boy's lit
    // amber and orange came out yellower than the same colours in the world. The setting's description says it applies everywhere.
    [branch] if (TESR_ToneMappingClip.x > 0.5f) {
        const float peak_f = max(final_c.r, max(final_c.g, final_c.b));
        [branch] if (peak_f > 1.0f) {
            const float loc_brightness = saturate(luma(final_c.rgb));
            final_c.rgb = loc_brightness + (final_c.rgb - loc_brightness) * saturate((1.0f - loc_brightness) / max(peak_f - loc_brightness, 1e-4f));
        }
    }

    if (gamma_space_post_process){
        final_c.rgb = delinearize(final_c.rgb);
    }
    
    final_c.rgb = lerp(final_c.rgb, (Cinematic.z * (final_c.rgb - Cinematic.y)) + Cinematic.y, cine_k * TESR_ToneMapping.y); // apply contrast from Cinematic, scaled by modifier
    final_c.rgb = lerp(final_c.rgb, fade_f, Fade.a); // apply night eye and fade

    if (!gamma_space_post_process){
        final_c.rgb = pows(final_c.rgb, 1/TESR_HDRData.w); // delinearize
    }

    // UNOFFICIAL sky dither at the tonemap ([Shaders.SkyDither.Main] AtToneMap): HERE the HDR picture is rounded to the 8-bit
    // target, and the sky's smooth gradients turn into bands of one level each. NVR's later effects and its end-of-chain dither
    // only see those steps (measured: the same staircase with the end dither on and off). A triangular noise of about one level
    // added before this rounding lets each pixel round up or down in proportion to where the true value lies, so the gradient
    // survives as a fine grain the eye averages away. A new pattern every frame; it fades out below half a level so black stays black.
    [branch] if (TESR_ToneMappingClip.y > 0.0f) {
        const float2 pt = pa.f_pos + TESR_ToneMappingClip.z * float2(5.588238f, 3.235f);
        const float nn = frac(52.9829189f * frac(dot(pt, float2(0.06711056f, 0.00583715f)))) +
            // UNOFFICIAL 2026-11-09 (review 6.6.1): the second read is taken at the pixel's SWAPPED coordinates. At a constant offset it was fract(first + 0.427 or 0.444) for every pixel (the noise is linear in the pixel), so the 'triangular' sum was one number counted twice: uneven grain. Every copy of this noise changes together (SkyDither, the tonemap, Cinema, and the SMAA / DitherBuster passes that take it off again).
            frac(52.9829189f * frac(dot(pt.yx + float2(47.0f, 17.0f), float2(0.06711056f, 0.00583715f)))) - 1.0f;
        final_c.rgb += nn * TESR_ToneMappingClip.y * saturate(final_c.rgb * 128.0f);
    }

    float4 bg_c = tex2D(DestBlend, pa.f_screen_offset.xy); // sdr image (already tonemapped) displayed within the mask
    pr.f_color_0 = float4(bg_c.w == 0 ? bg_c.rgb : final_c.rgb, BlurScale.z);

    return pr;
};

