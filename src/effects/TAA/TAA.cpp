// CREDIT: TAA for New Vegas Reloaded by Conor. See TAA.fx.hlsl for upstream credits.
// UNOFFICIAL: brought into this build from Balebandro's Physically-Based-Reloaded fork (Alejandro Martinez), which also
// added the Sharpness and MipBias settings (fork commit a2a2333a). Off by default ([Shaders.TAA.Status] Enabled = false).

#include <algorithm>

#include "TAA.h"
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-25 (NVR DXVK)
#include "../../core/Upscaler.h"		// UNOFFICIAL 2026-10-27 (DLSS helper options)
#include "../../core/ObjectMotion.h"	// UNOFFICIAL 2026-10-29 ObjectMotion (the Vulkan passes' motion input)
#include "../../core/Diagnostics.h"	// UNOFFICIAL 2026-11-01 the history watch (HistoryProbe)
#include "../../core/SessionLog.h"

void TAAEffect::RegisterConstants() {
	TheShaderManager->RegisterConstant("TESR_TAAData", &Constants.Data);
	TheShaderManager->RegisterConstant("TESR_TAAPrevProjection", &Constants.PrevProjection);
	TheShaderManager->RegisterConstant("TESR_TAACameraDelta", &Constants.CameraDelta);
	TheShaderManager->RegisterConstant("TESR_TAAWeapon", &Constants.Weapon);
	TheShaderManager->RegisterConstant("TESR_TAASharpen", &Constants.Sharpen);
	TheShaderManager->RegisterConstant("TESR_TAAPrevViewTransform", (D3DXVECTOR4*)&Constants.PrevViewTransform);
}

// UNOFFICIAL: the two full-screen FP16 buffers (about 60 MB at 1440p) are made on first use (EnsureTextures, from Render),
// not at startup, so TAA costs no video memory while it is off.
void TAAEffect::RegisterTextures() {}

bool TAAEffect::EnsureTextures() {
	if (Textures.ResolveSurface && Textures.HistorySurface) return true;
	if (texturesFailed) return false;
	int width = TheRenderManager->width;
	int height = TheRenderManager->height;

	if (!Textures.ResolveSurface)
		TheTextureManager->InitTexture("TESR_TAAResolveBuffer", &Textures.ResolveTexture, &Textures.ResolveSurface, width, height, D3DFMT_A16B16G16R16F);
	if (!Textures.HistorySurface)
		TheTextureManager->InitTexture("TESR_TAAHistoryBuffer", &Textures.HistoryTexture, &Textures.HistorySurface, width, height, D3DFMT_A16B16G16R16F);
	texturesFailed = !Textures.ResolveSurface || !Textures.HistorySurface;
	if (texturesFailed) Logger::Log("[ERROR] TAA : could not create its buffers; TAA stays off");
	return !texturesFailed;
}

