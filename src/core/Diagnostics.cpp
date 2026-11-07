// UNOFFICIAL 2026-11-01: the effects' status registry (EffectStatus.h), the automatic flash detector and the report key (Diagnostics.h).
#include "Diagnostics.h"
#include "FlashRule.h"
#include "TemporalBackbone.h"
#include "ImGuiManager.h"
#include "EffectSelfTestCore.h"	// SelfTestCore::ConvertRow / SavePng (WIC, no device) for the report burst
#include "RtCore.h"					// UNOFFICIAL 2026-11-01 the REPORT block's ray tracing line
#include "RtSunShadows.h"
#include "RtPathTrace.h"	// UNOFFICIAL 2026-11-02 the reference path tracer's state line
#include "../effects/Reflections/Reflections.h"
#include "../effects/AmbientOcclusion/AmbientOcclusion.h"
#include <cmath>
#include <cstdarg>
#include <ctime>
#include <mutex>

namespace {

	double Seconds() {
		static LARGE_INTEGER frequency = {}, start = {};
		LARGE_INTEGER now;
		if (!frequency.QuadPart) { QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&start); }
		QueryPerformanceCounter(&now);
		return (double)(now.QuadPart - start.QuadPart) / (double)frequency.QuadPart;
	}

	unsigned FrameNow() { return TheFrameRateManager ? TheFrameRateManager->FrameNumber : 0; }

	// Lines of up to ~900 characters: "<head> item, item, ..."
	void LogList(const char* head, const std::vector<std::string>& items, const char* separator = ", ") {
		std::string line;
		for (const std::string& item : items) {
			if (!line.empty() && line.size() + item.size() > 900) { Logger::Log("%s%s", head, line.c_str()); line.clear(); }
			if (!line.empty()) line += separator;
			line += item;
		}
		if (!line.empty()) Logger::Log("%s%s", head, line.c_str());
	}

	// --- the effects' status registry ---
	struct StatusSlot {
		const char* name = nullptr;
		unsigned frame[EffectStatus::Ring] = {};
		unsigned char result[EffectStatus::Ring] = {};
		const char* why[EffectStatus::Ring] = {};
		const char* history[EffectStatus::Ring] = {};
		unsigned drawn = 0, skipped = 0, resets = 0;	// the heartbeat's minute
		const char* lastSkip = nullptr;
		const char* lastReset = nullptr;
	};
	constexpr int MaxSlots = 160;
	StatusSlot Slots[MaxSlots];
	int SlotCount = 0;
	std::mutex RegisterLock;

	unsigned EntryOf(StatusSlot& s, unsigned frame) {
		const unsigned i = frame % EffectStatus::Ring;
		if (s.frame[i] != frame) { s.frame[i] = frame; s.result[i] = EffectStatus::NotCalled; s.why[i] = nullptr; s.history[i] = nullptr; }
		return i;
	}
	const StatusSlot* Find(int slot) { return slot >= 0 && slot < SlotCount ? &Slots[slot] : nullptr; }
	bool Has(const StatusSlot& s, unsigned frame) { return s.frame[frame % EffectStatus::Ring] == frame && frame != 0; }

	const char* ResultText(unsigned char r) {
		switch (r) {
		case EffectStatus::Drawn: return "drawn";
		case EffectStatus::Disabled: return "off";
		case EffectStatus::NotLoaded: return "NOT LOADED (its shader failed or is missing)";
		case EffectStatus::ShouldRenderFalse: return "skipped (ShouldRender false: its own conditions)";
		case EffectStatus::Skipped: return "skipped";
		default: return "not run";
		}
	}

	// "drawn (FSR 2) | history kept" for a slot's frame
	std::string StateText(const StatusSlot& s, unsigned frame) {
		if (!Has(s, frame)) return "not run";
		const unsigned i = frame % EffectStatus::Ring;
		std::string text = ResultText(s.result[i]);
		if (s.why[i]) text += std::string(s.result[i] == EffectStatus::Skipped ? ": " : " (") + s.why[i] + (s.result[i] == EffectStatus::Skipped ? "" : ")");
		if (s.history[i]) text += std::string(", history ") + s.history[i];
		return text;
	}

	struct LevelEntry { std::string what, chosen; bool isDefault; };
	std::vector<LevelEntry> Levels;
	std::mutex LevelLock;

	volatile LONG ShaderCounts[5] = {};
	unsigned LastShaderCounts[5] = {};

	struct FailSite { const char* site; unsigned count, firstFrame, minuteCount; long hr; };
	std::vector<FailSite> Failures;
	std::mutex FailLock;

	struct TargetEntry { std::string name; unsigned w, h; };
	std::vector<TargetEntry> Targets;
	std::mutex TargetLock;

	void EndFrame(unsigned frame) {
		for (int k = 0; k < SlotCount; k++) {
			StatusSlot& s = Slots[k];
			if (!Has(s, frame)) continue;
			const unsigned i = frame % EffectStatus::Ring;
			const unsigned char r = s.result[i];
			if (r == EffectStatus::Drawn) s.drawn++;
			else if (r == EffectStatus::ShouldRenderFalse || r == EffectStatus::Skipped || r == EffectStatus::NotLoaded) {
				s.skipped++;
				s.lastSkip = s.why[i] ? s.why[i] : ResultText(r);
			}
			if (s.history[i] && !strncmp(s.history[i], "reset", 5)) { s.resets++; s.lastReset = s.history[i]; }
		}
	}

	// Every effect's state in a frame, then what changed from the frame before and to the frame after.
	void DumpEffects(const char* head, unsigned frame) {
		std::vector<std::string> drawn, skipped, notRun, changed;
		unsigned off = 0;
		std::vector<bool> seen(SlotCount, false);
		for (int k = 0; k < SlotCount; k++) {
			const StatusSlot& s = Slots[k];
			if (!Has(s, frame)) continue;
			seen[k] = true;
			const unsigned char r = s.result[frame % EffectStatus::Ring];
			if (r == EffectStatus::Disabled) off++;
			else (r == EffectStatus::Drawn ? drawn : skipped).push_back(std::string(s.name) + " " + StateText(s, frame));
			for (unsigned other : { frame - 1, frame + 1 }) {
				const bool before = other < frame;
				const std::string a = StateText(s, other), b = StateText(s, frame);
				if (!Has(s, other) || a == b) continue;
				changed.push_back(std::string(s.name) + (before ? ": f-1 " : ": f+1 ") + a + (before ? " -> this frame " : " <- this frame ") + b);
			}
		}
		// Effects that are on but whose Render was not called this frame (the frame's path skipped them: interior / exterior, menus...)
		if (TheShaderManager) {
			for (const auto& entry : TheShaderManager->EffectsNames) {
				EffectRecord* effect = entry.second ? *entry.second : nullptr;
				if (!effect || !effect->Enabled) continue;
				const int slot = effect->StatusSlot;
				if (slot >= 0 && slot < SlotCount && seen[slot]) continue;
				notRun.push_back(entry.first);
			}
		}
		char line[160];
		sprintf_s(line, "%s effects drawn: ", head);
		LogList(line, drawn, " | ");
		sprintf_s(line, "%s effects skipped: ", head);
		LogList(line, skipped, " | ");
		if (!notRun.empty()) { sprintf_s(line, "%s effects on but not run this frame: ", head); LogList(line, notRun); }
		Logger::Log("%s effects off: %u", head, off);
		sprintf_s(line, "%s effects changed around this frame: ", head);
		if (changed.empty()) Logger::Log("%snone", line);
		else LogList(line, changed, " | ");
	}

	void DumpLevels(const char* head) {
		std::vector<std::string> items;
		{
			std::lock_guard<std::mutex> guard(LevelLock);
			for (const LevelEntry& l : Levels) if (!l.isDefault) items.push_back(l.what + ": " + l.chosen);
		}
		char line[160];
		sprintf_s(line, "%s shader levels / fallbacks not at the default: ", head);
		if (items.empty()) Logger::Log("%snone", line);
		else LogList(line, items, " | ");
	}

	std::string CountsText() {
		char text[400];
		sprintf_s(text, "shader cache: %ld hits, %ld compiled, %ld precompiled, %ld compile failures, %ld PREFER_FLOW_CONTROL retries",
			ShaderCounts[EffectStatus::CacheHit], ShaderCounts[EffectStatus::Compiled], ShaderCounts[EffectStatus::Precompiled],
			ShaderCounts[EffectStatus::CompileFailed], ShaderCounts[EffectStatus::FlowControlRetry]);
		std::string out = text;
		std::lock_guard<std::mutex> guard(FailLock);
		if (Failures.empty()) out += " | D3D / Vegas Shaders DXVK call failures: none";
		else {
			out += " | D3D / Vegas Shaders DXVK call failures:";
			for (const FailSite& f : Failures) {
				sprintf_s(text, " %s x%u (%08lX, first at frame %u);", f.site, f.count, (unsigned long)f.hr, f.firstFrame);
				out += text;
			}
		}
		return out;
	}

	unsigned BytesPerPixel(D3DFORMAT f) { return f == D3DFMT_A16B16G16R16F ? 8 : f == D3DFMT_A32B32G32R32F ? 16 : 4; }

	// --- a 2:1 shrink chain down to 8 x 8 (the flash detector's; 2026-11-01 shared with the report burst's VL stages) ---
	// Bilinear 2:1 StretchRects average 2 x 2 texels exactly (odd sizes round up), in the source's own format: a half-float source
	// stays half float, nothing is clamped. A multisampled source is resolved at full size first (a multisampled source cannot be
	// stretched everywhere). Make(d, w, h): for sources like d, of which the top-left w x h part is shrunk (all of it, or the drawn
	// part of a RenderScale frame). Run: the source -> Last(). LINEAR falls back to POINT for good if the driver cannot filter.
	struct ShrinkChain {
		std::vector<IDirect3DSurface9*> steps;	// [the resolve,] the 2:1 steps; the last one is 8 x 8
		D3DFORMAT format = D3DFMT_UNKNOWN;
		UINT width = 0, height = 0, areaW = 0, areaH = 0;
		D3DMULTISAMPLE_TYPE msaa = D3DMULTISAMPLE_NONE;
		D3DTEXTUREFILTERTYPE filter = D3DTEXF_LINEAR;
		bool resolved = false;
		double bytes = 0.0;

		bool Fits(const D3DSURFACE_DESC& d, UINT w, UINT h) const {
			return !steps.empty() && d.Format == format && d.Width == width && d.Height == height && d.MultiSampleType == msaa && w == areaW && h == areaH;
		}
		void Release() {
			for (IDirect3DSurface9* s : steps) if (s) s->Release();
			steps.clear();
			resolved = false;
			bytes = 0.0;
		}
		bool Make(IDirect3DDevice9* device, const D3DSURFACE_DESC& d, UINT w, UINT h) {
			Release();
			format = d.Format; width = d.Width; height = d.Height; msaa = d.MultiSampleType; areaW = w; areaH = h;
			if (d.MultiSampleType != D3DMULTISAMPLE_NONE) {
				IDirect3DSurface9* s = nullptr;
				if (FAILED(device->CreateRenderTarget(d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &s, NULL))) return false;
				steps.push_back(s);
				resolved = true;
				bytes += (double)d.Width * d.Height * BytesPerPixel(d.Format);
			}
			while (w > 8 || h > 8) {
				w = (std::max)(8u, (w + 1) / 2);
				h = (std::max)(8u, (h + 1) / 2);
				IDirect3DSurface9* s = nullptr;
				if (FAILED(device->CreateRenderTarget(w, h, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &s, NULL))) return false;
				steps.push_back(s);
				bytes += (double)w * h * BytesPerPixel(d.Format);
			}
			return !steps.empty();
		}
		HRESULT Run(IDirect3DDevice9* device, IDirect3DSurface9* source) {
			const RECT area = { 0, 0, (LONG)areaW, (LONG)areaH };
			const bool part = areaW != width || areaH != height;
			for (size_t i = 0; i < steps.size(); i++) {
				const RECT* from = part && i == (resolved ? 1u : 0u) ? &area : NULL;	// the first 2:1 step reads the drawn part
				HRESULT hr = device->StretchRect(source, from, steps[i], NULL, filter);
				if (FAILED(hr) && filter == D3DTEXF_LINEAR) { filter = D3DTEXF_POINT; hr = device->StretchRect(source, from, steps[i], NULL, filter); }
				if (FAILED(hr)) return hr;
				source = steps[i];
			}
			return S_OK;
		}
		IDirect3DSurface9* Last() const { return steps.empty() ? nullptr : steps.back(); }
	};

	// --- the flash detector ---
	constexpr unsigned FinalRing = 64, StageRing = 64;	// frames of history kept (UNOFFICIAL 2026-11-01 step 2; the read-back ring stays 4)
	struct Flash {
		bool on = true, failed = false, announced = false;
		ShrinkChain shrink;	// the picture's 2:1 steps down to 8 x 8
		struct Readback { IDirect3DSurface9* mem = nullptr; bool pending = false; unsigned frame = 0; const char* quiet = nullptr; float turn = 0.0f, ms = 0.0f; };
		Readback ring[4];
		struct Sample { unsigned frame = 0; bool valid = false; const char* quiet = nullptr; float turn = 0.0f, ms = 0.0f; float luma[FlashRule::Cells]; };
		Sample samples[FinalRing];	// UNOFFICIAL 2026-11-01 step 2: 64 frames of history (the beat detector's window is 48)
		// counts (session / heartbeat minute)
		unsigned judged = 0, flashes = 0, dumps = 0, shortLines = 0, late = 0, minuteFlashes = 0, minuteIgnored = 0, minuteJudged = 0, minuteLate = 0;
		std::string minuteIgnoredWhy;
		double lastDump = -1e9, lastShort = -1e9;
		// cost
		double costSum = 0.0, minuteCost = 0.0;
		unsigned costFrames = 0, minuteCostFrames = 0;
		// suppression state
		unsigned quietUntil = 0;
		TESObjectCELL* lastCell = nullptr;
		bool wasLoading = false;
		D3DXVECTOR3 lastForward = { 0.0f, 0.0f, 0.0f };
		double lastTime = -1.0;
	} F;

	void ReleaseFlash() {
		F.shrink.Release();
		for (auto& r : F.ring) { if (r.mem) r.mem->Release(); r = Flash::Readback(); }
		for (auto& s : F.samples) s.valid = false;
	}

	void FlashOff(const char* why) {
		ReleaseFlash();
		F.failed = true;
		Logger::Log("FLASH detector off for this session: %s", why);
	}

	bool FormatOk(D3DFORMAT f) {
		return f == D3DFMT_A8R8G8B8 || f == D3DFMT_X8R8G8B8 || f == D3DFMT_A16B16G16R16F || f == D3DFMT_A2R10G10B10 || f == D3DFMT_A2B10G10R10;
	}

	bool MakeFlash(IDirect3DDevice9* device, const D3DSURFACE_DESC& d) {
		ReleaseFlash();
		if (!F.shrink.Make(device, d, d.Width, d.Height)) return false;
		for (auto& r : F.ring)
			if (FAILED(device->CreateOffscreenPlainSurface(8, 8, d.Format, D3DPOOL_SYSTEMMEM, &r.mem, NULL))) return false;
		if (!F.announced) {
			F.announced = true;
			Logger::Log("FLASH detector on: picture %ux%u format %d, %u shrink steps to 8x8, read back 3 frames late without waiting", d.Width, d.Height,
				(int)d.Format, (unsigned)F.shrink.steps.size());
		}
		return true;
	}

	float HalfToFloat(unsigned short h) {
		D3DXFLOAT16 v;
		memcpy(&v, &h, 2);
		float f;
		D3DXFloat16To32Array(&f, &v, 1);
		return f;
	}

	bool ReadLuma(Flash::Readback& r, float* luma) {
		D3DLOCKED_RECT lock = {};
		const HRESULT hr = r.mem->LockRect(&lock, NULL, D3DLOCK_READONLY | D3DLOCK_DONOTWAIT);
		if (hr == D3DERR_WASSTILLDRAWING) { F.late++; F.minuteLate++; return false; }
		if (FAILED(hr)) { EffectStatus::D3DFail("flash detector LockRect", hr); return false; }
		for (int y = 0; y < 8; y++) {
			const unsigned char* row = (const unsigned char*)lock.pBits + (size_t)y * lock.Pitch;
			for (int x = 0; x < 8; x++) {
				float red, green, blue;
				const D3DFORMAT format = F.shrink.format;
				if (format == D3DFMT_A16B16G16R16F) {
					const unsigned short* p = (const unsigned short*)(row + x * 8);
					red = HalfToFloat(p[0]); green = HalfToFloat(p[1]); blue = HalfToFloat(p[2]);
				}
				else if (format == D3DFMT_A2R10G10B10 || format == D3DFMT_A2B10G10R10) {
					const UInt32 v = *(const UInt32*)(row + x * 4);
					const float lo = (v & 1023) / 1023.0f, mid = ((v >> 10) & 1023) / 1023.0f, hi = ((v >> 20) & 1023) / 1023.0f;
					if (format == D3DFMT_A2R10G10B10) { blue = lo; green = mid; red = hi; } else { red = lo; green = mid; blue = hi; }
				}
				else {
					const unsigned char* p = row + x * 4;
					blue = p[0] / 255.0f; green = p[1] / 255.0f; red = p[2] / 255.0f;
				}
				float l = 0.2126f * red + 0.7152f * green + 0.0722f * blue;
				luma[y * 8 + x] = l != l ? 0.0f : (std::min)((std::max)(l, 0.0f), 4.0f);
			}
		}
		r.mem->UnlockRect();
		return true;
	}

	const char* QuietReason(unsigned frame, float turn) {
		if (!Player || !Player->parentCell) return "no player cell";
		if (!InterfaceManager || !InterfaceManager->IsActive(Menu::kMenuType_None)) return "a game menu or loading screen";
		const ShaderManager::GameStateStruct& g = TheShaderManager->GameState;
		if (g.PipBoyIsOn) return "Pip-Boy";
		if (g.VATSIsOn) return "VATS";
		if (g.isDialog) return "dialogue";
		if (g.OverlayIsOn) return "overlay";
		if (frame < F.quietUntil) return "just after a load / cell change";
		if (!TemporalBackbone::HistoryValid()) return "history reset (cut, jump, gap or size change)";
		if (turn > 5.0f) return "fast camera turn";
		return nullptr;
	}

	// --- the stage probes (Diagnostics.h part 4; docs/next/FLICKER-ISOLATION-PLAN.md step 1) ---
	// The detector's 8 x 8 summary taken after every stage of the frame: StageMark("scene before NVR") at the start of the HDR pass,
	// each effect that wrote the frame (StageAfterEffect at PixelInspector::AfterFrame's call sites), StageMark("tonemapped") at the
	// start of the post pass, plus any EFFECT_PROBE of an effect's own buffer. Each probe: one StretchRect of the picture into a
	// 64 x 64 target (a uniform sample: 64 samples a cell, enough for a cell mean to within a few thousandths), then 2:1 to 32, 16, and
	// 8 x 8 into row k of this frame's strip (8 x 8 x MaxStages) of the picture's format. At the end of the frame one
	// GetRenderTargetData per format used; 3 frames later the copy is locked without waiting and every row read into that frame's
	// record. A FLASH at frame t then has the stages of t-1, t, t+1 and FlashRule::Attribute names the source.
	// Modes ([Main.Main.Misc] FlashStages): 0 off; 1 ARMED (default): off until a FLASH or a report key press, then on for
	// StageArmSeconds, so a recurring flicker is attributed by its next flash and idle frames pay nothing; 2 always (the bench).
	constexpr double StageArmSeconds = 60.0;
	struct Stages {
		int mode = 1;
		bool modeRead = false, failed = false, announced = false;
		double armedUntil = -1e9;
		struct FormatSet {	// the chain's buffers before the tonemap are float16, after it the game target's format: one set per format
			D3DFORMAT format = D3DFMT_UNKNOWN;
			IDirect3DSurface9* step[3] = {};	// 64, 32, 16
			IDirect3DSurface9* strip[4] = {};	// 8 x (8 x MaxStages), one per frame of the ring
			IDirect3DSurface9* sys[4] = {};		// their system-memory copies
			bool pending[4] = {};
			unsigned pendingFrame[4] = {};
		};
		FormatSet formats[3];
		int formatCount = 0;
		D3DTEXTUREFILTERTYPE filter = D3DTEXF_LINEAR;
		// the frame being probed
		unsigned frame = 0xFFFFFFFF, lastWrites = 0;
		const void* lastEffect = nullptr;
		struct Entry { const char* name = nullptr; unsigned char kind = 0; unsigned char fmt = 0; bool valid = false; float luma[FlashRule::Cells]; };	// kind 0 chain, 1 own buffer
		struct Frame { unsigned frame = 0; unsigned count = 0; unsigned usedFormats = 0; Entry e[FlashRule::MaxStages]; };
		Frame frames[StageRing];	// UNOFFICIAL 2026-11-01 step 2: 64 frames, so a beat's stages can be followed over the window
		// counts (session / heartbeat minute)
		unsigned probedFrames = 0, minuteProbedFrames = 0, minuteStages = 0, late = 0, minuteLate = 0, full = 0, msaa = 0, unknownFormat = 0, noFormatSlot = 0;
		double costSum = 0.0, minuteCost = 0.0, frameCost = 0.0;
		unsigned costFrames = 0, minuteCostFrames = 0;
	} S;

	bool StagesRunning() {
		if (S.failed || S.mode <= 0) return false;
		return S.mode >= 2 || Seconds() < S.armedUntil;
	}

	void StagesArm(const char* why) {
		if (S.mode != 1 || S.failed) return;
		const bool was = Seconds() < S.armedUntil;
		S.armedUntil = Seconds() + StageArmSeconds;
		if (!was) Logger::Log("FLASH stage probes armed for %.0f s (%s): the next flash says which effect's output jumped ([Main.Main.Misc] FlashStages)", StageArmSeconds, why);
	}

	void ReleaseFormat(Stages::FormatSet& f) {
		for (IDirect3DSurface9*& s : f.step) if (s) { s->Release(); s = nullptr; }
		for (IDirect3DSurface9*& s : f.strip) if (s) { s->Release(); s = nullptr; }
		for (IDirect3DSurface9*& s : f.sys) if (s) { s->Release(); s = nullptr; }
		for (bool& p : f.pending) p = false;
		f.format = D3DFMT_UNKNOWN;
	}

	void ReleaseStages() {
		for (Stages::FormatSet& f : S.formats) ReleaseFormat(f);
		S.formatCount = 0;
		for (Stages::Frame& fr : S.frames) { fr.frame = 0; fr.count = 0; fr.usedFormats = 0; }
		S.frame = 0xFFFFFFFF;
	}

	void StagesOff(const char* why) {
		ReleaseStages();
		S.failed = true;
		Logger::Log("FLASH stage probes off for this session: %s (the flash detector itself keeps running)", why);
	}

	Stages::FormatSet* StageFormat(IDirect3DDevice9* device, D3DFORMAT format) {
		for (int i = 0; i < S.formatCount; i++) if (S.formats[i].format == format) return &S.formats[i];
		if (S.formatCount >= 3) { S.noFormatSlot++; return nullptr; }
		Stages::FormatSet& f = S.formats[S.formatCount];
		f.format = format;
		const UINT sizes[3] = { 64, 32, 16 };
		bool ok = true;
		for (int i = 0; ok && i < 3; i++) ok = SUCCEEDED(device->CreateRenderTarget(sizes[i], sizes[i], format, D3DMULTISAMPLE_NONE, 0, FALSE, &f.step[i], NULL));
		for (int i = 0; ok && i < 4; i++)
			ok = SUCCEEDED(device->CreateRenderTarget(8, 8 * FlashRule::MaxStages, format, D3DMULTISAMPLE_NONE, 0, FALSE, &f.strip[i], NULL)) &&
				SUCCEEDED(device->CreateOffscreenPlainSurface(8, 8 * FlashRule::MaxStages, format, D3DPOOL_SYSTEMMEM, &f.sys[i], NULL));
		if (!ok) { ReleaseFormat(f); StagesOff("could not make its small render targets / read-back surfaces"); return nullptr; }
		S.formatCount++;
		if (!S.announced) {
			S.announced = true;
			Logger::Log("FLASH stage probes on (%s): the picture after every effect shrunk to 8x8 (a 64x64 sample, then 2:1 steps) into a row of one strip per picture "
				"format (%d rows), read back 3 frames late without waiting; a FLASH then names the stage whose own output jumped", S.mode >= 2 ? "always" : "armed", FlashRule::MaxStages);
		}
		return &f;
	}

	Stages::Frame& StageFrameFor(unsigned frame) {
		Stages::Frame& fr = S.frames[frame % StageRing];
		if (S.frame != frame) {
			S.frame = frame; S.lastWrites = 0xFFFFFFFF; S.lastEffect = nullptr;
			fr.frame = frame; fr.count = 0; fr.usedFormats = 0;
		}
		return fr;
	}

	void Probe(const char* name, IDirect3DSurface9* picture, unsigned char kind) {
		if (!name || !picture || !TheRenderManager || !TheRenderManager->device) return;
		IDirect3DDevice9* device = TheRenderManager->device;
		const double start = Seconds();
		const unsigned frame = FrameNow();
		Stages::Frame& fr = StageFrameFor(frame);
		if (fr.count >= (unsigned)FlashRule::MaxStages) { S.full++; return; }
		D3DSURFACE_DESC d = {};
		if (FAILED(picture->GetDesc(&d))) return;
		if (d.MultiSampleType != D3DMULTISAMPLE_NONE) { S.msaa++; return; }
		if (!SelfTestCore::KnownFormat(d.Format)) { S.unknownFormat++; return; }
		Stages::FormatSet* f = StageFormat(device, d.Format);
		if (!f) return;
		IDirect3DSurface9* source = picture;
		for (int i = 0; i < 3; i++) {
			HRESULT hr = device->StretchRect(source, NULL, f->step[i], NULL, S.filter);
			if (FAILED(hr) && S.filter == D3DTEXF_LINEAR) { S.filter = D3DTEXF_POINT; hr = device->StretchRect(source, NULL, f->step[i], NULL, S.filter); }
			if (FAILED(hr)) { EffectStatus::D3DFail("stage probe StretchRect", hr); StagesOff("StretchRect failed"); return; }
			source = f->step[i];
		}
		const RECT row = { 0, (LONG)fr.count * 8, 8, (LONG)fr.count * 8 + 8 };
		const HRESULT hr = device->StretchRect(source, NULL, f->strip[frame % 4], &row, S.filter);
		if (FAILED(hr)) { EffectStatus::D3DFail("stage probe StretchRect (strip row)", hr); StagesOff("StretchRect into the strip failed"); return; }
		Stages::Entry& e = fr.e[fr.count++];
		e.name = name; e.kind = kind; e.fmt = (unsigned char)(f - S.formats); e.valid = false;
		fr.usedFormats |= 1u << e.fmt;
		S.frameCost += Seconds() - start;
	}

	// End of the frame (OnFinalPicture, before the detector reads its own copy): this frame's strips copied, the copies of 3 frames ago read.
	void StagesEndFrame(IDirect3DDevice9* device, unsigned frame) {
		if (S.failed || !S.formatCount) return;
		const double start = Seconds();
		Stages::Frame& fr = S.frames[frame % StageRing];
		const bool probed = fr.frame == frame && fr.count;
		if (probed) {
			for (int i = 0; i < S.formatCount; i++) {
				if (!(fr.usedFormats & (1u << i))) continue;
				Stages::FormatSet& f = S.formats[i];
				const HRESULT hr = device->GetRenderTargetData(f.strip[frame % 4], f.sys[frame % 4]);
				f.pending[frame % 4] = SUCCEEDED(hr);
				f.pendingFrame[frame % 4] = frame;
				if (FAILED(hr)) EffectStatus::D3DFail("stage probe GetRenderTargetData", hr);
			}
			S.probedFrames++; S.minuteProbedFrames++; S.minuteStages += fr.count;
		}
		const unsigned slot = (frame + 1) % 4;	// the copies made 3 frames ago
		for (int i = 0; i < S.formatCount; i++) {
			Stages::FormatSet& f = S.formats[i];
			if (!f.pending[slot]) continue;
			f.pending[slot] = false;
			Stages::Frame& old = S.frames[f.pendingFrame[slot] % StageRing];
			if (old.frame != f.pendingFrame[slot]) continue;
			D3DLOCKED_RECT lock = {};
			const HRESULT hr = f.sys[slot]->LockRect(&lock, NULL, D3DLOCK_READONLY | D3DLOCK_DONOTWAIT);
			if (hr == D3DERR_WASSTILLDRAWING) { S.late++; S.minuteLate++; continue; }
			if (FAILED(hr)) { EffectStatus::D3DFail("stage probe LockRect", hr); continue; }
			float row[8 * 4];
			for (unsigned k = 0; k < old.count; k++) {
				Stages::Entry& e = old.e[k];
				if (e.fmt != i) continue;
				for (int y = 0; y < 8; y++) {
					SelfTestCore::ConvertRow(f.format, (const unsigned char*)lock.pBits + (size_t)(k * 8 + y) * lock.Pitch, 8, row);
					for (int x = 0; x < 8; x++) {
						const float* c = &row[x * 4];
						const float l = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
						e.luma[y * 8 + x] = l != l ? 0.0f : (std::min)((std::max)(l, 0.0f), 64.0f);	// HDR stages may pass 1; the attribution is relative
					}
				}
				e.valid = true;
			}
			f.sys[slot]->UnlockRect();
		}
		if (probed) {
			const double cost = S.frameCost + (Seconds() - start);
			S.costSum += cost; S.costFrames++; S.minuteCost += cost; S.minuteCostFrames++;
			if (S.costFrames == 600) {
				const double average = S.costSum / S.costFrames * 1000.0;
				S.costSum = 0.0; S.costFrames = 0;
				if (average > 0.5) {
					char why[160];
					sprintf_s(why, "they cost %.2f ms a frame on average here (over 0.5 ms: this driver's read-back waits)", average);
					StagesOff(why);
				}
			}
		}
		S.frameCost = 0.0;
	}

	// The attribution of a flash at frame t: summary (the source, what amplified / damped it) and detail (own buffers, every stage's jump)
	void StageText(unsigned t, const FlashRule::Verdict& v, const float* finalPrev, const float* finalCur, const float* finalNext, std::string& summary, std::string& detail) {
		detail.clear();
		const Stages::Frame* fr[3];
		for (int j = 0; j < 3; j++) {
			const unsigned f = t - 1 + j;
			fr[j] = &S.frames[f % StageRing];
			if (fr[j]->frame != f || !fr[j]->count) fr[j] = nullptr;
		}
		if (!fr[1]) {
			summary = S.failed ? "stages: the probes turned themselves off" : S.mode <= 0 ? "stages: probes off ([Main.Main.Misc] FlashStages 0)" :
				S.mode >= 2 ? "stages: not probed this frame" : "stages: not probed this frame (the probes are armed now: the next flash says which effect)";
			return;
		}
		FlashRule::StageInput in[FlashRule::MaxStages];
		const char* names[FlashRule::MaxStages];
		int count = 0;
		std::vector<std::string> ownBuffers;
		char item[200];
		for (unsigned k = 0; k < fr[1]->count; k++) {
			const Stages::Entry& e = fr[1]->e[k];
			if (!e.valid) continue;
			int occurrence = 0;
			for (unsigned j = 0; j < k; j++) if (fr[1]->e[j].kind == e.kind && !strcmp(fr[1]->e[j].name, e.name)) occurrence++;
			auto find = [&](const Stages::Frame* f) -> const float* {
				if (!f) return nullptr;
				int n = 0;
				for (unsigned j = 0; j < f->count; j++) {
					const Stages::Entry& o = f->e[j];
					if (o.kind != e.kind || strcmp(o.name, e.name)) continue;
					if (n == occurrence) return o.valid ? o.luma : nullptr;
					n++;
				}
				return nullptr;
			};
			const float* prev = find(fr[0]);
			const float* next = find(fr[2]);
			if (e.kind == 1) {	// an effect's own buffer: its raw jump on the flashing cells (not differenced: it is not the picture)
				if (prev && next) { sprintf_s(item, "%s %.1f %%", e.name, FlashRule::RelativeJump(prev, e.luma, next, v.mask, v.sign) * 100.0f); ownBuffers.push_back(item); }
				else { sprintf_s(item, "%s (not in all three frames)", e.name); ownBuffers.push_back(item); }
				continue;
			}
			if (count < FlashRule::MaxStages) { in[count] = { prev, e.luma, next, e.name == Diagnostics::TonemapMarkName }; names[count] = e.name; count++; }
		}
		const float finalRel = FlashRule::RelativeJump(finalPrev, finalCur, finalNext, v.mask, v.sign);
		const FlashRule::Thresholds thresholds;
		const FlashRule::Attribution r = FlashRule::Attribute(in, count, v.mask, v.sign, finalRel, thresholds);
		if (r.source >= 0) {
			sprintf_s(item, "source: %s (its own jump %.1f %% of the flashing cells' brightness; the final picture jumped %.1f %%)", names[r.source],
				r.stage[r.source].own * 100.0f, finalRel * 100.0f);
			summary = item;
			std::string amplified, damped;
			for (int k = 0; k < r.count; k++) {
				if (!r.amplified[k] && !r.damped[k]) continue;
				sprintf_s(item, "%s%s %+.1f %%", (r.amplified[k] ? amplified : damped).empty() ? "" : ", ", names[k], r.stage[k].own * 100.0f);
				(r.amplified[k] ? amplified : damped) += item;
			}
			if (!amplified.empty()) summary += " | amplified by: " + amplified;
			if (!damped.empty()) summary += " | damped by: " + damped;
		}
		else if (r.largest >= 0) {
			sprintf_s(item, "source: no stage's own jump reached %.1f %% (the final picture jumped %.1f %%; the largest: %s %.1f %%): below the probes' resolution, or after Vegas Shaders' effects",
				(std::max)(thresholds.StageMin, thresholds.StageShare * finalRel) * 100.0f, finalRel * 100.0f, names[r.largest], r.largestOwn * 100.0f);
			summary = item;
		}
		else summary = "source: no stage was probed in all three frames (a late read-back)";
		// Stages that drew in the frames before and after but not in this one (a dropout by not drawing: the jump then appears at the next
		// probed stage, which the rule above names), and stages that drew only in this frame.
		{
			auto has = [](const Stages::Frame* f, const Stages::Entry& e, int occurrence) {
				if (!f) return false;
				int n = 0;
				for (unsigned j = 0; j < f->count; j++) if (f->e[j].kind == e.kind && !strcmp(f->e[j].name, e.name) && n++ == occurrence) return true;
				return false;
			};
			std::string missing, extra;
			if (fr[0] && fr[2]) {
				for (unsigned k = 0; k < fr[0]->count; k++) {	// in t-1 and t+1, not in t
					const Stages::Entry& e = fr[0]->e[k];
					if (e.kind != 0) continue;
					int occurrence = 0;
					for (unsigned j = 0; j < k; j++) if (fr[0]->e[j].kind == 0 && !strcmp(fr[0]->e[j].name, e.name)) occurrence++;
					if (!has(fr[2], e, occurrence) || has(fr[1], e, occurrence)) continue;
					missing += (missing.empty() ? "" : ", ") + std::string(e.name);
					// does it sit right before the source? (its place in t-1 is after the source's previous stage and before the source)
					if (r.source >= 0) {
						int sourceAt = -1, previousAt = -1;
						for (unsigned j = 0; j < fr[0]->count; j++) {
							if (fr[0]->e[j].kind != 0) continue;
							if (!strcmp(fr[0]->e[j].name, names[r.source]) && sourceAt < 0) sourceAt = (int)j;
							if (r.source > 0 && !strcmp(fr[0]->e[j].name, names[r.source - 1])) previousAt = (int)j;
						}
						if (sourceAt >= 0 && (int)k < sourceAt && (int)k > previousAt) {
							sprintf_s(item, "%s did not draw this frame (it drew in the frames before and after): the jump appears at %s, the next stage probed | ", e.name, names[r.source]);
							summary = item + summary;
						}
					}
				}
				for (unsigned k = 0; k < fr[1]->count; k++) {	// in t only
					const Stages::Entry& e = fr[1]->e[k];
					if (e.kind != 0) continue;
					int occurrence = 0;
					for (unsigned j = 0; j < k; j++) if (fr[1]->e[j].kind == 0 && !strcmp(fr[1]->e[j].name, e.name)) occurrence++;
					if (has(fr[0], e, occurrence) || has(fr[2], e, occurrence)) continue;
					extra += (extra.empty() ? "" : ", ") + std::string(e.name);
				}
			}
			if (!missing.empty()) summary += " | drew in the frames before and after but NOT in this one: " + missing;
			if (!extra.empty()) summary += " | drew ONLY in this frame: " + extra;
		}
		// the detail: own buffers, then every chain stage with its (cumulative) jump
		std::string chain;
		for (int k = 0; k < count; k++) {
			if (r.stage[k].valid) sprintf_s(item, "%s%s %.1f", k ? " > " : "", names[k], r.stage[k].rel * 100.0f);
			else sprintf_s(item, "%s%s (not in all three frames)", k ? " > " : "", names[k]);
			chain += item;
		}
		if (!ownBuffers.empty()) {
			detail = "own buffers (jump %): ";
			for (size_t i = 0; i < ownBuffers.size(); i++) detail += (i ? ", " : "") + ownBuffers[i];
			detail += " | ";
		}
		detail += "stages this frame (picture jump % after each, cumulative): " + chain;
	}

	// --- the detector's self-test (Diagnostics.h SelfTestSkip; [Main.Develop.Main] FlashSelfTestEvery / FlashSelfTestEffect) ---
	struct SelfTest {
		int every = 0; char effect[64] = "";
		unsigned skips = 0, verdicts = 0;
		unsigned frames[16] = {};	// the frames left out, awaiting the detector's verdict 3 frames later
	} T;

	// --- UNOFFICIAL 2026-11-01 step 2 (docs/next/FLICKER-ISOLATION-PLAN.md): period, region and conditions on every FLASH, and the beat
	// detector ([Main.Main.Misc] BeatDetect) for flicker under the one-frame rule: FlashRule.h BeatRule::JudgeCells on the last Window
	// frames of a still camera, every 8 frames; a BEAT line names the period and phase, the cells, the amplitude, the region and (with the
	// stage probes running) the stage whose series carries the beat (PhaseAmplitude per stage, differenced as the flash attribution). ---
	struct Beat {
		bool on = true, onRead = false;
		unsigned lastCheck = 0;
		unsigned checks = 0, minuteChecks = 0, beats = 0, minuteBeats = 0, lines = 0, notStill = 0, minuteNotStill = 0;
		double lastLine = -1e9, lastDump = -1e9;
	} Bt;

	const char* Ordinal(int n) { return n == 2 ? "nd" : n == 3 ? "rd" : "th"; }

	// The upscaler path, as the REPORT block prints it (the TAA state of the given frame; the rest as it is now)
	std::string UpscalingText(unsigned frame) {
		SettingManager* s = TheSettingManager;
		TAAEffect* taa = TheShaderManager->Effects.TAA;
		const char* names[] = { "FSR 2", "DLSS", "FSR 3.1" };
		const int up = taa ? taa->FSRSettings.Upscaler : -1;
		const bool taaOn = taa && taa->Enabled;
		const bool fsrOn = taaOn && taa->FSRSettings.Enabled;
		std::string taaState = "not run";
		if (taa && taa->StatusSlot >= 0 && taa->StatusSlot < SlotCount) taaState = StateText(Slots[taa->StatusSlot], frame);
		char text[400];
		_snprintf_s(text, _TRUNCATE, "%s, RenderScale %.2f, DLSS Ray Reconstruction %s, SMAA %s | TAA that frame: %s | temporal resolve %s, history %s",
			!taaOn ? "none (TAA off)" : !fsrOn ? "TAA" : (up >= 0 && up <= 2 ? names[up] : "FSR / DLSS"), fsrOn ? taa->FSRSettings.RenderScale : 1.0f,
			s->GetSettingI("Shaders.TAA.DLSS", "RayReconstruction") ? "on" : "off", s->GetSettingI("Shaders.SMAA.Status", "Enabled") ? "on" : "off",
			taaState.c_str(), TemporalBackbone::TemporalResolve() ? "on" : "off", TemporalBackbone::HistoryValid() ? "valid" : "reset this frame");
		return text;
	}

	// What a flash or beat happened under: the facts a report needs next to it (every piece is already privacy-safe: no names, no paths)
	std::string ConditionsText(unsigned frame) {
		const unsigned events = FrameJournal::CountFrame(frame - 1) + FrameJournal::CountFrame(frame) + FrameJournal::CountFrame(frame + 1);
		const double up = Seconds();
		char tail[160];
		_snprintf_s(tail, _TRUNCATE, " | journal events within 1 frame: %u | session uptime %u:%02u", events, (unsigned)(up / 60.0), (unsigned)up % 60);
		return "where " + SessionLog::Where() + ", weather " + SessionLog::Weather() + ", game time " + SessionLog::Clock() + " | runtime " +
			(TheRenderManager->D3D9RuntimeDescription() ? TheRenderManager->D3D9RuntimeDescription() : "?") + " | upscaling: " + UpscalingText(frame) + tail;
	}


	// --- UNOFFICIAL 2026-11-01 the history watch (Diagnostics.h part 5) ---
	constexpr int WatchSlots = 4, WatchRing = 64, WatchReadbacks = 4;
	constexpr float DriftLimit = 0.15f;		// the output's mean cell difference from its input, relative to the input's mean (floor 0.05)
	constexpr unsigned DriftFrames = 24;	// judged frames in a row over the limit: a RUNAWAY
	constexpr float CycleSpread = 0.005f;	// a CYCLE needs the pictures' means to differ by this much (a converged still TAA may flip a rounding bit)
	struct Watch {
		struct Readback { IDirect3DSurface9* in = nullptr; IDirect3DSurface9* out = nullptr; bool pending = false, hasIn = false; unsigned frame = 0; const char* quiet = nullptr; };
		struct Sample { unsigned frame = 0; bool valid = false, hasIn = false; const char* quiet = nullptr; float drift = 0.0f, meanIn = 0.0f, meanOut = 0.0f, maxOut = 0.0f; unsigned bad = 0; uint64_t hash = 0; };
		struct Slot {
			const char* name = nullptr;
			std::string (*state)() = nullptr;
			ShrinkChain in, out;
			D3DFORMAT inFormat = D3DFMT_UNKNOWN, outFormat = D3DFMT_UNKNOWN;
			Readback ring[WatchReadbacks];
			Sample samples[WatchRing];
			unsigned driftRun = 0, usedFrame = 0, judged = 0, minuteJudged = 0, late = 0, minuteLate = 0;
			unsigned runaways = 0, cycles = 0, nans = 0, lines = 0, minuteBad = 0;
			double lastRunawayLine = -1e9, lastNanLine = -1e9, lastCycleLine = -1e9;
			int cycleNow = 0, cycleDistinct = 0;	// the period the output repeats with now (0 none), as last judged
			unsigned cycleFrames = 0;				// judged frames the cycle has lasted
			float driftNow = 0.0f, driftMax = 0.0f;
			double driftSum = 0.0; unsigned driftCount = 0;	// the minute's average drift
		} slots[WatchSlots];
		int count = 0;
		bool on = true, onRead = false, failed = false, announced = false;
		double costSum = 0.0, minuteCost = 0.0;
		unsigned costFrames = 0, minuteCostFrames = 0;
	} Wt;

	void WatchRelease(Watch::Slot& s) {
		s.in.Release(); s.out.Release();
		for (auto& r : s.ring) { if (r.in) r.in->Release(); if (r.out) r.out->Release(); r = Watch::Readback(); }
		for (auto& m : s.samples) m.valid = false;
		s.inFormat = s.outFormat = D3DFMT_UNKNOWN;
	}
	void WatchOff(const char* why) {
		for (int k = 0; k < Wt.count; k++) WatchRelease(Wt.slots[k]);
		Wt.failed = true;
		Logger::Log("HISTORY watch off for this session: %s", why);
	}
	bool WatchFormatOk(D3DFORMAT f) { return FormatOk(f) || f == D3DFMT_A32B32G32R32F; }

	// One 8 x 8 system-memory copy -> 64 luminances (NaN cells count as bad with luma 0, infinite ones as bad with luma 4), the mean, the max
	bool WatchRead(IDirect3DSurface9* mem, D3DFORMAT format, float* luma, unsigned& bad, float& mean, float& peak, bool& late) {
		D3DLOCKED_RECT lock = {};
		const HRESULT hr = mem->LockRect(&lock, NULL, D3DLOCK_READONLY | D3DLOCK_DONOTWAIT);
		if (hr == D3DERR_WASSTILLDRAWING) { late = true; return false; }
		if (FAILED(hr)) { EffectStatus::D3DFail("history watch LockRect", hr); return false; }
		bad = 0; mean = 0.0f; peak = 0.0f;
		for (int y = 0; y < 8; y++) {
			const unsigned char* row = (const unsigned char*)lock.pBits + (size_t)y * lock.Pitch;
			for (int x = 0; x < 8; x++) {
				float red, green, blue;
				if (format == D3DFMT_A16B16G16R16F) {
					const unsigned short* p = (const unsigned short*)(row + x * 8);
					red = HalfToFloat(p[0]); green = HalfToFloat(p[1]); blue = HalfToFloat(p[2]);
				}
				else if (format == D3DFMT_A32B32G32R32F) {
					const float* p = (const float*)(row + x * 16);
					red = p[0]; green = p[1]; blue = p[2];
				}
				else if (format == D3DFMT_A2R10G10B10 || format == D3DFMT_A2B10G10R10) {
					const UInt32 v = *(const UInt32*)(row + x * 4);
					const float lo = (v & 1023) / 1023.0f, mid = ((v >> 10) & 1023) / 1023.0f, hi = ((v >> 20) & 1023) / 1023.0f;
					if (format == D3DFMT_A2R10G10B10) { blue = lo; green = mid; red = hi; } else { red = lo; green = mid; blue = hi; }
				}
				else {
					const unsigned char* p = row + x * 4;
					blue = p[0] / 255.0f; green = p[1] / 255.0f; red = p[2] / 255.0f;
				}
				float l = 0.2126f * red + 0.7152f * green + 0.0722f * blue;
				if (red != red || green != green || blue != blue) { bad++; l = 0.0f; }
				else if (fabsf(red) > 1e30f || fabsf(green) > 1e30f || fabsf(blue) > 1e30f) { bad++; l = 4.0f; }
				l = (std::min)((std::max)(l, 0.0f), 4.0f);
				luma[y * 8 + x] = l;
				mean += l;
				peak = (std::max)(peak, l);
			}
		}
		mem->UnlockRect();
		mean /= 64.0f;
		return true;
	}

	bool WatchMakeRing(IDirect3DDevice9* device, Watch::Slot& s, bool input, D3DFORMAT format) {
		for (auto& r : s.ring) {
			IDirect3DSurface9*& mem = input ? r.in : r.out;
			if (mem) { mem->Release(); mem = nullptr; }
			if (FAILED(device->CreateOffscreenPlainSurface(8, 8, format, D3DPOOL_SYSTEMMEM, &mem, NULL))) return false;
			r.pending = false;
		}
		return true;
	}

	// The judging of one read-back sample: the three rules, each with its line (once per 10 s, 300 lines a session per probe)
	void WatchJudge(Watch::Slot& s, Watch::Sample& m) {
		s.judged++; s.minuteJudged++;
		const double now = Seconds();
		std::string state;
		auto stateText = [&]() -> const char* { if (state.empty()) state = s.state ? s.state() : "?"; return state.c_str(); };
		auto cycleText = [&]() { char t[96]; if (s.cycleNow) sprintf_s(t, "repeats every %d frames (%d pictures)", s.cycleNow, s.cycleDistinct); else strcpy_s(t, "no exact repeat"); return std::string(t); };
		// 1. NaN / infinite cells in the output
		if (m.bad) {
			s.nans++; s.minuteBad++;
			if (now - s.lastNanLine >= 10.0 && s.lines < 300) {
				s.lastNanLine = now; s.lines++;
				Logger::Log("HISTORY %s f%u: NaN or infinite values in its output (%u of 64 cells; mean %.3f, max %.3f): a history that will never recover by itself | %s | %s",
					s.name, m.frame, m.bad, m.meanOut, m.maxOut, stateText(), ConditionsText(m.frame).c_str());
			}
		}
		// 2. the drift from the input (RUNAWAY): judged frames outside menus, loads, cuts and the overlay
		if (m.hasIn && !m.quiet) {
			s.driftNow = m.drift; s.driftSum += m.drift; s.driftCount++;
			s.driftMax = (std::max)(s.driftMax, m.drift);
			if (m.drift > DriftLimit) s.driftRun++; else s.driftRun = 0;
			if (s.driftRun >= DriftFrames && now - s.lastRunawayLine >= 10.0 && s.lines < 300) {
				s.lastRunawayLine = now; s.lines++; s.runaways++;
				Logger::Log("HISTORY %s f%u: RUNAWAY: its output has differed from its input by %.0f %% (the mean over the 8x8 picture, relative to the input's mean; limit %.0f %%) "
					"for %u frames in a row: a history that no longer converges on the picture (a smear, melt or wash-out) | input mean %.3f, output mean %.3f max %.3f | %s | %s | %s",
					s.name, m.frame, m.drift * 100.0f, DriftLimit * 100.0f, s.driftRun, m.meanIn, m.meanOut, m.maxOut, cycleText().c_str(), stateText(), ConditionsText(m.frame).c_str());
				StagesArm("a history runaway");
			}
		}
		else s.driftRun = 0;
		// 3. the cycle (every 8 judged frames, over the last Window frames: all read, none quiet, the pictures' means not all alike)
		if ((s.judged & 7) == 0 && m.frame + 1 >= (unsigned)CycleRule::Window) {
			uint64_t hashes[CycleRule::Window];
			bool valid[CycleRule::Window];
			float low = 1e9f, high = -1e9f;
			for (int i = 0; i < CycleRule::Window; i++) {
				const unsigned f = m.frame + 1 - CycleRule::Window + i;
				const Watch::Sample& o = s.samples[f % WatchRing];
				valid[i] = o.valid && o.frame == f && !o.quiet;
				hashes[i] = o.hash;
				if (valid[i]) { low = (std::min)(low, o.meanOut); high = (std::max)(high, o.meanOut); }
			}
			CycleRule::Result c = CycleRule::Period(hashes, valid, CycleRule::Window);
			if (c.period && (c.distinct < 3 || high - low < CycleSpread)) c.period = 0;
			if (c.period) { if (c.period == s.cycleNow) s.cycleFrames += 8; else { s.cycleNow = c.period; s.cycleDistinct = c.distinct; s.cycleFrames = CycleRule::Window; } }
			else { s.cycleNow = 0; s.cycleDistinct = 0; s.cycleFrames = 0; }
			if (c.period && now - s.lastCycleLine >= 10.0 && s.lines < 300) {
				s.lastCycleLine = now; s.lines++; s.cycles++;
				Logger::Log("HISTORY %s f%u: CYCLE: its output repeats exactly every %d frames (%d different pictures in the last %d frames, means %.3f-%.3f; %u frames so far): "
					"a history that is a function of the jitter step and the ping-pong alone, not of the picture any more | drift from its input %.0f %% | %s | %s",
					s.name, m.frame, c.period, c.distinct, CycleRule::Window, low, high, s.cycleFrames, s.driftNow * 100.0f, stateText(), ConditionsText(m.frame).c_str());
				StagesArm("a history cycle");
			}
		}
	}

	// UNOFFICIAL 2026-11-01 (the REPORT block's ray tracing line): the mean of a surface's first channel and of its luminance, read back
	// WAITING (a report is rare; the burst's fallback waits too). Formats: the detector's, plus R16F / G16R16F / A32B32G32R32F (the RT
	// outputs). False: could not be read (the reason in why).
	bool MeanOfSurface(IDirect3DDevice9* device, IDirect3DSurface9* surface, float& meanX, float& meanLuma, const char*& why) {
		why = nullptr;
		D3DSURFACE_DESC d = {};
		if (!device || !surface || FAILED(surface->GetDesc(&d))) { why = "no surface"; return false; }
		const D3DFORMAT f = d.Format;
		if (!(WatchFormatOk(f) || f == D3DFMT_R16F || f == D3DFMT_G16R16F)) { why = "format not handled"; return false; }
		ShrinkChain chain;
		IDirect3DSurface9* mem = nullptr;
		bool ok = chain.Make(device, d, d.Width, d.Height) && SUCCEEDED(chain.Run(device, surface)) &&
			SUCCEEDED(device->CreateOffscreenPlainSurface(8, 8, f, D3DPOOL_SYSTEMMEM, &mem, NULL)) && SUCCEEDED(device->GetRenderTargetData(chain.Last(), mem));
		D3DLOCKED_RECT lock = {};
		if (ok) ok = SUCCEEDED(mem->LockRect(&lock, NULL, D3DLOCK_READONLY));
		if (!ok) { why = "the shrink or read-back failed"; chain.Release(); if (mem) mem->Release(); return false; }
		double sumX = 0.0, sumL = 0.0;
		for (int y = 0; y < 8; y++) {
			const unsigned char* row = (const unsigned char*)lock.pBits + (size_t)y * lock.Pitch;
			for (int x = 0; x < 8; x++) {
				float r = 0.0f, g = 0.0f, b = 0.0f;
				if (f == D3DFMT_R16F) r = g = b = HalfToFloat(*(const unsigned short*)(row + x * 2));
				else if (f == D3DFMT_G16R16F) { const unsigned short* p = (const unsigned short*)(row + x * 4); r = HalfToFloat(p[0]); g = HalfToFloat(p[1]); b = 0.0f; }
				else if (f == D3DFMT_A16B16G16R16F) { const unsigned short* p = (const unsigned short*)(row + x * 8); r = HalfToFloat(p[0]); g = HalfToFloat(p[1]); b = HalfToFloat(p[2]); }
				else if (f == D3DFMT_A32B32G32R32F) { const float* p = (const float*)(row + x * 16); r = p[0]; g = p[1]; b = p[2]; }
				else if (f == D3DFMT_A2R10G10B10 || f == D3DFMT_A2B10G10R10) {
					const UInt32 v = *(const UInt32*)(row + x * 4);
					const float lo = (v & 1023) / 1023.0f, mid = ((v >> 10) & 1023) / 1023.0f, hi = ((v >> 20) & 1023) / 1023.0f;
					if (f == D3DFMT_A2R10G10B10) { b = lo; g = mid; r = hi; } else { r = lo; g = mid; b = hi; }
				}
				else { const unsigned char* p = row + x * 4; b = p[0] / 255.0f; g = p[1] / 255.0f; r = p[2] / 255.0f; }
				if (r != r) r = 0.0f;
				if (g != g) g = 0.0f;
				if (b != b) b = 0.0f;
				sumX += r;
				sumL += 0.2126f * r + 0.7152f * g + 0.0722f * b;
			}
		}
		mem->UnlockRect();
		mem->Release();
		chain.Release();
		meanX = (float)(sumX / 64.0);
		meanLuma = (float)(sumL / 64.0);
		return true;
	}

	std::string WatchSlotText(const Watch::Slot& s, bool minute) {
		char t[300];
		const double avg = s.driftCount ? s.driftSum / s.driftCount * 100.0 : 0.0;
		sprintf_s(t, "%s: %u frames judged, drift %s %.0f %% (max %.0f %%, now %.0f %%, over the limit for %u frames), %s, NaN frames %u, runaways %u, cycles %u, late read-backs %u",
			s.name, minute ? s.minuteJudged : s.judged, minute ? "avg" : "avg", avg, s.driftMax * 100.0, s.driftNow * 100.0, s.driftRun,
			s.cycleNow ? (std::string("repeating every ") + std::to_string(s.cycleNow) + " frames").c_str() : "no exact repeat", minute ? s.minuteBad : s.nans, s.runaways, s.cycles,
			minute ? s.minuteLate : s.late);
		return t;
	}

	// A flash at frame t: does the flashing part of the picture repeat? The mask's mean over the last frames through BeatRule::Find.
	std::string PeriodText(unsigned t, uint64_t mask) {
		float values[BeatRule::MaxValues];
		bool valid[BeatRule::MaxValues];
		unsigned frames[BeatRule::MaxValues];
		int count = 0;
		const unsigned first = t + 2 >= (unsigned)BeatRule::Window ? t + 2 - BeatRule::Window : 0;
		for (unsigned f = first; f <= t + 1 && count < BeatRule::MaxValues; f++, count++) {
			const Flash::Sample& s = F.samples[f % FinalRing];
			frames[count] = f;
			valid[count] = s.valid && s.frame == f && !s.quiet;
			values[count] = valid[count] ? FlashRule::MaskMean(s.luma, mask) : 0.0f;
		}
		const BeatRule::Result r = BeatRule::Find(values, valid, frames, count);
		char text[240];
		if (r.beat)
			_snprintf_s(text, _TRUNCATE, "period %d: every %d%s frame over the last %d frames (this frame %s one of them; %.1f x the noise)", r.period, r.period,
				Ordinal(r.period), r.used, (int)(t % r.period) == r.phase ? "IS" : "is NOT", r.score);
		else if (r.used >= 2 * BeatRule::MinPeriod + 1)
			_snprintf_s(text, _TRUNCATE, "single (no repeat in the last %d frames; the best period %d reached %.1f x the noise, %.0f needed)", r.used, r.period, r.score, BeatRule::MinScore);
		else _snprintf_s(text, _TRUNCATE, "no history to judge a period (%d usable frames)", r.used);
		return text;
	}

	// The stage attribution of a beat (period, phase, mask over the window frames): each chain stage's series on the mask through
	// PhaseAmplitude, differenced from the stage before as the flash attribution (re-based at the tonemap mark).
	void BeatStageText(unsigned t, const BeatRule::CellsResult& b, float finalRel, const unsigned* frames, const bool* windowValid, std::string& summary, std::string& detail) {
		detail.clear();
		const Stages::Frame* now = &S.frames[t % StageRing];
		if (now->frame != t || !now->count) {
			summary = S.failed ? "stages: the probes turned themselves off" : S.mode <= 0 ? "stages: probes off ([Main.Main.Misc] FlashStages 0)" :
				S.mode >= 2 ? "stages: not probed this frame" : "stages: not probed (the probes are armed now: the next beat line says which effect)";
			return;
		}
		float values[BeatRule::Window];
		bool valid[BeatRule::Window];
		const char* names[FlashRule::MaxStages];
		float rel[FlashRule::MaxStages], own[FlashRule::MaxStages];
		bool rebase[FlashRule::MaxStages];
		int count = 0, source = -1, largest = -1;
		float previous = 0.0f, largestOwn = 0.0f;
		std::vector<std::string> ownBuffers;
		char item[200];
		for (unsigned k = 0; k < now->count; k++) {
			const Stages::Entry& e = now->e[k];
			int occurrence = 0;
			for (unsigned j = 0; j < k; j++) if (now->e[j].kind == e.kind && !strcmp(now->e[j].name, e.name)) occurrence++;
			int have = 0;
			for (int i = 0; i < BeatRule::Window; i++) {
				valid[i] = false; values[i] = 0.0f;
				if (!windowValid[i]) continue;
				const Stages::Frame& f = S.frames[frames[i] % StageRing];
				if (f.frame != frames[i]) continue;
				int n = 0;
				for (unsigned j = 0; j < f.count; j++) {
					const Stages::Entry& o = f.e[j];
					if (o.kind != e.kind || strcmp(o.name, e.name)) continue;
					if (n++ == occurrence) { if (o.valid) { valid[i] = true; values[i] = FlashRule::MaskMean(o.luma, b.mask); have++; } break; }
				}
			}
			if (have < BeatRule::Window / 2) continue;	// (the probes came on during the window: not enough of its frames)
			const float amplitude = BeatRule::PhaseAmplitude(values, valid, frames, BeatRule::Window, b.period, b.phase);
			if (e.kind == 1) { _snprintf_s(item, _TRUNCATE, "%s %+.1f %%", e.name, amplitude * 100.0f); ownBuffers.push_back(item); continue; }
			if (count >= FlashRule::MaxStages) break;
			names[count] = e.name; rel[count] = amplitude; rebase[count] = e.name == Diagnostics::TonemapMarkName;
			own[count] = rebase[count] ? 0.0f : amplitude - previous;
			previous = amplitude;
			const float share = finalRel < 0.0f ? -own[count] : own[count];	// in the beat's direction
			if (!rebase[count] && share > largestOwn) { largestOwn = share; largest = count; }
			if (!rebase[count] && source < 0 && share >= (std::max)(0.005f, 0.3f * fabsf(finalRel))) source = count;
			count++;
		}
		if (source >= 0) {
			_snprintf_s(item, _TRUNCATE, "source: %s (its own beat %+.1f %% of the beating cells' brightness; the final picture's %+.1f %%)", names[source], own[source] * 100.0f, finalRel * 100.0f);
			summary = item;
		}
		else if (largest >= 0) {
			_snprintf_s(item, _TRUNCATE, "source: no stage's own beat reached %.1f %% (the final picture's %+.1f %%; the largest: %s %+.1f %%)",
				(std::max)(0.005f, 0.3f * fabsf(finalRel)) * 100.0f, finalRel * 100.0f, names[largest], own[largest] * 100.0f);
			summary = item;
		}
		else summary = "source: no stage probed over enough of the window";
		std::string chain;
		for (int k = 0; k < count; k++) { _snprintf_s(item, _TRUNCATE, "%s%s %+.1f", k ? " > " : "", names[k], rel[k] * 100.0f); chain += item; }
		if (!ownBuffers.empty()) {
			detail = "own buffers (beat %): ";
			for (size_t i = 0; i < ownBuffers.size(); i++) detail += (i ? ", " : "") + ownBuffers[i];
			detail += " | ";
		}
		detail += "stages (beat % after each, cumulative): " + chain;
	}

	// Every 8 frames, with Window frames of a still camera behind frame t
	void BeatCheck(unsigned t) {
		if (!Bt.on || t < (unsigned)BeatRule::Window || t - Bt.lastCheck < 8) return;
		Bt.lastCheck = t;
		const float* lumas[BeatRule::Window];
		bool valid[BeatRule::Window];
		unsigned frames[BeatRule::Window];
		for (int i = 0; i < BeatRule::Window; i++) {
			const unsigned f = t + 1 - BeatRule::Window + i;
			const Flash::Sample& s = F.samples[f % FinalRing];
			frames[i] = f;
			if (!s.valid || s.frame != f) { valid[i] = false; lumas[i] = nullptr; continue; }	// a late read-back: Find leaves it out
			if (s.quiet || s.turn > 0.05f) { Bt.notStill++; Bt.minuteNotStill++; return; }	// the camera moved, or a menu: the window is not judged
			valid[i] = true; lumas[i] = s.luma;
		}
		Bt.checks++; Bt.minuteChecks++;
		const BeatRule::CellsResult b = BeatRule::JudgeCells(lumas, valid, frames, BeatRule::Window);
		if (!b.beat) return;
		Bt.beats++; Bt.minuteBeats++;
		const double now = Seconds();
		if (Bt.lines >= 100 || now - Bt.lastLine < 10.0) return;
		Bt.lines++;
		Bt.lastLine = now;
		char region[48], cells[80];
		FlashRule::RegionText(b.mask, region);
		FlashRule::MaskText(b.mask, cells);
		float values[BeatRule::Window];
		for (int i = 0; i < BeatRule::Window; i++) values[i] = valid[i] ? FlashRule::MaskMean(lumas[i], b.mask) : 0.0f;
		const float finalRel = BeatRule::PhaseAmplitude(values, valid, frames, BeatRule::Window, b.period, b.phase);
		std::string summary, detail;
		BeatStageText(t, b, finalRel, frames, valid, summary, detail);
		Logger::Log("BEAT at frame %u: every %d%s frame (frames with frame %% %d == %d), %d of 64 cells %s by %.3f (%.1f levels, %+.1f %%) over the last %d frames, "
			"%.1f x their noise, in %s | cells, top row first: %s | %s ([Main.Main.Misc] BeatDetect, one line per 10 s)",
			t, b.period, Ordinal(b.period), b.period, b.phase, b.cells, b.amplitude < 0.0 ? "darker" : "brighter", fabs(b.amplitude), fabs(b.amplitude) * 255.0,
			finalRel * 100.0f, BeatRule::Window, b.score, region, cells, summary.c_str());
		if (!detail.empty()) Logger::Log("BEAT %u %s", t, detail.c_str());
		if (now - Bt.lastDump >= 60.0) {
			Bt.lastDump = now;
			Logger::Log("BEAT %u conditions: %s", t, ConditionsText(t).c_str());
			char head[48];
			sprintf_s(head, "BEAT %u event", t);
			FrameJournal::Dump(head, t + 1 - BeatRule::Window, t);
		}
		StagesArm("a beat");
	}

	void Judge(unsigned frame) {
		const Flash::Sample& a = F.samples[(frame - 2) % FinalRing];
		const Flash::Sample& b = F.samples[(frame - 1) % FinalRing];
		const Flash::Sample& c = F.samples[frame % FinalRing];
		if (!a.valid || !b.valid || !c.valid || a.frame != frame - 2 || b.frame != frame - 1 || c.frame != frame) return;
		const FlashRule::Verdict v = FlashRule::Judge(a.luma, b.luma, c.luma);
		const char* quiet = a.quiet ? a.quiet : b.quiet ? b.quiet : c.quiet;
		// UNOFFICIAL 2026-11-01 the self-test: the verdict on every frame it left an effect out of, flash or not (the end-to-end check)
		for (unsigned k = 0; k < 16; k++) {
			if (T.frames[k] != b.frame || !b.frame) continue;
			T.frames[k] = 0;
			if (T.verdicts++ < 40)
				Logger::Log("FLASH self-test frame %u (%s left out) verdict: %s | %d cells darker, %d brighter, largest jump %.3f, picture mean %.3f -> %.3f -> %.3f%s%s",
					b.frame, T.effect, quiet ? "NOT JUDGED" : v.flash ? "FLASH" : "no flash (under the thresholds)", v.darker, v.brighter, fabsf(v.largest),
					v.meanPrev, v.meanThis, v.meanNext, quiet ? " | quiet: " : "", quiet ? quiet : "");
		}
		if (quiet) {
			if (v.flash) { F.minuteIgnored++; F.minuteIgnoredWhy = quiet; }
			return;
		}
		F.judged++; F.minuteJudged++;
		if (!v.flash) return;
		F.flashes++; F.minuteFlashes++;
		const unsigned t = b.frame;
		char cells[80];
		FlashRule::MaskText(v.mask, cells);
		const int count = v.sign < 0 ? v.darker : v.brighter;
		const float change = v.meanPrev + v.meanNext > 1e-4f ? (v.meanThis / (0.5f * (v.meanPrev + v.meanNext)) - 1.0f) * 100.0f : 0.0f;
		char region[48];
		FlashRule::RegionText(v.mask, region);	// UNOFFICIAL 2026-11-01 step 2: where, and does it repeat
		const std::string period = PeriodText(t, v.mask);
		char text[1200];
		_snprintf_s(text, _TRUNCATE, "FLASH at frame %u: %d of 64 cells %s by up to %.3f%s%s%s (picture mean %.3f -> %.3f -> %.3f, %+.0f %%) in %s | cells, top row first: %s | "
			"camera turn %.1f deg, frame %.1f ms | %s",
			t, count, v.sign < 0 ? "darker" : "brighter", fabsf(v.largest), v.whole ? ", the whole picture" : "", v.black ? ", a BLACK frame" : "",
			v.white ? ", a WHITE frame" : "", v.meanPrev, v.meanThis, v.meanNext, change, region, cells, b.turn, b.ms, period.c_str());
		// UNOFFICIAL 2026-11-01 the stage probes: which stage's own output jumped (summary on the FLASH line, detail in the dump)
		std::string stageSummary, stageDetail;
		StageText(t, v, a.luma, b.luma, c.luma, stageSummary, stageDetail);
		const double now = Seconds();
		if (F.dumps < 50 && now - F.lastDump >= 10.0) {
			F.dumps++;
			F.lastDump = now;
			Logger::Log("%s | %s -- automatic flash detector dump %u of at most 50 ([Main.Main.Misc] FlashDetect); paste from here to 'FLASH %u end' into a bug report",
				text, stageSummary.c_str(), F.dumps, t);
			if (!stageDetail.empty()) Logger::Log("FLASH %u %s", t, stageDetail.c_str());
			char head[48];
			sprintf_s(head, "FLASH %u event", t);
			FrameJournal::Dump(head, t > 30 ? t - 30 : 0, t + 3);
			sprintf_s(head, "FLASH %u", t);
			DumpEffects(head, t);
			DumpLevels(head);
			Logger::Log("FLASH %u conditions: %s", t, ConditionsText(t).c_str());	// UNOFFICIAL 2026-11-01 step 2 (was: where / weather / time only)
			Logger::Log("FLASH %u end", t);
		}
		else if (F.shortLines < 300 && now - F.lastShort >= 1.0) {
			F.shortLines++;
			F.lastShort = now;
			Logger::Log("%s | %s (no dump: %s)", text, stageSummary.c_str(), F.dumps >= 50 ? "50 dumps written this session" : "one dump per 10 s");
		}
		StagesArm("a flash");	// UNOFFICIAL 2026-11-01: armed mode: the probes run for the next 60 s so the next flash is attributed
	}

	void FlashFrame(IDirect3DDevice9* device, IDirect3DSurface9* picture, unsigned frame) {
		const double start = Seconds();
		// The frame's own circumstances, kept with its readback
		const D3DXVECTOR4& f4 = TheRenderManager->CameraForward;
		D3DXVECTOR3 forward(f4.x, f4.y, f4.z);
		const float length = D3DXVec3Length(&forward);
		if (length > 1e-4f) forward /= length;
		float turn = 0.0f;
		if (D3DXVec3LengthSq(&F.lastForward) > 0.5f) turn = acosf((std::min)(1.0f, (std::max)(-1.0f, D3DXVec3Dot(&forward, &F.lastForward)))) * 57.29578f;
		F.lastForward = forward;
		TESObjectCELL* cell = Player ? Player->parentCell : nullptr;
		const bool loading = InterfaceManager && InterfaceManager->IsActive(Menu::kMenuType_Loading);
		if (cell != F.lastCell || TheShaderManager->GameState.isCellChanged || F.wasLoading != loading) F.quietUntil = frame + 60;
		F.lastCell = cell; F.wasLoading = loading;
		const double now = TheFrameRateManager->Time;
		const float ms = F.lastTime >= 0.0 ? (float)((now - F.lastTime) * 1000.0) : 0.0f;
		F.lastTime = now;

		D3DSURFACE_DESC d = {};
		if (FAILED(picture->GetDesc(&d))) return;
		if (!FormatOk(d.Format)) { char why[96]; sprintf_s(why, "picture format %d not handled", (int)d.Format); FlashOff(why); return; }
		if (!F.shrink.Fits(d, d.Width, d.Height)) {
			if (!MakeFlash(device, d)) { FlashOff("could not make its small render targets / read-back surfaces"); return; }
		}
		// Shrink: 2:1 steps with bilinear filtering average 2 x 2 texels exactly
		const HRESULT shrunk = F.shrink.Run(device, picture);
		if (FAILED(shrunk)) { EffectStatus::D3DFail("flash detector StretchRect", shrunk); FlashOff("StretchRect failed"); return; }
		Flash::Readback& write = F.ring[frame % 4];
		const HRESULT copied = device->GetRenderTargetData(F.shrink.Last(), write.mem);
		write.pending = SUCCEEDED(copied);
		if (FAILED(copied)) EffectStatus::D3DFail("flash detector GetRenderTargetData", copied);
		write.frame = frame; write.quiet = QuietReason(frame, turn); write.turn = turn; write.ms = ms;

		// The copy made 3 frames ago
		Flash::Readback& read = F.ring[(frame + 1) % 4];
		if (read.pending) {
			read.pending = false;
			Flash::Sample& s = F.samples[read.frame % FinalRing];
			s.valid = ReadLuma(read, s.luma);
			s.frame = read.frame; s.quiet = read.quiet; s.turn = read.turn; s.ms = read.ms;
			if (s.valid) Judge(read.frame);
			if (s.valid) BeatCheck(read.frame);	// UNOFFICIAL 2026-11-01 step 2: the beat detector over the last Window frames
		}

		const double cost = Seconds() - start;
		F.costSum += cost; F.costFrames++; F.minuteCost += cost; F.minuteCostFrames++;
		if (F.costFrames == 600) {
			const double average = F.costSum / F.costFrames * 1000.0;
			F.costSum = 0.0; F.costFrames = 0;
			if (average > 0.5) {
				char why[160];
				sprintf_s(why, "it cost %.2f ms a frame on average here (over 0.5 ms: this driver's read-back waits)", average);
				FlashOff(why);
			}
		}
	}

	// --- the report key ---
	unsigned Reports = 0;

	// --- the report burst (user 2026-11-01: "make the report capture 15-20 frames and not skip any ... with time stamps directly
	// correlated to the same frames logged in the log") ---
	// The key press frame and the next N-1 rendered frames ([Main.Main.Misc] ReportFrames, 16, 1..32), each one:
	//  - its finished picture (before the HUD) StretchRect'ed into its own render target of a GPU ring (no wait: a copy on the GPU).
	//    The ring is made at the press and released when the pictures are read back: the picture's size if N pictures fit in
	//    RingBudget (300 MB), else half, else a quarter (a 2:1 / 4:1 linear stretch). A multisampled picture is resolved first.
	//  - the AvgLuma effect's 1 x 1 result (scene luma, adapted luma: what Exposure uses) StretchRect'ed into texel k of an N x 1 row;
	//  - one "REPORT n frame k/N f<frame>" line written in that frame (so the logger's "[time fNNNN]" prefix is that frame's).
	// After the burst, one picture a frame is read back (GetRenderTargetData into one of 2 system-memory surfaces, locked a frame
	// later without waiting, after 4 frames with waiting) and handed to a worker thread that converts it, measures its mean luma and
	// writes the PNG with WIC (no device); it logs one "REPORT n picture k/N f<frame>" line each. At most PendingBudget bytes wait
	// for the worker (the read-back waits for it, never the capture). If the ring cannot be made, every burst frame is read back at
	// once instead (the frame waits for the GPU: logged) at a size whose 8-bit pictures fit in FallbackBudget.
	// A press while a burst runs or its pictures are written is ignored (logged). The flash detector keeps running during the burst.
	constexpr unsigned MaxBurst = 32;
	constexpr double RingBudget = 300.0 * 1048576.0, FallbackBudget = 150.0 * 1048576.0, PendingBudget = 64.0 * 1048576.0;

	// A locked picture -> 8-bit BGRA, averaged scale x scale (1: as it is), and its mean luma (display values, floats clamped to 0..4
	// as the flash detector does). 8-bit formats are taken as they are; float formats clamped to 0-1 for the PNG.
	void ConvertPicture(D3DFORMAT format, const unsigned char* bits, unsigned pitch, unsigned w, unsigned h, unsigned scale,
		std::vector<unsigned>& bgra, unsigned& ow, unsigned& oh, double& meanLuma) {
		ow = (std::max)(1u, w / scale); oh = (std::max)(1u, h / scale);
		bgra.assign((size_t)ow * oh, 0xFF000000u);
		std::vector<float> row((size_t)w * 4), sum((size_t)ow * 3);
		double lumaSum = 0.0;
		const float area = 1.0f / (scale * scale);
		for (unsigned oy = 0; oy < oh; oy++) {
			std::fill(sum.begin(), sum.end(), 0.0f);
			for (unsigned sy = 0; sy < scale; sy++) {
				const unsigned y = (std::min)(h - 1, oy * scale + sy);
				SelfTestCore::ConvertRow(format, bits + (size_t)y * pitch, w, row.data());
				for (unsigned ox = 0; ox < ow; ox++)
					for (unsigned sx = 0; sx < scale; sx++) {
						const float* c = &row[(size_t)(std::min)(w - 1, ox * scale + sx) * 4];
						for (int ch = 0; ch < 3; ch++) sum[ox * 3 + ch] += (c[ch] == c[ch] ? (std::min)((std::max)(c[ch], 0.0f), 4.0f) : 0.0f);
					}
			}
			unsigned* out = &bgra[(size_t)oy * ow];
			for (unsigned ox = 0; ox < ow; ox++) {
				const float r = sum[ox * 3] * area, g = sum[ox * 3 + 1] * area, b = sum[ox * 3 + 2] * area;
				lumaSum += 0.2126f * r + 0.7152f * g + 0.0722f * b;
				out[ox] = 0xFF000000u | (SelfTestCore::Byte(r) << 16) | (SelfTestCore::Byte(g) << 8) | SelfTestCore::Byte(b);
			}
		}
		meanLuma = lumaSum / ((double)ow * oh);
	}

	struct PictureJob {
		std::vector<unsigned char> raw;	// the locked rows (pitch = w * bytes per pixel), or empty when bgra is ready (the fallback)
		std::vector<unsigned> bgra;
		D3DFORMAT format = D3DFMT_UNKNOWN;
		unsigned w = 0, h = 0, pitch = 0;
		double meanLuma = -1.0;
		unsigned n = 0, k = 0, count = 0, frame = 0;
		char file[128] = "";
		char exposure[160] = "";
		const char* failed = nullptr;	// no picture: why
	};

	struct Burst {
		enum Phase { Idle, Capturing, Draining, Writing } phase = Idle;
		// UNOFFICIAL 2026-11-01 (user: the report's pictures, the RT helper's log and the other files in one place): the report's own folder
		// in the logs folder, NVR-Report-<date>-<n> (MakeReportFolder); the logs folder itself when it cannot be made.
		char folder[MAX_PATH] = ".\\" VS_LOGS_FOLDER;
		char folderName[96] = VS_LOGS_FOLDER;
		unsigned n = 0, count = 0, next = 0;	// report number, N, the next frame's index
		unsigned frames[MaxBurst] = {};
		bool avgDrawn[MaxBurst] = {}, exposureOn[MaxBurst] = {};
		char date[32] = "";
		char files[MaxBurst][96] = {};
		D3DFORMAT format = D3DFMT_UNKNOWN;
		unsigned width = 0, height = 0, ringW = 0, ringH = 0, scale = 1;
		bool fallback = false;
		IDirect3DSurface9* ring[MaxBurst] = {};
		IDirect3DSurface9* resolve = nullptr;	// a multisampled picture, resolved at full size first
		IDirect3DSurface9* sys[2] = {};		// read-back (ring size; full size in the fallback)
		int issued[2] = { -1, -1 };
		unsigned issuedFrame[2] = {};
		unsigned drainNext = 0, drained = 0, captureFails = 0;
		IDirect3DSurface9* lumaRow = nullptr;	// N x 1, the AvgLuma result of each burst frame
		IDirect3DSurface9* lumaSys = nullptr;
		D3DFORMAT lumaFormat = D3DFMT_UNKNOWN;
		float scene[MaxBurst] = {}, adapted[MaxBurst] = {};
		bool lumaRead = false, lumaTried = false;
		double startTime = 0.0;
		// previous frame's counters (the per-frame counts are differences)
		unsigned lastLampSlots = 0, lastCascades = 0, lastResets = 0;
		double lastFrameTime = -1.0;
		// the worker
		HANDLE worker = NULL;
		std::mutex lock;
		std::vector<PictureJob*> queue;
		std::vector<PictureJob*> held;	// the fallback's pictures until the burst's end (their exposure text needs the AvgLuma row)
		HANDLE wake = NULL;
		volatile LONG pendingBytes = 0;
		volatile LONG written = 0;
		volatile bool closed = false;
	} B;

	unsigned Since(unsigned now, unsigned& last) {	// a counter's change (the heartbeat zeroes them every minute)
		const unsigned d = now >= last ? now - last : now;
		last = now;
		return d;
	}

	void ReleaseRing() {
		for (IDirect3DSurface9*& s : B.ring) if (s) { s->Release(); s = nullptr; }
		if (B.resolve) { B.resolve->Release(); B.resolve = nullptr; }
		if (B.lumaRow) { B.lumaRow->Release(); B.lumaRow = nullptr; }
		if (B.lumaSys) { B.lumaSys->Release(); B.lumaSys = nullptr; }
	}
	void ReleaseReadback() {
		for (IDirect3DSurface9*& s : B.sys) if (s) { s->Release(); s = nullptr; }
		B.issued[0] = B.issued[1] = -1;
	}

	// UNOFFICIAL 2026-11-01 report folder (user: "rt helper logs, the screenshots, and the other files end up in the same place ... a newly
	// created folder ... named with a time stamp; if we can't create a new folder, just have all of the files land in the nvr-optimized
	// folder"). Made at the key press, before the REPORT block names it.
	void MakeReportFolder(unsigned n) {
		CreateDirectoryA(".\\" VS_LOGS_FOLDER, NULL);
		char name[96], path[MAX_PATH];
		sprintf_s(name, "NVR-Report-%s-%u", B.date, n);
		sprintf_s(path, ".\\" VS_LOGS_FOLDER "\\%s", name);
		if (CreateDirectoryA(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS) {
			strcpy_s(B.folder, path);
			sprintf_s(B.folderName, VS_LOGS_FOLDER "\\%s", name);
		}
		else {
			strcpy_s(B.folder, ".\\" VS_LOGS_FOLDER);
			strcpy_s(B.folderName, VS_LOGS_FOLDER " (its own folder could not be made)");
		}
	}

	// One file copied as it is now, even while its writer has it open (shared read; at most 64 MB). False: missing or unreadable.
	bool CopyShared(const std::wstring& from, const std::wstring& to) {
		HANDLE in = CreateFileW(from.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (in == INVALID_HANDLE_VALUE) return false;
		LARGE_INTEGER size = {};
		bool ok = GetFileSizeEx(in, &size) && size.QuadPart <= 64ll * 1024 * 1024;
		std::vector<char> data(ok ? (size_t)size.QuadPart : 0);
		DWORD got = 0;
		ok = ok && (data.empty() || (ReadFile(in, data.data(), (DWORD)data.size(), &got, NULL) && got == data.size()));
		CloseHandle(in);
		if (!ok) return false;
		HANDLE out = CreateFileW(to.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (out == INVALID_HANDLE_VALUE) return false;
		DWORD wrote = 0;
		ok = data.empty() || (WriteFile(out, data.data(), (DWORD)data.size(), &wrote, NULL) && wrote == data.size());
		CloseHandle(out);
		return ok;
	}

	// The newest file matching a pattern in a folder (empty: none)
	std::wstring Newest(const std::wstring& folder, const wchar_t* pattern) {
		WIN32_FIND_DATAW fd;
		HANDLE h = FindFirstFileW((folder + L"\\" + pattern).c_str(), &fd);
		if (h == INVALID_HANDLE_VALUE) return std::wstring();
		std::wstring best; FILETIME bestTime = {};
		do {
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
			if (best.empty() || CompareFileTime(&fd.ftLastWriteTime, &bestTime) > 0) { best = fd.cFileName; bestTime = fd.ftLastWriteTime; }
		} while (FindNextFileW(h, &fd));
		FindClose(h);
		return best.empty() ? best : folder + L"\\" + best;
	}

	// After the pictures: this session's NVR log (as it is now, the report's lines included), the RT and DLSS helpers' logs, DXVK's log,
	// the newest FNVProbe log and the settings file, copied into the report's folder (each one only when it is there). When the
	// report folder could not be made: into the logs folder itself, each name led by the report's (MakeReportFolder's fallback).
	void CopyReportFiles(unsigned n) {
		// (No folder of its own: into the logs folder, each name led by the report's, and not this log or FNVProbe's, which live there.)
		const bool own = strcmp(B.folder, ".\\" VS_LOGS_FOLDER) != 0;
		wchar_t prefix[64] = L"";
		if (!own) swprintf_s(prefix, L"NVR-Report-%hs-%u - ", B.date, n);
		wchar_t exePath[MAX_PATH] = {};
		const DWORD length = GetModuleFileNameW(NULL, exePath, MAX_PATH);
		std::wstring game(exePath, length);
		game.resize(length && length < MAX_PATH ? game.find_last_of(L"\\/") : 0);
		if (game.empty()) game = L".";
		wchar_t folderW[MAX_PATH] = {};
		MultiByteToWideChar(CP_ACP, 0, B.folder, -1, folderW, MAX_PATH);
		const std::wstring folder = game + L"\\" + (folderW + 2);	// (B.folder starts with ".\\")
		if (Logger::LogFile) fflush(Logger::LogFile);
		std::wstring nvrLog;
		if (!Logger::FilePath.empty()) {
			wchar_t w[MAX_PATH] = {};
			MultiByteToWideChar(CP_ACP, 0, Logger::FilePath.c_str(), -1, w, MAX_PATH);
			nvrLog = w;
		}
		const std::wstring logs = game + L"\\" VS_LOGS_FOLDER;
		// The settings file in use: the new name, or the old one while it is the one read (Paths.h).
		const std::wstring plugins = game + L"\\Data\\NVSE\\Plugins\\";
		const bool onlyOld = !Paths::OldPathIfRead(Paths::PluginsDir(), Paths::File::UserSettings).empty();
		wchar_t tomlName[64] = {};
		MultiByteToWideChar(CP_ACP, 0, onlyOld ? Paths::OldName(Paths::File::UserSettings) : Paths::NewName(Paths::File::UserSettings), -1, tomlName, 64);
		// FNVProbe (its own plugin) may still write into the old logs folder (Paths::OldLogsFolder): the newest there when the new one has none
		wchar_t oldLogsW[64] = {};
		MultiByteToWideChar(CP_ACP, 0, Paths::OldLogsFolder, -1, oldLogsW, 64);
		std::wstring probeLog = Newest(logs, L"FNVProbe_*.log");
		const bool probeInLogs = !probeLog.empty();
		if (!probeInLogs) probeLog = Newest(game + L"\\" + oldLogsW, L"FNVProbe_*.log");
		struct Item { std::wstring from; const wchar_t* to; };
		const Item items[] = {
			{ nvrLog, L"VegasShaders.log" },
			{ probeLog, L"FNVProbe.log" },
			{ Newest(game + L"\\Data\\NVSE\\Plugins\\NvrRt", L"NvrRtHelper*.log"), L"NvrRtHelper.log" },
			{ Newest(game, L"NvrRtHelper*.log"), L"NvrRtHelper (game folder).log" },
			{ Newest(game + L"\\Data\\NVSE\\Plugins\\NvrDlss", L"NvrDlssHelper*.log"), L"NvrDlssHelper.log" },
			{ Newest(game, L"NvrDlssHelper*.log"), L"NvrDlssHelper (game folder).log" },
			{ game + L"\\FalloutNV_d3d9.log", L"FalloutNV_d3d9.log" },
			{ plugins + tomlName, tomlName },
		};
		std::string copied, missing;
		for (const Item& item : items) {
			char name[96];
			WideCharToMultiByte(CP_ACP, 0, item.to, -1, name, sizeof(name), NULL, NULL);
			if (!own && (&item == &items[0] || (&item == &items[1] && probeInLogs))) continue;	// (already in the logs folder)
			if (!item.from.empty() && CopyShared(item.from, folder + L"\\" + prefix + item.to)) copied += copied.empty() ? name : std::string(", ") + name;
			else missing += missing.empty() ? name : std::string(", ") + name;
		}
		Logger::Log("REPORT %u files: copied into %s: %s%s%s", n, B.folderName, copied.empty() ? "nothing" : copied.c_str(),
			missing.empty() ? "" : " | not there (or not readable): ", missing.c_str());
	}

	DWORD WINAPI BurstWorker(LPVOID) {
		unsigned done = 0, saved = 0, n = B.n, count = B.count;
		while (true) {
			PictureJob* job = nullptr;
			{
				std::lock_guard<std::mutex> guard(B.lock);
				if (!B.queue.empty()) { job = B.queue.front(); B.queue.erase(B.queue.begin()); }
			}
			if (!job) {
				if (B.closed) {	// (closed is set after the last push: look once more)
					std::lock_guard<std::mutex> guard(B.lock);
					if (B.queue.empty()) break;
					continue;
				}
				WaitForSingleObject(B.wake, 200);
				continue;
			}
			const ULONGLONG start = GetTickCount64();
			bool ok = false;
			unsigned w = job->w, h = job->h;
			if (!job->failed) {
				if (!job->raw.empty()) {
					ConvertPicture(job->format, job->raw.data(), job->pitch, job->w, job->h, 1, job->bgra, w, h, job->meanLuma);
					InterlockedExchangeAdd(&B.pendingBytes, -(LONG)job->raw.size());
					std::vector<unsigned char>().swap(job->raw);
				}
				char path[MAX_PATH];
				sprintf_s(path, "%s\\%s", B.folder, job->file);	// (UNOFFICIAL 2026-11-01: the report's folder)
				ok = SelfTestCore::SavePng(path, w, h, job->bgra);
			}
			done++;
			if (ok) saved++;
			if (job->failed || !ok)
				Logger::Log("REPORT %u picture %u/%u f%u: NOT saved (%s) | %s", job->n, job->k, job->count, job->frame, job->failed ? job->failed : "the PNG write failed",
					job->exposure);
			else
				Logger::Log("REPORT %u picture %u/%u f%u: %s %ux%u, mean luma %.4f | %s | written in %llu ms", job->n, job->k, job->count, job->frame, job->file, w, h,
					job->meanLuma, job->exposure, GetTickCount64() - start);
			delete job;
			InterlockedExchange(&B.written, (LONG)saved);
		}
		CopyReportFiles(n);	// UNOFFICIAL 2026-11-01: the logs next to the pictures
		Logger::Log("REPORT %u pictures done: %u of %u written to %s -- the end of report %u", n, saved, count, B.folderName, n);
		return 0;
	}

	void Push(PictureJob* job) {
		{
			std::lock_guard<std::mutex> guard(B.lock);
			B.queue.push_back(job);
		}
		SetEvent(B.wake);
	}

	// The exposure text of burst frame k (after the N x 1 row was read back)
	void ExposureText(unsigned k, char* out, size_t size) {
		ExposureEffect* e = TheShaderManager->Effects.Exposure;
		if (!B.lumaRead) { sprintf_s(out, size, "exposure: AvgLuma result not read back"); return; }
		const float scene = B.scene[k], adapted = B.adapted[k];
		const char* stale = B.avgDrawn[k] ? "" : " (AvgLuma not drawn this frame: an older value)";
		if (B.exposureOn[k] && e) {	// Exposure.fx.hlsl: lumaDiff from the adapted luma and MinBrightness, blended by MaxBrightness
			const float minB = e->Constants.Data.x, maxB = e->Constants.Data.y;
			const float a = (std::max)(adapted, 1e-6f);
			const float lumaDiff = ((std::max)(logf(a / (std::max)(minB, 1e-6f)), 0.0f) + 1.0f) / ((std::max)(logf((std::max)(minB, 1e-6f) / a), 0.0f) + 1.0f);
			const float gain = 1.0f + (1.0f / lumaDiff - 1.0f) * maxB;
			sprintf_s(out, size, "exposure: scene luma %.4f, adapted %.4f, picture gain x%.3f (MinBrightness %.2f, MaxBrightness %.2f)%s", scene, adapted, gain, minB, maxB, stale);
		}
		else sprintf_s(out, size, "exposure off | AvgLuma scene luma %.4f, adapted %.4f%s", scene, adapted, stale);
	}

	void ReadLumaRow(IDirect3DDevice9* device) {
		B.lumaRead = false;
		if (!B.lumaRow || !B.lumaSys || !SelfTestCore::KnownFormat(B.lumaFormat)) return;
		HRESULT hr = device->GetRenderTargetData(B.lumaRow, B.lumaSys);
		D3DLOCKED_RECT lock = {};
		if (SUCCEEDED(hr)) hr = B.lumaSys->LockRect(&lock, NULL, D3DLOCK_READONLY);	// (one wait: N texels)
		if (FAILED(hr)) { EffectStatus::D3DFail("report burst AvgLuma read-back", hr); return; }
		std::vector<float> row((size_t)B.count * 4);
		SelfTestCore::ConvertRow(B.lumaFormat, (const unsigned char*)lock.pBits, B.count, row.data());
		B.lumaSys->UnlockRect();
		for (unsigned k = 0; k < B.count; k++) { B.scene[k] = row[k * 4]; B.adapted[k] = row[k * 4 + 1]; }
		B.lumaRead = true;
	}

	PictureJob* NewJob(unsigned k) {
		PictureJob* job = new PictureJob();
		job->n = B.n; job->k = k + 1; job->count = B.count; job->frame = B.frames[k];
		strcpy_s(job->file, B.files[k]);
		ExposureText(k, job->exposure, sizeof(job->exposure));
		return job;
	}

	// --- the report burst's volumetric light stages (VL stages, 2026-11-01; docs/next/FLICKER-PROGRAM.md "Established facts": the
	// Strip flicker IS VolumetricLight, a steady 3-frame beat in the upper half; next: look inside it per frame) ---
	// In each burst frame (frames 2..N: the key is read at the end of frame 1, after its passes) the effect's stages are shrunk on the
	// GPU to 8 x 8 (ShrinkChain, in their own format: the half-float buffers stay half float, nothing is clamped) and copied into block
	// k of the stage's (8 N) x 8 row:
	//   march    = the march's output (technique 1: the March buffer, or Temp with Temporal off), right after the pass;
	//   history  = the History buffer right before the temporal pass reads it: last frame's temporal result (the copy that replaces it
	//              comes after the pass); Temporal on only;
	//   temporal = technique 3's output (Temp), before it is copied into History; Temporal on only;
	//   blur     = technique 4's output (the Buffer the composite reads);
	//   frame before VL / after VL = the picture the composite reads / the one it wrote (ShaderManager.cpp, right around it).
	// With them the effect's values that frame (Diagnostics::VLValues) and each pass's path (its Vulkan pass or its D3D9 technique).
	// After the burst the rows are read back (GetRenderTargetData at the end of its last frame, locked without waiting in the frames
	// after, waited for after 4) and written: a "REPORT n VL buffers" line (what the channels hold), one "REPORT n VL k/N f<frame>" line
	// per burst frame (the means of the upper and the lower half of each stage's 8 x 8, r/g/b/a) and a "REPORT n VL pattern" line (each
	// stage's upper-half value over the burst, and the first stage in the order march -> temporal -> blur -> composite whose value beats:
	// FlashRule.h BeatRule). Outside a burst Diagnostics::VLCapturing is false (the hooks test only that) and nothing is allocated: the
	// targets are made at a burst's first capture and released after its read-back. A failed allocation or copy is logged once and only
	// that stage is left out.
	//
	// The volumetric light buffers (VolumetricLight.fx.hlsl MarchPS, TemporalPS, BlurAxis; half resolution, A16B16G16R16F):
	//   r = the lit air's share of the view: the sunlight it scatters towards the eye (before Strength x phase),
	//   g = the same share if all the air were lit (so g - r = the shaded air),
	//   b = depth / farZ (for the depth-aware passes), a = 1 (written, never read).
	// The composite (CompositeAir, in linear light): frame x (1 - Darken x Fade x (g - r)) + sun colour x Strength x Fade x phase x r.
	// So the darkening of the distant air is g - r, not alpha.
	const char* const VLNames[Diagnostics::VLStageCount] = { "march", "history", "temporal", "blur", "frame before VL", "after VL" };

	struct VLSlot {
		unsigned frame = 0;	// the frame this slot was last written in (0: never): a burst frame's slot counts when it is that frame
		unsigned runs = 0;	// RenderBuffers calls that frame (normally 1; the last one's values and copies are the ones kept)
		bool noted = false;
		Diagnostics::VLValues values;
		char path[5] = { '-', '-', '-', '-', '-' };	// techniques 0 composite, 1 march, 2 blur X, 3 temporal, 4 blur Y: V Vulkan, D D3D9
		bool captured[Diagnostics::VLStageCount] = {};
		UINT width[Diagnostics::VLStageCount] = {}, height[Diagnostics::VLStageCount] = {};	// the source's size
		UINT areaW[Diagnostics::VLStageCount] = {}, areaH[Diagnostics::VLStageCount] = {};	// the part shrunk (a RenderScale frame's drawn part)
	};

	struct VLBurst {
		enum State { Off, Capturing, Reading } state = Off;
		unsigned n = 0, count = 0;
		ShrinkChain chains[2];		// 0: the volumetric light's buffers, 1: the frame
		bool chainFailed[2] = {};
		IDirect3DSurface9* row[Diagnostics::VLStageCount] = {};	// (8 count) x 8 in the stage's format: block k = burst frame k
		IDirect3DSurface9* sys[Diagnostics::VLStageCount] = {};
		D3DFORMAT format[Diagnostics::VLStageCount] = {};
		bool off[Diagnostics::VLStageCount] = {};		// left out for the rest of the burst (logged once)
		bool issued[Diagnostics::VLStageCount] = {}, done[Diagnostics::VLStageCount] = {};
		std::vector<float> texels[Diagnostics::VLStageCount];	// read back: the row as rgba floats
		unsigned readFrame = 0;
		VLSlot slots[MaxBurst];
	} V;

	void VLRelease() {
		for (ShrinkChain& c : V.chains) c.Release();
		for (int s = 0; s < Diagnostics::VLStageCount; s++) {
			if (V.row[s]) { V.row[s]->Release(); V.row[s] = nullptr; }
			if (V.sys[s]) { V.sys[s]->Release(); V.sys[s] = nullptr; }
			std::vector<float>().swap(V.texels[s]);
		}
	}

	// This frame's slot while a burst captures (a slot written by an earlier frame that did not reach the final picture starts again),
	// else null. The burst frame that is coming is B.next: the volumetric light draws before the final picture's BurstFrame.
	VLSlot* VLSlotNow() {
		if (!Diagnostics::VLCapturing || V.state != VLBurst::Capturing || B.phase != Burst::Capturing || B.next >= B.count || B.next >= V.count) return nullptr;
		VLSlot& slot = V.slots[B.next];
		const unsigned frame = FrameNow();
		if (slot.frame != frame) { slot = VLSlot(); slot.frame = frame; }
		return &slot;
	}

	void VLStageOff(int stage, const char* why) {
		if (V.off[stage]) return;
		V.off[stage] = true;
		Logger::Log("REPORT %u VL: the %s stage is left out of the rest of this burst (%s)", V.n, VLNames[stage], why);
	}

	// One stage: the source's drawn part shrunk to 8 x 8, into this frame's block of the stage's row
	void VLShrink(IDirect3DDevice9* device, int stage, IDirect3DSurface9* surface) {
		VLSlot* slot = VLSlotNow();
		if (!slot || !device || V.off[stage]) return;
		D3DSURFACE_DESC d = {};
		if (!surface || FAILED(surface->GetDesc(&d))) { VLStageOff(stage, "no surface"); return; }
		if (!SelfTestCore::KnownFormat(d.Format)) { char why[64]; sprintf_s(why, "its format %d is not handled", (int)d.Format); VLStageOff(stage, why); return; }
		UINT w = d.Width, h = d.Height;
		if (RenderRect::Active() && RenderRect::ScreenShaped(d.Width, d.Height)) { w = RenderRect::Width(d.Width); h = RenderRect::Height(d.Height); }
		const int c = stage >= Diagnostics::VLBefore ? 1 : 0;
		ShrinkChain& chain = V.chains[c];
		if (!chain.Fits(d, w, h)) {
			if (V.chainFailed[c] || !chain.Make(device, d, w, h)) {
				chain.Release();
				V.chainFailed[c] = true;
				VLStageOff(stage, "its shrink targets could not be made");
				return;
			}
		}
		if (!V.row[stage]) {
			if (FAILED(device->CreateRenderTarget(8 * V.count, 8, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &V.row[stage], NULL))) {
				V.row[stage] = nullptr;
				VLStageOff(stage, "its row target could not be made");
				return;
			}
			V.format[stage] = d.Format;
		}
		else if (V.format[stage] != d.Format) { VLStageOff(stage, "its format changed during the burst"); return; }
		HRESULT hr = chain.Run(device, surface);
		const RECT block = { (LONG)(8 * B.next), 0, (LONG)(8 * B.next + 8), 8 };
		if (SUCCEEDED(hr)) hr = device->StretchRect(chain.Last(), NULL, V.row[stage], &block, D3DTEXF_NONE);
		if (FAILED(hr)) { EffectStatus::D3DFail("report burst VL stage StretchRect", hr); VLStageOff(stage, "a StretchRect failed"); return; }
		slot->captured[stage] = true;
		slot->width[stage] = d.Width; slot->height[stage] = d.Height;
		slot->areaW[stage] = w; slot->areaH[stage] = h;
	}

	struct VLHalves { double up[4] = {}, low[4] = {}; };

	// Burst frame k's slot holds stage s, read back
	bool VLHas(unsigned k, int s) {
		return k < V.count && B.frames[k] && V.slots[k].frame == B.frames[k] && V.slots[k].captured[s] && V.texels[s].size() >= (size_t)V.count * 256;
	}

	// Block k of a stage's row: the means of its upper (rows 0-3) and lower (rows 4-7) half, r/g/b/a (NaN counted as 0)
	VLHalves VLBlock(unsigned k, int s) {
		VLHalves out;
		const size_t width = (size_t)8 * V.count;
		const std::vector<float>& t = V.texels[s];
		for (size_t y = 0; y < 8; y++)
			for (size_t x = 0; x < 8; x++) {
				const float* c = &t[(y * width + (size_t)k * 8 + x) * 4];
				double* half = y < 4 ? out.up : out.low;
				for (int ch = 0; ch < 4; ch++) half[ch] += c[ch] == c[ch] ? c[ch] / 32.0 : 0.0;
			}
		return out;
	}

	double VLLuma(const double* c) { return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]; }

	// Why burst frame k has no stage s
	const char* VLMissing(unsigned k, int s) {
		const VLSlot& slot = V.slots[k];
		if (slot.values.technique == 8) return s < Diagnostics::VLBefore ? "the froxel grid path: no march" : "the froxel grid's composite: not captured";
		if (!slot.values.temporal && (s == Diagnostics::VLHistory || s == Diagnostics::VLTemporal)) return "Temporal off";
		if (slot.captured[s]) return "not read back";
		if (V.off[s]) return "left out, see above";
		return "not captured";
	}

	// The "REPORT n VL pattern" line: each stage's upper-half value over the burst and where a beat starts
	void VLPattern() {
		using namespace Diagnostics;
		struct RowSpec { const char* name; int stage; int what; };	// what: 0 g - r, 1 r, 2 luma, 3 the composite's change in luma
		const RowSpec specs[] = {
			{ "march g-r", VLMarch, 0 }, { "march r", VLMarch, 1 }, { "temporal g-r", VLTemporal, 0 }, { "temporal r", VLTemporal, 1 },
			{ "blur g-r", VLBlur, 0 }, { "blur r", VLBlur, 1 }, { "VL change luma", VLAfter, 3 },
			{ "history g-r", VLHistory, 0 }, { "frame before VL luma", VLBefore, 2 }, { "after VL luma", VLAfter, 2 } };
		constexpr int Rows = sizeof(specs) / sizeof(specs[0]);
		const int groupOf[Rows] = { 0, 0, 1, 1, 2, 2, 3, -1, -1, -1 };	// the order march -> temporal -> blur -> composite
		const char* const groupNames[4] = { "march", "temporal", "blur", "composite (VL change)" };
		BeatRule::Result results[Rows];
		std::string rows;
		char text[1024];	// (sprintf_s ends the process on an overflow: room for any number, inf or 1e38 included)
		for (int r = 0; r < Rows; r++) {
			const RowSpec& spec = specs[r];
			float values[MaxBurst] = {};
			bool valid[MaxBurst] = {};
			for (unsigned k = 0; k < V.count; k++) {
				if (!VLHas(k, spec.stage) || (spec.what == 3 && !VLHas(k, VLBefore))) continue;
				const VLHalves h = VLBlock(k, spec.stage);
				double v = spec.what == 0 ? h.up[1] - h.up[0] : spec.what == 1 ? h.up[0] : VLLuma(h.up);
				if (spec.what == 3) v -= VLLuma(VLBlock(k, VLBefore).up);
				values[k] = (float)v;
				valid[k] = true;
			}
			const BeatRule::Result& b = results[r] = BeatRule::Find(values, valid, B.frames, (int)V.count);
			rows += rows.empty() ? "" : " | ";
			rows += spec.name;
			rows += ":";
			for (unsigned k = 0; k < V.count; k++) {
				if (!valid[k]) { rows += " -"; continue; }
				sprintf_s(text, spec.what == 3 ? " %+.5f" : spec.what == 2 ? " %.4f" : " %.5f", values[k]);
				rows += text;
			}
			if (b.beat) sprintf_s(text, " (BEAT: period %d, frames = %d mod %d at %.5f vs %.5f the others, %.1f x its noise %.5f)", b.period, b.phase, b.period, b.odd, b.rest, b.score, b.noise);
			else if (b.period) sprintf_s(text, " (no beat: best period %d at %.1f x its noise, %.0f needed)", b.period, b.score, BeatRule::MinScore);
			else sprintf_s(text, " (too few consecutive values to judge)");
			rows += text;
		}
		// The first stage in the order march -> temporal -> blur -> composite with a beat
		std::string verdict = "no stage beats more than its noise (march -> temporal -> blur -> composite)";
		for (int g = 0; g < 4; g++) {
			int best = -1;
			for (int r = 0; r < Rows; r++) if (groupOf[r] == g && results[r].beat && (best < 0 || results[r].score > results[best].score)) best = r;
			if (best < 0) continue;
			const BeatRule::Result& b = results[best];
			sprintf_s(text, "FIRST stage with a beat: %s (%s, period %d: frames = %d mod %d at %.5f vs %.5f the others, %.1f x its noise)", groupNames[g],
				specs[best].name, b.period, b.phase, b.period, b.odd, b.rest, b.score);
			verdict = text;
			break;
		}
		if (results[8].beat) verdict += "; the frame BEFORE the volumetric light beats too (outside the effect)";
		// History = last frame's temporal output? (History is a plain copy of Temp: shrunk the same way, the two are bit-identical)
		unsigned compared = 0, exact = 0, largestFrame = 0;
		double largest = 0.0;
		for (unsigned k = 1; k < V.count; k++) {
			if (!VLHas(k, VLHistory) || !VLHas(k - 1, VLTemporal) || B.frames[k - 1] + 1 != B.frames[k]) continue;
			const size_t width = (size_t)8 * V.count;
			double diff = 0.0;
			for (size_t y = 0; y < 8; y++)
				for (size_t x = 0; x < 8 * 4; x++) {
					const float a = V.texels[VLHistory][(y * width + (size_t)k * 8) * 4 + x], b = V.texels[VLTemporal][(y * width + (size_t)(k - 1) * 8) * 4 + x];
					const double e = a == b ? 0.0 : fabs((double)a - b);
					if (e > diff || e != e) diff = e != e ? 1e30 : e;
				}
			compared++;
			if (diff == 0.0) exact++;
			if (diff > largest) { largest = diff; largestFrame = B.frames[k]; }
		}
		if (!compared) sprintf_s(text, "history vs last frame's temporal output: nothing to compare");
		else if (exact == compared) sprintf_s(text, "history == last frame's temporal output: exact in all %u frames compared", compared);
		else sprintf_s(text, "history vs last frame's temporal output: DIFFERS in %u of %u frames (largest %.5f, at f%u)", compared - exact, compared, largest, largestFrame);
		Logger::Log("REPORT %u VL pattern: %s | %s | upper-half means per burst frame f%u..f%u ('-' none; g-r = the shaded air the composite darkens by, "
			"r = the lit air it adds light for; each value judged against the mean of its two neighbours, by period 2-4): %s", V.n, verdict.c_str(), text,
			B.frames[0], B.frames[V.count ? V.count - 1 : 0], rows.c_str());
	}

	// The lines, after the read-back
	void VLWrite() {
		using namespace Diagnostics;
		const unsigned n = V.n, count = V.count;
		const VolumetricLightEffect* vl = TheShaderManager->Effects.VolumetricLight;
		double bytes = V.chains[0].bytes + V.chains[1].bytes;
		int stages = 0;
		for (int s = 0; s < VLStageCount; s++) if (V.row[s]) { stages++; bytes += 64.0 * count * BytesPerPixel(V.format[s]); }
		char settings[160] = "";
		if (vl) sprintf_s(settings, " | settings: Steps %d, Temporal %.2f, BlurRadius %.1f, Froxels %s", vl->Settings.Steps, vl->Settings.Temporal, vl->Settings.BlurRadius,
			vl->Settings.Froxels ? "true" : "false");
		Logger::Log("REPORT %u VL buffers: r = the lit air's share of the view (the sunlight it scatters towards the eye, before Strength x phase), g = the same share "
			"if all the air were lit, b = depth / farZ, a = 1 (unused); the composite: frame x (1 - Darken x Fade x (g - r)) + sun colour x Strength x Fade x phase x r "
			"(in linear light), so the darkening of the distant air is g - r, not alpha | each stage: the means of the upper | lower half of its drawn part shrunk to 8x8 "
			"on the GPU (2:1 bilinear steps in its own format: half floats, not clamped), r/g/b/a | march = technique 1's output, history = last frame's temporal "
			"result as this frame's temporal pass read it, temporal = technique 3's output, blur = technique 4's (what the composite reads), frame before / after VL = "
			"the picture the composite reads / wrote (before tonemapping) | frame 1/%u has no stage data (the key is read at the end of it, after its passes) | "
			"%d stages captured, %.1f MB of GPU targets during the burst%s", n, count, stages, bytes / 1048576.0, settings);
		char text[1024];	// (sprintf_s ends the process on an overflow: room for any number, inf or 1e38 included)
		for (unsigned k = 0; k < count; k++) {
			const unsigned frame = B.frames[k];
			const VLSlot& slot = V.slots[k];
			if (k == 0) { Logger::Log("REPORT %u VL %u/%u f%u: no stage data (the report key is read at the end of this frame, after its volumetric light passes)", n, k + 1, count, frame); continue; }
			if (!frame || slot.frame != frame || !slot.noted) {
				Logger::Log("REPORT %u VL %u/%u f%u: the volumetric light's passes did not run in this frame (its state is on the 'REPORT %u frame %u/%u' line)", n, k + 1, count, frame, n, k + 1, count);
				continue;
			}
			std::string line;
			for (int s = VLMarch; s <= VLBlur; s++) {
				if (s != VLMarch) line += " | ";
				if (!VLHas(k, s)) { sprintf_s(text, "%s - (%s)", VLNames[s], VLMissing(k, s)); line += text; continue; }
				const VLHalves h = VLBlock(k, s);
				sprintf_s(text, "%s up %.5f/%.5f/%.4f/%.3f (g-r %.5f) low %.5f/%.5f/%.4f/%.3f (g-r %.5f)", VLNames[s], h.up[0], h.up[1], h.up[2], h.up[3], h.up[1] - h.up[0],
					h.low[0], h.low[1], h.low[2], h.low[3], h.low[1] - h.low[0]);
				line += text;
			}
			for (int s = VLBefore; s <= VLAfter; s++) {
				line += " | ";
				if (!VLHas(k, s)) { sprintf_s(text, "%s - (%s)", VLNames[s], VLMissing(k, s)); line += text; continue; }
				const VLHalves h = VLBlock(k, s);
				sprintf_s(text, "%s up %.4f/%.4f/%.4f/%.3f (luma %.4f) low %.4f/%.4f/%.4f/%.3f (luma %.4f)", VLNames[s], h.up[0], h.up[1], h.up[2], h.up[3], VLLuma(h.up),
					h.low[0], h.low[1], h.low[2], h.low[3], VLLuma(h.low));
				line += text;
			}
			if (VLHas(k, VLBefore) && VLHas(k, VLAfter)) {
				const VLHalves a = VLBlock(k, VLBefore), b = VLBlock(k, VLAfter);
				double up[4], low[4];
				for (int ch = 0; ch < 4; ch++) { up[ch] = b.up[ch] - a.up[ch]; low[ch] = b.low[ch] - a.low[ch]; }
				sprintf_s(text, " | VL change (after-before) up %+.5f/%+.5f/%+.5f/%+.4f (luma %+.5f) low %+.5f/%+.5f/%+.5f/%+.4f (luma %+.5f)", up[0], up[1], up[2], up[3],
					VLLuma(up), low[0], low[1], low[2], low[3], VLLuma(low));
			}
			else sprintf_s(text, " | VL change -");
			line += text;
			// The sizes: the buffers' and the frame's (and the drawn part in a RenderScale frame)
			int buffer = -1, picture = -1;
			for (int s = VLMarch; s <= VLBlur && buffer < 0; s++) if (slot.captured[s]) buffer = s;
			for (int s = VLBefore; s <= VLAfter && picture < 0; s++) if (slot.captured[s]) picture = s;
			std::string sizes;
			for (const int s : { buffer, picture }) {
				if (s < 0) continue;
				sprintf_s(text, "%s%s %ux%u", sizes.empty() ? "" : ", ", s >= VLBefore ? "frame" : "march", slot.width[s], slot.height[s]);
				sizes += text;
				if (slot.areaW[s] != slot.width[s] || slot.areaH[s] != slot.height[s]) { sprintf_s(text, " (drawn %ux%u)", slot.areaW[s], slot.areaH[s]); sizes += text; }
				sizes += " ";
				sizes += SelfTestCore::FormatName(V.format[s]);
			}
			if (sizes.empty()) sizes = "sizes -";
			const VLValues& v = slot.values;
			char runs[96] = "";
			if (slot.runs > 1) sprintf_s(runs, ", RenderBuffers ran %u times this frame (the last one's values and copies)", slot.runs);
			sprintf_s(text, " | noise idx %.0f, temporal weight %.3f, usable %d (have %d, drewLast %d, shared history valid %d%s), rect %d scaleChanged %d, cam move %.1f, "
				"%s, passes (V Vulkan, D D3D9) march %c temporal %c blurX %c blurY %c composite %c, fade %.3f (strength %.3f, darken %.3f), CompositeTechnique %u%s",
				v.noise, v.weight, (int)v.usable, (int)v.haveHistory, (int)v.drewLastFrame, (int)v.historyValid, v.sharedNoise ? "" : ", SharedNoise off",
				(int)v.rect, (int)v.scaleChanged, v.cameraMove, sizes.c_str(), slot.path[1], slot.path[3], slot.path[2], slot.path[4], slot.path[0],
				v.fade, v.strength, v.darken, v.technique, runs);
			line += text;
			Logger::Log("REPORT %u VL %u/%u f%u: %s", n, k + 1, count, frame, line.c_str());
		}
		VLPattern();
	}

	// After the burst: the rows locked without waiting (waited for after 4 frames, or when forced), then the lines, then everything released
	void VLRead(unsigned frame, bool force) {
		if (V.state != VLBurst::Reading) return;
		const bool wait = force || frame - V.readFrame >= 4;
		bool pending = false;
		for (int s = 0; s < Diagnostics::VLStageCount; s++) {
			if (!V.issued[s] || V.done[s]) continue;
			D3DLOCKED_RECT lock = {};
			const HRESULT hr = V.sys[s]->LockRect(&lock, NULL, D3DLOCK_READONLY | (wait ? 0 : D3DLOCK_DONOTWAIT));
			if (hr == D3DERR_WASSTILLDRAWING) { pending = true; continue; }
			V.done[s] = true;
			if (FAILED(hr)) { EffectStatus::D3DFail("report burst VL LockRect", hr); continue; }
			const unsigned width = 8 * V.count;
			V.texels[s].resize((size_t)width * 8 * 4);
			for (unsigned y = 0; y < 8; y++)
				SelfTestCore::ConvertRow(V.format[s], (const unsigned char*)lock.pBits + (size_t)y * lock.Pitch, width, &V.texels[s][(size_t)y * width * 4]);
			V.sys[s]->UnlockRect();
		}
		if (pending) return;
		VLWrite();
		VLRelease();
		V.state = VLBurst::Off;
	}

	// At the press (StartBurst): the new burst's state (its targets are made at its first capture)
	void VLBegin(unsigned n, unsigned count) {
		if (V.state == VLBurst::Reading) VLRead(FrameNow(), true);	// the last burst's lines (waits for its read-back)
		VLRelease();
		V = VLBurst();
		V.n = n;
		V.count = (std::min)(count, MaxBurst);
		V.state = VLBurst::Capturing;
	}

	// The burst's last frame (its copies are all issued): the rows' read-back, locked in the frames after (VLRead)
	void VLEndCapture(IDirect3DDevice9* device, unsigned frame) {
		if (V.state != VLBurst::Capturing) return;
		V.state = VLBurst::Reading;
		V.readFrame = frame;
		for (int s = 0; s < Diagnostics::VLStageCount; s++) {
			if (!V.row[s]) continue;
			HRESULT hr = device->CreateOffscreenPlainSurface(8 * V.count, 8, V.format[s], D3DPOOL_SYSTEMMEM, &V.sys[s], NULL);
			if (SUCCEEDED(hr)) hr = device->GetRenderTargetData(V.row[s], V.sys[s]);
			if (FAILED(hr)) {
				EffectStatus::D3DFail("report burst VL read-back", hr);
				Logger::Log("REPORT %u VL: the %s stage's read-back failed (%08lX)", V.n, VLNames[s], (unsigned long)hr);
				if (V.sys[s]) { V.sys[s]->Release(); V.sys[s] = nullptr; }
				continue;
			}
			V.issued[s] = true;
		}
	}

	// Makes the ring (or decides on the fallback) at the press
	void StartBurst(IDirect3DDevice9* device, IDirect3DSurface9* picture, unsigned n, unsigned count) {
		VLBegin(n, count);	// the volumetric light's stages (VL stages): captured from the next frame on
		B.n = n; B.count = count; B.next = 0; B.drainNext = 0; B.drained = 0; B.captureFails = 0; B.fallback = false; B.lumaRead = false; B.lumaTried = false;
		B.closed = false; B.pendingBytes = 0; B.written = 0; B.startTime = Seconds();
		CreateDirectoryA(".\\" VS_LOGS_FOLDER, NULL);
		D3DSURFACE_DESC d = {};
		const bool known = picture && SUCCEEDED(picture->GetDesc(&d)) && SelfTestCore::KnownFormat(d.Format);
		B.format = d.Format; B.width = d.Width; B.height = d.Height;
		const char* why = nullptr;
		if (known) {
			const double full = (double)d.Width * d.Height * BytesPerPixel(d.Format);
			B.scale = 1;
			while (B.scale < 4 && full / (B.scale * B.scale) * count > RingBudget) B.scale *= 2;
			B.ringW = (std::max)(1u, d.Width / B.scale); B.ringH = (std::max)(1u, d.Height / B.scale);
			bool ok = true;
			if (d.MultiSampleType != D3DMULTISAMPLE_NONE)
				ok = SUCCEEDED(device->CreateRenderTarget(d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &B.resolve, NULL));
			for (unsigned k = 0; ok && k < count; k++)
				ok = SUCCEEDED(device->CreateRenderTarget(B.ringW, B.ringH, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &B.ring[k], NULL));
			if (!ok) {
				for (IDirect3DSurface9*& s : B.ring) if (s) { s->Release(); s = nullptr; }
				B.fallback = true;
				// the fallback: read back at full size, converted to 8-bit at a size whose N pictures fit FallbackBudget
				B.scale = 1;
				while (B.scale < 8 && (double)d.Width * d.Height * 4.0 / (B.scale * B.scale) * count > FallbackBudget) B.scale *= 2;
				if (d.MultiSampleType != D3DMULTISAMPLE_NONE && !B.resolve) why = "the GPU ring and a resolve target could not be made";
				else if (FAILED(device->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &B.sys[0], NULL))) why = "no GPU ring and no read-back surface";
			}
		}
		else why = picture ? "picture format not handled" : "no picture";
		// The AvgLuma row
		AvgLumaEffect* avg = TheShaderManager->Effects.AvgLuma;
		D3DSURFACE_DESC ld = {};
		if (avg && avg->Textures.AvgLumaSurface && SUCCEEDED(avg->Textures.AvgLumaSurface->GetDesc(&ld))) {
			B.lumaFormat = ld.Format;
			if (FAILED(device->CreateRenderTarget(count, 1, ld.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &B.lumaRow, NULL)) ||
				FAILED(device->CreateOffscreenPlainSurface(count, 1, ld.Format, D3DPOOL_SYSTEMMEM, &B.lumaSys, NULL))) {
				if (B.lumaRow) { B.lumaRow->Release(); B.lumaRow = nullptr; }
				if (B.lumaSys) { B.lumaSys->Release(); B.lumaSys = nullptr; }
			}
		}
		for (unsigned k = 0; k < count; k++) B.frames[k] = 0;
		B.phase = Burst::Capturing;
		if (!B.wake) B.wake = CreateEventA(NULL, FALSE, FALSE, NULL);
		B.worker = CreateThread(NULL, 0, BurstWorker, NULL, 0, NULL);
		const double ringMB = B.fallback || why ? 0.0 : (double)B.ringW * B.ringH * BytesPerPixel(d.Format) * count / 1048576.0;
		if (why) Logger::Log("REPORT %u burst: NO pictures (%s); the per-frame lines are still written", n, why);
		else if (B.fallback)
			Logger::Log("REPORT %u burst: the GPU ring (%u x %ux%u) could not be made: each burst frame is read back at once instead (that frame WAITS for the "
				"graphics card, so the burst's frame times are longer than normal), pictures %ux%u", n, count, B.ringW, B.ringH,
				(std::max)(1u, d.Width / B.scale), (std::max)(1u, d.Height / B.scale));
		else Logger::Log("REPORT %u burst: GPU ring of %u pictures %ux%u (%s size, %.0f MB, format %d)%s", n, count, B.ringW, B.ringH,
			B.scale == 1 ? "full" : B.scale == 2 ? "half" : "quarter", ringMB, (int)d.Format, d.MultiSampleType != D3DMULTISAMPLE_NONE ? ", resolved first" : "");
		if (!B.worker) Logger::Log("REPORT %u burst: the picture writer thread could not start: no pictures", n);
		if (!why && !B.worker) why = "no writer thread";
		if (why) { ReleaseRing(); ReleaseReadback(); B.fallback = true; B.format = D3DFMT_UNKNOWN; }
	}

	// The fallback: this frame's picture read back now (waits), converted and handed to the writer
	void ReadNow(IDirect3DDevice9* device, IDirect3DSurface9* picture, unsigned k, PictureJob* job) {
		IDirect3DSurface9* source = picture;
		HRESULT hr = S_OK;
		if (B.resolve) { hr = device->StretchRect(picture, NULL, B.resolve, NULL, D3DTEXF_NONE); source = B.resolve; }
		D3DLOCKED_RECT lock = {};
		if (SUCCEEDED(hr)) hr = device->GetRenderTargetData(source, B.sys[0]);
		if (SUCCEEDED(hr)) hr = B.sys[0]->LockRect(&lock, NULL, D3DLOCK_READONLY);
		if (FAILED(hr)) { EffectStatus::D3DFail("report burst read-back (fallback)", hr); job->failed = "read-back failed"; return; }
		ConvertPicture(B.format, (const unsigned char*)lock.pBits, lock.Pitch, B.width, B.height, B.scale, job->bgra, job->w, job->h, job->meanLuma);
		B.sys[0]->UnlockRect();
	}

	// One burst frame: the copies and its line
	void BurstFrame(IDirect3DDevice9* device, IDirect3DSurface9* picture, unsigned frame) {
		const unsigned k = B.next++;
		B.frames[k] = frame;
		// The picture
		const char* note = "";
		if (B.format != D3DFMT_UNKNOWN && !B.fallback && B.ring[k]) {
			IDirect3DSurface9* source = picture;
			HRESULT hr = S_OK;
			if (B.resolve) { hr = device->StretchRect(picture, NULL, B.resolve, NULL, D3DTEXF_NONE); source = B.resolve; }
			if (SUCCEEDED(hr)) {
				hr = device->StretchRect(source, NULL, B.ring[k], NULL, B.scale > 1 ? D3DTEXF_LINEAR : D3DTEXF_NONE);
				if (FAILED(hr) && B.scale > 1) hr = device->StretchRect(source, NULL, B.ring[k], NULL, D3DTEXF_POINT);
			}
			if (FAILED(hr)) { EffectStatus::D3DFail("report burst StretchRect", hr); B.captureFails++; note = " (copy FAILED)"; B.ring[k]->Release(); B.ring[k] = nullptr; }
		}
		// The AvgLuma result
		AvgLumaEffect* avg = TheShaderManager->Effects.AvgLuma;
		const StatusSlot* avgSlot = avg ? Find(avg->StatusSlot) : nullptr;
		B.avgDrawn[k] = avgSlot && Has(*avgSlot, frame) && avgSlot->result[frame % EffectStatus::Ring] == EffectStatus::Drawn;
		ExposureEffect* exposure = TheShaderManager->Effects.Exposure;
		B.exposureOn[k] = exposure && exposure->Enabled;
		if (B.lumaRow && avg && avg->Textures.AvgLumaSurface) {
			const RECT texel = { (LONG)k, 0, (LONG)k + 1, 1 };
			const HRESULT hr = device->StretchRect(avg->Textures.AvgLumaSurface, NULL, B.lumaRow, &texel, D3DTEXF_POINT);
			if (FAILED(hr)) EffectStatus::D3DFail("report burst AvgLuma StretchRect", hr);
		}
		sprintf_s(B.files[k], "NVR-Report-%s-%u-f%u-%02uof%02u.png", B.date, B.n, frame, k + 1, B.count);

		// The line: frame time, exposure, what changed since the frame before, VolumetricLight, the journal's counts
		const double now = TheFrameRateManager->Time;
		const double ms = B.lastFrameTime >= 0.0 ? (now - B.lastFrameTime) * 1000.0 : 0.0;
		std::vector<std::string> changed;
		size_t changedLength = 0;
		unsigned more = 0;
		for (int s = 0; s < SlotCount; s++) {
			const StatusSlot& slot = Slots[s];
			if (!Has(slot, frame) && !Has(slot, frame - 1)) continue;
			const std::string a = StateText(slot, frame - 1), b = StateText(slot, frame);
			if (a == b) continue;
			std::string item = std::string(slot.name) + " " + a + " -> " + b;
			if (changedLength + item.size() > 700) { more++; continue; }
			changedLength += item.size() + 3;
			changed.push_back(item);
		}
		std::string changedText;
		for (const std::string& c : changed) { if (!changedText.empty()) changedText += " | "; changedText += c; }
		if (changedText.empty()) changedText = "none";
		if (more) { char t[32]; sprintf_s(t, " | +%u more", more); changedText += t; }
		VolumetricLightEffect* vl = TheShaderManager->Effects.VolumetricLight;
		const StatusSlot* vlSlot = vl ? Find(vl->StatusSlot) : nullptr;
		const std::string vlText = !vl ? "none" : !vl->Enabled ? "off" : vlSlot ? StateText(*vlSlot, frame) : "not registered";
		const unsigned lamps = Since(SessionLog::Count.lampSlotChanges, B.lastLampSlots);
		const unsigned cascades = Since(SessionLog::Count.cachedCascadeRedraws, B.lastCascades);
		const unsigned resets = Since(SessionLog::Count.historyResets, B.lastResets);
		char exposureText[128];
		if (B.exposureOn[k]) sprintf_s(exposureText, "exposure on (Min %.2f Max %.2f, AvgLuma %s; values on the picture line)", exposure->Constants.Data.x,
			exposure->Constants.Data.y, B.avgDrawn[k] ? "drawn" : "not drawn");
		else sprintf_s(exposureText, "exposure off");
		char gap[64] = "";
		if (k > 0 && frame != B.frames[k - 1] + 1) sprintf_s(gap, " | NOTE f%u-f%u did not reach the final picture", B.frames[k - 1] + 1, frame - 1);
		Logger::Log("REPORT %u frame %u/%u f%u: picture %s%s | %.2f ms | %s | luma on the picture line | changed vs f-1: %s | VolumetricLight %s | journal %u events "
			"(lamp slot changes %u, cached sun cascade redraws %u, sun cascade lines %u, history resets %u)%s",
			B.n, k + 1, B.count, frame, B.format == D3DFMT_UNKNOWN ? "none" : B.files[k], note, ms, exposureText, changedText.c_str(), vlText.c_str(),
			FrameJournal::CountFrame(frame), lamps, cascades, FrameJournal::CountFrame(frame, "sun cascades"), resets, gap);
		// UNOFFICIAL 2026-11-01 (the 4K smear report): the temporal resolve's state in this very frame
		if (TAAEffect* taa = TheShaderManager->Effects.TAA) Logger::Log("REPORT %u temporal %u/%u f%u: %s", B.n, k + 1, B.count, frame, taa->UpscaleDiagText().c_str());

		// The fallback reads it now (needs the AvgLuma row read first: the row is read at the end, so the exposure text is added then)
		if (B.fallback && B.format != D3DFMT_UNKNOWN && B.sys[0]) {
			PictureJob* job = NewJob(k);
			ReadNow(device, picture, k, job);
			B.held.push_back(job);	// its exposure text is filled at the burst's end (below), then it goes to the writer
		}
		if (B.next == B.count) {	// the burst's last frame
			if (B.fallback) {
				ReadLumaRow(device);	// (the fallback waits anyway; the GPU ring's burst reads it in the first read-back frame, after the burst)
				for (PictureJob* job : B.held) { ExposureText(job->k - 1, job->exposure, sizeof(job->exposure)); Push(job); }
				B.held.clear();
				B.closed = true;
				SetEvent(B.wake);
				ReleaseRing(); ReleaseReadback();
				B.phase = Burst::Writing;
			}
			else B.phase = Burst::Draining;
			Logger::Log("REPORT %u burst: %u frames captured (f%u-f%u, %.0f ms)%s; the pictures are read back and written now", B.n, B.count, B.frames[0], frame,
				(Seconds() - B.startTime) * 1000.0, B.captureFails ? ", some copies FAILED" : "");
			VLEndCapture(device, frame);	// the volumetric light's stages: read back in the next frames ("REPORT n VL" lines)
		}
		// The volumetric light's hooks capture in the frames that follow while the burst lasts (its passes draw before this call)
		Diagnostics::VLCapturing = B.phase == Burst::Capturing && V.state == VLBurst::Capturing;
	}

	// After the burst: one picture a frame read back (locked a frame later without waiting), to the writer
	void Drain(IDirect3DDevice9* device, unsigned frame) {
		if (!B.lumaTried) { B.lumaTried = true; ReadLumaRow(device); }	// (one wait for N texels, before the pictures' exposure text)
		if (!B.sys[0] && FAILED(device->CreateOffscreenPlainSurface(B.ringW, B.ringH, B.format, D3DPOOL_SYSTEMMEM, &B.sys[0], NULL))) B.sys[0] = nullptr;
		if (!B.sys[1] && B.sys[0] && FAILED(device->CreateOffscreenPlainSurface(B.ringW, B.ringH, B.format, D3DPOOL_SYSTEMMEM, &B.sys[1], NULL))) B.sys[1] = nullptr;
		for (int j = 0; j < 2; j++) {	// the copies issued earlier
			if (B.issued[j] < 0 || !B.sys[j]) continue;
			const unsigned k = (unsigned)B.issued[j];
			const bool wait = frame - B.issuedFrame[j] >= 4;
			D3DLOCKED_RECT lock = {};
			const HRESULT hr = B.sys[j]->LockRect(&lock, NULL, D3DLOCK_READONLY | (wait ? 0 : D3DLOCK_DONOTWAIT));
			if (hr == D3DERR_WASSTILLDRAWING) continue;
			PictureJob* job = NewJob(k);
			if (FAILED(hr)) { EffectStatus::D3DFail("report burst LockRect", hr); job->failed = "read-back lock failed"; }
			else {
				const unsigned row = B.ringW * BytesPerPixel(B.format);
				job->raw.resize((size_t)row * B.ringH);
				for (unsigned y = 0; y < B.ringH; y++) memcpy(&job->raw[(size_t)y * row], (const unsigned char*)lock.pBits + (size_t)y * lock.Pitch, row);
				B.sys[j]->UnlockRect();
				job->format = B.format; job->w = B.ringW; job->h = B.ringH; job->pitch = row;
				InterlockedExchangeAdd(&B.pendingBytes, (LONG)job->raw.size());
			}
			Push(job);
			B.issued[j] = -1;
			B.drained++;
		}
		// The next copy, when a read-back surface is free and the writer is not too far behind
		while (B.drainNext < B.count && !B.ring[B.drainNext]) {	// (a failed copy: its line says so)
			PictureJob* job = NewJob(B.drainNext);
			job->failed = "its GPU copy failed";
			Push(job);
			B.drainNext++; B.drained++;
		}
		if (B.drainNext < B.count && (double)B.pendingBytes < PendingBudget) {
			for (int j = 0; j < 2; j++) {
				if (B.issued[j] >= 0 || !B.sys[j]) continue;
				const HRESULT hr = device->GetRenderTargetData(B.ring[B.drainNext], B.sys[j]);
				if (FAILED(hr)) {
					EffectStatus::D3DFail("report burst GetRenderTargetData", hr);
					PictureJob* job = NewJob(B.drainNext);
					job->failed = "read-back failed";
					Push(job);
					B.drained++;
				}
				else { B.issued[j] = (int)B.drainNext; B.issuedFrame[j] = frame; }
				B.drainNext++;
				break;
			}
		}
		if (!B.sys[0] && B.drainNext < B.count) {	// no read-back surface at all
			for (; B.drainNext < B.count; B.drainNext++, B.drained++) { PictureJob* job = NewJob(B.drainNext); job->failed = "no read-back surface"; Push(job); }
		}
		if (B.drained >= B.count) {
			ReleaseRing(); ReleaseReadback();
			B.closed = true;
			SetEvent(B.wake);
			B.phase = Burst::Writing;
		}
	}

	// Every frame: the burst's state
	void BurstStep(IDirect3DDevice9* device, IDirect3DSurface9* picture, unsigned frame) {
		if (V.state == VLBurst::Reading) VLRead(frame, false);	// the volumetric light's stages after the burst (no wait for 4 frames)
		if (B.phase == Burst::Capturing) BurstFrame(device, picture, frame);
		else if (B.phase == Burst::Draining) Drain(device, frame);
		else if (B.phase == Burst::Writing && B.worker && WaitForSingleObject(B.worker, 0) == WAIT_OBJECT_0) {
			CloseHandle(B.worker); B.worker = NULL;
			B.phase = Burst::Idle;
		}
		else if (B.phase == Burst::Writing && !B.worker) B.phase = Burst::Idle;
		// The counters' frame before (the per-frame counts are differences from it)
		B.lastLampSlots = SessionLog::Count.lampSlotChanges;
		B.lastCascades = SessionLog::Count.cachedCascadeRedraws;
		B.lastResets = SessionLog::Count.historyResets;
		B.lastFrameTime = TheFrameRateManager->Time;
	}

	std::string CellDetail() {
		TESObjectCELL* cell = Player ? Player->parentCell : nullptr;
		if (!cell) return "none";
		const char* name = cell->GetEditorName();
		char text[200];
		sprintf_s(text, "cell '%s' (%08X), %s", name && name[0] ? name : "?", cell->refID, cell->IsInterior() ? "interior" : "exterior");
		return text;
	}

	void WriteReport(unsigned frame, unsigned burst) {
		Reports++;
		const unsigned n = Reports;
		Logger::Log("REPORT %u at frame %u (the report key, [Main.Main.Misc] ReportKey) -- you can paste everything from this line to 'REPORT %u pictures done' "
			"into a bug report", n, frame, n);
		Logger::Log("REPORT %u where: %s | %s | weather %s | game time %s (hour %.2f)", n, SessionLog::Where().c_str(), CellDetail().c_str(),
			SessionLog::Weather().c_str(), SessionLog::Clock().c_str(), TheShaderManager->ShaderConst.GameTime.y);
		const D3DXVECTOR4& p = TheRenderManager->CameraPosition;
		const D3DXVECTOR4& f = TheRenderManager->CameraForward;
		const float yaw = atan2f(f.x, f.y) * 57.29578f;
		const float pitch = asinf((std::min)(1.0f, (std::max)(-1.0f, f.z))) * 57.29578f;
		const ShaderManager::GameStateStruct& g = TheShaderManager->GameState;
		Logger::Log("REPORT %u camera: position %.0f, %.0f, %.0f, view yaw %.1f pitch %.1f deg, %s | underwater %s, dialogue %s, VATS %s, Pip-Boy %s, Vegas Shaders menu %s",
			n, p.x, p.y, p.z, yaw < 0.0f ? yaw + 360.0f : yaw, pitch, Player && Player->isThirdPerson ? "third person" : "first person",
			g.isUnderwater ? "yes" : "no", g.isDialog ? "yes" : "no", g.VATSIsOn ? "yes" : "no", g.PipBoyIsOn ? "yes" : "no", ImGuiManager::IsVisible() ? "open" : "closed");
		// The upscaler path (UNOFFICIAL 2026-11-01 step 2: shared with the FLASH / BEAT conditions line)
		Logger::Log("REPORT %u upscaling: %s", n, UpscalingText(frame).c_str());
		// UNOFFICIAL 2026-11-01 (a tester's 4K smear and "RT does nothing" reports): the temporal resolve's own state, the history
		// watch's verdicts, the programs round the game, the ray tracing state
		Logger::Log("REPORT %u temporal: %s", n, TheShaderManager->Effects.TAA ? TheShaderManager->Effects.TAA->UpscaleDiagText().c_str() : "no TAA effect");
		Logger::Log("REPORT %u history watch: %s", n, Diagnostics::HistoryWatchText().c_str());
		Logger::Log("REPORT %u environment: %s", n, SessionLog::Environment().c_str());
		{	// UNOFFICIAL 2026-11-01: every setting that differs from the defaults NOW (a tester's preset, loaded live after the SESSION
			// header, changed ~190 settings the header never saw): the same list as the header's, from the live values
			char settingsHead[48];
			sprintf_s(settingsHead, "REPORT %u settings", n);
			SessionLog::LogSettingsDiff(settingsHead);
		}
		Logger::Log("REPORT %u ray tracing: core: %s | %s | %s | %s", n, RtCore::StatusText() ? RtCore::StatusText() : "?", RtSunShadows::StatusText().c_str(),
			TheShaderManager->Effects.Reflections ? TheShaderManager->Effects.Reflections->RtStatusText().c_str() : "no Reflections effect", AmbientOcclusionEffect::RtStatusText().c_str());
		Logger::Log("REPORT %u ray tracing: %s", n, RtPathTrace::StatusText().c_str());	// UNOFFICIAL 2026-11-02
		{	// what the rays found: the traced outputs' means (a waiting 8x8 read-back of each, only here)
			IDirect3DDevice9* device = TheRenderManager->device;
			auto meanText = [&](const char* what, IDirect3DSurface9* surface, bool luma, const char* reading) {
				float x = 0.0f, l = 0.0f;
				const char* why = nullptr;
				char t[200];
				if (!surface) sprintf_s(t, "%s: not traced yet", what);
				else if (MeanOfSurface(device, surface, x, l, why)) sprintf_s(t, "%s: mean %.4f (%s)", what, luma ? l : x, reading);
				else sprintf_s(t, "%s: not read (%s)", what, why ? why : "?");
				return std::string(t);
			};
			ReflectionsEffect* refl = TheShaderManager->Effects.Reflections;
			Logger::Log("REPORT %u ray tracing outputs: %s | %s | %s", n,
				meanText("traced sun visibility", RtSunShadows::VisibilitySurface(), false, "1.0000 = every ray missed: nothing in shadow; 0 = all in shadow").c_str(),
				meanText("RT AO visibility", AmbientOcclusionEffect::RtRawSurface(), false, "1.0000 = no occlusion anywhere: the rays hit nothing").c_str(),
				meanText("RT reflections hit distance", refl ? refl->Rt.HitSurface : nullptr, false, "0.0000 = no ray hit anything (or no rays cast)").c_str());
		}
		// Frame times
		std::vector<float> times;
		SessionLog::RecentFrameTimes(times, 1024);
		double total = 0.0, worst = 0.0;
		unsigned count = 0;
		for (size_t i = times.size(); i-- > 0 && total < 5000.0; ) { total += times[i]; worst = (std::max)(worst, (double)times[i]); count++; }
		std::string last;
		for (size_t i = times.size() > 30 ? times.size() - 30 : 0; i < times.size(); i++) { char t[16]; sprintf_s(t, "%s%.1f", last.empty() ? "" : " ", times[i]); last += t; }
		Logger::Log("REPORT %u frame times: last %.1f s %u frames, avg %.2f ms, worst %.1f ms | last 30 (ms): %s", n, total / 1000.0, count,
			count ? total / count : 0.0, worst, last.c_str());
		char head[48];
		sprintf_s(head, "REPORT %u event", n);
		FrameJournal::Dump(head, 0, frame, 5.0);
		sprintf_s(head, "REPORT %u", n);
		DumpEffects(head, frame);
		DumpLevels(head);
		Logger::Log("REPORT %u counts: %s", n, CountsText().c_str());
		Logger::Log("REPORT %u flash detector: %s, %u frames judged, %u flashes this session", n, !F.on ? "off" : F.failed ? "turned itself off" : "on", F.judged, F.flashes);

		// The burst: this frame and the next count-1 rendered frames (their pictures before the HUD), one line each
		char firstFile[96], lastFile[96];
		const char* date = B.date;	// (set at the press, the burst's file names use it too)
		sprintf_s(firstFile, "NVR-Report-%s-%u-f%u-01of%02u.png", date, n, frame, burst);
		sprintf_s(lastFile, "NVR-Report-%s-%u-f<frame>-%02uof%02u.png", date, n, burst, burst);
		Logger::Log("REPORT %u screenshot: a burst of %u pictures, frames f%u to f%u if none is dropped ([Main.Main.Misc] ReportFrames), %s .. %s in "
			"%s, before the HUD; one 'REPORT %u frame k/%u' line in each of those frames, the pictures are written AFTER the burst ends ('REPORT %u picture "
			"k/%u' lines with each picture's mean luma and exposure); then this log, the RT / DLSS helpers' and DXVK's logs, the newest FNVProbe log and the "
			"settings file are copied in there", n, burst, frame, frame + burst - 1, firstFile, lastFile, B.folderName, n, burst, n, burst);
		Logger::Log("REPORT %u block end (the burst's per-frame lines follow)", n);
		if (InterfaceManager) {
			char message[96];
			sprintf_s(message, "Vegas Shaders report %u: capturing %u frames", n, burst);
			InterfaceManager->ShowMessage(message);
		}
	}

	struct Options { bool flash = true; int reportKey = 27; unsigned reportFrames = 16; double lastRead = -10.0; } O;
}

