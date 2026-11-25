// Skin lighting for SkinTemplate.hlsl. UNOFFICIAL 2026-11-05: the lighting of Physically Based NVR's skin shader
// (github.com/Balebandro/Physically-Based-Reloaded, Includes/SkinLighting.hlsl), on this build's stack: its BRDF terms
// (PBR.hlsl), light and ambient strengths (PBRScale.hlsl), sky (SkyAmbient.hlsl), linear lighting (LinearLight.hlsl) and lamp
// falloff (LampFalloff.hlsl), and the sun's and the lamps' shadows from the template. The 2026-10-25 rewrite's own model
// (refitted diffusion, signed curvature, bounded shadow edges, the sheen, the self-shadow look toward the sun, Jimenez's
// transmittance) is in VegasShaders-backups/skin_before_pb_look_20261105.
//   - Spherical-Gaussian skin diffusion (as in The Order: 1886, Neubelt & Pettineo): each colour's N.L blurred by a spherical
//     Gaussian whose width grows with the surface's curvature, with Stephen Hill's fitted SG irradiance (MJP, "SG Series"
//     part 3). The widths are fitted to Penner's pre-integrated skin (2011) for d'Eon's measured diffusion profile; no lookup
//     texture. Curvature from screen-space derivatives of the interpolated normal.
//   - Per-colour normal softening for the small-scale detail the curvature cannot see: red the blurriest normal, blue the
//     sharpest.
//   - Pre-integrated shadows: light bleeds into the penumbra, red farthest.
//   - Transmission (Barre-Brisebois & Bouchard 2011): red light through ears, nostrils and fingers against the light, the
//     curvature standing in for the thickness.
//   - Dual-lobe GGX specular (Jimenez / Unreal dual specular) at skin's F0 of 0.028, the sun as a disc (Karis), and
//     VanillaMatchedHighlights.
//   - The sky light, softened per colour like the direct light, and a dual-lobe sky reflection balanced against it.
// Not carried over from PB NVR: its lamp bounce (TESR_LampLook) and merged lamps (MergedLights.hlsl), which this build lacks.
//
// Colour space: everything here works in the build's lighting space (the game's gamma, or linear with [Shaders.PBR.Linear]):
// lights come in through PBRLight / PBRAmbient, the albedo through LinColor, physical colours through SkinLinearColor. Direct
// lighting works in whatever space the caller's vectors share (SkinTemplate: tangent space); the sky terms take WORLD-space
// vectors. Derivatives are taken in SkinSetup only, which must be called at the pixel shader's top level.
#ifndef SKIN_INCLUDED
#define SKIN_INCLUDED

#if defined(__INTELLISENSE__)
    #include "PBR.hlsl"
    #include "PBRScale.hlsl"
#elif !defined(SKIN_IN_SCREEN_PASS)	// UNOFFICIAL 2026-11-15 DeferredSkin (Includes/SkinDeferred.hlsl): the screen passes have PBR.hlsl already (no include guard)
    #include "includes/PBR.hlsl"
    #include "includes/PBRScale.hlsl"
#endif

// --- constants (effects/Skin.cpp). c192-c195 are free in every skin pixel shader: Shadow.hlsl pins c100-c133, the sky
// c134-c145, the sun cross-fade c146-c166, contact hardening c167, the lamp shadows c174-c190, linear lighting c191.
float4 TESR_SkinMaterial : register(c192); // x SpecularStrength, y Roughness, z Scattering (how far light spreads, x the measured profile), w Translucency
float4 TESR_SkinLook     : register(c193); // x SkyReflection (0 indoors), y wetness (rain x RainWetness), z ShadowScatter, w VanillaMatchedHighlights
// Per draw, set by the SkinShader hook (Hooks/Shaders.cpp) after the shader binds (so NVR_, not TESR_: ShaderRecord re-uploads
// every TESR_ constant when the shader changes): x 1 while the screen-space blur's targets are bound for this draw (the blur
// then does the diffusion: doing both doubled it), y 1 while the FaceGen maps are bound to a light-only pass.
#ifndef SKIN_IN_SCREEN_PASS	// (DeferredSkin: c194 is Object.hlsl's TESR_MicroShadow in the screen passes, which need no per-draw value)
float4 NVR_SkinDraw      : register(c194);
#endif
// x [Shaders.Skin.Debug] DebugView 1-7 (0 off, or one of the blur's views), y 1 while the Interiors settings are in use,
// z 1 = LegacyFaceShadows (the template's shadow reach; see SHADOW_REACH_ALONG_RAY). Declared by the template ahead of
// Shadow.hlsl when it needs it there (SKIN_DEBUG_DECLARED).
#ifndef SKIN_DEBUG_DECLARED
float4 TESR_SkinDebug    : register(c195);
#endif

