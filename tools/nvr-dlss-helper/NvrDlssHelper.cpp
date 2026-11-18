// NvrDlssHelper.exe -- the 64-bit helper behind New Vegas Reloaded's DLSS upscaler ([Shaders.TAA.FSR] Upscaler = 1).
// MIT licence (LICENSE in this folder). NVIDIA's DLSS SDK parts it links (nvsdk_ngx_s.lib) and loads (nvngx_dlss.dll) are
// NVIDIA's, under NVIDIA's RTX SDK licence (NVIDIA-SDK-NOTICE.txt). Built without the SDK (NVR_DLSS_NO_SDK) it can only
// do the pass-through test.
//
// Why a separate process: Fallout New Vegas, NVR and the NVR DXVK d3d9.dll are 32-bit; NVIDIA ships DLSS (NGX) for 64-bit
// Windows only. The helper makes its own Vulkan device on the same GPU, imports four images and two timeline semaphores the
// fork exports (VK_KHR_external_memory_win32 / VK_KHR_external_semaphore_win32), and per frame records DLSS (or, in the
// pass-through test, a stretch copy plus a marker) between a GPU wait for "inputs ready" = N and a signal of "output
// ready" = N. The protocol is NvrDlssProtocol.h; docs/next/FORK2-DLSS.md in the NVR repository has the whole design.
//
// UNOFFICIAL 2026-10-30 protocol 2 (DLSS Ray Reconstruction, docs/next/FORK2-DLSS-RR.md): with NVR's RR switch on there are
// eight images (four RR guides: diffuse albedo, specular albedo, normals, roughness), and a frame flagged NVR_DLSS_FRAME_RR is
// evaluated by DLSS-D (NGX's RayReconstruction feature, nvngx_dlssd.dll next to the helper) instead of DLSS SR. RR has its
// own NGX feature handle and parameter map; RR not available or failing only reports rrAvailable = 0, DLSS SR goes on. DLSS
// SR also takes NVR's model preset per frame now (a hint per quality mode, set before the feature is made).
//
// Usage:
//   NvrDlssHelper.exe --shm <name>   started by NVR (never by hand); UNOFFICIAL 2026-10-30 (DLAA stall report, RTX 5060: the game's
//                                    GPU sat ~45 ms in the wait for the helper while the helper's own work took 0.94 ms and it
//                                    submitted within 0.15 ms) two options after it, from [Shaders.TAA.DLSS] HelperWait / HelperGpuPriority:
//                                    --wait cpu: the helper waits on the CPU until the game's "inputs ready" is reached, then submits
//                                    a frame with no semaphore wait in it (no cross-process wait for the driver to resolve on the GPU);
//                                    --wait gpu: as before (the submission waits on the GPU). --gpu-priority high: the helper's queue at
//                                    VK_EXT/KHR_global_priority HIGH where the driver allows it, and its process at the WDDM scheduling
//                                    priority class HIGH (D3DKMTSetProcessSchedulingPriorityClass; CPU priority is not GPU priority);
//                                    --gpu-priority normal: neither.
//                                    UNOFFICIAL 2026-10-30 (tester report: with DLSS on, the render-scaled picture stayed small in the
//                                    top-left corner), from [Shaders.TAA.DLSS] HelperExactInputs / HelperFailTest: --inputs exact (the
//                                    default): the render rectangle of colour, depth and motion is copied into images of exactly the
//                                    render size each frame, and DLSS and the pass-through read those whole (RecordExactInputs);
//                                    --inputs subrect: DLSS reads the rectangle out of the screen-sized images (before 2026-10-30).
//                                    --fail-test report|hang, TEST ONLY: from frame 120 the helper reports a failure as a failed DLSS
//                                    evaluation does (report), or stops answering (hang: NVR's watchdog ends it after 3 s).
//                                    UNOFFICIAL 2026-10-31 (RTX 4070 Ti SUPER tester: --inputs exact gave DLSS a 0x0 render subrect and
//                                    NGX 310.9.16 refused every evaluate): --fail-test zero-subrect, TEST ONLY: that 0x0 again, for the
//                                    render subrect check (SubrectProblem) to catch; --subrect-range strict (the self-test's child): a
//                                    render subrect outside NGX's dynamic range is refused too, not only logged.
//   NvrDlssHelper.exe --probe       prints the GPU, the Vulkan interop support and whether DLSS (and Ray Reconstruction) is
//                                    available, then exits
//   NvrDlssHelper.exe --selftest     (or --selftest dlss: the child in DLSS mode; without DLSS it must give up cleanly) the whole cross-process exchange without the game: this process plays NVR's part
//                                    (exports images and semaphores from a Vulkan device of its own), starts a second copy
//                                    of itself in pass-through mode and checks the pixels and the order of 300 frames
//   NvrDlssHelper.exe --selftest rr  the same with the RR exchange: eight images, every frame an RR frame, the four guides
//                                    drawn as thumbnails by the pass-through child (--selftest dlss rr: the child in DLSS mode)
//                                    UNOFFICIAL 2026-10-30: every self-test fills the colour input outside the render rectangle green
//                                    and fails if green reaches the output (the small picture in the corner); --selftest ... subrect
//                                    runs the child with --inputs subrect (and ... subrect edges also checks its last column and row,
//                                    where the old stretch's bilinear tap reaches past the rectangle: it fails, proving the check works).
//                                    UNOFFICIAL 2026-10-31: the child checks the render subrect of every frame (the pass-through: the one
//                                    a real evaluate would be given) and must still be READY at the end; --selftest ... zerosubrect runs
//                                    it with --fail-test zero-subrect and PASSES only if the check catches the 0x0.
// Only runs on NVIDIA GPUs (vendor 0x10DE): NVIDIA's licence for the DLSS SDK covers systems with NVIDIA GPUs only.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#ifndef NVR_DLSS_NO_SDK
#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_helpers_vk.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "NvrDlssProtocol.h"

namespace {

	// ---------------------------------------------------------------------------------------------------------------------
	// Log: NvrDlssHelper.log next to the exe (or NvrDlssHelper-selftest.log), plus the last line into the shared block.

	CRITICAL_SECTION	g_logLock;
	FILE*				g_log = nullptr;
	NvrDlssShared*		g_shared = nullptr;
	HANDLE				g_statusEvent = nullptr;
	bool				g_console = false;
	// UNOFFICIAL 2026-10-30 (DLAA stall report): --wait cpu / --gpu-priority high (the helper's start options; see the header)
	bool				g_cpuWait = false;
	bool				g_highPriority = false;
	// UNOFFICIAL 2026-10-30 (top-left picture report): --inputs exact|subrect, --fail-test report|hang (test only; see the header)
	bool				g_exactInputs = true;
	int					g_failTest = 0;		// 0 none, 1 report, 2 hang, 3 zero-subrect (UNOFFICIAL 2026-10-31)
	bool				g_strictEdges = false;	// --selftest ... edges: the last column / row checked with --inputs subrect too
	bool				g_strictSubrect = false;	// UNOFFICIAL 2026-10-31 --subrect-range strict (the self-test's child; see the header)
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

	// A status line NVR should see (also logged).
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

	void SetState(NvrDlssState state) {
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
			// The game folder may be read-only: the temp folder then.
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
			case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
			case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
			case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
			default: return "VkResult(other)";
		}
	}

#define VK_CHECK(call, what) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { Status("Vulkan: %s failed: %s (%d)", what, VkResultName(r_), (int)r_); return false; } } while (0)

	uint16_t FloatToHalf(float value) {
		uint32_t f;
		memcpy(&f, &value, 4);
		uint32_t sign = (f >> 16) & 0x8000u;
		int exponent = (int)((f >> 23) & 0xFF) - 127 + 15;
		uint32_t mantissa = f & 0x7FFFFFu;
		if (exponent <= 0) return (uint16_t)sign;
		if (exponent >= 31) return (uint16_t)(sign | 0x7C00u);
		return (uint16_t)(sign | ((uint32_t)exponent << 10) | (mantissa >> 13));
	}

	float HalfToFloat(uint16_t h) {
		uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
		uint32_t exponent = (h >> 10) & 0x1F;
		uint32_t mantissa = h & 0x3FF;
		uint32_t f;
		if (exponent == 0) f = sign;	// zero / denormals as zero (enough here)
		else if (exponent == 31) f = sign | 0x7F800000u | (mantissa << 13);
		else f = sign | ((exponent - 15 + 127) << 23) | (mantissa << 13);
		float out;
		memcpy(&out, &f, 4);
		return out;
	}

	bool HasExtension(const std::vector<VkExtensionProperties>& list, const char* name) {
		for (const VkExtensionProperties& e : list) if (!strcmp(e.extensionName, name)) return true;
		return false;
	}

	void AddUnique(std::vector<const char*>& list, const char* name) {
		for (const char* n : list) if (!strcmp(n, name)) return;
		list.push_back(name);
	}

	const uint32_t kNvidiaVendor = 0x10DE;
	const uint32_t kCmdRing = 4;
	const uint32_t kMarkerSize = 24;	// px, the pass-through test's marker boxes

	// UNOFFICIAL 2026-10-30 protocol 2: the images' names (log lines) for the whole table; imageCount says how many exist.
	const char* const kImageNames[NVR_DLSS_IMAGE_COUNT] = { "colour", "depth", "motion", "output", "diffuse albedo", "specular albedo",
		"normals", "roughness" };
	const uint32_t kGuideCount = NVR_DLSS_IMAGE_COUNT - NVR_DLSS_IMAGE_COUNT_SR;	// the RR guides: images 4..7

	bool FileNextToHelper(const wchar_t* name) {
		const DWORD attributes = GetFileAttributesW((g_exeDir + L"\\" + name).c_str());
		return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
	}

	// UNOFFICIAL 2026-10-30 the pass-through RR test's guide thumbnails: along the output's bottom edge from the left, each a sixth
	// of the output's width with the render rectangle's aspect, 16 px apart. False: the output is too small for the four.
	bool GuideThumbnail(uint32_t outW, uint32_t outH, uint32_t renderW, uint32_t renderH, uint32_t index, VkOffset2D& at, VkExtent2D& size) {
		const uint32_t gap = 16;
		size.width = outW / 6;
		size.height = renderW ? (uint32_t)((uint64_t)size.width * renderH / renderW) : 0;
		if (!size.width || !size.height || gap + kGuideCount * (size.width + gap) > outW || size.height + 2 * gap > outH) return false;
		at.x = (int32_t)(gap + index * (size.width + gap));
		at.y = (int32_t)(outH - gap - size.height);
		return true;
	}

	// ---------------------------------------------------------------------------------------------------------------------
	// A Vulkan device on the GPU NVR's DXVK uses (matched by LUID, else UUID), with what the exchange and NGX need.

	struct VulkanDevice {
		VkInstance			instance = VK_NULL_HANDLE;
		VkPhysicalDevice	physical = VK_NULL_HANDLE;
		VkDevice			device = VK_NULL_HANDLE;
		uint32_t			queueFamily = 0;
		VkQueue				queue = VK_NULL_HANDLE;
		VkPhysicalDeviceProperties	properties = {};
		VkPhysicalDeviceIDProperties ids = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
		VkPhysicalDeviceMemoryProperties memory = {};
		uint32_t			timestampBits = 0;
		bool				ngxExtensions = false;	// every device extension NGX asked for is enabled
		bool				rrExtensions = false;	// UNOFFICIAL 2026-10-30: and every one NGX asked for Ray Reconstruction (only with wantRr)
		PFN_vkGetMemoryWin32HandleKHR		getMemoryWin32Handle = nullptr;
		PFN_vkGetSemaphoreWin32HandleKHR	getSemaphoreWin32Handle = nullptr;
		PFN_vkImportSemaphoreWin32HandleKHR	importSemaphoreWin32Handle = nullptr;
	};

	// UNOFFICIAL 2026-11-15 (docs/next/DLSS-RR-MODEL.md) Ray Reconstruction's model. nvngx_dlssd.dll 310.9.16 (the one NVR ships)
	// already holds NVIDIA's second-generation RR transformer ("DLSS 4.5 Ray Reconstruction", NGX preset F, "RR2" in its strings, with
	// kernels for sm_75 .. sm_120) and gates it on driver 580.00: an explicit F on an older driver fails the feature creation. PCGH
	// (2026-08-25): F runs about as fast as the first-generation model on RTX 40 / 50 but costs up to 25-29 % of the frame rate on
	// RTX 20 / 30 (no FP8 in their tensor cores). So RRPreset 4 (auto) sends F on Ada and newer and E (the first-generation transformer,
	// the model DLSS itself falls back to without the driver) on Turing / Ampere. The generation comes from the PCI device id, NVIDIA's
	// ranges by chip family: Vulkan has no portable "architecture" query, and NGX's only one (FeatureRequirement.MinHWArchitecture) is
	// a minimum, not the GPU's own. Outside the SDK guard so the pass-through build and --probe run the same choice.
	enum class NvidiaGeneration { Unknown, BeforeTuring, Turing, Ampere, AdaOrNewer };

	NvidiaGeneration GenerationOf(uint32_t deviceId) {
		if (deviceId < 0x1E00) return NvidiaGeneration::BeforeTuring;						// Pascal, Volta and older (no DLSS)
		if (deviceId < 0x2000) return NvidiaGeneration::Turing;							// TU102 / TU104 / TU106 / TU117 (RTX 20, GTX 16)
		if (deviceId < 0x2180) return NvidiaGeneration::Ampere;							// GA100
		if (deviceId < 0x2200) return NvidiaGeneration::Turing;							// TU116 (GTX 16)
		if (deviceId < 0x2300) return NvidiaGeneration::Ampere;							// GA102 (RTX 3080 / 3090, A-series)
		if (deviceId < 0x2400) return NvidiaGeneration::AdaOrNewer;						// GH100 (Hopper: FP8 tensor cores, data centre)
		if (deviceId < 0x2600) return NvidiaGeneration::Ampere;							// GA103 / GA104 / GA106 / GA107 (RTX 30)
		if (deviceId < 0x3000) return NvidiaGeneration::AdaOrNewer;						// AD10x (RTX 40, L4, L40) 0x26xx-0x28xx; GB10x / GB20x (RTX 50) 0x29xx-0x2Fxx
		return NvidiaGeneration::Unknown;												// later families: not guessed
	}

	const char* GenerationName(NvidiaGeneration g) {
		switch (g) {
		case NvidiaGeneration::BeforeTuring: return "older than RTX 20";
		case NvidiaGeneration::Turing: return "RTX 20 class (Turing)";
		case NvidiaGeneration::Ampere: return "RTX 30 class (Ampere)";
		case NvidiaGeneration::AdaOrNewer: return "RTX 40 / 50 class (Ada, Blackwell or Hopper)";
		default: return "an unknown generation";
		}
	}

	// NVIDIA's Vulkan driverVersion packing: 10 bits major, 8 bits minor (581.57 = 581 << 22 | 57 << 14).
	uint32_t NvidiaDriverMajor(uint32_t driverVersion) { return driverVersion >> 22; }
	uint32_t NvidiaDriverMinor(uint32_t driverVersion) { return (driverVersion >> 14) & 0xFF; }
	const uint32_t kRrPresetFMinDriverMajor = 580;	// nvngx_dlssd.dll 310.9.16: "Preset_F selected but driver %d.%02d is older than required 580.00"

	const char* const kRrModelNames[NVR_DLSS_RR_PRESET_COUNT] = { "NVIDIA's default for this nvngx_dlssd.dll (no hint)", "preset D",
		"preset E (the first-generation transformer, the current model)", "preset F (the second-generation transformer, the newer model)",
		"auto" };

	// The NvrDlssRrPreset the helper sends to NGX for NVR's RRPreset (never AUTO), and why, in words for the log.
	uint32_t ChooseRrModel(uint32_t nvrPreset, uint32_t deviceId, uint32_t driverVersion, std::string& why) {
		char text[320];
		const uint32_t major = NvidiaDriverMajor(driverVersion), minor = NvidiaDriverMinor(driverVersion);
		const bool driverForF = major >= kRrPresetFMinDriverMajor;
		const NvidiaGeneration gen = GenerationOf(deviceId);
		if (nvrPreset >= NVR_DLSS_RR_PRESET_COUNT) {
			snprintf(text, sizeof(text), "RRPreset %u is out of range, read as 0", nvrPreset);
			why = text;
			return NVR_DLSS_RR_PRESET_DEFAULT;
		}
		if (nvrPreset == NVR_DLSS_RR_PRESET_F) {
			if (driverForF) { why = "RRPreset 3 (the newer model) as set"; return NVR_DLSS_RR_PRESET_F; }
			snprintf(text, sizeof(text), "RRPreset 3 (the newer model) asked for, but driver %u.%02u is older than 580.00, which it needs "
				"(NGX would refuse it): preset E instead; update the NVIDIA driver to get it", major, minor);
			why = text;
			return NVR_DLSS_RR_PRESET_E;
		}
		if (nvrPreset != NVR_DLSS_RR_PRESET_AUTO) {
			snprintf(text, sizeof(text), "RRPreset %u as set", nvrPreset);
			why = text;
			return nvrPreset;
		}
		if (gen == NvidiaGeneration::AdaOrNewer && driverForF) {
			snprintf(text, sizeof(text), "RRPreset 4 (auto): GPU device 0x%04X is %s and driver %u.%02u is 580.00 or newer", deviceId,
				GenerationName(gen), major, minor);
			why = text;
			return NVR_DLSS_RR_PRESET_F;
		}
		if (gen == NvidiaGeneration::AdaOrNewer) snprintf(text, sizeof(text), "RRPreset 4 (auto): GPU device 0x%04X is %s, but driver %u.%02u "
			"is older than 580.00, which the newer model needs", deviceId, GenerationName(gen), major, minor);
		else if (gen == NvidiaGeneration::Turing || gen == NvidiaGeneration::Ampere) snprintf(text, sizeof(text), "RRPreset 4 (auto): GPU "
			"device 0x%04X is %s, where the newer model costs up to a quarter of the frame rate", deviceId, GenerationName(gen));
		else snprintf(text, sizeof(text), "RRPreset 4 (auto): GPU device 0x%04X is %s: the current model to be safe", deviceId, GenerationName(gen));
		why = text;
		return NVR_DLSS_RR_PRESET_E;
	}

#ifndef NVR_DLSS_NO_SDK
	// NVR's own project id for NGX's custom-engine identification (NVR has no NVIDIA application id).
	const char* const kNgxProjectId = "4f1c2b7e-9a63-4d0e-8b25-6e3d1a7c9f40";
	const char* const kNgxEngineVersion = "1.0";
	std::wstring	g_ngxDataPath;
	const wchar_t*	g_ngxPaths[1] = {};
	NVSDK_NGX_FeatureCommonInfo g_ngxFeatureInfo = {};

	void NVSDK_CONV NgxLog(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
		std::string line(message ? message : "");
		while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
		Log("NGX: %s", line.c_str());
	}

	// UNOFFICIAL 2026-10-30: feature = NVSDK_NGX_Feature_RayReconstruction for DLSS-D's own extension needs (the same project id,
	// paths and log as DLSS SR's).
	NVSDK_NGX_FeatureDiscoveryInfo NgxDiscoveryInfo(NVSDK_NGX_Feature feature = NVSDK_NGX_Feature_SuperSampling) {
		if (g_ngxDataPath.empty()) {
			wchar_t temp[MAX_PATH] = {};
			GetTempPathW(MAX_PATH, temp);
			g_ngxDataPath = std::wstring(temp) + L"NvrDlssHelper";
			CreateDirectoryW(g_ngxDataPath.c_str(), nullptr);
			g_ngxPaths[0] = g_exeDir.c_str();	// nvngx_dlss.dll next to the helper
			g_ngxFeatureInfo.PathListInfo.Path = g_ngxPaths;
			g_ngxFeatureInfo.PathListInfo.Length = 1;
			g_ngxFeatureInfo.LoggingInfo.LoggingCallback = NgxLog;
			g_ngxFeatureInfo.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
			g_ngxFeatureInfo.LoggingInfo.DisableOtherLoggingSinks = false;
		}
		NVSDK_NGX_FeatureDiscoveryInfo info = {};
		info.SDKVersion = NVSDK_NGX_Version_API;
		info.FeatureID = feature;
		info.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
		info.Identifier.v.ProjectDesc.ProjectId = kNgxProjectId;
		info.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
		info.Identifier.v.ProjectDesc.EngineVersion = kNgxEngineVersion;
		info.ApplicationDataPath = g_ngxDataPath.c_str();
		info.FeatureInfo = &g_ngxFeatureInfo;
		return info;
	}

	std::string NgxResultString(NVSDK_NGX_Result result) {
		const wchar_t* text = GetNGXResultAsString(result);
		std::string out;
		if (text) for (const wchar_t* c = text; *c; c++) out.push_back((char)(*c < 128 ? *c : '?'));
		char code[32];
		snprintf(code, sizeof(code), " (0x%08X)", (unsigned)result);
		return out + code;
	}