// --- EffectStatus ---

int EffectStatus::Register(const char* name) {
	if (!name) return -1;
	std::lock_guard<std::mutex> guard(RegisterLock);
	for (int k = 0; k < SlotCount; k++) if (Slots[k].name == name || !strcmp(Slots[k].name, name)) return k;
	if (SlotCount >= MaxSlots) return -1;
	Slots[SlotCount].name = name;
	return SlotCount++;
}

void EffectStatus::Note(int slot, Result result, const char* why) {
	if (slot < 0 || slot >= SlotCount) return;
	StatusSlot& s = Slots[slot];
	const unsigned i = EntryOf(s, FrameNow());
	if (result == Drawn) {
		if (s.result[i] != Drawn) { s.result[i] = Drawn; s.why[i] = why; }
		else if (why && !s.why[i]) s.why[i] = why;
	}
	else if (s.result[i] == NotCalled) { s.result[i] = result; s.why[i] = why; }
}

void EffectStatus::History(int slot, const char* what) {
	if (slot < 0 || slot >= SlotCount || !what) return;
	StatusSlot& s = Slots[slot];
	const unsigned i = EntryOf(s, FrameNow());
	if (!s.history[i] || strncmp(s.history[i], "reset", 5)) s.history[i] = what;	// a reset stays the frame's answer
}

