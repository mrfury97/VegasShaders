#pragma once

// UNOFFICIAL redundant shader-constant filter ([Main.Main.Performance] SkipRedundantConstants).
//
// The draw-stats measuring build (HANDOFF "MEASURING BUILD RESULT", 2026-10-06) found the game CPU-bound indoors by its
// own draw submission (RCHangar: 10 ms submitting about 5500 draws), with 24.6K SetPixelShaderConstantF calls per frame of
// which 88% set exactly the values the device already held (98% of the registers), and 35% of the vertex ones. Each call
// still costs a trip through the D3D9 runtime and driver. Here every upload is compared with a copy of what the device
// holds and dropped when nothing changes, so the device ends up in the same state: the picture cannot change.
//
// The copy is only trusted while every write to the device's float constants comes through these two calls:
//  - IDirect3DStateBlock9::Apply (D3DX effects restore the state they saved at End) and IDirect3DDevice9::Reset wipe it;
//  - between BeginStateBlock and EndStateBlock the calls are recorded, not applied: passed through, copy untouched;
//  - a call from another thread than the one that draws the frames (the loading screen's) never touches the copy: it
//    only counts itself, and the frame thread then passes every call through and forgets what it sets until no such
//    call has come for 30 frames; then the copy starts again from empty. (The copy has no lock: only the frame thread
//    reads or writes it.) Until 2026-10-06 the first such call turned the filter off until restart, so it never ran
//    after the first loading screen.
// Hooked on the function bodies (Detours), not in the device's vtable: the native D3D9 runtime rewrites the vtable
// every frame (measuring build's lesson).
#include "LinearPreDecode.h"	// UNOFFICIAL 2026-10-17 LinearPreDecode: decodes the lit objects' colour constants here
#include "DrawState.h"	// UNOFFICIAL 2026-11-04 DrawStateTracking: the pixel shader and lost-state events

namespace ConstantFilter {

	typedef HRESULT(STDMETHODCALLTYPE* SetConstantsFn)(IDirect3DDevice9*, UINT, const float*, UINT);
	typedef HRESULT(STDMETHODCALLTYPE* ResetFn)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
	typedef HRESULT(STDMETHODCALLTYPE* DeviceFn)(IDirect3DDevice9*);
	typedef HRESULT(STDMETHODCALLTYPE* EndStateBlockFn)(IDirect3DDevice9*, IDirect3DStateBlock9**);
	typedef HRESULT(STDMETHODCALLTYPE* ApplyFn)(IDirect3DStateBlock9*);
	typedef HRESULT(STDMETHODCALLTYPE* SetPixelShaderFn)(IDirect3DDevice9*, IDirect3DPixelShader9*);

	enum { Registers = 256 };
	struct Copy {
		float value[Registers][4];
		bool known[Registers];
	};

	inline bool Installed = false;
	inline bool Enabled = false;		// the setting, read each frame
	inline bool Broken = false;			// the detours failed: off for good
	inline bool Hooked = false;			// the detours are in (ManyLampsOnePass needs the pixel hook, PixelWriteAfter)
	inline bool Recording = false;		// between BeginStateBlock and EndStateBlock
	inline DWORD Thread = 0;			// the thread that draws the frames (BeginFrame's)
	inline volatile LONG ForeignTick = 0;	// counts calls from another thread
	inline LONG SeenForeignTick = 0;	// the last ForeignTick the frame thread has seen
	inline bool Foreign = false;		// another thread called lately: pass through, forget, until 30 quiet frames
	inline unsigned QuietFrames = 0;
	inline Copy Vertex = {}, Pixel = {};
	inline SetConstantsFn OrigVertex = nullptr, OrigPixel = nullptr;
	inline ResetFn OrigReset = nullptr;
	inline DeviceFn OrigBeginStateBlock = nullptr;
	inline EndStateBlockFn OrigEndStateBlock = nullptr;
	inline ApplyFn OrigApply = nullptr;
	inline SetPixelShaderFn OrigSetPixelShader = nullptr;
	inline unsigned StatFrames = 0;
	inline double StatVertexCalls = 0, StatVertexSkipped = 0, StatPixelCalls = 0, StatPixelSkipped = 0, StatWipes = 0;
	inline unsigned StatPauses = 0;		// times another thread paused it (since start)
	// UNOFFICIAL ShaderWarmUp2 (ShaderWarmUp2.h, 2026-10-18): told about every pixel shader set (it tracks the drawing setup) and
	// every state block applied / reset (its copy of the device state is then read back). Null unless that feature is on.
	inline void (*PixelShaderObserver)(IDirect3DPixelShader9*) = nullptr;
	inline void (*StateLostObserver)() = nullptr;

