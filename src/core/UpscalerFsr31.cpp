// UNOFFICIAL 2026-10-28 (NVR DXVK, [Shaders.TAA.FSR] Version 1): AMD's FSR 3.1 upscaler behind the Upscaler interface
// (core\Upscaler.h), next to FSR 2.2.1 (core\Upscaler.cpp). Upscaling only: FSR 3's frame generation is not part of this.
//
// CREDIT: FSR 3.1 is AMD's (FidelityFX Super Resolution 3.1.x upscaler, AMD FidelityFX SDK 1.1.4, GPUOpen,
// https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK), MIT licence: src\hlsl\NewVegas\Compute\FSR31\LICENSE-FidelityFX-SDK.txt.
// The shaders are AMD's GLSL passes as released, built to SPIR-V with NVR's callbacks header
// (FSR31\fsr3upscaler\ffx_fsr3upscaler_callbacks_nvr.h says what it changes and why); this file does what AMD's
// ffx_fsr3upscaler.cpp does on the CPU side (constants, ping-pong resources, clears, dispatch order), written for NVR's DXVK
// extension, as core\Upscaler.cpp does for FSR 2.
//
// Why no 64-bit helper (as DLSS needs): AMD's prebuilt SDK DLLs are 64-bit, but FSR is open source and its GPU half is plain
// GLSL. The game's process only records compute dispatches through the fork's extension (32-bit host code, GPU shaders), exactly
// like FSR 2. Nothing of AMD's runtime is needed.
//
// Same inputs as FSR 2 (Upscaler::Inputs: jittered colour, depth with the reactive mask in green, camera motion vectors, the jitter):
// FSR 3.1's jitter sequence is FSR 2's (Halton 2/3, 8 x (display / render)^2 steps: ffxFsr3UpscalerGetJitterOffset /
// GetJitterPhaseCount are the same functions), so the callers' jitter is used as it is; its sharpening is the same RCAS.
//
// Per Evaluate, in ffx_fsr3upscaler.cpp's order:
//   FSR31PrepareInputs, FSR31LumaPyramid, FSR31ShadingChangePyramid, FSR31ShadingChange, FSR31PrepareReactivity,
//   FSR31LumaInstability, FSR31Accumulate[Sharpen][Hdr], FSR31RCAS (Sharpness > 0)

#include <algorithm>
#include <cmath>

#include "Upscaler.h"
#include "DxvkCompute.h"
#include "GpuProfiler.h"

namespace {
	// cbFSR3Upscaler in ffx_fsr3upscaler_callbacks_nvr.h (std140; the same layout as AMD's Fsr3UpscalerConstants).
	struct FSR3Constants {
		INT32	renderSize[2];
		INT32	previousFrameRenderSize[2];
		INT32	upscaleSize[2];
		INT32	previousFrameUpscaleSize[2];
		INT32	maxRenderSize[2];
		INT32	maxUpscaleSize[2];
		float	deviceToViewDepth[4];
		float	jitterOffset[2];
		float	previousFrameJitterOffset[2];
		float	motionVectorScale[2];
		float	downscaleFactor[2];
		float	motionVectorJitterCancellation[2];
		float	tanHalfFOV;
		float	jitterPhaseCount;
		float	deltaTime;
		float	deltaPreExposure;
		float	viewSpaceToMetersFactor;
		float	frameIndex;
		float	velocityFactor;
		float	reactivenessScale;
		float	shadingChangeScale;
		float	accumulationAddedPerFrame;
		float	minDisocclusionAccumulation;
		float	pad[3];
	};
	static_assert(sizeof(FSR3Constants) == 160, "cbFSR3Upscaler layout");

	// The push block of every pass in NVR's build (ffx_fsr3upscaler_callbacks_nvr.h NvrFsr3Push_t): the three sampler heap indices
	// (the extension writes them), RCAS's constants (AMD's cbRCAS) and SPD's (AMD's cbSPD).
	struct FSR3Push {
		UInt32	pointIndex, linearIndex, linearMipsIndex, pad0;
		UInt32	rcas[4];
		UInt32	spdMips, spdNumWorkGroups, spdWorkGroupOffset[2], spdRenderSize[2], pad1[2];
	};
	static_assert(sizeof(FSR3Push) == 64, "NvrFsr3Push_t layout");

	// Slots. Images: slot = binding. Buffers: each its own slot number (a uniform and a storage buffer with the same slot number are
	// the same DXVK binding: nvr-dxvkcompute 2026-10-27 gotcha). Samplers: POINT (slot 0, push 0), LINEAR (1, 4), LINEAR MIPS (2, 8).
	enum { SlotConstants = 0, SlotReconstructedDepth = 1, SlotSpdAtomic = 2, SlotFrameInfo = 3 };

