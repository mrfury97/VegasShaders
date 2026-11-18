// WetWorld's last pass as a Vulkan pass (needs NVR DXVK): Wet(fast, fixTrace) of Effects\WetWorld.fx.hlsl, line for line
// (effects/WetWorld.cpp ExtRender). mode bit 0 = fast (FastPaths, techniques 2 and 4), bit 1 = fixTrace (FixedReflectionTrace,
// techniques 3 and 4). Technique 0 (FastPaths off, the original trace) is not ported: the D3D9 pass runs it.
// Without fixTrace the fast path traces nothing (its ssrHit stays 0, as the D3D9 fast path does).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;      // TESR_RenderedBuffer: the blurred wet map (LINEAR, CLAMP)
layout(set = 1, binding = 1) uniform texture2D t_source;        // TESR_SourceBuffer: the picture before the effect (LINEAR, CLAMP)
layout(set = 1, binding = 2) uniform texture2D t_depth;         // TESR_DepthBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 3) uniform texture2D t_depthVM;       // TESR_DepthBufferViewModel (LINEAR, CLAMP)
layout(set = 1, binding = 4) uniform texture2D t_ortho;         // TESR_OrthoMapBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 5) uniform texture2D t_normals;       // TESR_NormalsBuffer (point, CLAMP)
layout(set = 1, binding = 6) uniform texture2D t_pointShadow;   // TESR_PointShadowBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 7) uniform texture2D t_ripple;        // Precipitations\ripples.dds (LINEAR mips, WRAP)
layout(set = 1, binding = 8) uniform texture2D t_splash;        // Precipitations\splash.dds (LINEAR mips, CLAMP)
layout(set = 1, binding = 9) uniform texture2D t_splashN;       // Precipitations\splash_n.dds (LINEAR mips, CLAMP)
layout(set = 1, binding = 10) uniform texture2D t_cloud;        // water\water_d.dds (LINEAR mips, WRAP)
layout(set = 1, binding = 11) uniform texture2D t_puddleN;      // water\water_nrm.dds (LINEAR mips, WRAP)
layout(set = 1, binding = 12) uniform texture2D t_streak;       // Precipitations\water-streaks_n.dds (LINEAR mips, WRAP)

