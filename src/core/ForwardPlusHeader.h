#pragma once
// UNOFFICIAL 2026-11-15 DeferredTransparents (core/DeferredTransparents.h, Shaders/Includes/ForwardPlus.hlsl FPHeader; docs/next/FD-GLASS-RESULTS.md):
// what the forward+ records need besides the lamps and the froxel lists, written into the lamp table itself so that no object pixel constant is
// taken (c200-c223 are full): five texels at the right end of the second tile row (ManyLamps::TileRow() + 1), which holds one texel a lamp for
// at most 1,024 lamps, so its texels 1,024-2,047 were never written (zeros). ManyLamps::BuildTable calls Put after PutTiles, while the table
// is locked, only while Enabled (DeferredTransparents on at startup); otherwise nothing is written and the table is as before.
// Texel 2047 - k: k 0 the camera's world position, w 1 = the froxel lists are this frame's world view; k 1 the view's forward axis, w tan(half
// fov x); k 2 the right axis, w tan(half fov y); k 3 the up axis; k 4 x, y the viewport the lists were made for in 64-pixel tiles (fractional:
// a screen share times these is the tile, as LampTableCore::Rect binned), z tiles across, w tiles down (the lists' layout).
namespace ForwardPlusHeader {
	enum { TableTexels = 2048, Texels = 8 };
	inline bool Enabled = false;	// DeferredTransparents::Load sets it (records loaded); (2026-11-16) also the hair's and the leaves' forward+ records
	// (2026-11-16 fd-fwdgroups) texels k 5-7: the flashlight (FlashlightDeferred::LightParams' three rows: camera-relative position + reach,
	// axis + cos outer, linear light + cos inner), set by DeferredTransparents; k 5 w 0 = no flashlight this frame. The table leaves the
	// flashlight's game lamp out (the light pass lights the G-buffer) and the forward+ records compile the draw's lamps out: without these
	// texels glass lost the flashlight entirely.
	inline bool (*Flashlight)(float out[3][4]) = nullptr;
	inline unsigned Writes = 0, ValidWrites = 0;

	// row: the second tile row's first float (4 floats a texel). The axes and tangents: the world camera's, as ManyLamps::BinFroxels binned with them.
	inline void Put(float* row, const float camera[3], const float right[3], const float up[3], const float forward[3], float tanX, float tanY,
		float tileSpanX, float tileSpanY, int tilesX, int tilesY, bool valid) {
		if (!Enabled || !row) return;
		float* t = row + (TableTexels - 1) * 4;	// texel 2047 (k 0), then leftwards
		t[0] = camera[0]; t[1] = camera[1]; t[2] = camera[2]; t[3] = valid && tilesX > 0 && tilesY > 0 ? 1.0f : 0.0f;
		t -= 4; t[0] = forward[0]; t[1] = forward[1]; t[2] = forward[2]; t[3] = tanX;
		t -= 4; t[0] = right[0]; t[1] = right[1]; t[2] = right[2]; t[3] = tanY;
		t -= 4; t[0] = up[0]; t[1] = up[1]; t[2] = up[2]; t[3] = 0.0f;
		t -= 4; t[0] = tileSpanX; t[1] = tileSpanY; t[2] = (float)tilesX; t[3] = (float)tilesY;
		float beam[3][4] = {};
		const bool flash = valid && Flashlight && Flashlight(beam);
		for (int k = 0; k < 3; k++) { t -= 4; for (int c = 0; c < 4; c++) t[c] = flash ? beam[k][c] : 0.0f; }
		Writes++;
		if (valid && tilesX > 0 && tilesY > 0) ValidWrites++;
	}
}
