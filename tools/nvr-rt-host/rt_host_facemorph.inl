// ==============================================================================================================================
// Scene `facemorph` (2026-11-07): faces and lip-sync in the RT scene ([Main.Main.RayTracing] FaceMorphs; src/core/RtFaceMorph.h;
// docs/next/RT-DETAIL-GAPS.md section 4). Two parts:
//  A. no GPU: RtFaceMorph's hash (a morph of a few dozen vertices is seen, a change outside the positions is not), its decisions
//     (partition follows / never follows / nothing changes / a stray miss / only the own buffer changes) and PatchPositions (through
//     a vertex map; out-of-range maps refused).
//  B. GPU, through the fork's StageSkinnedGeometry (patch 0033) and the helper, as RtCore::AddSkinned does it: a "head" (a 24 x 24
//     grid, skinned to one bone that never moves, the partition's layout: position, weights, indices, 28 bytes) with ActorStillSkip on,
//     so a pose happens only when FaceObserve's hash asks for one. The mouth opens in the partition's buffer -> posed that frame, the
//     camera rays (RtLampPrimary) see the open mouth; frames with nothing changed send no BUILD op (cost). Then only the CPU vertices
//     (data order, a reversed vertex map) keep changing while the partition's buffer stays: within a few frames the poses switch to a
//     patched copy and the rays see each new mouth. Control: FaceMorphs off, the same mouth change -> no pose, the rays keep the shut
//     mouth (> 50 pixels wrong: the test sees the gap).
// PASS: every A check; B: depth = the CPU's exact rays (stable pixels) in every checked frame, 0 ops on unchanged frames, the switch
// within 8 frames, the control's stale pixels > 50.

