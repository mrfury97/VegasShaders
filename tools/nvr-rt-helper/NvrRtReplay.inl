// NvrRtReplay.inl -- `NvrRtHelper.exe --replay <capture.nvrcap>` (UNOFFICIAL 2026-11-03, claude/rt-replay). MIT licence (LICENSE here).
// Included by NvrRtHelper.cpp inside its namespace (it uses the helper's device, buffers, scene and binding layout as they are).
//
// Why: a trace shader change (Compute/Rt/*.comp) could only be tried in a game launch on the iGPU (6-12 minutes through the bench
// queue). A capture (NvrRtCapture.h, written by the running helper) holds one frame's complete input; this runs that frame again
// with no game and no shared memory:
//   1. the Vulkan device on the first GPU with ray queries (this PC: the 9800X3D iGPU; the GTX 1070 has none and is passed over),
//   2. the geometry data (binding 11) put back at the words it had, every bottom-level structure built from it, the scene (top
//      level) from the frame's instances, the records (binding 10) as the helper bound them,
//   3. the shared images the passes bind, uploaded as the passes found them (so temporal history is as it was),
//   4. the frame's passes with their constants and dispatch sizes, each one's .spv the captured one unless --shader / --shaders
//      give another (edit a .comp, compile, replay: seconds instead of a launch),
//   5. N runs (the images reset before each), every pass timed with timestamps (median), and each image a pass writes saved as
//      PNG (to look at) and .f32 (exact floats: rt_replay_compare.py), next to the game's own output of that frame (game_*), with
//      a report (report.txt: timings against the captured frame's, the difference to the game's output per image).
//
// Usage: NvrRtHelper.exe --replay <capture> [--shader <name>=<file.spv> ...] [--shaders <folder>] [--runs N] [--out <folder>]
//        [--inputs]  (also save every uploaded image as input_<i>.png/.f32)
//   <name> is the .spv's file name without .spv as NVR registered it (RtPathTrace, RtAOTrace, RtGITrace ...); --shaders <folder>
//   takes <folder>\<name>.spv for every shader found there (e.g. src\hlsl\NewVegas\Compute\Rt after compile-compute.sh).

	struct ReplayImage {
		NvrRtCaptureImage	desc = {};
		const uint8_t*		before = nullptr;	// in the capture file
		const uint8_t*		after = nullptr;
		SharedImage			image;				// made here (only for the images a pass binds)
		VkDeviceSize		staging = 0;		// offset of its "before" contents in the upload buffer
		VkDeviceSize		readback = 0;		// offset in the read-back buffer (written images)
		bool				written = false;
	};

	struct ReplayCapture {
		std::vector<uint8_t>	file;
		NvrRtCaptureHeader		header = {};
		std::vector<ReplayImage> images;
		const NvrRtCaptureGeometry* geometries = nullptr;
		uint32_t				geometryCount = 0;
		const uint8_t*			data = nullptr;
		uint64_t				dataBytes = 0;
		const NvrRtInstance*	instances = nullptr;
		const NvrRtInstanceRecord* records = nullptr;
		uint32_t				instanceCount = 0;
		std::vector<NvrRtPass>	passes;
		std::vector<float>		passMs;			// the captured frame's own pass times (the game's run)
		std::vector<std::string> shaderPaths;
		std::vector<std::vector<uint32_t>> shaderCode;
	};

	std::string Narrow(const std::wstring& w) {
		char out[1024] = {};
		WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, out, sizeof(out) - 1, nullptr, nullptr);
		return out;
	}

	bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>& out) {
		FILE* f = nullptr;
		if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
		_fseeki64(f, 0, SEEK_END);
		const int64_t n = _ftelli64(f);
		_fseeki64(f, 0, SEEK_SET);
		out.resize(n > 0 ? (size_t)n : 0);
		const size_t got = n > 0 ? fread(out.data(), 1, (size_t)n, f) : 0;
		fclose(f);
		return got == out.size();
	}

	std::vector<uint32_t> SpirvWords(const std::vector<uint8_t>& bytes) {
		std::vector<uint32_t> words(bytes.size() / 4);
		if (!words.empty()) memcpy(words.data(), bytes.data(), words.size() * 4);
		return words;
	}

	// "C:\...\RtPathTrace.spv" -> "RtPathTrace"
	std::string ShaderName(const std::string& path) {
		size_t slash = path.find_last_of("\\/");
		std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
		size_t dot = name.find_last_of('.');
		return dot == std::string::npos ? name : name.substr(0, dot);
	}

	bool LoadCapture(const std::wstring& path, ReplayCapture& c) {
		if (!ReadWholeFile(path, c.file) || c.file.size() < sizeof(NvrRtCaptureHeader)) { Log("replay: cannot read %ls", path.c_str()); return false; }
		memcpy(&c.header, c.file.data(), sizeof(c.header));
		if (c.header.magic != NVR_RT_CAPTURE_MAGIC || c.header.version != NVR_RT_CAPTURE_VERSION || c.header.headerSize != sizeof(NvrRtCaptureHeader)) {
			Log("replay: %ls is not a capture of this version (magic 0x%08X, version %u)", path.c_str(), c.header.magic, c.header.version);
			return false;
		}
		c.images.resize(c.header.imageCount);
		c.shaderPaths.resize(c.header.shaderCount);
		c.shaderCode.resize(c.header.shaderCount);
		size_t at = sizeof(NvrRtCaptureHeader);
		while (at + sizeof(NvrRtCaptureChunk) <= c.file.size()) {
			NvrRtCaptureChunk k;
			memcpy(&k, c.file.data() + at, sizeof(k));
			const uint8_t* p = c.file.data() + at + sizeof(k);
			if (at + sizeof(k) + k.size > c.file.size()) { Log("replay: the capture is cut short (chunk at %llu)", (unsigned long long)at); return false; }
			switch (k.tag) {
				case NVR_RT_CHUNK_IMAGE_DESC: if (k.index < c.images.size()) memcpy(&c.images[k.index].desc, p, sizeof(NvrRtCaptureImage)); break;
				case NVR_RT_CHUNK_IMAGE_BEFORE: if (k.index < c.images.size()) c.images[k.index].before = p; break;
				case NVR_RT_CHUNK_IMAGE_AFTER: if (k.index < c.images.size()) c.images[k.index].after = p; break;
				case NVR_RT_CHUNK_GEOMETRIES: c.geometries = (const NvrRtCaptureGeometry*)p; c.geometryCount = (uint32_t)(k.size / sizeof(NvrRtCaptureGeometry)); break;
				case NVR_RT_CHUNK_GEOMETRY_DATA: c.data = p; c.dataBytes = k.size; break;
				case NVR_RT_CHUNK_INSTANCES: c.instances = (const NvrRtInstance*)p; c.instanceCount = (uint32_t)(k.size / sizeof(NvrRtInstance)); break;
				case NVR_RT_CHUNK_RECORDS: c.records = (const NvrRtInstanceRecord*)p; break;
				case NVR_RT_CHUNK_PASSES: c.passes.assign((const NvrRtPass*)p, (const NvrRtPass*)p + k.size / sizeof(NvrRtPass)); break;
				case NVR_RT_CHUNK_PASS_TIMES: c.passMs.assign((const float*)p, (const float*)p + k.size / sizeof(float)); break;
				case NVR_RT_CHUNK_SHADER_PATH: if (k.index < c.shaderPaths.size()) c.shaderPaths[k.index] = std::string((const char*)p, strnlen((const char*)p, (size_t)k.size)); break;
				case NVR_RT_CHUNK_SHADER_CODE: if (k.index < c.shaderCode.size()) c.shaderCode[k.index] = SpirvWords(std::vector<uint8_t>(p, p + k.size)); break;
				default: break;
			}
			at += sizeof(k) + (size_t)((k.size + 7) & ~7ull);
		}
		if (!c.geometries || !c.instances || !c.records) { Log("replay: the capture has no scene"); return false; }
		return true;
	}

	// ---- image files: .f32 (exact) and .png (to look at)

	float HalfToFloat(uint16_t h) {
		const uint32_t sign = (h & 0x8000u) << 16, exp = (h >> 10) & 0x1F, man = h & 0x3FF;
		uint32_t bits;
		if (exp == 0) {
			if (!man) bits = sign;
			else { int e = -1; uint32_t m = man; do { e++; m <<= 1; } while (!(m & 0x400)); bits = sign | ((uint32_t)(127 - 15 - e) << 23) | ((m & 0x3FF) << 13); }
		}
		else if (exp == 31) bits = sign | 0x7F800000u | (man << 13);
		else bits = sign | ((exp + 112) << 23) | (man << 13);
		float f;
		memcpy(&f, &bits, 4);
		return f;
	}

	// The texels as floats, `channels` a texel (0: a format this does not convert)
	std::vector<float> ToFloats(const NvrRtCaptureImage& d, const uint8_t* texels, uint32_t& channels) {
		const size_t n = (size_t)d.width * d.height;
		std::vector<float> out;
		channels = 0;
		switch ((VkFormat)d.vkFormat) {
			case VK_FORMAT_R32_SFLOAT: channels = 1; out.assign((const float*)texels, (const float*)texels + n); break;
			case VK_FORMAT_R32G32_SFLOAT: channels = 2; out.assign((const float*)texels, (const float*)texels + 2 * n); break;
			case VK_FORMAT_R32G32B32A32_SFLOAT: channels = 4; out.assign((const float*)texels, (const float*)texels + 4 * n); break;
			case VK_FORMAT_R32_UINT: channels = 1; out.resize(n); for (size_t i = 0; i < n; i++) out[i] = (float)((const uint32_t*)texels)[i]; break;
			case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R16G16B16A16_SFLOAT:
				channels = d.vkFormat == VK_FORMAT_R16_SFLOAT ? 1 : d.vkFormat == VK_FORMAT_R16G16_SFLOAT ? 2 : 4;
				out.resize(n * channels);
				for (size_t i = 0; i < out.size(); i++) out[i] = HalfToFloat(((const uint16_t*)texels)[i]);
				break;
			case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R16G16B16A16_UNORM:
				channels = d.vkFormat == VK_FORMAT_R16_UNORM ? 1 : d.vkFormat == VK_FORMAT_R16G16_UNORM ? 2 : 4;
				out.resize(n * channels);
				for (size_t i = 0; i < out.size(); i++) out[i] = ((const uint16_t*)texels)[i] / 65535.0f;
				break;
			case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB:
				channels = d.vkFormat == VK_FORMAT_R8_UNORM ? 1 : d.vkFormat == VK_FORMAT_R8G8_UNORM ? 2 : 4;
				out.resize(n * channels);
				for (size_t i = 0; i < out.size(); i++) out[i] = texels[i] / 255.0f;
				break;
			case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
				channels = 4;
				out.resize(n * 4);
				for (size_t i = 0; i < n; i++) {
					out[4 * i] = texels[4 * i + 2] / 255.0f; out[4 * i + 1] = texels[4 * i + 1] / 255.0f;
					out[4 * i + 2] = texels[4 * i] / 255.0f; out[4 * i + 3] = texels[4 * i + 3] / 255.0f;
				}
				break;
			case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
				channels = 4;
				out.resize(n * 4);
				for (size_t i = 0; i < n; i++) {
					const uint32_t v = ((const uint32_t*)texels)[i];
					out[4 * i] = (v & 0x3FF) / 1023.0f; out[4 * i + 1] = ((v >> 10) & 0x3FF) / 1023.0f;
					out[4 * i + 2] = ((v >> 20) & 0x3FF) / 1023.0f; out[4 * i + 3] = (v >> 30) / 3.0f;
				}
				break;
			default: break;
		}
		return out;
	}

	// .f32: "NVRF", width, height, channels (uint32 each), then width * height * channels floats, rows top to bottom
	bool WriteF32(const std::wstring& path, uint32_t w, uint32_t h, uint32_t channels, const std::vector<float>& v) {
		FILE* f = nullptr;
		if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
		const uint32_t head[4] = { 0x4652564Eu, w, h, channels };	// 'NVRF'
		fwrite(head, 4, 4, f);
		fwrite(v.data(), 4, v.size(), f);
		return fclose(f) == 0;
	}

	uint32_t Crc32(const uint8_t* p, size_t n, uint32_t crc = 0) {
		static uint32_t table[256];
		static bool made = false;
		if (!made) {
			for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; table[i] = c; }
			made = true;
		}
		crc = ~crc;
		for (size_t i = 0; i < n; i++) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
		return ~crc;
	}

	// An 8-bit RGB PNG, stored (uncompressed) deflate blocks: no zlib needed
	bool WritePng(const std::wstring& path, uint32_t w, uint32_t h, const std::vector<uint8_t>& rgb) {
		std::vector<uint8_t> raw;
		raw.reserve((size_t)(3 * w + 1) * h);
		for (uint32_t y = 0; y < h; y++) { raw.push_back(0); raw.insert(raw.end(), rgb.begin() + (size_t)y * w * 3, rgb.begin() + (size_t)(y + 1) * w * 3); }
		std::vector<uint8_t> z = { 0x78, 0x01 };
		uint32_t a = 1, b = 0;
		for (uint8_t v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
		for (size_t at = 0; at < raw.size() || at == 0; ) {
			const size_t n = std::min<size_t>(65535, raw.size() - at);
			const bool last = at + n >= raw.size();
			z.push_back(last ? 1 : 0);
			z.push_back((uint8_t)(n & 0xFF)); z.push_back((uint8_t)(n >> 8));
			z.push_back((uint8_t)(~n & 0xFF)); z.push_back((uint8_t)((~n >> 8) & 0xFF));
			z.insert(z.end(), raw.begin() + at, raw.begin() + at + n);
			at += n;
			if (last) break;
		}
		const uint32_t adler = (b << 16) | a;
		for (int s = 24; s >= 0; s -= 8) z.push_back((uint8_t)(adler >> s));
		FILE* f = nullptr;
		if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
		auto chunk = [&](const char* type, const std::vector<uint8_t>& body) {
			const uint32_t n = (uint32_t)body.size();
			const uint8_t len[4] = { (uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n };
			fwrite(len, 1, 4, f);
			std::vector<uint8_t> tb(type, type + 4);
			tb.insert(tb.end(), body.begin(), body.end());
			fwrite(tb.data(), 1, tb.size(), f);
			const uint32_t crc = Crc32(tb.data(), tb.size());
			const uint8_t c[4] = { (uint8_t)(crc >> 24), (uint8_t)(crc >> 16), (uint8_t)(crc >> 8), (uint8_t)crc };
			fwrite(c, 1, 4, f);
		};
		const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
		fwrite(sig, 1, 8, f);
		std::vector<uint8_t> ihdr = { (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w, (uint8_t)(h >> 24), (uint8_t)(h >> 16),
			(uint8_t)(h >> 8), (uint8_t)h, 8, 2, 0, 0, 0 };
		chunk("IHDR", ihdr);
		chunk("IDAT", z);
		chunk("IEND", {});
		return fclose(f) == 0;
	}

	bool IsFloatFormat(uint32_t f) {
		return f == VK_FORMAT_R32_SFLOAT || f == VK_FORMAT_R32G32_SFLOAT || f == VK_FORMAT_R32G32B32A32_SFLOAT || f == VK_FORMAT_R16_SFLOAT ||
			f == VK_FORMAT_R16G16_SFLOAT || f == VK_FORMAT_R16G16B16A16_SFLOAT || f == VK_FORMAT_R32_UINT;
	}

	// <base>.f32 and <base>.png of one image's texels. PNG: rgb (one channel: grey; two: red / green), float formats scaled by 1 / max
	// when brighter than 1 (depth, distances), then gamma 2.2; unorm formats as stored. Returns the PNG scale in `note`.
	bool SaveImage(const std::wstring& base, const NvrRtCaptureImage& d, const uint8_t* texels, std::string& note) {
		uint32_t ch = 0;
		std::vector<float> v = ToFloats(d, texels, ch);
		if (!ch) { note = "format not converted"; return false; }
		bool ok = WriteF32(base + L".f32", d.width, d.height, ch, v);
		const size_t n = (size_t)d.width * d.height;
		const bool isFloat = IsFloatFormat(d.vkFormat);
		float maxV = 0.0f;
		for (size_t i = 0; i < n; i++) for (uint32_t k = 0; k < std::min(ch, 3u); k++) { const float x = v[i * ch + k]; if (x == x && x < 3e38f) maxV = std::max(maxV, x); }
		const float scale = isFloat && maxV > 1.0f ? 1.0f / maxV : 1.0f;
		std::vector<uint8_t> rgb(n * 3);
		for (size_t i = 0; i < n; i++)
			for (uint32_t k = 0; k < 3; k++) {
				float x = ch == 1 ? v[i] : k < ch ? v[i * ch + k] : 0.0f;
				x = x == x ? std::min(std::max(x * scale, 0.0f), 1.0f) : 0.0f;
				if (isFloat) x = powf(x, 1.0f / 2.2f);
				rgb[i * 3 + k] = (uint8_t)(x * 255.0f + 0.5f);
			}
		ok = WritePng(base + L".png", d.width, d.height, rgb) && ok;
		char buf[96];
		sprintf_s(buf, "%u channel%s, max %.4g%s", ch, ch > 1 ? "s" : "", maxV, scale != 1.0f ? " (png scaled by 1/max)" : "");
		note = buf;
		return ok;
	}

	struct DiffStats { double meanAbs = 0, maxAbs = 0; uint64_t over = 0, nanMismatch = 0, values = 0; };

	DiffStats Difference(const NvrRtCaptureImage& d, const uint8_t* a, const uint8_t* b) {
		DiffStats s;
		uint32_t ca = 0, cb = 0;
		const std::vector<float> va = ToFloats(d, a, ca), vb = ToFloats(d, b, cb);
		if (!ca || va.size() != vb.size()) return s;
		double sum = 0;
		for (size_t i = 0; i < va.size(); i++) {
			const bool na = va[i] != va[i], nb = vb[i] != vb[i];
			if (na || nb) { if (na != nb) s.nanMismatch++; continue; }
			const double e = fabs((double)va[i] - vb[i]);
			sum += e;
			s.maxAbs = std::max(s.maxAbs, e);
			if (e > 1e-3) s.over++;
			s.values++;
		}
		s.meanAbs = s.values ? sum / s.values : 0;
		return s;
	}

	// ---- the replay

	bool ReplaySubmit(Helper& h, VkCommandBuffer cmd) {
		VK_CHECK(vkEndCommandBuffer(cmd), "vkEndCommandBuffer (replay)");
		VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &cmd;
		VK_CHECK(vkQueueSubmit(h.vk.queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit (replay)");
		VK_CHECK(vkQueueWaitIdle(h.vk.queue), "vkQueueWaitIdle (replay)");
		return true;
	}

	bool ReplayBegin(VkCommandBuffer cmd) {
		vkResetCommandBuffer(cmd, 0);
		VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		VK_CHECK(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer (replay)");
		return true;
	}

	double Seconds() {
		LARGE_INTEGER now, frequency;
		QueryPerformanceCounter(&now);
		QueryPerformanceFrequency(&frequency);
		return (double)now.QuadPart / frequency.QuadPart;
	}

	bool CreateReplayImage(Helper& h, const NvrRtCaptureImage& d, SharedImage& out) {
		VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
		info.imageType = VK_IMAGE_TYPE_2D;
		info.format = (VkFormat)d.vkFormat;
		info.extent = { d.width, d.height, 1 };
		info.mipLevels = 1;
		info.arrayLayers = 1;
		info.samples = VK_SAMPLE_COUNT_1_BIT;
		info.tiling = VK_IMAGE_TILING_OPTIMAL;
		info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		VK_CHECK(vkCreateImage(h.vk.device, &info, nullptr, &out.image), "vkCreateImage (replay)");
		VkMemoryRequirements req = {};
		vkGetImageMemoryRequirements(h.vk.device, out.image, &req);
		int32_t type = FindMemoryType(h.vk, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		if (type < 0) type = FindMemoryType(h.vk, req.memoryTypeBits, 0);
		VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
		alloc.allocationSize = req.size;
		alloc.memoryTypeIndex = (uint32_t)std::max(type, 0);
		VK_CHECK(vkAllocateMemory(h.vk.device, &alloc, nullptr, &out.memory), "vkAllocateMemory (replay image)");
		VK_CHECK(vkBindImageMemory(h.vk.device, out.image, out.memory, 0), "vkBindImageMemory (replay)");
		VkImageViewCreateInfo view = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		view.image = out.image;
		view.viewType = VK_IMAGE_VIEW_TYPE_2D;
		view.format = (VkFormat)d.vkFormat;
		view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		VK_CHECK(vkCreateImageView(h.vk.device, &view, nullptr, &out.view), "vkCreateImageView (replay)");
		out.format = (VkFormat)d.vkFormat;
		out.width = d.width;
		out.height = d.height;
		return true;
	}

	VkPipeline CreateReplayPipeline(Helper& h, const std::vector<uint32_t>& code) {
		if (code.empty() || code[0] != 0x07230203u) return VK_NULL_HANDLE;
		VkShaderModuleCreateInfo moduleInfo = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
		moduleInfo.codeSize = code.size() * 4;
		moduleInfo.pCode = code.data();
		VkShaderModule module = VK_NULL_HANDLE;
		if (vkCreateShaderModule(h.vk.device, &moduleInfo, nullptr, &module) != VK_SUCCESS) return VK_NULL_HANDLE;
		VkComputePipelineCreateInfo info = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
		info.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
		info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
		info.stage.module = module;
		info.stage.pName = "main";
		info.layout = h.pipelineLayout;
		VkPipeline pipeline = VK_NULL_HANDLE;
		if (vkCreateComputePipelines(h.vk.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) != VK_SUCCESS) pipeline = VK_NULL_HANDLE;
		vkDestroyShaderModule(h.vk.device, module, nullptr);
		return pipeline;
	}

	int RunReplay(int argc, wchar_t** argv) {
		g_console = true;
		const double t0 = Seconds();
		std::wstring capturePath, outDir, shaderDir;
		std::vector<std::pair<std::string, std::wstring>> overrides;
		uint32_t runs = 5;
		bool dumpInputs = false;
		for (int i = 1; i < argc; i++) {
			if (!wcscmp(argv[i], L"--replay") && i + 1 < argc) capturePath = argv[++i];
			else if (!wcscmp(argv[i], L"--out") && i + 1 < argc) outDir = argv[++i];
			else if (!wcscmp(argv[i], L"--runs") && i + 1 < argc) runs = std::max(1u, (uint32_t)_wtoi(argv[++i]));
			else if (!wcscmp(argv[i], L"--shaders") && i + 1 < argc) shaderDir = argv[++i];
			else if (!wcscmp(argv[i], L"--inputs")) dumpInputs = true;
			else if (!wcscmp(argv[i], L"--shader") && i + 1 < argc) {
				const std::wstring a = argv[++i];
				const size_t eq = a.find(L'=');
				if (eq == std::wstring::npos) { printf("--shader wants <name>=<file.spv>\n"); return 1; }
				std::string name = Narrow(a.substr(0, eq));
				std::transform(name.begin(), name.end(), name.begin(), ::tolower);
				overrides.push_back({ name, a.substr(eq + 1) });
			}
		}
		if (capturePath.empty()) { printf("usage: NvrRtHelper.exe --replay <capture.nvrcap> [--shader <name>=<file.spv> ...] [--shaders <folder>] [--runs N] [--out <folder>] [--inputs]\n"); return 1; }
		if (outDir.empty()) {
			outDir = capturePath;
			const size_t dot = outDir.find_last_of(L'.');
			if (dot != std::wstring::npos && dot > outDir.find_last_of(L"\\/") + 1) outDir.resize(dot);
			outDir += L"-replay";
		}
		CreateDirectoryW(outDir.c_str(), nullptr);
		_wfopen_s(&g_log, (outDir + L"\\replay.log").c_str(), L"w");

		ReplayCapture c;
		if (!LoadCapture(capturePath, c)) return 2;
		const double tLoaded = Seconds();
		Log("replay of %ls: frame %llu on %s; %u images, %u geometries (%u without data), %u instances, %u passes, %llu MB",
			capturePath.c_str(), (unsigned long long)c.header.frame, c.header.device, c.header.imageCount, c.geometryCount, c.header.noDataGeometries,
			c.instanceCount, (uint32_t)c.passes.size(), (unsigned long long)(c.file.size() >> 20));

		Helper* hp = new Helper();
		Helper& h = *hp;
		if (!CreateVulkanDevice(h.vk, nullptr, false, nullptr)) { Log("replay: no GPU with ray queries"); return 3; }
		Log("replay device: %s%s", h.vk.properties.deviceName, strcmp(h.vk.properties.deviceName, c.header.device) ? " (NOT the captured frame's GPU: times differ)" : "");
		VkCommandPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
		poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		poolInfo.queueFamilyIndex = h.vk.queueFamily;
		if (vkCreateCommandPool(h.vk.device, &poolInfo, nullptr, &h.pool) != VK_SUCCESS) return 3;
		Slot& s = h.slots[0];
		VkCommandBufferAllocateInfo cmdInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
		cmdInfo.commandPool = h.pool;
		cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cmdInfo.commandBufferCount = 1;
		if (vkAllocateCommandBuffers(h.vk.device, &cmdInfo, &s.cmd) != VK_SUCCESS) return 3;
		const uint32_t instanceCount = std::min<uint32_t>(c.instanceCount, NVR_RT_MAX_INSTANCES);
		if (!CreateBuffer(h.vk, sizeof(VkAccelerationStructureInstanceKHR) * NVR_RT_MAX_INSTANCES, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true, s.instances) ||
			!CreateBuffer(h.vk, (VkDeviceSize)NVR_RT_PASS_CONSTANTS * NVR_RT_MAX_PASSES, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, s.constants) ||
			!CreateBuffer(h.vk, sizeof(NvrRtInstanceRecord) * NVR_RT_MAX_INSTANCES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, s.records) ||
			!CreateTlas(h) || !EnsureScratch(h, h.tlasScratch) || !CreatePipelineLayout(h) ||
			!CreateBuffer(h.vk, 256ull << 20, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, h.scratchStore))	// (2026-11-07, protocol 6)
			return 3;
		VkCommandBuffer cmd = s.cmd;

		// 1. the geometry data at the words it had (+ 12 words at the end: one empty triangle for geometries that had no data)
		uint64_t words = 0;
		for (uint32_t g = 0; g < c.geometryCount; g++)
			if (c.geometries[g].dataWords) words = std::max<uint64_t>(words, (uint64_t)c.geometries[g].vertexOffset + c.geometries[g].dataWords);
		const uint32_t emptyAt = (uint32_t)words;
		words += 12;
		Buffer upload;
		if (!CreateBuffer(h.vk, words * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, upload) ||
			!CreateBuffer(h.vk, words * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
				VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, false, h.geometryData)) return 3;
		memset(upload.mapped, 0, (size_t)words * 4);
		{
			uint64_t at = 0;
			for (uint32_t g = 0; g < c.geometryCount; g++) {
				const NvrRtCaptureGeometry& geo = c.geometries[g];
				if (!geo.dataWords) continue;
				if (at + (uint64_t)geo.dataWords * 4 > c.dataBytes) { Log("replay: the geometry data is shorter than its table"); return 2; }
				memcpy((uint8_t*)upload.mapped + (size_t)geo.vertexOffset * 4, c.data + at, (size_t)geo.dataWords * 4);
				at += (uint64_t)geo.dataWords * 4;
			}
			uint32_t* tail = (uint32_t*)upload.mapped + emptyAt;	// three equal vertices (0, 0, 0) and the triangle 0 1 2: hits nothing
			tail[9] = 0; tail[10] = 1; tail[11] = 2;
		}
		if (!ReplayBegin(cmd)) return 3;
		VkBufferCopy whole = { 0, 0, words * 4 };
		vkCmdCopyBuffer(cmd, upload.buffer, h.geometryData.buffer, 1, &whole);
		FullBarrier(cmd);
		if (!ReplaySubmit(h, cmd)) return 3;
		DestroyBuffer(h.vk, upload);

		// 2. every bottom level, as the helper builds them (static: fast trace; dynamic: fast build + update), in batches of <= 256 MB scratch
		const double tBuild0 = Seconds();
		const VkDeviceSize align = h.vk.asProperties.minAccelerationStructureScratchOffsetAlignment;
		std::unordered_map<uint32_t, uint64_t> addresses;
		Buffer structureStorage;	// every bottom level of the replay, one after the other
		std::vector<VkAccelerationStructureKHR> structures;
		uint64_t triangles = 0, structureBytes = 0;
		{
			struct Build { VkAccelerationStructureGeometryKHR g; VkAccelerationStructureBuildGeometryInfoKHR b; VkAccelerationStructureBuildRangeInfoKHR range; VkDeviceSize scratch, size; };
			std::vector<Build> builds(c.geometryCount);
			VkDeviceSize storage = 0;
			std::vector<VkDeviceSize> offsets(c.geometryCount);
			for (uint32_t i = 0; i < c.geometryCount; i++) {
				const NvrRtCaptureGeometry& geo = c.geometries[i];
				const bool has = geo.dataWords != 0;
				Build& bd = builds[i];
				bd.g = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
				bd.g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
				bd.g.flags = (geo.opFlags & NVR_RT_OP_FLAG_ALPHA) ? 0u : VK_GEOMETRY_OPAQUE_BIT_KHR;
				bd.g.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
				bd.g.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
				bd.g.geometry.triangles.vertexData.deviceAddress = h.geometryData.address + (VkDeviceSize)(has ? geo.vertexOffset : emptyAt) * 4;
				bd.g.geometry.triangles.vertexStride = 12;
				bd.g.geometry.triangles.maxVertex = has ? geo.vertices - 1 : 2;
				bd.g.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
				bd.g.geometry.triangles.indexData.deviceAddress = h.geometryData.address + (VkDeviceSize)(has ? geo.indexOffset : emptyAt + 9) * 4;
				const bool dynamic = (geo.opFlags & NVR_RT_OP_FLAG_DYNAMIC) != 0;
				bd.b = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
				bd.b.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
				bd.b.flags = dynamic ? (VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR)
					: VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
				bd.b.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
				bd.b.geometryCount = 1;
				const uint32_t tris = has ? geo.triangles : 1;
				bd.range = { tris, 0, 0, 0 };
				bd.b.pGeometries = &bd.g;
				VkAccelerationStructureBuildSizesInfoKHR sizes = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
				h.vk.buildSizes(h.vk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bd.b, &tris, &sizes);
				bd.size = sizes.accelerationStructureSize;
				bd.scratch = AlignUp(sizes.buildScratchSize, align);
				offsets[i] = storage;
				storage += AlignUp(bd.size, 256);
				triangles += tris;
			}
			if (!CreateBuffer(h.vk, std::max<VkDeviceSize>(storage, 256), VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false, structureStorage)) return 3;
			structureBytes = storage;
			structures.resize(c.geometryCount);
			for (uint32_t i = 0; i < c.geometryCount; i++) {
				VkAccelerationStructureCreateInfoKHR info = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
				info.buffer = structureStorage.buffer;
				info.offset = offsets[i];
				info.size = builds[i].size;
				info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
				if (h.vk.createAs(h.vk.device, &info, nullptr, &structures[i]) != VK_SUCCESS) { Log("replay: vkCreateAccelerationStructureKHR failed"); return 3; }
				VkAccelerationStructureDeviceAddressInfoKHR addressInfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
				addressInfo.accelerationStructure = structures[i];
				addresses[c.geometries[i].id] = h.vk.asAddress(h.vk.device, &addressInfo);
				builds[i].b.dstAccelerationStructure = structures[i];
			}
			const VkDeviceSize batchScratch = 256ull << 20;
			for (uint32_t first = 0; first < c.geometryCount; ) {
				VkDeviceSize used = 0;
				uint32_t last = first;
				while (last < c.geometryCount && (last == first || used + builds[last].scratch <= batchScratch)) used += builds[last++].scratch;
				if (!EnsureScratch(h, used)) return 3;
				const uint64_t base = AlignUp(h.scratch.address, align);
				std::vector<VkAccelerationStructureBuildGeometryInfoKHR> infos;
				std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> ranges;
				VkDeviceSize at = 0;
				for (uint32_t i = first; i < last; i++) {
					builds[i].b.pGeometries = &builds[i].g;
					builds[i].b.scratchData.deviceAddress = base + at;
					at += builds[i].scratch;
					infos.push_back(builds[i].b);
					ranges.push_back(&builds[i].range);
				}
				if (!ReplayBegin(cmd)) return 3;
				h.vk.cmdBuildAs(cmd, (uint32_t)infos.size(), infos.data(), ranges.data());
				FullBarrier(cmd);
				if (!ReplaySubmit(h, cmd)) return 3;
				first = last;
			}
		}
		const double tBuild1 = Seconds();
		Log("replay: %u bottom levels built (%llu triangles, %llu MB) in %.0f ms", c.geometryCount, (unsigned long long)triangles,
			(unsigned long long)(structureBytes >> 20), (tBuild1 - tBuild0) * 1000.0);

		// 3. the scene's instances and records as the helper had them
		uint32_t missing = 0;
		{
			VkAccelerationStructureInstanceKHR* out = (VkAccelerationStructureInstanceKHR*)s.instances.mapped;
			for (uint32_t i = 0; i < instanceCount; i++) {
				const NvrRtInstance& in = c.instances[i];
				VkAccelerationStructureInstanceKHR& o = out[i];
				memcpy(&o.transform, in.transform, sizeof(o.transform));
				o.instanceCustomIndex = in.customIndex & 0xFFFFFF;
				o.mask = in.mask & 0xFF;
				o.instanceShaderBindingTableRecordOffset = 0;
				o.flags = in.flags & 0xFF;
				auto it = addresses.find(in.geometryId);
				if (it == addresses.end()) { missing++; o.mask = 0; o.accelerationStructureReference = addresses.empty() ? 0 : addresses.begin()->second; }
				else o.accelerationStructureReference = it->second;
			}
			memcpy(s.records.mapped, c.records, (size_t)instanceCount * sizeof(NvrRtInstanceRecord));
		}
		if (missing) Log("replay: %u instances name a geometry the capture does not hold (masked out)", missing);

		// 4. the images the passes bind, with their contents before the passes
		std::vector<uint32_t> bound(c.images.size(), 0);
		for (const NvrRtPass& pass : c.passes)
			for (uint32_t i = 0; i < std::min<uint32_t>(pass.imageCount, NVR_RT_PASS_IMAGES_MAX); i++) {
				const uint32_t im = NvrRtPassImage(pass, i);
				if (im >= c.images.size()) continue;
				bound[im] = 1;
				if (pass.writeMask & (1u << i)) c.images[im].written = true;
			}
		VkDeviceSize uploadBytes = 0, readbackBytes = 0;
		for (uint32_t i = 0; i < c.images.size(); i++) {
			ReplayImage& im = c.images[i];
			if (!bound[i] && !(dumpInputs && im.before)) continue;
			const VkDeviceSize size = (VkDeviceSize)im.desc.width * im.desc.height * im.desc.texelBytes;
			if (im.before) { im.staging = uploadBytes; uploadBytes += AlignUp(size, 256); }
			if (im.written) { im.readback = readbackBytes; readbackBytes += AlignUp(size, 256); }
			if (bound[i]) {
				if (!im.desc.texelBytes) { Log("replay: image %u (format %u) is bound but its format is not known: the passes see zeros", i, im.desc.vkFormat); }
				if (!CreateReplayImage(h, im.desc, im.image)) return 3;
			}
		}
		Buffer imageUpload, imageReadback;
		if (!CreateBuffer(h.vk, std::max<VkDeviceSize>(uploadBytes, 256), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, imageUpload) ||
			!CreateBuffer(h.vk, std::max<VkDeviceSize>(readbackBytes, 256), VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, imageReadback)) return 3;
		for (ReplayImage& im : c.images)
			if (im.before && (im.image.image || dumpInputs)) memcpy((uint8_t*)imageUpload.mapped + im.staging, im.before, (size_t)im.desc.width * im.desc.height * im.desc.texelBytes);
		{
			if (!ReplayBegin(cmd)) return 3;
			std::vector<VkImageMemoryBarrier> barriers;
			for (ReplayImage& im : c.images) {
				if (!im.image.image) continue;
				VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
				b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
				b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
				b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
				b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				b.image = im.image.image;
				b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
				barriers.push_back(b);
			}
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, (uint32_t)barriers.size(), barriers.data());
			const VkClearColorValue zero = {};
			const VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			for (ReplayImage& im : c.images) if (im.image.image && !im.before) vkCmdClearColorImage(cmd, im.image.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
			if (!ReplaySubmit(h, cmd)) return 3;
		}

		// 5. the pipelines: --shader, else --shaders <folder>\<name>.spv, else the captured SPIR-V
		std::vector<VkPipeline> pipelines(c.shaderPaths.size(), VK_NULL_HANDLE);
		std::vector<std::string> shaderSource(c.shaderPaths.size());
		std::vector<uint32_t> shaderUsed(c.shaderPaths.size(), 0);
		for (const NvrRtPass& pass : c.passes) if (pass.shader < shaderUsed.size()) shaderUsed[pass.shader] = 1;
		for (uint32_t i = 0; i < c.shaderPaths.size(); i++) {
			if (!shaderUsed[i]) continue;
			const std::string name = ShaderName(c.shaderPaths[i]);
			std::string lower = name;
			std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
			std::vector<uint32_t> code;
			for (const auto& o : overrides) {
				if (o.first != lower) continue;
				std::vector<uint8_t> bytes;
				if (!ReadWholeFile(o.second, bytes)) { Log("replay: cannot read %ls (--shader %s)", o.second.c_str(), name.c_str()); return 2; }
				code = SpirvWords(bytes);
				shaderSource[i] = Narrow(o.second);
			}
			if (code.empty() && !shaderDir.empty()) {
				std::vector<uint8_t> bytes;
				const std::wstring p = shaderDir + L"\\" + std::wstring(name.begin(), name.end()) + L".spv";
				if (ReadWholeFile(p, bytes)) { code = SpirvWords(bytes); shaderSource[i] = Narrow(p); }
			}
			if (code.empty()) { code = c.shaderCode[i]; shaderSource[i] = "captured (" + c.shaderPaths[i] + ")"; }
			pipelines[i] = CreateReplayPipeline(h, code);
			Log("shader %u %s: %s%s", i, name.c_str(), shaderSource[i].c_str(), pipelines[i] ? "" : "  <- FAILED (not SPIR-V or no pipeline): its passes are skipped");
		}
		for (uint32_t i = 0; i < overrides.size(); i++) {
			bool used = false;
			for (uint32_t k = 0; k < c.shaderPaths.size(); k++) { std::string n = ShaderName(c.shaderPaths[k]); std::transform(n.begin(), n.end(), n.begin(), ::tolower); used = used || (shaderUsed[k] && n == overrides[i].first); }
			if (!used) Log("replay: --shader %s matches no pass of this frame", overrides[i].first.c_str());
		}

		// 6. the descriptor sets and constants, as RecordFrame binds them
		const uint32_t passCount = std::min<uint32_t>((uint32_t)c.passes.size(), NVR_RT_MAX_PASSES);
		std::vector<bool> runsPass(passCount, false);
		for (uint32_t p = 0; p < passCount; p++) {
			const NvrRtPass& pass = c.passes[p];
			if (pass.shader >= pipelines.size() || !pipelines[pass.shader] || pass.imageCount > NVR_RT_PASS_IMAGES_MAX) continue;
			bool imagesOk = true;
			for (uint32_t i = 0; i < pass.imageCount; i++) imagesOk = imagesOk && NvrRtPassImage(pass, i) < c.images.size() && c.images[NvrRtPassImage(pass, i)].image.view;
			if (!imagesOk) { Log("pass %u: an image is missing: skipped", p); continue; }
			runsPass[p] = true;
			memcpy((uint8_t*)s.constants.mapped + (size_t)p * NVR_RT_PASS_CONSTANTS, pass.constants, std::min<uint32_t>(pass.constantsSize, NVR_RT_PASS_CONSTANTS));
			VkWriteDescriptorSetAccelerationStructureKHR asWrite = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR };
			asWrite.accelerationStructureCount = 1;
			asWrite.pAccelerationStructures = &h.tlas;
			VkDescriptorBufferInfo ubo = { s.constants.buffer, (VkDeviceSize)p * NVR_RT_PASS_CONSTANTS, NVR_RT_PASS_CONSTANTS };
			VkDescriptorImageInfo imageInfos[NVR_RT_PASS_IMAGES_MAX] = {};
			VkWriteDescriptorSet writes[2 + NVR_RT_PASS_IMAGES_MAX + 3] = {};
			VkDescriptorBufferInfo recordInfo = { s.records.buffer, 0, VK_WHOLE_SIZE };
			VkDescriptorBufferInfo dataInfo = { h.geometryData.buffer, 0, VK_WHOLE_SIZE };
			VkDescriptorBufferInfo scratchInfo = { h.scratchStore.buffer, 0, VK_WHOLE_SIZE };	// (2026-11-07, protocol 6; a capture holds no scratch: zero)
			writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, &asWrite, s.sets[p], 0, 0, 1, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR };
			writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, s.sets[p], 1, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &ubo };
			for (uint32_t i = 0; i < pass.imageCount; i++) {
				imageInfos[i] = { VK_NULL_HANDLE, c.images[NvrRtPassImage(pass, i)].image.view, VK_IMAGE_LAYOUT_GENERAL };
				writes[2 + i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, s.sets[p], NvrRtPassBinding(i), 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &imageInfos[i] };
			}
			writes[2 + pass.imageCount] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, s.sets[p], NVR_RT_BINDING_RECORDS, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &recordInfo };
			writes[3 + pass.imageCount] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, s.sets[p], NVR_RT_BINDING_GEOMETRY, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &dataInfo };
			writes[4 + pass.imageCount] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, s.sets[p], NVR_RT_BINDING_SCRATCH, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &scratchInfo };
			vkUpdateDescriptorSets(h.vk.device, 5 + pass.imageCount, writes, 0, nullptr);
		}

		// 7. the runs: images reset to the captured contents, the scene built, the passes, each timed
		const uint32_t stampsPerRun = 2 + passCount;
		VkQueryPoolCreateInfo queryInfo = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
		queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
		queryInfo.queryCount = stampsPerRun;
		if (!h.vk.timestampBits || vkCreateQueryPool(h.vk.device, &queryInfo, nullptr, &h.queries) != VK_SUCCESS) h.queries = VK_NULL_HANDLE;
		std::vector<std::vector<double>> passTimes(passCount);
		std::vector<double> sceneTimes, wallTimes;
		const double tRuns0 = Seconds();
		for (uint32_t run = 0; run < runs; run++) {
			const double w0 = Seconds();
			if (!ReplayBegin(cmd)) return 3;
			for (ReplayImage& im : c.images) {
				if (!im.image.image || !im.before) continue;
				VkBufferImageCopy copy = {};
				copy.bufferOffset = im.staging;
				copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
				copy.imageExtent = { im.desc.width, im.desc.height, 1 };
				vkCmdCopyBufferToImage(cmd, imageUpload.buffer, im.image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
			}
			vkCmdFillBuffer(cmd, h.scratchStore.buffer, 0, VK_WHOLE_SIZE, 0u);	// (2026-11-07) every run from an empty scratch store: runs stay alike
			FullBarrier(cmd);
			if (h.queries) { vkCmdResetQueryPool(cmd, h.queries, 0, stampsPerRun); vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, h.queries, 0); }
			{
				VkAccelerationStructureGeometryKHR g = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
				g.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
				g.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
				g.geometry.instances.data.deviceAddress = s.instances.address;
				VkAccelerationStructureBuildGeometryInfoKHR b = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
				b.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
				b.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
				b.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
				b.dstAccelerationStructure = h.tlas;
				b.geometryCount = 1;
				b.pGeometries = &g;
				b.scratchData.deviceAddress = AlignUp(h.scratch.address, align);
				VkAccelerationStructureBuildRangeInfoKHR range = { instanceCount, 0, 0, 0 };
				const VkAccelerationStructureBuildRangeInfoKHR* rangePointer = &range;
				h.vk.cmdBuildAs(cmd, 1, &b, &rangePointer);
				FullBarrier(cmd);
			}
			if (h.queries) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, h.queries, 1);
			for (uint32_t p = 0; p < passCount; p++) {
				const NvrRtPass& pass = c.passes[p];
				if (runsPass[p]) {
					vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[pass.shader]);
					vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, h.pipelineLayout, 0, 1, &s.sets[p], 0, nullptr);
					vkCmdDispatch(cmd, std::max(1u, pass.groupsX), std::max(1u, pass.groupsY), std::max(1u, pass.groupsZ));
					FullBarrier(cmd);
				}
				if (h.queries) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, h.queries, 2 + p);
			}
			if (run + 1 == runs) {	// the written images out (after the last run)
				for (ReplayImage& im : c.images) {
					if (!im.image.image || !im.written || !im.desc.texelBytes) continue;
					VkBufferImageCopy copy = {};
					copy.bufferOffset = im.readback;
					copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
					copy.imageExtent = { im.desc.width, im.desc.height, 1 };
					vkCmdCopyImageToBuffer(cmd, im.image.image, VK_IMAGE_LAYOUT_GENERAL, imageReadback.buffer, 1, &copy);
				}
				VkMemoryBarrier b = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
				b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
				b.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
				vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
			}
			if (!ReplaySubmit(h, cmd)) return 3;
			wallTimes.push_back((Seconds() - w0) * 1000.0);
			if (h.queries) {
				std::vector<uint64_t> stamps(stampsPerRun);
				if (vkGetQueryPoolResults(h.vk.device, h.queries, 0, stampsPerRun, stamps.size() * 8, stamps.data(), 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
					const uint64_t mask = h.vk.timestampBits >= 64 ? ~0ull : ((1ull << h.vk.timestampBits) - 1);
					auto ms = [&](uint32_t a, uint32_t b) { return (double)((stamps[b] - stamps[a]) & mask) * h.vk.properties.limits.timestampPeriod / 1e6; };
					sceneTimes.push_back(ms(0, 1));
					for (uint32_t p = 0; p < passCount; p++) passTimes[p].push_back(ms(1 + p, 2 + p));
				}
			}
		}
		const double tRuns1 = Seconds();
		auto median = [](std::vector<double> v) { if (v.empty()) return -1.0; std::sort(v.begin(), v.end()); return v.size() % 2 ? v[v.size() / 2] : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]); };
		auto minimum = [](const std::vector<double>& v) { return v.empty() ? -1.0 : *std::min_element(v.begin(), v.end()); };

		// 8. the outputs and the report
		std::string report;
		auto add = [&](const char* format, ...) {
			char line[2048];
			va_list args;
			va_start(args, format);
			vsnprintf(line, sizeof(line), format, args);
			va_end(args);
			report += line;
			report += "\n";
			Log("%s", line);
		};
		add("NvrRtHelper replay report");
		add("capture   %s (%llu MB): frame %llu, captured on %s", Narrow(capturePath).c_str(), (unsigned long long)(c.file.size() >> 20),
			(unsigned long long)c.header.frame, c.header.device);
		add("device    %s", h.vk.properties.deviceName);
		add("scene     %u geometries (%u without data: empty), %llu triangles, %llu MB of structures; %u instances", c.geometryCount, c.header.noDataGeometries,
			(unsigned long long)triangles, (unsigned long long)(structureBytes >> 20), instanceCount);
		add("wall      load %.0f ms, bottom levels %.0f ms, %u runs %.0f ms (median run %.1f ms wall)", (tLoaded - t0) * 1000.0, (tBuild1 - tBuild0) * 1000.0,
			runs, (tRuns1 - tRuns0) * 1000.0, median(wallTimes));
		add("GPU       scene (top level) %.3f ms median", median(sceneTimes));
		add("passes    (GPU ms: median / min of %u runs | the captured frame in the game)", runs);
		uint32_t stamped = 0;
		for (uint32_t p = 0; p < passCount; p++) {
			const NvrRtPass& pass = c.passes[p];
			const std::string name = pass.shader < c.shaderPaths.size() ? ShaderName(c.shaderPaths[pass.shader]) : "?";
			std::string images;
			for (uint32_t i = 0; i < pass.imageCount; i++) {
				char one[16];
				sprintf_s(one, "%s%u%s", i ? " " : "", NvrRtPassImage(pass, i), (pass.writeMask >> i) & 1 ? "w" : "");
				images += one;
			}
			const double game = runsPass[p] && stamped < c.passMs.size() ? c.passMs[stamped] : -1.0;	// (the game's times are of the passes that ran there)
			if (runsPass[p]) stamped++;
			add("  pass %u  %-22s %7.3f / %7.3f | %7.3f  groups %ux%ux%u, images [%s]%s", p, name.c_str(), median(passTimes[p]), minimum(passTimes[p]), game,
				pass.groupsX, pass.groupsY, pass.groupsZ, images.c_str(), runsPass[p] ? "" : "  SKIPPED");
		}
		add("images    written by the passes: replay -> img<i>.png/.f32, the game's output of that frame -> game_img<i>.png/.f32");
		for (uint32_t i = 0; i < c.images.size(); i++) {
			ReplayImage& im = c.images[i];
			if (dumpInputs && im.before) {
				std::string note;
				wchar_t base[64];
				swprintf_s(base, L"\\input_%02u", i);
				SaveImage(outDir + base, im.desc, im.before, note);
			}
			if (!im.written || !im.image.image || !im.desc.texelBytes) continue;
			const uint8_t* replayed = (const uint8_t*)imageReadback.mapped + im.readback;
			std::string note, gameNote;
			wchar_t base[64];
			swprintf_s(base, L"\\img%02u", i);
			SaveImage(outDir + base, im.desc, replayed, note);
			if (im.after) {
				swprintf_s(base, L"\\game_img%02u", i);
				SaveImage(outDir + base, im.desc, im.after, gameNote);
				const DiffStats d = Difference(im.desc, replayed, im.after);
				add("  image %2u %ux%u format %u: %s; vs the game: mean |diff| %.3g, max %.3g, %llu of %llu values > 1e-3, %llu NaN mismatches", i,
					im.desc.width, im.desc.height, im.desc.vkFormat, note.c_str(), d.meanAbs, d.maxAbs, (unsigned long long)d.over,
					(unsigned long long)d.values, (unsigned long long)d.nanMismatch);
			}
			else add("  image %2u %ux%u format %u: %s (no game output in the capture)", i, im.desc.width, im.desc.height, im.desc.vkFormat, note.c_str());
		}
		add("total     %.1f s wall", Seconds() - t0);
		FILE* rf = nullptr;
		if (_wfopen_s(&rf, (outDir + L"\\report.txt").c_str(), L"wb") == 0 && rf) { fwrite(report.data(), 1, report.size(), rf); fclose(rf); }
		printf("report: %ls\\report.txt\n", outDir.c_str());
		if (g_log) fclose(g_log);
		g_log = nullptr;
		ExitProcess(0);	// (the device and its objects go with the process)
	}
