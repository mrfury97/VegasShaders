#pragma once

// UNOFFICIAL 2026-11-07 (protocol 8) the RT real textures' transport rules, without the game's types: which route a game texture takes to
// the RT helper (its BC blocks copied as they are, or decoded to RGBA8 by the fork's blit), which of its levels go, where they are packed
// in this frame's half of the route's transfer image, how many bytes the helper's copy takes (the budget), the size caps picked from the
// GPU's video memory (RealTextureMaxSize / RealTextureBudgetMB = 0: auto) and which textures leave when the budget is full (least
// recently used, released with NVR_RT_OP_TEXTURE_RELEASE). RtCore.cpp calls these with what it reads off the D3D9 textures;
// tools/nvr-rt-host checks every rule (scene "texpack") and feeds the helper through the same functions (scenes "realtexgpu",
// "texrelease"). Plain C++ only: no Gamebryo, D3D or NVR headers (the host includes this file as is). docs/next/RT-REAL-TEXTURES.md.

#include <stdint.h>
#include <algorithm>
#include <vector>

namespace RtTexturePack {

	// The transfer routes, one shared image each (NvrRtShared::textureImage byte = route)
	enum Route : uint32_t { Rgba8 = 0, Bc1 = 1, Bc3 = 2, Bc5 = 3, RouteCount = 4 };

	constexpr uint32_t Fourcc(char a, char b, char c, char d) { return uint32_t(uint8_t(a)) | (uint32_t(uint8_t(b)) << 8) | (uint32_t(uint8_t(c)) << 16) | (uint32_t(uint8_t(d)) << 24); }
	constexpr uint32_t kDxt1 = Fourcc('D', 'X', 'T', '1'), kDxt3 = Fourcc('D', 'X', 'T', '3'), kDxt5 = Fourcc('D', 'X', 'T', '5');
	constexpr uint32_t kAti1 = Fourcc('A', 'T', 'I', '1'), kAti2 = Fourcc('A', 'T', 'I', '2');

	// VkFormat values (no Vulkan header here)
	constexpr uint32_t kVkRgba8 = 37, kVkBgra8 = 44, kVkBc1 = 133 /* BC1_RGBA_UNORM */, kVkBc2 = 135, kVkBc3 = 137, kVkBc5 = 141;
	inline uint32_t VkFormatOf(Route r) { return r == Bc1 ? kVkBc1 : r == Bc3 ? kVkBc3 : r == Bc5 ? kVkBc5 : kVkRgba8; }
	inline uint32_t BlockBytes(Route r) { return r == Bc1 ? 8u : r == Bc3 || r == Bc5 ? 16u : 0u; }	// 0: not block compressed
	inline const char* RouteName(Route r) { return r == Bc1 ? "BC1" : r == Bc3 ? "BC3" : r == Bc5 ? "BC5" : "RGBA8"; }

	// Bytes of one level in the helper's image (its format = the route's)
	inline uint64_t LevelBytes(Route r, uint32_t w, uint32_t h) {
		const uint32_t b = BlockBytes(r);
		return b ? uint64_t((w + 3) / 4) * ((h + 3) / 4) * b : uint64_t(w) * h * 4u;
	}

	// The transfer image of a route: kTexW x 2 halves (a half per exchange parity). A half holds level 0 of the cap's size and the column of
	// its smaller levels beside it (1.5 cap wide, cap tall). 512 and less: 2048 x 1024 (RGBA8 8 MB); 1024: 4096 x 2048 (RGBA8 32 MB, BC1 4 MB,
	// BC3 / BC5 8 MB); 2048: 4096 x 4096 (RGBA8 64 MB, BC1 8, BC3 16); 4096: 8192 x 8192 (BC1 32 MB, BC3 64; RGBA8 never: 256 MB, so RGBA8
	// stops at 2048).
	inline void TransferSize(uint32_t cap, uint32_t& width, uint32_t& halfHeight) {
		if (cap <= 512u) { width = 2048u; halfHeight = 512u; return; }
		width = (std::max)(4096u, (cap * 3u / 2u + 4095u) & ~4095u);
		halfHeight = cap;
	}
	inline uint32_t RgbaCapLimit(bool bcRoutes) { return bcRoutes ? 1024u : 2048u; }	// with BC routes RGBA8 carries only the odd formats

	// RealTextureMaxSize as read: 0 = auto, else 256..4096 (powers of two, rounded down)
	inline uint32_t ExplicitMaxSize(int m) { return m >= 4096 ? 4096u : m >= 2048 ? 2048u : m >= 1024 ? 1024u : m >= 512 ? 512u : 256u; }

