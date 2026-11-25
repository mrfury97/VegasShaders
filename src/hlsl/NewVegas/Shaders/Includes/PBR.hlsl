// PBR lighting terms used by the object, skin, parallax and water shaders.
#ifdef __INTELLISENSE__
    #include "Helpers.hlsl"
#endif

// Specular anti-aliasing (geometric variance). Reference: Tokuyoshi and Kaplanyan, "Improved
// Geometric Specular Anti-Aliasing" (2019), and the jcgt 2021 paper of the same idea.
//
// Raises roughness where the normal changes quickly across one pixel's screen footprint. A high-frequency
// normal map (hair is the sharpest case) otherwise gives a GGX peak that scales about 1/roughness^4, so two
// neighbouring texels with glossiness 0.85 and 0.95 differ by roughly 80x in peak brightness. That is texel
// noise the material never meant to show. In ObjectTemplate.hlsl the same noise drives the HAIR (ONLY_SPECULAR)
// pass's alpha-blend weight, which appears as a blotchy alpha pattern rather than a shimmer in the highlight.
float SpecularAA(float3 n, float r) {
    const float invTwoPi = 0.15915494;
    const float widenCap = 0.18;
    float3 gradU = ddx(n);
    float3 gradV = ddy(n);
    float spread = invTwoPi * (dot(gradU, gradU) + dot(gradV, gradV));
    float kernelRough = min(widenCap, spread);
    return sqrt(saturate(r * r + kernelRough));
}

// UNOFFICIAL 2026-10-16 EnergySplit ([Shaders.PBR.Main], PBR.cpp; live). c223: clear of every other pixel shader constant
// NVR sets. Zero (not set) = off.
float4 TESR_PBRMath : register(c223); // x: EnergySplit (1 = on), y: Reflectivity - 1 (below), z/w: Object.hlsl RmaosRead

// UNOFFICIAL 2026-10-25 Reflectivity ([Shaders.PBR.Main] ReflectivityExterior / ReflectivityInterior, PBR.cpp; live): the share of
// light a surface reflects head-on (F0) and at grazing angles (F90) times the setting, so highlights from the sun and lamps follow
// it. TESR_PBRMath.y carries Reflectivity - 1: 0 (also a register never written) is the old picture exactly.
// UNOFFICIAL 2026-11-02: how much light a non-metal reflects head-on (F0). 0.04 unless the includer sets it first (Object.hlsl:
// the _rmaos map's specular level, its alpha).
#ifndef PBR_DIELECTRIC_F0
    #define PBR_DIELECTRIC_F0 0.04f
#endif
float3 ReflectivityF0(float3 f0) { return saturate(f0 + f0 * TESR_PBRMath.y); }
#define REFLECTIVITY_F90 saturate(1.0f + TESR_PBRMath.y).xxx
// UNOFFICIAL 2026-11-19 (Finding 4.2): opt-in roughness-damped grazing F90 (Lagarde 2014, matching RtPathTrace.comp PTRoughFresnel = 1)
// for deferred light passes; forward templates leave PBR_ROUGH_F90 undefined so their bytecode and ps_3_0 register counts stay identical.
#if defined(PBR_ROUGH_F90) && PBR_ROUGH_F90
float3 RoughReflectivityF90(float3 f0, float r, float metal) {
    return max(REFLECTIVITY_F90 * (1.0f - saturate(r) * saturate(1.0f - metal)), f0);
}
    #define PBR_F90_FOR(f0, r, metal) RoughReflectivityF90((f0), (r), (metal))
#else
    #define PBR_F90_FOR(f0, r, metal) REFLECTIVITY_F90
#endif
// UNOFFICIAL 2026-11-05 VanillaBridge (Object.hlsl): the Fresnel of the HIGHLIGHT, which may start from a different reflectance
// than the diffuse's (1 - F) share: an includer that defines PBR_SPEC_F0(baseColor, metal) gives the highlight that F0 (the
// authored Blinn-Phong highlight's strength); the diffuse keeps the dielectric's, so a surface's diffuse light is unchanged.
// Without it the highlight takes the same Fresnel as before (the macro is that value itself: the shaders are unchanged).
#ifdef PBR_SPEC_F0
    #define PBR_SPEC_FRESNEL(fres, baseColor, r, metal, cosT) Fresnel(ReflectivityF0(PBR_SPEC_F0(baseColor, metal)), PBR_F90_FOR(ReflectivityF0(PBR_SPEC_F0(baseColor, metal)), (r), (metal)), cosT)