void EffectStatus::Level(const char* what, const char* chosen, bool isDefault) {
	if (!what || !chosen) return;
	std::lock_guard<std::mutex> guard(LevelLock);
	for (LevelEntry& l : Levels) if (l.what == what) { l.chosen = chosen; l.isDefault = isDefault; return; }
	if (Levels.size() < 64) Levels.push_back({ what, chosen, isDefault });
}

void EffectStatus::ShaderLoaded(ShaderEvent what) {
	if ((unsigned)what < 5) InterlockedIncrement(&ShaderCounts[what]);
}

void EffectStatus::D3DFail(const char* site, long hr) {
	if (!site) return;
	std::lock_guard<std::mutex> guard(FailLock);
	for (FailSite& f : Failures) if (f.site == site || !strcmp(f.site, site)) { f.count++; f.minuteCount++; return; }
	if (Failures.size() >= 48) return;
	Failures.push_back({ site, 1, FrameNow(), 1, hr });
	Logger::Log("D3DFAIL %s: %08lX (first, at frame %u; later ones are counted in the HEARTBEAT)", site, (unsigned long)hr, FrameNow());
}

void EffectStatus::TargetSize(const char* name, unsigned width, unsigned height) {
	if (!name) return;
	std::lock_guard<std::mutex> guard(TargetLock);
	for (TargetEntry& t : Targets)
		if (t.name == name) {
			if (t.w != width || t.h != height) { Logger::Log("TARGET %s: %ux%u (was %ux%u)", name, width, height, t.w, t.h); t.w = width; t.h = height; }
			return;
		}
	if (Targets.size() < 512) Targets.push_back({ name, width, height });
}

