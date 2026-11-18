#pragma once
#include <cstring>
#include <cstdint>
// UNOFFICIAL 2026-11-14 fd-fixes (DeferredRenderer on native D3D9; docs/next/FD-FIXES-RESULTS.md item 5): the per-object one-pass limit.
// While the deferred renderer's 23 + f compare is on (LampsInOnePass) and ManyLamps does not feed the forward shaders (native), the
// objects that may draw FORWARD get the forward shader's own limit back in ActorsInOnePass's cave at 0x00BE00E3 (PbrPop.h): 5 + f with
// MoreLampsPerObject (the game's cap 6 + f = the shader's 6 slots: no lamp lost), else the game's 2 + f. Past it they are drawn in several
// passes, which light every lamp. Which objects: the first-person model, and every object seen drawing forward with lamps lost (the
// game's cap or the shader's slots: DeferredRenderer::AfterDraw -> Keep), learned per geometry; a new one asks for one pass rebuild (at
// most every kRebuildGap frames). The compare is read only when an object's pass list is built (registration), so the cave is the one
// place that can choose per object. Off (On 0): the cave runs as before. No dependencies (PbrPop.h and DeferredRenderer.h include it).
namespace OnePassLimit {
	inline unsigned char On = 0;			// LampsInOnePass::BeginFrame: the renderer's compare is on and ManyLamps does not feed the forward shaders
	inline unsigned char ForwardImm = 2;	// 5 with MoreLampsPerObject, else 2
	inline unsigned char ReferenceImm = 2;	// the forward reference's own compare (2, or 5 with LampsInOnePass)
	// UNOFFICIAL 2026-11-15 (bench batch 3, native parity): the game draws a shiny object in ONE pass (OPT, enums 0xC3-0xD6) only up to 2 + f
	// lamps, and an OPT pass gets the material's emittance ADDED TO ITS AMBIENT (SetupGeometryConstants 0x00B7BA70 -> AddEmitToAmbient
	// 0x00C03010), while the several-pass set (AD + lamp passes) does not (SetEmitConstant: only the glow shaders read it). So the renderer's
	// 23 + f compare made every emissive object with 3+ lamps glow by its emittance in the deferred picture only: native's "deferred
	// brighter" (forward = the game's several passes there; on the fork ManyLamps draws both sides in one pass, so it never showed).
	// DeferredRendererOnePass 1 (default): such objects (emittance x multiplier above EmissiveMin) keep the forward reference's compare,
	// ReferenceImm (2, or 5 with LampsInOnePass), and draw in several passes, deferred: their lamp passes are skipped as before.
	// 2 = every object one pass (batch 3), 0 = no compare (the game's own lists; the lamp passes still skipped).
	// 2026-11-15 (bench batch 4: holding them cost Vault 21 ~2.5 ms) EmitExact (DeferredRendererOnePass 1, native): the objects stay in one pass
	// and the emittance is matched per draw instead. The cave notes, per object, whether the forward reference (the game's own compare,
	// ReferenceImm + f) would draw it in several passes (ForwardMulti); LampsInOnePass::EmitForPass then makes the game's OPT constant setup
	// take SetEmitConstant (no emittance in the ambient, as the several-pass set) for those objects and AddEmitToAmbient for the others
	// (as their own one pass in forward). HoldEmissive is the fallback when the emittance code site could not be patched.
	inline unsigned char EmitExact = 0;		// LampsInOnePass::BeginFrame (the site patched and the mode 1)
	constexpr unsigned kMulti = 8192;
	inline const void* MultiKey[kMulti] = {};
	inline unsigned char MultiVal[kMulti] = {};
	inline unsigned MultiNoted = 0;
	inline unsigned EmitDraws = 0;		// (F10) one-pass draws given SetEmitConstant (their forward reference: several passes)
	inline unsigned char* MultiSlot(const void* geometry, bool insert) {
		unsigned h = (unsigned)(((uintptr_t)geometry >> 4) * 2654435761u) & (kMulti - 1);
		for (unsigned probe = 0; probe < 64; probe++, h = (h + 1) & (kMulti - 1)) {
			if (MultiKey[h] == geometry) return &MultiVal[h];
			if (!MultiKey[h]) { if (!insert) return nullptr; MultiKey[h] = geometry; MultiNoted++; return &MultiVal[h]; }
		}
		return nullptr;	// (full: such an object keeps the game's AddEmitToAmbient, as before the fix)
	}
	inline void NoteForward(const void* geometry, unsigned f, unsigned lamps) {
		if (!geometry) return;
		const bool multi = lamps > f + ReferenceImm;
		if (unsigned char* v = MultiSlot(geometry, multi)) *v = multi ? 1 : 0;
	}
	inline bool ForwardMulti(const void* geometry) { const unsigned char* v = geometry ? MultiSlot(geometry, false) : nullptr; return v && *v; }
	inline void ClearMulti() { memset(MultiKey, 0, sizeof(MultiKey)); memset(MultiVal, 0, sizeof(MultiVal)); MultiNoted = 0; }
	inline unsigned char HoldEmissive = 0;	// DeferredRenderer::BeginFrame
	inline unsigned HeldEmissive = 0;		// (F10)
	constexpr float EmissiveMin = 1.0f / 512.0f;
	inline bool CaveIn = false, Refused = false;
	inline unsigned Held = 0, Learned = 0, Rebuilds = 0;	// (F10, DeferredRenderer's line) held at the limit / learned / rebuilds
	constexpr unsigned kKeep = 8192, kRebuildGap = 8;
	inline const void* KeepKey[kKeep] = {};
	inline bool KeepFull = false, RebuildWanted = false;
	inline unsigned FramesSinceRebuild = 0;

