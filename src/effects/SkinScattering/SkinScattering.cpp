// UNOFFICIAL 2026-10-15: CREDIT Conor (mrfury97), from his NVR testbed (src/effects/SkinScattering.cpp); see SkinScattering.h.
#include <algorithm>
#include <vector>

#include "SkinScattering.h"
#include "../../core/TemporalBackbone.h"	// UNOFFICIAL 2026-11-01 SharedNoise
#include "../../core/PassSums.h"
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-25 (NVR DXVK)
#include "../../core/RenderRect.h"	// UNOFFICIAL 2026-11-18 next-111: the blur in the rect frame (ShouldRender)
#include "../../core/CapturedNormals.h"	// UNOFFICIAL 2026-10-31 AlbedoBuffer: the skin's albedo target is its skin source (NoteSkinAlbedo)
#include "../../core/DeferredSkin.h"	// UNOFFICIAL 2026-11-15 DeferredSkin: its records write the G-buffer, not these targets; DeferredSkinBlur fills them
#include "../../core/MaterialSSSProto.h"
#include "../../core/RtPathTrace.h"

static const float SkinScatterMMPerUnit = 14.2857f;   // one game unit is 1/70 m

void SkinScatteringEffect::RegisterConstants() {
	TheShaderManager->RegisterConstant("TESR_SkinScatterData", &Constants.Data);
	TheShaderManager->RegisterConstant("TESR_SkinScatterKernel", &Constants.Kernel[0]);
	TheShaderManager->RegisterConstant("TESR_SkinScatterDebug", &Constants.Debug);
}

void SkinScatteringEffect::RegisterTextures() {
	// Created on the first skin draw, to match the scene target exactly (size, multisampling).
	TheTextureManager->RegisterTexture("TESR_SkinScatterBuffer", (IDirect3DBaseTexture9**)&Texture);
	TheTextureManager->RegisterTexture("TESR_SkinAlbedoBuffer", (IDirect3DBaseTexture9**)&AlbedoTexture);

	D3DCAPS9 Caps;
	TheRenderManager->device->GetDeviceCaps(&Caps);
	Supported = Caps.NumSimultaneousRTs >= 3
		&& (Caps.PrimitiveMiscCaps & D3DPMISCCAPS_INDEPENDENTWRITEMASKS)
		&& (Caps.PrimitiveMiscCaps & D3DPMISCCAPS_MRTINDEPENDENTBITDEPTHS);
	if (!Supported) Logger::Log("[WARNING] SkinScattering: the device cannot bind three render targets of their own formats; skin scattering is off");
}

// The settings live on the skin shader's own menu pages: [Shaders.Skin.Scattering] outdoors,
// [Shaders.Skin.Interiors] indoors. There is no [Shaders.SkinScattering] section, so the effect
// record itself is always on and ScreenSpace is the switch (with [Shaders.Skin.Status] Enabled
// above it).
static void ReadProfile(SkinScatteringEffect::ProfileStruct* Profile, const char* Section) {
	Profile->ScreenSpace = TheSettingManager->GetSettingI(Section, "ScreenSpace") != 0;
	Profile->Width = max(0.0f, TheSettingManager->GetSettingF(Section, "Width"));
	Profile->DepthFollow = max(0.0f, TheSettingManager->GetSettingF(Section, "DepthFollow"));
	Profile->AlbedoDetail = std::clamp(TheSettingManager->GetSettingF(Section, "AlbedoDetail"), 0.0f, 1.0f);
	Profile->Strength = D3DXVECTOR3(
		TheSettingManager->GetSettingF(Section, "StrengthRed"),
		TheSettingManager->GetSettingF(Section, "StrengthGreen"),
		TheSettingManager->GetSettingF(Section, "StrengthBlue"));
	// Falloff narrows each channel's profile inside the kernel's fixed sample range (a colour,
	// 0-1, in Community Shaders). Above 1 the profile outgrows the samples: its weight lands on the
	// few widely spaced outer taps and every bright spot is copied out as a row of dots. Width is
	// what widens the scattering, samples and all.
	Profile->Falloff = D3DXVECTOR3(
		std::clamp(TheSettingManager->GetSettingF(Section, "FalloffRed"), 0.0f, 1.0f),
		std::clamp(TheSettingManager->GetSettingF(Section, "FalloffGreen"), 0.0f, 1.0f),
		std::clamp(TheSettingManager->GetSettingF(Section, "FalloffBlue"), 0.0f, 1.0f));
}

void SkinScatteringEffect::UpdateSettings() {
	ReadProfile(&ExteriorProfile, "Shaders.Skin.Scattering");
	ReadProfile(&InteriorProfile, "Shaders.Skin.Interiors");
	// [Shaders.Skin.Debug] DebugView 8-10 are this effect's (its views 1-3); see SkinShaders.
	const int DebugView = std::clamp(TheSettingManager->GetSettingI("Shaders.Skin.Debug", "DebugView"), 0, SkinShaders::DebugViewCount);
	Constants.Debug.x = DebugView >= SkinShaders::FirstScatteringDebugView ? (float)(DebugView - SkinShaders::FirstScatteringDebugView + 1) : 0.0f;
	SkinShaderDebug = DebugView > 0 && DebugView < SkinShaders::FirstScatteringDebugView;
	KernelDirty = true;
}

