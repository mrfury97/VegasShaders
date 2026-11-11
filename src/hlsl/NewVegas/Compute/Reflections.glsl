// Reflections as Vulkan passes (needs NVR DXVK): what every pass of Effects/Reflections.fx.hlsl shares, line for line
// (effects/Reflections.cpp ExtRender). Each technique is its own .frag (ReflectionsTrace, ...Temporal, ...Blur, ...Composite,
// ...Capture, ...Filter, ...Material) including this file; all of them declare the same bindings, and a pass's C++ binds a
// placeholder to any texture it does not read (a pass never has its own target bound as an input).
// The matrices arrive as the D3D9 effect uploads them (ID3DXEffect::SetMatrix: the logical matrix), so HLSL M[i][j] is the
// same element here, mul(v, M) is M * v and mul((float3x3)M, v) is v * mat3(M).
// The compile-time switches of the .fx (REFL_MATERIAL_ON, REFL_ADD_ON, REFL_SPECOCC_ON) are cb.flags here, set from the same
// startup values the effect was compiled with; with a switch off, its code is skipped exactly as the preprocessor drops it.
// (Each .frag starts with #version 450 and GL_GOOGLE_include_directive before including this file.)
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;     // TESR_RenderedBuffer (LINEAR)
layout(set = 1, binding = 1) uniform texture2D t_depth;        // TESR_DepthBuffer (POINT)
layout(set = 1, binding = 2) uniform texture2D t_normals;      // TESR_NormalsBuffer (POINT)
layout(set = 1, binding = 3) uniform texture2D t_viewmodel;    // TESR_DepthBufferViewModel (POINT)
layout(set = 1, binding = 4) uniform texture2D t_trace;        // TESR_ReflectionsTrace (POINT)
layout(set = 1, binding = 5) uniform texture2D t_buffer;       // TESR_ReflectionsBuffer (POINT)
layout(set = 1, binding = 6) uniform texture2D t_temp;         // TESR_ReflectionsTemp (POINT)
layout(set = 1, binding = 7) uniform texture2D t_history;      // TESR_ReflectionsHistory (POINT)
layout(set = 1, binding = 8) uniform textureCube t_capture;    // TESR_ReflectionsCapture (POINT)
layout(set = 1, binding = 9) uniform textureCube t_captureNew; // TESR_ReflectionsCaptureNew (LINEAR)
layout(set = 1, binding = 10) uniform textureCube t_env;       // TESR_ReflectionsEnv (LINEAR, MIPFILTER LINEAR)
layout(set = 1, binding = 11) uniform texture2D t_ortho;       // TESR_OrthoMapBuffer (POINT)
layout(set = 1, binding = 12) uniform texture2D t_gridLight;   // TESR_ReflectionsGridLight (LINEAR)
layout(set = 1, binding = 13) uniform texture2D t_gridColor;   // TESR_ReflectionsGridColor (LINEAR)
layout(set = 1, binding = 14) uniform texture2D t_skyVis;      // NVR_ReflectionsSkyVis (LINEAR)
layout(set = 1, binding = 15) uniform texture2D t_material;    // NVR_ReflectionsMaterial (POINT)
layout(set = 1, binding = 23) uniform texture2D t_glass;       // UNOFFICIAL 2026-10-29 TESR_GlassMask (core/GlassMask.h; a placeholder when off)
layout(set = 1, binding = 24) uniform texture2D t_matbuf;      // UNOFFICIAL 2026-10-30 TESR_MaterialBuffer (core/CapturedNormals.h; a placeholder when off)
layout(set = 1, binding = 25) uniform texture2D t_cloudSky;    // UNOFFICIAL 2026-10-30 TESR_CloudSkyProbe (VolumetricClouds RenderSkyProbe; a placeholder when off)
// (binding 26: the RT passes' Rt.Bounce, declared by them)
layout(set = 1, binding = 27) uniform texture2D t_weaponMask;  // UNOFFICIAL 2026-11-09 TESR_WeaponMask (core/WeaponMask.h): red 1 on the first-person weapon's own pixels; a placeholder unless weapon.x is 2

