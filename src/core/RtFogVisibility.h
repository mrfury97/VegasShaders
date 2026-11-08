#pragma once

// UNOFFICIAL 2026-11-07 (ptaaa-fog) RT fog visibility ([Shaders.VolumetricLight.RT]; docs\next\RT-PATH-TRACING.md "ptaaa-fog",
// src\effects\VolumetricLight\README.md): the sun's visibility in the AIR, per cell of the volumetric light's froxel grid, traced against
// the ray tracing core's scene (RtCore.h) instead of read from the cascaded shadow maps, so the shafts (VolumetricLight) and the fog's sun
// shading (VolumetricFog with FogVolume: the grid's sums) follow the same casters as the RT sun shadows and the path tracer: far
// geometry past the cascades, alpha-tested cards, actors, the far LOD. The trace (Compute\Rt\RtFogVisibility.comp, its constants by
// RtFogGrid.h) runs at the RT point of the frame into the helper's ping-pong pair (a temporal history in the helper) and is copied out into
// TESR_RtFogVisibility (VolumetricLightEffect::Textures.RtVisibility*, the grid's 8 x 8 slice tiles, R16F); the volumetric light's GridLight
// (Froxels) reads it texel for texel, its March (Froxels off, or a RenderScale rect frame) by position, with the cascades past the RT range.
// On by default, but it only rides along: it traces only while the helper already runs for another RT effect (RtCore::OthersWant), so
// without a ray tracing GPU, the fork or another RT effect on, nothing changes (the cascades, as before).

namespace RtFogVisibility {
	void		Register();				// once: the core user (after the other RT users: its images and its pass come last)
	// TESR_VolumetricLightRt for this frame: x 0 = not traced this frame (the cascades), 1 = traced on this frame's grid (GridLight reads
	// the cell's own texel), 2 = traced on a grid of another MaxDistance (read by position); y the RT range, z the fade to the cascades
	// before it, w the MaxDistance it was traced with
	D3DXVECTOR4	Constant(float GridDistance);
	std::string	StatusText();			// the REPORT block's line
}
