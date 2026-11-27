// AmbientLight (needs NVR DXVK): what the eleven AmbientLight passes share, line for line with Effects/AmbientLight.fx.hlsl and its
// Includes (Helpers, Depth, Normals, SkyTake). effects/AmbientLight.cpp (ExtDraw) fills the constants block from the effect's own
// parameter values right before each draw (what the D3D9 pass would read). A matrix is kept as stored (ID3DXEffect::GetMatrix):
// cb.M[i][j] is the HLSL's M[i][j]; HLSL mul(v, M) is cb.M * v, HLSL mul(M, v) is v * cb.M.
// The texture coordinate is the one the D3D9 frame quad interpolates (frameUV): uv = pixel / target size + 0.5 / full size, as DXVK
// puts D3D9's pixel centres (the half-resolution, stats and grid atlas passes are not at the quad's own resolution).
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 2, binding = 0, std140) uniform Constants {
    mat4 projectionTransform;    // TESR_ProjectionTransform
    mat4 invProjectionTransform; // TESR_InvProjectionTransform
    mat4 viewTransform;          // TESR_ViewTransform
    mat4 reproject;              // NVR_ALReproject
    mat4 gridSunToGrid;          // NVR_GridSunToGrid
    mat4 gridToSun;              // NVR_GridToSun
    vec4 data;                   // TESR_AmbientLightData
    vec4 data2;                  // TESR_AmbientLightData2
    vec4 reciprocalResolution;   // TESR_ReciprocalResolution
    vec4 alLayout;               // NVR_ALLayout
    vec4 alFlags;                // NVR_ALFlags
    vec4 alInterior;             // NVR_ALInterior
    vec4 alScene;                // NVR_ALScene
    vec4 alTemporal;             // NVR_ALTemporal
    vec4 alEdges;                // NVR_ALEdges
    vec4 waterSettings;          // TESR_WaterSettings
    vec4 shadowData;             // TESR_ShadowData
    vec4 shadowFade;             // TESR_ShadowFade
    vec4 skyColor;               // TESR_SkyColor
    vec4 sunAmbient;             // TESR_SunAmbient
    vec4 sunDirection;           // TESR_SunDirection
    vec4 aoSky5;                 // TESR_AmbientOcclusionSky5
    vec4 aoData;                 // TESR_AmbientOcclusionData
    vec4 depthConstants;         // TESR_DepthConstants
    vec4 cameraData;             // TESR_CameraData
    vec4 cameraPosition;         // TESR_CameraPosition
    vec4 sunFarFlags;            // NVR_SunFarFlags
    vec4 gridOrigin;             // NVR_GridOrigin
    vec4 gridOrigin1;            // NVR_GridOrigin1
    vec4 gridData;               // NVR_GridData
    vec4 gridLight;              // NVR_GridLight
    vec4 gridStep;               // NVR_GridStep
    vec4 gridShift;              // NVR_GridShift
    vec4 gridScreen;             // NVR_GridScreen
    vec4 gridWalls;              // NVR_GridWalls
    vec4 gridLampInfo;           // NVR_GridLampInfo
    vec4 gridLamps[32];          // NVR_GridLamps
    vec4 gridLampColors[32];     // NVR_GridLampColors
    vec4 renderScale;            // TESR_RenderScale (xy active RenderRect scale; 0 -> 1.0)
    vec4 alReach;                // NVR_ALReach (z AlbedoFallbackCap, w RtFarFade)
    vec4 alBump;                 // NVR_ALBump (x 1 = DeferredBumpedNormals shading normals bound)
} cb;

