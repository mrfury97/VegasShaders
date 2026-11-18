#pragma once
// UNOFFICIAL 2026-11-14 LampTableFast (fd-lamptable; docs/next/FD-LAMPTABLE-RESULTS.md, FULL-DEFERRED-RENDERER-PLAN.md phase 0 question 4):
// the plain-data part of ManyLamps::BuildTable / BinFroxels (the lamp table and its froxel lists), with no engine or D3D types, so the
// game and tests/lamp_table_bench.cpp call the SAME code. Two binning paths:
//  - BinReference: the 2026-10-30 .. 11-01 binning exactly as it was in ManyLamps.h (moved here, not changed): an unordered_map of
//    stable keys, lamp-major scattered writes into the locked texture (one float per froxel per lamp, then every froxel's count).
//  - BinFast: the same per-lamp maths (the shared ToView / LampKey / Rect below), the same sort, the same over-the-cap rule, but the
//    lists are binned into cached scratch first and written out once, froxel by froxel, in whole 16-byte texels (the locked texture is
//    write-combined memory on native D3D9: scattered 4-byte writes and any read of it are slow there), stable keys in flat maps.
// The shader reads a froxel's count (texel 0 .x) and its first `count` entries (LightCommon.hlsl / DeferredLamps.hlsl DLLamp); those are
// the same on both paths (ListsEqual, the bench's and the in-game LampTableCheck's test). BinFast writes 0 into the unused components of
// a list's last texel (the reference left whatever the discarded texture held; the shaders' dot() select multiplies them by 0).
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <utility>

#ifndef LT_MARK
#define LT_MARK(k)
#endif
namespace LampTableCore {

	enum { TableLamps = 1024, TableTexels = 2 * TableLamps,
		TilePx = 64, MaxTilesX = 40, MaxTilesY = 24, Slices = 8, MaxFroxels = MaxTilesX * MaxTilesY * Slices,
		MaxListTexels = 16, MaxListLamps = MaxListTexels * 4 - 1 };

	// One scene lamp as the walk of ShadowSceneNode's list read it (ManyLamps::BuildTable): world position, reach (Spec.r), diffuse,
	// dimmer (raw), LOD dimmer, and the flags the table needs.
	struct SceneLamp {
		const void* id;		// the ShadowSceneLight
		float pos[3], reach;
		float diff[3], dimmer, lod;
		bool active;		// GetNumberOfActiveNonShadowLights' test: the froxel lists and ActiveLamps
		bool hidden;		// app-culled (AllLamps leaves it out)
	};

	// Open-addressed pointer -> value map with a generation stamp (clear is O(1), no allocation): TableIndex and the stable keys.
	// Holds at most Capacity / 2 keys (the table holds at most TableLamps = 1024).
	template <class V> class PtrMap {
	public:
		enum { Capacity = 4096 };
		PtrMap() { memset(slots, 0, sizeof(slots)); }
		void clear() { if (++gen == 0) { memset(slots, 0, sizeof(slots)); gen = 1; } count = 0; }
		size_t size() const { return count; }
		const V* find(const void* key) const {
			for (uint32_t h = Hash(key);; h = (h + 1) & (Capacity - 1)) {
				const Slot& s = slots[h];
				if (s.gen != gen) return nullptr;
				if (s.key == key) return &s.value;
			}
		}
		// false when the key is there already (its value is left as it was) or the map is full
		bool insert(const void* key, const V& value) {
			if (count >= Capacity / 2) return false;
			for (uint32_t h = Hash(key);; h = (h + 1) & (Capacity - 1)) {
				Slot& s = slots[h];
				if (s.gen != gen) { s.key = key; s.value = value; s.gen = gen; count++; return true; }
				if (s.key == key) return false;
			}
		}
		template <class F> void forEach(F&& f) const { for (const Slot& s : slots) if (s.gen == gen) f(s.key, s.value); }	// (2026-11-15: LampTableCheck's seeding)
	private:
		struct Slot { const void* key; V value; uint32_t gen; };
		static uint32_t Hash(const void* p) {
			uint32_t x = (uint32_t)(uintptr_t)p;
			x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16;
			return x & (Capacity - 1);
		}
		Slot slots[Capacity];
		uint32_t gen = 1;
		size_t count = 0;
	};