layout(set = 2, binding = 0, std140) uniform Constants {
    mat4 projectionTransform;   // TESR_ProjectionTransform
    mat4 viewTransform;         // TESR_ViewTransform
    mat4 shadowOrtho;           // TESR_ShadowCameraToLightTransformOrtho
    mat4 reproject;             // TESR_ReflectionsReproject
    vec4 reciprocalResolution;  // TESR_ReciprocalResolution
    vec4 skyColor;              // TESR_SkyColor
    vec4 horizonColor;          // TESR_HorizonColor
    vec4 waterSettings;         // TESR_WaterSettings
    vec4 data;                  // TESR_ReflectionsData
    vec4 data2;                 // TESR_ReflectionsData2
    vec4 data3;                 // TESR_ReflectionsData3
    vec4 data4;                 // TESR_ReflectionsData4
    vec4 layoutData;            // TESR_ReflectionsLayout
    vec4 cube2;                 // TESR_ReflectionsCube2
    vec4 grid;                  // TESR_ReflectionsGrid
    vec4 grid2;                 // TESR_ReflectionsGrid2
    vec4 grid3;                 // TESR_ReflectionsGrid3
    vec4 material;              // TESR_ReflectionsMaterial
    vec4 depthConstants;        // TESR_DepthConstants
    vec4 cameraData;            // TESR_CameraData
    vec4 cameraPosition;        // TESR_CameraPosition
    vec4 sky;                   // NVR_ReflectionsSky (UNOFFICIAL 2026-10-31: z 1 = RT water this frame, w the water's roughness)
    vec4 flags;                 // x REFL_MATERIAL_ON, y REFL_ADD_ON, z REFL_SPECOCC_ON, w 1 = the material buffer is bound on stage 15
    vec4 material2;             // UNOFFICIAL 2026-10-30 x 1: TESR_MaterialBuffer's roughness for the floor guess (MaterialBufferRoughness),
                                // y 1: the cloud sky probe over the sky gradient (CloudSky), 2 (2026-10-31): t_cloudSky is the sky
                                // panorama (SkyPanorama, ReflectionsSkyPano.frag) instead, z the clouds' brightness (TESR_CloudsLayout.w)
    vec4 weapon;                // UNOFFICIAL 2026-10-30 RT weapon reflections ([Shaders.Reflections.RT] Weapon): x 1 on (RT traced this frame),
                                // y tan(world FOV / 2) / tan(1st-person FOV / 2), z the strength, w the roughness without material data
    vec4 renderScale;           // UNOFFICIAL 2026-11-03 TESR_RenderScale (Includes/RenderRect.hlsl): xy this frame's RenderScale rect, zw 1 / xy;
                                // (1, 1, 1, 1) in a native frame. Only the RT passes (RayGen, Resolve, SkyPano) run in rect frames.
    vec4 flashPos;              // UNOFFICIAL 2026-11-07 (perf-reflfix2) KeepFlashlight: the flashlight (FlashlightDeferred::LightParams) for the
    vec4 flashDir;              // composite: pos xyz camera-relative, w its reach (0: off this frame); dir xyz its axis, w cos of the cone's
    vec4 flashColor;            // edge; color rgb its light (the composite's linear space), w cos of the full-strength edge; look x Falloff
    vec4 flashLook;             // ([Shaders.Flashlight.Deferred]), y the cap on its highlight (linear)
    vec4 mirrorLamp;            // UNOFFICIAL 2026-11-08 (perf-reflfix3) MirrorLampHighlight (ReflLampHighlight.glsl): x 1 - MirrorLampHighlight (0: off),
    vec4 sunDirection;          // y the ring's radius (full-resolution pixels), z 1 = the sun is up (its glint kept); sunDirection xyz toward the sun (world)
    vec4 puddle;                // 2026-11-08 Puddles (src/effects/Puddles): TESR_PuddleData: x water level, y Coverage, z 1 / Size,
                                // w Quality (0: off, or ReflectionPuddles off)
    vec4 puddle2;               // x slope start, y slope full, z the water's roughness, w edge softness
} cb;