	// Binding tables: (set, binding) as the SPIR-V declares them (checked with spirv-dis: bindings the optimiser removed are not listed).
#define IMG(b)      { 1, b, NVR_EXT_BIND_SAMPLED_IMAGE, b, NVR_EXT_VIEW_2D, FALSE, 0 }
#define UAV(b)      { 1, b, NVR_EXT_BIND_STORAGE_IMAGE, b, NVR_EXT_VIEW_2D, TRUE, 0 }
#define CB(b)       { 1, b, NVR_EXT_BIND_UNIFORM_BUFFER, SlotConstants, NVR_EXT_VIEW_2D, FALSE, 0 }
#define BUF(b, s, w) { 1, b, NVR_EXT_BIND_STORAGE_BUFFER, s, NVR_EXT_VIEW_2D, w, 0 }
#define SAMPLERS    { 0, 0, NVR_EXT_BIND_SAMPLER, 0, NVR_EXT_VIEW_2D, FALSE, 0 }, { 0, 0, NVR_EXT_BIND_SAMPLER, 1, NVR_EXT_VIEW_2D, FALSE, 4 }, \
                    { 0, 0, NVR_EXT_BIND_SAMPLER, 2, NVR_EXT_VIEW_2D, FALSE, 8 }
	const NVR_EXT_BINDING_DESC PrepareInputsBindings[] = { IMG(0), IMG(1), IMG(2), UAV(3), UAV(4), BUF(5, SlotReconstructedDepth, TRUE), UAV(6), UAV(7), CB(8) };
	const NVR_EXT_BINDING_DESC LumaPyramidBindings[] = { IMG(0), IMG(1), BUF(2, SlotSpdAtomic, TRUE), BUF(3, SlotFrameInfo, TRUE), UAV(9), UAV(10), CB(11) };
	const NVR_EXT_BINDING_DESC ShadingChangePyramidBindings[] = { IMG(0), IMG(1), IMG(2), BUF(4, SlotSpdAtomic, TRUE), UAV(5), UAV(6), UAV(7), UAV(8), UAV(9), UAV(10), CB(11) };
	const NVR_EXT_BINDING_DESC ShadingChangeBindings[] = { IMG(0), UAV(1), CB(2), SAMPLERS };
	const NVR_EXT_BINDING_DESC PrepareReactivityBindings[] = { BUF(0, SlotReconstructedDepth, FALSE), IMG(1), IMG(2), IMG(3), IMG(5), IMG(6), IMG(7), UAV(9), UAV(10), UAV(11), CB(12), SAMPLERS };
	const NVR_EXT_BINDING_DESC LumaInstabilityBindings[] = { IMG(1), IMG(2), IMG(4), IMG(6), UAV(7), UAV(8), CB(9), SAMPLERS };
	const NVR_EXT_BINDING_DESC AccumulateBindings[] = { IMG(1), IMG(2), IMG(3), IMG(5), IMG(7), IMG(8), UAV(9), UAV(10), UAV(11), CB(12), SAMPLERS };
	const NVR_EXT_BINDING_DESC AccumulateSharpenBindings[] = { IMG(1), IMG(2), IMG(3), IMG(5), IMG(7), IMG(8), UAV(9), UAV(11), CB(12), SAMPLERS };
	const NVR_EXT_BINDING_DESC RCASBindings[] = { IMG(1), UAV(2) };
#undef IMG
#undef UAV
#undef CB
#undef BUF
#undef SAMPLERS

	enum { PrepareInputs, LumaPyramid, ShadingChangePyramid, ShadingChange, PrepareReactivity, LumaInstability, Accumulate, AccumulateSharpen,
		RCAS, PipelineCount };
	struct PipelineInfo { const char* name; const char* hdrName; const NVR_EXT_BINDING_DESC* bindings; UINT count; bool samplers; };
	const PipelineInfo Pipelines[PipelineCount] = {
		{ "FSR31PrepareInputs",        nullptr,                     PrepareInputsBindings,        _countof(PrepareInputsBindings),        false },
		{ "FSR31LumaPyramid",          nullptr,                     LumaPyramidBindings,          _countof(LumaPyramidBindings),          false },
		{ "FSR31ShadingChangePyramid", nullptr,                     ShadingChangePyramidBindings, _countof(ShadingChangePyramidBindings), false },
		{ "FSR31ShadingChange",        nullptr,                     ShadingChangeBindings,        _countof(ShadingChangeBindings),        true  },
		{ "FSR31PrepareReactivity",    nullptr,                     PrepareReactivityBindings,    _countof(PrepareReactivityBindings),    true  },
		{ "FSR31LumaInstability",      nullptr,                     LumaInstabilityBindings,      _countof(LumaInstabilityBindings),      true  },
		{ "FSR31Accumulate",           "FSR31AccumulateHdr",        AccumulateBindings,           _countof(AccumulateBindings),           true  },
		{ "FSR31AccumulateSharpen",    "FSR31AccumulateSharpenHdr", AccumulateSharpenBindings,    _countof(AccumulateSharpenBindings),    true  },
		{ "FSR31RCAS",                 nullptr,                     RCASBindings,                 _countof(RCASBindings),                 false },
	};
	// FSR31PrepareInputs uses no push data (spirv-dis); every other pass declares the whole block.
	UINT PushSizeOf(int pass) { return pass == PrepareInputs ? 0 : (UINT)sizeof(FSR3Push); }

