// NvrRtDisplace.inl -- UNOFFICIAL 2026-11-07 (protocol 8, rtgap-tessblas) displaced geometry in the RT scene. MIT licence (LICENSE here).
// Included by NvrRtHelper.cpp inside its namespace. docs/next/RT-DETAIL-GAPS.md section 1c has the design and its status.
//
// With the game's tessellation on (fork 1.7-displace: [Shaders.Terrain.Displacement] for the land, [Shaders.POM.Displacement] for the
// parallax objects), the depth buffer, the normals and the sun's shadow maps have bumps of up to Strength game units the RT scene did not
// have: its geometry is built once from the game's flat vertex buffers. Here, for every instance NVR flags (NVR_RT_INSTANCE_DISPLACE: the
// land and parallax objects within FadeEnd), the helper keeps a COPY of the instance's geometry, every triangle split N x N and every new
// point moved along the normal by the same height maps (the game's own textures, set 1) and the same formula as the game's tessellation
// evaluation shaders, with that instance's distance fade (RtDisplace.comp). The copy has its own bottom-level structure (ALLOW_UPDATE),
// redone and refit in place when the camera has moved far enough for the fade to change (the refresh distance), and the instance is
// traced through it: the TLAS entry points at the copy, its record at the copy's positions, triangles and texcoords (binding 11), so
// every trace pass sees and shades the bumps with no change of its own.
//
//   per source geometry (NVR_RT_OP_DISPLACE_SOURCE): its kind, layer textures, layer weights / normals (NVR_RT_OP_DATA ids); measured
//     once (MEASURE: its longest edge, read back when that frame is done), which with the spacing gives N
//   per instance (key = geometry id + NvrRtInstanceKey): the copy: NORMALS (when the game's are not sent) + INDICES once, DISPLACE at its
//     build and at each refresh; its structure built once, refit after
// Budgets ([Main.Main.RayTracing] DisplaceTrianglesPerFrame / DisplaceMaxTriangles via the CAMERA block): new copies first, then the
// stalest refreshes, as many copy triangles a frame as the budget allows; a copy over the per-copy cap gets a smaller N. An instance
// without a ready copy is traced flat, as before. A copy unused for 120 frames is freed (its structure and words once no frame in flight
// uses them).

namespace Displace {

	const uint32_t kMaxJobs = 1024;			// jobs a frame (each copy built or refreshed takes 1-3, each measurement 1)
	const uint32_t kExpireFrames = 120;

	struct Job {							// RtDisplace.comp's Job (std430: 13 x 16 bytes)
		uint32_t a[4], b[4], c[4], d[4], tex[4];
		float row0[4], row1[4], row2[4], p0[4], p1[4], p2[4], p3[4];
		uint32_t e[4];
	};
	static_assert(sizeof(Job) == 208, "RtDisplace.comp's Job");

	struct Data { uint32_t offset = NVR_RT_NO_DATA, words = 0, vertices = 0; bool uv = false; };
	struct Source {
		uint32_t kind = 0, layers = 1, heightBits = 1, tex[7] = {};
		uint32_t weightsA = NVR_RT_NO_DATA, weightsB = NVR_RT_NO_DATA, normals = NVR_RT_NO_DATA;	// word offsets (DATA), or none
		int measure = 0;					// 0 not yet, 1 in flight, 2 done
		uint64_t measureFrame = 0;
		uint32_t measureSlot = 0, measureJob = 0;
		float maxEdge = 0.0f, maxUvEdge = 0.0f;
	};
	enum CopyState { kWaiting, kBuilt, kSkip };
	struct Copy {
		uint32_t source = 0;
		CopyState state = kWaiting;
		uint32_t n = 0;
		Geometry geo;						// the copy: its structure, words (positions, triangles, texcoords, accumulator), counts
		uint32_t accOffset = NVR_RT_NO_DATA;
		float transform[3][4] = {};			// this frame's instance transform
		float lastT[3] = {}, lastFwd[3] = {};	// at its last displacement
		uint32_t version = 0;				// the parameters' version at its last displacement
		uint64_t lastSeen = 0;
		bool seen = false;					// flagged this frame
	};

