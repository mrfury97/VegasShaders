// NvrRtHelper.exe -- the 64-bit helper that owns New Vegas Reloaded's ray tracing scene. MIT licence (LICENSE here).
//
// Why a separate process: Fallout New Vegas, NVR and the NVR DXVK d3d9.dll are 32-bit, and Windows' 32-bit Vulkan drivers
// offer no ray tracing (AMD 25.10 / NVIDIA 582 measured 2026-10-28). This helper makes its own Vulkan 1.2 device on the GPU
// the game uses (matched by LUID), imports the images and the two timeline semaphores the fork exports (patch 0026), keeps
// the bottom-level structures of every geometry NVR sends through the arena image (fork patch 0028, StageGeometry), rebuilds
// the scene (top level) from NVR's instance list every frame and runs NVR's trace passes (compute shaders with
// GL_EXT_ray_query) between a GPU wait for "inputs ready" = N and a signal of "output ready" = N. Protocol: NvrRtProtocol.h;
// design and the pass rules: docs/next/FORK-RT-CORE.md in the NVR repository.
//
// Usage:
//   NvrRtHelper.exe --shm <name>   started by NVR (never by hand)
//   NvrRtHelper.exe --probe        prints every GPU and whether it can run the helper, then exits
//   NvrRtHelper.exe --replay <capture.nvrcap> [--shader <name>=<file.spv> ...] [--shaders <folder>] [--runs N] [--out <folder>]
//                                  UNOFFICIAL 2026-11-03: runs one captured frame again without the game (docs/next/RT-REPLAY.md).
//                                  A capture is written when NvrRtHelper.capture (checked every 60 frames) or NvrRtHelper.capture-at
//                                  (read at start) is next to the exe: CaptureSpec below.
//
// The pass binding layout (set 0), the same for every trace shader:
//   binding 0      accelerationStructureEXT     the scene
//   binding 1      uniform block (std140)        the pass's constants (NvrRtPass::constants, up to 512 bytes)
//   binding 2 + i  image2D (storage, GENERAL)    the pass's images[i] (declare the format, e.g. r32f, rgba16f, r16f)
//   binding 10     readonly SSBO                 NvrRtInstanceRecord per instance (albedo, flags, where its triangles are)
//   binding 11     readonly SSBO                 uint words: every geometry's float3 positions and uint triangle list

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstddef>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "NvrRtProtocol.h"
#include "NvrRtCapture.h"	// UNOFFICIAL 2026-11-03 frame capture / replay (claude/rt-replay)
#include "RtArenaGuard.h"	// kRtArenaGuardSpv (RtArenaGuard.comp)

namespace {

	// ---------------------------------------------------------------------------------------------------------------------
	// Log: NvrRtHelper.log next to the exe (or in the temp folder), plus the last status line into the shared block.

	CRITICAL_SECTION	g_logLock;
	FILE*				g_log = nullptr;
	NvrRtShared*		g_shared = nullptr;
	HANDLE				g_statusEvent = nullptr;
	bool				g_console = false;
	std::wstring		g_exeDir;

	void Log(const char* format, ...) {
		char line[1024];
		va_list args;
		va_start(args, format);
		vsnprintf(line, sizeof(line), format, args);
		va_end(args);
		SYSTEMTIME t;
		GetLocalTime(&t);
		EnterCriticalSection(&g_logLock);
		if (g_log) {
			fprintf(g_log, "%02u:%02u:%02u.%03u %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, line);
			fflush(g_log);
		}
		if (g_console) printf("%s\n", line);
		LeaveCriticalSection(&g_logLock);
	}

	void Status(const char* format, ...) {
		char line[512];
		va_list args;
		va_start(args, format);
		vsnprintf(line, sizeof(line), format, args);
		va_end(args);
		Log("%s", line);
		if (g_shared) {
			strncpy_s(g_shared->message, sizeof(g_shared->message), line, _TRUNCATE);
			MemoryBarrier();
			InterlockedIncrement((volatile LONG*)&g_shared->messageSerial);
			if (g_statusEvent) SetEvent(g_statusEvent);
		}
	}

	void SetState(NvrRtState state) {
		if (!g_shared) return;
		MemoryBarrier();
		g_shared->state = state;
		if (g_statusEvent) SetEvent(g_statusEvent);
	}

	std::wstring ExeDir() {
		wchar_t path[MAX_PATH] = {};
		GetModuleFileNameW(nullptr, path, MAX_PATH);
		std::wstring dir(path);
		size_t slash = dir.find_last_of(L"\\/");
		return slash == std::wstring::npos ? L"." : dir.substr(0, slash);
	}

	void OpenLog(const wchar_t* name) {
		std::wstring path = g_exeDir + L"\\" + name;
		if (_wfopen_s(&g_log, path.c_str(), L"w") != 0) {
			wchar_t temp[MAX_PATH] = {};
			GetTempPathW(MAX_PATH, temp);
			path = std::wstring(temp) + name;
			if (_wfopen_s(&g_log, path.c_str(), L"w") != 0) g_log = nullptr;
		}
	}

	const char* VkResultName(VkResult r) {
		switch (r) {
			case VK_SUCCESS: return "VK_SUCCESS";
			case VK_TIMEOUT: return "VK_TIMEOUT";
			case VK_NOT_READY: return "VK_NOT_READY";
			case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
			case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
			case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
			case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
			case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
			case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
			case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
			case VK_ERROR_INVALID_SHADER_NV: return "VK_ERROR_INVALID_SHADER_NV";
			default: return "VkResult(other)";
		}
	}

#define VK_CHECK(call, what) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { Status("Vulkan: %s failed: %s (%d)", what, VkResultName(r_), (int)r_); return false; } } while (0)

	bool HasExtension(const std::vector<VkExtensionProperties>& list, const char* name) {
		for (const VkExtensionProperties& e : list) if (!strcmp(e.extensionName, name)) return true;
		return false;
	}

	VkDeviceSize AlignUp(VkDeviceSize v, VkDeviceSize a) { return (v + a - 1) / a * a; }

	const uint32_t kCmdRing = 4;
	const uint32_t kQueriesPerSlot = 4 + NVR_RT_MAX_PASSES;	// start, after the builds, after the scene, end; then one after each pass
	const uint32_t kCompactPerSlot = 512;	// compacted-size queries a frame (more new static geometries wait for a later rebuild: none)
	const VkDeviceSize kPoolBlock = 64ull << 20;
	const uint32_t kDataWords = 32u << 20;	// the geometry data (binding 11): 128 MB of positions and indices

	// ---------------------------------------------------------------------------------------------------------------------
	// The device: the GPU NVR's DXVK uses, with ray queries, acceleration structures, buffer addresses, timeline semaphores and
	// Win32 external memory / semaphores.

	struct VulkanDevice {
		VkInstance			instance = VK_NULL_HANDLE;
		VkPhysicalDevice	physical = VK_NULL_HANDLE;
		VkDevice			device = VK_NULL_HANDLE;
		uint32_t			queueFamily = 0;
		VkQueue				queue = VK_NULL_HANDLE;
		VkPhysicalDeviceProperties	properties = {};
		VkPhysicalDeviceIDProperties ids = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
		VkPhysicalDeviceAccelerationStructurePropertiesKHR asProperties = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR };
		VkPhysicalDeviceMemoryProperties memory = {};
		uint32_t			timestampBits = 0;
		const char*			matchedBy = "";	// how the game's GPU was found: "adapter LUID" / "device UUID" / "first RT GPU (probe)"
		PFN_vkImportSemaphoreWin32HandleKHR			importSemaphoreWin32Handle = nullptr;
		PFN_vkCreateAccelerationStructureKHR		createAs = nullptr;
		PFN_vkDestroyAccelerationStructureKHR		destroyAs = nullptr;
		PFN_vkGetAccelerationStructureBuildSizesKHR	buildSizes = nullptr;
		PFN_vkGetAccelerationStructureDeviceAddressKHR asAddress = nullptr;
		PFN_vkCmdBuildAccelerationStructuresKHR		cmdBuildAs = nullptr;
		PFN_vkCmdWriteAccelerationStructuresPropertiesKHR cmdWriteAsProperties = nullptr;	// compaction (2026-10-30)
		PFN_vkCmdCopyAccelerationStructureKHR		cmdCopyAs = nullptr;
	};