	// Auto caps by the adapter's memory (bytes; 0 = unknown: the 8 GB tier, the old defaults). The figure is the dedicated video memory or,
	// where a 32-bit process cannot read that (DXGI's SIZE_T), the local budget divided by 0.92 (Windows gives a process ~90-95 % of the
	// card): the tiers' bounds sit half a GB to a GB under the card sizes so a 16 / 12 / 8 GB card lands in its tier either way.
	struct Caps { uint32_t maxSize; uint32_t budgetMB; const char* tier; };
	inline Caps AutoCaps(uint64_t memory) {
		const uint64_t GB = 1ull << 30;
		if (!memory) return { 1024u, 1536u, "unknown video memory: the 8 GB tier" };
		if (memory >= 15 * GB) return { 2048u, 4096u, "16 GB+" };
		if (memory >= 11 * GB) return { 2048u, 3072u, "12 GB+" };
		if (memory >= 7 * GB) return { 1024u, 1536u, "8 GB+" };
		return { 512u, 768u, "under 8 GB" };
	}

	// What goes for one texture
	struct Plan {
		Route		route = Rgba8;
		bool		swapRG = false;			// ATI2 -> BC5: the helper's view is (G, R, 1, 1), as DXVK samples ATI2
		uint32_t	first = 0;				// the game's first level sent
		uint32_t	levels = 0;				// levels sent (the helper's image has these)
		uint32_t	w0 = 0, h0 = 0;			// the first level's size
		uint32_t	fw = 0, fh = 0;			// its footprint in the transfer image (level 0 + the column of the rest)
		uint64_t	bytes = 0;				// the helper's image (all levels sent, the route's format)
	};

	// d3dFormat: the D3DFORMAT value; bc[r]: the route's transfer image exists (the fork has 1.8-rt-bc); capBc / capRgba: the biggest level
	// size a route sends. False: the texture keeps the atlas (why says so).
	inline bool Choose(uint32_t d3dFormat, uint32_t width, uint32_t height, uint32_t levelCount, const bool bc[RouteCount], uint32_t capBc, uint32_t capRgba,
		Plan& p, const char*& why) {
		p = Plan();
		why = nullptr;
		if (!width || !height || !levelCount) { why = "empty texture"; return false; }
		if (d3dFormat == kAti1) { why = "one-channel format (ATI1)"; return false; }
		Route r = Rgba8;
		if (d3dFormat == kDxt1 && bc[Bc1]) r = Bc1;
		else if (d3dFormat == kDxt5 && bc[Bc3]) r = Bc3;
		else if (d3dFormat == kAti2) {
			if (!bc[Bc5]) { why = "two-channel format (ATI2) and no BC5 transfer (fork older than 1.8-rt-bc)"; return false; }
			r = Bc5;
		}
		if (r != Rgba8 && ((width & 3u) || (height & 3u))) {	// a BC level 0 that is not whole blocks (D3D9 allows none; kept safe)
			if (r == Bc5) { why = "two-channel format (ATI2) not in whole blocks"; return false; }
			r = Rgba8;
		}
		for (int pass = 0; pass < 2; pass++) {
			const uint32_t cap = r == Rgba8 ? capRgba : capBc;
			uint32_t first = 0;
			while (first + 1 < levelCount && ((width >> first) > cap || (height >> first) > cap)) first++;
			const uint32_t w0 = (std::max)(1u, width >> first), h0 = (std::max)(1u, height >> first);
			uint32_t levels = levelCount - first;
			if (r != Rgba8) {	// BC: only levels of whole blocks (a 2 x 2 level cannot be copied into the middle of a BC image)
				levels = 0;
				for (uint32_t l = first; l < levelCount; l++) {
					const uint32_t w = (std::max)(1u, width >> l), h = (std::max)(1u, height >> l);
					if ((w & 3u) || (h & 3u)) break;
					levels++;
				}
				if (!levels) { if (r == Bc5) { why = "two-channel format (ATI2) not in whole blocks"; return false; } r = Rgba8; continue; }
			}
			p.route = r; p.swapRG = d3dFormat == kAti2; p.first = first; p.levels = levels; p.w0 = w0; p.h0 = h0;
			const uint32_t column = levels > 1 ? (std::max)(1u, w0 >> 1) : 0u;
			p.fw = w0 + column; p.fh = h0;
			if (r != Rgba8) p.fw = (p.fw + 3u) & ~3u;
			p.bytes = 0;
			for (uint32_t l = 0; l < levels; l++) p.bytes += LevelBytes(r, (std::max)(1u, w0 >> l), (std::max)(1u, h0 >> l));
			return true;
		}
		why = "no route";
		return false;
	}