	NVR_EXT_RESOURCE_BINDING Image(UINT slot, IUnknown* resource, bool storage = false, UINT mip = 0) {
		NVR_EXT_RESOURCE_BINDING b = {};
		b.Slot = slot; b.Type = storage ? NVR_EXT_BIND_STORAGE_IMAGE : NVR_EXT_BIND_SAMPLED_IMAGE; b.pResource = resource; b.MipLevel = mip;
		return b;
	}
	NVR_EXT_RESOURCE_BINDING Buffer(UINT slot, NVR_EXT_BIND_TYPE type, ID3D9NvrExtBuffer* buffer) {
		NVR_EXT_RESOURCE_BINDING b = {};
		b.Slot = slot; b.Type = type; b.pResource = buffer;
		return b;
	}
	NVR_EXT_RESOURCE_BINDING Sampler(UINT slot, ID3D9NvrExtSampler* sampler) {
		NVR_EXT_RESOURCE_BINDING b = {};
		b.Slot = slot; b.Type = NVR_EXT_BIND_SAMPLER; b.pResource = sampler;
		return b;
	}

	// AMD's ffx_fsr1.h FsrRcasCon: the sharpness in stops (0 = maximum), as a float and as two packed halves.
	UInt32 HalfBits(float value) {
		UInt32 f = *(UInt32*)&value;
		UInt32 sign = (f >> 16) & 0x8000u;
		int exponent = (int)((f >> 23) & 0xFF) - 127 + 15;
		UInt32 mantissa = f & 0x7FFFFFu;
		if (exponent <= 0) return sign;
		if (exponent >= 31) return sign | 0x7C00u;
		return sign | ((UInt32)exponent << 10) | (mantissa >> 13);
	}

	const UINT SpdMipCount = 6;		// AMD binds SPD mips 0-5 (the luminance pyramid reads mip 5 back, the shading change reads 0-2)

	class Fsr31Upscaler : public Upscaler::IUpscaler {
	public:
		const char*			Name() const override { return "FSR 3.1"; }
		IDirect3DTexture9*	Output() const override { return output.Texture; }
		bool				Failed() const override { return failed; }
		bool				Evaluate(IDirect3DDevice9* Device, const Upscaler::Inputs& In) override;
		void				ReleaseResources() override;
		void				RequestReset() override { resetPending = true; }

	private:
		struct Tex { IDirect3DTexture9* Texture = nullptr; IDirect3DSurface9* Surface = nullptr; };
		ID3D9NvrExtPipeline*	pipelines[PipelineCount] = {};
		ID3D9NvrExtPipeline*	hdrPipelines[PipelineCount] = {};	// Accumulate, AccumulateSharpen only
		bool		tried = false;
		bool		hdrTried = false;
		bool		failed = false;
		bool		firstExecution = true;
		bool		resetPending = false;
		ID3D9NvrExtBuffer*	constants = nullptr;
		bool		constantsTried = false;
		ID3D9NvrExtBuffer*	reconstructedDepth = nullptr;	// one uint per pixel of the max render size (D3D9 has no r32ui render target)
		ID3D9NvrExtBuffer*	spdAtomic = nullptr;			// SPD's global atomic counter
		ID3D9NvrExtBuffer*	frameInfo = nullptr;			// AMD's 1x1 rgba32f frame info
		IDirect3DTexture9*	spdMips = nullptr;				// rg16f, half the max render size, SpdMipCount levels
		Tex			dilatedMotion, dilatedDepth, luma[2], intermediate, farthestDepthMip1, shadingChange, accumulation[2], dilatedReactiveMasks,
					lumaHistory[2], newLocks, upscaled[2], output;
		UINT		inputWidth = 0, inputHeight = 0, displayWidth = 0, displayHeight = 0;
		int			parity = 0;		// AMD's isOddFrame (resourceFrameIndex & 1)
		FSR3Constants	c = {};		// kept between frames: AMD reads last frame's sizes, jitter and phase count from it

		bool	Fail(const char* why) {
			Logger::Log("[ERROR] UNOFFICIAL Vegas Shaders DXVK: FSR 3.1: %s; FSR 3.1 is off for this session (FSR 2 runs instead)", why);
			failed = true;
			return false;
		}
		bool	MakePipelines(bool hdr);
		bool	EnsureResources(UINT inW, UINT inH, UINT outW, UINT outH);
		void	ClearSpdMips(IDirect3DDevice9* Device);
		bool	Dispatch(int index, bool hdr, const NVR_EXT_RESOURCE_BINDING* bindings, UINT count, const FSR3Push* push, UINT groupsX, UINT groupsY);
	};

	void Fsr31Upscaler::ReleaseResources() {
		Tex* textures[] = { &dilatedMotion, &dilatedDepth, &luma[0], &luma[1], &intermediate, &farthestDepthMip1, &shadingChange, &accumulation[0],
			&accumulation[1], &dilatedReactiveMasks, &lumaHistory[0], &lumaHistory[1], &newLocks, &upscaled[0], &upscaled[1], &output };
		for (Tex* t : textures) {
			if (t->Surface) { t->Surface->Release(); t->Surface = nullptr; }
			if (t->Texture) { t->Texture->Release(); t->Texture = nullptr; }
		}
		if (spdMips) { spdMips->Release(); spdMips = nullptr; }
		DxvkCompute::Drop(reconstructedDepth);
		DxvkCompute::Drop(spdAtomic);
		DxvkCompute::Drop(frameInfo);
		inputWidth = inputHeight = displayWidth = displayHeight = 0;
	}