#else
    #define PBR_SPEC_FRESNEL(fres, baseColor, r, metal, cosT) (fres)
#endif

// UNOFFICIAL 2026-10-18 SpecularFade ([Shaders.PBR.Main], restart: SPEC_FADE_ON from ShaderRecord::BuildDefines; the object templates
// define PBR_SPEC_FADE in their highlight-drawing pixel shaders, so terrain, skin and water keep the old code; parallax since 2026-10-25). The game stops
// drawing an object's highlights where the object is fSpecularLODStartFade + fSpecularLODRange from the camera (2000 + 300 units by
// default): there it swaps the object's shaders for ones without a highlight (and, for shiny objects with 3-5 lamps, its whole set of
// passes). The game's own shaders faded the highlight out before that point (BSShaderPPLightingProperty::GetSpecularLODFade, 0x00B66B80,
// 1 near -> 0 at the end; sent in the sun's direction .w and in the lamps' colours of the highlight passes); NVR's PBR shaders ignored
// it, so at that distance the highlight vanished at once and the diffuse light jumped from the shiny form ((1 - F) x keep) to the plain
// one: the "PBR pop-in" as the camera nears or leaves an object. NVR_SpecFade.x is that same fade for the object being drawn (set per
// draw, core/PbrPop.h); the highlight is scaled by it and the diffuse light slides to the plain form, so at the swap the shiny shader
// gives exactly what the plain one does. 1 (near, or the switch off) leaves every result exactly as before (x * 1, x + y * 0).
#if defined(PBR_SPEC_FADE) || defined(NVR_FOLD) || (defined(CAPTURED_NORMALS_ON) && CAPTURED_NORMALS_ON >= 2)
float4 NVR_SpecFade : register(c211); // x: the draw's highlight fade (1 = full); pinned clear of every other object-shader constant
                                      // y (UNOFFICIAL 2026-10-21 FoldHighlights, ObjectTemplate.hlsl): 1 = fold this draw; z: 1 = with the vertex colour
                                      // w (UNOFFICIAL 2026-10-31 MaterialBuffer, Includes/CapturedNormals.hlsl): class + 8 x floor(bound radius)
#endif

// EnergySplit. The full pass and the sun pass give the diffuse part (1 - F(L.H)) of the light. For a white surface that adds up to
// MORE light than the surface receives: up to +8.9% on smooth surfaces seen at a low angle (roughness 0.07-0.45, N.V 0.06-0.58;
// white furnace, tests\furnace). This is that measured excess as a function of roughness and N.V (fitted to the furnace tables to
// +-0.4%, bench\foundations\REPORT.md) and the diffuse part keeps the rest: surfaces that did not gain keep exactly their light.
// It depends on the view and the roughness only, so the shader works it out once per pixel (SetDiffuseEnergyKeep, before its
// lighting calls) and every lamp reuses it; shaders that do not call it keep the old split (1). 0.95: the diffuse share there.
#ifndef PBR_SMITH_CORRELATED
    #define PBR_SMITH_CORRELATED 0
#endif
#if PBR_SMITH_CORRELATED
// UNOFFICIAL 2026-10-18 SmithCorrelated (see BRDF below; idea from Physically-Based-Reloaded (Balebandro), GPL-3.0, commit 4e64984b).
// With the exact masking term the highlight keeps its energy at grazing angles, and the (1 - F(L.H)) diffuse, which does not give
// that energy up, made a white dielectric send back up to 1.65x the light it receives (full pass, roughness 0.07, N.V 0.07; the
// lossy Karis term had kept the old total at 1.08). OpenPBR, which their fork follows, scales the diffuse by (1 - the highlight's
// albedo for the view), read from its split-sum table; NVR has no free sampler in the object shaders, so this is the share of the
// diffuse that leaves the total at 1, measured on the real PBR.hlsl (white furnace, tests\furnace\hckeep_table.cpp) and fitted
// (bench\deep-roadmap\evidence\pbrport\fit_keep2.py: 2D polynomial in roughness and sqrt(N.V), GGX's clamp below roughness 0.056
// modelled as its loss 1 - (1 - min(1e5 r^4, 1))^2). Furnace with it: 0.972-1.019 for roughness 0.05-1, N.V 0.05-1 (was up to
// 1.652). Always on with SmithCorrelated: it is that term's energy balance, not the EnergySplit switch.
float SmithCorrelatedKeep(float NdotV, float roughness) {
    const float rc = max(roughness, 0.056f);
    const float sq = sqrt(clamp(NdotV, 0.02f, 1.0f));
    const float q0 = 0.8720785f + sq * (2.377077f + sq * (-15.78768f + sq * (20.7581f + sq * (-8.469884f + sq * (0.2183369f)))));
    const float q1 = -2.350215f + sq * (6.164611f + sq * (12.53951f + sq * (-24.69559f + sq * (9.321004f))));
    const float q2 = -4.584592f + sq * (-17.91134f + sq * (14.6047f + sq * (0.8975321f)));
    const float q3 = 20.67309f + sq * (6.171803f + sq * (-8.573712f));
    const float q4 = -22.05094f + sq * (2.260031f);
    const float q5 = 7.527356f;
    const float surplus = q0 + rc * (q1 + rc * (q2 + rc * (q3 + rc * (q4 + rc * q5))));
    const float lossClamp = 1.0f - min(1e5f * roughness * roughness * roughness * roughness, 1.0f);
    return 1.0f - saturate(surplus * (1.0f - lossClamp * lossClamp));
}
#endif

