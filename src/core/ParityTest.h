#pragma once

// UNOFFICIAL, TEST ONLY (2026-11-14, full deferred renderer, docs/next/FULL-DEFERRED-RENDERER-PLAN.md section 6 "Parity"; the
// DEFERRED PARITY test DEFERRED-RENDERING-PLAN.md stage 3 specified): "does the picture with a live switch at B match the picture
// with it at A?", measured in ONE launch on a still view, without screenshots. Development builds only (NVR_TEST_TOOLS callers).
//
// Started two ways:
//   - a shot list line `parity <Section> <Key> [frames] [count] [A] [B]` (SettingShots.h), after the shot's own wait;
//   - the box [Main.Main.Misc] ParityTest (test only, default off): ticking it runs ParityCount measurements of
//     ParitySection / ParityKey (default [Shaders.ShadowsInteriors.Deferred] Live, DeferredLamps' A/B switch) with A = false,
//     B = true, ParityFrames frames a block. A box saved ticked does not start a run at launch.
// The switch is any setting by section and key (bool, int, float or string: set from text, SetSettingS), so the same test serves
// DeferredLamps Live today and the full renderer's own live switch later. A = B (e.g. `parity ... 64 3 false false`) is the
// forward-vs-forward control.
//
// One measurement = four blocks of `frames` frames: A1, A2, B1, B2. At the start of every block (also A1 -> A2 and B1 -> B2) the
// value is set and the settings are reloaded (LoadSettings), so each pair crosses the same kind of change. The LAST frame of each
// block is read at two points and compared with the block before:
//   AA = A1 vs A2 (repeat-noise floor of A), AB = A2 vs B1 (the difference measured), BB = B1 vs B2 (repeat-noise floor of B).
// All three pairs are one block apart, so a scene that drifts (time of day, a flickering lamp) shows in the floors as much as in AB.
// frames is rounded up to a multiple of 16, so every capture lands on the same phase of TAA's 8-step jitter (FSR / DLSS jitter with
// their own phase counts: their floor shows it).
//
// The two capture points (why there):
//   world - the game's scene target at the start of its image space (Render.cpp, where the bench's 'world' shots read it): the
//           world, glass, water and first person are drawn, LinearScene has encoded the frame back to its stored curve, and none of
//           Vegas Shaders' effects or the game's tonemapper has run. Every phase of the deferred renderer changes the picture before
//           this point (the deferred passes run inside the main list's span), and nothing after it with frame history (AL / GI
//           temporal filters, TAA, eye adaptation, bloom, the bodycam's grain and exposure) can bleed one block into the next or
//           re-expose a block. The main figure.
//   final - the finished picture before the HUD (where the shot pictures are read): what DEFERRED-RESULTS rounds 3-3e measured on
//           8-bit screenshots, kept so the numbers can be put next to that band. Includes eye adaptation (it settles within the block
//           only if the blocks are long enough) and every post effect.
//
// Figures (ParityCore.h): mean |A - B| in 8-bit levels, % of pixels with a channel more than 8 / 16 levels apart, the mean luma of
// each side, the same per region of a 3 x 3 grid. Log lines start "PARITY v1 " and are key=value words (tools/fd/parity_parse.py).
// The first measurement of a run also saves |A2 - B1| x 8 per point as a picture: VegasShaders-Logs\Parity_<run>_<label>_<point>_ab-x8.png.
//
// Cost while running: each block's last frame is read back whole at both points (GetRenderTargetData waits for the GPU to finish the
// frame so far): 8 stalls per measurement (4 blocks x 2 points), each logged (readms_max / readms_mean). Memory while running: per
// point one 16-bit picture (6 bytes a pixel) + a system-memory copy of the target (8 bytes a pixel for fp16, 4 for 8-bit): about
// 12 MB at 960 x 540 for both points together, about 50 MB at 1920 x 1080; all freed when the run ends. Off: one integer compare
// a frame (the settings revision, for the box).
#include <atomic>
#include <string>
#include "ParityCore.h"
#include "EffectSelfTestCore.h"

namespace ParityTest {

	enum Point { World, Final, Points };
	inline const char* const PointNames[Points] = { "world", "final" };
	enum { A1, A2, B1, B2, Blocks };
	inline const char* const PairNames[Blocks - 1] = { "AA", "AB", "BB" };	// pair k: block k (kept) against block k + 1

