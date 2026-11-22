// Land lighting shared by the terrain templates (TerrainTemplate, TerrainLODTemplate, TerrainFadeTemplate): layer blending and the
// sun / point light / sky terms, vanilla or PBR (TESR_TerrainExtraData.x).
#ifdef __INTELLISENSE__
    #include "Helpers.hlsl"
    #include "Pointlights.hlsl"
    #include "PBR.hlsl"
#else
    #include "includes/Pointlights.hlsl"	// falloff curves
    #include "includes/PBR.hlsl"	// PBR(), PBRSun()
#endif

// UNOFFICIAL 2026-10-17 (deep roadmap). x: IsoHeightTaps -> the parallax HEIGHT taps (march and self-shadow, Parallax.hlsl) use
// one isotropic footprint the size of the pixel's shorter axis: a single trilinear lookup at the mip anisotropic filtering
// would pick, without its extra taps along the long axis (the game's iMaxAnisotropy applies to the land maps); colour and
// normal taps keep the real gradients. In game at noon: ground 4.0 -> 2.4 ms, change 1.3x the frame-to-frame flicker. After
// Wronski, "Assassin's Creed 4: Road to Next-Gen Graphics", GDC 2014 ("turn off aniso filtering on heightmaps").
// y: SkipEmptyLayers -> blendDiffuseMaps/blendNormalMaps skip layers with weight exactly 0 (below).
// c94: free in every land variant (the game's land constants end at c93, Shadow.hlsl starts at c100).
float4 TESR_TerrainParallaxIso : register(c94);
// UNOFFICIAL 2026-10-19 [Shaders.Terrain.LODBlending] (after Community Shaders' LOD Blending, GPL-3.0; Terrain.cpp): x the far land's
// brightness, y its gamma, z 1 = the near land without its vertex colours, w 1 = the far land adjusted (0: untouched). c97: clear of
// the land's own constants, NVR's c89-c96 and Shadow.hlsl's c100-c133.
float4 TESR_LODBlendData : register(c97);

// UNOFFICIAL 2026-10-19 [Shaders.Terrain.Variation] (Includes/TerrainVariation.hlsl, after Conor's port of Community Shaders' Terrain
// Variation): compiled in only when Enabled at startup (TERRAIN_VARIATION from ShaderRecord::LoadShader); the first
// TERRAIN_VARIATION_LAYERS layers (by index) take the two hashed taps. Otherwise the land shaders are exactly what they were.
#ifndef TERRAIN_VARIATION
    #define TERRAIN_VARIATION 0
#endif
#ifndef TERRAIN_VARIATION_LAYERS
    #define TERRAIN_VARIATION_LAYERS 3
#endif
#if TERRAIN_VARIATION
    #ifdef __INTELLISENSE__
        #include "TerrainVariation.hlsl"
    #else
        #include "includes/TerrainVariation.hlsl"
    #endif
#endif

// UNOFFICIAL 2026-11-21 [Shaders.Terrain.*] SkylightingSaturation: terrain reads TESR_SkyIrradianceTerrain[9] at c137.
#define SKY_AMBIENT_TERRAIN
#ifdef __INTELLISENSE__
    #include "SkyAmbient.hlsl"
#else
    #include "includes/SkyAmbient.hlsl"	// SkyAmbientRadiance
#endif

// UNOFFICIAL linear lighting (Includes/LinearLight.hlsl): where a template defines LINEAR_LIGHTING the sky's colour is decoded
// too (LinColor); otherwise LinColor returns it unchanged.
#ifdef __INTELLISENSE__
    #include "LinearLight.hlsl"
#else
    #include "includes/LinearLight.hlsl"
#endif
#define LinSky(c) LinColor(c)

float4 TESR_TerrainData      : register(c89);	// x metallic scale, y roughness scale, z light scale, w ambient scale
float4 TESR_TerrainExtraData : register(c90);	// x use PBR, y saturation, z NoiseScale, w NoiseTile

// Hemisphere skylight, mirroring Includes/Object.hlsl. Terrain never includes that file -- it
// carries its own getVanillaLightingAtt -- so the constants are declared here. c134/c135 are
// clear on this side: the terrain chain tops out at c92 and Shadow.hlsl pins c100-c133.
// [Shaders.Terrain.*] SkylightingScale in .x. Its own constant because TESR_TerrainExtraData is
// full: x usePBR, y saturation, z NoiseScale, w NoiseTile.
float4 TESR_TerrainSkyData : register(c135);

