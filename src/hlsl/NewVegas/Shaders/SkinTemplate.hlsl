// The skin shader (UNOFFICIAL 2026-10-25: rewritten on this build's own stack; see Includes/Skin.hlsl for the lighting and
// where each model comes from). Every vanilla SKIN20xx vertex and pixel shader is built from this one file
// (SkinShaders::Templates() in src/effects/Skin.h), the way ObjectTemplate.hlsl builds the object shaders. It takes
//   - the sun's shadows from Includes/Shadow.hlsl (cascades, PCF / contact hardening, the character shadow map, the
//     smooth vertex normal and the skin's edge softness and reach: SHADOW_SKIN_SOFTNESS),
//   - the lamps' shadows indoors from Includes/PointShadowForward.hlsl (INTERIOR_SHADOWS records),
//   - the lamps' falloff from Includes/LampFalloff.hlsl, linear lighting from Includes/LinearLight.hlsl,
//   - rain drops on the first-person arms (and, AllObjects, on anyone out in the rain) from Includes/HudRain.hlsl (Wetness Effects HudRaindrops),
//   - and the screen-space skin blur's inputs (Effects/SkinScattering.fx.hlsl): COLOR1 / COLOR2 below.
//
// The engine's side, read off the vanilla shaders (shaderpackage010.sdp); only this has to match them:
//   VS constants: ModelViewProj c0 / SkinModelViewProj c1, FogParam c14, FogColor c15, EyePosition c16, ShadowProj c18-c23,
//   LightData c25+ (OBJECT space: [0] the sun's direction, then each lamp's position + radius; in the DIFFUSE family every
//   entry is a lamp), Bones c44-c97.
//   PS constants: AmbientColor c1, EmittanceColor c2 (.a = the live light count in the MANY family), PSLightColor c3+,
//   Toggles c27 (x vertex colour, y fog, w alpha test; not in the AD and DIFFUSE families).
//   Samplers: full passes s0 base, s1 normal, s2 / s3 the FaceGen maps, s6 / s7 the projected actor shadow; light-only
//   passes s0 base, s1 normal, s5 / s6 that shadow; DIFFUSE passes s0 the normal map alone.
//
// Variants (defines, from the map in Skin.h):
//   SKIN         bone-skinned geometry
//   POINTS=n     lamps after the sun (DIFFUSE: lamps in all)
//   PROJ_SHADOW  vanilla's projected actor shadow (never with more than 2 lamps)
//   ONLY_LIGHT   the AD pass: writes light only (the ambient included); the engine multiplies the texture in afterwards.
//                No fog, vertex colour or Toggles. The highlights are divided by that texture in advance (the FaceGen blend
//                on faces, which the SkinShader hook binds: NVR_SkinDraw.y), so they come out right after the multiply.
//   DIFFUSE      an added lamp-only pass. No ambient, base texture, fog or Toggles (and no texture to divide the highlights
//                by: a lamp-only pass's highlights come out tinted by the texture, as the old skin's did).
//   MANY         the many-light family (4 lamps): EmittanceColor.a says how many are live; the rest are skipped, not
//                multiplied by 0 (an unused slot can hold anything).
//
// Direct lighting is done in TANGENT space, as vanilla's is: the vertex shader takes the light and view vectors into the
// vertex's tangent frame, so nothing depends on the projection (the engine draws the first-person arms with their own
// camera; the world camera's inverse matrices only serve the sky, the shadows and the curvature, where a skew costs
// little). The world frame (the tangent, binormal and the smooth vertex normal in camera-relative world space) is rebuilt
// the way Shadow.hlsl rebuilds the position, so it is right for whatever camera drew the vertex.
//
// VS -> PS (all ten of ps_3_0's inputs in the largest variants):
//   TEXCOORD0  uv.xy, the sun's tangent-space direction .xy    TEXCOORD4  camera-relative world position, view.z
//   TEXCOORD1  world tangent, sun .z                           TEXCOORD5  lamp 1, lamp 4 .x
//   TEXCOORD2  world binormal, view.x (tangent space)          TEXCOORD6  lamp 2, lamp 4 .y
//   TEXCOORD3  world normal, view.y                            TEXCOORD7  lamp 3, lamp 4 .z, or the PROJ_SHADOW uvs
//   COLOR0/1   vertex colour, fog (the full and MANY families)
// A lamp's tangent-space vector divided by its radius gives both its direction and vanilla's falloff, 1 - |v|^2, exactly
// (the vector is rescaled to its object-space length: a skinned tangent frame can be sheared).
// Every SKIN vertex shader name is built from this file, so the pixel shader assumes its own vertex shader ran.

#ifdef __INTELLISENSE__	// editor preview: a 2-lamp pixel shader
    #define PS	// implied: pixel shader
    #define POINTS 2
#endif

#ifndef POINTS
    #define POINTS 0	// lamps carried per pass
#endif

