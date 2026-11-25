// Decal PS -- vanilla SM3007.pso (BSSM_3XLIGHTING_VcPxoSpc, bullet holes and impact decals),
// plus PBR light/ambient scaling, the hemisphere skylight, and a forward sun shadow.
// Pairs with SM3004.vso. Both were vanilla, which is why decals kept flat vanilla lighting
// while the surfaces under them had PBR, skylight and shadows -- a bullet hole on a shadowed
// wall lit as though it were in full sun.
// LightData[8] at c21: four (colour, position+radius) pairs, same layout as SM3003.pso. Light
// 0 is the DIRECTIONAL sun -- LightData[1].xyz is a direction, not a position -- gated by
// ToggleNumLights.x, and it is the one that takes the shadow.
// SPACES: tangent/binormal/normal and objPos arrive in OBJECT space (SM3004.vso passes the
// vertex attributes through untransformed), and EyePosition is object-space to match. The
// skylight needs a WORLD normal, so it comes from shadowWorldPos via GetShadowGeometricNormal,
// exactly as ObjectTemplate does.
#define SHADOW_CLOUD_IN 1   // UNOFFICIAL 2026-11-04 ForwardCloudShadows (Includes/Shadow.hlsl): the clouds' shadow on the sun term (always compiled in; its strength switches it live)
#include "includes/Shadow.hlsl"
#include "includes/Helpers.hlsl"
#include "includes/PBRScale.hlsl"
#include "includes/PointShadowForward.hlsl"  // UNOFFICIAL forward point-light shadows (INTERIOR_SHADOWS, interiors)

uniform float4 AmbientColor    : register(c0);
uniform float4 EyePosition     : register(c1);
uniform float4 MatAlpha        : register(c3);
uniform float4 ParallaxData    : register(c7);
uniform float4 ToggleNumLights : register(c20);   // x,y: light counts, z: specular power, w: spec scale
uniform float4 LightData[8]    : register(c21);   // (colour, position+radius) x 4

uniform sampler2D BaseMap   : register(s0);
uniform sampler2D NormalMap : register(s1);
uniform sampler2D HeightMap : register(s4);

struct VtxAttrs {
    float2 f_uv             : TEXCOORD0;
    float4 f_shadow_world_pos : TEXCOORD1;
    float3 f_color          : COLOR0;
    float3 f_tangent        : TEXCOORD3_centroid;
    float3 f_binormal       : TEXCOORD4_centroid;
    float3 f_normal         : TEXCOORD5_centroid;
    float3 f_obj_pos         : TEXCOORD6_centroid;
    float4 f_fog            : TEXCOORD7_centroid;
};

struct VtxVaryings {
    float4 f_color_0 : COLOR0;
};

