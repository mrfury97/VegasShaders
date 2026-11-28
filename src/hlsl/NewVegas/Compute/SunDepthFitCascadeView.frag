// SunDepthFit, CascadeView debug overlay as a Vulkan pass (PostTonemap, needs NVR DXVK).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;
layout(set = 1, binding = 1) uniform texture2D t_depth;
layout(set = 1, binding = 2) uniform texture2D t_blocks;

layout(set = 2, binding = 0, std140) uniform Constants {
    mat4 viewTransform;
    mat4 projectionTransform;
    vec4 reciprocalResolution;
    vec4 cameraData;
    vec4 info;
    vec4 splits;
    vec4 original;
    vec4 shadowNearCenter;
    vec4 shadowMiddleCenter;
    vec4 shadowFarCenter;
    vec4 shadowLodCenter;
    vec4 shadowForwardData;
} cb;

layout(push_constant) uniform Push {
    uint pointSampler;
    uint pad[3];
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

const float BlockPixels = 8.0;
const float SkyDepth = 0.98;
const float NoSurface = 1.0e9;

vec3 barColour(float eyeRange, vec4 splits) {
    return eyeRange < splits.x ? vec3(0.9, 0.2, 0.2)
         : eyeRange < splits.y ? vec3(0.2, 0.85, 0.2)
         : eyeRange < splits.z ? vec3(0.25, 0.4, 1.0)
         : eyeRange < splits.w ? vec3(0.95, 0.9, 0.2)
         : vec3(0.15, 0.15, 0.15);
}

vec3 toWorld(vec2 tex) {
    vec3 v = vec3(cb.viewTransform[0][2], cb.viewTransform[1][2], cb.viewTransform[2][2]);
    v += (1.0 / cb.projectionTransform[0][0] * (2.0 * tex.x - 1.0)) * vec3(cb.viewTransform[0][0], cb.viewTransform[1][0], cb.viewTransform[2][0]);
    v += (-1.0 / cb.projectionTransform[1][1] * (2.0 * tex.y - 1.0)) * vec3(cb.viewTransform[0][1], cb.viewTransform[1][1], cb.viewTransform[2][1]);
    return v;
}

void main() {
    vec3 sceneRgb = textureLod(sampler2D(t_rendered, NVR_SAMPLER(pc.pointSampler)), uv, 0.0).rgb;
    vec2 pixelV = floor(uv / cb.reciprocalResolution.xy);

    if (uv.x > 0.02 && uv.x < 0.98 && uv.y > 0.012 && uv.y < 0.042) {
        float t = (uv.x - 0.02) / 0.96;
        float eyeRange = t * t * max(cb.info.z, 1.0);
        if (uv.y > 0.03 && uv.y < 0.033) { o_color = vec4(0.0, 0.0, 0.0, 1.0); return; }
        if (uv.y < 0.03) {
            float nearMark = cb.info.w;
            float markX = 0.02 + 0.96 * sqrt(saturate(nearMark / max(cb.info.z, 1.0)));
            if (nearMark > 0.0 && abs(uv.x - markX) < cb.reciprocalResolution.x * 1.5) {
                o_color = vec4(1.0, 1.0, 1.0, 1.0);
                return;
            }
            o_color = vec4(barColour(eyeRange, cb.splits), 1.0);
            return;
        }
        o_color = vec4(barColour(eyeRange, cb.original) * 0.8, 1.0);
        return;
    }

    if (cb.info.x > 1.5) {
        vec2 gridV = ceil(1.0 / (cb.reciprocalResolution.xy * BlockPixels));
        vec2 cellV = floor(pixelV / BlockPixels);
        vec3 blockV = textureLod(sampler2D(t_blocks, NVR_SAMPLER(pc.pointSampler)), (cellV + 0.5) / gridV, 0.0).xyz;
        vec3 shownV = vec3(sqrt(saturate(blockV.x / max(cb.info.z, 1.0))));
        if (blockV.x > cb.info.y) shownV = vec3(1.0, 0.0, 1.0);
        if (blockV.y < NoSurface * 0.5 && blockV.y < cb.info.w) shownV = vec3(1.0, 0.5, 0.0);
        if (blockV.z > 0.5) shownV = mix(shownV, vec3(0.0, 1.0, 1.0), 0.6);
        o_color = vec4(mix(sceneRgb, shownV, 0.75), 1.0);
        return;
    }

    float eyeRange = textureLod(sampler2D(t_depth, NVR_SAMPLER(pc.pointSampler)), uv, 0.0).x * cb.cameraData.y;
    if (eyeRange <= 0.0 || eyeRange >= cb.cameraData.y * SkyDepth) {
        o_color = vec4(sceneRgb, 1.0);
        return;
    }
    vec3 posn = toWorld(uv) * eyeRange;
    vec4 radiiV = vec4(cb.shadowNearCenter.w, cb.shadowMiddleCenter.w, cb.shadowFarCenter.w, cb.shadowLodCenter.w);
    vec4 distancesV = vec4(length(posn - cb.shadowNearCenter.xyz), length(posn - cb.shadowMiddleCenter.xyz),
                           length(posn - cb.shadowFarCenter.xyz), length(posn - cb.shadowLodCenter.xyz));
    vec4 bandsV = max(0.1 * radiiV, min(vec4(cb.shadowForwardData.w), radiiV));
    vec3 tintRgb = vec3(0.45, 0.45, 0.45);
    float bandV = 0.0;
    if (distancesV.x < radiiV.x) { tintRgb = vec3(1.0, 0.25, 0.25); bandV = distancesV.x > radiiV.x - bandsV.x ? 1.0 : 0.0; }
    else if (distancesV.y < radiiV.y) { tintRgb = vec3(0.25, 1.0, 0.25); bandV = distancesV.y > radiiV.y - bandsV.y ? 1.0 : 0.0; }
    else if (distancesV.z < radiiV.z) { tintRgb = vec3(0.3, 0.45, 1.0); bandV = distancesV.z > radiiV.z - bandsV.z ? 1.0 : 0.0; }
    else if (distancesV.w < radiiV.w) { tintRgb = vec3(1.0, 0.95, 0.25); bandV = distancesV.w > radiiV.w - bandsV.w ? 1.0 : 0.0; }
    vec3 shownV = tintRgb * (0.3 + 0.7 * luma(sceneRgb));
    if (bandV > 0.5 && fract((pixelV.x + pixelV.y) / 10.0) < 0.5) shownV *= 0.65;
    o_color = vec4(mix(sceneRgb, shownV, 0.7), 1.0);
}
