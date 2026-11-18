#pragma once

#include "BloodFlowBoxes.h"

// UNOFFICIAL 2026-11-12 BloodFlow Surface ([Shaders.BloodFlow.Surface], off by default): blood as a thin layer of liquid lying on
// the ground or wall, simulated on the GPU with plain D3D9 passes (no DXVK needed), instead of BloodFlow's painted pools and
// streams. What it is and how the shader works: src/hlsl/NewVegas/Effects/Includes/BloodSurface.hlsl.
// This file is the CPU side: which splats and bodies have a patch (a square fixed in the world with a cell of the state
// atlas), which patches still run, and the vertices of the two passes. Kept free of NVR's globals, as BloodFlowBoxes.h, so the
// preview (tests\blood_surface_preview.cpp) runs exactly this code.
//  - Offer: every item BloodFlow gathers. A body's pool gets a patch (a body that was already dead when it was first seen: its
//    patch runs fast to its end and shows dry); a splat gets one while it is fresh. An item this takes is left out of
//    BloodFlow's own passes.
//  - Update, once a frame: which patches are on screen; a patch starts when it is first seen (the ground's heights are read
//    from the depth buffer) and then runs on, seen or not, for its bleeding time plus FlowTime; then it is frozen (no more
//    steps, drawing only). A patch whose splat or body is gone stays for Lifetime seconds, or until its cell is needed.
//  - Pairs / BuildSim / AfterPair: one quad per running patch for a pair of steps (atlas copy 0 -> 1 -> 0); steps of a fixed
//    length, as many pairs a frame as the frame's time holds (so the blood runs as fast at 30 frames a second as at 120).
//  - BuildBoxes: a box round every patch on screen, for the drawing.
//  - Measure: once a patch is frozen its cell is read back, once, and its box cut down to where its blood really lies: a
//    frozen patch then costs only the pixels round its own blood (NextToMeasure: one patch at a time).
namespace BloodSurface {
	static const int CellSize = 256;	// texels across a patch
	static const int CellsPerRow = 8;
	static const int Cells = 32;		// the atlas: 8 x 4 cells = 2048 x 1024 texels, fp16 rgba, two copies (16 MB each)
	static const int AtlasWidth = CellSize * CellsPerRow, AtlasHeight = CellSize * (Cells / CellsPerRow);
	static constexpr float Step = 1.0f / 120.0f;	// seconds a step
	static const int MaxPairs = 3;		// pairs of steps a frame at most (frames longer than 1/20 s: the blood slows down)
	static const int FastPairs = 8;		// more pairs a frame for a patch catching up (an old body's)
	static const int FastPatches = 2;	// patches catching up at once
	static constexpr float FastStep = 0.05f;	// seconds a step of a patch catching up
	static constexpr float BoxUp = 0.75f;	// a patch's box and the heights it holds reach this x its half side above and below its plane

	struct Settings {
		bool	Enabled = false;
		bool	Splats = true;			// fresh splats
		bool	Pools = true;			// bodies' pools
		float	SplatAmount = 1.0f;		// x the blood a splat lays down
		float	PoolAmount = 1.0f;		// x the blood a body bleeds
		float	BleedTime = 8.0f;		// seconds a body bleeds
		float	FlowTime = 25.0f;		// seconds blood keeps moving after its source stops (it stiffens over the last 40 %)
		float	Viscosity = 1.0f;		// higher: thicker, slower
		float	Roughness = 0.15f;		// the ground's fine roughness (game units): breaks up the edge on flat floors
		float	Lifetime = 600.0f;		// seconds a patch stays after its splat or body is gone
		int		MaxPatches = Cells;
		bool	UseAlbedo = true;		// light from the picture over the albedo buffer (false: over a fixed grey, the fallback)
		int		DebugView = 0;
	};

