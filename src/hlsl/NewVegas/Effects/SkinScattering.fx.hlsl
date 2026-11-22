// UNOFFICIAL 2026-10-15: CREDIT Conor (mrfury97): his screen-space skin scattering, from his NVR testbed
// (github.com/mrfury97/Conor-sClaudeHell-NVR-SAL-Fork, branch testbed, Effects/SkinScattering.fx.hlsl), unchanged but for this
// header and the early exit marked Port. Runs only with [Shaders.Skin.NewSkin] Enabled (src/effects/SkinScattering.h).
// Re-synced 2026-10-19 with his 00e81af5 (2026-10-16): Community Shaders' 21-sample kernel over +-3, the kernel turned per
// pixel with TAA, and CS's depth falloff (x 1 / tan(fov / 2)).
//
// Screen-space subsurface scattering for skin.
//
// Uses Separable SSS. Copyright (C) 2012 by Jorge Jimenez and Diego Gutierrez. Adapted from
// Community Shaders' SeparableSSS.hlsli and SSSCommon.hlsli (GPL-3.0-or-later) for D3D9 pixel
// shaders; see src/effects/SkinScattering.h for how skin is found without a G-buffer.
//
// Written by the skin shaders (SkinTemplate.hlsl) during their own draws:
//   TESR_SkinScatterBuffer  rgb  the highlights' share of the skin colour (gamma, fogged)
//                           a    the camera distance; a pixel is skin only where this matches
//                                the depth buffer, so anything drawn over the skin is left alone
//   TESR_SkinAlbedoBuffer   rgb  the skin's albedo (gamma)
//                           a    the luminance of the colour the skin drew; a pixel is skin only
//                                where the scene still has it, so hair cards, decals and anything
//                                else drawn over the skin are left alone even without depth.
//                                The effect runs first in the chain so the scene is still as drawn.
//
// What diffuses is light, not the texture: each pixel's colour has its highlights taken out, is
// brought to linear, and divided by albedo^AlbedoDetail; the blurred light is multiplied back by
// the same factor (Community Shaders' pre/post scatter). Dark texels (brows, stubble, lashes)
// would blow up when divided, so they take no part, smoothly, and keep their own colour.
//
// Pass 1 blurs horizontally and writes the blurred light for skin pixels; pass 2 blurs that
// vertically, multiplies the albedo back and adds the highlights. Other pixels pass through.

#define SAMPLES 21   // SkinScatteringEffect::KernelSamples (Community Shaders' SSSS_N_SAMPLES)

uniform float4 TESR_SkinScatterData;                 // x game units per kernel unit, y depth follow, z distance tolerance, w albedo detail
float4 TESR_SkinScatterKernel[SAMPLES];      // rgb weight, a offset; [0] is the centre
uniform float4 TESR_ReciprocalResolution;             // xy 1 / screen size
uniform float4 TESR_SkinScatterDebug;                // x [Shaders.Skin.Debug] DebugView 8-10 as 1-3: the skin test, scene / drawn brightness, what the blur changed;
                                             // y 1 to turn the kernel per pixel (TAA on), z this frame's turn (0-63)

uniform sampler2D TESR_RenderedBuffer : register(s0) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
uniform sampler2D TESR_DepthBuffer : register(s1) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
uniform sampler2D TESR_SkinScatterBuffer : register(s2) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
uniform sampler2D TESR_SkinAlbedoBuffer : register(s3) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
uniform sampler2D TESR_SourceBuffer : register(s4) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };

#include "includes/Helpers.hlsl"
#include "includes/Depth.hlsl"

struct VSOUT { float4 vertPos : POSITION; float2 UVCoord : TEXCOORD0; };

struct VSIN { float4 vertPos : POSITION0; float2 UVCoord : TEXCOORD0; };

VSOUT FrameVS(VSIN px) {
    VSOUT res = (VSOUT)0.0f;
    res.vertPos = px.vertPos;
    res.UVCoord = px.UVCoord;
    return res;
}

