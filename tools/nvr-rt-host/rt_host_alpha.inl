// rt_host_alpha.inl (2026-11-06): fine alpha-tested cut-outs through the RT alpha atlas (included by rt_host.cpp).
// The in-game chain-link fence's traced lamp shadow came out as rows of blobs where the cube map drew clean diamonds: the alpha atlas
// keeps each texture at RT_ALPHA_SLOT_SIZE (64) texels, made from the texture's own mips (RtAlphaToAtlas.frag), and a 2-3 texel wire
// of a 512 texture does not survive that. Scene: a 512 x 512 chain-link pattern (diagonal wires) on a card tiled 3 x 3 between a
// point lamp and the floor. The GPU gets what the game's fill pass makes of it (the mip of the slot's size, a box filter); the CPU
// traces the full 512 x 512 pattern. Reported: the shadowed share of the floor (GPU vs exact), the pixels that agree, and the RMSE
// of 8 x 8 pixel tiles' mean visibility (what the eye sees once the shadow is smoothed). PASS = the tile RMSE under 0.15 and the
// shadowed share within 0.05 of the exact one (the shipped 64-texel slots are expected to FAIL this until fixed).

#include <memory>

static const int kChainRes = 512, kChainPeriod = 64, kChainWire = 4;   // 8 diamonds across, wires 4 texels across (diagonal)
static float chainLinkFull(int x, int y) {
	const int s = ((x + y) % kChainPeriod + kChainPeriod) % kChainPeriod, t = ((x - y) % kChainPeriod + kChainPeriod) % kChainPeriod;
	return (s < kChainWire || t < kChainWire) ? 1.0f : 0.0f;
}
// the game's fill: the texture's mip at `res` texels (a box filter over the full texture), alpha as it comes out
static AlphaFn chainLinkMip(int res) {
	const int k = kChainRes / res;
	auto table = std::make_shared<std::vector<float>>(res * res);
	for (int y = 0; y < res; y++) for (int x = 0; x < res; x++) {
		float a = 0; for (int j = 0; j < k; j++) for (int i = 0; i < k; i++) a += chainLinkFull(x * k + i, y * k + j);
		(*table)[y * res + x] = a / float(k * k);
	}
	return [table, res](int x, int y) { return (*table)[std::min(std::max(y, 0), res - 1) * res + std::min(std::max(x, 0), res - 1)]; };
}

// (2026-11-07, protocol 7) the real texture: the full 512 x 512 pattern as BGRA8 (alpha = the pattern), every mip (box filtered), packed
static std::vector<uint8_t> chainLinkTexture(uint32_t& levels) {
	std::vector<uint8_t> out;
	std::vector<float> a(kChainRes * kChainRes);
	for (int y = 0; y < kChainRes; y++) for (int x = 0; x < kChainRes; x++) a[y * kChainRes + x] = chainLinkFull(x, y);
	int r = kChainRes;
	levels = 0;
	while (true) {
		for (int i = 0; i < r * r; i++) { const uint8_t v = uint8_t(a[i] * 255.0f + 0.5f); out.push_back(200); out.push_back(180); out.push_back(160); out.push_back(v); }
		levels++;
		if (r == 1) break;
		const int n = r / 2;
		std::vector<float> b(n * n);
		for (int y = 0; y < n; y++) for (int x = 0; x < n; x++)
			b[y * n + x] = 0.25f * (a[(2 * y) * r + 2 * x] + a[(2 * y) * r + 2 * x + 1] + a[(2 * y + 1) * r + 2 * x] + a[(2 * y + 1) * r + 2 * x + 1]);
		a.swap(b); r = n;
	}
	return out;
}