void TAAEffect::UpdateSettings() {
	// A key absent from the toml reads as 0. Both settings would be useless at 0 -- no history at
	// all, or a box collapsed onto the neighbourhood mean -- so 0 falls back to the default rather
	// than silently turning the effect into a blur or a no-op for anyone without the defaults file.
	float weight = TheSettingManager->GetSettingF("Shaders.TAA.Main", "HistoryWeight");
	Settings.HistoryWeight = weight > 0.0f ? std::clamp(weight, 0.5f, 0.97f) : 0.9f;

	float gamma = TheSettingManager->GetSettingF("Shaders.TAA.Main", "ClipGamma");
	Settings.ClipGamma = gamma > 0.0f ? std::clamp(gamma, 0.5f, 2.5f) : 1.0f;

	// 0 is off, which is also what a missing key reads as.
	Settings.DebugView = std::clamp(TheSettingManager->GetSettingI("Shaders.TAA.Main", "DebugView"), 0, 5);

	Settings.Jitter = TheSettingManager->GetSettingI("Shaders.TAA.Main", "Jitter") != 0;

	// 0 is a real value here -- the weapon skipped entirely -- so a missing key reading as 0 is taken
	// at its word rather than replaced by the default.
	Settings.WeaponTAA = std::clamp(TheSettingManager->GetSettingF("Shaders.TAA.Main", "WeaponTAA"), 0.0f, 1.0f);

	// 0 is off for both, which is also what a missing key reads as.
	Settings.Sharpness = std::clamp(TheSettingManager->GetSettingF("Shaders.TAA.Main", "Sharpness"), 0.0f, 1.0f);
	Settings.MipBias = std::clamp(TheSettingManager->GetSettingF("Shaders.TAA.Main", "MipBias"), -1.5f, 0.0f);

	// UNOFFICIAL 2026-10-26 (NVR DXVK): FSR 2 as the resolve. All three read as 0 (off) when missing.
	// UNOFFICIAL 2026-10-27 (user: "give dlss its own tab like fsr does ... toggling one disables the other"): [Shaders.TAA.DLSS]
	// Enabled runs the same upscaler stage with DLSS; FSR and DLSS are either/or (the menu switches the other off; a file with both
	// on gets DLSS).
	const bool dlss = TheSettingManager->GetSettingI("Shaders.TAA.DLSS", "Enabled") != 0;
	FSRSettings.Enabled = dlss || TheSettingManager->GetSettingI("Shaders.TAA.FSR", "Enabled") != 0;
	FSRSettings.Sharpness = std::clamp(TheSettingManager->GetSettingF("Shaders.TAA.FSR", "Sharpness"), 0.0f, 1.0f);
	FSRSettings.WeaponReactive = std::clamp(TheSettingManager->GetSettingF("Shaders.TAA.FSR", "WeaponReactive"), 0.0f, 1.0f);
	// UNOFFICIAL 2026-10-27 RenderScale (TAAUpscale.cpp). A missing key reads as 0, which means off (1) here, not "nothing".
	const float scale = TheSettingManager->GetSettingF(dlss ? "Shaders.TAA.DLSS" : "Shaders.TAA.FSR", "RenderScale");
	// UNOFFICIAL 2026-10-27 (user: "0.1 should look pretty compressed, yet it does not"): down to 0.33, FSR 2's and DLSS's lowest
	// ("Ultra Performance", 3x per axis); below that neither is made to work. Was 0.5.
	FSRSettings.RenderScale = scale > 0.0f ? std::clamp(scale, 0.33f, 1.0f) : 1.0f;
	// UNOFFICIAL 2026-10-28 ([Shaders.TAA.FSR] Version, 0 when missing): 0 AMD FSR 2.2.1 (as before), 1 AMD FSR 3.1 (core/UpscalerFsr31.cpp).
	// Not the key "Upscaler": until 2026-10-27 "Upscaler = 1" meant DLSS, and old settings files still have it.
	// DLSS keeps its own switch above and wins over both.
	const bool fsr31 = TheSettingManager->GetSettingI("Shaders.TAA.FSR", "Version") == 1;
	FSRSettings.Upscaler = dlss ? 1 : (fsr31 ? 2 : 0);
	// UNOFFICIAL 2026-10-29: every change of the upscaler or of its RenderScale says so (the RenderScale lines below only come from the
	// upscaling path: a switch to a RenderScale of 1 -- native resolution, TAA / FSR as anti-aliasing only -- was silent).
	{
		static int loggedUpscaler = -1; static float loggedScale = -1.0f; static bool loggedEnabled = false;
		if (FSRSettings.Upscaler != loggedUpscaler || FSRSettings.RenderScale != loggedScale || FSRSettings.Enabled != loggedEnabled) {
			if (loggedUpscaler >= 0 || FSRSettings.Enabled)
				Logger::Log("UNOFFICIAL TAA: %s, RenderScale %.2f (%s)%s", !FSRSettings.Enabled ? "TAA's own resolve" :
					FSRSettings.Upscaler == 1 ? "DLSS (helper; FSR 2 while it cannot run)" : FSRSettings.Upscaler == 2 ? "FSR 3.1" : "FSR 2.2.1",
					FSRSettings.RenderScale, FSRSettings.RenderScale < 1.0f ? "upscaled" : "native resolution, no upscale",
					FSRSettings.Enabled && FSRSettings.RenderScale >= 1.0f && loggedScale > 0.0f && loggedScale < 1.0f ?
					" -- was upscaled before: each of [Shaders.TAA.FSR] and [Shaders.TAA.DLSS] has its own RenderScale" : "");
			loggedUpscaler = FSRSettings.Upscaler; loggedScale = FSRSettings.RenderScale; loggedEnabled = FSRSettings.Enabled;
		}
	}
	FSRSettings.DebugView = std::clamp(TheSettingManager->GetSettingI("Shaders.TAA.FSR", "DebugView"), 0, 3);
	// UNOFFICIAL 2026-10-27 RenderScale v2 (TAAUpscale.cpp): also for DLSS (the same path).
	// UNOFFICIAL 2026-11-01 (user: "let's just run the effects before dlss instead of doing a band aid fix", dlss / fsr): below RenderScale 1
	// NVR's pre-tonemap effects always run in the small frame before FSR / DLSS, which then average their noise with the world (with them
	// after the upscale, AmbientLight / shadow / fog grain shimmered on grass, and it cost ~9 ms more at 1440p). The old LowResEffects key
	// is no longer read (a toml with it off got the shimmering order); [Shaders.TAA.FSR] EffectsAfterUpscale (developer A/B, off) is the old
	// order. Frames where it cannot run (RenderPreTonemapping off, missing resources) still fall back to the upscale first (SmoothEffects).
	{
		SettingManager::Configuration::ConfigNode node;
		const bool after = TheSettingManager->Config.FillNode(&node, "Shaders.TAA.FSR", "EffectsAfterUpscale") && node.BoolValue;
		FSRSettings.LowResEffects = !after;
		static int logged = -1;
		if (logged != (int)after) {
			logged = (int)after;
			SettingManager::Configuration::ConfigNode old;
			const bool oldOff = TheSettingManager->Config.FillNode(&old, "Shaders.TAA.FSR", "LowResEffects") && !old.BoolValue;
			Logger::Log("UNOFFICIAL TAA: below RenderScale 1 Vegas Shaders' effects run %s%s", after ? "AFTER the upscale ([Shaders.TAA.FSR] EffectsAfterUpscale, a developer test)" :
				"before FSR / DLSS (in the small frame; FSR / DLSS average their noise)",
				oldOff && !after ? " -- the old LowResEffects = false in your settings is no longer read" : "");
		}
	}
	{	// UNOFFICIAL 2026-11-01 SmoothEffects (missing key, an older toml: on)
		SettingManager::Configuration::ConfigNode node;
		FSRSettings.SmoothEffects = !TheSettingManager->Config.FillNode(&node, "Shaders.TAA.FSR", "SmoothEffects") || node.BoolValue;
	}
	// UNOFFICIAL 2026-10-27 DLSS through the 64-bit helper (core/UpscalerDlss.cpp keeps them; DLSS Enabled above is real DLSS,
	// with FSR 2 whenever it cannot run). Both read as 0 (off) when missing.
	// UNOFFICIAL 2026-10-30 (DLAA stall report): HelperWait / HelperGpuPriority, 1 when the key is missing (FillNode: 0 is a real value).
	SettingManager::Configuration::ConfigNode helperNode;
	const bool helperCpuWait = TheSettingManager->Config.FillNode(&helperNode, "Shaders.TAA.DLSS", "HelperWait") ? helperNode.FloatValue != 0.0f : true;
	const bool helperHighPriority = TheSettingManager->Config.FillNode(&helperNode, "Shaders.TAA.DLSS", "HelperGpuPriority") ? helperNode.FloatValue != 0.0f : true;
	// UNOFFICIAL 2026-10-30 (tester report: the render-scaled DLSS picture stayed small in the top-left corner): HelperExactInputs, 1 when the
	// key is missing, and HelperFailTest (test only, 0 when missing).
	const bool helperExactInputs = TheSettingManager->Config.FillNode(&helperNode, "Shaders.TAA.DLSS", "HelperExactInputs") ? helperNode.FloatValue != 0.0f : true;
	Upscaler::DlssOptions(TheSettingManager->GetSettingI("Shaders.TAA.DLSS", "HelperTest") != 0,
		std::clamp(TheSettingManager->GetSettingI("Shaders.TAA.DLSS", "JitterFlip"), 0, 3), helperCpuWait, helperHighPriority, helperExactInputs,
		std::clamp(TheSettingManager->GetSettingI("Shaders.TAA.DLSS", "HelperFailTest"), 0, 3));
	// UNOFFICIAL 2026-11-01 (player report: with DLSS the first-person arms and gun ghost and smear while they animate): WeaponMask, DLSS's bias
	// current colour mask on the weapon, 1 when the key is missing (FillNode: 0 is a real value, no mask).
	Upscaler::DlssWeaponMask(TheSettingManager->Config.FillNode(&helperNode, "Shaders.TAA.DLSS", "WeaponMask") ? std::clamp(helperNode.FloatValue, 0.0f, 1.0f) : 1.0f);
	// UNOFFICIAL 2026-10-30 DLSS Ray Reconstruction (TAAFSR.cpp RayReconstruct, docs/next/FORK2-DLSS-RR.md) and DLSS SR's preset. A missing
	// key reads as 0 / false: RR off, no preset hint (the DLL's own choice, as before), the guides' defaults. RRRoughness is the one where 0
	// is a real value (a mirror), so only a key that is there at all replaces its default.
	{
		Upscaler::RrOptions rr;
		// (2026-11-07 perf-rr) at every RenderScale: below 1 RR denoises AND upscales the small frame in one step (TAAFSR.cpp RayReconstruct,
		// a RenderScale v2 rect frame), in place of DLSS SR's upscale. Was: RenderScale 1 only (review 2026-10-30). The helper's sizes are the
		// same either way (inputs and output at the screen's size, the render rect inside), so crossing RenderScale 1 restarts nothing.
		const bool rrKey = dlss && TheSettingManager->GetSettingI("Shaders.TAA.DLSS", "RayReconstruction") != 0;
		rr.Wanted = rrKey;
		rr.KeepDenoisers = TheSettingManager->GetSettingI("Shaders.TAA.DLSS", "RRKeepDenoisers") != 0;
		rr.Preset = std::clamp(TheSettingManager->GetSettingI("Shaders.TAA.DLSS", "Preset"), 0, 3);
		rr.RrPreset = std::clamp(TheSettingManager->GetSettingI("Shaders.TAA.DLSS", "RRPreset"), 0, 2);
		RRSettings.Normals = std::clamp(TheSettingManager->GetSettingI("Shaders.TAA.DLSS", "RRNormals"), 0, 1);
		rr.NormalsView = RRSettings.Normals == 1;
		rr.DepthLinear = TheSettingManager->GetSettingI("Shaders.TAA.DLSS", "RRDepthLinear") != 0;
		Upscaler::DlssRrOptions(rr);
		RRSettings.Albedo = std::clamp(TheSettingManager->GetSettingI("Shaders.TAA.DLSS", "RRAlbedo"), 0, 2);	// 2: UNOFFICIAL 2026-10-31 the albedo buffer
		SettingManager::Configuration::ConfigNode node;
		RRSettings.Roughness = TheSettingManager->Config.FillNode(&node, "Shaders.TAA.DLSS", "RRRoughness") ? std::clamp(node.FloatValue, 0.0f, 1.0f) : 0.6f;
	}
}

