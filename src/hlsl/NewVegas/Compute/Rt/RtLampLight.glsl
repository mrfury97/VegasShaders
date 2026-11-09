// UNOFFICIAL 2026-11-07 (perf-reflfix2) one RT reflection lamp's light at a hit, as the game's shaders light it (the "orbs": white discs
// on the ceiling near each lamp in the RT test room's mirror, only in reflections). Included by ReflectionsRT.glsl (Resolve's estimate,
// RtShadeHit) and Rt/ReflectionsRTBody.glsl (the helper's key-lamp pick), and by the RT test host's check pass (Rt/RtLampLightCheck.comp).
// The lamp as Reflections.cpp RtGatherLamps packs it (src/effects/Reflections/RtReflLamps.h):
//   pos   xyz camera-relative position, w its reach (Spec.r)
//   col   rgb its colour as the game lights with it, LINEAR (the reflection's linear space, the screen's colours decoded): diffuse x dimmer
//         (at most 1 without HDR) x the LOD dimmer x the light scale (LightingScale), decoded together (x^2.2), x the shape's strength when
//         the physical shape is on; w the falloff: 0 the game's, > 0 LampFalloff's physical shape this frame (its 1 / Falloff^2), < 0 the
//         flashlight's (FlashlightDeferred.hlsl: -1 - its Falloff F)
// Falloff: the game's 1 - (d / r)^2 decoded like a colour (Pointlights.hlsl vanillaAtt; LinFalloff with FalloffMatch 1), or
// [Shaders.PBR.LampFalloff]'s shape (Includes/LampFalloff.hlsl LampShape: (1 - (d / r)^4)^2 / (1 + (d / r)^2 / Falloff^2)), which the
// object shaders light with in linear light. Before: the colour x dimmer UNCLAMPED decoded (a dimmer of 1.9, the room's lamps: 4.5x the
// game's light) and (1 - (d / r)^2)^2.
// The flashlight (2026-11-07, the flashlight team: its beam missing in mirrors with RT on): FlashlightDeferred.hlsl's falloff in linear
// light, (1 - (d/r)^2)^2 / (1 + F (d/r)^2) with Falloff F > 0, else 1 - (d/r)^2; its cone RtSpotCone squared (the pass's cone x cone).
#ifndef RT_LAMP_LIGHT_INCLUDED
#define RT_LAMP_LIGHT_INCLUDED

float RtLampFall(float d, float reach, float shape) {
	const float q = clamp((d * d) / max(reach * reach, 1.0e-6), 0.0, 1.0);   // (d / r)^2
	if (shape > 0.0) {
		float window = 1.0 - q * q;
		window *= window;
		return window / (1.0 + q * shape);
	}
	if (shape < 0.0) {   // the flashlight
		const float f = -shape - 1.0;
		return f > 0.0 ? (1.0 - q) * (1.0 - q) / (1.0 + f * q) : 1.0 - q;
	}
	return pow(1.0 - q, 2.2);
}

// The lamp's light (colour x falloff x N.L) at hitW (camera-relative) with the normal n (unit); 0 out of its reach
vec3 RtLampLight(vec4 pos, vec4 col, vec3 hitW, vec3 n) {
	const vec3 toLamp = pos.xyz - hitW;
	const float d = length(toLamp);
	if (!(d < pos.w)) return vec3(0.0);
	return col.rgb * (RtLampFall(d, pos.w, col.w) * max(dot(n, toLamp / max(d, 1.0e-3)), 0.0));
}

// A spot's cone (src/core/RtFlashlightCode.h, as RtPathTrace.comp LampSpot): code 1 + (angle x 512 + u) x 512 + v, the half-angle in
// degrees and the axis octahedral in 9 bits each; the share at the unit direction toPoint (lamp -> point): 1 inside half the cone, 0
// outside it, smooth between; 1 for code 0 (no cone)
float RtSpotCone(float code, vec3 toPoint) {
	if (code < 0.5) return 1.0;
	const uint c = uint(code - 1.0 + 0.5);
	const vec2 e = vec2(float((c >> 9u) & 511u), float(c & 511u)) / 511.0 * 2.0 - 1.0;
	vec3 a = vec3(e, 1.0 - abs(e.x) - abs(e.y));
	if (a.z < 0.0) a.xy = (1.0 - abs(a.yx)) * vec2(a.x >= 0.0 ? 1.0 : -1.0, a.y >= 0.0 ? 1.0 : -1.0);
	const float angle = radians(float(c >> 18u));
	return smoothstep(cos(angle), cos(0.5 * angle), dot(normalize(a), toPoint));
}

// The flashlight's light: the lamp's x its cone squared (FlashlightDeferred.hlsl att = falloff x cone x cone)
// (2026-11-08 perf-reflfix3, test 11: a big white orb round the reflected player's head) x RtSpotNear: nothing within 24 units of the
// light, full from 48: the flashlight sits on the player, and what a reflection ray meets right there is the player's own head / body (the
// falloff is 1 at d = 0 and part of the head lies inside the cone); the game's own pass never lights it there (its shadow map and the
// G-buffer: the player's head is not drawn in first person, and in third person the light leaves from in front of it)
float RtSpotNear(float d) { return smoothstep(24.0, 48.0, d); }
vec3 RtSpotLight(vec4 pos, vec4 col, float code, vec3 hitW, vec3 n) {
	const vec3 l = RtLampLight(pos, col, hitW, n);
	if (!(dot(l, l) > 0.0)) return l;
	const float s = RtSpotCone(code, normalize(hitW - pos.xyz));
	return l * (s * s * RtSpotNear(length(hitW - pos.xyz)));
}

#endif
