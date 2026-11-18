// UNOFFICIAL: the sun's and the lamps' light in ObjectTemplate.hlsl's LIGHTS >= 4 pixel shader, pasted into its main()
// twice: once with the metal finish and once without (LIGHT_FINISH, defined by the includer: "roughness" or
// "lightRoughness, metallic"). The copy without metal is the old code exactly, and pixels without metal take it, so metal
// costs nothing where there is none. Uses main()'s locals: IN, viewDir, normal, baseColor, att, lighting, sunShadow
// (without OPT) and the forward point shadows (SHADOWED).
// UNOFFICIAL 2026-10-29 ShadowContrast (SHADOW_CONTRAST_ON, ObjectTemplate.hlsl): each shadowed lamp's term is worked out unshadowed and
// split (SC_SPLIT) into the light kept (added to lighting) and the light its shadow takes (added to scLost). Off: the lines as before.

#ifdef NVR_LIGHT_DEFERRED
// UNOFFICIAL 2026-11-14 the lighting-free record (ObjectTemplate.hlsl NVR_LIGHT_DEFERRED, LAMPS_DEFERRED 2): no sun, no lamp; the deferred
// passes add them. The ambient the includer adds after this stays.
    lighting = 0.0f;
#elif defined(MANY_LAMPS)
// UNOFFICIAL 2026-10-27 ManyLampsOnePass (core/ManyLamps.h, ObjectTemplate.hlsl MANY_LAMPS; after Conor (mrfury97)'s fork, see ManyLamps.h): every lamp of the object from c37-c63,
// its direction taken into tangent space per pixel through the frame's rows (light2/3/4 .xyz) and its falloff from the distance, all in
// the object's own space as the game makes its lamp constants (2026-10-27 first version: camera-relative world space; reached nothing).
// The sun (without OPT) as before.
// Lamp k of c37-c63 is light k + 1 of the game's pass, so the first six keep their forward cube-map shadow slot (SHADOWED: slot k
// with OPT, where light 0 is not in the shader, k + 1 without); lamps the game did not keep in the pass (k past its count) and
// lamps 7-13 have none (their slots' lookups stay 1: see the INTERIOR_SHADOWS block in ObjectTemplate.hlsl).
#ifndef OPT
    lighting = getSunLighting(IN.lightDir.xyz, PSLightColor[0].rgb * sunShadow, viewDir.xyz, normal.xyz, baseColor.rgb, LIGHT_FINISH);
#else
    lighting = 0.0f;
