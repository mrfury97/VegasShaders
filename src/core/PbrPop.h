#pragma once
#include <algorithm>
#include <string>
#include "TestTools.h"
#include "OnePassLimit.h"	// UNOFFICIAL 2026-11-14 fd-fixes

// UNOFFICIAL 2026-10-18 PBR pop-in (helper pbrpop; bench\deep-roadmap\evidence\pbrpop-LOG.md).
//
// The game swaps an object's shaders, and sometimes its whole set of lighting passes, at fixed points, and NVR's PBR object shaders
// did not give the same picture on both sides, so the object's look changed at once ("popped"):
//  1. Highlight distance (fSpecularLODStartFade + fSpecularLODRange from the camera, 2000 + 300 units by default): beyond it the game
//     draws an object without its highlight passes and with the plain shaders. The game's own shaders faded the highlight out before
//     that point (BSShaderPPLightingProperty::GetSpecularLODFade 0x00B66B80); NVR's ignored the fade. SpecularFade (below, and
//     Includes/PBR.hlsl PBR_SPEC_FADE) gives the shaders that fade, so at the swap the shiny shader draws what the plain one does.
//  2. The same point also changes HOW MANY passes draw a shiny object lit by 3-5 lamps: near, the game draws it in several passes
//     (light, extra lamps, texture, highlights, fog), whose sums are made in the stored curve (with linear lighting that comes out up to
//     2-3x too bright: the known lamp-pass error); beyond the point it takes ONE many-lamp pass, whose sum is exact. The object jumped in
//     brightness as it crossed the point. SpecularLODPasses keeps the game's choice of pass set on the object's own specular flag, not on
//     the distance-cut one, so the object is drawn the same way on both sides (only without its highlight passes beyond the point).
//     Engine reading (Ghidra pseudo-code of FalloutNV.exe, names from WallSoGB's IDA map -- credit WallSoGB): in
//     BSShaderPPLightingProperty::GetRenderPasses_2x (0x00BDF790) the one-pass capacity test reads local_64 = 'highlight passes wanted'
//     (the specular flag after the distance cut) three times: CMP byte ptr [ESP+0x28],0 at 0x00BE00BD, 0x00BE00D2 and 0x00BE00EB
//     (80 7C 24 28 00). Each becomes TEST byte ptr [ESI+0x20],1 + NOP (F6 46 20 01 90): ESI is the property all through the function,
//     +0x20 its first flag word, bit 0 the specular flag. The highlight passes themselves still follow local_64 (dropped beyond the
//     point, as before). Every switch flips a bit of BSShaderManager::iEnabledPasses that nothing reads (as LampsInOnePass does) so every
//     object rebuilds its passes on the next frame.
// What is NOT fixed here: an object whose lamp count crosses 2 <-> 3 (a moving NPC, a lamp that comes and goes, LampsInOnePass off)
// still swaps between the exact one-pass sum and the multi-pass stored-curve sum. The cures for that are the existing options
// LampsInOnePass and ExactPassSums (both cost ~1-1.6 ms indoors) -- the user's call.
namespace PbrPop {

	constexpr UInt32 kEnabledPasses = 0x011F91D8;	// BSShaderManager::iEnabledPasses (see LampsInOnePass.h)

	inline const BSShaderProperty* LitProperty(const NiGeometry* geometry) {
		const BSShaderProperty* property = geometry ? (const BSShaderProperty*)geometry->propertyState.m_spShadeProperty : nullptr;
		if (!property) return nullptr;
		const UInt32 vtable = *(const UInt32*)property;
		return vtable == 0x10AE0D0 || vtable == 0x10B9910 ? property : nullptr;	// BSShaderPPLightingProperty, Lighting30ShaderProperty
	}

	// The game's highlight fade for the object (BSShaderPPLightingProperty::GetSpecularLODFade, vtable +0x10C in both classes):
	// 1 near (or not shiny, or the distance cut off), down to 0 at fSpecularLODEnd, from the distance GetRenderPasses keeps at +0x34.
	inline float GameSpecularFade(const BSShaderProperty* property) {
		if (!property) return 1.0f;
		typedef float(__thiscall* FadeFn)(const void*);
		const FadeFn fade = (FadeFn)(*(const UInt32*)(*(const UInt32*)property + 0x10C));
		const float f = fade(property);
		return f >= 0.0f ? (f <= 1.0f ? f : 1.0f) : 0.0f;
	}

