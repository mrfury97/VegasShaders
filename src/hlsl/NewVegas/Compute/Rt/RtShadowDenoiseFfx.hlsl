// UNOFFICIAL 2026-11-07 RT shadow denoiser, core 1: AMD FidelityFX Shadows Denoiser (MIT; ffx/LICENSE.txt, ffx/*.h = AMD's headers,
// GPUOpen-Effects/FidelityFX-Denoiser d7dfecba, with the few lines marked 'NVR:' changed). docs/next/RT-DENOISER.md.
// This file is the glue: AMD's callbacks over the game's inputs (view z / farZ depth, view-space normals, our reprojection from the
// camera move + ObjectMotion instead of 2D motion vectors), one entry per stage, layers (lamps) along the dispatch's z.
//   -D STAGE_TILE          the tile classification + temporal pass (FFX_DNSR_Shadows_TileClassification)
//   -D STAGE_FILTER -D PASS=0|1|2   the three edge-aware a-trous passes (steps 1, 2, 4); pass 0 also writes the history, pass 2 the result
// Compile (dxc from the Vulkan SDK):
//   dxc -spirv -fspv-target-env=vulkan1.2 -T cs_6_2 -E main -D STAGE_TILE -Fo RtShadowDenoiseFfxTile.spv RtShadowDenoiseFfx.hlsl
//   dxc -spirv -fspv-target-env=vulkan1.2 -T cs_6_2 -E main -D STAGE_FILTER -D PASS=0 -Fo RtShadowDenoiseFfxFilter0.spv RtShadowDenoiseFfx.hlsl
//   (PASS=1 -> RtShadowDenoiseFfxFilter1.spv, PASS=2 -> RtShadowDenoiseFfxFilter2.spv)

// AMD's filter uses 16-bit floats; the helper's device does not enable them: plain floats (the maths is the same)
#define float16_t float
#define float16_t2 float2
#define float16_t3 float3

[[vk::binding(1, 0)]] cbuffer Pass {	// = RtShadowDenoise.glsl `Pass`
	float4 size, forward, right, up, prevForward, prevRight, prevUp, cameraMove, jitter, full, params, params2, light;
	float4 layerSlot[2];
	float4 layerReset[2];
	float4 antiLag, history;
};