#define SKIN_F0 0.028f
#define SKIN_LOBE0 0.75f            // the sharp lobe's roughness, x Roughness
#define SKIN_LOBE1 1.3f             // the broad lobe's
#define SKIN_LOBE_MIX 0.15f         // the broad lobe's share
#define SKIN_TRANSMIT_TINT float3(1.0f, 0.28f, 0.12f)   // LINEAR colour of light through flesh
#define SKIN_TRANSMIT_DISTORTION 0.3f
#define SKIN_TRANSMIT_POWER 4.0f
#define SKIN_PENUMBRA_TINT float3(0.3f, 0.06f, 0.04f)   // light bled into a shadow edge
#define SKIN_SUN_RADIUS 0.00918043  // the sun's disc, angular radius in radians (PB NVR's PBR.hlsl SUN_RADIUS)
// Curvature in 1/mm (one game unit is 1/70 m), capped at 1 (a 1 mm radius), where the diffusion fit was made. Scattering 1
// widens d'Eon's profile 2x, which reads right at game viewing distances (the measured width barely shows on a head).
#define SKIN_MM_PER_UNIT 14.2857f
#define SKIN_SCATTER_BASE 2.0f
#define SKIN_SOFT_NORMAL_BIAS 2.0f  // mip levels the normal map is read blurred by for the per-colour softening

// Per pixel, from SkinSetup.
static float skinRoughness = 0.5f;
static float skinSpecScale = 0.0f;     // the highlights' strength: the specular mask x SpecularStrength (x wetness)
static float skinCurvature = 0.0f;     // 1/mm, after Scattering: 0 flat (or the blur's), 1 a 1 mm radius
static float skinThinness = 0.0f;      // the same curvature without Scattering, for the transmission
static float skinSkyWeight = 0.0f;     // the share of the sky the reflection takes (SkinSkyReflection), which the ambient leaves out

// A LINEAR colour (a physical constant) in the build's lighting space: encoded as the sky's colours are (sqrt), then decoded
// with the lights (LinColor returns it as is without linear lighting).
float3 SkinLinearColor(float3 c) { return LinColor(sqrt(max(c, 0.0f))); }

// Call once per pixel at the TOP LEVEL of the pixel shader (derivatives).
//   normalTS:     the bumped normal, tangent space (for the specular anti-aliasing)
//   worldNormal:  the interpolated vertex normal in world space (for the curvature)
//   worldPos:     the camera-relative world position
//   mask:         the normal map's alpha, the specular mask (0.5 reads as full skin)
//   blurTakes:    1 while the screen-space blur takes this draw (NVR_SkinDraw.x): the curvature diffusion then steps aside
void SkinSetup(float3 normalTS, float3 worldNormal, float3 worldPos, float mask, float blurTakes) {
    const float wet = saturate(TESR_SkinLook.y);
    const float roughness = lerp(TESR_SkinMaterial.y, TESR_SkinMaterial.y * 0.55f, wet);   // a water film is smoother than skin
    skinRoughness = clamp(SpecularAA(normalTS, roughness), 0.08f, 1.0f);
    skinSpecScale = mask * 2.0f * TESR_SkinMaterial.x * (1.0f + wet);   // SpecularStrength 1 reads a 50% mask as real skin at F0 0.028

    // How fast the normal turns per millimetre of surface (Penner).
    const float dN = length(fwidth(worldNormal));
    const float dP = length(fwidth(worldPos)) * SKIN_MM_PER_UNIT;
    const float curvature = dN / max(dP, 1e-4f);
    skinThinness = saturate(curvature * SKIN_SCATTER_BASE);
    skinCurvature = blurTakes > 0.0f ? 0.0f : saturate(curvature * SKIN_SCATTER_BASE * TESR_SkinMaterial.z);
}

