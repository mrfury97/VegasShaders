// Template for PAR parallax shaders.
//
// VS
//
// AD
// PAR2008 - LIGHTS = 2, PARALLAX (AD)
// PAR2009 - LIGHTS = 2, PARALLAX, PROJ_SHADOW (AD)
// PAR2010 - LIGHTS = 3, PARALLAX (AD)
// PAR2011 - LIGHTS = 3, PARALLAX, PROJ_SHADOW (AD)
//
// ADTS
// PAR2000 - PARALLAX
// PAR2001 - PARALLAX, PROJ_SHADOW
// PAR2002 - PARALLAX, LIGHTS = 2
// PAR2003 - PARALLAX, LIGHTS = 2, PROJ_SHADOW
// PAR2004 - PARALLAX, SPECULAR
// PAR2005 - PARALLAX, SPECULAR, PROJ_SHADOW
// PAR2006 - PARALLAX, SPECULAR, LIGHTS = 2
// PAR2007 - PARALLAX, SPECULAR, LIGHTS = 2, PROJ_SHADOW
//
// Diffuse
// PAR2012 - LIGHTS = 2, PARALLAX (NO_FOG, DIFFUSE)
// PAR2013 - LIGHTS = 3, PARALLAX (NO_FOG, DIFFUSE)
//
// Specular
// PAR2015 - PARALLAX (NO_FOG, NO_VERTEX_COLOR, SPECULAR)
// PAR2016 - PROJ_SHADOW, PARALLAX (NO_FOG, NO_VERTEX_COLOR, SPECULAR)
// PAR2017 - POINT, PARALLAX (NO_FOG. NO_VERTEX_COLOR, SPECULAR)
// PAR2018 - POINT, NUM_PT_LIGHTS = 2, PARALLAX (NO_FOG, NO_VERTEX_COLOR, SPECULAR)
// PAR2019 - POINT, NUM_PT_LIGHTS = 3, PARALLAX (NO_FOG, NO_VERTEX_COLOR, SPECULAR)
//
// Texture
// PAR2014 - PARALLAX (NO_LIGHT, NO_FOG)
//
// PS
//
// AD
// PAR2013 - LIGHTS = 2, PARALLAX (AD)
// PAR2014 - LIGHTS = 2, PARALLAX, SI (AD)
// PAR2015 - LIGHTS = 2, PARALLAX, PROJ_SHADOW (AD)
// PAR2016 - LIGHTS = 2, PARALLAX, SI, PROJ_SHADOW (AD)
// PAR2017 - LIGHTS = 3, PARALLAX (AD)
// PAR2018 - LIGHTS = 3, PARALLAX, SI (AD)
// PAR2019 - LIGHTS = 3, PARALLAX, PROJ_SHADOW (AD)
// PAR2020 - LIGHTS = 3, PARALLAX, SI, PROJ_SHADOW (AD)
//
// ADTS
// PAR2000 - PARALLAX
// PAR2001 - PARALLAX, OPT
// PAR2002 - PARALLAX, SI
// PAR2003 - PARALLAX, PROJ_SHADOW
// PAR2004 - PARALLAX, SI, PROJ_SHADOW
// PAR2005 - PARALLAX, LIGHTS = 2
// PAR2006 - PARALLAX, LIGHTS = 2, SI
// PAR2007 - PARALLAX, LIGHTS = 2, PROJ_SHADOW
// PAR2008 - PARALLAX, LIGHTS = 2, SI, PROJ_SHADOW
// PAR2009 - PARALLAX, SPECULAR
// PAR2010 - PARALLAX, SPECULAR, SI
// PAR2011 - PARALLAX, SPECULAR, PROJ_SHADOW
// PAR2012 - PARALLAX, SPECULAR, SI, PROJ_SHADOW
// PAR2029 - PARALLAX, LIGHTS = 2, SPECULAR
// PAR2030 - PARALLAX, LIGHTS = 2, SPECULAR, SI
// PAR2031 - PARALLAX, LIGHTS = 2, SPECULAR, PROJ_SHADOW
// PAR2032 - PARALLAX, LIGHTS = 2, SPECULAR, SI, PROJ_SHADOW
//
// Diffuse
// PAR2021 - LIGHTS = 2, PARALLAX (NO_FOG, NO_VERTEX_COLOR, DIFFUSE, ONLY_LIGHT, OPT)
// PAR2022 - LIGHTS = 3, PARALLAX (NO_FOG, NO_VERTEX_COLOR, DIFFUSE, ONLY_LIGHT, OPT)
//
// Specular
// PAR2024 - PARALLAX (ONLY_SPECULAR)
// PAR2025 - PARALLAX, PROJ_SHADOW (ONLY_SPECULAR)
// PAR2026 - PARALLAX, POINT (ONLY_SPECULAR)
// PAR2027 - PARALLAX, NUM_PT_LIGHTS = 2, POINT (ONLY_SPECULAR)
// PAR2028 - PARALLAX, NUM_PT_LIGHTS = 3, POINT (ONLY_SPECULAR)
//
// Texture
// PAR2023 - PARALLAX (NO_LIGHT)

#ifdef __INTELLISENSE__	// editor preview: the vertex shader
    #define VS	// implied: vertex shader
    #define REVERSED_DEPTH	// as the game runs it with NVR
#endif

// The light-only passes (AD the first light pass, DIFFUSE the extra lamps, ONLY_SPECULAR the highlights) draw light alone: the
// colour pass multiplies the texture in and applies fog and vertex colour. AD and DIFFUSE use the game's OPT layout (no Toggles).
#if defined(AD) || defined(DIFFUSE) || defined(ONLY_SPECULAR)
    #define ONLY_LIGHT	// implied: light-only pass
#endif
#if defined(AD) || defined(DIFFUSE)
    #define OPT	// implied: the game's OPT layout (no Toggles)
#endif
#ifdef ONLY_SPECULAR
    #define SPECULAR	// implied: with highlights
#endif
#ifdef ONLY_LIGHT	// light-only pass
    #define NO_FOG	// (not in a light-only pass)
    #define NO_VERTEX_COLOR	// (likewise)
#endif

// Variant shorthands, worked out once from the defines the shader is built with.
#ifndef NO_LIGHT	// not: unlit variant
    #define PAR_LIT	// lit at all (NO_LIGHT: colour only)
#endif
#ifndef NO_FOG	// not: no fog
    #define PAR_FOG
#endif
#ifndef NO_VERTEX_COLOR	// not: no vertex colour
    #define PAR_VCOLOR
#endif
#ifndef OPT	// not: the game's OPT layout (no Toggles)
    #define PAR_TOGGLES	// the game's non-OPT variants read Toggles (c27)
#endif
#if !defined(DIFFUSE) && !defined(ONLY_SPECULAR)	// not: extra-lamp pass
    #define PAR_AMBIENT	// the pass adds the ambient
#endif
#if !defined(ONLY_LIGHT) && !defined(ONLY_SPECULAR) && defined(PAR_LIT)
    #define PAR_FULL	// colour and light in one pass: the height map is read
