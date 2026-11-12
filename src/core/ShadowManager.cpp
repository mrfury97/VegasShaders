#include "ShadowFaceCull.h"
#include "../effects/Puddles/PuddleMap.h"	// 2026-11-12 Puddles map mode
#include "PointShadowSchedule.h"
#include "PointShadowForward.h"
#include "TestTools.h"	// UNOFFICIAL: NVR_TEST_TOOLS (SkipWhenFadedCheck)
#include "OrthoProbe.h"	// UNOFFICIAL 2026-10-17 test tools: ortho map caster dump
#include "SunShadowStats.h"	// UNOFFICIAL 2026-10-21 test tools: where the sun cascades' CPU time goes
#include "../effects/CoverMap/CoverMap.h"	// UNOFFICIAL 2026-10-27 Persistent wetness and snow: CoverMapOrthoFrame
#include "DisplaceShadows.h"	// UNOFFICIAL 2026-10-27 displacement shadows: the land drawn with its bumps into the sun cascades
#include "ShadowAtlas.h"	// UNOFFICIAL 2026-10-30 ShadowAtlas (INTERIOR-SHADOWS-MODERN step 1)
#include "CapsuleShadows.h"	// UNOFFICIAL 2026-10-31 CapsuleShadows (steps 2-3): DropActors
#include "PipboyFit.h"	// UNOFFICIAL 2026-11-04 ViewModelShadows: its object cleared before the viewmodel map

// UNOFFICIAL SkinnedShadowFaceTest ([Main.Main.Performance], default on, 2026-10-07). An animated (skinned) mesh's own
// world bound is stale, so skinned casters used to be drawn into all six faces of every lamp cube map near them; with a
// crowd that is most of the cube-map work (FNVProbe crowd test: NVR's shadow maps 1.8 ms with 8 NPCs, 7.7 with 58, 39
// with 354). Their bound is now rebuilt each frame from the skeleton, the way Gamebryo's NiSkinInstance::UpdateModelBound
// (0x00A86530) does: each bone's sphere of the vertices it moves (NiSkinData::BoneData::Bound, bone space) is carried to
// the bone's current world transform and the spheres are merged. A skinned vertex is a weighted blend of its bones'
// transformed positions, so it lies inside that merged sphere: the faces it reaches are exactly the faces drawn before
// that it can touch, so the picture does not change. Padded a little for float safety; any missing bone or bound falls
// back to every face, as before.
struct SkinnedBoundEntry { UInt32 frame; bool ok; NiBound bound; };
static std::unordered_map<NiGeometry*, SkinnedBoundEntry> SkinnedBounds;
static UInt32 SkinnedBoundFrame = 0;	// advanced once per ShadowManager::RenderShadowMaps
static unsigned statSkinnedCasters = 0, statSkinnedFaces = 0, statSkinnedNoBound = 0, statSkinnedOutOfRange = 0;

static bool SkinnedWorldBound(NiGeometry* geometry, NiBound& out) {
	SkinnedBoundEntry& entry = SkinnedBounds[geometry];
	if (entry.frame == SkinnedBoundFrame) { out = entry.bound; return entry.ok; }
	entry.frame = SkinnedBoundFrame;
	entry.ok = false;
	NiSkinInstance* skin = geometry->skinInstance;
	NiSkinData* data = skin ? skin->SkinData : nullptr;
	if (!data || !data->BoneData || !skin->BoneObjects || data->Bones == 0) return false;
	D3DXVECTOR3 center(0.0f, 0.0f, 0.0f);
	float radius = -1.0f;
	for (UInt32 i = 0; i < data->Bones; i++) {
		NiAVObject* bone = skin->BoneObjects[i];
		if (!bone) return false;
		const NiTransform& t = bone->m_worldTransform;
		const NiBound& b = data->BoneData[i].Bound;
		if (!(b.Radius >= 0.0f && b.Radius < 1.0e6f) || !(t.scale > 0.0f && t.scale < 1.0e3f)) return false;
		const NiPoint3 scaled = { b.Center.x * t.scale, b.Center.y * t.scale, b.Center.z * t.scale };
		const NiPoint3 rotated = t.rot * scaled;
		const D3DXVECTOR3 c(rotated.x + t.pos.x, rotated.y + t.pos.y, rotated.z + t.pos.z);
		const float r = b.Radius * t.scale;
		if (radius < 0.0f) { center = c; radius = r; continue; }
		const D3DXVECTOR3 d = c - center;
		const float dist = D3DXVec3Length(&d);
		if (dist + r <= radius) continue;					// inside what we have
		if (dist + radius <= r) { center = c; radius = r; continue; }	// swallows what we have
		const float merged = (dist + radius + r) * 0.5f;
		center += d * ((merged - radius) / dist);
		radius = merged;
	}
	if (radius < 0.0f) return false;
	entry.bound.Center.x = center.x; entry.bound.Center.y = center.y; entry.bound.Center.z = center.z;
	entry.bound.Radius = radius * 1.02f + 2.0f;
	entry.ok = true;
	out = entry.bound;
	return true;
}

static bool TouchesShadowFace(NiAVObject* object, const NiPoint3* light,
                              const D3DXVECTOR3& direction) {
	NiGeometry* geometry = object->IsGeometry() ? static_cast<NiGeometry*>(object) : nullptr;
	if (geometry && geometry->skinInstance) {
		// Animated/deformed geometry may exceed the engine's current CPU bound: use the skeleton's (see above), or every face.
		NiBound skinned;
		if (!TheSettingManager->SettingsMain.Main.SkinnedShadowFaceTest || !SkinnedWorldBound(geometry, skinned)) return true;
		return ShadowSphereTouchesFace(skinned.Center.x - light->x, skinned.Center.y - light->y, skinned.Center.z - light->z,
			skinned.Radius, direction.x, direction.y, direction.z);
	}
	NiBound* bound = object->m_kWorldBound;
	if (!bound) return true;
	return ShadowSphereTouchesFace(bound->Center.x - light->x,
		bound->Center.y - light->y, bound->Center.z - light->z, bound->Radius,
		direction.x, direction.y, direction.z);
}

// Hash exactly the scene state that can change an otherwise camera-independent point-shadow cubemap.
// If the game's geometry list is unavailable the caller keeps the existing PointShadowInterval schedule.
static UInt64 HashPointShadowBytes(UInt64 hash, const void* data, size_t size) {
	const unsigned char* bytes = static_cast<const unsigned char*>(data);
	for (size_t i = 0; i < size; ++i) { hash ^= bytes[i]; hash *= 1099511628211ULL; }
	return hash;
}

// UNOFFICIAL 2026-10-21 FastCasterHash ([Main.Main.Performance], cpulimits; bench\deep-roadmap\CPU-LIMITS.md). The byte-wise 64-bit
// FNV above costs a call to the compiler's 64-bit multiply helper (_allmul, 32-bit build) for every byte: ~90 bytes per caster, for
// every lamp the caster is near, every frame. The CPU sampler put ~3 % of the main thread in the Ultra-Luxe here (the hash, _allmul,
// RecentlyMoved). With it on: the same bytes, four at a time, in two 32-bit lanes (FNV-1a and a Murmur-style mix) that use the CPU's
// own multiply, and a caster's hash is made once per frame however many lamps it is near (CasterMotion keeps it). Only what a lamp
// redraws on depends on the hash: a change is still caught unless both 32-bit lanes collide (about 1 in 2^64).
static bool FastCasterHashOn() { return TheSettingManager->SettingsMain.Main.FastCasterHash; }
static UInt64 HashPointShadowWords(UInt64 hash, const void* data, size_t size) {
	const unsigned char* bytes = static_cast<const unsigned char*>(data);
	UInt32 a = (UInt32)hash, b = (UInt32)(hash >> 32);
	size_t i = 0;
	for (; i + 4 <= size; i += 4) {
		UInt32 w;
		memcpy(&w, bytes + i, sizeof(w));
		a = (a ^ w) * 16777619u;
		b = (b ^ w) * 0x5BD1E995u; b ^= b >> 15;
	}
	for (; i < size; ++i) {
		a = (a ^ bytes[i]) * 16777619u;
		b = (b ^ bytes[i]) * 0x5BD1E995u; b ^= b >> 15;
	}
	return ((UInt64)b << 32) | a;
}
static UInt64 HashCasterBytes(UInt64 hash, const void* data, size_t size) {
	return FastCasterHashOn() ? HashPointShadowWords(hash, data, size) : HashPointShadowBytes(hash, data, size);
}

// UNOFFICIAL RedrawActorsOnly (2026-10-08): rigid geometry that moves -- a held gun, a hat, a door swinging, a box being
// pushed -- counts as moving while its world transform keeps changing and for MovingSettleFrames after it last changed.
// Without this, people's rigid parts were in the static layer and changed it every frame, so it was redrawn every frame.
// Each geometry is looked at once per frame (SkinnedBoundFrame); first sight counts as still.
struct CasterMotion { UInt32 seen = 0, changed = 0; UInt64 transform = 0; bool known = false; UInt32 hashFrame = 0; UInt64 casterHash = 0; bool hashKnown = false; };
static std::unordered_map<NiGeometry*, CasterMotion> CasterMotions;
static const UInt32 MovingSettleFrames = 60;

// UNOFFICIAL 2026-10-30 CasterListCache ([Main.Main.Performance], live, with FastCasterHash; ROOTPLAN round 7 step 18): the lamps' caster
// check looked every caster up in CasterMotions once per lamp, every frame (the map lookup alone ~0.7 % of the main thread in the
// Ultra-Luxe, the whole check ~3 %). Each lamp now keeps its list's (geometry, entry) pairs and only looks up a caster that is new at
// that place in its list; a caster's hash takes its transform's hash from the motion check instead of hashing the matrix again.
// Same answers (redraw or keep): the entries are the same ones. CasterMotionsGeneration drops the kept lists when entries are erased.
struct LampCasterList { std::vector<std::pair<NiGeometry*, CasterMotion*>> casters; UInt32 generation = 0; UInt32 seen = 0; };
static std::unordered_map<ShadowSceneLight*, LampCasterList> LampCasterLists;
static UInt32 CasterMotionsGeneration = 1;
static bool CasterListCacheOn() { return TheSettingManager->SettingsMain.Main.CasterListCache && TheSettingManager->SettingsMain.Main.FastCasterHash; }

static bool RecentlyMovedAt(NiGeometry* geo, CasterMotion& motion) {
	if (motion.seen != SkinnedBoundFrame || !motion.known) {
		const UInt64 transform = HashCasterBytes(1469598103934665603ULL, &geo->m_worldTransform, sizeof(geo->m_worldTransform));
		if (motion.known && transform != motion.transform) motion.changed = SkinnedBoundFrame;
		motion.transform = transform;
		motion.known = true;
		motion.seen = SkinnedBoundFrame;
	}
	return motion.changed && SkinnedBoundFrame - motion.changed < MovingSettleFrames;
}

static bool RecentlyMoved(NiGeometry* geo, CasterMotion** out = nullptr) {
	CasterMotion& motion = CasterMotions[geo];
	if (out) *out = &motion;
	if (motion.seen != SkinnedBoundFrame || !motion.known) {
		const UInt64 transform = HashCasterBytes(1469598103934665603ULL, &geo->m_worldTransform, sizeof(geo->m_worldTransform));
		if (motion.known && transform != motion.transform) motion.changed = SkinnedBoundFrame;
		motion.transform = transform;
		motion.known = true;
		motion.seen = SkinnedBoundFrame;
	}
	return motion.changed && SkinnedBoundFrame - motion.changed < MovingSettleFrames;
}

// A caster that moves: skinned geometry (people, creatures), wind-blown SpeedTree leaves (both move without their transform
// changing), and rigid geometry that moved lately (RecentlyMoved). They make a lamp's caster list dynamic, and
// RedrawActorsOnly draws them apart from the rest (RenderShadowCubeMap).
static bool IsMovingCaster(NiGeometry* geo, NiShadeProperty* shade, CasterMotion** motion = nullptr) {
	if (motion) *motion = nullptr;
	if (!geo) return false;
	if (geo->skinInstance || (shade && shade->m_eShaderType == NiShadeProperty::kProp_SpeedTreeLeaf)) return true;
	return RecentlyMoved(geo, motion);
}

static void PointShadowCasterState(ShadowSceneLight* light, UInt64& hash, bool& staticCasters) {
	// UNOFFICIAL LampGlassShadows / LampModelShadows (2026-10-20): switching them changes what a lamp's cube map holds, so they change
	// the hash (a redraw); with LampModelShadows off so does a new listing of the cell's lamp models (LampFixtures::Generation).
	const ShadowsExteriorEffect::InteriorsStruct::ForwardStruct& forwardSettings = TheShaderManager->Effects.ShadowsExteriors->Settings.Interiors.Forward;
	hash = forwardSettings.LampGlassShadows ? 1469598103934665603ULL : 0x6C62272E07BB0142ULL;
	hash = (hash ^ (UInt64)(SInt64)(forwardSettings.OwnGlassDistance * 16.0f) << 8) * 1099511628211ULL;	// UNOFFICIAL 2026-10-30 OwnGlassDistance
	if (!forwardSettings.LampModelShadows)
		hash = (hash ^ 0x9E3779B97F4A7C15ULL ^ ((UInt64)LampFixtures::Generation << 32) ^ (UInt64)(forwardSettings.LampModelDistance * 16.0f) ^
			((UInt64)(SInt64)(LampFixtures::OwnLightDistance * 16.0f) << 16)) * 1099511628211ULL;
	// UNOFFICIAL 2026-10-29 TwoSidedCasters ([Shaders.ShadowsInteriors.Main], RenderShadowCubeMap): switching it redraws the kept maps too
	// (the first bench run switched it live and the static maps stayed as they were drawn).
	if (TheSettingManager->GetSettingI("Shaders.ShadowsInteriors.Main", "TwoSidedCasters") != 0)
		hash = (hash ^ 0x2D5A6B1C3E4F7081ULL) * 1099511628211ULL;
	if (!LampFixtures::NoShadowWords.empty())	// UNOFFICIAL NoShadowLampModels (2026-10-27): its words and the cell's lamp list redraw too
		hash = (hash ^ (UInt64)std::hash<std::string>{}(LampFixtures::NoShadowFrom) ^ ((UInt64)LampFixtures::Generation << 40) ^
			(UInt64)(forwardSettings.LampModelDistance * 16.0f)) * 1099511628211ULL;
	// UNOFFICIAL 2026-10-31 CapsuleShadows DropActors: people leave the maps (switching it redraws them)
	const bool dropActors = CapsuleShadows::DropActors();
	if (dropActors) hash = (hash ^ 0x5851F42D4C957F2DULL) * 1099511628211ULL;
	staticCasters = light && light->kGeometryList.start;
	if (!staticCasters) return;
	// FastCasterHash switched (live): its hashes differ from the old ones, so every caster's motion starts again from 'first sight'
	// (counts as still) instead of looking moved for MovingSettleFrames; each lamp's set hash changes once (one redraw).
	const bool fast = FastCasterHashOn();
	static bool fastBefore = false;
	if (fast != fastBefore) { fastBefore = fast; CasterMotions.clear(); CasterMotionsGeneration++; }
	// CasterListCache switched (live): its hashes take the transform's hash, so each lamp's set hash changes once (one redraw)
	const bool listCache = fast && CasterListCacheOn();
	static bool listCacheBefore = false;
	if (listCache != listCacheBefore) { listCacheBefore = listCache; LampCasterLists.clear(); }
	LampCasterList* kept = nullptr;
	size_t keptAt = 0;
	if (listCache) {
		kept = &LampCasterLists[light];
		if (kept->generation != CasterMotionsGeneration) { kept->casters.clear(); kept->generation = CasterMotionsGeneration; }
		kept->seen = SkinnedBoundFrame;
	}

	// The hash covers only the casters that never move, so with moving ones it describes what RedrawActorsOnly keeps in
	// the static cube map.
	for (auto entry = light->kGeometryList.start; entry; entry = entry->next) {
		NiGeometry* geo = entry->data;
		if (dropActors && geo && geo->skinInstance) continue;	// not in the map: neither moving nor hashed
		NiShadeProperty* shade = geo ? static_cast<NiShadeProperty*>(geo->GetProperty(NiProperty::kType_Shade)) : nullptr;
		CasterMotion* motion = nullptr;
		bool moving;
		if (kept && geo) {	// CasterListCache: this lamp's entry for the caster at this place in its list, looked up only when it changed
			const bool alwaysMoving = geo->skinInstance || (shade && shade->m_eShaderType == NiShadeProperty::kProp_SpeedTreeLeaf);	// as IsMovingCaster: no entry
			if (keptAt < kept->casters.size() && kept->casters[keptAt].first == geo) motion = kept->casters[keptAt].second;
			else {
				motion = alwaysMoving ? nullptr : &CasterMotions[geo];
				if (keptAt < kept->casters.size()) kept->casters[keptAt] = { geo, motion };
				else kept->casters.push_back({ geo, motion });
			}
			keptAt++;
			moving = alwaysMoving || !motion || RecentlyMovedAt(geo, *motion);
			if (alwaysMoving) motion = nullptr;
		}
		else
			moving = IsMovingCaster(geo, shade, &motion);
		if (moving) {
			staticCasters = false;
			continue;
		}
		if (fast && motion && motion->hashKnown && motion->hashFrame == SkinnedBoundFrame) {	// FastCasterHash: made for another lamp this frame
			hash = PointShadowAddCasterHash(hash, motion->casterHash);
			continue;
		}
		UInt64 casterHash = 1469598103934665603ULL;
		casterHash = HashCasterBytes(casterHash, &geo, sizeof(geo));
		if (!geo) continue;
		casterHash = HashCasterBytes(casterHash, &geo->m_flags, sizeof(geo->m_flags));
		if (kept && motion && motion->seen == SkinnedBoundFrame) casterHash = HashCasterBytes(casterHash, &motion->transform, sizeof(motion->transform));	// CasterListCache: hashed this frame already
		else casterHash = HashCasterBytes(casterHash, &geo->m_worldTransform, sizeof(geo->m_worldTransform));
		if (geo->m_kWorldBound) casterHash = HashCasterBytes(casterHash, geo->m_kWorldBound, sizeof(*geo->m_kWorldBound));

		NiMaterialProperty* material = static_cast<NiMaterialProperty*>(geo->GetProperty(NiProperty::kType_Material));
		if (shade) {
			casterHash = HashCasterBytes(casterHash, &shade->m_usFlags, sizeof(shade->m_usFlags));
			casterHash = HashCasterBytes(casterHash, &shade->m_eShaderType, sizeof(shade->m_eShaderType));
		}
		if (material) casterHash = HashCasterBytes(casterHash, &material->fAlpha, sizeof(material->fAlpha));
		if (fast && motion) { motion->casterHash = casterHash; motion->hashFrame = SkinnedBoundFrame; motion->hashKnown = true; }
		hash = PointShadowAddCasterHash(hash, casterHash);
	}
	if (kept && kept->casters.size() > keptAt) kept->casters.resize(keptAt);
}

// UNOFFICIAL RedrawActorsOnly needs a min blend into the R32F cube maps (and a face copy, checked as it happens); asked
// once. A failed copy turns it off for the session (ActorsOnlyBroken); the lamps are then redrawn whole, as before.
static bool ActorsOnlyBroken = false;
static bool ActorsOnlySupported(IDirect3DDevice9* device) {
	static int supported = -1;
	if (supported < 0) {
		supported = 0;
		D3DCAPS9 caps = {};
		IDirect3D9* d3d = nullptr;
		D3DDEVICE_CREATION_PARAMETERS creation = {};
		D3DDISPLAYMODE mode = {};
		if (SUCCEEDED(device->GetDeviceCaps(&caps)) && (caps.PrimitiveMiscCaps & D3DPMISCCAPS_BLENDOP) && SUCCEEDED(device->GetDirect3D(&d3d)) &&
			SUCCEEDED(device->GetCreationParameters(&creation)) && SUCCEEDED(device->GetDisplayMode(0, &mode)))
			supported = SUCCEEDED(d3d->CheckDeviceFormat(creation.AdapterOrdinal, creation.DeviceType, mode.Format,
				D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING, D3DRTYPE_CUBETEXTURE, D3DFMT_R32F)) ? 1 : 0;
		if (d3d) d3d->Release();
		Logger::Log(supported ? "UNOFFICIAL RedrawActorsOnly: available (min blend into R32F cube maps)"
			: "UNOFFICIAL RedrawActorsOnly: this GPU cannot min-blend into R32F cube maps; lamps are redrawn whole, as before");
	}
	return supported == 1 && !ActorsOnlyBroken;
}

// Lamps trade slots as the player walks (PointShadowSlots.h keeps them where it can). A lamp's static cube map stays with
// the lamp: when another slot's was drawn for it, the two slots swap theirs, textures and records together. The static
// cube maps are only ever copied from (never bound to a shader), so swapping the pointers is all it takes.
static void AdoptStaticCubeMap(ShadowsExteriorEffect* shadows, PointShadowSlotState* staticSlots, int slot, const void* light) {
	if (staticSlots[slot].valid && staticSlots[slot].light == light) return;
	for (int k = 0; k < ShadowCubeMapsMax; k++) {
		if (k == slot || !staticSlots[k].valid || staticSlots[k].light != light) continue;
		std::swap(shadows->Textures.ShadowCubeMapStaticTexture[slot], shadows->Textures.ShadowCubeMapStaticTexture[k]);
		for (int f = 0; f < 6; f++) std::swap(shadows->Textures.ShadowCubeMapStaticSurface[slot][f], shadows->Textures.ShadowCubeMapStaticSurface[k][f]);
		std::swap(staticSlots[slot], staticSlots[k]);
		return;
	}
}

/*
* UNOFFICIAL 2026-10-18 CubeFaceCopySkip ([Main.Main.Performance]; work\bench\deep-roadmap\evidence\intshadow-LOG.md). With
* RedrawActorsOnly a lamp with people in reach is refreshed by copying its static cube map (walls and furniture) into the live one,
* all six faces, then drawing the moving casters over it (min blend). A face no moving caster's bound reaches gets nothing drawn: it
* ends up a plain copy of the static face. If the live face already holds exactly that copy -- it was last written by such a
* copy-only refresh, of the same static texture with the same contents (StaticCubeSerials, bumped at every static-layer draw),
* into the same live texture (CubeGeneration guards against a new texture at an old address), and nothing has drawn into it since
* (a whole redraw or a refresh with people on that face clears the record) -- the copy is skipped: the face keeps the same bits.
* DIAGNOSTIC CubeFaceCopySkipCheck (test builds): reads such faces back and compares them with the static face.
*/
struct LiveFaceRecord { const void* live = nullptr; const void* staticTex = nullptr; unsigned serial = 0, generation = 0; bool pure = false; };
static LiveFaceRecord LiveFaces[ShadowCubeMapsMax][6];
static std::unordered_map<const void*, unsigned> StaticCubeSerials;
static unsigned StaticCubeSerialNext = 0;
static unsigned statFacesSkipped = 0, statFacesCopied = 0, statFacesDrawn = 0, statFacesOldPath = 0;

static void ForgetLiveFaces(int slot) {
	for (int f = 0; f < 6; f++) LiveFaces[slot][f].pure = false;
}

// UNOFFICIAL 2026-10-18 LampShadowMoveTolerance ([Main.Main.ReducedQuality], CHANGES THE IMAGE, off at 0): the point the cube map
// is drawn from when it is not the light's own position this frame (RenderShadowMaps sets it around the draw calls).
static NiPoint3* CubeEyeOverride = nullptr;
// UNOFFICIAL InvisibleShadowFromLamp (2026-10-20): while one lamp's cube map is drawn from a visible lamp instead of the light: the radius
// it covers (the light's reach plus the move) and the lamp model at its eye, left out (-1 none).
static float CubeRadiusOverride = 0.0f;
static bool CubeRelocated = false;
static int CubeSkipModel = -1;

// UNOFFICIAL 2026-10-18 CubeFaceViewCull ([Main.Main.Performance]; evidence\intshadow-LOG.md). Indoors with the forward lamp shadows,
// a lamp cube map is looked up only by the world pass and the first-person pass (PointShadowForward::SetForPass: never the water
// reflection or another camera), i.e. by pixels inside the camera's view or close to the camera; the screen-space lamp mask
// draws without cube lookups then. So a face whose pyramid (CubeFaceOutOfView: the lamp and its four corners at the cube radius,
// widened 10% for filtering across the seams; points within 100 units of the camera always count as seen, for the first-person
// model's own field of view) lies wholly outside the view is never read this frame and need not be drawn. Only for lamps that are
// redrawn every frame anyway (moving casters in reach at PointShadowInterval 1, or the lamp itself moved); such a draw is marked
// partial (PointShadowSlotState), so a lamp that turns still is redrawn in full before its cube map is kept. Static layers
// (RedrawActorsOnly) are always drawn in full. DIAGNOSTIC CubeFaceViewCullCheck: the left-out faces are filled with distance 0.1
// (a blocker at a tenth of the reach: all beyond in shadow; 0 would mean no data), so a face read after all shows as a dark patch.
static bool CubeCullAllowed = false;	// set by RenderShadowMaps around one lamp's draws
static bool ForwardWasActive = false;	// PointShadowForward::Active as Publish left it last frame (Begin clears it before the loop)
static bool CubeCulledAny = false;		// RenderShadowCubeMap left a face out
static unsigned statFacesCulled[3] = {};

// UNOFFICIAL 2026-10-18 diagnostic (intshadow): a lamp cube face that no on-screen pixel can look up -- its pyramid (the lamp and
// the face's four corners at the cube radius, widened 10% for filtering across the seams) lies wholly behind one side of the
// camera's view (left, right, top, bottom, behind; the camera as LampMaskRect reads it). Counted only, nothing skipped.
static unsigned statViewFaces[3] = {}, statViewFacesOut[3] = {};
static bool CubeFaceOutOfView(const NiPoint3* light, float radius, const D3DXVECTOR3& direction) {
	const D3DXMATRIX& V = TheRenderManager->viewMatrix;
	const D3DXMATRIX& P = TheRenderManager->projMatrix;
	const D3DXVECTOR3 R(V._11, V._21, V._31), U(V._12, V._22, V._32), F(V._13, V._23, V._33);
	const D3DXVECTOR3 planes[5] = { F, F + R * P._11, F - R * P._11, F + U * P._22, F - U * P._22 };
	const D3DXVECTOR3 L(light->x - TheRenderManager->CameraPosition.x, light->y - TheRenderManager->CameraPosition.y, light->z - TheRenderManager->CameraPosition.z);
	const D3DXVECTOR3 a = fabsf(direction.x) > 0.5f ? D3DXVECTOR3(0, 1, 0) : D3DXVECTOR3(1, 0, 0);
	D3DXVECTOR3 b; D3DXVec3Cross(&b, &direction, &a);
	D3DXVECTOR3 points[5] = { L, L, L, L, L };
	for (int c = 0; c < 4; c++) points[c + 1] = L + radius * (direction + 1.1f * (((c & 1) ? a : -a) + ((c & 2) ? b : -b)));
	for (const D3DXVECTOR3& n : planes) {
		const float margin = -100.0f * D3DXVec3Length(&n);	// points within 100 units of the camera count as seen (first-person model)
		bool allOut = true;
		for (const D3DXVECTOR3& p : points) if (D3DXVec3Dot(&p, &n) >= margin) { allOut = false; break; }
		if (allOut) return true;
	}
	return false;
}

#if NVR_TEST_TOOLS
// DIAGNOSTIC CubeFaceCopySkipCheck N (test builds): every Nth frame, up to two faces that CubeFaceCopySkip left alone are read back
// with the static face they should equal and compared bit for bit. Stalls the GPU.
static void CubeFaceSkipSelfCheck(IDirect3DDevice9* Device, IDirect3DSurface9* live, IDirect3DSurface9* still) {
	static int every = 0;
	static unsigned frameSeen = 0, checksThisFrame = 0, checks = 0, differing = 0, worstValues = 0;
	if (frameSeen != SkinnedBoundFrame) {
		frameSeen = SkinnedBoundFrame;
		checksThisFrame = 0;
		if ((SkinnedBoundFrame % 60) == 0 || every == 0) every = TheSettingManager->GetSettingI("Main.Main.Performance", "CubeFaceCopySkipCheck");
	}
	if (every <= 0 || (SkinnedBoundFrame % (unsigned)every) != 0 || checksThisFrame >= 2 || !live || !still) return;
	checksThisFrame++;
	static IDirect3DSurface9* readA = nullptr;
	static IDirect3DSurface9* readB = nullptr;
	static D3DSURFACE_DESC have = {};
	D3DSURFACE_DESC d = {};
	if (FAILED(live->GetDesc(&d)) || d.Format != D3DFMT_R32F) return;
	if (!readA || d.Width != have.Width || d.Height != have.Height) {
		if (readA) { readA->Release(); readA = nullptr; }
		if (readB) { readB->Release(); readB = nullptr; }
		if (FAILED(Device->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &readA, NULL)) ||
			FAILED(Device->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &readB, NULL))) {
			Logger::Log("CUBE FACE CHECK: could not create the read-back surfaces");
			if (readA) { readA->Release(); readA = nullptr; }
			return;
		}
		have = d;
	}
	if (FAILED(Device->GetRenderTargetData(live, readA)) || FAILED(Device->GetRenderTargetData(still, readB))) return;
	D3DLOCKED_RECT la = {}, lb = {};
	if (FAILED(readA->LockRect(&la, NULL, D3DLOCK_READONLY))) return;
	if (FAILED(readB->LockRect(&lb, NULL, D3DLOCK_READONLY))) { readA->UnlockRect(); return; }
	unsigned values = 0;
	for (UINT y = 0; y < d.Height; y++) {
		const UInt32* pa = (const UInt32*)((const UInt8*)la.pBits + (size_t)y * la.Pitch);
		const UInt32* pb = (const UInt32*)((const UInt8*)lb.pBits + (size_t)y * lb.Pitch);
		for (UINT x = 0; x < d.Width; x++) if (pa[x] != pb[x]) values++;
	}
	readB->UnlockRect();
	readA->UnlockRect();
	checks++;
	if (values) { differing++; worstValues = (std::max)(worstValues, values); }
	if (values && differing <= 10) Logger::Log("CUBE FACE CHECK: DIFFERENCE, a skipped face differs from its static face in %u of %u texels", values, d.Width * d.Height);
	if (checks % 30 == 0) Logger::Log("CUBE FACE CHECK: %u skipped faces compared with their static face, %u differ (worst %u texels)", checks, differing, worstValues);
}
#endif

/*
* Initializes the Shadow Manager by grabbing the relevant settings and shaders, and setting up map sizes.
*/
#include "GpuProfiler.h"

// Refresh period per cascade. By default this is the original schedule: every cascade every frame,
// except that LimitFrequency redraws the Lod cascade every fourth frame. StaggeredSunShadows
// (ReducedQuality, off by default) is the P21-P48 schedule: middle every 4th frame, far and Lod every
// 8th. A cached cascade follows the camera's movement, so still shadows stay put, but anything that
// moves (people, creatures, doors) keeps its old shadow until the next refresh: with the stagger,
// people walking a few metres away got shadows that stuttered and flickered (reported under DXVK).
// Resolve and blur run only on cascades updated that frame (BlurShadowAtlas mask), so cached
// cascades stay untouched either way.
// nearInterval is the ReducedQuality NearCascadeInterval switch (1 = every frame, the original).
// UNOFFICIAL 2026-10-28: the sun cascades (bit per ShadowMapTypeEnum) drawn this frame, for GrassShadows (CastShadowsReach): grass
// goes into a middle or far cascade only on the frames that cascade is redrawn.
unsigned SunCascadesDrawnMask = 0;

constexpr unsigned SunCascadeUpdatePeriod(int cascade, bool limitFrequency, int nearInterval = 1, bool staggered = false) {
	if (cascade == ShadowManager::MapNear) return nearInterval == 2 ? 2 : 1;
	if (staggered) {
		if (cascade == ShadowManager::MapMiddle) return 4;
		return (cascade == ShadowManager::MapFar || cascade == ShadowManager::MapLod) ? 8 : 1;
	}
	return (limitFrequency && cascade == ShadowManager::MapLod) ? 4 : 1;
}

static_assert(SunCascadeUpdatePeriod(ShadowManager::MapNear, true) == 1 &&
	SunCascadeUpdatePeriod(ShadowManager::MapMiddle, true) == 1 &&
	SunCascadeUpdatePeriod(ShadowManager::MapFar, true) == 1 &&
	SunCascadeUpdatePeriod(ShadowManager::MapLod, true) == 4 &&
	SunCascadeUpdatePeriod(ShadowManager::MapLod, false) == 1 &&
	SunCascadeUpdatePeriod(ShadowManager::MapNear, true, 1, true) == 1 &&
	SunCascadeUpdatePeriod(ShadowManager::MapMiddle, true, 1, true) == 4 &&
	SunCascadeUpdatePeriod(ShadowManager::MapFar, true, 1, true) == 8 &&
	SunCascadeUpdatePeriod(ShadowManager::MapLod, false, 1, true) == 8 &&
	SunCascadeUpdatePeriod(ShadowManager::MapNear, true, 2) == 2 && SunCascadeUpdatePeriod(ShadowManager::MapNear, false, 2) == 2 &&
	SunCascadeUpdatePeriod(ShadowManager::MapMiddle, true, 2) == 1 && SunCascadeUpdatePeriod(ShadowManager::MapMiddle, true, 2, true) == 4,
	"Sun cascade update schedule changed unexpectedly");

// Frame offset within each cascade's period, so the infrequent cascades never refresh on the
// same frame. With StaggeredSunShadows, over the 8-frame cycle: 0 near only, 1/5 middle, 2/6 ortho
// map, 3 far, 7 LOD. With the default schedule the Lod cascade (every 4th frame) takes frames 3 and 7.
// Refresh rates are unchanged; this only removes the frame where all four used to render.
constexpr unsigned SunCascadeUpdatePhase(int cascade) {
	if (cascade == ShadowManager::MapMiddle) return 1;
	if (cascade == ShadowManager::MapFar) return 3;
	if (cascade == ShadowManager::MapLod) return 7;
	return 0;
}

constexpr bool SunCascadeUpdatesOnFrame(int cascade, unsigned frame, unsigned period) {
	return frame % period == SunCascadeUpdatePhase(cascade) % period;
}