	// ---- 1. SpecularFade ([Shaders.PBR.Main] SpecularFade; compiled in at startup, live while compiled) ----
	namespace SpecFade {
		inline bool Checked = false, Compiled = false, On = false;
		inline bool FoldCompiled = false;	// UNOFFICIAL 2026-10-21 FoldHighlights (core/FoldHighlights.h) is compiled in: it sends c211.y/z
		inline float Sent = -1.0f, SentFold = 0.0f, SentVc = 0.0f;
		inline float MaterialW = 0.0f, SentW = 0.0f;	// UNOFFICIAL 2026-10-31 MaterialBuffer: c211.w (class + 8 x floor(radius), SendMaterial)

		inline void BeginFrame() {
			if (!Checked) {
				Checked = true;
				Compiled = TheSettingManager->GetSettingI("Shaders.PBR.Main", "SpecularFade") != 0;	// as ShaderRecord compiled it
				Logger::Log("UNOFFICIAL SpecularFade: %s", Compiled ? "on (object highlights fade out with the game's specular distance fade)" : "off (not compiled into the shaders)");
			}
			On = Compiled && TheSettingManager->GetSettingI("Shaders.PBR.Main", "SpecularFade") != 0;
			Sent = -1.0f;	// resent with the frame's first object (effects between frames may use the high registers)
		}

		// One object pass about to be drawn (RenderPassStandard/Skinned hooks): its fade in pixel c211.x; FoldHighlights' switches for the
		// draw in y (fold) and z (with the vertex colour), 0 for every draw it does not fold.
		// (2026-11-16 fd-batchE, docs/next/FD-BATCH-E.md) z is now the vertex colour flag for EVERY lit draw (the property's Vertex_Colors: the
		// game's TEXTURE pass is then TEXTURE_Vc), not only the folded ones: the deferred records of the light-only base pass (AD) take the
		// vertex colour into their G-buffer albedo by it (ObjectTemplate.hlsl OBJ_AD_GBUFFER, ParallaxTemplate.hlsl PAR_AD_GBUFFER). Every
		// other reader of z is unchanged by it: the fold reads it only with y 1 (and FoldHighlights gives the same flag), and the blended
		// mark tests (z 3 / -1: CapturedNormals::CoverageBegin) read 0 and 1 alike.
		inline void ForDraw(IDirect3DDevice9* device, const NiGeometry* geometry, float fold = 0.0f, float vertexColor = 0.0f) {
			// (fd-batchE: no early return when SpecularFade and FoldHighlights are both compiled out: the deferred AD records still read z (x 1 = full
			// highlight, y 0 = no fold: what those shaders read with the two compiled out); the upload happens only when a value changes)
			const BSShaderProperty* property = LitProperty(geometry);
			if (fold <= 0.0f) vertexColor = property && (property->ulFlags[1] & BSShaderProperty::Vertex_Colors) ? 1.0f : 0.0f;
			const float fade = Compiled && On ? GameSpecularFade(property) : 1.0f;
			if (fade == Sent && fold == SentFold && vertexColor == SentVc && MaterialW == SentW) return;
			Sent = fade; SentFold = fold; SentVc = vertexColor; SentW = MaterialW;
			const float value[4] = { fade, fold, vertexColor, MaterialW };
			device->SetPixelShaderConstantF(211, value, 1);
		}
		// UNOFFICIAL 2026-10-31 MaterialBuffer (Render.cpp MaterialForPass): the draw's class and radius in w, x-z kept as last sent (ForDraw
		// sends them again when they change; until the frame's first, x 1 = full highlight, y z 0 = as without those switches).
		inline void SendMaterial(IDirect3DDevice9* device, float w) {
			MaterialW = w;
			if (w == SentW && Sent >= 0.0f) return;
			SentW = w;
			const float value[4] = { Sent >= 0.0f ? Sent : 1.0f, Sent >= 0.0f ? SentFold : 0.0f, Sent >= 0.0f ? SentVc : 0.0f, w };
			device->SetPixelShaderConstantF(211, value, 1);
		}
	}

