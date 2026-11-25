// Sky-colour ambient for the lighting shaders (objects, parallax, terrain, skin, hair, grass,
// decals).
//
// Two models, selected at COMPILE time by [Shaders.PBR.Main] SkylightingMode. ps_3_0 flattens
// runtime branches, so a switch here would make every lit pixel pay for both paths; the macro
// comes through ShaderRecord::LoadShader the same way FORWARD_SHADOWS does, and changing the
// setting alters the preprocessed source so the shader cache recompiles.
//
//   0 -- spherical harmonic irradiance (default)
//   1 -- single directional sample, the older model
//
// Both return a GAMMA-ENCODED value. The object and terrain shaders carry no gamma conversion
// anywhere, so their ambient is gamma-encoded, and SKY.pso delinearises GetSkyColor before
// writing it. Keeping both modes in that space means switching between them compares the two
// MODELS rather than confounding the comparison with a colour-space difference.
//
// The encode is sqrt(), i.e. gamma 2.0, not the sRGB curve. One instruction per channel against
// a pow, and the few percent it differs from sRGB is far smaller than the error it replaces:
// integrating encoded values instead of encoding the result cost walls about a third of their
// brightness. Both modes use the same encode so the comparison stays honest.
#ifndef SKYAMBIENT_INCLUDED
#define SKYAMBIENT_INCLUDED

#ifndef SKYLIGHTING_MODE
    #define SKYLIGHTING_MODE 0
#endif

// UNOFFICIAL SharpSky: [Shaders.PBR.Metal] SharpSky in x, 0 = the plain sky. Both modes read it. Only mode 0 has the harmonics
// to sharpen (SkyAmbientSharp), so mode 1 keeps the plain sky. c136 is the one register between the PBR block (c135) and the
// sky (c137) that no shader uses.
float4 TESR_SharpSky : register(c136);

#if SKYLIGHTING_MODE == 1

// ---------------------------------------------------------------------------
// Mode 1: one sample of the sky, in a direction leaning from straight up toward the surface
// normal, scaled by a hand-written cosine form factor.
//
// Known shortcomings, kept deliberately so the two can be compared:
//   - lerp(up, N, d) is `up` for ANY d when N is up, so directionality does nothing at all on
//     floors and terrain -- they are pinned to the zenith colour.
//   - a point sample cannot represent a hemisphere: the cosine-weighted density peaks 45
//     degrees off the normal, so the horizon band is under-weighted.
//   - the horizon falloff has to be softened below SKY.pso's own 8, or sunInfluence -- which
//     only reaches the output through terms scaled by athmosphere -- vanishes at the lifted
//     sample direction.
// ---------------------------------------------------------------------------
float4 TESR_SkyColor     : register(c137);	// rgb sky at the zenith
float4 TESR_SkyLowColor  : register(c138);	// rgb lower sky
float4 TESR_HorizonColor : register(c139);	// rgb horizon band
float4 TESR_SunPosition  : register(c140);	// xyz direction to the sun
float4 TESR_SkyData      : register(c141);	// x haze thickness, y sun-side spread, z sun glow
float4 TESR_SunAmount    : register(c142);	// x day factor
float4 TESR_SunDiskColor : register(c143);	// rgb sun disc
float4 TESR_SunsetColor  : register(c144);	// rgb sunset tint

// Helpers first: Sky.hlsl uses linearize(), and several shaders include PBRScale (and so this
// file) before their own Helpers include.
#ifdef __INTELLISENSE__
    #include "Helpers.hlsl"	// editor only
    #include "Sky.hlsl"	// editor only
#else
    #include "includes/Helpers.hlsl"	// game: paths from the shader root
    #include "includes/Sky.hlsl"	// GetSunColor, GetSkyColor
#endif

#ifndef SKY_AMBIENT_ATMOSPHERE_POW
    #define SKY_AMBIENT_ATMOSPHERE_POW 2
#endif

// Mirrors SKY.pso's derivation with the sample direction substituted for its eye ray.
float3 GetSkyRadiance(float3 look) {
    float3 zenith = float3(0.0f, 0.0f, 1.0f);

    float height = pows(compress(dot(look, zenith)), 3);
    float sunUp = shade(TESR_SunPosition.xyz, zenith);
    float haze = pows(1 - height, SKY_AMBIENT_ATMOSPHERE_POW) * TESR_SkyData.x;
    float towardSun = pows(compress(dot(look, TESR_SunPosition.xyz)), 1 / TESR_SkyData.y);

    float3 disc = GetSunColor(sunUp, TESR_SkyData.x, TESR_SunAmount.x, TESR_SunDiskColor.rgb, TESR_SunsetColor.rgb);
    return GetSkyColor(height, haze, sunUp, towardSun, TESR_SkyData.z, TESR_SkyColor.rgb, TESR_SkyLowColor.rgb, TESR_HorizonColor.rgb, disc);
}

