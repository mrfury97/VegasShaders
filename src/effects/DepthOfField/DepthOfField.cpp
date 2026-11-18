#include "DepthOfField.h"
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-25 (NVR DXVK)

void DepthOfFieldEffect::UpdateConstants() {
	TheShaderManager->avglumaRequired = true;

	// the settings group for the camera that is active: vanity, first person, otherwise third person
	const ValuesStruct* view = &Settings.FirstPerson;
	if (TheCameraManager->IsVanity())
		view = &Settings.VanityView;
	else if (!TheCameraManager->IsFirstPerson())
		view = &Settings.ThirdPerson;

	const auto& game = TheShaderManager->GameState;
	const bool dialog = game.isDialog;

	// the mode can switch the effect off for the current situation
	bool suppressed = false;
	switch (view->Mode) {
	case 1: suppressed = dialog; break;
	case 2: suppressed = !dialog; break;
	case 3: case 4: suppressed = true; break;	// (2026-11-05) Oblivion's persuasion tests: New Vegas has no persuasion menu, so always off
	}
	const int active = suppressed ? 0 : (int)view->Enabled;

	Constants.Enabled = active;

	Constants.Blur = D3DXVECTOR4(view->DistantBlur, view->DistantBlurStartRange, view->DistantBlurEndRange, view->BaseBlurRadius);
	Constants.Data = D3DXVECTOR4(view->focusDistance * active, view->Radius, view->DiameterRange, view->NearBlurCutOff);
	// UNOFFICIAL 2026-11-01 JitterFix ([Shaders.DepthOfField.Status]; a tester's report, DLSS / FSR at native size, third person with
	// DistantBlur: blocks flickering along roofs and the skyline): this effect runs after TAA / FSR / DLSS, whose picture is unjittered,
	// but TESR_DepthBuffer still holds this frame's jittered depth. Along every edge against the sky the blur amounts moved by a fraction
	// of a pixel each frame, and the half-size mosaic made that a flickering block. On: the depth is read where the picture's pixel is.
	const D3DXVECTOR2 jitter = JitterFix && TheShaderManager->Effects.TAA ? TheShaderManager->Effects.TAA->PostResolveDepthJitterUV() : D3DXVECTOR2(0.0f, 0.0f);
	Constants.Jitter = D3DXVECTOR4(jitter.x, jitter.y, 0.0f, 0.0f);

}

// One camera group of the settings file. The setting named BlurFallOff feeds focusDistance.
static void ReadDofView(const char* section, DepthOfFieldEffect::ValuesStruct& out) {
	SettingManager* cfg = TheSettingManager;
	out.DistantBlur = cfg->GetSettingF(section, "DistantBlur");
	out.DistantBlurStartRange = cfg->GetSettingF(section, "DistantBlurStartRange");
	out.DistantBlurEndRange = cfg->GetSettingF(section, "DistantBlurEndRange");
	out.BaseBlurRadius = cfg->GetSettingF(section, "BaseBlurRadius");
	out.focusDistance = cfg->GetSettingF(section, "BlurFallOff");
	out.Radius = cfg->GetSettingF(section, "Radius");
	out.DiameterRange = cfg->GetSettingF(section, "DiameterRange");
	out.NearBlurCutOff = cfg->GetSettingF(section, "NearBlurCutOff");
	out.Mode = cfg->GetSettingI(section, "Mode");
	out.Enabled = cfg->GetSettingI(section, "Enabled");
}

