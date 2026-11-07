// Fade band where the full terrain hands over to the LOD terrain.

#if defined(__INTELLISENSE__)
    #define PS	// implied: pixel shader
#endif

#define REVERSED_DEPTH	// implied: NVR's reversed depth buffer

#include "includes/Helpers.hlsl"	// shared code
// UNOFFICIAL linear lighting (Includes/LinearLight.hlsl, [Shaders.PBR.Linear]): the far land lights in linear space while it
// is on, as TerrainTemplate.hlsl. Compiled in only when on at startup (LINEAR_LIGHTING_ON, ShaderRecord::LoadShader).
#ifndef LINEAR_LIGHTING_ON
    #define LINEAR_LIGHTING_ON 0
#endif
#if defined(PS) && LINEAR_LIGHTING_ON
    #define LINEAR_LIGHTING
#endif
#include "includes/Terrain.hlsl"	// shared code
#ifdef PS
    #define SHADOW_CLOUD_IN 1   // UNOFFICIAL 2026-11-04 ForwardCloudShadows (Includes/Shadow.hlsl): the clouds' shadow on the sun term (compiled in only when on at startup)
    #define SHADOW_SUN_CONTACT_ALWAYS 1
    #define SHADOW_SUN_CONTACT_IN 1     // UNOFFICIAL 2026-11-06 forward contact shadows (Includes/Shadow.hlsl), land registers s13 / c183-c187
    #define SHADOW_SUN_CONTACT_LAND 1
#endif
#include "includes/Shadow.hlsl"	// shared code
// UNOFFICIAL 2026-11-04 macro variation ([Shaders.Terrain.Macro], Includes/TerrainMacro.hlsl): compiled in only when on at startup.
#ifndef TERRAIN_MACRO
    #define TERRAIN_MACRO 0
#endif
#if TERRAIN_MACRO && defined(PS)
#include "includes/TerrainMacro.hlsl"
#endif


struct VS_INPUT {	// the vertex stream
    float4 vertexPos : POSITION;
    float4 texcoord : TEXCOORD0;
};

struct VS_OUTPUT {	// vertex to pixel
    float4 clipPos : POSITION;
    float2 landUV : TEXCOORD0;
    float3 shadowPos : TEXCOORD1;   // TEXCOORD1/2 unused by this template
    float3 sunDir : TEXCOORD3;
    float blendAmt : TEXCOORD4;
    float4 fogTerm : TEXCOORD5;
    float3 localPos : TEXCOORD6;
    float3 eyePos : TEXCOORD7;
};

#ifdef VS	// vertex shader

row_major float4x4 ModelViewProj : register(c0);	// object to clip space

float4 FogParam : register(c14);	// x fog far, y fog range, z fog power
float3 FogColor : register(c15);	// the game's fog colour
float4 EyePosition : register(c16);	// camera, object space
float4 LandBlendParams : register(c19);	// near / far blend range
float4 LightData : register(c25);	// the sun: xyz direction

// The game's fade constants: uv scale (1/64), its quantised form (127/128), half a texel (1/256), and where (and how fast) the
// near land fades into the far.
static const float fUVScale = 0.015625f, fUVScaleQuant = 0.9921875f, fUVOffset = 0.00390625f;
static const float fBlendBaseDistance = 9625.59961f, fBlendScale = 0.000375600968f;

VS_OUTPUT main(VS_INPUT vin) {
    VS_OUTPUT out_;

    float4 clipPos = mul(ModelViewProj, vin.vertexPos.xyzw);
    out_.clipPos.xyzw = clipPos;

    float2 scaled = (vin.texcoord.xy * fUVScale) + LandBlendParams.xy;
    scaled.x = 1 - scaled.x;
    out_.landUV.xy = (scaled.xy * fUVScaleQuant) + fUVOffset;

    out_.sunDir.xyz = LightData.xyz;

    float2 toBlendCentre = LandBlendParams.zw - vin.vertexPos.xy;
    out_.blendAmt = 1 - saturate((fBlendBaseDistance - length(toBlendCentre)) * fBlendScale);

    // Fog.
    float3 depthPos = out_.clipPos.xyz;
    #ifdef REVERSED_DEPTH	// NVR's reversed depth buffer
        depthPos.z = out_.clipPos.w - depthPos.z;
    #endif
    float fogAmount = 1 - saturate((FogParam.x - length(depthPos)) / FogParam.y);
    out_.fogTerm.rgb = FogColor.rgb;
    out_.fogTerm.a = pow(fogAmount, FogParam.z);

    out_.localPos.xyz = vin.vertexPos.xyz;
    out_.eyePos.xyz = EyePosition.xyz;
    out_.shadowPos = GetShadowWorldPos(clipPos);

    return out_;
};