#endif
#if !LAMPS_DEFERRED	// UNOFFICIAL 2026-10-31 DeferredLamps (docs/next/DEFERRED-C1-SPEC.md): the deferred record lights no lamp; the DeferredLamps pass adds them
#if MANY_LAMPS_ON >= 2
// UNOFFICIAL 2026-10-29 LightCommonLamps (Includes/LightCommon.hlsl, core/ManyLamps.h): the same lamps from the frame's lamp table. c37 x the
// count, y the reach scale; c38 the lighting offset; c39-c41 world -> object rows; c42-c44 the table indices (LightCommonIndex). The first six
// keep their compile-time cube-map shadow slot (SHADOWED needs it), so they are written out; every lamp after them is ONE real loop.
{
    const float3 mlWorldPos = IN.lPosition.xyz;	// the object's space, as the lamps once moved there
    const float mlCount = NVR_ManyLamps[0].x;
    #define ML_TS(d) float3(dot(IN.light2.xyz, (d)), dot(IN.light3.xyz, (d)), dot(IN.light4.xyz, (d)))
    #define ML_LAMP_AT(COND, POSITION, COLOR) [branch] if (COND) { \
            const float4 mlP = POSITION; \
            const float3 mlD = mlP.xyz - mlWorldPos; \
            const float3 mlL = ML_TS(mlD); \
            att = vanillaAtt(mlD, max(mlP.w, 1.0e-3f)); \
            [branch] if (att > 0.0f && mlP.w > 0.0f && dot(mlL, mlL) > 1.0e-8f) \
                lighting += getPointLightLightingAtt(mlL, att, COLOR, viewDir.xyz, normal.xyz, baseColor.rgb, LIGHT_FINISH); \
        }
    #if SHADOW_CONTRAST_ON	// UNOFFICIAL 2026-10-29 ShadowContrast: a shadowed lamp's term unshadowed, split into kept and lost light
    #define ML_LAMP_AT_SC(COND, POSITION, COLOR, SH) [branch] if (COND) { \
            const float4 mlP = POSITION; \
            const float3 mlD = mlP.xyz - mlWorldPos; \
            const float3 mlL = ML_TS(mlD); \
            att = vanillaAtt(mlD, max(mlP.w, 1.0e-3f)); \
            [branch] if (att > 0.0f && mlP.w > 0.0f && dot(mlL, mlL) > 1.0e-8f) \
                SC_SPLIT(getPointLightLightingAtt(mlL, att, COLOR, viewDir.xyz, normal.xyz, baseColor.rgb, LIGHT_FINISH), SH) \
        }
    #endif
    #define ML_TABLE_LAMP(j, COLOR) [branch] if ((j) < mlCount) { \
            const float mlI = LightCommonIndex(NVR_ManyLamps[5], NVR_ManyLamps[6], NVR_ManyLamps[7], (float)(j)); \
            float4 mlTP; float3 mlTC; \
            [branch] if (mlI < LIGHT_COMMON_NONE) { \
                LightCommonLamp(mlI, NVR_ManyLamps[1], NVR_ManyLamps[2], NVR_ManyLamps[3], NVR_ManyLamps[4], NVR_ManyLamps[0].y, mlTP, mlTC); \
                ML_LAMP_AT(true, mlTP, COLOR) \
            } \
        }
    #ifdef TABLE_LAMP_SHADOWS_ON	// UNOFFICIAL 2026-10-31 TableLamps: the shadow first (one value live across the lighting), then the lamp
    #define ML_TABLE_LAMP_SH(j) [branch] if ((j) < mlCount) { \
            const float mlI = LightCommonIndex(NVR_ManyLamps[5], NVR_ManyLamps[6], NVR_ManyLamps[7], (float)(j)); \
            [branch] if (mlI < LIGHT_COMMON_NONE) { \
                const float mlSh = TableLampShadow(mlI, ptWorldPos, ptNormal, ptValid); \
                float4 mlTP; float3 mlTC; \
                LightCommonLamp(mlI, NVR_ManyLamps[1], NVR_ManyLamps[2], NVR_ManyLamps[3], NVR_ManyLamps[4], NVR_ManyLamps[0].y, mlTP, mlTC); \
                ML_LAMP_AT(true, mlTP, mlTC * min(mlSh, 1.0f)) \
                CAPS_NOTE_UNSHADOWED(mlSh > 1.5f ? att : 0.0f) \
            } \
        }
    #endif
    #if SHADOW_CONTRAST_ON
    #define ML_TABLE_LAMP_SC(j, SH) [branch] if ((j) < mlCount) { \
            const float mlI = LightCommonIndex(NVR_ManyLamps[5], NVR_ManyLamps[6], NVR_ManyLamps[7], (float)(j)); \
            float4 mlTP; float3 mlTC; \
            [branch] if (mlI < LIGHT_COMMON_NONE) { \
                LightCommonLamp(mlI, NVR_ManyLamps[1], NVR_ManyLamps[2], NVR_ManyLamps[3], NVR_ManyLamps[4], NVR_ManyLamps[0].y, mlTP, mlTC); \
                ML_LAMP_AT_SC(true, mlTP, mlTC, SH) \
            } \
        }
    #endif
    #if SHADOW_CONTRAST_ON
    #ifdef OPT
        ML_TABLE_LAMP_SC(0, POINT_SHADOW(0, att))
        ML_TABLE_LAMP_SC(1, POINT_SHADOW(1, att))
        ML_TABLE_LAMP_SC(2, POINT_SHADOW(2, att))
        ML_TABLE_LAMP_SC(3, POINT_SHADOW(3, att))
        ML_TABLE_LAMP_SC(4, POINT_SHADOW(4, att))
        ML_TABLE_LAMP_SC(5, POINT_SHADOW(5, att))
    #else
        ML_TABLE_LAMP_SC(0, POINT_SHADOW(1, att))
        ML_TABLE_LAMP_SC(1, POINT_SHADOW(2, att))
        ML_TABLE_LAMP_SC(2, POINT_SHADOW(3, att))
        ML_TABLE_LAMP_SC(3, POINT_SHADOW(4, att))
        ML_TABLE_LAMP_SC(4, POINT_SHADOW(5, att))
        ML_TABLE_LAMP(5, mlTC)
    #endif
    #else
    #ifdef OPT
        ML_TABLE_LAMP(0, SHADOWED(mlTC, 0, att))
        ML_TABLE_LAMP(1, SHADOWED(mlTC, 1, att))
        ML_TABLE_LAMP(2, SHADOWED(mlTC, 2, att))
        ML_TABLE_LAMP(3, SHADOWED(mlTC, 3, att))
        ML_TABLE_LAMP(4, SHADOWED(mlTC, 4, att))
        ML_TABLE_LAMP(5, SHADOWED(mlTC, 5, att))
    #else
        ML_TABLE_LAMP(0, SHADOWED(mlTC, 1, att))
        ML_TABLE_LAMP(1, SHADOWED(mlTC, 2, att))
        ML_TABLE_LAMP(2, SHADOWED(mlTC, 3, att))
        ML_TABLE_LAMP(3, SHADOWED(mlTC, 4, att))
        ML_TABLE_LAMP(4, SHADOWED(mlTC, 5, att))
        ML_TABLE_LAMP(5, mlTC)
    #endif
    #endif
    #if MANY_LAMPS_ON == 3
    // UNOFFICIAL 2026-10-30 ClusteredLamps (core/ManyLamps.h BinFroxels): past the draw's six shadowed lamps, every lamp of this pixel's
    // froxel (screen tile x depth slice) instead of the draw's list. c45 the view depth from the object's space, c46 x 1 / tile size,
    // y tiles across, z tiles a slice, w 1 = on (0: first person, lists not made, setting off live), c47 x the last slice.
    [branch] if (NVR_ManyLamps[9].w > 0.5f) {
        const float mlZ = dot(float4(mlWorldPos, 1.0f), NVR_ManyLamps[8]);
        const float2 mlTile = floor(mlVPos * NVR_ManyLamps[9].x);
        const float mlSlice = mlZ < 64.0f ? 0.0f : min(NVR_ManyLamps[10].x, floor(log2(mlZ)) - 5.0f);
        const float mlBase = (mlSlice * NVR_ManyLamps[9].z + mlTile.y * NVR_ManyLamps[9].y + mlTile.x) * LIGHT_COMMON_LIST_TEXELS;
        const float mlN = LightCommonListTexel(mlBase).x;
        // the draw's six (already lit, with their shadows) are left out of the froxel's list
        const float mlS0 = LightCommonIndex(NVR_ManyLamps[5], NVR_ManyLamps[6], NVR_ManyLamps[7], 0.0f);
        const float mlS1 = LightCommonIndex(NVR_ManyLamps[5], NVR_ManyLamps[6], NVR_ManyLamps[7], 1.0f);
        const float mlS2 = LightCommonIndex(NVR_ManyLamps[5], NVR_ManyLamps[6], NVR_ManyLamps[7], 2.0f);
        const float mlS3 = LightCommonIndex(NVR_ManyLamps[5], NVR_ManyLamps[6], NVR_ManyLamps[7], 3.0f);
        const float mlS4 = LightCommonIndex(NVR_ManyLamps[5], NVR_ManyLamps[6], NVR_ManyLamps[7], 4.0f);
        const float mlS5 = LightCommonIndex(NVR_ManyLamps[5], NVR_ManyLamps[6], NVR_ManyLamps[7], 5.0f);
        [loop] for (int mlK = 0; mlK < 31; mlK++) {
            [branch] if ((float)mlK >= mlN) break;
            const float mlE = (float)mlK + 1.0f;
            const float mlT = floor((mlE + 0.5f) / 4.0f);
            const float mlC = mlE - mlT * 4.0f;
            const float4 mlV = LightCommonListTexel(mlBase + mlT);
            const float mlI = dot(mlV, float4(mlC < 0.5f, abs(mlC - 1.0f) < 0.5f, abs(mlC - 2.0f) < 0.5f, mlC > 2.5f));
            [branch] if (mlI != mlS0 && mlI != mlS1 && mlI != mlS2 && mlI != mlS3 && mlI != mlS4 && mlI != mlS5) {
                float4 mlTP; float3 mlTC;
                // (2026-10-31 TableLamps: the froxel lamps stay unshadowed: their lookup here runs SLS2034 / 2036 out of temp registers, X4505)
                LightCommonLamp(mlI, NVR_ManyLamps[1], NVR_ManyLamps[2], NVR_ManyLamps[3], NVR_ManyLamps[4], NVR_ManyLamps[0].y, mlTP, mlTC);
                ML_LAMP_AT(true, mlTP, mlTC)
                CAPS_NOTE_UNSHADOWED(att)	// UNOFFICIAL 2026-10-31 LampCapsCensus view (PointShadowForward.hlsl): a froxel lamp has no shadow source
            }
        }
    }
    else
    #endif
    [loop] for (int mlIndex = 6; mlIndex < 24; mlIndex++) {
        [branch] if (mlIndex >= mlCount) break;
        #if LAMP_CAPS_VIEW
        att = 0.0f;	// UNOFFICIAL 2026-10-31 LampCapsCensus view: an index the table has not got leaves att as it was
        #endif
        #ifdef TABLE_LAMP_SHADOWS_ON	// UNOFFICIAL 2026-10-31 TableLamps (LightCommon.hlsl): lamps 7-24 shadowed where they hold an atlas tile
        ML_TABLE_LAMP_SH(mlIndex)
        #else
        ML_TABLE_LAMP(mlIndex, mlTC)
        CAPS_NOTE_UNSHADOWED(att)	// (lamps 7-24 have no shadow source)
        #endif
    }
    #undef ML_TABLE_LAMP
    #ifdef ML_TABLE_LAMP_SH
    #undef ML_TABLE_LAMP_SH
    #endif
    #ifdef ML_TABLE_LAMP_SC
    #undef ML_TABLE_LAMP_SC
    #endif
    #undef ML_LAMP_AT
    #ifdef ML_LAMP_AT_SC
    #undef ML_LAMP_AT_SC
    #endif
    #undef ML_TS
}
#else
{
    const float3 mlWorldPos = IN.lPosition.xyz;	// the object's space, as the lamps in c38 + 2k
    const float mlCount = NVR_ManyLamps[0].x;
    #define ML_TS(d) float3(dot(IN.light2.xyz, (d)), dot(IN.light3.xyz, (d)), dot(IN.light4.xyz, (d)))
    // (A lamp without reach, or a pixel whose frame is all zero, adds nothing: no division by 0, no normalising a zero vector.)
    #define ML_LAMP_AT(COND, POSITION, COLOR) [branch] if (COND) { \
            const float4 mlP = POSITION; \
            const float3 mlD = mlP.xyz - mlWorldPos; \
            const float3 mlL = ML_TS(mlD); \
            att = vanillaAtt(mlD, max(mlP.w, 1.0e-3f)); \
            [branch] if (att > 0.0f && mlP.w > 0.0f && dot(mlL, mlL) > 1.0e-8f) \
                lighting += getPointLightLightingAtt(mlL, att, COLOR, viewDir.xyz, normal.xyz, baseColor.rgb, LIGHT_FINISH); \
        }
    #if SHADOW_CONTRAST_ON	// UNOFFICIAL 2026-10-29 ShadowContrast: a shadowed lamp's term unshadowed, split into kept and lost light
    #define ML_LAMP_AT_SC(COND, POSITION, COLOR, SH) [branch] if (COND) { \
            const float4 mlP = POSITION; \
            const float3 mlD = mlP.xyz - mlWorldPos; \
            const float3 mlL = ML_TS(mlD); \
            att = vanillaAtt(mlD, max(mlP.w, 1.0e-3f)); \
            [branch] if (att > 0.0f && mlP.w > 0.0f && dot(mlL, mlL) > 1.0e-8f) \
                SC_SPLIT(getPointLightLightingAtt(mlL, att, COLOR, viewDir.xyz, normal.xyz, baseColor.rgb, LIGHT_FINISH), SH) \
        }
    #endif
    #define ML_LAMP(j, COLOR) ML_LAMP_AT((j) < mlCount, NVR_ManyLamps[1 + 2 * (j)], COLOR)
    #if SHADOW_CONTRAST_ON
    #define ML_LAMP_SC(j, COLOR, SH) ML_LAMP_AT_SC((j) < mlCount, NVR_ManyLamps[1 + 2 * (j)], COLOR, SH)
    #endif
    #if SHADOW_CONTRAST_ON
    #ifdef OPT
        ML_LAMP_SC(0, NVR_ManyLamps[2].rgb, POINT_SHADOW(0, att))
        ML_LAMP_SC(1, NVR_ManyLamps[4].rgb, POINT_SHADOW(1, att))
        ML_LAMP_SC(2, NVR_ManyLamps[6].rgb, POINT_SHADOW(2, att))
        ML_LAMP_SC(3, NVR_ManyLamps[8].rgb, POINT_SHADOW(3, att))
        ML_LAMP_SC(4, NVR_ManyLamps[10].rgb, POINT_SHADOW(4, att))
        ML_LAMP_SC(5, NVR_ManyLamps[12].rgb, POINT_SHADOW(5, att))
    #else
        ML_LAMP_SC(0, NVR_ManyLamps[2].rgb, POINT_SHADOW(1, att))
        ML_LAMP_SC(1, NVR_ManyLamps[4].rgb, POINT_SHADOW(2, att))
        ML_LAMP_SC(2, NVR_ManyLamps[6].rgb, POINT_SHADOW(3, att))
        ML_LAMP_SC(3, NVR_ManyLamps[8].rgb, POINT_SHADOW(4, att))
        ML_LAMP_SC(4, NVR_ManyLamps[10].rgb, POINT_SHADOW(5, att))
        ML_LAMP(5, NVR_ManyLamps[12].rgb)
    #endif
    #else
    #ifdef OPT
        ML_LAMP(0, SHADOWED(NVR_ManyLamps[2].rgb, 0, att))
        ML_LAMP(1, SHADOWED(NVR_ManyLamps[4].rgb, 1, att))
        ML_LAMP(2, SHADOWED(NVR_ManyLamps[6].rgb, 2, att))
        ML_LAMP(3, SHADOWED(NVR_ManyLamps[8].rgb, 3, att))
        ML_LAMP(4, SHADOWED(NVR_ManyLamps[10].rgb, 4, att))
        ML_LAMP(5, SHADOWED(NVR_ManyLamps[12].rgb, 5, att))
    #else
        ML_LAMP(0, SHADOWED(NVR_ManyLamps[2].rgb, 1, att))
        ML_LAMP(1, SHADOWED(NVR_ManyLamps[4].rgb, 2, att))
        ML_LAMP(2, SHADOWED(NVR_ManyLamps[6].rgb, 3, att))
        ML_LAMP(3, SHADOWED(NVR_ManyLamps[8].rgb, 4, att))
        ML_LAMP(4, SHADOWED(NVR_ManyLamps[10].rgb, 5, att))
        ML_LAMP(5, NVR_ManyLamps[12].rgb)
    #endif
    #endif
    [loop] for (int mlIndex = 6; mlIndex < MANY_LAMPS_MAX; mlIndex++) {
        [branch] if (mlIndex >= mlCount) break;
        ML_LAMP(mlIndex, NVR_ManyLamps[2 + 2 * mlIndex].rgb)
        CAPS_NOTE_UNSHADOWED(att)	// UNOFFICIAL 2026-10-31 LampCapsCensus view (PointShadowForward.hlsl): lamps 7-24 have no shadow source
    }
    // 2026-10-27: lamps 14-24 from the two other ranges (ObjectTemplate.hlsl NVR_ManyLampsB / C).
    [branch] if (mlCount > MANY_LAMPS_MAX) {
        [loop] for (int mlB = 0; mlB < MANY_LAMPS_B; mlB++) {
            [branch] if (MANY_LAMPS_MAX + mlB >= mlCount) break;
            ML_LAMP_AT(true, NVR_ManyLampsB[2 * mlB], NVR_ManyLampsB[2 * mlB + 1].rgb)
            CAPS_NOTE_UNSHADOWED(att)
        }
        [loop] for (int mlC = 0; mlC < MANY_LAMPS_C; mlC++) {
            [branch] if (MANY_LAMPS_MAX + MANY_LAMPS_B + mlC >= mlCount) break;
            ML_LAMP_AT(true, NVR_ManyLampsC[2 * mlC], NVR_ManyLampsC[2 * mlC + 1].rgb)
            CAPS_NOTE_UNSHADOWED(att)
        }
    }
    #undef ML_LAMP
    #ifdef ML_LAMP_SC
    #undef ML_LAMP_SC
    #endif
    #undef ML_LAMP_AT
    #ifdef ML_LAMP_AT_SC
    #undef ML_LAMP_AT_SC
    #endif
    #undef ML_TS
}
#endif // MANY_LAMPS_ON >= 2
#endif // !LAMPS_DEFERRED
#else
#ifndef OPT
    lighting = getSunLighting(IN.lightDir.xyz, PSLightColor[0].rgb * sunShadow, viewDir.xyz, normal.xyz, baseColor.rgb, LIGHT_FINISH);