	// ---- 2. SpecularLODPasses ([Main.Main.Misc] SpecularLODPasses, live) ----
	namespace LodPasses {
		constexpr UInt32 kSites[3] = { 0x00BE00BD, 0x00BE00D2, 0x00BE00EB };
		constexpr UInt8 kOld[5] = { 0x80, 0x7C, 0x24, 0x28, 0x00 };	// CMP byte ptr [ESP+0x28],0 (local_64)
		constexpr UInt8 kNew[5] = { 0xF6, 0x46, 0x20, 0x01, 0x90 };	// TEST byte ptr [ESI+0x20],1 ; NOP (the specular flag itself)
		constexpr UInt32 kRebuildBit = 0x20000000;	// LampsInOnePass flips 0x10000000: a different bit, so two switches in one frame never cancel
		inline bool Applied = false, Refused = false;

		inline void BeginFrame() {
			const bool want = TheSettingManager->GetSettingI("Main.Main.Misc", "SpecularLODPasses") != 0;
			if (want == Applied || Refused) return;
			const UInt8* expect = Applied ? kNew : kOld;
			for (UInt32 site : kSites)
				if (memcmp((const void*)site, expect, 5)) {
					Logger::Log("UNOFFICIAL SpecularLODPasses: the game's code at %08X is not as expected (another plugin?); not changed", site);
					Refused = true;
					return;
				}
			for (UInt32 site : kSites) SafeWriteBuf(site, want ? kNew : kOld, 5);
			*(UInt32*)kEnabledPasses ^= kRebuildBit;	// new pass-list key: every object rebuilds its passes this frame
			Applied = want;
			Logger::Log("UNOFFICIAL SpecularLODPasses: %s (every object's passes rebuilt)", want ?
				"on (shiny objects keep the same set of lighting passes on both sides of the highlight distance)" : "off (the game's choice)");
		}
	}

