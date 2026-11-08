#pragma once

// UNOFFICIAL 2026-11-07 (ptaaa-fog) RT fog visibility: the trace pass's constants and the history rules, plain C++ (no game types), so
// the game (core/RtFogVisibility.cpp) and the RT test host (tools/nvr-rt-host, scene fogvis) fill Compute\Rt\RtFogVisibility.comp's block
// the same way. The pass traces the sun's visibility per cell of the volumetric light's froxel grid (VolumetricLight.fx.hlsl GridLightPS:
// one column per 16 x 16 screen pixels, 64 slices out to MaxDistance, slice s's near side at MaxDistance (s / 64)^2). See the .comp.

#include <algorithm>
#include <cmath>

namespace RtFogGrid {
	// RtFogVisibility.comp's uniform block (std140, float4 each; tools/nvr-rt-host/rt_layout_check.py checks the two)
	struct PassConstants {
		float grid[4];          // x columns, y rows, z slices (64), w MaxDistance
		float forward[4];       // xyz camera forward (unit), w the RT range (distance from the eye)
		float right[4];         // xyz camera right / P00
		float up[4];            // xyz camera up / P11
		float origin[4];        // xyz the eye in scene space (0), w the frame (the points' shift)
		float sun[4];           // xyz toward the sun (unit), w 1: it casts shadows
		float rays[4];          // x rays per cell, y last frame's weight (0: none), z cull mask, w 1: alpha-tested casters tested
		float trace[4];         // x ray length, y 1: the points move every frame, z debug (1: no history), w TemporalMoving
		float prevForward[4];   // last frame's camera: xyz forward, w its MaxDistance
		float prevRight[4];     // xyz right * P00, w its columns
		float prevUp[4];        // xyz up * P11, w its rows
		float prevOrigin[4];    // xyz last frame's eye minus this frame's (world units)
	};

	// A camera as the grid sees it: unit forward / right / up (scene axes: world, z up), the projection's [0][0] and [1][1], the eye's
	// world position (double: the Mojave's coordinates run past 100 000 units), and the grid it was traced with.
	struct View {
		double	eye[3] = { 0.0, 0.0, 0.0 };
		float	forward[3] = { 0.0f, 1.0f, 0.0f }, right[3] = { 1.0f, 0.0f, 0.0f }, up[3] = { 0.0f, 0.0f, 1.0f };
		float	p00 = 1.0f, p11 = 1.0f;
		float	maxDistance = 5000.0f;
		unsigned columns = 0, rows = 0;
		float	sun[3] = { 0.0f, 0.0f, 1.0f };
		bool	valid = false;
	};

	struct Settings {
		int		rays = 2;			// RaysPerCell (1-8)
		float	temporal = 0.9f;	// Temporal: the largest weight of last frame's value (0: no history)
		float	temporalMoving = 0.7f;	// TemporalMoving: the largest for a cell that moved a whole cell since last frame (the camera moves)
		float	range = 8000.0f;	// the RT range (distance from the eye; capped by SceneRadius by the caller)
		float	rayLength = 20000.0f;
		bool	jitter = true;		// the points move every frame (with Temporal > 0)
		int		debug = 0;			// 1: no history (each frame's rays alone)
		unsigned mask = 0xFFu;		// cull mask (land, objects, alpha-tested 0x10, actors 0x20, far LOD 0x80)
		bool	alphaTested = false;	// the scene holds alpha-tested casters (RtCore::AlphaTested): binding 4 is the alpha atlas
	};

	// One column per 16 x 16 screen pixels (VolumetricLight.cpp RegisterTextures: the grid textures are 8 x 8 tiles of these)
	inline void Columns(unsigned width, unsigned height, unsigned& columns, unsigned& rows) { columns = (width + 15) / 16; rows = (height + 15) / 16; }

	// Last frame's weight: an even average over the frames since the history restarted (1/2, 2/3, 3/4 ...), held at Temporal
	inline float HistoryWeight(unsigned framesSinceReset, float temporal) {
		if (framesSinceReset == 0 || temporal <= 0.0f) return 0.0f;
		return (std::min)(temporal, (float)framesSinceReset / ((float)framesSinceReset + 1.0f));
	}

	// Last frame's grid can feed this frame's: the same grid, the camera moved less than 500 units (a load, fast travel, a cut restart
	// it), the sun turned less than ~2.5 degrees (a time skip). The caller also needs last frame to have been traced.
	inline bool Continues(const View& now, const View& last) {
		if (!last.valid || !now.valid || last.columns != now.columns || last.rows != now.rows) return false;
		const double dx = now.eye[0] - last.eye[0], dy = now.eye[1] - last.eye[1], dz = now.eye[2] - last.eye[2];
		if (dx * dx + dy * dy + dz * dz > 500.0 * 500.0) return false;
		return now.sun[0] * last.sun[0] + now.sun[1] * last.sun[1] + now.sun[2] * last.sun[2] > 0.999f;
	}

	// The pass's constants: this frame's view, last frame's (read only when weight > 0), the frame number (the points' shift).
	inline void Fill(PassConstants& c, const View& now, const View& last, float weight, bool sunUp, const Settings& s, unsigned frame) {
		c = PassConstants();
		c.grid[0] = (float)now.columns; c.grid[1] = (float)now.rows; c.grid[2] = 64.0f; c.grid[3] = now.maxDistance;
		for (int i = 0; i < 3; i++) {
			c.forward[i] = now.forward[i];
			c.right[i] = now.right[i] / now.p00;
			c.up[i] = now.up[i] / now.p11;
			c.sun[i] = now.sun[i];
		}
		c.forward[3] = s.range;
		c.origin[3] = (float)(frame & 1023u);
		c.sun[3] = sunUp ? 1.0f : 0.0f;
		c.rays[0] = (float)(std::min)((std::max)(s.rays, 1), 8);
		c.rays[1] = s.debug == 1 ? 0.0f : weight;
		c.rays[2] = (float)(s.mask & 0xFFu);
		c.rays[3] = s.alphaTested ? 1.0f : 0.0f;
		c.trace[0] = s.rayLength;
		c.trace[1] = s.jitter && s.temporal > 0.0f && s.debug != 1 ? 1.0f : 0.0f;
		c.trace[2] = s.debug == 1 ? 1.0f : 0.0f;
		c.trace[3] = (std::min)((std::max)(s.temporalMoving, 0.0f), (std::max)(s.temporal, 0.0f));
		if (c.rays[1] > 0.0f) {
			for (int i = 0; i < 3; i++) {
				c.prevForward[i] = last.forward[i];
				c.prevRight[i] = last.right[i] * last.p00;
				c.prevUp[i] = last.up[i] * last.p11;
				c.prevOrigin[i] = (float)(last.eye[i] - now.eye[i]);
			}
			c.prevForward[3] = last.maxDistance;
			c.prevRight[3] = (float)last.columns;
			c.prevUp[3] = (float)last.rows;
		}
	}
}
