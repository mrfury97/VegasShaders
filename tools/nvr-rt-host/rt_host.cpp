// NvrRtHost: a standalone test host for the Vegas Shaders / LegacyRT ray tracing framework (tools/nvr-rt-host/README.md).
//
// This 32-bit program plays the game's side of the RT core (src/core/RtCore.cpp) without Fallout New Vegas: a D3D9 device through the
// NVR DXVK fork's d3d9.dll next to it, the fork's shared images + timeline semaphores (ID3D9NvrExtShare), geometry staged from D3D9
// vertex / index buffers into the arena (ID3D9NvrExtRt::StageGeometry, with texcoords, + the arena sequence texel when the fork has it),
// instance records with flags, and the real NvrRtHelper.exe (64-bit) as its child running the real trace shaders (Compute/Rt/*.spv).
// Each scene builds a synthetic world, feeds the shaders the constants and images the game's C++ feeds them (mirrored structs below:
// rt_layout_check.py checks they match the shaders' blocks), reads the outputs back through CopyFromShared and checks them against an
// exact CPU reference. Prints PASS / FAIL per scene; exit code 0 only when every scene passed.
//
// Usage: NvrRtHost.exe --helper <NvrRtHelper.exe> --spv <folder of .spv> [--out <folder>] [--device <DXVK device name part>]
//                      [--scene <name>] ... [--keep-going]
// Build: tools/nvr-rt-host/build.ps1 (MSVC x86); run everything: tools/nvr-rt-host/run.ps1.
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "NvrDxvkExt.h"
#include "NvrRtProtocol.h"
#include "RtScenePick.h"     // the game's record / mask packing (src/core)
#include "RtLightGrid.h"     // the game's PTLightGrid builder (src/core)
#include "RtReflDenoise.h"   // (2026-11-07) the RT reflection denoiser (src/core), run by the refldenoise scene
#include <d3dcompiler.h>

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "d3dcompiler.lib")