// Additive upper-sky term on top of the weather ambient, weighted by w = (1 + N.up) / 2.
// w must stay linear in the dot product: that is the exact cosine-weighted form factor.
// No separate toggle: a strength of 0 disables it.
#ifndef SKY_AMBIENT_STRENGTH
    #define SKY_AMBIENT_STRENGTH (TESR_TerrainSkyData.x)	// sky term at full view of the sky
#endif

// UNOFFICIAL 2026-10-17 SkipEmptyLayers (TESR_TerrainParallaxIso.y): a layer whose blend weight is exactly 0 adds exactly 0,
// so its colour and normal lookups are skipped (fixes-b #10: tested != 0, not > 0, so a slightly NEGATIVE weight at an MSAA edge pixel
// still adds its texel, exactly as with the option off) (with the game's anisotropic filtering each one is many taps). Inside a branch
// the lookups take explicit gradients: odx/ody = ddx/ddy of the same uv, taken at the top level (what tex2D used implicitly).
// Harness, real land maps, aniso 15: -12..-14% at 5-7 layers with patchy weights, +0..2% when every layer is present;
// largest pixel difference 3e-7 (rounding).
float3 blendDiffuseMaps(float3 tint, float2 uv, float2 odx, float2 ody, int layers, sampler2D maps[7], float weights[7]) {
    float3 sum = 0.0f.xxx;
    [unroll] for (int k = 0; k < layers; k++) {
        bool wanted = weights[k] != 0 || TESR_TerrainParallaxIso.y < 0.5f;
        [branch] if (wanted) sum += tex2Dgrad(maps[k], uv, odx, ody).xyz * weights[k];
    }
    return sum * tint;
}

#if TERRAIN_VARIATION
// UNOFFICIAL 2026-10-19 Terrain Variation: blendDiffuseMaps with the first TERRAIN_VARIATION_LAYERS layers taking two hashed taps
// where the pixel asks for them (stochastic.tap1Weight below 1; the template sets it to 1 nearer than StartDistance). Each such
// layer's tap weight goes out in tapWeights for its normal map. The i < TERRAIN_VARIATION_LAYERS test is on the unrolled index,
// so it folds away. (After Conor's blendDiffuseMaps, 66653eea.)
float3 blendDiffuseMapsVaried(float3 tint, float2 uv, float2 odx, float2 ody, int layers, sampler2D maps[7], float weights[7], StochasticOffsets stochastic, inout float tapWeights[7]) {
    float3 sum = 0.0f.xxx;
    [unroll] for (int k = 0; k < layers; k++) {
        bool wanted = weights[k] != 0 || TESR_TerrainParallaxIso.y < 0.5f;
        [branch] if (wanted) {
            if (k < TERRAIN_VARIATION_LAYERS) {
                [branch] if (stochastic.tap1Weight < 0.999f) sum += StochasticSampleAlbedo(maps[k], uv, odx, ody, stochastic, tapWeights[k]).xyz * weights[k];
                else sum += tex2Dgrad(maps[k], uv, odx, ody).xyz * weights[k];
            }
            else sum += tex2Dgrad(maps[k], uv, odx, ody).xyz * weights[k];
        }
    }
    return sum * tint;
}

float3 blendNormalMapsVaried(float2 uv, float2 odx, float2 ody, int layers, sampler2D maps[7], float weights[7], float specPow[7], StochasticOffsets stochastic, float tapWeights[7], out float glossOut, out float powOut) {
    glossOut = 0.0f;
    powOut = 0.0f;
    float3 nsum = 0.0f.xxx;
    float4 texel;
    [unroll] for (int k = 0; k < layers; k++) {
        bool wanted = weights[k] != 0 || TESR_TerrainParallaxIso.y < 0.5f;
        [branch] if (wanted) {
            if (k < TERRAIN_VARIATION_LAYERS) {
                [branch] if (stochastic.tap1Weight < 0.999f) texel = StochasticSampleWith(maps[k], uv, odx, ody, stochastic, tapWeights[k]);
                else texel = tex2Dgrad(maps[k], uv, odx, ody);
            }
            else texel = tex2Dgrad(maps[k], uv, odx, ody);
            nsum += texel.xyz * weights[k];
            glossOut += texel.w * weights[k] * (specPow[k] > 0 ? 1.0f : 0.0f);	// gloss only from layers with a specular power
            powOut += specPow[k] * weights[k];
        }
    }
    glossOut = saturate(glossOut);
    return normalize(expand(nsum));
}
#endif