// Texture mip LOD bias on every sampler while the jittered world draws. Averaging jittered frames
// already filters textures, so the usual mip level comes out softer than without TAA; a negative bias
// picks a sharper mip and TAA settles the extra shimmer that would otherwise bring. Set straight on
// the device, around the game's render state cache; should the game set this state itself for some
// texture, that sampler just goes back to the game's value. EndJitter resets all of them to 0.
void TAAEffect::SetMipBias(float bias) {
	DWORD value = *(DWORD*)&bias;
	for (DWORD sampler = 0; sampler < 16; sampler++)
		TheRenderManager->device->SetSamplerState(sampler, D3DSAMP_MIPMAPLODBIAS, value);
}

void TAAEffect::UpdateConstants() {
	Constants.Data.x = Settings.HistoryWeight;
	Constants.Data.y = Settings.ClipGamma;
	Constants.Data.w = (float)Settings.DebugView;
	Constants.Weapon = D3DXVECTOR4(Settings.WeaponTAA, 0.0f, 0.0f, 0.0f);
	Constants.Sharpen = D3DXVECTOR4(Settings.Sharpness, 0.0f, 0.0f, 0.0f);
}

// Record the camera that rendered the frame now in the history buffer, for next frame's
// reprojection. Called after the resolve, never before it: at that point TheRenderManager holds
// the camera the frame was drawn with, because RenderEffects rebuilds it right before this effect.
void TAAEffect::RememberCamera() {
	Constants.PrevViewTransform = TheRenderManager->viewMatrix;
	Constants.PrevProjection = D3DXVECTOR4(TheRenderManager->projMatrix._11, TheRenderManager->projMatrix._22, 0.0f, 0.0f);
	prevCameraPosition = TheRenderManager->CameraPosition;
}

bool TAAEffect::DrawTechnique(const char* TechniqueName) {
	D3DXHANDLE technique = Effect->GetTechniqueByName(TechniqueName);
	if (!technique) {
		Logger::Log("[ERROR] TAA : technique %s not found", TechniqueName);
		return false;
	}

	Effect->SetTechnique(technique);
	UINT passes;
	Effect->Begin(&passes, 0);
	Effect->BeginPass(0);
	TheRenderManager->device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
	Effect->EndPass();
	Effect->End();
	return true;
}

