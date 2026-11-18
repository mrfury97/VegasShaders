#pragma once

// UNOFFICIAL 2026-11-12 LampHazeShadows ([Shaders.VolumetricFog.Interiors] LampHazeShadows; core/RtFogVisibility.h RtFogLamps): the trace
// pass's constants, its image layout and the lamps' slots, plain C++ (no game types), so the game (core/RtFogVisibility.cpp) and the RT
// test host (tools/nvr-rt-host/rt_host_fog.inl, scene foglamps) fill Compute\Rt\RtFogLamps.comp's block the same way.
// The pass answers, for each of up to 16 lamps and each screen cell (one per Cell x Cell pixels): of the lamp's light the fog would
// scatter toward the camera along this view (VolumetricFog.fx.hlsl LampHazeLight: the view inside the lamp's sphere, up to the surface),
// which share really reaches the air (a ray from each sample point to the lamp). The image is 4 x 4 tiles of columns x rows, tile
// (slot mod 4, slot / 4) the lamp in that slot. A lamp keeps its slot while it stays listed, so its tile's history stays its own.

#include <algorithm>
#include <cmath>

namespace RtFogLampGrid {
	const unsigned kTilesAcross = 4, kSlots = 16;	// VolumetricFogEffect::LampHazeMax lamps at most

	// RtFogLamps.comp's uniform block (std140, float4 each: 22 x 16 = 352 bytes of the pass's 512)
	struct PassConstants {
		float grid[4];          // x columns, y rows of ONE tile, z tiles across (4), w slots traced (the highest used + 1)
		float forward[4];       // xyz camera forward (unit, scene space), w samples per cell and lamp (1-8)
		float right[4];         // xyz camera right / P00, w last frame's weight (0: no history this frame)
		float up[4];            // xyz camera up / P11, w the frame (the points' shift)
		float params[4];        // x the glow's core as a share of the reach (LampHazeCore), y a ray stops this far short of its lamp,
		                        // z the far cull mask, w the near cull mask (the last params2.x of a ray: lamp models left out)
		float params2[4];       // x LampModelDistance (0: one segment), y 1: alpha-tested casters tested (binding 4 the atlas),
		                        // z bit s: slot s holds the same lamp as last frame (its history counts), w 1: the points move every frame
		float lamps[kSlots][4]; // xyz the lamp in scene space (relative to the eye), w its reach (0: an empty slot)
	};

	struct Lamp { float world[3] = { 0.0f, 0.0f, 0.0f }; float reach = 0.0f; };
	struct Slots { Lamp lamp[kSlots]; bool used[kSlots] = {}; };

	// One tile: a cell per Cell x Cell screen pixels (the tile spans the whole screen: cell (x, y) is uv (x + 0.5) / columns ...)
	inline void Tile(unsigned width, unsigned height, unsigned cell, unsigned& columns, unsigned& rows) {
		cell = (std::min)((std::max)(cell, 4u), 32u);
		columns = (width + cell - 1) / cell; rows = (height + cell - 1) / cell;
	}

	// The same lamp a frame apart: within max(8 units, 6 % of its reach) on every axis (a swinging lamp, a light carried by a running
	// player: ~6 units a frame at 60 fps; the fog's own list holds no two lamps within 1 unit) and the same reach within 2 %
	inline bool Same(const Lamp& a, const Lamp& b) {
		const float tolerance = (std::max)(8.0f, 0.06f * (std::min)(a.reach, b.reach));
		return std::fabs(a.world[0] - b.world[0]) < tolerance && std::fabs(a.world[1] - b.world[1]) < tolerance && std::fabs(a.world[2] - b.world[2]) < tolerance &&
			std::fabs(a.reach - b.reach) <= 0.02f * (std::max)(a.reach, b.reach);
	}

	// The slot of a lamp (-1: not in the table)
	inline int Find(const Slots& slots, const Lamp& lamp) {
		for (unsigned s = 0; s < kSlots; s++) if (slots.used[s] && Same(slots.lamp[s], lamp)) return (int)s;
		return -1;
	}