// Light-only passes (ONLY_LIGHT, and the extra-lamp DIFFUSE passes, which are light-only too) use the game's OPT layout and get
// their colour and fog from a later pass; the full pass carries the vertex colour and fog (FOG_COLOR).
#if defined(DIFFUSE) && !defined(ONLY_LIGHT)
    #define ONLY_LIGHT	// implied: light-only pass
#endif
#ifdef ONLY_LIGHT	// light-only pass
    #define OPT	// implied: the game's OPT layout (no Toggles)
#else
    #define FOG_COLOR
#endif

#include "includes/Helpers.hlsl"	// shared code
// The camera matrices Shadow.hlsl rebuilds positions with move out of c100-c107 in the vertex shaders: the game's bone
// upload reaches past Bones[54] (c97) and overwrote them in skinned ones (see Shadow.hlsl).
#ifdef VS	// vertex shader
    #define SHADOW_INVPROJ_REG c240
    #define SHADOW_INVVIEW_REG c244
#endif
#define SHADOW_SKIN_SOFTNESS 1   // CharacterShadowSoftness and CharacterShadowReach (Includes/Shadow.hlsl): skin only
// No sun cross-fade on skin (SunSmoothing CrossFade): a second full cascade lookup takes these shaders to 4,900-5,500 slots with
// PCF and contact hardening, past what D3D9 drivers create (about 3,600), and the game would then draw skin with its own shaders.
// A sun step shows as one small shadow move on skin instead of a fade; the ground and objects keep their fade.
#undef SUN_CROSSFADE
#define SUN_CROSSFADE 0
#ifdef PS	// pixel shader
    #define SHADOW_CHARACTER_MAP_IN 1   // CharacterShadowMap (Includes/Shadow.hlsl CharacterMapShadow; compiled in only when on at startup)
    // TESR_SkinDebug (Includes/Skin.hlsl) declared here so Shadow.hlsl can read its LegacyFaceShadows switch (z): with it, the
    // CharacterShadowReach cap is the old one along the sun's ray (SHADOW_REACH_ALONG_RAY, Includes/Shadow.hlsl).
    float4 TESR_SkinDebug : register(c195);
    #define SKIN_DEBUG_DECLARED
    #define SHADOW_REACH_ALONG_RAY (TESR_SkinDebug.z < 0.5f)
    // SmithCorrelated ([Shaders.PBR.Main], restart): the same masking term as the object shaders (Includes/PBR.hlsl BRDF).
    #if defined(SMITH_CORRELATED_ON) && SMITH_CORRELATED_ON
        #define PBR_SMITH_CORRELATED 1
    #endif
#endif
#ifdef PS
    #define SHADOW_CLOUD_IN 1   // UNOFFICIAL 2026-11-04 ForwardCloudShadows (Includes/Shadow.hlsl): the clouds' shadow on the sun term (compiled in only when on at startup)
#endif
#include "includes/Shadow.hlsl"	// shared code
#define CAPTURED_ACTOR	// UNOFFICIAL 2026-10-28: every skin draw is an actor's (CapturedNormals alpha 2)
#include "includes/CapturedNormals.hlsl"	// UNOFFICIAL 2026-10-28 CapturedNormals (COLOR3, see there)


#ifdef VS	// vertex shader

#include "includes/SkinHelpers.hlsl"	// shared code

struct VS_INPUT {	// the vertex stream
    float4 posOS : POSITION;
    float3 tangentOS : TANGENT;
    float3 binormalOS : BINORMAL;
    float3 normalOS : NORMAL;
    float2 texc : TEXCOORD0;
#ifdef FOG_COLOR
    float4 vcolor : COLOR0;
#endif
#ifdef SKIN	// skinned mesh
    float3 boneWeights : BLENDWEIGHT;
    float4 boneIndices : BLENDINDICES;
#endif
};

struct VS_OUTPUT {	// vertex to pixel
    float4 clipPos : POSITION;
    float4 texc : TEXCOORD0;
    float4 frameT : TEXCOORD1;
    float4 frameB : TEXCOORD2;
    float4 frameN : TEXCOORD3;
    float4 worldPos : TEXCOORD4;
#if POINTS > 0
    float4 light1 : TEXCOORD5;
#endif
#if POINTS > 1
    float4 light2 : TEXCOORD6;
#endif
#if POINTS > 2
    float4 light3 : TEXCOORD7;
#elif defined(PROJ_SHADOW)	// variants with the game's projected (canopy) shadow
    float4 projUVs : TEXCOORD7;
#endif
#ifdef FOG_COLOR
    float4 vcolor : COLOR0;
    float4 fogOut : COLOR1;
#endif
};

#ifndef SKIN	// not: skinned mesh
    row_major float4x4 ModelViewProj : register(c0);	// object to clip space
    #define MVP ModelViewProj
#else
    row_major float4x4 SkinModelViewProj : register(c1);	// bone space to clip space
    float4 Bones[54] : register(c44);	// three rows a bone
    #define MVP SkinModelViewProj
#endif

#ifdef FOG_COLOR
    float4 FogParam : register(c14);	// x fog far, y fog range, z fog power
    float3 FogColor : register(c15);	// the game's fog colour