	// Where level l (0 = the plan's first) lies, relative to the footprint's corner: level 0 at (0, 0), level l >= 1 at (w0, the heights of
	// levels 1 .. l-1) (the helper reads the same layout: NvrRtTextureOp::bytes' comment)
	inline void LevelAt(const Plan& p, uint32_t l, uint32_t& x, uint32_t& y) {
		x = l ? p.w0 : 0u; y = 0;
		for (uint32_t k = 1; k < l; k++) y += (std::max)(1u, p.h0 >> k);
	}

	// Shelf packing in one half of a route's transfer image (reset per exchange). False: no room this exchange.
	struct Shelf {
		uint32_t x = 0, y = 0, h = 0;
		bool Place(uint32_t fw, uint32_t fh, uint32_t width, uint32_t halfHeight, uint32_t& outX, uint32_t& outY) {
			if (fw > width || fh > halfHeight) return false;
			if (x + fw > width) { x = 0; y += h; h = 0; }
			if (y + fh > halfHeight) return false;
			outX = x; outY = y;
			x += fw; h = (std::max)(h, fh);
			return true;
		}
	};

	// The ids alive in the helper and their bytes, last use and release: the budget made a working set instead of a one-way session cap.
	// Ids 1 .. maxIds - 1; a released id is given out again only `reuseDelay` exchanges later (frames in flight may still read the old one).
	class Residency {
	public:
		void Reset(uint32_t maxIds) {
			bytes.assign(maxIds, 0); lastUse.assign(maxIds, 0); live.assign(maxIds, 0); pinned.assign(maxIds, 0); freed.clear(); next = 1; alive = 0; count = 0;
		}
		// (merged with rtgap-tessblas) a pinned id is never a victim (the displacement's height textures: the helper's displaced copies
		// keep their ids); it still counts in the budget. Freeing it unpins it.
		void Pin(uint32_t id) { if (Live(id)) pinned[id] = 1; }
		bool Pinned(uint32_t id) const { return Live(id) && pinned[id]; }
		uint32_t Alloc(uint64_t now) {
			for (size_t i = 0; i < freed.size(); i++)
				if (freed[i].at <= now) { const uint32_t id = freed[i].id; freed.erase(freed.begin() + i); return id; }
			if (next < bytes.size()) return next++;
			return 0;
		}
		void Add(uint32_t id, uint64_t b, uint64_t now) { if (id && id < bytes.size() && !live[id]) { bytes[id] = b; lastUse[id] = now; live[id] = 1; alive += b; count++; } }
		void Use(uint32_t id, uint64_t now) { if (id && id < bytes.size() && live[id]) lastUse[id] = now; }
		bool Live(uint32_t id) const { return id && id < bytes.size() && live[id]; }
		void Free(uint32_t id, uint64_t now, uint64_t reuseDelay) {
			if (!Live(id)) return;
			alive -= bytes[id]; live[id] = 0; bytes[id] = 0; pinned[id] = 0; count--;
			freed.push_back({ id, now + reuseDelay });
		}
		// The least recently used ids, none used in the last `idle` exchanges, whose release lets `need` more bytes fit `budget`. False (and
		// none picked) when even all of them would not make room: nothing is released for nothing.
		bool PickVictims(uint64_t need, uint64_t budget, uint64_t now, uint64_t idle, std::vector<uint32_t>& victims) const {
			victims.clear();
			if (alive + need <= budget) return true;
			if (need > budget) return false;
			std::vector<uint32_t> idleIds;
			for (uint32_t id = 1; id < bytes.size(); id++) if (live[id] && !pinned[id] && lastUse[id] + idle <= now) idleIds.push_back(id);
			std::sort(idleIds.begin(), idleIds.end(), [&](uint32_t a, uint32_t b) { return lastUse[a] != lastUse[b] ? lastUse[a] < lastUse[b] : a < b; });
			uint64_t freedBytes = 0;
			for (uint32_t id : idleIds) {
				if (alive - freedBytes + need <= budget) break;
				victims.push_back(id); freedBytes += bytes[id];
			}
			if (alive - freedBytes + need > budget) { victims.clear(); return false; }
			return true;
		}
		uint64_t alive = 0;		// bytes of the live ids
		uint32_t count = 0;		// live ids
	private:
		struct Freed { uint32_t id; uint64_t at; };
		std::vector<uint64_t> bytes, lastUse;
		std::vector<uint8_t> live, pinned;
		std::vector<Freed> freed;
		uint32_t next = 1;
	};
}
