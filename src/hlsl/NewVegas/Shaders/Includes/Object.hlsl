// UNOFFICIAL 2026-11-02 SpecularFromMap ([Shaders.PBR.Rmaos], on unless RMAOS_SPEC_OFF): the _rmaos map's alpha is the "specular level"
// of the Community Shaders True PBR layout, a multiplier on a non-metal's 0.04 reflectance (white = 0.04, as without it; metal takes
// its colour as before). Before PBR.hlsl, which reads PBR_DIELECTRIC_F0. RmaosRead sets RmaosSpecular; without a map it stays 1.
// Not in the 6-lamp interior shader (NVR_SLOT_TIGHT: at the slot count the driver refuses): 0.04 there, as before.
#if defined(RMAOS) && !defined(RMAOS_SPEC_OFF) && !defined(NVR_SLOT_TIGHT)
#define RMAOS_SPEC
static float RmaosSpecular = 1.0f;
#define PBR_DIELECTRIC_F0 (0.04f * RmaosSpecular)
#endif

// UNOFFICIAL 2026-11-05 VanillaBridge ([Shaders.PBR.Main] VanillaBridge, restart: VANILLA_BRIDGE_ON; VanillaBridgeAmount live, TESR_PBRData.x).
// The game draws an object's highlight as Blinn-Phong: mask x (N.H)^n x the light, where the mask is the normal map's alpha (a
// highlight STRENGTH, not a gloss map) and n the mesh's glossiness (NiMaterialProperty, the game's Toggles.z). NVR drew every
// highlight with GGX at F0 0.04 and a roughness of (1 - mask) or a material class's: the mask set the WIDTH, n was dropped and the
// strength was fixed, so authored glints (power armour, polished metal; n 30-100, mask 0.3-0.7) became faint wide sheens. The
// bridge gives the GGX highlight the authored one's width, peak and energy instead (derivation and measurements: effects\PBR\README.md):
//   width: Blinn-Phong (N.H)^n ~ exp(-n t^2 / 2) is the Beckmann lobe of alpha^2 = 2 / (n + 2) (Walter et al. 2007). GGX at that
//          alpha, peak matched (below), has 0.77-0.91 of Blinn-Phong's half width and 1.04-1.20 x its energy for n 2-200 (GGX: a
//          narrower core, longer tails). An exact energy match would need alpha^2 = (1.7-1.9) / (n + 2) and narrows the core to 0.73;
//          the least-squares shape fit is (1.9-2.6) / (n + 2): 2 sits between them.
//   peak:  GGX lit and seen head-on returns D(1) x F x Vis x N.L x pi = F / (4 alpha^2) of the light (Vis = 1/4, both Smith terms),
//          the game's returns mask: F0 = 4 alpha^2 x mask = 8 mask / (n + 2) (at most 1). Normalised Blinn-Phong would need
//          (n + 8) / (8 pi); the game's lobe is not normalised, which is why F0 falls as n rises: a sharp lobe of the same peak holds
//          less energy.
//   mask:  used as it is, not decoded: with linear lighting the game's gamma-space sum (albedo + mask) x light is matched on
//          average over albedo 0.05-0.6 and mask 0.05-1 (mean error +0.004 of full scale; decoding the mask: -0.14).
// Only the highlight takes the bridged F0; the diffuse keeps the dielectric's (1 - F) share (PBR.hlsl PBR_SPEC_FRESNEL), so surfaces
// keep their diffuse light. Metal keeps the bridged strength, tinted by its colour's hue. Where the mask is (nearly) 0 the game drew
// no highlight and the old roughness and F0 fill the gap (BridgeT ramps over mask 0-0.04): matte surfaces are unchanged. An _rmaos map
// (RMAOS_ACTIVE) decides on its own, as before; a material class with authored=replace (MaterialRules.h) keeps its rule's look.
// Compiled in only where the template defines NVR_VANILLA_BRIDGE (the highlight-drawing object and parallax pixel shaders that know n);
// VanillaBridgeAmount 0 gives the old values exactly (lerp by 0).
#ifdef NVR_VANILLA_BRIDGE
static float BridgeT = 0.0f;		// how much the vanilla data decides this pixel's highlight (BridgeSetup)
static float BridgeF0 = 0.04f;		// the authored highlight's reflectance, 8 mask / (n + 2)
static float BridgeRough = 1.0f;	// its roughness, (2 / (n + 2))^(1/4) (GGX() takes roughness^4 = alpha^2)
static float BridgeTMap = 0.0f;	// VanillaBridgeOverMaps: BridgeT where an _rmaos map is active (applied after the map's roughness)
#define BRIDGE_OVER_MAP(r) lerp((r), BridgeRough, BridgeTMap)
#ifndef PBR_DIELECTRIC_F0
    #define PBR_DIELECTRIC_F0 0.04f
