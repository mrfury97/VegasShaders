#pragma once

// UNOFFICIAL 2026-11-07 the RT shadow denoiser's game side, plain C++ (no game types: the RT test host tools/nvr-rt-host includes it too).
// docs/next/RT-DENOISER.md. The denoiser itself runs in the RT helper (tools/nvr-rt-helper/NvrRtShadowDenoise.inl) as one pass of
// "RtShadowDenoise" after a trace pass; its stages are src/hlsl/NewVegas/Compute/Rt/RtShadowDenoise*.comp. This header fills that
// pass's constant block (DenoiseConstants = RtShadowDenoise.glsl's `Pass`) and picks which lamps it denoises (Layers).

#include <cmath>
#include <cstring>

namespace RtShadowDenoise {

	inline constexpr int MaxLayers = 8;		// lamps denoised at once (RtShadowDenoise.glsl layerSlot[2])

	// RtShadowDenoise.glsl `Pass` (std140, 304 bytes)
	struct DenoiseConstants {
		float size[4], forward[4], right[4], up[4], prevForward[4], prevRight[4], prevUp[4], cameraMove[4], jitter[4], full[4];
		float params[4], params2[4], light[4], layerSlot[2][4], layerReset[2][4], antiLag[4], history[4];
	};
	static_assert(sizeof(DenoiseConstants) == 19 * 16, "RtShadowDenoise.glsl Pass: 19 vec4s");

	struct Settings {
		float maxHistory = 32.0f;		// temporal accumulation (frames)
		float maxRadius = 32.0f;		// widest blur (traced pixels)
		float planeDistance = 0.02f;	// same-surface test: share of the view depth
		float stabilize = 4.0f;			// the result's own short history (frames, 0 off)
		float minRadius = 0.0f;			// blur at least this (pixels) where a shadow edge is (0: a contact shadow stays as hard as traced)
		float minRadiusNew = 4.0f;		// + this / history length (a new surface's first frames)
		float normalPower = 8.0f;		// tap weight: (n . n')^this
		float antiLag = 3.0f;			// history cut where it is further than this many sigmas (+ floor) from this frame's 5 x 5
		float antiLagFloor = 0.1f;
		float blurFrames = 4.0f;		// the blur radius x sqrt(this / history length) (0: always the full penumbra)
		float blurMinScale = 0.35f;		// ... but at least this share of it
		float minHistory = 1.0f;		// frames kept where the penumbra is under a pixel (a hard edge: each frame's ray is the answer)
		float historyRadius = 2.0f;		// penumbra radius (pixels) from which all of maxHistory is kept
		int core = 0;					// 0 our own (SIGMA-style), 1 AMD FidelityFX Shadows (RtShadowDenoiseFfx.hlsl)
		int debugView = 0;				// 1 raw, 2 temporal mean, 3 penumbra width, 4 history length, 5 before the stabilization
	};

	// A camera as the trace passes see it: forward (unit), right / P00, up / P11 (scene space = world minus the eye: only directions),
	// the eye in world units, the depth's jitter (screen uv)
	struct View {
		float forward[3] = {}, right[3] = {}, up[3] = {};
		double eye[3] = {};
		float jitter[2] = {};
		float farZ = 0.0f;
		bool valid = false;
	};