layout(push_constant) uniform Push {
    vec4 cube;                  // 0: TESR_ReflectionsCube (set per draw: the face, its size, the mip level, reset)
    uint linearIndex;           // 16: LINEAR, CLAMP
    uint pointIndex;            // 20: POINT, CLAMP
    uint mipIndex;              // 24: LINEAR, CLAMP, linear between mips (TESR_ReflectionsEnv)
    uint pass;                  // 28: Blur: 0 X (reads Temp), 1 Y (reads Trace); Composite: 1 = TESR_ReflectionsEnv is bound (DebugView 3)
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

#define LINEAR_S NVR_SAMPLER(pc.linearIndex)
#define POINT_S NVR_SAMPLER(pc.pointIndex)
#define MIP_S NVR_SAMPLER(pc.mipIndex)

#define MaterialOn (cb.flags.x > 0.5)
#define AddOn (cb.flags.y > 0.5)
#define SpecOccOn (cb.flags.z > 0.5)

#define DryReflect cb.data.x
#define Wetness cb.data.y
#define DryRoughness cb.data.z
#define WetRoughness cb.data.w
#define RayLength cb.data2.x
#define Steps cb.data2.y
#define ThicknessScale cb.data2.z
#define NoiseFrame cb.data2.w
#define HistoryWeight cb.data3.x
#define BlurRadius cb.data3.y
#define DebugView cb.data3.z
#define SkyFallback cb.data3.w
#define DrawDistance cb.data4.x
#define RoofMaskOn cb.data4.y
#define LowTexel cb.layoutData.xy
#define CubeFace pc.cube.x
#define CubeSize pc.cube.y
#define CubeMip pc.cube.z
#define CubeReset pc.cube.w
#define CaptureBlend cb.cube2.x
#define CubeFade cb.cube2.y
#define CubeLastMip cb.cube2.z
#define CubeOn cb.cube2.w
#define GridOn cb.grid2.x
const int RefineSteps = 5;
const float BrightestReflection = 16.0;   // linear: a lamp's core reflected in one texel cannot flash the whole puddle

// Includes/Depth.hlsl
#define nearZ cb.cameraData.x
#define farZ cb.cameraData.y

float readDepthLod(vec2 coord) { return textureLod(sampler2D(t_depth, POINT_S), coord, 0.0).x * farZ; }

// UNOFFICIAL 2026-11-03 RT reflections at RenderScale < 1 (docs/next/RT-RENDERSCALE.md): Includes/RenderRect.hlsl's rules, as the .fx
// follows them. In a rect frame the frame is the top-left renderScale.xy part of every screen-shaped texture: uv (texture coordinates)
// runs 0..renderScale.xy, the projection maths wants the screen point it shows (rectToScreen), and a projected point is read back at
// screenToRect of it. In a native frame renderScale is 1 and every function here changes nothing.
vec2 rectToScreen(vec2 uv) { return uv * cb.renderScale.zw; }
vec2 screenToRect(vec2 s01) { return s01 * cb.renderScale.xy; }

vec3 toWorld(vec2 tex) {
    tex = rectToScreen(tex);   // (2026-11-03, as Includes/Depth.hlsl)
    vec3 v = vec3(cb.viewTransform[0][2], cb.viewTransform[1][2], cb.viewTransform[2][2]);
    v += (1.0 / cb.projectionTransform[0][0] * (2.0 * tex.x - 1.0)) * vec3(cb.viewTransform[0][0], cb.viewTransform[1][0], cb.viewTransform[2][0]);
    v += (-1.0 / cb.projectionTransform[1][1] * (2.0 * tex.y - 1.0)) * vec3(cb.viewTransform[0][1], cb.viewTransform[1][1], cb.viewTransform[2][1]);
    return v;
}

// (smoothstep: every call in this effect has edge0 < edge1, where GLSL's is HLSL's formula.)
// HLSL fmod: x - y * trunc(x / y)
float fmodHlsl(float x, float y) { return x - y * trunc(x / y); }

// Interleaved gradient noise (Jimenez 2014) in [0, 1), moved on every frame.
float GradientNoise(vec2 pixel) {
    pixel += NoiseFrame * 5.588238;
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

// View-space position of a screen point at a linear depth (NVR's projection has no off-centre terms).
vec3 ViewPosition(vec2 uv, float depth) {
    uv = rectToScreen(uv);   // (2026-11-03: uv is a texture coordinate, as in the .fx)
    return vec3((uv.x * 2.0 - 1.0) / cb.projectionTransform[0][0], (1.0 - uv.y * 2.0) / cb.projectionTransform[1][1], 1.0) * depth;
}

vec2 ToScreen(vec3 view) {
    // (2026-11-03: a texture coordinate, as in the .fx: the rect's in a rect frame)
    return screenToRect(vec2(view.x * cb.projectionTransform[0][0] / view.z * 0.5 + 0.5, 0.5 - view.y * cb.projectionTransform[1][1] / view.z * 0.5));
}

bool IsWeapon(vec2 uv) {
    // The weapon's depth buffer is empty where there is no weapon: 1, or 0 with reversed depth (as WetWorld tests it).
    float viewmodelDepth = textureLod(sampler2D(t_viewmodel, POINT_S), uv, 0.0).x;
    return cb.depthConstants.z == 0.0 ? viewmodelDepth < 0.9 : viewmodelDepth > 0.01;
}

// UNOFFICIAL 2026-10-30 RT weapon reflections: the first-person weapon's pixels reflect too (only while RT traces this frame).
#define WeaponOn (cb.weapon.x > 0.5)
// UNOFFICIAL 2026-11-09 WeaponMask: weapon.x 2 = TESR_WeaponMask holds the weapon's own pixels (core/WeaponMask.h).
#define WeaponMasked (cb.weapon.x > 1.5)
// Any pixel of the first-person model while the weapon rays run: its depth is in the first-person FOV (SurfacePosition), it moves with the camera.
bool ViewModelAt(vec2 uv) { return WeaponOn && IsWeapon(uv); }
// The pixels that reflect as the weapon: the whole first-person model (hands, arms and gun: as before), or with the mask the weapon's own only.
bool WeaponAt(vec2 uv) {
    if (!ViewModelAt(uv)) return false;
    return !WeaponMasked || textureLod(sampler2D(t_weaponMask, POINT_S), uv, 0.0).r > 0.5;
}

vec3 ViewNormal(vec2 uv) { return normalize(textureLod(sampler2D(t_normals, POINT_S), uv, 0.0).xyz * 2.0 - 1.0); }

// View space -> world space direction: mul((float3x3)TESR_ViewTransform, v).
vec3 ViewToWorldDir(vec3 v) { return v * mat3(cb.viewTransform); }

const float RoofMargin = 40.0;   // world units; real cover (a porch roof, a marquee) is 100+ units above the floor
float OpenAbove(vec2 uv, float depth) {
    vec4 p = cb.shadowOrtho * vec4(toWorld(uv) * depth, 1.0);
    p.xyz /= p.w;
    if (abs(p.x) > 1.0 || abs(p.y) > 1.0 || p.z < 0.0 || p.z > 1.0) return 1.0;
    float top = textureLod(sampler2D(t_ortho, POINT_S), vec2(p.x * 0.5 + 0.5, p.y * -0.5 + 0.5), 0.0).r;
    // the map's depth per world unit along its view axis (Wet World's GetOrthoDepthScale)
    vec3 depthAxis = vec3(cb.shadowOrtho[0][2], cb.shadowOrtho[1][2], cb.shadowOrtho[2][2]);
    float perUnit = length(depthAxis) / max(abs(cb.shadowOrtho[3][3]), 1.0e-6);
    return top > p.z - max(RoofMargin * perUnit, 0.0001) ? 1.0 : 0.0;
}

// 2026-11-08 Puddles (src/effects/Puddles/README.md): the standing water the surface shaders draw (Shaders/Includes/Wetness.hlsl
// PuddleField), line for line, so the SSR and RT reflections mirror the same puddles at the water's roughness. Without the land's height
// term (no height map here) and the ortho map's soft edge (OpenAbove's hard test is applied by the caller). 0-1.
vec3 WetHash33(vec3 p3) {
    p3 = fract(p3 * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yxz + 33.33);
    return fract((p3.xxy + p3.yxx) * p3.zyx);
}
float PuddleHash(vec2 p) { return WetHash33(vec3(p, 7.13)).x; }
float PuddleValueNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = p - i;
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = PuddleHash(i), b = PuddleHash(i + vec2(1.0, 0.0));
    float c = PuddleHash(i + vec2(0.0, 1.0)), d = PuddleHash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}
#define PuddlesOn (cb.puddle.w > 0.5 && cb.puddle.x > 0.001)
float PuddleStanding(vec2 uv, float depth, float worldUp) {
    if (!PuddlesOn || depth > 10240.0) return 0.0;
    vec3 worldPos = cb.cameraPosition.xyz + toWorld(uv) * depth;
    vec2 p = worldPos.xy * cb.puddle.z;
    float n = PuddleValueNoise(p) * 0.62 + PuddleValueNoise(p * 2.31 + 17.7) * 0.38;
    if (cb.puddle.w > 2.5) n = n * 0.85 + PuddleValueNoise(p * 5.7 + 41.3) * 0.15;
    n = smoothstep(0.2, 0.8, n);
    float t = 1.0 - cb.puddle.x * cb.puddle.y;
    float shore = saturate((n - t) / max(cb.puddle2.w, 1.0e-3));
    float nearFactor = 1.0 - smoothstep(0.0, 10240.0, depth);
    return shore * shore * (3.0 - 2.0 * shore) * smoothstep(cb.puddle2.x, cb.puddle2.y, worldUp) * nearFactor;
}

// UNOFFICIAL 2026-10-30 a reflecting pixel's view-space position: ViewPosition, except on the first-person weapon, which the game draws
// with its own (narrower) field of view from the same eye: the same depth, the sideways offsets shrunk by the two FOVs' ratio.
vec3 SurfacePosition(vec2 uv, float depth) {
    vec3 p = ViewPosition(uv, depth);
    if (ViewModelAt(uv)) p.xy /= cb.weapon.y;   // (2026-11-09: every pixel of the first-person model, the hands too: they are drawn with the same FOV)
    return p;
}

// UNOFFICIAL 2026-11-05 (rtrefl-dir, the demo room's polished marble): a normal map with tilted tiles (ULInnerFloor) sends the mirror
// direction under the floor at a low view angle (half the disc's rays went down: the screen march found the floor itself, a blurred copy
// of the tiles, and RT rays met nothing). The reflected direction is kept above the real surface, the plane the depth buffer shows (from
// the neighbours' depths, the side with the smaller step at an edge): a direction under it is bent to just over it. Directions above
// the surface are unchanged. p: this pixel's view position (ViewPosition), R: the reflected direction, both view space.
vec3 AboveSurface(vec2 uv, vec3 p, vec3 R) {
    vec2 dx = vec2(LowTexel.x, 0.0), dy = vec2(0.0, LowTexel.y);
    vec3 xp = ViewPosition(uv + dx, readDepthLod(uv + dx)), xm = ViewPosition(uv - dx, readDepthLod(uv - dx));
    vec3 yp = ViewPosition(uv + dy, readDepthLod(uv + dy)), ym = ViewPosition(uv - dy, readDepthLod(uv - dy));
    vec3 ddx = abs(xp.z - p.z) < abs(p.z - xm.z) ? xp - p : p - xm;
    vec3 ddy = abs(yp.z - p.z) < abs(p.z - ym.z) ? yp - p : p - ym;
    vec3 n = cross(ddx, ddy);
    float len = length(n);
    if (!(len > 1.0e-6)) return R;
    n /= len;
    if (dot(n, p) > 0.0) n = -n;   // toward the eye
    float above = dot(R, n);
    const float minAbove = 0.03;   // ~2 degrees over the plane
    return above >= minAbove ? R : normalize(R + n * (minAbove - above));
}

// UNOFFICIAL 2026-10-31 RT water ([Shaders.Reflections.RT] Water): on a frame RT traces the water, water pixels reflect (RayGen casts,
// Resolve shades, the blurs run), but the composite leaves them alone: the water shaders read Buffer next frame in place of the planar map.
#define WaterRtOn (cb.sky.z > 0.5)
#define WaterPlanarSkipped (cb.sky.z > 1.5)   // UNOFFICIAL 2026-11-05: WaterSkipPlanar (no planar map for a water miss to fall back on)
bool SurfaceWater = false;    // set by Reflectivity: this pixel is the water (with WaterRtOn)
float WaterReflectivity(float depth, out float roughness) {
    if (!WaterRtOn) return 0.0;
    SurfaceWater = true;
    roughness = cb.sky.w;
    return 1.0;   // out to the far plane (rtrefl-24 og-lake: past DrawDistance the far water took the horizon colour, a bright band)
}

// REFL_MATERIAL_ON: the material buffer.
float SurfaceMetal = 0.0;     // set by Reflectivity: the surface's metal (0 without material data)
vec4 MaterialAt(vec2 uv) { return cb.material.x > 0.5 ? textureLod(sampler2D(t_material, POINT_S), uv, 0.0) : vec4(0.0); }

float MaterialReflectivity(vec2 uv, float depth, float floorShare, vec4 material, out float roughness) {
    float worldZ = cb.cameraPosition.z + toWorld(uv).z * depth;
    roughness = max(material.r, 0.02);
    if (floorShare > 0.0 && abs(worldZ - cb.waterSettings.x) < max(2.0, depth * 0.002)) return WaterReflectivity(depth, roughness);   // water reflects on its own (RT water: rays)
    float maxRough = cb.material.y;
    float dry = cb.material.z * (1.0 - smoothstep(0.6 * maxRough, maxRough, material.r));
    float wetness = Wetness * floorShare;
    float standing = floorShare > 0.0 ? PuddleStanding(uv, depth, ViewToWorldDir(ViewNormal(uv)).z) : 0.0;   // 2026-11-08 Puddles
    if ((wetness > 0.0 || standing > 0.0) && RoofMaskOn > 0.5) { float open = OpenAbove(uv, depth); wetness *= open; standing *= open; }   // the rain wets open ground only
    wetness = max(wetness, standing);
    float amount = dry + wetness;
    roughness = mix(roughness, WetRoughness, wetness / max(amount, 1.0e-4));
    roughness = mix(roughness, cb.puddle2.z, standing);
    SurfaceMetal = material.g;
    float fade = 1.0 - smoothstep(0.7 * DrawDistance, DrawDistance, depth);
    return min(amount, 2.0) * fade;
}

// UNOFFICIAL 2026-10-29 glass ([Shaders.Reflections.Main] GlassReflect = cb.material.w, 0 off): the glass mask's pixels are smooth glass
// facing any way (the depth and the rebuilt normal there are the glass's: FNV's glass writes depth). The Fresnel of the composite does the rest.
const float GlassRoughness = 0.03;

// How much a surface reflects (before Fresnel) and how rough it is. 0 where it never reflects.
float ReflectivityOwn(vec2 uv, float depth, vec3 viewNormal, out float roughness) {
    roughness = DryRoughness;
    if (MaterialOn) SurfaceMetal = 0.0;
    // UNOFFICIAL 2026-10-31 RT water: the water (flat, at the water height) at any distance, before DrawDistance's cut.
    if (WaterRtOn && depth < farZ * 0.98 && !IsWeapon(uv) && ViewToWorldDir(viewNormal).z > 0.75 &&
        abs(cb.cameraPosition.z + toWorld(uv).z * depth - cb.waterSettings.x) < max(2.0, depth * 0.002)) return WaterReflectivity(depth, roughness);
    if (depth >= DrawDistance || depth >= farZ * 0.98) return 0.0;
    if (IsWeapon(uv)) {
        // UNOFFICIAL 2026-10-30 RT weapon reflections: off, the weapon never reflects here (its own chrome pass does). On: its roughness
        // and metal from MaterialAware where its texture has a map, else the roughness the game lit it with (TESR_MaterialBuffer), else
        // the Weapon roughness; the strength x (1 - roughness) on the last two (a matte stock reflects little). The composite's Fresnel
        // (metal tinted by the weapon's own colour) lays it over the weapon's own reflection.
        // (2026-11-09 WeaponMask: with the mask, only the weapon's own pixels; the hands, gloves and arms of the first-person model do not reflect)
        if (!WeaponAt(uv)) return 0.0;
        if (MaterialOn) {
            vec4 material = MaterialAt(uv);
            if (material.b > 0.5) { roughness = max(material.r, 0.02); SurfaceMetal = material.g; return cb.weapon.z; }
        }
        roughness = cb.weapon.w;
        // (2026-10-30: the first-person pass never writes MaterialAware's buffer (world scene only), so a gun has no metal data:
        // WeaponMetal guesses it from the gun's own lit colour: grey steel (low saturation) metal, brown wood and coloured parts not.)
        if (cb.material2.w > 0.0) {
            vec3 lit = linearize(textureLod(sampler2D(t_rendered, POINT_S), uv, 0.0).rgb);
            float top = max(lit.r, max(lit.g, lit.b));
            float saturation = top > 1.0e-4 ? (top - min(lit.r, min(lit.g, lit.b))) / top : 0.0;
            SurfaceMetal = cb.material2.w * (1.0 - smoothstep(0.12, 0.35, saturation));
        }
        if (cb.material2.x > 0.5) {
            vec4 written = textureLod(sampler2D(t_matbuf, POINT_S), uv, 0.0);
            if (written.r > 0.5 && written.b >= 0.0) roughness = saturate(written.b);
        }
        return cb.weapon.z * (1.0 - roughness);
    }
    if (cb.material.w > 0.0 && textureLod(sampler2D(t_glass, POINT_S), uv, 0.0).r > 0.5) {
        roughness = GlassRoughness;
        return cb.material.w * (1.0 - smoothstep(0.7 * DrawDistance, DrawDistance, depth));
    }
    vec3 worldNormal = ViewToWorldDir(viewNormal);
    float floorShare = saturate((worldNormal.z - 0.75) / 0.2);
    if (MaterialOn) {
        vec4 material = MaterialAt(uv);
        if (material.b > 0.75) return MaterialReflectivity(uv, depth, floorShare, material, roughness);
    }
    if (floorShare <= 0.0) return 0.0;
    float worldZ = cb.cameraPosition.z + toWorld(uv).z * depth;
    if (abs(worldZ - cb.waterSettings.x) < max(2.0, depth * 0.002)) return WaterReflectivity(depth, roughness);   // water reflects on its own (RT water: rays)
    float wetness = Wetness;
    float standing = PuddleStanding(uv, depth, worldNormal.z);   // 2026-11-08 Puddles: the standing water mirrors after the rain too
    if ((wetness > 0.0 || standing > 0.0) && RoofMaskOn > 0.5) { float open = OpenAbove(uv, depth); wetness *= open; standing *= open; }   // the rain wets open ground only
    wetness = max(wetness, standing);
    float amount = DryReflect + wetness;
    // UNOFFICIAL 2026-10-30 MaterialBufferRoughness: where the game's shader wrote the roughness it lit the surface with (TESR_MaterialBuffer:
    // r class > 0, b roughness, -1 none), the dry surface takes it instead of the one DryRoughness (polished floors sharp, carpets blurred).
    float dryRoughness = DryRoughness;
    // UNOFFICIAL 2026-10-31: the copy with MaterialAware on carries it already (b 0.5, core/CapturedNormals.h MergeIntoReflMaterial)
    if (MaterialOn) { vec4 merged = MaterialAt(uv); if (merged.b > 0.25 && merged.b <= 0.75) { dryRoughness = max(merged.r, 0.02); SurfaceMetal = merged.g; } }
    if (cb.material2.x > 0.5) {
        vec4 written = textureLod(sampler2D(t_matbuf, POINT_S), uv, 0.0);
        if (written.r > 0.5 && written.b >= 0.0) dryRoughness = saturate(written.b);
    }
    roughness = mix(dryRoughness, WetRoughness, wetness / max(amount, 1.0e-4));
    roughness = mix(roughness, cb.puddle2.z, standing);
    float fade = 1.0 - smoothstep(0.7 * DrawDistance, DrawDistance, depth);
    return min(amount, 2.0) * floorShare * fade;   // above 1: stronger than a real surface, for taste
}

// UNOFFICIAL 2026-11-05 [Shaders.Reflections.Main] Mirror (cb.grid3.w): the roughness the surface's own material gives (the game's
// shader in TESR_MaterialBuffer, MaterialAware's maps) used to win over the Look's Roughness, so "Mirror (showcase)" left the demo room's
// marble rough tile by tile (its gloss map): rays scattered, most pixels borrowed a neighbour's, their reach cut by RoughReach, and with RT
// alone almost nothing showed (rtrefl-only). With Mirror on, every reflecting surface but the water and the weapon is pulled toward the
// Look's Roughness by Mirror (only ever smoother). Mirror 0: as before.
// (2026-11-06) MirrorSmoothOnly (cb.grid3.w negative): only surfaces whose own roughness is under 0.05 (a mirror panel, roughness 0.02 by
// RoughnessOverrides) get Mirror; the rest reflect as with Mirror 0. SurfaceMirror carries the surface's share to the composite.
float SurfaceMirror = 0.0;
float Reflectivity(vec2 uv, float depth, vec3 viewNormal, out float roughness) {
    float amount = ReflectivityOwn(uv, depth, viewNormal, roughness);
    float mirror = saturate(abs(cb.grid3.w));
    if (cb.grid3.w < 0.0 && roughness >= 0.05) mirror = 0.0;
    if (amount <= 0.0 || SurfaceWater || IsWeapon(uv)) mirror = cb.grid3.w < 0.0 ? 0.0 : mirror;
    SurfaceMirror = mirror;
    if (mirror > 0.0 && amount > 0.0 && !SurfaceWater && !IsWeapon(uv)) roughness = mix(roughness, min(roughness, DryRoughness), mirror);
    return amount;
}

// Split-sum environment BRDF (Karis's fit) for a dielectric (F0 0.04).
float EnvironmentFresnel(float roughness, float NdotV) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
    vec2 ab = vec2(-1.04, 1.04) * a004 + r.zw;
    return 0.04 * ab.x + ab.y;
}

