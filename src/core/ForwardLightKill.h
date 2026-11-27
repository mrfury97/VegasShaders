#pragma once
// UNOFFICIAL 2026-11-24 fd-complete (docs/next/FD-COMPLETE.md section 0, the measure of done): [Main.Main.Misc] ForwardLightKill (test only,
// live, default 0). The owner's and Conor's definition of a complete deferred renderer: no opaque surface takes light from the game's forward
// lighting. This switch removes that lighting at its one source so a picture shows what still depends on it:
//  - BSShaderLightingProperty::SetLight1x2x 0x00B70820 (static, cdecl; xFNV BSShaderLightingProperty.cpp): light k of a pass -> its colour in
//    kShaderLightConstants[k + 1] 0x011FA0C0 (PSLightColor[k] of every ShadowLightShader pass: objects, land, skin, parallax, their lamp and
//    highlight passes). Slot 0 (the ambient) is left alone; .w (the lamp count / gloss some passes keep there) is left alone.
//  - Lighting30Shader::SetLight 0x00BB95E0 (static, cdecl; xFNV Lighting30Shader.cpp): light k -> kPSConstants[2k + 21] (hair SM3002 / 3003,
//    the decal shaders SM3005 / 3007 and the other 3.0 shaders' LightData).
// After the game wrote a colour, its rgb is set to 0 while the switch is on and the draw is in the world or first-person context (Render.cpp's
// ShaderSplit context, through GBufferCensus::ContextOf). The deferred passes do not read these constants: they take the lamps from the lamp
// table and the sun from the scene's directional light, so a fully deferred opaque surface looks the same with it on. Anything that changes
// (or goes dark) still uses forward light: the inventory's "forward lighting still runs" column, measured. Not covered: shaders that get their
// lights from Vegas Shaders' own constants (grass lamp lists, the leaves' lamp, the forward+ records' table), which read no game constant.
// Known reader of the game's constants inside the deferred path (expect it to change too, a finding): LandLight's c3 read-back (the land's
// and objects' sun colour as the game set it) that DeferredSun uses.
// Off (0): one settings read per call, then exactly the game's function.
#include "GBufferCensus.h"
#include "TestTools.h"

namespace ForwardLightKill {
	inline const UInt32 kSetLight1x2x = 0x00B70820;
	inline const UInt32 kSetLight30 = 0x00BB95E0;
	inline float* const kShaderLightConstants = (float*)0x011FA0C0;	// NiVector4[]

	struct Color4 { float r, g, b, a; };
	typedef void(__cdecl* SetLight1x2xFn)(UInt32 index, void* light, float darkness);
	typedef void(__cdecl* SetLight30Fn)(SInt32 offset, Color4 data, Color4 color);
	inline SetLight1x2xFn SetLight1x2x = (SetLight1x2xFn)kSetLight1x2x;
	inline SetLight30Fn SetLight30 = (SetLight30Fn)kSetLight30;

	inline bool Installed = false;
	inline unsigned long long StatZeroed = 0;

	inline bool Active() {
		static UInt32 frame = 0xFFFFFFFFu;
		static bool on = false;
		const UInt32 now = TheFrameRateManager ? TheFrameRateManager->FrameNumber : 0;
		if (now != frame) { frame = now; on = TheSettingManager && TheSettingManager->GetSettingI("Main.Main.Misc", "ForwardLightKill") != 0; }
		if (!on) return false;
		const int context = GBufferCensus::ContextOf ? GBufferCensus::ContextOf() : 0;
		return context == 0 || context == 2;	// world, first person
	}

	inline void __cdecl SetLight1x2xHook(UInt32 index, void* light, float darkness) {
		SetLight1x2x(index, light, darkness);
		if (index < 8 && Active()) {
			float* c = kShaderLightConstants + 4 * (index + 1);
			c[0] = c[1] = c[2] = 0.0f;
			StatZeroed++;
		}
	}
	inline void __cdecl SetLight30Hook(SInt32 offset, Color4 data, Color4 color) {
		if (Active()) { color.r = color.g = color.b = 0.0f; StatZeroed++; }
		SetLight30(offset, data, color);
	}

	// Hooks.cpp, inside AttachHooks' Detours transaction (test-tools builds only).
	inline void Attach() {
#if NVR_TEST_TOOLS
		NoteHookSite(kSetLight1x2x, 8, "SetLight1x2x");
		NoteHookSite(kSetLight30, 8, "SetLight30");
		const LONG a = DetourAttach(&(PVOID&)SetLight1x2x, (PVOID)&SetLight1x2xHook);
		const LONG b = DetourAttach(&(PVOID&)SetLight30, (PVOID)&SetLight30Hook);
		Installed = a == NO_ERROR && b == NO_ERROR;
		Logger::Log("UNOFFICIAL ForwardLightKill (test only, [Main.Main.Misc] ForwardLightKill): SetLight1x2x %s, Lighting30Shader::SetLight %s",
			a == NO_ERROR ? "detoured" : "NOT detoured", b == NO_ERROR ? "detoured" : "NOT detoured");
#endif
	}
}
