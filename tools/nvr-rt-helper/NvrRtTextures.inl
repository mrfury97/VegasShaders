// NvrRtTextures.inl -- UNOFFICIAL 2026-11-07 (protocol 7) real textures in the helper. MIT licence (tools/nvr-rt-helper/LICENSE).
//
// What the RT scene's hits looked like until now: every texture's copy in the atlases, 64 (normal, glow) to 256 (alpha) texels a texture,
// read texel by texel (imageLoad: no filtering, no mips). Up close a hit showed big square texels (a Pip-Boy in PTRealtime, a person's
// coat in a mirror); far away neighbouring rays read unrelated texels (speckle). Here the game's own textures come over once each,
// at full size (or capped by NVR), every mip level, BC-compressed as the game keeps them, and the shaders sample them with filtering
// and a mip picked by the ray's footprint (src/hlsl/NewVegas/Compute/Rt/RtTextures.glsl), as modern RT renderers do.
//
//   NVR (RtCore.cpp, RealTextures)                      helper (this file)
//   locks each level of a texture the RT scene uses,    CopyOut: before consumedFrame, the frame's texture bytes out of the "-tex"
//   copies the blocks into "<base>-tex" at the frame     block (the region of frame % RING) into a CPU buffer
//   ring slot's region, op NVR_RT_OP_TEXTURE            Record: the image made (device local, BC / BGRA8, all levels), the bytes into
//   op NVR_RT_OP_SLOT_TEXTURES: atlas slot -> ids        the slot's upload buffer, copied to it, read-only layout; its descriptor
//   op NVR_RT_OP_TEXTURE_RELEASE when it goes           written at array element id in every slot's set 1; the slot table updated
//
// Set 1 is bound to every pass next to set 0. A shader that does not declare it is unaffected; a slot whose table entry is 0 keeps the
// atlas. Ids are never reused by NVR while frames in flight may still read them (it waits 16 exchanges), so a descriptor is only ever
// written where no pending frame reads it (UPDATE_UNUSED_WHILE_PENDING).
//
// The game's route (what RtCore really does, 2026-11-07): the game's textures are in D3D9's DEFAULT pool (no CPU copy to lock), so ops
// come with bytes 0: the fork's CopyToShared has put the levels into a transfer image this exchange, and Record copies them image to
// image. Protocol 8 (rtgap-textures): one transfer image per route (NvrRtShared::textureImage's bytes): RGBA8 (decoded by the fork's
// blit) and, with fork 1.8-rt-bc, BC1 / BC3 / BC5 (the game's blocks as they are: the helper's copy is then BC too, a quarter to an
// eighth of RGBA8's memory). The "-tex" CPU route above stays (the RT test host feeds it); NVR releases textures it no longer uses
// (TEXTURE_RELEASE, least recently used first, when RealTextureBudgetMB is full) and gives their ids out again 16+ exchanges later.

namespace RealTex {

	uint32_t BlockBytes(uint32_t vkFormat) {	// bytes a 4 x 4 block (BCn), or 0: not block compressed
		switch (vkFormat) {
		case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return 8;
		case VK_FORMAT_BC2_UNORM_BLOCK: case VK_FORMAT_BC3_UNORM_BLOCK: case VK_FORMAT_BC5_UNORM_BLOCK: return 16;
		default: return 0;
		}
	}
	bool Supported(uint32_t vkFormat) {
		return BlockBytes(vkFormat) != 0 || vkFormat == VK_FORMAT_B8G8R8A8_UNORM || vkFormat == VK_FORMAT_R8G8B8A8_UNORM;
	}
	VkDeviceSize LevelBytes(uint32_t vkFormat, uint32_t w, uint32_t hgt) {
		const uint32_t b = BlockBytes(vkFormat);
		return b ? (VkDeviceSize)((w + 3) / 4) * ((hgt + 3) / 4) * b : (VkDeviceSize)w * hgt * 4;
	}
	VkDeviceSize AllBytes(uint32_t vkFormat, uint32_t w, uint32_t hgt, uint32_t levels) {
		VkDeviceSize sum = 0;
		for (uint32_t l = 0; l < levels; l++) sum += LevelBytes(vkFormat, std::max(1u, w >> l), std::max(1u, hgt >> l));
		return sum;
	}