// The same with any F0 (MaterialAware: metal).
vec3 EnvironmentFresnelF0(float roughness, float NdotV, vec3 F0) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
    vec2 ab = vec2(-1.04, 1.04) * a004 + r.zw;
    return F0 * ab.x + ab.y;
}

// UNOFFICIAL 2026-10-31 SkyPanorama ([Shaders.Reflections.RT]): the sky as seen (ReflectionsSkyPano.frag), hemi-octahedral, z up; below
// the horizon the horizon's (a ray that met nothing heading slightly down still ends in the far haze, not in black).
#define SkyPanoOn (cb.material2.y > 1.5)
vec3 PanoAlong(vec3 worldDir) {
    vec3 d = normalize(vec3(worldDir.xy, max(worldDir.z, 0.0)) + vec3(0.0, 0.0, 1.0e-4));
    vec3 v = d / (abs(d.x) + abs(d.y) + d.z);
    return textureLod(sampler2D(t_cloudSky, LINEAR_S), vec2(v.x + v.y, v.x - v.y) * 0.5 + 0.5, 0.0).rgb;
}

// The sky along a world direction (below the horizon, nothing).
vec3 SkyAlong(vec3 worldDir) {
    if (SkyPanoOn) return PanoAlong(worldDir) * smoothstep(0.0, 0.15, worldDir.z);   // (2026-10-31: the old fallbacks keep their horizon fade)
    float up = saturate(worldDir.z);
    vec3 sky = mix(linearize(cb.horizonColor.rgb), linearize(cb.skyColor.rgb), sqrt(up));
    // UNOFFICIAL 2026-10-30 CloudSky: the clouds over that part of the sky, from the cloud sky probe (hemi-octahedral, z up), laid on as
    // the clouds' own composite lays them on the screen (sky x transmittance + their light x brightness).
    if (cb.material2.y > 0.5 && worldDir.z > 0.0) {
        vec3 v = worldDir / (abs(worldDir.x) + abs(worldDir.y) + worldDir.z);
        vec4 clouds = textureLod(sampler2D(t_cloudSky, LINEAR_S), vec2(v.x + v.y, v.x - v.y) * 0.5 + 0.5, 0.0);
        sky = sky * clouds.a + clouds.rgb * cb.material2.z;
    }
    return sky * smoothstep(0.0, 0.15, worldDir.z);
}

