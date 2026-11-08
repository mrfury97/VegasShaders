#include "DxvkCompute.h"
#include <unordered_map>
#include "RenderRect.h"	// UNOFFICIAL 2026-10-27 RenderScale v2
#include "CapturedNormals.h"	// UNOFFICIAL 2026-10-28 (DisplaceObjectsLayout)
#include "MixCheck.h"	// UNOFFICIAL 2026-10-30 NvrDxvkMixCheck
#include "RtCore.h"	// UNOFFICIAL 2026-11-07 (DisplaceRtSlack: the RT scene's displaced copies)

ID3D9NvrExt*	DxvkCompute::Ext = nullptr;
NVR_EXT_INFO	DxvkCompute::ExtInfo = {};
bool			DxvkCompute::Failed = false;
char			DxvkCompute::StatusText[160] = "not checked yet";
ID3D9NvrExtSampler* DxvkCompute::SamplerLinear = nullptr;
ID3D9NvrExtSampler* DxvkCompute::SamplerPoint = nullptr;
ID3D9NvrExtSampler* DxvkCompute::SamplerWrap = nullptr;
ID3D9NvrExtSampler* DxvkCompute::SamplerPointWrap = nullptr;

void DxvkCompute::Initialize(IDirect3DDevice9* device) {
	Ext = nullptr;
	Failed = false;
	if (!device) return;

	ID3D9NvrExt* ext = nullptr;
	if (FAILED(device->QueryInterface(__uuidof(ID3D9NvrExt), (void**)&ext)) || !ext) {
		_snprintf_s(StatusText, sizeof(StatusText), _TRUNCATE, "Vegas Shaders DXVK extension not present (native D3D9 or stock DXVK): compute paths off, D3D9 passes used");
		Logger::Log("UNOFFICIAL Vegas Shaders DXVK:%s", StatusText);
		return;
	}

	ext->GetInfo(&ExtInfo);
	if (ExtInfo.ExtVersionMajor != NVR_EXT_VERSION_MAJOR) {
		_snprintf_s(StatusText, sizeof(StatusText), _TRUNCATE, "Vegas Shaders DXVK extension %u.%u found but this build of Vegas Shaders needs %u.x: compute paths off",
			ExtInfo.ExtVersionMajor, ExtInfo.ExtVersionMinor, (unsigned)NVR_EXT_VERSION_MAJOR);
		Logger::Log("UNOFFICIAL Vegas Shaders DXVK:%s", StatusText);
		ext->Release();
		return;
	}

	Ext = ext;	// keeps a device reference for the session (never disposed, like the managers)
	_snprintf_s(StatusText, sizeof(StatusText), _TRUNCATE, "Vegas Shaders DXVK extension %u.%u on DXVK %s (push %u bytes, subgroup %u): compute paths available",
		ExtInfo.ExtVersionMajor, ExtInfo.ExtVersionMinor, ExtInfo.DxvkVersion, ExtInfo.MaxPushBytes, ExtInfo.SubgroupSize);
	Logger::Log("UNOFFICIAL Vegas Shaders DXVK:%s", StatusText);
}

const char* DxvkCompute::Status() {
	return StatusText;
}

void DxvkCompute::Fail(const char* Reason) {
	if (Failed) return;
	Failed = true;
	_snprintf_s(StatusText, sizeof(StatusText), _TRUNCATE, "compute paths OFF for this session: %s", Reason ? Reason : "unknown");
	Logger::Log("UNOFFICIAL Vegas Shaders DXVK:%s", StatusText);
}

bool DxvkCompute::LoadSpirv(const char* Name, std::vector<char>& Words) {
	Words.clear();
	char path[MAX_PATH];
	_snprintf_s(path, sizeof(path), _TRUNCATE, "%s%s.spv", Paths::ComputeDir(), Name);

	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file) {
		Logger::Log("UNOFFICIAL Vegas Shaders DXVK:cannot open %s", path);
		return false;
	}
	std::streamsize size = file.tellg();
	file.seekg(0, std::ios::beg);
	if (size < 20 || (size % 4) != 0) {
		Logger::Log("UNOFFICIAL Vegas Shaders DXVK:%s is not SPIR-V (size %d)", path, (int)size);
		return false;
	}
	Words.resize((size_t)size);
	file.read(Words.data(), size);
	if (!file) {
		Words.clear();
		Logger::Log("UNOFFICIAL Vegas Shaders DXVK:could not read %s", path);
		return false;
	}
	return true;
}

ID3D9NvrExtPipeline* DxvkCompute::CreatePipeline(const char* Name, const NVR_EXT_BINDING_DESC* Bindings, UINT Count, UINT PushSize, bool UsesSamplers) {
	if (!Available()) return nullptr;

	std::vector<char> words;
	if (!LoadSpirv(Name, words)) {	// 2026-11-02: only this pass is off (it used to switch every compute path off for the session)
		Logger::Log("UNOFFICIAL Vegas Shaders DXVK:%s.spv missing: that pass is off, the rest keep running", Name);
		return nullptr;
	}

	NVR_EXT_COMPUTE_PIPELINE_DESC desc = {};
	desc.pSpirv = words.data();
	desc.SpirvSize = (UINT)words.size();
	desc.BindingCount = Count;
	desc.pBindings = Bindings;
	desc.PushConstantSize = PushSize;
	desc.UsesSamplerHeap = UsesSamplers ? TRUE : FALSE;
	desc.SamplerHeapSet = 0;
	desc.SamplerHeapBinding = 0;
	desc.pDebugName = Name;

	ID3D9NvrExtPipeline* pipeline = nullptr;
	HRESULT result = Ext->CreateComputePipeline(&desc, &pipeline);
	if (FAILED(result) || !pipeline) {
		Logger::Log("UNOFFICIAL Vegas Shaders DXVK:CreateComputePipeline(%s) failed (%08lX); DXVK's log says why", Name, result);
		Fail("a compute pipeline was refused");
		return nullptr;
	}
	Logger::Log("UNOFFICIAL Vegas Shaders DXVK:compute pipeline %s ready (%u bindings, %u push bytes)", Name, Count, PushSize);
	return pipeline;
}

ID3D9NvrExtBuffer* DxvkCompute::CreateBuffer(UINT Size, UINT Flags, const char* Name) {
	if (!Available()) return nullptr;
	NVR_EXT_BUFFER_DESC desc = { Size, Flags, Name };
	ID3D9NvrExtBuffer* buffer = nullptr;
	HRESULT result = Ext->CreateBuffer(&desc, &buffer);
	if (FAILED(result) || !buffer) {
		Logger::Log("UNOFFICIAL Vegas Shaders DXVK:CreateBuffer(%s, %u bytes) failed (%08lX)", Name ? Name : "", Size, result);
		Fail("a buffer could not be created");
		return nullptr;
	}
	return buffer;
}

ID3D9NvrExtBuffer* DxvkCompute::CreateBufferNoFail(UINT Size, UINT Flags, const char* Name) {
	if (!Available()) return nullptr;
	NVR_EXT_BUFFER_DESC desc = { Size, Flags, Name };
	ID3D9NvrExtBuffer* buffer = nullptr;
	HRESULT result = Ext->CreateBuffer(&desc, &buffer);
	if (FAILED(result) || !buffer) {
		Logger::Log("UNOFFICIAL Vegas Shaders DXVK:CreateBuffer(%s, %u bytes) failed (%08lX)", Name ? Name : "", Size, result);
		return nullptr;
	}
	return buffer;
}

// UNOFFICIAL 2026-10-26 (alflicker): see DxvkCompute.h. A sampled image's pResource is an IDirect3DBaseTexture9* or IDirect3DSurface9*
// (single inheritance from IDirect3DResource9: the same pointer); DXVK's PreLoad returns at once for anything not in the managed pool.
void DxvkCompute::PreLoad(const NVR_EXT_RESOURCE_BINDING* Bindings, UINT Count) {
	for (UINT i = 0; Bindings && i < Count; i++)
		if (Bindings[i].Type == NVR_EXT_BIND_SAMPLED_IMAGE && Bindings[i].pResource)
			static_cast<IDirect3DResource9*>(static_cast<void*>(Bindings[i].pResource))->PreLoad();
	// UNOFFICIAL 2026-10-30 NvrDxvkMixCheck (core/MixCheck.h): every extension pass's bindings come through here: sampled images are its
	// reads, storage images its writes (off: one bool test).
	MixCheck::NoteBindings(Bindings, Count, NVR_EXT_BIND_SAMPLED_IMAGE, NVR_EXT_BIND_STORAGE_IMAGE);
}

ID3D9NvrExtSampler* DxvkCompute::CreateSampler(NVR_EXT_FILTER Filter, NVR_EXT_ADDRESS Address) {
	if (!Available()) return nullptr;
	NVR_EXT_SAMPLER_DESC desc = { Filter, Address };
	ID3D9NvrExtSampler* sampler = nullptr;
	HRESULT result = Ext->CreateSampler(&desc, &sampler);
	if (FAILED(result) || !sampler) {
		Logger::Log("UNOFFICIAL Vegas Shaders DXVK:CreateSampler failed (%08lX)", result);
		Fail("a sampler could not be created");
		return nullptr;
	}
	return sampler;
}

