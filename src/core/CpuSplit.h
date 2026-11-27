#pragma once

// UNOFFICIAL 2026-10-21 CpuSplit (diagnostic, F10 profiling only; side session cpulimits, bench\deep-roadmap\CPU-LIMITS.md).
// On a PC with a slower processor the frame waits for the CPU, not the graphics card: testers' F10 logs showed "Game update (CPU)"
// 4-12 ms and "Game render (CPU)" 8-12 ms per frame. Those two lines say nothing about WHAT inside them costs the time. This splits
// them: the game's own frame (TESMain::OnIdle 0x0086E650 and the functions it calls one after another) is cut at the game's own
// `call` instructions, and the same is done inside the two AI worker threads' task functions.
//
// How: each listed call site (a 5-byte E8 call) is pointed at a small stub that notes (site, clock) and then jumps on to wherever
// the call went before, with registers, stack and the game's return address untouched (a plugin that checks its caller still
// sees the game's address). A span lasts from its stamp to the next stamp on the same thread, so it covers the call plus the
// game's own code up to the next listed call; spans are summed per bucket ("Upd ...", "Rnd ...", "AI ...") and reported as F10
// "CPU PROFILE" lines (per-frame averages, the format FNV-decomp\tools\compare_logs.py reads). NVR's own render hook and Present
// hook add four marks (world render begin/end, present begin/end), so the buckets add up to the frame interval.
// The design is FNVProbe's (work\FNVProbe, 2026-10-07); the site list was re-checked against the dumped exe for this file
// (bench\deep-roadmap\cpulimits\check_sites.py).
//
// Safety: nothing is patched until F10 profiling is first switched on (a normal run is untouched, as with the Present hook).
// Every site is byte-checked first: it must be an E8 call whose target is the expected game function, or a function in another
// loaded module (another plugin got there first: we chain through it), else that site is left alone and named in the log.
// Main-thread sites are patched from the render hook (only the main thread runs them); AI-thread sites only at the next frame
// start, while the AI threads are parked (they are released again at 0x0086EC87, later in OnIdle). Once patched, the stubs stay
// (cost: about 130 clock reads per frame, a few microseconds); the per-frame sums run only while F10 profiling is on.
#include <windows.h>
#include <tlhelp32.h>
#include <string>
#include "GpuProfiler.h"

namespace CpuSplit {

	enum List : UInt8 { kMain, kAICombined, kAI1, kAI2, kListCount };

	enum Bucket : UInt8 {
		// main thread, TESMain::OnIdle before the render
		bFrameStart, bPlugins, bPlayer, bIO, bLodTex, bCells, bScripts, bTempLists, bMemory, bGrid, bVisibility, bAIStart, bAnims, bManagers,
		bParallelWait, bLights,
		// TESMain::Swap
		bSwapStart, bLockWait, bRndSetup, bRndParallelWait, bRndWater, bRndOffscreen, bRndCull, bRndAccum, bRndOpaque, bRndAfterDepth,
		bRndSceneEnd, bRnd1st, bRndImage, bRndEnd, bHud, bEndFrame, bPresent, bAfterPresent,
		// TESMain::PostSwapProcess and the rest of OnIdle
		bPostSwap, bAudio, bProcLists, bAIJoin, bPostAI, bPostThreads, bFrameEnd, bOverhead,
		// AI worker threads (both threads' task functions; one thread when the game runs them combined)
		bAICombat, bAIAnim, bAIPackages, bAIHigh, bAIDetect, bAIMove, bAIHavok, bAIOther, bAI1Wait, bAI2Wait, bAI1Busy, bAI2Busy,
		bNone,	// an AI task function's last stamp: marks its end, no span of its own
		kBucketCount = bNone
	};

	static const char* const BucketNames[kBucketCount] = {
		"Upd frame start", "Upd plugins (main loop)", "Upd player + controls", "Upd loads finished (IO)", "Upd LOD, textures, faders",
		"Upd cell tests + anims", "Upd actor scripts", "Upd temp process lists", "Upd memory, garbage, trees", "Upd cell grid changes",
		"Upd decals + visibility", "Upd AI start", "Upd animations + effects", "Upd interface,paths,combat", "Upd parallel update wait",
		"Upd lights + menu backgr.",
		"Swp before world render", "Swp renderer lock (wait)", "Rnd frame setup", "Rnd parallel update wait", "Rnd water reflections",
		"Rnd sun shadows+offscreen", "Rnd scene setup + culling", "Rnd accumulate", "Rnd world draws", "Rnd world draws (2nd)",
		"Rnd splatter + sun glare", "Rnd depth,refract,1st pers", "Rnd image space (effects)", "Rnd end", "Swp HUD + menus",
		"Swp end frame (events)", "Swp present", "Swp after present",
		"Post particles + misc", "Post audio", "Post process lists", "AI join wait (main)", "Post-AI actor updates",
		"Post-AI threads process", "Frame end + message loop", "CPU split overhead",
		"AI combat", "AI animation", "AI packages + magic", "AI high actors", "AI detection", "AI movement", "AI Havok + cell anims",
		"AI other", "AI1 waits", "AI2 waits", "AI1 busy (thread total)", "AI2 busy (thread total)",
	};

