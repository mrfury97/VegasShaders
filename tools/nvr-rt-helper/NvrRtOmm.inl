// NvrRtOmm.inl -- UNOFFICIAL 2026-11-07 (ptaaa-omm, protocol 9) opacity micromaps for alpha-tested geometry. MIT licence (LICENSE here).
// Included by NvrRtHelper.cpp inside its namespace. docs/next/RT-PATH-TRACING.md (section "ptaaa-omm") has the design and its status.
//
// Every trace pass tests alpha-tested candidates (fences, foliage, grass tufts, leaf cards) in its shader (RtAlpha.glsl rtAlphaPass):
// each candidate stops the hardware's traversal and runs code. With VK_EXT_opacity_micromap (RTX 40 / 50, other drivers that expose it)
// a triangle can carry a micromap: 4^level micro-triangles with a state each, read by the hardware during traversal. Here a micro-triangle
// is OPAQUE (committed by the hardware, no shader call) only where every texel rtAlphaPass can read on it passes the test with every
// dither, else UNKNOWN (the shader decides, as before). The picture is the same as without micromaps; the work where cut-outs are solid
// moves into the hardware. No TRANSPARENT state: the spec ignores such a candidate before it looks at the ray's opaque flag, and several
// passes trace cut-outs as solid cards with gl_RayFlagsOpaqueEXT on purpose (RtAOTrace AOAlpha 1, RtPathTrace PTBounceAlpha 0 / 3, AlphaTest
// off); they would see holes. (For the same reason nothing may ever trace with gl_RayFlagsForceOpacityMicromap2StateEXT.)
//
//   key        a geometry (static, NVR_RT_OP_FLAG_ALPHA, texcoords) x the instance's atlas slot x its alpha reference x the class (below):
//              one micromap structure (a second BLAS of the same triangles, the micromap attached) per key, built when first met (within a
//              per-frame budget) and traced through only after its states were checked (RtOmm.comp CHECK: the shader's own rtAlphaPass on
//              random points of the micro-triangles marked OPAQUE: any failure turns the micromaps off for the session, logged once)
//   class      the slot's REAL texture is sampled at a mip by the ray's footprint (rtTexLod); passes that never set rtConeWidth test at lod 0
//              exactly (the finest mip, the plain test). A frame whose passes are all of those (kFinestMip below) uses class-A structures
//              (states from lod 0); any other frame (the path tracer, RT reflections: rtConeWidth > 0 after their first hit) class B (states
//              from every mip: few known, but never wrong). The atlas (no real texture) has no mip: one class.
//   validity   the slot's real texture when built (id and RealTexture::serial): another one there since -> the plain BLAS until rebuilt
//   left out  decals (rtAlphaPassShadow: never solid for shadow rays), glass panes (merge 2026-11-07), the hidden first-person body, displaced instances, actors (DYNAMIC)
//   self-test  at the helper's start (a GPU with the extension): a known micromap traced (RtOmmProbe.comp) checks that the hardware's
//              micro-triangle order and state packing are the ones written here; a failure keeps the micromaps off (logged)
//   no extension  NVR_RT_OMM_SELF_TEST (the RT test host): the states are built and checked all the same (nothing traced through them)

namespace Omm {

	const uint32_t kMaxJobs = 8;					// micromaps built a frame
	const uint32_t kRecords = 8;					// record copies a check (8 dithers of rtAlphaPass's hash)
	const uint64_t kMicroPerFrame = 1ull << 21;		// micro-triangles built a frame (the builder: a thread each)
	const uint64_t kMicroPerGeometry = 1ull << 20;	// one micromap's micro-triangles at most (sets its level)
	const uint32_t kMaxLevel = 8;					// 65536 micro-triangles a triangle (a leaf card: about a texel each)
	const uint64_t kExpireFrames = 600;
	const VkDeviceSize kMaxBytes = 384ull << 20;	// micromaps + their structures alive at most
	const uint32_t kSamples = 16384, kSamplesSelfTest = 262144;	// check samples a micromap (the host's self-test: dense)

	struct Job {									// RtOmm.comp's Job (std430: 6 x 16 bytes)
		uint32_t geo[4], tex[4];
		float thr[4];
		uint32_t data[4], index[4], check[4];
	};
	static_assert(sizeof(Job) == 96, "RtOmm.comp's Job");

	enum VariantState { kBuilding, kReady, kSkipped, kChecked };	// kSkipped: no level fits / no memory; kChecked: self-test only, freed
	struct Variant {
		uint32_t geometryId = 0, slot = 0, ref = 0, cls = 0, level = 0, triangles = 0, texId = 0;
		uint64_t texSerial = 0;
		VariantState state = kBuilding;
		uint64_t frame = 0, lastUsed = 0;
		uint32_t cmdSlot = 0, job = 0;
		Buffer input;								// device local: the triangle array, the index buffer, the states
		VkDeviceSize triAt = 0, indexAt = 0, dataAt = 0;
		Buffer storage;								// the micromap's memory
		VkMicromapEXT micromap = VK_NULL_HANDLE;
		Geometry geo;								// its structure (the structure pool) and address
		VkDeviceSize bytes = 0;
	};
	struct PendingFree { uint64_t frame; Buffer input, storage; VkMicromapEXT micromap; };

	struct Module {
		bool tried = false, ok = false;
		bool hwTested = false, hwOk = false;		// the startup self-test (a GPU with the extension)
		bool disabled = false;						// a check failed: off for the session
		bool active = false;						// this frame: the scene may trace through ready structures
		bool finest = false;						// this frame: every pass tests alpha at lod 0 (class A)
		uint64_t frame = 0;
		VkDescriptorSetLayout layout = VK_NULL_HANDLE;
		VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
		VkPipeline pipeline = VK_NULL_HANDLE;
		VkDescriptorPool pool = VK_NULL_HANDLE;
		VkDescriptorSet sets[kCmdRing] = {};
		Buffer jobs[kCmdRing], counters[kCmdRing], records[kCmdRing];
		Buffer scratch;
		std::unordered_map<uint64_t, Variant> variants;
		std::vector<PendingFree> frees;
		std::vector<int8_t> finestOf;				// per shader index: 1 its alpha tests are at lod 0, 0 not known to be, -1 not looked at
		VkDeviceSize bytes = 0;
		uint64_t built = 0, micro = 0, opaque = 0, checked = 0, violations = 0, budgetWaits = 0, skipped = 0;
		uint32_t ready = 0, instances = 0, framesA = 0, framesB = 0;
		bool loggedFirst = false;
	};
	static Module g_omm;

	uint64_t KeyOf(uint32_t geometryId, uint32_t slot, uint32_t ref, uint32_t cls) {
		return ((uint64_t)(geometryId & 0xFFFFFFu) << 19) | ((uint64_t)(slot & 0x3FFu) << 9) | ((uint64_t)(ref & 0xFFu) << 1) | (cls & 1u);
	}