void DepthOfFieldEffect::UpdateSettings() {
	{	// UNOFFICIAL 2026-11-01 (missing key, an older toml: on)
		SettingManager::Configuration::ConfigNode node;
		JitterFix = !TheSettingManager->Config.FillNode(&node, "Shaders.DepthOfField.Status", "JitterFix") || node.BoolValue;
	}
	ReadDofView("Shaders.DepthOfField.FirstPersonView", Settings.FirstPerson);
	ReadDofView("Shaders.DepthOfField.ThirdPersonView", Settings.ThirdPerson);
	ReadDofView("Shaders.DepthOfField.VanityView", Settings.VanityView);

	// UNOFFICIAL 2026-10-26 (NVR DXVK, fork-only): see DepthOfField.h BokehStruct.
	Bokeh.Bokeh = TheSettingManager->GetSettingI("Shaders.DepthOfField.Bokeh", "Bokeh") != 0;
	Bokeh.BokehShape = TheSettingManager->GetSettingI("Shaders.DepthOfField.Bokeh", "BokehShape");
	if (Bokeh.BokehShape != 0) Bokeh.BokehShape = max(5, min(Bokeh.BokehShape, 8));
	Bokeh.BokehThreshold = max(0.0f, min(TheSettingManager->GetSettingF("Shaders.DepthOfField.Bokeh", "BokehThreshold"), 0.99f));
	Bokeh.BokehIntensity = max(0.0f, min(TheSettingManager->GetSettingF("Shaders.DepthOfField.Bokeh", "BokehIntensity"), 10.0f));
	Bokeh.BokehSize = max(1.0f, min(TheSettingManager->GetSettingF("Shaders.DepthOfField.Bokeh", "BokehSize"), 64.0f));
	Bokeh.BokehRotation = TheSettingManager->GetSettingF("Shaders.DepthOfField.Bokeh", "BokehRotation");
	Bokeh.BokehMaxSprites = max(256, min(TheSettingManager->GetSettingI("Shaders.DepthOfField.Bokeh", "BokehMaxSprites"), 16384));
}

// UNOFFICIAL 2026-10-26 (NVR DXVK, fork-only): with Bokeh on, the autofocus technique goes through ExtRender even when NvrDxvkPasses
// is off (the sprites only exist on that path). Everything else, and any failure, is the usual EffectRecord::Render.
void DepthOfFieldEffect::Render(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	if (techniqueIndex == 0 && BokehWanted() && !ExtFailed && !DxvkCompute::PassesOn() && DxvkCompute::Available() &&
		Enabled && Effect && ShouldRender() && ExtRender(Device, RenderTarget, RenderedSurface, techniqueIndex, ClearRenderTarget, SourceBuffer)) {
		Device->SetRenderTarget(0, RenderTarget);
		return;
	}
	EffectRecord::Render(Device, RenderTarget, RenderedSurface, techniqueIndex, ClearRenderTarget, SourceBuffer);
}

namespace {
	struct BokehPush {	// DepthOfFieldBokehScatter.comp's push block, 48 bytes
		UInt32 halfW, halfH, fullW, fullH, phase, maxSprites, shape, samplerIndex;
		float threshold, intensity, maxRadius, rotation;
	};
	const UINT BokehSpriteHeader = 16;	// uint count + 3 pad
}

void DepthOfFieldEffect::BokehRelease() {
	DxvkCompute::Drop(bokehScatter);
	DxvkCompute::Drop(bokehCombine);
	DxvkCompute::Drop(bokehResolve);
	DxvkCompute::Drop(bokehSprites);
	DxvkCompute::Drop(bokehAccum);
	DxvkCompute::Drop(bokehSurface);
	DxvkCompute::Drop(bokehTexture);
}

// The buffers and the resolve texture for the current size (half resolution) and sprite budget; false: not all made.
static bool MakeBokehTargets(UINT w, UINT h, UINT budget, ID3D9NvrExtBuffer*& sprites, ID3D9NvrExtBuffer*& accum, IDirect3DTexture9*& texture, IDirect3DSurface9*& surface) {
	sprites = DxvkCompute::CreateBuffer(16 + budget * 32, NVR_EXT_BUFFER_STORAGE | NVR_EXT_BUFFER_TRANSFER_DST, "DoF bokeh sprites");	// count + 3 pad, 32-byte sprites
	accum = DxvkCompute::CreateBuffer(w * h * 12, NVR_EXT_BUFFER_STORAGE | NVR_EXT_BUFFER_TRANSFER_DST, "DoF bokeh accumulation");
	if (FAILED(TheRenderManager->device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &texture, NULL))) texture = nullptr;
	if (texture && FAILED(texture->GetSurfaceLevel(0, &surface))) surface = nullptr;
	return sprites && accum && texture && surface;
}