static_assert(SunCascadeUpdatesOnFrame(ShadowManager::MapNear, 5, 1) &&
	SunCascadeUpdatesOnFrame(ShadowManager::MapMiddle, 5, 4) && !SunCascadeUpdatesOnFrame(ShadowManager::MapMiddle, 0, 4) &&
	SunCascadeUpdatesOnFrame(ShadowManager::MapFar, 3, 8) && SunCascadeUpdatesOnFrame(ShadowManager::MapLod, 7, 8) &&
	SunCascadeUpdatesOnFrame(ShadowManager::MapFar, 0, 1),
	"Sun cascade stagger changed unexpectedly");

// With StaggeredSunShadows and NearCascadeInterval 2 the near cascade takes the even frames and the
// limited ones keep the odd frames, so every frame of the 8-frame cycle draws exactly one sun cascade.
constexpr bool OneSunCascadePerFrame() {
	for (unsigned frame = 0; frame < 8; ++frame) {
		unsigned drawn = 0;
		for (int cascade = ShadowManager::MapNear; cascade < ShadowManager::MapOrtho; ++cascade)
			drawn += SunCascadeUpdatesOnFrame(cascade, frame, SunCascadeUpdatePeriod(cascade, true, 2, true)) ? 1 : 0;
		if (drawn != 1) return false;
	}
	return true;
}
static_assert(OneSunCascadePerFrame(), "StaggeredSunShadows with NearCascadeInterval 2 should leave one sun cascade per frame");

void ShadowManager::Initialize() {
	static const char* const startMessage = "Starting the shadows manager...";
	Logger::Log(startMessage);
	ShadowManager& sm = *(TheShadowManager = new ShadowManager());	// value-initialised: every member without an initialiser starts at zero

	// The render passes, one per caster kind (the draw code picks them by kind).
	sm.geometryPass = new ShadowRenderPass();
	sm.alphaPass = new AlphaShadowRenderPass();
	sm.skinnedGeoPass = new SkinnedGeoShadowRenderPass();
	sm.speedTreePass = new SpeedTreeShadowRenderPass();
	sm.terrainLODPass = new TerrainLODPass();

	// Shader files, all under Shadows\. The load order is kept as it was.
	const char* shadowDir = "Shadows\\";
	sm.ShadowMapVertex = (ShaderRecordVertex*)ShaderRecord::LoadShader("ShadowMap.vso", shadowDir);
	sm.ShadowMapPixel = (ShaderRecordPixel*)ShaderRecord::LoadShader("ShadowMap.pso", shadowDir);
	sm.ShadowCubeMapVertex = (ShaderRecordVertex*)ShaderRecord::LoadShader("ShadowCubeMap.vso", shadowDir);
	sm.ShadowCubeMapPixel = (ShaderRecordPixel*)ShaderRecord::LoadShader("ShadowCubeMap.pso", shadowDir);
	// UNOFFICIAL (from Physically-Based-Reloaded, Fable): opaque casters get a copy of the caster shader without its
	// alpha-test discard. A shader that can discard turns early depth rejection off for every caster drawn with it,
	// and an opaque caster never reaches the discard (TESR_ShadowData.y is 0). Alpha and leaf passes keep the clip shader.
	sm.ShadowMapOpaquePixel = (ShaderRecordPixel*)ShaderRecord::LoadShader("ShadowMapOpaque.pso", shadowDir);
	sm.ShadowCubeMapOpaquePixel = (ShaderRecordPixel*)ShaderRecord::LoadShader("ShadowCubeMapOpaque.pso", shadowDir);
	// UNOFFICIAL 2026-10-14: the light grid's sun map with colours; missing, the grid's bounce takes one ground colour.
	sm.ShadowGridColorPixel = (ShaderRecordPixel*)ShaderRecord::LoadShader("ShadowGridColor.pso", shadowDir);
	if (sm.ShadowGridColorPixel) sm.ShadowGridColorPixel->ClearSamplers = false;
	// A missing copy falls back to the clip shader: the same image, only the early-Z gain is lost.
	if (!sm.ShadowMapOpaquePixel) {
		sm.ShadowMapOpaquePixel = sm.ShadowMapPixel;
		Logger::Log("[WARNING] ShadowMapOpaque.pso did not load; opaque shadow casters use the alpha-test shader.");
	}
	if (!sm.ShadowCubeMapOpaquePixel) {
		sm.ShadowCubeMapOpaquePixel = sm.ShadowCubeMapPixel;
		Logger::Log("[WARNING] ShadowCubeMapOpaque.pso did not load; opaque cube casters use the alpha-test shader.");
	}

	sm.ShadowMapBlurVertex = (ShaderRecordVertex*)ShaderRecord::LoadShader("ShadowMapBlur.vso", shadowDir);
	sm.ShadowMapBlurPixel = (ShaderRecordPixel*)ShaderRecord::LoadShader("ShadowMapBlur.pso", shadowDir);
	sm.ShadowMapClearPixel = (ShaderRecordPixel*)ShaderRecord::LoadShader("ShadowMapClear.pso", shadowDir);

	// Samplers must not be reset on SetCT, which causes errors. A shader whose file is missing loads as null:
	// it used to be dereferenced right here, crashing the game at startup on an install without
	// Shaders\VegasShaders\Shaders\Shadows (seen in a user's log). Shadows are disabled instead.
	ShaderRecord* shadowShaders[] = { sm.ShadowMapVertex, sm.ShadowMapPixel,
		sm.ShadowCubeMapVertex, sm.ShadowCubeMapPixel, sm.ShadowMapBlurVertex,
		sm.ShadowMapBlurPixel, sm.ShadowMapClearPixel, sm.ShadowMapOpaquePixel,
		sm.ShadowCubeMapOpaquePixel };
	sm.ShadowShadersLoaded = true;
	for (ShaderRecord* shader : shadowShaders) {
		if (shader) shader->ClearSamplers = false;
		else sm.ShadowShadersLoaded = false;
	}
	if (!sm.ShadowShadersLoaded)
		Logger::Log("[ERROR]: Could not load one or more of the ShadowMap generation shaders (%sShaders\\Shadows). "
			"Shadow maps are disabled. Reinstall the mod.", Paths::ShaderRootShort());

	// UNOFFICIAL contact hardening: registered before the game's shaders load, so every shader that includes
	// Shadow.hlsl finds it by name. Whether the shaders have it is decided now, at startup (CONTACT_HARDENING).
	TheShaderManager->RegisterConstant("TESR_ContactHardeningData", &sm.ContactHardeningData);
	sm.ContactHardeningCompiled = TheSettingManager->GetSettingI("Shaders.ContactHardening.Status", "Enabled") != 0;

	UINT cubeSize = TheShaderManager->Effects.ShadowsExteriors->Settings.Interiors.ShadowCubeMapSize;
	sm.ShadowCubeMapViewPort = { 0, 0, cubeSize, cubeSize, 0.0f, 1.0f };

	sm.shadowMapsRenderTime = 0;
}


// Form-kind filter for GetRefNode: false when the Forms settings leave this kind out, and for land, which has its own pass.
static bool FormKindCastsShadows(UInt8 kind, ShadowsExteriorEffect::FormsStruct* forms) {
	typedef TESForm::FormType Kind;
	switch (kind) {
	case Kind::kFormType_Land:				return false;	// land is walked by itself (AccumExteriorCell)
	case Kind::kFormType_Activator:			return forms->Activators;
	case Kind::kFormType_Apparatus:			return forms->Apparatus;
	case Kind::kFormType_Book:				return forms->Books;
	case Kind::kFormType_Container:			return forms->Containers;
	case Kind::kFormType_Door:				return forms->Doors;
	case Kind::kFormType_Misc:				return forms->Misc;
	case Kind::kFormType_Tree:				return forms->Trees;
	case Kind::kFormType_Furniture:			return forms->Furniture;
	case Kind::kFormType_NPC:
	case Kind::kFormType_Creature:
	case Kind::kFormType_LeveledCreature:	return forms->Actors;	// D3D9 breaks on actors for some unknown reason
	case Kind::kFormType_Stat:
	case Kind::kFormType_StaticCollection:
	case Kind::kFormType_MoveableStatic:	return forms->Statics;
	default:								return true;	// kinds without a switch of their own always cast
	}
}

/*
* Returns the given object ref's NiNode if it passes the test for excluded form types, otherwise returns NULL.
*/
NiNode* ShadowManager::GetRefNode(TESObjectREFR* Ref, ShadowsExteriorEffect::FormsStruct* Forms) {
	const bool optedOut = Ref == nullptr || (Ref->flags & TESForm::FormFlags::kFormFlags_NotCastShadows) != 0;
	if (optedOut) return nullptr;

	// The form filter runs before GetNode() and the extra-data walk below, both of which are the
	// expensive part of this function. Called once per reference per cascade, so a reference the
	// filter rejects should cost only the flag test and the switch.
	if (!FormKindCastsShadows(Ref->baseForm->formType, Forms)) return nullptr;

	// GetNode() is a plain pointer read, so it stays here. The refraction test is NOT: it calls
	// into the engine to walk the reference's extra-data list. Callers apply IsRefracting()
	// after the cascade test instead, where only the references that survive it pay the cost --
	// measured at roughly 2 acceptances per 3400 references for the Near cascade.
	NiNode* root = Ref->GetNode();
	return root;
}

// Split out of GetRefNode. Walks the extra-data list, so call it as late as possible. A reference counts as refracting from
// half strength (refraction amount 0.5 or less) on; one without the extra data does not.
bool ShadowManager::IsRefracting(TESObjectREFR* Ref) {
	BSExtraData* extra = Ref->extraDataList.GetExtraData(BSExtraData::ExtraDataType::kExtraData_RefractionProperty);
	if (extra == nullptr) return false;
	const float strength = 1 - static_cast<ExtraRefractionProperty*>(extra)->refractionAmount;
	return strength >= 0.5;
}


// False for geometry without a shader property and for refraction, fire refraction and (dynamic) decal shaders: no shadow.
bool ShadowManager::CheckShaderFlags(NiGeometry* Geometry) {
	auto* bsProp = static_cast<BSShaderProperty*>(Geometry->GetProperty(NiProperty::kType_Shade));
	if (bsProp == nullptr) return false;
	const bool excluded = bsProp->GetFlag(BSSP_REFRACTION) || bsProp->GetFlag(BSSP_FIRE_REFRACTION) ||
		bsProp->GetFlag(BSSP_DECAL) || bsProp->GetFlag(BSSP_DYNAMIC_DECAL);
	return !excluded;
}


// UNOFFICIAL 2026-10-17 (TB-1): true while a cell's land is being accumulated (AccumExteriorCell), so its geometry is noted
// in RenderPass::LandSet for the Terrain Blending shadow drop.
static bool AccumulatingLand = false;

// UNOFFICIAL 2026-10-31 SunLayers (RenderPass::LayerMode, DrawSunLayer below): the reference being accumulated (its geometry is noted
// in RenderPass::LayerGeoRef while a static layer is drawn), the references a static layer held back (drawn by the overlay), and
// whether SpeedTrees count as moving (their leaves sway in the wind).
static TESObjectREFR* LayerAccumRef = nullptr;
static const std::unordered_set<const void*>* LayerOverlayRefs = nullptr;
static bool LayerTreesMoving = true;

// SunLayers: a reference that can move by itself (or be moved): anything not on the still list -- doors, actors, items the physics
// moves, projectiles, moveable statics, an unknown kind -- and a still kind whose model is animated (a controller on its root: flags,
// fans, animated activators and lights).
static bool RefMovesByKind(TESObjectREFR* Ref, NiNode* Node) {
	switch (Ref->baseForm->formType) {
	case TESForm::FormType::kFormType_Stat:
	case TESForm::FormType::kFormType_StaticCollection:
	case TESForm::FormType::kFormType_Container:
	case TESForm::FormType::kFormType_Furniture:
	case TESForm::FormType::kFormType_Terminal:
	case TESForm::FormType::kFormType_Flora:
	case TESForm::FormType::kFormType_IdleMarker:
	case TESForm::FormType::kFormType_Grass:
	case TESForm::FormType::kFormType_PlaceableWater:
	case TESForm::FormType::kFormType_Activator:
	case TESForm::FormType::kFormType_TalkingActivator:
	case TESForm::FormType::kFormType_Light:
		break;
	case TESForm::FormType::kFormType_Tree:
		if (LayerTreesMoving) return true;
		break;
	default:
		return true;
	}
	return Node->m_controller != nullptr;
}

// Hands one geometry to the pass that draws its kind (skinned, tree, LOD land, alpha tested, opaque).
void ShadowManager::AccumObject(NiAVObject* Object, ShadowsExteriorEffect::FormsStruct* Forms, bool isLODLand) {
	auto* geo = static_cast<NiGeometry*>(Object);
#if NVR_TEST_TOOLS
	OP_LAND(0);								// land diagnostics (OrthoProbe::LandCount): 0 geometry reached
	if (OrthoProbe::Recording && OrthoProbe::CurrentLand) {	// UNOFFICIAL fixes-b #2: these checks do real work, run them only while recording
		if (!geo->shader) OP_LAND(1);			// 1 no shader
		else if (!CheckShaderFlags(geo)) OP_LAND(2);	// 2 shader flags (decal, refraction) or no shader property
		else if (!geometryPass->TestObject(geo)) OP_LAND(3);	// 3 the opaque pass refuses it (no buffer data / not a lighting property)
	}
#endif
#if NVR_TEST_TOOLS
	if (SunShadowStats::Current) SunShadowStats::Current->geometry++;
#endif
	if (geo->shader == nullptr || !CheckShaderFlags(geo)) return;	// no shader, or one that casts no shadow
#if NVR_TEST_TOOLS
	OrthoProbe::NoteGeometry(geo);
#endif
	if (RenderPass::LayerMode == 1 && LayerAccumRef) RenderPass::LayerGeoRef[geo] = LayerAccumRef;	// UNOFFICIAL 2026-10-31 SunLayers

	// UNOFFICIAL 2026-10-17 Skylighting v2 (CleanMap): in the ortho map, geometry that is only alpha BLENDED (no alpha test) does not
	// block the sky: effect meshes (Goodsprings' FXDustWhirlWind01 over the road), glass, smoke cards. Set only around that map.
	if (OrthoSkipBlendOnly) {
		NiAlphaProperty* alpha = (NiAlphaProperty*)geo->GetProperty(NiProperty::PropertyType::kType_Alpha);
		if (alpha && (alpha->flags & NiAlphaProperty::AlphaFlags::ALPHA_BLEND_MASK) && !(alpha->flags & NiAlphaProperty::AlphaFlags::TEST_ENABLE_MASK))
			return;
	}
	// Each caster goes to the first pass that accepts it; the opaque pass takes whatever is left.
	if (skinnedGeoPass->AccumObject(geo) || speedTreePass->AccumObject(geo))
		return;
	if (Forms->Lod && isLODLand && terrainLODPass->AccumObject(geo))
		return;
	if (Forms->AlphaEnabled && alphaPass->AccumObject(geo))
		return;
	if (AccumulatingLand && DisplaceShadows::Takes(geo) && geometryPass->TestObject(geo) == 1) {	// UNOFFICIAL 2026-10-27 (sun cascades)
		DisplaceShadows::Pass.GeometryList.push(geo);
		if (RenderPass::LandDrop > 0.0f) RenderPass::LandSet.insert(geo);
		return;
	}
	if (geometryPass->AccumObject(geo) && AccumulatingLand && RenderPass::LandDrop > 0.0f) RenderPass::LandSet.insert(geo);
}


// UNOFFICIAL (from Physically-Based-Reloaded, Fable): object LOD inside the loaded cells. Every cascade also walks the
// loaded cells' references as full models (AccumExteriorCell), so their LOD copy cast a second shadow on top -- and a
// ghost one where the full object was swapped or disabled (Base Object Swapper), since the baked LOD never changes.
// The box is BSShaderManager::kLoadedRange (centre x/y, half size z/w), the one terrain LOD is hidden under. A geometry
// is inside when its bound centre is; a node only when its whole bound is, so a block straddling the edge is walked.
static bool IsLodInLoadedCells(NiAVObject* Object) {
	NiBound* Bound = Object->m_kWorldBound;
	if (!Bound) return false;
	const float Reach = Object->IsGeometry() ? 0.0f : Bound->Radius;
	const NiPoint4* Range = BSShaderManager::kLoadedRange;
	return fabsf(Bound->Center.x - Range->x) + Reach < Range->z && fabsf(Bound->Center.y - Range->y) + Reach < Range->w;
}

// go through the Object children and sort the ones that will be rendered based on their properties
void ShadowManager::AccumChildren(NiAVObject* Root, ShadowsExteriorEffect::FormsStruct* Forms, bool isLand, bool isLOD, NiFrustumPlanes* Planes) {
	if (Root == nullptr) return;

	// Land is never dropped for its size (it is all large anyway).
	auto belowMinRadius = [isLand, Forms](NiAVObject* object) { return !isLand && object->GetWorldBoundRadius() < Forms->MinRadius; };
	const bool skipLoadedLod = isLOD && !isLand && TheShaderManager->Effects.ShadowsExteriors->Settings.ShadowMaps.LodSkipLoaded;
	const bool lodOfLand = isLand && isLOD;

	std::vector<NiAVObject*>& pending = containersScratch;
	pending.clear();

	// Geometry goes to its pass at once; anything else waits in the pending list for the walk below.
	auto sortOrQueue = [&](NiAVObject* object, bool lodLand) {
		if (!object->IsGeometry())
			pending.push_back(object);
		else
			AccumObject(object, Forms, lodLand);
	};
	sortOrQueue(Root, lodOfLand);

	while (!pending.empty()) {
		NiAVObject* const object = pending.back();
		pending.pop_back();
		if (object == nullptr) continue;

		NiNode* node = object->IsNiNode();
#if NVR_TEST_TOOLS
		if (isLand && !isLOD) { OP_LAND(4); if (node && (node->m_flags & NiAVObject::NiFlags::APP_CULLED)) OP_LAND(5); }	// 4 land nodes, 5 culled
#endif
		// a container: culled or (outside the land) too small to cast
		if (!node || (node->m_flags & NiAVObject::NiFlags::APP_CULLED)) continue;
		if (belowMinRadius(node)) continue;
#if NVR_TEST_TOOLS
		if (SunShadowStats::Current) SunShadowStats::Current->nodes++;
#endif

		if (node->IsKindOf<NiSwitchNode>()) {
			// NiSwitchNode - only the active child (if there is one) goes to the shadow map.
			NiSwitchNode* switchNode = static_cast<NiSwitchNode*>(node);
			if (switchNode->m_iIndex < 0 || switchNode->m_iIndex >= node->m_children.end)
				continue;

			NiAVObject* active = node->m_children.data[switchNode->m_iIndex];
			if (!active || (skipLoadedLod && IsLodInLoadedCells(active)))
				continue;
			sortOrQueue(active, false);
			continue;
		}

		for (int i = 0; i < node->m_children.end; i++) {
			NiAVObject* child = node->m_children.data[i];
#if NVR_TEST_TOOLS
			if (isLand && !isLOD) { OP_LAND(6); if (child && (child->m_flags & NiAVObject::NiFlags::APP_CULLED)) OP_LAND(7); }	// 6 land children, 7 APP_CULLED
#endif
			// culled or (outside the land) too small to cast
			if (!child || (child->m_flags & NiAVObject::NiFlags::APP_CULLED)) continue;
			if (belowMinRadius(child)) continue;

			// Frustum culling.
#if NVR_TEST_TOOLS
			if (SunShadowStats::Current && Planes) SunShadowStats::Current->childTests++;
#endif
			// Land and LOD: only a multibound node's own bound is tested; anything else: the child's bound.
			if (Planes) {
				if (isLand || isLOD) {
					BSMultiBoundNode* mbNode = child->IsMultiBoundNode();
#if NVR_TEST_TOOLS
					if (OrthoProbe::Recording && isLand && !isLOD) { if (mbNode) OP_LAND(8); if (mbNode && !mbNode->spMultiBound->spShape->WithinFrustum(*Planes)) OP_LAND(9); }	// 8 multibound, 9 outside
#endif
					const bool outside = mbNode && !mbNode->spMultiBound->spShape->WithinFrustum(*Planes);
					if (outside) continue;
				}
				else if (!child->WithinFrustum(Planes)) {
					continue;
				}
			}

#if NVR_TEST_TOOLS
			if (OrthoProbe::Recording && isLand && !isLOD && child->IsFadeNode() && static_cast<BSFadeNode*>(child)->FadeAlpha < 0.75f) OP_LAND(10);	// 10 faded
#endif
			if (child->IsFadeNode() && static_cast<BSFadeNode*>(child)->FadeAlpha < TheShaderManager->Effects.ShadowsExteriors->Settings.ShadowMaps.CasterFadeMin) { FrameJournal::FadeDrops++; continue; } // stop rendering fadenodes below a certain opacity (UNOFFICIAL 2026-10-28: CasterFadeMin, 0.75 = as before)
			if (skipLoadedLod && IsLodInLoadedCells(child)) continue; // object LOD inside the loaded cells
			sortOrQueue(child, lodOfLand);
		}
	}
}

// Go through accumulations and render found objects
void ShadowManager::RenderAccums() {
	// UNOFFICIAL 2026-10-27 displacement shadows (DisplaceShadows.h): the land near the camera with its bumps, first: a piece it cannot
	// draw that way goes to the opaque pass below (empty outside the sun cascades)
	DisplaceShadows::Pass.Draw(geometryPass);
#if NVR_TEST_TOOLS
	if (SunShadowStats::Current) {	// UNOFFICIAL 2026-10-21 test tools: the same draws, each pass named for the counts
		SunShadowStats::BeginPass(SunShadowStats::kOpaque); geometryPass->RenderAccum();
		SunShadowStats::BeginPass(SunShadowStats::kLodLand); terrainLODPass->RenderAccum();
		SunShadowStats::BeginPass(SunShadowStats::kAlpha); alphaPass->RenderAccum();
		SunShadowStats::BeginPass(SunShadowStats::kSkinned); skinnedGeoPass->RenderAccum();
		SunShadowStats::BeginPass(SunShadowStats::kTree); speedTreePass->RenderAccum();
		SunShadowStats::CurrentPass = -1;
		return;
	}
#endif
	// Opaque first, then LOD land, alpha tested, skinned and trees (RenderAccum is RenderPass's own, not virtual).
	RenderPass* const drawOrder[] = { geometryPass, terrainLODPass, alphaPass, skinnedGeoPass, speedTreePass };
	for (RenderPass* pass : drawOrder)
		pass->RenderAccum();
}


static float SunLandDropScale = 0.0f;	// UNOFFICIAL fixes-b #9: set by the sun cascade loop around its RenderShadowMap call, 0 elsewhere

// UNOFFICIAL 2026-10-17 (TB-1): with Terrain Blending on, the land goes into the sun map BlendDistance * ShadowDrop lower
// (RenderPass::LandDrop), so the part of an object the faded land reveals is not in the land's own shadow.
// fixes-b #9: only the sun cascades drop the land (SunLandDropScale > 0 only around their RenderShadowMap call: not the
// rain / ortho map or the light grid's sun map), and less at a low sun: a lowered ridge casts a shadow shorter by
// drop / tan(elevation), so the drop fades x min(1, 2 sin(elevation)) (full above 30 degrees).
static float ShadowMapLandDrop() {
	TerrainShaders* terrain = TheShaderManager->Shaders.Terrain;
	const bool drop = terrain && terrain->BlendingSettings.Enabled && TheTextureManager->TerrainBlendDepthTexture;
	return drop ? terrain->BlendingSettings.BlendDistance * terrain->BlendingSettings.ShadowDrop * SunLandDropScale : 0.0f;
}

void ShadowManager::BeginShadowMap(ShadowsExteriorEffect::ShadowMapSettings* Map, D3DXMATRIX* LightViewProj, bool keepLandSet) {
	NiDX9RenderState* state = TheRenderManager->renderState;

	// The light's view-projection, and the cull frustum made from it.
	Map->ShadowCameraToLight = *LightViewProj;
	TheCameraManager->SetFrustum(&Map->ShadowMapFrustum, LightViewProj);

	// Every map starts from this state, set in this order.
	struct MapState { D3DRENDERSTATETYPE type; UInt32 value; };
	static const MapState mapStates[] = {
		{ D3DRS_ZENABLE, D3DZB_TRUE },
		{ D3DRS_ZWRITEENABLE, D3DZB_TRUE },
		{ D3DRS_CULLMODE, D3DCULL_NONE },
		{ D3DRS_ALPHABLENDENABLE, FALSE },
		{ D3DRS_ALPHATESTENABLE, FALSE },
		{ D3DRS_ALPHAREF, 0 },
		{ D3DRS_ALPHAFUNC, D3DCMP_ALWAYS },
		{ D3DRS_DEPTHBIAS, 0 },
		{ D3DRS_SLOPESCALEDEPTHBIAS, 0 },
	};
	for (const MapState& entry : mapStates)
		state->SetRenderState(entry.type, entry.value, RenderStateArgs);
	RenderPass::LandDrop = ShadowMapLandDrop();
	if (!keepLandSet) RenderPass::LandSet.clear();
}

void ShadowManager::WalkShadowMap(ShadowsExteriorEffect::ShadowMapSettings* Map) {
	NiFrustumPlanes* planes = &Map->ShadowMapFrustumPlanes;
#if NVR_TEST_TOOLS
	SunShadowStats::Cascade* stats = SunShadowStats::Current;
	unsigned long long t0 = stats ? SunShadowStats::Now() : 0;
#endif
	// Distant land and object LOD first (when this map takes LOD) ...
	if (Map->Forms.Lod) {
		AccumChildren(BGSTerrainManager::GetRootLandLODNode(), &Map->Forms, true, true, planes);
		AccumChildren(BGSTerrainManager::GetRootObjectLODNode(), &Map->Forms, false, true, planes);
	}
#if NVR_TEST_TOOLS
	unsigned long long t1 = stats ? SunShadowStats::Now() : 0;
#endif

	// ... then the loaded cells: the whole grid outside, the player's own cell inside.
	if (!Player->GetWorldSpace()) {
		AccumExteriorCell(Player->parentCell, Map);
	}
	else {
		GridCellArray* grid = Tes->gridCellArray;
		const UInt32 cellCount = grid->size * grid->size;
		for (UInt32 c = 0; c < cellCount; ++c)
			AccumExteriorCell(grid->GetCell(c), Map);
	}
#if NVR_TEST_TOOLS
	if (stats) { const unsigned long long t2 = SunShadowStats::Now(); stats->tWalkLod += t1 - t0; stats->tWalkCells += t2 - t1; }
#endif
}

void ShadowManager::DrawShadowMap(ShadowsExteriorEffect::ShadowMapSettings* Map, bool keepLandSet) {
	IDirect3DDevice9* dev = TheRenderManager->device;
#if NVR_TEST_TOOLS
	SunShadowStats::Cascade* stats = SunShadowStats::Current;
	unsigned long long t2 = stats ? SunShadowStats::Now() : 0;
#endif

	// The map's own rectangle of the atlas, cleared to the far value (white) and depth 1, or to its own clear colour.
	D3DVIEWPORT9* rect = &Map->ShadowMapViewPort;
	dev->SetViewport(rect);
	const D3DXCOLOR farWhite(1.0f, 1.0f, 1.0f, 1.0f);
	dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, farWhite, 1.0f, 0);
	if (Map->CustomClearRequired)
		ClearShadowCascade(rect, &Map->ClearColor);

	RenderPass::TwoSided = TheSettingManager->GetSettingI("Shaders.ShadowsExteriors.Main", "TwoSidedCasters") != 0;	// UNOFFICIAL
#if NVR_TEST_TOOLS
	unsigned long long t3 = stats ? SunShadowStats::Now() : 0;
#endif
	this->RenderAccums();
#if NVR_TEST_TOOLS
	if (stats) {
		const unsigned long long t4 = SunShadowStats::Now();
		stats->tClear += t3 - t2; stats->tDraws += t4 - t3;
	}
#endif
	RenderPass::TwoSided = false;
	RenderPass::LandDrop = 0.0f;
	if (!keepLandSet) RenderPass::LandSet.clear();
}

// One map in full: state, walk, draws.
void ShadowManager::RenderShadowMap(ShadowsExteriorEffect::ShadowMapSettings* Map, D3DXMATRIX* LightViewProj) {
	BeginShadowMap(Map, LightViewProj);
	WalkShadowMap(Map);
	DrawShadowMap(Map);
}


void ShadowManager::AccumExteriorCell(TESObjectCELL* Cell, ShadowsExteriorEffect::ShadowMapSettings* Map) {
	if (Cell == nullptr || Cell->IsInterior()) return;

	ShadowsExteriorEffect::FormsStruct* forms = &Map->Forms;
	NiFrustumPlanes* planes = &Map->ShadowMapFrustumPlanes;

	// The cell's land (unless this map leaves terrain out).
	if (forms->Terrain && RenderPass::LayerMode != 2) {	// (UNOFFICIAL SunLayers: the land is in the static layer)
		NiNode* landRoot = Cell->GetChildNode(TESObjectCELL::kCellNode_Land);
		AccumulatingLand = true;
#if NVR_TEST_TOOLS
		OrthoProbe::CurrentLand = true; OrthoProbe::CurrentRef = nullptr;
		if (OrthoProbe::Recording) { OrthoProbe::LandCount[11]++; if (!landRoot) OrthoProbe::LandCount[12]++; }	// 11 cells, 12 without a land node
#endif
		AccumChildren(landRoot, forms, true, false, planes);
		AccumulatingLand = false;
#if NVR_TEST_TOOLS
		OrthoProbe::CurrentLand = false;
#endif
	}

	// Then every reference in it that casts, is in the map's frustum and does not refract.
	for (TList<TESObjectREFR>::Entry* it = &Cell->objectList.First; it != nullptr; it = it->next) {
		TESObjectREFR* ref = it->item;
		NiNode* refRoot = GetRefNode(ref, forms);
#if NVR_TEST_TOOLS
		if (SunShadowStats::Current) { SunShadowStats::Current->refs++; if (refRoot) SunShadowStats::Current->refNodes++; }
#endif
		if (refRoot == nullptr) continue;

		// UNOFFICIAL 2026-10-31 SunLayers: a static layer takes the still references (all their geometry but what moves by itself), the
		// overlay the moving ones and those a static layer held something back from
		if (RenderPass::LayerMode) {
			const bool moves = RefMovesByKind(ref, refRoot) || (RenderPass::LayerMode == 2 && LayerOverlayRefs && LayerOverlayRefs->count(ref));
			if (moves != (RenderPass::LayerMode == 2)) continue;
		}

		if (!refRoot->WithinFrustum(planes) || IsRefracting(ref)) continue;
#if NVR_TEST_TOOLS
		OrthoProbe::CurrentRef = ref;
		if (SunShadowStats::Current) { SunShadowStats::Current->refsIn++; SunShadowStats::CheckRef(refRoot, ref); }
#endif
		LayerAccumRef = ref;
		AccumChildren(refRoot, forms, false, false, planes);
		LayerAccumRef = nullptr;
#if NVR_TEST_TOOLS
		OrthoProbe::CurrentRef = nullptr;
#endif
	}
}


// UNOFFICIAL 2026-10-21 (sunshadow) SunCascadeOneWalk. Measured (evidence\sunshadow\LOG.md): every sun cascade walked every
// reference of the loaded cells again (0.04-0.08 ms per cascade on a fast PC, 25-30% of the cascades' CPU time in open country).
// The walk below visits each reference and node once and tests it against every cascade in 'mask' (bit c = cascade c), with each
// cascade's own forms, size limit (MinRadius) and cull planes, exactly as AccumExteriorCell / AccumChildren / AccumObject test it
// for one cascade. A subtree is left the moment no cascade wants it. The order is that of the old walk: the same sections (land
// LOD, object LOD, then per cell its land and its references), and within a reference the same depth-first order (containers on
// a stack, popped last-in first, geometry taken as met), so every cascade's casters come out in the order its own walk found them:
// pushed into the passes in that order (PushSunCasters), every cascade draws exactly what and as it did. SunCascadeVerify 1 checks
// that in game, frame by frame.
static inline unsigned ForEachSunCascade(unsigned mask, unsigned& bit) {	// lowest set bit index, cleared from mask
	unsigned long index;
	_BitScanForward(&index, mask);
	bit = index;
	return mask & (mask - 1);
}

// UNOFFICIAL 2026-11-04 (from Physically-Based-Reloaded ShadowManager::CastsIntoView, per cascade of the one walk) caster culling: the
// bound swept the way the light travels (a capsule) must reach what the cascade shades, the view inside the cascade's sphere, or
// nothing it shadows is ever seen. The sweep runs as far as a shadow falls from the bound's top to CasterDrop below its bottom. A
// capsule wholly behind one of the view's sides, or wholly outside the cascade sphere, is out. Conservative. Covered casters
// (SkipCoveredCasters): Shadow.hlsl takes a pixel from the first cascade whose sphere holds it and cross-fades into the next over
// the band (ShadowCascadeBands); a capsule wholly inside an earlier cascade's sphere less its band (less the margin) only shades
// pixels that cascade answers for, so it is left out of this one. Only the cascades RenderShadowMaps marks (drawn fresh every
// frame: a kept cascade would show what was left out as the camera turns). Returns the cascades of mask the caster still goes to.
unsigned ShadowManager::SunCastMask(const NiBound* Bound, unsigned mask) {
	if (!(mask & SunCastAny) || !Bound) return mask;
	const D3DXVECTOR3 start(Bound->Center.x - TheRenderManager->CameraPosition.x, Bound->Center.y - TheRenderManager->CameraPosition.y,
		Bound->Center.z - TheRenderManager->CameraPosition.z);
	const float r = Bound->Radius;
	const ShadowsExteriorEffect::ShadowMapStruct& settings = TheShaderManager->Effects.ShadowsExteriors->Settings.ShadowMaps;
	const float length = (std::min)((2.0f * r + settings.CasterDrop) * SunCastPerHeight, 200000.0f);
	const D3DXVECTOR3 end = start + SunCastDir * length;
	for (unsigned rest = mask & SunCastAny, c; rest; ) {
		rest = ForEachSunCascade(rest, c);
		bool out = false;
		for (int k = 0; k < SunCoveringCount[c] && !out; k++) {
			const D3DXVECTOR3 centre(SunCovering[c][k].x, SunCovering[c][k].y, SunCovering[c][k].z);
			const float inner = SunCovering[c][k].w - settings.CasterMargin - r;
			if (inner <= 0.0f) continue;
			const D3DXVECTOR3 a = start - centre, b = end - centre;
			if (D3DXVec3LengthSq(&a) <= inner * inner && D3DXVec3LengthSq(&b) <= inner * inner) { SunCastersCovered++; out = true; }
		}
		if (!out && (SunCastView & (1u << c))) {
			const D3DXPLANE* planes = TheShaderManager->Effects.ShadowsExteriors->ShadowMaps[c].CasterPlanes;
			for (int p = 0; p < 4 && !out; p++)
				if (D3DXPlaneDotCoord(&planes[p], &start) < -r && D3DXPlaneDotCoord(&planes[p], &end) < -r) out = true;
			if (!out) {	// the sweep's nearest point to the sphere's centre
				const D3DXVECTOR3 centre(SunCastSphere[c].x, SunCastSphere[c].y, SunCastSphere[c].z);
				const D3DXVECTOR3 toCentre = centre - start;
				const float along = std::clamp(D3DXVec3Dot(&toCentre, &SunCastDir), 0.0f, length);
				const D3DXVECTOR3 nearest = start + SunCastDir * along - centre;
				const float reach = SunCastSphere[c].w + r + settings.CasterMargin;
				out = D3DXVec3LengthSq(&nearest) > reach * reach;
			}
			if (out) SunCastersCulled++;
		}
		if (out) mask &= ~(1u << c);
	}
	return mask;
}

