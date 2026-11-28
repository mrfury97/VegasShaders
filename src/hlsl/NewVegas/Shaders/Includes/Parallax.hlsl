#ifdef TERRAIN
float4 TESR_TerrainParallaxData : register(c91);
float4 TESR_TerrainParallaxExtraData : register(c92);
// TESR_TerrainParallaxIso (IsoHeightTaps, SkipEmptyLayers): declared in Terrain.hlsl, which every land template includes first.
#else
float4 TESR_ParallaxData : register(c35);
#endif

// ParallaxLite's secant steps (terrain and objects). Measured with tests/game_shaders.cpp on a GTX 1070
// (full-screen terrain, 2560x1440), against the defaults: 0 steps -23..-37% time but four times HighQuality-off's
// picture difference; 1 step -17..-31% at about 1.5 times it; 2 steps -16..-22% at about the same; 3 steps no faster
// than HighQuality off.
#ifndef LITE_SECANT_STEPS
#define LITE_SECANT_STEPS 1
#endif

#ifdef TERRAIN
// Complex Parallax Materials for Community Shaders
// https://bartwronski.com/wp-content/uploads/2014/03/ac4_gdc.pdf
// https://www.artstation.com/blogs/andreariccardi/3VPo/a-new-approach-for-parallax-mapping-presenting-the-contact-refinement-parallax-mapping-technique
// Blended pomDepth of the land at one point: each layer's pomDepth weighted by its blend, the blendOut returned in outW.
float landHeightBlend(float2 uvAt, float2 ddxUV, float2 ddyUV, float bf, int layerCount, sampler2D layerTex[7], float inBlend[7], float hasHeight[7], out float outW[7]) {
    outW = inBlend;

    float expo = bf * 4;
    float sum = 0;
    [unroll] for (int i = 0; i < layerCount; i++){
        outW[i] = pow(abs(inBlend[i]), 1 + 1 * bf);
        if (outW[i] > 0.0) {
            outW[i] *= 0.001 + pow(abs(hasHeight[i] ? tex2Dgrad(layerTex[i], uvAt, ddxUV, ddyUV).a : 0.5f), expo);
        }
        sum += outW[i];
    }

    float invSum = rcp(sum);

    [unroll] for (int j = 0; j < layerCount; j++)
    {
        outW[j] *= invSum;
    }

    return pow(sum, rcp(expo));
}
#endif

