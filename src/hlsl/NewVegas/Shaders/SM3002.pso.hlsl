// Hair PS -- vanilla SM3002.pso (BSSM_3XLIGHTING_H), PBR diffuse + ambient, forward sun shadow.
// The no-highlight sibling of SM3003.pso, which the trace pairs with the same SM3003.vso. The
// vanilla pair differ by 109 instructions, and SM3002 declares neither the AnisoMap sampler at
// s4 nor EyePosition at c1, and reads only ToggleNumLights .x and .y where SM3003 also takes .w
// for specular scale. Interpolators, MatAlpha components, the vertex-colour tint and the texkill
// are identical, so everything except the Marschner lobes carries over unchanged.
// LightData[8] at c21: four (colour, position+radius) pairs. Light 0 is the directional sun,
// gated by ToggleNumLights.x, and takes the shadow.

// Above the global 0: alpha-tested strands self-shadow without an offset. 0 inherits global.
#ifndef SHADOW_NORMAL_BIAS_TEXELS
    #define SHADOW_NORMAL_BIAS_TEXELS 2.50f
#endif
#define SHADOW_CLOUD_IN 1   // UNOFFICIAL 2026-11-04 ForwardCloudShadows (Includes/Shadow.hlsl): the clouds' shadow on the sun term (compiled in only when on at startup)
#include "includes/Shadow.hlsl"
#include "includes/Helpers.hlsl"
#include "includes/PBR.hlsl"
#include "includes/PBRScale.hlsl"
#include "includes/PointShadowForward.hlsl"  // UNOFFICIAL forward point-light shadows (INTERIOR_SHADOWS, interiors)
// UNOFFICIAL 2026-11-12 HairAndEyes (Includes/FlashBeam.hlsl; as SM3003.pso): the hair takes the flashlight's beam itself, and marks its
// pixels so the light pass does not light the scalp through it. c4-c12, s6 and s7 are free in this shader and its includes.
#define FLASH_BEAM_REG c4
#define FLASH_BEAM_MATRIX_REG c9
#define FLASH_BEAM_SAMPLER s6
#define FLASH_BEAM_COOKIE_SAMPLER s7
#include "includes/FlashBeam.hlsl"
#if defined(CAPTURED_NORMALS_ON) && CAPTURED_NORMALS_ON >= 3
    #define FLASH_BEAM_MARK 1
#else
    #define FLASH_BEAM_MARK 0
#endif

uniform float4 AmbientColor    : register(c0);
uniform float4 HairTint        : register(c2);
uniform float4 MatAlpha        : register(c3);
uniform float4 ToggleNumLights : register(c20);   // x,y: light counts. w is specular, unused here.
uniform float4 LightData[8]    : register(c21);   // (colour, position+radius) x 4

uniform sampler2D BaseMap   : register(s0);
uniform sampler2D NormalMap : register(s1);
uniform sampler2D LayerMap  : register(s5);

// Unused by PBRDiffuse, which has no lobe to widen; kept so the call matches SM3003.pso.
#ifndef HAIR_ROUGHNESS
    #define HAIR_ROUGHNESS 0.60f
#endif

// UNOFFICIAL 2026-11-16 fd-fwdgroups DeferredHair (Includes/HairForwardPlus.hlsl; as SM3003.pso): the forward+ record (HAIR_FORWARD_PLUS 1):
// the sun pair kept, the point lamps replaced by every lamp of the pixel's froxel. Without the define: as before, byte for byte.
#if defined(HAIR_FORWARD_PLUS) && HAIR_FORWARD_PLUS
    #define HAIR_FP 1
    #define HAIR_FP_V float4
    #define HAIR_GAME_LIGHTS 1
    #include "includes/HairForwardPlus.hlsl"
#else
    #define HAIR_FP 0
    #define HAIR_FP_V float3
    #define HAIR_GAME_LIGHTS 4
#endif

