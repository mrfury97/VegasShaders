// Reflections technique 0 (Composite, full resolution, in the frame chain) as a Vulkan pass: Effects/Reflections.fx.hlsl
// CompositePS line for line (REFL_MATERIAL_ON / REFL_SPECOCC_ON / REFL_ADD_ON: cb.flags).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Reflections.glsl"
#include "ReflLampHighlight.glsl"   // (2026-11-08 perf-reflfix3) MirrorLampHighlight
#include "ReflMetalF0.glsl"         // (2026-11-08 perf-reflfix4) metal's F0: neutral silver on a mirror

// UNOFFICIAL 2026-11-08 (perf-reflfix3) MirrorLampHighlight: the ring round the pixel (8 taps at cb.mirrorLamp.y full-resolution pixels, the
// same surface: depth within 5 % + 4 units) for ReflLampHighlightScale, and what to keep: the sun's glint (the reflected direction within
// ~2-4 degrees of the sun, while it is up) and the flashlight's (toward its light). P: the surface (view space), N, V as main's.
vec3 MirrorLampHighlight(vec3 lin, vec3 P, vec3 N, float depth, vec3 share, float roughness) {
    if (!(cb.mirrorLamp.x > 0.0) || roughness >= 0.15) return lin;
    vec3 top = vec3(0.0);
    int valid = 0;
    const vec2 px = cb.reciprocalResolution.xy * cb.mirrorLamp.y;
    for (int k = 0; k < 8; k++) {
        const float a = 0.7853982 * float(k);
        const vec2 at = uv + vec2(cos(a), sin(a)) * px;
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

// UNOFFICIAL 2026-11-07 (perf-reflfix2, the flashlight team: with RT on the flashlight's spot faded on mirrors, chrome and gold) KeepFlashlight:
// the composite lays the reflection over a share of the surface's lit colour, and that colour holds the flashlight's highlight
// (FlashlightDeferred.hlsl adds it); no reflection ray ever sees the flashlight itself (a light, not a mesh), so the highlight went with
// the share. Its GGX highlight here (the surface's roughness, the share's F0, the flashlight pass's cone, falloff and colour; no shadow map
// or cookie), capped at flashLook.y: main adds that share of it back (the reflection replaces the environment's light, not the light's).
// view: the surface (view space, from the eye), N its normal, F0 the share's Fresnel colour
vec3 FlashlightHighlight(vec3 P, vec3 N, float roughness, vec3 F0) {
    if (!(cb.flashPos.w > 0.0) || cb.flashLook.z < 0.5) return vec3(0.0);   // (2026-11-08: flashPos is there for MirrorLampHighlight too; z KeepFlashlight)
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
    const float k = a * 0.5;   // (Smith-Schlick, GGX)
    const float G = (NdotL / (NdotL * (1.0 - k) + k)) * (NdotV / (NdotV * (1.0 - k) + k));
    const vec3 F = F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);
    const vec3 spec = D * G * F / (4.0 * NdotV) * cb.flashColor.rgb * (falloff * cone * cone);   // (x N.L / (4 N.L N.V): the N.L cancels)
    return min(spec, vec3(cb.flashLook.y));
}

void main() {
    vec4 color = textureLod(sampler2D(t_rendered, LINEAR_S), uv, 0.0);
    float depth = readDepthLod(uv);
    // DebugView 4: where the rain wets the ground.
    if (abs(DebugView - 4.0) < 0.5) {
        bool isFloor = ViewToWorldDir(ViewNormal(uv)).z > 0.75 && depth < DrawDistance && depth < farZ * 0.98;
        if (!isFloor) { o_color = vec4(color.rgb * 0.15, color.a); return; }
        float open = RoofMaskOn > 0.5 ? OpenAbove(uv, depth) : 1.0;
        o_color = open > 0.5 ? vec4(vec3(0.4 + 0.6 * saturate(Wetness)), 1.0) : vec4(0.5, 0.05, 0.05, 1.0);
        return;
    }
    // DebugView 3: the surroundings cube as seen from here, over the whole screen (an unbound cube reads black, as in D3D9).
    if (abs(DebugView - 3.0) < 0.5) {
        vec4 env = pc.pass != 0u ? EnvAlong(normalize(toWorld(uv)), 0.0) : vec4(0.0);
        o_color = vec4(delinearize(env.rgb * env.a), 1.0);
        return;
    }
    // REFL_MATERIAL_ON, DebugView 7: the material buffer (an unbound one reads 0, as in D3D9).
    if (MaterialOn && abs(DebugView - 7.0) < 0.5) {
        vec4 m = cb.flags.w > 0.5 ? textureLod(sampler2D(t_material, POINT_S), uv, 0.0) : vec4(0.0);
        o_color = m.b > 0.5 ? vec4(m.r, m.g, 0.6 * (1.0 - m.r), 1.0) : vec4(color.rgb * 0.15, color.a);
        return;
    }
    vec3 N = ViewNormal(uv);
    float roughness;
    float amount = Reflectivity(uv, depth, N, roughness);
    if (SurfaceWater) { o_color = color; return; }   // UNOFFICIAL 2026-10-31 RT water: the water shaders lay it on next frame
    if (amount <= 0.0) { o_color = DebugView > 0.0 ? vec4(color.rgb * 0.15, color.a) : color; return; }

    // Depth-aware bilinear upsample (half-resolution texel k stands for its centre; its depth is read again here).
    vec2 position = uv / LowTexel - 0.5;
    vec2 base = floor(position);
    vec2 fraction = position - base;
    float tolerance = depth * 0.03 + 4.0;
    vec4 reflection = vec4(0.0);
    float weights = 1.0e-5;
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 2; x++) {
            vec2 at = (base + vec2(x, y) + 0.5) * LowTexel;
            float tapDepth = readDepthLod(at - 0.5 * (LowTexel - cb.reciprocalResolution.xy));
            float weight = (x != 0 ? fraction.x : 1.0 - fraction.x) * (y != 0 ? fraction.y : 1.0 - fraction.y);
            weight /= 1.0 + abs(tapDepth - depth) / tolerance;
            reflection += textureLod(sampler2D(t_buffer, POINT_S), at, 0.0) * weight;
            weights += weight;
        }
    }
    reflection /= weights;

    vec3 V = normalize(SurfacePosition(uv, depth));   // (2026-10-30: the weapon's own FOV)
    vec3 share;
    // UNOFFICIAL 2026-11-05 [Shaders.Reflections.Main] Mirror (cb.grid3.w, 0..1): the non-metal F0, 0.04 (stone, tile, polished floors:
    // ~4 % of the reflection looking down, so only bright lamps show) up to 1 (a true mirror: people and objects show at any angle).
    float dielectricF0 = mix(0.04, 1.0, SurfaceMirror);   // (2026-11-06: per surface, MirrorSmoothOnly; Reflections.glsl)
    vec3 f0 = vec3(0.04);   // (2026-11-07 perf-reflfix2: the share's F0, for the flashlight's highlight)
    if (MaterialOn) {
        // MaterialAware: metal reflects in its own colour (F0 = its albedo, guessed from the lit colour's hue), the rest at the F0 above.
        // (2026-11-08 perf-reflfix4) a mirror-smooth metal in neutral silver, not its lit colour's hue (that is the lamps': the mirror
        // turned the reflected room orange under warm lamps): ReflMetalF0.glsl
        vec3 litColour = linearize(color.rgb);
        vec3 tint = saturate(litColour / max(max(litColour.r, max(litColour.g, litColour.b)), 1.0e-4));
        f0 = mix(vec3(dielectricF0), ReflMetalF0(tint, roughness), SurfaceMetal);
        share = saturate(amount * EnvironmentFresnelF0(roughness, saturate(dot(-V, N)), f0)) * saturate(reflection.a);
    }
    else if (SurfaceMirror > 0.0) { f0 = vec3(dielectricF0); share = saturate(amount * EnvironmentFresnelF0(roughness, saturate(dot(-V, N)), vec3(dielectricF0))) * saturate(reflection.a); }
    else share = vec3(saturate(amount * EnvironmentFresnel(roughness, saturate(dot(-V, N)))) * saturate(reflection.a));
    vec3 reflected = Untame(reflection.rgb / max(reflection.a, 1.0e-4));
    if (any(isnan(reflected)) || any(isinf(reflected)) || isnan(reflection.a)) { o_color = color; return; }   // (UNOFFICIAL 2026-10-30)
    // REFL_SPECOCC_ON: SpecularOcclusion.
    // (2026-11-05: Mirror turns it off as it rises: a mirror, above all a black polished one, is darker than what it shows, so the
    // occlusion took people and objects out of the demo floor's reflection entirely and left only the lamps.)
    float specOcc = cb.data4.w * (1.0 - SurfaceMirror);
    if (SpecOccOn && specOcc > 0.0) {
        float surfaceLight = luma(linearize(color.rgb));
        float reflectedLight = max(luma(reflected) * cb.layoutData.z, 1.0e-6);
        float occlusion = mix(1.0, saturate(surfaceLight / reflectedLight), specOcc);
        // DebugView 8 (diagnostic): red the surface's own light, green the reflection's (x Level), both x 8; blue the factor.
        if (abs(DebugView - 8.0) < 0.5) { o_color = vec4(saturate(surfaceLight * 8.0), saturate(reflectedLight * 8.0), occlusion, 1.0); return; }
        share *= occlusion;
    }

    if (DebugView > 4.5) { o_color = vec4(delinearize(reflected), 1.0); return; }   // 5, 6: the trace's diagnostic colours as they are
    if (DebugView > 1.5) { o_color = vec4(1.0 - reflection.a, reflection.a, amount * 0.5, 1.0); return; }   // red: nothing found
    if (DebugView > 0.5) { o_color = vec4(delinearize(reflected * share), color.a); return; }

    vec3 lin = linearize(color.rgb);
    lin = MirrorLampHighlight(lin, SurfacePosition(uv, depth), N, depth, share, roughness);   // (2026-11-08 perf-reflfix3)
    // (2026-11-07 perf-reflfix2) KeepFlashlight: the flashlight's highlight, of which the mix below takes away the reflection's share
    const vec3 highlight = FlashlightHighlight(SurfacePosition(uv, depth), N, roughness, f0);
    // REFL_ADD_ON: ReflectAdd.
    if (AddOn && cb.data4.z > 0.0) lin = lin * (1.0 - share * (1.0 - cb.data4.z)) + reflected * share + highlight * share * (1.0 - cb.data4.z);
    else lin = mix(lin, reflected, share) + highlight * share;
    o_color = vec4(delinearize(lin), color.a);
}