void SkinScatteringEffect::UpdateConstants() {
	if (!TheShaderManager->Shaders.Skin->Active()) return;
	// Indoors or out: the kernel is rebuilt when the profile in use changes.
	const bool isInterior = !TheShaderManager->GameState.isExterior;
	if (isInterior != ProfileIsInterior) {
		ProfileIsInterior = isInterior;
		KernelDirty = true;
	}
	Profile = isInterior ? InteriorProfile : ExteriorProfile;
	ScreenSpace = Profile.ScreenSpace;

	if (KernelDirty) {
		CalculateKernel();
		KernelDirty = false;
	}
	Constants.Data.x = Profile.Width / SkinScatterMMPerUnit;   // game units per kernel unit (mm)
	Constants.Data.y = Profile.DepthFollow;
	Constants.Data.z = 0.002f;                                 // relative distance tolerance of the skin test
	Constants.Data.w = Profile.AlbedoDetail;
	// Re-sync 2026-10-19 (Conor's 00e81af5): as Community Shaders, the kernel turned by a random angle per pixel, the two
	// passes staying at right angles, which breaks the blur's axis-aligned rows of taps up into noise. Only with TAA, which
	// averages the noise away (CS always has its own temporal AA); the angle moves each frame. Port: the frame is
	// FrameNumber (once per rendered frame) instead of his own counter, which UpdateConstants advanced twice a frame.
	TAAEffect* TAA = TheShaderManager->Effects.TAA;
	Constants.Debug.y = TAA && TAA->Enabled && TAA->Effect ? 1.0f : 0.0f;
	Constants.Debug.z = (float)(TheFrameRateManager->FrameNumber % 64);
	// UNOFFICIAL 2026-11-01 SharedNoise (core/TemporalBackbone.h): turned only while TAA / FSR / DLSS really resolves this frame (not just
	// TAA switched on: a frame it skips would show the turned kernel raw), at the shared index with its own offset. Off: as above.
	if (TemporalBackbone::Shared()) {
		Constants.Debug.y = TemporalBackbone::TemporalResolve() ? 1.0f : 0.0f;
		Constants.Debug.z = (float)((TemporalBackbone::NoiseIndex() + TemporalBackbone::OffsetSkin) % 64);
	}
}

// Ported from Community Shaders' SubsurfaceScattering::CalculateKernel (after Jimenez's
// SeparableSSS): d'Eon's skin profile, its red channel shaped per channel by Falloff, sampled
// at offsets packed toward the centre, normalised, then lerped toward identity by Strength.
static D3DXVECTOR3 SkinScatterGaussian(const D3DXVECTOR3& Falloff, float Variance, float R) {
	float g[3];
	const float falloff[3] = { Falloff.x, Falloff.y, Falloff.z };
	for (int i = 0; i < 3; i++) {
		float rr = R / (0.001f + falloff[i]);
		g[i] = exp(-(rr * rr) / (2.0f * Variance)) / (2.0f * 3.14f * Variance);
	}
	return D3DXVECTOR3(g[0], g[1], g[2]);
}

static D3DXVECTOR3 SkinScatterProfile(const D3DXVECTOR3& Falloff, float R) {
	// The narrowest d'Eon lobe (0.233 at 0.0064 mm^2) is light that barely enters the skin;
	// Strength accounts for it.
	return 0.100f * SkinScatterGaussian(Falloff, 0.0484f, R)
		+ 0.118f * SkinScatterGaussian(Falloff, 0.187f, R)
		+ 0.113f * SkinScatterGaussian(Falloff, 0.567f, R)
		+ 0.358f * SkinScatterGaussian(Falloff, 1.99f, R)
		+ 0.078f * SkinScatterGaussian(Falloff, 7.41f, R);
}