// Weighted sum of the layers' normal maps (alpha = gloss), plus the blended gloss and specular power.
float3 blendNormalMaps(float2 uv, float2 odx, float2 ody, int layers, sampler2D maps[7], float weights[7], float specPow[7], out float glossOut, out float powOut) {
    glossOut = 0.0f;
    powOut = 0.0f;
    float3 nsum = 0.0f.xxx;
    float4 texel;
    [unroll] for (int k = 0; k < layers; k++) {
        bool wanted = weights[k] != 0 || TESR_TerrainParallaxIso.y < 0.5f;	// UNOFFICIAL SkipEmptyLayers (see blendDiffuseMaps)
        [branch] if (wanted) {
            texel = tex2Dgrad(maps[k], uv, odx, ody);
            nsum += texel.xyz * weights[k];
            glossOut += texel.w * weights[k] * (specPow[k] > 0 ? 1.0f : 0.0f);
            powOut += specPow[k] * weights[k];
        }
    }
    glossOut = saturate(glossOut);
    return normalize(expand(nsum));
}

// The game's land lighting: Lambert plus a Blinn highlight that is faded out where the surface turns from the light.
float3 getVanillaLightingAtt(float3 toLight, float falloff, float3 radiance, float3 toEye, float3 N, float3 baseColor, float glossAmt, float specPower) {
    float3 L = normalize(toLight);
    float3 V = normalize(toEye);
    float3 H = normalize(L + V);

    float cosL = shades(N.xyz, L.xyz);
    float highlight = glossAmt * pow(abs(shades(N.xyz, H.xyz)), specPower);
    float3 result = baseColor.rgb * cosL * radiance.rgb * falloff;
    float fadedHighlight = 0.2 >= cosL ? (highlight * saturate(cosL + 0.5)) : highlight;
    result += saturate((fadedHighlight * radiance.rgb) * falloff);
    return result;
}

float3 getPointLightLighting(float3 toLight, float falloff, float3 radiance, float3 toEye, float3 N, float3 baseColor, float glossAmt = 0.0, float specPower = 0.0, float metal = 1.0) {
    float3 scaledRadiance = radiance * LinScale(TESR_TerrainData.z);

    [branch] if (TESR_TerrainExtraData.x) {
        float r = saturate((1 - glossAmt) * TESR_TerrainData.y);
        float3 pbrLit = PBR(saturate(metal * TESR_TerrainData.x), r, baseColor, N, toEye, toLight, scaledRadiance);
        return max(0, pbrLit * falloff);
    }
    else {
        // the game's look (it is not scaled by TESR_TerrainData.z)
        return getVanillaLightingAtt(normalize(toLight), falloff, radiance, toEye, N, baseColor, glossAmt, specPower);
    }
}

float3 getSunLighting(float3 toSun, float3 sunRadiance, float3 toEye, float3 N, float3 gameAmbient, float3 baseColor, float glossAmt = 0.0, float specPower = 0.0, float metal = 1.0, float selfShadow = 1.0, float3 geoNormalWS = float3(0.0f, 0.0f, 1.0f)) {
    float3 sunLit = sunRadiance * LinScale(TESR_TerrainData.z) * selfShadow;
    float3 ambient = gameAmbient * LinScale(TESR_TerrainData.w);

    // Hemisphere skylight, matching getAmbientLighting in Object.hlsl. geoNormalWS is the
    // geometric world normal, not the tangent-space shading normal used above. AmbientScale
    // (TESR_TerrainData.w) scales the weather ambient above but not this: the sky is a second,
    // independent light source, so SkylightingScale is its only strength knob and it survives
    // AmbientScale = 0.
    ambient += LinSky(SkyAmbientRadiance(geoNormalWS, TESR_TerrainSkyData.y)) * LinScale(SKY_AMBIENT_STRENGTH);
    float3 graded = lerp(luma(baseColor), baseColor, TESR_TerrainExtraData.y);	// saturation

    [branch] if (TESR_TerrainExtraData.x) {
        float r = saturate((1 - glossAmt) * TESR_TerrainData.y);
        float3 pbrLit = PBRSun(saturate(metal * TESR_TerrainData.x), r, graded, N, toEye, toSun, sunLit);
        return max(0, pbrLit + ambient * graded);
    }
    else {
        // the game's look: unscaled sun, ungraded colour for the direct part
        float3 direct = getVanillaLightingAtt(toSun, 1.0, sunRadiance, toEye, N, baseColor, glossAmt, specPower);
        return direct + ambient * graded;
    }
}
