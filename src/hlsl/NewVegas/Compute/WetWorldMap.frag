// WetWorld's first three passes as a Vulkan pass (needs NVR DXVK): WetMap, then BlurWetMap across and down, line for line
// Effects\WetWorld.fx.hlsl (effects/WetWorld.cpp ExtRender). passIndex: 0 WetMap, 1 BlurWetMap (1, 0), 2 BlurWetMap (0, 1).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer (LINEAR, CLAMP): the image the blur reads
layout(set = 1, binding = 1) uniform texture2D t_depth;      // TESR_DepthBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 2) uniform texture2D t_depthVM;    // TESR_DepthBufferViewModel (LINEAR, CLAMP)
layout(set = 1, binding = 3) uniform texture2D t_ortho;      // TESR_OrthoMapBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 4) uniform texture2D t_normals;    // TESR_NormalsBuffer (point, CLAMP)
// UNOFFICIAL 2026-11-12 PuddlesLandOnly: TESR_MaterialBuffer (point, CLAMP; core/CapturedNormals.h: r class, g bound radius, a written).
// Read by the wet-map pass only while c.wetWorldLook.y says so (ExtRender sends the mode as 0 when the buffer holds nothing this
// frame, and binds the depth texture as a placeholder).
layout(set = 1, binding = 5) uniform texture2D t_material;