	struct Spec { std::string section, key, a = "false", b = "true", label = "menu"; int frames = 64, count = 3; };

	inline bool Running = false;
	inline Spec S;
	inline int Measure = 0, Block = 0, FrameInBlock = 0;
	inline bool CaptureNow = false;			// this frame is the last of its block: read at both points
	inline bool Captured[Points] = {};
	inline unsigned Missed[Points] = {};	// blocks whose last frame never reached a point
	inline char Run[32] = "";
	inline UInt32 SeenRevision = 0xFFFFFFFF;
	inline bool LastBox = false, BoxSeen = false;
	inline TESObjectCELL* Cell = nullptr;
	inline D3DXVECTOR4 CameraStart = {};
	inline bool CameraKnown = false;
	inline float CameraMoved = 0.0f;
	inline std::atomic<int> PicturesInFlight{ 0 };

	// the switch's own value before the run (put back at its end)
	struct Saved { UInt32 type = 0; bool b = false; int i = 0; float f = 0.0f; std::string s; };
	inline Saved Original;

	struct Capturer {
		IDirect3DSurface9* resolve = nullptr;	// multisampled target: resolved here first
		IDirect3DSurface9* readback = nullptr;	// system memory, full size
		D3DSURFACE_DESC desc = {};
		ParityCore::Picture kept;				// the previous block's capture
		std::vector<float> row;
		std::vector<unsigned> diff;				// |A2 - B1| x 8, the first measurement only
		bool failed = false, keptValid = false;
		double readMsMax = 0.0, readMsSum = 0.0;
		unsigned reads = 0;
		ParityCore::Result pair[Blocks - 1];
		bool have[Blocks - 1] = {};
		void ReleaseSurfaces() {
			if (resolve) { resolve->Release(); resolve = nullptr; }
			if (readback) { readback->Release(); readback = nullptr; }
			desc = {};
		}
		void Reset() {
			ReleaseSurfaces();
			kept.Free(); row.clear(); row.shrink_to_fit(); diff.clear(); diff.shrink_to_fit();
			failed = keptValid = false; readMsMax = readMsSum = 0.0; reads = 0;
			for (bool& h : have) h = false;
		}
	};
	inline Capturer Caps[Points];

	inline void Restore() {
		const char* section = S.section.c_str(); const char* key = S.key.c_str();
		switch (Original.type) {
		case SettingManager::Configuration::Boolean: TheSettingManager->SetSetting(section, key, Original.b); break;
		case SettingManager::Configuration::Integer: TheSettingManager->SetSetting(section, key, Original.i); break;
		case SettingManager::Configuration::String: TheSettingManager->SetSettingS(section, key, Original.s.substr(0, 79).c_str()); break;
		default: TheSettingManager->SetSetting(section, key, Original.f); break;
		}
		TheSettingManager->LoadSettings();
	}

	inline void End() {
		Running = false;
		CaptureNow = false;
		Restore();
		for (Capturer& c : Caps) c.Reset();
	}

	inline void Stop(const char* why) {
		if (!Running) return;
		Logger::Log("PARITY v1 run=%s shot=%s m=%d/%d kind=stopped | %s; the switch is back at its own value", Run, S.label.c_str(), Measure + 1, S.count, why);
		End();
	}

	// The block's value, set from text in the setting's own type, and the settings reloaded (as a shot's 'set' does).
	inline void ApplyBlock() {
		const std::string& value = Block < B1 ? S.a : S.b;
		TheSettingManager->SetSettingS(S.section.c_str(), S.key.c_str(), value.substr(0, 79).c_str());
		TheSettingManager->LoadSettings();
	}

