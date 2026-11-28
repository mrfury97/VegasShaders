#ifdef NVR_FORWARD_PLUS
    #include "includes/ForwardPlus.hlsl"	// UNOFFICIAL 2026-11-15 DeferredTransparents: the froxel's lamps (LAMPS_DEFERRED 4 / 5 only)
#endif
#ifdef MANY_LAMPS
    // UNOFFICIAL ManyLampsOnePass (core/ManyLamps.h): c37.x the lamp count, then per lamp k: c38 + 2k position in the object's space,
    // .w its reach; c39 + 2k its colour (already linear with linear lighting on). Free in every ObjectTemplate pixel shader.
    #define MANY_LAMPS_MAX 13
    float4 NVR_ManyLamps[1 + 2 * MANY_LAMPS_MAX] : register(c37);
    // 2026-10-27: lamps 14-19 in c76-c87 and 20-24 in c98-c107 (per lamp: position + reach, colour), free in every NVR pixel shader too.
    #define MANY_LAMPS_B 6
    #define MANY_LAMPS_C 5
    float4 NVR_ManyLampsB[2 * MANY_LAMPS_B] : register(c76);
#if defined(NVR_SM4) && NVR_SM4
    // (2026-11-14 mrfury97, port 2026-11-16) the no-limits build: SM4 refuses its overlap with Shadow.hlsl's c100-c107 matrices (ps_3_0
    // allows it: they are never both read there); placed by the compiler, the translator puts it back at c98 by name (the original's table)
    float4 NVR_ManyLampsC[2 * MANY_LAMPS_C];
#else
    float4 NVR_ManyLampsC[2 * MANY_LAMPS_C] : register(c98);
#endif
    #if MANY_LAMPS_ON >= 2
        #ifdef LIGHT_COMMON_ROWS_N
            #define LIGHT_COMMON_ROWS LIGHT_COMMON_ROWS_N	// UNOFFICIAL 2026-10-31 LampTableStandalone / TableLamps: ManyLamps::TableRowsNow
        #elif MANY_LAMPS_ON == 3
            #define LIGHT_COMMON_ROWS 31.0f	// UNOFFICIAL 2026-10-30 ClusteredLamps: ManyLamps::TableRows
        #endif
        #include "includes/LightCommon.hlsl"	// UNOFFICIAL 2026-10-29 LightCommonLamps: the lamps from the frame's table (s3)
    #endif
#endif

// UNOFFICIAL 2026-10-24 SmoothObjectNormals: the world normal at a point of this draw, p its camera-relative world position: the vertex
// normal the vertex shader sent in uv.zw, facing the camera; off, the per-triangle derivative normal as before (top level only then).
#if OBJECT_SMOOTH_NORMALS
    #define OBJ_WORLD_NORMAL(p) ShadowFacingNormal(ShadowPlainNormal(IN.uv.zw, IN.vertexColor.a), (p))
#else
    #define OBJ_WORLD_NORMAL(p) GetShadowGeometricNormal(p)
#endif

struct PS_OUTPUT {
    float4 color : COLOR0;
#if CAPTURED_NORMALS_ON
    // (ps_3_0 needs COLOR0..COLOR3 contiguous: COLOR1 / COLOR2 written as 0, discarded where nothing is bound there)
    float4 capturedPad1 : COLOR1;
    float4 capturedPad2 : COLOR2;
    float4 capturedNormal : COLOR3;	// UNOFFICIAL 2026-10-28 CapturedNormals (Includes/CapturedNormals.hlsl; bound only in the world's main list)
#endif
};

sampler2D BaseMap : register(s0);	// colour texture; alpha for the alpha test
sampler2D NormalMap : register(s1);	// rgb normal, alpha the highlight mask (gloss)
// UNOFFICIAL 2026-10-27 ManyLampsGlow (core/LampsInOnePass.h): glow-mapped objects in the one pass (BSSM_ADT10_G, ADT4_G, ADTS10_G and
// their _SG; the game binds the glow map on stage 4 and the emittance colour in c2 for these passes but never picked them before).
// (2026-10-28 port: only with MANY_LAMPS_ON, the only time ManyLampsGlow can patch them in; off = the shaders exactly as before.)
// (2026-11-15 bench batch 5: and in the lamps-deferred records, LAMPS_DEFERRED 3, which the deferred renderer binds on native too, where
// ManyLampsGlow now runs while it is live: Vault 21's 177 glow-mapped objects a frame went back to several passes without it.)
// (2026-11-15 bench batch 8, docs/next/FD-NATIVEFIX.md: and in the lighting-free records, LAMPS_DEFERRED 2, which DeferredRenderer 2 binds
// indoors: on native, without MANY_LAMPS_ON, they had no glow, so the Ultra-Luxe sculpture's glow-map panels went dark in deferred. The
// record keeps "ambient, emittance / glow" (see the top); the LIGHTS < 4 variant always did.)
#if defined(SI) && !defined(OPT) && ((defined(MANY_LAMPS_ON) && MANY_LAMPS_ON) || defined(NVR_LAMPS_OUT) || defined(NVR_LIGHT_DEFERRED))
    #define NVR_MANY_GLOW
#endif
#ifdef NVR_MANY_GLOW
    sampler2D GlowMap : register(s4);	// glow (SI) or hair tint (HAIR) map
#endif

#ifdef LINEAR_LIGHTING
    float4 AmbientColorIn : register(c1);
    static float4 AmbientColor;	// UNOFFICIAL linear lighting: as in the LIGHTS < 4 variant
    float4 PSLightColorIn[10] : register(c3);
    static float4 PSLightColor[10];
    #ifdef LIN_PREDECODED
        float4 AmbientColorDec : register(c64);		// UNOFFICIAL LinearPreDecode, as in the LIGHTS < 4 variant
        float4 PSLightColorDec[10] : register(c66);
    #endif