void ShadowManager::WalkSunCascades(unsigned mask) {
	for (int c = 0; c < 4; c++) SunCasters[c].clear();
	if (!mask) return;

	// The LOD sections are walked only when some cascade in the mask uses LOD forms.
	unsigned lodMask = 0;
	for (unsigned rest = mask, c; rest; ) { rest = ForEachSunCascade(rest, c); if (SunForms[c]->Lod) lodMask |= 1u << c; }
	if (lodMask) {
		WalkSunChildren(BGSTerrainManager::GetRootLandLODNode(), lodMask, true, true);
		WalkSunChildren(BGSTerrainManager::GetRootObjectLODNode(), lodMask, false, true);
	}
	if (!Player->GetWorldSpace()) {
		WalkSunCell(Player->parentCell, mask);
		return;
	}
	GridCellArray* cellArray = Tes->gridCellArray;
	const UInt32 cellCount = cellArray->size * cellArray->size;
	for (UInt32 i = 0; i < cellCount; i++) WalkSunCell(cellArray->GetCell(i), mask);
}

void ShadowManager::WalkSunCell(TESObjectCELL* Cell, unsigned mask) {
	if (!Cell || Cell->IsInterior()) return;

	unsigned landMask = 0;
	for (unsigned rest = mask, c; rest; ) { rest = ForEachSunCascade(rest, c); if (SunForms[c]->Terrain) landMask |= 1u << c; }
	if (landMask) {
		AccumulatingLand = true;
#if NVR_TEST_TOOLS
		OrthoProbe::CurrentLand = true; OrthoProbe::CurrentRef = nullptr;
#endif
		WalkSunChildren(Cell->GetChildNode(TESObjectCELL::kCellNode_Land), landMask, true, false);
		AccumulatingLand = false;
#if NVR_TEST_TOOLS
		OrthoProbe::CurrentLand = false;
#endif
	}

	for (TList<TESObjectREFR>::Entry* entry = &Cell->objectList.First; entry; entry = entry->next) {
		// GetRefNode per cascade: its form filter is the cascade's own (the node, when there is one, is the same).
		NiNode* refNode = nullptr;
		unsigned inMask = 0;
		for (unsigned rest = mask, c; rest; ) {
			rest = ForEachSunCascade(rest, c);
			NiNode* node = GetRefNode(entry->item, SunForms[c]);
			if (!node) continue;
			refNode = node;
			if (node->WithinFrustum(SunPlanes[c])) inMask |= 1u << c;
		}
		if (inMask && refNode) inMask = SunCastMask(refNode->GetWorldBound(), inMask);	// UNOFFICIAL 2026-11-04 CasterCulling
		if (!inMask || IsRefracting(entry->item)) continue;
#if NVR_TEST_TOOLS
		OrthoProbe::CurrentRef = entry->item;
#endif
		WalkSunChildren(refNode, inMask, false, false);
#if NVR_TEST_TOOLS
		OrthoProbe::CurrentRef = nullptr;
#endif
	}
}

void ShadowManager::WalkSunChildren(NiAVObject* Root, unsigned mask, bool isLand, bool isLOD) {
	if (Root == nullptr) return;

	const bool skipLoadedLod = isLOD && !isLand && TheShaderManager->Effects.ShadowsExteriors->Settings.ShadowMaps.LodSkipLoaded;
	// The cascades of 'm' whose size limit this object's bound reaches (land has none).
	auto bigEnough = [this, isLand](NiAVObject* object, unsigned m) {
		if (isLand) return m;
		const float radius = object->GetWorldBoundRadius();
		unsigned out = 0;
		for (unsigned c; m; ) { m = ForEachSunCascade(m, c); if (radius >= SunForms[c]->MinRadius) out |= 1u << c; }
		return out;
	};

	std::vector<std::pair<NiAVObject*, unsigned>>& pending = SunContainers;
	pending.clear();
	// Geometry is taken at once; a container waits in 'pending' with the cascades still interested in it.
	auto queueOrTake = [&](NiAVObject* object, unsigned m, bool lodLand) {
		if (object->IsGeometry()) WalkSunObject(object, m, lodLand);
		else pending.emplace_back(object, m);
	};
	queueOrTake(Root, mask, isLand && isLOD);

	while (!pending.empty()) {
		NiAVObject* const object = pending.back().first;
		unsigned m = pending.back().second;
		pending.pop_back();

		if (object == nullptr) continue;
		NiNode* node = object->IsNiNode();
		if (!node || (node->m_flags & NiAVObject::NiFlags::APP_CULLED)) continue; // culling containers
		m = bigEnough(node, m);
		if (!m) continue;

		if (node->IsKindOf<NiSwitchNode>()) {
			// NiSwitchNode - only the active child (if there is one) goes to the shadow map.
			NiSwitchNode* switchNode = static_cast<NiSwitchNode*>(node);
			if (switchNode->m_iIndex < 0 || switchNode->m_iIndex >= node->m_children.end)
				continue;

			NiAVObject* active = node->m_children.data[switchNode->m_iIndex];
			if (!active || (skipLoadedLod && IsLodInLoadedCells(active)))
				continue;
			queueOrTake(active, m, false);
			continue;
		}

		for (int i = 0; i < node->m_children.end; i++) {
			NiAVObject* child = node->m_children.data[i];
			if (!child || (child->m_flags & NiAVObject::NiFlags::APP_CULLED)) continue; // culling children
			unsigned cm = bigEnough(child, m);
			if (!cm) continue;

			// Frustum culling, per cascade.
			if (isLand || isLOD) {
				BSMultiBoundNode* mbNode = child->IsMultiBoundNode();
				if (mbNode) {
					unsigned in = 0;
					for (unsigned t = cm, c; t; ) { t = ForEachSunCascade(t, c); if (mbNode->spMultiBound->spShape->WithinFrustum(*SunPlanes[c])) in |= 1u << c; }
					cm = in;
				}
			}
			else {
				unsigned in = 0;
				for (unsigned t = cm, c; t; ) { t = ForEachSunCascade(t, c); if (child->WithinFrustum(SunPlanes[c])) in |= 1u << c; }
				cm = in;
			}
			if (!cm) continue;
			cm = SunCastMask(child->GetWorldBound(), cm);	// UNOFFICIAL 2026-11-04 CasterCulling: its shadow cannot reach the view
			if (!cm) continue;

			if (child->IsFadeNode() && static_cast<BSFadeNode*>(child)->FadeAlpha < TheShaderManager->Effects.ShadowsExteriors->Settings.ShadowMaps.CasterFadeMin) { FrameJournal::FadeDrops++; continue; } // stop rendering fadenodes below a certain opacity (UNOFFICIAL 2026-10-28: CasterFadeMin, 0.75 = as before)
			if (skipLoadedLod && IsLodInLoadedCells(child)) continue; // object LOD inside the loaded cells
			queueOrTake(child, cm, isLand && isLOD);
		}
	}
}

// AccumObject for the cascades of 'mask': the tests that do not depend on the cascade once, the pass chosen per cascade (its forms
// decide the terrain LOD and alpha passes), in AccumObject's order.
void ShadowManager::WalkSunObject(NiAVObject* Object, unsigned mask, bool isLODLand) {
	auto* geo = static_cast<NiGeometry*>(Object);
	if (geo->shader == nullptr || !CheckShaderFlags(geo)) return;	// as in AccumObject
#if NVR_TEST_TOOLS
	OrthoProbe::NoteGeometry(geo);
#endif
	auto add = [this](unsigned m, NiGeometry* g, UInt8 pass) {
		for (unsigned c; m; ) { m = ForEachSunCascade(m, c); SunCasters[c].push_back({ g, pass }); }
	};
	const int skinned = skinnedGeoPass->TestObject(geo);
	if (skinned) { if (skinned == 1) add(mask, geo, kSunSkinned); return; }
	if (speedTreePass->TestObject(geo)) { add(mask, geo, kSunTree); return; }

	int lodLand = -1, alpha = -1, opaque = -1;	// -1: not tested yet
	unsigned lodMask = 0, alphaMask = 0, opaqueMask = 0;
	for (unsigned m = mask, c; m; ) {
		m = ForEachSunCascade(m, c);
		const ShadowsExteriorEffect::FormsStruct* Forms = SunForms[c];
		if (Forms->Lod && isLODLand) {
			if (lodLand < 0) lodLand = terrainLODPass->TestObject(geo);
			if (lodLand) { lodMask |= 1u << c; continue; }
		}
		if (Forms->AlphaEnabled) {
			if (alpha < 0) alpha = alphaPass->TestObject(geo);
			if (alpha) { alphaMask |= 1u << c; continue; }
		}
		if (opaque < 0) opaque = geometryPass->TestObject(geo);
		if (opaque) opaqueMask |= 1u << c;
	}
	// One push per cascade, as AccumObject's (the pass order within a geometry does not matter: one geometry, one pass per cascade).
	// UNOFFICIAL 2026-10-27: cell land near the camera goes to the displaced land's pass (DisplaceShadows.h) instead of the opaque one.
	const UInt8 opaquePass = opaqueMask && AccumulatingLand && DisplaceShadows::Takes(geo) ? kSunDisplaceLand : kSunOpaque;
	for (unsigned m = mask, c; m; ) {
		m = ForEachSunCascade(m, c);
		const unsigned bit = 1u << c;
		if (lodMask & bit) SunCasters[c].push_back({ geo, kSunLodLand });
		else if (alphaMask & bit) SunCasters[c].push_back({ geo, kSunAlpha });
		else if (opaqueMask & bit) SunCasters[c].push_back({ geo, opaquePass });
	}
	if (opaqueMask && AccumulatingLand && RenderPass::LandDrop > 0.0f) RenderPass::LandSet.insert(geo);
}

RenderPass* ShadowManager::SunPassOf(UInt8 pass) {
	switch (pass) {
	case kSunOpaque: return geometryPass;
	case kSunLodLand: return terrainLODPass;
	case kSunAlpha: return alphaPass;
	case kSunSkinned: return skinnedGeoPass;
	case kSunDisplaceLand: return &DisplaceShadows::Pass;	// UNOFFICIAL 2026-10-27
	default: return speedTreePass;
	}
}

void ShadowManager::PushSunCasters(int cascade) {
	for (const SunCaster& caster : SunCasters[cascade]) SunPassOf(caster.pass)->GeometryList.push(caster.geometry);
}

// UNOFFICIAL 2026-10-21 (sunshadow) SunCascadeSkipFlat: the cascade's projection maps every point onto one line (or one point): the
// x or y column of its view-projection is all zero. That happens when the sun is exactly overhead, (0, 0, 1) -- QuantizeSun rounds
// any sun within half a PitchStepSize of the zenith to that -- because GetCascadeViewProj's view matrix takes (0, 0, 1) as its up
// direction: parallel to the sun, the view's side axes come out as zero vectors. Every caster then turns into zero-area triangles
// (the shadow vertex shader puts every caster through that matrix): nothing can be drawn, the map stays as cleared, and its cull
// planes are zero too (the side planes cull nothing: every cascade took nearly every object of the loaded cells).
static bool FlatCascade(const D3DXMATRIX& m) {
	return (m._11 == 0.0f && m._21 == 0.0f && m._31 == 0.0f) || (m._12 == 0.0f && m._22 == 0.0f && m._32 == 0.0f);
}

#if NVR_TEST_TOOLS
// SunCascadeVerify 2: one cascade's region of the atlas read back (resolved first when the atlas is multisampled).
static unsigned BytesPerTexel(D3DFORMAT format) {
	switch (format) {
	case D3DFMT_R16F: case D3DFMT_L16: return 2;
	case D3DFMT_A16B16G16R16F: case D3DFMT_A16B16G16R16: case D3DFMT_G32R32F: return 8;
	case D3DFMT_A32B32G32R32F: return 16;
	default: return 4;	// R32F, G16R16F, G16R16, A8R8G8B8, ...
	}
}
static bool ReadCascadeRegion(ShadowsExteriorEffect* Shadows, const D3DVIEWPORT9& viewport, std::vector<UInt8>& out) {
	IDirect3DDevice9* Device = TheRenderManager->device;
	static IDirect3DSurface9* resolved = nullptr;
	static IDirect3DSurface9* readback = nullptr;
	static D3DSURFACE_DESC have = {};
	if (!Shadows->ShadowAtlasSurface) return false;
	D3DSURFACE_DESC d = {};
	Shadows->ShadowAtlasSurface->GetDesc(&d);
	if (!resolved || have.Width != d.Width || have.Height != d.Height || have.Format != d.Format) {
		if (resolved) { resolved->Release(); resolved = nullptr; }
		if (readback) { readback->Release(); readback = nullptr; }
		if (FAILED(Device->CreateRenderTarget(d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, NULL)) ||
			FAILED(Device->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &readback, NULL))) {
			if (resolved) { resolved->Release(); resolved = nullptr; }
			return false;
		}
		have = d;
	}
	RECT r = { (LONG)viewport.X, (LONG)viewport.Y, (LONG)(viewport.X + viewport.Width), (LONG)(viewport.Y + viewport.Height) };
	IDirect3DSurface9* source = Shadows->ShadowAtlasSurfaceMSAA ? Shadows->ShadowAtlasSurfaceMSAA : Shadows->ShadowAtlasSurface;
	if (FAILED(Device->StretchRect(source, &r, resolved, &r, D3DTEXF_NONE))) return false;
	if (FAILED(Device->GetRenderTargetData(resolved, readback))) return false;
	D3DLOCKED_RECT locked = {};
	if (FAILED(readback->LockRect(&locked, &r, D3DLOCK_READONLY))) return false;
	const unsigned row = viewport.Width * BytesPerTexel(d.Format);
	out.resize((size_t)row * viewport.Height);
	for (UINT y = 0; y < viewport.Height; y++) memcpy(&out[(size_t)y * row], (const UInt8*)locked.pBits + (size_t)y * locked.Pitch, row);
	readback->UnlockRect();
	return true;
}

// SunCascadeVerify: what was compared, summed and logged every 240 checked frames.
struct SunVerifyStats { unsigned frames, cascades, listSame, listDiff, mapSame, mapDiff, mapFailed, flat; unsigned long long casters; };
static SunVerifyStats SunVerify = {};
#endif


void ShadowManager::RenderShadowSpotlight(NiSpotLight** Lights, UInt32 LightIndex) {
	NiSpotLight* light = Lights[LightIndex];
	if (light == NULL || !light->CastShadows) return;

	ShadowsExteriorEffect* shadows = TheShaderManager->Effects.ShadowsExteriors;
	ShadowsExteriorEffect::InteriorsStruct* interiors = &shadows->Settings.Interiors;
	IDirect3DDevice9* dev = TheRenderManager->device;
	NiDX9RenderState* state = TheRenderManager->renderState;
	D3DXMATRIX view, proj;

	NiPoint3* lightPos = &light->m_worldTransform.pos;
	const float radius = light->Spec.r;

	// The eye sits at the light, relative to the camera; the constants take that position and the reach.
	D3DXVECTOR3 up = D3DXVECTOR3(0, 0, 1);
	D3DXVECTOR3 eye = lightPos->toD3DXVEC3();
	eye.x -= TheRenderManager->CameraPosition.x;
	eye.y -= TheRenderManager->CameraPosition.y;
	eye.z -= TheRenderManager->CameraPosition.z;
	shadows->Constants.ShadowCubeMapLightPosition.x = eye.x;
	shadows->Constants.ShadowCubeMapLightPosition.y = eye.y;
	shadows->Constants.ShadowCubeMapLightPosition.z = eye.z;
	shadows->Constants.ShadowCubeMapLightPosition.w = radius;
	shadows->Constants.Data.z = radius;
	D3DXMatrixPerspectiveFovRH(&proj, D3DXToRadian(light->OuterSpotAngle * 2), 1.0f, 0.1f, radius);

	state->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE, RenderStateArgs);
	state->SetRenderState(D3DRS_ZWRITEENABLE, D3DZB_TRUE, RenderStateArgs);
	state->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE, RenderStateArgs);
	state->SetRenderState(D3DRS_ALPHABLENDENABLE, 0, RenderStateArgs);

	const NiMatrix33& lightRot = light->m_worldTransform.rot;
	D3DXVECTOR3 lookDir = D3DXVECTOR3(lightRot.data[0][0], lightRot.data[1][0], lightRot.data[2][0]);
	D3DXVECTOR3 target = eye + lookDir;

	// Every reference of the player's cell that lies in front of the light and within its reach adds its casters.
	// UNOFFICIAL 2026-11-04 but not the player: the flashlight is held by (or worn on) them, and the hands and gun at the light cast
	// bars across whatever it lights (third person, AttachToWeapon).
	TList<TESObjectREFR>::Entry* entry = &Player->parentCell->objectList.First;
	while (entry) {
		NiNode* refNode = entry->item == Player ? nullptr : GetRefNode(entry->item, &interiors->Forms);
		if (!refNode) {
			entry = entry->next;
			continue;
		}

		// Detect if the object is in front of the light in the direction of the current face
		// TODO: improve to base on frustum
		D3DXVECTOR3 objectPos = refNode->m_worldTransform.pos.toD3DXVEC3();
		D3DXVECTOR3 toLight = objectPos - lightPos->toD3DXVEC3();

		D3DXVec3Normalize(&toLight, &toLight);
		const bool inFront = D3DXVec3Dot(&toLight, &lookDir) > 0;
		if (inFront && refNode->GetDistance(lightPos) <= radius + refNode->GetWorldBoundRadius())
			AccumChildren(refNode, &interiors->Forms, false, false);

		entry = entry->next;
	}

	D3DXMatrixLookAtRH(&view, &eye, &target, &up);
	shadows->Constants.ShadowViewProj = view * proj;
	TheShaderManager->SpotLightWorldToLightMatrix[LightIndex] = (shadows->Constants.ShadowViewProj);

	dev->SetRenderTarget(0, shadows->Textures.ShadowSpotlightSurface[LightIndex]);
	dev->SetDepthStencilSurface(shadows->Textures.ShadowCubeMapDepthSurface);

	const D3DVIEWPORT9 spotViewport = { 0, 0, shadows->SpotMapSize, shadows->SpotMapSize, 0.0f, 1.0f };	// UNOFFICIAL 2026-11-04 its own size
	dev->SetViewport(&spotViewport);
	dev->Clear(0L, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DXCOLOR(1.0f, 1.0f, 1.0f, 1.0f), 1.0f, 0L);

	RenderAccums();
}


// UNOFFICIAL [Shaders.ShadowsInteriors.Main] PlayerInsideLamp: where a lamp casts the player's own shadow from when its
// light point is inside the player's body (or within PlayerLampMargin of it). A point inside a model is blocked on every
// side, so the player threw room-sized shadows onto every wall, often where the game places a light with no fixture and
// the player walks straight through it. The player's shadow is cast instead from a point just outside the body, chosen
// by where the light is: entering from the side, the point keeps the light's direction from the body and rises up the
// side as the light goes deeper, then moves in over the head; with the light in the middle it is straight above the head.
// So a lamp walked into from the front casts the player's shadow behind, then a shorter one, then a pool round the feet,
// then one in front as it leaves behind: no jump anywhere. The body is an upright cylinder round the player's feet, 32
// units each way times the player's scale plus the margin, up to the top of the model's bound plus the margin; at its top
// and bottom the move eases in over the margin. Returns false for a lamp outside it (nothing changes); offset: the cast
// point minus the light.
static bool PlayerLampOffset(const ShadowSceneLight* light, const NiPoint3* lightPos, D3DXVECTOR3& offset) {
	offset = D3DXVECTOR3(0.0f, 0.0f, 0.0f);
	bool now = false;
	const ShadowsExteriorEffect::InteriorsStruct& interiors = TheShaderManager->Effects.ShadowsExteriors->Settings.Interiors;
	if (Player && light && lightPos && interiors.PlayerInsideLamp) {
		const float scale = Player->scale > 0.0f ? Player->scale : 1.0f;
		const float margin = interiors.PlayerLampMargin;
		const NiPoint3& feet = Player->pos;
		float top = feet.z + 128.0f * scale;
		if (NiNode* node = Player->GetNode())
			if (NiBound* bound = node->GetWorldBound())
				top = (std::min)(bound->Center.z + bound->Radius, feet.z + 200.0f * scale);
		const float zTop = top + margin, zBottom = feet.z - margin, reach = 32.0f * scale + margin;
		const float dx = lightPos->x - feet.x, dy = lightPos->y - feet.y, d = sqrtf(dx * dx + dy * dy);
		if (d < reach && lightPos->z > zBottom && lightPos->z < zTop) {
			now = true;
			auto smooth = [](float a, float b, float x) { const float s = std::clamp((x - a) / (b - a), 0.0f, 1.0f); return s * s * (3.0f - 2.0f * s); };
			const float depth = 1.0f - d / reach;									// 0 at the side, 1 in the middle
			const float across = reach * (1.0f - smooth(0.5f, 1.0f, depth));		// the cast point's distance from the axis
			const float height = lightPos->z + (zTop - lightPos->z) * smooth(0.0f, 0.5f, depth);
			const float ux = d > 0.001f ? dx / d : 0.0f, uy = d > 0.001f ? dy / d : 0.0f;
			const float ends = (std::min)(std::clamp((zTop - lightPos->z) / margin, 0.0f, 1.0f), std::clamp((lightPos->z - zBottom) / margin, 0.0f, 1.0f));
			offset = D3DXVECTOR3(feet.x + ux * across - lightPos->x, feet.y + uy * across - lightPos->y, height - lightPos->z) * ends;
		}
	}
	// LogLamps: when a lamp starts and stops being inside.
	static const ShadowSceneLight* inside[ShadowCubeMapsMax] = {};
	int found = -1, empty = -1;
	for (int i = 0; i < ShadowCubeMapsMax; i++) {
		if (inside[i] == light) found = i;
		else if (!inside[i] && empty < 0) empty = i;
	}
	if (now && found < 0 && empty >= 0) inside[empty] = light;
	if (!now && found >= 0) inside[found] = nullptr;
	if (now != (found >= 0) && lightPos && interiors.Forward.LogLamps)
		Logger::Log("UNOFFICIAL PlayerInsideLamp: lamp %p at %.0f %.0f %.0f %s", (const void*)light, lightPos->x, lightPos->y, lightPos->z,
			now ? "is inside the player: the player is left out of its cube map" : "is outside the player again");
	return now;
}