[[vk::binding(2, 0)]] [[vk::image_format("r32f")]] RWTexture2D<float> depthImage;
[[vk::binding(3, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> normalImage;
[[vk::binding(4, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> motionImage;
[[vk::binding(12, 0)]] [[vk::image_format("r32ui")]] RWTexture2D<uint> maskImage;      // the traced 0/1 visibility, 1 bit a pixel, 8 x 4 a texel
[[vk::binding(13, 0)]] [[vk::image_format("r32ui")]] RWTexture2D<uint> metaImage;      // FFX's tile metadata (8 x 8 tiles)
#ifdef STAGE_TILE
[[vk::binding(14, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> momentsPrev;
[[vk::binding(15, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> momentsNext;
[[vk::binding(16, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> reprojImage;  // x mean, y variance
[[vk::binding(17, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> historyPrev;  // last frame's filter pass 0 (x)
[[vk::binding(18, 0)]] [[vk::image_format("r32f")]] RWTexture2D<float> prevDepth;        // last frame's view z per pixel
#endif
#ifdef STAGE_FILTER
[[vk::binding(14, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> inImage;
[[vk::binding(15, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> outImage;
[[vk::binding(16, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> historyNext;  // pass 0
[[vk::binding(17, 0)]] [[vk::image_format("rg16f")]] RWTexture2D<float2> finalImage;     // pass 2: what the pack reads
[[vk::binding(18, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> widthImage;    // core 2: our temporal stage's output (z: penumbra half-width)
#endif

static uint g_layer = 0;

int2 Dims() { return int2(size.xy); }
int2 Offset() { return int2(0, int(g_layer) * int(size.y)); }
int TilesX() { return (int(size.x) + 7) / 8; }
int MaskRows() { return (int(size.y) + 3) / 4; }
int MetaRows() { return (int(size.y) + 7) / 8; }
bool LayerReset() { uint k = g_layer; return prevForward.w < 0.5 || (k < 4 ? layerReset[0][k] : layerReset[1][k - 4]) > 0.5; }
int2 SourceOf(int2 p) { return int2((float2(p) + 0.5) * full.xy * size.zw); }
float Linear01(int2 p) { p = clamp(p, int2(0, 0), Dims() - 1); return depthImage[SourceOf(p)]; }
bool ValidDepth(float d) { return d > 0.0 && d < 0.9999 && d * forward.w >= 1.0; }

float3 PositionOf(int2 p, float linear01) {
	float2 uv = (float2(p) + 0.5) * size.zw + jitter.xy;
	return (forward.xyz + right.xyz * (2.0 * uv.x - 1.0) - up.xyz * (2.0 * uv.y - 1.0)) * (linear01 * forward.w);
}
float3 NormalOf(int2 p) {
	p = clamp(p, int2(0, 0), Dims() - 1);
	float3 n = normalImage[SourceOf(p)].xyz * 2.0 - 1.0;
	return normalize(normalize(right.xyz) * n.x + normalize(up.xyz) * n.y + normalize(forward.xyz) * n.z);
}
// As RtShadowDenoise.glsl reproject(): the pixel's point in last frame's pixels (continuous) and view z
bool Reproject(int2 p, out float2 prevPixel, out float prevZ) {
	prevPixel = float2(-1, -1); prevZ = 0;
	float d = Linear01(p);
	if (!ValidDepth(d)) return false;
	float3 move = prevRight.w > 0.5 ? motionImage[SourceOf(p)].xyz : float3(0, 0, 0);
	float3 q = PositionOf(p, d) + cameraMove.xyz - move;
	prevZ = dot(q, prevForward.xyz);
	if (!(prevZ > 1.0)) return false;
	float x = dot(q, prevRight.xyz) / (prevZ * dot(prevRight.xyz, prevRight.xyz));
	float y = -dot(q, prevUp.xyz) / (prevZ * dot(prevUp.xyz, prevUp.xyz));
	float2 uv = float2(x, y) * 0.5 + 0.5 - jitter.zw;
	prevPixel = uv * size.xy;
	return all(prevPixel >= 0.0) && all(prevPixel < size.xy);
}

// ---- FFX callbacks
int2 FFX_DNSR_Shadows_GetBufferDimensions() { return Dims(); }
float2 FFX_DNSR_Shadows_GetInvBufferDimensions() { return size.zw; }
float NVR_FFX_LinearDepth(float depth) { return depth * forward.w; }
bool FFX_DNSR_Shadows_IsShadowReciever(uint2 did) { return all(int2(did) < Dims()) && ValidDepth(Linear01(int2(did))); }
float FFX_DNSR_Shadows_ReadDepth(int2 did) { return Linear01(did); }
float16_t3 FFX_DNSR_Shadows_ReadNormals(int2 did) { return NormalOf(did); }
float FFX_DNSR_Shadows_GetDepthSimilaritySigma() { return 0.05; }	// (NVR: x the centre's view z: relative)
uint FFX_DNSR_Shadows_ReadRaytracedShadowMask(uint linear_tile_index) {
	int tx = TilesX();
	return maskImage[int2(int(linear_tile_index) % tx, int(linear_tile_index) / tx + int(g_layer) * MaskRows())];
}
uint FFX_DNSR_Shadows_ReadTileMetaData(uint linear_tile_index) {
	int tx = TilesX();
	return metaImage[int2(int(linear_tile_index) % tx, int(linear_tile_index) / tx + int(g_layer) * MetaRows())];
}
void FFX_DNSR_Shadows_WriteMetadata(uint linear_tile_index, uint mask) {
	int tx = TilesX();
	metaImage[int2(int(linear_tile_index) % tx, int(linear_tile_index) / tx + int(g_layer) * MetaRows())] = mask;
}

#ifdef STAGE_TILE
float2 NVR_FFX_Velocity(int2 did) {
	float2 prevPixel; float prevZ;
	if (!Reproject(did, prevPixel, prevZ)) return float2(10.0, 10.0);	// off screen: history_uv outside, disoccluded
	return (float2(did) + 0.5) * size.zw - prevPixel * size.zw;
}
bool NVR_FFX_IsDisoccluded(uint2 did) {
	if (LayerReset()) return true;
	float2 prevPixel; float prevZ;
	if (!Reproject(int2(did), prevPixel, prevZ)) return true;
	float stored = prevDepth[int2(prevPixel)];
	float facing = abs(dot(NormalOf(int2(did)), normalize(-PositionOf(int2(did), Linear01(int2(did))))));
	float tolerance = params.z * (1.0 + 4.0 * (1.0 - saturate(facing)));
	return !(stored > 0.0 && abs(stored - prevZ) <= tolerance * prevZ);
}
float3 FFX_DNSR_Shadows_ReadPreviousMomentsBuffer(int2 pos) { pos = clamp(pos, int2(0, 0), Dims() - 1); return momentsPrev[pos + Offset()].xyz; }
uint FFX_DNSR_Shadows_IsFirstFrame() { return LayerReset() ? 1u : 0u; }
float FFX_DNSR_Shadows_ReadHistory(float2 history_uv) {
	float2 f = history_uv * size.xy - 0.5;
	int2 b = int2(floor(f)); float2 t = f - float2(b);
	int2 m = Dims() - 1;
	float a = historyPrev[clamp(b, int2(0, 0), m) + Offset()].x, c = historyPrev[clamp(b + int2(1, 0), int2(0, 0), m) + Offset()].x;
	float d = historyPrev[clamp(b + int2(0, 1), int2(0, 0), m) + Offset()].x, e = historyPrev[clamp(b + int2(1, 1), int2(0, 0), m) + Offset()].x;
	return lerp(lerp(a, c, t.x), lerp(d, e, t.x), t.y);
}
void FFX_DNSR_Shadows_WriteReprojectionResults(uint2 did, float2 value) { if (all(int2(did) < Dims())) reprojImage[int2(did) + Offset()] = float4(value, 0, 0); }
void FFX_DNSR_Shadows_WriteMoments(uint2 did, float3 value) { if (all(int2(did) < Dims())) momentsNext[int2(did) + Offset()] = float4(value, 0); }
float2 FFX_DNSR_Shadows_ReadVelocity(int2 did) { return float2(0, 0); }	// (unused: NVR_FFX_Velocity)
float4x4 FFX_DNSR_Shadows_GetProjectionInverse() { return (float4x4)0; }	// (unused: NVR_FFX_LinearDepth)
float4x4 FFX_DNSR_Shadows_GetReprojectionMatrix() { return (float4x4)0; }	// (unused: NVR_FFX_IsDisoccluded)
float4x4 FFX_DNSR_Shadows_GetViewProjectionInverse() { return (float4x4)0; }
float3 FFX_DNSR_Shadows_GetEye() { return float3(0, 0, 0); }

#include "ffx/ffx_denoiser_shadows_tileclassification.h"

[numthreads(64, 1, 1)]
void main(uint group_index : SV_GroupIndex, uint3 gid : SV_GroupID) {
	g_layer = gid.z;
	FFX_DNSR_Shadows_TileClassification(group_index, gid.xy);
}
#endif

#ifdef STAGE_FILTER
// Core 2 (FFX + our penumbra sizing): the a-trous step of this pixel. AMD's passes reach 1 + 2 + 4 = 7 pixels whatever the shadow;
// here each pass's step is scaled by the widest penumbra radius (pixels) reaching this pixel (its own and its 3 x 3 neighbours' at 4
// pixels, on the same surface), over 7: no spatial blur where the caster touches the receiver (contact hardening), AMD's full reach
// for penumbrae of 7 pixels and more. Core 1: AMD's steps.
static uint g_step = 1;
uint NVR_FFX_Step(uint stepsize) { return history.z > 1.5 ? g_step : stepsize; }
float PenumbraPixels(int2 did) {
	float d = Linear01(did);
	if (!ValidDepth(d)) return 0.0;
	float3 pos = PositionOf(did, d), n = NormalOf(did);
	float viewZ = d * forward.w;
	float pixelWorld = 2.0 * viewZ * length(up.xyz) * size.w;
	float widest = 0.0;
	for (int y = -1; y <= 1; y++)
		for (int x = -1; x <= 1; x++) {
			int2 q = clamp(did + int2(x, y) * 4, int2(0, 0), Dims() - 1);
			float dq = Linear01(q);
			if (!ValidDepth(dq) || abs(dot(n, PositionOf(q, dq) - pos)) > 4.0 * params.z * viewZ) continue;
			float r = widthImage[q + Offset()].z / pixelWorld;
			// a neighbour's penumbra counts here only if it reaches this pixel (as our blur's blocker search)
			if (x == 0 && y == 0 || r + 1.0 >= 4.0 * length(float2(x, y))) widest = max(widest, r);
		}
	return min(widest, params.y);
}

float16_t2 FFX_DNSR_Shadows_ReadInput(int2 p) { return inImage[clamp(p, int2(0, 0), Dims() - 1) + Offset()].xy; }

#include "ffx/ffx_denoiser_shadows_filter.h"

[numthreads(8, 8, 1)]
void main(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID, uint3 did : SV_DispatchThreadID) {
	g_layer = gid.z;
	const uint stepsize = 1u << PASS;
	if (history.z > 1.5) g_step = (uint)round(stepsize * saturate(PenumbraPixels(int2(did.xy)) / 7.0));
	bool write = false;
	float2 results = FFX_DNSR_Shadows_FilterSoftShadowsPass(gid.xy, gtid.xy, did.xy, write, PASS, stepsize);
	if (!write) {	// (NVR) pass 1 skips cleared tiles in AMD's code; write their known answer so pass 2 never reads a stale texel
		bool cleared, light;
		FFX_DNSR_Shadows_ReadTileMetaData(gid.xy, cleared, light);
		results = float2(light ? 1.0 : 0.0, 0.0);
	}
	int2 p = int2(did.xy);
	if (any(p >= Dims())) return;
	outImage[p + Offset()] = float4(results, 0, 0);
#if PASS == 0
	historyNext[p + Offset()] = float4(results, 0, 0);
#endif
#if PASS == 2
	finalImage[p + Offset()] = float2(results.x, 0.0);
#endif
}
#endif