#endif

	// match: LUID (8 bytes, when valid) or device UUID; null = the first NVIDIA GPU (probe / self-test exporter).
	// UNOFFICIAL 2026-10-30 wantRr (with wantNgx): the instance and the device also get the extensions NGX asks for Ray
	// Reconstruction (the union with DLSS SR's). Only when NVR wants RR, so a helper without it makes the same device as before.
	bool CreateVulkanDevice(VulkanDevice& vk, const uint8_t* luid, bool luidValid, const uint8_t* uuid, bool wantNgx, bool wantRr = false) {
		// UNOFFICIAL 2026-10-30 (review): NGX's extension names are copied here before they are listed: with Ray Reconstruction a second
		// NGX query runs before vkCreateInstance / vkCreateDevice read the list, and NGX does not say how long its returned array stays
		// valid (the device one comes from the driver). A deque: growing it never moves the strings the list points to.
		std::deque<std::string> ownedNames;
		auto owned = [&ownedNames](const char* name) { ownedNames.emplace_back(name); return ownedNames.back().c_str(); };
		uint32_t loaderVersion = VK_API_VERSION_1_0;
		PFN_vkEnumerateInstanceVersion enumerateVersion = (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion");
		if (enumerateVersion) enumerateVersion(&loaderVersion);
		if (loaderVersion < VK_API_VERSION_1_2) {
			Status("Vulkan: the Vulkan loader is %u.%u, 1.2 is needed (timeline semaphores)", VK_VERSION_MAJOR(loaderVersion), VK_VERSION_MINOR(loaderVersion));
			return false;
		}

		uint32_t count = 0;
		vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
		std::vector<VkExtensionProperties> instanceExts(count);
		vkEnumerateInstanceExtensionProperties(nullptr, &count, instanceExts.data());
		std::vector<const char*> enabledInstance;
#ifndef NVR_DLSS_NO_SDK
		if (wantNgx) {
			NVSDK_NGX_FeatureDiscoveryInfo discovery = NgxDiscoveryInfo();
			uint32_t ngxCount = 0;
			VkExtensionProperties* ngxExts = nullptr;
			NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&discovery, &ngxCount, &ngxExts);
			if (NVSDK_NGX_FAILED(r)) Log("NGX: instance extension query failed: %s", NgxResultString(r).c_str());
			for (uint32_t i = 0; i < ngxCount && ngxExts; i++) {
				if (HasExtension(instanceExts, ngxExts[i].extensionName)) AddUnique(enabledInstance, owned(ngxExts[i].extensionName));
				else Log("NGX wants the instance extension %s, which the loader does not have", ngxExts[i].extensionName);
			}
		}
		if (wantNgx && wantRr) {
			// UNOFFICIAL 2026-10-30 DLSS Ray Reconstruction's instance extensions, added to DLSS SR's. A failed query is only logged:
			// RR then tries with SR's list, and a feature creation that fails turns RR off, never SR.
			NVSDK_NGX_FeatureDiscoveryInfo discovery = NgxDiscoveryInfo(NVSDK_NGX_Feature_RayReconstruction);
			uint32_t ngxCount = 0;
			VkExtensionProperties* ngxExts = nullptr;
			NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&discovery, &ngxCount, &ngxExts);
			if (NVSDK_NGX_FAILED(r)) Log("NGX: Ray Reconstruction's instance extension query failed: %s", NgxResultString(r).c_str());
			for (uint32_t i = 0; i < ngxCount && ngxExts; i++) {
				if (HasExtension(instanceExts, ngxExts[i].extensionName)) AddUnique(enabledInstance, owned(ngxExts[i].extensionName));
				else Log("NGX wants the instance extension %s for Ray Reconstruction, which the loader does not have", ngxExts[i].extensionName);
			}
		}
#else
		(void)wantNgx;
		(void)wantRr;