layout(push_constant) uniform Push {
    uint sLin;          // LINEAR, CLAMP (heap index, filled in by the extension)
    uint sPoint;        // POINT, CLAMP
    uint sMipWrap;      // LINEAR mips, WRAP
    uint sMipClamp;     // LINEAR mips, CLAMP
    uint mode;          // bit 0 fast, bit 1 fixTrace
    uint pad[3];
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 o_color;

#include "WetWorldCommon.glsl"

// Shared constants of WetWorld.fx.hlsl (the wet pass's own)
const float PI = 3.1415926538;	// the shader's PI: Helpers.hlsl's macro (it comes after the file's own PI)
const float rippleScale = 120.0;
const float refractionScale = 50.0;

const float SplashMaxDist = 2000.0;
const float SplashFadeStart = 1200.0;
const float SplashWorldScale = 8.0;
const float SplashAnimSpeed = 1.35;
const float SplashIntensity = 0.17;

const vec2 StreakWorldSize = vec2(64.0, 90.0);
const float StreakFlowSpeed = 12.0;
const float StreakFlowMinMultiplier = 0.45;
const float StreakFlowMaxMultiplier = 1.65;
const float StreakFlowGroups = 8.0;
const float StreakNormalStrength = 0.25;
const float StreakNormalYSign = -1.0;
const float StreakMaskStrength = 1.0;
const float StreakDarken = 0.45;
const float StreakSpecularStrength = 1.20;
const float StreakSunSpecularPower = 80.0;
const float StreakHighlightLimit = 0.25;
const float StreakFadeStart = 800.0;
const float StreakFadeFull = 1000.0;

const float PuddleAccumulationStart = 0.08;
const float PuddleAccumulationFull = 1.0;
const float FlatPuddleBlurStrength = 0.0;

const float PuddleNormalStrength = 0.25;
const float FlatPuddleNormalStrength = 0.15;
const float PuddleNormalYSign = 1.0;
const float PuddleCloudStrength = 0.38;
const float PuddleCloudWorldSize = 256.0;
const float PuddleCloudBrightness = 1.65;
const float PuddleCloudContrast = 15.0;
const float PuddleCloudRippleDistortion = 0.035;
const vec2 PuddleCloudDrift = vec2(-0.02, -0.01);

const float PuddleSkyStrength = 0.55;
const float PuddleSkyMinReflect = 0.12;
const float PuddleSkyMaxBlend = 0.28;
const float PuddleSkyBrightness = 0.55;
const float PuddleSkyTintInfluence = 0.25;
const float PuddleSunStrength = 2.25;
const float PuddleLightStrength = 2.50;
const float PuddleSunLimit = 1.95;
const float PuddleLightLimit = 0.90;
const float PuddleSunHighlightLimit = 1.90;
const float PuddlePointHighlightLimit = 1.8;
const float PuddleSkyLumaFloor = 0.015;
const float PuddleSkyLumaScale = 0.65;

const float PuddleSSRCandidateBlend = 0.55;
const float PuddleSSRMinTraceOpacity = 0.005;
const float PuddleSSRScreenFadeStart = 1.0 / 3.0;
const float PuddleSSRScreenFadeFull = 2.0 / 3.0;
const float PuddleSSRFadeStart = 0.20;
const float PuddleSSRFadeFull = 0.40;
const int PuddleSSRSteps = 8;
const float PuddleSSRMaxDistance = 1800.0;
const float PuddleSSRSurfaceBias = 3.0;
const float PuddleSSRNormalInfluence = 0.25;
const int PuddleSSRRefinementSteps = 2;
const float PuddleSSRRefinementTolerance = 12.0;
const float PuddleSSRThickness = 24.0;
const float PuddleSSRThicknessPerStep = 0.30;
const float PuddleSSREdgeFade = 0.05;

// HLSL fmod: x - y * trunc(x / y)
float fmodHlsl(float x, float y) { return x - y * trunc(x / y); }

vec3 WetOut(vec3 x) { return c.wetWorldLook.x > 0.5 ? max(x, 0.0) : saturate(x); }

// Rain ripple helper
vec3 ComputeRipple(vec2 uv, float currentTime, float weight) {
    vec4 ripple = texture(sampler2D(t_ripple, NVR_SAMPLER(pc.sMipWrap)), uv);
    ripple.yz = (ripple.yz - 0.5) / 0.5;

    float period = fract(ripple.w + currentTime);
    float timeFrac = period - 1.0 + ripple.x;
    float dropFactor = saturate(0.2 + weight * 0.8 - period);
    float finalFactor = dropFactor * ripple.x * sin(clamp(timeFrac * 9.0, 0.0, 3.0) * PI);

    return vec3(ripple.yz * finalFactor * 0.35, 1.0);
}

// Rain splash helpers
float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

vec2 hash22(vec2 p) {
    float n = hash12(p);
    return vec2(n, hash12(p + n * 19.19));
}

vec4 SampleSplash(vec2 worldXY, float time, float density, out float splashAlpha, out vec2 splashNXY) {
    vec2 cell = floor(worldXY / SplashWorldScale);
    float r = hash12(cell);

    float spawn = step(1.0 - density, r);
    float phase = fract(time * SplashAnimSpeed + r * 7.13);

    float fade = smoothstep(0.00, 0.10, phase) * (1.0 - smoothstep(0.85, 1.00, phase));

    float frame = floor(phase * 16.0);
    vec2 frameXY = vec2(fmodHlsl(frame, 4.0), floor(frame / 4.0));

    vec2 localUV = fract(worldXY / SplashWorldScale);
    vec2 jitter = (hash22(cell + 13.0) - 0.5) * 0.25;
    localUV = fract(localUV + jitter);

    vec2 uv = (localUV + frameXY) * 0.25;

    vec4 s = texture(sampler2D(t_splash, NVR_SAMPLER(pc.sMipClamp)), uv);

    vec3 nTex = texture(sampler2D(t_splashN, NVR_SAMPLER(pc.sMipClamp)), uv).xyz;
    vec2 nXY = nTex.xy * 2.0 - 1.0;
    splashNXY = normalize(vec3(nXY, 1.0)).xy;

    float a = saturate((s.a - 0.25) * 1.6);
    splashAlpha = a * spawn * fade;

    return s;
}

// Variable streak flow helper
vec4 SampleScrollingStreaks(vec2 uv, float flowTravel, float seed, vec2 uvDx, vec2 uvDy) {
    float laneCoord = uv.x * max(StreakFlowGroups, 1.0);
    float lane = floor(laneCoord);
    float blend = smoothstep(0.40, 0.60, fract(laneCoord));
    vec2 randomSpeed = vec2(hash12(vec2(lane, seed)), hash12(vec2(lane + 1.0, seed)));
    float minSpeed = max(min(StreakFlowMinMultiplier, StreakFlowMaxMultiplier), 0.0);
    float maxSpeed = max(max(StreakFlowMinMultiplier, StreakFlowMaxMultiplier), minSpeed);
    vec2 speed = mix(vec2(minSpeed), vec2(maxSpeed), randomSpeed);
    vec2 uvA = uv - vec2(0.0, flowTravel * speed.x);
    vec2 uvB = uv - vec2(0.0, flowTravel * speed.y);
    vec4 sampleA = textureGrad(sampler2D(t_streak, NVR_SAMPLER(pc.sMipWrap)), uvA, uvDx, uvDy);
    vec4 sampleB = textureGrad(sampler2D(t_streak, NVR_SAMPLER(pc.sMipWrap)), uvB, uvDx, uvDy);
    float alpha = mix(sampleA.a, sampleB.a, blend);
    vec3 rgb = mix(sampleA.rgb * sampleA.a, sampleB.rgb * sampleB.a, blend) / max(alpha, 0.000001);
    return vec4(rgb, alpha);
}

// Vertical streak coordinates
vec4 GetWaterStreakUVs(vec3 surfacePosition, vec3 surfaceNormal) {
    vec2 side = vec2(surfaceNormal.x < 0.0 ? -1.0 : 1.0, surfaceNormal.y < 0.0 ? -1.0 : 1.0);
    vec2 worldSize = max(StreakWorldSize, vec2(1.0));
    float verticalUV = -surfacePosition.z / worldSize.y;
    vec2 uvX = vec2(surfacePosition.y * side.x / worldSize.x, verticalUV);
    vec2 uvY = vec2(-surfacePosition.x * side.y / worldSize.x + 0.37, verticalUV + 0.19);
    return vec4(uvX, uvY);
}

// Vertical streak helper
vec4 SampleWaterStreaks(vec4 streakUV, vec3 surfaceNormal, vec4 uvDx, vec4 uvDy) {
    vec2 projectionWeights = pow(abs(surfaceNormal.xy), vec2(4.0));
    projectionWeights /= max(projectionWeights.x + projectionWeights.y, 0.000001);
    vec2 side = vec2(surfaceNormal.x < 0.0 ? -1.0 : 1.0, surfaceNormal.y < 0.0 ? -1.0 : 1.0);
    vec2 worldSize = max(StreakWorldSize, vec2(1.0));
    float flowTravel = c.gameTime.z * StreakFlowSpeed / worldSize.y;
    vec4 sampleX = SampleScrollingStreaks(streakUV.xy, flowTravel, 17.0, uvDx.xy, uvDy.xy);
    vec4 sampleY = SampleScrollingStreaks(streakUV.zw, flowTravel, 53.0, uvDx.zw, uvDy.zw);
    vec2 normalX = sampleX.xy * 2.0 - 1.0;
    vec2 normalY = sampleY.xy * 2.0 - 1.0;
    normalX.y *= StreakNormalYSign;
    normalY.y *= StreakNormalYSign;
    vec2 coverage = saturate(vec2(sampleX.a, sampleY.a) * StreakMaskStrength) * projectionWeights;
    vec3 offsetX = vec3(0.0, normalX.x * side.x, -normalX.y);
    vec3 offsetY = vec3(-normalY.x * side.y, 0.0, -normalY.y);
    vec3 offset3 = offsetX * coverage.x + offsetY * coverage.y;
    offset3 -= surfaceNormal * dot(surfaceNormal, offset3);
    return vec4(offset3 * StreakNormalStrength, coverage.x + coverage.y);
}

// Screen-space reflection helpers
vec3 GetSSRViewPosition(vec2 uv) {
    vec4 d = textureLod(sampler2D(t_depth, NVR_SAMPLER(pc.sLin)), uv, 0.0);
    vec4 p = vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.5, 1.0) * c.invProjection;
    float safeW = (p.w < 0.0 ? -1.0 : 1.0) * max(abs(p.w), 0.000001);
    vec3 viewPosition = p.xyz / safeW;
    viewPosition *= (d.x * farZ) / max(abs(viewPosition.z), 0.000001);
    return viewPosition;
}