	// ---- 3. ActorsInOnePass ([Main.Main.Misc], live, default off) ----
	// An object whose lamp count crosses 2 <-> 3 swaps between the one exact many-lamp pass (with metal) and the multi-pass set (summed in
	// the stored curve, no metal): it pops in brightness and loses or gains its metal. Static objects keep their lamps; MOVING ones do not:
	// NPCs walking through a lit room and the player's own body and first-person model (armour metal coming and going was the 2026-10-08
	// report). On: skinned objects (actors, the player's body and arms) and everything drawn in the first-person pass keep the one pass up
	// to 5 + f lamps (the 6 light slots MoreLampsPerObject gives the shader), as LampsInOnePass does for everything (measured +1.1 ms in the
	// Ultra-Luxe for all objects; this is the moving few). Code cave on GetRenderPasses_2x's specular capacity test: CMP ECX,EAX ;
	// JG 0x00BE017B at 0x00BE00E3 (3B C8 0F 8F 90 00 00 00) -> JMP to the cave + 3 NOPs. In the cave, when the lamps exceed the cap: the
	// geometry (param_1, [ESP+0x90]) skinned (+0xBC skin instance) or the first-person pass running -> cap f + 5 (f = local_38,
	// [ESP+0x54]); the following tests (specular, FaceGen) run as before, so only specular non-FaceGen objects change.
	namespace ActorPasses {
		inline UInt8 On = 0, FirstPersonNow = 0;
		inline UInt32 Widened = 0;
		// UNOFFICIAL 2026-11-14 fd-fixes (DeferredRenderer on native D3D9; docs/next/FD-FIXES-RESULTS.md item 5): the same cave also gives
		// the objects that may draw FORWARD while the renderer's 23 + f compare is on (LampsInOnePass) the forward shader's own limit back:
		// 5 + f with MoreLampsPerObject (the game's cap 6 + f = the shader's 6 slots: no lamp lost), else the game's 2 + f. Past it they are
		// drawn in several passes, which light every lamp (a deferred base with its lamp passes skipped, or a forward base with its lamp
		// passes). Which objects: the first-person model (FirstPersonNow), and every object seen drawing forward with lamps lost (the game's
		// cap or the shader's slots: DeferredRenderer::AfterDraw -> Keep), learned per geometry; a new one asks for one pass rebuild (at most
		// every kRebuildGap frames). The compare is read only when an object's pass list is built (registration), so this is the one place
		// that can choose per object. Off (OnePassLimit::On 0): the cave runs as before.
		// (the set, the switch and the counters: OnePassLimit.h, shared with DeferredRenderer.h)
		inline UInt32 __cdecl DeferredLimit(const void* geometry, UInt32 f, UInt32 limitNow, UInt32 lamps) {
			// (2026-11-15) the material's emittance x its multiplier (luma), only where OnePassLimit may hold the object for it
			float emission = 0.0f;
			if (OnePassLimit::HoldEmissive && geometry && lamps > f + OnePassLimit::ReferenceImm) {
				const NiMaterialProperty* material = ((const NiGeometry*)geometry)->propertyState.m_spMaterialProperty;
				if (material && material->fEmitMult > 0.0f)
					emission = (0.2126f * material->emit.r + 0.7152f * material->emit.g + 0.0722f * material->emit.b) * material->fEmitMult;
			}
			return OnePassLimit::Limit(geometry, f, limitNow, lamps, FirstPersonNow != 0, emission);
		}
		constexpr UInt32 kSite = 0x00BE00E3;
		constexpr UInt8 kOld[8] = { 0x3B, 0xC8, 0x0F, 0x8F, 0x90, 0x00, 0x00, 0x00 };
		constexpr UInt32 kRebuildBit = 0x08000000;
		inline bool Installed = false, Refused = false;
	}
}
static UInt8* PbrPopActorsOnFlag = &PbrPop::ActorPasses::On;
static UInt8* PbrPopFirstPersonFlag = &PbrPop::ActorPasses::FirstPersonNow;
static UInt32* PbrPopWidenedCount = &PbrPop::ActorPasses::Widened;
static UInt8* PbrPopDeferredLimitFlag = &OnePassLimit::On;
static void* PbrPopDeferredLimitFn = (void*)&PbrPop::ActorPasses::DeferredLimit;
static __declspec(naked) void PbrPopActorCave() {
	__asm {
		// UNOFFICIAL 2026-11-14 fd-fixes: the deferred renderer's per-object limit first (EAX = the limit, ECX = the lamps)
		push edx
		mov edx, PbrPopDeferredLimitFlag
		cmp byte ptr [edx], 0
		pop edx
		je actors
		pushad								// EAX at [esp+28], ECX at [esp+24]; entry offsets + 32
		push dword ptr [esp + 24]			// lamps
		push dword ptr [esp + 32]			// the limit now (EAX)
		push dword ptr [esp + 0x7C]			// f (local_38: [ESP+0x54] at entry + 32 + 8)
		push dword ptr [esp + 0xBC]			// the geometry (param_1: [ESP+0x90] at entry + 32 + 12)
		mov eax, PbrPopDeferredLimitFn
		call eax
		add esp, 16
		mov dword ptr [esp + 28], eax		// EAX after popad: the limit to compare with
		popad
	actors:
		cmp ecx, eax
		jle fits
		push edx
		mov edx, PbrPopActorsOnFlag
		cmp byte ptr [edx], 0
		je multi_pop
		mov edx, dword ptr [esp + 0x94]		// the geometry (param_1; [ESP+0x90] before the push)
		cmp dword ptr [edx + 0xBC], 0		// skinned
		jne wide
		mov edx, PbrPopFirstPersonFlag
		cmp byte ptr [edx], 0
		jne wide
	multi_pop:
		pop edx
		jmp multi
	wide:
		mov edx, dword ptr [esp + 0x58]		// f (local_38; [ESP+0x54] before the push)
		add edx, 5
		cmp ecx, edx
		jg multi_pop
		mov edx, PbrPopWidenedCount
		inc dword ptr [edx]
		pop edx
	fits:
		push 0x00BE00EB
		ret
	multi:
		push 0x00BE017B
		ret
	}
}
namespace PbrPop {
	namespace ActorPasses {
		inline void BeginFrame() {
			const bool want = TheSettingManager->GetSettingI("Main.Main.Misc", "ActorsInOnePass") != 0;
			// UNOFFICIAL 2026-11-14 fd-fixes: the cave also serves the deferred renderer's per-object limit (OnePassLimit::On, set by
			// LampsInOnePass::BeginFrame this frame, before this); a learned object asks for one rebuild, at most every kRebuildGap frames
			if (OnePassLimit::On && !Installed && !OnePassLimit::Refused) {
				if (memcmp((const void*)kSite, kOld, sizeof(kOld))) {
					Logger::Log("[WARNING] UNOFFICIAL DeferredRenderer: the game's code at %08X is not as expected (another plugin?): forward objects keep the 23 + f compare (lamps lost, counted)", kSite);
					OnePassLimit::Refused = true;
				}
				else {
					WriteRelJump(kSite, (UInt32)&PbrPopActorCave);
					SafeWrite8(kSite + 5, 0x90); SafeWrite8(kSite + 6, 0x90); SafeWrite8(kSite + 7, 0x90);
					Installed = true;
					Logger::Log("UNOFFICIAL DeferredRenderer: the per-object one-pass limit is in (forward objects keep %u + f lamps in one pass)", (unsigned)OnePassLimit::ForwardImm);
				}
			}
			OnePassLimit::CaveIn = Installed;
			if (Installed && OnePassLimit::RebuildDue()) {
				*(UInt32*)kEnabledPasses ^= kRebuildBit;	// every object's passes rebuilt: the learned ones take the forward limit
			}
			if (want == (On != 0) || Refused) return;
			if (want && !Installed) {
				if (memcmp((const void*)kSite, kOld, sizeof(kOld))) {
					Logger::Log("UNOFFICIAL ActorsInOnePass: the game's code at %08X is not as expected (another plugin?); not changed", kSite);
					Refused = true;
					return;
				}
				if (*(const UInt8*)0x00BDEA3F != 0x05) {	// MoreLampsPerObject's byte in AddPass_Opt (see LampsInOnePass.h)
					Logger::Log("UNOFFICIAL ActorsInOnePass: needs MoreLampsPerObject (the game's cap of 3 would drop lamps); not changed");
					Refused = true;
					return;
				}
				WriteRelJump(kSite, (UInt32)&PbrPopActorCave);
				SafeWrite8(kSite + 5, 0x90); SafeWrite8(kSite + 6, 0x90); SafeWrite8(kSite + 7, 0x90);
				Installed = true;
			}
			On = want ? 1 : 0;
			*(UInt32*)kEnabledPasses ^= kRebuildBit;
			Logger::Log("UNOFFICIAL ActorsInOnePass: %s (every object's passes rebuilt)", want ?
				"on (actors and the first-person model keep one lighting pass up to 5 lamps)" : "off (the game's choice)");
		}
	}