	// --- Row 0: the lamps (2 texels a lamp: position, reach | colour) ------------------------------------------------------------------
	// The colour as ManyLamps::Put / BuildTable make it: dimmer capped at 1 without HDR (BSShaderManager::bHDR), times the LOD dimmer.
	inline void LampRaw(const SceneLamp& l, bool hdr, float raw[4]) {
		float dimmer = l.dimmer;
		if (!hdr && dimmer > 1.0f) dimmer = 1.0f;
		raw[0] = l.diff[0] * dimmer * l.lod; raw[1] = l.diff[1] * dimmer * l.lod; raw[2] = l.diff[2] * dimmer * l.lod; raw[3] = 0.0f;
	}
	// The decoded colours of last frame per table slot: a lamp whose raw colour has the same bits gets the same decode (powf is
	// deterministic; LinearPreDecode's PreDecodeCache returns what powf returned), without a decode.
	struct ColourMemo {
		float raw[TableLamps][3];
		float out[TableLamps][4];
		bool valid[TableLamps];
		float gamma = -1.0f;
		ColourMemo() { memset(valid, 0, sizeof(valid)); }
	};
	// The lamps into row (TableTexels x 4 floats, the rest zeroed). decode(out, in, gamma) is LinearPreDecode::DecodeFast in the game.
	// memo may be null (the reference: decode every lamp).
	template <class Decode> inline void FillRow(float* row, const SceneLamp* lamps, int n, bool hdr, bool linear, float gamma, Decode&& decode, ColourMemo* memo) {
		if (memo && memcmp(&memo->gamma, &gamma, sizeof(float))) { memset(memo->valid, 0, sizeof(memo->valid)); memo->gamma = gamma; }
		for (int k = 0; k < n; k++) {
			const SceneLamp& l = lamps[k];
			float* p = row + k * 8;
			p[0] = l.pos[0]; p[1] = l.pos[1]; p[2] = l.pos[2]; p[3] = l.reach;
			float raw[4];
			LampRaw(l, hdr, raw);
			if (!linear) { memcpy(p + 4, raw, sizeof(raw)); continue; }
			if (memo) {
				if (memo->valid[k] && !memcmp(memo->raw[k], raw, 3 * sizeof(float))) { memcpy(p + 4, memo->out[k], 4 * sizeof(float)); continue; }
				decode(memo->out[k], raw, gamma);
				memcpy(memo->raw[k], raw, 3 * sizeof(float));
				memo->valid[k] = true;
				memcpy(p + 4, memo->out[k], 4 * sizeof(float));
			}
			else decode(p + 4, raw, gamma);
		}
		if (n < TableLamps) memset(row + n * 8, 0, (size_t)(TableLamps - n) * 8 * sizeof(float));
	}

	// --- The froxel lists (rows 1+) --------------------------------------------------------------------------------------------------
	// The world camera and viewport of the frame's first world draw.
	struct View {
		unsigned width = 0, height = 0;	// the viewport (D3DVIEWPORT9)
		float right[3] = {}, up[3] = {}, fwd[3] = {};	// InvViewMatrix rows 1-3
		float cam[3] = {};				// CameraPosition
		float tanX = 1.0f, tanY = 1.0f;	// |InvProjMatrix _11|, |_22|
		int listTexels = 8;				// ManyLamps::ListTexelsNow (8, or 16 with DeferredLamps)
		int tilePx = TilePx;			// UNOFFICIAL 2026-11-15 the tile's size in pixels (TileSizeFor: 64, larger past 2,560 x 1,536)
		bool stableOrder = true;		// [Shaders.ShadowsInteriors.Deferred] StableOrder
	};
	struct BinStats { uint64_t lamps = 0, entries = 0, over = 0; };
	struct BinLamp { int index; float key; float x, y, z, r; const void* id; };