	// The numbers behind the settings, and the shader's constants made from them (Includes/BloodSurface.hlsl).
	static constexpr float Mobility = 5000.0f;		// how easily blood moves at Viscosity 1
	static constexpr float SplatThickness = 0.12f;	// game units of blood where a splat's decal is solid, at SplatAmount 1
	static constexpr float PoolVolume = 0.5f;		// a pool's volume over its radius squared, at PoolAmount 1 (about its radius across on level ground)
	static constexpr float Soak = 0.0015f;			// game units of thickness that soak away a second
	static constexpr float Opaque = 0.1f;			// thickness at which blood hides what it lies on
	static constexpr float Pin = 0.15f;				// how much higher blood must stand to wet dry ground, at most
	inline D3DXVECTOR4 SizeConstants() { return D3DXVECTOR4(1.0f / AtlasWidth, 1.0f / AtlasHeight, (float)CellSize, (float)CellsPerRow); }
	inline D3DXVECTOR4 FlowConstants(const Settings& s) { return D3DXVECTOR4(SplatThickness * s.SplatAmount, PoolVolume * s.PoolAmount, s.BleedTime, Soak); }
	inline D3DXVECTOR4 LookConstants(const Settings& s) { return D3DXVECTOR4(Opaque, Pin, (float)s.DebugView, s.UseAlbedo ? 1.0f : 0.0f); }
	// A step of `seconds`; readDepth: the frame's first step (the patches on screen read their ground from the depth buffer).
	inline D3DXVECTOR4 SimConstants(const Settings& s, float seconds, bool readDepth) {	// (Atlas::StepOf, Kind 0's first step)
		return D3DXVECTOR4(seconds, readDepth ? 1.0f : 0.0f, Mobility / (std::max)(s.Viscosity, 0.05f), s.Roughness);
	}

	// The patch's two axes from its normal (Includes/BloodSurface.hlsl SurfAxes: the same sums).
	inline void Axes(const D3DXVECTOR3& n, D3DXVECTOR3& U, D3DXVECTOR3& V) {
		const D3DXVECTOR3 ref = fabsf(n.z) < 0.9f ? D3DXVECTOR3(0.0f, 0.0f, 1.0f) : D3DXVECTOR3(1.0f, 0.0f, 0.0f);
		D3DXVec3Cross(&U, &ref, &n);
		D3DXVec3Normalize(&U, &U);
		D3DXVec3Cross(&V, &n, &U);
	}

	struct Patch {
		bool		Used = false;
		unsigned long long Key = 0;	// its item's (BloodFlowEffect's slot key)
		D3DXVECTOR3	Center = D3DXVECTOR3(0, 0, 0);	// absolute world
		D3DXVECTOR3	Normal = D3DXVECTOR3(0, 0, 1);	// straight up (floors, slopes) or level (walls)
		float		Half = 100.0f;		// half its side, game units
		float		ItemU = 0.0f, ItemV = 0.0f;	// where the splat or body is on it, from the centre
		float		Radius = 10.0f, Seed = 0.0f, Tint = 0.0f;
		D3DXVECTOR3	DecalU = D3DXVECTOR3(0, 0, 0);
		int			DecalCode = 0;
		bool		Pool = false;
		bool		Old = false;		// its body was already dead when first seen: runs fast to its end, shown dry
		bool		Reset = true;		// its cell holds another patch's blood yet
		bool		Seen = false;		// has been on screen (its ground read): it runs from then on
		bool		Visible = false;	// its box touches the screen now
		bool		Frozen = false;		// done: drawn only
		bool		Fast = false;		// catching up this frame
		float		SimAge = 0.0f;		// seconds simulated
		float		EndAge = 30.0f;		// ... at which it is frozen
		float		FlowTime = 25.0f;
		double		FrozenAt = 0.0;		// (seconds, the caller's clock)
		double		WantedAt = 0.0;		// when its item was last offered
		float		Distance = 0.0f;	// from the eye
		// Its box, along its two axes and its normal from its centre (low and high corner): the whole patch while it runs, cut
		// down to its blood once it is frozen and measured (Measure). Empty: frozen without any blood left (not drawn).
		float		Box[6] = { -100.0f, -100.0f, -75.0f, 100.0f, 100.0f, 75.0f };
		bool		Measured = false, Empty = false;
		D3DXVECTOR3 Corner(int k) const { return D3DXVECTOR3(Box[(k & 1) ? 3 : 0], Box[(k & 2) ? 4 : 1], Box[(k & 4) ? 5 : 2]); }
	};

