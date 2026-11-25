#include <algorithm>
#include <unordered_set>
#include "CapturedNormals.h"	// UNOFFICIAL 2026-10-28 CapturedNormals
#include "ObjectMotion.h"	// UNOFFICIAL 2026-10-29 ObjectMotion
#include "ViewModelMotion.h"	// UNOFFICIAL 2026-11-21 ViewModelMotion
#include "SkyCoverage.h"	// UNOFFICIAL 2026-11-04
#include "BackDepth.h"	// UNOFFICIAL 2026-10-31 BackDepth
#include "RenderRect.h"	// UNOFFICIAL 2026-11-12 (memleak) idle targets: ForgetSurface
#include "DeferredDepth.h"	// UNOFFICIAL 2026-11-14 DeferredDepth

namespace {
	// A depth-stencil texture the shaders can sample (the INTZ format), screen sized.
	HRESULT MakeReadableDepth(IDirect3DDevice9* device, UInt32 w, UInt32 h, IDirect3DTexture9** out) {
		return device->CreateTexture(w, h, 1, D3DUSAGE_DEPTHSTENCIL, (D3DFORMAT)MAKEFOURCC('I', 'N', 'T', 'Z'), D3DPOOL_DEFAULT, out, NULL);
	}

	// Points every texture record in the list at the given texture.
	void Rebind(WaterMapList& records, IDirect3DBaseTexture9* texture) {
		for (TextureRecord* record : records) record->Texture = texture;
	}
}