	// The shaders known to leave rtConeWidth at 0 (src/hlsl/NewVegas/Compute/Rt: only RtPathTrace.comp and ReflectionsRTBody.glsl set it, and
	// their variants: RtPathTraceGrid / GI, ReflectionsRT*), so their alpha tests read the real textures at lod 0 exactly. Any other shader
	// name (a new pass) counts as "any mip": class B, always right. Keep this list in step with the shaders (RtTextures.glsl says so too).
	bool FinestMipShader(const char* path) {
		const char* name = strrchr(path, '\\');
		const char* slash = strrchr(path, '/');
		if (slash && (!name || slash > name)) name = slash;
		name = name ? name + 1 : path;
		char base[64] = {};
		for (size_t i = 0; i + 1 < sizeof(base) && name[i] && name[i] != '.'; i++) base[i] = (char)tolower((unsigned char)name[i]);
		static const char* kFinestMip[] = { "rtsunshadow", "rtlamptrace", "rtlamptraceexterior", "rtlamptracesigma", "rtlamptracesplit",
			"rtlamptracemany", "rtlamptracerestir", "rtlampeye", "rtlampprimary", "rtflashtrace", "rtaotrace", "rtgitrace", "rtradiancecache",
			"rtrestirgi", "rthitdebug", "rtlampfilter", "rtpathtracecache" };
		for (const char* k : kFinestMip) if (!strcmp(base, k)) return true;
		return !strncmp(base, "rtshadowdenoise", 15);	// the RT shadow denoiser's passes trace nothing
	}

	bool FrameFinest(Helper& h) {
		Module& m = g_omm;
		const NvrRtFrame& f = h.frame;
		for (uint32_t p = 0; p < f.passCount; p++) {
			const uint32_t s = f.passes[p].shader;
			if (s >= NVR_RT_MAX_SHADERS) return false;
			if (m.finestOf.size() <= s) m.finestOf.resize(s + 1, -1);
			if (m.finestOf[s] < 0) {
				char path[261] = {};
				memcpy(path, g_shared->shaderPaths[s], 260);
				m.finestOf[s] = FinestMipShader(path) ? 1 : 0;
			}
			if (!m.finestOf[s]) return false;
		}
		return true;
	}

	// An instance that may be traced through a micromap structure: alpha-tested, static, not a decal / the hidden body / displaced
	const Geometry* Eligible(Helper& h, const NvrRtInstance& in, const NvrRtInstanceRecord& r) {
		if (!(r.flags & NVR_RT_RECORD_ALPHA_TESTED)) return nullptr;
		if (in.flags & NVR_RT_INSTANCE_DISPLACE) return nullptr;
		if (in.customIndex & (NVR_RT_CUSTOM_DECAL | NVR_RT_CUSTOM_HIDDEN)) return nullptr;
		// (merge 2026-11-07, ptaaa-materials) glass panes: every shader passes them (only the path tracer tints by them); a micromap's
		// opaque state would make a pane block shadow rays in hardware
		if (NvrRtKindOf(r.flags) == NVR_RT_KIND_GLASS) return nullptr;
		auto it = h.geometries.find(in.geometryId);
		if (it == h.geometries.end()) return nullptr;
		const Geometry& g = it->second;
		if (g.dynamic || !(g.opFlags & NVR_RT_OP_FLAG_ALPHA) || !g.as || g.uvOffset == NVR_RT_NO_DATA || g.vertexOffset == NVR_RT_NO_DATA || !g.triangles)
			return nullptr;
		return &g;
	}

	uint32_t SlotOf(const NvrRtInstanceRecord& r) { return (r.flags >> 16) & 0x3FFu; }
	uint32_t RefOf(const NvrRtInstanceRecord& r) { return (r.flags >> 8) & 0xFFu; }
	uint32_t TextureOf(Helper& h, uint32_t slot) { return slot < NVR_RT_TEX_TABLE_SLOTS ? h.tex.table[slot][0] : 0u; }
	uint64_t SerialOf(Helper& h, uint32_t id) { return id && id < NVR_RT_MAX_TEXTURES ? h.tex.textures[id].serial : 0ull; }

	void FreeVariant(Helper& h, Variant& v, uint64_t frame) {
		Module& m = g_omm;
		if (v.geo.as) {
			Geometry g = v.geo;
			g.vertexOffset = NVR_RT_NO_DATA;	// (the words are the source geometry's)
			g.dataWords = 0;
			h.pendingFrees.push_back({ frame, g });
		}
		if (v.input.buffer || v.storage.buffer || v.micromap) m.frees.push_back({ frame, v.input, v.storage, v.micromap });
		if (v.state == kReady && m.ready) m.ready--;
		m.bytes -= std::min(m.bytes, v.bytes);
		v.input = Buffer(); v.storage = Buffer(); v.micromap = VK_NULL_HANDLE; v.geo = Geometry(); v.bytes = 0;
	}

	void FreeAll(Helper& h, uint64_t frame) {
		Module& m = g_omm;
		for (auto& kv : m.variants) FreeVariant(h, kv.second, frame);
		m.variants.clear();
		m.ready = 0;
	}

	// ReleaseGeometry: the geometry is gone or rebuilt: its micromap structures go (rebuilt later from the new data)
	void DropFor(Helper& h, uint32_t id, uint64_t frame) {
		Module& m = g_omm;
		for (auto it = m.variants.begin(); it != m.variants.end();) {
			if (it->second.geometryId == id) { FreeVariant(h, it->second, frame); it = m.variants.erase(it); }
			else ++it;
		}
	}

	void Disable(Helper& h, const char* why, uint64_t frame) {
		Module& m = g_omm;
		if (m.disabled) return;
		m.disabled = true;
		m.active = false;
		Status("opacity micromaps: OFF for this session: %s (the shaders' alpha test as before)", why);
		FreeAll(h, frame);
	}

	// The scene: the structure to trace instance `in` through (null: its own)
	const Geometry* VariantFor(Helper& h, const NvrRtInstance& in, const NvrRtInstanceRecord& r) {
		Module& m = g_omm;
		if (!m.active || m.disabled || !Eligible(h, in, r)) return nullptr;
		const uint32_t slot = SlotOf(r), ref = RefOf(r), tex = TextureOf(h, slot);
		const uint32_t classes[2] = { 0u, 1u };
		for (uint32_t k = tex ? (m.finest ? 0u : 1u) : 0u; k < (tex ? 2u : 1u); k++) {
			auto it = m.variants.find(KeyOf(in.geometryId, slot, ref, classes[k]));
			if (it == m.variants.end()) continue;
			Variant& v = it->second;
			if (v.state != kReady || v.texId != tex || v.texSerial != SerialOf(h, tex) || !v.geo.address) continue;
			v.lastUsed = m.frame;
			m.instances++;
			return &v.geo;
		}
		return nullptr;
	}