#endif

		VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
		app.pApplicationName = "NvrDlssHelper";
		app.applicationVersion = 1;
		app.pEngineName = "New Vegas Reloaded";
		app.engineVersion = 1;
		app.apiVersion = VK_API_VERSION_1_2;
		VkInstanceCreateInfo instanceInfo = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
		instanceInfo.pApplicationInfo = &app;
		instanceInfo.enabledExtensionCount = (uint32_t)enabledInstance.size();
		instanceInfo.ppEnabledExtensionNames = enabledInstance.data();
		VK_CHECK(vkCreateInstance(&instanceInfo, nullptr, &vk.instance), "vkCreateInstance");

		count = 0;
		vkEnumeratePhysicalDevices(vk.instance, &count, nullptr);
		std::vector<VkPhysicalDevice> devices(count);
		vkEnumeratePhysicalDevices(vk.instance, &count, devices.data());
		for (VkPhysicalDevice d : devices) {
			VkPhysicalDeviceIDProperties ids = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
			VkPhysicalDeviceProperties2 props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &ids };
			vkGetPhysicalDeviceProperties2(d, &props);
			bool match;
			if (luid && luidValid && ids.deviceLUIDValid) match = !memcmp(ids.deviceLUID, luid, VK_LUID_SIZE);
			else if (uuid) match = !memcmp(ids.deviceUUID, uuid, VK_UUID_SIZE);
			else match = props.properties.vendorID == kNvidiaVendor;
			Log("Vulkan device: %s (vendor 0x%04X, device 0x%04X, API %u.%u.%u)%s", props.properties.deviceName, props.properties.vendorID,
				props.properties.deviceID, VK_VERSION_MAJOR(props.properties.apiVersion), VK_VERSION_MINOR(props.properties.apiVersion),
				VK_VERSION_PATCH(props.properties.apiVersion), match && !vk.physical ? "  <- this one" : "");
			if (match && !vk.physical) {
				vk.physical = d;
				vk.properties = props.properties;
				vk.ids = ids;
				vk.ids.pNext = nullptr;
			}
		}
		if (!vk.physical) {
			Status("Vulkan: the GPU the game uses was not found among %u Vulkan devices", count);
			return false;
		}
		if (vk.properties.vendorID != kNvidiaVendor) {
			Status("not an NVIDIA GPU (vendor 0x%04X): the helper runs on NVIDIA GPUs only (NVIDIA's DLSS SDK licence)", vk.properties.vendorID);
			return false;
		}
		if (vk.properties.apiVersion < VK_API_VERSION_1_2) {
			Status("Vulkan: the driver offers Vulkan %u.%u, 1.2 is needed", VK_VERSION_MAJOR(vk.properties.apiVersion), VK_VERSION_MINOR(vk.properties.apiVersion));
			return false;
		}
		vkGetPhysicalDeviceMemoryProperties(vk.physical, &vk.memory);

		count = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(vk.physical, &count, nullptr);
		std::vector<VkQueueFamilyProperties> families(count);
		vkGetPhysicalDeviceQueueFamilyProperties(vk.physical, &count, families.data());
		bool found = false;
		for (uint32_t i = 0; i < count; i++) {
			if ((families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
				vk.queueFamily = i;
				vk.timestampBits = families[i].timestampValidBits;
				found = true;
				break;
			}
		}
		if (!found) { Status("Vulkan: no graphics + compute queue"); return false; }

		count = 0;
		vkEnumerateDeviceExtensionProperties(vk.physical, nullptr, &count, nullptr);
		std::vector<VkExtensionProperties> deviceExts(count);
		vkEnumerateDeviceExtensionProperties(vk.physical, nullptr, &count, deviceExts.data());
		std::vector<const char*> enabled;
		const char* required[] = { VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME };
		for (const char* name : required) {
			if (!HasExtension(deviceExts, name)) { Status("Vulkan: the driver lacks %s (needed to share images with the game)", name); return false; }
			AddUnique(enabled, name);
		}
		vk.ngxExtensions = false;
#ifndef NVR_DLSS_NO_SDK
		if (wantNgx) {
			NVSDK_NGX_FeatureDiscoveryInfo discovery = NgxDiscoveryInfo();
			uint32_t ngxCount = 0;
			VkExtensionProperties* ngxExts = nullptr;
			NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(vk.instance, vk.physical, &discovery, &ngxCount, &ngxExts);
			if (NVSDK_NGX_FAILED(r)) Log("NGX: device extension query failed: %s", NgxResultString(r).c_str());
			else {
				vk.ngxExtensions = true;
				for (uint32_t i = 0; i < ngxCount && ngxExts; i++) {
					if (HasExtension(deviceExts, ngxExts[i].extensionName)) AddUnique(enabled, owned(ngxExts[i].extensionName));
					else { vk.ngxExtensions = false; Log("NGX wants the device extension %s, which the driver does not have", ngxExts[i].extensionName); }
				}
			}
		}
		vk.rrExtensions = false;
		if (wantNgx && wantRr) {
			// UNOFFICIAL 2026-10-30 DLSS Ray Reconstruction's device extensions, added to DLSS SR's. One the driver lacks turns RR off
			// (InitNgx says why); a failed query is only logged (RR then tries with SR's list; a failed creation turns RR off).
			vk.rrExtensions = true;
			NVSDK_NGX_FeatureDiscoveryInfo discovery = NgxDiscoveryInfo(NVSDK_NGX_Feature_RayReconstruction);
			uint32_t ngxCount = 0;
			VkExtensionProperties* ngxExts = nullptr;
			NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(vk.instance, vk.physical, &discovery, &ngxCount, &ngxExts);
			if (NVSDK_NGX_FAILED(r)) Log("NGX: Ray Reconstruction's device extension query failed: %s (DLSS SR's extensions are used)", NgxResultString(r).c_str());
			else {
				for (uint32_t i = 0; i < ngxCount && ngxExts; i++) {
					if (HasExtension(deviceExts, ngxExts[i].extensionName)) AddUnique(enabled, owned(ngxExts[i].extensionName));
					else { vk.rrExtensions = false; Log("NGX wants the device extension %s for Ray Reconstruction, which the driver does not have", ngxExts[i].extensionName); }
				}
			}
		}
#endif
		std::string list;
		for (const char* n : enabled) { list += " "; list += n; }
		Log("Vulkan device extensions:%s", list.c_str());

		VkPhysicalDeviceVulkan12Features supported12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
		VkPhysicalDeviceFeatures2 supported = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &supported12 };
		vkGetPhysicalDeviceFeatures2(vk.physical, &supported);
		if (!supported12.timelineSemaphore) { Status("Vulkan: no timeline semaphores"); return false; }
		VkPhysicalDeviceVulkan12Features features12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
		features12.timelineSemaphore = VK_TRUE;
		features12.bufferDeviceAddress = supported12.bufferDeviceAddress;	// NGX may use it
		features12.hostQueryReset = supported12.hostQueryReset;
		VkPhysicalDeviceFeatures2 features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &features12 };
		features.features.shaderStorageImageWriteWithoutFormat = supported.features.shaderStorageImageWriteWithoutFormat;
		features.features.shaderStorageImageReadWithoutFormat = supported.features.shaderStorageImageReadWithoutFormat;
		features.features.shaderInt16 = supported.features.shaderInt16;

		float priority = 1.0f;
		VkDeviceQueueCreateInfo queueInfo = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
		queueInfo.queueFamilyIndex = vk.queueFamily;
		queueInfo.queueCount = 1;
		queueInfo.pQueuePriorities = &priority;
		// UNOFFICIAL 2026-10-30 (DLAA stall report): --gpu-priority high: the queue at the global priority HIGH where the driver offers
		// VK_KHR_global_priority or VK_EXT_global_priority (the same structure); a driver that does not permit it gets the device without.
		VkDeviceQueueGlobalPriorityCreateInfoEXT globalPriority = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_EXT };
		globalPriority.globalPriority = VK_QUEUE_GLOBAL_PRIORITY_HIGH_EXT;
		const char* priorityExtension = nullptr;
		if (g_highPriority) {
			if (HasExtension(deviceExts, "VK_KHR_global_priority")) priorityExtension = "VK_KHR_global_priority";
			else if (HasExtension(deviceExts, "VK_EXT_global_priority")) priorityExtension = "VK_EXT_global_priority";
			if (priorityExtension) {
				AddUnique(enabled, priorityExtension);
				queueInfo.pNext = &globalPriority;
			}
			else Log("GPU priority: the driver offers no global queue priority (VK_KHR/EXT_global_priority); the queue stays at the default");
		}
		VkDeviceCreateInfo deviceInfo = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &features };
		deviceInfo.queueCreateInfoCount = 1;
		deviceInfo.pQueueCreateInfos = &queueInfo;
		deviceInfo.enabledExtensionCount = (uint32_t)enabled.size();
		deviceInfo.ppEnabledExtensionNames = enabled.data();
		VkResult created = vkCreateDevice(vk.physical, &deviceInfo, nullptr, &vk.device);
		if (priorityExtension && created == VK_ERROR_NOT_PERMITTED_EXT) {
			Log("GPU priority: %s HIGH is not permitted here; the queue stays at the default", priorityExtension);
			queueInfo.pNext = nullptr;
			created = vkCreateDevice(vk.physical, &deviceInfo, nullptr, &vk.device);
		}
		else if (priorityExtension && created == VK_SUCCESS) Log("GPU priority: the queue is at the global priority HIGH (%s)", priorityExtension);
		VK_CHECK(created, "vkCreateDevice");
		vkGetDeviceQueue(vk.device, vk.queueFamily, 0, &vk.queue);
		vk.getMemoryWin32Handle = (PFN_vkGetMemoryWin32HandleKHR)vkGetDeviceProcAddr(vk.device, "vkGetMemoryWin32HandleKHR");
		vk.getSemaphoreWin32Handle = (PFN_vkGetSemaphoreWin32HandleKHR)vkGetDeviceProcAddr(vk.device, "vkGetSemaphoreWin32HandleKHR");
		vk.importSemaphoreWin32Handle = (PFN_vkImportSemaphoreWin32HandleKHR)vkGetDeviceProcAddr(vk.device, "vkImportSemaphoreWin32HandleKHR");
		if (!vk.getMemoryWin32Handle || !vk.getSemaphoreWin32Handle || !vk.importSemaphoreWin32Handle) { Status("Vulkan: the Win32 handle functions are missing"); return false; }
		return true;
	}

	void DestroyVulkanDevice(VulkanDevice& vk) {
		if (vk.device) vkDestroyDevice(vk.device, nullptr);
		if (vk.instance) vkDestroyInstance(vk.instance, nullptr);
		vk = VulkanDevice();
	}

	int32_t FindMemoryType(const VulkanDevice& vk, uint32_t bits, VkMemoryPropertyFlags flags) {
		for (uint32_t i = 0; i < vk.memory.memoryTypeCount; i++)
			if ((bits & (1u << i)) && (vk.memory.memoryTypes[i].propertyFlags & flags) == flags) return (int32_t)i;
		return -1;
	}

	struct SharedImage {
		VkImage			image = VK_NULL_HANDLE;
		VkDeviceMemory	memory = VK_NULL_HANDLE;
		VkImageView		view = VK_NULL_HANDLE;
		VkFormat		format = VK_FORMAT_UNDEFINED;
		uint32_t		width = 0, height = 0;
	};

	VkImageCreateInfo ImageInfo(const NvrDlssImageDesc& d, void* next) {
		VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, next };
		info.flags = d.flags;
		info.imageType = VK_IMAGE_TYPE_2D;
		info.format = (VkFormat)d.vkFormat;
		info.extent = { d.width, d.height, 1 };
		info.mipLevels = 1;
		info.arrayLayers = 1;
		info.samples = VK_SAMPLE_COUNT_1_BIT;
		info.tiling = (VkImageTiling)d.tiling;
		info.usage = d.usage;
		info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		return info;
	}

	// The importer's side of the fork's ID3D9NvrExtSharedImage: the identical image, its memory imported (dedicated, same size
	// and memory type, as Vulkan requires for OPAQUE_WIN32 handles).
	bool ImportImage(const VulkanDevice& vk, const NvrDlssImageDesc& d, const char* name, SharedImage& out) {
		VkExternalMemoryImageCreateInfo external = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
		external.handleTypes = (VkExternalMemoryHandleTypeFlags)d.handleType;
		VkImageCreateInfo info = ImageInfo(d, &external);
		VK_CHECK(vkCreateImage(vk.device, &info, nullptr, &out.image), "vkCreateImage (shared)");
		VkMemoryRequirements requirements = {};
		vkGetImageMemoryRequirements(vk.device, out.image, &requirements);
		if (requirements.size > d.allocationSize || !(requirements.memoryTypeBits & (1u << d.memoryTypeIndex))) {
			Status("Vulkan: the %s image does not fit the game's allocation (needs %llu bytes, types 0x%X; the game's: %llu bytes, type %u)",
				name, (unsigned long long)requirements.size, requirements.memoryTypeBits, (unsigned long long)d.allocationSize, d.memoryTypeIndex);
			return false;
		}
		VkMemoryDedicatedAllocateInfo dedicated = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
		dedicated.image = out.image;
		VkImportMemoryWin32HandleInfoKHR import = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, d.dedicated ? &dedicated : nullptr };
		import.handleType = (VkExternalMemoryHandleTypeFlagBits)d.handleType;
		import.handle = (HANDLE)(uintptr_t)d.handle;
		VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &import };
		alloc.allocationSize = d.allocationSize;
		alloc.memoryTypeIndex = d.memoryTypeIndex;
		VK_CHECK(vkAllocateMemory(vk.device, &alloc, nullptr, &out.memory), "vkAllocateMemory (import)");
		CloseHandle((HANDLE)(uintptr_t)d.handle);	// importing does not take ownership of an NT handle
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
		Log("imported the %s image: %ux%u, format %u, %llu bytes, memory type %u", name, d.width, d.height, d.vkFormat,
			(unsigned long long)d.allocationSize, d.memoryTypeIndex);
		return true;
	}

	bool ImportTimeline(const VulkanDevice& vk, uint64_t handle, const char* name, VkSemaphore& out) {
		VkSemaphoreTypeCreateInfo type = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
		type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
		type.initialValue = 0;
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

	// ---------------------------------------------------------------------------------------------------------------------
	// The helper proper (--shm).

	struct Helper {
		VulkanDevice	vk;
		SharedImage		images[NVR_DLSS_IMAGE_COUNT];
		VkSemaphore		inputsReady = VK_NULL_HANDLE, outputReady = VK_NULL_HANDLE;
		VkCommandPool	pool = VK_NULL_HANDLE;
		VkCommandBuffer	cmd[kCmdRing] = {};
		uint64_t		cmdFrame[kCmdRing] = {};
		VkQueryPool		queries = VK_NULL_HANDLE;
		VkBuffer		marker = VK_NULL_HANDLE;
		VkDeviceMemory	markerMemory = VK_NULL_HANDLE;
		bool			markerReady = false;
		uint32_t		mode = NVR_DLSS_MODE_PASSTHROUGH;
		uint64_t		gpuUs[64] = {}, responseUs[64] = {};
		double			inputsWaitMsSum = 0.0, inputsWaitMsMax = 0.0;	// UNOFFICIAL 2026-10-30: --wait cpu's waits for the game's inputs
		uint32_t		inputsWaitCount = 0;
		uint32_t		gpuCount = 0, responseCount = 0;
		bool			loggedFirst = false;
		// UNOFFICIAL 2026-10-30 protocol 2: the images NVR made (4, or 8 with the RR guides) and whether RR frames can come.
		uint32_t		imageCount = NVR_DLSS_IMAGE_COUNT_SR;
		bool			rrWanted = false;		// NVR_DLSS_FEATURE_RR with the eight images
		bool			loggedFirstRr = false;
		// UNOFFICIAL 2026-10-30 --inputs exact (RecordExactInputs): colour, depth and motion at exactly the render size, the helper's own
		SharedImage		exact[4];		// (UNOFFICIAL 2026-11-01: [3] the bias mask's, with hasBiasMask)
		uint32_t		exactW = 0, exactH = 0;
		// UNOFFICIAL 2026-11-01 protocol 3: DLSS SR's bias current colour mask (NvrDlssShared::biasMask, NVR_DLSS_FEATURE_BIAS_MASK)
		SharedImage		biasMask;
		bool			hasBiasMask = false;
		bool			loggedFirstMask = false;
		bool			exactFailed = false;
		bool			failTestDone = false;	// --fail-test report: the failure was reported
		bool			subrectFailed = false;	// UNOFFICIAL 2026-10-31: the pass-through test's render subrect check failed (reported once)
#ifndef NVR_DLSS_NO_SDK
		bool			ngxInit = false;
		bool			dlssAvailable = false;
		NVSDK_NGX_Parameter* ngxParams = nullptr;
		NVSDK_NGX_Handle* dlss = nullptr;
		uint32_t		dlssKey[7] = {};		// render, display, HDR, quality mode, preset (UNOFFICIAL 2026-10-30)
		bool			dlssFailed = false;
		bool			srPresetHints = false;	// UNOFFICIAL 2026-10-30: the preset hints were written once (a non-default preset)
		// UNOFFICIAL 2026-10-31: NGX's dynamic range for the DLSS SR feature's quality mode (min W, min H, max W, max H; all 0: NGX
		// reported none) and whether a render subrect outside it was logged for this feature (SubrectProblem)
		uint32_t		dlssRange[4] = {};
		bool			loggedRange = false;
		// UNOFFICIAL 2026-10-30 DLSS Ray Reconstruction (DLSS-D): its own feature handle and its own parameter map, so no RR key
		// (DLSS.Denoise.Mode and the others, which nvngx_dlss.dll's SR creation reads too) ever reaches a DLSS SR creation or
		// evaluate. Both features stay alive while frames alternate (re-creating one costs a vkDeviceWaitIdle).
		NVSDK_NGX_Parameter* rrParams = nullptr;
		NVSDK_NGX_Handle* dlssd = nullptr;
		uint32_t		dlssdKey[7] = {};		// render, display, quality mode, RR preset, depth option
		bool			rrFailed = false;
		float			rrWorldToView[16] = {}, rrViewToClip[16] = {};	// the frame's matrices; NGX gets pointers to these
#endif
	};

	bool CreateMarker(Helper& h) {
		// Two boxes in the output's format (RGBA16F): magenta, which moves along 8 places with the frame number (frame order),
		// and green, which stays (the helper is in the loop). Values above 1 so they stay visible after tonemapping.
		if (h.images[NVR_DLSS_IMAGE_OUTPUT].format != VK_FORMAT_R16G16B16A16_SFLOAT) {
			Log("the output is not RGBA16F: no marker in the pass-through test");
			return true;
		}
		const VkDeviceSize pixels = (VkDeviceSize)kMarkerSize * kMarkerSize, bytes = pixels * 8 * 2;
		VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
		info.size = bytes;
		info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		VK_CHECK(vkCreateBuffer(h.vk.device, &info, nullptr, &h.marker), "vkCreateBuffer (marker)");
		VkMemoryRequirements req = {};
		vkGetBufferMemoryRequirements(h.vk.device, h.marker, &req);
		int32_t type = FindMemoryType(h.vk, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		if (type < 0) { Status("Vulkan: no host-visible memory for the marker"); return false; }
		VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
		alloc.allocationSize = req.size;
		alloc.memoryTypeIndex = (uint32_t)type;
		VK_CHECK(vkAllocateMemory(h.vk.device, &alloc, nullptr, &h.markerMemory), "vkAllocateMemory (marker)");
		VK_CHECK(vkBindBufferMemory(h.vk.device, h.marker, h.markerMemory, 0), "vkBindBufferMemory (marker)");
		uint16_t* data = nullptr;
		VK_CHECK(vkMapMemory(h.vk.device, h.markerMemory, 0, bytes, 0, (void**)&data), "vkMapMemory (marker)");
		const float colors[2][4] = { { 4.0f, 0.0f, 4.0f, 1.0f }, { 0.0f, 4.0f, 0.0f, 1.0f } };
		for (int box = 0; box < 2; box++)
			for (VkDeviceSize p = 0; p < pixels; p++)
				for (int c = 0; c < 4; c++) data[(box * pixels + p) * 4 + c] = FloatToHalf(colors[box][c]);
		vkUnmapMemory(h.vk.device, h.markerMemory);
		h.markerReady = true;
		return true;
	}

#ifndef NVR_DLSS_NO_SDK
	// UNOFFICIAL 2026-10-30 DLSS Ray Reconstruction (DLSS-D): the NGX string keys the helper uses that have no #define in the local
	// SDK headers (NVIDIA's nvsdk_ngx_defs_dlssd.h and nvsdk_ngx_helpers_dlssd_vk.h are not in the local copy), each looked for as an
	// exact line in the string dumps of nvngx_dlss.dll 310.9.16 ("rel") and of the driver's NGX core _nvngx.dll 32.0.15.8266
	// ("core") on 2026-10-30:
	//   SuperSamplingDenoising.Available              core: found verbatim (rel: not there)
	//   SuperSamplingDenoising.NeedsUpdatedDriver     core: found verbatim
	//   SuperSamplingDenoising.MinDriverVersionMajor  core: found verbatim
	//   SuperSamplingDenoising.MinDriverVersionMinor  core: found verbatim
	//   SuperSamplingDenoising.FeatureInitResult      core: found verbatim
	//   DLSS.Denoise.Mode                             rel: found verbatim (core too). The value 1 (NVSDK_NGX_DLSS_Denoise_Mode_DLUnified) is
	//                                                 from memory; rel holds the enum's names as strings, _Off then _DLUnified, next to
	//                                                 "DLSS-D Mode %s (%d)", which fits Off 0 / DLUnified 1.
	//   DLSS.Use.HW.Depth                             rel: found verbatim. 1 = hardware depth, 0 = linear depth: from memory.
	//   DLSS.Roughness.Mode                           rel: found verbatim; deliberately not set (0, its default, is "unpacked": a roughness
	//                                                 image of its own; from memory)
	//   DLSS.Input.DiffuseAlbedo                      rel: found verbatim
	//   DLSS.Input.SpecularAlbedo                     rel: found verbatim
	//   DLSS.Input.Normals                            NOT found as such (only DLSS.Input.Normals.Subrect.Base.X/Y are). Believed (memory) to be
	//                                                 a macro alias of GBuffer.Normals (#defined locally, found verbatim in rel): both are set.
	//   DLSS.Input.Roughness                          NOT found as such (likewise); believed an alias of GBuffer.Roughness (#defined locally,
	//                                                 found verbatim in rel): both are set.
	//   WorldToViewMatrix                             rel: found verbatim (a float[16] by void pointer: memory)
	//   ViewToClipMatrix                              rel: found verbatim (likewise)
	//   RayReconstruction.Hint.Render.Preset.DLAA, .Quality, .Balanced, .Performance, .UltraPerformance, .UltraQuality
	//                                                 rel: all six found verbatim (nvsdk_ngx_s.lib too). The values (Default 0, D 4, E 5) are
	//                                                 the static library's debug types (NVSDK_NGX_RayReconstruction_Hint_Render_Preset).
	// Every other key (Width, OutWidth, PerfQualityValue, DLSS.Feature.Create.Flags, Color, Depth, MotionVectors, Jitter.Offset.X,
	// GBuffer.Normals, the DLSS SR preset hints, ...) is a #define of the local nvsdk_ngx_defs.h and is used by its macro name.
	const char* const kRrKeyAvailable          = "SuperSamplingDenoising.Available";
	const char* const kRrKeyNeedsUpdatedDriver = "SuperSamplingDenoising.NeedsUpdatedDriver";
	const char* const kRrKeyMinDriverMajor     = "SuperSamplingDenoising.MinDriverVersionMajor";
	const char* const kRrKeyMinDriverMinor     = "SuperSamplingDenoising.MinDriverVersionMinor";
	const char* const kRrKeyFeatureInitResult  = "SuperSamplingDenoising.FeatureInitResult";
	const char* const kRrKeyDenoiseMode        = "DLSS.Denoise.Mode";
	const char* const kRrKeyUseHwDepth         = "DLSS.Use.HW.Depth";
	const char* const kRrKeyDiffuseAlbedo      = "DLSS.Input.DiffuseAlbedo";
	const char* const kRrKeySpecularAlbedo     = "DLSS.Input.SpecularAlbedo";
	const char* const kRrKeyInputNormals       = "DLSS.Input.Normals";
	const char* const kRrKeyInputRoughness     = "DLSS.Input.Roughness";
	const char* const kRrKeyWorldToView        = "WorldToViewMatrix";
	const char* const kRrKeyViewToClip         = "ViewToClipMatrix";
	const int kRrDenoiseModeDLUnified = 1;

	// UNOFFICIAL 2026-10-30 the presets: one hint per DLSS quality mode, in this order: DLAA, Quality, Balanced, Performance,
	// Ultra Performance, Ultra Quality. DLSS SR's keys and letters are the local nvsdk_ngx_defs.h's; RR's keys are listed above.
	const char* const kSrPresetKeys[6] = { NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
		NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
		NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality };
	const unsigned int kSrPresetHints[NVR_DLSS_PRESET_COUNT][6] = {
		{ NVSDK_NGX_DLSS_Hint_Render_Preset_Default, NVSDK_NGX_DLSS_Hint_Render_Preset_Default, NVSDK_NGX_DLSS_Hint_Render_Preset_Default,
		  NVSDK_NGX_DLSS_Hint_Render_Preset_Default, NVSDK_NGX_DLSS_Hint_Render_Preset_Default, NVSDK_NGX_DLSS_Hint_Render_Preset_Default },
		{ NVSDK_NGX_DLSS_Hint_Render_Preset_F, NVSDK_NGX_DLSS_Hint_Render_Preset_E, NVSDK_NGX_DLSS_Hint_Render_Preset_E,	// CNN
		  NVSDK_NGX_DLSS_Hint_Render_Preset_E, NVSDK_NGX_DLSS_Hint_Render_Preset_F, NVSDK_NGX_DLSS_Hint_Render_Preset_E },
		{ NVSDK_NGX_DLSS_Hint_Render_Preset_K, NVSDK_NGX_DLSS_Hint_Render_Preset_K, NVSDK_NGX_DLSS_Hint_Render_Preset_K,	// transformer
		  NVSDK_NGX_DLSS_Hint_Render_Preset_M, NVSDK_NGX_DLSS_Hint_Render_Preset_L, NVSDK_NGX_DLSS_Hint_Render_Preset_K },
		{ NVSDK_NGX_DLSS_Hint_Render_Preset_J, NVSDK_NGX_DLSS_Hint_Render_Preset_J, NVSDK_NGX_DLSS_Hint_Render_Preset_J,	// transformer J
		  NVSDK_NGX_DLSS_Hint_Render_Preset_J, NVSDK_NGX_DLSS_Hint_Render_Preset_J, NVSDK_NGX_DLSS_Hint_Render_Preset_J },
	};
	const char* const kSrPresetNames[NVR_DLSS_PRESET_COUNT] = { "default (no hint)", "CNN (E; F at DLAA and Ultra Performance)",
		"transformer (K; M at Performance, L at Ultra Performance)", "transformer J" };
	const char* const kRrPresetKeys[6] = { "RayReconstruction.Hint.Render.Preset.DLAA", "RayReconstruction.Hint.Render.Preset.Quality",
		"RayReconstruction.Hint.Render.Preset.Balanced", "RayReconstruction.Hint.Render.Preset.Performance",
		"RayReconstruction.Hint.Render.Preset.UltraPerformance", "RayReconstruction.Hint.Render.Preset.UltraQuality" };
	// default (no hint), D, E, F: the same in every mode. UNOFFICIAL 2026-11-15: F = 6 (nvngx_dlssd.dll 310.9.16 names Preset_A ..
	// Preset_F in this order after Default 0, as D 4 and E 5 above). AUTO is never sent (ChooseRrModel turns it into F or E): 5 to be safe.
	const unsigned int kRrPresetHints[NVR_DLSS_RR_PRESET_COUNT] = { 0, 4, 5, 6, 5 };
	const char* const kRrPresetNames[NVR_DLSS_RR_PRESET_COUNT] = { "default (no hint)", "D", "E", "F", "auto" };

	// NVR's numbers, anything out of range = the default (no hint).
	uint32_t SrPreset(uint32_t preset) { return preset < NVR_DLSS_PRESET_COUNT ? preset : (uint32_t)NVR_DLSS_PRESET_DEFAULT; }

	// UNOFFICIAL 2026-10-30 DLSS Ray Reconstruction's capability keys (the driver NGX core's), from the capability parameters DLSS
	// SR's check reads. True: RR can be tried (its feature creation still decides); false with the reason. Reads only (the probe
	// uses it too).
	bool RayReconstructionCapability(Helper& h, std::string& why) {
		if (!h.ngxParams) { why = "no NGX capability parameters"; return false; }
		int available = 0, needsDriver = 0, initResult = 0;
		unsigned int minMajor = 0, minMinor = 0;
		const NVSDK_NGX_Result r = NVSDK_NGX_Parameter_GetI(h.ngxParams, kRrKeyAvailable, &available);
		NVSDK_NGX_Parameter_GetI(h.ngxParams, kRrKeyNeedsUpdatedDriver, &needsDriver);
		NVSDK_NGX_Parameter_GetUI(h.ngxParams, kRrKeyMinDriverMajor, &minMajor);
		NVSDK_NGX_Parameter_GetUI(h.ngxParams, kRrKeyMinDriverMinor, &minMinor);
		NVSDK_NGX_Parameter_GetI(h.ngxParams, kRrKeyFeatureInitResult, &initResult);
		const bool dll = FileNextToHelper(L"nvngx_dlssd.dll");
		char text[384];
		if (NVSDK_NGX_FAILED(r))
			snprintf(text, sizeof(text), "the driver's NGX does not report it: no SuperSamplingDenoising.Available, %s%s", NgxResultString(r).c_str(),
				dll ? "" : "; nvngx_dlssd.dll is not next to the helper either");
		else if (!available) {
			if (needsDriver) snprintf(text, sizeof(text), "the driver is too old: %u.%u or newer needed", minMajor, minMinor);
			else if (!dll) snprintf(text, sizeof(text), "nvngx_dlssd.dll, NVIDIA's Ray Reconstruction runtime, is not next to the helper");
			else snprintf(text, sizeof(text), "SuperSamplingDenoising not available, init result %s", NgxResultString((NVSDK_NGX_Result)initResult).c_str());
		}
		else if (!h.vk.rrExtensions) snprintf(text, sizeof(text), "the driver lacks a Vulkan extension NGX asked for it, see the log");
		else {
			if (!dll) Log("NGX: Ray Reconstruction is reported available although nvngx_dlssd.dll is not next to the helper (NGX found one elsewhere)");
			return true;
		}
		why = text;
		return false;
	}

	// NGX init and the capability check. Not available (any GTX card, an old driver, no nvngx_dlss.dll): logged, false.
	bool InitNgx(Helper& h) {
		NVSDK_NGX_FeatureDiscoveryInfo discovery = NgxDiscoveryInfo();
		NVSDK_NGX_FeatureRequirement requirement = {};
		NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_GetFeatureRequirements(h.vk.instance, h.vk.physical, &discovery, &requirement);
		if (g_shared) g_shared->ngxResult = (int32_t)r;
		if (r == NVSDK_NGX_Result_FAIL_FeatureNotSupported) {
			// What NGX answers on a GPU without DLSS (any GTX card: no tensor cores).
			Status("NGX: DLSS not available on this GPU (%s: NGX says DLSS is not supported here; it needs a GeForce RTX 20 series or newer)",
				h.vk.properties.deviceName);
			return false;
		}
		if (NVSDK_NGX_FAILED(r)) Log("NGX: feature requirement query failed: %s", NgxResultString(r).c_str());
		else if (requirement.FeatureSupported != 0) {
			const unsigned s = (unsigned)requirement.FeatureSupported;
			Status("NGX: DLSS not available on this GPU (%s%s%s%s%s; minimum GPU architecture 0x%X)",
				(s & NVSDK_NGX_FeatureSupportResult_AdapterUnsupported) ? "the GPU has no DLSS support (RTX 20 series or newer needed) " : "",
				(s & NVSDK_NGX_FeatureSupportResult_DriverVersionUnsupported) ? "the driver is too old " : "",
				(s & NVSDK_NGX_FeatureSupportResult_OSVersionBelowMinimumSupported) ? "Windows is too old " : "",
				(s & NVSDK_NGX_FeatureSupportResult_CheckNotPresent) ? "no check present " : "",
				(s & NVSDK_NGX_FeatureSupportResult_NotImplemented) ? "not implemented " : "", requirement.MinHWArchitecture);
			return false;
		}
		if (!h.vk.ngxExtensions) {
			Status("NGX: DLSS not available on this GPU (NGX refused this GPU's Vulkan device, or the driver lacks an extension NGX needs)");
			return false;
		}
		r = NVSDK_NGX_VULKAN_Init_with_ProjectID(kNgxProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kNgxEngineVersion, g_ngxDataPath.c_str(),
			h.vk.instance, h.vk.physical, h.vk.device, vkGetInstanceProcAddr, vkGetDeviceProcAddr, &g_ngxFeatureInfo, NVSDK_NGX_Version_API);
		if (g_shared) g_shared->ngxResult = (int32_t)r;
		if (NVSDK_NGX_FAILED(r)) {
			Status("NGX: DLSS not available on this GPU (NGX init failed: %s)", NgxResultString(r).c_str());
			return false;
		}
		h.ngxInit = true;
		r = NVSDK_NGX_VULKAN_GetCapabilityParameters(&h.ngxParams);
		if (NVSDK_NGX_FAILED(r) || !h.ngxParams) {
			Status("NGX: DLSS not available on this GPU (no capability parameters: %s)", NgxResultString(r).c_str());
			return false;
		}
		int available = 0, needsDriver = 0;
		unsigned int minMajor = 0, minMinor = 0;
		int initResult = 0;
		NVSDK_NGX_Parameter_GetI(h.ngxParams, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
		NVSDK_NGX_Parameter_GetI(h.ngxParams, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsDriver);
		NVSDK_NGX_Parameter_GetUI(h.ngxParams, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &minMajor);
		NVSDK_NGX_Parameter_GetUI(h.ngxParams, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minMinor);
		NVSDK_NGX_Parameter_GetI(h.ngxParams, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &initResult);
		if (!available) {
			if (needsDriver) Status("NGX: DLSS not available on this GPU (the driver is too old: %u.%u or newer needed)", minMajor, minMinor);
			else Status("NGX: DLSS not available on this GPU (SuperSampling not available, init result %s)", NgxResultString((NVSDK_NGX_Result)initResult).c_str());
			return false;
		}
		h.dlssAvailable = true;
		Status("NGX: DLSS available on %s", h.vk.properties.deviceName);
		if (h.mode == NVR_DLSS_MODE_DLSS && h.rrWanted && g_shared) {
			// UNOFFICIAL 2026-10-30 DLSS Ray Reconstruction, only when NVR made the RR images: available or not, DLSS SR runs on (this
			// never returns false). RR's own parameter map is made here, so a failure shows before the first frame.
			std::string why;
			bool rr = RayReconstructionCapability(h, why);
			if (rr) {
				r = NVSDK_NGX_VULKAN_AllocateParameters(&h.rrParams);
				if (NVSDK_NGX_FAILED(r) || !h.rrParams) {
					rr = false;
					h.rrParams = nullptr;
					why = "no parameter map of its own: " + NgxResultString(r);
				}
			}
			g_shared->rrAvailable = rr ? 1u : 0u;
			if (rr) Status("NGX: Ray Reconstruction available on %s", h.vk.properties.deviceName);
			else Status("NGX: Ray Reconstruction not available (%s); DLSS SR goes on", why.c_str());
		}
		return true;
	}

	// UNOFFICIAL 2026-10-30 (top-left picture report): NVIDIA's NGX_DLSS_GET_OPTIMAL_SETTINGS (nvsdk_ngx_helpers.h, which also pulls in the D3D
	// and CUDA helpers), the same steps on the generic parameter API: DLSS's optimal render size and dynamic range for a mode and display.
	// (UNOFFICIAL 2026-10-31 dynamic: all four dynamic-range values came back, non-zero and in order, for the render subrect check)
	NVSDK_NGX_Result DlssOptimalSettings(NVSDK_NGX_Parameter* params, unsigned int width, unsigned int height, NVSDK_NGX_PerfQuality_Value quality,
		unsigned int& optW, unsigned int& optH, unsigned int& maxW, unsigned int& maxH, unsigned int& minW, unsigned int& minH, bool& dynamic) {
		dynamic = false;
		void* callback = nullptr;
		NVSDK_NGX_Parameter_GetVoidPointer(params, NVSDK_NGX_Parameter_DLSSOptimalSettingsCallback, &callback);
		if (!callback) return NVSDK_NGX_Result_FAIL_OutOfDate;
		NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_Width, width);
		NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_Height, height);
		NVSDK_NGX_Parameter_SetI(params, NVSDK_NGX_Parameter_PerfQualityValue, quality);
		NVSDK_NGX_Parameter_SetI(params, NVSDK_NGX_Parameter_RTXValue, false);	// (NVIDIA's helper: some older DLSS DLLs still expect it)
		const NVSDK_NGX_Result r = ((PFN_NVSDK_NGX_DLSS_GetOptimalSettingsCallback)callback)(params);
		if (NVSDK_NGX_FAILED(r)) return r;
		NVSDK_NGX_Parameter_GetUI(params, NVSDK_NGX_Parameter_OutWidth, &optW);
		NVSDK_NGX_Parameter_GetUI(params, NVSDK_NGX_Parameter_OutHeight, &optH);
		maxW = minW = optW;	// older DLSS DLLs without dynamic resolution leave these unset
		maxH = minH = optH;
		int got = 0;
		got += !NVSDK_NGX_FAILED(NVSDK_NGX_Parameter_GetUI(params, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Width, &maxW));
		got += !NVSDK_NGX_FAILED(NVSDK_NGX_Parameter_GetUI(params, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Height, &maxH));
		got += !NVSDK_NGX_FAILED(NVSDK_NGX_Parameter_GetUI(params, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Width, &minW));
		got += !NVSDK_NGX_FAILED(NVSDK_NGX_Parameter_GetUI(params, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Height, &minH));
		dynamic = got == 4 && minW && minH && minW <= maxW && minH <= maxH;
		return r;
	}

	// DLSS's quality mode from render / display width (NVR's RenderScale; DLSS's own ratios: Quality 0.667, Balanced 0.58,
	// Performance 0.5, Ultra Performance 0.333, DLAA 1). NVR sends quality 0 = "pick it here".
	NVSDK_NGX_PerfQuality_Value QualityFor(uint32_t renderWidth, uint32_t displayWidth) {
		const float r = displayWidth ? (float)renderWidth / (float)displayWidth : 1.0f;
		if (r >= 0.99f) return NVSDK_NGX_PerfQuality_Value_DLAA;
		if (r >= 0.66f) return NVSDK_NGX_PerfQuality_Value_MaxQuality;
		if (r >= 0.57f) return NVSDK_NGX_PerfQuality_Value_Balanced;
		if (r >= 0.49f) return NVSDK_NGX_PerfQuality_Value_MaxPerf;
		return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
	}

	// The DLSS feature for these sizes / HDR / quality, (re)created inside this frame's command buffer when they change.
	// UNOFFICIAL 2026-10-30: and NVR's model preset ([Shaders.TAA.DLSS] Preset), whose hints are read at creation only.
	bool EnsureDlss(Helper& h, VkCommandBuffer cmd, const NvrDlssFrame& f) {
		const SharedImage& out = h.images[NVR_DLSS_IMAGE_OUTPUT];
		const uint32_t quality = (uint32_t)QualityFor(f.renderWidth, out.width);
		const uint32_t preset = SrPreset(f.preset);
		const uint32_t key[7] = { f.renderWidth, f.renderHeight, out.width, out.height, f.hdr, quality, preset };
		if (h.dlss && !memcmp(key, h.dlssKey, sizeof(key))) return true;
		if (h.dlss) {
			vkDeviceWaitIdle(h.vk.device);
			NVSDK_NGX_VULKAN_ReleaseFeature(h.dlss);
			h.dlss = nullptr;
		}
		// UNOFFICIAL 2026-10-30 all six hints, once a preset other than the default was asked for (the default then writes 0 = no
		// hint, which clears the earlier choice on this parameter map). Until then nothing is written: the parameter map is exactly
		// what it was before presets existed.
		if (preset != NVR_DLSS_PRESET_DEFAULT) h.srPresetHints = true;
		if (h.srPresetHints)
			for (int mode = 0; mode < 6; mode++) NVSDK_NGX_Parameter_SetUI(h.ngxParams, kSrPresetKeys[mode], kSrPresetHints[preset][mode]);
		// UNOFFICIAL 2026-10-30 (top-left picture report): the size DLSS itself would render at for this mode and display (optimal and
		// dynamic range; a DLSS override in the NVIDIA App or NVIDIA Profile Inspector changes these), next to NVR's RenderScale size. Log only.
		// UNOFFICIAL 2026-10-31: the dynamic range is also kept for the render subrect check (RecordDlss, SubrectProblem).
		memset(h.dlssRange, 0, sizeof(h.dlssRange));
		h.loggedRange = false;
		{
			unsigned int optW = 0, optH = 0, maxW = 0, maxH = 0, minW = 0, minH = 0;
			bool dynamic = false;
			const NVSDK_NGX_Result o = DlssOptimalSettings(h.ngxParams, out.width, out.height, (NVSDK_NGX_PerfQuality_Value)quality,
				optW, optH, maxW, maxH, minW, minH, dynamic);
			if (!NVSDK_NGX_FAILED(o) && dynamic) {
				h.dlssRange[0] = minW;
				h.dlssRange[1] = minH;
				h.dlssRange[2] = maxW;
				h.dlssRange[3] = maxH;
			}
			if (NVSDK_NGX_FAILED(o)) Log("NGX: DLSS's optimal settings for quality mode %u at %ux%u: not available (%s)", quality, out.width, out.height, NgxResultString(o).c_str());
			else {
				const bool inside = f.renderWidth >= minW && f.renderWidth <= maxW && f.renderHeight >= minH && f.renderHeight <= maxH;
				Log("NGX: DLSS's optimal settings for quality mode %u at %ux%u: render %ux%u, dynamic range %ux%u .. %ux%u; NVR renders %ux%u%s",
					quality, out.width, out.height, optW, optH, minW, minH, maxW, maxH, f.renderWidth, f.renderHeight,
					inside ? "" : " (outside that range: a DLSS override in the NVIDIA App or NVIDIA Profile Inspector? NVR keeps its own size)");
			}
		}
		NVSDK_NGX_DLSS_Create_Params create = {};
		create.Feature.InWidth = f.renderWidth;
		create.Feature.InHeight = f.renderHeight;
		create.Feature.InTargetWidth = out.width;
		create.Feature.InTargetHeight = out.height;
		create.Feature.InPerfQualityValue = (NVSDK_NGX_PerfQuality_Value)quality;
		create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
			(f.hdr ? (NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure) : 0);
		create.InEnableOutputSubrects = false;
		NVSDK_NGX_Result r = NGX_VULKAN_CREATE_DLSS_EXT1(h.vk.device, cmd, 1, 1, &h.dlss, h.ngxParams, &create);
		if (NVSDK_NGX_FAILED(r) || !h.dlss) {
			Status("NGX: DLSS feature creation failed (%ux%u -> %ux%u, %s, quality mode %u, preset %s): %s", f.renderWidth, f.renderHeight, out.width,
				out.height, f.hdr ? "HDR" : "LDR", quality, kSrPresetNames[preset], NgxResultString(r).c_str());
			h.dlss = nullptr;
			return false;
		}
		memcpy(h.dlssKey, key, sizeof(key));
		Log("NGX: DLSS feature made: %ux%u -> %ux%u, %s, quality mode %u, preset %s", f.renderWidth, f.renderHeight, out.width, out.height,
			f.hdr ? "HDR" : "LDR", quality, kSrPresetNames[preset]);
		return true;
	}

	// UNOFFICIAL 2026-10-30 DLSS Ray Reconstruction: the DLSS-D feature for these sizes / quality mode / RR preset / depth kind,
	// (re)created inside this frame's command buffer when they change, on RR's own parameter map. The keys are the ones NVIDIA's
	// NGX_VULKAN_CREATE_DLSS_EXT1 sets (nvsdk_ngx_helpers_vk.h) plus DLSS-D's denoise mode, depth kind and preset hints; NVIDIA's
	// own DLSS-D helper (not in the local SDK) is believed to set the same, with NVSDK_NGX_Feature_RayReconstruction.
	bool EnsureDlssd(Helper& h, VkCommandBuffer cmd, const NvrDlssFrame& f) {
		const SharedImage& out = h.images[NVR_DLSS_IMAGE_OUTPUT];
		if (!h.rrParams) {
			Status("NGX: Ray Reconstruction feature creation failed (no parameter map); DLSS SR goes on");
			return false;
		}
		const uint32_t quality = (uint32_t)QualityFor(f.renderWidth, out.width);
		// UNOFFICIAL 2026-11-15 RRPreset 3 / 4 (DLSS-RR-MODEL.md): the model actually sent (auto resolved, F without its driver -> E)
		std::string modelWhy;
		const uint32_t rrPreset = ChooseRrModel(f.rrPreset, h.vk.properties.deviceID, h.vk.properties.driverVersion, modelWhy);
		const uint32_t depthLinear = (f.rrOptions & NVR_DLSS_RR_DEPTH_LINEAR) ? 1u : 0u;
		const uint32_t key[7] = { f.renderWidth, f.renderHeight, out.width, out.height, quality, rrPreset, depthLinear };
		if (h.dlssd && !memcmp(key, h.dlssdKey, sizeof(key))) return true;
		// One line per feature (re)creation, also into the game's log (Status): which model and why.
		Status("NGX: Ray Reconstruction model: %s (%s)", kRrModelNames[rrPreset], modelWhy.c_str());
		if (h.dlssd) {
			vkDeviceWaitIdle(h.vk.device);
			NVSDK_NGX_VULKAN_ReleaseFeature(h.dlssd);
			h.dlssd = nullptr;
		}
		NVSDK_NGX_Parameter* p = h.rrParams;
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_Parameter_CreationNodeMask, 1);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_Parameter_VisibilityNodeMask, 1);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_Parameter_Width, f.renderWidth);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_Parameter_Height, f.renderHeight);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_Parameter_OutWidth, out.width);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_Parameter_OutHeight, out.height);
		NVSDK_NGX_Parameter_SetI(p, NVSDK_NGX_Parameter_PerfQualityValue, (int)quality);
		// RR frames are always linear HDR (the pre-tonemap RR point), with DLSS's own exposure as for DLSS SR's HDR frames.
		NVSDK_NGX_Parameter_SetI(p, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
			NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure);
		NVSDK_NGX_Parameter_SetI(p, NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 0);
		NVSDK_NGX_Parameter_SetI(p, kRrKeyDenoiseMode, kRrDenoiseModeDLUnified);
		NVSDK_NGX_Parameter_SetI(p, kRrKeyUseHwDepth, depthLinear ? 0 : 1);
		// DLSS.Roughness.Mode is not set: its default (unpacked) is a roughness image of its own, which NVR sends.
		for (int mode = 0; mode < 6; mode++) NVSDK_NGX_Parameter_SetUI(p, kRrPresetKeys[mode], kRrPresetHints[rrPreset]);
		NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_CreateFeature1(h.vk.device, cmd, NVSDK_NGX_Feature_RayReconstruction, p, &h.dlssd);
		if (g_shared) g_shared->rrResult = (int32_t)r;
		if (NVSDK_NGX_FAILED(r) || !h.dlssd) {
			Status("NGX: Ray Reconstruction feature creation failed (%ux%u -> %ux%u, quality mode %u, RR preset %s, %s depth): %s; DLSS SR goes on",
				f.renderWidth, f.renderHeight, out.width, out.height, quality, kRrPresetNames[rrPreset], depthLinear ? "linear" : "hardware",
				NgxResultString(r).c_str());
			h.dlssd = nullptr;
			return false;
		}
		memcpy(h.dlssdKey, key, sizeof(key));
		Log("NGX: Ray Reconstruction feature made: %ux%u -> %ux%u, quality mode %u, RR preset %s, %s depth", f.renderWidth, f.renderHeight,
			out.width, out.height, quality, kRrPresetNames[rrPreset], depthLinear ? "linear" : "hardware");
		return true;
	}