	inline bool Start(const Spec& spec) {
		if (Running) Stop("a new measurement started");
		SYSTEMTIME t; GetLocalTime(&t);
		sprintf_s(Run, "%02u-%02u-%02u", (unsigned)t.wHour, (unsigned)t.wMinute, (unsigned)t.wSecond);
		SettingManager::Configuration::ConfigNode node;
		if (spec.section.empty() || spec.key.empty() || !TheSettingManager->Config.FillNode(&node, spec.section.c_str(), spec.key.c_str())) {
			Logger::Log("PARITY v1 run=%s shot=%s kind=refused | no setting [%s] %s", Run, spec.label.c_str(), spec.section.c_str(), spec.key.c_str());
			return false;
		}
		if (!Player || !Player->parentCell) { Logger::Log("PARITY v1 run=%s shot=%s kind=refused | no game loaded", Run, spec.label.c_str()); return false; }
		Original.type = node.Type; Original.b = node.BoolValue; Original.i = node.IntValue; Original.f = node.FloatValue;
		Original.s = node.LongValue.empty() ? node.Value : node.LongValue;
		S = spec;
		S.frames = (std::clamp(S.frames, 16, 1200) + 15) / 16 * 16;
		S.count = std::clamp(S.count, 1, 20);
		for (char& ch : S.label) if (ch == ' ' || ch == '=') ch = '_';
		Running = true; Measure = 0; Block = A1; FrameInBlock = 0; CaptureNow = false;
		for (int p = 0; p < Points; p++) { Caps[p].Reset(); Missed[p] = 0; }
		Cell = Player->parentCell;
		CameraKnown = false; CameraMoved = 0.0f;
		Logger::Log("PARITY v1 run=%s shot=%s kind=start switch=%s/%s A=%s B=%s was=%s frames=%d count=%d | blocks A1 A2 B1 B2 of %d frames, the last frame of each read; "
			"pairs AA (floor of A), AB (the difference), BB (floor of B)", Run, S.label.c_str(), S.section.c_str(), S.key.c_str(), S.a.c_str(), S.b.c_str(),
			Original.s.empty() ? "?" : Original.s.c_str(), S.frames, S.count, S.frames);
		ApplyBlock();
		return true;
	}

	inline void StartFromBox() {
		char section[80] = "", key[80] = "";
		TheSettingManager->GetSettingS("Main.Main.Misc", "ParitySection", section);
		TheSettingManager->GetSettingS("Main.Main.Misc", "ParityKey", key);
		Spec spec;
		spec.section = section; spec.key = key; spec.label = "menu";
		spec.frames = TheSettingManager->GetSettingI("Main.Main.Misc", "ParityFrames");
		spec.count = TheSettingManager->GetSettingI("Main.Main.Misc", "ParityCount");
		Start(spec);
	}

	inline void PairLine(int point, int m, int k, const ParityCore::Result& r) {
		char regions[3][200] = {};
		for (int i = 0; i < ParityCore::Regions; i++) {
			const ParityCore::Figures& f = r.region[i];
			const char* sep = i ? "," : "";
			char v[24];
			sprintf_s(v, "%s%.3f", sep, f.mean); strcat_s(regions[0], v);
			sprintf_s(v, "%s%.3f", sep, f.gt8); strcat_s(regions[1], v);
			sprintf_s(v, "%s%.3f", sep, f.gt16); strcat_s(regions[2], v);
		}
		Logger::Log("PARITY v1 run=%s shot=%s m=%d/%d point=%s pair=%s mean=%.4f gt8=%.4f gt16=%.4f lumaA=%.3f lumaB=%.3f r_mean=%s r_gt8=%s r_gt16=%s", Run,
			S.label.c_str(), m, S.count, PointNames[point], PairNames[k], r.all.mean, r.all.gt8, r.all.gt16, r.all.lumaA, r.all.lumaB, regions[0], regions[1], regions[2]);
	}