	struct Module {
		bool tried = false, ok = false;
		VkPipelineLayout layout = VK_NULL_HANDLE;
		VkPipeline pipeline = VK_NULL_HANDLE;
		Buffer jobs[kCmdRing], results[kCmdRing];
		Buffer scratch;
		float params[NVR_RT_DISPLACE_BLOCKS][6] = {};
		bool have[NVR_RT_DISPLACE_BLOCKS] = {};
		uint32_t version = 1;
		std::unordered_map<uint32_t, Source> sources;
		std::unordered_map<uint32_t, Data> data;
		std::unordered_map<uint64_t, Copy> copies;
		uint64_t built = 0, refreshed = 0, skipped = 0, expired = 0, measured = 0, budgetWaits = 0, liveTriangles = 0;
		bool loggedFirst = false;
	};
	static Module g_disp;

	bool KindOn(uint32_t kind) {
		const uint32_t reach = kind == NVR_RT_DISPLACE_KIND_LAND ? NVR_RT_DISPLACE_LAND_REACH : NVR_RT_DISPLACE_OBJ_REACH;
		const uint32_t shape = kind == NVR_RT_DISPLACE_KIND_LAND ? NVR_RT_DISPLACE_LAND_SHAPE : NVR_RT_DISPLACE_OBJ_SHAPE;
		return g_disp.have[reach] && g_disp.have[shape] && g_disp.params[reach][5] != 0.0f;
	}