// Karis's reversible compression for averaging HDR samples: c / (1 + luminance), and back.
vec3 Tame(vec3 c) { return c / (1.0 + dot(c, vec3(0.2126, 0.7152, 0.0722))); }
vec3 Untame(vec3 t) { return t / max(1.0 - dot(t, vec3(0.2126, 0.7152, 0.0722)), 1.0e-3); }

// World direction through texel st (-1..1, s right, t down) of a cube face, in Direct3D's face order and layout.
vec3 FaceDirection(float face, vec2 st) {
    if (face < 0.5) return vec3(1.0, -st.y, -st.x);
    if (face < 1.5) return vec3(-1.0, -st.y, st.x);
    if (face < 2.5) return vec3(st.x, 1.0, st.y);
    if (face < 3.5) return vec3(st.x, -1.0, -st.y);
    if (face < 4.5) return vec3(st.x, -st.y, 1.0);
    return vec3(-st.x, -st.y, -1.0);
}

// mul(float4(w, 0.0), TESR_ViewTransform).xyz
vec3 WorldToViewDir(vec3 w) { return (cb.viewTransform * vec4(w, 0.0)).xyz; }

// The reflection cube along a world direction, blurred for the roughness: rgb colour, a how much of it was ever seen.
vec4 EnvAlong(vec3 worldDir, float roughness) {
    float lod = roughness * CubeLastMip;
    vec4 sharp = textureLod(samplerCube(t_env, MIP_S), worldDir, lod);
    vec4 coarse = textureLod(samplerCube(t_env, MIP_S), worldDir, min(lod + 2.0, CubeLastMip));
    vec4 m = sharp + coarse * (1.0 - saturate(sharp.a));
    return vec4(m.rgb / max(m.a, 1.0e-4), saturate(m.a));
}

