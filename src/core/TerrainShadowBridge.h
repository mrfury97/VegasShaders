#pragma once
// UNOFFICIAL 2026-11-28 mountain shadows in the deferred sun ([Shaders.ShadowsExteriors.TerrainShadows] Deferred; owner: "can it be
// fully deferred? doesnt seem to blend with fog or the scene too well"). The land's shadow (effects/TerrainShadows) was only ever a
// screen factor the SunFar pass worked out and the composites multiplied into the finished pixel, fog and all. Now the Vulkan deferred
// sun (Compute/DeferredSun.comp, kFlagTerrainShadow) takes it off the sunlight itself, before fog and sky light, for the pixels it
// lights; SunFar then leaves the land's part out of its factor on those pixels (Compute/SunFar.frag, the G-buffer's deferred mark)
// and keeps it for everything drawn forward (grass, water, trees far off, the sky's edge).
//
// Plain values between the three, so core/DeferredSun.h needs no effect headers:
//  - TerrainShadowMap::Publish (each frame the shadow maps are drawn, ShadowManager::RenderShadowMaps) fills the map's textures and
//    constants and the frame;
//  - SunFarEffect::ExtRender says whether its Vulkan pass runs (only that pass knows the deferred mark: the D3D9 technique has no
//    sampler left, so while it draws the deferred sun leaves the land's shadow to it, as before);
//  - DeferredSun::TryApplyCompute applies it and notes the frame it did, which SunFar reads.

struct IDirect3DTexture9;

namespace TerrainShadowBridge {
	inline IDirect3DTexture9* Heights = nullptr;	// G16R16: r upper, g lower shadow height (TESR_TerrainShadowBuffer)
	inline IDirect3DTexture9* Ground = nullptr;	// L16: the map's ground height (TESR_TerrainShadowGround)
	inline float Data[4] = {};		// TESR_TerrainShadowData: x west edge, y north edge (world), z 1 / width (units), w strength (0: off)
	inline float Range[4] = {};		// TESR_TerrainShadowRange: x lowest encoded height, y encoded range, z bias, w blur
	inline float Params[4] = {};		// TESR_TerrainShadowParams: x 1 / height (units), y DebugView, z Darkness, w Contrast
	inline bool DeferredWanted = true;	// [Shaders.ShadowsExteriors.TerrainShadows] Deferred
	inline unsigned PublishedFrame = ~0u;	// the frame the values above are for
	inline bool SunFarVulkan = false;	// SunFar's Vulkan pass has run (and was not refused since)
	inline unsigned AppliedFrame = ~0u;	// the last frame the deferred sun took the land's shadow off its sunlight (world pixels)
	// (review 2026-11-28) the pixels it did so, exactly: 1 where DeferredSun.comp multiplied the sunlight by the land's shadow (world and
	// first-person runs), cleared at the frame's first dispatch. SunFar leaves the land's part out where it is 1 (the G-buffer's albedo
	// mark was not the same set: deferred foliage keeps a = 1, and some marked records are never lit by the sun pass).
	inline IDirect3DTexture9* Mark = nullptr;

	// The deferred sun may take it this frame: the map is live, published this frame, and SunFar can leave it out where it did.
	inline bool Usable(unsigned frame) {
		return DeferredWanted && SunFarVulkan && PublishedFrame == frame && Data[3] > 0.0f && Heights && Ground;
	}
}