void SkinScatteringEffect::CalculateKernel() {
	const int N = KernelSamples;
	const float Range = N > 20 ? 3.0f : 2.0f;
	const float Exponent = 2.0f;
	D3DXVECTOR4* K = Constants.Kernel;

	float step = 2.0f * Range / (N - 1);
	for (int i = 0; i < N; i++) {
		float o = -Range + float(i) * step;
		float sign = o < 0.0f ? -1.0f : 1.0f;
		K[i].w = Range * sign * fabs(pow(o, Exponent)) / pow(Range, Exponent);
	}

	for (int i = 0; i < N; i++) {
		float w0 = i > 0 ? fabs(K[i].w - K[i - 1].w) : 0.0f;
		float w1 = i < N - 1 ? fabs(K[i].w - K[i + 1].w) : 0.0f;
		float area = (w0 + w1) / 2.0f;
		D3DXVECTOR3 t = area * SkinScatterProfile(Profile.Falloff, K[i].w);
		K[i].x = t.x;
		K[i].y = t.y;
		K[i].z = t.z;
	}

	// The centre sample first.
	D3DXVECTOR4 centre = K[N / 2];
	for (int i = N / 2; i > 0; i--) K[i] = K[i - 1];
	K[0] = centre;

	D3DXVECTOR3 sum(0.0f, 0.0f, 0.0f);
	for (int i = 0; i < N; i++) sum += D3DXVECTOR3(K[i].x, K[i].y, K[i].z);
	for (int i = 0; i < N; i++) {
		K[i].x /= max(sum.x, 1e-6f);
		K[i].y /= max(sum.y, 1e-6f);
		K[i].z /= max(sum.z, 1e-6f);
	}

	const D3DXVECTOR3& s = Profile.Strength;
	K[0].x = (1.0f - s.x) + s.x * K[0].x;
	K[0].y = (1.0f - s.y) + s.y * K[0].y;
	K[0].z = (1.0f - s.z) + s.z * K[0].z;
	for (int i = 1; i < N; i++) {
		K[i].x *= s.x;
		K[i].y *= s.y;
		K[i].z *= s.z;
	}
}

// --- The second render target ----------------------------------------------------------------

void SkinScatteringEffect::ReleaseTarget() {
	if (MSAASurface) { MSAASurface->Release(); MSAASurface = nullptr; }
	if (TextureSurface) { TextureSurface->Release(); TextureSurface = nullptr; }
	if (Texture) { Texture->Release(); Texture = nullptr; }
	if (AlbedoMSAASurface) { AlbedoMSAASurface->Release(); AlbedoMSAASurface = nullptr; }
	if (AlbedoTextureSurface) { AlbedoTextureSurface->Release(); AlbedoTextureSurface = nullptr; }
	if (AlbedoTexture) { AlbedoTexture->Release(); AlbedoTexture = nullptr; }
	TargetDesc = {};
	// Every shader that sampled the old textures holds their raw pointers.
	ClearSampler("TESR_SkinScatterBuffer", strlen("TESR_SkinScatterBuffer"));
	ClearSampler("TESR_SkinAlbedoBuffer", strlen("TESR_SkinAlbedoBuffer"));
}

// True when the scene target is the main view and the second target matches it.
bool SkinScatteringEffect::EnsureTarget(const D3DSURFACE_DESC& SceneDesc) {
	// Reflections, menus and other render-to-texture views draw skin into smaller targets.
	if (SceneDesc.Width != TheRenderManager->width || SceneDesc.Height != TheRenderManager->height) return false;

	if (Texture && TargetDesc.Width == SceneDesc.Width && TargetDesc.Height == SceneDesc.Height &&
		TargetDesc.MultiSampleType == SceneDesc.MultiSampleType && TargetDesc.MultiSampleQuality == SceneDesc.MultiSampleQuality)
		return true;

	// The world scene's target never changes between frames. If it seems to, some other view is
	// drawing skin into a target of the same size, and recreating every time would thrash.
	if (Texture && ++Recreations > 2) {
		Logger::Log("[WARNING] SkinScattering: skin is drawn into more than one full-screen target; skin scattering is off");
		ReleaseTarget();
		Supported = false;
		return false;
	}

	ReleaseTarget();
	IDirect3DDevice9* Device = TheRenderManager->device;
	if (FAILED(Device->CreateTexture(SceneDesc.Width, SceneDesc.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &Texture, NULL))) {
		Logger::Log("[ERROR] SkinScattering: could not create the skin target; skin scattering is off");
		Texture = nullptr;
		Supported = false;
		return false;
	}
	Texture->GetSurfaceLevel(0, &TextureSurface);
	if (FAILED(Device->CreateTexture(SceneDesc.Width, SceneDesc.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &AlbedoTexture, NULL))) {
		Logger::Log("[ERROR] SkinScattering: could not create the skin albedo target; skin scattering is off");
		AlbedoTexture = nullptr;
		ReleaseTarget();
		Supported = false;
		return false;
	}
	AlbedoTexture->GetSurfaceLevel(0, &AlbedoTextureSurface);

	// MRT targets must share the scene's multisampling; resolved into the textures before use.
	if (SceneDesc.MultiSampleType != D3DMULTISAMPLE_NONE) {
		if (FAILED(Device->CreateRenderTarget(SceneDesc.Width, SceneDesc.Height, D3DFMT_A16B16G16R16F,
			SceneDesc.MultiSampleType, SceneDesc.MultiSampleQuality, FALSE, &MSAASurface, NULL)) ||
			FAILED(Device->CreateRenderTarget(SceneDesc.Width, SceneDesc.Height, D3DFMT_A16B16G16R16F,
			SceneDesc.MultiSampleType, SceneDesc.MultiSampleQuality, FALSE, &AlbedoMSAASurface, NULL))) {
			Logger::Log("[ERROR] SkinScattering: could not create the multisampled skin targets; skin scattering is off");
			ReleaseTarget();
			Supported = false;
			return false;
		}
	}

	TargetDesc = SceneDesc;
	Logger::Log("SkinScattering: skin target %ux%u, %u samples", SceneDesc.Width, SceneDesc.Height, (UInt32)SceneDesc.MultiSampleType);
	return true;
}