bool ShadowManager::RenderShadowCubeMap(ShadowSceneLight** Lights, UInt32 LightIndex, CubeLayer Layer) {
	if (Lights[LightIndex] == NULL) return true; // No light at current index
	
	ShadowsExteriorEffect* fx = TheShaderManager->Effects.ShadowsExteriors;
	ShadowsExteriorEffect::InteriorsStruct* intCfg = &fx->Settings.Interiors;

	IDirect3DDevice9* dev = TheRenderManager->device;
	NiDX9RenderState* rs = TheRenderManager->renderState;

	// UNOFFICIAL RedrawActorsOnly: the moving layer starts from the static cube map, copied face by face (same size and
	// format). A failed copy draws nothing; the caller redraws the lamp whole.
	// UNOFFICIAL CubeFaceCopySkip (above): the copies are made after the caster walk, face by face, where needed.
	const bool cullFaces = CubeCullAllowed && Layer != CubeLayer::StaticOnly;	// UNOFFICIAL CubeFaceViewCull (above)
	const bool faceSkip = Layer == CubeLayer::MovingOnto && (TheSettingManager->SettingsMain.Main.CubeFaceCopySkip || cullFaces);
	const bool copySkip = TheSettingManager->SettingsMain.Main.CubeFaceCopySkip;
	if (Layer == CubeLayer::StaticOnly)
		StaticCubeSerials[fx->Textures.ShadowCubeMapStaticTexture[LightIndex]] = ++StaticCubeSerialNext;	// its contents change now
	if (!faceSkip) ForgetLiveFaces(LightIndex);	// a whole redraw, or the copies and draws below without the records
	if (Layer == CubeLayer::MovingOnto && !faceSkip) {
		for (int Face = 0; Face < 6; Face++) {
			IDirect3DSurface9* from = fx->Textures.ShadowCubeMapStaticSurface[LightIndex][Face];
			IDirect3DSurface9* to = fx->Textures.ShadowCubeMapSurface[LightIndex][Face];
			if (!from || !to || FAILED(dev->StretchRect(from, NULL, to, NULL, D3DTEXF_NONE))) return false;
		}
	}
	// The cube is drawn from the light (or the override eye, LampShadowMoveTolerance / InvisibleShadowFromLamp) out to its reach.
	NiPointLight* pointLight = Lights[LightIndex]->sourceLight;
	NiPoint3* eyePos = CubeEyeOverride ? CubeEyeOverride : &pointLight->m_worldTransform.pos;	// UNOFFICIAL LampShadowMoveTolerance (null: as before)
	const float cubeRange = CubeRadiusOverride > 0.0f ? CubeRadiusOverride : pointLight->Spec.r * fx->Settings.Interiors.LightRadiusMult;	// (InvisibleShadowFromLamp)
	// UNOFFICIAL (2026-10-07): New Vegas sets NiDynamicEffect::CanCarry itself for every light whose form has the "can be
	// carried" flag (NVR's hook that set it is off here), and the Oblivion torch rule above gave those lamps a 256-unit
	// cube map whatever their radius: everything they lit further out than 256 units had its shadow end on a circle,
	// with the light going on. The cube now covers Spec.r x LightRadiusMult for every lamp, the radius the lookups use.

	// The eye relative to the camera (the shaders work camera relative), and the 90 degree projection every face shares.
	const D3DXVECTOR4& camPos = TheRenderManager->CameraPosition;
	D3DXVECTOR3 eyeRel(eyePos->x - camPos.x, eyePos->y - camPos.y, eyePos->z - camPos.z);
	fx->Constants.ShadowCubeMapLightPosition = D3DXVECTOR4(eyeRel.x, eyeRel.y, eyeRel.z, cubeRange);
	fx->Constants.Data.z = cubeRange;
	D3DXMATRIX faceView, faceProj;
	D3DXVECTOR3 lookAt, upDir, camDir;
	D3DXMatrixPerspectiveFovRH(&faceProj, D3DXToRadian(90.0f), 1.0f, 0.1f, cubeRange);
	// UNOFFICIAL CubeFaceViewCull: the faces that can be seen this frame (all six unless culling applies to this draw)
	unsigned char viewFaces = 63;
	if (cullFaces) {
		static const D3DXVECTOR3 CullDirection[6] = { D3DXVECTOR3(1.0f, 0.0f, 0.0f), D3DXVECTOR3(-1.0f, 0.0f, 0.0f),
			D3DXVECTOR3(0.0f, 1.0f, 0.0f), D3DXVECTOR3(0.0f, -1.0f, 0.0f), D3DXVECTOR3(0.0f, 0.0f, -1.0f), D3DXVECTOR3(0.0f, 0.0f, 1.0f) };
		for (int face = 0; face < 6; face++)
			if (CubeFaceOutOfView(eyePos, cubeRange, CullDirection[face])) viewFaces &= (unsigned char)~(1 << face);
		if (viewFaces != 63) CubeCulledAny = true;
	}
	D3DXVECTOR3 playerOffset;
	const bool lampInsidePlayer = PlayerLampOffset(Lights[LightIndex], eyePos, playerOffset);	// UNOFFICIAL, see above

	// Cube faces start from depth test and write on, no culling (then no blending and no alpha test, below).
	struct CubeState { D3DRENDERSTATETYPE type; UInt32 value; };
	static const CubeState depthStates[] = { { D3DRS_ZENABLE, D3DZB_TRUE }, { D3DRS_ZWRITEENABLE, D3DZB_TRUE }, { D3DRS_CULLMODE, D3DCULL_NONE } };
	static const CubeState blendStates[] = { { D3DRS_ALPHABLENDENABLE, FALSE }, { D3DRS_ALPHATESTENABLE, FALSE }, { D3DRS_ALPHAREF, 0 }, { D3DRS_ALPHAFUNC, D3DCMP_ALWAYS } };
	for (const CubeState& s : depthStates)
		rs->SetRenderState(s.type, s.value, RenderStateArgs);
	// UNOFFICIAL 2026-10-29 [Shaders.ShadowsInteriors.Main] TwoSidedCasters (live; user: lamp shadows "that seem to trace the inside
	// of a mesh", rings under cups and glasses): each caster drew with its own cull mode (RenderPass::RenderGeometry), and the cube
	// faces are mirrored (lookup (1, 1, -1), ShadowCubeMap.vso), so the side kept was the one turned AWAY from the lamp: the map held
	// a cup's inner walls and the underside of its bottom (at the table's height, lost in the bias), and the shadow came out as a ring.
	// Double-sided, as the sun's maps (TwoSidedCasters there): the face nearest the lamp wins the depth test. Off: as before.
	struct TwoSidedScope {
		explicit TwoSidedScope(bool on) { RenderPass::TwoSided = on; }
		~TwoSidedScope() { RenderPass::TwoSided = false; }
	} twoSidedScope(TheSettingManager->GetSettingI("Shaders.ShadowsInteriors.Main", "TwoSidedCasters") != 0);
	for (const CubeState& s : blendStates)
		rs->SetRenderState(s.type, s.value, RenderStateArgs);

	// UNOFFICIAL (2026-10-07 review): the light's caster list is walked ONCE for all six faces; it used to be walked once
	// per face, with the same property lookups and tests each time. Every test but the face test is the same for every
	// face, so each caster that passes them remembers which faces its bound touches and which pass draws it, and the face
	// loop only pushes. The old form-based walk (no caster list from the game) stays per face as it was.
	static const D3DXVECTOR3 FaceDirection[6] = { D3DXVECTOR3(1.0f, 0.0f, 0.0f), D3DXVECTOR3(-1.0f, 0.0f, 0.0f),
		D3DXVECTOR3(0.0f, 1.0f, 0.0f), D3DXVECTOR3(0.0f, -1.0f, 0.0f), D3DXVECTOR3(0.0f, 0.0f, -1.0f), D3DXVECTOR3(0.0f, 0.0f, 1.0f) };
	static const D3DXVECTOR3 FaceUp[6] = { D3DXVECTOR3(0.0f, 1.0f, 0.0f), D3DXVECTOR3(0.0f, 1.0f, 0.0f),
		D3DXVECTOR3(0.0f, 0.0f, 1.0f), D3DXVECTOR3(0.0f, 0.0f, -1.0f), D3DXVECTOR3(0.0f, 1.0f, 0.0f), D3DXVECTOR3(0.0f, 1.0f, 0.0f) };
	const bool useGeometryList = Lights[LightIndex]->kGeometryList.start != nullptr;
	cubeCasters.clear();
	const bool dropActors = CapsuleShadows::DropActors();	// UNOFFICIAL 2026-10-31 CapsuleShadows (interiors only)
	std::unordered_map<int, int> lampModelCache;	// UNOFFICIAL LampModelShadows: this light's answer per lamp model (LampFixtures::NearLampModel)
	std::unordered_map<int, int> noShadowCache;		// UNOFFICIAL NoShadowLampModels (2026-10-27): the same for LampFixtures::NoShadowLampModel
	if (useGeometryList) {
		// Since this is pure geometry, getting reference data will be difficult (read: slow)
		for (auto iter = Lights[LightIndex]->kGeometryList.start; iter; iter = iter->next) {
			NiGeometry* const geo = iter->data;
			if (geo == nullptr || (geo->m_flags & NiAVObject::APP_CULLED) != 0) continue;

			auto* casterShade = static_cast<BSShaderProperty*>(geo->GetProperty(NiProperty::kType_Shade));
			auto* casterMat = static_cast<NiMaterialProperty*>(geo->GetProperty(NiProperty::kType_Material));
			if (casterShade == nullptr) continue;

			// UNOFFICIAL 2026-10-31 CapsuleShadows DropActors: skinned casters (people, creatures) cast their lamp shadows as capsules
			if (geo->skinInstance && dropActors) continue;

			// UNOFFICIAL RedrawActorsOnly: the static layer takes what never moves, the moving layer the rest.
			if (Layer != CubeLayer::All && IsMovingCaster(geo, casterShade) != (Layer == CubeLayer::MovingOnto))
				continue;

			if (!CheckShaderFlags(geo)) continue;	// refraction, fire refraction, decals

			// UNOFFICIAL LampGlassShadows false (2026-10-20): a lamp's glowing glass, bulb or glow sprite is a light source, not a
			// blocker; drawn solid it threw the whole shade outline of a sconce or chandelier onto the wall (LampFixtures::LampPart).
			if (!intCfg->Forward.LampGlassShadows && LampFixtures::LampPart(geo))
				continue;
			// UNOFFICIAL OwnGlassDistance (2026-10-30): the light's own glass (a lamp part it sits in or next to) casts nothing into its map
			// (Vault 21's floor lamps lost their floor pools to it once they had a shadow slot); other lamps' maps keep it.
			if (!TheShaderManager->GameState.isExterior && LampFixtures::OwnGlass(geo, eyePos, intCfg->Forward.OwnGlassDistance))
				continue;

			// UNOFFICIAL InvisibleShadowFromLamp (2026-10-20): drawn from a lamp, that lamp (its model, and every glowing lamp part) is
			// the light source here, not a blocker.
			if (CubeRelocated && ((CubeSkipModel >= 0 && LampFixtures::ModelIndexOf(geo) == CubeSkipModel) || LampFixtures::LampPart(geo)))
				continue;

			// UNOFFICIAL LampModelShadows false (2026-10-20): the lamp models near this light (whole models: a sconce, a chandelier, a ceiling
			// fan, an arm lamp) cast nothing into its map. The level designers put their invisible lights right next to the lamps, and from
			// there a lamp threw its whole outline, hard and room-sized; every other shadow of the light stays (LampFixtures::NearLampModel).
			if (!intCfg->Forward.LampModelShadows && !TheShaderManager->GameState.isExterior &&
				LampFixtures::NearLampModel(geo, pointLight, eyePos, intCfg->Forward.LampModelDistance, lampModelCache))
				continue;
			// UNOFFICIAL NoShadowLampModels (2026-10-27): lamp models named there (default: lanterns) cast no shadow for the lights in or
			// near them, their own included (a lantern's frame threw a moving cage of shadows round the room). Other lamps keep theirs.
			if (!TheShaderManager->GameState.isExterior &&
				LampFixtures::NoShadowLampModel(geo, eyePos, intCfg->Forward.LampModelDistance, noShadowCache))
				continue;

			// The player's first-person model and third-person body carry their own shade flags.
			const bool inFirstPerson = casterShade->m_usFlags.GetBit(NiShadeProperty::kFirstPerson);
			const bool inThirdPerson = casterShade->m_usFlags.GetBit(NiShadeProperty::kThirdPerson);

			// Nearly transparent geometry casts nothing.
			if (casterMat != nullptr && casterMat->fAlpha < 0.05f) continue;

			// Never the first-person model; the body only as the switch for the current view allows.
			if (inFirstPerson) continue;
			if (inThirdPerson && !(Player->isThirdPerson ? intCfg->PlayerShadowThirdPerson : intCfg->PlayerShadowFirstPerson))
				continue;

			// UNOFFICIAL PlayerInsideLamp (v3, 2026-10-07): a lamp inside the player's body leaves the player out of its
			// cube map. v2 drew the player into the same cube map from a cast point outside the body, but the apply pass
			// and the forward path read the cube by direction from the lamp itself, so what they saw was a body displaced
			// by up to its own height: a half-disc of shadow on the floor round the lamp (user's screenshot, Rivet City).
			if (lampInsidePlayer && inThirdPerson) continue;

			// The faces the caster's bound reaches (TouchesShadowFace: every face for skinned or unbounded geometry); only
			// lighting-property geometry is face-tested, as before. SpeedTree shaders move vertices beyond their static bounds.
			unsigned char faces = 63;
			if (casterShade->IsLightingProperty()) {
				// UNOFFICIAL SkinnedShadowFaceTest: a skinned caster whose skeleton bound lies wholly beyond the cube map's far
				// plane (the lamp's radius) cannot reach any face; the game's per-light list was built from its stale bound.
				if (geo->skinInstance && TheSettingManager->SettingsMain.Main.SkinnedShadowFaceTest) {
					NiBound skinned;
					if (SkinnedWorldBound(geo, skinned)) {
						const float dx = skinned.Center.x - eyePos->x, dy = skinned.Center.y - eyePos->y, dz = skinned.Center.z - eyePos->z;
						if (sqrtf(dx * dx + dy * dy + dz * dz) - skinned.Radius > cubeRange) { statSkinnedOutOfRange++; continue; }
					}
					else statSkinnedNoBound++;
				}
				faces = 0;
				for (int face = 0; face < 6; face++)
					if (TouchesShadowFace(geo, eyePos, FaceDirection[face])) faces |= (unsigned char)(1 << face);
				if (!faces) continue;
			}
			if (geo->skinInstance) {
				statSkinnedCasters++;
				for (int face = 0; face < 6; face++) if (faces & (1 << face)) statSkinnedFaces++;
			}

			// The pass that draws it: the AccumObject chain of before, without the pushes (RenderPass::TestObject).
			RenderPass* pass = nullptr;
			int test = skinnedGeoPass->TestObject(geo);
			if (test == 1) pass = skinnedGeoPass;
			if (!test) { test = speedTreePass->TestObject(geo); if (test == 1) pass = speedTreePass; }
			if (!test && intCfg->Forms.AlphaEnabled) { test = alphaPass->TestObject(geo); if (test == 1) pass = alphaPass; }
			if (!test) { test = geometryPass->TestObject(geo); if (test == 1) pass = geometryPass; }
			if (!pass) continue;
			cubeCasters.push_back({ geo, pass, faces });
		}
	}

	// UNOFFICIAL CubeFaceCopySkip: the faces the moving casters reach are copied and drawn; the others are copied, unless they still
	// hold the copy (above). A failed copy draws nothing more; the caller redraws the lamp whole (which forgets the records).
	unsigned char drawFaces = 63;
	if (faceSkip) {
		drawFaces = 0;
		for (const CubeCaster& caster : cubeCasters) drawFaces |= caster.faces;
		const void* liveTex = fx->Textures.ShadowCubeMapTexture[LightIndex];
		const void* staticTex = fx->Textures.ShadowCubeMapStaticTexture[LightIndex];
		const auto serial = StaticCubeSerials.find(staticTex);
		const unsigned staticSerial = serial != StaticCubeSerials.end() ? serial->second : 0;
		for (int Face = 0; Face < 6; Face++) {
			LiveFaceRecord& record = LiveFaces[LightIndex][Face];
			const bool holdsCopy = record.pure && staticSerial && record.live == liveTex && record.staticTex == staticTex &&
				record.serial == staticSerial && record.generation == fx->CubeGeneration;
			const bool draws = (drawFaces >> Face) & 1;
			if (!((viewFaces >> Face) & 1)) {	// UNOFFICIAL CubeFaceViewCull: out of view, left as it is (stale, never read)
				record.pure = false;
				continue;
			}
			if (!draws && holdsCopy && copySkip) {
				statFacesSkipped++;
#if NVR_TEST_TOOLS
				CubeFaceSkipSelfCheck(dev, fx->Textures.ShadowCubeMapSurface[LightIndex][Face], fx->Textures.ShadowCubeMapStaticSurface[LightIndex][Face]);
#endif
				continue;
			}
			IDirect3DSurface9* from = fx->Textures.ShadowCubeMapStaticSurface[LightIndex][Face];
			IDirect3DSurface9* to = fx->Textures.ShadowCubeMapSurface[LightIndex][Face];
			record.pure = false;
			if (!from || !to || !staticSerial || FAILED(dev->StretchRect(from, NULL, to, NULL, D3DTEXF_NONE))) { ForgetLiveFaces(LightIndex); return false; }
			if (draws) statFacesDrawn++;
			else {
				statFacesCopied++;
				record.live = liveTex; record.staticTex = staticTex; record.serial = staticSerial; record.generation = fx->CubeGeneration;
				record.pure = true;
			}
		}
	}
	else if (Layer == CubeLayer::MovingOnto) statFacesOldPath += 6;
	if (cullFaces) {
		drawFaces &= viewFaces;
		const int kind = Layer == CubeLayer::All ? 0 : 2;
		for (int face = 0; face < 6; face++) if (!((viewFaces >> face) & 1)) statFacesCulled[kind]++;
#if NVR_TEST_TOOLS
		static int poison = 0;
		static unsigned poisonFrame = 0;
		if (poisonFrame != SkinnedBoundFrame && (SkinnedBoundFrame % 30) == 0) poison = TheSettingManager->GetSettingI("Main.Main.Performance", "CubeFaceViewCullCheck");
		poisonFrame = SkinnedBoundFrame;
		if (poison > 0) {	// DIAGNOSTIC CubeFaceViewCullCheck: the left-out faces say "all in shadow"
			for (int face = 0; face < 6; face++) {
				if ((viewFaces >> face) & 1) continue;
				dev->SetRenderTarget(0, fx->Textures.ShadowCubeMapSurface[LightIndex][face]);
				dev->SetDepthStencilSurface(fx->Textures.ShadowCubeMapDepthSurface);
				{ const UINT n = fx->CubeSizeFor(LightIndex); const D3DVIEWPORT9 vp = { 0, 0, n, n, 0.0f, 1.0f }; dev->SetViewport(&vp); }	// UNOFFICIAL 2026-11-01 NearCubeSize
				dev->Clear(0L, NULL, D3DCLEAR_TARGET, D3DXCOLOR(0.1f, 0.1f, 0.1f, 0.1f), 1.0f, 0L);	// 0.1: a blocker at a tenth of the reach (0 would read as no data)
			}
		}
#endif
	}

	// UNOFFICIAL RedrawActorsOnly: the moving casters go over the copied static distances; a min blend keeps the nearer.
	if (Layer == CubeLayer::MovingOnto) {
		rs->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE, RenderStateArgs);
		rs->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_MIN, RenderStateArgs);
		rs->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE, RenderStateArgs);
		rs->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE, RenderStateArgs);
	}

	// UNOFFICIAL LogLamps diagnostic (2026-10-08): in the TOPS casino a ceiling fill light's forward shadow blacked out whole
	// floor pieces. What a lamp's cube map draws close to the lamp decides that, so the first time each lamp's static (or
	// whole) cube map is drawn, the casters within a fifth of its reach of it are logged (CASTERS lines): name, shader type,
	// alpha blending/testing, material alpha, which pass draws it, how far its bound is from the lamp (negative: around it).
	if (intCfg->Forward.LogLamps && useGeometryList && Layer != CubeLayer::MovingOnto) {
		static std::unordered_map<const void*, bool> loggedLamps;
		static unsigned casterLines = 0;
		const ShadowSceneLight* lamp = Lights[LightIndex];
		if (casterLines < 600 && !loggedLamps[lamp]) {
			loggedLamps[lamp] = true;
			const float nearLimit = (std::max)(48.0f, 0.2f * pointLight->Spec.r);
			int nearCount = 0;
			for (const CubeCaster& caster : cubeCasters) {
				const NiBound* b = caster.geometry->m_kWorldBound;
				if (!b) continue;
				const float dx = b->Center.x - eyePos->x, dy = b->Center.y - eyePos->y, dz = b->Center.z - eyePos->z;
				if (sqrtf(dx * dx + dy * dy + dz * dz) - b->Radius < nearLimit) nearCount++;
			}
			casterLines++;
			Logger::Log("CASTERS lamp %p at %.0f %.0f %.0f, reach %.0f, cube radius %.0f, %s: %u casters, %d within %.0f of the lamp:",
				(const void*)lamp, eyePos->x, eyePos->y, eyePos->z, pointLight->Spec.r, cubeRange,
				Layer == CubeLayer::StaticOnly ? "static layer" : "whole", (unsigned)cubeCasters.size(), nearCount, nearLimit);
			int listed = 0;
			for (const CubeCaster& caster : cubeCasters) {
				NiGeometry* geo = caster.geometry;
				const NiBound* b = geo->m_kWorldBound;
				if (!b || listed >= 14 || casterLines >= 600) continue;
				const float dx = b->Center.x - eyePos->x, dy = b->Center.y - eyePos->y, dz = b->Center.z - eyePos->z;
				const float gap = sqrtf(dx * dx + dy * dy + dz * dz) - b->Radius;
				if (gap >= nearLimit) continue;
				NiShadeProperty* shade = static_cast<NiShadeProperty*>(geo->GetProperty(NiProperty::kType_Shade));
				NiAlphaProperty* alpha = static_cast<NiAlphaProperty*>(geo->GetProperty(NiProperty::kType_Alpha));
				NiMaterialProperty* material = static_cast<NiMaterialProperty*>(geo->GetProperty(NiProperty::kType_Material));
				const char* passName = caster.pass == skinnedGeoPass ? "skinned" : caster.pass == speedTreePass ? "speedtree" : caster.pass == alphaPass ? "alpha-tested" : "opaque";
				listed++;
				casterLines++;
				Logger::Log("CASTERS   %s %p: shader type %d, alpha %s%s, material alpha %.2f, drawn as %s, bound radius %.0f, %.0f from the lamp%s",
					geo->m_pcName ? geo->m_pcName : "(unnamed)", (const void*)geo, shade ? (int)shade->m_eShaderType : -1,
					alpha && (alpha->flags & NiAlphaProperty::ALPHA_BLEND_MASK) ? "blended" : "not blended",
					alpha && (alpha->flags & NiAlphaProperty::TEST_ENABLE_MASK) ? ", tested" : "", material ? material->fAlpha : -1.0f,
					passName, b->Radius, gap, gap < 0.0f ? " (around the lamp)" : "");
			}
		}
	}

	// UNOFFICIAL (Fable): the form-based fallback selects the same references for every face (its old per-face
	// "in front" test was computed and never used), so they are collected once per light instead of six times.
	fallbackRefsScratch.clear();
	if (!useGeometryList) {
		// The player's cell's references that cast and whose bound reaches into the cube.
		for (auto* it = &Player->parentCell->objectList.First; it != nullptr; it = it->next) {
			TESObjectREFR* ref = it->item;
			NiNode* refRoot = GetRefNode(ref, &intCfg->Forms);
			if (refRoot == nullptr) continue;
			const bool inReach = refRoot->GetDistance(eyePos) <= cubeRange + refRoot->GetWorldBoundRadius();
			if (inReach && !IsRefracting(ref)) fallbackRefsScratch.push_back(refRoot);
		}
	}

	for (int Face = 0; Face < 6; Face++) {
		if (!((drawFaces >> Face) & 1)) continue;	// UNOFFICIAL CubeFaceCopySkip: nothing reaches it (it holds the static copy)
		if (GpuTimer::Enabled) {	// UNOFFICIAL diagnostic (intshadow), see CubeFaceOutOfView
			const int kind = Layer == CubeLayer::All ? 0 : Layer == CubeLayer::StaticOnly ? 1 : 2;
			statViewFaces[kind]++;
			if (CubeFaceOutOfView(eyePos, cubeRange, FaceDirection[Face])) statViewFacesOut[kind]++;
		}
		camDir = FaceDirection[Face];
		upDir = FaceUp[Face];
		lookAt = eyeRel + camDir;

		if (useGeometryList) {
			const unsigned char bit = (unsigned char)(1 << Face);
			for (const CubeCaster& caster : cubeCasters)
				if (caster.faces & bit) caster.pass->GeometryList.push(caster.geometry);
		}
		else {
			// old form based geo accumulation when the one perform by the game has not handled this light
			// (references gathered once per light, above the face loop)
			for (NiNode* refRoot : fallbackRefsScratch)
				AccumChildren(refRoot, &intCfg->Forms, false, false);
		}

		D3DXMatrixLookAtRH(&faceView, &eyeRel, &lookAt, &upDir);
		fx->Constants.ShadowViewProj = faceView * faceProj;

		dev->SetRenderTarget(0, Layer == CubeLayer::StaticOnly ? fx->Textures.ShadowCubeMapStaticSurface[LightIndex][Face]
			: fx->Textures.ShadowCubeMapSurface[LightIndex][Face]);
		dev->SetDepthStencilSurface(fx->Textures.ShadowCubeMapDepthSurface);

		// UNOFFICIAL 2026-11-01 NearCubeSize: the slot's own face size (the depth surface is the largest cube's)
		{ const UINT n = fx->CubeSizeFor(LightIndex); const D3DVIEWPORT9 vp = { 0, 0, n, n, 0.0f, 1.0f }; dev->SetViewport(&vp); }
		// (MovingOnto keeps the copied distances: only the depth buffer starts empty)
		dev->Clear(0L, NULL, Layer == CubeLayer::MovingOnto ? D3DCLEAR_ZBUFFER : D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DXCOLOR(1.0f, 1.0f, 1.0f, 1.0f), 1.0f, 0L);

		RenderAccums();
	}

	if (Layer == CubeLayer::MovingOnto) {
		rs->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD, RenderStateArgs);
		rs->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE, RenderStateArgs);
	}
	return true;
}


// Sets a shade-property flag on every geometry under Object: the player's models carry kFirstPerson / kThirdPerson, which the
// cube map caster test (RenderShadowCubeMap) reads.
static void MarkShadeFlag(NiAVObject* Object, UInt32 Flag) {
	if (Object == nullptr) return;
	if (!Object->IsGeometry()) {
		if (Object->IsNiNode()) {
			NiNode* node = static_cast<NiNode*>(Object);
			for (UInt32 c = 0; c < node->m_children.end; ++c)
				MarkShadeFlag(node->m_children.data[c], Flag);
		}
		return;
	}
	auto* shade = static_cast<NiShadeProperty*>(static_cast<NiGeometry*>(Object)->GetProperty(NiProperty::kType_Shade));
	if (shade) shade->m_usFlags.Set(Flag);
}

// Calls since the player's models were last flagged. The flags are refreshed on the 51st call and every 51 calls after it
// (so newly equipped models get theirs within about a second).
static SInt32 playerFlagCalls = -1;
static void FlagPlayerGeometry() {
	if (++playerFlagCalls <= 50) return;
	if (Player->firstPersonNiNode) MarkShadeFlag(Player->firstPersonNiNode, NiShadeProperty::kFirstPerson);
	if (NiNode* body = Player->GetNode()) MarkShadeFlag(body, NiShadeProperty::kThirdPerson);
	playerFlagCalls = 0;
}

// Billboard axes that face the sun (SpeedTree leaf cards in the sun maps): right = world up x sun (the x axis when the sun is
// straight above or below), billboard up = sun x right; both unit length, w = 0.
void ShadowManager::RecalculateBillboardVectors(D3DXVECTOR3* SunDir) {
	static const D3DXVECTOR3 zAxis(0.0f, 0.0f, 1.0f);
	D3DXVECTOR3 right, up;
	D3DXVec3Cross(&right, &zAxis, SunDir);
	if (D3DXVec3LengthSq(&right) < 0.0001f)
		right = D3DXVECTOR3(1.0f, 0.0f, 0.0f);
	D3DXVec3Normalize(&right, &right);
	D3DXVec3Cross(&up, SunDir, &right);
	D3DXVec3Normalize(&up, &up);
	BillboardRight = NiVector4(right.x, right.y, right.z, 0.0f);
	BillboardUp = NiVector4(up.x, up.y, up.z, 0.0f);
}

/*
* Renders the different shadow maps: Near, Far, Ortho.
*/
// CachedDistantShadows: the characters (and the player) in the loaded cells, as world bound spheres.
void ShadowManager::CollectMovers(const D3DXVECTOR3& SunDir) {
	FrameMovers.clear();
	// How far a shadow reaches sideways per unit of height, for the current sun (capped for a sun near the horizon).
	MoverShadowStretch = min(sqrtf(SunDir.x * SunDir.x + SunDir.y * SunDir.y) / max(SunDir.z, 0.1f), 10.0f);
	auto add = [this](NiNode* node) {
		if (!node || node->m_flags & NiAVObject::APP_CULLED) return;
		NiBound* bound = node->GetWorldBound();
		if (bound) FrameMovers.push_back(D3DXVECTOR4(bound->Center.x, bound->Center.y, bound->Center.z, bound->Radius));
	};
	add(Player->GetNode());
	if (!Player->GetWorldSpace() || !Tes || !Tes->gridCellArray) return;
	GridCellArray* CellArray = Tes->gridCellArray;
	const UInt32 CellArraySize = CellArray->size * CellArray->size;
	for (UInt32 c = 0; c < CellArraySize; c++) {
		TESObjectCELL* Cell = CellArray->GetCell(c);
		if (!Cell || Cell->IsInterior()) continue;
		for (TList<TESObjectREFR>::Entry* Entry = &Cell->objectList.First; Entry; Entry = Entry->next) {
			TESObjectREFR* Ref = Entry->item;
			if (!Ref || !Ref->baseForm) continue;
			const UInt8 type = Ref->baseForm->formType;
			if (type == TESForm::FormType::kFormType_NPC || type == TESForm::FormType::kFormType_Creature ||
				type == TESForm::FormType::kFormType_LeveledCreature)
				add(Ref->GetNode());
		}
	}
}

// Whether a character's shadow could come from this cascade. The planes are the ones from the cascade's last
// redraw (world space); the cached picture has moved with the camera since, so the test is widened by that distance.
// On screen, everything nearer than the previous cascade's end (InnerDepth, view depth) is shaded by the sharper
// cascades: their selection spheres enclose their slices of the view, and only their outer 10% cross-fades into the
// next one. So a character whose whole shadow stays nearer than 90% of InnerDepth is never sampled from this cascade
// and does not count; that keeps the player and anyone close by from forcing a redraw every frame.
bool ShadowManager::MoversInCascade(int cascade, ShadowsExteriorEffect::ShadowMapSettings* ShadowMap, float InnerDepth) {
	if (!ShadowMap->Forms.Actors || FrameMovers.empty()) return false;
	const D3DXVECTOR3 camera = WorldSceneGraph->camera->m_worldTransform.pos.toD3DXVEC3();
	const D3DXVECTOR3 forward(TheRenderManager->CameraForward.x, TheRenderManager->CameraForward.y, TheRenderManager->CameraForward.z);
	const D3DXVECTOR3 moved = camera - CascadeRefreshCamera[cascade];
	const float pad = D3DXVec3Length(&moved) + 64.0f;
	NiFrustumPlanes* planes = &ShadowMap->ShadowMapFrustumPlanes;
	for (const D3DXVECTOR4& mover : FrameMovers) {
		const D3DXVECTOR3 toMover = D3DXVECTOR3(mover.x, mover.y, mover.z) - camera;
		const float shadowReach = mover.w * (1.0f + 2.0f * MoverShadowStretch) + 64.0f;
		if (D3DXVec3Dot(&toMover, &forward) + shadowReach < 0.9f * InnerDepth) continue;
		bool inside = true;
		for (UInt32 p = 0; p < NiFrustumPlanes::MaxPlanes && inside; p++) {
			if (!planes->IsPlaneActive(p)) continue;
			const NiPlane& plane = planes->CullingPlanes[p];
			const float distance = plane.Normal.x * mover.x + plane.Normal.y * mover.y + plane.Normal.z * mover.z - plane.Constant;
			if (distance < -(mover.w + pad)) inside = false;
		}
		if (inside) return true;
	}
	return false;
}

// With the F10 profile and CachedDistantShadows on: every 240 frames, how often each sun cascade was redrawn on its
// schedule and how often because of characters.
void ShadowManager::LogSunShadowStats(bool cachedDistant) {
	if (!GpuTimer::Enabled || !cachedDistant) { SunStats = {}; return; }
	SunStats.Characters += (unsigned)FrameMovers.size();
	if (++SunStats.Frames < 240) return;
	const float perFrame = 100.0f / SunStats.Frames;
	Logger::Log("CACHED DISTANT SHADOWS %u frames: redrawn %% of frames (scheduled + for characters): near %.0f, "
		"middle %.0f + %.0f, far %.0f + %.0f, LOD %.0f + %.0f; characters tracked %.1f",
		SunStats.Frames, SunStats.Scheduled[MapNear] * perFrame,
		SunStats.Scheduled[MapMiddle] * perFrame, SunStats.ForCharacters[MapMiddle] * perFrame,
		SunStats.Scheduled[MapFar] * perFrame, SunStats.ForCharacters[MapFar] * perFrame,
		SunStats.Scheduled[MapLod] * perFrame, SunStats.ForCharacters[MapLod] * perFrame,
		SunStats.Characters / (float)SunStats.Frames);
	SunStats = {};
	// UNOFFICIAL 2026-10-31 SunCacheByChange: why each cached cascade was redrawn (or kept), % of frames
	if (TheSettingManager->GetSettingI("Main.Main.ReducedQuality", "SunCacheByChange")) {
		char text[400]; int used = 0;
		static const char* const names[4] = { "near", "middle", "far", "LOD" };
		for (int i = MapMiddle; i <= MapLod; i++) {
			const unsigned* r = SunCacheReasons[i];
			const float total = (float)max(r[0] + r[1] + r[2] + r[3] + r[4], 1u);
			used += sprintf_s(text + used, sizeof(text) - used, " | %s: kept %.0f, age %.0f, sun %.0f, moved %.0f, characters %.0f", names[i],
				100.0f * r[4] / total, 100.0f * r[0] / total, 100.0f * r[1] / total, 100.0f * r[2] / total, 100.0f * r[3] / total);
		}
		Logger::Log("SUN CACHE BY CHANGE (%% of frames)%s", text);
		memset(SunCacheReasons, 0, sizeof(SunCacheReasons));
	}
}

// UNOFFICIAL 2026-10-31 SunLayers ([Main.Main.ReducedQuality] SunLayers, live, off; root plan A1 done properly, docs/next/ROOT-ASSUMPTIONS-PLAN.md).
// The middle and far sun cascades in two layers. The static layer holds what never moves (land, statics, object LOD, every still part
// of a reference) in its own R32F map, SunLayersMargin texels wider on every side than the cascade, with the same texel size and the
// same world-anchored texel grid (GetCascadeViewProj with ExtentScale). It is redrawn only when it no longer fits: the quantised sun
// stepped, the cascade's radius (texel size) changed, the cascade's square slid past the margin, the loaded cells changed (new statics;
// the moving references' list holds no pointers past that), or SunLayersMaxAge frames passed (the net for anything misjudged as still).
// Every frame the cascade is built from it: a copy at the whole-texel offset between the two grids (the cascades snap to world texels,
// so the offset is a whole number and nothing swims), its depth shifted by the two maps' distance along the sun, written as colour and
// depth; then the overlay draws on top with the depth test (= the min of the two): actors, doors, items, moveable statics, animated
// models (a controller on the root, flags), trees (SunLayersTreesMoving: their leaves sway) and every reference a static layer held a
// caster back from (cloth or plant wind by texture, skinned); grass adds itself afterwards as on any drawn cascade.
namespace SunLayers {
	enum Reason { kFirst, kSun, kRadius, kCells, kAge, kOffset, kReasons };
	struct Layer {
		IDirect3DTexture9* texture = nullptr;
		IDirect3DSurface9* surface = nullptr;
		UINT size = 0;
		bool valid = false;
		D3DXMATRIX viewProj;	// camera-relative to 'camera' (the camera when it was drawn)
		D3DXVECTOR3 camera = {}, sun = {};
		float radius = 0.0f, landDrop = 0.0f;
		UInt32 cells = 0;
		unsigned age = 0;
		int debug = 0;
		bool settle = false;	// drawn fresh last frame: once more this frame
		std::unordered_set<const void*> movingRefs;	// references it held back a caster from
		// stats, per LogFrames frames
		unsigned frames = 0, redraws[kReasons] = {}, heldBack = 0, offGrid = 0;
		float worstFraction = 0.0f;
	};
	static Layer Layers[4];
	static IDirect3DSurface9* Depth = nullptr;
	static UINT DepthSize = 0;
	static IDirect3DPixelShader9* Composite = nullptr;
	static bool CompileFailed = false;

	// c0: z = the depth shift (layer depth - cascade depth). Cleared texels (1) stay cleared.
	static const char* const kCompositePS =
		"sampler2D Layer : register(s0);\n"
		"float4 Map : register(c0);\n"
		"struct PS_OUT { float4 color : COLOR0; float depth : DEPTH; };\n"
		"PS_OUT main(float2 uv : TEXCOORD0) {\n"
		"	float z = tex2Dlod(Layer, float4(uv, 0, 0)).r;\n"
		"	z = z >= 1.0f ? 1.0f : saturate(z - Map.z);\n"
		"	PS_OUT o; o.color = float4(z, 0, 0, 1); o.depth = z; return o;\n"
		"}\n";

	static void Release(Layer& layer) {
		if (layer.surface) { layer.surface->Release(); layer.surface = nullptr; }
		if (layer.texture) { layer.texture->Release(); layer.texture = nullptr; }
		layer.size = 0;
		layer.valid = false;
	}

	static bool Ensure(IDirect3DDevice9* Device, Layer& layer, UINT size) {
		if (layer.size != size) Release(layer);
		if (!layer.texture) {
			if (FAILED(Device->CreateTexture(size, size, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &layer.texture, NULL)) ||
				FAILED(layer.texture->GetSurfaceLevel(0, &layer.surface))) {
				Logger::Log("SunLayers: could not make a %ux%u R32F layer: off", size, size);
				Release(layer);
				return false;
			}
			layer.size = size;
			Logger::Log("SunLayers: %ux%u R32F static layer made (%.0f MB)", size, size, size * (double)size * 4.0 / 1048576.0);
		}
		if (DepthSize < size) {
			if (Depth) { Depth->Release(); Depth = nullptr; }
			DepthSize = 0;
			if (FAILED(Device->CreateDepthStencilSurface(size, size, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &Depth, NULL))) return false;
			DepthSize = size;
		}
		if (!Composite && !CompileFailed) {
			ID3DXBuffer* code = nullptr;
			ID3DXBuffer* errors = nullptr;
			if (FAILED(D3DXCompileShader(kCompositePS, (UINT)strlen(kCompositePS), NULL, NULL, "main", "ps_3_0", 0, &code, &errors, NULL)) || !code ||
				FAILED(Device->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &Composite))) {
				Logger::Log("SunLayers: composite shader failed: %s", errors ? (const char*)errors->GetBufferPointer() : "(no message)");
				CompileFailed = true;
				Composite = nullptr;
			}
			if (code) code->Release();
			if (errors) errors->Release();
		}
		return Composite && Depth && layer.surface;
	}

	static UInt32 LoadedCells() {
		UInt32 hash = 2166136261u;
		if (Player->GetWorldSpace()) {
			GridCellArray* cells = Tes->gridCellArray;
			const UInt32 count = cells->size * cells->size;
			for (UInt32 i = 0; i < count; i++) hash = (hash ^ (UInt32)(uintptr_t)cells->GetCell(i)) * 16777619u;
		}
		else hash ^= (UInt32)(uintptr_t)Player->parentCell;
		return hash;
	}

	static const unsigned LogFrames = 240;
	static void Log(int i) {
		Layer& l = Layers[i];
		if (++l.frames < LogFrames) return;
		if (GpuTimer::Enabled)
			Logger::Log("SUN LAYERS cascade %d (%u frames): static redraws %u (first %u, sun %u, radius %u, cells %u, age %u, slid past margin %u) |"
				" moving references held back %u (casters %u) | off-grid offsets %u (worst %.3f texel)", i, l.frames,
				l.redraws[kFirst] + l.redraws[kSun] + l.redraws[kRadius] + l.redraws[kCells] + l.redraws[kAge] + l.redraws[kOffset],
				l.redraws[kFirst], l.redraws[kSun], l.redraws[kRadius], l.redraws[kCells], l.redraws[kAge], l.redraws[kOffset],
				(unsigned)l.movingRefs.size(), l.heldBack, l.offGrid, l.worstFraction);
		l.frames = 0; l.heldBack = 0; l.offGrid = 0; l.worstFraction = 0.0f;
		for (unsigned& r : l.redraws) r = 0;
	}
}

