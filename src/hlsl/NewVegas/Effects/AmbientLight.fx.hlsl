// UNOFFICIAL 2026-10-14: ambient occlusion and bounce light in one pass ([Shaders.AmbientLight], src/effects/AmbientLight.cpp).
// While it is on it replaces the AmbientOcclusion and BounceLight effects (ShaderManager skips them).
//
// Both come from the same reads: for each half-resolution pixel, two directions across the screen (turned per pixel by blue
// noise), four reads each way along each. Along every direction the highest the surroundings rise above the pixel's surface (its
// "horizon") gives how much of the sky above the pixel they hide: ambient occlusion, worked out as in "Practical Real-Time
// Strategies for Accurate Indirect Occlusion" (Jimenez et al. 2016, GTAO), cosine-weighted, so creases darken and open ground
// does not. Each read also sends the light it shows on screen to the pixel, as far as it rises above the pixel's surface and the
// two face each other: the bounce light (first after "Horizon-based indirect lighting", Mayaux 2018, counting only the part of
// the view each read newly covered; UNOFFICIAL 2026-10-14 every read counts smoothly instead, see Estimate: far less shimmer).
//
// Pass 1 Prepare (half): depth, normal and colour of each half-resolution pixel packed into one texel (as BounceLight's), so
//        each read of the estimate is one small texel.
// Pass 2 Estimate (half): rgb = bounce light, a = visibility (1 open, 0 fully hidden).
// Pass 3-4 Blur (half): depth-aware, horizontal then vertical, all four channels.
// Pass 5 Accumulate (half, UNOFFICIAL 2026-10-14, user: "shimmers really hard above 1 bounce strength", worst on grass): the
//        bounce light blended into last frame's at the same spot (found by reprojection, kept only where the depth there
//        matches, less of it the further the spot moved, and held within the range of the bounce round it now so swaying grass
//        does not smear). With it the estimate's noise turns every frame, so the frames average out. rgb = bounce, a = view depth
//        (next frame's match). Off ([Shaders.AmbientLight.Main] Temporal) or after a break: this frame's bounce as it is.
// Pass 6 Apply (full): depth-aware upsample; the scene darkened by the occlusion (less on bright surfaces, as the old AO) and the
//        bounce added, times the surface's own colour as one picture tells it (SurfaceColour; there is no albedo buffer).
//
// UNOFFICIAL 2026-10-14 cheaper: outdoors, the sun-shadow apply (ShadowsExteriors.fx.hlsl, a full-resolution pass of its own while
// this effect is on) is done here instead (NVR_ALFlags.x): in Apply before the occlusion, and in Prepare on the colour the bounce
// light is gathered from, so both see the scene as they did after that pass. The blurs and the apply take their depths from the
// depth buffer (4 bytes a read) instead of the 16-byte prepared texel.
//
// UNOFFICIAL 2026-10-20 (skyamb) Skylighting ([Shaders.AmbientOcclusion.Skylighting], outdoors): this effect switches the
// AmbientOcclusion effect off, and with it the sky test that lives there, so the sky light under roofs, porches and overhangs stayed.
// Now the AO's sky pass runs just before this effect (ShaderManager) and technique AmbientLightSky (the end of the file) applies its
// factor in Apply, as the AO's combine does. Without the sky the first technique runs, unchanged. (Measured 2026-10-20: the same test
// inside Estimate, written as a second target, made Estimate 0.17-0.23 ms slower against the sky pass's 0.16-0.20 ms: dropped.)

float4 TESR_AmbientLightData;  // x: radius (world units), y: occlusion strength, z: bounce strength, w: draw distance
float4 TESR_AmbientLightData2;  // x: darkest the occlusion may make a pixel (0-1), y: brightness above which it fades, z: blur radius (half-res texels per tap), w: debug view (1 occlusion, 2 bounce)
float4 TESR_ReciprocalResolution;
float4 NVR_ALLayout;            // xy: half-resolution size / full size, zw: 1 / half-resolution size
float4 NVR_ALFlags;             // x: apply the sun shadows here (ShaderManager skips their own pass), y: NoEdgeOutline (UNOFFICIAL 2026-10-21), z: 1 = NVR_ALAlbedo holds this frame's albedo (UNOFFICIAL 2026-10-31 AlbedoBuffer), w: 1 = EdgeReject (UNOFFICIAL 2026-11-01, Apply)
float4 NVR_ALInterior;          // UNOFFICIAL 2026-10-18 (ambfix2): x 1 = indoor balance (InteriorBalance), y 1 = last frames' balance usable, z InteriorBounceShare, w share of last frames' balance kept per frame
float4 NVR_ALReach;             // UNOFFICIAL 2026-11-01 MinReach: x 1 = every read at least y texels from the pixel (Estimate; y MinReachTexels, 2026-11-02)
float4 NVR_ALScene;             // UNOFFICIAL 2026-10-18 (ambfix): rgb the light every surface gets (linear, on a white surface: the cell's ambient + half its directional indoors, the sky's ambient outdoors), w: share of the grid light's colour kept (GridColour)
float4 NVR_ALTemporal;          // x: share of last frame's bounce kept (0 none), y: moved pixels that halve it, z: frame number (0-1023),
                                // w: UNOFFICIAL 2026-11-01 TemporalJitterFix, the depth jitter's change since last frame, screen uv x (NVR_ALEdges.w: y)
float4 NVR_ALEdges;             // UNOFFICIAL 2026-10-21 (alrocks): x how far the normal is smoothed (reach, half-resolution texels, 0 off; technique AmbientLightSmooth), y the horizon bias (radians, 0 off), z (2026-10-24) the reach in world units where wider (0 off)
float4x4 NVR_ALReproject;       // this frame's view space -> last frame's clip space (BounceLightMath.h)
float4 TESR_WaterSettings;      // x: water height, z: camera underwater
float4 TESR_ShadowData;         // y: darkness
float4 TESR_ShadowFade;         // x: fading at sunrise/sunset, z: point light shadows
float4 TESR_SkyColor;
float4 TESR_SunAmbient;
float4 TESR_SunDirection;
// UNOFFICIAL 2026-10-20 (skyamb) Skylighting, for technique AmbientLightSky only (AmbientOcclusion.cpp's).
float4 TESR_AmbientOcclusionSky5;	// x LightCap, w no limit (Includes/SkyTake.hlsl)
float4 TESR_AmbientOcclusionData;	// z: BlurDropThreshold (the AO combine's upsample weights)

sampler2D TESR_DepthBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_SourceBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_NormalsBuffer : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_BlueNoiseSampler : register(s3) < string ResourceName = "Effects\bluenoise256.dds"; > = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_PointShadowBuffer : register(s4) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
// Bound by AmbientLight.cpp per pass (not TESR_ samplers: those bind by their order in the file). After the TESR_ ones.
sampler2D NVR_ALPrep : register(s5) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D NVR_ALBuffer : register(s6) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D NVR_ALHistory : register(s7) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
// UNOFFICIAL 2026-10-14: the occlusion kept from frame to frame too (r visibility, g view depth): with the sample pattern turning every
// frame the occlusion alone showed a new grain each frame (user: "a sort of noise filter over everything").
// UNOFFICIAL 2026-10-29 ObjectMotion (core/ObjectMotion.h): the world move of what moved itself since last frame (0 elsewhere); s8 is the grid
// passes' (NVR_GridWave), never read in Accumulate. TESR_ObjectMotionData.x 0 = not made this frame: Accumulate reprojects as before.
sampler2D TESR_ObjectMotionBuffer : register(s8) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
float4 TESR_ObjectMotionData;
sampler2D NVR_ALOcclusionHistory : register(s11) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
// UNOFFICIAL 2026-10-31 BackDepth (Includes/BackDepth.hlsl): a read in front of the pixel whose object's back face is known
sampler2D TESR_BackDepthBuffer : register(s12) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
// UNOFFICIAL 2026-10-31 AlbedoBuffer ([Shaders.AmbientLight.Main] AlbedoBuffer, live; core/CapturedNormals.h TESR_AlbedoBuffer): the surfaces' own linear base
// colour where NVR's templates wrote it this frame (a 1), for the bounce's surface colour in Apply and in the balance samples (SurfaceAlbedo); bound by
// AmbientLight.cpp on s12 for those two passes only (the grid passes' NVR_GridSunColor stage, which they never read). NVR_ALFlags.z 1 = bound.
sampler2D NVR_ALAlbedo : register(s12) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
// UNOFFICIAL 2026-11-03 RT GI on the D3D9 passes ([Shaders.AmbientOcclusion.RT] GI with [Main.Main.Performance] NvrDxvkAmbientLight off,
// its default since 2026-11-01): before this, the RT bounce light reached the picture only through the Vulkan twins of these passes
// (AmbientLightEstimateRT / AmbientLightGIAccumulate / AmbientLightApply-gi), so by default RT GI changed nothing. Now the same three
// steps run here, line for line with those shaders, switched by NVR_ALGI (all 0 = every pass exactly as before):
//   x 1: Estimate puts the traced bounce light (NVR_ALRTGI, bound by hand on s7, which Estimate never reads otherwise) into rgb, x the fade;
//   y 1: Accumulate keeps the bounce from frame to frame as it keeps the occlusion (same reprojection, depth test, neighbour clamp, share);
//   z 1: Apply adds this frame's accumulated bounce (NVR_ALGIHistory, bound by hand on s5: Apply never reads the prep) upsampled with the
//        occlusion's own depth-weighted taps, x the surface colour x TESR_AmbientLightData.z (GIStrength). DebugView 13: the light alone.
float4 NVR_ALGI;
sampler2D NVR_ALRTGI : register(s7) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D NVR_ALGIHistory : register(s5) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };

static const float Radius = max(TESR_AmbientLightData.x, 1.0f);
static const float OcclusionStrength = TESR_AmbientLightData.y;
static const float BounceStrength = TESR_AmbientLightData.z;
static const float DrawDistance = max(TESR_AmbientLightData.w, 100.0f);
static const float DarkestAO = saturate(TESR_AmbientLightData2.x);
static const float BrightFade = TESR_AmbientLightData2.y;
static const float BlurStep = max(TESR_AmbientLightData2.z, 0.5f);
static const float DebugView = TESR_AmbientLightData2.w;
static const int SLICES = 2;
static const int STEPS = 4;
static const float BounceScale = 2.5f;       // UNOFFICIAL 2026-10-14: the smooth bounce's level, matched to the old one's average in game (2.0 read ~20% lower)
static const float NearFade = 40.0f;          // view depth (world units) below which nothing is done: the weapon, the camera's own body
static const float MaxScreenRadius = 0.08f;   // largest radius on screen, as a share of the screen width (keeps the reads in cache)

struct VSOUT { float4 vertPos : POSITION; float2 UVCoord : TEXCOORD0; };
struct VSIN { float4 vertPos : POSITION0; float2 UVCoord : TEXCOORD0; };

VSOUT FrameVS(VSIN IN) {
	VSOUT OUT = (VSOUT)0.0f;
	OUT.vertPos = IN.vertPos;
	OUT.UVCoord = IN.UVCoord;
	return OUT;
}

#include "Includes/Helpers.hlsl"
#include "Includes/Depth.hlsl"
#include "Includes/BackDepth.hlsl"
#include "Includes/Normals.hlsl"

// UNOFFICIAL 2026-10-20 (skyamb) Skylighting (technique AmbientLightSky).
static const int endFade = 8000;	// (as AmbientOcclusion.fx.hlsl's: no sky factor beyond it)
#include "Includes/SkyTake.hlsl"

// UNOFFICIAL 2026-10-14 (user: an orange tip on the first-person gun): the weapon and arms' own depth, bound by hand (an NVR_
// sampler: a TESR_ one would bind by its place in the file). Those pixels get nothing from this effect and stand in for no
// surface round about (they would darken the ground behind the gun).
sampler2D NVR_ALViewModel : register(s15) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
bool IsViewModel(float2 uv) {
	float d = tex2Dlod(NVR_ALViewModel, float4(uv, 0.0f, 0.0f)).x;
	return (TESR_DepthConstants.z == 0 && d < 0.9f) || (TESR_DepthConstants.z > 0 && d > 0.01f);
}