struct VtxAttrs {
#if HAIR_FP
    float4 f_uv             : TEXCOORD0;   // zw: the world tangent's y z (SM3003.vso with G-buffer v2)
#else
    float2 f_uv             : TEXCOORD0;
#endif
    float4 f_shadow_world_pos : TEXCOORD1;
    float4 f_beam_obj       : TEXCOORD2;   // UNOFFICIAL 2026-11-12 xyz to the flashlight, object space (SM3003.vso; undefined under the game's VS)
    float4 f_color          : COLOR0;
    HAIR_FP_V f_tangent        : TEXCOORD3_centroid;   // (HAIR_FP: w the world normal's x)
    HAIR_FP_V f_binormal       : TEXCOORD4_centroid;   // (w its y)
    HAIR_FP_V f_normal         : TEXCOORD5_centroid;   // (w its z)
    HAIR_FP_V f_obj_pos         : TEXCOORD6_centroid;  // (w the world tangent's x)
    float4 f_fog            : TEXCOORD7_centroid;
#if FLASH_BEAM_MARK
    float2 f_vpos           : VPOS;        // UNOFFICIAL 2026-11-12 the pixel, for the mark's dither
#endif
};

struct VtxVaryings {
    float4 f_color_0 : COLOR0;
#if FLASH_BEAM_MARK
    // UNOFFICIAL 2026-11-12 (ps_3_0 wants COLOR0..3 without a gap: 1 and 2 are written as 0 and masked off by core/CapturedNormals.h)
    float4 f_pad1 : COLOR1;
    float4 f_pad2 : COLOR2;
    float4 f_mark : COLOR3;   // the captured normals' target: only its blue (the distance) is written, and only while the beam is on
#endif
};