// ---- The light grid (AmbientLight's GridBounce) as the far / off-screen fallback ----
const float GridX = 32.0, GridY = 32.0, GridZ = 16.0;
const float GridAlbedo = 0.4;   // a typical surface (desert ground, plaster) for the ambient share
const float GridIrradianceScale = 1.5;   // AmbientLight's GridBounce reads the grid x 1.5 (its spreading loses light)
vec4 GridShBasis(vec3 d) { return vec4(0.282095, 0.488603 * d); }
vec2 GridCellUV(vec3 c) { return (vec2(fmodHlsl(c.z, 4.0), floor(c.z / 4.0)) * 32.0 + c.xy + 0.5) / vec2(256.0, 128.0); }

// 1 where the top-down ortho map has something above the camera-relative point p, 0 in the open or outside the map's square.
float CoveredAt(vec3 p) {
    vec4 m = cb.shadowOrtho * vec4(p, 1.0);
    m.xyz /= m.w;
    if (abs(m.x) > 1.0 || abs(m.y) > 1.0 || m.z < 0.0 || m.z > 1.0) return 0.0;
    float top = textureLod(sampler2D(t_ortho, POINT_S), vec2(m.x * 0.5 + 0.5, m.y * -0.5 + 0.5), 0.0).r;
    vec3 depthAxis = vec3(cb.shadowOrtho[0][2], cb.shadowOrtho[1][2], cb.shadowOrtho[2][2]);
    float perUnit = length(depthAxis) / max(abs(cb.shadowOrtho[3][3]), 1.0e-6);
    return top < m.z - max(RoofMargin * perUnit, 0.0001) ? 1.0 : 0.0;
}

