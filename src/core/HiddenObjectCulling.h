#pragma once

// UNOFFICIAL hidden object culling ([Main.Main.Performance] HiddenObjectCulling, off by default, 2026-10-11).
// Ported from FNVProbe's [Fixes] HiddenObjectCulling version 2 (work\FNVProbe\src\HiddenDraws.inl, notes in
// HIDDEN-OBJECT-CULLING.md), now inside NVR's own RenderPassStandard/Skinned hooks (Render.cpp).
//
// Indoors the game draws most objects several times: the first pass writes depth and the base lighting, the later ones
// add more lamps, specular and similar (additive, never write depth; measured 2026-10-07). In the Rivet City hangar ~42% of
// objects drew zero pixels, yet all their later passes were still drawn (~1,200 passes, ~0.9 ms CPU per frame).
// In the world scene, an object's FIRST pass of the frame is always drawn. If the object had later passes the last time it
// was drawn, that first pass is wrapped in an occlusion query; its LATER passes are skipped when the newest answer that is
// in (GetData without flush, at most MaxAge frames old) says a depth-writing first pass drew 0 pixels. No answer = visible.
// Camera guard: answers from before a turn of more than 2.5 degrees or a move of more than 32 units per frame of age are not
// used. Safe to skip here: BSBatchRenderer::RenderPassImmediately 0x00B994F0 binds the pass's shaders before it calls
// _Standard/_Skinned, which only set up and draw their own object; the first pass is never skipped, so
// BSBatchRenderer::bFirstPass stays balanced. Only the first world scene of a frame is culled (a second one, e.g. behind a
// menu, could otherwise see its depth passes skipped as "later" passes).
// Red blot fix (2026-10-11): an answer is a few frames old, so an object coming into view round a corner can still be
// judged hidden for those frames. Skipping ALL its later passes then also dropped the game's texture pass, which multiplies
// the light already drawn by the object's texture: the object showed as a flat patch of raw (often red/orange) lamp
// light. Now only NVR-recognised lamp-only passes are skipped (ObjectTemplate DIFFUSE SLS2045/46 and POINT specular
// SLS2051-56, additive), so a wrong guess can only leave an object missing some lamp light for a few frames. Also, the
// answers are keyed by the NiGeometry pointer: when a cell loads, a new object can be created at a freed object's address
// and inherit its "hidden" answers. Each state now keeps the object's parent, world position and scale, and forgets its
// answers when they differ (this also stops culling objects while they move).
#include "BeatSeries.h"	// UNOFFICIAL 2026-11-11 the beat detector's per-frame series
namespace HiddenObjectCulling {

	enum { Slots = 8 };		// answers kept per object (frames F .. F-7); usable ages 1 .. Slots-1
	struct State {
		IDirect3DQuery9* query[Slots]; UInt32 issuedFrame[Slots]; bool writesDepth[Slots];
		UInt32 frame, lastSeen;
		UInt16 passes, passesLast;	// passes drawn this frame / in the last frame it was drawn
		bool hidden;
		const void* parent; float pos[3], scale;	// identity check (NiAVObject +0x18, +0x8C, +0x98)
	};

	inline bool Enabled = false;		// the setting, read each frame
	inline bool BoundLamp = false;		// UNOFFICIAL 2026-11-11 BeatSeries: the shader bound last is a lamp-only one (whether or not culling is on)
	inline bool LampPass = false;		// the pass about to be drawn is a lamp-only pass (OnBind; used once by Before)
	inline bool Active = false;			// culling the current world scene
	inline bool WorldSeen = false;		// a world scene was already drawn this frame
	inline int MaxAge = 4;
	inline UInt32 FrameNo = 0;
	inline DWORD Thread = 0;
	inline IDirect3DDevice9* Device = nullptr;
	inline std::unordered_map<const void*, State> States;
	inline State* Pending = nullptr;	// the object whose first pass is being drawn inside a query
	inline int PendingSlot = 0;

