// Underwater's shared part (needs NVR DXVK): the constants and the Includes/Depth.hlsl, Normals.hlsl and Helpers.hlsl functions
// Effects/Underwater.fx.hlsl uses, for the four passes UnderwaterGodrays, UnderwaterBlur, UnderwaterWater and
// UnderwaterDistortion (each includes Common.glsl first). effects/Underwater.cpp ExtRender.
//
// Matrices: cb.viewTransform[i][j] is the HLSL's [i][j] (the D3DX matrix as stored, as SkinScattering.frag reads them), so
// HLSL mul(M, v) is `v * M` here and HLSL mul(v, M) is `M * v`.
// Textures and samplers are the caller's: the functions below take the texture and the sampler's heap index (the sampler is
// built at the point of use, as glslang requires).

layout(set = 2, binding = 0, std140) uniform Constants {
    vec4 reciprocalResolution;   // TESR_ReciprocalResolution (xy 1 / size)
    vec4 sunDirection;           // TESR_SunDirection
    vec4 gameTime;               // TESR_GameTime (x time, z frame time)
    vec4 waterCoefficients;      // TESR_WaterCoefficients (xyz extinction, w scattering)
    vec4 sunColor;               // TESR_SunColor
    vec4 waterFog;               // TESR_WaterFog (x near, y far)
    vec4 waterVolume;            // TESR_WaterVolume (x caustics, y shore, z turbidity, w god rays)
    vec4 waveParams;             // TESR_WaveParams (z speed)
    vec4 waterSettings;          // TESR_WaterSettings (x surface height, y depth darkness, z underwater flag)
    vec4 waterShallowColor;      // TESR_WaterShallowColor
    vec4 waterDeepColor;         // TESR_WaterDeepColor
    vec4 horizonColor;           // TESR_HorizonColor
    vec4 skyColor;               // TESR_SkyColor
    vec4 cameraPosition;         // TESR_CameraPosition
    vec4 cameraData;             // TESR_CameraData (y far Z)
    mat4 projectionTransform;    // TESR_ProjectionTransform
    mat4 viewTransform;          // TESR_ViewTransform
    vec4 renderScale;            // RenderRect scale (xy = scale, zw = 1 / scale)
} cb;

const vec3 up = vec3(0.0, 0.0, 1.0);

// HLSL macros of Helpers.hlsl and Depth.hlsl
float shade(vec3 n, vec3 l) { return max(dot(n, l), 0.0); }
float shades(vec3 n, vec3 l) { return clamp(dot(n, l), 0.0, 1.0); }
float compressF(float v) { return (v * 0.5) + 0.5; }
float invlerp(float a, float b, float t) { return (t - a) / (b - a); }

// HLSL smoothstep for any order of the edges (GLSL leaves edge0 > edge1 undefined)
float hsmooth(float a, float b, float x) {
    float t = clamp((x - a) / (b - a), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// HLSL % on floats (fmod: the sign of the dividend, not GLSL mod)
float fmodHLSL(float x, float y) { return x - y * trunc(x / y); }

float sunLuma() { return luma(pows(cb.sunColor.rgb, 2.2)); } // linearise

// Includes/Depth.hlsl toWorld
vec3 toWorld(vec2 tex) {
    vec3 v = vec3(cb.viewTransform[0][2], cb.viewTransform[1][2], cb.viewTransform[2][2]);
    v += (1.0 / cb.projectionTransform[0][0] * (2.0 * tex.x - 1.0)) * vec3(cb.viewTransform[0][0], cb.viewTransform[1][0], cb.viewTransform[2][0]);
    v += (-1.0 / cb.projectionTransform[1][1] * (2.0 * tex.y - 1.0)) * vec3(cb.viewTransform[0][1], cb.viewTransform[1][1], cb.viewTransform[2][1]);
    return v;
}

// readDepth: TESR_DepthBuffer (CombineDepth's), farZ = TESR_CameraData.y
float readDepth(vec2 coord, texture2D depth, uint samplerIndex) {
    return texture(sampler2D(depth, NVR_SAMPLER(samplerIndex)), coord).x * cb.cameraData.y;
}

// Includes/Normals.hlsl GetWorldNormal: TESR_NormalsBuffer (POINT, CLAMP)
vec3 getWorldNormal(vec2 uv, texture2D normals, uint samplerIndex) {
    vec4 normal = vec4(texture(sampler2D(normals, NVR_SAMPLER(samplerIndex)), uv).xyz * 2.0 - 1.0, 1.0);
    return (normal * cb.viewTransform).xyz;
}

// Underwater.fx.hlsl getCaustics: TESR_CausticsSampler (ANISOTROPIC, WRAP)
float getCaustics(vec3 pos, texture2D caustics, uint samplerIndex) {
    float depth = cb.waterSettings.z - pos.z;
    pos += (depth / cb.sunDirection.z) * cb.sunDirection.xyz; // we go to the surface in the direction of the sun to sample

    float speed = cb.gameTime.x * 0.01 * cb.waveParams.z;
    float scale = 0.002;

    float layer1 = texture(sampler2D(caustics, NVR_SAMPLER(samplerIndex)), pos.xy * scale + speed * normalize(vec2(-1.2, -2.5))).r;
    float layer2 = texture(sampler2D(caustics, NVR_SAMPLER(samplerIndex)), pos.xy * scale * 1.2 + speed * normalize(vec2(0.5, 2.0))).r;

    return pows(min(layer1, layer2), 1.3) * 4.0; // strengthen the definition and strength of the caustics
}

// lerp between sky colors to recreate the sky for refractions as vanilla buffer is tinted
vec3 getSkyColor(vec3 eyeDirection, vec3 sunColor) {
    vec3 skyColor = mix(linearize(cb.horizonColor).rgb, linearize(cb.skyColor).rgb, pow(shade(eyeDirection, up), 0.5));
    skyColor += sunColor * pows(shades(eyeDirection, cb.sunDirection.xyz), 12.0);
    return skyColor;
}

bool aboveWater(float eyeHeight, float pointHeight, float waterHeight) {
    if (eyeHeight < waterHeight - 4.0) return false;
    return eyeHeight > (waterHeight + 4.0) || pointHeight > (waterHeight - 0.6);
}
