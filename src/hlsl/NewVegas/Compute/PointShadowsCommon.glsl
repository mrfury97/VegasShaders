// PointShadows (needs NVR DXVK): the functions the point-light passes share, line for line with Effects/PointShadows.fx.hlsl and
// its Includes (Helpers, Depth, Normals, Shadows). effects/PointShadows.cpp fills the constants block from the values the D3D9 path
// uploads. The matrices are kept as stored: cb.M[i][j] is the HLSL's M[i][j]; HLSL mul(v, M) is cb.M * v, HLSL mul(M, v) is v * cb.M.
// Every texture read is at level 0 (the buffers have one level, so this is what tex2D and tex2Dlod read too); the dynamic branches
// of the .fx need the explicit level.
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 2, binding = 0, std140) uniform Constants {
    vec4 shadowLightPosition[12];   // TESR_ShadowLightPosition: xyz position, w radius
    vec4 lightPosition[12];         // TESR_LightPosition
    vec4 lightColor[24];            // TESR_LightColor
    vec4 extraLights[16];           // TESR_ExtraLights
    vec4 shadowFade;                // TESR_ShadowFade
    vec4 pointShadowNear;           // TESR_PointShadowNear
    vec4 spotLightPosition;         // TESR_SpotLightPosition
    vec4 spotLightDirection;        // TESR_SpotLightDirection
    vec4 spotLightColor;            // TESR_SpotLightColor
    vec4 lampContact;               // TESR_LampContact
    vec4 lampContactFade;           // TESR_LampContactFade
    vec4 lampContactBias;           // TESR_LampContactBias
    vec4 lampContactNoise;          // TESR_LampContactNoise
    vec4 reciprocalResolution;      // TESR_ReciprocalResolution
    vec4 cameraPosition;            // TESR_CameraPosition
    vec4 cameraData;                // TESR_CameraData: x near, y far
    mat4 viewTransform;             // TESR_ViewTransform
    mat4 projectionTransform;       // TESR_ProjectionTransform
    // UNOFFICIAL 2026-10-26 (PointShadows.cpp FillPreshader): what the D3D9 effect framework's preshaders work out once per draw.
    // Worked out here per pixel, with full-precision divisions, they cost 0.3-0.5 ms in Lucky 38 (GTX 1070).
    vec4 shadowLightPre[12];        // x 1 / radius, y weight (luma(lightColor) * w), z 1 / near fade (0: none)
    vec4 lightPre[12];              // x 1 / radius, y weight (lightColor[i + 12])
    vec4 extraPre[16];              // x 1 / radius, y weight (both unpacked from extraLights[e].w)
    vec4 nearPre;                   // x 1 / pointShadowNear.y, y 1 / (1 - pointShadowNear.z), z 1 / spot radius, w spot weight
    vec4 spotPre;                   // x cos(spot angle), y 1 / (cos(half of it) - x)
    vec4 renderScale;               // TESR_RenderScale (xy scale, zw 1/scale)
} cb;

const float BIAS = 0.018;   // Shadows.hlsl

