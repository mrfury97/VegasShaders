// UNOFFICIAL: the environment-map pass ("chrome" reflections on guns, glass, metal props), replacing the game's
// SLS2050/SLS2051 vertex shaders and SLS2057/SLS2058 pixel shaders (src/effects/PBR.h). It is drawn on top of the
// object's own lighting: the object's cube map, along the reflected view, times its mask (the normal map's alpha or a
// custom mask) and the material's strength.
//
// The game's version adds that cube map at the same brightness whatever the light, with no reaction to it: a gun
// gleamed like chrome in a pitch-dark cave and looked the same at noon. Here it is first worked out exactly as the game
// did, then scaled by the light round the object ([Shaders.PBR.EnvMaps]): the ambient light and the sky along the
// reflected view, the same light the object shaders use (Object.hlsl getAmbientLighting). Lighting 0 gives the game's
// look. Where the first-person metal look is on (Object.hlsl getMetalAmbient), the metal already reflects its
// surroundings, so this pass is turned down there (WithMetal) rather than adding the reflection twice.
//
// VS: SLS2050 (and SLS2051, SKIN): the cube-space (world-aligned, camera-centred) tangent frame in TEXCOORD1-3 .xyz
//     -- tangent and binormal scaled by 0.1, as the game does, which flattens the normal map's bumps tenfold in the
//     reflection -- the view direction in their .w, the fog's visibility in TEXCOORD4.x, the vertex colour in COLOR0.
// PS: SLS2057, and SLS2058 (NEGATE_VIEW: the game's twin, which reads the view the other way round).
// Both stages must be shader model 3 together (D3D9 rejects a 2.x shader paired with a 3.0 one), so all four are
// replaced; SLS2050/2051 are the only game vertex shaders with this output layout.

#include "includes/Helpers.hlsl"	// shared code

#if defined(VS)	// vertex shader

struct VS_INPUT {	// the vertex stream
    float4 posOS : POSITION;
    float2 texc : TEXCOORD0;
    float3 normalOS : NORMAL;
    float3 tangentOS : TANGENT;
    float3 binormalOS : BINORMAL;
    float4 vcolor : COLOR0;
#ifdef SKIN	// skinned mesh
    float3 boneWeights : BLENDWEIGHT;
    float4 boneIndices : BLENDINDICES;
#endif
};

struct VS_OUTPUT {	// vertex to pixel
    float4 clipPos : POSITION;
    float2 texc : TEXCOORD0;
    float4 frameX : TEXCOORD1;   // cube-space x of axisT * 0.1, axisB * 0.1, axisN; w: toEye x
    float4 frameY : TEXCOORD2;   // ... y
    float4 frameZ : TEXCOORD3;   // ... z
    float2 fog : TEXCOORD4;      // x: 1 - fog, y: distance to the camera (the metal's fade, below)
    float3 vcolor : COLOR0;
};

#ifndef SKIN	// not: skinned mesh
    row_major float4x4 ModelViewProj : register(c0);	// object to clip space
    float4 ObjToCubeSpace[3] : register(c8);
#else
    #include "includes/SkinHelpers.hlsl"	// shared code
    #include "includes/PipboyFit.hlsl"	// UNOFFICIAL 2026-11-03 PipboyFit
    row_major float4x4 SkinModelViewProj : register(c1);	// bone space to clip space
    float4 ObjToCubeSpace[3] : register(c9);    // the game's SkinToCubeSpace
    float4 Bones[54] : register(c44);	// three rows a bone
#endif
float4 FogParam : register(c14);	// x fog far, y fog range, z fog power