// SunLayers: builds cascade i (middle or far) this frame from its static layer and the overlay. False: could not (no resources): the
// caller draws it the usual way.
static bool DrawSunLayer(IDirect3DDevice9* Device, ShadowsExteriorEffect* Shadows, int i, D3DXVECTOR3 SunDir, bool force, int margin, unsigned maxAge) {
	using namespace SunLayers;
	ShadowsExteriorEffect::ShadowMapSettings* ShadowMap = &Shadows->ShadowMaps[i];
	Layer& layer = Layers[i];
	const UINT res = (UINT)ShadowMap->ShadowMapResolution;
	const UINT size = res + 2 * (UINT)margin;
	if (!Ensure(Device, layer, size)) return false;
	NiDX9RenderState* RenderState = TheRenderManager->renderState;

	D3DXMATRIX cascade = Shadows->GetCascadeViewProj(ShadowMap, &SunDir);
	const float radius = ShadowMap->ShadowMapCascadeCenterRadius.w;
	const D3DXVECTOR3 camera = WorldSceneGraph->camera->m_worldTransform.pos.toD3DXVEC3();
	const UInt32 cells = LoadedCells();
	SunLandDropScale = min(1.0f, 2.0f * max(SunDir.z, 0.0f));
	const float landDrop = ShadowMapLandDrop();

	// the cascade's texel (0, 0) in the layer's texels, and the depth shift: the layer's matrix is relative to the camera it was drawn
	// from, so a point at the camera now is at (camera - layer.camera) for it
	float ox = 0.0f, oy = 0.0f, dz = 0.0f;
	auto offsets = [&]() {
		const D3DXVECTOR3 moved = camera - layer.camera, zero(0.0f, 0.0f, 0.0f);
		D3DXVECTOR4 l, c;
		D3DXVec3Transform(&l, &moved, &layer.viewProj);
		D3DXVec3Transform(&c, &zero, &cascade);
		ox = (l.x * 0.5f + 0.5f) * size - (c.x * 0.5f + 0.5f) * res;
		oy = (0.5f - l.y * 0.5f) * size - (0.5f - c.y * 0.5f) * res;
		dz = l.z - c.z;
	};
	// SunLayersDebug (test): 1 = everything in the static layer, no overlay (checks the copy and its offsets); 2 = an empty static layer
	// and the whole cascade drawn as overlay (checks the overlay path); 3 = the static layer redrawn every frame
	const int debug = TheSettingManager->GetSettingI("Main.Main.ReducedQuality", "SunLayersDebug");
	int reason = -1;
	// (a layer drawn fresh, on the frame it is made or the switch turns on, is drawn once more on the next frame: the first round of
	// the bench showed that frame's picture missing whole casters for as long as it was kept, while one redrawn each frame was right)
	const bool settling = layer.settle;
	if (!layer.valid || force || debug == 3 || debug != layer.debug || settling) reason = kFirst;
	else if (layer.sun != SunDir || fabsf(layer.landDrop - landDrop) > 0.01f) reason = kSun;
	else if (fabsf(layer.radius - radius) > radius * 1e-4f) reason = kRadius;
	else if (layer.cells != cells) reason = kCells;
	else if (layer.age + 1 >= maxAge) reason = kAge;
	else {
		offsets();
		const float slack = 2.0f * margin + 0.01f;
		if (!(ox > -0.01f && oy > -0.01f && ox < slack && oy < slack)) reason = kOffset;
	}

	RenderPass::TwoSided = TheSettingManager->GetSettingI("Shaders.ShadowsExteriors.Main", "TwoSidedCasters") != 0;
	if (reason >= 0) {
		layer.redraws[reason]++;
		GpuDebug::LabelScope label(i == 1 ? "Sun layer 1 (middle) static" : "Sun layer 2 (far) static");
		D3DXMATRIX still = Shadows->GetCascadeViewProj(ShadowMap, &SunDir, (float)size / (float)res, (float)size);
		Device->SetRenderTarget(0, layer.surface);
		Device->SetDepthStencilSurface(Depth);
		D3DVIEWPORT9 viewport = { 0, 0, size, size, 0.0f, 1.0f };
		Device->SetViewport(&viewport);
		Device->Clear(0L, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DXCOLOR(1.0f, 1.0f, 1.0f, 1.0f), 1.0f, 0L);
		Shadows->Constants.ShadowViewProj = still;
		TheShadowManager->BeginShadowMap(ShadowMap, &still);
		layer.movingRefs.clear();
		RenderPass::LayerGeoRef.clear();
		RenderPass::LayerMovingRefs = &layer.movingRefs;
		RenderPass::LayerTreePass = LayerTreesMoving ? TheShadowManager->speedTreePass : nullptr;
		RenderPass::LayerHeldBack = 0;
		RenderPass::LayerMode = debug == 1 ? 0 : 1;
		if (debug != 2) {
			TheShadowManager->WalkShadowMap(ShadowMap);
			TheShadowManager->RenderAccums();
		}
		RenderPass::LayerMode = 0;
		layer.debug = debug;
		layer.settle = !settling;
		RenderPass::LayerMovingRefs = nullptr;
		RenderPass::LayerGeoRef.clear();
		layer.heldBack += RenderPass::LayerHeldBack;
		RenderPass::LandSet.clear();
		layer.viewProj = still;
		layer.camera = camera;
		layer.sun = SunDir;
		layer.radius = radius;
		layer.landDrop = landDrop;
		layer.cells = cells;
		layer.age = 0;
		layer.valid = true;
		Device->SetRenderTarget(0, Shadows->ShadowAtlasSurface);
		Device->SetDepthStencilSurface(Shadows->ShadowAtlasDepthSurface);
		cascade = Shadows->GetCascadeViewProj(ShadowMap, &SunDir);	// the cascade's own cull planes again
		offsets();
	}
	else layer.age++;

	const float fraction = max(fabsf(ox - roundf(ox)), fabsf(oy - roundf(oy)));
	if (fraction > 0.02f) layer.offGrid++;
	layer.worstFraction = max(layer.worstFraction, fraction);
	ox = roundf(ox);
	oy = roundf(oy);

	// the copy: colour and depth from the layer, every texel of the cascade's square
	{
		GpuDebug::LabelScope label(i == 1 ? "Sun layer 1 (middle) copy" : "Sun layer 2 (far) copy");
		const D3DVIEWPORT9& port = ShadowMap->ShadowMapViewPort;
		Device->SetViewport(&port);
		DWORD zFunc = D3DCMP_LESSEQUAL;
		Device->GetRenderState(D3DRS_ZFUNC, &zFunc);
		// The device's vertex input and pixel constant c0 are put back as they were after the copy: the game's state cache keeps the FVF
		// and the declaration apart, so a caster whose declaration it thinks is still set would draw with this quad's FVF, and the
		// constant filter would not re-send c0 (TESR_ShadowData) it thinks is still there.
		IDirect3DVertexDeclaration9* savedDecl = nullptr;
		Device->GetVertexDeclaration(&savedDecl);
		DWORD savedFvf = 0;
		Device->GetFVF(&savedFvf);
		float savedC0[4] = {};
		Device->GetPixelShaderConstantF(0, savedC0, 1);
		static const D3DSAMPLERSTATETYPE kSS[] = { D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV };
		static const DWORD kWant[] = { D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP };
		DWORD saved[5];
		for (int k = 0; k < 5; k++) { if (FAILED(Device->GetSamplerState(0, kSS[k], &saved[k]))) saved[k] = kWant[k]; RenderState->SetSamplerState(0, kSS[k], kWant[k], false); }
		RenderState->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE, RenderStateArgs);
		RenderState->SetRenderState(D3DRS_ZWRITEENABLE, TRUE, RenderStateArgs);
		Device->SetRenderState(D3DRS_ZFUNC, D3DCMP_ALWAYS);	// (on the device, round the game's state cache: put back the same way below)
		RenderState->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE, RenderStateArgs);
		RenderState->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE, RenderStateArgs);
		RenderState->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE, RenderStateArgs);
		RenderState->SetVertexShader(NULL, false);
		RenderState->SetPixelShader(Composite, false);
		RenderState->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1, false);
		RenderState->SetTexture(0, layer.texture);
		const float constants[4] = { 0.0f, 0.0f, dz, 0.0f };
		Device->SetPixelShaderConstantF(0, constants, 1);
		const float u0 = ox / size, v0 = oy / size, u1 = (ox + port.Width) / size, v1 = (oy + port.Height) / size;
		const float x0 = (float)port.X - 0.5f, y0 = (float)port.Y - 0.5f, x1 = x0 + port.Width, y1 = y0 + port.Height;
		struct V { float x, y, z, rhw, u, v; };
		const V quad[4] = { { x0, y0, 0.5f, 1.0f, u0, v0 }, { x1, y0, 0.5f, 1.0f, u1, v0 }, { x0, y1, 0.5f, 1.0f, u0, v1 }, { x1, y1, 0.5f, 1.0f, u1, v1 } };
		Device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
		RenderState->SetTexture(0, nullptr);
		for (int k = 0; k < 5; k++) RenderState->SetSamplerState(0, kSS[k], saved[k], false);
		Device->SetRenderState(D3DRS_ZFUNC, zFunc);
		if (savedDecl) { Device->SetVertexDeclaration(savedDecl); savedDecl->Release(); }
		else Device->SetFVF(savedFvf);
		Device->SetPixelShaderConstantF(0, savedC0, 1);
	}

	// the overlay: the moving references, with the depth test, in the cascade's own matrix (BeginShadowMap sets what the scene reads)
	{
		GpuDebug::LabelScope label(i == 1 ? "Sun layer 1 (middle) overlay" : "Sun layer 2 (far) overlay");
		Shadows->Constants.ShadowViewProj = cascade;
		TheShadowManager->BeginShadowMap(ShadowMap, &cascade);
		LayerOverlayRefs = &layer.movingRefs;
		RenderPass::LayerMode = 2;
		if (debug == 1) {}
		else if (debug == 2) { RenderPass::LayerMode = 0; TheShadowManager->WalkShadowMap(ShadowMap); }
		else if (Player->GetWorldSpace()) {
			GridCellArray* CellArray = Tes->gridCellArray;
			const UInt32 count = CellArray->size * CellArray->size;
			for (UInt32 c = 0; c < count; c++) TheShadowManager->AccumExteriorCell(CellArray->GetCell(c), ShadowMap);
		}
		else TheShadowManager->AccumExteriorCell(Player->parentCell, ShadowMap);
		RenderPass::LayerMode = 0;
		LayerOverlayRefs = nullptr;
		TheShadowManager->RenderAccums();
	}
	RenderPass::TwoSided = false;
	RenderPass::LandDrop = 0.0f;
	RenderPass::LandSet.clear();
	SunLandDropScale = 0.0f;
#if NVR_TEST_TOOLS
	// SunLayersDebug 4 (test): every 60 frames the layered cascade is read back, the cascade then drawn the usual way over it and read back
	// too, and the two compared texel by texel (left in the atlas: the usual one, for that frame)
	if (debug == 4 && layer.frames % 60 == 30) {
		std::vector<UInt8> kept, fresh;
		const D3DVIEWPORT9& port = ShadowMap->ShadowMapViewPort;
		const bool keptRead = ReadCascadeRegion(Shadows, port, kept);
		SunLandDropScale = min(1.0f, 2.0f * max(SunDir.z, 0.0f));
		TheShadowManager->BeginShadowMap(ShadowMap, &cascade);
		TheShadowManager->WalkShadowMap(ShadowMap);
		TheShadowManager->DrawShadowMap(ShadowMap);
		SunLandDropScale = 0.0f;
		const bool freshRead = ReadCascadeRegion(Shadows, port, fresh);
		const size_t texels = (size_t)port.Width * port.Height;
		if (keptRead && freshRead && kept.size() == fresh.size() && kept.size() == texels * 4) {
			const float* k = (const float*)kept.data();
			const float* f = (const float*)fresh.data();
			unsigned farther = 0, nearer = 0, keptEmpty = 0, freshEmpty = 0;
			int x0 = INT_MAX, y0 = INT_MAX, x1 = -1, y1 = -1;
			float worst = 0.0f;
			for (size_t t = 0; t < texels; t++) {
				const float d = k[t] - f[t];
				if (d > 1e-4f) {
					farther++;
					const int x = (int)(t % port.Width), y = (int)(t / port.Width);
					x0 = min(x0, x); y0 = min(y0, y); x1 = max(x1, x); y1 = max(y1, y);
					if (k[t] >= 1.0f && f[t] < 1.0f) keptEmpty++;
				}
				else if (d < -1e-4f) { nearer++; if (f[t] >= 1.0f && k[t] < 1.0f) freshEmpty++; }
				worst = max(worst, fabsf(d));
			}
			Logger::Log("SUN LAYERS CHECK cascade %d: of %u texels the layered map is farther (a caster missing) at %u (%u where it is empty), nearer at %u (%u where the usual is empty);"
				" worst %.5f; missing box %d,%d - %d,%d; offset %.0f %.0f, depth shift %.6f, layer age %u", i, (unsigned)texels, farther, keptEmpty, nearer, freshEmpty, worst,
				x0, y0, x1, y1, ox, oy, dz, layer.age);
		}
		else Logger::Log("SUN LAYERS CHECK cascade %d: not read (%d %d, %u / %u bytes)", i, keptRead, freshRead, (unsigned)kept.size(), (unsigned)fresh.size());
		// the first check of a session also writes the three maps (raw R32F, width and height first) next to the log
		static int dumped = 0;
		if (dumped < 2 && keptRead && freshRead) {
			dumped++;
			std::vector<UInt8> still;
			IDirect3DSurface9* sys = nullptr;
			if (SUCCEEDED(Device->CreateOffscreenPlainSurface(layer.size, layer.size, D3DFMT_R32F, D3DPOOL_SYSTEMMEM, &sys, NULL)) &&
				SUCCEEDED(Device->GetRenderTargetData(layer.surface, sys))) {
				D3DLOCKED_RECT locked = {};
				if (SUCCEEDED(sys->LockRect(&locked, NULL, D3DLOCK_READONLY))) {
					still.resize((size_t)layer.size * layer.size * 4);
					for (UINT y = 0; y < layer.size; y++) memcpy(&still[(size_t)y * layer.size * 4], (const UInt8*)locked.pBits + (size_t)y * locked.Pitch, layer.size * 4);
					sys->UnlockRect();
				}
			}
			if (sys) sys->Release();
			auto write = [&](const char* what, const std::vector<UInt8>& data, UINT w, UINT h) {
				char name[160];
				sprintf_s(name, VS_LOGS_FOLDER "/sunlayer-c%d-%s.bin", i, what);
				FILE* file = nullptr;
				if (fopen_s(&file, name, "wb") == 0 && file) { fwrite(&w, 4, 1, file); fwrite(&h, 4, 1, file); fwrite(data.data(), 1, data.size(), file); fclose(file); }
			};
			write("kept", kept, port.Width, port.Height);
			write("fresh", fresh, port.Width, port.Height);
			if (!still.empty()) write("layer", still, layer.size, layer.size);
			Logger::Log("SUN LAYERS CHECK cascade %d: maps written (sunlayer-c%d-*.bin)", i, i);
		}
	}
#endif
	Log(i);
	return true;
}

void ShadowManager::RenderShadowMaps() {
	// UNOFFICIAL 2026-11-04: the Pip-Boy fit's object is the last first-person draw's, last frame's by now and maybe gone (a load, a weapon switch):
	// ClothWind's partition draws in the shadow passes read it (crashed in PipboyFit::ForPartition, right after a load). Off for them.
	PipboyFit::Active = nullptr;
	PointShadowForward::Begin();	// UNOFFICIAL: idle unless the cube maps below are published
	if (!TheSettingManager->SettingsMain.Main.RenderEffects) return; // cancel out if rendering effects is disabled
	// UNOFFICIAL SkinnedShadowFaceTest: skeleton bounds are rebuilt once per frame; forget meshes not seen for a while.
	if ((++SkinnedBoundFrame & 1023) == 0) {
		SkinnedBounds.clear();
		// UNOFFICIAL RedrawActorsOnly: forget geometry not seen for a while (keeping the rest's motion history).
		const size_t motionsBefore = CasterMotions.size();
		for (auto it = CasterMotions.begin(); it != CasterMotions.end();)
			it = SkinnedBoundFrame - it->second.seen > 1024 ? CasterMotions.erase(it) : std::next(it);
		if (CasterMotions.size() != motionsBefore) CasterMotionsGeneration++;	// CasterListCache: the lamps' kept entries may be gone
		for (auto it = LampCasterLists.begin(); it != LampCasterLists.end();)	// and lamps not seen for a while
			it = SkinnedBoundFrame - it->second.seen > 1024 ? LampCasterLists.erase(it) : std::next(it);
	}

	// track point lights for interiors and exteriors
	ShadowSceneLight* castLamps[ShadowCubeMapsMax] = { NULL };
	NiPointLight* nearLamps[TrackedLightsMax] = { NULL };
	NiSpotLight* nearSpots[SpotLightsMax] = { NULL };

	TheShaderManager->GetNearbyLights(castLamps, nearLamps, nearSpots);
	// UNOFFICIAL 2026-10-29 RT lamp shadows Exteriors (off): outdoors, the lamp mask's lamps go to the trace
	if (TheShaderManager->GameState.isExterior) {
		RtLampShadows::SubmitExterior(castLamps, ShadowCubeMapsMax, nearLamps, TrackedLightsMax);
		PointShadowForward::PublishExterior(castLamps, ShadowCubeMapsMax);	// UNOFFICIAL 2026-10-30 ExteriorForward
	}

	ShadowsExteriorEffect* fx = TheShaderManager->Effects.ShadowsExteriors;
	ShadowsExteriorEffect::ExteriorsStruct* extCfg = &fx->Settings.Exteriors;
	ShadowsExteriorEffect::InteriorsStruct* intCfg = &fx->Settings.Interiors;

	bool exteriorCell = TheShaderManager->GameState.isExterior;// || currentCell->flags0 & TESObjectCELL::kFlags0_BehaveLikeExterior; // exterior flag currently broken
	bool sunShadowsOn = exteriorCell && TheShaderManager->Effects.ShadowsExteriors->Enabled && extCfg->Enabled;
	bool lampShadowsOn = !exteriorCell && TheShaderManager->Effects.ShadowsInteriors->Enabled;

	// early out in case shadow rendering is not required
	if (!sunShadowsOn && !lampShadowsOn && !TheShaderManager->orthoRequired && !TheShaderManager->orthoSkyRequired || !ShadowShadersLoaded) {
		return;
	}
	if (!Player->parentCell) return;

	auto frameTimer = TimeLogger();
	static CpuTimer shadowMapsCpuTimer("Shadow maps (CPU)");
	CpuProfileScope shadowMapsCpu(shadowMapsCpuTimer);
	fx->Constants.CharacterData.w = 0.0f;	// UNOFFICIAL 2026-10-23 CharacterShadowMap: read only once this frame's draw is in it (RenderCharacterShadowMap)

	// prepare some pointers to the device and surfaces
	IDirect3DDevice9* dev = TheRenderManager->device;
	NiDX9RenderState* rs = TheRenderManager->renderState;
	static GpuTimer sunCascadesTimer("Sun cascade geometry");
	static GpuTimer atlasResolveTimer("Shadow atlas resolve");
	static GpuTimer atlasFilterTimer("Shadow atlas prefilter");
	static GpuTimer orthoMapTimer("Ortho shadow map");
	static GpuTimer pointMapsTimer("Point shadow cubemaps");
	static GpuTimer flashlightMapsTimer("Flashlight shadow maps");
	IDirect3DSurface9* savedDepth = NULL;
	IDirect3DSurface9* savedTarget = NULL;
	D3DVIEWPORT9 savedViewport;

	D3DXVECTOR4* dataConst = &TheShaderManager->Effects.ShadowsExteriors->Constants.Data;
	// UNOFFICIAL 2026-11-05 (from Bale, PR #6; user: exterior forward sun shadows show in third person, vanish in first): the world's TESR_ShadowData
	// (x quality, y darkness) is borrowed by the caster passes below (x caster kind, y alpha test) and was never put back, so the world read the
	// last pass's values: darkness 0 whenever an opaque pass drew last (in third person the character map happened to leave 1). Put back at the end.
	const float worldQuality = dataConst->x, worldDarkness = dataConst->y;
	D3DXVECTOR4* orthoConst = &TheShaderManager->Effects.ShadowsExteriors->Constants.OrthoData;
	dev->GetDepthStencilSurface(&savedDepth);
	dev->GetRenderTarget(0, &savedTarget);
	dev->GetViewport(&savedViewport);	

	DWORD savedZFunc;
	dev->GetRenderState(D3DRS_ZFUNC, &savedZFunc); // backup in case of inverted depth

	DWORD savedStencilEnable, savedStencilRef, savedStencilFunc;
	DWORD savedAlphaRef, savedNormalize, savedPointSize;

	dev->GetRenderState(D3DRS_STENCILENABLE, &savedStencilEnable);
	dev->GetRenderState(D3DRS_STENCILREF, &savedStencilRef);
	dev->GetRenderState(D3DRS_STENCILFUNC, &savedStencilFunc);
	dev->GetRenderState(D3DRS_ALPHAREF, &savedAlphaRef);
	dev->GetRenderState(D3DRS_NORMALIZENORMALS, &savedNormalize);
	dev->GetRenderState(D3DRS_POINTSIZE, &savedPointSize);

	if (1.0 - NiDX9Renderer::GetSingleton()->m_fZClear) // inverted depth
		rs->SetRenderState(D3DRS_ZFUNC, D3DCMP_GREATEREQUAL, RenderStateArgs);
	else
		rs->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL, RenderStateArgs);

	rs->SetRenderState(D3DRS_STENCILENABLE, 1, RenderStateArgs);
	rs->SetRenderState(D3DRS_STENCILREF, 0, RenderStateArgs);
	rs->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_ALWAYS, RenderStateArgs);
	rs->SetRenderState(D3DRS_ALPHAREF, 0, RenderStateArgs);
	rs->SetRenderState(D3DRS_NORMALIZENORMALS, 1, RenderStateArgs);
	rs->SetRenderState(D3DRS_POINTSIZE, 810365505, RenderStateArgs);

	TheRenderManager->UpdateSceneCameraData();
	TheRenderManager->SetupSceneCamera();
	
	D3DXVECTOR4 playerPos = Player->pos.toD3DXVEC4();
	TESObjectCELL* playerCell = Player->parentCell;

	// Flag player geometry so we can control if it should be rendered in shadow cubemaps
	FlagPlayerGeometry();

	// Render directional shadows for Sun/Moon
	NiNode* playerRoot = Player->GetNode();
	D3DXVECTOR3 playerRel;
	playerRel.x = playerRoot->m_worldTransform.pos.x - TheRenderManager->CameraPosition.x;
	playerRel.y = playerRoot->m_worldTransform.pos.y - TheRenderManager->CameraPosition.y;
	playerRel.z = playerRoot->m_worldTransform.pos.z - TheRenderManager->CameraPosition.z;

	// Render all shadow maps
	dev->BeginScene();

	// Quantize sun direction angle to reduce shimmer by a large factor.
	D3DXVECTOR3 sunDir = fx->CalculateSmoothedSunDir();

	if (exteriorCell && (sunShadowsOn || TheShaderManager->orthoRequired || TheShaderManager->orthoSkyRequired)) {

		// Update cascade depths based on current camera.
		fx->GetCascadeDepths();

		// Opaque passes (TESR_ShadowData.y always 0) use the no-discard copy, so the GPU keeps early-Z.
		geometryPass->VertexShader = ShadowMapVertex;
		geometryPass->PixelShader = ShadowMapOpaquePixel;
		alphaPass->VertexShader = ShadowMapVertex;
		alphaPass->PixelShader = ShadowMapPixel;
		skinnedGeoPass->VertexShader = ShadowMapVertex;
		// UNOFFICIAL 2026-11-02 SkinnedAlphaTest (RenderPass.h): on, skinned casters take the clip shader (opaque ones never discard).
		SkinnedGeoShadowRenderPass::AlphaTest = TheSettingManager->GetSettingI("Shaders.ShadowsExteriors.Main", "SkinnedAlphaTest") != 0;
		skinnedGeoPass->PixelShader = SkinnedGeoShadowRenderPass::AlphaTest ? ShadowMapPixel : ShadowMapOpaquePixel;
		speedTreePass->VertexShader = ShadowMapVertex;
		speedTreePass->PixelShader = ShadowMapPixel;
		terrainLODPass->VertexShader = ShadowMapVertex;
		terrainLODPass->PixelShader = ShadowMapOpaquePixel;

		// UNOFFICIAL contact hardening. The sun's disc is about 0.53 degrees wide, so a shadow edge blurs by about
		// 0.0093 world units per unit between the thing casting it and the ground; SunSize scales that.
		if (ContactHardeningCompiled) {
			// (The settings are read by ShadowsExteriorEffect::UpdateSettings when they change, not by name every frame.)
			// UNOFFICIAL 2026-10-16 (night cost study, work/bench/foundations/nightcost): at full night (dayLight 0) the
			// shadows are the moon's, faded and dim, and their soft edges did not show: off there, the night test spot
			// changed fewer pixels than two identical frames differ by, and the world scene took 0.8-1.0 ms less (GTX 1070,
			// 1440p). AtNight keeps it.
			const bool night = TheShaderManager->GameState.dayLight <= 0.0f && !fx->Settings.ContactHardening.AtNight;
			const bool on = fx->Settings.ContactHardening.Enabled && !night;
			ContactHardeningData.x = on ? 0.00925f * fx->Settings.ContactHardening.SunSize : 0.0f;
			ContactHardeningData.y = fx->Settings.ContactHardening.MaxSoftness;
			ContactHardeningData.z = fx->Settings.ContactHardening.Smooth ? 1.0f : 0.0f;	// UNOFFICIAL 2026-10-15 Smooth
			static float logged = -1.0f;
			if (ContactHardeningData.x != logged) {
				Logger::Log("UNOFFICIAL contact hardening %s%s (penumbra %.4f per unit of distance, max %.0f texels)",
					on ? "ON" : "OFF", night && fx->Settings.ContactHardening.Enabled ? " for the night (AtNight off)" : "",
					ContactHardeningData.x, ContactHardeningData.y);
				logged = ContactHardeningData.x;
			}
		}
		else {
			static bool hinted = false;
			if (!hinted && fx->Settings.ContactHardening.Enabled) {
				Logger::Log("UNOFFICIAL contact hardening was turned on after startup: restart the game once to apply it.");
				hinted = true;
			}
		}
		// UNOFFICIAL 2026-10-16 SkipWhenFaded (dawn cost study, work/bench/foundations/dawncost): while the sun's shadows are
		// fully faded and nothing reads the cascade maps (ShadowsExteriorEffect::FadedCascadesUnread), they are not drawn.
		// Only once they hold a full draw since the atlas was made (SunAtlasDrawn), and the first frame the cascades are drawn
		// after a skip redraws every one, so no reader ever sees a map that missed frames. While the sun is below the horizon
		// (the whole fade-1 window in the Mojave's climate) nothing redraws them anyway.
		const bool skipCascades = sunShadowsOn && fx->SunFaded && SunAtlasDrawn && fx->FadedCascadesUnread();
		if (skipCascades) {
			SunCascadesSkipped = true;
			if (sunDir.z > 0.0f) dataConst->z = 0;	// as the cascade pass leaves it
		}
		else if (SunCascadesSkipped) {
			ForceAllCascades = true;
			SunCascadesSkipped = false;
		}
#if NVR_TEST_TOOLS
		// DIAGNOSTIC SkipWhenFadedCheck: while the cascades are skipped the atlas is set to 0 (depth 0 / zero moments: everything
		// in shadow), so a reader the checks missed changes the picture. The real atlas is kept in a copy and put back when the
		// skip ends (the sun is below the horizon in the fade-1 window: no pass would redraw it). The render target and depth are
		// put back at the end of this function.
		{
			static IDirect3DTexture9* kept = nullptr;
			static IDirect3DSurface9* keptSurface = nullptr;
			static bool poisoned = false;
			const bool poison = skipCascades && fx->Settings.Exteriors.SkipWhenFadedCheck && fx->ShadowAtlasSurface;
			if (poison && !poisoned) {
				D3DSURFACE_DESC desc = {}, keptDesc = {};
				fx->ShadowAtlasSurface->GetDesc(&desc);
				if (keptSurface && (FAILED(keptSurface->GetDesc(&keptDesc)) || keptDesc.Width != desc.Width || keptDesc.Height != desc.Height ||
					keptDesc.Format != desc.Format)) {
					keptSurface->Release(); kept->Release(); keptSurface = nullptr; kept = nullptr;
				}
				if (!kept && SUCCEEDED(dev->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, desc.Format, D3DPOOL_DEFAULT, &kept, NULL)))
					kept->GetSurfaceLevel(0, &keptSurface);
				poisoned = keptSurface && SUCCEEDED(dev->StretchRect(fx->ShadowAtlasSurface, NULL, keptSurface, NULL, D3DTEXF_NONE));
				Logger::Log("SkipWhenFadedCheck: shadow atlas %s", poisoned ? "kept and poisoned while skipped" : "could not be kept: not poisoned");
			}
			if (poison && poisoned) {
				dev->SetRenderTarget(0, fx->ShadowAtlasSurface);
				dev->SetDepthStencilSurface(fx->ShadowAtlasDepthSurface);
				dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);
			}
			else if (!poison && poisoned) {
				const bool back = SUCCEEDED(dev->StretchRect(keptSurface, NULL, fx->ShadowAtlasSurface, NULL, D3DTEXF_NONE));
				Logger::Log("SkipWhenFadedCheck: shadow atlas %s", back ? "put back" : "could NOT be put back");
				poisoned = false;
			}
		}
