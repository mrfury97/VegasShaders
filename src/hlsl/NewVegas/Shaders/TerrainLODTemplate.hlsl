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
    #define SHADOW_CLOUD_IN 1   // UNOFFICIAL 2026-11-04 ForwardCloudShadows (Includes/Shadow.hlsl): the clouds' shadow on the sun term (always compiled in; its strength switches it live)
    #define SHADOW_SUN_CONTACT_ALWAYS 1
    #define SHADOW_SUN_CONTACT_IN 1     // UNOFFICIAL 2026-11-06 forward contact shadows (Includes/Shadow.hlsl; only with Bend on at start), land registers s13 / c183-c187
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
// UNOFFICIAL 2026-11-14 full deferred renderer, phase 3 (docs/next/FD-LOD-RESULTS.md; [Main.Main.Quality] DeferredDistant, core/DeferredDistant.h):
// DISTANT_GBUFFER 1 (ShaderRecord::BuildDefines, this template only, only with the switch on at startup) = the far land writes the G-buffer as
// the near land does (TerrainTemplate.hlsl): COLOR1 its linear colour (x the detail noise), COLOR2 class 4 (land) with its normal map's normal
// (GBufferV2) and COLOR3 the face normal with the camera distance; the game's geometry group 0 (LOD land, LOD objects, distant trees) is drawn
// inside the main list's G-buffer span for it. With LAMPS_DEFERRED 2 (LightFreeRecords, the deferred renderer's mode 2) the lighting-free
// record: no sun (nor its cascades or cloud shadow), the ambient, noise and fog kept, COLOR1.a the lighting-free code (Includes/DeferredPixel.hlsl:
// the land's PBRSun, DeferredSun's land model) and COLOR2.b the roughness the land's sun takes. Without the define nothing below changes.
#ifndef DISTANT_GBUFFER
    #define DISTANT_GBUFFER 0
#endif
#if DISTANT_GBUFFER && defined(PS)
    #include "includes/CapturedNormals.hlsl"	// (needs Includes/Shadow.hlsl first: above)
    #if !CAPTURED_NORMALS_ON
        #undef DISTANT_GBUFFER
        #define DISTANT_GBUFFER 0	// (no G-buffer compiled in: the far land as before)
    #elif defined(LAMPS_DEFERRED) && LAMPS_DEFERRED == 2 && CAPTURED_NORMALS_ON >= 3	// (the code needs the albedo target: the sun is never taken out without it)
        #define NVR_LIGHT_DEFERRED
        #include "includes/DeferredPixel.hlsl"
    #endif
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
#if DISTANT_GBUFFER
    // (ps_3_0 needs COLOR0..COLOR3 contiguous; core/CapturedNormals.h binds the three only for the writers' draws in the main list's span)
    float4 capturedPad1 : COLOR1;	// AlbedoBuffer (0 below its level)
    float4 capturedPad2 : COLOR2;	// MaterialBuffer
    float4 capturedNormal : COLOR3;	// CapturedNormals
#endif
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
#ifdef NVR_LIGHT_DEFERRED
    float sunShadow = 1.0f;	// (the lighting-free record: no sun, so no lookup; DeferredSun adds the sun with its own)
#else
    float sunShadow = FORWARD_SHADOWS ? GetSunShadowMaps(pin.shadowPos, geoNormal) : 1.0f;	// (2026-11-13: the cascades alone; far land is past the RT sun shadows' range)
#endif

#if TERRAIN_MACRO
    const float3 groundColor = MacroVary(LinColor(albedo), pin.shadowPos);	// 2026-11-04 macro variation, as the near land
#else
    const float3 groundColor = LinColor(albedo);
#endif
#ifdef NVR_LIGHT_DEFERRED
    float3 lit = getSunLighting(pin.sunDir.xyz, (0.0f).xxx, viewDir, nrm.xyz, LinColor(AmbientColor.rgb), groundColor, nrm.a, LandLODSpec.x, 1.0, sunShadow, geoNormal);
