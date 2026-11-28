#if defined(VS) && LIGHTS < 4

struct VS_OUTPUT {
#ifdef OBJ_VCOLOR
    float4 vertexColor : COLOR0;	// rgb vertex colour
#endif
#if defined(OBJ_FOLD_COLOR)
    float4 foldColor : COLOR0;	// UNOFFICIAL FoldHighlights: the vertex colour (the light-only passes had none)
#endif
#ifdef OBJ_FOG
    float4 fogColor : COLOR1;	// rgb fog colour, a fog amount
#endif
    float4 sPosition : POSITION;	// clip space
    float4 uv : TEXCOORD0; // .zw = local barrel position xy
    float4 lightDir : TEXCOORD1;	// light 0, tangent space

#ifdef OBJ_LAMP2
    float4 light2Dir : TEXCOORD2;	// light 1, tangent space; w its radius
#endif
    
#ifdef OBJ_LAMP3
    float4 light3Dir : TEXCOORD3;	// light 2, tangent space; w its radius
#endif
    
    float4 viewDir : TEXCOORD6; // .w = local barrel position z

    // Object-space squared distances for point-light attenuation (vanillaAttSq), bypassing
    // lightDir/light2Dir/light3Dir above -- those are tangent-space (TBN-transformed) and their
    // length is only correct if the TBN basis is orthonormal. .x = light0 (DIFFUSE/POINT only,
    // where light0 is itself a point light rather than the sun), .y = light2, .z = light3.
    // METAL_UP: .w = x of world up in tangent space (with lightDir.w and vertexColor.a, see main).
    float4 lightDistSq : TEXCOORD5;

    // TEXCOORD4 is free at LIGHTS < 4. .w carries SHADOW_VS_SENTINEL.
    float4 shadowWorldPos : TEXCOORD4;	// camera-relative world position; w SHADOW_VS_SENTINEL

#ifdef PROJ_SHADOW
    float4 shadowUVs : TEXCOORD7;	// xy canopy shadow map, zw its mask
#endif
#ifdef OBJ_SMOOTH_NORMAL_INTERP
    float4 smoothNormal : TEXCOORD8;	// UNOFFICIAL 2026-10-24 SmoothObjectNormals: the vertex normal, camera-relative world; .w = SHADOW_VS_SENTINEL
#endif
};

#ifdef PROJ_SHADOW
    row_major float4x4 ShadowProj : register(c18);	// object space to the canopy shadow map
    float4 ShadowProjData : register(c22);	// xy origin, w size of the mask
    float4 ShadowProjTransform : register(c23);	// scale and offset of the projection
#endif