// ------------------------------------------------------------------------------------------------------------------------------
// small maths
struct V3 { float x = 0, y = 0, z = 0; V3() {} V3(float a, float b, float c) : x(a), y(b), z(c) {} };
static V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
static V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
static V3 operator*(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
static V3 operator-(V3 a) { return { -a.x, -a.y, -a.z }; }
static float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
static float length(V3 a) { return std::sqrt(dot(a, a)); }
static V3 normalize(V3 a) { float l = length(a); return l > 0 ? a * (1.0f / l) : a; }
static void put4(float* d, V3 v, float w) { d[0] = v.x; d[1] = v.y; d[2] = v.z; d[3] = w; }
static void put4(float* d, float a, float b, float c, float e) { d[0] = a; d[1] = b; d[2] = c; d[3] = e; }

static uint16_t toHalf(float f) {
	uint32_t x; std::memcpy(&x, &f, 4);
	const uint32_t s = (x >> 16) & 0x8000u; const int e = int((x >> 23) & 0xff) - 127 + 15; uint32_t m = x & 0x7fffffu;
	if ((x & 0x7fffffffu) == 0) return uint16_t(s);
	if (e <= 0) return uint16_t(s);
	if (e >= 31) return uint16_t(s | 0x7c00u);
	uint32_t h = s | (uint32_t(e) << 10) | (m >> 13);
	if ((m >> 12) & 1u) h++;   // round half up (good enough for test inputs)
	return uint16_t(h);
}
static float fromHalf(uint16_t h) {
	const uint32_t s = (h & 0x8000u) << 16, e = (h >> 10) & 0x1fu, m = h & 0x3ffu;
	uint32_t x;
	if (e == 0) { if (!m) x = s; else { float f = std::ldexp(float(m), -24); return s ? -f : f; } }
	else if (e == 31) x = s | 0x7f800000u | (m << 13);
	else x = s | ((e - 15 + 127) << 23) | (m << 13);
	float f; std::memcpy(&f, &x, 4); return f;
}

// ------------------------------------------------------------------------------------------------------------------------------
// reporting
static FILE* gReport = nullptr;
static void say(const char* fmt, ...) {
	char line[2048]; va_list a; va_start(a, fmt); vsnprintf(line, sizeof(line), fmt, a); va_end(a);
	std::fputs(line, stdout); std::fflush(stdout);
	if (gReport) { std::fputs(line, gReport); std::fflush(gReport); }
}
#define CHECKHR(hr, what) do { HRESULT _hr = (hr); if (FAILED(_hr)) { say("  error: %s -> 0x%08x\n", what, unsigned(_hr)); return false; } } while (0)

// ------------------------------------------------------------------------------------------------------------------------------
// A synthetic world on the CPU: triangles (the same ones staged into the fork) for the exact reference
struct Mesh {
	std::vector<V3> pos;
	std::vector<float> uv;            // 2 per vertex (empty: none)
	std::vector<uint16_t> idx;
	void quad(V3 a, V3 b, V3 c, V3 d) {   // a b c d counter-clockwise; uv (0,0) (1,0) (1,1) (0,1)
		const uint16_t base = uint16_t(pos.size());
		pos.insert(pos.end(), { a, b, c, d });
		const float q[8] = { 0, 0, 1, 0, 1, 1, 0, 1 };
		uv.insert(uv.end(), q, q + 8);
		const uint16_t t[6] = { 0, 1, 2, 0, 2, 3 };
		for (uint16_t k : t) idx.push_back(uint16_t(base + k));
	}
	void box(V3 lo, V3 hi) {
		V3 p[8] = { {lo.x,lo.y,lo.z},{hi.x,lo.y,lo.z},{hi.x,hi.y,lo.z},{lo.x,hi.y,lo.z},{lo.x,lo.y,hi.z},{hi.x,lo.y,hi.z},{hi.x,hi.y,hi.z},{lo.x,hi.y,hi.z} };
		quad(p[0], p[3], p[2], p[1]); quad(p[4], p[5], p[6], p[7]);
		quad(p[0], p[1], p[5], p[4]); quad(p[1], p[2], p[6], p[5]);
		quad(p[2], p[3], p[7], p[6]); quad(p[3], p[0], p[4], p[7]);
	}
};

// An alpha pattern for an alpha-tested record (the alpha atlas slot's 64 x 64 texels)
using AlphaFn = std::function<float(int tx, int ty)>;

struct Object {               // one instance of one mesh
	int mesh;
	V3 offset;                // world translation (the transform is a pure translation)
	bool land = false;        // the record / mask inputs, packed by the game's RtScenePick (src/core/RtScenePick.h)
	bool albedoKnown = true;
	bool glow = false;
	bool textured = false;    // with alphaSlot as its atlas slot
	float rough = -1.0f, metal = -1.0f;   // a material rule (>= 0: bits 26-31)
	int hookMask = -1;
	V3 albedo = { 0.5f, 0.5f, 0.5f };
	bool alphaTested = false;
	bool hidden = false;      // (2026-11-07) custom index NVR_RT_CUSTOM_HIDDEN: the first-person body (camera rays pass over it)
	int alphaSlot = -1;       // atlas slot (bits 16-25)
	uint32_t alphaRef = 128;  // bits 8-15 (D3DCMP_GREATER)
	int id = 0;               // the host's own object id (CPU reference)
	RtScenePick::Record record() const {
		RtScenePick::Record r;
		r.land = land; r.albedoKnown = albedoKnown; r.glowMap = glow; r.alphaTested = alphaTested; r.alphaRef = alphaRef;
		r.textured = textured && !alphaTested; r.slot = alphaSlot; r.materialRule = rough >= 0.0f || metal >= 0.0f; r.rough = rough; r.metal = metal;
		r.hookMask = hookMask;
		return r;
	}
	uint32_t mask() const { return RtScenePick::InstanceMask(record()); }
};

struct World {
	std::vector<Mesh> meshes;
	std::vector<Object> objects;
	std::map<int, AlphaFn> slots;   // alpha atlas slot -> pattern
	std::map<int, int> slotRes;     // (2026-11-06) a slot's pattern resolution for the CPU (default 64 = the atlas'); e.g. 512 = the full texture
	float alphaAt(const Object& o, float u, float v) const {
		auto it = slots.find(o.alphaSlot);
		if (it == slots.end()) return 0.0f;
		const float fu = u - std::floor(u), fv = v - std::floor(v);
		auto rr = slotRes.find(o.alphaSlot);
		const int R = rr == slotRes.end() ? 64 : rr->second;
		int tx = std::min(std::max(int(fu * R), 0), R - 1), ty = std::min(std::max(int(fv * R), 0), R - 1);
		return it->second(tx, ty);
	}
};

struct Hit { bool hit = false; float t = 0; int object = -1; V3 n; float u = 0, v = 0; };

// Closest hit along o + d t, t in (tMin, tMax), honouring alpha tests (as the shaders: alpha > ref / 255 is solid) and the ray mask
static Hit trace(const World& w, V3 o, V3 d, float tMin, float tMax, uint32_t mask, bool testAlpha) {
	Hit best; best.t = tMax;
	for (size_t oi = 0; oi < w.objects.size(); oi++) {
		const Object& ob = w.objects[oi];
		if (!(ob.mask() & mask)) continue;
		if (ob.alphaTested && !testAlpha) continue;   // (rays that do not test alpha leave 0x10 out: the callers' masks do that too)
		const Mesh& m = w.meshes[ob.mesh];
		for (size_t k = 0; k + 2 < m.idx.size(); k += 3) {
			const V3 a = m.pos[m.idx[k]] + ob.offset, b = m.pos[m.idx[k + 1]] + ob.offset, c = m.pos[m.idx[k + 2]] + ob.offset;
			const V3 e1 = b - a, e2 = c - a, pv = cross(d, e2);
			const float det = dot(e1, pv);
			if (std::fabs(det) < 1e-12f) continue;
			const float inv = 1.0f / det;
			const V3 tv = o - a;
			const float bu = dot(tv, pv) * inv;
			if (bu < 0 || bu > 1) continue;
			const V3 qv = cross(tv, e1);
			const float bv = dot(d, qv) * inv;
			if (bv < 0 || bu + bv > 1) continue;
			const float t = dot(e2, qv) * inv;
			if (!(t > tMin && t < best.t)) continue;
			float u = 0, v = 0;
			if (!m.uv.empty()) {
				const float* ua = &m.uv[2 * m.idx[k]]; const float* ub = &m.uv[2 * m.idx[k + 1]]; const float* uc = &m.uv[2 * m.idx[k + 2]];
				u = ua[0] * (1 - bu - bv) + ub[0] * bu + uc[0] * bv;
				v = ua[1] * (1 - bu - bv) + ub[1] * bu + uc[1] * bv;
			}
			if (ob.alphaTested && !(w.alphaAt(ob, u, v) > float(ob.alphaRef) / 255.0f)) continue;
			best.hit = true; best.t = t; best.object = int(oi); best.u = u; best.v = v;
			V3 n = normalize(cross(e1, e2));
			best.n = dot(n, d) > 0 ? -n : n;
		}
	}
	return best;
}

// ------------------------------------------------------------------------------------------------------------------------------
// The camera: scene space = world - eye (RtCore::Eye() = 0), the shaders' convention (forward, right / P00, up / P11; v down)
struct Camera {
	V3 eye, f, r, u;      // unit forward, right, up
	float p00 = 1, p11 = 1, farZ = 20000;
	static Camera lookDir(V3 eye, V3 forward, V3 upHint, float p00, float p11) {
		Camera c; c.eye = eye; c.f = normalize(forward); c.r = normalize(cross(c.f, upHint)); c.u = cross(c.r, c.f); c.p00 = p00; c.p11 = p11; return c;
	}
	// the unnormalized ray through screen uv: a point at view z s is eye + ray * s
	V3 ray(float uvx, float uvy) const { return f + r * ((2 * uvx - 1) / p00) - u * ((2 * uvy - 1) / p11); }
};

// ------------------------------------------------------------------------------------------------------------------------------
// The game's side: the fork device, shared images, the helper
struct Image {
	ID3D9NvrExtSharedImage* shared = nullptr;
	UINT w = 0, h = 0;              // logical size (the shared image is rounded up to 256 multiples, as RtCore::AddImage)
	NVR_EXT_SHARED_FORMAT format = NVR_EXT_SHARED_R32F;
	IDirect3DTexture9* upTex = nullptr;     // CopyIn source (DEFAULT) and its system-memory twin
	IDirect3DTexture9* upSys = nullptr;
	IDirect3DTexture9* readTex = nullptr;   // CopyOut target (a texture: the D3D9 passes read it), its surface and system-memory twin
	IDirect3DSurface9* readRt = nullptr;
	IDirect3DSurface9* readSys = nullptr;
	const char* name = "";
};

static D3DFORMAT d3dFormat(NVR_EXT_SHARED_FORMAT f) {
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
static int channels(NVR_EXT_SHARED_FORMAT f) {
	switch (f) { case NVR_EXT_SHARED_RGBA16F: case NVR_EXT_SHARED_RGBA8: return 4; case NVR_EXT_SHARED_RG16F: case NVR_EXT_SHARED_RG32F: return 2; default: return 1; }
}

struct Geometry { uint32_t id; uint32_t vertices, triangles; IDirect3DVertexBuffer9* vb = nullptr; IDirect3DIndexBuffer9* ib = nullptr; };

struct Host {
	IDirect3D9* d3d = nullptr;
	IDirect3DDevice9* dev = nullptr;
	ID3D9NvrExtShare* share = nullptr;
	ID3D9NvrExtRt* rt = nullptr;
	ID3D9NvrExtRtSeq* rtSeq = nullptr;
	ID3D9NvrExt* ext = nullptr;      // the fork's passes (RtDenoise's)
	ID3D9NvrExtSharedFence* inputsReady = nullptr;
	ID3D9NvrExtSharedFence* outputReady = nullptr;
	std::vector<Image> images;      // index = the protocol's image index (0 = the arena)
	std::vector<std::string> shaders;
	NvrRtShared* shared = nullptr;
	HANDLE mapping = nullptr, frameEvent = nullptr, statusEvent = nullptr;
	PROCESS_INFORMATION helper = {};
	uint64_t frame = 0;
	UINT arenaW = 4096, arenaH = 256;
	// this frame (Begin .. Exchange)
	std::vector<NvrRtGeometryOp> ops;
	uint32_t arenaTexel = 0;
	std::vector<NvrRtPass> passes;
	std::vector<Geometry> staged;   // buffers kept until the frame's exchange
	std::map<uint32_t, Geometry> live;
	uint32_t nextGeometry = 1;

	int AddImage(const char* name, UINT w, UINT h, NVR_EXT_SHARED_FORMAT f) {
		Image im; im.w = w; im.h = h; im.format = f; im.name = name;
		NVR_EXT_SHARED_IMAGE_DESC d = { (w + 255u) & ~255u, (h + 255u) & ~255u, f, name };
		if (FAILED(share->CreateSharedImage(&d, &im.shared))) { say("  error: CreateSharedImage %s\n", name); return -1; }
		images.push_back(im);
		return int(images.size()) - 1;
	}
	int AddShader(const std::string& path) { shaders.push_back(path); return int(shaders.size()) - 1; }

	bool Init(const char* device) {
		if (device && *device) SetEnvironmentVariableA("DXVK_FILTER_DEVICE_NAME", device);
		WNDCLASSA wc = {}; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "nvrrthost";
		RegisterClassA(&wc);
		HWND win = CreateWindowExA(0, "nvrrthost", "NvrRtHost", WS_OVERLAPPEDWINDOW, 0, 0, 64, 48, nullptr, nullptr, wc.hInstance, nullptr);
		d3d = Direct3DCreate9(D3D_SDK_VERSION);
		if (!d3d) { say("  error: Direct3DCreate9 (is the fork's d3d9.dll next to the exe?)\n"); return false; }
		D3DPRESENT_PARAMETERS pp = {};
		pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = win;
		pp.BackBufferWidth = 64; pp.BackBufferHeight = 48; pp.BackBufferFormat = D3DFMT_X8R8G8B8;
		CHECKHR(d3d->CreateDevice(0, D3DDEVTYPE_HAL, win, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev), "CreateDevice");
		CHECKHR(dev->QueryInterface(__uuidof(ID3D9NvrExt), (void**)&ext), "ID3D9NvrExt (not the NVR DXVK fork?)");
		CHECKHR(ext->QueryInterface(__uuidof(ID3D9NvrExtShare), (void**)&share), "ID3D9NvrExtShare");
		CHECKHR(ext->QueryInterface(__uuidof(ID3D9NvrExtRt), (void**)&rt), "ID3D9NvrExtRt");
		if (FAILED(ext->QueryInterface(__uuidof(ID3D9NvrExtRtSeq), (void**)&rtSeq))) rtSeq = nullptr;
		NVR_EXT_SHARE_INFO si = {}; share->GetShareInfo(&si);
		say("device: %s (vendor 0x%04X), sharing %s, arena sequence texel %s\n", si.DeviceName, si.VendorId, si.Supported ? "yes" : "NO",
			rtSeq ? "yes (fork 0034)" : "no");
		if (!si.Supported) return false;
		CHECKHR(share->CreateSharedFence(0, &inputsReady), "inputsReady");
		CHECKHR(share->CreateSharedFence(0, &outputReady), "outputReady");
		if (AddImage("arena", arenaW, arenaH, NVR_EXT_SHARED_R32F) != 0) return false;
		// the shared block (filled in Start)
		wchar_t base[128];
		swprintf(base, 128, L"Local\\NvrRtHost-%lu-%llu", GetCurrentProcessId(), (unsigned long long)GetTickCount64());
		baseName = base;
		mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(NvrRtShared), base);
		shared = mapping ? (NvrRtShared*)MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(NvrRtShared)) : nullptr;
		if (!shared) { say("  error: shared block\n"); return false; }
		std::memset(shared, 0, sizeof(NvrRtShared));
		shared->magic = NVR_RT_MAGIC; shared->version = NVR_RT_PROTOCOL_VERSION; shared->size = sizeof(NvrRtShared);
		shared->parentPid = GetCurrentProcessId();
		shared->scratchMB = 64;   // (2026-11-07, protocol 6) the scratch store: the path tracer's reservoirs + radiance cache at 128 x 128
		shared->vendorId = si.VendorId;
		std::memcpy(shared->deviceLuid, si.DeviceLUID, 8); shared->deviceLuidValid = si.DeviceLUIDValid;
		std::memcpy(shared->deviceUuid, si.DeviceUUID, 16); std::memcpy(shared->driverUuid, si.DriverUUID, 16);
		return true;
	}
	std::wstring baseName;

	static bool giveHandle(HANDLE mine, HANDLE child, uint64_t& out) {
		HANDLE remote = nullptr;
		if (!DuplicateHandle(GetCurrentProcess(), mine, child, &remote, 0, FALSE, DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE)) return false;
		out = (uint64_t)(uintptr_t)remote;
		return true;
	}

	bool Start(const char* helperExe) {
		if (images.size() > NVR_RT_MAX_IMAGES || shaders.size() > NVR_RT_MAX_SHADERS) { say("  error: too many images / shaders\n"); return false; }
		frameEvent = CreateEventW(nullptr, FALSE, FALSE, (baseName + NVR_RT_EVENT_FRAME_SUFFIX).c_str());
		statusEvent = CreateEventW(nullptr, FALSE, FALSE, (baseName + NVR_RT_EVENT_STATUS_SUFFIX).c_str());
		for (size_t i = 0; i < shaders.size(); i++) strncpy_s(shared->shaderPaths[i], shaders[i].c_str(), _TRUNCATE);
		MemoryBarrier();
		shared->shaderCount = uint32_t(shaders.size());
		wchar_t exe[MAX_PATH]; MultiByteToWideChar(CP_ACP, 0, helperExe, -1, exe, MAX_PATH);
		std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --shm " + baseName;
		STARTUPINFOW su = { sizeof(su) };
		if (!CreateProcessW(exe, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &su, &helper)) {
			say("  error: could not start %s (%lu)\n", helperExe, GetLastError()); return false;
		}
		shared->imageCount = uint32_t(images.size());
		for (size_t i = 0; i < images.size(); i++) {
			NVR_EXT_SHARED_IMAGE_INFO in = {}; images[i].shared->GetInfo(&in);
			NvrRtImageDesc& d = shared->images[i];
			d.allocationSize = in.AllocationSize; d.memoryTypeIndex = in.MemoryTypeIndex; d.vkFormat = in.VkFormat; d.width = in.Width; d.height = in.Height;
			d.usage = in.Usage; d.flags = in.Flags; d.tiling = in.Tiling; d.layout = in.Layout; d.handleType = in.HandleType; d.dedicated = in.Dedicated;
			HANDLE h = nullptr;
			CHECKHR(images[i].shared->CreateHandle(&h), "image CreateHandle");
			if (!giveHandle(h, helper.hProcess, d.handle)) { say("  error: DuplicateHandle\n"); return false; }
		}
		HANDLE h1 = nullptr, h2 = nullptr;
		CHECKHR(inputsReady->CreateHandle(&h1), "fence handle"); CHECKHR(outputReady->CreateHandle(&h2), "fence handle");
		if (!giveHandle(h1, helper.hProcess, shared->semInputsReady) || !giveHandle(h2, helper.hProcess, shared->semOutputReady)) return false;
		MemoryBarrier();
		shared->setupDone = 1;
		SetEvent(frameEvent);
		const ULONGLONG start = GetTickCount64();
		while (shared->state == NVR_RT_STATE_STARTING && GetTickCount64() - start < 30000) {
			HANDLE waits[2] = { statusEvent, helper.hProcess };
			if (WaitForMultipleObjects(2, waits, FALSE, 200) == WAIT_OBJECT_0 + 1) break;
		}
		say("helper: state %u after %llu ms: %s\n", shared->state, (unsigned long long)(GetTickCount64() - start), shared->message);
		if (shared->state != NVR_RT_STATE_READY) return false;
		if (shared->shaderFailures) { say("  error: helper shader failures 0x%x (see NvrRtHelper.log)\n", shared->shaderFailures); return false; }
		return true;
	}

	void Stop() {
		if (!shared) return;
		shared->quit = 1;
		if (frameEvent) SetEvent(frameEvent);
		if (helper.hProcess) {
			if (WaitForSingleObject(helper.hProcess, 3000) != WAIT_OBJECT_0) TerminateProcess(helper.hProcess, 1);
			CloseHandle(helper.hProcess); CloseHandle(helper.hThread); helper = {};
		}
	}

	// ---- geometry: a mesh staged into the arena (as RtCore::Gather: float3 positions in a D3D9 VB, 16-bit list, texcoords via
	// NVR_EXT_RT_TEXCOORD_FLAGS(12) like the cards), a BUILD op this frame
	bool Stage(const Mesh& m, bool alpha, uint32_t& id) {
		Geometry g; g.id = id = nextGeometry++;
		const bool uv = !m.uv.empty();
		const UINT stride = uv ? 20 : 12, vcount = UINT(m.pos.size());
		CHECKHR(dev->CreateVertexBuffer(stride * vcount, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &g.vb, nullptr), "vb");
		{ float* v; g.vb->Lock(0, 0, (void**)&v, 0);
		  for (UINT i = 0; i < vcount; i++) { float* o = v + i * (stride / 4); o[0] = m.pos[i].x; o[1] = m.pos[i].y; o[2] = m.pos[i].z;
		    if (uv) { o[3] = m.uv[2 * i]; o[4] = m.uv[2 * i + 1]; } }
		  g.vb->Unlock(); }
		CHECKHR(dev->CreateIndexBuffer(UINT(m.idx.size() * 2), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &g.ib, nullptr), "ib");
		{ void* p; g.ib->Lock(0, 0, &p, 0); std::memcpy(p, m.idx.data(), m.idx.size() * 2); g.ib->Unlock(); }
		NVR_EXT_RT_RANGE r = { NVR_EXT_RT_TRIANGLE_LIST, 0, UINT(m.idx.size() / 3), 0 };
		NVR_EXT_RT_GEOMETRY_DESC desc = { g.vb, 0, stride, vcount, NVR_EXT_RT_POSITION_FLOAT3, g.ib, 1, &r, uv ? NVR_EXT_RT_TEXCOORD_FLAGS(12, false) : 0u, "host mesh" };
		NVR_EXT_RT_GEOMETRY_INFO gi = {};
		CHECKHR(rt->MeasureGeometry(&desc, &gi), "MeasureGeometry");
		if (arenaTexel + gi.Texels + 1 > arenaW * arenaH) { say("  error: arena full\n"); return false; }
		CHECKHR(rt->StageGeometry(&desc, images[0].shared, arenaTexel, &gi), "StageGeometry");
		NvrRtGeometryOp op = {};
		op.op = NVR_RT_OP_BUILD; op.id = id; op.vertexCount = gi.VertexCount; op.triangleCount = gi.TriangleCount; op.arenaTexel = arenaTexel;
		{ RtScenePick::Record pr; pr.alphaTested = alpha; pr.textured = uv && !alpha; op.flags = RtScenePick::OpFlags(pr); }
		ops.push_back(op);
		arenaTexel += gi.Texels;
		g.vertices = gi.VertexCount; g.triangles = gi.TriangleCount;
		staged.push_back(g);
		live[id] = g;
		return true;
	}
	// (2026-11-07) a mesh restaged under an id it already has (an actor's new pose: DYNAMIC, REBUILD every Nth), as RtCore::AddSkinned
	bool StageAs(const Mesh& m, uint32_t id, uint32_t opFlags) {
		const uint32_t keep = nextGeometry;
		uint32_t got = 0;
		nextGeometry = id;
		const bool ok = Stage(m, false, got);
		nextGeometry = keep;
		if (!ok) return false;
		ops.back().flags = opFlags;
		return true;
	}
	void Release(uint32_t id) {
		NvrRtGeometryOp op = {}; op.op = NVR_RT_OP_RELEASE; op.id = id; ops.push_back(op);
		live.erase(id);
	}

	// ---- CopyIn: floats (channels of the format, row-major w x h) into a shared image through a D3D9 texture, as RtCore::CopyIn
	bool Upload(int image, const std::vector<float>& data, UINT x = 0, UINT y = 0, UINT w = 0, UINT h = 0) {
		Image& im = images[image];
		if (!w) { w = im.w; h = im.h; }
		const int ch = channels(im.format);
		const D3DFORMAT fmt = d3dFormat(im.format);
		IDirect3DTexture9 *sys = nullptr, *gpu = nullptr;
		CHECKHR(dev->CreateTexture(w, h, 1, 0, fmt, D3DPOOL_SYSTEMMEM, &sys, nullptr), "upload sys");
		CHECKHR(dev->CreateTexture(w, h, 1, 0, fmt, D3DPOOL_DEFAULT, &gpu, nullptr), "upload gpu");
		D3DLOCKED_RECT lr;
		CHECKHR(sys->LockRect(0, &lr, nullptr, 0), "upload lock");
		for (UINT j = 0; j < h; j++) {
			char* row = (char*)lr.pBits + j * lr.Pitch;
			for (UINT i = 0; i < w; i++) for (int c = 0; c < ch; c++) {
				const float v = data[(size_t(j) * w + i) * ch + c];
				switch (im.format) {
				case NVR_EXT_SHARED_R32F: case NVR_EXT_SHARED_RG32F: ((float*)row)[i * ch + c] = v; break;
				case NVR_EXT_SHARED_R16F: case NVR_EXT_SHARED_RG16F: case NVR_EXT_SHARED_RGBA16F: ((uint16_t*)row)[i * ch + c] = toHalf(v); break;
				case NVR_EXT_SHARED_RGBA8: ((uint8_t*)row)[i * 4 + c] = uint8_t(std::min(std::max(v, 0.0f), 1.0f) * 255.0f + 0.5f); break;
				}
			}
		}
		sys->UnlockRect(0);
		CHECKHR(dev->UpdateTexture(sys, gpu), "UpdateTexture");
		CHECKHR(share->CopyToShared(im.shared, x, y, gpu, nullptr), "CopyToShared");
		uploads.push_back(sys); uploads.push_back(gpu);   // alive until the frame's exchange is submitted
		return true;
	}
	std::vector<IDirect3DTexture9*> uploads;

	// ---- CopyOut + read: the logical w x h of a shared image as floats (call after Exchange)
	bool Read(int image, std::vector<float>& out) {
		Image& im = images[image];
		const D3DFORMAT fmt = d3dFormat(im.format);
		if (!im.readRt) {
			CHECKHR(dev->CreateTexture(im.w, im.h, 1, D3DUSAGE_RENDERTARGET, fmt, D3DPOOL_DEFAULT, &im.readTex, nullptr), "read rt");
			im.readTex->GetSurfaceLevel(0, &im.readRt);
			CHECKHR(dev->CreateOffscreenPlainSurface(im.w, im.h, fmt, D3DPOOL_SYSTEMMEM, &im.readSys, nullptr), "read sys");
		}
		RECT rc = { 0, 0, LONG(im.w), LONG(im.h) };
		CHECKHR(share->CopyFromShared(im.readRt, 0, 0, im.shared, &rc), "CopyFromShared");
		CHECKHR(dev->GetRenderTargetData(im.readRt, im.readSys), "GetRenderTargetData");
		D3DLOCKED_RECT lr;
		CHECKHR(im.readSys->LockRect(&lr, nullptr, D3DLOCK_READONLY), "read lock");
		const int ch = channels(im.format);
		out.assign(size_t(im.w) * im.h * ch, 0.0f);
		for (UINT j = 0; j < im.h; j++) {
			const char* row = (const char*)lr.pBits + j * lr.Pitch;
			for (UINT i = 0; i < im.w; i++) for (int c = 0; c < ch; c++) {
				float v = 0;
				switch (im.format) {
				case NVR_EXT_SHARED_R32F: case NVR_EXT_SHARED_RG32F: v = ((const float*)row)[i * ch + c]; break;
				case NVR_EXT_SHARED_R16F: case NVR_EXT_SHARED_RG16F: case NVR_EXT_SHARED_RGBA16F: v = fromHalf(((const uint16_t*)row)[i * ch + c]); break;
				case NVR_EXT_SHARED_RGBA8: v = ((const uint8_t*)row)[i * 4 + c] / 255.0f; break;
				}
				out[(size_t(j) * im.w + i) * ch + c] = v;
			}
		}
		im.readSys->UnlockRect();
		return true;
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

	// ---- the exchange (RtCore::Exchange's order): flush the staged geometry (+ sequence texel), the frame record, SignalShared,
	// WaitShared. Instances are given in world space; the transform is made camera-relative here (eye = the scene's origin).
	bool Exchange(const World& w, const std::vector<uint32_t>& geometryOf, V3 eye) {
		const uint64_t n = ++frame;
		for (int i = 0; i < 5000 && shared->consumedFrame + (int64_t)NVR_RT_FRAME_RING < (int64_t)n; i++) Sleep(1);
		if (shared->consumedFrame + (int64_t)NVR_RT_FRAME_RING < (int64_t)n) { say("  error: helper did not read the ring (consumed %lld, frame %llu)\n", (long long)shared->consumedFrame, (unsigned long long)n); return false; }
		DWORD code = 0;
		if (GetExitCodeProcess(helper.hProcess, &code) && code != STILL_ACTIVE) { say("  error: helper exited (%lu): %s\n", code, shared->message); return false; }
		NvrRtFrame& f = shared->frames[n % NVR_RT_FRAME_RING];
		f.frame = 0;
		MemoryBarrier();
		f.arenaTexels = 0;
		if (arenaTexel) {
			if (rtSeq && arenaTexel < arenaW * arenaH && SUCCEEDED(rtSeq->FlushStagedGeometrySeq(images[0].shared, NvrRtSequenceOf(n), arenaTexel)))
				f.arenaTexels = arenaTexel | NVR_RT_ARENA_SEQUENCE;
			else { CHECKHR(rt->FlushStagedGeometry(images[0].shared), "FlushStagedGeometry"); f.arenaTexels = arenaTexel; }
		}
		f.opCount = uint32_t(ops.size());
		for (size_t i = 0; i < ops.size(); i++) f.ops[i] = ops[i];
		uint32_t count = 0;
		for (size_t i = 0; i < w.objects.size(); i++) {
			const Object& o = w.objects[i];
			NvrRtInstance in = {};
			in.transform[0][0] = in.transform[1][1] = in.transform[2][2] = 1.0f;
			in.transform[0][3] = o.offset.x - eye.x; in.transform[1][3] = o.offset.y - eye.y; in.transform[2][3] = o.offset.z - eye.z;
			in.geometryId = geometryOf[o.mesh];
			in.customIndex = (o.land ? NVR_RT_CUSTOM_LAND : 0u) | (o.hidden ? NVR_RT_CUSTOM_HIDDEN : 0u);
			in.mask = o.mask();
			in.flags = 0x1u;   // both faces (RtCore)
			NvrRtInstanceRecord r = {};
			r.albedo[0] = o.albedo.x; r.albedo[1] = o.albedo.y; r.albedo[2] = o.albedo.z; r.albedo[3] = 1.0f;
			r.flags = RtScenePick::RecordFlags(o.record());
			r.geometryId = in.geometryId;
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
		CHECKHR(share->SignalShared(inputsReady, n), "SignalShared");
		CHECKHR(share->WaitShared(outputReady, n), "WaitShared");
		ops.clear(); passes.clear(); arenaTexel = 0;
		return true;
	}
	// after the outputs were read: the uploads and staged buffers can go (the GPU is past them: Read waited)
	void EndFrame() {
		dev->Present(nullptr, nullptr, nullptr, nullptr);
		for (auto* t : uploads) t->Release();
		uploads.clear();
		for (auto& g : staged) { g.vb->Release(); g.ib->Release(); }
		staged.clear();
	}
	// wait until the helper finished frame `frame` (a CPU wait, for the host only: the game never does this)
	bool Finish() {
		UINT64 v = 0;
		const ULONGLONG start = GetTickCount64();
		while (SUCCEEDED(outputReady->GetCompletedValue(&v)) && v < frame) {
			if (GetTickCount64() - start > 20000) { say("  error: output %llu never signalled (at %llu): %s\n", (unsigned long long)frame, (unsigned long long)v, shared->message); return false; }
			Sleep(0);
		}
		return true;
	}
};

// ------------------------------------------------------------------------------------------------------------------------------
// The shaders' constant blocks, as the game's C++ fills them. rt_layout_check.py checks these against the .comp blocks AND the game's
// structs (RtLampShadows.h PassConstants, RtPathTrace.cpp PassConstants, Reflections.cpp RtPassConstants).
struct LampConstants {        // RtLampShadows.h PassConstants (RtLampTrace / Split / Exterior / Many / Restir)
	float size[4], forward[4], right[4], up[4], origin[4], params[4], params2[4];
	float flags[4];
	float mask[4];
	float many[4];
	float reproject[4][4];
	float many2[4];
	float jitter[4];
	float rect[4];
};
static_assert(offsetof(LampConstants, many) == 9 * 16 && offsetof(LampConstants, jitter) == 15 * 16 && offsetof(LampConstants, rect) == 16 * 16, "as RtLampShadows.h");
struct PathConstants {        // RtPathTrace.cpp PassConstants
	float size[4], forward[4], right[4], up[4], origin[4], accum[4], sunDir[4], sunColor[4], skyZenith[4], skyHorizon[4], ambient[4], misc[4];
	float lampPos[8][4], lampColor[8][4];
	float rect[4];
	float grid[3][4];
};
static_assert(sizeof(PathConstants) == 512, "RtPathTrace's block is full");
struct ReflConstants {        // Reflections.cpp RtPassConstants
	float size[4], forward[4], right[4], up[4], origin[4], sun[4], layout[4], lamps[24][4];
};

static const UINT S = 128;   // the scenes' screen

static void cameraConstants(const Camera& c, float* size, float* forward, float* right, float* up, float* origin) {
	put4(size, float(S), float(S), 1.0f / S, 1.0f / S);
	put4(forward, c.f, c.farZ);
	put4(right, c.r * (1.0f / c.p00), 0.0f);
	put4(up, c.u * (1.0f / c.p11), 0.0f);
	put4(origin, 0.0f, 0.0f, 0.0f, 0.0f);
}

// The G-buffer the game would hand the lamp passes (TESR_DepthBuffer red = view z / farZ, TESR_NormalsBuffer = view-space normal
// x 0.5 + 0.5), from the CPU world; plus the exact surface per pixel for the references
struct GPixel { bool valid = false; V3 pos, n; float viewZ = 0; int object = -1; float u = 0, v = 0; };
static void gbuffer(const World& w, const Camera& c, std::vector<GPixel>& px, std::vector<float>& depth, std::vector<float>& normals) {
	px.assign(S * S, GPixel()); depth.assign(S * S, 1.0f); normals.assign(S * S * 4, 0.5f);
	for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
		const V3 d = c.ray((i + 0.5f) / S, (j + 0.5f) / S);
		const Hit h = trace(w, c.eye, d, 1e-3f, 1e9f, 0xFF, true);
		GPixel& g = px[j * S + i];
		if (!h.hit) continue;
		g.valid = true; g.viewZ = h.t; g.pos = c.eye + d * h.t; g.n = h.n; g.object = h.object; g.u = h.u; g.v = h.v;
		depth[j * S + i] = h.t / c.farZ;
		float* n = &normals[(j * S + i) * 4];
		n[0] = dot(h.n, c.r) * 0.5f + 0.5f; n[1] = dot(h.n, c.u) * 0.5f + 0.5f; n[2] = dot(h.n, c.f) * 0.5f + 0.5f; n[3] = 1.0f;
	}
}

// ------------------------------------------------------------------------------------------------------------------------------
// Image dumps (PFM, for a failed scene)
static std::string gOut;
static void dumpPfm(const char* name, const std::vector<float>& data, UINT w, UINT h, int ch, int c0 = 0) {
	if (gOut.empty()) return;
	const std::string path = gOut + "\\" + name + ".pfm";
	FILE* f = nullptr; fopen_s(&f, path.c_str(), "wb");
	if (!f) return;
	std::fprintf(f, "Pf\n%u %u\n-1.0\n", w, h);
	for (int j = int(h) - 1; j >= 0; j--) for (UINT i = 0; i < w; i++) { float v = data[(size_t(j) * w + i) * ch + c0]; std::fwrite(&v, 4, 1, f); }
	std::fclose(f);
}

// ------------------------------------------------------------------------------------------------------------------------------
struct Ctx {
	Host* host;
	// images
	int depth, normals, lampResult, lampTable, alphaAtlas, alphaStandIn, histA, histB;
	int ptA, ptB, ptDisplay, aux16, pano;
	int refDir, refRay, refHit, refShade;
	// shaders
	int shPath, shLamp, shMany, shRefl;
};

// Stage every mesh of a world (one frame's BUILD ops); fills the geometry ids
static bool stageWorld(Host& h, const World& w, std::vector<uint32_t>& ids) {
	ids.assign(w.meshes.size(), 0);
	for (size_t m = 0; m < w.meshes.size(); m++) {
		bool alpha = false;
		for (auto& o : w.objects) if (o.mesh == int(m) && o.alphaTested) alpha = true;
		if (!h.Stage(w.meshes[m], alpha, ids[m])) return false;
	}
	return true;
}
static void releaseWorld(Host& h, const std::vector<uint32_t>& ids) { for (uint32_t id : ids) h.Release(id); }

// The alpha atlas: one slot's N x N texels at (slot % 32, slot / 32) x N (RtCore's layout), R16F. N = kAlphaSlot (the shaders'
// [Main.Main.RayTracing] AlphaDetail, read by the shaders off the atlas' size; 2026-11-06: was 64; NVR_RT_HOST_ALPHA_SLOT overrides it). `res`: the pattern's own resolution (a
// 64-texel pattern is repeated texel for texel into a bigger slot).
static int kAlphaSlot = 256;	// the game's default AlphaDetail
static bool uploadSlot(Host& h, int atlas, int slot, const AlphaFn& fn, int res = 64) {
	const int N = kAlphaSlot;
	std::vector<float> t(size_t(N) * N);
	for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) t[size_t(y) * N + x] = fn(x * res / N, y * res / N);
	return h.Upload(atlas, t, UINT(slot % 32) * N, UINT(slot / 32) * N, N, N);
}

