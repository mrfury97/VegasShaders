// pg_host.h: the game's side of the RT core for the RT playground (tools/nvr-rt-playground/README.md).
//
// The same device / helper / shared-image / exchange code as tools/nvr-rt-host/rt_host.cpp's Host (kept as a copy, not an include:
// rt_host.cpp is one translation unit with its own main and scene list), changed for an interactive program:
//   - a real window and swap chain of the window's size (the host's is a hidden 64 x 48 one);
//   - no per-frame texture allocation: inputs go from persistent D3D9 textures with CopyIn (RtCore::CopyIn's CopyToShared), outputs
//     come back with CopyOut into persistent render targets (RtCore::CopyOut) and are drawn from there on the GPU: no CPU read-back;
//   - instances carry a full 3 x 4 transform (rotation x uniform scale + translation), so things can turn.
// Everything else (the shared block, the frame ring, the ops, the semaphores, the helper's start) is the host's, line for line.
#pragma once
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "NvrDxvkExt.h"
#include "NvrRtProtocol.h"

void pgLog(const char* fmt, ...);
#define PG_CHECK(hr, what) do { HRESULT _hr = (hr); if (FAILED(_hr)) { pgLog("  error: %s -> 0x%08x\n", what, unsigned(_hr)); return false; } } while (0)

struct PgImage {
	ID3D9NvrExtSharedImage* shared = nullptr;
	UINT w = 0, h = 0;              // logical size (the shared image is rounded up to 256 multiples, as RtCore::AddImage)
	NVR_EXT_SHARED_FORMAT format = NVR_EXT_SHARED_R32F;
	const char* name = "";
};

inline D3DFORMAT pgD3dFormat(NVR_EXT_SHARED_FORMAT f) {
	switch (f) {
	case NVR_EXT_SHARED_RGBA16F: return D3DFMT_A16B16G16R16F;
	case NVR_EXT_SHARED_RG16F: return D3DFMT_G16R16F;
	case NVR_EXT_SHARED_R32F: return D3DFMT_R32F;
	case NVR_EXT_SHARED_RG32F: return D3DFMT_G32R32F;
	case NVR_EXT_SHARED_RGBA8: return D3DFMT_A8B8G8R8;
	case NVR_EXT_SHARED_R16F: return D3DFMT_R16F;
	}
	return D3DFMT_UNKNOWN;
}

// A mesh staged once (BUILD op) and kept alive in the helper until released
struct PgStagedMesh { uint32_t id = 0; IDirect3DVertexBuffer9* vb = nullptr; IDirect3DIndexBuffer9* ib = nullptr; };

// One traced instance this frame (world space; made camera relative in Exchange)
struct PgInstance {
	float transform[3][4];    // rows = world axes, column 3 = world translation
	uint32_t geometryId = 0;
	uint32_t customIndex = 0, mask = 0x02, flags = 0x1;   // flags: both faces (RtCore)
	float albedo[3] = { 0.5f, 0.5f, 0.5f };
	uint32_t recordFlags = 0;
};

struct PgHost {
	IDirect3D9* d3d = nullptr;
	IDirect3DDevice9* dev = nullptr;
	ID3D9NvrExtShare* share = nullptr;
	ID3D9NvrExtRt* rt = nullptr;
	ID3D9NvrExtRtSeq* rtSeq = nullptr;
	ID3D9NvrExt* ext = nullptr;
	ID3D9NvrExtSharedFence* inputsReady = nullptr;
	ID3D9NvrExtSharedFence* outputReady = nullptr;
	std::vector<PgImage> images;      // index = the protocol's image index (0 = the arena)
	std::vector<std::string> shaders;
	NvrRtShared* shared = nullptr;
	HANDLE mapping = nullptr, frameEvent = nullptr, statusEvent = nullptr;
	PROCESS_INFORMATION helper = {};
	uint64_t frame = 0;
	UINT arenaW = 4096, arenaH = 512;
	std::vector<NvrRtGeometryOp> ops;
	uint32_t arenaTexel = 0;
	std::vector<NvrRtPass> passes;
	std::vector<PgStagedMesh> staged;   // buffers kept until the frame's exchange is done
	uint32_t nextGeometry = 1;
	std::wstring baseName;
	char deviceName[256] = {};
	uint32_t scratchMB = 64;
	static const uint32_t kTexStagingMB = 32;
	HANDLE texMapping = nullptr;
	uint8_t* texStaging = nullptr;
	uint32_t texOffset = 0;