	// Whether a physical device can run the helper; why not in `why`.
	bool DeviceUsable(VkPhysicalDevice d, std::string& why) {
		uint32_t count = 0;
		vkEnumerateDeviceExtensionProperties(d, nullptr, &count, nullptr);
		std::vector<VkExtensionProperties> exts(count);
		vkEnumerateDeviceExtensionProperties(d, nullptr, &count, exts.data());
		const char* required[] = { VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_RAY_QUERY_EXTENSION_NAME,
			VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME, VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME };
		for (const char* name : required)
			if (!HasExtension(exts, name)) { why = std::string("no ") + name; return false; }
		VkPhysicalDeviceRayQueryFeaturesKHR rq = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR };
		VkPhysicalDeviceAccelerationStructureFeaturesKHR as = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, &rq };
		VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &as };
		VkPhysicalDeviceFeatures2 f = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12 };
		vkGetPhysicalDeviceFeatures2(d, &f);
		if (!rq.rayQuery) { why = "rayQuery feature off"; return false; }
		if (!as.accelerationStructure) { why = "accelerationStructure feature off"; return false; }
		if (!f12.bufferDeviceAddress) { why = "no bufferDeviceAddress"; return false; }
		if (!f12.timelineSemaphore) { why = "no timeline semaphores"; return false; }
		return true;
	}

	// luid / uuid: the game's GPU; null: the first usable one (probe).
	bool CreateVulkanDevice(VulkanDevice& vk, const uint8_t* luid, bool luidValid, const uint8_t* uuid) {
		uint32_t loaderVersion = VK_API_VERSION_1_0;
		PFN_vkEnumerateInstanceVersion enumerateVersion = (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion");
		if (enumerateVersion) enumerateVersion(&loaderVersion);
		if (loaderVersion < VK_API_VERSION_1_2) { Status("Vulkan: the loader is older than 1.2"); return false; }

		VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
		app.pApplicationName = "NvrRtHelper";
		app.applicationVersion = 1;
		app.pEngineName = "New Vegas Reloaded";
		app.engineVersion = 1;
		app.apiVersion = VK_API_VERSION_1_2;
		VkInstanceCreateInfo instanceInfo = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
		instanceInfo.pApplicationInfo = &app;
		// (2026-11-06, NVIDIA device-lost hunt) dev switch: a file NvrRtHelper.validate next to the exe turns on the Khronos validation
		// layer for this process only (the loader's VK_LOADER_LAYERS_ENABLE also hit the 32-bit game's DXVK, which crashed in it). Its
		// settings (log file, GPU-assisted / sync checks) come from vk_layer_settings.txt as usual (VK_LAYER_SETTINGS_PATH or the cwd).
		const char* validationLayer = "VK_LAYER_KHRONOS_validation";
		if (GetFileAttributesW((g_exeDir + L"\\NvrRtHelper.validate").c_str()) != INVALID_FILE_ATTRIBUTES) {
			instanceInfo.enabledLayerCount = 1;
			instanceInfo.ppEnabledLayerNames = &validationLayer;
			Log("validation: the Khronos validation layer is asked for (NvrRtHelper.validate)");
		}
		VK_CHECK(vkCreateInstance(&instanceInfo, nullptr, &vk.instance), "vkCreateInstance");

		uint32_t count = 0;
		vkEnumeratePhysicalDevices(vk.instance, &count, nullptr);
		std::vector<VkPhysicalDevice> devices(count);
		vkEnumeratePhysicalDevices(vk.instance, &count, devices.data());
		std::string whyNot;
		for (VkPhysicalDevice d : devices) {
			VkPhysicalDeviceIDProperties ids = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
			VkPhysicalDeviceProperties2 props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &ids };
			vkGetPhysicalDeviceProperties2(d, &props);
			std::string why;
			const bool usable = DeviceUsable(d, why);
			bool match;
			const char* how;
			if (luid && luidValid && ids.deviceLUIDValid) { match = !memcmp(ids.deviceLUID, luid, VK_LUID_SIZE); how = "adapter LUID"; }
			else if (uuid) { match = !memcmp(ids.deviceUUID, uuid, VK_UUID_SIZE); how = "device UUID"; }
			else { match = usable; how = "first RT GPU (probe)"; }
			Log("Vulkan device: %s (vendor 0x%04X, API %u.%u.%u): %s%s", props.properties.deviceName, props.properties.vendorID,
				VK_VERSION_MAJOR(props.properties.apiVersion), VK_VERSION_MINOR(props.properties.apiVersion), VK_VERSION_PATCH(props.properties.apiVersion),
				usable ? "ray tracing OK" : why.c_str(), match && !vk.physical ? "  <- this one" : "");
			if (match && !vk.physical) {
				vk.matchedBy = how;
				if (!usable) whyNot = std::string(props.properties.deviceName) + ": " + why;
				vk.physical = d;
				vk.properties = props.properties;
				vk.ids = ids;
				vk.ids.pNext = nullptr;
			}
		}
		if (!vk.physical) { Status("Vulkan: the GPU the game uses was not found among %u Vulkan devices", count); return false; }
		if (!whyNot.empty()) { Status("no ray tracing on the game's GPU (%s, found by %s): the RT effects stay off", whyNot.c_str(), vk.matchedBy); return false; }

		VkPhysicalDeviceProperties2 props2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &vk.asProperties };
		vkGetPhysicalDeviceProperties2(vk.physical, &props2);
		vkGetPhysicalDeviceMemoryProperties(vk.physical, &vk.memory);

		count = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(vk.physical, &count, nullptr);
		std::vector<VkQueueFamilyProperties> families(count);
		vkGetPhysicalDeviceQueueFamilyProperties(vk.physical, &count, families.data());
		bool found = false;
		for (uint32_t i = 0; i < count && !found; i++) {
			if ((families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
				vk.queueFamily = i;
				vk.timestampBits = families[i].timestampValidBits;
				found = true;
			}
		}
		if (!found) { Status("Vulkan: no graphics + compute queue"); return false; }

		const char* enabled[] = { VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_RAY_QUERY_EXTENSION_NAME, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
			VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME };

		VkPhysicalDeviceVulkan12Features supported12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
		VkPhysicalDeviceFeatures2 supported = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &supported12 };
		vkGetPhysicalDeviceFeatures2(vk.physical, &supported);

		VkPhysicalDeviceRayQueryFeaturesKHR rq = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR };
		rq.rayQuery = VK_TRUE;
		VkPhysicalDeviceAccelerationStructureFeaturesKHR as = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, &rq };
		as.accelerationStructure = VK_TRUE;
		VkPhysicalDeviceVulkan12Features features12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &as };
		features12.timelineSemaphore = VK_TRUE;
		features12.bufferDeviceAddress = VK_TRUE;
		features12.hostQueryReset = supported12.hostQueryReset;
		features12.descriptorBindingPartiallyBound = supported12.descriptorBindingPartiallyBound;
		VkPhysicalDeviceFeatures2 features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &features12 };
		features.features.shaderStorageImageExtendedFormats = supported.features.shaderStorageImageExtendedFormats;
		features.features.shaderStorageImageWriteWithoutFormat = supported.features.shaderStorageImageWriteWithoutFormat;
		features.features.shaderStorageImageReadWithoutFormat = supported.features.shaderStorageImageReadWithoutFormat;
		features.features.shaderInt64 = supported.features.shaderInt64;

		float priority = 1.0f;
		VkDeviceQueueCreateInfo queueInfo = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
		queueInfo.queueFamilyIndex = vk.queueFamily;
		queueInfo.queueCount = 1;
		queueInfo.pQueuePriorities = &priority;
		VkDeviceCreateInfo deviceInfo = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &features };
		deviceInfo.queueCreateInfoCount = 1;
		deviceInfo.pQueueCreateInfos = &queueInfo;
		deviceInfo.enabledExtensionCount = (uint32_t)(sizeof(enabled) / sizeof(enabled[0]));
		deviceInfo.ppEnabledExtensionNames = enabled;
		VK_CHECK(vkCreateDevice(vk.physical, &deviceInfo, nullptr, &vk.device), "vkCreateDevice");
		vkGetDeviceQueue(vk.device, vk.queueFamily, 0, &vk.queue);
		vk.importSemaphoreWin32Handle = (PFN_vkImportSemaphoreWin32HandleKHR)vkGetDeviceProcAddr(vk.device, "vkImportSemaphoreWin32HandleKHR");
		vk.createAs = (PFN_vkCreateAccelerationStructureKHR)vkGetDeviceProcAddr(vk.device, "vkCreateAccelerationStructureKHR");
		vk.destroyAs = (PFN_vkDestroyAccelerationStructureKHR)vkGetDeviceProcAddr(vk.device, "vkDestroyAccelerationStructureKHR");
		vk.buildSizes = (PFN_vkGetAccelerationStructureBuildSizesKHR)vkGetDeviceProcAddr(vk.device, "vkGetAccelerationStructureBuildSizesKHR");
		vk.asAddress = (PFN_vkGetAccelerationStructureDeviceAddressKHR)vkGetDeviceProcAddr(vk.device, "vkGetAccelerationStructureDeviceAddressKHR");
		vk.cmdBuildAs = (PFN_vkCmdBuildAccelerationStructuresKHR)vkGetDeviceProcAddr(vk.device, "vkCmdBuildAccelerationStructuresKHR");
		vk.cmdWriteAsProperties = (PFN_vkCmdWriteAccelerationStructuresPropertiesKHR)vkGetDeviceProcAddr(vk.device, "vkCmdWriteAccelerationStructuresPropertiesKHR");
		vk.cmdCopyAs = (PFN_vkCmdCopyAccelerationStructureKHR)vkGetDeviceProcAddr(vk.device, "vkCmdCopyAccelerationStructureKHR");
		if (!vk.importSemaphoreWin32Handle || !vk.createAs || !vk.destroyAs || !vk.buildSizes || !vk.asAddress || !vk.cmdBuildAs) {
			Status("Vulkan: entry points missing");
			return false;
		}
		Log("device: %s, max %llu instances, %llu triangles a geometry, scratch alignment %u", vk.properties.deviceName,
			(unsigned long long)vk.asProperties.maxInstanceCount, (unsigned long long)vk.asProperties.maxPrimitiveCount,
			vk.asProperties.minAccelerationStructureScratchOffsetAlignment);
		return true;
	}

	int32_t FindMemoryType(const VulkanDevice& vk, uint32_t bits, VkMemoryPropertyFlags flags) {
		for (uint32_t i = 0; i < vk.memory.memoryTypeCount; i++)
			if ((bits & (1u << i)) && (vk.memory.memoryTypes[i].propertyFlags & flags) == flags) return (int32_t)i;
		return -1;
	}

	struct Buffer {
		VkBuffer		buffer = VK_NULL_HANDLE;
		VkDeviceMemory	memory = VK_NULL_HANDLE;
		VkDeviceSize	size = 0;
		uint64_t		address = 0;
		void*			mapped = nullptr;
	};

	bool CreateBuffer(const VulkanDevice& vk, VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible, Buffer& out) {
		VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
		info.size = std::max<VkDeviceSize>(size, 256);
		info.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
		VK_CHECK(vkCreateBuffer(vk.device, &info, nullptr, &out.buffer), "vkCreateBuffer");
		VkMemoryRequirements req = {};
		vkGetBufferMemoryRequirements(vk.device, out.buffer, &req);
		int32_t type = FindMemoryType(vk, req.memoryTypeBits, hostVisible ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
			: VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		if (type < 0) type = FindMemoryType(vk, req.memoryTypeBits, 0);
		if (type < 0) { Status("Vulkan: no memory type for a buffer"); return false; }
		VkMemoryAllocateFlagsInfo flags = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO };
		flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
		VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flags };
		alloc.allocationSize = req.size;
		alloc.memoryTypeIndex = (uint32_t)type;
		VK_CHECK(vkAllocateMemory(vk.device, &alloc, nullptr, &out.memory), "vkAllocateMemory (buffer)");
		VK_CHECK(vkBindBufferMemory(vk.device, out.buffer, out.memory, 0), "vkBindBufferMemory");
		VkBufferDeviceAddressInfo addressInfo = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
		addressInfo.buffer = out.buffer;
		out.address = vkGetBufferDeviceAddress(vk.device, &addressInfo);
		out.size = info.size;
		if (hostVisible) VK_CHECK(vkMapMemory(vk.device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped), "vkMapMemory");
		return true;
	}

	void DestroyBuffer(const VulkanDevice& vk, Buffer& b) {
		if (b.buffer) vkDestroyBuffer(vk.device, b.buffer, nullptr);
		if (b.memory) vkFreeMemory(vk.device, b.memory, nullptr);
		b = Buffer();
	}

	// The memory of every bottom-level structure: 64 MB blocks, first fit, 256-byte aligned pieces.
	struct StructurePool {
		struct Block { Buffer buffer; std::map<VkDeviceSize, VkDeviceSize> free; VkDeviceSize size = 0; };
		std::vector<Block> blocks;
		VkDeviceSize used = 0, total = 0;	// in structures / in blocks
		uint32_t returned = 0;				// empty blocks given back (2026-10-30)

		bool Allocate(const VulkanDevice& vk, VkDeviceSize size, uint32_t& block, VkDeviceSize& offset) {
			size = AlignUp(size, 256);
			for (uint32_t b = 0; b < blocks.size(); b++) {
				if (!blocks[b].buffer.buffer) continue;	// given back (Free): reused below
				for (auto it = blocks[b].free.begin(); it != blocks[b].free.end(); ++it) {
					if (it->second < size) continue;
					block = b;
					offset = it->first;
					const VkDeviceSize rest = it->second - size, at = it->first + size;
					blocks[b].free.erase(it);
					if (rest) blocks[b].free[at] = rest;
					used += size;
					return true;
				}
			}
			Block nb;
			const VkDeviceSize blockSize = std::max(kPoolBlock, size);
			if (!CreateBuffer(vk, blockSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false, nb.buffer)) return false;
			if (blockSize > size) nb.free[size] = blockSize - size;
			nb.size = blockSize;
			block = (uint32_t)blocks.size();
			for (uint32_t b = 0; b < blocks.size(); b++) if (!blocks[b].buffer.buffer) { block = b; break; }
			if (block == blocks.size()) blocks.push_back(nb); else blocks[block] = nb;
			total += blockSize;
			offset = 0;
			used += size;
			Log("structure memory: block %u of %llu MB", block, (unsigned long long)(blockSize >> 20));
			return true;
		}

		void Free(const VulkanDevice& vk, uint32_t block, VkDeviceSize offset, VkDeviceSize size) {
			size = AlignUp(size, 256);
			auto& free = blocks[block].free;
			auto it = free.emplace(offset, size).first;
			auto next = std::next(it);
			if (next != free.end() && it->first + it->second == next->first) { it->second += next->second; free.erase(next); }
			if (it != free.begin()) {
				auto prev = std::prev(it);
				if (prev->first + prev->second == it->first) { prev->second += it->second; free.erase(it); }
			}
			used -= size;
			// a block with nothing in it goes back to the driver (not the first: the usual scene keeps it busy anyway)
			if (block > 0 && free.size() == 1 && free.begin()->first == 0 && free.begin()->second == blocks[block].size) {
				total -= blocks[block].size;
				DestroyBuffer(vk, blocks[block].buffer);
				blocks[block].free.clear();
				blocks[block].size = 0;
				returned++;
			}
		}
	};

	struct Geometry {
		VkAccelerationStructureKHR	as = VK_NULL_HANDLE;
		uint64_t					address = 0;
		uint32_t					block = 0;
		VkDeviceSize				offset = 0, size = 0;
		uint32_t					triangles = 0;
		uint32_t					vertexOffset = NVR_RT_NO_DATA;	// words in the geometry data (binding 11)
		uint32_t					indexOffset = NVR_RT_NO_DATA;
		uint32_t					uvOffset = NVR_RT_NO_DATA;		// the texcoords (float2), alpha-tested geometry only
		uint32_t					dataWords = 0;
		uint32_t					vertices = 0;
		bool						dynamic = false;				// built with ALLOW_UPDATE: later frames refit it in place
		bool						compacted = false;
		uint32_t					opFlags = 0;					// UNOFFICIAL 2026-11-03: NVR_RT_OP_FLAG_* of its build (the capture rebuilds it the same way)
	};

	// The geometry data's words: first fit.
	struct WordPool {
		std::map<uint32_t, uint32_t> free;	// offset -> count
		void Init(uint32_t words) { free.clear(); free[0] = words; }
		uint32_t Allocate(uint32_t words) {
			for (auto it = free.begin(); it != free.end(); ++it) {
				if (it->second < words) continue;
				const uint32_t at = it->first, rest = it->second - words;
				free.erase(it);
				if (rest) free[at + words] = rest;
				return at;
			}
			return NVR_RT_NO_DATA;
		}
		void Free(uint32_t at, uint32_t words) {
			if (at == NVR_RT_NO_DATA || !words) return;
			auto it = free.emplace(at, words).first;
			auto next = std::next(it);
			if (next != free.end() && it->first + it->second == next->first) { it->second += next->second; free.erase(next); }
			if (it != free.begin()) {
				auto prev = std::prev(it);
				if (prev->first + prev->second == it->first) { prev->second += it->second; free.erase(it); }
			}
		}
	};

	struct SharedImage {
		VkImage			image = VK_NULL_HANDLE;
		VkDeviceMemory	memory = VK_NULL_HANDLE;
		VkImageView		view = VK_NULL_HANDLE;
		VkFormat		format = VK_FORMAT_UNDEFINED;
		uint32_t		width = 0, height = 0;
		uint32_t		usage = 0;			// UNOFFICIAL 2026-11-03: as imported (the capture notes it)
	};

	// The importer's VkImage must have the exporter's layout. The game's driver is the 32-bit one, this is the 64-bit one: with the
	// same create info they may still disagree on the size (2026-10-28, AMD 25.10: a 1280x720 R32F image is 3686400 bytes in the
	// game, 3957104 here = compression metadata the 32-bit driver did not add). So: the exporter's create info first; if that needs
	// more memory than the game allocated, variants that commonly switch the metadata off (the usage bits that ask for it, a
	// mutable format), taking the first whose size is EXACTLY the game's (the same size = the same plain layout).
	bool ImportImage(const VulkanDevice& vk, const NvrRtImageDesc& d, uint32_t index, SharedImage& out) {
		VkExternalMemoryImageCreateInfo external = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
		external.handleTypes = (VkExternalMemoryHandleTypeFlags)d.handleType;
		VkFormat viewFormat = (VkFormat)d.vkFormat;
		VkImageFormatListCreateInfo formatList = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, &external };
		formatList.viewFormatCount = 1;
		formatList.pViewFormats = &viewFormat;
		struct Variant { const char* name; VkImageUsageFlags usageOff; VkImageCreateFlags flagsOn; bool list; };
		const Variant variants[] = {
			{ "as the game made it", 0, 0, false },
			{ "without colour attachment use", VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0, false },
			{ "mutable format", 0, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, false },
			{ "mutable format, one view format", 0, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, true },
			{ "without colour attachment use, mutable format", VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, false },
			{ "without sampled / colour attachment use", VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, false },
		};
		VkMemoryRequirements requirements = {};
		const Variant* chosen = nullptr;
		for (const Variant& v : variants) {
			VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, v.list ? (const void*)&formatList : (const void*)&external };
			info.flags = d.flags | v.flagsOn;
			info.imageType = VK_IMAGE_TYPE_2D;
			info.format = (VkFormat)d.vkFormat;
			info.extent = { d.width, d.height, 1 };
			info.mipLevels = 1;
			info.arrayLayers = 1;
			info.samples = VK_SAMPLE_COUNT_1_BIT;
			info.tiling = (VkImageTiling)d.tiling;
			info.usage = d.usage & ~v.usageOff;
			info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			VkImage image = VK_NULL_HANDLE;
			if (vkCreateImage(vk.device, &info, nullptr, &image) != VK_SUCCESS) continue;
			vkGetImageMemoryRequirements(vk.device, image, &requirements);
			const bool typeOk = (requirements.memoryTypeBits & (1u << d.memoryTypeIndex)) != 0;
			const bool fits = &v == &variants[0] ? requirements.size <= d.allocationSize : requirements.size == d.allocationSize;
			if (typeOk && fits) { out.image = image; chosen = &v; break; }
			Log("shared image %u (%ux%u, format %u, usage 0x%X, flags 0x%X): %s needs %llu bytes (types 0x%X), the game's is %llu (type %u)",
				index, d.width, d.height, d.vkFormat, info.usage, info.flags, v.name, (unsigned long long)requirements.size, requirements.memoryTypeBits,
				(unsigned long long)d.allocationSize, d.memoryTypeIndex);
			vkDestroyImage(vk.device, image, nullptr);
		}
		if (!out.image) {
			Status("Vulkan: shared image %u (%ux%u, format %u) does not fit the game's allocation in any layout this driver offers (see the log)",
				index, d.width, d.height, d.vkFormat);
			return false;
		}
		if (chosen != &variants[0]) Log("shared image %u imported %s (the 64-bit driver's default layout differs from the game's)", index, chosen->name);
		VkMemoryDedicatedAllocateInfo dedicated = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
		dedicated.image = out.image;
		VkImportMemoryWin32HandleInfoKHR import = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, d.dedicated ? &dedicated : nullptr };
		import.handleType = (VkExternalMemoryHandleTypeFlagBits)d.handleType;
		import.handle = (HANDLE)(uintptr_t)d.handle;
		VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &import };
		alloc.allocationSize = d.allocationSize;
		alloc.memoryTypeIndex = d.memoryTypeIndex;
		VK_CHECK(vkAllocateMemory(vk.device, &alloc, nullptr, &out.memory), "vkAllocateMemory (import)");
		CloseHandle((HANDLE)(uintptr_t)d.handle);
		VK_CHECK(vkBindImageMemory(vk.device, out.image, out.memory, 0), "vkBindImageMemory (shared)");
		VkImageViewCreateInfo view = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		view.image = out.image;
		view.viewType = VK_IMAGE_VIEW_TYPE_2D;
		view.format = (VkFormat)d.vkFormat;
		view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		VK_CHECK(vkCreateImageView(vk.device, &view, nullptr, &out.view), "vkCreateImageView (shared)");
		out.format = (VkFormat)d.vkFormat;
		out.width = d.width;
		out.height = d.height;
		out.usage = d.usage & ~chosen->usageOff;
		Log("imported shared image %u: %ux%u, format %u, %llu bytes, memory type %u", index, d.width, d.height, d.vkFormat,
			(unsigned long long)d.allocationSize, d.memoryTypeIndex);
		return true;
	}

	bool ImportTimeline(const VulkanDevice& vk, uint64_t handle, const char* name, VkSemaphore& out) {
		VkSemaphoreTypeCreateInfo type = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
		type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
		VkSemaphoreCreateInfo info = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type };
		VK_CHECK(vkCreateSemaphore(vk.device, &info, nullptr, &out), "vkCreateSemaphore (shared)");
		VkImportSemaphoreWin32HandleInfoKHR import = { VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR };
		import.semaphore = out;
		import.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
		import.handle = (HANDLE)(uintptr_t)handle;
		VK_CHECK(vk.importSemaphoreWin32Handle(vk.device, &import), "vkImportSemaphoreWin32HandleKHR");
		CloseHandle((HANDLE)(uintptr_t)handle);
		Log("imported the %s semaphore", name);
		return true;
	}

	void FullBarrier(VkCommandBuffer cmd) {
		VkMemoryBarrier b = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
		b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
		b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
	}

	uint32_t Average(const uint64_t* values, uint32_t count) {
		const uint32_t n = std::min(count, 64u);
		if (!n) return 0;
		uint64_t sum = 0;
		for (uint32_t i = 0; i < n; i++) sum += values[i];
		return (uint32_t)(sum / n);
	}

	// ---------------------------------------------------------------------------------------------------------------------
	// The helper proper.

	struct Slot {
		VkCommandBuffer	cmd = VK_NULL_HANDLE;
		uint64_t		frame = 0;			// the frame last submitted from this slot (0 = none)
		Buffer			instances;			// host-visible VkAccelerationStructureInstanceKHR[NVR_RT_MAX_INSTANCES]
		Buffer			constants;			// host-visible, NVR_RT_PASS_CONSTANTS per pass
		Buffer			records;			// host-visible NvrRtInstanceRecord[NVR_RT_MAX_INSTANCES] (binding 10)
		VkDescriptorSet	sets[NVR_RT_MAX_PASSES] = {};
		bool			measured[3] = {};	// which sections had work (build, scene, trace) in that frame
		uint32_t		passShaders[NVR_RT_MAX_PASSES] = {};	// the shader of each timed pass (timestamp 4 + i after it)
		uint32_t		passesStamped = 0;
		std::vector<std::pair<uint32_t, VkAccelerationStructureKHR>> compactAsked;	// query i = this geometry (id, the structure asked about)
	};

	struct CompactReady { uint32_t id; VkAccelerationStructureKHR as; VkDeviceSize size; };

	// UNOFFICIAL 2026-11-03 (claude/rt-replay) frame capture: what to capture, read from a small text file next to the exe, one
	// "key value" a line (# starts a comment), every key optional:
	//   frame  <N>        the first helper frame (exchange) that may be captured
	//   shader <text>     only frames with a pass whose .spv path contains this (any case), e.g. RtPathTrace
	//   skip   <K>        pass over the first K frames that match (let an accumulation settle)
	//   count  <C>        captures in a row (default 1)
	//   out    <folder>   where the .nvrcap goes (default: next to the exe)
	// NvrRtHelper.capture-at is read once at start (it stays: a bench copies it in with the helper); NvrRtHelper.capture is looked for
	// every 60 frames and deleted when read (drop it in while the game runs). No file: nothing else happens (one file check a second).
	struct CaptureSpec {
		bool			armed = false;
		uint64_t		minFrame = 0;
		std::string		shader;
		uint32_t		skip = 0;
		uint32_t		count = 1;
		std::wstring	out;
	};

	struct CaptureRegion { uint32_t tag, index; VkDeviceSize offset, size; };	// a chunk the GPU copies into the capture buffer

	struct Capture {
		CaptureSpec		spec;
		bool			active = false;		// the frame being recorded is captured
		Buffer			buffer;				// host-visible: the images before / after the passes, the geometry data
		std::vector<CaptureRegion> regions;
		std::vector<uint8_t> meta;			// the CPU chunks (images' descriptions, geometries, instances, records, passes, shaders)
		NvrRtCaptureHeader header = {};
		uint32_t		written = 0;
	};

	struct PendingFree { uint64_t frame; Geometry geometry; };

	struct Helper {
		VulkanDevice	vk;
		SharedImage		images[NVR_RT_MAX_IMAGES];
		uint32_t		imageCount = 0;
		VkSemaphore		inputsReady = VK_NULL_HANDLE, outputReady = VK_NULL_HANDLE;
		VkCommandPool	pool = VK_NULL_HANDLE;
		VkQueryPool		queries = VK_NULL_HANDLE;
		Slot			slots[kCmdRing];
		Buffer			staging;			// the arena's rows, copied out every frame that builds
		Buffer			scratch;			// build scratch (bottom levels, then the top level)
		Buffer			tlasStorage;
		VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
		VkDeviceSize	tlasScratch = 0;
		StructurePool	structures;
		Buffer			geometryData;		// binding 11
		Buffer			scratchStore;		// (2026-11-07, protocol 6) binding 20: the passes' own state, zero at start, kept between frames
		bool			scratchCleared = false;
		WordPool		dataWords;
		std::unordered_map<uint32_t, Geometry> geometries;
		std::vector<PendingFree> pendingFrees;
		uint64_t		triangles = 0;
		VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
		VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
		VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
		std::vector<VkPipeline> pipelines;	// per shader index (null: not loaded yet or failed)
		std::vector<bool>	pipelineTried;
		uint64_t		gpuUs[64] = {}, buildUs[64] = {}, sceneUs[64] = {}, traceUs[64] = {}, responseUs[64] = {};
		uint32_t		gpuCount = 0, buildCount = 0, sceneCount = 0, traceCount = 0, responseCount = 0;
		bool			loggedFirst = false;
		uint64_t		passUs[NVR_RT_MAX_SHADERS] = {};		// per shader: GPU time of its passes, summed until the next report
		uint32_t		passRuns[NVR_RT_MAX_SHADERS] = {};
		uint64_t		refits = 0, fullBuilds = 0;	// dynamic (actor) geometry: in-place updates / full builds
		uint64_t		duplicateBuilds = 0;		// (2026-11-06) BUILD ops skipped: a later op for the same id this frame
		VkQueryPool		compactQueries = VK_NULL_HANDLE;	// compacted sizes, kCompactPerSlot a slot (2026-10-30)
		// The arena guard (RtArenaGuard.comp, 2026-10-30): indices clamped before every build; a stale arena (sequence texel) emptied
		VkPipelineLayout guardLayout = VK_NULL_HANDLE;
		VkPipeline		guardPipeline = VK_NULL_HANDLE;
		Buffer			guardResult;		// 2 uints a slot, host visible: [0] stale indices, [1] out-of-range indices
		uint64_t		staleFrames = 0, rangeFrames = 0;
		bool			toldStale = false, toldRange = false;
		std::vector<CompactReady> compactReady;		// sizes read back, copied at the next frame
		uint64_t		compactedCount = 0, compactedBefore = 0, compactedAfter = 0;
		// Probe: the first build frame's first geometries read back from the staging (what the bottom levels were built from), logged once
		Buffer			probe;
		uint32_t		probeIds[3] = {}, probeVertices[3] = {}, probeTriangles[3] = {}, probeCount = 0;
		float			probeTransform[3][12] = {};
		int				probeSlot = -1;
		bool			probeDone = false;
		Capture			capture;			// UNOFFICIAL 2026-11-03 (claude/rt-replay): one frame's inputs to a file on request
		NvrRtFrame		frame;				// this frame's record, copied out of the ring (2 MB: on the heap via Helper)
	};

	bool CreateTlas(Helper& h) {
		VkAccelerationStructureGeometryKHR geometry = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
		geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
		geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
		VkAccelerationStructureBuildGeometryInfoKHR build = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
		build.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
		build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
		build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
		build.geometryCount = 1;
		build.pGeometries = &geometry;
		const uint32_t maxCount = NVR_RT_MAX_INSTANCES;
		VkAccelerationStructureBuildSizesInfoKHR sizes = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
		h.vk.buildSizes(h.vk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build, &maxCount, &sizes);
		if (!CreateBuffer(h.vk, sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false, h.tlasStorage)) return false;
		VkAccelerationStructureCreateInfoKHR info = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
		info.buffer = h.tlasStorage.buffer;
		info.size = sizes.accelerationStructureSize;
		info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
		VK_CHECK(h.vk.createAs(h.vk.device, &info, nullptr, &h.tlas), "vkCreateAccelerationStructureKHR (scene)");
		h.tlasScratch = sizes.buildScratchSize;
		Log("scene: up to %u instances, %llu KB structure, %llu KB scratch", maxCount, (unsigned long long)(sizes.accelerationStructureSize >> 10),
			(unsigned long long)(sizes.buildScratchSize >> 10));
		return true;
	}

	bool EnsureScratch(Helper& h, VkDeviceSize size) {
		size += h.vk.asProperties.minAccelerationStructureScratchOffsetAlignment;
		if (h.scratch.size >= size) return true;
		vkQueueWaitIdle(h.vk.queue);	// rare: the old scratch may be in use by a frame in flight
		DestroyBuffer(h.vk, h.scratch);
		return CreateBuffer(h.vk, std::max<VkDeviceSize>(size, 16ull << 20), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, h.scratch);
	}

	bool CreatePipelineLayout(Helper& h) {
		const uint32_t bindingCount = 2 + NVR_RT_PASS_IMAGES_MAX + 3;
		VkDescriptorSetLayoutBinding bindings[2 + NVR_RT_PASS_IMAGES_MAX + 3] = {};
		VkDescriptorBindingFlags flags[2 + NVR_RT_PASS_IMAGES_MAX + 3] = {};
		bindings[0] = { 0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
		bindings[1] = { 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
		for (uint32_t i = 0; i < NVR_RT_PASS_IMAGES_MAX; i++) {	// images 0..7 at 2..9, 8..11 at 12..15 (records / geometry stay 10 / 11)
			const uint32_t at = i < NVR_RT_PASS_IMAGES ? 2 + i : 4 + i;
			bindings[at] = { NvrRtPassBinding(i), VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
			flags[at] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
		}
		bindings[2 + NVR_RT_PASS_IMAGES] = { NVR_RT_BINDING_RECORDS, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
		bindings[3 + NVR_RT_PASS_IMAGES] = { NVR_RT_BINDING_GEOMETRY, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
		bindings[2 + NVR_RT_PASS_IMAGES_MAX + 2] = { NVR_RT_BINDING_SCRATCH, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };	// (protocol 6)
		VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlags = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
		bindingFlags.bindingCount = bindingCount;
		bindingFlags.pBindingFlags = flags;
		VkDescriptorSetLayoutCreateInfo setInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &bindingFlags };
		setInfo.bindingCount = bindingCount;
		setInfo.pBindings = bindings;
		VK_CHECK(vkCreateDescriptorSetLayout(h.vk.device, &setInfo, nullptr, &h.setLayout), "vkCreateDescriptorSetLayout");
		VkPipelineLayoutCreateInfo layoutInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
		layoutInfo.setLayoutCount = 1;
		layoutInfo.pSetLayouts = &h.setLayout;
		VK_CHECK(vkCreatePipelineLayout(h.vk.device, &layoutInfo, nullptr, &h.pipelineLayout), "vkCreatePipelineLayout");
		const uint32_t sets = kCmdRing * NVR_RT_MAX_PASSES;
		VkDescriptorPoolSize sizes[4] = {
			{ VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, sets },
			{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, sets },
			{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, sets * NVR_RT_PASS_IMAGES_MAX },
			{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, sets * 3 } };
		VkDescriptorPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
		poolInfo.maxSets = sets;
		poolInfo.poolSizeCount = 4;
		poolInfo.pPoolSizes = sizes;
		VK_CHECK(vkCreateDescriptorPool(h.vk.device, &poolInfo, nullptr, &h.descriptorPool), "vkCreateDescriptorPool");
		for (Slot& s : h.slots) {
			VkDescriptorSetLayout layouts[NVR_RT_MAX_PASSES];
			for (uint32_t i = 0; i < NVR_RT_MAX_PASSES; i++) layouts[i] = h.setLayout;
			VkDescriptorSetAllocateInfo alloc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
			alloc.descriptorPool = h.descriptorPool;
			alloc.descriptorSetCount = NVR_RT_MAX_PASSES;
			alloc.pSetLayouts = layouts;
			VK_CHECK(vkAllocateDescriptorSets(h.vk.device, &alloc, s.sets), "vkAllocateDescriptorSets");
		}
		return true;
	}

	// The pipeline of shader `index` (loaded from NVR's path on first use; a failure is logged once and skips its passes).
	VkPipeline Pipeline(Helper& h, uint32_t index) {
		if (index >= NVR_RT_MAX_SHADERS || index >= g_shared->shaderCount) return VK_NULL_HANDLE;
		if (h.pipelines.size() <= index) { h.pipelines.resize(index + 1, VK_NULL_HANDLE); h.pipelineTried.resize(index + 1, false); }
		if (h.pipelineTried[index]) return h.pipelines[index];
		h.pipelineTried[index] = true;
		char path[261] = {};
		memcpy(path, g_shared->shaderPaths[index], 260);
		FILE* f = nullptr;
		std::vector<uint32_t> code;
		if (fopen_s(&f, path, "rb") == 0 && f) {
			fseek(f, 0, SEEK_END);
			long n = ftell(f);
			fseek(f, 0, SEEK_SET);
			if (n > 0 && (n % 4) == 0) { code.resize((size_t)n / 4); fread(code.data(), 1, (size_t)n, f); }
			fclose(f);
		}
		if (code.empty() || code[0] != 0x07230203u) {
			Status("trace shader %u: cannot read SPIR-V from %s", index, path);
			InterlockedOr((volatile LONG*)&g_shared->shaderFailures, (LONG)(1u << index));
			return VK_NULL_HANDLE;
		}
		VkShaderModuleCreateInfo moduleInfo = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
		moduleInfo.codeSize = code.size() * 4;
		moduleInfo.pCode = code.data();
		VkShaderModule module = VK_NULL_HANDLE;
		VkResult r = vkCreateShaderModule(h.vk.device, &moduleInfo, nullptr, &module);
		VkPipeline pipeline = VK_NULL_HANDLE;
		if (r == VK_SUCCESS) {
			VkComputePipelineCreateInfo info = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
			info.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
			info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
			info.stage.module = module;
			info.stage.pName = "main";
			info.layout = h.pipelineLayout;
			r = vkCreateComputePipelines(h.vk.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
			vkDestroyShaderModule(h.vk.device, module, nullptr);
		}
		if (r != VK_SUCCESS) {
			Status("trace shader %u (%s): pipeline creation failed: %s", index, path, VkResultName(r));
			InterlockedOr((volatile LONG*)&g_shared->shaderFailures, (LONG)(1u << index));
			return VK_NULL_HANDLE;
		}
		Log("trace shader %u loaded: %s", index, path);
		h.pipelines[index] = pipeline;
		return pipeline;
	}

	// Frees geometries whose last possible user (a frame in flight) has finished.
	void ReleaseFinished(Helper& h) {
		if (h.pendingFrees.empty()) return;
		uint64_t done = 0;
		vkGetSemaphoreCounterValue(h.vk.device, h.outputReady, &done);
		auto it = std::remove_if(h.pendingFrees.begin(), h.pendingFrees.end(), [&](const PendingFree& p) {
			if (p.frame > done) return false;
			h.vk.destroyAs(h.vk.device, p.geometry.as, nullptr);
			h.structures.Free(h.vk, p.geometry.block, p.geometry.offset, p.geometry.size);
			h.dataWords.Free(p.geometry.vertexOffset, p.geometry.dataWords);
			return true;
		});
		h.pendingFrees.erase(it, h.pendingFrees.end());
	}

	void ReleaseGeometry(Helper& h, uint32_t id, uint64_t frame) {
		auto it = h.geometries.find(id);
		if (it == h.geometries.end()) return;
		h.triangles -= it->second.triangles;
		h.pendingFrees.push_back({ frame, it->second });	// frames up to this one may still trace it
		h.geometries.erase(it);
	}

	// ---------------------------------------------------------------------------------------------------------------------
	// UNOFFICIAL 2026-11-03 (claude/rt-replay) frame capture: the frame's whole input (every shared image as the passes found it,
	// the geometry data the bottom levels were built from, the scene's instances and records, the passes, their SPIR-V) and the
	// images the passes wrote, into one .nvrcap file (NvrRtCapture.h) that `--replay` runs again without the game.

	// Bytes a texel of the formats NVR shares (0: unknown, not captured)
	uint32_t TexelBytes(VkFormat f) {
		switch (f) {
			case VK_FORMAT_R8_UNORM: return 1;
			case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16_UNORM: case VK_FORMAT_R8G8_UNORM: return 2;
			case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R16G16_UNORM:
			case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_SRGB:
			case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_A2R10G10B10_UNORM_PACK32: case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return 4;
			case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R16G16B16A16_UNORM: return 8;
			case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
			default: return 0;
		}
	}

	void AddChunk(std::vector<uint8_t>& out, uint32_t tag, uint32_t index, const void* data, size_t size) {
		NvrRtCaptureChunk c = { tag, index, (uint64_t)size };
		const size_t at = out.size();
		out.resize(at + sizeof(c) + ((size + 7) & ~(size_t)7), 0);
		memcpy(out.data() + at, &c, sizeof(c));
		if (size) memcpy(out.data() + at + sizeof(c), data, size);
	}

	// A capture spec from a file (false: no such file)
	bool ReadCaptureSpec(const std::wstring& path, CaptureSpec& spec) {
		FILE* f = nullptr;
		if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
		spec = CaptureSpec();
		char line[1024];
		while (fgets(line, sizeof(line), f)) {
			char* hash = strchr(line, '#');
			if (hash) *hash = 0;
			char* key = line;
			while (*key == ' ' || *key == '\t') key++;
			char* value = key;
			while (*value && *value != ' ' && *value != '\t' && *value != '\r' && *value != '\n') value++;
			if (*value) *value++ = 0;
			while (*value == ' ' || *value == '\t') value++;
			size_t n = strlen(value);
			while (n && (value[n - 1] == '\r' || value[n - 1] == '\n' || value[n - 1] == ' ' || value[n - 1] == '\t')) value[--n] = 0;
			if (!_stricmp(key, "frame")) spec.minFrame = _strtoui64(value, nullptr, 10);
			else if (!_stricmp(key, "shader")) { spec.shader = value; std::transform(spec.shader.begin(), spec.shader.end(), spec.shader.begin(), ::tolower); }
			else if (!_stricmp(key, "skip")) spec.skip = (uint32_t)strtoul(value, nullptr, 10);
			else if (!_stricmp(key, "count")) spec.count = std::max(1u, (uint32_t)strtoul(value, nullptr, 10));
			else if (!_stricmp(key, "out") && n) {
				wchar_t wide[MAX_PATH] = {};
				MultiByteToWideChar(CP_ACP, 0, value, -1, wide, MAX_PATH);
				spec.out = wide;
			}
		}
		fclose(f);
		spec.armed = true;
		Log("capture armed from %ls: frame >= %llu, shader '%s', skip %u, count %u, out %ls", path.c_str(), (unsigned long long)spec.minFrame,
			spec.shader.c_str(), spec.skip, spec.count, spec.out.empty() ? g_exeDir.c_str() : spec.out.c_str());
		return true;
	}

	// Whether this frame (h.frame, intact) is the one to capture; counts the skips down
	bool CaptureThisFrame(Helper& h, uint64_t frameNumber) {
		CaptureSpec& spec = h.capture.spec;
		if (!spec.armed || frameNumber < spec.minFrame) return false;
		if (!spec.shader.empty()) {
			bool any = false;
			for (uint32_t p = 0; p < h.frame.passCount && !any; p++) {
				const uint32_t shader = h.frame.passes[p].shader;
				if (shader >= NVR_RT_MAX_SHADERS) continue;
				char path[261] = {};
				memcpy(path, g_shared->shaderPaths[shader], 260);
				for (char* c = path; *c; c++) *c = (char)tolower((unsigned char)*c);
				any = strstr(path, spec.shader.c_str()) != nullptr;
			}
			if (!any) return false;
		}
		if (spec.skip) { spec.skip--; return false; }
		return true;
	}

	// In RecordFrame, after the scene build: the capture buffer, the "before" copies, the CPU chunks. False: not captured (logged).
	bool CaptureBefore(Helper& h, Slot& s, VkCommandBuffer cmd, uint32_t instanceCount) {
		Capture& c = h.capture;
		const NvrRtFrame& f = h.frame;
		c.regions.clear();
		c.meta.clear();
		uint32_t bound[NVR_RT_MAX_IMAGES] = {}, written[NVR_RT_MAX_IMAGES] = {};
		for (uint32_t p = 0; p < f.passCount; p++)
			for (uint32_t i = 0; i < std::min<uint32_t>(f.passes[p].imageCount, NVR_RT_PASS_IMAGES_MAX); i++) {
				const uint32_t image = NvrRtPassImage(f.passes[p], i);
				if (image >= NVR_RT_MAX_IMAGES) continue;
				bound[image] = 1;
				if (f.passes[p].writeMask & (1u << i)) written[image] = 1;
			}
		VkDeviceSize total = 0;
		for (uint32_t i = 0; i < h.imageCount; i++) {
			const SharedImage& im = h.images[i];
			NvrRtCaptureImage d = { (uint32_t)im.format, im.width, im.height, TexelBytes(im.format), 0, im.usage };
			if (bound[i]) d.flags |= NVR_RT_CAPTURE_IMAGE_BOUND;
			const bool copy = d.texelBytes && im.image && (i != NVR_RT_ARENA_IMAGE || bound[i]);	// the arena (64 MB) only when a pass reads it
			if (copy) {
				const VkDeviceSize size = (VkDeviceSize)im.width * im.height * d.texelBytes;
				d.flags |= NVR_RT_CAPTURE_IMAGE_BEFORE;
				c.regions.push_back({ NVR_RT_CHUNK_IMAGE_BEFORE, i, total, size });
				total += AlignUp(size, 256);
				if (written[i]) {
					d.flags |= NVR_RT_CAPTURE_IMAGE_AFTER;
					c.regions.push_back({ NVR_RT_CHUNK_IMAGE_AFTER, i, total, size });
					total += AlignUp(size, 256);
				}
			}
			else d.flags |= NVR_RT_CAPTURE_IMAGE_NO_COPY;
			AddChunk(c.meta, NVR_RT_CHUNK_IMAGE_DESC, i, &d, sizeof(d));
		}
		// every geometry alive (in id order: the same file for the same scene), its words copied in that order
		std::vector<NvrRtCaptureGeometry> geos;
		geos.reserve(h.geometries.size());
		uint32_t noData = 0;
		for (const auto& kv : h.geometries) {
			const Geometry& g = kv.second;
			NvrRtCaptureGeometry cg = { kv.first, g.vertices, g.triangles, g.vertexOffset, g.indexOffset, g.uvOffset, g.dataWords, g.opFlags,
				g.compacted ? 1u : 0u, 0 };
			if (g.vertexOffset == NVR_RT_NO_DATA || !g.dataWords) { cg.vertexOffset = cg.indexOffset = cg.uvOffset = NVR_RT_NO_DATA; cg.dataWords = 0; noData++; }
			geos.push_back(cg);
		}
		std::sort(geos.begin(), geos.end(), [](const NvrRtCaptureGeometry& a, const NvrRtCaptureGeometry& b) { return a.id < b.id; });
		std::vector<VkBufferCopy> dataCopies;
		VkDeviceSize dataBytes = 0;
		for (const NvrRtCaptureGeometry& g : geos) {
			if (!g.dataWords) continue;
			dataCopies.push_back({ (VkDeviceSize)g.vertexOffset * 4, total + dataBytes, (VkDeviceSize)g.dataWords * 4 });
			dataBytes += (VkDeviceSize)g.dataWords * 4;
		}
		c.regions.push_back({ NVR_RT_CHUNK_GEOMETRY_DATA, 0, total, dataBytes });
		total += AlignUp(dataBytes, 256);
		if (!CreateBuffer(h.vk, total, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, c.buffer)) {
			Log("capture: no %llu MB host-visible buffer: frame not captured", (unsigned long long)(total >> 20));
			c.buffer = Buffer();
			return false;
		}
		for (const CaptureRegion& r : c.regions) {
			if (r.tag != NVR_RT_CHUNK_IMAGE_BEFORE) continue;
			const SharedImage& im = h.images[r.index];
			VkBufferImageCopy copy = {};
			copy.bufferOffset = r.offset;
			copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			copy.imageExtent = { im.width, im.height, 1 };
			vkCmdCopyImageToBuffer(cmd, im.image, VK_IMAGE_LAYOUT_GENERAL, c.buffer.buffer, 1, &copy);
		}
		if (!dataCopies.empty()) vkCmdCopyBuffer(cmd, h.geometryData.buffer, c.buffer.buffer, (uint32_t)dataCopies.size(), dataCopies.data());
		FullBarrier(cmd);	// the passes may write what was just copied

		AddChunk(c.meta, NVR_RT_CHUNK_GEOMETRIES, 0, geos.data(), geos.size() * sizeof(NvrRtCaptureGeometry));
		// the scene as built: the instance buffer's entries (the geometry id from the record next to it) and the records as bound
		std::vector<NvrRtInstance> instances(instanceCount);
		const VkAccelerationStructureInstanceKHR* built = (const VkAccelerationStructureInstanceKHR*)s.instances.mapped;
		const NvrRtInstanceRecord* records = (const NvrRtInstanceRecord*)s.records.mapped;
		for (uint32_t i = 0; i < instanceCount; i++) {
			memcpy(instances[i].transform, &built[i].transform, sizeof(instances[i].transform));
			instances[i].geometryId = records[i].geometryId;
			instances[i].customIndex = built[i].instanceCustomIndex;
			instances[i].mask = built[i].mask;
			instances[i].flags = built[i].flags;
		}
		AddChunk(c.meta, NVR_RT_CHUNK_INSTANCES, 0, instances.data(), instances.size() * sizeof(NvrRtInstance));
		AddChunk(c.meta, NVR_RT_CHUNK_RECORDS, 0, records, (size_t)instanceCount * sizeof(NvrRtInstanceRecord));
		AddChunk(c.meta, NVR_RT_CHUNK_PASSES, 0, f.passes, (size_t)f.passCount * sizeof(NvrRtPass));
		const uint32_t shaderCount = std::min<uint32_t>((uint32_t)g_shared->shaderCount, NVR_RT_MAX_SHADERS);
		for (uint32_t i = 0; i < shaderCount; i++) {
			char path[261] = {};
			memcpy(path, g_shared->shaderPaths[i], 260);
			AddChunk(c.meta, NVR_RT_CHUNK_SHADER_PATH, i, path, strlen(path) + 1);
			FILE* sf = nullptr;
			if (fopen_s(&sf, path, "rb") == 0 && sf) {
				std::vector<uint8_t> code;
				fseek(sf, 0, SEEK_END);
				const long n = ftell(sf);
				fseek(sf, 0, SEEK_SET);
				if (n > 0) { code.resize((size_t)n); code.resize(fread(code.data(), 1, (size_t)n, sf)); }
				fclose(sf);
				AddChunk(c.meta, NVR_RT_CHUNK_SHADER_CODE, i, code.data(), code.size());
			}
		}
		NvrRtCaptureHeader& hd = c.header;
		hd = NvrRtCaptureHeader();
		hd.magic = NVR_RT_CAPTURE_MAGIC;
		hd.version = NVR_RT_CAPTURE_VERSION;
		hd.protocol = NVR_RT_PROTOCOL_VERSION;
		hd.headerSize = sizeof(NvrRtCaptureHeader);
		hd.frame = f.frame;
		hd.imageCount = h.imageCount;
		hd.geometryCount = (uint32_t)geos.size();
		hd.instanceCount = instanceCount;
		hd.passCount = f.passCount;
		hd.shaderCount = shaderCount;
		hd.dataWords = kDataWords;
		hd.noDataGeometries = noData;
		strncpy_s(hd.device, h.vk.properties.deviceName, _TRUNCATE);
		return true;
	}

	// In RecordFrame, after the passes: the written images as the game gets them
	void CaptureAfter(Helper& h, VkCommandBuffer cmd) {
		Capture& c = h.capture;
		for (const CaptureRegion& r : c.regions) {
			if (r.tag != NVR_RT_CHUNK_IMAGE_AFTER) continue;
			const SharedImage& im = h.images[r.index];
			VkBufferImageCopy copy = {};
			copy.bufferOffset = r.offset;
			copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			copy.imageExtent = { im.width, im.height, 1 };
			vkCmdCopyImageToBuffer(cmd, im.image, VK_IMAGE_LAYOUT_GENERAL, c.buffer.buffer, 1, &copy);
		}
		VkMemoryBarrier b = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
		b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		b.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
	}

	// After the captured frame's submit: wait for it, then the file is written on a thread of its own (the game's next frames go on)
	void CaptureFinish(Helper& h, Slot& s, uint64_t frameNumber) {
		Capture& c = h.capture;
		c.active = false;
		VkSemaphoreWaitInfo wait = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
		wait.semaphoreCount = 1;
		wait.pSemaphores = &h.outputReady;
		wait.pValues = &frameNumber;
		const VkResult r = vkWaitSemaphores(h.vk.device, &wait, 10ull * 1000 * 1000 * 1000);
		if (r != VK_SUCCESS) { Log("capture: frame %llu did not finish in 10 s (%s): not written", (unsigned long long)frameNumber, VkResultName(r)); DestroyBuffer(h.vk, c.buffer); return; }
		// this frame's own pass times (the same timestamps WaitSlot reads later)
		std::vector<float> passMs;
		if (h.queries) {
			const uint32_t slot = (uint32_t)(&s - h.slots);
			uint64_t stamps[kQueriesPerSlot] = {};
			if (vkGetQueryPoolResults(h.vk.device, h.queries, slot * kQueriesPerSlot, kQueriesPerSlot, sizeof(stamps), stamps, sizeof(uint64_t),
				VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
				const uint64_t mask = h.vk.timestampBits >= 64 ? ~0ull : ((1ull << h.vk.timestampBits) - 1);
				for (uint32_t p = 0; p < s.passesStamped; p++)
					passMs.push_back((float)((double)((stamps[4 + p] - stamps[p == 0 ? 2 : 4 + p - 1]) & mask) * h.vk.properties.limits.timestampPeriod / 1e6));
			}
		}
		AddChunk(c.meta, NVR_RT_CHUNK_PASS_TIMES, 0, passMs.data(), passMs.size() * sizeof(float));
		size_t bytes = sizeof(NvrRtCaptureHeader) + c.meta.size();
		for (const CaptureRegion& rg : c.regions) bytes += sizeof(NvrRtCaptureChunk) + (size_t)AlignUp(rg.size, 8);
		std::vector<uint8_t>* file = new std::vector<uint8_t>();
		file->reserve(bytes);
		file->insert(file->end(), (const uint8_t*)&c.header, (const uint8_t*)&c.header + sizeof(c.header));
		file->insert(file->end(), c.meta.begin(), c.meta.end());
		for (const CaptureRegion& rg : c.regions) AddChunk(*file, rg.tag, rg.index, (const uint8_t*)c.buffer.mapped + rg.offset, (size_t)rg.size);
		DestroyBuffer(h.vk, c.buffer);
		c.meta.clear();
		c.regions.clear();
		const std::wstring folder = c.spec.out.empty() ? g_exeDir : c.spec.out;
		CreateDirectoryW(folder.c_str(), nullptr);
		wchar_t name[64];
		swprintf_s(name, L"\\NvrRtCapture-%llu.nvrcap", (unsigned long long)frameNumber);
		const std::wstring path = folder + name;
		std::string passes;
		for (uint32_t p = 0; p < c.header.passCount; p++) {
			const NvrRtPass& pass = h.frame.passes[p];
			const char* sp = pass.shader < NVR_RT_MAX_SHADERS ? (const char*)g_shared->shaderPaths[pass.shader] : "?";
			const char* nm = strrchr(sp, '\\');
			char one[160];
			sprintf_s(one, " %s%.40s (%ux%u groups, %u images, %.3f ms)", p ? "," : "", nm ? nm + 1 : sp, pass.groupsX, pass.groupsY, pass.imageCount,
				p < passMs.size() ? passMs[p] : -1.0f);
			passes += one;
		}
		Log("capture of frame %llu: %u images, %u geometries (%u without data), %u instances, %u passes:%s; %llu MB -> %ls (writing)",
			(unsigned long long)frameNumber, c.header.imageCount, c.header.geometryCount, c.header.noDataGeometries, c.header.instanceCount,
			c.header.passCount, passes.c_str(), (unsigned long long)(file->size() >> 20), path.c_str());
		std::thread([file, path]() {
			FILE* out = nullptr;
			bool ok = false;
			if (_wfopen_s(&out, path.c_str(), L"wb") == 0 && out) { ok = fwrite(file->data(), 1, file->size(), out) == file->size(); ok = fclose(out) == 0 && ok; }
			Status("capture %ls: %s (%llu MB)", path.c_str(), ok ? "written" : "FAILED to write", (unsigned long long)(file->size() >> 20));
			delete file;
		}).detach();
		c.written++;
		if (c.spec.count > 1) c.spec.count--;
		else c.spec.armed = false;
	}

	bool WaitSlot(Helper& h, Slot& s, HANDLE parent) {
		if (!s.frame) return true;
		VkSemaphoreWaitInfo wait = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
		wait.semaphoreCount = 1;
		wait.pSemaphores = &h.outputReady;
		wait.pValues = &s.frame;
		for (;;) {
			VkResult r = vkWaitSemaphores(h.vk.device, &wait, 250ull * 1000 * 1000);
			if (r == VK_SUCCESS) break;
			if (r != VK_TIMEOUT) { Status("Vulkan: waiting for an earlier frame failed: %s", VkResultName(r)); return false; }
			if (WaitForSingleObject(parent, 0) == WAIT_OBJECT_0 || g_shared->quit) return false;
		}
		if (h.queries) {
			const uint32_t slot = (uint32_t)(&s - h.slots);
			uint64_t stamps[kQueriesPerSlot] = {};
			if (vkGetQueryPoolResults(h.vk.device, h.queries, slot * kQueriesPerSlot, kQueriesPerSlot, sizeof(stamps), stamps, sizeof(uint64_t),
				VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
				const uint64_t mask = h.vk.timestampBits >= 64 ? ~0ull : ((1ull << h.vk.timestampBits) - 1);
				auto us = [&](uint32_t a, uint32_t b) { return (uint64_t)((double)((stamps[b] - stamps[a]) & mask) * h.vk.properties.limits.timestampPeriod / 1000.0); };
				const uint64_t total = us(0, 3);
				h.gpuUs[h.gpuCount++ % 64] = total;
				g_shared->gpuTimeUsLast = (uint32_t)total;
				g_shared->gpuTimeUsAvg = Average(h.gpuUs, h.gpuCount);
				if (s.measured[0]) { h.buildUs[h.buildCount++ % 64] = us(0, 1); g_shared->buildUsAvg = Average(h.buildUs, h.buildCount); }
				if (s.measured[1]) { h.sceneUs[h.sceneCount++ % 64] = us(1, 2); g_shared->sceneUsAvg = Average(h.sceneUs, h.sceneCount); }
				if (s.measured[2]) { h.traceUs[h.traceCount++ % 64] = us(2, 3); g_shared->traceUsAvg = Average(h.traceUs, h.traceCount); }
				for (uint32_t p = 0; p < s.passesStamped && p < NVR_RT_MAX_PASSES; p++) {
					const uint32_t shader = s.passShaders[p];
					if (shader >= NVR_RT_MAX_SHADERS) continue;
					h.passUs[shader] += us(p == 0 ? 2 : 4 + p - 1, 4 + p);
					h.passRuns[shader]++;
				}
			}
		}
		if (h.guardResult.mapped) {
			uint32_t* counts = (uint32_t*)h.guardResult.mapped + (&s - h.slots) * 2;
			if (counts[0]) {
				h.staleFrames++;
				if (!h.toldStale) {
					h.toldStale = true;
					Status("the arena held an earlier flush's data in frame %llu (%u indices): its new geometry was built empty instead of from stale "
						"indices. The d3d9.dll orders its copy wrongly: use the NVR DXVK fork with 1.8-rt-seq (patch 0034) or later. Logged once.",
						(unsigned long long)s.frame, counts[0]);
				}
			}
			if (counts[1]) {
				h.rangeFrames++;
				if (!h.toldRange) {
					h.toldRange = true;
					Status("frame %llu: %u triangle indices past their geometry's vertex count were set to 0 before the build (logged once)",
						(unsigned long long)s.frame, counts[1]);
				}
			}
			counts[0] = counts[1] = 0;
		}
		if (h.compactQueries && !s.compactAsked.empty()) {
			const uint32_t slot = (uint32_t)(&s - h.slots);
			std::vector<uint64_t> sizes(s.compactAsked.size());
			if (vkGetQueryPoolResults(h.vk.device, h.compactQueries, slot * kCompactPerSlot, (uint32_t)sizes.size(), sizes.size() * sizeof(uint64_t),
				sizes.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
				for (size_t i = 0; i < sizes.size(); i++)
					if (sizes[i]) h.compactReady.push_back({ s.compactAsked[i].first, s.compactAsked[i].second, sizes[i] });
			s.compactAsked.clear();
		}
		if (h.probeSlot == (int)(&s - h.slots) && h.probe.mapped) {
			for (uint32_t k = 0; k < h.probeCount; k++) {
				const float* v = (const float*)((const uint8_t*)h.probe.mapped + k * 4096);
				const uint32_t* t = (const uint32_t*)((const uint8_t*)h.probe.mapped + k * 4096 + 3072);
				const uint32_t n = std::min<uint32_t>(h.probeVertices[k], 256);
				float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
				uint32_t bad = 0, maxIndex = 0;
				for (uint32_t i = 0; i < n; i++) for (int a = 0; a < 3; a++) {
					const float x = v[3 * i + a];
					if (!(x == x) || fabsf(x) > 1e7f) bad++;
					lo[a] = std::min(lo[a], x); hi[a] = std::max(hi[a], x);
				}
				for (uint32_t i = 0; i < std::min<uint32_t>(3 * h.probeTriangles[k], 256); i++) maxIndex = std::max(maxIndex, t[i]);
				const float* m = h.probeTransform[k];
				Log("probe geometry %u: %u vertices, %u triangles; first %.1f %.1f %.1f / %.1f %.1f %.1f; bounds (first %u) %.1f %.1f %.1f .. %.1f %.1f %.1f, %u bad; "
					"indices %u %u %u %u %u %u (max %u); transform [%.3f %.3f %.3f %.1f | %.3f %.3f %.3f %.1f | %.3f %.3f %.3f %.1f]",
					h.probeIds[k], h.probeVertices[k], h.probeTriangles[k], v[0], v[1], v[2], v[3], v[4], v[5], n, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2], bad,
					t[0], t[1], t[2], t[3], t[4], t[5], maxIndex, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11]);
			}
			h.probeSlot = -1;
		}
		return true;
	}

#include "NvrRtShadowDenoise.inl"	// UNOFFICIAL 2026-11-07 the RT shadow denoiser module (docs/next/RT-DENOISER.md)

	// Records this frame's work into s.cmd (open). False: something could not be recorded (logged); the caller still submits.
	bool RecordFrame(Helper& h, Slot& s, uint64_t frameNumber) {
		const NvrRtFrame& f = h.frame;
		const uint32_t slot = (uint32_t)(&s - h.slots);
		VkCommandBuffer cmd = s.cmd;
		s.measured[0] = s.measured[1] = s.measured[2] = false;
		s.passesStamped = 0;
		FullBarrier(cmd);	// the previous frame's builds / passes and the game's writes (the semaphore makes them visible)

		// 0. compaction (NVR_RT_OP_FLAG_COMPACT): the structures whose compacted size came back are copied into one of that size; the
		//    old one is freed once this frame is done (its data words stay with the geometry). Rebuilt or released since: skipped.
		bool compactedNow = false;
		for (const CompactReady& c : h.compactReady) {
			auto it = h.geometries.find(c.id);
			if (it == h.geometries.end() || it->second.as != c.as || it->second.compacted || c.size >= it->second.size) continue;
			Geometry& geo = it->second;
			Geometry fresh = geo;
			if (!h.structures.Allocate(h.vk, c.size, fresh.block, fresh.offset)) break;
			fresh.size = c.size;
			VkAccelerationStructureCreateInfoKHR info = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
			info.buffer = h.structures.blocks[fresh.block].buffer.buffer;
			info.offset = fresh.offset;
			info.size = fresh.size;
			info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
			if (h.vk.createAs(h.vk.device, &info, nullptr, &fresh.as) != VK_SUCCESS) { h.structures.Free(h.vk, fresh.block, fresh.offset, fresh.size); continue; }
			VkCopyAccelerationStructureInfoKHR copy = { VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR };
			copy.src = geo.as;
			copy.dst = fresh.as;
			copy.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
			h.vk.cmdCopyAs(cmd, &copy);
			VkAccelerationStructureDeviceAddressInfoKHR addressInfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
			addressInfo.accelerationStructure = fresh.as;
			fresh.address = h.vk.asAddress(h.vk.device, &addressInfo);
			fresh.compacted = true;
			Geometry old = geo;
			old.vertexOffset = NVR_RT_NO_DATA;	// the data words move to the compacted one
			old.dataWords = 0;
			h.pendingFrees.push_back({ frameNumber, old });
			h.compactedCount++;
			h.compactedBefore += geo.size;
			h.compactedAfter += fresh.size;
			geo = fresh;
			compactedNow = true;
		}
		h.compactReady.clear();
		if (compactedNow) FullBarrier(cmd);

		// 1. new geometry: the arena's rows into the staging buffer, then every build in one call
		std::vector<VkAccelerationStructureBuildGeometryInfoKHR> infos;
		std::vector<VkAccelerationStructureGeometryKHR> geoms;
		std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
		std::vector<uint32_t> builtIds;
		const SharedImage& arena = h.images[NVR_RT_ARENA_IMAGE];
		uint32_t builds = 0;
		for (uint32_t i = 0; i < f.opCount; i++) if (f.ops[i].op == NVR_RT_OP_BUILD) builds++;
		const bool sequence = (f.arenaTexels & NVR_RT_ARENA_SEQUENCE) != 0;
		const uint32_t arenaTexels = f.arenaTexels & ~NVR_RT_ARENA_SEQUENCE;
		if (builds && arenaTexels) {
			const uint64_t capacity = (uint64_t)arena.width * arena.height;
			const uint32_t rows = (uint32_t)std::min<uint64_t>((arenaTexels + (sequence ? 1u : 0u) + arena.width - 1) / arena.width, arena.height);
			VkBufferImageCopy copy = {};
			copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			copy.imageExtent = { arena.width, rows, 1 };
			vkCmdCopyImageToBuffer(cmd, arena.image, VK_IMAGE_LAYOUT_GENERAL, h.staging.buffer, 1, &copy);
			FullBarrier(cmd);
			if (h.guardPipeline && h.guardResult.buffer) {
				struct { uint64_t staging, result; uint32_t first, count, vertices, check, seqTexel, seq; } push = {};
				push.staging = h.staging.address;
				push.result = h.guardResult.address + (uint64_t)slot * 8;
				push.check = sequence ? 1u : 0u;
				push.seqTexel = arenaTexels;
				push.seq = NvrRtSequenceOf(f.frame);
				vkCmdFillBuffer(cmd, h.guardResult.buffer, (VkDeviceSize)slot * 8, 8, 0);
				FullBarrier(cmd);
				vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, h.guardPipeline);
				for (uint32_t i = 0; i < f.opCount; i++) {
					const NvrRtGeometryOp& op = f.ops[i];
					if (op.op != NVR_RT_OP_BUILD || !op.vertexCount || !op.triangleCount) continue;
					if ((uint64_t)op.arenaTexel + 3ull * ((uint64_t)op.vertexCount + op.triangleCount) > std::min<uint64_t>(capacity, arenaTexels)) continue;
					push.first = op.arenaTexel + 3u * op.vertexCount;
					push.count = 3u * op.triangleCount;
					push.vertices = op.vertexCount;
					vkCmdPushConstants(cmd, h.guardLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
					vkCmdDispatch(cmd, (push.count + 63u) / 64u, 1u, 1u);
				}
				FullBarrier(cmd);
			}

			geoms.reserve(builds);
			infos.reserve(builds);
			ranges.reserve(builds);
			VkDeviceSize scratchTotal = 0;
			std::vector<VkDeviceSize> scratchOffsets;
			std::vector<VkBufferCopy> dataCopies;
			const VkDeviceSize align = h.vk.asProperties.minAccelerationStructureScratchOffsetAlignment;
			// (2026-11-06, the NVIDIA device-lost crashes) one build per geometry id a frame: the game can send the same id twice in a
			// frame (an actor partition met twice), and both then went into the one vkCmdBuildAccelerationStructuresKHR call with the same
			// destination (a full build and a refit of it, or two refits): VUID-...-dstAccelerationStructure-03698, found by the Khronos
			// validation layer (NvrRtHelper.validate). AMD's builder survived it; NVIDIA's lost the device. Only the LAST op for an id is
			// built (the newest pose / data); the earlier ones are skipped (counted in h.duplicateBuilds, logged every 600 frames).
			std::unordered_map<uint32_t, uint32_t> lastBuildOf;
			for (uint32_t i = 0; i < f.opCount; i++)
				if (f.ops[i].op == NVR_RT_OP_BUILD && f.ops[i].id) lastBuildOf[f.ops[i].id] = i;
			for (uint32_t i = 0; i < f.opCount; i++) {
				const NvrRtGeometryOp& op = f.ops[i];
				if (op.op != NVR_RT_OP_BUILD) continue;
				if (op.id) { auto last = lastBuildOf.find(op.id); if (last != lastBuildOf.end() && last->second != i) { h.duplicateBuilds++; continue; } }
				const bool uv = (op.flags & NVR_RT_OP_FLAG_TEXCOORDS) != 0;
				const uint64_t texels = 3ull * ((uint64_t)op.vertexCount + op.triangleCount) + (uv ? 2ull * op.vertexCount : 0ull);
				if (!op.id || !op.vertexCount || !op.triangleCount || (uint64_t)op.arenaTexel + texels > std::min<uint64_t>(capacity, arenaTexels) ||
					op.triangleCount > h.vk.asProperties.maxPrimitiveCount) {
					g_shared->failedOps++;
					Log("geometry %u refused (%u vertices, %u triangles at texel %u, arena %u texels)", op.id, op.vertexCount, op.triangleCount, op.arenaTexel, arenaTexels);
					continue;
				}
				const bool dynamic = (op.flags & NVR_RT_OP_FLAG_DYNAMIC) != 0;
				auto existing = h.geometries.find(op.id);
				// Actors: the same partition again (same counts) is refit in place (UPDATE: far cheaper than a build; the topology is the
				// same, only the posed positions move). Anything else is rebuilt under the same id: the old one goes.
				// ... and only when it keeps its texcoords the same (a partition first built before its texture had an atlas slot comes
				// back with texcoords: a refit would keep the old data without them; rtsun-25: textured actors stayed grey)
				const bool refit = dynamic && !(op.flags & NVR_RT_OP_FLAG_REBUILD) && existing != h.geometries.end() && existing->second.dynamic &&
					existing->second.vertices == op.vertexCount && existing->second.triangles == op.triangleCount &&
					(existing->second.uvOffset != NVR_RT_NO_DATA) == ((op.flags & NVR_RT_OP_FLAG_TEXCOORDS) != 0);
				if (!refit) ReleaseGeometry(h, op.id, frameNumber);
				VkAccelerationStructureGeometryKHR g = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
				g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
				g.flags = (op.flags & NVR_RT_OP_FLAG_ALPHA) ? 0u : VK_GEOMETRY_OPAQUE_BIT_KHR;	// alpha-tested: the rays see the candidates
				g.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
				g.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
				g.geometry.triangles.vertexData.deviceAddress = h.staging.address + (VkDeviceSize)op.arenaTexel * 4;
				g.geometry.triangles.vertexStride = 12;
				g.geometry.triangles.maxVertex = op.vertexCount - 1;
				g.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
				g.geometry.triangles.indexData.deviceAddress = h.staging.address + ((VkDeviceSize)op.arenaTexel + 3ull * op.vertexCount) * 4;
				VkAccelerationStructureBuildGeometryInfoKHR b = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
				b.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
				b.flags = dynamic ? (VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR)
					: VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;	// actors: posed every frame, refit after the first build
				const bool compact = !dynamic && (op.flags & NVR_RT_OP_FLAG_COMPACT) && h.compactQueries && s.compactAsked.size() < kCompactPerSlot;
				if (compact) b.flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
				b.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
				b.geometryCount = 1;
				b.pGeometries = &g;
				VkAccelerationStructureBuildSizesInfoKHR sizes = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
				h.vk.buildSizes(h.vk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &b, &op.triangleCount, &sizes);
				if (refit) {
					Geometry& old = existing->second;
					b.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
					b.srcAccelerationStructure = old.as;
					b.dstAccelerationStructure = old.as;
					if (old.vertexOffset != NVR_RT_NO_DATA)	// the posed positions for the shaders too (the triangle list is the same)
						dataCopies.push_back({ (VkDeviceSize)op.arenaTexel * 4, (VkDeviceSize)old.vertexOffset * 4, (VkDeviceSize)op.vertexCount * 12 });
					geoms.push_back(g);
					infos.push_back(b);
					ranges.push_back({ op.triangleCount, 0, 0, 0 });
					scratchOffsets.push_back(scratchTotal);
					scratchTotal += AlignUp(sizes.updateScratchSize, align);
					h.refits++;
					continue;
				}
				if (dynamic) h.fullBuilds++;
				Geometry geo;
				geo.vertices = op.vertexCount;
				geo.dynamic = dynamic;
				geo.opFlags = op.flags;
				geo.size = sizes.accelerationStructureSize;
				geo.triangles = op.triangleCount;
				if (!h.structures.Allocate(h.vk, geo.size, geo.block, geo.offset)) { g_shared->failedOps++; continue; }
				VkAccelerationStructureCreateInfoKHR info = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
				info.buffer = h.structures.blocks[geo.block].buffer.buffer;
				info.offset = geo.offset;
				info.size = geo.size;
				info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
				if (h.vk.createAs(h.vk.device, &info, nullptr, &geo.as) != VK_SUCCESS) {
					h.structures.Free(h.vk, geo.block, geo.offset, geo.size);
					g_shared->failedOps++;
					continue;
				}
				VkAccelerationStructureDeviceAddressInfoKHR addressInfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
				addressInfo.accelerationStructure = geo.as;
				geo.address = h.vk.asAddress(h.vk.device, &addressInfo);
				// its positions and triangle list kept for the shaders (binding 11), when there is room
				geo.dataWords = (uint32_t)texels;
				geo.vertexOffset = h.dataWords.Allocate(geo.dataWords);
				if (geo.vertexOffset != NVR_RT_NO_DATA) {
					geo.indexOffset = geo.vertexOffset + 3u * op.vertexCount;
					if (uv) geo.uvOffset = geo.indexOffset + 3u * op.triangleCount;
					dataCopies.push_back({ (VkDeviceSize)op.arenaTexel * 4, (VkDeviceSize)geo.vertexOffset * 4, (VkDeviceSize)texels * 4 });
				}
				else geo.dataWords = 0;
				h.geometries[op.id] = geo;
				h.triangles += geo.triangles;
				if (compact) s.compactAsked.push_back({ op.id, geo.as });
				geoms.push_back(g);
				b.dstAccelerationStructure = geo.as;
				infos.push_back(b);
				ranges.push_back({ op.triangleCount, 0, 0, 0 });
				scratchOffsets.push_back(scratchTotal);
				scratchTotal += AlignUp(sizes.buildScratchSize, align);
			}
			if (!infos.empty()) {
				if (!EnsureScratch(h, std::max(scratchTotal, h.tlasScratch))) return false;
				const uint64_t base = AlignUp(h.scratch.address, align);
				std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> rangePointers(infos.size());
				for (size_t i = 0; i < infos.size(); i++) {
					infos[i].pGeometries = &geoms[i];
					infos[i].scratchData.deviceAddress = base + scratchOffsets[i];
					rangePointers[i] = &ranges[i];
				}
				h.vk.cmdBuildAs(cmd, (uint32_t)infos.size(), infos.data(), rangePointers.data());
				if (!dataCopies.empty()) vkCmdCopyBuffer(cmd, h.staging.buffer, h.geometryData.buffer, (uint32_t)dataCopies.size(), dataCopies.data());
				if (!h.probeDone && h.probe.buffer) {
					for (uint32_t i = 0; i < f.opCount && h.probeCount < 3; i++) {
						const NvrRtGeometryOp& op = f.ops[i];
						if (op.op != NVR_RT_OP_BUILD || h.geometries.find(op.id) == h.geometries.end()) continue;
						const uint32_t k = h.probeCount++;
						h.probeIds[k] = op.id; h.probeVertices[k] = op.vertexCount; h.probeTriangles[k] = op.triangleCount;
						VkBufferCopy c[2] = {
							{ (VkDeviceSize)op.arenaTexel * 4, (VkDeviceSize)k * 4096, std::min<VkDeviceSize>(3ull * op.vertexCount, 768) * 4 },
							{ ((VkDeviceSize)op.arenaTexel + 3ull * op.vertexCount) * 4, (VkDeviceSize)k * 4096 + 3072, std::min<VkDeviceSize>(3ull * op.triangleCount, 256) * 4 } };
						vkCmdCopyBuffer(cmd, h.staging.buffer, h.probe.buffer, 2, c);
					}
					h.probeSlot = (int)slot;
					h.probeDone = true;
				}
				FullBarrier(cmd);
				if (!s.compactAsked.empty()) {	// the compacted sizes, read back once this frame is done (WaitSlot)
					std::vector<VkAccelerationStructureKHR> asked;
					for (const auto& a : s.compactAsked) asked.push_back(a.second);
					vkCmdResetQueryPool(cmd, h.compactQueries, slot * kCompactPerSlot, (uint32_t)asked.size());
					h.vk.cmdWriteAsProperties(cmd, (uint32_t)asked.size(), asked.data(), VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR,
						h.compactQueries, slot * kCompactPerSlot);
				}
				s.measured[0] = true;
			}
		}
		for (uint32_t i = 0; i < f.opCount; i++)
			if (f.ops[i].op == NVR_RT_OP_RELEASE) ReleaseGeometry(h, f.ops[i].id, frameNumber);
		if (h.queries) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, h.queries, slot * kQueriesPerSlot + 1);

		// 2. the scene from this frame's instances
		VkAccelerationStructureInstanceKHR* out = (VkAccelerationStructureInstanceKHR*)s.instances.mapped;
		NvrRtInstanceRecord* records = (NvrRtInstanceRecord*)s.records.mapped;
		uint32_t count = 0;
		for (uint32_t i = 0; i < std::min<uint32_t>(f.instanceCount, NVR_RT_MAX_INSTANCES); i++) {
			const NvrRtInstance& in = f.instances[i];
			auto it = h.geometries.find(in.geometryId);
			if (it == h.geometries.end()) continue;
			NvrRtInstanceRecord& r = records[count];
			r = f.records[i];
			r.geometryId = in.geometryId;
			r.vertexOffset = it->second.vertexOffset;
			r.indexOffset = it->second.indexOffset;
			if (r.flags & NVR_RT_RECORD_ALPHA_TESTED) {
				if (it->second.uvOffset == NVR_RT_NO_DATA) continue;	// no texcoords (no room in the data): it would block as a solid quad
				memcpy(&r.albedo[3], &it->second.uvOffset, 4);
			}
			else if (r.flags & NVR_RT_RECORD_TEXTURED) {	// hit textures: the texcoords' offset, or no texture when there was no room
				if (it->second.uvOffset == NVR_RT_NO_DATA) r.flags &= ~NVR_RT_RECORD_TEXTURED;
				else memcpy(&r.albedo[3], &it->second.uvOffset, 4);
			}
			VkAccelerationStructureInstanceKHR& o = out[count++];
			memcpy(&o.transform, in.transform, sizeof(o.transform));
			o.instanceCustomIndex = in.customIndex & 0xFFFFFF;
			o.mask = in.mask & 0xFF;
			o.instanceShaderBindingTableRecordOffset = 0;
			o.flags = in.flags & 0xFF;
			o.accelerationStructureReference = it->second.address;
		}
		g_shared->lastInstances = count;
		if (h.probeSlot == (int)slot)
			for (uint32_t k = 0; k < h.probeCount; k++)
				for (uint32_t i = 0; i < std::min<uint32_t>(f.instanceCount, NVR_RT_MAX_INSTANCES); i++)
					if (f.instances[i].geometryId == h.probeIds[k]) { memcpy(h.probeTransform[k], f.instances[i].transform, 48); break; }
		if (!EnsureScratch(h, h.tlasScratch)) return false;
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
			b.scratchData.deviceAddress = AlignUp(h.scratch.address, h.vk.asProperties.minAccelerationStructureScratchOffsetAlignment);
			VkAccelerationStructureBuildRangeInfoKHR range = { count, 0, 0, 0 };
			const VkAccelerationStructureBuildRangeInfoKHR* rangePointer = &range;
			h.vk.cmdBuildAs(cmd, 1, &b, &rangePointer);
			FullBarrier(cmd);
			s.measured[1] = true;
		}
		if (h.capture.active && !CaptureBefore(h, s, cmd, count)) h.capture.active = false;	// (2026-11-03) before stamp 2: not in the passes' times
		if (h.queries) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, h.queries, slot * kQueriesPerSlot + 2);

		// 3. the passes
		if (!h.scratchCleared && h.scratchStore.buffer && f.passCount) {	// (2026-11-07) the scratch store starts at zero (the passes' "empty")
			vkCmdFillBuffer(cmd, h.scratchStore.buffer, 0, VK_WHOLE_SIZE, 0u);
			FullBarrier(cmd);
			h.scratchCleared = true;
		}
		for (uint32_t p = 0; p < std::min<uint32_t>(f.passCount, NVR_RT_MAX_PASSES); p++) {
			const NvrRtPass& pass = f.passes[p];
			// UNOFFICIAL 2026-11-07 the RT shadow denoiser (NvrRtShadowDenoise.inl): a pass of RtShadowDenoise.spv runs the module
			if (ShadowDenoise::IsModulePass(h, pass.shader)) {
				if (!ShadowDenoise::Run(h, slot, cmd, p, pass)) continue;
				s.measured[2] = true;
				if (h.queries && s.passesStamped < NVR_RT_MAX_PASSES) {
					vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, h.queries, slot * kQueriesPerSlot + 4 + s.passesStamped);
					s.passShaders[s.passesStamped++] = pass.shader;
				}
				continue;
			}
			VkPipeline pipeline = Pipeline(h, pass.shader);
			if (!pipeline || pass.imageCount > NVR_RT_PASS_IMAGES_MAX) continue;
			bool imagesOk = true;
			for (uint32_t i = 0; i < pass.imageCount; i++) imagesOk = imagesOk && NvrRtPassImage(pass, i) < h.imageCount && h.images[NvrRtPassImage(pass, i)].view;
			if (!imagesOk) { Log("pass %u: an image index is not a shared image", p); continue; }
			memcpy((uint8_t*)s.constants.mapped + (size_t)p * NVR_RT_PASS_CONSTANTS, pass.constants, std::min<uint32_t>(pass.constantsSize, NVR_RT_PASS_CONSTANTS));

			VkWriteDescriptorSetAccelerationStructureKHR asWrite = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR };
			asWrite.accelerationStructureCount = 1;
			asWrite.pAccelerationStructures = &h.tlas;
			VkDescriptorBufferInfo ubo = { s.constants.buffer, (VkDeviceSize)p * NVR_RT_PASS_CONSTANTS, NVR_RT_PASS_CONSTANTS };
			VkDescriptorImageInfo imageInfos[NVR_RT_PASS_IMAGES_MAX] = {};
			VkWriteDescriptorSet writes[2 + NVR_RT_PASS_IMAGES_MAX + 3] = {};
			VkDescriptorBufferInfo recordInfo = { s.records.buffer, 0, VK_WHOLE_SIZE };
			VkDescriptorBufferInfo dataInfo = { h.geometryData.buffer, 0, VK_WHOLE_SIZE };
			VkDescriptorBufferInfo scratchInfo = { h.scratchStore.buffer, 0, VK_WHOLE_SIZE };	// (protocol 6)
			writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, &asWrite, s.sets[p], 0, 0, 1, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR };
			writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, s.sets[p], 1, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &ubo };
			for (uint32_t i = 0; i < pass.imageCount; i++) {
				imageInfos[i] = { VK_NULL_HANDLE, h.images[NvrRtPassImage(pass, i)].view, VK_IMAGE_LAYOUT_GENERAL };
				writes[2 + i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, s.sets[p], NvrRtPassBinding(i), 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &imageInfos[i] };
			}
			writes[2 + pass.imageCount] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, s.sets[p], NVR_RT_BINDING_RECORDS, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &recordInfo };
			writes[3 + pass.imageCount] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, s.sets[p], NVR_RT_BINDING_GEOMETRY, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &dataInfo };
			writes[4 + pass.imageCount] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, s.sets[p], NVR_RT_BINDING_SCRATCH, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &scratchInfo };
			vkUpdateDescriptorSets(h.vk.device, 5 + pass.imageCount, writes, 0, nullptr);
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, h.pipelineLayout, 0, 1, &s.sets[p], 0, nullptr);
			vkCmdDispatch(cmd, std::max(1u, pass.groupsX), std::max(1u, pass.groupsY), std::max(1u, pass.groupsZ));
			FullBarrier(cmd);	// the next pass may read this one's output; the game reads them after the signal
			s.measured[2] = true;
			if (h.queries && s.passesStamped < NVR_RT_MAX_PASSES) {	// per-pass GPU time (reported by shader every 120 frames)
				vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, h.queries, slot * kQueriesPerSlot + 4 + s.passesStamped);
				s.passShaders[s.passesStamped++] = pass.shader;
			}
		}
		if (h.capture.active) CaptureAfter(h, cmd);	// (2026-11-03) what the passes wrote
		return true;
	}

	// One frame: GPU wait for inputsReady >= N, the work, signal outputReady = N. Always submits (even when the work could not be
	// recorded) so the game's GPU wait for N is released.
	bool ProcessFrame(Helper& h, uint64_t frameNumber, HANDLE parent) {
		const NvrRtFrame& ring = g_shared->frames[frameNumber % NVR_RT_FRAME_RING];
		const bool intact = ring.frame == frameNumber;
		if (intact) {
			// Copy the record out (only as much as it uses), then tell NVR the slot is free again
			memcpy(&h.frame, &ring, offsetof(NvrRtFrame, ops));
			h.frame.opCount = std::min<uint32_t>(h.frame.opCount, NVR_RT_MAX_OPS);
			h.frame.instanceCount = std::min<uint32_t>(h.frame.instanceCount, NVR_RT_MAX_INSTANCES);
			h.frame.passCount = std::min<uint32_t>(h.frame.passCount, NVR_RT_MAX_PASSES);
			memcpy(h.frame.ops, ring.ops, sizeof(NvrRtGeometryOp) * h.frame.opCount);
			memcpy(h.frame.instances, ring.instances, sizeof(NvrRtInstance) * h.frame.instanceCount);
			memcpy(h.frame.records, ring.records, sizeof(NvrRtInstanceRecord) * h.frame.instanceCount);
			memcpy(h.frame.passes, ring.passes, sizeof(NvrRtPass) * h.frame.passCount);
		} else {
			// NVR waits before reusing a slot, so this means NVR skipped the frame: nothing to do but release its wait
			memset(&h.frame, 0, offsetof(NvrRtFrame, ops));
		}
		MemoryBarrier();
		g_shared->consumedFrame = (int64_t)frameNumber;

		Slot& s = h.slots[frameNumber % kCmdRing];
		if (!WaitSlot(h, s, parent)) return false;
		ReleaseFinished(h);
		VkCommandBuffer cmd = s.cmd;
		vkResetCommandBuffer(cmd, 0);
		VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		VK_CHECK(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer");
		const uint32_t slot = (uint32_t)(frameNumber % kCmdRing);
		if (h.queries) {
			vkCmdResetQueryPool(cmd, h.queries, slot * kQueriesPerSlot, kQueriesPerSlot);
			vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, h.queries, slot * kQueriesPerSlot);
		}
		// UNOFFICIAL 2026-11-03 (claude/rt-replay): a capture asked for by a file dropped next to the exe (looked for every 60 frames)
		if (!h.capture.spec.armed && (frameNumber % 60) == 0) {
			const std::wstring trigger = g_exeDir + L"\\NvrRtHelper.capture";
			if (GetFileAttributesW(trigger.c_str()) != INVALID_FILE_ATTRIBUTES && ReadCaptureSpec(trigger, h.capture.spec)) DeleteFileW(trigger.c_str());
		}
		h.capture.active = intact && h.capture.spec.armed && CaptureThisFrame(h, frameNumber);
		if (intact && !RecordFrame(h, s, frameNumber)) {
			if (h.capture.active) { h.capture.active = false; DestroyBuffer(h.vk, h.capture.buffer); }	// (nothing of it was submitted)
			// Start over with an empty command buffer: the signal must still go out
			vkResetCommandBuffer(cmd, 0);
			VK_CHECK(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer");
			if (h.queries) {
				vkCmdResetQueryPool(cmd, h.queries, slot * kQueriesPerSlot, kQueriesPerSlot);
				vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, h.queries, slot * kQueriesPerSlot);
				vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, h.queries, slot * kQueriesPerSlot + 1);
				vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, h.queries, slot * kQueriesPerSlot + 2);
			}
			s.passesStamped = 0;
		} else if (!intact && h.queries) {
			vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, h.queries, slot * kQueriesPerSlot + 1);
			vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, h.queries, slot * kQueriesPerSlot + 2);
		}
		if (h.queries) {
			if (!intact) s.passesStamped = 0;
			// every timestamp of the slot is written each frame (an unwritten one would make the whole read-back fail)
			for (uint32_t p = s.passesStamped; p < NVR_RT_MAX_PASSES; p++)
				vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, h.queries, slot * kQueriesPerSlot + 4 + p);
			vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, h.queries, slot * kQueriesPerSlot + 3);
		}
		VK_CHECK(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

		const uint64_t waitValue = frameNumber, signalValue = frameNumber;
		VkTimelineSemaphoreSubmitInfo timeline = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
		timeline.waitSemaphoreValueCount = 1;
		timeline.pWaitSemaphoreValues = &waitValue;
		timeline.signalSemaphoreValueCount = 1;
		timeline.pSignalSemaphoreValues = &signalValue;
		const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
		VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO, &timeline };
		submit.waitSemaphoreCount = 1;
		submit.pWaitSemaphores = &h.inputsReady;
		submit.pWaitDstStageMask = &waitStage;
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &cmd;
		submit.signalSemaphoreCount = 1;
		submit.pSignalSemaphores = &h.outputReady;
		VK_CHECK(vkQueueSubmit(h.vk.queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit");
		s.frame = frameNumber;
		if (h.capture.active) CaptureFinish(h, s, frameNumber);	// (2026-11-03) waits for this frame, writes the file on a thread

		LARGE_INTEGER now, frequency;
		QueryPerformanceCounter(&now);
		QueryPerformanceFrequency(&frequency);
		if (intact && h.frame.writtenQpc) {
			const uint64_t us = (uint64_t)((now.QuadPart - h.frame.writtenQpc) * 1000000 / frequency.QuadPart);
			h.responseUs[h.responseCount++ % 64] = us;
			g_shared->responseUsAvg = Average(h.responseUs, h.responseCount);
			if (us > g_shared->responseUsMax) g_shared->responseUsMax = (uint32_t)us;
		}
		g_shared->geometries = (uint32_t)h.geometries.size();
		g_shared->triangles = h.triangles;
		g_shared->geometryBytes = h.structures.used;
		g_shared->framesSubmitted++;
		MemoryBarrier();
		g_shared->submittedFrame = (int64_t)frameNumber;
		if (!h.loggedFirst) {
			h.loggedFirst = true;
			Log("first frame %llu submitted (%u ops, %u instances, %u passes)", (unsigned long long)frameNumber, h.frame.opCount, h.frame.instanceCount, h.frame.passCount);
		}
		if ((frameNumber % 600) == 0)
			Log("frame %llu: %u geometries (%llu triangles, %llu MB), %u instances; GPU %.3f ms avg (builds %.3f, scene %.3f, passes %.3f), response %.3f ms avg (max %.3f)",
				(unsigned long long)frameNumber, g_shared->geometries, (unsigned long long)g_shared->triangles, (unsigned long long)(g_shared->geometryBytes >> 20),
				g_shared->lastInstances, g_shared->gpuTimeUsAvg / 1000.0, g_shared->buildUsAvg / 1000.0, g_shared->sceneUsAvg / 1000.0, g_shared->traceUsAvg / 1000.0,
				g_shared->responseUsAvg / 1000.0, g_shared->responseUsMax / 1000.0);
		if (frameNumber % 120 == 0) {	// per-shader pass times (the RT sessions' request): to NVR's log via the status line
			char line[480];
			int n = sprintf_s(line, "pass GPU times (avg of the last 120 frames, ms a run):");
			bool any = false;
			for (uint32_t i = 0; i < NVR_RT_MAX_SHADERS && n > 0 && n < 420; i++) {
				if (!h.passRuns[i]) continue;
				const char* path = (const char*)g_shared->shaderPaths[i];
				const char* name = strrchr(path, '\\');
				name = name ? name + 1 : path;
				n += sprintf_s(line + n, sizeof(line) - n, " %.40s %.3f (%u runs)", name, h.passUs[i] / 1000.0 / h.passRuns[i], h.passRuns[i]);
				h.passUs[i] = 0; h.passRuns[i] = 0;
				any = true;
			}
			if (any) Status("%s", line);
		}
		if (frameNumber % 600 == 0)
			Log("frame %llu: structure memory %llu MB in use of %llu MB in blocks (%u empty blocks given back); compacted %llu structures, %llu -> %llu MB",
				(unsigned long long)frameNumber, (unsigned long long)(h.structures.used >> 20), (unsigned long long)(h.structures.total >> 20),
				h.structures.returned, (unsigned long long)h.compactedCount, (unsigned long long)(h.compactedBefore >> 20), (unsigned long long)(h.compactedAfter >> 20));
		if (frameNumber % 600 == 0 && (h.refits || h.fullBuilds))
			Log("frame %llu: dynamic (actor) geometry: %llu refits in place, %llu full builds so far", (unsigned long long)frameNumber,
				(unsigned long long)h.refits, (unsigned long long)h.fullBuilds);
		if (frameNumber % 600 == 0 && h.duplicateBuilds)
			Log("frame %llu: %llu duplicate builds skipped so far (the same geometry id twice in a frame: only the last is built)",
				(unsigned long long)frameNumber, (unsigned long long)h.duplicateBuilds);
		return true;
	}

	int RunHelper(const wchar_t* base) {
		OpenLog(L"NvrRtHelper.log");
		Log("NvrRtHelper (protocol %u) for New Vegas Reloaded, shared block %ls", NVR_RT_PROTOCOL_VERSION, base);
		HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, base);
		if (!mapping) { Log("the shared block %ls does not exist (error %lu)", base, GetLastError()); return 2; }
		NvrRtShared* shared = (NvrRtShared*)MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(NvrRtShared));
		if (!shared) { Log("could not map the shared block (error %lu)", GetLastError()); return 2; }
		if (shared->magic != NVR_RT_MAGIC || shared->version != NVR_RT_PROTOCOL_VERSION || shared->size != sizeof(NvrRtShared)) {
			Log("the shared block is of another version (magic 0x%08X, version %u, size %u; this helper: version %u, size %u)", shared->magic,
				shared->version, shared->size, NVR_RT_PROTOCOL_VERSION, (unsigned)sizeof(NvrRtShared));
			return 2;
		}
		const std::wstring frameName = std::wstring(base) + NVR_RT_EVENT_FRAME_SUFFIX, statusName = std::wstring(base) + NVR_RT_EVENT_STATUS_SUFFIX;
		HANDLE frameEvent = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, frameName.c_str());
		g_statusEvent = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, statusName.c_str());
		g_shared = shared;
		if (!frameEvent || !g_statusEvent) { Status("the helper's events do not exist"); SetState(NVR_RT_STATE_FAILED); return 2; }
		HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, shared->parentPid);
		if (!parent) { Status("the game's process %u cannot be opened", shared->parentPid); SetState(NVR_RT_STATE_FAILED); return 2; }

		for (int i = 0; i < 100 && !shared->setupDone; i++) {
			HANDLE waits[2] = { frameEvent, parent };
			if (WaitForMultipleObjects(2, waits, FALSE, 100) == WAIT_OBJECT_0 + 1) return 0;
		}
		MemoryBarrier();
		if (!shared->setupDone) { Status("NVR did not finish the setup in 10 seconds"); SetState(NVR_RT_STATE_FAILED); return 2; }

		Helper* hp = new Helper();
		Helper& h = *hp;
		auto fail = [&](const char* why) { if (why) Status("%s", why); SetState(NVR_RT_STATE_FAILED); return 3; };
		if (!CreateVulkanDevice(h.vk, shared->deviceLuid, shared->deviceLuidValid != 0, shared->deviceUuid)) return fail(nullptr);
		if (memcmp(h.vk.ids.driverUUID, shared->driverUuid, VK_UUID_SIZE))
			return fail("the helper's Vulkan driver is not the game's (driver UUIDs differ): images cannot be shared");
		h.imageCount = std::min<uint32_t>(shared->imageCount, NVR_RT_MAX_IMAGES);
		if (!h.imageCount) return fail("no arena image");
		for (uint32_t i = 0; i < h.imageCount; i++)
			if (!ImportImage(h.vk, shared->images[i], i, h.images[i])) return fail(nullptr);
		if (h.images[NVR_RT_ARENA_IMAGE].format != VK_FORMAT_R32_SFLOAT) return fail("the arena (image 0) is not R32F");
		if (!ImportTimeline(h.vk, shared->semInputsReady, "inputs-ready", h.inputsReady) ||
			!ImportTimeline(h.vk, shared->semOutputReady, "output-ready", h.outputReady)) return fail(nullptr);

		VkCommandPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
		poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		poolInfo.queueFamilyIndex = h.vk.queueFamily;
		if (vkCreateCommandPool(h.vk.device, &poolInfo, nullptr, &h.pool) != VK_SUCCESS) return fail("Vulkan: vkCreateCommandPool failed");
		for (Slot& s : h.slots) {
			VkCommandBufferAllocateInfo cmdInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
			cmdInfo.commandPool = h.pool;
			cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
			cmdInfo.commandBufferCount = 1;
			if (vkAllocateCommandBuffers(h.vk.device, &cmdInfo, &s.cmd) != VK_SUCCESS) return fail("Vulkan: vkAllocateCommandBuffers failed");
			if (!CreateBuffer(h.vk, sizeof(VkAccelerationStructureInstanceKHR) * NVR_RT_MAX_INSTANCES,
					VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true, s.instances) ||
				!CreateBuffer(h.vk, (VkDeviceSize)NVR_RT_PASS_CONSTANTS * NVR_RT_MAX_PASSES, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, s.constants) ||
				!CreateBuffer(h.vk, sizeof(NvrRtInstanceRecord) * NVR_RT_MAX_INSTANCES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, s.records))
				return fail(nullptr);
		}
		if (h.vk.timestampBits) {
			VkQueryPoolCreateInfo queryInfo = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
			queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
			queryInfo.queryCount = kCmdRing * kQueriesPerSlot;
			if (vkCreateQueryPool(h.vk.device, &queryInfo, nullptr, &h.queries) != VK_SUCCESS) h.queries = VK_NULL_HANDLE;
			{
				VkPushConstantRange range = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 40 };
				VkPipelineLayoutCreateInfo layoutInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
				layoutInfo.pushConstantRangeCount = 1;
				layoutInfo.pPushConstantRanges = &range;
				VkShaderModuleCreateInfo moduleInfo = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
				moduleInfo.codeSize = sizeof(kRtArenaGuardSpv);
				moduleInfo.pCode = kRtArenaGuardSpv;
				VkShaderModule module = VK_NULL_HANDLE;
				if (vkCreatePipelineLayout(h.vk.device, &layoutInfo, nullptr, &h.guardLayout) == VK_SUCCESS &&
					vkCreateShaderModule(h.vk.device, &moduleInfo, nullptr, &module) == VK_SUCCESS) {
					VkComputePipelineCreateInfo info = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
					info.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
					info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
					info.stage.module = module;
					info.stage.pName = "main";
					info.layout = h.guardLayout;
					if (vkCreateComputePipelines(h.vk.device, VK_NULL_HANDLE, 1, &info, nullptr, &h.guardPipeline) != VK_SUCCESS) h.guardPipeline = VK_NULL_HANDLE;
					vkDestroyShaderModule(h.vk.device, module, nullptr);
				}
				if (!CreateBuffer(h.vk, kCmdRing * 8, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, h.guardResult))
					h.guardResult = Buffer();
				else memset(h.guardResult.mapped, 0, kCmdRing * 8);
				Log("arena guard: %s", h.guardPipeline && h.guardResult.buffer ? "on (indices clamped before every build, sequence texel checked)" : "OFF (pipeline or buffer failed)");
			}
			if (h.vk.cmdWriteAsProperties && h.vk.cmdCopyAs) {
				VkQueryPoolCreateInfo compactInfo = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
				compactInfo.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
				compactInfo.queryCount = kCmdRing * kCompactPerSlot;
				if (vkCreateQueryPool(h.vk.device, &compactInfo, nullptr, &h.compactQueries) != VK_SUCCESS) h.compactQueries = VK_NULL_HANDLE;
			}
		}
		const SharedImage& arena = h.images[NVR_RT_ARENA_IMAGE];
		if (!CreateBuffer(h.vk, (VkDeviceSize)arena.width * arena.height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT |
				VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false, h.staging) ||
			!CreateBuffer(h.vk, (VkDeviceSize)kDataWords * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
				VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false, h.geometryData) ||	// (TRANSFER_SRC: the capture copies it out, 2026-11-03)
			!CreateTlas(h) || !EnsureScratch(h, h.tlasScratch) || !CreatePipelineLayout(h))
			return fail(nullptr);
		{	// (2026-11-07, protocol 6) the scratch store (binding 20): as big as NVR asked, at least 1 MB, cleared before the first pass
			const uint32_t mb = std::min<uint32_t>(std::max<uint32_t>(shared->scratchMB, 1u), 1024u);
			if (!CreateBuffer(h.vk, (VkDeviceSize)mb << 20, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, h.scratchStore))
				return fail("Vulkan: the scratch store could not be made");
			Log("scratch store: %u MB (binding %u)", mb, NVR_RT_BINDING_SCRATCH);
		}
		if (!CreateBuffer(h.vk, 3 * 4096, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, h.probe)) h.probe = Buffer();	// the probe is optional

		// The shaders NVR registered at setup are compiled now (a first use mid-game would hold the game's GPU ~2 s on the iGPU)
		for (uint32_t i = 0; i < std::min<uint32_t>((uint32_t)shared->shaderCount, NVR_RT_MAX_SHADERS); i++) Pipeline(h, i);

		h.dataWords.Init(kDataWords);
		ReadCaptureSpec(g_exeDir + L"\\NvrRtHelper.capture-at", h.capture.spec);	// UNOFFICIAL 2026-11-03: a capture at a given frame (CaptureSpec)
		// The first structure block and a build scratch now, not in the first frame (on the iGPU with the game running, the first
		// allocations took seconds: the game's GPU would have waited for them)
		{
			uint32_t block = 0;
			VkDeviceSize offset = 0;
			if (h.structures.Allocate(h.vk, 256, block, offset)) h.structures.Free(h.vk, block, offset, 256);
			if (!EnsureScratch(h, 16ull << 20)) return fail(nullptr);
		}
		// (2026-11-06, validation: "expects ... VK_IMAGE_LAYOUT_GENERAL, instead current layout is VK_IMAGE_LAYOUT_UNDEFINED") the shared
		// images are used in GENERAL from the first frame but were never put there: one transition, now, before the game writes anything
		// (it waits for READY), so dropping the old (empty) contents is harmless. NVIDIA is the stricter driver about layouts.
		{
			std::vector<VkImageMemoryBarrier> to;
			for (uint32_t i = 0; i < h.imageCount; i++) {
				if (!h.images[i].image) continue;
				VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
				b.srcAccessMask = 0;
				b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
				b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
				b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
				b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				b.image = h.images[i].image;
				b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS };
				to.push_back(b);
			}
			VkCommandBuffer cmd = h.slots[0].cmd;
			VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
			begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			if (!to.empty() && vkBeginCommandBuffer(cmd, &begin) == VK_SUCCESS) {
				vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, (uint32_t)to.size(), to.data());
				if (vkEndCommandBuffer(cmd) == VK_SUCCESS) {
					VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
					submit.commandBufferCount = 1;
					submit.pCommandBuffers = &cmd;
					if (vkQueueSubmit(h.vk.queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS) vkQueueWaitIdle(h.vk.queue);
				}
				vkResetCommandBuffer(cmd, 0);
			}
		}
		Status("ready: ray tracing on %s, the game's GPU (found by %s; %u shared images, arena %ux%u)", h.vk.properties.deviceName, h.vk.matchedBy,
			h.imageCount, arena.width, arena.height);
		SetState(NVR_RT_STATE_READY);

		uint64_t next = 1;
		bool parentGone = false;
		for (;;) {
			HANDLE waits[2] = { frameEvent, parent };
			const DWORD w = WaitForMultipleObjects(2, waits, FALSE, 250);
			if (w == WAIT_OBJECT_0 + 1) { Log("the game exited"); parentGone = true; break; }
			if (shared->quit) {
				Log("NVR asked the helper to exit");
				if (next > 1) {
					const uint64_t last = next - 1;
					VkSemaphoreWaitInfo wait = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
					wait.semaphoreCount = 1;
					wait.pSemaphores = &h.outputReady;
					wait.pValues = &last;
					vkWaitSemaphores(h.vk.device, &wait, 2000ull * 1000 * 1000);
				}
				break;
			}
			for (uint32_t i = 0; i < std::min<uint32_t>((uint32_t)shared->shaderCount, NVR_RT_MAX_SHADERS); i++) Pipeline(h, i);	// shaders registered later
			const int64_t latest = InterlockedCompareExchange64((volatile LONG64*)&shared->latestFrame, 0, 0);
			MemoryBarrier();
			while ((int64_t)next <= latest) {
				if (!ProcessFrame(h, next, parent)) { Log("stopping at frame %llu", (unsigned long long)next); goto done; }
				next++;
			}
		}
	done:
		if (shared->state != NVR_RT_STATE_FAILED) SetState(NVR_RT_STATE_EXITED);
		Log("exit after %llu frames%s", (unsigned long long)shared->framesSubmitted, parentGone ? " (the game is gone)" : "");
		if (g_log) fclose(g_log);
		ExitProcess(0);
	}

	int RunProbe() {
		g_console = true;
		OpenLog(L"NvrRtHelper-probe.log");
		Log("NvrRtHelper probe (protocol %u, shared block %u bytes)", NVR_RT_PROTOCOL_VERSION, (unsigned)sizeof(NvrRtShared));
		VulkanDevice vk;
		if (!CreateVulkanDevice(vk, nullptr, false, nullptr)) { Log("probe: no GPU here can run the ray tracing helper"); return 1; }
		Log("probe: the helper can run on %s", vk.properties.deviceName);
		{
			uint32_t n = 0;
			vkEnumerateDeviceExtensionProperties(vk.physical, nullptr, &n, nullptr);
			std::vector<VkExtensionProperties> exts(n);
			vkEnumerateDeviceExtensionProperties(vk.physical, nullptr, &n, exts.data());
			Log("probe: VK_KHR_ray_tracing_position_fetch %s, VK_KHR_ray_tracing_pipeline %s", HasExtension(exts, "VK_KHR_ray_tracing_position_fetch") ? "yes" : "no",
				HasExtension(exts, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME) ? "yes" : "no");
		}
		vkDestroyDevice(vk.device, nullptr);
		vkDestroyInstance(vk.instance, nullptr);
		return 0;
	}

#include "NvrRtReplay.inl"	// UNOFFICIAL 2026-11-03 (claude/rt-replay): RunReplay, --replay <capture>
}

int wmain(int argc, wchar_t** argv) {
	InitializeCriticalSection(&g_logLock);
	g_exeDir = ExeDir();
	for (int i = 1; i < argc; i++) {
		if (!wcscmp(argv[i], L"--shm") && i + 1 < argc) return RunHelper(argv[i + 1]);
		if (!wcscmp(argv[i], L"--probe")) return RunProbe();
		if (!wcscmp(argv[i], L"--replay")) return RunReplay(argc, argv);
	}
	printf("NvrRtHelper: started by New Vegas Reloaded. By hand: --probe (can this PC run it?), --replay <capture.nvrcap> (docs/next/RT-REPLAY.md).\n");
	return 1;
}