layout(push_constant) uniform Push {
    vec4 frame;          // 0: xy 1 / the pass's viewport size, zw the frame quad's texture offset (0.5 / full width, height)
    uint pointClamp;     // 16: POINT, CLAMP
    uint linearClamp;    // 20: LINEAR, CLAMP
    uint pointWrap;      // 24: POINT, WRAP (the blue noise)
    uint variant;        // 28: bit 0 the technique's uniform argument (SmoothNormal alongY, Blur axis y, Apply sky); bit 1 see the pass
#ifdef NVR_GI
    // UNOFFICIAL 2026-10-27 (deep/fork2-ssgi) ComputeGI's GI pass only (AmbientLightEstimate.frag with -DNVR_GI; effects/AmbientLight.cpp
    // ALExtPushGI, 48 bytes). Every other pass is compiled without it (its 32-byte block).
    vec4 gi;             // 32: x thickness (world units), y unused, z strength, w 1 = ComputeGIThinOcclusion
#endif
} pc;

#define POINT_S  NVR_SAMPLER(pc.pointClamp)
#define LINEAR_S NVR_SAMPLER(pc.linearClamp)

// IN.UVCoord at this pixel (see the top).
vec2 frameUV() { return floor(gl_FragCoord.xy) * pc.frame.xy + pc.frame.zw; }

// tex2Dlod(sampler, float4(uv, 0, 0)) through a POINT or LINEAR sampler
vec4 pointLod(texture2D t, vec2 uv) { return textureLod(sampler2D(t, POINT_S), uv, 0.0); }
vec4 linearLod(texture2D t, vec2 uv) { return textureLod(sampler2D(t, LINEAR_S), uv, 0.0); }

// HLSL fmod (sign of x): x - y * trunc(x / y)
float hlslFmod(float x, float y) { return x - y * trunc(x / y); }

// ---- AmbientLight.fx.hlsl's statics ----
#define Radius            max(cb.data.x, 1.0)
#define OcclusionStrength (cb.data.y)
#define BounceStrength    (cb.data.z)
#define DrawDistance      max(cb.data.w, 100.0)
#define DarkestAO         saturate(cb.data2.x)
#define BrightFade        (cb.data2.y)
#define BlurStep          max(cb.data2.z, 0.5)
#define DebugView         (cb.data2.w)
const int SLICES = 2;
const int STEPS = 4;
const float BounceScale = 2.5;
const float NearFade = 40.0;
const float MaxScreenRadius = 0.08;
const float endFade = 8000.0;

// ---- Depth.hlsl ----
#define nearZ (cb.cameraData.x)
#define farZ  (cb.cameraData.y)

float projectedDepthFromLinear(float linear01) {
    float viewZ = max(linear01 * farZ, 1.0e-6);
    return nearZ * (farZ - viewZ) / (viewZ * (farZ - nearZ));
}

// readDepthLod / readDepth (TESR_DepthBuffer: POINT, CLAMP, one level)
float readDepthLod(texture2D depthTex, vec2 coord) { return pointLod(depthTex, coord).x * farZ; }

vec3 projectPosition(vec3 position) {
    vec4 projection = cb.projectionTransform * vec4(position, 1.0);   // mul(float4(position, 1), TESR_ProjectionTransform)
    projection.xyz /= projection.w;
    projection.x = projection.x * 0.5 + 0.5;
    projection.y = 0.5 - 0.5 * projection.y;
    return projection.xyz;
}

vec3 toWorld(vec2 tex) {
    vec3 v = vec3(cb.viewTransform[0][2], cb.viewTransform[1][2], cb.viewTransform[2][2]);
    v += (1.0 / cb.projectionTransform[0][0] * (2.0 * tex.x - 1.0)) * vec3(cb.viewTransform[0][0], cb.viewTransform[1][0], cb.viewTransform[2][0]);
    v += (-1.0 / cb.projectionTransform[1][1] * (2.0 * tex.y - 1.0)) * vec3(cb.viewTransform[0][1], cb.viewTransform[1][1], cb.viewTransform[2][1]);
    return v;
}