#endif
// The highlight's F0 (before Reflectivity): the old one where BridgeT is 0, the authored strength where it is 1 (metal: tinted by
// its colour's hue, normalised to its brightest channel, so the strength stays the authored one).
// UNOFFICIAL 2026-11-06 VanillaBridgeDielectricMax ([Shaders.PBR.Main], restart: VANILLA_BRIDGE_DIELECTRIC_MAX, defined only below 1;
// report: cliffs and other rough stone very shiny). Matching the game's peak gives F0 = 8 mask / (n + 2): a WIDE authored lobe (low
// glossiness n, about 10 on many meshes) with a bright mask (or a normal map with no alpha, which reads as 1) came out at F0 0.3-0.7 on
// rock, concrete, wood and plaster, near a metal's, spread over a broad sheen. Non-metal surfaces reflect 2-8 % head-on, so the non-metal
// share of the bridged strength is capped there; metal keeps the full authored strength (power armour's glints are its metal share).
// Sharp authored glints (high n) are under the cap anyway (n 100, mask 0.5: 0.04), so they are unchanged.
float3 BridgeSpecF0(float3 baseColor, float metal) {
    const float3 plain = lerp(PBR_DIELECTRIC_F0, baseColor, metal);
    const float3 hue = baseColor / max(max(baseColor.r, max(baseColor.g, baseColor.b)), 1e-4f);
    #ifdef VANILLA_BRIDGE_DIELECTRIC_MAX
        return lerp(plain, lerp(min(BridgeF0, VANILLA_BRIDGE_DIELECTRIC_MAX).xxx, BridgeF0 * hue, metal), BridgeT);
    #else
        return lerp(plain, BridgeF0 * lerp(1.0f, hue, metal), BridgeT);
    #endif
}
#define PBR_SPEC_F0(baseColor, metal) BridgeSpecF0((baseColor), (metal))
#else
#define BRIDGE_OVER_MAP(r) (r)
#endif
#ifdef __INTELLISENSE__
    #include "Pointlights.hlsl"
    #include "PBR.hlsl"
    #include "SkyAmbient.hlsl"
#else
    #include "includes/Pointlights.hlsl"	// vanillaAtt
    #include "includes/PBR.hlsl"	// PBR, PBRSun, PBRDiffuse, PBRSpecular ...
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

float4 TESR_PBRData : register(c32);	// x VanillaBridgeAmount (2026-11-05; was the unused Metallicness), y roughness scale, z light scale, w ambient scale
float4 TESR_PassRedirect : register(c210);	// UNOFFICIAL ExactPassSums (core/PassSums.h): x 1 = this added pass draws into NVR's buffer, linear
float4 TESR_PBRExtraData : register(c33);	// x saturation, y sky strength, z sky directionality, w metal fade distance
// UNOFFICIAL 2026-10-17 LinearPreDecode (ObjectTemplate.hlsl LIN_PREDECODED): the strengths LinScale would decode per pixel, decoded
// once per frame on the CPU (PBR.cpp); otherwise the same LinScale calls as before.
#ifdef LIN_PREDECODED
float4 TESR_LinearScales : register(c168); // x LinScale(TESR_PBRData.z), y LinScale(TESR_PBRData.w), z LinScale(TESR_PBRExtraData.y)
#define LIN_SCALE_LIGHT TESR_LinearScales.x
#define LIN_SCALE_AMBIENT TESR_LinearScales.y
#define LIN_SCALE_SKY TESR_LinearScales.z
#else
#define LIN_SCALE_LIGHT LinScale(TESR_PBRData.z)
#define LIN_SCALE_AMBIENT LinScale(TESR_PBRData.w)
#define LIN_SCALE_SKY LinScale(SKY_AMBIENT_STRENGTH)
#endif

// UNOFFICIAL material rules ([Shaders.PBR.MaterialRules], core/MaterialRules.h): the class of the texture being drawn, from
// the editable table NewVegasReloaded.dll.materials.txt, written by NVR before each object (Render.cpp MaterialRulesDraw).
// Read only where MATERIAL_RULES is defined (ObjectTemplate). While TESR_MaterialRule.x is 0 (rules off, or no rule for
// the texture) every function below returns exactly what it did without the rules.
#ifdef MATERIAL_RULES
float4 TESR_MaterialRule  : register(c197); // x: 1 a rule applies, y: metal strength (0 = none), z: 2 = all metal (else the guess), w: reflections multiplier
float4 TESR_MaterialRough : register(c198); // x/y: roughness where the highlight mask is full / bare (x < 0: the global roughness), z: 1 DebugView
float4 TESR_MaterialTint  : register(c199); // rgb: the class's DebugView colour
#endif

// UNOFFICIAL 2026-10-14 _rmaos maps ([Shaders.PBR.Rmaos], Render.cpp RmaosDraw): where a texture has a <name>_rmaos.dds (the
// Community Shaders "True PBR" layout: r roughness, g metallic, b ambient occlusion with white = none, a specular level), NVR binds
// it on s2 (no object shader samples s2, and the game binds nothing there for objects) and sets NVR_Rmaos.x for the draw. The
// map's roughness and metal then replace the guesses from the normal map's alpha and the colour, and its occlusion darkens the
// ambient light. RmaosRead samples it once at the top of main (derivatives); everything below reads RmaosValue. Defined only
// in ObjectTemplate's pixel shaders, and only when on at startup (RMAOS_ON); while NVR_Rmaos.x is 0 every function returns
// exactly what it did before.
#ifdef RMAOS
float4 NVR_Rmaos : register(c220); // x: 1 = this draw has an _rmaos map on s2, y: occlusion strength, z: DebugView (1 roughness, 2 metal, 3 occlusion, 4 which objects have one), w: 1 = metal from the map (MetalFromMap)
sampler2D NVR_RmaosMap : register(s2);
static float4 RmaosValue = float4(1.0f, 0.0f, 1.0f, 1.0f);
// UNOFFICIAL 2026-10-18 FadeIn (pbrpop, [Shaders.PBR.Rmaos] FadeIn): x below 1 = the map arrived a moment ago: its values blend in
// from what the object had without it (x = the map's share), so a map that arrives while the object is in view does not pop. x = 1:
// exactly the map, as before; 0: no map.
#define RMAOS_ACTIVE (NVR_Rmaos.x > 0.0f)
#ifdef RMAOS_NO_FADE
    #define RMAOS_FADING false	// (the includer's shader is near its slot limit: the map comes in at once there)