ID3D9NvrExtPipeline* DxvkCompute::CreateGraphicsPipeline(const char* Name, const NVR_EXT_BINDING_DESC* Bindings, UINT Count, UINT PushSize, bool UsesSamplers, UINT TargetCount, NVR_EXT_BLEND Blend, bool FailAll) {
	if (!Available()) return nullptr;
	if (ExtInfo.ExtVersionMinor < 1) {
		Logger::Log("UNOFFICIAL Vegas Shaders DXVK:full-screen passes need extension 1.1 (this d3d9.dll has %u.%u): %s stays a D3D9 pass", ExtInfo.ExtVersionMajor, ExtInfo.ExtVersionMinor, Name);
		return nullptr;
	}

	std::vector<char> words;
	// 2026-11-02: a missing file only keeps this effect on its D3D9 shader. It used to switch every compute path off for the
	// session (FSR included) the first time such an effect drew: GodRays, WetWorld, AmbientOcclusion and ShadowsExteriors still try
	// the Vulkan passes the shader rewrite (wave 3) removed. A refused pipeline below still stops everything (a broken d3d9.dll).
	if (!LoadSpirv(Name, words)) {
		Logger::Log("UNOFFICIAL Vegas Shaders DXVK:%s.spv missing: %s stays a D3D9 pass, the rest keep running", Name, Name);
		return nullptr;
	}

	NVR_EXT_GRAPHICS_PIPELINE_DESC desc = {};
	desc.pFragmentSpirv = words.data();
	desc.FragmentSpirvSize = (UINT)words.size();
	desc.BindingCount = Count;
	desc.pBindings = Bindings;
	desc.PushConstantSize = PushSize;
	desc.UsesSamplerHeap = UsesSamplers ? TRUE : FALSE;
	desc.SamplerHeapSet = 0;
	desc.SamplerHeapBinding = 0;
	desc.ColorTargetCount = TargetCount;
	desc.Blend = Blend;
	desc.ColorWriteMask = 0;
	desc.pDebugName = Name;

	ID3D9NvrExtPipeline* pipeline = nullptr;
	HRESULT result = Ext->CreateGraphicsPipeline(&desc, &pipeline);
	if (FAILED(result) || !pipeline) {
		Logger::Log("UNOFFICIAL Vegas Shaders DXVK:CreateGraphicsPipeline(%s) failed (%08lX); DXVK's log says why", Name, result);
		if (FailAll) Fail("a pass pipeline was refused");
		return nullptr;
	}
	Logger::Log("UNOFFICIAL Vegas Shaders DXVK:pass %s ready (%u bindings, %u push bytes, %u target%s)", Name, Count, PushSize, TargetCount, TargetCount == 1 ? "" : "s");
	return pipeline;
}

ID3D9NvrExtSampler* DxvkCompute::LinearClamp() {
	if (!SamplerLinear) SamplerLinear = CreateSampler(NVR_EXT_FILTER_LINEAR, NVR_EXT_ADDRESS_CLAMP);
	return SamplerLinear;
}

ID3D9NvrExtSampler* DxvkCompute::PointClamp() {
	if (!SamplerPoint) SamplerPoint = CreateSampler(NVR_EXT_FILTER_NEAREST, NVR_EXT_ADDRESS_CLAMP);
	return SamplerPoint;
}

bool DxvkCompute::HasSpirv(const char* Name) {
	char path[MAX_PATH];
	_snprintf_s(path, sizeof(path), _TRUNCATE, "%s%s.spv", Paths::ComputeDir(), Name);
	return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

bool DxvkCompute::VariantsOn() {
	return TheSettingManager->SettingsMain.Main.NvrDxvkVariants;
}

bool DxvkCompute::StencilOn() {
	return TheSettingManager->SettingsMain.Main.NvrDxvkStencil && Available() && ExtInfo.ExtVersionMinor >= 6;
}

bool DxvkCompute::KeptOnD3D9(const char* Name) {
	const char* list = TheSettingManager->SettingsMain.Main.NvrDxvkKeepD3D9;
	if (!list[0] || !Name || !Name[0]) return false;
	const size_t length = strlen(Name);
	for (const char* p = list; (p = strstr(p, Name)) != nullptr; p += length) {
		const bool startOk = p == list || p[-1] == ',' || p[-1] == ' ';
		const bool endOk = p[length] == '\0' || p[length] == ',' || p[length] == ' ';
		if (startOk && endOk) return true;
	}
	return false;
}

bool DxvkCompute::PassesOn() {
	// UNOFFICIAL 2026-10-27 RenderScale v2: the Vulkan passes are off in rect frames (they draw the whole target; Available stays
	// true, FSR runs through the extension).
	return TheSettingManager->SettingsMain.Main.NvrDxvkPasses && !RenderRect::Active() && Available();
}

void DxvkCompute::ProfileTick(bool On) {
	static bool profiling = false;
	static unsigned frames = 0;
	static NVR_EXT_PROFILE last = {};
	if (!Ext || ExtInfo.ExtVersionMinor < 5) return;
	if (On != profiling) {
		profiling = On;
		Ext->SetProfiling(On ? TRUE : FALSE);
		if (On) { Ext->GetProfile(&last); frames = 0; }
		Logger::Log("DXVK PROFILE %s (every 8th frame's D3D9 calls timed; a line every 120 frames)", On ? "on" : "off");
		return;
	}
	if (!On || ++frames < 120) return;
	frames = 0;

	NVR_EXT_PROFILE now = {};
	if (FAILED(Ext->GetProfile(&now))) return;
	const double f = (double)(now.Frames - last.Frames), timed = (double)(now.TimedFrames - last.TimedFrames);
	const double us = (double)(now.TimeUs - last.TimeUs);
	if (f < 1.0 || us <= 0.0) { last = now; return; }
	const double ticksPerMs = (double)(now.Tsc - last.Tsc) / us * 1000.0;
	const double calls = (double)(now.ApiCalls - last.ApiCalls);
	const double frameMs = us / 1000.0 / f;
	// The game thread inside D3D9 calls (timed frames, the timer's own cost taken off), and the waits inside that.
	const double apiMs = timed > 0.0 ? ((double)(now.ApiTicks - last.ApiTicks) - calls * (double)now.OverheadTicks100 / 100.0) / ticksPerMs / timed : 0.0;
	const double presentWaitMs = timed > 0.0 ? (double)(now.TimedWaitTicks - last.TimedWaitTicks) / ticksPerMs / timed : 0.0;
	const double csSyncMs = (double)(now.CsSyncUs - last.CsSyncUs) / 1000.0 / f;
	const double gpuSyncMs = (double)(now.GpuSyncUs - last.GpuSyncUs) / 1000.0 / f;
	const double dxvkWorkMs = apiMs - presentWaitMs - csSyncMs - gpuSyncMs;
	const double csBusyMs = frameMs - (double)(now.CsIdleUs - last.CsIdleUs) / 1000.0 / f;
	const double gpuBusyMs = frameMs - (double)(now.GpuIdleUs - last.GpuIdleUs) / 1000.0 / f;
	Logger::Log("DXVK PROFILE frame %.2f ms (%.0f frames, passes %s) | game thread: outside D3D9 %.2f, inside D3D9 %.2f = DXVK work %.2f + "
		"present wait %.2f + wait for DXVK thread %.2f + wait for GPU %.2f (%.0f calls) | DXVK thread busy %.2f | GPU busy %.2f | per frame: "
		"draws %.0f, render passes %.0f, barriers %.0f, submits %.1f, chunks %.0f | new pipelines %llu",
		frameMs, f, PassesOn() ? "Vulkan" : "D3D9", frameMs - apiMs, apiMs, dxvkWorkMs, presentWaitMs, csSyncMs, gpuSyncMs, timed > 0.0 ? calls / timed : 0.0,
		csBusyMs, gpuBusyMs, (double)(now.Draws - last.Draws) / f, (double)(now.RenderPasses - last.RenderPasses) / f,
		(double)(now.Barriers - last.Barriers) / f, (double)(now.Submits - last.Submits) / f, (double)(now.CsChunks - last.CsChunks) / f,
		(unsigned long long)(now.GraphicsPipelines - last.GraphicsPipelines));
	last = now;
}

bool DxvkCompute::ReplacementsOn() {
	return TheSettingManager->SettingsMain.Main.NvrDxvkReplacements && Available();
}

// "textures=0,1,2" -> bit mask; "floats=0-3,8-15" -> 224-bit register mask. Unknown lines are ignored.
static bool ParseReplacementManifest(const char* Path, UINT& TextureMask, UINT* FloatMask, bool& FloatsGiven) {
	std::ifstream file(Path);
	if (!file) return false;
	std::string line;
	while (std::getline(file, line)) {
		const char* s = line.c_str();
		while (*s == ' ' || *s == '\t') s++;
		bool textures = !_strnicmp(s, "textures=", 9), floats = !_strnicmp(s, "floats=", 7);
		if (!textures && !floats) continue;
		s += textures ? 9 : 7;
		if (floats) FloatsGiven = true;
		while (*s) {
			while (*s == ' ' || *s == ',') s++;
			if (!*s) break;
			char* end = nullptr;
			long first = strtol(s, &end, 10), last = first;
			if (end == s) break;
			s = end;
			if (*s == '-') { last = strtol(s + 1, &end, 10); s = end; }
			for (long r = first; r <= last; r++) {
				if (textures && r >= 0 && r < 16) TextureMask |= 1u << r;
				if (floats && r >= 0 && r < 224) FloatMask[r / 32] |= 1u << (r % 32);
			}
		}
	}
	return true;
}

bool DxvkCompute::RegisterReplacement(const char* Name, const void* Bytecode, UINT Size, const char* Template, const D3DXMACRO* Defines) {
	if (!Name || !Bytecode || !Size || !ReplacementsOn()) return false;

	// 2026-10-25: the interior and exterior records of a shader share its name but not its bytecode, so a replacement is
	// looked up as <Name>@<hash>.spv first (FNV-1a of the bytecode, 8 hex digits), then <Name>.spv. With a Replace\dump
	// folder present, every pixel shader is written there as <Name>@<hash>.pso (the exact bytes a replacement is made from).
	UINT hash = 2166136261u;
	for (UINT i = 0; i < Size; i++) hash = (hash ^ ((const unsigned char*)Bytecode)[i]) * 16777619u;
	char dump[MAX_PATH];
	_snprintf_s(dump, sizeof(dump), _TRUNCATE, "%sReplace\\dump\\%s@%08X.pso", Paths::ComputeDir(), Name, hash);
	if (GetFileAttributesA((std::string(Paths::ComputeDir()) + "Replace\\dump").c_str()) != INVALID_FILE_ATTRIBUTES &&
		GetFileAttributesA(dump) == INVALID_FILE_ATTRIBUTES) {
		std::ofstream out(dump, std::ios::binary);
		out.write((const char*)Bytecode, Size);
		// UNOFFICIAL 2026-10-26 (FORK-1026-LIMITS.md): the source and defines this bytecode came from, one per line:
		// "template=<file name without .hlsl>" then "<NAME>=<value>" (the no-limits tool compiles the same variant without ps_3_0's limits).
		if (Template) {
			char definesPath[MAX_PATH];
			_snprintf_s(definesPath, sizeof(definesPath), _TRUNCATE, "%sReplace\\dump\\%s@%08X.defines", Paths::ComputeDir(), Name, hash);
			std::ofstream defs(definesPath, std::ios::binary);
			defs << "template=" << Template << "\n";
			for (int d = 0; Defines && d < 64 && Defines[d].Name; d++)
				defs << Defines[d].Name << "=" << (Defines[d].Definition ? Defines[d].Definition : "") << "\n";
		}
	}

	char path[MAX_PATH], key[MAX_PATH];
	_snprintf_s(key, sizeof(key), _TRUNCATE, "%s@%08X", Name, hash);
	_snprintf_s(path, sizeof(path), _TRUNCATE, "%sReplace\\%s.spv", Paths::ComputeDir(), key);
	if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
		_snprintf_s(key, sizeof(key), _TRUNCATE, "%s", Name);
		_snprintf_s(path, sizeof(path), _TRUNCATE, "%sReplace\\%s.spv", Paths::ComputeDir(), key);
		if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return false;	// the normal case: no replacement for this shader
	}

	if (ExtInfo.ExtVersionMinor < 2) {
		static bool said = false;
		if (!said) Logger::Log("UNOFFICIAL Vegas Shaders DXVK:shader replacements need extension 1.2 (this d3d9.dll has %u.%u): %s and the others stay as they are", ExtInfo.ExtVersionMajor, ExtInfo.ExtVersionMinor, Name);
		said = true;
		return false;
	}

	std::vector<char> words;
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	std::streamsize size = file ? (std::streamsize)file.tellg() : 0;
	if (size < 20 || (size % 4) != 0) {
		Logger::Log("UNOFFICIAL Vegas Shaders DXVK:%s is not SPIR-V (size %d): %s stays as it is", path, (int)size, Name);
		return false;
	}
	file.seekg(0, std::ios::beg);
	words.resize((size_t)size);
	file.read(words.data(), size);
	if (!file) { Logger::Log("UNOFFICIAL Vegas Shaders DXVK:could not read %s", path); return false; }

	char manifest[MAX_PATH];
	_snprintf_s(manifest, sizeof(manifest), _TRUNCATE, "%sReplace\\%s.txt", Paths::ComputeDir(), key);
	NVR_EXT_SHADER_REPLACEMENT_DESC desc = {};
	bool floatsGiven = false;
	if (!ParseReplacementManifest(manifest, desc.TextureMask, desc.FloatRegisterMask, floatsGiven)) {
		Logger::Log("UNOFFICIAL Vegas Shaders DXVK:%s has no %s (textures= / floats= lines): %s stays as it is", path, manifest, Name);
		return false;
	}

	desc.pBytecode = Bytecode;
	desc.BytecodeSize = Size;
	desc.pSpirv = words.data();
	desc.SpirvSize = (UINT)words.size();
	desc.pDebugName = Name;
	HRESULT result = Ext->RegisterShaderReplacement(&desc);
	if (FAILED(result)) {
		Logger::Log("UNOFFICIAL Vegas Shaders DXVK:RegisterShaderReplacement(%s) refused (%08lX; DXVK's log says why, usually textures= naming a sampler the original does not use): it stays as it is", Name, result);
		return false;
	}
	Logger::Log("UNOFFICIAL Vegas Shaders DXVK:%s will run as %s (textures %04X, %s float layout)", Name, path, desc.TextureMask, floatsGiven ? "listed" : "original compacted");
	return true;
}

