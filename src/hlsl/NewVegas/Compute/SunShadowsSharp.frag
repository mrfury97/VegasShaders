// SunShadows' sharp contact shadows (technique ForwardContactSharp, first pass) as a Vulkan pass: Effects/SunShadows.fx.hlsl
// SharpContactShadow line for line.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "SunShadows.glsl"

#define SHARP_STEPS 16

void main() {
    vec2 ruv = toRectUV(uv);
    vec4 color = sampleShadow(ruv);
    if (cb.screenSpaceData.x == 0.0) { o_color = vec4(1.0, color.g, 0.0, 1.0); return; }
    vec3 pos = reconstructPosition(uv);
    if (pos.z > SSS_MAXDEPTH) { o_color = vec4(1.0, color.g, 0.0, 1.0); return; }
    vec3 light = normalize(cb.viewSpaceLightDir.xyz);
    float facing = saturate(dot(GetNormal(ruv), light) * 8.0);
    if (facing <= 0.0) { o_color = vec4(1.0, color.g, 0.0, 1.0); return; }
    float bias = max(0.01, pos.z * 0.002);

    float rayLength = pows(getHomogenousDepth(uv) / farZ, 0.6) * SSS_DIST * 2.0;
    if (light.z < 0.0) rayLength = min(rayLength, max(pos.z - 2.0 * nearZ, 0.0) / -light.z);   // stay in front of the camera
    vec3 endPos = pos + light * rayLength;
    vec2 endUV = projectPosition(endPos).xy;
    float invZ0 = 1.0 / pos.z, invZ1 = 1.0 / max(endPos.z, nearZ);

    float occlusion = 0.0, total = 0.0;
    for (int i = 1; i <= SHARP_STEPS; i++) {
        float t = float(i) / float(SHARP_STEPS);
        float rayZ = 1.0 / mix(invZ0, invZ1, t);
        float delta = rayZ - readDepthLod(toRectUV(mix(uv, endUV, t)));
        float w = 1.0 / (1.0 + t * 4.0);                    // nearer steps count more, as the old 1 / step
        occlusion += ((delta > bias && delta < SSS_THICKNESS) ? 1.0 : 0.0) * w;
        total += w;
    }
    occlusion = pows(occlusion / total, 0.3) * facing * ContactNearFade(pos.z);   // (NearFade, 2026-10-27)
    o_color = vec4(mix(1.0 - occlusion, 1.0, hlslSmoothstep(SSS_MAXDEPTH * 0.8, SSS_MAXDEPTH, pos.z)), color.g, 0.0, 1.0);
}