#else
    #define RMAOS_FADING (NVR_Rmaos.x < 1.0f)
#endif

// UNOFFICIAL 2026-10-18 per-weather RoughnessScale / MetalnessScale for the maps ([Shaders.PBR.Main/Rain/Night/NightRain/Interiors],
// PBRShaders::UpdateConstants; idea from Physically-Based-Reloaded (Balebandro), GPL-3.0, commit c1415cb8). TESR_PBRMath (PBR.hlsl)
// z = MetalnessScale - 1, w = RoughnessScale - 1, so a register never written (0) and the neutral 1.0 defaults leave the map's
// values exactly as read (x + x * 0 = x); the map's own values are in 0-1, so the saturate changes nothing there either.
// RoughnessScale is 1-2 (rougher only, never above 1), MetalnessScale 0-1. AOScale needs no shader change (c220.y, RmaosDraw).
// 2026-11-04 micro-shadowing ([Shaders.PBR.MicroShadow], off by default; compiled in only when on at startup: MICRO_SHADOW_ON).
// A normal map's bumps never shadow themselves, so cracks and crevices look painted on. Naughty Dog's micro-shadowing (Uncharted 4,
// The Last of Us Part II) darkens the direct light where the material's own ambient occlusion says the surface sits in a crevice,
// more as the light comes in at a grazing angle: saturate(|N.L| + 2 AO^2 - 1) per light. The occlusion is the _rmaos map's blue
// channel; without a map (or with the feature off) the factor is exactly 1. Strength blends it in (c194.x).
#if defined(RMAOS) && defined(MICRO_SHADOW_ON) && MICRO_SHADOW_ON
#define MICRO_SHADOW_ACTIVE
float4 TESR_MicroShadow : register(c194);	// x strength (0: none, 1: the map as it is, up to 3: its crevices that much deeper)
static float MicroShadowAO = 1.0f;	// the map's occlusion at this pixel, toward 1 by (1 - strength); set in RmaosRead
float MicroShadowTerm(float3 N, float3 L) {
    return MicroShadowAO >= 1.0f ? 1.0f : saturate(abs(dot(N, normalize(L))) + 2.0f * MicroShadowAO * MicroShadowAO - 1.0f);
}
// MICRO_SHADOW(N, L) is a factor with its own "*": "x * MICRO_SHADOW(N, L) y". Off it expands to nothing at all, so the compiled
// shaders stay byte-identical (a literal 1.0f factor reordered the compiler's constant pool in four parallax shaders).
#define MICRO_SHADOW(N, L) MicroShadowTerm((N), (L)) *
#endif

void RmaosRead(float2 uv) {
    float2 dx = ddx(uv), dy = ddy(uv);
    [branch] if (RMAOS_ACTIVE) {
        RmaosValue = tex2Dgrad(NVR_RmaosMap, uv, dx, dy);
        RmaosValue.rg = saturate(RmaosValue.rg + RmaosValue.rg * TESR_PBRMath.wz);
        #ifdef RMAOS_SPEC
            RmaosSpecular = RMAOS_FADING ? lerp(1.0f, RmaosValue.a, NVR_Rmaos.x) : RmaosValue.a;	// (FadeIn: from the plain 0.04)
        #endif
        #ifdef MICRO_SHADOW_ACTIVE
            // 2026-11-04 (user: "no visible change between 0-1 or 1-2"): the formula only bites below an occlusion of about 0.7, and
            // generated maps are mostly lighter; Strength above 1 deepens the map's occlusion (2: twice as far from white) before it.
            MicroShadowAO = saturate(1.0f - (1.0f - RmaosValue.b) * TESR_MicroShadow.x * (RMAOS_FADING ? NVR_Rmaos.x : 1.0f));
        #endif
    }
}

// The ambient light's share the map's occlusion leaves (1 without a map).
float RmaosAmbient() {
    return RMAOS_ACTIVE ? lerp(1.0f, RmaosValue.b, RMAOS_FADING ? saturate(NVR_Rmaos.y) * NVR_Rmaos.x : saturate(NVR_Rmaos.y)) : 1.0f;
}

