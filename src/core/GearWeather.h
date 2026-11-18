#pragma once
// UNOFFICIAL 2026-11-15 GearWeather ([Main.Main.Quality] GearWeather, live, test only, off; needs DeferredFirstPerson): weather on the
// player's gear in first person (docs/next/FD-FIRSTPERSON-RESULTS.md part B; Shaders/GearWeather.hlsl).
//
// Four layers, each an amount 0-1 that builds up while its weather lasts and fades when it is over (Step, once a frame, real seconds):
//   wet   - rain falling on the player outdoors (RainState: the rain amount, after RainState::Tick), or the player in water. Up in about
//           20 s of full rain; dries in about 3 min outdoors, 90 s indoors.
//   frost - a snow weather outdoors (GameState.isSnow, ForceSnow included). Up in about 60 s; melts in about 45 s indoors, 4 min outdoors
//           without snow, 30 s in rain.
//   sand  - a sandstorm (the Sandstorm effect's eased intensity) outdoors, and slowly in a strong dry wind (the weather's wind speed
//           past half, no rain): desert dust. Up in about 30 s of a full storm; fades over 5 min outdoors (blown off), 10 min indoors;
//           rain washes it off in about 40 s.
//   mud   - CHOSEN TRIGGER: moving over wet ground outdoors. The ground is wet while it rains or puddles stand (RainState's factor:
//           max(rain, puddles)), and mud is thrown up only while the player moves (walking 0.5, running 1: the player's speed / 250 units
//           a second). Up in about 45 s of running through the wet; dries and flakes off over 10 min (6 min indoors). Standing in the
//           rain does not make mud; walking a dry road after the rain still does while the puddles last.
// Water (the camera under water) washes sand, mud and frost off in about 5 s and soaks the gear.
// All rates x [Main.Main.Quality] GearWeatherSpeed; what the pass gets is the amount x GearWeatherStrength x the layer's own strength
// (GearWeatherWet / Frost / Sand / Mud, advanced keys). GearWeatherForce (test only, live) holds the amounts for the bench: 0 auto,
// 1 wet, 2 frost, 3 sand, 4 mud (that layer 1, the others 0), 5 all four at 0.6. GearWeatherDebug 1 paints the covers as colours.
//
// WHY THE PATTERN STICKS (the choice): a gear-local coordinate per pixel is rebuilt in the pass from the draw that drew the pixel, not
// stored in the G-buffer (no target is free: ps_3_0's four are the v2 layout's, and the object shaders have no constant or define room
// for a fifth output's code). Each first-person object draw that goes through the object draw hooks (Render.cpp MaterialDraw) writes its
// own id (1-63) into the stencil's low six bits where it passes the depth test (DrawBegin; the id survives as an id buffer does: the
// last draw to pass at a pixel owns it). After the first-person render the pass runs once per id, over the draw's own screen box, with
// the stencil test EQUAL that id (+ DeferredFirstPerson's view-model mark 0x40), and the draw's transform in its constants: a still part
// (the gun's body, magazine, scope, the Pip-Boy) its world transform; a skinned part (hands, gloves, sleeves) its bones (up to 16 with
// vertices, each bone's bound and transform), the pixel taking its two nearest bones' local positions blended. The pixel's position
// comes from the G-buffer's camera distance along the first-person camera's ray, so the noise is read where the gear's own surface is,
// and moves with the part through a reload or a walk. The bit 0x80 stays WeaponMask's; ids past 63 share id 63's transform.
//
// Cost (designed for <= 0.15 ms at 1080p with the gun on a third of the screen): two copies of the G-buffer's view-model box (albedo and
// material, fp16), one pass over the view model's pixels (two noise reads, four layers; a skinned part's bone loop). Nothing at all runs
// while every amount is 0 (no copy, no pass): dry weather indoors costs nothing.
#include "SkinBones.h"	// 2026-11-18: a skin's bone slots may be stale
#include "CapturedNormals.h"
#include "ShaderRecord.h"
#include "RainState.h"
#include "../effects/Sandstorm/Sandstorm.h"
#include <algorithm>

namespace GearWeather {
	inline const char* kSection = "Main.Main.Quality";
	inline constexpr DWORD kIdMask = 0x3F, kMark = 0x40;
	inline constexpr int kMaxIds = 63, kMaxBones = 16;