// The colour test: the scene about as bright as the skin's own pass left it, or brighter by up to
// 60%. Later additive light passes on the same skin only brighten it (and the blur handles them:
// their highlights accumulate in the skin target too); hair, brows, decals and blood drawn over
// the skin darken it, or, where sunlit hair lies over darker skin, brighten it far more.
bool ColourStillSkin(float scene_luma, float drawn_luma) {
    return scene_luma > drawn_luma * 0.97f - 0.01f && scene_luma < drawn_luma * 1.6f + 0.02f;
}

// The distance test: the scene's depth is the skin's, or something sits just in front of it
// (within 1.5 units, about 2 cm). The second case is FaceGen's brow strip, a mostly transparent
// mesh a few millimetres over the forehead that writes depth everywhere; the colour test then
// keeps its transparent part (still skin coloured) and drops the brow hairs. Anything further in
// front (gear, hands) fails here, whatever its colour.
bool DistanceStillSkin(float skin_distance, float scene_distance) {
    float gap_v = skin_distance - scene_distance;
    return abs(gap_v) < scene_distance * TESR_SkinScatterData.z + 0.02f || (gap_v > 0.0f && gap_v < 1.5f);
}

// UNOFFICIAL 2026-10-19 (deep/skin-highlights): the highlights test. Every highlight pass ADDS the same colour to the scene and
// to the skin target's share (the skin shader's own, and the game's own highlight passes on skin, which no longer are muted
// while the blur runs: SkinScatteringEffect::BindForGameHighlights), so visible skin holds at least that share in every
// channel. Where hair lies over the scalp's highlights (close enough for the distance test, about as bright as the skin) a
// channel falls well below the share (brown hair over a white highlight): covered skin, left alone. Without this test those
// hair pixels took the share out and back in as if it were all there, and the blur's light came on top: a blue-white tint on
// the hair (in game: ~600 px, +21/255 blue at the top of the head). A quarter of the share is allowed below, for fog, which
// the game's fog pass lays over the scene but not over the share.
bool HighlightsStillThere(float3 share_v, float3 scene_color) {
    return all(share_v <= scene_color + share_v * 0.25f + 0.02f);
}

// Skin where all tests pass.
bool IsSkin(float4 scatter_v, float drawn_luma, float3 scene_color, float view_depth, float2 uv) {
    float flat_dist = view_depth * length(toWorld(uv));
    return scatter_v.a > 0.0f
        && DistanceStillSkin(scatter_v.a, flat_dist)
        && ColourStillSkin(luma(scene_color), drawn_luma)
        && HighlightsStillThere(scatter_v.rgb, scene_color);
}

// The scene is gamma encoded (the skin shaders encode with sqrt); the blur works on light.
float3 Decode(float3 c) { return c * c; }
float3 Encode(float3 c) { return sqrt(max(c, 0.0f)); }

// Dark texels (painted brows, stubble, lashes; linear albedo under about 0.04, gamma 0.2) take no
// part: dividing by them turns any error into a bright smear.
float3 Participation(float3 base_rgb) { return smoothstep(0.01f, 0.04f, base_rgb); }
float3 AlbedoFactor(float3 base_rgb) { return pow(max(base_rgb, 1e-4f), TESR_SkinScatterData.w); }

// Light per unit (albedo ^ detail), the quantity that diffuses.
float3 ToLight(float3 color, float3 highlights_v, float3 albedo_gamma) {
    float3 base_rgb = Decode(albedo_gamma);
    float3 light_v = Decode(max(color - highlights_v, 0.0f)) * Participation(base_rgb) / AlbedoFactor(base_rgb);
    return min(light_v, 16.0f);   // a stray pixel can never flood its neighbours
}

// Pass 1's output can exceed 1; stored compressed, in case the target it is drawn into is 8 bit.
float3 Pack(float3 x) { return x / (1.0f + x); }
float3 Unpack(float3 x) { return x / max(1.0f - x, 1e-4f); }