void TextureManager::Initialize() {
	Logger::Log("Starting the textures manager...");
	TimeLogger startup = TimeLogger();

	TextureManager* mgr = new TextureManager();
	TheTextureManager = mgr;

	IDirect3DDevice9* dev = TheRenderManager->device;
	const UInt32 screenW = TheRenderManager->width;
	const UInt32 screenH = TheRenderManager->height;

	// NVR's own colour targets (each comes with a surface so it can be rendered to)
	mgr->InitTexture("TESR_SourceBuffer", &mgr->SourceTexture, &mgr->SourceSurface, screenW, screenH, D3DFMT_A16B16G16R16F);
	mgr->InitTexture("TESR_RenderedBuffer", &mgr->RenderedTexture, &mgr->RenderedSurface, screenW, screenH, D3DFMT_A16B16G16R16F);

	// readable copies of the world's and of the first-person model's depth
	MakeReadableDepth(dev, screenW, screenH, &mgr->DepthTexture);
	mgr->RegisterTexture("TESR_DepthBufferWorld", (IDirect3DBaseTexture9**)&mgr->DepthTexture);
	MakeReadableDepth(dev, screenW, screenH, &mgr->DepthTextureViewModel);
	mgr->RegisterTexture("TESR_DepthBufferViewModel", (IDirect3DBaseTexture9**)&mgr->DepthTextureViewModel);
	// UNOFFICIAL 2026-10-28 CapturedNormals (core/CapturedNormals.h): made on first use (empty until then: readers keep the depth-rebuilt normal)
	mgr->RegisterTexture("TESR_CapturedNormalsBuffer", (IDirect3DBaseTexture9**)&CapturedNormals::Texture);
	mgr->RegisterTexture("TESR_MaterialBuffer", (IDirect3DBaseTexture9**)&CapturedNormals::MaterialTexture);
	mgr->RegisterTexture("TESR_AlbedoBuffer", (IDirect3DBaseTexture9**)&CapturedNormals::AlbedoTexture);	// UNOFFICIAL 2026-10-31 AlbedoBuffer (made on first use)
	mgr->RegisterTexture("TESR_BackDepthBuffer", (IDirect3DBaseTexture9**)&BackDepth::Texture);	// UNOFFICIAL 2026-10-31 BackDepth	// UNOFFICIAL 2026-10-29 MaterialBuffer
	mgr->RegisterTexture("TESR_ObjectMotionBuffer", (IDirect3DBaseTexture9**)&ObjectMotion::Texture);	// UNOFFICIAL 2026-10-29 ObjectMotion (made on first use)
	mgr->RegisterTexture("TESR_ViewModelMotionBuffer", (IDirect3DBaseTexture9**)&ViewModelMotion::Texture);	// UNOFFICIAL 2026-11-21 ViewModelMotion (made on first use)
	mgr->RegisterTexture("TESR_SkyCoverage", (IDirect3DBaseTexture9**)&SkyCoverage::Texture);	// UNOFFICIAL 2026-11-04 SkyCoverage (the game's MSAA; made on first use)

	// let NVAPI see the two depth textures (for the depth copies)
	NvAPI_D3D9_RegisterResource(mgr->DepthTexture);
	NvAPI_D3D9_RegisterResource(mgr->DepthTextureViewModel);

	// UNOFFICIAL terrain blending ([Shaders.Terrain.Blending]): its depth copy exists only when the feature was on at startup,
	// which is also when the land shaders are compiled with it (TERRAIN_BLEND, ShaderRecord::LoadShader).
	// UNOFFICIAL 2026-10-27: also with [Main.Main.Performance] NvrDxvkDisplacement on at startup: displacement's ObjectBlend reads the
	// same copy (TESR_DisplaceDepth, the land pixel shaders' s6 for their tessellation shaders; NVR_DISPLACE_DEPTH).
	if (TheSettingManager->GetSettingI("Shaders.Terrain.Blending", "Enabled") || TheSettingManager->GetSettingI("Main.Main.Performance", "NvrDxvkDisplacement")) {
		if (SUCCEEDED(MakeReadableDepth(dev, screenW, screenH, &mgr->TerrainBlendDepthTexture))) {
			mgr->RegisterTexture("TESR_TerrainBlendDepth", (IDirect3DBaseTexture9**)&mgr->TerrainBlendDepthTexture);
			mgr->RegisterTexture("TESR_DisplaceDepth", (IDirect3DBaseTexture9**)&mgr->TerrainBlendDepthTexture);
			NvAPI_D3D9_RegisterResource(mgr->TerrainBlendDepthTexture);
		}
		else {
			mgr->TerrainBlendDepthTexture = nullptr;
			Logger::Log("[ERROR] Terrain blending: could not create its depth texture; blending (and displacement's ObjectBlend) stays off");
		}
	}

	// UNOFFICIAL 2026-11-14 DeferredDepth (core/DeferredDepth.h; [Main.Main.Quality] DeferredDepth / DeferredDepthProbe at startup): the world's
	// depth as the main opaque list leaves it, for the deferred light passes. Not made (and never copied) with both off.
	if (DeferredDepth::ReadStartup() > 0) {
		const bool shareWorld = !DeferredDepth::ProbeWanted && mgr->DepthTexture;
		if (shareWorld || SUCCEEDED(MakeReadableDepth(dev, screenW, screenH, &DeferredDepth::Texture))) {
			if (shareWorld) DeferredDepth::Texture = mgr->DepthTexture;
			else NvAPI_D3D9_RegisterResource(DeferredDepth::Texture);
			DeferredDepth::SharedWithWorld = shareWorld;
			DeferredDepth::Width = screenW; DeferredDepth::Height = screenH;
			mgr->RegisterTexture("TESR_DeferredDepth", (IDirect3DBaseTexture9**)&DeferredDepth::Texture);
			// mode 2: the view z made from it (R32F), which D3D9 passes read as any float texture
			if (DeferredDepth::Mode >= 2) mgr->InitTexture("TESR_DeferredDepthLinear", &DeferredDepth::LinearTexture, &DeferredDepth::LinearSurface, screenW, screenH, D3DFMT_R32F);
			Logger::Log("UNOFFICIAL DeferredDepth: mode %d, the depth copy for the deferred passes is on (%ux%u INTZ, TESR_DeferredDepth%s)%s%s", DeferredDepth::Mode, screenW, screenH,
				shareWorld ? ", shared with TESR_DepthBufferWorld" : "",
				DeferredDepth::LinearSurface ? " + its view z (R32F, TESR_DeferredDepthLinear)" : "", DeferredDepth::ProbeWanted ? "; the probe is on (test only)" : "");
		}
		else {
			DeferredDepth::Texture = nullptr;
			Logger::Log("[ERROR] UNOFFICIAL DeferredDepth: could not create its INTZ depth texture: no depth copy");
		}
	}

	// UNOFFICIAL 2026-11-04 soft particles (Particles.h): with lit particles on at startup, as their vertices carry the size (ParticleSize.h).
	if (TheSettingManager->GetSettingI("Shaders.Particles.Status", "Enabled")) {
		if (SUCCEEDED(MakeReadableDepth(dev, screenW, screenH, &mgr->ParticleDepthTexture))) {
			mgr->RegisterTexture("TESR_ParticleDepth", (IDirect3DBaseTexture9**)&mgr->ParticleDepthTexture);
			NvAPI_D3D9_RegisterResource(mgr->ParticleDepthTexture);
		}
		else {
			mgr->ParticleDepthTexture = nullptr;
			Logger::Log("[ERROR] Soft particles: could not create their depth texture; they stay off");
		}
	}

	// the water maps are slots the water hooks fill in later
	mgr->RegisterTexture(WordWaterHeightMapBuffer, &mgr->WaterHeightMapB);
	mgr->RegisterTexture(WordWaterReflectionMapBuffer, &mgr->WaterReflectionMapB);

	startup.LogTime("TextureManager::Initialize");
}

