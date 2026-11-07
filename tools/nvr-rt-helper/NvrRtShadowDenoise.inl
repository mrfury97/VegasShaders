// NvrRtShadowDenoise.inl -- the RT shadow denoiser module of NvrRtHelper.exe (MIT, tools/nvr-rt-helper/LICENSE). Included by
// NvrRtHelper.cpp inside its namespace. UNOFFICIAL 2026-11-07, docs/next/RT-DENOISER.md.
//
// A game pass whose shader file is RtShadowDenoise.spv is not dispatched as one shader: the helper runs this module instead, which
// owns the denoiser's history images (they never leave the helper, so they cost no shared image) and dispatches its five stages
// (the RtShadowDenoise*.spv files next to RtShadowDenoise.spv; src/hlsl/NewVegas/Compute/Rt/RtShadowDenoise.glsl has the design):
//   temporal (Sun / Lamps variant) -> blur -> post-blur -> stabilize -> pack (Sun / Lamps)
// The pass's constant block is the shaders' (RtShadowDenoise.glsl `Pass`); its images, in order:
//   [0] depth (R32F, view z / farZ)  [1] normals (RGBA16F)  [2] object motion (RGBA16F, or any image with prevRight.w 0)
//   [3] sun: the traced visibility (R16F, written back denoised) | lamps: the per-lamp signal (RG16F, layers stacked)
//   [4] sun: the blocker distance (R16F)                         | lamps: the packed lamp result (RG32F, written back)
// Two instances (params2.x: 0 sun, 1 lamps), each with its own histories, so both can run in one frame. A helper without this module
// runs RtShadowDenoise.spv itself, which does nothing: the game then reads the raw signal.

namespace ShadowDenoise {

	enum Stage { kTemporal, kBlur, kPostBlur, kStabilize, kPack, kStages };
	// (2026-11-07) core 1 = AMD FidelityFX Shadows (RtShadowDenoiseFfx.hlsl): mask prepare, tile classification + temporal, 3 a-trous
	// passes, then our pack
	enum FfxStage { kFfxPrepare, kFfxTile, kFfxFilter0, kFfxFilter1, kFfxFilter2, kFfxPack, kFfxStages };

	struct Image {
		VkImage			image = VK_NULL_HANDLE;
		VkDeviceMemory	memory = VK_NULL_HANDLE;
		VkImageView		view = VK_NULL_HANDLE;
	};

	struct Instance {
		uint32_t	width = 0, height = 0, layers = 0;	// the traced rect's size and the layer count the images hold
		Image		hist[2], tempA, tempB, stab[2], prevDepth;
		// core 1 (FFX): the bit masks (8 x 4 pixels a texel), its tile metadata (8 x 8), moments, the temporal output, its history
		Image		ffxMask, ffxMeta, ffxMoments[2], ffxReproj, ffxHist[2];
		bool		ffxMade = false;
		uint32_t	parity = 0;
		uint64_t	runs = 0;
	};

	struct Module {
		bool		tried = false, ok = false;
		VkPipeline	pipelines[2][kStages] = {};	// [mode][stage] (blur / post-blur / stabilize are the same for both modes)
		VkDescriptorPool pool = VK_NULL_HANDLE;
		VkDescriptorSet sets[kCmdRing][2][kStages] = {};
		bool		ffxOk = false;
		VkPipeline	ffx[2][kFfxStages] = {};		// [mode][stage]
		VkDescriptorSet ffxSets[kCmdRing][2][kFfxStages] = {};
		Buffer		constants;					// host visible: NVR_RT_PASS_CONSTANTS per slot and mode
		Instance	instances[2];
		std::vector<int8_t> isModule;			// per shader index: 1 = RtShadowDenoise.spv, 0 = not, -1 = not looked at yet
	};
	static Module g_module;

	// The pass's constant block, as the shaders read it (RtShadowDenoise.glsl; the game's RtShadowDenoise.h DenoiseConstants)
	struct Constants {
		float size[4], forward[4], right[4], up[4], prevForward[4], prevRight[4], prevUp[4], cameraMove[4], jitter[4], full[4];
		float params[4], params2[4], light[4], layerSlot[2][4], layerReset[2][4], antiLag[4], history[4];
	};
	static_assert(sizeof(Constants) <= NVR_RT_PASS_CONSTANTS, "the denoiser's block fits a pass");