#endif
#if defined(DIFFUSE) || defined(POINT)	// extra-lamp pass
    #define PAR_LAMP_FIRST	// light 0 is a lamp, not the sun
#endif
#if LIGHTS >= 2
    #define PAR_L2	// second and third light with attenuation map coordinates
#endif
#if LIGHTS >= 3
    #define PAR_L3
#endif
#if NUM_PT_LIGHTS >= 2
    #define PAR_PT2	// second and third point light (POINT passes)
#endif
#if NUM_PT_LIGHTS >= 3
    #define PAR_PT3
#endif
#define PAR_SELF_SHADOW (TESR_ParallaxData.y)	// [Shaders.POM] parallax shadows (and the PBR path) on

#include "includes/Helpers.hlsl"	// shared code
#include "includes/Parallax.hlsl"	// shared code
// UNOFFICIAL linear lighting (Includes/LinearLight.hlsl, [Shaders.PBR.Linear]): the pixel shaders light in linear space while
// it is on (as ObjectTemplate.hlsl). Compiled in only when on at startup (LINEAR_LIGHTING_ON, ShaderRecord::LoadShader).
#ifndef LINEAR_LIGHTING_ON
    #define LINEAR_LIGHTING_ON 0
#endif
#if defined(PS) && LINEAR_LIGHTING_ON
    #define LINEAR_LIGHTING
#endif
// UNOFFICIAL 2026-10-25 pass sums for parallax objects, as ObjectTemplate.hlsl: ExactPassSums' redirect (c210 x: the pass gives its
// linear light) and LampSums' / HighlightSums' increments (c210 y: enc(dec(s) + light) - s, s the run's copy of the frame) in the
// extra-lamp (DIFFUSE) and highlight (ONLY_SPECULAR) passes. A parallax object lit by 3 or more lamps is drawn in several passes like any
// other, but FoldHighlights never folds it and these shaders had none of the sum code, so every added pass was summed in the stored
// curve: lamp-rich parallax rubble and road pieces came out lighter than the pieces next to them (tester report, Novac, 2026-10-25).
// The copy is read on s9 in the lamp passes (the sun's atlas, which they never read) and s6 in the highlight passes (free there: the
// game's normal map s0, height map s2, attenuation s3-s5 or shadow maps s4/s5). Off (not compiled): the shaders exactly as before.
#ifndef LAMP_SUMS_ON
    #define LAMP_SUMS_ON 0
#endif
#ifndef HIGHLIGHT_SUMS_ON
    #define HIGHLIGHT_SUMS_ON 0
#endif
#if defined(PS) && defined(DIFFUSE) && LAMP_SUMS_ON && LINEAR_LIGHTING_ON
    #define NVR_LAMP_SUMS
    sampler2D NVR_LampRunStart : register(s9);
#elif defined(PS) && defined(ONLY_SPECULAR) && HIGHLIGHT_SUMS_ON && LINEAR_LIGHTING_ON
    #define NVR_LAMP_SUMS
    sampler2D NVR_LampRunStart : register(s6);
#endif
// UNOFFICIAL _rmaos maps (Object.hlsl): the main lit passes only (the lamp-only and specular-only passes take s2 for the height
// map). Compiled in only when [Shaders.PBR.Rmaos] Enabled is on at startup (RMAOS_ON, ShaderRecord::LoadShader).
#ifndef RMAOS_ON
    #define RMAOS_ON 0
#endif
#if defined(PS) && RMAOS_ON && !defined(ONLY_LIGHT) && !defined(ONLY_SPECULAR) && !defined(NO_LIGHT)
    #define RMAOS
#endif
// UNOFFICIAL 2026-10-18 SmithCorrelated: as ObjectTemplate.hlsl (Includes/PBR.hlsl). Off: exactly as before.
#if defined(PS) && defined(SMITH_CORRELATED_ON) && SMITH_CORRELATED_ON
    #define PBR_SMITH_CORRELATED SMITH_CORRELATED_ON	// 1 on, 2 the test mode (PBR.hlsl)
#endif
// UNOFFICIAL 2026-10-25 SpecularFade here too (as ObjectTemplate.hlsl; Includes/PBR.hlsl PBR_SPEC_FADE, per draw NVR_SpecFade.x from the
// pass hook, core/PbrPop.h). Users' report: crater cliffs (Mesquite Mountains crater, parallax rock) flicked between shiny black and
// matte grey as the camera moved; PbrPopLog showed them swapping BSSM_ADTS_Px <-> BSSM_ADT_Px / ADT_PxOpt at the game's highlight
// distance, and the parallax shaders kept the full highlight up to the swap. Off ([Shaders.PBR.Main] SpecularFade false): as before.
#if defined(PS) && defined(SPECULAR) && defined(SPEC_FADE_ON) && SPEC_FADE_ON
    #define PBR_SPEC_FADE
#endif
// UNOFFICIAL 2026-11-05 VanillaBridge (as ObjectTemplate.hlsl; Includes/Object.hlsl): the highlight variants with Toggles. Off: as before.
#if defined(PS) && defined(VANILLA_BRIDGE_ON) && VANILLA_BRIDGE_ON && defined(SPECULAR) && defined(PAR_TOGGLES)
    #define NVR_VANILLA_BRIDGE
#endif
#include "includes/Object.hlsl"	// shared code
#ifdef PS
    #define SHADOW_CLOUD_IN 1   // UNOFFICIAL 2026-11-04 ForwardCloudShadows (Includes/Shadow.hlsl): the clouds' shadow on the sun term (compiled in only when on at startup)
#endif
#include "includes/Shadow.hlsl"	// shared code
// 2026-11-04 dust on top surfaces ([Shaders.PBR.Dust], Includes/ObjectDust.hlsl): compiled in only when on at startup (DUST_ON).
#ifndef DUST_ON
    #define DUST_ON 0
#endif
#if defined(PS) && DUST_ON
#include "includes/ObjectDust.hlsl"
#endif
// UNOFFICIAL 2026-11-04 Wetness Effects (Includes/Wetness.hlsl; as ObjectTemplate.hlsl): the sunlit passes of the material get the water
// layer. c146-c157 (the sun cross-fade's), the cover map on s12 (free outdoors).
#if defined(PS) && defined(WETNESS_EFFECTS) && !defined(WETNESS_OFF) && FORWARD_SHADOWS && !SUN_CROSSFADE && !(defined(INTERIOR_SHADOWS) && INTERIOR_SHADOWS && !(defined(POINT_SHADOW_RT_ONLY) && POINT_SHADOW_RT_ONLY)) && \
    !defined(DIFFUSE) && !defined(POINT) && !defined(ONLY_SPECULAR) && !defined(PAR_LAMP_FIRST)
    #define PARALLAX_WETNESS
    #if defined(POINT_SHADOW_RT_ONLY) && POINT_SHADOW_RT_ONLY
        // 2026-11-09 the outdoor RT lamp-shadow record (INTERIOR_SHADOWS + POINT_SHADOW_RT_ONLY, ShaderManager ExteriorForward) gets the water
        // layer too; PointShadowForward writes c174-c189 raw on every draw there (NVR_PointShadowLight + params), so the puddles' two take
        // c160 / c161 (in none of those records' tables: Bend's c160-c164 is not compiled into them; no raw write)
        #define PR_DATA c160
        #define PR_LOOK c161
    #endif
    #define WETNESS_ORTHO_REG s12
    #include "includes/Wetness.hlsl"
