// 2026-11-04 dust on top surfaces ([Shaders.PBR.Dust], effects/PBR.cpp), off by default and compiled in only when on at startup
// (DUST_ON, ShaderRecord::LoadShader). In the desert, sand settles on whatever faces up: the tops of rocks, car roofs and hoods,
// ledges and crates (Death Stranding, the sand in Uncharted 4). Here it goes into the surface's own colour, before any lighting,
// so the sun and the lamps light it as they light the surface and the fog sits in front of it: a thin sand-coloured layer where
// the surface faces up (from UpFrom to UpFull of the world normal's height), broken into patches by world-space value noise
// (Patchiness), keeping a little of the texture's own detail so it reads as a coat, not paint. Object and parallax shaders, the
// passes that carry the texture only (not the lighting-only lamp passes, whose texture a later pass multiplies in), both lamp
// paths (1-3 and 4+: an object switching between them as lamps come into reach made the dust come and go), static meshes in the
// world scene only (NVR_DustDraw: not people, creatures, armour or the first-person arms and weapon), and not hair.
//
// TESR_ names: ShaderRecord binds only TESR_-prefixed constants by name (2026-11-04: first named NVR_, they were never sent).
// Registers: c192, c193, c195 are clear in the object and parallax pixel shaders (Skin.hlsl's, which they do not include).

float4 TESR_DustData : register(c192);	// x amount (0: skipped), y 1 / patch size (game units), z UpFrom, w UpFull (world normal height)
float4 TESR_DustColor : register(c193);	// rgb the dust's colour (linear), w Patchiness (0 an even coat, 1 patchy)
float4 TESR_DustOrigin : register(c195);	// xyz the camera's world position (the pattern stays on the world as the camera moves), w Variation
// Per draw, not by name (core/DustDraw.h): x 1 = a static mesh in the world scene; 0 = skinned (people, creatures, armour, the
// first-person arms), first person, reflections. A pixel shader cannot tell skinned from static by itself (SKIN is a vertex define).
bool NVR_DustDraw : register(b14);	// (2026-11-05: was c169.x, which ForwardCloudShadows' NVR_CloudSun took (X4500 with both on); every float
									// register below ps_3_0's c224 is taken: a bool register, as SmoothCutouts' b15)

// A hash without sin (after Dave Hoskins' hash12, MIT): steady for the large cell numbers of world positions.
float DustHash(float2 p) {
    float3 p3 = frac(p.xyx * 0.1031f);
    p3 += dot(p3, p3.yzx + 33.33f);
    return frac((p3.x + p3.y) * p3.z);
}

// Smooth value noise, 0..1.
float DustNoise(float2 p) {
    const float2 i = floor(p);
    const float2 f = frac(p);
    const float2 u = f * f * (3.0f - 2.0f * f);
    return lerp(lerp(DustHash(i), DustHash(i + float2(1.0f, 0.0f)), u.x),
        lerp(DustHash(i + float2(0.0f, 1.0f)), DustHash(i + 1.0f), u.x), u.y);
}

// albedo: the surface colour as it goes into the lighting; worldNormal: its world-space normal (z up); cameraRelative: the pixel's
// camera-relative world position; valid: the position came from this build's vertex shader.
float3 DustCoverDraw(float3 albedo, float3 worldNormal, float3 cameraRelative, bool valid) {
    [branch] if (TESR_DustData.x <= 0.0f || !valid) return albedo;
    const float up = smoothstep(TESR_DustData.z, TESR_DustData.w, worldNormal.z);
    [branch] if (up <= 0.0f) return albedo;
    const float2 w = (cameraRelative.xy + TESR_DustOrigin.xy) * TESR_DustData.y;
    float noise = DustNoise(w) * 0.7f + DustNoise(w * 4.3f + 11.7f) * 0.3f;
    // UNOFFICIAL 2026-11-12 Variation (TESR_DustOrigin.w; the owner: "the dust should be patchy and inconsistent"): every upward face got
    // the same patch pattern at the same strength, so it read as one even coat. A much larger pattern (about five patches across) scales
    // it from 0.3 to 1.5, so whole rocks and roofs differ: some nearly clean, some well dusted. 0 = as before.
    noise *= lerp(1.0f, lerp(0.3f, 1.5f, DustNoise(w * 0.19f + 5.3f)), TESR_DustOrigin.w);
    const float patches = lerp(1.0f, saturate(noise * 2.4f - 0.7f), TESR_DustColor.w);
    const float cover = saturate(up * patches * TESR_DustData.x);
    // a third of the texture's own light and dark kept under the dust (a coat, not paint)
    const float detail = luma(albedo) / max(luma(TESR_DustColor.rgb), 1e-3f);
    return lerp(albedo, TESR_DustColor.rgb * lerp(1.0f, clamp(detail, 0.25f, 2.0f), 0.33f), cover);
}

float3 DustCover(float3 albedo, float3 worldNormal, float3 cameraRelative, bool valid) {
    float3 result = albedo;
    if (NVR_DustDraw) result = DustCoverDraw(albedo, worldNormal, cameraRelative, valid);	// a bool register: a static branch round it all
    return result;
}