#else
    att = vanillaAtt(PSLightPosition[0].xyz - IN.lPosition.xyz, PSLightPosition[0].w);
    lighting = 0.0f;
    LAMP_IFK(0, att)   // UNOFFICIAL LampReachSkip
#if SHADOW_CONTRAST_ON
        SC_SPLIT((0 >= lightsUsed ? 0.0 : 1.0) * getPointLightLightingAtt(IN.lightDir.xyz, att, PSLightColor[0].rgb, viewDir.xyz, normal.xyz, baseColor.rgb, LIGHT_FINISH), POINT_SHADOW(0, att))
#else
        lighting = (0 >= lightsUsed ? 0.0 : 1.0) * getPointLightLightingAtt(IN.lightDir.xyz, att, SHADOWED(PSLightColor[0].rgb, 0, att), viewDir.xyz, normal.xyz, baseColor.rgb, LIGHT_FINISH);
#endif
#endif

// UNOFFICIAL: SHADOWED(colour, k, att) -- light k of the draw is PSLightColor[k] (k = 0 is the sun's slot without OPT).
// UNOFFICIAL (2026-10-08): light k counts only when lightsUsed > k, as in the decoded vanilla shader (upstream 0f03812a).
// Lights 2-5 were taken at lightsUsed == k (and light 0 under OPT always): the game sets only the used lights'
// constants, so that light was whatever the previous draw left in c3-c26 -- a phantom lamp per object that came and
// went with the draw order (floor pieces lighting up and going dark with hard edges as the camera moved, PBR only).
att = vanillaAtt(PSLightPosition[lightOffset + 0].xyz - IN.lPosition.xyz, PSLightPosition[lightOffset + 0].w);
LAMP_IFK(1, att)   // UNOFFICIAL 2026-10-17 LampReachSkip (ObjectTemplate.hlsl): a lamp out of reach adds exactly 0
#if SHADOW_CONTRAST_ON
    SC_SPLIT((1 >= lightsUsed ? 0.0 : 1.0) * getPointLightLightingAtt(IN.light2.xyz, att, PSLightColor[1].rgb, viewDir.xyz, normal.xyz, baseColor.rgb, LIGHT_FINISH), POINT_SHADOW(1, att))