	bool Fsr31Upscaler::MakePipelines(bool hdr) {
		if (!hdr) {
			if (tried) return pipelines[RCAS] != nullptr;
			tried = true;
			for (int i = 0; i < PipelineCount; i++) {
				const PipelineInfo& p = Pipelines[i];
				// Checked first: DxvkCompute::CreatePipeline turns every Vulkan pass off for the session when a file is missing.
				if (!DxvkCompute::HasSpirv(p.name)) return Fail("Compute\\FSR31*.spv is missing (copy the FSR 3.1 shaders)");
				pipelines[i] = DxvkCompute::CreatePipeline(p.name, p.bindings, p.count, PushSizeOf(i), p.samplers);
				if (!pipelines[i]) return Fail("a compute pipeline could not be made (Compute\\FSR31*.spv refused)");
			}
			return true;
		}
		if (hdrTried) return hdrPipelines[Accumulate] != nullptr;
		hdrTried = true;
		for (int i = 0; i < PipelineCount; i++) {
			const PipelineInfo& p = Pipelines[i];
			if (!p.hdrName) continue;
			if (!DxvkCompute::HasSpirv(p.hdrName)) return Fail("Compute\\FSR31Accumulate*Hdr.spv is missing (copy the FSR 3.1 shaders)");
			hdrPipelines[i] = DxvkCompute::CreatePipeline(p.hdrName, p.bindings, p.count, PushSizeOf(i), p.samplers);
			if (!hdrPipelines[i]) return Fail("an HDR compute pipeline could not be made (Compute\\FSR31Accumulate*Hdr.spv refused)");
		}
		return true;
	}

