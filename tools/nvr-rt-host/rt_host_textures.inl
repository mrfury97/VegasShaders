// rt_host_textures.inl (2026-11-07, protocol 8, rtgap-textures): the RT real textures' transport (included by rt_host.cpp after
// rt_host_alpha.inl). docs/next/RT-REAL-TEXTURES.md, docs/next/RT-DETAIL-GAPS.md section 3.
//
// The game sends its textures on the GPU route (they are in D3D9's DEFAULT pool: RtCore::SendTexture): each level's surface is copied
// by the fork's CopyToShared into a transfer image (RGBA8: the fork's blit decodes the BC blocks; with fork 1.8-rt-bc, BC1 / BC3 / BC5
// transfer images: the blocks as they are) and a TEXTURE op with bytes 0 points the helper at the rectangles. These scenes drive the
// same route with RtTexturePack.h (the game's own rules: route, levels, packing, budget, release) on D3D9 DEFAULT-pool DXT textures:
//   texpack    (no GPU) the rules: auto caps by video memory (and what this PC's adapters report), routes, levels, footprints, packing,
//              the budget's least-recently-used release and id reuse
//   realtexbc  the CPU route with BC3 / BC1 blocks (the helper's BC images): the chain-link shadow vs the exact one (realtex's bar)
//   realtexgpu the GPU route through the fork: DXT5 / DXT1 chain-link (alpha) vs the exact shadow, and a DXT1 colour floor (+ ATI2 as BC5
//              with its swizzle when the fork has 1.8-rt-bc) read by the path tracer vs the same texels sent as BGRA8 (CPU route);
//              every route this d3d9.dll offers (RGBA8 always; BC1 / BC3 / BC5 with 1.8-rt-bc), RGBA8 also forced with BC there
//   texrelease TEXTURE_RELEASE: a slot's real texture (opaque) released -> the atlas (clear) shows, the id reused for the chain-link

#include "../../src/core/RtTexturePack.h"
#include "../../src/core/RtVideoMemory.h"

namespace HostTex {
	struct Rgba { uint8_t r = 0, g = 0, b = 0, a = 255; };
	using Level = std::vector<Rgba>;
	using RT = RtTexturePack::Route;

	inline uint16_t To565(const Rgba& c) { return uint16_t(((c.r >> 3) << 11) | ((c.g >> 2) << 5) | (c.b >> 3)); }
	inline Rgba From565(uint16_t v) {
		Rgba c; const uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
		c.r = uint8_t((r << 3) | (r >> 2)); c.g = uint8_t((g << 2) | (g >> 4)); c.b = uint8_t((b << 3) | (b >> 2)); c.a = 255; return c;
	}
	inline int Dist(const Rgba& a, const Rgba& b) { return (a.r - b.r) * (a.r - b.r) + (a.g - b.g) * (a.g - b.g) + (a.b - b.b) * (a.b - b.b); }

