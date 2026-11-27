#pragma once
#include "PassSums.h"
#include "PbrPop.h"
#include "BeatSeries.h"	// UNOFFICIAL 2026-11-11 the beat detector's per-frame series

// UNOFFICIAL 2026-10-21 FoldHighlights ([Shaders.PBR.Main] FoldHighlights: compiled in at startup, live while compiled; ObjectTemplate.hlsl
// NVR_FOLD; bench\deep-roadmap\evidence\pbrtiles\LOG.md).
//
// User report 2026-10-21 (Lucky 38 casino floor): "pbr seems to apply more to some tiles even though they're supposed to be the same type
// of tile". The game draws a shiny object reached by 0-2 lamps in ONE pass (NVR's full PBR shader: the lamps summed in linear light, metal,
// the PBR diffuse) and one reached by 3 or more in SEVERAL: a light-only pass (BSSM_AD3: the sun, the first lamps, the ambient), extra lamp
// passes (DIFFUSEPT2/3), a texture pass that MULTIPLIES what is there by the colour (TEXTURE / TEXTURE_Vc), highlight passes ADDED after
// it (2x_SPECULARDIR / PT / PT2 / PT3) and fog. With linear lighting each highlight pass is encoded on its own, so a small highlight comes
// out several times too strong (1% light -> 12% stored), and those passes have no metal and the plain diffuse: two identical floor pieces,
// one reached by 2 lamps and its neighbour by 3, look different. Measured at the user's spot (run 20261021-014725): the highlight passes'
// stored-curve sum is nearly all of the difference (fixed alone: the several-pass pieces 0.44x as bright; the lamp passes' sum: 1.00x).
//
// On: for each shiny object NVR's light-only and extra-lamp shaders draw (non-FaceGen, non-hair, non-parallax, no glow map, world scene,
// inside the highlight distance), those passes work their lamps' light out as the one-pass shader does (diffuse + highlight + metal) and
// divide it by the colour the texture pass then multiplies in, so each pass ends up holding its lamps' light exactly as one pass would;
// the object's highlight passes are then skipped (their highlights are already drawn). Per draw: NVR_SpecFade (c211) y = 1 (fold) and
// z = 1 when the game's texture pass multiplies the vertex colour too (the property's Vertex_Colors flag); for the extra-lamp passes the
// object's base texture is bound on sampler 7 (they have only the normal map; the game binds 3 for some objects -- env-mapped bottles, run 20261021-023111 -- so 7, past every stage the game gives these passes, checked per draw). What stays: the lamps of each extra-lamp pass are still
// added to the rest in the stored curve (their highlights now with them), much smaller here. Off (live): every draw exactly as before.
namespace FoldHighlights {

	enum Kind : UInt8 { KindNone = 0, KindLight, KindLamps };
	inline bool Checked = false, Compiled = false, On = false;
	inline UInt16 BindPass = 0xFFFF;		// the last shader bind (SetShadersHook): its pass enum and what NVR's pixel shader can do
	inline Kind BindKind = KindNone;
	constexpr UInt32 kBaseStage = 7;		// ObjectTemplate.hlsl FoldBaseMap (s7)
	inline IDirect3DBaseTexture9* LastBase = nullptr;	// bound for the extra-lamp pass being drawn (nullptr: none)
	inline unsigned long long BaseLost = 0, StageRefused = 0;
	inline UInt32 MaxStages = 0;	// the most texture stages the game's extra-lamp passes had (diagnostic)
	inline unsigned LostLogged = 0;
	inline const NiGeometry* LastGeometry = nullptr;	// diagnostic (AfterDraw): the extra-lamp pass being drawn
	inline UInt16 LastPass = 0;

	// Objects whose light-only pass was folded this frame (state 1), or folded but one of their extra-lamp passes could not be (2: their
	// highlight passes are drawn after all). Open addressing, tagged with the frame number (no clearing).
	// UNOFFICIAL 2026-11-16 fd-batchB (deferred gaps audit item 15, the same table as DeferredRenderer::Marked): 32,768 slots (8,192 before)
	// and the insert failures counted (SlotFailed, on its log line): an object not found keeps its highlight passes.
	constexpr unsigned kSlots = 32768;	// (a power of two: the hash masks with kSlots - 1)
	static_assert((kSlots & (kSlots - 1)) == 0, "kSlots must be a power of two");
	inline unsigned long long SlotFailed = 0;
	inline const void* Key[kSlots] = {};
	inline unsigned Frame[kSlots] = {};
	inline UInt8 State[kSlots] = {};
	inline unsigned FrameId = 1;
	inline unsigned long long Folded = 0, LampsFolded = 0, Skipped = 0, Broken = 0, Frames = 0;
	inline unsigned long long VcFlag = 0, VcPass = 0, VcMismatch = 0;

