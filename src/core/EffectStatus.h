#pragma once

#include <string>
#include <vector>

// UNOFFICIAL 2026-11-01 why each effect did what it did, per frame (core/Diagnostics.cpp; docs/UNOFFICIAL-TECHNICAL.md "Diagnostics").
// A small registry the effects report into with one-line calls where they already decide; nothing is restructured. Kept in memory
// only (a 16-frame ring per effect, a few bytes each) and written to the log in three places:
//   - a FLASH dump (the flash detector) and a REPORT block (the report key): every effect's state that frame, what changed from the
//     frame before, the shader levels / fallbacks that are not the default, the shader cache and D3D failure counts;
//   - the 60 s HEARTBEAT: per effect how many frames it drew and skipped in the minute (and the last skip reason).
// Calls:
//   - EffectRecord::Render notes every effect it is called for (drawn, or skipped: off / not loaded / ShouldRender false), so every
//     effect is covered at that level; an effect whose Render override returns early notes its own reason (EFFECT_STATUS).
//   - EFFECT_HISTORY(name, "kept" / "reset: why") where an effect decides whether to reuse last frame's history;
//     SessionLog::HistoryReset notes it too.
//   - Level(what, chosen, isDefault): a shader level or a fallback chosen at load or later (FIT levels, X4505 retries, FSR / DLSS
//     failures). Only the non-default ones are printed.
//   - ShaderLoaded / D3DFail / TargetSize: the shader cache counters, a failed D3D / NVR DXVK call (count + first frame per site),
//     a render target made at another size than before.
// The strings passed must live for the session (literals, effect names): only their pointers are kept.

namespace EffectStatus {

	enum Result : unsigned char { NotCalled = 0, Drawn, Disabled, NotLoaded, ShouldRenderFalse, Skipped };
	constexpr unsigned Ring = 16;

	int		Register(const char* name);	// the slot of a name (looked up by text, registered once); -1 when the table is full
	void	Note(int slot, Result result, const char* why = nullptr);	// this frame's result (Drawn wins over a skip in the same frame)
	void	History(int slot, const char* what);	// this frame's history decision
	inline void Note(int& cache, const char* name, Result result, const char* why = nullptr) {
		if (cache < 0) cache = Register(name);
		if (cache >= 0) Note(cache, result, why);
	}
	inline void History(int& cache, const char* name, const char* what) {
		if (cache < 0) cache = Register(name);
		if (cache >= 0) History(cache, what);
	}
	// The common temporal decision (VolumetricLight, Reflections): which check dropped the history, or "kept".
	inline const char* HistoryWhy(bool temporal, bool haveHistory, bool drewLastFrame, bool usable) {
		return !temporal ? "temporal off" : !haveHistory ? "reset: no history yet" : !drewLastFrame ? "reset: not drawn last frame" :
			!usable ? "reset: camera cut / jump / render size (shared rule)" : "kept";
	}

	void	Level(const char* what, const char* chosen, bool isDefault);	// copied (may be called from the compile threads)
	enum ShaderEvent { CacheHit, Compiled, Precompiled, CompileFailed, FlowControlRetry };
	void	ShaderLoaded(ShaderEvent what);	// thread-safe counters (ShaderRecord::LoadShader, EffectRecord::LoadEffect)
	long	CompileFailureCount();
	std::vector<std::string> CompileFailureItems();
	void	D3DFail(const char* site, long hr);	// the first one of a site is logged; all counted
	void	TargetSize(const char* name, unsigned width, unsigned height);	// logged when a name is made at another size than before
}

#define EFFECT_STATUS(name, result, why) do { static int effectStatusSlot_ = -1; EffectStatus::Note(effectStatusSlot_, name, EffectStatus::result, why); } while (0)
#define EFFECT_HISTORY(name, what) do { static int effectStatusSlot_ = -1; EffectStatus::History(effectStatusSlot_, name, what); } while (0)
