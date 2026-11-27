#pragma once
#include <d3dx9.h>
#include "TestTools.h"
#include "LampsInOnePass.h"

// UNOFFICIAL 2026-10-21 DIAGNOSTIC (development builds only), [Main.Main.Misc] PassFamilyView, live: which family of lighting
// passes draws each lit object (user report 2026-10-21: "pbr seems to apply more to some tiles even though they're supposed
// to be the same type of tile", Lucky 38 floor; bench\deep-roadmap\evidence\pbrtiles\LOG.md).
//
// The game draws a shiny object lit by up to 2 lamps in ONE pass (NVR's full PBR shaders, SLS2000-2036, the lamps summed in
// linear light), and one lit by 3 or more in SEVERAL: a light-only pass (SLS2037-2044), extra lamp passes (SLS2045/2046), a
// texture pass that multiplies, highlight passes (SLS2047-2056) and a fog pass, each added in the stored curve. Two identical
// floor pieces, one reached by 2 lamps and its neighbour by 3, take different families.
// On, in the world scene, every lit object is drawn in one flat colour (and nothing else): its FIRST pass draws the colour, the
// added passes (extra lamps, texture, highlights, fog) are skipped (any added pass that still binds an added-pass shader draws
// black: it blends ONE+ONE). Colours: one pass with 0-1 lamps blue, 2 lamps green, 3+ cyan; several passes with 3 lamps yellow,
// 4 orange, 5 red, 6+ magenta; other NVR shaders (parallax, skin, ...) grey; vanilla shaders are left as they are. Each piece
// takes one of 4 shades (from its address), so neighbouring pieces of one family can still be told apart.
// The frame the view has been on for 6 frames, every object it colours is logged once ('PASS FAMILY' lines: name, parent,
// family, lamps, pass, where on the screen), up to 1200 lines per switch-on.
// The PS is put back to the game's own (its render-state cache) after every replaced draw, so the game's state tracking is
// never wrong.
#if NVR_TEST_TOOLS
namespace PassFamilyView {

	enum Bind { BindNone = 0, BindOne, BindMulti, BindAdded, BindOther };
	constexpr int kColours = 9, kShades = 4;
	inline const float kColour[kColours][3] = {
		{ 0.00f, 0.00f, 0.00f },	// 0 added pass: adds nothing
		{ 0.12f, 0.30f, 1.00f },	// 1 one pass, 0-1 lamps: blue
		{ 0.10f, 0.85f, 0.15f },	// 2 one pass, 2 lamps: green
		{ 0.05f, 0.85f, 0.85f },	// 3 one pass, 3+ lamps: cyan
		{ 1.00f, 0.90f, 0.05f },	// 4 several passes, 3 lamps (or fewer): yellow
		{ 1.00f, 0.45f, 0.02f },	// 5 several passes, 4 lamps: orange
		{ 1.00f, 0.06f, 0.04f },	// 6 several passes, 5 lamps: red
		{ 0.95f, 0.08f, 0.90f },	// 7 several passes, 6+ lamps: magenta
		{ 0.45f, 0.45f, 0.45f },	// 8 other NVR shaders: grey
	};
	inline const float kShade[kShades] = { 0.55f, 0.70f, 0.85f, 1.00f };

	inline int Mode = 0, OnFrames = 0;
	inline bool Failed = false, Replaced = false;
	inline IDirect3DPixelShader9* Shader[kColours][kShades] = {};
	inline Bind LastBind = BindNone;
	inline UInt16 LastBindPass = 0xFFFF;
	inline const NiGeometry* Geometry = nullptr;
	inline unsigned Lamps = 0;
	inline UInt16 PassEnum = 0;
	inline unsigned LogLines = 0;
	constexpr unsigned kLogged = 4096;
	inline const void* Logged[kLogged] = {};
	inline unsigned LoggedCount = 0;
	inline unsigned Counts[kColours] = {};

	inline Bind Classify(const char* ps, bool nvr) {
		if (!nvr || !ps) return BindNone;
		if (strncmp(ps, "SLS", 3)) return BindOther;
		const int n = atoi(ps + 3);
		if (n >= 2000 && n <= 2036) return BindOne;
		if (n >= 2037 && n <= 2044) return BindMulti;
		if (n >= 2045 && n <= 2056) return BindAdded;
		return BindOther;
	}

	inline int ColourOf(Bind bind, unsigned lamps) {
		switch (bind) {
		case BindAdded: return 0;
		case BindOne: return lamps <= 1 ? 1 : lamps == 2 ? 2 : 3;
		case BindMulti: return lamps <= 3 ? 4 : lamps == 4 ? 5 : lamps == 5 ? 6 : 7;
		default: return 8;
		}
	}