	inline void Wipe() {
		memset(Vertex.known, 0, sizeof(Vertex.known));
		memset(Pixel.known, 0, sizeof(Pixel.known));
		StatWipes += 1;
	}

	// True when the call can be dropped; otherwise the copy is updated to what the call sets.
	inline bool Redundant(Copy& copy, UINT start, const float* data, UINT count) {
		if (!data || start >= Registers || start + count > Registers) {
			// Out of the copy's range: let it through and forget what it covers.
			for (UINT i = start; i < Registers && i < start + count; i++) copy.known[i] = false;
			return false;
		}
		bool same = true;
		for (UINT i = 0; i < count; i++) {
			if (!copy.known[start + i] || memcmp(copy.value[start + i], data + i * 4, sizeof(float) * 4) != 0) { same = false; break; }
		}
		if (same) return count > 0;
		memcpy(copy.value[start], data, sizeof(float) * 4 * count);
		for (UINT i = 0; i < count; i++) copy.known[start + i] = true;
		return false;
	}

	// 1: filter the call; 0: pass it through and forget the registers it sets; -1: pass it through, copy untouched.
	inline int Use() {
		if (Recording) return -1;
		const DWORD thread = GetCurrentThreadId();
		if (!Thread) return -1;
		if (thread != Thread) {
			InterlockedIncrement(&ForeignTick);
			return -1;
		}
		if (!Enabled || Broken) return 0;
		if (Foreign || ForeignTick != SeenForeignTick) { if (!Foreign) StatPauses++; Foreign = true; return 0; }
		return 1;
	}

	inline HRESULT STDMETHODCALLTYPE HookVertex(IDirect3DDevice9* self, UINT start, const float* data, UINT count) {
		const int use = Use();
		if (use > 0) {
			if (GpuTimer::Enabled) StatVertexCalls += 1;
			if (Redundant(Vertex, start, data, count)) { if (GpuTimer::Enabled) StatVertexSkipped += 1; return D3D_OK; }
		}
		else if (use == 0) for (UINT i = start; i < Registers && i < start + count; i++) Vertex.known[i] = false;
		return OrigVertex(self, start, data, count);
	}

	// The filter and the upload (HookPixel's body before LinearPreDecode).
	inline HRESULT SendPixel(IDirect3DDevice9* self, UINT start, const float* data, UINT count) {
		const int use = Use();
		if (use > 0) {
			if (GpuTimer::Enabled) StatPixelCalls += 1;
			if (Redundant(Pixel, start, data, count)) { if (GpuTimer::Enabled) StatPixelSkipped += 1; return D3D_OK; }
		}
		else if (use == 0) for (UINT i = start; i < Registers && i < start + count; i++) Pixel.known[i] = false;
		return OrigPixel(self, start, data, count);
	}

	// UNOFFICIAL 2026-10-17 LinearPreDecode (core/LinearPreDecode.h): true on the frame thread outside state-block recording.
	inline bool PreDecodeThread() { return !Recording && Thread && GetCurrentThreadId() == Thread; }

	// The decoded copy of game value v (register r: c1, c3-c12) into c(63 + r), unless the copy there was made from exactly v.
	inline void PreDecodeCopy(IDirect3DDevice9* self, UINT r, const float* v) {
		using namespace LinearPreDecode;
		if (CopiesValid && CopyGamma == Gamma && SourceKnown[r] && !memcmp(Source[r], v, sizeof(float) * 4)) return;
		float d[4];
		DecodeFast(d, v, Gamma);
		SendPixel(self, CopyBase + r, d, 1);
		memcpy(Source[r], v, sizeof(float) * 4);
		SourceKnown[r] = true;
		StatCopies++;
	}

