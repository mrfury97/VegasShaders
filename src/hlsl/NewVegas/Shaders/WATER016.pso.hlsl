// autowater seen from below

uniform float4 EyePos : register(c1);
uniform float4 ShallowColor : register(c2);
uniform float4 DeepColor : register(c3);
uniform float4 ReflectionColor : register(c4);
uniform float4 FresnelRI : register(c5); //x: reflectamount, y:fresnel, w: opacity, z:speed
uniform float4 VarAmounts : register(c8); // x: water glossiness y: reflectivity z: refrac, w: lod
uniform float4 FogParam : register(c9);
uniform float4 FogColor : register(c10);
uniform float2 DepthFalloff : register(c11); // start / end depth fog
uniform float4 SunDir : register(c12);
uniform float4 SunColor : register(c13);
uniform float4 TESR_WaveParams : register(c14); // x: choppiness, y:wave width, z: wave speed, w: reflectivity?
uniform float4 TESR_WaterVolume : register(c15); // x: caustic strength, y:shoreFactor, w: turbidity, z: caustic strength S ?
uniform float4 TESR_WaterSettings : register(c16); // x: caustic strength, y:depthDarkness, w: turbidity, z: caustic strength S ?
uniform float4 TESR_GameTime : register(c17);
uniform float4 TESR_HorizonColor : register(c18);
uniform float4 TESR_SkyLowColor : register(c19);
uniform float4 TESR_SunDirection : register(c20);
uniform float4 TESR_ReciprocalResolution : register(c21);
uniform float4 TESR_WetWorldData : register(c22);
uniform float4 TESR_WaterShorelineParams : register(c23);
uniform float4 TESR_SunColor : register(c24);
uniform float4 TESR_CameraPosition : register(c25);
uniform float4 TESR_DebugVar : register(c26);
uniform float4 TESR_RenderScale : register(c27); // UNOFFICIAL 2026-10-27 RenderScale: xy = the internal resolution / the screen's while the world is drawn smaller (the refraction copy is made mid-scene, so its picture fills only that part), else 1

uniform sampler2D ReflectionMap : register(s0);
uniform sampler2D RefractionMap : register(s1);
uniform sampler2D NoiseMap : register(s2);
uniform sampler2D DisplacementMap : register(s3); //unused
uniform sampler2D DepthMap : register(s4);
uniform sampler2D TESR_samplerWater : register(s5) < string ResourceName = "Water\water_NRM.dds"; > = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; ADDRESSW = WRAP; MAGFILTER = ANISOTROPIC; MINFILTER = ANISOTROPIC; MIPFILTER = ANISOTROPIC; } ;
uniform sampler2D TESR_RippleSampler : register(s6) < string ResourceName = "Precipitations\ripples.dds"; > = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };

#include "Includes/Helpers.hlsl"
#include "Includes/Water.hlsl"

float4 getFresnelBelowWater(float3 surface_normal, float3 eye_direction, float4 reflection_color, float4 pix){
    float fresnel_coeff = 1 - pow(dot(eye_direction, surface_normal), 5);  // texture based fresnel
    fresnel_coeff += 0.5 * pows(1 - dot(eye_direction.xyz, float3(0, 0, -1)), 1.5);  // modulate with distance
    return lerp(pix, reflection_color, pow(saturate(fresnel_coeff), 5));
}

float4 skyColor(float3 eye_direction, float4 sun_color){
    float3 skyColor = lerp(linearize(TESR_HorizonColor), linearize(TESR_SkyLowColor), pow(abs(dot(eye_direction, float3(0, 0, 1))), 0.5)).rgb; //linearise (UNOFFICIAL 2026-10-22: abs() written out; D3D9's pow already used the absolute value, so the picture is the same)
    skyColor += sun_color.rgb * 5 * pow(shades(eye_direction, -TESR_SunDirection.xyz), 20);
    return float4(skyColor, 1);
}

PS_OUTPUT main(PS_INPUT pa) {
    PS_OUTPUT pr;

    float4 lin_sun_color = linearize(TESR_SunColor);
    float4 lin_shallow_color = linearize(ShallowColor);

    float3 eye_vector = EyePos.xyz - pa.LTEXCOORD_0.xyz; // vector of camera position to point being shaded
    float3 eye_direction = normalize(eye_vector);         // normalized eye to world vector (for lighting)
    float flat_dist = length(eye_vector.xy);              // surface distance to eye

    // calculate fog coeffs
    //float4 screenPos = getScreenpos(IN);                // point coordinates in screen space for water surface

    float sun_luma = luma(lin_sun_color.rgb);
    float exterior_refraction_modifier = TESR_WaterSettings.w;	// reduce refraction because of the way interior depth is encoded

    float3 surface_normal = getWaveTexture(pa, flat_dist, TESR_WaveParams).xyz;
    surface_normal = getRipples(pa, TESR_RippleSampler, surface_normal, flat_dist, TESR_WetWorldData.x) * -1;

    float4 refraction_pos = getReflectionSamplePosition(pa, surface_normal, exterior_refraction_modifier);
    refraction_pos.y = refraction_pos.w - refraction_pos.y;

    float4 sky_rgb = skyColor(eye_direction, lin_sun_color);
    float eye_range = TESR_WaterSettings.x - TESR_CameraPosition.z;
    float4 refr_set = linearize(tex2Dproj(RefractionMap, refractionLookup(refraction_pos, TESR_RenderScale, TESR_ReciprocalResolution))) * smoothstep(200, 0, eye_range) + sky_rgb; // mix & fade refraction with depth because vanilla refractions suck

    float4 pix = sky_rgb;
    pix = getFresnelBelowWater(surface_normal, eye_direction, lin_shallow_color * sun_luma, pix);
    pix +=  2 * pow(dot(surface_normal, eye_direction), 2) * (refr_set); // highlight

    pix = delinearize(pix); //delinearise
    
    // Standard fog.
    float fog_strength = pow(1 - saturate((FogParam.x - eye_range) / FogParam.y), FresnelRI.y);
    
    pr.color_0.rgb = fog_strength * (FogColor.rgb - pix.rgb) + pix.rgb;
    pr.color_0.a = 1;
    
    return pr;
};

