// CoverMap (needs NVR DXVK): shared constants and helpers matching Effects/CoverMap.fx.hlsl & Includes/CoverMap.hlsl.
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 2, binding = 0, std140) uniform Constants {
    mat4 orthoInverse;          // TESR_CoverMapOrthoInverse
    mat4 shadowOrtho;           // TESR_ShadowCameraToLightTransformOrtho
    mat4 viewTransform;         // TESR_ViewTransform
    mat4 projectionTransform;   // TESR_ProjectionTransform
    vec4 update;                // TESR_CoverMapUpdate
    vec4 origin;                // TESR_CoverMapOrigin
    vec4 shift;                 // TESR_CoverMapShift
    vec4 rates;                 // TESR_CoverMapRates
    vec4 snowRates;             // TESR_CoverMapSnowRates
    vec4 sun;                   // TESR_CoverMapSun
    vec4 shade;                 // TESR_CoverMapShade
    vec4 start;                 // TESR_CoverMapStart
    vec4 footA;                 // TESR_CoverMapFootA
    vec4 footB;                 // TESR_CoverMapFootB
    vec4 debug;                 // TESR_CoverMapDebug
    vec4 data;                  // TESR_CoverMapData
    vec4 window;                // TESR_CoverMapWindow
    vec4 globalVal;             // TESR_CoverMapGlobal
    vec4 trail;                 // TESR_CoverMapTrail
    vec4 reciprocalResolution;  // TESR_ReciprocalResolution
    vec4 cameraData;            // TESR_CameraData (x nearZ, y farZ)
    vec4 renderScale;           // TESR_RenderScale (xy scale, zw 1/scale)
} cb;

const float HeightRange = 65536.0;
const float HeightNone = 1.0 / 65535.0;

float EncodeHeight(float h) { return clamp((h + cb.shade.z) / HeightRange + 0.5, 2.0 * HeightNone, 1.0); }
float DecodeHeight(float a) { return (a - 0.5) * HeightRange - cb.shade.z; }
float HeightKnown(float a) { return a > HeightNone ? 1.0 : 0.0; }

float readDepthLod(texture2D tDepth, uint samp, vec2 ruv) {
    return textureLod(sampler2D(tDepth, NVR_SAMPLER(samp)), ruv, 0.0).x * cb.cameraData.y;
}

vec3 toWorld(vec2 ruv) {
    vec2 s = ruv * cb.renderScale.zw;
    vec3 v = vec3(cb.viewTransform[0][2], cb.viewTransform[1][2], cb.viewTransform[2][2]);
    v += (1.0 / cb.projectionTransform[0][0] * (2.0 * s.x - 1.0)) * vec3(cb.viewTransform[0][0], cb.viewTransform[1][0], cb.viewTransform[2][0]);
    v += (-1.0 / cb.projectionTransform[1][1] * (2.0 * s.y - 1.0)) * vec3(cb.viewTransform[0][1], cb.viewTransform[1][1], cb.viewTransform[2][1]);
    return v;
}

vec3 projectPosition(vec3 posn) {
    vec4 proj = cb.projectionTransform * vec4(posn, 1.0);
    proj.xyz /= proj.w;
    proj.x = proj.x * 0.5 + 0.5;
    proj.y = 0.5 - 0.5 * proj.y;
    proj.xy *= cb.renderScale.xy;
    return proj.xyz;
}