static bool faceChecksNoGpu() {
	using namespace RtFaceMorph;
	int bad = 0;
	auto expect = [&](bool ok, const char* what) { if (!ok) { bad++; say("    A FAIL: %s\n", what); } };
	// --- the hash
	const uint32_t V = 1500, stride = 28;
	std::vector<uint8_t> buf(V * stride);
	uint32_t seed = 12345;
	auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed; };
	for (uint32_t i = 0; i < V; i++) {
		float p[3] = { float(rnd() % 1000) * 0.1f, float(rnd() % 1000) * 0.1f, float(rnd() % 1000) * 0.1f };
		std::memcpy(&buf[i * stride], p, 12);
		for (uint32_t b = 12; b < stride; b++) buf[i * stride + b] = uint8_t(rnd());
	}
	const uint32_t h0 = Hash(buf.data(), V, stride, 0);
	expect(Hash(buf.data(), V, stride, 0) == h0, "the same vertices hash the same");
	{ std::vector<uint8_t> b2 = buf; b2[5 * stride + 13] ^= 0xFF; expect(Hash(b2.data(), V, stride, 0) == h0, "a byte outside the positions (weights) changes nothing"); }
	int seen = 0, trials = 200;
	for (int t = 0; t < trials; t++) {	// a morph: 40 random vertices moved 0.25 units (a small mouth shape)
		std::vector<uint8_t> b2 = buf;
		for (int k = 0; k < 40; k++) { float* p = (float*)&b2[(rnd() % V) * stride]; p[2] -= 0.25f; }
		if (Hash(b2.data(), V, stride, 0) != h0) seen++;
	}
	expect(seen == trials, "every 40-vertex morph changes the hash");
	int single = 0;
	for (uint32_t i = 0; i < V; i++) { std::vector<uint8_t> b2 = buf; ((float*)&b2[i * stride])[0] += 1.0f; if (Hash(b2.data(), V, stride, 0) != h0) single++; }
	expect(single == int(kSamples), "a single moved vertex is seen exactly when it is one of the sampled ones");
	say("    hash: 40-vertex morphs seen %d of %d; single moved vertices seen %d of %u (the %u sampled)\n", seen, trials, single, V, kSamples);

	// --- the decisions
	auto run = [&](const char* what, int frames, std::function<void(int, Sample&)> fill, std::function<bool(const State&, const std::vector<Decision>&)> check) {
		State s; std::vector<Decision> ds;
		for (int f = 0; f < frames; f++) { Sample in; fill(f, in); ds.push_back(Observe(s, in)); }
		const bool ok = check(s, ds);
		if (!ok) say("    A FAIL: %s (feed %d, misses %u, followed %u, forced %u)\n", what, s.feed, s.misses, s.followed, s.forced);
		else say("    %s: feed %s, %u changes of the partition, %u of the CPU vertices, %u missed, %u followed, %u poses asked for\n", what,
			s.feed == FeedPartition ? "partition" : s.feed == FeedCpu ? "CPU copy" : "own-buffer copy", s.part.changes, s.cpu.changes, s.misses, s.followed, s.forced);
		if (!ok) bad++;
	};
	run("nothing changes (60 frames)", 60, [](int, Sample& in) { in.part = in.cpu = true; in.partHash = 7; in.cpuHash = 9; },
		[](const State& s, const std::vector<Decision>& d) { for (auto& x : d) if (x.force) return false; return s.feed == FeedPartition; });
	run("talking, the partition follows in the same frame", 60, [](int f, Sample& in) { in.part = in.cpu = true; in.partHash = 100 + f; in.cpuHash = 500 + f; },
		[](const State& s, const std::vector<Decision>& d) { for (size_t i = 1; i < d.size(); i++) if (!d[i].force) return false; return s.feed == FeedPartition && s.misses == 0; });
	run("talking, the partition follows a frame late", 60, [](int f, Sample& in) { in.part = in.cpu = true; in.partHash = 100 + (f > 0 ? f - 1 : 0); in.cpuHash = 500 + f; },
		[](const State& s, const std::vector<Decision>& d) { for (size_t i = 2; i < d.size(); i++) if (!d[i].force) return false; return s.feed == FeedPartition && s.misses == 0; });
	run("talking, the partition never follows", 30, [](int f, Sample& in) { in.part = in.cpu = true; in.partHash = 100; in.cpuHash = 500 + f; },
		[](const State& s, const std::vector<Decision>& d) {
			int sw = -1; for (size_t i = 0; i < d.size(); i++) if (d[i].switched) { sw = int(i); break; }
			if (sw < 0 || sw > 8 || s.feed != FeedCpu) return false;
			for (size_t i = sw; i < d.size(); i++) if (!d[i].force) return false;
			return true; });
	run("blinks seen by the CPU sample only now and then (one miss, many follows)", 120, [](int f, Sample& in) {
			in.part = in.cpu = true; in.cpuHash = 500 + f / 4;
			in.partHash = (f >= 40 && f < 44) ? 100 + 9 : 100 + f / 4;	// at 40-43 the partition's sampled vertices miss one change
		},
		[](const State& s, const std::vector<Decision>&) { return s.feed == FeedPartition && s.followed > s.misses; });
	run("only the data's own buffer changes (no CPU vertices)", 30, [](int f, Sample& in) { in.part = in.own = true; in.partHash = 100; in.ownHash = 700 + f; },
		[](const State& s, const std::vector<Decision>&) { return s.feed == FeedOwn; });
	run("only the partition changes (no CPU vertices)", 30, [](int f, Sample& in) { in.part = true; in.partHash = 100 + f / 2; },
		[](const State& s, const std::vector<Decision>& d) { int n = 0; for (auto& x : d) n += x.force; return s.feed == FeedPartition && n == 14; });
	run("the partition's buffer unreadable some frames", 40, [](int f, Sample& in) { in.part = (f % 3) != 1; in.cpu = true; in.partHash = 100 + f; in.cpuHash = 500 + f; },
		[](const State& s, const std::vector<Decision>&) { return s.feed == FeedPartition; });

	// --- PatchPositions
	{
		const uint32_t n = 6, st = 20;
		std::vector<uint8_t> dst(n * st, 0xAB);
		std::vector<float> src = { 0, 0, 0, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 5, 6, 6, 6 };	// 7 data vertices
		const uint16_t map[n] = { 6, 0, 3, 3, 1, 5 };
		expect(PatchPositions(dst.data(), n, st, 4, (const uint8_t*)src.data(), 7, 12, 0, map), "PatchPositions through a vertex map");
		bool ok = true;
		for (uint32_t i = 0; i < n; i++) {
			float p[3]; std::memcpy(p, &dst[i * st + 4], 12);
			ok = ok && p[0] == float(map[i]) && p[1] == float(map[i]) && p[2] == float(map[i]);
			for (uint32_t b = 0; b < 4; b++) ok = ok && dst[i * st + b] == 0xAB;
			for (uint32_t b = 16; b < st; b++) ok = ok && dst[i * st + b] == 0xAB;
		}
		expect(ok, "patched positions = the mapped data vertices, other bytes untouched");
		const uint16_t badMap[n] = { 0, 1, 2, 7, 1, 1 };
		std::vector<uint8_t> d2(n * st, 0xCD);
		expect(!PatchPositions(d2.data(), n, st, 4, (const uint8_t*)src.data(), 7, 12, 0, badMap), "a map past the data refused");
		bool untouched = true; for (uint8_t b : d2) untouched = untouched && b == 0xCD;
		expect(untouched, "a refused patch leaves the buffer as it was");
	}
	// --- MapAgreement: a stale partition (40 of 1500 vertices morphed) agrees with the right map; a wrong map (or offset) does not
	{
		std::vector<float> data(V * 3);
		for (uint32_t v = 0; v < V; v++) { data[v * 3] = float(rnd() % 1000) * 0.1f; data[v * 3 + 1] = float(rnd() % 1000) * 0.1f; data[v * 3 + 2] = float(rnd() % 1000) * 0.1f; }
		std::vector<uint16_t> map(V);
		for (uint32_t i = 0; i < V; i++) map[i] = uint16_t((i * 7u) % V);	// 7 and 1500 coprime: a permutation
		std::vector<uint8_t> part(V * stride, 0);
		for (uint32_t i = 0; i < V; i++) std::memcpy(&part[i * stride + 4], &data[map[i] * 3], 12);
		for (int k = 0; k < 40; k++) data[(rnd() % V) * 3 + 2] += 5.0f;	// the CPU vertices morphed, the partition stale
		uint32_t tested = 0;
		const uint32_t right = MapAgreement(part.data(), V, stride, 4, (const uint8_t*)data.data(), V, 12, 0, map.data(), tested);
		const uint32_t none = MapAgreement(part.data(), V, stride, 4, (const uint8_t*)data.data(), V, 12, 0, nullptr, tested);
		const uint32_t shifted = MapAgreement(part.data(), V, stride, 8, (const uint8_t*)data.data(), V, 12, 0, map.data(), tested);
		expect(right * 10 >= tested * 9, "the right map agrees on most sampled vertices");
		expect(none * 2 < tested && shifted * 2 < tested, "no map / a wrong position offset agree on few");
		say("    map agreement of %u sampled: right map %u, no map %u, offset off by 4 bytes %u\n", tested, right, none, shifted);
	}
	return bad == 0;
}