	int AddImage(const char* name, UINT w, UINT h, NVR_EXT_SHARED_FORMAT f) {
		if (images.size() >= NVR_RT_MAX_IMAGES) { pgLog("  error: more than %u shared images\n", NVR_RT_MAX_IMAGES); return -1; }
		PgImage im; im.w = w; im.h = h; im.format = f; im.name = name;
		NVR_EXT_SHARED_IMAGE_DESC d = { (w + 255u) & ~255u, (h + 255u) & ~255u, f, name };
		if (FAILED(share->CreateSharedImage(&d, &im.shared))) { pgLog("  error: CreateSharedImage %s\n", name); return -1; }
		images.push_back(im);
		return int(images.size()) - 1;
	}
	int AddShader(const std::string& path) { shaders.push_back(path); return int(shaders.size()) - 1; }

	// (2026-11-07) no --device: the first Vulkan GPU with VK_KHR_ray_query, by name for DXVK (a PC with a GTX 10xx + an iGPU otherwise got
	// the GTX, which has no ray queries, and the helper refused it). vulkan-1.dll is loaded by hand: no Vulkan SDK needed to build or run.
	static bool PickRayQueryGpu(char* name, size_t size) {
		struct Props { uint32_t api, driver, vendor, device, type; char name[256]; uint8_t rest[1024]; };
		struct Ext { char name[256]; uint32_t spec; };
		typedef int (__stdcall *PFN_CreateInstance)(const void*, const void*, void**);
		typedef int (__stdcall *PFN_Enumerate)(void*, uint32_t*, void**);
		typedef void (__stdcall *PFN_Props)(void*, Props*);
		typedef int (__stdcall *PFN_Exts)(void*, const char*, uint32_t*, Ext*);
		typedef void (__stdcall *PFN_Destroy)(void*, const void*);
		typedef void* (__stdcall *PFN_GetProc)(void*, const char*);
		HMODULE vk = LoadLibraryA("vulkan-1.dll");
		if (!vk) return false;
		auto getProc = (PFN_GetProc)GetProcAddress(vk, "vkGetInstanceProcAddr");
		auto create = getProc ? (PFN_CreateInstance)getProc(nullptr, "vkCreateInstance") : nullptr;
		bool found = false;
		if (create) {
			struct AppInfo { uint32_t sType; const void* next; const char* app; uint32_t appV; const char* eng; uint32_t engV; uint32_t api; } app = { 0, nullptr, "NvrRtPlayground", 1, "", 0, (1u << 22) | (2u << 12) };
			struct CreateInfo { uint32_t sType; const void* next; uint32_t flags; const AppInfo* app; uint32_t layers; const char* const* layerNames; uint32_t exts; const char* const* extNames; } ci = { 1, nullptr, 0, &app, 0, nullptr, 0, nullptr };
			void* inst = nullptr;
			if (create(&ci, nullptr, &inst) == 0 && inst) {
				auto enumerate = (PFN_Enumerate)getProc(inst, "vkEnumeratePhysicalDevices");
				auto props = (PFN_Props)getProc(inst, "vkGetPhysicalDeviceProperties");
				auto exts = (PFN_Exts)getProc(inst, "vkEnumerateDeviceExtensionProperties");
				uint32_t n = 0;
				if (enumerate && props && exts && enumerate(inst, &n, nullptr) == 0 && n) {
					std::vector<void*> gpus(n);
					enumerate(inst, &n, gpus.data());
					for (uint32_t i = 0; i < n && !found; i++) {
						uint32_t e = 0;
						if (exts(gpus[i], nullptr, &e, nullptr) != 0 || !e) continue;
						std::vector<Ext> list(e);
						exts(gpus[i], nullptr, &e, list.data());
						for (const Ext& x : list) if (!strcmp(x.name, "VK_KHR_ray_query")) {
							Props p = {}; props(gpus[i], &p);
							strncpy_s(name, size, p.name, _TRUNCATE);
							found = true; break;
						}
					}
				}
				auto destroy = (PFN_Destroy)getProc(inst, "vkDestroyInstance");
				if (destroy) destroy(inst, nullptr);
			}
		}
		FreeLibrary(vk);
		return found;
	}