// ------------------------------------------------------------------------------------------------------------------------------
// Scene 1: white furnace (RtPathTrace.comp). An open box, albedo 1 inside, under a uniform sky L: every path that escapes carries
// exactly L (albedo 1 everywhere, Russian roulette unbiased), so the image's mean must be L per channel. Then a lone floor of albedo
// 0.5 under the same sky: every pixel exactly 0.5 L (one bounce, all bounce rays escape).
static bool sceneFurnace(Ctx& c) {
	Host& h = *c.host;
	World w;
	Mesh box; const float half = 1000, height = 1500;
	box.quad({ -half, -half, 0 }, { half, -half, 0 }, { half, half, 0 }, { -half, half, 0 });   // floor
	box.quad({ -half, -half, 0 }, { -half, half, 0 }, { -half, half, height }, { -half, -half, height });
	box.quad({ half, -half, 0 }, { half, -half, height }, { half, half, height }, { half, half, 0 });
	box.quad({ -half, -half, 0 }, { -half, -half, height }, { half, -half, height }, { half, -half, 0 });
	box.quad({ -half, half, 0 }, { half, half, 0 }, { half, half, height }, { -half, half, height });
	w.meshes.push_back(box);
	Mesh floor; floor.quad({ -20000, -20000, 0 }, { 20000, -20000, 0 }, { 20000, 20000, 0 }, { -20000, 20000, 0 });
	w.meshes.push_back(floor);
	std::vector<uint32_t> ids;
	if (!stageWorld(h, w, ids)) return false;
	const Camera cam = Camera::lookDir({ 0, 0, 3000 }, { 0, 0, -1 }, { 0, 1, 0 }, 2.2f, 2.2f);
	const V3 L = { 0.8f, 0.6f, 0.4f };
	bool ok = true;
	for (int part = 0; part < 2; part++) {
		Object o; o.mesh = part; o.albedo = part == 0 ? V3(1, 1, 1) : V3(0.5f, 0.5f, 0.5f);
		w.objects = { o };
		PathConstants k = {};
		cameraConstants(cam, k.size, k.forward, k.right, k.up, k.origin);
		k.origin[3] = 17.0f;                       // the frame's seed
		put4(k.accum, 0.0f, part == 0 ? 48.0f : 4.0f, part == 0 ? 16.0f : 2.0f, 0.0f);   // fresh mean, bounces, paths a pixel, no flags
		put4(k.sunDir, 0, 0, 1, 0);               // no sun
		put4(k.skyZenith, L, 1.0f);               // outdoors: the sky on misses
		put4(k.skyHorizon, L, 0.0f);              // PTGlow 0
		put4(k.misc, 0.0f, 0.0f, 60000.0f, 0.0f);   // no lamps, no clamp, ray length (RtPathTrace.cpp), DebugView 0
		k.rect[3] = 1.0f;                         // PTBounceAlpha 1 (the default)
		// images as RtPathTrace.cpp Record: stand-ins where a flag is off
		const std::vector<int> imgs = { c.ptA, c.ptB, c.ptDisplay, c.ptDisplay, c.ptDisplay, c.ptDisplay, c.depth, c.normals, c.aux16, c.pano,
			c.aux16, c.ptDisplay, c.aux16, c.aux16, c.lampTable };
		h.AddPass(c.shPath, imgs, 0x7u, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
		if (!h.Exchange(w, ids, cam.eye)) return false;
		std::vector<float> disp;
		if (!h.Read(c.ptDisplay, disp)) return false;
		h.EndFrame();
		double sum[3] = {}; int nan = 0; float maxDev = 0;
		for (UINT i = 0; i < S * S; i++) for (int ch = 0; ch < 3; ch++) {
			const float v = disp[i * 4 + ch];
			if (!std::isfinite(v)) { nan++; continue; }
			sum[ch] += v;
			const float want = (part == 0 ? 1.0f : 0.5f) * (&L.x)[ch];
			maxDev = std::max(maxDev, std::fabs(v - want) / want);
		}
		if (part == 0) {
			float worst = 0;
			for (int ch = 0; ch < 3; ch++) worst = std::max(worst, float(std::fabs(sum[ch] / (S * S) / (&L.x)[ch] - 1.0)));
			const bool pass = nan == 0 && worst < 0.015f;
			say("  furnace, open box albedo 1, 48 bounces, 16 paths: mean / L = %.4f %.4f %.4f (worst %.2f %%, limit 1.5 %%), NaN %d -> %s\n",
				sum[0] / (S * S) / L.x, sum[1] / (S * S) / L.y, sum[2] / (S * S) / L.z, worst * 100, nan, pass ? "ok" : "BAD");
			if (!pass) { dumpPfm("furnace_box_r", disp, S, S, 4, 0); ok = false; }
		} else {
			const bool pass = nan == 0 && maxDev < 2e-3f;   // (fp16 display: 1e-3 relative)
			say("  furnace, floor albedo 0.5: every pixel 0.5 L, worst pixel off by %.3f %% (limit 0.2 %%), NaN %d -> %s\n", maxDev * 100, nan, pass ? "ok" : "BAD");
			if (!pass) { dumpPfm("furnace_floor_r", disp, S, S, 4, 0); ok = false; }
		}
	}
	releaseWorld(h, ids);
	return ok;
}

// ------------------------------------------------------------------------------------------------------------------------------
// The lamp table (RtLampTrace.comp / Many): lamp s at texels 4s..4s+3 = (x, y), (z, reach), (intensity, game radius), (stop, disc)
struct Lamp { V3 pos; float reach, intensity, radius, stop = 0, disc = 0; };
static std::vector<float> lampTable(const std::vector<Lamp>& lamps, V3 eye) {
	std::vector<float> t(1024 * 2, 0.0f);
	for (size_t s = 0; s < lamps.size(); s++) {
		const Lamp& l = lamps[s]; float* p = &t[s * 8];
		const V3 q = l.pos - eye;
		p[0] = q.x; p[1] = q.y; p[2] = q.z; p[3] = l.reach; p[4] = l.intensity; p[5] = l.radius; p[6] = l.stop; p[7] = l.disc;
	}
	return t;
}

static void lampPassConstants(LampConstants& k, const Camera& cam, int lamps, int perPixel) {
	std::memset(&k, 0, sizeof(k));
	cameraConstants(cam, k.size, k.forward, k.right, k.up, k.origin);
	k.origin[3] = 4095.0f / std::log2(cam.farZ);
	put4(k.params, 1.0f, 0.5f, 0.0f, 0.0f);                       // 1 ray (disc 0: exact), StartOffset 0.5, LampStop 0
	put4(k.params2, 0.0f, float(0x03), float(0x03), float(lamps)); // one segment, masks land + objects (+0x10 when alpha is tested)
	put4(k.flags, 0.0f, float(perPixel), 1.0f, 0.0f);
	put4(k.mask, 0.0f, 0.0f, 0.0f, 0.0f);                          // RankBy 0, StartOffsetPerUnit 0
	put4(k.jitter, 0.0f, 0.0f, 1.0f, 0.0f);                        // no jitter, alpha-tested casters tested
	put4(k.rect, float(S), float(S), 0.0f, 0.0f);
}

// The CPU's version of RtLampTrace.comp's ranking (the light a lamp brings unshadowed) and its visibility ray
static float lampWeightCpu(const Lamp& l, V3 origin, V3 normal) {
	if (!(l.reach > 0)) return 0;
	const V3 to = l.pos - origin;
	const float d2 = dot(to, to);
	if (d2 >= l.reach * l.reach) return 0;
	const float ndl = dot(to, normal);
	if (ndl <= 0) return 0;
	const float falloff = std::min(std::max(1.0f - d2 / std::max(l.radius * l.radius, 1.0f), 0.0f), 1.0f);
	return l.intensity * std::max(falloff, 0.02f) * ndl / std::sqrt(d2);
}
static bool lampLitCpu(const World& w, V3 origin, const Lamp& l, float offset) {
	const V3 to = l.pos - origin;
	const float dist = length(to), stop = l.stop;
	if (!(dist > stop + offset)) return true;
	const float invLen = 1.0f / dist, tEnd = std::min(std::max(1.0f - stop * invLen, 0.0f), 1.0f), tMin = std::min(offset * invLen, tEnd);
	const Hit hh = trace(w, origin, to, tMin, tEnd, 0x13, true);
	return !hh.hit;
}

// A pixel's expected lamp entries: up to 3 lamps by weight (ties within 0.1 % = ambiguous), their visibility. `stable` = the same
// answer for the surface points around the pixel's centre (a pixel on a shadow / alpha / geometry edge is not checked).
struct LampExpect { bool valid = false, stable = true; int slot[3] = { -1, -1, -1 }; int lit[3] = { 0, 0, 0 }; float viewZ = 0; };
static LampExpect expectLamps(const World& w, const std::vector<Lamp>& lamps, const Camera& cam, UINT i, UINT j, int perPixel, float offset) {
	LampExpect e;
	const float du[5] = { 0, -0.3f, 0.3f, 0, 0 }, dv[5] = { 0, 0, 0, -0.3f, 0.3f };
	for (int s = 0; s < 5; s++) {
		const V3 d = cam.ray((i + 0.5f + du[s]) / S, (j + 0.5f + dv[s]) / S);
		const Hit hh = trace(w, cam.eye, d, 1e-3f, 1e9f, 0xFF, true);
		if (s == 0) { e.valid = hh.hit; e.viewZ = hh.t; }
		if (hh.hit != e.valid) { e.stable = false; return e; }
		if (!hh.hit) return e;
		const V3 origin = cam.eye + d * hh.t + hh.n * offset;
		int best[3] = { -1, -1, -1 }; float wt[3] = { 0, 0, 0 };
		std::vector<float> all(lamps.size());
		for (size_t k = 0; k < lamps.size(); k++) {
			const float v = lampWeightCpu(lamps[k], origin, hh.n); all[k] = v;
			if (!(v > 0)) continue;
			if (v > wt[2]) {
				if (v > wt[1]) { wt[2] = wt[1]; best[2] = best[1]; if (v > wt[0]) { wt[1] = wt[0]; best[1] = best[0]; wt[0] = v; best[0] = int(k); } else { wt[1] = v; best[1] = int(k); } }
				else { wt[2] = v; best[2] = int(k); }
			}
		}
		// a near tie between the kept ones (or with the first left out) can go either way on the GPU
		for (int a = 0; a < perPixel; a++) for (size_t k = 0; k < lamps.size(); k++)
			if (best[a] >= 0 && int(k) != best[a] && all[k] > 0 && std::fabs(all[k] - wt[a]) < 1e-3f * wt[a]) e.stable = false;
		for (int a = 0; a < 3; a++) {
			const int slot = a < perPixel ? best[a] : -1;
			const int lit = slot >= 0 ? (lampLitCpu(w, origin, lamps[slot], offset) ? 15 : 0) : 0;
			if (s == 0) { e.slot[a] = slot; e.lit[a] = lit; }
			else if (slot != e.slot[a] || lit != e.lit[a]) e.stable = false;
		}
	}
	return e;
}

// Run RtLampTrace once on a world + lamps, compare every stable pixel's entries with the CPU's. Returns mismatches.
static bool runLampTrace(Ctx& c, const World& w, const std::vector<uint32_t>& ids, const std::vector<Lamp>& lamps, const Camera& cam,
	int perPixel, const char* label, int& checked, int& bad, int* highSlots, std::vector<float>* resultOut) {
	Host& h = *c.host;
	std::vector<GPixel> px; std::vector<float> depth, normals;
	gbuffer(w, cam, px, depth, normals);
	if (!h.Upload(c.depth, depth) || !h.Upload(c.normals, normals) || !h.Upload(c.lampTable, lampTable(lamps, cam.eye), 0, 0, 1024, 1)) return false;
	LampConstants k; lampPassConstants(k, cam, int(lamps.size()), perPixel);
	// images as RtLampShadows.h Record (no split, no filter): [4]-[7] unused, [8] the alpha atlas (binding 12)
	const std::vector<int> imgs = { c.depth, c.normals, c.lampResult, c.lampTable, c.depth, c.depth, c.depth, c.depth, c.alphaAtlas };
	h.AddPass(c.shLamp, imgs, 0x4, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
	if (!h.Exchange(w, ids, cam.eye)) return false;
	std::vector<float> res;
	if (!h.Read(c.lampResult, res)) return false;
	h.EndFrame();
	if (resultOut) *resultOut = res;
	checked = bad = 0;
	int shown = 0;
	for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
		const LampExpect e = expectLamps(w, lamps, cam, i, j, perPixel, 0.5f);
		if (!e.stable) continue;
		const float r = res[(j * S + i) * 2], g = res[(j * S + i) * 2 + 1];
		const int entry[3] = { int(r) % 1024, int(r) / 1024, int(g) % 1024 };
		const int code = int(g) / 1024;
		bool ok = true;
		if (!e.valid) ok = entry[0] == 1023 && entry[1] == 1023 && entry[2] == 1023 && code == 0;
		else {
			for (int a = 0; a < 3; a++) {
				const int want = e.slot[a] >= 0 ? e.slot[a] + 64 * e.lit[a] : 1023;
				if (entry[a] != want) ok = false;
				if (highSlots && e.slot[a] >= 32 && entry[a] == want) (*highSlots)++;
			}
			const int wantCode = int(std::min(std::max(std::floor(std::log2(e.viewZ) * k.origin[3] + 0.5f), 1.0f), 4095.0f));
			if (std::abs(code - wantCode) > 1) ok = false;
		}
		checked++;
		if (!ok) {
			bad++;
			if (shown++ < 5) say("    %s pixel (%u, %u): got entries %d %d %d code %d; expected slots %d %d %d lit %d %d %d\n", label, i, j,
				entry[0], entry[1], entry[2], code, e.slot[0], e.slot[1], e.slot[2], e.lit[0], e.lit[1], e.lit[2]);
		}
	}
	return true;
}

// Scenes 2 + 3: a lamp behind an alpha-tested card (RtLampTrace.comp + RtAlpha.glsl). The card's atlas slot holds a checker; the
// traced lamp visibility on the floor must be the checker's shadow, texel for texel. Slot 37 (the atlas' second row). Scene 3 is the
// same card whose record also carries a material rule in bits 26-31 (the 2026-11-06 bug: an unmasked slot read alpha 0, every ray
// passed the card): the result must equal scene 2's bit for bit.
static World cardWorld(bool materialBits) {
	World w;
	Mesh floor; floor.quad({ -4000, -4000, 0 }, { 4000, -4000, 0 }, { 4000, 4000, 0 }, { -4000, 4000, 0 });
	Mesh card; card.quad({ -500, -500, 300 }, { 500, -500, 300 }, { 500, 500, 300 }, { -500, 500, 300 });
	w.meshes = { floor, card };
	Object f; f.mesh = 0; f.id = 1;
	Object k; k.mesh = 1; k.id = 2; k.alphaTested = true; k.alphaSlot = 37; k.alphaRef = 128;
	if (materialBits) { k.rough = 1.0f; k.metal = 0.67f; }   // packed as rough 7, metal 5 (bits 26-31)
	w.objects = { f, k };
	w.slots[37] = [](int x, int y) { return ((x / 8 + y / 8) & 1) ? 1.0f : 0.0f; };
	return w;
}

static std::vector<float> gCardResult;
static bool sceneAlphaCard(Ctx& c, bool materialBits) {
	Host& h = *c.host;
	World w = cardWorld(materialBits);
	if (!uploadSlot(h, c.alphaAtlas, 37, w.slots[37])) return false;
	std::vector<uint32_t> ids;
	if (!stageWorld(h, w, ids)) return false;
	const std::vector<Lamp> lamps = { { V3(0, 0, 900), 0, 0, 0 }, { V3(0, 0, 900), 0, 0, 0 }, { V3(0, 0, 900), 0, 0, 0 },
		{ V3(0, 0, 900), 0, 0, 0 }, { V3(0, 0, 900), 0, 0, 0 }, { V3(60, -40, 900), 6000, 1.0f, 6000 } };   // slot 5 (0-4 empty: reach 0)
	const Camera cam = Camera::lookDir({ 0, 0, 2500 }, { 0, 0, -1 }, { 0, 1, 0 }, 2.0f, 2.0f);
	int checked = 0, bad = 0;
	std::vector<float> res;
	if (!runLampTrace(c, w, ids, lamps, cam, 1, materialBits ? "material bits" : "alpha card", checked, bad, nullptr, &res)) return false;
	// how much of the floor is in the card's shadow (the test must see both)
	int shadowed = 0, lit = 0;
	for (UINT i = 0; i < S * S; i++) { const int e0 = int(res[i * 2]) % 1024; if (e0 == 5) shadowed++; else if (e0 == 5 + 64 * 15) lit++; }
	bool ok = bad == 0 && checked > int(S * S) / 2 && shadowed > 500 && lit > 500;
	say("  %s: %d pixels checked (%d on edges skipped), %d mismatches; %d shadowed + %d lit by slot 5%s\n", materialBits ? "card with material rule bits 26-31" : "alpha card (slot 37, checker)",
		checked, int(S * S) - checked, bad, shadowed, lit, materialBits ? "" : "");
	if (materialBits) {
		int diff = 0;
		if (gCardResult.size() == res.size()) for (size_t i = 0; i < res.size(); i++) diff += res[i] != gCardResult[i];
		else diff = -1;
		say("  vs the card without material bits: %d texels differ%s\n", diff, diff < 0 ? " (that scene did not run)" : "");
		if (diff != 0) ok = false;
	} else gCardResult = res;
	if (!ok) dumpPfm(materialBits ? "card_material_result" : "card_result", res, S, S, 2, 0);
	releaseWorld(h, ids);
	return ok;
}

// Scene 4: more than 32 lamps (RtLampTrace.comp): 48 lamps over a floor, small blocks casting shadows next to lamps 3, 33, 40, 47.
// Every pixel's three traced entries (slot ids up to 47 in the 6-bit field, visibility) must match the CPU's ranking + rays.
static World lampFloorWorld(std::vector<Lamp>& lamps, int nx, int ny, float spacing, float height, float reach, const std::vector<int>& blocked) {
	World w;
	const float hx = nx * spacing * 0.5f, hy = ny * spacing * 0.5f;
	Mesh floor; floor.quad({ -hx - 2000, -hy - 2000, 0 }, { hx + 2000, -hy - 2000, 0 }, { hx + 2000, hy + 2000, 0 }, { -hx - 2000, hy + 2000, 0 });
	Mesh block; block.box({ -60, -60, 0 }, { 60, 60, 140 });
	w.meshes = { floor, block };
	Object f; f.mesh = 0; f.land = true; w.objects.push_back(f);   // as land
	lamps.clear();
	for (int y = 0; y < ny; y++) for (int x = 0; x < nx; x++) {
		const int s = y * nx + x;
		const V3 p = { -hx + (x + 0.5f) * spacing, -hy + (y + 0.5f) * spacing, height };
		lamps.push_back({ p, reach, 1.0f + 0.013f * s, reach });
		if (std::find(blocked.begin(), blocked.end(), s) != blocked.end()) { Object b; b.mesh = 1; b.offset = { p.x + 150, p.y + 90, 0 }; w.objects.push_back(b); }
	}
	return w;
}

static bool sceneManyLamps(Ctx& c) {
	Host& h = *c.host;
	std::vector<Lamp> lamps;
	World w = lampFloorWorld(lamps, 8, 6, 800, 220, 900, { 3, 33, 40, 47 });
	std::vector<uint32_t> ids;
	if (!stageWorld(h, w, ids)) return false;
	const Camera cam = Camera::lookDir({ 0, 0, 5000 }, { 0, 0, -1 }, { 0, 1, 0 }, 1.45f, 1.45f);
	int checked = 0, bad = 0, high = 0;
	std::vector<float> res;
	if (!runLampTrace(c, w, ids, lamps, cam, 3, "48 lamps", checked, bad, &high, &res)) return false;
	int shadowedHigh = 0;
	for (UINT i = 0; i < S * S; i++) { const int e0 = int(res[i * 2]) % 1024; if (e0 % 64 >= 32 && e0 / 64 == 0) shadowedHigh++; }
	const bool ok = bad == 0 && checked > int(S * S) / 2 && high > 1000 && shadowedHigh > 10;
	say("  48 lamps, 3 a pixel: %d pixels checked, %d mismatches; %d correct entries name slots 32-47, %d pixels shadowed from a slot >= 32\n",
		checked, bad, high, shadowedHigh);
	if (!ok) dumpPfm("manylamps_result", res, S, S, 2, 0);
	releaseWorld(h, ids);
	return ok;
}

// ------------------------------------------------------------------------------------------------------------------------------
// Scene 5: RT reflections (ReflectionsRT.comp): a mirror floor, a red block and a green block. RayGen's inputs come from the CPU
// (the reflected direction octahedral, view z + roughness 0 + 2); the hit distance, the hit's albedo + normal and its sun visibility
// must be the CPU's.
static void octEncode(V3 v, float& ex, float& ey) {
	v = v * (1.0f / (std::fabs(v.x) + std::fabs(v.y) + std::fabs(v.z)));
	ex = v.x; ey = v.y;
	if (v.z < 0) { const float x = ex, y = ey; ex = (1 - std::fabs(y)) * (x >= 0 ? 1.0f : -1.0f); ey = (1 - std::fabs(x)) * (y >= 0 ? 1.0f : -1.0f); }
}
static V3 octDecode(float ex, float ey) {
	V3 v = { ex, ey, 1 - std::fabs(ex) - std::fabs(ey) };
	if (v.z < 0) { const float x = v.x, y = v.y; v.x = (1 - std::fabs(y)) * (x >= 0 ? 1.0f : -1.0f); v.y = (1 - std::fabs(x)) * (y >= 0 ? 1.0f : -1.0f); }
	return normalize(v);
}

static bool sceneMirror(Ctx& c) {
	Host& h = *c.host;
	World w;
	Mesh floor; floor.quad({ -3000, -3000, 0 }, { 3000, -3000, 0 }, { 3000, 3000, 0 }, { -3000, 3000, 0 });
	Mesh block; block.box({ -320, -320, 0 }, { 320, 320, 1100 });
	Mesh plate; plate.quad({ -700, 60, 700 }, { 0, 60, 700 }, { 0, 380, 700 }, { -700, 380, 700 });   // shades the red block's front
	w.meshes = { floor, block, plate };
	Object f; f.mesh = 0; f.albedo = { 0.5f, 0.5f, 0.5f }; f.id = 1;
	Object red; red.mesh = 1; red.offset = { -350, 700, 0 }; red.albedo = { 0.8f, 0.1f, 0.1f }; red.id = 2;
	Object green; green.mesh = 1; green.offset = { 520, 1100, 0 }; green.albedo = { 0.1f, 0.7f, 0.2f }; green.id = 3;
	Object cover; cover.mesh = 2; cover.albedo = { 0.2f, 0.2f, 0.9f }; cover.id = 4;
	w.objects = { f, red, green, cover };
	std::vector<uint32_t> ids;
	if (!stageWorld(h, w, ids)) return false;
	const Camera cam = Camera::lookDir({ 0, -1600, 700 }, normalize({ 0, 1, -0.45f }), { 0, 0, 1 }, 1.3f, 1.3f);
	const V3 sun = normalize({ 0.25f, -0.35f, 0.9f });
	// RayGen's targets: where the camera sees the floor
	std::vector<float> dir(S * S * 2, 0.0f), ray(S * S * 2, 0.0f);
	std::vector<GPixel> px(S * S);
	for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
		const V3 d = cam.ray((i + 0.5f) / S, (j + 0.5f) / S);
		const Hit hh = trace(w, cam.eye, d, 1e-3f, 1e9f, 0xFF, true);
		if (!hh.hit || hh.object != 0) continue;
		const V3 v = normalize(d), r = v - hh.n * (2 * dot(v, hh.n));
		octEncode(r, dir[(j * S + i) * 2], dir[(j * S + i) * 2 + 1]);
		ray[(j * S + i) * 2] = hh.t; ray[(j * S + i) * 2 + 1] = 2.0f;   // view z, roughness 0 + 2 (cast)
		GPixel& g = px[j * S + i]; g.valid = true; g.viewZ = hh.t; g.pos = cam.eye + d * hh.t; g.n = hh.n;
	}
	if (!h.Upload(c.refDir, dir) || !h.Upload(c.refRay, ray)) return false;
	ReflConstants k = {};
	cameraConstants(cam, k.size, k.forward, k.right, k.up, k.origin);
	k.size[2] = 10000.0f; k.size[3] = 1.0f;     // Distance, sun shadow rays
	k.forward[3] = 0.0f;                       // no lamps, RoughReach 0
	put4(k.sun, sun, 1.0f);
	put4(k.layout, 1.0f / S, 1.0f / S, 1.0f / S, 1.0f / S);   // (full resolution here: RayGen's texel = the screen's)
	h.AddPass(c.shRefl, { c.refDir, c.refRay, c.refHit, c.refShade }, 0xC, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
	if (!h.Exchange(w, ids, cam.eye)) return false;
	std::vector<float> hit, shade;
	if (!h.Read(c.refHit, hit) || !h.Read(c.refShade, shade)) return false;
	h.EndFrame();
	int checked = 0, bad = 0, hits = 0, misses = 0, shadowed = 0, shown = 0, notCast = 0;
	for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
		const float* H = &hit[(j * S + i) * 4]; const float* C = &shade[(j * S + i) * 4];
		const GPixel& g = px[j * S + i];
		if (!g.valid) { if (H[0] != -1.0f) { bad++; if (shown++ < 5) say("    pixel (%u, %u): no ray asked, hit.x %.3f (want -1)\n", i, j, H[0]); } notCast++; continue; }
		// the start as the shader makes it, then the CPU's hit (and its neighbourhood: edges are not checked)
		bool stable = true; Hit ref; V3 start, rdir;
		for (int s = 0; s < 3; s++) {
			const float off = s == 0 ? 0.0f : s == 1 ? -0.25f : 0.25f;
			const V3 d = cam.ray((i + 0.5f + off) / S, (j + 0.5f + off) / S);
			const Hit fh = trace(w, cam.eye, d, 1e-3f, 1e9f, 0xFF, true);
			if (!fh.hit || fh.object != 0) { stable = false; break; }
			const V3 surface = d * fh.t;   // scene space (eye 0)
			const V3 v = normalize(d), r = v - fh.n * (2 * dot(v, fh.n));
			const V3 half = r - normalize(surface);
			const V3 st = surface + half * ((0.5f + fh.t * 0.002f) / length(half));
			const Hit rh = trace(w, cam.eye + st, r, 1.0f + fh.t * 0.0015f, 10000.0f, 0xEF, false);
			if (s == 0) { ref = rh; start = st; rdir = r; }
			else if (rh.hit != ref.hit || rh.object != ref.object) stable = false;
		}
		if (!stable) continue;
		checked++;
		bool ok;
		if (!ref.hit) { ok = H[0] == 0.0f; misses++; }
		else {
			hits++;
			const Object& o = w.objects[ref.object];
			const V3 n = octDecode(H[2], H[3]);
			// the sun at the hit (the shader: off the hit along its normal, 0.5 .. 30000)
			const V3 at = cam.eye + start + rdir * ref.t;
			float sunVis = 1.0f;
			if (dot(ref.n, sun) > 0) sunVis = trace(w, at + ref.n * std::max(1.0f, ref.t * 0.001f), sun, 0.5f, 30000.0f, 0xEF, false).hit ? 0.0f : 1.0f;
			if (sunVis == 0.0f) shadowed++;
			ok = std::fabs(H[0] - ref.t) <= 0.002f * ref.t + 1.0f && (int(H[1]) & 6) == 6 && dot(n, ref.n) > 0.995f &&
				std::fabs(C[0] - o.albedo.x) < 4e-3f && std::fabs(C[1] - o.albedo.y) < 4e-3f && std::fabs(C[2] - o.albedo.z) < 4e-3f && C[3] == sunVis;
			if (!ok && shown++ < 5) say("    pixel (%u, %u): got t %.2f flags %d n (%.2f %.2f %.2f) albedo (%.2f %.2f %.2f) sun %.0f; expected t %.2f object %d n (%.2f %.2f %.2f) sun %.0f\n",
				i, j, H[0], int(H[1]), n.x, n.y, n.z, C[0], C[1], C[2], C[3], ref.t, ref.object, ref.n.x, ref.n.y, ref.n.z, sunVis);
		}
		if (!ok) bad++;
	}
	const bool ok = bad == 0 && hits > 1000 && misses > 200 && shadowed > 20;
	say("  mirror floor: %d rays checked (%d hit a block: %d of them in shadow, %d missed), %d pixels with no ray, %d mismatches\n", checked, hits, shadowed, misses, notCast, bad);
	if (!ok) { dumpPfm("mirror_hit_t", hit, S, S, 4, 0); dumpPfm("mirror_shade_r", shade, S, S, 4, 0); }
	releaseWorld(h, ids);
	return ok;
}

