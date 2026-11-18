

// Requires the following registers:
//
//   Name            Reg   Size
//   --------------- ----- ----
//   EyePos          const_1       1
//   shallowColor    const_2       1
//   deepColor       const_3       1
//   ReflectionColor const_4       1
//   FresnelRI       const_5       1  //x: reflectamount, y:fresnel, w: opacity, z:speed
//   BlendRadius     const_6       1
//   VarAmounts      const_8       1  // x: water glossiness y: reflectivity z: refrac, w: lod
//   FogParam        const_9       1
//   FogColor        const_10      1
//   DepthFalloff    const_11      1  // start / end depth fog
//   SunDir          const_12      1
//   SunColor        const_13      1
//   ReflectionMap   texture_0       1
//   RefractionMap   texture_1       1
//   NoiseMap        texture_2       1
//   DisplacementMap texture_3       1
//   DepthMap        texture_4       1
//   float4 TESR_WaveParams : register(c14); // x: choppiness, y:wave width, z: wave speed, w: reflectivity?
//   float4 TESR_WaterVolume : register(c15); // x: caustic strength, y:shoreFactor, w: turbidity, z: caustic strength S ?
//   float4 TESR_WaterSettings : register(c16); // x: caustic strength, y:depthDarkness, w: turbidity, z: caustic strength S ?
//   float4 TESR_GameTime : register(c17);
//   float4 TESR_SkyColor : register(c18);
//   float4 TESR_SunDirection : register(c19);
//   sampler2D TESR_samplerWater : register(s5);


// The game's water vertex shader outputs (member names are what the WATER*.pso shaders use).
struct PS_INPUT {
    float4 LTEXCOORD_0: TEXCOORD0_centroid;	// camera-relative world position (of the point under water)
    float4 LTEXCOORD_1: TEXCOORD1_centroid;	// the point on the water plane, object space
    float4 LTEXCOORD_2: TEXCOORD2_centroid;	// object-to-clip, row 0
    float4 LTEXCOORD_3: TEXCOORD3_centroid;	// row 1
    float4 LTEXCOORD_4: TEXCOORD4_centroid;	// row 2
    float4 LTEXCOORD_5: TEXCOORD5_centroid;	// row 3
    float4 LTEXCOORD_6: TEXCOORD6;	// displacement map uv
    float2 LTEXCOORD_7: TEXCOORD7;	// wave map uv
    float4 WorldPosition: TEXCOORD8;
};

struct PS_OUTPUT { float4 color_0: COLOR0; };

#include "Includes/PBR.hlsl"	// BRDF, FresnelShlick

// Screen-space position of the water point: the vertex shader passes the object-to-clip rows (TEXCOORD2-5) and the local point.
float4 getScreenpos(PS_INPUT v){
    float4 clip;
    clip.x = dot(v.LTEXCOORD_2, v.LTEXCOORD_1);
    clip.w = dot(v.LTEXCOORD_5, v.LTEXCOORD_1);
    clip.y = clip.w - dot(v.LTEXCOORD_3, v.LTEXCOORD_1);	// flipped: texture space runs down
    clip.z = dot(v.LTEXCOORD_4, v.LTEXCOORD_1);
    return clip;
}

// Three scrolling samples of the water noise map, summed and turned into a normal.
float3 getWaveTexture(PS_INPUT IN, float dist, float4 waveParams) {

    float2 uv = IN.LTEXCOORD_7;

    float width = waveParams.y;
    float chop = waveParams.x;
    float drift = TESR_GameTime.x * 0.002 * waveParams.z;
    float fineScale = 0.5;
    float broadScale = 2;
    float3 fine = expand(tex2D(TESR_samplerWater, uv * fineScale * width + normalize(float2(1, 4)) * drift)).xyz * 0.5;
    float3 mid = expand(tex2D(TESR_samplerWater, uv * broadScale * width + normalize(float2(-3, -2)) * drift)).xyz * 1;
    float3 micro = expand(tex2D(TESR_samplerWater, uv * broadScale * 2 * width + normalize(float2(2, 2)) * drift)).xyz * 0.3;

    // combine waves
    float3 combined = float3(mid.xy + fine.xy + micro.xy,  mid.z + fine.z + micro.z);
    combined.z *= 1/max(chop, 0.000001);
    // combined.z *= lerp(1, 0.5, (dist / 4096)) / max(chop, 0.000001);

    return normalize(combined);
}