layout(push_constant) uniform Push {
    uint sLin;          // LINEAR, CLAMP (heap index, filled in by the extension)
    uint sPoint;        // POINT, CLAMP
    uint sMipWrap;      // not used by these passes
    uint sMipClamp;     // not used by these passes
    uint passIndex;     // 0 WetMap, 1 blur across, 2 blur down
    uint gbufferV2;     // UNOFFICIAL 2026-11-16 fd-batchCG: 1 = the material buffer is in layout v2 (core/CapturedNormals.h V2Compiled)
    uint pad[2];
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 o_color;

#include "WetWorldCommon.glsl"

// UNOFFICIAL 2026-11-12 PuddlesLandOnly / WeaponRainHandOff / ReflectionsOwnPuddles: GetSurfaceWetClass of WetWorld.fx.hlsl, line for
// line (what the wet pass may put on this pixel: 1 everything, 1.5 everything but the puddle's own mirror image, 0.5 the wet look
// without puddles, 0 nothing). c.wetWorldLook: y the mode (0 off, 1 ground-like, 2 land only) + 4 with WeaponRainHandOff, z
// PuddlesMinObjectSize, w 1 while the Reflections effect owns the puddles' mirror image.
// UNOFFICIAL 2026-11-16 fd-batchCG (DEFERRED-GAPS-AUDIT item 4): + the GBufferV2 decode of WetWorld.fx.hlsl's GBUFFER_V2 branch, chosen
// by pc.gbufferV2 (the texel alone cannot tell the layouts apart: a v2 object without the specular bit reads as an old class).
float GetSurfaceWetClass(vec2 uv, float vmMask) {
    float wetClass = 1.0;
    float weaponOff = step(3.5, c.wetWorldLook.y);
    float mode = c.wetWorldLook.y - 4.0 * weaponOff;
    if (mode > 0.5) {
        vec4 material = textureLod(sampler2D(t_material, NVR_SAMPLER(pc.sPoint)), uv, 0.0);
        float written, kind, radius;
        if (pc.gbufferV2 != 0u) {
            // GBufferV2 (core/CapturedNormals.h): r the shading model (+16 specular), written = r > 0.5; a is a normal's y and the bound
            // radius is gone: every object counts as large enough (PuddlesMinObjectSize has no effect with it)
            written = step(0.5, material.r);
            kind = floor(material.r + 0.01);
            kind -= 16.0 * step(15.5, kind);
            kind = (kind == 8.0 || kind == 9.0) ? 2.0 : kind;	// hair, eyes: an actor's, as worn gear
            radius = c.wetWorldLook.z;
        } else {
            float share = clamp(material.a, 0.5, 1.0);
            written = step(0.5, material.a) * step(0.5, material.r);
            kind = floor(material.r / share + 0.01);
            radius = material.g / share;
        }
        float isActor = (kind == 2.0 || kind == 6.0) ? 1.0 : 0.0;
        float isLand = (kind == 4.0) ? 1.0 : 0.0;
        float isObject = (kind == 1.0) ? 1.0 : 0.0;
        float groundLike = max(isLand, isObject * step(c.wetWorldLook.z, radius));
        float puddleSurface = mode > 1.5 ? isLand : groundLike;
        float unwritten = mode > 1.5 ? 0.5 : 1.0;
        float known = isActor > 0.5 ? 0.0 : mix(0.5, 1.0, puddleSurface);
        wetClass = written > 0.5 ? known : unwritten;
    }
    wetClass = mix(wetClass, 1.0 - weaponOff, vmMask);
    return wetClass + 0.5 * step(0.75, wetClass) * step(0.5, c.wetWorldLook.w);
}

// Wet-map pass
vec4 WetMap(vec2 uv) {
    float depth = readDepth(uv);
    if (depth > DrawD)
        return vec4(0.0, 0.0, 0.0, 1.0);
    vec4 surfacePosition = GetOrthoSurfacePosition(uv);
    float vmMask = GetViewModelMask(uv);
    float wetClass = GetSurfaceWetClass(uv, vmMask);	// UNOFFICIAL 2026-11-12 PuddlesLandOnly (1 with the keys off)
    float coverage = ComputeDepressionMask(surfacePosition, depth, vmMask) * step(0.75, wetClass);
    float dotUp = dot(GetWorldNormal(uv), vec3(0.0, 0.0, 1.0));
    vec4 roofPosition = surfacePosition * c.shadowToLight;
    float flatCoverage = GetRoofMask(roofPosition) * smoothstep(0.65, 0.92, dotUp)
        * (1.0 - vmMask);
    float flatWetCoverage = flatCoverage * smoothstep(0.90, 0.97, dotUp);
    return vec4(coverage, flatCoverage, flatWetCoverage, wetClass);
}

// Wet-map blur pass
vec4 BlurWetMap(vec2 uv, vec2 offsetMask) {
    float blurRadius = PuddleBlurRadius;
    float centerDepth = readDepth(uv);
    vec2 blurStep = c.reciprocalResolution.xy * offsetMask * max(blurRadius, 0.0) * 0.5;
    float depthTolerance = max(PuddleBlurDepthTolerance * max(centerDepth, 1.0), 0.0001);
    float weightSum = 0.375;
    vec4 centre = texture(sampler2D(t_rendered, NVR_SAMPLER(pc.sLin)), uv);	// UNOFFICIAL 2026-11-12 PuddlesLandOnly: a = the pixel's class, not blurred
    vec3 coverage = centre.rgb * weightSum;
    for (int tap = -2; tap <= 2; tap++) {
        if (tap != 0) {
            vec2 sampleUV = saturate(uv + blurStep * float(tap));
            float sampleDepth = readDepth(sampleUV);
            float depthWeight = 1.0 - smoothstep(depthTolerance * 0.5,
                depthTolerance, abs(sampleDepth - centerDepth));
            float kernelWeight = abs(tap) == 1 ? 0.25 : 0.0625;
            float weight = kernelWeight * depthWeight;
            coverage += texture(sampler2D(t_rendered, NVR_SAMPLER(pc.sLin)), sampleUV).rgb * weight;
            weightSum += weight;
        }
    }
    coverage /= weightSum;
    return vec4(coverage, centre.a);
}

void main() {
    if (pc.passIndex == 0u) o_color = WetMap(vUV);
    else if (pc.passIndex == 1u) o_color = BlurWetMap(vUV, vec2(1.0, 0.0));
    else o_color = BlurWetMap(vUV, vec2(0.0, 1.0));
}
