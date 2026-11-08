// UNOFFICIAL 2026-10-29 (NVR DXVK) the shared RT denoiser (RtDenoise.h). Passes: Compute\RtDenoiseTemporal.frag, Compute\RtDenoiseAtrous.frag.

#include "RtDenoise.h"
#include "RenderRect.h"	// UNOFFICIAL 2026-11-03 RenderScale < 1 (docs/next/RT-RENDERSCALE.md)
#include "DxvkCompute.h"
#include "GpuProfiler.h"
#include "Upscaler.h"	// UNOFFICIAL 2026-10-30 DLSS RR (RayReconstructionWantsRaw)
#include "ObjectMotion.h"	// UNOFFICIAL 2026-10-31 DenoiseBackbone (B1's object motion)

#include <cmath>
#include <string>

namespace RtDenoise {
	struct Denoiser {
		std::string			name;
		std::string			effect;					// UNOFFICIAL 2026-10-30 the caller's own name (the DLSS RR bypass' log line)
		UInt32				rawSeen = 0;			// UNOFFICIAL 2026-10-30 rawGeneration when this one last ran (below)
		bool				rawLogged = false;
		IDirect3DTexture9*	history[2] = {};	// RGBA16F: value, second moment, history length, view depth (ping-pong by frame)
		IDirect3DTexture9*	chain[2] = {};		// RGBA16F: the a-trous ping-pong
		// RGB mode (Settings.Channels 3): A = (rgb, history length) in history / chain, B = (luma second moment, view depth) here
		IDirect3DTexture9*	historyB[2] = {};
		IDirect3DTexture9*	chainB[2] = {};
		IDirect3DTexture9*	scratchB = nullptr;		// the last a-trous pass' B output (not needed after)
		IDirect3DTexture9*	historyC[2] = {};		// (2026-11-07 ptaaa-denoiser) Specular: (tracked hit distance, roughness, virtual share, 0)
		bool				specular = false;
		IDirect3DTexture9*	park = nullptr;			// 8 x 8: the device's render target while the passes run (a pass may not draw into it)
		int					channels = 0;
		GpuTimer*			timer = nullptr;			// "RT denoise: <name>" in the GPU profile
		UINT				width = 0, height = 0;
		UInt32				frame = 0;
		bool				hasHistory = false;
		D3DXMATRIX			prevViewProj;
		float				lastScale[2] = { 1.0f, 1.0f };	// (2026-11-03) the RenderRect scale the history was made at
		D3DXVECTOR3			prevCamera = { 0.0f, 0.0f, 0.0f };
	};

	namespace {
		ID3D9NvrExtPipeline* temporal = nullptr;
		ID3D9NvrExtPipeline* atrous = nullptr;
		ID3D9NvrExtPipeline* temporalRgb = nullptr;
		ID3D9NvrExtPipeline* atrousRgb = nullptr;
		ID3D9NvrExtPipeline* temporalSpec = nullptr;	// (2026-11-07 ptaaa-denoiser) Settings.Specular
		ID3D9NvrExtPipeline* fireflyPipe = nullptr;		// (2026-11-07 ptaaa-denoiser) Settings.AntiFirefly
		bool specTried = false;
		bool tried = false;
		// UNOFFICIAL 2026-10-30 DLSS RR: counts the calls Run skipped for RR (0 while it never did). A denoiser that ran before it changed
		// holds a history from before those frames, also one its caller did not call meanwhile (RtLampShadows stops at rank 0).
		UInt32 rawGeneration = 0;

		// UNOFFICIAL 2026-10-31 DenoiseBackbone ([Main.Main.RayTracing], docs\next\RT-DENOISE-BACKBONE.md): ONE full-size pass a frame
		// (Compute\RtBackbone.frag), made by the first denoiser that runs in the frame and read by all of them: each pixel's screen move
		// since last frame (camera + the object's own move, B1) and whether it is newly seen (disocclusion). The temporal passes then
		// subtract the object motion before reprojecting and restart where the shared mask says so. Ping-pong: .a keeps the depth.
		ID3D9NvrExtPipeline* backbonePipe = nullptr;
		IDirect3DTexture9* backbone[2] = {};
		IDirect3DTexture9* backboneNow = nullptr;	// this frame's (nullptr: not made this frame)
		UINT backboneW = 0, backboneH = 0;
		UInt32 backboneFrame = 0xFFFFFFFF, backboneCount = 0, backboneMotionFrames = 0, backboneLogged = 0;
		bool backboneHas = false;
		D3DXMATRIX backbonePrevViewProj;
		D3DXVECTOR3 backbonePrevCamera = { 0.0f, 0.0f, 0.0f };
		GpuTimer* backboneTimer = nullptr;
		// 2026-10-31 (rt-ao-sky) GIMotion: its pass (alpha only), last frame's camera
		ID3D9NvrExtPipeline* giMotionPipe = nullptr;
		bool giMotionTried = false, giMotionHas = false;
		UInt32 giMotionFrame = 0xFFFFFFFF, giMotionCount = 0;
		D3DXMATRIX giMotionPrevViewProj;
		D3DXVECTOR3 giMotionPrevCamera = { 0.0f, 0.0f, 0.0f };
		GpuTimer* giMotionTimer = nullptr;