// --- The Vulkan passes (NVR DXVK) -----------------------------------------------------------------------------------------
// UNOFFICIAL 2026-10-25 (NVR DXVK): the resolve, the debug view and the output as Compute\TAAResolve, TAADebug and TAAOutput
// (.frag; TAA.glsl holds what they share), line for line. Same targets, same order and same inputs as the D3D9 draws in Render.
// The constants go to a uniform buffer (304 bytes); the matrices are copied as the effect's SetMatrix uploads them.
namespace {
	// The Constants block of TAA.glsl (std140; the matrices as the effect's SetMatrix uploads them).
	struct TAAUniforms {
		D3DXMATRIX	projection, view, prevView;
		D3DXVECTOR4	reciprocalResolution, cameraData, data, prevProjection, cameraDelta, weapon, sharpen;
	};
	struct TAAPush { UInt32 pointIndex, linearIndex; };	// the extension writes the sampler heap indices here

	// Set 1: the textures in the .fx's sampler order (binding = slot); set 2: the constants; the samplers' heap indices at 0 and 4.
	const NVR_EXT_BINDING_DESC TAABindings[] = {
		{ 1, 0, NVR_EXT_BIND_SAMPLED_IMAGE, 0, NVR_EXT_VIEW_2D,   FALSE, 0 },	// TESR_SourceBuffer
		{ 1, 1, NVR_EXT_BIND_SAMPLED_IMAGE, 1, NVR_EXT_VIEW_2D,   FALSE, 0 },	// TESR_TAAHistoryBuffer
		{ 1, 2, NVR_EXT_BIND_SAMPLED_IMAGE, 2, NVR_EXT_VIEW_2D,   FALSE, 0 },	// TESR_DepthBuffer
		{ 1, 3, NVR_EXT_BIND_SAMPLED_IMAGE, 3, NVR_EXT_VIEW_2D,   FALSE, 0 },	// TESR_DepthBufferViewModel
		{ 1, 4, NVR_EXT_BIND_SAMPLED_IMAGE, 4, NVR_EXT_VIEW_2D,   FALSE, 0 },	// UNOFFICIAL 2026-10-29 TESR_ObjectMotionBuffer (placeholder: depth)
		{ 2, 0, NVR_EXT_BIND_UNIFORM_BUFFER, 0, NVR_EXT_VIEW_2D,  FALSE, 0 },	// Constants
		{ 0, 0, NVR_EXT_BIND_SAMPLER,        0, NVR_EXT_VIEW_2D,  FALSE, 0 },	// POINT, CLAMP
		{ 0, 0, NVR_EXT_BIND_SAMPLER,        1, NVR_EXT_VIEW_2D,  FALSE, 4 },	// LINEAR, CLAMP
	};
	const UINT TAATextureCount = 5;

	IDirect3DBaseTexture9* SlotTexture(const char* name) {
		std::string slotName = name;
		IDirect3DBaseTexture9** slot = TheTextureManager->GetTextureSlotByName(slotName);
		return slot ? *slot : nullptr;
	}
	// The constants from the same registered memory the effect's SetCT uploads.
	const D3DXVECTOR4* Registered(const char* name) {
		auto found = TheShaderManager->ConstantsTable.find(name);
		return found == TheShaderManager->ConstantsTable.end() ? nullptr : found->second;
	}
}

bool TAAEffect::ExtDraw(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, int write) {
	const D3DXVECTOR4* projection = Registered("TESR_ProjectionTransform");
	const D3DXVECTOR4* view = Registered("TESR_ViewTransform");
	const D3DXVECTOR4* reciprocal = Registered("TESR_ReciprocalResolution");
	const D3DXVECTOR4* cameraData = Registered("TESR_CameraData");
	if (!projection || !view || !reciprocal || !cameraData) return false;

	IDirect3DBaseTexture9* source = TheTextureManager->SourceTexture;
	IDirect3DBaseTexture9* depth = SlotTexture("TESR_DepthBuffer");
	IDirect3DBaseTexture9* viewModel = SlotTexture("TESR_DepthBufferViewModel");
	if (!source || !depth || !viewModel) return false;

	TAAUniforms uniforms = {};
	memcpy(&uniforms.projection, projection, sizeof(D3DXMATRIX));
	memcpy(&uniforms.view, view, sizeof(D3DXMATRIX));
	uniforms.prevView = Constants.PrevViewTransform;
	uniforms.reciprocalResolution = *reciprocal;
	uniforms.cameraData = *cameraData;
	uniforms.data = Constants.Data;
	uniforms.prevProjection = Constants.PrevProjection;
	uniforms.cameraDelta = Constants.CameraDelta;
	const bool objectMotion = ObjectMotion::Ready();	// UNOFFICIAL 2026-10-29: cameraDelta.w = TAA.glsl's switch for t_objmotion
	uniforms.cameraDelta.w = objectMotion ? 1.0f : 0.0f;
	uniforms.weapon = Constants.Weapon;
	uniforms.sharpen = Constants.Sharpen;
	ID3D9NvrExtBuffer* buffer = ExtUniforms(extUniforms, &uniforms, sizeof(uniforms));
	if (!buffer) return false;

	ID3D9NvrExtSampler* point = DxvkCompute::PointClamp();
	ID3D9NvrExtSampler* linear = DxvkCompute::LinearClamp();
	if (!point || !linear) return false;

	// One pass: the textures as the .fx binds them (a placeholder where the pass does not read one), into one target.
	static const char* const spvNames[3] = { "TAAResolve", "TAADebug", "TAAOutput" };
	auto draw = [&](int index, IDirect3DBaseTexture9* const* textures, IDirect3DSurface9* const* targets) -> bool {
		ExtPass& pass = extPasses[index];
		if (!pass.Tried) {
			pass.Tried = true;
			pass.Pipeline = DxvkCompute::CreateGraphicsPipeline(spvNames[index], TAABindings, _countof(TAABindings), sizeof(TAAPush), true);
		}
		if (!pass.Pipeline) return false;
		NVR_EXT_RESOURCE_BINDING resources[_countof(TAABindings)] = {};
		for (UINT i = 0; i < TAATextureCount; i++) {
			resources[i].Slot = i; resources[i].Type = NVR_EXT_BIND_SAMPLED_IMAGE; resources[i].pResource = textures[i];
		}
		resources[TAATextureCount + 0].Slot = 0; resources[TAATextureCount + 0].Type = NVR_EXT_BIND_UNIFORM_BUFFER; resources[TAATextureCount + 0].pResource = buffer;
		resources[TAATextureCount + 1].Slot = 0; resources[TAATextureCount + 1].Type = NVR_EXT_BIND_SAMPLER; resources[TAATextureCount + 1].pResource = point;
		resources[TAATextureCount + 2].Slot = 1; resources[TAATextureCount + 2].Type = NVR_EXT_BIND_SAMPLER; resources[TAATextureCount + 2].pResource = linear;
		TAAPush push = {};
		return ExtDrawTo(pass.Pipeline, targets, 1, resources, _countof(resources), &push, sizeof(push));
	};

	// The same reads as the D3D9 draws: the resolve reads last frame's result (historyIndex) and writes the other buffer; the debug
	// view reads what the resolve read; the output reads this frame's resolve (the buffer that becomes next frame's history).
	IDirect3DBaseTexture9* history[2] = { Textures.ResolveTexture, Textures.HistoryTexture };
	IDirect3DSurface9* surfaces[2] = { Textures.ResolveSurface, Textures.HistorySurface };
	IDirect3DBaseTexture9* motion = objectMotion ? (IDirect3DBaseTexture9*)ObjectMotion::Texture : depth;
	IDirect3DBaseTexture9* resolveInputs[TAATextureCount] = { source, history[historyIndex], depth, viewModel, motion };
	IDirect3DSurface9* resolveTarget = surfaces[write];
	if (!draw(0, resolveInputs, &resolveTarget)) return false;

	IDirect3DBaseTexture9* outputInputs[TAATextureCount] = { source, history[write], depth, viewModel, motion };
	const bool shown = Settings.DebugView > 0 ? draw(1, resolveInputs, &RenderTarget) : draw(2, outputInputs, &RenderTarget);
	if (shown && ++ExtFrames == 1) Logger::Log("UNOFFICIAL Vegas Shaders DXVK: %s runs as a Vulkan pass.", Name);
	return shown;
}