// ShadowsExteriors.fx.hlsl Shadow() on a gamma-encoded colour (the same maths as VolumetricFog.fx.hlsl CompositeSunShadow).
// UNOFFICIAL 2026-10-19 SunFar (SunFar.fx.hlsl): the land's and the clouds' shadow, a factor on the stored (gamma) colour, bound by
// hand on stage 13 while NVR_SunFarFlags.x is 1 (AmbientLight.cpp); stage 13 is otherwise the grid technique's.
float4 NVR_SunFarFlags;
sampler2D NVR_SunFarAL : register(s13) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
float3 SunShadowApply(float3 color, float2 uv) {
	color *= lerp(1.0f, tex2Dlod(NVR_SunFarAL, float4(uv, 0.0f, 0.0f)).rgb, NVR_SunFarFlags.x);	// (no branch on the flag: the composites have no boolean register to spare with the physical sky compiled in; 0 or 1)
	[branch] if (TESR_WaterSettings.z == 1) {
		float3 worldPos = TESR_CameraPosition.xyz + toWorld(uv) * readDepthLod(uv);
		if (worldPos.z < (TESR_WaterSettings.x + 2) && worldPos.z > (TESR_WaterSettings.x - 2) && dot(GetWorldNormalLod(uv), float3(0, 0, -1)) > 0.999)
			return color;
	}
	float darkness = max(0.0, 1 - TESR_ShadowData.y);
	float2 shadow = tex2Dlod(TESR_PointShadowBuffer, float4(uv, 0, 0)).rg;
	shadow.r = lerp(TESR_ShadowFade.x, 1.0f, shadow.r);
	float ambient = lerp(1, luma(TESR_SunAmbient), darkness * TESR_ShadowFade.z);
	shadow.r = lerp(0, ambient, shadow.r);
	shadow.r += shadow.g;
	shadow.r = saturate(lerp(darkness, 1.0, shadow.r));
	float3 linearColor = pows(color, 2.2);
	float3 colorShadow = luma(linearColor) * shadow.r * pows(TESR_SkyColor.rgb, 2.2);
	colorShadow = lerp(colorShadow, linearColor * shadow.r, saturate(shadow.r + 0.5));
	return pows(max(0.0, colorShadow), 1.0 / 2.2);
}

// View-space position (x right, y up, z forward, world units) from a screen position and the linear depth (view z / far z).
float3 PositionFromLinear(float2 uv, float linear01) {
	uv = rectToScreen(uv);	// UNOFFICIAL 2026-10-27 RenderScale v2: callers pass texture coordinates, the ray needs the screen position
	float z = projectedDepthFromLinear(linear01);
	float4 viewSpace = mul(float4(uv.x * 2 - 1, (1 - uv.y) * 2 - 1, z, 1.0f), TESR_InvProjectionTransform);
	return viewSpace.xyz / viewSpace.w;
}

// View-space position from a screen position and its view depth: two multiply-adds (the projection is symmetric).
float3 PositionFromViewZ(float2 uv, float z) {
	uv = rectToScreen(uv);	// UNOFFICIAL 2026-10-27 RenderScale v2 (as PositionFromLinear)
	return float3((uv.x * 2.0f - 1.0f) / TESR_ProjectionTransform[0][0], (1.0f - uv.y * 2.0f) / TESR_ProjectionTransform[1][1], 1.0f) * z;
}

// Two values in [0, 1] as 12-bit integers in one 32-bit float, and a unit vector as two numbers in [0, 1] (as BounceLight's).
float Pack2(float2 v) { v = floor(saturate(v) * 4095.0f + 0.5f); return v.x * 4096.0f + v.y; }
float2 Unpack2(float p) { float high = floor(p / 4096.0f); return float2(high, p - high * 4096.0f) / 4095.0f; }
float2 OctEncode(float3 n) {
	n /= abs(n.x) + abs(n.y) + abs(n.z);
	float2 s = n.xy >= 0.0f ? 1.0f : -1.0f;
	float2 e = n.z >= 0.0f ? n.xy : (1.0f - abs(n.yx)) * s;
	return e * 0.5f + 0.5f;
}
float3 OctDecode(float2 e) {
	e = e * 2.0f - 1.0f;
	float3 n = float3(e, 1.0f - abs(e.x) - abs(e.y));
	float t = saturate(-n.z);
	n.xy += n.xy >= 0.0f ? -t : t;
	return normalize(n);
}

// Pass 1: x = view depth (world units), y = normal, z = colour red and green (square-root encoded), w = colour blue (linear light, capped
// so small bright glints do not spray light around).
// UNOFFICIAL 2026-10-14 (user: remove the screen bounce; the light grid is the bounce light): depth and normal only.
// UNOFFICIAL 2026-10-21 (alrocks; user: with AmbientLight on, in the dark, the light cuts off hard along the triangle edges of low-poly
// rocks, one face dark and its neighbour light). Every normal this effect and Skylighting's sky test work from is Normals.fx.hlsl's:
// rebuilt from the depth buffer, so every triangle gets one flat normal that flips at each edge (no smooth vertex normal exists: the
// object shaders keep theirs in tangent space, and the game's MSAA rules out a second target). Measured 2026-10-21 (rocks24, 20.5 h):
// the black faces are Skylighting's (its rays count by their cosine to that normal, so a face turned from the sky sees none and
// SkyTake takes all its light at night: 30 % of the rock black, 0.4 % with Skylighting off); the occlusion here steps too, mildly.
// Mend, live ([Shaders.AmbientLight.Main] NormalSmoothing = reach r in half-resolution texels, 0 off): after Prepare, technique
// AmbientLightSmooth averages the prep's normal along x then along y (3 taps each side at r/3, 2r/3 and r), each tap counted as far as
// it lies on the pixel's own tangent plane (a crease has continuous depth, a silhouette is a depth step and keeps its edge; the
// tolerance grows with the distance so a neighbouring face bent up to ~30 degrees still counts) and as far as its normal is within
// about 40-60 degrees of the pixel's (a wall meeting the floor stays a crease). Estimate reads the smoothed prep, and the sky test runs
// after it and reads the same texel (AmbientOcclusion.fx.hlsl NVR_SkyPrep). A 5 x 5 or 24-tap kernel inside Prepare was tried first:
// +0.21 / +0.64 ms, and the faces are 50-150 pixels across. HorizonBias (Estimate, degrees above the tangent plane each horizon starts)
// measured no help on the faces (it only lightens): default 0.
float4 Prepare(VSOUT IN) : COLOR0 {
	float2 uv = IN.UVCoord;
	float viewZ = PositionFromLinear(uv, tex2Dlod(TESR_DepthBuffer, float4(uv, 0.0f, 0.0f)).x).z;
	if (IsViewModel(uv)) viewZ = 1.0e6f;	// the weapon: as if open sky behind (nothing done, hides nothing)
	float3 normal = normalize(tex2Dlod(TESR_NormalsBuffer, float4(uv, 0.0f, 0.0f)).xyz * 2 - 1);
	return float4(viewZ, Pack2(OctEncode(normal)), 0.0f, 0.0f);
}

// Technique AmbientLightSmooth (see above): pass X reads the prep (NVR_ALPrep) and writes (normal, view depth) into the ping-pong
// buffer; pass Y reads that (NVR_ALBuffer) and writes the prep again, packed as Prepare does.
float4 SmoothNormal(VSOUT IN, uniform bool alongY) : COLOR0 {
	float2 uv = IN.UVCoord;
	float viewZ; float3 normal;
	// UNOFFICIAL 2026-11-09 (rt-review; docs/next/RT-PT-REVIEW-2026-11-09.md section 6) pass X's target is a 16-bit float buffer (largest value 65504):
	// the sky's depth (the far plane, ~350000) and the weapon's 1.0e6 came back from it as INFINITE, and pass Y's taps on them worked out
	// inf - inf = not a number in the plane test. What a GPU does with that in saturate / max differs by vendor and by D3D9 / Vulkan: on some the
	// normals within the reach of a skyline or of the weapon's outline turned to garbage, new garbage every frame (flicker along skylines
	// for some players only). Pass X now stores at most 64000 and pass Y reads 64000 or more back as the 1.0e6 'nothing here' it stood for.
	if (alongY) { float4 c = tex2Dlod(NVR_ALBuffer, float4(uv, 0.0f, 0.0f)); normal = c.xyz; viewZ = c.w >= 64000.0f ? 1.0e6f : c.w; }
	else { float4 c = tex2Dlod(NVR_ALPrep, float4(uv, 0.0f, 0.0f)); viewZ = c.x; normal = OctDecode(Unpack2(c.y)); }
	[branch] if (viewZ < DrawDistance && NVR_ALEdges.x > 0.5f) {
		float3 origin = PositionFromViewZ(uv, viewZ);
		float2 axis = alongY ? float2(0.0f, 1.0f) : float2(1.0f, 0.0f);
		// UNOFFICIAL 2026-10-24 (charfacets) NormalSmoothingWorld: the reach is at least NVR_ALEdges.z world units (one half-resolution texel
		// is 2 viewZ / (P00 x half width) world units here, PositionFromViewZ's slope), at most 192 texels; so near the camera, where a
		// character's triangles are hundreds of pixels across, the average still spans a couple of them (as vertex normals would blend).
		float texelWorld = 2.0f * viewZ * NVR_ALLayout.z / max(TESR_ProjectionTransform[0][0], 1.0e-3f);
		float reach = clamp(NVR_ALEdges.z / max(texelWorld, 1.0e-4f), NVR_ALEdges.x, 192.0f);
		float spacing = reach / 3.0f;
		float3 sum = normal;
		[unroll] for (int k = -3; k <= 3; k++) {
			if (k == 0) continue;
			float dist = abs((float)k) * spacing;
			float2 tapUV = uv + axis * (k * spacing) * NVR_ALLayout.zw;
			// UNOFFICIAL 2026-10-27 RenderScale v2: "on screen" = inside the drawn part (TESR_RenderScale.xy; 1 off: as saturate)
			float onScreen = step(0.0f, tapUV.x) * step(tapUV.x, TESR_RenderScale.x) * step(0.0f, tapUV.y) * step(tapUV.y, TESR_RenderScale.y);
			tapUV = clamp(tapUV, 0.0f, TESR_RenderScale.xy);
			float tapZ; float3 tapNormal;
			if (alongY) { float4 t = tex2Dlod(NVR_ALBuffer, float4(tapUV, 0.0f, 0.0f)); tapNormal = t.xyz; tapZ = t.w >= 64000.0f ? 1.0e6f : t.w; }
			else { float4 t = tex2Dlod(NVR_ALPrep, float4(tapUV, 0.0f, 0.0f)); tapZ = t.x; tapNormal = OctDecode(Unpack2(t.y)); }
			float tolerance = max(viewZ * 0.001f * dist, 0.5f);	// a 30-degree bend at this distance stays within it; a rock's thickness does not
			float plane = abs(dot(PositionFromViewZ(tapUV, tapZ) - origin, normal));
			float weight = saturate(1.0f - plane / tolerance) * smoothstep(0.5f, 0.8f, dot(tapNormal, normal)) * onScreen;
			sum += tapNormal * weight;
		}
		normal = normalize(sum);
	}
	return alongY ? float4(viewZ, Pack2(OctEncode(normal)), 0.0f, 0.0f) : float4(normal, min(viewZ, 64000.0f));
}

// GTAO's cosine-weighted visible arc of one slice between its two horizons (cosines), each kept within the hemisphere round the
// normal.
float SliceArc(float horizonCos0, float horizonCos1, float n, float cosNormal, float sinN) {
	float h0 = n + clamp(-acos(clamp(horizonCos1, -1.0f, 1.0f)) - n, -1.5707963f, 1.5707963f);
	float h1 = n + clamp(acos(clamp(horizonCos0, -1.0f, 1.0f)) - n, -1.5707963f, 1.5707963f);
	return (cosNormal + 2.0f * h0 * sinN - cos(2.0f * h0 - n)) * 0.25f + (cosNormal + 2.0f * h1 * sinN - cos(2.0f * h1 - n)) * 0.25f;
}