	// ---- 4. SpecularDistance ([Shaders.PBR.Main] SpecularDistance, live; UNOFFICIAL 2026-10-27) ----
	// User: "why pbr pops in almost selectively on meshes and pops in at a different range depending on first or third person? can we
	// just apply pbr to these meshes all the time instead of it turning on and off?" Beyond the game's highlight distance
	// (fSpecularLODEnd, 2300 units, measured from the CAMERA: in third person that line lies metres nearer the player than in first)
	// the game draws every shiny object with its plain shaders and no highlight passes. NVR's PBR (highlights, metal, the rmaos maps'
	// shine) is in the shiny family only, so it switched off at that line -- only on meshes with a specular map, hence "selectively".
	// This sets that distance (fSpecularLODEnd = the setting, fSpecularLODStartFade = it minus the game's own fade range) and keeps it
	// there every frame (the game's Specularity Fade slider rewrites both); every object rebuilds its passes when it changes. 0 = the
	// game's own distance. (The test tool below, PbrPopSpecLODEnd, writes the same two values when it is set.)
	namespace SpecDistance {
		inline float OrigStart = 0.0f, OrigEnd = 0.0f, Target = 0.0f;
		inline bool Saved = false;
		constexpr UInt32 kRebuildBit = 0x04000000;	// read nowhere else (all readers mask 0x10, 0x20 or 0xF; see LampsInOnePass.h)
		inline void BeginFrame() {
			float& start = *BSShaderManager::fSpecularLODStartFade;
			float& end = *BSShaderManager::fSpecularLODEnd;
			if (!Saved) { OrigStart = start; OrigEnd = end; Saved = true; }
			const float want = (std::max)(TheSettingManager->GetSettingF("Shaders.PBR.Main", "SpecularDistance"), 0.0f);
			if (want <= 0.0f) {
				if (Target > 0.0f) {	// switched back to the game's own
					start = OrigStart; end = OrigEnd; Target = 0.0f;
					*(UInt32*)kEnabledPasses ^= kRebuildBit;
					Logger::Log("UNOFFICIAL SpecularDistance: the game's own highlight distance %.0f-%.0f (every object's passes rebuilt)", start, end);
				}
				return;
			}
			const float wantStart = (std::max)(want - (OrigEnd - OrigStart), 1.0f);
			if (start == wantStart && end == want) return;
			const bool announce = want != Target;
			start = wantStart; end = want; Target = want;
			*(UInt32*)kEnabledPasses ^= kRebuildBit;
			if (announce)
				Logger::Log("UNOFFICIAL SpecularDistance: objects keep their full PBR shading (highlights, metal) out to %.0f units (the game's own: "
					"%.0f); every object's passes rebuilt", want, OrigEnd);
		}
	}

#if NVR_TEST_TOOLS
	// ---- TEST: move the game's highlight distance in place ([Main.Main.Misc] PbrPopSpecLODEnd, 0 = the game's own) ----
	// fSpecularLODEnd = the value, fSpecularLODStartFade = the value minus the game's own fade range; every object rebuilds its passes, so
	// a still camera sees what the objects crossing that distance do (the pop without camera motion).
	namespace SpecLodTest {
		inline float Current = 0.0f, OrigStart = 0.0f, OrigEnd = 0.0f;
		inline bool Saved = false;
		constexpr UInt32 kRebuildBit = 0x40000000;
		inline void BeginFrame() {
			const float want = (std::max)(TheSettingManager->GetSettingF("Main.Main.Misc", "PbrPopSpecLODEnd"), 0.0f);
			if (want == Current) return;
			if (!Saved) { OrigStart = *BSShaderManager::fSpecularLODStartFade; OrigEnd = *BSShaderManager::fSpecularLODEnd; Saved = true; }
			if (want > 0.0f) {
				*BSShaderManager::fSpecularLODEnd = want;
				*BSShaderManager::fSpecularLODStartFade = (std::max)(want - (OrigEnd - OrigStart), 1.0f);
			}
			else { *BSShaderManager::fSpecularLODStartFade = OrigStart; *BSShaderManager::fSpecularLODEnd = OrigEnd; }
			*(UInt32*)kEnabledPasses ^= kRebuildBit;
			Current = want;
			Logger::Log("PBR POP TEST: highlight distance %.0f-%.0f (game's own %.0f-%.0f); every object's passes rebuilt",
				*BSShaderManager::fSpecularLODStartFade, *BSShaderManager::fSpecularLODEnd, OrigStart, OrigEnd);
		}
	}