// DebugView: the map's roughness, metal or occlusion in grey; 4: objects with a map in green, others dim grey.
float3 RmaosDebug(float3 color, float3 albedo) {
    [branch] if (NVR_Rmaos.z > 0.5f) {
        #ifdef ONLY_LIGHT
            return 0.0f;
        #else
            float dim = luma(albedo) * 0.35f;
            if (NVR_Rmaos.z > 3.5f) return RMAOS_ACTIVE ? lerp(dim.xxx, float3(0.1f, 0.9f, 0.2f), 0.6f) : dim.xxx;
            if (!RMAOS_ACTIVE) return dim.xxx;
            float value = NVR_Rmaos.z < 1.5f ? RmaosValue.r : NVR_Rmaos.z < 2.5f ? RmaosValue.g : RmaosValue.b;
            return value.xxx;
        #endif
    }
    return color;
}
#else
float RmaosAmbient() { return 1.0f; }
float3 RmaosDebug(float3 color, float3 albedo) { return color; }
#endif
#ifndef MICRO_SHADOW
#define MICRO_SHADOW(N, L)	// micro-shadowing off (or no _rmaos maps): nothing, the lights exactly as before
#endif

// UNOFFICIAL 2026-10-20 MaterialAware reflections ([Shaders.Reflections.Main] MaterialAware, core/ReflMaterial.h): the first
// (opaque) pass of an object with an _rmaos map leaves its roughness and metal in the scene's alpha for the Reflections effect.
// D3D9 cannot draw into a second target with the game's MSAA on, so the scene's own alpha (fp16) carries it:
// 2 + 0.25 x metal (4 steps) + 0.24 x roughness, always 2 or more where the game writes 0-1. NVR sets c212.x per pass
// (core/ReflMaterial.h): 1 only for the first passes of opaque, untested objects with a map, in the world scene; never for a
// blended pass (its alpha weighs its colour). The world scene keeps blended passes off the alpha (separate alpha blend ZERO/ONE).
// Only alpha 1 is replaced (an object that fades keeps its own). Compiled in only when MaterialAware is on at startup
// (REFL_MATERIAL_ON, with RMAOS_ON); otherwise the shaders are exactly what they were. Kept to a few instructions: the 6-lamp
// interior shader (SLS2035) is near the slot count the driver refuses.
#if defined(RMAOS) && defined(REFL_MATERIAL_ON) && REFL_MATERIAL_ON
float4 NVR_ReflMaterial : register(c212); // yz: the reflections' roughness = the map's x y + z (RoughnessOverrides: 1, 0 = the map's) // x: 1 = this pass leaves its material in the alpha (pinned clear of every other object-shader constant; NVR_, not TESR_: ShaderRecord re-uploads every TESR_ constant when the shader binds, after the pass hook set it)
float ReflMaterialAlpha(float alpha) {
    // (the map's own roughness: the shader's, after SpecularAA, would bring back its whole computation into the shaders without a highlight)
    // Metal only above 0.6 (the generated maps guess metal from grey colours, up to ~0.6: grey marble and concrete came out half metal).
    #ifdef NVR_SLOT_TIGHT
        const float metal = 0.0f;   // the 6-lamp interior shader (SLS2035) sits at the slot count the driver refuses (run 2026-10-21 01:51: refused at +7)
    #else
        float metal = floor(saturate(RmaosValue.g * 2.5f - 1.5f) * 3.0f + 0.5f);
    #endif
    // RoughnessOverrides (core/ReflMaterial.h): y, z = 1, 0 keep the map's roughness, 0, r give r.
    float roughness = saturate(RmaosValue.r * NVR_ReflMaterial.y + NVR_ReflMaterial.z);
    float encoded = 2.0f + metal * 0.25f + roughness * 0.24f;
    return lerp(alpha, encoded, NVR_ReflMaterial.x * (alpha >= 0.999f ? 1.0f : 0.0f));
}
#else
#define ReflMaterialAlpha(alpha) (alpha)
#endif

// UNOFFICIAL 2026-11-05 VanillaBridge (see the top of this file): this pixel's authored highlight, from the normal map's alpha
// (mask) and the mesh's glossiness (shine: Toggles.z, or PSLightColor[1].w in the 4+ lamp OPT shaders). Call once per pixel before
// getRoughness / getMetalRoughness and the lighting, after RmaosRead.
#ifdef NVR_VANILLA_BRIDGE
void BridgeSetup(float mask, float shine) {
    const float n = clamp(shine, 2.0f, 2048.0f);	// (2: F0 = mask; the game's own default glossiness when a mesh has none is in range)
    const float a2 = 2.0f / (n + 2.0f);
    BridgeRough = saturate(max(sqrt(sqrt(a2)), 0.043f) * TESR_PBRData.y);	// [Shaders.PBR.*] Roughness scales it as it scaled the old one
    BridgeF0 = min(4.0f * a2 * saturate(mask), 1.0f);
    // TESR_PBRData.x: VanillaBridgeAmount, + 2 with VanillaBridgeOverMaps (PBR.cpp). Mask below 0.04: no highlight authored, the old look fills the gap.
    const bool overMaps = TESR_PBRData.x > 1.5f;
    float t = saturate(mask * 25.0f) * (overMaps ? TESR_PBRData.x - 2.0f : TESR_PBRData.x);
    #ifdef MATERIAL_RULES
        t = (TESR_MaterialRule.x > 0.0f && TESR_MaterialRough.w > 0.5f) ? 0.0f : t;	// the class opted out (authored=replace)
    #endif
    #ifdef RMAOS
        // the map decides (FadeIn: as it blends in), unless VanillaBridgeOverMaps: then the map keeps its occlusion (and metal), the highlight's
        // roughness and strength come from the game's data (getRoughness / getMetalRoughness apply BridgeTMap after the map's roughness)
        BridgeTMap = RMAOS_ACTIVE && overMaps ? t : 0.0f;
        t = RMAOS_ACTIVE && !overMaps ? (RMAOS_FADING ? t * (1.0f - NVR_Rmaos.x) : 0.0f) : t;
    #endif
    BridgeT = t;
}
#else
#define BridgeSetup(mask, shine)
#endif

