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
	// UNOFFICIAL 2026-11-13 (rtvol2) [Shaders.VolumetricLight.RT] Expanded (off by default): while the air is traced, the look of a current
	// game's traced / froxel volumetric light instead of the shadow maps' lookup alone: the lit path out to ExpandedDistance (every cell
	// traced, so hills and buildings far past the shadow maps shade the air), its own density thinning with height above the eye
	// (ExpandedDensity, ExpandedHeight), a tight forward glow round the sun (ExpandedSunGlow, ExpandedGlowSharpness) and the volumetric
	// clouds' shadow in the air (ExpandedClouds, in the trace). Old behaviour: Expanded false (or DebugView 1).
	struct Look { float distance, density, heightInv, glow, sharpness; };
	float		ExpandedGridDistance();	// the grid's reach the trace should use (0: not asked for); VolumetricLightEffect::GridDistance
	bool		ExpandedLook(Look& Out);	// true: the last trace (this frame or the one before) was the expanded one: use its look
	std::string	StatusText();			// the REPORT block's line
	// UNOFFICIAL 2026-11-13 (a tester: "rt vol light = no worky"): what the traced shafts do RIGHT NOW, in one plain line, for the Ray
	// tracing page's status block ("Light shafts") and the session log. Returns 0 = off or nothing to do here (dim), 1 = running (green),
	// 2 = on, but something stops it or leaves nothing to see (amber). Code (may be null): a number per reason (the log's change test).
	// Indoors the line also says what the indoor neighbour does (the fog's lamp haze and its traced shadows, RtFogLamps below).
	int			MenuState(std::string& Text, int* Code = nullptr);
	// Once a second (SessionLog::OnFrame), only while the RT core runs: the line above as "RT LIGHT SHAFTS: ..." when its reason changes
	// (at most 12 lines a session).
	void		LogIfChanged();
}

// UNOFFICIAL 2026-11-12 LampHazeShadows ([Shaders.VolumetricFog.Interiors] LampHazeShadows, off by default; LampHazeShadowCell,
// LampHazeShadowSamples, LampHazeShadowTemporal): the lamps' light in the indoor fog's air (LampHaze) shadowed by the room. Before, a lamp
// whose sphere of light reached through a wall lit the air on the other side too (the fog's lamp term has no march and no shadow read).
// A second rider of the RT core, registered by RtFogVisibility::Register, indoors only: for each lamp the fog lists (up to 16) and each
// screen cell it traces the share of the lamp's light along that view that really reaches the air (Compute\Rt\RtFogLamps.comp, constants
// and tile layout core\RtFogLampGrid.h; the rays follow the RT lamp shadows' rules: LampStop, SkipLampModels, SkipLampGlass, alpha-tested
// casters), with a history in the helper, and copies it out into its own R16F texture (4 x 4 tiles, one per lamp). The fog multiplies
// each lamp's light by its tile's value (VolumetricFog.fx.hlsl LampHazeLight). Like the sun's rider it never starts the helper or keeps
// it tracing by itself (RtCore::OthersWant): without a ray tracing GPU, the fork or another RT effect on, the haze is unshadowed as before.
// The lamps traced are the fog's of its LAST frame (the fog runs after the RT point of the frame): a lamp new to the list is dark in the
// air for its first frame.
namespace RtFogLamps {
	// The fog, every frame its lamp term is on with LampHazeShadows: the lamps it lights the air with (world xyz, w the reach)
	void		Submit(const D3DXVECTOR4* WorldReach, int Count);
	// A lamp's code for the fog's lamp table this frame: 0 = no trace to use (unshadowed, as before); > 0 = its tile + 1 + 32 x the cell
	// size in pixels; -1 = traced, but this lamp is not in the tiles yet (no light in the air this frame)
	float		Code(const D3DXVECTOR4& WorldReach);
	IDirect3DTexture9* Texture();		// the tiles (null: no trace in the last 8 frames)
	std::string	StatusText();			// a line for the REPORT block
}