	inline const double kMaxDegreesPerFrame = 2.5, kMaxUnitsPerFrame = 32.0;
	inline float CamRot[Slots][9] = {}, CamPos[Slots][3] = {};
	inline bool CamHave[Slots] = {};
	inline bool Stale[Slots] = {};		// [age]: answers that old are not used this frame

	inline unsigned StatFrames = 0;
	inline double StatObjects = 0, StatMulti = 0, StatHidden = 0, StatSkipped = 0, StatDrawnLater = 0, StatNotReady = 0,
		StatStaleDropped = 0, StatQueryFails = 0, StatQueries = 0, StatStaleFrames = 0, StatKept = 0, StatReused = 0;
	inline double StatAge[Slots] = {};

	// SetShadersHook, after the game bound the pass's shaders (before _Standard/_Skinned): lamp-only passes are the only
	// ones a hidden object may skip. Same pixel shaders as LampScissor::OnBind, matched by name whether or not NVR replaced
	// them (both are additive lamp passes).
	inline void OnBind(const NiD3DPixelShaderEx* shader) {
		LampPass = false;
		BoundLamp = false;
		if (!shader || !shader->Name) return;
		const char* name = shader->Name;
		if (strncmp(name, "SLS20", 5) || strlen(name) != 11 || strcmp(name + 7, ".pso")) return;
		const int number = (name[5] - '0') * 10 + (name[6] - '0');
		BoundLamp = number == 45 || number == 46 || (number >= 51 && number <= 56);
		LampPass = Active && BoundLamp;	// (as before: only while culling the world scene)
	}

	inline void ReleaseAll() {
		for (auto& entry : States) for (IDirect3DQuery9* q : entry.second.query) if (q) q->Release();
		States.clear();
		Pending = nullptr;
	}