	inline bool Kept(const void* geometry, bool insert) {
		if (!geometry) return false;
		unsigned h = (unsigned)(((uintptr_t)geometry >> 4) * 2654435761u) & (kKeep - 1);
		for (unsigned probe = 0; probe < 64; probe++, h = (h + 1) & (kKeep - 1)) {
			if (KeepKey[h] == geometry) return true;
			if (!KeepKey[h]) {
				if (!insert) return false;
				KeepKey[h] = geometry; Learned++; RebuildWanted = true;
				return true;
			}
		}
		KeepFull = true;	// (8192 objects: past any interior; the rest keep the 23 + f list and are counted by DeferredRenderer)
		return false;
	}
	inline void Keep(const void* geometry) { if (On && !KeepFull) Kept(geometry, true); }
	inline void Clear() { memset(KeepKey, 0, sizeof(KeepKey)); KeepFull = false; Learned = 0; RebuildWanted = false; }
	// Called by the cave (registration, the main thread): the limit (EAX) to compare the object's lamps with.
	inline unsigned Limit(const void* geometry, unsigned f, unsigned limitNow, unsigned lamps, bool firstPerson, float emission = 0.0f) {
		if (EmitExact) NoteForward(geometry, f, lamps);
		if (HoldEmissive && emission > EmissiveMin && lamps > f + ReferenceImm && f + ReferenceImm < limitNow) { HeldEmissive++; return f + ReferenceImm; }
		const unsigned forward = f + ForwardImm;
		if (lamps <= forward || forward >= limitNow) return limitNow;
		if (!firstPerson && !Kept(geometry, false)) return limitNow;
		Held++;
		return forward;
	}
	// Once a frame before the scene walk (PbrPop::ActorPasses::BeginFrame): true = flip the cave's rebuild bit now.
	inline bool RebuildDue() {
		FramesSinceRebuild++;
		if (!On || !RebuildWanted || FramesSinceRebuild < kRebuildGap) return false;
		RebuildWanted = false; FramesSinceRebuild = 0; Rebuilds++;
		return true;
	}
}