	// UNOFFICIAL 2026-11-15 (fd-lightpass, docs/next/FD-LIGHTPASS-RESULTS.md section 6) the tile size for a viewport: 64 pixels while the
	// screen fits the 40 x 24 grid (2,560 x 1,536), else the next multiple of 64 that fits (128 at 3840 x 2160, 192 past 5,120 x 3,072). With a
	// fixed 64 the grid was capped and the shaders' tile index ran past it on bigger screens: its right / bottom part read other tiles' lists.
	inline int TileSizeFor(unsigned width, unsigned height) {
		int s = TilePx;
		while ((int)(width + s - 1) / s > (int)MaxTilesX || (int)(height + s - 1) / s > (int)MaxTilesY) s += TilePx;
		return s;
	}
	inline int TilesX(const View& v) { return (std::min)((int)MaxTilesX, (int)(v.width + v.tilePx - 1) / v.tilePx); }
	inline int TilesY(const View& v) { return (std::min)((int)MaxTilesY, (int)(v.height + v.tilePx - 1) / v.tilePx); }
	inline int SliceOf(float z) { return z < 64.0f ? 0 : (std::min)((int)Slices - 1, (int)floorf(log2f(z)) - 5); }
	// BinFast's equals of (int)floorf(x) and SliceOf without the CRT calls, the same for EVERY float (tests/lamp_table_bench.cpp
	// --exhaustive checks all 2^32 inputs): a truncation stepped down for negatives (left alone at the conversion's INT_MIN "indefinite",
	// as floorf's result converts there too), and the exponent of z instead of floor(log2(z)), except where log2f rounds up to the next
	// power of two (the top 255 mantissas below it), infinity and NaN: those still take log2f.
	inline int FloorInt(float x) { int i = (int)x; if (i != INT32_MIN && x < (float)i) i--; return i; }
	inline int SliceFast(float z) {
		if (z < 64.0f) return 0;
		uint32_t bits; memcpy(&bits, &z, sizeof(bits));
		const int e = (int)((bits >> 23) & 0xFF);
		if (e == 0xFF || (bits & 0x7FFFFF) >= 0x7FFF00) return SliceOf(z);
		return (std::min)((int)Slices - 1, e - 127 - 5);
	}

	// The lamp in view space; false when it is wholly behind the camera (or has no reach).
	inline bool ToView(const View& v, const SceneLamp& l, float& x, float& y, float& z) {
		const float dx = l.pos[0] - v.cam[0], dy = l.pos[1] - v.cam[1], dz = l.pos[2] - v.cam[2];
		x = dx * v.right[0] + dy * v.right[1] + dz * v.right[2];
		y = dx * v.up[0] + dy * v.up[1] + dz * v.up[2];
		z = dx * v.fwd[0] + dy * v.fwd[1] + dz * v.fwd[2];
		const float r = l.reach;
		return !(r <= 0.0f || z + r <= 1.0f);
	}
	inline float Brightness(const SceneLamp& l) { return (l.diff[0] + l.diff[1] + l.diff[2]) * l.dimmer * l.lod; }
	inline float DistanceKey(const SceneLamp& l, float x, float y, float z) {	// StableOrder off: brightness x reach / distance
		const float r = l.reach;
		const float dist = (std::max)(sqrtf(x * x + y * y + z * z), 0.25f * r + 1.0f);
		return Brightness(l) * r / dist;
	}
	inline bool KeptKey(float key, float kept) { return fabsf(key - kept) < 0.1f * kept; }	// within 10 %: the key of the frame before stays

	// The froxels the lamp's sphere may touch; false = off screen. A conservative screen rect (each side taken at the depth that makes it
	// widest) once the sphere is wholly past 8 units, else the whole screen; slices from its depth range. x0 > x1 is possible (no tiles).
	// Fast: FloorInt / SliceFast for floorf / SliceOf (the same results; BinFast). Off: BinReference, as it was.
	template <bool Fast = false> inline bool Rect(const View& v, int tilesX, int tilesY, const BinLamp& l, int& x0, int& x1, int& y0, int& y1, int& s0, int& s1) {
		auto floorInt = [](float x) { return Fast ? FloorInt(x) : (int)floorf(x); };
		auto slice = [](float z) { return Fast ? SliceFast(z) : SliceOf(z); };
		const float zmin = l.z - l.r, zmax = l.z + l.r;
		x0 = 0; x1 = tilesX - 1; y0 = 0; y1 = tilesY - 1;
		if (zmin > 8.0f) {
			const float lx = l.x - l.r, hx = l.x + l.r, ly = l.y - l.r, hy = l.y + l.r;
			const float nx0 = lx / ((lx < 0.0f ? zmin : zmax) * v.tanX), nx1 = hx / ((hx > 0.0f ? zmin : zmax) * v.tanX);
			const float ny0 = ly / ((ly < 0.0f ? zmin : zmax) * v.tanY), ny1 = hy / ((hy > 0.0f ? zmin : zmax) * v.tanY);
			if (nx0 > 1.0f || nx1 < -1.0f || ny0 > 1.0f || ny1 < -1.0f) return false;
			x0 = (std::max)(0, floorInt((nx0 * 0.5f + 0.5f) * v.width / (float)v.tilePx));
			x1 = (std::min)(tilesX - 1, floorInt((nx1 * 0.5f + 0.5f) * v.width / (float)v.tilePx));
			y0 = (std::max)(0, floorInt((0.5f - ny1 * 0.5f) * v.height / (float)v.tilePx));
			y1 = (std::min)(tilesY - 1, floorInt((0.5f - ny0 * 0.5f) * v.height / (float)v.tilePx));
		}
		s0 = slice((std::max)(zmin, 0.0f)); s1 = slice(zmax);
		return true;
	}
	inline bool ByKeyStable(const BinLamp& a, const BinLamp& b) { return a.key != b.key ? a.key > b.key : (uintptr_t)a.id < (uintptr_t)b.id; }
	inline bool ByKey(const BinLamp& a, const BinLamp& b) { return a.key > b.key; }	// a full list drops the least important

