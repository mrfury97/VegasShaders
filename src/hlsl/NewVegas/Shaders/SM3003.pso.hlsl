// Hair pixel shader (SM3003). Diffuse and ambient go through PBR with a forward sun shadow;
// the specular keeps the game's model: Marschner R lobe plus TRT, using Scheuermann-style
// tangent shifts and two AnisoMap taps at (dot(shift,L), dot(shift,H)).
// LightData[8] at c21 holds four (colour, position+radius) pairs. Pair 0 is the directional
// sun when ToggleNumLights.x is set, and it is the one that casts the shadow.

// Overrides the global bias for alpha-tested strands, which self-shadow without an offset. 0 keeps the global value.
#ifndef SHADOW_NORMAL_BIAS_TEXELS
    #define SHADOW_NORMAL_BIAS_TEXELS 2.5f
#endif
#define SHADOW_CLOUD_IN 1   // UNOFFICIAL 2026-11-04 ForwardCloudShadows (Includes/Shadow.hlsl): the clouds' shadow on the sun term (always compiled in; its strength switches it live)
#include "includes/Shadow.hlsl"
#include "includes/Helpers.hlsl"
#include "includes/PBR.hlsl"
#include "includes/PBRScale.hlsl"
#include "includes/PointShadowForward.hlsl"  // UNOFFICIAL forward point-light shadows (INTERIOR_SHADOWS, interiors)
// UNOFFICIAL 2026-11-12 HairAndEyes (Includes/FlashBeam.hlsl): the hair takes the flashlight's beam itself, and marks its pixels so the
// light pass does not light the scalp through it. c4-c12, s6 and s7 are free in this shader and its includes.
#define FLASH_BEAM_REG c4
#define FLASH_BEAM_MATRIX_REG c9
#define FLASH_BEAM_SAMPLER s6
#define FLASH_BEAM_COOKIE_SAMPLER s7
#include "includes/FlashBeam.hlsl"
// The mark is written only where the records exist with their distance (the AlbedoBuffer level, as the light pass needs).
#if defined(CAPTURED_NORMALS_ON) && CAPTURED_NORMALS_ON >= 3
    #define FLASH_BEAM_MARK 1
#else
    #define FLASH_BEAM_MARK 0
#endif

uniform float4 AmbientColor    : register(c0);
uniform float4 EyePosition     : register(c1);
uniform float4 HairTint        : register(c2);
uniform float4 MatAlpha        : register(c3);
uniform float4 ToggleNumLights : register(c20);   // x,y: light counts, w: specular scale
uniform float4 LightData[8]    : register(c21);   // (colour, position+radius) x 4
// UNOFFICIAL 2026-11-01 HairHighlight ([Shaders.PBR.Main] HairHighlight / HairHighlightGameLook, PBR.cpp; user: hair far shinier with PBR
// on, no slider reaches it). With linear lighting the strand highlight was summed in linear light and the total encoded, which made it
// about twice as bright on screen as the game's (which adds it in its stored curve), and the PBR light strength scaled it too.
// x: HairHighlight - 1 (0, also a register never written: 1x), y: 1 = the game's look (the highlight from the game's own light
// colours, added after the encode as the game does; 0 = summed in linear light, as before). c147: free in this shader.
uniform float4 TESR_HairData   : register(c147);

uniform sampler2D BaseMap   : register(s0);
uniform sampler2D NormalMap : register(s1);
uniform sampler2D AnisoMap  : register(s4);
uniform sampler2D LayerMap  : register(s5);

// UNOFFICIAL 2026-11-16 fd-fwdgroups DeferredHair (Includes/HairForwardPlus.hlsl): the forward+ record (HAIR_FORWARD_PLUS 1, made by
// core/DeferredTransparents.h with [Main.Main.Quality] DeferredHair): the game's sun pair kept, its point lamps replaced by every lamp of the
// pixel's froxel. The vertex shader's spare components carry the world tangent and normal. Without the define: as before, byte for byte.
#if defined(HAIR_FORWARD_PLUS) && HAIR_FORWARD_PLUS
    #define HAIR_FP 1
    #define HAIR_FP_V float4	// (the interpolators with the spare components the record reads)
    #define HAIR_GAME_LIGHTS 1	// the sun pair only (light 0)