		IDirect3DBaseTexture9* Texture(const char* name) {
			auto found = TheTextureManager->TextureNames.find(name);
			return found == TheTextureManager->TextureNames.end() || !found->second ? nullptr : *found->second;
		}
		const D3DXVECTOR4* Constant(const char* name) {
			auto found = TheShaderManager->ConstantsTable.find(name);
			return found == TheShaderManager->ConstantsTable.end() ? nullptr : found->second;
		}

		// (2026-11-07 ptaaa-denoiser) samplerAt: the push block's byte of the sampler's heap index (default: its last 16 bytes' start)
		ID3D9NvrExtPipeline* Make(const char* name, UINT images, UINT pushSize, UINT targets = 1, UINT writeMask = 0xFu, UINT samplerAt = 0xFFFFFFFFu) {
			std::vector<char> spirv;
			ID3D9NvrExt* ext = DxvkCompute::Extension();
			if (!ext || !DxvkCompute::LoadSpirv(name, spirv)) return nullptr;
			NVR_EXT_BINDING_DESC bindings[10] = {};	// images + the sampler
			for (UINT i = 0; i < images; i++) bindings[i] = { 1, i, NVR_EXT_BIND_SAMPLED_IMAGE, i, NVR_EXT_VIEW_2D, FALSE, 0 };
			bindings[images] = { 0, 0, NVR_EXT_BIND_SAMPLER, 0, NVR_EXT_VIEW_2D, FALSE, samplerAt != 0xFFFFFFFFu ? samplerAt : pushSize - 16 };	// samplerIndex: the last 16 bytes
			NVR_EXT_GRAPHICS_PIPELINE_DESC desc = {};
			desc.pFragmentSpirv = spirv.data();
			desc.FragmentSpirvSize = (UINT)spirv.size();
			desc.BindingCount = images + 1;
			desc.pBindings = bindings;
			desc.PushConstantSize = pushSize;
			desc.UsesSamplerHeap = TRUE;
			desc.SamplerHeapSet = 0;
			desc.SamplerHeapBinding = 0;
			desc.ColorTargetCount = targets;
			desc.Blend = NVR_EXT_BLEND_NONE;
			desc.ColorWriteMask = writeMask;
			desc.pDebugName = name;
			ID3D9NvrExtPipeline* pipeline = nullptr;
			if (FAILED(ext->CreateGraphicsPipeline(&desc, &pipeline))) pipeline = nullptr;
			if (!pipeline) Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RT denoiser: the pass %s could not be made (Compute\\%s.spv; the DXVK log says why)", name, name);
			return pipeline;
		}

		bool Draw(ID3D9NvrExtPipeline* pipeline, IDirect3DSurface9* target, IUnknown* const* images, UINT count, const void* push, UINT pushSize,
			IDirect3DSurface9* target2 = nullptr, IDirect3DSurface9* target3 = nullptr, IDirect3DSurface9* target4 = nullptr) {
			NVR_EXT_RESOURCE_BINDING bindings[10] = {};	// images + the sampler
			for (UINT i = 0; i < count; i++) { bindings[i].Slot = i; bindings[i].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[i].pResource = images[i]; }
			bindings[count].Slot = 0; bindings[count].Type = NVR_EXT_BIND_SAMPLER; bindings[count].pResource = DxvkCompute::PointClamp();
			NVR_EXT_DRAW_DESC draw = {};
			draw.pPipeline = pipeline;
			draw.TargetCount = target4 ? 4 : target3 ? 3 : target2 ? 2 : 1;
			draw.Targets[0].pResource = target;
			draw.Targets[1].pResource = target2;
			draw.Targets[2].pResource = target3;
			draw.Targets[3].pResource = target4;
			draw.BindingCount = count + 1;
			draw.pBindings = bindings;
			draw.pPushData = push;
			draw.PushDataSize = pushSize;
			return SUCCEEDED(DxvkCompute::Extension()->Draw(&draw));
		}