// Halton low-discrepancy sequence: the radical inverse of index in the given base. Bases 2 and 3
// give well-spread 2D points, so any run of consecutive frames covers the pixel evenly rather than
// clumping, which is why it is the standard TAA jitter pattern.
static float Halton(int index, int base) {
	float result = 0.0f;
	float fraction = 1.0f / (float)base;
	while (index > 0) {
		result += (float)(index % base) * fraction;
		index /= base;
		fraction /= (float)base;
	}
	return result;
}

// The same gates RenderEffects applies before running any effect, plus TAA's own. Jitter without a
// resolve to average it is just the whole world shaking by half a pixel, so it must only be applied
// on frames where this effect is going to run.
bool TAAEffect::WillResolveThisFrame() {
	return Enabled && Effect != nullptr && ShouldRender() && Settings.Jitter &&
		Textures.ResolveSurface && Textures.HistorySurface &&
		TheSettingManager->SettingsMain.Main.RenderEffects &&
		Player && Player->parentCell &&
		!TheShaderManager->GameState.OverlayIsOn;
}

// Jitter the world render by shifting the camera frustum a fraction of a pixel.
//
// Gamebryo builds the projection from the camera's frustum, so an off-centre frustum IS a jittered
// projection -- the same _31/_32 offsets NVR's own SetupSceneCamera derives from Left+Right and
// Top+Bottom. Shifting the frustum rather than patching a matrix means every path agrees: whether
// the game rebuilds its projection from the frustum when it sets the camera, or keeps the matrix
// NVR last wrote, it gets the jittered one, because SetupSceneCamera is re-run right here.
//
// Applied around the world scene only. Shadow maps are rendered before it -- their cascades are
// fitted to the view frustum, and at the far cascade half a pixel is several world units, about a
// shadow texel, so jittering them would make shadow edges shimmer (RenderShadowMapHook logs if
// that ordering is ever violated). The first-person weapon has its own camera and stays unjittered,
// which matches TAA giving it zero motion. Everything after End -- image space, every NVR effect,
// and TAA's own reprojection -- sees the unjittered camera, exactly as without jitter.
void TAAEffect::BeginJitter(bool firstWorld) {
	EndJitter(); // never stack two shifts, whatever happened last frame
	frameJittered = false;
	postResolveJitter = D3DXVECTOR2(0.0f, 0.0f);	// UNOFFICIAL 2026-11-01
	if (firstWorld) worldMipBias = 0.0f;	// UNOFFICIAL 2026-10-28 (WorldMipBias): set again below when a bias goes on

	// UNOFFICIAL 2026-10-27 RenderScale (TAAUpscale.cpp): the world scene at the lower internal resolution. The jitter is then FSR
	// 2's own (Halton, ffxFsr2GetJitterPhaseCount steps, in render pixels) and the mip bias adds log2 of the scale (AMD's advice:
	// textures at the screen's sharpness, not the render size's). Off: the code below, exactly as before.
	if (UpscaleBegin(firstWorld)) {
		NiCamera* camera = Up.Camera;
		Up.Jittered = false;
		if (Settings.Jitter && camera) {
			const int phases = (std::max)(1, (int)Up.JitterPhases);
			const int index = (Up.JitterIndex % phases) + 1;
			const float jitterX = Halton(index, 2) - 0.5f;
			const float jitterY = Halton(index, 3) - 0.5f;
			// The frustum spans the render viewport: one render pixel is (Right - Left) / RenderWidth.
			frustumShiftX = jitterX * (camera->Frustum.Right - camera->Frustum.Left) / (float)Up.RenderWidth;
			frustumShiftY = jitterY * (camera->Frustum.Top - camera->Frustum.Bottom) / (float)Up.RenderHeight;
			camera->Frustum.Left += frustumShiftX;
			camera->Frustum.Right += frustumShiftX;
			camera->Frustum.Top += frustumShiftY;
			camera->Frustum.Bottom += frustumShiftY;
			jitterCamera = camera;
			jitterActive = true;
			Up.JitterX = jitterX;
			Up.JitterY = jitterY;
			Up.Jittered = true;
			TheRenderManager->SetupSceneCamera();
		}
		const float scale = (float)Up.RenderWidth / (float)(std::max)(Up.Width, 1u);
		const float bias = (std::max)(-3.0f, log2f(scale) + (Settings.Jitter ? Settings.MipBias : 0.0f));
		if (bias < 0.0f) {
			SetMipBias(bias);
			mipBiasActive = true;
			if (firstWorld) worldMipBias = bias;
		}
		return;
	}

	if (!WillResolveThisFrame()) return;
	NiCamera* camera = WorldSceneGraph ? WorldSceneGraph->camera : nullptr;
	if (!camera || camera->Frustum.Ortho) return;

	// Pixel offset in (-0.5, 0.5), from Halton(2,3) indices 1..8.
	int index = (jitterIndex % JitterSequenceLength) + 1;
	float jitterX = Halton(index, 2) - 0.5f;
	float jitterY = Halton(index, 3) - 0.5f;

	// Frustum edges are in view-plane units at distance 1, so one pixel is (Right - Left) / width.
	frustumShiftX = jitterX * (camera->Frustum.Right - camera->Frustum.Left) / (float)TheRenderManager->width;
	frustumShiftY = jitterY * (camera->Frustum.Top - camera->Frustum.Bottom) / (float)TheRenderManager->height;

	camera->Frustum.Left += frustumShiftX;
	camera->Frustum.Right += frustumShiftX;
	camera->Frustum.Top += frustumShiftY;
	camera->Frustum.Bottom += frustumShiftY;

	jitterCamera = camera;
	jitterActive = true;
	frameJitterX = jitterX;	// UNOFFICIAL 2026-10-26: FSR 2 needs this frame's offset (TAAFSR.cpp)
	frameJitterY = jitterY;
	frameJittered = true;
	TheRenderManager->SetupSceneCamera();

	if (Settings.MipBias < 0.0f) {
		SetMipBias(Settings.MipBias);
		mipBiasActive = true;
		if (firstWorld) worldMipBias = Settings.MipBias;
	}
}