// VanillaMatchedHighlights: vanilla lit a face's specular mask with its own Blinn-Phong pass, peaking at mask x the light. The two
// GGX lobes at skin's F0 peak at F0 / (4 alpha^2) each; this scales the direct highlight so their mixed peak reaches the mask,
// keeping the PBR shape. Matched at the Roughness setting, before rain smooths it and before SpecularAA. 1x at the default 0.45,
// up to the 12x cap for very rough settings. From the constants alone, worked out where it is used: held in a register across
// the shader it took the many-lamp variants past ps_3_0's 32 temporaries.
float SkinVanillaMatch() {
    const float alpha0 = pow(clamp(TESR_SkinMaterial.y * SKIN_LOBE0, 0.04f, 1.0f), 2.0f);
    const float alpha1 = pow(clamp(TESR_SkinMaterial.y * SKIN_LOBE1, 0.04f, 1.0f), 2.0f);
    const float peak = lerp(SKIN_F0 / (4.0f * alpha0 * alpha0), SKIN_F0 / (4.0f * alpha1 * alpha1), SKIN_LOBE_MIX);
    const float matched = clamp(1.0f / (2.0f * peak), 1.0f, 12.0f);   // the 2: skinSpecScale reads a 50% mask as 1
    return lerp(1.0f, matched, saturate(TESR_SkinLook.w));
}

// Stephen Hill's fit to the irradiance of a normalised spherical Gaussian of sharpness lambda, per colour: N.L blurred by the
// lobe (MJP, "SG Series" part 3). Plain saturate(N.L) as lambda grows; light wraps past the terminator and the peak spreads
// as it shrinks.
float3 SkinSGIrradiance(float3 NdotL, float3 lambda) {
    const float c0 = 0.36f;
    const float c1 = 1.0f / (4.0f * c0);
    const float3 eml = exp(-lambda);
    const float3 em2l = eml * eml;
    const float3 rl = 1.0f / lambda;
    const float3 scale = 1.0f + 2.0f * em2l - rl;
    const float3 bias = (eml - em2l) * rl - em2l;
    const float3 x = sqrt(max(1.0f - scale, 1e-8f));
    const float3 x0 = c0 * NdotL;
    const float3 x1 = c1 * x;
    const float3 n = x0 + x1;
    const float3 y = abs(x0) <= x1 ? n * n / x : saturate(NdotL);
    return saturate(scale * y + bias);
}

// Diffusion for one light, along per-colour softened normals. Each colour's lobe sharpness comes from the curvature c (1/mm),
// fitted to Penner's pre-integrated skin for d'Eon's profile over N.L -1..1 and radii 1-20 mm: worst error 0.07 red (radii
// under 2 mm, slightly dark), 0.016 green, 0.009 blue.
float3 SkinDiffusion(float3 N, float3 Nsoft, float3 L) {
    const float3 normalGreen = normalize(lerp(N, Nsoft, 0.6f));
    const float3 normalBlue = normalize(lerp(N, Nsoft, 0.3f));
    const float3 NdotL = float3(dot(Nsoft, L), dot(normalGreen, L), dot(normalBlue, L));
    // Flat skin, or skin the screen-space blur takes (skinCurvature 0): the lobes are plain saturate(N.L) there, so skipped.
    [branch] if (skinCurvature > 0.002f) {
        const float c = skinCurvature;
        const float3 lambda = float3(2.4f * pow(c, -1.5f), 28.0f / (c * c), 71.0f / (c * c));
        return SkinSGIrradiance(NdotL, lambda);
    }
    return saturate(NdotL);
}

// Shadow visibility (0 dark .. 1 lit) with light scattered into the penumbra, by ShadowScatter.
float3 SkinShadow(float shadow) {
    const float penumbra = shadow * (1.0f - shadow) * 4.0f;
    return saturate(shadow + penumbra * SKIN_PENUMBRA_TINT * TESR_SkinLook.z);
}