// D3D9's Clear clears every bound target, so the skin target is cleared on its own, bound as
// target 0 for a moment, with everything Clear honours put back.
void SkinScatteringEffect::ClearTarget(IDirect3DSurface9* SceneTarget) {
	IDirect3DDevice9* Device = TheRenderManager->device;
	D3DVIEWPORT9 Viewport;
	RECT ScissorRect;
	DWORD Scissor, WriteMask;
	Device->GetViewport(&Viewport);
	Device->GetScissorRect(&ScissorRect);   // binding target 0 resets both
	Device->GetRenderState(D3DRS_SCISSORTESTENABLE, &Scissor);
	Device->GetRenderState(D3DRS_COLORWRITEENABLE, &WriteMask);
	Device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
	Device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);

	Device->SetRenderTarget(0, MSAASurface ? MSAASurface : TextureSurface);
	Device->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);
	Device->SetRenderTarget(0, AlbedoMSAASurface ? AlbedoMSAASurface : AlbedoTextureSurface);
	Device->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);
	Device->SetRenderTarget(0, SceneTarget);

	Device->SetRenderState(D3DRS_COLORWRITEENABLE, WriteMask);
	Device->SetRenderState(D3DRS_SCISSORTESTENABLE, Scissor);
	Device->SetScissorRect(&ScissorRect);
	Device->SetViewport(&Viewport);
}

void SkinScatteringEffect::BeginFrame() {
	Unbind();
	ClearedThisFrame = false;
	SkinDrawn = false;
}

// Called by the SkinShader hook just before a skin geometry is drawn.
void SkinScatteringEffect::BindForSkinDraw(NiD3DPixelShaderEx* PixelShader) {
	if (Bound) return;   // the partitions of one skinned geometry
	if (!Enabled || !ScreenSpace || !Supported || Effect == nullptr) return;

	// The world scene only. The engine draws first-person arms, menus and reflections into other
	// targets, some full-screen without multisampling; the depth test would reject the arms anyway.
	if (!InWorldScene) return;
	if (!TheSettingManager->SettingsMain.Main.RenderEffects || !TheShaderManager->Shaders.Skin->Enabled) return;

	// Only NVR's own skin pixel shaders write the second target.
	if (!PixelShader || PixelShader->ShaderHandle == PixelShader->ShaderHandleBackup) return;
	if (TheRenderManager->renderState->GetPixelShader() != PixelShader->ShaderHandle) return;
	// UNOFFICIAL 2026-11-15 DeferredSkin (core/DeferredSkin.h): a deferred skin record writes the G-buffer on targets 1-3 (CapturedNormals binds them)
	if (DeferredSkin::IsHandle(PixelShader->ShaderHandle)) return;

	IDirect3DDevice9* Device = TheRenderManager->device;

	// Opaque passes write everything; additive light passes (blend ONE, ONE) add their
	// highlights to the colour channels and leave the distance alone. Anything else is a fading
	// or alpha-blended actor, which keeps its own complete colour and is simply not scattered.
	DWORD Blend = FALSE;
	Device->GetRenderState(D3DRS_ALPHABLENDENABLE, &Blend);
	bool Additive = false;
	if (Blend) {
		DWORD Src, Dst;
		Device->GetRenderState(D3DRS_SRCBLEND, &Src);
		Device->GetRenderState(D3DRS_DESTBLEND, &Dst);
		if (Src != D3DBLEND_ONE || Dst != D3DBLEND_ONE) return;
		Additive = true;
	}

	IDirect3DSurface9* Scene = nullptr;
	if (FAILED(Device->GetRenderTarget(0, &Scene)) || !Scene) return;
	D3DSURFACE_DESC SceneDesc;
	Scene->GetDesc(&SceneDesc);
	// Port (2026-10-15): not with a multisampled scene. Measured on a GTX 1070 (native D3D9): with multisampled targets bound
	// at 1 and 2, a draw writes nothing at all to target 0 (no error returned; the caps claim MRT support), so every skin draw
	// vanished from the scene (the background showed through faces and arms) with the game's MSAA on. Without MSAA it works.
	// The skin then keeps the per-pixel diffusion (PerPixelWidth), as when ScreenSpace is off.
	// UNOFFICIAL 2026-11-04: native D3D9 only. Under NVR DXVK multisampled extra targets draw (CapturedNormals relies on the same), so the
	// multisampled skin targets made by EnsureTarget are used there.
	if (SceneDesc.MultiSampleType != D3DMULTISAMPLE_NONE && !DxvkCompute::Available()) {
		Scene->Release();
		Logger::Log("[WARNING] SkinScattering: the scene is multisampled (the game's MSAA); drawing with extra multisampled targets bound draws nothing on D3D9 drivers such as NVIDIA's, so screen-space skin scattering is off (per-pixel diffusion instead). Switch MSAA off to use it.");
		Supported = false;
		return;
	}
	if (!EnsureTarget(SceneDesc)) {
		Scene->Release();
		return;
	}

	if (!ClearedThisFrame) {
		ClearTarget(Scene);
		ClearedThisFrame = true;
	}
	Scene->Release();

	D3DVIEWPORT9 Viewport;
	Device->GetViewport(&Viewport);
	Device->GetRenderState(D3DRS_COLORWRITEENABLE1, &SavedWriteMask1);
	Device->GetRenderState(D3DRS_COLORWRITEENABLE2, &SavedWriteMask2);
	Device->SetRenderState(D3DRS_COLORWRITEENABLE1, Additive ? (D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE) : 0xF);
	// Albedo is the same in every pass: the opaque one writes it, additive ones would sum it.
	Device->SetRenderState(D3DRS_COLORWRITEENABLE2, Additive ? 0 : 0xF);
	Device->SetRenderTarget(1, MSAASurface ? MSAASurface : TextureSurface);
	Device->SetRenderTarget(2, AlbedoMSAASurface ? AlbedoMSAASurface : AlbedoTextureSurface);
	Device->SetViewport(&Viewport);

	Bound = true;
	SkinDrawn = true;
	CapturedNormals::SkinScatteringBound = true;
	CapturedNormals::NoteSkinAlbedo(AlbedoTexture, AlbedoMSAASurface, AlbedoTextureSurface);	// UNOFFICIAL 2026-10-31 AlbedoBuffer (copied in after the list)
}