#endif
		static int loggedSkip = -1;
		const int skipState = skipCascades ? 2 : fx->SunFaded && sunShadowsOn ? 1 : 0;
		if (skipState != loggedSkip) {
			Logger::Log("SUN FADE STATE: sun cascade maps %s", skipState == 2 ? "SKIPPED" : skipState == 1 ?
				"drawn (read this frame: the volumetric fog's sun glow, the volumetric light or the deferred lookup; or no full draw yet)" : "drawn");
			loggedSkip = skipState;
		}
		SunCascadesDrawnMask = 0;	// set again below on the frames the sun cascades are drawn
		if (sunShadowsOn && sunDir.z > 0.0f && !skipCascades) {
			// Recalculate billboard vectors for speedtree leaves shader.
			RecalculateBillboardVectors(&sunDir);

			dataConst->z = 0; // set shader constant to identify other shadow maps
			auto shadowMapTimer = TimeLogger();

			// UNOFFICIAL 2026-10-21 (sunfix): one log line whenever the sun reaches or leaves straight overhead (GetCascadeViewProj).
			{
				static int loggedOverhead = -1;
				const int overheadState = !ShadowsExteriorEffect::SunOverhead(sunDir) ? 0 : fx->Settings.Exteriors.OverheadSunFix ? 2 : 1;
				if (overheadState != loggedOverhead) {
					Logger::Log("SUN OVERHEAD STATE: sun %.4f %.4f %.4f %s", sunDir.x, sunDir.y, sunDir.z, overheadState == 2 ?
						"straight overhead: cascades built with +Y as up (OverheadSunFix)" : overheadState == 1 ?
						"straight overhead: cascades flat and empty, no sun shadows from them (OverheadSunFix off)" : "not overhead: cascades as always");
					loggedOverhead = overheadState;
				}
			}

			if (fx->ShadowAtlasSurfaceMSAA)
				dev->SetRenderTarget(0, fx->ShadowAtlasSurfaceMSAA);
			else
				dev->SetRenderTarget(0, fx->ShadowAtlasSurface);

			dev->SetDepthStencilSurface(fx->ShadowAtlasDepthSurface);

			unsigned updatedCascades = 0;
			unsigned layeredCascades = 0;	// UNOFFICIAL 2026-10-31 SunLayers: built by DrawSunLayer, not by the walk below
			unsigned freshCascades = 0;	// UNOFFICIAL 2026-11-04 CasterCulling: redrawn every frame and not kept (period 1, not cached): culled to the view
			{
			static CpuTimer sunCascadesCpuTimer("Sun cascades (CPU)");
			CpuProfileScope cpu(sunCascadesCpuTimer);
			GpuProfileScope gpu(sunCascadesTimer, dev);
#if NVR_TEST_TOOLS
			SunShadowStats::Mode = fx->Settings.Exteriors.CascadeStats;	// UNOFFICIAL 2026-10-21 test tools
			if (SunShadowStats::Mode) SunShadowStats::BeginFrame();
			const unsigned long long statsLoopStart = SunShadowStats::Mode ? SunShadowStats::Now() : 0;
#endif
			// CachedDistantShadows (ReducedQuality): the staggered schedule for the middle, far and LOD cascades, but a
			// cascade with a character in it is redrawn every frame, and once more after the last one left, so moving
			// characters never keep a stale shadow (the flicker the plain stagger caused). Other moving objects (doors,
			// physics clutter) are treated as still and can lag up to 3 or 7 frames in those cascades.
			const bool staggered = TheSettingManager->SettingsMain.Main.StaggeredSunShadows;
			// UNOFFICIAL 2026-10-31 SunCacheByChange ([Main.Main.ReducedQuality], off; root plan A1): the same cached middle, far and LOD
			// cascades, but redrawn when something changed instead of on the 4 / 8 frame schedule: the sun stepped, the camera moved or turned
			// by more than SunCacheMoveShare of the cascade's radius, a character is in it (as CachedDistantShadows), or SunCacheMaxAge
			// frames passed (the net for doors and physics clutter, which are not tracked). Their selection spheres stay with the world, as
			// the kept maps do.
			const bool byChange = TheSettingManager->GetSettingI("Main.Main.ReducedQuality", "SunCacheByChange") != 0 && !staggered;
			const unsigned cacheMaxAge = (unsigned)max(TheSettingManager->GetSettingI("Main.Main.ReducedQuality", "SunCacheMaxAge"), 1);
			const float cacheMoveShare = max(TheSettingManager->GetSettingF("Main.Main.ReducedQuality", "SunCacheMoveShare"), 0.0f);
			const bool cachedDistant = (TheSettingManager->SettingsMain.Main.CachedDistantShadows || byChange) && !staggered;
			// UNOFFICIAL 2026-10-31 SunLayers (DrawSunLayer above): the middle and far cascades from a kept static layer + a per-frame overlay
			const bool sunLayers = TheSettingManager->GetSettingI("Main.Main.ReducedQuality", "SunLayers") != 0 && !staggered;
			const int layerMargin = std::clamp(TheSettingManager->GetSettingI("Main.Main.ReducedQuality", "SunLayersMargin"), 8, 512);
			const unsigned layerMaxAge = (unsigned)max(TheSettingManager->GetSettingI("Main.Main.ReducedQuality", "SunLayersMaxAge"), 1);
			LayerTreesMoving = TheSettingManager->GetSettingI("Main.Main.ReducedQuality", "SunLayersTreesMoving") != 0;
			if (cachedDistant) CollectMovers(sunDir);
			// UNOFFICIAL 2026-10-21 (sunshadow): SunCascadeOneWalk schedules every cascade first (the schedule of one does not depend
			// on another's draw), walks once for all that redraw, then draws them in the same order. Without it each cascade is
			// scheduled, walked and drawn in turn, as before. SunCascadeSkipFlat: a flat cascade is cleared, not walked or drawn.
			const bool oneWalk = fx->Settings.Exteriors.SunOneWalk;
			const bool skipFlat = fx->Settings.Exteriors.SunSkipFlat;
			const bool leanDraws = fx->Settings.Exteriors.SunLeanDraws;	// RenderPass::Lean around the new path's draws
#if NVR_TEST_TOOLS
			const int verify = fx->Settings.Exteriors.SunVerify;
#endif
			// UNOFFICIAL 2026-10-27 displacement shadows (DisplaceShadows.h): from here to EndCascades the cell land near the camera is
			// sorted into the displaced land's pass.
			DisplaceShadows::BeginCascades();
			D3DXMATRIX cascadeViewProj[4];
			bool cascadeCached[4] = {};
			float cascadeInner[4] = {};
			unsigned flatCascades = 0;
			const char* cascadeWhy[4] = { "kept", "kept", "kept", "kept" };	// UNOFFICIAL 2026-11-01 frame journal: why each cascade was drawn
			auto drawCascade = [&](int i) {
				static const char* const cascadeNames[] = { "Sun cascade 0 (near)", "Sun cascade 1 (middle)", "Sun cascade 2 (far)", "Sun cascade 3 (lod)" };
				GpuDebug::LabelScope label(i >= 0 && i < 4 ? cascadeNames[i] : "Sun cascade");	// UNOFFICIAL 2026-10-26 GpuDebugLabels
				ShadowsExteriorEffect::ShadowMapSettings* sm = &fx->ShadowMaps[i];
				const bool flat = (flatCascades >> i) & 1;
				fx->Constants.ShadowViewProj = cascadeViewProj[i];
				SunLandDropScale = min(1.0f, 2.0f * max(sunDir.z, 0.0f));	// SunDir is a unit vector: z = sin(elevation)
#if NVR_TEST_TOOLS
				if (SunShadowStats::Mode) SunShadowStats::Sum[i].frames++;
				SunShadowStats::Current = SunShadowStats::Mode >= 2 ? &SunShadowStats::Sum[i] : nullptr;	// 2: the full split
				if (SunShadowStats::Current) {	// 3: the column check (ShadowMapCascadeCenterRadius is camera-relative)
					const D3DXVECTOR4& cr = sm->ShadowMapCascadeCenterRadius;
					SunShadowStats::CurrentColumn = { D3DXVECTOR3(cr.x + TheRenderManager->CameraPosition.x, cr.y + TheRenderManager->CameraPosition.y,
						cr.z + TheRenderManager->CameraPosition.z), sunDir, cr.w, &sm->ShadowMapFrustumPlanes };
					SunShadowStats::Current->radius = cr.w;
					static unsigned columnLogs = 0;
					if (SunShadowStats::Mode >= 3 && columnLogs < 16) {
						columnLogs++;
						NiCamera* camera = WorldSceneGraph->camera;
						Logger::Log("SUN CASCADE COLUMN %d: centre (camera-relative) %.0f %.0f %.0f radius %.1f | slice %.6f..%.6f of the clip range | camera near %.2f far %.0f fov %.1f"
							" | sun %.3f %.3f %.3f | camera at %.0f %.0f %.0f", i, cr.x, cr.y, cr.z, cr.w, sm->ShadowMapNear, sm->ShadowMapRadius,
							camera->Frustum.Near, camera->Frustum.Far, WorldSceneGraph->cameraFOV, sunDir.x, sunDir.y, sunDir.z,
							TheRenderManager->CameraPosition.x, TheRenderManager->CameraPosition.y, TheRenderManager->CameraPosition.z);
					}
				}
				// SunCascadeVerify 2: first the old way (its own walk, every caster drawn), its map kept for the comparison below.
				std::vector<UInt8> oldMap;
				bool oldRead = false;
				if (verify >= 2 && (oneWalk || flat || leanDraws)) {
					std::unordered_set<NiGeometry*> sharedLand;
					if (oneWalk) sharedLand.swap(RenderPass::LandSet);
					RenderShadowMap(sm, &cascadeViewProj[i]);
					if (oneWalk) RenderPass::LandSet.swap(sharedLand);
					oldRead = ReadCascadeRegion(fx, sm->ShadowMapViewPort, oldMap);
				}
#endif
				BeginShadowMap(sm, &cascadeViewProj[i], oneWalk);
#if NVR_TEST_TOOLS
				// SunCascadeVerify 1: the old walk of this cascade into the passes' lists, taken out again in draw order and compared
				// with this cascade's share of the one walk (and the land the drop applies to). Flat cascades: the map check proves them.
				if (verify >= 1 && oneWalk && !flat) {
					std::unordered_set<NiGeometry*> sharedLand;
					sharedLand.swap(RenderPass::LandSet);
					WalkShadowMap(sm);
					std::vector<std::pair<NiGeometry*, UInt8>> oldOrder, newOrder;
					for (UInt8 pass = kSunOpaque; pass <= kSunDisplaceLand; pass++) {
						std::stack<NiGeometry*>& list = SunPassOf(pass)->GeometryList;
						while (!list.empty()) { oldOrder.emplace_back(list.top(), pass); list.pop(); }
						for (auto c = SunCasters[i].rbegin(); c != SunCasters[i].rend(); ++c) if (c->pass == pass) newOrder.emplace_back(c->geometry, pass);
					}
					bool same = oldOrder == newOrder;
					unsigned landDiff = 0;
					for (const auto& caster : newOrder)
						if (caster.second == kSunOpaque && (RenderPass::LandSet.count(caster.first) != 0) != (sharedLand.count(caster.first) != 0)) landDiff++;
					RenderPass::LandSet.swap(sharedLand);
					if (same && !landDiff) SunVerify.listSame++;
					else {
						SunVerify.listDiff++;
						if (SunVerify.listDiff <= 20) {
							size_t first = 0;
							while (first < oldOrder.size() && first < newOrder.size() && oldOrder[first] == newOrder[first]) first++;
							Logger::Log("SUN CASCADE VERIFY: cascade %d lists DIFFER: old walk %u casters, one walk %u, first difference at %u; land drop differs on %u",
								i, (unsigned)oldOrder.size(), (unsigned)newOrder.size(), (unsigned)first, landDiff);
						}
					}
					SunVerify.casters += newOrder.size();
				}
#endif
				if (oneWalk) PushSunCasters(i);
				else if (!flat) WalkShadowMap(sm);
				RenderPass::Lean = leanDraws;
				DrawShadowMap(sm, oneWalk);
				RenderPass::Lean = false;
#if NVR_TEST_TOOLS
				if (verify >= 2 && (oneWalk || flat || leanDraws)) {
					std::vector<UInt8> newMap;
					if (!oldRead || !ReadCascadeRegion(fx, sm->ShadowMapViewPort, newMap)) SunVerify.mapFailed++;
					else if (oldMap == newMap) SunVerify.mapSame++;
					else {
						SunVerify.mapDiff++;
						if (SunVerify.mapDiff <= 20) {
							size_t differ = 0;
							for (size_t b = 0; b < oldMap.size() && b < newMap.size(); b++) differ += oldMap[b] != newMap[b];
							Logger::Log("SUN CASCADE VERIFY: cascade %d MAPS DIFFER%s: %u of %u bytes", i, flat ? " (flat)" : "", (unsigned)differ, (unsigned)oldMap.size());
						}
					}
				}
				if (verify) { SunVerify.cascades++; if (flat) SunVerify.flat++; }
				SunShadowStats::Current = nullptr;
#endif
				SunLandDropScale = 0.0f;
				CascadeRefreshCamera[i] = WorldSceneGraph->camera->m_worldTransform.pos.toD3DXVEC3();
				CascadeRefreshForward[i] = D3DXVECTOR3(TheRenderManager->CameraForward.x, TheRenderManager->CameraForward.y, TheRenderManager->CameraForward.z);	// SunCacheByChange
				CascadeRefreshSun[i] = sunDir;
				CascadeHadMover[i] = cascadeCached[i] && MoversInCascade(i, sm, cascadeInner[i]);
			};
			for (int i = MapNear; i < MapOrtho; i++) {
				ShadowsExteriorEffect::ShadowMapSettings* sm = &fx->ShadowMaps[i];
				if (sunLayers && (i == MapMiddle || i == MapFar) && DrawSunLayer(dev, fx, i, sunDir, ForceAllCascades, layerMargin, layerMaxAge)) {
					updatedCascades |= 1u << i;
					layeredCascades |= 1u << i;
					cascadeWhy[i] = "layer";
					SunCasters[i].clear();	// (not walked by the one walk: no list from an earlier frame left for CasterView)
					CascadeRefreshCamera[i] = WorldSceneGraph->camera->m_worldTransform.pos.toD3DXVEC3();
					CascadeRefreshForward[i] = D3DXVECTOR3(TheRenderManager->CameraForward.x, TheRenderManager->CameraForward.y, TheRenderManager->CameraForward.z);
					CascadeRefreshSun[i] = sunDir;
					CascadeHadMover[i] = false;
					continue;
				}
				const bool cachedCascade = cachedDistant && i != MapNear;
				const unsigned updatePeriod = SunCascadeUpdatePeriod(i, fx->Settings.ShadowMaps.LimitFrequency,
					TheSettingManager->SettingsMain.Main.NearCascadeInterval, staggered || cachedCascade);

				bool update = ForceAllCascades || SunCascadeUpdatesOnFrame(i, FrameCounter, updatePeriod);
				const float innerDepth = i > MapNear ? ((const float*)fx->Constants.ShadowMapRadius)[i - 1] : 0.0f;
				if (byChange && cachedCascade && !ForceAllCascades) {	// SunCacheByChange: the reasons replace the schedule
					const D3DXVECTOR3 camera = WorldSceneGraph->camera->m_worldTransform.pos.toD3DXVEC3();
					const D3DXVECTOR3 forward(TheRenderManager->CameraForward.x, TheRenderManager->CameraForward.y, TheRenderManager->CameraForward.z);
					const D3DXVECTOR4& sphere = sm->ShadowMapCascadeCenterRadius;
					const D3DXVECTOR3 moved = camera - CascadeRefreshCamera[i], turned = forward - CascadeRefreshForward[i];
					// how far the cascade's fitted region has drifted: the camera's move plus the turn swinging its centre round
					const float drift = D3DXVec3Length(&moved) + D3DXVec3Length(&turned) * sqrtf(sphere.x * sphere.x + sphere.y * sphere.y + sphere.z * sphere.z);
					const D3DXVECTOR3 sunMoved = sunDir - CascadeRefreshSun[i];
					int reason = -1;
					if (CascadeAge[i] + 1 >= cacheMaxAge) reason = 0;
					else if (D3DXVec3Length(&sunMoved) > 1e-4f) reason = 1;
					else if (drift > cacheMoveShare * sphere.w) reason = 2;
					update = reason >= 0;
					if (update) SunCacheReasons[i][reason]++;
					static const char* const reasonNames[] = { "max age", "sun moved", "camera moved" };
					if (update) cascadeWhy[i] = reasonNames[reason];
				}
				if (update) SunStats.Scheduled[i]++;
				else if (cachedCascade && (CascadeHadMover[i] || MoversInCascade(i, sm, innerDepth))) {
					update = true;
					SunStats.ForCharacters[i]++;
					cascadeWhy[i] = "character";
					if (byChange) SunCacheReasons[i][3]++;
				}
				if (byChange && cachedCascade) { if (update) CascadeAge[i] = 0; else { CascadeAge[i]++; SunCacheReasons[i][4]++; } }
				if (update) {
					updatedCascades |= 1u << i;
					if (updatePeriod == 1 && !cachedCascade) freshCascades |= 1u << i;
					if (!strcmp(cascadeWhy[i], "kept")) cascadeWhy[i] = ForceAllCascades ? "forced" : "schedule";
					if (cachedCascade) SessionLog::Count.cachedCascadeRedraws++;
					cascadeViewProj[i] = fx->GetCascadeViewProj(sm, &sunDir);
					cascadeCached[i] = cachedCascade;
					cascadeInner[i] = innerDepth;
					if (skipFlat && FlatCascade(cascadeViewProj[i])) flatCascades |= 1u << i;
					if (!oneWalk) drawCascade(i);
				}
				else {
					// Keep cached cascades locked to camera translation between geometry refreshes.
					D3DXVECTOR3 newCameraTranslation = WorldSceneGraph->camera->m_worldTransform.pos.toD3DXVEC3();
					D3DXVECTOR3 difference = newCameraTranslation - sm->CameraTranslation;
					D3DXMATRIX translationMatrix;
					D3DXMatrixTranslation(&translationMatrix, difference.x, difference.y, difference.z);
					sm->ShadowCameraToLight = translationMatrix * sm->ShadowCameraToLight;
					sm->CameraTranslation = newCameraTranslation;
					// A cached NEAR cascade (NearCascadeInterval 2) also moves its selection sphere with the
					// camera: the centre is camera-relative, and the near sphere is small enough for a stale
					// centre to matter. The rarely refreshed cascades keep their original behaviour.
					if (i == MapNear || byChange) {	// (SunCacheByChange: every kept cascade, its map stays with the world)
						sm->ShadowMapCascadeCenterRadius.x -= difference.x;
						sm->ShadowMapCascadeCenterRadius.y -= difference.y;
						sm->ShadowMapCascadeCenterRadius.z -= difference.z;
					}

				}

				std::string message = "ShadowManager::RenderShadowMap ";
				message += std::to_string(i);
				shadowMapTimer.LogTime(message.c_str());
			}
			// UNOFFICIAL 2026-11-01 frame journal: the cached / staggered cascades' redraws (the near one, drawn every frame, only when not)
			if ((updatedCascades & ~1u) || !(updatedCascades & 1u))	// (2026-11-01: always, for the in-memory ring of the FLASH / REPORT dumps)
				FRAME_JOURNAL("sun cascades: near %s, middle %s, far %s, lod %s%s", cascadeWhy[0], cascadeWhy[1], cascadeWhy[2], cascadeWhy[3],
					flatCascades ? " (some flat: cleared)" : "");
			SunCascadesDrawnMask = updatedCascades;	// UNOFFICIAL 2026-10-28 grass shadows past the near cascade (GrassShadows.h)
			if (oneWalk && (updatedCascades & ~layeredCascades)) {
				// The one walk, for the cascades that redraw and are not flat; then each draws its share, in the usual order.
				for (int c = 0; c < 4; c++) { SunForms[c] = &fx->ShadowMaps[c].Forms; SunPlanes[c] = &fx->ShadowMaps[c].ShadowMapFrustumPlanes; }
				SunLandDropScale = min(1.0f, 2.0f * max(sunDir.z, 0.0f));
				RenderPass::LandDrop = ShadowMapLandDrop();	// (AccumObject notes land only while a drop is wanted)
				RenderPass::LandSet.clear();
#if NVR_TEST_TOOLS
				const unsigned long long walkStart = SunShadowStats::Mode >= 2 ? SunShadowStats::Now() : 0;
#endif
				// UNOFFICIAL 2026-11-04 (from Physically-Based-Reloaded) CasterCulling / SkipCoveredCasters (SunCastMask), for the fresh cascades
				// only: one kept over frames (staggered, cached, the Lod cascade under LimitFrequency, the SunLayers ones) would show what was
				// left out as the camera turns. A fresh cascade's covering cascades are the fresh ones before it, each answering alone for its
				// sphere less its cross-fade band (Shadow.hlsl ShadowCascadeBands).
				{
					const ShadowsExteriorEffect::ShadowMapStruct& maps = fx->Settings.ShadowMaps;
					const unsigned walked = updatedCascades & ~layeredCascades & ~flatCascades;
					SunCastView = SunCastAny = 0;
					for (int c = 0; c < 4; c++) {
						SunCoveringCount[c] = 0;
						const unsigned bit = 1u << c;
						if (!(walked & bit) || !(freshCascades & bit)) continue;
						SunCastSphere[c] = fx->ShadowMaps[c].ShadowMapCascadeCenterRadius;
						if (maps.CasterCulling && fx->ShadowMaps[c].CasterPlanesValid) SunCastView |= bit;
						if (maps.SkipCoveredCasters)
							for (int j = 0; j < c; j++) {
								if (!(walked & freshCascades & (1u << j))) continue;
								D3DXVECTOR4 sphere = fx->ShadowMaps[j].ShadowMapCascadeCenterRadius;
								sphere.w -= (std::max)(0.1f * sphere.w, (std::min)(fx->Constants.ForwardData.w, sphere.w));
								SunCovering[c][SunCoveringCount[c]++] = sphere;
							}
						if ((SunCastView & bit) || SunCoveringCount[c]) SunCastAny |= bit;
					}
					D3DXVec3Normalize(&SunCastDir, &sunDir);
					SunCastPerHeight = 1.0f / (std::max)(SunCastDir.z, 0.05f);
					SunCastDir = -SunCastDir;
				}
				WalkSunCascades(updatedCascades & ~layeredCascades & ~flatCascades);
				SunCastView = SunCastAny = 0;
				{	// UNOFFICIAL 2026-11-04 CasterCulling: what it left out, every 1800 walks
					static unsigned walks = 0;
					if (++walks >= 1800) {
						Logger::Log("UNOFFICIAL sun caster culling: per frame %.1f casters left out of a cascade their shadow cannot reach the view in (CasterCulling), %.1f left out where an earlier cascade answers (SkipCoveredCasters)",
							SunCastersCulled / (double)walks, SunCastersCovered / (double)walks);
						walks = 0; SunCastersCulled = SunCastersCovered = 0;
					}
				}
#if NVR_TEST_TOOLS
				if (SunShadowStats::Mode >= 2) SunShadowStats::tSharedWalk += SunShadowStats::Now() - walkStart;
#endif
				RenderPass::LandDrop = 0.0f;
				SunLandDropScale = 0.0f;
				for (int i = MapNear; i < MapOrtho; i++) if (updatedCascades & ~layeredCascades & (1u << i)) drawCascade(i);
				RenderPass::LandSet.clear();
			}
#if NVR_TEST_TOOLS
			if (verify && ++SunVerify.frames >= 240) {
				Logger::Log("SUN CASCADE VERIFY %u frames, %u cascades drawn (%u flat): lists %u identical, %u differ (%.0f casters each on average); "
					"maps %u identical, %u differ, %u not read", SunVerify.frames, SunVerify.cascades, SunVerify.flat, SunVerify.listSame, SunVerify.listDiff,
					SunVerify.listSame + SunVerify.listDiff ? (double)SunVerify.casters / (SunVerify.listSame + SunVerify.listDiff) : 0.0,
					SunVerify.mapSame, SunVerify.mapDiff, SunVerify.mapFailed);
				SunVerify = {};
			}
			else if (!verify) SunVerify = {};
#endif
			if (ForceAllCascades) SunAtlasDrawn = true;	// UNOFFICIAL SkipWhenFaded: every cascade was drawn this frame
			ForceAllCascades = false;
			DisplaceShadows::EndCascades();	// UNOFFICIAL 2026-10-27
			LogSunShadowStats(cachedDistant);
#if NVR_TEST_TOOLS
			if (SunShadowStats::Mode) SunShadowStats::EndFrame(SunShadowStats::Now() - statsLoopStart);
#endif
			}

			// Resolve MSAA.
			if (fx->ShadowAtlasSurfaceMSAA) {
				GpuProfileScope gpu(atlasResolveTimer, dev);
				bool partialResolveFailed = false;
				for (int i = MapNear; i < MapOrtho; ++i) {
					if (!(updatedCascades & (1u << i))) continue;
					const D3DVIEWPORT9& savedViewport = fx->ShadowMaps[i].ShadowMapViewPort;
					RECT region = {(LONG)savedViewport.X, (LONG)savedViewport.Y,
						(LONG)(savedViewport.X + savedViewport.Width), (LONG)(savedViewport.Y + savedViewport.Height)};
					if (FAILED(dev->StretchRect(fx->ShadowAtlasSurfaceMSAA, &region,
						fx->ShadowAtlasSurface, &region, D3DTEXF_NONE))) {
						partialResolveFailed = true;
						break;
					}
				}
				if (partialResolveFailed) {
					dev->StretchRect(fx->ShadowAtlasSurfaceMSAA, NULL,
						fx->ShadowAtlasSurface, NULL, D3DTEXF_NONE);
					updatedCascades = (1u << MapOrtho) - 1;
				}
			}

			if (fx->Settings.ShadowMaps.Prefilter) {
				GpuProfileScope gpu(atlasFilterTimer, dev);
				BlurShadowAtlas(updatedCascades);
			}

			if (fx->Settings.ShadowMaps.Mipmaps)
				fx->ShadowAtlasTexture->GenerateMipSubLevels();

			// UNOFFICIAL 2026-10-23 CharacterShadowMap: the player's own map, from the same (quantised) sun, after the cascades.
			RenderCharacterShadowMap(sunDir);
		}
#if NVR_TEST_TOOLS
		// DIAGNOSTIC SkipWhenFadedCheck 4: the atlas set to 0 at any hour, nothing skipped (shows that the poison of 1 and 2
		// reaches the shaders that read the maps: by day everything in the cascades' reach turns to shadow).
		if (fx->Settings.Exteriors.SkipWhenFadedCheck == 4 && fx->ShadowAtlasSurface) {
			dev->SetRenderTarget(0, fx->ShadowAtlasSurface);
			dev->SetDepthStencilSurface(fx->ShadowAtlasDepthSurface);
			dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);
			ForceAllCascades = true;	// redrawn in full once the check is off
		}
