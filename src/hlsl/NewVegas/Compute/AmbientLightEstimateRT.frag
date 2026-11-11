// UNOFFICIAL 2026-10-28 (NVR DXVK fork, hybrid RT item 2; docs/next/FORK-RT-AO.md) [Shaders.AmbientOcclusion.RT]: AmbientLight's
// Estimate from the RT helper's traced visibility (RtAOTrace.comp, read back into NVR_RTAORaw) instead of the screen-space GTAO search:
// what is off screen, behind the first surface or thinner than the depth tells still occludes. Into NVR_AmbientLight0 (half resolution),
// in Estimate's place, with Estimate's output (rgb 0: no screen bounce; a the visibility after OcclusionStrength and the near/far fade),
// so the passes after it (blur X / Y, Accumulate's reprojected history, Apply) take it as they take Estimate's: they are the denoiser.
// pc.variant = [Shaders.AmbientOcclusion.RT] DebugView (1 the raw visibility, 3 the inside / hit-distance view, 8 (2026-11-09) the rays' tangent frame). effects/AmbientLight.cpp.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "AmbientLight.glsl"

layout(set = 1, binding = 0) uniform texture2D t_prep;       // NVR_ALPrep (POINT)
layout(set = 1, binding = 1) uniform texture2D t_rt;         // NVR_RTAORaw (the trace's output at the same half resolution; LINEAR)
layout(set = 1, binding = 2) uniform texture2D t_gi;         // NVR_RTGIRaw (2026-10-29, rt-ao-gi: the bounce light, rgb; read with variant bit 8)

layout(location = 0) out vec4 o_color;

vec4 EstimateRT(vec2 uv) {
    vec4 center = pointLod(t_prep, uv);
    vec3 origin = PositionFromViewZ(uv, center.x);
    float fade = smoothstep(NearFade, NearFade * 2.0, origin.z) * (1.0 - smoothstep(DrawDistance * 0.7, DrawDistance, origin.z));
    // UNOFFICIAL 2026-11-08 (alsky; tester: RT GI strength + AmbientLight DrawDistance 12000 drew a hard line across the ground) RtFarFade
    // (cb.sunFarFlags.w, [Shaders.AmbientOcclusion.RT]): the trace stops at endFade (RtAOInput.frag: no rays past it, the raw then reads
    // open with no bounce), but this fade went on to DrawDistance, so past 8000 units the traced occlusion and bounce light stopped dead.
    // On: both fade out over the last quarter before endFade, whatever DrawDistance is. Off: as before.
    if (cb.sunFarFlags.w > 0.5) fade *= 1.0 - smoothstep(0.75 * endFade, endFade, origin.z);
    if (fade <= 0.0) return vec4(0.0, 0.0, 0.0, 1.0);
    vec4 rt = linearLod(t_rt, uv);
    // (rt-ao-gi) variant bit 8: the RT bounce light into rgb, as ComputeGI's GI pass writes it (linear, x the fade); AmbientLight's GI
    // accumulate and apply take it from there. Point read: each texel's own ray (the blurs and the history average them).
    vec3 bounce = (pc.variant & 0x100u) != 0u ? pointLod(t_gi, uv).rgb * fade : vec3(0.0);
    uint view = pc.variant & 0xFFu;
    if (view == 1u || view == 4u || view == 5u || view == 8u) return vec4(bounce, rt.x);   // the raw RT visibility (AmbientLight's view 1 shows it); 4, 5: the input checks
    if (view == 3u) return vec4(bounce, rt.w);    // black: rays began inside something; grey: mean hit / Distance
    float visibility = pow(saturate(rt.x), 1.5 * OcclusionStrength);   // Estimate's mapping: OcclusionStrength means the same on both
    return vec4(bounce, saturate(mix(1.0, visibility, fade)));
}

void main() { o_color = EstimateRT(frameUV()); }