// True when the pixel shader writes COLOR1 (oC1), read from its bytecode: the destination of each instruction (D3D9 shader
// token format; declarations and constant definitions carry no destination there).
static bool PixelShaderWritesColor1(const std::vector<DWORD>& Code) {
	const size_t Count = Code.size();
	if (Count < 2 || (Code[0] & 0xFFFF0000) != 0xFFFF0000 || ((Code[0] >> 8) & 0xFF) < 2) return false;   // ps_2_0 or later: instruction lengths below
	for (size_t i = 1; i < Count; ) {
		const DWORD Token = Code[i];
		const DWORD Opcode = Token & 0xFFFF;
		if (Opcode == 0xFFFF) break;                                                // end
		if (Opcode == 0xFFFE) { i += 1 + ((Token >> 16) & 0x7FFF); continue; }      // comment
		const DWORD Length = (Token >> 24) & 0x0F;                                  // tokens that follow
		const bool Declaration = Opcode == 0x1F || Opcode == 0x51 || Opcode == 0x30 || Opcode == 0x2F;   // dcl, def, defi, defb
		if (!Declaration && Length >= 1 && i + 1 < Count) {
			const DWORD Dest = Code[i + 1];
			const DWORD Type = ((Dest >> 28) & 0x7) | ((Dest >> 8) & 0x18);
			if (Type == D3DSPR_COLOROUT && (Dest & D3DSP_REGNUM_MASK) == 1) return true;
		}
		i += 1 + Length;
	}
	return false;
}