	// ---- the amounts (pure: tests/fd_firstperson_math.py mirrors Step line for line)
	struct Levels { float wet = 0.0f, frost = 0.0f, sand = 0.0f, mud = 0.0f; };
	struct Inputs {
		float rain = 0.0f;		// falling now, 0-1
		float ground = 0.0f;	// the ground's wetness, 0-1 (max(rain, puddles))
		float snow = 0.0f;		// 1 in a snow weather
		float storm = 0.0f;		// the sandstorm's intensity, 0-1
		float wind = 0.0f;		// the weather's wind, 0-1
		float moving = 0.0f;	// the player's speed share, 0-1
		bool exterior = false, underwater = false;
	};
	inline float Clamp01(float v) { return v > 0.0f ? (v < 1.0f ? v : 1.0f) : 0.0f; }
	// One step of dt seconds (speed: GearWeatherSpeed). Each layer moves up at its build rate while its weather acts and down at its fade
	// rate otherwise (both per second, for an amount 0-1).
	inline Levels Step(Levels l, const Inputs& in, float dt, float speed) {
		const float t = (dt > 0.0f ? (dt < 0.5f ? dt : 0.5f) : 0.0f) * (speed > 0.0f ? speed : 0.0f);	// (a long hitch counts as half a second)
		const bool out = in.exterior && !in.underwater;
		const float rain = out ? Clamp01(in.rain) : 0.0f;
		// wet
		if (in.underwater) l.wet += t * 2.0f;
		else if (rain > 0.01f) l.wet += t * rain / 20.0f;
		else l.wet -= t * (out ? 1.0f / 180.0f : 1.0f / 90.0f);
		// frost
		const float snow = out ? Clamp01(in.snow) : 0.0f;
		if (snow > 0.01f) l.frost += t * snow / 60.0f;
		else l.frost -= t * (in.underwater ? 1.0f / 5.0f : !in.exterior ? 1.0f / 45.0f : rain > 0.01f ? 1.0f / 30.0f : 1.0f / 240.0f);
		// sand / dust
		const float storm = out ? Clamp01(in.storm) : 0.0f;
		const float dust = out && rain <= 0.01f ? Clamp01((in.wind - 0.5f) * 2.0f) : 0.0f;
		if (storm > 0.01f || dust > 0.01f) l.sand += t * (storm / 30.0f + dust / 180.0f);
		else l.sand -= t * (in.underwater ? 1.0f / 5.0f : rain > 0.01f ? 1.0f / 40.0f : !in.exterior ? 1.0f / 600.0f : 1.0f / 300.0f);
		// mud
		const float splash = out ? Clamp01(in.ground) * Clamp01(in.moving) : 0.0f;
		if (splash > 0.01f) l.mud += t * splash / 45.0f;
		else l.mud -= t * (in.underwater ? 1.0f / 5.0f : !in.exterior ? 1.0f / 360.0f : 1.0f / 600.0f);
		l.wet = Clamp01(l.wet); l.frost = Clamp01(l.frost); l.sand = Clamp01(l.sand); l.mud = Clamp01(l.mud);
		return l;
	}

	inline int Wanted = -1;
	inline bool WantedAtStartup() {	// compiled in with DeferredFirstPerson at startup (read once): GearWeather itself is then live
		if (Wanted < 0) Wanted = TheSettingManager->GetSettingI(kSection, "DeferredFirstPerson") != 0 ? 1 : 0;
		return Wanted != 0;
	}
	inline ShaderRecordPixel* Pass = nullptr;
	inline bool PassFailed = false;
	inline Levels Now;					// the amounts (auto)
	inline float Sent[4] = {};			// what the pass gets this frame (x strengths, or forced)
	inline bool Live = false;			// this frame: the switch on and something to draw
	inline bool Tagging = false;		// the first-person render is under way and its draws take ids
	inline UInt32 LastFrame = 0;
	inline double LastTime = -1.0;
	inline D3DXVECTOR3 LastPlayer(0.0f, 0.0f, 0.0f);
	inline bool LastPlayerKnown = false;
	inline float Moving = 0.0f;

