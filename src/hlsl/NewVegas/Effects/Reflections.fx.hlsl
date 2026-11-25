// UNOFFICIAL: screen-space reflections on the ground (src/effects/Reflections.cpp, [Shaders.Reflections]). Written for
// this build after the general technique (follow the reflected view ray through the depth buffer); the march and the
// fallbacks follow the parked Water SSR (work\parked\water-ssr).
//
// Which pixels reflect: there is no material buffer, so the ground decides by shape. Surfaces that face up (floors,
// roads, pavements) reflect by the Fresnel term of a dielectric (little looking down, a lot at a grazing angle -- the
// look of a wet street at night that mirrors the neon), times:
//   outdoors  GroundSheen (dry ground, rough) + the rain's wetness (RainWetness x how wet it is now, smooth; only where the
//             sky is open above, from the top-down ortho map as Wet World's roof mask: porches, awnings and marquees stay dry)
//   indoors   InteriorFloors (rough)
// Water, the sky, the first-person weapon and anything past DrawDistance never reflect (water has its own reflection).
//
// Passes (techniques), all but the last at half resolution, driven by ReflectionsEffect::RenderBuffers:
//   5 Capture  (CubeMap on; 6 faces of a small cube round the camera, before the trace) the surroundings cube: every
//              direction that is on screen now takes the image's colour there, blended into what the cube held; what
//              is off screen keeps its last colour and slowly fades. Premultiplied: rgb = colour x coverage, a = coverage.
//              Into one of two capture cubes, the other holding last frame's.
//   6 Filter   (CubeMap on; each face of each mip level) the capture cube blurred into the reflection cube's mips, one
//              level per roughness (level 0 sharp, the last the average of a whole face).
//   1 Trace    the reflected ray from each block's surface, its direction spread by the surface's roughness with a
//              noise that changes every frame; up to Steps steps that grow along the ray, then a binary refinement.
//              Where it lands: the image's colour there (linear). Nothing found: the surroundings cube along the ray
//              (CubeMap) and AmbientLight's light grid (GridFallback; rough surfaces ask the grid first, smooth ones the
//              cube), then the sky outdoors (SkyFallback), else nothing. Output premultiplied: rgb = colour x
//              confidence, a = confidence. -> Temp, or -> Trace when Temporal is on.
//   3 Temporal Trace + last frame's result (History) at the same surface point, held to the range of this frame's
//              neighbours (no trails) -> Temp; the effect then copies Temp to History.
//   2 Blur X, 4 Blur Y  depth-aware Gaussian, wider on rough surfaces than on wet ones: Temp -> Trace -> Buffer.
//   0 Composite (full resolution, in the chain): depth-aware upsample, then the reflection replaces the share of the
//              surface's own colour the Fresnel term gives it.

float4 TESR_ReciprocalResolution;
float4 TESR_SkyColor;
float4 TESR_SkyLowColor;
float4 TESR_HorizonColor;
float4 TESR_SunPosition;
float4 TESR_SkyData;
float4 TESR_SunAmount;
float4 TESR_SunDiskColor;
float4 TESR_SunsetColor;
float4 TESR_SkyIrradianceObjects[9];
float4 TESR_WaterSettings;           // x: height of the water in view
float4 TESR_ReflectionsData;         // x: dry reflectivity of floors now, y: wetness now (0-1), z: dry roughness, w: wet roughness
float4 TESR_ReflectionsData2;        // x: ray length (game units), y: steps, z: thickness scale, w: noise frame offset
float4 TESR_ReflectionsData3;        // x: history weight (0 = none this frame), y: blur radius, z: debug view, w: 1 = sky fallback
float4 TESR_ReflectionsData4;        // x: draw distance, y: 1 = rain wets only open ground (the ortho map is kept up), z: ReflectAdd (REFL_ADD_ON), w: SpecularOcclusion (REFL_SPECOCC_ON)
float4x4 TESR_ShadowCameraToLightTransformOrtho;   // camera-relative world position -> the top-down ortho map (as Wet World)
float4 TESR_ReflectionsLayout;       // xy: one half-resolution texel in screen UV, z: SpecularOcclusionLevel (REFL_SPECOCC_ON)
float4x4 TESR_ReflectionsReproject;  // camera-relative world position -> last frame's clip space
float4 TESR_ObjectMotionData;        // UNOFFICIAL 2026-10-29 ObjectMotion: x 1 = Temporal finds its buffer on stage 11 (Reflections.cpp)
float4 TESR_ReflectionsCube;         // x: face being drawn (0-5, D3D order), y: its size in texels, z: mip level, w: 1 = empty the cube
float4 TESR_ReflectionsCube2;        // x: capture blend, y: fade per frame, z: last mip level, w: 1 = the cube is in use
float4 TESR_ReflectionsGrid;         // UNOFFICIAL 2026-10-19 GridFallback: xyz the light grid's corner relative to the camera, w: cell size
float4 TESR_ReflectionsGrid2;        // x: 1 = the grid is in use, y: brightness (GridBrightness), z: 1 = the grid carries colours
float4 TESR_ReflectionsGrid3;        // rgb: the light every surface gets here (linear: the sky's or the cell's ambient, AmbientLight's)
float4 TESR_ReflectionsWetFx;        // 2026-11-19 WetnessEffects consolidation: x 1 = active (+2 = SkylightingMode 1), y rainWetness, z puddleWetness, w MinRainWetness
float4 TESR_ReflectionsWetFx2;       // x MaxPuddleWetness, y PuddleRoughness, z AlbedoBuffer bound on stage 14 for CompositePS, w PuddlesOn
float4 TESR_PuddleData;              // x water level, y Coverage, z 1 / Size, w Quality
float4 TESR_PuddleShape;             // x slope start, y slope full, z cavity, w wet ring
float4 TESR_PuddleLook;              // x roughness, y darkening, z edge softness, w ripples

// Declared in register order: the effect binds TESR_ samplers by declaration order (a mismatch silently swaps them).
sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_DepthBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_NormalsBuffer : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_DepthBufferViewModel : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_ReflectionsTrace : register(s4) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_ReflectionsBuffer : register(s5) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_ReflectionsTemp : register(s6) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_ReflectionsHistory : register(s7) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
samplerCUBE TESR_ReflectionsCapture : register(s8) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
samplerCUBE TESR_ReflectionsCaptureNew : register(s9) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
samplerCUBE TESR_ReflectionsEnv : register(s10) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_OrthoMapBuffer : register(s11) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
// UNOFFICIAL 2026-10-19 GridFallback: AmbientLight's finished light grid (its NVR_GridAccum / NVR_GridAccumColor, the atlas of
// 4 x 4 tiles of 32 x 32 cells), bilinear within a layer as AmbientLight reads it.
sampler2D TESR_ReflectionsGridLight : register(s12) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_ReflectionsGridColor : register(s13) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
// UNOFFICIAL 2026-10-19 Skylighting v3 phase 1 ([Shaders.AmbientOcclusion.Skylighting] Reflections): the sky visibility (Skylighting
// v2's b), bound by hand for the trace (ReflectionsEffect::RenderBuffers) while NVR_ReflectionsSky.x is 1, so the sky fallback fades
// where the sky cannot be seen, down to y (MinReflection), as Community Shaders' Skylighting MixSpecular (GPL-3.0). After the TESR_ samplers:
// stage 14 (2026-10-19 merges: TESR_OrthoMapBuffer binds to stage 11, the GridFallback samplers to 12 and 13).
float4 NVR_ReflectionsSky;
sampler2D NVR_ReflectionsSkyVis : register(s14) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
// UNOFFICIAL 2026-10-20 MaterialAware ([Shaders.Reflections.Main] MaterialAware, core/ReflMaterial.h): the material buffer, copied out of
// the scene's alpha before any effect draws (technique 7): r roughness, g metal, b 1 where an object with an _rmaos map is seen (0:
// no material known: the floor guess as before). Bound by hand on stage 15 (ReflectionsEffect::BindMaterial). Compiled in only
// when MaterialAware is on at startup (REFL_MATERIAL_ON); otherwise this effect is exactly what it was.
#ifndef REFL_MATERIAL_ON
    #define REFL_MATERIAL_ON 0
#endif
// UNOFFICIAL 2026-11-20 UnifiedReflections ([Main.Main.Quality], test only; docs/DESIGN.md "Unified reflections design" step 1; core/FdIndirect.h):
// with the renderer in mode 2 the surfaces' material is the G-buffer's own record, read per pixel (GBufferMaterial below; the Vulkan passes'
// Reflections.glsl has the same) instead of MaterialAware's copy, which is not made then. Compiled in only when the switch is on at startup
// (REFL_GBUFFER_ON), so that otherwise this effect is exactly what it was. While TESR_ReflectionsGBuffer.x is 1 (ReflectionsEffect::
// BindMaterial): stage 15 holds COLOR2 in place of the copy, stage 8 COLOR3 and stage 9 COLOR1, the capture cubes' stages, which only the
// Capture and Filter techniques read (drawn before; their shaders never touch the two names below).
#ifndef REFL_GBUFFER_ON
    #define REFL_GBUFFER_ON 0
