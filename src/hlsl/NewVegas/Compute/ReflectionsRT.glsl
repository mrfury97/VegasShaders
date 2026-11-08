// UNOFFICIAL 2026-10-28 (NVR DXVK, RT reflections, docs/next/FORK-RT-REFLECTIONS.md): what the two in-game RT passes of Reflections share
// (ReflectionsRayGen.frag, ReflectionsRTResolve.frag). The rays themselves are traced OUTSIDE the game: AMD's 32-bit Vulkan driver has
// no ray tracing, so the RT core (session rt-sun-core, docs/next/FORK-RT-CORE.md) runs them in its 64-bit helper (NvrRtHelper.exe)
// with RT/ReflectionsRT.comp. Per frame:
//   1 RayGen (here, half resolution): the screen march, as ReflectionsTrace; writes what it found (rgb linear, a confidence), the
//     reflected direction (octahedral, RG32F) and the surface (its view z, roughness, +2 where a ray is wanted: the screen left a share
//     missing on a surface no rougher than MaxRoughness). The two ray images are shared with the helper (RG32F: the RT core's formats).
//   2 the helper traces those rays (closest hit, then one sun shadow ray from the hit) and writes the hit image (RGBA16F), at the RT
//     core's late exchange (RtCore::Exchange, inside ReflectionsEffect::RtTrace).
//   3 Resolve (here): shades the hits (the screen at the hit, the cube toward it, or the estimate), then the old fallbacks for what is
//     left, the firefly cap and the diagnostics: the rest of ReflectionsTrace, line for line. Its output is the trace's output.
// Without the helper this frame the plain ReflectionsTrace runs instead (exactly as before).
// Spaces: camera-relative world (world axes, origin at the camera) for every ray start and direction.

layout(set = 1, binding = 17) uniform texture2D t_rtMarch;    // RayGen out 0: the march's colour (linear rgb), confidence (a)
layout(set = 1, binding = 18) uniform texture2D t_rtDir;      // RayGen out 1: the reflected direction (world axes), octahedral (xy)
layout(set = 1, binding = 19) uniform texture2D t_rtRay;      // RayGen out 2: x the surface's view z (0: nothing reflects), y roughness (+2: a ray)
layout(set = 1, binding = 20) uniform texture2D t_rtHit;      // the helper: x hit distance (-1 no ray, 0 nothing within Distance),
                                                              // y flags (1 land, 2 albedo known, 4 normal known, 8 glow map), zw the hit's normal (octahedral)
layout(set = 1, binding = 22) uniform texture2D t_rtMarchHit;  // 2026-11-07 RayGen out 3 (R16F): the march's hit distance (0: none)
layout(set = 1, binding = 21) uniform texture2D t_rtShade;    // the helper: rgb the hit draw's albedo (linear), a sun visible at the hit (0..1)
layout(set = 1, binding = 26) uniform texture2D t_rtBounce;   // 2026-11-07 (perf-reflfix) MetalBounce, where t_rtHit.y has 256: rgb the metal's
                                                              // Fresnel tint at the first hit, a the bounce's hit distance from it (0: none)

layout(set = 2, binding = 1, std140) uniform RtConstants {
    vec4 rt;    // x 1 = on, y MaxRoughness, z Distance (game units), w DebugView
    vec4 rt2;   // x screen match tolerance (share of the depth), y the estimate's brightness, z sun amount (0 night / indoors), w the lamp count
    vec4 sunDir;    // world, toward the sun (TESR_SunDirection)
    vec4 sunColor;  // TESR_SunColor (gamma)
    vec4 rt3;       // 2026-10-29: x 1 = VNDF-sampled rays (RtVndf), y RoughShare's roughness (above: 1 ray per 2x2 block a frame; 0 off),
                    // z ContactSpread (the eye-distance share at which a hit counts as 'far' for the blur), w GlowBrightness
    vec4 rt4;       // 2026-10-30: x 1 = the hit's ambient from the light grid at the hit (HitAmbientGrid), else the cube's;
                    // 2026-10-31: y 1 = RayReuse (rough pixels reuse their neighbours' hits), z its lowest roughness;
                    // 2026-11-05: w 1 = ShadeOffscreen (a hit out of view shaded from itself, not the cube)
    vec4 lamps[24]; // 2026-10-29: the 12 nearest scene lamps: [2i] xyz camera-relative position, w reach; [2i + 1] rgb colour x dimmer (gamma)
    vec4 rt5;       // 2026-11-07: x 1 = the RT reflection denoiser runs this frame (core/RtReflDenoise.h): RayGen's second noise number
                    // decorrelated, RayGen writes the march's hit distance, Resolve the hit distance (Spread .y)
} rtc;