// --- Diagnostics ---

void Diagnostics::OnFinalPicture(IDirect3DDevice9* device, IDirect3DSurface9* picture) {
	static unsigned lastFrame = 0xFFFFFFFF;
	const unsigned frame = FrameNow();
	if (frame == lastFrame || !TheShaderManager || !TheRenderManager || !TheSettingManager) return;
	lastFrame = frame;
	EndFrame(frame);

	const double now = Seconds();
	if (now - O.lastRead >= 1.0) {	// the switches, once a second (both live)
		O.lastRead = now;
		const bool flash = TheSettingManager->GetSettingI("Main.Main.Misc", "FlashDetect") != 0;
		if (flash != O.flash) { Logger::Log("FLASH detector %s ([Main.Main.Misc] FlashDetect)", flash ? "on" : "off"); if (!flash) ReleaseFlash(); }
		O.flash = flash;
		F.on = flash;
		O.reportKey = TheSettingManager->GetSettingI("Main.Main.Misc", "ReportKey");
		const int frames = TheSettingManager->GetSettingI("Main.Main.Misc", "ReportFrames");	// (0: missing from the file -> 16)
		O.reportFrames = frames <= 0 ? 16u : (unsigned)(std::min)(frames, (int)MaxBurst);
		// UNOFFICIAL 2026-11-01 the self-test (a one-frame dropout of one effect every N frames)
		const int every = TheSettingManager->GetSettingI("Main.Develop.Main", "FlashSelfTestEvery");
		if (every != T.every) {
			char effect[512] = "";
			TheSettingManager->GetSettingS("Main.Develop.Main", "FlashSelfTestEffect", effect);
			strncpy_s(T.effect, effect, _TRUNCATE);
			Logger::Log("FLASH self-test %s: %s left out of every %d%s frame on purpose ([Main.Develop.Main] FlashSelfTestEvery / FlashSelfTestEffect); each one must give a FLASH naming it as the source",
				every > 0 ? "ON" : "off", T.effect[0] ? T.effect : "(no effect named)", every, every == 2 ? "nd" : every == 3 ? "rd" : "th");
			T.every = every;
		}
		// UNOFFICIAL 2026-11-01 the stage probes' mode (live)
		const int stages = (std::min)(2, (std::max)(0, TheSettingManager->GetSettingI("Main.Main.Misc", "FlashStages")));
		if (S.modeRead && stages != S.mode) {
			Logger::Log("FLASH stage probes mode %d (%s) ([Main.Main.Misc] FlashStages)", stages, stages <= 0 ? "off" : stages == 1 ? "armed after a flash" : "always");
			if (stages <= 0) ReleaseStages();
		}
		S.modeRead = true;
		S.mode = stages;
		const bool beat = TheSettingManager->GetSettingI("Main.Main.Misc", "BeatDetect") != 0;	// UNOFFICIAL 2026-11-01 step 2
		if (Bt.onRead && beat != Bt.on) Logger::Log("BEAT detector %s ([Main.Main.Misc] BeatDetect)", beat ? "on" : "off");
		Bt.onRead = true;
		Bt.on = beat;
		const bool watch = TheSettingManager->GetSettingI("Main.Main.Misc", "HistoryWatch") != 0;	// UNOFFICIAL 2026-11-01 part 5
		if (Wt.onRead && watch != Wt.on) {
			Logger::Log("HISTORY watch %s ([Main.Main.Misc] HistoryWatch)", watch ? "on" : "off");
			if (!watch) for (int k = 0; k < Wt.count; k++) WatchRelease(Wt.slots[k]);
		}
		Wt.onRead = true;
		Wt.on = watch;
	}

	// The report key: the same game key check as the screenshot key (Global->OnKeyDown, DirectInput: works under DXVK; user 2026-11-01:
	// the first version polled GetAsyncKeyState with a foreground-window test and never fired), not while typing in an NVR text box.
	if (O.reportKey > 0 && O.reportKey < 256 && Global->OnKeyDown(O.reportKey) && !(ImGuiManager::IsVisible() && ImGuiManager::TextInputActive())) {
		if (B.phase != Burst::Idle)
			Logger::Log("REPORT key pressed during report %u's burst (%s): ignored", B.n, B.phase == Burst::Capturing ? "capturing" : "writing its pictures");
		else if (device) {
			const unsigned count = O.reportFrames;
			const time_t clock = time(NULL);
			strftime(B.date, sizeof(B.date), "%Y-%m-%d_%H-%M-%S", localtime(&clock));
			MakeReportFolder(Reports + 1);	// UNOFFICIAL 2026-11-01: the report's own folder (WriteReport numbers it Reports + 1 and names it)
			WriteReport(frame, count);
			StartBurst(device, picture, Reports, count);
			StagesArm("the report key");	// UNOFFICIAL 2026-11-01: a flash in the next 60 s is attributed to its stage
		}
	}
	// The burst (this frame's capture, or the read-back / writing after it); the flash detector below keeps running meanwhile
	if (device) BurstStep(device, picture, frame);

	if (device) StagesEndFrame(device, frame);	// UNOFFICIAL 2026-11-01: this frame's stage copies, the ones of 3 frames ago read (before the detector judges that frame)
	if (O.flash && !F.failed && device && picture) FlashFrame(device, picture, frame);
}