#endif
#if REFL_MATERIAL_ON || REFL_GBUFFER_ON
float4 TESR_ReflectionsMaterial;     // x: 1 = the buffer is read (filled this frame), y: MaxRoughness, z: MaterialReflect
sampler2D NVR_ReflectionsMaterial : register(s15) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
static float SurfaceMetal = 0.0;     // set by Reflectivity: the surface's metal (0 without material data)
#endif
#if REFL_MATERIAL_ON
float4 MaterialAt(float2 uv) { return TESR_ReflectionsMaterial.x > 0.5 ? tex2Dlod(NVR_ReflectionsMaterial, float4(uv, 0, 0)) : 0.0; }
#endif
#if REFL_GBUFFER_ON
float4 TESR_ReflectionsGBuffer;      // x: 1 = the G-buffer's material this frame, y z: COLOR3's distance match (share, units: TESR_CapturedNormalsData y z)
sampler2D NVR_ReflectionsGNormals : register(s8) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D NVR_ReflectionsGAlbedo : register(s9) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
static bool SurfaceGBuffer = false;  // set by Reflectivity: the material is the G-buffer's (the composite takes a metal's colour from COLOR1)
// UNOFFICIAL 2026-11-20 unified reflections step 2: DeferredWetness's film (core/DeferredWetness.h FilmTexture: r round(8 murk) + 0.99 gloss,
// g roughness), on the history's stage 7 (only the Temporal technique reads that one; ReflectionsEffect::RenderBuffers takes it off for that
// pass). x 1 = bound this frame (a whole film, with TESR_ReflectionsGBuffer.x), y the wet strength (RainWetness outdoors, 0 indoors), w 1 /
// MaxRainWetness. As the Vulkan passes' cb.film.
float4 TESR_ReflectionsFilm;
sampler2D NVR_ReflectionsFilm : register(s7) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
#endif
texture NoTexture;   // never set: unbinds a technique's own render target from its sampler

static const float DryReflect = TESR_ReflectionsData.x;
static const float Wetness = TESR_ReflectionsData.y;
static const float DryRoughness = TESR_ReflectionsData.z;
static const float WetRoughness = TESR_ReflectionsData.w;
static const float RayLength = TESR_ReflectionsData2.x;
static const float Steps = TESR_ReflectionsData2.y;
static const float ThicknessScale = TESR_ReflectionsData2.z;
static const float NoiseFrame = TESR_ReflectionsData2.w;
static const float HistoryWeight = TESR_ReflectionsData3.x;
static const float BlurRadius = TESR_ReflectionsData3.y;
static const float DebugView = TESR_ReflectionsData3.z;
static const float SkyFallback = TESR_ReflectionsData3.w;
static const float DrawDistance = TESR_ReflectionsData4.x;
static const float RoofMaskOn = TESR_ReflectionsData4.y;
static const float2 LowTexel = TESR_ReflectionsLayout.xy;
static const float CubeFace = TESR_ReflectionsCube.x;
static const float CubeSize = TESR_ReflectionsCube.y;
static const float CubeMip = TESR_ReflectionsCube.z;
static const float CubeReset = TESR_ReflectionsCube.w;
static const float CaptureBlend = TESR_ReflectionsCube2.x;
static const float CubeFade = TESR_ReflectionsCube2.y;
static const float CubeLastMip = TESR_ReflectionsCube2.z;
static const float CubeOn = TESR_ReflectionsCube2.w;
static const float GridOn = TESR_ReflectionsGrid2.x;
static const int RefineSteps = 5;
static const float BrightestReflection = 16.0;   // linear: a lamp's core reflected in one texel cannot flash the whole puddle

struct VSOUT { float4 vertPos : POSITION; float2 UVCoord : TEXCOORD0; };
struct VSIN { float4 vertPos : POSITION0; float2 UVCoord : TEXCOORD0; };
VSOUT FrameVS(VSIN IN) { VSOUT OUT = (VSOUT)0.0f; OUT.vertPos = IN.vertPos; OUT.UVCoord = IN.UVCoord; return OUT; }

#include "Includes/Helpers.hlsl"
#include "Includes/Depth.hlsl"
#include "Includes/FrameNoise.hlsl"	// UNOFFICIAL 2026-11-01 the shared noise code (the same pattern as before)

// Interleaved gradient noise (Jimenez 2014) in [0, 1), moved by NoiseFrame (core/TemporalBackbone.h with SharedNoise: the shared
// index with Temporal, else moving only under TAA / FSR / DLSS).
float GradientNoise(float2 pixel) {
	return FrameNoiseIGN(pixel, NoiseFrame);
}

// View-space position of a screen point at a linear depth (NVR's projection has no off-centre terms).
// (UNOFFICIAL 2026-10-27 RenderScale v2: uv is a texture coordinate; ToScreen returns one too. Edge tests compare rectToScreen of it.)
float3 ViewPosition(float2 uv, float depth) {
	uv = rectToScreen(uv);
	return float3((uv.x * 2.0 - 1.0) / TESR_ProjectionTransform[0][0], (1.0 - uv.y * 2.0) / TESR_ProjectionTransform[1][1], 1.0) * depth;
}

float2 ToScreen(float3 view) {
	return screenToRect(float2(view.x * TESR_ProjectionTransform[0][0] / view.z * 0.5 + 0.5, 0.5 - view.y * TESR_ProjectionTransform[1][1] / view.z * 0.5));
}

bool IsWeapon(float2 uv) {
	// The weapon's depth buffer is empty where there is no weapon: 1, or 0 with reversed depth (matching CombineDepth > 0.0).
	float viewmodelDepth = tex2Dlod(TESR_DepthBufferViewModel, float4(uv, 0, 0)).x;
	return TESR_DepthConstants.z == 0.0 ? viewmodelDepth < 0.99999 : viewmodelDepth > 0.0;
}

#if !REFL_GBUFFER_ON
float3 ViewNormal(float2 uv) { return normalize(tex2Dlod(TESR_NormalsBuffer, float4(uv, 0, 0)).xyz * 2.0 - 1.0); }
#endif

// View space -> world space direction (the view transform's rows are the camera axes in world space, see toWorld).
float3 ViewToWorldDir(float3 v) { return mul((float3x3)TESR_ViewTransform, v); }

// UNOFFICIAL 2026-10-19 (P81 tester reports: the ground under the Vault 21 marquee wet and dark, mirroring its underside):
// 1 where nothing is above the surface in the top-down ortho map (the test of Wet World's GetRoofMask), 0 under a roof.
// Outside the map's square (it covers only the near area) the ground counts as open. A surface up to RoofMargin world units
// below the highest thing in its map texel still counts as open: the map's texels are several units wide, so a roof's own
// slope (or the top of a sign) would otherwise read as covered by itself (Wet World's 0.0001 depth margin is far smaller).
static const float RoofMargin = 40.0;   // world units; real cover (a porch roof, a marquee) is 100+ units above the floor
float OpenAbove(float2 uv, float depth) {
	float4 p = mul(float4(toWorld(uv) * depth, 1.0), TESR_ShadowCameraToLightTransformOrtho);
	p.xyz /= p.w;
	[branch] if (abs(p.x) > 1.0 || abs(p.y) > 1.0 || p.z < 0.0 || p.z > 1.0) return 1.0;
	float top = tex2Dlod(TESR_OrthoMapBuffer, float4(p.x * 0.5 + 0.5, p.y * -0.5 + 0.5, 0.0, 0.0)).r;
	// the map's depth per world unit along its view axis (Wet World's GetOrthoDepthScale)
	float3 depthAxis = float3(TESR_ShadowCameraToLightTransformOrtho[0][2], TESR_ShadowCameraToLightTransformOrtho[1][2], TESR_ShadowCameraToLightTransformOrtho[2][2]);
	float perUnit = length(depthAxis) / max(abs(TESR_ShadowCameraToLightTransformOrtho[3][3]), 1.0e-6);
	return top > p.z - max(RoofMargin * perUnit, 0.0001) ? 1.0 : 0.0;
}

static float SurfaceWetGloss = 0.0;
static float SurfaceWetRough = 1.0;

