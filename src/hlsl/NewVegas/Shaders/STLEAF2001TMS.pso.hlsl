// SpeedTree leaf PS -- BSSM_LEAVES_ADPt with fog, two-sided mask. Port of vanilla STLEAF2001TMS.pso, plus forward sun shadows.
// ps_2_x -> ps_3_0: Shadow.hlsl needs tex2Dlod, and D3D9 requires a matching vs_3_0.
//
// Vanilla: as STLEAF2001, alpha doubled (add r2.w, r0.w, r0.w -- unsaturated)
// UNOFFICIAL 2026-11-16 fast-restart (testers: grass and leaves glowing under a cloud's shadow; docs/next/FAST-RESTART.md): the clouds' shadow
// on the sun term with ForwardShadows on, as every object (Includes/Shadow.hlsl SHADOW_CLOUD_IN; strength 0 with PlantShadows off). Not in
// the deferred record (FOLIAGE_DEFERRED): DeferredSun shades its sun.
#if !(defined(FOLIAGE_DEFERRED) && FOLIAGE_DEFERRED)
    #define SHADOW_CLOUD_IN 1
#endif
#include "includes/Shadow.hlsl"
#include "includes/Leaf.hlsl"

PS_OUTPUT main(PS_INPUT IN) {
    PS_OUTPUT OUT;

    float3 lighting = LeafLighting(IN);
    float4 albedo = tex2D(DiffuseMap, IN.uv);
    float3 lit = LinEncode(LinColor(albedo.rgb) * lighting);	// UNOFFICIAL linear lighting: decoded albedo, encoded result (Leaf.hlsl decodes the light)

    OUT.color.rgb = lerp(lit, IN.fog.rgb, IN.fog.w);
    OUT.color.a = albedo.a + albedo.a;

    LeafRecords(OUT, IN, albedo)	// UNOFFICIAL 2026-11-12 leaf records (Includes/Leaf.hlsl; nothing without the AlbedoBuffer)
    LeafDeferred(OUT, IN, albedo, IN.fog.rgb, IN.fog.w)	// UNOFFICIAL 2026-11-14 full deferred renderer: the deferred record's split (Includes/Leaf.hlsl; nothing without FOLIAGE_DEFERRED)

    return OUT;
};