// Where the planar reflection map should be sampled for this water point: the local point pushed along the wave normal, then
// through the object-to-clip rows.
float4 getReflectionSamplePosition(PS_INPUT v, float3 waveN, float bend) {
    int4 k = {0, 2, -1, 1};	// swizzle source for zeroing / doubling / flipping parts

    float4 pushed;
    pushed.xy = ((bend * waveN.xy)) + v.LTEXCOORD_1.xy;
    pushed.zw = (v.LTEXCOORD_1.z * k.wx) + k.xw;

    float4x4 toClip = float4x4(v.LTEXCOORD_2, v.LTEXCOORD_3, v.LTEXCOORD_4, v.LTEXCOORD_5);
    return mul(toClip, pushed);
}

// UNOFFICIAL 2026-11-16 RenderScale (docs/next/INTERIOR-WATER.md): the projective lookup of the game's refraction copy. While the world is
// drawn smaller (renderScale.xy < 1) the copy holds its picture in its top-left renderScale.xy part, the world target's own layout, so the
// projected position is scaled by it; a lookup the waves bend past that part's right or bottom edge is held at its last texel, as the
// sampler's clamp holds one bent past the copy's own edge at full size (unheld, it read the target's stale area outside the picture: a dark
// band where the water meets the frame's right or bottom edge). min() hands back one of its operands, so with RenderScale off (1) the
// lookup is exactly the plain projected position, as before.
float4 refractionLookup(float4 pos, float4 renderScale, float4 reciprocalResolution) {
    float2 last = (renderScale.x < 0.999 || renderScale.y < 0.999) ? renderScale.xy - 0.5 * reciprocalResolution.xy : 1e6;
    return float4(min(pos.xy * renderScale.xy, last * pos.w), pos.zw);
}

#ifdef RT_WATER
// UNOFFICIAL 2026-10-31 RT water reflections ([Shaders.Reflections.RT] Water, docs/next/FORK-RT-REFLECTIONS.md "Water"): last frame's
// RT reflection of this spot (TESR_ReflectionsBuffer, half resolution: Tame(colour) x confidence, confidence), found by reprojecting
// the surface point -- moved along the waves as the planar map's lookup is (refractionCoeff x the wave normal, in world units on the
// plane) -- into the frame that traced it. Where it holds nothing (off screen then, past DrawDistance, RT not running: TESR_RTWaterData.x 0)
// the planar map, or the horizon colour while the planar pass is skipped (TESR_RTWaterData.y).
float4 getRTWaterReflection(PS_INPUT IN, float3 surfaceNormal, float refractionCoeff, float4 planar, float4 horizon) {
    [branch] if (TESR_RTWaterData.x < 0.5) return planar;
    float3 p = IN.LTEXCOORD_0.xyz;   // camera-relative world (the vertex shader's WorldMat)
    p.xy += refractionCoeff * surfaceNormal.xy;
    float4 clip = mul(float4(p, 1.0), TESR_RTWaterReproject);
    float2 uv = clip.xy / max(clip.w, 1.0e-4) * float2(0.5, -0.5) + 0.5;
    // UNOFFICIAL 2026-11-03 RenderScale < 1: a rect frame traced the water into Buffer's top-left part; TESR_RTWaterReproject comes
    // pre-scaled into it (Reflections.cpp UpdateWaterConstants) and .w is that frame's scale (1 native; 0 from an older DLL: 1).
    float edge = TESR_RTWaterData.w > 0.0 ? TESR_RTWaterData.w : 1.0;
    float inside = (clip.w > 0.0 && uv.x > 0.0 && uv.y > 0.0 && uv.x < edge && uv.y < edge) ? 1.0 : 0.0;
    float4 s = tex2Dlod(TESR_ReflectionsBuffer, float4(uv, 0.0, 0.0));
    float share = saturate(s.a) * inside * TESR_RTWaterData.z;
    float3 t = s.rgb / max(s.a, 1.0e-4);
    float3 traced = t / max(1.0 - dot(t, float3(0.2126, 0.7152, 0.0722)), 1.0e-3);   // Untame (Compute/Reflections.glsl)
    float3 otherwise = TESR_RTWaterData.y > 0.5 ? horizon.rgb : planar.rgb;
    return float4(lerp(otherwise, traced, share), planar.a);
}
#endif