	// AMD's internal resources (fsr3upscalerCreate), made on first use and again when a size changes. Render-resolution ones at the
	// input textures' size (AMD's max render size), display-resolution ones at the display size. Formats: AMD's, or a float format of
	// at least the same precision where D3D9 has no render target of AMD's (see ffx_fsr3upscaler_callbacks_nvr.h). All are
	// render-target textures so DXVK can add storage use to them.
	bool Fsr31Upscaler::EnsureResources(UINT inW, UINT inH, UINT outW, UINT outH) {
		if (inputWidth == inW && inputHeight == inH && displayWidth == outW && displayHeight == outH && output.Surface) return true;
		ReleaseResources();

		struct Make { Tex* texture; const char* name; UINT w, h; D3DFORMAT format; };
		const UINT halfW = (std::max)(1u, inW / 2), halfH = (std::max)(1u, inH / 2);
		const Make list[] = {
			{ &dilatedMotion,        "TESR_FSR31DilatedMotion",     inW,   inH,   D3DFMT_G16R16F },
			{ &dilatedDepth,         "TESR_FSR31DilatedDepth",      inW,   inH,   D3DFMT_R32F },
			{ &luma[0],              "TESR_FSR31Luma0",             inW,   inH,   D3DFMT_R16F },
			{ &luma[1],              "TESR_FSR31Luma1",             inW,   inH,   D3DFMT_R16F },
			{ &intermediate,         "TESR_FSR31Intermediate",      inW,   inH,   D3DFMT_R16F },	// farthest depth, then luma instability
			{ &farthestDepthMip1,    "TESR_FSR31FarthestDepthMip1", halfW, halfH, D3DFMT_R16F },
			{ &shadingChange,        "TESR_FSR31ShadingChange",     halfW, halfH, D3DFMT_R16F },
			{ &accumulation[0],      "TESR_FSR31Accumulation0",     inW,   inH,   D3DFMT_R16F },
			{ &accumulation[1],      "TESR_FSR31Accumulation1",     inW,   inH,   D3DFMT_R16F },
			{ &dilatedReactiveMasks, "TESR_FSR31ReactiveMasks",     inW,   inH,   D3DFMT_A16B16G16R16F },
			{ &lumaHistory[0],       "TESR_FSR31LumaHistory0",      inW,   inH,   D3DFMT_A16B16G16R16F },
			{ &lumaHistory[1],       "TESR_FSR31LumaHistory1",      inW,   inH,   D3DFMT_A16B16G16R16F },
			{ &newLocks,             "TESR_FSR31NewLocks",          outW,  outH,  D3DFMT_R16F },
			{ &upscaled[0],          "TESR_FSR31Upscaled0",         outW,  outH,  D3DFMT_A16B16G16R16F },
			{ &upscaled[1],          "TESR_FSR31Upscaled1",         outW,  outH,  D3DFMT_A16B16G16R16F },
			{ &output,               "TESR_FSR31Output",            outW,  outH,  D3DFMT_A16B16G16R16F },
		};
		for (const Make& m : list) {
			TheTextureManager->InitTexture(m.name, &m.texture->Texture, &m.texture->Surface, m.w, m.h, m.format);
			if (!m.texture->Surface) {
				Logger::Log("[ERROR] FSR 3.1: could not create %s (%ux%u, format %u)", m.name, m.w, m.h, (UInt32)m.format);
				ReleaseResources();
				return false;
			}
		}
		IDirect3DDevice9* device = TheRenderManager->device;
		if (FAILED(device->CreateTexture(halfW, halfH, SpdMipCount, D3DUSAGE_RENDERTARGET, D3DFMT_G16R16F, D3DPOOL_DEFAULT, &spdMips, NULL)) ||
			spdMips->GetLevelCount() < SpdMipCount) {
			Logger::Log("[ERROR] FSR 3.1: could not create its SPD mips (%ux%u, %u levels)", halfW, halfH, SpdMipCount);
			ReleaseResources();
			return false;
		}
		reconstructedDepth = DxvkCompute::CreateBuffer(inW * inH * 4, NVR_EXT_BUFFER_STORAGE | NVR_EXT_BUFFER_TRANSFER_DST, "FSR 3.1 reconstructed depth");
		spdAtomic = DxvkCompute::CreateBuffer(16, NVR_EXT_BUFFER_STORAGE | NVR_EXT_BUFFER_TRANSFER_DST, "FSR 3.1 SPD counter");
		frameInfo = DxvkCompute::CreateBuffer(16, NVR_EXT_BUFFER_STORAGE | NVR_EXT_BUFFER_TRANSFER_DST, "FSR 3.1 frame info");
		if (!reconstructedDepth || !spdAtomic || !frameInfo) { ReleaseResources(); return false; }

		// D3D9 textures start undefined: everything FSR reads before writing starts at 0 (AMD's first-execution clears cover
		// the accumulation and luma; the new locks are cleared by Accumulate after each read, so they only need this once).
		Tex* cleared[] = { &dilatedMotion, &luma[0], &luma[1], &accumulation[0], &accumulation[1], &lumaHistory[0], &lumaHistory[1],
			&newLocks, &upscaled[0], &upscaled[1], &intermediate, &farthestDepthMip1, &shadingChange, &dilatedReactiveMasks };
		for (Tex* t : cleared) device->ColorFill(t->Surface, nullptr, D3DCOLOR_ARGB(0, 0, 0, 0));
		ClearSpdMips(device);

		inputWidth = inW; inputHeight = inH; displayWidth = outW; displayHeight = outH;
		parity = 0;
		firstExecution = true;
		c = FSR3Constants();
		const UInt64 renderBytes = (UInt64)inW * inH * (4 + 4 + 2 * 2 + 2 + 2 * 2 + 8 + 8 * 2 + 4) + (UInt64)halfW * halfH * (2 + 2 + 4 * 4 / 3);
		const UInt64 displayBytes = (UInt64)outW * outH * (2 + 8 * 2 + 8);
		Logger::Log("UNOFFICIAL Vegas Shaders DXVK: FSR 3.1 resources made (render up to %ux%u, display %ux%u, about %u MB)", inW, inH, outW, outH,
			(UInt32)((renderBytes + displayBytes) / (1024 * 1024)));
		return true;
	}

	void Fsr31Upscaler::ClearSpdMips(IDirect3DDevice9* Device) {
		for (UINT level = 0; level < SpdMipCount; level++) {
			IDirect3DSurface9* surface = nullptr;
			if (SUCCEEDED(spdMips->GetSurfaceLevel(level, &surface))) {
				Device->ColorFill(surface, nullptr, D3DCOLOR_ARGB(0, 0, 0, 0));
				surface->Release();
			}
		}
	}

	bool Fsr31Upscaler::Dispatch(int index, bool hdr, const NVR_EXT_RESOURCE_BINDING* bindings, UINT count, const FSR3Push* push, UINT groupsX, UINT groupsY) {
		NVR_EXT_DISPATCH_DESC dispatch = {};
		dispatch.pPipeline = (hdr && Pipelines[index].hdrName) ? hdrPipelines[index] : pipelines[index];
		dispatch.BindingCount = count;
		dispatch.pBindings = bindings;
		dispatch.pPushData = PushSizeOf(index) ? push : nullptr;
		dispatch.PushDataSize = PushSizeOf(index);
		dispatch.GroupsX = groupsX;
		dispatch.GroupsY = groupsY;
		dispatch.GroupsZ = 1;
		// F10: one GPU timer per pass, inside the caller's "TAA" or "FSR upscale" timer.
		static GpuTimer timers[PipelineCount] = { GpuTimer("  FSR3.1 prepare inputs"), GpuTimer("  FSR3.1 luma pyramid"),
			GpuTimer("  FSR3.1 shading change pyramid"), GpuTimer("  FSR3.1 shading change"), GpuTimer("  FSR3.1 prepare reactivity"),
			GpuTimer("  FSR3.1 luma instability"), GpuTimer("  FSR3.1 accumulate"), GpuTimer("  FSR3.1 accumulate (sharpen)"), GpuTimer("  FSR3.1 RCAS") };
		GpuProfileScope gpu(timers[index], TheRenderManager->device);
		DxvkCompute::PreLoad(bindings, count);	// the alflicker rule: managed textures on the GPU (the inputs are render targets; harmless)
		if (FAILED(DxvkCompute::Extension()->Dispatch(&dispatch))) {
			Logger::Log("[ERROR] UNOFFICIAL Vegas Shaders DXVK: FSR 3.1 pass %s failed to dispatch; FSR 3.1 is off for this session (FSR 2 runs instead)", Pipelines[index].name);
			failed = true;
			return false;
		}
		return true;
	}