// ---- part B: the GPU rig
static const UINT kFaceGrid = 24, kFaceStride = 28;
static V3 facePoint(UINT i, UINT j, float open) {	// the grid on the plane y = 300, facing the camera; the mouth pushed toward it by `open`
	const float x = -300.0f + 600.0f * i / (kFaceGrid - 1), z = -140.0f + 600.0f * j / (kFaceGrid - 1);
	const bool mouth = std::fabs(x) <= 110.0f && z >= 50.0f && z <= 170.0f;
	return { x, 300.0f - (mouth ? open : 0.0f), z };
}
static Mesh faceMeshOf(float open) {
	Mesh m;
	for (UINT j = 0; j < kFaceGrid; j++) for (UINT i = 0; i < kFaceGrid; i++) m.pos.push_back(facePoint(i, j, open));
	for (UINT j = 0; j + 1 < kFaceGrid; j++) for (UINT i = 0; i + 1 < kFaceGrid; i++) {
		const uint16_t a = uint16_t(j * kFaceGrid + i), b = uint16_t(a + 1), c = uint16_t(a + kFaceGrid + 1), d = uint16_t(a + kFaceGrid);
		const uint16_t t[6] = { a, b, c, a, c, d };
		m.idx.insert(m.idx.end(), t, t + 6);
	}
	return m;
}
// as RtCore's LockRead: read-only, no wait
static const uint8_t* faceLockRead(IDirect3DVertexBuffer9* vb, UINT first, UINT bytes) {
	void* bits = nullptr;
	if (FAILED(vb->Lock(first, bytes, &bits, D3DLOCK_READONLY | D3DLOCK_DONOTWAIT)) || !bits) return nullptr;
	return (const uint8_t*)bits;
}