#endif

	void Barrier(VkCommandBuffer cmd, VkImage image, VkAccessFlags srcAccess, VkAccessFlags dstAccess, VkImageLayout from, VkImageLayout to,
		VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
		VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
		b.srcAccessMask = srcAccess;
		b.dstAccessMask = dstAccess;
		b.oldLayout = from;
		b.newLayout = to;
		b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.image = image;
		b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
	}

	// UNOFFICIAL 2026-10-30 (tester report: with DLSS on, the render-scaled picture stayed small in the top-left corner) --inputs exact:
	// NVR's colour, depth and motion are screen-sized images with the frame in their top-left render rectangle, and DLSS was told that
	// rectangle (InRenderSubrectDimensions). Anything that has DLSS read the whole image instead (an nvngx_dlss.dll without dynamic
	// resolution, a driver-side DLSS override) shows the small frame 1:1 in the corner. Here the rectangle is copied into images of exactly
	// the render size (the helper's own, made again when the size changes) and DLSS and the pass-through read those whole, as in the
	// plainest DLSS integration (input size = render size, no subrect). Three copies a frame, about 0.1 ms at 1080p. Also no bilinear
	// tap past the rectangle's edge in the pass-through stretch. --inputs subrect: the shared images' rectangle, as before.
	const uint32_t kExactInputs[4] = { NVR_DLSS_IMAGE_COLOR, NVR_DLSS_IMAGE_DEPTH, NVR_DLSS_IMAGE_MOTION, NVR_DLSS_IMAGE_BIAS_MASK };
	// UNOFFICIAL 2026-11-01 protocol 3: the bias mask is the fourth input copied, when NVR made it
	uint32_t ExactCount(const Helper& h) { return h.hasBiasMask ? 4u : 3u; }
	// A shared input by its protocol index (the bias mask is outside the image table).
	const SharedImage& Shared(const Helper& h, uint32_t i) { return i == NVR_DLSS_IMAGE_BIAS_MASK ? h.biasMask : h.images[i]; }
	const char* InputName(uint32_t i) { return i == NVR_DLSS_IMAGE_BIAS_MASK ? "bias mask" : kImageNames[i]; }

	void DestroyExact(Helper& h) {
		for (SharedImage& e : h.exact) {
			if (e.view) vkDestroyImageView(h.vk.device, e.view, nullptr);
			if (e.image) vkDestroyImage(h.vk.device, e.image, nullptr);
			if (e.memory) vkFreeMemory(h.vk.device, e.memory, nullptr);
			e = SharedImage();
		}
		h.exactW = h.exactH = 0;
	}

	bool MakeExact(Helper& h, uint32_t w, uint32_t ht) {
		if (h.exact[0].image && h.exactW == w && h.exactH == ht) return true;
		if (h.exact[0].image) vkDeviceWaitIdle(h.vk.device);	// frames in flight may still read them (as EnsureDlss before a re-creation)
		DestroyExact(h);
		for (uint32_t k = 0; k < ExactCount(h); k++) {
			const SharedImage& s = Shared(h, kExactInputs[k]);
			SharedImage& e = h.exact[k];
			VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
			info.imageType = VK_IMAGE_TYPE_2D;
			info.format = s.format;
			info.extent = { w, ht, 1 };
			info.mipLevels = 1;
			info.arrayLayers = 1;
			info.samples = VK_SAMPLE_COUNT_1_BIT;
			info.tiling = VK_IMAGE_TILING_OPTIMAL;
			info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
			info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			VkResult r = vkCreateImage(h.vk.device, &info, nullptr, &e.image);
			VkMemoryRequirements req = {};
			if (r == VK_SUCCESS) vkGetImageMemoryRequirements(h.vk.device, e.image, &req);
			const int32_t type = r == VK_SUCCESS ? FindMemoryType(h.vk, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) : -1;
			if (r == VK_SUCCESS && type < 0) r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
			if (r == VK_SUCCESS) {
				VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
				alloc.allocationSize = req.size;
				alloc.memoryTypeIndex = (uint32_t)type;
				r = vkAllocateMemory(h.vk.device, &alloc, nullptr, &e.memory);
			}
			if (r == VK_SUCCESS) r = vkBindImageMemory(h.vk.device, e.image, e.memory, 0);
			if (r == VK_SUCCESS) {
				VkImageViewCreateInfo view = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
				view.image = e.image;
				view.viewType = VK_IMAGE_VIEW_TYPE_2D;
				view.format = s.format;
				view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
				r = vkCreateImageView(h.vk.device, &view, nullptr, &e.view);
			}
			if (r != VK_SUCCESS) {
				Log("--inputs exact: the %ux%u image for input %u could not be made (%s)", w, ht, kExactInputs[k], VkResultName(r));
				DestroyExact(h);
				return false;
			}
			e.format = s.format;
			e.width = w;
			e.height = ht;
		}
		h.exactW = w;
		h.exactH = ht;
		Log("--inputs exact: colour, depth, motion%s are copied into %ux%u images each frame (the render size; the shared inputs are %ux%u)",
			h.hasBiasMask ? " and the bias mask" : "", w, ht, h.images[NVR_DLSS_IMAGE_COLOR].width, h.images[NVR_DLSS_IMAGE_COLOR].height);
		return true;
	}

	// This frame's render rectangle of the three inputs into the exact images: true. False: nothing to copy (render size = the inputs'),
	// --inputs subrect, or the images could not be made (logged; the shared images' rectangle from then on, as before).
	bool RecordExactInputs(Helper& h, VkCommandBuffer cmd, uint32_t rw, uint32_t rh) {
		const SharedImage& colour = h.images[NVR_DLSS_IMAGE_COLOR];
		rw = std::max(1u, std::min(rw, colour.width));
		rh = std::max(1u, std::min(rh, colour.height));
		if (!g_exactInputs || h.exactFailed || (rw == colour.width && rh == colour.height)) return false;
		if (!MakeExact(h, rw, rh)) {
			h.exactFailed = true;
			Log("--inputs exact: off for this start; DLSS reads the render rectangle of the shared images (--inputs subrect)");
			return false;
		}
		for (uint32_t k = 0; k < ExactCount(h); k++) {
			const SharedImage& s = Shared(h, kExactInputs[k]);
			const SharedImage& e = h.exact[k];
			Barrier(cmd, s.image, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
				VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
			// (UNDEFINED: the copy rewrites the whole image; the stage mask orders it after last frame's reads)
			Barrier(cmd, e.image, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
				VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
			VkImageCopy region = {};
			region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			region.extent = { rw, rh, 1 };
			vkCmdCopyImage(cmd, s.image, VK_IMAGE_LAYOUT_GENERAL, e.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
			Barrier(cmd, e.image, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
				VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
			Barrier(cmd, s.image, VK_ACCESS_TRANSFER_READ_BIT, 0, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
				VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
		}
		return true;
	}

	// The image input i is read from this frame: the exact one after RecordExactInputs, else the shared one (the output always).
	const SharedImage& InputImage(const Helper& h, uint32_t i, bool exact) {
		for (uint32_t k = 0; exact && k < ExactCount(h); k++) if (kExactInputs[k] == i) return h.exact[k];
		return Shared(h, i);
	}

	// UNOFFICIAL 2026-10-31 (RTX 4070 Ti SUPER tester, nvngx_dlss.dll 310.9.16, RenderScale 0.67 at 2560x1440): with --inputs exact the
	// helper gave DLSS SR a 0x0 render subrect, meant as "all of the exact images". 310.9.16 checks the subrect against the quality mode's
	// dynamic range and refused every evaluate ("RenderSubrect (0x0) outside of Min (1280x720) and Max (2560x1440) dynamic res"), so NVR
	// went to FSR 2 for the session. The subrect is now always the render rectangle at 0, 0 (with --inputs exact: the whole of each exact
	// image), made here for both evaluates (RecordDlss, RecordDlssd) and checked (SubrectProblem) before every evaluate and on every
	// pass-through frame: the pass-through test, which loads no NGX, checks on any NVIDIA GPU what a real evaluate would be given.
	struct DlssSubrect {
		uint32_t	x = 0, y = 0;			// every input's subrect base (colour, depth, motion)
		uint32_t	width = 0, height = 0;	// the render subrect's size
	};

	DlssSubrect DlssSubrectFor(const NvrDlssFrame& f, bool exact) {
		DlssSubrect s;
		if (g_failTest == 3 && exact) return s;	// --fail-test zero-subrect (TEST ONLY): b0146f54's 0x0, for the check to catch
		s.width = f.renderWidth;
		s.height = f.renderHeight;
		return s;
	}

	// What is wrong with the render subrect s of this frame (nullptr: nothing; else why, in buf). It must be NVR's render rectangle,
	// not empty, inside each input the evaluate reads (InputImage: the exact images or the shared ones; RR reads the shared ones) and
	// no larger than the output. range (min W, min H, max W, max H: NGX's dynamic range for the DLSS SR feature's quality mode; nullptr:
	// none known, as in the pass-through, which loads no NGX): outside it is a problem too.
	const char* SubrectProblem(const Helper& h, const NvrDlssFrame& f, const DlssSubrect& s, bool exact, const uint32_t* range, char* buf,
		size_t size) {
		const SharedImage& out = h.images[NVR_DLSS_IMAGE_OUTPUT];
		if (!s.width || !s.height) {
			snprintf(buf, size, "the render subrect is empty (%ux%u; NGX refuses it)", s.width, s.height);
			return buf;
		}
		if (s.x || s.y || s.width != f.renderWidth || s.height != f.renderHeight) {
			snprintf(buf, size, "the render subrect %ux%u at %u, %u is not NVR's render rectangle %ux%u at 0, 0", s.width, s.height, s.x, s.y,
				f.renderWidth, f.renderHeight);
			return buf;
		}
		for (uint32_t k = 0; k < ExactCount(h); k++) {
			const uint32_t i = kExactInputs[k];
			const SharedImage& in = InputImage(h, i, exact);
			if (s.x + s.width > in.width || s.y + s.height > in.height) {
				snprintf(buf, size, "the render subrect %ux%u at %u, %u reaches past the %s%s image (%ux%u)", s.width, s.height, s.x, s.y,
					exact ? "exact " : "", InputName(i), in.width, in.height);
				return buf;
			}
		}
		if (s.width > out.width || s.height > out.height) {
			snprintf(buf, size, "the render subrect %ux%u is larger than the output (%ux%u)", s.width, s.height, out.width, out.height);
			return buf;
		}
		if (range && (s.width < range[0] || s.height < range[1] || s.width > range[2] || s.height > range[3])) {
			snprintf(buf, size, "the render subrect %ux%u is outside NGX's dynamic range %ux%u .. %ux%u for this quality mode", s.width, s.height,
				range[0], range[1], range[2], range[3]);
			return buf;
		}
		return nullptr;
	}

	// Pass-through: the colour input's render rectangle stretched (bilinear) over the whole output, plus the marker boxes.
	// (UNOFFICIAL 2026-10-30 exact: from the exact colour image, RecordExactInputs)
	void RecordPassThrough(Helper& h, VkCommandBuffer cmd, const NvrDlssFrame& f, uint64_t frame, bool exact) {
		const SharedImage& in = InputImage(h, NVR_DLSS_IMAGE_COLOR, exact);
		const SharedImage& out = h.images[NVR_DLSS_IMAGE_OUTPUT];
		const uint32_t rw = std::max(1u, std::min(f.renderWidth, in.width)), rh = std::max(1u, std::min(f.renderHeight, in.height));
		Barrier(cmd, in.image, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
			VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		Barrier(cmd, out.image, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
			VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		VkImageBlit blit = {};
		blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		blit.srcOffsets[1] = { (int32_t)rw, (int32_t)rh, 1 };
		blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		blit.dstOffsets[1] = { (int32_t)out.width, (int32_t)out.height, 1 };
		vkCmdBlitImage(cmd, in.image, VK_IMAGE_LAYOUT_GENERAL, out.image, VK_IMAGE_LAYOUT_GENERAL, 1, &blit, VK_FILTER_LINEAR);
		if (h.markerReady && out.width >= 16 + 32 * 9 && out.height >= 16 + kMarkerSize) {
			Barrier(cmd, out.image, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
				VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
			VkBufferImageCopy regions[2] = {};
			for (int i = 0; i < 2; i++) {
				regions[i].bufferOffset = (VkDeviceSize)i * kMarkerSize * kMarkerSize * 8;
				regions[i].imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
				regions[i].imageExtent = { kMarkerSize, kMarkerSize, 1 };
			}
			regions[0].imageOffset = { (int32_t)(16 + 32 * (frame % 8)), 16, 0 };
			regions[1].imageOffset = { 16 + 32 * 8, 16, 0 };
			vkCmdCopyBufferToImage(cmd, h.marker, out.image, VK_IMAGE_LAYOUT_GENERAL, 2, regions);
		}
		Barrier(cmd, out.image, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL,
			VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
	}

	// UNOFFICIAL 2026-10-30 the pass-through test of an RR frame (NVR's HelperTest with RayReconstruction, any NVIDIA GPU): the same
	// stretch and marker, then the four guides' render rectangles as thumbnails along the bottom edge (GuideThumbnail), so a look
	// at the game shows that NVR's guides arrive. The blits convert the formats: roughness (R16F) shows as red, and the normals'
	// negative parts as black.
	void RecordPassThroughRR(Helper& h, VkCommandBuffer cmd, const NvrDlssFrame& f, uint64_t frame, bool exact) {
		RecordPassThrough(h, cmd, f, frame, exact);
		const SharedImage& out = h.images[NVR_DLSS_IMAGE_OUTPUT];
		const SharedImage& in = h.images[NVR_DLSS_IMAGE_COLOR];
		const uint32_t rw = std::max(1u, std::min(f.renderWidth, in.width)), rh = std::max(1u, std::min(f.renderHeight, in.height));
		VkOffset2D at = {};
		VkExtent2D size = {};
		if (h.imageCount < NVR_DLSS_IMAGE_COUNT || !GuideThumbnail(out.width, out.height, rw, rh, 0, at, size)) return;
		// The pass-through's last barrier already made its writes available; this one orders the blits after them explicitly.
		Barrier(cmd, out.image, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
			VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		for (uint32_t g = 0; g < kGuideCount; g++) {
			const SharedImage& guide = h.images[NVR_DLSS_IMAGE_DIFFUSE_ALBEDO + g];
			if (!GuideThumbnail(out.width, out.height, rw, rh, g, at, size)) break;
			Barrier(cmd, guide.image, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
				VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
			VkImageBlit blit = {};
			blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			blit.srcOffsets[1] = { (int32_t)std::min(rw, guide.width), (int32_t)std::min(rh, guide.height), 1 };
			blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			blit.dstOffsets[0] = { at.x, at.y, 0 };
			blit.dstOffsets[1] = { at.x + (int32_t)size.width, at.y + (int32_t)size.height, 1 };
			vkCmdBlitImage(cmd, guide.image, VK_IMAGE_LAYOUT_GENERAL, out.image, VK_IMAGE_LAYOUT_GENERAL, 1, &blit, VK_FILTER_LINEAR);
		}
		Barrier(cmd, out.image, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL,
			VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
	}

#ifndef NVR_DLSS_NO_SDK
	// DLSS: inputs to SHADER_READ_ONLY_OPTIMAL and the output to GENERAL for NGX, everything back to GENERAL (the fork's layout).
	// UNOFFICIAL 2026-10-30 protocol 2: DLSS SR's inputs are named (colour, depth, motion), not "every index below OUTPUT": the RR
	// guides come after OUTPUT and are not DLSS SR's.
	// (UNOFFICIAL 2026-11-01: [3] the bias mask, on frames that have it)
	const uint32_t kSrInputs[4] = { NVR_DLSS_IMAGE_COLOR, NVR_DLSS_IMAGE_DEPTH, NVR_DLSS_IMAGE_MOTION, NVR_DLSS_IMAGE_BIAS_MASK };

	// (UNOFFICIAL 2026-10-30 exact: the inputs are the exact images, RecordExactInputs; UNOFFICIAL 2026-10-31: the render subrect is the
	// whole of them, never 0x0: DlssSubrectFor, checked by SubrectProblem first)
	bool RecordDlss(Helper& h, VkCommandBuffer cmd, const NvrDlssFrame& f, bool exact) {
		if (!EnsureDlss(h, cmd, f)) return false;
		// UNOFFICIAL 2026-10-31 an empty or misplaced render subrect is never evaluated (NVR goes to FSR 2, as after a refused evaluate).
		// One outside NGX's dynamic range is logged once per feature and evaluated: NGX decides (--subrect-range strict: refused too).
		const DlssSubrect sub = DlssSubrectFor(f, exact);
		char why[256];
		if (const char* problem = SubrectProblem(h, f, sub, exact, nullptr, why, sizeof(why))) {
			Status("NGX: DLSS evaluate not called: %s", problem);
			return false;
		}
		if (h.dlssRange[2]) {
			if (const char* problem = SubrectProblem(h, f, sub, exact, h.dlssRange, why, sizeof(why))) {
				if (g_strictSubrect) {
					Status("NGX: DLSS evaluate not called (--subrect-range strict): %s", problem);
					return false;
				}
				if (!h.loggedRange) Log("NGX: %s: NGX may refuse the evaluate", problem);
				h.loggedRange = true;
			}
		}
		const VkImageLayout read = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		// UNOFFICIAL 2026-11-01 protocol 3: the bias current colour mask, on the frames NVR wrote it (WeaponMask above 0)
		const bool mask = h.hasBiasMask && (f.flags & NVR_DLSS_FRAME_BIAS_MASK) != 0;
		const uint32_t srCount = mask ? 4u : 3u;
		for (uint32_t k = 0; k < srCount; k++)
			Barrier(cmd, InputImage(h, kSrInputs[k], exact).image, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, read,
				VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
		Barrier(cmd, h.images[NVR_DLSS_IMAGE_OUTPUT].image, 0, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

		const VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		NVSDK_NGX_Resource_VK res[NVR_DLSS_IMAGE_COUNT] = {};
		for (uint32_t i = 0; i < NVR_DLSS_IMAGE_COUNT_SR; i++) {
			const SharedImage& s = InputImage(h, i, exact);
			res[i] = NVSDK_NGX_Create_ImageView_Resource_VK(s.view, s.image, range, s.format, s.width, s.height, i == NVR_DLSS_IMAGE_OUTPUT);
		}
		NVSDK_NGX_VK_DLSS_Eval_Params eval = {};
		eval.Feature.pInColor = &res[NVR_DLSS_IMAGE_COLOR];
		eval.Feature.pInOutput = &res[NVR_DLSS_IMAGE_OUTPUT];
		eval.pInDepth = &res[NVR_DLSS_IMAGE_DEPTH];
		eval.pInMotionVectors = &res[NVR_DLSS_IMAGE_MOTION];
		// UNOFFICIAL 2026-11-01 (player report: the first-person arms and gun ghost and smear with DLSS while they animate): NVR's mask, 1 on the
		// weapon (fixed to the camera: motion 0 says nothing about its own sway, bob and reload), FSR's reactive value elsewhere. Without it
		// (no flag) the key is set to null, as before.
		NVSDK_NGX_Resource_VK maskRes = {};
		if (mask) {
			const SharedImage& m = InputImage(h, NVR_DLSS_IMAGE_BIAS_MASK, exact);
			maskRes = NVSDK_NGX_Create_ImageView_Resource_VK(m.view, m.image, range, m.format, m.width, m.height, false);
			eval.pInBiasCurrentColorMask = &maskRes;
			eval.InBiasCurrentColorSubrectBase.X = sub.x;
			eval.InBiasCurrentColorSubrectBase.Y = sub.y;
		}
		eval.InJitterOffsetX = f.jitterX;
		eval.InJitterOffsetY = f.jitterY;
		// UNOFFICIAL 2026-10-31: the render rectangle, never 0x0 (nvngx_dlss.dll 310.9.16 refuses it; with --inputs exact it is all of each image)
		eval.InRenderSubrectDimensions.Width = sub.width;
		eval.InRenderSubrectDimensions.Height = sub.height;
		eval.InColorSubrectBase.X = eval.InDepthSubrectBase.X = eval.InMVSubrectBase.X = sub.x;
		eval.InColorSubrectBase.Y = eval.InDepthSubrectBase.Y = eval.InMVSubrectBase.Y = sub.y;
		eval.InReset = f.reset ? 1 : 0;
		eval.InMVScaleX = f.mvScaleX;
		eval.InMVScaleY = f.mvScaleY;
		eval.InPreExposure = f.preExposure > 0.0f ? f.preExposure : 1.0f;
		eval.InFrameTimeDeltaInMsec = f.frameTimeMs;
		NVSDK_NGX_Result r = NGX_VULKAN_EVALUATE_DLSS_EXT(cmd, h.dlss, h.ngxParams, &eval);
		if (g_shared) g_shared->ngxResult = (int32_t)r;

		for (uint32_t k = 0; k < srCount; k++)
			Barrier(cmd, InputImage(h, kSrInputs[k], exact).image, VK_ACCESS_SHADER_READ_BIT, 0, read, VK_IMAGE_LAYOUT_GENERAL,
				VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
		Barrier(cmd, h.images[NVR_DLSS_IMAGE_OUTPUT].image, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
			VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
		if (NVSDK_NGX_FAILED(r)) {
			Status("NGX: DLSS evaluate failed: %s", NgxResultString(r).c_str());
			return false;
		}
		return true;
	}

	// UNOFFICIAL 2026-10-30 DLSS Ray Reconstruction (DLSS-D) on an RR frame: as RecordDlss, with all seven inputs (every image but
	// OUTPUT) and the keys NVIDIA's NGX_VULKAN_EVALUATE_DLSS_EXT sets (the same names), plus the guides and the matrices, on RR's own
	// parameter map. False: logged, RR is off from here on (the caller runs DLSS SR on the same colour into the same command buffer:
	// the inputs are back in GENERAL whatever the evaluate did).
	bool RecordDlssd(Helper& h, VkCommandBuffer cmd, const NvrDlssFrame& f) {
		if (!EnsureDlssd(h, cmd, f)) return false;
		// UNOFFICIAL 2026-10-31 the render subrect as for DLSS SR (DlssSubrectFor), on the shared images RR reads in place
		const DlssSubrect sub = DlssSubrectFor(f, false);
		char why[256];
		if (const char* problem = SubrectProblem(h, f, sub, false, nullptr, why, sizeof(why))) {
			Status("NGX: Ray Reconstruction evaluate not called: %s; DLSS SR goes on", problem);
			return false;
		}
		const VkImageLayout read = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		for (uint32_t i = 0; i < h.imageCount; i++) {
			if (i == NVR_DLSS_IMAGE_OUTPUT) continue;
			Barrier(cmd, h.images[i].image, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, read,
				VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
		}
		Barrier(cmd, h.images[NVR_DLSS_IMAGE_OUTPUT].image, 0, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

		const VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		NVSDK_NGX_Resource_VK res[NVR_DLSS_IMAGE_COUNT] = {};
		for (uint32_t i = 0; i < h.imageCount; i++) {
			const SharedImage& s = h.images[i];
			res[i] = NVSDK_NGX_Create_ImageView_Resource_VK(s.view, s.image, range, s.format, s.width, s.height, i == NVR_DLSS_IMAGE_OUTPUT);
		}
		// NGX takes the matrices by pointer: copies that outlive this call (the frame ring's slot may be rewritten by NVR meanwhile).
		memcpy(h.rrWorldToView, f.worldToView, sizeof(h.rrWorldToView));
		memcpy(h.rrViewToClip, f.viewToClip, sizeof(h.rrViewToClip));

		NVSDK_NGX_Parameter* p = h.rrParams;
		NVSDK_NGX_Parameter_SetVoidPointer(p, NVSDK_NGX_Parameter_Color, &res[NVR_DLSS_IMAGE_COLOR]);
		NVSDK_NGX_Parameter_SetVoidPointer(p, NVSDK_NGX_Parameter_Output, &res[NVR_DLSS_IMAGE_OUTPUT]);
		NVSDK_NGX_Parameter_SetVoidPointer(p, NVSDK_NGX_Parameter_Depth, &res[NVR_DLSS_IMAGE_DEPTH]);
		NVSDK_NGX_Parameter_SetVoidPointer(p, NVSDK_NGX_Parameter_MotionVectors, &res[NVR_DLSS_IMAGE_MOTION]);
		NVSDK_NGX_Parameter_SetVoidPointer(p, kRrKeyDiffuseAlbedo, &res[NVR_DLSS_IMAGE_DIFFUSE_ALBEDO]);
		NVSDK_NGX_Parameter_SetVoidPointer(p, kRrKeySpecularAlbedo, &res[NVR_DLSS_IMAGE_SPECULAR_ALBEDO]);
		// (also under the GBuffer names, as the normals and roughness are: NGX ignores keys it does not read)
		NVSDK_NGX_Parameter_SetVoidPointer(p, NVSDK_NGX_Parameter_GBuffer_DiffuseAlbedo, &res[NVR_DLSS_IMAGE_DIFFUSE_ALBEDO]);
		NVSDK_NGX_Parameter_SetVoidPointer(p, NVSDK_NGX_Parameter_GBuffer_SpecularAlbedo, &res[NVR_DLSS_IMAGE_SPECULAR_ALBEDO]);
		NVSDK_NGX_Parameter_SetVoidPointer(p, NVSDK_NGX_Parameter_GBuffer_Normals, &res[NVR_DLSS_IMAGE_NORMALS]);
		NVSDK_NGX_Parameter_SetVoidPointer(p, kRrKeyInputNormals, &res[NVR_DLSS_IMAGE_NORMALS]);
		NVSDK_NGX_Parameter_SetVoidPointer(p, NVSDK_NGX_Parameter_GBuffer_Roughness, &res[NVR_DLSS_IMAGE_ROUGHNESS]);
		NVSDK_NGX_Parameter_SetVoidPointer(p, kRrKeyInputRoughness, &res[NVR_DLSS_IMAGE_ROUGHNESS]);
		NVSDK_NGX_Parameter_SetVoidPointer(p, kRrKeyWorldToView, h.rrWorldToView);
		NVSDK_NGX_Parameter_SetVoidPointer(p, kRrKeyViewToClip, h.rrViewToClip);
		// The scalars, as NGX_VULKAN_EVALUATE_DLSS_EXT sets them for DLSS SR (its zero-or-one defaults included).
		NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_Parameter_Jitter_Offset_X, f.jitterX);
		NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_Parameter_Jitter_Offset_Y, f.jitterY);
		NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_Parameter_Sharpness, 0.0f);
		NVSDK_NGX_Parameter_SetI(p, NVSDK_NGX_Parameter_Reset, f.reset ? 1 : 0);
		NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_Parameter_MV_Scale_X, f.mvScaleX == 0.0f ? 1.0f : f.mvScaleX);
		NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_Parameter_MV_Scale_Y, f.mvScaleY == 0.0f ? 1.0f : f.mvScaleY);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_Parameter_TonemapperType, 0);
		NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, f.frameTimeMs);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, sub.x);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, sub.y);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, sub.x);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, sub.y);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, sub.x);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, sub.y);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_Parameter_DLSS_Input_Translucency_SubrectBase_X, 0);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_Parameter_DLSS_Input_Translucency_SubrectBase_Y, 0);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_X, 0);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_Y, 0);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, 0);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, 0);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, sub.width);
		NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, sub.height);
		NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_Parameter_DLSS_Pre_Exposure, f.preExposure > 0.0f ? f.preExposure : 1.0f);
		NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.0f);
		NVSDK_NGX_Parameter_SetI(p, NVSDK_NGX_Parameter_DLSS_Indicator_Invert_X_Axis, 0);
		NVSDK_NGX_Parameter_SetI(p, NVSDK_NGX_Parameter_DLSS_Indicator_Invert_Y_Axis, 0);
		NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_EvaluateFeature_C(cmd, h.dlssd, p, nullptr);
		if (g_shared) g_shared->rrResult = (int32_t)r;

		for (uint32_t i = 0; i < h.imageCount; i++) {
			if (i == NVR_DLSS_IMAGE_OUTPUT) continue;
			Barrier(cmd, h.images[i].image, VK_ACCESS_SHADER_READ_BIT, 0, read, VK_IMAGE_LAYOUT_GENERAL,
				VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
		}
		Barrier(cmd, h.images[NVR_DLSS_IMAGE_OUTPUT].image, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
			VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
		if (NVSDK_NGX_FAILED(r)) {
			Status("NGX: Ray Reconstruction evaluate failed: %s; DLSS SR goes on", NgxResultString(r).c_str());
			return false;
		}
		return true;
	}
#endif

	uint32_t Average(const uint64_t* values, uint32_t count) {
		const uint32_t n = std::min(count, 64u);
		if (!n) return 0;
		uint64_t sum = 0;
		for (uint32_t i = 0; i < n; i++) sum += values[i];
		return (uint32_t)(sum / n);
	}

	bool WaitSlot(Helper& h, uint32_t slot, HANDLE parent) {
		if (!h.cmdFrame[slot]) return true;
		VkSemaphoreWaitInfo wait = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
		wait.semaphoreCount = 1;
		wait.pSemaphores = &h.outputReady;
		wait.pValues = &h.cmdFrame[slot];
		for (;;) {
			VkResult r = vkWaitSemaphores(h.vk.device, &wait, 250ull * 1000 * 1000);
			if (r == VK_SUCCESS) break;
			if (r != VK_TIMEOUT) { Status("Vulkan: waiting for an earlier frame failed: %s", VkResultName(r)); return false; }
			if (WaitForSingleObject(parent, 0) == WAIT_OBJECT_0 || g_shared->quit) return false;
		}
		if (h.queries) {
			uint64_t stamps[2] = {};
			if (vkGetQueryPoolResults(h.vk.device, h.queries, slot * 2, 2, sizeof(stamps), stamps, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
				const uint64_t mask = h.vk.timestampBits >= 64 ? ~0ull : ((1ull << h.vk.timestampBits) - 1);
				const double us = (double)((stamps[1] - stamps[0]) & mask) * h.vk.properties.limits.timestampPeriod / 1000.0;
				h.gpuUs[h.gpuCount++ % 64] = (uint64_t)us;
				g_shared->gpuTimeUsLast = (uint32_t)us;
				g_shared->gpuTimeUsAvg = Average(h.gpuUs, h.gpuCount);
			}
		}
		return true;
	}

	// One frame: GPU wait for inputsReady >= N, the work, signal outputReady = N. Always submits (even when the work could not be
	// recorded) so the game's GPU wait for N is released.
	bool ProcessFrame(Helper& h, uint64_t frame, HANDLE parent) {
		const NvrDlssFrame f = g_shared->frames[frame % NVR_DLSS_FRAME_RING];
		NvrDlssFrame use = f;
		if (f.frame != frame) {
			// Overwritten before it was read (NVR ran far ahead): stretch with the latest sizes; the picture is still whole.
			g_shared->paramMismatches++;
			use.renderWidth = f.renderWidth ? f.renderWidth : h.images[NVR_DLSS_IMAGE_COLOR].width;
			use.renderHeight = f.renderHeight ? f.renderHeight : h.images[NVR_DLSS_IMAGE_COLOR].height;
			use.reset = 1;
		}
		// UNOFFICIAL 2026-10-30 TEST ONLY (--fail-test, [Shaders.TAA.DLSS] HelperFailTest 2 / 3): from frame 120 on, hang: this frame is
		// never submitted (NVR's watchdog ends the helper after 3 s, the game's GPU wait is released); report: a failure is reported as
		// a failed DLSS evaluation reports it, and this and the frames in flight are stretched (NVR goes to FSR 2).
		if ((g_failTest == 1 || g_failTest == 2) && frame >= 120) {	// (3, zero-subrect: DlssSubrectFor)
			if (g_failTest == 2) {
				Log("--fail-test hang: frame %llu is never submitted", (unsigned long long)frame);
				while (WaitForSingleObject(parent, 250) != WAIT_OBJECT_0 && !g_shared->quit) {}
				return false;
			}
			if (!h.failTestDone) {
				h.failTestDone = true;
				Status("--fail-test report: a failure reported at frame %llu (test), as a failed DLSS evaluation reports one", (unsigned long long)frame);
				SetState(NVR_DLSS_STATE_FAILED);
#ifndef NVR_DLSS_NO_SDK
				h.dlssFailed = true;
#endif
			}
		}
		const uint32_t slot = (uint32_t)(frame % kCmdRing);
		if (!WaitSlot(h, slot, parent)) return false;
		VkCommandBuffer cmd = h.cmd[slot];
		vkResetCommandBuffer(cmd, 0);
		VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		VK_CHECK(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer");
		if (h.queries) {
			vkCmdResetQueryPool(cmd, h.queries, slot * 2, 2);
			vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, h.queries, slot * 2);
		}
		bool dlssRan = false;
		// UNOFFICIAL 2026-10-30 --inputs exact: the render rectangle into images of its own size, read by DLSS SR and the pass-through
		// (RR frames come at render size = input size only: nothing to copy then)
		const bool exact = RecordExactInputs(h, cmd, use.renderWidth, use.renderHeight);
		// UNOFFICIAL 2026-10-30 DLSS Ray Reconstruction: an RR frame (only possible with the eight images) goes to DLSS-D while RR is
		// available, else to DLSS SR on the same colour (the picture stays whole; NVR stops sending RR frames once it sees
		// rrAvailable 0). In the pass-through test it is the stretch plus the guide thumbnails. Either way the frame is submitted.
		const bool rrFrame = (use.flags & NVR_DLSS_FRAME_RR) != 0 && h.rrWanted;
		bool rrRan = false;
#ifndef NVR_DLSS_NO_SDK
		if (h.mode == NVR_DLSS_MODE_DLSS && h.dlssAvailable && !h.dlssFailed && f.frame == frame) {
			if (rrFrame && g_shared->rrAvailable && !h.rrFailed) {
				rrRan = RecordDlssd(h, cmd, use);
				if (!rrRan) {
					h.rrFailed = true;
					MemoryBarrier();
					g_shared->rrAvailable = 0;	// never FAILED: that would end DLSS SR too
					if (g_statusEvent) SetEvent(g_statusEvent);
				}
			}
			if (!rrRan) {
				dlssRan = RecordDlss(h, cmd, use, exact);
				if (!dlssRan) {
					h.dlssFailed = true;
					SetState(NVR_DLSS_STATE_FAILED);	// NVR goes back to FSR 2; this and the frames in flight are stretched
				}
			}
		}
#endif
		// UNOFFICIAL 2026-10-31 the pass-through test (no NGX) checks the render subrect a real evaluate of this frame would be given
		// (DlssSubrectFor, SubrectProblem; NGX's dynamic range is not known here). A problem is reported once, as a failed DLSS evaluation
		// reports one (NVR goes to FSR 2): on a GTX card the test fails where DLSS would. This frame and the next are still stretched.
		if (h.mode == NVR_DLSS_MODE_PASSTHROUGH && !h.subrectFailed) {
			char why[256];
			const char* problem = SubrectProblem(h, use, DlssSubrectFor(use, exact), exact, nullptr, why, sizeof(why));
			if (!problem && rrFrame) problem = SubrectProblem(h, use, DlssSubrectFor(use, false), false, nullptr, why, sizeof(why));
			if (problem) {
				h.subrectFailed = true;
				Status("pass-through test: DLSS would not evaluate frame %llu: %s", (unsigned long long)frame, problem);
				SetState(NVR_DLSS_STATE_FAILED);
			}
		}
		if (!dlssRan && !rrRan) {
			if (rrFrame && h.mode == NVR_DLSS_MODE_PASSTHROUGH) {
				RecordPassThroughRR(h, cmd, use, frame, exact);
				rrRan = true;
			}
			else RecordPassThrough(h, cmd, use, frame, exact);
		}
		if (h.queries) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, h.queries, slot * 2 + 1);
		VK_CHECK(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

		LARGE_INTEGER now, frequency;
		QueryPerformanceFrequency(&frequency);
		LARGE_INTEGER readyFrom = {};	// UNOFFICIAL 2026-10-30: from when this frame could go (the response below)
		readyFrom.QuadPart = f.frame == frame ? f.writtenQpc : 0;

		// UNOFFICIAL 2026-10-30 (DLAA stall report) --wait cpu: the game's "inputs ready" = N reached on the CPU first; the submission then
		// carries no wait, so the GPU runs it as soon as it is submitted (no cross-process GPU wait). Its end still signals "output ready".
		bool gpuWait = !g_cpuWait;
		if (g_cpuWait) {
			VkSemaphoreWaitInfo wait = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
			wait.semaphoreCount = 1;
			wait.pSemaphores = &h.inputsReady;
			wait.pValues = &frame;
			LARGE_INTEGER waitStart, waitEnd;
			QueryPerformanceCounter(&waitStart);
			for (;;) {
				const VkResult r = vkWaitSemaphores(h.vk.device, &wait, 250ull * 1000 * 1000);
				if (r == VK_SUCCESS) break;
				if (r != VK_TIMEOUT) {
					// Waiting on the CPU failed: this frame waits on the GPU as before, and so do the rest (logged once).
					Log("--wait cpu: waiting for the game's inputs failed (%s); the helper's submissions wait on the GPU from now on", VkResultName(r));
					g_cpuWait = false;
					gpuWait = true;
					break;
				}
				if (WaitForSingleObject(parent, 0) == WAIT_OBJECT_0 || g_shared->quit) return false;
			}
			QueryPerformanceCounter(&waitEnd);
			if (readyFrom.QuadPart && waitEnd.QuadPart > readyFrom.QuadPart) readyFrom = waitEnd;	// the frame could go once the game's inputs were there
			const double waitedMs = (double)(waitEnd.QuadPart - waitStart.QuadPart) * 1000.0 / (double)frequency.QuadPart;
			h.inputsWaitMsSum += waitedMs;
			h.inputsWaitMsMax = std::max(h.inputsWaitMsMax, waitedMs);
			h.inputsWaitCount++;
		}

		const uint64_t waitValue = frame, signalValue = frame;
		VkTimelineSemaphoreSubmitInfo timeline = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
		timeline.waitSemaphoreValueCount = gpuWait ? 1 : 0;
		timeline.pWaitSemaphoreValues = gpuWait ? &waitValue : nullptr;
		timeline.signalSemaphoreValueCount = 1;
		timeline.pSignalSemaphoreValues = &signalValue;
		const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
		VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO, &timeline };
		submit.waitSemaphoreCount = gpuWait ? 1 : 0;
		submit.pWaitSemaphores = gpuWait ? &h.inputsReady : nullptr;
		submit.pWaitDstStageMask = gpuWait ? &waitStage : nullptr;
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &cmd;
		submit.signalSemaphoreCount = 1;
		submit.pSignalSemaphores = &h.outputReady;
		VK_CHECK(vkQueueSubmit(h.vk.queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit");
		h.cmdFrame[slot] = frame;

		// The response: from when this frame could go (NVR's write; with --wait cpu, the game's inputs ready if that came later) to its
		// submission (UNOFFICIAL 2026-10-30: under --wait cpu the frames are taken in turn, so NVR's write alone would count the wait for
		// the frame before it).
		QueryPerformanceCounter(&now);
		if (readyFrom.QuadPart) {
			const uint64_t us = (uint64_t)((now.QuadPart - readyFrom.QuadPart) * 1000000 / frequency.QuadPart);
			h.responseUs[h.responseCount++ % 64] = us;
			g_shared->responseUsAvg = Average(h.responseUs, h.responseCount);
			if (us > g_shared->responseUsMax) g_shared->responseUsMax = (uint32_t)us;
		}
		if (dlssRan) g_shared->dlssEvaluated++;
		if (rrRan) g_shared->rrEvaluated++;
		// UNOFFICIAL 2026-11-01 protocol 3: a DLSS SR frame given the bias mask, or a pass-through frame that had it (imported and copied:
		// the pass-through draws nothing from it)
		const bool maskUsed = h.hasBiasMask && (use.flags & NVR_DLSS_FRAME_BIAS_MASK) && !rrRan && (dlssRan || h.mode == NVR_DLSS_MODE_PASSTHROUGH);
		if (maskUsed) g_shared->biasMaskFrames++;
		if (maskUsed && !h.loggedFirstMask) {
			h.loggedFirstMask = true;
			Log("first frame with the bias current colour mask: %llu (%s)", (unsigned long long)frame, dlssRan ? "given to DLSS SR" : "pass-through: imported, not drawn");
		}
		g_shared->framesSubmitted++;
		MemoryBarrier();
		g_shared->submittedFrame = (int64_t)frame;
		const char* const work = rrRan ? (h.mode == NVR_DLSS_MODE_DLSS ? "DLSS Ray Reconstruction" : "pass-through RR") : dlssRan ? "DLSS" : "pass-through";
		if (!h.loggedFirst) {
			h.loggedFirst = true;
			Log("first frame %llu submitted (%s, render %ux%u -> %ux%u, %s, %s)", (unsigned long long)frame, work,
				use.renderWidth, use.renderHeight, h.images[NVR_DLSS_IMAGE_OUTPUT].width, h.images[NVR_DLSS_IMAGE_OUTPUT].height, use.hdr ? "HDR" : "LDR",
				exact ? "inputs copied at the render size" : "inputs read in place");
		}
		if (rrRan && !h.loggedFirstRr) {
			h.loggedFirstRr = true;
			Log("first RR frame %llu submitted (%s, render %ux%u -> %ux%u, %s normals, %s depth)", (unsigned long long)frame, work, use.renderWidth,
				use.renderHeight, h.images[NVR_DLSS_IMAGE_OUTPUT].width, h.images[NVR_DLSS_IMAGE_OUTPUT].height,
				(use.rrOptions & NVR_DLSS_RR_NORMALS_VIEW) ? "view-space" : "world-space", (use.rrOptions & NVR_DLSS_RR_DEPTH_LINEAR) ? "linear" : "hardware");
		}
		if ((frame % 600) == 0 && h.inputsWaitCount) {
			Log("frame %llu: --wait cpu waited for the game's inputs %.3f ms avg (max %.3f) over the last %u frames", (unsigned long long)frame,
				h.inputsWaitMsSum / h.inputsWaitCount, h.inputsWaitMsMax, h.inputsWaitCount);
			h.inputsWaitMsSum = h.inputsWaitMsMax = 0.0;
			h.inputsWaitCount = 0;
		}
		if ((frame % 600) == 0) {
			char rr[64] = "";
			if (h.rrWanted) snprintf(rr, sizeof(rr), ", %u RR (%s)", g_shared->rrEvaluated, g_shared->rrAvailable ? "available" : "off");
			Log("frame %llu: %llu submitted, helper GPU %.3f ms avg, response %.3f ms avg (max %.3f), %u overwritten, %u DLSS%s",
				(unsigned long long)frame, (unsigned long long)g_shared->framesSubmitted, g_shared->gpuTimeUsAvg / 1000.0,
				g_shared->responseUsAvg / 1000.0, g_shared->responseUsMax / 1000.0, g_shared->paramMismatches, g_shared->dlssEvaluated, rr);
		}
		return true;
	}

	int RunHelper(const wchar_t* base) {
		OpenLog(L"NvrDlssHelper.log");
		Log("NvrDlssHelper (protocol %u, %s) for New Vegas Reloaded, shared block %ls", NVR_DLSS_PROTOCOL_VERSION,
#ifndef NVR_DLSS_NO_SDK
			"with NVIDIA DLSS (NGX)",
#else
			"built without NVIDIA's SDK: pass-through only",
#endif
			base);
		HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, base);
		if (!mapping) { Log("the shared block %ls does not exist (error %lu)", base, GetLastError()); return 2; }
		NvrDlssShared* shared = (NvrDlssShared*)MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(NvrDlssShared));
		if (!shared) { Log("could not map the shared block (error %lu)", GetLastError()); return 2; }
		if (shared->magic != NVR_DLSS_MAGIC || shared->version != NVR_DLSS_PROTOCOL_VERSION || shared->size != sizeof(NvrDlssShared)) {
			Log("the shared block is of another version (magic 0x%08X, version %u, size %u; this helper: version %u, size %u)", shared->magic,
				shared->version, shared->size, NVR_DLSS_PROTOCOL_VERSION, (unsigned)sizeof(NvrDlssShared));
			return 2;
		}
		// UNOFFICIAL 2026-10-30 (DLAA stall report): the WDDM GPU scheduling priority class of this process (gdi32's
		// D3DKMTSetProcessSchedulingPriorityClass; D3DKMT_SCHEDULINGPRIORITYCLASS_HIGH = 4). The CPU priority class does not reach the GPU.
		if (g_highPriority) {
			typedef LONG(APIENTRY* SetGpuPriorityClass)(HANDLE, int);
			const SetGpuPriorityClass setClass = (SetGpuPriorityClass)GetProcAddress(GetModuleHandleW(L"gdi32.dll") ? GetModuleHandleW(L"gdi32.dll") :
				LoadLibraryW(L"gdi32.dll"), "D3DKMTSetProcessSchedulingPriorityClass");
			const LONG status = setClass ? setClass(GetCurrentProcess(), 4) : -1;
			Log("GPU priority: the process's GPU scheduling priority class HIGH: %s (NTSTATUS 0x%08lX)", status == 0 ? "set" : "not set",
				(unsigned long)status);
		}
		Log("start options: %s, GPU priority %s, %s%s", g_cpuWait ? "--wait cpu (the helper waits for the game's inputs on the CPU, then submits)" :
			"--wait gpu (the submission waits on the GPU)", g_highPriority ? "high" : "normal",
			g_exactInputs ? "--inputs exact (a render rectangle below the inputs' size is copied into images of its own size)" : "--inputs subrect (DLSS reads the render rectangle in place)",
			g_failTest == 1 ? ", --fail-test report (TEST: a failure at frame 120)" : g_failTest == 2 ? ", --fail-test hang (TEST: no answer from frame 120)" :
			g_failTest == 3 ? ", --fail-test zero-subrect (TEST: a 0x0 render subrect with --inputs exact)" : "");
		if (g_strictSubrect) Log("start options: --subrect-range strict (a render subrect outside NGX's dynamic range is refused, not only logged)");
		const std::wstring frameName = std::wstring(base) + NVR_DLSS_EVENT_FRAME_SUFFIX, statusName = std::wstring(base) + NVR_DLSS_EVENT_STATUS_SUFFIX;
		HANDLE frameEvent = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, frameName.c_str());
		g_statusEvent = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, statusName.c_str());
		g_shared = shared;
		if (!frameEvent || !g_statusEvent) { Status("the helper's events do not exist"); SetState(NVR_DLSS_STATE_FAILED); return 2; }
		HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, shared->parentPid);
		if (!parent) { Status("the game's process %u cannot be opened", shared->parentPid); SetState(NVR_DLSS_STATE_FAILED); return 2; }

		// NVR fills in the handles after it started this process.
		for (int i = 0; i < 100 && !shared->setupDone; i++) {
			HANDLE waits[2] = { frameEvent, parent };
			if (WaitForMultipleObjects(2, waits, FALSE, 100) == WAIT_OBJECT_0 + 1) return 0;
		}
		MemoryBarrier();
		if (!shared->setupDone) { Status("NVR did not finish the setup in 10 seconds"); SetState(NVR_DLSS_STATE_FAILED); return 2; }

		Helper h;
		h.mode = shared->mode;
		Log("mode: %s, display %ux%u, inputs %ux%u", h.mode == NVR_DLSS_MODE_DLSS ? "DLSS" : "pass-through test", shared->displayWidth,
			shared->displayHeight, shared->inputWidth, shared->inputHeight);
		auto fail = [&](const char* why) { if (why) Status("%s", why); SetState(NVR_DLSS_STATE_FAILED); return 3; };
		if (shared->vendorId != kNvidiaVendor) return fail("not an NVIDIA GPU: the helper runs on NVIDIA GPUs only (NVIDIA's DLSS SDK licence)");
		// UNOFFICIAL 2026-10-30 protocol 2: four images, or eight (the RR guides) only together with NVR_DLSS_FEATURE_RR. The RR bit
		// without the guides only means no RR (DLSS SR as before).
		shared->rrAvailable = 0;
		h.imageCount = shared->imageCount;
		if (h.imageCount != NVR_DLSS_IMAGE_COUNT_SR && h.imageCount != NVR_DLSS_IMAGE_COUNT) {
			char why[160];
			snprintf(why, sizeof(why), "the shared block names %u images (%u or %u expected)", h.imageCount, (unsigned)NVR_DLSS_IMAGE_COUNT_SR,
				(unsigned)NVR_DLSS_IMAGE_COUNT);
			return fail(why);
		}
		const bool rrFeature = (shared->features & NVR_DLSS_FEATURE_RR) != 0;
		if (h.imageCount == NVR_DLSS_IMAGE_COUNT && !rrFeature) return fail("the shared block has the RR guide images but not the RR feature bit");
		h.rrWanted = rrFeature && h.imageCount == NVR_DLSS_IMAGE_COUNT;
		if (rrFeature && !h.rrWanted) Log("NVR wants Ray Reconstruction but made no RR guide images: no Ray Reconstruction (DLSS SR as before)");
		if (h.rrWanted) Log("Ray Reconstruction wanted: %u images (the colour, depth, motion and output, plus %u RR guides)", h.imageCount, kGuideCount);
		// (RR's extensions only for real DLSS-D: the pass-through test's RR frames need no NGX.)
		if (!CreateVulkanDevice(h.vk, shared->deviceLuid, shared->deviceLuidValid != 0, shared->deviceUuid, true,
			h.rrWanted && h.mode == NVR_DLSS_MODE_DLSS)) return fail(nullptr);
		if (memcmp(h.vk.ids.driverUUID, shared->driverUuid, VK_UUID_SIZE))
			return fail("the helper's Vulkan driver is not the game's (driver UUIDs differ): images cannot be shared");
		for (uint32_t i = 0; i < h.imageCount; i++)
			if (!ImportImage(h.vk, shared->images[i], kImageNames[i], h.images[i])) return fail(nullptr);
		// UNOFFICIAL 2026-11-01 protocol 3: the bias current colour mask ([Shaders.TAA.DLSS] WeaponMask), when NVR made it
		if (shared->features & NVR_DLSS_FEATURE_BIAS_MASK) {
			if (!ImportImage(h.vk, shared->biasMask, "bias mask", h.biasMask)) return fail(nullptr);
			h.hasBiasMask = true;
		}
		else Log("no bias current colour mask from NVR (WeaponMask 0, or its pass could not be made): DLSS SR gets none");
		if (!ImportTimeline(h.vk, shared->semInputsReady, "inputs-ready", h.inputsReady) ||
			!ImportTimeline(h.vk, shared->semOutputReady, "output-ready", h.outputReady)) return fail(nullptr);

		VkCommandPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
		poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		poolInfo.queueFamilyIndex = h.vk.queueFamily;
		if (vkCreateCommandPool(h.vk.device, &poolInfo, nullptr, &h.pool) != VK_SUCCESS) return fail("Vulkan: vkCreateCommandPool failed");
		VkCommandBufferAllocateInfo cmdInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
		cmdInfo.commandPool = h.pool;
		cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cmdInfo.commandBufferCount = kCmdRing;
		if (vkAllocateCommandBuffers(h.vk.device, &cmdInfo, h.cmd) != VK_SUCCESS) return fail("Vulkan: vkAllocateCommandBuffers failed");
		if (h.vk.timestampBits) {
			VkQueryPoolCreateInfo queryInfo = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
			queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
			queryInfo.queryCount = kCmdRing * 2;
			if (vkCreateQueryPool(h.vk.device, &queryInfo, nullptr, &h.queries) != VK_SUCCESS) h.queries = VK_NULL_HANDLE;
		}
		if (!CreateMarker(h)) return fail(nullptr);