// Roughness from the normal map's gloss (alpha), scaled by [Shaders.PBR] Roughness; a material rule or an _rmaos map overrides it.
// UNOFFICIAL 2026-11-05 VanillaBridge: where BridgeT > 0, towards the authored highlight's roughness (the rule fills only the gap).
float getRoughness(float glossAmt) {
    #ifdef RMAOS
        if (RMAOS_ACTIVE && !RMAOS_FADING) return BRIDGE_OVER_MAP(max(RmaosValue.r, 0.02f));
    #endif
    float r = saturate(max(0.043, 1 - glossAmt) * TESR_PBRData.y);
    #ifdef MATERIAL_RULES
        [flatten] if (TESR_MaterialRule.x > 0.0f && TESR_MaterialRough.x >= 0.0f)
            r = lerp(TESR_MaterialRough.x, TESR_MaterialRough.y, saturate(1.0f - glossAmt));
    #endif
    #ifdef NVR_VANILLA_BRIDGE
        r = lerp(r, BridgeRough, BridgeT);
    #endif
    #ifdef RMAOS
        if (RMAOS_ACTIVE) r = RMAOS_FADING ? lerp(r, max(RmaosValue.r, 0.02f), NVR_Rmaos.x) : max(RmaosValue.r, 0.02f);	// FadeIn
        r = BRIDGE_OVER_MAP(r);	// UNOFFICIAL 2026-11-05 VanillaBridgeOverMaps (0 weight otherwise)
    #endif
    return r;
}

// Roughness from a gloss map and the mesh's specular power: 1 without gloss, falling with log(power) where the map is glossy.
float getRoughness(float mapGloss, float meshPower){
    float sharpness = log(meshPower) / 4;
    return saturate(1 - sharpness * mapGloss);
}

// The game's highlight is weakened where the surface faces the light at a low angle (cos <= 0.2), smoothly to 0.5 x at cos 0.
float objHighlightFade(float highlight, float cosL) {
    return 0.2 >= cosL ? (highlight * saturate(cosL + 0.5)) : highlight;
}

// The game's object lighting with a precomputed falloff: Lambert, and in the SPECULAR variants a Blinn highlight that fades out
// where the surface turns from the light (ONLY_SPECULAR: the highlight alone).
float3 getVanillaLightingAtt(float3 toLight, float falloff, float3 radiance, float3 toEye, float3 N, float3 baseColor, float glossAmt, float specPower) {
    float3 L = normalize(toLight);
    float3 V = normalize(toEye);
    float3 H = normalize(L + V);
    float cosL = shades(N.xyz, L.xyz);

    #if defined(ONLY_SPECULAR) || defined(SPECULAR)
        float highlight = glossAmt * pow(abs(shades(N.xyz, H.xyz)), specPower);
    #endif
    #if defined(ONLY_SPECULAR)
        return saturate((objHighlightFade(highlight, cosL) * radiance.rgb) * falloff);
    #else
        float3 result = baseColor.rgb * cosL * radiance.rgb * falloff;
        #if defined(SPECULAR)
            result += saturate((objHighlightFade(highlight, cosL) * radiance.rgb) * falloff);
        #endif
        return result;
    #endif
}

// The same for a lamp of the given radius (the game's falloff).
float3 getVanillaLighting(float3 toLight, float radius, float3 radiance, float3 toEye, float3 N, float3 baseColor, float glossAmt, float specPower) {
    // UNOFFICIAL 2026-10-20 (LinearLight.hlsl); 2026-10-21 or LampFalloff's shape (LampFalloff.hlsl)
    float falloff = LampAtt(vanillaAtt(toLight, radius));
    return getVanillaLightingAtt(toLight, falloff, radiance, toEye, N, baseColor, glossAmt, specPower);
}

// PBR lamp term for this shader variant: the highlight alone (ONLY_SPECULAR), diffuse and highlight (SPECULAR), or diffuse only.
float3 objLampTerm(float metal, float r, float3 baseColor, float3 N, float3 V, float3 L, float3 radiance) {
    #if defined(ONLY_SPECULAR)
        return PBRSpecular(metal, r, baseColor, N, V, L, radiance);
    #elif defined(SPECULAR)
        return PBR(metal, r, baseColor, N, V, L, radiance);
    #else
        return PBRDiffuse(metal, r, baseColor, N, V, L, radiance);
    #endif
}

// A lamp of the given radius (the game's falloff), PBR. Colour scaled by Light scale, base colour by Saturation.
float3 getPointLightLighting(float3 toLight, float radius, float3 radiance, float3 toEye, float3 N, float3 baseColor, float r) {
    float3 scaled = radiance * LIN_SCALE_LIGHT;
    float3 graded = lerp(luma(baseColor), baseColor, TESR_PBRExtraData.x);
    // UNOFFICIAL 2026-10-20: the falloff decoded like the colours (LinearLight.hlsl); 2026-10-21 or LampFalloff's shape (LampFalloff.hlsl)
    float falloff = LampAtt(vanillaAtt(toLight, radius));
    return falloff * MICRO_SHADOW(N, toLight) objLampTerm(0, r, graded, N, toEye, toLight, scaled);
}

