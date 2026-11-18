// wading (displacement) water surface shader

uniform float4 EyePos : register(c1);
uniform float4 ShallowColor : register(c2);
uniform float4 DeepColor : register(c3);
uniform float4 ReflectionColor : register(c4);
uniform float4 FresnelRI : register(c5);
uniform float4 BlendRadius : register(c6);
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
uniform float4 TESR_WetWorldData : register(c20);
uniform float4 TESR_WaterShorelineParams : register(c21);
uniform float4 TESR_WaterLODColor : register(c22);
uniform float4 TESR_DebugVar : register(c23);
uniform float4 TESR_SunAmount : register(c24);
uniform float4 TESR_RenderScale : register(c25); // UNOFFICIAL 2026-10-27 RenderScale: xy = the internal resolution / the screen's while the world is drawn smaller (the refraction copy is made mid-scene, so its picture fills only that part), else 1
uniform float4 TESR_ReciprocalResolution : register(c45); // UNOFFICIAL 2026-11-16: one pixel of the screen (= the refraction copy) in uv, for refractionLookup (Includes/Water.hlsl)

uniform sampler2D ReflectionMap : register(s0);
uniform sampler2D RefractionMap : register(s1);
uniform sampler2D NoiseMap : register(s2);
uniform sampler2D DisplacementMap : register(s3);
uniform sampler2D DepthMap : register(s4);
uniform sampler2D TESR_samplerWater : register(s5) < string ResourceName = "Water\water_NRM.dds"; > = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; ADDRESSW = WRAP; MAGFILTER = ANISOTROPIC; MINFILTER = ANISOTROPIC; MIPFILTER = ANISOTROPIC; } ;
uniform sampler2D TESR_RippleSampler : register(s6) < string ResourceName = "Precipitations\ripples.dds"; > = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };

// UNOFFICIAL 2026-10-31 RT water reflections (Includes/Water.hlsl getRTWaterReflection)
#define RT_WATER
uniform float4 TESR_RTWaterData : register(c40); // x 1 = last frame's RT reflection holds the water, y 1 = no planar map this frame, z strength
uniform row_major float4x4 TESR_RTWaterReproject : register(c41); // camera-relative world now -> that frame's clip space
uniform sampler2D TESR_ReflectionsBuffer : register(s7) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };

#include "Includes/Helpers.hlsl"
#include "Includes/Water.hlsl"

PS_OUTPUT main(PS_INPUT pa) {
    PS_OUTPUT pr;

    float4 lin_sun_color = linearize(SunColor);
    float4 lin_shallow_color = linearize(ShallowColor);
    float4 lin_deep_color = linearize(DeepColor);
    float4 lin_horizon_color = linearize(TESR_HorizonColor);

    float3 eye_vector = EyePos.xyz - pa.LTEXCOORD_0.xyz; // vector of camera position to point being shaded
    float3 eye_direction = normalize(eye_vector);         // normalized eye to world vector (for lighting)
    float flat_dist = length(eye_vector.xy);              // surface distance to eye
    float eye_range = length(eye_vector);                    // depth distance to eye

    // calculate fog coeffs
    float4 screen_pos = getScreenpos(pa);                // point coordinates in screen space for water surface

    float2 water_depth = tex2Dproj(DepthMap, screen_pos).xy;  // x= shallowfog, y = deepfog?
    float depth_fog = saturate(invlerp(DepthFalloff.x, DepthFalloff.y, water_depth.y));
    
    float2 faded_depth = saturate(lerp(water_depth, 1, invlerp(0, 4096, flat_dist)));

    float lodfade = saturate(smoothstep(4096,4096 * 2, flat_dist));
    float3 surface_normal = getWaveTexture(pa, flat_dist, TESR_WaveParams).xyz;
    // UNOFFICIAL LOD seam: past the fade the surface must shade exactly like the LOD water (WATER033), which has no rain
    // ripples, no depth-weighted reflection and no depth-weighted distortion; each of those is faded out with LODfade.
    float3 rippled_normal = getRipples(pa, TESR_RippleSampler, surface_normal, flat_dist, TESR_WetWorldData.x);
    surface_normal = normalize(lerp(rippled_normal, surface_normal, lodfade));
    surface_normal = getDisplacement(pa, BlendRadius.w, surface_normal);

    float is_day_time = smoothstep(0, 0.5, TESR_SunAmount.x);
    float sun_luma = luma(lin_sun_color) * is_day_time;
    float exterior_refraction_modifier = TESR_WaterSettings.w;		// reduce refraction because of the way interior depth is encoded
    float exterior_depth_modifier = 1;			// reduce depth value for fog because of the way interior depth is encoded

    float lod_refraction_coeff = (saturate(flat_dist * 0.002) * (-4 + VarAmounts.w)) + 4;
    float refraction_coeff = lerp((water_depth.y * depth_fog) * lod_refraction_coeff, lod_refraction_coeff, lodfade);
    float4 reflection_pos = getReflectionSamplePosition(pa, surface_normal, refraction_coeff * exterior_refraction_modifier);
    float4 refl_c = getRTWaterReflection(pa, surface_normal, refraction_coeff * exterior_refraction_modifier, linearize(tex2Dproj(ReflectionMap, reflection_pos)), lin_horizon_color);
    float4 refraction_pos = reflection_pos;
    refraction_pos.y = refraction_pos.w - reflection_pos.y;
    float3 refracted_depth = tex2Dproj(DepthMap, refraction_pos).rgb;

    float4 pix = linearize(tex2Dproj(RefractionMap, refractionLookup(refraction_pos, TESR_RenderScale, TESR_ReciprocalResolution)));
    pix = getLightTravel(refracted_depth, lin_shallow_color, lin_deep_color, sun_luma, TESR_WaterSettings, pix);
    pix = lerp(getTurbidityFog(refracted_depth, lin_shallow_color, TESR_WaterVolume, sun_luma, pix), linearize(TESR_WaterLODColor) * sun_luma, lodfade); // fade to full fog to hide LOD seam
    // color = getTurbidityFog(refractedDepth, linShallowColor, TESR_WaterVolume, sunLuma, color); // fade to full fog to hide LOD seam
    // color = lerp(getDiffuse(surfaceNormal, TESR_SunDirection.xyz, eyeDirection, distance, linHorizonColor, color), linShallowColor,LODfade);
    pix = lerp(pix, getFresnel(surface_normal, eye_direction, refl_c, TESR_WaveParams.w, pix), lerp(smoothstep(0, 0.2, refracted_depth.x), 1, lodfade)); // reduce fresnel in low depths (not past the LOD fade)
    pix = getSpecular(surface_normal, TESR_SunDirection.xyz, eye_direction, lin_sun_color.rgb, pix);
    pix = lerp(getShoreFade(pa, water_depth.x, TESR_WaterShorelineParams.x, TESR_WaterVolume.y, pix), pix, lodfade);

    pix = delinearize(pix); //delinearise
    
    // Standard fog.
    float fog_strength = pow(1 - saturate((FogParam.x - eye_range) / FogParam.y), FresnelRI.y);
    
    pr.color_0.rgb = fog_strength * (FogColor.rgb - pix.rgb) + pix.rgb;
    pr.color_0.a = lerp(pix.a, 1, lodfade); // fade to full opacity to hide LOD seam
    return pr;
};