	struct Atlas {
		Patch	patches[Cells];
		int		made = 0, reclaimed = 0, refused = 0, shared = 0;	// (for the log)
		float	debt = 0.0f;	// seconds not stepped yet

		int Find(unsigned long long key) const {
			for (int i = 0; i < Cells; i++) if (patches[i].Used && patches[i].Key == key) return i;
			return -1;
		}
		bool Any() const { for (int i = 0; i < Cells; i++) if (patches[i].Used) return true; return false; }
		int Count() const { int n = 0; for (int i = 0; i < Cells; i++) n += patches[i].Used ? 1 : 0; return n; }
		int Running() const { int n = 0; for (int i = 0; i < Cells; i++) n += patches[i].Used && patches[i].Seen && !patches[i].Frozen ? 1 : 0; return n; }
		void Clear() { for (int i = 0; i < Cells; i++) patches[i] = Patch(); }

		// An item BloodFlow gathered (key: its slot key; now: seconds). True: the surface mode has it, BloodFlow's own passes
		// leave it out.
		bool Offer(unsigned long long key, const BloodFlowBoxes::Item& item, const D3DXVECTOR3& eye, double now, const Settings& s) {
			if (!s.Enabled || !(item.Pool ? s.Pools : s.Splats)) return false;
			int c = Find(key);
			if (c >= 0) { patches[c].WantedAt = now; return true; }
			const bool old = item.Age > 1e4f;
			// A splat that was already there (dried long ago), or one first met after it has stopped running: the game's own decal is
			// all there is of it.
			if (!item.Pool && (old || item.Age > s.FlowTime)) return true;
			// A splat on the middle of another splat's patch (shot after shot into one spot): that patch is its blood; a patch each
			// would only be drawn over one another.
			if (!item.Pool) for (int i = 0; i < Cells; i++) {
				const Patch& q = patches[i];
				if (!q.Used || q.Pool || (fabsf(q.Normal.z) > 0.5f) != (fabsf(item.Normal.z) > 0.7f * BloodFlowBoxes::Length(item.Normal))) continue;
				if (BloodFlowBoxes::Length(item.Center - q.Center) < 0.35f * q.Half) { shared++; return true; }
			}
			const int limit = (std::min)((std::max)(s.MaxPatches, 1), (int)Cells);
			for (int i = 0; i < limit && c < 0; i++) if (!patches[i].Used) c = i;
			if (c < 0) {
				// No free cell: the patch whose item has been gone longest, if any is gone; else the furthest frozen one, if it is
				// further than this item.
				const float distance = BloodFlowBoxes::Length(item.Center - eye);
				double oldest = now - 0.5;
				for (int i = 0; i < limit; i++) if (patches[i].WantedAt < oldest) { oldest = patches[i].WantedAt; c = i; }
				if (c < 0) {
					float furthest = distance;
					for (int i = 0; i < limit; i++) if (patches[i].Frozen && patches[i].Distance > furthest) { furthest = patches[i].Distance; c = i; }
				}
				if (c < 0) { refused++; return true; }
				reclaimed++;
			}
			// A body's new pool where an old patch of a pool still lies (the body was pushed, or forgotten and met again): that one goes.
			if (item.Pool) for (int i = 0; i < Cells; i++) {
				if (i == c || !patches[i].Used || !patches[i].Pool || patches[i].WantedAt >= now - 0.5) continue;
				if (BloodFlowBoxes::Length(patches[i].Center - item.Center) < patches[i].Radius) patches[i] = Patch();
			}
			Patch& p = patches[c];
			p = Patch();
			p.Used = true;
			p.Key = key;
			p.Pool = item.Pool;
			p.Old = old;
			p.Radius = item.Radius;
			p.Seed = item.Seed;
			p.Tint = item.Tint;
			p.DecalU = item.DecalU;
			p.DecalCode = item.DecalCode;
			p.WantedAt = now;
			p.FlowTime = s.FlowTime;
			p.EndAge = (item.Pool ? s.BleedTime : 0.5f) + s.FlowTime;
			p.Center = item.Center;
			// The patch's plane: level for anything floor-like (slopes are heights on it), upright for a wall, facing the eye. The
			// item's own normal is only a guess (against the way the blood was thrown), so it only picks between the two.
			float nl = BloodFlowBoxes::Length(item.Normal);
			const D3DXVECTOR3 guess = nl > 1e-4f ? item.Normal / nl : D3DXVECTOR3(0.0f, 0.0f, 1.0f);
			if (item.Pool || fabsf(guess.z) > 0.7f) {
				p.Normal = D3DXVECTOR3(0.0f, 0.0f, 1.0f);
				p.Half = item.Pool ? (std::min)((std::max)(item.Radius * 3.0f + 20.0f, 60.0f), 150.0f) : (std::min)((std::max)(item.Radius * 1.6f + 40.0f, 60.0f), 140.0f);
				// A pool on a slope the bones show: the patch reaches further downhill.
				const float steep = sqrtf(item.SlopeX * item.SlopeX + item.SlopeY * item.SlopeY);
				if (item.Pool && steep > 0.03f) {
					const float shift = p.Half * 0.35f * (std::min)(steep / 0.15f, 1.0f);
					const float dx = -item.SlopeX / steep * shift, dy = -item.SlopeY / steep * shift;
					p.Center += D3DXVECTOR3(dx, dy, item.SlopeX * dx + item.SlopeY * dy);
				}
			}
			else {
				p.Normal = D3DXVECTOR3(guess.x, guess.y, 0.0f);
				p.Normal /= BloodFlowBoxes::Length(p.Normal);
				const D3DXVECTOR3 toEye = eye - item.Center;
				if (D3DXVec3Dot(&p.Normal, &toEye) < 0.0f) p.Normal = -p.Normal;
				p.Half = (std::min)((std::max)(item.Radius * 1.6f + 40.0f, 60.0f), 140.0f);
				p.Center.z -= p.Half * 0.45f;	// most of it below the splat: blood runs down
			}
			D3DXVECTOR3 U, V;
			Axes(p.Normal, U, V);
			const D3DXVECTOR3 off = item.Center - p.Center;
			p.ItemU = D3DXVec3Dot(&off, &U);
			p.ItemV = D3DXVec3Dot(&off, &V);
			p.Box[0] = p.Box[1] = -p.Half; p.Box[3] = p.Box[4] = p.Half;
			p.Box[2] = -p.Half * BoxUp; p.Box[5] = p.Half * BoxUp;
			made++;
			return true;
		}