float3 WetHash33Fx(float3 p3) {
	p3 = frac(p3 * float3(0.1031, 0.1030, 0.0973));
	p3 += dot(p3, p3.yxz + 33.33);
	return frac((p3.xxy + p3.yxx) * p3.zyx);
}
float PuddleHashFx(float2 p) { return WetHash33Fx(float3(p, 7.13)).x; }
float PuddleValueNoiseFx(float2 p) {
	float2 i = floor(p);
	float2 f = p - i;
	float2 u = f * f * (3.0 - 2.0 * f);
	return lerp(lerp(PuddleHashFx(i), PuddleHashFx(i + float2(1.0, 0.0)), u.x),
		lerp(PuddleHashFx(i + float2(0.0, 1.0)), PuddleHashFx(i + float2(1.0, 1.0)), u.x), u.y);
}
float PuddleStandingFx(float2 uv, float depth, float worldUp) {
	[branch] if (TESR_ReflectionsWetFx2.w < 0.5 || TESR_PuddleData.x <= 0.001 || depth > 10240.0) return 0.0;
	float3 worldPos = TESR_CameraPosition.xyz + toWorld(uv) * depth;
	float2 p = worldPos.xy * TESR_PuddleData.z;
	float n = PuddleValueNoiseFx(p) * 0.62 + PuddleValueNoiseFx(p * 2.31 + 17.7) * 0.38;
	if (TESR_PuddleData.w > 2.5) n = n * 0.85 + PuddleValueNoiseFx(p * 5.7 + 41.3) * 0.15;
	n = smoothstep(0.2, 0.8, n);
	float t = 1.0 - TESR_PuddleData.x * TESR_PuddleData.y;
	float shore = saturate((n - t) / max(TESR_PuddleLook.z, 1.0e-3));
	float nearFactor = 1.0 - smoothstep(0.0, 10240.0, depth);
	return shore * shore * (3.0 - 2.0 * shore) * smoothstep(TESR_PuddleShape.x, TESR_PuddleShape.y, worldUp) * nearFactor;
}
void ComputeWetFxFilmFx(float2 uv, float depth, float3 worldNormal) {
	SurfaceWetGloss = 0.0;
	SurfaceWetRough = 1.0;
	[branch] if (TESR_ReflectionsWetFx.x < 0.5 || depth >= DrawDistance || depth >= farZ * 0.98 || depth > 10240.0 || IsWeapon(uv)) return;
	float open = RoofMaskOn > 0.5 ? OpenAbove(uv, depth) : 1.0;
	[branch] if (open <= 0.0) return;
	float nearFactor = 1.0 - smoothstep(0.0, 10240.0, depth);
	float minAngle = saturate(max(TESR_ReflectionsWetFx.w, worldNormal.z));
	float rain = saturate(TESR_ReflectionsWetFx.y * minAngle) * open * nearFactor;
	float standing = PuddleStandingFx(uv, depth, worldNormal.z) * open;
	if (TESR_ReflectionsWetFx2.w > 0.5) {
		SurfaceWetGloss = saturate(max(rain, standing));
		SurfaceWetRough = lerp(max(lerp(1.0, 0.22, rain), 0.05), max(TESR_ReflectionsWetFx2.y, 0.01), standing);
	} else {
		float puddle = max(rain, saturate(TESR_ReflectionsWetFx.z * minAngle * TESR_ReflectionsWetFx2.x * 0.5) * open * nearFactor);
		SurfaceWetGloss = saturate(puddle);
		SurfaceWetRough = max(1.0 - SurfaceWetGloss, 0.05);
	}
}

#if REFL_GBUFFER_ON
// UNOFFICIAL 2026-11-20 UnifiedReflections: the G-buffer's record decoded into the copy's layout (r roughness, g metal, b 1 = reflects by its
// smoothness whatever way it faces: COLOR2's specular bit on models 1-3; b 0.5 = a known material for the floor rule; plants, skin, grass,
// hair and eyes rough; 0 = none). None also where the record is not the surface on screen: COLOR1 unwritten, or COLOR3's camera distance not
// the depth's (the game's own shaders, water, glass, grass, the first person drawn over it). As Compute/Reflections.glsl GBufferMaterial.
bool GBufferRecordAt(float2 uv) {
	[branch] if (tex2Dlod(NVR_ReflectionsGAlbedo, float4(uv, 0, 0)).a < 0.5) return false;
	float4 c = tex2Dlod(NVR_ReflectionsGNormals, float4(uv, 0, 0));
	float z = readDepthLod(uv);
	[branch] if (c.a <= 0.99 || z >= farZ * 0.99) return false;
	float d = length(ViewPosition(uv, z));
	return abs(c.b - d) <= d * TESR_ReflectionsGBuffer.y + TESR_ReflectionsGBuffer.z;
}
float4 GBufferMaterial(float2 uv) {
	float4 m = tex2Dlod(NVR_ReflectionsMaterial, float4(uv, 0, 0));
	[branch] if (m.r <= 0.5 || !GBufferRecordAt(uv)) return 0.0;
	float c = floor(m.r + 0.01);
	float spec = c >= 15.5 ? 1.0 : 0.0;
	float model = c - 16.0 * spec;
	[branch] if (model > 4.5 && model < 9.5) return float4(1.0, 0.0, 0.5, 0.0);
	float b = m.b < -1.99 && m.b > -3.01 ? -2.0 - m.b : m.b;   // (the path tracer's matte code back to the roughness)
	[branch] if (b < 0.0) return 0.0;                         // no roughness recorded
	return float4(saturate(b), saturate(frac(m.r) / 0.49), spec > 0.5 && model < 3.5 ? 1.0 : 0.5, 0.0);
}
// The surface's material: the G-buffer's while TESR_ReflectionsGBuffer.x is 1, else MaterialAware's copy (or none).
float4 SurfaceMaterialAt(float2 uv) {
	[branch] if (TESR_ReflectionsGBuffer.x > 0.5) { float4 m = GBufferMaterial(uv); SurfaceGBuffer = m.b > 0.25; return m; }
#if REFL_MATERIAL_ON
	return MaterialAt(uv);
#else
	return 0.0;
#endif
}

// UNOFFICIAL 2026-11-20 unified reflections step 2: the Vulkan passes' normal rules (Compute/Reflections.glsl SmoothViewNormal, BumpKeep,
// ViewNormal, NormalVarianceRoughness, FilmWetness: keep the numbers in step), so both back ends reflect the same. While
// TESR_ReflectionsGBuffer.x is 1, stage 2 holds the normal-mapped shading normals (FdIndirect ShadingNormals); the smooth normal under them is
// COLOR3's (world, octahedral), where the record is the surface on screen; anywhere else stage 2 holds the smooth one itself.
float3 GBDecodeOct(float2 e) {
	float3 n = float3(e, 1.0 - abs(e.x) - abs(e.y));
	float t = saturate(-n.z);
	n.xy += float2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);
	return normalize(n);
}
float3 SmoothViewNormalAt(float2 uv, float3 n) {
	[branch] if (TESR_ReflectionsGBuffer.x > 0.5 && GBufferRecordAt(uv))
		return normalize(mul(float4(GBDecodeOct(tex2Dlod(NVR_ReflectionsGNormals, float4(uv, 0, 0)).rg), 0.0), TESR_ViewTransform).xyz);
	return n;
}
// The film's water at a pixel (0-1, x the strength) and its roughness (at most WetRoughness); 0 where no film was worked out.
float FilmWetness(float2 uv, out float roughness) {
	float4 f = tex2Dlod(NVR_ReflectionsFilm, float4(uv, 0, 0));
	roughness = min(max(f.g, 0.01), WetRoughness);
	return f.g > 0.001 ? saturate(saturate(frac(f.r) / 0.99) * TESR_ReflectionsFilm.w) * TESR_ReflectionsFilm.y : 0.0;
}
// How much of the bump the reflection follows: whole up to 0.5 world units per pixel, none from 2.0; damp ground keeps 1 - DampBumpLean of it
// at full wet (Reflections.glsl: 0 since 2026-11-20, the owner's "keep 100% of its bump").
float BumpKeep(float2 uv) {
	float footprint = readDepthLod(uv) * 2.0 * rectToScreen(TESR_ReciprocalResolution.xy).x / TESR_ProjectionTransform[0][0];
	float keep = 1.0 - smoothstep(0.5, 2.0, footprint);
	[branch] if (TESR_ReflectionsFilm.x > 0.5 && keep > 0.0) {
		float4 f = tex2Dlod(NVR_ReflectionsFilm, float4(uv, 0, 0));
		float wet = f.g > 0.001 ? saturate(saturate(frac(f.r) / 0.99) * TESR_ReflectionsFilm.w) : 0.0;
		keep *= 1.0 - 0.0 * wet;
	}
	return keep;
}
float3 ViewNormal(float2 uv) {
	float3 n = normalize(tex2Dlod(TESR_NormalsBuffer, float4(uv, 0, 0)).xyz * 2.0 - 1.0);
	[branch] if (TESR_ReflectionsGBuffer.x > 0.5) {
		float keep = BumpKeep(uv);
		[branch] if (keep < 1.0) n = normalize(lerp(SmoothViewNormalAt(uv, n), n, keep));
	}
	return n;
}
// Specular AA on the geometry's curvature (the smooth normal's change to the right and lower neighbour), at most 0.35^4 added to alpha^2.
float CurvatureRoughness(float2 uv, float depth, float roughness) {
	float3 n = SmoothViewNormalAt(uv, normalize(tex2Dlod(TESR_NormalsBuffer, float4(uv, 0, 0)).xyz * 2.0 - 1.0));
	float sum = 0.0;
	[unroll] for (int i = 0; i < 2; i++) {
		float2 q = uv + (i == 0 ? float2(TESR_ReciprocalResolution.x, 0.0) : float2(0.0, TESR_ReciprocalResolution.y));
		[branch] if (abs(readDepthLod(q) - depth) <= depth * 0.02 + 1.0) {
			float3 d = SmoothViewNormalAt(q, normalize(tex2Dlod(TESR_NormalsBuffer, float4(q, 0, 0)).xyz * 2.0 - 1.0)) - n;
			sum += dot(d, d);
		}
	}
	float a = roughness * roughness;
	return sqrt(sqrt(saturate(a * a + min(2.0 * 0.15915494 * sum, 0.015))));
}
#endif

