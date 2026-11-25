// UNOFFICIAL 2026-11-04 the game's MSAA and the effects that treat sky and surfaces apart (VolumetricClouds, VolumetricLight).
// A pixel on an edge against the sky holds samples of both, but its one depth is the nearer surface's (NVR DXVK resolves the nearest
// sample), so an effect that tells sky from surface by depth handled the whole pixel as surface and its sky share kept what the sky
// had before: a rim round every leaf, branch and ridge. TESR_SkyCoverage (core/SkyCoverage.h) holds each pixel's exact sky share,
// from a far-plane quad drawn against the multisampled depth at the end of the world's main list. Only with the game's MSAA
// (TESR_GameMultisample.x > 1); without it nothing is read and SkyShare is 0.
// The includer declares TESR_RenderedBuffer, TESR_ReciprocalResolution and TESR_GameMultisample, includes Depth.hlsl (readDepthLod,
// farZ), defines linearize, and defines MSAA_SKY_EDGE_REG: the sampler register that is this sampler's place among the file's TESR_
// samplers (EffectRecord binds them by that place), so it is included after all of them.
//
// UNOFFICIAL 2026-11-05 alpha cutouts ([Main.Main.Quality] MsaaEdgeFix, TESR_GameMultisample.z; report: the clouds do not respect a lattice
// pylon's cutouts, the volumetric light turns a see-through desert bush solid). With transparency multisampling the game draws its
// cutout meshes (pylon lattices, foliage: the TMS shaders) with alpha to coverage: a pixel where the texture's alpha is 0.4 gets about
// 40 % of its samples, and far off, where the texture's smaller mips average the holes into the bars, the whole outline is such pixels.
// The one depth the effects read is the nearest sample's under NVR DXVK (the whole see-through part reads as solid), sample 0's under
// stock DXVK and native D3D9 (each pixel solid or sky by chance). The share is the exact answer either way, so with the fix every
// pixel takes it: MsaaSkyCoverage below, for sky-by-depth pixels too (sample 0 left those whole sky: the clouds over the bars), with
// the sky behind found from the pixels round it that are all sky by their share (not by depth), out to 8 pixels (a lattice far off is
// all mixed pixels: within 2 pixels there was often none, and the pixel's own colour, part bar, stood in for the sky).
// TESR_GameMultisample: x the game's samples, y 1 = this frame's shares were made (SkyCoverage::Draw), z 1 = the fix on, w the debug
// view (1: red = sky by depth, green = sky share).

sampler2D TESR_SkyCoverage : register(MSAA_SKY_EDGE_REG) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };

// The sky share of a pixel that is not sky by its depth, and skyBefore: the sky's colour there before the effect (linear), from the
// sky pixels one or two pixels away (the pixel's own colour when none is that near). (MsaaEdgeFix off: as on 2026-11-04.)
float SkyShare(float2 uv, float3 color, out float3 skyBefore) {
	skyBefore = linearize(max(color, 0.0));
	[branch] if (TESR_GameMultisample.x <= 1.0 || TESR_GameMultisample.y < 0.5) return 0.0;
	float share = tex2Dlod(TESR_SkyCoverage, float4(uv, 0, 0)).r;
	[branch] if (share <= 0.002) return 0.0;
	float3 skySum = 0.0;
	float skyCount = 0.0;
	const float2 offsets[8] = { float2(1, 0), float2(-1, 0), float2(0, 1), float2(0, -1), float2(2, 0), float2(-2, 0), float2(0, 2), float2(0, -2) };
	[unroll] for (int i = 0; i < 8; i++) {
		float2 o = offsets[i] * TESR_ReciprocalResolution.xy;
		[branch] if (readDepthLod(uv + o) >= farZ * 0.996) { skySum += tex2Dlod(TESR_RenderedBuffer, float4(uv + o, 0, 0)).rgb; skyCount += 1.0; }
	}
	[flatten] if (skyCount > 0.0) skyBefore = linearize(max(skySum / skyCount, 0.0));
	return share;
}

// ---- UNOFFICIAL 2026-11-05 MsaaEdgeFix ----
// True when this frame's shares are there and the fix is on.
bool MsaaEdgeFixOn() { return TESR_GameMultisample.x > 1.0 && TESR_GameMultisample.y > 0.5 && TESR_GameMultisample.z > 0.5; }

// The pixel's sky share, 0 .. 1 (any pixel).
float MsaaSkyCoverage(float2 uv) { return tex2Dlod(TESR_SkyCoverage, float4(uv, 0, 0)).r; }

// A pixel counts as all sky / all surface past these shares (an 8-bit mask: 1/255 steps).
static const float MsaaAllSky = 0.998;
static const float MsaaNoSky = 0.002;

// The sky's colour behind a mixed pixel before the effect (linear): the mean of the pixels all sky by their share 1, 2, 4 and 8 pixels
// away along the axes; the pixel's own colour when none is.
float3 MsaaSkyBehind(float2 uv, float3 color) {
	float3 skySum = 0.0;
	float skyCount = 0.0;
	const float2 offsets[4] = { float2(1, 0), float2(-1, 0), float2(0, 1), float2(0, -1) };
	const float radii[4] = { 1.0, 2.0, 4.0, 8.0 };
	[unroll] for (int r = 0; r < 4; r++) {
		[branch] if (skyCount < 2.0) {   // the nearest rings that have some
			[unroll] for (int i = 0; i < 4; i++) {
				float2 at = uv + offsets[i] * radii[r] * TESR_ReciprocalResolution.xy;
				[branch] if (MsaaSkyCoverage(at) >= MsaaAllSky) { skySum += tex2Dlod(TESR_RenderedBuffer, float4(at, 0, 0)).rgb; skyCount += 1.0; }
			}
		}
	}
	return linearize(max(skyCount > 0.0 ? skySum / skyCount : color, 0.0));
}

// The depth of the surface part of a mixed pixel whose one depth is the sky's (sample 0 was a sky sample): the nearest surface depth
// among its neighbours 1 and 2 pixels away (fallback when none).
float MsaaSurfaceDepth(float2 uv, float fallback) {
	float nearest = fallback;
	const float2 offsets[8] = { float2(1, 0), float2(-1, 0), float2(0, 1), float2(0, -1), float2(1, 1), float2(-1, 1), float2(1, -1), float2(-1, -1) };
	[unroll] for (int r = 1; r <= 2; r++) {
		[unroll] for (int i = 0; i < 8; i++) {
			float d = readDepthLod(uv + offsets[i] * r * TESR_ReciprocalResolution.xy);
			[flatten] if (d < farZ * 0.996) nearest = min(nearest, d);
		}
	}
	return nearest;
}

// MsaaEdgeDebugView 1 (with or without the game's MSAA): red = sky by the depth the effects read, green = the exact sky share (0 without
// this frame's shares), blue = the picture's luma at a third (to find one's way).
bool MsaaEdgeDebugOn() { return TESR_GameMultisample.w > 0.5; }
float4 MsaaEdgeDebug(float2 uv, float depth, float3 color) {
	float share = TESR_GameMultisample.x > 1.0 && TESR_GameMultisample.y > 0.5 ? MsaaSkyCoverage(uv) : 0.0;
	return float4(depth >= farZ * 0.996 ? 1.0 : 0.0, share, saturate(dot(color, float3(0.2126, 0.7152, 0.0722))) * 0.33, 1.0);
}
