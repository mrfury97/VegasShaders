// Underwater pass 4, the water (needs NVR DXVK): Effects/Underwater.fx.hlsl Water line for line. effects/Underwater.cpp ExtRender.
// t_rendered is the current image (the blurred god rays in its upper left quarter), t_source the frame before the effect.
// Depth: CombineDepth's; normals: TESR_NormalsBuffer (POINT, CLAMP); t_shadow: TESR_PointShadowBuffer (ANISOTROPIC, WRAP).
#version 450
#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_samplerless_texture_functions : enable
#include "Common.glsl"
#include "Underwater.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 1) uniform texture2D t_source;     // TESR_SourceBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 2) uniform texture2D t_depth;      // TESR_DepthBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 3) uniform texture2D t_normals;    // TESR_NormalsBuffer (POINT, CLAMP)
layout(set = 1, binding = 4) uniform texture2D t_caustics;   // TESR_CausticsSampler (ANISOTROPIC, WRAP)
layout(set = 1, binding = 5) uniform texture2D t_shadow;     // TESR_PointShadowBuffer (ANISOTROPIC, WRAP)

layout(push_constant) uniform Push {
    uint linearIndex;     // 0: LINEAR, CLAMP
    uint pointIndex;      // 4: POINT, CLAMP
    uint anisoIndex;      // 8: ANISOTROPIC, WRAP
    uint pad;             // 12
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

// Underwater.fx.hlsl ditherMat (row, column)
const float ditherMat[16] = float[16](
    0.0588, 0.5294, 0.1765, 0.6471,
    0.7647, 0.2941, 0.8824, 0.4118,
    0.2353, 0.7059, 0.1176, 0.5882,
    0.9412, 0.4706, 0.8235, 0.3259);

void main() {
    vec2 minUV = 0.5 * cb.reciprocalResolution.xy;
    vec2 maxUV = max(cb.renderScale.xy - minUV, minUV);
    vec2 ruv = clamp(uv * cb.renderScale.xy, minUV, maxUV);
    float depth = readDepth(ruv, t_depth, pc.linearIndex);
    vec3 eyeVector = toWorld(uv);
    vec3 eyeDirection = normalize(eyeVector);
    vec3 worldPos = cb.cameraPosition.xyz + eyeVector * depth;
    float fogDepth = length(eyeVector * depth); // a depth measure that's at the same distance from the camera at all angles
    float waterDepth = cb.waterSettings.x - worldPos.z;
    vec3 floorNormal = getWorldNormal(ruv, t_normals, pc.pointIndex);

    vec4 color = texture(sampler2D(t_source, NVR_SAMPLER(pc.linearIndex)), ruv);
    if (aboveWater(cb.cameraPosition.z, worldPos.z, cb.waterSettings.x)) {
        o_color = color;
        return;
    }

    color = linearize(color); // linearise

    // vertical exponential depth fog used to darken bottom
    float density = 0.08;
    float falloff = 0.00225;
    float fogAmount = clamp((density / falloff) * exp(-cb.cameraPosition.z * falloff) * (1.0 - exp(-fogDepth * eyeDirection.z * falloff)) / eyeDirection.z, 0.0, 1.0);

    vec3 waterDeepColor = linearize(cb.waterDeepColor).rgb;
    vec3 waterShallowColor = linearize(cb.waterShallowColor).rgb;
    vec3 sunColor = linearize(cb.sunColor).rgb;
    float sunL = sunLuma();
    float causticsStrength = cb.waterVolume.x;
    float depthDarkness = cb.waterSettings.y;
    vec3 extCoeff = cb.waterCoefficients.xyz;
    float scattCoeff = cb.waterCoefficients.w;
    float turbidity = cb.waterVolume.z;
    float godraysStrength = cb.waterVolume.w;

    // horizontal scattering
    vec3 skyColor = getSkyColor(eyeDirection, sunColor);

    // interpolate fog color between shallow and deep water based on viewing angle. Shallow waters get tinted with the sky
    float verticalFade = clamp(compressF(dot(eyeDirection, up) * 1.5), 0.0, 1.0); // fade from deep to shallow based on viewing angle
    float sunDirFade = compressF(dot(eyeDirection.xyz, cb.sunDirection.xyz));     // fade based on proximity to the sun angle

    vec3 fogColor = mix(waterDeepColor, waterShallowColor + skyColor * 0.2, verticalFade); // mix in some sky color into shallow fog
    fogColor *= sunL * (0.4 + (1.0 - 0.4) * sunDirFade) * extCoeff; // scale fog brightness with sunluma/sun direction and apply user setting modifiers
    vec3 scattering = (1.0 - fogColor) * scattCoeff; // lerp colors at different speed
    float linearFog = clamp(invlerp(cb.waterFog.x, cb.waterFog.y, fogDepth), 0.0, 1.0);

    // fading parameters for caustics and god rays
    float floorAngle = (smoothstep(0.5, 1.0, dot(floorNormal, up)) + smoothstep(0.8, 1.0, dot(floorNormal, cb.sunDirection.xyz))) / 2.0;
    float depthFade = hsmooth(800.0, 0.0, waterDepth);
    float distanceFade = invlerp(6000.0, 0.0, fogDepth);

    vec2 shadows = texture(sampler2D(t_shadow, NVR_SAMPLER(pc.anisoIndex)), ruv).rg; // only sample point light shadows
    float caustics = getCaustics(worldPos, t_caustics, pc.anisoIndex) * shadows.r;

    color.rgb += color.rgb * pows(caustics, 2.0) * sunColor * depthFade * floorAngle * distanceFade * sunL * causticsStrength * 100.0;

    color.rgb = mix(color.rgb, fogColor, clamp(pow(invlerp(0.0, 5000.0, fogDepth), 10.0), 0.0, 1.0)); // lod hiding

    // blend fog layers colors
    color.rgb *= mix(vec3(1.0), waterDeepColor, clamp(fogAmount * pows(depthDarkness, 0.5), 0.0, 1.0) * (1.0 - shadows.g * distanceFade)); // surface light absorption

    float fogThickness = clamp(2.0 - pows(turbidity, 0.5) * 2.0, 0.0, 1.0);
    float turbidPower = (turbidity * 3.0) / (1.0 + 3.0 * turbidity);
    color.rgb = mix(color.rgb, fogColor, clamp(scattering * pows(linearFog, fogThickness) * turbidPower, 0.0, 1.0)); // scattering absorption

    // blend in godrays
    vec2 godraysMaxUV = max(0.5 * cb.renderScale.xy - minUV, minUV);
    color.rgb += texture(sampler2D(t_rendered, NVR_SAMPLER(pc.linearIndex)), clamp((uv * cb.renderScale.xy) * 0.5, minUV, godraysMaxUV)).r * sunColor * godraysStrength * color.rgb;

    // reduce banding
    vec2 pixel = (uv * cb.renderScale.xy) / cb.reciprocalResolution.xy;
    color = delinearize(color);
    int row = int(fmodHLSL(pixel.x, 4.0));
    int column = int(fmodHLSL(pixel.y, 4.0));
    color.rgb += ditherMat[row * 4 + column] / 255.0;

    o_color = vec4(color.rgb, 1.0);
}