#ifndef TERRAIN
// ReducedQuality ParallaxLite on objects (TESR_ParallaxData.w = 1): getParallaxCoords' search for objects with a pomDepth map,
// cut down as ParallaxLite cuts the terrain's: up to 8 steps in batches of four, then LITE_SECANT_STEPS secant
// steps in place of the contact refinement. Same start shift, bounds and final interpolation as getParallaxCoords.
float2 objectLiteParallax(float viewDist, float2 uv0, float2 gx, float2 gy, float3 eyeTS, sampler2D hmap) {
    float farness = saturate(viewDist / 2048.0f);
    if (farness >= 1.0) return uv0;

    eyeTS = normalize(eyeTS);
    float fullDepth = 0.1 * TESR_ParallaxData.x;
    float halfDepth = fullDepth * 0.5;

    int stepsLeft = int((8.0f * (1.0 - farness)) + 0.5);
    stepsLeft = clamp(((stepsLeft + 3) / 4) * 4, 4, 8);
    float layerH = rcp((float) stepsLeft);
    float2 uvPerLayer = eyeTS.xy * fullDepth * layerH;
    float2 lastUV = eyeTS.xy * halfDepth + uv0.xy;
    float lastRay = 1.0;
    float lastSurf = 1.0;
    float2 ptHit = 0;
    float2 ptPrev = 0;
    bool hit = false;

    [loop] while (stepsLeft > 0 && !hit) {
        float4 tapUV[2];
        tapUV[0] = lastUV.xyxy - float4(1, 1, 2, 2) * uvPerLayer.xyxy;
        tapUV[1] = lastUV.xyxy - float4(3, 3, 4, 4) * uvPerLayer.xyxy;
        float4 rayAt = lastRay.xxxx - float4(1, 2, 3, 4) * layerH;
        float4 surfAt;
        surfAt.x = tex2Dgrad(hmap, tapUV[0].xy, gx, gy).r;
        surfAt.y = tex2Dgrad(hmap, tapUV[0].zw, gx, gy).r;
        surfAt.z = tex2Dgrad(hmap, tapUV[1].xy, gx, gy).r;
        surfAt.w = tex2Dgrad(hmap, tapUV[1].zw, gx, gy).r;
        bool4 hits = surfAt >= rayAt;
        [branch] if (any(hits)) {
            [flatten] if (hits.w) { ptHit = float2(rayAt.w, surfAt.w); ptPrev = float2(rayAt.z, surfAt.z); }
            [flatten] if (hits.z) { ptHit = float2(rayAt.z, surfAt.z); ptPrev = float2(rayAt.y, surfAt.y); }
            [flatten] if (hits.y) { ptHit = float2(rayAt.y, surfAt.y); ptPrev = float2(rayAt.x, surfAt.x); }
            [flatten] if (hits.x) { ptHit = float2(rayAt.x, surfAt.x); ptPrev = float2(lastRay, lastSurf); }
            hit = true;
        }
        else {
            lastUV = tapUV[1].zw;
            lastRay = rayAt.w;
            lastSurf = surfAt.w;
            stepsLeft -= 4;
        }
    }

    [branch] if (hit) {
        [unroll] for (int r = 0; r < LITE_SECANT_STEPS; r++) {	// (unroll: 1 step by default; [loop] warned X3557 at every compile)
            float gapA = ptPrev.x - ptPrev.y;
            float gapB = ptHit.x - ptHit.y;
            float gapSpan = gapA - gapB;
            float xing = gapSpan == 0.0 ? ptHit.x : (ptHit.x * gapA - ptPrev.x * gapB) / gapSpan;
            float xingSurf = tex2Dgrad(hmap, eyeTS.xy * ((1.0 - xing) * -fullDepth + halfDepth) + uv0.xy, gx, gy).r;
            if (xingSurf >= xing) ptHit = float2(xing, xingSurf);
            else ptPrev = float2(xing, xingSurf);
        }
    }

    float gapAbove = ptPrev.x - ptPrev.y;
    float gapBelow = ptHit.x - ptHit.y;
    float span = gapAbove - gapBelow;
    float depthT = span == 0.0 ? 0.0 : (ptHit.x * gapAbove - ptPrev.x * gapBelow) / span;
    float shift = (1.0 - depthT) * -fullDepth + halfDepth;
    // UNOFFICIAL: the bumps fade as before (1 - d^2) up to 3/4 of the viewDist, then ease out to nothing with no slope left
    // at the end: the old fade dropped fastest right at the cut-off, a line you could see sliding along as you walked.
    const float strength = (1.0 - farness * farness) * (1.0 - smoothstep(0.75, 1.0, farness));
    return lerp(uv0, eyeTS.xy * shift + uv0.xy, strength);
}
#endif