// The same with the falloff given. UNOFFICIAL: metal, the share of the pixel that is metal (getDerivedMetallic below; 0 elsewhere).
float3 getPointLightLightingAtt(float3 toLight, float falloff, float3 radiance, float3 toEye, float3 N, float3 baseColor, float r, float metal) {
    float3 scaled = radiance * LIN_SCALE_LIGHT;
    float3 graded = lerp(luma(baseColor), baseColor, TESR_PBRExtraData.x);
    float shaped = LampAtt(falloff);	// UNOFFICIAL 2026-10-20 / 2026-10-21, as above
    return shaped * MICRO_SHADOW(N, toLight) objLampTerm(metal, r, graded, N, toEye, toLight, scaled);
}

float3 getPointLightLightingAtt(float3 toLight, float falloff, float3 radiance, float3 toEye, float3 N, float3 baseColor, float r) {
    return getPointLightLightingAtt(toLight, falloff, radiance, toEye, N, baseColor, r, 0.0f);
}

// The sun, PBR with the sun's disc for the highlight (the diffuse-only variant takes plain PBRDiffuse).
float3 getSunLighting(float3 toSun, float3 radiance, float3 toEye, float3 N, float3 baseColor, float r, float metal) {
    float3 scaled = radiance * LIN_SCALE_LIGHT;
    float3 graded = lerp(luma(baseColor), baseColor, TESR_PBRExtraData.x);
    #if defined(ONLY_SPECULAR)
        return MICRO_SHADOW(N, toSun) PBRSunSpecular(metal, r, graded, N, toEye, toSun, scaled);
    #elif defined(SPECULAR)
        return MICRO_SHADOW(N, toSun) PBRSun(metal, r, graded, N, toEye, toSun, scaled);
    #else
        return MICRO_SHADOW(N, toSun) PBRDiffuse(metal, r, graded, N, toEye, toSun, scaled);
    #endif
}

float3 getSunLighting(float3 toSun, float3 radiance, float3 toEye, float3 N, float3 baseColor, float r) {
    return getSunLighting(toSun, radiance, toEye, N, baseColor, r, 0.0f);
}

#ifdef NVR_FOLD
// UNOFFICIAL 2026-10-21 FoldHighlights (ObjectTemplate.hlsl): the lamps' and the sun's light as the one-pass shaders give it (SPECULAR:
// the PBR diffuse with its highlight, metal), for the light-only and extra-lamp passes, whatever variant they are.
float3 getPointLightLightingAttFull(float3 lightDir, float att, float3 lightColor, float3 viewDir, float3 normal, float3 albedo, float roughness, float metallic) {
    lightColor = lightColor * LIN_SCALE_LIGHT;
    albedo = lerp(luma(albedo), albedo, TESR_PBRExtraData.x);
    att = LampAtt(att);
    return att * MICRO_SHADOW(normal, lightDir) PBR(metallic, roughness, albedo, normal, viewDir, lightDir, lightColor);
}

float3 getSunLightingFull(float3 lightDir, float3 lightColor, float3 viewDir, float3 normal, float3 albedo, float roughness, float metallic) {
    lightColor = lightColor * LIN_SCALE_LIGHT;
    albedo = lerp(luma(albedo), albedo, TESR_PBRExtraData.x);
    return MICRO_SHADOW(normal, lightDir) PBRSun(metallic, roughness, albedo, normal, viewDir, lightDir, lightColor);
}
#endif



// [_Main.Develop.Main], via Debug.cpp UpdateSettings. c135: c132 is TESR_ShadowBlur.
// Populated even with Shaders.Debug disabled -- Debug has no per-frame UpdateConstants.
float4 TESR_DebugVar : register(c135);

// --- Hemisphere skylight ------------------------------------------------------------------
// Additive upper-sky term on top of the weather ambient, weighted by w = (1 + N.up) / 2.
// w must stay linear in the dot product: that is the exact cosine-weighted form factor.
// [Shaders.PBR.*] SkylightingScale. No separate toggle: 0 disables the term.
#define SKY_AMBIENT_STRENGTH (TESR_PBRExtraData.y)	// sky term at full view of the sky

// The game's ambient, scaled by Ambient scale (and an _rmaos map's occlusion).
float3 getAmbientLighting(float3 gameAmbient, float3 baseColor) {
    return gameAmbient * LIN_SCALE_AMBIENT * baseColor * RmaosAmbient();
}

// The same plus the sky term along the geometric world normal.
float3 getAmbientLighting(float3 gameAmbient, float3 baseColor, float3 geoNormalWS, float normalKnown) {
    float3 flatAmbient = gameAmbient * LIN_SCALE_AMBIENT;

    // AmbientScale (TESR_PBRData.w) scales the weather ambient above but not this: the sky is a
    // second, independent light source, so SkylightingScale is its only strength knob and it
    // survives AmbientScale = 0.
    float3 skyTerm = LinSky(SkyAmbientRadiance(geoNormalWS, TESR_PBRExtraData.z)) * LIN_SCALE_SKY;

    // normalKnown is 0 under a vanilla vertex shader, which carries no world position.
    return (flatAmbient + skyTerm * normalKnown) * baseColor * RmaosAmbient();
}