// --- the report burst's volumetric light stages (VL stages, above; each returns at once unless a burst captures this frame) ---

void Diagnostics::VLNote(const VLValues& values) {
	VLSlot* slot = VLSlotNow();
	if (!slot) return;
	slot->values = values;
	slot->noted = true;
	slot->runs++;
}

void Diagnostics::VLCapture(IDirect3DDevice9* device, VLStage stage, IDirect3DSurface9* surface) {
	if (stage < VLMarch || stage > VLBlur) return;
	VLShrink(device, stage, surface);
}

void Diagnostics::VLFrame(IDirect3DDevice9* device, bool after) {
	if (!VLSlotNow() || !TheShaderManager || !TheTextureManager) return;
	// Before: what the composite reads (TESR_RenderedBuffer; the game target when the chain's last pass wrote it, which Owns() then
	// copies back). After: the picture it wrote, as PixelInspector::AfterFrame finds it (the chain's current buffer after a Commit, the
	// game target after CommitFinal / Sync); without the chain RenderedSurface either way (the legacy path copies each result there).
	FrameChain& chain = TheShaderManager->Chain;
	IDirect3DSurface9* picture = TheTextureManager->RenderedSurface;
	if (chain.IsActive() && chain.FinalSurface() && (after ? chain.LastWriteKind() > 1 : chain.FinalIsWritten())) picture = chain.FinalSurface();
	VLShrink(device, after ? VLAfter : VLBefore, picture);
}