struct FaceRig {
	Host* h = nullptr;
	ID3D9NvrExtRtSkin* skin = nullptr;
	uint32_t id = 0;
	IDirect3DVertexBuffer9* part = nullptr;	// the game's partition buffer (partition order)
	IDirect3DVertexBuffer9* copy = nullptr;	// ours (FeedCpu)
	IDirect3DIndexBuffer9* ib = nullptr;
	std::vector<uint16_t> map;				// partition vertex -> data vertex (reversed)
	std::vector<V3> cpu;					// the geometry data's CPU vertices (data order)
	RtFaceMorph::State st;
	bool sent = false, faceMorphs = true, pending = false;
	uint32_t opsThisFrame = 0;
	uint32_t mapAgree = 0, mapTested = 0;	// the copy's map check (RtCore::FaceCopy)
	bool init(Host& host, ID3D9NvrExtRtSkin* s, bool on) {
		h = &host; skin = s; faceMorphs = on; id = host.nextGeometry++;
		const UINT V = kFaceGrid * kFaceGrid;
		CHECKHR(host.dev->CreateVertexBuffer(V * kFaceStride, 0, 0, D3DPOOL_MANAGED, &part, nullptr), "face vb");
		const Mesh m = faceMeshOf(0.0f);
		CHECKHR(host.dev->CreateIndexBuffer(UINT(m.idx.size() * 2), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, nullptr), "face ib");
		{ void* p; ib->Lock(0, 0, &p, 0); std::memcpy(p, m.idx.data(), m.idx.size() * 2); ib->Unlock(); }
		map.resize(V);
		for (UINT i = 0; i < V; i++) map[i] = uint16_t(V - 1 - i);
		cpu.assign(V, V3());
		return writePart(0.0f) && setCpu(0.0f);
	}
	bool writePart(float open) {	// the partition's buffer: position, weights (1, 0, 0), indices (0, 0, 0, 0)
		uint8_t* v = nullptr;
		CHECKHR(part->Lock(0, 0, (void**)&v, 0), "face vb lock");
		for (UINT j = 0; j < kFaceGrid; j++) for (UINT i = 0; i < kFaceGrid; i++) {
			uint8_t* o = v + (j * kFaceGrid + i) * kFaceStride;
			const V3 p = facePoint(i, j, open);
			const float w[3] = { 1.0f, 0.0f, 0.0f };
			std::memcpy(o, &p.x, 4); std::memcpy(o + 4, &p.y, 4); std::memcpy(o + 8, &p.z, 4);
			std::memcpy(o + 12, w, 12);
			std::memset(o + 24, 0, 4);
		}
		part->Unlock();
		return true;
	}
	bool setCpu(float open) {	// data vertex map[k] = partition vertex k
		for (UINT j = 0; j < kFaceGrid; j++) for (UINT i = 0; i < kFaceGrid; i++) cpu[map[j * kFaceGrid + i]] = facePoint(i, j, open);
		return true;
	}
	// RtCore::AddSkinned for this partition: FaceObserve, ActorStillSkip (the bone never moves), the forced pose, the stage
	bool frame(RtFaceMorph::Decision& d) {
		const UINT V = kFaceGrid * kFaceGrid, bytes = V * kFaceStride;
		RtFaceMorph::Sample in;
		if (const uint8_t* bits = faceLockRead(part, 0, bytes)) { in.part = true; in.partHash = RtFaceMorph::Hash(bits, V, kFaceStride, 0); part->Unlock(); }
		static_assert(sizeof(V3) == 12, "x y z floats");
		in.cpu = true; in.cpuHash = RtFaceMorph::Hash((const uint8_t*)cpu.data(), V, 12, 0);
		d = RtFaceMorph::Observe(st, in);
		const bool force = faceMorphs && (d.force || pending);
		bool pose = !sent || force;	// ActorStillSkip: nothing else poses it
		IDirect3DVertexBuffer9* from = part;
		if (faceMorphs && st.feed == RtFaceMorph::FeedCpu) {	// RtCore::FaceCopy
			const bool fresh = !copy;
			if (fresh) CHECKHR(h->dev->CreateVertexBuffer(bytes, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &copy, nullptr), "face copy");
			if (fresh || force) {
				uint8_t* dst = nullptr;
				CHECKHR(copy->Lock(0, bytes, (void**)&dst, 0), "copy lock");
				bool ok = true;
				if (fresh) {	// as RtCore: the partition's bytes, then the map checked against the feed
					const uint8_t* pb = faceLockRead(part, 0, bytes);
					ok = pb != nullptr;
					if (pb) {
						std::memcpy(dst, pb, bytes);
						uint32_t tested = 0;
						mapAgree = RtFaceMorph::MapAgreement(pb, V, kFaceStride, 0, (const uint8_t*)cpu.data(), V, 12, 0, map.data(), tested);
						mapTested = tested;
						ok = mapAgree * 2 >= tested;
						part->Unlock();
					}
				}
				ok = ok && RtFaceMorph::PatchPositions(dst, V, kFaceStride, 0, (const uint8_t*)cpu.data(), V, 12, 0, map.data());
				copy->Unlock();
				if (!ok) { say("  error: face copy\n"); return false; }
			}
			from = copy;
		}
		opsThisFrame = 0;
		if (pending && !pose) pending = false;
		if (!pose) return true;
		NVR_EXT_RT_RANGE r = { NVR_EXT_RT_TRIANGLE_LIST, 0, UINT((kFaceGrid - 1) * (kFaceGrid - 1) * 2), 0 };
		NVR_EXT_RT_GEOMETRY_DESC desc = { from, 0, kFaceStride, V, NVR_EXT_RT_POSITION_FLOAT3, ib, 1, &r, 0u, "host face" };
		static const float palette[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
		NVR_EXT_RT_SKIN_DESC s = {};
		s.WeightsDelta = 12; s.WeightsFormat = NVR_EXT_RT_SKIN_WEIGHTS_FLOAT; s.WeightCount = 1; s.BonesPerVertex = 1;
		s.IndicesDelta = 24; s.IndicesFormat = NVR_EXT_RT_SKIN_INDICES_UBYTE4; s.PaletteCount = 1; s.pPalette = palette;
		NVR_EXT_RT_GEOMETRY_INFO gi = {};
		CHECKHR(h->rt->MeasureGeometry(&desc, &gi), "MeasureGeometry (face)");
		if (h->arenaTexel + gi.Texels + 1 > h->arenaW * h->arenaH) { say("  error: arena full\n"); return false; }
		CHECKHR(skin->StageSkinnedGeometry(&desc, &s, h->images[0].shared, h->arenaTexel, &gi), "StageSkinnedGeometry");
		NvrRtGeometryOp op = {};
		op.op = NVR_RT_OP_BUILD; op.id = id; op.vertexCount = gi.VertexCount; op.triangleCount = gi.TriangleCount; op.arenaTexel = h->arenaTexel;
		op.flags = NVR_RT_OP_FLAG_DYNAMIC;
		h->ops.push_back(op);
		h->arenaTexel += gi.Texels;
		sent = true; pending = false; opsThisFrame = 1;
		return true;
	}
	void releaseBuffers() {
		if (part) part->Release(); if (copy) copy->Release(); if (ib) ib->Release();
		part = copy = nullptr; ib = nullptr;
	}
};

static bool sceneFaceMorph(Ctx& c) {
	Host& h = *c.host;
	bool pass = faceChecksNoGpu();
	say("  A (no GPU): %s\n", pass ? "all checks right" : "FAILED");
	ID3D9NvrExtRtSkin* skin = nullptr;
	if (FAILED(h.ext->QueryInterface(__uuidof(ID3D9NvrExtRtSkin), (void**)&skin)) || !skin) { say("  error: the fork has no ID3D9NvrExtRtSkin (patch 0033)\n"); return false; }
	if (gShPrimary < 0) { say("  error: RtLampPrimary.spv not registered\n"); skin->Release(); return false; }
	const Camera cam = Camera::lookDir({ 0, -200, 160 }, { 0, 1, 0 }, { 0, 0, 1 }, 1.2f, 1.2f);
	// trace the rig's current shape with the camera rays; compare with the CPU's exact rays on `expect` (stable pixels); `differ`: pixels
	// whose depth is not `expect`'s
	bool printWrong = true;	// off for the control (its wrong pixels are the point)
	auto traceFrame = [&](FaceRig& rig, const Mesh& expect, int& wrong, int& checked, int& differ) -> bool {
		World w; w.meshes = { expect };
		Object o; o.mesh = 0; o.hookMask = 0x20; w.objects = { o };
		PrimaryConstantsHost pc = {};
		cameraConstants(cam, pc.size, pc.forward, pc.right, pc.up, pc.origin);
		put4(pc.jitter, 0.0f, 0.0f, 0.0f, 0.0f);
		put4(pc.masks, float(0xFFu & ~0x60u), float(0x60u), 0.0f, 0.0f);
		h.AddPass(gShPrimary, { c.depth, c.normals, c.depth, c.depth, c.depth, c.depth, c.depth, c.depth, c.alphaStandIn }, 0x3, &pc, sizeof(pc), (S + 7) / 8, (S + 7) / 8);
		if (!h.Exchange(w, { rig.id }, cam.eye)) return false;
		std::vector<float> got;
		if (!h.Read(c.depth, got)) return false;
		h.EndFrame();
		wrong = checked = differ = 0;
		for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
			const V3 d = cam.ray((i + 0.5f) / S, (j + 0.5f) / S);
			const Hit hh = trace(w, cam.eye, d, 0.5f / length(d), 1e9f, 0xFF, true);
			const float want = hh.hit ? hh.t / cam.farZ : 1.0f;
			const float g = got[j * S + i];
			const bool same = std::fabs(g - want) * cam.farZ <= 1e-3f * (hh.hit ? hh.t : 0.0f) + 0.05f;
			if (!same) differ++;
			bool stable = true;
			for (int s = 0; s < 4 && stable; s++) {
				const V3 dd = cam.ray((i + 0.5f + (s == 0 ? -0.6f : s == 1 ? 0.6f : 0)) / S, (j + 0.5f + (s == 2 ? -0.6f : s == 3 ? 0.6f : 0)) / S);
				const Hit h2 = trace(w, cam.eye, dd, 0.5f / length(dd), 1e9f, 0xFF, true);
				stable = h2.hit == hh.hit && (!hh.hit || std::fabs(h2.t - hh.t) < 2.0f);
			}
			if (!stable) continue;
			checked++;
			if (!same) { if (wrong++ < 3 && printWrong) say("    pixel (%u, %u): depth %.3f, CPU %.3f\n", i, j, g * cam.farZ, want * cam.farZ); }
		}
		return true;
	};