// ---- Metal (UNOFFICIAL) ----------------------------------------------------------------------------------------------
// New Vegas has no metalness map, so every object was lit as a dielectric: a white 4 % highlight over full diffuse. Bare
// metal -- guns above all -- read as plastic: white highlights in the sun, matte in the shade, where nothing reflected
// its surroundings. The normal map's alpha is a highlight-STRENGTH mask, not a gloss map: a gun's body is mostly near 0
// (the 10mm pistol's median is 0.06) with its edges and worn spots painted high, so it cannot find the metal alone.
// Metal is guessed per pixel instead: grey (unsaturated) colour counts as metal, partly, and the mask makes it fully
// metal and polished. [Shaders.PBR.Metal] sets the strength separately for the first-person model and for the world
// (PBRShaders swaps them round the first-person pass; World 0 leaves the world as it was). docs/derived-metallicness.md.
float4 TESR_PBRMetal     : register(c34); // x: strength this pass (0 off), y: metal share of grey parts the mask leaves bare, z/w: colour saturation where metal starts/stops being ruled out
float4 TESR_PBRMetalLook : register(c36); // x: roughness of bare metal, y: of fully masked (polished) metal, z: sky reflection strength, w: 1 debug view

float getMetalMask(float specMask) {
    return smoothstep(0.05f, 0.45f, specMask);
}

// This draw's metal strength: [Shaders.PBR.Metal] for the pass, or the material rule's (0 for a class that is never metal).
// Uniform over the draw, so the metal branches on it stay cheap.
float getMetalStrength() {
    #ifdef RMAOS
        if (RMAOS_ACTIVE) return 1.0f;	// the map says where the metal is
    #endif
    #ifdef MATERIAL_RULES
        if (TESR_MaterialRule.x > 0.0f) return TESR_MaterialRule.y;
    #endif
    return TESR_PBRMetal.x;
}

float getDerivedMetallicGuess(float specMask, float greyness) {
    #ifdef MATERIAL_RULES
        // A rule's "full" metal (chrome, brass) is metal whatever its colour; otherwise the guess at the rule's strength.
        [branch] if (TESR_MaterialRule.x > 0.0f)
            return TESR_MaterialRule.z > 1.5f ? TESR_MaterialRule.y : saturate(greyness * lerp(TESR_PBRMetal.y, 1.0f, getMetalMask(specMask)) * TESR_MaterialRule.y);
    #endif
    return saturate(greyness * lerp(TESR_PBRMetal.y, 1.0f, getMetalMask(specMask)) * TESR_PBRMetal.x);
}

float getDerivedMetallic(float specMask, float3 albedo) {
    float peak = max(max(albedo.r, albedo.g), albedo.b);
    float chroma = (peak - min(min(albedo.r, albedo.g), albedo.b)) / max(peak, 0.001f);
    float greyness = 1.0f - smoothstep(TESR_PBRMetal.z, max(TESR_PBRMetal.w, TESR_PBRMetal.z + 0.001f), chroma);
    #ifdef RMAOS
        // UNOFFICIAL 2026-10-28 MetalFromMap (c220 w): only then the map's metal. a tester: "metal in pbr doesn't seem to do anything anymore" --
        // NVR's generated maps carry the metal guess baked at World 0.5, so on every object with a map the Metal sliders did nothing.
        [branch] if (RMAOS_ACTIVE && NVR_Rmaos.w > 0.5f) return RMAOS_FADING ? lerp(getDerivedMetallicGuess(specMask, greyness), RmaosValue.g, NVR_Rmaos.x) : RmaosValue.g;
    #endif
    return getDerivedMetallicGuess(specMask, greyness);
}

// The metal fades out with distance, from 60% of TESR_PBRExtraData.w to all of it (w 0: never). PBRShaders keeps w short of
// where the game starts dropping objects' highlights: beyond it the game draws them without, and the metal went at once.
float getMetalFade(float3 cameraRelativePos) {
    return TESR_PBRExtraData.w > 0.0f ? 1.0f - smoothstep(0.6f * TESR_PBRExtraData.w, TESR_PBRExtraData.w, length(cameraRelativePos)) : 1.0f;
}

// Metal's roughness: satin where the mask is bare, polished where it is painted (the mask is no gloss map, see above).
float getMetalRoughness(float specMask) {
    #ifdef RMAOS
        if (RMAOS_ACTIVE && !RMAOS_FADING) return BRIDGE_OVER_MAP(max(RmaosValue.r, 0.02f));
    #endif
    float roughness = lerp(TESR_PBRMetalLook.x, TESR_PBRMetalLook.y, getMetalMask(specMask));
    #ifdef MATERIAL_RULES
        // A rule's roughness range: its bare end where the mask is bare, its polished end where the mask is painted.
        [flatten] if (TESR_MaterialRule.x > 0.0f && TESR_MaterialRough.x >= 0.0f)
            roughness = lerp(TESR_MaterialRough.y, TESR_MaterialRough.x, getMetalMask(specMask));
    #endif
    #ifdef NVR_VANILLA_BRIDGE
        roughness = lerp(roughness, BridgeRough, BridgeT);	// UNOFFICIAL 2026-11-05 VanillaBridge: the authored finish where there is one
    #endif
    #ifdef RMAOS
        if (RMAOS_ACTIVE) roughness = RMAOS_FADING ? lerp(roughness, max(RmaosValue.r, 0.02f), NVR_Rmaos.x) : max(RmaosValue.r, 0.02f);	// FadeIn
        roughness = BRIDGE_OVER_MAP(roughness);	// UNOFFICIAL 2026-11-05 VanillaBridgeOverMaps (0 weight otherwise)
    #endif
    return roughness;
}