#else
    lighting += (1 >= lightsUsed ? 0.0 : 1.0) * getPointLightLightingAtt(IN.light2.xyz, att, SHADOWED(PSLightColor[1].rgb, 1, att), viewDir.xyz, normal.xyz, baseColor.rgb, LIGHT_FINISH);
#endif

att = vanillaAtt(PSLightPosition[lightOffset + 1].xyz - IN.lPosition.xyz, PSLightPosition[lightOffset + 1].w);
LAMP_IFK(2, att)   // UNOFFICIAL 2026-10-17 LampReachSkip (ObjectTemplate.hlsl): a lamp out of reach adds exactly 0
#if SHADOW_CONTRAST_ON
    SC_SPLIT((2 >= lightsUsed ? 0.0 : 1.0) * getPointLightLightingAtt(IN.light3.xyz, att, PSLightColor[2].rgb, viewDir.xyz, normal.xyz, baseColor.rgb, LIGHT_FINISH), POINT_SHADOW(2, att))
#else
    lighting += (2 >= lightsUsed ? 0.0 : 1.0) * getPointLightLightingAtt(IN.light3.xyz, att, SHADOWED(PSLightColor[2].rgb, 2, att), viewDir.xyz, normal.xyz, baseColor.rgb, LIGHT_FINISH);