// Tilts the wave normal by the displacement map (its zw), scaled by the blend radius.
float3 getDisplacement(PS_INPUT v, float radius, float3 waveN){
    float4 dmap = tex2D(DisplacementMap, v.LTEXCOORD_6.xy);
    dmap.xy = (dmap.zw - 0.5) * radius / 2;
    float3 dN = normalize(reconstructZ(dmap.xy));

    float3 tilted = float3(waveN.xy + dN.xy * 2, waveN.z * dN.z);
    return normalize(tilted);
}

// Light that came up through the water column: tinted from the shallow toward the deep colour with depth (y), and never fully
// passed (at most 0.7 of it at depth x = 0), so the water always absorbs a little.
float4 getLightTravel(float3 depths, float4 shallowTint, float4 deepTint, float sunY, float4 settings, float4 under){
    float4 tint = lerp(shallowTint, deepTint, depths.y);
    float clarity = saturate(pows((1 - settings.y), 3));	// depth darkness: high setting, low clarity
    float3 passed = under.rgb * lerp(0.7, lerp(tint.rgb * clarity, 1, clarity), depths.x);
    return float4(passed, 1);
}

// Murky water: fog from the game's water fog range over depth, by turbidity.
float4 getTurbidityFog(float3 depths, float4 shallowTint, float4 volume, float sunY, float4 under){
    float murk = volume.z;
    float amount = 1 - saturate((FogParam.z - (depths.x * FogParam.z)) / FogParam.w);
    float3 fogTint = shallowTint.rgb * sunY;
    return float4(lerp(under.rgb, fogTint.rgb, saturate(amount * FogColor.a * murk)), 1);
}

// Sunlit diffuse term (not used by the water shaders at the moment): stronger far away and at low view angles.
float4 getDiffuse(float3 waveN, float3 toSun, float3 toEye, float distance, float4 tint, float4 under){
    float grazing = (1 - shades(toEye, float3(0, 0, 1)));
    float distFade = smoothstep(0, 1, distance * 0.001);
    float amount = shades(toSun, waveN) * grazing * distFade;
    return float4(lerp(under.rgb, tint.rgb, saturate(amount)), 1);
}

// Reflection mixed in by Schlick's falloff, with a little extra where the reflection is brighter than the water.
float4 getFresnel(float3 waveN, float3 toEye, float4 reflected, float strength, float4 under){
    float schlick = saturate(pow(1 - dot(toEye, waveN), 5));
    float reflY = luma(reflected);
    float brighter = saturate(reflY - luma(under));
    float3 mixed = lerp(under.rgb, reflected.rgb, saturate((schlick * 0.8 + 0.2 * brighter) * strength));
    return float4(mixed, 1);
}

// Sun highlight: the PBR BRDF with a fixed very smooth surface and water's reflectance.
float4 getSpecular(float3 waveN, float3 toSun, float3 toEye, float3 sunTint, float4 under){
    float boost = 10;
    float3 N = normalize(waveN);
    float3 H = normalize(toEye + toSun);
    float cosH = shades(N, H);
    float cosL = shades(N, toSun);
    float cosV = shades(N, toEye);
    float3 F = FresnelShlick(0.08, H, toEye);
    return float4(under.rgb + BRDF(0.02, F, cosV, cosL, cosH) * sunTint * boost * cosL, 1);
}