// ------------------------------------------------------------------------------------------------------------------------------
// Scene 6: a camera move with history (RtLampTraceMany.comp: ManyLights' rest estimate R = sum(w V) / sum(w) over every lamp but a
// pixel's two strongest, reservoirs + a temporal history in an image pair, reprojected into the previous trace). 40 lamps over a
// floor with blocks. 40 frames at camera A, then the camera slides sideways (the same height: the history's depth test passes) and
// one frame traces with the history. With the right reprojection R stays as close to the CPU's exact R as before; with a stale one
// (the previous camera given as the current) the shadows of the rest ghost by the slide. Also every pixel's two exact entries.
static bool sceneCameraMove(Ctx& c) {
	Host& h = *c.host;
	std::vector<Lamp> lamps;
	World w = lampFloorWorld(lamps, 8, 5, 520, 260, 1400, { 2, 9, 17, 22, 33, 36, 39 });
	std::vector<uint32_t> ids;
	if (!stageWorld(h, w, ids)) return false;
	const Camera camA = Camera::lookDir({ 0, 0, 3400 }, { 0, 0, -1 }, { 0, 1, 0 }, 1.6f, 1.6f);
	Camera camB = camA; camB.eye = camA.eye + V3(260, 0, 0);
	const float offset = 0.5f;

	// the CPU's exact R and the two strongest per pixel
	struct Ref { bool valid = false, hasRest = false, stable = true; float R = 1; int best[2] = { -1, -1 }; int lit[2] = { 0, 0 }; };
	auto reference = [&](const Camera& cam, std::vector<Ref>& out) {
		out.assign(S * S, Ref());
		for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
			Ref& r = out[j * S + i];
			const V3 d = cam.ray((i + 0.5f) / S, (j + 0.5f) / S);
			const Hit hh = trace(w, cam.eye, d, 1e-3f, 1e9f, 0xFF, true);
			if (!hh.hit) continue;
			r.valid = true;
			const V3 origin = cam.eye + d * hh.t + hh.n * offset;
			std::vector<float> wt(lamps.size());
			int b0 = -1, b1 = -1; float w0 = 0, w1 = 0;
			for (size_t s = 0; s < lamps.size(); s++) {
				wt[s] = lampWeightCpu(lamps[s], origin, hh.n);
				if (!(wt[s] > 0)) continue;
				if (wt[s] > w1) { if (wt[s] > w0) { b1 = b0; w1 = w0; b0 = int(s); w0 = wt[s]; } else { b1 = int(s); w1 = wt[s]; } }
			}
			double sum = 0, vis = 0;
			for (size_t s = 0; s < lamps.size(); s++) {
				if (!(wt[s] > 0) || int(s) == b0 || int(s) == b1) continue;
				if (std::fabs(wt[s] - w1) < 1e-3f * w1) r.stable = false;
				sum += wt[s];
				vis += wt[s] * (lampLitCpu(w, origin, lamps[s], offset) ? 1.0 : 0.0);
			}
			if (b1 >= 0 && std::fabs(w0 - w1) < 1e-3f * w0) r.stable = false;
			r.hasRest = sum > 0; r.R = sum > 0 ? float(vis / sum) : 1.0f;
			r.best[0] = b0; r.best[1] = b1;
			for (int a = 0; a < 2; a++) r.lit[a] = r.best[a] >= 0 && lampLitCpu(w, origin, lamps[r.best[a]], offset) ? 15 : 0;
		}
	};
	std::vector<Ref> refA, refB;
	reference(camA, refA); reference(camB, refB);

	// the previous trace's clip space for a scene-space point of THIS frame (eye = cur): x = P00 (R.(p - prevEye)), y = P11 (U.(p - prevEye)), w = view z
	auto reproject = [&](LampConstants& k, const Camera& prev, const Camera& cur) {
		const V3 pe = prev.eye - cur.eye;
		const V3 ax = prev.r * prev.p00, ay = prev.u * prev.p11, aw = prev.f;
		put4(k.reproject[0], ax.x, ay.x, 0.0f, aw.x);
		put4(k.reproject[1], ax.y, ay.y, 0.0f, aw.y);
		put4(k.reproject[2], ax.z, ay.z, 0.0f, aw.z);
		put4(k.reproject[3], -dot(ax, pe), -dot(ay, pe), 0.0f, -dot(aw, pe));
	};
	int parity = 0;
	uint32_t frameNo = 0;
	std::vector<GPixel> px; std::vector<float> depthA, normalsA, depthB, normalsB;
	gbuffer(w, camA, px, depthA, normalsA); gbuffer(w, camB, px, depthB, normalsB);
	auto trace1 = [&](const Camera& cam, const Camera& prev, bool history, std::vector<float>* result) -> bool {
		const bool atA = cam.eye.x == camA.eye.x;
		if (!h.Upload(c.depth, atA ? depthA : depthB) || !h.Upload(c.normals, atA ? normalsA : normalsB) ||
			!h.Upload(c.lampTable, lampTable(lamps, cam.eye), 0, 0, 1024, 1)) return false;
		LampConstants k; lampPassConstants(k, cam, int(lamps.size()), 2);
		put4(k.many, 4.0f, 64.0f, 0.0f, history ? 1.0f : 0.0f);         // 4 reservoirs, 64 history frames, no spatial reuse, history valid
		put4(k.many2, float(frameNo++), 0.05f, 1.0f, 0.0f);              // frame (seed), depth tolerance, rest on every pixel
		reproject(k, prev, cam);
		const std::vector<int> imgs = { c.depth, c.normals, c.lampResult, c.lampTable, parity ? c.histB : c.histA, parity ? c.histA : c.histB, c.depth, c.depth, c.alphaAtlas };
		h.AddPass(c.shMany, imgs, 0x24, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
		parity = 1 - parity;
		if (!h.Exchange(w, ids, cam.eye)) return false;
		if (result && !h.Read(c.lampResult, *result)) return false;
		if (!result && !h.Finish()) return false;
		h.EndFrame();
		return true;
	};
	// mean |R - R exact| over the pixels with a rest; exact entries checked too
	auto score = [&](const std::vector<float>& res, const std::vector<Ref>& ref, int& n, int& badEntries, int& high) -> double {
		double err = 0; n = 0; badEntries = 0;
		for (UINT p = 0; p < S * S; p++) {
			const Ref& r = ref[p];
			if (!r.valid || !r.stable) continue;
			const float x = res[p * 2], y = res[p * 2 + 1];
			const int e0 = int(x) % 4096, e1 = int(x) / 4096, restCode = int(y) % 4096;
			for (int a = 0; a < 2; a++) {
				const int want = r.best[a] >= 0 ? r.best[a] + 256 * r.lit[a] : 4095, got = a ? e1 : e0;
				if (got != want) badEntries++;
				else if (r.best[a] >= 32) high++;
			}
			if (!r.hasRest) continue;
			err += std::fabs(restCode / 4095.0 - r.R); n++;
		}
		return n ? err / n : 1.0;
	};
	bool ok = true;
	double errA = 0, errB = 0, errStale = 0;
	int nA = 0, nB = 0, nS = 0, badA = 0, badB = 0, badS = 0, high = 0;
	for (int run = 0; run < 2; run++) {
		std::vector<float> resA, resB;
		for (int f = 0; f < 40; f++) if (!trace1(camA, camA, f > 0, f == 39 ? &resA : nullptr)) return false;
		if (!trace1(camB, run == 0 ? camA : camB, true, &resB)) return false;
		if (run == 0) {
			errA = score(resA, refA, nA, badA, high);
			errB = score(resB, refB, nB, badB, high);
			if (errB > 0.05) dumpPfm("cameramove_result_b", resB, S, S, 2, 1);
		} else {
			errStale = score(resB, refB, nS, badS, high);
			if (errStale < 2.0 * errB) dumpPfm("cameramove_result_stale", resB, S, S, 2, 1);
		}
	}
	ok = nA > int(S * S) / 2 && errA < 0.03 && errB < std::max(1.5 * errA, errA + 0.01) && errStale > 2.0 * errB && badA == 0 && badB == 0 && high > 500;
	say("  camera move (40 lamps, ManyLights, 64-frame history): rest R off the exact mean by %.4f at A after 40 frames, %.4f right after the slide "
		"(limit max(1.5 x A, A + 0.01) and A < 0.03); a stale reprojection gives %.4f (must be > 2 x the right one); exact entries wrong: %d at A, %d at B "
		"(%d right ones name slots 32-39)\n", errA, errB, errStale, badA, badB, high);
	releaseWorld(h, ids);
	// one empty frame so the releases reach the helper
	if (!h.Exchange(World(), {}, V3()) || !h.Finish()) return false;
	h.EndFrame();
	return ok;
}

