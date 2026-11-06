// Shaders for the LOD terrain tiles.

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
    #define SHADOW_SUN_CONTACT_ALWAYS 1
    #define SHADOW_SUN_CONTACT_IN 1     // UNOFFICIAL 2026-11-06 forward contact shadows (Includes/Shadow.hlsl), land registers s13 / c183-c187
    #define SHADOW_SUN_CONTACT_LAND 1
#endif
#include "includes/Shadow.hlsl"	// shared code
#include "includes/TerrainErosion.hlsl"
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
    float morphHeight : TEXCOORD1;
};

struct VS_OUTPUT {	// vertex to pixel
    float4 fogTerm : COLOR1;
    float4 clipPos : POSITION;
    float2 landUV : TEXCOORD0;
    float3 sunDir : TEXCOORD1;
    float3 localPos : TEXCOORD2;
    float3 eyePos : TEXCOORD3;
    float3 shadowPos : TEXCOORD4;   // TEXCOORD4-7 unused by this template
};

#ifdef VS	// vertex shader

row_major float4x4 ModelViewProj : register(c0);	// object to clip space
row_major float4x4 ObjToCubeSpace : register(c8);	// object to world (cube) space

float4 HighDetailRange : register(c12);	// where the near land takes over
float4 FogParam : register(c14);	// x fog far, y fog range, z fog power
float3 FogColor : register(c15);	// the game's fog colour
float4 EyePosition : register(c16);	// camera, object space
float4 GeomorphParams : register(c19);	// LOD morph
float4 LightData : register(c25);	// the sun: xyz direction

VS_OUTPUT main(VS_INPUT vin) {
    VS_OUTPUT out_;

    float4 localPos = vin.vertexPos;
    float4 morphed = vin.vertexPos;
    morphed.z = lerp(vin.morphHeight.x, vin.vertexPos.z, GeomorphParams.x);

    float tileA = (abs(dot(ObjToCubeSpace[1].xyzw, morphed.xyzw) - HighDetailRange.y) < HighDetailRange.w ? 1.0 : 0.0);
    float tileB = (abs(dot(ObjToCubeSpace[0].xyzw, morphed.xyzw) - HighDetailRange.x) < HighDetailRange.z ? 1.0 : 0.0);

    localPos.z = morphed.z - ((tileA.x * tileB.x) * GeomorphParams.y);
    float4 clip = mul(ModelViewProj, localPos.xyzw);

    out_.clipPos.xyzw = clip.xyzw;
    out_.landUV.xy = vin.texcoord.xy;
    out_.sunDir = LightData.xyz;

    // Fog.
    float3 depthPos = out_.clipPos.xyz;
    #ifdef REVERSED_DEPTH	// NVR's reversed depth buffer
        depthPos.z = out_.clipPos.w - depthPos.z;
    #endif
    float fogAmount = 1 - saturate((FogParam.x - length(depthPos)) / FogParam.y);
    out_.fogTerm.rgb = FogColor.rgb;
    out_.fogTerm.a = pow(fogAmount, FogParam.z);

    out_.localPos.xyz = localPos.xyz;
    out_.eyePos.xyz = EyePosition.xyz;
    out_.shadowPos = GetShadowWorldPos(clip);

    return out_;
};

#endif	// VS

struct PS_INPUT	// vertex to pixel
{
    float4 fogTerm : COLOR1;
    float4 clipPos : POSITION;
    float2 landUV : TEXCOORD0;
    float3 sunDir : TEXCOORD1_centroid;
    float3 localPos : TEXCOORD2_centroid;
    float3 eyePos : TEXCOORD3_centroid;
    float3 shadowPos : TEXCOORD4_centroid;
};

struct PS_OUTPUT {	// render targets
    float4 finalColor : COLOR0;
};

#ifdef PS	// pixel shader

sampler2D BaseMap : register(s0);	// colour texture; alpha for the alpha test
sampler2D NormalMap : register(s1);	// rgb normal, alpha the highlight mask (gloss)
sampler2D LODParentTex : register(s4);	// the parent LOD tile's colour
sampler2D LODParentNormals : register(s6);	// the parent LOD tile's normals
sampler2D LODLandNoise : register(s7);	// detail noise

float4 AmbientColor : register(c1);	// rgb ambient light; a 1 = opaque, no alpha test
float4 PSLightColor : register(c3);	// the sun's colour
float4 LODTexParams : register(c31);	// LOD tile texture offset / scale

float4 LandLODSpec : register(c38);	// distant land specular


// The game's LOD tile constants: uv scale (1/128), its quantised form (127/128), half a texel (1/256), detail noise scale and offset.
static const float fUVScale = 0.0078125f, fUVScaleQuant = 0.9921875f, fUVOffset = 0.00390625f;
static const float fNoiseScale = 0.8f, fNoiseOffset = 0.55f;

PS_OUTPUT main(PS_INPUT pin) {
    PS_OUTPUT po;

    float4 nrm = tex2D(NormalMap, pin.landUV);
    float4 parentNrm = tex2D(LODParentNormals, (pin.landUV * 0.5f) + LODTexParams.xy);

    float noiseSample = tex2D(LODLandNoise, pin.landUV * TESR_TerrainExtraData.w).r;

    nrm.rgba = LODTexParams.w >= 1 ? nrm.rgba : lerp(parentNrm, nrm.rgba, LODTexParams.w);
    nrm.rgb = expand(nrm.rgb);
    nrm.rgb = normalize(nrm.rgb);
    nrm.a *= LandLODSpec.x > 0.0f ? 1.0f : 0.0f;

    float2 tileUV = (pin.landUV * fUVScaleQuant) + fUVOffset;
    float3 parentTex = tex2D(LODParentTex, (0.5f * tileUV) + lerp(LODTexParams.xy, 0.25f, fUVScale)).rgb;
    float3 albedo = tex2D(BaseMap, tileUV).rgb;

    albedo = LODTexParams.w >= 1 ? albedo : lerp(parentTex, albedo, LODTexParams.w);
    // UNOFFICIAL 2026-10-19 LOD Blending ([Shaders.Terrain.LODBlending]; after Community Shaders' LOD Blending, GPL-3.0): the far
    // land's colour by a gamma and a brightness, to match the near land. Skipped while off.
    [branch] if (TESR_LODBlendData.w > 0.0f) albedo = pow(abs(albedo), TESR_LODBlendData.y) * TESR_LODBlendData.x;

    // UNOFFICIAL erosion relief ([Shaders.Terrain.Erosion], after Physically-Based-Reloaded): the LOD normal map's slopes
    // carved with branching gullies; their floors darker. The pixel's footprint (top level: derivatives) fades the layers
    // too fine to show. Skipped while off.
    float3 dX = ddx(pin.shadowPos);
    float3 dY = ddy(pin.shadowPos);
    float footprint = max(1.0f, max(length(dX.xy), length(dY.xy)));
    [branch] if (TESR_TerrainErosionData.x > 0.0f) {
        float cavity;
        nrm.xyz = GetErodedNormal(nrm.xyz, pin.shadowPos, footprint, cavity);
        albedo *= 1.0f - cavity * TESR_TerrainErosionData.z;
    }

    float3 viewDir = normalize(pin.eyePos.xyz - pin.localPos.xyz);

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
    col = lerp(col, col * (0.8 * noiseSample + 0.55), saturate(TESR_TerrainExtraData.z)); // Apply noise.
    col = lerp(col, pin.fogTerm.rgb, pin.fogTerm.a);

    po.finalColor.rgb = col;
    po.finalColor.a = 1;

    return po;
};

#endif	// PS