bool DepthOfFieldEffect::BokehReady() {
	const UINT w = (TheRenderManager->width + 1) / 2, h = (TheRenderManager->height + 1) / 2, budget = (UINT)Bokeh.BokehMaxSprites;
	if (bokehScatter && bokehCombine && bokehResolve && bokehSprites && bokehAccum && bokehSurface) {
		if (w == bokehW && h == bokehH && budget == bokehMax) return true;
		DxvkCompute::Drop(bokehSprites);	// resolution or sprite budget changed: new buffers and texture, same pipelines
		DxvkCompute::Drop(bokehAccum);
		DxvkCompute::Drop(bokehSurface);
		DxvkCompute::Drop(bokehTexture);
		bokehW = w; bokehH = h; bokehMax = budget;
		if (MakeBokehTargets(w, h, budget, bokehSprites, bokehAccum, bokehTexture, bokehSurface)) return true;
		BokehRelease();
		bokehFailed = true;
		Logger::Log("UNOFFICIAL DepthOfField: bokeh buffers could not be made (%ux%u); bokeh off for this session", w, h);
		return false;
	}
	if (bokehTried) return false;
	bokehTried = true;
	const NVR_EXT_BINDING_DESC scatterBindings[] = {
		{ 1, 0, NVR_EXT_BIND_SAMPLED_IMAGE,  0, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_source
		{ 1, 1, NVR_EXT_BIND_SAMPLED_IMAGE,  1, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_blur
		{ 1, 2, NVR_EXT_BIND_SAMPLED_IMAGE,  2, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_depth
		{ 1, 3, NVR_EXT_BIND_STORAGE_BUFFER, 0, NVR_EXT_VIEW_2D, TRUE,  0 },	// Sprites
		{ 1, 4, NVR_EXT_BIND_STORAGE_BUFFER, 1, NVR_EXT_VIEW_2D, TRUE,  0 },	// Accum
		{ 0, 0, NVR_EXT_BIND_SAMPLER,        0, NVR_EXT_VIEW_2D, FALSE, 28 },	// samplerIndex at 28
	};
	const NVR_EXT_BINDING_DESC combineBindings[] = {
		{ 1, 0, NVR_EXT_BIND_SAMPLED_IMAGE,  0, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_rendered
		{ 1, 1, NVR_EXT_BIND_SAMPLED_IMAGE,  1, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_source
		{ 1, 2, NVR_EXT_BIND_SAMPLED_IMAGE,  2, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_bokeh (the resolved sprites)
		{ 0, 0, NVR_EXT_BIND_SAMPLER,        0, NVR_EXT_VIEW_2D, FALSE, 208 },	// DofPush.samplerIndex
	};
	const NVR_EXT_BINDING_DESC resolveBindings[] = {
		{ 1, 0, NVR_EXT_BIND_STORAGE_BUFFER, 0, NVR_EXT_VIEW_2D, FALSE, 0 },	// Accum (read)
	};
	bokehScatter = DxvkCompute::CreatePipeline("DepthOfFieldBokehScatter", scatterBindings, _countof(scatterBindings), sizeof(BokehPush), true);
	bokehCombine = DxvkCompute::CreateGraphicsPipeline("DepthOfFieldCombineBokeh", combineBindings, _countof(combineBindings), 224, true);
	bokehResolve = DxvkCompute::CreateGraphicsPipeline("DepthOfFieldBokehResolve", resolveBindings, _countof(resolveBindings), 16, false);
	bokehW = w; bokehH = h; bokehMax = budget;
	const bool targets = MakeBokehTargets(w, h, budget, bokehSprites, bokehAccum, bokehTexture, bokehSurface);
	if (!bokehScatter || !bokehCombine || !bokehResolve || !targets) {
		BokehRelease();
		bokehFailed = true;
		Logger::Log("UNOFFICIAL DepthOfField: bokeh could not be set up (needs the Vegas Shaders DXVK fork and Compute\\DepthOfFieldBokehScatter.spv, DepthOfFieldBokehResolve.spv, DepthOfFieldCombineBokeh.spv); bokeh off for this session");
		return false;
	}
	Logger::Log("UNOFFICIAL DepthOfField: bokeh ready (%ux%u half resolution, up to %u sprites)", w, h, budget);
	return true;
}

bool DepthOfFieldEffect::BokehScatter(IDirect3DDevice9* Device, IDirect3DBaseTexture9* depth) {
	static GpuTimer bokehTimer("  DoF bokeh scatter (compute)");
	GpuProfileScope gpu(bokehTimer, Device);
	ID3D9NvrExt* ext = DxvkCompute::Extension();
	ID3D9NvrExtSampler* sampler = DxvkCompute::LinearClamp();
	IDirect3DBaseTexture9* source = TheTextureManager->SourceTexture;	// the image before the effect
	IDirect3DBaseTexture9* blur = TheTextureManager->RenderedTexture;	// the DoF pass's blur amounts, just drawn
	if (!ext || !sampler || !source || !blur || !depth) return false;

	BokehPush push = {};
	push.halfW = bokehW; push.halfH = bokehH;
	push.fullW = TheRenderManager->width; push.fullH = TheRenderManager->height;
	push.maxSprites = bokehMax;
	push.shape = (UInt32)Bokeh.BokehShape;
	push.threshold = Bokeh.BokehThreshold;
	push.intensity = Bokeh.BokehIntensity;
	push.maxRadius = Bokeh.BokehSize * (float)TheRenderManager->height / 1080.0f * 0.5f;
	push.rotation = Bokeh.BokehRotation * (float)D3DX_PI / 180.0f;

	NVR_EXT_RESOURCE_BINDING bindings[] = {
		{ 0, NVR_EXT_BIND_SAMPLED_IMAGE,  source,       0, 0, 0, FALSE },
		{ 1, NVR_EXT_BIND_SAMPLED_IMAGE,  blur,         0, 0, 0, FALSE },
		{ 2, NVR_EXT_BIND_SAMPLED_IMAGE,  depth,        0, 0, 0, FALSE },
		{ 0, NVR_EXT_BIND_STORAGE_BUFFER, bokehSprites, 0, 0, 0, FALSE },
		{ 1, NVR_EXT_BIND_STORAGE_BUFFER, bokehAccum,   0, 0, 0, FALSE },
		{ 0, NVR_EXT_BIND_SAMPLER,        sampler,      0, 0, 0, FALSE },
	};
	NVR_EXT_DISPATCH_DESC dispatch = {};
	dispatch.pPipeline = bokehScatter;
	dispatch.BindingCount = _countof(bindings);
	dispatch.pBindings = bindings;
	dispatch.pPushData = &push;
	dispatch.PushDataSize = sizeof(push);

	DxvkCompute::PreLoad(bindings, _countof(bindings));	// UNOFFICIAL 2026-10-27 (merge with the alflicker fix): managed textures on the GPU
	HRESULT result = ext->ClearBuffer(bokehSprites, 0, BokehSpriteHeader, 0);
	if (SUCCEEDED(result)) result = ext->ClearBuffer(bokehAccum, 0, 0, 0);
	push.phase = 0;		// detect: one thread per half-resolution pixel
	dispatch.GroupsX = (bokehW + 15) / 16; dispatch.GroupsY = (bokehH + 15) / 16; dispatch.GroupsZ = 1;
	if (SUCCEEDED(result)) result = ext->Dispatch(&dispatch);
	push.phase = 1;		// splat: one group per sprite slot
	dispatch.GroupsX = bokehMax; dispatch.GroupsY = 1;
	if (SUCCEEDED(result)) result = ext->Dispatch(&dispatch);
	if (SUCCEEDED(result)) {
		// the sprite buffer into the half-resolution texture the bokeh Combine samples
		struct { UInt32 width, height, pad0, pad1; } resolvePush = { bokehW, bokehH, 0, 0 };
		NVR_EXT_RESOURCE_BINDING resolveResources[] = { { 0, NVR_EXT_BIND_STORAGE_BUFFER, bokehAccum, 0, 0, 0, FALSE } };
		NVR_EXT_DRAW_DESC draw = {};
		draw.pPipeline = bokehResolve;
		draw.TargetCount = 1;
		draw.Targets[0].pResource = bokehSurface;
		draw.BindingCount = _countof(resolveResources);
		draw.pBindings = resolveResources;
		draw.pPushData = &resolvePush;
		draw.PushDataSize = sizeof(resolvePush);
		result = ext->Draw(&draw);
	}
	if (FAILED(result)) {
		Logger::Log("UNOFFICIAL DepthOfField: bokeh dispatch failed (%08lX); bokeh off for this session", result);
		BokehRelease();
		bokehFailed = true;
		return false;
	}
	if (++bokehFrames == 1) Logger::Log("UNOFFICIAL DepthOfField: bokeh running (shape %d, threshold %.2f, size %.1f px at 1080p)", Bokeh.BokehShape, Bokeh.BokehThreshold, Bokeh.BokehSize);
	return true;
}

void DepthOfFieldEffect::RegisterConstants() {
	ShaderManager* sm = TheShaderManager;
	sm->RegisterConstant("TESR_DepthOfFieldBlur", &Constants.Blur);
	sm->RegisterConstant("TESR_DepthOfFieldData", &Constants.Data);
	sm->RegisterConstant("TESR_DepthOfFieldJitter", &Constants.Jitter);	// UNOFFICIAL 2026-11-01 JitterFix
}

bool DepthOfFieldEffect::ShouldRender() {
	if (Constants.Enabled) return true;
	if (!Constants.Blur.x) return false;
	// UNOFFICIAL: distant blur only. Its pass (DistantOnly) leaves every pixel nearer than DistantBlurStartRange unchanged,
	// and nothing is drawn beyond the camera's far plane, so when the far plane is nearer than the blur start (interiors,
	// 30000 by default) the pass would only copy the image: skip it (~0.16 ms at 1440p).
	const float farZ = TheRenderManager->CameraData.y;
	const bool skip = farZ > 0.0f && farZ <= Constants.Blur.y;
	static int logged = -1;
	if (GpuTimer::Enabled && logged != (int)skip) {
		logged = skip;
		Logger::Log("UNOFFICIAL distant blur %s: camera far plane %.0f, blur starts at %.0f", skip ? "skipped (nothing that far)" : "runs", farZ, Constants.Blur.y);
	}
	return !skip;
};

// The push block of every DepthOfField pass (the GLSL declares the same one: DepthOfField.frag for technique 1, DepthOfField.glsl for
// technique 0). The matrices are the D3DXMATRIX memory the D3D9 constants come from; the sampler's heap index is at 208.
struct DofPush {
	D3DXMATRIX viewTransform, projectionTransform;
	D3DXVECTOR4 reciprocalResolution, blur, data, cameraPosition, cameraData;
	UInt32 samplerIndex;
	float useBokeh;	// BokehBlur's useBokeh (1 or 0); the other passes ignore it
	UInt32 pad[2];	// UNOFFICIAL 2026-10-26: DepthOfFieldCombineBokeh reads the bokeh buffer's half-resolution width, height here;
					// 2026-11-01 the passes that read depth (DepthOfField.frag, DepthOfFieldFocus.frag) read the JitterFix offset here (floats)
};
static_assert(sizeof(DofPush) == 224, "DofPush: DepthOfField.glsl's push block (BokehReady makes the bokeh Combine with 224)");

// UNOFFICIAL 2026-10-25 (NVR DXVK): the two techniques as Vulkan passes. DistantBlurOnly (technique 1, the common gameplay case) is one
// pass: DepthOfField.frag is DistantOnly line for line. AutofocusBokeh (technique 0) is six passes through the frame chain, as Render
// runs them: DoF (blur amounts), halfRes (the quadrant mosaic), BokehBlur twice with bokeh and once without, Combine (last, may write the
// game target). Each GLSL file is its .fx.hlsl pass line for line. Only the first pass copies the image before the effect into
// TESR_SourceBuffer (ExtFullScreen copies it per call); the others get no SourceBuffer, as the D3D9 passes read it unchanged.
bool DepthOfFieldEffect::ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	if (ClearRenderTarget) return false;
	DofPush push = {};
	push.viewTransform = TheRenderManager->viewMatrix;
	push.projectionTransform = TheRenderManager->projMatrix;
	push.reciprocalResolution = TheShaderManager->ShaderConst.ReciprocalResolution;
	push.blur = Constants.Blur;
	push.data = Constants.Data;
	push.cameraPosition = TheRenderManager->CameraPosition;
	push.cameraData = TheRenderManager->CameraData;
	memcpy(push.pad, &Constants.Jitter.x, sizeof(push.pad));	// UNOFFICIAL 2026-11-01 JitterFix (CombineBokeh overwrites it below)

	IDirect3DBaseTexture9* depth = TheShaderManager->Effects.CombineDepth ? TheShaderManager->Effects.CombineDepth->Textures.CombinedDepthTexture : nullptr;
	if (techniqueIndex == 1) {
		IDirect3DBaseTexture9* textures[] = { TheTextureManager->RenderedTexture, depth };
		return ExtTexturePass(Device, RenderTarget, RenderedSurface, SourceBuffer, extPass, "DepthOfField", textures, _countof(textures), &push, sizeof(push), 208);
	}
	if (techniqueIndex != 0) return false;

	// Technique 0: the chain must be the one this effect renders into, and TESR_SourceBuffer the image before it (as Render sets it up).
	if (!SourceBuffer || SourceBuffer == RenderTarget || SourceBuffer != TheTextureManager->SourceSurface || !TheTextureManager->SourceTexture) return false;
	FrameChain& chain = TheShaderManager->Chain;
	if (!chain.Owns(RenderTarget, RenderedSurface)) return false;
	IDirect3DBaseTexture9* avgLuma = TheShaderManager->Effects.AvgLuma ? TheShaderManager->Effects.AvgLuma->Textures.AvgLumaTexture : nullptr;
	if (!depth || !avgLuma) return false;

	// The four pipelines are made before anything is drawn, so a refused one leaves the frame as the D3D9 technique finds it.
	// Textures bind in order (set 1, binding i); the LINEAR/CLAMP sampler (slot 0) takes its heap index from the push block at 208.
	auto make = [](ExtPass& pass, const char* spvName, UINT textureCount) {
		if (pass.Tried) return;
		pass.Tried = true;
		NVR_EXT_BINDING_DESC bindings[3] = {};
		for (UINT i = 0; i < textureCount; i++) {
			bindings[i].Set = 1; bindings[i].Binding = i; bindings[i].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[i].Slot = i; bindings[i].ViewType = NVR_EXT_VIEW_2D;
		}
		bindings[textureCount].Type = NVR_EXT_BIND_SAMPLER; bindings[textureCount].Slot = 0; bindings[textureCount].PushOffset = 208;
		pass.Pipeline = DxvkCompute::CreateGraphicsPipeline(spvName, bindings, textureCount + 1, sizeof(DofPush), true);
	};
	make(extFocus, "DepthOfFieldFocus", 2);
	make(extMosaic, "DepthOfFieldMosaic", 2);
	make(extBokeh, "DepthOfFieldBokeh", 1);
	make(extCombine, "DepthOfFieldCombine", 2);
	ID3D9NvrExtSampler* sampler = DxvkCompute::LinearClamp();
	if (!extFocus.Pipeline || !extMosaic.Pipeline || !extBokeh.Pipeline || !extCombine.Pipeline || !sampler) return false;

	// One pass through the chain (as ExtFullScreen does it): the textures in binding order, then the sampler at slot 0.
	auto draw = [&](ExtPass& pass, IDirect3DBaseTexture9* const* textures, UINT textureCount, IDirect3DSurface9* source, bool lastPass, float useBokeh) {
		push.useBokeh = useBokeh;
		NVR_EXT_RESOURCE_BINDING resources[3] = {};
		for (UINT i = 0; i < textureCount; i++) { resources[i].Slot = i; resources[i].Type = NVR_EXT_BIND_SAMPLED_IMAGE; resources[i].pResource = textures[i]; }
		resources[textureCount].Slot = 0; resources[textureCount].Type = NVR_EXT_BIND_SAMPLER; resources[textureCount].pResource = sampler;
		return ExtFullScreen(Device, RenderTarget, TheTextureManager->RenderedSurface, source, pass.Pipeline, resources, textureCount + 1, &push, sizeof(push), lastPass);
	};

	// The DoF pass reads the depth and the focus and writes the blur amounts into the chain. Nothing is drawn yet if it fails,
	// so the D3D9 technique runs as before.
	IDirect3DBaseTexture9* focusTextures[] = { depth, avgLuma };
	if (!draw(extFocus, focusTextures, 2, SourceBuffer, false, 0.0f)) return false;

	// UNOFFICIAL 2026-10-26 (fork-only): the bokeh sprites, from the sharp image (TESR_SourceBuffer, just copied) and the blur
	// amounts the DoF pass just drew (the chain's current image); added in the bokeh Combine below. Not done: the plain Combine.
	const bool bokeh = BokehWanted() && BokehReady() && BokehScatter(Device, depth);

	// From here on the chain holds an image: a pass that fails leaves it as the last pass drew it (ExtFullScreen logs the failure and
	// the effect's Vulkan path is off from the next frame). The D3D9 passes cannot pick up from there, so the frame keeps what is there.
	// halfRes: TESR_RenderedBuffer is the blur amounts now, TESR_SourceBuffer the image before the effect.
	IDirect3DBaseTexture9* mosaicTextures[] = { TheTextureManager->RenderedTexture, TheTextureManager->SourceTexture };
	if (!draw(extMosaic, mosaicTextures, 2, nullptr, false, 0.0f)) return true;
	// BokehBlur twice with bokeh, then once as a plain blur; each reads the chain's current image.
	const float bokehPasses[] = { 1.0f, 1.0f, 0.0f };
	for (float useBokeh : bokehPasses) {
		IDirect3DBaseTexture9* bokehTextures[] = { TheTextureManager->RenderedTexture };
		if (!draw(extBokeh, bokehTextures, 1, nullptr, false, useBokeh)) return true;
	}
	// Combine writes the frame, straight into the game target when the chain predicts this effect last.
	IDirect3DBaseTexture9* combineTextures[] = { TheTextureManager->RenderedTexture, TheTextureManager->SourceTexture };
	if (bokeh) {
		// DepthOfFieldCombineBokeh: Combine's textures, then the resolved sprites, the sampler.
		push.useBokeh = 0.0f;
		push.pad[0] = bokehW; push.pad[1] = bokehH;
		NVR_EXT_RESOURCE_BINDING resources[] = {
			{ 0, NVR_EXT_BIND_SAMPLED_IMAGE,  combineTextures[0], 0, 0, 0, FALSE },
			{ 1, NVR_EXT_BIND_SAMPLED_IMAGE,  combineTextures[1], 0, 0, 0, FALSE },
			{ 2, NVR_EXT_BIND_SAMPLED_IMAGE,  bokehTexture,       0, 0, 0, FALSE },
			{ 0, NVR_EXT_BIND_SAMPLER,        sampler,            0, 0, 0, FALSE },
		};
		ExtFullScreen(Device, RenderTarget, TheTextureManager->RenderedSurface, nullptr, bokehCombine, resources, _countof(resources), &push, sizeof(push), true);
		return true;
	}
	draw(extCombine, combineTextures, 2, nullptr, true, 0.0f);
	return true;
}