VS_OUTPUT main(VS_INPUT vin) {
    VS_OUTPUT vout;
#ifdef NVR_INSTANCED
    NvrInstancedLoad(vin);	// UNOFFICIAL 2026-11-14 MergeCopies
#endif

    // Zeroed unconditionally: the DIFFUSE/POINT (.x) and LIGHTS>1/LIGHTS>2 (.y/.z) blocks below
    // only overwrite the components they actually use, and vs_3_0 requires every component of
    // vout to be written before return -- a component that stays at this default is simply never
    // read by the PS either (same macro guards on both sides).
    vout.lightDistSq = 0;

    vout.uv.xy = vin.uv.xy;
    
    float4 pos = vin.position.xyzw;
    
    #ifndef SKIN
        float3x3 toTangent = float3x3(vin.tangent.xyz, vin.binormal.xyz, vin.normal.xyz);
        #ifdef CLOTH_WIND
            ClothWindTBN(pos.xyz, toTangent, vin.vertexColor, vin.position.xyz, vin.clothWeight.x, float3(vin.clothWeight.y, 0.0f, 0.0f), 0.0f, 0.0f, 0.0f);	// UNOFFICIAL cloth wind (2026-10-20: still cloth's mask 6 from stream 1)
        #endif
        #ifdef PLANT_WIND
            [branch] if (PlantA.x < -0.5f) pos.xyz += PlantWindMove(pos.xyz);	// UNOFFICIAL 2026-10-20 plant wind (static plants)
        #endif
    
        vout.sPosition.xyzw = mul(ModelViewProj, pos.xyzw);
    #else
        float4 boneOff = vin.blendIndices.zyxw * 765.01001;
        float4 boneW = vin.blendWeight.xyzz;
        boneW.w = 1 - weight(vin.blendWeight.xyz);
        float3x3 toTangent = BonesTransformTBN(Bones, boneOff, boneW, vin.tangent, vin.binormal, vin.normal);
    
        pos.w = 1;
        pos.xyz = BonesTransformPosition(Bones, boneOff, boneW, pos);
        pos.xyz = PipboyFitPosition(pos.xyz, boneOff, boneW);	// UNOFFICIAL 2026-11-03 PipboyFit (includes/PipboyFit.hlsl)
        #ifdef CLOTH_WIND
            float3 clothOut = 0.0f;	// mask 5's billow direction, carried by the same bones
            float4 clothLegL = 0.0f, clothLegR = 0.0f;	// mask 5 (round 8): each leg (ClothLegBind), carried by the same bones
            float3 clothUp = 0.0f;	// ...and the bind pose's up (the legs' direction)
            [branch] if (ClothIsMask5()) {
                [branch] if (ClothWalk.x > 0.5f) {	// 2026-10-20 round 9: hangs from the hips, the hem ring, the current legs
                    const float hw = saturate(vin.clothWeight.x) * ClothWalk.y;
                    pos.xyz = lerp(pos.xyz, ClothPelvisPoint(vin.position.xyz), hw);
                    toTangent = float3x3(normalize(lerp(toTangent[0], normalize(ClothPelvisVector(vin.tangent)), hw)),
                                   normalize(lerp(toTangent[1], normalize(ClothPelvisVector(vin.binormal)), hw)),
                                   normalize(lerp(toTangent[2], normalize(ClothPelvisVector(vin.normal)), hw)));
                    clothOut = ClothPelvisVector(ClothOutwardBind(vin.position.xyz));
                    clothUp = ClothPelvisVector(float3(0.0f, 0.0f, 1.0f));
                    clothLegL = ClothLegBind(pos.xyz, ClothLegNow[0], ClothLegNow[1], ClothLegNow[2]);
                    clothLegR = ClothLegBind(pos.xyz, ClothLegNow[3], ClothLegNow[4], ClothLegNow[5]);
                }
                else {
                    clothOut = BonesTransformVector(Bones, boneOff, boneW, ClothOutwardBind(vin.position.xyz));
                    const float4 legL = ClothLegBind(vin.position.xyz, ClothLimb[0], ClothLimb[1], ClothLimb[2]);
                    const float4 legR = ClothLegBind(vin.position.xyz, ClothLimb[4], ClothLimb[5], ClothLimb[6]);
                    clothLegL = float4(BonesTransformVector(Bones, boneOff, boneW, legL.xyz), legL.w);
                    clothLegR = float4(BonesTransformVector(Bones, boneOff, boneW, legR.xyz), legR.w);
                    clothUp = BonesTransformVector(Bones, boneOff, boneW, float3(0.0f, 0.0f, 1.0f));
                }
            }
            ClothWindTBN(pos.xyz, toTangent, vin.vertexColor, vin.position.xyz, vin.clothWeight.x, clothOut, clothLegL, clothLegR, clothUp);	// UNOFFICIAL cloth wind
            [branch] if (ClothIsMask5() && ClothWalk.x > 0.5f) {	// round 9: the hem ring by the pendulum share, then out of the current legs
                pos.xyz += ClothRingAt(vin.clothWeight.y) * vin.clothWeight.z;
                pos.xyz += ClothLegPush(pos.xyz, clothOut);
            }
        #endif
    
        vout.sPosition.xyzw = mul(SkinModelViewProj, pos.xyzw);
    #endif
    vout.uv.zw = pos.xy;
    
    #ifdef OBJ_LAMP_FIRST
        float3 lPos = LightData[0].xyz - pos.xyz;
        vout.lightDistSq.x = dot(lPos, lPos);
    #else
        float3 lPos = LightData[0].xyz;
    #endif

    vout.lightDir.w = LightData[0].w;
    #ifdef OBJ_LAMP_FIRST
        vout.lightDir.xyz = LAMP_VS_DIR(toTangent, lPos);	// UNOFFICIAL LampDistanceFix (see the top): a lamp here
    #else
        vout.lightDir.xyz = mul(toTangent, lPos);
    #endif

    vout.viewDir.xyz = mul(toTangent, EyePosition.xyz - pos.xyz);
    vout.viewDir.w = pos.z;

    #ifdef OBJ_LAMP2
        lPos = LightData[1].xyz - pos.xyz;
        vout.light2Dir.w = LightData[1].w;
        vout.light2Dir.xyz = LAMP_VS_DIR(toTangent, lPos);	// UNOFFICIAL LampDistanceFix
        vout.lightDistSq.y = dot(lPos, lPos);
    #endif

    #ifdef OBJ_LAMP3
        lPos = LightData[2].xyz - pos.xyz;
        vout.light3Dir.w = LightData[2].w;
        vout.light3Dir.xyz = LAMP_VS_DIR(toTangent, lPos);	// UNOFFICIAL LampDistanceFix
        vout.lightDistSq.z = dot(lPos, lPos);
    #endif
    
    #ifdef OBJ_VCOLOR
        vout.vertexColor = clamp(vin.vertexColor, 0.0f, 1.0f);
    #endif
    #if defined(OBJ_FOLD_COLOR)
        vout.foldColor = clamp(vin.vertexColor, 0.0f, 1.0f);	// UNOFFICIAL FoldHighlights
    #endif
    #if defined(OBJ_AD_EXTRA) && defined(AD_FOG_KEEP_ON)
        {	// UNOFFICIAL 2026-11-16 fd-batchE (see the top; [Main.Main.Quality] DeferredADFog): the fog amount the game's later FOG pass gives this vertex (OBJ_FOG's formula below;
            // 0 where the game's fog range is not a positive number)
            float3 adFogEye = vout.sPosition.xyz;
            #ifdef REVERSED_DEPTH
                adFogEye.z = vout.sPosition.w - adFogEye.z;
            #endif
            const float adFogT = 1 - saturate((FogParam.x - length(adFogEye)) / max(FogParam.y, 1.0e-6f));
            vout.foldColor.a = (FogParam.y > 0.0f && adFogT > 0.0f) ? exp2(log2(adFogT) * FogParam.z) : 0.0f;
        }
    #endif

    #ifdef OBJ_FOG
        float3 fogEye = vout.sPosition.xyz;
    
        #ifdef REVERSED_DEPTH
            fogEye.z = vout.sPosition.w - fogEye.z;
        #endif
    
        float fogT = 1 - saturate((FogParam.x - length(fogEye)) / FogParam.y);
        fogT = log2(fogT);
        vout.fogColor.a = exp2(fogT * FogParam.z);
        vout.fogColor.rgb = FogColor.rgb;
    #endif
    
    #ifdef PROJ_SHADOW
        float projW = dot(ShadowProj[3].xyzw, pos.xyzw);
        float2 projUV;
        projUV.x = dot(ShadowProj[0].xyzw, pos.xyzw);
        projUV.y = dot(ShadowProj[1].xyzw, pos.xyzw);
        vout.shadowUVs.xy = ((projW * ShadowProjTransform.xy) + projUV) / (projW * ShadowProjTransform.w);
        vout.shadowUVs.zw = ((projUV.xy - ShadowProjData.xy) / ShadowProjData.w) * float2(1, -1) + float2(0, 1);
    #endif

    // Model-space shaders: recover a camera-relative world pos from the clip pos.
    // Written unconditionally, or the interpolator is left undefined.
    vout.shadowWorldPos = float4(GetShadowWorldPos(vout.sPosition), SHADOW_VS_SENTINEL);
    #ifdef OBJ_SMOOTH_NORMAL_INTERP
        // UNOFFICIAL 2026-10-24 SmoothObjectNormals: tbn[2] is the vertex normal in the space pos is in (model, or the bones' with SKIN)
        #ifndef SKIN
            vout.smoothNormal = GetShadowSmoothNormal(ModelViewProj, pos.xyz, toTangent[2], vout.shadowWorldPos.xyz);
        #else
            vout.smoothNormal = GetShadowSmoothNormal(SkinModelViewProj, pos.xyz, toTangent[2], vout.shadowWorldPos.xyz);
        #endif
    #endif

    #ifdef METAL_UP
        // UNOFFICIAL: which way is world up in this vertex's tangent space, so the pixel shader can tell how far a
        // reflected view points at the sky (Object.hlsl getMetalAmbient). The world z of an object-space direction d is
        // d . objUp, objUp = upView x ModelViewProj, with upView the camera's inverse projection times the up column of
        // its inverse view (GetShadowWorldPos's matrices; exact for the world, a little off for the first-person model
        // when its field of view differs). The spare channels carry it: lightDir.w (read only by the DIFFUSE and POINT
        // passes), lightDistSq.w and vertexColor.a (unused by the pixel shader; COLOR0 is clamped, so encoded).
        float4 upView = mul(TESR_InvProjectionTransform, float4(TESR_InvViewTransform[0][2], TESR_InvViewTransform[1][2], TESR_InvViewTransform[2][2], 0.0f));
        #ifndef SKIN
            float3 objUp = mul(upView, ModelViewProj).xyz;
        #else
            float3 objUp = mul(upView, SkinModelViewProj).xyz;
        #endif
        float3 upTangent = normalize(mul(toTangent, objUp));
        vout.lightDistSq.w = upTangent.x;
        vout.lightDir.w = upTangent.y;
        vout.vertexColor.a = upTangent.z * 0.5f + 0.5f;
    #endif
    #ifdef OBJ_SMOOTH_NORMAL_W
        {	// UNOFFICIAL 2026-10-24 SmoothObjectNormals: the vertex normal, camera-relative world, in METAL_UP's three channels
            // (2026-10-26: a plain vector, no longer octahedral: ShadowPlainNormal)
            #ifndef SKIN
                const float3 smoothN = GetShadowSmoothNormal(ModelViewProj, pos.xyz, toTangent[2], vout.shadowWorldPos.xyz).xyz;
            #else
                const float3 smoothN = GetShadowSmoothNormal(SkinModelViewProj, pos.xyz, toTangent[2], vout.shadowWorldPos.xyz).xyz;
            #endif
            vout.lightDistSq.w = smoothN.x;
            vout.lightDir.w = smoothN.y;
            vout.vertexColor.a = smoothN.z * 0.5f + 0.5f;
        }
    #endif

    return vout;
};