VtxVaryings main(VtxAttrs pa) {
    VtxVaryings pr;

    float3 T = normalize(pa.f_tangent.xyz);
    float3 B = normalize(pa.f_binormal.xyz);
    float3 nw_dir = normalize(pa.f_normal.xyz);
    float3x3 loc_tbn = float3x3(T, B, nw_dir);

    float3 N = normalize(expand(tex2D(NormalMap, pa.f_uv.xy).xyz));

    // LayerMap over BaseMap. Vertex colour .y folds into the tint, not the albedo.
    float4 base_tex  = tex2D(BaseMap,  pa.f_uv.xy);
    float4 layer_tex = tex2D(LayerMap, pa.f_uv.xy);
    float3 base_rgb = lerp(base_tex.rgb, layer_tex.rgb, layer_tex.a);

    //   base = lerp(0.5, HairTint, vertexColour.y), doubled into the albedo tint.
    float3 tint_base = pa.f_color.y * (HairTint.rgb - 0.5f) + 0.5f;
    float3 tinted_albedo = base_rgb * (tint_base * 2.0f);

    // Hair strands are thin, alpha-tested and often near-edge-on or overlapping within a
    // pixel quad, so GetShadowGeometricNormal's ddx/ddy reconstruction reads a different (or
    // near-degenerate) normal every couple of pixels. The self-shadow bias below offsets the
    // shadow-map sample ALONG this normal, so that noise turns directly into per-pixel
    // light/dark jitter -- the splotching. Substitute the view direction instead: it is
    // smooth (no derivatives), still points outward from the strand toward the camera, which
    // is enough to suppress self-shadow acne and give SkyAmbient a stable hemisphere split.
    // worldPos is camera-relative, so -worldPos already points from the surface to the camera.
    float3 shadow_normal = normalize(-pa.f_shadow_world_pos.xyz);
#if (FORWARD_SHADOWS)
    float sun_shadow = SHADOW_VS_PRESENT(pa.f_shadow_world_pos.w)
                    ? GetSunShadow(pa.f_shadow_world_pos.xyz, shadow_normal)
                    : 1.0f;
#else
    float sun_shadow = 1.0f;
#endif

#if INTERIOR_SHADOWS && !HAIR_FP
    // UNOFFICIAL forward point-light shadows (Includes/PointShadowForward.hlsl). Light i of the draw is LightData pair i;
    // the DLL leaves a directional light 0 without a shadow. Attenuation as in the loop below.
    float3 ptWorldPos = pa.f_shadow_world_pos.xyz;
    float3 ptNormal = 0.0f;
    float ptValid = SHADOW_VS_PRESENT(pa.f_shadow_world_pos.w) ? 1.0f : 0.0f;
    #define SM3_POINT_ATT(i) saturate(1.0f - sqr(saturate(length(LightData[(i) * 2 + 1].xyz - pa.f_obj_pos.xyz) / max(LightData[(i) * 2 + 1].w, 0.0001f))))
    float3 ptShadow[4] = { (float3)1.0f, (float3)1.0f, (float3)1.0f, (float3)1.0f };
    [branch] if (NVR_PointShadowParams.x > 0.0f) {
        ptShadow[0] = POINT_SHADOW_LOOKUP(0, SM3_POINT_ATT(0));
        ptShadow[1] = POINT_SHADOW_LOOKUP(1, SM3_POINT_ATT(1));
        ptShadow[2] = POINT_SHADOW_LOOKUP(2, SM3_POINT_ATT(2));
        ptShadow[3] = POINT_SHADOW_LOOKUP(3, SM3_POINT_ATT(3));
    }
    #undef SM3_POINT_ATT
#endif

    int num_lights = (int)min(ToggleNumLights.x + ToggleNumLights.y, 4.0f);

    float3 lambert = 0.0f;

    // .x gates the sun; .y counts point lights 1..3.
    bool has_sun = ToggleNumLights.x > 0.0f;

    // UNOFFICIAL 2026-11-12 HairAndEyes (as SM3003.pso): while the hair takes the flashlight's beam itself (after the loop), the
    // flashlight's game lamp is left out of the lamps: the one in the beam's own direction from this pixel (within half a degree).
    const bool beam_on = FLASH_BEAM_ON && SHADOW_VS_PRESENT(pa.f_shadow_world_pos.w);
    const float3 beam_obj = pa.f_beam_obj.xyz * rsqrt(max(dot(pa.f_beam_obj.xyz, pa.f_beam_obj.xyz), 1.0e-12f));

    [unroll]
    for (int i = 0; i < HAIR_GAME_LIGHTS; i++) {
        float3 light_color = PBRLight(LightData[i * 2].rgb);
#if INTERIOR_SHADOWS && !HAIR_FP
        light_color *= ptShadow[i];   // all of this light's terms
#endif
        float4 light_vec   = LightData[i * 2 + 1];

        bool is_directional = (i == 0) && has_sun;

        float3 to_light = is_directional ? light_vec.xyz : (light_vec.xyz - pa.f_obj_pos.xyz);

        // 1 - (dist/radius)^2. None for directional.
        float len_v = length(to_light);
        float d = saturate(len_v / max(light_vec.w, 0.0001f));
        float falloff_v = is_directional ? 1.0f : LampAttOther(saturate(1.0f - d * d)); // UNOFFICIAL 2026-10-21 LampFalloff (Includes/LampFalloff.hlsl): as before, the objects' decode or the shape

        float3 L = normalize(mul(loc_tbn, to_light));

        // Albedo 1: pure lighting term, tinted albedo multiplied in once below. Separable at
        // metallicness 0, where reflectance is a constant 0.04. PBRDiffuse reads neither the eye
        // vector nor roughness, which is why this permutation needs no EyePosition.
        float3 light_diffuse = PBRDiffuse(0.0f, HAIR_ROUGHNESS, 1.0f, N, N, L, light_color) * falloff_v;

        // Sun only.
        if (is_directional) light_diffuse *= sun_shadow;

        // Mask inactive lights (UNOFFICIAL 2026-11-12: and the flashlight's game lamp while the beam below lights the hair).
        const bool flash_lamp = beam_on && !is_directional && dot(to_light, beam_obj) > 0.99996f * len_v;
#if HAIR_FP
        if (!is_directional) light_diffuse = 0.0f;   // UNOFFICIAL 2026-11-16 forward+: the game's point lamps are the table's, below
#endif
        lambert += light_diffuse * ((i < num_lights && !flash_lamp) ? 1.0f : 0.0f);
    }
#if HAIR_FP
    // UNOFFICIAL 2026-11-16 fd-fwdgroups DeferredHair: every lamp of the pixel's froxel (Includes/HairForwardPlus.hlsl; as SM3003.pso)
    hairFpM = HairFPFrame(loc_tbn, pa.f_tangent.xyz, pa.f_normal.xyz, float3(pa.f_obj_pos.w, pa.f_uv.zw), float3(pa.f_tangent.w, pa.f_binormal.w, pa.f_normal.w));
    hairFpN = N;
    lambert += FPLampSum(pa.f_shadow_world_pos.xyz, shadow_normal, SHADOW_VS_PRESENT(pa.f_shadow_world_pos.w) ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
#endif

    // UNOFFICIAL 2026-11-12 HairAndEyes (Includes/FlashBeam.hlsl): the flashlight's beam as one more lamp's diffuse, with the beam's own
    // strength in place of a lamp's falloff. Its direction comes from the vertex shader in object space; the cone, reach and shadow
    // are taken at the world position.
    [branch] if (beam_on) {
        float3 beam_to_light;
        const float beam = FlashBeamStrength(pa.f_shadow_world_pos.xyz, beam_to_light);
        const float3 beam_tan = mul(loc_tbn, pa.f_beam_obj.xyz);
        const float3 beam_dir = beam_tan * rsqrt(max(dot(beam_tan, beam_tan), 1.0e-12f));
        lambert += PBRDiffuse(0.0f, HAIR_ROUGHNESS, 1.0f, N, N, beam_dir, TESR_FlashBeam[2].rgb) * beam;
    }

    // Vanilla: (diffuseSum + AmbientColor) * (tint * albedo), with no highlight to add.
    // Ambient joins the sum before the albedo multiply, so unlit hair keeps its tint.
    float3 amb_rgb = PBRAmbient(AmbientColor.rgb) + SkyAmbient(shadow_normal, SHADOW_VS_PRESENT(pa.f_shadow_world_pos.w) ? 1.0f : 0.0f);
    float3 color = LinEncode((lambert + amb_rgb) * LinColor(tinted_albedo));	// UNOFFICIAL linear lighting: decoded albedo, encoded result (PBRScale.hlsl decodes the light)

    // Blend mode: fog / premultiplied / additive, by MatAlpha.z and .w.
    float3 loc_fogged   = lerp(color, pa.f_fog.rgb, pa.f_fog.w);
    float3 loc_premult  = color * (1.0f - pa.f_fog.w);
    float3 additive_c = lerp(color, 1.0f, pa.f_fog.w);

    float3 result_c = lerp(loc_fogged, loc_premult, MatAlpha.z);
    result_c = lerp(result_c, additive_c, MatAlpha.w);

    pr.f_color_0.rgb = result_c;
    pr.f_color_0.a = base_tex.a * MatAlpha.x;
#if FLASH_BEAM_MARK
    // UNOFFICIAL 2026-11-12 HairAndEyes: the mark (Includes/FlashBeam.hlsl; as SM3003.pso). Blue = far away, alpha = 1 or 0 by the
    // hair's own alpha against the pixel's dither; core/CapturedNormals.h lets only the blue through, and only while the beam is on.
    pr.f_pad1 = 0.0f;
    pr.f_pad2 = 0.0f;
    pr.f_mark = float4(0.0f, 0.0f, FLASH_BEAM_FAR, pr.f_color_0.a > FlashBeamDither(pa.f_vpos) ? 1.0f : 0.0f);
#endif

    // No texkill. baseTex.a fades smoothly from opaque core to fully transparent at strand
    // tips/edges; clip() at MatAlpha.y's mid-range threshold discarded that entire soft
    // transition, leaving only the fully-opaque core with a hard edge at the threshold contour
    // instead of a feathered fade. Real alpha blending (already active for this pass) handles
    // the full gradient correctly on its own -- a near-zero-alpha pixel contributes ~nothing to
    // the blend, so there's no correctness cost to leaving it in rather than discarding it.

    return pr;
};