	inline void Put(float* d, const float* s, float w) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = w; }

	// The block for one run. mode 0 the sun (tanHalfAngle: the disc's), 1 lamps (layers + their slots / resets from Layers).
	// size: the traced rect; full: the depth / normal images' size it maps onto (the sun's HalfResolution); motion: ObjectMotion is
	// this frame's (the motion image is real).
	inline void Fill(DenoiseConstants& c, const View& now, const View& before, unsigned width, unsigned height, unsigned fullWidth,
		unsigned fullHeight, int mode, int layers, unsigned frame, const Settings& s, bool motion, float tanHalfAngle) {
		std::memset(&c, 0, sizeof(c));
		c.size[0] = (float)width; c.size[1] = (float)height; c.size[2] = 1.0f / (float)width; c.size[3] = 1.0f / (float)height;
		Put(c.forward, now.forward, now.farZ);
		Put(c.right, now.right, 0.0f);
		Put(c.up, now.up, 0.0f);
		Put(c.prevForward, before.forward, before.valid ? 1.0f : 0.0f);
		Put(c.prevRight, before.right, motion ? 1.0f : 0.0f);
		Put(c.prevUp, before.up, 0.0f);
		for (int i = 0; i < 3; i++) c.cameraMove[i] = before.valid ? (float)(now.eye[i] - before.eye[i]) : 0.0f;
		c.jitter[0] = now.jitter[0]; c.jitter[1] = now.jitter[1]; c.jitter[2] = before.jitter[0]; c.jitter[3] = before.jitter[1];
		c.full[0] = (float)fullWidth; c.full[1] = (float)fullHeight;
		c.params[0] = s.maxHistory; c.params[1] = s.maxRadius; c.params[2] = s.planeDistance; c.params[3] = s.stabilize;
		c.params2[0] = (float)mode; c.params2[1] = (float)layers; c.params2[2] = (float)frame; c.params2[3] = (float)s.debugView;
		c.light[0] = s.minRadius; c.light[1] = s.minRadiusNew; c.light[2] = s.normalPower; c.light[3] = tanHalfAngle;
		for (int k = 0; k < MaxLayers; k++) c.layerSlot[k / 4][k % 4] = -1.0f;
		c.antiLag[0] = s.antiLag; c.antiLag[1] = s.antiLagFloor; c.antiLag[2] = s.blurFrames; c.antiLag[3] = s.blurMinScale;
		c.history[0] = s.minHistory; c.history[1] = s.historyRadius; c.history[2] = (float)s.core;
	}

	// Which lamps get a layer. A lamp keeps its layer while it is still wanted (its history stays valid); a newly wanted lamp takes a free
	// layer and that layer's history restarts; so does a layer whose lamp moved more than MoveTolerance since the last frame.
	struct Candidate { int slot; float score; float position[3]; };
	struct Layers {
		int slot[MaxLayers];				// cube slot per layer, -1 none
		float position[MaxLayers][3];		// where its lamp was (world)
		bool reset[MaxLayers];				// this frame: restart its history
		int count = 0;						// layers in use (the highest used + 1)
		Layers() { Clear(); }
		void Clear() { for (int k = 0; k < MaxLayers; k++) { slot[k] = -1; reset[k] = true; position[k][0] = position[k][1] = position[k][2] = 0.0f; } count = 0; }

		// candidates: this frame's lamps with a score (higher = more worth denoising); wanted: up to this many (1-8)
		void Update(const Candidate* candidates, int n, int wanted, float moveTolerance = 1.0f) {
			if (wanted < 0) wanted = 0;
			if (wanted > MaxLayers) wanted = MaxLayers;
			// the best `wanted` candidates (n is small: up to 63)
			int best[MaxLayers]; int picked = 0;
			for (int round = 0; round < wanted; round++) {
				int at = -1;
				for (int i = 0; i < n; i++) {
					if (candidates[i].slot < 0 || !(candidates[i].score > 0.0f)) continue;
					bool taken = false;
					for (int j = 0; j < picked && !taken; j++) taken = best[j] == i;
					if (!taken && (at < 0 || candidates[i].score > candidates[at].score)) at = i;
				}
				if (at < 0) break;
				best[picked++] = at;
			}
			bool keep[MaxLayers] = {};
			for (int k = 0; k < MaxLayers; k++) reset[k] = false;
			// lamps already in a layer stay there
			bool placed[MaxLayers] = {};
			for (int j = 0; j < picked; j++) {
				const Candidate& c = candidates[best[j]];
				for (int k = 0; k < wanted; k++) {
					if (slot[k] != c.slot) continue;
					keep[k] = true; placed[j] = true;
					const float dx = c.position[0] - position[k][0], dy = c.position[1] - position[k][1], dz = c.position[2] - position[k][2];
					if (dx * dx + dy * dy + dz * dz > moveTolerance * moveTolerance) reset[k] = true;
					position[k][0] = c.position[0]; position[k][1] = c.position[1]; position[k][2] = c.position[2];
					break;
				}
			}
			for (int k = 0; k < MaxLayers; k++) if (!keep[k] || k >= wanted) { slot[k] = -1; }
			// the others take free layers
			for (int j = 0; j < picked; j++) {
				if (placed[j]) continue;
				const Candidate& c = candidates[best[j]];
				for (int k = 0; k < wanted; k++) {
					if (slot[k] >= 0) continue;
					slot[k] = c.slot; reset[k] = true;
					position[k][0] = c.position[0]; position[k][1] = c.position[1]; position[k][2] = c.position[2];
					break;
				}
			}
			count = 0;
			for (int k = 0; k < MaxLayers; k++) if (slot[k] >= 0) count = k + 1;
		}

		void Write(DenoiseConstants& c) const {
			c.params2[1] = (float)count;
			for (int k = 0; k < MaxLayers; k++) {
				c.layerSlot[k / 4][k % 4] = (float)slot[k];
				c.layerReset[k / 4][k % 4] = reset[k] ? 1.0f : 0.0f;
			}
		}
	};

}