ID3D9NvrExtSampler* DxvkCompute::LinearWrap() {
	if (!SamplerWrap) SamplerWrap = CreateSampler(NVR_EXT_FILTER_LINEAR, NVR_EXT_ADDRESS_WRAP);
	return SamplerWrap;
}

// UNOFFICIAL 2026-10-25 (extension 1.3): see DxvkCompute.h. One sampler per kind, made on first use.
ID3D9NvrExtSampler* DxvkCompute::MipSampler(bool Anisotropic, NVR_EXT_ADDRESS Address) {
	static ID3D9NvrExtSampler* samplers[2][4] = {};
	if (!Available() || ExtInfo.ExtVersionMinor < 3 || (UINT)Address > 3) return nullptr;
	ID3D9NvrExtSampler*& s = samplers[Anisotropic ? 1 : 0][Address];
	if (!s) s = CreateSampler(Anisotropic ? NVR_EXT_FILTER_ANISOTROPIC : NVR_EXT_FILTER_LINEAR_MIPMAPS, Address);
	return s;
}

// UNOFFICIAL 2026-10-25 (Cinema): NEAREST min and mag, WRAP (mips are not filtered either way).
ID3D9NvrExtSampler* DxvkCompute::PointWrap() {
	if (!SamplerPointWrap) SamplerPointWrap = CreateSampler(NVR_EXT_FILTER_NEAREST, NVR_EXT_ADDRESS_WRAP);
	return SamplerPointWrap;
}

// ------------------------------------------------------------------------------------------------------------------------------
// UNOFFICIAL 2026-10-27 tessellation displacement (fork patch 0021, extension 1.7-displace; see DxvkCompute.h and
// docs/next/FORK2-DISPLACE.md). The land family: TerrainTemplate.hlsl's vertex shader (SLS2100.vso: ModelViewProj in c0-c3, its
// outputs at TEXCOORD0-7 / COLOR0-1) and its pixel shaders (BaseMap[TEX_COUNT] in s0..; LandHeight[2] in c34-c35 says which layers
// carry a height map in the diffuse alpha, as the land's parallax reads it).

static ID3D9NvrExtTess*	DisplaceInterface = nullptr;
static bool				DisplaceQueried = false;
static UINT				DisplaceRegistered[2] = {};	// vertex, pixel shaders registered this session
static UINT				DisplaceDepthShaders = 0;	// 2026-10-27: land pixel shaders registered with ObjectBlend's TES (-d: the depth at s6)
// 2026-10-27 CastShadows (core/DisplaceShadows.h): family 2, the land's shadow casters. The vertex shader registered, the pixel shaders
// registered (bit N - 1: ShadowMapLand-tN.pso), the setting, and how many frames ago land was last drawn displaced into the cascades.
static UINT				DisplaceShadowVs = 0;
static unsigned			DisplaceShadowPs = 0;
static bool				DisplaceCastShadows = false;
static unsigned			DisplaceShadowAge = 1000;
// 2026-10-28 (tester: crusty dark ring round the player, bumps changing size with TAA jitter): ContactShadowsOutside, IgnoreMipBias
static bool				DisplaceContactOutside = true;
static bool				DisplaceIgnoreMipBias = true;
// 2026-10-28 (user: ObjectBlend "does cause heavy shimmering when other objects cast shadows onto the tessellated geometry"):
// ObjectBlendShadows off (default) = the sun's maps hold the full bumps and the land's lookup stays at or above them (MapsFull)
static bool				DisplaceBlendShadows = false;
// The land pixel shaders NVR made (TerrainTemplate PS) and their layer counts, for DisplaceShadows' capture of the land's draws.
static std::vector<std::pair<IDirect3DPixelShader9*, int>> LandPixelShaders;

