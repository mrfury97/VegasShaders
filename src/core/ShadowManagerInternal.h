#pragma once
// Shared shadow walk helpers/state used across ShadowManager.cpp, ShadowSunCascades.cpp, and ShadowCharacterAndRefl.cpp.
#include "DisplaceShadows.h"
#include <unordered_set>

namespace ShadowManagerInternal {
inline bool AccumulatingLand = false;
inline bool WalkSunIsTree = false;
inline TESObjectREFR* LayerAccumRef = nullptr;
inline const std::unordered_set<const void*>* LayerOverlayRefs = nullptr;
inline bool LayerTreesMoving = true;

// SunLayers: a reference that can move by itself (or be moved): anything not on the still list -- doors, actors, items the physics
// moves, projectiles, moveable statics, an unknown kind -- and a still kind whose model is animated (a controller on its root: flags,
// fans, animated activators and lights).
inline bool RefMovesByKind(TESObjectREFR* Ref, NiNode* Node, bool treesMoving) {
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
		if (treesMoving) return true;
		break;
	default:
		return true;
	}
	return Node->m_controller != nullptr;
}
inline bool RefMovesByKind(TESObjectREFR* Ref, NiNode* Node) { return RefMovesByKind(Ref, Node, LayerTreesMoving); }

// Hands one geometry to the pass that draws its kind (skinned, tree, LOD land, alpha tested, opaque).
// UNOFFICIAL 2026-11-20 (exterior ~24m dark circle with Forms.Actors = true): weapon animation mods (e.g. MMP TOZ-34 / TOZ-66)
// hide sub-meshes like "##CVSHStock:0" by scaling a bone by 1e-9 and counter-scaling the child by 1e+9; on NPCs not playing that
// animation the child sits at world scale ~1e9 with a ~26-billion-unit world bound, passes frustum tests in every cascade, and is
// pancaked to depth 0 across 100% of cascades 0..2. Reject any non-land/non-LOD caster with a non-finite or absurd scale/bound.
inline bool IsExplodedShadowCaster(const NiGeometry* geo, bool isLandOrLOD = false) {
	if (!geo || isLandOrLOD) return false;
	const float s = geo->m_worldTransform.scale;
	if (!isfinite(s) || fabsf(s) > 1000.0f) return true;
	const NiBound* b = geo->m_kWorldBound;
	if (b && (!isfinite(b->Radius) || !isfinite(b->Center.x) || !isfinite(b->Center.y) || !isfinite(b->Center.z) || b->Radius > 1000000.0f))
		return true;
	return false;
}

inline unsigned ForEachSunCascade(unsigned mask, unsigned& bit) {	// lowest set bit index, cleared from mask
	unsigned long index;
	_BitScanForward(&index, mask);
	bit = index;
	return mask & (mask - 1);
}

// Object LOD inside loaded cells (center inside BSShaderManager::kLoadedRange for geometry, full bound for nodes).
inline bool IsLodInLoadedCells(NiAVObject* Object) {
	NiBound* Bound = Object->m_kWorldBound;
	if (!Bound) return false;
	const float Reach = Object->IsGeometry() ? 0.0f : Bound->Radius;
	const NiPoint4* Range = BSShaderManager::kLoadedRange;
	return fabsf(Bound->Center.x - Range->x) + Reach < Range->z && fabsf(Bound->Center.y - Range->y) + Reach < Range->w;
}
}