#define RtOn (rtc.rt.x > 0.5)
#define RtDenoiserOn (rtc.rt5.x > 0.5)
#define RtMaxRoughness rtc.rt.y
#define RtDistance rtc.rt.z
#define RtDebugView rtc.rt.w
#define RtTolerance rtc.rt2.x

// Octahedral unit vectors (exact enough in RG32F; the helper decodes with the same formula).
vec2 OctEncode(vec3 v) {
    v /= abs(v.x) + abs(v.y) + abs(v.z);
    vec2 e = v.xy;
    if (v.z < 0.0) e = (1.0 - abs(v.yx)) * vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
    return e;
}
vec3 OctDecode(vec2 e) {
    vec3 v = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (v.z < 0.0) v.xy = (1.0 - abs(v.yx)) * vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
    return normalize(v);
}

// How much RT counts on a surface of this roughness: 1 up to 75 % of MaxRoughness, 0 at it.
float RtFade(float roughness) { return 1.0 - smoothstep(0.75 * RtMaxRoughness, RtMaxRoughness, roughness); }

// The shading of a hit neither the screen nor the cube can show (FORK-RT-REFLECTIONS.md "Hit shading", tiers 2-4): the hit draw's
// average diffuse colour (protocol v2 records; 0.4 grey when unknown) on the hit triangle's own normal (facing the ray when unknown),
// lit by the surroundings cube's blurriest mip round that normal (the light the place has) and, outdoors by day, the sun as far as
// the helper's shadow ray from the hit saw it. No texture detail and no lamps of their own (tiers 5-6).
// 2026-10-29: plus the 12 nearest scene lamps (the game's own colour, dimmer and reach; a squared-quadratic falloff), the hit's key
// lamp (keyLamp >= 0, indoors / at night) through the helper's shadow ray (visible = shade.a then).
vec3 RtShadeHit(vec3 hitW, vec3 normal, vec3 albedo, float visible, int keyLamp) {
    vec3 light = CubeOn > 0.0 ? EnvAlong(normal, 1.0).rgb : SkyAlong(vec3(0.0, 0.0, 1.0)) * 0.5;
    // 2026-10-30 HitAmbientGrid: the light grid (AmbientLight's, world cells) where the hit is: the light that reaches that spot from
    // around it (bounce included), facing the hit's normal; the cube (one probe near the camera) stays where the grid does not reach.
    if (rtc.rt4.x > 0.5 && GridOn > 0.0) {
        vec4 grid = GridIrradianceAt(hitW, normal);
        light = mix(light, grid.rgb, grid.a);
    }
    float sunAmount = rtc.rt2.z;
    if (sunAmount > 0.0) light += linearize(rtc.sunColor.rgb) * saturate(dot(normal, normalize(rtc.sunDir.xyz))) * sunAmount * visible;
    int count = int(rtc.rt2.w + 0.5);
    for (int i = 0; i < count; i++) {
        vec3 toLamp = rtc.lamps[2 * i].xyz - hitW;
        float d = length(toLamp), reach = rtc.lamps[2 * i].w;
        float fall = saturate(1.0 - (d * d) / (reach * reach));
        vec3 c = linearize(rtc.lamps[2 * i + 1].rgb) * fall * fall * saturate(dot(normal, toLamp / max(d, 1.0e-3)));
        light += i == keyLamp ? c * visible : c;
    }
    return albedo * light * rtc.rt2.y;
}
