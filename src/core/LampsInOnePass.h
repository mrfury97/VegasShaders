#pragma once
#include <string>
#include "TestTools.h"
#include "ManyLamps.h"	// UNOFFICIAL 2026-10-27 ManyLampsOnePass: the compare goes to 12 + f lamps
#include "DeferredRenderer.h"	// UNOFFICIAL 2026-11-14 full deferred renderer: one pass per object while it is live indoors

// UNOFFICIAL 2026-10-18 LampsInOnePass ([Main.Main.Misc] LampsInOnePass, live, default off).
//
// How the game picks an object's lighting passes (read in the Ghidra pseudo-code of FalloutNV.exe, function names from
// WallSoGB's IDA map of the game -- credit WallSoGB; bench\deep-roadmap\evidence\wave-1018b\f02-lightpasses.md, lightcap-LOG.md):
// BSShaderPPLightingProperty::GetRenderPasses_2x (0x00BDF790) gives an opaque lit object ONE many-lamp pass
// (AddPass_Opt 0x00BDE9B0: the main light and the lamps in one shader, summed in the shader in linear light) only while its
// lamps -- BSShaderLightingProperty::GetNumberOfActiveNonShadowLights (0x00B707D0) -- are at most 5 + f (plain), 4 + f
// (FaceGen) or 2 + f (SPECULAR: anything with a specular map, most lit surfaces indoors); f = 1 when the cell's directional light
// has no diffuse colour and the game's bUseOptPasses is on (that light's slot then holds a lamp), else 0. Specular FaceGen,
// parallax, hair, glow-mapped, model-space-normal, canopy-shadowed and tree-branch objects never take it. Past the limit the
// object is drawn as a light-only pass with 2 lamps (AD3), extra lamp passes of 2-3 lamps (DiffusePoint2/3), a texture pass
// that multiplies, a highlight pass for the main light and highlight passes for the lamps (SpecularPoint2/3) and a fog pass:
// 5-7 draws instead of one, every added pass summed in the stored (encoded) curve -- the linear-light pass-sum error
// (bench\foundations\REPORT.md, "Lamp-pass error": up to 3.7x too bright indoors).
// AddPass_Opt caps the specular pass at 3 + f entries (main light + 2 + f lamps); MoreLampsPerObject raised that cap to 6 + f
// (ADD ECX,2 -> ADD ECX,5 at 0x00BDEA3D, Hooks.cpp), but the compare in GetRenderPasses_2x (ADD EAX,2 at 0x00BE00E0, then
// CMP lamps,EAX / JG multi-pass) stayed at 2, so only alpha-blended and Single_Pass objects (they skip the compare) ever used the
// bigger cap. On: ADD EAX,5 -- specular objects with up to 5 + f lamps take the one pass, exactly what MoreLampsPerObject's cap
// holds, so no lamp is dropped; the shaders are the ones MoreLampsPerObject already uses (SPECULAR_LAMPS6: 6 light slots), no
// shader changes. Needs MoreLampsPerObject (refused without it: the game's cap of 3 would drop lamps).
// Live: the game keeps each object's pass list until its lights change or its key does (render mode << 8 |
// BSShaderManager::iEnabledPasses 0x011F91D8, BSShaderPPLightingProperty::GetRenderPasses 0x00B67BE0); every switch flips bit
// 0x10000000 of iEnabledPasses -- read nowhere else (all readers mask 0x10, 0x20 or 0xF) -- so every object rebuilds its passes
// on the next frame, the way the game's own console 'SetLightingPasses opt' does it with bit 0x100.
// MEASURED 2026-10-18 (bench\deep-roadmap\evidence\lightcap-LOG.md, same-launch pairs, frames 224): it works as designed
// (Ultra-Luxe: shiny multi-pass objects with 3-5 lamps 305 -> 0 a frame, world draws 3,836 -> 2,243, no lamp dropped; frame
// area with extra passes 83% -> 20%, FLIP to the exact-sum reference 0.39 -> 0.20) but it is SLOWER where it matters: Ultra-Luxe
// world scene +1.08 ms [+1.06, +1.13] (the one many-lamp pass costs +4.6 ms, the passes it replaces -3.2 ms); casino no
// measurable change (-0.05 ms world, one pair). Kept OFF: a look/accuracy option, not a speed-up, until the many-lamp shader
// is made cheaper.
namespace LampsInOnePass {

	constexpr UInt32 kCompareSite = 0x00BE00E0;		// ADD EAX,2 (83 C0 02) in GetRenderPasses_2x
	constexpr UInt32 kOptCapByte = 0x00BDEA3F;		// MoreLampsPerObject's byte (ADD ECX,imm8 in AddPass_Opt): 5 when it is on
	constexpr UInt32 kEnabledPasses = 0x011F91D8;	// BSShaderManager::iEnabledPasses
	constexpr UInt32 kRebuildBit = 0x10000000;
	constexpr UInt32 kGlowRebuildBit = 0x02000000;	// ManyLampsGlow's own (2026-11-15)
	constexpr UInt32 kGlowSite = 0x00BE0134;		// JNZ multi-pass (75 45) after CMP bGlowMap,0 (80 7C 24 3C 00) in GetRenderPasses_2x
	constexpr UInt32 kEmitSite = 0x00B7BAFA;		// the OPT range test (24 bytes) choosing SetEmitConstant / AddEmitToAmbient in 0x00B7BA70