#endif

#ifdef PROJ_SHADOW	// variants with the game's projected (canopy) shadow
    row_major float4x4 ShadowProj : register(c18);	// object space to the canopy shadow map
    float4 ShadowProjData : register(c22);	// xy origin, w size of the mask
    float4 ShadowProjTransform : register(c23);	// scale and offset of the projection
#endif

float4 EyePosition : register(c16);	// camera, object space
float4 LightData[10] : register(c25);	// per light: xyz object space, w radius

#ifdef DIFFUSE	// extra-lamp pass
    #define FIRST_POINT 0
#else
    #define FIRST_POINT 1
#endif

// Lamp i's tangent-space vector divided by its radius, at its object-space length.
float3 LampVector(int i, float3 pos, float3x3 tbn) {
    const float4 lamp = LightData[FIRST_POINT + i];
    const float3 v = (lamp.xyz - pos) / lamp.w;
    const float3 t = mul(tbn, v);
    return t * sqrt(dot(v, v) / max(dot(t, t), 1e-12f));
}

VS_OUTPUT main(VS_INPUT vin) {
    VS_OUTPUT vout;

    float4 pos = float4(vin.posOS.xyz, 1.0f);
    #ifndef SKIN	// not: skinned mesh
        const float3x3 tbn = float3x3(normalize(vin.tangentOS), normalize(vin.binormalOS), normalize(vin.normalOS));
    #else
        const float4 boneOff = vin.boneIndices.zyxw * 765.01001;
        float4 boneW = vin.boneWeights.xyzz;
        boneW.w = 1 - weight(vin.boneWeights.xyz);
        const float3x3 tbn = BonesTransformTBN(Bones, boneOff, boneW, vin.tangentOS, vin.binormalOS, vin.normalOS);
        pos.xyz = BonesTransformPosition(Bones, boneOff, boneW, pos);
    #endif

    vout.clipPos = mul(MVP, pos);

    // The world frame for the sky and the shadows (Shadow.hlsl's round trip through the draw's own projection), the
    // tangent-space view and sun for the direct light.
    const float3 worldPos = GetShadowWorldPos(vout.clipPos);
    const float3 viewTS = mul(tbn, EyePosition.xyz - pos.xyz);
    vout.frameT = float4(GetShadowSmoothNormal(MVP, pos.xyz, tbn[0], worldPos).xyz, 0.0f);
    vout.frameB = float4(GetShadowSmoothNormal(MVP, pos.xyz, tbn[1], worldPos).xyz, viewTS.x);
    vout.frameN = float4(GetShadowSmoothNormal(MVP, pos.xyz, tbn[2], worldPos).xyz, viewTS.y);
    vout.worldPos = float4(worldPos, viewTS.z);

    #ifndef DIFFUSE	// not: extra-lamp pass
        const float3 sunTS = normalize(mul(tbn, LightData[0].xyz));
    #else
        const float3 sunTS = 0.0f;
    #endif
    vout.texc = float4(vin.texc.xy, sunTS.xy);
    vout.frameT.w = sunTS.z;

    float3 lamp4 = 0.0f;
    #if POINTS > 3
        lamp4 = LampVector(3, pos.xyz, tbn);
    #endif
    #if POINTS > 0
        vout.light1 = float4(LampVector(0, pos.xyz, tbn), lamp4.x);
    #endif
    #if POINTS > 1
        vout.light2 = float4(LampVector(1, pos.xyz, tbn), lamp4.y);
    #endif
    #if POINTS > 2
        vout.light3 = float4(LampVector(2, pos.xyz, tbn), lamp4.z);
    #elif defined(PROJ_SHADOW)
        const float projW = dot(ShadowProj[3].xyzw, pos.xyzw);
        float2 projUV;
        projUV.x = dot(ShadowProj[0].xyzw, pos.xyzw);
        projUV.y = dot(ShadowProj[1].xyzw, pos.xyzw);
        vout.projUVs.xy = ((projW * ShadowProjTransform.xy) + projUV) / (projW * ShadowProjTransform.w);
        vout.projUVs.zw = ((projUV.xy - ShadowProjData.xy) / ShadowProjData.w) * float2(1, -1) + float2(0, 1);
    #endif

    #ifdef FOG_COLOR
        // Vanilla's fog, from the clip-space distance.
        const float fogLeft = saturate((FogParam.x - length(vout.clipPos.xyz)) / FogParam.y);
        vout.fogOut = float4(FogColor, pow(1.0f - fogLeft, FogParam.z));
        vout.vcolor = vin.vcolor;
    #endif

    return vout;
}

#endif	// vertex shader

#ifdef PS	// pixel shader