#endif
// UNOFFICIAL 2026-11-04 Wetness Effects' HudRaindrops (includes/HudRain.hlsl, as ObjectTemplate.hlsl; compiled in only when on at startup).
#if defined(PS) && defined(HUD_RAIN_ON) && HUD_RAIN_ON
    #define HUD_RAIN 1
#endif
#include "includes/HudRain.hlsl"
#include "includes/CapturedNormals.hlsl"	// UNOFFICIAL 2026-10-28 CapturedNormals (COLOR3, see there)
// UNOFFICIAL 2026-11-03 SmoothCutouts (Includes/AlphaHash.hlsl): the alpha test hashed under TAA / FSR / DLSS.
#if defined(PS) && defined(ALPHA_HASH_ON) && defined(PAR_FULL) && defined(PAR_TOGGLES)
    #define PAR_ALPHA_HASH 1
    #include "includes/AlphaHash.hlsl"
#endif

// Forward sun shadows -- see ObjectTemplate.hlsl. PAR shaders do full sun lighting but had
// no shadow term at all, so every parallax-material object rendered fully sunlit once the
// deferred sun was switched off.

// UNOFFICIAL 2026-10-28 object displacement ([Shaders.POM.Displacement], NVR DXVK 1.7-displace; docs/next/FORK2-DISPLACE.md "Objects"):
// with [Main.Main.Performance] NvrDxvkDisplacement on at startup under NVR DXVK (NVR_DISPLACE_OBJECTS, ShaderRecord::BuildDefines) the
// parallax objects (rocks, cliffs, walls) are drawn through Compute\Tess\ObjectDisplace.tcs / .tes, which move their surface along the
// normal by this shader's own height map. Those shaders pass the vertex shader's outputs on by location, and DXVK numbers TEXCOORD8 and
// TEXCOORD9 by their order in each shader (only NORMAL0, TEXCOORD0-7 and COLOR0-1 have fixed places), so in this layout:
//  - every vertex shader writes exactly 10 of NORMAL0, TEXCOORD0-7 and COLOR0-1 (vs_3_0 has 10 outputs besides the position), unused
//    ones as 0: all but TEXCOORD2, or all but COLOR0 in the DIFFUSE lamp passes (TEXCOORD2 is their light's attenuation). A control
//    shader for each of the two sets serves every vertex shader (DXVK checks that it reads only what the vertex shader writes);
//  - shadowUVs and shadowWorldPos move to TEXCOORD5 / TEXCOORD6 in the variants with a vertex colour or fog (those have at most 2
//    lights), else to COLOR0 / COLOR1 (the lamp and highlight passes have neither; DXVK does not clamp colours in shader model 3);
//  - NORMAL0 carries the clip position again and TEXCOORD0's zw the object normal (octahedral), for the tessellation only.
// Off (the default), or native D3D9 / stock DXVK: the shaders exactly as before.
#if defined(NVR_DISPLACE_OBJECTS) && NVR_DISPLACE_OBJECTS
    #define DISPLACE_LAYOUT
    #if !defined(NO_VERTEX_COLOR) || !defined(NO_FOG)
        #define DISPLACE_COLORS
        #if defined(PAR_L3) || defined(PAR_PT3)
            #error "displacement layout: TEXCOORD5/6 are taken by the third light"
        #endif
        #define SEM_SHADOW_UVS TEXCOORD5
        #define SEM_SHADOW_POS TEXCOORD6
    #else
        #define SEM_SHADOW_UVS COLOR0
        #define SEM_SHADOW_POS COLOR1
    #endif
#else
    #define SEM_SHADOW_UVS TEXCOORD8
    #define SEM_SHADOW_POS TEXCOORD9
#endif

struct VS_INPUT	// the vertex stream
{
    float4 vpos : POSITION;
    float3 tanV : TANGENT;
    float3 binV : BINORMAL;
    float3 nrmV : NORMAL;
    float4 texc : TEXCOORD0;
#ifdef PAR_VCOLOR
    float4 vcol : COLOR0;
#endif
};

struct VS_OUTPUT	// vertex to pixel
{
#ifdef PAR_VCOLOR
    float4 vertexColor : COLOR0;	// rgb vertex colour
#endif
#ifdef PAR_FOG
    float4 fogColor : COLOR1;	// rgb fog colour, a fog amount
#endif
    float4 scrPos : POSITION;
#ifdef DISPLACE_LAYOUT
    float4 texc : TEXCOORD0;              // xy the uv, zw the object normal (octahedral): for the tessellation only
    float4 displaceClip : NORMAL0;      // the clip position again: for the tessellation only
    // The slots this variant leaves unused, written as 0 (see the top)
    #if defined(NO_LIGHT)
        float4 displaceFill1 : TEXCOORD1;
    #endif
    #if !(LIGHTS > 1) && !(NUM_PT_LIGHTS > 1)
        float4 displaceFill3 : TEXCOORD3;
    #endif
    #if !(LIGHTS > 1)
        float4 displaceFill4 : TEXCOORD4;
    #endif
    #if !(LIGHTS > 2) && !(NUM_PT_LIGHTS > 2) && !(defined(DISPLACE_COLORS) && defined(PROJ_SHADOW))
        float4 displaceFill5 : TEXCOORD5;
    #endif
    #if !(LIGHTS > 2) && !defined(DISPLACE_COLORS)
        float4 displaceFill6 : TEXCOORD6;
    #endif
    #if defined(NO_VERTEX_COLOR) && !defined(DIFFUSE) && (defined(DISPLACE_COLORS) || !defined(PROJ_SHADOW))
        float4 displaceFill9 : COLOR0;
    #endif
    #if defined(NO_FOG) && defined(DISPLACE_COLORS)
        float4 displaceFill10 : COLOR1;
    #endif
#elif CAPTURED_NORMALS_ON
    float4 texc : TEXCOORD0;              // UNOFFICIAL 2026-10-28 CapturedNormals: zw the vertex normal, camera-relative world (octahedral)
#else
    float2 texc : TEXCOORD0;
#endif
#ifdef PAR_LIT
    float4 lightDir : TEXCOORD1;	// light 0, tangent space
    #ifdef DIFFUSE	// extra-lamp pass
        float4 lightAtt : TEXCOORD2;	// falloff texture coordinates for light 0
    #endif
#endif
#ifdef PAR_L2
    float4 light2Dir : TEXCOORD3;	// light 1, tangent space
    float4 light2Att : TEXCOORD4;	// falloff texture coordinates for light 1
#endif
#ifdef PAR_L3	// (the AD and diffuse passes only)
    float4 light3Dir : TEXCOORD5;	// light 2, tangent space
    float4 light3Att : TEXCOORD6;	// falloff texture coordinates for light 2
#endif
#ifdef PAR_PT2
    float4 light2Dir : TEXCOORD3;	// light 1, tangent space
#endif
#ifdef PAR_PT3
    float4 light3Dir : TEXCOORD5;	// light 2, tangent space
#endif
#ifdef PROJ_SHADOW	// variants with the game's projected (canopy) shadow
    float4 shadowUVs : SEM_SHADOW_UVS;
#endif
    float4 viewDir : TEXCOORD7;	// to the eye, tangent space