void Diagnostics::VLPass(unsigned technique, bool vulkan) {
	VLSlot* slot = VLSlotNow();
	if (!slot) return;
	const unsigned i = technique == 8 ? 0 : technique;
	if (i < 5) slot->path[i] = vulkan ? 'V' : 'D';
}

void Diagnostics::Heartbeat() {
	std::vector<std::string> items;
	for (int k = 0; k < SlotCount; k++) {
		StatusSlot& s = Slots[k];
		if (s.drawn + s.skipped == 0 && !s.resets) continue;
		char text[300];
		int length = sprintf_s(text, "%s %u/%u", s.name, s.drawn, s.skipped);
		if (s.skipped && s.lastSkip) length += sprintf_s(text + length, sizeof(text) - length, " (%s)", s.lastSkip);
		if (s.resets) sprintf_s(text + length, sizeof(text) - length, " %u history resets (last: %s)", s.resets, s.lastReset ? s.lastReset : "?");
		items.push_back(text);
		s.drawn = s.skipped = s.resets = 0;
		s.lastSkip = s.lastReset = nullptr;
	}
	LogList("HEARTBEAT effects (frames drawn/skipped in the minute): ", items, " | ");
	if (O.flash || F.failed)
		Logger::Log("HEARTBEAT flash detector: %s, %u frames judged, %u flashes (%u this session, %u dumps), %u candidates ignored%s%s%s, %u late read-backs, cost %.3f ms a frame",
			F.failed ? "turned itself off" : "on", F.minuteJudged, F.minuteFlashes, F.flashes, F.dumps, F.minuteIgnored, F.minuteIgnored ? " (last: " : "",
			F.minuteIgnored ? F.minuteIgnoredWhy.c_str() : "", F.minuteIgnored ? ")" : "", F.minuteLate,
			F.minuteCostFrames ? F.minuteCost / F.minuteCostFrames * 1000.0 : 0.0);
	F.minuteJudged = F.minuteFlashes = F.minuteIgnored = F.minuteLate = 0;
	F.minuteCost = 0.0; F.minuteCostFrames = 0;
	// UNOFFICIAL 2026-11-01 the stage probes
	if (S.mode > 0 || S.failed) {
		char skipped[160] = "";
		if (S.full || S.msaa || S.unknownFormat || S.noFormatSlot)
			sprintf_s(skipped, " | probes skipped (session): %u over the %d-stage cap, %u multisampled pictures, %u unknown formats, %u over 3 formats", S.full, FlashRule::MaxStages,
				S.msaa, S.unknownFormat, S.noFormatSlot);
		Logger::Log("HEARTBEAT stage probes: %s, %u frames probed (%.1f stages a frame), %u late read-backs, cost %.3f ms a probed frame%s",
			S.failed ? "turned themselves off" : S.mode >= 2 ? "always on" : Seconds() < S.armedUntil ? "armed, running" : "armed, idle (no flash in the last 60 s)",
			S.minuteProbedFrames, S.minuteProbedFrames ? (double)S.minuteStages / S.minuteProbedFrames : 0.0, S.minuteLate,
			S.minuteCostFrames ? S.minuteCost / S.minuteCostFrames * 1000.0 : 0.0, skipped);
	}
	S.minuteProbedFrames = S.minuteStages = S.minuteLate = 0;
	S.minuteCost = 0.0; S.minuteCostFrames = 0;
	// UNOFFICIAL 2026-11-01 step 2 the beat detector
	if (Bt.on || Bt.lines)
		Logger::Log("HEARTBEAT beat detector: %s, %u windows of %d still frames judged, %u beats (%u BEAT lines this session), %u windows not judged (camera moving, menu or load)",
			Bt.on ? "on" : "off", Bt.minuteChecks, BeatRule::Window, Bt.minuteBeats, Bt.lines, Bt.minuteNotStill);
	Bt.minuteChecks = Bt.minuteBeats = Bt.minuteNotStill = 0;
	// UNOFFICIAL 2026-11-01 part 5 the history watch
	if ((Wt.on && Wt.count) || Wt.failed) {
		std::string slots;
		for (int k = 0; k < Wt.count; k++) {
			Watch::Slot& s = Wt.slots[k];
			slots += (k ? " | " : "") + WatchSlotText(s, true);
			s.minuteJudged = s.minuteLate = s.minuteBad = 0;
			s.driftSum = 0.0; s.driftCount = 0; s.driftMax = 0.0f;
		}
		Logger::Log("HEARTBEAT history watch: %s, cost %.3f ms a probe | %s", Wt.failed ? "turned itself off" : "on", Wt.minuteCostFrames ? Wt.minuteCost / Wt.minuteCostFrames * 1000.0 : 0.0, slots.c_str());
		Wt.minuteCost = 0.0; Wt.minuteCostFrames = 0;
	}
	// The counters when they changed, and the minute's call failures
	bool changed = false;
	for (int i = 0; i < 5; i++) if ((unsigned)ShaderCounts[i] != LastShaderCounts[i]) { changed = true; LastShaderCounts[i] = (unsigned)ShaderCounts[i]; }
	bool failures = false;
	{
		std::lock_guard<std::mutex> guard(FailLock);
		for (FailSite& f : Failures) if (f.minuteCount) { failures = true; f.minuteCount = 0; }
	}
	if (changed || failures) Logger::Log("HEARTBEAT counts (session): %s", CountsText().c_str());
}