	inline bool Applied = false;
	inline UInt8 AppliedImm = 0x02;	// the compare's immediate now (2 = the game's, 5 = LampsInOnePass, 12 = ManyLampsOnePass)
	inline bool RefusedLogged = false;
	inline unsigned Switches = 0;
	inline bool GlowApplied = false, GlowRefusedLogged = false;
	// Pass enums whose emittance the game adds to the ambient colour (bit set): the OPT passes without a glow map. Read by the code at
	// kEmitSite while ManyLampsGlow is on (BT with a bit offset: the enum, below 0x300).
	inline UInt32 AmbientEmitEnums[0x300 / 32] = {};
	inline bool EmitOnlyApplied = false, EmitOnlyRefused = false;	// (2026-11-15 ApplyEmitOnly / EmitForPass)
	inline UInt32 EmitCleared = 0;	// the enum whose bit EmitForPass cleared for the draw under way
	inline bool IsGlowOptEnum(UInt32 e) { return e == 0xC4 || e == 0xC7 || e == 0xCB || e == 0xCE || e == 0xD2 || e == 0xD5; }
	inline void InitAmbientEmitEnums() {	// the game's own choice: every OPT enum adds its emittance to the ambient but the glow ones
		if (AmbientEmitEnums[0xC3 / 32] || AmbientEmitEnums[0xD6 / 32]) return;
		for (UInt32 e = 0xC3; e <= 0xD6; e++) if (!IsGlowOptEnum(e)) AmbientEmitEnums[e / 32] |= 1u << (e % 32);
	}

	// UNOFFICIAL 2026-10-27 ManyLampsGlow ([Main.Main.Misc] ManyLampsGlow, live while ManyLampsOnePass is on; after Conor (mrfury97)'s fork, see ManyLamps.h). F10 census in the Vault 21
	// lobby with ManyLampsOnePass (log 14-58-42): of the highlight passes still drawn, 739 of ~790 a frame were glow-mapped shiny objects
	// (177 a frame, 8.5 lamps each, plus their 446 lamp passes): GetRenderPasses_2x never gives a glow-mapped object the one pass
	// (&& !bGlowMap). The game has everything else for it: AddPass_Opt picks BSSM_ADT10_G / ADT4_G / ADTS10_G (+ _SG skinned) for them,
	// those passes have the plain ones' shaders but the SI pixel shader (ADTS10_G: SLS2025.vso + SLS2036.pso, PresetADTSPasses_2x), the
	// glow map on stage 4 (0x00B7D200) and the emittance constant (pixel flag 0x10, ulPixelConstantFlags 316 as ADTS10). NVR's
	// LIGHTS >= 4 shaders had no glow (the passes were never used); ObjectTemplate.hlsl now adds it for SI. On: that JNZ becomes two NOPs,
	// so glow-mapped objects take the one pass like the others (shiny ones up to 12 + f lamps, plain ones up to 5 + f). Off: as before.
	inline void ApplyGlow(bool manyLampsOn) {
		const bool want = manyLampsOn && TheSettingManager->GetSettingI("Main.Main.Misc", "ManyLampsGlow") != 0;
		if (want == GlowApplied) return;
		const UInt8* code = (const UInt8*)kGlowSite;
		static const UInt8 kCmp[] = { 0x80, 0x7C, 0x24, 0x3C, 0x00 };
		const bool known = !memcmp(code - 5, kCmp, sizeof(kCmp)) && ((code[0] == 0x75 && code[1] == 0x45) || (code[0] == 0x90 && code[1] == 0x90));
		if (!known) {
			if (!GlowRefusedLogged) Logger::Log("UNOFFICIAL ManyLampsGlow: the game's code at %08X is not as expected (another plugin?); not changed", kGlowSite);
			GlowRefusedLogged = true;
			return;
		}
		// The game gives an OPT pass no emittance constant: SetupGeometryConstants (0x00B7BA70) adds the material's emittance to the
		// ambient colour for every enum 0xC3-0xD6 (AddEmitToAmbient 0x00C03010; the shader's c2 is left stale, .w = the lamp count).
		// Right for objects without a glow map; with one, the whole surface glowed (first run 2026-10-27, Vault 21: white walls) instead
		// of the map's lit parts. So the glow OPT enums (ADT10_G C4, ADT10_SG C7, ADT4_G CB, ADT4_SG CE, ADTS10_G D2, ADTS10_SG D5) take
		// SetEmitConstant (0x00C03180: emittance x HDR multiplier in c2, as the multi-pass glow passes) and the shader masks it with the
		// glow map; the game then still writes the lamp count into c2.w.
		static const UInt8 kEmitOriginal[] = { 0x0F, 0xB7, 0x46, 0x04, 0xBA, 0xC3, 0x00, 0x00, 0x00, 0x66, 0x3B, 0xC2, 0x72, 0x0A,
			0xBA, 0xD6, 0x00, 0x00, 0x00, 0x66, 0x3B, 0xC2, 0x76, 0x0C };
		UInt8 emitPatched[sizeof(kEmitOriginal)];
		memset(emitPatched, 0x90, sizeof(emitPatched));
		static const UInt8 kEmitHead[] = { 0x0F, 0xB7, 0x46, 0x04, 0x0F, 0xA3, 0x05 };	// movzx eax,[esi+4] ; bt [AmbientEmitEnums],eax
		memcpy(emitPatched, kEmitHead, sizeof(kEmitHead));
		*(UInt32*)(emitPatched + 7) = (UInt32)AmbientEmitEnums;
		emitPatched[11] = 0x72; emitPatched[12] = (UInt8)(0x00B7BB1E - (kEmitSite + 13));	// jc AddEmitToAmbient; else SetEmitConstant (0x00B7BB12)
		const UInt8* emit = (const UInt8*)kEmitSite;
		if (memcmp(emit, kEmitOriginal, sizeof(kEmitOriginal)) && memcmp(emit, emitPatched, sizeof(emitPatched))) {
			if (!GlowRefusedLogged) Logger::Log("UNOFFICIAL ManyLampsGlow: the game's code at %08X is not as expected (another plugin?); not changed", kEmitSite);
			GlowRefusedLogged = true;
			return;
		}
		InitAmbientEmitEnums();
		static const UInt8 kNops[] = { 0x90, 0x90 }, kJnz[] = { 0x75, 0x45 };
		SafeWriteBuf(kEmitSite, want ? emitPatched : kEmitOriginal, sizeof(kEmitOriginal));
		SafeWriteBuf(kGlowSite, want ? kNops : kJnz, 2);
		// (2026-11-15 bench batch 6: its own bit. With the compare's bit, a glow switch and a compare switch in the same frame (the deferred
		// renderer going forward on native: both follow DeferredRenderer::OnePass) flipped 0x10000000 twice: no rebuild, every object kept its
		// one-pass list, and the glow OPT passes, now without the patch, added their whole emittance to the ambient (Live off over-bright,
		// one-pass speed). 0x02000000 is read nowhere else (PbrPop.h's bits: 0x04, 0x08, 0x20, 0x40 x 0x1000000; FoldFog 0x20000000).)
		*(UInt32*)kEnabledPasses ^= kGlowRebuildBit;	// every object rebuilds its passes this frame
		GlowApplied = want;
		Logger::Log("UNOFFICIAL ManyLampsGlow: %s (glow-mapped objects %s; every object's passes rebuilt)", want ? "on" : "off",
			want ? "take the one pass too, their emittance masked by the glow map" : "drawn in several passes, as the game does");
	}
	inline bool ApplyEmitOnly(bool want) {
		if (GlowApplied) { EmitOnlyApplied = false; return want; }	// (2026-11-15) the site is ManyLampsGlow's patch: the same bit lookup, owned there
		if (want == EmitOnlyApplied || (want && EmitOnlyRefused)) return EmitOnlyApplied;
		static const UInt8 kEmitOriginal[] = { 0x0F, 0xB7, 0x46, 0x04, 0xBA, 0xC3, 0x00, 0x00, 0x00, 0x66, 0x3B, 0xC2, 0x72, 0x0A,
			0xBA, 0xD6, 0x00, 0x00, 0x00, 0x66, 0x3B, 0xC2, 0x76, 0x0C };	// (as ApplyGlow)
		UInt8 emitPatched[sizeof(kEmitOriginal)];
		memset(emitPatched, 0x90, sizeof(emitPatched));
		static const UInt8 kEmitHead[] = { 0x0F, 0xB7, 0x46, 0x04, 0x0F, 0xA3, 0x05 };	// movzx eax,[esi+4] ; bt [AmbientEmitEnums],eax
		memcpy(emitPatched, kEmitHead, sizeof(kEmitHead));
		*(UInt32*)(emitPatched + 7) = (UInt32)AmbientEmitEnums;
		emitPatched[11] = 0x72; emitPatched[12] = (UInt8)(0x00B7BB1E - (kEmitSite + 13));	// jc AddEmitToAmbient; else SetEmitConstant
		const UInt8* emit = (const UInt8*)kEmitSite;
		if (memcmp(emit, want ? kEmitOriginal : emitPatched, sizeof(kEmitOriginal))) {
			EmitOnlyRefused = true;
			Logger::Log("UNOFFICIAL DeferredRenderer: the game's emittance code at %08X is not as expected (another plugin?); not changed: emissive objects are held at the game's own one-pass limit instead", kEmitSite);
			return EmitOnlyApplied;
		}
		InitAmbientEmitEnums();
		SafeWriteBuf(kEmitSite, want ? emitPatched : kEmitOriginal, sizeof(kEmitOriginal));
		EmitOnlyApplied = want;
		Logger::Log("UNOFFICIAL DeferredRenderer: %s", want ? "one-pass emittance per object on (an OPT draw adds its emittance to the ambient only where the game's own passes would have drawn it in one pass)" :
			"one-pass emittance per object off (the game's code back)");
		return EmitOnlyApplied;
	}