// Pass 2: the occlusion and the bounce light (see the top of the file).
float4 Estimate(VSOUT IN) : COLOR0 {
	float2 uv = IN.UVCoord;
	float4 center = tex2Dlod(NVR_ALPrep, float4(uv, 0.0f, 0.0f));
	float3 origin = PositionFromViewZ(uv, center.x);
	float fade = smoothstep(NearFade, NearFade * 2.0f, origin.z) * (1.0f - smoothstep(DrawDistance * 0.7f, DrawDistance, origin.z));
	[branch] if (fade <= 0.0f) return float4(0.0f, 0.0f, 0.0f, 1.0f);

	float3 normal = OctDecode(Unpack2(center.y));
	float3 viewVec = normalize(-origin);
	float3 noise = tex2Dlod(TESR_BlueNoiseSampler, float4(uv / (256.0f * NVR_ALLayout.zw), 0.0f, 0.0f)).xyz;
	// UNOFFICIAL: with Temporal, a new pattern every frame (golden-ratio steps keep it blue noise), so the frames average out.
	noise.xy = frac(noise.xy + (NVR_ALTemporal.x > 0.0f ? NVR_ALTemporal.z : 0.0f) * float2(0.618034f, 0.754878f));
	// The radius on screen: r world units at depth z span r * P00 / (2 z) of the screen's width (P11 for the height).
	float2 screenRadius = Radius * float2(TESR_ProjectionTransform[0][0], TESR_ProjectionTransform[1][1]) / (2.0f * origin.z);
	screenRadius *= min(1.0f, MaxScreenRadius / screenRadius.x);
	float falloffStart = 0.4f * Radius;

	// Diagnostics (DebugView 3-6): the pixel's normal, its view depth / 2000, the reach on screen x 10, raw horizons.
	[branch] if (DebugView > 2.5f && DebugView < 3.5f) return float4(normal * 0.5f + 0.5f, 1.0f);
	[branch] if (DebugView > 3.5f && DebugView < 4.5f) return float4(origin.z / 2000.0f, frac(origin.z / 100.0f), 0.0f, 1.0f);
	[branch] if (DebugView > 4.5f && DebugView < 5.5f) return float4(screenRadius * 10.0f, Radius / 1000.0f, 1.0f);
	float visibility = 0.0f, weightSum = 0.0f;
	float3 bounce = 0.0f;
	float firstCos = 0.0f, firstLow = 0.0f;
	[unroll] for (int slice = 0; slice < SLICES; slice++) {
		float phi = (slice + noise.x) * (3.14159265f / SLICES);
		float2 omega = float2(cos(phi), -sin(phi));             // on screen (y down)
		float3 direction = float3(cos(phi), sin(phi), 0.0f);    // the same in view space (y up)
		float3 orthoDirection = direction - dot(direction, viewVec) * viewVec;
		float3 axis = normalize(cross(orthoDirection, viewVec));
		float3 projectedNormal = normal - axis * dot(normal, axis);
		float projectedLength = length(projectedNormal) + 1e-5f;
		float signNormal = dot(orthoDirection, projectedNormal) >= 0.0f ? 1.0f : -1.0f;
		float cosNormal = saturate(dot(projectedNormal, viewVec) / projectedLength);
		float n = signNormal * acos(cosNormal);
		// Start each side's horizon at the pixel's own tangent plane: nothing below it can hide anything.
		// UNOFFICIAL 2026-10-21 (alrocks) HorizonBias: a few degrees above it (NVR_ALEdges.y, radians; see Prepare), the same for the
		// open-ground normalisation below (weightSum), so open ground stays exactly 1.
		float lowCos0 = cos(n + 1.5707963f - NVR_ALEdges.y), lowCos1 = cos(n - 1.5707963f + NVR_ALEdges.y);
		float horizonCos0 = lowCos0, horizonCos1 = lowCos1;
		[unroll] for (int k = 0; k < STEPS; k++) {
			float s = (k + frac(noise.y + k * 0.618034f)) / STEPS;
			s *= s;                                              // more reads close to the pixel
			// UNOFFICIAL 2026-10-27 RenderScale v2: screenRadius is a share of the screen; in texture coordinates x TESR_RenderScale.xy,
			// and the march stops at the drawn part's edge (1 off: exactly as before).
			float2 offset = omega * screenRadius * max(s, 2.0f * NVR_ALLayout.z) * TESR_RenderScale.xy;
			// UNOFFICIAL 2026-11-01 MinReach ([Shaders.AmbientLight.Main]; user: a ring on the ground round the player with FSR + LowResEffects,
			// seen in DebugView 6, the raw estimate): max(s, 2 texels) above compared a share of the radius with a texel size, so it hardly
			// ever held; past some distance the nearest reads fell under half a texel, snapped back on to the pixel's own texel (a read at
			// distance 0, which counts as a horizon at 90 degrees from the view) and the ground darkened itself from that distance on. With
			// LowResEffects at RenderScale 0.33 the reads are three times shorter in texels, so that distance came three times closer: in
			// view as a ring. On: every read is at least two of this buffer's texels from the pixel.
			[flatten] if (NVR_ALReach.x > 0.5f) {
				float2 reach = screenRadius * s * TESR_RenderScale.xy;
				offset = omega * reach * max(1.0f, NVR_ALReach.y * NVR_ALLayout.z / max(reach.x, 1.0e-6f));
			}
			[unroll] for (int side = 0; side < 2; side++) {
				float2 sampleUV = uv + (side == 0 ? offset : -offset);
				float onScreen = step(0.0f, sampleUV.x) * step(sampleUV.x, TESR_RenderScale.x) * step(0.0f, sampleUV.y) * step(sampleUV.y, TESR_RenderScale.y);
				sampleUV = (floor(clamp(sampleUV, 0.0f, TESR_RenderScale.xy) / NVR_ALLayout.zw) + 0.5f) * NVR_ALLayout.zw;   // the centre of the texel read
				float4 texel = tex2Dlod(NVR_ALPrep, float4(sampleUV, 0.0f, 0.0f));
				float3 delta = PositionFromViewZ(sampleUV, texel.x) - origin;
				float dist = length(delta) + 1e-4f;
				float3 dir = delta / dist;
				float weight = saturate((Radius - dist) / (Radius - falloffStart)) * onScreen * step(NearFade, origin.z + delta.z);
				float lowCos = side == 0 ? lowCos0 : lowCos1;
				float sampleCos = lerp(lowCos, dot(dir, viewVec), weight);
				// UNOFFICIAL 2026-10-31 BackDepth: the horizon search takes the read as solid all the way back. Where the read's object has
				// a known back face and the pixel lies behind that, the object is a slab floating in front: it hides no more than its
				// back side does (the lower of the two horizons), so thin things in front leave no dark halo on what is behind them.
				float backZ = BackViewZ(sampleUV);
				[branch] if (backZ > texel.x && origin.z > backZ + 1.0f) {
					float3 backDelta = PositionFromViewZ(sampleUV, backZ) - origin;
					float backDist = length(backDelta) + 1e-4f;
					float backWeight = saturate((Radius - backDist) / (Radius - falloffStart)) * onScreen;
					sampleCos = min(sampleCos, lerp(lowCos, dot(backDelta / backDist, viewVec), backWeight));
				}
				// Bounce: what the read shows, as far as it rises above the pixel's own surface (within reach) and the two
				// surfaces face each other. UNOFFICIAL 2026-10-14: every read counts smoothly; it used to count only the part
				// of the view a read newly covered above the horizon so far, so on grass and rough ground a pixel's light came
				// from one or two reads picked by chance, and jumped from pixel to pixel and frame to frame (user: shimmer and
				// orange speckles).
				if (side == 0) horizonCos0 = max(horizonCos0, sampleCos); else horizonCos1 = max(horizonCos1, sampleCos);
				if (slice == 0 && side == 0 && k == 1) { firstCos = dot(dir, viewVec); firstLow = weight; }
			}
		}
		// The slice's visible arc, over what it would be with nothing in the way: one slice's arc is only right on average
		// over all directions (it exceeds 1 for a surface seen at an angle), so a few slices alone need this to keep open
		// ground at exactly 1.
		float sinN = sin(n);
		visibility += projectedLength * SliceArc(horizonCos0, horizonCos1, n, cosNormal, sinN);
		weightSum += projectedLength * SliceArc(lowCos0, lowCos1, n, cosNormal, sinN);
	}
	// Raised to a power (1.5 at strength 1, as XeGTAO does) so creases and contact read as the eye expects.
	// (UNOFFICIAL 2026-11-03: not with NVR_ALGI.x, whose DebugView 13 is the RT bounce light: Estimate's normal output then)
	[branch] if (DebugView > 5.5f && !(NVR_ALGI.x > 0.5f)) return float4(visibility / max(weightSum, 1e-4f), firstCos * 0.5f + 0.5f, firstLow, 1.0f);
	visibility = pow(saturate(visibility / max(weightSum, 1e-4f)), 1.5f * OcclusionStrength);
	visibility = lerp(1.0f, visibility, fade);
	// Bounce: the average over the reads, x BounceScale so a pixel enclosed by surfaces showing light L receives about L.
	// UNOFFICIAL 2026-11-03 RT GI on the D3D9 passes (NVR_ALGI.x, see the samplers): the traced bounce light instead, x the fade, as
	// AmbientLightEstimateRT.frag writes it (point read: each texel's own ray; the blurs and Accumulate average them).
	// (UNOFFICIAL 2026-11-08 alsky RtFarFade, NVR_ALReach.w: the trace stops at endFade, so the bounce fades out before it, not at DrawDistance)
	[branch] if (NVR_ALGI.x > 0.5f) bounce = tex2Dlod(NVR_ALRTGI, float4(uv, 0.0f, 0.0f)).rgb * fade *
		(NVR_ALReach.w > 0.5f ? 1.0f - smoothstep(0.75f * endFade, (float)endFade, origin.z) : 1.0f);
	else bounce *= BounceScale * fade / (SLICES * STEPS * 2);
	return float4(bounce, saturate(visibility));
}

// Passes 3-4: depth-aware blur, 7 taps along axis (half-resolution texels apart x BlurStep). UNOFFICIAL 2026-10-14: the bounce
// (rgb) blurs across a much looser depth step than the occlusion (a). Grass is a field of blades each at its own depth, so the
// tight step kept every grass pixel to itself and its grain was never smoothed; light that bounces is soft anyway, while the
// occlusion keeps its crisp edges.
float4 Blur(VSOUT IN, uniform float2 axis) : COLOR0 {
	float depth = readDepthLod(IN.UVCoord);
	float tolerance = max(depth * 0.03f, 2.0f);
	float looseTolerance = max(depth * 0.25f, 16.0f);
	static const float weights[4] = { 0.30f, 0.23f, 0.11f, 0.05f };
	float4 center = tex2Dlod(NVR_ALBuffer, float4(IN.UVCoord, 0.0f, 0.0f));
	[branch] if (DebugView > 2.5f && DebugView < 6.5f) return center;	// UNOFFICIAL 2026-10-21 (alrocks): the diagnostics 3-6 unblurred
	float3 bounce = center.rgb * weights[0];
	float visibility = center.a * weights[0];
	float2 total = weights[0];	// x occlusion, y bounce
	[unroll] for (int i = 1; i < 4; i++) {
		[unroll] for (int side = -1; side <= 1; side += 2) {
			float2 tapUV = IN.UVCoord + axis * NVR_ALLayout.zw * (i * side * BlurStep);
			tapUV = clampToRectIfOn(tapUV, NVR_ALLayout.zw);	// UNOFFICIAL 2026-10-27 RenderScale v2: no stale texels past the drawn part
			float dz = abs(readDepthLod(tapUV) - depth);
			float2 w = weights[i] * saturate(1.0f - dz / float2(tolerance, looseTolerance));
			float4 tap = tex2Dlod(NVR_ALBuffer, float4(tapUV, 0.0f, 0.0f));
			visibility += tap.a * w.x;
			bounce += tap.rgb * w.y;
			total += w;
		}
	}
	return float4(bounce / total.y, visibility / total.x);
}