	// (2026-11-16 fd-fwdgroups) the part's transforms as DRAWN: batch 8 tagged parts (passes 0.63-1.00 a frame, nothing skipped) but drew none
	// ("parts 0.0, box pixels 0" in every window): every part's bound read at DeferredFirstPerson::End, after the first-person render, missed
	// the 150-unit test round the first-person eye. Believed: the view model's transforms no longer hold the pose the render drew (not proven
	// in game; the F10 line's "nearest part" now says how far they were). Taken at the part's first draw instead, used for its box and noise rows.
	struct Snap { NiTransform T; NiBound bound; float radius; };
	struct Part { NiGeometry* geometry; UInt32 seed; bool weapon; int bones; Snap still; Snap bone[kMaxBones]; };
	inline Part Parts[kMaxIds];
	inline int PartCount = 0;
	inline unsigned long long StatFrames = 0, StatPasses = 0, StatParts = 0, StatPixelsBox = 0, StatUntagged = 0, StatSkipped = 0;
	inline IDirect3DTexture9* CopyTex[2] = {};
	inline IDirect3DSurface9* Copy[2] = {};
	inline D3DSURFACE_DESC CopyMade = {};
	inline NiNode* WeaponNode = nullptr;

	inline bool Loaded() { return Pass != nullptr; }

	// ShaderManager::Initialize (with DeferredFirstPerson::Load): the pass, compiled now.
	inline void Load() {
		if (!WantedAtStartup() || Pass || PassFailed) return;
		ShaderTemplate tmpl = {};
		tmpl.Name = "GearWeather";
		static D3DXMACRO overrides[1] = { { NULL, NULL } };
		Pass = (ShaderRecordPixel*)ShaderRecord::LoadShader("GearWeather.pso", NULL, tmpl, "Deferred\\FirstPerson\\", overrides);
		if (Pass) { Pass->ClearSamplers = false; Logger::Log("UNOFFICIAL GearWeather: the pass loaded (Shaders\\GearWeather.hlsl)"); }
		else { PassFailed = true; Logger::Log("[ERROR] UNOFFICIAL GearWeather: the pass (Shaders\\GearWeather.hlsl) did not load: no weather on the gear"); }
	}

	// Once a frame (DeferredFirstPerson::BeginFrame): the amounts from the weather, what the pass gets.
	inline void BeginFrame(bool fpLive) {
		Live = false;
		if (!WantedAtStartup() || !Pass || !TheShaderManager || !TheFrameRateManager) return;
		const double now = TheFrameRateManager->Time;
		const float dt = LastTime < 0.0 ? 0.0f : (float)(now - LastTime);
		LastTime = now;
		const auto& gs = TheShaderManager->GameState;
		Inputs in;
		in.exterior = gs.isExterior;
		in.underwater = gs.isUnderwater;
		const float rainFactor = RainState::Factor();	// (Tick inside: this frame's amounts)
		const WetWorldEffect* wet = TheShaderManager->Effects.WetWorld;
		in.rain = wet ? wet->Constants.Data.x : (gs.isRainy ? 1.0f : 0.0f);
		in.ground = rainFactor;
		in.snow = gs.isSnow ? 1.0f : 0.0f;
		SandstormEffect* storm = TheShaderManager->Effects.Sandstorm;
		in.storm = storm && storm->Enabled ? storm->Intensity : 0.0f;
		in.wind = Clamp01(TheShaderManager->ShaderConst.windSpeed);
		if (Player && dt > 0.0f) {
			const D3DXVECTOR3 p(Player->pos.x, Player->pos.y, Player->pos.z);
			if (LastPlayerKnown) {
				const D3DXVECTOR3 v = p - LastPlayer;
				const float speed = D3DXVec3Length(&v) / dt;
				const float target = speed > 1000.0f ? 0.0f : Clamp01(speed / 250.0f);	// (a jump: a door, fast travel)
				Moving += (target - Moving) * Clamp01(dt * 4.0f);
			}
			LastPlayer = p; LastPlayerKnown = true;
		}
		in.moving = Moving;
		Now = Step(Now, in, dt, TheSettingManager->GetSettingF(kSection, "GearWeatherSpeed"));
		const int force = TheSettingManager->GetSettingI(kSection, "GearWeatherForce");
		Levels l = Now;
		if (force >= 1 && force <= 4) { l = Levels(); (force == 1 ? l.wet : force == 2 ? l.frost : force == 3 ? l.sand : l.mud) = 1.0f; }
		else if (force == 5) { l.wet = l.frost = l.sand = l.mud = 0.6f; }
		const float all = (std::max)(TheSettingManager->GetSettingF(kSection, "GearWeatherStrength"), 0.0f);
		Sent[0] = Clamp01(l.wet * all * TheSettingManager->GetSettingF(kSection, "GearWeatherWet"));
		Sent[1] = Clamp01(l.frost * all * TheSettingManager->GetSettingF(kSection, "GearWeatherFrost"));
		Sent[2] = Clamp01(l.sand * all * TheSettingManager->GetSettingF(kSection, "GearWeatherSand"));
		Sent[3] = Clamp01(l.mud * all * TheSettingManager->GetSettingF(kSection, "GearWeatherMud"));
		const bool debug = TheSettingManager->GetSettingI(kSection, "GearWeatherDebug") != 0;
		Live = fpLive && TheSettingManager->GetSettingI(kSection, "GearWeather") != 0 && (debug || Sent[0] + Sent[1] + Sent[2] + Sent[3] > 0.001f);
		if (!GpuTimer::Enabled) { StatFrames = 0; return; }
		if (++StatFrames < 240) return;
		const double f = (double)StatFrames;
		Logger::Log("GEAR WEATHER %u frames: amounts wet %.2f frost %.2f sand %.2f mud %.2f (sent %.2f %.2f %.2f %.2f%s) | inputs rain %.2f ground %.2f snow %.0f storm %.2f wind %.2f moving %.2f %s | "
			"passes %.2f a frame, parts %.1f, box pixels %.0f, draws without an id (their own stencil) %.2f, skipped %.2f",
			(unsigned)StatFrames, Now.wet, Now.frost, Now.sand, Now.mud, Sent[0], Sent[1], Sent[2], Sent[3], force ? ", FORCED" : "", in.rain, in.ground, in.snow, in.storm, in.wind, in.moving,
			in.underwater ? "underwater" : in.exterior ? "outdoors" : "indoors", StatPasses / f, StatParts / f, StatPixelsBox / f, StatUntagged / f, StatSkipped / f);
		StatFrames = 0; StatPasses = StatParts = StatPixelsBox = StatUntagged = StatSkipped = 0;
	}