	bool IsModulePass(Helper& h, uint32_t shader) {
		(void)h;
		if (shader >= NVR_RT_MAX_SHADERS || shader >= g_shared->shaderCount) return false;
		if (g_module.isModule.size() <= shader) g_module.isModule.resize(shader + 1, -1);
		if (g_module.isModule[shader] < 0) {
			char path[261] = {};
			memcpy(path, g_shared->shaderPaths[shader], 260);
			const char* name = strrchr(path, '\\');
			const char* name2 = strrchr(path, '/');
			name = (name2 > name ? name2 : name);
			name = name ? name + 1 : path;
			g_module.isModule[shader] = _stricmp(name, "RtShadowDenoise.spv") == 0 ? 1 : 0;
		}
		return g_module.isModule[shader] == 1;
	}

	VkPipeline LoadStage(Helper& h, const std::string& folder, const char* file) {
		const std::string path = folder + file;
		FILE* f = nullptr;
		std::vector<uint32_t> code;
		if (fopen_s(&f, path.c_str(), "rb") == 0 && f) {
			fseek(f, 0, SEEK_END);
			long n = ftell(f);
			fseek(f, 0, SEEK_SET);
			if (n > 0 && (n % 4) == 0) { code.resize((size_t)n / 4); fread(code.data(), 1, (size_t)n, f); }
			fclose(f);
		}
		if (code.empty() || code[0] != 0x07230203u) { Status("RT shadow denoiser: cannot read %s", path.c_str()); return VK_NULL_HANDLE; }
		VkShaderModuleCreateInfo moduleInfo = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
		moduleInfo.codeSize = code.size() * 4;
		moduleInfo.pCode = code.data();
		VkShaderModule module = VK_NULL_HANDLE;
		VkPipeline pipeline = VK_NULL_HANDLE;
		if (vkCreateShaderModule(h.vk.device, &moduleInfo, nullptr, &module) == VK_SUCCESS) {
			VkComputePipelineCreateInfo info = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
			info.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
			info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
			info.stage.module = module;
			info.stage.pName = "main";
			info.layout = h.pipelineLayout;
			if (vkCreateComputePipelines(h.vk.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) != VK_SUCCESS) pipeline = VK_NULL_HANDLE;
			vkDestroyShaderModule(h.vk.device, module, nullptr);
		}
		if (!pipeline) Status("RT shadow denoiser: %s did not make a pipeline", path.c_str());
		return pipeline;
	}

