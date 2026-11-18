// SnowAccumulation as Vulkan passes (needs NVR DXVK): Effects\SnowAccumulation.fx.hlsl line for line, the five passes of its
// technique (effects/SnowAccumulation.cpp ExtRender). mode: 0 SnowCoverage, 1 Scale (x4), 2 DepthBlur (offsetMask: its direction),
// 3 Snow. The matrices reach the effect through ID3DXEffect::SetMatrix, so HLSL M[i][j] is the D3DXMATRIX element (i+1, j+1). The
// constants block holds the matrices as stored (memory rows are the GLSL columns): M[i][j] here is the HLSL M[i][j], mul(v, M) is
// M * v and mul(M, v) is v * M.
// The D3D9 technique clips pixels in Scale and DepthBlur and leaves the destination holding its input there (the frame chain
// pre-fills it); those branches return the input directly.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;     // TESR_RenderedBuffer: the current image (LINEAR, CLAMP)
layout(set = 1, binding = 1) uniform texture2D t_source;       // TESR_SourceBuffer: the frame before the effect (LINEAR, CLAMP)
layout(set = 1, binding = 2) uniform texture2D t_depth;        // TESR_DepthBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 3) uniform texture2D t_ortho;        // TESR_OrthoMapBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 4) uniform texture2D t_normals;      // TESR_NormalsBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 5) uniform texture2D t_pointShadow;  // TESR_PointShadowBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 6) uniform texture2D t_snowNorm;     // Precipitations\snow_NRM.dds (LINEAR mips, WRAP)
layout(set = 1, binding = 7) uniform texture2D t_blueNoise;    // Effects\bluenoise256.dds (LINEAR mips, WRAP)
layout(set = 1, binding = 8) uniform texture2D t_captured;     // UNOFFICIAL 2026-10-28 TESR_CapturedNormalsBuffer (read texel by texel)

layout(set = 2, binding = 0, std140) uniform Consts {
    mat4 shadowToLight;             // TESR_ShadowCameraToLightTransformOrtho
    mat4 projection;                // TESR_ProjectionTransform
    mat4 invProjection;             // TESR_InvProjectionTransform
    mat4 view;                      // TESR_ViewTransform
    mat4 invView;                   // TESR_InvViewTransform
    vec4 cameraData;                // TESR_CameraData (x near, y far)
    vec4 cameraPosition;            // TESR_CameraPosition
    vec4 reciprocalResolution;      // TESR_ReciprocalResolution
    vec4 sunDirection;              // TESR_SunDirection
    vec4 sunColor;                  // TESR_SunColor
    vec4 sunAmbient;                // TESR_SunAmbient
    vec4 waterSettings;             // TESR_WaterSettings
    vec4 shadowFade;                // TESR_ShadowFade
    vec4 shadowScreenSpaceData;     // TESR_ShadowScreenSpaceData
    vec4 orthoData;                 // TESR_OrthoData
    vec4 snowParams;                // TESR_SnowAccumulationParams (x BlurNormDropThreshhold, y BlurRadiusMultiplier, z SunPower, w SnowAmount)
    vec4 snowAccColor;              // TESR_SnowAccumulationColor
    vec4 shadowLightPosition[12];   // TESR_ShadowLightPosition
    vec4 lightPosition[12];         // TESR_LightPosition
    vec4 lightColor[24];            // TESR_LightColor
    vec4 capturedData;              // UNOFFICIAL 2026-10-28 TESR_CapturedNormalsData (x 1 = this frame's, y z the distance match, w least cosine)
    vec4 snowExtra;                 // TESR_SnowAccumulationExtra (x CapturedNormals, y CapturedNormalsDebugView)
} c;