		// Once a frame, after the offers. eye: the camera's absolute position; origin, viewProj: as BloodFlowBoxes::Build; now:
		// seconds (the clock Offer was given); maxDistance: BloodFlow's (patches fade out over its last fifth).
		void Update(const D3DXVECTOR3& eye, const D3DXVECTOR3& origin, const D3DXMATRIX& viewProj, double now, const Settings& s) {
			int fast = 0;
			for (int i = 0; i < Cells; i++) {
				Patch& p = patches[i];
				if (!p.Used) continue;
				if (now - p.WantedAt > (double)s.Lifetime + 10.0) { p = Patch(); continue; }
				p.Distance = BloodFlowBoxes::Length(p.Center - eye);
				D3DXVECTOR3 U, V;
				Axes(p.Normal, U, V);
				const D3DXVECTOR3 center = p.Center - eye;
				int outside[6] = {};
				for (int k = 0; k < 8; k++) {
					const D3DXVECTOR3 at = p.Corner(k);
					const D3DXVECTOR3 q = center + U * at.x + V * at.y + p.Normal * at.z + origin;
					const D3DXVECTOR4 q4(q.x, q.y, q.z, 1.0f);
					D3DXVECTOR4 c;
					D3DXVec4Transform(&c, &q4, &viewProj);
					if (c.x < -c.w) outside[0]++;
					if (c.x > c.w) outside[1]++;
					if (c.y < -c.w) outside[2]++;
					if (c.y > c.w) outside[3]++;
					if (c.w <= 0.0f) outside[4]++;
					if (c.z > c.w) outside[5]++;
				}
				p.Visible = !p.Empty;
				for (int f = 0; f < 6; f++) if (outside[f] == 8) p.Visible = false;
				if (p.Visible) p.Seen = true;
				p.Fast = false;
				if (p.Old && p.Seen && !p.Frozen && fast < FastPatches) { p.Fast = true; fast++; }
			}
		}