// FixedReflectionTrace: the corrected hit test (the D3D9 technique's loop exits do not work on the GPU; see WetWorld.fx.hlsl).
// xy = hit UV, z = visibility (0 = miss).
vec3 TraceWetReflectionFixed(vec2 sourceUV, vec3 waterNormal) {
    vec3 result = vec3(0.0);
    vec3 origin = GetSSRViewPosition(sourceUV);
    vec4 sourceClip = vec4(origin, 1.0) * c.projection;
    float viewSign = origin.z < 0.0 ? -1.0 : 1.0;
    float clipSign = sourceClip.w < 0.0 ? -1.0 : 1.0;
    vec3 viewNormal = normalize((vec4(waterNormal, 0.0) * c.view).xyz);
    if (dot(viewNormal, origin) > 0.0) viewNormal = -viewNormal;
    vec3 rayDirection = normalize(reflect(normalize(origin), viewNormal));
    origin += viewNormal * PuddleSSRSurfaceBias;
    vec4 projectedOrigin = vec4(origin, 1.0) * c.projection;
    vec4 projectedDirection = vec4(rayDirection, 0.0) * c.projection;
    float previousDelta = -PuddleSSRRefinementTolerance;
    float previousTravel = 0.0;
    bool done = abs(sourceClip.w) < 0.0001 || abs(origin.z) < 0.0001;
    for (int ssrStep = 1; ssrStep <= PuddleSSRSteps; ssrStep++) {
        float fraction = float(ssrStep) / float(PuddleSSRSteps);
        float travel = 8.0 + PuddleSSRMaxDistance * fraction * fraction;
        vec3 probe = origin + rayDirection * travel;
        vec4 projected = projectedOrigin + projectedDirection * travel;
        vec2 hitUV = projected.xy / max(abs(projected.w), 0.0001) * vec2(0.5, -0.5) + 0.5;
        bool onScreen = projected.w * clipSign > 0.0001 && probe.z * viewSign > 0.0001
            && hitUV.x > 0.001 && hitUV.x < 0.999 && hitUV.y > 0.001 && hitUV.y < 0.999;
        float depthSample = textureLod(sampler2D(t_depth, NVR_SAMPLER(pc.sLin)), hitUV, 0.0).x;
        float delta = probe.z * viewSign - depthSample * farZ;
        if (!done && onScreen && depthSample > 0.0 && depthSample < 0.99999 && delta >= 0.0 && previousDelta < 0.0) {
            float lowerTravel = previousTravel;
            float upperTravel = travel;
            for (int refinement = 0; refinement < PuddleSSRRefinementSteps; refinement++) {
                float midTravel = (lowerTravel + upperTravel) * 0.5;
                vec3 midProbe = origin + rayDirection * midTravel;
                vec4 midClip = projectedOrigin + projectedDirection * midTravel;
                vec2 midUV = midClip.xy / midClip.w * vec2(0.5, -0.5) + 0.5;
                float midZ = textureLod(sampler2D(t_depth, NVR_SAMPLER(pc.sLin)), midUV, 0.0).x * farZ;
                bool behind = midProbe.z * viewSign - midZ >= 0.0;
                upperTravel = behind ? midTravel : upperTravel;
                lowerTravel = behind ? lowerTravel : midTravel;
            }
            vec3 hitProbe = origin + rayDirection * upperTravel;
            vec4 hitClip = projectedOrigin + projectedDirection * upperTravel;
            vec2 refinedUV = hitClip.xy / hitClip.w * vec2(0.5, -0.5) + 0.5;
            float hitDepth = textureLod(sampler2D(t_depth, NVR_SAMPLER(pc.sLin)), refinedUV, 0.0).x;
            float hitDelta = hitProbe.z * viewSign - hitDepth * farZ;
            float vmHitDepth = textureLod(sampler2D(t_depthVM, NVR_SAMPLER(pc.sLin)), refinedUV, 0.0).x;
            bool hitViewModel = (c.depthConstants.z == 0.0 && vmHitDepth < 0.9)
                || (c.depthConstants.z > 0.0 && vmHitDepth > 0.01);
            vec2 pixelSeparation = (refinedUV - sourceUV) / c.reciprocalResolution.xy;
            bool validHitDepth = hitDepth > 0.0 && hitDepth < 0.99999;
            bool separatedHit = dot(pixelSeparation, pixelSeparation) >= 16.0;
            float thickness = PuddleSSRThickness + PuddleSSRThicknessPerStep * (travel - previousTravel);
            float edge = min(min(refinedUV.x, 1.0 - refinedUV.x), min(refinedUV.y, 1.0 - refinedUV.y));
            float edgeVisibility = smoothstep(0.0, PuddleSSREdgeFade, edge);
            float distanceVisibility = max(1.0 - smoothstep(PuddleSSRMaxDistance * 0.65,
                PuddleSSRMaxDistance, upperTravel), 0.25);
            float visibility = edgeVisibility * distanceVisibility;
            if (hitDelta >= 0.0 && hitDelta <= thickness && validHitDepth && !hitViewModel && separatedHit && visibility > 0.0) {
                result = vec3(refinedUV, visibility);
                done = true;
            }
        }
        done = done || !onScreen;
        previousDelta = delta;
        previousTravel = travel;
    }
    return result;
}

// FastPaths helpers
vec2 GetPuddleCloudUV(vec4 worldPosRelRoof, vec3 combinedNormals) {
    vec3 cloudSurface = c.cameraPosition.xyz + worldPosRelRoof.xyz;
    vec2 cloudUV = cloudSurface.xy / max(PuddleCloudWorldSize, 1.0);
    cloudUV += c.gameTime.z * PuddleCloudDrift;
    cloudUV += combinedNormals.xy * PuddleCloudRippleDistortion;
    return cloudUV;
}

// One lamp's highlight, as the main path's loop body (the same expressions; att > 0 gates the work).
void AddLampHighlightFast(vec4 lightPos, vec3 col, vec3 worldPos, vec3 V, vec3 N, float specMask, float streakMask,
    inout vec3 pointSpecCol, inout vec3 streakPointCol) {
    vec3 L = lightPos.xyz - worldPos;
    float dist = length(L);
    float ld = dist / max(lightPos.w, 1e-4);
    float att = saturate(1.0 - ld);
    att *= att;
    att = lightPos.w > 0.0 ? att : 0.0;
    if (att > 0.0) {
        L /= max(dist, 1e-4);
        vec3 H = normalize(L - V);

        float ndh = saturate(dot(N, H));
        float specLobe = pow(ndh, 30.0);
        float spec = specLobe * specMask;

        pointSpecCol += col * spec * att;
        float streakSpec = specLobe * streakMask * StreakSpecularStrength;
        streakPointCol += col * streakSpec * att;
    }
}

// Engine/Includes Sky.hlsl GetSkyColor
vec3 GetSkyColor(float verticality, float athmosphere, float sunHeight, float sunInfluence, float sunStrength,
    vec3 skyColor, vec3 skyLowColor, vec3 horizonColor, vec3 sunColor) {
    skyColor = linearize(skyColor);
    skyLowColor = linearize(skyLowColor);
    horizonColor = linearize(horizonColor);

    float isDayTime = smoothstep(0.0, 0.5, c.sunAmount.x);

    vec3 color = mix(skyLowColor, skyColor, verticality);
    color = mix(color, horizonColor, saturate(athmosphere * (0.5 + sunInfluence * 0.5)));
    color += mix(vec3(0.0), sunColor * sunInfluence * (1.0 - sunHeight) * (((1.0 - verticality) + athmosphere) / 2.0) * sunStrength, isDayTime);

    return color;
}