layout(push_constant) uniform Push {
    vec2 offsetMask;    // 0: DepthBlur's direction (OffsetMaskH or OffsetMaskV)
    uint sClamp;        // 8: LINEAR, CLAMP (heap index, filled in by the extension)
    uint sWrap;         // 12: LINEAR mips, WRAP
    uint mode;          // 16: 0 SnowCoverage, 1 Scale, 2 DepthBlur, 3 Snow
    uint pad;           // 20
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

#define SAMP_CLAMP(t) sampler2D(t, NVR_SAMPLER(pc.sClamp))
#define SAMP_WRAP(t) sampler2D(t, NVR_SAMPLER(pc.sWrap))

#define farZ (c.cameraData.y)
#define nearZ (c.cameraData.x)

// Constants of SnowAccumulation.fx.hlsl
const float noiseSize = 200.0;
const float thickness = 0.002;
const float sparkleScale = 200.0;

// HLSL helpers: lerp(a, b, t) = a + (b - a) * t (the same rounding as the HLSL), shade, shades, smoothstep for any edges
float lerpH(float a, float b, float t) { return a + (b - a) * t; }
float shadeH(vec3 n, vec3 l) { return max(dot(n, l), 0.0); }
float shadesH(vec3 n, vec3 l) { return saturate(dot(n, l)); }
float shadeH4(vec4 n, vec4 l) { return max(dot(n, l), 0.0); }	// shade() on float4 arguments: a 4-component dot, as the HLSL
float smoothstepH(float a, float b, float x) {
    float t = clamp((x - a) / (b - a), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// Depth.hlsl
float projectedDepthFromLinear(float linear01) {
    float viewZ = max(linear01 * farZ, 1.0e-6);
    return nearZ * (farZ - viewZ) / (viewZ * (farZ - nearZ));
}

float readDepth(vec2 coord) {
    return texture(SAMP_CLAMP(t_depth), coord).x * farZ;
}

vec4 reconstructWorldPosition(vec2 coord, out float viewDepth) {
    float x = coord.x * 2.0 - 1.0;
    float y = (1.0 - coord.y) * 2.0 - 1.0;
    float z = projectedDepthFromLinear(texture(SAMP_CLAMP(t_depth), coord).x);
    vec4 clipSpace = vec4(x, y, z, 1.0);

    vec4 viewSpace = c.invProjection * clipSpace;	// mul(clipSpace, TESR_InvProjectionTransform)
    viewSpace /= viewSpace.w;

    viewDepth = viewSpace.z;

    vec4 worldSpace = c.invView * viewSpace;	// mul(viewSpace, TESR_InvViewTransform)
    return vec4(worldSpace.xyz, 1.0);
}

vec3 toWorld(vec2 tex) {
    vec3 v = vec3(c.view[0][2], c.view[1][2], c.view[2][2]);
    v += (1.0 / c.projection[0][0] * (2.0 * tex.x - 1.0)) * vec3(c.view[0][0], c.view[1][0], c.view[2][0]);
    v += (-1.0 / c.projection[1][1] * (2.0 * tex.y - 1.0)) * vec3(c.view[0][1], c.view[1][1], c.view[2][1]);
    return v;
}

// Normals.hlsl
vec3 getWorldNormal(vec2 coord) {
    vec4 normal = vec4(texture(SAMP_CLAMP(t_normals), coord).xyz * 2.0 - 1.0, 1.0);
    return (normal * c.view).xyz;	// mul(TESR_ViewTransform, normal)
}

// Blur.hlsl
const float blurWeights[12] = float[12](0.057424882, 0.058107773, 0.061460144, 0.071020611, 0.088092873, 0.106530916,
    0.106530916, 0.088092873, 0.071020611, 0.061460144, 0.058107773, 0.057424882);
const float blurSteps[12] = float[12](-6.0, -5.0, -4.0, -3.0, -2.0, -1.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0);

// SnowAccumulation.fx.hlsl: GetOrtho
float getOrtho(vec4 worldPos) {
    // get puddle mask from ortho map
    vec4 orthoPos = c.shadowToLight * worldPos;	// mul(worldPos, TESR_ShadowCameraToLightTransformOrtho)

    // apply perspective (perspective division) and convert from -1/1 to range to 0/1 (shadowMap range);
    orthoPos.xyz /= orthoPos.w;
    orthoPos.x = orthoPos.x * 0.5 + 0.5;
    orthoPos.y = orthoPos.y * -0.5 + 0.5;

    bool outOfBounds = (saturate(orthoPos.x) != orthoPos.x) || (saturate(orthoPos.y) != orthoPos.y);

    float ortho = texture(SAMP_CLAMP(t_ortho), orthoPos.xy).r; // ortho height

    bool aboveGround = orthoPos.z < ortho + thickness;
    bool belowGround = orthoPos.z > ortho - thickness;

    return (outOfBounds || (belowGround && aboveGround)) ? 1.0 : 0.0;
}

// The same noise as SnowAccumulation.fx.hlsl (snNoise, snHash).
// 3D value noise: a random value in [0, 1) at each whole-number lattice point, blended between the 8 corners of the cell
// with a smoothstep curve. Output in [0, 1), mean 0.5, one lattice cell per unit. Written for Vegas Shaders (2026-11-15);
// it replaces upstream's copy of a Shadertoy noise under CC BY-NC-SA 3.0 (not GPL-compatible), with the same range and scale.
// Lattice hash: Dave Hoskins' "Hash without Sine" hash13, MIT, Copyright (c) 2014 David Hoskins (shadertoy.com/view/4djSRW).
float hashH(vec3 p3) {
    p3 = fract(p3 * 0.1031);
    p3 += dot(p3, p3.zyx + 31.32);
    return fract((p3.x + p3.y) * p3.z);
}

float snowNoise(vec3 x) {
    vec3 i = floor(x);
    vec3 u = fract(x);
    u = u * u * (3.0 - 2.0 * u);
    float z0 = lerpH(lerpH(hashH(i), hashH(i + vec3(1, 0, 0)), u.x),
                     lerpH(hashH(i + vec3(0, 1, 0)), hashH(i + vec3(1, 1, 0)), u.x), u.y);
    float z1 = lerpH(lerpH(hashH(i + vec3(0, 0, 1)), hashH(i + vec3(1, 0, 1)), u.x),
                     lerpH(hashH(i + vec3(0, 1, 1)), hashH(i + vec3(1, 1, 1)), u.x), u.y);
    return lerpH(z0, z1, u.z);
}

// returns a point light contribution with no shadow map sampling
float getPointLightContribution(vec4 worldPos, vec4 lightPos, vec4 normal) {
    if (lightPos.w == 0.0) return 0.0;

    vec3 lightDir = lightPos.xyz - worldPos.xyz;
    float dist = length(lightDir) / lightPos.w; // normalize distance over light range

    // radius based attenuation based on https://lisyarus.github.io/blog/graphics/2022/07/30/point-light-attenuation.html
    float s = dist * dist;
    float atten = saturate((1.0 - s) / (1.0 + s));

    lightDir = normalize(lightDir); // normalize
    float diffuse = dot(lightDir, normal.xyz);

    vec3 eyeDir = normalize(c.cameraPosition.xyz - worldPos.xyz);
    vec3 halfway = normalize(lightDir + eyeDir);
    float spec = pows(shadeH(halfway, normal.xyz), 20.0);

    return saturate((diffuse + spec) * atten);
}

// Compute the areas of the screen that must be covered with snow (SnowCoverage)
vec4 snowCoverage(vec2 coordIn) {
    // compute at quarter scale
    vec2 q = coordIn * 4.0;
    if (q.x > 1.0 || q.y > 1.0) return vec4(1.0);

    float depth;
    vec4 world = reconstructWorldPosition(q, depth);

    if (depth > c.orthoData.x) return vec4(1.0); // early out for the sky pixels

    // sample an average ortho
    float orthoRadius = c.snowParams.x; // radius of the area to sample ortho from
    float ortho = getOrtho(world);
    ortho += getOrtho(world + vec4(-1.0, 0.0, 0.0, 0.0) * orthoRadius);
    ortho += getOrtho(world + vec4(1.0, 0.0, 0.0, 0.0) * orthoRadius);
    ortho += getOrtho(world + vec4(0.0, -1.0, 0.0, 0.0) * orthoRadius);
    ortho += getOrtho(world + vec4(0.0, 1.0, 0.0, 0.0) * orthoRadius);
    ortho += getOrtho(world + vec4(-0.7, -0.7, 0.0, 0.0) * orthoRadius);
    ortho += getOrtho(world + vec4(-0.7, 0.7, 0.0, 0.0) * orthoRadius);
    ortho += getOrtho(world + vec4(0.7, -0.7, 0.0, 0.0) * orthoRadius);
    ortho += getOrtho(world + vec4(0.7, 0.7, 0.0, 0.0) * orthoRadius);
    ortho += getOrtho(world + vec4(-0.3, 0.0, 0.0, 0.0) * orthoRadius);
    ortho += getOrtho(world + vec4(0.3, 0.0, 0.0, 0.0) * orthoRadius);
    ortho += getOrtho(world + vec4(0.0, -0.3, 0.0, 0.0) * orthoRadius);
    ortho += getOrtho(world + vec4(0.0, 0.3, 0.0, 0.0) * orthoRadius);
    ortho /= 13.0;

    ortho = smoothstepH(0.1, 0.9, ortho); // reduce glitches by removing outlier values
    ortho = lerpH(ortho, 1.0, smoothstepH(0.6 * c.orthoData.x, c.orthoData.x, depth)); // fade out ortho with distance

    return vec4(ortho, ortho, ortho, 1.0);
}

// Scale(TESR_RenderedBuffer, 4): the clip leaves the destination with its input (the current image) where uv >= 1
vec4 scalePass(vec2 coordIn) {
    vec2 scaled = coordIn / 4.0; // scale the uv by wanted scale
    if (!(scaled.x < 1.0 && scaled.y < 1.0)) return texture(SAMP_CLAMP(t_rendered), coordIn);
    return texture(SAMP_CLAMP(t_rendered), scaled);
}

// DepthBlur(TESR_RenderedBuffer, offsetMask, TESR_SnowAccumulationParams.y, 3500, TESR_OrthoData.x): clip(endFade - depth1)
// leaves the destination with its input, so beyond endFade the input is returned.
vec4 depthBlur(vec2 coordIn, vec2 offsetMaskIn) {
    const float blurRadius = c.snowParams.y;
    const float endFade = c.orthoData.x;

    float weightSum = 0.114725602;
    vec4 center = texture(SAMP_CLAMP(t_rendered), coordIn);
    vec4 color1 = center * weightSum;

    float depth1 = readDepth(coordIn);
    if (depth1 > endFade) return center;

    float depthDrop = 3500.0;
    depthDrop *= (depth1 / farZ);

    for (int i = 0; i < 12; i++) {
        vec2 offset = vec2(blurSteps[i] * c.reciprocalResolution.x, blurSteps[i] * c.reciprocalResolution.y);
        vec2 uv2 = coordIn + (offset * offsetMaskIn) * blurRadius;
        vec4 color2 = texture(SAMP_CLAMP(t_rendered), uv2);
        float depth2 = readDepth(uv2);
        float diff = abs(depth1 - depth2);

        float useForBlur = float(diff <= depthDrop);
        color1 += blurWeights[i] * color2 * useForBlur;
        weightSum += blurWeights[i] * useForBlur;
    }
    color1 /= weightSum;
    return vec4(color1.rgb, 1.0);
}

// SnowAccumulation.fx.hlsl SnowSmoothNormal (UNOFFICIAL 2026-10-28 NormalSmoothing, snowAccColor.w: game units, 0 = off): the normal
// averaged over a disc of that radius on the surface, so on displaced land the snow does not come and go triangle by triangle.
const vec2 snowSmoothTaps[12] = vec2[12](vec2(0.35, 0.0), vec2(-0.35, 0.0), vec2(0.0, 0.35), vec2(0.0, -0.35),
    vec2(0.5, 0.5), vec2(-0.5, 0.5), vec2(0.5, -0.5), vec2(-0.5, -0.5),
    vec2(1.0, 0.0), vec2(-1.0, 0.0), vec2(0.0, 1.0), vec2(0.0, -1.0));

vec3 snowSmoothNormal(vec2 coord, vec3 center, vec3 norm, float depth) {
    float radius = c.snowAccColor.w;
    if (radius <= 0.0 || depth >= farZ * 0.99) return norm;
    vec2 perUnit = 0.5 * vec2(c.projection[0][0], c.projection[1][1]) / max(depth, 1.0);
    vec3 sum = norm;
    for (int i = 0; i < 12; i++) {
        // 2026-10-28 (as the .fx): only taps on screen whose normal is near this pixel's (no sheets on cars, no edge streaks)
        vec2 raw = coord + snowSmoothTaps[i] * radius * perUnit;
        vec2 tap = clamp(raw, 0.0, 1.0);
        float onScreen = (raw == tap) ? 1.0 : 0.0;
        float tapDepth = textureLod(SAMP_CLAMP(t_depth), tap, 0.0).x * farZ;
        vec3 tapPos = toWorld(tap) * tapDepth;
        vec4 tapNormal = vec4(textureLod(SAMP_CLAMP(t_normals), tap, 0.0).xyz * 2.0 - 1.0, 1.0);
        vec3 tapWorldNormal = (tapNormal * c.view).xyz;
        float weight = saturate(1.0 - length(tapPos - center) / (2.0 * radius)) * (tapDepth < farZ * 0.99 ? 1.0 : 0.0) * onScreen *
            smoothstep(0.5, 0.85, dot(tapWorldNormal, norm));
        sum += tapWorldNormal * weight;
    }
    return normalize(sum);
}

// UNOFFICIAL 2026-10-28 CapturedNormals (SnowAccumulation.fx.hlsl SnowCapturedNormal, Effects/Includes/CapturedNormals.hlsl CapturedNormal):
// the surface's own smooth normal where it wrote one and its distance matches the depth buffer's.
vec3 capturedDecode(vec2 e) {
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    float t = saturate(-n.z);
    n.xy += vec2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);
    return normalize(n);
}
bool capturedNormalRaw(vec2 coord, float dist, out vec3 n) {
    n = vec3(0.0, 0.0, 1.0);
    if (c.capturedData.x <= 0.0) return false;
    ivec2 size = textureSize(sampler2D(t_captured, NVR_SAMPLER(pc.sClamp)), 0);
    ivec2 texel = clamp(ivec2(coord * vec2(size)), ivec2(0), size - 1);
    vec4 v = texelFetch(sampler2D(t_captured, NVR_SAMPLER(pc.sClamp)), texel, 0);
    n = capturedDecode(v.rg);
    return v.a > 0.99 && abs(v.b - dist) <= dist * c.capturedData.y + c.capturedData.z;
}
bool capturedActor(vec2 coord, float dist) {	// Effects/Includes/CapturedNormals.hlsl CapturedActor
    if (c.capturedData.x <= 0.0) return false;
    ivec2 size = textureSize(sampler2D(t_captured, NVR_SAMPLER(pc.sClamp)), 0);
    ivec2 texel = clamp(ivec2(coord * vec2(size)), ivec2(0), size - 1);
    vec4 v = texelFetch(sampler2D(t_captured, NVR_SAMPLER(pc.sClamp)), texel, 0);
    return v.a > 1.5 && abs(v.b - dist) <= dist * c.capturedData.y + c.capturedData.z;
}
bool snowCapturedNormal(vec2 coord, float dist, vec3 rebuilt, out vec3 n) {
    n = vec3(0.0, 0.0, 1.0);
    if (c.snowExtra.x <= 0.0) return false;
    return capturedNormalRaw(coord, dist, n) && dot(n, rebuilt) > c.capturedData.w;	// CapturedNormalAgrees
}
vec4 snowCapturedView(vec2 coord, float dist) {
    vec3 rebuilt = getWorldNormal(coord);
    vec3 n;
    bool captured = capturedNormalRaw(coord, dist, n);
    if (c.snowExtra.y > 1.5) return captured && dot(n, rebuilt) <= c.capturedData.w ? vec4(1.0, 0.0, 0.0, 1.0) : vec4(vec3(captured ? saturate(1.0 - dot(n, rebuilt)) : 0.0), 1.0);
    captured = captured && dot(n, rebuilt) > c.capturedData.w;
    return vec4(captured ? n * 0.5 + 0.5 : (rebuilt * 0.5 + 0.5) * 0.3, 1.0);
}

// Snow (the last pass)
vec4 snowPass(vec2 coordIn) {
    vec4 color = texture(SAMP_CLAMP(t_source), coordIn);
    vec3 world = toWorld(coordIn);

    float depth = readDepth(coordIn);
    vec3 cameraVector = world * depth;
    vec3 eyeDirection = -1.0 * normalize(world);
    vec4 worldPos = vec4(c.cameraPosition.xyz + cameraVector, 1.0);

    if (c.snowExtra.y > 0.5) return snowCapturedView(coordIn, length(cameraVector));	// UNOFFICIAL 2026-10-28
    if (capturedActor(coordIn, length(cameraVector))) return color;	// UNOFFICIAL 2026-10-28: no snow on actors (SnowAccumulation.fx.hlsl)

    float ortho = texture(SAMP_CLAMP(t_rendered), coordIn).x;
    vec3 norm = getWorldNormal(coordIn);
    vec3 capturedNorm;	// UNOFFICIAL 2026-10-28 CapturedNormals
    bool captured = snowCapturedNormal(coordIn, length(cameraVector), norm, capturedNorm);
    if (captured) norm = capturedNorm;

    // early out for the character gun, water surfaces/areas and surfaces above the ortho map (such as actors)
    float waterTreshold = (depth / farZ) * 200.0;
    bool isWaterSurface = (dot(norm, vec3(0.0, 0.0, 1.0)) > 0.9) && (worldPos.z > c.waterSettings.x - waterTreshold) && (worldPos.z < c.waterSettings.x + waterTreshold);
    if (isWaterSurface || !(ortho > 0.0)) return color;
    if (!captured) norm = snowSmoothNormal(coordIn, cameraVector, norm, depth);	// UNOFFICIAL 2026-10-28 NormalSmoothing (after the water test; captured: smooth already)

    color = linearize(color);
    vec3 sunColor = linearize(c.sunColor).rgb;

    vec2 uvN = worldPos.xy / 200.0;
    vec3 localNorm = (texture(SAMP_WRAP(t_snowNorm), uvN).xyz - 0.5) / 0.5;	// expand()
    vec3 surfaceNormal = normalize(vec3(localNorm.xy + norm.xy, localNorm.z * norm.z));
    vec4 normal = vec4(surfaceNormal, 1.0);

    vec3 snowTex = c.snowAccColor.rgb;

    // pows, not pow: 1 - shade can be a hair below zero from rounding
    float fresnelCoeff = saturate(pows(1.0 - shadeH(eyeDirection, surfaceNormal), 5.0));
    vec3 snowSpec = vec3(pows(shadesH(normalize(c.sunDirection.xyz + eyeDirection), surfaceNormal), 20.0) * fresnelCoeff);

    vec3 ambient = snowTex * pows(c.sunAmbient.rgb, 2.2); // linearise
    vec2 shadow = texture(SAMP_CLAMP(t_pointShadow), coordIn).rg;
    float useShadows = (c.shadowScreenSpaceData.x != 0.0 || c.shadowFade.y != 0.0) ? 1.0 : 0.0;	// TESR_ShadowScreenSpaceData.x || TESR_ShadowFade.y
    shadow.r = lerpH(1.0, shadow.r, useShadows); // disable shadow sampling if shadows are disabled in game
    shadow.r = lerpH(shadow.r, 1.0, c.shadowFade.x);	// fade shadows to light when sun is low

    float sunPower = c.snowParams.z;
    float specularPower = 1.0 * sunPower;
    float fresnelPower = 0.65 * sunPower;
    vec3 diffuse = snowTex * shadeH4(c.sunDirection, normal) * sunColor * (0.56 * sunPower) * shadow.r;
    vec3 spec = snowSpec * sunColor * specularPower * shadow.r;
    vec3 fresnel = fresnelCoeff * sunColor * fresnelPower;
    vec3 sparkles = vec3(pows(shadesH(eyeDirection, normalize((texture(SAMP_WRAP(t_blueNoise), worldPos.xy / sparkleScale).rgb - 0.5) / 0.5)), 1000.0) * 0.2 * shadow.r);

    vec4 snowColor = vec4(ambient + diffuse + spec + fresnel + sparkles, 1.0);
    const float pointLightsPower = 0.5;

    shadow.g = 1.0; // disable point light shadows for debug

    for (int i = 0; i < 12; i++) {
        snowColor.rgb += getPointLightContribution(worldPos, c.shadowLightPosition[i], normal) * linearize(vec4(c.lightColor[i].rgb * c.lightColor[i].a, 1.0)).rgb * pointLightsPower * shadow.g;
        snowColor.rgb += getPointLightContribution(worldPos, c.lightPosition[i], normal) * linearize(vec4(c.lightColor[12 + i].rgb * c.lightColor[12 + i].a, 1.0)).rgb * pointLightsPower * shadow.g;
    }

    // create a noisy pattern of accumulation over time
    float coverage = saturate(pows(snowNoise(worldPos.xyz / (noiseSize * 0.5)) * snowNoise(worldPos.xyz / (noiseSize * 3.0)) * snowNoise(worldPos.xyz / noiseSize), 2.0 - 2.0 * c.snowParams.w));
    coverage = saturate(smoothstepH(0.2, 0.4, coverage));
    float vertical = smoothstepH(0.5, 0.8, shadeH(vec3(0.0, 0.0, 1.0), norm));

    float amount = coverage * min(vertical, ortho);
    color = color + (snowColor - color) * amount;	// lerp(color, snowColor, ...)
    color.a = 1.0;

    color.rgb = delinearize(color.rgb);
    return color;
}

void main() {
    if (pc.mode == 0u) o_color = snowCoverage(uv);
    else if (pc.mode == 1u) o_color = scalePass(uv);
    else if (pc.mode == 2u) o_color = depthBlur(uv, pc.offsetMask);
    else o_color = snowPass(uv);
}
