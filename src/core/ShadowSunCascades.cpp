// Sun shadow cascade walk, mover tracking, and stats (extracted from ShadowManager.cpp).
#include "SkinBones.h"	// 2026-11-18: a skin's bone slots may be stale
#include "ClothWind.h"	// UNOFFICIAL 2026-11-21: sync c248 tracking across ViewModelShadowMap constant restore
#include "SunFadeDeferred.h"	// UNOFFICIAL 2026-11-21: static/dynamic split during DeferredFade
#include "ShadowFaceCull.h"
#include "PortalVisibility.h"	// UNOFFICIAL 2026-11-24: skip occluded-room refs in interior reflections and spot/fallback shadows
#include "../effects/Puddles/PuddleMap.h"	// 2026-11-12 Puddles map mode
#include "PointShadowSchedule.h"
#include "PointShadowForward.h"
#include "BeatSeries.h"	// UNOFFICIAL 2026-11-11 the beat detector's per-frame series
#include "TestTools.h"	// UNOFFICIAL: NVR_TEST_TOOLS (SkipWhenFadedCheck)
#include "OrthoProbe.h"	// UNOFFICIAL 2026-10-17 test tools: ortho map caster dump
#include "SunShadowStats.h"	// UNOFFICIAL 2026-10-21 test tools: where the sun cascades' CPU time goes
#include "../effects/CoverMap/CoverMap.h"	// UNOFFICIAL 2026-10-27 Persistent wetness and snow: CoverMapOrthoFrame
#include "DisplaceShadows.h"	// UNOFFICIAL 2026-10-27 displacement shadows: the land drawn with its bumps into the sun cascades
#include "ShadowAtlas.h"	// UNOFFICIAL 2026-10-30 ShadowAtlas (INTERIOR-SHADOWS-MODERN step 1)
#include "DeferredLamps.h"	// UNOFFICIAL 2026-11-23 deferred exterior night lamp shadows via ShadowAtlas
#include "CapsuleShadows.h"	// UNOFFICIAL 2026-10-31 CapsuleShadows (steps 2-3): DropActors
#include "PipboyFit.h"	// UNOFFICIAL 2026-11-04 ViewModelShadows: its object cleared before the viewmodel map
#include "NpcCharacterMaps.h"	// UNOFFICIAL 2026-11-08 CharacterShadowMapNpcs
#include "ViewModelLight.h"	// UNOFFICIAL 2026-11-15 DeferredFirstPerson: the view model map's light transform for the first-person deferred sun pass
#include "GpuProfiler.h"
#include "ShadowManagerInternal.h"
using namespace ShadowManagerInternal;

// UNOFFICIAL 2026-11-04 (from Physically-Based-Reloaded ShadowManager::CastsIntoView, per cascade of the one walk) caster culling: the
// bound swept the way the light travels (a capsule) must reach what the cascade shades, the view inside the cascade's sphere, or
// nothing it shadows is ever seen. The sweep runs as far as a shadow falls from the bound's top to CasterDrop below its bottom. A
// capsule wholly behind one of the view's sides, or wholly outside the cascade sphere, is out. Conservative. Covered casters
// (SkipCoveredCasters): Shadow.hlsl takes a pixel from the first cascade whose sphere holds it and cross-fades into the next over
// the band (ShadowCascadeBands); a capsule wholly inside an earlier cascade's sphere less its band (less the margin) only shades
// pixels that cascade answers for, so it is left out of this one. Only the cascades RenderShadowMaps marks (drawn fresh every
// frame: a kept cascade would show what was left out as the camera turns). Returns the cascades of mask the caster still goes to.
unsigned ShadowManager::SunCastMask(const NiBound* Bound, unsigned mask) {
	if (!(mask & SunCastAny) || !Bound || Bound->Radius <= 0.0f) return mask;
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
	WalkSunDynamic = false;
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
		WalkSunDynamic = entry->item && entry->item->baseForm ? RefMovesByKind(entry->item, refNode, false) : false;
		WalkSunIsTree = entry->item && entry->item->baseForm && entry->item->baseForm->formType == TESForm::FormType::kFormType_Tree;
		WalkSunChildren(refNode, inMask, false, false);
		WalkSunIsTree = false;
		WalkSunDynamic = false;
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
			if (!isLOD) cm = SunCastMask(child->GetWorldBound(), cm);	// UNOFFICIAL 2026-11-04 CasterCulling: skip LOD chunks (distant ridges taller than CasterDrop cast into Near/Middle at low sun)
			if (!cm) continue;

			if (!WalkSunIsTree && child->IsFadeNode() && static_cast<BSFadeNode*>(child)->FadeAlpha < TheShaderManager->Effects.ShadowsExteriors->Settings.ShadowMaps.CasterFadeMin) { FrameJournal::FadeDrops++; continue; } // stop rendering fadenodes below a certain opacity (UNOFFICIAL 2026-10-28: CasterFadeMin)
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
	if (IsExplodedShadowCaster(geo, isLODLand || AccumulatingLand)) return;
#if NVR_TEST_TOOLS
	OrthoProbe::NoteGeometry(geo);
#endif
	auto add = [this](unsigned m, NiGeometry* g, UInt8 pass, UInt8 dyn) {
		for (unsigned c; m; ) { m = ForEachSunCascade(m, c); SunCasters[c].push_back({ g, pass, dyn }); }
	};
	const int skinned = skinnedGeoPass->TestObject(geo);
	const UInt8 dyn = (WalkSunDynamic || skinned == 1) ? 1 : 0;
	if (skinned) { if (skinned == 1) add(mask, geo, kSunSkinned, 1); return; }
	if (speedTreePass->TestObject(geo)) { add(mask, geo, kSunTree, dyn); return; }

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
		if (lodMask & bit) SunCasters[c].push_back({ geo, kSunLodLand, 0 });
		else if (alphaMask & bit) SunCasters[c].push_back({ geo, kSunAlpha, dyn });
		else if (opaqueMask & bit) SunCasters[c].push_back({ geo, opaquePass, dyn });
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

void ShadowManager::PushSunCasters(int cascade, int dynamicFilter) {
	for (const SunCaster& caster : SunCasters[cascade]) {
		if (dynamicFilter == 0 && caster.dynamic) continue;
		if (dynamicFilter == 1 && !caster.dynamic) continue;
		SunPassOf(caster.pass)->GeometryList.push(caster.geometry);
	}
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
	auto add = [this](NiNode* node, const TESObjectREFR* ref) {
		if (!node || node->m_flags & NiAVObject::APP_CULLED) return;
		NiBound* bound = node->GetWorldBound();
		if (bound && isfinite(bound->Radius) && isfinite(bound->Center.x) && isfinite(bound->Center.y) && isfinite(bound->Center.z) && bound->Radius <= 2000.0f)
			FrameMovers.push_back(D3DXVECTOR4(bound->Center.x, bound->Center.y, bound->Center.z, bound->Radius));
		else if (ref)	// exploded child bound (e.g. counter-scaled weapon anim node): keep a sane mover sphere at the actor's position
			FrameMovers.push_back(D3DXVECTOR4(ref->pos.x, ref->pos.y, ref->pos.z + 64.0f, 128.0f));
	};
	add(Player->GetNode(), Player);
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
				add(Ref->GetNode(), Ref);
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

