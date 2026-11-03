#pragma once

// UNOFFICIAL 2026-10-16 cloth wind ([Shaders.Cloth.Main]; shaders: Includes/ClothWind.hlsl, ObjectTemplate.hlsl, ShadowMap.vso).
//
// Capes, dusters, flags, banners and curtains move in the wind, done entirely in the vertex shader by maths (a travelling smooth
// triangle wave for the billow, a slow gust, a two-wave ripple at the flutter speed), weighted per vertex by the vertex colour's
// RED channel (1 = loose ends, 0 = pinned) or the whole mesh (MaskMode 1). No physics, nothing per vertex on the CPU.
// Which objects: those whose diffuse texture path contains one of the Keywords (cached per texture). Per draw this sets the
// vertex shader constants c248-c251 (strength 0 for everything else, so other objects run no wind code at all).
// The wind: the settings' direction and speed, or the game's own (the sky's wind speed and direction, UseGameWind).
// Phases are worked out here in double precision and wrapped, so the motion never jitters after long play.
// 2026-10-17 (cloth round 2): the phases are INTEGRATED from the game's clock (a pause or menu stops them, the time scale does not
// change the speed) instead of being a speed times the hour of the day: that clock was in game seconds / 1000, i.e. 30x too slow in
// play (the billow crept at 6 units/s, the 3 Hz flutter at 0.09 Hz), it jumped back at midnight, and every change of the weather's
// wind speed made the phase jump (time x new speed). Test knob TimeOffset (seconds) shifts the pattern for time-step pictures.
// The vertex shader works in the object's space for static meshes (the wind is turned into it with the object's rotation and
// scale); for skinned ones in the skin root's space in the game's passes and in world space in NVR's shadow pass (its own bones).
// SAFETY: every pass of an object must move it the same way (lamp passes test depth EQUAL). Some passes use the game's own
// (vanilla) vertex shaders, which do not move it -- the texture pass of multi-pass objects then misses and the cloth turns white.
// So an object gets cloth only if, last frame, every one of its passes had an NVR vertex shader (SetShadersHook notes them).
// Interiors: no wind, so no cloth (2026-10-21: unless [Shaders.Cloth.Interiors] is on: its own gentle values, see BeginFrame).
// 2026-10-18 (cloth round 3, 'cloth does nothing'): keywords match as substrings (with an exclusion list), worn clothing is masked by
// the garment's own BIND-POSE height (pinned shoulders, loose hem), the calm-weather strength is higher, and non-cloth draws can no
// longer inherit c248 from the grass shaders (they relocate their shadow matrices to c248..c255). See evidence\cloth3-LOG.md.
// 2026-10-19 (cloth round 5): worn clothing by a per-vertex weight from its own mesh (ClothWeights.h, mask 5), delivered as vertex
// stream 1 around the skinned partition draw (DrawPartition); the skeleton mask (4) stays the fallback. evidence\cloth3-LOG.md C3-11.
// 2026-10-20 (cloth round 9): worn coats hang from the hips and their hem is simulated while walking (ClothWalk.h, WalkOf). C3-15.
// 2026-10-20 (user's video: 'the entire tent mesh wobbles around'; 'only move thin parts of static cloth objects'): still cloth by a
// per-vertex weight from its own mesh (ClothStatic.h, mask 6: poles, ropes and solid parts still, the cloth held where it touches them,
// the ground and its top line, loosest at its free edges), worked out once per mesh on a worker thread and delivered as vertex stream
// 1 around the game's still-object draw (NiDX9Renderer::Do_OnlyRenderShape / Do_OnlyRenderTristrips: OnlyRender). See StillOf.
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include "PipboyFit.h"	// UNOFFICIAL 2026-11-03 PipboyFit: its constants go out from DrawPartition too
#include <memory>
#include <atomic>
#include "ClothWeights.h"	// 2026-10-19 round 5: per-vertex weights of worn meshes (mask 5)
#include "ClothWalk.h"		// 2026-10-20 round 9: the walking simulation of worn coats (hem ring)
#include "ClothStatic.h"	// 2026-10-20: per-vertex weights of still cloth (mask 6: tents, flags, banners)
#include "TestTools.h"		// NVR_TEST_TOOLS (2026-10-21 diagnostic DiagStillDraw)

namespace ClothWind {

	struct Settings {
		bool  CompiledIn = false;		// Enabled at startup (CLOTH_WIND_ON in the shaders)
		bool  On = false;				// Enabled, live
		int   MaskMode = 2;				// 0 vertex colour red, 1 whole mesh, 2 by height (pinned at the top)
		float HeightTop = 95.0f, HeightBottom = 25.0f;	// mask 2 on worn clothing: units above the actor's root (waist .. near the ground)
		bool  UseGameWind = true;
		float Dir[3] = { 1.0f, 0.0f, 0.0f };
		float Speed = 220.0f;			// units per second along the wind (the billow's travel)
		float Intensity = 6.0f;			// units (about 9 cm) of movement at full mask
		float Wavelength = 260.0f;		// units
		float Flutter = 3.0f;			// flutter cycles per second
		float FlutterAmount = 0.35f;
		float Ripple = 0.5f;
		float RippleScale = 0.012f;		// ripple cycles per unit
		float TimeOffset = 0.0f;		// test only: seconds the pattern is ahead
		bool  HeightFromFeet = true;	// SkinMask 0 only: heights from the actor's feet (the node the mesh hangs from), not the skin root
		int   SkinMask = 1;				// worn clothing: 0 the old actor-height mask (skin root), 1 bind-pose height above the feet (HeightTop..HeightBottom, legs still below LegCut), 2 the garment's own height range (PinFraction)
		float LegCut = 14.0f;			// SkinMask 1: below this bind-pose height (boots, ankles) nothing moves, full strength 10 units higher
		float PinFraction = 0.4f;		// SkinMask 1: the top part of the garment (fraction of its height) that does not move at all
		int   SkinSpace = 1;			// worn clothing, what space the bones put vertices in: 0 the skin root's (rotated wind), 1 camera-relative world, 2 world
		float CalmFactor = 0.6f;		// UseGameWind: strength in dead calm (rising to 1.7 in a gale); it was 0.3
		bool  LogMatches = false;		// diagnostic: every skinned texture and every match goes to the log with its mesh numbers
		// 2026-10-19 worn clothing follows its wearer (see MotionOf)
		float MotionDrag = 0.1f;		// units the hem trails behind per unit/s of the wearer's speed, at low speed (walk ~100/s: ~8.5 units)
		float MotionMax = 30.0f;		// the most it trails (approached softly: a run ~350/s ~20 units)
		float MotionSpring = 1.5f;		// how fast it swings back (cycles per second)
		float MotionDamping = 0.25f;	// 0 swings on and on .. 1 settles without swinging past
		float StrideSwing = 6.0f;		// units of side-to-side swish at a run (less walking, none standing)
		float StrideLength = 250.0f;	// units run per swish (one left and one right step); shorter steps when walking
		float TurnLag = 0.12f;			// seconds the hem lags behind a turn (it flares out); 0 off
		bool  MotionIndoors = true;		// the movement part also indoors (no wind there)
		bool  UseSkeleton = true;		// 2026-10-19 worn clothing masked by its own skeleton (see SkeletonOf); off: SkinMask
		bool  UseWeights = true;		// 2026-10-19 round 5: worn clothing by per-vertex weights from its own mesh (WeightsOf, mask 5); off: the skeleton mask (4)
		// 2026-10-20 round 9: the walking simulation of worn coats (mask 5; see WalkOf)
		bool  WalkSim = true;
		float WalkHang = 1.0f;			// how fully the coat hangs from the hips instead of riding the legs (0 = rides them as made, 1 = hangs)
		float WalkSpring = 1.4f, WalkDamping = 0.2f, WalkDrag = 1.0f, WalkLegMargin = 1.5f, WalkMaxSwing = 25.0f;
		float WalkDragCap = 180.0f, WalkMaxAngle = 35.0f;	// 2026-10-20 round 9b (see ClothWalk.h)
		float WalkHemSmooth = 1.0f;							// 2026-10-20 round 9c (see ClothWalk.h)
		bool  StillWeights = true;		// 2026-10-20 still cloth by per-vertex weights from its own mesh (mask 6, ClothStatic.h); off: the height mask (2)
		bool  AutoDetect = false;		// 2026-10-20 worn garments not named in Keywords move when their own mesh hangs free (AutoVerdictOf)
		float AutoShare = 2.0f;			// ...at least this share (%) of the garment hangs free
		bool  LogAuto = true;			// ...every verdict to the log once
		float LimbGap = 1.0f;			// units off the limb (beyond the garment's own measured leg thickness) where cloth starts to move
		float LimbFade = 6.0f;			// ...and how many units further it moves fully (2026-10-19: was 2.5 -- a coat's lining 1 unit inside
										// its shell lagged up to 12 units behind it, 'the coat splits into pieces by layer'; now <= 5)
		// 2026-10-21 indoors ([Shaders.Cloth.Interiors], read while Enabled; see BeginFrame): a slow, gentle sway of its own
		struct Indoor { float Speed = 80.0f, Intensity = 2.0f, Wavelength = 400.0f, Flutter = 0.8f, FlutterAmount = 0.1f, Ripple = 0.3f, Strength = 1.0f; } In;
		std::string Keywords = "cape,cloak,duster,flag,banner,curtain,drape,tarp,poncho,robe,scarf,tent";
		std::string Exclude = "scape,probe,microbe,wardrobe,drobe,content,intent,extent,patent,potent,latent,flagpole,flagstone,pole,stone";
		std::string ExcludeParts = "hat,helmet,head,hair,glove,boot,shoe,mask,goggle,hand,body,beard,mouth,teeth,eyes";
	};

	inline Settings S;
	inline std::vector<std::string> Words, Excluded, ExcludedParts, ForcedOff;
	inline std::string WordsFrom, ExcludeFrom, ExcludePartsFrom, ForceOffFrom;
	struct Extent { uint16_t count = 0; float lo = 0.0f, hi = 0.0f; bool fromVertices = false; };
	inline std::unordered_map<const void*, Extent> Extents;		// bind-pose height range per skinned mesh (geometry data)
	inline UInt32 SkinnedDrawsNow = 0;
	inline std::unordered_map<const void*, std::pair<std::string, bool>> ByTexture;
	inline float WorldDir[3] = { 1.0f, 0.0f, 0.0f };
	inline float Phase[4] = {};			// main wave, gust, flutter 1, flutter 2 (cycles, wrapped)
	inline double Cycles[4] = {};		// the same, integrated, plus the rate each runs at now (cycles per second)
	inline double Rate[4] = {};
	inline double ClockSeconds = 0.0;	// seconds of play at the normal time scale (stops in a pause, not scaled by the time scale)
	inline float LastHour = -1.0f;
	inline double RealStart = -1.0, ClockStart = 0.0;
	inline float SpeedNow = 220.0f, IntensityNow = 6.0f;
	inline float WavelengthNow = 260.0f, FlutterNow = 3.0f, FlutterAmountNow = 0.35f, RippleNow = 0.5f;	// 2026-10-21 Main's, or the indoor set's
	inline bool Interior = false;		// 2026-10-21 this frame is indoors
	inline bool IndoorWind = false;		// ...and the indoor set is on ([Shaders.Cloth.Interiors] Enabled)
	inline UInt32 IndoorLogs = 0;
	inline bool NonZeroSent = false;	// the device's c248 may hold a cloth draw's values (or the grass shaders' matrices: see NoteVertexShader)
	inline bool GrassInC248 = false;	// 2026-10-19: ...and it is the grass shaders' matrices (cleared when a cloth-carrying shader is bound)
	inline UInt32 ClothDraws = 0, Frames = 0;
	inline bool LoggedGameWind = false;
	inline bool CompiledInRead = false;
	inline bool Exterior = true;
	// 2026-10-19 per wearer (the skin's root node): its movement, the hem's lag as a damped spring, the stride phase
	struct Motion {
		float pos[3] = {}, vel[3] = {};			// last position, smoothed velocity (units/s)
		float offset[3] = {}, offsetVel[3] = {};	// the hem's trailing offset (world units) and its rate
		float yaw = 0.0f, yawRate = 0.0f, turn = 0.0f, turnVel = 0.0f;	// heading, smoothed turn rate (rad/s), lag angle (rad) and its rate
		double stride = 0.0, clock = 0.0;
		UInt32 frame = 0;
		bool valid = false;
		float out[4] = {};						// this frame's offset (xyz) and turn lag (w), the same for every pass
	};
	inline std::unordered_map<const void*, Motion> Motions;
	inline UInt32 FrameId = 0;
	inline std::unordered_set<const void*> SeenNow, SeenLast, VanillaNow, VanillaLast;	// objects drawn / with a vanilla vertex shader
	// The vertex shaders that carry the cloth code: NVR's ObjectTemplate ones (PBR.h's table), by name, built once.
	inline std::unordered_set<std::string> CapableNames;
	inline bool CapableBuilt = false;
	inline bool CapableName(const char* name) {
		if (!CapableBuilt && TheShaderManager && TheShaderManager->Shaders.PBR) {
			CapableBuilt = true;
			for (const auto& [n, t] : TheShaderManager->Shaders.PBR->Templates())
				if (t.Name && !strcmp(t.Name, "ObjectTemplate") && n.size() > 4 && n.substr(n.size() - 4) == ".vso") CapableNames.insert(std::string(n));
			Logger::Log("Cloth wind: %u vertex shaders carry it", (unsigned)CapableNames.size());
		}
		return name && CapableNames.count(name) != 0;
	}
	// SetShadersHook, for every pass: did this object's pass use a vertex shader that carries the cloth code?
	// The grass vertex shaders (GRASS23x00x) keep their shadow matrices in c248..c255: after one of them c248 is not zero, and the
	// next object with cloth code would move although it is no cloth. Mark the register as 'to be zeroed' (the next non-cloth draw does it).
	inline bool CurrentVSCapable = false;	// 2026-10-19: the vertex shader the last SetShaders bound carries the cloth code
	inline const char* CurrentVSName = nullptr;	// 2026-10-21 diagnostic (DiagStillDraw): its name
	inline std::unordered_map<const void*, bool> TentativeNow;	// 2026-10-19: per object this frame, did the BOUND vertex shader carry it at each lit pass
	// 2026-10-19 (test on the user's flicker save NVRAutoTest13, run 20261019-204305: with the cloth code compiled in but cloth switched
	// OFF, 8.4 % of the picture changed -- houses and the road white, rocks and poles bent -- vs 0.02 % frame-to-frame noise): the grass
	// mark was set only while cloth was ON, so with it off nothing ever cleared the grass matrices out of c248, and every NVR object
	// vertex shader drawn after grass ran the cloth code on them (passes no longer met: white objects, torn meshes). With cloth on the
	// same happened to anything drawn after grass by a path without ForGeometry. Now, whenever the cloth code is compiled in, the grass
	// marks c248 and binding a cloth-carrying vertex shader clears it at once -- before its draw, whatever path draws it.
	inline void Zero(IDirect3DDevice9* device);
	// UNOFFICIAL 2026-10-20 plant wind (core/PlantWind.h): static plants use this machinery too (c248-c253 with c248.x = -1, the SAFETY
	// bookkeeping below, AfterDraw's clean-up), so it runs while either is on. Set by PlantWind::BeginFrame.
	inline bool PlantsCompiledIn = false, PlantsOn = false;
	inline void NoteVertexShader(const char* name) {
		if (!CompiledInRead && TheSettingManager) { CompiledInRead = true; S.CompiledIn = TheSettingManager->GetSettingI("Shaders.Cloth.Main", "Enabled") != 0; }
		if (!S.CompiledIn && !PlantsCompiledIn) { CurrentVSCapable = false; return; }
		// 2026-10-20 (user, cloth 9b + Particles on: 'pbr is now incredibly broken in interiors', walls and floors gone or torn): not only
		// the grass. The particle and blood-decal shaders (NOLIGHT006/016/017, GDECAL/GDECALS: Includes/ParticleLight.hlsl) keep their
		// lamp colours in c227-c250, so after a particle the next object with cloth code took a lamp colour for ClothA and moved
		// (indoors, where the lamps are). Any vertex shader without the cloth code may leave c248 non-zero: mark it, and the next
		// cloth-carrying bind clears it before its draw (one register write per such switch).
		const bool capable = CapableName(name);
		if (!capable) { NonZeroSent = true; GrassInC248 = true; }
		if (capable && GrassInC248 && TheRenderManager) Zero(TheRenderManager->device);
		CurrentVSCapable = (S.On || PlantsOn) && capable;
		CurrentVSName = name;
	}
	// 2026-10-19 (P81 report 'cloth: only the player's SHADOW moves'): the object the render-pass hooks are drawing right now. SetShadersHook
	// used the game's 'current geometry' global, which for worn (skinned) meshes is not the mesh being drawn: their lit passes were never
	// noted, the SAFETY rule skipped every one of them (600 of 600 frames on NVRAutoTest12) while the shadow passes moved them.
	inline const void* PassGeometry = nullptr;
	inline std::unordered_map<const void*, std::string> VanillaVSNow, VanillaVSLast;
	// 2026-10-19 diagnostic (LogMatches): per worn cloth mesh, lit passes moved / skipped (not seen by SetShaders, vanilla VS), shadow passes
	struct PassCounts { std::string name; UInt32 lit = 0, notSeen = 0, vanilla = 0, shadow = 0; };
	inline std::unordered_map<const void*, PassCounts> ClothPassCounts;	// 2026-10-19 diagnostic (LogMatches): which vertex shader
	inline void NoteShaders(const void* geometry, bool nvrVertexShader, const char* vertexShaderName = nullptr) {
		if (!(S.On || PlantsOn) || !geometry) return;
		SeenNow.insert(geometry);
		if (!nvrVertexShader) {
			VanillaNow.insert(geometry);
			if (S.LogMatches) VanillaVSNow[geometry] = vertexShaderName ? vertexShaderName : "(none)";
		}
	}