	// BC1 / the colour half of BC3: endpoints = the brightest and darkest texels; four-colour mode (c0 > c1), or three-colour + transparent
	// when `punch` and a texel's alpha < 128 (BC1 only). Equal endpoints: index 0 everywhere (exact).
	inline void ColourBlock(const Rgba px[16], uint8_t out[8], bool punch) {
		bool anyClear = false;
		int hi = -1, lo = -1, hiL = -1, loL = 1 << 30;
		for (int i = 0; i < 16; i++) {
			if (punch && px[i].a < 128) { anyClear = true; continue; }
			const int l = px[i].r * 2 + px[i].g * 4 + px[i].b;
			if (l > hiL) { hiL = l; hi = i; }
			if (l < loL) { loL = l; lo = i; }
		}
		uint16_t c0 = hi >= 0 ? To565(px[hi]) : 0, c1 = lo >= 0 ? To565(px[lo]) : 0;
		const bool three = punch && anyClear;
		if (three ? c0 > c1 : c0 < c1) std::swap(c0, c1);
		Rgba p[4] = { From565(c0), From565(c1) };
		if (three) { p[2] = { uint8_t((p[0].r + p[1].r) / 2), uint8_t((p[0].g + p[1].g) / 2), uint8_t((p[0].b + p[1].b) / 2), 255 }; }
		else {
			p[2] = { uint8_t((2 * p[0].r + p[1].r) / 3), uint8_t((2 * p[0].g + p[1].g) / 3), uint8_t((2 * p[0].b + p[1].b) / 3), 255 };
			p[3] = { uint8_t((p[0].r + 2 * p[1].r) / 3), uint8_t((p[0].g + 2 * p[1].g) / 3), uint8_t((p[0].b + 2 * p[1].b) / 3), 255 };
		}
		uint32_t idx = 0;
		for (int i = 0; i < 16; i++) {
			int best = 0;
			if (three && px[i].a < 128) best = 3;
			else if (c0 != c1) { int bd = 1 << 30; for (int k = 0; k < (three ? 3 : 4); k++) { const int d = Dist(px[i], p[k]); if (d < bd) { bd = d; best = k; } } }
			idx |= uint32_t(best) << (2 * i);
		}
		out[0] = uint8_t(c0); out[1] = uint8_t(c0 >> 8); out[2] = uint8_t(c1); out[3] = uint8_t(c1 >> 8);
		out[4] = uint8_t(idx); out[5] = uint8_t(idx >> 8); out[6] = uint8_t(idx >> 16); out[7] = uint8_t(idx >> 24);
	}
	// BC4 (BC3's alpha, BC5's channels): a0 = max > a1 = min, eight-value mode
	inline void ScalarBlock(const uint8_t v[16], uint8_t out[8]) {
		uint8_t a0 = 0, a1 = 255;
		for (int i = 0; i < 16; i++) { a0 = std::max(a0, v[i]); a1 = std::min(a1, v[i]); }
		int pal[8] = { a0, a1 };
		for (int k = 1; k < 7; k++) pal[k + 1] = ((7 - k) * a0 + k * a1) / 7;
		uint64_t idx = 0;
		for (int i = 0; i < 16; i++) {
			int best = 0;
			if (a0 != a1) { int bd = 1 << 30; for (int k = 0; k < 8; k++) { const int d = std::abs(int(v[i]) - pal[k]); if (d < bd) { bd = d; best = k; } } }
			idx |= uint64_t(best) << (3 * i);
		}
		out[0] = a0; out[1] = a1;
		for (int k = 0; k < 6; k++) out[2 + k] = uint8_t(idx >> (8 * k));
	}
	inline std::vector<uint8_t> Encode(const Level& lv, uint32_t w, uint32_t h, RT r) {
		std::vector<uint8_t> out;
		const uint32_t bw = std::max(1u, (w + 3) / 4), bh = std::max(1u, (h + 3) / 4);
		for (uint32_t by = 0; by < bh; by++) for (uint32_t bx = 0; bx < bw; bx++) {
			Rgba px[16]; uint8_t ch[2][16], a[16];
			for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) {
				const uint32_t x = std::min(bx * 4 + i, w - 1), y = std::min(by * 4 + j, h - 1);
				px[j * 4 + i] = lv[size_t(y) * w + x]; a[j * 4 + i] = px[j * 4 + i].a; ch[0][j * 4 + i] = px[j * 4 + i].r; ch[1][j * 4 + i] = px[j * 4 + i].g;
			}
			uint8_t b[16];
			if (r == RtTexturePack::Bc1) { ColourBlock(px, b, true); out.insert(out.end(), b, b + 8); }
			else if (r == RtTexturePack::Bc3) { ScalarBlock(a, b); ColourBlock(px, b + 8, false); out.insert(out.end(), b, b + 16); }
			else if (r == RtTexturePack::Bc5) { ScalarBlock(ch[0], b); ScalarBlock(ch[1], b + 8); out.insert(out.end(), b, b + 16); }
		}
		return out;
	}
	// The reference decoder (the spec's palettes; GPUs may round the thirds one step differently)
	inline Level Decode(const std::vector<uint8_t>& bytes, uint32_t w, uint32_t h, RT r) {
		Level lv(size_t(w) * h);
		const uint32_t bw = std::max(1u, (w + 3) / 4), bh = std::max(1u, (h + 3) / 4), bb = RtTexturePack::BlockBytes(r);
		auto scalar = [](const uint8_t* b, int i) {
			const int a0 = b[0], a1 = b[1]; uint64_t idx = 0; for (int k = 0; k < 6; k++) idx |= uint64_t(b[2 + k]) << (8 * k);
			const int s = int((idx >> (3 * i)) & 7);
			if (s == 0) return a0; if (s == 1) return a1;
			if (a0 > a1) return ((8 - s) * a0 + (s - 1) * a1) / 7;
			if (s == 6) return 0; if (s == 7) return 255;
			return ((6 - s) * a0 + (s - 1) * a1) / 5;
		};
		for (uint32_t by = 0; by < bh; by++) for (uint32_t bx = 0; bx < bw; bx++) {
			const uint8_t* b = &bytes[(size_t(by) * bw + bx) * bb];
			for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) {
				const uint32_t x = bx * 4 + i, y = by * 4 + j;
				if (x >= w || y >= h) continue;
				Rgba o;
				if (r == RtTexturePack::Bc5) { o.r = uint8_t(scalar(b, j * 4 + i)); o.g = uint8_t(scalar(b + 8, j * 4 + i)); o.b = 0; o.a = 255; }
				else {
					const uint8_t* cb = r == RtTexturePack::Bc3 ? b + 8 : b;
					const uint16_t c0 = uint16_t(cb[0] | (cb[1] << 8)), c1 = uint16_t(cb[2] | (cb[3] << 8));
					const uint32_t idx = uint32_t(cb[4]) | (uint32_t(cb[5]) << 8) | (uint32_t(cb[6]) << 16) | (uint32_t(cb[7]) << 24);
					const int s = int((idx >> (2 * (j * 4 + i))) & 3);
					const Rgba p0 = From565(c0), p1 = From565(c1);
					const bool four = r == RtTexturePack::Bc3 || c0 > c1;
					if (s == 0) o = p0; else if (s == 1) o = p1;
					else if (four) o = s == 2 ? Rgba{ uint8_t((2 * p0.r + p1.r) / 3), uint8_t((2 * p0.g + p1.g) / 3), uint8_t((2 * p0.b + p1.b) / 3), 255 }
						: Rgba{ uint8_t((p0.r + 2 * p1.r) / 3), uint8_t((p0.g + 2 * p1.g) / 3), uint8_t((p0.b + 2 * p1.b) / 3), 255 };
					else o = s == 2 ? Rgba{ uint8_t((p0.r + p1.r) / 2), uint8_t((p0.g + p1.g) / 2), uint8_t((p0.b + p1.b) / 2), 255 } : Rgba{ 0, 0, 0, 0 };
					if (r == RtTexturePack::Bc3) o.a = uint8_t(scalar(b, j * 4 + i));
				}
				lv[size_t(y) * w + x] = o;
			}
		}
		return lv;
	}
	inline std::vector<Level> Mips(Level top, uint32_t size) {	// a box-filtered chain down to 1 x 1 (square, power of two)
		std::vector<Level> out = { top };
		for (uint32_t r = size; r > 1; r /= 2) {
			const Level& a = out.back(); const uint32_t n = r / 2; Level b(size_t(n) * n);
			for (uint32_t y = 0; y < n; y++) for (uint32_t x = 0; x < n; x++) {
				int s[4] = {};
				for (int k = 0; k < 4; k++) { const Rgba& p = a[size_t(2 * y + k / 2) * r + 2 * x + k % 2]; s[0] += p.r; s[1] += p.g; s[2] += p.b; s[3] += p.a; }
				b[size_t(y) * n + x] = { uint8_t((s[0] + 2) / 4), uint8_t((s[1] + 2) / 4), uint8_t((s[2] + 2) / 4), uint8_t((s[3] + 2) / 4) };
			}
			out.push_back(b);
		}
		return out;
	}
	inline std::vector<uint8_t> Bgra(const std::vector<Level>& levels, uint32_t from = 0) {	// packed BGRA8 levels (the CPU route)
		std::vector<uint8_t> out;
		for (size_t l = from; l < levels.size(); l++) for (const Rgba& p : levels[l]) { out.push_back(p.b); out.push_back(p.g); out.push_back(p.r); out.push_back(p.a); }
		return out;
	}

	// The game's transfer images (RtCore::Start's), registered in main
	struct Routes { int image[RtTexturePack::RouteCount] = { -1, -1, -1, -1 }; uint32_t w[RtTexturePack::RouteCount] = {}, halfH[RtTexturePack::RouteCount] = {};
		bool on[RtTexturePack::RouteCount] = {}; RtTexturePack::Shelf shelf[RtTexturePack::RouteCount]; };
	static Routes gRoutes;

	// A D3D9 DEFAULT-pool texture (as the game's), its levels' raw data (BC blocks or A8R8G8B8 texels) put in through UpdateTexture
	inline IDirect3DTexture9* GameTexture(Host& h, D3DFORMAT fmt, uint32_t w, uint32_t hh, const std::vector<std::vector<uint8_t>>& data) {
		IDirect3DTexture9 *sys = nullptr, *gpu = nullptr;
		const UINT levels = UINT(data.size());
		if (FAILED(h.dev->CreateTexture(w, hh, levels, 0, fmt, D3DPOOL_SYSTEMMEM, &sys, nullptr)) ||
			FAILED(h.dev->CreateTexture(w, hh, levels, 0, fmt, D3DPOOL_DEFAULT, &gpu, nullptr))) { say("  error: CreateTexture format 0x%08x\n", unsigned(fmt)); if (sys) sys->Release(); return nullptr; }
		const bool bc = fmt == D3DFMT_DXT1 || fmt == D3DFMT_DXT5 || fmt == (D3DFORMAT)RtTexturePack::kAti2;
		const uint32_t bb = fmt == D3DFMT_DXT1 ? 8u : 16u;
		for (UINT l = 0; l < levels; l++) {
			const uint32_t lw = std::max(1u, w >> l), lh = std::max(1u, hh >> l);
			const uint32_t rows = bc ? std::max(1u, (lh + 3) / 4) : lh, rowBytes = bc ? std::max(1u, (lw + 3) / 4) * bb : lw * 4;
			D3DLOCKED_RECT lr;
			if (FAILED(sys->LockRect(l, &lr, nullptr, 0))) { say("  error: LockRect\n"); return nullptr; }
			// ATI1 / ATI2: D3D9 (and DXVK's "atiHack") report the pitch of a one-byte-a-texel image; the blocks lie packed: one copy
			if (fmt == (D3DFORMAT)RtTexturePack::kAti2) std::memcpy(lr.pBits, data[l].data(), data[l].size());
			else for (uint32_t r = 0; r < rows; r++) std::memcpy((uint8_t*)lr.pBits + size_t(r) * lr.Pitch, data[l].data() + size_t(r) * rowBytes, rowBytes);
			sys->UnlockRect(l);
		}
		if (FAILED(h.dev->UpdateTexture(sys, gpu))) { say("  error: UpdateTexture\n"); return nullptr; }
		h.uploads.push_back(sys); h.uploads.push_back(gpu);	// alive until the frame's exchange is done (EndFrame)
		return gpu;
	}

	// RtCore::SendTexture on the host: the game's plan, its rectangles in this exchange's half, CopyToShared per level, the op (bytes 0)
	inline bool SendGpu(Host& h, IDirect3DTexture9* t, uint32_t id, uint32_t cap, const bool routes[RtTexturePack::RouteCount], RtTexturePack::Plan& p) {
		D3DSURFACE_DESC d = {};
		t->GetLevelDesc(0, &d);
		const bool bc = routes[RtTexturePack::Bc1] || routes[RtTexturePack::Bc3] || routes[RtTexturePack::Bc5];
		const char* why = nullptr;
		if (!RtTexturePack::Choose((uint32_t)d.Format, d.Width, d.Height, t->GetLevelCount(), routes, cap, std::min(cap, RtTexturePack::RgbaCapLimit(bc)), p, why)) {
			say("  error: the game would keep the atlas: %s\n", why); return false;
		}
		const int img = gRoutes.image[p.route];
		uint32_t x = 0, y = 0;
		if (img < 0 || !gRoutes.shelf[p.route].Place(p.fw, p.fh, gRoutes.w[p.route], gRoutes.halfH[p.route], x, y)) { say("  error: no room in the %s transfer image\n", RtTexturePack::RouteName(p.route)); return false; }
		y += uint32_t((h.frame + 1) & 1) * gRoutes.halfH[p.route];
		for (uint32_t l = 0; l < p.levels; l++) {
			IDirect3DSurface9* s = nullptr;
			CHECKHR(t->GetSurfaceLevel(p.first + l, &s), "GetSurfaceLevel");
			uint32_t lx = 0, ly = 0;
			RtTexturePack::LevelAt(p, l, lx, ly);
			const HRESULT hr = h.share->CopyToShared(h.images[img].shared, x + lx, y + ly, s, nullptr);
			s->Release();
			CHECKHR(hr, "CopyToShared (a level into the transfer image)");
		}
		NvrRtTextureOp op = { NVR_RT_OP_TEXTURE, id, p.w0, p.h0, x | (y << 16), RtTexturePack::VkFormatOf(p.route), 0u, p.levels | (p.swapRG ? NVR_RT_TEX_SWIZZLE_GR : 0u) };
		NvrRtGeometryOp g; std::memcpy(&g, &op, sizeof(g)); h.ops.push_back(g);
		return true;
	}
	inline void ResetShelves() { for (auto& s : gRoutes.shelf) s = RtTexturePack::Shelf(); }
}