#endif

#if MAX_LIGHTS > 3
    att = vanillaAtt(PSLightPosition[lightOffset + 2].xyz - IN.lPosition.xyz, PSLightPosition[lightOffset + 2].w);
    LAMP_IFK(3, att)   // UNOFFICIAL 2026-10-17 LampReachSkip (ObjectTemplate.hlsl): a lamp out of reach adds exactly 0
#if SHADOW_CONTRAST_ON
        SC_SPLIT((3 >= lightsUsed ? 0.0 : 1.0) * getPointLightLightingAtt(IN.light4.xyz, att, PSLightColor[3].rgb, viewDir.xyz, normal.xyz, baseColor.rgb, LIGHT_FINISH), POINT_SHADOW(3, att))
#else
        lighting += (3 >= lightsUsed ? 0.0 : 1.0) * getPointLightLightingAtt(IN.light4.xyz, att, SHADOWED(PSLightColor[3].rgb, 3, att), viewDir.xyz, normal.xyz, baseColor.rgb, LIGHT_FINISH);
#endif
#endif

#if MAX_LIGHTS > 4
    // UNOFFICIAL 2026-10-13: lightOffset, as for the lamps above (missing: in the OPT passes lamps 5 and 6 took the previous
    // lamp's position for their falloff).
    att = vanillaAtt(PSLightPosition[lightOffset + 3].xyz - IN.lPosition.xyz, PSLightPosition[lightOffset + 3].w);
    LAMP_IFK(4, att)   // UNOFFICIAL 2026-10-17 LampReachSkip (ObjectTemplate.hlsl): a lamp out of reach adds exactly 0