// --- the stage probes' entry points (Diagnostics.h part 4) ---

void Diagnostics::StageAfterEffect(EffectRecord* effect) {
	if (!effect || !StagesRunning() || !TheShaderManager || !TheTextureManager) return;
	const unsigned frame = FrameNow();
	StageFrameFor(frame);
	FrameChain& chain = TheShaderManager->Chain;
	IDirect3DSurface9* image = TheTextureManager->RenderedSurface;
	if (chain.IsActive()) {	// as PixelInspector::AfterFrame: a stage only when the frame was written since the last one
		if (chain.WriteCount() == S.lastWrites) return;
		S.lastWrites = chain.WriteCount();
		if (chain.LastWriteKind() != 1) image = chain.FinalSurface();
	}
	else {	// without the chain: the effect drew this frame (the status registry), one stage for consecutive Render calls of the same effect
		const StatusSlot* slot = Find(effect->StatusSlot);
		if (!slot || !Has(*slot, frame) || slot->result[frame % EffectStatus::Ring] != EffectStatus::Drawn) return;
		if (effect == S.lastEffect) return;
	}
	S.lastEffect = effect;
	Probe(effect->Name, image, 0);
}

void Diagnostics::StageMark(const char* name, IDirect3DSurface9* picture) {
	if (!StagesRunning() || !TheShaderManager) return;
	StageFrameFor(FrameNow());
	S.lastWrites = TheShaderManager->Chain.WriteCount();	// the effects after this mark count from here
	S.lastEffect = nullptr;
	Probe(name, picture, 0);
}