	bool Create(Helper& h) {
		Module& m = g_omm;
		if (m.tried) return m.ok;
		m.tried = true;
		VkDescriptorSetLayoutBinding b[3] = {};
		b[0] = { 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
		b[1] = { NVR_RT_BINDING_RECORDS, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
		b[2] = { NVR_RT_BINDING_GEOMETRY, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
		VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
		li.bindingCount = 3;
		li.pBindings = b;
		if (vkCreateDescriptorSetLayout(h.vk.device, &li, nullptr, &m.layout) != VK_SUCCESS) { Log("opacity micromaps: OFF (descriptor set layout)"); return false; }
		const VkDescriptorSetLayout layouts[2] = { m.layout, h.tex.layout };	// set 1: the real textures (rtAlphaPass reads them)
		VkPushConstantRange range = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 32 };
		VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
		pli.setLayoutCount = 2;
		pli.pSetLayouts = layouts;
		pli.pushConstantRangeCount = 1;
		pli.pPushConstantRanges = &range;
		VkShaderModuleCreateInfo mi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
		mi.codeSize = sizeof(kRtOmmSpv);
		mi.pCode = kRtOmmSpv;
		VkShaderModule module = VK_NULL_HANDLE;
		if (vkCreatePipelineLayout(h.vk.device, &pli, nullptr, &m.pipelineLayout) != VK_SUCCESS || vkCreateShaderModule(h.vk.device, &mi, nullptr, &module) != VK_SUCCESS) {
			Log("opacity micromaps: OFF (pipeline layout / shader module)");
			return false;
		}
		VkComputePipelineCreateInfo pi = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
		pi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
		pi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
		pi.stage.module = module;
		pi.stage.pName = "main";
		pi.layout = m.pipelineLayout;
		const VkResult r = vkCreateComputePipelines(h.vk.device, VK_NULL_HANDLE, 1, &pi, nullptr, &m.pipeline);
		vkDestroyShaderModule(h.vk.device, module, nullptr);
		if (r != VK_SUCCESS) { Log("opacity micromaps: OFF (pipeline: %s)", VkResultName(r)); m.pipeline = VK_NULL_HANDLE; return false; }
		VkDescriptorPoolSize sizes[2] = { { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kCmdRing }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * kCmdRing } };
		VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
		dpi.maxSets = kCmdRing;
		dpi.poolSizeCount = 2;
		dpi.pPoolSizes = sizes;
		if (vkCreateDescriptorPool(h.vk.device, &dpi, nullptr, &m.pool) != VK_SUCCESS) { Log("opacity micromaps: OFF (descriptor pool)"); return false; }
		VkDescriptorSetLayout all[kCmdRing];
		for (uint32_t i = 0; i < kCmdRing; i++) all[i] = m.layout;
		VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
		ai.descriptorPool = m.pool;
		ai.descriptorSetCount = kCmdRing;
		ai.pSetLayouts = all;
		if (vkAllocateDescriptorSets(h.vk.device, &ai, m.sets) != VK_SUCCESS) { Log("opacity micromaps: OFF (descriptor sets)"); return false; }
		for (uint32_t i = 0; i < kCmdRing; i++) {
			if (!CreateBuffer(h.vk, sizeof(Job) * kMaxJobs, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, m.jobs[i]) ||
				!CreateBuffer(h.vk, 32 * kMaxJobs, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, m.counters[i]) ||
				!CreateBuffer(h.vk, sizeof(NvrRtInstanceRecord) * kRecords * kMaxJobs, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, m.records[i])) {
				Log("opacity micromaps: OFF (job buffers)");
				return false;
			}
			memset(m.counters[i].mapped, 0, 32 * kMaxJobs);
		}
		m.ok = true;
		Log("opacity micromaps: builder ready (RtOmm.comp: up to %u micromaps, %llu micro-triangles a frame, level %u at most)", kMaxJobs,
			(unsigned long long)kMicroPerFrame, std::min(kMaxLevel, h.vk.omm ? h.vk.ommMaxLevel : kMaxLevel));
		return true;
	}

	// At the helper's start, before READY, on a GPU with the extension: a small geometry with a known pseudo-random micromap (OPAQUE /
	// UNKNOWN) is traced (RtOmmProbe.comp) and the hardware's answers compared with the states as this file writes and reads them (the spec's
	// curve, LSB-first packing, the triangle array, the index buffer). Levels 1, 3 and the highest one used. False: the micromaps stay off.
	bool SelfTest(Helper& h) {
		Module& m = g_omm;
		m.hwTested = true;
		if (!h.vk.omm) return false;
		VulkanDevice& vk = h.vk;
		const uint32_t top = std::min(kMaxLevel, vk.ommMaxLevel);
		const float pos[7][3] = { { 0, 0, 0 }, { 100, 0, 0 }, { 100, 100, 0 }, { 0, 100, 0 }, { 200, 0, 0 }, { 330, 40, 0 }, { 250, 120, 0 } };
		const uint32_t idx[3][3] = { { 0, 1, 2 }, { 0, 2, 3 }, { 4, 5, 6 } };
		const uint32_t T = 3, kSamplesProbe = 65536;
		VkDescriptorSetLayout layout = VK_NULL_HANDLE;
		VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
		VkPipeline pipeline = VK_NULL_HANDLE;
		VkDescriptorPool pool = VK_NULL_HANDLE;
		VkDescriptorSet set = VK_NULL_HANDLE;
		bool pass = true;
		char report[512] = {};
		{
			VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
			VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
			li.bindingCount = 1; li.pBindings = &b;
			VkPushConstantRange range = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 48 };
			VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
			pli.setLayoutCount = 1; pli.pSetLayouts = &layout; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &range;
			VkShaderModuleCreateInfo mi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
			mi.codeSize = sizeof(kRtOmmProbeSpv); mi.pCode = kRtOmmProbeSpv;
			VkShaderModule module = VK_NULL_HANDLE;
			VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1 };
			VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
			dpi.maxSets = 1; dpi.poolSizeCount = 1; dpi.pPoolSizes = &ps;
			bool ok = vkCreateDescriptorSetLayout(vk.device, &li, nullptr, &layout) == VK_SUCCESS && vkCreatePipelineLayout(vk.device, &pli, nullptr, &pipelineLayout) == VK_SUCCESS &&
				vkCreateShaderModule(vk.device, &mi, nullptr, &module) == VK_SUCCESS && vkCreateDescriptorPool(vk.device, &dpi, nullptr, &pool) == VK_SUCCESS;
			if (ok) {
				VkComputePipelineCreateInfo pi = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
				pi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
				pi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; pi.stage.module = module; pi.stage.pName = "main";
				pi.layout = pipelineLayout;
				ok = vkCreateComputePipelines(vk.device, VK_NULL_HANDLE, 1, &pi, nullptr, &pipeline) == VK_SUCCESS;
				VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
				ai.descriptorPool = pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &layout;
				ok = ok && vkAllocateDescriptorSets(vk.device, &ai, &set) == VK_SUCCESS;
			}
			if (module) vkDestroyShaderModule(vk.device, module, nullptr);
			if (!ok) { pass = false; sprintf_s(report, "the probe pipeline could not be made"); }
		}
		const uint32_t levels[3] = { 1u, std::min(3u, top), top };
		for (uint32_t li = 0; li < 3 && pass; li++) {
			const uint32_t level = levels[li];
			if (!level || (li > 0 && level == levels[li - 1])) continue;
			const VkDeviceSize bpt = OmmMath::OmmBytesPerTriangle(level), dataBytes = T * bpt;
			const VkDeviceSize posAt = 0, idxAt = 256, triAt = 512, mmIdxAt = 768, dataAt = 1024, resAt = dataAt + AlignUp(dataBytes, 256);
			Buffer in, mmStore, blasStore, tlasStore, inst, scratch;
			VkMicromapEXT mm = VK_NULL_HANDLE;
			VkAccelerationStructureKHR blas = VK_NULL_HANDLE, tlas = VK_NULL_HANDLE;
			bool ok = CreateBuffer(vk, resAt + 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_MICROMAP_BUILD_INPUT_READ_ONLY_BIT_EXT |
				VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true, in) &&
				CreateBuffer(vk, sizeof(VkAccelerationStructureInstanceKHR), VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true, inst);
			if (ok) {
				uint8_t* p = (uint8_t*)in.mapped;
				memset(p, 0, (size_t)in.size);
				memcpy(p + posAt, pos, sizeof(pos));
				memcpy(p + idxAt, idx, sizeof(idx));
				for (uint32_t t = 0; t < T; t++) {
					VkMicromapTriangleEXT tri = { (uint32_t)(t * bpt), (uint16_t)level, (uint16_t)VK_OPACITY_MICROMAP_FORMAT_4_STATE_EXT };
					memcpy(p + triAt + 8 * t, &tri, 8);
					memcpy(p + mmIdxAt + 4 * t, &t, 4);
					for (uint32_t i = 0; i < (1u << (2 * level)); i++) {
						uint32_t x = t * 0x9E3779B9u ^ (i * 0x85EBCA6Bu) ^ (level * 0xC2B2AE35u);
						x ^= x >> 15; x *= 0x2C1B3C6Du; x ^= x >> 12;
						const uint32_t state = (x >> 7) & 1u ? 1u : 3u;
						const uint64_t bit = (uint64_t)t * bpt * 8 + 2ull * i;
						p[dataAt + (bit >> 3)] |= (uint8_t)(state << (bit & 7));
					}
				}
				VkAccelerationStructureInstanceKHR* ins = (VkAccelerationStructureInstanceKHR*)inst.mapped;
				*ins = VkAccelerationStructureInstanceKHR();
				ins->transform.matrix[0][0] = ins->transform.matrix[1][1] = ins->transform.matrix[2][2] = 1.0f;
				ins->mask = 0xFF;
				ins->flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
			}
			VkMicromapUsageEXT usage = { T, level, VK_OPACITY_MICROMAP_FORMAT_4_STATE_EXT };
			VkMicromapBuildInfoEXT bi = { VK_STRUCTURE_TYPE_MICROMAP_BUILD_INFO_EXT };
			bi.type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT; bi.flags = VK_BUILD_MICROMAP_PREFER_FAST_TRACE_BIT_EXT; bi.mode = VK_BUILD_MICROMAP_MODE_BUILD_EXT;
			bi.usageCountsCount = 1; bi.pUsageCounts = &usage;
			VkMicromapBuildSizesInfoEXT ms = { VK_STRUCTURE_TYPE_MICROMAP_BUILD_SIZES_INFO_EXT };
			if (ok) vk.micromapBuildSizes(vk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &ms);
			ok = ok && ms.micromapSize && CreateBuffer(vk, ms.micromapSize, VK_BUFFER_USAGE_MICROMAP_STORAGE_BIT_EXT, false, mmStore);
			if (ok) {
				VkMicromapCreateInfoEXT ci = { VK_STRUCTURE_TYPE_MICROMAP_CREATE_INFO_EXT };
				ci.buffer = mmStore.buffer; ci.size = ms.micromapSize; ci.type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT;
				ok = vk.createMicromap(vk.device, &ci, nullptr, &mm) == VK_SUCCESS;
			}
			VkAccelerationStructureTrianglesOpacityMicromapEXT ot = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_TRIANGLES_OPACITY_MICROMAP_EXT };
			ot.indexType = VK_INDEX_TYPE_UINT32; ot.indexBuffer.deviceAddress = in.address + mmIdxAt; ot.indexStride = 4;
			ot.usageCountsCount = 1; ot.pUsageCounts = &usage; ot.micromap = mm;
			VkAccelerationStructureGeometryKHR g = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
			g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
			g.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
			g.geometry.triangles.pNext = &ot;
			g.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
			g.geometry.triangles.vertexData.deviceAddress = in.address + posAt;
			g.geometry.triangles.vertexStride = 12;
			g.geometry.triangles.maxVertex = 6;
			g.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
			g.geometry.triangles.indexData.deviceAddress = in.address + idxAt;
			VkAccelerationStructureBuildGeometryInfoKHR bb = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
			bb.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR; bb.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
			bb.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR; bb.geometryCount = 1; bb.pGeometries = &g;
			VkAccelerationStructureBuildSizesInfoKHR bs = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
			if (ok) vk.buildSizes(vk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bb, &T, &bs);
			ok = ok && CreateBuffer(vk, bs.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false, blasStore);
			if (ok) {
				VkAccelerationStructureCreateInfoKHR ci = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
				ci.buffer = blasStore.buffer; ci.size = bs.accelerationStructureSize; ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
				ok = vk.createAs(vk.device, &ci, nullptr, &blas) == VK_SUCCESS;
			}
			if (ok) {
				VkAccelerationStructureDeviceAddressInfoKHR ai = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
				ai.accelerationStructure = blas;
				((VkAccelerationStructureInstanceKHR*)inst.mapped)->accelerationStructureReference = vk.asAddress(vk.device, &ai);
			}
			VkAccelerationStructureGeometryKHR tg = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
			tg.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
			tg.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
			tg.geometry.instances.data.deviceAddress = inst.address;
			VkAccelerationStructureBuildGeometryInfoKHR tb = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
			tb.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR; tb.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
			tb.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR; tb.geometryCount = 1; tb.pGeometries = &tg;
			const uint32_t one = 1;
			VkAccelerationStructureBuildSizesInfoKHR ts = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
			if (ok) vk.buildSizes(vk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &tb, &one, &ts);
			ok = ok && CreateBuffer(vk, ts.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false, tlasStore);
			if (ok) {
				VkAccelerationStructureCreateInfoKHR ci = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
				ci.buffer = tlasStore.buffer; ci.size = ts.accelerationStructureSize; ci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
				ok = vk.createAs(vk.device, &ci, nullptr, &tlas) == VK_SUCCESS;
			}
			const VkDeviceSize align = std::max<VkDeviceSize>(vk.asProperties.minAccelerationStructureScratchOffsetAlignment, 256);
			const VkDeviceSize scratchBytes = std::max(ms.buildScratchSize, std::max(bs.buildScratchSize, ts.buildScratchSize)) + align;
			ok = ok && CreateBuffer(vk, scratchBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, scratch);
			uint32_t got[4] = {};
			if (ok) {
				const uint64_t scr = AlignUp(scratch.address, align);
				VkCommandBuffer cmd = h.slots[0].cmd;
				VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
				begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
				ok = vkBeginCommandBuffer(cmd, &begin) == VK_SUCCESS;
				if (ok) {
					bi.dstMicromap = mm; bi.data.deviceAddress = in.address + dataAt; bi.triangleArray.deviceAddress = in.address + triAt;
					bi.triangleArrayStride = 8; bi.scratchData.deviceAddress = scr;
					vk.cmdBuildMicromaps(cmd, 1, &bi);
					FullBarrier(cmd);
					bb.dstAccelerationStructure = blas; bb.scratchData.deviceAddress = scr;
					VkAccelerationStructureBuildRangeInfoKHR br = { T, 0, 0, 0 };
					const VkAccelerationStructureBuildRangeInfoKHR* brp = &br;
					vk.cmdBuildAs(cmd, 1, &bb, &brp);
					FullBarrier(cmd);
					tb.dstAccelerationStructure = tlas; tb.scratchData.deviceAddress = scr;
					VkAccelerationStructureBuildRangeInfoKHR tr = { 1, 0, 0, 0 };
					const VkAccelerationStructureBuildRangeInfoKHR* trp = &tr;
					vk.cmdBuildAs(cmd, 1, &tb, &trp);
					FullBarrier(cmd);
					VkWriteDescriptorSetAccelerationStructureKHR asw = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR };
					asw.accelerationStructureCount = 1; asw.pAccelerationStructures = &tlas;
					VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, &asw, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR };
					vkUpdateDescriptorSets(vk.device, 1, &w, 0, nullptr);
					vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
					vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &set, 0, nullptr);
					struct { uint64_t positions, indices, states, results; uint32_t triangles, level, samples, seed; } push = {
						in.address + posAt, in.address + idxAt, in.address + dataAt, in.address + resAt, T, level, kSamplesProbe, 12345u + level };
					vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
					vkCmdDispatch(cmd, (kSamplesProbe + 63) / 64, 1, 1);
					VkMemoryBarrier hb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
					hb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
					vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, nullptr, 0, nullptr);
					ok = vkEndCommandBuffer(cmd) == VK_SUCCESS;
					if (ok) {
						VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
						submit.commandBufferCount = 1; submit.pCommandBuffers = &cmd;
						ok = vkQueueSubmit(vk.queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS && vkQueueWaitIdle(vk.queue) == VK_SUCCESS;
					}
					vkResetCommandBuffer(cmd, 0);
				}
				if (ok) memcpy(got, (const uint8_t*)in.mapped + resAt, sizeof(got));
			}
			if (!ok) { pass = false; sprintf_s(report, "level %u: the micromap, its structures or the probe could not be made / run", level); }
			else if (got[0] < kSamplesProbe / 4 || got[1] || got[2]) {
				pass = false;
				sprintf_s(report, "level %u: %u rays, %u hits where the states say UNKNOWN, %u misses where they say OPAQUE (%u on another triangle): "
					"the hardware does not read the micromap as this helper writes it", level, got[0], got[1], got[2], got[3]);
			}
			else Log("opacity micromaps: hardware self-test level %u: %u rays, every answer as the states say (%u hits on a neighbour)", level, got[0], got[3]);
			if (tlas) vk.destroyAs(vk.device, tlas, nullptr);
			if (blas) vk.destroyAs(vk.device, blas, nullptr);
			if (mm) vk.destroyMicromap(vk.device, mm, nullptr);
			for (Buffer* b : { &in, &mmStore, &blasStore, &tlasStore, &inst, &scratch }) DestroyBuffer(vk, *b);
		}
		if (pipeline) vkDestroyPipeline(vk.device, pipeline, nullptr);
		if (pool) vkDestroyDescriptorPool(vk.device, pool, nullptr);
		if (pipelineLayout) vkDestroyPipelineLayout(vk.device, pipelineLayout, nullptr);
		if (layout) vkDestroyDescriptorSetLayout(vk.device, layout, nullptr);
		m.hwOk = pass;
		if (pass) Log("opacity micromaps: hardware self-test passed (levels 1, %u, %u): alpha-tested geometry gets micromaps where [Main.Main.RayTracing] OpacityMicromaps is on", levels[1], top);
		else {
			char why[600];
			sprintf_s(why, "the hardware self-test failed: %s", report);
			Disable(h, why, 0);
		}
		return pass;
	}

	void PublishStats(Helper& h, uint32_t state) {
		Module& m = g_omm;
		if (!g_shared) return;
		g_shared->ommState = state;
		g_shared->ommVariants = m.ready;
		g_shared->ommBuilt = m.built;
		g_shared->ommMicro = m.micro;
		g_shared->ommOpaque = m.opaque;
		g_shared->ommChecked = m.checked;
		g_shared->ommViolations = m.violations;
		g_shared->ommFlags = (h.vk.omm ? NVR_RT_OMM_FLAG_EXTENSION : 0u) | (m.hwOk ? NVR_RT_OMM_FLAG_HW_TESTED : 0u) | (m.finest ? NVR_RT_OMM_FLAG_FINEST_MIP : 0u);
	}

	// The checks of earlier frames' builds (their frame is done): ready, or off for the session
	void Collect(Helper& h, uint64_t done) {
		Module& m = g_omm;
		std::vector<uint64_t> drop;
		for (auto& kv : m.variants) {
			Variant& v = kv.second;
			if (v.state != kBuilding || v.frame > done) continue;
			const uint32_t* c = (const uint32_t*)m.counters[v.cmdSlot].mapped + 8u * v.job;
			const uint64_t micro = (uint64_t)v.triangles << (2 * v.level);
			m.built++;
			m.micro += micro;
			m.opaque += c[0];
			m.checked += c[1];
			m.violations += c[2];
			if (c[2]) {
				char why[400];
				float bu = 0, bv = 0;
				memcpy(&bu, &c[6], 4); memcpy(&bv, &c[7], 4);
				sprintf_s(why, "a check found %u of %u samples on micro-triangles marked opaque where the shader's alpha test says not solid "
					"(geometry %u, slot %u, ref %u, %s, level %u: triangle %u, micro-triangle %u, barycentrics %.7f %.7f)", c[2], c[1], v.geometryId, v.slot,
					v.ref, v.texId ? (v.cls ? "real texture, every mip" : "real texture, lod 0") : "the atlas", v.level, c[4] ? c[4] - 1 : 0, c[5], bu, bv);
				Disable(h, why, m.frame);
				return;
			}
			if (!m.loggedFirst || (m.built % 64) == 0)
				Log("opacity micromaps: geometry %u (slot %u, ref %u, %s, %u triangles, level %u): %u of %llu micro-triangles opaque, %u check samples on them, none failed",
					v.geometryId, v.slot, v.ref, v.texId ? (v.cls ? "real texture, every mip" : "real texture, lod 0") : "the atlas", v.triangles, v.level, c[0],
					(unsigned long long)micro, c[1]);
			m.loggedFirst = true;
			if (v.micromap && v.geo.as) { v.state = kReady; m.ready++; }
			else { v.state = kChecked; FreeVariant(h, v, m.frame); }	// self-test without the extension: checked, nothing to trace
		}
	}

	// RecordFrame, after the builds and the displaced copies, before the scene: this frame's micromaps
	void Run(Helper& h, uint32_t slot, VkCommandBuffer cmd, uint64_t frameNumber) {
		Module& m = g_omm;
		m.frame = frameNumber;
		m.active = false;
		m.instances = 0;
		uint64_t done = 0;
		vkGetSemaphoreCounterValue(h.vk.device, h.outputReady, &done);
		for (auto it = m.frees.begin(); it != m.frees.end();) {
			if (it->frame > done) { ++it; continue; }
			if (it->micromap) h.vk.destroyMicromap(h.vk.device, it->micromap, nullptr);
			DestroyBuffer(h.vk, it->input);
			DestroyBuffer(h.vk, it->storage);
			it = m.frees.erase(it);
		}
		Collect(h, done);
		const uint32_t control = g_shared ? g_shared->opacityMicromaps : 0u;
		const bool on = (control & NVR_RT_OMM_ON) != 0, selfTest = (control & NVR_RT_OMM_SELF_TEST) != 0, breakTest = (control & NVR_RT_OMM_BREAK_TEST) != 0;
		const uint32_t atlas = (control & NVR_RT_OMM_ATLAS_MASK);
		const bool hw = h.vk.omm && m.hwOk;
		if (m.disabled) { PublishStats(h, NVR_RT_OMM_STATE_DISABLED); return; }
		const bool atlasOk = atlas && atlas - 1 < h.imageCount && h.images[atlas - 1].view && h.images[atlas - 1].format == VK_FORMAT_R16_SFLOAT;
		const bool build = atlasOk && ((on && hw) || selfTest);
		if (!build) {
			if (!m.variants.empty()) FreeAll(h, frameNumber);	// (turned off: the structures go)
			PublishStats(h, !on || !atlasOk ? NVR_RT_OMM_STATE_OFF : NVR_RT_OMM_STATE_NO_EXT);
			return;
		}
		if (!Create(h)) { PublishStats(h, NVR_RT_OMM_STATE_OFF); return; }
		m.active = on && hw;
		m.finest = FrameFinest(h);
		if (m.finest) m.framesA++; else m.framesB++;
		const bool makeStructures = on && hw;	// (else: states built and checked only)

		// 1. the keys this frame's instances want (first come), within the budgets
		const NvrRtFrame& f = h.frame;
		Job* jobs = (Job*)m.jobs[slot].mapped;
		NvrRtInstanceRecord* recs = (NvrRtInstanceRecord*)m.records[slot].mapped;
		memset(m.counters[slot].mapped, 0, 32 * kMaxJobs);
		std::vector<Variant*> made;
		uint64_t spent = 0;
		VkDeviceSize scratchTotal = 0;
		const VkDeviceSize align = std::max<VkDeviceSize>(h.vk.asProperties.minAccelerationStructureScratchOffsetAlignment, 256);
		std::vector<VkMicromapBuildInfoEXT> mmInfos;
		std::vector<VkMicromapUsageEXT> usages;
		std::vector<VkAccelerationStructureTrianglesOpacityMicromapEXT> ommTris;
		std::vector<VkAccelerationStructureGeometryKHR> geoms;
		std::vector<VkAccelerationStructureBuildGeometryInfoKHR> asInfos;
		std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
		std::vector<VkDeviceSize> mmScratchAt, asScratchAt;
		usages.reserve(kMaxJobs); ommTris.reserve(kMaxJobs); geoms.reserve(kMaxJobs); mmInfos.reserve(kMaxJobs); asInfos.reserve(kMaxJobs); ranges.reserve(kMaxJobs);
		const uint32_t maxLevel = std::min(kMaxLevel, makeStructures ? h.vk.ommMaxLevel : kMaxLevel);
		for (uint32_t i = 0; i < f.instanceCount; i++) {
			const NvrRtInstance& in = f.instances[i];
			const NvrRtInstanceRecord& r = f.records[i];
			const Geometry* src = Eligible(h, in, r);
			if (!src) continue;
			const uint32_t s = SlotOf(r), ref = RefOf(r), tex = TextureOf(h, s), cls = tex && !m.finest ? 1u : 0u;
			const uint64_t key = KeyOf(in.geometryId, s, ref, cls);
			auto it = m.variants.find(key);
			if (it != m.variants.end()) {
				Variant& old = it->second;
				const bool stale = old.texId != tex || old.texSerial != SerialOf(h, tex);
				if (!stale || old.state == kBuilding) { old.lastUsed = frameNumber; continue; }
				FreeVariant(h, old, frameNumber);	// (the slot's texture changed: made again below)
				m.variants.erase(it);
			}
			if (made.size() >= kMaxJobs) { m.budgetWaits++; break; }
			Variant v;
			v.geometryId = in.geometryId; v.slot = s; v.ref = ref; v.cls = cls; v.texId = tex; v.texSerial = SerialOf(h, tex);
			v.triangles = src->triangles; v.lastUsed = frameNumber; v.frame = frameNumber; v.cmdSlot = slot;
			v.level = OmmMath::OmmLevel(src->triangles, maxLevel, kMicroPerGeometry);
			if (!v.level) { v.state = kSkipped; m.skipped++; m.variants[key] = v; continue; }
			const uint64_t micro = (uint64_t)v.triangles << (2 * v.level);
			if (spent && spent + micro > kMicroPerFrame) { m.budgetWaits++; continue; }
			if (m.bytes > kMaxBytes) { m.budgetWaits++; break; }
			const VkDeviceSize bpt = OmmMath::OmmBytesPerTriangle(v.level);
			v.triAt = 0;
			v.indexAt = AlignUp((VkDeviceSize)v.triangles * 8, 256);
			v.dataAt = v.indexAt + AlignUp((VkDeviceSize)v.triangles * 4, 256);
			const VkDeviceSize inputBytes = v.dataAt + AlignUp((VkDeviceSize)v.triangles * bpt, 256);
			VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
			if (makeStructures) usage |= VK_BUFFER_USAGE_MICROMAP_BUILD_INPUT_READ_ONLY_BIT_EXT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
			if (!CreateBuffer(h.vk, inputBytes, usage, false, v.input)) { v.state = kSkipped; m.skipped++; m.variants[key] = v; continue; }
			v.bytes = inputBytes;
			usages.push_back({ v.triangles, v.level, VK_OPACITY_MICROMAP_FORMAT_4_STATE_EXT });
			if (makeStructures) {
				VkMicromapBuildInfoEXT bi = { VK_STRUCTURE_TYPE_MICROMAP_BUILD_INFO_EXT };
				bi.type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT;
				bi.flags = VK_BUILD_MICROMAP_PREFER_FAST_TRACE_BIT_EXT;
				bi.mode = VK_BUILD_MICROMAP_MODE_BUILD_EXT;
				bi.usageCountsCount = 1;
				bi.pUsageCounts = &usages.back();
				VkMicromapBuildSizesInfoEXT ms = { VK_STRUCTURE_TYPE_MICROMAP_BUILD_SIZES_INFO_EXT };
				h.vk.micromapBuildSizes(h.vk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &ms);
				bool okBuild = ms.micromapSize > 0 && CreateBuffer(h.vk, ms.micromapSize, VK_BUFFER_USAGE_MICROMAP_STORAGE_BIT_EXT, false, v.storage);
				if (okBuild) {
					VkMicromapCreateInfoEXT ci = { VK_STRUCTURE_TYPE_MICROMAP_CREATE_INFO_EXT };
					ci.buffer = v.storage.buffer;
					ci.size = ms.micromapSize;
					ci.type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT;
					okBuild = h.vk.createMicromap(h.vk.device, &ci, nullptr, &v.micromap) == VK_SUCCESS;
				}
				// the second structure: the same triangles (the geometry data's words), non-opaque, the micromap attached
				VkAccelerationStructureTrianglesOpacityMicromapEXT ot = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_TRIANGLES_OPACITY_MICROMAP_EXT };
				ot.indexType = VK_INDEX_TYPE_UINT32;
				ot.indexBuffer.deviceAddress = v.input.address + v.indexAt;
				ot.indexStride = 4;
				ot.usageCountsCount = 1;
				ot.pUsageCounts = &usages.back();
				ot.micromap = v.micromap;
				ommTris.push_back(ot);
				VkAccelerationStructureGeometryKHR g = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
				g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
				g.flags = 0;	// non-opaque, as the source (the micromap decides where it has a state)
				g.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
				g.geometry.triangles.pNext = &ommTris.back();
				g.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
				g.geometry.triangles.vertexData.deviceAddress = h.geometryData.address + (VkDeviceSize)src->vertexOffset * 4;
				g.geometry.triangles.vertexStride = 12;
				g.geometry.triangles.maxVertex = src->vertices - 1;
				g.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
				g.geometry.triangles.indexData.deviceAddress = h.geometryData.address + (VkDeviceSize)src->indexOffset * 4;
				geoms.push_back(g);
				VkAccelerationStructureBuildGeometryInfoKHR b = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
				b.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
				b.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
				b.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
				b.geometryCount = 1;
				b.pGeometries = &geoms.back();
				VkAccelerationStructureBuildSizesInfoKHR as = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
				if (okBuild) {
					h.vk.buildSizes(h.vk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &b, &v.triangles, &as);
					v.geo.size = as.accelerationStructureSize;
					v.geo.triangles = v.triangles;
					okBuild = h.structures.Allocate(h.vk, v.geo.size, v.geo.block, v.geo.offset);
					if (okBuild) {
						VkAccelerationStructureCreateInfoKHR aci = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
						aci.buffer = h.structures.blocks[v.geo.block].buffer.buffer;
						aci.offset = v.geo.offset;
						aci.size = v.geo.size;
						aci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
						if (h.vk.createAs(h.vk.device, &aci, nullptr, &v.geo.as) != VK_SUCCESS) { h.structures.Free(h.vk, v.geo.block, v.geo.offset, v.geo.size); v.geo.as = VK_NULL_HANDLE; okBuild = false; }
					}
				}
				if (!okBuild) {
					geoms.pop_back(); ommTris.pop_back(); usages.pop_back();
					FreeVariant(h, v, frameNumber);
					v.state = kSkipped; m.skipped++; m.variants[key] = v;
					continue;
				}
				VkAccelerationStructureDeviceAddressInfoKHR adi = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
				adi.accelerationStructure = v.geo.as;
				v.geo.address = h.vk.asAddress(h.vk.device, &adi);
				v.bytes += ms.micromapSize + v.geo.size;
				bi.dstMicromap = v.micromap;
				bi.data.deviceAddress = v.input.address + v.dataAt;
				bi.triangleArray.deviceAddress = v.input.address + v.triAt;
				bi.triangleArrayStride = 8;
				mmScratchAt.push_back(scratchTotal);
				scratchTotal += AlignUp(std::max<VkDeviceSize>(ms.buildScratchSize, 1), align);
				mmInfos.push_back(bi);
				b.dstAccelerationStructure = v.geo.as;
				asInfos.push_back(b);
				ranges.push_back({ v.triangles, 0, 0, 0 });
				asScratchAt.push_back(scratchTotal);
				scratchTotal += AlignUp(as.buildScratchSize, align);
			}
			m.bytes += v.bytes;
			spent += micro;
			// the job and its record copies (the instance's own record: slot, reference, material bits; the geometry's words)
			const uint32_t k = (uint32_t)made.size();
			v.job = k;
			Job& j = jobs[k];
			j = Job();
			j.geo[0] = v.triangles; j.geo[1] = v.level; j.geo[2] = src->vertexOffset; j.geo[3] = src->indexOffset;
			j.tex[0] = src->uvOffset; j.tex[1] = s; j.tex[2] = tex; j.tex[3] = cls;
			OmmMath::OmmThresholds(ref, j.thr[0], j.thr[1]);
			j.thr[2] = 1.0f / 1024.0f;	// a mixed footprint's filtered value may round below its lowest texel by this much (generous)
			const uint64_t data = v.input.address + v.dataAt, tris = v.input.address + v.triAt, index = v.input.address + v.indexAt;
			j.data[0] = (uint32_t)data; j.data[1] = (uint32_t)(data >> 32); j.data[2] = (uint32_t)tris; j.data[3] = (uint32_t)(tris >> 32);
			j.index[0] = (uint32_t)index; j.index[1] = (uint32_t)(index >> 32); j.index[2] = k * kRecords; j.index[3] = kRecords;
			j.check[0] = selfTest ? kSamplesSelfTest : kSamples; j.check[1] = (uint32_t)(frameNumber * 7919u + k * 104729u + in.geometryId);
			for (uint32_t c = 0; c < kRecords; c++) {
				NvrRtInstanceRecord& rc = recs[k * kRecords + c];
				rc = r;
				memcpy(&rc.albedo[3], &src->uvOffset, 4);
				rc.geometryId = in.geometryId;
				rc.vertexOffset = src->vertexOffset;
				rc.indexOffset = src->indexOffset;
			}
			auto& slotOf = m.variants[key];
			slotOf = v;
			made.push_back(&slotOf);
		}
		// unused for a while: freed
		for (auto it = m.variants.begin(); it != m.variants.end();) {
			if (it->second.state != kBuilding && frameNumber > it->second.lastUsed + kExpireFrames) { FreeVariant(h, it->second, frameNumber); it = m.variants.erase(it); }
			else ++it;
		}
		if (made.empty()) { PublishStats(h, makeStructures ? NVR_RT_OMM_STATE_ON : NVR_RT_OMM_STATE_SELF_TEST); return; }
		if (scratchTotal && m.scratch.size < scratchTotal + align) {
			vkQueueWaitIdle(h.vk.queue);	// rare: a frame in flight may still use the old scratch
			DestroyBuffer(h.vk, m.scratch);
			if (!CreateBuffer(h.vk, std::max<VkDeviceSize>(scratchTotal + align, 8ull << 20), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, m.scratch)) {
				Log("opacity micromaps: no build scratch (%llu bytes): these builds wait", (unsigned long long)scratchTotal);
				for (Variant* v : made) { FreeVariant(h, *v, frameNumber); v->state = kSkipped; }
				return;
			}
		}

		// 2. record: the states start at 3 (UNKNOWN), INIT + BUILD, CHECK, then the micromaps and their structures
		for (Variant* v : made) vkCmdFillBuffer(cmd, v->input.buffer, v->dataAt, v->input.size - v->dataAt, 0xFFFFFFFFu);
		FullBarrier(cmd);
		const SharedImage& atlasImage = h.images[atlas - 1];
		VkDescriptorImageInfo ii = { VK_NULL_HANDLE, atlasImage.view, VK_IMAGE_LAYOUT_GENERAL };
		VkDescriptorBufferInfo rb = { m.records[slot].buffer, 0, VK_WHOLE_SIZE }, wb = { h.geometryData.buffer, 0, VK_WHOLE_SIZE };
		VkWriteDescriptorSet w[3] = {};
		w[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m.sets[slot], 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ii };
		w[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m.sets[slot], NVR_RT_BINDING_RECORDS, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &rb };
		w[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m.sets[slot], NVR_RT_BINDING_GEOMETRY, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &wb };
		vkUpdateDescriptorSets(h.vk.device, 3, w, 0, nullptr);
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m.pipeline);
		const VkDescriptorSet sets[2] = { m.sets[slot], h.tex.sets[slot] };
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m.pipelineLayout, 0, 2, sets, 0, nullptr);
		struct { uint64_t jobs, counters; uint32_t mode, job, threads, flags; } push = {};
		push.jobs = m.jobs[slot].address;
		push.counters = m.counters[slot].address;
		push.flags = breakTest ? 1u : 0u;
		auto dispatch = [&](uint32_t mode, uint32_t job, uint64_t threads) {
			push.mode = mode; push.job = job; push.threads = (uint32_t)threads;
			vkCmdPushConstants(cmd, m.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
			vkCmdDispatch(cmd, (uint32_t)((threads + 63) / 64), 1u, 1u);
		};
		for (Variant* v : made) {
			dispatch(0u, v->job, v->triangles);
			dispatch(1u, v->job, 2ull * v->triangles << (2 * v->level));
		}
		FullBarrier(cmd);
		for (Variant* v : made) dispatch(2u, v->job, jobs[v->job].check[0]);
		FullBarrier(cmd);
		if (!mmInfos.empty()) {
			const uint64_t base = AlignUp(m.scratch.address, align);
			for (size_t i = 0; i < mmInfos.size(); i++) mmInfos[i].scratchData.deviceAddress = base + mmScratchAt[i];
			h.vk.cmdBuildMicromaps(cmd, (uint32_t)mmInfos.size(), mmInfos.data());
			FullBarrier(cmd);
			std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> rp(asInfos.size());
			for (size_t i = 0; i < asInfos.size(); i++) {
				geoms[i].geometry.triangles.pNext = &ommTris[i];
				ommTris[i].pUsageCounts = &usages[i];
				mmInfos[i].pUsageCounts = &usages[i];
				asInfos[i].pGeometries = &geoms[i];
				asInfos[i].scratchData.deviceAddress = base + asScratchAt[i];
				rp[i] = &ranges[i];
			}
			h.vk.cmdBuildAs(cmd, (uint32_t)asInfos.size(), asInfos.data(), rp.data());
			FullBarrier(cmd);
		}
		VkMemoryBarrier hb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
		hb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, nullptr, 0, nullptr);
		PublishStats(h, makeStructures ? NVR_RT_OMM_STATE_ON : NVR_RT_OMM_STATE_SELF_TEST);
		if ((frameNumber % 600) == 0)
			Log("opacity micromaps: %u ready, %zu kept, %llu MB; so far %llu built (%llu of %llu micro-triangles opaque), %llu check samples on opaque ones, "
				"%llu failed, %llu budget waits, %llu left out (too many triangles / no memory); frames: %u lod 0 only, %u any mip",
				m.ready, m.variants.size(), (unsigned long long)(m.bytes >> 20), (unsigned long long)m.built, (unsigned long long)m.opaque,
				(unsigned long long)m.micro, (unsigned long long)m.checked, (unsigned long long)m.violations, (unsigned long long)m.budgetWaits,
				(unsigned long long)m.skipped, m.framesA, m.framesB);
	}
}