// Pass 5 (UNOFFICIAL): this frame's blurred bounce (NVR_ALBuffer) blended into last frame's (NVR_ALHistory); see the top. The
// occlusion the same way into its own history (second target).
struct AccumulateOut { float4 bounce : COLOR0; float4 occlusion : COLOR1; };
AccumulateOut Accumulate(VSOUT IN) {
	AccumulateOut OUT;
	float2 uv = IN.UVCoord;
	float4 current = tex2Dlod(NVR_ALBuffer, float4(uv, 0.0f, 0.0f));
	float viewZ = tex2Dlod(NVR_ALPrep, float4(uv, 0.0f, 0.0f)).x;
	OUT.bounce = float4(current.rgb, viewZ);
	OUT.occlusion = float4(current.a, viewZ, 0.0f, 1.0f);
	[branch] if (!(NVR_ALTemporal.x > 0.0f) || viewZ < NearFade) return OUT;
	float3 origin = PositionFromViewZ(uv, viewZ);
	// UNOFFICIAL 2026-10-29 ObjectMotion: a surface that moved itself (a grabbed teapot, a walking NPC) was that far back along its path
	// (the world move, into view space)
	[branch] if (TESR_ObjectMotionData.x > 0.0f) origin -= mul(tex2Dlod(TESR_ObjectMotionBuffer, float4(uv, 0.0f, 0.0f)).xyz, (float3x3)TESR_ViewTransform);
	float4 clip = mul(float4(origin, 1.0f), NVR_ALReproject);
	float w = max(clip.w, 0.001f);
	float3 last = float3(clip.x / w * 0.5f + 0.5f, 0.5f - clip.y / w * 0.5f, clip.w);
	// Last frame's at that point: the four half-resolution pixels round it, bilinearly, each only if its depth matches.
	// UNOFFICIAL 2026-10-27 RenderScale v2: last.xy is a screen position; the history holds it at screenToRect (the edge test below stays on last.xy).
	// UNOFFICIAL 2026-11-01 TemporalJitterFix ([Shaders.AmbientLight.Main]; user: streaks on shrubs and ground while moving, FSR 0.33 +
	// LowResEffects at dawn): this frame's depth and last frame's history were each drawn shifted by their frame's TAA / FSR jitter
	// (up to a rendered pixel, three screen pixels at 0.33), and the reprojection took neither, so the history was read off by the
	// jitter's change every frame; moving, that smeared it into streaks. The history is read where that shift puts it (0 = as before).
	float2 lastUV = screenToRect(last.xy + float2(NVR_ALTemporal.w, NVR_ALEdges.w));
	float2 position = lastUV / NVR_ALLayout.zw - 0.5f;
	float2 base = floor(position);
	float2 f = position - base;
	// (2026-10-14: the occlusion's own history holds the view depth too; the bounce's history is gone with the screen bounce.)
	float2 t00 = tex2Dlod(NVR_ALOcclusionHistory, float4((base + 0.5f) * NVR_ALLayout.zw, 0.0f, 0.0f)).rg;
	float2 t10 = tex2Dlod(NVR_ALOcclusionHistory, float4((base + float2(1.5f, 0.5f)) * NVR_ALLayout.zw, 0.0f, 0.0f)).rg;
	float2 t01 = tex2Dlod(NVR_ALOcclusionHistory, float4((base + float2(0.5f, 1.5f)) * NVR_ALLayout.zw, 0.0f, 0.0f)).rg;
	float2 t11 = tex2Dlod(NVR_ALOcclusionHistory, float4((base + 1.5f) * NVR_ALLayout.zw, 0.0f, 0.0f)).rg;
	float4 weight = float4((1.0f - f.x) * (1.0f - f.y), f.x * (1.0f - f.y), (1.0f - f.x) * f.y, f.x * f.y);
	weight *= abs(float4(t00.g, t10.g, t01.g, t11.g) - last.z) < max(last.z * 0.05f, 4.0f);
	float found = dot(weight, 1.0f);
	float occlusionHistory = dot(float4(t00.r, t10.r, t01.r, t11.r), weight) / max(found, 1.0e-4f);
	// Held within what the bounce round the spot shows now (its four neighbours), a little wider: a blade that swayed away, or
	// light that changed, does not linger.
	float low = current.a, high = current.a;
	float3 lowC = current.rgb, highC = current.rgb;	// UNOFFICIAL 2026-11-03 (NVR_ALGI.y) the bounce's range the same way
	[unroll] for (int i = 0; i < 4; i++) {
		float2 offset = (i < 2 ? float2(i * 2 - 1, 0.0f) : float2(0.0f, i * 2 - 5)) * NVR_ALLayout.zw;
		// (UNOFFICIAL 2026-11-04 RT GI at RenderScale < 1, phase 2: the neighbours kept inside the rect: past it the buffer holds texels of
		// an older frame's layout, which would widen the bounce's range there; native frames unchanged)
		float4 n = tex2Dlod(NVR_ALBuffer, float4(clampToRectIfOn(uv + offset, NVR_ALLayout.zw), 0.0f, 0.0f));
		low = min(low, n.a);
		high = max(high, n.a);
		lowC = min(lowC, n.rgb);
		highC = max(highC, n.rgb);
	}
	float margin = (high - low) * 0.25f;
	occlusionHistory = clamp(occlusionHistory, low - margin, high + margin);
	float inside = all(last.xy > 0.0f) && all(last.xy < 1.0f) && last.z > NearFade;
	float moved = length((lastUV - uv) / NVR_ALLayout.zw);
	float keep = NVR_ALTemporal.x / (1.0f + moved / NVR_ALTemporal.y) * saturate(found * 2.0f) * inside;
	OUT.occlusion = float4(lerp(current.a, occlusionHistory, keep), viewZ, 0.0f, 1.0f);
	// UNOFFICIAL 2026-11-03 RT GI on the D3D9 passes (NVR_ALGI.y, see the samplers): the bounce light kept from frame to frame in the
	// bounce history (NVR_ALHistory: last frame's, rgb bounce, a view depth) as AmbientLightGIAccumulate.frag keeps it: the same four
	// taps and depth weights as the occlusion, held within this frame's neighbours' range (a quarter of it as margin, per channel), the
	// same share kept.
	[branch] if (NVR_ALGI.y > 0.5f) {
		float3 bounceHistory = (tex2Dlod(NVR_ALHistory, float4((base + 0.5f) * NVR_ALLayout.zw, 0.0f, 0.0f)).rgb * weight.x +
			tex2Dlod(NVR_ALHistory, float4((base + float2(1.5f, 0.5f)) * NVR_ALLayout.zw, 0.0f, 0.0f)).rgb * weight.y +
			tex2Dlod(NVR_ALHistory, float4((base + float2(0.5f, 1.5f)) * NVR_ALLayout.zw, 0.0f, 0.0f)).rgb * weight.z +
			tex2Dlod(NVR_ALHistory, float4((base + 1.5f) * NVR_ALLayout.zw, 0.0f, 0.0f)).rgb * weight.w) / max(found, 1.0e-4f);
		float3 marginC = (highC - lowC) * 0.25f;
		bounceHistory = clamp(bounceHistory, lowC - marginC, highC + marginC);
		// UNOFFICIAL 2026-11-11 (sungi) GISingleHistory (NVR_ALGI.w, STUDY F7): the RT denoiser's GI has its own history; keep 0 here
		OUT.bounce = float4(lerp(current.rgb, bounceHistory, NVR_ALGI.w > 0.5f ? 0.0f : keep), viewZ);
	}
	return OUT;
}

// ---------------------------------------------------------------------------------------------------------------------
// UNOFFICIAL 2026-10-14 (user: the screen-space bounce is noisy and comes and goes as the camera turns): the sun's bounce
// light from a 3D light grid round the player ([Shaders.AmbientLight.Main] GridBounce), after CryEngine 3's light propagation
// volumes (Kaplanyan 2009). It does not depend on what the screen shows, so it neither shimmers nor pops when turning.
//  - ShadowManager draws a small depth map of the world from the sun round the grid (NVR_GridSunDepth, every few frames
//    and whenever the grid moves): every surface the sun lights there, on screen or not.
//  - Pass GridInject: each grid cell (a texel of a 2D atlas: 32 x 32 cells per tile, 4 x 4 tiles for 16 layers) gathers
//    the sunlit surfaces in it from that map; each sends its light out round its normal (a cosine lobe, as order-1
//    spherical harmonics: one number for all directions and three for the direction; the light's colour is the sun's x
//    a ground colour, the same everywhere for now).
//  - Pass GridPropagate (GridSteps times): each cell takes the light its six neighbours send its way, so it spreads a cell
//    a step; the total over the steps is the bounce light (NVR_GridAccum).
//  - Apply: each pixel reads the grid where it is (between the eight nearest cell centres), turns it into the light
//    falling on its surface, and adds it as the screen bounce was added.
// The grid moves with the player in whole cells, so it does not crawl; it has no occlusion, so light can cross a thin wall.
float4 NVR_GridOrigin;          // xyz: the grid's corner relative to the camera now, w: cell size (world units)
// UNOFFICIAL 2026-10-22 (lightgrid2) fine level ([Shaders.AmbientLight.Main] GridFine): outdoors with the sun map, a second grid of
// 32 x 32 x 16 cells a quarter the size (50 units at the default 200: about 16 x 16 x 8 m round the camera), in the right half of a
// 256 x 128 atlas, made from the same sun map in the same passes. Near the camera the bounce takes it (light and shade the size of a
// door or a car instead of a room), fading to the coarse grid toward its edges. The sun map and its matrices stay in the coarse grid's
// space: a fine cell's corner there is NVR_GridOrigin1 - NVR_GridOrigin + cell x its size. Indoors and at night it is off (w 0).
float4 NVR_GridOrigin1;         // xyz: the fine grid's corner relative to the camera now, w: its cell size (0: no fine level)
float4 NVR_GridData;            // x: strength (0: no grid), y: share of a neighbour's light passed on per step, z: sun map texel size (world units), w: 1 the grid carries the surfaces' colours
float4 NVR_GridLight;           // rgb: the sun's colour (linear; the sun map's surfaces send their colour x it), w: injection scale
float4x4 NVR_GridSunToGrid;     // sun depth map clip space (x, y, depth) -> position relative to the grid's corner
float4x4 NVR_GridToSun;         // position relative to the grid's corner -> sun depth map clip space
sampler2D NVR_GridWave : register(s8) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D NVR_GridAccum : register(s9) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D NVR_GridSunDepth : register(s10) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
// UNOFFICIAL 2026-10-14 colour: the sunlit surfaces' colours (ShadowGridColor.pso), and per cell the colour carried with the light
// (rgb: colour x light, a: light; the colour is rgb / a), for each step and over all steps.
sampler2D NVR_GridSunColor : register(s12) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D NVR_GridWaveColor : register(s13) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D NVR_GridAccumColor : register(s14) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
// UNOFFICIAL 2026-10-14 blocking: per cell the surfaces in it (xyz: their average normal, w: how much of a cell face they cover),
// from the sun map at injection. (lightgrid2, 2026-10-22: xyz is now the cover per axis, see NVR_GridWalls.) Read through stage 5 during the grid passes (every sampler register is taken; NVR_ALPrep is not
// used by them), so it is NVR_ALPrep under another name.
#define NVR_GridGeometry NVR_ALPrep
float4 NVR_GridStep;            // x: 1 on the first spreading step (the running total starts at 0), y: blocking strength, z: 1 sun map in, w: 1 screen memory in
static const float3 GridGroundColor = float3(0.55f, 0.42f, 0.30f);	// without colours: a desert ground (linear)

static const float GridX = 32.0f, GridY = 32.0f, GridZ = 16.0f;
static const float AtlasSize = 128.0f;   // 4 x 4 tiles of 32 x 32 (one level)
static const float2 AtlasDims = float2(256.0f, 128.0f);	// (lightgrid2) two levels side by side: coarse left, fine right
static const float SunMapSize = 256.0f;

// Order-1 spherical harmonics: a clamped cosine lobe round n, and a function's value in direction d.
float4 CosineLobe(float3 n) { return float4(0.886227f, 1.023328f * n); }
float4 ShBasis(float3 d) { return float4(0.282095f, 0.488603f * d); }

// The atlas texel (centre, uv) of cell (x, y, z).
float2 CellUV(float3 c, float level = 0.0f) {
	float tile = c.z;
	float2 t = float2(fmod(tile, 4.0f), floor(tile / 4.0f));
	return (t * 32.0f + c.xy + 0.5f + float2(level * AtlasSize, 0.0f)) / AtlasDims;
}

// The cell an atlas texel holds.
float3 AtlasCell(float2 uv) {
	float2 texel = floor(uv * AtlasDims);
	texel.x -= texel.x >= AtlasSize ? AtlasSize : 0.0f;	// (lightgrid2) the fine level's half
	float2 t = floor(texel / 32.0f);
	return float3(texel - t * 32.0f, t.y * 4.0f + t.x);
}

// A point of the sun's depth map (uv, depth) relative to the grid's corner.
float3 SunMapPosition(float2 uv, float depth) {
	float4 p = mul(float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, depth, 1.0f), NVR_GridSunToGrid);
	return p.xyz / p.w;
}

