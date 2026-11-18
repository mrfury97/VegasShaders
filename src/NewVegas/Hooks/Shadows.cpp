// Shadow hooks: NVR's shadow maps drawn in place of the game's actor shadow maps, and the name of the leaves node of a SpeedTree.
// (2026-11-05: the Oblivion torch cast-shadow flag hook, AddCastShadowFlag, never installed in New Vegas, was removed.)

#include "../../core/GpuProfiler.h"
#include "../../core/SessionLog.h"	// 2026-11-04: CRASH lines' stage

// RenderShadowMapHook replaces the whole of TESMain::DrawWorld_UpdateOffscreenBuffers (0x871290, a jump at its entry, Hooks.cpp): the
// game's actor shadow maps there are skipped for NVR's own. 0x871A50 is not a shadow render: it is TESMain::UpdateOffscreenInterface (the
// rendered menu, the Pip-Boy screen, drawn into its render texture), the last thing 0x871290 does, so the hook still calls it.
// (Name kept from when it was believed to be the game's shadow render; checked against the exe in Ghidra by room 05, 2026-11-18.)
static constexpr UInt32 kGameRenderShadowMaps = 0x871A50;

void __fastcall RenderShadowMapHook(void* apThis) {
	// UNOFFICIAL TAA: the jitter covers the world scene only, on the understanding that shadow maps are rendered before it.
	// If that ever stops being true, the cascades get fitted to a jittered frustum and shadow edges shimmer -- say so.
	static bool jitterWarned = false;
	if (!jitterWarned) {
		TAAEffect* taa = TheShaderManager->Effects.TAA;
		if (taa && taa->IsJitterActive()) {
			Logger::Log("[WARNING] TAA : shadow maps rendered while the world camera was jittered; cascades will shimmer");
			jitterWarned = true;
		}
	}

	{
		SessionLog::StageScope stage("shadow maps");
		TheShadowManager->RenderShadowMaps();
	}

	// Then the rendered-menu (Pip-Boy screen) texture update that 0x871290 ends with, timed so its share of the frame is visible.
	static GpuTimer gameShadowTimer("Game call 0x871A50 (rendered-menu texture)");
	GpuProfileScope scope(gameShadowTimer, TheRenderManager->device);
	CdeclCall(kGameRenderShadowMaps);
}

// The second child of a tree's node holds its leaves; name it so it can be told apart later.
static void NameTreeLeaves(BSTreeNode* TreeNode) {
	auto* leavesChild = TreeNode->m_children.data[1];
	leavesChild->SetName("Leaves");
}

__declspec(naked) void LeavesNodeNameHook() {
	__asm {
		mov		dword ptr [ebp - 70h], eax
		push	eax
		call	NameTreeLeaves
		pop		eax
		jmp		Jumpers::Shadows::LeavesNodeNameReturn
	}
}