// ---- the chain-link card's lamp shadow (rt_host_alpha.inl's measure) with whatever texture the setup sends for the slot
struct ChainResult { int both = 0, tiles = 0; float agree = 0, gShare = 0, cShare = 0, recall = 0, rmse = 1; };
static bool chainLinkMeasure(Ctx& c, int slot, const AlphaFn& atlas, int atlasRes, const AlphaFn& exactFn, int exactRes, const std::function<bool()>& setup, ChainResult& r) {
	Host& h = *c.host;
	World w;
	Mesh floor; floor.quad({ -4000, -4000, 0 }, { 4000, -4000, 0 }, { 4000, 4000, 0 }, { -4000, 4000, 0 });
	Mesh card; card.quad({ -700, -700, 260 }, { 700, -700, 260 }, { 700, 700, 260 }, { -700, 700, 260 });
	for (size_t i = card.uv.size() - 8; i < card.uv.size(); i++) card.uv[i] *= 3.0f;
	w.meshes = { floor, card };
	Object f; f.mesh = 0; f.id = 1;
	Object k; k.mesh = 1; k.id = 2; k.alphaTested = true; k.alphaSlot = slot; k.alphaRef = 128;
	w.objects = { f, k };
	if (!uploadSlot(h, c.alphaAtlas, slot, atlas, atlasRes)) return false;
	HostTex::ResetShelves();
	if (setup && !setup()) return false;
	World exact = w; exact.slots[slot] = exactFn; exact.slotRes[slot] = exactRes;
	w.slots[slot] = atlas; w.slotRes[slot] = atlasRes;
	std::vector<uint32_t> ids;
	if (!stageWorld(h, w, ids)) return false;
	const std::vector<Lamp> lamps = { { V3(0, 0, 900), 0, 0, 0 }, { V3(0, 0, 900), 0, 0, 0 }, { V3(0, 0, 900), 0, 0, 0 },
		{ V3(0, 0, 900), 0, 0, 0 }, { V3(0, 0, 900), 0, 0, 0 }, { V3(37, -23, 1100), 8000, 1.0f, 8000 } };
	const Camera cam = Camera::lookDir({ 0, 0, 2400 }, { 0, 0, -1 }, { 0, 1, 0 }, 1.6f, 1.6f);
	int checked = 0, bad = 0;
	std::vector<float> res;
	if (!runLampTrace(c, w, ids, lamps, cam, 1, "(vs the atlas; not judged)", checked, bad, nullptr, &res)) return false;
	std::vector<GPixel> px; std::vector<float> depth, normals;
	gbuffer(exact, cam, px, depth, normals);
	std::vector<float> gpu(S * S, -1.0f), cpu(S * S, -1.0f);
	int agree = 0, gs = 0, cs = 0, recall = 0;
	r = ChainResult();
	for (UINT i = 0; i < S * S; i++) {
		if (!px[i].valid || px[i].object != 0) continue;
		const int e0 = int(res[i * 2]) % 1024;
		if (e0 % 64 != 5) continue;
		gpu[i] = float(e0 / 64) / 15.0f;
		cpu[i] = lampLitCpu(exact, px[i].pos + px[i].n * 0.5f, lamps[5], 0.5f) ? 1.0f : 0.0f;
		r.both++; agree += (gpu[i] > 0.5f) == (cpu[i] > 0.5f); gs += gpu[i] < 0.5f; cs += cpu[i] < 0.5f; recall += cpu[i] < 0.5f && gpu[i] < 0.5f;
	}
	double se = 0;
	for (UINT ty = 0; ty < S / 8; ty++) for (UINT tx = 0; tx < S / 8; tx++) {
		double g = 0, e = 0; int n = 0, sh = 0;
		for (UINT y = 0; y < 8; y++) for (UINT x = 0; x < 8; x++) {
			const UINT i = (ty * 8 + y) * S + tx * 8 + x;
			if (gpu[i] < 0) continue;
			g += gpu[i]; e += cpu[i]; n++; sh += cpu[i] < 0.5f;
		}
		if (n < 48 || sh == 0) continue;
		se += (g / n - e / n) * (g / n - e / n); r.tiles++;
	}
	r.rmse = r.tiles ? float(std::sqrt(se / r.tiles)) : 0.0f;
	r.agree = r.both ? float(agree) / r.both : 0; r.gShare = r.both ? float(gs) / r.both : 0; r.cShare = r.both ? float(cs) / r.both : 0;
	r.recall = cs ? float(recall) / cs : 1.0f;
	releaseWorld(h, ids);
	return true;
}
// realtex's bar (rt_host_alpha.inl: the real 512 texture, measured 99.5 % / 94.3 % / 0.020)
static bool chainLinkRealOk(const ChainResult& r) {
	return r.both > 1000 && r.tiles > 50 && r.agree > 0.99f && r.recall > 0.9f && std::fabs(r.gShare - r.cShare) < 0.1f * r.cShare && r.rmse < 0.025f;
}
static void chainLinkSay(const char* label, const ChainResult& r, bool ok) {
	say("  %-44s %d floor pixels, agree %.1f %%, shadowed %.3f (exact %.3f), exact shadow found %.1f %%, 8x8 tiles RMSE %.3f over %d -> %s\n", label, r.both,
		100.0f * r.agree, r.gShare, r.cShare, 100.0f * r.recall, r.rmse, r.tiles, ok ? "ok" : "BAD");
}
// the chain-link pattern's levels as RGBA (colour 200 180 160, alpha = the wires), the full mip chain
static std::vector<HostTex::Level> chainLinkLevels() {
	HostTex::Level top(size_t(kChainRes) * kChainRes);
	for (int y = 0; y < kChainRes; y++) for (int x = 0; x < kChainRes; x++) top[size_t(y) * kChainRes + x] = { 200, 180, 160, uint8_t(chainLinkFull(x, y) > 0.5f ? 255 : 0) };
	return HostTex::Mips(top, kChainRes);
}