// Highlight from a point light (lightPos.w = radius, 0 = no light), with the windowed falloff of Pointlights.hlsl's lisyarusAtt
// (https://lisyarus.github.io/blog/graphics/2022/07/30/point-light-attenuation.html).
float4 getPointLightSpecular(float3 waveN, float4 lightPos, float3 surfacePos, float3 toEye, float3 lightTint, float4 under){
    if (lightPos.w == 0) return under;

    float3 offset = lightPos.xyz - surfacePos;
    float rel = length(offset) / lightPos.w;
    float d2 = saturate(rel * rel);
    float falloff = saturate(((1 - d2) * (1 - d2)) / (1 + 5.0 * d2));

    float3 L = normalize(offset);
    float3 H = normalize(L + toEye);
    float cosL = shades(waveN, L);
    float cosV = shades(waveN, toEye);
    float cosH = shades(waveN, H);
    float3 F = FresnelShlick(0.08, H, toEye);
    under.rgb += BRDF(0.02, F, cosV, cosL, cosH) * lightTint * falloff * cosL;
    return under;
}

// Shoreline: alpha fades out in shallow water, the edge moving over an animated sine grid.
float4 getShoreFade(PS_INPUT v, float depth, float speed, float reach, float4 under){
    float tile = 0.07;
    float rate = speed * 0.1;
    float range = reach * 0.1;

    float wave = sin(v.LTEXCOORD_7.x/tile + TESR_GameTime.x * rate);
    wave *= cos(v.LTEXCOORD_7.y/tile + TESR_GameTime.x * rate);
    wave = compress(wave);	// grid of 0..1 cells

    float edge = smoothstep(saturate(range) * compress(sin(TESR_GameTime.x * rate) * wave), 0, depth);
    under.a = 1 - edge;
    return under;
}

// One layer of rain rings from the ripple texture (x ring mask, yz ring normal, w per-drop time offset).
float3 ComputeRipple(sampler2D rippleMap, float2 uv, float time, float strength)
{
    float4 drop = tex2D(rippleMap, uv);
    drop.yz = expand(drop.yz);

    float age = frac(drop.w + time);
    float ringPhase = age - 1.0f + drop.x;
    float alive = saturate(0.2f + strength * 0.8f - age);
    float height = alive * drop.x * sin(clamp(ringPhase * 9.0f, 0.0f, 3.0f) * PI);
    return float3(drop.yz * height * 0.35f, 1.0f);
}

// Four ripple layers at different scales and speeds, by rain amount, faded out with distance, added to the wave normal.
float3 getRipples(PS_INPUT v, sampler2D rippleMap, float3 waveN, float distance, float rain){
    float closeness = 1 - saturate(invlerp(0, 3500, distance));
    if (!rain || !closeness) return waveN;

    float4 speeds = float4(0.96f, 0.97f, 0.98f, 0.99f) * 0.07;
    float2 uv = v.LTEXCOORD_7 * 5;
    float4 layerW = float4(1, 0.75, 0.5, 0.25) * rain;
    layerW = saturate(layerW * 4) * 2 * closeness;
    float3 a = ComputeRipple(rippleMap, uv + float2(0.25f, 0.0f), speeds.x * TESR_GameTime.x, layerW.x);
    float3 b = ComputeRipple(rippleMap, uv * 1.1 + float2(-0.55f, 0.3f), speeds.y * TESR_GameTime.x, layerW.y);
    float3 c = ComputeRipple(rippleMap, uv * 1.3 + float2(0.6f, 0.85f), speeds.z * TESR_GameTime.x, layerW.z);
    float3 d = ComputeRipple(rippleMap, uv * 1.5 + float2(0.5f, -0.75f), speeds.w * TESR_GameTime.x, layerW.w);

    float4 zParts = lerp(1, float4(a.z, b.z, c.z, d.z), layerW);
    float3 ringN = normalize(float3(layerW.x * a.xy + layerW.y * b.xy + layerW.z * c.xy + layerW.w * d.xy, zParts.x * zParts.y * zParts.z * zParts.w));
    return normalize(float3(ringN.xy + waveN.xy, waveN.z));
}