#if REFL_MATERIAL_ON || REFL_GBUFFER_ON
// UNOFFICIAL 2026-10-20 MaterialAware: a surface whose material is known (its _rmaos map). Whatever way it faces, it reflects by how
// smooth it is: MaterialReflect where its roughness is well under MaxRoughness, fading to nothing at MaxRoughness (rough concrete,
// dirt and carpet stop sheening; tiles, glass, polished counters and car paint get it, walls too). The rain still wets floors that
// face up and are open to the sky, whatever they are made of (a wet surface is smooth). Its metal tints the reflection (Composite).
float MaterialReflectivity(float2 uv, float depth, float floorShare, float4 material, out float roughness) {
	float worldZ = TESR_CameraPosition.z + toWorld(uv).z * depth;
	roughness = max(material.r, 0.02);
	[branch] if (floorShare > 0.0 && abs(worldZ - TESR_WaterSettings.x) < max(2.0, depth * 0.002)) return 0.0;   // water reflects on its own
	float maxRough = TESR_ReflectionsMaterial.y;
	float dry = TESR_ReflectionsMaterial.z * (1.0 - smoothstep(0.6 * maxRough, maxRough, material.r));
	float wetness = Wetness * floorShare;
#if REFL_GBUFFER_ON
	float wetRoughness = WetRoughness;   // (2026-11-20 step 2: the film's water, as the Vulkan passes)
	[branch] if (TESR_ReflectionsFilm.x > 0.5) wetness = floorShare > 0.0 ? FilmWetness(uv, wetRoughness) * floorShare : 0.0;
	else if (wetness > 0.0 && RoofMaskOn > 0.5) wetness *= OpenAbove(uv, depth);
	float amount = dry + wetness;
	roughness = lerp(roughness, wetRoughness, wetness / max(amount, 1.0e-4));
#else
	// (2026-11-22 upstream 1.1.3 merge: upstream's forward film, ComputeWetFxFilmFx; this variant is the one without UnifiedReflections)
	float standing = floorShare > 0.0 ? PuddleStandingFx(uv, depth, ViewToWorldDir(ViewNormal(uv)).z) : 0.0;
	[branch] if ((wetness > 0.0 || standing > 0.0) && RoofMaskOn > 0.5) { float open = OpenAbove(uv, depth); wetness *= open; standing *= open; }
	wetness = max(max(wetness, standing), SurfaceWetGloss);
	float amount = dry + wetness;
	float wetTargetRough = TESR_ReflectionsWetFx.x > 0.5 && SurfaceWetGloss > 0.0 ? SurfaceWetRough : WetRoughness;
	roughness = lerp(roughness, wetTargetRough, wetness / max(amount, 1.0e-4));
	roughness = lerp(roughness, max(TESR_ReflectionsWetFx2.y, 0.01), standing);
#endif
	SurfaceMetal = material.g;
	float fade = 1.0 - smoothstep(0.7 * DrawDistance, DrawDistance, depth);
	return min(amount, 2.0) * fade;
}
#endif

// How much a surface reflects (before Fresnel) and how rough it is. 0 where it never reflects.
#if REFL_GBUFFER_ON
    #define Reflectivity ReflectivityOwn   // (2026-11-20 step 2: wrapped below with the specular AA)
#endif
float Reflectivity(float2 uv, float depth, float3 viewNormal, out float roughness) {
	roughness = DryRoughness;
	SurfaceWetGloss = 0.0;
	SurfaceWetRough = 1.0;
#if REFL_MATERIAL_ON || REFL_GBUFFER_ON
	SurfaceMetal = 0.0;
#endif
#if REFL_GBUFFER_ON
	SurfaceGBuffer = false;
#endif
	[branch] if (depth >= DrawDistance || depth >= farZ * 0.98 || IsWeapon(uv)) return 0.0;
	float3 worldNormal = ViewToWorldDir(viewNormal);
	ComputeWetFxFilmFx(uv, depth, worldNormal);
	float floorShare = saturate((worldNormal.z - 0.75) / 0.2);
#if REFL_MATERIAL_ON || REFL_GBUFFER_ON
#if REFL_GBUFFER_ON
	float4 material = SurfaceMaterialAt(uv);
#else
	float4 material = MaterialAt(uv);
#endif
	[branch] if (material.b > 0.75) return MaterialReflectivity(uv, depth, floorShare, material, roughness);
	float dryRoughness = material.b > 0.25 ? max(material.r, 0.02) : DryRoughness;
	SurfaceMetal = material.b > 0.25 ? material.g : 0.0;
#else
	float dryRoughness = DryRoughness;
#endif
	float fade = 1.0 - smoothstep(0.7 * DrawDistance, DrawDistance, depth);
	[branch] if (floorShare <= 0.0) {
		[branch] if (SurfaceWetGloss > 0.01) {
			roughness = SurfaceWetRough;
			return SurfaceWetGloss * (1.0 - smoothstep(0.35, 0.75, SurfaceWetRough)) * fade;
		}
		return 0.0;
	}
	float worldZ = TESR_CameraPosition.z + toWorld(uv).z * depth;
	[branch] if (abs(worldZ - TESR_WaterSettings.x) < max(2.0, depth * 0.002)) return 0.0;   // water reflects on its own
	float wetness = Wetness;
#if REFL_GBUFFER_ON
	float wetRoughness = WetRoughness;   // (2026-11-20 step 2: the film's water, as the Vulkan passes)
	[branch] if (TESR_ReflectionsFilm.x > 0.5) wetness = FilmWetness(uv, wetRoughness);
	else if (wetness > 0.0 && RoofMaskOn > 0.5) wetness *= OpenAbove(uv, depth);
	float amount = DryReflect + wetness;
	roughness = lerp(dryRoughness, wetRoughness, wetness / max(amount, 1.0e-4));
#else
	// (2026-11-22 upstream 1.1.3 merge: upstream's forward film, ComputeWetFxFilmFx; this variant is the one without UnifiedReflections)
	float standing = PuddleStandingFx(uv, depth, worldNormal.z);
	[branch] if ((wetness > 0.0 || standing > 0.0) && RoofMaskOn > 0.5) { float open = OpenAbove(uv, depth); wetness *= open; standing *= open; }
	wetness = max(max(wetness, standing), SurfaceWetGloss);
	float amount = DryReflect + wetness;
	float wetTargetRough = TESR_ReflectionsWetFx.x > 0.5 && SurfaceWetGloss > 0.0 ? SurfaceWetRough : WetRoughness;
	roughness = lerp(dryRoughness, wetTargetRough, wetness / max(amount, 1.0e-4));
	roughness = lerp(roughness, max(TESR_ReflectionsWetFx2.y, 0.01), standing);
#endif
	return min(amount, 2.0) * floorShare * fade;   // above 1: stronger than a real surface, for taste
}
#if REFL_GBUFFER_ON
    #undef Reflectivity
float Reflectivity(float2 uv, float depth, float3 viewNormal, out float roughness) {
	float amount = ReflectivityOwn(uv, depth, viewNormal, roughness);
	[branch] if (TESR_ReflectionsGBuffer.x > 0.5 && amount > 0.0) roughness = CurvatureRoughness(uv, depth, roughness);
	return amount;
}
#endif

// Split-sum environment BRDF (Karis's fit) with roughness-damped grazing F90 (eliminates chalky white horizon rims on rough dirt/rock).
float3 EnvironmentFresnelF0(float roughness, float NdotV, float3 F0) {
	const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
	const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
	float4 r = roughness * c0 + c1;
	float a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
	float2 ab = float2(-1.04, 1.04) * a004 + r.zw;
	float f90 = saturate(50.0 * dot(F0, float3(0.3333, 0.3333, 0.3333))) * saturate(1.0 - roughness);
	float3 F90 = max(f90.xxx, F0);
	return F0 * ab.x + F90 * ab.y;
}

float EnvironmentFresnel(float roughness, float NdotV) {
	return EnvironmentFresnelF0(roughness, NdotV, float3(0.04, 0.04, 0.04)).x;
}

#if REFL_MATERIAL_ON || REFL_GBUFFER_ON
// UNOFFICIAL 2026-11-08 (perf-reflfix4) fallback metal F0 when AlbedoBuffer is unavailable.
float3 ReflMetalF0(float3 colour, float roughness) {
	return lerp(float3(0.95, 0.95, 0.95), saturate(colour), smoothstep(0.06, 0.15, roughness));
}
#endif

float WetFilmReflectanceFx(float NdotV, float rough, float gloss) {
	const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
	const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
	float4 r = rough * c0 + c1;
	float a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
	float2 ab = float2(-1.04, 1.04) * a004 + r.zw;
	return saturate((0.02 * ab.x + ab.y) * gloss);
}

float3 WetSkyReflectionLinearFx(float3 r) {
	float3 encoded;
	[branch] if (TESR_ReflectionsWetFx.x > 1.5) {
		float height = pow(saturate(r.z * 0.5 + 0.5), 3.0);
		float sunUp = max(TESR_SunPosition.z, 0.0);
		float haze = (1.0 - height) * (1.0 - height) * TESR_SkyData.x;
		float towardSun = pow(saturate(dot(r, TESR_SunPosition.xyz) * 0.5 + 0.5), 1.0 / max(TESR_SkyData.y, 1.0e-4));
		float3 sunLin = linearize(TESR_SunDiskColor.rgb);
		float3 sunsetLin = linearize(TESR_SunsetColor.rgb);
		float lowSun = saturate(pow(1.0 - sunUp, 8.0)) * TESR_SunAmount.x;
		float3 disc = (1.0 + sunUp) * sunLin + sunsetLin * lowSun * haze;
		float byDay = smoothstep(0.0, 0.5, TESR_SunAmount.x);
		float3 skyRad = lerp(linearize(TESR_SkyLowColor.rgb), linearize(TESR_SkyColor.rgb), height);
		skyRad = lerp(skyRad, linearize(TESR_HorizonColor.rgb), saturate(haze * (0.5 + towardSun * 0.5)));
		skyRad += lerp(0.0.xxx, disc * towardSun * (1.0 - sunUp) * haze * TESR_SkyData.z, byDay);
		float seen = 0.5 * r.z + 0.5;
		encoded = sqrt(max(skyRad, 0.0.xxx)) * seen;
	} else {
		float3 E = TESR_SkyIrradianceObjects[0].rgb
			+ TESR_SkyIrradianceObjects[1].rgb * r.y
			+ TESR_SkyIrradianceObjects[2].rgb * r.z
			+ TESR_SkyIrradianceObjects[3].rgb * r.x
			+ TESR_SkyIrradianceObjects[4].rgb * (r.x * r.y)
			+ TESR_SkyIrradianceObjects[5].rgb * (r.y * r.z)
			+ TESR_SkyIrradianceObjects[6].rgb * (3.0 * r.z * r.z - 1.0)
			+ TESR_SkyIrradianceObjects[7].rgb * (r.x * r.z)
			+ TESR_SkyIrradianceObjects[8].rgb * (r.x * r.x - r.y * r.y);
		encoded = sqrt(max(E, 0.0.xxx));
	}
	return linearize(encoded);
}