		// The passes' push blocks (tools/nvr-rt-host/rt_layout_check.py checks them against the shaders' and the host's mirrors)
		struct TemporalPush {	// RtDenoiseTemporal.frag / RtDenoiseTemporalRgb.frag
			float prevViewProj[16];
			float forward[4], right[4], up[4], move[4], params[4];
			float backbone[4];	// 2026-10-31 DenoiseBackbone: x object motion, y the shared disocclusion mask
			float rect[4];		// (2026-11-03) xy this frame's RenderRect scale, zw last frame's
			UInt32 samplerIndex;
			float clampK;		// (2026-11-06) RGB mode: HistoryClamp
			float peopleCap;	// (2026-11-07) RGB mode: PeopleHistory (0 off)
			float antiLag;		// (2026-11-07) RGB mode: AntiLag (0 off)
			float fireflyK;		// (2026-11-07 ptaaa-denoiser) RGB mode: AntiFirefly (0 off; not in RtDenoiseTemporal.frag's block: it gets 192 bytes)
			float fastFrames;	// (2026-11-07 perf-history) RGB mode: the anti-lag's fast history in frames (0: 4, as before)
		};
		struct SpecPush {		// (2026-11-07 ptaaa-denoiser) RtDenoiseTemporalSpec.frag: TemporalPush + spec before the sampler
			float prevViewProj[16];
			float forward[4], right[4], up[4], move[4], params[4];
			float backbone[4];
			float rect[4];
			float spec[4];		// x MirrorHistory, y 1: the virtual reprojection
			UInt32 samplerIndex;
			float clampK;
			float peopleCap;
			float antiLag;
			float fireflyK;
			float fastFrames;	// (2026-11-07 perf-history)
		};
		struct AtrousPush {		// RtDenoiseAtrous.frag / RtDenoiseAtrousRgb.frag (guideSigma, specular: RGB mode only)
			float params[4], size[4];
			UInt32 samplerIndex;
			float guideSigma;
			UInt32 specular;
			float historyFix;	// (2026-11-07 ptaaa-denoiser) RGB mode: Settings.HistoryFix
		};
		static_assert(sizeof(TemporalPush) == 200 && sizeof(SpecPush) == 216 && sizeof(AtrousPush) == 48, "the passes' push blocks");
		struct FireflyPush {	// (2026-11-07 ptaaa-denoiser) RtDenoiseFirefly.frag
			float size[4];
			UInt32 samplerIndex;
			UInt32 pad0, pad1, pad2;
		};
		static_assert(offsetof(SpecPush, spec) == offsetof(TemporalPush, samplerIndex), "SpecPush = TemporalPush's head + spec");

		IDirect3DSurface9* Surface(IDirect3DTexture9* t) {
			IDirect3DSurface9* s = nullptr;
			return t && SUCCEEDED(t->GetSurfaceLevel(0, &s)) ? s : nullptr;
		}