	// UNOFFICIAL 2026-10-21 PreDecodeCache: the decoded copies of one call (or one refresh) go up in one upload per run of consecutive
	// registers (c66-c75 in one piece when the game sends c3-c12 together) instead of one upload per register; the device ends with
	// the same values. A copy whose source did not change is left out, as in PreDecodeCopy.
	struct PreDecodeRun {
		IDirect3DDevice9* device;
		UINT first = 0, n = 0;
		float values[LinearPreDecode::Last + 1][4];
		explicit PreDecodeRun(IDirect3DDevice9* d) : device(d) {}
		~PreDecodeRun() { Flush(); }
		void Flush() { if (n) SendPixel(device, LinearPreDecode::CopyBase + first, &values[0][0], n); n = 0; }
		void Add(UINT r, const float* v) {
			using namespace LinearPreDecode;
			if (CopiesValid && CopyGamma == Gamma && SourceKnown[r] && !memcmp(Source[r], v, sizeof(float) * 4)) { Flush(); return; }
			if (n && first + n != r) Flush();
			if (!n) first = r;
			DecodeFast(values[n], v, Gamma);
			memcpy(Source[r], v, sizeof(float) * 4);
			SourceKnown[r] = true;
			StatCopies++;
			n++;
		}
	};

	// A LIN_PREDECODED shader is now bound: read the game's c1-c12 back from the device (the runtime's own copy, so it is the
	// truth whatever wrote them: the game, a state block, anyone) and remake every copy that does not match.
	inline void PreDecodeRefresh(IDirect3DDevice9* self) {
		using namespace LinearPreDecode;
		float game[(Last + 1) * 4];
		if (FAILED(self->GetPixelShaderConstantF(0, game, Last + 1))) { StatReadFailures++; return; }
		if (!CopiesValid || CopyGamma != Gamma) for (UINT r = 0; r <= Last; r++) SourceKnown[r] = false;
		CopiesValid = true;	// (before the copies: PreDecodeCopy compares against the sources only while valid)
		CopyGamma = Gamma;
		if (UseCache) {	// UNOFFICIAL 2026-10-21 PreDecodeCache
			PreDecodeRun run(self);
			for (UINT r = 1; r <= Last; r++) { if (Decodes(r)) run.Add(r, game + r * 4); else run.Flush(); }
		}
		else for (UINT r = 1; r <= Last; r++) if (Decodes(r)) PreDecodeCopy(self, r, game + r * 4);
		StatRefreshes++;
	}

	// After a state block or a reset: the bound shader may have changed, the copies may be stale.
	inline void PreDecodeResync(IDirect3DDevice9* device) {
		LinearPreDecode::CopiesValid = false;
		LinearPreDecode::Active = false;
		IDirect3DPixelShader9* shader = nullptr;
		if (device && SUCCEEDED(device->GetPixelShader(&shader)) && shader) {
			LinearPreDecode::Active = LinearPreDecode::Is(shader);
			shader->Release();
		}
		if (LinearPreDecode::Active) PreDecodeRefresh(device);
	}

	// UNOFFICIAL 2026-10-27 (P83 crash on native Windows D3D9, switching weapons indoors: an access violation in d3d9.dll, stack
	// EffectRecord::Render -> the effect's state block Apply -> HookApply -> PreDecodeResync -> SendPixel; DXVK tolerates it): after
	// a state block (Apply, EndStateBlock) or a Reset the copies are only marked stale; the shader and constants are read back and
	// the copies remade at the next constant or shader call on the frame thread (DeferredResync), outside the runtime's own call.
	// Every draw is preceded by one of those calls, so no lit draw sees stale copies.
	inline bool ResyncPending = false;
	inline void DeferResync() {
		if (LinearPreDecode::Shaders.empty()) return;
		LinearPreDecode::CopiesValid = false;
		ResyncPending = true;
	}
	inline void DeferredResync(IDirect3DDevice9* self) {
		if (!ResyncPending || !PreDecodeThread()) return;
		ResyncPending = false;
		PreDecodeResync(self);
	}

	// UNOFFICIAL 2026-10-27 ManyLampsOnePass (ManyLamps.h AfterPixelWrite): called after every pixel-constant upload made through this body,
	// so a block NVR keeps in some registers can be put back when something else wrote over it. Null unless set (Render.cpp).
	inline void (*PixelWriteAfter)(UINT start, UINT count) = nullptr;