#else
    #define HAIR_FP 0
    #define HAIR_FP_V float3
    #define HAIR_GAME_LIGHTS 4
#endif

// Only the diffuse lobe uses this roughness; the highlight comes from AnisoMap.
#ifndef HAIR_ROUGHNESS
    #define HAIR_ROUGHNESS 0.6f
#endif
#if HAIR_FP
    #define HAIR_FP_SPEC
    #include "includes/HairForwardPlus.hlsl"
#endif

struct PixIn {
#if HAIR_FP
    float4 f_tex_coord       : TEXCOORD0;   // zw: the world tangent's y z (SM3003.vso with G-buffer v2)
#else
    float2 f_tex_coord       : TEXCOORD0;
#endif
    float4 f_shadow_pos      : TEXCOORD1;
    float4 f_beam_obj        : TEXCOORD2;   // UNOFFICIAL 2026-11-12 xyz to the flashlight, object space (SM3003.vso; undefined under the game's VS)
    float4 f_vtx_color       : COLOR0;
    HAIR_FP_V f_tan_in          : TEXCOORD3_centroid;   // (HAIR_FP: w the world normal's x)
    HAIR_FP_V f_bin_in          : TEXCOORD4_centroid;   // (w its y)
    HAIR_FP_V f_nrm_in          : TEXCOORD5_centroid;   // (w its z)
    HAIR_FP_V f_obj_pos_in       : TEXCOORD6_centroid;  // (w the world tangent's x)
    float4 f_fog_in          : TEXCOORD7_centroid;
#if FLASH_BEAM_MARK
    float2 f_vpos            : VPOS;        // UNOFFICIAL 2026-11-12 the pixel, for the mark's dither
#endif
};

struct PixOut {
    float4 f_color_out : COLOR0;
#if FLASH_BEAM_MARK
    // UNOFFICIAL 2026-11-12 (ps_3_0 wants COLOR0..3 without a gap: 1 and 2 are written as 0 and masked off by core/CapturedNormals.h)
    float4 f_pad1 : COLOR1;
    float4 f_pad2 : COLOR2;
    float4 f_mark : COLOR3;   // the captured normals' target: only its blue (the distance) is written, and only while the beam is on
#endif
};