// ------------------------------------------------------------------------------------------------------------------------------
static bool sceneTexPack(Ctx& c) {
	using namespace RtTexturePack;
	int bad = 0;
	auto check = [&](bool ok, const char* what) { if (!ok) { bad++; say("    WRONG: %s\n", what); } };
	const uint64_t GB = 1ull << 30, MB = 1ull << 20;
	// auto caps: the tiers, with a 32-bit process's DXGI figure (budget / 0.92 of a card's ~90-95 %) landing in the card's tier
	struct T { uint64_t mem; uint32_t size, budget; } tiers[] = { { 0, 1024, 1536 }, { 512 * MB, 512, 768 }, { 6 * GB, 512, 768 }, { 8 * GB, 1024, 1536 },
		{ uint64_t(8 * GB * 0.90 / 0.92), 1024, 1536 }, { 11 * GB, 2048, 3072 }, { 12 * GB, 2048, 3072 }, { uint64_t(12 * GB * 0.9 / 0.92), 2048, 3072 },
		{ 16 * GB, 2048, 4096 }, { uint64_t(16 * GB * 0.95 / 0.92), 2048, 4096 }, { 24 * GB, 2048, 4096 } };
	for (const T& t : tiers) { const Caps k = AutoCaps(t.mem); char w[96]; snprintf(w, sizeof(w), "AutoCaps(%llu MB) = %u / %u", (unsigned long long)(t.mem >> 20), k.maxSize, k.budgetMB);
		check(k.maxSize == t.size && k.budgetMB == t.budget, w); }
	check(ExplicitMaxSize(1024) == 1024 && ExplicitMaxSize(3000) == 2048 && ExplicitMaxSize(9999) == 4096 && ExplicitMaxSize(1) == 256, "ExplicitMaxSize");
	// this PC's adapters, as RtCore reads them (32-bit, like the game)
	{
		IDXGIFactory1* factory = nullptr;
		typedef HRESULT(WINAPI* CreateFactory)(REFIID, void**);
		const CreateFactory create = (CreateFactory)GetProcAddress(RtVideoMemory::System32("dxgi.dll"), "CreateDXGIFactory1");
		IDXGIAdapter1* a = nullptr;
		if (create && SUCCEEDED(create(__uuidof(IDXGIFactory1), (void**)&factory)))
			for (UINT i = 0; SUCCEEDED(factory->EnumAdapters1(i, &a)); i++) {
				DXGI_ADAPTER_DESC1 d = {}; a->GetDesc1(&d); a->Release();
				if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
				char name[128]; WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof(name), nullptr, nullptr);
				char text[256]; BYTE luid[8]; std::memcpy(luid, &d.AdapterLuid, 8);
				const unsigned long long m = RtVideoMemory::Dedicated(luid, true, d.VendorId, d.DeviceId, text, sizeof(text));
				char viaDxgi[256]; const unsigned long long m2 = RtVideoMemory::Dedicated(luid, false, d.VendorId, d.DeviceId, viaDxgi, sizeof(viaDxgi));
				const Caps k = AutoCaps(m);
				say("    adapter %s: %s -> tier %s: RealTextureMaxSize %u, RealTextureBudgetMB %u (DXGI alone: %s)\n", name, text, k.tier, k.maxSize, k.budgetMB, viaDxgi);
				// under 2.75 GB DXGI's figure is exact (a 32-bit process reads at most 3 GB): the kernel's must agree with it; either way
				// the two must land the adapter in the same tier (the DXGI fallback with its budget / 0.92 stand-in)
				if (m && m2 && m2 < (11ull << 28)) check(m / MB == m2 / MB, "D3DKMT and DXGI disagree on an adapter under 2.75 GB");
				if (m && m2) check(AutoCaps(m).maxSize == AutoCaps(m2).maxSize && AutoCaps(m).budgetMB == AutoCaps(m2).budgetMB, "D3DKMT and the DXGI fallback pick different tiers");
			}
		if (factory) factory->Release();
	}
	// routes
	const bool bcOn[4] = { true, true, true, true }, bcOff[4] = { true, false, false, false };
	Plan p; const char* why = nullptr;
	check(Choose(kDxt1, 2048, 2048, 12, bcOn, 2048, 1024, p, why) && p.route == Bc1 && p.first == 0 && p.levels == 10 && p.w0 == 2048, "DXT1 2048, cap 2048: BC1, levels 2048..4");
	{ uint64_t b = 0; for (uint32_t s = 2048; s >= 4; s /= 2) b += uint64_t(s / 4) * (s / 4) * 8; check(p.bytes == b, "BC1 bytes"); }
	check(Choose(kDxt1, 2048, 2048, 12, bcOn, 1024, 1024, p, why) && p.route == Bc1 && p.first == 1 && p.levels == 9 && p.w0 == 1024 && p.fw == 1536 && p.fh == 1024, "DXT1 2048, cap 1024: first 1");
	check(Choose(kDxt1, 2048, 2048, 12, bcOff, 2048, 2048, p, why) && p.route == Rgba8 && p.first == 0 && p.levels == 12 && p.bytes > 16ull * MB, "DXT1 without BC routes: RGBA8, every level");
	check(Choose(kDxt5, 512, 256, 10, bcOn, 1024, 1024, p, why) && p.route == Bc3 && p.levels == 7 && p.h0 == 256 && p.fw == 768, "DXT5 512 x 256: BC3, levels to 8 x 4");
	check(Choose(kAti2, 256, 256, 9, bcOn, 1024, 1024, p, why) && p.route == Bc5 && p.swapRG && p.levels == 7, "ATI2: BC5 + swizzle");
	check(!Choose(kAti2, 256, 256, 9, bcOff, 1024, 1024, p, why), "ATI2 without BC5: refused");
	check(!Choose(kAti1, 256, 256, 9, bcOn, 1024, 1024, p, why), "ATI1: refused");
	check(Choose(21 /* A8R8G8B8 */, 2048, 2048, 12, bcOn, 2048, 1024, p, why) && p.route == Rgba8 && p.w0 == 1024, "A8R8G8B8 with BC routes: RGBA8 at most 1024");
	check(Choose(kDxt1, 8, 8, 4, bcOn, 1024, 1024, p, why) && p.route == Bc1 && p.levels == 2 && p.fw == 12, "DXT1 8 x 8: 8 and 4 (whole blocks), footprint 12");
	check(Choose(kDxt1, 4, 4, 3, bcOn, 1024, 1024, p, why) && p.route == Bc1 && p.levels == 1 && p.fw == 4, "DXT1 4 x 4: one level");
	check(Choose(kDxt1, 12, 20, 5, bcOn, 1024, 1024, p, why) && p.route == Bc1 && p.levels == 1, "DXT1 12 x 20: level 0 only (6 x 10 is not whole blocks)");
	{ uint32_t x, y; Choose(kDxt1, 64, 64, 7, bcOn, 1024, 1024, p, why); LevelAt(p, 1, x, y); check(x == 64 && y == 0, "LevelAt 1"); LevelAt(p, 3, x, y); check(x == 64 && y == 48, "LevelAt 3"); }
	uint32_t tw, th;
	TransferSize(1024, tw, th); check(tw == 4096 && th == 1024, "TransferSize 1024");
	TransferSize(2048, tw, th); check(tw == 4096 && th == 2048, "TransferSize 2048");
	TransferSize(4096, tw, th); check(tw == 8192 && th == 4096, "TransferSize 4096");
	TransferSize(512, tw, th); check(tw == 2048 && th == 512, "TransferSize 512");
	// packing: every BC rectangle on whole blocks, none overlapping, inside the half
	{
		Shelf s; uint32_t placed = 0; std::vector<std::array<uint32_t, 4>> rects; uint32_t seed = 7;
		for (int i = 0; i < 400; i++) {
			seed = seed * 1664525u + 1013904223u;
			const uint32_t size = 4u << ((seed >> 16) % 9), aspect = (seed >> 8) % 3;
			if (!Choose(kDxt5, size, aspect == 1 ? std::max(4u, size / 2) : size, 12, bcOn, 1024, 1024, p, why)) continue;
			uint32_t x, y;
			if (!s.Place(p.fw, p.fh, 4096, 1024, x, y)) continue;
			placed++;
			check(!(x & 3) && !(y & 3) && x + p.fw <= 4096 && y + p.fh <= 1024, "a BC rectangle off its blocks / outside the half");
			for (auto& q : rects) if (x < q[0] + q[2] && q[0] < x + p.fw && y < q[1] + q[3] && q[1] < y + p.fh) { check(false, "overlapping rectangles"); break; }
			rects.push_back({ x, y, p.fw, p.fh });
		}
		say("    packing: %u footprints placed in a 4096 x 1024 half, all on whole blocks, none overlapping\n", placed);
		check(placed > 20, "packing placed too few");
		Shelf one; uint32_t x, y; int n = 0;
		Choose(kDxt1, 1024, 1024, 11, bcOn, 1024, 1024, p, why);
		while (one.Place(p.fw, p.fh, 4096, 1024, x, y)) n++;
		check(n == 2, "two 1024 footprints (1536 wide) fit a 4096 x 1024 half");
	}
	// residency: least recently used first, only idle ones, nothing for nothing, ids reused after the delay
	{
		Residency r; r.Reset(8);
		const uint32_t a = r.Alloc(0), b = r.Alloc(0);
		r.Add(a, 50, 0); r.Add(b, 40, 10);
		std::vector<uint32_t> v;
		check(r.PickVictims(10, 100, 100, 600, v) && v.empty(), "fits: no victims");
		check(!r.PickVictims(30, 100, 300, 600, v) && v.empty(), "nothing idle yet: refused, nothing released");
		check(r.PickVictims(30, 100, 700, 600, v) && v.size() == 1 && v[0] == a, "the least recently used goes first");
		r.Use(a, 650);
		check(r.PickVictims(30, 100, 700, 600, v) && v.size() == 1 && v[0] == b, "a texture used recently stays");
		check(!r.PickVictims(200, 100, 5000, 600, v), "bigger than the budget: refused");
		check(r.PickVictims(95, 100, 5000, 600, v) && v.size() == 2, "both go for a big one");
		r.Free(a, 700, 64);
		check(r.alive == 40 && r.count == 1, "freed bytes");
		const uint32_t c1 = r.Alloc(710);
		check(c1 != a && c1 == 3, "a released id is not reused at once");
		const uint32_t c2 = r.Alloc(764);
		check(c2 == a, "reused after the delay");
		Residency full; full.Reset(3); full.Alloc(0); full.Alloc(0); check(full.Alloc(0) == 0, "ids run out: 0");
		// (merged with rtgap-tessblas) a pinned id (a displacement's height texture) is never a victim, even the least recently used
		Residency pin; pin.Reset(8);
		const uint32_t p1 = pin.Alloc(0), p2 = pin.Alloc(0);
		pin.Add(p1, 50, 0); pin.Add(p2, 40, 10); pin.Pin(p1);
		check(pin.PickVictims(30, 100, 5000, 600, v) && v.size() == 1 && v[0] == p2, "a pinned id stays; the next one goes");
		check(!pin.PickVictims(60, 100, 5000, 600, v), "only pinned ones left: refused");
		pin.Free(p1, 5000, 0); const uint32_t p3 = pin.Alloc(5000); pin.Add(p3, 10, 5000);
		check(!pin.Pinned(p3), "a freed id comes back unpinned");
	}
	say("  texpack: %s\n", bad ? "WRONG answers above" : "every rule as RtCore applies it");
	return bad == 0;
}