	// Keywords and exclusions: lower case, '/' written for the path separator (the game's paths use '\').
	inline std::string Norm(const std::string& in) {
		std::string o(in);
		for (char& c : o) { c = (char)tolower((unsigned char)c); if (c == '/') c = '\\'; }
		return o;
	}
	inline bool Letter(const std::string& p, size_t i) { return i < p.size() && p[i] >= 'a' && p[i] <= 'z'; }
	// A keyword is found ANYWHERE in the path ('lecloak', 'leflag', 'bosbanner', 'heavyduster' count); a short one (4 letters or
	// fewer: cape, flag, tent, robe, tarp, coat) must close a word on its right (a plural s allowed): 'flag' is not 'flagstone'.
	// Longer ones can be a path piece like 'lucassimms\outfit' (several textures of one folder).
	inline bool KeywordIn(const std::string& p, const std::string& w) {
		for (size_t at = p.find(w); at != std::string::npos; at = p.find(w, at + 1)) {
			if (w.size() > 4) return true;
			size_t end = at + w.size();
			if (end < p.size() && p[end] == 's' && !Letter(p, end + 1)) end++;
			if (!Letter(p, end)) return true;
		}
		return false;
	}
	// The path is cloth when a keyword is in it, none of the Exclude pieces is, and the FILE's own words name no body part (a hat or
	// gloves that share the garment's folder or keyword do not move: ExcludeParts, matched against the file name's letters).
	// Diagnostic (LogMatches): a static texture that looks like cloth but did not match is listed, so missed words can be found.
	inline bool LooksLikeCloth(const char* path) {
		const std::string p = Norm(path);
		for (const char* w : { "cloth", "flag", "banner", "cloak", "cape", "sheet", "tent", "tarp", "canvas", "rag", "curtain", "drape", "awning", "fabric", "blanket", "towel", "laundry", "pennant", "rug", "sail", "coat", "robe", "scarf" })
			if (p.find(w) != std::string::npos) return true;
		return false;
	}
	inline bool ExcludedPart(const std::string& p);
	// 2026-10-20 ForceOff: a texture whose path holds one of these never moves, whether a keyword or AutoDetect picked it.
	inline bool IsForcedOff(const std::string& p) {
		for (const std::string& x : ForcedOff) if (!x.empty() && p.find(x) != std::string::npos) return true;
		return false;
	}
	inline bool IsClothTexture(const char* path) {
		if (!path || !path[0]) return false;
		const std::string p = Norm(path);
		bool hit = false;
		for (const std::string& w : Words) if (!w.empty() && KeywordIn(p, w)) { hit = true; break; }
		if (!hit) return false;
		for (const std::string& x : Excluded) if (!x.empty() && p.find(x) != std::string::npos) return false;
		return !IsForcedOff(p) && !ExcludedPart(p);
	}
	// The FILE name's words name a body part (ExcludeParts: a hat or gloves sharing the garment's folder, a body's skin).
	inline bool ExcludedPart(const std::string& p) {
		const size_t slash = p.find_last_of('\\');
		const std::string file = p.substr(slash == std::string::npos ? 0 : slash + 1);
		for (size_t i = 0; i < file.size();) {
			if (!Letter(file, i)) { i++; continue; }
			size_t j = i;
			while (Letter(file, j)) j++;
			const std::string word = file.substr(i, j - i);
			for (const std::string& x : ExcludedParts) {
				if (x.empty()) continue;
				if (x.size() >= 4 ? word.find(x) != std::string::npos
					: (word.size() >= x.size() && (word.compare(word.size() - x.size(), x.size(), x) == 0 || (word.back() == 's' && word.size() > x.size() && word.compare(word.size() - 1 - x.size(), x.size(), x) == 0))))
					return true;
			}
			i = j;
		}
		return false;
	}
	inline void SplitList(const char* text, std::vector<std::string>& out) {
		out.clear();
		std::string w;
		for (const char* c = text; ; c++) {
			if (!*c || *c == ',') { while (!w.empty() && w.back() == ' ') w.pop_back(); if (!w.empty()) out.push_back(Norm(w)); w.clear(); if (!*c) break; }
			else if (!(w.empty() && *c == ' ')) w += *c;
		}
	}

	// The bind-pose height range of a skinned mesh (its own vertices before skinning), once per geometry data. The vertex array
	// is read only when the game kept it (KEEP_XYZ); else the bound sphere stands in.
	inline const Extent& ExtentOf(NiGeometry* geometry) {
		NiGeometryData* d = geometry->geomData;
		auto it = Extents.find(d);
		if (it != Extents.end() && it->second.count == d->m_usVertices) return it->second;
		if (Extents.size() >= 4096) Extents.clear();
		Extent e;
		e.count = d->m_usVertices;
		if ((d->m_ucKeepFlags & NiGeometryData::KEEP_XYZ) && d->m_pkVertex && d->m_usVertices > 3) {
			e.lo = 1e30f; e.hi = -1e30f;
			for (uint16_t i = 0; i < d->m_usVertices; i++) { const float z = d->m_pkVertex[i].z; if (z < e.lo) e.lo = z; if (z > e.hi) e.hi = z; }
			e.fromVertices = true;
		}
		else { e.lo = d->m_kBound.Center.z - d->m_kBound.Radius; e.hi = d->m_kBound.Center.z + d->m_kBound.Radius; }
		return Extents[d] = e;
	}