struct GridOut { float4 wave : COLOR0; float4 accum : COLOR1; float4 waveColor : COLOR2; float4 accumColor : COLOR3; };

// UNOFFICIAL 2026-10-14 screen memory (indoors and at night, where there is no sun map): every grid update, each cell seen on
// screen takes the light its surfaces show there (lamps, neon, fire: whatever lit them), as order-1 harmonics round their
// normals and colour x light; a cell out of view, or hidden behind something, keeps what it had (so the bounce does not come
// and go as the camera turns). Stored per cell of the world: when the grid moves, the old values move with it (NVR_GridShift).
// Read through the stages of NVR_ALBuffer / NVR_ALHistory (every sampler register is taken; the grid passes use neither).
#define NVR_GridScreenOld NVR_ALBuffer
#define NVR_GridScreenColorOld NVR_ALHistory
float4 NVR_GridShift;           // xyz: the grid's move since the screen memory was stored (cells), w: 1 the memory is usable
float4 NVR_GridScreen;          // x: the screen memory's strength (GridScreenStrength)

// UNOFFICIAL 2026-10-22 (lightgrid2) wall memory ([Shaders.AmbientLight.Main] GridWallMemory): indoors and at night there is no sun map,
// so no cell had any surfaces and GridBlocking stopped nothing: the light of one room crossed the wall into the next. The screen memory
// now also keeps, per cell, how much of each axis's cell face its surfaces cover (x, y, z: |normal| per axis x the share of the face a
// hit stands for), from the same reads, remembered and moved with the grid as the light is. GridInject takes the larger of it and the
// sun map's cover. The blocking target is per axis now; for the sun map's surfaces it is cover x |average normal| per axis, which
// GridPropagate turns into exactly the stop it gave before (cover x |normal . axis|).
float4 NVR_GridWalls;           // x: 1 the screen memory keeps the walls, y: 1 GridInject reads them (wall memory usable this update), z: 1 the last update kept them
#define NVR_GridWallsOld NVR_GridWave            // ScreenInject reads the last update's wall memory through NVR_GridWave's stage (s8)
#define NVR_GridWallMemory NVR_GridWaveColor     // GridInject reads it through NVR_GridWaveColor's stage (s13; it reads no colour)

// UNOFFICIAL 2026-10-22 (lightgrid2) lamps ([Shaders.AmbientLight.Main] GridLamps): indoors and at night the grid's light came only from
// what the screen showed lit, which is little (measured: the grid's bounce added about 1/255 in the saloon and the Lucky 38, and the
// InteriorBalance cap was never reached). GridInject now also lights each cell's surfaces from the game's lamps round the grid (the
// nearest GridLampsMax, AmbientLight.cpp): the surfaces' cover facing the lamp (the walls above), the lamp's colour x dimmer (linear)
// x a game-like falloff over its radius, x a grey reflectance, sent back toward the lamp as the sun map's surfaces send theirs; light
// stopped on the way by the remembered walls (three reads between the lamp and the cell). Then it spreads as all the grid's light.
#define GridLampsMax 32
float4 NVR_GridLamps[GridLampsMax];      // xyz: the lamp relative to the camera, w: its radius (0: none)
float4 NVR_GridLampColors[GridLampsMax]; // rgb: colour x dimmer (linear)
float4 NVR_GridLampInfo;                 // x: lamps in the list, y: strength (GridLamps), z: blocking along the way (GridBlocking)
static const float GridLampReflectance = 0.4f;

struct ScreenOut { float4 sh : COLOR0; float4 colour : COLOR1; float4 walls : COLOR2; };

ScreenOut ScreenInject(VSOUT IN) {
	ScreenOut OUT;
	// (lightgrid2) the fine level keeps no screen memory (it is made from the sun map only).
	[branch] if (IN.UVCoord.x * AtlasDims.x >= AtlasSize) { OUT.sh = 0.0f; OUT.colour = 0.0f; OUT.walls = 0.0f; return OUT; }
	float3 cell = AtlasCell(IN.UVCoord);
	float size = NVR_GridOrigin.w;
	// What this cell held before (from where it was in the grid then).
	float3 oldCell = cell + NVR_GridShift.xyz;
	bool had = NVR_GridShift.w > 0.5f && all(oldCell >= 0.0f) && oldCell.x < GridX && oldCell.y < GridY && oldCell.z < GridZ;
	OUT.sh = had ? tex2Dlod(NVR_GridScreenOld, float4(CellUV(oldCell), 0.0f, 0.0f)) : 0.0f;
	OUT.colour = had ? tex2Dlod(NVR_GridScreenColorOld, float4(CellUV(oldCell), 0.0f, 0.0f)) : 0.0f;
	OUT.walls = had && NVR_GridWalls.z > 0.5f ? tex2Dlod(NVR_GridWallsOld, float4(CellUV(oldCell), 0.0f, 0.0f)) : 0.0f;
	// The cell on screen: its centre in view space (camera-relative world -> view: the view matrix's rotation alone).
	float3 centre = NVR_GridOrigin.xyz + (cell + 0.5f) * size;
	float3 view = mul(float4(centre, 0.0f), TESR_ViewTransform).xyz;
	[branch] if (view.z < 0.5f * size) return OUT;	// behind the camera, or round it (a cell's centre half a cell ahead still counts)
	float3 screen = projectPosition(view);
	// UNOFFICIAL 2026-10-27 RenderScale v2: projectPosition gives texture coordinates; the edge is the drawn part's, the extent
	// (a share of the screen) x TESR_RenderScale.xy.
	[branch] if (outsideRect(screen.xy)) return OUT;
	float2 extent = (0.6f * size) * float2(TESR_ProjectionTransform[0][0], TESR_ProjectionTransform[1][1]) / (2.0f * view.z) * TESR_RenderScale.xy;
	float3 low = centre - 0.5f * size;
	float4 sh = 0.0f, colour = 0.0f;
	float3 axes = 0.0f;	// (lightgrid2) the hits' |normal| per axis
	float hits = 0.0f, empty = 0.0f;
	[loop] for (int j = 0; j < 4; j++) {
		[loop] for (int i = 0; i < 4; i++) {
			float2 uv = screen.xy + ((float2(i, j) + 0.5f) / 4.0f * 2.0f - 1.0f) * extent;
			if (outsideRect(uv)) continue;	// (RenderScale v2)
			float depth = readDepthLod(uv);
			float3 p = toWorld(uv) * depth;
			float3 inCell = p - low;
			[branch] if (all(inCell >= 0.0f) && all(inCell < size)) {
				// A surface in the cell: the light it shows (linear, capped), round its normal.
				float3 light = min(pows(tex2Dlod(TESR_SourceBuffer, float4(uv, 0.0f, 0.0f)).rgb, 2.2f), 4.0f);
				float l = luma(light);
				float3 n = normalize(GetWorldNormalLod(uv));
				sh += CosineLobe(n) * l;
				colour += float4(light, l);
				axes += abs(n);
				hits += 1.0f;
			}
			else if (depth > view.z + size) empty += 1.0f;	// seen through: the cell is open air there
		}
	}
	[branch] if (hits + empty < 4.0f) return OUT;	// mostly hidden behind something nearer: keep what it had
	// Each hit stands for a sixteenth of about 1.2 x 1.2 cell faces of surface.
	float share = 1.44f / 16.0f * NVR_GridScreen.x;
	OUT.sh = lerp(OUT.sh, sh * share, 0.5f);
	OUT.colour = lerp(OUT.colour, colour * share, 0.5f);
	// (lightgrid2) the walls: the same share of a face per hit, without the light's strength; a cell seen open fades its walls out.
	OUT.walls = NVR_GridWalls.x > 0.5f ? lerp(OUT.walls, float4(axes * (1.44f / 16.0f), hits / 16.0f), 0.5f) : 0.0f;
	return OUT;
}

GridOut GridInject(VSOUT IN) {
	float3 cell = AtlasCell(IN.UVCoord);
	// (lightgrid2) the fine level: its own cell size, its corner in the coarse grid's space (where the sun map's positions are).
	bool fine = IN.UVCoord.x * AtlasDims.x >= AtlasSize;
	float size = fine ? NVR_GridOrigin1.w : NVR_GridOrigin.w;
	float3 low = (fine ? NVR_GridOrigin1.xyz - NVR_GridOrigin.xyz : 0.0f) + cell * size;
	float4 sh = 0.0f;
	float4 colour = 0.0f;	// the surfaces' colour x light (rgb) and light (a), added up
	float3 normals = 0.0f;	// their normals added up (blocking)
	float count = 0.0f;
	float faceShare = 0.0f;
	[branch] if (NVR_GridStep.z > 0.5f) {
		float4 centre = mul(float4(low + 0.5f * size, 1.0f), NVR_GridToSun);
		float2 centreUV = float2(centre.x * 0.5f + 0.5f, 0.5f - centre.y * 0.5f);
		// A cell seen from the sun covers up to about 1.75 cells across: 6 x 6 reads over that.
		float span = 1.75f * size / NVR_GridData.z;
		float stepTexels = span / 6.0f;
		[loop] for (int j = 0; j < 6; j++) {
			[loop] for (int i = 0; i < 6; i++) {
				float2 uv = centreUV + (float2(i, j) - 2.5f) * stepTexels / SunMapSize;
				float depth = tex2Dlod(NVR_GridSunDepth, float4(uv, 0.0f, 0.0f)).r;
				if (depth >= 0.9999f) continue;	// nothing drawn there
				float3 p = SunMapPosition(uv, depth);
				float3 inCell = p - low;
				if (any(inCell < 0.0f) || any(inCell >= size)) continue;
				// The surface's normal from its neighbours in the map (facing the sun).
				float2 px = 1.0f / SunMapSize;
				float3 px1 = SunMapPosition(uv + float2(px.x, 0.0f), tex2Dlod(NVR_GridSunDepth, float4(uv + float2(px.x, 0.0f), 0.0f, 0.0f)).r);
				float3 py1 = SunMapPosition(uv + float2(0.0f, px.y), tex2Dlod(NVR_GridSunDepth, float4(uv + float2(0.0f, px.y), 0.0f, 0.0f)).r);
				float3 n = cross(px1 - p, py1 - p);
				float len = length(n);
				n = len > 1e-4f ? n / len : float3(0.0f, 0.0f, 1.0f);
				n *= dot(n, TESR_SunDirection.xyz) < 0.0f ? -1.0f : 1.0f;
				// What it sends out: its colour (linear) x the sun's.
				float3 albedo = NVR_GridData.w > 0.5f ? pow(tex2Dlod(NVR_GridSunColor, float4(uv, 0.0f, 0.0f)).rgb, 2.2f) : GridGroundColor;
				float3 light = albedo * NVR_GridLight.rgb;
				float l = luma(light);
				sh += CosineLobe(n) * l;
				colour += float4(light, l);
				normals += n;
				count += 1.0f;
			}
		}
		// Each read stands for stepTexels^2 map texels of sunlit surface; as a share of a cell's face.
		faceShare = (stepTexels * NVR_GridData.z) * (stepTexels * NVR_GridData.z) / (size * size);
		sh *= NVR_GridLight.w * faceShare;
		colour *= faceShare;
	}
	// Indoors and at night: the screen memory (ScreenInject), already colour x light.
	[branch] if (NVR_GridStep.w > 0.5f) {
		sh += tex2Dlod(NVR_GridScreenOld, float4(IN.UVCoord, 0.0f, 0.0f));
		colour += tex2Dlod(NVR_GridScreenColorOld, float4(IN.UVCoord, 0.0f, 0.0f));
	}
	GridOut OUT;
	OUT.wave = sh;
	// The second target holds the cell's surfaces for blocking (not the running total: that starts at 0 on the first step,
	// NVR_GridStep.x; injection is kept out of it, else a surface's own cell lit the surface with what it sends out).
	// (lightgrid2) per axis: the cover x |average normal| (GridPropagate's stop is then the same as before), and with the wall memory
	// the larger of that and the cover the screen remembered.
	float normalLength = length(normals);
	float cover = count * faceShare;
	OUT.accum = float4(cover * abs(normalLength > 1e-4f ? normals / normalLength : float3(0.0f, 0.0f, 1.0f)), cover);
	[branch] if (NVR_GridWalls.y > 0.5f) OUT.accum = max(OUT.accum, tex2Dlod(NVR_GridWallMemory, float4(IN.UVCoord, 0.0f, 0.0f)));
	// (lightgrid2) the lamps (GridLamps, see NVR_GridLamps): only where the cell has surfaces to light.
	[branch] if (NVR_GridLampInfo.x > 0.5f && any(OUT.accum.xyz > 0.0f)) {
		float3 centre = low + 0.5f * size;
		[loop] for (int k = 0; k < GridLampsMax; k++) {
			if (k >= (int)NVR_GridLampInfo.x) break;
			float4 lamp = NVR_GridLamps[k];
			float3 v = (lamp.xyz - NVR_GridOrigin.xyz) - centre;	// cell centre -> lamp, in the grid's space
			float d = length(v);
			[branch] if (d >= lamp.w + 0.5f * size) continue;
			float3 dir = d > 1e-3f ? v / d : float3(0.0f, 0.0f, 1.0f);
			float fall = saturate(1.0f - max(d - 0.5f * size, 0.0f) / lamp.w);
			fall *= fall;
			// The walls between: three points along the way (the cell's own and the lamp's cells left out).
			float through = 1.0f;
			[branch] if (NVR_GridWalls.y > 0.5f && NVR_GridLampInfo.z > 0.0f && d > size) {
				[unroll] for (int s = 1; s <= 3; s++) {
					float3 c = floor((centre + v * (s / 4.0f)) / NVR_GridOrigin.w);	// (the wall memory is the coarse level's)
					if (any(c < 0.0f) || c.x >= GridX || c.y >= GridY || c.z >= GridZ) continue;
					float3 w = tex2Dlod(NVR_GridWallMemory, float4(CellUV(c), 0.0f, 0.0f)).xyz;
					through *= 1.0f - saturate(dot(w, abs(dir)) * NVR_GridLampInfo.z);
				}
			}
			float facing = min(dot(OUT.accum.xyz, abs(dir)), 1.0f);
			float3 light = NVR_GridLampColors[k].rgb * (fall * through * facing * GridLampReflectance * NVR_GridLampInfo.y);
			float l = luma(light);
			sh += CosineLobe(dir) * l;
			colour += float4(light, l);
		}
		OUT.wave = sh;
		OUT.waveColor = colour;
	}
	OUT.waveColor = colour;
	OUT.accumColor = 0.0f;
	return OUT;
}