// Makes a render-target texture (optionally with an auto-generated mip chain), hands back its top surface and registers it
// under the given sampler name.
void TextureManager::InitTexture(const char* samplerName, IDirect3DTexture9** target, IDirect3DSurface9** topSurface, int w, int h, D3DFORMAT fmt, bool mipmaps) {
	const UINT levels = mipmaps ? 0 : 1;
	const DWORD usage = mipmaps ? (D3DUSAGE_RENDERTARGET | D3DUSAGE_AUTOGENMIPMAP) : D3DUSAGE_RENDERTARGET;
	// UNOFFICIAL 2026-11-12 (memleak) idle targets (TextureManager.h): the target of an effect that is off is not made at its size at
	// all; its slot gets the 1 x 1 stand-in, and TargetsFrame makes the real one when the effect is switched on.
	if (Targets.empty() && EffectSpans.empty()) ReadTargetSettings();	// (start-up: before the first frame's TargetsFrame)
	EffectSpan* span = SpanOf(target);
	EffectRecord* owner = span && span->Effect->TargetParkable(samplerName) ? span->Effect : nullptr;
	const bool parked = owner && TargetReleaseSeconds > 0.0f && !OwnerWanted(owner);
	const HRESULT result = TheRenderManager->device->CreateTexture(parked ? 1 : w, parked ? 1 : h, levels, usage, fmt, D3DPOOL_DEFAULT, target, NULL);
	if (FAILED(result)) {
		Logger::Log("[ERROR] : Failed to init texture %s", samplerName);
		EffectStatus::D3DFail("CreateTexture (TextureManager::InitTexture)", result);	// UNOFFICIAL 2026-11-01
		return;
	}

	EffectStatus::TargetSize(samplerName, (unsigned)w, (unsigned)h);	// UNOFFICIAL 2026-11-01: logged when made at another size than before
	(*target)->GetSurfaceLevel(0, topSurface);
	RegisterTexture(samplerName, reinterpret_cast<IDirect3DBaseTexture9**>(target));

	// (memleak) noted, by its slot: a slot made again (another size) replaces its old note
	if (parked && *topSurface) TheRenderManager->device->ColorFill(*topSurface, NULL, 0);
	OwnedTarget note = { samplerName, target, topSurface, w, h, fmt, mipmaps, owner, parked };
	const unsigned long long bytes = TargetBytes(w, h, fmt, mipmaps);
	bool known = false;
	for (OwnedTarget& t : Targets) if (t.Texture == target) {
		if (t.Parked) TargetBytesParked -= (std::min)(TargetBytesParked, TargetBytes(t.Width, t.Height, t.Format, t.Mipmaps));
		t = note; known = true; break;
	}
	if (!known) Targets.push_back(note);
	if (parked) TargetBytesParked += bytes;
	if (TargetInventory)
		Logger::Log("TARGETS inventory: %s %d x %d format %u%s, %.2f MB, %s%s%s", samplerName, w, h, (unsigned)fmt, mipmaps ? " + mips" : "", bytes / 1048576.0,
			span ? span->Effect->Name : "no effect's (a core buffer)", !span ? "" : owner ? " (follows the effect's switch)" : " (kept whatever the switch)",
			parked ? ": NOT made, the effect is off (a 1 x 1 stand-in)" : "");
}