	// UNOFFICIAL 2026-11-15 (bench batch 4) the emittance code site alone (no glow patch), for the deferred renderer's one-pass compare on
	// native: AmbientEmitEnums then decides per OPT enum between AddEmitToAmbient (bit set) and SetEmitConstant, and EmitForPass sets that bit
	// before each OPT draw from the object's forward reference (OnePassLimit::ForwardMulti). Only while ManyLampsGlow has not patched the site
	// (it never does on native now). Off: the game's code back.
	inline bool ApplyEmitOnly(bool want);	// (below ApplyGlow's tables)
	inline void EmitAfterPass();
	inline void EmitForPass(const PointShadowForward::RenderPassView* pass) {
		EmitAfterPass();	// (a pass hook that returned before its after-draw call left a bit cleared)
		if (!(EmitOnlyApplied || (GlowApplied && OnePassLimit::EmitExact)) || !pass || !pass->geometry) return;
		const UInt16 e = pass->passEnum;
		if (e < 0xC3 || e > 0xD6 || IsGlowOptEnum(e)) return;
		const bool multi = OnePassLimit::ForwardMulti(pass->geometry);
		if (!multi) return;
		AmbientEmitEnums[e / 32] &= ~(1u << (e % 32));	// this draw: SetEmitConstant (no emittance in the ambient)
		EmitCleared = e;
		OnePassLimit::EmitDraws++;
	}
	// After the draw (Render.cpp, next to GlowAfterDraw): the game's own choice back for every other draw of that enum (draws that do not
	// pass through EmitForPass keep the game's behaviour).
	inline void EmitAfterPass() {
		if (!EmitCleared) return;
		AmbientEmitEnums[EmitCleared / 32] |= 1u << (EmitCleared % 32);
		EmitCleared = 0;
	}

