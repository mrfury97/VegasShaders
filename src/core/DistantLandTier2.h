#pragma once

// UNOFFICIAL Phase 4 Distant Land (Task 1, docs/next/DISTANT-LAND-PLAN.md):
// Tier-2 load-side suppression via clean Detours entry hooks on BGSTerrainNode::UpdateTrees (0x006FE330)
// and BGSTerrainNode::UpdateTerrain (0x006FDAA0).
// Attached only when [Main.Main.Quality] DistantLandSuppressVanilla > 0 at startup; falls back seamlessly
// whenever DistantLand < 2, DistantLandSuppressVanilla is lowered, or the active worldspace lacks a VS cache.

#include "DistantLandTerrain.h"

namespace DistantLandTier2 {
	inline constexpr UInt32 kUpdateTerrainAddr = 0x006FDAA0; // BGSTerrainNode::UpdateTerrain(const NiPoint3&, bool)
	inline constexpr UInt32 kUpdateTreesAddr   = 0x006FE330; // BGSTerrainNode::UpdateTrees(const NiPoint3&)

	typedef void(__thiscall* UpdateTerrainFn)(void* node, const NiPoint3* viewPosition, bool updateWaterVisibility);
	typedef void(__thiscall* UpdateTreesFn)(void* node, const NiPoint3* viewPosition);

	inline UpdateTerrainFn OrigUpdateTerrain = (UpdateTerrainFn)kUpdateTerrainAddr;
	inline UpdateTreesFn   OrigUpdateTrees   = (UpdateTreesFn)kUpdateTreesAddr;
	inline bool            Installed         = false;
	inline unsigned long   SuppressedTrees   = 0;
	inline unsigned long   SuppressedTerrain = 0;

	inline void LogVaSnapshot(const char* what, TESWorldSpace* world, void* resolvedWorld) {
		MEMORYSTATUSEX ms = {};
		ms.dwLength = sizeof(ms);
		double usedMB = 0.0, freeMB = 0.0;
		if (GlobalMemoryStatusEx(&ms)) {
			usedMB = (double)(ms.ullTotalVirtual - ms.ullAvailVirtual) / (1024.0 * 1024.0);
			freeMB = (double)ms.ullAvailVirtual / (1024.0 * 1024.0);
		}
		Logger::Log("UNOFFICIAL DistantLand Tier-2: suppressing %s for world 0x%08X (resolved %p; VA %.0f MB used, %.0f MB free)",
			what, world ? world->refID : 0u, resolvedWorld, usedMB, freeMB);
	}

	inline TESWorldSpace* WorldOfNode(void* node) {
		if (!node) return nullptr;
		void* mgr = *(void* const*)node; // BGSTerrainNode::pManager (+0x00)
		if (!mgr) return nullptr;
		return *(TESWorldSpace* const*)mgr; // BGSTerrainManager::pWorld (+0x00)
	}

	inline void __fastcall HookUpdateTrees(void* self, void*, const NiPoint3* viewPosition) {
		TESWorldSpace* world = WorldOfNode(self);
		if (world && DistantLandTerrain::ShouldSuppressVanillaTrees(world)) {
			++SuppressedTrees;
			static void* loggedWorld = nullptr;
			void* resolved = DistantLandTerrain::ResolveLodWorldPtr(world);
			if (loggedWorld != resolved) {
				loggedWorld = resolved;
				LogVaSnapshot("BGSTerrainNode::UpdateTrees (0x006FE330)", world, resolved);
			}
			return;
		}
		OrigUpdateTrees(self, viewPosition);
	}

	inline void __fastcall HookUpdateTerrain(void* self, void*, const NiPoint3* viewPosition, bool updateWaterVisibility) {
		TESWorldSpace* world = WorldOfNode(self);
		if (world && DistantLandTerrain::ShouldSuppressVanillaTerrain(world)) {
			++SuppressedTerrain;
			static void* loggedWorld = nullptr;
			void* resolved = DistantLandTerrain::ResolveLodWorldPtr(world);
			if (loggedWorld != resolved) {
				loggedWorld = resolved;
				LogVaSnapshot("BGSTerrainNode::UpdateTerrain (0x006FDAA0)", world, resolved);
			}
			return;
		}
		OrigUpdateTerrain(self, viewPosition, updateWaterVisibility);
	}

	inline void Attach() {
		const int suppressLvl = DistantLandLightStore::EffectiveSuppressLevel();
		if (suppressLvl <= 0) return;

		const UInt8* bTrees = (const UInt8*)kUpdateTreesAddr;
		const UInt8* bTerr  = (const UInt8*)kUpdateTerrainAddr;
		Logger::Log("UNOFFICIAL DistantLand Tier-2: attaching entry detours (level %d) | UpdateTrees(006FE330)=%02X %02X %02X %02X | UpdateTerrain(006FDAA0)=%02X %02X %02X %02X",
			suppressLvl,
			bTrees[0], bTrees[1], bTrees[2], bTrees[3],
			bTerr[0], bTerr[1], bTerr[2], bTerr[3]);

		NoteHookSite((SIZE_T)kUpdateTreesAddr, 8, "DistantLandTier2::UpdateTrees");
		NoteHookSite((SIZE_T)kUpdateTerrainAddr, 8, "DistantLandTier2::UpdateTerrain");
		DetourAttach(&(PVOID&)OrigUpdateTrees, (PVOID)&HookUpdateTrees);
		DetourAttach(&(PVOID&)OrigUpdateTerrain, (PVOID)&HookUpdateTerrain);
		Installed = true;
	}
}