// The sky along a world direction: the weather's horizon and zenith colours (as Specular reads it). Below the horizon,
// nothing (the ground there is not on screen, and the sky would light it up).
float3 SkyAlong(float3 worldDir) {
	float up = saturate(worldDir.z);
	return lerp(linearize(TESR_HorizonColor.rgb), linearize(TESR_SkyColor.rgb), sqrt(up)) * smoothstep(0.0, 0.15, worldDir.z);
}

// Karis's reversible compression for averaging HDR samples: c / (1 + luminance), and back.
float3 Tame(float3 c) { return c / (1.0 + dot(c, float3(0.2126, 0.7152, 0.0722))); }
float3 Untame(float3 t) {
	t = max(t, 0.0);
	return t / max(1.0 - dot(t, float3(0.2126, 0.7152, 0.0722)), 1.0 / (1.0 + BrightestReflection));
}

// ---- The surroundings cube ----
// World direction through texel st (-1..1, s right, t down) of a cube face, in Direct3D's face order and layout (the same
// convention texCUBE reads with). The cube's axes are the world's (z up), centred on the camera.
float3 FaceDirection(float face, float2 st) {
	if (face < 0.5) return float3(1.0, -st.y, -st.x);
	if (face < 1.5) return float3(-1.0, -st.y, st.x);
	if (face < 2.5) return float3(st.x, 1.0, st.y);
	if (face < 3.5) return float3(st.x, -1.0, -st.y);
	if (face < 4.5) return float3(st.x, -st.y, 1.0);
	return float3(-st.x, -st.y, -1.0);
}

float3 WorldToViewDir(float3 w) { return mul(float4(w, 0.0), TESR_ViewTransform).xyz; }

// The reflection cube along a world direction, blurred for the roughness: rgb colour, a how much of it was ever seen
// (0: nothing there yet). A coarser level fills the gaps of the sharper one.
float4 EnvAlong(float3 worldDir, float roughness) {
	float lod = roughness * CubeLastMip;
	float4 sharp = texCUBElod(TESR_ReflectionsEnv, float4(worldDir, lod));
	float4 coarse = texCUBElod(TESR_ReflectionsEnv, float4(worldDir, min(lod + 2.0, CubeLastMip)));
	float4 m = sharp + coarse * (1.0 - saturate(sharp.a));
	return float4(m.rgb / max(m.a, 1.0e-4), saturate(m.a));
}

// ---- The light grid (AmbientLight's GridBounce) as the far / off-screen fallback ----
// UNOFFICIAL 2026-10-19 GridFallback (user: could the bounce light's grid stop reflections cutting off at the screen edge?).
// Each grid cell holds the light passing through it as order-1 spherical harmonics (one number for all directions, three for
// the direction; brightness only) and that light's colour (colour x light, light): by day outdoors the sunlit surfaces round
// the player, indoors and at night what the screen showed (AmbientLight.fx.hlsl GridInject / ScreenInject). The light that
// reaches the surface from the reflected direction R travels along -R: it is read at a few points along the ray (so it is
// position-correct, unlike the cube, which is seen from the camera) and averaged. Coarse (a cell is GridCellSize wide, the
// direction a single lobe): fit for blurred reflections, not for a mirror. The grid holds no sky and no ambient light, so the
// light a typical surface shows from the scene's ambient is added (what a shaded wall would show), in the grid's colour.
static const float GridX = 32.0, GridY = 32.0, GridZ = 16.0;
static const float GridAlbedo = 0.4;   // a typical surface (desert ground, plaster) for the ambient share
static const float GridIrradianceScale = 1.5;   // AmbientLight's GridBounce reads the grid x 1.5 (its spreading loses light)
float4 GridShBasis(float3 d) { return float4(0.282095, 0.488603 * d); }
// (2026-10-22 lightgrid2: the atlas is 256 x 128 now, AmbientLight's coarse level in its left half; GridFallback reads that one.)
float2 GridCellUV(float3 c) { return (float2(fmod(c.z, 4.0), floor(c.z / 4.0)) * 32.0 + c.xy + 0.5) / float2(256.0, 128.0); }

// 1 where the top-down ortho map has something above the camera-relative point p (inside or under a building, a rock, a
// roof: a ray that got there met it), 0 in the open or outside the map's square. Only while the map is kept up (RoofMaskOn).
float CoveredAt(float3 p) {
	float4 m = mul(float4(p, 1.0), TESR_ShadowCameraToLightTransformOrtho);
	m.xyz /= m.w;
	[branch] if (abs(m.x) > 1.0 || abs(m.y) > 1.0 || m.z < 0.0 || m.z > 1.0) return 0.0;
	float top = tex2Dlod(TESR_OrthoMapBuffer, float4(m.x * 0.5 + 0.5, m.y * -0.5 + 0.5, 0.0, 0.0)).r;
	float3 depthAxis = float3(TESR_ShadowCameraToLightTransformOrtho[0][2], TESR_ShadowCameraToLightTransformOrtho[1][2], TESR_ShadowCameraToLightTransformOrtho[2][2]);
	float perUnit = length(depthAxis) / max(abs(TESR_ShadowCameraToLightTransformOrtho[3][3]), 1.0e-6);
	return top < m.z - max(RoofMargin * perUnit, 0.0001) ? 1.0 : 0.0;
}

// The light reaching a surface at camera-relative position start from world direction worldR, out of the grid: rgb colour
// (linear), a how much of the missing reflection it answers for (0 outside the grid; outdoors only where the ray meets
// something in the ortho map or runs near the horizon, toward the land and buildings round about: the grid has no sky).
float4 GridAlong(float3 start, float3 worldR) {
	float size = TESR_ReflectionsGrid.w;
	float3 dims = float3(GridX, GridY, GridZ);
	float4 toward = GridShBasis(-worldR);
	float light = 0.0, weight = 0.0, inside = 0.0, covered = 0.0;
	float4 colour = 0.0;
	bool open = true;   // nothing met yet along the ray (the ortho map's test)
	[loop] for (float k = 0.0; k < 3.5; k += 1.0) {
		float t = size * (k < 0.5 ? 0.75 : exp2(k));   // 0.75, 2, 4, 8 cells out
		float3 p = start + worldR * t;
		float3 g = (p - TESR_ReflectionsGrid.xyz) / size - 0.5;
		float3 edge = min(g + 0.5, dims - 0.5 - g);
		float fade = saturate(min(min(edge.x, edge.y), edge.z) / 2.0);
		[branch] if (open && fade > 0.0) {
			g = clamp(g, 0.0, dims - 1.0);
			float z0 = floor(g.z), z1 = min(z0 + 1.0, GridZ - 1.0);
			float4 sh = lerp(tex2Dlod(TESR_ReflectionsGridLight, float4(GridCellUV(float3(g.xy, z0)), 0.0, 0.0)),
				tex2Dlod(TESR_ReflectionsGridLight, float4(GridCellUV(float3(g.xy, z1)), 0.0, 0.0)), g.z - z0);
			light += max(dot(sh, toward), 0.0) * fade;
			// the colour from the nearer layer (one read)
			[branch] if (TESR_ReflectionsGrid2.z > 0.5) colour += tex2Dlod(TESR_ReflectionsGridColor, float4(GridCellUV(float3(g.xy, round(g.z))), 0.0, 0.0)) * fade;
			weight += fade;
			inside = max(inside, fade);
			[branch] if (RoofMaskOn > 0.5) {
				float c = CoveredAt(p);
				covered = max(covered, c * fade);
				open = c < 0.5;
			}
		}
	}
	[branch] if (weight <= 0.0) return 0.0;
	float3 hue = 1.0;
	[branch] if (colour.a > 1.0e-5) {
		hue = max(colour.rgb / colour.a, 0.0);
		hue = min(hue / max(luma(hue), 1.0e-4), 3.0);
	}
	float3 radiance = (light / weight * GridIrradianceScale + TESR_ReflectionsGrid3.rgb * GridAlbedo) * hue * TESR_ReflectionsGrid2.y;
	float share = inside;
	[branch] if (SkyFallback > 0.0) share *= max(covered, 1.0 - smoothstep(0.0, 0.15, worldR.z));
	return float4(radiance, share);
}