// UNOFFICIAL 2026-10-28 (TAA.h): the picture moved left / down by (JitterX, JitterY) render pixels (BeginJitter), so a pixel of the small
// frame shows the unjittered screen at + (JitterX / width, -JitterY / height). Only while the effects run small before the upscale (Deferred).
D3DXVECTOR2 TAAEffect::LowResJitterUV() const {
	if (!Up.Deferred || !Up.Jittered || !Up.RenderWidth || !Up.RenderHeight) return D3DXVECTOR2(0.0f, 0.0f);
	return D3DXVECTOR2(Up.JitterX / (float)Up.RenderWidth, -Up.JitterY / (float)Up.RenderHeight);
}

D3DXVECTOR2 TAAEffect::DepthJitterUV() const {
	const D3DXVECTOR2 low = LowResJitterUV();
	if (low.x != 0.0f || low.y != 0.0f) return low;
	if (!frameJittered || !TheRenderManager->width || !TheRenderManager->height) return D3DXVECTOR2(0.0f, 0.0f);
	return D3DXVECTOR2(frameJitterX / (float)TheRenderManager->width, -frameJitterY / (float)TheRenderManager->height);
}

void TAAEffect::EndJitter() {
	if (mipBiasActive) {
		SetMipBias(0.0f);
		mipBiasActive = false;
	}
	UpscaleRestorePort();	// UNOFFICIAL 2026-10-27 RenderScale: the world camera's viewport port back to the whole screen
	if (!jitterActive) return;

	if (jitterCamera) {
		jitterCamera->Frustum.Left -= frustumShiftX;
		jitterCamera->Frustum.Right -= frustumShiftX;
		jitterCamera->Frustum.Top -= frustumShiftY;
		jitterCamera->Frustum.Bottom -= frustumShiftY;
	}

	jitterActive = false;
	jitterCamera = nullptr;
	frustumShiftX = 0.0f;
	frustumShiftY = 0.0f;
	TheRenderManager->SetupSceneCamera();
}