// 2026-10-27 ObjectBlend: the state DisplaceTick read (ObjectBlend's per-draw fields off), the packed ObjectBlend value, and what
// SetTessState last got (DisplaceSceneDepth sends the base with ObjectBlend on for the world scene's land draws only).
static NVR_EXT_TESS_STATE	DisplaceBase = {};
static NVR_EXT_TESS_STATE	DisplaceSent = {};
static bool					DisplaceSentOnce = false;
static float				DisplaceObjectBlend = 0.0f;	// floor = ObjectBlendWidth, fraction / 0.99 = ObjectBlendKeep; 0 = off

// 2026-10-28 objects ([Shaders.POM.Displacement], family 3): their own state, the shaders registered (vertex, pixel), the pixel shaders
// the registration turned away, and which family's state SetTessState last got (the fork keeps one state for every family).
static NVR_EXT_TESS_STATE	DisplaceObj = {};
static UINT					DisplaceObjRegistered[2] = {};
static UINT					DisplaceObjSkipped = 0;
static unsigned				DisplaceSentFamily = DxvkCompute::kDisplaceLand;
static unsigned				DisplaceObjBinds = 0;	// object shader binds that sent the objects' state (the log's proof that they bind)

// 2026-10-28 far land (DxvkCompute::DisplaceLandPass): the land piece about to be drawn lies wholly past FadeEnd, so the land's state
// goes to the fork switched off. DisplaceWanted is the land state last asked for (sent as it is again when a near piece or any other
// pass comes); the setting ([Shaders.Terrain.Displacement] FarUntessellated). For the log, per context (world, water reflection,
// other): land passes, their strip draws (the game draws a strips geometry one DrawIndexedPrimitive per strip, NiDX9Renderer
// 0xE74840: NumArrays draws a pass), and the far ones; near <-> far changes.
static bool					DisplaceFar = false;
static NVR_EXT_TESS_STATE	DisplaceWanted = {};
static bool					DisplaceFarOn = true;
static unsigned				DisplaceLandPasses[3] = {}, DisplaceLandStrips[3] = {}, DisplaceFarPasses = 0, DisplaceFarStrips = 0, DisplaceFarSwitches = 0;
static const float			kDisplaceFarMargin = 256.0f;	// game units past FadeEnd (the eye the TCS gets is the game's, in object space)
// UNOFFICIAL 2026-10-29 FarHysteresis ([Shaders.Terrain.Displacement], game units, live, 0 = off; ROOTPLAN round 3 step 9: a Nuclear
// Sunset tester saw road pieces pop near FadeEnd + 256): a piece's far / near decision is kept per piece and changes only once the piece is
// FarHysteresis past the line on the other side, so a camera moving back and forth round the line does not flip it every frame.
static float				DisplaceFarHysteresis = 0.0f;
struct DisplaceFarRecord { bool isFar; UInt32 frame; };
static std::unordered_map<const NiGeometry*, DisplaceFarRecord> DisplaceFarPieces;
static unsigned				DisplaceFarFlips = 0;	// pieces whose choice differs from the frame before (the pop itself), for the log

static void DisplaceSend(const NVR_EXT_TESS_STATE& State, unsigned Family = DxvkCompute::kDisplaceLand) {
	// 2026-10-28 far land: one off state for every far piece, so a run of them sends nothing more
	static const NVR_EXT_TESS_STATE off = {};
	const NVR_EXT_TESS_STATE* send = &State;
	if (Family == DxvkCompute::kDisplaceLand) {
		if (&State != &DisplaceWanted) DisplaceWanted = State;
		if (DisplaceFar) send = &off;
	}
	DisplaceSentFamily = Family;
	if (DisplaceSentOnce && !memcmp(send, &DisplaceSent, sizeof(*send))) return;
	DisplaceSent = *send;
	DisplaceSentOnce = true;
	DisplaceInterface->SetTessState(send);
}

// TerrainDisplace.tese's sceneScale: the viewport's share of the depth copy, x and y to 1/2048
static float DisplacePackScale(float X, float Y) {
	const float qx = floorf(std::clamp(X, 0.0f, 1.0f) * 2048.0f + 0.5f), qy = floorf(std::clamp(Y, 0.0f, 1.0f) * 2048.0f + 0.5f);
	return qx * 4096.0f + qy;
}

// [Main.Main.Performance] NvrDxvkDisplacement (read here, not kept in SettingsMain: at shader load, then every 8 frames)
static bool DisplaceSwitch() {
	return TheSettingManager->GetSettingI("Main.Main.Performance", "NvrDxvkDisplacement") != 0;
}

// A [Shaders.Terrain.Displacement] switch, Missing when the key is in neither file (a DLL used with an older defaults toml)
static bool DisplaceSettingOr(const char* Key, bool Missing, const char* Section = "Shaders.Terrain.Displacement") {
	SettingManager::Configuration::ConfigNode node;
	return TheSettingManager->Config.FillNode(&node, Section, Key) ? node.IntValue != 0 : Missing;
}

static float DisplaceFloatOr(const char* Key, float Missing, const char* Section = "Shaders.Terrain.Displacement") {
	SettingManager::Configuration::ConfigNode node;
	return TheSettingManager->Config.FillNode(&node, Section, Key) ? node.FloatValue : Missing;
}

static ID3D9NvrExtTess* DisplaceTess() {
	if (!DisplaceQueried) {
		DisplaceQueried = true;
		ID3D9NvrExt* ext = DxvkCompute::Extension();
		if (ext && SUCCEEDED(ext->QueryInterface(__uuidof(ID3D9NvrExtTess), (void**)&DisplaceInterface)) && DisplaceInterface)
			Logger::Log("UNOFFICIAL Vegas Shaders DXVK:NvrDxvkDisplacement: this d3d9.dll has the tessellation interface (1.7-displace); the land's shaders are registered as they load");
		else {
			DisplaceInterface = nullptr;
			Logger::Log("UNOFFICIAL Vegas Shaders DXVK:NvrDxvkDisplacement is on but this d3d9.dll has no tessellation interface (needs Vegas Shaders DXVK 1.7-displace): the land stays as it is");
		}
	}
	return DisplaceInterface;	// kept for the session (a device reference, like the extension itself)
}

bool DxvkCompute::DisplaceOn() {
	return DisplaceSwitch() && Ext != nullptr && DisplaceInterface != nullptr
		&& DisplaceRegistered[0] && DisplaceRegistered[1];
}

// 2026-10-28 objects: a define in the shader's list (Template.Defines, after ShaderRecord::BuildDefines)
static const char* DisplaceDefine(const D3DXMACRO* Defines, const char* Name) {
	for (const D3DXMACRO* m = Defines; m && m->Name; m++)
		if (!strcmp(m->Name, Name)) return m->Definition ? m->Definition : "";
	return nullptr;
}

bool DxvkCompute::DisplaceObjectsLayout() {
	// UNOFFICIAL 2026-10-28 CapturedNormals (core/CapturedNormals.h): the layout has no channel left for the smooth normal, so every
	// parallax rock and cliff wrote none and kept the depth-rebuilt, per-triangle one (user: snow in shards on NVN_RockCanyon08, a
	// parallax mesh, with object displacement off). With captured normals compiled in, the layout is used only when object displacement
	// ([Shaders.POM.Displacement] Enabled) is on at startup; turning it on later then needs a restart.
	if (CapturedNormals::CompiledIn() && !TheSettingManager->GetSettingI("Shaders.POM.Displacement", "Enabled")) return false;
	return DisplaceSwitch() && Ext != nullptr;
}