	// Set 1's layout, pool, sets, sampler and tables. Always made (the pipeline layout has set 1 either way); without the descriptor
	// indexing features nothing is ever written and every table stays 0 (the shaders keep the atlases).
	bool Create(Helper& h) {
		RealTextures& t = h.tex;
		t.textures.resize(NVR_RT_MAX_TEXTURES);
		const bool uab = h.vk.bindless;
		VkDescriptorSetLayoutBinding b[3] = {};
		b[0] = { NVR_RT_BINDING_TEX_TABLE, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
		b[1] = { NVR_RT_BINDING_TEX_SAMPLER, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
		b[2] = { NVR_RT_BINDING_TEXTURES, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, NVR_RT_MAX_TEXTURES, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
		VkDescriptorBindingFlags flags[3] = { 0, 0, h.vk.partiallyBound ? (VkDescriptorBindingFlags)VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT : 0u };
		if (uab) flags[2] |= VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
		VkDescriptorSetLayoutBindingFlagsCreateInfo bf = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
		bf.bindingCount = 3;
		bf.pBindingFlags = flags;
		VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &bf };
		li.flags = uab ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT : 0;
		li.bindingCount = 3;
		li.pBindings = b;
		VK_CHECK(vkCreateDescriptorSetLayout(h.vk.device, &li, nullptr, &t.layout), "vkCreateDescriptorSetLayout (textures)");
		VkDescriptorPoolSize sizes[3] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kCmdRing }, { VK_DESCRIPTOR_TYPE_SAMPLER, kCmdRing },
			{ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kCmdRing * NVR_RT_MAX_TEXTURES } };
		VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
		pi.flags = uab ? VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT : 0;
		pi.maxSets = kCmdRing;
		pi.poolSizeCount = 3;
		pi.pPoolSizes = sizes;
		VK_CHECK(vkCreateDescriptorPool(h.vk.device, &pi, nullptr, &t.pool), "vkCreateDescriptorPool (textures)");
		VkDescriptorSetLayout layouts[kCmdRing];
		for (uint32_t i = 0; i < kCmdRing; i++) layouts[i] = t.layout;
		VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
		ai.descriptorPool = t.pool;
		ai.descriptorSetCount = kCmdRing;
		ai.pSetLayouts = layouts;
		VK_CHECK(vkAllocateDescriptorSets(h.vk.device, &ai, t.sets), "vkAllocateDescriptorSets (textures)");

		VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
		si.magFilter = si.minFilter = VK_FILTER_LINEAR;
		si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;	// the game's texcoords tile
		si.anisotropyEnable = h.vk.anisotropy > 1.0f ? VK_TRUE : VK_FALSE;
		si.maxAnisotropy = std::min(h.vk.anisotropy, 16.0f);	// (2026-11-07: 16, was 8: RtTextures.glsl rtTexGrads keeps the ray cone's ellipse within 16:1)
		si.maxLod = VK_LOD_CLAMP_NONE;
		VK_CHECK(vkCreateSampler(h.vk.device, &si, nullptr, &t.sampler), "vkCreateSampler (textures)");

		for (uint32_t i = 0; i < kCmdRing; i++) {
			if (!CreateBuffer(h.vk, sizeof(t.table), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, t.tables[i])) return false;
			memset(t.tables[i].mapped, 0, sizeof(t.table));
			VkDescriptorBufferInfo bi = { t.tables[i].buffer, 0, VK_WHOLE_SIZE };
			VkDescriptorImageInfo sampler = { t.sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
			VkWriteDescriptorSet w[2] = {};
			w[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, t.sets[i], NVR_RT_BINDING_TEX_TABLE, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &bi };
			w[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, t.sets[i], NVR_RT_BINDING_TEX_SAMPLER, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER, &sampler };
			vkUpdateDescriptorSets(h.vk.device, 2, w, 0, nullptr);
		}
		Log("real textures: set %u, up to %u textures, %s", NVR_RT_TEXTURE_SET, NVR_RT_MAX_TEXTURES - 1,
			uab ? "on (descriptor indexing)" : "OFF: the GPU lacks descriptor indexing (update after bind): the atlases stay");
		return true;
	}

	// The "-tex" block NVR fills (opened at start when NvrRtShared::textureStagingMB > 0) and the slots' upload buffers
	bool Open(Helper& h, const wchar_t* base, uint32_t mb) {
		RealTextures& t = h.tex;
		if (!h.vk.bindless) return true;
		for (uint32_t k = 0; k < NVR_RT_TEX_TRANSFERS; k++) {	// (protocol 8) one transfer image a route
			const uint32_t b = (g_shared->textureImage >> (8u * k)) & 0xFFu;
			if (b && b - 1 < h.imageCount)
				Log("real textures: transfer image %u (%ux%u, format %u: %s)", b - 1, h.images[b - 1].width, h.images[b - 1].height, (unsigned)h.images[b - 1].format,
					BlockBytes(h.images[b - 1].format) ? "the game's BC blocks copied as they are by the fork" : "the game's textures decoded by the fork's blit");
		}
		if (!mb) return true;
		std::wstring name = std::wstring(base) + NVR_RT_EVENT_TEX_SUFFIX;
		HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
		if (!mapping) { Log("real textures: no %ls block (error %lu): the atlases stay", name.c_str(), GetLastError()); return true; }
		const size_t size = (size_t)mb << 20;
		t.staging = (const uint8_t*)MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, size);
		if (!t.staging) { Log("real textures: the %ls block could not be mapped (error %lu)", name.c_str(), GetLastError()); return true; }
		t.stagingRegion = (uint32_t)(size / NVR_RT_FRAME_RING);
		for (uint32_t i = 0; i < kCmdRing; i++)
			if (!CreateBuffer(h.vk, t.stagingRegion, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, t.uploads[i])) { t.staging = nullptr; return true; }
		Log("real textures: staging %u MB (%u MB a frame)", mb, t.stagingRegion >> 20);
		return true;
	}