	// serial: one more for every binning of the game (whichever path made it), so a path that did not run last frame keeps no key.
	struct RefState { std::unordered_map<const void*, std::pair<float, uint32_t>> keys; std::vector<uint8_t> counts; std::vector<BinLamp> lamps; };

	// The binning as ManyLamps::BinFroxels did it until 2026-11-14. lists = row 1 of the table, pitch in floats. active: table indices.
	inline bool BinReference(const View& v, const SceneLamp* table, const int* active, int nActive, float* lists, int pitch, uint32_t serial, RefState& st, BinStats& stats) {
		const int tilesX = TilesX(v), tilesY = TilesY(v);
		if (tilesX <= 0 || tilesY <= 0) return false;
		const uint32_t stableFrame = serial;
		std::vector<BinLamp>& lamps = st.lamps;
		lamps.clear();
		for (int a = 0; a < nActive; a++) {
			const SceneLamp& l = table[active[a]];
			float x, y, z;
			if (!ToView(v, l, x, y, z)) continue;
			float key;
			if (v.stableOrder) {
				key = Brightness(l) * l.reach;
				auto& kept = st.keys[l.id];
				if (kept.second && kept.second + 1 >= stableFrame && KeptKey(key, kept.first)) key = kept.first;
				kept = { key, stableFrame };
			}
			else key = DistanceKey(l, x, y, z);
			lamps.push_back({ active[a], key, x, y, z, l.reach, l.id });
		}
		if (v.stableOrder) {
			if ((stableFrame & 1023) == 0) for (auto it = st.keys.begin(); it != st.keys.end();) it = stableFrame - it->second.second > 1024 ? st.keys.erase(it) : std::next(it);
			std::sort(lamps.begin(), lamps.end(), ByKeyStable);
		}
		else {
			if (!st.keys.empty()) st.keys.clear();
			std::sort(lamps.begin(), lamps.end(), ByKey);
		}
		const int listTexels = v.listTexels, listLamps = listTexels * 4 - 1;
		st.counts.assign((size_t)tilesX * tilesY * Slices, 0);
		auto put = [&](int froxel, int k, float value) {	// entry k of the froxel's list (-1 = its count)
			const int texel = froxel * listTexels + (k + 1) / 4, comp = (k + 1) % 4;
			lists[(texel / TableTexels) * pitch + (texel % TableTexels) * 4 + comp] = value;
		};
		for (const BinLamp& l : lamps) {
			int x0, x1, y0, y1, s0, s1;
			if (!Rect(v, tilesX, tilesY, l, x0, x1, y0, y1, s0, s1)) continue;
			stats.lamps++;
			for (int sl = s0; sl <= s1; sl++)
				for (int ty = y0; ty <= y1; ty++)
					for (int tx = x0; tx <= x1; tx++) {
						const int froxel = (sl * tilesY + ty) * tilesX + tx;
						uint8_t& c = st.counts[froxel];
						if (c >= listLamps) { stats.over++; continue; }
						put(froxel, c, (float)l.index);
						c++;
						stats.entries++;
					}
		}
		for (size_t f = 0; f < st.counts.size(); f++) put((int)f, -1, (float)st.counts[f]);
		return true;
	}