	bool Create(Helper& h) {
		Module& m = g_disp;
		if (m.tried) return m.ok;
		m.tried = true;
		if (!h.vk.bindless) { Log("displacement: OFF (the GPU lacks descriptor indexing: no real textures to read the heights from)"); return false; }
		VkPushConstantRange range = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 32 };
		VkPipelineLayoutCreateInfo li = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
		li.setLayoutCount = 1;
		li.pSetLayouts = &h.tex.layout;			// set 0 here = the real textures' set (binding 1 sampler, 2 the textures)
		li.pushConstantRangeCount = 1;
		li.pPushConstantRanges = &range;
		VkShaderModuleCreateInfo mi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
		mi.codeSize = sizeof(kRtDisplaceSpv);
		mi.pCode = kRtDisplaceSpv;
		VkShaderModule module = VK_NULL_HANDLE;
		if (vkCreatePipelineLayout(h.vk.device, &li, nullptr, &m.layout) != VK_SUCCESS || vkCreateShaderModule(h.vk.device, &mi, nullptr, &module) != VK_SUCCESS) {
			Log("displacement: OFF (pipeline layout / shader module failed)");
			return false;
		}
		VkComputePipelineCreateInfo pi = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
		pi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
		pi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
		pi.stage.module = module;
		pi.stage.pName = "main";
		pi.layout = m.layout;
		const VkResult r = vkCreateComputePipelines(h.vk.device, g_pipelineCache, 1, &pi, nullptr, &m.pipeline);
		vkDestroyShaderModule(h.vk.device, module, nullptr);
		if (r != VK_SUCCESS) { Log("displacement: OFF (pipeline: %s)", VkResultName(r)); m.pipeline = VK_NULL_HANDLE; return false; }
		for (uint32_t i = 0; i < kCmdRing; i++) {
			if (!CreateBuffer(h.vk, sizeof(Job) * kMaxJobs, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, m.jobs[i]) ||
				!CreateBuffer(h.vk, 8 * kMaxJobs, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, m.results[i])) {
				Log("displacement: OFF (job buffers)");
				return false;
			}
			memset(m.results[i].mapped, 0, 8 * kMaxJobs);
		}
		m.ok = true;
		Log("displacement: ready (displaced copies of the flagged land / parallax objects, RtDisplace.comp)");
		return true;
	}

	bool EnsureDispScratch(Helper& h, VkDeviceSize size) {
		Module& m = g_disp;
		size += h.vk.asProperties.minAccelerationStructureScratchOffsetAlignment;
		if (m.scratch.size >= size) return true;
		vkQueueWaitIdle(h.vk.queue);	// rare: a frame in flight may still use the old one (nothing of this frame has yet)
		DestroyBuffer(h.vk, m.scratch);
		return CreateBuffer(h.vk, std::max<VkDeviceSize>(size, 8ull << 20), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, m.scratch);
	}

	// Step 1 of RecordFrame: a DATA op's words out of the staging copy of the arena (positions + texcoords; the triangle list skipped)
	void StageData(Helper& h, const NvrRtGeometryOp& op, uint64_t capacity, uint32_t arenaTexels, std::vector<VkBufferCopy>& copies) {
		const bool uv = (op.flags & NVR_RT_OP_FLAG_TEXCOORDS) != 0;
		const uint64_t texels = 3ull * ((uint64_t)op.vertexCount + op.triangleCount) + (uv ? 2ull * op.vertexCount : 0ull);
		if (!op.id || !op.vertexCount || (uint64_t)op.arenaTexel + texels > std::min<uint64_t>(capacity, arenaTexels)) {
			g_shared->failedOps++;
			Log("displacement: data %u refused (%u vertices at texel %u, arena %u texels)", op.id, op.vertexCount, op.arenaTexel, arenaTexels);
			return;
		}
		Module& m = g_disp;
		auto old = m.data.find(op.id);
		if (old != m.data.end()) { h.dataWords.Free(old->second.offset, old->second.words); m.data.erase(old); }
		Data d;
		d.words = 3u * op.vertexCount + (uv ? 2u * op.vertexCount : 0u);
		d.offset = h.dataWords.Allocate(d.words);
		d.vertices = op.vertexCount;
		d.uv = uv;
		if (d.offset == NVR_RT_NO_DATA) { Log("displacement: no room for data %u (%u words)", op.id, d.words); return; }
		copies.push_back({ (VkDeviceSize)op.arenaTexel * 4, (VkDeviceSize)d.offset * 4, (VkDeviceSize)op.vertexCount * 12 });
		if (uv) copies.push_back({ ((VkDeviceSize)op.arenaTexel + 3ull * ((uint64_t)op.vertexCount + op.triangleCount)) * 4,
			((VkDeviceSize)d.offset + 3ull * op.vertexCount) * 4, (VkDeviceSize)op.vertexCount * 8 });
		m.data[op.id] = d;
	}

	void FreeCopy(Helper& h, Copy& c, uint64_t frame) {
		if (c.geo.as || c.geo.vertexOffset != NVR_RT_NO_DATA) {
			if (c.state == kBuilt) g_disp.liveTriangles -= c.geo.triangles;
			h.pendingFrees.push_back({ frame, c.geo });	// (ReleaseFinished: the structure and every word of the copy)
		}
		c.geo = Geometry();
		c.accOffset = NVR_RT_NO_DATA;
		c.state = kWaiting;
		c.n = 0;
	}

	// The source geometry `id` is gone or rebuilt (ReleaseGeometry): its copies go (a rebuild gets new ones from the new data)
	void DropCopiesOf(Helper& h, uint32_t id, uint64_t frame) {
		Module& m = g_disp;
		if (!m.ok) return;
		for (auto it = m.copies.begin(); it != m.copies.end();) {
			if (it->second.source == id) { FreeCopy(h, it->second, frame); it = m.copies.erase(it); }
			else ++it;
		}
		auto s = m.sources.find(id);
		if (s != m.sources.end()) s->second.measure = 0;	// (a rebuild under the same id: measured again from the new data)
	}

	// A RELEASE op: a DATA id's words, or a source's description
	void Release(Helper& h, uint32_t id, uint64_t frame) {
		Module& m = g_disp;
		auto d = m.data.find(id);
		if (d != m.data.end()) {
			Geometry words;
			words.vertexOffset = d->second.offset;
			words.dataWords = d->second.words;
			h.pendingFrees.push_back({ frame, words });	// (no structure: ReleaseFinished frees the words only)
			m.data.erase(d);
		}
		m.sources.erase(id);
	}

	uint32_t DataOffset(uint32_t id) {
		if (!id) return NVR_RT_NO_DATA;
		auto d = g_disp.data.find(id);
		return d == g_disp.data.end() ? NVR_RT_NO_DATA : d->second.offset;
	}

	// The copy to trace instance `in` through (null: the flat geometry)
	const Geometry* CopyFor(const NvrRtInstance& in) {
		Module& m = g_disp;
		if (!m.ok || !(in.flags & NVR_RT_INSTANCE_DISPLACE)) return nullptr;
		auto it = m.copies.find(((uint64_t)in.geometryId << 32) | NvrRtInstanceKey(in.flags));
		return it != m.copies.end() && it->second.state == kBuilt && it->second.seen ? &it->second.geo : nullptr;
	}

	void Invert3(const float t[3][4], float inv[3][3]) {
		const float a = t[0][0], b = t[0][1], c = t[0][2], d = t[1][0], e = t[1][1], f = t[1][2], g = t[2][0], hh = t[2][1], i = t[2][2];
		const float A = e * i - f * hh, B = -(d * i - f * g), C = d * hh - e * g;
		float det = a * A + b * B + c * C;
		if (std::fabs(det) < 1e-20f) det = 1e-20f;
		const float k = 1.0f / det;
		inv[0][0] = A * k; inv[0][1] = -(b * i - c * hh) * k; inv[0][2] = (b * f - c * e) * k;
		inv[1][0] = B * k; inv[1][1] = (a * i - c * g) * k; inv[1][2] = -(a * f - c * d) * k;
		inv[2][0] = C * k; inv[2][1] = -(a * hh - b * g) * k; inv[2][2] = (a * e - b * d) * k;
	}

	// The job of a copy (mode, the source's words, the copy's, the parameters, this frame's transform)
	Job MakeJob(Helper& h, const Copy& c, const Source& s, const Geometry& src, uint32_t mode) {
		Module& m = g_disp;
		Job j = {};
		const bool land = s.kind == NVR_RT_DISPLACE_KIND_LAND;
		j.a[0] = mode; j.a[1] = src.triangles; j.a[2] = c.n; j.a[3] = land ? 0u : 1u;
		j.b[0] = src.vertexOffset; j.b[1] = src.indexOffset; j.b[2] = src.uvOffset; j.b[3] = c.accOffset;
		j.c[0] = c.geo.vertexOffset; j.c[1] = c.geo.indexOffset; j.c[2] = c.geo.uvOffset; j.c[3] = s.normals;
		j.d[0] = land ? s.weightsA : NVR_RT_NO_DATA; j.d[1] = land ? s.weightsB : NVR_RT_NO_DATA; j.d[2] = land ? s.layers : 1u; j.d[3] = s.heightBits;
		j.tex[0] = s.tex[0] | (s.tex[1] << 16); j.tex[1] = s.tex[2] | (s.tex[3] << 16); j.tex[2] = s.tex[4] | (s.tex[5] << 16); j.tex[3] = s.tex[6];
		for (uint32_t k = 0; k < 7; k++) {	// a texture the helper does not have reads as "no height"
			const uint32_t id = s.tex[k];
			if (id && (id >= NVR_RT_MAX_TEXTURES || !h.tex.textures[id].view)) j.d[3] &= ~(1u << k);
		}
		memcpy(j.row0, c.transform[0], 16); memcpy(j.row1, c.transform[1], 16); memcpy(j.row2, c.transform[2], 16);
		const float* shape = m.params[land ? NVR_RT_DISPLACE_LAND_SHAPE : NVR_RT_DISPLACE_OBJ_SHAPE];
		const float* reach = m.params[land ? NVR_RT_DISPLACE_LAND_REACH : NVR_RT_DISPLACE_OBJ_REACH];
		const float* cam = m.params[NVR_RT_DISPLACE_CAMERA];
		j.p0[0] = shape[0]; j.p0[1] = shape[1]; j.p0[2] = shape[2]; j.p0[3] = shape[4];
		j.p1[0] = reach[0]; j.p1[1] = reach[1]; j.p1[2] = shape[3]; j.p1[3] = land ? shape[5] : 0.0f;
		const float sx = std::sqrt(c.transform[0][0] * c.transform[0][0] + c.transform[1][0] * c.transform[1][0] + c.transform[2][0] * c.transform[2][0]);
		j.p2[0] = cam[0]; j.p2[1] = cam[1]; j.p2[2] = cam[2]; j.p2[3] = sx;
		float inv[3][3];
		Invert3(c.transform, inv);
		for (int r = 0; r < 3; r++) j.p3[r] = -(inv[r][0] * c.transform[0][3] + inv[r][1] * c.transform[1][3] + inv[r][2] * c.transform[2][3]);
		j.p3[3] = c.n ? s.maxEdge / (float)c.n : s.maxEdge;
		j.e[0] = src.vertices;
		return j;
	}

	// RecordFrame, after the builds and releases, before the scene: this frame's displacement work
	void Run(Helper& h, uint32_t slot, VkCommandBuffer cmd, uint64_t frameNumber) {
		Module& m = g_disp;
		const NvrRtFrame& f = h.frame;
		// 1. this frame's parameters and sources
		bool any = false;
		for (uint32_t i = 0; i < f.opCount; i++) {
			const NvrRtGeometryOp& op = f.ops[i];
			if (op.op == NVR_RT_OP_DISPLACE_PARAMS) {
				const NvrRtDisplaceParamsOp& p = (const NvrRtDisplaceParamsOp&)op;
				if (p.id >= NVR_RT_DISPLACE_BLOCKS) continue;
				bool same = m.have[p.id];
				for (int k = 0; k < 6 && same; k++) same = m.params[p.id][k] == p.v[k];
				// the camera block changes every frame: the copies follow it through their own refresh test, not the version
				if (!same && p.id != NVR_RT_DISPLACE_CAMERA) m.version++;
				memcpy(m.params[p.id], p.v, sizeof(p.v));
				m.have[p.id] = true;
				any = true;
			}
			else if (op.op == NVR_RT_OP_DISPLACE_SOURCE) any = true;
		}
		if (!m.tried && !any) return;	// nothing displaced yet this session: not even the pipeline
		if (!Create(h)) return;
		for (uint32_t i = 0; i < f.opCount; i++) {
			const NvrRtGeometryOp& op = f.ops[i];
			if (op.op != NVR_RT_OP_DISPLACE_SOURCE) continue;
			const NvrRtDisplaceSourceOp& p = (const NvrRtDisplaceSourceOp&)op;
			Source s;
			auto old = m.sources.find(p.id);
			if (old != m.sources.end()) { s.measure = old->second.measure; s.maxEdge = old->second.maxEdge; s.maxUvEdge = old->second.maxUvEdge;
				s.measureFrame = old->second.measureFrame; s.measureSlot = old->second.measureSlot; s.measureJob = old->second.measureJob; }
			for (uint32_t k = 0; k < 7; k++) s.tex[k] = NvrRtDisplaceField(p.packed, 12 * k, 12);
			s.layers = std::max(1u, NvrRtDisplaceField(p.packed, 84, 3));
			s.heightBits = NvrRtDisplaceField(p.packed, 87, 7);
			s.kind = NvrRtDisplaceField(p.packed, 94, 1);
			s.weightsA = DataOffset(p.weightsA);
			s.weightsB = DataOffset(p.weightsB);
			s.normals = DataOffset(p.normals);
			if (s.weightsA != NVR_RT_NO_DATA) {	// layer 3 is in the texcoords half: needed
				auto d = m.data.find(p.weightsA);
				if (!d->second.uv) s.weightsA = NVR_RT_NO_DATA;
			}
			m.sources[p.id] = s;
			for (auto& c : m.copies) if (c.second.source == p.id && c.second.state != kWaiting) c.second.version = 0;	// (redone)
		}

		// 2. the measurements that came back (their frame is done)
		uint64_t done = 0;
		vkGetSemaphoreCounterValue(h.vk.device, h.outputReady, &done);
		for (auto& e : m.sources) {
			Source& s = e.second;
			if (s.measure != 1 || s.measureFrame > done) continue;
			const uint32_t* r = (const uint32_t*)m.results[s.measureSlot].mapped + 2 * s.measureJob;
			memcpy(&s.maxEdge, &r[0], 4);
			memcpy(&s.maxUvEdge, &r[1], 4);
			s.measure = 2;
			m.measured++;
		}

		// 3. the flagged instances: their copies (this frame's transform)
		for (auto& c : m.copies) c.second.seen = false;
		for (uint32_t i = 0; i < f.instanceCount; i++) {
			const NvrRtInstance& in = f.instances[i];
			if (!(in.flags & NVR_RT_INSTANCE_DISPLACE)) continue;
			auto s = m.sources.find(in.geometryId);
			if (s == m.sources.end() || !KindOn(s->second.kind)) continue;
			auto src = h.geometries.find(in.geometryId);
			if (src == h.geometries.end() || src->second.vertexOffset == NVR_RT_NO_DATA || src->second.uvOffset == NVR_RT_NO_DATA) continue;
			Copy& c = m.copies[((uint64_t)in.geometryId << 32) | NvrRtInstanceKey(in.flags)];
			c.source = in.geometryId;
			memcpy(c.transform, in.transform, sizeof(c.transform));
			c.lastSeen = frameNumber;
			c.seen = true;
		}

		// 4. what to do this frame
		Job* jobs = (Job*)m.jobs[slot].mapped;
		uint32_t jobCount = 0;
		struct Dispatch { uint32_t job, threads; };
		std::vector<Dispatch> first, second;	// before / after the barrier (normals, indices, measurements | displacement)
		std::vector<VkAccelerationStructureGeometryKHR> geoms;
		std::vector<VkAccelerationStructureBuildGeometryInfoKHR> infos;
		std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
		std::vector<VkDeviceSize> scratchAt;
		VkDeviceSize scratchTotal = 0;
		const VkDeviceSize align = h.vk.asProperties.minAccelerationStructureScratchOffsetAlignment;
		std::vector<std::pair<uint32_t, uint32_t>> fills;	// accumulators to zero (word offset, words)
		bool measuring = false;
		const float* cam = m.params[NVR_RT_DISPLACE_CAMERA];
		const uint64_t budget = cam[3] > 0.0f ? (uint64_t)cam[3] : 2000000ull;
		const uint64_t perCopy = cam[4] > 0.0f ? (uint64_t)cam[4] : 262144ull;
		uint64_t spent = 0;

		auto addBuild = [&](Copy& c, bool update) {
			VkAccelerationStructureGeometryKHR gg = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
			gg.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
			gg.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
			gg.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
			gg.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
			gg.geometry.triangles.vertexData.deviceAddress = h.geometryData.address + (VkDeviceSize)c.geo.vertexOffset * 4;
			gg.geometry.triangles.vertexStride = 12;
			gg.geometry.triangles.maxVertex = c.geo.vertices - 1;
			gg.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
			gg.geometry.triangles.indexData.deviceAddress = h.geometryData.address + (VkDeviceSize)c.geo.indexOffset * 4;
			VkAccelerationStructureBuildGeometryInfoKHR b = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
			b.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
			b.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
			b.mode = update ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
			b.geometryCount = 1;
			VkAccelerationStructureBuildSizesInfoKHR sizes = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
			b.pGeometries = &gg;
			h.vk.buildSizes(h.vk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &b, &c.geo.triangles, &sizes);
			if (!update) {
				c.geo.size = sizes.accelerationStructureSize;
				if (!h.structures.Allocate(h.vk, c.geo.size, c.geo.block, c.geo.offset)) return false;
				VkAccelerationStructureCreateInfoKHR ci = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
				ci.buffer = h.structures.blocks[c.geo.block].buffer.buffer;
				ci.offset = c.geo.offset;
				ci.size = c.geo.size;
				ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
				if (h.vk.createAs(h.vk.device, &ci, nullptr, &c.geo.as) != VK_SUCCESS) {
					h.structures.Free(h.vk, c.geo.block, c.geo.offset, c.geo.size);
					c.geo.as = VK_NULL_HANDLE;
					return false;
				}
				VkAccelerationStructureDeviceAddressInfoKHR ai = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
				ai.accelerationStructure = c.geo.as;
				c.geo.address = h.vk.asAddress(h.vk.device, &ai);
			}
			else b.srcAccelerationStructure = c.geo.as;
			b.dstAccelerationStructure = c.geo.as;
			geoms.push_back(gg);
			infos.push_back(b);
			ranges.push_back({ c.geo.triangles, 0, 0, 0 });
			scratchAt.push_back(scratchTotal);
			scratchTotal += AlignUp(update ? sizes.updateScratchSize : sizes.buildScratchSize, align);
			return true;
		};
		auto displace = [&](Copy& c, const Source& s, const Geometry& src) {
			jobs[jobCount] = MakeJob(h, c, s, src, 3u);
			const uint32_t points = (c.n + 1) * (c.n + 2) / 2;
			second.push_back({ jobCount++, src.triangles * points });
			c.lastT[0] = c.transform[0][3]; c.lastT[1] = c.transform[1][3]; c.lastT[2] = c.transform[2][3];
			c.lastFwd[0] = cam[0]; c.lastFwd[1] = cam[1]; c.lastFwd[2] = cam[2];
			c.version = m.version;
		};

		// measurements wanted (a source with a flagged instance, not measured yet)
		for (auto& e : m.copies) {
			Copy& c = e.second;
			if (!c.seen || jobCount + 1 >= kMaxJobs) continue;
			Source& s = m.sources[c.source];
			if (s.measure != 0) continue;
			const Geometry& src = h.geometries[c.source];
			jobs[jobCount] = MakeJob(h, c, s, src, 0u);
			first.push_back({ jobCount, src.triangles });
			s.measure = 1; s.measureFrame = frameNumber; s.measureSlot = slot; s.measureJob = jobCount;
			jobCount++;
			measuring = true;
		}
		// new copies of measured sources (first come, within the budget)
		for (auto& e : m.copies) {
			Copy& c = e.second;
			if (!c.seen || c.state != kWaiting || jobCount + 3 >= kMaxJobs) continue;
			const Source& s = m.sources[c.source];
			if (s.measure != 2) continue;
			const Geometry& src = h.geometries[c.source];
			const bool land = s.kind == NVR_RT_DISPLACE_KIND_LAND;
			const float* reach = m.params[land ? NVR_RT_DISPLACE_LAND_REACH : NVR_RT_DISPLACE_OBJ_REACH];
			const float scale = std::sqrt(c.transform[0][0] * c.transform[0][0] + c.transform[1][0] * c.transform[1][0] + c.transform[2][0] * c.transform[2][0]);
			const float spacing = std::max(reach[2], 1.0f);
			uint32_t n = (uint32_t)std::ceil(s.maxEdge * scale / spacing);
			n = std::min<uint32_t>(std::max<uint32_t>(n, 1u), (uint32_t)std::max(1.0f, std::min(reach[3], 32.0f)));
			while (n > 1 && (uint64_t)src.triangles * n * n > perCopy) n--;
			// an object needs points inside its triangles (its edges are pinned): N 3 at least, else it stays flat
			if (!(s.maxEdge > 0.0f) || (!land && n < 3)) { c.state = kSkip; m.skipped++; continue; }
			const uint64_t tris = (uint64_t)src.triangles * n * n;
			if (spent && spent + tris > budget) { m.budgetWaits++; continue; }
			const uint32_t points = (n + 1) * (n + 2) / 2;
			c.n = n;
			c.geo = Geometry();
			c.geo.vertices = src.triangles * points;
			c.geo.triangles = (uint32_t)tris;
			c.geo.dynamic = true;
			const uint32_t needAcc = s.normals == NVR_RT_NO_DATA ? 3u * src.vertices : 0u;
			c.geo.dataWords = 3u * c.geo.vertices + 3u * c.geo.triangles + 2u * c.geo.vertices + needAcc;
			c.geo.vertexOffset = h.dataWords.Allocate(c.geo.dataWords);
			if (c.geo.vertexOffset == NVR_RT_NO_DATA) { c.geo = Geometry(); c.n = 0; m.budgetWaits++; continue; }
			c.geo.indexOffset = c.geo.vertexOffset + 3u * c.geo.vertices;
			c.geo.uvOffset = c.geo.indexOffset + 3u * c.geo.triangles;
			c.accOffset = needAcc ? c.geo.uvOffset + 2u * c.geo.vertices : NVR_RT_NO_DATA;
			if (!addBuild(c, false)) { h.dataWords.Free(c.geo.vertexOffset, c.geo.dataWords); c.geo = Geometry(); c.n = 0; m.budgetWaits++; continue; }
			if (needAcc) {
				fills.push_back({ c.accOffset, needAcc });
				jobs[jobCount] = MakeJob(h, c, s, src, 1u);
				first.push_back({ jobCount++, src.triangles });
			}
			jobs[jobCount] = MakeJob(h, c, s, src, 2u);
			first.push_back({ jobCount++, src.triangles });
			displace(c, s, src);
			c.state = kBuilt;
			m.liveTriangles += c.geo.triangles;
			spent += tris;
			m.built++;
		}
		// refreshes: the camera moved by the refresh distance (objects: or turned, their fade is by view depth), or the settings changed
		struct Stale { Copy* c; float score; };
		std::vector<Stale> stale;
		for (auto& e : m.copies) {
			Copy& c = e.second;
			if (!c.seen || c.state != kBuilt) continue;
			const Source& s = m.sources[c.source];
			const bool land = s.kind == NVR_RT_DISPLACE_KIND_LAND;
			const float refresh = std::max(m.params[land ? NVR_RT_DISPLACE_LAND_REACH : NVR_RT_DISPLACE_OBJ_REACH][4], 1.0f);
			const float dx = c.transform[0][3] - c.lastT[0], dy = c.transform[1][3] - c.lastT[1], dz = c.transform[2][3] - c.lastT[2];
			float moved = std::sqrt(dx * dx + dy * dy + dz * dz);
			if (!land) {
				const float fx = cam[0] - c.lastFwd[0], fy = cam[1] - c.lastFwd[1], fz = cam[2] - c.lastFwd[2];
				const float reachOf = std::sqrt(c.transform[0][3] * c.transform[0][3] + c.transform[1][3] * c.transform[1][3] + c.transform[2][3] * c.transform[2][3]);
				moved += std::sqrt(fx * fx + fy * fy + fz * fz) * reachOf;
			}
			const float score = c.version != m.version ? 1e30f : moved / refresh;
			if (score >= 1.0f) stale.push_back({ &c, score });
		}
		std::sort(stale.begin(), stale.end(), [](const Stale& a, const Stale& b) { return a.score > b.score; });
		for (const Stale& st : stale) {
			Copy& c = *st.c;
			if (jobCount + 1 >= kMaxJobs) break;
			if (spent && spent + c.geo.triangles > budget) { m.budgetWaits++; break; }
			const Source& s = m.sources[c.source];
			const Geometry& src = h.geometries[c.source];
			if (!addBuild(c, true)) continue;
			displace(c, s, src);
			spent += c.geo.triangles;
			m.refreshed++;
		}
		// copies not flagged for a while (out of reach, the object gone) go
		for (auto it = m.copies.begin(); it != m.copies.end();) {
			if (frameNumber - it->second.lastSeen > kExpireFrames || !m.sources.count(it->second.source)) {
				FreeCopy(h, it->second, frameNumber);
				it = m.copies.erase(it);
				m.expired++;
			}
			else ++it;
		}
		if (!jobCount) return;

		// 5. record: zero the measurements / accumulators, then the first dispatches, the displacement, the structures
		if (measuring) vkCmdFillBuffer(cmd, m.results[slot].buffer, 0, 8 * kMaxJobs, 0u);
		for (const auto& fl : fills) vkCmdFillBuffer(cmd, h.geometryData.buffer, (VkDeviceSize)fl.first * 4, (VkDeviceSize)fl.second * 4, 0u);
		FullBarrier(cmd);
		if (!infos.empty() && !EnsureDispScratch(h, scratchTotal)) { Log("displacement: no build scratch (%llu bytes)", (unsigned long long)scratchTotal); return; }
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m.pipeline);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m.layout, 0, 1, &h.tex.sets[slot], 0, nullptr);
		struct { uint64_t data, jobs, result; uint32_t job, threads; } push = {};
		push.data = h.geometryData.address;
		push.jobs = m.jobs[slot].address;
		push.result = m.results[slot].address;
		auto run = [&](const std::vector<Dispatch>& list) {
			for (const Dispatch& dsp : list) {
				if (!dsp.threads) continue;
				push.job = dsp.job;
				push.threads = dsp.threads;
				vkCmdPushConstants(cmd, m.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
				vkCmdDispatch(cmd, (dsp.threads + 63u) / 64u, 1u, 1u);
			}
		};
		run(first);
		FullBarrier(cmd);
		run(second);
		FullBarrier(cmd);
		if (!infos.empty()) {
			const uint64_t base = AlignUp(m.scratch.address, align);
			std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> rp(infos.size());
			for (size_t i = 0; i < infos.size(); i++) {
				infos[i].pGeometries = &geoms[i];
				infos[i].scratchData.deviceAddress = base + scratchAt[i];
				rp[i] = &ranges[i];
			}
			h.vk.cmdBuildAs(cmd, (uint32_t)infos.size(), infos.data(), rp.data());
			FullBarrier(cmd);
		}
		if (measuring) {	// the measurements for the CPU (read once this frame is done)
			VkMemoryBarrier hb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
			hb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
			hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, nullptr, 0, nullptr);
		}
		if (!m.loggedFirst && m.built) {
			m.loggedFirst = true;
			Log("displacement: first copies built (frame %llu, %llu copy triangles)", (unsigned long long)frameNumber, (unsigned long long)m.liveTriangles);
		}
		if ((frameNumber % 600) == 0)
			Log("displacement: %zu copies (%llu triangles), %zu sources; so far %llu built, %llu refreshed, %llu left flat (too coarse), %llu expired, "
				"%llu measured, %llu budget waits", m.copies.size(), (unsigned long long)m.liveTriangles, m.sources.size(), (unsigned long long)m.built,
				(unsigned long long)m.refreshed, (unsigned long long)m.skipped, (unsigned long long)m.expired, (unsigned long long)m.measured,
				(unsigned long long)m.budgetWaits);
	}
}