#if SHADOW_CONTRAST_ON
        SC_SPLIT((4 >= lightsUsed ? 0.0 : 1.0) * getPointLightLightingAtt(IN.light5.xyz, att, PSLightColor[4].rgb, viewDir.xyz, normal.xyz, baseColor.rgb, LIGHT_FINISH), POINT_SHADOW(4, att))
#else
        lighting += (4 >= lightsUsed ? 0.0 : 1.0) * getPointLightLightingAtt(IN.light5.xyz, att, SHADOWED(PSLightColor[4].rgb, 4, att), viewDir.xyz, normal.xyz, baseColor.rgb, LIGHT_FINISH);
#endif

    att = vanillaAtt(PSLightPosition[lightOffset + 4].xyz - IN.lPosition.xyz, PSLightPosition[lightOffset + 4].w);
    LAMP_IFK(5, att)   // UNOFFICIAL 2026-10-17 LampReachSkip (ObjectTemplate.hlsl): a lamp out of reach adds exactly 0
#if SHADOW_CONTRAST_ON
        SC_SPLIT((5 >= lightsUsed ? 0.0 : 1.0) * getPointLightLightingAtt(IN.light6.xyz, att, PSLightColor[5].rgb, viewDir.xyz, normal.xyz, baseColor.rgb, LIGHT_FINISH), POINT_SHADOW(5, att))
#else
        lighting += (5 >= lightsUsed ? 0.0 : 1.0) * getPointLightLightingAtt(IN.light6.xyz, att, SHADOWED(PSLightColor[5].rgb, 5, att), viewDir.xyz, normal.xyz, baseColor.rgb, LIGHT_FINISH);
#endif
#endif
#endif // MANY_LAMPS (and NVR_LIGHT_DEFERRED)