// ---- 5: Capture, one cube face per draw ----
float4 CapturePS(VSOUT IN, float2 vpos : VPOS) : COLOR0 {
	float2 st = (vpos + 0.5) / CubeSize * 2.0 - 1.0;
	float3 dir = FaceDirection(CubeFace, st);
	float4 result = CubeReset > 0.0 ? 0.0 : texCUBElod(TESR_ReflectionsCapture, float4(dir, 0.0)) * (1.0 - CubeFade);

	float3 view = WorldToViewDir(dir);
	[branch] if (view.z > 0.05) {
		float2 uv = ToScreen(view);
		[branch] if (all(rectToScreen(uv) > 0.005) && all(rectToScreen(uv) < 0.995)) {
			// A cube texel covers a patch of many screen pixels (about 1.4 degrees): average a 4 x 4 grid of reads across
			// it, so a thin neon tube counts by how much of the patch it fills instead of flickering in and out.
			float2 du = ToScreen(WorldToViewDir(FaceDirection(CubeFace, st + float2(2.0 / CubeSize, 0.0)))) - uv;
			float2 dv = ToScreen(WorldToViewDir(FaceDirection(CubeFace, st + float2(0.0, 2.0 / CubeSize)))) - uv;
			float3 sum = 0.0;
			float found = 0.0;
			[unroll] for (int y = 0; y < 4; y++) {
				[unroll] for (int x = 0; x < 4; x++) {
					float2 at = uv + du * ((x - 1.5) / 4.0) + dv * ((y - 1.5) / 4.0);
					bool usable = all(at > 0.0) && all(at < TESR_RenderScale.xy) && !IsWeapon(at) && readDepthLod(at) > 40.0;
					float3 c = linearize(tex2Dlod(TESR_RenderedBuffer, float4(at, 0, 0)).rgb);
					float brightest = max(c.r, max(c.g, c.b));
					sum += usable ? c * min(1.0, 32.0 / max(brightest, 1.0e-4)) : 0.0;
					found += usable ? 1.0 : 0.0;
				}
			}
			[branch] if (found > 0.0) {
				float2 edge = min(rectToScreen(uv), 1.0 - rectToScreen(uv));
				float blend = CaptureBlend * saturate(min(edge.x, edge.y) * 20.0) * found / 16.0;
				result = lerp(result, float4(sum / found, 1.0), blend);
			}
		}
	}
	return result;
}

// ---- 6: Filter, one face of one mip level per draw ----
// The capture cube averaged over this texel's footprint (a 4 x 4 grid of reads across it, a little wider than the texel so
// neighbouring texels overlap): mip m is the capture blurred to its own size, premultiplied, so the gaps stay empty and
// EnvAlong can fill them from a coarser level.
float4 FilterPS(VSOUT IN, float2 vpos : VPOS) : COLOR0 {
	float2 st = (vpos + 0.5) / CubeSize * 2.0 - 1.0;
	float3 dir = FaceDirection(CubeFace, st);
	[branch] if (CubeMip < 0.5) return texCUBElod(TESR_ReflectionsCaptureNew, float4(dir, 0.0));
	float2 texel = 2.0 / CubeSize;
	float4 sum = 0.0;
	[unroll] for (int y = 0; y < 4; y++) {
		[unroll] for (int x = 0; x < 4; x++) {
			float2 offset = (float2(x, y) - 1.5) / 4.0 * 1.5 * texel;
			sum += texCUBElod(TESR_ReflectionsCaptureNew, float4(FaceDirection(CubeFace, st + offset), 0.0));
		}
	}
	return sum / 16.0;
}

// ---- 1: Trace, half resolution ----
float4 TracePS(VSOUT IN, float2 vpos : VPOS) : COLOR0 {
	float2 uv = (vpos + 0.5) * LowTexel;
	float2 pixelUV = uv - 0.5 * (LowTexel - TESR_ReciprocalResolution.xy);   // a pixel centre in the block
	float depth = readDepthLod(pixelUV);
	float3 N = ViewNormal(pixelUV);
	float roughness;
	float amount = Reflectivity(pixelUV, depth, N, roughness);
	[branch] if (amount <= 0.0) return 0.0;

	float3 position = ViewPosition(pixelUV, depth);
	float3 V = normalize(position);   // eye -> surface

	// Spread the reflected direction by the roughness: a GGX half vector round the normal, a new one every frame
	// (the history and the blur average them into a blurred reflection).
	float a = roughness * roughness;
	float u1 = GradientNoise(vpos), u2 = GradientNoise(vpos + float2(37.0, 17.0));
	float cosTheta = sqrt((1.0 - u1) / (1.0 + (a * a - 1.0) * u1));
	float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
	float phi = 6.2831853 * u2;
	float3 tangent = normalize(abs(N.z) < 0.999 ? cross(N, float3(0, 0, 1)) : cross(N, float3(1, 0, 0)));
	float3 bitangent = cross(N, tangent);
	float3 H = normalize(tangent * (sinTheta * cos(phi)) + bitangent * (sinTheta * sin(phi)) + N * cosTheta);
	float3 R = reflect(V, H);
	[branch] if (dot(R, N) <= 0.0) R = reflect(V, N);   // spread below the surface: the plain mirror direction

	// March: steps that grow along the ray (fine near the surface, coarse far off), the first one shifted by the noise.
	float jitter = GradientNoise(vpos + float2(11.0, 71.0));
	float lastT = 0.0;
	float lastZ = position.z;
	float hitT = -1.0;
	// (No break: D3DX 9.43 compiled a break inside these nested branches so that the hit was never kept.)
	bool searching = true;
	[loop] for (float i = 1.0; i <= Steps && searching; i += 1.0) {
		float s = (i - 1.0 + jitter) / Steps;
		float t = RayLength * s * s + 2.0;
		float3 p = position + R * t;
		float2 at = ToScreen(p);
		[branch] if (p.z <= nearZ || any(at <= 0.0) || any(at >= TESR_RenderScale.xy)) searching = false;   // off the screen (the drawn part)
		else {
			float sceneZ = readDepthLod(at);
			float behind = p.z - sceneZ;
			// A hit if the ray went behind the surface during this step: within the depth the step itself covered (the
			// steps grow along the ray, so a fixed thickness let long steps jump through walls), plus a margin. Far
			// behind something thin in front: keep going, the ray may come out behind it.
			float thickness = (max(abs(p.z - lastZ), 20.0) * 1.5 + t * 0.03) * ThicknessScale;
			[branch] if (behind > 0.0 && behind < thickness && sceneZ < farZ * 0.98) { hitT = t; searching = false; }
			else { lastT = t; lastZ = p.z; }
		}
	}

	float3 colour = 0.0;
	float confidence = 0.0;
	[branch] if (hitT > 0.0) {
		float lo = lastT, hi = hitT;
		[unroll] for (int r = 0; r < RefineSteps; r++) {
			float m = 0.5 * (lo + hi);
			float3 q = position + R * m;
			if (q.z > readDepthLod(ToScreen(q))) hi = m; else lo = m;
		}
		float2 hit = ToScreen(position + R * hi);
		float3 hitNormal = ViewNormal(hit);
		// Not the weapon (it is not where the depth says, for the world), not the back of something.
		[branch] if (!IsWeapon(hit) && dot(hitNormal, R) < 0.2) {
			colour = linearize(tex2Dlod(TESR_RenderedBuffer, float4(hit, 0, 0)).rgb);
			float2 edge = min(rectToScreen(hit), 1.0 - rectToScreen(hit));
			confidence = saturate(min(edge.x, edge.y) * 12.0) * (1.0 - smoothstep(0.75, 1.0, hi / RayLength));
		}
	}
	// What the ray did not find on screen: the surroundings cube and (GridFallback) the light grid along it, then (outdoors)
	// the sky if it points up into it. Each fills only the share still missing (premultiplied sums). UNOFFICIAL 2026-10-19:
	// rough surfaces ask the grid first (position-correct, coarse: what a blurred reflection needs), smooth ones the cube first
	// (sharper, but seen from the camera, so it is in the wrong place for a puddle far ahead); the other fills what is left.
	float3 sum = colour * confidence;
	float3 worldR = ViewToWorldDir(R);
	float screenShare = confidence, cubeShare = 0.0, gridShare = 0.0, skyShare = 0.0;   // (DebugView 6)
	float4 grid = 0.0, env = 0.0;
	[branch] if (CubeOn > 0.0 && confidence < 1.0) env = EnvAlong(worldR, roughness);
	float gridFirst = smoothstep(0.15, 0.35, roughness);
	// The grid is read only where its share can be above 0: not where the cube goes first and covers the whole direction (wet,
	// smooth ground with a full cube), nor outdoors without the ortho map above the horizon band (GridAlong's share).
	bool gridUseful = (gridFirst > 0.0 || env.a < 1.0) && !(SkyFallback > 0.0 && RoofMaskOn < 0.5 && worldR.z >= 0.15);
	[branch] if (GridOn > 0.0 && ((confidence < 1.0 && gridUseful) || abs(DebugView - 5.0) < 0.5)) grid = GridAlong(ViewToWorldDir(position), worldR);
	[branch] if (confidence < 1.0 && (CubeOn > 0.0 || GridOn > 0.0)) {
		float missing = 1.0 - confidence;
		float cubeA = missing * env.a, gridA = (missing - cubeA) * grid.a;   // the cube first
		float gridB = missing * grid.a, cubeB = (missing - gridB) * env.a;   // the grid first
		cubeShare = lerp(cubeA, cubeB, gridFirst);
		gridShare = lerp(gridA, gridB, gridFirst);
		sum += env.rgb * cubeShare + grid.rgb * gridShare;
		confidence += cubeShare + gridShare;
	}
	[branch] if (confidence < 1.0 && SkyFallback > 0.0) {
		skyShare = (1.0 - confidence) * smoothstep(0.0, 0.15, worldR.z);
		skyShare *= lerp(1.0, lerp(NVR_ReflectionsSky.y, 1.0, tex2Dlod(NVR_ReflectionsSkyVis, float4(uv, 0, 0)).b), NVR_ReflectionsSky.x);	// UNOFFICIAL 2026-10-19 (no branch on the flag)
		sum += SkyAlong(worldR) * skyShare;
		confidence += skyShare;
	}
	colour = sum / max(confidence, 1.0e-4);
	// UNOFFICIAL 2026-10-19 diagnostics: 5 what the light grid gives along every reflected ray (its colour alone, wherever it
	// is read; black outside it), 6 where each share came from (green the screen, blue the cube, red the grid, grey the sky).
	[branch] if (DebugView > 4.5) {
		[branch] if (DebugView < 5.5) colour = grid.rgb;
		else colour = float3(gridShare, screenShare, cubeShare) + 0.5 * skyShare;
		return float4(Tame(colour), 1.0);
	}
	// Fireflies (in game 2026-10-12: tiny bright marquee bulbs caught by single rays sparkled across rough ground): a lower cap
	// on rough surfaces, and everything after this pass averages in a compressed space (Tame) where one very bright read
	// counts little; Untame in the composite turns the average back.
	float brightest = max(colour.r, max(colour.g, colour.b));
	colour *= min(1.0, lerp(BrightestReflection, 3.0, saturate(roughness * 2.0)) / max(brightest, 1.0e-4));
	return float4(Tame(colour) * confidence, confidence);
}