	// The end of a measurement: three pair lines and a summary per point.
	inline void Report() {
		const int m = Measure + 1;
		for (int p = 0; p < Points; p++) {
			Capturer& c = Caps[p];
			for (int k = 0; k < Blocks - 1; k++) if (c.have[k]) PairLine(p, m, k, c.pair[k]);
			if (!c.have[0] && !c.have[1] && !c.have[2]) {
				Logger::Log("PARITY v1 run=%s shot=%s m=%d/%d point=%s kind=summary valid=0 | not read (%s; %u block ends missed)", Run, S.label.c_str(), m, S.count,
					PointNames[p], c.failed ? "the read-back failed" : "no capture", Missed[p]);
				continue;
			}
			const bool all = c.have[0] && c.have[1] && c.have[2];
			const double floor16 = all ? (std::max)(c.pair[0].all.gt16, c.pair[2].all.gt16) : -1.0;
			const double floor8 = all ? (std::max)(c.pair[0].all.gt8, c.pair[2].all.gt8) : -1.0;
			Logger::Log("PARITY v1 run=%s shot=%s m=%d/%d point=%s kind=summary valid=%d switch=%s/%s A=%s B=%s size=%ux%u format=%s msaa=%u frames=%d "
				"readms_max=%.2f readms_mean=%.2f camera_moved=%.2f missed=%u ab_gt16=%.4f floor_gt16=%.4f ab_gt8=%.4f floor_gt8=%.4f",
				Run, S.label.c_str(), m, S.count, PointNames[p], all ? 1 : 0, S.section.c_str(), S.key.c_str(), S.a.c_str(), S.b.c_str(), c.desc.Width, c.desc.Height,
				SelfTestCore::FormatName(c.desc.Format), (unsigned)c.desc.MultiSampleType, S.frames, c.readMsMax, c.reads ? c.readMsSum / c.reads : 0.0,
				CameraMoved, Missed[p], c.have[1] ? c.pair[1].all.gt16 : -1.0, floor16, c.have[1] ? c.pair[1].all.gt8 : -1.0, floor8);
			for (bool& h : c.have) h = false;
		}
		for (unsigned& n : Missed) n = 0;	// per measurement
	}

	struct PictureJob { std::vector<unsigned> bgra; unsigned w = 0, h = 0; char path[MAX_PATH] = ""; };
	inline DWORD WINAPI PictureWorker(LPVOID parameter) {
		PictureJob* job = (PictureJob*)parameter;
		const bool saved = SelfTestCore::SavePng(job->path, job->w, job->h, job->bgra);
		Logger::Log("PARITY v1: %s %s", job->path, saved ? "saved" : "NOT saved");
		delete job;
		PicturesInFlight--;
		return 0;
	}

	inline void SaveDiff(int point, Capturer& c) {
		if (PicturesInFlight >= 2) return;
		PictureJob* job = new (std::nothrow) PictureJob();
		if (!job) return;
		job->bgra.swap(c.diff);
		job->w = c.kept.width; job->h = c.kept.height;
		sprintf_s(job->path, ".\\" VS_LOGS_FOLDER "\\Parity_%s_%s_%s_ab-x8.png", Run, S.label.c_str(), PointNames[point]);
		PicturesInFlight++;
		HANDLE thread = CreateThread(nullptr, 0, PictureWorker, job, 0, nullptr);
		if (thread) CloseHandle(thread);
		else { PicturesInFlight--; delete job; }
	}

	// The block is over (BeginFrame of the frame after its last one): the next block's value.
	inline void EndBlock() {
		CaptureNow = false;
		// a point that missed this block's last frame keeps an older block: the next comparison would cross two blocks under the
		// wrong pair name, so it starts again from the next capture (that pair is left out)
		for (int p = 0; p < Points; p++) if (!Captured[p]) { Missed[p]++; Caps[p].keptValid = false; }
		Block++;
		FrameInBlock = 0;
		if (Block == Blocks) {
			Report();
			Block = A1;
			if (++Measure >= S.count) {
				Logger::Log("PARITY v1 run=%s shot=%s kind=done measurements=%d | the switch is back at its own value", Run, S.label.c_str(), S.count);
				End();
				return;
			}
		}
		ApplyBlock();
	}

	// Render.cpp, the start of the game's image space, after SettingShots::BeginFrame (which may start a run for a shot).
	inline void BeginFrame() {
		if (TheSettingManager->SettingsRevision != SeenRevision) {	// the box: read only when a setting changed
			SeenRevision = TheSettingManager->SettingsRevision;
			const bool box = TheSettingManager->GetSettingI("Main.Main.Misc", "ParityTest") != 0;
			if (BoxSeen && box && !LastBox && !Running) StartFromBox();
			LastBox = box; BoxSeen = true;
		}
		if (!Running) return;
		if (CaptureNow) { EndBlock(); if (!Running) return; }
		if (!Player || Player->parentCell != Cell) { Stop("the player changed place"); return; }
		if (TheRenderManager->device->TestCooperativeLevel() != D3D_OK) { Stop("the device was lost"); return; }
		FrameInBlock++;
		CaptureNow = FrameInBlock >= S.frames;
		Captured[World] = Captured[Final] = false;
	}

