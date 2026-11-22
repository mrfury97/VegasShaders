// Reflections technique 0 (Composite, full resolution, in the frame chain) as a Vulkan pass: Effects/Reflections.fx.hlsl
// CompositePS line for line (REFL_MATERIAL_ON / REFL_SPECOCC_ON / REFL_ADD_ON: cb.flags).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Reflections.glsl"
#include "ReflLampHighlight.glsl"   // (2026-11-08 perf-reflfix3) MirrorLampHighlight
#include "ReflMetalF0.glsl"         // (2026-11-08 perf-reflfix4) fallback metal F0 when AlbedoBuffer is unavailable

// (2026-11-22 upstream 1.1.3 merge) upstream's t_albedo (binding 28) and t_capNormals (29) are the textures Reflections.glsl declares as
// t_gbAlbedo (30) and t_gbNormals (31), whose bindings 28 / 29 here hold this fork's film and smooth normals: read through those
#define t_albedo t_gbAlbedo          // TESR_AlbedoBuffer (CapturedNormals::AlbedoTexture; cb.wetFx2.z > 0.5)
#define t_capNormals t_gbNormals     // CapturedNormals::Texture (COLOR3, GBufferV2 FDSunVisDecode; cb.wetFx2.w > 0.5)

float pow5(float x) { float x2 = x * x; return x2 * x2 * x; }

// Decode GBufferV2's fd-sunvis dielectric specular F0 (Includes/DeferredPixel.hlsl FDSunVisDecode: 0..0.08 in 0.01 steps, 0.04 default).
float DecodeGBufferF0(vec2 ruv) {
    float f0 = 0.04;
    if (cb.wetFx2.w > 0.5) {
        float a = textureLod(sampler2D(t_capNormals, POINT_S), ruv, 0.0).a;
        float q = (a > 0.99 && a < 2.005) ? floor(abs(a - (a > 1.5 ? 2.0 : 1.0)) * 1024.0 + 0.5) : 0.0;
        float s = floor((q + 0.5) / 48.0);
        if (s > 0.5) f0 = (s - 1.0) * 0.01;
    }
    if (mod(cb.material2.x, 2.0) > 0.5) {
        float b = textureLod(sampler2D(t_matbuf, POINT_S), ruv, 0.0).b;
        if (b < -1.99 && b > -3.01) f0 = 0.0;   // CapturedNoHighlight matte draw
    }
    return f0;
}

// Forward shaders' WetSkyReflection (Includes/Wetness.hlsl + SkyAmbient.hlsl) in the composite's linear space, for both SkylightingMode 0 and 1.
vec3 WetSkyReflectionLinear(vec3 r) {
    vec3 encoded;
    if (cb.wetFx.x > 1.5) {
        float height = pow(clamp(r.z * 0.5 + 0.5, 0.0, 1.0), 3.0);
        float sunUp = max(cb.sunPosition.z, 0.0);
        float haze = (1.0 - height) * (1.0 - height) * cb.skyData.x;
        float towardSun = pow(clamp(dot(r, cb.sunPosition.xyz) * 0.5 + 0.5, 0.0, 1.0), 1.0 / max(cb.skyData.y, 1.0e-4));
        vec3 sunLin = linearize(cb.sunDiskColor.rgb);
        vec3 sunsetLin = linearize(cb.sunsetColor.rgb);
        float lowSun = clamp(pow(1.0 - sunUp, 8.0), 0.0, 1.0) * cb.sunAmount.x;
        vec3 disc = (1.0 + sunUp) * sunLin + sunsetLin * lowSun * haze;
        float byDay = smoothstep(0.0, 0.5, cb.sunAmount.x);
        vec3 skyRad = mix(linearize(cb.skyLowColor.rgb), linearize(cb.skyColor.rgb), height);
        skyRad = mix(skyRad, linearize(cb.horizonColor.rgb), clamp(haze * (0.5 + towardSun * 0.5), 0.0, 1.0));
        skyRad += mix(vec3(0.0), disc * towardSun * (1.0 - sunUp) * haze * cb.skyData.z, byDay);
        float seen = 0.5 * r.z + 0.5;
        encoded = sqrt(max(skyRad, vec3(0.0))) * seen;
    } else {
        vec3 E = cb.skySh[0].rgb
               + cb.skySh[1].rgb * r.y
               + cb.skySh[2].rgb * r.z
               + cb.skySh[3].rgb * r.x
               + cb.skySh[4].rgb * (r.x * r.y)
               + cb.skySh[5].rgb * (r.y * r.z)
               + cb.skySh[6].rgb * (3.0 * r.z * r.z - 1.0)
               + cb.skySh[7].rgb * (r.x * r.z)
               + cb.skySh[8].rgb * (r.x * r.x - r.y * r.y);
        encoded = sqrt(max(E, vec3(0.0)));
    }
    return linearize(encoded);
}