// ---- 3: Temporal, half resolution ----
float4 TemporalPS(VSOUT IN, float2 vpos : VPOS) : COLOR0 {
	float2 uv = (vpos + 0.5) * LowTexel;
	float4 current = tex2Dlod(TESR_ReflectionsTrace, float4(uv, 0, 0));
	float depth = readDepthLod(uv - 0.5 * (LowTexel - TESR_ReciprocalResolution.xy));

	bool hasValid = current.a > 1.0e-4;
	float3 curC = hasValid ? max(current.rgb / current.a, 0.0) : 0.0;
	float3 lowC = curC, highC = curC;
	float lowA = current.a, highA = current.a;
	[unroll] for (int y = -1; y <= 1; y++) {
		[unroll] for (int x = -1; x <= 1; x++) {
			if (x == 0 && y == 0) continue;
			float4 tap = tex2Dlod(TESR_ReflectionsTrace, float4(uv + float2(x, y) * LowTexel, 0, 0));
			lowA = min(lowA, tap.a); highA = max(highA, tap.a);
			if (tap.a > 1.0e-4) {
				float3 tc = max(tap.rgb / tap.a, 0.0);
				if (!hasValid) { lowC = tc; highC = tc; hasValid = true; }
				else { lowC = min(lowC, tc); highC = max(highC, tc); }
			}
		}
	}

	float3 world = toWorld(uv) * depth;
	// UNOFFICIAL 2026-10-29 ObjectMotion (core/ObjectMotion.h): a surface that moved itself was that far back along its path. Stage 11
	// (TESR_OrthoMapBuffer, which this pass never reads) holds TESR_ObjectMotionBuffer for this pass only (Reflections.cpp RenderBuffers).
	[branch] if (TESR_ObjectMotionData.x > 0.0) world -= tex2Dlod(TESR_OrthoMapBuffer, float4(uv - 0.5 * (LowTexel - TESR_ReciprocalResolution.xy), 0, 0)).xyz;
	float4 clip = mul(float4(world, 1.0), TESR_ReflectionsReproject);
	float w = max(clip.w, 0.001);
	float2 last = screenToRect(float2(clip.x / w * 0.5 + 0.5, 0.5 - clip.y / w * 0.5));	// UNOFFICIAL 2026-10-27 RenderScale v2: the history is in the rect too
	float inside = all(last > 0.0) && all(last < TESR_RenderScale.xy) && highA > 0.0;
	float4 history = tex2Dlod(TESR_ReflectionsHistory, float4(last, 0, 0));
	if (hasValid && history.a > 1.0e-4) {
		float3 hc = clamp(max(history.rgb / history.a, 0.0), lowC, highC);
		float hl = dot(hc, float3(0.2126, 0.7152, 0.0722));
		float maxTameL = BrightestReflection / (1.0 + BrightestReflection);
		if (hl > maxTameL) hc *= maxTameL / hl;
		float ha = clamp(history.a, lowA, highA);
		history = float4(hc * ha, ha);
	} else {
		history = 0.0;
	}
	return lerp(current, history, HistoryWeight * inside);
}

// ---- 2 / 4: Blur X / Blur Y, half resolution ----
float4 BlurAxis(sampler2D source, float2 vpos, float2 axis) {
	float2 uv = (vpos + 0.5) * LowTexel;
	float4 centre = tex2Dlod(source, float4(uv, 0, 0));
	float2 pixelUV = uv - 0.5 * (LowTexel - TESR_ReciprocalResolution.xy);
	float depth = readDepthLod(pixelUV);
	float roughness;
	float amount = Reflectivity(pixelUV, depth, ViewNormal(pixelUV), roughness);
	[branch] if (amount <= 0.0) return centre;

	// Wider on rough ground, narrow on wet ground, so puddles stay sharp.
	// (2026-11-06) mirrors get no blur: none below roughness 0.03, full from 0.15 (as Compute/ReflectionsBlur.frag)
	float radius = BlurRadius * saturate(0.5 + roughness * 2.0) * smoothstep(0.03, 0.15, roughness);
	float sigma = max(radius * 0.5, 0.5);
	float falloff = -0.5 / (sigma * sigma);
	float tolerance = depth * 0.03 + 4.0;
	float4 sum = centre;
	float weights = 1.0;
	[unroll] for (int i = 1; i <= 8; i++) {
		float gauss = i <= radius ? exp(falloff * i * i) : 0.0;
		float2 ua = clampToRectIfOn(uv + axis * i * LowTexel, LowTexel), ub = clampToRectIfOn(uv - axis * i * LowTexel, LowTexel);	// UNOFFICIAL 2026-10-27 RenderScale v2
		float da = readDepthLod(ua - 0.5 * (LowTexel - TESR_ReciprocalResolution.xy)), db = readDepthLod(ub - 0.5 * (LowTexel - TESR_ReciprocalResolution.xy));
		float wa = gauss / (1.0 + abs(da - depth) / tolerance);
		float wb = gauss / (1.0 + abs(db - depth) / tolerance);
		sum += tex2Dlod(source, float4(ua, 0, 0)) * wa + tex2Dlod(source, float4(ub, 0, 0)) * wb;
		weights += wa + wb;
	}
	return sum / weights;
}

float4 BlurXPS(VSOUT IN, float2 vpos : VPOS) : COLOR0 { return BlurAxis(TESR_ReflectionsTemp, vpos, float2(1.0, 0.0)); }
float4 BlurYPS(VSOUT IN, float2 vpos : VPOS) : COLOR0 { return BlurAxis(TESR_ReflectionsTrace, vpos, float2(0.0, 1.0)); }