// ---- Normals.hlsl: GetWorldNormalLod, mul(TESR_ViewTransform, normal).xyz (TESR_NormalsBuffer: POINT) ----
vec3 GetWorldNormalLod(texture2D normalsTex, vec2 uv) {
    vec4 normal = vec4(pointLod(normalsTex, uv).xyz * 2.0 - 1.0, 1.0);
    return (normal * cb.viewTransform).xyz;
}

// ---- SkyTake.hlsl ----
vec3 SkyTake(vec3 linearColor, float factor, float visibility, float cap, float noLimit) {
    float take = min(1.0 - factor, ((1.0 - visibility) * cap + noLimit) / max(luma(linearColor), 1.0e-6));
    return linearColor * (1.0 - take);
}

// ---- AmbientLight.fx.hlsl ----
bool IsViewModel(texture2D viewModelTex, vec2 uv) {
    float d = pointLod(viewModelTex, uv).x;
    return (cb.depthConstants.z == 0.0 && d < 0.99999) || (cb.depthConstants.z > 0.0 && d > 0.0);
}

vec3 PositionFromLinear(vec2 uv, float linear01) {
    float z = projectedDepthFromLinear(linear01);
    vec4 viewSpace = cb.invProjectionTransform * vec4(uv.x * 2.0 - 1.0, (1.0 - uv.y) * 2.0 - 1.0, z, 1.0);   // mul(v, TESR_InvProjectionTransform)
    return viewSpace.xyz / viewSpace.w;
}

vec3 PositionFromViewZ(vec2 uv, float z) {
    return vec3((uv.x * 2.0 - 1.0) / cb.projectionTransform[0][0], (1.0 - uv.y * 2.0) / cb.projectionTransform[1][1], 1.0) * z;
}

float Pack2(vec2 v) { v = floor(clamp(v, 0.0, 1.0) * 4095.0 + 0.5); return v.x * 4096.0 + v.y; }
vec2 Unpack2(float p) { float high = floor(p / 4096.0); return vec2(high, p - high * 4096.0) / 4095.0; }
vec2 OctEncode(vec3 n) {
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    vec2 s = vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    vec2 e = n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * s;
    return e * 0.5 + 0.5;
}
vec3 OctDecode(vec2 e) {
    e = e * 2.0 - 1.0;
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    float t = saturate(-n.z);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}

// ---- the light grid ----
const vec3 GridGroundColor = vec3(0.55, 0.42, 0.30);
const float GridX = 32.0, GridY = 32.0, GridZ = 16.0;
const float AtlasSize = 128.0;
const vec2 AtlasDims = vec2(256.0, 128.0);
const float SunMapSize = 256.0;
const int GridLampsMax = 32;
const float GridLampReflectance = 0.4;

vec4 CosineLobe(vec3 n) { return vec4(0.886227, 1.023328 * n); }
vec4 ShBasis(vec3 d) { return vec4(0.282095, 0.488603 * d); }

vec2 CellUV(vec3 c, float level) {
    float tile = c.z;
    vec2 t = vec2(hlslFmod(tile, 4.0), floor(tile / 4.0));
    return (t * 32.0 + c.xy + 0.5 + vec2(level * AtlasSize, 0.0)) / AtlasDims;
}
vec2 CellUV(vec3 c) { return CellUV(c, 0.0); }

vec3 AtlasCell(vec2 uv) {
    vec2 texel = floor(uv * AtlasDims);
    texel.x -= texel.x >= AtlasSize ? AtlasSize : 0.0;
    vec2 t = floor(texel / 32.0);
    return vec3(texel - t * 32.0, t.y * 4.0 + t.x);
}