	inline bool Make(IDirect3DDevice9* device) {
		if (Shader[0][0]) return true;
		if (Failed) return false;
		for (int c = 0; c < kColours; c++) for (int s = 0; s < kShades; s++) {
			char src[256];
			const float k = c ? kShade[s] : 0.0f;
			sprintf_s(src, "float4 main() : COLOR0 { return float4(%.4f, %.4f, %.4f, 1.0); }", kColour[c][0] * k, kColour[c][1] * k, kColour[c][2] * k);
			ID3DXBuffer* code = nullptr; ID3DXBuffer* errors = nullptr;
			if (FAILED(D3DXCompileShader(src, (UINT)strlen(src), NULL, NULL, "main", "ps_3_0", 0, &code, &errors, NULL)) || !code ||
				FAILED(device->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &Shader[c][s]))) {
				Logger::Log("PassFamilyView: shader failed: %s", errors ? (const char*)errors->GetBufferPointer() : "?");
				if (errors) errors->Release();
				if (code) code->Release();
				Failed = true;
				return false;
			}
			code->Release();
		}
		Logger::Log("PassFamilyView: shaders made");
		return true;
	}

	inline void BeginFrame() {
		const int want = TheSettingManager->GetSettingI("Main.Main.Misc", "PassFamilyView");
		if (want && !Mode) { OnFrames = 0; LogLines = 0; }
		if (Mode && OnFrames == 6) {
			Logger::Log("PASS FAMILY frame: objects coloured: one pass 0-1 lamps %u, 2 lamps %u, 3+ lamps %u | several passes 3 lamps %u, 4 lamps %u, 5 lamps %u, 6+ lamps %u | other shaders of Vegas Shaders %u",
				Counts[1], Counts[2], Counts[3], Counts[4], Counts[5], Counts[6], Counts[7], Counts[8]);
		}
		Mode = want;
		if (Mode) OnFrames++;
		LoggedCount = 0;
		memset(Counts, 0, sizeof(Counts));
		LastBind = BindNone;
	}

	inline bool LogFrame() { return OnFrames == 6; }

	inline void Apply(IDirect3DDevice9* device) {
		if (LastBind == BindNone || !Make(device)) return;
		const int c = ColourOf(LastBind, Lamps);
		const unsigned s = (unsigned)((((uintptr_t)Geometry >> 4) * 2654435761u) >> 30);
		device->SetPixelShader(Shader[c][s & 3]);
		Replaced = true;
	}

	// SetShadersHook, after the game bound a pass's shaders. The game binds once for a run of passes of one kind, BEFORE the run's
	// draws (run 20261021-014725: nothing was coloured when this waited for a draw in flight), so the bind is noted here and the
	// colour put on in BeforeDraw for every pass of that kind; a bind made inside a hooked draw is coloured at once.
	inline bool InPass = false;
	inline void OnBind(IDirect3DDevice9* device, const char* ps, bool nvr, bool world, UInt32 passIndex) {
		if (!Mode || !world) { LastBind = BindNone; return; }
		LastBind = Classify(ps, nvr);
		LastBindPass = (UInt16)passIndex;
		if (InPass) Apply(device);
	}

	// Early in the pass hooks: true = skip this pass (the added passes of a several-pass object).
	inline bool Skip(const void* passPtr, bool world) {
		if (!Mode || !world || !passPtr) return false;
		const UInt16 e = ((const PointShadowForward::RenderPassView*)passPtr)->passEnum;
		return LampsInOnePass::Census::IsLampPass(e) || LampsInOnePass::Census::IsHighlightPass(e) || (e >= 0x14A && e <= 0x153) ||
			(e >= 0x248 && e <= 0x256);
	}

	// Right before the game draws the pass (nothing can skip it after this): note the object, colour it if no new bind follows.
	inline void BeforeDraw(IDirect3DDevice9* device, const void* passPtr, bool world) {
		if (!Mode || !world || !passPtr) return;
		const PointShadowForward::RenderPassView* pass = (const PointShadowForward::RenderPassView*)passPtr;
		InPass = true;
		Geometry = pass->geometry;
		PassEnum = pass->passEnum;
		const BSShaderProperty* property = Geometry ? (const BSShaderProperty*)Geometry->propertyState.m_spShadeProperty : nullptr;
		const UInt32 vtable = property ? *(const UInt32*)property : 0;
		Lamps = vtable == 0x10AE0D0 || vtable == 0x10B9910 ? LampsInOnePass::Census::CountLamps(property) : 0;
		if (LastBind != BindNone && PassEnum == LastBindPass) Apply(device);	// a pass drawn without a new bind (a run of one pass kind)
	}

	// After the draw: the game's own pixel shader back on the device, and the census line.
	inline void After(IDirect3DDevice9* device, bool world) {
		InPass = false;
		if (!Replaced) return;
		Replaced = false;
		device->SetPixelShader(TheRenderManager->renderState->GetPixelShader());
		if (!world || !Geometry) return;
		const int c = ColourOf(LastBind, Lamps);
		for (unsigned i = 0; i < LoggedCount; i++) if (Logged[i] == Geometry) return;
		if (LoggedCount < kLogged) Logged[LoggedCount++] = Geometry;
		Counts[c]++;
		if (!LogFrame() || LogLines >= 1200 || c == 0) return;
		LogLines++;
		static const char* kName[kColours] = { "added", "one-pass(0-1)", "one-pass(2)", "one-pass(3+)", "multi(3)", "multi(4)", "multi(5)", "multi(6+)", "other" };
		float u = -1.0f, v = -1.0f, ru = 0.0f;
		PbrPop::PopLog::ScreenOf(Geometry, u, v, ru);
		const NiAVObject* parent = (const NiAVObject*)Geometry->m_parent;
		Logger::Log("PASS FAMILY %s lamps %u pass %03X '%s' (in '%s') screen u %.3f v %.3f r %.3f", kName[c], Lamps, (unsigned)PassEnum,
			Geometry->m_pcName ? Geometry->m_pcName : "", parent && parent->m_pcName ? parent->m_pcName : "", u, v, ru);
	}
}
#endif