GridOut GridPropagate(VSOUT IN) {
	float3 cell = AtlasCell(IN.UVCoord);
	float level = IN.UVCoord.x * AtlasDims.x >= AtlasSize ? 1.0f : 0.0f;	// (lightgrid2) light spreads within its own level
	float4 sh = 0.0f;
	float4 colour = 0.0f;
	float4 blocker = tex2Dlod(NVR_GridGeometry, float4(IN.UVCoord, 0.0f, 0.0f));
	[unroll] for (int k = 0; k < 6; k++) {
		float3 axis = k == 0 ? float3(1, 0, 0) : k == 1 ? float3(-1, 0, 0) : k == 2 ? float3(0, 1, 0) : k == 3 ? float3(0, -1, 0) : k == 4 ? float3(0, 0, 1) : float3(0, 0, -1);
		float3 neighbour = cell - axis;	// the neighbour on the far side; its light travels along +axis to this cell
		if (any(neighbour < 0.0f) || neighbour.x >= GridX || neighbour.y >= GridY || neighbour.z >= GridZ) continue;
		float4 from = tex2Dlod(NVR_GridWave, float4(CellUV(neighbour, level), 0.0f, 0.0f));
		float sent = max(dot(from, ShBasis(axis)), 0.0f);
		// Blocking: the surfaces in this cell stop the light crossing them, as far as they face across its path (a floor does
		// not stop light running along it).
		sent *= 1.0f - saturate(dot(blocker.xyz, abs(axis)) * NVR_GridStep.y);	// (lightgrid2) per-axis cover (was cover x |normal . axis|, the same for the sun map)
		sh += CosineLobe(axis) * (sent * NVR_GridData.y);
		// The neighbour's colour travels with the light it sends.
		float4 fromColour = tex2Dlod(NVR_GridWaveColor, float4(CellUV(neighbour, level), 0.0f, 0.0f));
		colour += float4(fromColour.rgb / max(fromColour.a, 1e-5f), 1.0f) * (sent * NVR_GridData.y);
	}
	GridOut OUT;
	OUT.wave = sh;
	OUT.accum = (NVR_GridStep.x > 0.5f ? 0.0f : tex2Dlod(NVR_GridAccum, float4(IN.UVCoord, 0.0f, 0.0f))) + sh;	// the total so far (read at the texel centre) plus this step
	OUT.waveColor = colour;
	OUT.accumColor = tex2Dlod(NVR_GridAccumColor, float4(IN.UVCoord, 0.0f, 0.0f)) + colour;
	return OUT;
}

// (lightgrid2) one level's harmonics at a surface (between the eight nearest cell centres), its colour x light and light, and how far
// inside the level the surface is (fade: 1 well inside, 0 outside; over edgeCells cells).
float4 GridLevel(float3 cameraRelative, float3 normal, float4 origin, float level, float edgeCells, out float4 colourLight, out float fade) {
	float size = origin.w;
	// Half a cell off the surface along its normal: a surface's own cell holds the light it sends out, not what it receives.
	float3 g = (cameraRelative - origin.xyz + normal * (0.5f * size)) / size - 0.5f;
	float3 dims = float3(GridX, GridY, GridZ);
	float3 edge = min(g + 0.5f, dims - 0.5f - g);
	fade = saturate(min(min(edge.x, edge.y), edge.z) / edgeCells);
	colourLight = 0.0f;
	[branch] if (fade <= 0.0f) return 0.0f;
	g = clamp(g, 0.0f, dims - 1.0f);
	float z0 = floor(g.z);
	float z1 = min(z0 + 1.0f, GridZ - 1.0f);
	float fz = g.z - z0;
	// Within a layer the atlas's own bilinear filter (kept inside the tile: g.xy is clamped to the cell centres).
	float4 a = tex2Dlod(NVR_GridAccum, float4(CellUV(float3(g.xy, z0), level), 0.0f, 0.0f));
	float4 b = tex2Dlod(NVR_GridAccum, float4(CellUV(float3(g.xy, z1), level), 0.0f, 0.0f));
	[branch] if (NVR_GridData.w > 0.5f) {
		float4 ca = tex2Dlod(NVR_GridAccumColor, float4(CellUV(float3(g.xy, z0), level), 0.0f, 0.0f));
		float4 cb = tex2Dlod(NVR_GridAccumColor, float4(CellUV(float3(g.xy, z1), level), 0.0f, 0.0f));
		colourLight = lerp(ca, cb, fz);
	}
	return lerp(a, b, fz);
}

// The grid's bounce light falling on a surface at a camera-relative position with a world normal (0 outside the grid).
// (lightgrid2) Near the camera the fine level takes over (GridFine), fading to the coarse one over its outer four cells.
float3 GridBounce(float3 cameraRelative, float3 normal) {
	float4 c, cFine;
	float fade, fadeFine;
	float4 sh = GridLevel(cameraRelative, normal, NVR_GridOrigin, 0.0f, 2.0f, c, fade);
	[branch] if (NVR_GridOrigin1.w > 0.0f) {
		float4 shFine = GridLevel(cameraRelative, normal, NVR_GridOrigin1, 1.0f, 4.0f, cFine, fadeFine);
		sh = lerp(sh, shFine, fadeFine);
		c = lerp(c, cFine, fadeFine);
		fade = max(fade, fadeFine);
	}
	[branch] if (fade <= 0.0f) return 0.0f;
	// The colour of the light arriving (colour x light over light: its brightness is in the harmonics), white without colours.
	float3 colour = 1.0f;
	[branch] if (NVR_GridData.w > 0.5f) {
		// UNOFFICIAL 2026-10-18 (ambfix): only GridColour of it (0.25). Until today the colour never arrived (the alphas of targets 1-3 were
		// dropped, see AmbientLight.cpp RenderGrid), so the bounce was white; at full strength it takes the colour of the brightest
		// emitters in reach (Gomorrah: the flames, rgb/luma 2.32 0.70 0.04) and fire-lit rooms turned orange (Lab: bright areas |dab|
		// ~11 vs 2.8 for the old picture); a quarter keeps a warm room warm without painting it.
		colour = c.a > 1e-5f ? lerp(1.0f, c.rgb / c.a, NVR_ALScene.w) : 1.0f;
	}
	// The light arriving over the hemisphere round the normal; / pi so that a surface ringed by the bounce of ground lit with
	// colour C gets about C x the share of the view the ground fills (test 1: x 1 washed the picture out).
	// The harmonics hold the way the light travels; a surface receives what travels toward it, against its normal (2026-10-14: it
	// read the light leaving it, so the ground lit itself and indoors almost nothing arrived).
	// x 1.5 (was 1 / pi): the spreading loses most of the light on the way (each step passes on GridSpread of it); at 1.5 a rock face
	// beside sunlit ground gets about the share of the ground's light a real one does (half its view is ground).
	float irradiance = max(dot(sh, CosineLobe(-normal)), 0.0f) * 1.5f;
	return irradiance * colour * fade;
}

// UNOFFICIAL 2026-10-18 (ambfix; user: interiors washed purple/blue and orange): the surface's own colour, as far as one picture can
// tell it (there is no albedo buffer): the pixel's light over the light the scene gives every surface (NVR_ALScene: its ambient). A
// surface only the ambient lights comes out as its own colour, without the ambient's tint (the Tops: a purple-blue ambient, XCLL
// 57 55 85); one brighter than the ambient can explain (a lamp or the sun on it) is capped at a bright surface's reflectance and its
// hue counts less (it is the lamp's as much as the surface's). The old stand-in (the pixel's hue scaled to 0.6 at its brightest
// channel) made every surface, dark or light, send back 0.6 of the bounce in the colour of the light already on it: dark corners lit
// by a tinted ambient came out as bright pastel copies of that tint (measured in game, Gomorrah at GridBounce 2: the picture's
// median brightness x 2.2-3.2 and its local contrast x 0.74 (Lab); with this x 1.2-1.4 and x 1.06).
static const float MaxReflectance = 0.6f;	// the old stand-in's level; 92 % of Gomorrah's pixels come out below it
float3 SurfaceColourCap(float3 lin, float cap) {
	float3 ambient = max(NVR_ALScene.rgb, 0.25f * luma(NVR_ALScene.rgb) + 1.0e-4f);
	float3 raw = max(lin, 0.0f) / ambient;
	float level = luma(raw);
	float3 hue = raw / max(level, 1.0e-4f);
	float trust = saturate(cap / max(level, 1.0e-4f));
	return min(level, cap) * min(lerp(1.0f, hue, trust), 3.0f);
}
float3 SurfaceColour(float3 lin) { return SurfaceColourCap(lin, MaxReflectance); }
// UNOFFICIAL 2026-11-10 AlbedoFallbackCap (NVR_ALReach.z; testers: "RT GI / ambient light only affect the grass now", "interiors still
// discoloured with AlbedoBuffer on"): with the albedo buffer live, a pixel no template wrote (grass, and whatever else draws with a shader
// that is not ours: 80 % of one tester's 4K outdoor frame) still took the guess above, capped at 0.6, next to written surfaces whose real
// base colour is 0.05-0.15: the bounce was 5-10 times stronger there, in the light's own tint. The guess for those pixels is now capped at
// the level real surfaces have (the cap also fades the guessed hue: a lit pixel's hue is the lamp's as much as the surface's). 0 = the old
// 0.6. Without the buffer (AlbedoBuffer off) nothing changes.
float FallbackCap() { return NVR_ALFlags.z > 0.5f && NVR_ALReach.z > 0.0f ? NVR_ALReach.z : MaxReflectance; }
// UNOFFICIAL 2026-10-31 AlbedoBuffer: the surface's real linear base colour where the templates wrote it (NVR_ALAlbedo, a 1), SurfaceColour's guess
// elsewhere. Branch-free on purpose: Apply already uses every one of ps_3_0's 16 boolean registers for its constant branches (one more [branch]
// on NVR_ALFlags.z failed the whole effect with X4550 in the first bench run), so the read is unconditional (s12 is bound or black) and a lerp picks.
float3 SurfaceAlbedo(float2 uv, float3 lin) {
	float4 a = tex2Dlod(NVR_ALAlbedo, float4(uv, 0.0f, 0.0f));
	float take = step(0.5f, NVR_ALFlags.z) * step(0.5f, a.a);
	return lerp(SurfaceColourCap(lin, FallbackCap()), max(a.rgb, 0.0f), take);
}