	inline HRESULT STDMETHODCALLTYPE HookPixel(IDirect3DDevice9* self, UINT start, const float* data, UINT count) {
		DeferredResync(self);
		const HRESULT result = SendPixel(self, start, data, count);
		// (2026-11-11: not inside one of NVR's own effect draws, SharedDrawState::OwnPass: those uploads are the effect's own constants,
		// and putting the lamp block "back" over them blackened Reflections' D3D9 composite in RenderScale rect frames)
		if (PixelWriteAfter && SharedDrawState::OwnPassDepth == 0) PixelWriteAfter(start, count);
		// UNOFFICIAL LinearPreDecode: while a LIN_PREDECODED shader is bound, the game's new values of c1 and c3-c12 get their
		// decoded copies at once. A write into c64-c75 by anything else makes the copies stale (remade at the next bind).
		if (data && count && !LinearPreDecode::Shaders.empty() && PreDecodeThread()) {
			if (start <= LinearPreDecode::CopyLast && start + count > LinearPreDecode::CopyFirst) LinearPreDecode::CopiesValid = false;
			// fixes-b #39: stale copies are remade AT ONCE while such a shader is bound (a write into c64-c75 alone used to only flag
			// them, and the draws that followed used the overwritten values until the next shader bind).
			if (LinearPreDecode::Active) {
				if (!LinearPreDecode::CopiesValid) PreDecodeRefresh(self);	// (reads back what this call just set too)
				else if (start <= LinearPreDecode::Last && LinearPreDecode::UseCache) {	// UNOFFICIAL 2026-10-21 PreDecodeCache
					PreDecodeRun run(self);
					for (UINT i = 0; i < count && start + i <= LinearPreDecode::Last; i++) {
						if (LinearPreDecode::Decodes(start + i)) run.Add(start + i, data + i * 4);
						else run.Flush();
					}
				}
				else if (start <= LinearPreDecode::Last)
					for (UINT i = 0; i < count && start + i <= LinearPreDecode::Last; i++)
						if (LinearPreDecode::Decodes(start + i)) PreDecodeCopy(self, start + i, data + i * 4);
			}
		}
		return result;
	}

	// F10 self-check (only while the GPU profile runs): when a LIN_PREDECODED shader is about to be replaced, its draws are done:
	// every copy in c64-c75 must equal the decode of the game's register it stands for, as the device holds both now.
	inline void PreDecodeCheck(IDirect3DDevice9* self) {
		using namespace LinearPreDecode;
		float game[(CopyLast + 1) * 4];
		if (FAILED(self->GetPixelShaderConstantF(0, game, CopyLast + 1))) { StatReadFailures++; return; }
		StatChecks++;
		for (UINT r = 1; r <= Last; r++) {
			if (!Decodes(r)) continue;
			float d[4];
			Decode(d, game + r * 4, Gamma);
			if (memcmp(d, game + (CopyBase + r) * 4, sizeof(d))) { StatMismatches++; break; }
		}
	}

	inline HRESULT STDMETHODCALLTYPE HookSetPixelShader(IDirect3DDevice9* self, IDirect3DPixelShader9* shader) {
		if (GpuTimer::Enabled && LinearPreDecode::Active && !LinearPreDecode::Shaders.empty() && PreDecodeThread()) PreDecodeCheck(self);
		const HRESULT result = OrigSetPixelShader(self, shader);
		if (SUCCEEDED(result) && PixelShaderObserver) PixelShaderObserver(shader);
		if (SUCCEEDED(result) && SharedDrawState::PixelShaderSet) SharedDrawState::PixelShaderSet(shader);	// UNOFFICIAL DrawStateTracking
		if (SUCCEEDED(result) && !LinearPreDecode::Shaders.empty() && PreDecodeThread()) {
			ResyncPending = false;	// (2026-10-27) this bind decides Active and refreshes the copies itself
			LinearPreDecode::Active = LinearPreDecode::Is(shader);
			if (LinearPreDecode::Active) PreDecodeRefresh(self);
		}
		return result;
	}

	inline HRESULT STDMETHODCALLTYPE HookReset(IDirect3DDevice9* self, D3DPRESENT_PARAMETERS* parameters) {
		if (GetCurrentThreadId() != Thread) { InterlockedIncrement(&ForeignTick); const HRESULT r = OrigReset(self, parameters); if (StateLostObserver) StateLostObserver(); if (SharedDrawState::StateLost) SharedDrawState::StateLost(); return r; }
		Wipe();
		const HRESULT result = OrigReset(self, parameters);
		if (StateLostObserver) StateLostObserver(); if (SharedDrawState::StateLost) SharedDrawState::StateLost();
		Wipe();
		DeferResync();	// UNOFFICIAL LinearPreDecode (2026-10-27: at the next constant or shader call, see DeferResync)
		return result;
	}