	// { call site, its target in the dumped exe (0 = owned by a plugin when dumped: any module accepted), list, bucket, what }
	// Checked against fnv-re\FalloutNV.dumped.exe by bench\deep-roadmap\cpulimits\check_sites.py (parses these lines).
	struct Site { UInt32 site, expected; UInt8 list, bucket; const char* what; };
	static const Site Sites[] = {
		{ 0x0086B3E3, 0x0086E650, kMain, bFrameStart, "WinMain -> TESMain::OnIdle (frame start)" },
		{ 0x0086E667, 0x00000000, kMain, bPlugins, "OnIdle start hook (xNVSE main loop, plugins)" },
		{ 0x0086E758, 0x008782B0, kMain, bPlayer, "MemoryLevelManager::RunNonDestructiveFree" },
		{ 0x0086E760, 0x0086F940, kMain, bPlayer, "OnIdle_UpdatePlayer" },
		{ 0x0086E76C, 0x006FF860, kMain, bPlayer, "SteamAPI_RunCallbacks" },
		{ 0x0086E799, 0x0086FD90, kMain, bPlayer, "OnIdle_UpdateImageSpace" },
		{ 0x0086E826, 0x006DA7C0, kMain, bPlayer, "Pathing::ProfilePathing" },
		{ 0x0086E836, 0x00851D90, kMain, bPlayer, "BGSSaveLoadManager::UpdateQueuedSaves" },
		{ 0x0086E855, 0x004E1610, kMain, bPlayer, "SleepManager::RunSleep" },
		{ 0x0086E884, 0x0086F260, kMain, bPlayer, "OnIdle_UpdateTimer" },
		{ 0x0086E88C, 0x0086F390, kMain, bPlayer, "OnIdle_PollControls" },
		{ 0x0086E897, 0x00C3DBF0, kMain, bIO, "IOManager::UpdateQueue" },
		{ 0x0086E8B4, 0x0078CFC0, kMain, bIO, "LoadingMenu::SuspendBackgroundThread" },
		{ 0x0086E8D3, 0x00457D70, kMain, bIO, "TES::ShowLoadingMenu" },
		{ 0x0086E8DB, 0x0086EFE0, kMain, bLodTex, "OnIdle_ScaleLOD" },
		{ 0x0086E8F5, 0x00B6DD00, kMain, bLodTex, "BSTextureManager::Update" },
		{ 0x0086E8FD, 0x0086F450, kMain, bLodTex, "OnIdle_HandleMenuBackground" },
		{ 0x0086E90A, 0x007011D0, kMain, bLodTex, "FaderManager::UpdateFaders" },
		{ 0x0086E925, 0x004E0110, kMain, bLodTex, "ScreenSplatter::Update" },
		{ 0x0086E92A, 0x004DE600, kMain, bLodTex, "ScreenCustomSplatter::Update" },
		{ 0x0086E938, 0x00A81A20, kMain, bLodTex, "NiParallelUpdateTaskManager::BeginUpdate" },
		{ 0x0086E987, 0x004556D0, kMain, bCells, "TES::TestAllCells" },
		{ 0x0086E9CA, 0x00867A40, kMain, bCells, "Calendar::Update" },
		{ 0x0086E9E4, 0x00455640, kMain, bCells, "TES::RunAnimations" },
		{ 0x0086E9F0, 0x0040FBF0, kMain, bScripts, "ProcessLists lock" },
		{ 0x0086E9FA, 0x00978550, kMain, bScripts, "ProcessLists::RunActorScripts" },
		{ 0x0086EA3F, 0x009777A0, kMain, bTempLists, "ProcessLists::UpdateRadiationList" },
		{ 0x0086EA49, 0x0096EB40, kMain, bTempLists, "ProcessLists::ChangeProcessLevelTempList" },
		{ 0x0086EA53, 0x0096E9B0, kMain, bTempLists, "ProcessLists::UpdateFollowerTempList" },
		{ 0x0086EA8B, 0x009777A0, kMain, bTempLists, "ProcessLists::UpdateRadiationList (b)" },
		{ 0x0086EAA7, 0x0096EB40, kMain, bTempLists, "ProcessLists::ChangeProcessLevelTempList (b)" },
		{ 0x0086EAB1, 0x0096E9B0, kMain, bTempLists, "ProcessLists::UpdateFollowerTempList (b)" },
		{ 0x0086EAC9, 0x00878080, kMain, bMemory, "MemoryLevelManager::CheckMemoryLevel" },
		{ 0x0086EADA, 0x00A61CD0, kMain, bMemory, "BSTexturePalette::PurgeUnusedTextures" },
		{ 0x0086EADF, 0x00868850, kMain, bMemory, "GarbageCollector::Update" },
		{ 0x0086EAE4, 0x00868D10, kMain, bMemory, "GarbageCollector::ClearTempEffects" },
		{ 0x0086EB14, 0x006652E0, kMain, bMemory, "BSTreeManager::Update" },
		{ 0x0086EB1F, 0x0086FBE0, kMain, bGrid, "OnIdle_UpdateCurrentGridCell" },
		{ 0x0086EB36, 0x0086FD70, kMain, bGrid, "OnIdle_DoInterfaceIdle (menu path)" },
		{ 0x0086EB4A, 0x0049FFF0, kMain, bVisibility, "BGSDecalManager::UpdateDecals" },
		{ 0x0086EBCC, 0x0045BC80, kMain, bVisibility, "TES::ResetAllMultiBoundNodes" },
		{ 0x0086EBF5, 0x00B5AC90, kMain, bVisibility, "ShadowSceneNode::UpdateOcclusionPlaneVisibility" },
		{ 0x0086EC18, 0x0045B070, kMain, bVisibility, "TES::UpdateMultiBoundVisibility" },
		{ 0x0086EC87, 0x008C78C0, kMain, bAIStart, "AILinearTaskThreadManager::StartThreads" },
		{ 0x0086EC93, 0x008CA070, kMain, bAIStart, "AITaskManager::StartTasksDuringRendering" },
		{ 0x0086EC9B, 0x0086FC60, kMain, bAnims, "OnIdle_UpdateAnimationsAndEffects" },
		{ 0x0086ECBA, 0x0086FD70, kMain, bManagers, "OnIdle_DoInterfaceIdle" },
		{ 0x0086ECCB, 0x007058E0, kMain, bManagers, "Interface::LastMinuteUpdate" },
		{ 0x0086ECE2, 0x006EBC50, kMain, bManagers, "PathManager::Update" },
		{ 0x0086ECEE, 0x006A61B0, kMain, bManagers, "NavMeshRender::Update" },
		{ 0x0086ED1A, 0x006C3640, kMain, bManagers, "NavMeshObstacleManager::Update" },
		{ 0x0086ED55, 0x00991500, kMain, bManagers, "CombatManager::Update" },
		{ 0x0086ED66, 0x00A81A80, kMain, bParallelWait, "NiParallelUpdateTaskManager::EndUpdate" },
		{ 0x0086ED77, 0x007056F0, kMain, bLights, "SleepWaitMenu::UpdateSleeping" },
		{ 0x0086ED86, 0x00871DC0, kMain, bLights, "TESMain::RenderMenuBackground" },
		{ 0x0086ED8B, 0x0057AB70, kMain, bLights, "CheckWithinMultiBoundTask::DoAttachments" },
		{ 0x0086ED90, 0x00B60040, kMain, bLights, "ShadowSceneNode::ProcessAllQueuedLights" },
		{ 0x0086EDE8, 0x0086FF70, kMain, bSwapStart, "TESMain::Swap" },
		{ 0x0086FF94, 0x004A0370, kMain, bLockWait, "Swap/NiDX9Renderer::LockRenderer" },
		{ 0x0087000F, 0x00871DC0, kMain, bSwapStart, "Swap/TESMain::RenderMenuBackground" },
		{ 0x00870063, 0x00BA2F30, kMain, bSwapStart, "Swap/MTRenderingSystem::StartThreads" },
		{ 0x00870073, 0x004DCEF0, kMain, bSwapStart, "Swap/Renderer::UpdateGamma" },
		{ 0x00870233, 0x004BAF10, kMain, bSwapStart, "Swap/GridCellArray::RecreateCanopyShadowMaskTexture" },
		{ 0x0087075E, 0x00870BD0, kMain, bRndSetup, "DrawWorld/TESMain::DrawWorldStandard" },
		{ 0x00870C16, 0x00872AD0, kMain, bRndParallelWait, "DrawWorld/WaitForParallelUpdate" },
		{ 0x00870C1E, 0x00872B00, kMain, bRndSetup, "DrawWorld/DrawWorld_Init" },
		{ 0x00870C29, 0x008727D0, kMain, bRndWater, "DrawWorld/UpdateWater" },
		{ 0x00870C31, 0x008749B0, kMain, bRndSetup, "DrawWorld/VATSOcclusionQuery" },
		{ 0x00870C3C, 0x00871290, kMain, bRndOffscreen, "DrawWorld/UpdateOffscreenBuffers (NVR: RenderShadowMapHook)" },
		{ 0x00870C53, 0x00872F50, kMain, bRndSetup, "DrawWorld/InitRenderTexture" },
		{ 0x00870C63, 0x00874AC0, kMain, bRndSetup, "DrawWorld/DrawWorld_VATS" },
		{ 0x00870C76, 0x00874B50, kMain, bRndSetup, "DrawWorld/RestartRenderTexture" },
		{ 0x00870DFD, 0x00B4F540, kMain, bRndSetup, "DrawWorld/EnableFullScreenScissor" },
		{ 0x00870E18, 0x00873200, kMain, bRndCull, "DrawWorld/DrawScene" },
		{ 0x00873258, 0x00642860, kMain, bRndCull, "DrawScene/Sun::StartSunOcclusion" },
		{ 0x008732C5, 0x00871220, kMain, bRndCull, "DrawScene/DepthOfField::DoDepth" },
		{ 0x0087330C, 0x00871220, kMain, bRndCull, "DrawScene/DepthOfField::DoDepth (b)" },
		{ 0x0087337E, 0x0041FD00, kMain, bRndCull, "DrawScene/culling setup + scene lists" },
		{ 0x00873AA0, 0x00454B30, kMain, bRndCull, "DrawScene/portal graph" },
		{ 0x00873C40, 0x00B5BA80, kMain, bRndCull, "DrawScene/ShadowSceneNode::BuildSharedCompoundFrustum" },
		{ 0x00873F5F, 0x00B5E870, kMain, bRndCull, "DrawScene/ShadowSceneNode::PreOnVisible" },
		{ 0x00873F8C, 0x00C49850, kMain, bRndCull, "DrawScene/BSCompoundFrustum::PrethreadOpList + cull" },
		{ 0x00873FD4, 0x00B54AC0, kMain, bRndAccum, "DrawScene/BSShaderManager::SetAccumulator" },
		{ 0x00874037, 0x00BA3390, kMain, bRndAccum, "DrawScene/MTRenderingSystem::AddAccumTask" },
		{ 0x00874047, 0x00BA30F0, kMain, bRndAccum, "DrawScene/MTRenderingSystem::SetThreadStage" },
		{ 0x008740CE, 0x00B6BFC0, kMain, bRndAccum, "DrawScene/BSShaderUtil::AccumulateSceneList" },
		{ 0x008740F2, 0x00B6BFC0, kMain, bRndAccum, "DrawScene/BSShaderUtil::AccumulateSceneList (b)" },
		{ 0x00874105, 0x00BA3130, kMain, bRndAccum, "DrawScene/MTRenderingSystem::WaitOnThreadStage" },
		{ 0x0087415B, 0x00B6BEE0, kMain, bRndAccum, "DrawScene/BSShaderUtil::AccumulateScene" },
		{ 0x00874180, 0x00B6BA20, kMain, bRndOpaque, "DrawScene/BSShaderUtil::RenderScenePreResolveDepth" },
		{ 0x008741AF, 0x00800F30, kMain, bRndAfterDepth, "DrawScene/FOVATSEffectManager::DrawTargetToTexture" },
		{ 0x008741DB, 0x00B6B930, kMain, bRndAfterDepth, "DrawScene/BSShaderUtil::RenderScenePostResolveDepth" },
		{ 0x00874210, 0x00404F70, kMain, bRndSceneEnd, "DrawScene/AutoMemContext::Leave + splatter setup" },
		{ 0x008742B7, 0x00B6BEE0, kMain, bRndSceneEnd, "DrawScene/ScreenSplatter AccumulateScene" },
		{ 0x008742D9, 0x00B6C0D0, kMain, bRndSceneEnd, "DrawScene/ScreenSplatter RenderScene" },
		{ 0x0087436D, 0x00B6BEE0, kMain, bRndSceneEnd, "DrawScene/ScreenCustomSplatter AccumulateScene" },
		{ 0x0087438F, 0x00B6C0D0, kMain, bRndSceneEnd, "DrawScene/ScreenCustomSplatter RenderScene" },
		{ 0x0087440F, 0x006426A0, kMain, bRndSceneEnd, "DrawScene/Sun::CalculateSunGlare" },
		{ 0x00874418, 0x00872DD0, kMain, bRndSceneEnd, "DrawScene/SetRunBoundingSphereTests + exit" },
		{ 0x00870E4F, 0x00871220, kMain, bRnd1st, "DrawWorld/DepthOfField::DoDepth" },
		{ 0x00870ED0, 0x008746D0, kMain, bRnd1st, "DrawWorld/Add1stPersonToDepthBuffer" },
		{ 0x00870EE3, 0x00874B50, kMain, bRnd1st, "DrawWorld/RestartRenderTexture (b)" },
		{ 0x00870F23, 0x00874920, kMain, bRnd1st, "DrawWorld/Refraction" },
		{ 0x00870F33, 0x00874C10, kMain, bRnd1st, "DrawWorld/Setup1stPerson" },
		{ 0x00870F46, 0x00874B50, kMain, bRnd1st, "DrawWorld/RestartRenderTexture (c)" },
		{ 0x00870F5E, 0x00BA3130, kMain, bRnd1st, "DrawWorld/MTRenderingSystem::WaitOnThreadStage" },
		{ 0x00870F74, 0x00875110, kMain, bRnd1st, "DrawWorld/MTRender1stPerson" },
		{ 0x00870F86, 0x00BA30F0, kMain, bRnd1st, "DrawWorld/MTRenderingSystem::SetThreadStage" },
		{ 0x00870F96, 0x00BA3130, kMain, bRnd1st, "DrawWorld/MTRenderingSystem::WaitOnThreadStage (b)" },
		{ 0x008710E4, 0x00875FD0, kMain, bRndImage, "DrawWorld/DrawWorld_ImageSpace" },
		{ 0x00871114, 0x00876A20, kMain, bRndEnd, "DrawWorld/DrawWorld_ScreenSplatter" },
		{ 0x0087112D, 0x00876830, kMain, bRndEnd, "DrawWorld/StopRenderTarget" },
		{ 0x00871146, 0x00B540D0, kMain, bRndEnd, "DrawWorld/DisableFullScreenScissor" },
		{ 0x00871208, 0x00876850, kMain, bRndEnd, "DrawWorld/DrawWorld_End" },
		{ 0x008702EA, 0x00BA3130, kMain, bHud, "Swap/MTRenderingSystem::WaitOnThreadStage" },
		{ 0x008702F7, 0x00871A50, kMain, bHud, "Swap/TESMain::UpdateOffscreenInterface" },
		{ 0x008703BC, 0x00872940, kMain, bHud, "Swap/TESMain::DrawWorldMenu3D" },
		{ 0x008703EC, 0x00874B90, kMain, bHud, "Swap/TESMain::DrawWorld_CopyMenuBG" },
		{ 0x00870403, 0x00709B40, kMain, bHud, "Swap/Interface::DrawHUDMenu3D" },
		{ 0x00870417, 0x007028B0, kMain, bHud, "Swap/Interface::Click (menus)" },
		{ 0x00870426, 0x00BA2FA0, kMain, bHud, "Swap/MTRenderingSystem::WaitForThreads" },
		{ 0x00870545, 0x00878860, kMain, bEndFrame, "Swap/ScreenShot::TakeScreenShot" },
		{ 0x0087054D, 0x00C458F0, kMain, bEndFrame, "Swap/NiObjectGarbage::Update" },
		{ 0x0087055E, 0x00B6B730, kMain, bEndFrame, "Swap/BSRenderedTexture::Stop (xNVSE events first)" },
		{ 0x00870594, 0x004A03C0, kMain, bAfterPresent, "Swap/NiDX9Renderer::UnlockRenderer" },
		{ 0x0086EDF0, 0x008705D0, kMain, bPostSwap, "TESMain::PostSwapProcess" },
		{ 0x008705DA, 0x0086F640, kMain, bAudio, "PostSwap/OnIdle_UpdateAudio" },
		{ 0x008705E2, 0x0086F890, kMain, bProcLists, "PostSwap/OnIdle_UpdateProcessLists" },
		{ 0x008705F6, 0x00552570, kMain, bPostSwap, "PostSwap/TESObjectCELL::RunQueuedParticleUpdates" },
		{ 0x0086EE00, 0x004DC360, kMain, bAIJoin, "TESRenderer::DoResize" },
		{ 0x0086EE4E, 0x008C7990, kMain, bAIJoin, "AILinearTaskThreadManager::WaitForThreads" },
		{ 0x0086EE5A, 0x008CA300, kMain, bAIJoin, "AITaskManager::WaitForTasksDuringRendering" },
		{ 0x0086EE62, 0x0086F6A0, kMain, bPostAI, "TESMain::UpdateNonRenderSafeAITasks" },
		{ 0x0086EE6A, 0x00870610, kMain, bPostThreads, "OnIdle_PostThreadsProcess" },
		{ 0x0086EE8F, 0x005AE270, kMain, bFrameEnd, "Script::ClearOptimizations" },
		{ 0x0086EE94, 0x005A9D60, kMain, bFrameEnd, "ScriptLocals::ClearOptimizations" },
		{ 0x0086EF0E, 0x00AA7290, kMain, bFrameEnd, "MemoryPool::SortFreeBlocks (then OnIdle exit, message loop)" },
		// AI thread 1 (AIThread1TaskFunc 0x008C7DA0)
		{ 0x008C7DAE, 0x008C79E0, kAI1, bAIOther, "AI1/start (SetThreadStage)" },
		{ 0x008C7DB9, 0x0086FD70, kAI1, bAIOther, "AI1/OnIdle_DoInterfaceIdle" },
		{ 0x008C7DD5, 0x0087A6B0, kAI1, bAIOther, "AI1/TaskQueueInterface::ThreadBeginInput" },
		{ 0x008C7DE9, 0x00991500, kAI1, bAICombat, "AI1/CombatManager::Update" },
		{ 0x008C7DF3, 0x0096CCA0, kAI1, bAIAnim, "AI1/ProcessLists::RunActorAnimationUpdates" },
		{ 0x008C7DFD, 0x0096CDA0, kAI1, bAIAnim, "AI1/ProcessLists::ParallelActorAnimationMovementUpdates" },
		{ 0x008C7E18, 0x00000000, kAI1, bAIOther, "AI1/call 008C7E18 (plugin-owned)" },
		{ 0x008C7E33, 0x00991DC0, kAI1, bAICombat, "AI1/CombatManager::UpdateCombatants" },
		{ 0x008C7E4D, 0x009784C0, kAI1, bAIPackages, "AI1/ProcessLists::RunActorMagicUpdates" },
		{ 0x008C7E6F, 0x0096BCD0, kAI1, bAIPackages, "AI1/ProcessLists::UpdateHighListPackages" },
		{ 0x008C7E84, 0x0096D520, kAI1, bAIPackages, "AI1/ProcessLists::UpdatePlayerFollowers" },
		{ 0x008C7EA4, 0x008C7A70, kAI1, bAI1Wait, "AI1/WaitOnThreadStage" },
		{ 0x008C7EB4, 0x008C7A70, kAI1, bAI1Wait, "AI1/WaitOnThreadStage (b)" },
		{ 0x008C7EBE, 0x0096C7C0, kAI1, bAIHigh, "AI1/ProcessLists::UpdateHighActors" },
		{ 0x008C7EDA, 0x0087A6D0, kAI1, bAIOther, "AI1/TaskQueueInterface::ThreadEndInput" },
		{ 0x008C7EFA, 0x008C7A70, kAI1, bAI1Wait, "AI1/WaitOnThreadStage (c)" },
		{ 0x008C7EFF, 0x008C80D0, kAI1, bAI1Wait, "AI1/WaitMainRendering" },
		{ 0x008C7F2C, 0x006C3640, kAI1, bAIOther, "AI1/NavMeshObstacleManager::Update" },
		{ 0x008C7F31, 0x005AE270, kAI1, bAIOther, "AI1/Script::ClearOptimizations" },
		{ 0x008C7F36, 0x00000000, kAI1, bAIOther, "AI1/call 008C7F36 (plugin-owned)" },
		{ 0x008C7F46, 0x008C79E0, kAI1, bNone, "AI1/end (SetThreadStage)" },
		// AI thread 2 (AIThread2TaskFunc)
		{ 0x008C7F5E, 0x008C79E0, kAI2, bAIOther, "AI2/start (SetThreadStage)" },
		{ 0x008C7F6E, 0x008C7A70, kAI2, bAI2Wait, "AI2/WaitOnThreadStage" },
		{ 0x008C7F7A, 0x0087A6B0, kAI2, bAIOther, "AI2/TaskQueueInterface::ThreadBeginInput" },
		{ 0x008C7F84, 0x0096C330, kAI2, bAIDetect, "AI2/ProcessLists::RunDetectionForAllActors" },
		{ 0x008C7F94, 0x008C79E0, kAI2, bAIOther, "AI2/SetThreadStage" },
		{ 0x008C7FB4, 0x008C7A70, kAI2, bAI2Wait, "AI2/WaitOnThreadStage (b)" },
		{ 0x008C7FC8, 0x0096DB30, kAI2, bAIMove, "AI2/ProcessLists::UpdateActorsMovement" },
		{ 0x008C7FF4, 0x0096CB50, kAI2, bAIAnim, "AI2/ProcessLists::RunActorRagdollAnimationUpdates" },
		{ 0x008C8009, 0x004772F0, kAI2, bAIOther, "AI2/BGSDestructibleObjectForm::UpdateDestructibleObjects" },
		{ 0x008C8029, 0x008C7A70, kAI2, bAI2Wait, "AI2/WaitOnThreadStage (c)" },
		{ 0x008C803D, 0x00453550, kAI2, bAIHavok, "AI2/TES::UpdateCellAnimations (Havok step)" },
		{ 0x008C8059, 0x0087A6D0, kAI2, bAIOther, "AI2/TaskQueueInterface::ThreadEndInput" },
		{ 0x008C8069, 0x008C7A70, kAI2, bAI2Wait, "AI2/WaitOnThreadStage (d)" },
		{ 0x008C807E, 0x005AE270, kAI2, bAIOther, "AI2/Script::ClearOptimizations" },
		{ 0x008C8093, 0x008C7A70, kAI2, bAI2Wait, "AI2/WaitOnThreadStage (e)" },
		{ 0x008C80A3, 0x008C7A70, kAI2, bNone, "AI2/end (last WaitOnThreadStage)" },
		// one AI thread (the game's combined task function, when it runs a single AI thread)
		{ 0x008C7BDE, 0x008C79E0, kAICombined, bAIOther, "AI/start (SetThreadStage)" },
		{ 0x008C7BE9, 0x0086FD70, kAICombined, bAIOther, "AI/OnIdle_DoInterfaceIdle" },
		{ 0x008C7C19, 0x00991500, kAICombined, bAICombat, "AI/CombatManager::Update" },
		{ 0x008C7C2F, 0x0096CCA0, kAICombined, bAIAnim, "AI/ProcessLists::RunActorAnimationUpdates" },
		{ 0x008C7C39, 0x0096CDA0, kAICombined, bAIAnim, "AI/ProcessLists::ParallelActorAnimationMovementUpdates" },
		{ 0x008C7C4E, 0x00000000, kAICombined, bAIOther, "AI/call 008C7C4E (plugin-owned)" },
		{ 0x008C7C64, 0x009784C0, kAICombined, bAIPackages, "AI/ProcessLists::RunActorMagicUpdates" },
		{ 0x008C7C79, 0x00991DC0, kAICombined, bAICombat, "AI/CombatManager::UpdateCombatants" },
		{ 0x008C7C97, 0x0096BCD0, kAICombined, bAIPackages, "AI/ProcessLists::UpdateHighListPackages" },
		{ 0x008C7C9C, 0x0096D520, kAICombined, bAIPackages, "AI/ProcessLists::UpdatePlayerFollowers" },
		{ 0x008C7CA6, 0x0096C330, kAICombined, bAIDetect, "AI/ProcessLists::RunDetectionForAllActors" },
		{ 0x008C7CBA, 0x0096DB30, kAICombined, bAIMove, "AI/ProcessLists::UpdateActorsMovement" },
		{ 0x008C7CD6, 0x0096C7C0, kAICombined, bAIHigh, "AI/ProcessLists::UpdateHighActors" },
		{ 0x008C7CE0, 0x0096CB50, kAICombined, bAIAnim, "AI/ProcessLists::RunActorRagdollAnimationUpdates" },
		{ 0x008C7CE5, 0x004772F0, kAICombined, bAIOther, "AI/BGSDestructibleObjectForm::UpdateDestructibleObjects" },
		{ 0x008C7D03, 0x00453550, kAICombined, bAIHavok, "AI/TES::UpdateCellAnimations (Havok step)" },
		{ 0x008C7D18, 0x008C80D0, kAICombined, bAI1Wait, "AI/WaitMainRendering" },
		{ 0x008C7D35, 0x006C3640, kAICombined, bAIOther, "AI/NavMeshObstacleManager::Update" },
		{ 0x008C7D46, 0x005AE270, kAICombined, bAIOther, "AI/Script::ClearOptimizations" },
		{ 0x008C7D6B, 0x008C7D80, kAICombined, bNone, "AI/end (SetThreadStageDirect)" },
	};
	static const UInt32 kSiteCount = sizeof(Sites) / sizeof(Sites[0]);
	static const UInt32 kAnchor = 0;	// Sites[0]: WinMain's call of OnIdle starts each frame