// The sun is a disc, not a point (PB NVR's PBR.hlsl SunSpecularDir, after Karis, Real Shading in Unreal Engine 4, 2013): the
// highlight is lit from the point of the disc nearest the view reflection, and each lobe widened to cover the disc is
// normalised so it gains no energy. The point does not depend on the roughness: both lobes share it (and so H and the cosines
// below), which keeps the many-lamp variants inside ps_3_0's 32 temporaries.
float3 SkinSunSpecularDir(float3 N, float3 V, float3 L) {
    const float3 r = reflect(-V, N);
    const float3 centerToRay = dot(L, r) * r - L;
    return normalize(L + centerToRay * saturate(SKIN_SUN_RADIUS / max(length(centerToRay), 1e-5f)));
}
float SkinSunNormalization(float roughness) {
    const float alpha = roughness * roughness;
    const float ratio = alpha / saturate(alpha + SKIN_SUN_RADIUS * 0.5f);
    return ratio * ratio;
}

// Both GGX highlight lobes of one light (the light already decoded: PBRLight), times N.L, PI and the light (PB NVR's PBR.hlsl
// SpecularLobe each); the sun as a disc.
float3 SkinSpecular(float3 N, float3 V, float3 L, float3 light, bool sun) {
    const float r0 = clamp(skinRoughness * SKIN_LOBE0, 0.04f, 1.0f);
    const float r1 = clamp(skinRoughness * SKIN_LOBE1, 0.04f, 1.0f);
    float n0 = 1.0f;
    float n1 = 1.0f;
    if (sun) {
        L = SkinSunSpecularDir(N, V, L);
        n0 = SkinSunNormalization(r0);
        n1 = SkinSunNormalization(r1);
    }
    const float3 H = normalize(V + L);
    const float NdotL = max(shades(N, L), 0.00001f);
    const float NdotV = max(shades(N, V), 0.00001f);
    const float NdotH = shades(N, H);
    const float LdotH = shades(L, H);
    const float3 fresnel = Fresnel(SKIN_F0.xxx, (1.0f).xxx, LdotH);
    const float3 lobes = lerp(BRDF(r0, fresnel, NdotV, NdotL, NdotH) * n0, BRDF(r1, fresnel, NdotV, NdotL, NdotH) * n1, SKIN_LOBE_MIX);
    return lobes * (NdotL * PI * skinSpecScale * SkinVanillaMatch()) * light;
}

// One light. Returns the diffuse light (diffusion and transmission, times the albedo) and, in specular, the highlights, kept
// apart for the screen-space blur. Every vector in one space (SkinTemplate: tangent).
//   albedo:         decoded (LinColor)
//   N, Nsoft:       the bumped normal and its blurred version
//   geometricNdotL: the vertex normal's cosine to the light (the surface's own far side, for the self-shadow and transmission)
//   light:          the light's colour, decoded (PBRLight), with any gamma-space factor (the lamp falloff, the projected actor
//                   shadow) multiplied in beforehand
//   shadow:         the light's shadow here, 0..1 (NVR's sun shadow; 1 for a lamp, whose shadow the template applies after)
//   sun:            the sun (its highlight from a disc), or a lamp
float3 SkinLight(float3 albedo, float3 N, float3 Nsoft, float geometricNdotL, float3 V, float3 L, float3 light, float shadow, bool sun, out float3 specular) {
    L = normalize(L);

    // Diffusion, split into plain N.L and the light scattering carries beyond it.
    const float3 scattered = SkinDiffusion(N, Nsoft, L);
    const float3 direct = min(scattered, saturate(dot(N, L)));
    const float3 wrapped = scattered - direct;

    // The shadow map also marks the surface's own far side as shadowed, which would cut the scattered wrap off at the
    // geometric terminator. On that far side only (geometric N.L 0.05 to -0.1), the WRAPPED part escapes the shadow map;
    // plain N.L always keeps it. Skin facing the light inside another surface's shadow (a neck under the jaw) stays shadowed.
    float selfShadowed = saturate((0.05f - geometricNdotL) / 0.15f);
    selfShadowed = selfShadowed * selfShadowed * (3.0f - 2.0f * selfShadowed) * saturate(TESR_SkinLook.z);
    const float3 shadowColor = SkinShadow(shadow);
    const float3 diffuse = albedo * (direct * shadowColor + wrapped * lerp(shadowColor, 1.0f, selfShadowed));

    // Transmission only through thin features (ears, nostrils, fingers: high curvature), and only where the light reaches the
    // far side of the surface (Jimenez: saturate(0.3 - N.L)).
    const float thin = smoothstep(0.3f, 0.9f, skinThinness);
    const float farSide = saturate(0.3f - geometricNdotL);
    const float3 transmitDir = normalize(L + Nsoft * SKIN_TRANSMIT_DISTORTION);
    const float back = pow(saturate(dot(V, -transmitDir)), SKIN_TRANSMIT_POWER);
    const float3 transmitted = albedo * SkinLinearColor(SKIN_TRANSMIT_TINT) * (back * thin * farSide * TESR_SkinMaterial.w * shadow);

    specular = SkinSpecular(N, V, L, light, sun) * shadow;
    return (diffuse + transmitted) * light;
}