    // TEXCOORD0-8 are taken by this template; 9 is the first free slot. (Displacement layout: see the top.)
    float4 shadowWorldPos : SEM_SHADOW_POS;
};

#ifdef VS	// vertex shader

float4 EyePosition : register(c16);	// camera, object space
row_major float4x4 ModelViewProj : register(c0);	// object to clip space

#ifdef PAR_LIT
    float4 LightData[10] : register(c25);	// per light: xyz object space, w radius
#endif

#ifdef PAR_FOG
    float3 FogColor : register(c15);	// the game's fog colour
    float4 FogParam : register(c14);	// x fog far, y fog range, z fog power
#endif

#ifdef PROJ_SHADOW	// variants with the game's projected (canopy) shadow
    row_major float4x4 ShadowProj : register(c18);	// object space to the canopy shadow map
    float4 ShadowProjData : register(c22);	// xy origin, w size of the mask
    float4 ShadowProjTransform : register(c23);	// scale and offset of the projection
#endif

VS_OUTPUT main(VS_INPUT vin)
{
    VS_OUTPUT vo;
 
    float3x3 tbnM = float3x3(vin.tanV.xyz, vin.binV.xyz, vin.nrmV.xyz);
    
    vo.scrPos.xyzw = mul(ModelViewProj, vin.vpos.xyzw);
    vo.texc.xy = vin.texc.xy;
    
    float3 eyeV = EyePosition.xyz - vin.vpos.xyz;
    vo.viewDir.xyz = mul(tbnM, eyeV);
    vo.viewDir.w = length(eyeV);
    
    #ifdef PAR_VCOLOR
        vo.vertexColor = clamp(vin.vcol, 0.0f, 1.0f);
    #endif
    
    #ifdef PAR_LIT
        #ifndef POINT	// not: point-light pass
            vo.lightDir.w = LightData[0].w;
            #ifndef DIFFUSE	// not: extra-lamp pass
                vo.lightDir.xyz = mul(tbnM, LightData[0].xyz);
            #else
                float3 light = LightData[0].xyz - vin.vpos.xyz;
                vo.lightDir.xyz = mul(tbnM, light);
                vo.lightAtt.w = 0.5;
                vo.lightAtt.xyz = compress(light / LightData[0].w);
            #endif
            #ifdef PAR_L2
                float3 light2 = LightData[1].xyz - vin.vpos.xyz;
                vo.light2Dir.w = LightData[1].w;
                vo.light2Dir.xyz = mul(tbnM, light2);
                vo.light2Att.w = 0.5;
                vo.light2Att.xyz = compress(light2 / LightData[1].w);
            #endif
            #ifdef PAR_L3	// (the AD and diffuse passes only)
                float3 light3 = LightData[2].xyz - vin.vpos.xyz;
                vo.light3Dir.w = LightData[2].w;
                vo.light3Dir.xyz = mul(tbnM, light3);
                vo.light3Att.w = 0.5;
                vo.light3Att.xyz = compress(light3 / LightData[2].w);
            #endif
        #else
            vo.lightDir.w = LightData[0].w;
            vo.lightDir.xyz = mul(tbnM, LightData[0].xyz - vin.vpos.xyz);
            #ifdef PAR_PT2
                vo.light2Dir.w = LightData[1].w;
                vo.light2Dir.xyz = mul(tbnM, LightData[1].xyz - vin.vpos.xyz);
            #endif
            #ifdef PAR_PT3
                vo.light3Dir.w = LightData[2].w;
                vo.light3Dir.xyz = mul(tbnM, LightData[2].xyz - vin.vpos.xyz);
            #endif
        #endif
    #endif
    
    #ifdef PAR_FOG
        float3 fogP = vo.scrPos.xyz;
        #ifdef REVERSED_DEPTH	// NVR's reversed depth buffer
            fogP.z = vo.scrPos.w - fogP.z;
        #endif
        float fogAmt = 1 - saturate((FogParam.x - length(fogP)) / FogParam.y);
        fogAmt = log2(fogAmt);  // Unclear.
        vo.fogColor.a = exp2(fogAmt * FogParam.z);
        vo.fogColor.rgb = FogColor.rgb;
    #endif
    
    #ifdef PROJ_SHADOW	// variants with the game's projected (canopy) shadow
        float shadowParam = dot(ShadowProj[3].xyzw, vin.vpos.xyzw);
        float2 projUV;
        projUV.x = dot(ShadowProj[0].xyzw, vin.vpos.xyzw);
        projUV.y = dot(ShadowProj[1].xyzw, vin.vpos.xyzw);
        vo.shadowUVs.xy = ((shadowParam * ShadowProjTransform.xy) + projUV) / (shadowParam * ShadowProjTransform.w);
        vo.shadowUVs.zw = ((projUV.xy - ShadowProjData.xy) / ShadowProjData.w) * float2(1, -1) + float2(0, 1);
    #endif

    vo.shadowWorldPos = float4(GetShadowWorldPos(vo.scrPos), SHADOW_VS_SENTINEL);
    #if CAPTURED_NORMALS_ON && !defined(DISPLACE_LAYOUT)
        // UNOFFICIAL 2026-10-28 CapturedNormals: the vertex normal in camera-relative world space, in uv.zw (no interpolator is spare)
        // 2026-10-28 (user: snow on rocks in shards, "shift around in multiple layers" up close): a PLAIN vector, not octahedral. The
        // octahedral map folds at the horizon, where a rock's side faces' normals are, and a triangle with corners on both sides of the
        // fold interpolated to garbage (as Includes/Shadow.hlsl ShadowPlainNormal found for objects on 10-26). x y in uv.zw, z in
        // shadowWorldPos.w as 2 + z / 2 (1.5 .. 2.5: told apart from SHADOW_VS_SENTINEL, which it replaces here; PAR_WP_PRESENT below).
        {
            const float3 smoothN = GetShadowSmoothNormal(ModelViewProj, vin.vpos.xyz, vin.nrmV.xyz, vo.shadowWorldPos.xyz).xyz;
            vo.texc.zw = smoothN.xy;
            vo.shadowWorldPos.w = 2.0f + 0.5f * smoothN.z;
        }
    #endif

    #ifdef DISPLACE_LAYOUT
        // UNOFFICIAL 2026-10-28 object displacement (see the top): what the tessellation reads, and 0 in the unused slots.
        float3 displaceNormal = vin.nrmV.xyz * rsqrt(max(dot(vin.nrmV.xyz, vin.nrmV.xyz), 1.0e-12f));
        vo.texc = float4(vin.texc.xy, ShadowEncodeNormal(displaceNormal));
        vo.displaceClip = vo.scrPos;
        #if defined(NO_LIGHT)
            vo.displaceFill1 = 0;
        #endif
        #if !(LIGHTS > 1) && !(NUM_PT_LIGHTS > 1)
            vo.displaceFill3 = 0;
        #endif
        #if !(LIGHTS > 1)
            vo.displaceFill4 = 0;
        #endif
        #if !(LIGHTS > 2) && !(NUM_PT_LIGHTS > 2) && !(defined(DISPLACE_COLORS) && defined(PROJ_SHADOW))
            vo.displaceFill5 = 0;
        #endif
        #if !(LIGHTS > 2) && !defined(DISPLACE_COLORS)
            vo.displaceFill6 = 0;
        #endif
        #if defined(NO_VERTEX_COLOR) && !defined(DIFFUSE) && (defined(DISPLACE_COLORS) || !defined(PROJ_SHADOW))
            vo.displaceFill9 = 0;
        #endif
        #if defined(NO_FOG) && defined(DISPLACE_COLORS)
            vo.displaceFill10 = 0;
        #endif
    #endif

    return vo;
};