VtxVaryings main(VtxAttrs pa) {
    VtxVaryings pr;

    float3x3 loc_tbn = float3x3(normalize(pa.f_tangent), normalize(pa.f_binormal), normalize(pa.f_normal));

    float3 to_eye = EyePosition.xyz - pa.f_obj_pos;
    float3 eye_t = normalize(mul(loc_tbn, to_eye));

    // ddx/ddy must stay at top level, outside the raymarch below.
    float2 dx_v = ddx(pa.f_uv);
    float2 dy_v = ddy(pa.f_uv);

    // Parallax occlusion, reproducing vanilla's linear search. The ray starts at height 1 and
    // steps down until it falls below the height field; step sizes come straight from the
    // disassembly, ParallaxData.x scaling depth and .y the uv stride.
    float2 uv_stride = eye_t.xy * (0.125f * ParallaxData.y);
    float inv_z = 1.0f / (eye_t.z * -(length(eye_t.xy) * ParallaxData.x));
    float2 step_uv = uv_stride * inv_z;
    float step_z = eye_t.z * inv_z;

    float3 ray_v = float3(pa.f_uv, 1.0f);
    float prev_diff = 0.0f;
    float cur_diff = 1.0f;

    [loop] for (int p = 0; p < 255; p++) {
        if (cur_diff <= 0.0f) break;
        float hgt = tex2Dgrad(HeightMap, ray_v.xy, dx_v, dy_v).x;
        float diff_v = ray_v.z - hgt;
        ray_v.xy += step_uv;
        ray_v.z += step_z;
        prev_diff = cur_diff;
        cur_diff = diff_v;
    }

    // Linear interpolation between the last two samples.
    float2 uv = (cur_diff / (prev_diff - cur_diff) - 1.0f) * step_uv + ray_v.xy;

    float4 normal_tex = tex2D(NormalMap, uv);
    float3 N = normalize(expand(normal_tex.xyz));

    // Applied once to the summed highlight, as vanilla does.
    float spec_scale = normal_tex.w * LightData[1].w;

    // ddx/ddy must stay at top level, outside dynamic flow control.
    float3 shadow_normal = GetShadowGeometricNormal(pa.f_shadow_world_pos.xyz);
#if FORWARD_SHADOWS
    float sun_shadow = SHADOW_VS_PRESENT(pa.f_shadow_world_pos.w)
                    ? GetSunShadow(pa.f_shadow_world_pos.xyz, shadow_normal)
                    : 1.0f;
#else
    float sun_shadow = 1.0f;
#endif

    bool has_sun = ToggleNumLights.x > 0.0f;
#if INTERIOR_SHADOWS
    // UNOFFICIAL forward point-light shadows (Includes/PointShadowForward.hlsl). Light i of the draw is LightData pair i;
    // the DLL leaves a directional light 0 without a shadow. Attenuation as in the loop below.
    float3 ptWorldPos = pa.f_shadow_world_pos.xyz;
    float3 ptNormal = 0.0f;
    float ptValid = SHADOW_VS_PRESENT(pa.f_shadow_world_pos.w) ? 1.0f : 0.0f;
    #define SM3_POINT_ATT(i) saturate(1.0f - sqr(saturate(length(LightData[(i) * 2 + 1].xyz - pa.f_obj_pos) / max(LightData[(i) * 2 + 1].w, 0.0001f))))
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
    float3 spec_c = 0.0f;

    [unroll]
    for (int i = 0; i < 4; i++) {
        if (i >= num_lights) break;

        float3 light_color = PBRLight(LightData[i * 2].rgb);
#if INTERIOR_SHADOWS
        light_color *= ptShadow[i];   // all of this light's terms
#endif
        float4 light_vec   = LightData[i * 2 + 1];

        // Light 0 is the directional sun: lightVec.xyz is a direction and there is no falloff.
        bool is_directional = (i == 0) && has_sun;

        float3 to_light = is_directional ? light_vec.xyz : (light_vec.xyz - pa.f_obj_pos);
        float3 light_t = is_directional ? mul(loc_tbn, to_light) : normalize(mul(loc_tbn, to_light));

        // 1 - (dist/radius)^2, clamped. None for the directional light.
        float att = 1.0f;
        if (!is_directional) {
            float d = saturate(length(to_light) / light_vec.w);
            att = LampAttOther(1.0f - d * d); // UNOFFICIAL 2026-10-21 LampFalloff (Includes/LampFalloff.hlsl): as before, the objects' decode or the shape
        }

        // The shadow scales the sun only; point lights and ambient stay untouched.
        if (is_directional) att *= sun_shadow;

        float n_dot_l = max(dot(N, light_t), 0.0f);
        lambert += light_color * (n_dot_l * att);

        float3 half_vec = normalize(eye_t + light_t);
        float spec_strength = pow(saturate(dot(half_vec, N)), ToggleNumLights.z);
        spec_c += light_color * (spec_strength * ToggleNumLights.w * att);
    }

    // shadowNormal is the geometric WORLD normal; N above is tangent space.
    float3 amb_rgb = PBRAmbient(AmbientColor.rgb) + SkyAmbient(shadow_normal, SHADOW_VS_PRESENT(pa.f_shadow_world_pos.w) ? 1.0f : 0.0f);

    // Vanilla: (diffuseSum + AmbientColor) * albedo + specSum * specScale
    float4 base_tex = tex2D(BaseMap, uv);
    float3 base_rgb = base_tex.rgb * pa.f_color;
    float3 color = LinEncode((lambert + amb_rgb) * LinColor(base_rgb) + spec_c * spec_scale);	// UNOFFICIAL linear lighting: decoded albedo, encoded result (PBRScale.hlsl decodes the light)

    // Decal fog and blend modes, straight from the disassembly. MatAlpha .z and .w select
    // between the fogged, unfogged and whitened variants; .x scales the output alpha.
    float3 loc_fogged   = lerp(color, pa.f_fog.rgb, pa.f_fog.w);
    float3 loc_unfogged = color * (1.0f - pa.f_fog.w);
    float3 loc_blended  = lerp(loc_fogged, loc_unfogged, MatAlpha.z);
    float3 loc_whitened = lerp(color, 1.0f, pa.f_fog.w);

    pr.f_color_0.rgb = lerp(loc_blended, loc_whitened, MatAlpha.w);
    pr.f_color_0.a = base_tex.a * MatAlpha.x;

    return pr;
};