// HLSL smoothstep for any edges (GLSL's is undefined when edge0 > edge1)
float hlslSmoothstep(float a, float b, float x) {
    float t = clamp((x - a) / (b - a), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// smoothstep's curve on an already scaled t ((x - a) * 1 / (b - a), the reciprocal from the preshader values)
float smooth01(float t) {
    t = clamp(t, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// Depth.hlsl: readDepth (the stored linear depth times farZ)
float readDepthAt(texture2D depthTex, sampler samp, vec2 uv) {
    return textureLod(sampler2D(depthTex, samp), uv, 0.0).x * cb.cameraData.y;
}

// Depth.hlsl: toWorld
vec3 toWorld(vec2 tex) {
    vec3 v = vec3(cb.viewTransform[0][2], cb.viewTransform[1][2], cb.viewTransform[2][2]);
    v += (1.0 / cb.projectionTransform[0][0] * (2.0 * tex.x - 1.0)) * vec3(cb.viewTransform[0][0], cb.viewTransform[1][0], cb.viewTransform[2][0]);
    v += (-1.0 / cb.projectionTransform[1][1] * (2.0 * tex.y - 1.0)) * vec3(cb.viewTransform[0][1], cb.viewTransform[1][1], cb.viewTransform[2][1]);
    return v;
}

// Normals.hlsl: GetWorldNormal (the normal as stored, turned into world space by HLSL mul(TESR_ViewTransform, normal))
vec3 worldNormalAt(texture2D normalsTex, sampler samp, vec2 uv) {
    vec4 normal = vec4(textureLod(sampler2D(normalsTex, samp), uv, 0.0).xyz * 2.0 - 1.0, 1.0);
    return (normal * cb.viewTransform).xyz;
}

// Shadows.hlsl: GetPointLightAmountValue, explicit LOD (PointLightAmountValueLod in the .fx). invNearFade: 1 / the near fade
// (smoothstep(0, nearFade, lightDepth)), 0 when there is none.
float pointLightAmountValueLod(textureCube cubeTex, sampler samp, vec3 lightDir, float dn, float invNearFade) {
    if (cb.shadowFade.z == 0.0) return 1.0;
#ifdef PS_TEST_NO_CUBES
    return 1.0;   // TEST variant (variants.txt, NvrDxvkTestSuffix): no cube map reads
#endif

    float lightDepth = textureLod(samplerCube(cubeTex, samp), lightDir, 0.0).r;
    float shadow = lightDepth + BIAS * dn > dn ? 1.0 : 0.0;
    float blocker = invNearFade > 0.0 ? smooth01(lightDepth * invNearFade) : 1.0;
    return mix(1.0, shadow, float(lightDepth > 0.0 && lightDepth < 1.0) * blocker);
}

// PointShadows.fx: EdgeWindow (smoothstep(1 - near.y, 1, dn))
float edgeWindow(float dn) {
    return cb.pointShadowNear.y > 0.0 ? 1.0 - smooth01((dn - (1.0 - cb.pointShadowNear.y)) * cb.nearPre.x) : 1.0;
}

// PointShadows.fx: LampReach (smoothstep(near.z, 1, dn))
float lampReach(float dn) {
    return 1.0 - smooth01((dn - cb.pointShadowNear.z) * cb.nearPre.y);
}

// Shadows.hlsl: GetPointLightAtten (smoothstep(0, 0.2, dn) = dn * 5, as fxc folds it)
float getPointLightAtten(vec3 lightDir, float dn, vec3 n) {
    float s = clamp(dn * dn, 0.0, 1.0);
    float atten = clamp(((1.0 - s) * (1.0 - s)) / (1.0 + 5.0 * s), 0.0, 1.0);

    vec3 l = normalize(lightDir);
    float diffuse = mix(1.0, dot(l, n), smooth01(dn * 5.0));

    return clamp(diffuse * atten, 0.0, 1.0);
}

// PointShadows.fx: LampAmount
float lampAmount(vec3 lightDir, float dn, vec3 n) {
    // (2026-10-25 ForwardReachMask 2, as the .fx) pointShadowNear.z < 0: the old attenuation without its angle term (the normal along the lamp).
    if (cb.pointShadowNear.z < 0.0) return getPointLightAtten(lightDir, dn, normalize(lightDir));
    return cb.pointShadowNear.z > 0.0 ? lampReach(dn) : getPointLightAtten(lightDir, dn, n);
}

// PointShadows.fx: ShadowedLight (one shadow-casting light, its cube map read behind the same exact test). pre: cb.shadowLightPre[i].
float shadowedLight(textureCube cubeTex, sampler samp, vec3 worldPos, vec4 lightPos, vec4 pre, vec3 n) {
    vec3 lightDir = lightPos.xyz - worldPos;
    float dn = length(lightDir) * pre.x;
    float weight = pre.y;
    if (!(dn < 1.0) || weight == 0.0) return 0.0;

    float amount = pointLightAmountValueLod(cubeTex, samp, lightDir * vec3(-1.0, -1.0, 1.0), dn, pre.z) * lampAmount(lightDir, dn, n);
    return clamp(clamp(amount, 0.0, 1.0) * weight, 0.0, 1.0) * edgeWindow(dn);
}

// Shadows.hlsl: GetPointLightDistance (xyz direction to the light, w normalised distance; invRadius from the preshader values)
vec4 pointLightDistance(vec3 worldPos, vec3 lightPos, float invRadius) {
    vec3 lightDir = lightPos - worldPos;
    return vec4(lightDir, length(lightDir) * invRadius);
}

// PointShadows.fx: GetSpotLightAmount (its radius and cone from the preshader values; pow(x, 2) = x * x, as fxc writes it)
float spotLightAmount(vec3 worldPos, vec4 spotPos, vec4 spotDir, vec3 n) {
    vec3 lightToWorld = spotPos.rgb - worldPos;
    vec3 lightVector = normalize(lightToWorld);

    float dn = length(lightToWorld) * cb.nearPre.z;
    float s = clamp(dn * dn, 0.0, 1.0);
    float atten = clamp(((1.0 - s) * (1.0 - s)) / (1.0 + 5.0 * s), 0.0, 1.0);

    float cone = clamp((clamp(dot(spotDir.xyz, lightVector * -1.0), 0.0, 1.0) - cb.spotPre.x) * cb.spotPre.y, 0.0, 1.0);
    cone *= cone;

    float diffuse = max(dot(lightVector, n), 0.0);
    return diffuse * cone * atten;
}