	// ---- the ids (Render.cpp MaterialDraw, round every first-person object draw, after WeaponMask::DrawBegin)
	struct DrawState { bool tagged = false, alone = false; DWORD func = 0, pass = 0, fail = 0, zfail = 0, ref = 0, write = 0; };
	inline UInt32 NameSeed(const char* name) {	// a stable seed per part: the same blotches on the same gun after a reload or a restart
		UInt32 h = 2166136261u;
		for (const char* c = name ? name : ""; *c; c++) { h ^= (UInt8)*c; h *= 16777619u; }
		return h;
	}
	inline int IdOf(NiGeometry* geometry) {
		for (int i = 0; i < PartCount; i++) if (Parts[i].geometry == geometry) return i + 1;
		if (PartCount >= kMaxIds) return kMaxIds;
		bool weapon = false;
		for (NiAVObject* n = geometry; WeaponNode && n && !weapon; n = n->m_parent) weapon = n == WeaponNode;
		Part& p = Parts[PartCount];
		p.geometry = geometry; p.seed = NameSeed(geometry->m_pcName); p.weapon = weapon; p.bones = 0;
		p.still.T = geometry->m_worldTransform;
		if (geometry->geomData) { p.still.bound = geometry->geomData->m_kBound; p.still.radius = 1.0f; } else p.still.radius = 0.0f;
		NiSkinInstance* skin = geometry->skinInstance;
		if (skin && skin->SkinData && skin->SkinData->BoneData && skin->BoneObjects) {
			// the bones that move vertices, the largest bounds first when there are more than 16 (the hands and forearms carry most of the skin)
			struct B { UInt32 i; float r; } list[128]; int n = 0;
			for (UInt32 i = 0; i < skin->SkinData->Bones && n < 128; i++)
				if (SkinBones::Live(skin->BoneObjects[i]) && skin->SkinData->BoneData[i].Verts) list[n++] = { i, skin->SkinData->BoneData[i].Bound.Radius * skin->BoneObjects[i]->m_worldTransform.scale };
			if (n > kMaxBones) std::partial_sort(list, list + kMaxBones, list + n, [](const B& a, const B& b) { return a.r > b.r; });
			for (int k = 0; k < n && p.bones < kMaxBones; k++) {
				Snap& b = p.bone[p.bones++];
				b.T = skin->BoneObjects[list[k].i]->m_worldTransform;
				b.bound = skin->SkinData->BoneData[list[k].i].Bound;
				b.radius = 1.0f;
			}
		}
		return ++PartCount;
	}
	// wmTagged: WeaponMask tagged this draw (its own bit 0x80 in the same stencil states); the ids are OR'd into its reference and mask.
	inline void DrawBegin(IDirect3DDevice9* d, NiGeometry* geometry, bool wmTagged, DrawState& s) {
		s = DrawState();
		if (!Tagging || !d || !geometry) return;
		DWORD enabled = FALSE;
		d->GetRenderState(D3DRS_STENCILENABLE, &enabled);
		if (enabled && !wmTagged) { StatUntagged++; return; }	// the draw's own stencil test: left as it is (its pixels get no layer)
		const DWORD id = (DWORD)IdOf(geometry);
		d->GetRenderState(D3DRS_STENCILREF, &s.ref);
		d->GetRenderState(D3DRS_STENCILWRITEMASK, &s.write);
		if (!wmTagged) {
			d->GetRenderState(D3DRS_STENCILFUNC, &s.func);
			d->GetRenderState(D3DRS_STENCILPASS, &s.pass);
			d->GetRenderState(D3DRS_STENCILFAIL, &s.fail);
			d->GetRenderState(D3DRS_STENCILZFAIL, &s.zfail);
			d->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
			d->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_REPLACE);
			d->SetRenderState(D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
			d->SetRenderState(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
			d->SetRenderState(D3DRS_STENCILREF, id);
			d->SetRenderState(D3DRS_STENCILWRITEMASK, kIdMask);
			d->SetRenderState(D3DRS_STENCILENABLE, TRUE);
			s.alone = true;
		}
		else {
			d->SetRenderState(D3DRS_STENCILREF, (s.ref & ~kIdMask) | id);
			d->SetRenderState(D3DRS_STENCILWRITEMASK, s.write | kIdMask);
		}
		s.tagged = true;
	}
	inline void DrawEnd(IDirect3DDevice9* d, const DrawState& s) {
		if (!s.tagged || !d) return;
		d->SetRenderState(D3DRS_STENCILREF, s.ref);
		d->SetRenderState(D3DRS_STENCILWRITEMASK, s.write);
		if (s.alone) {
			d->SetRenderState(D3DRS_STENCILENABLE, FALSE);
			d->SetRenderState(D3DRS_STENCILFUNC, s.func);
			d->SetRenderState(D3DRS_STENCILPASS, s.pass);
			d->SetRenderState(D3DRS_STENCILFAIL, s.fail);
			d->SetRenderState(D3DRS_STENCILZFAIL, s.zfail);
		}
	}

	// ---- the part's transforms and screen box (FP space: the first-person camera at eye)
	// A transform's inverse as three rows that take a camera-relative point (P, 1) to the part's own space: l = R^T (P + eye - pos) / scale.
	inline void Rows(const NiTransform& T, const D3DXVECTOR3& eye, float out[3][4]) {
		const float inv = T.scale != 0.0f ? 1.0f / T.scale : 1.0f;
		const D3DXVECTOR3 shift(eye.x - T.pos.x, eye.y - T.pos.y, eye.z - T.pos.z);
		for (int i = 0; i < 3; i++) {
			out[i][0] = T.rot.data[0][i] * inv; out[i][1] = T.rot.data[1][i] * inv; out[i][2] = T.rot.data[2][i] * inv;
			out[i][3] = (T.rot.data[0][i] * shift.x + T.rot.data[1][i] * shift.y + T.rot.data[2][i] * shift.z) * inv;
		}
	}
	// A bound's sphere, camera-relative (as ShadowManager's ViewModelSpheres takes it).
	inline D3DXVECTOR4 Sphere(const NiTransform& T, const NiBound& B, const D3DXVECTOR3& eye) {
		const NiPoint3 c = T.rot * B.Center;
		return D3DXVECTOR4(T.pos.x + c.x * T.scale - eye.x, T.pos.y + c.y * T.scale - eye.y, T.pos.z + c.z * T.scale - eye.z, B.Radius * T.scale);
	}
	// ViewModelSpheres' filter: a bound centred more than 150 units from the eye is a stray (a bone parked far away), not a part on screen
	inline bool Near(const D3DXVECTOR4& s) { return s.x * s.x + s.y * s.y + s.z * s.z < 150.0f * 150.0f; }
	// (2026-11-16) the nearest part bound's centre from the eye this frame (DeferredFirstPerson's diagnostic line): a box that is never made
	// shows here as hundreds or thousands of units
	inline float NearestPart = -1.0f;
	inline void Note(const D3DXVECTOR4& s) { const float d = sqrtf(s.x * s.x + s.y * s.y + s.z * s.z); if (NearestPart < 0.0f || d < NearestPart) NearestPart = d; }

	// The camera's frame in Includes/DeferredDepth.hlsl's DD layout (DeferredFirstPerson::Frame): a sphere's box on screen, in pixels.
	inline bool SphereBox(const D3DXVECTOR4& s, const D3DXVECTOR4* frame, RECT& box) {
		const D3DXVECTOR3 right(frame[0].x, frame[0].y, frame[0].z), up(frame[1].x, frame[1].y, frame[1].z), fwd(frame[2].x, frame[2].y, frame[2].z);
		const D3DXVECTOR3 c(s.x, s.y, s.z);
		const float x = D3DXVec3Dot(&c, &right), y = D3DXVec3Dot(&c, &up), z = D3DXVec3Dot(&c, &fwd), r = s.w;
		const float vpW = frame[3].x > 0.0f ? 1.0f / frame[3].x : 0.0f, vpH = frame[3].y > 0.0f ? 1.0f / frame[3].y : 0.0f;
		if (z - r < 0.5f) { box = { (LONG)frame[3].z, (LONG)frame[3].w, (LONG)(frame[3].z + vpW), (LONG)(frame[3].w + vpH) }; return true; }	// round the eye: the whole view
		const float zNear = z - r, zFar = z + r;	// (not near / far: the Windows headers define those away); ndc over the sphere's box: each side's extreme at its nearest or farthest depth
		const float nx0 = (std::min)((x - r) / zNear, (x - r) / zFar) / frame[0].w, nx1 = (std::max)((x + r) / zNear, (x + r) / zFar) / frame[0].w;
		const float ny0 = (std::min)((y - r) / zNear, (y - r) / zFar) / frame[1].w, ny1 = (std::max)((y + r) / zNear, (y + r) / zFar) / frame[1].w;
		const float lx0 = ((nx0 + frame[5].x) + 1.0f) * 0.5f, lx1 = ((nx1 + frame[5].x) + 1.0f) * 0.5f;
		const float ly0 = (1.0f - (ny1 + frame[5].y)) * 0.5f, ly1 = (1.0f - (ny0 + frame[5].y)) * 0.5f;
		if (lx1 < 0.0f || lx0 > 1.0f || ly1 < 0.0f || ly0 > 1.0f) return false;
		box.left = (LONG)floorf(frame[3].z + Clamp01(lx0) * vpW) - 1; box.right = (LONG)ceilf(frame[3].z + Clamp01(lx1) * vpW) + 1;
		box.top = (LONG)floorf(frame[3].w + Clamp01(ly0) * vpH) - 1; box.bottom = (LONG)ceilf(frame[3].w + Clamp01(ly1) * vpH) + 1;
		return true;
	}
	inline void Grow(RECT& into, const RECT& r, bool& any) {
		if (!any) { into = r; any = true; return; }
		into.left = (std::min)(into.left, r.left); into.top = (std::min)(into.top, r.top); into.right = (std::max)(into.right, r.right); into.bottom = (std::max)(into.bottom, r.bottom);
	}
	inline void ClipTo(RECT& r, UINT w, UINT h) {
		r.left = (std::max)(r.left, 0L); r.top = (std::max)(r.top, 0L); r.right = (std::min)(r.right, (LONG)w); r.bottom = (std::min)(r.bottom, (LONG)h);
	}

	// One part's constants (c8 the header, c9-c11 a still part's rows, c12-c75 a skinned part's bones) and its box.
	struct PartData { float header[4]; float rigid[3][4]; float boneS[kMaxBones][4], boneX[kMaxBones][4], boneY[kMaxBones][4], boneZ[kMaxBones][4]; RECT box; bool any; };
	inline void Describe(const Part& p, const D3DXVECTOR3& eye, const D3DXVECTOR4* frame, PartData& out) {
		memset(&out, 0, sizeof(out));
		out.header[1] = p.weapon ? 1.0f : 0.0f;
		int bones = 0;
		// (2026-11-16) from the snapshot taken at the part's first draw (IdOf), not the transforms as they stand after the render
		for (int k = 0; k < p.bones; k++) {
			const D3DXVECTOR4 s = Sphere(p.bone[k].T, p.bone[k].bound, eye);
			float rows[3][4]; Rows(p.bone[k].T, eye, rows);
			memcpy(out.boneS[bones], &s.x, 16); memcpy(out.boneX[bones], rows[0], 16); memcpy(out.boneY[bones], rows[1], 16); memcpy(out.boneZ[bones], rows[2], 16);
			Note(s);
			RECT r; if (Near(s) && SphereBox(s, frame, r)) Grow(out.box, r, out.any);
			bones++;
		}
		if (bones) { out.header[0] = (float)bones; return; }
		Rows(p.still.T, eye, out.rigid);
		if (p.still.radius > 0.0f) { const D3DXVECTOR4 s = Sphere(p.still.T, p.still.bound, eye); Note(s); RECT r; if (Near(s) && SphereBox(s, frame, r)) Grow(out.box, r, out.any); }
		else { out.box = { (LONG)frame[3].z, (LONG)frame[3].w, (LONG)(frame[3].z + 1.0f / frame[3].x), (LONG)(frame[3].w + 1.0f / frame[3].y) }; out.any = true; }
	}

	// The first-person render begins (DeferredFirstPerson::Begin, the first-person depth surface bound): ids from now on.
	inline void BeginTagging(NiNode* weaponNode) { Tagging = Live && Pass; PartCount = 0; WeaponNode = weaponNode; NearestPart = -1.0f; }
	inline void EndTagging() { Tagging = false; }

	inline bool EnsureCopies(IDirect3DDevice9* d, const D3DSURFACE_DESC& t) {
		if (CopyTex[0] && CopyMade.Width == t.Width && CopyMade.Height == t.Height && CopyMade.Format == t.Format) return true;
		for (int k = 0; k < 2; k++) { if (Copy[k]) { Copy[k]->Release(); Copy[k] = nullptr; } if (CopyTex[k]) { CopyTex[k]->Release(); CopyTex[k] = nullptr; } }
		for (int k = 0; k < 2; k++)
			if (FAILED(d->CreateTexture(t.Width, t.Height, 1, D3DUSAGE_RENDERTARGET, t.Format, D3DPOOL_DEFAULT, &CopyTex[k], NULL)) || FAILED(CopyTex[k]->GetSurfaceLevel(0, &Copy[k]))) {
				Logger::Log("[ERROR] UNOFFICIAL GearWeather: could not make the G-buffer copies (%ux%u): no weather on the gear", t.Width, t.Height);
				PassFailed = true;
				return false;
			}
		CopyMade = t;
		return true;
	}

	// DeferredFirstPerson::End, after the view-model mark (stencil 0x40) and before the light passes. frame: the first-person camera (DD
	// layout); eye: its position (FP space); vmBox: the view model's screen box. The device state is DeferredFirstPerson's to put back
	// (a state block round the whole); the render targets and textures set here are left for it too.
	inline void Apply(IDirect3DDevice9* d, const D3DXVECTOR4* frame, const D3DXVECTOR3& eye, const RECT& vmBox) {
		if (!Live || !Pass || PartCount == 0 || !CapturedNormals::AlbedoSurface || !CapturedNormals::MaterialSurface || !CapturedNormals::Texture) { if (Live) StatSkipped++; return; }
		D3DSURFACE_DESC t = {};
		if (FAILED(CapturedNormals::AlbedoSurface->GetDesc(&t)) || !EnsureCopies(d, t)) { StatSkipped++; return; }
		RECT box = vmBox; ClipTo(box, t.Width, t.Height);
		if (box.right <= box.left || box.bottom <= box.top) { StatSkipped++; return; }
		static GpuTimer timer("First person: gear weather");
		GpuProfileScope gpu(timer, d);
		d->StretchRect(CapturedNormals::AlbedoSurface, &box, Copy[0], &box, D3DTEXF_NONE);
		d->StretchRect(CapturedNormals::MaterialSurface, &box, Copy[1], &box, D3DTEXF_NONE);
		d->SetRenderTarget(0, CapturedNormals::AlbedoSurface);
		d->SetRenderTarget(1, CapturedNormals::MaterialSurface);
		d->SetRenderTarget(2, nullptr); d->SetRenderTarget(3, nullptr);
		const D3DVIEWPORT9 full = { 0, 0, t.Width, t.Height, 0.0f, 1.0f };
		d->SetViewport(&full);
		const struct { D3DRENDERSTATETYPE s; DWORD v; } kStates[] = {
			{ D3DRS_ZENABLE, D3DZB_FALSE }, { D3DRS_ZWRITEENABLE, FALSE }, { D3DRS_ALPHATESTENABLE, FALSE }, { D3DRS_ALPHABLENDENABLE, FALSE },
			{ D3DRS_SEPARATEALPHABLENDENABLE, FALSE }, { D3DRS_CULLMODE, D3DCULL_NONE }, { D3DRS_SCISSORTESTENABLE, FALSE }, { D3DRS_SRGBWRITEENABLE, FALSE },
			{ D3DRS_FOGENABLE, FALSE }, { D3DRS_CLIPPLANEENABLE, 0 }, { D3DRS_COLORWRITEENABLE, 0xF }, { D3DRS_COLORWRITEENABLE1, 0xF },
			{ D3DRS_STENCILENABLE, TRUE }, { D3DRS_TWOSIDEDSTENCILMODE, FALSE }, { D3DRS_STENCILFUNC, D3DCMP_EQUAL }, { D3DRS_STENCILMASK, kIdMask | kMark },
			{ D3DRS_STENCILWRITEMASK, 0 }, { D3DRS_STENCILPASS, D3DSTENCILOP_KEEP }, { D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP }, { D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP } };
		for (const auto& k : kStates) d->SetRenderState(k.s, k.v);
		d->SetVertexShader(nullptr);
		d->SetPixelShader(Pass->ShaderHandle);
		d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
		d->SetTexture(0, CopyTex[0]); d->SetTexture(1, CopyTex[1]); d->SetTexture(2, CapturedNormals::Texture);
		for (DWORD s = 0; s < 3; s++) {
			d->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT); d->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT); d->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
			d->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); d->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP); d->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
		}
		d->SetPixelShaderConstantF(0, &frame[0].x, 6);
		d->SetPixelShaderConstantF(6, Sent, 1);
		const float skinShare = TheSettingManager->GetSettingF(kSection, "GearWeatherSkin");
		const float scale = TheSettingManager->GetSettingF(kSection, "GearWeatherScale");
		const float debug = (float)TheSettingManager->GetSettingI(kSection, "GearWeatherDebug");
		static PartData data;
		for (int i = 0; i < PartCount; i++) {
			Describe(Parts[i], eye, frame, data);
			if (!data.any) continue;
			RECT r = data.box; ClipTo(r, t.Width, t.Height);
			if (r.right <= r.left || r.bottom <= r.top) continue;
			const float look[4] = { scale > 0.0f ? 1.0f / scale : 0.3f, Clamp01(skinShare), debug, (float)(Parts[i].seed % 997u) * 0.731f };
			d->SetPixelShaderConstantF(7, look, 1);
			d->SetPixelShaderConstantF(8, data.header, 1);
			d->SetPixelShaderConstantF(9, &data.rigid[0][0], 3);
			if (data.header[0] > 0.5f) {
				const UINT n = (UINT)data.header[0];
				d->SetPixelShaderConstantF(12, &data.boneS[0][0], n); d->SetPixelShaderConstantF(28, &data.boneX[0][0], n);
				d->SetPixelShaderConstantF(44, &data.boneY[0][0], n); d->SetPixelShaderConstantF(60, &data.boneZ[0][0], n);
			}
			d->SetRenderState(D3DRS_STENCILREF, (DWORD)(i + 1) | kMark);
			const float x0 = (float)r.left - 0.5f, y0 = (float)r.top - 0.5f, x1 = (float)r.right - 0.5f, y1 = (float)r.bottom - 0.5f;
			const float u0 = (float)r.left / t.Width, v0 = (float)r.top / t.Height, u1 = (float)r.right / t.Width, v1 = (float)r.bottom / t.Height;
			const float quad[4][6] = { { x0, y0, 0, 1, u0, v0 }, { x1, y0, 0, 1, u1, v0 }, { x0, y1, 0, 1, u0, v1 }, { x1, y1, 0, 1, u1, v1 } };
			d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
			StatParts++;
			StatPixelsBox += (unsigned long long)(r.right - r.left) * (unsigned long long)(r.bottom - r.top);
		}
		StatPasses++;
	}
}