	inline UInt8* Slot(const void* geometry, bool insert) {
		unsigned h = (unsigned)(((uintptr_t)geometry >> 4) * 2654435761u) & (kSlots - 1);
		for (unsigned probe = 0; probe < 64; probe++, h = (h + 1) & (kSlots - 1)) {
			if (Frame[h] == FrameId && Key[h] == geometry) return &State[h];
			if (Frame[h] != FrameId) {
				if (!insert) return nullptr;
				Key[h] = geometry; Frame[h] = FrameId; State[h] = 0;
				return &State[h];
			}
		}
		if (insert) SlotFailed++;	// (2026-11-16) the table was full along the 64 probes
		return nullptr;
	}

	inline void BeginFrame() {
		if (!Checked) {
			Checked = true;
			Compiled = TheSettingManager->GetSettingI("Shaders.PBR.Main", "FoldHighlights") != 0;	// as ShaderRecord compiled it
			PbrPop::SpecFade::FoldCompiled = Compiled;
			Logger::Log("UNOFFICIAL FoldHighlights: %s", Compiled ? "compiled in (the highlights of shiny objects with 3+ lamps drawn with their lamps' light)" :
				"off (not compiled into the shaders)");
		}
		On = Compiled && TheSettingManager->GetSettingI("Shaders.PBR.Main", "FoldHighlights") != 0;
		FrameId++;
		if (On && ++Frames >= 600) {
			Logger::Log("UNOFFICIAL FoldHighlights: per frame %.1f light passes and %.1f extra-lamp passes folded, %.1f highlight passes skipped, %.2f objects not "
				"folded in full (highlights kept); vertex colour flag on %.1f of folded objects, texture pass with it %.1f, disagreeing %.2f; base texture replaced by the game %.2f; most texture stages of the game's extra-lamp passes %u (refused %.2f); table full (objects not noted) %llu",
				Folded / (double)Frames, LampsFolded / (double)Frames, Skipped / (double)Frames, Broken / (double)Frames,
				VcFlag / (double)Frames, VcPass / (double)Frames, VcMismatch / (double)Frames, BaseLost / (double)Frames, MaxStages, StageRefused / (double)Frames, SlotFailed);
			SlotFailed = 0;
			Folded = LampsFolded = Skipped = Broken = Frames = VcFlag = VcPass = VcMismatch = BaseLost = StageRefused = 0;
		}
	}

	// SetShadersHook, after the game bound a pass's shaders (it binds once before a run of passes of one kind).
	inline void OnBind(const char* ps, bool nvr, UInt32 passIndex) {
		BindPass = (UInt16)passIndex;
		BindKind = KindNone;
		if (!Compiled || !nvr || !ps || strncmp(ps, "SLS", 3)) return;
		switch (atoi(ps + 3)) {
		case 2037: case 2039: case 2041: case 2043: BindKind = KindLight; break;	// light-only (AD), no glow map (ObjectTemplate.hlsl NVR_FOLD)
		case 2045: case 2046: BindKind = KindLamps; break;							// extra lamp passes (DIFFUSE)
		}
	}

	// Which objects may fold: NVR's shiny lit objects that the game draws with the plain multi-pass set.
	inline bool Foldable(const NiGeometry* geometry) {
		const BSShaderProperty* property = PbrPop::LitProperty(geometry);
		if (!property) return false;
		const UInt32 flags = property->ulFlags[0];
		if (!(flags & BSShaderProperty::Specular)) return false;
		if (flags & (BSShaderProperty::FaceGen | BSShaderProperty::Parallax_Shader | BSShaderProperty::Model_Space_Normals | BSShaderProperty::Hair |
			BSShaderProperty::Landscape | BSShaderProperty::Tree_Billboard | BSShaderProperty::Parallax_Occlusion)) return false;
		return PbrPop::GameSpecularFade(property) > 0.0f;	// beyond the highlight distance the game draws no highlight passes either
	}

	// Early in the pass hooks: true = skip this pass (a highlight pass of an object whose light passes drew its highlights).
	inline bool Skip(const void* passPtr, bool world) {
		if (!On || !world || !passPtr) return false;
		const PointShadowForward::RenderPassView* pass = (const PointShadowForward::RenderPassView*)passPtr;
#if NVR_TEST_TOOLS
		if (pass->passEnum >= 0x14A && pass->passEnum <= 0x153) {	// diagnostic: does the vertex colour flag match the texture pass the game picked?
			const UInt8* state = Slot(pass->geometry, false);
			if (state && *state == 1) {
				const BSShaderProperty* property = PbrPop::LitProperty(pass->geometry);
				const char* name = Pointers::Functions::GetPassDescription(pass->passEnum);
				const bool flag = property && (property->ulFlags[1] & BSShaderProperty::Vertex_Colors);
				const bool vc = name && strstr(name, "_Vc");
				VcFlag += flag; VcPass += vc; VcMismatch += flag != vc;
			}
		}
#endif
		if (PassSums::KindOf(pass->passEnum) != PassSums::Highlights) return false;
		const UInt8* state = Slot(pass->geometry, false);
		if (!state || *state != 1) return false;
		Skipped++; BeatSeries::Add(BeatSeries::HighlightsSkipped);
		return true;
	}