#endif	// VS

struct PS_INPUT	// vertex to pixel
{
#ifdef NVR_LAMP_SUMS
    float2 vpos : VPOS;	// UNOFFICIAL LampSums / HighlightSums: the pixel, to read the run's copy of the frame
#endif
#ifdef PAR_VCOLOR
    float3 vertexColor : COLOR0;	// vertex colour
#endif
#ifdef PAR_FOG
    float4 fogColor : COLOR1;	// rgb fog colour, a fog amount
#endif
#if CAPTURED_NORMALS_ON && !defined(DISPLACE_LAYOUT)
    float4 texc : TEXCOORD0;	// UNOFFICIAL 2026-10-28 CapturedNormals: zw the vertex normal (see the vertex shader)
#else
    float2 texc : TEXCOORD0;
#endif
#ifdef PAR_LIT
    float4 lightDir : TEXCOORD1_centroid;	// light 0, tangent space
#endif
#ifdef DIFFUSE	// extra-lamp pass
    float4 lightAtt : TEXCOORD2;	// falloff texture coordinates for light 0
#endif
#ifdef PAR_L2
    float4 light2Dir : TEXCOORD3_centroid;	// light 1, tangent space
    float4 light2Att : TEXCOORD4;	// falloff texture coordinates for light 1
#endif
#ifdef PAR_L3	// (the AD and diffuse passes only)
    float4 light3Dir : TEXCOORD5_centroid;	// light 2, tangent space
    float4 light3Att : TEXCOORD6;	// falloff texture coordinates for light 2
#endif
#ifdef PAR_PT2
    float4 light2Dir : TEXCOORD3_centroid;	// light 1, tangent space
#endif
#ifdef PAR_PT3
    float4 light3Dir : TEXCOORD5_centroid;	// light 2, tangent space
#endif
#ifdef PROJ_SHADOW	// variants with the game's projected (canopy) shadow
    float4 shadowUVs : SEM_SHADOW_UVS;
#endif
    float4 viewDir : TEXCOORD7_centroid;	// to the eye, tangent space
    float4 shadowWorldPos : SEM_SHADOW_POS;
};

// UNOFFICIAL 2026-10-28 CapturedNormals: the smooth vertex normal as a plain vector (x y in uv.zw, z in shadowWorldPos.w = 2 + z / 2, see the
// vertex shader), and the vertex shader's marker on shadowWorldPos.w: the sentinel, or that range.
#if CAPTURED_NORMALS_ON && !defined(DISPLACE_LAYOUT)
    #define PAR_SMOOTH_NORMAL float3(pin.texc.zw, (pin.shadowWorldPos.w - 2.0f) * 2.0f)
    #define PAR_WP_PRESENT(w) (SHADOW_VS_PRESENT(w) || ((w) > 1.499f && (w) < 2.501f))
#else
    #define PAR_WP_PRESENT(w) SHADOW_VS_PRESENT(w)
#endif

struct PS_OUTPUT {	// render targets
    float4 color : COLOR0;
#if CAPTURED_NORMALS_ON
    // (ps_3_0 needs COLOR0..COLOR3 contiguous: COLOR1 / COLOR2 written as 0, discarded where nothing is bound there)
    float4 capturedPad1 : COLOR1;
    float4 capturedPad2 : COLOR2;
    float4 capturedNormal : COLOR3;	// UNOFFICIAL 2026-10-28 CapturedNormals (Includes/CapturedNormals.hlsl; bound only in the world's main list)
#endif
};

#ifdef PS	// pixel shader


#ifdef PAR_AMBIENT
    #if !defined(NO_LIGHT) && defined(LINEAR_LIGHTING)
        float4 AmbientColorIn : register(c1);
        static float4 AmbientColor;	// UNOFFICIAL linear lighting: AmbientColorIn decoded (rgb), at the start of main
    #elif !defined(NO_LIGHT)
        float4 AmbientColor : register(c1);	// rgb ambient light; a 1 = opaque, no alpha test
    #endif
    sampler2D BaseMap : register(s0);	// colour texture; alpha for the alpha test
#endif
#ifdef PAR_LIT
    #ifdef PAR_AMBIENT
        sampler2D NormalMap : register(s1);	// rgb normal, alpha the highlight mask (gloss)
    #else
        sampler2D NormalMap : register(s0);	// passes without the colour texture take the normal map first
    #endif
#endif
#ifdef PAR_FULL
    sampler2D HeightMap : register(s3);	// parallax height (red)
#else
    sampler2D HeightMap : register(s2);	// parallax height (red)
#endif
#ifdef SI	// self-illuminated: glow map
float4 EmittanceColor : register(c2);	// glow or hair colour
    #ifndef ONLY_LIGHT	// not: light-only pass
        sampler2D GlowMap : register(s4);	// glow (SI) or hair tint (HAIR) map
    #else
        sampler2D GlowMap : register(s3);	// glow (SI) or hair tint (HAIR) map
    #endif
#endif
#ifdef PAR_L2
    #ifdef DIFFUSE	// extra-lamp pass
        sampler2D AttenuationMap : register(s3);	// the game's lamp falloff texture
    #elif defined(ONLY_LIGHT)	// light-only pass
        sampler2D AttenuationMap : register(s4);	// the game's lamp falloff texture
    #else
        sampler2D AttenuationMap : register(s5);	// the game's lamp falloff texture
    #endif