	// TESMain::GetWorldRootCamera 0x00524C90 (cdecl) -> NiCamera; world rotation (3x3) at +0x68, world position at +0x8C.
	inline bool ReadCamera(float* rot, float* pos) {
		__try {
			typedef UInt8* (__cdecl* GetCameraFn)();
			UInt8* camera = ((GetCameraFn)0x00524C90)();
			if (!camera) return false;
			memcpy(rot, camera + 0x68, sizeof(float) * 9);
			memcpy(pos, camera + 0x8C, sizeof(float) * 3);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	inline void UpdateCameraGuard() {
		const int cur = FrameNo % Slots;
		CamHave[cur] = ReadCamera(CamRot[cur], CamPos[cur]);
		Stale[0] = false;
		for (UInt32 age = 1; age < Slots; age++) {
			const int old = (FrameNo - age) % Slots;
			bool stale = !CamHave[cur] || !CamHave[old];
			if (!stale) {
				double dot = 0, dist = 0;	// trace(Ra^T Rb) = sum of products; angle = acos((trace - 1) / 2)
				for (int i = 0; i < 9; i++) dot += (double)CamRot[cur][i] * CamRot[old][i];
				for (int i = 0; i < 3; i++) { const double d = CamPos[cur][i] - CamPos[old][i]; dist += d * d; }
				const double cosAngle = (std::max)(-1.0, (std::min)(1.0, (dot - 1.0) / 2.0));
				const double degrees = acos(cosAngle) * 57.29577951308232;
				stale = degrees > kMaxDegreesPerFrame * age || sqrt(dist) > kMaxUnitsPerFrame * age;
			}
			Stale[age] = stale;
		}
		if (Stale[1] && Stale[2]) StatStaleFrames += 1;
	}

	// Each frame (RenderHook, before the game renders): the setting, the camera guard, housekeeping and the F10 line.
	inline void BeginFrame(IDirect3DDevice9* device) {
		Thread = GetCurrentThreadId();
		Device = device;
		WorldSeen = false;
		Active = false;
		LampPass = false;
		Pending = nullptr;
		const bool enabled = TheSettingManager->SettingsMain.Main.HiddenObjectCulling;
		MaxAge = TheSettingManager->SettingsMain.Main.HiddenObjectCullingMaxAge;
		if (!enabled && Enabled) ReleaseAll();
		if (enabled && !Enabled) Logger::Log("UNOFFICIAL HiddenObjectCulling on: extra passes of objects hidden a few frames ago are skipped.");
		Enabled = enabled;
		FrameNo++;
		if (FrameNo == 0) FrameNo = 1;	// 0 means "never drawn" in State::frame
		if (Enabled) {
			UpdateCameraGuard();
			if ((FrameNo & 1023) == 0) {	// forget objects not seen for 1,200 frames
				for (auto it = States.begin(); it != States.end();) {
					if (FrameNo - it->second.lastSeen > 1200) {
						for (IDirect3DQuery9* q : it->second.query) if (q) q->Release();
						it = States.erase(it);
					}
					else ++it;
				}
			}
		}
		if (GpuTimer::Enabled && Enabled && ++StatFrames >= 240) {
			const double f = StatFrames;
			char ages[160] = {};
			size_t used = 0;
			for (int age = 1; age < Slots; age++) {
				const int w = _snprintf_s(ages + used, sizeof(ages) - used, _TRUNCATE, "%s%d: %.0f", age > 1 ? ", " : "", age, StatAge[age] / f);
				if (w < 0) break;
				used += w;
			}
			Logger::Log("HIDDEN OBJECT CULLING: objects %.0f/frame, with later passes (queried) %.0f, hidden %.0f (%.0f%% of queried), lamp passes skipped %.0f/frame, later passes drawn %.0f/frame (of hidden objects, not lamp passes: %.0f) | answers used by age in frames (max %d) %s | none in yet %.0f, dropped (camera moved) %.0f /frame | frames with camera moving fast %.0f%% | new object at a reused address %.0f /frame | objects tracked %u%s",
				StatObjects / f, StatMulti / f, StatHidden / f, StatMulti ? 100.0 * StatHidden / StatMulti : 0.0, StatSkipped / f, StatDrawnLater / f, StatKept / f,
				MaxAge, ages, StatNotReady / f, StatStaleDropped / f, 100.0 * StatStaleFrames / f, StatReused / f, (unsigned)States.size(),
				StatQueryFails ? " | query creation failed" : "");
			StatFrames = 0;
			StatObjects = StatMulti = StatHidden = StatSkipped = StatDrawnLater = StatNotReady = StatStaleDropped = StatQueryFails = StatQueries = StatStaleFrames = StatKept = StatReused = 0;
			for (double& a : StatAge) a = 0;
		}
	}

	// RenderWorldSceneGraphHook: the world scene starts / ends. Only the frame's first one is culled.
	inline void BeginWorld() {
		Active = Enabled && !WorldSeen && Device && GetCurrentThreadId() == Thread;
		WorldSeen = true;
	}
	inline void EndWorld() { Active = false; LampPass = false; Pending = nullptr; }

	// Before an object's pass in the world scene. True: skip the pass (a later pass of a hidden object). Otherwise the
	// draw goes ahead; when it has to be measured, an occlusion query has begun and After() ends it.
	inline bool Before(const void* pass, bool world) {
		Pending = nullptr;
		const bool lampPass = LampPass;
		LampPass = false;	// one bind, one pass: a pass whose bind we did not see counts as not a lamp pass
		if (!Active || !world || !pass) return false;
		const UInt8* geometry = *(const UInt8* const*)pass;
		if (!geometry) return false;
		State& s = States[geometry];
		s.lastSeen = FrameNo;
		if (s.frame == FrameNo) {
			if (s.passes < 0xFFFF) s.passes++;
			if (s.hidden && lampPass) { BeatSeries::Add(BeatSeries::LampPassesHidden); if (GpuTimer::Enabled) StatSkipped += 1; return true; }
			if (GpuTimer::Enabled) { StatDrawnLater += 1; if (s.hidden) StatKept += 1; }
			return false;
		}
		// First pass of this object this frame. Objects that had no later passes last time are drawn without a query
		// (nothing to skip). Otherwise the newest answer that is in decides, and this pass is drawn inside a new query.
		// A different parent, position or scale than last time = another object at a reused address (or one that moved):
		// its old answers are forgotten.
		const void* parent = *(const void* const*)(geometry + 0x18);
		const float* pos = (const float*)(geometry + 0x8C);
		const float scale = *(const float*)(geometry + 0x98);
		bool firstTime = s.frame == 0;
		if (!firstTime && (s.parent != parent || s.pos[0] != pos[0] || s.pos[1] != pos[1] || s.pos[2] != pos[2] || s.scale != scale)) {
			for (UInt32& issued : s.issuedFrame) issued = 0;
			firstTime = true;
			if (GpuTimer::Enabled) StatReused += 1;
		}
		s.parent = parent;
		s.pos[0] = pos[0]; s.pos[1] = pos[1]; s.pos[2] = pos[2];
		s.scale = scale;
		if (!firstTime) s.passesLast = s.passes;
		s.frame = FrameNo;
		s.passes = 1;
		s.hidden = false;
		if (GpuTimer::Enabled) StatObjects += 1;
		if (!firstTime && s.passesLast <= 1) return false;
		if (GpuTimer::Enabled) StatMulti += 1;
		const int cur = FrameNo % Slots;
		bool answered = false;
		for (UInt32 age = 1; age <= (UInt32)MaxAge && age < Slots && !answered; age++) {
			const int slot = (FrameNo - age) % Slots;
			if (!s.query[slot] || s.issuedFrame[slot] != FrameNo - age) continue;	// not queried that frame; an older one may be
			if (Stale[age]) { if (GpuTimer::Enabled) StatStaleDropped += 1; break; }	// camera moved too much since (older is worse)
			DWORD pixels = 1;
			if (s.query[slot]->GetData(&pixels, sizeof(pixels), 0) != S_OK) continue;
			answered = true;
			s.hidden = s.writesDepth[slot] && pixels == 0;
			if (GpuTimer::Enabled) StatAge[age] += 1;
		}
		if (GpuTimer::Enabled) {
			if (!answered) StatNotReady += 1;
			if (s.hidden) StatHidden += 1;
		}
		if (!s.query[cur] && FAILED(Device->CreateQuery(D3DQUERYTYPE_OCCLUSION, &s.query[cur]))) { s.query[cur] = nullptr; StatQueryFails += 1; }
		if (!s.query[cur]) return false;
		s.query[cur]->Issue(D3DISSUE_BEGIN);
		Pending = &s;
		PendingSlot = cur;
		return false;
	}

	// After the draw: end the query and note whether that pass wrote depth (only then does "0 pixels" mean hidden; a first
	// pass that is a pure light pass can draw 0 pixels because its lamp misses the visible part).
	inline void After() {
		State* s = Pending;
		if (!s) return;
		Pending = nullptr;
		s->query[PendingSlot]->Issue(D3DISSUE_END);
		s->issuedFrame[PendingSlot] = FrameNo;
		if (GpuTimer::Enabled) StatQueries += 1;
		DWORD zwrite = 0;
		Device->GetRenderState(D3DRS_ZWRITEENABLE, &zwrite);	// the pass's state is still on the device
		s->writesDepth[PendingSlot] = zwrite != 0;
	}

	// The first pass was not drawn after all (LampScissor dropped it): end the query and never use its answer.
	inline void Abandon() {
		State* s = Pending;
		if (!s) return;
		Pending = nullptr;
		s->query[PendingSlot]->Issue(D3DISSUE_END);
		s->issuedFrame[PendingSlot] = 0;
	}
}