// UNOFFICIAL 2026-11-15 DeferredSkin (core/DeferredSkin.h, Includes/SkinDeferred.hlsl; [Main.Main.Quality] DeferredSkin, test only): the skin's
// deferred record, the full (ADTS) and many-lamp (ADTS10) families only. LAMPS_DEFERRED 2: the sun and every lamp compiled out (DeferredSun and
// DeferredLamps add them with this file's own SkinLight); 3: the lamps out, the sun kept (DeferredRenderer 1). The ambient, the sky light and
// reflection and the fog stay; COLOR1-3 hold G-buffer v2 (see SkinDeferred.hlsl) instead of the skin blur's targets. Never made otherwise:
// every other record compiles exactly as before (SKIN_DEFERRED 0).
#if defined(LAMPS_DEFERRED) && LAMPS_DEFERRED >= 2
    #if defined(ONLY_LIGHT) || !defined(CAPTURED_NORMALS_ON) || CAPTURED_NORMALS_ON < 4
        #error DeferredSkin: a deferred skin record is made for the full and many-lamp families with GBufferV2 only
    #endif
    #define SKIN_DEFERRED LAMPS_DEFERRED
    #include "includes/DeferredPixel.hlsl"
    #include "includes/SkinDeferred.hlsl"
#else
    #define SKIN_DEFERRED 0
#endif

#include "includes/Skin.hlsl"
// The lamps' shadows indoors (INTERIOR_SHADOWS records): lamp k of the draw is PSLightColor[k], looked up once at the top
// into ptShadow<k> (POINT_SHADOW_SCALAR: plain numbers, the debug views' tint applied where they are used).
#define POINT_SHADOW_SCALAR
#include "includes/PointShadowForward.hlsl"
// Rain on the first-person arms (Wet World HudDrops), compiled in only when on at startup.
#if defined(HUD_RAIN_ON) && HUD_RAIN_ON
    #define HUD_RAIN 1
#endif
#include "includes/HudRain.hlsl"

// The draw's light index of lamp 0-3 (literal numbers: PointShadowForward.hlsl pastes them into names).
#ifdef DIFFUSE	// extra-lamp pass
    #define FIRST_POINT 0
    #define SKIN_LAMP0 0
    #define SKIN_LAMP1 1
    #define SKIN_LAMP2 2
    #define SKIN_LAMP3 3
#else
    #define FIRST_POINT 1
    #define SKIN_LAMP0 1
    #define SKIN_LAMP1 2
    #define SKIN_LAMP2 3
    #define SKIN_LAMP3 4
#endif
// Two levels, so SKIN_LAMPn becomes its number before it is pasted: ptShadow<k> = lamp k's shadow, d its vector / radius.
#define SKIN_LAMP_SET(k, d) SKIN_LAMP_SET_(k, d)
#define SKIN_LAMP_SET_(k, d) ptShadow##k = POINT_SHADOW_LOOKUP(k, saturate(1.0f - dot(d, d)))
#define SKIN_LAMP_SHADOW(k) POINT_SHADOW(k, 1.0f)
// UNOFFICIAL 2026-10-30 ExteriorForward: how many of the draw's lamps get a shadow lookup (all; the outdoor record's fallback levels
// ask for fewer when the sun's code beside them runs out of temp registers: SKIN2011, X4505)
#ifndef SKIN_PT_LAMPS
    #define SKIN_PT_LAMPS POINTS
#endif

struct PS_INPUT {	// vertex to pixel
    float4 uv : TEXCOORD0;	// xy texture coordinates
    float4 tangent : TEXCOORD1_centroid;
    float4 binormal : TEXCOORD2_centroid;
    float4 normal : TEXCOORD3_centroid;
    float4 worldPos : TEXCOORD4;
#if POINTS > 0
    float4 light1 : TEXCOORD5_centroid;
#endif
#if POINTS > 1
    float4 light2 : TEXCOORD6_centroid;
#endif
#if POINTS > 2
    float4 light3 : TEXCOORD7_centroid;
#elif defined(PROJ_SHADOW)
    float4 shadowUVs : TEXCOORD7;	// xy canopy shadow map, zw its mask
#endif
#ifdef FOG_COLOR
    float4 color : COLOR0;
    float4 fog : COLOR1;	// rgb fog colour, a fog amount
#endif
};

// Vanilla's sampler slots per family.
#ifdef DIFFUSE	// extra-lamp pass
    sampler2D NormalMap : register(s0);	// passes without the colour texture take the normal map first
#elif defined(ONLY_LIGHT)	// light-only pass
    sampler2D BaseMap : register(s0);	// colour texture; alpha for the alpha test
    sampler2D NormalMap : register(s1);	// rgb normal, alpha the highlight mask (gloss)
    sampler2D FaceGenMap0 : register(s2);   // bound by the SkinShader hook on faces (NVR_SkinDraw.y)
    sampler2D FaceGenMap1 : register(s3);	// FaceGen detail (age / complexion)
    #ifdef PROJ_SHADOW	// variants with the game's projected (canopy) shadow
        sampler2D ShadowMap : register(s5);	// the game's projected (canopy) shadow
        sampler2D ShadowMaskMap : register(s6);	// where that shadow applies
    #endif