	bool Init(Helper& h, uint32_t shader) {
		if (g_module.tried) return g_module.ok;
		g_module.tried = true;
		std::string folder(g_shared->shaderPaths[shader], strnlen(g_shared->shaderPaths[shader], 260));
		const size_t cut = folder.find_last_of("\\/");
		folder = cut == std::string::npos ? std::string() : folder.substr(0, cut + 1);
		static const char* files[2][kStages] = {
			{ "RtShadowDenoiseTemporalSun.spv", "RtShadowDenoiseBlur.spv", "RtShadowDenoisePostBlur.spv", "RtShadowDenoiseStabilize.spv", "RtShadowDenoisePackSun.spv" },
			{ "RtShadowDenoiseTemporalLamps.spv", "RtShadowDenoiseBlur.spv", "RtShadowDenoisePostBlur.spv", "RtShadowDenoiseStabilize.spv", "RtShadowDenoisePackLamps.spv" } };
		for (int m = 0; m < 2; m++)
			for (int st = 0; st < kStages; st++) {
				if (m == 1 && st >= kBlur && st <= kStabilize) { g_module.pipelines[1][st] = g_module.pipelines[0][st]; continue; }
				g_module.pipelines[m][st] = LoadStage(h, folder, files[m][st]);
				if (!g_module.pipelines[m][st]) return false;
			}
		// core 1 (FFX), optional: without its files only core 0 runs
		static const char* ffxFiles[2][kFfxStages] = {
			{ "RtShadowDenoiseFfxPrepareSun.spv", "RtShadowDenoiseFfxTile.spv", "RtShadowDenoiseFfxFilter0.spv", "RtShadowDenoiseFfxFilter1.spv", "RtShadowDenoiseFfxFilter2.spv", nullptr },
			{ "RtShadowDenoiseFfxPrepareLamps.spv", "RtShadowDenoiseFfxTile.spv", "RtShadowDenoiseFfxFilter0.spv", "RtShadowDenoiseFfxFilter1.spv", "RtShadowDenoiseFfxFilter2.spv", nullptr } };
		g_module.ffxOk = true;
		for (int m = 0; m < 2 && g_module.ffxOk; m++)
			for (int st = 0; st < kFfxStages && g_module.ffxOk; st++) {
				if (st == kFfxPack) { g_module.ffx[m][st] = g_module.pipelines[m][kPack]; continue; }
				if (m == 1 && st != kFfxPrepare) { g_module.ffx[1][st] = g_module.ffx[0][st]; continue; }
				g_module.ffx[m][st] = LoadStage(h, folder, ffxFiles[m][st]);
				g_module.ffxOk = g_module.ffx[m][st] != VK_NULL_HANDLE;
			}
		if (!g_module.ffxOk) Log("RT shadow denoiser: the FidelityFX core's stages did not load: core 0 only");
		const uint32_t setCount = kCmdRing * 2 * (kStages + kFfxStages);
		// (the helper's set layout also holds the scene and its buffers: the pool must cover every type, even unused. 2026-11-07: protocol 6
		// added a third storage buffer (the scratch store, binding 20) and the old "* 2" ran the pool dry: the module then never ran.
		// Sized with room for more buffers of the shared layout.)
		VkDescriptorPoolSize sizes[4] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, setCount * 2 }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, setCount * NVR_RT_PASS_IMAGES_MAX },
			{ VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, setCount * 2 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, setCount * 8 } };
		VkDescriptorPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
		poolInfo.maxSets = setCount;
		poolInfo.poolSizeCount = 4;
		poolInfo.pPoolSizes = sizes;
		if (vkCreateDescriptorPool(h.vk.device, &poolInfo, nullptr, &g_module.pool) != VK_SUCCESS) { Status("RT shadow denoiser: no descriptor pool"); return false; }
		std::vector<VkDescriptorSetLayout> layouts(setCount, h.setLayout);
		VkDescriptorSetAllocateInfo alloc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
		alloc.descriptorPool = g_module.pool;
		alloc.descriptorSetCount = setCount;
		alloc.pSetLayouts = layouts.data();
		alloc.descriptorSetCount = kCmdRing * 2 * kStages;
		if (vkAllocateDescriptorSets(h.vk.device, &alloc, &g_module.sets[0][0][0]) != VK_SUCCESS) { Status("RT shadow denoiser: no descriptor sets"); return false; }
		alloc.descriptorSetCount = kCmdRing * 2 * kFfxStages;
		if (g_module.ffxOk && vkAllocateDescriptorSets(h.vk.device, &alloc, &g_module.ffxSets[0][0][0]) != VK_SUCCESS) {	// (the A/B core only: core 0 runs on)
			Status("RT shadow denoiser: no descriptor sets for the FidelityFX core: core 0 only");
			g_module.ffxOk = false;
		}
		if (!CreateBuffer(h.vk, (VkDeviceSize)kCmdRing * 2 * NVR_RT_PASS_CONSTANTS, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, g_module.constants)) return false;
		g_module.ok = true;
		Log("RT shadow denoiser: module ready (stages from %s)", folder.c_str());
		return true;
	}

	void DestroyImage(Helper& h, Image& im) {
		if (im.view) vkDestroyImageView(h.vk.device, im.view, nullptr);
		if (im.image) vkDestroyImage(h.vk.device, im.image, nullptr);
		if (im.memory) vkFreeMemory(h.vk.device, im.memory, nullptr);
		im = Image();
	}

	bool CreateImage(Helper& h, uint32_t w, uint32_t hgt, VkFormat format, Image& out) {
		VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
		info.imageType = VK_IMAGE_TYPE_2D;
		info.format = format;
		info.extent = { w, hgt, 1 };
		info.mipLevels = 1;
		info.arrayLayers = 1;
		info.samples = VK_SAMPLE_COUNT_1_BIT;
		info.tiling = VK_IMAGE_TILING_OPTIMAL;
		info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		if (vkCreateImage(h.vk.device, &info, nullptr, &out.image) != VK_SUCCESS) return false;
		VkMemoryRequirements req = {};
		vkGetImageMemoryRequirements(h.vk.device, out.image, &req);
		int32_t type = FindMemoryType(h.vk, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		if (type < 0) type = FindMemoryType(h.vk, req.memoryTypeBits, 0);
		VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
		alloc.allocationSize = req.size;
		alloc.memoryTypeIndex = (uint32_t)(type < 0 ? 0 : type);
		if (type < 0 || vkAllocateMemory(h.vk.device, &alloc, nullptr, &out.memory) != VK_SUCCESS) return false;
		if (vkBindImageMemory(h.vk.device, out.image, out.memory, 0) != VK_SUCCESS) return false;
		VkImageViewCreateInfo view = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		view.image = out.image;
		view.viewType = VK_IMAGE_VIEW_TYPE_2D;
		view.format = format;
		view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		return vkCreateImageView(h.vk.device, &view, nullptr, &out.view) == VK_SUCCESS;
	}

	// The instance's images for this size and layer count, made (and cleared to 0: no history) when they change
	bool Ensure(Helper& h, VkCommandBuffer cmd, Instance& in, uint32_t w, uint32_t hgt, uint32_t layers, bool& fresh) {
		fresh = false;
		if (in.width == w && in.height == hgt && in.layers >= layers && in.prevDepth.view) return true;
		vkQueueWaitIdle(h.vk.queue);	// rare (resolution / layer count change): older frames may still use the old images
		Image* all[7] = { &in.hist[0], &in.hist[1], &in.tempA, &in.tempB, &in.stab[0], &in.stab[1], &in.prevDepth };
		for (Image* im : all) DestroyImage(h, *im);
		Image* ffxAll[7] = { &in.ffxMask, &in.ffxMeta, &in.ffxMoments[0], &in.ffxMoments[1], &in.ffxReproj, &in.ffxHist[0], &in.ffxHist[1] };
		for (Image* im : ffxAll) DestroyImage(h, *im);
		in.ffxMade = false;
		const uint32_t stacked = hgt * std::max(layers, 1u);
		bool ok = CreateImage(h, w, stacked, VK_FORMAT_R16G16B16A16_SFLOAT, in.hist[0]) && CreateImage(h, w, stacked, VK_FORMAT_R16G16B16A16_SFLOAT, in.hist[1]) &&
			CreateImage(h, w, stacked, VK_FORMAT_R16G16B16A16_SFLOAT, in.tempA) && CreateImage(h, w, stacked, VK_FORMAT_R16G16B16A16_SFLOAT, in.tempB) &&
			CreateImage(h, w, stacked, VK_FORMAT_R16G16_SFLOAT, in.stab[0]) && CreateImage(h, w, stacked, VK_FORMAT_R16G16_SFLOAT, in.stab[1]) &&
			CreateImage(h, w, hgt, VK_FORMAT_R32_SFLOAT, in.prevDepth);
		if (!ok) {
			for (Image* im : all) DestroyImage(h, *im);
			in.width = in.height = in.layers = 0;
			Status("RT shadow denoiser: out of GPU memory for %ux%u x %u layers", w, hgt, layers);
			return false;
		}
		std::vector<VkImageMemoryBarrier> barriers;
		for (Image* im : all) {
			VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
			b.srcAccessMask = 0;
			b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
			b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b.image = im->image;
			b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			barriers.push_back(b);
		}
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, (uint32_t)barriers.size(), barriers.data());
		VkClearColorValue zero = {};
		VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		for (Image* im : all) vkCmdClearColorImage(cmd, im->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
		FullBarrier(cmd);
		in.width = w; in.height = hgt; in.layers = std::max(layers, 1u);
		in.parity = 0;
		fresh = true;
		Log("RT shadow denoiser: %ux%u, %u layer(s): history images made (%llu MB)", w, hgt, in.layers,
			(unsigned long long)(((uint64_t)w * stacked * (4 * 8 + 2 * 4) + (uint64_t)w * hgt * 4) >> 20));
		return true;
	}

	// Core 1's images (once per size: made with the instance's, cleared to 0 = no history)
	bool EnsureFfx(Helper& h, VkCommandBuffer cmd, Instance& in) {
		if (in.ffxMade) return true;
		const uint32_t w = in.width, hgt = in.height, L = in.layers, tilesX = (w + 7) / 8;
		Image* all[7] = { &in.ffxMask, &in.ffxMeta, &in.ffxMoments[0], &in.ffxMoments[1], &in.ffxReproj, &in.ffxHist[0], &in.ffxHist[1] };
		bool ok = CreateImage(h, tilesX, ((hgt + 3) / 4) * L, VK_FORMAT_R32_UINT, in.ffxMask) && CreateImage(h, tilesX, ((hgt + 7) / 8) * L, VK_FORMAT_R32_UINT, in.ffxMeta) &&
			CreateImage(h, w, hgt * L, VK_FORMAT_R32G32B32A32_SFLOAT, in.ffxMoments[0]) && CreateImage(h, w, hgt * L, VK_FORMAT_R32G32B32A32_SFLOAT, in.ffxMoments[1]) &&
			CreateImage(h, w, hgt * L, VK_FORMAT_R16G16B16A16_SFLOAT, in.ffxReproj) && CreateImage(h, w, hgt * L, VK_FORMAT_R16G16B16A16_SFLOAT, in.ffxHist[0]) &&
			CreateImage(h, w, hgt * L, VK_FORMAT_R16G16B16A16_SFLOAT, in.ffxHist[1]);
		if (!ok) { for (Image* im : all) DestroyImage(h, *im); Status("RT shadow denoiser: out of GPU memory for the FFX core"); return false; }
		std::vector<VkImageMemoryBarrier> barriers;
		for (Image* im : all) {
			VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
			b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
			b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b.image = im->image;
			b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			barriers.push_back(b);
		}
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, (uint32_t)barriers.size(), barriers.data());
		VkClearColorValue zero = {};
		VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		for (Image* im : all) vkCmdClearColorImage(cmd, im->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
		FullBarrier(cmd);
		in.ffxMade = true;
		Log("RT shadow denoiser: FidelityFX core images made (%ux%u, %u layer(s))", w, hgt, L);
		return true;
	}

	void Dispatch(Helper& h, VkCommandBuffer cmd, VkDescriptorSet set, VkPipeline pipeline, uint32_t slot, uint32_t mode,
		const std::vector<std::pair<uint32_t, VkImageView>>& binds, uint32_t gx, uint32_t gy, uint32_t gz) {
		VkDescriptorBufferInfo ubo = { g_module.constants.buffer, ((VkDeviceSize)slot * 2 + mode) * NVR_RT_PASS_CONSTANTS, NVR_RT_PASS_CONSTANTS };
		VkDescriptorImageInfo infos[16] = {};
		VkWriteDescriptorSet writes[17] = {};
		writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &ubo };
		uint32_t n = 1;
		for (size_t i = 0; i < binds.size() && i < 16; i++, n++) {
			infos[i] = { VK_NULL_HANDLE, binds[i].second, VK_IMAGE_LAYOUT_GENERAL };
			writes[n] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, binds[i].first, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &infos[i] };
		}
		vkUpdateDescriptorSets(h.vk.device, n, writes, 0, nullptr);
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, h.pipelineLayout, 0, 1, &set, 0, nullptr);
		vkCmdDispatch(cmd, std::max(gx, 1u), std::max(gy, 1u), std::max(gz, 1u));
		FullBarrier(cmd);
	}

	// Runs the module for one game pass; false = not run (the caller logs nothing more: Status did)
	bool Run(Helper& h, uint32_t slot, VkCommandBuffer cmd, uint32_t passIndex, const NvrRtPass& pass) {
		if (!Init(h, pass.shader)) return false;
		if (pass.imageCount < 5) return false;
		for (uint32_t i = 0; i < 5; i++) if (NvrRtPassImage(pass, i) >= h.imageCount || !h.images[NvrRtPassImage(pass, i)].view) return false;
		Constants c = {};
		memcpy(&c, pass.constants, std::min<size_t>(sizeof(c), std::min<uint32_t>(pass.constantsSize, NVR_RT_PASS_CONSTANTS)));
		const uint32_t mode = c.params2[0] > 0.5f ? 1u : 0u;
		const uint32_t w = (uint32_t)std::max(c.size[0], 0.0f), hgt = (uint32_t)std::max(c.size[1], 0.0f);
		const uint32_t layers = mode ? (uint32_t)std::min(std::max(c.params2[1], 0.0f), 8.0f) : 1u;
		if (!w || !hgt || !layers) return false;
		c.params2[1] = (float)layers;
		Instance& in = g_module.instances[mode];
		bool fresh = false;
		if (!Ensure(h, cmd, in, w, hgt, layers, fresh)) return false;
		if (fresh) c.prevForward[3] = 0.0f;	// no history yet
		uint8_t* block = (uint8_t*)g_module.constants.mapped + ((size_t)slot * 2 + mode) * NVR_RT_PASS_CONSTANTS;
		memset(block, 0, NVR_RT_PASS_CONSTANTS);
		memcpy(block, &c, sizeof(c));

		const VkImageView game[5] = { h.images[NvrRtPassImage(pass, 0)].view, h.images[NvrRtPassImage(pass, 1)].view, h.images[NvrRtPassImage(pass, 2)].view,
			h.images[NvrRtPassImage(pass, 3)].view, h.images[NvrRtPassImage(pass, 4)].view };
		const VkImageView histPrev = in.hist[in.parity].view, histNext = in.hist[in.parity ^ 1].view;
		const VkImageView stabPrev = in.stab[in.parity].view, stabNext = in.stab[in.parity ^ 1].view;
		// (2026-11-07) core 1: AMD FidelityFX Shadows (history.z 1), when its stages loaded
		if (c.history[2] > 0.5f && g_module.ffxOk && EnsureFfx(h, cmd, in)) {
			const uint32_t L = layers, tilesX = (w + 7) / 8, rows = (hgt + 3) / 4;
			const VkImageView momPrev = in.ffxMoments[in.parity].view, momNext = in.ffxMoments[in.parity ^ 1].view;
			const VkImageView fhPrev = in.ffxHist[in.parity].view, fhNext = in.ffxHist[in.parity ^ 1].view;
			typedef std::vector<std::pair<uint32_t, VkImageView>> B;
			const uint32_t gx = (w + 7) / 8, gy = (hgt + 7) / 8;
			VkDescriptorSet* sets = g_module.ffxSets[slot][mode];
			VkPipeline* pipes = g_module.ffx[mode];
			// core 2 (FFX + our penumbra sizing): our temporal stage first, for the accumulated penumbra width the filter scales its steps by
			const VkImageView histPrev0 = in.hist[in.parity].view, histNext0 = in.hist[in.parity ^ 1].view;
			if (c.history[2] > 1.5f)
				Dispatch(h, cmd, g_module.sets[slot][mode][kTemporal], g_module.pipelines[mode][kTemporal], slot, mode, B{ { 2, game[0] }, { 3, game[1] }, { 4, game[2] },
					{ 5, game[3] }, { 6, game[4] }, { 12, histPrev0 }, { 13, histNext0 }, { 18, in.prevDepth.view } }, gx, (hgt * L + 7) / 8, 1);
			Dispatch(h, cmd, sets[kFfxPrepare], pipes[kFfxPrepare], slot, mode, B{ { 5, game[3] }, { 12, in.ffxMask.view } }, (tilesX + 7) / 8, (rows + 7) / 8, L);
			Dispatch(h, cmd, sets[kFfxTile], pipes[kFfxTile], slot, mode, B{ { 2, game[0] }, { 3, game[1] }, { 4, game[2] }, { 12, in.ffxMask.view }, { 13, in.ffxMeta.view },
				{ 14, momPrev }, { 15, momNext }, { 16, in.ffxReproj.view }, { 17, fhPrev }, { 18, in.prevDepth.view } }, gx, gy, L);
			Dispatch(h, cmd, sets[kFfxFilter0], pipes[kFfxFilter0], slot, mode, B{ { 2, game[0] }, { 3, game[1] }, { 13, in.ffxMeta.view }, { 14, in.ffxReproj.view },
				{ 15, in.tempA.view }, { 16, fhNext }, { 17, stabNext }, { 18, histNext0 } }, gx, gy, L);
			Dispatch(h, cmd, sets[kFfxFilter1], pipes[kFfxFilter1], slot, mode, B{ { 2, game[0] }, { 3, game[1] }, { 13, in.ffxMeta.view }, { 14, in.tempA.view },
				{ 15, in.tempB.view }, { 16, fhNext }, { 17, stabNext }, { 18, histNext0 } }, gx, gy, L);
			Dispatch(h, cmd, sets[kFfxFilter2], pipes[kFfxFilter2], slot, mode, B{ { 2, game[0] }, { 3, game[1] }, { 13, in.ffxMeta.view }, { 14, in.tempB.view },
				{ 15, in.tempA.view }, { 16, fhNext }, { 17, stabNext }, { 18, histNext0 } }, gx, gy, L);
			// our pack: 12 = the temporal output (DebugView 2), 14 = the a-trous' last input (5), 17 = the result
			Dispatch(h, cmd, sets[kFfxPack], pipes[kFfxPack], slot, mode, B{ { 2, game[0] }, { 3, game[1] }, { 5, game[3] }, { 6, game[4] }, { 12, in.ffxReproj.view },
				{ 14, in.tempB.view }, { 17, stabNext }, { 18, in.prevDepth.view } }, gx, gy, 1);
			in.parity ^= 1;
			if (in.runs++ == 0) Log("RT shadow denoiser: first run (%s, %ux%u, %u layer(s), pass %u, FidelityFX core)", mode ? "lamps" : "sun", w, hgt, layers, passIndex);
			return true;
		}
		struct Bind { uint32_t binding; VkImageView view; };
		std::vector<Bind> binds[kStages] = {
			// temporal: depth, normals, motion, signal / vis (+ hit), history in / out, last depth
			{ { 2, game[0] }, { 3, game[1] }, { 4, game[2] }, { 5, game[3] }, { 6, game[4] }, { 12, histPrev }, { 13, histNext }, { 18, in.prevDepth.view } },
			{ { 2, game[0] }, { 3, game[1] }, { 14, histNext }, { 15, in.tempA.view } },
			{ { 2, game[0] }, { 3, game[1] }, { 14, in.tempA.view }, { 15, in.tempB.view } },
			{ { 2, game[0] }, { 3, game[1] }, { 4, game[2] }, { 14, in.tempB.view }, { 16, stabPrev }, { 17, stabNext }, { 18, in.prevDepth.view } },
			{ { 2, game[0] }, { 3, game[1] }, { 5, game[3] }, { 6, game[4] }, { 12, histNext }, { 14, in.tempB.view }, { 17, stabNext }, { 18, in.prevDepth.view } } };
		const uint32_t groupsX = (w + 7) / 8, groupsLayers = (hgt * layers + 7) / 8, groupsY = (hgt + 7) / 8;
		for (int st = 0; st < kStages; st++) {
			VkDescriptorSet set = g_module.sets[slot][mode][st];
			VkDescriptorBufferInfo ubo = { g_module.constants.buffer, ((VkDeviceSize)slot * 2 + mode) * NVR_RT_PASS_CONSTANTS, NVR_RT_PASS_CONSTANTS };
			VkDescriptorImageInfo infos[16] = {};
			VkWriteDescriptorSet writes[17] = {};
			writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &ubo };
			uint32_t n = 1;
			for (size_t i = 0; i < binds[st].size(); i++, n++) {
				infos[i] = { VK_NULL_HANDLE, binds[st][i].view, VK_IMAGE_LAYOUT_GENERAL };
				writes[n] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, binds[st][i].binding, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &infos[i] };
			}
			vkUpdateDescriptorSets(h.vk.device, n, writes, 0, nullptr);
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_module.pipelines[mode][st]);
			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, h.pipelineLayout, 0, 1, &set, 0, nullptr);
			vkCmdDispatch(cmd, groupsX, st == kPack ? groupsY : groupsLayers, 1);
			FullBarrier(cmd);
		}
		in.parity ^= 1;
		if (in.runs++ == 0)
			Log("RT shadow denoiser: first run (%s, %ux%u, %u layer(s), pass %u)", mode ? "lamps" : "sun", w, hgt, layers, passIndex);
		return true;
	}

}