	// BinFast's state: stable keys of the binning before (prev) and this one (cur), flat; the scratch lists; reused every frame.
	struct FastState {
		PtrMap<float> keys[2];
		int cur = 0;
		uint32_t lastSerial = 0;
		std::vector<BinLamp> lamps, sorted;
		struct Order { uint64_t k; int pos; };
		std::vector<Order> order;
		uint8_t counts[MaxFroxels];
		uint16_t entries[MaxFroxels * MaxListLamps];
		FastState() { lamps.reserve(TableLamps); sorted.reserve(TableLamps); order.reserve(TableLamps); }
	};

	// The same lists as BinReference, written froxel by froxel in whole texels (only the texels a list uses: its count + entries).
	inline bool BinFast(const View& v, const SceneLamp* table, const int* active, int nActive, float* lists, int pitch, uint32_t serial, FastState& st, BinStats& stats) {
		const int tilesX = TilesX(v), tilesY = TilesY(v);
		if (tilesX <= 0 || tilesY <= 0) return false;
		LT_MARK(0);
		// the keys of the binning before count only when it was the one just before (as BinReference's frame stamps)
		PtrMap<float>& prev = st.keys[st.cur];
		PtrMap<float>& cur = st.keys[st.cur ^ 1];
		if (!v.stableOrder || st.lastSerial + 1 != serial) prev.clear();
		cur.clear();
		st.lastSerial = serial;
		std::vector<BinLamp>& lamps = st.lamps;
		lamps.clear();
		for (int a = 0; a < nActive; a++) {
			const SceneLamp& l = table[active[a]];
			float x, y, z;
			if (!ToView(v, l, x, y, z)) continue;
			float key;
			if (v.stableOrder) {
				key = Brightness(l) * l.reach;
				const float* kept = prev.find(l.id);
				if (kept && KeptKey(key, *kept)) key = *kept;
				cur.insert(l.id, key);
			}
			else key = DistanceKey(l, x, y, z);
			lamps.push_back({ active[a], key, x, y, z, l.reach, l.id });
		}
		st.cur ^= 1;
		LT_MARK(1);
		// StableOrder (the default) is a total order (key, then the lamp's address): sorted as one 64-bit number a lamp, which any correct
		// sort puts in the same order as BinReference's std::sort. Not for NaN keys (no order at all) or 64-bit addresses (the tests): there,
		// and with StableOrder off (ties left to std::sort), the same std::sort as BinReference.
		bool composite = v.stableOrder && sizeof(void*) == 4;
		if (composite) {
			st.order.clear();
			for (int i = 0; i < (int)lamps.size(); i++) {
				float key = lamps[i].key;
				if (key != key) { composite = false; break; }
				if (key == 0.0f) key = 0.0f;	// -0 = +0, as the comparison has them
				uint32_t b; memcpy(&b, &key, sizeof(b));
				const uint32_t ascending = (b & 0x80000000u) ? ~b : (b | 0x80000000u);	// larger key, larger number
				st.order.push_back({ ((uint64_t)~ascending << 32) | (uint32_t)(uintptr_t)lamps[i].id, i });
			}
		}
		const BinLamp* ordered;
		if (composite) {
			std::sort(st.order.begin(), st.order.end(), [](const FastState::Order& a, const FastState::Order& b) { return a.k < b.k; });
			st.sorted.resize(lamps.size());
			for (size_t i = 0; i < st.order.size(); i++) st.sorted[i] = lamps[st.order[i].pos];
			ordered = st.sorted.data();
		}
		else {
			std::sort(lamps.begin(), lamps.end(), v.stableOrder ? ByKeyStable : ByKey);
			ordered = lamps.data();
		}
		const size_t count = lamps.size();
		LT_MARK(2);
		const int listTexels = v.listTexels, listLamps = listTexels * 4 - 1;
		const int froxels = tilesX * tilesY * Slices;
		uint8_t* const counts = st.counts;
		uint16_t* const entries = st.entries;
		memset(counts, 0, (size_t)froxels);
		uint64_t binned = 0, made = 0, over = 0;	// (locals: the uint8_t stores may alias anything)
		for (size_t i = 0; i < count; i++) {
			int x0, x1, y0, y1, s0, s1;
			if (!Rect<true>(v, tilesX, tilesY, ordered[i], x0, x1, y0, y1, s0, s1)) continue;
			binned++;
			const uint16_t index = (uint16_t)ordered[i].index;
			for (int sl = s0; sl <= s1; sl++)
				for (int ty = y0; ty <= y1; ty++) {
					const int first = (sl * tilesY + ty) * tilesX;
					for (int froxel = first + x0, end = first + x1; froxel <= end; froxel++) {
						const int c = counts[froxel];
						if (c >= listLamps) { over++; continue; }
						entries[c * MaxFroxels + froxel] = index;	// entry-major: neighbour froxels' entries k side by side
						counts[froxel] = (uint8_t)(c + 1);
						made++;
					}
				}
		}
		stats.lamps += binned; stats.entries += made; stats.over += over;
		LT_MARK(3);
		// write-out: froxel f's list is texels f x listTexels .. (never across a row: listTexels divides TableTexels), froxels in order
		const int perRow = TableTexels / listTexels, stride = listTexels * 4;
		for (int f = 0, row = 0; f < froxels; row++) {
			float* dst = lists + row * pitch;
			for (const int end = (std::min)(froxels, f + perRow); f < end; f++, dst += stride) {
				const int c = counts[f];
				const uint16_t* e = entries + f;	// entry k at e[k * MaxFroxels]
				float texel[4] = { (float)c, c > 0 ? (float)e[0] : 0.0f, c > 1 ? (float)e[MaxFroxels] : 0.0f, c > 2 ? (float)e[2 * MaxFroxels] : 0.0f };
				memcpy(dst, texel, sizeof(texel));
				float* d = dst;
				for (int k = 3; k < c; k += 4) {
					d += 4;
					texel[0] = (float)e[k * MaxFroxels];
					texel[1] = k + 1 < c ? (float)e[(k + 1) * MaxFroxels] : 0.0f;
					texel[2] = k + 2 < c ? (float)e[(k + 2) * MaxFroxels] : 0.0f;
					texel[3] = k + 3 < c ? (float)e[(k + 3) * MaxFroxels] : 0.0f;
					memcpy(d, texel, sizeof(texel));
				}
			}
		}
		LT_MARK(4);
		return true;
	}