// ---- UNOFFICIAL 2026-11-12 (memleak) idle targets (TextureManager.h) -------------------------------------------------------------------

// A render target's size in bytes (the formats InitTexture's callers use; an autogen mip chain adds a third).
unsigned long long TextureManager::TargetBytes(int Width, int Height, D3DFORMAT Format, bool Mipmaps) {
	unsigned long long texel = 4;
	switch ((DWORD)Format) {
	case D3DFMT_A32B32G32R32F: texel = 16; break;
	case D3DFMT_A16B16G16R16F: case D3DFMT_A16B16G16R16: case D3DFMT_G32R32F: texel = 8; break;
	case D3DFMT_R16F: case D3DFMT_L16: case D3DFMT_R5G6B5: case D3DFMT_A8L8: texel = 2; break;
	case D3DFMT_A8: case D3DFMT_L8: texel = 1; break;
	default: break;	// the 32-bit formats: A8R8G8B8, X8R8G8B8, R32F, G16R16, G16R16F, A2R10G10B10 ...
	}
	const unsigned long long top = (unsigned long long)(std::max)(Width, 1) * (unsigned long long)(std::max)(Height, 1) * texel;
	return Mipmaps ? top + top / 3 : top;
}

void TextureManager::ReadTargetSettings() {
	SettingManager::Configuration::ConfigNode node;
	// (a build whose defaults lack the keys: 20 s, no inventory)
	TargetReleaseSeconds = TheSettingManager->Config.FillNode(&node, "Main.Main.Performance", "TargetReleaseSeconds") ?
		std::clamp(TheSettingManager->GetSettingF("Main.Main.Performance", "TargetReleaseSeconds"), 0.0f, 3600.0f) : 20.0f;
	TargetInventory = TheSettingManager->Config.FillNode(&node, "Main.Develop.Main", "TargetInventory") && node.BoolValue;
}

// ShaderManager::RegisterEffect: the effect's object (its targets' slots lie inside it) and its switch in the settings.
void TextureManager::NoteEffect(EffectRecord* Effect, size_t Bytes, bool WantedAtStart) {
	if (EffectSpans.empty() && Targets.empty()) ReadTargetSettings();
	EffectSpans.push_back({ Effect, (const char*)Effect, (const char*)Effect + Bytes, 0ull });
	StartingEffect = Effect;
	StartingWanted = WantedAtStart;
}

TextureManager::EffectSpan* TextureManager::SpanOf(const void* Slot) {
	for (EffectSpan& span : EffectSpans) if ((const char*)Slot >= span.Begin && (const char*)Slot < span.End) return &span;
	return nullptr;
}

bool TextureManager::OwnerWanted(EffectRecord* Owner) {
	if (Owner == StartingEffect) return StartingWanted;	// start-up: Enabled is set by LoadEffect, after RegisterTextures
	return Owner->TargetsWanted();
}