// The metal's reflections of its surroundings, times the rule's (1 without one).
float getMaterialReflections() {
    #ifdef MATERIAL_RULES
        if (TESR_MaterialRule.x > 0.0f) return TESR_MaterialRule.w;
    #endif
    return 1.0f;
}

// [Shaders.PBR.MaterialRules] DebugView: each class in its colour over a dim grey image, textures without a rule grey.
// Passes that only add lamp light add nothing while it is on.
float3 MaterialRulesDebug(float3 color, float3 albedo) {
    #ifdef MATERIAL_RULES
        [branch] if (TESR_MaterialRough.z > 0.5f) {
            #ifdef ONLY_LIGHT
                return 0.0f.xxx;
            #else
                float dim = luma(albedo) * 0.35f;
                return TESR_MaterialRule.x > 0.0f ? lerp(dim.xxx, TESR_MaterialTint.rgb, 0.7f) : dim.xxx;
            #endif
        }
    #endif
    return color;
}

// The split-sum environment response EnvBRDFApprox now lives in PBR.hlsl (UNOFFICIAL 2026-10-25: the skin shader's sky
// reflection takes it too, and the skin cannot include this file).

// The sky's light along a direction of world up component z, averaged round the horizon (the object shaders know which
// way is up per pixel, not the compass bearing): SkyAmbientRadiance's scale and encoding. Mode 0: the spherical harmonic
// terms that do not depend on the bearing.
float3 SkyAmbientElevation(float z) {
    #if SKYLIGHTING_MODE == 1
        return SkyAmbientRadiance(float3(sqrt(saturate(1.0f - z * z)), 0.0f, z), TESR_PBRExtraData.z);
    #else
        float3 E = TESR_SkyIrradianceObjects[0].rgb + TESR_SkyIrradianceObjects[2].rgb * z + TESR_SkyIrradianceObjects[6].rgb * (3.0f * z * z - 1.0f);
        return sqrt(max(E, 0.0f));
    #endif
}

// UNOFFICIAL SharpSky: the reflected view in world space, for SkyAmbientSharpened. Its bearing is the reflection of the geometric
// normal (worldPos is camera-relative, so the view ray is its direction); its height is upReflect, so the normal map's bumps still
// show. The tangent frame carries no world bearing, so a bump does not turn the sky sideways.
float3 getMetalSkyDirection(float3 worldPos, float3 worldNormal, float upReflect) {
    float3 r = reflect(normalize(worldPos), worldNormal);
    float horizontal = length(r.xy);
    float2 bearing = horizontal > 1e-4f ? r.xy / horizontal : float2(1.0f, 0.0f);
    return float3(bearing * sqrt(saturate(1.0f - upReflect * upReflect)), upReflect);
}

// What metal reflects in place of the diffuse light it lacks: the weather ambient and the sky along the reflected view
// (upReflect: that view's world up component; skyValid 0 when it is unknown), weighted by the split-sum response for
// the metal's colour (F0) and roughness. getAmbientLighting's terms, with the reflection for the normal.
// UNOFFICIAL SharpSky: with it on and the sky known, the sky is taken along the reflected direction (getMetalSkyDirection, from
// worldPos and the geometric worldNormal), blended from the plain sky. Off, this is the plain sky exactly.
float3 getMetalAmbient(float3 ambient, float3 f0, float roughness, float NdotV, float upReflect, float skyValid,
                       float3 worldPos, float3 worldNormal) {
    float3 sky = SkyAmbientElevation(upReflect);
    [branch] if (TESR_SharpSky.x > 0.0f && skyValid > 0.0f)
        sky = SkyAmbientSharpened(getMetalSkyDirection(worldPos, worldNormal, upReflect), sky);
    float3 env = ambient * LIN_SCALE_AMBIENT + LinSky(sky) * LIN_SCALE_SKY * skyValid;
    // UNOFFICIAL CubeReflections ([Shaders.PBR.Metal], ObjectTemplate defines NVR_LIVE_CUBE): what the camera saw round it
    // along the reflected view (includes/LiveCube.hlsl) in place of the ambient and sky, as far as the cube has seen that
    // direction. Needs the direction, so only where the sky is known (skyValid).
    #ifdef NVR_LIVE_CUBE
    [branch] if (TESR_EnvCube.x > 0.0f && skyValid > 0.0f) {
        float4 live = LiveCube(getMetalSkyDirection(worldPos, worldNormal, upReflect), roughness);
        env = lerp(env, LinSky(live.rgb), TESR_EnvCube.x * live.a);
    }
    #endif
    return EnvBRDFApprox(f0, roughness, NdotV) * env * TESR_PBRMetalLook.z * getMaterialReflections() * RmaosAmbient();
}
