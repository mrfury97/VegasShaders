#pragma once
// UNOFFICIAL 2026-11-16 fd-batchB (docs/next/FD-BATCH-B.md), TEST ONLY: forced faults for the deferred renderer's retry rules (the gaps audit's
// item 8), so a bench log can show that one bad frame no longer turns the renderer forward for the session and that a real failure still
// does (until the next cell change). Driven by [Main.Develop.Main] DebugVar4 (an existing internal key; no shader reads its w), live: a NEW
// value from 901 to 905 arms one fault, logged; 0 or any other value does nothing.
//   901  DeferredSun: one refused frame (a per-frame cause: the pass skips that frame and runs again the next)
//   902  DeferredSun: a real failure (its conversion to linear light fails): off until the next cell change, then tried again
//   903  LinearScene: one frame copy failure: off until the next cell change, then tried again
//   904  CapturedNormals: the next self-check reads WRONG once (it checks again a few frames later)
//   905  CapturedNormals: every self-check reads WRONG while DebugVar4 stays 905 (a broken target: three in a row turn it off)
// Off (DebugVar4 0, the default): one settings read a frame, nothing else.

namespace FdTestFault {
	inline int Last = 0;		// DebugVar4 as read last frame
	inline int Armed = 0;		// the fault waiting to be taken (901-904), 0 none
	inline bool Broken = false;	// 905 while it stays set

	// Once a frame (DeferredSun::BeginFrame).
	inline void BeginFrame() {
		const float v = TheSettingManager->GetSettingF("Main.Develop.Main", "DebugVar4");
		const int code = (int)(v + 0.5f);
		Broken = code == 905;
		if (code == Last) return;
		Last = code;
		if (code < 901 || code > 905) return;
		if (code != 905) Armed = code;
		static const char* const what[] = { "one refused DeferredSun frame", "a real DeferredSun failure (its conversion)", "one LinearScene frame copy failure",
			"one CapturedNormals self-check read as WRONG", "every CapturedNormals self-check read as WRONG while it stays set (a broken target)" };
		Logger::Log("UNOFFICIAL TEST FAULT %d armed ([Main.Develop.Main] DebugVar4, test only): %s", code, what[code - 901]);
	}
	// true once for an armed fault (then disarmed)
	inline bool Take(int code) {
		if (Armed != code) return false;
		Armed = 0;
		Logger::Log("UNOFFICIAL TEST FAULT %d taken now", code);
		return true;
	}
}