// UNOFFICIAL 2026-10-30 (RT hit shading, HitAmbientGrid): the light arriving at camera-relative point p on a surface facing worldN, from
// the grid's cell there (one trilinear read); a: how far inside the grid p is (0 outside).
vec4 GridIrradianceAt(vec3 p, vec3 worldN) {
    float size = cb.grid.w;
    vec3 dims = vec3(GridX, GridY, GridZ);
    vec3 g = (p - cb.grid.xyz) / size - 0.5;
    vec3 edge = min(g + 0.5, dims - 0.5 - g);
    float fade = saturate(min(min(edge.x, edge.y), edge.z) / 2.0);
    if (fade <= 0.0) return vec4(0.0);
    g = clamp(g, vec3(0.0), dims - 1.0);
    float z0 = floor(g.z), z1 = min(z0 + 1.0, GridZ - 1.0);
    vec4 sh = mix(textureLod(sampler2D(t_gridLight, LINEAR_S), GridCellUV(vec3(g.xy, z0)), 0.0),
        textureLod(sampler2D(t_gridLight, LINEAR_S), GridCellUV(vec3(g.xy, z1)), 0.0), g.z - z0);
    float light = max(dot(sh, GridShBasis(-worldN)), 0.0);
    vec3 hue = vec3(1.0);
    if (cb.grid2.z > 0.5) {
        vec4 colour = textureLod(sampler2D(t_gridColor, LINEAR_S), GridCellUV(vec3(g.xy, floor(g.z + 0.5))), 0.0);
        if (colour.a > 1.0e-5) { hue = max(colour.rgb / colour.a, 0.0); hue = min(hue / max(luma(hue), 1.0e-4), 3.0); }
    }
    return vec4((light * GridIrradianceScale + cb.grid3.rgb) * hue * cb.grid2.y, fade);   // light arriving (the hit multiplies by its albedo)
}