void TAAEffect::Render(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	if (!Enabled || Effect == nullptr || !ShouldRender() || !EnsureTextures()) {
		// UNOFFICIAL 2026-11-01 (core/EffectStatus.h)
		EffectStatus::Note(StatusSlot, Name, !Enabled ? EffectStatus::Disabled : Effect == nullptr ? EffectStatus::NotLoaded :
			!ShouldRender() ? EffectStatus::ShouldRenderFalse : EffectStatus::Skipped, Enabled && Effect ? "no textures" : nullptr);
		// Whatever is in the history now will be stale, or somewhere else entirely, by the time
		// this runs again.
		historyValid = false;
		renderTime = 0.0f;
		return;
	}

	// UNOFFICIAL 2026-10-27 RenderScale (TAAUpscale.cpp): this frame's world was drawn at the lower internal resolution and
	// already upscaled (and anti-aliased) by FSR 2 before NVR's effects; there is nothing left to resolve here. TAA's own history
	// (and FSR's at native resolution) starts again whenever a frame is not upscaled.
	// UNOFFICIAL 2026-11-01 SmoothEffects ([Shaders.TAA.FSR]; user: "the noise smoothing looks 100x better than none, we should apply that to
	// both regular fsr and dlss"): with LowResEffects on, FSR / DLSS run after NVR's effects and average their noise (AmbientLight's, the
	// shadows', the fog's grain) over frames; with it off the world is upscaled first and nothing averaged the effects, so they shimmered
	// on grass and foliage. On: those frames get TAA's own resolve here, after the effects, on the upscaled picture (it reprojects by depth
	// and the camera and needs no jitter; the upscaler keeps its own jitter steps, so the index is not moved here). Off: skipped, as before.
	// (2026-11-01: not when the upscale ran after the effects, v2: FSR / DLSS averaged them already; a second resolve only blurred)
	const bool afterUpscale = Up.Upscaled && !Up.AfterEffects && FSRSettings.SmoothEffects;
	// UNOFFICIAL 2026-11-01 (PostResolveDepthJitterUV): the depth keeps this frame's jitter past the resolve at native size (an upscale
	// redrew it unjittered: TAAUpscale.cpp's composite).
	postResolveJitter = !Up.Upscaled && frameJittered && TheRenderManager->width && TheRenderManager->height ?
		D3DXVECTOR2(frameJitterX / (float)TheRenderManager->width, -frameJitterY / (float)TheRenderManager->height) : D3DXVECTOR2(0.0f, 0.0f);
	Up.AfterEffects = false;
	if (Up.Upscaled && !afterUpscale) {
		// (2026-11-07 perf-rr) a rect frame DLSS RR denoised and upscaled at the RR point (TAAFSR.cpp, UpscaleAfterRayReconstruction) is one of
		// these too: RenderScale's jitter index stepped on there; its flag is spent here (a frame without the RR point must not find it set)
		EffectStatus::Note(StatusSlot, Name, EffectStatus::Skipped, rrRanThisFrame ? "DLSS Ray Reconstruction denoised and upscaled this RenderScale frame" :
			"RenderScale: the world was upscaled before the effects");	// UNOFFICIAL 2026-11-01
		rrRanThisFrame = false;
		Up.Upscaled = false;
		historyValid = false;
		lastPathFSR = false;
		frameJittered = false;
		renderTime = 0.0f;
		return;
	}
	Up.Upscaled = false;
	// UNOFFICIAL 2026-10-30 DLSS Ray Reconstruction (TAAFSR.cpp RayReconstruct): this frame was denoised and anti-aliased by DLSS-D before the
	// tonemap, on TAA's own jitter; nothing is left to resolve. The jitter goes on to its next step (RR keeps no index of its own); TAA's
	// history (and DLSS SR's) starts again on the next frame RR does not run.
	if (rrRanThisFrame) {
		EffectStatus::Note(StatusSlot, Name, EffectStatus::Skipped, "DLSS Ray Reconstruction resolved this frame");	// UNOFFICIAL 2026-11-01
		rrRanThisFrame = false;
		historyValid = false;
		lastPathFSR = false;
		jitterIndex = (jitterIndex + 1) % JitterSequenceLength;
		frameJittered = false;
		renderTime = 0.0f;
		return;
	}

	auto timer = TimeLogger();
	// UNOFFICIAL (as in the pr/nvr-optimizations merge of #78): this path renders into the game target and copies it into
	// RenderedSurface itself. With the copy-free frame chain the current image may sit in a chain texture instead, so hand
	// it to the game target first and use the current RenderedSurface, which the sync can move.
	TheShaderManager->Chain.Sync();
	RenderedSurface = TheTextureManager->RenderedSurface;

	// UNOFFICIAL 2026-10-26 (NVR DXVK): FSR 2 as the resolve when [Shaders.TAA.FSR] Enabled and the extension is there. Each path
	// keeps its own history, so switching between them starts from scratch.
	const bool useFSR = !afterUpscale && FSRWanted();	// (SmoothEffects: TAA's own resolve, the upscaler already ran)
	const bool hadHistory = historyValid;	// UNOFFICIAL 2026-11-01 (Diag)
	if (useFSR != lastPathFSR) { if (historyValid) SessionLog::HistoryReset("TAA", "path switch (TAA / FSR)"); historyValid = false; }

	// A new cell means a loading screen or a teleport: last frame's history shows another place.
	if (TheShaderManager->GameState.isCameraCut) { if (historyValid) SessionLog::HistoryReset("TAA", "camera cut"); historyValid = false; }

	// With no usable history, make last frame's camera this frame's. The reprojection becomes the
	// identity, and the shader ignores the history anyway because Data.z is 0.
	if (!historyValid) RememberCamera();

	D3DXVECTOR4 delta = TheRenderManager->CameraPosition - prevCameraPosition;
	Constants.CameraDelta = D3DXVECTOR4(delta.x, delta.y, delta.z, 0.0f);
	Constants.Data.z = historyValid ? 1.0f : 0.0f;

	if (SourceBuffer) Device->StretchRect(RenderTarget, NULL, SourceBuffer, NULL, D3DTEXF_NONE);

	// UNOFFICIAL 2026-10-13: the two FP16 buffers take turns. Last frame's result (historyIndex) is read as the history
	// (TESR_TAAHistoryBuffer) and this frame's resolve goes into the other one, which Output then shows and which is next
	// frame's history: no full-screen copy of the resolve into a fixed history buffer.
	const int write = historyIndex ^ 1;
	// UNOFFICIAL 2026-10-25 (NVR DXVK): the passes as Vulkan passes when NvrDxvkPasses is on (ExtDraw); the D3D9 techniques otherwise,
	// and when a pass fails (the extension path is then off for the session).
	bool fsrShown = useFSR && FSRDraw(Device, RenderTarget, !historyValid);
	if (useFSR && !fsrShown) {
		// FSR failed this frame (logged; it stays off): TAA's own resolve, from scratch.
		historyValid = false;
		RememberCamera();
		Constants.CameraDelta = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);
		Constants.Data.z = 0.0f;
	}
	bool shown = fsrShown || (!ExtFailed && DxvkCompute::PassesOn() && ExtDraw(Device, RenderTarget, write));
	if (shown) {
		Device->SetRenderTarget(0, RenderTarget);
	}
	else {
		SetCT();
		IDirect3DTexture9* const buffers[2] = { Textures.ResolveTexture, Textures.HistoryTexture };
		IDirect3DSurface9* const surfaces[2] = { Textures.ResolveSurface, Textures.HistorySurface };
		Device->SetTexture(1, buffers[historyIndex]);
		Device->SetRenderTarget(0, surfaces[write]);
		bool resolved = DrawTechnique("Resolve");

		// A debug view goes to the screen reading the same last frame the resolve read (stage 1 still holds it), so the view
		// sees exactly what the resolve saw, and the debug picture never becomes next frame's input.
		Device->SetRenderTarget(0, RenderTarget);
		bool debugDrawn = resolved && Settings.DebugView > 0 && DrawTechnique("Debug");

		if (resolved) Device->SetTexture(1, buffers[write]);	// Output shows this frame's resolve

		shown = resolved && (debugDrawn || DrawTechnique("Output"));
	}

	// UNOFFICIAL 2026-11-01 (core/EffectStatus.h): the resolve this frame and its history
	EffectStatus::Note(StatusSlot, Name, shown ? EffectStatus::Drawn : EffectStatus::Skipped, fsrShown ? "FSR / DLSS at native resolution" :
		useFSR ? "TAA resolve (FSR failed this frame)" : afterUpscale && shown ? "TAA resolve after the upscale (SmoothEffects)" : shown ? "TAA resolve" : "the resolve failed");
	// UNOFFICIAL 2026-11-01 (Diagnostics.h part 5): the path's state for the log, and the history watch's probe of this frame's input
	// (the picture before the resolve, SourceBuffer) and output (the upscaler's output, or TAA's own resolve buffer)
	if (!fsrShown) {
		Diag.path = useFSR ? "TAA's own resolve (FSR failed this frame)" : "TAA's own resolve";
		Diag.upscaler.clear();
		Diag.reset = !hadHistory || Constants.Data.z < 0.5f;
		Diag.jitter = jitterIndex;
		Diag.jitterX = frameJittered ? frameJitterX : 0.0f;
		Diag.jitterY = frameJittered ? frameJitterY : 0.0f;
		Diag.inW = Diag.outW = TheRenderManager->width;
		Diag.inH = Diag.outH = TheRenderManager->height;
		Diag.hdr = false;
		Diag.output = nullptr;
	}
	Diag.sinceReset = Diag.reset ? 0 : Diag.sinceReset + 1;
	Diag.frame = TheFrameRateManager ? TheFrameRateManager->FrameNumber : 0;
	if (shown) {
		IDirect3DSurface9* probeOut = nullptr;
		if (fsrShown && Diag.output) Diag.output->GetSurfaceLevel(0, &probeOut);
		else if (!fsrShown && Settings.DebugView <= 0) { probeOut = (historyIndex ^ 1) ? Textures.HistorySurface : Textures.ResolveSurface; if (probeOut) probeOut->AddRef(); }
		if (probeOut) {
			Diagnostics::HistoryProbe("TAA / upscaler at native resolution", SourceBuffer, 0, 0, probeOut, &TAAEffect::DiagTextStatic);
			probeOut->Release();
		}
	}
	EffectStatus::History(StatusSlot, Name, Constants.Data.z > 0.5f ? "kept" : "reset: no history (cut, path switch, first frame)");
	if (shown) {
		if (RenderedSurface) Device->StretchRect(RenderTarget, NULL, RenderedSurface, NULL, D3DTEXF_NONE);
		RememberCamera();
		historyValid = true;
		if (!fsrShown) historyIndex = write;	// FSR keeps its own ping-pong resources
		if (!afterUpscale) jitterIndex = (jitterIndex + 1) % JitterSequenceLength;	// (SmoothEffects: the upscaler steps its own jitter)
	}
	else {
		historyValid = false;
	}
	lastPathFSR = fsrShown;
	frameJittered = false;

	renderTime = timer.LogTime("EffectRecord::Render TAA");
}