	// ---- TEST diagnostic: which lit objects change their set of lighting passes from one frame to the next ([Main.Main.Misc] PbrPopLog) ----
	// Every lit object pass (world and first person) is noted by geometry; at the end of each frame each object drawn in this frame AND
	// the one before is compared: a different set of pass kinds (the pass enums, by the game's names) is a switch, logged with the
	// object's name, its distance (the property's +0x34, what the game's distance cuts use), its highlight fade, its lamps, and where it
	// is on the screen (its bound sphere projected; u/v 0-1 across the picture) -- so a picture strip can be checked at that place.
	namespace PopLog {
		constexpr unsigned kSlots = 16384, kEnums = 6;
		struct Slot {
			const NiGeometry* key = nullptr;
			unsigned frame = 0;		// frame of cur
			bool prevValid = false;
			UInt8 nCur = 0, nPrev = 0;
			UInt16 cur[kEnums] = {}, prev[kEnums] = {};
			UInt8 context = 0;
		};
		inline Slot* Table = nullptr;
		inline unsigned FrameId = 1, Lines = 0, Frames = 0, Enabled = 0;
		inline unsigned long long Switches = 0;

		inline bool IsTexture(UInt16 e) { return e >= 0x14A && e <= 0x150; }
		inline bool IsOpt(UInt16 e) { return e >= 0xC3 && e <= 0xD6; }
		inline bool HasTexture(const UInt16* e, unsigned n) { for (unsigned i = 0; i < n; i++) if (IsTexture(e[i])) return true; return false; }
		inline bool HasHighlight(const UInt16* e, unsigned n) { for (unsigned i = 0; i < n; i++) if (LampsInOnePass::Census::IsHighlightPass(e[i])) return true; return false; }
		inline bool SameSet(const Slot& s) {
			if (s.nCur != s.nPrev) return false;
			for (unsigned i = 0; i < s.nCur; i++) { bool found = false; for (unsigned j = 0; j < s.nPrev; j++) if (s.cur[i] == s.prev[j]) found = true; if (!found) return false; }
			return true;
		}
		inline std::string Names(const UInt16* e, unsigned n) {
			std::string text;
			for (unsigned i = 0; i < n; i++) {
				char one[96];
				const char* name = Pointers::Functions::GetPassDescription(e[i]);
				sprintf_s(one, "%s%03X %s", i ? ", " : "", e[i], name ? name : "?");
				text += one;
			}
			return text;
		}

