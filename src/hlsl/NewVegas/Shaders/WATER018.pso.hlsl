// wading (displacement) placed water

uniform float4 NotUsed0 : register(c0);
uniform float4 EyePos : register(c1);
uniform float4 ShallowColor : register(c2);
uniform float4 DeepColor : register(c3);
uniform float4 ReflectionColor : register(c4);
uniform float4 FresnelRI : register(c5);
uniform float4 BlendRadius : register(c6);
uniform float4 NotUsed7 : register(c7);
uniform float4 VarAmounts : register(c8);
uniform float4 FogParam : register(c9);
uniform float4 FogColor : register(c10);
uniform float2 DepthFalloff : register(c11);
uniform float4 SunDir : register(c12);
uniform float4 SunColor : register(c13);

uniform float4 TESR_PlacedWaveParams : register(c14); // x: choppiness, y:wave width, z: wave speed, w: reflectivity?
uniform float4 TESR_PlacedWaterVolume : register(c15); // x: caustic strength, y:shoreFactor, w: turbidity, z: caustic strength S ?
uniform float4 TESR_PlacedWaterSettings : register(c16); // x: caustic strength, y:depthDarkness, w: turbidity, z: caustic strength S ?
uniform float4 TESR_GameTime : register(c17);
uniform float4 TESR_HorizonColor : register(c18);
uniform float4 TESR_SunDirection : register(c19);
uniform float4 TESR_WetWorldData : register(c20);
uniform float4 TESR_PlacedWaterShorelineParams : register(c21);
uniform float4 TESR_DebugVar : register(c22);
uniform float4 TESR_SunAmount : register(c23);
uniform float4 TESR_RenderScale : register(c24); // UNOFFICIAL 2026-10-27 RenderScale: xy = the internal resolution / the screen's while the world is drawn smaller (the refraction copy is made mid-scene, so its picture fills only that part), else 1
uniform float4 TESR_ReciprocalResolution : register(c25); // UNOFFICIAL 2026-11-16: one pixel of the screen (= the refraction copy) in uv, for refractionLookup (Includes/Water.hlsl)

uniform sampler2D ReflectionMap : register(s0);
uniform sampler2D RefractionMap : register(s1);
uniform sampler2D NormalMap : register(s2);
uniform sampler2D DisplacementMap : register(s3);
uniform sampler2D DepthMap : register(s4);
uniform sampler2D TESR_samplerWater : register(s5) < string ResourceName = "Water\watercalm_NRM.dds"; > = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; ADDRESSW = WRAP; MAGFILTER = ANISOTROPIC; MINFILTER = ANISOTROPIC; MIPFILTER = ANISOTROPIC; } ;
uniform sampler2D TESR_RippleSampler : register(s6) < string ResourceName = "Precipitations\ripples.dds"; > = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };

#include "Includes/Helpers.hlsl"
#include "Includes/Water.hlsl"