	// NVR's own hooks mark these (stamp numbers after the sites)
	enum Mark : UInt32 { kRenderBegin = kSiteCount, kRenderEnd, kPresentBegin, kPresentEnd, kStampIds };
	static UInt8 MarkBucket(UInt32 id) {
		switch (id) {
		case kRenderBegin: return bRndSetup;
		case kRenderEnd: return bHud;
		case kPresentBegin: return bPresent;
		default: return bAfterPresent;
		}
	}

	struct Stamp { UInt32 id; LONGLONG t; };
	static const UInt32 kMainCap = 2048, kAICap = 256;
	struct StampList { volatile LONG count; Stamp* items; UInt32 cap; };
	static Stamp MainStamps[kMainCap], AIStamps[3][kAICap];
	static StampList Lists[kListCount] = { { 0, MainStamps, kMainCap }, { 0, AIStamps[0], kAICap }, { 0, AIStamps[1], kAICap }, { 0, AIStamps[2], kAICap } };

	static DWORD AIThreadIds[kListCount];	// the thread each AI list was last stamped on (for the test-only CpuSampler)
	static UInt32 Targets[kSiteCount];		// where each patched call goes now (the game's function, or another plugin's hook)
	static bool Patched[kSiteCount];
	static UInt8* Code = nullptr;			// the stubs: one common stub, then 16 bytes per site
	static DWORD MainThread = 0;
	static bool MainInstalled = false, AIInstalled = false, Tried = false;
	static LONGLONG Frequency = 0;
	static LONGLONG LastOverhead = 0;		// the previous frame start's own time (sums and reports), counted in the next frame
	static CpuTimer* Timers[kBucketCount] = {};
	static unsigned FramesSummed = 0, FramesDropped = 0;