#else
    sampler2D BaseMap : register(s0);	// colour texture; alpha for the alpha test
    sampler2D NormalMap : register(s1);	// rgb normal, alpha the highlight mask (gloss)
    sampler2D FaceGenMap0 : register(s2);	// FaceGen detail (face tint)
    sampler2D FaceGenMap1 : register(s3);	// FaceGen detail (age / complexion)
    #ifdef PROJ_SHADOW	// variants with the game's projected (canopy) shadow
        sampler2D ShadowMap : register(s6);	// the game's projected (canopy) shadow
        sampler2D ShadowMaskMap : register(s7);	// where that shadow applies
    #endif
#endif

float4 AmbientColor : register(c1);	// rgb ambient light; a 1 = opaque, no alpha test
float4 EmittanceColor : register(c2);	// glow or hair colour
float4 PSLightColor[10] : register(c3);	// rgb per light (sun first where there is one)
#ifndef OPT	// not: the game's OPT layout (no Toggles)
    float4 Toggles : register(c27);   // x vertex colour, y fog, z shine (unused: the roughness is the setting's), w alpha test ref
#endif

// Lamp i: d its tangent-space vector / radius, shadow its lamp shadow (SKIN_LAMP_SHADOW, 1 without one). Vanilla's falloff
// 1 - |d|^2 through LampAttOther (the objects' decode or the physical shape, Includes/LampFalloff.hlsl), applied to the
// decoded light as the object shaders apply theirs.
float3 SkinLamp(int i, float3 d, float3 shadow, float3 albedo, float3 N, float3 Nsoft, float3 V, out float3 specular) {
    const float att = LampAttOther(saturate(1.0f - dot(d, d)));
    const float3 light = PBRLight(PSLightColor[FIRST_POINT + i].rgb) * att;
    float3 lamp = 0.0f;
    specular = 0.0f;
    #ifdef MANY
        // Only the live lamps (EmittanceColor.a), and only within reach: the branch also keeps the compiler from
        // interleaving all four lamps, which overflows ps_3_0's 32 temporaries.
        [branch] if ((FIRST_POINT + i) < EmittanceColor.a && att > 0.0f)
    #endif
        lamp = SkinLight(albedo, N, Nsoft, normalize(d).z, V, d, light, 1.0f, false, specular);
    specular *= shadow;
    return lamp * shadow;
}

// [Shaders.Skin.Debug] DebugView 1-7, in place of the colour this pass writes:
//   1 which pass drew the skin: green one full pass (the sun and up to one lamp), cyan the many-lamp full pass, yellow a
//     light-only pass the engine multiplies by the texture afterwards, red added for every extra lamp pass on top
//   2 which settings are in use: blue Interiors, orange outdoors; bright where the screen-space blur takes this skin, dark
//     where the per-pixel diffusion stands in
//   3 direct light (sun and lamps, with the diffusion and transmission) on white skin
//   4 ambient and sky light on white skin
//   5 highlights alone: the lights' lobes and the sky reflection
//   6 material: red roughness, green highlight strength (the mask x SpecularStrength), blue wetness
//   7 curvature: red the per-pixel diffusion (0 where the blur takes over), green thin features (transmission)
// The light-only passes are divided by the texture the engine multiplies them by afterwards, so every view reads true.
float3 SkinDebugView(float view, float3 direct, float3 ambient, float3 highlights, float3 albedo, float3 adAlbedo) {
    #ifdef DIFFUSE
        if (view < 1.5f) return float3(0.5f, 0.0f, 0.0f);
        if (view > 2.5f && view < 3.5f) return LinEncode(direct);
        if (view > 4.5f && view < 5.5f) return LinEncode(highlights);
        return 0.0f.xxx;
    #else
        const float3 onWhite = 1.0f / max(albedo, 0.02f);
        float3 c;
        if (view < 1.5f) {
            #ifdef ONLY_LIGHT	// light-only pass
                c = float3(1.0f, 1.0f, 0.0f);
            #elif defined(MANY)
                c = float3(0.0f, 1.0f, 1.0f);
            #else
                c = float3(0.0f, 1.0f, 0.0f);
            #endif
        }
        else if (view < 2.5f) {
            c = TESR_SkinDebug.y > 0.0f ? float3(0.2f, 0.45f, 1.0f) : float3(1.0f, 0.55f, 0.1f);
            c *= NVR_SkinDraw.x > 0.0f ? 1.0f : 0.3f;
        }
        else if (view < 3.5f) c = LinEncode(direct * onWhite);
        else if (view < 4.5f) c = LinEncode(ambient * onWhite);
        else if (view < 5.5f) c = LinEncode(highlights);
        else if (view < 6.5f) c = float3(skinRoughness, saturate(skinSpecScale * 0.5f), saturate(TESR_SkinLook.y));
        else c = float3(skinCurvature, skinThinness, 0.0f);
        #ifdef ONLY_LIGHT	// light-only pass
            c /= max(adAlbedo, 0.05f);
        #endif
        return c;
    #endif
}

