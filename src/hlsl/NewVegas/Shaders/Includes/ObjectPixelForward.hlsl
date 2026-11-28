struct PS_INPUT {
#if defined(OBJ_FOLD_COLOR)
    float4 foldColor : COLOR0;	// UNOFFICIAL FoldHighlights: the vertex colour
#endif
#ifdef NVR_LAMP_SUMS
#if defined(NVR_SM4) && NVR_SM4
    float4 vpos4 : VPOS;	// the no-limits build (NVR DXVK fork, TerrainTemplate.hlsl): SV_Position, the pixel centre
    #define NVR_IN_VPOS (IN.vpos4.xy - 0.5f)
#else
    float2 vpos : VPOS;	// UNOFFICIAL LampSums: the pixel, to read the run's copy of the frame
    #define NVR_IN_VPOS IN.vpos
#endif
#endif
#ifdef OBJ_VCOLOR
    float4 vertexColor : COLOR0;    // METAL: .a = world up's z in tangent space, encoded (see the VS)
#endif
#ifdef OBJ_FOG
    float4 fogColor : COLOR1;	// rgb fog colour, a fog amount
#endif
    float4 uv : TEXCOORD0;	// xy texture coordinates
    float4 lightDir : TEXCOORD1_centroid;	// light 0, tangent space
#ifdef OBJ_LAMP2
    float4 light2Dir : TEXCOORD2_centroid;	// light 1, tangent space; w its radius
#endif
#ifdef OBJ_LAMP3
    float4 light3Dir : TEXCOORD3_centroid;	// light 2, tangent space; w its radius
#endif
    float4 viewDir : TEXCOORD6_centroid;
    float4 lightDistSq : TEXCOORD5;    // METAL: .w = world up's x in tangent space
    float4 shadowWorldPos : TEXCOORD4;	// camera-relative world position; w SHADOW_VS_SENTINEL
#ifdef PROJ_SHADOW
    float4 shadowUVs : TEXCOORD7;	// xy canopy shadow map, zw its mask
#endif
#ifdef OBJ_SMOOTH_NORMAL_INTERP
    float4 smoothNormal : TEXCOORD8;	// UNOFFICIAL 2026-10-24 SmoothObjectNormals
#endif
};

struct PS_OUTPUT {
    float4 color : COLOR0;
#ifdef ONLY_SPECULAR
    // UNOFFICIAL 2026-10-19 NewSkin: a copy of COLOR0. The game draws its own highlight passes on skin with these shaders;
    // while the screen-space skin blur runs, the SkinShader hook (Hooks/Shaders.cpp) binds the blur's skin target here, so
    // the same highlights add to its highlight share as to the scene: the blur takes them out before it blurs and puts them
    // back after, as it does the skin shader's own (Effects/SkinScattering.fx.hlsl). Anywhere else it is discarded.
    float4 highlightCopy : COLOR1;
#endif
#if CAPTURED_NORMALS_ON
    // (ps_3_0 needs COLOR0..COLOR3 contiguous: COLOR2 written as 0, discarded where nothing is bound there; COLOR1 is highlightCopy)
    #if !defined(ONLY_SPECULAR)
    float4 capturedPad1 : COLOR1;
    #endif
    float4 capturedPad2 : COLOR2;
    float4 capturedNormal : COLOR3;	// UNOFFICIAL 2026-10-28 CapturedNormals (Includes/CapturedNormals.hlsl; bound only in the world's main list)
#endif
};

#ifndef OBJ_AMBIENT
    sampler2D NormalMap : register(s0);	// passes without the colour texture take the normal map first
    #if defined(NVR_FOLD) && defined(DIFFUSE)
        sampler2D FoldBaseMap : register(s7);	// UNOFFICIAL FoldHighlights: the object's base texture, bound by NVR for folded draws (core\FoldHighlights.h kBaseStage)
    #endif
#else
    sampler2D BaseMap : register(s0);	// colour texture; alpha for the alpha test
    sampler2D NormalMap : register(s1);	// rgb normal, alpha the highlight mask (gloss)
#endif

#ifdef LINEAR_LIGHTING
    #ifdef OBJ_AMBIENT
        float4 AmbientColorIn : register(c1);
        static float4 AmbientColor;	// UNOFFICIAL linear lighting: AmbientColorIn decoded (rgb), at the start of main
    #endif
    float4 PSLightColorIn[10] : register(c3);
    static float4 PSLightColor[10];	// UNOFFICIAL linear lighting: PSLightColorIn decoded (rgb; .a as it is), at the start of main
    #ifdef LIN_PREDECODED
        float4 AmbientColorDec : register(c64);		// UNOFFICIAL LinearPreDecode: c1 decoded on the CPU (core/LinearPreDecode.h)
        float4 PSLightColorDec[10] : register(c66);	// c3-c12 decoded on the CPU
    #endif
#else
    #ifdef OBJ_AMBIENT
        float4 AmbientColor : register(c1);	// rgb ambient light; a 1 = opaque, no alpha test
    #endif
    float4 PSLightColor[10] : register(c3);	// rgb per light (sun first where there is one)
#endif

#if (defined(SI) || defined(HAIR)) && !defined(ONLY_SPECULAR)
    #ifdef ONLY_LIGHT
        sampler2D GlowMap : register(s3);	// glow (SI) or hair tint (HAIR) map
    #else
        sampler2D GlowMap : register(s4);	// glow (SI) or hair tint (HAIR) map
    #endif
    float4 EmittanceColor : register(c2);	// glow or hair colour
#endif

#ifdef PROJ_SHADOW
    #ifdef ONLY_SPECULAR
        sampler2D ShadowMap : register(s4);	// the game's projected (canopy) shadow
        sampler2D ShadowMaskMap : register(s5);	// where that shadow applies
    #elif defined(ONLY_LIGHT)
        sampler2D ShadowMap : register(s5);	// the game's projected (canopy) shadow
        sampler2D ShadowMaskMap : register(s6);	// where that shadow applies
    #else
        sampler2D ShadowMap : register(s6);	// the game's projected (canopy) shadow
        sampler2D ShadowMaskMap : register(s7);	// where that shadow applies
    #endif
#endif

#ifdef OBJ_TOGGLES
    float4 Toggles : register(c27);	// x vertex colour on, y fog on, z gloss power, w alpha test reference
#endif

#ifdef OBJ_LAMP_FIRST
    float4 TESR_LampPassTrim : register(c196);   // UNOFFICIAL TrimLampPasses: x > 0 = on (pinned clear of the game's own registers)
#endif
#ifdef NVR_FORWARD_PLUS
    #include "includes/ForwardPlus.hlsl"	// UNOFFICIAL 2026-11-15 DeferredTransparents: the froxel's lamps (LAMPS_DEFERRED 4 / 5 only)
#endif