static bool sceneChainLink(Ctx& c, bool real = false) {
	Host& h = *c.host;
	const int slot = 40, tiles = 3;
	World w;
	Mesh floor; floor.quad({ -4000, -4000, 0 }, { 4000, -4000, 0 }, { 4000, 4000, 0 }, { -4000, 4000, 0 });
	Mesh card; card.quad({ -700, -700, 260 }, { 700, -700, 260 }, { 700, 700, 260 }, { -700, 700, 260 });
	for (size_t i = card.uv.size() - 8; i < card.uv.size(); i++) card.uv[i] *= float(tiles);   // the texture repeats 3 x 3
	w.meshes = { floor, card };
	Object f; f.mesh = 0; f.id = 1;
	Object k; k.mesh = 1; k.id = 2; k.alphaTested = true; k.alphaSlot = slot; k.alphaRef = 128;
	w.objects = { f, k };
	// the GPU's atlas slot: the 64-texel mip (as RtAlphaToAtlas fills it); the CPU world: the full texture
	const AlphaFn mip = chainLinkMip(kAlphaSlot);
	if (!uploadSlot(h, c.alphaAtlas, slot, mip, kAlphaSlot)) return false;
	const uint32_t realId = 7;
	if (real) {	// (2026-11-07, protocol 7) the full texture over the real-textures path, the slot pointed at it (the atlas stays as the fallback)
		uint32_t levels = 0;
		const std::vector<uint8_t> tex = chainLinkTexture(levels);
		if (!h.StageTexture(realId, 44 /* VK_FORMAT_B8G8R8A8_UNORM */, kChainRes, kChainRes, levels, tex)) return false;
		h.SlotTextures(slot, realId, 0, 0);
	}
	World exact = w; exact.slots[slot] = [](int x, int y) { return chainLinkFull(x, y); }; exact.slotRes[slot] = kChainRes;
	w.slots[slot] = mip; w.slotRes[slot] = kAlphaSlot;
	std::vector<uint32_t> ids;
	if (!stageWorld(h, w, ids)) return false;
	const std::vector<Lamp> lamps = { { V3(0, 0, 900), 0, 0, 0 }, { V3(0, 0, 900), 0, 0, 0 }, { V3(0, 0, 900), 0, 0, 0 },
		{ V3(0, 0, 900), 0, 0, 0 }, { V3(0, 0, 900), 0, 0, 0 }, { V3(37, -23, 1100), 8000, 1.0f, 8000 } };   // slot 5, a point lamp
	const Camera cam = Camera::lookDir({ 0, 0, 2400 }, { 0, 0, -1 }, { 0, 1, 0 }, 1.6f, 1.6f);
	int checked = 0, bad = 0;
	std::vector<float> res;
	if (!runLampTrace(c, w, ids, lamps, cam, 1, "chain-link (vs its own 64 texels)", checked, bad, nullptr, &res)) return false;
	// the exact answer: every pixel's floor point traced to the lamp through the full 512 x 512 pattern
	std::vector<GPixel> px; std::vector<float> depth, normals;
	gbuffer(exact, cam, px, depth, normals);
	std::vector<float> gpu(S * S, -1.0f), cpu(S * S, -1.0f);
	int both = 0, agree = 0, gpuShadow = 0, cpuShadow = 0;
	for (UINT i = 0; i < S * S; i++) {
		if (!px[i].valid || px[i].object != 0) continue;   // floor pixels only
		const int e0 = int(res[i * 2]) % 1024;
		if (e0 % 64 != 5) continue;
		gpu[i] = float(e0 / 64) / 15.0f;
		const V3 origin = px[i].pos + px[i].n * 0.5f;
		cpu[i] = lampLitCpu(exact, origin, lamps[5], 0.5f) ? 1.0f : 0.0f;
		both++; agree += (gpu[i] > 0.5f) == (cpu[i] > 0.5f); gpuShadow += gpu[i] < 0.5f; cpuShadow += cpu[i] < 0.5f;
	}
	// 8 x 8 tiles under the card's shadow (tiles with some shadow in the exact answer)
	double se = 0; int tiles8 = 0;
	for (UINT ty = 0; ty < S / 8; ty++) for (UINT tx = 0; tx < S / 8; tx++) {
		double g = 0, e = 0; int n = 0, sh = 0;
		for (UINT y = 0; y < 8; y++) for (UINT x = 0; x < 8; x++) {
			const UINT i = (ty * 8 + y) * S + tx * 8 + x;
			if (gpu[i] < 0) continue;
			g += gpu[i]; e += cpu[i]; n++; sh += cpu[i] < 0.5f;
		}
		if (n < 48 || sh == 0) continue;
		se += (g / n - e / n) * (g / n - e / n); tiles8++;
	}
	const float rmse = tiles8 ? float(std::sqrt(se / tiles8)) : 1.0f;
	const float gShare = both ? float(gpuShadow) / both : 0, cShare = both ? float(cpuShadow) / both : 0;
	// (the shadow is thin wires: ~5 % of the floor; judge it relative to itself) the GPU must find the shadow at all and get its density
	// within 30 % of the exact share, and its 8 x 8 tiles (what the eye sees once the denoiser / TAA smooth the shadow) within 0.08.
	// Pixel for pixel the wires cannot match: they are under a texel of the 64-texel slot (the share found is reported, not judged).
	int recall = 0, exactShadow = 0;
	for (UINT i = 0; i < S * S; i++) if (gpu[i] >= 0 && cpu[i] < 0.5f) { exactShadow++; recall += gpu[i] < 0.5f; }
	const float recallShare = exactShadow ? float(recall) / exactShadow : 0;
	// real texture (protocol 7): the full pattern itself, read filtered (bilinear, as the game's own alpha test reads it), against the exact
	// answer's nearest texel: they differ only where filtering rounds the diagonal wires' texel steps (measured 2026-11-07: 99.5 % of the
	// pixels agree, 94.3 % of the exact shadow found, tiles 0.020; the 64-texel atlas: 98.4 %, 80.8 %, 0.035). Pass: 99 %, 90 %, 0.025.
	const bool ok = real ? both > 1000 && tiles8 > 50 && agree > 0.99f * both && recallShare > 0.9f && std::fabs(gShare - cShare) < 0.1f * cShare && rmse < 0.025f
		: both > 1000 && tiles8 > 50 && cShare > 0.01f && std::fabs(gShare - cShare) < 0.3f * cShare && rmse < 0.08f;
	say("  chain-link (512 texels, 4-texel wires, 3 x 3 tiles; GPU = %s): %d floor pixels, agree %.1f %%, shadowed %.3f (exact %.3f), "
		"exact shadow found %.1f %%, 8x8 tile RMSE %.3f over %d tiles%s\n", real ? "the REAL 512 texture, protocol 7" : "its 64-texel atlas mip", both, both ? 100.0f * agree / both : 0.0f, gShare, cShare, 100.0f * recallShare, rmse, tiles8,
		ok ? "" : real ? "  <- the real texture does not reach the rays" : "  <- the atlas loses the pattern");
	if (!ok) { dumpPfm("chainlink_gpu", gpu, S, S, 1, 0); dumpPfm("chainlink_exact", cpu, S, S, 1, 0); }
	releaseWorld(h, ids);
	return ok;
}
