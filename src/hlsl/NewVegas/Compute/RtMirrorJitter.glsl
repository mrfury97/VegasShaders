// UNOFFICIAL 2026-11-08 (perf-reflfix4, "fix blocky mirrors") [Shaders.Reflections.RT] MirrorJitter: a mirror-smooth pixel (roughness under
// 0.03, ReflectionsRayGen.frag's mirrorPixel) traced ONE ray through its half-resolution texel's centre, the same every frame, with no
// history: what the mirror shows was point-sampled at half resolution (stair-steps 2 output pixels wide on outlines and hard shadow edges).
// With MirrorJitter the ray leaves from a point of the texel that moves frame by frame (8 Halton (2, 3) points, the same for every texel:
// the C++ side's code), and the RT reflection denoiser keeps a short history on mirror pixels (virtual reprojection through the mirror,
// anti-lag): the mean of the 8 is the texel's area, filtered (anti-aliased), and steady. code: 0 off, else 1 + qx + 64 qy (q 0..63: the
// offset (q + 0.5) / 64 - 0.5 of a texel). RayGen and Resolve read it from rtc.rt5.w, the helper's pass (ReflectionsRTBody.glsl) from
// -frame.y. One function so the three passes move the same point.
#ifndef RT_MIRROR_JITTER_INCLUDED
#define RT_MIRROR_JITTER_INCLUDED
const float RtMirrorJitterRoughness = 0.03;   // (ReflectionsRayGen.frag mirrorPixel)
// the offset in texels for a mirror pixel of this roughness (0, 0 when off or not a mirror)
vec2 RtMirrorJitter(float code, float roughness) {
    if (!(code > 0.5) || !(roughness < RtMirrorJitterRoughness)) return vec2(0.0);
    const uint c = uint(code + 0.5) - 1u;
    return (vec2(float(c & 63u), float((c >> 6u) & 63u)) + 0.5) / 64.0 - 0.5;
}
#endif