// COLOR1 and COLOR2 go to the screen-space blur's targets, bound only during skin draws in the world scene
// (src/effects/SkinScattering.h); anywhere else they are discarded.
//   COLOR1 rgb  the highlights' share of COLOR0 (gamma, fogged): taken out before the blur, put back after
//          a    the camera distance, which the blur checks against the depth buffer
//   COLOR2 rgb  the colour the light is multiplied by, divided out before the blur so only light diffuses
//          a    the luminance of the colour this draw leaves in the scene: anything drawn over the skin afterwards changes it
struct PS_OUTPUT {	// render targets
    float4 color : COLOR0;
    float4 scatter : COLOR1;
    float4 albedo : COLOR2;
#if CAPTURED_NORMALS_ON
    float4 capturedNormal : COLOR3;	// UNOFFICIAL 2026-10-28 CapturedNormals (Includes/CapturedNormals.hlsl; bound only in the world's main list)
#endif
};

PS_OUTPUT main(PS_INPUT pin) {
    const float3 worldPos = pin.worldPos.xyz;

    // Rain on the first-person arms (Includes/HudRain.hlsl): the textures are read under the drops.
    HudRain(pin.uv.xy, worldPos, true);
    const float2 uv = HudRainUV(pin.uv.xy);

    // --- the lamps' shadows first, while little else is held (nothing without INTERIOR_SHADOWS; nothing in a deferred record: its lamps are DeferredLamps')
    #if INTERIOR_SHADOWS && POINTS > 0 && !SKIN_DEFERRED
        const float3 ptWorldPos = worldPos;
        const float3 ptNormal = ShadowFacingNormal(pin.normal.xyz, worldPos);   // the smooth vertex normal, for the lookup's offset
        const float ptValid = 1.0f;
        float ptShadow0 = 1.0f, ptShadow1 = 1.0f, ptShadow2 = 1.0f, ptShadow3 = 1.0f, ptShadow4 = 1.0f;
        [branch] if (NVR_PointShadowParams.x > 0.0f) {
            #if POINTS > 0 && SKIN_PT_LAMPS > 0
                SKIN_LAMP_SET(SKIN_LAMP0, pin.light1.xyz);
            #endif
            #if POINTS > 1 && SKIN_PT_LAMPS > 1
                SKIN_LAMP_SET(SKIN_LAMP1, pin.light2.xyz);
            #endif
            #if POINTS > 2 && SKIN_PT_LAMPS > 2
                SKIN_LAMP_SET(SKIN_LAMP2, pin.light3.xyz);
            #endif
            #if POINTS > 3 && SKIN_PT_LAMPS > 3
                SKIN_LAMP_SET(SKIN_LAMP3, float3(pin.light1.w, pin.light2.w, pin.light3.w));
            #endif
        }
    #endif

    // --- the material
    #ifdef DIFFUSE
        float4 texColor = 1.0f;
        const float3 adAlbedo = 1.0f;   // no texture to divide by
    #elif defined(ONLY_LIGHT)	// light-only pass
        float4 texColor = HUD_RAIN_TEX(BaseMap, pin.uv.xy);
        // What the engine multiplies this pass by afterwards: the base texture, or on faces the FaceGen blend its texture
        // pass applies, the same as the full pass's (the maps are bound on faces only; derivatives taken out here: none are
        // allowed inside the branch).
        float3 adAlbedo = texColor.rgb;
        const float2 uvDx = ddx(pin.uv.xy);
        const float2 uvDy = ddy(pin.uv.xy);
        [branch] if (NVR_SkinDraw.y > 0.0f)
            adAlbedo = 2.0f * ((expand(tex2Dgrad(FaceGenMap0, uv, uvDx, uvDy).rgb) + texColor.rgb) * (2.0f * tex2Dgrad(FaceGenMap1, uv, uvDx, uvDy).rgb));
        texColor.rgb = 1.0f;
    #else
        const float4 baseTexture = HUD_RAIN_TEX(BaseMap, pin.uv.xy);
        clip(AmbientColor.a >= 1 ? 0 : (baseTexture.a - Toggles.w));

        const float3 faceGenVar = HUD_RAIN_TEX(FaceGenMap0, pin.uv.xy).rgb;
        const float3 faceGenBlend = HUD_RAIN_TEX(FaceGenMap1, pin.uv.xy).rgb;
        float4 texColor = float4(2.0f * ((expand(faceGenVar) + baseTexture.rgb) * (2.0f * faceGenBlend)), baseTexture.a);
        texColor.rgb = Toggles.x <= 0.0f ? texColor.rgb : texColor.rgb * pin.color.rgb;
        const float3 adAlbedo = 1.0f;
    #endif

    // --- normals: tangent space for the direct light, world for the sky
    const float4 normalTexel = HUD_RAIN_TEX(NormalMap, pin.uv.xy);
    const float3 N = HudRainNormal(normalize(expand(normalTexel.xyz)));
    const float3 Nsoft = normalize(expand(tex2Dbias(NormalMap, float4(uv, 0.0f, SKIN_SOFT_NORMAL_BIAS)).xyz));
    const float3 V = normalize(float3(pin.binormal.w, pin.normal.w, pin.worldPos.w));

    SkinSetup(N, pin.normal.xyz, worldPos, normalTexel.a, NVR_SkinDraw.x);
    skinRoughness = HudRainRoughness(skinRoughness);   // wet: smooth, and shining at least as a full mask would
    #if HUD_RAIN
        skinSpecScale = lerp(skinSpecScale, max(skinSpecScale, 1.0f), hudRainGloss);
    #endif
    const float3 albedo = LinColor(HudRainAlbedo(texColor.rgb));   // darker under the rain drops

    // --- the sun's shadow (top level: derivatives), and vanilla's projected actor shadow (not in a lighting-free record: DeferredSun's)
    #if !defined(DIFFUSE) && SKIN_DEFERRED != 2
        const float3 sunTS = float3(pin.uv.zw, pin.tangent.w);
        float sunVis = 1.0f;
        #if FORWARD_SHADOWS	// NVR sun shadows in the object pass
            #if OBJECT_SMOOTH_NORMALS
                const float3 flatNormal = ShadowFacingNormal(pin.normal.xyz, worldPos);   // SmoothObjectNormals: no per-triangle normal is built
            #else
                const float3 flatNormal = GetShadowGeometricNormal(worldPos);
            #endif
            const float3 shadowNormal = ShadowLookupNormal(flatNormal, float4(pin.normal.xyz, SHADOW_VS_SENTINEL));   // SmoothCharacterShadows, and the skin's edge softness
            sunVis = GetSunShadow(worldPos, shadowNormal);
        #endif
        float3 projShadow = 1.0f;
        #ifdef PROJ_SHADOW	// variants with the game's projected (canopy) shadow
            projShadow = lerp(1.0f, tex2D(ShadowMap, pin.shadowUVs.xy).rgb, tex2D(ShadowMaskMap, pin.shadowUVs.zw).x);
        #endif
    #endif

    // --- the lights: diffuse and highlights kept apart (see PS_OUTPUT)
    float3 diffuseLight = 0.0f;
    float3 specularLight = 0.0f;
    float3 ambientLight = 0.0f;   // kept apart for the debug views
    float3 specular;

    #if !defined(DIFFUSE) && SKIN_DEFERRED != 2
        const float3 sunLight = PBRLight(PSLightColor[0].rgb) * projShadow;
        diffuseLight += SkinLight(albedo, N, Nsoft, sunTS.z, V, sunTS, sunLight, sunVis, true, specular);
        specularLight += specular;
    #endif

    #if !SKIN_DEFERRED	// (a deferred record's lamps: DeferredLamps')
    #if POINTS > 0
        diffuseLight += SkinLamp(0, pin.light1.xyz, SKIN_LAMP_SHADOW(SKIN_LAMP0), albedo, N, Nsoft, V, specular);
        specularLight += specular;
    #endif
    #if POINTS > 1
        diffuseLight += SkinLamp(1, pin.light2.xyz, SKIN_LAMP_SHADOW(SKIN_LAMP1), albedo, N, Nsoft, V, specular);
        specularLight += specular;
    #endif
    #if POINTS > 2
        diffuseLight += SkinLamp(2, pin.light3.xyz, SKIN_LAMP_SHADOW(SKIN_LAMP2), albedo, N, Nsoft, V, specular);
        specularLight += specular;
    #endif
    #if POINTS > 3
        diffuseLight += SkinLamp(3, float3(pin.light1.w, pin.light2.w, pin.light3.w), SKIN_LAMP_SHADOW(SKIN_LAMP3), albedo, N, Nsoft, V, specular);
        specularLight += specular;
    #endif
    #endif

    // --- the sky and the ambient (the reflection first: it sets the share of the sky it takes)
    #ifndef DIFFUSE
        // Built here, not at the top: nine values held through every light overflow ps_3_0's 32 temporaries in the
        // many-light variants.
        const float3x3 worldFrame = float3x3(normalize(pin.tangent.xyz), normalize(pin.binormal.xyz), normalize(pin.normal.xyz));
        const float3 worldN = normalize(mul(N, worldFrame));
        specularLight += SkinSkyReflection(-normalize(worldPos), worldFrame[2], worldN);
        ambientLight = SkinAmbient(AmbientColor.rgb, albedo, worldN, normalize(mul(Nsoft, worldFrame)));
        diffuseLight += ambientLight;
    #endif

    // --- out. The light-only passes are multiplied by the texture afterwards: their highlights are divided by it here.
    const float3 compensation = 1.0f / max(LinColor(adAlbedo), 0.125f);
    float4 outColor;
    outColor.rgb = LinEncode(diffuseLight + specularLight * compensation);

    // The highlights' share of the colour once the pass is done, for the blur (only while its targets are bound).
    float3 specularShare = 0.0f;
    [branch] if (NVR_SkinDraw.x > 0.0f) {
        float3 finalDiffuse = diffuseLight;
        #if defined(ONLY_LIGHT) && !defined(DIFFUSE)
            finalDiffuse *= LinColor(adAlbedo);
        #endif
        specularShare = LinEncode(finalDiffuse + specularLight) - LinEncode(finalDiffuse);
    }

    #ifdef FOG_COLOR
        const bool fogOn = Toggles.y > 0.0f;
        outColor.rgb = fogOn ? lerp(outColor.rgb, pin.fog.rgb, pin.fog.a) : outColor.rgb;
        specularShare *= fogOn ? 1.0f - pin.fog.a : 1.0f;
    #endif

    #if SKIN_DEFERRED == 2	// (DeferredSkin: no direct light in a lighting-free record; a folded zero in LinEncode's pow is an infinity literal, X4579)
        #define SKIN_DEBUG_DIRECT 1.0e-6f
    #else
        #define SKIN_DEBUG_DIRECT (diffuseLight - ambientLight)
    #endif
    [branch] if (TESR_SkinDebug.x > 0.0f)
        outColor.rgb = SkinDebugView(TESR_SkinDebug.x, SKIN_DEBUG_DIRECT, ambientLight, specularLight, albedo, adAlbedo);

    #ifdef DIFFUSE
        outColor.a = 1.0f;
    #elif defined(ONLY_LIGHT)	// light-only pass
        outColor.a = texColor.a;
    #else
        outColor.a = texColor.a * AmbientColor.a;
    #endif

    PS_OUTPUT pout;
    pout.color = outColor;
    #if INTERIOR_SHADOWS && POINTS > 0 && POINT_SHADOW_RT && !SKIN_DEFERRED
        [branch] if (NVR_PointShadowRT.x > 2.5f) pout.color.rgb = PointShadowRtCoverage(ptWorldPos);	// UNOFFICIAL 2026-10-29 RT coverage view
    #endif
    pout.color.rgb = LinearSceneOut(pout.color.rgb);	// UNOFFICIAL 2026-10-28 LinearScene (includes/LinearLight.hlsl)
#if CAPTURED_NORMALS_ON
    // UNOFFICIAL 2026-10-28 CapturedNormals: the smooth vertex normal after skinning, before the normal map (the added lamp passes: none)
    #ifdef DIFFUSE
        pout.capturedNormal = CAPTURED_NORMAL_INVALID;
    #else
        pout.capturedNormal = CapturedNormalOut(pin.normal.xyz, worldPos);
        #if CAPTURED_NORMALS_ON >= 2   // (2026-11-11: was == 2. With AlbedoBuffer on the define is 3, so skin kept alpha 2.0, SkinFill (clip(a - 2.02)) wrote nothing and skin pixels kept the MaterialBuffer class, roughness and metal of whatever was drawn before them)
            // UNOFFICIAL 2026-10-31 MaterialBufferSkin (core/CapturedNormals.h SkinFill): the skin's roughness rides in the alpha (2.05 + 0.4 x it;
            // worn gear stays 2), copied into MaterialBuffer as class 6 after the list (this template's COLOR2 is the skin blur's)
            [flatten] if (pout.capturedNormal.a > 1.5f) pout.capturedNormal.a = 2.05f + 0.4f * saturate(skinRoughness);
        #endif
    #endif
#endif
    pout.scatter = float4(specularShare, length(worldPos));
    #ifdef DIFFUSE
        pout.albedo = 1.0f;   // never written: the added passes leave the albedo target alone (its writes are masked off)
    #elif defined(ONLY_LIGHT)	// light-only pass
        pout.albedo = float4(saturate(adAlbedo), luma(outColor.rgb * adAlbedo));   // what the engine multiplies in, and the colour once it has
    #else
        pout.albedo = float4(saturate(texColor.rgb), luma(outColor.rgb));
    #endif
    #if SKIN_DEFERRED
        // UNOFFICIAL 2026-11-15 DeferredSkin: G-buffer v2 on COLOR1-3 (Includes/SkinDeferred.hlsl), the core binds the G-buffer's targets for this
        // record (no skin blur's: core/DeferredSkin.h). keep = 1 - the fog this pass lerped COLOR0 with.
        const float skinKeep = fogOn ? 1.0f - pin.fog.a : 1.0f;
        #if SKIN_DEFERRED == 2
            pout.scatter = float4(albedo, FD_LF_CODE(FD_LF_SPEC, skinKeep));	// lighting-free: DeferredSun and DeferredLamps light it
        #else
            pout.scatter = float4(albedo, FD_LAMPS_CODE(skinKeep));	// the sun kept: DeferredLamps lights it
        #endif
        const float2 skinOct = ShadowEncodeNormal(worldN);
        pout.albedo = float4(6.0f, skinOct.x, skinRoughness, skinOct.y);	// model 6 (skin), no specular bit, metal 0; the bumped normal; roughness
        pout.capturedNormal.a = FDSkinCode(normalTexel.a, skinThinness, FDSkinSoftness(N, Nsoft));
    #endif
    return pout;
}

#endif	// pixel shader