	// Render.cpp: World (the game's scene target, right after BeginFrame) and Final (the finished picture before the HUD).
	inline void Capture(int point, IDirect3DSurface9* surface) {
		if (!Running || !CaptureNow || Captured[point] || !surface) return;
		Captured[point] = true;
		Capturer& c = Caps[point];
		if (c.failed) return;
		IDirect3DDevice9* device = TheRenderManager->device;
		LARGE_INTEGER t0, t1, hz;
		QueryPerformanceCounter(&t0);
		D3DSURFACE_DESC d = {};
		const char* error = nullptr;
		if (FAILED(surface->GetDesc(&d))) error = "no description";
		else if (!SelfTestCore::KnownFormat(d.Format)) error = "format not handled";
		else if (c.keptValid && (d.Width != c.kept.width || d.Height != c.kept.height)) { Stop("the screen size changed"); return; }
		if (!error && (!c.readback || d.Width != c.desc.Width || d.Height != c.desc.Height || d.Format != c.desc.Format || d.MultiSampleType != c.desc.MultiSampleType)) {
			c.ReleaseSurfaces();
			if (d.MultiSampleType != D3DMULTISAMPLE_NONE && FAILED(device->CreateRenderTarget(d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &c.resolve, NULL))) error = "no resolve target";
			else if (FAILED(device->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &c.readback, NULL))) error = "no read-back surface";
			else c.desc = d;
		}
		if (!error && !c.kept.Matches(d.Width, d.Height) && !c.kept.Make(d.Width, d.Height)) error = "no memory for the picture";
		IDirect3DSurface9* source = surface;
		if (!error && c.resolve) { if (FAILED(device->StretchRect(surface, NULL, c.resolve, NULL, D3DTEXF_NONE))) error = "resolve failed"; else source = c.resolve; }
		if (!error && FAILED(device->GetRenderTargetData(source, c.readback))) error = "GetRenderTargetData failed";
		D3DLOCKED_RECT lock = {};
		if (!error && FAILED(c.readback->LockRect(&lock, NULL, D3DLOCK_READONLY))) error = "lock failed";
		if (error) {
			c.failed = true;
			Logger::Log("PARITY v1 run=%s shot=%s point=%s kind=error | %s (format %u, %ux%u); this point is off for the run", Run, S.label.c_str(), PointNames[point], error,
				(unsigned)d.Format, d.Width, d.Height);
			c.ReleaseSurfaces();
			c.kept.Free();
			return;
		}
		const bool compare = Block != A1 && c.keptValid;
		const bool picture = compare && Block == B1 && Measure == 0;
		if (picture) c.diff.assign((size_t)d.Width * d.Height, 0xFF000000u);
		c.row.resize((size_t)d.Width * 4);
		ParityCore::Accum acc;
		acc.Begin(d.Width, d.Height);
		for (UInt32 y = 0; y < d.Height; y++) {
			SelfTestCore::ConvertRow(d.Format, (const unsigned char*)lock.pBits + (size_t)y * lock.Pitch, d.Width, c.row.data());
			ParityCore::CompareRow(acc, c.kept, y, c.row.data(), compare, picture ? &c.diff[(size_t)y * d.Width] : nullptr);
		}
		c.readback->UnlockRect();
		c.keptValid = true;
		if (compare) { c.pair[Block - 1] = ParityCore::Finish(acc); c.have[Block - 1] = true; }
		if (picture) SaveDiff(point, c);
		QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&hz);
		const double ms = 1000.0 * (double)(t1.QuadPart - t0.QuadPart) / (double)hz.QuadPart;
		c.readMsMax = (std::max)(c.readMsMax, ms); c.readMsSum += ms; c.reads++;
		if (point == World) {	// a still view: how far the camera went during the run
			const D3DXVECTOR4& cam = TheRenderManager->CameraPosition;
			if (!CameraKnown) { CameraStart = cam; CameraKnown = true; }
			const float dx = cam.x - CameraStart.x, dy = cam.y - CameraStart.y, dz = cam.z - CameraStart.z;
			CameraMoved = (std::max)(CameraMoved, sqrtf(dx * dx + dy * dy + dz * dz));
		}
	}
}