// ---- 0: Composite, full resolution ----
float4 CompositePS(VSOUT IN) : COLOR0 {
	float2 uv = IN.UVCoord;
	float4 color = tex2Dlod(TESR_RenderedBuffer, float4(uv, 0, 0));
	float depth = readDepthLod(uv);
	// UNOFFICIAL 2026-10-19 DebugView 4: where the rain wets the ground: white on floors open to the sky (brighter the wetter it
	// is now), dark red on floors the rain cannot reach (covered, by the ortho map's roof test), the rest of the picture dimmed.
	[branch] if (abs(DebugView - 4.0) < 0.5) {
		bool isFloor = ViewToWorldDir(ViewNormal(uv)).z > 0.75 && depth < DrawDistance && depth < farZ * 0.98;
		[branch] if (!isFloor) return float4(color.rgb * 0.15, color.a);
		float open = RoofMaskOn > 0.5 ? OpenAbove(uv, depth) : 1.0;
		return open > 0.5 ? float4((0.4 + 0.6 * saturate(Wetness)).xxx, 1.0) : float4(0.5, 0.05, 0.05, 1.0);
	}
	// DebugView 3: the surroundings cube as seen from here, over the whole screen (it should look like a blurred, slightly
	// late copy of the picture; black where nothing was seen yet).
	[branch] if (abs(DebugView - 3.0) < 0.5) {
		float4 env = EnvAlong(normalize(toWorld(uv)), 0.0);
		return float4(delinearize(env.rgb * env.a), 1.0);
	}
#if REFL_MATERIAL_ON || REFL_GBUFFER_ON
	// UNOFFICIAL 2026-10-20 DebugView 7: the material buffer (MaterialAware): red the roughness, green the metal, blue the smoothness,
	// where a material is known (dark red: rough; blue: smooth; green/yellow: metal); the rest of the picture dimmed (the floor guess).
	[branch] if (abs(DebugView - 7.0) < 0.5) {
#if REFL_GBUFFER_ON
		float4 m = 0.0;   // (2026-11-20 UnifiedReflections: the G-buffer's material as Reflectivity decodes it)
		[branch] if (TESR_ReflectionsGBuffer.x > 0.5) m = GBufferMaterial(uv);
		else m = tex2Dlod(NVR_ReflectionsMaterial, float4(uv, 0, 0));
#else
		float4 m = tex2Dlod(NVR_ReflectionsMaterial, float4(uv, 0, 0));
#endif
		// UNOFFICIAL 2026-10-31 MaterialBufferRoughness (b 0.5): the game shader's roughness and metal, in grey (rough) to white (smooth) + green metal
		[branch] if (m.b > 0.25 && m.b <= 0.75) return float4((1.0 - m.r).xx * float2(1.0 - m.g, 1.0), 1.0 - m.r, 1.0);
		return m.b > 0.5 ? float4(m.r, m.g, 0.6 * (1.0 - m.r), 1.0) : float4(color.rgb * 0.15, color.a);
	}
#endif
	float3 N = ViewNormal(uv);
	float roughness;
	float amount = Reflectivity(uv, depth, N, roughness);
	[branch] if (amount <= 0.0) return DebugView > 0.0 ? float4(color.rgb * 0.15, color.a) : color;

	// Depth-aware bilinear upsample (half-resolution texel k stands for its centre; its depth is read again here).
	float2 position = uv / LowTexel - 0.5;
	float2 base = floor(position);
	float2 fraction = position - base;
	float tolerance = depth * 0.03 + 4.0;
	bool pixelWeapon = IsWeapon(uv);
	float4 reflection = 0.0;
	float weights = 1.0e-5;
	[unroll] for (int y = 0; y < 2; y++) {
		[unroll] for (int x = 0; x < 2; x++) {
			float2 at = clampToRectIfOn((base + float2(x, y) + 0.5) * LowTexel, LowTexel);	// UNOFFICIAL 2026-10-27 RenderScale v2
			float2 tapPixelUV = at - 0.5 * (LowTexel - TESR_ReciprocalResolution.xy);
			float tapDepth = readDepthLod(tapPixelUV);
			float weight = (x ? fraction.x : 1 - fraction.x) * (y ? fraction.y : 1 - fraction.y);
			weight /= 1.0 + abs(tapDepth - depth) / tolerance;
			if (IsWeapon(tapPixelUV) != pixelWeapon) weight *= 1.0e-4;
			reflection += tex2Dlod(TESR_ReflectionsBuffer, float4(at, 0, 0)) * weight;
			weights += weight;
		}
	}
	reflection /= weights;

	float3 V = normalize(ViewPosition(uv, depth));
	float NdotV = saturate(dot(-V, N));
	// Stage 14 (NVR_ReflectionsSkyVis, unused by CompositePS) carries TESR_AlbedoBuffer when bound by BindMaterial (TESR_ReflectionsWetFx2.z > 0.5).
	float4 albedoSample = TESR_ReflectionsWetFx2.z > 0.5 ? tex2Dlod(NVR_ReflectionsSkyVis, float4(uv, 0, 0)) : 0.0.xxxx;
	bool albedoKnown = albedoSample.a > 0.5;
#if REFL_MATERIAL_ON || REFL_GBUFFER_ON
	float3 metalF0;
	if (albedoKnown) {
		metalF0 = max(albedoSample.rgb, 0.02.xxx);
	} else {
		float3 litColour = linearize(color.rgb);
		float3 tint = saturate(litColour / max(max(litColour.r, max(litColour.g, litColour.b)), 1.0e-4));
#if REFL_GBUFFER_ON
		[branch] if (SurfaceGBuffer) tint = saturate(tex2Dlod(NVR_ReflectionsGAlbedo, float4(uv, 0, 0)).rgb);   // (2026-11-20 UnifiedReflections: its own base colour, linear)
#endif
		metalF0 = ReflMetalF0(tint, roughness);   // (perf-reflfix4)
	}
	float3 f0 = lerp(0.04.xxx, metalF0, SurfaceMetal);
	float3 share = saturate(amount * EnvironmentFresnelF0(roughness, NdotV, f0)) * saturate(reflection.a);
	float nonMetal = saturate(1.0 - SurfaceMetal);
#else
	float3 share = saturate(amount * EnvironmentFresnel(roughness, NdotV)).xxx * saturate(reflection.a);
	float nonMetal = 1.0;
#endif
	float3 reflected = Untame(reflection.rgb / max(reflection.a, 1.0e-4));
#ifndef REFL_SPECOCC_ON
    #define REFL_SPECOCC_ON 0
#endif
#if REFL_SPECOCC_ON
	[branch] if (TESR_ReflectionsData4.w > 0.0) {
		float surfaceLight = luma(linearize(color.rgb));
		float reflectedLight = max(luma(reflected) * TESR_ReflectionsLayout.z, 1.0e-6);
		float occlusion = lerp(1.0, saturate(surfaceLight / reflectedLight), TESR_ReflectionsData4.w);
		[branch] if (abs(DebugView - 8.0) < 0.5) return float4(saturate(surfaceLight * 8.0), saturate(reflectedLight * 8.0), occlusion, 1.0);
		share *= occlusion;
	}
#endif

	[branch] if (DebugView > 4.5) return float4(delinearize(reflected), 1.0);   // 5, 6: the trace's diagnostic colours as they are
	[branch] if (DebugView > 1.5) return float4(1.0 - reflection.a, reflection.a, amount * 0.5, 1.0);   // red: nothing found
	[branch] if (DebugView > 0.5) return float4(delinearize(reflected * share), color.a);

	float3 lin = linearize(color.rgb);
	// 1. Subtract forward WetSkyReflection on wet surfaces under both SkylightingMode 0 and 1.
	[branch] if (TESR_ReflectionsWetFx.x > 0.5 && SurfaceWetGloss > 0.0) {
		float3 worldN = ViewToWorldDir(N);
		float3 worldV = -normalize(toWorld(uv));
		float3 worldR = reflect(-worldV, worldN);
		float horizon = saturate(1.0 + 1.2 * dot(worldR, worldN));
		float wetRefl = WetFilmReflectanceFx(saturate(dot(worldN, worldV)), SurfaceWetRough, SurfaceWetGloss) * horizon * horizon;
		lin = max(lin - WetSkyReflectionLinearFx(worldR) * (wetRefl * saturate(reflection.a)), 0.0.xxx);
	}
#ifndef REFL_ADD_ON
    #define REFL_ADD_ON 0
#endif
#if REFL_ADD_ON
	float reflectAdd = saturate(TESR_ReflectionsData4.z);
#else
	float reflectAdd = 0.0;
#endif
	// 2. Physically energy-conserving additive indirect reflection: metals keep 100% of lin (preserving direct sun & lamp GGX highlights);
	//    dielectrics attenuate only the diffuse body up to diffuseCeiling while keeping direct specular highlights un-dimmed.
	float3 diffuseCeiling = albedoKnown ? max(albedoSample.rgb * 2.0, 0.25.xxx) : 1.0.xxx;
	float3 bodyToDim = min(lin, diffuseCeiling);
	lin = (lin - bodyToDim * (share * (nonMetal * (1.0 - reflectAdd)))) + reflected * share;
	return float4(delinearize(lin), color.a);
}

technique Composite {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 CompositePS();
	}
}

technique Trace {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 TracePS();
		Texture[4] = <NoTexture>;   // writes Trace (Temporal on) or Temp (off); reads neither
		Texture[6] = <NoTexture>;
	}
}

technique BlurX {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 BlurXPS();
		Texture[4] = <NoTexture>;   // writes Trace
	}
}

technique Temporal {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 TemporalPS();
		Texture[6] = <NoTexture>;   // writes Temp
	}
}

technique BlurY {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 BlurYPS();
		Texture[5] = <NoTexture>;   // writes Buffer
	}
}

technique Capture {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 CapturePS();
		Texture[9] = <NoTexture>;   // writes the new capture cube; reads last frame's at s8
		Texture[10] = <NoTexture>;
	}
}

technique Filter {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 FilterPS();
		Texture[10] = <NoTexture>;  // writes the reflection cube; reads the new capture cube at s9
		Texture[8] = <NoTexture>;
	}
}

#if REFL_MATERIAL_ON
// ---- 7: Material, full resolution (MaterialAware), before any effect draws: the scene's alpha decoded (Includes/Object.hlsl
// ReflMaterialAlpha: 2 + 0.25 x metal in 4 steps + 0.24 x roughness). Anything under 2 is the game's own alpha (no material); an
// edge the game's MSAA averaged with such a pixel falls under 2 as well, or decodes as a mix of the two materials.
float4 MaterialCapturePS(VSOUT IN, float2 vpos : VPOS) : COLOR0 {
	float a = tex2Dlod(TESR_RenderedBuffer, float4((vpos + 0.5) * TESR_ReciprocalResolution.xy, 0, 0)).a - 2.0;
	[branch] if (a < 0.0 || a >= 1.0) return 0.0;
	float metal = floor(a * 4.0);
	return float4(saturate((a - metal * 0.25) / 0.24), metal / 3.0, 1.0, 1.0);
}

technique MaterialCapture {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 MaterialCapturePS();
		Texture[15] = <NoTexture>;  // writes the material buffer
	}
}
#endif