		// DenoiseBackbone: this frame's backbone, made on the first call of the frame (the device's target parked by the caller)
		IDirect3DTexture9* BuildBackbone(IDirect3DDevice9* Device, IDirect3DBaseTexture9* depth, float farZ, const D3DXVECTOR4* basis) {
			const UInt32 frame = TheFrameRateManager->FrameNumber;
			if (backboneFrame == frame) return backboneNow;
			backboneFrame = frame;
			backboneNow = nullptr;
			if (!backbonePipe) return nullptr;
			const UINT w = TheRenderManager->width, h = TheRenderManager->height;
			if (!backbone[0] || backboneW != w || backboneH != h) {
				for (IDirect3DTexture9*& t : backbone) if (t) { t->Release(); t = nullptr; }
				for (IDirect3DTexture9*& t : backbone) Device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &t, nullptr);
				backboneW = w; backboneH = h;
				backboneHas = false;
				if (!backbone[0] || !backbone[1]) return nullptr;
			}
			if (!backboneTimer) backboneTimer = new GpuTimer("RT denoise backbone");
			GpuProfileScope gpu(*backboneTimer, Device);
			const D3DXVECTOR3 camera(TheRenderManager->CameraPosition.x, TheRenderManager->CameraPosition.y, TheRenderManager->CameraPosition.z);
			const D3DXVECTOR3 move = camera - backbonePrevCamera;
			const bool cut = !backboneHas || D3DXVec3Length(&move) > 512.0f || TheShaderManager->GameState.isCameraCut;
			const bool motion = ObjectMotion::Ready();
			struct {
				float prevViewProj[16];
				float forward[4], right[4], up[4], move[4], params[4];
				UInt32 samplerIndex, pad[3];
			} b = {};
			memcpy(b.prevViewProj, &backbonePrevViewProj, 64);
			for (int i = 0; i < 4; i++) { b.forward[i] = basis[0][i]; b.right[i] = basis[1][i]; b.up[i] = basis[2][i]; }
			b.forward[3] = farZ;
			b.move[0] = move.x; b.move[1] = move.y; b.move[2] = move.z; b.move[3] = cut ? 1.0f : 0.0f;
			b.params[0] = 0.03f; b.params[1] = motion ? 1.0f : 0.0f;
			const UInt32 last = backboneCount & 1, now = (backboneCount + 1) & 1;
			IUnknown* images[3] = { depth, backbone[last], motion ? (IUnknown*)ObjectMotion::Texture : (IUnknown*)depth };
			IDirect3DSurface9* target = Surface(backbone[now]);
			const bool ok = target && Draw(backbonePipe, target, images, 3, &b, sizeof(b));
			if (target) target->Release();
			backbonePrevViewProj = TheRenderManager->ViewProjMatrix;
			backbonePrevCamera = camera;
			backboneHas = ok;
			backboneCount++;
			if (motion) backboneMotionFrames++;
			if ((backboneCount % 600) == 1 && backboneLogged < 20) {
				backboneLogged++;
				Logger::Log("UNOFFICIAL RT denoise backbone: %s (%ux%u), %u frames made, %u with object motion (B1 [Main.Main.Quality] ObjectMotion %s)",
					ok ? "made" : "NOT made", w, h, backboneCount, backboneMotionFrames, motion ? "on, this frame's" : "off or not this frame's");
			}
			if (ok) backboneNow = backbone[now];
			return backboneNow;
		}
	}

	IDirect3DTexture9* Backbone() { return backboneFrame == TheFrameRateManager->FrameNumber ? backboneNow : nullptr; }

	// UNOFFICIAL 2026-10-31 (rt-ao-sky) RT GI with DenoiseBackbone (RtDenoise.h): at the RT point, once a frame. The camera as Run's.
	bool GIMotion(IDirect3DDevice9* Device, IDirect3DSurface9* Target, UINT Width, UINT Height, float ScaleX, float ScaleY) {
		const UInt32 frame = TheFrameRateManager->FrameNumber;
		if (giMotionFrame == frame) return false;
		const bool lastFrame = giMotionFrame + 1 == frame;	// (a frame without this call: its camera is not last frame's)
		giMotionFrame = frame;
		const D3DXVECTOR3 camera(TheRenderManager->CameraPosition.x, TheRenderManager->CameraPosition.y, TheRenderManager->CameraPosition.z);
		const D3DXVECTOR3 move = camera - giMotionPrevCamera;
		const bool had = giMotionHas && lastFrame && D3DXVec3Length(&move) <= 512.0f && !TheShaderManager->GameState.isCameraCut;
		const D3DXMATRIX prevViewProj = giMotionPrevViewProj;
		giMotionPrevViewProj = TheRenderManager->ViewProjMatrix;
		giMotionPrevCamera = camera;
		giMotionHas = true;
		if (!had || !Target || !ObjectMotion::Ready() || !DxvkCompute::Extension() || !DxvkCompute::PointClamp()) return false;
		if (!giMotionTried) { giMotionTried = true; giMotionPipe = Make("RtGIMotion", 2, 160, 1, 0x8u); }
		IDirect3DBaseTexture9* depth = Texture("TESR_DepthBuffer");
		const D3DXVECTOR4* cameraData = Constant("TESR_CameraData");
		if (!giMotionPipe || !depth || !cameraData) return false;
		const D3DXMATRIX& view = TheRenderManager->viewMatrix;
		const D3DXMATRIX& proj = TheRenderManager->projMatrix;
		struct {
			float prevViewProj[16];
			float forward[4], right[4], up[4], move[4], params[4];
			UInt32 samplerIndex, pad[3];
		} b = {};
		memcpy(b.prevViewProj, &prevViewProj, 64);
		b.forward[0] = view._13; b.forward[1] = view._23; b.forward[2] = view._33; b.forward[3] = cameraData->y;
		b.right[0] = view._11 / proj._11; b.right[1] = view._21 / proj._11; b.right[2] = view._31 / proj._11;
		b.up[0] = view._12 / proj._22; b.up[1] = view._22 / proj._22; b.up[2] = view._32 / proj._22;
		b.move[0] = move.x; b.move[1] = move.y; b.move[2] = move.z;
		b.params[0] = (float)Width; b.params[1] = (float)Height;
		// (2026-11-04 RenderScale < 1, phase 2) the rect's scale: RtGIMotion.frag rebuilds the point at uv / scale (1, 1: as before)
		b.params[2] = ScaleX; b.params[3] = ScaleY;
		if (!giMotionTimer) giMotionTimer = new GpuTimer("RT GI object motion");
		GpuProfileScope gpu(*giMotionTimer, Device);
		IUnknown* images[2] = { depth, ObjectMotion::Texture };
		const bool ok = Draw(giMotionPipe, Target, images, 2, &b, sizeof(b));
		if (ok && (giMotionCount++ % 600) == 0 && giMotionCount < 6000)
			Logger::Log("UNOFFICIAL RT GI: the reuse follows the object motion (DenoiseBackbone; %ux%u, %u frames)", Width, Height, giMotionCount);
		return ok;
	}

	// UNOFFICIAL 2026-11-07 (perf-history) the history clock (RtDenoise.h, RtHistoryTime.h): fed once a rendered frame, by the first caller
	RtHistoryTime::Clock HistoryClock() {
		static RtHistoryTime::Smoother smoother;
		static RtHistoryTime::Clock clock;
		static UInt32 frame = 0xFFFFFFFF;
		static UInt32 logged = 0, fed = 0;
		const UInt32 now = TheFrameRateManager->FrameNumber;
		if (now == frame) return clock;
		if (frame != 0xFFFFFFFF && now - frame > 30u) smoother.Reset();	// (no RT denoiser for a while: start from this frame's time)
		frame = now;
		smoother.Feed((float)TheFrameRateManager->ElapsedTime, TheShaderManager->GameState.isCameraCut);
		clock.dt = smoother.average;
		clock.on = TheSettingManager->GetSettingI("Main.Main.RayTracing", "DenoiseTimeBased") != 0;
		clock.maxSeconds = (std::max)(0.0f, TheSettingManager->GetSettingF("Main.Main.RayTracing", "DenoiseMaxHistorySeconds"));
		if ((fed++ % 3600u) == 120u && logged < 12) {
			logged++;
			Logger::Log("UNOFFICIAL RT denoise history: %s, frame time %.1f ms (%.0f fps, smoothed): a history of 32 frames at 60 fps keeps %.1f frames (%.2f s)%s",
				clock.on ? "time-based (DenoiseTimeBased)" : "frame-based (DenoiseTimeBased off)", clock.dt * 1000.0f, 1.0f / clock.dt,
				RtHistoryTime::Frames(32.0f, clock), RtHistoryTime::Frames(32.0f, clock) * clock.dt,
				clock.maxSeconds > 0.0f ? ", DenoiseMaxHistorySeconds on" : "");
		}
		return clock;
	}

	Denoiser* Create(const char* Name) {
		Denoiser* d = new Denoiser();
		d->effect = Name ? Name : "RT denoiser";
		d->name = std::string("RT denoise: ") + d->effect;
		d->timer = new GpuTimer(d->name.c_str());
		return d;
	}

	bool Run(Denoiser* d, IDirect3DDevice9* Device, IDirect3DBaseTexture9* Noisy, IDirect3DSurface9* Output, const Settings& S) {
		// UNOFFICIAL 2026-10-30 DLSS Ray Reconstruction (docs\next\FORK2-DLSS-RR.md): while RR denoises the frame (Upscaler::
		// RayReconstructionWantsRaw: RR ran last frame, RayReconstruction on, RRKeepDenoisers off) no pass runs and the caller takes its
		// noisy signal, as it does whenever this returns false. Every denoiser's history is dropped then (rawGeneration), so the first
		// frame denoised again starts afresh instead of reprojecting from a camera of frames ago.
		if (!S.IgnoreRr && Upscaler::RayReconstructionWantsRaw()) {
			rawGeneration++;
			if (d && !d->rawLogged) {
				d->rawLogged = true;
				Logger::Log("UNOFFICIAL DLSS RR: %s: its own denoiser is skipped (RR denoises the raw signal)", d->effect.c_str());
			}
			return false;
		}
		if (!d || !Noisy || !Output || !DxvkCompute::Extension() || !DxvkCompute::PointClamp()) return false;
		if (!tried) {
			tried = true;
			temporal = Make("RtDenoiseTemporal", 5, 192, 1, 0xFu, offsetof(TemporalPush, samplerIndex));	// (2026-11-03: + rect)	// 2026-10-31: + motion, backbone (stand-ins when off)
			atrous = Make("RtDenoiseAtrous", 3, 48);
			temporalRgb = Make("RtDenoiseTemporalRgb", 7, sizeof(TemporalPush), 3, 0xFu, offsetof(TemporalPush, samplerIndex));	// (2026-11-07: + the captured normals, PeopleHistory; ptaaa-denoiser: + the firefly excess target)
			fireflyPipe = Make("RtDenoiseFirefly", 3, sizeof(FireflyPush), 2);	// (2026-11-07 ptaaa-denoiser) AntiFirefly's spread (a missing .spv: AntiFirefly off)
			backbonePipe = Make("RtBackbone", 3, 160);
			atrousRgb = Make("RtDenoiseAtrousRgb", 5, 48, 2);	// (2026-11-07: + the guide frame and albedo)
		}
		// (2026-11-07 ptaaa-denoiser) the specular temporal pass, made on its first use (a missing .spv only turns the specular mode off)
		if (S.Specular && !specTried) { specTried = true; temporalSpec = Make("RtDenoiseTemporalSpec", 9, sizeof(SpecPush), 4, 0xFu, offsetof(SpecPush, samplerIndex)); }
		const bool rgb = S.Channels == 3;
		if (rgb && (!temporalRgb || !atrousRgb)) return false;
		const bool spec = rgb && S.Specular;	// (2026-11-07 ptaaa-denoiser)
		if (spec && (!temporalSpec || !S.Roughness)) return false;
		IDirect3DBaseTexture9* depth = Texture("TESR_DepthBuffer");
		IDirect3DBaseTexture9* normals = Texture("TESR_NormalsBuffer");
		const D3DXVECTOR4* cameraData = Constant("TESR_CameraData");
		if (!temporal || !atrous || !depth || !normals || !cameraData) return false;

		const UINT scale = S.Downscale == 2 ? 2u : 1u;
		const UINT w = (std::max)(1u, (UINT)TheRenderManager->width / scale), h = (std::max)(1u, (UINT)TheRenderManager->height / scale);
		if (d->width != w || d->height != h || !d->history[0] || d->channels != S.Channels || d->specular != spec) {
			IDirect3DTexture9** all[] = { &d->history[0], &d->history[1], &d->chain[0], &d->chain[1], &d->historyB[0], &d->historyB[1],
				&d->chainB[0], &d->chainB[1], &d->scratchB, &d->historyC[0], &d->historyC[1], &d->park };
			for (IDirect3DTexture9** t : all) if (*t) { (*t)->Release(); *t = nullptr; }
			d->specular = spec;
			const int count = spec ? 11 : rgb ? 9 : 4;
			for (int i = 0; i < count; i++) Device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, all[i], nullptr);
			Device->CreateTexture(8, 8, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &d->park, nullptr);
			d->width = w; d->height = h;
			d->channels = S.Channels;
			d->hasHistory = false;
			for (int i = 0; i < count; i++) if (!*all[i]) return false;
			if (!d->park) return false;
		}
		if (spec && (!d->historyC[0] || !d->historyC[1])) return false;

		// The camera: this frame's basis (as RtSunShadows / SunShadows.glsl), last frame's view-projection and the camera's move since
		const D3DXMATRIX& view = TheRenderManager->viewMatrix;
		const D3DXMATRIX& proj = TheRenderManager->projMatrix;
		const D3DXVECTOR3 camera(TheRenderManager->CameraPosition.x, TheRenderManager->CameraPosition.y, TheRenderManager->CameraPosition.z);
		const D3DXVECTOR3 move = camera - d->prevCamera;
		if (d->rawSeen != rawGeneration) { d->rawSeen = rawGeneration; d->hasHistory = false; }	// UNOFFICIAL 2026-10-30 DLSS RR (above)
		// (2026-11-03) RenderScale < 1: the signal is in the top-left RenderRect part; a scale change (or a rect <-> native frame) restarts
		const float scaleX = RenderRect::On ? RenderRect::ScaleX : 1.0f, scaleY = RenderRect::On ? RenderRect::ScaleY : 1.0f;
		const bool rescaled = scaleX != d->lastScale[0] || scaleY != d->lastScale[1];
		const bool cut = !d->hasHistory || S.Reset || D3DXVec3Length(&move) > 512.0f || rescaled;	// a jump: a camera cut, a load, a teleport, a new RenderScale
		if (cut) SessionLog::HistoryReset("RT denoiser", !d->hasHistory ? "no history" : S.Reset ? "reset asked (cut / mode / size)" : "camera moved over 512 units");	// UNOFFICIAL 2026-11-01

		TemporalPush t = {};
		t.rect[0] = scaleX; t.rect[1] = scaleY; t.rect[2] = rescaled ? scaleX : d->lastScale[0]; t.rect[3] = rescaled ? scaleY : d->lastScale[1];
		d->lastScale[0] = scaleX; d->lastScale[1] = scaleY;
		memcpy(t.prevViewProj, &d->prevViewProj, 64);
		t.forward[0] = view._13; t.forward[1] = view._23; t.forward[2] = view._33; t.forward[3] = cameraData->y;
		t.right[0] = view._11 / proj._11; t.right[1] = view._21 / proj._11; t.right[2] = view._31 / proj._11;
		t.up[0] = view._12 / proj._22; t.up[1] = view._22 / proj._22; t.up[2] = view._32 / proj._22;
		t.move[0] = move.x; t.move[1] = move.y; t.move[2] = move.z; t.move[3] = cut ? 1.0f : 0.0f;
		// (2026-11-07 perf-history) every length in frames is "frames at 60 fps": the same seconds at this frame rate (RtHistoryTime.h)
		const RtHistoryTime::Clock clock = HistoryClock();
		t.params[0] = RtHistoryTime::Frames((std::max)(S.MaxHistory, 1.0f), clock); t.params[1] = S.DepthTolerance; t.params[2] = 1.0f / w; t.params[3] = 1.0f / h;
		t.fastFrames = RtHistoryTime::Frames(RtHistoryTime::kFastFrames, clock, RtHistoryTime::kFastFramesMin);
		t.clampK = (std::max)(S.HistoryClamp, 0.0f);

		GpuProfileScope gpu(*d->timer, Device);
		IDirect3DSurface9* current = nullptr;
		Device->GetRenderTarget(0, &current);	// a pass may not draw into the device's current target: park it on an 8 x 8 texture
		IDirect3DSurface9* park = Surface(d->park);
		Device->SetRenderTarget(0, park);
		// 2026-10-31 DenoiseBackbone: the frame's shared motion + disocclusion (made by the first denoiser of the frame)
		IDirect3DTexture9* shared = nullptr;
		if (TheSettingManager->GetSettingI("Main.Main.RayTracing", "DenoiseBackbone") != 0) {
			const D3DXVECTOR4 basis[3] = { D3DXVECTOR4(t.forward), D3DXVECTOR4(t.right), D3DXVECTOR4(t.up) };
			shared = BuildBackbone(Device, depth, cameraData->y, basis);
		}
		// (2026-11-07 ptaaa-denoiser) Settings.ObjectMotion: the object motion also without the shared backbone (RGB mode)
		const bool motion = (shared || (rgb && S.ObjectMotion)) && ObjectMotion::Ready();
		t.backbone[0] = motion ? 1.0f : 0.0f;
		t.backbone[1] = shared ? 1.0f : 0.0f;
		t.backbone[2] = motion && rgb ? RtHistoryTime::Frames((float)(std::max)(S.MovingHistory, 0), clock) : 0.0f;	// 2026-10-31 (rt-ao-sky) RT GI: GIMovingHistory
		t.backbone[3] = shared && scale == 2 ? 1.0f : 0.0f;	// 2026-10-31 (rt-ao-sky) half size: the mask over the pixel's 2 x 2 full-size ones
		IUnknown* motionImage = motion ? (IUnknown*)ObjectMotion::Texture : (IUnknown*)depth;
		IUnknown* sharedImage = shared ? (IUnknown*)shared : (IUnknown*)depth;
		AtrousPush a = {};
		a.params[1] = S.DepthSigma; a.params[2] = S.NormalPower; a.params[3] = S.ValueSigma;
		a.size[0] = 1.0f / w; a.size[1] = 1.0f / h; a.size[2] = S.Ids ? 1.0f : rgb && S.MeanVariance > 1.0f ? S.MeanVariance : 0.0f; a.size[3] = S.Kernel == 3 ? 1.0f : 0.0f;	// (2026-11-07 ptaaa-denoiser: size.z >= 2 = MeanVariance, RGB mode)
		const int iterations = (std::max)(0, (std::min)(S.Iterations, 5));
		const UInt32 last = d->frame & 1, now = (d->frame + 1) & 1;
		bool ok = true;
		if (!rgb) {
			IDirect3DSurface9* nowSurface = Surface(d->history[now]);
			IUnknown* temporalImages[5] = { Noisy, d->history[last], depth, motionImage, sharedImage };
			ok = nowSurface && Draw(temporal, nowSurface, temporalImages, 5, &t, 192);	// (its block ends before fireflyK)
			if (nowSurface) nowSurface->Release();
			// a-trous: history -> chain[0] -> chain[1] -> ... ; the last one into Output (no iterations: a copy, step 0)
			IDirect3DTexture9* source = d->history[now];
			for (int i = 0; ok && i < (std::max)(iterations, 1); i++) {
				const bool lastPass = i == (std::max)(iterations, 1) - 1;
				IDirect3DSurface9* target = lastPass ? nullptr : Surface(d->chain[i & 1]);
				a.params[0] = iterations ? (float)(1 << i) : 0.0f;
				IUnknown* images[3] = { source, normals, S.Ids ? (IUnknown*)S.Ids : (IUnknown*)normals };	// [2]: a stand-in without ids
				ok = Draw(atrous, lastPass ? Output : target, images, 3, &a, sizeof(a));
				if (target) target->Release();
				source = d->chain[i & 1];
			}
		}
		else {
			// (2026-11-07 ptaaa-denoiser) AntiFirefly: the temporal pass writes A / B into chain[0] / chainB[0] and the clamped excess into
			// scratchB, then RtDenoiseFirefly.frag spreads the excess into history[now] / historyB[now]; off: A / B straight into the history
			// (the excess target: scratchB, 0)
			const bool firefly = S.AntiFirefly > 0.0f && fireflyPipe;
			t.fireflyK = firefly ? S.AntiFirefly : 0.0f;
			IDirect3DSurface9* nowA = Surface(firefly ? d->chain[0] : d->history[now]);
			IDirect3DSurface9* nowB = Surface(firefly ? d->chainB[0] : d->historyB[now]);
			IDirect3DSurface9* excess = Surface(d->scratchB);
			// (2026-11-07) PeopleHistory: the captured normals mark people's pixels (alpha > 1.5); without them (CapturedNormals off) no cap
			// (2026-11-07 ptaaa-firstperson) or the caller's mask (Settings.People: also the view model, > 3.5)
			IDirect3DBaseTexture9* captured = S.PeopleHistory > 0.0f ? (S.People ? S.People : Texture("TESR_CapturedNormalsBuffer")) : nullptr;
			t.peopleCap = captured ? RtHistoryTime::Frames(S.PeopleHistory, clock) : 0.0f;
			t.antiLag = (std::max)(S.AntiLag, 0.0f);
			IUnknown* temporalImages[7] = { Noisy, d->history[last], depth, d->historyB[last], motionImage, sharedImage, captured ? (IUnknown*)captured : (IUnknown*)depth };
			if (!spec) ok = nowA && nowB && excess && Draw(temporalRgb, nowA, temporalImages, 7, &t, sizeof(t), nowB, excess);
			else {	// (2026-11-07 ptaaa-denoiser) the specular temporal pass: + the roughness and history C; its push = t's head + spec before the sampler
				IDirect3DSurface9* nowC = Surface(d->historyC[now]);
				SpecPush ts = {};
				memcpy(&ts, &t, offsetof(TemporalPush, samplerIndex));	// prevViewProj .. rect
				ts.spec[0] = RtHistoryTime::Frames((std::max)(S.MirrorHistory, 1.0f), clock); ts.spec[1] = S.Virtual ? 1.0f : 0.0f;
				ts.clampK = t.clampK; ts.peopleCap = t.peopleCap; ts.antiLag = t.antiLag; ts.fireflyK = t.fireflyK; ts.fastFrames = t.fastFrames;
				IUnknown* specImages[9] = { temporalImages[0], temporalImages[1], temporalImages[2], temporalImages[3], temporalImages[4], temporalImages[5],
					temporalImages[6], S.Roughness, d->historyC[last] };
				ok = nowA && nowB && nowC && excess && Draw(temporalSpec, nowA, specImages, 9, &ts, sizeof(ts), nowB, nowC, excess);
				if (nowC) nowC->Release();
			}
			if (nowA) nowA->Release();
			if (nowB) nowB->Release();
			if (excess) excess->Release();
			if (ok && firefly) {	// (2026-11-07 ptaaa-denoiser) the excess spread over the 5 x 5 round, into the history
				IDirect3DSurface9* history = Surface(d->history[now]);
				IDirect3DSurface9* historyB = Surface(d->historyB[now]);
				FireflyPush f = {};
				f.size[0] = 1.0f / w; f.size[1] = 1.0f / h;
				IUnknown* images[3] = { d->chain[0], d->scratchB, d->chainB[0] };
				ok = history && historyB && Draw(fireflyPipe, history, images, 3, &f, sizeof(f), historyB);
				if (history) history->Release();
				if (historyB) historyB->Release();
			}
			IDirect3DTexture9* sourceA = d->history[now];
			IDirect3DTexture9* sourceB = d->historyB[now];
			IDirect3DSurface9* scratch = Surface(d->scratchB);
			for (int i = 0; ok && i < (std::max)(iterations, 1); i++) {
				const bool lastPass = i == (std::max)(iterations, 1) - 1;
				IDirect3DSurface9* targetA = lastPass ? nullptr : Surface(d->chain[i & 1]);
				IDirect3DSurface9* targetB = lastPass ? nullptr : Surface(d->chainB[i & 1]);
				a.params[0] = iterations ? (float)(1 << i) : 0.0f;
				// (2026-11-07) the guide: the game's frame and the first surface's colour (stand-ins and sigma 0 when not given)
				const bool guide = rgb && !spec && S.Guide && S.GuideAlbedo && S.GuideSigma > 0.0f;
				a.guideSigma = guide ? S.GuideSigma : 0.0f;
				a.specular = (spec ? 1u : 0u) | (rgb && S.AntiFirefly > 0.0f && fireflyPipe ? 2u : 0u);	// (2026-11-07 ptaaa-denoiser) bit 0 specular mode (the roughness in the guide frame's slot), bit 1 the symmetric test (AntiFirefly)
				a.historyFix = RtHistoryTime::Frames((std::max)(S.HistoryFix, 0.0f), clock);	// (2026-11-07 ptaaa-denoiser; perf-history: frames at 60 fps)
				IUnknown* images[5] = { sourceA, normals, sourceB, spec ? (IUnknown*)S.Roughness : guide ? (IUnknown*)S.Guide : (IUnknown*)sourceA,
					guide ? (IUnknown*)S.GuideAlbedo : (IUnknown*)sourceA };
				ok = Draw(atrousRgb, lastPass ? Output : targetA, images, 5, &a, sizeof(a), lastPass ? scratch : targetB);
				if (targetA) targetA->Release();
				if (targetB) targetB->Release();
				sourceA = d->chain[i & 1];
				sourceB = d->chainB[i & 1];
			}
			if (scratch) scratch->Release();
		}
		Device->SetRenderTarget(0, current);
		if (current) current->Release();
		if (park) park->Release();

		d->prevViewProj = TheRenderManager->ViewProjMatrix;
		d->prevCamera = camera;
		d->hasHistory = ok;
		d->frame++;
		return ok;
	}
}