	// UNOFFICIAL 2026-11-15 LampTableCheck's key history (docs/next/FD-LAMPTHREAD-RESULTS.md "Batch 3"). With StableOrder a lamp's sort key is
	// kept from the binning before while it changes by under 10 %, and the kept value carries on from frame to frame: it can be one fixed many
	// frames ago. A check path that did not run on the binning before starts without that history (raw keys), so for every flickering or
	// fading lamp it keeps a different value than the path that made the lists, and the order can differ for the whole check window although
	// both paths are the same function of (history, frame). Batch 3: lists differed in every binning in Vault 21 and the cave, 0 in Lucky 38.
	// The check path is therefore given the making path's history first (the keys of the binning before, stamped serial - 1), whenever it
	// did not run on that binning itself. Call BEFORE the making path bins (the history it is about to replace).
	inline void SeedRefFromFast(const FastState& fs, uint32_t serial, RefState& rs) {
		rs.keys.clear();
		if (fs.lastSerial + 1 != serial) return;	// the fast path has no history for this binning either (its prev is cleared on entry)
		fs.keys[fs.cur].forEach([&](const void* id, float key) { rs.keys[id] = { key, serial - 1 }; });
	}
	inline void SeedFastFromRef(const RefState& rs, uint32_t serial, FastState& fs) {
		PtrMap<float>& prev = fs.keys[fs.cur];
		prev.clear();
		for (const auto& e : rs.keys) if (e.second.second && e.second.second + 1 == serial) prev.insert(e.first, e.second.first);
		fs.lastSerial = serial - 1;
	}

	// What the shaders read of the lists: every froxel's count and its first count entries. -1 = the same, else the first froxel that differs.
	inline int ListsEqual(const View& v, const float* a, int pitchA, const float* b, int pitchB) {
		const int tilesX = TilesX(v), tilesY = TilesY(v);
		if (tilesX <= 0 || tilesY <= 0) return -1;
		auto at = [&](const float* t, int pitch, int froxel, int k) {
			const int texel = froxel * v.listTexels + (k + 1) / 4, comp = (k + 1) % 4;
			return t[(texel / TableTexels) * pitch + (texel % TableTexels) * 4 + comp];
		};
		for (int f = 0; f < tilesX * tilesY * Slices; f++) {
			const float ca = at(a, pitchA, f, -1);
			if (ca != at(b, pitchB, f, -1)) return f;
			for (int k = 0; k < (int)ca; k++) if (at(a, pitchA, f, k) != at(b, pitchB, f, k)) return f;
		}
		return -1;
	}
}