#ifndef NVR_DLSS_NO_SDK
		shared->ngxAvailable = InitNgx(h) ? 1u : 0u;
		if (h.mode == NVR_DLSS_MODE_DLSS && !h.dlssAvailable) return fail(nullptr);	// NVR logs the reason and runs FSR 2
#else
		shared->ngxAvailable = 0;
		if (h.mode == NVR_DLSS_MODE_DLSS) return fail("NGX: this helper was built without NVIDIA's DLSS SDK (pass-through test only)");
		Status("NGX: not built in (pass-through test only)");
#endif
		// UNOFFICIAL 2026-10-30 the pass-through test takes RR frames on any NVIDIA GPU (no NGX: the stretch plus the guide thumbnails).
		if (h.mode == NVR_DLSS_MODE_PASSTHROUGH && h.rrWanted) shared->rrAvailable = 1;
		char rrNote[64] = "";
		if (h.rrWanted) snprintf(rrNote, sizeof(rrNote), ", Ray Reconstruction %s", shared->rrAvailable ? "available" : "not available");
		Status("ready: %s on %s (%ux%u -> %ux%u)%s", h.mode == NVR_DLSS_MODE_DLSS ? "DLSS" : "pass-through test", h.vk.properties.deviceName,
			shared->inputWidth, shared->inputHeight, shared->displayWidth, shared->displayHeight, rrNote);
		SetState(NVR_DLSS_STATE_READY);

		uint64_t next = 1;
		bool parentGone = false;
		for (;;) {
			HANDLE waits[2] = { frameEvent, parent };
			const DWORD w = WaitForMultipleObjects(2, waits, FALSE, 250);
			if (w == WAIT_OBJECT_0 + 1) { Log("the game exited"); parentGone = true; break; }
			if (shared->quit) {
				// Frames already submitted finish first (their GPU waits are released by the game's own signals), so nothing the
				// game's queue waits for is left behind; NVR signals the semaphore itself if this takes too long.
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
			const int64_t latest = InterlockedCompareExchange64((volatile LONG64*)&shared->latestFrame, 0, 0);
			MemoryBarrier();
			while ((int64_t)next <= latest) {
				if (!ProcessFrame(h, next, parent)) { Log("stopping at frame %llu", (unsigned long long)next); goto done; }
				next++;
			}
		}
	done:
		if (shared->state != NVR_DLSS_STATE_FAILED) SetState(NVR_DLSS_STATE_EXITED);
		Log("exit after %llu frames%s", (unsigned long long)shared->framesSubmitted, parentGone ? " (the game is gone)" : "");
		// No vkDeviceWaitIdle / NGX shutdown here: a submission may still wait for a signal the game will never send (it exited);
		// the process exit releases everything.
		// UNOFFICIAL 2026-10-30: the DLSS SR and RR features likewise (ReleaseFeature may touch the GPU); only RR's own parameter map
		// (CPU memory, no GPU work) is destroyed by hand.
#ifndef NVR_DLSS_NO_SDK
		if (h.rrParams) { NVSDK_NGX_VULKAN_DestroyParameters(h.rrParams); h.rrParams = nullptr; }
#endif
		if (g_log) fclose(g_log);
		ExitProcess(0);
	}

	// ---------------------------------------------------------------------------------------------------------------------
	// --selftest: this process does what NVR + the fork do (export, signal N, wait N, read back), a child copy is the helper in
	// pass-through mode. Checks every 50th frame's pixels: the stretched input pattern and the marker box at frame % 8.

	struct Exported {
		VkImage image = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		NvrDlssImageDesc desc = {};
	};

	bool ExportImage(const VulkanDevice& vk, VkFormat format, uint32_t w, uint32_t h, Exported& out) {
		out.desc.vkFormat = (uint32_t)format;
		out.desc.width = w;
		out.desc.height = h;
		out.desc.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
			VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		out.desc.flags = 0;
		out.desc.tiling = VK_IMAGE_TILING_OPTIMAL;
		out.desc.layout = VK_IMAGE_LAYOUT_GENERAL;
		out.desc.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
		out.desc.dedicated = 1;
		VkExternalMemoryImageCreateInfo external = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
		external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
		VkImageCreateInfo info = ImageInfo(out.desc, &external);
		VK_CHECK(vkCreateImage(vk.device, &info, nullptr, &out.image), "vkCreateImage (export)");
		VkMemoryRequirements req = {};
		vkGetImageMemoryRequirements(vk.device, out.image, &req);
		int32_t type = FindMemoryType(vk, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		if (type < 0) { Status("selftest: no device-local memory type"); return false; }
		VkMemoryDedicatedAllocateInfo dedicated = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
		dedicated.image = out.image;
		VkExportMemoryAllocateInfo exportInfo = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &dedicated };
		exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
		VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &exportInfo };
		alloc.allocationSize = req.size;
		alloc.memoryTypeIndex = (uint32_t)type;
		VK_CHECK(vkAllocateMemory(vk.device, &alloc, nullptr, &out.memory), "vkAllocateMemory (export)");
		VK_CHECK(vkBindImageMemory(vk.device, out.image, out.memory, 0), "vkBindImageMemory (export)");
		out.desc.allocationSize = req.size;
		out.desc.memoryTypeIndex = (uint32_t)type;
		return true;
	}

	bool ExportHandle(const VulkanDevice& vk, VkDeviceMemory memory, HANDLE child, uint64_t& out) {
		VkMemoryGetWin32HandleInfoKHR info = { VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR };
		info.memory = memory;
		info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
		HANDLE handle = nullptr;
		VK_CHECK(vk.getMemoryWin32Handle(vk.device, &info, &handle), "vkGetMemoryWin32HandleKHR");
		HANDLE remote = nullptr;
		if (!DuplicateHandle(GetCurrentProcess(), handle, child, &remote, 0, FALSE, DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE)) {
			Status("selftest: DuplicateHandle failed (%lu)", GetLastError());
			return false;
		}
		out = (uint64_t)(uintptr_t)remote;
		return true;
	}

	bool ExportSemaphore(const VulkanDevice& vk, VkSemaphore& sem, HANDLE child, uint64_t& out) {
		VkExportSemaphoreCreateInfo exportInfo = { VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO };
		exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
		VkSemaphoreTypeCreateInfo type = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, &exportInfo };
		type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
		VkSemaphoreCreateInfo info = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type };
		VK_CHECK(vkCreateSemaphore(vk.device, &info, nullptr, &sem), "vkCreateSemaphore (export)");
		VkSemaphoreGetWin32HandleInfoKHR get = { VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR };
		get.semaphore = sem;
		get.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
		HANDLE handle = nullptr;
		VK_CHECK(vk.getSemaphoreWin32Handle(vk.device, &get, &handle), "vkGetSemaphoreWin32HandleKHR");
		HANDLE remote = nullptr;
		if (!DuplicateHandle(GetCurrentProcess(), handle, child, &remote, 0, FALSE, DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE)) {
			Status("selftest: DuplicateHandle failed (%lu)", GetLastError());
			return false;
		}
		out = (uint64_t)(uintptr_t)remote;
		return true;
	}

	// ---------------------------------------------------------------------------------------------------------------------
	// --probe: what this PC offers.

	int RunProbe() {
		g_console = true;
		OpenLog(L"NvrDlssHelper-probe.log");
		Log("NvrDlssHelper probe (%s)",
#ifndef NVR_DLSS_NO_SDK
			"with NVIDIA DLSS (NGX)"
#else
			"built without NVIDIA's SDK"
#endif
		);
		Helper h;
		// UNOFFICIAL 2026-10-30: with Ray Reconstruction's extension needs too (logged), so its line below sees the same device.
		if (!CreateVulkanDevice(h.vk, nullptr, false, nullptr, true, true)) { Log("probe: no usable NVIDIA Vulkan device"); return 1; }
		Log("probe: %s, Vulkan %u.%u, external memory + semaphores (Win32): yes, timeline semaphores: yes", h.vk.properties.deviceName,
			VK_VERSION_MAJOR(h.vk.properties.apiVersion), VK_VERSION_MINOR(h.vk.properties.apiVersion));
		{
			// Vulkan allows one NT-handle export per semaphore; what this driver does with a second one (the 2026-10-27 bug: DXVK
			// exported once and closed it, NVR's export got a dead handle).
			VkExportSemaphoreCreateInfo exportInfo = { VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO };
			exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
			VkSemaphoreTypeCreateInfo type = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, &exportInfo };
			type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
			VkSemaphoreCreateInfo info = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type };
			VkSemaphore sem = VK_NULL_HANDLE;
			if (vkCreateSemaphore(h.vk.device, &info, nullptr, &sem) == VK_SUCCESS) {
				VkSemaphoreGetWin32HandleInfoKHR get = { VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR };
				get.semaphore = sem;
				get.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
				HANDLE first = nullptr, second = nullptr, copy = nullptr;
				const VkResult r1 = h.vk.getSemaphoreWin32Handle(h.vk.device, &get, &first);
				CloseHandle(first);
				const VkResult r2 = h.vk.getSemaphoreWin32Handle(h.vk.device, &get, &second);
				const BOOL dup = DuplicateHandle(GetCurrentProcess(), second, GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS);
				const DWORD error = dup ? 0 : GetLastError();
				Log("probe: semaphore NT handle exported twice (first closed): %d / %d, handles %p / %p (%s), second %s (Win32 error %lu)",
					(int)r1, (int)r2, first, second, first == second ? "the SAME value" : "different", dup ? "usable" : "DEAD", error);
				if (dup) CloseHandle(copy);
				if (second != first) CloseHandle(second);
				vkDestroySemaphore(h.vk.device, sem, nullptr);
			}
			// The same for an image's dedicated memory.
			Exported image;
			if (ExportImage(h.vk, VK_FORMAT_R16G16B16A16_SFLOAT, 64, 64, image)) {
				VkMemoryGetWin32HandleInfoKHR get = { VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR };
				get.memory = image.memory;
				get.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
				HANDLE first = nullptr, second = nullptr, copy = nullptr;
				const VkResult r1 = h.vk.getMemoryWin32Handle(h.vk.device, &get, &first);
				CloseHandle(first);
				const VkResult r2 = h.vk.getMemoryWin32Handle(h.vk.device, &get, &second);
				const BOOL dup = DuplicateHandle(GetCurrentProcess(), second, GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS);
				const DWORD error = dup ? 0 : GetLastError();
				Log("probe: memory NT handle exported twice (first closed): %d / %d, handles %p / %p (%s), second %s (Win32 error %lu)",
					(int)r1, (int)r2, first, second, first == second ? "the SAME value" : "different", dup ? "usable" : "DEAD", error);
				if (dup) CloseHandle(copy);
				if (second != first) CloseHandle(second);
				vkDestroyImage(h.vk.device, image.image, nullptr);
				vkFreeMemory(h.vk.device, image.memory, nullptr);
			}
		}
		// UNOFFICIAL 2026-10-30 DLSS Ray Reconstruction: NVIDIA's runtime next to the helper, and NGX's own answer (only asked when
		// DLSS SR is available: RR needs it).
		const bool dlssdHere = FileNextToHelper(L"nvngx_dlssd.dll");