		// Where the object is on the screen: its world bound projected (u, v across the picture, 0-1; radius as a share of the width).
		inline bool ScreenOf(const NiGeometry* geometry, float& u, float& v, float& ru) {
			const NiBound* bound = geometry ? geometry->m_kWorldBound : nullptr;
			if (!bound) return false;
			const D3DXMATRIX& m = TheRenderManager->ViewProjMatrix;
			const float x = bound->Center.x, y = bound->Center.y, z = bound->Center.z;
			const float cx = x * m._11 + y * m._21 + z * m._31 + m._41;
			const float cy = x * m._12 + y * m._22 + z * m._32 + m._42;
			const float cw = x * m._14 + y * m._24 + z * m._34 + m._44;
			if (cw <= 1.0f) return false;
			u = cx / cw * 0.5f + 0.5f;
			v = 0.5f - cy / cw * 0.5f;
			const float projX = TheRenderManager->InvProjMatrix._11 != 0.0f ? 1.0f / TheRenderManager->InvProjMatrix._11 : 1.0f;
			ru = bound->Radius * projX / cw * 0.5f;
			return true;
		}

		inline void Note(const PointShadowForward::RenderPassView* pass, UInt8 context) {
			if (!Enabled || !pass || !pass->geometry || !LitProperty(pass->geometry)) return;
			if (!Table) { Table = new (std::nothrow) Slot[kSlots]; if (!Table) { Enabled = 0; return; } }
			unsigned h = (unsigned)(((uintptr_t)pass->geometry >> 4) * 2654435761u) & (kSlots - 1);
			Slot* s = nullptr;
			Slot* spare = nullptr;	// the first free slot, or one not drawn for 2 frames
			for (unsigned probe = 0; probe < 64; probe++, h = (h + 1) & (kSlots - 1)) {
				Slot& c = Table[h];
				if (c.key == pass->geometry) { s = &c; break; }
				if (!spare && (!c.key || c.frame + 2 < FrameId)) spare = &c;
				if (!c.key) break;	// the key would have been before the first never-used slot
			}
			if (!s) { if (!spare) return; *spare = Slot(); spare->key = pass->geometry; s = spare; }
			if (s->frame != FrameId) {
				s->prevValid = s->frame && s->frame + 1 == FrameId;
				memcpy(s->prev, s->cur, sizeof(s->cur)); s->nPrev = s->nCur;
				s->nCur = 0; s->frame = FrameId; s->context = context;
			}
			const UInt16 e = pass->passEnum;
			for (unsigned i = 0; i < s->nCur; i++) if (s->cur[i] == e) return;
			if (s->nCur < kEnums) s->cur[s->nCur++] = e;
		}

