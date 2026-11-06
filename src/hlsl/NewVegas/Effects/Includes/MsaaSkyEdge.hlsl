// UNOFFICIAL 2026-11-05 (from Balebandro's fork, mo2-dxvk-loader 256de14) the game's MSAA and the effects that treat sky and surfaces apart (VolumetricClouds, VolumetricLight).
// A pixel on an edge against the sky holds samples of both, but its one depth is the nearer surface's (NVR DXVK resolves the nearest
// sample), so an effect that tells sky from surface by depth handled the whole pixel as surface and its sky share kept what the sky
// had before: a rim round every leaf, branch and ridge. TESR_SkyCoverage (core/SkyCoverage.h) holds each pixel's exact sky share,
// from a far-plane quad drawn against the multisampled depth at the end of the world's main list. Only with the game's MSAA
// (TESR_GameMultisample.x > 1); without it nothing is read and SkyShare is 0.
// The includer declares TESR_RenderedBuffer, TESR_ReciprocalResolution and TESR_GameMultisample, includes Depth.hlsl (readDepthLod,
// farZ), defines linearize, and defines MSAA_SKY_EDGE_REG: a sampler register nothing else in the effect uses (NVR_ samplers bound by
// hand included; EffectRecord binds TESR_ samplers by their register).

sampler2D TESR_SkyCoverage : register(MSAA_SKY_EDGE_REG) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };

// The sky share of a pixel that is not sky by its depth, and skyBefore: the sky's colour there before the effect (linear), from the
// sky pixels one or two pixels away (the pixel's own colour when none is that near).
float SkyShare(float2 uv, float3 color, out float3 skyBefore) {
	skyBefore = linearize(max(color, 0.0));
	[branch] if (TESR_GameMultisample.x <= 1.0) return 0.0;
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