	// The 32-bit Vulkan drivers may hide ray queries (AMD's do): ask the 64-bit helper instead ("probe: the helper can run on <name>")
	static bool ProbeHelperGpu(const char* helperExe, char* name, size_t size) {
		SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
		HANDLE rd = nullptr, wr = nullptr;
		if (!helperExe || !*helperExe || !CreatePipe(&rd, &wr, &sa, 0)) return false;
		SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
		STARTUPINFOA si = { sizeof(si) }; si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = wr; si.hStdError = wr; si.hStdInput = nullptr;
		PROCESS_INFORMATION pi = {};
		std::string cmd = std::string("\"") + helperExe + "\" --probe";
		std::vector<char> line(cmd.begin(), cmd.end()); line.push_back(0);
		const BOOL ok = CreateProcessA(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
		CloseHandle(wr);
		std::string out;
		if (ok) {
			char buf[4096]; DWORD n = 0;
			while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n) out.append(buf, n);
			WaitForSingleObject(pi.hProcess, 60000);
			CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
		}
		CloseHandle(rd);
		const char* key = "probe: the helper can run on ";
		const size_t at = out.find(key);
		if (at == std::string::npos) return false;
		const size_t from = at + strlen(key), to = out.find_first_of("\r\n", from);
		strncpy_s(name, size, out.substr(from, to == std::string::npos ? std::string::npos : to - from).c_str(), _TRUNCATE);
		return name[0] != 0;
	}