// Nothing may keep the old texture's address past the swap (this code base has crashed that way: a game shader kept a texture the
// plugin had released). Where a raw pointer to a TESR_ / NVR_ target can sit, and what is done about each:
// (1) the game shaders' sampler records (ShaderRecord::SetCT binds its cached pointer until it is null): ClearShaderSamplers, so each
//     reads the slot again at its next bind. The effects' own samplers read the slot at every bind (EffectRecord::SetCT, Sm6Pass).
// (2) the device's texture stages, pixel and vertex, and the game's note of each pixel stage (NiDX9RenderState: with a dead address
//     in it the game would skip its own bind of a new texture that got the same address): taken off, both.
// (3) the spare RenderRect::RescaleAfterUpscale keeps per surface: dropped with it (ShaderManager.cpp RenderRect::ForgetSurface).
// Not covered, and why an effect must say so itself (EffectRecord::TargetParkable): a pointer an effect copied into a member or a
// static, or a pass binding kept across frames.
void TextureManager::ForgetTarget(const std::string& Name, IDirect3DBaseTexture9* Texture, IDirect3DSurface9* Surface) {
	IDirect3DDevice9* device = TheRenderManager->device;
	TheShaderManager->ClearShaderSamplers(Name.c_str(), Name.size() + 1);	// (+ 1: the whole name, not names that start with it)
	NiDX9RenderState* state = TheRenderManager->renderState;
	for (DWORD stage = 0; stage < 16; stage++) {
		IDirect3DBaseTexture9* bound = nullptr;
		const bool known = SUCCEEDED(device->GetTexture(stage, &bound));
		if (bound) bound->Release();
		if (!(known && bound == Texture) && state->unk10A0[stage] != (UInt32)Texture) continue;
		device->SetTexture(stage, nullptr);
		state->NoteDeviceTexture(stage, nullptr);
	}
	for (DWORD stage = D3DVERTEXTEXTURESAMPLER0; stage <= D3DVERTEXTEXTURESAMPLER3; stage++) {
		IDirect3DBaseTexture9* bound = nullptr;
		if (SUCCEEDED(device->GetTexture(stage, &bound)) && bound) {
			bound->Release();
			if (bound == Texture) device->SetTexture(stage, nullptr);
		}
	}
	RenderRect::ForgetSurface(Surface);
}

// Park: the real target leaves, a 1 x 1 stand-in takes its slot. Unpark: the other way. 1: swapped; 0: the new texture could not be
// made (left as it was, tried again later); -1: the slot no longer holds what was noted (left alone for good).
int TextureManager::SwapTarget(OwnedTarget& Target, bool Park) {
	IDirect3DDevice9* device = TheRenderManager->device;
	IDirect3DTexture9* old = *Target.Texture;
	IDirect3DSurface9* oldSurface = *Target.Surface;
	if (!old || !oldSurface) return -1;	// the effect gave it up itself (a failed creation): nothing to swap
	{	// only a target that is still the one noted (an effect may have made another by its own means): the real size, or the stand-in
		D3DSURFACE_DESC desc = {};
		const int w = Park ? Target.Width : 1, h = Park ? Target.Height : 1;
		if (FAILED(old->GetLevelDesc(0, &desc)) || (int)desc.Width != w || (int)desc.Height != h || desc.Format != Target.Format) {
			if (!Park) TargetBytesParked -= (std::min)(TargetBytesParked, TargetBytes(Target.Width, Target.Height, Target.Format, Target.Mipmaps));
			Logger::Log("TARGETS: %s is not the buffer that was noted (%u x %u, noted %d x %d): left alone from now on", Target.Name.c_str(), desc.Width, desc.Height,
				Target.Width, Target.Height);
			Target.Owner = nullptr;
			Target.Parked = false;
			return -1;
		}
	}
	const UINT levels = Target.Mipmaps ? 0 : 1;
	const DWORD usage = Target.Mipmaps ? (D3DUSAGE_RENDERTARGET | D3DUSAGE_AUTOGENMIPMAP) : D3DUSAGE_RENDERTARGET;
	IDirect3DTexture9* made = nullptr;
	IDirect3DSurface9* madeSurface = nullptr;
	if (FAILED(device->CreateTexture(Park ? 1 : Target.Width, Park ? 1 : Target.Height, levels, usage, Target.Format, D3DPOOL_DEFAULT, &made, NULL)) || !made ||
		FAILED(made->GetSurfaceLevel(0, &madeSurface)) || !madeSurface) {
		if (made) made->Release();
		return 0;
	}
	device->ColorFill(madeSurface, NULL, 0);	// a new target's contents are not defined on every driver: zero, as a never-drawn one reads
	ForgetTarget(Target.Name, old, oldSurface);
	*Target.Texture = made;
	*Target.Surface = madeSurface;
	oldSurface->Release();
	old->Release();
	Target.Parked = Park;
	const unsigned long long bytes = TargetBytes(Target.Width, Target.Height, Target.Format, Target.Mipmaps);
	if (Park) TargetBytesParked += bytes; else TargetBytesParked -= (std::min)(TargetBytesParked, bytes);
	return 1;
}