// 2026-10-28 objects (see DxvkCompute.h and docs/next/FORK2-DISPLACE.md "Objects"): ParallaxTemplate's vertex shaders (PAR2000-2019.vso,
// ModelViewProj in c0-c3) with ObjectDisplace.tcs, its pixel shaders with ObjectDisplace-h<HeightMap's sampler>.tes. The DIFFUSE lamp
// passes write TEXCOORD2 instead of COLOR0 (their light's attenuation; vs_3_0 has room for 10 outputs): the -c shaders. Only shaders
// compiled in the displacement layout (NVR_DISPLACE_OBJECTS); a pixel shader whose height map is elsewhere stays untessellated (logged).
static unsigned RegisterDisplaceObject(ID3D9NvrExtTess* Tess, const char* Name, const D3DXMACRO* Defines, bool Vertex, const void* Bytecode, UINT Size) {
	const char* layout = DisplaceDefine(Defines, "NVR_DISPLACE_OBJECTS");
	if (!layout || strcmp(layout, "1")) return 0;
	const bool diffuse = DisplaceDefine(Defines, "DIFFUSE") != nullptr;

	if (Vertex) {
		std::vector<char> tcs;
		if (!DxvkCompute::LoadSpirv(diffuse ? "Tess\\ObjectDisplace-c.tcs" : "Tess\\ObjectDisplace.tcs", tcs)) return 0;
		NVR_EXT_TESS_VS_DESC desc = {};
		desc.pBytecode = Bytecode;
		desc.BytecodeSize = Size;
		desc.Family = DxvkCompute::kDisplaceObjects;
		desc.pControlSpirv = tcs.data();
		desc.ControlSpirvSize = (UINT)tcs.size();
		for (UINT k = 0; k < 4; k++) desc.VsFloatRegisters[k] = k;	// ModelViewProj's rows, c0-c3
		desc.pDebugName = Name;
		const HRESULT result = Tess->RegisterTessVertexShader(&desc);
		if (SUCCEEDED(result)) DisplaceObjRegistered[0]++;
		else Logger::Log("UNOFFICIAL Vegas Shaders DXVK:displacement: object vertex shader %s refused (%08lX; DXVK's log says why): its objects stay flat", Name, result);
		return SUCCEEDED(result) ? DxvkCompute::kDisplaceObjects : 0;
	}

	// The height map's sampler, from the bytecode's constant table (s3 in the main pass, s2 in the others)
	int slot = -1;
	{
		ID3DXConstantTable* table = nullptr;
		if (SUCCEEDED(D3DXGetShaderConstantTableEx((const DWORD*)Bytecode, D3DXCONSTTABLE_LARGEADDRESSAWARE, &table)) && table) {
			D3DXHANDLE handle = table->GetConstantByName(NULL, "HeightMap");
			if (handle) slot = (int)table->GetSamplerIndex(handle);
			table->Release();
		}
	}
	char cw[64], ccw[64];
	_snprintf_s(cw, sizeof(cw), _TRUNCATE, "Tess\\ObjectDisplace-h%d%s.tes", slot, diffuse ? "-c" : "");
	_snprintf_s(ccw, sizeof(ccw), _TRUNCATE, "Tess\\ObjectDisplace-h%d%s-ccw.tes", slot, diffuse ? "-c" : "");
	std::vector<char> tes, tesCcw;
	if (slot < 2 || slot > 3 || !DxvkCompute::LoadSpirv(cw, tes)) {
		DisplaceObjSkipped++;
		Logger::Log("UNOFFICIAL Vegas Shaders DXVK:displacement: object pixel shader %s: height map at s%d, no %s.spv: drawn flat (its object's other passes may not be)", Name, slot, cw);
		return 0;
	}
	const bool haveCcw = DxvkCompute::LoadSpirv(ccw, tesCcw);

	NVR_EXT_TESS_PS_DESC desc = {};
	desc.pBytecode = Bytecode;
	desc.BytecodeSize = Size;
	desc.Family = DxvkCompute::kDisplaceObjects;
	desc.pEvalSpirv[0] = tes.data();
	desc.EvalSpirvSize[0] = (UINT)tes.size();
	desc.pEvalSpirv[1] = haveCcw ? tesCcw.data() : nullptr;
	desc.EvalSpirvSize[1] = haveCcw ? (UINT)tesCcw.size() : 0;
	desc.TextureMask = 1u << slot;			// HeightMap
	desc.PsFloatRegisters[0] = 0xFFFFFFFF;	// every parallax object has its height map
	desc.PsFloatRegisters[1] = 0xFFFFFFFF;
	desc.pDebugName = Name;
	const HRESULT result = Tess->RegisterTessPixelShader(&desc);
	if (SUCCEEDED(result)) DisplaceObjRegistered[1]++;
	else {
		DisplaceObjSkipped++;
		Logger::Log("UNOFFICIAL Vegas Shaders DXVK:displacement: object pixel shader %s refused (%08lX; DXVK's log says why): drawn flat", Name, result);
	}
	return SUCCEEDED(result) ? DxvkCompute::kDisplaceObjects : 0;
}

unsigned DxvkCompute::RegisterDisplace(const char* Name, const char* TemplateName, const D3DXMACRO* Defines, bool Vertex, const void* Bytecode, UINT Size) {
	if (!Name || !Bytecode || !Size) return 0;
	// 2026-10-28: the parallax objects (family 3)
	const bool object = TemplateName && !strcmp(TemplateName, "ParallaxTemplate");
	// 2026-10-27 CastShadows: the land's shadow casters (Shadows\ShadowMapLand.vso, ShadowMapLand-t<N>.pso from ShadowMapLand.pso.hlsl)
	const bool shadow = Vertex ? !strcmp(Name, "ShadowMapLand.vso") : TemplateName && !strcmp(TemplateName, "ShadowMapLand.pso");
	if (!shadow && !object && (!TemplateName || strcmp(TemplateName, "TerrainTemplate"))) return 0;
	if (!DisplaceSwitch() || !Ext) return 0;	// off at load (or native D3D9 / stock DXVK): nothing at all
	ID3D9NvrExtTess* tess = DisplaceTess();
	if (!tess) return 0;
	if (object) return RegisterDisplaceObject(tess, Name, Defines, Vertex, Bytecode, Size);

	const UINT family = shadow ? kDisplaceLandShadow : kDisplaceLand;	// the land drawn by the camera / into the sun's shadow maps
	if (Vertex) {
		std::vector<char> tcs;
		if (!LoadSpirv("Tess\\TerrainDisplace.tcs", tcs)) return 0;
		NVR_EXT_TESS_VS_DESC desc = {};
		desc.pBytecode = Bytecode;
		desc.BytecodeSize = Size;
		desc.Family = family;
		desc.pControlSpirv = tcs.data();
		desc.ControlSpirvSize = (UINT)tcs.size();
		for (UINT k = 0; k < 4; k++) desc.VsFloatRegisters[k] = k;	// ModelViewProj's rows, c0-c3 (the shadow casters': last frame's main camera)
		desc.pDebugName = Name;
		const HRESULT result = tess->RegisterTessVertexShader(&desc);
		if (SUCCEEDED(result)) { if (shadow) DisplaceShadowVs++; else DisplaceRegistered[0]++; }
		Logger::Log("UNOFFICIAL Vegas Shaders DXVK:displacement: land %svertex shader %s %s (%08lX)", shadow ? "shadow caster " : "", Name,
			SUCCEEDED(result) ? "registered" : "refused (DXVK's log says why)", result);
		return SUCCEEDED(result) ? family : 0;
	}

	int layers = 0;
	for (const D3DXMACRO* m = Defines; m && m->Name; m++)
		if (!strcmp(m->Name, "TEX_COUNT") && m->Definition) layers = atoi(m->Definition);
	if (layers < 1 || layers > 7) return 0;	// not one of the land's lit pixel shaders

	// 2026-10-27 ObjectBlend: a land pixel shader compiled with the scene's depth copy at s6 (TESR_DisplaceDepth, below 7 layers with
	// NVR_DISPLACE_DEPTH) gets the TES that reads it (-d); any other (7 layers, or a .spv missing) the plain one, which keeps its bumps.
	// Checked on the bytecode itself: DXVK refuses a TextureMask bit the pixel shader does not use, and the land would stay flat.
	// The shadow casters: last frame's depth copy at s6 (DisplaceDepth, below 7 layers), TES -sd; else -s.
	bool depthAtS6 = false;
	{
		ID3DXConstantTable* table = nullptr;
		if (SUCCEEDED(D3DXGetShaderConstantTableEx((const DWORD*)Bytecode, D3DXCONSTTABLE_LARGEADDRESSAWARE, &table)) && table) {	// as ShaderRecord does
			D3DXHANDLE handle = table->GetConstantByName(NULL, shadow ? "DisplaceDepth" : "TESR_DisplaceDepth");
			depthAtS6 = handle && table->GetSamplerIndex(handle) == 6;
			table->Release();
		}
	}

	char cw[64], ccw[64];
	std::vector<char> tes, tesCcw;
	bool haveCcw = false;
	const char* kind = shadow ? "s" : "";
	if (depthAtS6) {
		_snprintf_s(cw, sizeof(cw), _TRUNCATE, "Tess\\TerrainDisplace-t%d-%sd.tes", layers, kind);
		_snprintf_s(ccw, sizeof(ccw), _TRUNCATE, "Tess\\TerrainDisplace-t%d-%sd-ccw.tes", layers, kind);
		if (LoadSpirv(cw, tes)) haveCcw = LoadSpirv(ccw, tesCcw);
		else {
			depthAtS6 = false;
			Logger::Log("UNOFFICIAL Vegas Shaders DXVK:displacement: %s: no %s.spv (copy the new Compute\\Tess shaders): ObjectBlend off for this land", Name, cw);
		}
	}
	if (!depthAtS6) {
		if (shadow) {
			_snprintf_s(cw, sizeof(cw), _TRUNCATE, "Tess\\TerrainDisplace-t%d-s.tes", layers);
			_snprintf_s(ccw, sizeof(ccw), _TRUNCATE, "Tess\\TerrainDisplace-t%d-s-ccw.tes", layers);
		}
		else {
			_snprintf_s(cw, sizeof(cw), _TRUNCATE, "Tess\\TerrainDisplace-t%d.tes", layers);
			_snprintf_s(ccw, sizeof(ccw), _TRUNCATE, "Tess\\TerrainDisplace-t%d-ccw.tes", layers);
		}
		if (!LoadSpirv(cw, tes)) {
			if (shadow) Logger::Log("UNOFFICIAL Vegas Shaders DXVK:displacement: %s: no %s.spv (copy the new Compute\\Tess shaders): %d-layer land casts flat shadows", Name, cw, layers);
			return 0;
		}
		haveCcw = LoadSpirv(ccw, tesCcw);
	}

	NVR_EXT_TESS_PS_DESC desc = {};
	desc.pBytecode = Bytecode;
	desc.BytecodeSize = Size;
	desc.Family = family;
	desc.pEvalSpirv[0] = tes.data();
	desc.EvalSpirvSize[0] = (UINT)tes.size();
	desc.pEvalSpirv[1] = haveCcw ? tesCcw.data() : nullptr;
	desc.EvalSpirvSize[1] = haveCcw ? (UINT)tesCcw.size() : 0;
	desc.TextureMask = (1u << layers) - 1u;	// BaseMap[0..layers-1] (s0..)
	if (depthAtS6) desc.TextureMask |= 1u << 6;	// TESR_DisplaceDepth / DisplaceDepth (ObjectBlend)
	desc.PsFloatRegisters[0] = 34;			// LandHeight[0] (layers 0-3; the shadow casters get the land's own, DisplaceShadows.h)
	desc.PsFloatRegisters[1] = 35;			// LandHeight[1] (layers 4-6)
	desc.pDebugName = Name;
	const HRESULT result = tess->RegisterTessPixelShader(&desc);
	if (SUCCEEDED(result)) {
		if (shadow) DisplaceShadowPs |= 1u << (layers - 1);
		else {
			DisplaceRegistered[1]++;
			if (depthAtS6) DisplaceDepthShaders++;
		}
	}
	else Logger::Log("UNOFFICIAL Vegas Shaders DXVK:displacement: land %spixel shader %s refused (%08lX; DXVK's log says why): its land stays flat%s", shadow ? "shadow caster " : "",
		Name, result, shadow ? " in the sun's shadow maps" : "");
	return SUCCEEDED(result) ? family : 0;
}