#ifdef TERRAIN
float2 getParallaxCoords(
    float viewDist,
    float2 uv0,
    float2 gx,
    float2 gy,
    float3 eyeTS,
    int layerN,
    sampler2D maps[7],
    float blendIn[7],
    float hasH[7],
    out float blendOut[7]
) {
#else
float2 getParallaxCoords(float viewDist, float2 uv0, float2 gx, float2 gy, float3 eyeTS, sampler2D hmap) {
#endif
    #ifdef TERRAIN
        // Check if parallax is active first.
        if (!TESR_TerrainParallaxData.x) {
            blendOut = blendIn;
            return uv0;
        }

        // Variables. TESR_TerrainParallaxData.w: 0 = 8 steps, 1 = 16 steps (HighQuality), 2 = liteMode
        // (ReducedQuality ParallaxLite): 8 steps, then secant steps instead of the contact
        // refinement (see below), and two parallax shadow taps instead of four.
        static const bool liteMode = TESR_TerrainParallaxData.w > 1.5f;
        static const float stepCap = (TESR_TerrainParallaxData.w == 1.0f) ? 16.0f : 8.0f;
        float fadeEnd = TESR_TerrainParallaxExtraData.x;
        float pomDepth = TESR_TerrainParallaxExtraData.y;
    #else
        // ReducedQuality ParallaxLite: a separate function, so that this path compiles exactly as before
        // (sharing the loop with the liteMode code cost the default 4-9% in tests/game_shaders.cpp).
        [branch] if (TESR_ParallaxData.w > 0.5f) return objectLiteParallax(viewDist, uv0, gx, gy, eyeTS, hmap);

        static const float stepCap = 16.0f;
        float fadeEnd = 2048;
        float pomDepth = 0.1 * TESR_ParallaxData.x;
    #endif

    float farness = saturate(viewDist / fadeEnd);
    float nearness = saturate(1.0 - farness);

    #ifdef TERRAIN
        float sharpen = TESR_TerrainParallaxData.z ? nearness : 0.25;
    #endif

    eyeTS = normalize(eyeTS);

    #ifdef TERRAIN
        // UNOFFICIAL IsoHeightTaps (see TESR_TerrainParallaxIso): only the pomDepth taps below see these gradients.
        // (Not with terrain blending at 7 layers, or 6 layers with SkylightingMode 1 (fixes-b #8, full define matrix compiled): there the branch takes the shader past ps_3_0's 32 temporary registers and the
        // game falls back to its own shader. Those draws strength the anisotropic pomDepth lookups.)
// (The no-limits build, NVR_NO_SM3_LIMITS: see TerrainTemplate.hlsl. No temp limit there, so every land shader has it.)
#if NVR_NO_SM3_LIMITS || !(TERRAIN_BLEND && !SUN_CROSSFADE && (TEX_COUNT >= 7 || (TEX_COUNT >= 6 && SKYLIGHTING_MODE)))   // fixes-b #8: 6 layers + SkylightingMode 1 also exceeds 32 temps
        [branch] if (TESR_TerrainParallaxIso.x > 0.5f) { float L = sqrt(min(dot(gx, gx), dot(gy, gy))); gx = float2(L, 0); gy = float2(0, L); }
#endif
        // Fix for angles.
        eyeTS.z = ((eyeTS.z * 0.7) + 0.3);
        eyeTS.xy /= eyeTS.z;
    #endif

    float fullDepth = pomDepth;
    float halfDepth = fullDepth * 0.5;

    if (farness < 1.0)
    {
        int stepsLeft = int((stepCap * (1.0 - farness)) + 0.5);
        stepsLeft = ((stepsLeft + 3) / 4) * 4;
        stepsLeft = clamp(stepsLeft, 4, stepCap);

        float layerH = rcp((float) stepsLeft);

        float2 uvPerLayer = eyeTS.xy * float2(fullDepth, fullDepth) * layerH.xx;
        float2 lastUV = eyeTS.xy * float2(halfDepth, halfDepth) + uv0.xy;

        float lastRay = 1.0;
        float lastSurf = 1.0;

        float2 ptHit = 0;
        float2 ptPrev = 0;

        int stepsAll = stepsLeft;
        bool finished = false;
        bool refining = false;

        // Need fastopt otherwise compile times are crazy.
        [loop][fastopt] while (stepsLeft > 0 && !finished) {
            float4 tapUV[2];
            tapUV[0] = lastUV.xyxy - float4(1, 1, 2, 2) * uvPerLayer.xyxy;
            tapUV[1] = lastUV.xyxy - float4(3, 3, 4, 4) * uvPerLayer.xyxy;
            float4 rayAt = lastRay.xxxx - float4(1, 2, 3, 4) * layerH;

            float4 surfAt;

            #ifdef TERRAIN
                surfAt.x = landHeightBlend(tapUV[0].xy, gx, gy, sharpen, layerN, maps, blendIn, hasH, blendOut);
                surfAt.y = landHeightBlend(tapUV[0].zw, gx, gy, sharpen, layerN, maps, blendIn, hasH, blendOut);
                surfAt.z = landHeightBlend(tapUV[1].xy, gx, gy, sharpen, layerN, maps, blendIn, hasH, blendOut);
                surfAt.w = landHeightBlend(tapUV[1].zw, gx, gy, sharpen, layerN, maps, blendIn, hasH, blendOut);
            #else
                surfAt.x = tex2Dgrad(hmap, tapUV[0].xy, gx, gy).r;
                surfAt.y = tex2Dgrad(hmap, tapUV[0].zw, gx, gy).r;
                surfAt.z = tex2Dgrad(hmap, tapUV[1].xy, gx, gy).r;
                surfAt.w = tex2Dgrad(hmap, tapUV[1].zw, gx, gy).r;
            #endif

            bool4 hits = surfAt >= rayAt;

            [branch] if (any(hits))
            {
                float2 hitUV = 0;
                [flatten] if (hits.w)
                {
                    hitUV = tapUV[1].xy;
                    ptHit = float2(rayAt.w, surfAt.w);
                    ptPrev = float2(rayAt.z, surfAt.z);
                }
                [flatten] if (hits.z)
                {
                    hitUV = tapUV[0].zw;
                    ptHit = float2(rayAt.z, surfAt.z);
                    ptPrev = float2(rayAt.y, surfAt.y);
                }
                [flatten] if (hits.y)
                {
                    hitUV = tapUV[0].xy;
                    ptHit = float2(rayAt.y, surfAt.y);
                    ptPrev = float2(rayAt.x, surfAt.x);
                }
                [flatten] if (hits.x)
                {
                    hitUV = lastUV;
                    ptHit = float2(rayAt.x, surfAt.x);
                    ptPrev = float2(lastRay, lastSurf);
                }

                #ifdef TERRAIN
                if (refining || liteMode) {
                #else
                if (refining) {
                #endif
                    finished = true;
                }
                else {
                    refining = true;
                    lastUV = hitUV;
                    lastRay = ptPrev.x;
                    stepsLeft = stepsAll;
                    layerH /= (float) stepsLeft;
                    uvPerLayer /= (float) stepsLeft;
                }
            }
            else {
                lastUV = tapUV[1].zw;
                lastRay = rayAt.w;
                lastSurf = surfAt.w;
                stepsLeft -= 4;
            }
        }

        #ifdef TERRAIN
        // Lite: in place of the contact refinement (a second march of up to 16 steps through the bracketing step),
        // secant steps: evaluate the pomDepth where the straight line between the bracket's two samples crosses the
        // ray, and strength the half that still brackets the hit. One pomDepth lookup each instead of four or more.
        [branch] if (liteMode && finished) {
            // [loop], not [unroll], although it warns X3557 (one step) at every compile: [unroll] (10-19 08af8504) took the land shaders
            // with 6 and 7 layers and terrain blending on past ps_3_0's 32 temporary registers (X4505, they failed to load; 2026-10-20).
            [loop] for (int r = 0; r < LITE_SECANT_STEPS; r++) {
                float gapA = ptPrev.x - ptPrev.y;
                float gapB = ptHit.x - ptHit.y;
                float gapSpan = gapA - gapB;
                float xing = gapSpan == 0.0 ? ptHit.x : (ptHit.x * gapA - ptPrev.x * gapB) / gapSpan;
                float2 xingUV = eyeTS.xy * ((1.0 - xing) * -fullDepth + halfDepth) + uv0.xy;
                float xingSurf = landHeightBlend(xingUV, gx, gy, sharpen, layerN, maps, blendIn, hasH, blendOut);
                if (xingSurf >= xing) ptHit = float2(xing, xingSurf);
                else ptPrev = float2(xing, xingSurf);
            }
        }
        #endif

        float gapAbove = ptPrev.x - ptPrev.y;
        float gapBelow = ptHit.x - ptHit.y;

        float span = gapAbove - gapBelow;

        float depthT = 0.0;
        if (span == 0.0)
        {
            depthT = 0.0;
        }
        else
        {
            depthT = (ptHit.x * gapAbove - ptPrev.x * gapBelow) / span;
        }

        float shift = (1.0 - depthT) * -fullDepth + halfDepth;
        #ifdef TERRAIN
        // UNOFFICIAL, liteMode: the same ease-out at the cut-off as the objects' liteMode path (see objectLiteParallax).
        [branch] if (liteMode) {
            const float strength = (1.0 - farness * farness) * (1.0 - smoothstep(0.75, 1.0, farness));
            return lerp(uv0, eyeTS.xy * shift + uv0.xy, strength);
        }
        #endif

        farness *= farness;
        return lerp(eyeTS.xy * shift + uv0.xy, uv0, farness);
    }

    #ifdef TERRAIN
        blendOut = blendIn;
    #endif
    return uv0;
}

#ifdef TERRAIN
float getParallaxShadowMultipler(float viewDist, float2 uv0, float2 gx, float2 gy, float3 lightTS, int layerN, float blendIn[7], float hasH[7], sampler2D maps[7]) {
    if (!TESR_TerrainParallaxData.y || lightTS.z <= 0.0f)
        return 1.0;

    float fadeEnd = TESR_TerrainParallaxExtraData.x;
    float shadowGain = TESR_TerrainParallaxExtraData.z;

    float nearness = 1.0 - viewDist / fadeEnd;

    if (nearness > 0.0)
    {
#if NVR_NO_SM3_LIMITS || !(TERRAIN_BLEND && !SUN_CROSSFADE && (TEX_COUNT >= 7 || (TEX_COUNT >= 6 && SKYLIGHTING_MODE)))   // as in getParallaxCoords
        [branch] if (TESR_TerrainParallaxIso.x > 0.5f) { float L = sqrt(min(dot(gx, gx), dot(gy, gy))); gx = float2(L, 0); gy = float2(0, L); }  // UNOFFICIAL IsoHeightTaps
#endif
        float blendOut[7] = { 0, 0, 0, 0, 0, 0, 0 };
        float h0 = landHeightBlend(uv0, gx, gy, nearness, layerN, maps, blendIn, hasH, blendOut);

        const float2 rayUV = lightTS.xy * 0.1;
        float4 stepFrac = rcp((float4(1, 2, 3, 4)));

        float4 hRay = landHeightBlend(uv0 + rayUV * stepFrac.x, gx, gy, nearness, layerN, maps, blendIn, hasH, blendOut);
        // Lite (TESR_TerrainParallaxData.w = 2): two taps, at 1 and 1/2 of the ray, each counted twice, so the
        // four-tap sum keeps its scale.
        [branch] if (TESR_TerrainParallaxData.w > 1.5f) {
            // UNOFFICIAL: the second tap fades in between nearness 0.15 and 0.35 instead of switching on at 0.25, and the last
            // quarter of the viewDist eases out: the two rings on the ground (where the tap came in, and where the shadows
            // ended) moved along with you as you walked.
            if (nearness > 0.15) {
                const float h2 = landHeightBlend(uv0 + rayUV * stepFrac.y, gx, gy, nearness, layerN, maps, blendIn, hasH, blendOut);
                hRay.yw = lerp(hRay.xx, h2.xx, saturate((nearness - 0.15) / 0.2));
            }
            return 1.0 - saturate(dot(max(0, hRay - h0), 1.0) * shadowGain) * nearness * smoothstep(0.0, 0.25, nearness);
        }
        if (nearness > 0.25)
            hRay.y = landHeightBlend(uv0 + rayUV * stepFrac.y, gx, gy, nearness, layerN, maps, blendIn, hasH, blendOut);
        if (nearness > 0.5)
            hRay.z = landHeightBlend(uv0 + rayUV * stepFrac.z, gx, gy, nearness, layerN, maps, blendIn, hasH, blendOut);
        if (nearness > 0.75)
            hRay.w = landHeightBlend(uv0 + rayUV * stepFrac.w, gx, gy, nearness, layerN, maps, blendIn, hasH, blendOut);

        return 1.0 - saturate(dot(max(0, hRay - h0), 1.0) * shadowGain) * nearness;
    }

    return 1.0;
}
#else
float getParallaxShadowMultipler(float viewDist, float2 uv0, float2 gx, float2 gy, float3 lightTS, sampler2D hmap) {
    float fadeEnd = 2048;
    float shadowGain = 2;

    float nearness = 1.0 - viewDist / fadeEnd;

    if (nearness > 0.0)
    {
        float h0 = tex2Dgrad(hmap, uv0, gx, gy).r;

        const float2 rayUV = lightTS.xy * 0.04;
        float4 stepFrac = rcp((float4(1, 2, 3, 4)));

        // (ParallaxLite leaves these taps alone: one fetch each here, and a liteMode branch cost the default
        // more than it saved.)
        float4 hRay = tex2Dgrad(hmap, uv0 + rayUV * stepFrac.x, gx, gy).r;
        if (nearness > 0.25)
            hRay.y = tex2Dgrad(hmap, uv0 + rayUV * stepFrac.y, gx, gy).r;
        if (nearness > 0.5)
            hRay.z = tex2Dgrad(hmap, uv0 + rayUV * stepFrac.z, gx, gy).r;
        if (nearness > 0.75)
            hRay.w = tex2Dgrad(hmap, uv0 + rayUV * stepFrac.w, gx, gy).r;

        return 1.0 - saturate(dot(max(0, hRay - h0), 1.0) * shadowGain) * nearness;
    }

    return 1.0;
}
#endif
