// UNOFFICIAL 2026-10-26 (NVR DXVK): AMD FidelityFX Super Resolution 2 (FSR 2.2.1) as TAA's resolve, at native resolution.
//
// CREDIT: FSR 2 is AMD's (FidelityFX Super Resolution 2.2.1, GPUOpen, https://github.com/GPUOpen-Effects/FidelityFX-FSR2),
// MIT licence: src\hlsl\NewVegas\Compute\FSR2\LICENSE-FidelityFX-FSR2.txt. AMD's passes and their CPU side live in
// core\Upscaler.cpp since 2026-10-27 (shared with RenderScale, effects\TAAUpscale.cpp); this file builds NVR's inputs for them
// at native resolution and puts the result into the frame.
//
// Needs NVR DXVK (compute through the extension). Without it, or after any failure (logged once), TAA's own resolve runs.
// Settings: [Shaders.TAA.FSR] Enabled / Sharpness / WeaponReactive (TAA must be on; TAA's Jitter and MipBias are used).
//
// Per frame, all at the render (= display) resolution, after TAA's usual inputs are ready (post-tonemap frame in
// TESR_SourceBuffer, the world jittered by BeginJitter):
//   FSRPrep        NVR: camera motion vectors from depth (TAA's reprojection) + FSR-style device depth + reactive (weapon)
//   Upscaler::Fsr2()->Evaluate: FSRLumaMip (NVR), then AMD's FSR2Reconstruct, FSR2DepthClip, FSR2Lock, FSR2Accumulate[Sharpen], FSR2RCAS
//   FSRComposite   NVR: FSR's output into the game target, the first-person weapon put back as rendered
// What FSR does not get (yet): motion of skinned characters and other moving objects (camera motion only; FSR's depth clip and
// luma locks handle what they can), a reactive mask for particles/transparencies. Real upscaling: RenderScale (TAAUpscale.cpp).
//
// UNOFFICIAL 2026-10-30: DLSS Ray Reconstruction's point of the frame (RayReconstruct, below) lives here too: it shares FSRPrep and
// its inputs, and its composite is FSRComposite's rule for the weapon.

#include <algorithm>

#include "TAA.h"
#include "../../core/DxvkCompute.h"
#include "../../core/GpuProfiler.h"
#include "../../core/Upscaler.h"
#include "../../core/ReactiveMask.h"	// UNOFFICIAL 2026-10-30 ReactiveMask: FSR's reactive mask from the world's alpha draws
#include "../../core/ObjectMotion.h"	// UNOFFICIAL 2026-10-29 ObjectMotion: FSR's motion vectors for what moved itself
#include "../../core/RenderRect.h"	// UNOFFICIAL 2026-10-30 DLSS Ray Reconstruction (no RR in rect frames)
#include "../../core/RtPathTrace.h"	// UNOFFICIAL 2026-11-07 (ptaaa-denoiser) RR's guides from the path tracer
#include "../../core/RrRectPlan.h"	// UNOFFICIAL 2026-11-07 (perf-rr) RR at RenderScale < 1: sizes, jitter, the composite's rect mapping

namespace {
	// Constants of FSRPrep.comp (std140).
	struct FSRPrepConstants {
		D3DXMATRIX	projection, view, prevView;
		D3DXVECTOR4	cameraData, prevProjection, cameraDelta, size;
		D3DXVECTOR4	extra;	// UNOFFICIAL 2026-10-30 x: 1 = ReactiveMask::Texture at binding 5, y: ReactiveMaxDistance
	};
	// UNOFFICIAL 2026-10-30 DLSS Ray Reconstruction: the Constants of DlssRRGuides.comp (std140).
	struct RRGuidesConstants {
		D3DXVECTOR4	right, up, forward, view, options, size;
	};

	// Binding tables: (set, binding) as the SPIR-V declares them (checked with spirv-dis), slot = binding for images,
	// buffer slot 0 = the constants; sampler POINT (slot 0, push 0).
#define IMG(b)      { 1, b, NVR_EXT_BIND_SAMPLED_IMAGE, b, NVR_EXT_VIEW_2D, FALSE, 0 }
#define UAV(b)      { 1, b, NVR_EXT_BIND_STORAGE_IMAGE, b, NVR_EXT_VIEW_2D, TRUE, 0 }
#define CB(s, b)    { s, b, NVR_EXT_BIND_UNIFORM_BUFFER, 0, NVR_EXT_VIEW_2D, FALSE, 0 }
#define POINTS      { 0, 0, NVR_EXT_BIND_SAMPLER, 0, NVR_EXT_VIEW_2D, FALSE, 0 }
	const NVR_EXT_BINDING_DESC PrepBindings[] = { IMG(0), IMG(1), UAV(2), UAV(3), CB(2, 0), POINTS, IMG(4), IMG(5) };	// IMG(4): UNOFFICIAL 2026-10-29 TESR_ObjectMotionBuffer; IMG(5): 2026-10-30 ReactiveMask
	const NVR_EXT_BINDING_DESC CompositeBindings[] = { IMG(0), IMG(1), IMG(2), IMG(3), IMG(4), POINTS };
	// UNOFFICIAL 2026-10-30 DLSS Ray Reconstruction: DlssRRGuides.comp (colour, normals, depth, material; the five guides; the constants)
	// and DlssRRComposite.frag (RR's output, the frame before RR, the weapon's depth). Same conventions as above.
	const NVR_EXT_BINDING_DESC RRGuidesBindings[] = { IMG(0), IMG(1), IMG(2), IMG(3), UAV(4), UAV(5), UAV(6), UAV(7), UAV(8), CB(2, 0), POINTS, IMG(9),
		IMG(10), IMG(11), IMG(12) };	// IMG(9): UNOFFICIAL 2026-10-31 TESR_AlbedoBuffer (RRAlbedo 2); IMG(10-12): 2026-11-07 (ptaaa-denoiser) the path tracer's albedos
	const NVR_EXT_BINDING_DESC RRCompositeBindings[] = { IMG(0), IMG(1), IMG(2), POINTS };
#undef IMG
#undef UAV
#undef CB
#undef POINTS

	const D3DXVECTOR4* Registered(const char* name) {
		auto found = TheShaderManager->ConstantsTable.find(name);
		return found == TheShaderManager->ConstantsTable.end() ? nullptr : found->second;
	}
	IDirect3DBaseTexture9* SlotTexture(const char* name) {
		std::string slotName = name;
		IDirect3DBaseTexture9** slot = TheTextureManager->GetTextureSlotByName(slotName);
		return slot ? *slot : nullptr;
	}