void DxvkCompute::NoteLandPixelShader(IDirect3DPixelShader9* Shader, const D3DXMACRO* Defines) {
	int layers = 0;
	for (const D3DXMACRO* m = Defines; m && m->Name; m++)
		if (!strcmp(m->Name, "TEX_COUNT") && m->Definition) layers = atoi(m->Definition);
	if (Shader && layers >= 1 && layers <= 7) LandPixelShaders.emplace_back(Shader, layers);
}

int DxvkCompute::LandLayersOf(IDirect3DPixelShader9* Shader) {
	static IDirect3DPixelShader9* last = nullptr;
	static int lastLayers = 0;
	if (Shader == last) return lastLayers;
	last = Shader;
	lastLayers = 0;
	for (const auto& entry : LandPixelShaders) if (entry.first == Shader) { lastLayers = entry.second; break; }
	return lastLayers;
}

bool DxvkCompute::DisplaceShadowsWanted() {
	return DisplaceCastShadows && DisplaceOn() && DisplaceBase.Enable;
}

unsigned DxvkCompute::DisplaceShadowLayers() {
	return DisplaceShadowVs ? DisplaceShadowPs : 0;
}

// ObjectBlendShadows: the sun cascades flatten the land near objects too, from last frame's depth copy (as before 2026-10-28). Off: they
// hold the full bumps, the same every frame (the copy is jittered and a frame late, and the far cascades a few frames old: near
// objects the maps' land and the screen's differed by a different amount every frame, and the shadows on it shimmered).
bool DxvkCompute::DisplaceBlendInShadows() {
	return DisplaceBlendShadows;
}

float DxvkCompute::DisplaceReach() {
	return DisplaceBase.ControlParams[4];
}

void DxvkCompute::DisplaceShadowBegin(bool ObjectBlend, float ScaleX, float ScaleY) {
	if (!DisplaceInterface) return;
	DisplaceFar = false;	// 2026-10-28: the shadow pass takes only land within FadeEnd (DisplaceShadows::Takes)
	NVR_EXT_TESS_STATE state = DisplaceBase;
	state.Wireframe = 0;	// DebugView 1 is the camera's view of the land; the maps keep solid land
	state.ControlParams[6] = ObjectBlend ? DisplaceObjectBlend : 0.0f;
	state.EvalParams[3] = ObjectBlend ? DisplacePackScale(ScaleX, ScaleY) : DisplacePackScale(1.0f, 1.0f);
	DisplaceSend(state);
}

void DxvkCompute::DisplaceShadowEnd() {
	if (DisplaceInterface) DisplaceSend(DisplaceBase);
}

void DxvkCompute::DisplaceShadowsDrawn(unsigned Pieces) {
	if (Pieces) DisplaceShadowAge = 0;
}

// The screen-space contact shadows march the depth buffer toward the sun, and on tessellated land every bump blocks that march: crusty
// little shadows on the bumps (they already cast their real ones through the sun's maps, CastShadows). With NearFade they started 3-6 m
// from the camera, a dark ring that followed the player. Here they come in only where the bumps have gone, round FadeEnd (a quarter of
// the fade band either side of it), so no bump casts one and no ring shows.
bool DxvkCompute::DisplaceContactFade(float& Start, float& End) {
	if (!DisplaceContactOutside || !DisplaceInterface || !DisplaceRegistered[0] || !DisplaceRegistered[1] || !DisplaceBase.Enable
		|| !(DisplaceBase.EvalParams[0] > 0.0f)) return false;
	const float fadeStart = DisplaceBase.ControlParams[3], fadeEnd = DisplaceBase.ControlParams[4];
	const float quarter = 0.25f * (std::max)(fadeEnd - fadeStart, 1.0f);
	Start = (std::max)(fadeEnd - quarter, 0.0f);
	End = fadeEnd + quarter;
	return true;
}

// UNOFFICIAL 2026-11-07 (rtgap-tess) the bumps the RT scene lacks: the largest Strength on and the widest fade (docs/next/RT-DETAIL-GAPS.md 1a)
bool DxvkCompute::DisplaceRtSlack(float Out[3]) {
	Out[0] = Out[1] = Out[2] = 0.0f;
	if (!DisplaceInterface || !DisplaceSettingOr("TessStartSlack", true, "Main.Main.RayTracing")) return false;
	bool any = false;
	// (2026-11-07 merge of rtgap-tess + rtgap-tessblas) a family whose bumps the RT scene carries as displaced copies (RtCore::Displace)
	// needs no Strength-sized start: the depth's point lies on the traced bumps (0.23 units mean). A small 2 units stays for the copies'
	// split error and the frames a new copy waits for its build budget (traced flat until then).
	bool copiedLand = false, copiedObjects = false;
	RtCore::DisplacedCopies(copiedLand, copiedObjects);
	auto take = [&](const NVR_EXT_TESS_STATE& s, bool copied) {
		if (!s.Enable || !(s.EvalParams[0] > 0.0f)) return;
		const float strength = copied ? (std::min)(s.EvalParams[0], 2.0f) : s.EvalParams[0];
		Out[1] = any ? (std::min)(Out[1], s.ControlParams[3]) : s.ControlParams[3];
		Out[2] = (std::max)(Out[2], s.ControlParams[4]);
		Out[0] = (std::max)(Out[0], strength);
		any = true;
	};
	if (DisplaceRegistered[0] && DisplaceRegistered[1]) take(DisplaceBase, copiedLand);
	if (DisplaceObjRegistered[0] && DisplaceObjRegistered[1]) take(DisplaceObj, copiedObjects);
	if (!any) Out[0] = Out[1] = Out[2] = 0.0f;
	return any;
}

// IgnoreMipBias: TAA / FSR / DLSS put a texture mip bias on every sampler while the world draws (TAAEffect::SetMipBias: MipBias, and
// log2 of RenderScale), and the tessellation's height lookups (explicit detail level) take the sampler's bias too: sharper bumps with
// jitter on or a lower RenderScale (tester: "turning off jitter seems to shrink tessellation"). Added back here, so the lookups stay
// at the detail level the point spacing asks for. Last frame's bias (the same unless the TAA settings changed).
static float DisplaceMipCompensation() {
	if (!DisplaceIgnoreMipBias) return 0.0f;
	TAAEffect* taa = TheShaderManager ? TheShaderManager->Effects.TAA : nullptr;
	return taa ? std::clamp(-taa->WorldMipBias(), 0.0f, 3.0f) : 0.0f;
}

bool DxvkCompute::DisplaceDepthWanted() {
	return DisplaceInterface && DisplaceRegistered[0] && DisplaceDepthShaders && DisplaceBase.Enable && DisplaceObjectBlend > 0.0f;
}

void DxvkCompute::DisplaceSceneDepth(bool Valid, float ScaleX, float ScaleY) {
	if (!DisplaceInterface || !DisplaceRegistered[0] || !DisplaceRegistered[1]) return;
	NVR_EXT_TESS_STATE state = DisplaceBase;
	if (Valid && DisplaceObjectBlend > 0.0f) {
		state.ControlParams[6] = DisplaceObjectBlend;
		state.EvalParams[3] = DisplacePackScale(ScaleX, ScaleY);
	}
	DisplaceSend(state);
}

void DxvkCompute::DisplaceBind(unsigned Family) {
	if (!DisplaceInterface) return;
	if (Family == kDisplaceObjects) {
		DisplaceObjBinds++;
		DisplaceSend(DisplaceObj, kDisplaceObjects);
	}
	// The land: back to its base state after the objects' (a land shader with ObjectBlend's s6 then refines it, DisplaceSceneDepth)
	else if (Family == kDisplaceLand && DisplaceSentFamily == kDisplaceObjects) DisplaceSend(DisplaceBase);
}