		// How many pairs of steps this frame of `seconds` takes: `normal` for every running patch (0 to MaxPairs, the time left
		// over is carried to the next frame), and FastPairs more after them for the patches catching up, if any. Returns their sum;
		// pair i is then drawn with Kind(i, normal).
		int Pairs(float seconds, int& normal) {
			debt += (std::min)((std::max)(seconds, 0.0f), 0.25f);
			normal = (std::min)((int)(debt / (2.0f * Step)), (int)MaxPairs);
			debt = (std::min)(debt - (float)normal * 2.0f * Step, 2.0f * Step);
			bool anyFast = false;
			for (int i = 0; i < Cells; i++) anyFast |= patches[i].Used && patches[i].Fast && !patches[i].Reset;
			return normal + (anyFast ? (int)FastPairs : 0);
		}
		// A pair's kind: 0 the frame's first (every running patch; a new patch's cell starts from nothing; the patches on screen read
		// their ground from the depth buffer on its first step), 1 a further one for every running patch, 2 one for the patches
		// catching up only. Seconds a step of it takes: StepOf.
		static int Kind(int pair, int normal) { return pair >= normal ? 2 : (pair == 0 ? 0 : 1); }
		static float StepOf(int kind) { return kind == 2 ? FastStep : Step; }
		bool Runs(const Patch& p, int kind) const { return p.Used && p.Seen && !p.Frozen && (kind != 2 || (p.Fast && !p.Reset)); }

		// The quads of one step (0 or 1) of a pair of `kind`, 6 vertices each into out (room for Cells x 6); returns the
		// triangle count.
		int BuildSim(int kind, int step, const D3DXVECTOR3& eye, VolumetricSmokeQuads::Vertex* out) const {
			const float seconds = StepOf(kind);
			int triangles = 0;
			for (int i = 0; i < Cells; i++) {
				const Patch& p = patches[i];
				if (!Runs(p, kind)) continue;
				VolumetricSmokeQuads::Vertex data = {};
				const D3DXVECTOR3 center = p.Center - eye;
				const float code = (float)i + 64.0f * ((p.Pool ? 1.0f : 0.0f) + (p.Reset && kind == 0 && step == 0 ? 2.0f : 0.0f) + (p.Visible ? 4.0f : 0.0f));
				const float mobility = (std::min)((std::max)((p.EndAge - p.SimAge) / (0.4f * (std::max)(p.FlowTime, 1.0f)), 0.0f), 1.0f);
				const float a[4] = { center.x, center.y, center.z, code };
				const float b[4] = { p.Normal.x, p.Normal.y, p.Normal.z, p.Half };
				const float c[4] = { p.SimAge + (float)step * seconds, mobility, floorf(p.Seed * 4095.0f + 0.5f) + 4096.0f * (float)p.DecalCode, p.Radius };
				const float d[4] = { p.DecalU.x, p.DecalU.y, p.DecalU.z, p.Tint };
				memcpy(data.a, a, 16); memcpy(data.b, b, 16); memcpy(data.c, c, 16); memcpy(data.d, d, 16);
				// D3D9 puts pixel centres on whole coordinates: the quad from x0 to x0 + N covers texels x0 .. x0 + N - 1.
				const float x0 = (float)((i % CellsPerRow) * CellSize), y0 = (float)((i / CellsPerRow) * CellSize);
				const float u0 = x0 / AtlasWidth * 2.0f - 1.0f, u1 = (x0 + CellSize) / AtlasWidth * 2.0f - 1.0f;
				const float v0 = 1.0f - y0 / AtlasHeight * 2.0f, v1 = 1.0f - (y0 + CellSize) / AtlasHeight * 2.0f;
				const float corner[4][2] = { { u0, v0 }, { u1, v0 }, { u1, v1 }, { u0, v1 } };
				static const int order[6] = { 0, 1, 2, 0, 2, 3 };
				for (int t = 0; t < 6; t++) {
					VolumetricSmokeQuads::Vertex& v = out[triangles * 3 + t];
					v = data;
					v.x = corner[order[t]][0]; v.y = corner[order[t]][1]; v.z = p.ItemU; v.w = p.ItemV;
				}
				triangles += 2;
			}
			return triangles;
		}