// GridLevel / GridBounce (NVR_GridAccum, NVR_GridAccumColor: LINEAR)
vec4 GridLevel(texture2D accumTex, texture2D accumColorTex, vec3 cameraRelative, vec3 normal, vec4 origin, float level, float edgeCells,
    out vec4 colourLight, out float fade) {
    float size = origin.w;
    vec3 g = (cameraRelative - origin.xyz + normal * (0.5 * size)) / size - 0.5;
    vec3 dims = vec3(GridX, GridY, GridZ);
    vec3 edge = min(g + 0.5, dims - 0.5 - g);
    fade = saturate(min(min(edge.x, edge.y), edge.z) / edgeCells);
    colourLight = vec4(0.0);
    if (fade <= 0.0) return vec4(0.0);
    g = clamp(g, vec3(0.0), dims - 1.0);
    float z0 = floor(g.z);
    float z1 = min(z0 + 1.0, GridZ - 1.0);
    float fz = g.z - z0;
    vec4 a = linearLod(accumTex, CellUV(vec3(g.xy, z0), level));
    vec4 b = linearLod(accumTex, CellUV(vec3(g.xy, z1), level));
    if (cb.gridData.w > 0.5) {
        vec4 ca = linearLod(accumColorTex, CellUV(vec3(g.xy, z0), level));
        vec4 cb_ = linearLod(accumColorTex, CellUV(vec3(g.xy, z1), level));
        colourLight = mix(ca, cb_, fz);
    }
    return mix(a, b, fz);
}

vec3 GridBounce(texture2D accumTex, texture2D accumColorTex, vec3 cameraRelative, vec3 normal) {
    vec4 c, cFine;
    float fade, fadeFine;
    vec4 sh = GridLevel(accumTex, accumColorTex, cameraRelative, normal, cb.gridOrigin, 0.0, 2.0, c, fade);
    if (cb.gridOrigin1.w > 0.0) {
        vec4 shFine = GridLevel(accumTex, accumColorTex, cameraRelative, normal, cb.gridOrigin1, 1.0, 4.0, cFine, fadeFine);
        sh = mix(sh, shFine, fadeFine);
        c = mix(c, cFine, fadeFine);
        fade = max(fade, fadeFine);
    }
    if (fade <= 0.0) return vec3(0.0);
    vec3 colour = vec3(1.0);
    if (cb.gridData.w > 0.5) {
        colour = c.a > 1e-5 ? mix(vec3(1.0), c.rgb / c.a, cb.alScene.w) : vec3(1.0);
    }
    float irradiance = max(dot(sh, CosineLobe(-normal)), 0.0) * 1.5;
    return irradiance * colour * fade;
}

const float MaxReflectance = 0.6;
vec3 SurfaceColourCap(vec3 lin, float cap) {
    vec3 ambient = max(cb.alScene.rgb, vec3(0.25 * luma(cb.alScene.rgb) + 1.0e-4));
    vec3 raw = max(lin, vec3(0.0)) / ambient;
    float level = luma(raw);
    vec3 hue = raw / max(level, 1.0e-4);
    float trust = saturate(cap / max(level, 1.0e-4));
    return min(level, cap) * min(mix(vec3(1.0), hue, trust), vec3(3.0));
}
vec3 SurfaceColour(vec3 lin) { return SurfaceColourCap(lin, MaxReflectance); }
float FallbackCap() { return cb.alFlags.z > 0.5 && cb.alReach.z > 0.0 ? cb.alReach.z : MaxReflectance; }
vec3 SurfaceAlbedo(texture2D albedoTex, vec2 uv, vec3 lin) {
    vec4 a = pointLod(albedoTex, uv);
    float take = step(0.5, cb.alFlags.z) * step(0.5, a.a);
    return mix(SurfaceColourCap(lin, FallbackCap()), clamp(a.rgb, vec3(0.0), vec3(0.85)), take);
}
vec3 MultiBounceAO(float v, vec3 albedo) {
    vec3 a =  2.0404 * albedo - 0.3324;
    vec3 b = -4.7951 * albedo + 0.6417;
    vec3 c =  2.7552 * albedo + 0.6903;
    return max(vec3(v), ((v * a + b) * v + c) * v);
}