#else
    float4 AmbientColor : register(c1);	// rgb ambient light; a 1 = opaque, no alpha test
    float4 PSLightColor[10] : register(c3);	// rgb per light (sun first where there is one)
#endif
float4 PSLightPosition[8] : register(c19);	// per lamp: xyz object space, w radius


#ifdef OBJ_TOGGLES
    float4 EmittanceColor : register(c2);	// glow or hair colour

    float4 Toggles : register(c27);	// x vertex colour on, y fog on, z gloss power, w alpha test reference

    #define lightsUsed EmittanceColor.a	// how many of the lamp slots are live
    #define lightOffset 0	// first light slot that is a lamp
    #define glossPow Toggles.z	// the game's highlight exponent
#else
    #define lightsUsed PSLightColor[0].a	// how many of the lamp slots are live
    #define lightOffset 1	// first light slot that is a lamp (slot 0 is the sun)
    #define glossPow PSLightColor[1].w	// the game's highlight exponent
#endif

#if defined(MANY_LAMPS) && MANY_LAMPS_ON == 3
#if defined(NVR_SM4) && NVR_SM4 && defined(NVR_LAMP_SUMS)
PS_OUTPUT main(PS_INPUT IN) {	// (2026-11-14 mrfury97, port 2026-11-16) the no-limits build: one SV_Position (LampSums declares it)
    const float2 mlVPos = NVR_IN_VPOS;
#elif defined(NVR_SM4) && NVR_SM4
PS_OUTPUT main(PS_INPUT IN, float4 mlVPos4 : VPOS) {	// (2026-11-14 mrfury97) the no-limits build: SV_Position, the pixel centre
    const float2 mlVPos = mlVPos4.xy - 0.5f;
#else
PS_OUTPUT main(PS_INPUT IN, float2 mlVPos : VPOS) {	// UNOFFICIAL 2026-10-30 ClusteredLamps: the pixel's froxel
#endif
    #if defined(OBJ_ALPHA_HASH)
        #define OBJ_HASH_VPOS mlVPos
    #endif
#elif defined(OBJ_ALPHA_HASH) && !(defined(NVR_SM4) && NVR_SM4 && (defined(NVR_LAMP_SUMS) || defined(OBJ_PS_SPOSITION)))
PS_OUTPUT main(PS_INPUT IN ALPHA_HASH_VPOS_PARAM) {	// UNOFFICIAL 2026-11-03 SmoothCutouts: the pixel, for the noise
    #define OBJ_HASH_VPOS ALPHA_HASH_VPOS_VALUE
#else
PS_OUTPUT main(PS_INPUT IN) {
    #if defined(OBJ_ALPHA_HASH) && defined(OBJ_PS_SPOSITION)
        #define OBJ_HASH_VPOS (IN.sPosition.xy - 0.5f)	// (2026-11-14 mrfury97) the no-limits build: sPosition is SV_Position (the centre)
    #elif defined(OBJ_ALPHA_HASH)
        #define OBJ_HASH_VPOS NVR_IN_VPOS	// (2026-11-14 mrfury97) the no-limits build with LampSums: one SV_Position (IN.vpos4)
    #endif
#endif
    PS_OUTPUT pout;
#ifndef NVR_LIGHT_DEFERRED	// (the lighting-free record: no lamp, so no flashlight cone)
    [branch] if (NVR_FlashLamp[0].x > 0.5f) flashLampCone = FlashLampCone(SHADOW_WP_LOAD(IN), SHADOW_WP_VALID(IN));	// UNOFFICIAL 2026-11-04
#endif

#ifdef LINEAR_LIGHTING
    // UNOFFICIAL linear lighting: as in the LIGHTS < 4 variant.
    #ifdef LIN_PREDECODED	// UNOFFICIAL LinearPreDecode: decoded on the CPU
    [unroll] for (int lightIndex = 0; lightIndex < 10; lightIndex++) PSLightColor[lightIndex] = float4(PSLightColorDec[lightIndex].rgb, PSLightColorIn[lightIndex].a);	// (.a from the game's register: keeps it reserved)
    #else
    [unroll] for (int lightIndex = 0; lightIndex < 10; lightIndex++)
        PSLightColor[lightIndex] = float4(LinColor(PSLightColorIn[lightIndex].rgb), PSLightColorIn[lightIndex].a);
    #endif
    #ifdef LIN_PREDECODED
        AmbientColor = float4(AmbientColorDec.rgb, AmbientColorIn.a);
    #else
    AmbientColor = float4(LinColor(AmbientColorIn.rgb), AmbientColorIn.a);
    #endif
#endif
#ifdef NVR_LIGHT_DEFERRED
    // UNOFFICIAL 2026-11-14 the lighting-free record: every light colour of the draw a compile-time 0 (as in the LIGHTS < 4 variant; the
    // lamps of ManyLamps' block and table are compiled out in ManyLightsDirect.hlsl). The .a channels (lamp count, gloss) stay the game's.
    float4 nvrNoDirect[10];
    [unroll] for (int nvrLight = 0; nvrLight < 10; nvrLight++) nvrNoDirect[nvrLight] = float4(0.0f, 0.0f, 0.0f, PSLightColor[nvrLight].a);
    #define PSLightColor nvrNoDirect
#elif defined(NVR_LAMPS_OUT)
    // UNOFFICIAL 2026-11-14 the lamps-deferred record (LAMPS_DEFERRED 3, see the top): the lamps' colours a compile-time 0 (the passes add
    // them), the sun slot kept. The .a channels stay the game's.
    float4 nvrNoLamps[10];
    [unroll] for (int nvrLamp = 0; nvrLamp < 10; nvrLamp++) nvrNoLamps[nvrLamp] = float4(nvrLamp < NVR_SUN_SLOTS ? PSLightColor[nvrLamp].rgb : (0.0f).xxx, PSLightColor[nvrLamp].a);
    #define PSLightColor nvrNoLamps
#endif

    HudRain(IN.uv.xy, SHADOW_WP_LOAD(IN), SHADOW_WP_VALID(IN));	// UNOFFICIAL: rain drops, before the textures are read
    float4 baseColor = HUD_RAIN_TEX(BaseMap, IN.uv.xy);
    
    #ifdef OBJ_TOGGLES
        OBJ_ALPHA_TEST(baseColor.a);
    #endif
    
    baseColor.rgb = OBJ_WITH_VCOLOR(baseColor.rgb, IN.vertexColor.rgb);
    
    float4 normal = HUD_RAIN_TEX(NormalMap, IN.uv.xy);
    normal.xyz = OBJ_UNIT_NORMAL(normal.xyz);
    
    #ifdef RMAOS
        RmaosRead(IN.uv.xy);	// UNOFFICIAL _rmaos map (Object.hlsl): read once, here at top level
    #endif
    #if defined(METAL) && defined(RMAOS) && !defined(NVR_SLOT_TIGHT) && !defined(HAIR)
        const float2 coatUvDx4 = ddx(IN.uv.xy);
        const float2 coatUvDy4 = ddy(IN.uv.xy);
    #endif
    BridgeSetup(normal.a, glossPow);	// UNOFFICIAL 2026-11-05 VanillaBridge (Object.hlsl; nothing without NVR_VANILLA_BRIDGE)
    float roughness = getRoughness(normal.a);
    // UNOFFICIAL 2026-10-13 (user: polished marble walls in the casinos flickered round their lamps): the same geometric
    // specular AA as the other path above. This 4+ lamp path had none, so a glossy wall (material rules' stonepolished)
    // with a fine normal map sparkled from pixel to pixel as the view moved.
    roughness = SpecularAA(normal.xyz, roughness);
    // UNOFFICIAL 2026-10-13: blood on first-person geometry (Includes/HandBlood.hlsl); derivatives, so at top level.
    HandBlood(IN.uv.xy, SHADOW_WP_LOAD(IN), SHADOW_WP_VALID(IN));
    roughness = HandBloodRoughness(roughness);
    normal.xyz = HandBloodNormal(normal.xyz);
    roughness = HudRainRoughness(roughness);	// UNOFFICIAL: the rain drops, wet and rounded
    normal.xyz = HudRainNormal(normal.xyz);
    SetDiffuseEnergyKeep(normal.xyz, float3(IN.lightDir.w, IN.light2.w, IN.light3.w), roughness);	// UNOFFICIAL 2026-10-16 EnergySplit (Includes/PBR.hlsl): once per pixel, every lamp reuses it (view packed in .w)
    baseColor.rgb = HandBloodAlbedo(baseColor.rgb);
    const float3 albedoStored = baseColor.rgb;	// UNOFFICIAL linear lighting: as in the LIGHTS < 4 variant
    baseColor.rgb = LinColor(HudRainAlbedo(baseColor.rgb));	// UNOFFICIAL: darker under the rain drops (includes/HudRain.hlsl)

    // Lighting.
    float3 viewDir = { IN.lightDir.w, IN.light2.w, IN.light3.w };

    #ifdef METAL
        // UNOFFICIAL metal (Object.hlsl), as in the LIGHTS < 4 variant. An object lit by four or more lamps at once is drawn
        // with this shader, which had none: metal (and its DebugView) came and went as lamps came into or out of an
        // object's reach, the player's armour walking through the TOPS casino above all (2026-10-08). Pixels with metal get
        // the lighting with the metal finish, the others the old lighting exactly (see the lighting below). The vertex
        // shaders here send no world up (no interpolator is spare at MAX_LIGHTS 6), so the reflection has no sky term;
        // indoors that term is off anyway. Finish specular-anti-aliased here at top level (ddx/ddy).
        float metalRoughness = SpecularAA(normal.xyz, getMetalRoughness(normal.a));
        float metallic = 0.0f;
        [branch] if (getMetalStrength() > 0.0f)
            metallic = HAND_BLOOD_METAL(getDerivedMetallic(normal.a, albedoStored) * (SHADOW_WP_VALID(IN) ? getMetalFade(SHADOW_WP_LOAD(IN)) : 1.0f));
        float lightRoughness = lerp(roughness, metalRoughness, metallic);
        #if defined(RMAOS) && !defined(NVR_SLOT_TIGHT) && !defined(HAIR)
            [branch] if (TESR_SharpSky.y > 0.001f && RMAOS_ACTIVE && metallic > 0.55f) {
                float3 vTan4 = normalize(viewDir);
                float2 coatUvOff4 = -(vTan4.xy / max(vTan4.z, 0.35f)) * (0.0018f * saturate(TESR_SharpSky.y));
                float3 subCol4 = LinColor(HudRainAlbedo(HandBloodAlbedo(OBJ_WITH_VCOLOR(tex2Dgrad(BaseMap, IN.uv.xy + coatUvOff4, coatUvDx4, coatUvDy4).rgb, IN.vertexColor.rgb))));
                float subW4 = smoothstep(0.55f, 0.90f, metallic) * saturate(TESR_SharpSky.y) * 0.72f;
                baseColor.rgb = lerp(baseColor.rgb, subCol4, subW4);
            }
        #endif
    #endif

    float att;

    #if INTERIOR_SHADOWS
        // UNOFFICIAL forward point-light shadows -- see the LIGHTS < 4 variant. One geometric normal (ddx/ddy) serves the
        // sun, the point lights and the ambient.
        float3 ptWorldPos = SHADOW_WP_LOAD(IN);
        float3 ptNormal = OBJ_WORLD_NORMAL(ptWorldPos);
        float ptValid = SHADOW_WP_VALID(IN) ? 1.0f : 0.0f;
        // Light k of the draw is PSLightColor[k] (k = 0 is the sun's slot without OPT); the attenuations below are the
        // ones the lighting further down computes, so the compiler shares them.
        #define PT_ATT(i) vanillaAtt(PSLightPosition[i].xyz - IN.lPosition.xyz, PSLightPosition[i].w)
        float ptShadow0 = 1.0f, ptShadow1 = 1.0f, ptShadow2 = 1.0f, ptShadow3 = 1.0f, ptShadow4 = 1.0f, ptShadow5 = 1.0f;
        #if LAMPS_DEFERRED
        // UNOFFICIAL 2026-10-31 DeferredLamps: no lamp here, no lookups (ptShadow0..5 stay 1; the sun keeps its own)
        #elif defined(LAMP_SLOT_SKIP) || defined(MANY_LAMPS)	// (ManyLampsOnePass: only the game's own lamps of the draw have cube slots)
        // UNOFFICIAL 2026-10-25 NO_SLOT_LIMIT (DXVK only, see LAMP_IFK): a slot's cube lookup only where its lamp is used and reaches.
        [branch] if (NVR_PointShadowParams.x > 0.0f) {
            float a;
            #ifdef MANY_LAMPS
            // UNOFFICIAL 2026-10-31 SteadyLampShadows (core/ManyLamps.h Steady): c37.w 1 = this draw's slots hold re-sorted lamps, so the
            // game's lamp k (PSLightPosition) is not slot k's lamp: its reach must not gate the lookup (the cube's own radius still does).
            const float slotsResorted = NVR_ManyLamps[0].w;
            #else
            const float slotsResorted = 0.0f;
            #endif
            #ifndef OBJ_TOGGLES
                a = max(PT_ATT(0), slotsResorted);				[branch] if (a > 0.0f && 0 < lightsUsed) ptShadow0 = POINT_SHADOW_LOOKUP(0, a);
            #endif
            a = max(PT_ATT(lightOffset + 0), slotsResorted);	[branch] if (a > 0.0f && 1 < lightsUsed) ptShadow1 = POINT_SHADOW_LOOKUP(1, a);
            a = max(PT_ATT(lightOffset + 1), slotsResorted);	[branch] if (a > 0.0f && 2 < lightsUsed) ptShadow2 = POINT_SHADOW_LOOKUP(2, a);
            a = max(PT_ATT(lightOffset + 2), slotsResorted);	[branch] if (a > 0.0f && 3 < lightsUsed) ptShadow3 = POINT_SHADOW_LOOKUP(3, a);
            a = max(PT_ATT(lightOffset + 3), slotsResorted);	[branch] if (a > 0.0f && 4 < lightsUsed) ptShadow4 = POINT_SHADOW_LOOKUP(4, a);
            a = max(PT_ATT(lightOffset + 4), slotsResorted);	[branch] if (a > 0.0f && 5 < lightsUsed) ptShadow5 = POINT_SHADOW_LOOKUP(5, a);
        }
        #else
        [branch] if (NVR_PointShadowParams.x > 0.0f) {
            #ifndef OBJ_TOGGLES
                ptShadow0 = POINT_SHADOW_LOOKUP(0, PT_ATT(0));
            #endif
            ptShadow1 = POINT_SHADOW_LOOKUP(1, PT_ATT(lightOffset + 0));
            ptShadow2 = POINT_SHADOW_LOOKUP(2, PT_ATT(lightOffset + 1));
            #if MAX_LIGHTS >= 4
                ptShadow3 = POINT_SHADOW_LOOKUP(3, PT_ATT(lightOffset + 2));
            #endif
            #if MAX_LIGHTS >= 5
                // UNOFFICIAL 2026-10-13: lightOffset, as for the lamps above (it was missing: in the OPT passes lamps 5 and 6
                // took the previous lamp's position).
                ptShadow4 = POINT_SHADOW_LOOKUP(4, PT_ATT(lightOffset + 3));
                ptShadow5 = POINT_SHADOW_LOOKUP(5, PT_ATT(lightOffset + 4));
            #endif
        }
        #endif
        #undef PT_ATT
    #endif

    // Forward sun shadows -- see the LIGHTS < 4 variant. Only the OPT-off path has a sun
    // term; with OPT the first slot is a point light and must not be shadowed by the sun.
    #ifdef OBJ_TOGGLES
        float3 sunWP = SHADOW_WP_LOAD(IN);
        #if INTERIOR_SHADOWS
            float3 sunFaceN = ptNormal;
        #else
            float3 sunFaceN = OBJ_WORLD_NORMAL(sunWP);
        #endif
        // Decline to shadow if a vanilla vertex shader ran: the interpolator is undefined.
        float sunShadow = 1.0f;
        #if FORWARD_SHADOWS && !defined(NVR_LIGHT_DEFERRED)	// (the lighting-free record: no sun, no sun shadow)
        #ifdef NVR_EYE
        // UNOFFICIAL 2026-11-19: sample sun shadow EYE_SHADOW_OFFSET units toward the sun (clears eyelids/brow socket while catching
        // hats, roofs, walls, cliffs, and clouds; bypasses SunShadowWanted so wrapped diffuse is shadowed too).
        sunShadow = SHADOW_WP_VALID(IN)
                  ? GetSunShadow(sunWP + TESR_SmoothedSunDir.xyz * EYE_SHADOW_OFFSET, TESR_SmoothedSunDir.xyz)
                  : 1.0f;
        #elif UNLIT_SHADOW_SKIP
        // UNOFFICIAL 2026-10-17 UnlitShadowSkip (SunShadowWanted), as in the LIGHTS < 4 variant.
        [branch] if (SHADOW_WP_VALID(IN) && SunShadowWanted(normal.xyz, IN.lightDir.xyz))
            sunShadow = GetSunShadow(sunWP, sunFaceN);
        #else
        sunShadow = SHADOW_WP_VALID(IN)
                  ? GetSunShadow(sunWP, sunFaceN)
                  : 1.0f;
        #endif
        #endif
    #endif

    // ddx/ddy must stay at pixel-shader top level.
    #if INTERIOR_SHADOWS
        float3 skyNormal = ptNormal;
    #else
        float3 skyNormal = OBJ_WORLD_NORMAL(SHADOW_WP_LOAD(IN));
    #endif
    #if CAPTURED_NORMALS_ON >= 4
        // UNOFFICIAL 2026-11-14 GBufferV2 (Includes/CapturedNormals.hlsl): the normal-mapped normal in world axes. The table family carries the
        // tangent frame's rows (light2/3/4, the vertex's own space) and the world -> object rows (c39-c41, core/ManyLamps.h): exact, as the
        // DeferredLamps records take it. Every other record: the screen-derivative frame about the facing normal (top level: ddx / ddy).
        #if defined(MANY_LAMPS) && MANY_LAMPS_ON >= 2
            const float3 gbObjNormal = normal.x * IN.light2.xyz + normal.y * IN.light3.xyz + normal.z * IN.light4.xyz;
            const float3 gbShadingNormal = gbObjNormal.x * NVR_ManyLamps[2].xyz + gbObjNormal.y * NVR_ManyLamps[3].xyz + gbObjNormal.z * NVR_ManyLamps[4].xyz;
        #else
            const float3 gbShadingNormal = SHADOW_WP_VALID(IN) ? CapturedDerivNormal(SHADOW_WP_LOAD(IN), IN.uv.xy, skyNormal, normal.xyz) : 0.0f;
        #endif
    #endif
    #if DUST_ON && !defined(HAIR) && !defined(DIFFUSE) && !defined(ONLY_SPECULAR) && !defined(ONLY_LIGHT)
        // 2026-11-04 dust on top surfaces, as in the LIGHTS < 4 variant (user: the dust came and went as the player moved: objects
        // switch between the two variants as lamps come into reach, and this one had none)
        baseColor.rgb = DustCover(baseColor.rgb, skyNormal, SHADOW_WP_LOAD(IN), SHADOW_WP_VALID(IN));
    #endif

    // UNOFFICIAL 2026-11-04 Wetness Effects (as the LIGHTS < 4 variant): the albedo darkened before any light sees it. Only the sun's
    // path (OBJ_TOGGLES) has the position and the sun.
    #if defined(OBJECT_WETNESS) && defined(OBJ_TOGGLES)
        Wetness wet = (Wetness)0;
        wet.roughness = 1.0f;
        wet.normal = sunFaceN;
        [branch] if (SHADOW_WP_VALID(IN)) wet = GetWetness(sunWP, sunFaceN, sunFaceN OBJ_PUDDLE_HEIGHT(IN.uv.xy, sunWP));
        #ifdef PUDDLES_COMPILED	// 2026-11-08 Puddles: standing water hides the normal map (the sun, lamps and ambient see a flat surface)
            normal.xyz = normalize(lerp(normal.xyz, float3(0.0f, 0.0f, 1.0f), wet.flatten));
        #endif
        #ifdef METAL
            baseColor.rgb = ObjectWetAlbedo(baseColor.rgb, 1.0f, wet, 1.0f - saturate(sqrt(metallic)));
        #else
            baseColor.rgb = ObjectWetAlbedo(baseColor.rgb, 1.0f, wet, 1.0f);
        #endif
    #endif

    // The sun's and the lamps' light (includes/ManyLightsDirect.hlsl), then the ambient.
    float3 lighting;
    #if SHADOW_CONTRAST_ON
        float3 scLost = 0.0f;	// UNOFFICIAL ShadowContrast: the light the lamps' shadows take here (SC_SPLIT)
    #endif
    #ifdef METAL
    float3 viewNorm4 = normalize(viewDir);
    float ndv4 = saturate(dot(normal.xyz, viewNorm4));
    float dielKS4 = 0.0f;
    #if defined(RMAOS) && !defined(NVR_SLOT_TIGHT)
        [branch] if (RMAOS_ACTIVE) dielKS4 = getDielectricEnvBRDF(lightRoughness, ndv4);
    #endif
    #if defined(RMAOS) && !defined(NVR_SLOT_TIGHT)
    [branch] if (metallic > 0.0f || RMAOS_ACTIVE)
    #else
    [branch] if (metallic > 0.0f)
    #endif
    {
        #define LIGHT_FINISH lightRoughness, metallic
        #include "includes/ManyLightsDirect.hlsl"
        #undef LIGHT_FINISH
        lighting += getAmbientLighting(AmbientColor.rgb, baseColor.rgb * (1.0f - metallic) * (1.0f - dielKS4), skyNormal,
                                       SHADOW_WP_VALID(IN) ? 1.0f : 0.0f);
        #if defined(NVR_LIVE_CUBE) && !defined(NVR_SLOT_TIGHT)
            #if CAPTURED_NORMALS_ON >= 4
                float3 metalWorldNormal4 = dot(gbShadingNormal, gbShadingNormal) > 1.0e-4f ? normalize(gbShadingNormal) : skyNormal;
            #else
                float3 metalWorldNormal4 = skyNormal;
            #endif
            float metalWpValid4 = SHADOW_WP_VALID(IN) ? 1.0f : 0.0f;
            float3 metalWp4 = SHADOW_WP_LOAD(IN);
            float upReflect4 = metalWpValid4 > 0.0f ? reflect(normalize(metalWp4), metalWorldNormal4).z : 0.0f;
            lighting += getSurfaceIBL(AmbientColor.rgb, baseColor.rgb, lightRoughness, ndv4, upReflect4, metalWpValid4,
                                      metalWp4, metalWorldNormal4, skyNormal, saturate(viewNorm4.z), metallic, dielKS4);
        #else
            lighting += getSurfaceIBL(AmbientColor.rgb, baseColor.rgb, lightRoughness, ndv4, 0.0f, 0.0f,
                                      (0.0f).xxx, (0.0f).xxx, (0.0f).xxx, saturate(viewNorm4.z), metallic, dielKS4);
        #endif
    }
    else
    #endif
    {
        #define LIGHT_FINISH roughness
        #include "includes/ManyLightsDirect.hlsl"
        #undef LIGHT_FINISH
        lighting += getAmbientLighting(AmbientColor.rgb, baseColor.rgb, skyNormal,
                                       SHADOW_WP_VALID(IN) ? 1.0f : 0.0f);
    }
    #if defined(OBJECT_WETNESS) && defined(OBJ_TOGGLES)	// the water film over the material
        [branch] if (wet.gloss > 0.001f)
            lighting += ObjectWetFilm(wet, sunWP, sunFaceN, PSLightColor[0].rgb * sunShadow * LIN_SCALE_LIGHT, 1.0f);
    #endif
    #ifdef NVR_FORWARD_PLUS
        // UNOFFICIAL 2026-11-15 DeferredTransparents (Includes/ForwardPlus.hlsl), as in the LIGHTS < 4 variant: the froxel's lamps from the table
        #if CAPTURED_NORMALS_ON >= 4
            #define FP_N FPUnit(gbShadingNormal, skyNormal)
        #else
            #define FP_N FPUnit(skyNormal, skyNormal)
        #endif
        #ifdef METAL
            lighting += FPLampSum(SHADOW_WP_LOAD(IN), FP_N, SHADOW_WP_VALID(IN) ? 1.0f : 0.0f, baseColor.rgb, lightRoughness, metallic);
        #else
            lighting += FPLampSum(SHADOW_WP_LOAD(IN), FP_N, SHADOW_WP_VALID(IN) ? 1.0f : 0.0f, baseColor.rgb, roughness, 0.0f);
        #endif
        #undef FP_N
    #endif

    #if SHADOW_CONTRAST_ON
        lighting *= ShadowContrastKeep(lighting, scLost);	// UNOFFICIAL ShadowContrast (see its constant); before the glow, which stays as it is
    #endif

    // UNOFFICIAL ManyLampsGlow + 2026-11-23 DecoupleGlowMaps: self emittance with scene lighting attenuated on glowing texels.
    #ifdef NVR_MANY_GLOW
        float3 glowRaw = tex2D(GlowMap, IN.uv.xy).rgb * EmittanceColor.rgb;
        float glowDecouple = GlowDecoupleMask(glowRaw);
        lighting = lighting * (1.0f - glowDecouple) + baseColor.rgb * LinColor(glowRaw) * GlowScaleFactor();
    #endif

    // UNOFFICIAL 2026-11-12 HairAndEyes (as the LIGHTS < 4 shader): the flashlight's beam on the eyeball. Not in a deferred-lamp record:
    // the DeferredLamps pass takes the pixel's place from the recorded distance, so that record must stay true, and the flashlight's
    // light pass lights the eye there as before.
    #if defined(NVR_EYE) && !(LAMPS_DEFERRED && defined(MANY_LAMPS) && MANY_LAMPS_ON >= 2) && !defined(NVR_LIGHT_DEFERRED)
        #define EYE_BEAM_HERE
        lighting += EyeBeam(baseColor.rgb, skyNormal, SHADOW_WP_LOAD(IN), SHADOW_WP_VALID(IN));
    #endif

    // TODO: Vanilla attenuates the full specular term by IN.lPosition.w for some reason. Is this a problem?
    float3 shaded = LinEncode(lighting);	// UNOFFICIAL linear lighting
    shaded = MaterialRulesDebug(shaded, albedoStored);   // UNOFFICIAL [Shaders.PBR.MaterialRules] DebugView
    shaded = RmaosDebug(shaded, albedoStored);           // UNOFFICIAL [Shaders.PBR.Rmaos] DebugView

    #ifdef METAL
        // [Shaders.PBR.Metal] DebugView: metal in orange over a dim grey image (as in the LIGHTS < 4 variant).
        [branch] if (TESR_PBRMetalLook.w > 0.5f) shaded = lerp(luma(albedoStored).xxx * 0.35f, float3(1.0f, 0.45f, 0.05f), metallic);
    #endif
    
    shaded.rgb = OBJ_WITH_FOG(shaded.rgb, IN.fogColor);
    

#if SHADOW_FORCE_MARKER
    shaded.rgb = OBJ_MARKER_COLOR;
#endif

    pout.color.rgb = ApplyBarrelHeat(shaded.rgb, BarrelHeatMask(IN.lPosition.xyz));
    #if INTERIOR_SHADOWS && !defined(NVR_NO_LAMPS)	// (the lighting-free / lamps-deferred records: these views trace the lamp shadows they have not got)
        // UNOFFICIAL DebugView 3 (PointShadowForward.hlsl PointShadowTrace).
        [branch] if (NVR_PointShadowDebug.x > 2.5f) pout.color.rgb = PointShadowTrace(ptWorldPos, ptNormal, ptValid);
        #if POINT_SHADOW_RT
            [branch] if (NVR_PointShadowRT.x > 2.5f) pout.color.rgb = PointShadowRtCoverage(ptWorldPos);	// UNOFFICIAL 2026-10-29 RT coverage view
        #endif
        #if LAMP_CAPS_VIEW
            // UNOFFICIAL 2026-10-31 LampCapsCensus view 4 (PointShadowForward.hlsl NVR_CapsUnshadowed): red where a lamp with no shadow source lights this pass
            [branch] if (NVR_PointShadowDebug.x > 3.5f) pout.color.rgb = float3(NVR_CapsUnshadowed > 0.5f ? 1.0f : 0.0f, 0.0f, 0.0f);
        #endif
    #endif
    pout.color.a = ReflMaterialAlpha(OBJ_CUT_OUT(baseColor.a) * AmbientColor.a);	// UNOFFICIAL MaterialAware reflections (Object.hlsl)
#if CAPTURED_NORMALS_ON
    #ifdef NVR_MANY_GLOW
        pout.capturedPad1 = CapturedAlbedoOut(baseColor.rgb * (1.0f - glowDecouple));	// UNOFFICIAL 2026-10-31 AlbedoBuffer (Includes/CapturedNormals.hlsl; the pad's 0 below level 3)
    #else
        pout.capturedPad1 = CapturedAlbedoOut(baseColor.rgb);	// UNOFFICIAL 2026-10-31 AlbedoBuffer (Includes/CapturedNormals.hlsl; the pad's 0 below level 3)
    #endif
    // (2026-11-12 ptbump: the *NoHighlight forms, as in the LIGHTS < 4 variant; not the many-lamp shiny records, whose roughness the
    // DeferredLamps pass reads as it is: they are drawn for meshes with a highlight only)
    #ifdef NVR_MANY_GLOW
        #if defined(MANY_LAMPS)
            #ifdef METAL
                pout.capturedPad2 = CapturedMaterialOutMetal(lerp(lightRoughness, 1.0f, glowDecouple), metallic * (1.0f - glowDecouple));
            #else
                pout.capturedPad2 = CapturedMaterialOut(lerp(roughness, 1.0f, glowDecouple));
            #endif
        #elif defined(METAL)
            pout.capturedPad2 = CapturedMaterialOutMetalNoHighlight(lerp(lightRoughness, 1.0f, glowDecouple), metallic * (1.0f - glowDecouple));
        #else
            pout.capturedPad2 = CapturedMaterialOutNoHighlight(lerp(roughness, 1.0f, glowDecouple));
        #endif
    #elif defined(MANY_LAMPS)
        #ifdef METAL	// UNOFFICIAL 2026-10-31: the metal share and the finish it was lit with
            pout.capturedPad2 = CapturedMaterialOutMetal(lightRoughness, metallic);
        #else
            pout.capturedPad2 = CapturedMaterialOut(roughness);	// UNOFFICIAL 2026-10-29 MaterialBuffer (0 without it)
        #endif
    #elif defined(METAL)
        pout.capturedPad2 = CapturedMaterialOutMetalNoHighlight(lightRoughness, metallic);
    #else
        pout.capturedPad2 = CapturedMaterialOutNoHighlight(roughness);
    #endif
    #if CAPTURED_NORMALS_ON >= 4
        pout.capturedPad2 = CapturedShadingNormal(pout.capturedPad2, gbShadingNormal);	// UNOFFICIAL 2026-11-14 GBufferV2: g / a (the deferred records too)
    #endif
    #if defined(NVR_LIGHT_DEFERRED) && CAPTURED_NORMALS_ON >= 3
        // UNOFFICIAL 2026-11-14 the lighting-free record's COLOR1.a, its one writer (Includes/DeferredPixel.hlsl FD_LF_CODE, as in the LIGHTS < 4
        // variant): OPT's light 0 is a lamp (no sun); else the sun of ManyLightsDirect.hlsl
        #ifdef OBJ_FOG
            #define FD_LF_KEEP OBJ_FOG_KEEP(IN.fogColor)
        #elif defined(OBJ_AD_GBUFFER) && defined(AD_FOG_KEEP_ON)
            #define FD_LF_KEEP (1.0f - IN.foldColor.a)	// UNOFFICIAL 2026-11-16 fd-batchE: AD's fog comes in the game's later FOG pass (see the top)
        #else
            #define FD_LF_KEEP 1.0f
        #endif
        #if defined(OPT) || defined(NVR_EYE)
            pout.capturedPad1.a = FD_LF_CODE(FD_LF_NOSUN, FD_LF_KEEP);
        #elif defined(SPECULAR)
            pout.capturedPad1.a = FD_LF_CODE(FD_LF_SPEC, FD_LF_KEEP);
        #else
            pout.capturedPad1.a = FD_LF_CODE(FD_LF_MATTE, FD_LF_KEEP);
        #endif
        #undef FD_LF_KEEP
    #endif
    // UNOFFICIAL 2026-10-28 CapturedNormals: the smooth vertex normal, before the normal map (as in the LIGHTS < 4 variant)
    #if LAMPS_DEFERRED && INTERIOR_SHADOWS
        // UNOFFICIAL 2026-10-31 DeferredLamps: the deferred record always writes it, the pass takes the pixel's position from its distance.
        // SLS2035 (slot-tight) gets no smooth normal from its vertex shader: there the geometric normal the lamp shadows use (ptNormal).
        #if OBJECT_SMOOTH_NORMALS && !defined(NVR_SLOT_TIGHT)
            pout.capturedNormal = OBJ_CAPTURED_NORMAL_OUT(ShadowPlainNormal(IN.uv.zw, IN.vertexColor.a), SHADOW_WP_LOAD(IN));
        #else
            pout.capturedNormal = CapturedNormalOut(ptNormal, SHADOW_WP_LOAD(IN));
        #endif
    #elif defined(NVR_GBUFFER_LIT)
        // UNOFFICIAL 2026-11-14 the lighting-free record (no INTERIOR_SHADOWS: one record indoors and out) always writes it, as above:
        // the smooth normal, or (slot-tight SLS2035, SmoothObjectNormals off) the geometric one the ambient used
        #if OBJECT_SMOOTH_NORMALS && !defined(NVR_SLOT_TIGHT)
            pout.capturedNormal = OBJ_CAPTURED_NORMAL_OUT(ShadowPlainNormal(IN.uv.zw, IN.vertexColor.a), SHADOW_WP_LOAD(IN));
        #else
            pout.capturedNormal = CapturedNormalOut(skyNormal, SHADOW_WP_LOAD(IN));
        #endif
    #elif OBJECT_SMOOTH_NORMALS && !defined(NVR_SLOT_TIGHT)
        pout.capturedNormal = OBJ_CAPTURED_NORMAL_OUT(ShadowPlainNormal(IN.uv.zw, IN.vertexColor.a), SHADOW_WP_LOAD(IN));
    #else
        pout.capturedNormal = CAPTURED_NORMAL_INVALID;
    #endif
    #ifdef EYE_BEAM_HERE
        // UNOFFICIAL 2026-11-12 HairAndEyes (as in the LIGHTS < 4 variant): recorded as far away while the eye takes the beam itself
        [flatten] if (FLASH_BEAM_ON && SHADOW_WP_VALID(IN) && pout.capturedNormal.a > 0.5f) pout.capturedNormal.b = FLASH_BEAM_FAR;
    #endif
    #if defined(FD_SV_RECORD) && !defined(NVR_EYE)
        // UNOFFICIAL 2026-11-16 / 2026-11-21 fd-sunvis (as in the LIGHTS < 4 variant; OPT records F0 with vis 1.0f for DeferredLamps / FlashlightDeferred)
        #ifdef NVR_MANY_GLOW
            const float fdManyGlowF0 = FD_SV_F0 * (1.0f - glowDecouple);
        #else
            const float fdManyGlowF0 = FD_SV_F0;
        #endif
        #ifdef OPT
            pout.capturedNormal.a = FDSunVisCode(pout.capturedNormal.a, 1.0f, fdManyGlowF0, NVR_SpecFade.z);
        #else
            pout.capturedNormal.a = FDSunVisCode(pout.capturedNormal.a, FDSunVisMicro(normal.xyz, IN.lightDir.xyz), fdManyGlowF0, NVR_SpecFade.z);
        #endif
    #endif
    // UNOFFICIAL 2026-11-12 CapturedCoverage (Includes/CapturedNormals.hlsl; as in the LIGHTS < 4 variant): x 1 on an opaque draw.
    // Not in the slot-tight six-lamp shader (it sits at the slot count drivers refuse): it records the whole polygon, as before.
    #if CAPTURED_NORMALS_ON >= 3 && !defined(NVR_SLOT_TIGHT)
    {
        const float capKeep = CapturedCoverage(baseColor.a * AmbientColor.a);
        pout.capturedPad1.a *= capKeep;
        pout.capturedPad2.a *= capKeep;
        pout.capturedNormal.a *= capKeep;
    }
    #endif
#endif
    #if LAMPS_DEFERRED && defined(MANY_LAMPS) && INTERIOR_SHADOWS
        // UNOFFICIAL 2026-10-31 DeferredLamps test view ([Shaders.ShadowsInteriors.Forward] DebugView 6): how far the tangent frame the forward
        // records light in (light2/3/4) is from orthonormal. Forward dots in that frame equal the pass's world-space dots only if it is;
        // red = the rows' worst cosine to each other x 4, green / blue = the tangent's / binormal's length against the normal's, -1, x 4.
        [branch] if (NVR_PointShadowDebug.x > 5.5f) {
            const float3 tT = IN.light2.xyz, tB = IN.light3.xyz, tN = IN.light4.xyz;
            const float lT = length(tT), lB = length(tB), lN = max(length(tN), 1.0e-6f);
            const float skew = max(max(abs(dot(tT, tB)) / max(lT * lB, 1.0e-6f), abs(dot(tT, tN)) / max(lT * lN, 1.0e-6f)), abs(dot(tB, tN)) / max(lB * lN, 1.0e-6f));
            pout.color.rgb = saturate(float3(skew, abs(lT / lN - 1.0f), abs(lB / lN - 1.0f)) * 4.0f);
        }
    #endif
    #if defined(OBJECT_WETNESS) && defined(PUDDLES_COMPILED)
        [branch] if (PuddleDebugColor.x >= 0.0f) pout.color.rgb = PuddleDebugColor;	// 2026-11-08 [Shaders.Puddles.Debug] DebugView
    #endif
    pout.color.rgb = LinearSceneOut(pout.color.rgb);	// UNOFFICIAL 2026-10-28 LinearScene (includes/LinearLight.hlsl)

    return pout;
}