	// The glow OPT passes read the glow map on stage 4. The game binds it there (SetupGeometryTextures 0x00B7D200) through its texture
	// cache, which skips the device call when the cache already shows the texture; code binding s4 on the device directly (interior
	// shadows, volumetric smoke) can leave the cache showing a glow map the device no longer holds. Second run 2026-10-27 (Vault 21,
	// same spot on/off): one-pass lamp boxes and grates had no glow. So before each glow OPT draw NVR binds the object's glow map itself
	// and notes it in the cache (the game's own bind then matches it). F10 only: what s4 held before the bind and after the draw.
	inline bool IsGlowOpt(UInt16 e) { return e == 0xC4 || e == 0xC7 || e == 0xCB || e == 0xCE || e == 0xD2 || e == 0xD5; }
	inline const void* GlowLastPass = nullptr;
	inline IDirect3DBaseTexture9* GlowLastTexture = nullptr;
	inline unsigned long long GlowDraws = 0, GlowRebound = 0, GlowCacheStale = 0, GlowLost = 0;
	inline unsigned GlowFrames = 0;

	// Before the game sets the draw up (Render.cpp ForwardShadowsForPass).
	inline void GlowForPass(const PointShadowForward::RenderPassView* pass) {
		GlowLastTexture = nullptr;
		if (!GlowApplied || !pass || !pass->geometry || !IsGlowOpt(pass->passEnum)) return;
		const BSShaderProperty* property = (const BSShaderProperty*)pass->geometry->propertyState.m_spShadeProperty;
		if (!property || *(const UInt32*)property != 0x10AE0D0) return;	// BSShaderPPLightingProperty
		const BSShaderPPLightingProperty* lighting = (const BSShaderPPLightingProperty*)property;
		const NiSourceTexture* glow = lighting->ppTextures[2] ? lighting->ppTextures[2][0] : nullptr;
		IDirect3DBaseTexture9* texture = glow && glow->rendererData ? glow->rendererData->dTexture : nullptr;
		if (!texture) return;
		IDirect3DDevice9* device = TheRenderManager->device;
		if (GpuTimer::Enabled) {
			IDirect3DBaseTexture9* now = nullptr;
			if (SUCCEEDED(device->GetTexture(4, &now)) && now) now->Release();
			if (now != texture) {
				GlowRebound++;
				if (TheRenderManager->renderState->unk10A0[4] == (UInt32)texture) GlowCacheStale++;	// the game would have skipped its bind
			}
			GlowDraws++;
			GlowLastPass = pass; GlowLastTexture = texture;
		}
		device->SetTexture(4, texture);
		TheRenderManager->renderState->NoteDeviceTexture(4, texture);
	}

	// After the draw (Render.cpp, next to ManyLamps::AfterDraw).
	inline void GlowAfterDraw(const void* pass) {
		if (!GlowLastTexture || pass != GlowLastPass) { GlowLastTexture = nullptr; return; }
		IDirect3DBaseTexture9* now = nullptr;
		if (SUCCEEDED(TheRenderManager->device->GetTexture(4, &now)) && now) now->Release();
		if (now != GlowLastTexture) GlowLost++;
		GlowLastTexture = nullptr;
	}