VS_OUTPUT main(VS_INPUT vin) {
    VS_OUTPUT vout;
    float4 pos = vin.posOS;
    #ifndef SKIN	// not: skinned mesh
        float3 axisT = vin.tangentOS, axisB = vin.binormalOS, axisN = vin.normalOS;
        vout.clipPos = mul(ModelViewProj, pos);
    #else
        float4 boneOff = vin.boneIndices.zyxw * 765.01001;
        float4 boneW = vin.boneWeights.xyzz;
        boneW.w = 1 - weight(vin.boneWeights.xyz);
        float3x3 tbn = BonesTransformTBN(Bones, boneOff, boneW, vin.tangentOS, vin.binormalOS, vin.normalOS);
        float3 axisT = tbn[0], axisB = tbn[1], axisN = tbn[2];
        pos.w = 1;
        pos.xyz = BonesTransformPosition(Bones, boneOff, boneW, pos);
        pos.xyz = PipboyFitPosition(pos.xyz, boneOff, boneW);	// UNOFFICIAL 2026-11-03 PipboyFit (includes/PipboyFit.hlsl)
        vout.clipPos = mul(SkinModelViewProj, pos);
    #endif
    axisT *= 0.1f;
    axisB *= 0.1f;
    vout.frameX.xyz = float3(dot(axisT, ObjToCubeSpace[0].xyz), dot(axisB, ObjToCubeSpace[0].xyz), dot(axisN, ObjToCubeSpace[0].xyz));
    vout.frameY.xyz = float3(dot(axisT, ObjToCubeSpace[1].xyz), dot(axisB, ObjToCubeSpace[1].xyz), dot(axisN, ObjToCubeSpace[1].xyz));
    vout.frameZ.xyz = float3(dot(axisT, ObjToCubeSpace[2].xyz), dot(axisB, ObjToCubeSpace[2].xyz), dot(axisN, ObjToCubeSpace[2].xyz));
    // The toEye: from the vertex to the camera, which sits at cube space's origin.
    float3 toVertex = float3(dot(ObjToCubeSpace[0], pos), dot(ObjToCubeSpace[1], pos), dot(ObjToCubeSpace[2], pos));
    float3 toEye = normalize(-toVertex);
    vout.frameX.w = toEye.x;
    vout.frameY.w = toEye.y;
    vout.frameZ.w = toEye.z;
    // Fog as the object shaders work it out (ObjectTemplate.hlsl), reversed depth included; the game's own shader used
    // the clip pos as it was.
    float3 fogEye = vout.clipPos.xyz;
    #ifdef REVERSED_DEPTH	// NVR's reversed depth buffer
        fogEye.z = vout.clipPos.w - fogEye.z;
    #endif
    vout.fog.x = 1 - pow(1 - saturate((FogParam.x - length(fogEye)) / FogParam.y), FogParam.z);
    vout.fog.y = length(toVertex);
    vout.texc = vin.texc;
    vout.vcolor = vin.vcolor.rgb;
    return vout;
}

#elif defined(PS)

#include "includes/SkyAmbient.hlsl"	// shared code
// UNOFFICIAL CubeReflections: compiled in only when one of the CubeReflections settings is above 0 at startup (LIVE_CUBE from
// ShaderRecord::LoadShader), so that otherwise this pass is exactly what it was. s10 is free here (this pass reads s0, s1, s3).
#ifndef LIVE_CUBE
    #define LIVE_CUBE 0
#endif
#if LIVE_CUBE
    #include "includes/LiveCube.hlsl"
#endif

struct PS_INPUT {	// vertex to pixel
    float2 texc : TEXCOORD0;
    float4 frameX : TEXCOORD1_centroid;
    float4 frameY : TEXCOORD2_centroid;
    float4 frameZ : TEXCOORD3_centroid;
    float2 fog : TEXCOORD4;
    float3 vcolor : COLOR0;
};

sampler2D NormalMap : register(s0);	// passes without the colour texture take the normal map first
samplerCUBE EnvironmentCubeMap : register(s1);
sampler2D CustomEnvMask : register(s3);
float4 AmbientColor : register(c1);      // rgb: the ambient light, a: the material's alpha (all the game's version used)
float4 EnvToggles : register(c27);       // x: tint by the vertex colour, z: reflection strength, w: 1 = custom mask, 0 = the normal map's alpha

float4 TESR_PBRData : register(c32);     // w: AmbientScale
float4 TESR_PBRExtraData : register(c33); // y: SkylightingScale, z: SkylightingDirectionality, w: where the metal has faded out
float4 TESR_PBRMetal : register(c34);    // x: the metal look's strength for this pass (Object.hlsl)
float4 TESR_PBREnv : register(c35);      // x: Lighting (0 the game's fixed brightness, 1 by the light), y: Strength, z: WithMetal, w: Reflectivity - 1 (PBR.cpp, 0 = as before)

