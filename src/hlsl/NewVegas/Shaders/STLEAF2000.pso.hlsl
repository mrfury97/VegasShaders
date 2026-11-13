// SpeedTree leaf PS -- BSSM_LEAVES_AD (no fog). Port of vanilla STLEAF2000.pso, plus forward sun shadows.
// ps_2_x -> ps_3_0: Shadow.hlsl needs tex2Dlod, and D3D9 requires a matching vs_3_0.
//
// Vanilla: oC0.rgb = albedo * lighting, oC0.a = albedo.a
// UNOFFICIAL 2026-11-14 (rt-shadows F1) the forward RT sun (Includes/Shadow.hlsl SunForward): last frame's traced visibility instead of
// the cascades where it is there; the default registers (s10, c160-c164: free here; not with the sun cross-fade). Not Bend's contact.
#define SHADOW_SUN_CONTACT_IN 1
#define SHADOW_SUN_CONTACT_RT_ONLY 1
#include "includes/Shadow.hlsl"
#include "includes/Leaf.hlsl"

PS_OUTPUT main(PS_INPUT IN) {
    PS_OUTPUT OUT;

    float3 lighting = LeafLighting(IN);
    float4 albedo = tex2D(DiffuseMap, IN.uv);
    float3 lit = LinEncode(LinColor(albedo.rgb) * lighting);	// UNOFFICIAL linear lighting: decoded albedo, encoded result (Leaf.hlsl decodes the light)

    OUT.color.rgb = lit;
    OUT.color.a = albedo.a;

    return OUT;
};