#endif
#ifdef PROJ_SHADOW	// variants with the game's projected (canopy) shadow
    #ifdef ONLY_SPECULAR
        sampler2D ShadowMap : register(s4);	// the game's projected (canopy) shadow
        sampler2D ShadowMaskMap : register(s5);	// where that shadow applies
    #elif defined(ONLY_LIGHT)	// light-only pass
        sampler2D ShadowMap : register(s5);	// the game's projected (canopy) shadow
        sampler2D ShadowMaskMap : register(s6);	// where that shadow applies
    #else
        sampler2D ShadowMap : register(s6);	// the game's projected (canopy) shadow
        sampler2D ShadowMaskMap : register(s7);	// where that shadow applies
    #endif
#endif
#if !defined(NO_LIGHT) && defined(LINEAR_LIGHTING)
    float4 PSLightColorIn[10] : register(c3);
    static float4 PSLightColor[10];	// UNOFFICIAL linear lighting: PSLightColorIn decoded (rgb; .a as it is), at the start of main
#elif !defined(NO_LIGHT)
    float4 PSLightColor[10] : register(c3);	// rgb per light (sun first where there is one)
#endif
#ifdef PAR_TOGGLES
    float4 Toggles : register(c27);	// x vertex colour on, y fog on, z gloss power, w alpha test reference

    #define useVertexColor Toggles.x	// > 0: multiply the colour by the vertex colour
    #define useFog Toggles.y	// > 0: apply the fog
    #define glossPower Toggles.z	// the game's highlight exponent
    #define alphaTestRef Toggles.w	// alpha test reference
#else
    #define glossPower 1	// (no OPT parallax variant has highlights)
#endif

#define uvtile(t) (((t) * 0.04) - 0.02)	// 0..1 tile weight to a -0.02..0.02 uv nudge

#if defined(PAR_ALPHA_HASH) && !defined(NVR_LAMP_SUMS)
PS_OUTPUT main(PS_INPUT pin ALPHA_HASH_VPOS_PARAM)	// UNOFFICIAL 2026-11-03 SmoothCutouts: the pixel, for the noise
    #define PAR_HASH_VPOS ALPHA_HASH_VPOS_VALUE
#else
PS_OUTPUT main(PS_INPUT pin)
    #if defined(PAR_ALPHA_HASH)
        #define PAR_HASH_VPOS pin.vpos	// LampSums has it already
    #endif