	bool Fsr31Upscaler::Evaluate(IDirect3DDevice9* Device, const Upscaler::Inputs& In) {
		if (failed) return false;
		if (!DxvkCompute::Available()) return false;
		if (!In.Color || !In.Depth || !In.Motion || !In.RenderWidth || !In.RenderHeight || In.RenderWidth > In.InputWidth ||
			In.RenderHeight > In.InputHeight || !In.DisplayWidth || !In.DisplayHeight) return Fail("an input is missing");
		if (!MakePipelines(false)) return false;
		if (In.Hdr && !MakePipelines(true)) return false;
		if (!EnsureResources(In.RenderWidth, In.RenderHeight, In.DisplayWidth, In.DisplayHeight)) return Fail("its textures could not be made");

		ID3D9NvrExtSampler* point = DxvkCompute::PointClamp();
		ID3D9NvrExtSampler* linear = DxvkCompute::LinearClamp();
		ID3D9NvrExtSampler* linearMips = DxvkCompute::MipSampler(false, NVR_EXT_ADDRESS_CLAMP);
		if (!point || !linear || !linearMips) return Fail("its samplers could not be made (needs Vegas Shaders DXVK extension 1.3 or newer)");

		// ffx_fsr3upscaler.cpp fsr3upscalerDispatch, in its order.
		const bool odd = parity != 0;
		const int srv = odd ? 1 : 0, uav = odd ? 0 : 1;	// accumulation / upscaled colour / luma history: last frame's (srv), this frame's (uav)
		Tex& currentLuma = luma[srv];						// AMD: LUMA_2 on odd frames, LUMA_1 on even ones; the previous luma is the other
		Tex& previousLuma = luma[uav];
		if (firstExecution) {
			Device->ColorFill(accumulation[0].Surface, nullptr, D3DCOLOR_ARGB(0, 0, 0, 0));
			Device->ColorFill(accumulation[1].Surface, nullptr, D3DCOLOR_ARGB(0, 0, 0, 0));
			Device->ColorFill(luma[0].Surface, nullptr, D3DCOLOR_ARGB(0, 0, 0, 0));
			Device->ColorFill(luma[1].Surface, nullptr, D3DCOLOR_ARGB(0, 0, 0, 0));
		}
		const bool reset = In.Reset || resetPending || firstExecution;
		firstExecution = false;
		resetPending = false;

		const INT32 renderW = (INT32)In.RenderWidth, renderH = (INT32)In.RenderHeight;
		const INT32 displayW = (INT32)In.DisplayWidth, displayH = (INT32)In.DisplayHeight;
		c.previousFrameJitterOffset[0] = c.jitterOffset[0];
		c.previousFrameJitterOffset[1] = c.jitterOffset[1];
		c.jitterOffset[0] = In.JitterX;
		c.jitterOffset[1] = In.JitterY;
		c.previousFrameRenderSize[0] = c.renderSize[0];
		c.previousFrameRenderSize[1] = c.renderSize[1];
		c.renderSize[0] = renderW;
		c.renderSize[1] = renderH;
		c.maxRenderSize[0] = renderW;
		c.maxRenderSize[1] = renderH;
		c.tanHalfFOV = In.TanHalfFovX;
		c.viewSpaceToMetersFactor = 1.0f / In.UnitsPerMetre;
		// setupDeviceDepthToViewSpaceDepthParams for a standard (not inverted, finite) projection with the inputs' near and far: the
		// callers write a depth that formula turns back into the linear view depth (as for FSR 2).
		const float nearZ = In.NearZ, farZ = In.FarZ;
		const float q = farZ / (nearZ - farZ);
		c.deviceToViewDepth[0] = -q;
		c.deviceToViewDepth[1] = q * nearZ;
		c.deviceToViewDepth[2] = In.TanHalfFovX;	// 1 / a = aspect * tan(fovY / 2)
		c.deviceToViewDepth[3] = In.TanHalfFovY;	// 1 / b
		c.previousFrameUpscaleSize[0] = c.upscaleSize[0];
		c.previousFrameUpscaleSize[1] = c.upscaleSize[1];
		c.upscaleSize[0] = c.maxUpscaleSize[0] = displayW;
		c.upscaleSize[1] = c.maxUpscaleSize[1] = displayH;
		c.downscaleFactor[0] = (float)renderW / (float)displayW;
		c.downscaleFactor[1] = (float)renderH / (float)displayH;
		c.deltaPreExposure = 1.0f;					// NVR passes no pre-exposure (1 every frame)
		c.motionVectorScale[0] = c.motionVectorScale[1] = 1.0f;	// the callers write uv units (AMD: the app's scale / render size)
		c.motionVectorJitterCancellation[0] = c.motionVectorJitterCancellation[1] = 0.0f;	// unjittered motion vectors
		// The jitter sequence length: the callers' (ffxFsr3UpscalerGetJitterPhaseCount's formula), stepped by one per frame towards
		// a new value as AMD does.
		const float phases = (std::max)(1.0f, In.JitterPhases);
		if (reset || c.jitterPhaseCount == 0.0f) c.jitterPhaseCount = phases;
		else if (phases > c.jitterPhaseCount) c.jitterPhaseCount += 1.0f;
		else if (phases < c.jitterPhaseCount) c.jitterPhaseCount -= 1.0f;
		c.deltaTime = In.DeltaSeconds < 0.0f ? 0.0f : (In.DeltaSeconds > 1.0f ? 1.0f : In.DeltaSeconds);
		c.frameIndex = reset ? 0.0f : c.frameIndex + 1.0f;
		// AMD's defaults (FfxFsr3UpscalerConfigureKey: velocity factor, reactiveness / shading change scales, accumulation rates).
		c.velocityFactor = 1.0f;
		c.reactivenessScale = 1.0f;
		c.shadingChangeScale = 1.0f;
		c.accumulationAddedPerFrame = 1.0f / 3.0f;
		c.minDisocclusionAccumulation = -1.0f / 3.0f;
		if (!constants && !constantsTried) {
			constantsTried = true;
			constants = DxvkCompute::CreateBuffer(sizeof(c), NVR_EXT_BUFFER_UNIFORM | NVR_EXT_BUFFER_TRANSFER_DST, "FSR 3.1 constants");
		}
		if (!constants || FAILED(DxvkCompute::Extension()->UpdateBuffer(constants, 0, sizeof(c), &c))) return Fail("its constant buffer could not be written");

		// The clears AMD schedules: on a reset this frame's accumulation history, the scene luminance (SPD mips) and the frame info
		// (exposure -1, log luma 1); every frame the reconstructed depth (to the far plane, 1.0) and SPD's counter (and the SPD mips).
		if (reset) {
			Device->ColorFill(accumulation[srv].Surface, nullptr, D3DCOLOR_ARGB(0, 0, 0, 0));
			const float info[4] = { -1.0f, 1.0f, 0.0f, 0.0f };
			if (FAILED(DxvkCompute::Extension()->UpdateBuffer(frameInfo, 0, sizeof(info), info))) return Fail("its frame info could not be reset");
		}
		DxvkCompute::Extension()->ClearBuffer(reconstructedDepth, 0, 0, 0x3f800000u);
		DxvkCompute::Extension()->ClearBuffer(spdAtomic, 0, 0, 0u);
		ClearSpdMips(Device);

		// SPD (ffxSpdSetup over the render size): 64x64 tiles, mips = floor(log2(max side)), at most 12.
		const UINT spdGroupsX = (UINT)(renderW - 1) / 64 + 1, spdGroupsY = (UINT)(renderH - 1) / 64 + 1;
		FSR3Push push = {};
		push.spdMips = (UInt32)(std::min)(floorf(log2f((float)(std::max)(renderW, renderH))), 12.0f);
		push.spdNumWorkGroups = spdGroupsX * spdGroupsY;
		push.spdRenderSize[0] = (UInt32)renderW;
		push.spdRenderSize[1] = (UInt32)renderH;
		const float sharpness = In.Sharpness;
		const bool sharpen = sharpness > 0.0f;
		if (sharpen) {
			// sharpenessRemapped = -2 * sharpness + 2, then FsrRcasCon: exp2(-stops) as float and packed halves.
			const float stops = -2.0f * sharpness + 2.0f;
			const float s = exp2f(-stops);
			push.rcas[0] = *(UInt32*)&s;
			push.rcas[1] = HalfBits(s) | (HalfBits(s) << 16);
		}

		const UINT src8X = (renderW + 7) / 8, src8Y = (renderH + 7) / 8;
		const UINT dst8X = (displayW + 7) / 8, dst8Y = (displayH + 7) / 8;
		const UINT shading8X = ((UINT)(renderW * 0.5f) + 7) / 8, shading8Y = ((UINT)(renderH * 0.5f) + 7) / 8;
		const NVR_EXT_RESOURCE_BINDING cb = Buffer(SlotConstants, NVR_EXT_BIND_UNIFORM_BUFFER, constants);
		const NVR_EXT_RESOURCE_BINDING samplers[3] = { Sampler(0, point), Sampler(1, linear), Sampler(2, linearMips) };

		{
			NVR_EXT_RESOURCE_BINDING b[] = { Image(0, In.Motion), Image(1, In.Depth), Image(2, In.Color), Image(3, dilatedMotion.Texture, true),
				Image(4, dilatedDepth.Texture, true), Buffer(SlotReconstructedDepth, NVR_EXT_BIND_STORAGE_BUFFER, reconstructedDepth),
				Image(6, intermediate.Texture, true), Image(7, currentLuma.Texture, true), cb };
			if (!Dispatch(PrepareInputs, false, b, _countof(b), &push, src8X, src8Y)) return false;
		}
		{
			NVR_EXT_RESOURCE_BINDING b[] = { Image(0, currentLuma.Texture), Image(1, intermediate.Texture),
				Buffer(SlotSpdAtomic, NVR_EXT_BIND_STORAGE_BUFFER, spdAtomic), Buffer(SlotFrameInfo, NVR_EXT_BIND_STORAGE_BUFFER, frameInfo),
				Image(9, spdMips, true, 5), Image(10, farthestDepthMip1.Texture, true), cb };
			if (!Dispatch(LumaPyramid, false, b, _countof(b), &push, spdGroupsX, spdGroupsY)) return false;
		}
		// AMD clears SPD's counter before every use (the last group of a pass resets it too).
		DxvkCompute::Extension()->ClearBuffer(spdAtomic, 0, 0, 0u);
		{
			NVR_EXT_RESOURCE_BINDING b[] = { Image(0, currentLuma.Texture), Image(1, previousLuma.Texture), Image(2, dilatedMotion.Texture),
				Buffer(SlotSpdAtomic, NVR_EXT_BIND_STORAGE_BUFFER, spdAtomic), Image(5, spdMips, true, 0), Image(6, spdMips, true, 1),
				Image(7, spdMips, true, 2), Image(8, spdMips, true, 3), Image(9, spdMips, true, 4), Image(10, spdMips, true, 5), cb };
			if (!Dispatch(ShadingChangePyramid, false, b, _countof(b), &push, spdGroupsX, spdGroupsY)) return false;
		}
		{
			NVR_EXT_RESOURCE_BINDING b[] = { Image(0, spdMips), Image(1, shadingChange.Texture, true), cb, samplers[0], samplers[1], samplers[2] };
			if (!Dispatch(ShadingChange, false, b, _countof(b), &push, shading8X, shading8Y)) return false;
		}
		{
			NVR_EXT_RESOURCE_BINDING b[] = { Buffer(SlotReconstructedDepth, NVR_EXT_BIND_STORAGE_BUFFER, reconstructedDepth),
				Image(1, dilatedMotion.Texture), Image(2, dilatedDepth.Texture), Image(3, In.Depth), Image(5, accumulation[srv].Texture),
				Image(6, shadingChange.Texture), Image(7, currentLuma.Texture), Image(9, dilatedReactiveMasks.Texture, true),
				Image(10, newLocks.Texture, true), Image(11, accumulation[uav].Texture, true), cb, samplers[0], samplers[1], samplers[2] };
			if (!Dispatch(PrepareReactivity, false, b, _countof(b), &push, src8X, src8Y)) return false;
		}
		{
			NVR_EXT_RESOURCE_BINDING b[] = { Image(1, dilatedReactiveMasks.Texture), Image(2, dilatedMotion.Texture), Image(4, lumaHistory[srv].Texture),
				Image(6, currentLuma.Texture), Image(7, lumaHistory[uav].Texture, true), Image(8, intermediate.Texture, true), cb,
				samplers[0], samplers[1], samplers[2] };
			if (!Dispatch(LumaInstability, false, b, _countof(b), &push, src8X, src8Y)) return false;
		}
		{
			NVR_EXT_RESOURCE_BINDING b[] = { Image(1, dilatedReactiveMasks.Texture), Image(2, dilatedMotion.Texture), Image(3, upscaled[srv].Texture),
				Image(5, farthestDepthMip1.Texture), Image(7, intermediate.Texture), Image(8, In.Color), Image(9, upscaled[uav].Texture, true),
				Image(10, output.Texture, true), Image(11, newLocks.Texture, true), cb, samplers[0], samplers[1], samplers[2] };
			if (sharpen) {
				// The sharpening variant has no output binding (RCAS writes it).
				NVR_EXT_RESOURCE_BINDING s[_countof(b) - 1];
				UINT n = 0;
				for (const NVR_EXT_RESOURCE_BINDING& r : b) if (!(r.Type == NVR_EXT_BIND_STORAGE_IMAGE && r.Slot == 10)) s[n++] = r;
				if (!Dispatch(AccumulateSharpen, In.Hdr, s, n, &push, dst8X, dst8Y)) return false;
			}
			else if (!Dispatch(Accumulate, In.Hdr, b, _countof(b), &push, dst8X, dst8Y)) return false;
		}
		if (sharpen) {
			NVR_EXT_RESOURCE_BINDING b[] = { Image(1, upscaled[uav].Texture), Image(2, output.Texture, true) };
			if (!Dispatch(RCAS, false, b, _countof(b), &push, (displayW + 15) / 16, (displayH + 15) / 16)) return false;
		}
		parity ^= 1;	// AMD: resourceFrameIndex = (resourceFrameIndex + 1) % 16, of which only the parity is used
		return true;
	}

	Fsr31Upscaler TheFsr31;
}

namespace Upscaler {
	IUpscaler* Fsr31() { return &TheFsr31; }
}