float4 main(PS_INPUT pin) : COLOR0 {
    // The game's reflection, as it worked it out.
    float4 nmap = tex2D(NormalMap, pin.texc);
    float3 toEye = float3(pin.frameX.w, pin.frameY.w, pin.frameZ.w);
    #ifdef NEGATE_VIEW
        toEye = -toEye;
    #endif
    toEye = normalize(toEye);
    float3 nTS = normalize((nmap.xyz - 0.5f) * 2.0f);
    float3 N = normalize(float3(dot(nTS, pin.frameX.xyz), dot(nTS, pin.frameY.xyz), dot(nTS, pin.frameZ.xyz)));
    float3 reflected = 2.0f * dot(N, toEye) * N - toEye * dot(N, N);
    float3 refl = texCUBE(EnvironmentCubeMap, reflected).rgb;
    refl *= lerp(nmap.a, tex2D(CustomEnvMask, pin.texc).x, EnvToggles.w) * EnvToggles.z * AmbientColor.a;
    refl = EnvToggles.x > 0.0f ? refl * pin.vcolor : refl;

    // UNOFFICIAL: by the light round the object -- the ambient light and the sky along the reflected view (cube space is
    // world-aligned), scaled as the object shaders scale them.
    // UNOFFICIAL SharpSky: that sky along the reflected toEye, sharper when [Shaders.PBR.Metal] SharpSky is on (SkyAmbient.hlsl).
    float3 skyReflected = SkyAmbientRadiance(normalize(reflected), TESR_PBRExtraData.z);
    [branch] if (TESR_SharpSky.x > 0.0f) skyReflected = SkyAmbientSharpened(normalize(reflected), skyReflected);
    float3 surroundings = AmbientColor.rgb * TESR_PBRData.w + skyReflected * TESR_PBRExtraData.y;
    // UNOFFICIAL 2026-10-13: never brighter than the game's own (at most 1). Indoors the ambient light can be high enough
    // that ambient x AmbientScale x Strength came to 2-3, and the reflection on the casinos' marble walls (env-mapped, with a
    // mask) was drawn at 1.5-3 times the game's: a pale sheen that the PBR-darkened walls under it showed up, sweeping across
    // them as the player moved (the user: "goes away with PBR off"). Dimmer in the dark, as before.
    refl *= min(lerp((1.0f).xxx, surroundings * TESR_PBREnv.y, TESR_PBREnv.x), 1.0f);
    #if LIVE_CUBE
    // UNOFFICIAL CubeReflections ([Shaders.PBR.EnvMaps]): what the camera actually saw round it along the reflected toEye
    // (includes/LiveCube.hlsl, already lit, so not scaled by the light again) in place of the game's painted cube map, as
    // far as the live cube has seen that direction. Through the same mask and strength.
    [branch] if (TESR_EnvCube.y > 0.0f) {
        float4 live = LiveCube(reflected, 0.1f);   // a cube read needs no unit direction
        float3 liveColor = live.rgb * (lerp(nmap.a, tex2Dlod(CustomEnvMask, float4(pin.texc, 0.0f, 0.0f)).x, EnvToggles.w) * EnvToggles.z * AmbientColor.a);
        liveColor = EnvToggles.x > 0.0f ? liveColor * pin.vcolor : liveColor;
        refl = lerp(refl, liveColor, TESR_EnvCube.y * live.a);
    }
    #endif
    // Less where the metal look already reflects the surroundings, and back in full where it has faded out with distance
    // (Object.hlsl getMetalFade, by TESR_PBRExtraData.w).
    float metalFade = TESR_PBRExtraData.w > 0.0f ? 1.0f - smoothstep(0.6f * TESR_PBRExtraData.w, TESR_PBRExtraData.w, pin.fog.y) : 1.0f;
    refl *= lerp(1.0f, TESR_PBREnv.z, saturate(TESR_PBRMetal.x) * metalFade);
    refl *= 1.0f + TESR_PBREnv.w;  // UNOFFICIAL 2026-10-25 [Shaders.PBR.Main] Reflectivity
    return float4(refl * pin.fog.x, 1.0f);
}

#endif