	int depthWrong = 0, opsOnStill = 0;
	// ---- the rig with FaceMorphs on (ActorStillSkip on: only a hash change poses it)
	FaceRig on;
	if (!on.init(h, skin, true)) { skin->Release(); return false; }
	struct Step { float part, cpu; const char* what; };
	const Step steps[] = {
		{ 0, 0, "shut (first pose)" }, { 0, 0, "unchanged" }, { 40, 40, "mouth opens 40 (partition + CPU)" }, { 40, 40, "unchanged" },
		// the partition's buffer stops following: only the CPU vertices change
		{ 40, 20, "CPU only 20" }, { 40, 55, "CPU only 55" }, { 40, 10, "CPU only 10" }, { 40, 60, "CPU only 60" }, { 40, 30, "CPU only 30" },
		{ 40, 50, "CPU only 50" }, { 40, 15, "CPU only 15" }, { 40, 45, "CPU only 45" }, { 40, 45, "unchanged" },
	};
	int switchedAt = -1, frames = 0;
	bool ok = true;
	for (const Step& s : steps) {
		if (!on.writePart(s.part) || !on.setCpu(s.cpu)) { ok = false; break; }
		RtFaceMorph::Decision d;
		if (!on.frame(d)) { ok = false; break; }
		if (d.switched && switchedAt < 0) switchedAt = frames;
		// what the traced head must be: the partition's shape while it is the feed, the CPU's after the switch; before the switch the
		// CPU-only frames keep the last posed shape (the partition's 40): stale by design, checked as such
		const float shape = on.st.feed == RtFaceMorph::FeedCpu ? s.cpu : s.part;
		int wrong = 0, checked = 0, differ = 0;
		if (!traceFrame(on, faceMeshOf(shape), wrong, checked, differ)) { ok = false; break; }
		const bool still = std::string(s.what) == "unchanged";
		if (still && on.opsThisFrame) opsOnStill++;
		depthWrong += wrong;
		say("  frame %d (%s): feed %s, %s, %d of %d stable pixels wrong (traced as open %.0f)\n", frames, s.what,
			on.st.feed == RtFaceMorph::FeedCpu ? "CPU copy" : "partition", on.opsThisFrame ? "posed (BUILD op)" : "no op", wrong, checked, shape);
		frames++;
	}
	releaseWorld(h, { on.id });
	// ---- the control: FaceMorphs off, the same mouth change in the partition's buffer -> never posed again
	FaceRig off;
	int controlStale = 0, controlOps = 0;
	if (ok && off.init(h, skin, false)) {
		RtFaceMorph::Decision d;
		int wrong = 0, checked = 0, differ = 0;
		ok = off.frame(d) && traceFrame(off, faceMeshOf(0.0f), wrong, checked, differ);
		depthWrong += wrong;
		off.writePart(40.0f); off.setCpu(40.0f);
		ok = ok && off.frame(d);
		controlOps = int(off.opsThisFrame);
		printWrong = false;
		ok = ok && traceFrame(off, faceMeshOf(40.0f), wrong, checked, controlStale);
		say("  control (FaceMorphs off, ActorStillSkip on): the mouth opens 40 -> %s, %d pixels still show the shut mouth\n", controlOps ? "posed" : "no op", controlStale);
		releaseWorld(h, { off.id });
	}
	else ok = false;
	// the releases go with the next exchange (the next scene's or the host's last); our buffers are past the GPU (Read waited)
	on.releaseBuffers(); off.releaseBuffers();
	skin->Release();
	pass = pass && ok && depthWrong == 0 && opsOnStill == 0 && switchedAt >= 0 && switchedAt <= 8 && controlOps == 0 && controlStale > 50;
	say("  B (GPU): %d wrong depths, %d ops on unchanged frames, switch to the CPU copy at frame %d (CPU-only changes from frame 4; the copy's map "
		"check %u of %u), control: %d stale pixels (must be > 50)\n", depthWrong, opsOnStill, switchedAt, on.mapAgree, on.mapTested, controlStale);
	return pass;
}