		// Start of a frame: the frame before is complete; compare its objects with the frame before that.
		inline void NewFrame() {
			Enabled = (unsigned)(std::max)(TheSettingManager->GetSettingI("Main.Main.Misc", "PbrPopLog"), 0);
			if (!Enabled || !Table) { FrameId++; return; }
			// 2026-10-28 (user's crash at a loading screen, PbrPop.h:348): the objects noted last frame can be deleted before this runs (a
			// loading screen, a cell change), and their names were read here. In menus and on a cell change everything noted is forgotten.
			if (TheShaderManager->GameState.isCellChanged || (InterfaceManager && InterfaceManager->currentMode != 1)) {
				for (unsigned i = 0; i < kSlots; i++) Table[i] = Slot();
				FrameId++;
				return;
			}
			const unsigned done = FrameId;
			unsigned count = 0, oneToMulti = 0, multiToOne = 0, toPlain = 0, toShiny = 0, other = 0, detail = 0;
			const int shot = SettingShots::Running ? (int)SettingShots::Index : -1;
			const int shotFrame = SettingShots::Running ? SettingShots::Frame : -1;
			for (unsigned i = 0; i < kSlots; i++) {
				Slot& s = Table[i];
				if (!s.key || s.frame != done || !s.prevValid || SameSet(s)) continue;
				count++;
				const bool texWas = HasTexture(s.prev, s.nPrev), texNow = HasTexture(s.cur, s.nCur);
				const bool hlWas = HasHighlight(s.prev, s.nPrev), hlNow = HasHighlight(s.cur, s.nCur);
				const char* kind = "other";
				if (!texWas && texNow) { oneToMulti++; kind = "one->multi"; }
				else if (texWas && !texNow) { multiToOne++; kind = "multi->one"; }
				else if (hlWas && !hlNow) { toPlain++; kind = "shiny->plain"; }
				else if (!hlWas && hlNow) { toShiny++; kind = "plain->shiny"; }
				else other++;
				if (detail < 24 && Lines < 6000) {
					detail++; Lines++;
					const BSShaderProperty* property = LitProperty(s.key);
					float u = -1.0f, v = -1.0f, ru = 0.0f;
					ScreenOf(s.key, u, v, ru);
					const NiAVObject* parent = (const NiAVObject*)s.key->m_parent;
					Logger::Log("PBR POP   %s %s '%s' (in '%s') dist %.0f fade %.2f lamps %u flags %08X screen u %.3f v %.3f r %.3f | %s -> %s",
						s.context ? "1st" : "world", kind, s.key->m_pcName ? s.key->m_pcName : "", parent && parent->m_pcName ? parent->m_pcName : "",
						property ? *(const float*)((const UInt8*)property + 0x34) : -1.0f, GameSpecularFade(property),
						property ? LampsInOnePass::Census::CountLamps(property) : 0u, property ? property->ulFlags[0] : 0u, u, v, ru,
						Names(s.prev, s.nPrev).c_str(), Names(s.cur, s.nCur).c_str());
				}
			}
			Switches += count;
			if (count && Lines < 6000) {
				Lines++;
				Logger::Log("PBR POP frame %u (shot %d frame %d): %u objects changed their passes: one->multi %u, multi->one %u, shiny->plain %u, plain->shiny %u, other %u",
					done, shot, shotFrame, count, oneToMulti, multiToOne, toPlain, toShiny, other);
			}
			if (++Frames % 600 == 0) Logger::Log("PBR POP: %u frames, %llu pass-set switches so far; ActorsInOnePass kept %u objects in one pass so far", Frames, Switches, ActorPasses::Widened);
			FrameId++;
		}
	}
#endif
}