// The CPU route with BC blocks: the helper's BC3 / BC1 images (alpha from the blocks), the chain-link shadow vs the exact one
static bool sceneRealTexBc(Ctx& c) {
	Host& h = *c.host;
	const int slot = 41;
	const std::vector<HostTex::Level> levels = chainLinkLevels();
	bool all = true;
	const struct { RtTexturePack::Route r; uint32_t vk; uint32_t id; const char* label; } runs[] = {
		{ RtTexturePack::Bc3, RtTexturePack::kVkBc3, 31, "BC3 blocks (CPU route)" }, { RtTexturePack::Bc1, RtTexturePack::kVkBc1, 32, "BC1 blocks, punch-through (CPU route)" } };
	for (const auto& run : runs) {
		std::vector<uint8_t> bytes;
		uint32_t n = 0;
		for (uint32_t l = 0; l < levels.size(); l++) { const uint32_t s = std::max(1u, uint32_t(kChainRes) >> l); const auto b = HostTex::Encode(levels[l], s, s, run.r); bytes.insert(bytes.end(), b.begin(), b.end()); n++; }
		ChainResult r;
		if (!chainLinkMeasure(c, slot, chainLinkMip(kAlphaSlot), kAlphaSlot, [](int x, int y) { return chainLinkFull(x, y); }, kChainRes, [&] {
			if (!h.StageTexture(run.id, run.vk, kChainRes, kChainRes, n, bytes)) return false;
			h.SlotTextures(slot, run.id, 0, 0); return true; }, r)) return false;
		const bool ok = chainLinkRealOk(r);
		chainLinkSay(run.label, r, ok);
		all = all && ok;
		h.ReleaseTexture(run.id);
	}
	h.SlotTextures(slot, 0, 0, 0);
	return all;
}

