#pragma once
#include "GpuProfiler.h"

// UNOFFICIAL 2026-11-24 PortalVisibility ([Main.Main.Performance] PortalVisibilityCulling, live, default true):
// Uses the engine's interior room/portal graph (ShadowSceneNode::pBSPortalGraph, BSMultiBoundRoom, BSPortal)
// to cull hidden-room lamps from the froxel table, prioritize visible-room lamps for interior shadow slots at
// zero GPU cost, and skip references in distant occluded rooms (2+ hops behind portals) during interior
// reflection cubemap and flashlight/fallback shadow passes. Fails conservatively open whenever unbound space
// is visible or portal-graph metadata is absent.
namespace PortalVisibility {

	inline bool Enabled() {
		return TheSettingManager && TheSettingManager->GetSettingI("Main.Main.Performance", "PortalVisibilityCulling") != 0;
	}

	inline bool Active(const ShadowSceneNode* ssn) {
		if (!Enabled() || !ssn || ssn->bVisibleUnboundSpace) return false;
		const BSPortalGraph* pg = ssn->pBSPortalGraph;
		return pg && pg->lMultiBoundRoomList.numItems > 0 && pg->lMultiBoundRoomAccumList.numItems > 0;
	}

	inline bool IsRoomVisible(const BSMultiBoundNode* room) {
		return room && room->spMultiBound && room->spMultiBound->spShape &&
			room->spMultiBound->spShape->kCullResult == BSMultiBoundShape::kCull_Visible;
	}

	inline bool PortalTouchesVisible(const BSPortal* portal) {
		if (!portal) return false;
		return !portal->pMultiBoundRoom[0] || IsRoomVisible(portal->pMultiBoundRoom[0]) ||
			!portal->pMultiBoundRoom[1] || IsRoomVisible(portal->pMultiBoundRoom[1]);
	}

	// 0 = room currently visible, 1 = 1-hop neighbor connected through a portal bordering a visible room
	// (or unbound space), 2 = occluded behind multiple walls/portals.
	inline int RoomTier(const BSMultiBoundRoom* room) {
		if (IsRoomVisible(room)) return 0;
		const auto* node = room->kPortalList.start;
		for (unsigned steps = 0; node && steps < 256; node = node->next, steps++) {
			if (node->data && PortalTouchesVisible(node->data)) return 1;
		}
		return 2;
	}

	inline int LightTier(const ShadowSceneNode* ssn, const ShadowSceneLight* light) {
		if (!Active(ssn) || !light || light->pPortalGraph != ssn->pBSPortalGraph ||
			!light->kMultiboundRooms.pBuffer || light->kMultiboundRooms.iSize == 0 ||
			light->kMultiboundRooms.iSize > 4096) {
			return 0;
		}
		bool adjacent = false;
		for (UInt32 i = 0; i < light->kMultiboundRooms.iSize; i++) {
			const BSMultiBoundNode* mb = light->kMultiboundRooms.pBuffer[i];
			if (!mb) continue;
			const int tier = RoomTier(static_cast<const BSMultiBoundRoom*>(mb));
			if (tier == 0) return 0;
			if (tier == 1) adjacent = true;
		}
		if (light->kPortals.pBuffer && light->kPortals.iSize <= 4096) {
			for (UInt32 i = 0; i < light->kPortals.iSize; i++) {
				if (PortalTouchesVisible(light->kPortals.pBuffer[i])) return 0;
			}
		}
		return adjacent ? 1 : 2;
	}

	inline bool IsLightVisibleNow(const ShadowSceneNode* ssn, const ShadowSceneLight* light) {
		return LightTier(ssn, light) == 0;
	}

	struct RoomSet {
		static constexpr unsigned kMaxRooms = 256;
		struct Entry { const BSMultiBoundRoom* room; bool allowed; };
		Entry entries[kMaxRooms] = {};
		unsigned count = 0;
		bool active = false;

		void Build(const ShadowSceneNode* ssn) {
			count = 0;
			active = false;
			if (!Active(ssn)) return;
			const auto* pg = ssn->pBSPortalGraph;
			for (const auto* it = pg->lMultiBoundRoomList.start; it && count < kMaxRooms; it = it->next) {
				const BSMultiBoundRoom* room = it->data;
				if (!room) continue;
				entries[count++] = { room, RoomTier(room) <= 1 };
			}
			active = count > 0;
		}

		bool IsNodeHidden(const NiAVObject* node) const {
			if (!active || !node) return false;
			const NiAVObject* cur = node->m_parent;
			for (int d = 0; cur && d < 8; d++, cur = cur->m_parent) {
				for (unsigned i = 0; i < count; i++) {
					if (entries[i].room == cur) return !entries[i].allowed;
				}
			}
			return false;
		}
	};

	// F10 profiler statistics (logged every 240 frames when GpuTimer::Enabled).
	inline unsigned StatFrames = 0;
	inline UInt32 StatLastFrame = 0xFFFFFFFFu;
	inline unsigned long long StatFroxelTested = 0, StatFroxelCulled = 0;
	inline unsigned long long StatShadowTier0 = 0, StatShadowTier1 = 0, StatShadowTier2 = 0;
	inline unsigned long long StatReflTested = 0, StatReflCulled = 0;
	inline unsigned long long StatSpotTested = 0, StatSpotCulled = 0;

	inline void ResetStats() {
		StatFrames = 0;
		StatFroxelTested = StatFroxelCulled = 0;
		StatShadowTier0 = StatShadowTier1 = StatShadowTier2 = 0;
		StatReflTested = StatReflCulled = 0;
		StatSpotTested = StatSpotCulled = 0;
	}

	inline void NoteFroxelTest() { if (GpuTimer::Enabled) StatFroxelTested++; }
	inline void NoteFroxelCulled() { if (GpuTimer::Enabled) StatFroxelCulled++; }
	inline void NoteShadowRank(int tier) {
		if (!GpuTimer::Enabled) return;
		if (tier <= 0) StatShadowTier0++;
		else if (tier == 1) StatShadowTier1++;
		else StatShadowTier2++;
	}
	inline void NoteReflRef(bool hidden) {
		if (!GpuTimer::Enabled) return;
		StatReflTested++;
		if (hidden) StatReflCulled++;
	}
	inline void NoteSpotRef(bool hidden) {
		if (!GpuTimer::Enabled) return;
		StatSpotTested++;
		if (hidden) StatSpotCulled++;
	}

	inline void OnFrame(UInt32 frameNo) {
		if (!GpuTimer::Enabled) { ResetStats(); return; }
		if (frameNo == StatLastFrame) return;
		StatLastFrame = frameNo;
		if (++StatFrames < 240) return;
		if (StatFroxelTested || StatShadowTier0 || StatShadowTier1 || StatShadowTier2 || StatReflTested || StatSpotTested) {
			const double f = (double)StatFrames;
			Logger::Log("PORTAL CULL %u frames: froxel lamps culled %.1f / %.1f | shadow lamps tier 0/1/2: %.1f / %.1f / %.1f | refl refs culled %.1f / %.1f | spot refs culled %.1f / %.1f",
				StatFrames, StatFroxelCulled / f, StatFroxelTested / f,
				StatShadowTier0 / f, StatShadowTier1 / f, StatShadowTier2 / f,
				StatReflCulled / f, StatReflTested / f,
				StatSpotCulled / f, StatSpotTested / f);
		}
		ResetStats();
	}
}