// Called by the SkinShader hook just before one of the game's own highlight passes on skin (Hooks/Shaders.cpp), while the
// blur is on. These passes add highlights on top of the lit skin, and the blur used to need them muted, or it would have
// blurred them as light (faces came out about a fifth darker with the blur: evidence\skin-resync-LOG.md run B2). NVR's
// versions of their pixel shaders write the same colour to COLOR1 (ObjectTemplate.hlsl ONLY_SPECULAR); with the skin target
// bound there (rgb only: its alpha is the skin's distance), the highlights add to the skin target's highlight share exactly as
// they add to the scene. The blur takes that share out before it blurs and puts it back after, so they stay sharp where it
// blurs and are kept as drawn wherever it leaves a pixel alone. Only the second target changes: the scene target, depth
// test, viewport and scissor stay the game's.
// True when the pass can draw: the copy is bound, or the blur cannot take these pixels anyway (another view than the world
// scene's own full-screen target). False when the blur could take them but the copy cannot be made (the game's own pixel
// shader or an older ObjectTemplate.hlsl, without COLOR1; or a pass that does not simply add): the caller mutes the pass.
bool SkinScatteringEffect::BindForGameHighlights(NiD3DPixelShaderEx* PixelShader) {
	if (HighlightsBound) return true;   // the partitions of one skinned geometry
	if (Bound) return false;            // a skin draw's targets: never expected here
	if (!Enabled || !ScreenSpace || !Supported || Effect == nullptr) return true;
	if (!InWorldScene) return true;     // first-person arms, reflections, menus: never blurred
	// ExactPassSums ([Shaders.PBR.Linear], off by default) draws a run of highlight passes into its own buffer, in linear light,
	// and folds it into the scene afterwards: not an addition the skin target could follow. Muted, as before.
	if (PassSums::Active != PassSums::None) return false;

	IDirect3DDevice9* Device = TheRenderManager->device;
	IDirect3DSurface9* Scene = nullptr;
	if (FAILED(Device->GetRenderTarget(0, &Scene)) || !Scene) return false;
	D3DSURFACE_DESC SceneDesc;
	Scene->GetDesc(&SceneDesc);
	// Not the blur's: smaller views, and a multisampled scene (the first skin draw switches the blur off then).
	if (SceneDesc.Width != TheRenderManager->width || SceneDesc.Height != TheRenderManager->height || SceneDesc.MultiSampleType != D3DMULTISAMPLE_NONE) {
		Scene->Release();
		return true;
	}

	// The pass must add its colour (destination x 1, a source factor that does not read the target): then each target
	// grows by the same amount, whatever it held.
	DWORD Blend = FALSE, Op = 0, Src = 0, Dst = 0;
	Device->GetRenderState(D3DRS_ALPHABLENDENABLE, &Blend);
	Device->GetRenderState(D3DRS_BLENDOP, &Op);
	Device->GetRenderState(D3DRS_SRCBLEND, &Src);
	Device->GetRenderState(D3DRS_DESTBLEND, &Dst);
	bool Copy = Blend && Op == D3DBLENDOP_ADD && Dst == D3DBLEND_ONE && (Src == D3DBLEND_ONE || Src == D3DBLEND_SRCALPHA);

	// NVR's pixel shader, writing COLOR1 (read once per shader; a recompiled shader may reuse an address, so its size is checked).
	IDirect3DPixelShader9* Shader = PixelShader ? PixelShader->ShaderHandle : nullptr;
	if (Copy) Copy = Shader && TheRenderManager->renderState->GetPixelShader() == Shader;
	if (Copy) {
		UINT Size = 0;
		Copy = SUCCEEDED(Shader->GetFunction(NULL, &Size)) && Size >= 2 * sizeof(DWORD);
		if (Copy) {
			auto Known = WritesColor1.find(Shader);
			if (Known == WritesColor1.end() || Known->second.first != Size) {
				std::vector<DWORD> Code(Size / sizeof(DWORD));
				const bool Writes = SUCCEEDED(Shader->GetFunction(Code.data(), &Size)) && PixelShaderWritesColor1(Code);
				Known = WritesColor1.insert_or_assign(Shader, std::make_pair(Size, Writes)).first;
				if (!Writes) Logger::Log("[WARNING] SkinScattering: the game's highlight pass on skin uses pixel shader %s, which does not write the skin target (COLOR1; ObjectTemplate.hlsl from this build?): those highlights are muted while the skin blur runs",
					PixelShader->Name ? PixelShader->Name : "?");
			}
			Copy = Known->second.second;
		}
	}
	if (!Copy || !EnsureTarget(SceneDesc)) {
		Scene->Release();
		return false;
	}
	if (!ClearedThisFrame) {   // no skin drawn yet this frame
		ClearTarget(Scene);
		ClearedThisFrame = true;
	}
	Scene->Release();

	D3DVIEWPORT9 Viewport;
	Device->GetViewport(&Viewport);
	Device->GetRenderState(D3DRS_COLORWRITEENABLE1, &SavedWriteMask1);
	Device->GetRenderState(D3DRS_COLORWRITEENABLE2, &SavedWriteMask2);
	Device->SetRenderState(D3DRS_COLORWRITEENABLE1, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
	Device->SetRenderTarget(1, MSAASurface ? MSAASurface : TextureSurface);
	Device->SetViewport(&Viewport);
	HighlightsBound = true;
	return true;
}

// Called by the SkinShader hook after the geometry is drawn, and defensively at the end of the
// world and first-person scenes: nothing else may write the skin target.
void SkinScatteringEffect::Unbind() {
	if (!Bound && !HighlightsBound) return;
	IDirect3DDevice9* Device = TheRenderManager->device;
	D3DVIEWPORT9 Viewport;
	Device->GetViewport(&Viewport);
	Device->SetRenderTarget(1, NULL);
	Device->SetRenderTarget(2, NULL);
	Device->SetViewport(&Viewport);
	Device->SetRenderState(D3DRS_COLORWRITEENABLE1, SavedWriteMask1);
	Device->SetRenderState(D3DRS_COLORWRITEENABLE2, SavedWriteMask2);
	Bound = false;
	HighlightsBound = false;
	CapturedNormals::SkinScatteringBound = false;
}

// UNOFFICIAL 2026-11-15 DeferredSkinBlur (core/DeferredSkin.h): after the deferred light passes (Render.cpp DeferredSpan), the two targets filled
// from the G-buffer for the deferred skin's pixels, as the forward skin record fills them during its draw; then the blur runs as for any skin.
void SkinScatteringEffect::FillFromDeferredSkin() {
	if (!Enabled || !ScreenSpace || !Supported || Effect == nullptr) return;
	if (!TheSettingManager->SettingsMain.Main.RenderEffects || !TheShaderManager->Shaders.Skin->Enabled || !TheShaderManager->Shaders.Skin->Active()) return;
	IDirect3DDevice9* Device = TheRenderManager->device;
	IDirect3DSurface9* Scene = nullptr;
	if (FAILED(Device->GetRenderTarget(0, &Scene)) || !Scene) return;
	D3DSURFACE_DESC SceneDesc;
	Scene->GetDesc(&SceneDesc);
	if (SceneDesc.MultiSampleType != D3DMULTISAMPLE_NONE || !EnsureTarget(SceneDesc)) { Scene->Release(); return; }
	if (!ClearedThisFrame) { ClearTarget(Scene); ClearedThisFrame = true; }
	Scene->Release();
	if (DeferredSkin::FillBlurTargets(Device, MSAASurface ? MSAASurface : TextureSurface, AlbedoMSAASurface ? AlbedoMSAASurface : AlbedoTextureSurface, PassSums::Gamma, LinearScene::Linear))
		SkinDrawn = true;
}

void DeferredSkin::AfterPasses(IDirect3DDevice9* d) {
	const int matMode = MaterialSSSProto::EffectiveMode();
	const bool unifySolidsInBurley = (matMode == 2) && BurleyWanted() && !SssFailed && SssPrep && SssBlur && !CapturedNormals::VmSpan;
	if ((DrawnThisSpan && BurleyWanted()) || unifySolidsInBurley) {
		if (!LinearScene::Linear && LinearScene::InSpan && LinearScene::SpanTarget) {
			IDirect3DSurface9* cur = nullptr;
			if (SUCCEEDED(d->GetRenderTarget(0, &cur)) && cur) {
				if (cur == LinearScene::SpanTarget) LinearScene::Convert(d, cur, true);
				cur->Release();
			}
		}
		BurleyApply(d, matMode == 2);	// (2026-11-15 SSS v2 + 2026-11-25 unified solids scatter, Shaders\DeferredSkinSSS.hlsl)
	}
	else if (DrawnThisSpan && BlurWanted() && TheShaderManager->Effects.SkinScattering) {
		TheShaderManager->Effects.SkinScattering->FillFromDeferredSkin();
	}
	if (!unifySolidsInBurley) {
		MaterialSSSProto::Apply(d);
	}
	DrawnThisSpan = false;
	DeferredLamps::SkinSpecSurface = nullptr;
}

bool SkinScatteringEffect::ShouldRender() {
	if (RtPathTrace::Replacing()) return false;
	if (!TheShaderManager->Shaders.Skin->Active()) return false;	// Vanilla Plus Skin's shaders write none of its targets
	if (!ScreenSpace || !SkinDrawn || !Texture || !TextureSurface || !AlbedoTextureSurface) return false;
	// The skin shaders' debug views: shown as drawn, not blurred. The target stays bound during
	// skin draws, so the views still show which skin the blur would take.
	if (SkinShaderDebug) return false;
	// UNOFFICIAL 2026-10-27 RenderScale (TAAUpscale.cpp): the skin targets were drawn with the world at the lower internal resolution
	// (their top-left corner) while the scene is now upscaled: their test would compare the wrong pixels. Not blurred this frame.
	// UNOFFICIAL 2026-11-18 next-111: that holds only where the world was upscaled BEFORE the effects ([Shaders.TAA.FSR] EffectsAfterUpscale,
	// not the default). In the default order (RenderRect: the effects run in the same top-left rect as the world, upscaled after them) the
	// targets and the scene share the rect's layout, and the blur runs there (SkinScattering.fx.hlsl: steps in rect coordinates, taps kept
	// inside the rect). Before, every FSR / DLSS player below RenderScale 1 (0.67 by default with FSR) had no skin scattering at all.
	if (TAAEffect::UpscaledThisFrame() && !RenderRect::Active()) return false;
	if (MSAASurface) TheRenderManager->device->StretchRect(MSAASurface, NULL, TextureSurface, NULL, D3DTEXF_NONE);
	if (AlbedoMSAASurface) TheRenderManager->device->StretchRect(AlbedoMSAASurface, NULL, AlbedoTextureSurface, NULL, D3DTEXF_NONE);
	return true;
}

// --- The Vulkan passes (NVR DXVK) -----------------------------------------------------------------------------------
// UNOFFICIAL 2026-10-25 (NVR DXVK): SkinScattering.frag, both passes of the technique, line for line. The constants the HLSL
// reads go to a uniform buffer every frame (the kernel alone is 336 bytes; push data holds 240). Everything is point
// sampled, as the effect's samplers are. Pass 1 blurs the light horizontally into the chain's current image; pass 2 reads
// that and the scene (TESR_SourceBuffer, copied once before pass 1 as Render does) and blurs vertically.
namespace {
	// The Constants block of SkinScattering.frag (std140).
	struct SkinScatterUniforms {
		D3DXVECTOR4	Data;
		D3DXVECTOR4	Kernel[SkinScatteringEffect::KernelSamples];
		D3DXVECTOR4	ReciprocalResolution;
		D3DXVECTOR4	Debug;
		D3DXVECTOR4	CameraData;
		D3DXMATRIX	Projection;
		D3DXMATRIX	View;
	};
	// Bindings as SkinScattering.frag declares them (the sampler heap is set 0, binding 0). The sampler's heap index goes to the
	// push block at offset 0, which DXVK fills in.
	const NVR_EXT_BINDING_DESC SkinScatterBindings[] = {
		{ 1, 0, NVR_EXT_BIND_SAMPLED_IMAGE,  0, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_rendered: TESR_RenderedBuffer
		{ 1, 1, NVR_EXT_BIND_SAMPLED_IMAGE,  1, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_depth: TESR_DepthBuffer
		{ 1, 2, NVR_EXT_BIND_SAMPLED_IMAGE,  2, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_scatter: TESR_SkinScatterBuffer
		{ 1, 3, NVR_EXT_BIND_SAMPLED_IMAGE,  3, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_albedo: TESR_SkinAlbedoBuffer
		{ 1, 4, NVR_EXT_BIND_SAMPLED_IMAGE,  4, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_source: TESR_SourceBuffer
		{ 2, 0, NVR_EXT_BIND_UNIFORM_BUFFER, 0, NVR_EXT_VIEW_2D, FALSE, 0 },	// Constants
		{ 0, 0, NVR_EXT_BIND_SAMPLER,        0, NVR_EXT_VIEW_2D, FALSE, 0 },	// point clamp: index at push offset 0
	};
	struct SkinScatterPush { UInt32 samplerIndex; UInt32 firstPass; UInt32 pad[2]; };
}

bool SkinScatteringEffect::ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	if (techniqueIndex != 0 || ClearRenderTarget) return false;
	IDirect3DTexture9* depth = TheShaderManager->Effects.CombineDepth ? TheShaderManager->Effects.CombineDepth->Textures.CombinedDepthTexture : nullptr;
	if (!Texture || !AlbedoTexture || !depth || !TheTextureManager->SourceTexture || !TheTextureManager->RenderedTexture) return false;

	if (!extPass.Tried) {
		extPass.Tried = true;
		extPass.Pipeline = DxvkCompute::CreateGraphicsPipeline("SkinScattering", SkinScatterBindings, _countof(SkinScatterBindings), sizeof(SkinScatterPush), true);
	}
	if (!extUniformsTried) {
		extUniformsTried = true;
		extUniforms = DxvkCompute::CreateBuffer(sizeof(SkinScatterUniforms), NVR_EXT_BUFFER_UNIFORM | NVR_EXT_BUFFER_TRANSFER_DST, "SkinScattering constants");
	}
	ID3D9NvrExtSampler* sampler = DxvkCompute::PointClamp();
	if (!extPass.Pipeline || !extUniforms || !sampler) return false;

	// The constants as the D3D9 constant table holds them this frame.
	SkinScatterUniforms uniforms = {};
	uniforms.Data = Constants.Data;
	memcpy(uniforms.Kernel, Constants.Kernel, sizeof(uniforms.Kernel));
	uniforms.ReciprocalResolution = TheShaderManager->ShaderConst.ReciprocalResolution;
	uniforms.Debug = Constants.Debug;
	uniforms.CameraData = TheRenderManager->CameraData;
	uniforms.Projection = TheRenderManager->projMatrix;
	uniforms.View = TheRenderManager->viewMatrix;
	if (FAILED(DxvkCompute::Extension()->UpdateBuffer(extUniforms, 0, sizeof(uniforms), &uniforms))) return false;

	for (UINT pass = 0; pass < 2; pass++) {
		// The current image moves on each Commit: pass 2 reads pass 1's result, with the surface the chain publishes then.
		IDirect3DBaseTexture9* inputs[5] = { TheTextureManager->RenderedTexture, depth, Texture, AlbedoTexture, TheTextureManager->SourceTexture };
		NVR_EXT_RESOURCE_BINDING resources[7] = {};
		for (UINT i = 0; i < 5; i++) {
			resources[i].Slot = i; resources[i].Type = NVR_EXT_BIND_SAMPLED_IMAGE; resources[i].pResource = inputs[i];
		}
		resources[5].Slot = 0; resources[5].Type = NVR_EXT_BIND_UNIFORM_BUFFER; resources[5].pResource = extUniforms;
		resources[6].Slot = 0; resources[6].Type = NVR_EXT_BIND_SAMPLER; resources[6].pResource = sampler;
		SkinScatterPush push = {};
		push.firstPass = pass == 0 ? 1 : 0;
		// The scene is copied into SourceBuffer by the first pass only (as Render does); pass 2 must read the scene, not pass 1's light.
		// Pass 1 is not the effect's last pass, so it never writes straight into the game target.
		IDirect3DSurface9* surface = pass == 0 ? RenderedSurface : TheTextureManager->RenderedSurface;
		if (!ExtFullScreen(Device, RenderTarget, surface, pass == 0 ? SourceBuffer : nullptr, extPass.Pipeline, resources, _countof(resources), &push, sizeof(push), pass == 1))
			return pass == 1;	// the first pass drew and committed: finishing with D3D9 would blur twice more; keep what is there
	}
	return true;
}