float WetFilmReflectance(float NdotV, float rough, float gloss) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = rough * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
    vec2 ab = vec2(-1.04, 1.04) * a004 + r.zw;
    return clamp((0.02 * ab.x + ab.y) * gloss, 0.0, 1.0);
}

// UNOFFICIAL 2026-11-08 (perf-reflfix3) MirrorLampHighlight: the ring round the pixel (8 taps at cb.mirrorLamp.y full-resolution pixels, the
// same surface: depth within 5 % + 4 units) for ReflLampHighlightScale, and what to keep: the sun's glint (the reflected direction within
// ~2-4 degrees of the sun, while it is up) and the flashlight's (toward its light). P: the surface (view space), N, V as main's.
vec3 MirrorLampHighlight(vec2 ruv, vec3 lin, vec3 P, vec3 N, float depth, vec3 share, float roughness) {
    if (!(cb.mirrorLamp.x > 0.0) || roughness >= 0.15) return lin;
    vec3 top = vec3(0.0);
    int valid = 0;
    const vec2 px = cb.reciprocalResolution.xy * cb.mirrorLamp.y;
    for (int k = 0; k < 8; k++) {
        const float a = 0.7853982 * float(k);
        const vec2 at = ruv + vec2(cos(a), sin(a)) * px;
        if (any(lessThan(at, vec2(0.0))) || any(greaterThan(at, cb.renderScale.xy))) continue;
        if (abs(readDepthLod(at) - depth) > depth * 0.05 + 4.0) continue;
        ReflHighlightRingAdd(ReflHighlightLuma(linearize(textureLod(sampler2D(t_rendered, POINT_S), at, 0.0).rgb)), top);
        valid++;
    }
    const float ringMax = top.z;
    const vec3 R = reflect(normalize(P), N);   // (view space)
    float keep = 0.0;
    if (cb.mirrorLamp.z > 0.5) keep = smoothstep(0.9976, 0.9994, dot(R, normalize(WorldToViewDir(cb.sunDirection.xyz))));   // (cos 4 .. 2 degrees)
    if (cb.flashPos.w > 0.0) keep = max(keep, smoothstep(0.9976, 0.9994, dot(R, normalize(WorldToViewDir(cb.flashPos.xyz) - P))));
    return ReflLampHighlightScale(lin, ringMax, valid, share, roughness, cb.mirrorLamp.x, keep);
}

vec3 FlashlightHighlight(vec3 P, vec3 N, float roughness, vec3 F0) {
    if (!(cb.flashPos.w > 0.0) || cb.flashLook.z < 0.5) return vec3(0.0);
    const vec3 toLight = WorldToViewDir(cb.flashPos.xyz) - P;
    const float d = length(toLight);
    if (!(d < cb.flashPos.w) || d < 1.0e-3) return vec3(0.0);
    const vec3 L = toLight / d, V = -normalize(P);
    const float coneCos = dot(-L, WorldToViewDir(cb.flashDir.xyz));
    if (coneCos <= cb.flashDir.w) return vec3(0.0);
    const float cone = smoothstep(cb.flashDir.w, cb.flashColor.w, coneCos);
    const float q = d * d / (cb.flashPos.w * cb.flashPos.w), window = saturate(1.0 - q);
    const float falloff = cb.flashLook.x > 0.0 ? window * window / (1.0 + cb.flashLook.x * q) : window;
    const float NdotL = dot(N, L), NdotV = dot(N, V);
    if (NdotL <= 0.0 || NdotV <= 0.0) return vec3(0.0);
    const vec3 H = normalize(L + V);
    const float NdotH = saturate(dot(N, H)), VdotH = saturate(dot(V, H));
    const float a = max(roughness * roughness, 0.002), a2 = a * a;
    const float dd = NdotH * NdotH * (a2 - 1.0) + 1.0;
    const float D = a2 / (3.14159265 * dd * dd);
    const float k = a * 0.5;
    const float G = (NdotL / (NdotL * (1.0 - k) + k)) * (NdotV / (NdotV * (1.0 - k) + k));
    const float f90 = clamp(50.0 * dot(F0, vec3(0.3333)), 0.0, 1.0) * clamp(1.0 - roughness, 0.0, 1.0);
    const vec3 F = F0 + (max(vec3(f90), F0) - F0) * pow5(1.0 - VdotH);
    const vec3 spec = D * G * F / (4.0 * NdotV) * cb.flashColor.rgb * (falloff * cone * cone);
    return min(spec, vec3(cb.flashLook.y));
}

