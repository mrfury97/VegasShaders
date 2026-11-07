// Reflections technique 0 (Composite, full resolution, in the frame chain) as a Vulkan pass: Effects/Reflections.fx.hlsl
// CompositePS line for line (REFL_MATERIAL_ON / REFL_SPECOCC_ON / REFL_ADD_ON: cb.flags).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Reflections.glsl"

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
    if (MaterialOn) {
        // MaterialAware: metal reflects in its own colour (F0 = its albedo, guessed from the lit colour's hue), the rest at the F0 above.
        vec3 litColour = linearize(color.rgb);
        vec3 tint = saturate(litColour / max(max(litColour.r, max(litColour.g, litColour.b)), 1.0e-4));
        share = saturate(amount * EnvironmentFresnelF0(roughness, saturate(dot(-V, N)), mix(vec3(dielectricF0), tint, SurfaceMetal))) * saturate(reflection.a);
    }
    else if (SurfaceMirror > 0.0) share = saturate(amount * EnvironmentFresnelF0(roughness, saturate(dot(-V, N)), vec3(dielectricF0))) * saturate(reflection.a);
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
    // REFL_ADD_ON: ReflectAdd.
    if (AddOn && cb.data4.z > 0.0) lin = lin * (1.0 - share * (1.0 - cb.data4.z)) + reflected * share;
    else lin = mix(lin, reflected, share);
    o_color = vec4(delinearize(lin), color.a);
}