// The light reaching a surface at camera-relative position start from world direction worldR, out of the grid.
vec4 GridAlong(vec3 start, vec3 worldR) {
    float size = cb.grid.w;
    vec3 dims = vec3(GridX, GridY, GridZ);
    vec4 toward = GridShBasis(-worldR);
    float light = 0.0, weight = 0.0, inside = 0.0, covered = 0.0;
    vec4 colour = vec4(0.0);
    bool open = true;   // nothing met yet along the ray (the ortho map's test)
    for (float k = 0.0; k < 3.5; k += 1.0) {
        float t = size * (k < 0.5 ? 0.75 : exp2(k));   // 0.75, 2, 4, 8 cells out
        vec3 p = start + worldR * t;
        vec3 g = (p - cb.grid.xyz) / size - 0.5;
        vec3 edge = min(g + 0.5, dims - 0.5 - g);
        float fade = saturate(min(min(edge.x, edge.y), edge.z) / 2.0);
        if (open && fade > 0.0) {
            g = clamp(g, vec3(0.0), dims - 1.0);
            float z0 = floor(g.z), z1 = min(z0 + 1.0, GridZ - 1.0);
            vec4 sh = mix(textureLod(sampler2D(t_gridLight, LINEAR_S), GridCellUV(vec3(g.xy, z0)), 0.0),
                textureLod(sampler2D(t_gridLight, LINEAR_S), GridCellUV(vec3(g.xy, z1)), 0.0), g.z - z0);
            light += max(dot(sh, toward), 0.0) * fade;
            // the colour from the nearer layer (one read; HLSL round on ps_3_0 = floor(x + 0.5), g.z >= 0)
            if (cb.grid2.z > 0.5) colour += textureLod(sampler2D(t_gridColor, LINEAR_S), GridCellUV(vec3(g.xy, floor(g.z + 0.5))), 0.0) * fade;
            weight += fade;
            inside = max(inside, fade);
            if (RoofMaskOn > 0.5) {
                float c = CoveredAt(p);
                covered = max(covered, c * fade);
                open = c < 0.5;
            }
        }
    }
    if (weight <= 0.0) return vec4(0.0);
    vec3 hue = vec3(1.0);
    if (colour.a > 1.0e-5) {
        hue = max(colour.rgb / colour.a, 0.0);
        hue = min(hue / max(luma(hue), 1.0e-4), 3.0);
    }
    vec3 radiance = (light / weight * GridIrradianceScale + cb.grid3.rgb * GridAlbedo) * hue * cb.grid2.y;
    float share = inside;
    if (SkyFallback > 0.0) share *= max(covered, 1.0 - smoothstep(0.0, 0.15, worldR.z));
    return vec4(radiance, share);
}