float DiffuseEnergyKeep(float NdotV, float roughness) {
#if PBR_SMITH_CORRELATED == 2
    // TEST mode ([Shaders.PBR.Main] SmithCorrelated = 2, for one-launch A/B tests only): blended in by SmithCorrelatedTest (TESR_PBRMath.x,
    // PBR.cpp; EnergySplit, which uses that lane otherwise, is not compiled in here). 0 = the old split exactly (EnergySplit off: 1).
    return 1.0f + (SmithCorrelatedKeep(NdotV, roughness) - 1.0f) * TESR_PBRMath.x;
#elif PBR_SMITH_CORRELATED
    return SmithCorrelatedKeep(NdotV, roughness);
#else
    const float tv = saturate((NdotV - 0.055f) * (1.0f / 0.525f));
    const float rx = saturate((0.53f - roughness) * (1.0f / 0.461f));
    const float lowRough = saturate((roughness - 0.02f) * (1.0f / 0.05f));
    const float inv = 1.0f - tv;
    const float bump = rx * lowRough * tv * inv * inv * ((0.10380582f + 0.69366243f * rx) + tv * (0.27762281f - 0.88561283f * rx));
    return TESR_PBRMath.x > 0.5f ? 1.0f - bump * (1.0f / 0.95f) : 1.0f;
#endif
}
static float PBRDiffuseKeep = 1.0f;
void SetDiffuseEnergyKeep(float3 normal, float3 eyeDir, float roughness) {
    PBRDiffuseKeep = DiffuseEnergyKeep(max(shades(normalize(normal), normalize(eyeDir)), 0.00001), roughness);
}

// Fresnel, Schlick approximation.
// UNOFFICIAL 2026-11-19: exact multiplies instead of pow(1 - cosT, 5) (saves log2+mul+exp2 and avoids pow(0,5) NaN).
float3 Fresnel(float3 f0, float3 f90, float cosT) {
    float3 span = f90 - f0;
    float omc = 1.0f - cosT;
    float omc2 = omc * omc;
    float falloff = omc2 * omc2 * omc;
    return f0 + span * falloff;
}

// Split-sum environment response (scale and bias on F0) for a roughness and N.V, after Karis, "Physically Based Shading on
// Mobile" (2014). Used for metal reflections (Object.hlsl getMetalAmbient) and the skin's sky reflection (Skin.hlsl). UNOFFICIAL
// 2026-10-25: moved here from Object.hlsl unchanged, so the skin shader can share it.
float3 EnvBRDFApprox(float3 f0, float roughness, float NdotV) {
    const float4 k0 = float4(-1.0f, -0.0275f, -0.572f, 0.022f);
    const float4 k1 = float4(1.0f, 0.0425f, 1.04f, -0.04f);
    float4 mixed = roughness * k0 + k1;
    float bias0 = min(mixed.x * mixed.x, exp2(-9.28f * NdotV)) * mixed.x + mixed.y;
    float2 scaleBias = float2(-1.04f, 1.04f) * bias0 + mixed.zw;
    return f0 * scaleBias.x + scaleBias.y;
}

// Diffuse: Lambert.
float3 LambertianDiffuse(float3 diffColor, float3 F) {
    float3 notReflected = 1 - F;
    return notReflected * diffColor / PI;
}