// --- sky -------------------------------------------------------------------------------------------------------------

// The sky along a world direction, GAMMA-ENCODED as SkyAmbient.hlsl gives it, blurred by the roughness: the sharp sky
// (SkyAmbientSharp, the harmonics read as radiance) for a smooth lobe, the cosine-convolved sky for a rough one. Below the
// horizon the harmonics hold no sky: the convolved value stands in, as SkyAmbientSharpened does for metal.
float3 SkinSkyRadiance(float3 r, float roughness) {
#if SKYLIGHTING_MODE == 1
    // No harmonics in this mode: one read of the sky, kept above the horizon, dimmed to the ground below it.
    const float3 sky = sqrt(max(GetSkyRadiance(normalize(float3(r.xy, max(r.z, 0.02f)))), 0.0f));
    return sky * lerp(1.0f, 0.4f, saturate(-r.z));
#else
    const float3 plain = SkyAmbientRadiance(r, TESR_PBRExtraData.z);
    float3 sharp = SkyAmbientSharp(r);
    sharp = lerp(sharp, max(sharp, plain), saturate(-r.z));
    return lerp(sharp, plain, saturate(roughness));
#endif
}

// The sky reflected off both lobes (the split-sum response at skin's F0, PBR.hlsl EnvBRDFApprox), faded where the
// reflection points into the surface (horizon occlusion). Sets skinSkyWeight: call it BEFORE SkinAmbient. WORLD-space
// vectors; geometricNormal is the vertex normal, N the bumped one.
float3 SkinSkyReflection(float3 V, float3 geometricNormal, float3 N) {
    const float3 r = reflect(-V, N);
    const float NdotV = saturate(dot(N, V));
    float horizon = saturate(1.0f + 1.2f * dot(r, geometricNormal));
    horizon *= horizon;

    const float r0 = clamp(skinRoughness * SKIN_LOBE0, 0.04f, 1.0f);
    const float r1 = clamp(skinRoughness * SKIN_LOBE1, 0.04f, 1.0f);
    const float response = lerp(EnvBRDFApprox(SKIN_F0.xxx, r0, NdotV), EnvBRDFApprox(SKIN_F0.xxx, r1, NdotV), SKIN_LOBE_MIX).x;
    const float weight = response * horizon * skinSpecScale * TESR_SkinLook.x;
    skinSkyWeight = saturate(weight);

    const float3 radiance = lerp(SkinSkyRadiance(r, r0), SkinSkyRadiance(r, r1), SKIN_LOBE_MIX);
    return LinColor(radiance) * (LinScale(TESR_PBRExtraData.y) * weight);
}

// The weather ambient plus the sky light (PBRScale.hlsl SkyAmbient), the sky read along the bumped and the blurred normal
// and blended per colour as the direct light is; less the share the reflection took. WORLD-space normals.
float3 SkinAmbient(float3 ambient, float3 albedo, float3 N, float3 Nsoft) {
    const float3 flat = PBRAmbient(ambient);
    const float3 sharp = SkyAmbient(N, 1.0f);
    const float3 soft = SkyAmbient(Nsoft, 1.0f);
    const float3 sky = float3(soft.r, lerp(sharp.g, soft.g, 0.6f), lerp(sharp.b, soft.b, 0.3f));
    return (flat + sky * (1.0f - skinSkyWeight)) * albedo;
}

#endif