	inline HRESULT STDMETHODCALLTYPE HookBeginStateBlock(IDirect3DDevice9* self) {
		const HRESULT result = OrigBeginStateBlock(self);
		if (SUCCEEDED(result)) Recording = true;
		return result;
	}

	inline HRESULT STDMETHODCALLTYPE HookEndStateBlock(IDirect3DDevice9* self, IDirect3DStateBlock9** block) {
		Recording = false;
		if (GetCurrentThreadId() == Thread) Wipe();	// in case the runtime applied what it recorded
		else InterlockedIncrement(&ForeignTick);
		const HRESULT result = OrigEndStateBlock(self, block);
		if (StateLostObserver) StateLostObserver(); if (SharedDrawState::StateLost) SharedDrawState::StateLost();
		if (GetCurrentThreadId() == Thread) DeferResync();	// UNOFFICIAL LinearPreDecode (2026-10-27: deferred, see DeferResync)
		return result;
	}

	inline HRESULT STDMETHODCALLTYPE HookApply(IDirect3DStateBlock9* self) {
		if (GetCurrentThreadId() == Thread) Wipe();	// it may set any constant without going through the calls above
		else InterlockedIncrement(&ForeignTick);
		const HRESULT result = OrigApply(self);
		if (StateLostObserver) StateLostObserver(); if (SharedDrawState::StateLost) SharedDrawState::StateLost();
		// UNOFFICIAL LinearPreDecode: it may also have set the pixel shader. 2026-10-27: no device calls in here any more (the native
		// D3D9 crash): the copies are remade at the next constant or shader call (DeferResync).
		if (GetCurrentThreadId() == Thread) DeferResync();
		return result;
	}

	// Once, with the device (Render.cpp, at the start of a frame). Detours the bodies the device's vtable points at now.
	inline void Install(IDirect3DDevice9* device) {
		if (Installed || !device) return;
		Installed = true;
		void** vtable = *(void***)device;
		OrigReset = (ResetFn)vtable[16];
		OrigBeginStateBlock = (DeviceFn)vtable[60];
		OrigEndStateBlock = (EndStateBlockFn)vtable[61];
		OrigVertex = (SetConstantsFn)vtable[94];
		OrigPixel = (SetConstantsFn)vtable[109];
		OrigSetPixelShader = (SetPixelShaderFn)vtable[107];	// UNOFFICIAL LinearPreDecode
		IDirect3DStateBlock9* block = nullptr;
		if (SUCCEEDED(device->CreateStateBlock(D3DSBT_PIXELSTATE, &block)) && block) {
			OrigApply = (ApplyFn)(*(void***)block)[5];
			block->Release();
		}
		if (!OrigApply) {
			Logger::Log("UNOFFICIAL SkipRedundantConstants: no state block to hook; filter not installed.");
			if (!LinearPreDecode::Shaders.empty()) Logger::Log("ERROR: LinearPreDecode needs the constant hooks: lit objects' light colours will be wrong; set [Shaders.PBR.Linear] PreDecode = false.");
			return;
		}
		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		DetourAttach(&(PVOID&)OrigReset, (PVOID)&HookReset);
		DetourAttach(&(PVOID&)OrigBeginStateBlock, (PVOID)&HookBeginStateBlock);
		DetourAttach(&(PVOID&)OrigEndStateBlock, (PVOID)&HookEndStateBlock);
		DetourAttach(&(PVOID&)OrigApply, (PVOID)&HookApply);
		DetourAttach(&(PVOID&)OrigVertex, (PVOID)&HookVertex);
		DetourAttach(&(PVOID&)OrigPixel, (PVOID)&HookPixel);
		DetourAttach(&(PVOID&)OrigSetPixelShader, (PVOID)&HookSetPixelShader);
		const LONG result = DetourTransactionCommit();
		if (result != NO_ERROR) {
			Broken = true;
			Logger::Log("UNOFFICIAL SkipRedundantConstants: detours failed (%ld); filter off.", result);
			if (!LinearPreDecode::Shaders.empty()) Logger::Log("ERROR: LinearPreDecode needs the constant hooks: lit objects' light colours will be wrong; set [Shaders.PBR.Linear] PreDecode = false.");
			return;
		}
		Hooked = true;
		SharedDrawState::ObserversLive = true;	// UNOFFICIAL DrawStateTracking
		Wipe();
		Logger::Log("UNOFFICIAL SkipRedundantConstants: shader-constant uploads the device already holds are skipped (setting %s).",
			TheSettingManager->GetSettingI("Main.Main.Performance", "SkipRedundantConstants") ? "on" : "off");
	}