// Disney diffuse (Burley 2012).
float3 DisneyDiffuse(float3 diffColor, float r, float nv, float nl, float lh) {
    const float rough2 = r * r;

    const float bias = lerp (0, 0.5 , rough2);
    const float factor = lerp (1.0, 1.0 / 1.51, rough2);
    const float fd90 = bias + 2.0 * lh * lh * rough2;
    const float3 unit = float(1.0).xxx;
    const float lightScatter = Fresnel(unit, fd90, nl).r;
    const float viewScatter = Fresnel(unit, fd90, nv).r;

    return (diffColor / PI) * lightScatter * viewScatter * factor;
}

// Specular. D: GGX normal distribution (times PI helper cancels /PI * PI in PBR* callers).
// UNOFFICIAL 2026-11-19: exact multiplies instead of pow(r, 4) and pow(r + 1, 2); exact nv*nl cancellation in BRDF.
float GGXPi(float nh, float r) {
    float r2 = r * r;
    float aSq = r2 * r2;
    float dTerm = max((nh * aSq - nh) * nh + 1.0f, 1e-5f);
    return aSq / (dTerm * dTerm);
}

float GGX(float nh, float r) {
    return GGXPi(nh, r) / PI;
}

// G1: Schlick-Beckmann.
float ShlickBeckmann(float nx, float r) {
    float rp1 = r + 1.0f;
    float kk = (rp1 * rp1) * 0.125f;
    return nx / max(nx * (1.0f - kk) + kk, 0.00000001f);
}

// Smith masking, separable.
float GeometryShadowing(float r, float nv, float nl) {
    return ShlickBeckmann(nv, r) * ShlickBeckmann(nl, r);
}

// F: Schlick.
float3 FresnelShlick(float3 R0, float3 H, float3 V) {
    float3 invRefl = 1.0f - R0;
    float omc = 1.0f - shades(H, V);
    float omc2 = omc * omc;
    return R0 + invRefl * (omc2 * omc2 * omc);
}

// UNOFFICIAL 2026-10-18 SmithCorrelated ([Shaders.PBR.Main], restart: SMITH_CORRELATED_ON from ShaderRecord::BuildDefines; the object
// templates map it to PBR_SMITH_CORRELATED, so terrain, skin and water keep the old term). The idea is from Physically-Based-Reloaded
// (Balebandro), GPL-3.0, commit 4e64984b 'OpenPBR: exact split-sum table and height-correlated Smith masking'; the code is NVR's.
// The masking-shadowing term above is Karis 2013 ('Real Shading in UE4'): separable Schlick-Smith with k = (r + 1)^2 / 8, a remap
// for analytic lights that keeps k >= 0.125 even for a mirror, so smooth surfaces seen or lit at a low angle lose most of their
// highlight (white furnace, F0 = 1: 0.18 of the light at roughness 0.2, N.V 0.1, where the microfacet model keeps 0.93). This is the
// exact height-correlated Smith term for GGX instead (Heitz 2014, 'Understanding the Masking-Shadowing Function in Microfacet-Based
// BRDFs', eq. 99), written as the visibility V = G2 / (4 N.V N.L) (Hammon 2017; Filament):
//   V = 0.5 / (N.L sqrt(N.V^2 (1 - a^2) + a^2) + N.V sqrt(N.L^2 (1 - a^2) + a^2)),  a = roughness^2 (GGX's alpha, as GGX() above).
// Face-on (N.V = N.L = 1) both give 1; off-normal highlights are brighter (about 1.45x at 45 degrees for roughness 0.3-0.65).
// A look change: off by default. Off, BRDF is exactly the old code (the preprocessor removes this). SmithCorrelated = 2 is a TEST mode:
// both terms, blended by SmithCorrelatedTest (TESR_PBRMath.x), so one launch can show off (0, the old picture exactly) and on (1).
// BRDF * PI (avoids dividing by PI then multiplying by PI in PBR* callers).
float3 BRDFPi(float r, float3 F, float nv, float nl, float nh) {
#if PBR_SMITH_CORRELATED
    // The callers clamp N.V and N.L to 1e-5 and r >= 0.02, so the sum below is >= 8e-9: never a division by zero.
    const float r2 = r * r;
    const float aSq = r2 * r2;
    const float visV = nl * sqrt(nv * nv * (1.0f - aSq) + aSq);
    const float visL = nv * sqrt(nl * nl * (1.0f - aSq) + aSq);
    const float dPi = GGXPi(nh, r);
    #if PBR_SMITH_CORRELATED == 2
        const float rp1 = r + 1.0f;
        const float kk = (rp1 * rp1) * 0.125f;
        const float invK = 1.0f - kk;
        const float3 legacy = dPi * (0.25f / ((nv * invK + kk) * (nl * invK + kk))) * F;
        return legacy + (dPi * (0.5f / (visV + visL)) * F - legacy) * TESR_PBRMath.x;
    #else
        return dPi * (0.5f / (visV + visL)) * F;
    #endif
#else
    const float rp1 = r + 1.0f;
    const float kk = (rp1 * rp1) * 0.125f;
    const float invK = 1.0f - kk;
    const float vis = 0.25f / ((nv * invK + kk) * (nl * invK + kk));
    return GGXPi(nh, r) * vis * F;
#endif
}

