// wading (displacement) interior autowater

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

uniform float4 TESR_WaveParams : register(c14); // x: choppiness, y:wave width, z: wave speed, w: reflectivity?
uniform float4 TESR_WaterVolume : register(c15); // x: caustic strength, y:shoreFactor, w: turbidity, z: caustic strength S ?
uniform float4 TESR_WaterSettings : register(c16); // x: caustic strength, y:depthDarkness, w: turbidity, z: caustic strength S ?
uniform float4 TESR_GameTime : register(c17);
uniform float4 TESR_HorizonColor : register(c18);
uniform float4 TESR_SunDirection : register(c19);
uniform float4 TESR_WaterShorelineParams : register(c20);
uniform float4 TESR_DebugVar : register(c21);
uniform float4 TESR_ShadowLightPosition[12] : register(c22);
uniform float4 TESR_LightPosition[12] : register(c34);
uniform float4 TESR_LightColor[24] : register(c46);
uniform float4 TESR_RenderScale : register(c70); // UNOFFICIAL 2026-10-27 RenderScale: xy = the internal resolution / the screen's while the world is drawn smaller (the refraction copy is made mid-scene, so its picture fills only that part), else 1
uniform float4 TESR_ReciprocalResolution : register(c71); // UNOFFICIAL 2026-11-16: one pixel of the screen (= the refraction copy) in uv, for refractionLookup (Includes/Water.hlsl)

uniform sampler2D ReflectionMap : register(s0);
uniform sampler2D RefractionMap : register(s1);
uniform sampler2D NormalMap : register(s2);
uniform sampler2D DisplacementMap : register(s3);
uniform sampler2D DepthMap : register(s4);
uniform sampler2D TESR_samplerWater : register(s5) < string ResourceName = "Water\water_NRM.dds"; > = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; ADDRESSW = WRAP; MAGFILTER = ANISOTROPIC; MINFILTER = ANISOTROPIC; MIPFILTER = ANISOTROPIC; } ;

#include "Includes/Helpers.hlsl"
#include "Includes/Water.hlsl"

PS_OUTPUT main(PS_INPUT pa, float2 pixel_pos : VPOS) {
    PS_OUTPUT pr;

    float4 lin_shallow_color = linearize(ShallowColor); 
    float4 lin_deep_color = linearize(DeepColor);
    float4 lin_fog_color = linearize(FogColor);

    float3 eye_vector = EyePos.xyz - pa.LTEXCOORD_0.xyz; // vector of camera position to point being shaded
    float3 eye_direction = normalize(eye_vector);         // normalized eye to world vector (for lighting)
    float flat_dist = length(eye_vector.xy);              // surface distance to eye
    float eye_range = length(eye_vector);                    // depth distance to eye
	float3 posn = pa.WorldPosition.xyz;             // world position

	float3 light_dir = normalize(float3(0.2, 0.2, 1));
	float sun_luma = 0.3;
	float interior_refraction_modifier = TESR_WaterSettings.w;		// reduce refraction because of the way interior depth is encoded
	float interior_depth_modifier = 0.5;			// reduce depth value for fog because of the way interior depth is encoded

    // calculate fog coeffs
    float4 screen_pos = getScreenpos(pa);                // point coordinates in screen space for water surface
    float2 water_depth = tex2Dproj(DepthMap, screen_pos).xy;  // x= shallowfog, y = deepfog?
    float depth_fog = saturate(invlerp(DepthFalloff.x, DepthFalloff.y, water_depth.y));
    
    float2 faded_depth = saturate(lerp(water_depth, 1, invlerp(0, 4096, flat_dist)));

    // sample displacement and mix with the wave texture
    float3 surface_normal = getWaveTexture(pa, flat_dist, TESR_WaveParams).xyz;
    surface_normal = getDisplacement(pa, BlendRadius.w, surface_normal);

    float refraction_coeff = (water_depth.y * depth_fog) * ((saturate(flat_dist * 0.002) * (-4 + VarAmounts.w)) + 4);
    float4 reflection_pos = getReflectionSamplePosition(pa, surface_normal, refraction_coeff * interior_refraction_modifier );
    //float4 reflection = linearize(tex2Dproj(ReflectionMap, reflectionPos));
    float4 refraction_pos = reflection_pos;
    refraction_pos.y = refraction_pos.w - reflection_pos.y;
    float3 refracted_depth = tex2Dproj(DepthMap, refraction_pos).rgb * interior_depth_modifier;

    float4 pix = linearize(tex2Dproj(RefractionMap, refractionLookup(refraction_pos, TESR_RenderScale, TESR_ReciprocalResolution)));
    pix = getLightTravel(refracted_depth, lin_shallow_color, lin_deep_color, 0.5, TESR_WaterSettings, pix);
    pix = getTurbidityFog(refracted_depth, lin_shallow_color, TESR_WaterVolume, sun_luma, pix);
    //color = getDiffuse(surfaceNormal, lightDir, eyeDirection, distance, linFogColor, color);
    pix = getFresnel(surface_normal, eye_direction, lin_fog_color, TESR_WaveParams.w, pix);

	for (int k= 0; k< 12; k++){
	    pix = getPointLightSpecular(surface_normal, TESR_ShadowLightPosition[k], posn, eye_direction, TESR_LightColor[k].rgb * TESR_LightColor[k].w, pix);
	    pix = getPointLightSpecular(surface_normal, TESR_LightPosition[k], posn, eye_direction,  TESR_LightColor[ 12 + k].rgb * TESR_LightColor[ 12 + k].w, pix);
	}

    pix = getShoreFade(pa, water_depth.x, TESR_WaterShorelineParams.x, TESR_WaterVolume.y, pix);

    pix = delinearize(pix); //delinearise
    
    // Standard fog.
    float fog_strength = pow(1 - saturate((FogParam.x - eye_range) / FogParam.y), FresnelRI.y);
    
    pr.color_0.rgb = fog_strength * (FogColor.rgb - pix.rgb) + pix.rgb;
    pr.color_0.a = pix.a;
    
    return pr;
};

