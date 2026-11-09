// UNOFFICIAL 2026-11-08 (perf-reflfix4, test 12: the whole reflected room orange-sepia in the RT test room's mirror under the warm lamps,
// neutral under the cool Pip-Boy light) a metal's reflection colour (F0). MaterialAware took it from the HUE of the metal's own lit colour
// (lit / its brightest channel: ReflectionsComposite.frag, and Effects/Reflections.fx.hlsl's D3D9 composite). On a mirror that lit colour
// is the lamps' light and highlights, not a colour of its own (the test room's vs_mirror01: a 4 x 4 near-black diffuse, metal 1, roughness
// 0.02), so the mirror reflected the room in the lamps' colour. Polished chrome likewise. Now a metal smooth enough to be a mirror
// (roughness up to 0.06) reflects with a neutral silver F0 (0.95: polished silver / aluminium); from 0.06 to 0.15 it blends into the given
// colour, which rough coloured metal (the test room's gold at 0.2, the material rules' colouredmetal 0.2-0.5) keeps in full.
// colour: the composite's guess from the lit colour, or the RT hit's own texture colour (ReflectionsRTBody.glsl MetalBounce: a mirror's
// near-black texture would make a mirror seen in a mirror black). The D3D9 composite has the same rule in HLSL (ReflMetalF0 there).
#ifndef REFL_METAL_F0_INCLUDED
#define REFL_METAL_F0_INCLUDED
const float ReflMirrorSilver = 0.95;
vec3 ReflMetalF0(vec3 colour, float roughness) {
    return mix(vec3(ReflMirrorSilver), clamp(colour, vec3(0.0), vec3(1.0)), smoothstep(0.06, 0.15, roughness));
}
#endif