void Diagnostics::ProbeOwn(const char* name, IDirect3DSurface9* buffer) {
	if (!StagesRunning()) return;
	Probe(name, buffer, 1);
}

bool Diagnostics::SelfTestSkip(const EffectRecord* effect) {
	if (T.every <= 0 || !effect || !effect->Name || !T.effect[0]) return false;
	const unsigned frame = FrameNow();
	if (frame == 0 || frame % (unsigned)T.every || _stricmp(effect->Name, T.effect)) return false;
	bool noted = false;
	for (unsigned k = 0; k < 16 && !noted; k++) noted = T.frames[k] == frame;	// (an effect may Render more than once a frame)
	if (!noted) {
		T.frames[T.skips % 16] = frame;
		if (T.skips++ < 3) Logger::Log("FLASH self-test: %s left out of frame %u on purpose (skip %u; later ones are not logged, their verdicts are)", effect->Name, frame, T.skips);
	}
	return true;
}

// --- UNOFFICIAL 2026-11-01 the history watch's entry points (Diagnostics.h part 5) ---

void Diagnostics::HistoryProbe(const char* name, IDirect3DSurface9* input, unsigned inputWidth, unsigned inputHeight, IDirect3DSurface9* output, std::string (*state)()) {
	if (!Wt.on || Wt.failed || !name || !output || !TheRenderManager || !TheRenderManager->device || !TheShaderManager) return;
	IDirect3DDevice9* device = TheRenderManager->device;
	const unsigned frame = FrameNow();
	Watch::Slot* slot = nullptr;
	for (int k = 0; k < Wt.count && !slot; k++) if (!strcmp(Wt.slots[k].name, name)) slot = &Wt.slots[k];
	if (!slot) { if (Wt.count >= WatchSlots) return; slot = &Wt.slots[Wt.count++]; slot->name = name; }
	Watch::Slot& s = *slot;
	s.state = state;
	if (s.usedFrame == frame) return;	// one probe per name and frame
	s.usedFrame = frame;
	const double start = Seconds();

	D3DSURFACE_DESC od = {}, id = {};
	if (FAILED(output->GetDesc(&od))) return;
	if (!WatchFormatOk(od.Format)) { char why[96]; sprintf_s(why, "%s: output format %d not handled", name, (int)od.Format); WatchOff(why); return; }
	const bool hasIn = input && SUCCEEDED(input->GetDesc(&id)) && WatchFormatOk(id.Format) && id.MultiSampleType == D3DMULTISAMPLE_NONE;
	const UINT inW = hasIn ? (inputWidth ? (std::min)((UINT)inputWidth, id.Width) : id.Width) : 0;
	const UINT inH = hasIn ? (inputHeight ? (std::min)((UINT)inputHeight, id.Height) : id.Height) : 0;
	if (!s.out.Fits(od, od.Width, od.Height)) {
		if (!s.out.Make(device, od, od.Width, od.Height) || !WatchMakeRing(device, s, false, od.Format)) { WatchOff("its small render targets / read-back surfaces could not be made"); return; }
		s.outFormat = od.Format;
		for (auto& m : s.samples) m.valid = false;
	}
	if (hasIn && !s.in.Fits(id, inW, inH)) {
		if (!s.in.Make(device, id, inW, inH) || !WatchMakeRing(device, s, true, id.Format)) { WatchOff("its input's small render targets could not be made"); return; }
		s.inFormat = id.Format;
	}
	HRESULT hr = s.out.Run(device, output);
	if (FAILED(hr)) { EffectStatus::D3DFail("history watch StretchRect", hr); WatchOff("StretchRect failed"); return; }
	if (hasIn && FAILED(hr = s.in.Run(device, input))) { EffectStatus::D3DFail("history watch StretchRect (input)", hr); WatchOff("StretchRect of the input failed"); return; }
	Watch::Readback& w = s.ring[frame % WatchReadbacks];
	hr = device->GetRenderTargetData(s.out.Last(), w.out);
	w.pending = SUCCEEDED(hr);
	if (FAILED(hr)) EffectStatus::D3DFail("history watch GetRenderTargetData", hr);
	w.hasIn = hasIn && w.in && SUCCEEDED(device->GetRenderTargetData(s.in.Last(), w.in));
	w.frame = frame;
	w.quiet = QuietReason(frame, 0.0f);

	// The copy made 3 frames ago
	Watch::Readback& r = s.ring[(frame + 1) % WatchReadbacks];
	if (r.pending) {
		r.pending = false;
		Watch::Sample& m = s.samples[r.frame % WatchRing];
		float lumaOut[FlashRule::Cells], lumaIn[FlashRule::Cells];
		bool late = false;
		m.valid = WatchRead(r.out, s.outFormat, lumaOut, m.bad, m.meanOut, m.maxOut, late);
		if (late) { s.late++; s.minuteLate++; }
		m.frame = r.frame; m.quiet = r.quiet; m.hasIn = false; m.drift = 0.0f; m.meanIn = 0.0f;
		if (m.valid) {
			m.hash = CycleRule::Hash(lumaOut);
			unsigned badIn = 0;
			float meanIn = 0.0f, peakIn = 0.0f;
			if (r.hasIn && WatchRead(r.in, s.inFormat, lumaIn, badIn, meanIn, peakIn, late)) {
				double diff = 0.0;
				for (int i = 0; i < FlashRule::Cells; i++) diff += fabs((double)lumaOut[i] - lumaIn[i]);
				m.meanIn = meanIn;
				m.drift = (float)(diff / FlashRule::Cells / (std::max)(0.05, (double)meanIn));
				m.hasIn = true;
			}
			WatchJudge(s, m);
		}
	}
	if (!Wt.announced) {
		Wt.announced = true;
		Logger::Log("HISTORY watch on: '%s' hands its input and output here each frame (8x8 on the GPU, read back 3 frames late without waiting); RUNAWAY = the output off its input "
			"by over %.0f %% for %u frames, CYCLE = an exact repeat every 2..%d frames, plus NaN / infinite cells ([Main.Main.Misc] HistoryWatch)", name, DriftLimit * 100.0f, DriftFrames, CycleRule::MaxPeriod);
	}
	const double cost = Seconds() - start;
	Wt.costSum += cost; Wt.costFrames++; Wt.minuteCost += cost; Wt.minuteCostFrames++;
	if (Wt.costFrames == 600) {
		const double average = Wt.costSum / Wt.costFrames * 1000.0;
		Wt.costSum = 0.0; Wt.costFrames = 0;
		if (average > 0.5) { char why[160]; sprintf_s(why, "it cost %.2f ms a probe on average here (over 0.5 ms: this driver's read-back waits)", average); WatchOff(why); }
	}
}

std::string Diagnostics::HistoryWatchText() {
	if (Wt.failed) return "turned itself off (logged)";
	if (!Wt.on) return "off ([Main.Main.Misc] HistoryWatch)";
	if (!Wt.count) return "on, nothing probed yet (no TAA / upscaler frame so far)";
	std::string text;
	for (int k = 0; k < Wt.count; k++) text += (k ? " | " : "") + WatchSlotText(Wt.slots[k], false);
	return text;
}