	// Each frame: the setting (live; turning it on starts from an empty copy), and the F10 line.
	inline void BeginFrame(IDirect3DDevice9* device) {
		Thread = GetCurrentThreadId();
		Install(device);
		LinearPreDecode::UseCache = TheSettingManager->SettingsMain.Main.PreDecodeCache;	// UNOFFICIAL 2026-10-21 (cpulimits)
		// UNOFFICIAL LinearPreDecode: the gamma changed (live setting): the copies are remade at the next bind of such a shader.
		if (LinearPreDecode::CopyGamma != LinearPreDecode::Gamma) LinearPreDecode::CopiesValid = false;
		const bool enabled = TheSettingManager->SettingsMain.Main.SkipRedundantConstants;	// read by SettingManager::LoadSettings, not by name each frame
		if (enabled && !Enabled) Wipe();
		Enabled = enabled;
		// Another thread (the loading screen) set constants: pause until it has been quiet for 30 frames, then start again
		// from an empty copy.
		const LONG foreignTick = ForeignTick;
		if (foreignTick != SeenForeignTick) {
			if (!Foreign) StatPauses++;
			SeenForeignTick = foreignTick;
			Foreign = true;
			QuietFrames = 0;
			// fixes-b #39: another thread (the loading screen) may have written the device's constants, c64-c75 included, without
			// this thread seeing it: re-sync the decoded copies from the device.
			if (!LinearPreDecode::Shaders.empty()) PreDecodeResync(device);
		}
		else if (Foreign && ++QuietFrames >= 30) {
			Foreign = false;
			Wipe();
		}
		if (GpuTimer::Enabled && Installed && ++StatFrames >= 240) {
			const double perFrame = 1.0 / StatFrames;
			Logger::Log("CONSTANT FILTER %s: pixel %.0f calls, %.0f skipped (%.0f%%) | vertex %.0f calls, %.0f skipped (%.0f%%) per frame | copy wiped %.1f times per frame | paused for another thread %u times so far",
				Broken ? "OFF (detours failed)" : !Enabled ? "off" : Foreign ? "paused (another thread)" : "on", StatPixelCalls * perFrame, StatPixelSkipped * perFrame,
				StatPixelCalls > 0 ? 100.0 * StatPixelSkipped / StatPixelCalls : 0.0, StatVertexCalls * perFrame, StatVertexSkipped * perFrame,
				StatVertexCalls > 0 ? 100.0 * StatVertexSkipped / StatVertexCalls : 0.0, StatWipes * perFrame, StatPauses);
			if (!LinearPreDecode::Shaders.empty())	// UNOFFICIAL LinearPreDecode
				Logger::Log("LINEAR PREDECODE: %u shaders, %.0f decoded copies and %.0f copy refreshes per frame (gamma %.2f), %u read-back failures | self-check: %u of %u finished draw runs had a stale copy | PreDecodeCache %s: %.0f channel lookups per frame, %.1f%% from the table",
					(unsigned)LinearPreDecode::Shaders.size(), LinearPreDecode::StatCopies * perFrame, LinearPreDecode::StatRefreshes * perFrame, LinearPreDecode::Gamma,
					LinearPreDecode::StatReadFailures, LinearPreDecode::StatMismatches, LinearPreDecode::StatChecks, LinearPreDecode::UseCache ? "on" : "off",
					LinearPreDecode::StatPowLookups * perFrame, LinearPreDecode::StatPowLookups ? 100.0 * (LinearPreDecode::StatPowLookups - LinearPreDecode::StatPowMisses) / LinearPreDecode::StatPowLookups : 0.0);
			LinearPreDecode::StatPowLookups = LinearPreDecode::StatPowMisses = 0;
			LinearPreDecode::StatMismatches = LinearPreDecode::StatChecks = 0;
			LinearPreDecode::StatCopies = LinearPreDecode::StatRefreshes = 0;
			StatFrames = 0;
			StatVertexCalls = StatVertexSkipped = StatPixelCalls = StatPixelSkipped = StatWipes = 0;
		}
	}
}