#endif
{
    PS_OUTPUT po;

#if defined(LINEAR_LIGHTING) && !defined(NO_LIGHT)
    // UNOFFICIAL linear litSum: the light colours decoded.
    [unroll] for (int lightIndex = 0; lightIndex < 10; lightIndex++)
        PSLightColor[lightIndex] = float4(LinColor(PSLightColorIn[lightIndex].rgb), PSLightColorIn[lightIndex].a);
    #ifdef PAR_AMBIENT
        AmbientColor = float4(LinColor(AmbientColorIn.rgb), AmbientColorIn.a);
    #endif
#endif
    
    #ifdef PAR_FULL
        float texAlpha = tex2D(BaseMap, pin.texc.xy).a;
    
        #ifdef PAR_TOGGLES
            #ifdef PAR_ALPHA_HASH
                texAlpha = AmbientColor.a >= 1 ? texAlpha : AlphaHash(texAlpha, alphaTestRef, PAR_HASH_VPOS);	// UNOFFICIAL 2026-11-03 SmoothCutouts: 1 / -1 on a hashed draw
            #endif
            clip(AmbientColor.a >= 1 ? 0 : (texAlpha - alphaTestRef));
        #endif
    #endif
    
    // Parallax.
    float3 viewDir = normalize(pin.viewDir.xyz);
    float viewDist = pin.viewDir.w;
    
    float2 gx, gy;
    gx = ddx(pin.texc.xy);
    gy = ddy(pin.texc.xy);
    
    float2 offsetUV = getParallaxCoords(viewDist, pin.texc.xy, gx, gy, viewDir.xyz, HeightMap);
    // UNOFFICIAL 2026-11-04: rain drops (includes/HudRain.hlsl), placed on the surface itself: the relief flattened under them.
    HudRain(pin.texc.xy, pin.shadowWorldPos.xyz, PAR_WP_PRESENT(pin.shadowWorldPos.w));
    offsetUV = lerp(offsetUV, pin.texc.xy, hudRainStrength) + hudRainOffset;

    #ifdef PAR_AMBIENT
        float4 albedoPx = tex2D(BaseMap, offsetUV.xy);
        #ifdef PARALLAX_WETNESS
            const float3 wetDryTexture = albedoPx.rgb;	// UNOFFICIAL 2026-11-04 Wetness Effects: the colour a light-only pass's light is multiplied by later
        #endif

        #ifdef ONLY_LIGHT	// light-only pass
            albedoPx.rgb = 1.f;
        #endif
    #else
        float4 albedoPx = 1.f;
        #ifdef PARALLAX_WETNESS
            const float3 wetDryTexture = 1.0f;
        #endif
    #endif
    
    // Vertex color.
    #ifdef PAR_VCOLOR
        #ifdef PAR_TOGGLES
            // Apply vertex color if toggled.
            albedoPx.xyz = (useVertexColor <= 0.0 ? albedoPx.xyz : (albedoPx.xyz * pin.vertexColor.rgb));
        #else
            albedoPx.xyz = albedoPx.xyz * pin.vertexColor.rgb;
        #endif
    #endif
    albedoPx.rgb = LinColor(HudRainAlbedo(albedoPx.rgb));	// UNOFFICIAL linear litSum: the albedo decoded for the litSum (darker under the rain drops)
    
    // Shadows.
    float3 sunAndProj = 1.0;
    #ifdef PROJ_SHADOW	// variants with the game's projected (canopy) shadow
        float3 projShade = tex2D(ShadowMap, pin.shadowUVs.xy).xyz;
        float projMask = tex2D(ShadowMaskMap, pin.shadowUVs.zw).x;
        sunAndProj = lerp(1, projShade, projMask);
    #endif
    
    #ifdef PAR_LIT
        sunAndProj *= getParallaxShadowMultipler(viewDist, offsetUV, gx, gy, normalize(pin.lightDir.xyz), HeightMap);
    #endif

    // Forward sun shadows, folded into sunAndProj -- which scales PSLightColor[0]
    // (the sun) only, leaving ambient untouched. Skipped for DIFFUSE/POINT passes, which
    // carry a point light in slot 0 rather than the sun.
    // ddx/ddy must stay at top level, outside dynamic flow control.
    // Hoisted out of the projShade block below: the ambient term also needs it, and that runs for
    // DIFFUSE/POINT passes which the projShade block skips. ddx/ddy must stay at top level.
#if defined(CAPTURED_NORMALS_FORWARD) && CAPTURED_NORMALS_FORWARD && CAPTURED_NORMALS_ON && !defined(DISPLACE_LAYOUT)
    // UNOFFICIAL 2026-10-28 [Main.Main.Quality] CapturedNormals: the smooth vertex nmap the vertex shader now sends in uv.zw, instead of
    // each triangle's flat one (rocks, cliffs and walls: sun-projShade steps and ambient facets per triangle). The displacement layout has
    // no channel for it and keeps the flat one.
    float3 sunShadowNormal = ShadowFacingNormal(PAR_SMOOTH_NORMAL, pin.shadowWorldPos.xyz);
#else
    float3 sunShadowNormal = GetShadowGeometricNormal(pin.shadowWorldPos.xyz);
#endif
#if DUST_ON && !defined(ONLY_LIGHT) && !defined(ONLY_SPECULAR) && !defined(NO_LIGHT)
    // 2026-11-04 dust on top surfaces (Includes/ObjectDust.hlsl): rocks and cliffs, on the decoded colour, before any lighting
    albedoPx.rgb = DustCover(albedoPx.rgb, sunShadowNormal, pin.shadowWorldPos.xyz, PAR_WP_PRESENT(pin.shadowWorldPos.w));
#endif
    float shadowWorldPosValid = PAR_WP_PRESENT(pin.shadowWorldPos.w) ? 1.0f : 0.0f;

    #if defined(PAR_LIT) && !defined(PAR_LAMP_FIRST)
        #if FORWARD_SHADOWS	// NVR sun shadows in the object pass
        sunAndProj *= shadowWorldPosValid
                          ? GetSunShadow(pin.shadowWorldPos.xyz, sunShadowNormal)
                          : 1.0f;
        #endif
    #endif
    
    // Lighting.
    float3 litSum;
    float attOut;
    
    #ifndef PAR_LIT
        litSum = albedoPx.rgb;
    #else
        float4 nmap = tex2D(NormalMap, offsetUV.xy);
        nmap.xyz = HudRainNormal(normalize(expand(nmap.xyz)));	// UNOFFICIAL: the rain drops' tilt

        #ifdef RMAOS
            RmaosRead(offsetUV.xy);	// UNOFFICIAL _rmaos map (Object.hlsl)
        #endif
        #ifdef NVR_VANILLA_BRIDGE
            BridgeSetup(nmap.a, glossPower);	// UNOFFICIAL 2026-11-05 VanillaBridge (Object.hlsl)
        #endif
        float rough = HudRainRoughness(getRoughness(nmap.a));	// UNOFFICIAL: wet under the rain drops
        SetDiffuseEnergyKeep(nmap.xyz, pin.viewDir.xyz, rough);	// UNOFFICIAL 2026-10-16 EnergySplit (Includes/PBR.hlsl): once per pixel, every lamp reuses it

        #ifdef PARALLAX_WETNESS	// UNOFFICIAL 2026-11-04 Wetness Effects: the albedo darkened before any light sees it (as ObjectTemplate.hlsl)
            Wetness wet = (Wetness)0;
            wet.roughness = 1.0f;
            wet.normal = sunShadowNormal;
            [branch] if (shadowWorldPosValid > 0.0f) wet = GetWetness(pin.shadowWorldPos.xyz, sunShadowNormal, sunShadowNormal);
            #ifdef PUDDLES_COMPILED	// 2026-11-08 Puddles: standing water hides the normal map (the sun, lamps and ambient see a flat surface)
                nmap.xyz = normalize(lerp(nmap.xyz, float3(0.0f, 0.0f, 1.0f), wet.flatten));
            #endif
            const float3 wetDry = max(LinColor(wetDryTexture), 1.0f / 256.0f);
            #ifdef ONLY_LIGHT
                albedoPx.rgb *= WetAlbedo(wetDry, wet.darkening, 1.0f) / wetDry;
            #else
                albedoPx.rgb = WetAlbedo(albedoPx.rgb, wet.darkening, 1.0f);
            #endif
        #endif

        #ifndef PAR_LAMP_FIRST
            if (PAR_SELF_SHADOW)
                litSum = getSunLighting(pin.lightDir.xyz, PSLightColor[0].rgb * sunAndProj, pin.viewDir.xyz, nmap.xyz, albedoPx.rgb, rough);
            else
                litSum = getVanillaLightingAtt(pin.lightDir.xyz, 1.f, PSLightColor[0].rgb * sunAndProj, pin.viewDir.xyz, nmap.xyz, albedoPx.rgb, nmap.a, glossPower);
        #elif defined(DIFFUSE)	// extra-lamp pass
            // Pointlight vanilla att.
            if (PAR_SELF_SHADOW)
                litSum = getPointLightLighting(pin.lightDir.xyz, pin.lightDir.w, PSLightColor[0].rgb, pin.viewDir.xyz, nmap.xyz, albedoPx.rgb, rough);
            else {
                attOut = saturate(1 - tex2D(AttenuationMap, pin.lightAtt.xy).x - tex2D(AttenuationMap, pin.lightAtt.zw).x);
                litSum = getVanillaLightingAtt(pin.lightDir.xyz, attOut, PSLightColor[0].rgb, pin.viewDir.xyz, nmap.xyz, albedoPx.rgb, nmap.a, glossPower);
            }
        #else
            if (PAR_SELF_SHADOW)
                litSum = getPointLightLighting(pin.lightDir.xyz, pin.lightDir.w, PSLightColor[0].rgb, pin.viewDir.xyz, nmap.xyz, albedoPx.rgb, rough);
            else
                litSum = getVanillaLighting(pin.lightDir.xyz, pin.lightDir.w, PSLightColor[0].rgb, pin.viewDir.xyz, nmap.xyz, albedoPx.rgb, nmap.a, glossPower);
        #endif
    
        // Self emmitance.
        #ifdef SI	// self-illuminated: glow map
            float3 glow = tex2D(GlowMap, pin.texc.xy).rgb;
            litSum += albedoPx.rgb * LinColor(glow.rgb * EmittanceColor.rgb);
        #endif
    
        #ifdef PAR_AMBIENT
            if (PAR_SELF_SHADOW)
                litSum += getAmbientLighting(AmbientColor.rgb, albedoPx.rgb, sunShadowNormal, shadowWorldPosValid);
            else
                litSum += albedoPx.rgb * AmbientColor.rgb;
        #endif
        #ifdef PARALLAX_WETNESS	// the water film: the sun's highlight on it and its reflection, over the material
            [branch] if (wet.gloss > 0.001f) {
                float3 wetV = -normalize(pin.shadowWorldPos.xyz);
                float3 film = WetSpecular(wet.normal, wetV, TESR_WetnessSun.xyz, PSLightColor[0].rgb * sunAndProj * LIN_SCALE_LIGHT, wet.roughness) * smoothstep(0.0f, 0.2f, wet.gloss);
                float3 wetR = reflect(-wetV, wet.normal);
                float wetHorizon = saturate(1.0f + 1.2f * dot(wetR, sunShadowNormal));
                film += WetSkyReflection(wetR) * WetReflectance(saturate(dot(wet.normal, wetV)), wet.roughness, wet.gloss) * wetHorizon * wetHorizon;
                #ifdef ONLY_LIGHT
                    film /= wetDry;	// the texture pass multiplies this pass's light by the texture
                #endif
                litSum += film;
            }
        #endif
    
        // Other light sources.
        #ifdef PAR_L2
            attOut = saturate(1 - tex2D(AttenuationMap, pin.light2Att.xy).x - tex2D(AttenuationMap, pin.light2Att.zw).x);
        
            if (PAR_SELF_SHADOW)
                litSum += getPointLightLightingAtt(pin.light2Dir.xyz, attOut, PSLightColor[1].rgb, pin.viewDir.xyz, nmap.xyz, albedoPx.rgb, rough);
            else
                litSum += getVanillaLightingAtt(pin.light2Dir.xyz, attOut, PSLightColor[1].rgb, pin.viewDir.xyz, nmap.xyz, albedoPx.rgb, nmap.a, glossPower);
        #endif
    
        #ifdef PAR_L3	// (the AD and diffuse passes only)
            attOut = saturate(1 - tex2D(AttenuationMap, pin.light3Att.xy).x - tex2D(AttenuationMap, pin.light3Att.zw).x);
        
            if (PAR_SELF_SHADOW)
                litSum += getPointLightLightingAtt(pin.light3Dir.xyz, attOut, PSLightColor[2].rgb, pin.viewDir.xyz, nmap.xyz, albedoPx.rgb, rough);
            else
                litSum += getVanillaLightingAtt(pin.light3Dir.xyz, attOut, PSLightColor[2].rgb, pin.viewDir.xyz, nmap.xyz, albedoPx.rgb, nmap.a, glossPower);
        #endif
    
        #ifdef PAR_PT2
            if (PAR_SELF_SHADOW)
                litSum += getPointLightLighting(pin.light2Dir.xyz, pin.light2Dir.w, PSLightColor[1].rgb, pin.viewDir.xyz, nmap.xyz, albedoPx.rgb, rough);
            else
                litSum += getVanillaLighting(pin.light2Dir.xyz, pin.light2Dir.w, PSLightColor[1].rgb, pin.viewDir.xyz, nmap.xyz, albedoPx.rgb, nmap.a, glossPower);
        #endif
    
        #ifdef PAR_PT3
            if (PAR_SELF_SHADOW)
                litSum += getPointLightLighting(pin.light3Dir.xyz, pin.light3Dir.w, PSLightColor[2].rgb, pin.viewDir.xyz, nmap.xyz, albedoPx.rgb, rough);
            else
                litSum += getVanillaLighting(pin.light3Dir.xyz, pin.light3Dir.w, PSLightColor[2].rgb, pin.viewDir.xyz, nmap.xyz, albedoPx.rgb, nmap.a, glossPower);
        #endif
    #endif
    
    float3 outCol = LinEncode(litSum.rgb);	// UNOFFICIAL linear litSum: back to the stored (gamma) space, before the fog
    #ifdef ONLY_LIGHT	// light-only pass
        // UNOFFICIAL 2026-10-25 ExactPassSums (core/PassSums.h): while NVR redirects this run of added passes into its own buffer, the pass
        // gives its LINEAR light (as ObjectTemplate.hlsl).
        [flatten] if (TESR_PassRedirect.x > 0.5f) outCol = max(litSum.rgb, 0.0f);
    #endif
    #ifdef NVR_LAMP_SUMS
        // UNOFFICIAL 2026-10-25 LampSums / HighlightSums (see the top): c210 y 1 during the world's run of these passes, zw 1 / the frame's size.
        [branch] if (TESR_PassRedirect.y > 0.5f) {
            const float3 runStart = max(tex2Dlod(NVR_LampRunStart, float4((pin.vpos + 0.5f) * TESR_PassRedirect.zw, 0.0f, 0.0f)).rgb, 0.0f);
            outCol = LinEncode(LinColor(runStart) + max(litSum.rgb, 0.0f)) - runStart;
        }
    #endif
    litSum.rgb = outCol;
    litSum.rgb = RmaosDebug(litSum.rgb, albedoPx.rgb);	// UNOFFICIAL [Shaders.PBR.Rmaos] DebugView

    // Fog.
    #ifdef PAR_FOG
        #ifdef PAR_TOGGLES
            litSum.rgb = (useFog <= 0.0 ? litSum.rgb : lerp(litSum.rgb, pin.fogColor.rgb, pin.fogColor.a));
        #else
            litSum.rgb = lerp(litSum.rgb, pin.fogColor.rgb, pin.fogColor.a);
        #endif
    #endif
    
    po.color.rgb = litSum.rgb;
    
    #if defined(DIFFUSE) || !defined(PAR_LIT)
        po.color.a = 1;
    #elif defined(ONLY_SPECULAR)	// highlight-only pass
        if (!PAR_SELF_SHADOW)
            po.color.rgb = saturate(po.color.rgb);
        po.color.a = weight(litSum.rgb);
    #elif defined(ONLY_LIGHT)	// light-only pass
        po.color.a = albedoPx.a;
    #else
        po.color.a = ReflMaterialAlpha(texAlpha * AmbientColor.a);	// UNOFFICIAL MaterialAware reflections (Object.hlsl)
    #endif
    #if defined(PARALLAX_WETNESS) && defined(PUDDLES_COMPILED)
        [branch] if (PuddleDebugColor.x >= 0.0f) po.color.rgb = PuddleDebugColor;	// 2026-11-08 [Shaders.Puddles.Debug] DebugView
    #endif
    po.color.rgb = LinearSceneOut(po.color.rgb);	// UNOFFICIAL 2026-10-28 LinearScene (includes/LinearLight.hlsl)
#if CAPTURED_NORMALS_ON
    // UNOFFICIAL 2026-10-31 AlbedoBuffer (Includes/CapturedNormals.hlsl): the linear colour the light is multiplied by (NO_LIGHT: the texture itself);
    // the lamp and highlight passes have no texture of their own (unwritten)
    #if defined(DIFFUSE) || defined(ONLY_SPECULAR) || defined(ONLY_LIGHT)
        po.capturedPad1 = CAPTURED_ALBEDO_NONE;
    #else
        po.capturedPad1 = CapturedAlbedoOut(albedoPx.rgb);
    #endif
    #ifndef PAR_LIT
        po.capturedPad2 = CapturedMaterialOut(-1.0f);	// UNOFFICIAL 2026-10-29 MaterialBuffer (0 without it; no litSum, no rough)
    #else
        po.capturedPad2 = CapturedMaterialOut(rough);	// UNOFFICIAL 2026-10-31: the rough it was lit with (no metal path here)
    #endif
    // UNOFFICIAL 2026-10-28 CapturedNormals: the smooth vertex nmap, before the nmap map and the parallax offset. The displacement
    // layout (NVR DXVK object tessellation) has no channel for it (uv.zw is the object nmap there): none, the depth-rebuilt nmap stays.
    #if defined(DISPLACE_LAYOUT) || defined(DIFFUSE) || defined(ONLY_SPECULAR)
        po.capturedNormal = CAPTURED_NORMAL_INVALID;
    #else
        po.capturedNormal = CapturedNormalOut(PAR_SMOOTH_NORMAL, pin.shadowWorldPos.xyz);
    #endif
#endif

    return po;
};

#endif	// PS