	bool Init(HWND win, UINT backW, UINT backH, const char* device, const char* helperExe = nullptr) {
		if (device && *device) SetEnvironmentVariableA("DXVK_FILTER_DEVICE_NAME", device);
		else {
			char gpu[256] = {};
			if (ProbeHelperGpu(helperExe, gpu, sizeof(gpu)) || PickRayQueryGpu(gpu, sizeof(gpu))) {
				SetEnvironmentVariableA("DXVK_FILTER_DEVICE_NAME", gpu); pgLog("GPU with ray queries: %s (no --device given)\n", gpu);
			}
			else pgLog("no GPU with VK_KHR_ray_query found: DXVK picks (ray tracing will likely not start)\n");
		}
		d3d = Direct3DCreate9(D3D_SDK_VERSION);
		if (!d3d) { pgLog("  error: Direct3DCreate9 (is the fork's d3d9.dll next to the exe?)\n"); return false; }
		D3DPRESENT_PARAMETERS pp = {};
		pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = win;
		pp.BackBufferWidth = backW; pp.BackBufferHeight = backH; pp.BackBufferFormat = D3DFMT_X8R8G8B8;
		pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;   // uncapped: the fps readout is the cost
		PG_CHECK(d3d->CreateDevice(0, D3DDEVTYPE_HAL, win, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev), "CreateDevice");
		PG_CHECK(dev->QueryInterface(__uuidof(ID3D9NvrExt), (void**)&ext), "ID3D9NvrExt (not the NVR DXVK fork?)");
		PG_CHECK(ext->QueryInterface(__uuidof(ID3D9NvrExtShare), (void**)&share), "ID3D9NvrExtShare");
		PG_CHECK(ext->QueryInterface(__uuidof(ID3D9NvrExtRt), (void**)&rt), "ID3D9NvrExtRt");
		if (FAILED(ext->QueryInterface(__uuidof(ID3D9NvrExtRtSeq), (void**)&rtSeq))) rtSeq = nullptr;
		NVR_EXT_SHARE_INFO si = {}; share->GetShareInfo(&si);
		strncpy_s(deviceName, si.DeviceName, _TRUNCATE);
		pgLog("device: %s (vendor 0x%04X), sharing %s, arena sequence texel %s\n", si.DeviceName, si.VendorId, si.Supported ? "yes" : "NO",
			rtSeq ? "yes" : "no");
		if (!si.Supported) return false;
		PG_CHECK(share->CreateSharedFence(0, &inputsReady), "inputsReady");
		PG_CHECK(share->CreateSharedFence(0, &outputReady), "outputReady");
		if (AddImage("arena", arenaW, arenaH, NVR_EXT_SHARED_R32F) != 0) return false;
		wchar_t base[128];
		swprintf(base, 128, L"Local\\NvrRtPlayground-%lu-%llu", GetCurrentProcessId(), (unsigned long long)GetTickCount64());
		baseName = base;
		mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(NvrRtShared), base);
		shared = mapping ? (NvrRtShared*)MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(NvrRtShared)) : nullptr;
		if (!shared) { pgLog("  error: shared block\n"); return false; }
		std::memset(shared, 0, sizeof(NvrRtShared));
		shared->magic = NVR_RT_MAGIC; shared->version = NVR_RT_PROTOCOL_VERSION; shared->size = sizeof(NvrRtShared);
		shared->parentPid = GetCurrentProcessId();
		texMapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, kTexStagingMB << 20, (std::wstring(base) + NVR_RT_EVENT_TEX_SUFFIX).c_str());
		texStaging = texMapping ? (uint8_t*)MapViewOfFile(texMapping, FILE_MAP_ALL_ACCESS, 0, 0, (size_t)kTexStagingMB << 20) : nullptr;
		if (texStaging) shared->textureStagingMB = kTexStagingMB;
		shared->vendorId = si.VendorId;
		std::memcpy(shared->deviceLuid, si.DeviceLUID, 8); shared->deviceLuidValid = si.DeviceLUIDValid;
		std::memcpy(shared->deviceUuid, si.DeviceUUID, 16); std::memcpy(shared->driverUuid, si.DriverUUID, 16);
		return true;
	}

	static bool giveHandle(HANDLE mine, HANDLE child, uint64_t& out) {
		HANDLE remote = nullptr;
		if (!DuplicateHandle(GetCurrentProcess(), mine, child, &remote, 0, FALSE, DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE)) return false;
		out = (uint64_t)(uintptr_t)remote;
		return true;
	}

	// The helper: NvrRtHelper.exe --shm <name>, the images' handles, ready within timeoutMs (the first start after a shader change
	// compiles its pipelines: tens of seconds on an iGPU)
	bool Start(const char* helperExe, DWORD timeoutMs) {
		if (images.size() > NVR_RT_MAX_IMAGES || shaders.size() > NVR_RT_MAX_SHADERS) { pgLog("  error: too many images / shaders\n"); return false; }
		shared->scratchMB = scratchMB;
		frameEvent = CreateEventW(nullptr, FALSE, FALSE, (baseName + NVR_RT_EVENT_FRAME_SUFFIX).c_str());
		statusEvent = CreateEventW(nullptr, FALSE, FALSE, (baseName + NVR_RT_EVENT_STATUS_SUFFIX).c_str());
		for (size_t i = 0; i < shaders.size(); i++) strncpy_s(shared->shaderPaths[i], shaders[i].c_str(), _TRUNCATE);
		MemoryBarrier();
		shared->shaderCount = uint32_t(shaders.size());
		wchar_t exe[MAX_PATH]; MultiByteToWideChar(CP_ACP, 0, helperExe, -1, exe, MAX_PATH);
		std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --shm " + baseName;
		STARTUPINFOW su = { sizeof(su) };
		if (!CreateProcessW(exe, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &su, &helper)) {
			pgLog("  error: could not start %s (%lu)\n", helperExe, GetLastError()); return false;
		}
		shared->imageCount = uint32_t(images.size());
		for (size_t i = 0; i < images.size(); i++) {
			NVR_EXT_SHARED_IMAGE_INFO in = {}; images[i].shared->GetInfo(&in);
			NvrRtImageDesc& d = shared->images[i];
			d.allocationSize = in.AllocationSize; d.memoryTypeIndex = in.MemoryTypeIndex; d.vkFormat = in.VkFormat; d.width = in.Width; d.height = in.Height;
			d.usage = in.Usage; d.flags = in.Flags; d.tiling = in.Tiling; d.layout = in.Layout; d.handleType = in.HandleType; d.dedicated = in.Dedicated;
			HANDLE h = nullptr;
			PG_CHECK(images[i].shared->CreateHandle(&h), "image CreateHandle");
			if (!giveHandle(h, helper.hProcess, d.handle)) { pgLog("  error: DuplicateHandle\n"); return false; }
		}
		HANDLE h1 = nullptr, h2 = nullptr;
		PG_CHECK(inputsReady->CreateHandle(&h1), "fence handle"); PG_CHECK(outputReady->CreateHandle(&h2), "fence handle");
		if (!giveHandle(h1, helper.hProcess, shared->semInputsReady) || !giveHandle(h2, helper.hProcess, shared->semOutputReady)) return false;
		MemoryBarrier();
		shared->setupDone = 1;
		SetEvent(frameEvent);
		const ULONGLONG start = GetTickCount64();
		while (shared->state == NVR_RT_STATE_STARTING && GetTickCount64() - start < timeoutMs) {
			HANDLE waits[2] = { statusEvent, helper.hProcess };
			if (WaitForMultipleObjects(2, waits, FALSE, 200) == WAIT_OBJECT_0 + 1) break;
			MSG m; while (PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }   // (the window stays responsive)
		}
		pgLog("helper: state %u after %llu ms: %s\n", shared->state, (unsigned long long)(GetTickCount64() - start), shared->message);
		if (shared->state != NVR_RT_STATE_READY) return false;
		if (shared->shaderFailures) { pgLog("  error: helper shader failures 0x%x (see NvrRtHelper.log)\n", shared->shaderFailures); return false; }
		return true;
	}

	// (2026-11-07) THE SHUTDOWN, in order (a close used to reset the display driver, System event 4101: the process left with the D3D9
	// queue still waiting on the helper's "output ready" for frames the helper never ran: it broke out on `quit` before them):
	//   1. no more frames (the caller stopped calling Exchange); the helper, still running, finishes every frame submitted: wait on the CPU
	//      for outputReady >= frame. If it is gone or hung, SignalFromHost releases our GPU queue's waits (the fork's recovery path);
	//   2. drain DXVK: an EVENT query issued and polled with D3DGETDATA_FLUSH until the GPU has run everything we recorded;
	//   3. ask the helper to quit (it waits for its last frame, then exits); TerminateProcess only when it is truly hung (logged);
	//   4. Release(): the caller's D3D9 resources first, then the shared images, fences, the fork's interfaces, the device.
	// Returns false when something had to be forced (logged).
	bool Stop(DWORD timeoutMs = 15000) {
		if (!shared) return true;
		bool clean = true;
		const ULONGLONG start = GetTickCount64();
		if (outputReady && frame > 0) {
			UINT64 v = 0;
			while (SUCCEEDED(outputReady->GetCompletedValue(&v)) && v < frame && HelperAlive() && GetTickCount64() - start < timeoutMs) Sleep(1);
			outputReady->GetCompletedValue(&v);
			if (v < frame) {
				pgLog("shutdown: the helper did not finish frame %llu (output ready at %llu, helper %s): released from the host side\n",
					(unsigned long long)frame, (unsigned long long)v, HelperAlive() ? "running" : "gone");
				outputReady->SignalFromHost(frame);
				clean = false;
			}
		}
		if (!Drain(timeoutMs)) {
			// something on our queue still waits: release every GPU wait on the helper's semaphore, then give the queue another chance
			if (outputReady) outputReady->SignalFromHost(frame + 16);
			if (!Drain(5000)) pgLog("shutdown: the D3D9 queue still did not drain after the release\n");
			clean = false;
		}
		shared->quit = 1;
		MemoryBarrier();
		if (frameEvent) SetEvent(frameEvent);
		if (helper.hProcess) {
			if (WaitForSingleObject(helper.hProcess, timeoutMs) != WAIT_OBJECT_0) {
				pgLog("shutdown: the helper did not exit within %lu ms (hung): terminated\n", timeoutMs);
				TerminateProcess(helper.hProcess, 1);
				WaitForSingleObject(helper.hProcess, 5000);
				clean = false;
			}
			CloseHandle(helper.hProcess); CloseHandle(helper.hThread); helper = {};
		}
		pgLog("shutdown: %s in %llu ms\n", clean ? "clean" : "FORCED (see above)", (unsigned long long)(GetTickCount64() - start));
		return clean;
	}
	// every recorded D3D9 / fork command executed on the GPU. The fork's own fence: SignalShared submits everything recorded so far with a
	// signal of our "inputs ready" semaphore (a value past every frame: the helper never waits for it), polled on the CPU. (2026-11-07: a
	// D3D9 EVENT query with D3DGETDATA_FLUSH never reported done under the fork here, 15 s, while the helper had finished: it is only a
	// second, short check now.)
	uint64_t drainValue = 0;
	bool Drain(DWORD timeoutMs) {
		if (!dev || !share || !inputsReady) return true;
		const uint64_t target = (std::max)(frame, drainValue) + 1;
		drainValue = target;
		if (FAILED(share->SignalShared(inputsReady, target))) { pgLog("shutdown: SignalShared for the drain failed\n"); return false; }
		const ULONGLONG start = GetTickCount64();
		UINT64 v = 0;
		while (SUCCEEDED(inputsReady->GetCompletedValue(&v)) && v < target && GetTickCount64() - start < timeoutMs) Sleep(1);
		if (v < target) { pgLog("shutdown: the D3D9 queue did not drain in %lu ms (fence at %llu of %llu)\n", timeoutMs, (unsigned long long)v, (unsigned long long)target); return false; }
		IDirect3DQuery9* q = nullptr;   // (the second check, logged only)
		if (SUCCEEDED(dev->CreateQuery(D3DQUERYTYPE_EVENT, &q)) && q) {
			q->Issue(D3DISSUE_END);
			const ULONGLONG qs = GetTickCount64();
			HRESULT hr;
			while ((hr = q->GetData(nullptr, 0, D3DGETDATA_FLUSH)) == S_FALSE && GetTickCount64() - qs < 1000) Sleep(1);
			if (hr != S_OK) pgLog("shutdown: (note) the D3D9 EVENT query still reports busy after the fence drained (0x%08x)\n", unsigned(hr));
			q->Release();
		}
		return true;
	}
	// after Stop and after the caller released its own D3D9 resources
	void Release() {
		for (auto& g : staged) { if (g.vb) g.vb->Release(); if (g.ib) g.ib->Release(); }
		staged.clear();
		for (auto& im : images) if (im.shared) { im.shared->Release(); im.shared = nullptr; }
		images.clear();
		if (inputsReady) { inputsReady->Release(); inputsReady = nullptr; }
		if (outputReady) { outputReady->Release(); outputReady = nullptr; }
		if (rtSeq) { rtSeq->Release(); rtSeq = nullptr; }
		if (rt) { rt->Release(); rt = nullptr; }
		if (share) { share->Release(); share = nullptr; }
		if (ext) { ext->Release(); ext = nullptr; }
		if (dev) { const ULONG left = dev->Release(); dev = nullptr; if (left) pgLog("shutdown: the device still had %lu references\n", left); }
		if (d3d) { d3d->Release(); d3d = nullptr; }
		if (shared) { UnmapViewOfFile(shared); shared = nullptr; }
		if (mapping) { CloseHandle(mapping); mapping = nullptr; }
		if (texStaging) { UnmapViewOfFile(texStaging); texStaging = nullptr; }
		if (texMapping) { CloseHandle(texMapping); texMapping = nullptr; }
		if (frameEvent) { CloseHandle(frameEvent); frameEvent = nullptr; }
		if (statusEvent) { CloseHandle(statusEvent); statusEvent = nullptr; }
	}

	bool HelperAlive() const {
		DWORD code = 0;
		return helper.hProcess && GetExitCodeProcess(helper.hProcess, &code) && code == STILL_ACTIVE;
	}

	// ---- geometry: positions (float3) + texcoords (float2) in a D3D9 VB, a 16-bit list, staged into the arena (RtCore::Gather), BUILD op
	bool Stage(const std::vector<float>& posUv, uint32_t vertexCount, const std::vector<uint16_t>& idx, uint32_t opFlags, uint32_t& id) {
		PgStagedMesh g; g.id = id = nextGeometry++;
		const UINT stride = 20;
		PG_CHECK(dev->CreateVertexBuffer(stride * vertexCount, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &g.vb, nullptr), "vb");
		{ void* v; g.vb->Lock(0, 0, &v, 0); std::memcpy(v, posUv.data(), stride * vertexCount); g.vb->Unlock(); }
		PG_CHECK(dev->CreateIndexBuffer(UINT(idx.size() * 2), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &g.ib, nullptr), "ib");
		{ void* p; g.ib->Lock(0, 0, &p, 0); std::memcpy(p, idx.data(), idx.size() * 2); g.ib->Unlock(); }
		NVR_EXT_RT_RANGE r = { NVR_EXT_RT_TRIANGLE_LIST, 0, UINT(idx.size() / 3), 0 };
		NVR_EXT_RT_GEOMETRY_DESC desc = { g.vb, 0, stride, vertexCount, NVR_EXT_RT_POSITION_FLOAT3, g.ib, 1, &r, NVR_EXT_RT_TEXCOORD_FLAGS(12, false), "playground mesh" };
		NVR_EXT_RT_GEOMETRY_INFO gi = {};
		PG_CHECK(rt->MeasureGeometry(&desc, &gi), "MeasureGeometry");
		if (arenaTexel + gi.Texels + 1 > arenaW * arenaH) { pgLog("  error: arena full (%u texels)\n", arenaTexel + gi.Texels); return false; }
		PG_CHECK(rt->StageGeometry(&desc, images[0].shared, arenaTexel, &gi), "StageGeometry");
		NvrRtGeometryOp op = {};
		op.op = NVR_RT_OP_BUILD; op.id = id; op.vertexCount = gi.VertexCount; op.triangleCount = gi.TriangleCount; op.arenaTexel = arenaTexel;
		op.flags = opFlags;
		ops.push_back(op);
		arenaTexel += gi.Texels;
		staged.push_back(g);
		return true;
	}
	void Release(uint32_t id) { NvrRtGeometryOp op = {}; op.op = NVR_RT_OP_RELEASE; op.id = id; ops.push_back(op); }

	// ---- (protocol 7) real textures: packed levels into the next frame's staging region + an op; a slot's ids
	bool StageTexture(uint32_t id, uint32_t vkFormat, uint32_t w, uint32_t hgt, uint32_t levels, const std::vector<uint8_t>& bytes) {
		const uint32_t region = (kTexStagingMB << 20) / NVR_RT_FRAME_RING;
		const uint32_t at = (texOffset + 15u) & ~15u;
		if (!texStaging || at + bytes.size() > region) { pgLog("  error: texture staging full\n"); return false; }
		std::memcpy(texStaging + (size_t)((frame + 1) % NVR_RT_FRAME_RING) * region + at, bytes.data(), bytes.size());
		texOffset = at + uint32_t(bytes.size());
		NvrRtTextureOp op = { NVR_RT_OP_TEXTURE, id, w, hgt, at, vkFormat, uint32_t(bytes.size()), levels };
		NvrRtGeometryOp g; std::memcpy(&g, &op, sizeof(g)); ops.push_back(g);
		return true;
	}
	void SlotTextures(uint32_t slot, uint32_t diffuse, uint32_t normal, uint32_t glow) {
		NvrRtTextureOp op = { NVR_RT_OP_SLOT_TEXTURES, slot, diffuse, normal, glow, 0, 0, 0 };
		NvrRtGeometryOp g; std::memcpy(&g, &op, sizeof(g)); ops.push_back(g);
	}

	// ---- RtCore::CopyIn / CopyOut: a D3D9 texture / surface into a shared image and back (recorded on the device, no CPU wait)
	bool CopyIn(int image, IUnknown* src, UINT w = 0, UINT h = 0) {
		RECT rc = { 0, 0, LONG(w ? w : images[image].w), LONG(h ? h : images[image].h) };
		return SUCCEEDED(share->CopyToShared(images[image].shared, 0, 0, src, &rc));
	}
	bool CopyOut(int image, IUnknown* dst, UINT w = 0, UINT h = 0) {
		RECT rc = { 0, 0, LONG(w ? w : images[image].w), LONG(h ? h : images[image].h) };
		return SUCCEEDED(share->CopyFromShared(dst, 0, 0, images[image].shared, &rc));
	}

	void AddPass(int shader, const std::vector<int>& imgs, uint32_t writeMask, const void* constants, uint32_t size, uint32_t gx, uint32_t gy) {
		NvrRtPass p; std::memset(&p, 0, sizeof(p));
		p.shader = uint32_t(shader); p.groupsX = gx; p.groupsY = gy; p.groupsZ = 1;
		p.imageCount = uint32_t(imgs.size());
		for (size_t i = 0; i < imgs.size(); i++) {
			if (i < NVR_RT_PASS_IMAGES) p.images[i] = uint32_t(imgs[i]);
			else if (i < NVR_RT_PASS_IMAGES + 4) p.extraImages |= uint32_t(imgs[i] & 0xFF) << (8 * (i - NVR_RT_PASS_IMAGES));
			else p.extraImages2 |= uint32_t(imgs[i] & 0xFF) << (8 * (i - NVR_RT_PASS_IMAGES - 4));
		}
		p.writeMask = writeMask;
		p.constantsSize = size;
		std::memcpy(p.constants, constants, size);
		passes.push_back(p);
	}

	// ---- the exchange (RtCore::Exchange's order): the staged geometry (+ sequence texel), the frame record, SignalShared, WaitShared
	// (a GPU wait: the CPU goes on). Instances in world space; made camera relative here (eye = the scene's origin).
	bool Exchange(const std::vector<PgInstance>& inst, const double eye[3]) {
		const uint64_t n = ++frame;
		for (int i = 0; i < 5000 && shared->consumedFrame + (int64_t)NVR_RT_FRAME_RING < (int64_t)n; i++) Sleep(1);
		if (shared->consumedFrame + (int64_t)NVR_RT_FRAME_RING < (int64_t)n) { pgLog("  error: helper did not read the ring (consumed %lld, frame %llu)\n", (long long)shared->consumedFrame, (unsigned long long)n); return false; }
		if (!HelperAlive()) { pgLog("  error: helper exited: %s\n", shared->message); return false; }
		NvrRtFrame& f = shared->frames[n % NVR_RT_FRAME_RING];
		f.frame = 0;
		MemoryBarrier();
		f.arenaTexels = 0;
		if (arenaTexel) {
			if (rtSeq && arenaTexel < arenaW * arenaH && SUCCEEDED(rtSeq->FlushStagedGeometrySeq(images[0].shared, NvrRtSequenceOf(n), arenaTexel)))
				f.arenaTexels = arenaTexel | NVR_RT_ARENA_SEQUENCE;
			else { PG_CHECK(rt->FlushStagedGeometry(images[0].shared), "FlushStagedGeometry"); f.arenaTexels = arenaTexel; }
		}
		if (ops.size() > NVR_RT_MAX_OPS) { pgLog("  error: too many ops\n"); return false; }
		f.opCount = uint32_t(ops.size());
		for (size_t i = 0; i < ops.size(); i++) f.ops[i] = ops[i];
		uint32_t count = 0;
		for (const PgInstance& o : inst) {
			if (count >= NVR_RT_MAX_INSTANCES) break;
			NvrRtInstance in = {};
			for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) in.transform[r][c] = o.transform[r][c];
			in.transform[0][3] = float(o.transform[0][3] - eye[0]); in.transform[1][3] = float(o.transform[1][3] - eye[1]); in.transform[2][3] = float(o.transform[2][3] - eye[2]);
			in.geometryId = o.geometryId; in.customIndex = o.customIndex; in.mask = o.mask; in.flags = o.flags;
			NvrRtInstanceRecord r = {};
			r.albedo[0] = o.albedo[0]; r.albedo[1] = o.albedo[1]; r.albedo[2] = o.albedo[2]; r.albedo[3] = 1.0f;
			r.flags = o.recordFlags; r.geometryId = o.geometryId;
			f.instances[count] = in; f.records[count] = r; count++;
		}
		f.instanceCount = count;
		f.passCount = uint32_t(passes.size());
		for (size_t i = 0; i < passes.size(); i++) f.passes[i] = passes[i];
		LARGE_INTEGER q; QueryPerformanceCounter(&q); f.writtenQpc = q.QuadPart;
		MemoryBarrier();
		f.frame = n;
		MemoryBarrier();
		InterlockedExchange64((volatile LONG64*)&shared->latestFrame, (LONG64)n);
		SetEvent(frameEvent);
		PG_CHECK(share->SignalShared(inputsReady, n), "SignalShared");
		PG_CHECK(share->WaitShared(outputReady, n), "WaitShared");
		ops.clear(); passes.clear(); arenaTexel = 0; texOffset = 0;
		return true;
	}
	// The staged buffers of a frame can go once the helper is past it (called a few frames later)
	void ReleaseStaged() {
		for (auto& g : staged) { g.vb->Release(); g.ib->Release(); }
		staged.clear();
	}
	bool Finish(DWORD timeoutMs = 20000) {
		UINT64 v = 0;
		const ULONGLONG start = GetTickCount64();
		while (SUCCEEDED(outputReady->GetCompletedValue(&v)) && v < frame) {
			if (GetTickCount64() - start > timeoutMs) { pgLog("  error: output %llu never signalled (at %llu): %s\n", (unsigned long long)frame, (unsigned long long)v, shared->message); return false; }
			Sleep(0);
		}
		return true;
	}
};