// UNOFFICIAL 2026-10-18 (ambfix2, user: "indoors it's too powerful ... it diverges from the original mood of each vanilla room"):
// indoors the room keeps its own brightness. The game's interior ambient already stands in for all the light bouncing round the
// room, so the grid's bounce on top lifted every room (Gomorrah at GridBounce 2: median x1.3, 40 of 144 screen tiles brighter
// than any lit surface round them). Indoors (NVR_ALInterior.x) the occlusion now darkens only the ambient share of a pixel (a lamp's
// light is not occluded by the corner it falls into), and the bounce is scaled so that it gives back at most InteriorBounceShare x
// the light the occlusion took over the whole picture: light moves from creases to the surfaces next to lit ones, the room's
// average stays. The two totals come from 32 x 18 samples of the same terms (StatsSample: 16 x 9 texels of 2 x 2 samples each)
// averaged into one texel (StatsReduce), eased over ~10 frames (fires flicker). Outdoors none of it runs.
#define NVR_ALStats NVR_ALBuffer        // StatsReduce reads the samples through NVR_ALBuffer's stage (s6)
#define NVR_ALBalance NVR_ALHistory     // StatsReduce reads last frames' balance, Apply this frame's, through NVR_ALHistory's stage (s7)
static const float2 StatsSize = float2(16.0f, 9.0f);	// texels of StatsSample's target (NVR_AmbientLightStats), 2 x 2 samples each

// One sample: the picture's light, the light the occlusion takes from its ambient share, the bounce (luma), as Apply works them out.
float3 StatsAt(float2 uv) {
	float3 lin = linearize(tex2Dlod(TESR_SourceBuffer, float4(uv, 0.0f, 0.0f)).rgb);
	float depth = readDepthLod(uv);
	[branch] if (depth < NearFade || depth > DrawDistance || IsViewModel(uv)) return float3(luma(lin), 0.0f, 0.0f);
	float ao = lerp(DarkestAO, 1.0f, tex2Dlod(NVR_ALOcclusionHistory, float4(uv, 0.0f, 0.0f)).r);
	ao = lerp(ao, 1.0f, saturate((luma(lin) - BrightFade) * 3.0f));
	float3 albedo = SurfaceAlbedo(uv, lin);	// UNOFFICIAL 2026-10-31 AlbedoBuffer (the buffer where written, SurfaceColour's guess elsewhere)
	float3 normal = normalize(GetWorldNormalLod(uv));
	float3 bounce = GridBounce(toWorld(uv) * depth, normal) * (NVR_GridData.x * ao) * albedo;
	float3 taken = (1.0f - ao) * min(lin, albedo * NVR_ALScene.rgb);
	return float3(luma(lin), luma(taken), luma(bounce));
}

// Pass 6 (indoors): each texel, the sum of its 2 x 2 samples (a 32 x 18 grid over the picture).
float4 StatsSample(VSOUT IN) : COLOR0 {
	float2 cell = floor(IN.UVCoord * StatsSize);
	float3 sum = 0.0f;
	[unroll] for (int j = 0; j < 2; j++) {
		// UNOFFICIAL 2026-10-27 RenderScale v2: the stats target stays whole (AmbientLight.cpp); its grid spans the drawn part.
		[unroll] for (int i = 0; i < 2; i++) sum += StatsAt(screenToRect((cell * 2.0f + float2(i, j) + 0.5f) / (StatsSize * 2.0f)));
	}
	return float4(sum, 1.0f);
}

// Pass 7 (indoors): the totals, the bounce's scale (at most 1: never more than GridBounce asks for), eased with last frames'.
float4 StatsReduce(VSOUT IN) : COLOR0 {
	float3 total = 0.0f;
	[loop] for (int j = 0; j < 9; j++) {
		[loop] for (int i = 0; i < 16; i++) total += tex2Dlod(NVR_ALStats, float4((float2(i, j) + 0.5f) / StatsSize, 0.0f, 0.0f)).xyz;
	}
	float scale = saturate(NVR_ALInterior.z * total.y / max(total.z, 1.0e-6f));
	float last = tex2Dlod(NVR_ALBalance, float4(0.5f, 0.5f, 0.0f, 0.0f)).x;
	scale = NVR_ALInterior.y > 0.5f ? lerp(scale, last, NVR_ALInterior.w) : scale;
	return float4(scale, total.yzx / (StatsSize.x * StatsSize.y * 4.0f));
}

// UNOFFICIAL 2026-10-20 (skyamb) Skylighting: the sky target (AmbientOcclusion's, bound by hand on stage 10 for Apply in technique AmbientLightSky;
// r the factor for the sky's share, g its depth, b the visibility) at a full-resolution pixel, upsampled as AmbientOcclusion.fx
// DedicatedCombine does it: the four nearest half-resolution texels, bilinear, each counted less across a depth step of
// BlurDropThreshold. (factor, visibility); (1, 1) beyond endFade.
sampler2D NVR_ALSkyVis : register(s10) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
float2 SkyFactorAt(float2 uv, float depth) {
	[branch] if (!(depth < endFade)) return 1.0f;
	float2 position = uv / NVR_ALLayout.zw - 0.5f;
	float2 base = floor(position);
	float2 fraction = position - base;
	float2 sum = 0.0f;
	float weights = 0.0f;
	[unroll] for (int y = 0; y < 2; y++) {
		[unroll] for (int x = 0; x < 2; x++) {
			float2 tapUV = clamp((base + float2(x, y) + 0.5f) * NVR_ALLayout.zw, 0.5f * NVR_ALLayout.zw, TESR_RenderScale.xy - 0.5f * NVR_ALLayout.zw);	// (RenderScale v2: the drawn part)
			float3 tap = tex2Dlod(NVR_ALSkyVis, float4(tapUV, 0.0f, 0.0f)).rgb;
			float weight = (x ? fraction.x : 1.0f - fraction.x) * (y ? fraction.y : 1.0f - fraction.y);
			weight /= 1.0f + abs(tap.y - depth) / max(TESR_AmbientOcclusionData.z, 0.001f);
			sum += tap.xz * weight;
			weights += weight;
		}
	}
	return saturate(sum / max(weights, 1.0e-6f));
}