	static inline LONGLONG Now() { LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart; }

	static void FrameStart(LONGLONG t);

	// Called by the common stub with the site number; returns where the call goes on to.
	static UInt32 __cdecl OnStamp(UInt32 id) {
		const LONGLONG t = Now();
		const Site& s = Sites[id];
		if (s.list == kMain) {
			if (GetCurrentThreadId() != MainThread) return Targets[id];
			if (id == kAnchor) { FrameStart(t); return Targets[id]; }
		}
		StampList& l = Lists[s.list];
		const LONG n = InterlockedIncrement(&l.count) - 1;
		if ((UInt32)n < l.cap) { l.items[n].id = id; l.items[n].t = t; }
		if (s.list != kMain) AIThreadIds[s.list] = GetCurrentThreadId();
		return Targets[id];
	}

	// NVR's render and Present hooks (main thread).
	static void MarkNow(UInt32 id) {
		if (!MainInstalled || GetCurrentThreadId() != MainThread) return;
		StampList& l = Lists[kMain];
		const LONG n = InterlockedIncrement(&l.count) - 1;
		if ((UInt32)n < l.cap) { l.items[n].id = id; l.items[n].t = Now(); }
	}

	static UInt8 BucketOf(UInt32 id) { return id < kSiteCount ? Sites[id].bucket : MarkBucket(id); }