	// Once a frame, before the game's scene walk registers its objects.
	inline void BeginFrame() {
		// (2026-11-15 batch 3, native: the deferred renderer's own 23 + f compare switched ManyLampsGlow on too, and native's many-lamp SI
		// shaders (MANY_LAMPS_ON 0) have no glow (ObjectTemplate.hlsl ManyLampsGlow: only with MANY_LAMPS_ON): glow-mapped objects (Lucky 38's
		// red globes) went dark in the deferred picture. The glow patch now follows ManyLamps itself, the only time its shaders exist.)
		// (2026-11-15 batch 5: native Vault 21 kept ~1,400 extra draws a frame: its glow-mapped objects (wall lights, 177 a frame, 8.5 lamps
		// each) in several passes again. ManyLampsGlow now also runs for the renderer's compare on native, where the glow comes from the
		// lamps-deferred records (ObjectTemplate.hlsl: SI glow with LAMPS_DEFERRED 3); forward draws of those passes outside the G-buffer
		// span (reflections) have no glow on native.)
		ApplyGlow(Applied && AppliedImm == (UInt8)ManyLamps::CompareImm && (ManyLamps::Active() || (DeferredRenderer::OnePass && DeferredRenderer::Native)));	// last frame's limit (a frame late after a switch)
		if (!GpuTimer::Enabled) { GlowDraws = GlowRebound = GlowCacheStale = GlowLost = 0; GlowFrames = 0; }
		else if (GlowDraws && ++GlowFrames >= 600) {
			Logger::Log("UNOFFICIAL ManyLampsGlow (F10): per frame %.1f glow one-pass draws, s4 not their glow map before Vegas Shaders' bind %.1f (the game's cache showed it already: %.1f), "
				"s4 not the glow map after the draw %.2f", GlowDraws / (double)GlowFrames, GlowRebound / (double)GlowFrames, GlowCacheStale / (double)GlowFrames,
				GlowLost / (double)GlowFrames);
			GlowDraws = GlowRebound = GlowCacheStale = GlowLost = 0; GlowFrames = 0;
		}
		const UInt8* code = (const UInt8*)kCompareSite;
		// UNOFFICIAL 2026-10-27 ManyLampsOnePass (ManyLamps.h): up to 12 + f lamps (NVR gives the shaders the lamps past the game's cap).
		// Its limit is live (the setting read here each frame) while its shaders stay as loaded: off, the limit goes back to 5 + f, so
		// the two can be compared in one launch at the same spot (F10).
		// UNOFFICIAL 2026-10-27: and only while NVR's object shaders draw (ShaderRecord: with Effects off or the PBR shaders off the game's
		// own draw, with 6 lamp slots and no glow in their many-lamp shaders): otherwise LampsInOnePass's limit, and ManyLampsGlow off.
		const bool nvrObjectShaders = TheSettingManager->SettingsMain.Main.RenderEffects && TheShaderManager->Shaders.PBR && TheShaderManager->Shaders.PBR->Enabled;
		const bool manyLamps = ManyLamps::Active() && nvrObjectShaders && TheSettingManager->GetSettingI("Main.Main.Misc", "ManyLampsOnePass") != 0;
		// UNOFFICIAL 2026-11-14 full deferred renderer (core/DeferredRenderer.h, plan 4.5): while it is live indoors (any runtime) the game draws
		// shiny objects with up to 23 + f lamps in ONE pass (the deferred record lights none of them; the lamp pass lights every lamp of the
		// table), so native D3D9 draws one pass per object. Its forward fallbacks then light only the shader's slots: DeferredRenderer counts
		// those draws. Not outdoors and not while it is forward: the limit goes back (every object's passes rebuilt).
		// (2026-11-14 fd-fixes) It cannot be limited to the G-buffer span: the compare is read when an object's pass list is built, at
		// registration (BSShaderAccumulator::RegisterObject_Standard 0x00B64020 -> GetRenderPasses 0x00B67BE0 -> GetRenderPasses_2x 0x00BDF790),
		// before the main list draws, and the list is kept until the pass key changes; flipping it twice a frame would only rebuild every
		// object's passes twice a frame. It is read per object, though: the per-object limit below holds the objects that draw forward at
		// the forward shader's limit (native); what is left is counted on DeferredRenderer's F10 line (docs/next/FD-FIXES-RESULTS.md).
		const bool deferred = DeferredRenderer::OnePass;
		{	// UNOFFICIAL 2026-11-14 fd-fixes (bench batch 2, native D3D9: forward draws lost the lamps past the game's cap of 6 + f): without
			// ManyLamps feeding the forward shaders (native), the objects that may draw forward keep the forward limit, per object, in
			// ActorsInOnePass's cave at 0x00BE00E3 (OnePassLimit.h, PbrPop.h ActorPasses::DeferredLimit). A switch on (interior again) forgets the learned set.
			const UInt8 limitOn = deferred && !manyLamps ? 1 : 0;
			if (limitOn && !OnePassLimit::On) OnePassLimit::Clear();
			OnePassLimit::On = limitOn;
			OnePassLimit::ForwardImm = *(const UInt8*)kOptCapByte == 0x05 ? 5 : 2;
			OnePassLimit::ReferenceImm = TheSettingManager->GetSettingI("Main.Main.Misc", "LampsInOnePass") != 0 ? 5 : 2;	// (the forward reference's compare)
			// (2026-11-15 batch 4) mode 1: the emittance matched per draw (EmitForPass); the hold only if the code site could not be patched
			const bool exact = ApplyEmitOnly(limitOn && DeferredRenderer::OnePassMode == 1);
			if (exact && !OnePassLimit::EmitExact) { OnePassLimit::ClearMulti(); OnePassLimit::RebuildWanted = true; }	// (every object's forward choice noted at its next build)
			OnePassLimit::EmitExact = exact ? 1 : 0;
			OnePassLimit::HoldEmissive = limitOn && DeferredRenderer::OnePassMode == 1 && !exact ? 1 : 0;
		}
		const UInt8 onImm = manyLamps || deferred ? (UInt8)ManyLamps::CompareImm : (UInt8)0x05;
		// UNOFFICIAL 2026-10-28 (port onto P84 v3 / LinearScene): LampsInOnePass is developer-only and off by default now (LinearScene makes the
		// game's several passes add up exactly, so its picture reason is gone), so ManyLampsOnePass switches the one-pass limit on by itself:
		// ManyLampsOnePass off (live) = LampsInOnePass's own setting (off: the game's 2 + f).
		const bool want = manyLamps || deferred || TheSettingManager->GetSettingI("Main.Main.Misc", "LampsInOnePass") != 0;
		if ((want ? onImm : (UInt8)0x02) == AppliedImm) return;
		if (code[0] != 0x83 || code[1] != 0xC0 || (code[2] != 0x02 && code[2] != 0x05 && code[2] != (UInt8)ManyLamps::CompareImm)) {
			if (!RefusedLogged) Logger::Log("UNOFFICIAL LampsInOnePass: the game's code at %08X is not as expected (another plugin?); not changed", kCompareSite);
			RefusedLogged = true;
			return;
		}
		if (want && !deferred && *(const UInt8*)kOptCapByte != 0x05) {	// (deferred: the game's cap only bounds its own arrays; no lamp is lost to it)
			if (!RefusedLogged) Logger::Log("UNOFFICIAL LampsInOnePass: needs MoreLampsPerObject (the game's cap of 3 would drop lamps); not changed");
			RefusedLogged = true;
			return;
		}
		SafeWrite8(kCompareSite + 2, want ? onImm : 0x02);
		*(UInt32*)kEnabledPasses ^= kRebuildBit;	// new pass-list key: every object rebuilds its passes this frame
		Applied = want;
		AppliedImm = want ? onImm : (UInt8)0x02;
		Switches++;
		Logger::Log("UNOFFICIAL LampsInOnePass: %s (shiny objects with up to %s lamps in one pass; every object's passes rebuilt)%s",
			want ? "on" : "off", want ? (onImm == 0x05 ? "5-6" : "23-24") : "2-3", deferred ? " for the deferred renderer" : "");
	}

#if NVR_TEST_TOOLS
	// DIAGNOSTIC (only while the F10 GPU profile is on, world scene only): which objects the game draws in several lighting
	// passes and with how many lamps, the many-lamp ("OPT") passes and whether any of their lights are dropped -- by the game's
	// caps (RenderPass +0x0A = lamps + 1 before the cap, +0x09 after it) or by the shader's light slots. Logged as LAMPS IN ONE
	// PASS lines every 112 frames and whenever the switch changes (so a window never mixes the two states).
	namespace Census {
		struct PassView { NiGeometry* geometry; UInt16 passEnum; UInt8 unk06, unk07, unk08, numLights, maxLights, unk0B; void** lights; };