PixOut main(PixIn src_c) {
    PixOut dst;

    float3 tan_dir = normalize(src_c.f_tan_in.xyz);
    float3 bin_dir = normalize(src_c.f_bin_in.xyz);
    float3 nrm_dir = normalize(src_c.f_nrm_in.xyz);
    float3x3 tbnMat = float3x3(tan_dir, bin_dir, nrm_dir);

    float4 nmap_c = tex2D(NormalMap, src_c.f_tex_coord.xy);
    float3 n_tan = normalize(expand(nmap_c.xyz));

    // Shift vectors for the R (shiftA) and TRT (shiftB) lobes.
    float3 trt_shift = normalize(n_tan * 0.5f + float3(0.0f, 0.0f, 1.0f));
    float3 r_shift = normalize(trt_shift + float3(-0.05f, 0.0f, 0.0f));

    float3 eye_tan = normalize(mul(tbnMat, EyePosition.xyz - src_c.f_obj_pos_in.xyz));

#if HAIR_FP
    // UNOFFICIAL 2026-11-16 fd-fwdgroups DeferredHair: every lamp of the pixel's froxel (Includes/HairForwardPlus.hlsl), each with its atlas
    // shadow, lit as the loop below lights a point lamp; the shadow lookup's normal offset along the view (as the sun's). Summed here, before
    // the textures and the loop (fewer values live across it: X4505 otherwise); added to the loop's sums after it.
    hairFpM = HairFPFrame(tbnMat, src_c.f_tan_in.xyz, src_c.f_nrm_in.xyz, float3(src_c.f_obj_pos_in.w, src_c.f_tex_coord.zw),
        float3(src_c.f_tan_in.w, src_c.f_bin_in.w, src_c.f_nrm_in.w));
    hairFpN = n_tan; hairFpEye = eye_tan;
    hairFpTint = (src_c.f_vtx_color.y * (HairTint.rgb - 0.5f) + 0.5f) * 0.3f + 0.2f;	// the point lamps' tint (point_tint below)
    const float3 fp_diff = FPLampSum(src_c.f_shadow_pos.xyz, normalize(-src_c.f_shadow_pos.xyz), SHADOW_VS_PRESENT(src_c.f_shadow_pos.w) ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
    const float3 fp_spec = hairFpSpec;
#endif

    // LayerMap blends over BaseMap. Vertex colour .y goes into the tint, not into the albedo.
    float4 base_sample  = tex2D(BaseMap,  src_c.f_tex_coord.xy);
    float4 layer_sample = tex2D(LayerMap, src_c.f_tex_coord.xy);
    float3 albedo_mix = lerp(base_sample.rgb, layer_sample.rgb, layer_sample.a);

    //   mid = lerp(0.5, HairTint, vertexColour.y)
    //   sun R-lobe tint   = mid * 2
    //   point R-lobe tint = mid * 0.3 + 0.2, taken from mid before the doubling
    float3 tint_mid = src_c.f_vtx_color.y * (HairTint.rgb - 0.5f) + 0.5f;
    float3 point_tint = tint_mid * 0.3f + 0.2f;  // point lights
    float3 sun_tint   = tint_mid * 2.0f;         // sun, also the albedo tint
    float3 albedo_tinted = albedo_mix * sun_tint;

    // Applied once to the summed highlight. LightData[1].w is the sun's specular strength.
    float spec_gain = nmap_c.w * LightData[1].w * src_c.f_vtx_color.y;

    // Strands are thin and alpha-tested, so ddx/ddy normals from GetShadowGeometricNormal flicker
    // between pixels, and the self-shadow bias pushes the shadow-map lookup along that normal,
    // which turns the flicker into per-pixel light/dark noise. Use the view direction instead: it
    // has no derivatives, still faces the camera, and is enough to stop self-shadow acne and give
    // SkyAmbient a stable hemisphere. worldPos is camera-relative, so -worldPos points at the camera.
    float3 shadow_n = normalize(-src_c.f_shadow_pos.xyz);
#if FORWARD_SHADOWS
    float sun_occ = SHADOW_VS_PRESENT(src_c.f_shadow_pos.w)
                    ? GetSunShadow(src_c.f_shadow_pos.xyz, shadow_n)
                    : 1.0f;
#else
    float sun_occ = 1.0f;
#endif

#if INTERIOR_SHADOWS && !HAIR_FP
    // UNOFFICIAL forward point-light shadows (Includes/PointShadowForward.hlsl). Light i of the draw is LightData pair i;
    // the DLL leaves a directional light 0 without a shadow. Attenuation as in the loop below.
    float3 ptWorldPos = src_c.f_shadow_pos.xyz;
    float3 ptNormal = 0.0f;
    float ptValid = SHADOW_VS_PRESENT(src_c.f_shadow_pos.w) ? 1.0f : 0.0f;
    #define HAIR_PT_ATT(k) saturate(1.0f - sqr(saturate(length(LightData[(k) * 2 + 1].xyz - src_c.f_obj_pos_in.xyz) / max(LightData[(k) * 2 + 1].w, 0.0001f))))
    float3 ptShadow[4] = { (float3)1.0f, (float3)1.0f, (float3)1.0f, (float3)1.0f };
    [branch] if (NVR_PointShadowParams.x > 0.0f) {
        ptShadow[0] = POINT_SHADOW_LOOKUP(0, HAIR_PT_ATT(0));
        ptShadow[1] = POINT_SHADOW_LOOKUP(1, HAIR_PT_ATT(1));
        ptShadow[2] = POINT_SHADOW_LOOKUP(2, HAIR_PT_ATT(2));
        ptShadow[3] = POINT_SHADOW_LOOKUP(3, HAIR_PT_ATT(3));
    }
    #undef HAIR_PT_ATT
#endif

    int n_lights = (int)min(ToggleNumLights.x + ToggleNumLights.y, 4.0f);

    float3 diff_sum = 0.0f;
    float3 spec_sum = 0.0f;
    float3 spec_sum_game = 0.0f;   // UNOFFICIAL HairHighlight: the same lobes with the game's light colours (TESR_HairData.y)

    // .x enables the sun; .y counts point lights 1..3.
    bool sun_on = ToggleNumLights.x > 0.0f;

    // UNOFFICIAL 2026-11-12 HairAndEyes (Includes/FlashBeam.hlsl): while the hair takes the flashlight's beam itself (after the loop),
    // the flashlight's game lamp (core/FlashlightLamp.h: the Pip-Boy light, placed at the flashlight) is left out of the lamps, or the
    // hair would be lit twice: it is the lamp that lies in the beam's own direction from this pixel (within half a degree). The game
    // draws its blended hair in its sorted list, where no per-object hook shapes that lamp: it lit the hair at full strength, with no
    // cone and no shadow, or not at all when it was not among the hair's few lamps.
    const bool beam_on = FLASH_BEAM_ON && SHADOW_VS_PRESENT(src_c.f_shadow_pos.w);
    const float3 beam_obj = src_c.f_beam_obj.xyz * rsqrt(max(dot(src_c.f_beam_obj.xyz, src_c.f_beam_obj.xyz), 1.0e-12f));

    [unroll]
    for (int li_v = 0; li_v < HAIR_GAME_LIGHTS; li_v++) {
        float3 lc_v = PBRLight(LightData[li_v * 2].rgb);
        float3 gc_c = LightData[li_v * 2].rgb;   // UNOFFICIAL HairHighlight: the game's colour (stored curve, no PBR strength)
#if INTERIOR_SHADOWS && !HAIR_FP
        lc_v *= ptShadow[li_v];   // all of this light's terms
        gc_c *= ptShadow[li_v];
#endif
        float4 lv_v = LightData[li_v * 2 + 1];

        bool is_sun = (li_v == 0) && sun_on;

        float3 to_l = is_sun ? lv_v.xyz : (lv_v.xyz - src_c.f_obj_pos_in.xyz);

        // Falloff 1 - (dist/radius)^2. Sun has none.
        float d_len = length(to_l);
        float dn_v = saturate(d_len / max(lv_v.w, 0.0001f));
        float att = is_sun ? 1.0f : LampAttOther(saturate(1.0f - dn_v * dn_v)); // UNOFFICIAL 2026-10-21 LampFalloff (Includes/LampFalloff.hlsl): as before, the objects' decode or the shape

        float3 lv_dir = normalize(mul(tbnMat, to_l));
        // UNOFFICIAL 2026-11-04 (hair black in the flashlight, third person): a lamp inside the head (the flashlight on the head bone, at the
        // hair's origin) sits straight opposite the eye for the hair facing the camera: eye + light is 0 there, and its normalize a NaN that
        // took the whole hair card (colour and blend) black. Then the normal: NL below decides the light as for any other direction.
        const float3 hv_sum = eye_tan + lv_dir;
        const float hv_len2 = dot(hv_sum, hv_sum);
        float3 hv_dir = hv_len2 > 1.0e-6f ? hv_sum * rsqrt(hv_len2) : n_tan;

        // tex2Dlod, not tex2D: gradients under flow control are illegal in ps_3_0 (X3528).
        float lobe_a = tex2Dlod(AnisoMap, float4(dot(r_shift, lv_dir), dot(r_shift, hv_dir), 0.0f, 0.0f)).a;
        float lobe_b = tex2Dlod(AnisoMap, float4(dot(trt_shift, lv_dir), dot(trt_shift, hv_dir), 0.0f, 0.0f)).a;

        float NL = saturate(dot(n_tan, lv_dir));

        // The sun's R lobe uses sunTint, point lights use pointTint. ToggleNumLights.w applies to the sun only.
        float3 rw_v = is_sun ? sun_tint : point_tint;
        float3 lobe_sum = rw_v * lobe_a + lobe_b * 0.7f;
        float3 spec_a = lobe_sum * lc_v * NL * att;
        float3 spec_b = lobe_sum * gc_c * NL * att;
        if (is_sun) { spec_a *= ToggleNumLights.w; spec_b *= ToggleNumLights.w; }

        // Albedo is applied once below. Here only the light term is computed; at metallicness 0 the
        // reflectance is a constant 0.04, so the two parts separate cleanly.
        float3 dif_a = PBRDiffuse(0.0f, HAIR_ROUGHNESS, 1.0f, n_tan, eye_tan, lv_dir, lc_v) * att;

        // Sun only.
        if (is_sun) {
            dif_a *= sun_occ;
            spec_a *= sun_occ;
            spec_b *= sun_occ;
        }

        // Zero out lights that are not active.
        float act_f = (li_v < n_lights) ? 1.0f : 0.0f;
#if HAIR_FP
        if (!is_sun) act_f = 0.0f;   // UNOFFICIAL 2026-11-16 forward+: the game's point lamps are the table's, below
#endif
        if (beam_on && !is_sun && dot(to_l, beam_obj) > 0.99996f * d_len) act_f = 0.0f;   // UNOFFICIAL 2026-11-12: the flashlight's game lamp (see above)
        diff_sum += dif_a * act_f;
        spec_sum += spec_a * act_f;
        spec_sum_game += spec_b * act_f;
    }

#if HAIR_FP
    diff_sum += fp_diff;
    if (TESR_HairData.y <= 0.5f) spec_sum += fp_spec;
#endif

    // UNOFFICIAL 2026-11-12 HairAndEyes (Includes/FlashBeam.hlsl): the flashlight's beam as one more lamp, with the terms a lamp has
    // above (the diffuse, and the strand highlight with the lamps' tint) and the beam's own strength in place of a lamp's falloff.
    // Its direction comes from the vertex shader in object space; the cone, reach and shadow are taken at the world position.
    [branch] if (beam_on) {
        float3 beam_to_light;
        const float beam = FlashBeamStrength(src_c.f_shadow_pos.xyz, beam_to_light);
        [branch] if (beam > 0.0f) {
            const float3 beam_tan = mul(tbnMat, src_c.f_beam_obj.xyz);
            const float3 beam_dir = beam_tan * rsqrt(max(dot(beam_tan, beam_tan), 1.0e-12f));
            const float3 beam_hv = eye_tan + beam_dir;
            const float beam_hv2 = dot(beam_hv, beam_hv);
            const float3 beam_half = beam_hv2 > 1.0e-6f ? beam_hv * rsqrt(beam_hv2) : n_tan;   // (the light behind the head: as above)
            const float beam_a = tex2Dlod(AnisoMap, float4(dot(r_shift, beam_dir), dot(r_shift, beam_half), 0.0f, 0.0f)).a;
            const float beam_b = tex2Dlod(AnisoMap, float4(dot(trt_shift, beam_dir), dot(trt_shift, beam_half), 0.0f, 0.0f)).a;
            const float3 beam_lobes = (point_tint * beam_a + beam_b * 0.7f) * (saturate(dot(n_tan, beam_dir)) * beam);
            spec_sum += beam_lobes * TESR_FlashBeam[2].rgb;
            spec_sum_game += beam_lobes * LinEncode(TESR_FlashBeam[2].rgb);   // the game's look takes the light in its stored curve
            diff_sum += PBRDiffuse(0.0f, HAIR_ROUGHNESS, 1.0f, n_tan, eye_tan, beam_dir, TESR_FlashBeam[2].rgb) * beam;
        }
    }

    // Original order: (diffuse + ambient) * (sun tint * albedo) + specular * gain.
    // Ambient joins the sum before the albedo multiply, so unlit hair keeps its tint.
    float3 amb_v = PBRAmbient(AmbientColor.rgb) + SkyAmbient(shadow_n, SHADOW_VS_PRESENT(src_c.f_shadow_pos.w) ? 1.0f : 0.0f);
    // UNOFFICIAL linear lighting: decoded albedo, encoded result (PBRScale.hlsl decodes the light). HairHighlight: the game's look adds the
    // highlight after the encode, from the game's light colours, as the game's own shader does; either way times HairHighlight.
    float hi_scale = 1.0f + TESR_HairData.x;
#if HAIR_FP
    float3 col_v = TESR_HairData.y > 0.5f
        ? LinEncode((diff_sum + amb_v) * LinColor(albedo_tinted) + fp_spec * (spec_gain * hi_scale)) + spec_sum_game * (spec_gain * hi_scale)
        : LinEncode((diff_sum + amb_v) * LinColor(albedo_tinted) + spec_sum * (spec_gain * hi_scale));
#else
    float3 col_v = TESR_HairData.y > 0.5f
        ? LinEncode((diff_sum + amb_v) * LinColor(albedo_tinted)) + spec_sum_game * (spec_gain * hi_scale)
        : LinEncode((diff_sum + amb_v) * LinColor(albedo_tinted) + spec_sum * (spec_gain * hi_scale));
#endif

    // Blend mode from MatAlpha.z and .w: fogged, premultiplied or additive.
    float3 fog_l = lerp(col_v, src_c.f_fog_in.rgb, src_c.f_fog_in.w);
    float3 prem_l = col_v * (1.0f - src_c.f_fog_in.w);
    float3 add_l = lerp(col_v, 1.0f, src_c.f_fog_in.w);

    float3 out_col = lerp(fog_l, prem_l, MatAlpha.z);
    out_col = lerp(out_col, add_l, MatAlpha.w);

    dst.f_color_out.rgb = out_col;
    dst.f_color_out.a = base_sample.a * MatAlpha.x;
#if FLASH_BEAM_MARK
    // UNOFFICIAL 2026-11-12 HairAndEyes: the mark (Includes/FlashBeam.hlsl). Blue = far away, alpha = 1 or 0 by the hair's own alpha
    // against the pixel's dither: under the hair's blend (SRCALPHA / INVSRCALPHA, each target by its own alpha) the record's distance
    // becomes "far" or stays; drawn without a blend, far wherever the hair draws. core/CapturedNormals.h lets only the blue through,
    // and only while the beam is on.
    dst.f_pad1 = 0.0f;
    dst.f_pad2 = 0.0f;
    dst.f_mark = float4(0.0f, 0.0f, FLASH_BEAM_FAR, dst.f_color_out.a > FlashBeamDither(src_c.f_vpos) ? 1.0f : 0.0f);
#endif

    // No texkill. baseTex.a fades smoothly from opaque core to fully transparent at strand
    // tips/edges; clip() at MatAlpha.y's mid-range threshold discarded that entire soft
    // transition, leaving only the fully-opaque core with a hard edge at the threshold contour
    // instead of a feathered fade. Real alpha blending (already active for this pass) handles
    // the full gradient correctly on its own -- a near-zero-alpha pixel contributes ~nothing to
    // the blend, so there's no correctness cost to leaving it in rather than discarding it.

    return dst;
};