#endif

		// render ortho map if one of the effects using ortho is active
		if (TheShaderManager->orthoRequired || TheShaderManager->orthoSkyRequired) {
			GpuProfileScope gpu(orthoMapTimer, dev);
			auto shadowMapTimer = TimeLogger();

			ShadowsExteriorEffect::ShadowMapSettings* sm = &fx->ShadowMaps[MapOrtho];

			// UNOFFICIAL 2026-10-17 Skylighting v2 (evidence skylight2-LOG.md): when ONLY the sky visibility needs the map (no rain or
			// snow effect asked for it this frame), it is redrawn only when the camera moved MoveRefresh units since the last draw,
			// every MapRefresh frames (cells that streamed in), on a settings change or after a pause in its use; in between it
			// moves with the camera as below. The statics, terrain and trees it holds do not change from frame to frame.
			bool draw = !fx->Settings.OrthoMap.LimitFrequency || !((FrameCounter + 2) % 4);
			static D3DXVECTOR3 skyLastDrawPos(0.0f, 0.0f, 0.0f);
			static int skyFramesSinceDraw = 1 << 20;
			static int skySettingsSeen = -1;
			static bool skyLastDrawSkyOnly = false;	// the map in the texture was drawn camera-centred (below)
			const bool skyOnly = TheShaderManager->orthoSkyRequired && !TheShaderManager->orthoRequired;
			const D3DXVECTOR3 cameraNow = WorldSceneGraph->camera->m_worldTransform.pos.toD3DXVEC3();
			if (skyOnly) {
				const D3DXVECTOR3 moved = cameraNow - skyLastDrawPos;
				const float moveRefresh = fx->Settings.OrthoMap.SkyMoveRefresh;
				draw = skyFramesSinceDraw >= fx->Settings.OrthoMap.SkyMapRefresh || TheShaderManager->orthoSkyFresh || !skyLastDrawSkyOnly ||
					D3DXVec3LengthSq(&moved) > moveRefresh * moveRefresh || skySettingsSeen != fx->Settings.OrthoMap.SkySettingsGeneration;
			}
			if (draw) {
				dev->SetRenderTarget(0, fx->ShadowMapOrthoSurface);
				dev->SetDepthStencilSurface(fx->ShadowMapOrthoDepthSurface);

				dataConst->z = 1; // identify ortho map in shader constant
				D3DXVECTOR3 orthoDir = D3DXVECTOR3(0.05f, 0.05f, 1.0f);
				// UNOFFICIAL Skylighting v2 (sky only): the map is centred on the CAMERA (a view slice from -2/3 to +2/3 of the ortho
				// distance: its bounding sphere is centred on the camera, ~3000 units round it at the default 3000), not on the view
				// ahead, so turning the camera never needs a redraw; only moving does (and then the cached map is translated).
				const float orthoNearWas = sm->ShadowMapNear, orthoRadiusWas = sm->ShadowMapRadius;
				if (skyOnly && fx->Settings.OrthoMap.SkyOwnProjection) {
					// UNOFFICIAL 2026-10-17 Skylighting v2 (OwnProjection, launch 2 diagnosis): an explicit top-down projection in camera-
					// relative world space (the space of the casters' world matrices, RenderManager::CreateD3DMatrix): straight down from
					// Distance above the camera, +y up in the map, +-Distance round the camera, depth 0..2 Distance, the world origin
					// snapped to a texel (as GetCascadeViewProj does) so a redraw does not shift the texels.
					NiCamera* sceneCamera = WorldSceneGraph->camera;
					const float R = fx->Settings.OrthoMap.Distance, H = fx->Settings.OrthoMap.Distance;
					D3DXVECTOR3 eye(0.0f, 0.0f, H), at(0.0f, 0.0f, 0.0f), up(0.0f, 1.0f, 0.0f);
					D3DXMATRIX view, proj;
					D3DXMatrixLookAtRH(&view, &eye, &at, &up);
					D3DXMatrixOrthoOffCenterRH(&proj, -R, R, -R, R, 0.0f, 2.0f * H);
					D3DXMATRIX viewProj = view * proj;
					const float mapSize = sm->ShadowMapResolution;
					D3DXVECTOR4 origin(-sceneCamera->m_worldTransform.pos.x, -sceneCamera->m_worldTransform.pos.y, -sceneCamera->m_worldTransform.pos.z, 1.0f);
					D3DXVec4Transform(&origin, &origin, &viewProj);
					const float ox = origin.x * mapSize * 0.5f, oy = origin.y * mapSize * 0.5f;
					proj._41 += (floorf(ox + 0.5f) - ox) * 2.0f / mapSize;
					proj._42 += (floorf(oy + 0.5f) - oy) * 2.0f / mapSize;
					viewProj = view * proj;
					NiFrustum frustum(-R, R, R, -R, 0.0f, 2.0f * H, true);
					TheCameraManager->SetFrustumPlanes(&sm->ShadowMapFrustumPlanes, &viewProj, eye, frustum);
					sm->ShadowMapFrustumPlanes.SetActivePlaneState(62);
					sm->ShadowMapCascadeCenterRadius = D3DXVECTOR4(0.0f, 0.0f, 0.0f, R);
					sm->Forms.MinRadius = sm->Forms.OrigMinRadius * R * sm->ShadowMapInverseResolution;
					sm->CameraTranslation = sceneCamera->m_worldTransform.pos.toD3DXVEC3();
					fx->Constants.ShadowViewProj = viewProj;
				}
				else {
					if (skyOnly) { sm->ShadowMapRadius = orthoRadiusWas * (2.0f / 3.0f); sm->ShadowMapNear = -sm->ShadowMapRadius; }
					fx->Constants.ShadowViewProj = fx->GetCascadeViewProj(sm, &orthoDir);
				}
				sm->ShadowMapNear = orthoNearWas;
				sm->ShadowMapRadius = orthoRadiusWas;

#if NVR_TEST_TOOLS
				// UNOFFICIAL 2026-10-17 OrthoProbe (test tools): a new ProbeDump value = this draw also writes each texel's caster (OrthoProbe.h).
				const int probeValue = fx->Settings.OrthoMap.ProbeDump;
				const bool probe = probeValue != 0 && probeValue != OrthoProbe::LastValue && OrthoProbe::Prepare(dev, (UINT)fx->Settings.OrthoMap.Resolution);
				DWORD probeWrite1 = 0;
				if (probe) {
					OrthoProbe::Owners.clear(); OrthoProbe::Drawn.clear(); OrthoProbe::Recording = true;
					for (int& n : OrthoProbe::LandCount) n = 0;
					dev->GetRenderState(D3DRS_COLORWRITEENABLE1, &probeWrite1);
					dev->SetRenderState(D3DRS_COLORWRITEENABLE1, 0xF);
					dev->SetRenderTarget(1, OrthoProbe::IdSurface);
				}
#endif
				// UNOFFICIAL Skylighting v2 CleanMap: alpha-tested casters with their test (leaves, fences), no blended-only geometry.
				// fixes-b #4: only the sky's own map (skyOnly): a map the rain or snow effect also reads stays as it was before v2 (solid casters), or the rain would fall through leaves and fences
				const bool clean = skyOnly && fx->Settings.OrthoMap.SkyCleanMap;
				const bool alphaWas = sm->Forms.AlphaEnabled;
				if (clean) { sm->Forms.AlphaEnabled = true; OrthoSkipBlendOnly = true; }
				RenderShadowMap(sm, &fx->Constants.ShadowViewProj);
				sm->Forms.AlphaEnabled = alphaWas;
				OrthoSkipBlendOnly = false;
				skyLastDrawPos = cameraNow;
				skyFramesSinceDraw = 0;
				skyLastDrawSkyOnly = skyOnly;
				skySettingsSeen = fx->Settings.OrthoMap.SkySettingsGeneration;
#if NVR_TEST_TOOLS
				if (probe) {
					OrthoProbe::Recording = false;
					dev->SetRenderTarget(1, nullptr);
					dev->SetRenderState(D3DRS_COLORWRITEENABLE1, probeWrite1);
					OrthoProbe::Dump(dev, fx->ShadowMapOrthoSurface, sm->ShadowCameraToLight, probeValue);
					OrthoProbe::LastValue = probeValue;
					OrthoProbe::Owners.clear(); OrthoProbe::Drawn.clear();
				}
#endif
				// 2026-11-12 Puddles map mode (effects/Puddles/PuddleMap.h): the puddle map from this frame's ortho map (solid casters only:
				// the map the rain reads)
				if (!skyOnly && PuddleMap::MapMode) {
					PuddlesShaders* puddles = TheShaderManager->Shaders.Puddles;
					if (puddles) PuddleMap::Build(dev, sm->ShadowCameraToLight, fx->ShadowMapOrthoTexture, fx->Settings.OrthoMap.Resolution,
						1.0f / puddles->Settings.Size, (float)puddles->Settings.Quality);
				}
			}
			else {
				D3DXVECTOR3 newCameraTranslation = WorldSceneGraph->camera->m_worldTransform.pos.toD3DXVEC3();
				D3DXVECTOR3 difference = newCameraTranslation - sm->CameraTranslation;
				D3DXMATRIX translationMatrix;
				D3DXMatrixTranslation(&translationMatrix, difference.x, difference.y, difference.z);
				sm->ShadowCameraToLight = translationMatrix * sm->ShadowCameraToLight;
				sm->CameraTranslation = newCameraTranslation;
				++skyFramesSinceDraw;
			}
			// UNOFFICIAL 2026-10-27 Persistent wetness and snow (CoverMap.h): the map the rain and snow effects read is this frame's (drawn
			// with solid casters, or moved with the camera), so the cover map may take the top surfaces' heights from it.
			if (!skyOnly) CoverMapOrthoFrame = TheFrameRateManager->FrameNumber;

			orthoConst->x = fx->Settings.OrthoMap.Distance * 2;
			orthoConst->y = sm->ShadowMapInverseResolution;
	
			shadowMapTimer.LogTime("ShadowManager::RenderShadowMap Ortho");
		}

		// UNOFFICIAL 2026-10-14: the light grid's map of the world from the sun (AmbientLight GridBounce), plain depth as the
		// ortho map, every few frames.
		AmbientLightEffect* ambientLight = TheShaderManager->Effects.AmbientLight;
		if (ambientLight && ambientLight->WantsGridSunMap(sunDir)) {
			static GpuTimer gridSunTimer("Light grid sun map");
			GpuProfileScope gpu(gridSunTimer, dev);
			RecalculateBillboardVectors(&sunDir);
			dev->SetRenderTarget(0, ambientLight->gridSunSurface);
			dev->SetDepthStencilSurface(ambientLight->gridSunDepth);
			dataConst->z = 1; // plain depth, as the ortho map
			fx->Constants.ShadowViewProj = ambientLight->GridSunViewProj(sunDir);
			// With the colour shader: every pass draws depth + the caster's colour (its diffuse texture) into two targets.
			const bool colour = ShadowGridColorPixel && ambientLight->gridSunColorSurface;
			RenderPass* passes[] = { geometryPass, alphaPass, skinnedGeoPass, speedTreePass, terrainLODPass };
			ShaderRecordPixel* saved[5] = {};
			if (colour) {
				for (int i = 0; i < 5; i++) { saved[i] = passes[i]->PixelShader; passes[i]->PixelShader = ShadowGridColorPixel; }
				dev->SetRenderTarget(1, ambientLight->gridSunColorSurface);
				RenderPass::BindDiffuse = true;
			}
			RenderShadowMap(&ambientLight->gridSunMap, &fx->Constants.ShadowViewProj);
			if (colour) {
				for (int i = 0; i < 5; i++) passes[i]->PixelShader = saved[i];
				dev->SetRenderTarget(1, nullptr);
				RenderPass::BindDiffuse = false;
				dataConst->w = 0.0f;
			}
			ambientLight->gridSunHasColor = colour;
		}

		// UNOFFICIAL 2026-10-19 [ShadowsExteriors.TerrainShadows] (after Conor's port of Community Shaders' Terrain Shadows): the land's
		// heights loaded and swept toward the sun on worker threads (effects/TerrainShadows.cpp); the SunFar pass applies them.
		fx->TerrainShadows.Update(dev, D3DXVECTOR3(TheShaderManager->ShaderConst.SunDir));   // the real sun, not the quantized one
	}

	// Render shadow maps for point lights
	bool usePointLights = (TheShaderManager->GameState.isDayTime > 0.5) ? extCfg->UsePointShadowsDay : extCfg->UsePointShadowsNight;

	AlphaEnabled = intCfg->Forms.AlphaEnabled;
	geometryPass->VertexShader = ShadowCubeMapVertex;
	geometryPass->PixelShader = ShadowCubeMapOpaquePixel;
	alphaPass->VertexShader = ShadowCubeMapVertex;
	alphaPass->PixelShader = ShadowCubeMapPixel;
	skinnedGeoPass->VertexShader = ShadowCubeMapVertex;
	// UNOFFICIAL 2026-11-02 SkinnedAlphaTest (RenderPass.h), lamp cube maps
	SkinnedGeoShadowRenderPass::AlphaTest = TheSettingManager->GetSettingI("Shaders.ShadowsInteriors.Main", "SkinnedAlphaTest") != 0;
	skinnedGeoPass->PixelShader = SkinnedGeoShadowRenderPass::AlphaTest ? ShadowCubeMapPixel : ShadowCubeMapOpaquePixel;
	speedTreePass->VertexShader = ShadowCubeMapVertex;
	speedTreePass->PixelShader = ShadowCubeMapPixel;

	auto shadowMapTimer = TimeLogger();
	// (UNOFFICIAL 2026-10-29: outdoors no cube maps while the traced lamp mask stands in for them: RT lamp shadows Exteriors)
	if ((exteriorCell && usePointLights && !RtLampShadows::ExteriorReplacing()) || (!exteriorCell && lampShadowsOn)) {
		GpuProfileScope gpu(pointMapsTimer, dev);
		// Each slot's cubemap stores length(light - point) / radius, which does not depend on the camera,
		// so with PointShadowInterval > 1 a slot can keep its contents between redraws (see
		// PointShadowSchedule.h for when it must be redrawn at once).
		static PointShadowSlotState slots[ShadowCubeMapsMax];
		static unsigned scheduleFrame = 0, statFrames = 0, statPresent = 0, statRedrawn = 0, statStaticReused = 0;
		static unsigned statReasons[(int)PointShadowRedraw::Count] = {};
		const unsigned interval = (unsigned)TheSettingManager->SettingsMain.Main.PointShadowInterval;
		// UNOFFICIAL RedrawActorsOnly (PointShadowSchedule.h PointShadowDrawPlan): what each slot's static cube map holds.
		static PointShadowSlotState staticSlots[ShadowCubeMapsMax];
		static unsigned statStaticDrawn = 0, statMovingOnly = 0, statWhole = 0;
		static unsigned statMovedLamps = 0, statMovedWithPeople = 0, statAnchored = 0;	// UNOFFICIAL 2026-10-18 diagnostic (intshadow)
		static float statMovedStep = 0.0f, statMovedStepMax = 0.0f, statMovedWander = 0.0f, statMovedRadius = 0.0f;
		static unsigned statMovedResized = 0;
		static float statMovedRadiusStep = 0.0f;
		static const void* movedLight[ShadowCubeMapsMax] = {};
		static float movedHome[ShadowCubeMapsMax][3] = {};
		const bool split = intCfg->RedrawActorsOnly && ActorsOnlySupported(dev);

		// render the cubemaps for each light; the last slot's cubemap is never sampled (ShadowCubeMapsSampled), so it is not drawn
		// (UNOFFICIAL: with forward shadows indoors every slot up to LightPoints is drawn, PointShadowForward::ShadowedSlots)
		const int shadowedSlots = PointShadowForward::ShadowedSlots(exteriorCell);
		for (int i = 0; i < shadowedSlots; i++) {
			ShadowSceneLight* shadowLight = castLamps[i];
			if (!shadowLight || !fx->EnsureCubeMap(i)) { slots[i].valid = false; continue; } // no light (or no cube map) at this index

			PointShadowSlotState now;
			NiPointLight* pointLight = shadowLight->sourceLight;
			now.light = shadowLight;
			now.texture = fx->Textures.ShadowCubeMapTexture[i];
			now.cell = playerCell;
			now.x = pointLight->m_worldTransform.pos.x;
			now.y = pointLight->m_worldTransform.pos.y;
			now.z = pointLight->m_worldTransform.pos.z;
			now.radius = pointLight->Spec.r * intCfg->LightRadiusMult; // as in RenderShadowCubeMap (no 256 for CanCarry lamps, see there)
			PointShadowCasterState(shadowLight, now.casterHash, now.staticCasters);
			now.valid = true;
			// UNOFFICIAL 2026-10-18 LampShadowMoveTolerance ([Main.Main.ReducedQuality], CHANGES THE IMAGE, off at 0): a lamp that has
			// moved less than this from where its cube map was drawn (a flickering light's jitter) keeps that point: its cube map is
			// kept or refreshed from there, and the forward lookups use it (PointShadowForward::Publish takes the drawn position).
			// Indoors with the forward shadows only (the screen-space pass looks up from the light's own position).
			NiPoint3 anchor = { now.x, now.y, now.z };
			bool anchored = false;
			{
				const float tolerance = TheSettingManager->SettingsMain.Main.LampShadowMoveTolerance;
				if (tolerance > 0.0f && !exteriorCell && PointShadowForward::UsesAllSlots(exteriorCell) && ForwardWasActive &&
					slots[i].valid && slots[i].light == shadowLight && slots[i].cell == now.cell && slots[i].radius == now.radius) {
					const float dx = now.x - slots[i].x, dy = now.y - slots[i].y, dz = now.z - slots[i].z;
					if ((dx != 0.0f || dy != 0.0f || dz != 0.0f) && dx * dx + dy * dy + dz * dz <= tolerance * tolerance) {
						now.x = anchor.x = slots[i].x; now.y = anchor.y = slots[i].y; now.z = anchor.z = slots[i].z;
						anchored = true;
						if (GpuTimer::Enabled) statAnchored++;
					}
				}
			}
			// UNOFFICIAL 2026-10-20 InvisibleShadowFromLamp ([Shaders.ShadowsInteriors.Forward], a LOOK OPTION, 0 off): an invisible light
			// (no lamp part within FixtureDistance) with a lamp part within that distance has its cube map drawn from that lamp part, over
			// its reach plus the move, without that lamp: it still lights the room from where the designers put it, but its shadows fall
			// from a lamp the player can see. The forward lookups read the cube from where it was drawn (Publish takes now's position).
			float radiusOverride = 0.0f;
			int skipModel = -1;
			bool relocated = false;
			{
				const ShadowsExteriorEffect::InteriorsStruct::ForwardStruct& forward = intCfg->Forward;
				if (forward.InvisibleShadowFromLamp > 0.0f && forward.Enabled && !exteriorCell && PointShadowForward::UsesAllSlots(exteriorCell) &&
					LampFixtures::Invisible(pointLight, forward.FixtureDistance)) {
					if (const LampFixtures::Glow* lamp = LampFixtures::NearestLampPart(pointLight, forward.InvisibleShadowFromLamp)) {
						const float dx = lamp->x - pointLight->m_worldTransform.pos.x, dy = lamp->y - pointLight->m_worldTransform.pos.y,
							dz = lamp->z - pointLight->m_worldTransform.pos.z;
						now.x = anchor.x = lamp->x; now.y = anchor.y = lamp->y; now.z = anchor.z = lamp->z;
						now.radius += sqrtf(dx * dx + dy * dy + dz * dz);
						radiusOverride = now.radius;
						skipModel = lamp->model;
						relocated = true;
						static std::unordered_map<const void*, int> loggedMove;
						if (intCfg->Forward.LogLamps && loggedMove.size() < 256 && !loggedMove.count(shadowLight)) {
							loggedMove[shadowLight] = 1;
							Logger::Log("LAMP %p (invisible, at %.0f %.0f %.0f) casts its shadows from the lamp part '%s' of '%s' at %.0f %.0f %.0f (%.0f units away), cube radius %.0f",
								(const void*)shadowLight, pointLight->m_worldTransform.pos.x, pointLight->m_worldTransform.pos.y, pointLight->m_worldTransform.pos.z,
								lamp->name ? lamp->name : "", lamp->root ? lamp->root : "", lamp->x, lamp->y, lamp->z, sqrtf(dx * dx + dy * dy + dz * dz), now.radius);
						}
					}
				}
			}
			if (GpuTimer::Enabled) statPresent++;
			const PointShadowRedraw why = PointShadowRedrawReason(slots[i], now, scheduleFrame, i, interval);
			// UNOFFICIAL 2026-10-29 RT lamp shadows SkipTracedCubes: a traced lamp keeps its cube map (the fallback) instead of a redraw
			if (why != PointShadowRedraw::None && !exteriorCell && RtLampShadows::KeepCube((int)i, slots[i], now)) continue;
			if (why == PointShadowRedraw::None) {
				if (GpuTimer::Enabled && now.staticCasters) statStaticReused++;
				continue;
			}
			// UNOFFICIAL 2026-10-18 diagnostic (intshadow): lamps redrawn because they moved -- how far, and whether they wander or
			// jitter round one place (a flickering light's movement) -- with the F10 profile on.
			if (GpuTimer::Enabled && why == PointShadowRedraw::Moved) {
				const float dx = now.x - slots[i].x, dy = now.y - slots[i].y, dz = now.z - slots[i].z;
				const float step = sqrtf(dx * dx + dy * dy + dz * dz);
				statMovedLamps++; statMovedStep += step; statMovedStepMax = (std::max)(statMovedStepMax, step); statMovedRadius += now.radius;
				if (now.radius != slots[i].radius) {
					statMovedResized++;
					statMovedRadiusStep = (std::max)(statMovedRadiusStep, fabsf(now.radius - slots[i].radius) / (std::max)(now.radius, 1.0f));
				}
				if (!now.staticCasters && shadowLight->kGeometryList.start) statMovedWithPeople++;
				if (movedLight[i] != shadowLight) { movedLight[i] = shadowLight; movedHome[i][0] = now.x; movedHome[i][1] = now.y; movedHome[i][2] = now.z; }
				const float hx = now.x - movedHome[i][0], hy = now.y - movedHome[i][1], hz = now.z - movedHome[i][2];
				statMovedWander = (std::max)(statMovedWander, sqrtf(hx * hx + hy * hy + hz * hz));
				static std::unordered_map<const void*, bool> named;
				static unsigned namedLines = 0;
				if (namedLines < 16 && !named[shadowLight]) {
					named[shadowLight] = true; namedLines++;
					NiNode* parent = pointLight->m_parent;
					Logger::Log("POINT SHADOWS MOVING LAMP %p '%s' (parent '%s') at %.1f %.1f %.1f, cube radius %.0f, step %.3f units, %s casters",
						(const void*)shadowLight, pointLight->m_pcName ? pointLight->m_pcName : "", parent && parent->m_pcName ? parent->m_pcName : "",
						now.x, now.y, now.z, now.radius, step, now.staticCasters ? "only still" : shadowLight->kGeometryList.start ? "moving" : "no listed");
				}
			}

			// UNOFFICIAL RedrawActorsOnly: with people in reach, the walls and furniture come from the lamp's static cube map
			// (drawn when missing or stale) and only the people are drawn again.
			const bool hasList = shadowLight->kGeometryList.start != nullptr;
			const bool moving = hasList && !now.staticCasters;
			PointShadowSlotState staticNow = now;
			staticNow.staticCasters = true;
			staticNow.texture = nullptr;
			if (split && hasList) {
				AdoptStaticCubeMap(fx, staticSlots, i, shadowLight);
				if (moving) fx->EnsureStaticCubeMap(i);
				staticNow.texture = fx->Textures.ShadowCubeMapStaticTexture[i];
			}
			const PointShadowDraw draw = PointShadowDrawPlan(split && hasList, staticSlots[i], staticNow, moving);
			CubeEyeOverride = anchored || relocated ? &anchor : nullptr;	// UNOFFICIAL LampShadowMoveTolerance: draw from the kept point
			CubeRadiusOverride = radiusOverride;	// UNOFFICIAL InvisibleShadowFromLamp (0: the light's own)
			CubeRelocated = relocated;
			CubeSkipModel = skipModel;
			// UNOFFICIAL CubeFaceViewCull: only for a lamp redrawn every frame anyway (above RenderShadowCubeMap)
			// (Any reader of the cube maps indoors -- the forward lookups or the screen-space lamp mask -- reads them for pixels in view.)
			CubeCullAllowed = TheSettingManager->SettingsMain.Main.CubeFaceViewCull && interval <= 1 && !exteriorCell &&
				(moving || why == PointShadowRedraw::Moved);
			CubeCulledAny = false;
			struct EyeReset { ~EyeReset() { CubeEyeOverride = nullptr; CubeCullAllowed = false; CubeRadiusOverride = 0.0f; CubeRelocated = false; CubeSkipModel = -1; } } eyeReset;
			// UNOFFICIAL 2026-10-18 diagnostic (intshadow): GPU time per lamp of each kind of draw (avg per lamp; the counts per frame
			// are in the POINT SHADOWS lines), F10 profile only.
			static GpuTimer staticLayerTimer("  Cube static layer (1 lamp)", 480), movingOntoTimer("  Cube people onto copy (1 lamp)", 960),
				wholeTimer("  Cube whole lamp (1 lamp)", 480);
			if (draw == PointShadowDraw::StaticThenMoving) {
				GpuProfileScope gpuLamp(staticLayerTimer, dev);
				RenderShadowCubeMap(castLamps, i, CubeLayer::StaticOnly);
				staticSlots[i] = staticNow;
				if (GpuTimer::Enabled) statStaticDrawn++;
			}
			bool drawn = false;
			if (draw != PointShadowDraw::All) {
				GpuProfileScope gpuLamp(movingOntoTimer, dev);
				drawn = RenderShadowCubeMap(castLamps, i, CubeLayer::MovingOnto);
				if (drawn && GpuTimer::Enabled) statMovingOnly++;
				if (!drawn) {
					ActorsOnlyBroken = true;
					Logger::Log("UNOFFICIAL RedrawActorsOnly: copying a static cube map failed; lamps are redrawn whole from now on, as before");
				}
			}
			// Render targets set in function due to rendering multiple faces.
			if (!drawn) {
				GpuProfileScope gpuLamp(wholeTimer, dev);
				RenderShadowCubeMap(castLamps, i);
				if (GpuTimer::Enabled) statWhole++;
			}
			now.partial = CubeCulledAny;	// UNOFFICIAL CubeFaceViewCull: redrawn next frame whatever happens
			slots[i] = now;
			if (i < 64) ShadowAtlas::LiveRedrawn[i] = true;	// UNOFFICIAL 2026-10-30 ShadowAtlas: its tile is converted again
			if (GpuTimer::Enabled) { statRedrawn++; statReasons[(int)why]++; }

			std::string message = "ShadowManager::RenderShadowCubeMap ";
			message += std::to_string(i);
			shadowMapTimer.LogTime(message.c_str());
		}
		scheduleFrame++;
		// UNOFFICIAL 2026-10-29 ShadowPool (core/ShadowPool.h): static cube maps for the next lamps of the ranking, drawn a few a frame.
		static PointShadowForward::PoolEntry poolEntries[ShadowPoolMax];
		int poolCount = 0;
		{
			const int want = ShadowPool::Wanted();
			const int spare = ShadowCubeMapsMax - 1;	// its static entries hold each pool cube while it is drawn
			static bool spareWarned = false;
			if (!want || exteriorCell || !PointShadowForward::UsesAllSlots(exteriorCell)) {
				if (!want && !ShadowPool::Cubes.empty()) ShadowPool::Release();
			}
			else if (shadowedSlots >= spare) {
				if (!spareWarned) { spareWarned = true; Logger::Log("[WARNING] UNOFFICIAL ShadowPool: needs [Shaders.ShadowsInteriors.Main] LightPoints below %d (it draws through the last slot): off", spare); }
			}
			else if (fx->CubeMapSizeCreated && fx->EnsureStaticCubeMap(spare)) {
				ShadowPool::Frame++;
				if (ShadowPool::MadeSize != fx->CubeMapSizeCreated) { ShadowPool::Release(); ShadowPool::MadeSize = fx->CubeMapSizeCreated; }
				LARGE_INTEGER frequency, counter;
				QueryPerformanceFrequency(&frequency);
				QueryPerformanceCounter(&counter);
				const double nowSeconds = (double)counter.QuadPart / (double)frequency.QuadPart;
				int budget = ShadowPool::Budget();
				static GpuTimer poolTimer("  Cube pool static layer (per frame)", 480);
				GpuProfileScope gpuPool(poolTimer, dev);
				// UNOFFICIAL 2026-10-30 ShadowHold (core/ShadowHold.h): members first (a lamp that had a shadow last frame and ranked inside the
				// pool within the last ShadowHold seconds), then the newcomers inside the pool's rank, so the edge of the pool stops flipping.
				const float holdSeconds = ShadowHold::Seconds();
				std::vector<ShadowSceneLight*> poolOrder;
				if (holdSeconds > 0.0f) {
					for (size_t r = 0; r < ShadowPool::Candidates.size(); r++) if ((int)r < want) ShadowHold::Map[ShadowPool::Candidates[r]].lastTop = nowSeconds;
					for (size_t r = 0; r < ShadowPool::Candidates.size(); r++) {
						ShadowSceneLight* l = ShadowPool::Candidates[r];
						if (ShadowHold::Continuous(l) && nowSeconds - ShadowHold::Map[l].lastTop <= holdSeconds) {
							poolOrder.push_back(l);
							if ((int)r >= want && GpuTimer::Enabled) ShadowHold::StatHeld++;
						}
					}
					for (size_t r = 0; r < ShadowPool::Candidates.size() && (int)r < want; r++) {
						ShadowSceneLight* l = ShadowPool::Candidates[r];
						if (std::find(poolOrder.begin(), poolOrder.end(), l) == poolOrder.end()) poolOrder.push_back(l);
					}
				}
				for (ShadowSceneLight* light : holdSeconds > 0.0f ? poolOrder : ShadowPool::Candidates) {
					if (poolCount >= want) break;
					NiPointLight* source = light ? light->sourceLight : nullptr;
					if (!source) continue;
					const float radius = source->Spec.r * intCfg->LightRadiusMult;	// as the slot loop
					const NiPoint3& at = source->m_worldTransform.pos;
					const int index = ShadowPool::EntryFor(light, fx->CubeMapSizeCreated, want);
					if (index < 0) continue;
					ShadowPool::Cube& cube = ShadowPool::Cubes[index];
					bool current = cube.valid && cube.x == at.x && cube.y == at.y && cube.z == at.z && cube.radius == radius && cube.cell == playerCell;
					// UNOFFICIAL 2026-10-30 ShadowHold: a flicker's reach change under 2 % (position under 1 unit) keeps the map; a map that is not
					// current is still used until its redraw (same cell, moved under 32 units), instead of no shadow meanwhile.
					bool stale = false;
					if (!current && holdSeconds > 0.0f && cube.valid && cube.cell == playerCell) {
						const float dx = cube.x - at.x, dy = cube.y - at.y, dz = cube.z - at.z, d2 = dx * dx + dy * dy + dz * dz;
						if (d2 < 1.0f && fabsf(cube.radius - radius) <= 0.02f * radius) { current = true; if (GpuTimer::Enabled) ShadowHold::StatNear++; }
						else stale = d2 < 32.0f * 32.0f;
					}
					if (!current && stale && budget <= 0) {
						if (GpuTimer::Enabled) ShadowHold::StatStale++;
						current = true;	// published below with its last map
					}
					if (!current) {
						if (budget <= 0) { if (GpuTimer::Enabled) ShadowPool::StatWaiting++; continue; }	// drawn on a later frame; no shadow until then
						budget--;
						IDirect3DCubeTexture9*& tex = fx->Textures.ShadowCubeMapStaticTexture[spare];
						IDirect3DSurface9** faces = fx->Textures.ShadowCubeMapStaticSurface[spare];
						std::swap(tex, cube.texture);
						for (int f = 0; f < 6; f++) std::swap(faces[f], cube.faces[f]);
						ShadowSceneLight* one[ShadowCubeMapsMax] = {};
						one[spare] = light;
						const bool drawn = RenderShadowCubeMap(one, spare, CubeLayer::StaticOnly);
						std::swap(tex, cube.texture);
						for (int f = 0; f < 6; f++) std::swap(faces[f], cube.faces[f]);
						if (!drawn) continue;
						cube.x = at.x; cube.y = at.y; cube.z = at.z; cube.radius = radius; cube.cell = playerCell; cube.valid = true;
						cube.drawn++;	// UNOFFICIAL 2026-10-30 ShadowAtlas
						SessionLog::Count.poolDraws++;	// UNOFFICIAL 2026-11-01
						FRAME_JOURNAL("lamp shadow pool: cube %d drawn for lamp %p (static casters)", index, light);
						if (GpuTimer::Enabled) ShadowPool::StatDrawn++;
					}
					if (cube.since <= 0.0) cube.since = nowSeconds;
					cube.used = ShadowPool::Frame;
					poolEntries[poolCount++] = { light, D3DXVECTOR3(cube.x, cube.y, cube.z), cube.radius, cube.texture, ShadowCubeMapsMax + index, cube.since };
				}
				if (GpuTimer::Enabled) {
					ShadowPool::StatPublished += poolCount;
					if (++ShadowPool::StatFrames >= 240) {
						const float f = 1.0f / ShadowPool::StatFrames;
						Logger::Log("SHADOW POOL per frame: %.1f lamps past the live slots, %.1f with their static cube map, %.2f drawn, %.2f waiting for a draw; %u cube maps (%u x %u)",
							(float)ShadowPool::Candidates.size(), ShadowPool::StatPublished * f, ShadowPool::StatDrawn * f, ShadowPool::StatWaiting * f,
							(unsigned)ShadowPool::Cubes.size(), ShadowPool::MadeSize, ShadowPool::MadeSize);
						ShadowPool::StatFrames = ShadowPool::StatPublished = ShadowPool::StatDrawn = ShadowPool::StatWaiting = 0;
					}
				}
			}
		}
		// UNOFFICIAL interior forward point-light shadows: the lights whose cube map is theirs this frame.
		if (!exteriorCell) {
			PointShadowForward::Publish(slots, castLamps, shadowedSlots, fx->Textures.ShadowCubeMapTexture);
			const int liveCount = PointShadowForward::SlotCount;	// UNOFFICIAL 2026-10-30 ShadowHold
			PointShadowForward::PublishPool(poolEntries, poolCount);	// UNOFFICIAL 2026-10-29 ShadowPool (nothing when the pool is off)
			ShadowAtlas::Update(dev, shadowedSlots);	// UNOFFICIAL 2026-10-30 ShadowAtlas: every published lamp's tile (nothing when off)
			{	// UNOFFICIAL 2026-10-30 ShadowHold: which lamps have a shadow this frame (FadeIn continuity, the SHADOW FLIPS line)
				static const void* published[ShadowCubeMapsMax + ShadowPoolMax];
				int n = 0;
				for (int s = 0; s < PointShadowForward::SlotCount; s++) {
					const void* l = PointShadowForward::Slots[s].light;
					bool dup = false;
					for (int k = 0; k < n; k++) if (published[k] == l) { dup = true; break; }
					if (!dup) published[n++] = l;
				}
				ShadowHold::AfterPublish(published, n, liveCount);
			}
			ForwardWasActive = PointShadowForward::Active;
			PointShadowForward::FrameStats();
		}

		if (GpuTimer::Enabled && ++statFrames >= 240) { // with the F10 profile: how much work the cubemaps really are
			const float perFrame = 1.0f / statFrames;
			Logger::Log("POINT SHADOWS interval %u: %.1f lights present, %.1f cubemaps redrawn, %.1f static cubemaps reused per frame (%u frames); "
				"redrawn because: scheduled %.2f, casters %.2f, new %.2f, other light %.2f, moved %.2f, resized %.2f, cell %.2f, texture %.2f",
				interval, statPresent * perFrame, statRedrawn * perFrame, statStaticReused * perFrame, statFrames,
				statReasons[(int)PointShadowRedraw::Scheduled] * perFrame, statReasons[(int)PointShadowRedraw::CastersChanged] * perFrame,
				statReasons[(int)PointShadowRedraw::NewSlot] * perFrame,
				statReasons[(int)PointShadowRedraw::OtherLight] * perFrame, statReasons[(int)PointShadowRedraw::Moved] * perFrame,
				statReasons[(int)PointShadowRedraw::Resized] * perFrame, statReasons[(int)PointShadowRedraw::OtherCell] * perFrame,
				statReasons[(int)PointShadowRedraw::OtherTexture] * perFrame);
			Logger::Log("POINT SHADOWS skinned casters (SkinnedShadowFaceTest %s): %.1f per frame drawn into %.1f faces (all faces would be %.1f), %.1f skipped beyond the lamp's reach, %.1f without a usable skeleton bound",
				TheSettingManager->SettingsMain.Main.SkinnedShadowFaceTest ? "on" : "off", statSkinnedCasters * perFrame, statSkinnedFaces * perFrame,
				statSkinnedCasters * perFrame * 6.0f, statSkinnedOutOfRange * perFrame, statSkinnedNoBound * perFrame);
			unsigned rigidMoving = 0;
			for (const auto& entry : CasterMotions)
				if (entry.second.seen == SkinnedBoundFrame && entry.second.changed && SkinnedBoundFrame - entry.second.changed < MovingSettleFrames) rigidMoving++;
			Logger::Log("POINT SHADOWS RedrawActorsOnly %s: per frame %.1f lamps redrawn whole, %.1f only their people (a copy of the walls and furniture, plus %.1f static layers drawn); %u rigid pieces moving now (held weapons, doors...)",
				split ? "on" : (intCfg->RedrawActorsOnly ? "unavailable" : "off"), statWhole * perFrame, statMovingOnly * perFrame, statStaticDrawn * perFrame, rigidMoving);
			// UNOFFICIAL 2026-10-18 (intshadow): CubeFaceCopySkip's faces, and the lamps that moved.
			Logger::Log("POINT SHADOWS CubeFaceCopySkip %s: per frame %.1f faces left as they were (still the static copy), %.1f copied only, %.1f copied and drawn on, %.1f by the old path (copy all six, draw all six)",
				TheSettingManager->SettingsMain.Main.CubeFaceCopySkip ? "on" : "off", statFacesSkipped * perFrame, statFacesCopied * perFrame, statFacesDrawn * perFrame, statFacesOldPath * perFrame);
			Logger::Log("POINT SHADOWS moved lamps: %.2f per frame (%.2f with moving casters), step per frame mean %.3f max %.3f units, furthest from where first seen %.2f units, cube radius mean %.0f",
				statMovedLamps * perFrame, statMovedWithPeople * perFrame, statMovedLamps ? statMovedStep / statMovedLamps : 0.0f, statMovedStepMax, statMovedWander,
				statMovedLamps ? statMovedRadius / statMovedLamps : 0.0f);
			Logger::Log("POINT SHADOWS LampShadowMoveTolerance %.1f: %.2f lamps per frame kept their drawn point; view test (diagnostic): per frame faces drawn whole %.1f / static layer %.1f / people %.1f, of them wholly outside the view %.1f / %.1f / %.1f",
				TheSettingManager->SettingsMain.Main.LampShadowMoveTolerance, statAnchored * perFrame, statViewFaces[0] * perFrame, statViewFaces[1] * perFrame, statViewFaces[2] * perFrame,
				statViewFacesOut[0] * perFrame, statViewFacesOut[1] * perFrame, statViewFacesOut[2] * perFrame);
			Logger::Log("POINT SHADOWS CubeFaceViewCull %s: per frame faces left out (out of view) %.1f of whole lamps, %.1f of people refreshes; partial redraws %.2f; moved lamps whose radius changed too %.2f per frame (largest change %.1f%%)",
				TheSettingManager->SettingsMain.Main.CubeFaceViewCull ? "on" : "off", statFacesCulled[0] * perFrame, statFacesCulled[2] * perFrame,
				statReasons[(int)PointShadowRedraw::Partial] * perFrame, statMovedResized * perFrame, 100.0f * statMovedRadiusStep);
			statAnchored = 0; statMovedResized = 0; statMovedRadiusStep = 0.0f;
			for (int k = 0; k < 3; k++) statViewFaces[k] = statViewFacesOut[k] = statFacesCulled[k] = 0;
			statFacesSkipped = statFacesCopied = statFacesDrawn = statFacesOldPath = 0;
			statMovedLamps = statMovedWithPeople = 0; statMovedStep = statMovedStepMax = statMovedWander = statMovedRadius = 0.0f;
			statSkinnedCasters = statSkinnedFaces = statSkinnedOutOfRange = statSkinnedNoBound = 0;
			statFrames = statPresent = statRedrawn = statStaticReused = 0;
			statStaticDrawn = statMovingOnly = statWhole = 0;
			for (unsigned& r : statReasons) r = 0;
		}
	}

	if (TheShaderManager->Effects.Flashlight->Enabled && TheShaderManager->Effects.Flashlight->spotLightActive && TheShaderManager->Effects.Flashlight->WantsShadowMap()) {
		GpuProfileScope gpu(flashlightMapsTimer, dev);
		// render shadow maps for spotlights
		
		for (int i = 0; i < SpotLightsMax; i++) {
			if (!nearSpots[i] || nearSpots[i]->Spec.r == 0) continue; //bypass lights with no radius

			// Render targets set in function.
			RenderShadowSpotlight(nearSpots, i);

			std::string message = "ShadowManager::RenderShadowSpotLight";
			message += std::to_string(i);
			shadowMapTimer.LogTime(message.c_str());
		}
	}

	// reset renderer to previous state
	dataConst->x = worldQuality;	// UNOFFICIAL 2026-11-05 (above)
	dataConst->y = worldDarkness;
	dev->SetDepthStencilSurface(savedDepth);
	dev->SetRenderTarget(0, savedTarget);
	dev->SetViewport(&savedViewport);
	dev->SetRenderState(D3DRS_ZFUNC, savedZFunc);
	dev->SetRenderState(D3DRS_STENCILENABLE, savedStencilEnable);
	dev->SetRenderState(D3DRS_STENCILREF, savedStencilRef);
	dev->SetRenderState(D3DRS_STENCILFUNC, savedStencilFunc);
	dev->SetRenderState(D3DRS_ALPHAREF, savedAlphaRef);
	dev->SetRenderState(D3DRS_NORMALIZENORMALS, savedNormalize);
	dev->SetRenderState(D3DRS_POINTSIZE, savedPointSize);

	//release smart pointers to prevent memory leak
	if (savedDepth) savedDepth->Release();
	if (savedTarget) savedTarget->Release();

	if (TheSettingManager->SettingsMain.Develop.DebugMode && !InterfaceManager->IsActive(Menu::MenuType::kMenuType_Console)) {
		if (Global->OnKeyDown(0x17)) { // TODO: setting for debug key ?
			char dumpDir[MAX_PATH];

			time_t stamp = time(NULL);
			GetCurrentDirectoryA(MAX_PATH, dumpDir);
			strcat(dumpDir, "\\Test");
			if (GetFileAttributesA(dumpDir) == INVALID_FILE_ATTRIBUTES) CreateDirectoryA(dumpDir, NULL);
			D3DXSaveSurfaceToFileA(".\\Test\\shadowmapatlas.jpg", D3DXIFF_JPG, fx->ShadowAtlasSurface, NULL, NULL);
			D3DXSaveSurfaceToFileA(".\\Test\\shadowmaportho.jpg", D3DXIFF_JPG, fx->ShadowMapOrthoSurface, NULL, NULL);
			if (fx->CharacterMapSurface) D3DXSaveSurfaceToFileA(".\\Test\\charshadowmap.jpg", D3DXIFF_JPG, fx->CharacterMapSurface, NULL, NULL);	// UNOFFICIAL 2026-10-24 CharacterShadowMap

			InterfaceManager->ShowMessage("Textures taken!");
		}
	}

	dev->EndScene();

	FrameCounter = (FrameCounter + 1) % 8;
	shadowMapsRenderTime = frameTimer.LogTime("ShadowManager::RenderShadowMaps");
}


// UNOFFICIAL 2026-10-23 CharacterShadowMap (docs/next/CHARACTER-SHADOW-MAP-DESIGN.md). A small sun shadow map (R32F, plain depth
// whatever the cascades' Mode: TESR_ShadowData.z = 1 makes ShadowMap.pso write depth, as for the ortho map) fitted round the player
// (ShadowsExteriorEffect::GetCharacterViewProj), drawn every frame the cascades are, with the cascade passes' shaders and states
// (BeginShadowMap / DrawShadowMap) but its own walk: the player's node alone (and, with CharacterShadowMapOthers, the other actors of
// the loaded cells whose bound touches the box), not the loaded cells. The lookups (Includes/Shadow.hlsl CharacterMapShadow,
// SunShadows.fx.hlsl) take the darker of the cascades' shadow and this map's where a pixel is inside the box, fading to the cascades
// alone over the box's outer band; so the cascades keep drawing the player as before (the player's shadow on the ground, and the
// shadows of walls and trees on the player, stay theirs), and this map only adds the fine self-shadow the cascades' texels lose.
// Off, or not compiled in, or in first person: nothing here, CharacterData.w stays 0, every shader skips the lookup.
void ShadowManager::RenderCharacterShadowMap(const D3DXVECTOR3& SunDir) {
	ShadowsExteriorEffect* Shadows = TheShaderManager->Effects.ShadowsExteriors;
	if (!Shadows->Settings.ShadowMaps.CharacterMap || !Shadows->CharacterMapCompiled) return;
	if (!Shadows->EnsureCharacterMap()) return;
	D3DXMATRIX viewProj;
	if (!Shadows->GetCharacterViewProj(SunDir, viewProj)) return;

	IDirect3DDevice9* Device = TheRenderManager->device;
	static GpuTimer characterMapTimer("Character shadow map");
	static CpuTimer characterMapCpuTimer("Character shadow map (CPU)");
	CpuProfileScope cpu(characterMapCpuTimer);
	GpuProfileScope gpu(characterMapTimer, Device);

	ShadowsExteriorEffect::ShadowMapSettings* map = &Shadows->CharacterMap;
	D3DXVECTOR4* ShadowData = &Shadows->Constants.Data;
	const float zWas = ShadowData->z;
	ShadowData->z = 1.0f;	// plain depth (as the ortho map), whatever the cascades' Mode
	Shadows->Constants.ShadowViewProj = viewProj;
	Device->SetRenderTarget(0, Shadows->CharacterMapSurface);
	Device->SetDepthStencilSurface(Shadows->CharacterMapDepthSurface);

	BeginShadowMap(map, &viewProj);
	// The walk: the player's node (the body, hair, headgear, the held weapon: all under it), culled against the box's planes.
	// (UNOFFICIAL 2026-10-31 CharacterShadowMapTarget: or the NPC it was fitted round in first person)
	NiNode* playerNode = Shadows->CharacterTargetNode ? Shadows->CharacterTargetNode : Player->GetNode();
	AccumChildren(playerNode, &map->Forms, false, false, &map->ShadowMapFrustumPlanes);
	unsigned others = 0;
	if (Shadows->Settings.ShadowMaps.CharacterMapOthers && Player->GetWorldSpace() && Tes && Tes->gridCellArray) {
		GridCellArray* CellArray = Tes->gridCellArray;
		const UInt32 CellArraySize = CellArray->size * CellArray->size;
		for (UInt32 c = 0; c < CellArraySize; c++) {
			TESObjectCELL* Cell = CellArray->GetCell(c);
			if (!Cell || Cell->IsInterior()) continue;
			for (TList<TESObjectREFR>::Entry* Entry = &Cell->objectList.First; Entry; Entry = Entry->next) {
				TESObjectREFR* Ref = Entry->item;
				if (!Ref || !Ref->baseForm || Ref == (TESObjectREFR*)Player) continue;
				const UInt8 type = Ref->baseForm->formType;
				if (type != TESForm::FormType::kFormType_NPC && type != TESForm::FormType::kFormType_Creature && type != TESForm::FormType::kFormType_LeveledCreature) continue;
				NiNode* node = Ref->GetNode();
				if (!node || node == playerNode || (node->m_flags & NiAVObject::APP_CULLED) || !node->WithinFrustum(&map->ShadowMapFrustumPlanes)) continue;
				AccumChildren(node, &map->Forms, false, false, &map->ShadowMapFrustumPlanes);
				others++;
			}
		}
	}
	const unsigned casters = (unsigned)(geometryPass->GeometryList.size() + alphaPass->GeometryList.size() + skinnedGeoPass->GeometryList.size());
	DrawShadowMap(map);
	ShadowData->z = zWas;
	Shadows->Constants.CharacterData.w = 1.0f + (float)Shadows->Settings.ShadowMaps.CharacterMapDebug;	// the map holds this frame's draw: the lookups read it (2-4: a debug view, Shadow.hlsl)

	if (Shadows->Settings.ShadowMaps.CharacterMapLog) {
		static DWORD lastLog = 0;
		if (GetTickCount() - lastLog > 1000) {
			lastLog = GetTickCount();
			const float radius = Shadows->Constants.CharacterCenter.w;
			Logger::Log("CharacterShadowMap: fit %d, half-extent %.0f units (texel %.3f units = %.2f cm), %u casters (%u other actors), sun %.3f %.3f %.3f, bias %.5f, band %.3f",
				Shadows->Settings.ShadowMaps.CharacterMapFit, radius, 2.0f * radius / map->ShadowMapResolution, 2.0f * radius / map->ShadowMapResolution * 1.428f,
				casters, others, SunDir.x, SunDir.y, SunDir.z, Shadows->Constants.CharacterData.z, Shadows->Constants.CharacterData.y);
		}
	}
}