// The game's GPU route through the fork (RtTexturePack's rules, D3D9 DEFAULT-pool DXT textures, CopyToShared into the transfer images)
static bool sceneRealTexGpu(Ctx& c) {
	Host& h = *c.host;
	using HostTex::gRoutes;
	const bool bc = gRoutes.on[RtTexturePack::Bc1];
	say("  routes in this d3d9.dll: RGBA8%s\n", bc ? " + BC1 / BC3 / BC5 (fork 1.8-rt-bc)" : " only: the fork has no BC shared images (1.8-rt-bc): the BC routes are NOT tested here");
	bool all = true;
	const bool rgbaOnly[4] = { true, false, false, false };
	struct Mode { const char* name; const bool* routes; };
	std::vector<Mode> modes = { { "RGBA8 (decoded by the fork's blit)", rgbaOnly } };
	if (bc) modes.insert(modes.begin(), { "BC (blocks as they are)", gRoutes.on });
	// 1. the chain-link card: DXT5 and DXT1 game textures, the lamp shadow vs the exact one
	const int slot = 42;
	const std::vector<HostTex::Level> levels = chainLinkLevels();
	for (const Mode& m : modes) for (int f = 0; f < 2; f++) {
		const D3DFORMAT fmt = f ? D3DFMT_DXT1 : D3DFMT_DXT5;
		const RtTexturePack::Route enc = f ? RtTexturePack::Bc1 : RtTexturePack::Bc3;
		std::vector<std::vector<uint8_t>> data;
		for (uint32_t l = 0; l < levels.size(); l++) { const uint32_t s = std::max(1u, uint32_t(kChainRes) >> l); data.push_back(HostTex::Encode(levels[l], s, s, enc)); }
		ChainResult r;
		RtTexturePack::Plan p;
		const uint32_t id = 40 + f;
		if (!chainLinkMeasure(c, slot, chainLinkMip(kAlphaSlot), kAlphaSlot, [](int x, int y) { return chainLinkFull(x, y); }, kChainRes, [&] {
			IDirect3DTexture9* t = HostTex::GameTexture(h, fmt, kChainRes, kChainRes, data);
			if (!t || !HostTex::SendGpu(h, t, id, 1024, m.routes, p)) return false;
			h.SlotTextures(slot, id, 0, 0); return true; }, r)) return false;
		const bool ok = chainLinkRealOk(r);
		char label[96]; snprintf(label, sizeof(label), "%s chain-link via %s, %u levels:", f ? "DXT1" : "DXT5", RtTexturePack::RouteName(p.route), p.levels);
		chainLinkSay(label, r, ok);
		all = all && ok;
		h.ReleaseTexture(id);
	}
	h.SlotTextures(slot, 0, 0, 0);
	// 2. a colour floor read by the path tracer (first-hit colour), the GPU route vs the same texels as BGRA8 (CPU route). A 256 texture
	// capped at 128 (the plan starts at its second level), a distinct colour pair per level (a wrong level or rectangle shows), the same
	// colour from 8 x 8 down (the BC routes stop at 4 x 4: the clamp reads the same). Exact 565 colours a 4 x 4 block: lossless BC.
	const uint32_t res = 256, cap = 128;
	auto palette = [](uint32_t l, int k) { static const uint8_t c[8][2][3] = { { { 248, 0, 0 }, { 0, 252, 0 } }, { { 0, 0, 248 }, { 248, 252, 0 } },
		{ { 0, 252, 248 }, { 248, 0, 248 } }, { { 128, 128, 128 }, { 248, 252, 248 } }, { { 200, 100, 0 }, { 0, 100, 200 } }, { { 64, 32, 96 }, { 160, 200, 40 } },
		{ { 120, 120, 40 }, { 120, 120, 40 } }, { { 120, 120, 40 }, { 120, 120, 40 } } };
		const uint8_t* q = c[std::min(l, 7u)][k]; return HostTex::Rgba{ q[0], q[1], q[2], 255 }; };
	std::vector<HostTex::Level> colour;
	for (uint32_t l = 0, s = res; s >= 1; l++, s /= 2) {
		HostTex::Level lv(size_t(s) * s);
		for (uint32_t y = 0; y < s; y++) for (uint32_t x = 0; x < s; x++) lv[size_t(y) * s + x] = s <= 8 ? palette(7, 0) : palette(l, ((x / 4) + (y / 4)) % 2);
		colour.push_back(lv);
		if (s == 1) break;
	}
	const int slotC = 62;
	World w;
	Mesh floor; floor.quad({ -6000, -6000, 0 }, { 6000, -6000, 0 }, { 6000, 6000, 0 }, { -6000, 6000, 0 });
	for (size_t i = floor.uv.size() - 8; i < floor.uv.size(); i++) floor.uv[i] *= 60.0f;
	w.meshes = { floor };
	Object fo; fo.mesh = 0; fo.textured = true; fo.albedo = { 0.5f, 0.5f, 0.5f }; fo.alphaSlot = slotC;
	w.objects = { fo };
	const Camera cam = Camera::lookDir({ 0, -3000, 260 }, { 0, 1, -0.12f }, { 0, 0, 1 }, 1.4f, 1.4f);
	const std::vector<int> imgs = { c.ptA, c.ptB, c.ptDisplay, c.ptDisplay, gM.albedoAtlas, gM.albedoAtlas, c.depth, c.normals, c.aux16, c.pano,
		c.aux16, gM.albedoAtlas, c.aux16, gM.ptAlbedoOut, gM.ptLamps };
	const World* world = &w;
	auto picture = [&](const std::function<bool()>& setup, std::vector<float>& out) {
		HostTex::ResetShelves();
		if (!setup()) return false;
		std::vector<uint32_t> ids;
		if (!stageWorld(h, *world, ids)) return false;
		PathConstants k = {};
		cameraConstants(cam, k.size, k.forward, k.right, k.up, k.origin);
		k.origin[3] = 9.0f;
		put4(k.accum, 0.0f, 1.0f, 1.0f, 2.0f);
		put4(k.sunDir, 0, 0, 1, 0); put4(k.skyZenith, 0.6f, 0.7f, 0.8f, 1.0f); put4(k.skyHorizon, 0.6f, 0.7f, 0.8f, 0.0f);
		put4(k.misc, 0.0f, 0.0f, 60000.0f, 1.0f);   // DebugView 1: the first hit's colour
		k.rect[3] = 1.0f;
		h.AddPass(c.shPath, imgs, 0x7u, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
		if (!h.Exchange(*world, ids, cam.eye)) return false;
		const bool ok = h.Read(c.ptDisplay, out);
		h.EndFrame();
		releaseWorld(h, ids);
		return ok;
	};
	auto compare = [&](const char* label, const std::vector<float>& a, const std::vector<float>& ref, bool& ok) {
		double sum = 0, worst = 0; int n = 0, off = 0;
		for (size_t q = 0; q < size_t(S) * S; q++) {
			if (!(ref[q * 4 + 3] > 0.0f) && ref[q * 4] == 0.0f && ref[q * 4 + 1] == 0.0f) continue;
			double d = 0; for (int ch = 0; ch < 3; ch++) d = std::max(d, (double)std::fabs(a[q * 4 + ch] - ref[q * 4 + ch]));
			sum += d; worst = std::max(worst, d); n++; off += d > 0.02;
		}
		const double mean = n ? sum / n : 1.0;
		ok = n > 4000 && mean < 0.004 && off <= n / 200;
		say("  %-44s %d pixels: mean difference %.4f, largest %.4f, %d over 0.02 -> %s\n", label, n, mean, worst, off, ok ? "ok" : "BAD");
		if (!ok) { dumpPfm("realtexgpu_route", a, S, S, 4, 0); dumpPfm("realtexgpu_reference", ref, S, S, 4, 0); }
	};
	// the reference: the texels the GPU route should deliver, from the plan's first level (128) down, as BGRA8 on the CPU route
	std::vector<float> ref;
	const uint32_t refId = 50;
	if (!picture([&] {
		std::vector<HostTex::Level> dec;
		for (uint32_t l = 0; l < colour.size(); l++) { const uint32_t s = std::max(1u, res >> l); dec.push_back(HostTex::Decode(HostTex::Encode(colour[l], s, s, RtTexturePack::Bc1), s, s, RtTexturePack::Bc1)); }
		if (!h.StageTexture(refId, RtTexturePack::kVkBgra8, cap, cap, uint32_t(dec.size() - 1), HostTex::Bgra(dec, 1))) return false;
		h.SlotTextures(slotC, refId, 0, 0); return true; }, ref)) return false;
	for (const Mode& m : modes) {
		std::vector<float> got;
		RtTexturePack::Plan p;
		const uint32_t id = 51;
		std::vector<std::vector<uint8_t>> data;
		for (uint32_t l = 0; l < colour.size(); l++) { const uint32_t s = std::max(1u, res >> l); data.push_back(HostTex::Encode(colour[l], s, s, RtTexturePack::Bc1)); }
		if (!picture([&] {
			IDirect3DTexture9* t = HostTex::GameTexture(h, D3DFMT_DXT1, res, res, data);
			if (!t || !HostTex::SendGpu(h, t, id, cap, m.routes, p)) return false;
			h.SlotTextures(slotC, id, 0, 0); return true; }, got)) return false;
		bool ok = false;
		char label[96]; snprintf(label, sizeof(label), "DXT1 colour via %s (first level %u, %u levels):", RtTexturePack::RouteName(p.route), p.first, p.levels);
		compare(label, got, ref, ok);
		all = all && ok && p.first == 1;
		h.ReleaseTexture(id);
	}
	// 3. ATI2 (two channels) as BC5 with the (G, R, 1, 1) view: the reference is the swizzled texels as BGRA8
	if (bc) {
		std::vector<HostTex::Level> two;
		for (uint32_t l = 0, s = res; ; l++, s /= 2) {
			HostTex::Level lv(size_t(s) * s);
			for (uint32_t y = 0; y < s; y++) for (uint32_t x = 0; x < s; x++) { const HostTex::Rgba p = s <= 8 ? palette(7, 0) : palette(l, ((x / 4) + (y / 4)) % 2); lv[size_t(y) * s + x] = { p.r, p.g, 0, 255 }; }
			two.push_back(lv);
			if (s == 1) break;
		}
		std::vector<float> refT, got;
		if (!picture([&] {
			std::vector<HostTex::Level> sw;
			for (uint32_t l = 0; l < two.size(); l++) {
				const uint32_t s = std::max(1u, res >> l);
				HostTex::Level d = HostTex::Decode(HostTex::Encode(two[l], s, s, RtTexturePack::Bc5), s, s, RtTexturePack::Bc5);
				for (auto& p : d) p = { p.g, p.r, 255, 255 };   // the view: r = G, g = R, b = 1, a = 1
				sw.push_back(d);
			}
			if (!h.StageTexture(refId + 2, RtTexturePack::kVkBgra8, cap, cap, uint32_t(sw.size() - 1), HostTex::Bgra(sw, 1))) return false;
			h.SlotTextures(slotC, refId + 2, 0, 0); return true; }, refT)) return false;
		std::vector<std::vector<uint8_t>> data;
		for (uint32_t l = 0; l < two.size(); l++) { const uint32_t s = std::max(1u, res >> l); data.push_back(HostTex::Encode(two[l], s, s, RtTexturePack::Bc5)); }
		{	// the helper's BC5 + swizzle alone: the same blocks on the CPU route (levels 128 .. 4, as the GPU route sends them)
			std::vector<float> cpu;
			std::vector<uint8_t> bytes;
			for (uint32_t l = 1; l + 2 < data.size(); l++) bytes.insert(bytes.end(), data[l].begin(), data[l].end());
			if (!picture([&] { if (!h.StageTexture(54, RtTexturePack::kVkBc5, cap, cap, uint32_t(data.size() - 3) | NVR_RT_TEX_SWIZZLE_GR, bytes)) return false;
				h.SlotTextures(slotC, 54, 0, 0); return true; }, cpu)) return false;
			bool okCpu = false;
			compare("BC5 + (G, R, 1, 1) view, CPU route:", cpu, refT, okCpu);
			all = all && okCpu;
			h.ReleaseTexture(54);
		}
		RtTexturePack::Plan p;
		if (!picture([&] {
			IDirect3DTexture9* t = HostTex::GameTexture(h, (D3DFORMAT)RtTexturePack::kAti2, res, res, data);
			if (!t || !HostTex::SendGpu(h, t, 53, cap, gRoutes.on, p)) return false;
			h.SlotTextures(slotC, 53, 0, 0); return true; }, got)) return false;
		bool ok = false;
		compare("ATI2 via BC5 + (G, R, 1, 1) view:", got, refT, ok);
		all = all && ok && p.route == RtTexturePack::Bc5;
		h.ReleaseTexture(53); h.ReleaseTexture(refId + 2);
	}
	// 4. sizes above 1024 (RealTextureMaxSize 2048: the 12 / 16 GB tiers): a 2048 DXT1 texture over the whole floor (its finest level
	// reaches the near pixels), sent with cap 2048 on every route this d3d9.dll has: the routes must agree, and differ from cap 1024 (a
	// level of its own colours per mip: the 2048 level must show)
	{
		const uint32_t big = 2048;
		std::vector<std::vector<uint8_t>> data;
		for (uint32_t l = 0, s = big; ; l++, s /= 2) {
			HostTex::Level lv(size_t(s) * s);
			for (uint32_t y = 0; y < s; y++) for (uint32_t x = 0; x < s; x++) lv[size_t(y) * s + x] = s <= 8 ? palette(7, 0) : palette(l, ((x / 4) + (y / 4)) % 2);
			data.push_back(HostTex::Encode(lv, s, s, RtTexturePack::Bc1));
			if (s == 1) break;
		}
		World one = w;
		for (size_t i = 0; i < one.meshes[0].uv.size(); i++) one.meshes[0].uv[i] /= 60.0f;	// one repeat over the floor
		world = &one;
		std::vector<float> first;
		uint32_t firstLevel0 = 0;
		for (size_t mi = 0; mi < modes.size(); mi++) {
			std::vector<float> got;
			RtTexturePack::Plan p;
			if (!picture([&] {
				IDirect3DTexture9* t = HostTex::GameTexture(h, D3DFMT_DXT1, big, big, data);
				if (!t || !HostTex::SendGpu(h, t, 55, 2048, modes[mi].routes, p)) return false;
				h.SlotTextures(slotC, 55, 0, 0); return true; }, got)) { world = &w; return false; }
			h.ReleaseTexture(55);
			char label[96]; snprintf(label, sizeof(label), "2048 DXT1 via %s (cap 2048: level 0 %u):", RtTexturePack::RouteName(p.route), p.w0);
			all = all && p.w0 == 2048;
			if (mi == 0) { first = got; firstLevel0 = p.w0; say("  %-44s sent, %u levels\n", label, p.levels); continue; }
			bool ok = false;
			compare(label, got, first, ok);
			all = all && ok;
		}
		std::vector<float> capped;
		RtTexturePack::Plan p;
		if (!picture([&] {
			IDirect3DTexture9* t = HostTex::GameTexture(h, D3DFMT_DXT1, big, big, data);
			if (!t || !HostTex::SendGpu(h, t, 56, 1024, modes[0].routes, p)) return false;
			h.SlotTextures(slotC, 56, 0, 0); return true; }, capped)) { world = &w; return false; }
		h.ReleaseTexture(56);
		int differ = 0, n = 0;
		for (size_t q = 0; q < size_t(S) * S; q++) { n++; double d = 0; for (int ch = 0; ch < 3; ch++) d = std::max(d, (double)std::fabs(capped[q * 4 + ch] - first[q * 4 + ch])); differ += d > 0.02; }
		const bool shows = differ > n / 50;
		say("  the same at cap 1024 (level 0 %u): %d of %d pixels differ from cap 2048 (the 2048 level is read) -> %s\n", p.w0, differ, n, shows ? "ok" : "BAD");
		all = all && shows && firstLevel0 == 2048;
		world = &w;
	}
	h.ReleaseTexture(refId);
	h.SlotTextures(slotC, 0, 0, 0);
	return all;
}

// TEXTURE_RELEASE (RtCore releases textures unused for a while when RealTextureBudgetMB is full): the slot falls back to the atlas at
// once, and the id given out again carries the new texture
static bool sceneTexRelease(Ctx& c) {
	Host& h = *c.host;
	const int slot = 43;
	const uint32_t id = 60;
	const AlphaFn clear = [](int, int) { return 0.0f; };
	const AlphaFn solid = [](int, int) { return 1.0f; };
	bool all = true;
	// a. an opaque real texture over a clear atlas slot: the whole card shadows
	{
		HostTex::Level top(64 * 64, { 90, 90, 90, 255 });
		const auto levels = HostTex::Mips(top, 64);
		std::vector<uint8_t> bytes; for (uint32_t l = 0; l < levels.size(); l++) { const uint32_t s = 64u >> l; const auto b = HostTex::Encode(levels[l], s, s, RtTexturePack::Bc3); bytes.insert(bytes.end(), b.begin(), b.end()); }
		ChainResult r;
		if (!chainLinkMeasure(c, slot, clear, 64, solid, 64, [&] { if (!h.StageTexture(id, RtTexturePack::kVkBc3, 64, 64, uint32_t(levels.size()), bytes)) return false; h.SlotTextures(slot, id, 0, 0); return true; }, r)) return false;
		const bool ok = r.both > 1000 && r.agree > 0.995f && r.cShare > 0.05f;
		chainLinkSay("a. opaque real texture (atlas slot clear):", r, ok);
		all = all && ok;
	}
	// b. released (no SLOT_TEXTURES: the helper drops the id from every slot): the clear atlas shows, no shadow
	{
		ChainResult r;
		if (!chainLinkMeasure(c, slot, clear, 64, clear, 64, [&] { h.ReleaseTexture(id); return true; }, r)) return false;
		const bool ok = r.both > 1000 && r.gShare < 0.002f && r.agree > 0.995f;
		chainLinkSay("b. released: the atlas (clear) again:", r, ok);
		all = all && ok;
	}
	// c. the id given out again for the chain-link (BC3), the slot pointed at it
	{
		const std::vector<HostTex::Level> levels = chainLinkLevels();
		std::vector<uint8_t> bytes; uint32_t n = 0;
		for (uint32_t l = 0; l < levels.size(); l++) { const uint32_t s = std::max(1u, uint32_t(kChainRes) >> l); const auto b = HostTex::Encode(levels[l], s, s, RtTexturePack::Bc3); bytes.insert(bytes.end(), b.begin(), b.end()); n++; }
		ChainResult r;
		if (!chainLinkMeasure(c, slot, clear, 64, [](int x, int y) { return chainLinkFull(x, y); }, kChainRes,
			[&] { if (!h.StageTexture(id, RtTexturePack::kVkBc3, kChainRes, kChainRes, n, bytes)) return false; h.SlotTextures(slot, id, 0, 0); return true; }, r)) return false;
		const bool ok = chainLinkRealOk(r);
		chainLinkSay("c. the id reused for the chain-link (BC3):", r, ok);
		all = all && ok;
	}
	h.ReleaseTexture(id);
	h.SlotTextures(slot, 0, 0, 0);
	{ std::vector<uint32_t> none; World e; if (!h.Exchange(e, none, V3()) || !h.Finish()) return false; h.EndFrame(); }
	return all;
}

// main: the game's transfer images (RtCore::Start: RGBA8 always, BC1 / BC3 / BC5 when the fork makes BC shared images)
static bool registerTextureRoutes(Host& h) {
	using HostTex::gRoutes;
	NVR_EXT_SHARED_IMAGE_DESC probe = { 256, 256, NVR_EXT_SHARED_BC1, "BC probe" };
	ID3D9NvrExtSharedImage* img = nullptr;
	const bool bc = SUCCEEDED(h.share->CreateSharedImage(&probe, &img)) && img;
	if (img) img->Release();
	static const NVR_EXT_SHARED_FORMAT formats[4] = { NVR_EXT_SHARED_RGBA8, NVR_EXT_SHARED_BC1, NVR_EXT_SHARED_BC3, NVR_EXT_SHARED_BC5 };
	static const char* names[4] = { "texture transfer RGBA8", "texture transfer BC1", "texture transfer BC3", "texture transfer BC5" };
	h.shared->textureImage = 0;
	for (uint32_t r = 0; r < RtTexturePack::RouteCount; r++) {
		gRoutes.on[r] = r == RtTexturePack::Rgba8 || bc;
		if (!gRoutes.on[r]) continue;
		RtTexturePack::TransferSize(2048, gRoutes.w[r], gRoutes.halfH[r]);	// (the 12 / 16 GB tiers' size: realtexgpu sends a 2048 texture)
		gRoutes.image[r] = h.AddImage(names[r], gRoutes.w[r], 2 * gRoutes.halfH[r], formats[r]);
		if (gRoutes.image[r] < 0) return false;
		h.shared->textureImage |= uint32_t(gRoutes.image[r] + 1) << (8 * r);
	}
	say("texture routes: RGBA8%s (shared images %zu)\n", bc ? " + BC1 / BC3 / BC5 (fork 1.8-rt-bc)" : " only (this d3d9.dll has no BC shared images)", h.images.size());
	return true;
}