#if defined(OBJ_ALPHA_HASH) && !defined(NVR_LAMP_SUMS)
PS_OUTPUT main(PS_INPUT IN ALPHA_HASH_VPOS_PARAM) {	// UNOFFICIAL 2026-11-03 SmoothCutouts: the pixel, for the noise
    #define OBJ_HASH_VPOS ALPHA_HASH_VPOS_VALUE
#else
PS_OUTPUT main(PS_INPUT IN) {
    #if defined(OBJ_ALPHA_HASH)
        #define OBJ_HASH_VPOS NVR_IN_VPOS	// LampSums has it already
    #endif
#endif
    #if SHADOW_CONTRAST_ON
        float3 scLost = 0.0f;	// UNOFFICIAL ShadowContrast: the light the lamps' shadows take here (SC_SPLIT)
    #endif
    PS_OUTPUT pout;
#ifndef NVR_LIGHT_DEFERRED	// (the lighting-free record: no lamp, so no flashlight cone)
    [branch] if (NVR_FlashLamp[0].x > 0.5f) flashLampCone = FlashLampCone(IN.shadowWorldPos.xyz, SHADOW_VS_PRESENT(IN.shadowWorldPos.w));	// UNOFFICIAL 2026-11-04
#endif

#ifdef LINEAR_LIGHTING
    // UNOFFICIAL linear lighting: the light colours decoded.
    #ifdef LIN_PREDECODED	// UNOFFICIAL LinearPreDecode: decoded on the CPU
    [unroll] for (int lightIndex = 0; lightIndex < 10; lightIndex++) PSLightColor[lightIndex] = float4(PSLightColorDec[lightIndex].rgb, PSLightColorIn[lightIndex].a);	// (.a from the game's register: keeps it reserved)
    #else
    [unroll] for (int lightIndex = 0; lightIndex < 10; lightIndex++)
        PSLightColor[lightIndex] = float4(LinColor(PSLightColorIn[lightIndex].rgb), PSLightColorIn[lightIndex].a);
    #endif
    #ifdef OBJ_AMBIENT
        #ifdef LIN_PREDECODED
            AmbientColor = float4(AmbientColorDec.rgb, AmbientColorIn.a);
        #else
        AmbientColor = float4(LinColor(AmbientColorIn.rgb), AmbientColorIn.a);
        #endif
    #endif
#endif
#ifdef NVR_LIGHT_DEFERRED
    // UNOFFICIAL 2026-11-14 the lighting-free record (see NVR_LIGHT_DEFERRED at the top): every light colour of the draw (the sun's slot and
    // the lamps') is a compile-time 0 from here on, so each direct term (diffuse, highlight, fold, wet film's sun) and the shadow lookups
    // that only scale them leave the compiled shader. The .a channels (lamp count, gloss) stay the game's.
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

    // UNOFFICIAL: rain drops on the first-person weapon (includes/HudRain.hlsl), before the textures are read: they are read
    // through the drops (HUD_RAIN_TEX).
    HudRain(IN.uv.xy, IN.shadowWorldPos.xyz, SHADOW_VS_PRESENT(IN.shadowWorldPos.w));

#ifdef OBJ_AD_PASS
    float3 heatMaterialFactor = 1.0f;
#endif
    #ifdef OBJ_AMBIENT
        float4 surface = HUD_RAIN_TEX(BaseMap, IN.uv.xy);
    
        #ifdef ONLY_LIGHT
            // Keep the sampled albedo before the lighting-only path discards it.
            // Bound the reciprocal on black texels to avoid extreme HDR values.
            heatMaterialFactor = max(surface.rgb, 0.03f);
            #ifdef NVR_FOLD
                const float3 foldTexture = surface.rgb;	// UNOFFICIAL FoldHighlights: the colour the texture pass multiplies in
            #endif
            #if CAPTURED_NORMALS_ON >= 3
                const float3 capBaseTex = surface.rgb;	// UNOFFICIAL 2026-10-31 AlbedoBuffer: the texture the TEXTURE pass multiplies in (see the output)
            #endif
            surface.rgb = 1;
        #endif
    #else
        float4 surface = 1;
    #endif
    
    #if defined(OBJ_TOGGLES) && !defined(ONLY_SPECULAR)
        OBJ_ALPHA_TEST(surface.a);
    #endif
    // (2026-11-16 fd-merged-1116) the alpha this pass writes (OBJ_CUT_OUT: Smooth Cutouts' hash, FarCoverage), taken here, while few values
    // are live, instead of at the output: there, at the end of the lighting, the hash's fwidth / noise temps ran SLS2027 (SPECULAR SI
    // PROJ_SHADOW, LIGHTS 2) out of temp registers with MicroShadow on (X4505 on the fork too, PREFER_FLOW_CONTROL as well). surface.a
    // does not change between here and the output, so the value is the same.
    #if defined(OBJ_ALPHA_HASH) || defined(OBJ_CUT_COVERAGE)
        const float objCutAlpha = OBJ_CUT_OUT(surface.a);
        #define OBJ_CUT_ALPHA objCutAlpha
    #else
        #define OBJ_CUT_ALPHA OBJ_CUT_OUT(surface.a)
    #endif
    
    float4 normal = HUD_RAIN_TEX(NormalMap, IN.uv.xy);
    normal.xyz = OBJ_UNIT_NORMAL(normal.xyz);
    
    #ifdef RMAOS
        RmaosRead(IN.uv.xy);	// UNOFFICIAL _rmaos map (Object.hlsl): read once, here at top level
    #endif
    #if defined(METAL) && defined(RMAOS) && defined(OBJ_AMBIENT) && !defined(ONLY_LIGHT) && !defined(ONLY_SPECULAR) && !defined(DIFFUSE) && !defined(NVR_SLOT_TIGHT) && !defined(NVR_PEARL_AMBIENT_LEAN)
        const float2 coatUvDx = ddx(IN.uv.xy);
        const float2 coatUvDy = ddy(IN.uv.xy);
    #endif
    #if defined(NVR_VANILLA_BRIDGE) && defined(OBJ_TOGGLES)
        BridgeSetup(normal.a, Toggles.z);	// UNOFFICIAL 2026-11-05 VanillaBridge (Object.hlsl): the authored highlight, before the roughness
    #endif
    float rough = getRoughness(normal.a);
    #ifdef HAIR
        // Keep unmapped hair matte enough not to look like glossy plastic (bright/DXT1 normal alpha).
        #ifdef RMAOS
            if (!RMAOS_ACTIVE) rough = max(rough, 0.58f);
        #else
            rough = max(rough, 0.58f);
        #endif
    #endif

    // Geometric specular AA -- see the comment on SpecularAA itself. Unconditional: this is a
    // quality fix for high-frequency normal maps (hair chief among them), not a debug toggle,
    // and ddx/ddy have to run here at top level regardless of anything below.
    rough = SpecularAA(normal.xyz, rough);
    // UNOFFICIAL 2026-10-13: blood on first-person geometry (Includes/HandBlood.hlsl); derivatives, so at top level.
    HandBlood(IN.uv.xy, IN.shadowWorldPos.xyz, SHADOW_VS_PRESENT(IN.shadowWorldPos.w));
    rough = HandBloodRoughness(rough);
    normal.xyz = HandBloodNormal(normal.xyz);
    rough = HudRainRoughness(rough);	// UNOFFICIAL: the rain drops, wet and rounded
    normal.xyz = HudRainNormal(normal.xyz);
    SetDiffuseEnergyKeep(normal.xyz, IN.viewDir.xyz, rough);	// UNOFFICIAL 2026-10-16 EnergySplit (Includes/PBR.hlsl): once per pixel, every lamp reuses it
    #ifdef NVR_FOLD
        // UNOFFICIAL FoldHighlights: at top level (ddx/ddy): the metal finish, and the base texture's gradients for the extra-lamp passes.
        const float foldMetalRoughness = SpecularAA(normal.xyz, getMetalRoughness(normal.a));
        #ifdef DIFFUSE
            const float2 foldUvDx = ddx(IN.uv.xy), foldUvDy = ddy(IN.uv.xy);
        #endif
    #endif

    //if (TESR_DebugVar.y > 0.0) {
    //    pout.color.a = 1;
    //    if (TESR_DebugVar.y > 0.1)
    //        pout.color.rgb = rough.xxx;
    //    else
    //        pout.color.rgb = normal.aaa;
    //    return pout;
    //}
    
    #ifdef OBJ_VCOLOR
        #ifdef HAIR
            float4 glowTexel = tex2D(GlowMap, IN.uv.xy);
            surface.rgb = (2 * ((IN.vertexColor.g * (EmittanceColor.rgb - 0.5)) + 0.5)) * lerp(surface.rgb, glowTexel.rgb, glowTexel.a);
        #else
            surface.rgb = OBJ_WITH_VCOLOR(surface.rgb, IN.vertexColor.rgb);
        #endif
    #endif
    #if !defined(DIFFUSE) && !defined(ONLY_SPECULAR) && !defined(ONLY_LIGHT)
        surface.rgb = HandBloodAlbedo(surface.rgb);	// UNOFFICIAL: the blood's colour (after the vertex colour: blood is not tinted by it)
    #elif HAND_BLOOD && !defined(ONLY_SPECULAR)
        // UNOFFICIAL 2026-10-13 (user: the blood on the gun looks clear in some lighting): the lighting-only passes (ONLY_LIGHT,
        // and the DIFFUSE lamp passes added to them) draw the light alone, and a later pass multiplies in the gun's texture, so
        // the blood's colour above never reached them: only its gloss and bumps showed. Here the light is scaled so that,
        // once multiplied by the texture, the blood comes out its own colour.
        #ifdef DIFFUSE
            // (No texture in these passes: the gun's texture taken as a dark grey, 0.25.)
            surface.rgb = handBloodCover > 0.0f ? HandBloodAlbedo((0.25f).xxx) * 4.0f : surface.rgb;
        #else
            surface.rgb = handBloodCover > 0.0f ? HandBloodAlbedo(heatMaterialFactor) / heatMaterialFactor : surface.rgb;	// (exactly as before where there is no blood)
        #endif
    #endif
    // UNOFFICIAL linear lighting: the albedo decoded for the lighting (where the texture is multiplied in by a later pass,
    // the factor above is, which comes out the same). The metal guess and the debug views keep the stored colour.
    const float3 albedoStored = surface.rgb;
    surface.rgb = LinColor(HudRainAlbedo(surface.rgb));	// UNOFFICIAL: darker under the rain drops (includes/HudRain.hlsl)

    #ifdef METAL
        // UNOFFICIAL: metal (Object.hlsl), only while [Shaders.PBR.Metal] is on for this pass; see the lighting below.
        // Its finish, specular-anti-aliased here at top level (ddx/ddy).
        float metalRoughness = SpecularAA(normal.xyz, getMetalRoughness(normal.a));
        float metallic = 0.0f;
    #endif

    // Vanilla shadows.
    float3 sunAndProj = 1.0;
    #ifdef STBB
        sunAndProj = 0.85;
    #elif defined(PROJ_SHADOW)
        float3 projShade = tex2D(ShadowMap, IN.shadowUVs.xy).xyz;
        float projMask = tex2D(ShadowMaskMap, IN.shadowUVs.zw).x;
        sunAndProj = lerp(1, projShade, projMask);
    #endif
    
    // Applied to PSLightColor[0], the sun, only: ambient, emittance and point lights are
    // untouched. ddx/ddy must stay at top level, outside any dynamic branch.
    //
    // Computed once here and reused by the ambient block further down (that block's condition,
    // !DIFFUSE && !ONLY_SPECULAR, is a strict subset of this one, since POINT implies
    // ONLY_SPECULAR) -- two separate ddx/ddy evaluations of the identical
    // GetShadowGeometricNormal(IN.shadowWorldPos.xyz) call previously coexisted in the same
    // pixel shader whenever FORWARD_SHADOWS was compiled in, which is fragile enough on its own
    // to corrupt unrelated interpolator reads placed nearby.
    #ifndef OBJ_LAMP_FIRST
        #ifdef OBJ_SMOOTH_NORMAL_INTERP
            #define OBJ_VERTEX_NORMAL IN.smoothNormal.xyz
            float3 faceNormal = ShadowFacingNormal(IN.smoothNormal.xyz, IN.shadowWorldPos.xyz);	// UNOFFICIAL 2026-10-24 SmoothObjectNormals: the vertex normal
        #elif defined(OBJ_SMOOTH_NORMAL_W)
            #define OBJ_VERTEX_NORMAL ShadowPlainNormal(float2(IN.lightDistSq.w, IN.lightDir.w), IN.vertexColor.a)
            float3 faceNormal = ShadowFacingNormal(ShadowPlainNormal(float2(IN.lightDistSq.w, IN.lightDir.w), IN.vertexColor.a), IN.shadowWorldPos.xyz);	// UNOFFICIAL 2026-10-24
        #else
            float3 faceNormal = GetShadowGeometricNormal(IN.shadowWorldPos.xyz);
        #endif
        #if CAPTURED_NORMALS_ON >= 4 && !defined(ONLY_SPECULAR)
            // UNOFFICIAL 2026-11-14 GBufferV2 (Includes/CapturedNormals.hlsl): this family's lamps are in tangent space only (no frame rows):
            // the normal-mapped normal through the screen-derivative frame about the facing normal (top level: ddx / ddy). 0 (none) where a
            // vanilla vertex shader ran (no world position).
            // (2026-11-16 fd-directlight) with the vertex normal (SmoothObjectNormals): a triangle seen from behind records its front's normal,
            // as this shader lights it (Includes/CapturedNormals.hlsl CapturedDerivNormalFront); with the flat face normal, as before.
            #ifdef OBJ_VERTEX_NORMAL
                #ifdef HAIR
                    #define OBJ_ALLOW_BACK true
                #else
                    #define OBJ_ALLOW_BACK (NVR_MATERIAL_CLASS > 2.5f || AmbientColor.a < 0.99f)
                #endif
                const float3 gbShadingNormal = SHADOW_VS_PRESENT(IN.shadowWorldPos.w) ? CapturedDerivNormalFront(IN.shadowWorldPos.xyz, IN.uv.xy, OBJ_VERTEX_NORMAL, normal.xyz, OBJ_ALLOW_BACK) : 0.0f;
                #undef OBJ_ALLOW_BACK
            #else
                const float3 gbShadingNormal = SHADOW_VS_PRESENT(IN.shadowWorldPos.w) ? CapturedDerivNormal(IN.shadowWorldPos.xyz, IN.uv.xy, faceNormal, normal.xyz) : 0.0f;
            #endif
        #endif
        #undef OBJ_VERTEX_NORMAL
        #if DUST_ON && !defined(HAIR) && !defined(DIFFUSE) && !defined(ONLY_SPECULAR) && !defined(ONLY_LIGHT)
            // 2026-11-04 dust on top surfaces (Includes/ObjectDust.hlsl): on the decoded colour, before any lighting (skinned meshes are
            // left out per draw: NVR_DustDraw)
            surface.rgb = DustCover(surface.rgb, faceNormal, IN.shadowWorldPos.xyz, SHADOW_VS_PRESENT(IN.shadowWorldPos.w));
        #endif
        // Decline to projShade if a vanilla vertex shader ran: the interpolator is undefined.
        float sunVis = 1.0f;
        #if FORWARD_SHADOWS && !defined(NVR_LIGHT_DEFERRED)	// (the lighting-free record: no sun, no sun shadow)
        #ifdef NVR_EYE
        // UNOFFICIAL 2026-11-19: sample sun shadow EYE_SHADOW_OFFSET units toward the sun (clears eyelids/brow socket while catching
        // hats, roofs, walls, cliffs, and clouds; bypasses SunShadowWanted so wrapped diffuse is shadowed too).
        sunVis = SHADOW_VS_PRESENT(IN.shadowWorldPos.w)
                  ? GetSunShadow(IN.shadowWorldPos.xyz + TESR_SmoothedSunDir.xyz * EYE_SHADOW_OFFSET, TESR_SmoothedSunDir.xyz)
                  : 1.0f;
        #elif UNLIT_SHADOW_SKIP
        // UNOFFICIAL 2026-10-17 UnlitShadowSkip: no projShade-map lookups where the sun cannot light the pixel (SunShadowWanted).
        [branch] if (SHADOW_VS_PRESENT(IN.shadowWorldPos.w) && SunShadowWanted(normal.xyz, IN.lightDir.xyz))
            sunVis = GetSunShadow(IN.shadowWorldPos.xyz, faceNormal);
        #else
        sunVis = SHADOW_VS_PRESENT(IN.shadowWorldPos.w)
                  ? GetSunShadow(IN.shadowWorldPos.xyz, faceNormal)
                  : 1.0f;
        #endif
        #endif
        sunAndProj *= sunVis;
    #endif

    #if INTERIOR_SHADOWS
        // UNOFFICIAL forward point-light shadows (Includes/PointShadowForward.hlsl). The normal offset uses the sun's
        // geometric normal where the shader has one. The point-light-only passes (DIFFUSE, POINT) have none and do not
        // get one: its ddx/ddy would cost more than these short passes' own work, so they rely on the depth bias alone,
        // as the screen-space pass always did.
        float3 ptWorldPos = IN.shadowWorldPos.xyz;
        #ifndef OBJ_LAMP_FIRST
            float3 ptNormal = faceNormal;
        #elif defined(OBJ_SMOOTH_NORMAL_INTERP)
            float3 ptNormal = ShadowFacingNormal(IN.smoothNormal.xyz, IN.shadowWorldPos.xyz);	// UNOFFICIAL 2026-10-24 SmoothObjectNormals: free here, no derivatives
        #else
            float3 ptNormal = 0.0f;
        #endif
        float ptValid = SHADOW_VS_PRESENT(IN.shadowWorldPos.w) ? 1.0f : 0.0f;
        float ptShadow0 = 1.0f, ptShadow1 = 1.0f, ptShadow2 = 1.0f;
        #ifndef NVR_NO_LAMPS	// (the lighting-free and lamps-deferred records: no lamp, no lookup)
        [branch] if (NVR_PointShadowParams.x > 0.0f) {
            // Light k of the draw is PSLightColor[k]; light 0 is a point light only in the DIFFUSE and POINT passes.
            #ifdef OBJ_LAMP_FIRST
                ptShadow0 = POINT_SHADOW_LOOKUP(0, LAMP_ATT_SQ(IN.lightDistSq.x, IN.lightDir));
            #endif
            #ifdef OBJ_LAMP2
                ptShadow1 = POINT_SHADOW_LOOKUP(1, LAMP_ATT_SQ(IN.lightDistSq.y, IN.light2Dir));
            #endif
            #ifdef OBJ_LAMP3
                ptShadow2 = POINT_SHADOW_LOOKUP(2, LAMP_ATT_SQ(IN.lightDistSq.z, IN.light3Dir));
            #endif
        }
        #endif
        float pointWeight = 0.0f;   // ONLY_SPECULAR POINT: this pass's blend weight without the point shadows (see the alpha)
    #endif

    #ifdef NVR_FOLD
    float3 lighting;
    #if CAPTURED_NORMALS_ON >= 3 && (defined(DIFFUSE) || defined(ONLY_LIGHT))
    float4 capFoldAlbedo = 0.0f;	// UNOFFICIAL 2026-10-31 AlbedoBuffer: the fold's own albedo, where the fold is on (see the output)
    #endif
    [branch] if (NVR_SpecFade.y > 0.5f) {
        // UNOFFICIAL FoldHighlights (see the top): these lamps' light as the one-pass shaders give it (Object.hlsl ...Full), divided
        // by the colour the texture pass multiplies in afterwards.
        #ifdef DIFFUSE
            float3 foldStored = tex2Dgrad(FoldBaseMap, IN.uv.xy, foldUvDx, foldUvDy).rgb;
        #else
            float3 foldStored = foldTexture;
        #endif
        foldStored *= NVR_SpecFade.z > 0.5f ? IN.foldColor.rgb : 1.0f;	// the game's TEXTURE_Vc pass multiplies the vertex colour in too
        const float3 foldAlbedo = LinColor(foldStored);
        #if CAPTURED_NORMALS_ON >= 3 && (defined(DIFFUSE) || defined(ONLY_LIGHT))
            capFoldAlbedo = float4(foldAlbedo, 1.0f);	// UNOFFICIAL 2026-10-31 AlbedoBuffer
        #endif
        const float foldValid = SHADOW_VS_PRESENT(IN.shadowWorldPos.w) ? 1.0f : 0.0f;
        float foldMetal = 0.0f;
        [branch] if (getMetalStrength() > 0.0f)
            foldMetal = getDerivedMetallic(normal.a, foldStored) * (foldValid > 0.0f ? getMetalFade(IN.shadowWorldPos.xyz) : 1.0f);
        const float foldRough = lerp(rough, foldMetalRoughness, foldMetal);
        #if !defined(DIFFUSE)
            lighting = getSunLightingFull(IN.lightDir.xyz, PSLightColor[0].rgb * sunAndProj, IN.viewDir.xyz, normal.xyz, foldAlbedo, foldRough, foldMetal);
            float3 foldView = normalize(IN.viewDir.xyz);
            float foldNdotV = saturate(dot(normal.xyz, foldView));
            float foldDielKS = 0.0f;
            #if defined(RMAOS) && !defined(NVR_SLOT_TIGHT)
                [branch] if (RMAOS_ACTIVE) foldDielKS = getDielectricEnvBRDF(foldRough, foldNdotV);
            #endif
            lighting += getAmbientLighting(AmbientColor.rgb, foldAlbedo * (1.0f - foldMetal) * (1.0f - foldDielKS), faceNormal, foldValid);
            #if defined(RMAOS) && !defined(NVR_SLOT_TIGHT)
            [branch] if (foldMetal > 0.0f || RMAOS_ACTIVE)
            #else
            [branch] if (foldMetal > 0.0f)
            #endif
            {
                lighting += getSurfaceIBL(AmbientColor.rgb, foldAlbedo, foldRough, foldNdotV,
                                          0.0f, 0.0f, IN.shadowWorldPos.xyz, faceNormal, faceNormal, saturate(foldView.z),
                                          foldMetal, foldDielKS);
            }
            float foldReach = 1.0f;
        #else
            const float foldAtt0 = LAMP_ATT_SQ(IN.lightDistSq.x, IN.lightDir);
            lighting = 0.0f;
            LAMP_IF(foldAtt0)
#if SHADOW_CONTRAST_ON
                { lighting = 0.0f; SC_SPLIT(getPointLightLightingAttFull(IN.lightDir.xyz, foldAtt0, PSLightColor[0].rgb * sunAndProj, IN.viewDir.xyz, normal.xyz, foldAlbedo, foldRough, foldMetal), POINT_SHADOW(0, foldAtt0)) }
#else
                lighting = getPointLightLightingAttFull(IN.lightDir.xyz, foldAtt0, SHADOWED(PSLightColor[0].rgb, 0, foldAtt0) * sunAndProj, IN.viewDir.xyz, normal.xyz, foldAlbedo, foldRough, foldMetal);
#endif
            float foldReach = foldAtt0;
        #endif
        #ifdef OBJ_LAMP2
            const float foldAtt2 = LAMP_ATT_SQ(IN.lightDistSq.y, IN.light2Dir);
            LAMP_IF(foldAtt2)
#if SHADOW_CONTRAST_ON
                SC_SPLIT(getPointLightLightingAttFull(IN.light2Dir.xyz, foldAtt2, PSLightColor[1].rgb, IN.viewDir.xyz, normal.xyz, foldAlbedo, foldRough, foldMetal), POINT_SHADOW(1, foldAtt2))
#else
                lighting += getPointLightLightingAttFull(IN.light2Dir.xyz, foldAtt2, SHADOWED(PSLightColor[1].rgb, 1, foldAtt2), IN.viewDir.xyz, normal.xyz, foldAlbedo, foldRough, foldMetal);
#endif
            foldReach = max(foldReach, foldAtt2);
        #endif
        #ifdef OBJ_LAMP3
            const float foldAtt3 = LAMP_ATT_SQ(IN.lightDistSq.z, IN.light3Dir);
            LAMP_IF(foldAtt3)
#if SHADOW_CONTRAST_ON
                SC_SPLIT(getPointLightLightingAttFull(IN.light3Dir.xyz, foldAtt3, PSLightColor[2].rgb, IN.viewDir.xyz, normal.xyz, foldAlbedo, foldRough, foldMetal), POINT_SHADOW(2, foldAtt3))
#else
                lighting += getPointLightLightingAttFull(IN.light3Dir.xyz, foldAtt3, SHADOWED(PSLightColor[2].rgb, 2, foldAtt3), IN.viewDir.xyz, normal.xyz, foldAlbedo, foldRough, foldMetal);
#endif
            foldReach = max(foldReach, foldAtt3);
        #endif
        // fp16 target: kept finite where the colour is (almost) black; the texture pass then multiplies it by (almost) nothing.
        lighting = min(lighting / max(foldAlbedo, 1e-6f), 60000.0f);
        #ifdef DIFFUSE
            clip((TESR_LampPassTrim.x > 0.0f && foldReach <= 0.0f && BarrelHeatMuzzle.w <= 0.0f) ? -1.0f : 0.0f);	// TrimLampPasses, as below
        #endif
    }
    else {
    #endif
    #if defined(METAL)
        // UNOFFICIAL metal (Object.hlsl). getMetalStrength() (TESR_PBRMetal.x, or the material rule's) is the same for every pixel of a draw, so this is one cheap
        // branch: off ([Shaders.PBR.Metal] 0 for this pass) runs the old lighting below exactly. On: how much of the pixel
        // is metal; the metal share has a tinted highlight (F0 = its colour) and no diffuse light, and reflects its
        // surroundings instead of the diffuse ambient (the sky along the reflected view, by world up from the VS).
        float skyValid = SHADOW_VS_PRESENT(IN.shadowWorldPos.w) ? 1.0f : 0.0f;
        #ifdef OBJ_LAMP2
            float att2 = LAMP_ATT_SQ(IN.lightDistSq.y, IN.light2Dir);
#if SHADOW_CONTRAST_ON
            float3 light2Color = PSLightColor[1].rgb;
            const float3 scShadow2 = POINT_SHADOW(1, att2);
#else
            float3 light2Color = SHADOWED(PSLightColor[1].rgb, 1, att2);
#endif
        #endif
        #ifdef OBJ_LAMP3
            float att3 = LAMP_ATT_SQ(IN.lightDistSq.z, IN.light3Dir);
#if SHADOW_CONTRAST_ON
            float3 light3Color = PSLightColor[2].rgb;
            const float3 scShadow3 = POINT_SHADOW(2, att3);
#else
            float3 light3Color = SHADOWED(PSLightColor[2].rgb, 2, att3);
#endif
        #endif
        float3 lighting;
        #ifdef OBJECT_WETNESS	// UNOFFICIAL 2026-11-04 Wetness Effects: the water layer, the albedo darkened before any light sees it
            #ifdef OBJ_AD_PASS
                const float3 wetDryTexture = heatMaterialFactor;
            #else
                const float3 wetDryTexture = 1.0f;
            #endif
            Wetness wet = (Wetness)0;
            wet.roughness = 1.0f;
            wet.normal = faceNormal;
            [branch] if (skyValid > 0.0f) wet = GetWetness(IN.shadowWorldPos.xyz, faceNormal, faceNormal OBJ_PUDDLE_HEIGHT(IN.uv.xy, IN.shadowWorldPos.xyz));
            #ifdef PUDDLES_COMPILED	// 2026-11-08 Puddles: standing water hides the normal map (the sun, lamps and ambient see a flat surface)
                normal.xyz = normalize(lerp(normal.xyz, float3(0.0f, 0.0f, 1.0f), wet.flatten));
            #endif
        #endif
        // P6A fix: share the sun/ambient/lamp lighting calls across metal and dielectric paths so 2-lamp specular variants don't double live temp registers.
        metallic = 0.0f;
        [branch] if (getMetalStrength() > 0.0f) {
            // Faded out with distance (getMetalFade); at full without the position (a vanilla vertex shader ran).
            metallic = HAND_BLOOD_METAL(getDerivedMetallic(normal.a, albedoStored) * (skyValid > 0.0f ? getMetalFade(IN.shadowWorldPos.xyz) : 1.0f));
            #if defined(RMAOS) && defined(OBJ_AMBIENT) && !defined(ONLY_LIGHT) && !defined(ONLY_SPECULAR) && !defined(DIFFUSE) && !defined(NVR_SLOT_TIGHT) && !defined(HAIR) && !defined(NVR_PEARL_AMBIENT_LEAN)
                [branch] if (TESR_SharpSky.y > 0.001f && RMAOS_ACTIVE && metallic > 0.55f) {
                    float3 vTan = normalize(IN.viewDir.xyz);
                    float2 coatUvOff = -(vTan.xy / max(vTan.z, 0.35f)) * (0.0018f * saturate(TESR_SharpSky.y));
                    float3 subCol = tex2Dgrad(BaseMap, IN.uv.xy + coatUvOff, coatUvDx, coatUvDy).rgb;
                    #ifdef OBJ_VCOLOR
                        subCol = OBJ_WITH_VCOLOR(subCol, IN.vertexColor.rgb);
                    #endif
                    subCol = LinColor(HudRainAlbedo(HandBloodAlbedo(subCol)));
                    float subW = smoothstep(0.55f, 0.90f, metallic) * saturate(TESR_SharpSky.y) * 0.72f;
                    surface.rgb = lerp(surface.rgb, subCol, subW);
                }
            #endif
        }
        metalRoughness = lerp(rough, metalRoughness, metallic);
        #ifdef OBJECT_WETNESS
            surface.rgb = ObjectWetAlbedo(surface.rgb, wetDryTexture, wet, 1.0f - saturate(sqrt(metallic)));	// metals darken less (CS's porosity)
        #endif
        lighting = getSunLighting(IN.lightDir.xyz, PSLightColor[0].rgb * sunAndProj, IN.viewDir.xyz, normal.xyz, surface.rgb, metalRoughness, metallic);
        float3 metalView = normalize(IN.viewDir.xyz);
        float metalNdotV = saturate(dot(normal.xyz, metalView));
        float dielKS = 0.0f;
        #if defined(RMAOS) && !defined(NVR_SLOT_TIGHT)
            [branch] if (RMAOS_ACTIVE) dielKS = getDielectricEnvBRDF(metalRoughness, metalNdotV);
        #endif
        lighting += getAmbientLighting(AmbientColor.rgb, surface.rgb * (1.0f - metallic) * (1.0f - dielKS), faceNormal, skyValid);
        #if defined(RMAOS) && !defined(NVR_SLOT_TIGHT)
        [branch] if (metallic > 0.0f || RMAOS_ACTIVE)
        #else
        [branch] if (metallic > 0.0f)
        #endif
        {
            #if CAPTURED_NORMALS_ON >= 4 && !defined(ONLY_SPECULAR)
                float3 metalWorldNormal = dot(gbShadingNormal, gbShadingNormal) > 1.0e-4f ? normalize(gbShadingNormal) : faceNormal;
            #else
                float3 metalWorldNormal = faceNormal;
            #endif
            #ifdef OBJ_SMOOTH_NORMAL_W
            // UNOFFICIAL 2026-10-24: lightDistSq.w / lightDir.w carry the smooth normal now; the reflection's height in world space from it
            // (camera-relative position: the eye is at the origin; world up is +z), with the captured shading normal when available.
            bool upKnown = skyValid > 0.0f;
            float upReflect = upKnown ? reflect(normalize(IN.shadowWorldPos.xyz), metalWorldNormal).z : 0.0f;
            #else
            float3 upTangent = float3(IN.lightDistSq.w, IN.lightDir.w, IN.vertexColor.a * 2.0f - 1.0f);
            float upLength = length(upTangent);
            bool upKnown = upLength > 0.5f && skyValid > 0.0f;
            float upReflect = upKnown ? dot(reflect(-metalView, normal.xyz), upTangent / upLength) : 0.0f;
            #endif
            lighting += getSurfaceIBL(AmbientColor.rgb, surface.rgb, metalRoughness, metalNdotV,
                                      upReflect, upKnown ? 1.0f : 0.0f,
                                      IN.shadowWorldPos.xyz, metalWorldNormal, faceNormal, saturate(metalView.z),
                                      metallic, dielKS);
        }
        #ifdef OBJ_LAMP2
            LAMP_IF(att2)   // UNOFFICIAL LampReachSkip
#if SHADOW_CONTRAST_ON
                SC_SPLIT(getPointLightLightingAtt(IN.light2Dir.xyz, att2, light2Color, IN.viewDir.xyz, normal.xyz, surface.rgb, metalRoughness, metallic), scShadow2)
#else
                lighting += getPointLightLightingAtt(IN.light2Dir.xyz, att2, light2Color, IN.viewDir.xyz, normal.xyz, surface.rgb, metalRoughness, metallic);
#endif
        #endif
        #ifdef OBJ_LAMP3
            LAMP_IF(att3)   // UNOFFICIAL LampReachSkip
#if SHADOW_CONTRAST_ON
                SC_SPLIT(getPointLightLightingAtt(IN.light3Dir.xyz, att3, light3Color, IN.viewDir.xyz, normal.xyz, surface.rgb, metalRoughness, metallic), scShadow3)
#else
                lighting += getPointLightLightingAtt(IN.light3Dir.xyz, att3, light3Color, IN.viewDir.xyz, normal.xyz, surface.rgb, metalRoughness, metallic);
#endif
        #endif
        #ifdef OBJECT_WETNESS	// the water film: the sun's highlight on it and its reflection, over the material
            [branch] if (wet.gloss > 0.001f)
                lighting += ObjectWetFilm(wet, IN.shadowWorldPos.xyz, faceNormal, PSLightColor[0].rgb * sunAndProj * LIN_SCALE_LIGHT, wetDryTexture);
            #if defined(PUDDLES_COMPILED) && !defined(PUDDLES_LEAN) && (defined(OBJ_LAMP2) || defined(OBJ_LAMP3))
            // 2026-11-12 Puddles: the draw's lamps on the wet film (wet roads and slabs that are objects shine under street lamps)
            [branch] if (wet.gloss > 0.05f) {
                const float3 filmV = normalize(IN.viewDir.xyz);
                #if SHADOW_CONTRAST_ON	// (there the colours are unshadowed and the shadow comes separately)
                    #define OBJ_FILM_SHADOW2 * scShadow2
                    #define OBJ_FILM_SHADOW3 * scShadow3
                #else
                    #define OBJ_FILM_SHADOW2
                    #define OBJ_FILM_SHADOW3
                #endif
                #ifdef OBJ_LAMP2
                    lighting += ObjectLampFilm(wet, normal.xyz, filmV, IN.light2Dir.xyz, light2Color * (att2 * LIN_SCALE_LIGHT) OBJ_FILM_SHADOW2, wetDryTexture);
                #endif
                #ifdef OBJ_LAMP3
                    lighting += ObjectLampFilm(wet, normal.xyz, filmV, IN.light3Dir.xyz, light3Color * (att3 * LIN_SCALE_LIGHT) OBJ_FILM_SHADOW3, wetDryTexture);
                #endif
            }
            #endif
        #endif
    #else
    #ifndef OBJ_LAMP_FIRST
        #ifdef OBJECT_WETNESS	// UNOFFICIAL 2026-11-04 Wetness Effects (as in the METAL path above)
            #ifdef OBJ_AD_PASS
                const float3 wetDryTexture = heatMaterialFactor;
            #else
                const float3 wetDryTexture = 1.0f;
            #endif
            Wetness wet = (Wetness)0;
            wet.roughness = 1.0f;
            wet.normal = faceNormal;
            [branch] if (SHADOW_VS_PRESENT(IN.shadowWorldPos.w)) wet = GetWetness(IN.shadowWorldPos.xyz, faceNormal, faceNormal OBJ_PUDDLE_HEIGHT(IN.uv.xy, IN.shadowWorldPos.xyz));
            #ifdef PUDDLES_COMPILED	// 2026-11-08 Puddles: standing water hides the normal map (the sun, lamps and ambient see a flat surface)
                normal.xyz = normalize(lerp(normal.xyz, float3(0.0f, 0.0f, 1.0f), wet.flatten));
            #endif
            surface.rgb = ObjectWetAlbedo(surface.rgb, wetDryTexture, wet, 1.0f);
        #endif
        FOLD_DECL lighting = getSunLighting(IN.lightDir.xyz, PSLightColor[0].rgb * sunAndProj, IN.viewDir.xyz, normal.xyz, surface.rgb, rough);
    #else
        // Pointlights only. Attenuate from the object-space lightDistSq.x carried from the VS,
        // not length(IN.lightDir.xyz) -- that vector is tangent-space (TBN-transformed) and its
        // length is only correct if the TBN basis is orthonormal.
        float att0 = LAMP_ATT_SQ(IN.lightDistSq.x, IN.lightDir);
        #if INTERIOR_SHADOWS && defined(POINT)
            FOLD_DECL lighting = 0.0f;
            LAMP_IF(att0) {   // UNOFFICIAL LampReachSkip
                lighting = getPointLightLightingAtt(IN.lightDir.xyz, att0, PSLightColor[0].rgb * sunAndProj, IN.viewDir.xyz, normal.xyz, surface.rgb, rough);
                pointWeight += weight(lighting);
                lighting *= POINT_SHADOW(0, att0);
            }
        #else
            FOLD_DECL lighting = 0.0f;
            LAMP_IF(att0)   // UNOFFICIAL LampReachSkip
#if SHADOW_CONTRAST_ON
                { lighting = 0.0f; SC_SPLIT(getPointLightLightingAtt(IN.lightDir.xyz, att0, PSLightColor[0].rgb * sunAndProj, IN.viewDir.xyz, normal.xyz, surface.rgb, rough), POINT_SHADOW(0, att0)) }
#else
                lighting = getPointLightLightingAtt(IN.lightDir.xyz, att0, SHADOWED(PSLightColor[0].rgb, 0, att0) * sunAndProj, IN.viewDir.xyz, normal.xyz, surface.rgb, rough);
#endif
        #endif
    #endif
    
    #ifdef OBJ_AMBIENT
        // Reuses faceNormal computed above -- see the comment there. Always in scope
        // here: POINT implies ONLY_SPECULAR, so !ONLY_SPECULAR implies !POINT.
        lighting += getAmbientLighting(AmbientColor.rgb, surface.rgb, faceNormal,
                                       SHADOW_VS_PRESENT(IN.shadowWorldPos.w) ? 1.0f : 0.0f);
    #endif
    #if defined(OBJECT_WETNESS) && !defined(METAL)	// UNOFFICIAL 2026-11-04 Wetness Effects: the water film (the METAL path adds its own above)
        [branch] if (wet.gloss > 0.001f)
            lighting += ObjectWetFilm(wet, IN.shadowWorldPos.xyz, faceNormal, PSLightColor[0].rgb * sunAndProj * LIN_SCALE_LIGHT, wetDryTexture);
    #endif

    // Other light sources. Same object-space attenuation fix as light0 above.
    #ifdef OBJ_LAMP2
        float att2 = LAMP_ATT_SQ(IN.lightDistSq.y, IN.light2Dir);
        #if INTERIOR_SHADOWS && defined(POINT)
            LAMP_IF(att2) {   // UNOFFICIAL LampReachSkip
                float3 light2Term = getPointLightLightingAtt(IN.light2Dir.xyz, att2, PSLightColor[1].rgb, IN.viewDir.xyz, normal.xyz, surface.rgb, rough);
                pointWeight += weight(light2Term);
                lighting += light2Term * POINT_SHADOW(1, att2);
            }
        #else
            LAMP_IF(att2)   // UNOFFICIAL LampReachSkip
#if SHADOW_CONTRAST_ON
                SC_SPLIT(getPointLightLightingAtt(IN.light2Dir.xyz, att2, PSLightColor[1].rgb, IN.viewDir.xyz, normal.xyz, surface.rgb, rough), POINT_SHADOW(1, att2))
#else
                lighting += getPointLightLightingAtt(IN.light2Dir.xyz, att2, SHADOWED(PSLightColor[1].rgb, 1, att2), IN.viewDir.xyz, normal.xyz, surface.rgb, rough);
#endif
        #endif
    #endif

    #ifdef OBJ_LAMP3
        float att3 = LAMP_ATT_SQ(IN.lightDistSq.z, IN.light3Dir);
        #if INTERIOR_SHADOWS && defined(POINT)
            LAMP_IF(att3) {   // UNOFFICIAL LampReachSkip
                float3 light3Term = getPointLightLightingAtt(IN.light3Dir.xyz, att3, PSLightColor[2].rgb, IN.viewDir.xyz, normal.xyz, surface.rgb, rough);
                pointWeight += weight(light3Term);
                lighting += light3Term * POINT_SHADOW(2, att3);
            }
        #else
            LAMP_IF(att3)   // UNOFFICIAL LampReachSkip
#if SHADOW_CONTRAST_ON
                SC_SPLIT(getPointLightLightingAtt(IN.light3Dir.xyz, att3, PSLightColor[2].rgb, IN.viewDir.xyz, normal.xyz, surface.rgb, rough), POINT_SHADOW(2, att3))
#else
                lighting += getPointLightLightingAtt(IN.light3Dir.xyz, att3, SHADOWED(PSLightColor[2].rgb, 2, att3), IN.viewDir.xyz, normal.xyz, surface.rgb, rough);
#endif
        #endif
    #endif

    #ifdef OBJ_LAMP_FIRST
        // UNOFFICIAL TrimLampPasses ([Main.Main.Performance], TESR_LampPassTrim.x, 2026-10-11): these passes add lamp light
        // on top of an object already drawn (additive, no fog, no depth write). Where the pixel is outside every lamp's
        // reach each lamp adds exactly 0 (vanillaAttSq), so the pixel is dropped instead of being blended in as 0
        // (POINT: alpha, the blend weight, is 0 there too). Not while the GunFX barrel glowTexel is on (it adds its own colour).
        float lampReach = att0;
        #ifdef OBJ_LAMP2
            lampReach = max(lampReach, att2);
        #endif
        #ifdef OBJ_LAMP3
            lampReach = max(lampReach, att3);
        #endif
        clip((TESR_LampPassTrim.x > 0.0f && lampReach <= 0.0f && BarrelHeatMuzzle.w <= 0.0f) ? -1.0f : 0.0f);
    #endif

    #endif // METAL
    #ifdef NVR_FOLD
    }
    #endif
    #if defined(NVR_FORWARD_PLUS) && !defined(OBJ_LAMP_FIRST)
        // UNOFFICIAL 2026-11-15 DeferredTransparents (Includes/ForwardPlus.hlsl): every lamp of the pixel's froxel from the frame's table, in
        // place of the game's lamps compiled out above (the bumped normal: G-buffer v2's derivative frame where it is compiled in)
        #if CAPTURED_NORMALS_ON >= 4 && !defined(ONLY_SPECULAR)
            lighting += FPLampSum(IN.shadowWorldPos.xyz, FPUnit(gbShadingNormal, faceNormal), SHADOW_VS_PRESENT(IN.shadowWorldPos.w) ? 1.0f : 0.0f, surface.rgb, rough, 0.0f);
        #else
            lighting += FPLampSum(IN.shadowWorldPos.xyz, FPUnit(faceNormal, faceNormal), SHADOW_VS_PRESENT(IN.shadowWorldPos.w) ? 1.0f : 0.0f, surface.rgb, rough, 0.0f);
        #endif
    #endif

    #if SHADOW_CONTRAST_ON && !defined(DIFFUSE) && !defined(POINT)
        lighting *= ShadowContrastKeep(lighting, scLost);	// UNOFFICIAL ShadowContrast: a base pass has the ambient: darkened here
    #endif
    // UNOFFICIAL 2026-11-23 DecoupleGlowMaps: attenuate scene lighting on glowing texels so ambient/light scales don't wash out glow maps.
    #ifdef SI
        float3 glowRaw = tex2D(GlowMap, IN.uv.xy).rgb * EmittanceColor.rgb;
        float glowDecouple = GlowDecoupleMask(glowRaw);
        lighting = lighting * (1.0f - glowDecouple) + surface.rgb * LinColor(glowRaw) * GlowScaleFactor();
    #endif
    #if defined(NVR_EYE) && defined(OBJ_AMBIENT) && !defined(NVR_LIGHT_DEFERRED)	// (the lighting-free record: the light pass lights the eye)
        // UNOFFICIAL 2026-11-12 HairAndEyes: the flashlight's beam on the eyeball, once (the pass that adds the ambient), on the colour the
        // lamps above lit (a light-only pass: the texture pass multiplies its colour in afterwards, as for them)
        lighting += EyeBeam(surface.rgb, faceNormal, IN.shadowWorldPos.xyz, SHADOW_VS_PRESENT(IN.shadowWorldPos.w));
    #endif
    float3 shaded = LinEncode(lighting.rgb);	// UNOFFICIAL linear lighting: back to the stored (gamma) space
    #ifdef ONLY_LIGHT
        // UNOFFICIAL 2026-10-16 ExactPassSums (core/PassSums.h): while NVR redirects this run of added passes into its own buffer,
        // (the extra lamp passes use the DIFFUSE and the plain light-only shaders; the flag is set only during a redirected run)
        // the pass gives its LINEAR light; the buffer is folded into the frame once, in linear light, when the run ends.
        [flatten] if (TESR_PassRedirect.x > 0.5f) shaded = max(lighting.rgb, 0.0f);
    #endif
    #ifdef NVR_LAMP_SUMS
        // UNOFFICIAL 2026-10-21 LampSums (see the top): c210 y 1 during the world's run of extra lamp passes, zw 1 / the frame's size.
        [branch] if (TESR_PassRedirect.y > 0.5f) {
            const float3 runStart = max(tex2Dlod(NVR_LampRunStart, float4((NVR_IN_VPOS + 0.5f) * TESR_PassRedirect.zw, 0.0f, 0.0f)).rgb, 0.0f);
            shaded = LinEncode(LinColor(runStart) + max(lighting.rgb, 0.0f)) - runStart;
        }
    #endif
    shaded = MaterialRulesDebug(shaded, albedoStored);   // UNOFFICIAL [Shaders.PBR.MaterialRules] DebugView
    shaded = RmaosDebug(shaded, albedoStored);           // UNOFFICIAL [Shaders.PBR.Rmaos] DebugView

    #ifdef METAL
        // [Shaders.PBR.Metal] DebugView: metal in orange over a dim grey image.
        [branch] if (TESR_PBRMetalLook.w > 0.5f) shaded = lerp(luma(albedoStored).xxx * 0.35f, float3(1.0f, 0.45f, 0.05f), metallic);
    #endif
    
    // Fog.
    #ifdef OBJ_FOG
        shaded.rgb = OBJ_WITH_FOG(shaded.rgb, IN.fogColor);
    #endif
    

#if SHADOW_FORCE_MARKER
    shaded.rgb = OBJ_MARKER_COLOR;
#endif

#ifdef OBJ_AD_PASS
    pout.color.rgb = ApplyBarrelHeat(shaded.rgb, BarrelHeatMask(float3(IN.uv.zw, IN.viewDir.w)), heatMaterialFactor);
#else
    pout.color.rgb = ApplyBarrelHeat(shaded.rgb, BarrelHeatMask(float3(IN.uv.zw, IN.viewDir.w)));
#endif

#if INTERIOR_SHADOWS && !defined(NVR_NO_LAMPS)	// (the lighting-free / lamps-deferred records: these views trace the lamp shadows they have not got)
    // UNOFFICIAL DebugView 3 (PointShadowForward.hlsl PointShadowTrace): the base pass shows it, the passes that add more
    // light on top (DIFFUSE, ONLY_SPECULAR) add nothing.
    [branch] if (NVR_PointShadowDebug.x > 2.5f) {
        #ifndef OBJ_AMBIENT
            pout.color.rgb = 0.0f;
        #else
            pout.color.rgb = PointShadowTrace(ptWorldPos, ptNormal, ptValid);
        #endif
    }
    #if POINT_SHADOW_RT
        // UNOFFICIAL 2026-10-29 RT coverage view (PointShadowForward.hlsl PointShadowRtCoverage); the added passes add nothing
        [branch] if (NVR_PointShadowRT.x > 2.5f) {
            #ifndef OBJ_AMBIENT
                pout.color.rgb = 0.0f;
            #else
                pout.color.rgb = PointShadowRtCoverage(ptWorldPos);
            #endif
        }
    #endif
    #if LAMP_CAPS_VIEW
        // UNOFFICIAL 2026-10-31 LampCapsCensus view 4 (PointShadowForward.hlsl NVR_CapsUnshadowed): red where a lamp with no projShade source lights this pass
        [branch] if (NVR_PointShadowDebug.x > 3.5f) pout.color.rgb = float3(NVR_CapsUnshadowed > 0.5f ? 1.0f : 0.0f, 0.0f, 0.0f);
    #endif
#endif

    #ifdef DIFFUSE
        pout.color.a = 1;
    #elif defined(ONLY_SPECULAR)
        #ifndef OBJ_LAMP_FIRST
            // Alpha here is a blend WEIGHT (this pass's own brightness, used to fade
            // its additive specular/sheen layer smoothly), not a darkness value --
            // unlike the colour output, it must not shrink with sun projShade, or the
            // whole layer fades toward invisible in projShade instead of just going
            // dark. That read as "alpha blending broken" under Forward Shadows
            // specifically, because sunVis is the one new factor it added to
            // sunAndProj here (STBB/PROJ_SHADOW's own contribution predates
            // this and is left alone). Dividing it back out recovers the weight
            // this pass would have produced without Forward Shadows; harmless and
            // exact when Forward Shadows is compiled out, since sunVis is then
            // fixed at 1.0.
            pout.color.a = weight(shaded.rgb) / max(sunVis, 0.05f);
        #elif INTERIOR_SHADOWS
            // The same for the point lights' shadows: the weight is what the lights would give unshadowed.
            pout.color.a = pointWeight;
        #else
            pout.color.a = weight(shaded.rgb);
        #endif
    #elif defined(ONLY_LIGHT)
        pout.color.a = ReflMaterialAlpha(OBJ_CUT_ALPHA);	// UNOFFICIAL MaterialAware reflections (Object.hlsl)
    #else
        pout.color.a = ReflMaterialAlpha(OBJ_CUT_ALPHA * AmbientColor.a);
        #define CAP_COVERAGE (OBJ_CUT_ALPHA * AmbientColor.a)	// UNOFFICIAL 2026-11-12 CapturedCoverage: the draw's own alpha, before the material code (the one-pass records only: a light-only pass's alpha is the texture's alone)
    #endif
    #ifdef ONLY_SPECULAR
        pout.highlightCopy = pout.color;   // UNOFFICIAL NewSkin: alpha too, so any blend the pass uses adds alike to both targets
    #endif
#if CAPTURED_NORMALS_ON
    #if !defined(ONLY_SPECULAR)
        // UNOFFICIAL 2026-10-31 AlbedoBuffer (Includes/CapturedNormals.hlsl): COLOR1 = the linear colour the light is multiplied by. The DIFFUSE
        // lamp passes have no texture of their own: unwritten, unless FoldHighlights bound the object's base map on s7 (then the fold's albedo);
        // the ONLY_LIGHT base passes take the texture the game's TEXTURE pass multiplies in afterwards (s0, as FoldHighlights' light pass
        // reads it: foldTexture) times the vertex colour, as the one-pass shader's surface is made.
        #ifdef DIFFUSE
            #if CAPTURED_NORMALS_ON >= 3 && defined(NVR_FOLD)
                pout.capturedPad1 = capFoldAlbedo;
            #else
                pout.capturedPad1 = CAPTURED_ALBEDO_NONE;
            #endif
        #elif defined(ONLY_LIGHT) && CAPTURED_NORMALS_ON >= 3
            #ifdef OBJ_AD_GBUFFER
                // UNOFFICIAL 2026-11-16 fd-batchE (audit item 20; see the top): the deferred record's albedo with the vertex colour where the
                // game's TEXTURE pass multiplies it (the draw's flag in NVR_SpecFade.z: 1, or the blended mark 3)
                float3 capBase = LinColor(capBaseTex * (NVR_SpecFade.z > 0.5f ? IN.foldColor.rgb : 1.0f)).rgb;
            #elif defined(NO_VERTEX_COLOR)
                float3 capBase = LinColor(capBaseTex).rgb;
            #else
                float3 capBase = LinColor(capBaseTex * IN.vertexColor.rgb).rgb;
            #endif
            #if DUST_ON && !defined(HAIR) && !defined(OBJ_LAMP_FIRST)
                // UNOFFICIAL 2026-11-12: the dust goes on in the game's later TEXTURE pass, which writes no record, so this pass's record had
                // the bare texture: under path tracing (which reads the record) such objects lost their dust. The same dust as that pass.
                capBase = DustCover(capBase, faceNormal, IN.shadowWorldPos.xyz, SHADOW_VS_PRESENT(IN.shadowWorldPos.w));
            #endif
            #ifdef OBJ_AD_GBUFFER
                // UNOFFICIAL 2026-11-16 fd-batchE (audit item 26): the factor this pass scaled its own light by (surface.rgb is 1 in a light-only
                // pass, times the wet darkening's ratio, hand blood's and the rain drops'), so the light the deferred passes add is what this
                // pass's light would have been on the wet surface (it was the dry texture's)
                capBase *= surface.rgb;
            #endif
            #ifdef SI
                capBase *= (1.0f - glowDecouple);
            #endif
            float4 capAlbedo = CapturedAlbedoOut(capBase);
            #ifdef NVR_FOLD
                pout.capturedPad1 = NVR_SpecFade.y > 0.5f ? capFoldAlbedo : capAlbedo;
            #else
                pout.capturedPad1 = capAlbedo;
            #endif
        #else
            #ifdef SI
                pout.capturedPad1 = CapturedAlbedoOut(surface.rgb * (1.0f - glowDecouple));
            #else
                pout.capturedPad1 = CapturedAlbedoOut(surface.rgb);
            #endif
        #endif
    #endif
    // (2026-11-12 ptbump: the *NoHighlight forms, Includes/CapturedNormals.hlsl; the same values unless the draw is flagged for the path tracer)
    #ifdef SI
        #ifdef METAL
            pout.capturedPad2 = CapturedMaterialOutMetalNoHighlight(lerp(metallic > 0.0f ? metalRoughness : rough, 1.0f, glowDecouple), metallic * (1.0f - glowDecouple));
        #else
            pout.capturedPad2 = CapturedMaterialOutNoHighlight(lerp(rough, 1.0f, glowDecouple));
        #endif
    #elif defined(METAL)	// UNOFFICIAL 2026-10-31: the metal share and the finish it was lit with (metalRoughness is lerped where metallic > 0)
        pout.capturedPad2 = CapturedMaterialOutMetalNoHighlight(metallic > 0.0f ? metalRoughness : rough, metallic);
    #else
        pout.capturedPad2 = CapturedMaterialOutNoHighlight(rough);	// UNOFFICIAL 2026-10-29 MaterialBuffer (0 without it)
    #endif
    #if defined(OBJECT_WETNESS) && defined(PUDDLES_COMPILED) && CAPTURED_NORMALS_ON >= 2
        pout.capturedPad2.a += PuddleStandingOut;	// 2026-11-12 Puddles: the standing water drawn here, for the SSR/RT pass (Reflections.glsl)
    #endif
    #if SHADOW_CONTRAST_ON && defined(DIFFUSE)
        // UNOFFICIAL ShadowContrast: an added lamp pass gives the light its shadows took (alpha only: core/CapturedNormals.h masks rgb off)
        pout.capturedPad2 = float4(0.0f, 0.0f, 0.0f, dot(max(scLost, 0.0f), float3(0.2126f, 0.7152f, 0.0722f)));
    #endif
    #if CAPTURED_NORMALS_ON >= 4 && !defined(OBJ_LAMP_FIRST) && !defined(ONLY_SPECULAR)
        pout.capturedPad2 = CapturedShadingNormal(pout.capturedPad2, gbShadingNormal);	// UNOFFICIAL 2026-11-14 GBufferV2: g / a
    #endif
    #if defined(NVR_FOLD) && defined(NVR_GBUFFER_LIT) && CAPTURED_NORMALS_ON >= 4
        // UNOFFICIAL 2026-11-15 (bench batch 6, Vault 21 darker deferred): the light-only pass of a shiny object that FoldHighlights folded
        // (NVR_SpecFade.y, per draw) is not a SPECULAR record, so it wrote "no highlight"; but its highlight passes are skipped (FoldHighlights:
        // drawn with its lamps), so in a deferred frame nobody drew its lamps' highlights. Folded: the specular bit, the lamp pass draws them.
        [flatten] if (NVR_SpecFade.y > 0.5f && pout.capturedPad2.r > 0.5f && pout.capturedPad2.r < 15.5f) pout.capturedPad2.r += GBV2_SPEC_BIT;
    #endif
    // UNOFFICIAL 2026-10-28 CapturedNormals: the smooth vertex normal, before the normal map. The added lamp and highlight passes write
    // none (they are masked off anyway); nor does the slot-tight six-lamp shader (the depth-rebuilt normal stays in use there).
    #if defined(DIFFUSE) || defined(ONLY_SPECULAR) || defined(NVR_SLOT_TIGHT)
        pout.capturedNormal = CAPTURED_NORMAL_INVALID;
    #elif defined(OBJ_SMOOTH_NORMAL_INTERP)
        pout.capturedNormal = OBJ_CAPTURED_NORMAL_OUT(IN.smoothNormal.xyz, IN.shadowWorldPos.xyz);
    #elif defined(OBJ_SMOOTH_NORMAL_W)
        pout.capturedNormal = OBJ_CAPTURED_NORMAL_OUT(ShadowPlainNormal(float2(IN.lightDistSq.w, IN.lightDir.w), IN.vertexColor.a), IN.shadowWorldPos.xyz);
    #elif defined(NVR_GBUFFER_LIT) && !defined(OBJ_LAMP_FIRST)
        // UNOFFICIAL 2026-11-14 the lighting-free record always writes it: the light passes take the pixel's place from its distance
        // (SmoothObjectNormals off: the geometric normal the sun's lookup used)
        pout.capturedNormal = CapturedNormalOut(faceNormal, IN.shadowWorldPos.xyz);	// (geometric here: already the triangle's side)
    #else
        pout.capturedNormal = CAPTURED_NORMAL_INVALID;	// SmoothObjectNormals off: no vertex normal here
    #endif
    #if defined(NVR_EYE) && defined(OBJ_AMBIENT) && !defined(NVR_LIGHT_DEFERRED)
        // UNOFFICIAL 2026-11-12 HairAndEyes (Includes/FlashBeam.hlsl): while the eye takes the flashlight's beam itself (above) its record
        // says "far away", so the light pass does not light it a second time. Its colour, material and normal stay.
        [flatten] if (FLASH_BEAM_ON && SHADOW_VS_PRESENT(IN.shadowWorldPos.w) && pout.capturedNormal.a > 0.5f) pout.capturedNormal.b = FLASH_BEAM_FAR;
    #endif
    #if defined(FD_SV_RECORD) && !defined(DIFFUSE) && !defined(ONLY_SPECULAR) && !defined(NVR_EYE)
        // UNOFFICIAL 2026-11-16 / 2026-11-21 fd-sunvis (Includes/DeferredPixel.hlsl FDSunVisCode): sun vis (besides cascades) and non-metal F0 (OBJ_LAMP_FIRST records F0 with vis 1.0f for DeferredLamps / FlashlightDeferred)
        #ifdef SI
            const float fdGlowF0 = FD_SV_F0 * (1.0f - glowDecouple);
        #else
            const float fdGlowF0 = FD_SV_F0;
        #endif
        #ifdef OBJ_LAMP_FIRST
            pout.capturedNormal.a = FDSunVisCode(pout.capturedNormal.a, 1.0f, fdGlowF0, NVR_SpecFade.z);
        #else
            pout.capturedNormal.a = FDSunVisCode(pout.capturedNormal.a, FDSunVisCanopy(sunAndProj) * FDSunVisMicro(normal.xyz, IN.lightDir.xyz), fdGlowF0, NVR_SpecFade.z);
        #endif
    #endif
    #if defined(NVR_LIGHT_DEFERRED) && CAPTURED_NORMALS_ON >= 3 && !defined(DIFFUSE) && !defined(ONLY_SPECULAR)
        // UNOFFICIAL 2026-11-14 the lighting-free record's COLOR1.a, its one writer (Includes/DeferredPixel.hlsl FD_LF_CODE, both layouts): the
        // share of the light the passes add later that this draw's fog leaves (1 - f), with whether this record had a sun term for DeferredSun
        // to give back (not where light 0 is a lamp, not the eye's EyeLight) and, without GBufferV2, the model it lit with (SPECULAR: PBRSun;
        // the folded light-only pass: getSunLightingFull's PBRSun while the fold is on; else PBRDiffuse; GBufferV2 has its specular bit)
        #ifdef OBJ_FOG
            #define FD_LF_KEEP OBJ_FOG_KEEP(IN.fogColor)
        #elif defined(OBJ_AD_GBUFFER) && defined(AD_FOG_KEEP_ON)
            #define FD_LF_KEEP (1.0f - IN.foldColor.a)	// UNOFFICIAL 2026-11-16 fd-batchE: AD's fog comes in the game's later FOG pass (see the top)
        #else
            #define FD_LF_KEEP 1.0f
        #endif
        #if defined(OBJ_LAMP_FIRST) || defined(NVR_EYE)
            pout.capturedPad1.a = FD_LF_CODE(FD_LF_NOSUN, FD_LF_KEEP);
        #elif defined(SPECULAR)
            pout.capturedPad1.a = FD_LF_CODE(FD_LF_SPEC, FD_LF_KEEP);
        #elif defined(NVR_FOLD)
            pout.capturedPad1.a = FD_LF_CODE(NVR_SpecFade.y > 0.5f ? FD_LF_SPEC : FD_LF_MATTE, FD_LF_KEEP);
        #else
            pout.capturedPad1.a = FD_LF_CODE(FD_LF_MATTE, FD_LF_KEEP);
        #endif
        #undef FD_LF_KEEP
    #endif
    // UNOFFICIAL 2026-11-12 CapturedCoverage (Includes/CapturedNormals.hlsl): a blended draw (fading in, a rubble skirt's vertex alpha)
    // records only where its own alpha is 0.5 or more; below, alpha 0 = the texels stay as they were. An opaque draw: x 1, as before.
    #if CAPTURED_NORMALS_ON >= 3 && defined(CAP_COVERAGE) && !defined(DIFFUSE) && !defined(ONLY_SPECULAR)
    {
        const float capKeep = CapturedCoverage(CAP_COVERAGE);
        pout.capturedPad1.a *= capKeep;
        pout.capturedPad2.a *= capKeep;
        pout.capturedNormal.a *= capKeep;
    }
    #endif
#endif
    #if defined(OBJECT_WETNESS) && defined(PUDDLES_COMPILED)
        [branch] if (PuddleDebugColor.x >= 0.0f) pout.color.rgb = PuddleDebugColor;	// 2026-11-08 [Shaders.Puddles.Debug] DebugView
    #endif
    pout.color.rgb = LinearSceneOut(pout.color.rgb);	// UNOFFICIAL 2026-10-28 LinearScene (includes/LinearLight.hlsl)

    return pout;
}