#else
    float3 lit = getSunLighting(pin.sunDir.xyz, LinColor(PSLightColor.rgb), viewDir, nrm.xyz, LinColor(AmbientColor.rgb), groundColor, nrm.a, LandLODSpec.x, 1.0, sunShadow, geoNormal);
#endif

    float3 col = LinEncode(lit);	// UNOFFICIAL linear lighting: back to the stored (gamma) space, before the fog
    col = lerp(col, col * (0.8 * noiseSample + 0.55), saturate(TESR_TerrainExtraData.z)); // Apply noise.
    col = lerp(col, pin.fogTerm.rgb, pin.fogTerm.a);

    po.finalColor.rgb = col;
    po.finalColor.a = 1;
#if DISTANT_GBUFFER
    // UNOFFICIAL 2026-11-14 DeferredDistant (see the top): the far land's records, as TerrainTemplate.hlsl writes the near land's.
    // COLOR1: the linear colour the light is multiplied by. The detail noise multiplies the finished colour in the stored curve above (x m),
    // which in linear light is x dec(m): it goes into the albedo, so the light a pass adds is noised as the forward light was.
    {
        const float noiseMul = lerp(1.0f, 0.8f * noiseSample + 0.55f, saturate(TESR_TerrainExtraData.z));
        po.capturedPad1 = CapturedAlbedoOut(groundColor * LinColor(noiseMul.xxx));
    }
    #if CAPTURED_NORMALS_ON >= 4
        // GBufferV2: land (class 4), its lamps lit with a highlight as the near land's (LandLODSpec), no roughness (-1, as the near land's
        // forward record); the normal map's normal (world axes: the LOD tiles are not rotated) as the shading normal. An opaque draw: no
        // CapturedCoverage mark (z 0), so no NVR_SpecFade (c211 is not set for this pass).
        po.capturedPad2 = CapturedShadingNormalZ(float4(4.0f + GBV2_SPEC_BIT, GBV2_NO_NORMAL, -1.0f, 1.0f), nrm.xyz, 0.0f);
    #elif CAPTURED_NORMALS_ON >= 2
        po.capturedPad2 = float4(4.0f, 0.0f, -1.0f, 1.0f);	// MaterialBuffer: class 4, land (no bound radius: 0)
    #else
        po.capturedPad2 = 0.0f;
    #endif
    #ifdef NVR_LIGHT_DEFERRED
        // the lighting-free code (the land's sun is PBRSun: base 3; keep = 1 - fog), the roughness the land's sun takes ((1 - gloss) x
        // RoughnessScale: Includes/Terrain.hlsl getSunLighting), and without GBufferV2 the normal map's normal in the older deferred code
        po.capturedPad1.a = FD_LF_CODE(FD_LF_SPEC, 1.0f - pin.fogTerm.a);
        po.capturedPad2.b = saturate((1.0f - nrm.a) * TESR_TerrainData.y);
        #if CAPTURED_NORMALS_ON >= 4
        // (2026-11-16 batch D, DEFERRED-GAPS-AUDIT item 11) the far land's mark in r's fraction (Includes/DeferredPixel.hlsl
        // FD_FAR_LAND_MARK): still model 4; DeferredSun gives it no ShadowBounce, which this shader's forward record never had
        po.capturedPad2.r += FD_FAR_LAND_MARK;
        #endif
        #if CAPTURED_NORMALS_ON == 3
        {
            const float2 fdOct = ShadowEncodeNormal(normalize(nrm.xyz));
            po.capturedPad2.g = fdOct.x;
            po.capturedPad2.a = 1.01f + 0.49f * (fdOct.y + 1.0f);
        }
        #endif
    #endif
    // COLOR3: the far land has no vertex normal (its normal map is in world axes): the triangle's face normal, with the camera distance
    po.capturedNormal = CapturedNormalOut(geoNormal, pin.shadowPos);
#endif

    return po;
};

#endif	// PS