// Pass 8: upsample and apply. UNOFFICIAL 2026-10-20 (skyamb) sky (technique AmbientLightSky): Skylighting's factor too, as the AO's combine
// applies it: on every pixel up to endFade, the ones this effect leaves alone included (near, beyond DrawDistance, the weapon), times
// the occlusion; the bounce light is not sky light and keeps its strength. The debug views stay this effect's own.
float4 Apply(VSOUT IN, uniform bool sky) : COLOR0 {
	float4 color = tex2D(TESR_SourceBuffer, IN.UVCoord);
	[branch] if (NVR_ALFlags.x > 0.5f) color = float4(SunShadowApply(color.rgb, IN.UVCoord), 1.0f);
	float depth = readDepth(IN.UVCoord);
	float2 skyFactor = 1.0f;	// (the factor, the visibility; Includes/SkyTake.hlsl. 'if (sky)': compiled out of the technique without the sky)
	if (sky) skyFactor = SkyFactorAt(IN.UVCoord, depth);
	[branch] if (depth < NearFade || depth > DrawDistance || IsViewModel(IN.UVCoord)) {
		if (sky) color.rgb = pows(SkyTake(pows(color.rgb, 2.2f), skyFactor.x, skyFactor.y, TESR_AmbientOcclusionSky5.x, TESR_AmbientOcclusionSky5.w), 1.0f / 2.2f);	// (on gamma 2.2 linear, as the AO's combine)
		return DebugView > 0.5f ? float4(0.0f, 0.0f, 0.0f, 1.0f) : color;
	}

	// The four nearest half-resolution texels, the ones at another depth (across an object's edge) counted less.
	float2 position = IN.UVCoord / NVR_ALLayout.zw - 0.5f;
	float2 base = floor(position);
	float2 fraction = position - base;
	float tolerance = max(depth * 0.03f, 2.0f);
	float4 sum = 0.0f;
	float weights = 1.0e-5f;
	// UNOFFICIAL 2026-11-01 EdgeReject ([Shaders.AmbientLight.Main], NVR_ALFlags.w; user: blocks on grass and a dark wedge beside it with
	// FSR at RenderScale 0.33 + LowResEffects, where a half-resolution texel is 6 screen pixels): a tap at another depth only counted
	// less (1 / (1 + d / tolerance)), never nothing, so across a grass blade's edge the ground's occlusion went into the grass and back.
	// On: a tap past the tolerance counts nothing (saturate(1 - d / tolerance)), and where all four do, the nearest in depth is taken.
	const float edgeReject = NVR_ALFlags.w > 0.5f ? 1.0f : 0.0f;
	float nearestCloseness = 1.0e9f, nearestValue = 1.0f;
	// UNOFFICIAL 2026-11-03 RT GI on the D3D9 passes (see the samplers). Tested per pixel (x depth, > 0 here) on purpose: as a plain
	// constant test the compiler gives each branch on it a boolean register, and technique AmbientLightSky's Apply ran out of ps_3_0's 16.
	const bool giApply = NVR_ALGI.z * depth > 0.5f;
	float3 giSum = 0.0f, nearestGI = 0.0f;
	[unroll] for (int y = 0; y < 2; y++) {
		[unroll] for (int x = 0; x < 2; x++) {
			float2 tapUV = clampToRectIfOn((base + float2(x, y) + 0.5f) * NVR_ALLayout.zw, NVR_ALLayout.zw);	// (RenderScale v2: no stale texels)
			float tapDepth = readDepthLod(tapUV);
			float weight = (x ? fraction.x : 1 - fraction.x) * (y ? fraction.y : 1 - fraction.y);
			float closeness = abs(tapDepth - depth) / tolerance;
			weight *= lerp(1.0f / (1.0f + closeness), saturate(1.0f - closeness), edgeReject);
			// UNOFFICIAL: the bounce from the accumulated buffer (Accumulate), the occlusion from the blurred one.
			float value = tex2Dlod(NVR_ALOcclusionHistory, float4(tapUV, 0.0f, 0.0f)).r;
			sum.a += value * weight;	// (the occlusion only: no screen bounce)
			weights += weight;
			// (NVR_ALGI.z) the accumulated bounce light through the same taps and weights, as AmbientLightApply-gi does
			[branch] if (giApply) {
				float3 gi = tex2Dlod(NVR_ALGIHistory, float4(tapUV, 0.0f, 0.0f)).rgb;
				giSum += gi * weight;
				nearestGI = closeness < nearestCloseness ? gi : nearestGI;
			}
			nearestValue = closeness < nearestCloseness ? value : nearestValue;
			nearestCloseness = min(closeness, nearestCloseness);
		}
	}
	float4 ambient = sum / weights;
	ambient.a = edgeReject > 0.5f && weights < 0.01f ? nearestValue : ambient.a;	// (EdgeReject: no tap at this depth, the nearest)

	float3 lin = linearize(color.rgb);
	float ao = lerp(DarkestAO, 1.0f, ambient.a);
	ao = lerp(ao, 1.0f, saturate((luma(lin) - BrightFade) * 3.0f));   // bright surfaces keep their light, as the old AO
	// UNOFFICIAL 2026-10-14 (user: the occlusion still shimmers on grass): where the depth is busy (grass, bushes: neighbouring
	// pixels on different blades, a bend in the depth no flat surface has, even seen edge on), the occlusion is mostly chance
	// (which blade covers the pixel this frame, swaying), and the blur and the frame history cannot settle it (they keep
	// pixels at other depths apart). There it keeps only a quarter of its darkening; solid ground and walls keep all of it.
	float2 px = TESR_ReciprocalResolution.xy;
	float left = readDepthLod(IN.UVCoord - float2(px.x, 0.0f)), right = readDepthLod(IN.UVCoord + float2(px.x, 0.0f));
	float up = readDepthLod(IN.UVCoord - float2(0.0f, px.y)), down = readDepthLod(IN.UVCoord + float2(0.0f, px.y));
	float bendX = abs(left + right - 2.0f * depth);
	float bendY = abs(up + down - 2.0f * depth);
	// UNOFFICIAL 2026-10-21 (aloutline, user: "this outline that occurs in some areas w/ ambient light", a dim saloon with a seated NPC:
	// a thin light line round every character, table, booth and lamp): the rule above took any bend in the depth for busy depth, and
	// the edge of every object is one (a single step bends the depth at the two pixels either side of it), so a line two pixels wide
	// round everything kept only a quarter of its darkening; in a dim room, where the occlusion takes much of the light, it showed
	// as a light outline (measured: edge pixels 1.42 x the light round them, 1.09 with AmbientLight off). With NoEdgeOutline
	// (NVR_ALFlags.y) the depth must bend at three pixels in a row (the bends one pixel either side as well, the smallest of the
	// three counts): a lone edge has two, grass and bushes (blades a pixel or two wide, each at its own depth) keep theirs. Four
	// more depth reads.
	[branch] if (NVR_ALFlags.y > 0.5f) {
		float left2 = readDepthLod(IN.UVCoord - float2(2.0f * px.x, 0.0f)), right2 = readDepthLod(IN.UVCoord + float2(2.0f * px.x, 0.0f));
		float up2 = readDepthLod(IN.UVCoord - float2(0.0f, 2.0f * px.y)), down2 = readDepthLod(IN.UVCoord + float2(0.0f, 2.0f * px.y));
		bendX = min(bendX, min(abs(left2 + depth - 2.0f * left), abs(depth + right2 - 2.0f * right)));
		bendY = min(bendY, min(abs(up2 + depth - 2.0f * up), abs(depth + down2 - 2.0f * down)));
	}
	float busy = saturate((bendX + bendY) / depth * 20.0f - 0.1f);
	// UNOFFICIAL 2026-11-03 RT GI on the D3D9 passes (NVR_ALGI.z): the bounce light arriving (EdgeReject: the nearest tap's where no tap is at
	// this depth, as the occlusion), x GIStrength; DebugView 13 shows it alone (before the surface colour), as AmbientLightApply-gi (in
	// view 12's branch below: one more constant branch here would run out of the 16 boolean registers).
	float3 giLight = 0.0f;
	[branch] if (giApply) giLight = (edgeReject > 0.5f && weights < 0.01f ? nearestGI : giSum / weights) * BounceStrength;
	// 12 (lightgrid2, diagnostic): the walls the grid knows in the pixel's own cell (r, g, b: cover across x, y, z; the wall memory merged
	// with the sun map's, as GridPropagate reads it), NVR_GridWave's stage bound to the blocking buffer for it.
	[branch] if (DebugView > 11.5f) {
		float3 c = floor((toWorld(IN.UVCoord) * depth - NVR_GridOrigin.xyz) / NVR_GridOrigin.w);
		float4 m = tex2Dlod(NVR_GridWave, float4(CellUV(clamp(c, 0.0f, float3(GridX, GridY, GridZ) - 1.0f)), 0.0f, 0.0f));
		return giApply && DebugView > 12.5f ? delinearize(float4(giLight, 1.0f)) : float4(saturate(m.xyz * NVR_GridStep.y), 1.0f);	// (13 with NVR_ALGI.z: the RT bounce light, above)
	}
	[branch] if (DebugView > 10.5f) return float4(busy.xxx, 1.0f);	// 11 (diagnostic): where the busy-depth rule lifts the occlusion
	ao = lerp(ao, lerp(1.0f, ao, 0.25f), busy);
	float3 albedo = SurfaceAlbedo(IN.UVCoord, lin);                      // UNOFFICIAL 2026-10-18 (ambfix): was lin / max channel * 0.6; 2026-10-31 AlbedoBuffer: the buffer where written
	// UNOFFICIAL 2026-10-14: the light grid's bounce (GridBounce), darkened by the occlusion as light from round about is.
	float3 gridBounce = 0.0f;
	[branch] if (NVR_GridData.x > 0.0f) {
		float3 cameraRelative = toWorld(IN.UVCoord) * depth;
		float3 normal = normalize(GetWorldNormalLod(IN.UVCoord));
		gridBounce = GridBounce(cameraRelative, normal) * (NVR_GridData.x * ao);
	}
	// UNOFFICIAL 2026-10-18 (ambfix) diagnostics: 9 the grid's light on the surface (strength and occlusion included, before the
	// surface colour), 10 the surface colour the apply multiplies it by; both linear, display-encoded.
	[branch] if (DebugView > 8.5f) return delinearize(float4(DebugView < 9.5f ? gridBounce : albedo, 1.0f));
	// UNOFFICIAL 2026-10-18 (ambfix2): indoors, the bounce at the scale StatsReduce found (see above StatsSample).
	[branch] if (NVR_ALInterior.x > 0.5f) gridBounce *= tex2Dlod(NVR_ALBalance, float4(0.5f, 0.5f, 0.0f, 0.0f)).x;
	// 8 (diagnostic): the screen memory in the pixel's own cell, unspread (r: its light x 4, g: cell seen at all, b: grid colour
	// light x 4), NVR_GridWave's stage bound to the memory for it.
	[branch] if (DebugView > 7.5f) {
		float3 cameraRelative = toWorld(IN.UVCoord) * depth;
		float3 c = floor((cameraRelative - NVR_GridOrigin.xyz) / NVR_GridOrigin.w);
		float4 m = tex2Dlod(NVR_GridWave, float4(CellUV(clamp(c, 0.0f, float3(GridX, GridY, GridZ) - 1.0f)), 0.0f, 0.0f));
		return float4(saturate(m.x * 4.0f), m.x != 0.0f ? 0.5f : 0.0f, saturate(gridBounce.b * 4.0f), 1.0f);
	}
	[branch] if (DebugView > 6.5f) return delinearize(float4(gridBounce * albedo, 1.0f));	// 7: the grid's bounce alone
	// UNOFFICIAL 2026-10-21 (alrocks): diagnostics 3-6 (the estimate's raw values: normal, depth, reach, horizons) read from the estimate's
	// buffer (NVR_ALBuffer is bufferTexture[0] here); since the screen bounce went they came through the upsample's rgb, which is 0.
	[branch] if (DebugView > 2.5f) return float4(tex2Dlod(NVR_ALBuffer, float4(IN.UVCoord, 0.0f, 0.0f)).rgb, 1.0f);
	[branch] if (DebugView > 0.5f) return delinearize(float4(DebugView < 1.5f ? ao.xxx : gridBounce * albedo, 1.0f));	// 2: the bounce (the light grid's)
	// UNOFFICIAL 2026-10-18 (ambfix2): indoors the occlusion takes only from the ambient share (see above StatsSample).
	[branch] if (NVR_ALInterior.x > 0.5f) lin = lin - (1.0f - ao) * min(lin, albedo * NVR_ALScene.rgb) + gridBounce * albedo;
	else {
		// UNOFFICIAL 2026-11-11 (sungi) ExteriorBalance (NVR_ALInterior.w > 0 outdoors = the ambient scale): the occlusion darkens only the
		// ambient share of the pixel (as indoors), not the direct sun: lit = lin * ao took the sun's light out of every crease.
		float3 lit = NVR_ALInterior.w > 0.0f ? lin - (1.0f - ao) * min(lin, albedo * NVR_ALScene.rgb * NVR_ALInterior.w) : lin * ao;
		// UNOFFICIAL 2026-11-11 (sungi p2) AmbientReplace (NVR_ALInterior.y = r, .z = AmbientFloor outdoors; r 0 = the line above, unchanged): remove r of the game's
		// ambient share A (before occlusion), keep k = 1 - r + r*floor of it occluded as today: lit = lin - A + k*A*ao. The RT GI adds the sky back.
		[branch] if (NVR_ALInterior.y > 0.0f && NVR_ALInterior.w > 0.0f) {
			float3 share = min(lin, albedo * NVR_ALScene.rgb * NVR_ALInterior.w);
			float keepShare = 1.0f - NVR_ALInterior.y * (1.0f - NVR_ALInterior.z);
			lit = lin - share + keepShare * share * ao;
		}
		if (sky) lit = SkyTake(lit, skyFactor.x, skyFactor.y, TESR_AmbientOcclusionSky5.x, TESR_AmbientOcclusionSky5.w);	// (UNOFFICIAL 2026-10-20 skyamb)
		lin = lit + gridBounce * albedo;	// (the screen bounce removed 2026-10-14: the light grid is the bounce)
	}
	// UNOFFICIAL 2026-11-03 RT GI on the D3D9 passes (NVR_ALGI.z): the bounce light x the surface colour, after the occlusion, as
	// AmbientLightApply-gi adds it (that one takes SurfaceColour's guess; here SurfaceAlbedo, the same guess unless AlbedoBuffer wrote the pixel)
	// (2026-11-07, user: "RT GI at a high strength overrides the normal maps, looks extremely weird") SurfaceColour divides the lit
	// colour by the ambient and caps it at MaxReflectance; under a lamp nearly every pixel is over the cap, so dark grout, bright tiles
	// and the normal map's shading all got one colour and the bounce laid a flat milky layer over them (bench shipfix, demo room: GI High
	// lifted the floor 28 %, its contrast down 9 %). The guess now takes the cap on a small local average (4 taps 4 px out, same surface
	// only; the source before the sun shadow, its ratio carried over) and puts each pixel's own detail back on top (at most 2x).
	[branch] if (giApply) {
		float3 c0 = linearize(tex2Dlod(TESR_SourceBuffer, float4(IN.UVCoord, 0.0f, 0.0f)).rgb);
		float3 localSum = c0;
		float localWeight = 1.0f;
		for (int t = 0; t < 4; t++) {
			float2 o = float2((t == 1 || t == 3) ? 4.0f : -4.0f, t >= 2 ? 4.0f : -4.0f) * px;
			float w = step(abs(readDepthLod(IN.UVCoord + o) - depth), max(depth * 0.02f, 1.0f));
			localSum += linearize(tex2Dlod(TESR_SourceBuffer, float4(IN.UVCoord + o, 0.0f, 0.0f)).rgb) * w;
			localWeight += w;
		}
		float3 localLin = localSum / localWeight;
		float shade = luma(linearize(color.rgb)) / max(luma(c0), 1.0e-4f);	// the sun shadow on this pixel
		float detail = clamp(luma(c0) / max(luma(localLin), 1.0e-4f), 0.0f, 2.0f);
		float3 guess = SurfaceColourCap(localLin * shade, FallbackCap()) * detail;	// (2026-11-10 AlbedoFallbackCap)
		float4 a = tex2Dlod(NVR_ALAlbedo, float4(IN.UVCoord, 0.0f, 0.0f));
		lin += giLight * lerp(guess, max(a.rgb, 0.0f), step(0.5f, NVR_ALFlags.z) * step(0.5f, a.a));
	}
	return delinearize(float4(lin, color.a));
}

technique AmbientLight {
	pass Prepare { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 Prepare(); }
	pass Estimate { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 Estimate(); }
	pass BlurX { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 Blur(float2(1.0f, 0.0f)); }
	pass BlurY { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 Blur(float2(0.0f, 1.0f)); }
	pass Accumulate { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 Accumulate(); }
	pass StatsSample { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 StatsSample(); }
	pass StatsReduce { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 StatsReduce(); }
	pass Apply { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 Apply(false); }

}

// UNOFFICIAL 2026-10-14: the light grid (see GridInject above), each pass into two targets (the step and the running total).
technique AmbientLightGrid {
	pass ScreenInject { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 ScreenInject(); }
	pass GridInject { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 GridInject(); }
	pass GridPropagate { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 GridPropagate(); }
}

// UNOFFICIAL 2026-10-20 (skyamb) Skylighting (see the top): the same passes with the sky's factor in Apply; the AO's sky pass wrote the
// sky target just before.
technique AmbientLightSky {
	pass Prepare { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 Prepare(); }
	pass Estimate { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 Estimate(); }
	pass BlurX { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 Blur(float2(1.0f, 0.0f)); }
	pass BlurY { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 Blur(float2(0.0f, 1.0f)); }
	pass Accumulate { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 Accumulate(); }
	pass StatsSample { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 StatsSample(); }
	pass StatsReduce { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 StatsReduce(); }
	pass Apply { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 Apply(true); }
}

// (after the main technique: AmbientLight.cpp takes GetTechnique(0) for it)
// UNOFFICIAL 2026-10-21 (alrocks): the prep's normal smoothed, drawn between Prepare and the sky test (AmbientLight.cpp RenderPrepare).
technique AmbientLightSmooth {
	pass SmoothX { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 SmoothNormal(false); }
	pass SmoothY { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 SmoothNormal(true); }
}