// Once a frame before anything draws (ShaderManager::UpdateConstants), and again at the start of each effect chain (an effect
// switched on in between draws into its real targets the same frame). Reads each effect's switch where it has its real value (the
// chain switches AmbientOcclusion / BounceLight off for the length of a call while AmbientLight replaces them, and back).
void TextureManager::TargetsFrame() {
	if (Targets.empty()) return;
	const unsigned long long now = GetTickCount64();
	static unsigned long long settingsAt = 0;
	if (now - settingsAt >= 1000) { settingsAt = now; ReadTargetSettings(); }
	StartingEffect = nullptr;
	for (EffectSpan& span : EffectSpans) {
		if (span.Effect->TargetsWanted()) span.OffSince = 0;
		else if (!span.OffSince) span.OffSince = now;
	}
	static bool toldStart = false;
	if (!toldStart) {	// what start-up made, once
		toldStart = true;
		unsigned long long all = 0, follow = 0;
		unsigned followCount = 0, parkedCount = 0;
		std::string off;
		for (const OwnedTarget& t : Targets) {
			const unsigned long long bytes = TargetBytes(t.Width, t.Height, t.Format, t.Mipmaps);
			all += bytes;
			if (t.Owner) { follow += bytes; followCount++; }
			if (t.Parked) { parkedCount++; if (off.find(t.Owner->Name) == std::string::npos) off += (off.empty() ? "" : ", ") + std::string(t.Owner->Name); }
		}
		Logger::Log("TARGETS at start: %u effect buffers noted, %.0f MB at their sizes; %u of them (%.0f MB) follow their effect's switch; %u (%.0f MB) not made, "
			"their effect is off: %s ([Main.Main.Performance] TargetReleaseSeconds %.0f; 0 = every buffer made at start and kept, as before)", (unsigned)Targets.size(),
			all / 1048576.0, followCount, follow / 1048576.0, parkedCount, TargetBytesParked / 1048576.0, off.empty() ? "none" : off.c_str(), TargetReleaseSeconds);
	}
	if (now < TargetRetryAt) return;
	const unsigned long long grace = (unsigned long long)(TargetReleaseSeconds * 1000.0f);
	EffectRecord* told = nullptr;	// one line an effect and frame
	for (size_t i = 0; i < Targets.size(); i++) {
		OwnedTarget& t = Targets[i];
		if (!t.Owner || !*t.Texture || !*t.Surface) continue;
		EffectSpan* span = SpanOf(t.Texture);
		if (!span) continue;
		const bool wanted = span->OffSince == 0;
		const bool unpark = t.Parked && (wanted || TargetReleaseSeconds <= 0.0f);
		const bool park = !t.Parked && !wanted && TargetReleaseSeconds > 0.0f && now - span->OffSince >= grace;
		if ((!unpark && !park) || t.Owner == told) continue;
		// every target of this effect in the same state, in one go (one log line, one time)
		told = t.Owner;
		LARGE_INTEGER t0, t1, freq;
		QueryPerformanceCounter(&t0);
		unsigned count = 0, failed = 0;
		unsigned long long bytes = 0;
		EffectRecord* const owner = t.Owner;	// (a swap may take the note's owner away)
		const bool wasParked = t.Parked;
		for (size_t k = i; k < Targets.size(); k++) {
			OwnedTarget& u = Targets[k];
			if (u.Owner != owner || u.Parked != wasParked || !*u.Texture || !*u.Surface) continue;
			const int swapped = SwapTarget(u, park);
			if (swapped > 0) { count++; bytes += TargetBytes(u.Width, u.Height, u.Format, u.Mipmaps); }
			else if (swapped == 0) failed++;
		}
		QueryPerformanceCounter(&t1);
		QueryPerformanceFrequency(&freq);
		const double ms = 1000.0 * (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
		if (failed) TargetRetryAt = now + 5000;	// (video memory short, most likely: not every frame)
		if (!count && !failed) continue;
		if (park) Logger::Log("TARGETS: %s has been off for %.0f s: %u of its buffers released, %.1f MB given back (%.2f ms%s); %.0f MB of buffers released in all now",
			owner->Name, (now - span->OffSince) / 1000.0, count, bytes / 1048576.0, ms, failed ? "; some could not be swapped and stay" : "", TargetBytesParked / 1048576.0);
		else Logger::Log("TARGETS: %s is on again: %u buffers made again, %.1f MB, in %.2f ms%s", owner->Name, count, bytes / 1048576.0, ms,
			failed ? " ([ERROR] some could not be made: the effect draws into 1 x 1 stand-ins until they can; tried again in 5 s)" : "");
	}
}

// Makes a texture slot known to shaders under a sampler name (TESR_...).
void TextureManager::RegisterTexture(const char* samplerName, IDirect3DBaseTexture9** slot) {
	Logger::Log("Registering Texture %s", samplerName);
	TextureNames[samplerName] = slot;
}

// A texture file loaded before, by its path (nullptr if it never was).
IDirect3DBaseTexture9* TextureManager::GetCachedTexture(std::string& path) {
	auto found = TextureCache.find(path);
	return found != TextureCache.end() ? found->second : nullptr;
}


/*
* A sampler whose texture file failed to load asks for it by name again at every bind, so the error is reported once
* per name: a tester's log had 53,000 identical lines (four per frame), each written and flushed on the render thread.
*/
static void ReportMissingTexture(const std::string& Name) {
	static std::unordered_set<std::string> reported;
	if (reported.insert(Name).second)
		Logger::Log("[ERROR] Texture %s not found (reported once; the shaders using it run without it).", Name.c_str());
}

// The slot of a named texture, so a caller can follow a slot whose texture is swapped later.
IDirect3DBaseTexture9** TextureManager::GetTextureSlotByName(std::string& Name) {
	auto found = TextureNames.find(Name);
	if (found != TextureNames.end()) return found->second;
	ReportMissingTexture(Name);
	return nullptr;
}

// The texture a named slot holds right now.
IDirect3DBaseTexture9* TextureManager::GetTextureByName(std::string& samplerName) {
	IDirect3DBaseTexture9** slot = GetTextureSlotByName(samplerName);
	return slot ? *slot : nullptr;
}


// Loads a texture file of the given kind once; later calls (also after a failure) get the cached result.
IDirect3DBaseTexture9* TextureManager::GetFileTexture(std::string TexturePath, TextureRecord::TextureRecordType Kind) {
	IDirect3DBaseTexture9* loaded = GetCachedTexture(TexturePath);
	if (loaded) return loaded;

	IDirect3DDevice9* dev = TheRenderManager->device;
	const char* file = TexturePath.data();
	if (Kind == TextureRecord::TextureRecordType::PlanarBuffer)
		D3DXCreateTextureFromFileA(dev, file, reinterpret_cast<IDirect3DTexture9**>(&loaded));
	else if (Kind == TextureRecord::TextureRecordType::VolumeBuffer)
		D3DXCreateVolumeTextureFromFileA(dev, file, reinterpret_cast<IDirect3DVolumeTexture9**>(&loaded));
	else if (Kind == TextureRecord::TextureRecordType::CubeBuffer)
		D3DXCreateCubeTextureFromFileA(dev, file, reinterpret_cast<IDirect3DCubeTexture9**>(&loaded));
	else
		Logger::Log("[ERROR] : Invalid texture type %i for %s", Kind, TexturePath.c_str());

	if (!loaded) {
		// UNOFFICIAL 2026-10-30 (tester log, P84 v4: "NVR's texture files are missing or incomplete" with every NVR texture loaded): the
		// hint below came with ANY file that failed, and the one that failed was BloodFlow's optional loose replacer of a vanilla decal
		// (Data\Textures\decals\fxbloodsplatterdecals01.dds; the game's own is inside its BSA, which this loader does not read). Only
		// NVR's own folders get the error and the hint now; another path is a plain line (the effect that names it does without).
		std::string lower = TexturePath;
		for (char& c : lower) c = c == '/' ? '\\' : (char)tolower((unsigned char)c);
		const bool nvrOwn = lower.find("textures\\effects\\") != std::string::npos || lower.find("textures\\water\\") != std::string::npos ||
			lower.find("textures\\precipitations\\") != std::string::npos || lower.find("textures\\newvegasreloaded\\") != std::string::npos;
		if (!nvrOwn) Logger::Log("Texture file %s is not there as a loose file (optional; the effect that names it does without)", TexturePath.c_str());
		else {
			Logger::Log("[ERROR] : Couldn't load texture file %s", TexturePath.c_str());
			// NVR's own textures (Effects, Water, Precipitations) ship with the mod; when they are missing, effects still
			// run but read black instead (ambient occlusion loses its noise pattern and bands, SMAA stops smoothing).
			static bool hinted = false;
			if (!hinted) {
				hinted = true;
				Logger::Log("[ERROR] : Vegas Shaders' texture files are missing or incomplete (Data\\Textures\\Effects, Water, Precipitations). "
					"Effects that use them will look wrong. Install the complete Vegas Shaders package, including its Textures folder.");
			}
		}
	}
	else Logger::Log("Loaded texture file %s", TexturePath.c_str());

	TextureCache[TexturePath] = loaded;	// a failed load is cached too (as nullptr)
	return loaded;
}


// The game hands over its current water height map; every record bound to it follows. (Kept as a raw pointer: it may
// crash if the game frees the map while a record still points at it.)
void TextureManager::SetWaterHeightMap(IDirect3DBaseTexture9* HeightMap) {
	if (HeightMap == WaterHeightMapB) return;
	WaterHeightMapB = HeightMap;
	Rebind(TheTextureManager->WaterHeightMapTextures, HeightMap);
}

// Same for the water reflection map.
void TextureManager::SetWaterReflectionMap(IDirect3DBaseTexture9* ReflectionMap) {
	if (ReflectionMap == WaterReflectionMapB) return;
	WaterReflectionMapB = ReflectionMap;
	Rebind(TheTextureManager->WaterReflectionMapTextures, ReflectionMap);
}


// Debug: writes a texture's top level to .\Test\<name>.jpg.
void TextureManager::DumpToFile(IDirect3DTexture9* Source, const char* FileName) {
	IDirect3DSurface9* top = nullptr;
	Source->GetSurfaceLevel(0, &top);
	if (!top) {
		Logger::Log("Surface is null for %s", FileName);
		return;
	}
	std::string target = std::string(".\\Test\\") + FileName + ".jpg";
	D3DXSaveSurfaceToFileA(target.c_str(), D3DXIFF_JPG, top, NULL, NULL);
	top->Release();
}