	inline void (*OnFrameBuckets)(const double* ms, UInt32 count) = nullptr;

	// The frame that just ended: Lists[kMain] starts with the previous frame start; the AI lists hold this frame's AI work
	// (the AI threads are parked now).
	static void Sum(LONGLONG frameEnd) {
		double ms[kBucketCount] = {};
		const double toMs = 1000.0 / (double)Frequency;
		StampList& m = Lists[kMain];
		const UInt32 n = (UInt32)m.count;
		if (n == 0 || n > m.cap || m.items[0].id != kAnchor) { FramesDropped++; return; }	// a frame that overflowed (a loading screen) or the first one
		for (UInt32 i = 0; i < n; i++) {
			const LONGLONG end = i + 1 < n ? m.items[i + 1].t : frameEnd;
			ms[BucketOf(m.items[i].id)] += (double)(end - m.items[i].t) * toMs;
		}
		ms[bOverhead] += (double)LastOverhead * toMs;
		for (UInt32 list = kAICombined; list < kListCount; list++) {
			StampList& a = Lists[list];
			const UInt32 count = (UInt32)a.count;
			if (count < 2 || count > a.cap) continue;
			double waits = 0.0;
			for (UInt32 i = 0; i + 1 < count; i++) {
				const UInt8 b = BucketOf(a.items[i].id);
				const double d = (double)(a.items[i + 1].t - a.items[i].t) * toMs;
				if (b == bNone) continue;
				ms[b] += d;
				if (b == bAI1Wait || b == bAI2Wait) waits += d;
			}
			const double total = (double)(a.items[count - 1].t - a.items[0].t) * toMs - waits;
			ms[list == kAI2 ? bAI2Busy : bAI1Busy] += total > 0.0 ? total : 0.0;
		}
		if (OnFrameBuckets) OnFrameBuckets(ms, kBucketCount);
		if (GpuTimer::Enabled) {
			for (UInt32 b = 0; b < kBucketCount; b++) if (Timers[b]) Timers[b]->Add(ms[b]);
			FramesSummed++;
		}
	}