float3 BRDF(float r, float3 F, float nv, float nl, float nh) {
    return BRDFPi(r, F, nv, nl, nh) / PI;
}

// Plain diffuse term for the light: no highlight, Lambert only.
float3 PBRDiffuse(float metal, float r, float3 baseColor, float3 N, float3 V, float3 L, float3 radiance) {
    float3 nrm = normalize(N);
    float3 toLight = normalize(L);

    const float NdotL = shades(nrm, toLight);

    // No Fresnel here. These permutations draw no specular lobe, so energy removed from the diffuse
    // has nowhere to go; PBRSun returns it as spec * NdotS, while this path just drops it.
    // Keeping (1 - LdotH)^5 would send the surface black as V nears -L, where
    // normalize(V + L) is singular. Leaving the term out removes the last view
    // dependence, which is what a purely Lambertian material should have.
    return (1.0f - metal) * baseColor * (NdotL * radiance);
}

float3 PBRSpecular(float metal, float r, float3 baseColor, float3 N, float3 V, float3 L, float3 radiance) {
    const float3 f0Base = ReflectivityF0(lerp(PBR_DIELECTRIC_F0, baseColor, metal));

    float3 nrm = normalize(N);
    float3 viewV = normalize(V);
    float3 toLight = normalize(L);

    const float3 halfV = normalize(viewV + toLight);
    const float NdotL = max(shades(nrm, toLight), 0.00001);
    const float NdotV = max(shades(nrm, viewV), 0.00001);
    const float NdotH = shades(nrm, halfV);
    const float LdotH = shades(toLight, halfV);

    const float3 fres = Fresnel(f0Base, PBR_F90_FOR(f0Base, r, metal), LdotH);

    float3 specOut = BRDFPi(r, PBR_SPEC_FRESNEL(fres, baseColor, r, metal, LdotH), NdotV, NdotL, NdotH);
#ifdef PBR_SPEC_FADE
    specOut *= NVR_SpecFade.x;	// UNOFFICIAL SpecularFade (above)
#endif

    return specOut * (NdotL * radiance);
}

// Full PBR: Lambert diffuse plus GGX specular for one light.
float3 PBR(float metal, float r, float3 baseColor, float3 N, float3 V, float3 L, float3 radiance) {
    const float3 f0Base = ReflectivityF0(lerp(PBR_DIELECTRIC_F0, baseColor, metal));

    float3 nrm = normalize(N);
    float3 viewV = normalize(V);
    float3 toLight = normalize(L);

    const float3 halfV = normalize(viewV + toLight);
    const float NdotL = max(shades(nrm, toLight), 0.00001);
    const float NdotV = max(shades(nrm, viewV), 0.00001);
    const float NdotH = shades(nrm, halfV);
    const float LdotH = shades(toLight, halfV);

    const float3 fres = Fresnel(f0Base, PBR_F90_FOR(f0Base, r, metal), LdotH);

    float3 diff = (1.0f - metal) * (1.0f - fres) * baseColor * PBRDiffuseKeep;

    float3 specOut = BRDFPi(r, PBR_SPEC_FRESNEL(fres, baseColor, r, metal, LdotH), NdotV, NdotL, NdotH);
#ifdef PBR_SPEC_FADE
    // UNOFFICIAL SpecularFade (above): the highlight out, the diffuse light towards PBRDiffuse's plain form.
    #ifndef PBR_SPEC_FADE_HIGHLIGHT_ONLY
    diff += ((1.0f - metal) * baseColor - diff) * (1.0f - NVR_SpecFade.x);
    #endif
    specOut *= NVR_SpecFade.x;
#endif

    return (diff + specOut) * (NdotL * radiance);
}

#define SUN_RADIUS 0.00918043