void DxvkCompute::DisplaceLandPass(NiGeometry* Geometry, UInt16 PassEnum, int Context) {
	bool pieceFar = false;
	// The land passes drawn with SLS2100.vso (BSSM_LAND<n>O...; 0x93-0x94, BSSM_LANDLO_A, is drawn with SLS2080.vso: never tessellated)
	if (DisplaceBase.Enable && Geometry && PassEnum >= 0x1EF && PassEnum <= 0x230) {
		const int context = std::clamp(Context, 0, 2);
		const NiGeometryBufferData* buffers = Geometry->geomData ? Geometry->geomData->m_pkBuffData : nullptr;
		const unsigned strips = buffers ? (buffers->IB ? buffers->NumArrays : 1u) : 0u;
		DisplaceLandPasses[context]++;
		DisplaceLandStrips[context] += strips;
		// The bound holds the piece, so each of its points is at least this far from the camera horizontally, and so from the eye the
		// TCS measures from: the camera's in the world scene, the camera mirrored below the water (same x and y) in the reflection.
		const NiBound* bound = Geometry->m_kWorldBound;
		if (DisplaceFarOn && context != kDisplaceOther && bound) {
			const D3DXVECTOR4& cam = TheRenderManager->CameraPosition;
			const float dx = bound->Center.x - cam.x, dy = bound->Center.y - cam.y;
			const float reach = DisplaceBase.ControlParams[4] + kDisplaceFarMargin + bound->Radius;
			pieceFar = dx * dx + dy * dy > reach * reach;
			{	// UNOFFICIAL 2026-10-29 FarHysteresis: the piece's last decision, unless clearly past the other way (and the flips counted)
				const UInt32 frame = TheFrameRateManager->FrameNumber;
				auto known = DisplaceFarPieces.find(Geometry);
				if (known != DisplaceFarPieces.end() && frame - known->second.frame <= 2) {
					const float d = sqrtf(dx * dx + dy * dy);
					if (DisplaceFarHysteresis > 0.0f) pieceFar = known->second.isFar ? d > reach - DisplaceFarHysteresis : d > reach + DisplaceFarHysteresis;
					if (pieceFar != known->second.isFar && known->second.frame != frame) DisplaceFarFlips++;
				}
				DisplaceFarPieces[Geometry] = { pieceFar, frame };
				if (DisplaceFarPieces.size() > 8192) {	// pieces long gone (cells unloaded): dropped
					for (auto it = DisplaceFarPieces.begin(); it != DisplaceFarPieces.end();) it = frame - it->second.frame > 600 ? DisplaceFarPieces.erase(it) : std::next(it);
				}
			}
		}
		if (pieceFar) {
			DisplaceFarPasses++;
			DisplaceFarStrips += strips;
		}
	}
	if (pieceFar == DisplaceFar) return;
	DisplaceFar = pieceFar;
	DisplaceFarSwitches++;
	// The land's state is the one in use: sent again, switched off or as it was asked for. After an object's state the land shader's
	// bind sends it (DisplaceBind), and the shadow pass sends its own (DisplaceShadowBegin).
	if (DisplaceInterface && DisplaceSentOnce && DisplaceSentFamily == kDisplaceLand) DisplaceSend(DisplaceWanted);
}

// 2026-10-28 objects: [Shaders.POM.Displacement] into their state (every 8 frames, from DisplaceTick). A missing key (a DLL used with an
// older defaults toml) reads as the shipped default.
static void DisplaceObjectSettings() {
	const char* s = "Shaders.POM.Displacement";
	NVR_EXT_TESS_STATE& state = DisplaceObj;
	state.Enable = DisplaceSwitch() && DisplaceSettingOr("Enabled", false, s) ? TRUE : FALSE;	// off unless switched on
	const int debugView = std::clamp((int)DisplaceFloatOr("DebugView", 0.0f, s), 0, 1);
	state.Winding = (int)DisplaceFloatOr("Winding", 0.0f, s) == 1 ? 1 : 0;
	state.Wireframe = debugView == 1 ? 1 : 0;
	const float strength = std::clamp(DisplaceFloatOr("Strength", 8.0f, s), 0.0f, 64.0f);
	const float fadeStart = (std::max)(DisplaceFloatOr("FadeStart", 1024.0f, s), 0.0f);
	// ObjectDisplace.tesc push data
	state.ControlParams[0] = (float)std::clamp((int)DisplaceFloatOr("MaxFactor", 16.0f, s), 1, 64)
		+ 128.0f * (float)std::clamp((int)DisplaceFloatOr("FixedFactor", 0.0f, s), 0, 64);
	state.ControlParams[1] = std::clamp(DisplaceFloatOr("EdgePixels", 12.0f, s), 2.0f, 64.0f);
	state.ControlParams[2] = 0.5f * (float)TheRenderManager->width;
	state.ControlParams[3] = fadeStart;
	state.ControlParams[4] = (std::max)(DisplaceFloatOr("FadeEnd", 2048.0f, s), fadeStart + 1.0f);
	state.ControlParams[5] = 0.5f * (float)TheRenderManager->height;
	// The view test's margin: the most the surface can move (Strength, with Center at an end) times a clip-space slope above any field of
	// view the game uses, scopes included (x moves by up to Strength x (ModelViewProj's x scale + 1); about 12 at a 10 degree scope)
	state.ControlParams[6] = 16.0f * strength + 1.0f;
	state.ControlParams[7] = 0.0f;
	// ObjectDisplace.tese push data
	state.EvalParams[0] = strength;
	state.EvalParams[1] = floorf(std::clamp(DisplaceFloatOr("Contrast", 2.0f, s), 0.0f, 32.0f) * 16.0f + 0.5f)
		+ 0.99f * std::clamp(DisplaceFloatOr("Center", 0.5f, s), 0.0f, 1.0f);
	state.EvalParams[2] = std::clamp(DisplaceFloatOr("LodBias", 0.0f, s), -2.0f, 4.0f) + 4.0f + DisplaceMipCompensation();	// (IgnoreMipBias, the land's setting)
	state.EvalParams[3] = std::clamp(DisplaceFloatOr("EdgeFade", 1.5f, s), 0.0f, 8.0f);
}

// UNOFFICIAL 2026-11-07 (rtgap-tessblas) RtCore::Displace: whether the game draws the bumps (so the RT scene should have them too)
bool DxvkCompute::DisplaceLandActive() {
	return DisplaceInterface && DisplaceRegistered[0] && DisplaceRegistered[1] && DisplaceBase.Enable;
}

bool DxvkCompute::DisplaceObjectsActive() {
	return DisplaceInterface && DisplaceObjRegistered[0] && DisplaceObjRegistered[1] && DisplaceObj.Enable;
}