#ifndef NVR_DLSS_NO_SDK
		const bool dlss = InitNgx(h);
		Log("probe: DLSS %s", dlss ? "AVAILABLE" : "not available (see the line above)");
		std::string rrWhy = "DLSS is not available";
		const bool rr = dlss && RayReconstructionCapability(h, rrWhy);
		Log("probe: Ray Reconstruction (SuperSamplingDenoising.Available) %s%s%s; nvngx_dlssd.dll next to the helper: %s", rr ? "AVAILABLE" : "not available",
			rr ? "" : ": ", rr ? "" : rrWhy.c_str(), dlssdHere ? "yes" : "no");
		if (h.ngxInit) NVSDK_NGX_VULKAN_Shutdown1(h.vk.device);
#else
		Log("probe: Ray Reconstruction not built in; nvngx_dlssd.dll next to the helper: %s", dlssdHere ? "yes" : "no");
#endif
		{
			// UNOFFICIAL 2026-11-15 (DLSS-RR-MODEL.md): what [Shaders.TAA.DLSS] RRPreset 4 (auto) and 3 would send on this GPU and driver
			std::string why;
			const uint32_t autoModel = ChooseRrModel(NVR_DLSS_RR_PRESET_AUTO, h.vk.properties.deviceID, h.vk.properties.driverVersion, why);
			Log("probe: Ray Reconstruction model with RRPreset 4 (auto): %s (%s)", kRrModelNames[autoModel], why.c_str());
			const uint32_t newer = ChooseRrModel(NVR_DLSS_RR_PRESET_F, h.vk.properties.deviceID, h.vk.properties.driverVersion, why);
			Log("probe: Ray Reconstruction model with RRPreset 3 (newer): %s (%s)", kRrModelNames[newer], why.c_str());
		}
		DestroyVulkanDevice(h.vk);
		return 0;
	}

	// dlssMode: the child runs in DLSS mode instead. On a GPU without DLSS it must give up cleanly (FAILED + the NGX reason, the
	// fallback NVR relies on): that is a PASS there; on an RTX GPU it must get READY and run the frames.
	// UNOFFICIAL 2026-10-30 rrMode: protocol 2's RR exchange, as NVR's HelperTest with RayReconstruction makes it: eight images (the
	// guides cleared to known colours once), features RR, every frame an RR frame (identity matrices). The pass-through child must
	// report rrAvailable, count every frame in rrEvaluated and draw each guide's colour in its thumbnail. In DLSS mode (any of
	// them) the output is DLSS's, not the pass-through's: the frames must come back and be counted as DLSS SR or RR frames.
	int RunSelfTest(bool dlssMode, bool rrMode) {
		g_console = true;
		OpenLog(dlssMode ? (rrMode ? L"NvrDlssHelper-selftest-dlss-rr.log" : L"NvrDlssHelper-selftest-dlss.log")
			: (rrMode ? L"NvrDlssHelper-selftest-rr.log" : L"NvrDlssHelper-selftest.log"));
		Log("NvrDlssHelper self-test: this process plays NVR + the fork, a child copy of itself is the helper (%s%s)", dlssMode ? "DLSS mode" : "pass-through",
			rrMode ? ", Ray Reconstruction frames" : "");
		// UNOFFICIAL 2026-10-31 zerosubrect gives the 0x0 to DLSS SR frames with --inputs exact only (the path b0146f54 broke): with
		// subrect, or with dlss rr (every frame DLSS-D, which reads in place), there is nothing for the check to catch.
		if (g_failTest == 3 && (!g_exactInputs || (dlssMode && rrMode))) {
			Log("selftest zerosubrect FAILED: it needs --inputs exact and DLSS SR frames (not with subrect, not with dlss rr)");
			return 1;
		}
		const uint32_t displayW = 1280, displayH = 720, renderW = 960, renderH = 540;	// RenderScale 0.75
		const uint32_t imageCount = rrMode ? NVR_DLSS_IMAGE_COUNT : NVR_DLSS_IMAGE_COUNT_SR;
		// The guides' colours (diffuse albedo, specular albedo, normals, roughness: R16F keeps only the red one).
		const float guideColours[kGuideCount][4] = { { 0.8f, 0.4f, 0.2f, 1.0f }, { 0.1f, 0.5f, 0.9f, 1.0f }, { 0.25f, 0.75f, 0.5f, 1.0f }, { 0.6f, 0.0f, 0.0f, 1.0f } };
		VulkanDevice vk;
		if (!CreateVulkanDevice(vk, nullptr, false, nullptr, false)) { Log("selftest FAILED: no NVIDIA Vulkan device"); return 1; }

		// Shared block + events, then the child helper.
		wchar_t base[128];
		swprintf_s(base, L"Local\\NvrDlssSelfTest-%lu-%llu", GetCurrentProcessId(), (unsigned long long)GetTickCount64());
		HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(NvrDlssShared), base);
		NvrDlssShared* shared = mapping ? (NvrDlssShared*)MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(NvrDlssShared)) : nullptr;
		if (!shared) { Log("selftest FAILED: no shared block"); return 1; }
		memset(shared, 0, sizeof(*shared));
		shared->magic = NVR_DLSS_MAGIC;
		shared->version = NVR_DLSS_PROTOCOL_VERSION;
		shared->size = sizeof(NvrDlssShared);
		shared->mode = dlssMode ? NVR_DLSS_MODE_DLSS : NVR_DLSS_MODE_PASSTHROUGH;
		shared->parentPid = GetCurrentProcessId();
		shared->vendorId = vk.properties.vendorID;
		memcpy(shared->deviceLuid, vk.ids.deviceLUID, 8);
		shared->deviceLuidValid = vk.ids.deviceLUIDValid;
		memcpy(shared->deviceUuid, vk.ids.deviceUUID, 16);
		memcpy(shared->driverUuid, vk.ids.driverUUID, 16);
		shared->imageCount = imageCount;	// UNOFFICIAL 2026-10-30 protocol 2
		shared->features = (rrMode ? NVR_DLSS_FEATURE_RR : 0u) | NVR_DLSS_FEATURE_BIAS_MASK;	// UNOFFICIAL 2026-11-01 protocol 3: always the mask
		HANDLE frameEvent = CreateEventW(nullptr, FALSE, FALSE, (std::wstring(base) + NVR_DLSS_EVENT_FRAME_SUFFIX).c_str());
		HANDLE statusEvent = CreateEventW(nullptr, FALSE, FALSE, (std::wstring(base) + NVR_DLSS_EVENT_STATUS_SUFFIX).c_str());
		wchar_t exe[MAX_PATH] = {};
		GetModuleFileNameW(nullptr, exe, MAX_PATH);
		std::wstring cmdLine = L"\"" + std::wstring(exe) + L"\" --shm " + base;
		cmdLine += g_cpuWait ? L" --wait cpu" : L" --wait gpu";				// UNOFFICIAL 2026-10-30
		cmdLine += g_highPriority ? L" --gpu-priority high" : L" --gpu-priority normal";
		cmdLine += g_exactInputs ? L" --inputs exact" : L" --inputs subrect";
		// UNOFFICIAL 2026-10-31: the child refuses a render subrect outside NGX's dynamic range too (DLSS mode), and zerosubrect gives it
		// b0146f54's 0x0 for the check to catch
		cmdLine += L" --subrect-range strict";
		if (g_failTest == 3) cmdLine += L" --fail-test zero-subrect";
		Log("selftest: the child runs with --inputs %s --subrect-range strict%s", g_exactInputs ? "exact" : "subrect",
			g_failTest == 3 ? " --fail-test zero-subrect (TEST: the render subrect check must catch the 0x0)" : "");
		STARTUPINFOW si = { sizeof(si) };
		PROCESS_INFORMATION pi = {};
		if (!CreateProcessW(exe, &cmdLine[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, g_exeDir.c_str(), &si, &pi)) {
			Log("selftest FAILED: could not start the child (%lu)", GetLastError());
			return 1;
		}
		CloseHandle(pi.hThread);

		// What the fork does: four exportable images (eight with RR), two exportable timeline semaphores, handles duplicated into the child.
		const VkFormat formats[NVR_DLSS_IMAGE_COUNT] = { VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT,
			VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16_SFLOAT };
		Exported images[NVR_DLSS_IMAGE_COUNT];
		Exported biasMask;	// UNOFFICIAL 2026-11-01 protocol 3: the bias current colour mask (R16F, cleared to 1)
		VkSemaphore inputsReady = VK_NULL_HANDLE, outputReady = VK_NULL_HANDLE;
		bool ok = true;
		for (uint32_t i = 0; i < imageCount && ok; i++) {
			const bool display = i == NVR_DLSS_IMAGE_OUTPUT;
			ok = ExportImage(vk, formats[i], displayW, displayH, images[i]) && ExportHandle(vk, images[i].memory, pi.hProcess, images[i].desc.handle);
			shared->images[i] = images[i].desc;
			(void)display;
		}
		ok = ok && ExportImage(vk, VK_FORMAT_R16_SFLOAT, displayW, displayH, biasMask) && ExportHandle(vk, biasMask.memory, pi.hProcess, biasMask.desc.handle);
		shared->biasMask = biasMask.desc;
		ok = ok && ExportSemaphore(vk, inputsReady, pi.hProcess, shared->semInputsReady) && ExportSemaphore(vk, outputReady, pi.hProcess, shared->semOutputReady);
		if (!ok) { Log("selftest FAILED: export"); TerminateProcess(pi.hProcess, 1); return 1; }
		shared->displayWidth = displayW;
		shared->displayHeight = displayH;
		shared->inputWidth = displayW;
		shared->inputHeight = displayH;
		MemoryBarrier();
		shared->setupDone = 1;
		SetEvent(frameEvent);

		// Wait for READY (or FAILED).
		const ULONGLONG start = GetTickCount64();
		while (shared->state == NVR_DLSS_STATE_STARTING && GetTickCount64() - start < 15000) {
			HANDLE waits[2] = { statusEvent, pi.hProcess };
			if (WaitForMultipleObjects(2, waits, FALSE, 200) == WAIT_OBJECT_0 + 1) break;
		}
		Log("selftest: helper state %u after %llu ms: %s", shared->state, (unsigned long long)(GetTickCount64() - start), shared->message);
		if (dlssMode && shared->state == NVR_DLSS_STATE_FAILED && strstr((const char*)shared->message, "not available")) {
			WaitForSingleObject(pi.hProcess, 3000);
			Log("selftest PASSED (DLSS mode on a GPU without DLSS: the helper gave up cleanly, NVR would run FSR 2): %s", shared->message);
			return 0;
		}
		if (shared->state != NVR_DLSS_STATE_READY) { Log("selftest FAILED: the helper did not get ready"); TerminateProcess(pi.hProcess, 1); return 1; }

		// Exporter-side command buffers: clear the colour input to a per-frame colour, signal N (the helper works), wait N, read back.
		VkCommandPool pool = VK_NULL_HANDLE;
		VkCommandPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
		poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		poolInfo.queueFamilyIndex = vk.queueFamily;
		vkCreateCommandPool(vk.device, &poolInfo, nullptr, &pool);
		VkCommandBuffer cmds[2] = {};
		VkCommandBufferAllocateInfo cmdInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
		cmdInfo.commandPool = pool;
		cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cmdInfo.commandBufferCount = 2;
		vkAllocateCommandBuffers(vk.device, &cmdInfo, cmds);
		const VkDeviceSize readBytes = (VkDeviceSize)displayW * displayH * 8;
		VkBuffer readback = VK_NULL_HANDLE;
		VkDeviceMemory readMemory = VK_NULL_HANDLE;
		{
			VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
			info.size = readBytes;
			info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
			vkCreateBuffer(vk.device, &info, nullptr, &readback);
			VkMemoryRequirements req = {};
			vkGetBufferMemoryRequirements(vk.device, readback, &req);
			VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
			alloc.allocationSize = req.size;
			alloc.memoryTypeIndex = (uint32_t)FindMemoryType(vk, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
			vkAllocateMemory(vk.device, &alloc, nullptr, &readMemory);
			vkBindBufferMemory(vk.device, readback, readMemory, 0);
		}
		// UNOFFICIAL 2026-10-30 (top-left picture report) the colour input outside the render rectangle, green every frame (2.0, so it shows
		// whatever the output does with it): green in the output = the picture read past its rectangle (small, in the corner, at worst).
		VkBuffer leak = VK_NULL_HANDLE;
		VkDeviceMemory leakMemory = VK_NULL_HANDLE;
		{
			VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
			info.size = readBytes;
			info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
			vkCreateBuffer(vk.device, &info, nullptr, &leak);
			VkMemoryRequirements req = {};
			vkGetBufferMemoryRequirements(vk.device, leak, &req);
			VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
			alloc.allocationSize = req.size;
			alloc.memoryTypeIndex = (uint32_t)FindMemoryType(vk, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
			vkAllocateMemory(vk.device, &alloc, nullptr, &leakMemory);
			vkBindBufferMemory(vk.device, leak, leakMemory, 0);
			uint16_t* px = nullptr;
			vkMapMemory(vk.device, leakMemory, 0, readBytes, 0, (void**)&px);
			for (size_t p = 0; p < (size_t)displayW * displayH; p++) { px[p * 4] = 0; px[p * 4 + 1] = 0x4000; px[p * 4 + 2] = 0; px[p * 4 + 3] = 0x3C00; }
			vkUnmapMemory(vk.device, leakMemory);
		}
		// Initialise every image to GENERAL once (as the fork's initImage does).
		{
			VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
			vkBeginCommandBuffer(cmds[0], &begin);
			for (uint32_t i = 0; i < imageCount; i++)
				Barrier(cmds[0], images[i].image, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
					VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
			// UNOFFICIAL 2026-11-01 the bias mask: GENERAL, all 1 (the whole picture "the weapon"), for the whole run
			{
				Barrier(cmds[0], biasMask.image, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
					VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
				VkClearColorValue one = {};
				one.float32[0] = 1.0f;
				const VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
				vkCmdClearColorImage(cmds[0], biasMask.image, VK_IMAGE_LAYOUT_GENERAL, &one, 1, &range);
				Barrier(cmds[0], biasMask.image, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
					VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
			}
			// UNOFFICIAL 2026-10-30 the RR guides: one known colour each, for the whole run.
			for (uint32_t g = 0; rrMode && g < kGuideCount; g++) {
				const VkImage guide = images[NVR_DLSS_IMAGE_DIFFUSE_ALBEDO + g].image;
				VkClearColorValue colour = {};
				for (int c = 0; c < 4; c++) colour.float32[c] = guideColours[g][c];
				const VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
				vkCmdClearColorImage(cmds[0], guide, VK_IMAGE_LAYOUT_GENERAL, &colour, 1, &range);
				Barrier(cmds[0], guide, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
					VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
			}
			vkEndCommandBuffer(cmds[0]);
			VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
			submit.commandBufferCount = 1;
			submit.pCommandBuffers = &cmds[0];
			vkQueueSubmit(vk.queue, 1, &submit, VK_NULL_HANDLE);
			vkQueueWaitIdle(vk.queue);
		}

		LARGE_INTEGER frequency;
		QueryPerformanceFrequency(&frequency);
		const uint64_t frames = 300;
		uint32_t checked = 0, failures = 0;
		double roundTripMs = 0.0, worstMs = 0.0;
		for (uint64_t n = 1; n <= frames && ok; n++) {
			// 1. inputs (the left half of the render rectangle red-ish, the right half blue-ish, brightness by frame), signal N
			const float level = 0.25f + 0.5f * (float)(n % 4) / 3.0f;
			VkCommandBuffer cmd = cmds[0];
			vkResetCommandBuffer(cmd, 0);
			VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
			vkBeginCommandBuffer(cmd, &begin);
			VkClearColorValue color = {};
			color.float32[0] = level; color.float32[1] = 0.0f; color.float32[2] = 1.0f - level; color.float32[3] = 1.0f;
			const VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			vkCmdClearColorImage(cmd, images[NVR_DLSS_IMAGE_COLOR].image, VK_IMAGE_LAYOUT_GENERAL, &color, 1, &range);
			// UNOFFICIAL 2026-10-30 the right and bottom strips outside the render rectangle green (the leak buffer)
			Barrier(cmd, images[NVR_DLSS_IMAGE_COLOR].image, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL,
				VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
			VkBufferImageCopy strips[2] = {};
			for (VkBufferImageCopy& s : strips) s.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			strips[0].imageOffset = { (int32_t)renderW, 0, 0 };
			strips[0].imageExtent = { displayW - renderW, displayH, 1 };
			strips[1].imageOffset = { 0, (int32_t)renderH, 0 };
			strips[1].imageExtent = { renderW, displayH - renderH, 1 };
			vkCmdCopyBufferToImage(cmd, leak, images[NVR_DLSS_IMAGE_COLOR].image, VK_IMAGE_LAYOUT_GENERAL, 2, strips);
			Barrier(cmd, images[NVR_DLSS_IMAGE_COLOR].image, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
				VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
			vkEndCommandBuffer(cmd);
			NvrDlssFrame& f = shared->frames[n % NVR_DLSS_FRAME_RING];
			f = NvrDlssFrame();
			f.frame = n;
			f.renderWidth = renderW;
			f.renderHeight = renderH;
			f.reset = n == 1;
			f.mvScaleX = (float)renderW;
			f.mvScaleY = (float)renderH;
			f.preExposure = 1.0f;
			f.frameTimeMs = 16.7f;
			f.flags = NVR_DLSS_FRAME_BIAS_MASK;	// UNOFFICIAL 2026-11-01 protocol 3 (an RR frame does not use it)
			if (rrMode) {
				// UNOFFICIAL 2026-10-30 an RR frame as NVR sends one: linear HDR colour, the default presets, identity matrices.
				f.flags |= NVR_DLSS_FRAME_RR;
				f.hdr = 1;
				for (int i = 0; i < 4; i++) f.worldToView[i * 5] = f.viewToClip[i * 5] = 1.0f;
			}
			LARGE_INTEGER written;
			QueryPerformanceCounter(&written);
			f.writtenQpc = written.QuadPart;
			MemoryBarrier();
			InterlockedExchange64((volatile LONG64*)&shared->latestFrame, (LONG64)n);
			SetEvent(frameEvent);
			const uint64_t value = n;
			VkTimelineSemaphoreSubmitInfo timeline = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
			timeline.signalSemaphoreValueCount = 1;
			timeline.pSignalSemaphoreValues = &value;
			VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO, &timeline };
			submit.commandBufferCount = 1;
			submit.pCommandBuffers = &cmd;
			submit.signalSemaphoreCount = 1;
			submit.pSignalSemaphores = &inputsReady;
			vkQueueSubmit(vk.queue, 1, &submit, VK_NULL_HANDLE);

			// 2. GPU wait for the helper's N, then (every 50th frame) copy the output back and look at it.
			const bool check = (n % 50) == 0 || n == frames;
			VkCommandBuffer cmd2 = cmds[1];
			vkResetCommandBuffer(cmd2, 0);
			vkBeginCommandBuffer(cmd2, &begin);
			if (check) {
				VkBufferImageCopy copy = {};
				copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
				copy.imageExtent = { displayW, displayH, 1 };
				vkCmdCopyImageToBuffer(cmd2, images[NVR_DLSS_IMAGE_OUTPUT].image, VK_IMAGE_LAYOUT_GENERAL, readback, 1, &copy);
			}
			vkEndCommandBuffer(cmd2);
			const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
			VkTimelineSemaphoreSubmitInfo timeline2 = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
			timeline2.waitSemaphoreValueCount = 1;
			timeline2.pWaitSemaphoreValues = &value;
			VkSubmitInfo submit2 = { VK_STRUCTURE_TYPE_SUBMIT_INFO, &timeline2 };
			submit2.waitSemaphoreCount = 1;
			submit2.pWaitSemaphores = &outputReady;
			submit2.pWaitDstStageMask = &stage;
			submit2.commandBufferCount = 1;
			submit2.pCommandBuffers = &cmd2;
			vkQueueSubmit(vk.queue, 1, &submit2, VK_NULL_HANDLE);
			// The test's own CPU wait (NVR never does this): the round trip, and the command buffers can be reused.
			VkSemaphoreWaitInfo wait = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
			wait.semaphoreCount = 1;
			wait.pSemaphores = &outputReady;
			wait.pValues = &value;
			if (vkWaitSemaphores(vk.device, &wait, 5ull * 1000 * 1000 * 1000) != VK_SUCCESS) {
				Log("selftest FAILED: frame %llu did not come back within 5 s (helper: %s)", (unsigned long long)n, shared->message);
				ok = false;
				break;
			}
			vkQueueWaitIdle(vk.queue);
			LARGE_INTEGER done;
			QueryPerformanceCounter(&done);
			const double ms = (double)(done.QuadPart - written.QuadPart) * 1000.0 / (double)frequency.QuadPart;
			roundTripMs += ms;
			worstMs = std::max(worstMs, ms);
			// UNOFFICIAL 2026-10-30 (top-left picture report): green near the output's right and bottom edges = the picture was read past its
			// render rectangle (the strips outside it are green); inner points 4 pixels in (any upscaler), and in the pass-through with
			// --inputs exact also the very last column and row (the subrect stretch's bilinear tap there reaches a little past the rectangle).
			auto leaked = [&](const uint16_t* px, bool edges, char* where, size_t size) {
				const uint32_t xs[3] = { displayW - 4, displayW / 2, displayW - 4 }, ys[3] = { displayH / 2, displayH - 4, displayH - 4 };
				const uint32_t ex[2] = { displayW - 1, displayW / 2 }, ey[2] = { displayH / 2, displayH - 1 };
				for (int i = 0; i < 3; i++) {
					const float g = HalfToFloat(px[((size_t)ys[i] * displayW + xs[i]) * 4 + 1]);
					if (g > 0.5f) { snprintf(where, size, "green %.3f at (%u, %u)", g, xs[i], ys[i]); return true; }
				}
				for (int i = 0; edges && i < 2; i++) {
					const float g = HalfToFloat(px[((size_t)ey[i] * displayW + ex[i]) * 4 + 1]);
					if (g > 0.01f) { snprintf(where, size, "green %.3f at the edge (%u, %u)", g, ex[i], ey[i]); return true; }
				}
				return false;
			};
			if (check && dlssMode) {
				// UNOFFICIAL 2026-10-30 DLSS mode: the output is DLSS's own picture, not the pass-through's pattern: the frame came back,
				// and the counters below say which feature ran it; no green from outside the render rectangle in it.
				checked++;
				const uint16_t* px = nullptr;
				vkMapMemory(vk.device, readMemory, 0, readBytes, 0, (void**)&px);
				char where[96] = "";
				const bool leakSeen = !rrMode && leaked(px, false, where, sizeof(where));
				vkUnmapMemory(vk.device, readMemory);
				if (leakSeen) failures++;
				Log("selftest frame %llu: back (DLSS SR %u, Ray Reconstruction %u so far); outside the render rectangle %s%s; round trip %.3f ms",
					(unsigned long long)n, shared->dlssEvaluated, shared->rrEvaluated, leakSeen ? "REACHED the output: " : "kept out", where, ms);
			}
			else if (check) {
				checked++;
				const uint16_t* px = nullptr;
				vkMapMemory(vk.device, readMemory, 0, readBytes, 0, (void**)&px);
				auto at = [&](uint32_t x, uint32_t y, int c) { return HalfToFloat(px[((size_t)y * displayW + x) * 4 + c]); };
				if (rrMode) {
					// UNOFFICIAL 2026-10-30 each guide's thumbnail centre shows its colour (the pass-through RR drawing, GuideThumbnail).
					for (uint32_t g = 0; g < kGuideCount; g++) {
						VkOffset2D tAt = {};
						VkExtent2D tSize = {};
						bool good = GuideThumbnail(displayW, displayH, renderW, renderH, g, tAt, tSize);
						float c[3] = {};
						if (good) {
							for (int k = 0; k < 3; k++) {
								c[k] = at((uint32_t)tAt.x + tSize.width / 2, (uint32_t)tAt.y + tSize.height / 2, k);
								good = good && fabsf(c[k] - guideColours[g][k]) < 0.01f;
							}
						}
						if (!good) failures++;
						Log("selftest frame %llu: %s thumbnail (%.3f, %.3f, %.3f) want (%.3f, %.3f, %.3f) %s", (unsigned long long)n,
							kImageNames[NVR_DLSS_IMAGE_DIFFUSE_ALBEDO + g], c[0], c[1], c[2], guideColours[g][0], guideColours[g][1], guideColours[g][2],
							good ? "ok" : "WRONG");
					}
				}
				// the stretched input (uniform colour) in the middle, the moving magenta box at frame % 8, the green box
				const float r = at(displayW / 2, displayH / 2, 0), b = at(displayW / 2, displayH / 2, 2);
				const uint32_t mx = 16 + 32 * (uint32_t)(n % 8) + 12, other = 16 + 32 * (uint32_t)((n + 4) % 8) + 12;
				const bool picture = fabsf(r - level) < 0.01f && fabsf(b - (1.0f - level)) < 0.01f;
				const bool marker = at(mx, 28, 0) > 3.5f && at(mx, 28, 1) < 0.5f && at(16 + 32 * 8 + 12, 28, 1) > 3.5f;
				const bool notStale = !(at(other, 28, 0) > 3.5f && at(other, 28, 1) < 0.5f);
				char where[96] = "";
				const bool leakSeen = leaked(px, g_exactInputs || g_strictEdges, where, sizeof(where));	// UNOFFICIAL 2026-10-30
				if (!picture || !marker || !notStale || leakSeen) failures++;
				Log("selftest frame %llu: centre (%.3f, %.3f) want (%.3f, %.3f) %s; marker %s; old marker gone %s; outside the render rectangle %s%s; round trip %.3f ms",
					(unsigned long long)n, r, b, level, 1.0f - level, picture ? "ok" : "WRONG", marker ? "ok" : "WRONG", notStale ? "ok" : "WRONG",
					leakSeen ? "REACHED the output (WRONG): " : "kept out", where, ms);
				vkUnmapMemory(vk.device, readMemory);
			}
		}
		// UNOFFICIAL 2026-10-31 the helper's state before it is told to exit: READY, or FAILED = it reported a failure during the frames (a
		// DLSS evaluate refused or not called, the render subrect check), which in the game sends NVR to FSR 2 for the session.
		const uint32_t endState = shared->state;
		char endMessage[sizeof(shared->message)];
		memcpy(endMessage, (const void*)shared->message, sizeof(endMessage));
		endMessage[sizeof(endMessage) - 1] = 0;
		shared->quit = 1;
		SetEvent(frameEvent);
		WaitForSingleObject(pi.hProcess, 3000);
		const bool stillReady = endState == NVR_DLSS_STATE_READY;
		Log("selftest: helper state at the end %u, %s: %s", endState, stillReady ? "READY" : "NOT READY (it reported a failure)", endMessage);
		Log("selftest: helper submitted %llu frames, its GPU time %.3f ms avg, its response %.3f ms avg (max %.3f), %u overwritten; "
			"test round trip (CPU write -> output back, with the test's own waits) %.3f ms avg, %.3f max",
			(unsigned long long)shared->framesSubmitted, shared->gpuTimeUsAvg / 1000.0, shared->responseUsAvg / 1000.0, shared->responseUsMax / 1000.0,
			shared->paramMismatches, roundTripMs / (double)frames, worstMs);
		bool passed = ok && checked > 0 && failures == 0 && shared->framesSubmitted == frames && stillReady;
		// UNOFFICIAL 2026-10-30 the counters: in DLSS mode every frame ran DLSS SR or RR (RR not available on this PC: all SR, which
		// is NVR's fallback); the pass-through RR test evaluated every frame as an RR frame and said so.
		// UNOFFICIAL 2026-11-01 protocol 3: every DLSS SR (or pass-through SR) frame had the bias mask; RR frames do not use it
		{
			const uint32_t want = dlssMode ? shared->dlssEvaluated : (rrMode ? 0u : (uint32_t)frames);
			const bool maskCounted = shared->biasMaskFrames == want;
			Log("selftest: bias current colour mask on %u frames (want %u)%s", shared->biasMaskFrames, want, maskCounted ? "" : ": WRONG");
			passed = passed && maskCounted;
		}
		if (dlssMode) {
			const bool counted = shared->dlssEvaluated + shared->rrEvaluated == frames;
			Log("selftest: DLSS SR ran %u frames, Ray Reconstruction %u (rrAvailable %u, last RR result 0x%08X)%s", shared->dlssEvaluated,
				shared->rrEvaluated, shared->rrAvailable, (unsigned)shared->rrResult, counted ? "" : ": NOT every frame");
			passed = passed && counted;
		}
		else if (rrMode) {
			const bool counted = shared->rrAvailable == 1 && shared->rrEvaluated == frames;
			Log("selftest: rrAvailable %u, %u RR frames evaluated (want 1, %llu)%s", shared->rrAvailable, shared->rrEvaluated, (unsigned long long)frames,
				counted ? "" : ": WRONG");
			passed = passed && counted;
		}
		// UNOFFICIAL 2026-10-31 zerosubrect: the child was given b0146f54's 0x0 render subrect; the test passes when the check caught it
		// (the child FAILED with "the render subrect is empty") and everything else held.
		if (g_failTest == 3) {
			const bool caught = !stillReady && strstr(endMessage, "the render subrect is empty") != nullptr;
			const bool rest = ok && checked > 0 && failures == 0 && shared->framesSubmitted == frames;
			Log("selftest zerosubrect %s: the 0x0 render subrect %s (%u frames checked, %u wrong)", caught && rest ? "PASSED" : "FAILED",
				caught ? "was caught by the check" : "was NOT caught", checked, failures);
			vkDeviceWaitIdle(vk.device);
			return caught && rest ? 0 : 1;
		}
		Log("selftest %s (%u frames checked, %u wrong)", passed ? "PASSED" : "FAILED", checked, failures);
		vkDeviceWaitIdle(vk.device);
		return passed ? 0 : 1;
	}
}

int wmain(int argc, wchar_t** argv) {
	InitializeCriticalSection(&g_logLock);
	g_exeDir = ExeDir();
	for (int i = 1; i < argc; i++) {
		if (!wcscmp(argv[i], L"--shm") && i + 1 < argc) {
			// UNOFFICIAL 2026-10-30 (DLAA stall report): the start options after it
			for (int j = i + 2; j + 1 < argc; j += 2) {
				if (!wcscmp(argv[j], L"--wait")) g_cpuWait = !wcscmp(argv[j + 1], L"cpu");
				else if (!wcscmp(argv[j], L"--gpu-priority")) g_highPriority = !wcscmp(argv[j + 1], L"high");
				else if (!wcscmp(argv[j], L"--inputs")) g_exactInputs = wcscmp(argv[j + 1], L"subrect") != 0;	// UNOFFICIAL 2026-10-30
				else if (!wcscmp(argv[j], L"--fail-test")) g_failTest = !wcscmp(argv[j + 1], L"report") ? 1 : !wcscmp(argv[j + 1], L"hang") ? 2 :
					!wcscmp(argv[j + 1], L"zero-subrect") ? 3 : 0;
				else if (!wcscmp(argv[j], L"--subrect-range")) g_strictSubrect = !wcscmp(argv[j + 1], L"strict");	// UNOFFICIAL 2026-10-31
			}
			return RunHelper(argv[i + 1]);
		}
		if (!wcscmp(argv[i], L"--probe")) return RunProbe();
		if (!wcscmp(argv[i], L"--selftest")) {
			// UNOFFICIAL 2026-10-30: "dlss" and "rr" after it, in either order (--selftest rr, --selftest dlss rr).
			bool dlss = false, rr = false;
			for (int j = i + 1; j < argc; j++) {
				if (!wcscmp(argv[j], L"dlss")) dlss = true;
				else if (!wcscmp(argv[j], L"rr")) rr = true;
				else if (!wcscmp(argv[j], L"cpuwait")) g_cpuWait = true;		// UNOFFICIAL 2026-10-30: the child gets --wait cpu
				else if (!wcscmp(argv[j], L"highprio")) g_highPriority = true;	// and --gpu-priority high
				else if (!wcscmp(argv[j], L"subrect")) g_exactInputs = false;	// and --inputs subrect
				else if (!wcscmp(argv[j], L"edges")) g_strictEdges = true;	// (the check of the last column / row with subrect too)
				else if (!wcscmp(argv[j], L"zerosubrect")) g_failTest = 3;	// UNOFFICIAL 2026-10-31: the child gets --fail-test zero-subrect
			}
			return RunSelfTest(dlss, rr);
		}
	}
	printf("NvrDlssHelper: started by New Vegas Reloaded. By hand: --probe (is DLSS available here?) or --selftest [dlss] [rr] [cpuwait] [highprio] [subrect] [edges] [zerosubrect] (the exchange without the game).\n");
	return 1;
}