		// After a pair of steps of `kind` was drawn.
		void AfterPair(int kind, double now) {
			for (int i = 0; i < Cells; i++) {
				Patch& p = patches[i];
				if (!Runs(p, kind)) continue;
				if (kind == 0) p.Reset = false;
				p.SimAge += 2.0f * StepOf(kind);
				if (p.SimAge >= p.EndAge) { p.Frozen = true; p.FrozenAt = now; }
			}
		}

		// The next frozen patch whose blood has not been measured yet (-1: none).
		int NextToMeasure() const {
			for (int i = 0; i < Cells; i++) if (patches[i].Used && patches[i].Frozen && !patches[i].Measured) return i;
			return -1;
		}
		// A frozen patch's cell, read back from the atlas's first copy (texels: CellSize rows of CellSize fp16 rgba texels, `pitch`
		// bytes a row): its box becomes the box round its blood (two texels and the surface test's room to spare).
		void Measure(int cell, const void* texels, int pitch) {
			Patch& p = patches[cell];
			p.Measured = true;
			int x0 = CellSize, y0 = CellSize, x1 = -1, y1 = -1;
			float z0 = 1e9f, z1 = -1e9f;
			static float row[CellSize * 4];
			for (int y = 0; y < CellSize; y++) {
				D3DXFloat16To32Array(row, (const D3DXFLOAT16*)((const char*)texels + (size_t)y * pitch), CellSize * 4);
				for (int x = 0; x < CellSize; x++) {
					if (!(row[x * 4] > 0.004f)) continue;
					if (x < x0) x0 = x;
					if (x > x1) x1 = x;
					if (y < y0) y0 = y;
					if (y > y1) y1 = y;
					if (row[x * 4 + 3] > 0.4f) { z0 = (std::min)(z0, row[x * 4 + 2]); z1 = (std::max)(z1, row[x * 4 + 2]); }
				}
			}
			if (x1 < 0) { p.Empty = true; return; }
			if (z0 > z1) { z0 = -p.Half * BoxUp; z1 = p.Half * BoxUp; }
			const float texel = 2.0f * p.Half / CellSize;
			p.Box[0] = (std::max)(-p.Half, ((float)x0 - 2.0f) * texel - p.Half); p.Box[3] = (std::min)(p.Half, ((float)x1 + 3.0f) * texel - p.Half);
			p.Box[1] = (std::max)(-p.Half, ((float)y0 - 2.0f) * texel - p.Half); p.Box[4] = (std::min)(p.Half, ((float)y1 + 3.0f) * texel - p.Half);
			p.Box[2] = (std::max)(-p.Half * BoxUp, z0 - 6.0f); p.Box[5] = (std::min)(p.Half * BoxUp, z1 + 6.0f);
		}

		// A patch's cell in the atlas, in texels (for clearing a new patch's cell in both copies).
		static void CellRect(int cell, RECT& r) {
			r.left = (cell % CellsPerRow) * CellSize; r.top = (cell / CellsPerRow) * CellSize;
			r.right = r.left + CellSize; r.bottom = r.top + CellSize;
		}