// UNOFFICIAL 2026-11-04 ViewModelShadows (from Physically-Based-Reloaded f8b6114a): the first-person arms and weapon casting their sun
// shadows on themselves. The sun cascades never hold the viewmodel (its own scene graph, posed for the frame only as its render starts),
// and in first person the character map has nothing to do (or, with CharacterShadowMapTarget, an NPC's draws are done with it by then).
// So just before the game's first-person render (Render.cpp RenderFirstPersonHook) the viewmodel alone is drawn into the character map
// with its own light box, an ortho box fitted round its parts in the sun's frame, and the first-person draws read it through
// CharacterMapShadow with TESR_CharShadowData.w = 0.5 (a 6 x 6 tent PCF there, Includes/Shadow.hlsl); the caller uploads the constants
// and puts them back after that render. The game draws the viewmodel moved to the origin (0x874C10) and camera-relative to the camera
// drawing it (NiRenderer::kPosAdjust, set by Do_SetCameraData), here still the world camera's: for this draw both kPosAdjust and NVR's
// camera position are the first-person camera's, so the map holds what the first-person draw sees and the bone matrices computed now
// are the ones that draw uses (made with the world camera's, the arms were drawn far off). Called inside the game's scene: the targets,
// viewport, render states, shaders, buffers, textures and shader constants the draws change are put back after. False when nothing was
// drawn (the Character* constants are then unchanged).

// The viewmodel's parts as spheres relative to Eye, for the light box: skinned meshes by the bound NiSkinData keeps for each bone's
// vertices (in that bone's space), rigid ones by their model bound. Walked as AccumChildren walks it; anything far from the camera (the
// first-person skeleton's lower body) is left out.
// UNOFFICIAL 2026-11-04 CharacterShadowMapLog: this frame's viewmodel map as drawn, for ViewModelLogDraw (the first-person draws read it).
static struct {
	bool valid = false;
	UInt32 frame = 0;
	NiPoint3 eye = {};
	D3DXMATRIX lightView;
	float cx = 0.0f, cy = 0.0f, half = 0.0f, zNear = 0.0f, zFar = 0.0f;
	std::unordered_map<const void*, NiPoint3> posAtMap;	// each geometry's world position when the map was drawn
	int lines = 0;
} ViewModelLog;

static void ViewModelLogPositions(NiAVObject* Object) {
	if (!Object) return;
	if (Object->IsGeometry()) { ViewModelLog.posAtMap[Object] = Object->m_worldTransform.pos; return; }
	if (NiNode* Node = Object->IsNiNode()) for (int i = 0; i < Node->m_children.end; i++) ViewModelLogPositions(Node->m_children.data[i]);
}

static void ViewModelSpheres(NiAVObject* Object, const NiPoint3& Eye, std::vector<D3DXVECTOR4>& Spheres);

// Render.cpp ForwardShadowsForPass, every first-person pass: how far the part drawn sits outside the map's box (in the sun's frame, world
// units; 0 inside), whether it moved since the map was drawn, and the character-map switch the shaders read (w: 0.5 the viewmodel map).
void ShadowManager::ViewModelLogDraw(NiGeometry* Geometry) {
	ShadowsExteriorEffect* Shadows = TheShaderManager->Effects.ShadowsExteriors;
	if (!Geometry || !Shadows || !Shadows->Settings.ShadowMaps.CharacterMapLog || ViewModelLog.lines >= 4000) return;
	const UInt32 frame = TheFrameRateManager ? TheFrameRateManager->FrameNumber : 0u;
	// Only the draws that could lose the shadow are written, and a summary every 120 frames (all draws would fill the log in two seconds).
	static UInt32 summaryFrame = 0, draws = 0, flagged = 0;
	draws++;
	if (frame >= summaryFrame + 120) {
		ViewModelLog.lines++;
		Logger::Log("VIEWMODEL DRAW f%u: summary: %u first-person draws since f%u, %u flagged", frame, draws, summaryFrame, flagged);
		summaryFrame = frame; draws = 0; flagged = 0;
	}
	if (!ViewModelLog.valid || ViewModelLog.frame != frame) {
		flagged++; ViewModelLog.lines++;
		Logger::Log("VIEWMODEL DRAW f%u: '%s' with no map this frame (last map f%u) | shaders read w %.2f", frame, Geometry->m_pcName ? Geometry->m_pcName : "?",
			ViewModelLog.frame, Shadows->Constants.CharacterData.w);
		return;
	}
	static std::vector<D3DXVECTOR4> spheres;
	spheres.clear();
	ViewModelSpheres(Geometry, ViewModelLog.eye, spheres);
	float over = 0.0f;
	for (const D3DXVECTOR4& s : spheres) {
		D3DXVECTOR3 v;
		D3DXVec3TransformCoord(&v, (const D3DXVECTOR3*)&s, &ViewModelLog.lightView);
		over = max(over, fabsf(v.x - ViewModelLog.cx) + s.w - ViewModelLog.half);
		over = max(over, fabsf(v.y - ViewModelLog.cy) + s.w - ViewModelLog.half);
		over = max(over, max(-v.z + s.w - ViewModelLog.zFar, ViewModelLog.zNear - (-v.z - s.w)));
	}
	auto at = ViewModelLog.posAtMap.find(Geometry);
	const NiPoint3& p = Geometry->m_worldTransform.pos;
	const float moved = at == ViewModelLog.posAtMap.end() ? -1.0f :
		sqrtf((p.x - at->second.x) * (p.x - at->second.x) + (p.y - at->second.y) * (p.y - at->second.y) + (p.z - at->second.z) * (p.z - at->second.z));
	const float w = Shadows->Constants.CharacterData.w;
	if (over <= 0.1f && moved >= 0.0f && moved <= 0.05f && w > 0.49f && w < 0.51f) return;	// (0.1: a texel's snap of the box centre)
	flagged++; ViewModelLog.lines++;
	Logger::Log("VIEWMODEL DRAW f%u: '%s' %u spheres | outside the box by %.2f | moved %.2f since the map (-1: not in it) | shaders read w %.2f",
		frame, Geometry->m_pcName ? Geometry->m_pcName : "?", (unsigned)spheres.size(), max(over, 0.0f), moved, w);
}

static void ViewModelSpheres(NiAVObject* Object, const NiPoint3& Eye, std::vector<D3DXVECTOR4>& Spheres) {
	if (!Object || Object->m_flags & NiAVObject::NiFlags::APP_CULLED) return;
	auto Add = [&](const NiTransform& T, const NiBound& B) {
		const NiPoint3 c = T.rot * B.Center;
		const D3DXVECTOR4 s(T.pos.x + c.x * T.scale - Eye.x, T.pos.y + c.y * T.scale - Eye.y, T.pos.z + c.z * T.scale - Eye.z, B.Radius * T.scale);
		if (s.x * s.x + s.y * s.y + s.z * s.z < 150.0f * 150.0f) Spheres.push_back(s);
	};
	if (Object->IsGeometry()) {
		NiGeometry* Geo = static_cast<NiGeometry*>(Object);
		NiSkinInstance* Skin = Geo->skinInstance;
		if (Skin && Skin->SkinData && Skin->SkinData->BoneData && Skin->BoneObjects) {
			for (UInt32 i = 0; i < Skin->SkinData->Bones; i++)
				if (Skin->BoneObjects[i] && Skin->SkinData->BoneData[i].Verts) Add(Skin->BoneObjects[i]->m_worldTransform, Skin->SkinData->BoneData[i].Bound);
		}
		else if (Geo->geomData) Add(Geo->m_worldTransform, Geo->geomData->m_kBound);
		return;
	}
	NiNode* Node = Object->IsNiNode();
	if (!Node) return;
	if (Node->IsKindOf<NiSwitchNode>()) {
		NiSwitchNode* SwitchNode = static_cast<NiSwitchNode*>(Node);
		if (SwitchNode->m_iIndex >= 0 && SwitchNode->m_iIndex < Node->m_children.end) ViewModelSpheres(Node->m_children.data[SwitchNode->m_iIndex], Eye, Spheres);
		return;
	}
	for (int i = 0; i < Node->m_children.end; i++) ViewModelSpheres(Node->m_children.data[i], Eye, Spheres);
}

bool ShadowManager::RenderViewModelShadowMap(NiCamera* FirstPersonCamera) {
	ShadowsExteriorEffect* Shadows = TheShaderManager->Effects.ShadowsExteriors;
	if (!Shadows || !ShadowShadersLoaded || !TheSettingManager->SettingsMain.Main.RenderEffects) return false;
	// UNOFFICIAL 2026-11-04 [Shaders.ShadowsExteriors.ShadowMaps] CharacterShadowMapLog: each first-person frame, made or why not, and where
	// (user: the arms' and weapon's shadows flicker in and out on a jump). At most 4000 lines a session.
	static int viewModelLogLines = 0;
	const bool logThis = Shadows->Settings.ShadowMaps.CharacterMapLog && viewModelLogLines < 4000;
	auto skip = [&](const char* why) { if (logThis) { viewModelLogLines++; Logger::Log("VIEWMODEL SHADOW f%u: skipped (%s)", TheFrameRateManager ? TheFrameRateManager->FrameNumber : 0u, why); } return false; };
	if (!Shadows->Settings.ShadowMaps.CharacterMap || !Shadows->CharacterMapCompiled || !Shadows->EnsureCharacterMap()) return skip("character map off or not made");
	if (!TheShaderManager->GameState.isExterior || !Shadows->Enabled || !Shadows->Settings.Exteriors.Enabled) return skip("indoors or exterior shadows off");
	if (Shadows->Constants.ShadowFade.y <= 0.0f || Shadows->Constants.ForwardData.x != 0.0f) return skip(Shadows->Constants.ShadowFade.y <= 0.0f ? "sun shadow maps inactive" : "forward sun shadows off this frame");
	if (!Player || !Player->firstPersonNiNode || !FirstPersonCamera) return skip("no first-person model or camera");
	// UNOFFICIAL 2026-11-05 (from Bale, PR #6): no map while the weapon is holstered, nothing of the viewmodel is drawn for it to shadow (the world's
	// shadows dropped out in first person then).
	if (!Player->process || !Player->process->IsWeaponOut()) return skip("weapon holstered: no viewmodel on screen");

	IDirect3DDevice9* Device = TheRenderManager->device;
	NiDX9RenderState* RenderState = TheRenderManager->renderState;
	static GpuTimer viewModelMapTimer("View model shadow map");
	GpuProfileScope gpu(viewModelMapTimer, Device);
	IDirect3DSurface9* DepthSurface = NULL;
	IDirect3DSurface9* RenderSurface = NULL;
	D3DVIEWPORT9 viewport;
	Device->GetDepthStencilSurface(&DepthSurface);
	Device->GetRenderTarget(0, &RenderSurface);
	Device->GetViewport(&viewport);
	static const D3DRENDERSTATETYPE kStates[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_CULLMODE, D3DRS_ALPHABLENDENABLE,
		D3DRS_ALPHATESTENABLE, D3DRS_ALPHAREF, D3DRS_ALPHAFUNC, D3DRS_DEPTHBIAS, D3DRS_SLOPESCALEDEPTHBIAS, D3DRS_STENCILENABLE, D3DRS_STENCILREF,
		D3DRS_STENCILFUNC, D3DRS_COLORWRITEENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_CLIPPLANEENABLE, D3DRS_NORMALIZENORMALS, D3DRS_POINTSIZE };
	DWORD saved[_countof(kStates)];
	for (int i = 0; i < _countof(kStates); i++) Device->GetRenderState(kStates[i], &saved[i]);
	// The rest of the device state the draws below change, as the flashlight's MaterialPass keeps it (in the middle of the game's own
	// draws): shaders, vertex layout, buffers, textures and their sampling, and every shader constant (the skinned pass writes bones
	// from c9, the game sets some of its constants once per pass).
	static const D3DSAMPLERSTATETYPE kSamplerStates[] = { D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER };
	IDirect3DVertexShader9* savedVS = NULL; IDirect3DPixelShader9* savedPS = NULL; IDirect3DVertexDeclaration9* savedDecl = NULL;
	IDirect3DIndexBuffer9* savedIB = NULL; DWORD savedFVF = 0;
	IDirect3DVertexBuffer9* savedVB[8] = {}; UINT savedOffset[8] = {}, savedStride[8] = {};
	IDirect3DBaseTexture9* savedTex[16] = {}; DWORD savedSampler[16][_countof(kSamplerStates)];
	static float savedVSConst[256][4], savedPSConst[224][4];
	Device->GetVertexShader(&savedVS); Device->GetPixelShader(&savedPS); Device->GetVertexDeclaration(&savedDecl);
	Device->GetIndices(&savedIB); Device->GetFVF(&savedFVF);
	for (UINT i = 0; i < 8; i++) Device->GetStreamSource(i, &savedVB[i], &savedOffset[i], &savedStride[i]);
	for (DWORD i = 0; i < 16; i++) {
		Device->GetTexture(i, &savedTex[i]);
		for (int s = 0; s < _countof(kSamplerStates); s++) Device->GetSamplerState(i, kSamplerStates[s], &savedSampler[i][s]);
	}
	Device->GetVertexShaderConstantF(0, &savedVSConst[0][0], 256);
	Device->GetPixelShaderConstantF(0, &savedPSConst[0][0], 224);
	const D3DXVECTOR4 savedData = Shadows->Constants.Data;
	const D3DXMATRIX savedViewProj = Shadows->Constants.ShadowViewProj;
	ShadowsExteriorEffect::ShadowMapSettings& map = Shadows->CharacterMap;
	const float res = map.ShadowMapResolution;

	// Camera-relative to the first-person camera (see above), put back at the end.
	NiPoint3* PosAdjust = (NiPoint3*)0x011F474C;	// NiRenderer::kPosAdjust
	const NiPoint3 savedPosAdjust = *PosAdjust;
	const D3DXVECTOR4 savedCameraPosition = TheRenderManager->CameraPosition;
	const NiPoint3 eye = FirstPersonCamera->m_worldTransform.pos;
	*PosAdjust = eye;
	TheRenderManager->CameraPosition = D3DXVECTOR4(eye.x, eye.y, eye.z, 1.0f);

	// The light transform: an ortho box fitted, in the sun's frame, round the parts drawn (ViewModelSpheres; the viewmodel's root bound
	// has no size), so the map's texels cover only the arms and weapon. Square, its size in 8-unit steps and its centre snapped to whole
	// texels: the texel grid holds still while the weapon sways and the shadow edges do not crawl. At least as deep as it is wide
	// (CharacterMapShadow's receiver-plane slack takes one texel of depth as one texel across).
	static std::vector<D3DXVECTOR4> spheres;
	spheres.clear();
	ViewModelSpheres(Player->firstPersonNiNode, eye, spheres);
	D3DXVECTOR3 sunDir(Shadows->Constants.SmoothedSunDir.x, Shadows->Constants.SmoothedSunDir.y, Shadows->Constants.SmoothedSunDir.z);
	D3DXVec3Normalize(&sunDir, &sunDir);
	const D3DXVECTOR3 origin(0.0f, 0.0f, 0.0f);
	const D3DXVECTOR3 upDir = fabsf(sunDir.z) > 0.99f ? D3DXVECTOR3(0.0f, 1.0f, 0.0f) : D3DXVECTOR3(0.0f, 0.0f, 1.0f);
	D3DXMATRIX lightView, lightProj;
	D3DXMatrixLookAtRH(&lightView, &sunDir, &origin, &upDir);
	D3DXVECTOR3 lo(FLT_MAX, FLT_MAX, FLT_MAX), hi(-FLT_MAX, -FLT_MAX, -FLT_MAX);
	for (const D3DXVECTOR4& s : spheres) {
		D3DXVECTOR3 v;
		D3DXVec3TransformCoord(&v, (const D3DXVECTOR3*)&s, &lightView);
		lo.x = min(lo.x, v.x - s.w); lo.y = min(lo.y, v.y - s.w); lo.z = min(lo.z, v.z - s.w);
		hi.x = max(hi.x, v.x + s.w); hi.y = max(hi.y, v.y + s.w); hi.z = max(hi.z, v.z + s.w);
	}
	if (spheres.empty()) { lo = D3DXVECTOR3(-1.0f, -1.0f, -1.0f); hi = D3DXVECTOR3(1.0f, 1.0f, 1.0f); }
	const float extent = max(8.0f, ceilf(max(hi.x - lo.x, hi.y - lo.y) / 8.0f) * 8.0f);
	const float texelWorld = extent / res;
	const float cx = floorf((lo.x + hi.x) * 0.5f / texelWorld) * texelWorld;
	const float cy = floorf((lo.y + hi.y) * 0.5f / texelWorld) * texelWorld;
	float zNear = -hi.z - 1.0f, zFar = -lo.z + 1.0f;	// right-handed: in front is -z
	if (zFar - zNear < extent) { const float grow = 0.5f * (extent - (zFar - zNear)); zNear -= grow; zFar += grow; }
	D3DXMatrixOrthoOffCenterRH(&lightProj, cx - 0.5f * extent, cx + 0.5f * extent, cy - 0.5f * extent, cy + 0.5f * extent, zNear, zFar);
	D3DXMATRIX viewProj = lightView * lightProj;

	// The lookup's space is not the one drawn here: the object and skin shaders rebuild their position from the clip position with NVR's
	// inverse view and projection (Includes/Shadow.hlsl GetShadowWorldPos), made from the world camera when the world scene began
	// (RenderManager::SetupSceneCamera), and the viewmodel is projected with the first-person camera's own field of view (by the time of
	// this call the world camera's frustum already reads the first-person one, so the inverse projection is the only true record of the
	// world's). Along each view ray at the same view depth, the rebuilt point is the true one with its view x and y scaled by
	// InvProj._11 * 2 / first-person width (and _22 for y): 1.092 measured at the default FOVs, enough to put every lookup behind the
	// viewmodel's own surfaces in a box texels of 0.06 units. So the lookup undoes it before the light transform: the inverse view's
	// translation off, NVR's view rotation, that scale inverted, the first-person camera's inverse view rotation.
	auto viewRotation = [](NiCamera* camera) {
		const NiMatrix33& axes = camera->m_worldTransform.rot;
		D3DXMATRIX m;
		D3DXMatrixIdentity(&m);
		for (int r = 0; r < 3; r++) {	// columns side, up, forward (RenderManager::SetupSceneCamera)
			m.m[r][0] = axes.data[r][2];
			m.m[r][1] = axes.data[r][1];
			m.m[r][2] = axes.data[r][0];
		}
		return m;
	};
	const D3DXMATRIX& invView = TheRenderManager->InvViewMatrix;
	const D3DXMATRIX& invProj = TheRenderManager->InvProjMatrix;
	const NiFrustum& f = FirstPersonCamera->Frustum;
	D3DXMATRIX untranslate, nvrView, scale, firstPersonView, firstPersonInverse;
	D3DXMatrixTranslation(&untranslate, -invView._41, -invView._42, -invView._43);
	D3DXMatrixTranspose(&nvrView, &invView);	// the rotation's inverse (the translation row is dropped below)
	nvrView._14 = nvrView._24 = nvrView._34 = 0.0f;
	nvrView._41 = nvrView._42 = nvrView._43 = 0.0f;
	const float scaleX = invProj._11 * 2.0f / max(f.Right - f.Left, 1e-6f), scaleY = invProj._22 * 2.0f / max(f.Top - f.Bottom, 1e-6f);
	D3DXMatrixScaling(&scale, 1.0f / scaleX, 1.0f / scaleY, 1.0f);
	firstPersonView = viewRotation(FirstPersonCamera);
	D3DXMatrixTranspose(&firstPersonInverse, &firstPersonView);
	const D3DXMATRIX rebuiltToTrue = untranslate * nvrView * scale * firstPersonInverse;

	// The lookup's constants (CharacterMapShadow): w 0.5 = this map, read with the tent; a band of 0.01 (the box is fitted round the
	// parts, no hand-over at its edge); a depth bias of two texels' worth.
	Shadows->Constants.CharacterCameraToLight = rebuiltToTrue * viewProj;
	Shadows->Constants.CharacterCenter = D3DXVECTOR4(cx, cy, 0.0f, 0.5f * extent);
	Shadows->Constants.CharacterData = D3DXVECTOR4(1.0f / res, 0.01f, max(2.0f * texelWorld, 0.03f) / (zFar - zNear), 
		Shadows->Settings.ShadowMaps.CharacterMapDebug ? 1.0f + (float)Shadows->Settings.ShadowMaps.CharacterMapDebug : 0.5f);	// (CharacterShadowMapDebug: its views, Shadow.hlsl)

	Shadows->Constants.Data.z = 1.0f;	// plain depth (as RenderCharacterShadowMap)
	Shadows->Constants.ShadowViewProj = viewProj;
	if (Shadows->Settings.ShadowMaps.CharacterMapLog) {	// for ViewModelLogDraw
		ViewModelLog.valid = true;
		ViewModelLog.frame = TheFrameRateManager ? TheFrameRateManager->FrameNumber : 0u;
		ViewModelLog.eye = eye;
		ViewModelLog.lightView = lightView;
		ViewModelLog.cx = cx; ViewModelLog.cy = cy; ViewModelLog.half = 0.5f * extent;
		ViewModelLog.zNear = zNear; ViewModelLog.zFar = zFar;
		ViewModelLog.posAtMap.clear();
		ViewModelLogPositions(Player->firstPersonNiNode);
	}
	if (logThis) {
		viewModelLogLines++;
		Logger::Log("VIEWMODEL SHADOW f%u: made | eye %.1f %.1f %.1f | world camera %.1f %.1f %.1f (eye - world %.2f %.2f %.2f) | inv view t %.2f %.2f %.2f | "
			"%u parts, box centre %.2f %.2f, extent %.1f, depth %.1f..%.1f | sun %.3f %.3f %.3f",
			TheFrameRateManager ? TheFrameRateManager->FrameNumber : 0u, eye.x, eye.y, eye.z, savedCameraPosition.x, savedCameraPosition.y, savedCameraPosition.z,
			eye.x - savedCameraPosition.x, eye.y - savedCameraPosition.y, eye.z - savedCameraPosition.z, invView._41, invView._42, invView._43,
			(unsigned)spheres.size(), cx, cy, extent, zNear, zFar, sunDir.x, sunDir.y, sunDir.z);
	}
	geometryPass->VertexShader = alphaPass->VertexShader = skinnedGeoPass->VertexShader = ShadowMapVertex;
	geometryPass->PixelShader = ShadowMapOpaquePixel;
	alphaPass->PixelShader = ShadowMapPixel;
	skinnedGeoPass->PixelShader = SkinnedGeoShadowRenderPass::AlphaTest ? ShadowMapPixel : ShadowMapOpaquePixel;

	// Any other targets the game's scene left bound (multiple render targets) would not match the map's size, which drops every draw;
	// put back at the end.
	IDirect3DSurface9* savedRT[3] = {};
	for (DWORD i = 1; i < 4; i++) { Device->GetRenderTarget(i, &savedRT[i - 1]); if (savedRT[i - 1]) Device->SetRenderTarget(i, NULL); }
	Device->SetRenderTarget(0, Shadows->CharacterMapSurface);
	Device->SetDepthStencilSurface(Shadows->CharacterMapDepthSurface);
	// The states RenderShadowMaps draws every map with, on the device as well: the first-person scene arrives with colour writes off,
	// and NiDX9RenderState may believe it already holds a value the device does not. The depth test is LESSEQUAL whatever the scene's
	// depth convention: ShadowMap.vso writes plain depth (0 at the light) and DrawShadowMap clears to 1, so nearest wins (with the
	// reversed scene's GREATEREQUAL set on the device too, every draw failed here: an empty map).
	const DWORD zFunc = D3DCMP_LESSEQUAL;
	const struct { D3DRENDERSTATETYPE type; DWORD value; } kMapStates[] = {
		{ D3DRS_ZFUNC, zFunc }, { D3DRS_STENCILENABLE, 1 }, { D3DRS_STENCILREF, 0 }, { D3DRS_STENCILFUNC, D3DCMP_ALWAYS }, { D3DRS_NORMALIZENORMALS, 1 },
		{ D3DRS_POINTSIZE, 810365505 }, { D3DRS_COLORWRITEENABLE, 0xF }, { D3DRS_SCISSORTESTENABLE, FALSE }, { D3DRS_CLIPPLANEENABLE, 0 },
		{ D3DRS_ZENABLE, D3DZB_TRUE }, { D3DRS_ZWRITEENABLE, TRUE } };
	for (const auto& entry : kMapStates) {
		RenderState->SetRenderState(entry.type, entry.value, RenderStateArgs);
		Device->SetRenderState(entry.type, entry.value);
	}

	// Every part of the viewmodel, however small (fingers and sights), alpha tested parts included.
	map.Forms = {};
	map.Forms.Actors = true;
	map.Forms.AlphaEnabled = true;
	// The Pip-Boy fit's object is the game's last first-person draw: last frame's here (this map comes before this frame's draws), maybe gone
	// since (a weapon switch, the Pip-Boy): ClothWind's partition draws read it (crashed in PipboyFit::ForPartition). Off for this map.
	PipboyFit::Active = nullptr;
	map.CustomClearRequired = false;
	BeginShadowMap(&map, &viewProj);
	AccumChildren(Player->firstPersonNiNode, &map.Forms, false, false, nullptr);
	DrawShadowMap(&map);

	for (int i = 0; i < _countof(kStates); i++) {
		RenderState->SetRenderState(kStates[i], saved[i], RenderStateArgs);
		Device->SetRenderState(kStates[i], saved[i]);	// (the cache may already hold it and skip the device)
	}
	*PosAdjust = savedPosAdjust;
	TheRenderManager->CameraPosition = savedCameraPosition;
	Shadows->Constants.Data = savedData;
	Shadows->Constants.ShadowViewProj = savedViewProj;
	Device->SetDepthStencilSurface(DepthSurface);
	Device->SetRenderTarget(0, RenderSurface);
	for (DWORD i = 1; i < 4; i++) if (savedRT[i - 1]) { Device->SetRenderTarget(i, savedRT[i - 1]); savedRT[i - 1]->Release(); }
	Device->SetViewport(&viewport);
	if (DepthSurface) DepthSurface->Release();
	if (RenderSurface) RenderSurface->Release();
	// (through NiDX9RenderState where it keeps its own idea of the state, so it and the device agree again)
	RenderState->SetVertexShader(savedVS, false);
	RenderState->SetPixelShader(savedPS, false);
	if (savedDecl) RenderState->SetVertexDeclaration(savedDecl, false);
	else RenderState->SetFVF(savedFVF, false);
	Device->SetIndices(savedIB);
	for (UINT i = 0; i < 8; i++) Device->SetStreamSource(i, savedVB[i], savedOffset[i], savedStride[i]);
	for (DWORD i = 0; i < 16; i++) {
		RenderState->SetTexture(i, savedTex[i]);
		Device->SetTexture(i, savedTex[i]);	// (the cache may already hold it and skip the device)
		for (int s = 0; s < _countof(kSamplerStates); s++) {
			TheRenderManager->SetSamplerState(i, kSamplerStates[s], savedSampler[i][s]);
			Device->SetSamplerState(i, kSamplerStates[s], savedSampler[i][s]);
		}
	}
	Device->SetVertexShaderConstantF(0, &savedVSConst[0][0], 256);
	Device->SetPixelShaderConstantF(0, &savedPSConst[0][0], 224);
	if (savedVS) savedVS->Release();
	if (savedPS) savedPS->Release();
	if (savedDecl) savedDecl->Release();
	if (savedIB) savedIB->Release();
	for (UINT i = 0; i < 8; i++) if (savedVB[i]) savedVB[i]->Release();
	for (DWORD i = 0; i < 16; i++) if (savedTex[i]) savedTex[i]->Release();
	return true;
}

/*
 * Clear a part of the shadow map atlas based on the shadow mapping mode.
 * 
 * Note: Render target, view port should be set beforehand. Scene has to be already being rendered.
 */
void ShadowManager::ClearShadowCascade(D3DVIEWPORT9* ViewPort, D3DXVECTOR4* ClearColor) {
	ShadowsExteriorEffect* shadows = TheShaderManager->Effects.ShadowsExteriors;
	IDirect3DDevice9* dev = TheRenderManager->device;
	NiDX9RenderState* state = TheRenderManager->renderState;

	dev->SetDepthStencilSurface(NULL);
	state->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE, RenderStateArgs);
	state->SetRenderState(D3DRS_ZWRITEENABLE, D3DZB_FALSE, RenderStateArgs);
	state->SetPixelShader(ShadowMapClearPixel->ShaderHandle, false);

	dev->SetPixelShaderConstantF(0, (const float*)ClearColor, 1);

	// A full-screen quad covering the viewport, half a texel in from its edges.
	struct QuadVertex { float x, y, z, rhw; };
	const float left = (float)ViewPort->X - 0.5f;
	const float top = (float)ViewPort->Y - 0.5f;
	const float right = (float)(ViewPort->X + ViewPort->Width) - 0.5f;
	const float bottom = (float)(ViewPort->Y + ViewPort->Height) - 0.5f;
	QuadVertex quad[] = {
		{ left, top, 0.5f, 1.0f },
		{ right, top, 0.5f, 1.0f },
		{ left, bottom, 0.5f, 1.0f },
		{ right, bottom, 0.5f, 1.0f }
	};

	state->SetFVF(D3DFVF_XYZRHW, false);
	dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(QuadVertex));

	dev->SetDepthStencilSurface(shadows->ShadowAtlasDepthSurface);
	state->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE, RenderStateArgs);
	state->SetRenderState(D3DRS_ZWRITEENABLE, D3DZB_TRUE, RenderStateArgs);
}


/*
* Filters the Shadow Map of given index using a 2 pass gaussian blur
*/
void ShadowManager::BlurShadowAtlas(unsigned cascadeMask) {
	ShadowsExteriorEffect* shadows = TheShaderManager->Effects.ShadowsExteriors;
	IDirect3DDevice9* dev = TheRenderManager->device;
	NiDX9RenderState* state = TheRenderManager->renderState;

	// Ping-pong: horizontal pass atlas -> scratch, vertical pass scratch -> atlas.
	//
	// Neither pass may sample the atlas while ShadowAtlasSurface -- level 0 of that same
	// texture -- is the bound render target. Reading a bound render target is undefined in
	// D3D9 and a read/write feedback loop on Vulkan under DXVK. This Gaussian is the only
	// filtering the shadow maps get (the cascade lookup is a single tap), so anything that
	// compromises it shows up directly as hard, unfiltered texels along every shadow edge.
	if (!cascadeMask || !shadows->ShadowAtlasBlurTexture || !shadows->ShadowAtlasBlurSurface) return;

	DWORD oldScissorEnabled = FALSE;
	RECT oldScissor = {};
	if (FAILED(dev->GetRenderState(D3DRS_SCISSORTESTENABLE, &oldScissorEnabled)) ||
		FAILED(dev->GetScissorRect(&oldScissor))) return;

	dev->SetDepthStencilSurface(NULL);
	state->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE, RenderStateArgs);
	state->SetRenderState(D3DRS_ZWRITEENABLE, D3DZB_FALSE, RenderStateArgs);
	state->SetVertexShader(ShadowMapBlurVertex->ShaderHandle, false);
	state->SetPixelShader(ShadowMapBlurPixel->ShaderHandle, false);
	state->SetFVF(FrameFVF, false);
	dev->SetStreamSource(0, shadows->ShadowAtlasVertexBuffer, 0, sizeof(FrameVS));

	// Pass map resolution to shader as a constant
	ShadowMapBlurPixel->SetShaderConstantF(0, &shadows->Constants.ShadowBlur, 1);

	// blur in two passes, horizontally then vertically
	D3DXVECTOR4 blurDir[2] = {
		D3DXVECTOR4(1.0f, 0.0f, 0.0f, 0.0f),
		D3DXVECTOR4(0.0f, 1.0f, 0.0f, 0.0f),
	};
	IDirect3DTexture9* source[2] = { shadows->ShadowAtlasTexture, shadows->ShadowAtlasBlurTexture };
	IDirect3DSurface9* target[2] = { shadows->ShadowAtlasBlurSurface, shadows->ShadowAtlasSurface };

	for (int pass = 0; pass < 2; pass++) {
		// Unbind the previous pass's target before it becomes this pass's source, so the
		// two are never bound as texture and render target at the same time.
		state->SetTexture(0, nullptr);
		dev->SetRenderTarget(0, target[pass]);
		state->SetTexture(0, source[pass]);

		// set blur direction shader constants
		ShadowMapBlurPixel->SetShaderConstantF(1, &blurDir[pass], 1);

		state->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE, RenderStateArgs);
		for (int cascade = MapNear; cascade < MapOrtho; ++cascade) {
			if (!(cascadeMask & (1u << cascade))) continue;
			const D3DVIEWPORT9& viewport = shadows->ShadowMaps[cascade].ShadowMapViewPort;
			RECT region = {(LONG)viewport.X, (LONG)viewport.Y,
				(LONG)(viewport.X + viewport.Width), (LONG)(viewport.Y + viewport.Height)};
			dev->SetScissorRect(&region);
			dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
		}
	}

	state->SetTexture(0, nullptr);
	dev->SetScissorRect(&oldScissor);
	state->SetRenderState(D3DRS_SCISSORTESTENABLE, oldScissorEnabled, RenderStateArgs);
	state->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE, RenderStateArgs);
	state->SetRenderState(D3DRS_ZWRITEENABLE, D3DZB_TRUE, RenderStateArgs);
}
