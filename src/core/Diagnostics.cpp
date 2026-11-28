// UNOFFICIAL 2026-11-01: the effects' status registry (EffectStatus.h), the automatic flash detector and the report key (Diagnostics.h).
#include "Diagnostics.h"
#include "FlashRule.h"
#include "BeatSeries.h"	// UNOFFICIAL 2026-11-11 the world draw's per-frame series, matched against a beat
#include "TemporalBackbone.h"
#include "ImGuiManager.h"
#include "EffectSelfTestCore.h"	// SelfTestCore::ConvertRow / SavePng (WIC, no device) for the report burst
#include "RtCore.h"					// UNOFFICIAL 2026-11-01 the REPORT block's ray tracing line
#include "RtSunShadows.h"
#include "RtFogVisibility.h"	// UNOFFICIAL 2026-11-07 (ptaaa-fog) the REPORT block's line
#include "RtPathTrace.h"	// UNOFFICIAL 2026-11-02 the reference path tracer's state line
#include "VaMemoryMap.h"	// UNOFFICIAL 2026-11-23: 32-bit VA space breakdown for bug reports
#include "../effects/Reflections/Reflections.h"
#include "../effects/AmbientOcclusion/AmbientOcclusion.h"
#include "RenderScaleGuardRule.h"	// UNOFFICIAL 2026-11-23 RenderScaleGuard
#include "ScopeOverlay.h"
#include "../effects/TAA/TAA.h"
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
		sprintf_s(text, "shader cache: %ld hits, %ld compiled, %ld precompiled, %ld compile failures, %ld PREFER_FLOW_CONTROL retries, %ld filled from the release's cache",
			ShaderCounts[EffectStatus::CacheHit], ShaderCounts[EffectStatus::Compiled], ShaderCounts[EffectStatus::Precompiled],
			ShaderCounts[EffectStatus::CompileFailed], ShaderCounts[EffectStatus::FlowControlRetry], ShaderProgram::ShippedSeeds());	// (the last: 2026-11-10)
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

	// --- UNOFFICIAL 2026-11-23 RenderScaleGuard: runtime corner-box / seam self-check watchdog ---
	struct RenderScaleGuardState {
		bool on = true, onRead = false, failed = false, formatWarned = false;
		D3DFORMAT format = D3DFMT_UNKNOWN;
		D3DTEXTUREFILTERTYPE filter = D3DTEXF_LINEAR;
		IDirect3DSurface9* hRt = nullptr;	// 64 x 4 bands straddling y = renderHeight
		IDirect3DSurface9* vRt = nullptr;	// 4 x 64 bands straddling x = renderWidth
		struct Slot {
			IDirect3DSurface9* hSys = nullptr;
			IDirect3DSurface9* vSys = nullptr;
			bool pending = false;
			unsigned frame = 0;
			UINT rw = 0, rh = 0, sw = 0, sh = 0;
			UINT probeW = 0, probeH = 0;
			bool isSq = false;
			bool isV2 = false;
			float scale = 1.0f;
			const char* upscaler = nullptr;
		};
		Slot ring[4];
		int next = 0;
		unsigned lastSubmitFrame = 0, burstLeft = 20, cooldownUntil = 0, warmupLeft = 5;
		int confirmStreak = 0;
		bool probeSqNext = false, lockedSq = false, slowMode = false;
		UINT lastRw = 0, lastRh = 0;
		bool lastV2 = false;
		const void* lastCell = nullptr;
		const char* lastUpscaler = nullptr;
		unsigned probes = 0, minuteProbes = 0, minuteLate = 0, tripsV1 = 0, tripsNative = 0;
		double cost = 0.0, minuteCost = 0.0;
		unsigned costProbes = 0, minuteCostProbes = 0;
	} RsGuard;

	void ReleaseRsGuard() {
		if (RsGuard.hRt) { RsGuard.hRt->Release(); RsGuard.hRt = nullptr; }
		if (RsGuard.vRt) { RsGuard.vRt->Release(); RsGuard.vRt = nullptr; }
		for (auto& s : RsGuard.ring) {
			if (s.hSys) { s.hSys->Release(); s.hSys = nullptr; }
			if (s.vSys) { s.vSys->Release(); s.vSys = nullptr; }
			s.pending = false;
		}
		RsGuard.format = D3DFMT_UNKNOWN;
	}

	bool RsFormatOk(D3DFORMAT f) {
		return f == D3DFMT_A8R8G8B8 || f == D3DFMT_X8R8G8B8 ||
			f == D3DFMT_A8B8G8R8 || f == D3DFMT_X8B8G8R8 ||
			f == D3DFMT_A2R10G10B10 || f == D3DFMT_A2B10G10R10 ||
			f == D3DFMT_A16B16G16R16F;
	}

	bool EnsureRsGuard(IDirect3DDevice9* device, D3DFORMAT fmt) {
		if (RsGuard.hRt && RsGuard.vRt && RsGuard.format == fmt) return true;
		ReleaseRsGuard();
		if (!RsFormatOk(fmt)) {
			if (!RsGuard.formatWarned) {
				RsGuard.formatWarned = true;
				Logger::Log("UNOFFICIAL RenderScaleGuard: unsupported picture format %d (skipped)", (int)fmt);
			}
			return false;
		}
		if (FAILED(device->CreateRenderTarget(RenderScaleGuardRule::Samples, 4, fmt, D3DMULTISAMPLE_NONE, 0, FALSE, &RsGuard.hRt, NULL)) ||
			FAILED(device->CreateRenderTarget(4, RenderScaleGuardRule::Samples, fmt, D3DMULTISAMPLE_NONE, 0, FALSE, &RsGuard.vRt, NULL))) {
			ReleaseRsGuard();
			RsGuard.failed = true;
			Logger::Log("[WARN] UNOFFICIAL RenderScaleGuard: off for this session (could not create probe render targets)");
			return false;
		}
		for (auto& s : RsGuard.ring) {
			if (FAILED(device->CreateOffscreenPlainSurface(RenderScaleGuardRule::Samples, 4, fmt, D3DPOOL_SYSTEMMEM, &s.hSys, NULL)) ||
				FAILED(device->CreateOffscreenPlainSurface(4, RenderScaleGuardRule::Samples, fmt, D3DPOOL_SYSTEMMEM, &s.vSys, NULL))) {
				ReleaseRsGuard();
				RsGuard.failed = true;
				Logger::Log("[WARN] UNOFFICIAL RenderScaleGuard: off for this session (could not create system-memory readback surfaces)");
				return false;
			}
		}
		RsGuard.format = fmt;
		RsGuard.filter = D3DTEXF_LINEAR;
		return true;
	}

	RenderScaleGuardRule::Rgb RsDecodeRgb(const unsigned char* row, int x, D3DFORMAT fmt) {
		RenderScaleGuardRule::Rgb out;
		if (fmt == D3DFMT_A16B16G16R16F) {
			const unsigned short* p = (const unsigned short*)(row + x * 8);
			out.r = HalfToFloat(p[0]); out.g = HalfToFloat(p[1]); out.b = HalfToFloat(p[2]);
		}
		else if (fmt == D3DFMT_A2R10G10B10 || fmt == D3DFMT_A2B10G10R10) {
			const UInt32 v = *(const UInt32*)(row + x * 4);
			const float lo = (v & 1023) / 1023.0f, mid = ((v >> 10) & 1023) / 1023.0f, hi = ((v >> 20) & 1023) / 1023.0f;
			if (fmt == D3DFMT_A2R10G10B10) { out.b = lo; out.g = mid; out.r = hi; } else { out.r = lo; out.g = mid; out.b = hi; }
		}
		else if (fmt == D3DFMT_A8B8G8R8 || fmt == D3DFMT_X8B8G8R8) {
			const unsigned char* p = row + x * 4;
			out.r = p[0] / 255.0f; out.g = p[1] / 255.0f; out.b = p[2] / 255.0f;
		}
		else {
			const unsigned char* p = row + x * 4;
			out.b = p[0] / 255.0f; out.g = p[1] / 255.0f; out.r = p[2] / 255.0f;
		}
		return out;
	}

	HRESULT RsStretch(IDirect3DDevice9* device, IDirect3DSurface9* src, const RECT& srcRect, IDirect3DSurface9* dst, const RECT& dstRect) {
		HRESULT hr = device->StretchRect(src, &srcRect, dst, &dstRect, RsGuard.filter);
		if (FAILED(hr) && RsGuard.filter == D3DTEXF_LINEAR) {
			RsGuard.filter = D3DTEXF_POINT;
			hr = device->StretchRect(src, &srcRect, dst, &dstRect, RsGuard.filter);
		}
		return hr;
	}

	void RenderScaleGuardFrame(IDirect3DDevice9* device, IDirect3DSurface9* picture, unsigned frame) {
		if (!RsGuard.on || RsGuard.failed || !device || !picture) return;
		UINT rw = 0, rh = 0, sw = 0, sh = 0;
		bool isV2 = false;
		float scale = 1.0f;
		const char* upscaler = nullptr;
		if (!TAAEffect::RenderScaleGuardInfo(rw, rh, sw, sh, isV2, scale, upscaler)) {
			RsGuard.confirmStreak = 0;
			return;
		}

		const void* cell = Player ? Player->parentCell : nullptr;
		const bool upscalerChanged = (upscaler != RsGuard.lastUpscaler) &&
			(!upscaler || !RsGuard.lastUpscaler || strcmp(upscaler, RsGuard.lastUpscaler) != 0);
		if (cell != RsGuard.lastCell || rw != RsGuard.lastRw || rh != RsGuard.lastRh || isV2 != RsGuard.lastV2 || upscalerChanged) {
			RsGuard.lastCell = cell;
			RsGuard.lastRw = rw;
			RsGuard.lastRh = rh;
			RsGuard.lastV2 = isV2;
			RsGuard.lastUpscaler = upscaler;
			RsGuard.confirmStreak = 0;
			RsGuard.lockedSq = false;
			RsGuard.probeSqNext = false;
			RsGuard.burstLeft = 20;
			for (auto& s : RsGuard.ring) {
				if (s.pending && (s.rw != rw || s.rh != rh || s.sw != sw || s.sh != sh || s.isV2 != isV2 || upscalerChanged)) s.pending = false;
			}
		}

		const double t0 = Seconds();
		bool didWork = false;
		HWND fg = GetForegroundWindow();
		DWORD fgPid = 0;
		const bool unfocused = !fg || (GetWindowThreadProcessId(fg, &fgPid), fgPid != GetCurrentProcessId()) || IsIconic(fg);
		const bool quietNow = unfocused || QuietReason(frame, 0.0f) != nullptr || ScopeOverlay::Visible();

		for (auto& slot : RsGuard.ring) {
			if (!slot.pending) continue;
			if (frame < slot.frame) { slot.pending = false; continue; }
			if (frame - slot.frame < 3) continue;
			if (slot.rw != rw || slot.rh != rh || slot.sw != sw || slot.sh != sh || slot.isV2 != isV2) {
				slot.pending = false;
				continue;
			}
			if (RsGuard.confirmStreak > 0 && slot.isSq != RsGuard.lockedSq) {
				slot.pending = false;
				continue;
			}
			didWork = true;
			D3DLOCKED_RECT hLock = {}, vLock = {};
			const HRESULT hrH = slot.hSys->LockRect(&hLock, NULL, D3DLOCK_READONLY | D3DLOCK_DONOTWAIT);
			if (hrH == D3DERR_WASSTILLDRAWING) {
				RsGuard.minuteLate++;
				if (frame - slot.frame > 16) slot.pending = false;
				continue;
			}
			if (FAILED(hrH)) {
				EffectStatus::D3DFail("RenderScaleGuard LockRect H", hrH);
				slot.pending = false;
				continue;
			}
			const HRESULT hrV = slot.vSys->LockRect(&vLock, NULL, D3DLOCK_READONLY | D3DLOCK_DONOTWAIT);
			if (hrV == D3DERR_WASSTILLDRAWING) {
				slot.hSys->UnlockRect();
				RsGuard.minuteLate++;
				if (frame - slot.frame > 16) slot.pending = false;
				continue;
			}
			if (FAILED(hrV)) {
				slot.hSys->UnlockRect();
				EffectStatus::D3DFail("RenderScaleGuard LockRect V", hrV);
				slot.pending = false;
				continue;
			}
			RenderScaleGuardRule::Rgb h[4][RenderScaleGuardRule::Samples];
			RenderScaleGuardRule::Rgb v[4][RenderScaleGuardRule::Samples];
			for (int band = 0; band < 4; band++) {
				const unsigned char* hRow = (const unsigned char*)hLock.pBits + (size_t)band * hLock.Pitch;
				for (int x = 0; x < RenderScaleGuardRule::Samples; x++) h[band][x] = RsDecodeRgb(hRow, x, RsGuard.format);
			}
			for (int y = 0; y < RenderScaleGuardRule::Samples; y++) {
				const unsigned char* vRow = (const unsigned char*)vLock.pBits + (size_t)y * vLock.Pitch;
				for (int band = 0; band < 4; band++) v[band][y] = RsDecodeRgb(vRow, band, RsGuard.format);
			}
			slot.hSys->UnlockRect();
			slot.vSys->UnlockRect();
			slot.pending = false;

			if (frame < RsGuard.cooldownUntil || quietNow) continue;
			RsGuard.probes++;
			RsGuard.minuteProbes++;
			const RenderScaleGuardRule::Verdict verdict = RenderScaleGuardRule::Judge(h, v);
			if (verdict.seam) {
				if (RsGuard.confirmStreak == 0) RsGuard.lockedSq = slot.isSq;
				RsGuard.confirmStreak++;
				if (RsGuard.confirmStreak >= RenderScaleGuardRule::Thresholds().ConfirmNeeded) {
					for (auto& s : RsGuard.ring) s.pending = false;
					RsGuard.confirmStreak = 0;
					RsGuard.lockedSq = false;
					RsGuard.probeSqNext = false;
					RsGuard.cooldownUntil = frame + 30;
					char reason[256];
					if (slot.isSq) {
						sprintf_s(reason, "RenderScale scale^2 seam detected at %ux%u (render %ux%u / %ux%u, %s %.2f, H=%d/%d mean=%.2f, V=%d/%d mean=%.2f)",
							slot.probeW, slot.probeH, slot.rw, slot.rh, slot.sw, slot.sh, slot.upscaler ? slot.upscaler : "Upscaler", slot.scale,
							verdict.hHits, RenderScaleGuardRule::Samples, verdict.hMean, verdict.vHits, RenderScaleGuardRule::Samples, verdict.vMean);
					}
					else {
						sprintf_s(reason, "RenderScale seam detected at %ux%u/%ux%u (%s %.2f, H=%d/%d mean=%.2f, V=%d/%d mean=%.2f)",
							slot.probeW, slot.probeH, slot.sw, slot.sh, slot.upscaler ? slot.upscaler : "Upscaler", slot.scale,
							verdict.hHits, RenderScaleGuardRule::Samples, verdict.hMean, verdict.vHits, RenderScaleGuardRule::Samples, verdict.vMean);
					}
					if (slot.isV2 && !TAAEffect::IsDeferredV1Unsafe()) {
						RsGuard.tripsV1++;
						RsGuard.burstLeft = 20;
						Logger::Log("[WARN] UNOFFICIAL RenderScaleGuard: %s (f%u, v2) -> switching to v1", reason, slot.frame);
						TAAEffect::ForceRenderScaleV1(reason);
					}
					else {
						RsGuard.tripsNative++;
						Logger::Log("[WARN] UNOFFICIAL RenderScaleGuard: %s (f%u, %s) -> forcing native 1.0x",
							reason, slot.frame, slot.isV2 ? "v2, DeferredRenderer active" : "v1");
						TAAEffect::ForceRenderScaleNative(reason);
					}
					break;
				}
			}
			else {
				RsGuard.confirmStreak = 0;
				RsGuard.lockedSq = false;
			}
		}

		const unsigned steadyInterval = RsGuard.slowMode ? 120u : 60u;
		const unsigned interval = RsGuard.confirmStreak > 0 ? 5u : (RsGuard.burstLeft > 0 ? 15u : steadyInterval);
		const bool due = RsGuard.lastSubmitFrame == 0 || frame < RsGuard.lastSubmitFrame || (frame - RsGuard.lastSubmitFrame >= interval);
		if (due && !quietNow && frame >= RsGuard.cooldownUntil && RsGuard.on && !RsGuard.failed) {
			uint32_t rw2 = 0, rh2 = 0;
			const bool canSq = RenderScaleGuardRule::SecondarySeam(rw, rh, sw, sh, rw2, rh2);
			bool useSq = false;
			if (RsGuard.confirmStreak > 0) useSq = RsGuard.lockedSq && canSq;
			else if (canSq) {
				useSq = RsGuard.probeSqNext;
				RsGuard.probeSqNext = !RsGuard.probeSqNext;
			}
			const UINT pw = useSq ? rw2 : rw;
			const UINT ph = useSq ? rh2 : rh;
			D3DSURFACE_DESC d = {};
			if (SUCCEEDED(picture->GetDesc(&d)) && d.MultiSampleType == D3DMULTISAMPLE_NONE &&
				d.Width == sw && d.Height == sh &&
				pw >= 24 && ph >= 24 && pw + 12 < d.Width && ph + 12 < d.Height &&
				EnsureRsGuard(device, d.Format)) {
				didWork = true;
				const LONG mx = (std::min)((LONG)pw - 8, (std::max)(8L, (LONG)(pw / 25)));
				const LONG my = (std::min)((LONG)ph - 8, (std::max)(8L, (LONG)(ph / 25)));
				const LONG hDy[4][2] = { { -10, -7 }, { -3, 0 }, { 1, 4 }, { 8, 11 } };
				const LONG vDx[4][2] = { { -10, -7 }, { -3, 0 }, { 1, 4 }, { 8, 11 } };
				bool copyOk = true;
				for (int band = 0; band < 4 && copyOk; band++) {
					const RECT srcH = { mx, (LONG)ph + hDy[band][0], (LONG)pw - mx, (LONG)ph + hDy[band][1] };
					const RECT dstH = { 0, band, RenderScaleGuardRule::Samples, band + 1 };
					const HRESULT hr = RsStretch(device, picture, srcH, RsGuard.hRt, dstH);
					if (FAILED(hr)) { EffectStatus::D3DFail("RenderScaleGuard StretchRect H", hr); copyOk = false; }
				}
				for (int band = 0; band < 4 && copyOk; band++) {
					const RECT srcV = { (LONG)pw + vDx[band][0], my, (LONG)pw + vDx[band][1], (LONG)ph - my };
					const RECT dstV = { band, 0, band + 1, RenderScaleGuardRule::Samples };
					const HRESULT hr = RsStretch(device, picture, srcV, RsGuard.vRt, dstV);
					if (FAILED(hr)) { EffectStatus::D3DFail("RenderScaleGuard StretchRect V", hr); copyOk = false; }
				}
				if (copyOk) {
					RenderScaleGuardState::Slot& slot = RsGuard.ring[RsGuard.next];
					const HRESULT hrG1 = device->GetRenderTargetData(RsGuard.hRt, slot.hSys);
					const HRESULT hrG2 = SUCCEEDED(hrG1) ? device->GetRenderTargetData(RsGuard.vRt, slot.vSys) : hrG1;
					if (SUCCEEDED(hrG2)) {
						slot.pending = true;
						slot.frame = frame;
						slot.rw = rw; slot.rh = rh; slot.sw = sw; slot.sh = sh;
						slot.probeW = pw; slot.probeH = ph;
						slot.isSq = useSq;
						slot.isV2 = isV2;
						slot.scale = scale;
						slot.upscaler = upscaler;
						RsGuard.next = (RsGuard.next + 1) & 3;
						RsGuard.lastSubmitFrame = frame;
						if (RsGuard.burstLeft > 0) RsGuard.burstLeft--;
					}
					else {
						slot.pending = false;
						EffectStatus::D3DFail("RenderScaleGuard GetRenderTargetData", hrG2);
					}
				}
			}
		}

		if (didWork) {
			const double dt = Seconds() - t0;
			if (RsGuard.warmupLeft > 0) {
				RsGuard.warmupLeft--;
			}
			else {
				RsGuard.cost += dt;
				RsGuard.costProbes++;
				RsGuard.minuteCost += dt;
				RsGuard.minuteCostProbes++;
				const double avg = RsGuard.costProbes > 0 ? (RsGuard.cost / RsGuard.costProbes) : 0.0;
				if (!RsGuard.slowMode && RsGuard.costProbes >= 20 && avg > 0.001) {
					RsGuard.slowMode = true;
					Logger::Log("[WARN] UNOFFICIAL RenderScaleGuard: probe cost averaged %.3f ms (> 1.0 ms); reducing steady-state probe frequency to every 120 frames", avg * 1000.0);
				}
				if (RsGuard.costProbes >= 40 && avg > 0.005) {
					const double avgMs = avg * 1000.0;
					RsGuard.failed = true;
					ReleaseRsGuard();
					Logger::Log("[WARN] UNOFFICIAL RenderScaleGuard: turned itself off (average probe cost %.3f ms exceeded 5.0 ms)", avgMs);
				}
			}
		}
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

	// UNOFFICIAL 2026-11-11 BeatSeries (BeatSeries.h): each frame's values of the world draw's series, next to the 64 frames of pictures.
	// SeriesText: for a beat of that period and phase over the frames first .. last, every series that has the same period and phase.
	struct SeriesRing { unsigned frame[FinalRing] = {}; unsigned value[FinalRing][BeatSeries::Count] = {}; } Sr;
	void SeriesEndFrame(unsigned frame, bool on) {
		BeatSeries::On = on;
		BeatSeries::Take(Sr.value[frame % FinalRing]);
		Sr.frame[frame % FinalRing] = on ? frame : 0;
	}
	std::string SeriesText(unsigned first, unsigned last, int period, int phase) {
		if (period < 2 || last < first) return "";
		enum { Max = 64 };
		unsigned values[Max]; bool valid[Max], beat[Max];
		int count = 0, have = 0;
		for (unsigned f = first; f <= last && count < Max; f++, count++) {
			valid[count] = f != 0 && Sr.frame[f % FinalRing] == f;
			beat[count] = (int)(f % (unsigned)period) == phase;
			have += valid[count] ? 1 : 0;
		}
		if (have < 3 * period) { char t[96]; sprintf_s(t, "matches: not judged (the series of only %d of these frames are kept)", have); return t; }
		std::string matched, rest;
		for (int id = 0; id < BeatSeries::Count; id++) {
			for (int i = 0; i < count; i++) values[i] = valid[i] ? Sr.value[(first + i) % FinalRing][id] : 0;
			const BeatSeries::Match m = BeatSeries::Test(values, valid, beat, count, BeatSeries::IsHash[id]);
			if (!m.match) { rest += (rest.empty() ? "" : ", ") + std::string(BeatSeries::Name[id]); continue; }
			char t[320];
			if (m.kind == 1 && BeatSeries::IsHash[id])
				_snprintf_s(t, _TRUNCATE, "%s (same period, phase; on the beat frames another value than %08X on all %d others)", BeatSeries::Name[id], m.offLo, m.offFrames);
			else if (m.kind == 1)
				_snprintf_s(t, _TRUNCATE, "%s (same period, phase; value on beat frames %u-%u vs %u-%u)", BeatSeries::Name[id], m.onLo, m.onHi, m.offLo, m.offHi);
			else
				_snprintf_s(t, _TRUNCATE, "%s (same period; it changes exactly on %s, and on no other; values %u-%u on beat frames, %u-%u on the others)",
					BeatSeries::Name[id], m.kind == 2 ? "the beat frames" : "the frames after the beat frames", m.onLo, m.onHi, m.offLo, m.offHi);
			matched += (matched.empty() ? "" : "; ") + std::string(t);
		}
		if (matched.empty()) return "matches: none of " + rest + " (" + std::to_string(have) + " frames compared)";
		return "matches: " + matched + " | not matching: " + (rest.empty() ? "none" : rest);
	}

	// The upscaler path, as the REPORT block prints it (the TAA state of the given frame; the rest as it is now)
	std::string UpscalingText(unsigned frame) {
		SettingManager* s = TheSettingManager;
		TAAEffect* taa = TheShaderManager->Effects.TAA;
		const char* names[] = { "FSR 3.1", "DLSS" };
		const int up = taa ? taa->FSRSettings.Upscaler : -1;
		const bool taaOn = taa && taa->Enabled;
		const bool fsrOn = taaOn && taa->FSRSettings.Enabled;
		std::string taaState = "not run";
		if (taa && taa->StatusSlot >= 0 && taa->StatusSlot < SlotCount) taaState = StateText(Slots[taa->StatusSlot], frame);
		char text[400];
		_snprintf_s(text, _TRUNCATE, "%s, RenderScale %.2f, DLSS Ray Reconstruction %s, SMAA %s | TAA that frame: %s | temporal resolve %s, history %s",
			!taaOn ? "none (TAA off)" : !fsrOn ? "TAA" : (up >= 0 && up <= 1 ? names[up] : "FSR / DLSS"), fsrOn ? taa->FSRSettings.RenderScale : 1.0f,
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
	std::string PeriodText(unsigned t, uint64_t mask, int* periodOut = nullptr, int* phaseOut = nullptr, unsigned* firstOut = nullptr) {
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
		if (periodOut) *periodOut = r.beat ? r.period : 0;	// UNOFFICIAL 2026-11-11 BeatSeries: a periodic flash is matched against the series
		if (phaseOut) *phaseOut = r.phase;
		if (firstOut) *firstOut = first;
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
		{	// UNOFFICIAL 2026-11-11 BeatSeries: which of the world draw's series beat with it
			const std::string series = SeriesText(t + 1 - BeatRule::Window, t, b.period, b.phase);
			if (!series.empty()) Logger::Log("BEAT %u %s", t, series.c_str());
		}
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
		int beatPeriod = 0, beatPhase = 0; unsigned beatFirst = 0;
		const std::string period = PeriodText(t, v.mask, &beatPeriod, &beatPhase, &beatFirst);
		// UNOFFICIAL 2026-11-11 BeatSeries: a flash that repeats: which of the world draw's series repeat with it (the frames up to t + 1)
		const std::string series = beatPeriod >= 2 ? SeriesText(beatFirst, t + 1, beatPeriod, beatPhase) : std::string();
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
			if (!series.empty()) Logger::Log("FLASH %u %s", t, series.c_str());
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
			if (!series.empty()) Logger::Log("FLASH %u %s", t, series.c_str());
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

	struct Options { bool flash = true; double lastRead = -10.0; } O;
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

long EffectStatus::CompileFailureCount() {
	return ShaderCounts[CompileFailed];
}

std::vector<std::string> EffectStatus::CompileFailureItems() {
	std::vector<std::string> out;
	std::lock_guard<std::mutex> guard(LevelLock);
	for (const LevelEntry& l : Levels) {
		if (l.isDefault) continue;
		if (l.chosen.find("FAILED") != std::string::npos || l.chosen.find("REFUSED") != std::string::npos || l.chosen.find("X4505") != std::string::npos)
			out.push_back(l.what + ": " + l.chosen);
	}
	return out;
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
	SessionLog::StageScope stage("flash detector / report (read-backs)");	// (2026-11-07 perf-ptcost: STALL lines)
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
		if (watch != Wt.on) {
			// 2026-11-14: off by default now, and the first frames run before this first read: their probes' surfaces are freed here too
			Logger::Log("HISTORY watch %s ([Main.Main.Misc] HistoryWatch)", watch ? "on" : "off");
			if (!watch) for (int k = 0; k < Wt.count; k++) WatchRelease(Wt.slots[k]);
		}
		Wt.onRead = true;
		Wt.on = watch;
		const bool rsGuard = TheSettingManager->GetSettingI("Main.Main.Misc", "RenderScaleGuard") != 0;	// UNOFFICIAL 2026-11-23 RenderScaleGuard
		if (RsGuard.onRead && rsGuard != RsGuard.on) {
			Logger::Log("UNOFFICIAL RenderScaleGuard %s ([Main.Main.Misc] RenderScaleGuard)", rsGuard ? "on" : "off");
			if (!rsGuard) { ReleaseRsGuard(); TAAEffect::ResetRenderScaleGuard(); }
		}
		RsGuard.onRead = true;
		RsGuard.on = rsGuard;
	}

	SeriesEndFrame(frame, (O.flash && !F.failed) || Bt.on);	// UNOFFICIAL 2026-11-11 BeatSeries: this frame's values kept, the counters back to 0
	if (device) StagesEndFrame(device, frame);	// UNOFFICIAL 2026-11-01: this frame's stage copies, the ones of 3 frames ago read (before the detector judges that frame)
	if (O.flash && !F.failed && device && picture) FlashFrame(device, picture, frame);
	if (RsGuard.on && !RsGuard.failed && device && picture) RenderScaleGuardFrame(device, picture, frame);
}

// UNOFFICIAL 2026-11-11: what the world is drawn into (the period-3 interior beat appears only with the game's own MSAA so far).
void Diagnostics::WorldTargetNote(IDirect3DDevice9* device) {
	static D3DSURFACE_DESC lastTarget = {}, lastDepth = {};
	static unsigned lines = 0, calls = 0;
	if (!device || lines >= 8 || (lines && (++calls & 255))) return;	// the first world scene, then a look every 256th
	IDirect3DSurface9* target = nullptr; IDirect3DSurface9* depth = nullptr; IDirect3DSurface9* back = nullptr;
	D3DSURFACE_DESC t = {}, z = {}, b = {};
	if (SUCCEEDED(device->GetRenderTarget(0, &target)) && target) target->GetDesc(&t);
	if (SUCCEEDED(device->GetDepthStencilSurface(&depth)) && depth) depth->GetDesc(&z);
	if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) && back) back->GetDesc(&b);
	const bool isBack = target && target == back;
	IDirect3DTexture9* owner = nullptr;	// a texture's level: the game can sample it; a plain surface has to be copied (resolved) first
	const bool isTexture = target && SUCCEEDED(target->GetContainer(IID_IDirect3DTexture9, (void**)&owner)) && owner;
	if (owner) owner->Release();
	const bool same = lines && t.Width == lastTarget.Width && t.Height == lastTarget.Height && t.Format == lastTarget.Format &&
		t.MultiSampleType == lastTarget.MultiSampleType && t.MultiSampleQuality == lastTarget.MultiSampleQuality && z.Width == lastDepth.Width &&
		z.Height == lastDepth.Height && z.Format == lastDepth.Format && z.MultiSampleType == lastDepth.MultiSampleType;
	if (!same) {
		lines++;
		lastTarget = t; lastDepth = z;
		Logger::Log("SESSION world target%s: the world scene is drawn into %s, %ux%u format %u, multisample %u quality %u (%s) | depth-stencil %ux%u format %u, multisample %u quality %u | "
			"back buffer %ux%u format %u, multisample %u quality %u",
			lines > 1 ? " (changed)" : "", !target ? "no render target" : isBack ? "the back buffer itself" : isTexture ? "a texture" : "a plain render-target surface (not a texture)",
			t.Width, t.Height, (unsigned)t.Format, (unsigned)t.MultiSampleType, (unsigned)t.MultiSampleQuality,
			t.MultiSampleType == D3DMULTISAMPLE_NONE ? "not multisampled: sampled as it is" :
			isBack ? "multisampled: resolved by a StretchRect out of the back buffer, or by Present" : "multisampled: it must be resolved by a StretchRect into a texture before anything reads it",
			z.Width, z.Height, (unsigned)z.Format, (unsigned)z.MultiSampleType, (unsigned)z.MultiSampleQuality,
			b.Width, b.Height, (unsigned)b.Format, (unsigned)b.MultiSampleType, (unsigned)b.MultiSampleQuality);
	}
	if (target) target->Release();
	if (depth) depth->Release();
	if (back) back->Release();
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
	// UNOFFICIAL 2026-11-23 RenderScaleGuard: probes, readback pressure, and fallback trips
	if (RsGuard.minuteProbes > 0 || RsGuard.tripsV1 > 0 || RsGuard.tripsNative > 0 || RsGuard.failed)
		Logger::Log("HEARTBEAT RenderScale guard: %s, %u probes (%u this session), %u late read-backs, trips v1=%u native=%u, cost %.3f ms a probe",
			RsGuard.failed ? "turned itself off" : (RsGuard.on ? "on" : "off"),
			RsGuard.minuteProbes, RsGuard.probes, RsGuard.minuteLate, RsGuard.tripsV1, RsGuard.tripsNative,
			RsGuard.minuteCostProbes ? RsGuard.minuteCost / RsGuard.minuteCostProbes * 1000.0 : 0.0);
	RsGuard.minuteProbes = RsGuard.minuteLate = 0;
	RsGuard.minuteCost = 0.0; RsGuard.minuteCostProbes = 0;
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
	SessionLog::StageScope stage("history watch (read-backs)");	// (2026-11-07 perf-ptcost: STALL lines)
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