void main() {
    vec2 ruv = screenToRect(uv);
    vec4 color = textureLod(sampler2D(t_rendered, LINEAR_S), ruv, 0.0);
    float depth = readDepthLod(ruv);
    // DebugView 4: where the rain wets the ground.
    if (abs(DebugView - 4.0) < 0.5) {
        bool isFloor = ViewToWorldDir(ViewNormal(ruv)).z > 0.75 && depth < DrawDistance && depth < farZ * 0.98;
        if (!isFloor) { o_color = vec4(color.rgb * 0.15, color.a); return; }
        float open = RoofMaskOn > 0.5 ? OpenAbove(ruv, depth) : 1.0;
        o_color = open > 0.5 ? vec4(vec3(0.4 + 0.6 * saturate(Wetness)), 1.0) : vec4(0.5, 0.05, 0.05, 1.0);
        return;
    }
    // DebugView 3: the surroundings cube as seen from here, over the whole screen (an unbound cube reads black, as in D3D9).
    if (abs(DebugView - 3.0) < 0.5) {
        vec4 env = pc.pass != 0u ? EnvAlong(normalize(toWorld(ruv)), 0.0) : vec4(0.0);
        o_color = vec4(delinearize(env.rgb * env.a), 1.0);
        return;
    }
    // REFL_MATERIAL_ON, DebugView 7: the material buffer (an unbound one reads 0, as in D3D9).
    // (2026-11-20 UnifiedReflections: the G-buffer's material as Reflectivity decodes it; the floor rule's known materials in grey (rough) to
    // white (smooth) + green metal, as the D3D9 view shows MaterialBufferRoughness's)
    if ((MaterialOn || GBufferOn) && abs(DebugView - 7.0) < 0.5) {
        vec4 m = GBufferOn ? GBufferMaterial(ruv) : cb.flags.w > 0.5 ? textureLod(sampler2D(t_material, POINT_S), ruv, 0.0) : vec4(0.0);
        if (GBufferOn && m.b > 0.25 && m.b <= 0.75) { o_color = vec4(vec2(1.0 - m.r) * vec2(1.0 - m.g, 1.0), 1.0 - m.r, 1.0); return; }
        o_color = m.b > 0.5 ? vec4(m.r, m.g, 0.6 * (1.0 - m.r), 1.0) : vec4(color.rgb * 0.15, color.a);
        return;
    }
    // UNOFFICIAL 2026-11-09 DebugView 11 (diagnostic, core/WeaponMask.h)
    if (abs(DebugView - 11.0) < 0.5) {
        const bool model = IsWeapon(ruv);
        const vec4 dim = vec4(color.rgb * 0.25, color.a);
        if (!WeaponOn) { o_color = model ? vec4(1.0, 0.0, 1.0, 1.0) : dim; return; }
        if (!WeaponMasked) { o_color = model ? vec4(1.0, 1.0, 0.0, 1.0) : dim; return; }
        const bool set = textureLod(sampler2D(t_weaponMask, POINT_S), ruv, 0.0).r > 0.5;
        o_color = model ? (set ? vec4(1.0, 0.0, 0.0, 1.0) : vec4(0.0, 0.0, 1.0, 1.0)) : (set ? vec4(0.0, 1.0, 0.0, 1.0) : dim);
        return;
    }
    vec3 N = ViewNormal(ruv);
    float roughness;
    float amount = Reflectivity(ruv, depth, N, roughness);
    if (SurfaceWater) { o_color = color; return; }   // UNOFFICIAL 2026-10-31 RT water: the water shaders lay it on next frame
    if (amount <= 0.0) { o_color = DebugView > 0.0 ? vec4(color.rgb * 0.15, color.a) : color; return; }
    // UNOFFICIAL 2026-11-20 DebugView 12 (diagnostic): on the reflecting pixels, red how far the normal map bends the reflection away from the
    // smooth normal (full red at 20 degrees; 0 without the normal-mapped normals), green the roughness the reflection is blurred with (after
    // Mirror and the specular AA: 1 full green). Dimmed: no reflection.
    if (abs(DebugView - 12.0) < 0.5) {
        const float bend = acos(clamp(dot(N, SmoothViewNormal(ruv)), -1.0, 1.0));
        o_color = vec4(saturate(bend / 0.349), saturate(roughness), 0.0, 1.0);
        return;
    }

    // Depth-aware bilinear upsample (clamped to active trace sub-rect so right/bottom RenderScale borders never read stale texels).
    vec2 position = ruv / LowTexel - 0.5;
    vec2 base = floor(position);
    vec2 fraction = position - base;
    float tolerance = depth * 0.03 + 4.0;
    bool pixelWeapon = IsWeapon(ruv);
    vec4 reflection = vec4(0.0);
    float weights = 1.0e-5;
    vec2 rectMaxTap = max(cb.renderScale.xy - 0.5 * LowTexel, 0.5 * LowTexel);
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 2; x++) {
            vec2 at = clamp((base + vec2(x, y) + 0.5) * LowTexel, 0.5 * LowTexel, rectMaxTap);
            vec2 tapPixelUV = at - 0.5 * (LowTexel - cb.reciprocalResolution.xy);
            float tapDepth = readDepthLod(tapPixelUV);
            float weight = (x != 0 ? fraction.x : 1.0 - fraction.x) * (y != 0 ? fraction.y : 1.0 - fraction.y);
            weight /= 1.0 + abs(tapDepth - depth) / tolerance;
            if (IsWeapon(tapPixelUV) != pixelWeapon) weight *= 1.0e-4;
            reflection += textureLod(sampler2D(t_buffer, POINT_S), at, 0.0) * weight;
            weights += weight;
        }
    }
    reflection /= weights;

    vec3 surfPos = SurfacePosition(ruv, depth);
    vec3 V = normalize(surfPos);   // (2026-10-30: the weapon's own FOV)
    float NdotV = saturate(dot(-V, N));
    // (2026-11-21, Reflections.glsl FilmWetness) damp ground's reflection is the cube at the film's own roughness (0.61 at MaxRainWetness 0.5),
    // but its STRENGTH stays what the traced damp ground had (the Fresnel at most DampMaxRoughness 0.3): at that roughness the split-sum
    // Fresnel falls to a quarter at 5 degrees, the owner's test10 look ("no reflections"). A blurred sheen at the old strength, not a mirror.
    float fresnelRough = mix(roughness, min(roughness, DampMaxRoughness), SurfaceDamp);
    // Read AlbedoBuffer for both world and 1st-person weapon pixels; on weapons, non-PBR diffuse textures paint dark blued
    // steel at low linear values (~0.03-0.08), so remap perceptual brightness into realistic conductor F0 [0.30..0.90]
    // instead of letting dark diffuse albedo crush metal reflections down to dielectric 0.04.
    vec4 albedoSample = cb.wetFx2.z > 0.5 ? textureLod(sampler2D(t_albedo, POINT_S), ruv, 0.0) : vec4(0.0);
    bool albedoKnown = albedoSample.a > 0.5;
    float baseDielectricF0 = !pixelWeapon ? DecodeGBufferF0(ruv) : 0.04;
    if (SurfaceWetGloss > 0.0 || Wetness > 0.0) baseDielectricF0 = max(baseDielectricF0, 0.04 * max(SurfaceWetGloss, saturate(Wetness)));
    float dielectricF0 = mix(baseDielectricF0, 1.0, SurfaceMirror);
    vec3 f0 = vec3(dielectricF0);
    vec3 share;
    if (MaterialOn || GBufferOn) {
        vec3 metalF0;
        if (pixelWeapon) {
            // Non-PBR weapon diffuse textures paint dark blued steel at linear ~0.03-0.08 (sRGB 0.2-0.3); map perceptual
            // brightness into physical gunmetal conductor F0 [0.30..0.90] so blued steel reflects realistically without
            // turning black guns into chrome, while ReflMetalF0 still ramps smooth mirrors (< 0.06) to 0.95 silver.
            vec3 weaponTint;
            if (albedoKnown) {
                vec3 pCol = sqrt(clamp(albedoSample.rgb, vec3(0.0), vec3(1.0)));
                float pMax = max(pCol.r, max(pCol.g, pCol.b));
                vec3 rawHue = pMax > 1.0e-4 ? pCol / pMax : vec3(1.0);
                vec3 hue = mix(vec3(1.0), rawHue, smoothstep(0.08, 0.35, pMax));
                weaponTint = hue * mix(0.22, 0.85, pMax);
            } else {
                vec3 litColour = linearize(color.rgb);
                float top = max(litColour.r, max(litColour.g, litColour.b));
                vec3 rawHue = top > 1.0e-4 ? saturate(litColour / top) : vec3(1.0);
                vec3 hue = mix(vec3(1.0), rawHue, smoothstep(0.03, 0.20, top));
                weaponTint = hue * mix(0.25, 0.80, sqrt(saturate(top)));
            }
            metalF0 = ReflMetalF0(weaponTint, roughness);
        } else if (albedoKnown) {
            metalF0 = mix(max(albedoSample.rgb, vec3(0.02)), vec3(1.0), SurfaceMirror);
        } else if (SurfaceGBuffer) {
            // (2026-11-20 UnifiedReflections) a material from the G-buffer has its own base colour (COLOR1, linear): that, not a guess
            metalF0 = ReflMetalF0(saturate(textureLod(sampler2D(t_gbAlbedo, POINT_S), ruv, 0.0).rgb), roughness);
        } else {
            vec3 litColour = linearize(color.rgb);
            vec3 tint = saturate(litColour / max(max(litColour.r, max(litColour.g, litColour.b)), 1.0e-4));
            metalF0 = ReflMetalF0(tint, roughness);
        }
        f0 = mix(vec3(dielectricF0), metalF0, SurfaceMetal);
        share = saturate(amount * EnvironmentFresnelF0(fresnelRough, NdotV, f0)) * saturate(reflection.a);
    }
    else {
        share = saturate(amount * EnvironmentFresnelF0(fresnelRough, NdotV, f0)) * saturate(reflection.a);
    }
    vec3 reflected = Untame(reflection.rgb / max(reflection.a, 1.0e-4));
    if (any(isnan(reflected)) || any(isinf(reflected)) || isnan(reflection.a)) { o_color = color; return; }
    // REFL_SPECOCC_ON: SpecularOcclusion (world floor attenuation + mild roughness-aware occlusion on shadowed weapons).
    float specOcc = pixelWeapon ? cb.data4.w * 0.4 * smoothstep(0.15, 0.65, roughness) : cb.data4.w * (1.0 - SurfaceMirror);
    if (SpecOccOn && specOcc > 0.0) {
        float surfaceLight = luma(linearize(color.rgb));
        float reflectedLight = max(luma(reflected) * cb.layoutData.z, 1.0e-6);
        float occlusion = mix(1.0, saturate(surfaceLight / reflectedLight), specOcc);
        if (abs(DebugView - 8.0) < 0.5) { o_color = vec4(saturate(surfaceLight * 8.0), saturate(reflectedLight * 8.0), occlusion, 1.0); return; }
        share *= occlusion;
    }

    if (DebugView > 4.5) { o_color = vec4(delinearize(reflected), 1.0); return; }
    if (DebugView > 1.5) { o_color = vec4(1.0 - reflection.a, reflection.a, amount * 0.5, 1.0); return; }
    if (DebugView > 0.5) { o_color = vec4(delinearize(reflected * share), color.a); return; }

    vec3 lin = linearize(color.rgb);
    // 1. Subtract forward WetSkyReflection on wet surfaces so Reflections replaces it cleanly under SkylightingMode 0 & 1.
    vec3 forwardEnvSpec = vec3(0.0);
    if (cb.wetFx.x > 0.5 && SurfaceWetGloss > 0.0 && !IsWeapon(ruv)) {
        vec3 worldN = ViewToWorldDir(N);
        vec3 worldV = -normalize(toWorld(ruv));
        vec3 worldR = reflect(-worldV, worldN);
        float horizon = saturate(1.0 + 1.2 * dot(worldR, worldN));
        float wetRefl = WetFilmReflectance(saturate(dot(worldN, worldV)), SurfaceWetRough, SurfaceWetGloss) * horizon * horizon;
        forwardEnvSpec = WetSkyReflectionLinear(worldR) * wetRefl * saturate(reflection.a);
    }
    lin = max(lin - forwardEnvSpec, vec3(0.0));
    lin = MirrorLampHighlight(ruv, lin, surfPos, N, depth, share, roughness);
    // 2. Energy-conserving additive indirect reflection: metals keep 100% of lin (preserving direct sun & lamp GGX highlights);
    //    dielectrics attenuate only the diffuse body up to diffuseCeiling while keeping direct specular highlights un-dimmed.
    float reflectAdd = AddOn ? clamp(cb.data4.z, 0.0, 1.0) : 0.0;
    float nonMetal = clamp(1.0 - SurfaceMetal, 0.0, 1.0);
    vec3 diffuseCeiling = albedoKnown ? max(albedoSample.rgb * 2.0, vec3(0.25)) : vec3(1.0);
    vec3 bodyToDim = mix(min(lin, diffuseCeiling), lin, SurfaceMirror);
    vec3 attenShare = share * mix(nonMetal, 1.0, SurfaceMirror) * (1.0 - reflectAdd);
    lin = (lin - bodyToDim * attenShare) + reflected * share;
    if (SurfaceMirror > 0.0) lin += FlashlightHighlight(surfPos, N, roughness, f0) * attenShare;
    o_color = vec4(delinearize(lin), color.a);
}