#include "rt_host_more.inl"
#include "rt_host_alpha.inl"
#include "rt_host_sameframe.inl"	// (2026-11-07) SameFrame lamp shadows
#include "rt_host_movingrest.inl"	// (2026-11-07) MovingCasterHistory
#include "rt_host_sigma.inl"	// (2026-11-07) the RT shadow denoiser vs the exact soft shadow (docs/next/RT-DENOISER.md)
#include "rt_host_sigma_sun.inl"
#include "rt_host_refl.inl"	// (2026-11-07) the RT reflection denoiser vs the exact glossy reflection (docs/next/RT-REFLECTION-DENOISER.md)
#include "rt_host_light.inl"	// (2026-11-06) the path tracer's lamp falloff: game vs physical (PTLampFalloff)	// (2026-11-06) chain-link: fine cut-outs through the 64 x 64 atlas slot
#include "rt_host_pt.inl"	// (2026-11-07) the path tracer's quality scenes: ReSTIR DI (PTRestirDI)

// ------------------------------------------------------------------------------------------------------------------------------
int main(int argc, char** argv) {
	std::string helperExe, spv, compute, includes, device = "Radeon";
	std::vector<std::string> only;
	bool keepGoing = false;
	for (int i = 1; i < argc; i++) {
		const std::string a = argv[i];
		if (a == "--helper" && i + 1 < argc) helperExe = argv[++i];
		else if (a == "--spv" && i + 1 < argc) spv = argv[++i];
		else if (a == "--compute" && i + 1 < argc) compute = argv[++i];    // RtDenoise's fork passes (default: <spv>\..)
		else if (a == "--includes" && i + 1 < argc) includes = argv[++i];  // PointShadowForward.hlsl's folder
		else if (a == "--out" && i + 1 < argc) gOut = argv[++i];
		else if (a == "--device" && i + 1 < argc) device = argv[++i];
		else if (a == "--scene" && i + 1 < argc) only.push_back(argv[++i]);
		else if (a == "--keep-going") keepGoing = true;
		else { std::printf("usage: NvrRtHost.exe --helper <NvrRtHelper.exe> --spv <folder> [--out <folder>] [--device <name part>] [--scene <name>]...\n"
			"scenes: furnace alphacard material chainlink manylamps mirror cameramove sun ao pathtex falloff restirlamps restirgi denoise lamplookup scenepick sameframe movingrest\n"); return 2; }
	}
	if (helperExe.empty() || spv.empty()) { std::printf("--helper and --spv are required\n"); return 2; }
	{ char full[MAX_PATH]; GetFullPathNameA(spv.c_str(), MAX_PATH, full, nullptr); spv = full; }
	if (compute.empty()) compute = spv + "\\..";
	{ char full[MAX_PATH]; GetFullPathNameA(compute.c_str(), MAX_PATH, full, nullptr); compute = full; }
	if (includes.empty()) includes = compute + "\\..\\Shaders\\Includes";
	if (!gOut.empty()) { CreateDirectoryA(gOut.c_str(), nullptr); fopen_s(&gReport, (gOut + "\\report.txt").c_str(), "w"); }
	const ULONGLONG t0 = GetTickCount64();

	Host host;
	Ctx c = {}; c.host = &host;
	if (!host.Init(device.c_str())) { say("RESULT: FAIL (no device / fork)\n"); return 1; }
	// the images (one set for every scene; at most 48)
	c.depth = host.AddImage("depth", S, S, NVR_EXT_SHARED_R32F);
	c.normals = host.AddImage("normals", S, S, NVR_EXT_SHARED_RGBA16F);
	c.lampResult = host.AddImage("lamp result", S, S, NVR_EXT_SHARED_RG32F);
	c.lampTable = host.AddImage("lamp table", 1024, 1, NVR_EXT_SHARED_RG32F);
	if (const char* e = std::getenv("NVR_RT_HOST_ALPHA_SLOT")) kAlphaSlot = std::max(16, std::min(512, std::atoi(e)));
	c.alphaAtlas = host.AddImage("alpha atlas", 32 * kAlphaSlot, 32 * kAlphaSlot, NVR_EXT_SHARED_R16F);
	c.alphaStandIn = host.AddImage("alpha stand-in", 1, 1, NVR_EXT_SHARED_R16F);
	c.histA = host.AddImage("history 0", S, S, NVR_EXT_SHARED_RGBA16F);
	c.histB = host.AddImage("history 1", S, S, NVR_EXT_SHARED_RGBA16F);
	c.ptA = host.AddImage("pt mean rg", S, S, NVR_EXT_SHARED_RG32F);
	c.ptB = host.AddImage("pt mean b", S, S, NVR_EXT_SHARED_RG32F);
	c.ptDisplay = host.AddImage("pt display", S, S, NVR_EXT_SHARED_RGBA16F);
	c.aux16 = host.AddImage("rgba16f aux", S, S, NVR_EXT_SHARED_RGBA16F);
	c.pano = host.AddImage("sky panorama", 128, 128, NVR_EXT_SHARED_RGBA16F);
	c.refDir = host.AddImage("refl dir", S, S, NVR_EXT_SHARED_RG32F);
	c.refRay = host.AddImage("refl ray", S, S, NVR_EXT_SHARED_RG32F);
	c.refHit = host.AddImage("refl hit", S, S, NVR_EXT_SHARED_RGBA16F);
	c.refShade = host.AddImage("refl shade", S, S, NVR_EXT_SHARED_RGBA16F);
	for (int i : { c.depth, c.normals, c.lampResult, c.lampTable, c.alphaAtlas, c.alphaStandIn, c.histA, c.histB, c.ptA, c.ptB, c.ptDisplay, c.aux16,
		c.pano, c.refDir, c.refRay, c.refHit, c.refShade }) if (i < 0) { say("RESULT: FAIL (shared images)\n"); return 1; }
	c.shPath = host.AddShader(spv + "\\RtPathTrace.spv");
	c.shLamp = host.AddShader(spv + "\\RtLampTrace.spv");
	c.shMany = host.AddShader(spv + "\\RtLampTraceMany.spv");
	c.shRefl = host.AddShader(spv + "\\ReflectionsRT.spv");
	if (!registerMore(host, spv, compute, includes) || !registerSameFrame(host, spv)) { say("RESULT: FAIL (shared images)\n"); return 1; }
	if (!registerSigma(host, spv)) { say("RESULT: FAIL (shared images)\n"); return 1; }
	gCacheShader = host.AddShader(spv + "\\RtPathTraceCache.spv");   // (2026-11-07) PTRadianceCache's resolve pass
	for (auto& s : host.shaders) if (GetFileAttributesA(s.c_str()) == INVALID_FILE_ATTRIBUTES) { say("RESULT: FAIL (no %s)\n", s.c_str()); return 1; }
	if (!host.Start(helperExe.c_str())) { host.Stop(); say("RESULT: FAIL (helper did not get ready)\n"); return 1; }
	// clear the atlas once (an empty slot reads alpha 0) and the history pair
	{ std::vector<float> z(size_t(32 * kAlphaSlot) * (32 * kAlphaSlot), 0.0f); if (!host.Upload(c.alphaAtlas, z)) return 1;
	  std::vector<float> hz(size_t(S) * S * 4, 0.0f); host.Upload(c.histA, hz); host.Upload(c.histB, hz);
	  if (!host.Exchange(World(), {}, V3()) || !host.Finish()) { host.Stop(); say("RESULT: FAIL (first exchange)\n"); return 1; }
	  host.EndFrame(); }
	say("ready in %.2f s\n", (GetTickCount64() - t0) / 1000.0);

	struct Scene { const char* name; const char* what; std::function<bool()> run; };
	const std::vector<Scene> scenes = {
		{ "furnace", "white furnace / energy (RtPathTrace)", [&] { return sceneFurnace(c); } },
		{ "alphacard", "lamp behind an alpha-tested card (RtLampTrace + RtAlpha)", [&] { return sceneAlphaCard(c, false); } },
		{ "material", "card record with material bits 26-31 (atlas slot mask)", [&] { return sceneAlphaCard(c, true); } },
		{ "chainlink", "fine chain-link cut-out: lamp shadow vs the full-resolution texture (atlas fill / alpha read quality)", [&] { return sceneChainLink(c); } },
		{ "manylamps", "48 lamps, slots past 32 (RtLampTrace)", [&] { return sceneManyLamps(c); } },
		{ "mirror", "mirror floor + blocks (ReflectionsRT)", [&] { return sceneMirror(c); } },
		{ "cameramove", "camera move with history (RtLampTraceMany)", [&] { return sceneCameraMove(c); } },
		{ "sun", "sun shadows, half resolution, Penumbra blockers (RtSunShadow)", [&] { return sceneSun(c); } },
		{ "ao", "AO + sky visibility (RtAOTrace)", [&] { return sceneAO(c); } },
		{ "pathtex", "path tracer: albedo atlas, 64 lamps in the lamp image, PTLightGrid (RtPathTrace / Grid)", [&] { return scenePathTextures(c); } },
		{ "falloff", "path tracer lamp falloff: the game's vs physical (PTLampFalloff 1), against the formulas", [&] { return sceneFalloff(c); } },
		{ "restirlamps", "ManyLights one ray a pixel, reuse + history (RtLampTraceRestir)", [&] { return sceneRestirLamps(c); } },
		{ "restirdi", "path tracer ReSTIR DI for the lamps (PTRestirDI): exact shadowed lamp sum, shuffled lamp list, slide, broken-remap control", [&] { return sceneRestirDI(c); } },
		{ "specfurnace", "path tracer specular energy: a white metal box under a uniform sky (VNDF + multiple-scattering compensation)", [&] { return sceneSpecularFurnace(c); } },
		{ "cache", "path tracer radiance cache (PTRadianceCache): 2 bounces + the cache vs 32 bounces, the light halved", [&] { return sceneRadianceCache(c); } },
		{ "restirgi", "ReSTIR GI on the path tracer (RtPathTraceGI + RtRestirGI)", [&] { return sceneRestirGI(c); } },
		{ "denoise", "the game's RtDenoise passes (fork, D3D9 side)", [&] { return sceneDenoise(c); } },
		{ "sigmalamps", "RT shadow denoiser, lamps of physical size vs the exact soft shadow, still + moving (RtLampTraceSigma + RtShadowDenoise)", [&] { return sceneSigmaLamps(c); } },
		{ "sigmasun", "RT shadow denoiser on the sun (10 deg disc + the real 0.53 deg) vs the exact soft shadow, still + moving (RtSunShadow + RtShadowDenoise)", [&] { return sceneSigmaSun(c); } },
		{ "refldenoise", "RT reflection denoiser (FFX port / ours) vs the old Temporal + Blur and the exact glossy reflection, still + slide + turn (RtReflDenoise*)", [&] { return sceneReflDenoise(c); } },
		{ "lamplookup", "the lamp shaders' lookup PointShadowTraced (D3D9 ps_3_0)", [&] { return sceneLampLookup(c); } },
		{ "scenepick", "RtCore's scene picking rules + record packing (RtScenePick.h)", [&] { return sceneScenePick(c); } },
		{ "movingrest", "ManyLights histories under a walking person: MovingCasterHistory 0 vs 2 (RtLampTraceMany + RtLampTraceRestir)", [&] { return sceneMovingRest(c); } },
		{ "sameframe", "SameFrame lamp shadows: camera rays + trace in one exchange, a walking caster, the hidden first-person body (RtLampPrimary + RtLampTrace + PointShadowTraced)", [&] { return sceneSameFrame(c); } },
	};
	int failed = 0, ran = 0;
	std::string summary;
	for (const Scene& s : scenes) {
		if (!only.empty() && std::find(only.begin(), only.end(), s.name) == only.end()) continue;
		const ULONGLONG ts = GetTickCount64();
		say("[%s] %s\n", s.name, s.what);
		const bool ok = s.run();
		ran++;
		if (!ok) failed++;
		char line[256];
		snprintf(line, sizeof(line), "  %-11s %s  (%.2f s)\n", s.name, ok ? "PASS" : "FAIL", (GetTickCount64() - ts) / 1000.0);
		summary += line;
		say("%s", line);
		DWORD code = 0;
		if (!ok && GetExitCodeProcess(host.helper.hProcess, &code) && code != STILL_ACTIVE) { say("  the helper exited (%lu): stopping\n", code); break; }
	}
	// one empty frame so the last scene's releases reach the helper (a failed scene may have left its geometry: released here)
	for (auto it = host.live.begin(); it != host.live.end();) { NvrRtGeometryOp op = {}; op.op = NVR_RT_OP_RELEASE; op.id = it->first; host.ops.push_back(op); it = host.live.erase(it); }
	if (host.Exchange(World(), {}, V3()) && host.Finish()) host.EndFrame();
	say("helper: %u geometries alive, %u failed ops, shader failures 0x%x, GPU %.3f ms avg (builds %.3f, scene %.3f, passes %.3f), last: %s\n",
		host.shared->geometries, host.shared->failedOps, host.shared->shaderFailures, host.shared->gpuTimeUsAvg / 1000.0, host.shared->buildUsAvg / 1000.0,
		host.shared->sceneUsAvg / 1000.0, host.shared->traceUsAvg / 1000.0, host.shared->message);
	if (host.shared->failedOps) { say("  helper refused %u ops -> FAIL\n", host.shared->failedOps); failed++; }
	host.Stop();
	say("\nSUMMARY (%d scenes, %.1f s)\n%s", ran, (GetTickCount64() - t0) / 1000.0, summary.c_str());
	say("RESULT: %s\n", failed ? "FAIL" : "PASS");
	if (gReport) std::fclose(gReport);
	return failed ? 1 : 0;
}