	// 2026-10-19 (user: 'our body itself is billowing around, not just the coat -- how can we do this intelligently without a lot of
	// hand-made tweaking'): worn clothing masked by the garment's OWN skeleton. Every skinned mesh carries its bones (names) and where
	// each sat when the mesh was made (NiSkinData SkinToBone; the bone's origin in the mesh's own space = the space the vertex shader's
	// IN.position is in). From them: hip, knee, ankle and toe of each leg, elbow and wrist of each arm. The shader moves only what stands
	// OFF those limbs (distance to the nearest limb segment minus a limb radius: hip 7 -> knee 5 -> ankle 5 (boots) -> toe tip 3.5,
	// elbow 3.5 -> wrist 2.5 units, 1 unit = 1.43 cm), only below the hips (pinned at the hip joints, loosest from the knees down) and
	// nothing at ground level (fades in over the 6 units up to the ankles): legs, trousers, boots, hands and the torso stay put, a coat's
	// skirt and tails move -- with no per-armour numbers. The height mask it replaces (SkinMask 1) let everything below HeightTop 95
	// (chest height) move, hips, legs and hands included. Checked offline on vanilla NIFs (evidence\cloth3\skeleton_mask_check.py:
	// hips z 67.8, knees 35.8, ankles 7.1, toes 1.2 forward +y; legs 0.00 moving vs 0.4-0.95 before). A mesh without thigh bones (a
	// creature, a flag) keeps SkinMask. A missing ankle is put a thigh's length below the knee, a missing toe 12 units forward of the
	// ankle (+y, the bind pose's forward); missing arms are left out.
	struct Skeleton {
		bool ok = false;
		bool actor = false;				// it hangs on a human skeleton (any 'Bip01' bone): without usable legs it does not move at all
		bool measured = false;			// 2026-10-19: the leg radii are the garment's own (MeasureLegs), not the fixed ones
		const void* boneData = nullptr; UInt32 bones = 0;	// what it was made from (the skin data can be freed and reused)
		float pts[12][4] = {};			// L hip, knee, ankle, toe tip, R hip, knee, ankle, toe tip, L elbow, wrist, R elbow, wrist (xyz, radius)
		float hipZ = 0.0f, kneeZ = 0.0f, groundZ = 0.0f;
		int bone[5] = { -1, -1, -1, -1, -1 };	// 2026-10-20 skin bone indices: pelvis (or the lowest spine bone), L thigh, L calf, R thigh, R calf
	};
	inline std::unordered_map<const void*, Skeleton> Skeletons;	// per NiSkinData
	inline UInt32 SkeletonsLogged = 0;
	inline float LimbDistance(const float* p, const float* a, const float* b) {	// to the segment a-b, minus its radius there
		const float ab[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, ap[3] = { p[0] - a[0], p[1] - a[1], p[2] - a[2] };
		const float len2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
		const float t = std::clamp(len2 > 1e-4f ? (ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / len2 : 0.0f, 0.0f, 1.0f);
		const float d[3] = { ap[0] - ab[0] * t, ap[1] - ab[1] * t, ap[2] - ab[2] * t };
		return sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) - (a[3] + (b[3] - a[3]) * t);
	}
	inline float Smooth01(float x) { x = std::clamp(x, 0.0f, 1.0f); return x * x * (3.0f - 2.0f * x); }
	// The same mask as the shader's (ClothWind.hlsl, mode 4): for the log's check that the bones and the vertices line up.
	inline float SkeletonMask(const Skeleton& k, const float* p) {
		static const int seg[8][2] = { {0, 1}, {1, 2}, {2, 3}, {4, 5}, {5, 6}, {6, 7}, {8, 9}, {10, 11} };
		float d = 1e30f;
		for (const auto& sg : seg) d = (std::min)(d, LimbDistance(p, k.pts[sg[0]], k.pts[sg[1]]));
		const float off = Smooth01((d - S.LimbGap) / (std::max)(S.LimbFade, 0.1f));
		const float h = Smooth01((k.hipZ - p[2]) / (std::max)(k.hipZ - k.kneeZ, 1.0f));
		return off * h * Smooth01((p[2] - k.groundZ) / 6.0f);
	}
	// 2026-10-19 (user: 'how can we detect what's part of the cloth on a mesh intelligently?'): the garment's OWN leg thickness. For every
	// kept vertex below the hips: its nearest thigh or calf segment and its distance to that segment's axis; per joint (hip, knee, ankle
	// of each leg) the 10th percentile of those distances = the innermost surface there -- the trousers under a coat. Kept within
	// hip 5-10, knee 3.5-8, ankle 3-6.5 units, so a robe with no legs modelled under it falls back to the most a leg can be and still
	// moves. Then cloth = what stands LimbGap (0.5) beyond that surface, fully LimbFade (2.5) further. Offline on vanilla + the user's
	// ADAM replacer NIFs (evidence\cloth3\skeleton_mask_check.py): coats move more than with fixed radii (Sheriff's duster 10.5 -> 20.8 %,
	// robe 19.8 -> 24.3 %, the ADAM ranger duster 0.0 -> 3.7 % -- modelled skin-tight on the thighs, no trousers under it, so only its
	// panels between the legs and its tails stand off), legs ~0.05 of the motion. Without kept vertices the fixed radii stay.
	inline void MeasureLegs(Skeleton& k, NiGeometry* geometry) {
		const NiGeometryData* gd = geometry->geomData;
		if (!gd || !(gd->m_ucKeepFlags & NiGeometryData::KEEP_XYZ) || !gd->m_pkVertex) return;
		static const int seg[4][2] = { {0, 1}, {1, 2}, {4, 5}, {5, 6} };
		static const float rmin[3] = { 5.0f, 3.5f, 3.0f }, rmax[3] = { 10.0f, 8.0f, 6.5f };
		std::vector<float> samples[7];
		for (UInt16 v = 0; v < gd->m_usVertices; v++) {
			const float* p = &gd->m_pkVertex[v].x;
			if (p[2] > k.hipZ + 2.0f || p[2] < k.groundZ + 6.0f) continue;
			float best = 1e30f; int joint = -1;
			for (const auto& sg : seg) {
				const float* a = k.pts[sg[0]]; const float* b = k.pts[sg[1]];
				const float ab[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, ap[3] = { p[0] - a[0], p[1] - a[1], p[2] - a[2] };
				const float len2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
				const float t = std::clamp(len2 > 1e-4f ? (ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / len2 : 0.0f, 0.0f, 1.0f);
				const float dx = ap[0] - ab[0] * t, dy = ap[1] - ab[1] * t, dz = ap[2] - ab[2] * t;
				const float d = sqrtf(dx * dx + dy * dy + dz * dz);
				if (d < best) { best = d; joint = t < 0.5f ? sg[0] : sg[1]; }
			}
			if (joint >= 0) samples[joint].push_back(best);
		}
		for (int j : { 0, 1, 2, 4, 5, 6 }) {
			std::vector<float>& ds = samples[j];
			if (ds.size() < 20) continue;
			std::nth_element(ds.begin(), ds.begin() + ds.size() / 10, ds.end());
			const int kind = j % 4;	// 0 hip, 1 knee, 2 ankle
			k.pts[j][3] = std::clamp(ds[ds.size() / 10], rmin[kind], rmax[kind]);
		}
		k.measured = true;
	}

	inline const Skeleton& SkeletonOf(NiGeometry* geometry) {
		static const Skeleton none;
		const NiSkinInstance* si = geometry->skinInstance;
		const NiSkinData* sd = si ? si->SkinData : nullptr;
		if (!sd || !sd->BoneData || !si->BoneObjects) return none;
		auto it = Skeletons.find(sd);
		if (it != Skeletons.end() && it->second.boneData == sd->BoneData && it->second.bones == sd->Bones) return it->second;
		if (Skeletons.size() >= 2048) Skeletons.clear();
		Skeleton k;
		k.boneData = sd->BoneData; k.bones = sd->Bones;
		static const char* const names[12] = { "bip01 l thigh", "bip01 l calf", "bip01 l foot", "bip01 l toe0", "bip01 r thigh", "bip01 r calf",
			"bip01 r foot", "bip01 r toe0", "bip01 l forearm", "bip01 l hand", "bip01 r forearm", "bip01 r hand" };
		static const float radius[12] = { 7.0f, 5.0f, 5.0f, 3.5f, 7.0f, 5.0f, 5.0f, 3.5f, 3.5f, 2.5f, 3.5f, 2.5f };
		bool found[12] = {};
		// 2026-10-19 (user: 'our legs still bend around when we walk'): the count is the skin data's, as ShadowManager's SkinnedWorldBound
		// reads it -- NiSkinInstance::Bones (+0x1C) read 0 in game, so no bone was ever looked at, every garment fell back to the height
		// mask (SkinMask 1) and the legs below the knee kept moving
		const UInt32 count = (std::min)(sd->Bones, (UInt32)512);
		for (UInt32 b = 0; b < count; b++) {
			const NiAVObject* bone = si->BoneObjects[b];
			const char* name = bone ? bone->m_pcName : nullptr;
			if (!name) continue;
			if (!_strnicmp(name, "bip01", 5)) k.actor = true;
			if (!_stricmp(name, "bip01 pelvis")) k.bone[0] = (int)b;
			else if (!_stricmp(name, "bip01 spine") && k.bone[0] < 0) k.bone[0] = (int)b;	// (replaced by the pelvis when it comes later)
			for (int n = 0; n < 12; n++) {
				if (found[n] || _stricmp(name, names[n])) continue;
				if (n == 0) k.bone[1] = (int)b; else if (n == 1) k.bone[2] = (int)b; else if (n == 4) k.bone[3] = (int)b; else if (n == 5) k.bone[4] = (int)b;
				// the bone's origin in the mesh's space: SkinToBone maps p to s R p + t, so its origin is -R^T t / s
				const NiTransform& t = sd->BoneData[b].SkinToBone;
				const float sc = t.scale > 1e-4f ? t.scale : 1.0f;
				for (int i = 0; i < 3; i++)
					k.pts[n][i] = -(t.rot.data[0][i] * t.pos.x + t.rot.data[1][i] * t.pos.y + t.rot.data[2][i] * t.pos.z) / sc;
				k.pts[n][3] = radius[n];
				found[n] = true;
			}
		}
		k.ok = found[0] && found[1] && found[4] && found[5];
		if (k.ok) {
			for (int leg = 0; leg < 2; leg++) {
				float* hip = k.pts[leg * 4], *knee = k.pts[leg * 4 + 1], *ankle = k.pts[leg * 4 + 2], *toe = k.pts[leg * 4 + 3];
				if (!found[leg * 4 + 2]) { for (int i = 0; i < 3; i++) ankle[i] = knee[i] + (knee[i] - hip[i]) * 0.95f; ankle[3] = radius[2]; }	// a thigh's length below the knee
				if (!found[leg * 4 + 3]) { toe[0] = ankle[0]; toe[1] = ankle[1] + 11.6f; toe[2] = ankle[2] - 5.9f; toe[3] = radius[3]; }	// vanilla: 11.6 forward, 5.9 down
				for (int i = 0; i < 3; i++) toe[i] += (toe[i] - ankle[i]) * 0.35f;	// the toe bone sits at the ball of the foot: on to the tip
			}
			for (int arm = 0; arm < 2; arm++) {	// a missing arm: far away (no effect)
				if (!found[8 + arm * 2] || !found[9 + arm * 2])
					for (int n = 8 + arm * 2; n < 10 + arm * 2; n++) { k.pts[n][0] = k.pts[n][1] = 0.0f; k.pts[n][2] = -1e5f; k.pts[n][3] = 0.0f; }
			}
			k.hipZ = 0.5f * (k.pts[0][2] + k.pts[4][2]);
			k.kneeZ = 0.5f * (k.pts[1][2] + k.pts[5][2]);
			k.groundZ = 0.5f * (k.pts[2][2] + k.pts[6][2]) - 6.0f;
			k.ok = k.hipZ - k.kneeZ > 5.0f;	// an upright leg (else the space is not what we think: keep SkinMask)
			if (!std::isfinite(k.hipZ) || !std::isfinite(k.kneeZ) || !std::isfinite(k.groundZ) || k.hipZ - k.groundZ > (float)ClothWeights::kMaxLegs) k.ok = false;	// 2026-10-20: broken bone data
			if (k.ok) MeasureLegs(k, geometry);
		}
		if (SkeletonsLogged < 24 || S.LogMatches) {	// the check: bone heights, and how much of the mesh the mask moves
			SkeletonsLogged++;
			const NiGeometryData* gd = geometry->geomData;
			UInt32 moving = 0, total = 0;
			if (k.ok && gd && (gd->m_ucKeepFlags & NiGeometryData::KEEP_XYZ) && gd->m_pkVertex)
				for (UInt16 v = 0; v < gd->m_usVertices; v++, total++) if (SkeletonMask(k, &gd->m_pkVertex[v].x) > 0.5f) moving++;
			Logger::Log("Cloth wind: skeleton of '%s' (%u bones%s): %s; hips z %.1f, knees z %.1f, ankles z %.1f %.1f, leg radii %s %.1f %.1f %.1f / %.1f %.1f %.1f, elbows %s%s%s; %u of %u vertices (kept) move > half",
				geometry->m_pcName ? geometry->m_pcName : "?", sd->Bones, k.actor ? ", human" : "", k.ok ? "used" : (k.actor ? "NOT used: no legs, stays still" : "NOT used (no upright legs: SkinMask)"),
				k.hipZ, k.kneeZ, k.pts[2][2], k.pts[6][2], k.measured ? "measured" : "fixed", k.pts[0][3], k.pts[1][3], k.pts[2][3], k.pts[4][3], k.pts[5][3], k.pts[6][3],
				found[8] && found[10] ? "found" : "missing", found[2] && found[6] ? "" : " (ankles guessed)", found[3] && found[7] ? "" : " (toes guessed)", moving, total);
		}
		return Skeletons[sd] = k;
	}

	// 2026-10-19 round 5 (user: 'how can we detect what's part of the cloth on a mesh intelligently?'): worn clothing by a PER-VERTEX
	// weight from the garment's own mesh (ClothWeights.h: trouser legs and boots found as cuts around one leg, the coat's weight pinned
	// at the hips, loosest at the hem, its shell and lining averaged together), worked out once per mesh. The armour vertex formats have
	// no spare channel, so the weight goes to the vertex shader as a SECOND VERTEX STREAM (one float per vertex, TEXCOORD7): one small
	// vertex buffer per skin partition (the partition draws its own vertices, in its vertex map's order), bound around the partition's
	// draw -- the game's NiDX9Renderer::Do_DrawPartition (0xE6D310) through its two calls in BSBatchRenderer::RenderPassImmediately_Skinned
	// (patched in Hooks.cpp only when the cloth code is compiled in), and NVR's own skinned shadow draw (RenderPass.cpp). The vertex
	// declaration for that draw = the partition's own + the stream-1 element; stream 1 and the declaration are put back right after it.
	// Mask 5 only when EVERY partition of the mesh has its buffer (else mask 4 for the whole mesh: no seams between partitions).
	struct PartBuf { const void* partition = nullptr; const UInt16* vertexMap = nullptr; UInt16 vertices = 0; UInt32 base = 0; IDirect3DVertexBuffer9* vb = nullptr; };
	struct MeshWeights {
		const void* skinData = nullptr; const void* skinPartition = nullptr; const void* partitions = nullptr; UInt32 partitionCount = 0;
		UInt16 vertices = 0;
		int state = 0;					// 0 not worked out yet, 1 weights made, -1 not possible (no kept vertices or triangles)
		bool buffers = false;			// every partition has its buffer
		std::vector<float> w;
		std::vector<PartBuf> parts;
		float center[2] = {};			// the wearer's upright axis in the bind pose (between the hip joints): mask 5's billow points away from it
		UInt32 lastFrame = 0;
		ClothWalk::Ring ring;			// 2026-10-20 round 9: the hem ring (bind pose); ring.u is each vertex's place around it (stream 1's second float)
	};
	inline std::unordered_map<const void*, MeshWeights> Weights;	// per NiGeometryData
	inline MeshWeights* ActiveWeights = nullptr;	// the mesh ForGeometry chose mask 5 for (until AfterDraw)
	inline float ActiveC248[4] = {};				// the c248 sent with it (a partition without its buffer draws with mask 4: see DrawPartition)
	inline bool DrawHookOn = false;					// Hooks.cpp patched the game's two partition draw calls
	inline UInt32 WeightsMadeNow = 0, WeightsLogged = 0;
	inline UInt32 StatWeighted = 0, StatFallback = 0, StatNoDecl = 0;	// partition draws with weights / with mask 4 / no declaration (per 600 frames)

	inline void ReleaseBuffers(MeshWeights& m) {
		for (PartBuf& p : m.parts) if (p.vb) { p.vb->Release(); p.vb = nullptr; }
		m.parts.clear(); m.buffers = false;
	}
	// A partition's weights as a vertex buffer: [base] zeros (the draw's BaseVertexIndex), then w[vertex map[i]]. 2026-10-20 round 9: three
	// floats a vertex -- the weight, the vertex's place around the hem ring (ClothWalk::Ring::u, 0..16) and its pendulum share (round 9b).
	inline bool FillPart(IDirect3DDevice9* device, const MeshWeights& m, PartBuf& p) {
		if (p.vb) { p.vb->Release(); p.vb = nullptr; }
		if (!p.vertexMap || !p.vertices || p.base > 65535) return false;
		const UINT bytes = (p.base + p.vertices) * 12;
		void* data = nullptr;
		if (FAILED(device->CreateVertexBuffer(bytes, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &p.vb, NULL)) || !p.vb) { p.vb = nullptr; return false; }
		if (FAILED(p.vb->Lock(0, 0, &data, 0)) || !data) { p.vb->Release(); p.vb = nullptr; return false; }
		float* f = (float*)data;
		for (UInt32 i = 0; i < p.base * 3; i++) f[i] = 0.0f;
		const bool ring = m.ring.u.size() == m.w.size() && m.ring.swing.size() == m.w.size();
		for (UInt32 i = 0; i < p.vertices; i++) {
			const UInt16 v = p.vertexMap[i];
			f[(p.base + i) * 3] = v < m.w.size() ? m.w[v] : 0.0f;
			f[(p.base + i) * 3 + 1] = ring && v < m.w.size() ? m.ring.u[v] : 0.0f;
			f[(p.base + i) * 3 + 2] = ring && v < m.w.size() ? m.ring.swing[v] : 0.0f;
		}
		p.vb->Unlock();
		return true;
	}
	// The mesh's triangles: the geometry data's own list (NiTriShapeData) or strips (NiTriStripsData) when the game kept them, else the
	// partitions' (their own indices, through each vertex map).
	inline const char* TrianglesOf(NiGeometry* geometry, const NiSkinPartition* sp, std::vector<UInt16>& tris) {
		tris.clear();
		const NiGeometryData* gd = geometry->geomData;
		const UInt16 nv = gd->m_usVertices;
		auto addStrip = [&](const UInt16* idx, UInt32 len, const UInt16* map, UInt32 mapCount) {
			for (UInt32 k = 0; k + 2 < len; k++) {
				UInt16 a = idx[k], b = idx[k + 1], c = idx[k + 2];
				if (map) { if (a >= mapCount || b >= mapCount || c >= mapCount) return; a = map[a]; b = map[b]; c = map[c]; }
				if (a == b || b == c || a == c || a >= nv || b >= nv || c >= nv) continue;
				if (k & 1) std::swap(b, c);
				tris.push_back(a); tris.push_back(b); tris.push_back(c);
			}
		};
		const UInt32 vt = *(const UInt32*)gd;
		if (vt == 0x109DAC4) {	// NiTriShapeData: m_usTriangles +0x40, m_pusTriList +0x48
			const UInt16 count = *(const UInt16*)((const char*)gd + 0x40);
			const UInt16* list = *(UInt16* const*)((const char*)gd + 0x48);
			if (list && count) {
				for (UInt32 i = 0; i < (UInt32)count * 3; i += 3)
					if (list[i] < nv && list[i + 1] < nv && list[i + 2] < nv) { tris.push_back(list[i]); tris.push_back(list[i + 1]); tris.push_back(list[i + 2]); }
				if (!tris.empty()) return "the mesh's triangle list";
			}
		}
		else if (vt == 0x109D8AC) {	// NiTriStripsData: m_usStrips +0x44, m_pusStripLengths +0x48, m_pusStripLists +0x4C
			const UInt16 strips = *(const UInt16*)((const char*)gd + 0x44);
			const UInt16* lengths = *(UInt16* const*)((const char*)gd + 0x48);
			const UInt16* lists = *(UInt16* const*)((const char*)gd + 0x4C);
			if (strips && lengths && lists) {
				UInt32 at = 0;
				for (UInt16 s = 0; s < strips; s++) { addStrip(lists + at, lengths[s], nullptr, 0); at += lengths[s]; }
				if (!tris.empty()) return "the mesh's strips";
			}
		}
		if (!sp || !sp->Partitions) return nullptr;
		for (UInt32 p = 0; p < sp->PartitionsCount; p++) {	// the partitions' own: m_pusVertexMap +0x0C, m_pusTriList +0x14, m_pusStripLengths +0x18
			const NiSkinPartition::Partition& part = sp->Partitions[p];
			const UInt16* map = *(UInt16* const*)((const char*)&part + 0x0C);
			const UInt16* list = *(UInt16* const*)((const char*)&part + 0x14);
			const UInt16* lengths = *(UInt16* const*)((const char*)&part + 0x18);
			if (!map || !list) continue;
			if (part.Strips && lengths) {
				UInt32 at = 0;
				for (UInt16 s = 0; s < part.Strips; s++) { addStrip(list + at, lengths[s], map, part.Vertices); at += lengths[s]; }
			}
			else
				for (UInt32 i = 0; i < (UInt32)part.Triangles * 3; i += 3) {
					const UInt16 a = list[i], b = list[i + 1], c = list[i + 2];
					if (a >= part.Vertices || b >= part.Vertices || c >= part.Vertices) continue;
					const UInt16 ma = map[a], mb = map[b], mc = map[c];
					if (ma < nv && mb < nv && mc < nv) { tris.push_back(ma); tris.push_back(mb); tris.push_back(mc); }
				}
		}
		return tris.empty() ? nullptr : "the skin partitions' triangles";
	}
	// The weights of a worn mesh with usable legs, made once (at most 2 new meshes a frame: a crowd coming into view spreads over a few
	// frames, those wait with mask 4); nullptr unless every partition has its buffer.
	// 2026-10-20: pre = the weights AutoDetect already worked out for this mesh on the worker (the same inputs): used as they are.
	inline MeshWeights* WeightsOf(IDirect3DDevice9* device, NiGeometry* geometry, const Skeleton& k, const ClothWeights::Result* pre = nullptr) {
		NiGeometryData* gd = geometry->geomData;
		const NiSkinInstance* si = geometry->skinInstance;
		const NiSkinPartition* sp = si ? si->SkinPartition : nullptr;
		if (!gd || !si || !sp || !sp->Partitions || !sp->PartitionsCount) return nullptr;
		MeshWeights& m = Weights[gd];
		if (m.state != 0 && (m.vertices != gd->m_usVertices || m.skinData != si->SkinData || m.skinPartition != sp || m.partitions != sp->Partitions || m.partitionCount != sp->PartitionsCount)) {
			ReleaseBuffers(m); m = MeshWeights();	// the data was freed and the memory reused for another mesh
		}
		m.lastFrame = FrameId;
		if (m.state < 0) return nullptr;
		if (m.state == 0) {
			const bool given = pre && pre->weight.size() == gd->m_usVertices && pre->label.size() == gd->m_usVertices;
			if (!given && WeightsMadeNow >= 2) return nullptr;
			if (!given) WeightsMadeNow++;
			m.vertices = gd->m_usVertices; m.skinData = si->SkinData; m.skinPartition = sp; m.partitions = sp->Partitions; m.partitionCount = sp->PartitionsCount;
			m.state = -1;
			std::vector<UInt16> tris;
			const char* from = given ? "the auto-detect measure (worker)" : ((gd->m_ucKeepFlags & NiGeometryData::KEEP_XYZ) && gd->m_pkVertex ? TrianglesOf(geometry, sp, tris) : nullptr);
			ClothWeights::Input in;
			ClothWeights::Result r;
			LARGE_INTEGER q0 = {}, q1 = {}, qf = {};
			QueryPerformanceCounter(&q0);
			if (given) { r = *pre; m.w = r.weight; m.state = 1; }
			else if (from) {
				in.pos = &gd->m_pkVertex[0].x; in.vertices = gd->m_usVertices; in.tris = tris.data(); in.triangles = (uint32_t)(tris.size() / 3);
				for (int leg = 0; leg < 2; leg++) for (int j = 0; j < 3; j++) for (int c = 0; c < 4; c++) in.legs[leg][j][c] = k.pts[leg * 4 + j][c];
				in.hipZ = k.hipZ; in.kneeZ = k.kneeZ; in.groundZ = k.groundZ;
				if (ClothWeights::Compute(in, r)) { m.w = std::move(r.weight); m.state = 1; }
			}
			m.center[0] = 0.5f * (k.pts[0][0] + k.pts[4][0]); m.center[1] = 0.5f * (k.pts[0][1] + k.pts[4][1]);
			if (m.state > 0)	// 2026-10-20 round 9: the hem ring (needs the labels; a mesh without coat directions has none: no walking simulation)
				ClothWalk::BuildRing(&gd->m_pkVertex[0].x, gd->m_usVertices, m.w.data(), r.label.data(), m.center, k.hipZ, m.ring);
			if (WeightsLogged < 24 || S.LogMatches) {
				WeightsLogged++;
				QueryPerformanceCounter(&q1); QueryPerformanceFrequency(&qf);
				const double ms = qf.QuadPart ? (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)qf.QuadPart : 0.0;
				if (m.state > 0)
					Logger::Log("Cloth wind: cloth weights of '%s': %u vertices (%u welded): body %u, cloth %u, torso %u; cuts %u (tubes %u, pads %u, cloth %u), crotch z %.1f; from %s, %u triangles; %.2f ms; hem ring %s",
						geometry->m_pcName ? geometry->m_pcName : "?", (unsigned)gd->m_usVertices, r.welded, r.body, r.cloth, r.torso, r.cuts, r.tubes, r.pads, r.clothCuts, r.crotch, from, (unsigned)(tris.size() / 3), ms,
						m.ring.ok ? "made" : "none (no coat below the hips)");
				else
					Logger::Log("Cloth wind: cloth weights of '%s': NOT made (%s): the skeleton mask (4) stays", geometry->m_pcName ? geometry->m_pcName : "?",
						!(gd->m_ucKeepFlags & NiGeometryData::KEEP_XYZ) || !gd->m_pkVertex ? "the game did not keep its vertices" : (!from ? "no triangles kept" : "the weights failed"));
			}
			if (m.state < 0) return nullptr;
		}
		if (!m.buffers) {
			if (!device) return nullptr;
			m.parts.assign(sp->PartitionsCount, PartBuf());
			bool all = true;
			for (UInt32 p = 0; p < sp->PartitionsCount; p++) {
				const NiSkinPartition::Partition& part = sp->Partitions[p];
				PartBuf& pb = m.parts[p];
				pb.partition = &part;
				pb.vertexMap = *(UInt16* const*)((const char*)&part + 0x0C);	// m_pusVertexMap
				pb.vertices = part.Vertices;
				pb.base = part.BuffData ? part.BuffData->BaseVertexIndex : 0;
				if (!FillPart(device, m, pb)) all = false;
			}
			if (!all) {
				if (WeightsLogged < 48 || S.LogMatches) { WeightsLogged++; Logger::Log("Cloth wind: cloth weights of '%s': partition buffers NOT made (vertex map missing?): the skeleton mask (4) stays", geometry->m_pcName ? geometry->m_pcName : "?"); }
				ReleaseBuffers(m); m.state = -1;
				return nullptr;
			}
			m.buffers = true;
		}
		return &m;
	}
	// The partition's vertex declaration + stream 1 (TEXCOORD7, one float). Cached per declaration, which is kept alive (AddRef) so its
	// address is never another declaration's while cached. Not for declarations that already use stream 1 or TEXCOORD7.
	struct DeclPair { IDirect3DVertexDeclaration9* orig; IDirect3DVertexDeclaration9* aug; };
	inline std::vector<DeclPair> Decls;
	inline IDirect3DVertexDeclaration9* Augmented(IDirect3DDevice9* device, IDirect3DVertexDeclaration9* orig) {
		if (!orig) return nullptr;
		for (const DeclPair& d : Decls) if (d.orig == orig) return d.aug;
		D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 2] = {};
		UINT n = MAXD3DDECLLENGTH + 1;
		IDirect3DVertexDeclaration9* aug = nullptr;
		if (SUCCEEDED(orig->GetDeclaration(el, &n)) && n >= 1 && n <= MAXD3DDECLLENGTH) {	// n counts the D3DDECL_END
			bool free = true;
			for (UINT i = 0; i + 1 < n; i++) if (el[i].Stream != 0 || (el[i].Usage == D3DDECLUSAGE_TEXCOORD && el[i].UsageIndex == 7)) free = false;
			if (free) {
				el[n - 1] = { 1, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 7 };	// weight, place on the hem ring, pendulum share
				el[n] = D3DDECL_END();
				if (FAILED(device->CreateVertexDeclaration(el, &aug))) aug = nullptr;
			}
		}
		orig->AddRef();
		Decls.push_back({ orig, aug });
		if (Decls.size() > 64) { if (Decls.front().aug) Decls.front().aug->Release(); Decls.front().orig->Release(); Decls.erase(Decls.begin()); }
		return aug;
	}
	// 2026-10-20 round 9 (user: 'can our cloth have some sort of faux physics simulation when we walk? right now it still kinda just
	// follows our player'): the garment's own skin weights glue a coat's skirt to the leg bones, so it moved like trousers (the old walking
	// part only added one offset for the whole coat). Now, per wearer, once a frame: the pelvis bone and the legs (thigh and calf bones
	// carrying the garment's own hip, knee and ankle points) are read from the skeleton, and the hem ring (ClothWalk.h: 16 points around
	// the wearer, each a damped spring towards its spot on the hips-hung coat, pushed out of the current legs, hanging from the hips) is
	// stepped on the play clock (a pause holds it; a jump of 400 units starts it again at rest). The shader then lets each coat vertex
	// HANG FROM THE HIPS (blends its skinned place towards the pelvis bone's alone by weight x hang), moves it by the ring at its angle x
	// weight, and keeps it out of the current legs. Hang fades out when a thigh bends more than 45..70 degrees off standing (sitting,
	// crouching), smoothed over 0.15 s. Everything is worked out in world space; DrawPartition carries it into the vertex shader's own
	// space by comparing the bone palette the game just uploaded with the same bone computed here (so it does not matter what space the
	// game skins into). c200-c202 pelvis, c203-c218 ring, c219 (on, hang), c220-c225 the current legs (xyz, radius).
	struct Walker {
		ClothWalk::State st;
		UInt32 frame = 0;
		double clock = 0.0;
		const void* data = nullptr;
		bool ok = false;
		double d[ClothWalk::kRing][3] = {};	// this frame's ring displacements (world)
		double pelvis[3][4] = {};			// the pelvis bone: bind pose -> world (rows r | t)
		double legs[6][4] = {};				// the current legs: L hip, knee, ankle, R hip, knee, ankle (world xyz, radius)
		double lastPos[3] = {};
		float hang = -1.0f;					// the smoothed hang factor
		int pelvisBone = -1;
	};
	inline std::unordered_map<const void*, Walker> Walkers;	// per worn geometry (each wearer has its own)
	inline Walker* ActiveWalk = nullptr;
	inline const NiSkinInstance* ActiveSkin = nullptr;
	inline int ActiveBonesBase = 44;		// where the vertex shader's bone palette starts: ObjectTemplate c44, Shadows\ShadowMap.vso c9
	inline UInt32 StatWalkDraws = 0, StatWalkNoSpace = 0, WalkChecksLogged = 0;

	// Skin bone b: bind (skin) space -> world, i.e. the bone's world transform after its SkinToBone (p' = s R p + t each), as rows r | t.
	inline bool BoneXf(const NiSkinInstance* si, int b, double out[3][4]) {
		const NiSkinData* sd = si ? si->SkinData : nullptr;
		if (b < 0 || !sd || !sd->BoneData || !si->BoneObjects || (UInt32)b >= sd->Bones || !si->BoneObjects[b]) return false;
		const NiTransform& W = si->BoneObjects[b]->m_worldTransform;
		const NiTransform& S = sd->BoneData[b].SkinToBone;
		const double sp[3] = { S.pos.x, S.pos.y, S.pos.z }, wp[3] = { W.pos.x, W.pos.y, W.pos.z };
		for (int i = 0; i < 3; i++) {
			for (int j = 0; j < 3; j++) {
				double acc = 0.0;
				for (int m = 0; m < 3; m++) acc += (double)W.rot.data[i][m] * S.rot.data[m][j];
				out[i][j] = acc * W.scale * S.scale;
			}
			out[i][3] = ((double)W.rot.data[i][0] * sp[0] + (double)W.rot.data[i][1] * sp[1] + (double)W.rot.data[i][2] * sp[2]) * W.scale + wp[i];
		}
		return true;
	}
	inline double Smooth01d(double x) { x = x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); return x * x * (3.0 - 2.0 * x); }

	// The wearer's walking simulation for this frame (worked out once a frame; the same numbers for every pass), or nullptr.
	inline Walker* WalkOf(NiGeometry* geometry, const Skeleton& k, const MeshWeights& mw) {
		const NiSkinInstance* si = geometry->skinInstance;
		if (!si || !mw.ring.ok) return nullptr;
		for (int b : k.bone) if (b < 0) return nullptr;
		Walker& w = Walkers[geometry];
		if (w.data != geometry->geomData) { w = Walker(); w.data = geometry->geomData; }
		if (w.frame == FrameId && w.ok) return &w;
		w.frame = FrameId; w.ok = false;
		double tl[3][4], cl[3][4], tr[3][4], cr[3][4];
		if (!BoneXf(si, k.bone[0], w.pelvis) || !BoneXf(si, k.bone[1], tl) || !BoneXf(si, k.bone[2], cl) || !BoneXf(si, k.bone[3], tr) || !BoneXf(si, k.bone[4], cr)) return nullptr;
		w.pelvisBone = k.bone[0];
		const double (*bone[6])[4] = { tl, tl, cl, tr, tr, cr };
		static const int joint[6] = { 0, 1, 2, 4, 5, 6 };
		for (int j = 0; j < 6; j++) {
			const float* q = k.pts[joint[j]];
			for (int i = 0; i < 3; i++) w.legs[j][i] = bone[j][i][0] * q[0] + bone[j][i][1] * q[1] + bone[j][i][2] * q[2] + bone[j][i][3];
			w.legs[j][3] = q[3];
		}
		const double dt = ClockSeconds - w.clock;
		w.clock = ClockSeconds;
		const double jx = w.pelvis[0][3] - w.lastPos[0], jy = w.pelvis[1][3] - w.lastPos[1], jz = w.pelvis[2][3] - w.lastPos[2];
		if (jx * jx + jy * jy + jz * jz > 400.0 * 400.0) w.st.valid = false;	// a load, a teleport: start again at rest
		for (int i = 0; i < 3; i++) w.lastPos[i] = w.pelvis[i][3];
		ClothWalk::Xf x;
		for (int i = 0; i < 3; i++) { for (int j = 0; j < 3; j++) x.r[i][j] = w.pelvis[i][j]; x.t[i] = w.pelvis[i][3]; }
		ClothWalk::Params prm;
		prm.spring = S.WalkSpring; prm.damping = S.WalkDamping; prm.drag = S.WalkDrag; prm.legMargin = S.WalkLegMargin; prm.maxSwing = S.WalkMaxSwing;
		prm.dragCap = S.WalkDragCap; prm.maxAngle = S.WalkMaxAngle; prm.hemSmooth = S.WalkHemSmooth;
		if (!w.st.valid || dt > 1e-5) ClothWalk::Step(w.st, mw.ring, x, w.legs, dt, prm, w.d);	// (no play time passed: hold)
		// hang fades out when BOTH thighs are bent far from standing (sitting, crouching, lying). 2026-10-20 round 9b: it was the more bent
		// thigh -- at a run the forward thigh passes 45 degrees every stride, so the coat switched between hanging and riding the legs
		double worst = -1.0;
		for (int leg = 0; leg < 2; leg++) {
			const float* hb = k.pts[leg * 4]; const float* kb = k.pts[leg * 4 + 1];
			const double rb[3] = { (double)kb[0] - hb[0], (double)kb[1] - hb[1], (double)kb[2] - hb[2] };
			double rest[3], cur[3];
			for (int i = 0; i < 3; i++) { rest[i] = w.pelvis[i][0] * rb[0] + w.pelvis[i][1] * rb[1] + w.pelvis[i][2] * rb[2]; cur[i] = w.legs[leg * 3 + 1][i] - w.legs[leg * 3][i]; }
			const double lr = std::sqrt(rest[0] * rest[0] + rest[1] * rest[1] + rest[2] * rest[2]), lc = std::sqrt(cur[0] * cur[0] + cur[1] * cur[1] + cur[2] * cur[2]);
			const double cosLeg = lr > 1e-6 && lc > 1e-6 ? (rest[0] * cur[0] + rest[1] * cur[1] + rest[2] * cur[2]) / (lr * lc) : 1.0;
			worst = (std::max)(worst, cosLeg);	// the LESS bent thigh decides
		}
		const double angle = std::acos(std::clamp(worst, -1.0, 1.0)) * 57.29578;
		const float target = S.WalkHang * (float)(1.0 - Smooth01d((angle - 45.0) / 25.0));
		if (w.hang < 0.0f || dt > 0.25) w.hang = target;
		else if (dt > 0.0) w.hang += (target - w.hang) * (float)(1.0 - std::exp(-dt / 0.15));
		w.ok = true;
		return &w;
	}
	// The walking constants for the partition about to be drawn, in the vertex shader's own space: T = (the palette's first bone as the
	// game just uploaded it) x (the same bone computed here, inverted) maps world to it; then pelvis, ring and legs go through T.
	inline bool WalkConstants(IDirect3DDevice9* device, const NiSkinPartition::Partition* part) {
		const Walker* w = ActiveWalk;
		if (!w || !ActiveSkin || !part || !part->pBones || !part->Bones) return false;
		double M[3][4];
		if (!BoneXf(ActiveSkin, part->pBones[0], M)) return false;
		float R[12];
		if (FAILED(device->GetVertexShaderConstantF(ActiveBonesBase, R, 3))) return false;
		const double a = M[0][0], b = M[0][1], c = M[0][2], d = M[1][0], e = M[1][1], f = M[1][2], g = M[2][0], h = M[2][1], i = M[2][2];
		const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
		if (std::fabs(det) < 1e-9) return false;
		const double inv[3][3] = { { (e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det },
								   { (f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det },
								   { (d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det } };
		double Mi[3][4];
		for (int r = 0; r < 3; r++) { for (int q = 0; q < 3; q++) Mi[r][q] = inv[r][q]; Mi[r][3] = -(inv[r][0] * M[0][3] + inv[r][1] * M[1][3] + inv[r][2] * M[2][3]); }
		double T[3][4];
		for (int r = 0; r < 3; r++) {
			for (int q = 0; q < 4; q++) T[r][q] = R[r * 4] * Mi[0][q] + R[r * 4 + 1] * Mi[1][q] + R[r * 4 + 2] * Mi[2][q];
			T[r][3] += R[r * 4 + 3];
		}
		const double scale = std::cbrt(std::fabs(T[0][0] * (T[1][1] * T[2][2] - T[1][2] * T[2][1]) - T[0][1] * (T[1][0] * T[2][2] - T[1][2] * T[2][0]) + T[0][2] * (T[1][0] * T[2][1] - T[1][1] * T[2][0])));
		float k[26][4] = {};
		for (int r = 0; r < 3; r++) {	// c200-c202: the pelvis bone, bind pose -> the shader's space
			for (int q = 0; q < 4; q++) k[r][q] = (float)(T[r][0] * w->pelvis[0][q] + T[r][1] * w->pelvis[1][q] + T[r][2] * w->pelvis[2][q]);
			k[r][3] += (float)T[r][3];
		}
		for (int n = 0; n < ClothWalk::kRing; n++)	// c203-c218: the ring's displacements (vectors)
			for (int r = 0; r < 3; r++) k[3 + n][r] = (float)(T[r][0] * w->d[n][0] + T[r][1] * w->d[n][1] + T[r][2] * w->d[n][2]);
		k[19][0] = 1.0f; k[19][1] = w->hang;	// c219
		for (int j = 0; j < 6; j++) {	// c220-c225: the current legs
			for (int r = 0; r < 3; r++) k[20 + j][r] = (float)(T[r][0] * w->legs[j][0] + T[r][1] * w->legs[j][1] + T[r][2] * w->legs[j][2] + T[r][3]);
			k[20 + j][3] = (float)(w->legs[j][3] * scale);
		}
		device->SetVertexShaderConstantF(200, &k[0][0], 26);
		if (S.LogMatches && WalkChecksLogged < 6) {	// the check that the space is right: the pelvis from the palette itself, when it is in it
			for (UInt16 sl = 0; sl < part->Bones; sl++) {
				if (part->pBones[sl] != w->pelvisBone) continue;
				float P[12];
				if (SUCCEEDED(device->GetVertexShaderConstantF(ActiveBonesBase + sl * 3, P, 3))) {
					float worst = 0.0f;
					for (int r = 0; r < 3; r++) for (int q = 0; q < 4; q++) worst = (std::max)(worst, std::fabs(P[r * 4 + q] - k[r][q]));
					WalkChecksLogged++;
					Logger::Log("Cloth wind: walk space check (%s pass): the pelvis from the bone palette vs computed: largest difference %.4f (translation %.1f %.1f %.1f), hang %.2f, ring point 0 off by %.1f %.1f %.1f",
						ActiveBonesBase == 9 ? "shadow" : "lit", worst, P[3], P[7], P[11], w->hang, k[3][0], k[3][1], k[3][2]);
				}
				break;
			}
		}
		return true;
	}

	// The per-partition draw (thiscall on the renderer: buffer data, partition, unused): with the mesh's weights in stream 1 while
	// ForGeometry chose mask 5 for it, else exactly the game's call.
	typedef void(__thiscall* DrawPartitionFn)(void*, NiGeometryBufferData*, NiSkinPartition::Partition*, void*);
	inline const DrawPartitionFn DrawPartitionOriginal = (DrawPartitionFn)0x00E6D310;
	inline void DrawPartition(void* renderer, NiGeometryBufferData* buffer, NiSkinPartition::Partition* partition, void* unused) {
		MeshWeights* m = ActiveWeights;
		IDirect3DDevice9* device = TheRenderManager ? TheRenderManager->device : nullptr;
		PipboyFit::ForPartition(device, partition);	// UNOFFICIAL 2026-11-03 PipboyFit: this partition's constants (or off)
		if (!S.CompiledIn) { DrawPartitionOriginal(renderer, buffer, partition, unused); return; }	// (the hook is on for the Pip-Boy fit only)
		if (!m || !device || !partition) { DrawPartitionOriginal(renderer, buffer, partition, unused); return; }
		PartBuf* pb = nullptr;
		for (PartBuf& p : m->parts) if (p.partition == partition) { pb = &p; break; }
		if (pb && buffer && buffer->BaseVertexIndex != pb->base) {	// the game moved the partition in its vertex buffer: follow it
			pb->base = buffer->BaseVertexIndex;
			if (!FillPart(device, *m, *pb)) pb = nullptr;
		}
		IDirect3DVertexDeclaration9* decl = nullptr;
		IDirect3DVertexDeclaration9* aug = nullptr;
		if (pb && pb->vb && buffer && buffer->StreamCount <= 1 && !buffer->FVF) {
			device->GetVertexDeclaration(&decl);
			aug = Augmented(device, decl);
		}
		if (!aug) {	// this partition draws with the skeleton mask (4)
			if (decl) decl->Release();
			if (pb) StatNoDecl++;
			StatFallback++;
			float c4[4] = { ActiveC248[0], 4.0f, ActiveC248[2], ActiveC248[3] };
			device->SetVertexShaderConstantF(248, c4, 1);
			DrawPartitionOriginal(renderer, buffer, partition, unused);
			device->SetVertexShaderConstantF(248, ActiveC248, 1);
			return;
		}
		if (ActiveWalk) {	// 2026-10-20 round 9: the walking constants in this draw's space (else this partition draws without the walk part)
			if (WalkConstants(device, partition)) StatWalkDraws++;
			else { StatWalkNoSpace++; const float off[4] = { 0.0f, 0.0f, 0.0f, 0.0f }; device->SetVertexShaderConstantF(219, off, 1); }
		}
		IDirect3DVertexBuffer9* old = nullptr; UINT oldOffset = 0, oldStride = 0;
		device->GetStreamSource(1, &old, &oldOffset, &oldStride);
		device->SetStreamSource(1, pb->vb, 0, 12);
		device->SetVertexDeclaration(aug);
		DrawPartitionOriginal(renderer, buffer, partition, unused);
		device->SetVertexDeclaration(decl);
		device->SetStreamSource(1, old, oldOffset, oldStride);
		if (old) old->Release();
		decl->Release();
		StatWeighted++;
	}
	inline void __fastcall DrawPartitionHook(void* renderer, void* edx, NiGeometryBufferData* buffer, NiSkinPartition::Partition* partition, void* unused) {
		DrawPartition(renderer, buffer, partition, unused);
	}

	// 2026-10-20 still cloth (mask 6; user: 'here the entire tent mesh wobbles around', 'only move thin parts of static cloth objects
	// instead of the whole thing'). The height mask (2) pinned still cloth at the top of its bound and moved it most at the BOTTOM: a
	// tent's poles, ropes and the canvas edge on the ground swung, a flag swung its pole. Now each still cloth mesh gets a per-vertex
	// weight from its own shape (ClothStatic.h: poles, ropes, pegs and other solid parts never move; the cloth is held where it touches
	// them, at the ground and along its top line, and moves most at its free edges; two-sided cloth moves as one), worked out ONCE per
	// mesh on a worker thread (the largest vanilla tent takes ~45 ms; the object stays still until its weights are ready, a frame or
	// two). Delivered as vertex stream 1 like the worn coats' (FLOAT3: weight, the billow's side, 0) around the game's still-object
	// draw: NiDX9Renderer::Do_OnlyRenderShape / Do_OnlyRenderTristrips (renderer vtable 0x10EE670 / 0x10EE674, called by the
	// geometry's OnlyRenderImmediate after the pass set the shaders, streams and declaration; Hooks.cpp patches the two slots) and NVR's
	// own shadow draw (RenderPass.cpp). A draw that cannot have its stream draws without cloth (c248 zero): never stale weights.
	// One piece of work for the cloth worker thread (still cloth weights, auto-detect measures): its own copies of the inputs.
	struct Job {
		virtual ~Job() {}
		virtual bool Run() = 0;
		bool ok = false;
		double ms = 0.0;
		std::atomic<bool> done{ false };
	};
	struct StillJob : Job {
		std::vector<float> pos, nrm;
		std::vector<uint16_t> tris;
		ClothStatic::Result r;
		bool Run() override {
			ClothStatic::Input in;
			in.pos = pos.data(); in.nrm = nrm.empty() ? nullptr : nrm.data(); in.vertices = (uint32_t)(pos.size() / 3);
			in.tris = tris.data(); in.triangles = (uint32_t)(tris.size() / 3);
			return ClothStatic::Compute(in, r);
		}
	};
	struct StillMesh {
		const void* data = nullptr; UInt16 vertices = 0;
		int state = 0;					// 0 new, 1 being worked out, 2 ready, -1 none (no kept vertices / triangles, or nothing moves)
		std::shared_ptr<StillJob> job;
		std::vector<float> w, side;
		IDirect3DVertexBuffer9* vb = nullptr; UInt32 base = 0;
		UInt32 lastFrame = 0;
		std::string name;
	};
	inline std::unordered_map<const void*, StillMesh> Stills;	// per NiGeometryData
	inline StillMesh* ActiveStill = nullptr;					// the mesh ForGeometry chose mask 6 for (until AfterDraw)
	inline bool StillHookOn = false;							// Hooks.cpp patched the two renderer slots
	inline UInt32 StillLogged = 0, StatStillDraws = 0, StatStillMissed = 0;
	struct ClothWorker { std::mutex m; std::condition_variable cv; std::deque<std::shared_ptr<Job>> q; bool started = false; };
	inline ClothWorker& Worker() { static ClothWorker* w = new ClothWorker(); return *w; }	// never freed: the thread may outlive the statics at exit
	inline void Submit(const std::shared_ptr<Job>& job) {
		ClothWorker& W = Worker();
		{
			std::lock_guard<std::mutex> lock(W.m);
			W.q.push_back(job);
			if (!W.started) {
				W.started = true;
				std::thread([] {
					ClothWorker& W = Worker();
					for (;;) {
						std::shared_ptr<Job> j;
						{
							std::unique_lock<std::mutex> lock(W.m);
							W.cv.wait(lock, [&] { return !W.q.empty(); });
							j = W.q.front(); W.q.pop_front();
						}
						LARGE_INTEGER q0 = {}, q1 = {}, qf = {};
						QueryPerformanceCounter(&q0);
						try { j->ok = j->Run(); }
						catch (...) { j->ok = false; }
						QueryPerformanceCounter(&q1); QueryPerformanceFrequency(&qf);
						j->ms = qf.QuadPart ? (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)qf.QuadPart : 0.0;
						j->done = true;
					}
				}).detach();
			}
		}
		W.cv.notify_one();
	}
	inline bool FillStill(IDirect3DDevice9* device, StillMesh& m, UInt32 base) {
		if (m.vb) { m.vb->Release(); m.vb = nullptr; }
		if (base > 262144 || !m.vertices) return false;	// (a mesh far into a shared buffer: 3 MB of leading zeros is the most)
		void* data = nullptr;
		if (FAILED(device->CreateVertexBuffer((base + m.vertices) * 12, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &m.vb, NULL)) || !m.vb) { m.vb = nullptr; return false; }
		if (FAILED(m.vb->Lock(0, 0, &data, 0)) || !data) { m.vb->Release(); m.vb = nullptr; return false; }
		float* f = (float*)data;
		memset(f, 0, base * 12);
		for (UInt32 i = 0; i < m.vertices; i++) {
			f[(base + i) * 3] = i < m.w.size() ? m.w[i] : 0.0f;
			f[(base + i) * 3 + 1] = i < m.side.size() ? m.side[i] : 1.0f;
			f[(base + i) * 3 + 2] = 0.0f;
		}
		m.vb->Unlock();
		m.base = base;
		return true;
	}
	// The still cloth weights of this mesh when ready, else nullptr (the first sight queues them; the mesh stays still meanwhile).
	inline StillMesh* StillOf(NiGeometry* geometry, const char* texture) {
		NiGeometryData* gd = geometry->geomData;
		if (!gd) return nullptr;
		StillMesh& m = Stills[gd];
		if (m.state != 0 && m.vertices != gd->m_usVertices) {	// the data was freed and the memory reused for another mesh
			if (m.vb) m.vb->Release();
			m = StillMesh();
		}
		m.lastFrame = FrameId;
		if (m.state == 0) {
			m.data = gd; m.vertices = gd->m_usVertices; m.state = -1;
			m.name = std::string(geometry->m_pcName ? geometry->m_pcName : "?") + "' (" + (texture ? texture : "");
			std::vector<UInt16> tris;
			if ((gd->m_ucKeepFlags & NiGeometryData::KEEP_XYZ) && gd->m_pkVertex && gd->m_usVertices >= 3 && TrianglesOf(geometry, nullptr, tris)) {
				auto job = std::make_shared<StillJob>();
				job->pos.assign(&gd->m_pkVertex[0].x, &gd->m_pkVertex[0].x + gd->m_usVertices * 3);
				if (gd->m_pkNormal) job->nrm.assign(&gd->m_pkNormal[0].x, &gd->m_pkNormal[0].x + gd->m_usVertices * 3);
				job->tris.assign(tris.begin(), tris.end());
				m.job = job; m.state = 1;
				Submit(job);
			}
			else if (StillLogged < 48 || S.LogMatches) { StillLogged++; Logger::Log("Cloth wind: still cloth '%s): NOT worked out (%s): it stays still", m.name.c_str(), !(gd->m_ucKeepFlags & NiGeometryData::KEEP_XYZ) || !gd->m_pkVertex ? "the game did not keep its vertices" : "no triangles kept"); }
		}
		if (m.state == 1 && m.job && m.job->done) {
			const ClothStatic::Result& r = m.job->r;
			UInt32 moving = 0;
			if (m.job->ok && r.weight.size() == m.vertices) { m.w = r.weight; m.side = r.side; for (float v : m.w) if (v > 0.02f) moving++; }
			m.state = moving ? 2 : -1;
			if (StillLogged < 48 || S.LogMatches) {
				StillLogged++;
				if (m.job->ok)
					Logger::Log("Cloth wind: still cloth '%s): %u vertices (%u welded): %u pieces = %u poles/ropes, %u solid, %u tiny, %u cloth (%u two-sided of %u groups); cloth held by poles/ropes %u, the ground %u (at z %.1f%s), its top %u (%u flat sheets tied all round); free edges %u; %u of %u vertices move, %u > half; %.2f ms on the worker",
						m.name.c_str(), (unsigned)m.vertices, r.welded, r.pieces, r.rods, r.solids, r.tiny, r.sheets, r.layered, r.clusters, r.heldRigid, r.heldGround, r.groundZ, r.standing ? "" : ", hanging: none",
						r.heldTop, r.canopies, r.freeEdges, moving, (unsigned)m.vertices, r.moving, m.job->ms);
				else Logger::Log("Cloth wind: still cloth '%s): NOT worked out (the weights failed): it stays still", m.name.c_str());
			}
			m.job.reset();
		}
		return m.state == 2 ? &m : nullptr;
	}
	// The still-object draw (thiscall on the renderer: the geometry): with the mesh's weights in stream 1 while ForGeometry chose mask
	// 6 for it, else exactly the game's call.
	typedef void(__thiscall* OnlyRenderFn)(void*, NiGeometry*);
	inline OnlyRenderFn OnlyRenderShapeOriginal = (OnlyRenderFn)0x00E745A0, OnlyRenderStripsOriginal = (OnlyRenderFn)0x00E74840;
#if NVR_TEST_TOOLS
	// 2026-10-21 diagnostic (LogMatches, test builds): one frame's every draw of a still cloth mesh with its cloth values and depth state
	// (indoors a Gomorrah curtain vanished with any wind at all: a draw that writes its depth unmoved?)
	inline UInt32 DiagFrame = 0, DiagLines = 0;
	inline void DiagStillDraw(IDirect3DDevice9* device, NiGeometry* geometry, const StillMesh* active) {
		if (!DiagFrame || FrameId < DiagFrame || FrameId > DiagFrame + 2 || DiagLines >= 900 || !device || !geometry || !geometry->geomData) return;
		auto it = Stills.find(geometry->geomData);
		if (it == Stills.end() || it->second.state != 2) return;
		DiagLines++;
		DWORD zenable = 0, zfunc = 0, zwrite = 0, colorwrite = 0, alphablend = 0, alphatest = 0;
		device->GetRenderState(D3DRS_ZENABLE, &zenable); device->GetRenderState(D3DRS_ZFUNC, &zfunc); device->GetRenderState(D3DRS_ZWRITEENABLE, &zwrite);
		device->GetRenderState(D3DRS_COLORWRITEENABLE, &colorwrite); device->GetRenderState(D3DRS_ALPHABLENDENABLE, &alphablend); device->GetRenderState(D3DRS_ALPHATESTENABLE, &alphatest);
		float c[4] = {};
		device->GetVertexShaderConstantF(248, c, 1);
		IDirect3DVertexShader9* vs = nullptr; device->GetVertexShader(&vs);
		IDirect3DSurface9* rt = nullptr; device->GetRenderTarget(0, &rt);
		Logger::Log("Cloth wind: still draw (frame %u) '%s): geometry %p, chosen %d, in a pass hook %d, c248 %.2f %.2f, z %u func %u write %u, colour write %X, blend %u, alpha test %u, VS %p %s (carries cloth %d), target %p | seen last frame %d, vanilla VS last frame %d",
			FrameId, it->second.name.c_str(), geometry, active == &it->second, PassGeometry == geometry, c[0], c[1], zenable, zfunc, zwrite, colorwrite, alphablend, alphatest, vs, CurrentVSName ? CurrentVSName : "?", CurrentVSCapable, rt,
			(int)SeenLast.count(geometry), (int)VanillaLast.count(geometry));
		if (vs) vs->Release();
		if (rt) rt->Release();
	}
#endif
	inline void OnlyRender(bool strips, void* renderer, NiGeometry* geometry) {
		const OnlyRenderFn original = strips ? OnlyRenderStripsOriginal : OnlyRenderShapeOriginal;
		StillMesh* m = ActiveStill;
		IDirect3DDevice9* device = TheRenderManager ? TheRenderManager->device : nullptr;
#if NVR_TEST_TOOLS
		DiagStillDraw(device, geometry, m);
#endif
		if (!m || !device || !geometry || geometry->geomData != m->data) { original(renderer, geometry); return; }
		const NiGeometryBufferData* buffer = geometry->geomData->m_pkBuffData;
		IDirect3DVertexDeclaration9* decl = nullptr;
		IDirect3DVertexDeclaration9* aug = nullptr;
		bool ready = buffer && buffer->VertCount == m->vertices && buffer->StreamCount <= 1 && !buffer->FVF;
		if (ready && (!m->vb || buffer->BaseVertexIndex != m->base)) ready = FillStill(device, *m, buffer->BaseVertexIndex);	// (the game moved it in its buffer: follow)
		if (ready) { device->GetVertexDeclaration(&decl); aug = Augmented(device, decl); }
		if (!aug) {	// no stream for this draw: it draws without cloth
			if (decl) decl->Release();
			StatStillMissed++;
			float c[4] = {}, zero[4] = {};
			device->GetVertexShaderConstantF(248, c, 1);
			device->SetVertexShaderConstantF(248, zero, 1);
			original(renderer, geometry);
			device->SetVertexShaderConstantF(248, c, 1);
			return;
		}
		IDirect3DVertexBuffer9* old = nullptr; UINT oldOffset = 0, oldStride = 0;
		device->GetStreamSource(1, &old, &oldOffset, &oldStride);
		device->SetStreamSource(1, m->vb, 0, 12);
		device->SetVertexDeclaration(aug);
		original(renderer, geometry);
		device->SetVertexDeclaration(decl);
		device->SetStreamSource(1, old, oldOffset, oldStride);
		if (old) old->Release();
		decl->Release();
		StatStillDraws++;
	}
	inline void __fastcall OnlyRenderShapeHook(void* renderer, void* edx, NiGeometry* geometry) { OnlyRender(false, renderer, geometry); }
	inline void __fastcall OnlyRenderStripsHook(void* renderer, void* edx, NiGeometry* geometry) { OnlyRender(true, renderer, geometry); }

	// 2026-10-20 AutoDetect (user: 'expanding our clothing physics shader to other vanilla armors and potentially modded ones (idk how we'd
	// detect they'd benefit from the physics)'): a worn garment that no keyword names moves when its own mesh HANGS FREE of the legs --
	// a coat, a duster, a robe, a dress -- and trousers, power armour and tight suits stay still. Per garment, once: the cheap look first
	// (ClothWeights::FreeShare: does any of it stand off the legs at all, ~20 us), then the full per-vertex weights on the worker thread
	// (ClothWeights::Compute, 1-4 ms) and the verdict (ClothWeights::HangsFree: enough of it, hanging from the waist, down below the
	// crotch); a garment that hangs free keeps those weights (WeightsOf uses them: no second computation). Cached per texture + shape
	// name + vertex count for the whole session (every wearer of an armour shares it), logged once each (LogAuto). Keywords still force
	// a garment on, ForceOff forces it off. Survey of every vanilla, DLC and TTW armour: bench\deep-roadmap\evidence\clothauto.
	struct AutoJob : Job {
		std::vector<float> pos;
		std::vector<uint16_t> tris;
		ClothWeights::Input in;
		ClothWeights::Result r;
		bool Run() override {
			in.pos = pos.data(); in.vertices = (uint32_t)(pos.size() / 3); in.tris = tris.data(); in.triangles = (uint32_t)(tris.size() / 3);
			return ClothWeights::Compute(in, r);
		}
	};
	struct Verdict {
		int state = 0;					// 0 new, 1 measuring on the worker, 2 hangs free (cloth), -1 still
		UInt16 vertices = 0;
		float free = 0.0f;
		std::shared_ptr<AutoJob> job;
		ClothWeights::Result r;			// kept when it hangs free (its weights)
		std::string name;
	};
	inline std::unordered_map<std::string, Verdict> Verdicts;		// texture | shape | vertices
	struct VerdictRef { UInt16 vertices = 0; const void* texture = nullptr; Verdict* v = nullptr; };
	inline std::unordered_map<const void*, VerdictRef> VerdictOfData;	// per NiGeometryData: its entry above
	inline UInt32 AutoLogged = 0;
	inline float AutoShareUsed = -1.0f;
	inline void LogVerdict(const Verdict& v, const char* why) {
		if (!S.LogAuto || AutoLogged >= 400) return;
		AutoLogged++;
		const ClothWeights::Result& r = v.state == 2 ? v.r : (v.job ? v.job->r : v.r);
		if (v.state == 2)
			Logger::Log("Cloth wind: auto: CLOTH %s: hangs free -- %.0f %% of its thigh band stands off the legs, %u of %u vertices move (%.1f %%), down to %.0f units below the crotch; %.2f ms on the worker",
				v.name.c_str(), v.free * 100.0f, r.moving, r.welded, r.welded ? 100.0 * r.moving / r.welded : 0.0, r.hemDrop, v.job ? v.job->ms : 0.0);
		else if (v.job && v.job->ok)
			Logger::Log("Cloth wind: auto: still %s: %s -- %.0f %% of its thigh band stands off the legs, %u of %u vertices would move (%.1f %%), down to %.0f units below the crotch",
				v.name.c_str(), why, v.free * 100.0f, r.moving, r.welded, r.welded ? 100.0 * r.moving / r.welded : 0.0, r.hemDrop);
		else Logger::Log("Cloth wind: auto: still %s: %s", v.name.c_str(), why);
	}
	// The verdict for this worn mesh (nullptr: not a garment AutoDetect looks at). Starts the measure on first sight; until it is done
	// the garment stays still.
	inline const Verdict* AutoVerdictOf(NiGeometry* geometry, const NiSourceTexture* diffuse) {
		NiGeometryData* gd = geometry->geomData;
		if (!gd || !diffuse || !diffuse->ddsPath1) return nullptr;
		Verdict* v = nullptr;
		auto it = VerdictOfData.find(gd);
		if (it != VerdictOfData.end() && it->second.vertices == gd->m_usVertices && it->second.texture == diffuse) v = it->second.v;
		else {
			if (VerdictOfData.size() >= 4096) VerdictOfData.clear();
			const std::string path = Norm(diffuse->ddsPath1);
			const std::string key = path + "|" + (geometry->m_pcName ? geometry->m_pcName : "") + "|" + std::to_string(gd->m_usVertices);
			v = &Verdicts[key];
			VerdictOfData[gd] = { gd->m_usVertices, diffuse, v };
			if (v->state == 0) {
				v->vertices = gd->m_usVertices;
				v->name = std::string("'") + (geometry->m_pcName ? geometry->m_pcName : "?") + "' (" + diffuse->ddsPath1 + ", " + std::to_string(gd->m_usVertices) + " vertices)";
				v->state = -1;
				const Skeleton& k = SkeletonOf(geometry);
				std::vector<UInt16> tris;
				const NiSkinPartition* sp = geometry->skinInstance ? geometry->skinInstance->SkinPartition : nullptr;
				if (IsForcedOff(path)) LogVerdict(*v, "ForceOff");
				else if (ExcludedPart(path)) LogVerdict(*v, "its file name is a body part (ExcludeParts)");
				else if (!k.ok) LogVerdict(*v, k.actor ? "no legs in its skeleton" : "not on a human skeleton");
				else if (!(gd->m_ucKeepFlags & NiGeometryData::KEEP_XYZ) || !gd->m_pkVertex || !TrianglesOf(geometry, sp, tris)) LogVerdict(*v, "the game did not keep its vertices or triangles");
				else {
					auto job = std::make_shared<AutoJob>();
					job->pos.assign(&gd->m_pkVertex[0].x, &gd->m_pkVertex[0].x + gd->m_usVertices * 3);
					job->tris.assign(tris.begin(), tris.end());
					for (int leg = 0; leg < 2; leg++) for (int j = 0; j < 3; j++) for (int c = 0; c < 4; c++) job->in.legs[leg][j][c] = k.pts[leg * 4 + j][c];
					job->in.hipZ = k.hipZ; job->in.kneeZ = k.kneeZ; job->in.groundZ = k.groundZ;
					job->in.pos = job->pos.data(); job->in.vertices = gd->m_usVertices;
					v->free = ClothWeights::FreeShare(job->in);
					if (v->free < (float)ClothWeights::kFreeFirst) {
					char why[96];
					snprintf(why, sizeof(why), "it hugs the legs (%.0f %% of its thigh band stands off them)", v->free * 100.0f);
					LogVerdict(*v, why);
				}
					else { v->job = job; v->state = 1; Submit(job); }
				}
			}
		}
		if (v->state == 1 && v->job && v->job->done) {
			const char* why = "the weights failed";
			if (v->job->ok) {
				const ClothWeights::Result& r = v->job->r;
				if (ClothWeights::HangsFree(r, S.AutoShare * 0.01)) { v->r = r; v->state = 2; why = ""; }
				else why = r.moving < ClothWeights::kAutoMoving || r.moving < S.AutoShare * 0.01 * r.welded ? "too little of it hangs free" : "it moves only at the hips";
			}
			if (v->state != 2) v->state = -1;
			LogVerdict(*v, why);
			v->job.reset();
		}
		return v;
	}

	// Once a frame (Render.cpp, image space): the settings (live), the wind and the phases.
	inline void AdvanceClock() {
		TimeGlobals* g = TimeGlobals::Get();
		if (!g || !g->GameHour) return;
		const float hour = g->GameHour->data;
		const double scale = g->TimeScale && g->TimeScale->data > 0.0f ? (double)g->TimeScale->data : 30.0;
		if (LastHour >= 0.0f) {
			double dh = (double)hour - (double)LastHour;
			if (dh < -12.0) dh += 24.0;		// past midnight
			if (dh > 0.0) ClockSeconds += (std::min)(dh * 3600.0 / scale, 0.1);	// a wait or sleep moves on by at most a frame's worth
		}
		LastHour = hour;
	}

	inline void BeginFrame(const Sky* sky, bool exterior) {
		// 2026-10-21 (testers: still cloth indoors 'moves around too much'): indoors cloth moves only with the indoor set on, and with its
		// own values ([Shaders.Cloth.Interiors]). It took over [Shaders.Cloth.Main] Interiors, which moved it with the outdoor ones (a
		// saved 'true' carries over: SettingManager's UpdateOldDefaults, version 67).
		Interior = !exterior;
		IndoorWind = Interior && TheSettingManager->GetSettingI("Shaders.Cloth.Interiors", "Enabled") != 0;
		Exterior = exterior || IndoorWind;	// (named Exterior: 'cloth may move here')
		// 2026-10-19: objects whose passes the game drew without setting shaders (not noted by SetShadersHook) count by the shader bound then
		for (const auto& [g, capable] : TentativeNow)
			if (!SeenNow.count(g)) { SeenNow.insert(g); if (!capable) VanillaNow.insert(g); }
		TentativeNow.clear();
		SeenLast.swap(SeenNow); SeenNow.clear(); VanillaLast.swap(VanillaNow); VanillaNow.clear();
		VanillaVSLast.swap(VanillaVSNow); VanillaVSNow.clear();
		// The shaders were compiled with Enabled as it was at startup (CLOTH_WIND_ON, ShaderRecord::BuildDefines): read once.
		if (!CompiledInRead) { CompiledInRead = true; S.CompiledIn = TheSettingManager->GetSettingI("Shaders.Cloth.Main", "Enabled") != 0; }
		S.On = S.CompiledIn && TheSettingManager->GetSettingI("Shaders.Cloth.Main", "Enabled");
		if (!S.On) return;
		const char* section = "Shaders.Cloth.Main";
		S.MaskMode = std::clamp(TheSettingManager->GetSettingI(section, "MaskMode"), 0, 2);
		S.HeightTop = TheSettingManager->GetSettingF(section, "HeightTop");
		S.HeightBottom = TheSettingManager->GetSettingF(section, "HeightBottom");
		S.HeightFromFeet = TheSettingManager->GetSettingI(section, "HeightFromFeet") != 0;
		S.UseGameWind = TheSettingManager->GetSettingI(section, "UseGameWind") != 0;
		S.Dir[0] = TheSettingManager->GetSettingF(section, "WindDirX"); S.Dir[1] = TheSettingManager->GetSettingF(section, "WindDirY");
		S.Dir[2] = TheSettingManager->GetSettingF(section, "WindDirZ");
		S.Speed = (std::max)(TheSettingManager->GetSettingF(section, "WindSpeed"), 0.0f);
		S.Intensity = (std::max)(TheSettingManager->GetSettingF(section, "WindIntensity"), 0.0f);
		S.Wavelength = (std::max)(TheSettingManager->GetSettingF(section, "Wavelength"), 16.0f);
		S.Flutter = (std::max)(TheSettingManager->GetSettingF(section, "FlutterSpeed"), 0.0f);
		S.FlutterAmount = std::clamp(TheSettingManager->GetSettingF(section, "FlutterAmount"), 0.0f, 1.0f);
		S.Ripple = std::clamp(TheSettingManager->GetSettingF(section, "RippleAmount"), 0.0f, 1.0f);
		S.RippleScale = (std::max)(TheSettingManager->GetSettingF(section, "RippleScale"), 0.0f);
		// (the whole text: GetSettingS goes through the node's 80-byte Value, which a longer list overflowed -- the game crashed)
		// 2026-10-19 (user: 'toggled it, restarted, couldn't see much'): a settings file saved before 2026-10-18 holds the OLD default
		// keyword list word for word (the defaults are written back into it), which names none of the vanilla coats and dusters, so
		// only flags and tarps moved. Exactly that old list now means 'the current default'.
		auto readList = [&](const char* key, std::string& from, std::vector<std::string>& out, const char* oldDefault) {
			SettingManager::Configuration::ConfigNode node;
			if (!TheSettingManager->Config.FillNode(&node, section, key)) return;
			std::string text = node.LongValue;
			if (oldDefault && text == oldDefault) {
				try { text = TheSettingManager->Config.DefaultConfig.at("_Shaders").at("Cloth").at("Main").at(key).as_string(); }
				catch (...) {}
			}
			if (from != text) { from = text; SplitList(text.c_str(), out); ByTexture.clear(); }
		};
		readList("Keywords", WordsFrom, Words, "cape,cloak,duster,flag,banner,curtain,drape,tarp,poncho,robe,scarf,tent");
		readList("Exclude", ExcludeFrom, Excluded, nullptr);
		readList("ExcludeParts", ExcludePartsFrom, ExcludedParts, nullptr);
		const std::string forceOffWas = ForceOffFrom;
		readList("ForceOff", ForceOffFrom, ForcedOff, nullptr);	// 2026-10-20
		if (ForceOffFrom != forceOffWas) { VerdictOfData.clear(); Verdicts.clear(); AutoLogged = 0; }	// AutoDetect judges again
		S.MotionDrag = std::clamp(TheSettingManager->GetSettingF(section, "MotionDrag"), 0.0f, 0.3f);
		S.MotionMax = std::clamp(TheSettingManager->GetSettingF(section, "MotionMax"), 0.0f, 80.0f);
		S.MotionSpring = std::clamp(TheSettingManager->GetSettingF(section, "MotionSpring"), 0.2f, 6.0f);
		S.MotionDamping = std::clamp(TheSettingManager->GetSettingF(section, "MotionDamping"), 0.05f, 1.0f);
		S.StrideSwing = std::clamp(TheSettingManager->GetSettingF(section, "StrideSwing"), 0.0f, 30.0f);
		S.StrideLength = std::clamp(TheSettingManager->GetSettingF(section, "StrideLength"), 40.0f, 600.0f);
		S.TurnLag = std::clamp(TheSettingManager->GetSettingF(section, "TurnLag"), 0.0f, 0.5f);
		S.MotionIndoors = TheSettingManager->GetSettingI(section, "MotionIndoors") != 0;
		S.UseSkeleton = TheSettingManager->GetSettingI(section, "UseSkeleton") != 0;
		S.UseWeights = TheSettingManager->GetSettingI(section, "UseWeights") != 0;
		S.WalkSim = TheSettingManager->GetSettingI(section, "WalkSim") != 0;
		S.WalkHang = std::clamp(TheSettingManager->GetSettingF(section, "WalkHang"), 0.0f, 1.0f);
		S.WalkSpring = std::clamp(TheSettingManager->GetSettingF(section, "WalkSpring"), 0.3f, 6.0f);
		S.WalkDamping = std::clamp(TheSettingManager->GetSettingF(section, "WalkDamping"), 0.02f, 1.0f);
		S.WalkDrag = std::clamp(TheSettingManager->GetSettingF(section, "WalkDrag"), 0.0f, 1.0f);
		S.WalkLegMargin = std::clamp(TheSettingManager->GetSettingF(section, "WalkLegMargin"), 0.0f, 10.0f);
		S.WalkMaxSwing = std::clamp(TheSettingManager->GetSettingF(section, "WalkMaxSwing"), 0.0f, 80.0f);
		S.WalkDragCap = std::clamp(TheSettingManager->GetSettingF(section, "WalkDragCap"), 20.0f, 2000.0f);
		S.WalkMaxAngle = std::clamp(TheSettingManager->GetSettingF(section, "WalkMaxAngle"), 5.0f, 89.0f);
		S.WalkHemSmooth = std::clamp(TheSettingManager->GetSettingF(section, "WalkHemSmooth"), 0.0f, 1.0f);
		S.StillWeights = TheSettingManager->GetSettingI(section, "StillWeights") != 0;
		S.AutoDetect = TheSettingManager->GetSettingI(section, "AutoDetect") != 0;	// 2026-10-20 (see AutoVerdictOf)
		S.AutoShare = std::clamp(TheSettingManager->GetSettingF(section, "AutoShare"), 0.0f, 50.0f);
		S.LogAuto = TheSettingManager->GetSettingI(section, "LogAuto") != 0;
		if (S.AutoShare != AutoShareUsed) { AutoShareUsed = S.AutoShare; VerdictOfData.clear(); Verdicts.clear(); AutoLogged = 0; }	// judged again (the worker measures again)
		WeightsMadeNow = 0;
		S.LimbGap = std::clamp(TheSettingManager->GetSettingF(section, "LimbGap"), -5.0f, 30.0f);
		S.LimbFade = std::clamp(TheSettingManager->GetSettingF(section, "LimbFade"), 0.5f, 40.0f);
		FrameId++;
		S.SkinMask = std::clamp(TheSettingManager->GetSettingI(section, "SkinMask"), 0, 2);
		S.LegCut = std::clamp(TheSettingManager->GetSettingF(section, "LegCut"), 0.0f, 60.0f);
		S.PinFraction = std::clamp(TheSettingManager->GetSettingF(section, "PinFraction"), 0.0f, 0.9f);
		S.SkinSpace = std::clamp(TheSettingManager->GetSettingI(section, "SkinSpace"), 0, 2);
		S.CalmFactor = std::clamp(TheSettingManager->GetSettingF(section, "CalmFactor"), 0.0f, 1.7f);
		S.LogMatches = TheSettingManager->GetSettingI(section, "LogMatches") != 0;
#if NVR_TEST_TOOLS
		if (S.LogMatches && !DiagFrame) { DiagFrame = FrameId + 60; DiagLines = 0; }	// 2026-10-21 diagnostic (DiagStillDraw)
#endif
		float d[3] = { S.Dir[0], S.Dir[1], S.Dir[2] };
		float speed = S.Speed, intensity = S.Intensity;
		WavelengthNow = S.Wavelength; FlutterNow = S.Flutter; FlutterAmountNow = S.FlutterAmount; RippleNow = S.Ripple;
		if (IndoorWind) {
			// 2026-10-21 indoors: the indoor set alone. There is no weather wind indoors (the sky still holds the last exterior's), so
			// neither its strength nor its direction: a still draught along WindDirX/Y/Z.
			const char* in = "Shaders.Cloth.Interiors";
			S.In.Strength = std::clamp(TheSettingManager->GetSettingF(in, "Strength"), 0.0f, 4.0f);
			S.In.Intensity = std::clamp(TheSettingManager->GetSettingF(in, "WindIntensity"), 0.0f, 40.0f);
			S.In.Speed = std::clamp(TheSettingManager->GetSettingF(in, "WindSpeed"), 0.0f, 2000.0f);
			S.In.Wavelength = (std::max)(TheSettingManager->GetSettingF(in, "Wavelength"), 16.0f);
			S.In.Flutter = std::clamp(TheSettingManager->GetSettingF(in, "FlutterSpeed"), 0.0f, 20.0f);
			S.In.FlutterAmount = std::clamp(TheSettingManager->GetSettingF(in, "FlutterAmount"), 0.0f, 1.0f);
			S.In.Ripple = std::clamp(TheSettingManager->GetSettingF(in, "RippleAmount"), 0.0f, 1.0f);
			speed = S.In.Speed; intensity = S.In.Intensity * S.In.Strength;
			WavelengthNow = S.In.Wavelength; FlutterNow = S.In.Flutter; FlutterAmountNow = S.In.FlutterAmount; RippleNow = S.In.Ripple;
		}
		else if (S.UseGameWind && sky) {
			// The sky's wind (weather): direction as an angle around up, speed 0-1. Calm weather still moves cloth a little.
			const float a = sky->windDirection;
			d[0] = sinf(a); d[1] = cosf(a); d[2] = 0.0f;
			const float w = std::clamp(sky->windSpeed, 0.0f, 1.0f);
			speed *= 0.4f + 1.2f * w;
			intensity *= S.CalmFactor + (1.7f - S.CalmFactor) * w;	// (2026-10-18: calm was 0.3, i.e. 4 cm of movement in the desert: invisible)
			if (!LoggedGameWind) { LoggedGameWind = true; Logger::Log("Cloth wind: the game's wind, direction %.3f, speed %.3f", a, sky->windSpeed); }
		}
		const float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		for (int i = 0; i < 3; i++) WorldDir[i] = len > 1e-4f ? d[i] / len : (i == 0 ? 1.0f : 0.0f);
		SpeedNow = speed; IntensityNow = intensity;
		static bool wasIndoorWind = false;
		if (IndoorWind && !wasIndoorWind && IndoorLogs < 8) {	// (once per interior entered, for the first few)
			IndoorLogs++;
			Logger::Log("Cloth wind: indoors with the indoor set: %.2f units (WindIntensity %.2f x Strength %.2f), speed %.0f, wavelength %.0f, flutter %.2f Hz x %.2f, ripple %.2f, motion indoors %s",
				intensity, S.In.Intensity, S.In.Strength, speed, WavelengthNow, FlutterNow, FlutterAmountNow, RippleNow, S.MotionIndoors ? "on" : "off");
		}
		wasIndoorWind = IndoorWind;
		// The clock runs only while cloth is on (BeginFrame returns above when off); the phases are integrated.
		const double before = ClockSeconds;
		AdvanceClock();
		const double dt = ClockSeconds - before;
		S.TimeOffset = TheSettingManager->GetSettingF(section, "TimeOffset");
		Rate[0] = speed / WavelengthNow; Rate[1] = 0.061 * (0.5 + speed / 300.0); Rate[2] = FlutterNow; Rate[3] = FlutterNow * 1.31;
		for (int i = 0; i < 4; i++) {
			Cycles[i] += dt * Rate[i]; Cycles[i] -= floor(Cycles[i]);
			const double c = Cycles[i] + (double)S.TimeOffset * Rate[i];
			Phase[i] = (float)(c - floor(c));
		}
		if (RealStart < 0.0 && TheFrameRateManager) { RealStart = TheFrameRateManager->Time; ClockStart = ClockSeconds; }
		if (++Frames >= 600) {
			const double real = TheFrameRateManager ? TheFrameRateManager->Time - RealStart : 0.0;
			Logger::Log("Cloth wind: %.1f cloth draws per frame (%.1f worn); clock %.2f s of wind per second of real time (1 = normal play, 0 = paused)", ClothDraws / (float)Frames, SkinnedDrawsNow / (float)Frames, real > 0.0 ? (ClockSeconds - ClockStart) / real : 0.0);
			RealStart = TheFrameRateManager ? TheFrameRateManager->Time : -1.0; ClockStart = ClockSeconds;
			if (S.LogMatches) {
				for (const auto& [g, c] : ClothPassCounts)
					Logger::Log("Cloth wind: worn mesh '%s': lit passes moved %u, skipped %u (not seen by SetShaders) + %u (vertex shader without cloth code%s%s), shadow passes %u",
						c.name.c_str(), c.lit, c.notSeen, c.vanilla, VanillaVSLast.count(g) ? ": " : "", VanillaVSLast.count(g) ? VanillaVSLast.at(g).c_str() : "", c.shadow);
				ClothPassCounts.clear();
			}
			for (auto m = Motions.begin(); m != Motions.end();)	// wearers not drawn for a while (gone, or the node freed)
				m = FrameId - m->second.frame > 600 ? Motions.erase(m) : std::next(m);
			if (S.LogMatches || StatFallback) {
				Logger::Log("Cloth wind: weights: %u partition draws with weights, %u with the skeleton mask instead (%u without a declaration); %u meshes known",
					StatWeighted, StatFallback, StatNoDecl, (unsigned)Weights.size());
			}
			if (S.LogMatches && (StatWalkDraws || StatWalkNoSpace))
				Logger::Log("Cloth wind: walking: %u partition draws with the walk part, %u without (no space); %u wearers known", StatWalkDraws, StatWalkNoSpace, (unsigned)Walkers.size());
			StatWeighted = StatFallback = StatNoDecl = StatWalkDraws = StatWalkNoSpace = 0;
			for (auto w = Walkers.begin(); w != Walkers.end();)	// 2026-10-20 wearers not drawn for a while
				w = FrameId - w->second.frame > 600 ? Walkers.erase(w) : std::next(w);
			for (auto w = Weights.begin(); w != Weights.end();)	// 2026-10-19 meshes not drawn for a while: their buffers go
				if (FrameId - w->second.lastFrame > 1800) { ReleaseBuffers(w->second); w = Weights.erase(w); } else ++w;
			if (S.LogMatches && (StatStillDraws || StatStillMissed))
				Logger::Log("Cloth wind: still cloth: %u draws with weights, %u without (drawn still); %u meshes known", StatStillDraws, StatStillMissed, (unsigned)Stills.size());
			StatStillDraws = StatStillMissed = 0;
			for (auto m = Stills.begin(); m != Stills.end();)	// 2026-10-20 still cloth not drawn for a while (a job still running keeps its own copy)
				if (FrameId - m->second.lastFrame > 1800) { if (m->second.vb) m->second.vb->Release(); m = Stills.erase(m); } else ++m;
			Frames = ClothDraws = SkinnedDrawsNow = 0;
		}
	}

	inline void Zero(IDirect3DDevice9* device) {
		ActiveWeights = nullptr;	// 2026-10-19 round 5: no weights bound for draws that are no cloth
		ActiveWalk = nullptr; ActiveSkin = nullptr;
		ActiveStill = nullptr;		// 2026-10-20 still cloth (mask 6)
		if (!NonZeroSent || !device) return;
		static const float zero[4] = { 0, 0, 0, 0 };
		device->SetVertexShaderConstantF(248, zero, 1);
		NonZeroSent = false; GrassInC248 = false;
	}
	// 2026-10-19 (user: 'cloth is causing unintended meshes to flicker and distort violently'): right after each hooked draw. Before,
	// a cloth draw's values stayed in c248 until the next hooked object; anything drawn in between by another path with an NVR object
	// vertex shader moved as that cloth did -- harmless while cloth moved 5 cm, violent with the walking offset.
	// 2026-10-27 (Conor: grass sun shadows matched nothing): not when c248 holds a non-cloth shader's own data (GrassInC248: the grass
	// shaders' inverse projection, the particles' lamp colours). Zeroed after the first grass draw, the rest of the batch (the game sends
	// the constants once) rebuilt its positions from a broken matrix. NoteVertexShader clears it before the next cloth-carrying bind.
	// 2026-10-28 (Conor, grass sun shadows in streaks with cloth on: "still cooked, it's the cloth"): the same rule for ForGeometry's
	// returns on an object that is no cloth. It zeroed c248 at the start of every such pass, grass included, so with cloth compiled in
	// the grass shaders' inverse projection lost its first row (NVR may not send an unchanged constant again): the grass shadow casters
	// (GrassShadows), which place each tuft through it, came out squeezed into streaks. A cloth-carrying bind still clears it first
	// (NoteVertexShader), so no cloth code ever reads the grass values.
	inline void ZeroCloth(IDirect3DDevice9* device) {
		if (GrassInC248) { ActiveWeights = nullptr; ActiveWalk = nullptr; ActiveSkin = nullptr; ActiveStill = nullptr; return; }
		if (device) Zero(device);
	}
	inline void AfterDraw(IDirect3DDevice9* device) { ZeroCloth(device); }
	// ForGeometry's returns: the shadow maps bind their own vertex shaders (no NoteVertexShader to clear grass values first), so there c248
	// is always cleared, as before.
	inline void ZeroFor(IDirect3DDevice9* device, bool shadowPass) { if (shadowPass) Zero(device); else ZeroCloth(device); }

	// 2026-10-19 (user: 'make it more dramatic for our armor so we see the trench coat moving around when we walk'): worn clothing
	// follows its wearer -- the hem trails behind as they walk or run and swings past and settles when they stop (a damped spring),
	// swishes side to side with the steps, and lags behind a turn so the coat flares out. Worked out once per wearer per frame (every
	// pass of the frame gets the same numbers: see SAFETY) from the skin root's movement in play time (a pause holds it); a jump of
	// more than 400 units or a gap of more than a quarter second (a load, a teleport, not drawn for a while) starts it again at rest.
	// The shader adds offset + lag angle x (the point's distance from the wearer's upright axis) under the wind's height mask, so the
	// shoulders stay pinned and the boots still. The spring: semi-implicit Euler in steps of at most 1/120 s.
	inline const float* MotionOf(const NiAVObject* root) {
		Motion& m = Motions[root];
		if (m.valid && m.frame == FrameId) return m.out;
		const NiTransform& t = root->m_worldTransform;
		const float p[3] = { t.pos.x, t.pos.y, t.pos.z };
		const float yaw = atan2f(t.rot.data[1][0], t.rot.data[0][0]);	// the heading of the root's x axis in the world
		const double dt = ClockSeconds - m.clock;
		const float jump = sqrtf((p[0] - m.pos[0]) * (p[0] - m.pos[0]) + (p[1] - m.pos[1]) * (p[1] - m.pos[1]) + (p[2] - m.pos[2]) * (p[2] - m.pos[2]));
		if (!m.valid || dt > 0.25 || dt < 0.0 || jump > 400.0f) {
			m = Motion();
			m.valid = true; m.frame = FrameId; m.clock = ClockSeconds; m.yaw = yaw;
			for (int i = 0; i < 3; i++) m.pos[i] = p[i];
			return m.out;
		}
		m.frame = FrameId;
		if (dt < 1e-5) return m.out;	// no play time passed (paused): hold
		const float h = (float)dt, smooth = 1.0f - expf(-h / 0.1f);	// the velocity is smoothed over ~0.1 s (the animation's own bob)
		if (jump / h > 1500.0f) {	// 2026-10-19: faster than any run (a snap, a push, a moving platform's hand-over): start again at rest
			m = Motion();
			m.valid = true; m.frame = FrameId; m.clock = ClockSeconds; m.yaw = yaw;
			for (int i = 0; i < 3; i++) m.pos[i] = p[i];
			return m.out;
		}
		m.clock = ClockSeconds;
		for (int i = 0; i < 3; i++) { m.vel[i] += ((p[i] - m.pos[i]) / h - m.vel[i]) * smooth; m.pos[i] = p[i]; }
		float dy = yaw - m.yaw;
		if (dy > 3.14159265f) dy -= 6.2831853f; else if (dy < -3.14159265f) dy += 6.2831853f;
		m.yaw = yaw;
		m.yawRate += (dy / h - m.yawRate) * smooth;
		// targets: trail behind the movement (half as much up and down), Drag x speed at low speed, softly up to Max (a walk shows,
		// a sprint does not throw the coat over the legs); lag behind the turn
		float target[3] = { -m.vel[0], -m.vel[1], -m.vel[2] * 0.5f };
		const float tl = sqrtf(target[0] * target[0] + target[1] * target[1] + target[2] * target[2]);
		const float trail = S.MotionMax > 0.0f ? S.MotionMax * (1.0f - expf(-tl * S.MotionDrag / S.MotionMax)) : 0.0f;
		for (float& v : target) v = tl > 1e-3f ? v * trail / tl : 0.0f;
		const float turnTarget = t.rot.data[2][2] > 0.8f ? std::clamp(-m.yawRate * S.TurnLag, -0.5f, 0.5f) : 0.0f;	// (a tilted root: no turn)
		const float w = 6.2831853f * S.MotionSpring, k = w * w, c = 2.0f * S.MotionDamping * w;
		const int steps = (std::min)((int)ceilf(h * 120.0f), 30);
		const float s = h / steps;
		for (int n = 0; n < steps; n++) {
			for (int i = 0; i < 3; i++) { m.offsetVel[i] += (k * (target[i] - m.offset[i]) - c * m.offsetVel[i]) * s; m.offset[i] += m.offsetVel[i] * s; }
			m.turnVel += (k * (turnTarget - m.turn) - c * m.turnVel) * s; m.turn += m.turnVel * s;
		}
		const float ol = sqrtf(m.offset[0] * m.offset[0] + m.offset[1] * m.offset[1] + m.offset[2] * m.offset[2]), most = 1.5f * S.MotionMax + 1.0f;
		if (ol > most) for (float& v : m.offset) v *= most / ol;
		m.turn = std::clamp(m.turn, -0.6f, 0.6f);
		// the stride swish: across the movement, one cycle per two steps -- StrideLength at a run (~350/s, 1.4 a second), shorter
		// steps when walking (~0.65 a second at 100/s) -- stronger the faster (about half at a walk, full at a run)
		const float speed = sqrtf(m.vel[0] * m.vel[0] + m.vel[1] * m.vel[1]), pace = (std::min)(speed / 350.0f, 1.5f);
		m.stride += dt * speed / (S.StrideLength * (0.45f + 0.55f * (std::min)(pace, 1.0f))); m.stride -= floor(m.stride);
		const float swish = speed > 1.0f ? sinf(6.2831853f * (float)m.stride) * S.StrideSwing * sqrtf(pace) / speed : 0.0f;
		m.out[0] = m.offset[0] - m.vel[1] * swish;
		m.out[1] = m.offset[1] + m.vel[0] * swish;
		m.out[2] = m.offset[2];
		m.out[3] = m.turn;
		return m.out;
	}

	// Before an object is drawn (the game's passes and NVR's shadow passes): its cloth values, or strength 0.
	// 2026-10-19 (user: 'our first person view of the coat was heavily distorted, the mesh stretching in random directions'): nothing
	// moves in the first-person pass -- there the garment hangs on the first-person skeleton, posed around the camera, so the walking
	// offset and the turn swing tore it apart. (Its shadow still comes from the third-person body.)
	inline void ForGeometry(IDirect3DDevice9* device, NiGeometry* geometry, bool shadowPass = false, bool firstPerson = false) {
		if (!device) return;
		ActiveWeights = nullptr;
		// 2026-10-19: indoors (no wind) worn clothing still follows its wearer (MotionIndoors); everything else stays still there
		// (2026-10-21: unless the indoor set is on -- then Exterior is true and all cloth gets its gentle wind)
		if (!S.On || !geometry || firstPerson || (!Exterior && !S.MotionIndoors)) { ZeroFor(device, shadowPass); return; }
		// 2026-10-19 (P81 'only the player's SHADOW moves'; measured on NVRAutoTest12: the Combat Ranger coat's lit pass skipped 600 of 600
		// frames, 'not seen by SetShaders'): the game sets no shaders for a pass whose shaders are already bound (the coat follows its
		// Pip-Boy piece, same shaders), so SetShadersHook never notes it. Such a pass draws with the vertex shader bound now: judge it by
		// that one, and note it so the rule below holds for it from the next frame on (a vanilla one still keeps the object still).
		bool boundAnswer = false;
		if (!shadowPass) {
			auto t = TentativeNow.find(geometry);
			if (t == TentativeNow.end()) TentativeNow.emplace(geometry, CurrentVSCapable);
			else t->second = t->second && CurrentVSCapable;	// every pass of the object this frame
			// 2026-10-21 (indoor cloth test, Gomorrah: a curtain broke apart with any wind at all, even 0.02 units): an object whose
			// other passes SetShaders noted as carrying the cloth code, but with a pass drawn with a vertex shader that does not (the
			// game set no shaders for it: the lamp passes of a batch share theirs), counted as all-NVR -- the tentative answer above is
			// only used for objects never noted -- so its passes no longer met. Any lit pass drawn without the cloth code keeps the
			// object still from the next frame on.
			if (!CurrentVSCapable) VanillaNow.insert(geometry);
			boundAnswer = !SeenLast.count(geometry) && CurrentVSCapable && !VanillaLast.count(geometry);
		}
		if (!shadowPass && !boundAnswer && (!SeenLast.count(geometry) || VanillaLast.count(geometry))) {	// see SAFETY above
			if (S.LogMatches) {	// 2026-10-19 diagnostic: how often the safety rule skips a known worn cloth mesh, and why
				auto c = ClothPassCounts.find(geometry);
				if (c != ClothPassCounts.end()) { if (!SeenLast.count(geometry)) c->second.notSeen++; else c->second.vanilla++; }
			}
			ZeroFor(device, shadowPass); return;
		}
		if (shadowPass && VanillaLast.count(geometry)) { ZeroFor(device, shadowPass); return; }
		BSShaderProperty* property = (BSShaderProperty*)geometry->GetProperty(NiProperty::PropertyType::kType_Shade);
		const UInt32 vtable = property ? *(UInt32*)property : 0;
		if (vtable != 0x10AE0D0 && vtable != 0x10B9910) { ZeroFor(device, shadowPass); return; }	// BSShaderPPLightingProperty, Lighting30ShaderProperty
		BSShaderPPLightingProperty* lighting = (BSShaderPPLightingProperty*)property;
		const NiSourceTexture* diffuse = lighting->ppTextures[0] ? lighting->ppTextures[0][0] : nullptr;
		if (!diffuse || !diffuse->ddsPath1) { ZeroFor(device, shadowPass); return; }
		const bool skinned = geometry->skinInstance != nullptr;
		if (!Exterior && !skinned) { ZeroFor(device, shadowPass); return; }
		bool cloth;
		auto known = ByTexture.find(diffuse);
		if (known != ByTexture.end() && known->second.first == diffuse->ddsPath1) cloth = known->second.second;
		else {
			if (ByTexture.size() >= 4096) ByTexture.clear();	// 2026-10-17: entries are per texture object and were never dropped; the answer is worked out again on demand (same result)
			cloth = IsClothTexture(diffuse->ddsPath1); ByTexture[diffuse] = { std::string(diffuse->ddsPath1), cloth };
			if (cloth || (S.LogMatches && (skinned || LooksLikeCloth(diffuse->ddsPath1)))) {
				const NiGeometryData* gd = geometry->geomData;
				Logger::Log("Cloth wind: %s %s (%s '%s', %u vertices%s)", cloth ? "CLOTH" : "not cloth:", diffuse->ddsPath1, skinned ? "skinned" : "static",
					geometry->m_pcName ? geometry->m_pcName : "", gd ? (unsigned)gd->m_usVertices : 0u, gd && (gd->m_ucKeepFlags & NiGeometryData::KEEP_XYZ) ? ", vertices kept" : ", vertices not kept");
			}
		}
		// 2026-10-20 AutoDetect: a worn garment no keyword names moves when its own mesh hangs free (AutoVerdictOf); it brings its weights
		const Verdict* autoVerdict = nullptr;
		if (!cloth && skinned && S.AutoDetect && S.MaskMode == 2 && S.UseSkeleton && S.UseWeights && DrawHookOn) {
			autoVerdict = AutoVerdictOf(geometry, diffuse);
			if (autoVerdict && autoVerdict->state == 2) cloth = true; else autoVerdict = nullptr;
		}
		// The vertex colour is the mask: without colours in the mesh, only the whole-mesh mode applies.
		if (!cloth || (S.MaskMode == 0 && (!geometry->geomData || !geometry->geomData->m_pkColor))) { ZeroFor(device, shadowPass); return; }
		// Into the vertex shader's space: world for skinned meshes, the object's own for the rest.
		float dir[3] = { WorldDir[0], WorldDir[1], WorldDir[2] };
		float scale = 1.0f;
		float mode = (float)S.MaskMode;
		float top = 0.0f, invRange = 0.0f;	// mask 2 / 3, in the shader's space
		float origin[3] = { 0.0f, 0.0f, 0.0f };	// where the shader's space origin lies in the world (phases are shifted by it, so the pattern stays on the world)
		float motion[4] = { 0.0f, 0.0f, 0.0f, 0.0f }, turn[4] = { 0.0f, 0.0f, 0.0f, 0.0f };	// 2026-10-19 worn clothing: the wearer's movement (c252, c253)
		if (!skinned) {
			const NiTransform& w = geometry->m_worldTransform;
			scale = w.scale > 1e-4f ? w.scale : 1.0f;
			for (int i = 0; i < 3; i++) dir[i] = w.rot.data[0][i] * WorldDir[0] + w.rot.data[1][i] * WorldDir[1] + w.rot.data[2][i] * WorldDir[2];
			if (geometry->geomData) {	// (statics have no movement part: c252/c253 stay 0)	// hanging cloth: pinned at the top of its bound, loosest at the bottom (object space)
				const NiBound& b = geometry->geomData->m_kBound;
				top = b.Center.z + 0.8f * b.Radius; invRange = b.Radius > 1e-3f ? 1.0f / (1.6f * b.Radius) : 0.0f;
			}
			// 2026-10-20 still cloth by its own mesh (mask 6, StillOf): still until its weights are ready
			if (S.MaskMode == 2 && S.StillWeights && StillHookOn) {
				StillMesh* sm = StillOf(geometry, diffuse->ddsPath1);
				if (!sm) { ZeroFor(device, shadowPass); return; }
				mode = 6.0f; ActiveStill = sm;
			}
		}
		else {	// worn clothing
			const NiAVObject* root = geometry->skinInstance->RootParent ? geometry->skinInstance->RootParent : geometry;
			SkinnedDrawsNow++;
			if (S.LogMatches) {	// 2026-10-19 diagnostic (see ClothPassCounts)
				PassCounts& c = ClothPassCounts[geometry];
				if (c.name.empty()) c.name = geometry->m_pcName ? geometry->m_pcName : "?";
				if (shadowPass) c.shadow++; else c.lit++;
			}
			const Skeleton* skeleton = S.MaskMode == 2 && S.UseSkeleton ? &SkeletonOf(geometry) : nullptr;
			// 2026-10-19 (user: 'cloth is causing unintended meshes to flicker and distort'): a piece on a human skeleton without legs --
			// a Pip-Boy or spurs sharing the outfit's texture folder, gloves, a hat, first-person arms -- is no cloth: it stays still
			// (it got the old height mask and the walking offset before)
			if (skeleton && !skeleton->ok && skeleton->actor) { ZeroFor(device, shadowPass); return; }
			if (S.MaskMode != 2) {}	// mask 0 (vertex colour) and 1 (whole mesh) need no heights
			else if (skeleton && skeleton->ok) {	// 2026-10-19: the garment's own skeleton (SkeletonOf); c226-c239
				mode = 4.0f;
				float limbs[56] = {};
				memcpy(limbs, skeleton->pts, sizeof(skeleton->pts));
				limbs[48] = skeleton->hipZ; limbs[49] = 1.0f / (std::max)(skeleton->hipZ - skeleton->kneeZ, 1.0f);
				limbs[50] = S.LimbGap; limbs[51] = 1.0f / (std::max)(S.LimbFade, 0.1f);
				limbs[52] = skeleton->groundZ; limbs[53] = 1.0f / 6.0f;
				// 2026-10-19 round 5: the mesh's own per-vertex weights (mask 5) when every partition can have them; c239.zw = the
				// wearer's upright axis (bind pose), which mask 5's billow points away from
				if (MeshWeights* mw = S.UseWeights && DrawHookOn ? WeightsOf(device, geometry, *skeleton, autoVerdict ? &autoVerdict->r : nullptr) : nullptr) {
					mode = 5.0f; limbs[54] = mw->center[0]; limbs[55] = mw->center[1]; ActiveWeights = mw;
					// 2026-10-20 round 9: the walking simulation (DrawPartition sends its constants); without it c219.x = 0 (round 8's path)
					ActiveSkin = geometry->skinInstance; ActiveBonesBase = shadowPass ? 9 : 44;
					ActiveWalk = S.WalkSim && (!Interior || S.MotionIndoors) ? WalkOf(geometry, *skeleton, *mw) : nullptr;	// (2026-10-21: indoors only with MotionIndoors, as the trail below)
					if (!ActiveWalk) { const float off[4] = { 0.0f, 0.0f, 0.0f, 0.0f }; device->SetVertexShaderConstantF(219, off, 1); }
				}
				device->SetVertexShaderConstantF(226, limbs, 14);
			}
			else if (S.SkinMask == 1 && geometry->geomData) {
				// Actor meshes are authored with the feet at z 0 (checked on the vanilla Combat Ranger, Sheriff's duster and Patrol NIFs:
				// z 0..116): the bind-pose height IS the height above the feet. Mask as the settings say, legs and boots still.
				mode = 3.0f + S.LegCut * 0.001f;
				const float range = S.HeightTop > S.HeightBottom ? S.HeightTop - S.HeightBottom : 1.0f;
				top = S.HeightTop; invRange = 1.0f / range;
			}
			else if (S.SkinMask == 2 && geometry->geomData) {
				// 2026-10-18 (round 3): the mask is the garment's own BIND-POSE height (the shader reads the vertex before skinning): the top
				// PinFraction of the garment never moves, the hem moves most. Independent of the actor's pose and of the space the bones
				// put the vertices in -- round 2's actor-height mask (heights above the skin root) put the pin ~70 units too high.
				const Extent& e = ExtentOf(geometry);
				const float height = e.hi - e.lo > 1.0f ? e.hi - e.lo : 1.0f;
				mode = 3.0f;
				top = e.hi - S.PinFraction * height;
				invRange = 1.0f / ((1.0f - S.PinFraction) * height);
			}
			else {	// the old actor-height mask (SkinMask 0): from the waist down to near the ground
				const float range = S.HeightTop > S.HeightBottom ? S.HeightTop - S.HeightBottom : 1.0f;
				float rootAbove = 0.0f;
				if (S.HeightFromFeet) {
					const float d = root->m_worldTransform.pos.z - geometry->m_worldTransform.pos.z;
					if (d > -20.0f && d < 140.0f) rootAbove = d;
				}
				if (shadowPass || S.SkinSpace != 0) {
					top = root->m_worldTransform.pos.z - rootAbove + S.HeightTop - (S.SkinSpace == 1 ? (float)TheRenderManager->CameraPosition.z : 0.0f);
					invRange = 1.0f / range;
				}
				else {
					const NiTransform& w = root->m_worldTransform;
					scale = w.scale > 1e-4f ? w.scale : 1.0f;
					top = (S.HeightTop - rootAbove) / scale;
					invRange = scale / range;
				}
			}
			// The wind direction in the space the bones put the vertices in.
			if (S.SkinSpace == 0 && !shadowPass) {	// the skin root's space (round 2's assumption)
				const NiTransform& w = root->m_worldTransform;
				scale = w.scale > 1e-4f ? w.scale : 1.0f;
				for (int i = 0; i < 3; i++) dir[i] = w.rot.data[0][i] * WorldDir[0] + w.rot.data[1][i] * WorldDir[1] + w.rot.data[2][i] * WorldDir[2];
			}
			else if (S.SkinSpace == 1) { origin[0] = TheRenderManager->CameraPosition.x; origin[1] = TheRenderManager->CameraPosition.y; origin[2] = TheRenderManager->CameraPosition.z; }
			// 2026-10-19 the wearer's movement (MotionOf), in the same space: the offset, and the turn lag as a small turn of the garment's
			// BIND-POSE position around its own upright axis (garments are made centred on x = y = 0), carried into the space by the
			// wearer's heading: turn = bind.x * c253.xy + bind.y * c253.zw. (The first version turned the posed point around the root
			// relative to the camera: wrong wherever that camera is not the one the pass draws with.)
			// 2026-10-21: indoors with the indoor set on and MotionIndoors off, worn coats get its wind but do not follow their wearer
			// (before, Main's Interiors switch gave them both whatever MotionIndoors said)
			static const float still[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			const float* mo = !Interior || S.MotionIndoors ? MotionOf(root) : still;
			const float th = mo[3];
			if (S.SkinSpace == 0 && !shadowPass) {	// the skin root's space
				const NiTransform& w = root->m_worldTransform;
				for (int i = 0; i < 3; i++) motion[i] = (w.rot.data[0][i] * mo[0] + w.rot.data[1][i] * mo[1] + w.rot.data[2][i] * mo[2]) / scale;
				turn[1] = th; turn[2] = -th;
			}
			else {
				for (int i = 0; i < 3; i++) motion[i] = mo[i];
				const NiMatrix33& r = root->m_worldTransform.rot;	// the root's x and y axes in the world (its columns)
				turn[0] = th * r.data[0][1]; turn[1] = th * r.data[1][1]; turn[2] = -th * r.data[0][0]; turn[3] = -th * r.data[1][0];
			}
			if (S.LogMatches) {
				static int told = 0;
				if (told < 12) {
					told++;
					const NiSkinInstance* si = geometry->skinInstance;
					const float* bm = (const float*)si->BoneMatrixes;
					const float* sw = (const float*)si->SkinToWorldWorldToSkin;
					const Extent& e = ExtentOf(geometry);
					Logger::Log("Cloth wind: skin %s: root (%.0f %.0f %.0f) geometry (%.0f %.0f %.0f) camera (%.0f %.0f %.0f) bind z %.1f..%.1f%s bound c.z %.1f r %.1f",
						diffuse->ddsPath1, root->m_worldTransform.pos.x, root->m_worldTransform.pos.y, root->m_worldTransform.pos.z,
						geometry->m_worldTransform.pos.x, geometry->m_worldTransform.pos.y, geometry->m_worldTransform.pos.z,
						TheRenderManager->CameraPosition.x, TheRenderManager->CameraPosition.y, TheRenderManager->CameraPosition.z, e.lo, e.hi, e.fromVertices ? "" : " (bound)",
						geometry->geomData ? geometry->geomData->m_kBound.Center.z : 0.0f, geometry->geomData ? geometry->geomData->m_kBound.Radius : 0.0f);
					if (bm) Logger::Log("Cloth wind:   bone matrix 0 rows: %.2f %.2f %.2f %.1f | %.2f %.2f %.2f %.1f | %.2f %.2f %.2f %.1f", bm[0], bm[1], bm[2], bm[3], bm[4], bm[5], bm[6], bm[7], bm[8], bm[9], bm[10], bm[11]);
					if (sw) Logger::Log("Cloth wind:   skin-to-world: %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.1f %.1f %.1f %.2f", sw[0], sw[1], sw[2], sw[3], sw[4], sw[5], sw[6], sw[7], sw[8], sw[9], sw[10], sw[11], sw[12], sw[13], sw[14], sw[15]);
				}
			}
		}
		// Phases for this draw: the pattern is a function of the vertex's position in the shader's space; when that space's origin is
		// not the world's (camera-relative skinned meshes), shift the phases so the pattern stays where the wind put it.
		float ph[4] = { Phase[0], Phase[1], Phase[2], Phase[3] };
		if (origin[0] != 0.0f || origin[1] != 0.0f || origin[2] != 0.0f) {
			const double w = (double)scale / WavelengthNow, r = (double)S.RippleScale * scale;
			const double d0 = (origin[0] * dir[0] + origin[1] * dir[1] + origin[2] * dir[2]) * w;
			const double d1 = origin[0] * 0.0021 + origin[1] * 0.0027 + origin[2] * 0.0013;
			const double d2 = (origin[0] * 0.71 + origin[1] * 0.53 + origin[2] * 0.37) * r;
			const double d3 = (origin[0] * -0.43 + origin[1] * 0.81 + origin[2] * 0.59) * r * 1.7;
			auto wrap = [](double c) { return (float)(c - floor(c)); };
			ph[0] = wrap(Phase[0] - d0); ph[1] = wrap(Phase[1] + d1); ph[2] = wrap(Phase[2] + d2); ph[3] = wrap(Phase[3] - d3);
		}
		if (ActiveWalk) { for (float& v : motion) v = 0.0f; for (float& v : turn) v = 0.0f; }	// 2026-10-20: the hem ring replaces the one-offset trail
		const float c[24] = {
			1.0f, mode, RippleNow, FlutterAmountNow,	// (2026-10-21: Main's, or indoors the indoor set's)
			dir[0], dir[1], dir[2], scale / WavelengthNow,
			ph[0], ph[1], ph[2], ph[3],
			(Exterior ? IntensityNow : 0.0f) / scale, S.RippleScale * scale, top, invRange,	// (indoors without the indoor set: no wind, the movement part only)
			motion[0], motion[1], motion[2], 0.0f,
			turn[0], turn[1], turn[2], turn[3] };
		device->SetVertexShaderConstantF(248, c, 6);
		if (ActiveWeights) memcpy(ActiveC248, c, sizeof(ActiveC248));
		NonZeroSent = true; GrassInC248 = false;
		ClothDraws++;
	}
}