	NVR_EXT_RESOURCE_BINDING Image(UINT slot, IUnknown* resource, bool storage = false) {
		NVR_EXT_RESOURCE_BINDING b = {};
		b.Slot = slot; b.Type = storage ? NVR_EXT_BIND_STORAGE_IMAGE : NVR_EXT_BIND_SAMPLED_IMAGE; b.pResource = resource;
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
}

void TAAEffect::FSRReleaseResources() {
	FSRTexture* textures[] = { &fsrMotion, &fsrDepth };
	for (FSRTexture* t : textures) {
		if (t->Surface) { t->Surface->Release(); t->Surface = nullptr; }
		if (t->Texture) { t->Texture->Release(); t->Texture = nullptr; }
	}
	fsrWidth = fsrHeight = 0;
}

// NVR's per-frame inputs for FSR (motion vectors, FSR-style depth + reactive), made on first use and again when the resolution
// changes; FSR's own resources are the upscaler's (core\Upscaler.cpp). Render-target textures so DXVK can add storage use.
bool TAAEffect::FSREnsureResources() {
	const int width = TheRenderManager->width, height = TheRenderManager->height;
	if (fsrWidth == width && fsrHeight == height && fsrMotion.Surface && fsrDepth.Surface) return true;
	FSRReleaseResources();
	TheTextureManager->InitTexture("TESR_FSRMotion", &fsrMotion.Texture, &fsrMotion.Surface, width, height, D3DFMT_G16R16F);
	TheTextureManager->InitTexture("TESR_FSRDepth", &fsrDepth.Texture, &fsrDepth.Surface, width, height, D3DFMT_G32R32F);
	if (!fsrMotion.Surface || !fsrDepth.Surface) {
		Logger::Log("[ERROR] TAA FSR: could not create its motion / depth inputs (%ix%i)", width, height);
		FSRReleaseResources();
		return false;
	}
	fsrWidth = width;
	fsrHeight = height;
	Logger::Log("UNOFFICIAL Vegas Shaders DXVK: TAA FSR 2 inputs made (%ix%i, about %u MB)", width, height, (UInt32)((UInt64)width * height * 12 / (1024 * 1024)));
	return true;
}

bool TAAEffect::FSRWanted() {
	if (!FSRSettings.Enabled || fsrFailed) return false;
	if (!DxvkCompute::Available()) {
		if (!fsrMissingLogged) Logger::Log("UNOFFICIAL: TAA FSR is on but needs the Vegas Shaders DXVK fork (%s); TAA's own resolve runs.", DxvkCompute::Status());
		fsrMissingLogged = true;
		return false;
	}
	if (Upscaler::Fsr2()->Failed()) return false;
	return true;
}

bool TAAEffect::FSRDispatchPrep(const NVR_EXT_RESOURCE_BINDING* bindings, UINT count, const void* push, UINT pushSize, UINT groupsX, UINT groupsY) {
	NVR_EXT_DISPATCH_DESC dispatch = {};
	dispatch.pPipeline = fsrPrep;
	dispatch.BindingCount = count;
	dispatch.pBindings = bindings;
	dispatch.pPushData = push;
	dispatch.PushDataSize = pushSize;
	dispatch.GroupsX = groupsX;
	dispatch.GroupsY = groupsY;
	dispatch.GroupsZ = 1;
	// F10: inside TAA's timer ("TAA" covers the whole FSR path).
	static GpuTimer timer("  FSR prep (NVR)");
	GpuProfileScope gpu(timer, TheRenderManager->device);
	DxvkCompute::PreLoad(bindings, count);	// UNOFFICIAL 2026-10-27 (merge with the alflicker fix): managed textures on the GPU
	if (FAILED(DxvkCompute::Extension()->Dispatch(&dispatch))) {
		Logger::Log("[ERROR] UNOFFICIAL Vegas Shaders DXVK: TAA FSR pass FSRPrep failed to dispatch; FSR is off for this session (TAA's own resolve runs)");
		fsrFailed = true;
		return false;
	}
	return true;
}

// UNOFFICIAL 2026-10-30: FSRDraw's pipelines, made on the first use of either FSRDraw or the RR point (RayReconstruct, which shares FSRPrep),
// exactly as FSRDraw made them: a failure turns FSR off for the session (logged); false then, and while either pipeline is missing.
bool TAAEffect::FSRPipelines() {
	auto fail = [this](const char* why) {
		Logger::Log("[ERROR] UNOFFICIAL Vegas Shaders DXVK: TAA FSR: %s; FSR is off for this session (TAA's own resolve runs)", why);
		EffectStatus::Level("TAA FSR / DLSS (native)", (std::string("OFF for the session, TAA's resolve: ") + why).c_str(), false);	// UNOFFICIAL 2026-11-01
		fsrFailed = true;
		return false;
	};
	if (!fsrTried) {
		fsrTried = true;
		fsrPrep = DxvkCompute::CreatePipeline("FSRPrep", PrepBindings, _countof(PrepBindings), 4, true);
		if (!fsrPrep) return fail("a compute pipeline could not be made (Compute\\FSR*.spv missing or refused)");
		fsrComposite.Tried = true;
		fsrComposite.Pipeline = DxvkCompute::CreateGraphicsPipeline("FSRComposite", CompositeBindings, _countof(CompositeBindings), 16, true);
		if (!fsrComposite.Pipeline) return fail("the composite pass could not be made (Compute\\FSRComposite.spv)");
		QueryPerformanceCounter(&fsrLastTime);
	}
	return fsrPrep && fsrComposite.Pipeline;
}

bool TAAEffect::FSRDraw(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, bool reset) {
	auto fail = [this](const char* why) {
		Logger::Log("[ERROR] UNOFFICIAL Vegas Shaders DXVK: TAA FSR: %s; FSR is off for this session (TAA's own resolve runs)", why);
		EffectStatus::Level("TAA FSR / DLSS (native)", (std::string("OFF for the session, TAA's resolve: ") + why).c_str(), false);	// UNOFFICIAL 2026-11-01
		fsrFailed = true;
		return false;
	};

	if (!FSRPipelines()) return false;	// UNOFFICIAL 2026-10-30: the pipelines' block that was here, unchanged (the RR point shares FSRPrep)
	if (!FSREnsureResources()) return fail("its textures could not be made");

	const D3DXVECTOR4* projection = Registered("TESR_ProjectionTransform");
	const D3DXVECTOR4* view = Registered("TESR_ViewTransform");
	const D3DXVECTOR4* cameraData = Registered("TESR_CameraData");
	IDirect3DBaseTexture9* source = TheTextureManager->SourceTexture;
	IDirect3DBaseTexture9* depth = SlotTexture("TESR_DepthBuffer");
	IDirect3DBaseTexture9* viewModel = SlotTexture("TESR_DepthBufferViewModel");
	ID3D9NvrExtSampler* point = DxvkCompute::PointClamp();
	if (!projection || !view || !cameraData || !source || !depth || !viewModel || !point) return fail("an input is missing");

	const int width = fsrWidth, height = fsrHeight;
	const float nearZ = cameraData->x, farZ = cameraData->y;
	const D3DXMATRIX& proj = *(const D3DXMATRIX*)projection;

	LARGE_INTEGER now, frequency;
	QueryPerformanceCounter(&now);
	QueryPerformanceFrequency(&frequency);
	float deltaTime = (float)(now.QuadPart - fsrLastTime.QuadPart) / (float)frequency.QuadPart;
	fsrLastTime = now;

	// 1. NVR's inputs: motion vectors, FSR depth + reactive.
	FSRPrepConstants prep = {};
	memcpy(&prep.projection, projection, sizeof(D3DXMATRIX));
	memcpy(&prep.view, view, sizeof(D3DXMATRIX));
	prep.prevView = Constants.PrevViewTransform;
	prep.cameraData = *cameraData;
	prep.prevProjection = Constants.PrevProjection;
	prep.cameraDelta = Constants.CameraDelta;
	const bool objectMotion = ObjectMotion::Ready();	// UNOFFICIAL 2026-10-29: cameraDelta.w = FSRPrep.comp's switch for t_objmotion
	prep.cameraDelta.w = objectMotion ? 1.0f : 0.0f;
	prep.size = D3DXVECTOR4((float)width, (float)height, reset ? 1.0f : 0.0f, FSRSettings.WeaponReactive);
	const bool reactive = ReactiveMask::Ready();	// UNOFFICIAL 2026-10-30
	prep.extra = D3DXVECTOR4(reactive ? 1.0f : 0.0f, ReactiveMask::MaxDistance(), 0.0f, 0.0f);
	ID3D9NvrExtBuffer* prepBuffer = ExtUniforms(fsrPrepUniforms, &prep, sizeof(prep));
	if (!prepBuffer) return fail("its constant buffer could not be written");
	{
		NVR_EXT_RESOURCE_BINDING b[] = { Image(0, depth), Image(1, viewModel), Image(2, fsrMotion.Texture, true), Image(3, fsrDepth.Texture, true),
			Buffer(0, NVR_EXT_BIND_UNIFORM_BUFFER, prepBuffer), Sampler(0, point), Image(4, objectMotion ? (IUnknown*)ObjectMotion::Texture : (IUnknown*)depth),
			Image(5, reactive ? (IUnknown*)ReactiveMask::Texture : (IUnknown*)depth) };
		UInt32 prepPush = 0;
		if (!FSRDispatchPrep(b, _countof(b), &prepPush, 4, (width + 7) / 8, (height + 7) / 8)) return false;
	}

	// 2. The upscaler (FSR 2) at render size = display size, on TAA's 8-step jitter.
	Upscaler::Inputs in;
	in.Color = source;
	in.Depth = fsrDepth.Texture;
	in.Motion = fsrMotion.Texture;
	in.ViewModel = viewModel;	// UNOFFICIAL 2026-11-01: DLSS's bias current colour mask on the weapon ([Shaders.TAA.DLSS] WeaponMask)
	in.RenderWidth = in.InputWidth = in.DisplayWidth = (UINT)width;
	in.RenderHeight = in.InputHeight = in.DisplayHeight = (UINT)height;
	// The jitter as FSR defines it (the sample at pixel p shows the unjittered point p - jitter, y down): BeginJitter moves the
	// frustum right/up by (x, y) pixels, i.e. the picture left/down, so FSR's offset is (-x, +y).
	in.JitterX = frameJittered ? -frameJitterX : 0.0f;
	in.JitterY = frameJittered ? frameJitterY : 0.0f;
	in.JitterPhases = 8.0f;					// ffxFsr2GetJitterPhaseCount at scale 1; TAA's Halton sequence is 8 long
	in.NearZ = nearZ;
	in.FarZ = farZ;
	in.TanHalfFovX = 1.0f / proj._11;		// tan(horizontal fov / 2)
	in.TanHalfFovY = 1.0f / proj._22;		// tan(vertical fov / 2)
	in.DeltaSeconds = deltaTime;
	in.Sharpness = FSRSettings.Sharpness;
	in.Hdr = false;							// the tonemapped frame
	in.Reset = reset || !lastPathFSR;		// the upscaler's history is RenderScale's after an upscaled frame
	if (in.Reset && !reset) SessionLog::HistoryReset("TAA FSR / DLSS (native)", "the last frame was upscaled by RenderScale or resolved by TAA");	// UNOFFICIAL 2026-11-01
	Upscaler::IUpscaler* upscaler = Upscaler::Choose(FSRSettings.Upscaler);	// FSR 2 (DLSS is not built: core/Upscaler.h)
	bool evaluated = upscaler->Evaluate(Device, in);
	if (!evaluated && upscaler == Upscaler::Fsr31() && upscaler->Failed()) {
		// UNOFFICIAL 2026-10-28: FSR 3.1 failing (logged by it) hands this frame and the rest of the session to FSR 2.
		upscaler = Upscaler::Choose(FSRSettings.Upscaler);
		in.Reset = true;
		evaluated = upscaler->Evaluate(Device, in);
	}
	if (!evaluated) return fail(upscaler->Failed() ? "the upscaler failed" : "the upscaler could not run this frame");
	EffectStatus::Level("TAA upscaler in use (native)", upscaler->Name(), true);	// UNOFFICIAL 2026-11-01 (the REPORT's path; a fallback shows as its own line)
	// UNOFFICIAL 2026-11-01 (Diagnostics.h part 5): this frame's state for the log lines (Render adds the frame count and probes the output)
	Diag.path = FSRSettings.Upscaler == 1 ? "DLSS at native resolution (FSR 2 while the helper cannot run)" : FSRSettings.Upscaler == 2 ? "FSR 3.1 at native resolution" : "FSR 2 at native resolution";
	Diag.upscaler = std::string(upscaler->Name()) + ": " + upscaler->DiagText();
	Diag.reset = in.Reset;
	Diag.jitter = jitterIndex;
	Diag.jitterX = in.JitterX;
	Diag.jitterY = in.JitterY;
	Diag.deltaMs = deltaTime * 1000.0f;
	Diag.inW = in.RenderWidth; Diag.inH = in.RenderHeight; Diag.outW = in.DisplayWidth; Diag.outH = in.DisplayHeight;
	Diag.hdr = in.Hdr;
	Diag.output = upscaler->Output();

	// 3. Into the game target, the weapon as rendered (DebugView 1 / 5: NVR's motion / depth inputs instead).
	{
		NVR_EXT_RESOURCE_BINDING b[] = { Image(0, upscaler->Output()), Image(1, source), Image(2, viewModel), Image(3, fsrMotion.Texture),
			Image(4, fsrDepth.Texture), Sampler(0, point) };
		struct { UInt32 pointIndex; float debugView; float w, h; } compositePush = { 0, (float)Settings.DebugView, (float)width, (float)height };
		static GpuTimer compositeTimer("  FSR composite (NVR)");
		GpuProfileScope gpu(compositeTimer, Device);
		if (!ExtDrawTo(fsrComposite.Pipeline, &RenderTarget, 1, b, _countof(b), &compositePush, sizeof(compositePush))) return fail("the composite pass failed");
	}

	if (++fsrFrames == 1) Logger::Log("UNOFFICIAL Vegas Shaders DXVK: TAA runs %s (AMD FidelityFX Super Resolution) as its resolve, %ix%i native, sharpness %.2f.", upscaler->Name(), width, height, FSRSettings.Sharpness);
	return true;
}

// --- UNOFFICIAL 2026-10-30 DLSS Ray Reconstruction ------------------------------------------------------------------------------------------
// (docs/next/FORK2-DLSS-RR.md; [Shaders.TAA.DLSS] RayReconstruction, off by default: then RayReconstructionPlanned and RayReconstruct return at
// their first test and nothing below is made or run.) The RR point of the pre-tonemap chain, after the god rays and before the average luma,
// exposure and bloom (ShaderManager::RenderEffectsPreTonemapping), at native resolution only. There every RT composite is in the picture
// (sun, AO / GI through AmbientLight, reflections; the lamps inside the object shaders), and so are the fog, the volumetric light, the
// sandstorm and the god rays: drawn with this frame's jittered depth, they are resolved by RR with the rest (review 2026-10-30: right
// after the Reflections composite, as first built, they came after RR and nothing resolved their jittered edges). Per frame:
//   FSRPrep          motion vectors and FSR-style depth, as DLSS SR gets them, from RR's own previous camera (TAA's is only kept after the
//                    tonemap, later in the frame); the same pipeline and textures as FSRDraw, which does not run in an RR frame
//   DlssRRGuides     the picture decoded to linear, and the guides: diffuse albedo (a stand-in), specular albedo, normals, roughness
//   Upscaler::EvaluateRayReconstruction: DLSS-D in the DLSS helper; its output in Upscaler::RayReconstructionOutput()
//   DlssRRComposite  RR's output encoded back into the chain's curve, the first-person weapon put back as rendered, then Commit
// Render then skips TAA's resolve for the frame (rrRanThisFrame) and only moves the jitter on. Any check failing leaves the frame alone: DLSS
// SR runs after the tonemap as before.

// Every check of the RR point except the frame chain's ownership (RayReconstruct asks that itself, Planned only that the chain runs).
// RR off: false at once, silently. Each other reason is logged once.
bool TAAEffect::RRFrameChecks(bool frameStart) {
	if (!Upscaler::DlssRrSettings().Wanted) return false;
	auto no = [this](UInt32 reason, const char* why) {
		if (!(rrLogged & reason)) {
			rrLogged |= reason;
			Logger::Log("UNOFFICIAL DLSS RR: %s (logged once)", why);
		}
		return false;
	};
	if (rrFailed) return false;	// (logged when it happened)
	if (!FSRSettings.Enabled || FSRSettings.Upscaler != 1) return no(1, "DLSS is not the upscaler: no Ray Reconstruction");
	// (2026-11-07 perf-rr) RenderScale < 1: RR runs in RenderScale v2's rect frames (the effects drawn small, the upscale after them): it
	// denoises the render rect and upscales it to the screen in one step, in place of DLSS SR's upscale. Was: RenderScale 1 only (reason 2).
	// A frame upscaled BEFORE the effects (v1: [Shaders.TAA.FSR] EffectsAfterUpscale, or v2 off for the session after frames dropped) has
	// had DLSS SR already: RR would run on an upscaled picture, so it goes without RR (DLSS SR's frame, as before).
	if (Up.Upscaled)
		return no(2, "this RenderScale frame was upscaled before the effects (v1: [Shaders.TAA.FSR] EffectsAfterUpscale on, or RenderScale v2 off for the session): no Ray Reconstruction (DLSS SR upscaled it)");
	if (!Enabled || !Effect || !ShouldRender()) return no(4, "TAA is off ([Shaders.TAA.Status] Enabled): DLSS and its Ray Reconstruction need it");
	const bool rect = RenderRect::Active();
	if (rect != Up.Deferred) return no(8, "a RenderScale frame whose rect is not the effects' (RenderRect and the deferred upscale disagree): no Ray Reconstruction in it");
	// Every RR pass goes through the extension itself (RRDispatch, ExtDrawTo), drawing / writing what it is given: in a rect frame the
	// render rect (one thread per render pixel) and the whole screen (the composite). DxvkCompute::PassesOn is false in rect frames (the
	// whole-target passes of the effects), so it is not asked there: the setting and the fork are.
	const bool passesOn = rect ? (TheSettingManager->SettingsMain.Main.NvrDxvkPasses && DxvkCompute::Available()) : DxvkCompute::PassesOn();
	if (!passesOn || DxvkCompute::KeptOnD3D9(Name))
		return no(16, "the Vegas Shaders DXVK fork's Vulkan passes are off ([Main.Main.Performance] NvrDxvkPasses, or TAA in NvrDxvkKeepD3D9): no Ray Reconstruction");
	if (!TheSettingManager->SettingsMain.Main.RenderPreTonemapping)
		return no(32, "[Main.Main.Main] RenderPreTonemapping is off (the effects run after the tonemap): no Ray Reconstruction");
	if (!TheSettingManager->SettingsMain.Main.RenderEffects || !Player || !Player->parentCell || TheShaderManager->GameState.OverlayIsOn ||
		InterfaceManager->IsPipBoyOpen())
		return no(64, "not a world frame (a menu, a terminal, the Pip-Boy): those frames go to DLSS SR");
	const bool chainOn = frameStart ? !TheSettingManager->SettingsMain.Main.DisableFrameChain : TheShaderManager->Chain.IsActive();
	if (!chainOn) return no(128, "the frame chain is off ([Main.Main.Performance] FrameChain): no Ray Reconstruction");
	if (fsrFailed) return no(256, "TAA's FSR / DLSS path is off for this session (see above): no Ray Reconstruction");
	const bool ready = Upscaler::RayReconstructionReady();
	if (frameStart) RRLogPlan(rect, ready);	// (perf-rr) the planned sizes, also where the helper cannot run RR (a log dry run)
	if (!ready)
		return no(512, "the DLSS helper cannot evaluate Ray Reconstruction (yet: starting, or no RR on this card / no nvngx_dlssd.dll: the DLSS lines say why); DLSS SR meanwhile");
	return true;
}

// UNOFFICIAL 2026-11-07 (perf-rr) one line with this frame's RR plan (core/RrRectPlan.h: what RayReconstruct sends), logged whenever it changes
// (at most 24 a session): the sizes, the jitter steps, the motion scale, the dispatch, the helper's subrect and whether the helper can run RR.
// On a card without RR (or before the helper is up) this is the dry run: the frame goes to DLSS SR, the line says what RR would have got.
void TAAEffect::RRLogPlan(bool rect, bool ready) {
	const UINT screenW = TheRenderManager->width, screenH = TheRenderManager->height;
	const RrRect::Plan plan = RrRect::Make(rect, screenW, screenH, Up.RenderWidth, Up.RenderHeight, Up.Jittered, Up.JitterX, Up.JitterY, Up.JitterPhases,
		frameJittered, frameJitterX, frameJitterY, (float)JitterSequenceLength);
	const char* problem = RrRect::Problem(plan);
	char line[640];
	sprintf_s(line, "UNOFFICIAL DLSS RR plan: %s, render %ux%u -> output %ux%u (%.0f%% of the pixels; RenderScale %.2f), inputs %ux%u (the render rect at 0, 0 = "
		"the helper's subrect), jitter %s over %.0f steps, motion x %.0f / %.0f (render pixels), guides + prep %ux%u groups, composite %s; the helper %s%s%s",
		rect ? "RenderScale rect frame (RR denoises AND upscales; DLSS SR's upscale step is skipped in its frames)" : "native frame (RR denoises; no upscale)",
		plan.renderW, plan.renderH, plan.displayW, plan.displayH, 100.0 * plan.renderW * plan.renderH / ((double)plan.displayW * plan.displayH), FSRSettings.RenderScale,
		plan.inputW, plan.inputH, rect ? "RenderScale's (Halton, render pixels)" : "TAA's (Halton)", plan.jitterPhases, plan.mvScaleX, plan.mvScaleY, plan.groupsX, plan.groupsY,
		rect ? "reads the rect at pixel x scale" : "pixel for pixel", ready ? "runs RR" : "cannot run RR here (dry run: DLSS SR takes these frames)",
		problem ? "; PROBLEM: " : "", problem ? problem : "");
	if (rrPlanLogged == line || rrPlanLogs >= 24) return;
	rrPlanLogged = line;
	rrPlanLogs++;
	Logger::Log("%s", line);
}

bool TAAEffect::RayReconstructionPlanned() {
	TAAEffect* taa = TheShaderManager ? TheShaderManager->Effects.TAA : nullptr;
	if (!taa) return false;
	taa->rrPlanned = taa->RRFrameChecks(true);
	Upscaler::RayReconstructionPlan(taa->rrPlanned);
	return taa->rrPlanned;
}

void TAAEffect::RRReleaseResources() {
	FSRTexture* textures[] = { &rrColor, &rrDiffuse, &rrSpecular, &rrNormals, &rrRoughness };
	for (FSRTexture* t : textures) {
		if (t->Surface) { t->Surface->Release(); t->Surface = nullptr; }
		if (t->Texture) { t->Texture->Release(); t->Texture = nullptr; }
	}
	rrWidth = rrHeight = 0;
}

// DlssRRGuides' outputs at the screen's size, made on RR's first frame and again when the resolution changes (render-target textures, as
// FSREnsureResources makes FSR's, so DXVK can add storage use; the DLSS helper's copies read them).
bool TAAEffect::RREnsureResources() {
	const int width = TheRenderManager->width, height = TheRenderManager->height;
	if (rrWidth == width && rrHeight == height && rrColor.Surface && rrDiffuse.Surface && rrSpecular.Surface && rrNormals.Surface && rrRoughness.Surface) return true;
	RRReleaseResources();
	TheTextureManager->InitTexture("TESR_DlssRRColor", &rrColor.Texture, &rrColor.Surface, width, height, D3DFMT_A16B16G16R16F);
	TheTextureManager->InitTexture("TESR_DlssRRDiffuseAlbedo", &rrDiffuse.Texture, &rrDiffuse.Surface, width, height, D3DFMT_A16B16G16R16F);
	TheTextureManager->InitTexture("TESR_DlssRRSpecularAlbedo", &rrSpecular.Texture, &rrSpecular.Surface, width, height, D3DFMT_A16B16G16R16F);
	TheTextureManager->InitTexture("TESR_DlssRRNormals", &rrNormals.Texture, &rrNormals.Surface, width, height, D3DFMT_A16B16G16R16F);
	TheTextureManager->InitTexture("TESR_DlssRRRoughness", &rrRoughness.Texture, &rrRoughness.Surface, width, height, D3DFMT_R16F);
	if (!rrColor.Surface || !rrDiffuse.Surface || !rrSpecular.Surface || !rrNormals.Surface || !rrRoughness.Surface) {
		Logger::Log("[ERROR] DLSS Ray Reconstruction: could not create its guide textures (%ix%i)", width, height);
		RRReleaseResources();
		return false;
	}
	rrWidth = width;
	rrHeight = height;
	Logger::Log("UNOFFICIAL Vegas Shaders DXVK: DLSS Ray Reconstruction inputs made (%ix%i, about %u MB)", width, height, (UInt32)((UInt64)width * height * 34 / (1024 * 1024)));
	return true;
}

// RR's own two pipelines, made once. DxvkCompute::CreatePipeline turns every Vulkan pass off when its .spv is missing, so the files are
// looked for first: without them only RR is off (logged).
bool TAAEffect::RRPipelines() {
	if (!rrTried) {
		rrTried = true;
		auto fail = [this](const char* why) {
			Logger::Log("[ERROR] UNOFFICIAL Vegas Shaders DXVK: DLSS Ray Reconstruction: %s; RR is off for this session (DLSS SR runs after the tonemap as before)", why);
			EffectStatus::Level("DLSS Ray Reconstruction", (std::string("OFF for the session: ") + why).c_str(), false);	// UNOFFICIAL 2026-11-01
			rrFailed = true;
			return false;
		};
		if (!DxvkCompute::HasSpirv("DlssRRGuides") || !DxvkCompute::HasSpirv("DlssRRComposite"))
			return fail("Compute\\DlssRRGuides.spv or DlssRRComposite.spv (in Shaders\\VegasShaders) is missing");
		rrGuides = DxvkCompute::CreatePipeline("DlssRRGuides", RRGuidesBindings, _countof(RRGuidesBindings), 4, true);
		if (!rrGuides) return fail("its compute pipeline could not be made (Compute\\DlssRRGuides.spv refused)");
		rrComposite.Tried = true;
		// (2026-11-07 perf-rr: push block 32 bytes, + the rect mapping)
		rrComposite.Pipeline = DxvkCompute::CreateGraphicsPipeline("DlssRRComposite", RRCompositeBindings, _countof(RRCompositeBindings), 32, true, 1, NVR_EXT_BLEND_NONE, false);
		if (!rrComposite.Pipeline) return fail("its composite pass could not be made (Compute\\DlssRRComposite.spv)");
	}
	return rrGuides && rrComposite.Pipeline;
}

// One of RR's compute passes (FSRPrep, DlssRRGuides): push block = the point sampler's heap index (the extension writes it).
bool TAAEffect::RRDispatch(ID3D9NvrExtPipeline* pipeline, const NVR_EXT_RESOURCE_BINDING* bindings, UINT count, UINT groupsX, UINT groupsY) {
	UInt32 push = 0;
	NVR_EXT_DISPATCH_DESC dispatch = {};
	dispatch.pPipeline = pipeline;
	dispatch.BindingCount = count;
	dispatch.pBindings = bindings;
	dispatch.pPushData = &push;
	dispatch.PushDataSize = 4;
	dispatch.GroupsX = groupsX;
	dispatch.GroupsY = groupsY;
	dispatch.GroupsZ = 1;
	DxvkCompute::PreLoad(bindings, count);	// (the alflicker rule: managed textures on the GPU first)
	return SUCCEEDED(DxvkCompute::Extension()->Dispatch(&dispatch));
}

bool TAAEffect::RayReconstruct(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget) {
	rrRanThisFrame = false;
	const bool planned = rrPlanned;
	rrPlanned = false;
	if (!Upscaler::DlssRrSettings().Wanted) {
		// Off (the default): nothing runs. Switched off after it ran: its textures go (made again on its next frame), its reasons may log again.
		if (rrWidth) RRReleaseResources();
		rrHistoryValid = false;
		rrLogged = 0;
		return false;
	}
	FrameChain& chain = TheShaderManager->Chain;
	if (!planned || !RRFrameChecks() || !chain.Owns(RenderTarget, TheTextureManager->RenderedSurface)) {
		rrHistoryValid = false;
		// (review 2026-10-30) wanted but not running for 600 frames (the helper stopped, RR failed, the Vulkan passes went off...): the
		// guide textures (about 34 bytes a pixel) go; made again on RR's next frame. A counter, so a Pip-Boy look does not free and remake them.
		if (rrWidth && ++rrIdleFrames >= 600) RRReleaseResources();
		return false;
	}
	auto fail = [this](const char* why) {
		Logger::Log("[ERROR] UNOFFICIAL Vegas Shaders DXVK: DLSS Ray Reconstruction: %s; RR is off for this session (DLSS SR runs after the tonemap as before)", why);
		EffectStatus::Level("DLSS Ray Reconstruction", (std::string("OFF for the session: ") + why).c_str(), false);	// UNOFFICIAL 2026-11-01
		rrFailed = true;
		rrHistoryValid = false;
		RRReleaseResources();	// (review 2026-10-30) never used again this session
		return false;
	};
	if (!FSRPipelines() || !RRPipelines()) { rrHistoryValid = false; return false; }	// (each logged its failure)
	if (!FSREnsureResources()) return fail("FSR's motion / depth textures could not be made");
	if (!RREnsureResources()) return fail("its guide textures could not be made");

	const D3DXVECTOR4* projection = Registered("TESR_ProjectionTransform");
	const D3DXVECTOR4* view = Registered("TESR_ViewTransform");
	const D3DXVECTOR4* cameraData = Registered("TESR_CameraData");
	const D3DXVECTOR4* toneMapping = Registered("TESR_ToneMapping");
	const D3DXVECTOR4* materialData = Registered("TESR_MaterialBufferData");
	IDirect3DBaseTexture9* color = TheTextureManager->RenderedTexture;	// the chain's current image: the frame after the god rays
	IDirect3DBaseTexture9* depth = SlotTexture("TESR_DepthBuffer");
	IDirect3DBaseTexture9* normals = SlotTexture("TESR_NormalsBuffer");
	IDirect3DBaseTexture9* viewModel = SlotTexture("TESR_DepthBufferViewModel");
	IDirect3DBaseTexture9* material = SlotTexture("TESR_MaterialBuffer");
	ID3D9NvrExtSampler* point = DxvkCompute::PointClamp();
	if (!projection || !view || !cameraData || !color || !depth || !normals || !viewModel || !point) return fail("an input is missing");

	const int width = rrWidth, height = rrHeight;
	// UNOFFICIAL 2026-11-07 (perf-rr) a RenderScale v2 rect frame: everything before this point drew the top-left render rect of the screen-size
	// targets; FSRPrep and DlssRRGuides run one thread per render pixel (they read and write that rect: texelFetch at the pixel, the view ray's
	// uv over the render size), DLSS-D gets render size -> screen size on RenderScale's jitter, and the composite fills the whole screen. A
	// native frame: render = screen, TAA's jitter, as before. (core/RrRectPlan.h; RRFrameChecks made sure rect == Up.Deferred.)
	const bool rect = RenderRect::Active() && Up.Deferred;
	const RrRect::Plan plan = RrRect::Make(rect, (unsigned)width, (unsigned)height, Up.RenderWidth, Up.RenderHeight, Up.Jittered, Up.JitterX, Up.JitterY,
		Up.JitterPhases, frameJittered, frameJitterX, frameJitterY, (float)JitterSequenceLength);
	if (const char* problem = RrRect::Problem(plan)) {
		rrHistoryValid = false;
		static bool logged = false;
		if (!logged) Logger::Log("UNOFFICIAL DLSS RR: a frame left to DLSS SR: %s (render %ux%u, inputs %ux%u, output %ux%u; logged once)", problem, plan.renderW, plan.renderH,
			plan.inputW, plan.inputH, plan.displayW, plan.displayH);
		logged = true;
		return false;
	}
	const UINT rw = plan.renderW, rh = plan.renderH;
	const D3DXMATRIX& proj = *(const D3DXMATRIX*)projection;	// TheRenderManager->projMatrix: unjittered here (the image-space hook rebuilt the camera)
	const D3DXMATRIX& viewMatrix = *(const D3DXMATRIX*)view;	// TheRenderManager->viewMatrix
	const float nearZ = cameraData->x, farZ = cameraData->y;
	// The chain's curve (TESR_ToneMapping.w, what the game's tonemap decodes with); 2.2 if it is not there.
	const float linearization = toneMapping && toneMapping->w > 0.0f ? toneMapping->w : 2.2f;
	// The material buffer's roughness ([Main.Main.Quality] MaterialBuffer) only when it holds this frame's, at the screen's size.
	bool materialOn = materialData && materialData->x > 0.5f && material && material->GetType() == D3DRTYPE_TEXTURE;
	if (materialOn) {
		D3DSURFACE_DESC desc = {};
		materialOn = SUCCEEDED(static_cast<IDirect3DTexture9*>(material)->GetLevelDesc(0, &desc)) && desc.Width == (UINT)width && desc.Height == (UINT)height;
	}
	// UNOFFICIAL 2026-10-31 AlbedoBuffer (RRAlbedo 2): the templates' own base colour ([Main.Main.Quality] AlbedoBuffer) when it holds this frame's, at the screen's size
	const D3DXVECTOR4* albedoData = Registered("TESR_AlbedoBufferData");
	IDirect3DBaseTexture9* albedo = SlotTexture("TESR_AlbedoBuffer");
	bool albedoOn = RRSettings.Albedo == 2 && albedoData && albedoData->x > 0.5f && albedo && albedo->GetType() == D3DRTYPE_TEXTURE;
	if (albedoOn) {
		D3DSURFACE_DESC desc = {};
		albedoOn = SUCCEEDED(static_cast<IDirect3DTexture9*>(albedo)->GetLevelDesc(0, &desc)) && desc.Width == (UINT)width && desc.Height == (UINT)height;
	}
	if (RRSettings.Albedo == 2 && !albedoOn && !rrAlbedoMissingLogged) {
		rrAlbedoMissingLogged = true;
		Logger::Log("UNOFFICIAL DLSS RR: RRAlbedo 2 asked for the albedo buffer, but TESR_AlbedoBuffer holds nothing this frame ([Main.Main.Quality] AlbedoBuffer off, or not captured): the picture's chroma stands in (logged once)");
	}

	// RR's own previous camera and reset rule, as RenderScale's UpscaleRun: the history goes on only from the last frame if it went to RR too,
	// at the same size, in the same cell, recently.
	LARGE_INTEGER now, frequency;
	QueryPerformanceCounter(&now);
	QueryPerformanceFrequency(&frequency);
	const float deltaTime = rrLastTime.QuadPart ? (float)(now.QuadPart - rrLastTime.QuadPart) / (float)frequency.QuadPart : 0.0f;
	rrLastTime = now;
	// (perf-rr: + the render size, which changes with RenderScale and between rect and native frames)
	const bool reset = !rrHistoryValid || TheShaderManager->GameState.isCameraCut || width != rrLastWidth || height != rrLastHeight || rw != rrLastRenderWidth ||
		rh != rrLastRenderHeight || deltaTime > 0.5f;
	if (reset) SessionLog::HistoryReset("DLSS Ray Reconstruction", !rrHistoryValid ? "no history" : TheShaderManager->GameState.isCameraCut ? "camera cut" :
		deltaTime > 0.5f ? "frame gap over 0.5 s" : "size");	// UNOFFICIAL 2026-11-01
	if (reset) {
		rrPrevView = TheRenderManager->viewMatrix;
		rrPrevProjection = D3DXVECTOR4(TheRenderManager->projMatrix._11, TheRenderManager->projMatrix._22, 0.0f, 0.0f);
		rrPrevCameraPosition = TheRenderManager->CameraPosition;
	}

	static GpuTimer rrTimer("DLSS Ray Reconstruction");
	GpuProfileScope gpu(rrTimer, Device);

	// 1. Motion vectors and FSR-style depth + the weapon's reactive value, as FSRDraw makes them, from RR's previous camera.
	FSRPrepConstants prep = {};
	memcpy(&prep.projection, projection, sizeof(D3DXMATRIX));
	memcpy(&prep.view, view, sizeof(D3DXMATRIX));
	prep.prevView = rrPrevView;
	prep.cameraData = *cameraData;
	prep.prevProjection = rrPrevProjection;
	const D3DXVECTOR4 delta = TheRenderManager->CameraPosition - rrPrevCameraPosition;
	const bool objectMotion = ObjectMotion::Ready();	// cameraDelta.w = FSRPrep.comp's switch for t_objmotion
	prep.cameraDelta = D3DXVECTOR4(delta.x, delta.y, delta.z, objectMotion ? 1.0f : 0.0f);
	// (perf-rr) the render size: in a rect frame the rect of the screen-size depth / weapon depth, motion in uv of the render picture
	prep.size = D3DXVECTOR4((float)rw, (float)rh, reset ? 1.0f : 0.0f, FSRSettings.WeaponReactive);
	ID3D9NvrExtBuffer* prepBuffer = ExtUniforms(rrPrepUniforms, &prep, sizeof(prep));
	if (!prepBuffer) return fail("its constant buffers could not be written");
	{
		NVR_EXT_RESOURCE_BINDING b[] = { Image(0, depth), Image(1, viewModel), Image(2, fsrMotion.Texture, true), Image(3, fsrDepth.Texture, true),
			Buffer(0, NVR_EXT_BIND_UNIFORM_BUFFER, prepBuffer), Sampler(0, point), Image(4, objectMotion ? (IUnknown*)ObjectMotion::Texture : (IUnknown*)depth) };
		static GpuTimer prepTimer("  RR prep (FSRPrep)");
		GpuProfileScope gpuPrep(prepTimer, Device);
		if (!RRDispatch(fsrPrep, b, _countof(b), plan.groupsX, plan.groupsY)) return fail("FSRPrep failed to dispatch");
	}

	// 2. The linear picture and the guides.
	RRGuidesConstants guides = {};
	guides.right = D3DXVECTOR4(viewMatrix._11, viewMatrix._21, viewMatrix._31, 0.0f);
	guides.up = D3DXVECTOR4(viewMatrix._12, viewMatrix._22, viewMatrix._32, 0.0f);
	guides.forward = D3DXVECTOR4(viewMatrix._13, viewMatrix._23, viewMatrix._33, 0.0f);
	guides.view = D3DXVECTOR4(1.0f / proj._11, 1.0f / proj._22, linearization, RRSettings.Roughness);
	// UNOFFICIAL 2026-11-07 (ptaaa-denoiser) the path traced picture is the scene (PTRealtime): RR's albedos and roughness are the path
	// tracer's own (what it divided its light by), w 1 diffuse only, 2 + specular and roughness (PTDenoiseSplit)
	IDirect3DTexture9 *ptAlbedo = nullptr, *ptSpecRG = nullptr, *ptSpecB = nullptr;
	const bool ptGuides = RtPathTrace::RrGuides(&ptAlbedo, &ptSpecRG, &ptSpecB);
	const float ptMode = ptGuides ? (ptSpecRG && ptSpecB ? 2.0f : 1.0f) : 0.0f;
	static UInt32 ptGuideLogs = 0;
	if (ptMode != rrPtGuideMode) {
		rrPtGuideMode = ptMode;
		if (++ptGuideLogs <= 40) Logger::Log("UNOFFICIAL DLSS RR: the guides' albedo %s", ptMode > 1.5f ? "and roughness are the path tracer's (diffuse + specular, PTDenoiseSplit)" :
			ptMode > 0.5f ? "is the path tracer's diffuse one (the specular albedo and roughness: the G-buffer's)" : "and roughness are the G-buffer's (no path traced picture)");
	}
	guides.options = D3DXVECTOR4(albedoOn ? 2.0f : RRSettings.Albedo >= 1 ? 1.0f : 0.0f, RRSettings.Normals == 1 ? 1.0f : 0.0f, materialOn ? 1.0f : 0.0f, ptMode);	// x 2: the albedo buffer (2026-10-31)
	guides.size = D3DXVECTOR4((float)rw, (float)rh, 0.0f, 0.0f);	// (perf-rr) the render size: one thread per render pixel, the rect of every image
	ID3D9NvrExtBuffer* guidesBuffer = ExtUniforms(rrGuidesUniforms, &guides, sizeof(guides));
	if (!guidesBuffer) return fail("its constant buffers could not be written");
	{
		NVR_EXT_RESOURCE_BINDING b[] = { Image(0, color), Image(1, normals), Image(2, depth), Image(3, materialOn ? (IUnknown*)material : (IUnknown*)depth),
			Image(4, rrColor.Texture, true), Image(5, rrDiffuse.Texture, true), Image(6, rrSpecular.Texture, true), Image(7, rrNormals.Texture, true),
			Image(8, rrRoughness.Texture, true), Buffer(0, NVR_EXT_BIND_UNIFORM_BUFFER, guidesBuffer), Sampler(0, point),
			Image(9, albedoOn ? (IUnknown*)albedo : (IUnknown*)depth),	// UNOFFICIAL 2026-10-31 AlbedoBuffer (a placeholder the shader never reads below RRAlbedo 2)
			Image(10, ptAlbedo ? (IUnknown*)ptAlbedo : (IUnknown*)depth), Image(11, ptSpecRG ? (IUnknown*)ptSpecRG : (IUnknown*)depth),	// (2026-11-07 ptaaa-denoiser)
			Image(12, ptSpecB ? (IUnknown*)ptSpecB : (IUnknown*)depth) };
		static GpuTimer guidesTimer("  RR guides");
		GpuProfileScope gpuGuides(guidesTimer, Device);
		if (!RRDispatch(rrGuides, b, _countof(b), plan.groupsX, plan.groupsY)) return fail("DlssRRGuides failed to dispatch");
	}

	// 3. DLSS-D. Native: render size = display size, on TAA's 8-step jitter (FSRDraw's sign convention: BeginJitter moved the picture left/down).
	// (perf-rr) Rect frame: the render rect (the helper copies it at 0, 0 and gives DLSS-D that subrect, as for DLSS SR) -> the screen, on
	// RenderScale's jitter (Up: Halton over Up.JitterPhases steps in render pixels, the same sign); the motion scale is the render size (Exchange).
	Upscaler::RrInputs in;
	in.Color = rrColor.Texture;
	in.Depth = fsrDepth.Texture;
	in.Motion = fsrMotion.Texture;
	in.RenderWidth = plan.renderW;
	in.RenderHeight = plan.renderH;
	in.InputWidth = plan.inputW;
	in.InputHeight = plan.inputH;
	in.DisplayWidth = plan.displayW;
	in.DisplayHeight = plan.displayH;
	in.JitterX = plan.jitterX;
	in.JitterY = plan.jitterY;
	in.JitterPhases = plan.jitterPhases;
	in.NearZ = nearZ;
	in.FarZ = farZ;
	in.TanHalfFovX = 1.0f / proj._11;
	in.TanHalfFovY = 1.0f / proj._22;
	in.DeltaSeconds = deltaTime;
	in.Hdr = true;							// the decoded picture, before the tonemap
	in.Reset = reset;
	in.DiffuseAlbedo = rrDiffuse.Texture;
	in.SpecularAlbedo = rrSpecular.Texture;
	in.Normals = rrNormals.Texture;
	in.Roughness = rrRoughness.Texture;
	// The camera: the view's rotation only (camera-relative), and the projection with the depth terms of the depth RR is given. The game's
	// projMatrix is reversed-Z (_33 = -n / (f - n), _43 = n f / (f - n)); FSRPrep's depth is the standard d = f / (f - n) * (1 - n / z), 0
	// near .. 1 far, so ViewToClip carries that one (_33 = f / (f - n), _43 = -n f / (f - n)) and the two agree. Both D3DX row-vector layout.
	D3DXMATRIX worldToView = viewMatrix;
	worldToView._41 = worldToView._42 = worldToView._43 = 0.0f;
	D3DXMATRIX viewToClip = proj;
	viewToClip._33 = farZ / (farZ - nearZ);
	viewToClip._43 = -nearZ * farZ / (farZ - nearZ);
	memcpy(in.WorldToView, &worldToView, sizeof(in.WorldToView));
	memcpy(in.ViewToClip, &viewToClip, sizeof(in.ViewToClip));
	if (!Upscaler::EvaluateRayReconstruction(Device, in)) {
		// Nothing was recorded (logged by the upscaler): the frame is untouched, and DLSS SR runs after the tonemap.
		rrHistoryValid = false;
		return false;
	}
	IDirect3DTexture9* output = Upscaler::RayReconstructionOutput();
	if (!output) return fail("there is no output after the evaluate");

	// 4. Into the frame chain: RR's output in the chain's curve, the weapon as rendered.
	{
		IDirect3DSurface9* target = chain.Output();
		NVR_EXT_RESOURCE_BINDING b[] = { Image(0, output), Image(1, color), Image(2, viewModel), Sampler(0, point) };
		// (review 2026-10-30) rawFrame: the RT effects handed RR raw signals this frame, the weapon's too: it takes RR's output then.
		// (2026-11-07 perf-rr) rect: the frame before RR (its alpha, the weapon test and colour) is the render rect: read at the screen pixel's
		// centre x scale (DlssRRComposite.frag; core/RrRectPlan.h RectPoint / NearestRectPixel); RR's output is the whole screen either way.
		struct { UInt32 pointIndex; float invLinearization; UInt32 rawFrame, rect; float scaleX, scaleY, renderW, renderH; } push = { 0, 1.0f / linearization,
			Upscaler::RayReconstructionWantsRaw() ? 1u : 0u, rect ? 1u : 0u, plan.scaleX, plan.scaleY, (float)plan.renderW, (float)plan.renderH };
		static_assert(sizeof(push) == 32, "DlssRRComposite.frag's push block");
		static GpuTimer compositeTimer("  RR composite");
		GpuProfileScope gpuComposite(compositeTimer, Device);
		if (!ExtDrawTo(rrComposite.Pipeline, &target, 1, b, _countof(b), &push, sizeof(push))) return fail("the composite pass failed");
	}
	chain.Commit();
	Upscaler::RayReconstructionDelivered();	// (review 2026-10-30) only now: next frame's WantsRaw goes by a delivered RR frame
	rrIdleFrames = 0;
	// (perf-rr) the rect frame is upscaled now: its end here (the rect off for the chain's last steps, depths full size), and nothing pends
	// for the image-space hook's UpscaleAfterEffects (no DLSS SR upscale after this)
	if (rect) UpscaleAfterRayReconstruction(Device, RenderTarget);

	rrPrevView = TheRenderManager->viewMatrix;
	rrPrevProjection = D3DXVECTOR4(TheRenderManager->projMatrix._11, TheRenderManager->projMatrix._22, 0.0f, 0.0f);
	rrPrevCameraPosition = TheRenderManager->CameraPosition;
	rrLastWidth = width;
	rrLastHeight = height;
	rrLastRenderWidth = rw;
	rrLastRenderHeight = rh;
	rrHistoryValid = true;
	rrRanThisFrame = true;
	// UNOFFICIAL 2026-11-01 (Diagnostics.h part 5) the history watch's state, as the other resolve paths keep it
	Diag.path = rect ? "DLSS Ray Reconstruction at RenderScale (denoise + upscale in one step, after the god rays)" : "DLSS Ray Reconstruction at native resolution";
	Diag.upscaler.clear();
	Diag.reset = reset;
	Diag.sinceReset = reset ? 0 : Diag.sinceReset + 1;
	Diag.jitter = rect ? Up.JitterIndex : jitterIndex;
	Diag.jitterX = in.JitterX;
	Diag.jitterY = in.JitterY;
	Diag.deltaMs = deltaTime * 1000.0f;
	Diag.inW = rw; Diag.inH = rh; Diag.outW = (unsigned)width; Diag.outH = (unsigned)height;
	Diag.hdr = true;
	Diag.output = output;
	Diag.frame = TheFrameRateManager ? TheFrameRateManager->FrameNumber : 0;
	static int loggedMode = -1;
	static UINT loggedW = 0, loggedH = 0;
	if (++rrFrames == 1 || loggedMode != (int)rect || loggedW != rw || loggedH != rh) {
		if (rrFrames == 1)
			Logger::Log("UNOFFICIAL Vegas Shaders DXVK: DLSS Ray Reconstruction runs after the god rays, before exposure and bloom (albedo %s, normals in %s space, roughness %s).",
				albedoOn ? "the albedo buffer where written, else the picture's chroma" : RRSettings.Albedo >= 1 ? "the picture's chroma" : "flat grey", RRSettings.Normals == 1 ? "view" : "world",
				materialOn ? "from the material buffer" : "constant");
		static UInt32 modeLogs = 0;
		if (++modeLogs <= 24) {
			if (rect) Logger::Log("UNOFFICIAL DLSS RR: denoises AND upscales at RenderScale %.2f: %ux%u -> %ux%u in one step (jitter %.0f steps, RenderScale's); DLSS SR's upscale step is "
				"skipped in these frames, the effects after RR (exposure, bloom, lens) run at %ux%u (frame %u).", FSRSettings.RenderScale, rw, rh, (unsigned)width, (unsigned)height, plan.jitterPhases,
				(unsigned)width, (unsigned)height, Diag.frame);
			else Logger::Log("UNOFFICIAL DLSS RR: denoises at native resolution %ux%u (jitter %.0f steps, TAA's) (frame %u).", rw, rh, plan.jitterPhases, Diag.frame);
		}
		loggedMode = (int)rect; loggedW = rw; loggedH = rh;
	}
	return true;
}