	// Before consumedFrame: this frame's texture bytes out of the shared block (NVR reuses the region once the frame is consumed)
	void CopyOut(Helper& h, uint64_t frameNumber) {
		RealTextures& t = h.tex;
		t.bytes.clear();
		t.offsets.assign(h.frame.opCount, ~0u);
		if (!t.staging) return;
		const uint8_t* region = t.staging + (size_t)(frameNumber % NVR_RT_FRAME_RING) * t.stagingRegion;
		for (uint32_t i = 0; i < h.frame.opCount; i++) {
			const NvrRtTextureOp& op = (const NvrRtTextureOp&)h.frame.ops[i];
			if (op.op != NVR_RT_OP_TEXTURE || !op.bytes || (uint64_t)op.offset + op.bytes > t.stagingRegion) continue;
			const size_t at = (t.bytes.size() + 15) & ~(size_t)15;
			if (at + op.bytes > t.stagingRegion) continue;	// (the upload buffer is a region's size)
			t.bytes.resize(at + op.bytes);
			memcpy(t.bytes.data() + at, region + op.offset, op.bytes);
			t.offsets[i] = (uint32_t)at;
		}
	}

	void Free(Helper& h, RealTexture& x) {
		if (x.view) vkDestroyImageView(h.vk.device, x.view, nullptr);
		if (x.image) vkDestroyImage(h.vk.device, x.image, nullptr);
		if (x.memory) vkFreeMemory(h.vk.device, x.memory, nullptr);
		x = RealTexture();
	}

	void ReleaseFinished(Helper& h, uint64_t done) {
		RealTextures& t = h.tex;
		auto it = std::remove_if(t.frees.begin(), t.frees.end(), [&](PendingTextureFree& p) {
			if (p.frame > done) return false;
			Free(h, p.texture);
			return true;
		});
		t.frees.erase(it, t.frees.end());
	}

	void Retire(Helper& h, uint32_t id, uint64_t frame) {
		RealTextures& t = h.tex;
		RealTexture& x = t.textures[id];
		if (!x.image) return;
		t.alive--;
		t.aliveBytes -= x.bytes;
		t.frees.push_back({ frame, x });
		x = RealTexture();
	}

	// (protocol 8) the op's level count and view swizzle
	uint32_t LevelsOf(const NvrRtTextureOp& op) { return op.levels & NVR_RT_TEX_LEVELS_MASK; }

	// (protocol 8) the transfer image a GPU-route op reads: the one of NvrRtShared::textureImage's bytes whose format is the op's (~0u: none)
	uint32_t TransferFor(const Helper& h, uint32_t vkFormat) {
		for (uint32_t k = 0; k < NVR_RT_TEX_TRANSFERS; k++) {
			const uint32_t b = (g_shared->textureImage >> (8u * k)) & 0xFFu;
			if (b && b - 1 < h.imageCount && h.images[b - 1].image && (uint32_t)h.images[b - 1].format == vkFormat) return b - 1;
		}
		return ~0u;
	}