	static void InstallAI();

	static void FrameStart(LONGLONG t) {
		if (!AIInstalled) InstallAI();
		if (GpuTimer::Enabled || OnFrameBuckets) Sum(t);
		for (UInt32 list = 0; list < kListCount; list++) Lists[list].count = 0;
		const LONGLONG start = Now();
		LastOverhead = start - t;
		MainStamps[0].id = kAnchor;
		MainStamps[0].t = start;
		Lists[kMain].count = 1;
	}

	static bool InExe(UInt32 address) {
		static UInt32 base = 0, end = 0;
		if (!base) {
			HMODULE exe = GetModuleHandleA(NULL);
			const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)exe;
			const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)((const UInt8*)exe + dos->e_lfanew);
			base = (UInt32)exe;
			end = base + nt->OptionalHeader.SizeOfImage;
		}
		return address >= base && address < end;
	}

	static void ModuleName(UInt32 address, char* out, size_t size) {
		HMODULE m = NULL;
		char path[MAX_PATH] = "?";
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)address, &m) && m)
			GetModuleFileNameA(m, path, MAX_PATH);
		const char* base = strrchr(path, '\\');
		sprintf_s(out, size, "%s", base ? base + 1 : path);
	}

	// The common stub: [esp] = site number (pushed by the site's stub), [esp+4] = the game's return address.
	//   pushad / push [esp+32] / call OnStamp / add esp,4 / mov [esp+32],eax / popad / ret  (= jmp target, return address on top)
	static bool BuildCode() {
		Code = (UInt8*)VirtualAlloc(NULL, 64 + 16 * kSiteCount, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
		if (!Code) return false;
		UInt8* p = Code;
		*p++ = 0x60;
		*p++ = 0xFF; *p++ = 0x74; *p++ = 0x24; *p++ = 0x20;
		*p++ = 0xE8; *(UInt32*)p = (UInt32)&OnStamp - (UInt32)(p + 4); p += 4;
		*p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;
		*p++ = 0x89; *p++ = 0x44; *p++ = 0x24; *p++ = 0x20;
		*p++ = 0x61;
		*p++ = 0xC3;
		for (UInt32 i = 0; i < kSiteCount; i++) {
			UInt8* stub = Code + 64 + 16 * i;
			stub[0] = 0x68; *(UInt32*)(stub + 1) = i;			// push i
			stub[5] = 0xE9; *(UInt32*)(stub + 6) = (UInt32)Code - (UInt32)(stub + 10);	// jmp common
		}
		FlushInstructionCache(GetCurrentProcess(), Code, 64 + 16 * kSiteCount);
		return true;
	}

	// Byte-check one site and point it at its stub. Returns false (and says why) when it is left alone.
	static bool PatchSite(UInt32 i, char* note, size_t noteSize) {
		const Site& s = Sites[i];
		UInt8* at = (UInt8*)s.site;
		note[0] = 0;
		if (at[0] != 0xE8) { sprintf_s(note, noteSize, "%08X not a call (byte %02X)", s.site, at[0]); return false; }
		const UInt32 target = s.site + 5 + *(UInt32*)(at + 1);
		if (s.expected && target != s.expected) {
			if (InExe(target)) { sprintf_s(note, noteSize, "%08X calls %08X inside the game, expected %08X", s.site, target, s.expected); return false; }
			char module[MAX_PATH];
			ModuleName(target, module, sizeof(module));
			if (!strcmp(module, "?")) {
				// Not in a module: a hook's stub in memory it allocated (FNVProbe's call-site stubs, trampolines). Chained only when
				// that memory is committed and executable.
				MEMORY_BASIC_INFORMATION mbi = {};
				const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
				if (!VirtualQuery((LPCVOID)target, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || !(mbi.Protect & exec)) {
					sprintf_s(note, noteSize, "%08X calls %08X in no module and not executable memory", s.site, target);
					return false;
				}
				strcpy_s(module, "a hook stub");
			}
			sprintf_s(note, noteSize, "%08X via %s", s.site, module);	// chained through another plugin's hook
		}
		Targets[i] = target;
		const UInt32 stub = (UInt32)(Code + 64 + 16 * i);
		DWORD old = 0;
		if (!VirtualProtect(at + 1, 4, PAGE_EXECUTE_READWRITE, &old)) { sprintf_s(note, noteSize, "%08X not writable", s.site); return false; }
		*(UInt32*)(at + 1) = stub - (s.site + 5);
		VirtualProtect(at + 1, 4, old, &old);
		FlushInstructionCache(GetCurrentProcess(), at, 5);
		Patched[i] = true;
		return true;
	}

	static void PatchList(bool ai) {
		unsigned done = 0, chained = 0, refused = 0;
		std::string chainedText, refusedText;
		for (UInt32 i = 0; i < kSiteCount; i++) {
			if ((Sites[i].list != kMain) != ai) continue;
			char note[160];
			if (PatchSite(i, note, sizeof(note))) {
				done++;
				if (note[0]) { chained++; if (chainedText.size() < 600) { chainedText += " "; chainedText += note; } }
			}
			else { refused++; if (refusedText.size() < 900) { refusedText += " "; refusedText += note; } }
		}
		// (2026-11-25 hook cleanup: "a hook already there" can be VS's own, e.g. Menu3DGuard 008703BC and DoFDepthGuard 00870ED0: the
		// module after "via" says whose.)
		Logger::Log("CPU SPLIT: %s call sites timed: %u (%u through a hook already there:%s), left alone: %u%s%s", ai ? "AI thread" : "main thread",
			done, chained, chainedText.c_str(), refused, refused ? " -" : "", refusedText.c_str());
	}

	// Frame start (main thread), the AI threads parked: their sites can be pointed at the stubs safely.
	static void InstallAI() {
		AIInstalled = true;
		PatchList(true);
	}

	// NVR's render hook, the first frame F10 profiling is on.
	static void Install() {
		if (Tried) return;
		Tried = true;
		LARGE_INTEGER f; QueryPerformanceFrequency(&f); Frequency = f.QuadPart;
		MainThread = GetCurrentThreadId();
		if (!BuildCode()) { Logger::Log("CPU SPLIT: no memory for the stubs; the game update split is not available"); return; }
		for (UInt32 b = 0; b < kBucketCount; b++) Timers[b] = new CpuTimer(BucketNames[b]);
		PatchList(false);
		MainInstalled = true;
		Logger::Log("CPU SPLIT: on (diagnostic, F10 only). 'Upd'/'Swp'/'Post' lines are the game's own main-thread frame cut at its calls, 'Rnd' the "
			"world render (Vegas Shaders' passes inside), 'AI' the two AI threads (they run beside the main thread; 'AI join wait' is the main thread "
			"waiting for them). Per-frame averages; the main-thread lines add up to the frame interval.");
	}

	// ---------------------------------------------------------------------------------------------------------------
	// TEST ONLY helper for the shot lists' 'affinity' command (SettingShots.h): how many of the process's threads may run outside
	// the process mask after SetProcessAffinityMask (expected 0: Windows moves every thread to the new mask).
	typedef LONG (NTAPI* NtQueryInformationThreadFn)(HANDLE, int, PVOID, ULONG, PULONG);
	struct ThreadBasicInformation { LONG ExitStatus; PVOID Teb; HANDLE Process, Thread; ULONG_PTR AffinityMask; LONG Priority, BasePriority; };
	static void CountThreadsOutside(DWORD_PTR mask, unsigned& threads, unsigned& outside) {
		threads = outside = 0;
		static NtQueryInformationThreadFn query = (NtQueryInformationThreadFn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		if (snap == INVALID_HANDLE_VALUE) return;
		THREADENTRY32 e = { sizeof(e) };
		const DWORD pid = GetCurrentProcessId();
		for (BOOL ok = Thread32First(snap, &e); ok; ok = Thread32Next(snap, &e)) {
			if (e.th32OwnerProcessID != pid) continue;
			threads++;
			HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, e.th32ThreadID);
			if (!h) continue;
			ThreadBasicInformation info = {};
			if (query && query(h, 0, &info, sizeof(info), NULL) >= 0 && (info.AffinityMask & ~mask)) outside++;
			CloseHandle(h);
		}
		CloseHandle(snap);
	}
}