void DxvkCompute::DisplaceTick() {
	if (!DisplaceInterface) return;
	DisplaceFar = false;	// 2026-10-28 far land: each frame starts with the land's state as asked for (the land passes decide again)

	// 2026-10-28 objects: their settings, and once a frame their state again if the objects were the last to bind (their shaders may stay
	// bound into the next frame); proof in the log every 600 frames
	const bool objects = DisplaceObjRegistered[0] && DisplaceObjRegistered[1];
	static unsigned objFrame = 0;
	if (objects) {
		if ((objFrame % 8) == 0) DisplaceObjectSettings();
		if (DisplaceSentFamily == kDisplaceObjects) DisplaceSend(DisplaceObj, kDisplaceObjects);
		if ((objFrame % 600) == 599)
			Logger::Log("NVR TESS objects %s: %u vertex, %u pixel shaders registered (%u left flat), %.1f object shader binds a frame",
				DisplaceObj.Enable ? "on" : "off", DisplaceObjRegistered[0], DisplaceObjRegistered[1], DisplaceObjSkipped, (double)DisplaceObjBinds / 600.0);
		if ((objFrame % 600) == 599) DisplaceObjBinds = 0;
		objFrame++;
	}

	if (!DisplaceRegistered[0] || !DisplaceRegistered[1]) return;

	// The settings, re-read every 8 frames (menu and test-list changes apply within that)
	NVR_EXT_TESS_STATE& state = DisplaceBase;
	static unsigned frame = 0;
	static NVR_EXT_TESS_STATS last = {};
	static float viewParams = 4.0f;	// EvalParams[2] without ShadowsHold (added below, every frame)
	if ((frame++ % 8) == 0) {
		const char* s = "Shaders.Terrain.Displacement";
		// 2026-10-27 (user: "we also need our toggle for tessellation in the menu"): [Shaders.Terrain.Displacement] Enabled switches the
		// bumps at once, in the Tessellation panel; NvrDxvkDisplacement still sets it up at startup.
		state.Enable = DisplaceSwitch() && DisplaceSettingOr("Enabled", false) ? TRUE : FALSE;	// off unless switched on (2026-10-27: not ready to be used)
		const int debugView = std::clamp(TheSettingManager->GetSettingI(s, "DebugView"), 0, 7);
		state.Winding = TheSettingManager->GetSettingI(s, "Winding") == 1 ? 1 : 0;
		state.Wireframe = debugView == 1 ? 1 : 0;
		const float fadeStart = (std::max)(TheSettingManager->GetSettingF(s, "FadeStart"), 0.0f);
		// TerrainDisplace.tesc push data. 2026-10-27: MaxFactor and FixedFactor share [0] (small whole numbers), so [6] can carry
		// ObjectBlend (on for the world scene's land draws only: DisplaceSceneDepth; 0 here).
		state.ControlParams[0] = (float)std::clamp(TheSettingManager->GetSettingI(s, "MaxFactor"), 1, 64)
			+ 128.0f * (float)std::clamp(TheSettingManager->GetSettingI(s, "FixedFactor"), 0, 64);
		state.ControlParams[1] = std::clamp(TheSettingManager->GetSettingF(s, "EdgeLength"), 2.0f, 64.0f);
		state.ControlParams[2] = std::clamp(TheSettingManager->GetSettingF(s, "EdgeGrowth"), 0.0f, 0.1f);
		state.ControlParams[3] = fadeStart;
		state.ControlParams[4] = (std::max)(TheSettingManager->GetSettingF(s, "FadeEnd"), fadeStart + 1.0f);
		state.ControlParams[5] = (std::max)(TheSettingManager->GetSettingF(s, "BorderGrid"), 0.0f);
		state.ControlParams[6] = 0.0f;
		state.ControlParams[7] = 0.0f;
		// TerrainDisplace.tese push data. 2026-10-27: DebugView and LodBias share [2], so [3] can carry the viewport's share of the
		// depth copy (ObjectBlend, per draw; 1 x 1 here).
		state.EvalParams[0] = std::clamp(TheSettingManager->GetSettingF(s, "Strength"), 0.0f, 64.0f);
		// 2026-10-27 (user: the bumps only moved the ground up and down): Contrast shares [1] with Center, 16 x Contrast in the whole
		// part. A missing key (an older defaults toml) reads as the shipped default, not 0 (flat).
		// 2026-10-28 (Conor, untouched settings: a field of spikes; measured on the landscape textures): Contrast 4 by default (was 10).
		state.EvalParams[1] = floorf(std::clamp(DisplaceFloatOr("Contrast", 4.0f), 0.0f, 32.0f) * 16.0f + 0.5f)
			+ 0.99f * std::clamp(DisplaceFloatOr("Center", 0.5f), 0.0f, 1.0f);
		viewParams = 16.0f * (float)debugView + std::clamp(TheSettingManager->GetSettingF(s, "LodBias"), -2.0f, 4.0f) + 4.0f;
		// 2026-10-28 BumpSize: the finest bump in whole game units (+ 512 each): the height lookup never sharper than that, whatever
		// the point spacing, so the bumps follow the textures' broad shapes instead of their grain
		viewParams += 512.0f * floorf(std::clamp(DisplaceFloatOr("BumpSize", 48.0f), 0.0f, 255.0f) + 0.5f);
		// 2026-10-28 BendNormals (plan step 3): the lighting frame tilted by the bumps' slope, on by default; off = + 128 (FlatNormals),
		// so shaders from before it (which never bend) are unchanged unless it is switched off
		if (!DisplaceSettingOr("BendNormals", true)) viewParams += 128.0f;
		state.EvalParams[3] = DisplacePackScale(1.0f, 1.0f);
		// ObjectBlend: the width in whole game units, the share kept at the object as the fraction
		const bool blend = DisplaceSettingOr("ObjectBlend", false);	// off by default (user's test: it also flattens round grass)
		const float width = floorf(std::clamp(TheSettingManager->GetSettingF(s, "ObjectBlendWidth"), 4.0f, 512.0f) + 0.5f);
		const float keep = std::clamp(TheSettingManager->GetSettingF(s, "ObjectBlendKeep"), 0.0f, 1.0f);
		DisplaceObjectBlend = blend && keep < 1.0f ? width + keep * 0.99f : 0.0f;
		DisplaceCastShadows = DisplaceSettingOr("CastShadows", true);	// 2026-10-27 (core/DisplaceShadows.h); on by default
		DisplaceFarOn = DisplaceSettingOr("FarUntessellated", true);	// 2026-10-28 (DisplaceLandPass); on by default
		DisplaceFarHysteresis = std::clamp(DisplaceFloatOr("FarHysteresis", 0.0f), 0.0f, 4096.0f);	// UNOFFICIAL 2026-10-29 (DisplaceLandPass)
		DisplaceContactOutside = DisplaceSettingOr("ContactShadowsOutside", true);	// 2026-10-28 (DisplaceContactFade)
		DisplaceIgnoreMipBias = DisplaceSettingOr("IgnoreMipBias", true);			// 2026-10-28 (DisplaceMipCompensation)
		DisplaceBlendShadows = DisplaceSettingOr("ObjectBlendShadows", false);		// 2026-10-28 (DisplaceBlendInShadows)
		// MapsFull (TerrainDisplace.tese mapsFull): with ObjectBlend on and its shadows off, the maps hold the full bumps
		if (DisplaceObjectBlend > 0.0f && !DisplaceBlendShadows) viewParams += 131072.0f;
	}
	// 2026-10-27 CastShadows: ShadowsHold (+ 256) while the sun's shadow maps hold displaced land (drawn into them within the last 16
	// frames: the far cascades are redrawn only every few frames), so the land's forward shadow lookup follows the displaced surface,
	// below the flat land too. Otherwise it never goes below the flat land (those maps hold the flat land).
	if (DisplaceShadowAge < 1000) DisplaceShadowAge++;
	const bool shadowsHold = DisplaceCastShadows && DisplaceShadowVs && DisplaceShadowAge <= 16;
	// 2026-10-28 IgnoreMipBias: the world's texture mip bias added to LodBias's part (at most 3 on top of LodBias + 4 <= 8: stays below
	// DebugView's 16s)
	state.EvalParams[2] = viewParams + DisplaceMipCompensation() + (shadowsHold ? 256.0f : 0.0f);
	// Once a frame the base (ObjectBlend off: the first land draws of a frame are the water's reflection, whose view the copy is not);
	// the world scene's land binds then switch it on (DisplaceSceneDepth). SetTessState only when it changes. 2026-10-28: not while the
	// objects' state is the one in use (their shaders may still be bound); the next land bind sends this (DisplaceBind).
	if (DisplaceSentFamily != kDisplaceObjects) DisplaceSend(state);

	// Proof in the log that it runs: every 600 ticks, per frame, how many land draws ran tessellated. 2026-10-28: per RENDERED frame
	// (FrameNumber; this tick runs twice a frame, from both ShaderManager::UpdateConstants calls, so the old divisor of 600 gave half
	// the real counts), and where the land draws come from (DisplaceLandPass): the land passes of the world scene, the water's
	// reflection and anything else, the strip draws they make (the fork counts each strip draw), and how many went untessellated.
	if ((frame % 600) == 1) {
		static UINT lastFrameNumber = 0;
		const UINT frames = TheFrameRateManager->FrameNumber - lastFrameNumber;
		lastFrameNumber = TheFrameRateManager->FrameNumber;
		NVR_EXT_TESS_STATS now = {};
		if (frames && SUCCEEDED(DisplaceInterface->GetTessStats(&now))) {
			const double f = (double)frames;
			Logger::Log("NVR TESS %s: per frame %.1f land draws tessellated (lists and strips), %.1f left as they are (fans, UP draws: the DXVK log names the type), %.1f rebinds | land passes (strip draws): world %.1f (%.1f), water reflection %.1f (%.1f), other %.1f (%.1f) | far land %s: %.1f passes (%.1f strip draws) untessellated (wholly past FadeEnd + %.0f), %.1f near/far switches, %.2f pieces flipped since the frame before (FarHysteresis %.0f) | shaders %u VS, %u PS, %u refused by the interface check | ObjectBlend %s (%u of the %u land pixel shaders can: below 7 layers)",
				state.Enable ? "on" : "off", (double)(now.TessellatedDraws - last.TessellatedDraws) / f, (double)(now.OtherTopologyDraws - last.OtherTopologyDraws) / f,
				(double)(now.Bindings - last.Bindings) / f,
				DisplaceLandPasses[kDisplaceWorld] / f, DisplaceLandStrips[kDisplaceWorld] / f, DisplaceLandPasses[kDisplaceReflection] / f,
				DisplaceLandStrips[kDisplaceReflection] / f, DisplaceLandPasses[kDisplaceOther] / f, DisplaceLandStrips[kDisplaceOther] / f,
				DisplaceFarOn ? "on" : "off", DisplaceFarPasses / f, DisplaceFarStrips / f, kDisplaceFarMargin, DisplaceFarSwitches / f, DisplaceFarFlips / f, DisplaceFarHysteresis,
				now.VertexShaders, now.PixelShaders, now.RefusedPairs, DisplaceObjectBlend > 0.0f ? "on" : "off", DisplaceDepthShaders, DisplaceRegistered[1]);
			last = now;
		}
		for (int c = 0; c < 3; c++) DisplaceLandPasses[c] = DisplaceLandStrips[c] = 0;
		DisplaceFarPasses = DisplaceFarStrips = DisplaceFarSwitches = DisplaceFarFlips = 0;
	}
}