	// The c211 values for this draw (SpecFade::ForDraw sends them); binds the base texture for a folded extra-lamp pass.
	inline void ForDraw(IDirect3DDevice9* device, const void* passPtr, bool world, float& fold, float& vertexColor) {
		fold = 0.0f; vertexColor = 0.0f; LastBase = nullptr;
		if (!On || !world || !passPtr) return;
		const PointShadowForward::RenderPassView* pass = (const PointShadowForward::RenderPassView*)passPtr;
		if (pass->passEnum != BindPass || BindKind == KindNone) {
			if (PassSums::KindOf(pass->passEnum) == PassSums::Lamps) {	// an extra lamp pass of a folded object drawn without NVR's shader
				UInt8* state = Slot(pass->geometry, false);
				if (state && *state == 1) { *state = 2; Broken++; }
			}
			return;
		}
		NiGeometry* geometry = pass->geometry;
		if (!Foldable(geometry)) return;
		const BSShaderPPLightingProperty* property = (const BSShaderPPLightingProperty*)PbrPop::LitProperty(geometry);
		if (BindKind == KindLight) {
			UInt8* state = Slot(geometry, true);
			if (!state) return;	// table full this frame: drawn as before
			*state = 1;
			Folded++; BeatSeries::Add(BeatSeries::PassesFolded);
		}
		else {
			UInt8* state = Slot(geometry, false);
			if (!state || *state != 1) return;	// its light pass was not folded: as before
			const NiSourceTexture* base = property->ppTextures[0] ? property->ppTextures[0][0] : nullptr;
			IDirect3DBaseTexture9* texture = base && base->rendererData ? base->rendererData->dTexture : nullptr;
			const NiD3DPass* d3dPass = *(NiD3DPass**)0x0126F74C;	// the pass the game bound (SetShadersHook reads the same)
			const UInt32 stages = d3dPass ? d3dPass->StageCount : 0;
			if (stages > MaxStages) MaxStages = stages;
			if (!texture || stages > kBaseStage) { if (stages > kBaseStage) StageRefused++; *state = 2; Broken++; return; }
			device->SetTexture(kBaseStage, texture);
			TheRenderManager->renderState->NoteDeviceTexture(kBaseStage, texture);
			LastBase = texture; LastGeometry = geometry; LastPass = pass->passEnum;
			static const D3DSAMPLERSTATETYPE kStates[] = { D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER,
				D3DSAMP_MAXANISOTROPY, D3DSAMP_MIPMAPLODBIAS };
			for (D3DSAMPLERSTATETYPE type : kStates) {	// as the normal map on sampler 0 (the texture pass reads the base texture alike)
				DWORD value = 0;
				device->GetSamplerState(0, type, &value);
				TheRenderManager->SetSamplerState(kBaseStage, type, value);
			}
			LampsFolded++; BeatSeries::Add(BeatSeries::PassesFolded);
		}
		fold = 1.0f;
		vertexColor = (property->ulFlags[1] & BSShaderProperty::Vertex_Colors) ? 1.0f : 0.0f;
	}

#if NVR_TEST_TOOLS
	// After the draw (diagnostic): the base texture was still bound when the game drew the extra-lamp pass (it did not set that sampler itself).
	inline void AfterDraw(IDirect3DDevice9* device) {
		if (!LastBase) return;
		IDirect3DBaseTexture9* now = nullptr;
		if (SUCCEEDED(device->GetTexture(kBaseStage, &now)) && now) now->Release();
		if (now != LastBase) {
			BaseLost++;
			if (LostLogged < 24 && LastGeometry) {
				LostLogged++;
				const BSShaderPPLightingProperty* property = (const BSShaderPPLightingProperty*)PbrPop::LitProperty(LastGeometry);
				int which = -1;	// which of the property's own textures the game put there, if any
				for (int i = 0; i < 6 && property; i++) {
					const NiSourceTexture* t = property->ppTextures[i] ? property->ppTextures[i][0] : nullptr;
					if (t && t->rendererData && t->rendererData->dTexture == now) which = i;
				}
				const NiAVObject* parent = (const NiAVObject*)LastGeometry->m_parent;
				Logger::Log("FOLD BASE LOST '%s' (in '%s') pass %03X %s flags %08X %08X: sampler %u now %p (property texture %d)", LastGeometry->m_pcName ? LastGeometry->m_pcName : "",
					parent && parent->m_pcName ? parent->m_pcName : "", (unsigned)LastPass, Pointers::Functions::GetPassDescription(LastPass),
					property ? property->ulFlags[0] : 0u, property ? property->ulFlags[1] : 0u, kBaseStage, now, which);
			}
		}
		LastBase = nullptr;
	}
#endif
}
