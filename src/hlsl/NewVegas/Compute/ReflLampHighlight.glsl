// UNOFFICIAL 2026-11-08 (perf-reflfix3, the user: the lamp dots on mirrors and chrome "look weird") [Shaders.Reflections.Main]
// MirrorLampHighlight: on mirror-smooth and chrome pixels the reflection is laid over, the game's own highlight of each POINT lamp (its
// object shaders' GGX specular at roughness 0.02-0.1: tens to thousands of times the surface's own light in a spot a few pixels wide)
// stayed in the share the composite keeps and bloomed into a white dot. A real mirror shows a lamp only where the lamp has a glowing
// surface, and the RT reflection already shows those (glow geometry x GlowBrightness, PTEmissive). So, before the mix, the part of the
// lit colour that stands above both the ring of taps round the pixel (its third brightest: a spot smaller than the ring is a highlight, not
// the surface's texture) and 1.0 linear
// is scaled by MirrorLampHighlight, weighted by the reflection's share (in full from 0.25) and faded out from roughness 0.08 to 0.15 (the gold sphere's broad
// soft highlights stay; they are also wider than the ring). The sun's and the flashlight's glints are kept (keep: the reflected direction
// toward them). Pure: ReflectionsComposite.frag gathers the ring, the RT test host's check pass (Rt/ReflHighlightCheck.comp) runs the same.
#ifndef REFL_LAMP_HIGHLIGHT_INCLUDED
#define REFL_LAMP_HIGHLIGHT_INCLUDED

float ReflHighlightLuma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// The ring's level: the THIRD brightest of its taps (a pixel just beside a highlight has one or two taps in the highlight's tail; a
// pattern wider than the ring lights three or more)
void ReflHighlightRingAdd(float l, inout vec3 top) {
	if (l > top.x) top = vec3(l, top.xy);
	else if (l > top.y) top = vec3(top.x, l, top.y);
	else if (l > top.z) top.z = l;
}

// lin: the pixel's lit colour (linear); ringMax: the ring's level (ReflHighlightRingAdd's top.z); valid: how many taps were the same surface;
// share: the reflection's share (the composite's); roughness: the surface's; strength: 1 - MirrorLampHighlight; keep: 0..1, the sun's /
// the flashlight's glint here (left as it is)
vec3 ReflLampHighlightScale(vec3 lin, float ringMax, int valid, vec3 share, float roughness, float strength, float keep) {
	if (!(strength > 0.0) || valid < 4) return lin;
	// (the reflection's weight: in full from a share of 0.25: a highlight thousands of times the surface's light must go all the way, a
	// share-sized cut would leave it blooming; floors at F0 0.04 keep most of theirs)
	const float w = strength * (1.0 - smoothstep(0.08, 0.15, roughness)) * smoothstep(0.0, 0.25, max(share.r, max(share.g, share.b))) *
		(1.0 - clamp(keep, 0.0, 1.0));
	const float l = ReflHighlightLuma(lin);
	// (only what stands above 1.0 linear too: a dim mirror's own light and the patterns of a chrome's lit colour stay below it and are
	// left alone; a lamp's highlight, its long GGX tail included, is what climbs past it and blooms)
	const float excess = l - max(ringMax, 1.0);
	if (!(w > 0.0) || !(excess > 0.0) || !(l > 0.0)) return lin;
	return lin * (1.0 - w * excess / l);
}

#endif