#endif	// VS

struct PS_INPUT	// vertex to pixel
{
    float2 landUV : TEXCOORD0;
    float3 shadowPos : TEXCOORD1_centroid;
    float3 sunDir : TEXCOORD3_centroid;
    float blendAmt : TEXCOORD4_centroid;
    float4 fogTerm : TEXCOORD5_centroid;
    float3 localPos : TEXCOORD6_centroid;
    float3 eyePos : TEXCOORD7_centroid;
};

struct PS_OUTPUT {	// render targets
    float4 finalColor : COLOR0;
};

#ifdef PS	// pixel shader

sampler2D BaseMap : register(s0);	// colour texture; alpha for the alpha test
sampler2D NormalMap : register(s1);	// rgb normal, alpha the highlight mask (gloss)
sampler2D LODLandNoise : register(s2);	// detail noise

float4 AmbientColor : register(c1);	// rgb ambient light; a 1 = opaque, no alpha test
float4 PSLightColor : register(c3);	// the sun's colour

float4 LandLODSpec : register(c38);	// distant land specular


static const float fNoiseScale = 0.8f, fNoiseOffset = 0.55f;	// detail noise

PS_OUTPUT main(PS_INPUT pin) {
    PS_OUTPUT po;

    float3 viewDir = normalize(pin.eyePos.xyz - pin.localPos.xyz);

    float noiseSample = tex2D(LODLandNoise, pin.landUV.xy * TESR_TerrainExtraData.w).r;

    float4 nrm = tex2D(NormalMap, pin.landUV.xy);
    nrm.rgb = normalize(expand(nrm.rgb));
    nrm.a *= LandLODSpec.x > 0.0f ? 1.0f : 0.0f;

    float3 albedo = tex2D(BaseMap, pin.landUV.xy).rgb;
    // UNOFFICIAL 2026-10-19 LOD Blending: the far land's texture as TerrainLODTemplate adjusts it, so the fade band matches it.
    [branch] if (TESR_LODBlendData.w > 0.0f) albedo = pow(abs(albedo), TESR_LODBlendData.y) * TESR_LODBlendData.x;

    // Forward sun shadows. Passed as parallaxMultiplier, which getSunLighting applies to
    // the sun colour only; ambient is added afterwards and stays untouched.
    // ddx/ddy must stay at top level, outside dynamic flow control.
    float3 geoNormal = GetShadowGeometricNormal(pin.shadowPos);
    float sunShadow = FORWARD_SHADOWS ? GetSunShadow(pin.shadowPos, geoNormal) : 1.0f;

#if TERRAIN_MACRO
    const float3 groundColor = MacroVary(LinColor(albedo), pin.shadowPos);	// 2026-11-04 macro variation, as the near land
#else
    const float3 groundColor = LinColor(albedo);
#endif
    float3 lit = getSunLighting(pin.sunDir.xyz, LinColor(PSLightColor.rgb), viewDir, nrm.xyz, LinColor(AmbientColor.rgb), groundColor, nrm.a, LandLODSpec.x, 1.0, sunShadow, geoNormal);

    float3 col = LinEncode(lit);	// UNOFFICIAL linear lighting: back to the stored (gamma) space, before the fog
    col = lerp(col, col * (noiseSample * fNoiseScale + fNoiseOffset), saturate(TESR_TerrainExtraData.z)); // Apply noise.
    col = lerp(col, pin.fogTerm.rgb, pin.fogTerm.a); // Apply fog.

    po.finalColor.rgb = col;
    po.finalColor.a = pin.blendAmt;

    return po;
};

#endif	// PS