#elif defined(VS) && LIGHTS >= 4

#ifdef OBJ_TOGGLES
    float4 fvars0 : register(c17);	// the game's, unused here

    #define lightOffset 0	// first light slot that is a lamp
#else
    #define lightOffset 1	// first light slot that is a lamp (slot 0 is the sun)
#endif

VS_OUTPUT main(VS_INPUT vin) {
    VS_OUTPUT vout;
    
    vout.uv.xy = vin.uv.xy;
    
    float4 pos = vin.position.xyzw;
    
    #ifndef SKIN
    float3x3 toTangent = float3x3(vin.tangent.xyz, vin.binormal.xyz, vin.normal.xyz);
    #ifdef CLOTH_WIND
        ClothWindTBN(pos.xyz, toTangent, vin.vertexColor, vin.position.xyz, vin.clothWeight.x, float3(vin.clothWeight.y, 0.0f, 0.0f), 0.0f, 0.0f, 0.0f);	// UNOFFICIAL cloth wind (2026-10-20: still cloth's mask 6 from stream 1)
    #endif
    #ifdef PLANT_WIND
        [branch] if (PlantA.x < -0.5f) pos.xyz += PlantWindMove(pos.xyz);	// UNOFFICIAL 2026-10-20 plant wind (static plants)
    #endif
    
    vout.sPosition.xyzw = mul(ModelViewProj, pos.xyzw);
    #else
        float4 boneOff = vin.blendIndices.zyxw * 765.01001;
        float4 boneW = vin.blendWeight.xyzz;
        boneW.w = 1 - weight(vin.blendWeight.xyz);
        float3x3 toTangent = BonesTransformTBN(Bones, boneOff, boneW, vin.tangent, vin.binormal, vin.normal);
    
        pos.w = 1;
        pos.xyz = BonesTransformPosition(Bones, boneOff, boneW, pos);
        pos.xyz = PipboyFitPosition(pos.xyz, boneOff, boneW);	// UNOFFICIAL 2026-11-03 PipboyFit (includes/PipboyFit.hlsl)
        #ifdef CLOTH_WIND
            float3 clothOut = 0.0f;	// mask 5's billow direction, carried by the same bones
            float4 clothLegL = 0.0f, clothLegR = 0.0f;	// mask 5 (round 8): each leg (ClothLegBind), carried by the same bones
            float3 clothUp = 0.0f;	// ...and the bind pose's up (the legs' direction)
            [branch] if (ClothIsMask5()) {
                [branch] if (ClothWalk.x > 0.5f) {	// 2026-10-20 round 9: hangs from the hips, the hem ring, the current legs
                    const float hw = saturate(vin.clothWeight.x) * ClothWalk.y;
                    pos.xyz = lerp(pos.xyz, ClothPelvisPoint(vin.position.xyz), hw);
                    toTangent = float3x3(normalize(lerp(toTangent[0], normalize(ClothPelvisVector(vin.tangent)), hw)),
                                   normalize(lerp(toTangent[1], normalize(ClothPelvisVector(vin.binormal)), hw)),
                                   normalize(lerp(toTangent[2], normalize(ClothPelvisVector(vin.normal)), hw)));
                    clothOut = ClothPelvisVector(ClothOutwardBind(vin.position.xyz));
                    clothUp = ClothPelvisVector(float3(0.0f, 0.0f, 1.0f));
                    clothLegL = ClothLegBind(pos.xyz, ClothLegNow[0], ClothLegNow[1], ClothLegNow[2]);
                    clothLegR = ClothLegBind(pos.xyz, ClothLegNow[3], ClothLegNow[4], ClothLegNow[5]);
                }
                else {
                    clothOut = BonesTransformVector(Bones, boneOff, boneW, ClothOutwardBind(vin.position.xyz));
                    const float4 legL = ClothLegBind(vin.position.xyz, ClothLimb[0], ClothLimb[1], ClothLimb[2]);
                    const float4 legR = ClothLegBind(vin.position.xyz, ClothLimb[4], ClothLimb[5], ClothLimb[6]);
                    clothLegL = float4(BonesTransformVector(Bones, boneOff, boneW, legL.xyz), legL.w);
                    clothLegR = float4(BonesTransformVector(Bones, boneOff, boneW, legR.xyz), legR.w);
                    clothUp = BonesTransformVector(Bones, boneOff, boneW, float3(0.0f, 0.0f, 1.0f));
                }
            }
            ClothWindTBN(pos.xyz, toTangent, vin.vertexColor, vin.position.xyz, vin.clothWeight.x, clothOut, clothLegL, clothLegR, clothUp);	// UNOFFICIAL cloth wind
            [branch] if (ClothIsMask5() && ClothWalk.x > 0.5f) {	// round 9: the hem ring by the pendulum share, then out of the current legs
                pos.xyz += ClothRingAt(vin.clothWeight.y) * vin.clothWeight.z;
                pos.xyz += ClothLegPush(pos.xyz, clothOut);
            }
        #endif
        vout.sPosition.xyzw = mul(SkinModelViewProj, pos.xyzw);
    #endif
    
    float3 viewDir = mul(toTangent, EyePosition.xyz - pos.xyz);
    
    vout.lPosition.xyz = pos.xyz;
    vout.lPosition.w = LightData[0].w;
    
    #ifdef OBJ_TOGGLES
        float lightCount = min(MAX_LIGHTS, fvars0.z);
    #elif defined(SPECULAR)
        float lightCount = min(MAX_LIGHTS, EyePosition.w);
    #else
        float lightCount = min(MAX_LIGHTS, LightData[0].w);
    #endif
    float lightFrac = frac(lightCount);
    float lightThr = (lightCount < 0.0 ? (-lightFrac < lightFrac ? 1.0 : 0.0) : 0) + (lightCount - lightFrac);
    float lightOn;
    
    #ifdef OBJ_TOGGLES
        vout.lightDir.w = viewDir.x;
        vout.lightDir.xyz = mul(toTangent, LightData[0].xyz);
    #else
        lightOn = 0 < lightThr ? 1.0 : 0.0;
        vout.lightDir.xyz = lightOn * mul(toTangent, LightData[lightOffset + 0].xyz - pos.xyz);
        vout.lightDir.w = viewDir.x;
    #endif
    
    #ifdef MANY_LAMPS
        // UNOFFICIAL ManyLampsOnePass: the tangent frame's rows (written at the end); the view as before.
        vout.light2 = float4(0.0f, 0.0f, 0.0f, viewDir.y);
        vout.light3 = float4(0.0f, 0.0f, 0.0f, viewDir.z);
        vout.light4 = 0.0f;
    #else
    lightOn = 1 < lightThr ? 1.0 : 0.0;
    vout.light2.xyz = lightOn * mul(toTangent, LightData[lightOffset + 1].xyz - pos.xyz);
    vout.light2.w = viewDir.y;

    lightOn = 2 < lightThr ? 1.0 : 0.0;
    vout.light3.xyz = lightOn * mul(toTangent, LightData[lightOffset + 2].xyz - pos.xyz);
    vout.light3.w = viewDir.z;

    #if MAX_LIGHTS >= 4
        lightOn = 3 < lightThr ? 1.0 : 0.0;
        vout.light4.xyz = lightOn * mul(toTangent, LightData[lightOffset + 3].xyz - pos.xyz);
        vout.light4.w = lightOn * LightData[lightOffset + 3].w;
    #endif
    #endif
    
    #if MAX_LIGHTS >= 5
        lightOn = 4 < lightThr ? 1.0 : 0.0;
        vout.light5.xyz = lightOn * mul(toTangent, LightData[lightOffset + 4].xyz - pos.xyz);
        vout.light5.w = lightOn * LightData[lightOffset + 4].w;
        
        lightOn = 5 < lightThr ? 1.0 : 0.0;
        vout.light6.xyz = lightOn * mul(toTangent, LightData[lightOffset + 5].xyz - pos.xyz);
        vout.light6.w = lightOn * LightData[lightOffset + 5].w;
    #endif
    
    vout.vertexColor = clamp(vin.vertexColor, 0.0f, 1.0f);

    float3 fogEye = vout.sPosition.xyz;
    #ifdef REVERSED_DEPTH
        fogEye.z = vout.sPosition.w - fogEye.z;
    #endif
    float fogT = 1 - saturate((FogParam.x - length(fogEye)) / FogParam.y);
    fogT = log2(fogT);
    vout.fogColor.a = exp2(fogT * FogParam.z);
    vout.fogColor.rgb = FogColor.rgb;

    // Through SHADOW_WP_STORE: at MAX_LIGHTS 6 this overwrites light4/5/6's .w, which the PS
    // ignores.
    float3 shadowWorldPos = GetShadowWorldPos(vout.sPosition);
    SHADOW_WP_STORE(vout, shadowWorldPos);
    #if OBJECT_SMOOTH_NORMALS
        // UNOFFICIAL 2026-10-24 SmoothObjectNormals: the vertex normal (tbn[2], in pos's space) in camera-relative world, in uv.zw and
        // vertexColor.a (2026-10-26: a plain vector, no longer octahedral: ShadowPlainNormal)
        #ifndef SKIN
            const float3 smoothN = GetShadowSmoothNormal(ModelViewProj, pos.xyz, toTangent[2], shadowWorldPos).xyz;
        #else
            const float3 smoothN = GetShadowSmoothNormal(SkinModelViewProj, pos.xyz, toTangent[2], shadowWorldPos).xyz;
        #endif
        vout.uv.zw = smoothN.xy;
        vout.vertexColor.a = smoothN.z * 0.5f + 0.5f;
    #endif
    #ifdef MANY_LAMPS
        // UNOFFICIAL ManyLampsOnePass: the frame's rows; the pixel shader takes mul(tbn, lamp - pos) per pixel, as this shader did per vertex.
        vout.light2.xyz = toTangent[0];
        vout.light3.xyz = toTangent[1];
        vout.light4.xyz = toTangent[2];
    #endif

    return vout;
};
#endif // Vertex shaders.