		inline bool IsLampPass(UInt16 e) { return (e >= 0x132 && e <= 0x13C && e != 0x137) || (e >= 0x13E && e <= 0x148 && e != 0x143); }	// as LampPassTest
		inline bool IsHighlightPass(UInt16 e) {
			switch (e) {
			case 0x15A: case 0x15C: case 0x15D: case 0x160: case 0x162: case 0x163:
			case 0x166: case 0x168: case 0x169: case 0x16C: case 0x16E: case 0x16F:
			case 0x172: case 0x174: case 0x175:
				return true;
			}
			return false;
		}
		inline bool IsOptPass(UInt16 e) { return e >= 0xC3 && e <= 0xD6; }
		inline bool FirstLightIsLamp(UInt16 e) { return e == 0xCA || e == 0xD1; }	// PointShadowForward::FirstShaderLight

		// Light slots of NVR's many-lamp object shaders (ObjectTemplate.hlsl MAX_LIGHTS; src/effects/PBR.h): SLS2029/2030 (LIGHTS 9)
		// 6, SLS2031-2033 (LIGHTS 4) 4, SLS2034-2036 (LIGHTS 4, SPECULAR) 6 with MoreLampsPerObject, else 3. 0 = unknown.
		inline unsigned ShaderSlots(const char* ps) {
			if (!ps || strncmp(ps, "SLS", 3)) return 0;
			const int n = atoi(ps + 3);
			if (n == 2029 || n == 2030) return 6;
			if (n >= 2031 && n <= 2033) return 4;
			if (n >= 2034 && n <= 2036) return ManyLamps::Active() ? (unsigned)ManyLamps::MaxLamps : *(const UInt8*)kOptCapByte == 0x05 ? 6 : 3;
			return 0;
		}

		// BSShaderLightingProperty::GetNumberOfActiveNonShadowLights (0x00B707D0), read the same way (light list at +0x60).
		inline unsigned CountLamps(const void* property) {
			unsigned n = 0;
			for (const UInt32* node = *(const UInt32* const*)((const UInt8*)property + 0x60); node; node = (const UInt32*)node[0]) {
				const UInt8* light = (const UInt8*)node[2];
				if (!light || *(const UInt16*)(light + 0x110) == 0xFF) continue;
				const UInt8* niLight = *(const UInt8* const*)(light + 0xF8);
				if (!niLight || (niLight[0x30] & 1) || light[0xEC]) continue;
				n++;
			}
			return n;
		}

		constexpr unsigned kWindow = 112;
		inline unsigned Frames = 0;
		inline bool WindowState = false;
		inline UInt8 WindowImm = 0x02;	// the limit the open window was measured with (2026-10-27: ManyLampsOnePass switches live too)
		inline unsigned long long WorldDraws = 0, LampPasses = 0, HighlightPasses = 0, TexturePasses = 0, FogPasses = 0;
		inline unsigned long long OptDraws[0x14] = {}, OptLights[0x14][16] = {};
		inline unsigned long long Dropped = 0, DroppedDraws = 0, DroppedSkipCompare = 0, OverSlots = 0, OverSlotDraws = 0;
		inline std::string ShaderOf[0x14];
		inline unsigned long long Multi = 0, ShinyByLamps[10] = {}, ShinyFaceGen = 0, Plain = 0, Flagged = 0, NotPP = 0, Lighting30 = 0;
		inline unsigned long long ShinySkinned = 0;
		// 2026-10-27 highlight-pass study: the multi-pass objects by group, with their lamp passes and their highlight passes drawn or
		// skipped (FoldHighlights), to see whose highlight passes are left with ManyLampsOnePass.
		enum Group : UInt8 { GShiny = 0, GShinyOver, GFaceGen, GPlain, GParallax, GMsn, GHair, GGlow, GLighting30, GOther, kGroups };
		inline const char* const kGroupNames[kGroups] = { "shiny within the limit", "shiny past the limit", "shiny FaceGen", "not shiny", "parallax", "MSN",
			"hair", "glow", "Lighting30", "other property" };
		inline unsigned long long GroupObjects[kGroups] = {}, GroupLampCount[kGroups] = {}, GroupLamps[kGroups] = {}, GroupHighlights[kGroups] = {},
			GroupSkipped[kGroups] = {}, GroupShiny[kGroups] = {};
		// 2026-10-27: the shiny objects still past the limit, by their lamps (40 = 40 or more).
		constexpr unsigned kPastMax = 40;
		inline unsigned long long PastByLamps[kPastMax + 1] = {};
		// geometries with a lamp or highlight pass this frame (open addressing, tagged with the frame number: no clearing)
		constexpr unsigned kSeen = 8192;
		inline const void* SeenKey[kSeen] = {};
		inline unsigned SeenFrame[kSeen] = {};
		inline UInt8 SeenGroup[kSeen] = {};
		inline UInt8 Classify(const BSShaderProperty* property);
		inline unsigned FrameId = 1;
		// The shaders of the last bind (SetShadersHook). The game binds about once per run of same-enum passes (Ultra-Luxe: 321
		// binds for 3,836 world draws a frame), and matching each OPT draw with a bind made after it found none (run 20261018-105643):
		// the bind comes before the run's draws (inferred), so a pass is matched with the last bind of its enum. (Not yet run in game.)
		inline UInt32 BoundPass = 0xFFFFFFFF;
		inline const char* BoundPS = nullptr;
		inline const char* BoundVS = nullptr;

		inline bool SkipsCompare(const NiGeometry* geometry, UInt32 flags0) {
			const NiAlphaProperty* alpha = geometry ? geometry->propertyState.m_spAlphaProperty : nullptr;
			return (flags0 & 0x20) || (alpha && (alpha->flags & 1));
		}