	bool Make(Helper& h, const NvrRtTextureOp& op, RealTexture& x) {
		const uint32_t levels = LevelsOf(op);
		VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
		ii.imageType = VK_IMAGE_TYPE_2D;
		ii.format = (VkFormat)op.vkFormat;
		ii.extent = { op.width, op.height, 1 };
		ii.mipLevels = levels;
		ii.arrayLayers = 1;
		ii.samples = VK_SAMPLE_COUNT_1_BIT;
		ii.tiling = VK_IMAGE_TILING_OPTIMAL;
		ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		if (vkCreateImage(h.vk.device, &ii, nullptr, &x.image) != VK_SUCCESS) { x.image = VK_NULL_HANDLE; return false; }
		VkMemoryRequirements req = {};
		vkGetImageMemoryRequirements(h.vk.device, x.image, &req);
		int32_t type = FindMemoryType(h.vk, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		if (type < 0) type = FindMemoryType(h.vk, req.memoryTypeBits, 0);
		VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
		ai.allocationSize = req.size;
		ai.memoryTypeIndex = (uint32_t)std::max(type, 0);
		if (type < 0 || vkAllocateMemory(h.vk.device, &ai, nullptr, &x.memory) != VK_SUCCESS) { x.memory = VK_NULL_HANDLE; Free(h, x); return false; }
		if (vkBindImageMemory(h.vk.device, x.image, x.memory, 0) != VK_SUCCESS) { Free(h, x); return false; }
		VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		vi.image = x.image;
		vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vi.format = ii.format;
		vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1 };
		if (op.levels & NVR_RT_TEX_SWIZZLE_GR)	// (protocol 8) ATI2 as BC5: what DXVK's view of ATI2 gives the game's shaders
			vi.components = { VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ONE };
		if (vkCreateImageView(h.vk.device, &vi, nullptr, &x.view) != VK_SUCCESS) { x.view = VK_NULL_HANDLE; Free(h, x); return false; }
		x.bytes = req.size;
		x.width = op.width; x.height = op.height; x.levels = levels;
		return true;
	}