// lean: [Shaders.PBR.*] / [Shaders.Terrain.*] SkylightingDirectionality.
float3 SkyAmbientRadiance(float3 N, float lean) {
    float3 zenith = float3(0.0f, 0.0f, 1.0f);

    // Sample direction between straight up and the normal. For a downward normal with lean exactly 0.5 (every ceiling) the blend
    // is the zero vector, which normalize() would turn into NaN, so that case falls back to up.
    float3 mixed = lerp(zenith, N, lean);
    float mixedLen = length(mixed);
    float3 look = (mixedLen > 1e-4f) ? (mixed / mixedLen) : zenith;

    // how much of the sky the surface sees: 1 facing up, 0 facing down (cosine weighting)
    float seen = 0.5f * dot(N, zenith) + 0.5f;

    // the sky colour is linear; the callers want it encoded (sqrt)
    return sqrt(max(GetSkyRadiance(look), 0.0f)) * seen;
}

// UNOFFICIAL SharpSky: no harmonics to sharpen in this mode, so the plain sky stays.
float3 SkyAmbientSharpened(float3 dir, float3 plain) {
    return plain;
}

#else

// ---------------------------------------------------------------------------
// Mode 0: order-2 spherical harmonic irradiance.
//
// SkyShaders::UpdateConstants projects the sky -- the same GetSkyColor that SKY.pso renders the
// dome with -- onto 9 coefficients once per frame and convolves them with the clamped-cosine
// kernel, so this evaluates
//
//     E(N) = INTEGRAL L(w) max(N.w, 0) dw
//
// rather than approximating it by a sample direction. The cosine kernel is a severe low-pass
// filter, so 9 coefficients carry that integral to within about 1% (Ramamoorthi & Hanrahan
// 2001).
//
// The projection runs on LINEAR radiance, which is what the integral is defined on; the
// reconstruction below is encoded.
//
// The cosine form factor, the wall/floor split and the sun-side azimuthal bias are all inherent
// to the convolution. There is no direction to lean, so `directionality` is ignored here.
// ---------------------------------------------------------------------------
#ifdef SKY_AMBIENT_TERRAIN
float4 TESR_SkyIrradianceTerrain[9] : register(c137);
#define SKY_IRRADIANCE_SH TESR_SkyIrradianceTerrain
#else
float4 TESR_SkyIrradianceObjects[9] : register(c137);
#define SKY_IRRADIANCE_SH TESR_SkyIrradianceObjects
#endif

// N: the GEOMETRIC world normal, unit length; lean is unused in this mode.
float3 SkyAmbientRadiance(float3 N, float lean) {

    // Reconstruction is LINEAR irradiance; encode it. max() first because an order-2 SH fit can
    // ring slightly negative, and sqrt of a negative is NaN.
    float3 E = SKY_IRRADIANCE_SH[0].rgb
         + SKY_IRRADIANCE_SH[1].rgb * N.y
         + SKY_IRRADIANCE_SH[2].rgb * N.z
         + SKY_IRRADIANCE_SH[3].rgb * N.x
         + SKY_IRRADIANCE_SH[4].rgb * (N.x * N.y)
         + SKY_IRRADIANCE_SH[5].rgb * (N.y * N.z)
         + SKY_IRRADIANCE_SH[6].rgb * (3.0f * N.z * N.z - 1.0f)
         + SKY_IRRADIANCE_SH[7].rgb * (N.x * N.z)
         + SKY_IRRADIANCE_SH[8].rgb * (N.x * N.x - N.y * N.y);
    return sqrt(max(E, 0.0f));
}

// UNOFFICIAL SharpSky: the same nine coefficients evaluated as the sky's radiance along dir, not its smooth irradiance. Sky.cpp
// stores band l at (A_l / pi) of its projection (A0/pi = 1, A1/pi = 2/3, A2/pi = 1/4: the clamped cosine's bands), so undoing
// the convolution weights bands 1, 1.5 and 4 (band 0 stays 1). Order 2 rings: a plain sky reads about a quarter brighter in
// linear light at the zenith and dips below zero under the horizon, so the sum is clamped here.
float3 SkyAmbientSharp(float3 dir) {
    float3 radiance = SKY_IRRADIANCE_SH[0].rgb
         + 1.5f * (SKY_IRRADIANCE_SH[1].rgb * dir.y + SKY_IRRADIANCE_SH[2].rgb * dir.z + SKY_IRRADIANCE_SH[3].rgb * dir.x)
         + 4.0f * (SKY_IRRADIANCE_SH[4].rgb * (dir.x * dir.y) + SKY_IRRADIANCE_SH[5].rgb * (dir.y * dir.z)
                 + SKY_IRRADIANCE_SH[6].rgb * (3.0f * dir.z * dir.z - 1.0f) + SKY_IRRADIANCE_SH[7].rgb * (dir.x * dir.z)
                 + SKY_IRRADIANCE_SH[8].rgb * (dir.x * dir.x - dir.y * dir.y));
    return sqrt(max(radiance, 0.0f));
}

// UNOFFICIAL SharpSky: plain (the caller's encoded sky along dir) blended to the sharp one by TESR_SharpSky.x. Below the horizon
// the sharp value is kept no lower than the plain one, more so the further dir turns down: the sky is zero there, and the plain
// value is what the object reflected before.
float3 SkyAmbientSharpened(float3 dir, float3 plain) {
    float3 sharp = SkyAmbientSharp(dir);
    sharp = lerp(sharp, max(sharp, plain), saturate(-dir.z));
    return lerp(plain, sharp, TESR_SharpSky.x);
}

#endif

#endif