		inline void Reset() {
			Frames = 0; WorldDraws = LampPasses = HighlightPasses = TexturePasses = FogPasses = 0;
			memset(OptDraws, 0, sizeof(OptDraws)); memset(OptLights, 0, sizeof(OptLights));
			Dropped = DroppedDraws = DroppedSkipCompare = OverSlots = OverSlotDraws = 0;
			Multi = ShinyFaceGen = Plain = Flagged = NotPP = ShinySkinned = Lighting30 = 0; memset(ShinyByLamps, 0, sizeof(ShinyByLamps));
			memset(GroupObjects, 0, sizeof(GroupObjects)); memset(GroupLampCount, 0, sizeof(GroupLampCount)); memset(GroupLamps, 0, sizeof(GroupLamps));
			memset(GroupHighlights, 0, sizeof(GroupHighlights)); memset(GroupSkipped, 0, sizeof(GroupSkipped)); memset(GroupShiny, 0, sizeof(GroupShiny));
			memset(PastByLamps, 0, sizeof(PastByLamps));
		}

		inline void Report() {
			if (!Frames) return;
			const double f = Frames;
			char line[2048]; int n = 0;
			n += sprintf_s(line + n, sizeof(line) - n, "LAMPS IN ONE PASS census (%s, one pass up to %u lamps + f), %u frames, per frame: world draws %.0f | OPT passes",
				WindowState ? "on" : "off", (unsigned)WindowImm, Frames, WorldDraws / f);
			for (int i = 0; i < 0x14; i++) if (OptDraws[i]) n += sprintf_s(line + n, sizeof(line) - n, " %02X:%.1f", 0xC3 + i, OptDraws[i] / f);
			n += sprintf_s(line + n, sizeof(line) - n, " | extra lamp passes %.1f, highlight passes %.1f, texture passes %.1f, fog passes %.1f",
				LampPasses / f, HighlightPasses / f, TexturePasses / f, FogPasses / f);
			Logger::Log("%s", line);
			n = 0;
			n += sprintf_s(line + n, sizeof(line) - n, "LAMPS IN ONE PASS census (%s): multi-pass objects %.1f a frame: shiny by lamps", WindowState ? "on" : "off", Multi / f);
			for (int k = 0; k < 10; k++) if (ShinyByLamps[k]) n += sprintf_s(line + n, sizeof(line) - n, " %d%s:%.1f", k, k == 9 ? "+" : "", ShinyByLamps[k] / f);
			n += sprintf_s(line + n, sizeof(line) - n, " (of them skinned %.1f) | shiny FaceGen %.1f | not shiny %.1f | parallax/hair/glow/MSN %.1f | Lighting30 property %.1f | other property %.1f",
				ShinySkinned / f, ShinyFaceGen / f, Plain / f, Flagged / f, Lighting30 / f, NotPP / f);
			Logger::Log("%s", line);
			Logger::Log("LAMPS IN ONE PASS census (%s): OPT lights dropped by the game's cap %.2f a frame in %.2f draws (alpha/Single_Pass objects %.2f, all others %.2f); "
				"lights past the shader's slots %.2f a frame in %.2f draws; bUseOptPasses %u, switches so far %u",
				WindowState ? "on" : "off", Dropped / f, DroppedDraws / f, DroppedSkipCompare / f, (Dropped - DroppedSkipCompare) / f, OverSlots / f, OverSlotDraws / f,
				(unsigned)*(const UInt8*)0x011AD80D, Switches);
			n = 0;
			n += sprintf_s(line + n, sizeof(line) - n, "LAMPS IN ONE PASS census (%s) by group, per frame (objects, of them shiny, lamps per object, lamp passes, "
				"highlight passes drawn + skipped):", WindowState ? "on" : "off");
			for (int g = 0; g < kGroups; g++) if (GroupObjects[g])
				n += sprintf_s(line + n, sizeof(line) - n, " | %s %.1f, %.1f, %.1f, %.1f, %.1f + %.1f", kGroupNames[g], GroupObjects[g] / f, GroupShiny[g] / f,
					GroupLampCount[g] / (double)GroupObjects[g], GroupLamps[g] / f, (GroupHighlights[g] - GroupSkipped[g]) / f, GroupSkipped[g] / f);
			Logger::Log("%s", line);
			if (GroupObjects[GShinyOver]) {
				n = 0;
				n += sprintf_s(line + n, sizeof(line) - n, "LAMPS IN ONE PASS census (%s): shiny objects past the limit by lamps, per frame:", WindowState ? "on" : "off");
				for (unsigned k = 0; k <= kPastMax; k++) if (PastByLamps[k])
					n += sprintf_s(line + n, sizeof(line) - n, " %u%s:%.1f", k, k == kPastMax ? "+" : "", PastByLamps[k] / f);
				Logger::Log("%s", line);
			}
			for (int i = 0; i < 0x14; i++) {
				if (!OptDraws[i]) continue;
				n = 0;
				n += sprintf_s(line + n, sizeof(line) - n, "LAMPS IN ONE PASS   OPT %02X (%s) drawn with %s: %.1f a frame, lights per draw", 0xC3 + i,
					Pointers::Functions::GetPassDescription(0xC3 + i), ShaderOf[i].empty() ? "?" : ShaderOf[i].c_str(), OptDraws[i] / f);
				for (int k = 0; k < 16; k++) if (OptLights[i][k]) n += sprintf_s(line + n, sizeof(line) - n, " %d:%.1f", k, OptLights[i][k] / f);
				Logger::Log("%s", line);
			}
			Reset();
		}