PS_OUTPUT main(PS_INPUT pa, float2 pixel_pos : VPOS) {
    PS_OUTPUT pr;

    float4 lin_sun_color = linearize(SunColor); //linearise
    float4 lin_shallow_color = linearize(ShallowColor); //linearise
    float4 lin_deep_color = linearize(DeepColor); //linearise
    float4 lin_horizon_color = linearize(TESR_HorizonColor); //linearise

    float opacity = pa.LTEXCOORD_6.w; // the water mesh's vertex alpha
    float3 eye_vector = EyePos.xyz - pa.LTEXCOORD_0.xyz; // vector of camera position to point being shaded
    float3 eye_direction = normalize(eye_vector);         // normalized eye to world vector (for lighting)
    float flat_dist = length(eye_vector.xy);              // surface distance to eye
    float eye_range = length(eye_vector);                    // depth distance to eye

	// UNOFFICIAL placed-water fog: no sun fog at night, as the world water (WATER017)
	float is_day_time = smoothstep(0, 0.5, TESR_SunAmount.x);
	float sun_luma = luma(lin_sun_color) * is_day_time;
	float placed_water_refraction_modifier = TESR_PlacedWaterSettings.w;		// reduce refraction because of the way placed depth is encoded
	float placed_water_depth_modifier = 0.2;			// reduce depth value for fog because of the way placed depth is encoded
	// UNOFFICIAL 2026-10-25: was 0.1, WATER001 (the same water without ripples) uses 0.2: the game swaps to this shader while anything
	// touches the water (wading displacement) and back after, so the depth fog halved and doubled at every splash (placed water
	// "flickers whenever something collides with it"). Both now 0.2. WATER001's other two differences are fixed on the green-well branch.

    // calculate fog coeffs
    float4 screen_pos = getScreenpos(pa);                // point coordinates in screen space for water surface
    float2 water_depth = tex2Dproj(DepthMap, screen_pos).xy;  // x= shallowfog, y = deepfog?
    float depth_fog = saturate(invlerp(DepthFalloff.x, DepthFalloff.y, water_depth.y));

    float2 faded_depth = saturate(lerp(water_depth, 1, invlerp(0, 4096, flat_dist)));

    float3 surface_normal = getWaveTexture(pa, flat_dist, TESR_PlacedWaveParams).xyz;
    surface_normal = getRipples(pa, TESR_RippleSampler, surface_normal, flat_dist, TESR_WetWorldData.x);
    surface_normal = getDisplacement(pa, BlendRadius.w, surface_normal);
    
    float refraction_coeff = (water_depth.y * depth_fog) * ((saturate(flat_dist * 0.002) * (-4 + VarAmounts.w)) + 4);
    float4 reflection_pos = getReflectionSamplePosition(pa, surface_normal, refraction_coeff * placed_water_refraction_modifier );
    //float4 reflection = linearize(tex2Dproj(ReflectionMap, reflectionPos));
    float4 refraction_pos = reflection_pos;
    refraction_pos.y = refraction_pos.w - reflection_pos.y;
    float3 refracted_depth = tex2Dproj(DepthMap, refraction_pos).rgb * placed_water_depth_modifier;

    float4 refr_c = linearize(tex2Dproj(RefractionMap, refractionLookup(refraction_pos, TESR_RenderScale, TESR_ReciprocalResolution)));
    float4 pix = refr_c;
    pix = getLightTravel(refracted_depth, lin_shallow_color, lin_deep_color, sun_luma, TESR_PlacedWaterSettings, pix);
    pix = getTurbidityFog(refracted_depth, lin_shallow_color, TESR_PlacedWaterVolume, sun_luma, pix);
    //color = getDiffuse(surfaceNormal, TESR_SunDirection.xyz, eyeDirection, distance, linShallowColor, color);
    // UNOFFICIAL placed water as the game's own shader: the water tint (light travel + haze) only where the water depth map
    // has depth (vanilla multiplies its haze by this same depthFog). Where the depth map is empty - a narrow well, a small
    // trough - the game shows the bottom; NVR painted the haze over it (a flat opaque green puddle).
    pix = lerp(refr_c, pix, depth_fog);
    pix = getFresnel(surface_normal, eye_direction, lin_horizon_color, TESR_PlacedWaveParams.w, pix);
    pix = getSpecular(surface_normal, TESR_SunDirection.xyz, eye_direction, lin_sun_color.rgb, pix);

    pix = delinearize(pix); //delinearise
    
    // Standard fog.
    float fog_strength = pow(1 - saturate((FogParam.x - eye_range) / FogParam.y), FresnelRI.y);
    
    pr.color_0.rgb = fog_strength * (FogColor.rgb - pix.rgb) + pix.rgb;
    pr.color_0.a = opacity; // UNOFFICIAL: the mesh's vertex alpha, as the game's own placed-water shaders (was always opaque)
    
    return pr;
};