// Wet-surface pass (Wet(fast, fixTrace) of WetWorld.fx.hlsl)
vec4 WetPass(vec2 uv, bool fast, bool fixTrace) {
    // Surface reconstruction and weather
    vec4 baseColor = texture(sampler2D(t_source, NVR_SAMPLER(pc.sLin)), uv);

    // UNOFFICIAL 2026-11-12 PuddlesLandOnly / WeaponRainHandOff / ReflectionsOwnPuddles (WetWorld.fx.hlsl Wet, line for line): the wet
    // map's alpha is this pixel's class (WetWorldMap.frag GetSurfaceWetClass), exactly 0, 0.5, 1 or 1.5; 1 with the keys off, and
    // every product below is then x 1.
    vec4 wetMapSample = texture(sampler2D(t_rendered, NVR_SAMPLER(pc.sLin)), uv);
    float puddleAllow = floor(wetMapSample.a);
    float wetAllow = max(puddleAllow, ceil(fract(wetMapSample.a)));
    float reflectionAllow = puddleAllow - puddleAllow * fract(wetMapSample.a) - puddleAllow * fract(wetMapSample.a);

    float depth = readDepth(uv);

    vec4 worldPosRelRoof = GetOrthoSurfacePosition(uv);
    vec4 orthoRoofPos = worldPosRelRoof * c.shadowToLight;
    float roofMask = GetRoofMask(orthoRoofPos) * wetAllow;

    vec3 eyeDirection = toWorld(uv);
    vec3 camera_vector = eyeDirection * depth;
    vec4 worldPos = vec4(c.cameraPosition.xyz + camera_vector, 1.0);

    vec3 normal = GetWorldNormal(uv);
    vec3 up = vec3(0.0, 0.0, 1.0);

    float floorAngle = smoothstep(0.94, 1.0, dot(normal, up));
    eyeDirection = normalize(eyeDirection);
    vec3 V = normalize(eyeDirection);

    float rainAmt = saturate(c.wetWorldData.x / max(c.wetWorldData.y, 0.0001));
    float rainFade = smoothstep(0.05, 0.35, rainAmt);
    float rainVis = rainFade * roofMask;

    // First-person surface mask
    float vmDepthX = texture(sampler2D(t_depthVM, NVR_SAMPLER(pc.sLin)), uv).x;
    float vmMask = GetViewModelMask(uv);
    float puddleVMExclude = GetPuddleViewModelExclusion(uv, vmMask);

    vec2 dd = vec2(dFdx(vmDepthX), dFdy(vmDepthX));
    float grad = length(dd);
    float vmFlatMask = 1.0 - saturate(grad * 80.0);
    vmFlatMask = smoothstep(0.55, 0.90, vmFlatMask);

    // Rain splashes
    vec2 splashBumpXY = vec2(0.0);

    float dotUp = dot(normal, up);
    float splashDistFade = 1.0 - smoothstep(SplashFadeStart, SplashMaxDist, depth);
    float splashFlatMask = smoothstep(0.7071, 0.92, dotUp);

    vec2 splashXY = worldPos.xy;

    float rand = hash12(floor(worldPos.xy * 0.5));

    float worldSizeVar = mix(0.75, 1.35, rand);
    vec2 worldSplashXY = splashXY / worldSizeVar;

    float vmSizeVar = mix(0.70, 1.30, rand);

    float vmScale = 0.28;

    float vmSplashSizeMul = 1.0;

    vec2 vmSplashXY = (splashXY / (vmScale * vmSplashSizeMul)) * vmSizeVar;

    splashXY = mix(worldSplashXY, vmSplashXY, vmMask);

    float splashAlpha;
    vec2 splashNXY;
    vec4 splashSample = SampleSplash(splashXY, c.gameTime.z, (rainAmt * roofMask) * 0.395, splashAlpha, splashNXY);
    splashAlpha *= roofMask;

    float splashStrength = 2.2;
    splashStrength *= splashAlpha;
    splashStrength *= splashFlatMask;

    splashStrength *= saturate(0.5 + 0.5 * rainVis);
    splashBumpXY = splashNXY * splashStrength;

    float vmIntensity = mix(1.0, 0.90, vmMask);

    vec3 splashRGB = splashSample.rgb;
    splashRGB = saturate((splashRGB - 0.5) * 1.6 + 0.5);
    splashRGB = mix(splashRGB, vec3(1.0), 0.35);

    vec3 splashAdd = splashRGB * (splashAlpha * splashDistFade * splashFlatMask * SplashIntensity * vmIntensity);
    splashAdd *= roofMask;
    splashAdd *= 0.6;

    // Water exclusion and distance fade
    float waterThreshold = (depth / farZ) * 500.0;
    bool isWaterSurface = (floorAngle > 0.9) &&
        (worldPos.z > c.waterSettings.x - waterThreshold) &&
        (worldPos.z < c.waterSettings.x + waterThreshold);

    if (!fast && (depth > DrawD || isWaterSurface))
        return vec4(WetOut(baseColor.rgb + splashAdd), baseColor.a);
    vec4 baseColorIn = baseColor;

    float LODfade = smoothstepH(DrawD, 0.0, depth);

    // Puddle coverage and wetness
    vec3 blurredWetCoverage = saturate(wetMapSample.rgb);
    float puddles = blurredWetCoverage.r;
    puddles *= (1.0 - puddleVMExclude) * (1.0 - vmMask) * puddleAllow;
    puddles *= smoothstep(DepressionSlopeCutoff, DepressionSlopeFadeStart, dotUp);

    float puddleFade = smoothstep(PuddleAccumulationStart, PuddleAccumulationFull,
        saturate(c.wetWorldData.z));
    float puddlemask = puddles * puddleFade;

    float slopeMask = smoothstep(0.90, 0.97, dotUp);

    float depthish = saturate(puddlemask * 1.15);
    depthish = depthish * depthish;

    float rim = 4.0 * puddlemask * (1.0 - puddlemask);
    float rimDark = rim * 0.15;
    float rimReflBoost = rim * 0.08;

    float rippleFlatMask = smoothstep(0.65, 0.92, dotUp);
    float rippleFlatFinal = mix(rippleFlatMask, rippleFlatMask * vmFlatMask, vmMask);
    float rippleMask = rainVis * rippleFlatFinal;

    rippleMask *= mix(1.0, vmFlatMask, vmMask);

    float flatBlurStrength = saturate(FlatPuddleBlurStrength);
    float flatCoverage = mix(roofMask * rippleFlatMask, blurredWetCoverage.g, flatBlurStrength);
    float flatWetCoverage = mix(roofMask * rippleFlatMask * slopeMask,
        blurredWetCoverage.b, flatBlurStrength);
    rippleMask = mix(rainFade * flatCoverage, rippleMask, vmMask);

    float wetSlope = mix(slopeMask, 1.0, vmMask);
    float wetRippleMask = mix(rainFade * flatWetCoverage, rippleMask * wetSlope, vmMask);
    float wetMask = saturate(max(puddlemask, wetRippleMask * 0.85));
    float specMask = saturate(max(puddlemask,
        wetRippleMask + rim * 0.15 * wetSlope));

    specMask = saturate(specMask + splashAlpha * 0.55 * splashFlatMask);

    // Vertical streaks
    float streakExposure = (1.0 - smoothstep(0.55, 0.80, abs(dotUp)))
        * (1.0 - vmMask) * rainVis;
    float streakDistance = length(camera_vector);
    streakExposure *= 1.0 - smoothstep(StreakFadeStart, StreakFadeFull, streakDistance);
    vec3 streakSurfaceNormal = normalize(normal);
    vec4 streakUV = GetWaterStreakUVs(worldPos.xyz, streakSurfaceNormal);
    vec4 streakUVDx = dFdx(streakUV);
    vec4 streakUVDy = dFdy(streakUV);
    vec4 streakSample = vec4(0.0);
    if (streakDistance < StreakFadeFull && streakExposure > 0.0)
        streakSample = SampleWaterStreaks(streakUV, streakSurfaceNormal, streakUVDx, streakUVDy);
    float streakMask = saturate(streakSample.a * streakExposure);
    vec3 streakNormalOffset = streakSample.xyz * streakExposure;
    baseColor.rgb *= 1.0 - StreakDarken * streakMask;

    // Ripple normals
    vec2 rippleUV_world = worldPos.xy / rippleScale;

    float vmRippleScale = 0.28;
    float vmRippleSizeMul = 0.2;

    vec2 rippleUV_vm = worldPos.xy / (rippleScale * vmRippleScale * vmRippleSizeMul);

    vec2 rippleUV = mix(rippleUV_world, rippleUV_vm, vmMask);

    vec4 Weights = vec4(1.0, 0.75, 0.5, 0.25) * c.wetWorldData.x;
    Weights = saturate(Weights * 4.0);

    float time1 = c.gameTime.z * 0.96;
    float time2 = c.gameTime.z * 0.97;
    float time3 = c.gameTime.z * 0.98;
    float time4 = c.gameTime.z * 0.99;

    vec3 Ripple1 = ComputeRipple(rippleUV + vec2(0.25, 0.0), time1, Weights.x);
    vec3 Ripple2 = ComputeRipple(rippleUV * 1.1 + vec2(-0.55, 0.3), time2, Weights.y);
    vec3 Ripple3 = ComputeRipple(rippleUV * 1.3 + vec2(0.6, 0.85), time3, Weights.z);
    vec3 Ripple4 = ComputeRipple(rippleUV * 1.5 + vec2(0.5, -0.75), time4, Weights.w);

    vec4 Z = mix(vec4(1.0), vec4(Ripple1.z, Ripple2.z, Ripple3.z, Ripple4.z), Weights);

    vec3 ripple = vec3(
        Weights.x * Ripple1.xy + Weights.y * Ripple2.xy + Weights.z * Ripple3.xy + Weights.w * Ripple4.xy,
        Z.x * Z.y * Z.z * Z.w
        );

    ripple.xy *= 2.2;

    vec3 combinedNormals = normalize(ripple);
    combinedNormals = normalize(mix(vec3(0.0, 0.0, 1.0), combinedNormals, LODfade * wetMask));

    combinedNormals.xy += splashBumpXY * 2.0;
    combinedNormals = normalize(combinedNormals);

    // Puddle waves
    float t = c.gameTime.z;

    vec2 windDir = vec2(1.0, 0.0);
    float p = dot(worldPos.xy, windDir);

    vec2 swell;
    swell.x =
    sin(p * 0.260 + t * 3.10) +
    0.55 * sin((p + worldPos.y) * 0.200 + t * 3.90);

    swell.y =
    sin(p * 0.240 + t * 3.40) +
    0.55 * sin((p + worldPos.x) * 0.210 + t * 3.60);

    float distFade = (1.0 - smoothstep(400.0, 1800.0, depth));
    float swellAmp = 0.0030 * pow(puddlemask, 1.35) * distFade;

    combinedNormals.xy += swell * swellAmp;
    combinedNormals = normalize(combinedNormals);

    // Puddle texture normals
    vec3 puddleNormalSurface = c.cameraPosition.xyz + worldPosRelRoof.xyz;
    vec2 puddleSurfaceUV = puddleNormalSurface.xy / max(PuddleCloudWorldSize, 1.0);
    vec2 puddleNormalUV = puddleSurfaceUV + c.gameTime.z * PuddleCloudDrift;
    puddleNormalUV += combinedNormals.xy * PuddleCloudRippleDistortion;
    vec2 puddleNormalUV2 = puddleSurfaceUV * 1.37 + vec2(0.23, 0.61)
        - c.gameTime.z * PuddleCloudDrift * 0.65;
    puddleNormalUV2 += combinedNormals.xy * PuddleCloudRippleDistortion;
    vec2 puddleTexNormalA = texture(sampler2D(t_puddleN, NVR_SAMPLER(pc.sMipWrap)), puddleNormalUV).xy * 2.0 - 1.0;
    vec2 puddleTexNormalB = texture(sampler2D(t_puddleN, NVR_SAMPLER(pc.sMipWrap)), puddleNormalUV2).xy * 2.0 - 1.0;
    vec2 puddleTexNormal = mix(puddleTexNormalA, puddleTexNormalB, 0.35);
    puddleTexNormal.y *= PuddleNormalYSign;
    float flatNormalStrength = FlatPuddleNormalStrength * saturate(wetRippleMask) * (1.0 - vmMask);
    float textureNormalStrength = mix(flatNormalStrength, PuddleNormalStrength, puddlemask);
    vec2 puddleNormalOffset = puddleTexNormal * textureNormalStrength * LODfade;
    combinedNormals = normalize(vec3(combinedNormals.xy + puddleNormalOffset,
        combinedNormals.z));

    // Surface normals
    vec3 baseN = normalize(mix(normal, up, puddlemask));
    vec3 N = normalize(mix(baseN, combinedNormals, specMask));

    N = normalize(N + streakNormalOffset);

    // Refraction
    float NdotV_refr = saturate(dot(N, -V));
    float viewRefr = smoothstep(0.18, 0.45, NdotV_refr);

    vec2 refrOff = ((projectPosition(combinedNormals) - 0.5) / 0.5).xy * c.reciprocalResolution.xy * refractionScale;

    float splashRefr = 0.35 * splashAlpha * splashFlatMask;
    refrOff += splashBumpXY * (refractionScale * 0.35) * c.reciprocalResolution.xy * splashRefr;

    refrOff *= mix(1.0, 0.35, puddlemask);

    refrOff *= viewRefr;

    float swellUV = (swellAmp * 0.50) * viewRefr;
    refrOff += swell * swellUV;

    vec2 refrClamp = c.reciprocalResolution.xy * (refractionScale * 0.75);
    refrOff = clamp(refrOff, -refrClamp, refrClamp);

    // FastPaths: every texture read the original makes with implicit derivatives is made here, on every pixel, before the
    // first branch (the same fetches as the D3D9 path's, in the same places).
    vec4 cloudSample = vec4(0.0);
    float wetWeight = LODfade * wetMask;
    vec3 refrLinPre = vec3(0.0);
    float vmSampleValidPre = 0.0;
    vec4 shadowsPre = vec4(0.0);
    if (fast) {
        cloudSample = texture(sampler2D(t_cloud, NVR_SAMPLER(pc.sMipWrap)), GetPuddleCloudUV(worldPosRelRoof, combinedNormals));
        refrLinPre = linearize(texture(sampler2D(t_source, NVR_SAMPLER(pc.sLin)), uv + refrOff).rgb);
        vmSampleValidPre = GetViewModelMask(saturate(uv + refrOff));
        shadowsPre = texture(sampler2D(t_pointShadow, NVR_SAMPLER(pc.sLin)), uv);

        // (shadowsPre.y < -1: never true; it only keeps the read above the branch)
        if (depth > DrawD || isWaterSurface || shadowsPre.y < -1.0)
            return vec4(WetOut(baseColorIn.rgb + splashAdd), baseColorIn.a);

        // the SSR trace condition of the main path below (same expressions)
        float fLinger = puddleFade * flatCoverage;
        float fSsrMask = saturate(max(rippleMask, max(fLinger, puddlemask))) * reflectionAllow;
        float fScreen = smoothstep(PuddleSSRScreenFadeStart, PuddleSSRScreenFadeFull, uv.y);
        float fAngle = 1.0 - smoothstep(PuddleSSRFadeStart, PuddleSSRFadeFull, saturate(dot(-V, up)));
        float fOpacity = fScreen * fAngle;
        float fTraceOpacity = saturate(PuddleSSRCandidateBlend) * saturate(fSsrMask * LODfade) * fOpacity;
        bool fTraces = fSsrMask > 0.001 && PuddleSSRCandidateBlend > 0.0 && fOpacity > 0.0
            && fTraceOpacity > PuddleSSRMinTraceOpacity;

        // without FixedReflectionTrace no trace runs (the zero-weight exit below), as the D3D9 fast path
        if (vmMask <= 0.5 && !(wetWeight > 0.0) && !(streakMask > 0.0) && !(fixTrace && fTraces))
            return vec4(WetOut(baseColor.rgb + splashAdd), 1.0);
    }

    if (!fast) {
        vec3 refrLin = linearize(texture(sampler2D(t_source, NVR_SAMPLER(pc.sLin)), uv + refrOff).rgb);

        vec3 baseLin = linearize(baseColor).rgb;
        vec3 rippleColor = mix(baseLin, refrLin, viewRefr);

        // First-person composition
        if (vmMask > 0.5) {
            vec2 vmRefrUV = saturate(uv + refrOff);
            float vmSampleValid = GetViewModelMask(vmRefrUV);

            vec3 vmRefrLin = mix(baseLin, refrLin, vmSampleValid);
            float vmRippleBlend = saturate(LODfade * rippleMask) * viewRefr;
            vec3 vmWetLin = mix(baseLin, vmRefrLin, vmRippleBlend);
            vec3 vmWetRGB = delinearize(vec4(vmWetLin, 1.0)).rgb;
            return vec4(WetOut(vmWetRGB + splashAdd), baseColor.a);
        }

        // Puddle sky lighting
        float sunAmbient = max(luma(linearize(c.sunAmbient)), 0.0001);
        vec4 shadows = texture(sampler2D(t_pointShadow, NVR_SAMPLER(pc.sLin)), uv);
        float inShadow = saturate(pow((shadows.r + shadows.g) / sunAmbient, 5.0));

        float sunHeight = max(dot(c.sunPosition.xyz, up), 0.0);

        vec3 fresnelColor = GetSkyColor(
            0.0, 0.5, sunHeight, 0.0, c.skyData.z,
            c.skyColor.rgb, c.skyLowColor.rgb, c.horizonColor.rgb, c.sunColor.rgb
            );

        float NdotV = saturate(dot(N, -V));
        float F = PuddleSkyMinReflect + (1.0 - PuddleSkyMinReflect)
        * pow(1.0 - NdotV, 5.0);

        float reflDist = 1.0 - smoothstepH(600.0, DrawD, depth);
        float reflStrength = saturate(puddlemask * F * PuddleSkyStrength
            * mix(1.0, 1.15, rainVis) * reflDist);

        vec3 puddleBase = rippleColor.rgb * mix(1.0, 0.55, c.wetWorldData.w);
        puddleBase = mix(rippleColor.rgb, puddleBase, puddlemask);

        vec3 waterTint = vec3(0.92, 0.97, 1.05);
        puddleBase *= mix(vec3(1.0), waterTint, depthish * 0.25);
        puddleBase *= (1.0 - depthish * 0.18);
        puddleBase *= (1.0 - rimDark);

        float reflMix = min(PuddleSkyMaxBlend, reflStrength * (1.0 + depthish * 0.20 + rimReflBoost));
        reflMix *= smoothstep(0.03, 0.12, puddlemask) * reflectionAllow;

        vec3 reflectionSky = max(fresnelColor, vec3(0.0));
        float reflectionSkyLuma = max(luma(reflectionSky), 0.0);
        reflectionSky = mix(vec3(reflectionSkyLuma), reflectionSky,
            saturate(PuddleSkyTintInfluence));
        vec3 skyReflColor = reflectionSky * PuddleSkyBrightness;
        float skyPeak = max(skyReflColor.r, max(skyReflColor.g, skyReflColor.b));
        skyReflColor /= (1.0 + skyPeak);

        float skyLuma = max(luma(skyReflColor), 0.0001);
        float skyLumaLimit = PuddleSkyLumaFloor + luma(baseLin) * PuddleSkyLumaScale;
        skyReflColor *= min(1.0, skyLumaLimit / skyLuma);

        // Puddle cloud reflection
        vec3 cloudSurface = c.cameraPosition.xyz + worldPosRelRoof.xyz;
        vec2 cloudUV = cloudSurface.xy / max(PuddleCloudWorldSize, 1.0);
        cloudUV += c.gameTime.z * PuddleCloudDrift;
        cloudUV += combinedNormals.xy * PuddleCloudRippleDistortion;
        cloudSample = texture(sampler2D(t_cloud, NVR_SAMPLER(pc.sMipWrap)), cloudUV);

        vec3 cloudDetail = saturate((cloudSample.rgb - 0.5) * PuddleCloudContrast + 0.5);
        float cloudAlpha = saturate((cloudSample.a - 0.5) * PuddleCloudContrast + 0.5);

        vec3 cloudLight = reflectionSky;
        cloudLight /= (1.0 + max(cloudLight.r, max(cloudLight.g, cloudLight.b)));
        cloudLight *= min(1.0, skyLumaLimit / max(luma(cloudLight), 0.0001));
        vec3 cloudReflection = cloudLight * PuddleCloudBrightness
        * mix(vec3(0.12), vec3(1.0), cloudDetail);
        float cloudCoverage = saturate(cloudAlpha * PuddleCloudStrength);
        skyReflColor = mix(skyReflColor, cloudReflection, cloudCoverage);
        vec3 puddleColor = mix(puddleBase, skyReflColor, reflMix);

        // Screen-space reflections
        float lingeringReflectionMask = puddleFade * flatCoverage;
        float ssrMask = saturate(max(rippleMask, max(lingeringReflectionMask, puddlemask))) * reflectionAllow;
        float ssrScreenOpacity = smoothstep(PuddleSSRScreenFadeStart,
            PuddleSSRScreenFadeFull, uv.y);
        float ssrDownAngle = saturate(dot(-V, up));
        float ssrAngleOpacity = 1.0 - smoothstep(PuddleSSRFadeStart, PuddleSSRFadeFull, ssrDownAngle);
        float ssrOpacity = ssrScreenOpacity * ssrAngleOpacity;
        float ssrTraceOpacity = saturate(PuddleSSRCandidateBlend)
            * saturate(ssrMask * LODfade) * ssrOpacity;
        vec3 ssrHit = vec3(0.0);
        // (technique 0 never gets here: the D3D9 pass runs the original trace; this path is FixedReflectionTrace only)
        if (ssrMask > 0.001 && PuddleSSRCandidateBlend > 0.0 && ssrOpacity > 0.0
            && ssrTraceOpacity > PuddleSSRMinTraceOpacity)
        {
            vec3 ssrNormal = normalize(mix(baseN, N, saturate(PuddleSSRNormalInfluence)));
            if (fixTrace)
                ssrHit = TraceWetReflectionFixed(uv, ssrNormal);
        }
        float ssrCoverage = saturate(ssrHit.z * ssrMask * LODfade) * ssrOpacity;

        // Sun highlights
        vec3 Ls = normalize(c.sunDirection.xyz);
        vec3 Hs = normalize(Ls - V);

        float sunNdotH = saturate(dot(N, Hs));
        float sunSpecPow = mix(mix(80.0, 200.0, specMask), 700.0, puddlemask);
        float sunSpec = pow(sunNdotH, sunSpecPow) * specMask;

        float sunSpecMul = mix(0.25, 0.70, saturate(sunHeight));
        vec3 sunSpecCol = linearize(c.sunColor).rgb * sunSpec * sunSpecMul * inShadow
        * mix(1.0, PuddleSunStrength, puddlemask);
        float sunHighlightLimit = mix(PuddleSunLimit, PuddleSunHighlightLimit, puddlemask);
        sunSpecCol = min(sunSpecCol, vec3(sunHighlightLimit));

        vec3 streakSunCol = vec3(0.0);
        if (streakMask > 0.0) {
            float streakSunSpec = pow(sunNdotH, StreakSunSpecularPower)
                * streakMask * StreakSpecularStrength;
            streakSunCol = linearize(c.sunColor).rgb * streakSunSpec * sunSpecMul * inShadow;
        }

        // Local light highlights
        vec3 pointSpecCol = vec3(0.0);
        vec3 streakPointCol = vec3(0.0);

        for (int i = 0; i < 12; i++)
        {
            if (c.shadowLightPosition[i].w > 0.0)
            {
                vec3 L = c.shadowLightPosition[i].xyz - worldPos.xyz;
                float dist = length(L);
                L /= max(dist, 1e-4);

                float ld = dist / max(c.shadowLightPosition[i].w, 1e-4);
                float att = saturate(1.0 - ld);
                att *= att;

                vec3 H = normalize(L - V);

                float ndh = saturate(dot(N, H));
                float specLobe = pow(ndh, 30.0);
                float spec = specLobe * specMask;

                vec3 lampCol = c.lightColor[i].rgb;
                pointSpecCol += lampCol * spec * att;
                float streakSpec = specLobe * streakMask * StreakSpecularStrength;
                streakPointCol += lampCol * streakSpec * att;
            }

            if (c.lightPosition[i].w > 0.0)
            {
                vec3 L = c.lightPosition[i].xyz - worldPos.xyz;
                float dist = length(L);
                L /= max(dist, 1e-4);

                float ld = dist / max(c.lightPosition[i].w, 1e-4);
                float att = saturate(1.0 - ld);
                att *= att;

                vec3 H = normalize(L - V);

                float ndh = saturate(dot(N, H));
                float specLobe = pow(ndh, 30.0);
                float spec = specLobe * specMask;

                vec3 lampCol = c.lightColor[i + 12].rgb;
                pointSpecCol += lampCol * spec * att;
                float streakSpec = specLobe * streakMask * StreakSpecularStrength;
                streakPointCol += lampCol * streakSpec * att;
            }
        }

        pointSpecCol *= mix(1.0, PuddleLightStrength, puddlemask);
        float pointHighlightLimit = mix(PuddleLightLimit, PuddlePointHighlightLimit, puddlemask);
        pointSpecCol = min(pointSpecCol, vec3(pointHighlightLimit));

        // Final composition
        vec3 finalPuddle = puddleColor + sunSpecCol + pointSpecCol;

        vec4 outCol = vec4(finalPuddle, 1.0);
        outCol = delinearize(outCol);

        vec3 wetRGB = mix(baseColor.rgb, outCol.rgb, LODfade * wetMask);

        if (ssrHit.z > (fixTrace ? 0.001 : 0.5) && PuddleSSRCandidateBlend > 0.0)
        {
            vec3 reflectionRGB = textureLod(sampler2D(t_source, NVR_SAMPLER(pc.sLin)), ssrHit.xy, 0.0).rgb;
            float reflectionBlend = saturate(PuddleSSRCandidateBlend) * ssrCoverage;
            wetRGB = mix(wetRGB, reflectionRGB, reflectionBlend);
        }

        // Streak lighting composition
        if (streakMask > 0.0)
        {
            vec3 streakLighting = min(streakSunCol + streakPointCol,
                vec3(StreakHighlightLimit * streakMask));
            vec3 wetLinear = linearize(vec4(wetRGB, 1.0)).rgb;
            wetRGB = delinearize(vec4(wetLinear + streakLighting * LODfade, 1.0)).rgb;
        }
        return vec4(WetOut(wetRGB + splashAdd), 1.0);
    }

    // FastPaths (fast == true): the same composition, with real branches. Pixels reaching this point are not far, not water,
    // and have a puddle weight, a streak or an SSR trace (or are first-person).
    vec3 baseLin = linearize(baseColor).rgb;

    // First-person composition
    if (vmMask > 0.5)
    {
        vec3 vmRefrLin = mix(baseLin, refrLinPre, vmSampleValidPre);
        float vmRippleBlend = saturate(LODfade * rippleMask) * viewRefr;
        vec3 vmWetLin = mix(baseLin, vmRefrLin, vmRippleBlend);
        vec3 vmWetRGB = delinearize(vec4(vmWetLin, 1.0)).rgb;
        return vec4(WetOut(vmWetRGB + splashAdd), baseColor.a);
    }

    float sunAmbient = max(luma(linearize(c.sunAmbient)), 0.0001);
    vec4 shadows = shadowsPre;
    float inShadow = saturate(pow((shadows.r + shadows.g) / sunAmbient, 5.0));

    float sunHeight = max(dot(c.sunPosition.xyz, up), 0.0);

    // Screen-space reflections (FixedReflectionTrace only: the D3D9 fast path has no other trace)
    float lingeringReflectionMask = puddleFade * flatCoverage;
    float ssrMask = saturate(max(rippleMask, max(lingeringReflectionMask, puddlemask))) * reflectionAllow;
    float ssrScreenOpacity = smoothstep(PuddleSSRScreenFadeStart,
        PuddleSSRScreenFadeFull, uv.y);
    float ssrDownAngle = saturate(dot(-V, up));
    float ssrAngleOpacity = 1.0 - smoothstep(PuddleSSRFadeStart, PuddleSSRFadeFull, ssrDownAngle);
    float ssrOpacity = ssrScreenOpacity * ssrAngleOpacity;
    float ssrTraceOpacity = saturate(PuddleSSRCandidateBlend)
        * saturate(ssrMask * LODfade) * ssrOpacity;
    vec3 ssrHit = vec3(0.0);
    if (ssrMask > 0.001 && PuddleSSRCandidateBlend > 0.0 && ssrOpacity > 0.0
        && ssrTraceOpacity > PuddleSSRMinTraceOpacity)
    {
        vec3 ssrNormal = normalize(mix(baseN, N, saturate(PuddleSSRNormalInfluence)));
        if (fixTrace)
            ssrHit = TraceWetReflectionFixed(uv, ssrNormal);
    }
    float ssrCoverage = saturate(ssrHit.z * ssrMask * LODfade) * ssrOpacity;

    // Sun highlights (the half vector and the strength are shared with the streaks)
    vec3 Ls = normalize(c.sunDirection.xyz);
    vec3 Hs = normalize(Ls - V);
    float sunNdotH = saturate(dot(N, Hs));
    float sunSpecMul = mix(0.25, 0.70, saturate(sunHeight));

    vec3 streakSunCol = vec3(0.0);
    if (streakMask > 0.0)
    {
        float streakSunSpec = pow(sunNdotH, StreakSunSpecularPower)
            * streakMask * StreakSpecularStrength;
        streakSunCol = linearize(c.sunColor).rgb * streakSunSpec * sunSpecMul * inShadow;
    }

    // Local light highlights: a lamp slot that is empty (w = 0) is skipped for the whole screen, a lamp that does not reach
    // the pixel (attenuation 0) for that pixel. Same accumulation order as the main path.
    vec3 pointSpecCol = vec3(0.0);
    vec3 streakPointCol = vec3(0.0);

    float anyLamp = 0.0;
    for (int j = 0; j < 12; j++)
        anyLamp = max(anyLamp, max(c.shadowLightPosition[j].w, c.lightPosition[j].w));
    if (anyLamp > 0.0)
    {
        for (int i = 0; i < 12; i++)
        {
            AddLampHighlightFast(c.shadowLightPosition[i], c.lightColor[i].rgb, worldPos.xyz, V, N, specMask, streakMask,
                pointSpecCol, streakPointCol);
            AddLampHighlightFast(c.lightPosition[i], c.lightColor[i + 12].rgb, worldPos.xyz, V, N, specMask, streakMask,
                pointSpecCol, streakPointCol);
        }
    }

    vec3 wetRGB = baseColor.rgb;
    if (wetWeight > 0.0)
    {
        // Refraction
        vec3 refrLin = refrLinPre;
        vec3 rippleColor = mix(baseLin, refrLin, viewRefr);

        // Puddle sky lighting
        vec3 fresnelColor = GetSkyColor(
            0.0, 0.5, sunHeight, 0.0, c.skyData.z,
            c.skyColor.rgb, c.skyLowColor.rgb, c.horizonColor.rgb, c.sunColor.rgb
            );

        float NdotV = saturate(dot(N, -V));
        float F = PuddleSkyMinReflect + (1.0 - PuddleSkyMinReflect)
        * pow(1.0 - NdotV, 5.0);

        float reflDist = 1.0 - smoothstepH(600.0, DrawD, depth);
        float reflStrength = saturate(puddlemask * F * PuddleSkyStrength
            * mix(1.0, 1.15, rainVis) * reflDist);

        vec3 puddleBase = rippleColor.rgb * mix(1.0, 0.55, c.wetWorldData.w);
        puddleBase = mix(rippleColor.rgb, puddleBase, puddlemask);

        vec3 waterTint = vec3(0.92, 0.97, 1.05);
        puddleBase *= mix(vec3(1.0), waterTint, depthish * 0.25);
        puddleBase *= (1.0 - depthish * 0.18);
        puddleBase *= (1.0 - rimDark);

        float reflMix = min(PuddleSkyMaxBlend, reflStrength * (1.0 + depthish * 0.20 + rimReflBoost));
        reflMix *= smoothstep(0.03, 0.12, puddlemask) * reflectionAllow;

        vec3 reflectionSky = max(fresnelColor, vec3(0.0));
        float reflectionSkyLuma = max(luma(reflectionSky), 0.0);
        reflectionSky = mix(vec3(reflectionSkyLuma), reflectionSky,
            saturate(PuddleSkyTintInfluence));
        vec3 skyReflColor = reflectionSky * PuddleSkyBrightness;
        float skyPeak = max(skyReflColor.r, max(skyReflColor.g, skyReflColor.b));
        skyReflColor /= (1.0 + skyPeak);

        float skyLuma = max(luma(skyReflColor), 0.0001);
        float skyLumaLimit = PuddleSkyLumaFloor + luma(baseLin) * PuddleSkyLumaScale;
        skyReflColor *= min(1.0, skyLumaLimit / skyLuma);

        // Puddle cloud reflection (its texture read is above the branches)
        vec3 cloudDetail = saturate((cloudSample.rgb - 0.5) * PuddleCloudContrast + 0.5);
        float cloudAlpha = saturate((cloudSample.a - 0.5) * PuddleCloudContrast + 0.5);

        vec3 cloudLight = reflectionSky;
        cloudLight /= (1.0 + max(cloudLight.r, max(cloudLight.g, cloudLight.b)));
        cloudLight *= min(1.0, skyLumaLimit / max(luma(cloudLight), 0.0001));
        vec3 cloudReflection = cloudLight * PuddleCloudBrightness
        * mix(vec3(0.12), vec3(1.0), cloudDetail);
        float cloudCoverage = saturate(cloudAlpha * PuddleCloudStrength);
        skyReflColor = mix(skyReflColor, cloudReflection, cloudCoverage);
        vec3 puddleColor = mix(puddleBase, skyReflColor, reflMix);

        // Sun highlight
        float sunSpecPow = mix(mix(80.0, 200.0, specMask), 700.0, puddlemask);
        float sunSpec = pow(sunNdotH, sunSpecPow) * specMask;
        vec3 sunSpecCol = linearize(c.sunColor).rgb * sunSpec * sunSpecMul * inShadow
        * mix(1.0, PuddleSunStrength, puddlemask);
        float sunHighlightLimit = mix(PuddleSunLimit, PuddleSunHighlightLimit, puddlemask);
        sunSpecCol = min(sunSpecCol, vec3(sunHighlightLimit));

        pointSpecCol *= mix(1.0, PuddleLightStrength, puddlemask);
        float pointHighlightLimit = mix(PuddleLightLimit, PuddlePointHighlightLimit, puddlemask);
        pointSpecCol = min(pointSpecCol, vec3(pointHighlightLimit));

        // Final composition
        vec3 finalPuddle = puddleColor + sunSpecCol + pointSpecCol;

        vec4 outCol = vec4(finalPuddle, 1.0);
        outCol = delinearize(outCol);

        wetRGB = mix(baseColor.rgb, outCol.rgb, wetWeight);
    }

    if (ssrHit.z > (fixTrace ? 0.001 : 0.5) && PuddleSSRCandidateBlend > 0.0)
    {
        vec3 reflectionRGB = textureLod(sampler2D(t_source, NVR_SAMPLER(pc.sLin)), ssrHit.xy, 0.0).rgb;
        float reflectionBlend = saturate(PuddleSSRCandidateBlend) * ssrCoverage;
        wetRGB = mix(wetRGB, reflectionRGB, reflectionBlend);
    }

    // Streak lighting composition
    if (streakMask > 0.0)
    {
        vec3 streakLighting = min(streakSunCol + streakPointCol,
            vec3(StreakHighlightLimit * streakMask));
        vec3 wetLinear = linearize(vec4(wetRGB, 1.0)).rgb;
        wetRGB = delinearize(vec4(wetLinear + streakLighting * LODfade, 1.0)).rgb;
    }
    return vec4(WetOut(wetRGB + splashAdd), 1.0);
}

void main() {
    o_color = WetPass(vUV, (pc.mode & 1u) != 0u, (pc.mode & 2u) != 0u);
}