		inline void NewFrame() {
			FrameId++;
			if (!GpuTimer::Enabled) { if (Frames) Reset(); return; }
			if (Frames && (WindowState != Applied || WindowImm != AppliedImm)) Report();	// the switch changed: close the old state's window
			WindowState = Applied;
			WindowImm = AppliedImm;
			if (++Frames >= kWindow) Report();
		}

		// Before a world pass is drawn (RenderPassStandardHook / RenderPassSkinnedHook).
		inline void Note(const void* passPtr, bool world) {
			if (!world || !passPtr || !GpuTimer::Enabled) return;
			const PassView* pass = (const PassView*)passPtr;
			const UInt16 e = pass->passEnum;
			WorldDraws++;
			if (e >= 0x14A && e <= 0x150) { TexturePasses++; return; }
			if (e >= 0x252 && e <= 0x255) { FogPasses++; return; }
			NiGeometry* geometry = pass->geometry;
			const BSShaderProperty* property = geometry ? (const BSShaderProperty*)geometry->propertyState.m_spShadeProperty : nullptr;
			if (IsOptPass(e)) {
				const unsigned i = e - 0xC3;
				OptDraws[i]++;
				OptLights[i][min((unsigned)pass->numLights, 15u)]++;
				const bool skips = property && SkipsCompare(geometry, property->ulFlags[0]);
				if (pass->maxLights > pass->numLights) {
					Dropped += pass->maxLights - pass->numLights; DroppedDraws++;
					if (skips) DroppedSkipCompare += pass->maxLights - pass->numLights;
				}
				if (BoundPass == e) {	// its shaders: the last bind of this enum (OnBind), and their light slots
					if (ShaderOf[i].empty()) ShaderOf[i] = std::string(BoundPS ? BoundPS : "?") + "/" + (BoundVS ? BoundVS : "?");
					const unsigned slots = ShaderSlots(BoundPS);
					const unsigned used = FirstLightIsLamp(e) ? (pass->numLights ? pass->numLights - 1u : 0u) : pass->numLights;
					if (slots && used > slots) { OverSlots += used - slots; OverSlotDraws++; }
				}
				return;
			}
			const bool lamp = IsLampPass(e), highlight = !lamp && IsHighlightPass(e);
			if (lamp) LampPasses++;
			if (highlight) HighlightPasses++;
			if (!lamp && !highlight) return;
			// a multi-pass object: classified once a frame, its lamp and highlight passes counted by group
			unsigned h = (unsigned)(((uintptr_t)geometry >> 4) * 2654435761u) & (kSeen - 1);
			UInt8 group = kGroups;
			for (unsigned probe = 0; probe < 32; probe++, h = (h + 1) & (kSeen - 1)) {
				if (SeenFrame[h] == FrameId && SeenKey[h] == geometry) { group = SeenGroup[h]; break; }
				if (SeenFrame[h] != FrameId) { SeenFrame[h] = FrameId; SeenKey[h] = geometry; SeenGroup[h] = group = Classify(property); break; }
			}
			if (group >= kGroups) return;
			if (lamp) GroupLamps[group]++; else GroupHighlights[group]++;
		}

		// The group of a multi-pass object (once a frame, at its first lamp or highlight pass).
		inline UInt8 Classify(const BSShaderProperty* property) {
			Multi++;
			const UInt32 vtable = property ? *(const UInt32*)property : 0;
			if (vtable == 0x10B9910) { Lighting30++; GroupObjects[GLighting30]++; return GLighting30; }	// Lighting30ShaderProperty: its own GetRenderPasses (0x00BB4740), not changed here
			if (vtable != 0x10AE0D0) { NotPP++; GroupObjects[GOther]++; return GOther; }		// BSShaderPPLightingProperty
			const UInt32 flags0 = property->ulFlags[0];
			const BSShaderPPLightingProperty* lighting = (const BSShaderPPLightingProperty*)property;
			const bool glow = !(flags0 & 0x400) && lighting->ppTextures[2] && lighting->ppTextures[2][0];
			const unsigned lamps = CountLamps(property);
			UInt8 group;
			if ((flags0 & (0x800 | 0x1000 | 0x40000)) || glow) {
				Flagged++;
				group = (flags0 & 0x40000) ? GHair : (flags0 & 0x1000) ? GMsn : (flags0 & 0x800) ? GParallax : GGlow;
			}
			else if (!(flags0 & 1)) { Plain++; group = GPlain; }
			else if (flags0 & 0x400) { ShinyFaceGen++; group = GFaceGen; }
			else {
				ShinyByLamps[min(lamps, 9u)]++;
				if (flags0 & 2) ShinySkinned++;
				group = lamps > WindowImm ? GShinyOver : GShiny;
				if (group == GShinyOver) PastByLamps[min(lamps, kPastMax)]++;
			}
			GroupObjects[group]++;
			GroupLampCount[group] += lamps;
			if (flags0 & 1) GroupShiny[group]++;
			return group;
		}

		// A highlight pass FoldHighlights skipped (Note counted it first).
		inline void NoteSkipped(const void* passPtr) {
			if (!GpuTimer::Enabled || !passPtr) return;
			const void* geometry = ((const PassView*)passPtr)->geometry;
			unsigned h = (unsigned)(((uintptr_t)geometry >> 4) * 2654435761u) & (kSeen - 1);
			for (unsigned probe = 0; probe < 32; probe++, h = (h + 1) & (kSeen - 1)) {
				if (SeenFrame[h] != FrameId) return;
				if (SeenKey[h] == geometry) { if (SeenGroup[h] < kGroups) GroupSkipped[SeenGroup[h]]++; return; }
			}
		}

		// After the game bound a pass's shaders (SetShadersHook; once per run of same-enum passes, before its first draw).
		inline void OnBind(UInt32 passIndex, const char* ps, const char* vs) { BoundPass = passIndex; BoundPS = ps; BoundVS = vs; }
	}
#endif
}