// UNOFFICIAL 2026-11-01 (core/Diagnostics.h part 5): the resolve path's state as of its last frame, one phrase
std::string TAAEffect::UpscaleDiagText() const {
	char t[400];
	sprintf_s(t, "%s | history %s, %u frames accumulated | jitter step %d of %d (%.3f, %.3f px) | %ux%u -> %ux%u%s | frame gap %.1f ms | "
		"[Shaders.TAA.FSR] Enabled %s RenderScale %.2f Version %d, [Shaders.TAA.DLSS] Enabled %s RenderScale %.2f, Upscaler %d, LowResEffects %s | last described frame f%u",
		Diag.path, Diag.reset ? "RESET that frame" : "kept", Diag.sinceReset, Diag.jitter, JitterSequenceLength, Diag.jitterX, Diag.jitterY, Diag.inW, Diag.inH,
		Diag.outW, Diag.outH, Diag.hdr ? " (HDR input)" : "", Diag.deltaMs, FSRSettings.Enabled ? "on" : "off", TheSettingManager->GetSettingF("Shaders.TAA.FSR", "RenderScale"),
		TheSettingManager->GetSettingI("Shaders.TAA.FSR", "Version"), TheSettingManager->GetSettingI("Shaders.TAA.DLSS", "Enabled") ? "on" : "off",
		TheSettingManager->GetSettingF("Shaders.TAA.DLSS", "RenderScale"), FSRSettings.Upscaler, FSRSettings.LowResEffects ? "on" : "off", Diag.frame);
	std::string text = t;
	if (!Diag.upscaler.empty()) text += " | " + Diag.upscaler;
	return text;
}

std::string TAAEffect::DiagTextStatic() {
	TAAEffect* taa = TheShaderManager ? TheShaderManager->Effects.TAA : nullptr;
	return taa ? taa->UpscaleDiagText() : std::string("no TAA effect");
}