	// This frame's lamps into the table: a lamp already there keeps its slot (returned bit s set: its history is still its own), a new
	// one takes a free slot, slots of lamps no longer listed are freed. At most kSlots lamps.
	inline unsigned Assign(Slots& slots, const Lamp* lamps, int count) {
		count = (std::min)((std::max)(count, 0), (int)kSlots);
		bool taken[kSlots] = {}, keep[kSlots] = {};
		unsigned kept = 0;
		for (unsigned s = 0; s < kSlots; s++) {
			if (!slots.used[s]) continue;
			for (int i = 0; i < count; i++)
				if (!taken[i] && Same(slots.lamp[s], lamps[i])) { taken[i] = true; keep[s] = true; slots.lamp[s] = lamps[i]; kept |= 1u << s; break; }
		}
		for (unsigned s = 0; s < kSlots; s++) slots.used[s] = keep[s];
		for (int i = 0; i < count; i++) {
			if (taken[i]) continue;
			for (unsigned s = 0; s < kSlots; s++)
				if (!slots.used[s]) { slots.used[s] = true; slots.lamp[s] = lamps[i]; break; }
		}
		return kept;
	}

	struct Settings {
		int		samples = 4;			// LampHazeShadowSamples (1-8): points along the view per cell and lamp, each one ray to the lamp
		float	weight = 0.0f;			// last frame's weight this frame (0: none)
		bool	jitter = true;			// the points move every frame (with a history)
		float	core = 0.1f;			// LampHazeCore
		float	lampStop = 12.0f;		// [Shaders.ShadowsInteriors.RT] LampStop
		float	modelDistance = 0.0f;	// [Shaders.ShadowsInteriors.Forward] LampModelDistance with SkipLampModels (0: one segment)
		unsigned farMask = 0xEBu, nearMask = 0xE3u;	// without the alpha-tested (0x10, added by the shader when tested) and lamp glass (0x04); near: no lamp models (0x08)
		bool	alphaTested = false;
	};

	// forward / right / up: the camera's unit axes in scene space (world, z up); eye: its world position (the lamps become relative to it)
	inline void Fill(PassConstants& c, const float forward[3], const float right[3], const float up[3], float p00, float p11, const double eye[3],
		unsigned columns, unsigned rows, const Slots& slots, unsigned keepBits, const Settings& s, unsigned frame) {
		c = PassConstants();
		unsigned traced = 0;
		for (unsigned i = 0; i < kSlots; i++) {
			if (!slots.used[i] || !(slots.lamp[i].reach > 0.0f)) continue;
			for (int a = 0; a < 3; a++) c.lamps[i][a] = (float)((double)slots.lamp[i].world[a] - eye[a]);
			c.lamps[i][3] = slots.lamp[i].reach;
			traced = i + 1;
		}
		c.grid[0] = (float)columns; c.grid[1] = (float)rows; c.grid[2] = (float)kTilesAcross; c.grid[3] = (float)traced;
		for (int a = 0; a < 3; a++) { c.forward[a] = forward[a]; c.right[a] = right[a] / p00; c.up[a] = up[a] / p11; }
		c.forward[3] = (float)(std::min)((std::max)(s.samples, 1), 8);
		c.right[3] = (std::min)((std::max)(s.weight, 0.0f), 0.97f);
		c.up[3] = (float)(frame & 1023u);
		c.params[0] = (std::min)((std::max)(s.core, 0.05f), 1.0f);
		c.params[1] = (std::max)(s.lampStop, 0.0f);
		c.params[2] = (float)(s.farMask & 0xFFu);
		c.params[3] = (float)(s.nearMask & 0xFFu);
		c.params2[0] = (std::max)(s.modelDistance, 0.0f);
		c.params2[1] = s.alphaTested ? 1.0f : 0.0f;
		c.params2[2] = (float)(keepBits & 0xFFFFu);
		c.params2[3] = s.jitter && c.right[3] > 0.0f ? 1.0f : 0.0f;
	}

	// Rows of the image the pass must cover: the tiles up to the highest slot traced (grid.w)
	inline unsigned TileRows(const PassConstants& c) { return ((unsigned)c.grid[3] + kTilesAcross - 1) / kTilesAcross; }
}