float4 SkinBlur(float2 uv, float2 dir, bool first_pass) {
    float4 color_m = tex2Dlod(TESR_RenderedBuffer, float4(uv, 0.0f, 0.0f));
    float4 scatter_m = tex2Dlod(TESR_SkinScatterBuffer, float4(uv, 0.0f, 0.0f));
    // Port (2026-10-15): no skin drawn here (most of the screen): nothing else to read. Same result as the full test below,
    // which also starts with scatter.a > 0.
    [branch] if (!(scatter_m.a > 0.0f)) return color_m;
    float4 albedo_m = tex2Dlod(TESR_SkinAlbedoBuffer, float4(uv, 0.0f, 0.0f));
    float depth_m = tex2Dlod(TESR_DepthBuffer, float4(uv, 0.0f, 0.0f)).x * farZ;
    // The scene as drawn: pass 1 reads it directly, pass 2 from the copy taken before pass 1.
    float4 scene_m = first_pass ? color_m : tex2Dlod(TESR_SourceBuffer, float4(uv, 0.0f, 0.0f));

    // Debug views (pass 2 only).
    //   1: green skin, red distance mismatch, blue colour mismatch
    //   2: the scene's brightness over the skin pass's own: green equal, red brighter, blue
    //      darker, full colour at 20% either way
    //   3: (at the end) how much the blur changed each pixel
    if (!first_pass && TESR_SkinScatterDebug.x > 0.0f && TESR_SkinScatterDebug.x < 2.5f && scatter_m.a > 0.0f) {
        float3 tint_rgb;
        if (TESR_SkinScatterDebug.x > 1.5f) {
            float ratio_v = luma(scene_m.rgb) / max(albedo_m.a, 1e-3f);
            tint_rgb = ratio_v < 1.0f ? lerp(float3(0.0f, 1.0f, 0.0f), float3(0.0f, 0.2f, 1.0f), saturate((1.0f - ratio_v) * 5.0f))
                                : lerp(float3(0.0f, 1.0f, 0.0f), float3(1.0f, 0.0f, 0.0f), saturate((ratio_v - 1.0f) * 5.0f));
        }
        else {
            float flat_dist = depth_m * length(toWorld(uv));
            bool distance_ok = DistanceStillSkin(scatter_m.a, flat_dist);
            bool colour_ok = ColourStillSkin(luma(scene_m.rgb), albedo_m.a) && HighlightsStillThere(scatter_m.rgb, scene_m.rgb);
            tint_rgb = !distance_ok ? float3(1.0f, 0.0f, 0.0f) : (!colour_ok ? float3(0.0f, 0.2f, 1.0f) : float3(0.0f, 1.0f, 0.0f));
        }
        return float4(lerp(scene_m.rgb, tint_rgb, 0.6f), scene_m.a);
    }

    if (!IsSkin(scatter_m, albedo_m.a, scene_m.rgb, depth_m, uv)) return color_m;

    float3 light_m = first_pass ? ToLight(color_m.rgb, scatter_m.rgb, albedo_m.rgb) : Unpack(color_m.rgb);

    // As Community Shaders, the kernel turned by a random angle per pixel (interleaved gradient noise,
    // moving each frame), the same in both passes so they stay at right angles: no axis-aligned rows
    // of taps, only noise for TAA to average. Off without TAA.
    [flatten] if (TESR_SkinScatterDebug.y > 0.5f) {
        float2 pixel_v = floor(uv / TESR_ReciprocalResolution.xy) + 5.588238f * TESR_SkinScatterDebug.z;
        float angle_v = frac(52.9829189f * frac(dot(pixel_v, float2(0.06711056f, 0.00583715f)))) * 6.2831853f;
        float c = cos(angle_v), s = sin(angle_v);
        dir = float2(dir.x * c - dir.y * s, dir.x * s + dir.y * c);
    }

    // One kernel unit on screen: the projection of TESR_SkinScatterData.x game units at this depth
    // (the same number of pixels across and down, so the turned kernel stays round).
    float2 step_uv = dir * float2(TESR_ProjectionTransform[0][0], TESR_ProjectionTransform[1][1]) * (0.5f * TESR_SkinScatterData.x / max(depth_m, 1.0f));
    // UNOFFICIAL 2026-11-18 next-111 (RenderScale below 1, effects in the rect: Includes/RenderRect.hlsl): the step above is a share of the
    // screen; in rect coordinates it is that share of the drawn part. 1 at RenderScale 1.
    step_uv *= TESR_RenderScale.xy;

    float3 blurred_v = light_m * TESR_SkinScatterKernel[0].rgb;
    [unroll]
    for (int i = 1; i < SAMPLES; i++) {
        float4 sample_uv = float4(clampToRectIfOn(uv + TESR_SkinScatterKernel[i].a * step_uv, TESR_ReciprocalResolution.xy), 0.0f, 0.0f);	// (next-111: never past the drawn part)
        float4 color = tex2Dlod(TESR_RenderedBuffer, sample_uv);
        float4 scatter_v = tex2Dlod(TESR_SkinScatterBuffer, sample_uv);
        float4 base_rgb = tex2Dlod(TESR_SkinAlbedoBuffer, sample_uv);
        float eye_range = tex2Dlod(TESR_DepthBuffer, sample_uv).x * farZ;
        float3 scene_v = first_pass ? color.rgb : tex2Dlod(TESR_SourceBuffer, sample_uv).rgb;

        float3 light_v = first_pass ? ToLight(color.rgb, scatter_v.rgb, base_rgb.rgb) : Unpack(color.rgb);

        // Light does not scatter across a depth jump (Jimenez: lerp back to the centre), nor
        // in from anything that is not skin.
        float follow_v = saturate(TESR_SkinScatterData.y * TESR_ProjectionTransform[1][1] * abs(depth_m - eye_range));   // x 1 / tan(fov / 2), as CS
        light_v = lerp(light_v, light_m, follow_v * follow_v);
        light_v = IsSkin(scatter_v, base_rgb.a, scene_v, eye_range, sample_uv.xy) ? light_v : light_m;

        blurred_v += TESR_SkinScatterKernel[i].rgb * light_v;
    }

    if (first_pass) return float4(Pack(blurred_v), color_m.a);

    // Back to colour: the blurred light times the albedo factor, the original where the texture
    // took no part (an exact identity without blur), then the highlights.
    float4 original_v = scene_m;
    float3 base_rgb = Decode(albedo_m.rgb);
    float3 participation_v = Participation(base_rgb);
    // Port 2026-10-19: never more highlights back than the pixel held (the share can exceed it under fog; same light term).
    float3 highlights_v = min(scatter_m.rgb, original_v.rgb);
    float3 light_v = blurred_v * AlbedoFactor(base_rgb) * participation_v
                 + Decode(original_v.rgb - highlights_v) * (1.0f - participation_v * participation_v);
    float3 result_c = Encode(light_v) + highlights_v;

    // View 3, on skin (everything else is left as it is): the change, x8, over the dimmed scene:
    // dark where the blur did nothing, bright where it moved the colour by an eighth or more. Red
    // where it brightened, blue where it darkened.
    if (TESR_SkinScatterDebug.x > 2.5f) {
        float change_v = luma(result_c) - luma(original_v.rgb);
        float3 tint_rgb = change_v > 0.0f ? float3(1.0f, 0.35f, 0.25f) : float3(0.25f, 0.45f, 1.0f);
        float amount_v = saturate(length(result_c - original_v.rgb) * 8.0f);
        return float4(luma(original_v.rgb) * 0.25f + tint_rgb * amount_v, original_v.a);
    }
    return float4(result_c, original_v.a);
}

float4 BlurHorizontal(VSOUT px) : COLOR0 {
    return SkinBlur(px.UVCoord, float2(1.0f, 0.0f), true);
}

float4 BlurVertical(VSOUT px) : COLOR0 {
    return SkinBlur(px.UVCoord, float2(0.0f, 1.0f), false);
}

technique {
    pass {
        VertexShader = compile vs_3_0 FrameVS( );
        PixelShader = compile ps_3_0 BlurHorizontal( );
    }
    pass {
        VertexShader = compile vs_3_0 FrameVS( );
        PixelShader = compile ps_3_0 BlurVertical( );
    }
}