// Specular for the sun, which is a disc: the light direction is moved to the closest point on the disc.
float3 PBRSunSpecular(float metal, float r, float3 baseColor, float3 N, float3 V, float3 L, float3 radiance) {
    const float3 f0Base = ReflectivityF0(lerp(PBR_DIELECTRIC_F0, baseColor, metal));

    float3 nrm = normalize(N);
    float3 viewV = normalize(V);
    float3 toLight = normalize(L);

    const float3 mirrorDir = reflect(toLight, nrm);

    const float discSin = sin(SUN_RADIUS);
    const float discCos = cos(SUN_RADIUS);

    const float3 LdotR = dot(toLight, mirrorDir);
    const float3 offAxis = mirrorDir - LdotR * toLight;
    // UNOFFICIAL 2026-10-16: closestPoint is zero when the sun is exactly along the N; normalize() of it was NaN. Elsewhere
    // the same normalize() as before, so the result is bit for bit the old one.
    const float3 offAxisDir = dot(offAxis, offAxis) > 1e-12f ? normalize(offAxis) : 0.0f;
    const float3 sunDirV = LdotR < discCos ? normalize(discCos * toLight + offAxisDir * discSin) : mirrorDir;

    const float3 halfV = normalize(viewV + sunDirV);
    const float NdotS = max(shades(nrm, sunDirV), 0.00001);
    const float NdotV = max(shades(nrm, viewV), 0.00001);
    const float NdotH = shades(nrm, halfV);
    const float NdotL = shades(nrm, toLight);
    const float LdotH = shades(toLight, halfV);

    const float3 fres = Fresnel(f0Base, PBR_F90_FOR(f0Base, r, metal), LdotH);

    float3 specOut = BRDFPi(r, PBR_SPEC_FRESNEL(fres, baseColor, r, metal, LdotH), NdotV, NdotS, NdotH);
#ifdef PBR_SPEC_FADE
    specOut *= NVR_SpecFade.x;	// UNOFFICIAL SpecularFade (above)
#endif

    return specOut * (NdotS * radiance);
}

// Sun: diffuse plus the disc specular, in one call.
float3 PBRSun(float metal, float r, float3 baseColor, float3 N, float3 V, float3 L, float3 radiance) {
    const float3 f0Base = ReflectivityF0(lerp(PBR_DIELECTRIC_F0, baseColor, metal));

    float3 nrm = normalize(N);
    float3 viewV = normalize(V);
    float3 toLight = normalize(L);

    const float3 mirrorDir = reflect(toLight, nrm);

    const float discSin = sin(SUN_RADIUS);
    const float discCos = cos(SUN_RADIUS);

    const float3 LdotR = dot(toLight, mirrorDir);
    const float3 offAxis = mirrorDir - LdotR * toLight;
    // UNOFFICIAL 2026-10-16: closestPoint is zero when the sun is exactly along the N; normalize() of it was NaN. Elsewhere
    // the same normalize() as before, so the result is bit for bit the old one.
    const float3 offAxisDir = dot(offAxis, offAxis) > 1e-12f ? normalize(offAxis) : 0.0f;
    const float3 sunDirV = LdotR < discCos ? normalize(discCos * toLight + offAxisDir * discSin) : mirrorDir;

    const float3 halfV = normalize(viewV + sunDirV);
    const float NdotS = max(shades(nrm, sunDirV), 0.00001);
    const float NdotV = max(shades(nrm, viewV), 0.00001);
    const float NdotH = shades(nrm, halfV);
    const float NdotL = shades(nrm, toLight);
    const float LdotH = shades(toLight, halfV);

    const float3 fres = Fresnel(f0Base, PBR_F90_FOR(f0Base, r, metal), LdotH);

    float3 diff = (1.0f - metal) * (1.0f - fres) * baseColor * PBRDiffuseKeep;

    float3 specOut = BRDFPi(r, PBR_SPEC_FRESNEL(fres, baseColor, r, metal, LdotH), NdotV, NdotS, NdotH);
#ifdef PBR_SPEC_FADE
    // UNOFFICIAL SpecularFade (above): the highlight out, the diffuse light towards PBRDiffuse's plain form.
    #ifndef PBR_SPEC_FADE_HIGHLIGHT_ONLY
    diff += ((1.0f - metal) * baseColor - diff) * (1.0f - NVR_SpecFade.x);
    #endif
    specOut *= NVR_SpecFade.x;
#endif

    return (diff * NdotL + specOut * NdotS) * radiance;
}