	// This frame's texture ops, recorded at the start of its command buffer (before the passes); then the slot table into this slot's copy
	void Record(Helper& h, uint32_t slot, VkCommandBuffer cmd, uint64_t frameNumber) {
		RealTextures& t = h.tex;
		const NvrRtFrame& f = h.frame;
		bool copied = false;
		std::vector<VkImageMemoryBarrier> toDst, toRead;
		struct Copy { VkImage image; std::vector<VkBufferImageCopy> regions; };
		std::vector<Copy> copies;
		std::vector<VkDescriptorImageInfo> infos;
		std::vector<uint32_t> infoIds;
		if (t.uploads[slot].mapped && !t.bytes.empty()) memcpy(t.uploads[slot].mapped, t.bytes.data(), t.bytes.size());
		t.gpuCopies.clear();
		for (uint32_t i = 0; i < f.opCount; i++) {
			const NvrRtTextureOp& op = (const NvrRtTextureOp&)f.ops[i];
			if (op.op == NVR_RT_OP_SLOT_TEXTURES) {
				if (op.id < NVR_RT_TEX_TABLE_SLOTS) {
					auto valid = [&](uint32_t id) { return id < NVR_RT_MAX_TEXTURES && t.textures[id].view ? id : 0u; };
					t.table[op.id][0] = valid(op.width); t.table[op.id][1] = valid(op.height); t.table[op.id][2] = valid(op.offset); t.table[op.id][3] = 0;
				}
				continue;
			}
			if (op.op == NVR_RT_OP_TEXTURE_RELEASE) {
				if (op.id && op.id < NVR_RT_MAX_TEXTURES) {
					Retire(h, op.id, frameNumber);
					for (auto& row : t.table) for (uint32_t k = 0; k < 3; k++) if (row[k] == op.id) row[k] = 0;
				}
				continue;
			}
			if (op.op != NVR_RT_OP_TEXTURE) continue;
			// bytes 0: the levels are in the transfer image of the op's format (the game's default-pool textures, copied there by the fork:
			// BC blocks as they are, protocol 8, or decoded to RGBA8)
			const uint32_t levels = LevelsOf(op);
			const bool onGpu = op.bytes == 0;
			const uint32_t xfer = onGpu ? TransferFor(h, op.vkFormat) : ~0u;
			bool gpuOk = onGpu && xfer != ~0u && levels >= 1;
			if (gpuOk) {
				const uint32_t x0 = op.offset & 0xFFFFu, y0 = op.offset >> 16, fw = op.width + (levels > 1 ? std::max(1u, op.width >> 1) : 0u);
				gpuOk = x0 + fw <= h.images[xfer].width && y0 + op.height <= h.images[xfer].height;
				if (BlockBytes(op.vkFormat)) {	// a block copy: whole blocks only (the rectangles in the transfer image and every level)
					gpuOk = gpuOk && !(x0 & 3u) && !(y0 & 3u);
					for (uint32_t l = 0; l < levels && gpuOk; l++) gpuOk = !(std::max(1u, op.width >> l) & 3u) && !(std::max(1u, op.height >> l) & 3u);
				}
			}
			const bool sane = op.id && op.id < NVR_RT_MAX_TEXTURES && Supported(op.vkFormat) && op.width && op.height && op.width <= 16384 &&
				op.height <= 16384 && levels >= 1 && levels <= 15 && !(op.levels & ~(NVR_RT_TEX_LEVELS_MASK | NVR_RT_TEX_SWIZZLE_GR)) && (onGpu ? gpuOk :
				i < t.offsets.size() && t.offsets[i] != ~0u && AllBytes(op.vkFormat, op.width, op.height, levels) == op.bytes);
			if (!sane) { t.refused++; continue; }
			Retire(h, op.id, frameNumber);	// (NVR does not resend a live id; a resend replaces it)
			RealTexture x;
			if (!Make(h, op, x)) { t.refused++; continue; }
			VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
			b.image = x.image;
			b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1 };
			b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			b.srcAccessMask = 0; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			toDst.push_back(b);
			b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			toRead.push_back(b);
			Copy c = { x.image };
			if (onGpu) {	// the levels' rectangles in the transfer image (layout in NvrRtTextureOp::bytes' comment), image to image
				const uint32_t x0 = op.offset & 0xFFFFu, y0 = op.offset >> 16;
				uint32_t below = 0;
				std::vector<VkImageCopy> rc;
				for (uint32_t l = 0; l < levels; l++) {
					const uint32_t w = std::max(1u, op.width >> l), hh = std::max(1u, op.height >> l);
					VkImageCopy r = {};
					r.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
					r.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1 };
					r.srcOffset = l == 0 ? VkOffset3D{ (int32_t)x0, (int32_t)y0, 0 } : VkOffset3D{ (int32_t)(x0 + op.width), (int32_t)(y0 + below), 0 };
					r.extent = { w, hh, 1 };
					if (l > 0) below += hh;
					rc.push_back(r);
				}
				t.gpuCopies.push_back({ h.images[xfer].image, x.image, std::move(rc) });
			}
			VkDeviceSize at = onGpu ? 0 : t.offsets[i];
			for (uint32_t l = 0; l < levels && !onGpu; l++) {
				const uint32_t w = std::max(1u, op.width >> l), hh = std::max(1u, op.height >> l);
				VkBufferImageCopy r = {};
				r.bufferOffset = at;
				r.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1 };
				r.imageExtent = { w, hh, 1 };
				c.regions.push_back(r);
				at += LevelBytes(op.vkFormat, w, hh);
			}
			copies.push_back(std::move(c));
			infos.push_back({ VK_NULL_HANDLE, x.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL });
			infoIds.push_back(op.id);
			x.serial = ++t.serials;	// (2026-11-07, ptaaa-omm) micromaps made from the old texture under this id are stale (NvrRtOmm.inl)
			t.textures[op.id] = x;
			t.alive++;
			t.aliveBytes += x.bytes;
			t.uploaded++;
			copied = true;
		}
		if (copied) {
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, (uint32_t)toDst.size(), toDst.data());
			for (const Copy& c : copies)
				if (!c.regions.empty()) vkCmdCopyBufferToImage(cmd, t.uploads[slot].buffer, c.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)c.regions.size(), c.regions.data());
			for (const auto& g : t.gpuCopies)	// (the transfer image stays GENERAL, as every shared image)
				vkCmdCopyImage(cmd, g.src, VK_IMAGE_LAYOUT_GENERAL, g.dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)g.regions.size(), g.regions.data());
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, (uint32_t)toRead.size(), toRead.data());
			std::vector<VkWriteDescriptorSet> writes;
			for (uint32_t s = 0; s < kCmdRing; s++)
				for (size_t k = 0; k < infos.size(); k++)
					writes.push_back({ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, t.sets[s], NVR_RT_BINDING_TEXTURES, infoIds[k], 1,
						VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &infos[k] });
			vkUpdateDescriptorSets(h.vk.device, (uint32_t)writes.size(), writes.data(), 0, nullptr);
			if (!t.loggedFirst) { t.loggedFirst = true; Log("real textures: the first %zu arrived (frame %llu)", infos.size(), (unsigned long long)frameNumber); }
		}
		if (t.tables[slot].mapped) memcpy(t.tables[slot].mapped, t.table, sizeof(t.table));
	}
}