		// A box round every patch on screen that has been seen, 36 vertices each into out (room for Cells x 36): position xyz a
		// corner along the patch's axes and normal, w its face (SurfaceBoxVS). Returns the triangle count; bounds as
		// BloodFlowBoxes::Build (the boxes' screen area in UV).
		int BuildBoxes(const D3DXVECTOR3& eye, const D3DXVECTOR3& origin, const D3DXMATRIX& viewProj, double now, float maxDistance, const Settings& s,
			VolumetricSmokeQuads::Vertex* out, float bounds[4]) const {
			float minU = 1.0f, minV = 1.0f, maxU = 0.0f, maxV = 0.0f;
			int triangles = 0;
			for (int i = 0; i < Cells; i++) {
				const Patch& p = patches[i];
				if (!p.Used || !p.Seen || !p.Visible || p.Reset) continue;
				float fade = 1.0f - (std::min)((std::max)((p.Distance - 0.8f * maxDistance) / (0.2f * maxDistance), 0.0f), 1.0f);
				fade *= (std::min)((std::max)(((float)s.Lifetime + 10.0f - (float)(now - p.WantedAt)) / 10.0f, 0.0f), 1.0f);
				// (An old body's pool shows once it has all but run to its end.)
				if (p.Old && !p.Frozen) fade *= (std::min)((std::max)((p.SimAge / p.EndAge - 0.7f) / 0.3f, 0.0f), 1.0f);
				if (fade <= 0.0f) continue;
				D3DXVECTOR3 U, V;
				Axes(p.Normal, U, V);
				const D3DXVECTOR3 center = p.Center - eye;
				bool behind = false;
				D3DXVECTOR4 clip[8];
				for (int k = 0; k < 8; k++) {
					const D3DXVECTOR3 at = p.Corner(k);
					const D3DXVECTOR3 q = center + U * at.x + V * at.y + p.Normal * at.z + origin;
					const D3DXVECTOR4 q4(q.x, q.y, q.z, 1.0f);
					D3DXVec4Transform(&clip[k], &q4, &viewProj);
					if (clip[k].w <= 0.0f) behind = true;
				}
				if (behind) { minU = minV = 0.0f; maxU = maxV = 1.0f; }
				else for (int k = 0; k < 8; k++) {
					const float su = 0.5f + 0.5f * clip[k].x / clip[k].w, sv = 0.5f - 0.5f * clip[k].y / clip[k].w;
					if (su < minU) minU = su;
					if (su > maxU) maxU = su;
					if (sv < minV) minV = sv;
					if (sv > maxV) maxV = sv;
				}
				VolumetricSmokeQuads::Vertex data = {};
				const float code = (float)i + 64.0f * (p.Pool ? 1.0f : 0.0f);
				// (Drying goes on after it is frozen: the seconds since; an old body's blood is dry.)
				const float since = (p.Frozen ? (float)(now - p.FrozenAt) : 0.0f) + (p.Old ? 1e5f : 0.0f);
				const float a[4] = { center.x, center.y, center.z, code };
				const float b[4] = { p.Normal.x, p.Normal.y, p.Normal.z, p.Half };
				const float c[4] = { since, fade, floorf(p.Seed * 4095.0f + 0.5f) + 4096.0f * (float)p.DecalCode, p.Radius };
				const float d[4] = { p.DecalU.x, p.DecalU.y, p.DecalU.z, p.Tint };
				memcpy(data.a, a, 16); memcpy(data.b, b, 16); memcpy(data.c, c, 16); memcpy(data.d, d, 16);
				static const int faces[6][4] = { { 0, 2, 6, 4 }, { 1, 5, 7, 3 }, { 0, 4, 5, 1 }, { 2, 3, 7, 6 }, { 0, 1, 3, 2 }, { 4, 6, 7, 5 } };
				for (int f = 0; f < 6; f++) {
					static const int order[6] = { 0, 1, 2, 0, 2, 3 };
					for (int t = 0; t < 6; t++) {
						VolumetricSmokeQuads::Vertex& v = out[triangles * 3 + t];
						v = data;
						const D3DXVECTOR3 at = p.Corner(faces[f][order[t]]);
						v.x = at.x; v.y = at.y; v.z = at.z; v.w = (float)f;
					}
					triangles += 2;
				}
			}
			bounds[0] = minU; bounds[1] = minV; bounds[2] = maxU; bounds[3] = maxV;
			return triangles;
		}
	};
}
