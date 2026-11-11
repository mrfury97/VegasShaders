// SunShadows' contact-shadow march (first pass of techniques 0, ForwardContactShadows, ContactFast, ForwardContactShadowsFast)
// as a Vulkan pass: Effects/SunShadows.fx.hlsl ScreenSpaceShadowFast line for line, which is ScreenSpaceShadow with the far and
// facing-away returns as one branch around the march (the same value either way; the D3D9 compiler had flattened ScreenSpaceShadow's
// returns into selects). Here that layout is needed too: the blue noise is read with implicit derivatives, so in uniform flow.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "SunShadows.glsl"

// returns a semi random vec3 between 0 and 1 based on the given seed (blue noise)
vec3 random(vec2 seed) {
    return texture(sampler2D(t_noise, NVR_SAMPLER(pc.noiseIndex)), (seed / 256.0 + 0.5) / cb.reciprocalResolution.xy).xyz;
}

void main() {
    vec4 color = sampleShadow(uv);
    if (cb.screenSpaceData.x == 0.0) { o_color = vec4(1.0, color.g, 0.0, 1.0); return; }
    vec3 pos = reconstructPosition(uv);
    float pixelZ = pos.z;   // (2026-11-09 review 7.2.3: the fades go by the pixel's depth, not the marched ray's end)
    bool isFar = pos.z > SSS_MAXDEPTH;
    float NdotL = dot(GetNormal(uv), normalize(cb.viewSpaceLightDir.xyz));
    float facing = saturate(NdotL * 8.0);
    float bias = max(0.01, pos.z * 0.002);
    vec3 random3 = random(uv);
    float rand = mix(min(0.8, pos.z / SSS_MAXDEPTH), 1.0, random3.r);
    float depth = getHomogenousDepth(uv) / farZ;
    vec3 marchStep = pows(depth, 0.6) * (SSS_DIST / float(SSS_STEPNUM)) * cb.viewSpaceLightDir.xyz * rand;
    vec4 result = vec4(1.0, color.g, 0.0, 1.0);
    if (!isFar && !(facing <= 0.0)) {
        float occlusion = 0.0;
        float total = 0.0;
        for (float i = 1.0; i < float(SSS_STEPNUM); i += 2.0) {
            float step1 = i;
            float step2 = i + 1.0;
            vec3 pos1 = pos + step1 * marchStep;
            vec3 pos2 = pos1 + step2 * marchStep;
            vec2 depth = vec2(pos1.z, pos2.z);
            vec2 depthCompare = vec2(
                readDepthLod(projectPosition(pos1).xy),
                readDepthLod(projectPosition(pos2).xy));
            vec2 depthDelta = depth - depthCompare;
            occlusion += ((depthDelta.x > bias && depthDelta.x < SSS_THICKNESS) ? 1.0 : 0.0) / step1;
            occlusion += ((depthDelta.y > bias && depthDelta.y < SSS_THICKNESS) ? 1.0 : 0.0) / step2;
            pos = pos2;
            total += 1.0 / step1 + 1.0 / step2;
        }
        occlusion = pows(occlusion / total, 0.3) * facing * ContactNearFade(pixelZ);   // (NearFade, 2026-10-27)
        color.r = mix(1.0 - occlusion, 1.0, hlslSmoothstep(SSS_MAXDEPTH * 0.8, SSS_MAXDEPTH, pixelZ));
        result = color;
    }
    o_color = result;
}
