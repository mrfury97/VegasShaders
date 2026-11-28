#include "Hooks.h"
#include "../../core/PointShadowForward.h"
#include "../../core/ClothWind.h"	// UNOFFICIAL 2026-10-19 cloth round 5: the partition draw hook
#include "../../core/ParticleSize.h"	// UNOFFICIAL 2026-11-04 soft particles
#include "../../core/GrassTiming.h"	// UNOFFICIAL 2026-11-09 grass-cache study (test builds, GrassTiming key)
#include "../../core/DeferredDistant.h"	// UNOFFICIAL 2026-11-14 DeferredDistant: the RenderGeometryGroup detour
#include "../../core/BoneMatrixBatch.h"	// UNOFFICIAL 2026-11-24 batched bone-matrix uploads
#include "../../core/ConstantMapSort.h"	// UNOFFICIAL 2026-11-24 refcount-free constant-map partition
#include "../../core/DistantLandTier2.h"	// UNOFFICIAL 2026-11-25 Phase 4 Distant Land Tier-2 load suppression

void AttachScopeCameraHooks();	// Render.cpp (UNOFFICIAL 2026-11-12 [Main.Main.Scope] Detect: a plugin's extra-camera scene of the world)

namespace {
	// One Detours redirection: the game function pointer variable and the function that replaces it.
	struct Redirection {
		const char*	name;
		PVOID*	original;
		PVOID	replacement;
	};
	#define NVR_REDIRECT(name) Redirection{ #name, &(PVOID&)name, (PVOID)&name##Hook }

	// UNOFFICIAL 2026-11-19: attach and commit each detour in its own transaction and check return codes.
	// Bundling ~25 detours into one unchecked transaction let a single incompatible inline patch from another
	// plugin (e.g. NVAC) silently roll back InitializeRenderer and every other core hook, leaving
	// TheRenderManager / TheShaderManager null while raw WriteRelCall / vtable hooks still ran and crashed.
	bool AttachOne(const Redirection& r) {
		const PVOID target = *r.original;
		NoteHookSite((SIZE_T)target, 8, r.name);	// 2026-11-24 hook inventory: what the function's first bytes were before VS
		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		const LONG attached = DetourAttach(r.original, r.replacement);
		const LONG committed = attached == NO_ERROR ? DetourTransactionCommit() : (DetourTransactionAbort(), attached);
		if (committed != NO_ERROR) {
			const UInt8* b = (const UInt8*)target;
			Logger::Log("[ERROR] AttachHooks: detour failed for %s (%08X, first bytes %02X %02X %02X %02X, attach %ld, commit %ld) -- another plugin (e.g. NVAC) may have patched this function",
				r.name, (UInt32)target, b ? b[0] : 0, b ? b[1] : 0, b ? b[2] : 0, b ? b[3] : 0, attached, committed);
			return false;
		}
		return true;
	}

	bool Redirect(std::initializer_list<Redirection> list) {
		bool allOk = true;
		for (const Redirection& r : list) {
			if (!AttachOne(r)) allOk = false;
		}
		return allOk;
	}

	template <typename Fn>
	bool RunDetourGroup(const char* name, Fn&& fn) {
		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		fn();
		const LONG committed = DetourTransactionCommit();
		if (committed != NO_ERROR) {
			Logger::Log("[ERROR] AttachHooks: DetourTransactionCommit failed for %s (err %ld)", name, committed);
			return false;
		}
		return true;
	}

	// A patched call or jump instruction in the game's code.
	enum class Branch { Call, Jump };
	struct CodePatch {
		Branch	kind;
		UInt32	site;
		UInt32	target;
	};

	void Patch(std::initializer_list<CodePatch> list) {
		for (const CodePatch& p : list) {
			if (p.kind == Branch::Call) WriteRelCall(p.site, p.target);
			else WriteRelJump(p.site, p.target);
		}
	}

	// 2026-11-25 hook cleanup batch 2 (docs/next/HOOK-INVENTORY.md): patches in the middle of game functions are checked first. The
	// game's own bytes at each site come from the HOOKSITE log of 2026-11-24 (a bench slot with JIP, NVTF, Johnny Guitar and Stewie's
	// Tweaks loaded before VS) or, for sites that run did not write, from the dumped exe. A site that holds neither the game's bytes
	// nor VS's own (written already) was changed by another plugin: a fix every player has keeps being written, with a warning
	// (IfChanged::WriteAnyway, as before this check); an optional or debug patch is left out (IfChanged::Skip), so the other
	// plugin's code keeps working.
	enum class IfChanged { WriteAnyway, Skip };

	bool SiteIs(UInt32 site, std::initializer_list<UInt8> game, const UInt8* ours, size_t oursCount, const char* what, IfChanged mode) {
		const UInt8* now = (const UInt8*)site;
		if (!memcmp(now, game.begin(), game.size())) return true;
		if (ours && !memcmp(now, ours, oursCount)) return true;	// VS's own patch is already there
		char found[3 * 8 + 1] = {};
		for (size_t i = 0; i < 8; ++i) sprintf_s(found + 3 * i, 4, "%02X ", now[i]);
		Logger::Log("[WARN] AttachHooks: %s at %08X is not the game's code (found %s; another plugin?): %s", what, site, found,
			mode == IfChanged::Skip ? "left alone, this VS patch is off" : "VS's patch is written anyway (as VS always did)");
		return mode == IfChanged::WriteAnyway;
	}

	// A call or jump written over `game` (its first 5 bytes or fewer) only after the check.
	void PatchChecked(const CodePatch& p, std::initializer_list<UInt8> game, const char* what, IfChanged mode) {
		UInt8 ours[5] = { UInt8(p.kind == Branch::Call ? 0xE8 : 0xE9) };
		const UInt32 rel = p.target - (p.site + 5);
		memcpy(ours + 1, &rel, 4);
		if (SiteIs(p.site, game, ours, 5, what, mode)) Patch({ p });
	}

	void Write8Checked(UInt32 site, UInt8 game, UInt8 value, const char* what, IfChanged mode) {
		if (SiteIs(site, { game }, &value, 1, what, mode)) SafeWrite8(site, value);
	}

	void Write16Checked(UInt32 site, std::initializer_list<UInt8> game, UInt16 value, const char* what, IfChanged mode) {
		if (SiteIs(site, game, (const UInt8*)&value, 2, what, mode)) SafeWrite16(site, value);
	}

	// An existing call retargeted: only over a call instruction.
	void CallChecked(UInt32 site, UInt32 target, const char* what) {
		if (*(const UInt8*)site == 0xE8) WriteRelCall(site, target);
		else Logger::Log("[WARN] AttachHooks: %s at %08X is not a call (byte %02X; another plugin?): left alone, this VS patch is off", what, site, *(const UInt8*)site);
	}

	// BSShaderManager::GetFogProperty (0x00B55520, cdecl, 18 bytes): the fog the renderer and Atmosphere::Update ask for. VS's
	// version (ShadowSceneNode__GetFogPropertyEx) answers instead: no game fog while VolumetricFog draws the fog. 2026-11-24 hook
	// cleanup: one Detours hook on the function instead of rewriting its 14 calls inside 9 shader functions (they are all its
	// callers in the exe; a plugin calling it now gets the same answer). The calls stay as the fallback if the hook fails.
	BSFogProperty* (__cdecl* GetFogProperty)(UInt32) = (BSFogProperty* (__cdecl*)(UInt32))0x00B55520;
	const UInt32 kFogPropertyCallers[] = {
		0x6335EE, 0xB795FA, 0xB7AE86, 0xB7B539, 0xB7C3AB, 0xB86738, 0xBAA43B,
		0xBB1B5B, 0xBB1FA5, 0xBB670E, 0xBBDF26, 0xBBE3EC, 0xBC6E33, 0xBD4BED,
	};

	// 2026-11-24 hook cleanup: the game allocates VS's bigger versions of some objects (the size is the imm32 of the `push` before
	// the allocation call). Never shrink: if another plugin already asks for more, keep its size and say so.
	void GrowAllocation(UInt32 operand, UInt32 size, const char* what) {
		const UInt32 found = *(const UInt32*)operand;
		if (*(const UInt8*)(operand - 1) != 0x68) {
			Logger::Log("[WARN] AttachHooks: %s size at %08X is not a push imm32 (byte %02X); not changed", what, operand, *(const UInt8*)(operand - 1));
			return;
		}
		if (found >= size) {
			Logger::Log("UNOFFICIAL AttachHooks: %s size at %08X is already 0x%X (VS needs 0x%X; another plugin?); kept", what, operand, found, size);
			return;
		}
		SafeWrite32(operand, size);
	}
}

void AttachHooks() {
	SettingsMainStruct* settings = &TheSettingManager->SettingsMain;

	// --- function detours (isolated transactions so one plugin conflict cannot roll back InitializeRenderer) ---
	if (!AttachOne(NVR_REDIRECT(InitializeRenderer))) {
		Logger::Log("[ERROR] AttachHooks: InitializeRenderer could not be hooked -- skipping remaining hooks/patches to prevent null-manager crashes (check for NVAC or conflicting plugins)");
		return;
	}
	Redirect({
		NVR_REDIRECT(ReadSetting), NVR_REDIRECT(WriteSetting),			// INI reads / writes
		NVR_REDIRECT(NewMain),											// startup: main object
		NVR_REDIRECT(NewTES), NVR_REDIRECT(NewPlayerCharacter), NVR_REDIRECT(NewSceneGraph),
		NVR_REDIRECT(NewMainDataHandler), NVR_REDIRECT(NewMenuInterfaceManager), NVR_REDIRECT(NewQueuedModelLoader),
		NVR_REDIRECT(CreateVertexShader), NVR_REDIRECT(CreatePixelShader), NVR_REDIRECT(SetShaderPackage),	// shader loading
		NVR_REDIRECT(Render), NVR_REDIRECT(ProcessImageSpaceShaders), NVR_REDIRECT(RenderWorldSceneGraph),	// the frame
		NVR_REDIRECT(RenderShadowMap),
		NVR_REDIRECT(RenderBlended),   // UNOFFICIAL sandport: glass writes no depth in a sandstorm (ThroughBlended)
		NVR_REDIRECT(RenderFirstPerson), NVR_REDIRECT(SetShaders), NVR_REDIRECT(SetSamplerState),
	});
	// UNOFFICIAL 2026-11-23 RenderScaleGuard: catch mid-frame ImageSpaceManager::RenderEffect calls (e.g. DoF-Fix.dll) while RenderScale v2 is pending
	if (!AttachOne(NVR_REDIRECT(RenderImageSpaceEffect))) {
		Logger::Log("[WARN] AttachHooks: RenderImageSpaceEffect (0x00B97550) could not be hooked; mid-frame ImageSpace guard inactive");
	}
	if (!RunDetourGroup("GrassFirst", [] { AttachGrassFirstRenderPassHook(); })) ClearGrassFirstInstalled();	// UNOFFICIAL 2026-10-17 GrassFirst (Render.cpp): pass-through unless DrawFirst is on
	if (!RunDetourGroup("DeferredDistant", [] { DeferredDistant::Attach(); })) DeferredDistant::Installed = false;	// UNOFFICIAL 2026-11-14 [Main.Main.Quality] DeferredDistant
	if (!RunDetourGroup("DistantLandTier2", [] { DistantLandTier2::Attach(); })) DistantLandTier2::Installed = false;	// UNOFFICIAL 2026-11-25 [Main.Main.Quality] DistantLandSuppressVanilla
	if (!RunDetourGroup("ScopeCamera", [] { AttachScopeCameraHooks(); })) ClearScopeCameraHooks();	// UNOFFICIAL 2026-11-12 [Main.Main.Scope] Detect (Render.cpp ScopeScene)
	LampReachMargin = (std::max)(0.0f, TheSettingManager->GetSettingF("Main.Main.Misc", "LampReachMargin"));
	if (LampReachMargin > 0.0f) {	// UNOFFICIAL, Render.cpp
		const bool ok = AttachOne(NVR_REDIRECT(BoundWithinLightRadius));
		Logger::Log("UNOFFICIAL LampReachMargin %.0f: hook %s, first bytes %02X %02X %02X", LampReachMargin, ok ? "attached" : "FAILED",
			((const UInt8*)0x00B9DAE0)[0], ((const UInt8*)0x00B9DAE0)[1], ((const UInt8*)0x00B9DAE0)[2]);
	}
	// UNOFFICIAL per-object pass hooks: always installed (no gate gap for DeferredRenderer, DeferredTransparents, RmaosDraw,
	// WaterMaskDraw, GameEyes, BarrelHeat, FlashlightLamp, EnvLightFade, NpcCharacterMaps, LampsInOnePass, FoldFog, FoldHighlights,
	// ManyLamps, Cloth, Plants, PipboyFit, ShadowsInteriors, HiddenObjectCulling, LampScissor, MaterialRules, LampsDepthFirst, TAA).
	Redirect({ NVR_REDIRECT(RenderPassStandard), NVR_REDIRECT(RenderPassSkinned) });
	Redirect({
		NVR_REDIRECT(GetWaterHeightLOD),
		NVR_REDIRECT(IsWaterNodeVisible),   // always: water with no child nodes (crash in the reflection pass; ported from Balebandro/Physically-Based-Reloaded 1ce5d62)
		// in even when ForceReflections is off, so the reflection pass can be profiled (RenderReflectionsHook decides what it changes)
		NVR_REDIRECT(RenderReflections),
		NVR_REDIRECT(RenderPipboy), NVR_REDIRECT(ShowDetectorWindow), NVR_REDIRECT(LoadForm),
	});
	if (settings->FlyCam.Enabled) Redirect({ NVR_REDIRECT(UpdateFlyCam) });
	GrassTiming::Install();	// UNOFFICIAL 2026-11-09, test builds only, off unless [Main.Main.Performance] GrassTiming > 0 (own Detours transaction)

	// --- the game's own shaders: per-draw constant updates ---
	kSkyShaderConstantsDetour.ReplaceVirtualFunc(0x10AFE94, SkyShader__UpdateConstants);
	// UNOFFICIAL the skin shader (Skin.h, Shaders.cpp): SkinShader's per-draw slots bind the FaceGen maps to the light-only face
	// passes, mute the engine's own face highlight passes and bind the skin blur's targets; interior faces take SkinShader's own
	// passes. Always installed: each does nothing while Vanilla Plus Skin's shaders are in use (SkinShaders::Active, decided
	// when the SKIN shaders load, after the VPS check in GameCommon.cpp).
	kSkinPrepareGeometryDetour.ReplaceVirtualFunc(0x10BB980 + 27 * 4, SkinShader__PrepareGeometryForRendering);
	kSkinPostGeometryDetour.ReplaceVirtualFunc(0x10BB980 + 35 * 4, SkinShader__PostGeometry);
	InstallFaceGenInteriorPatch();
	kPipboyPostProcessDetour.ReplaceVirtualFunc(0x10780AC + 20 * 4, FOPipboyManager__RenderPostProcess);	// UNOFFICIAL 2026-11-04: raindrops on the Pip-Boy screen (HudRaindrops)

	// UNOFFICIAL KeepLampsInView (PointShadowForward.h): the game's one lamp view test call, with forward shadows, unless another
	// plugin has taken it.
	{
		const UInt32 site = PointShadowForward::kTestFrustumCallSite;
		if (*(UInt8*)site == 0xE8 && site + 5 + *(SInt32*)(site + 1) == PointShadowForward::kTestFrustumCull) {
			WriteRelCall(site, (UInt32)&PointShadowForward::TestFrustumCullHook);
			Logger::Log("UNOFFICIAL KeepLampsInView: the game's lamp view test (ShadowSceneNode::PreOnVisible) is checked against the render camera");
		}
		else Logger::Log("UNOFFICIAL KeepLampsInView: the call at %08X is not the game's lamp view test (another plugin?); not hooked", site);
	}

	InstallGrassFirstHooks();	// UNOFFICIAL 2026-10-17 GrassFirst (Render.cpp): pass-through unless [Shaders.Grass.Main] DrawFirst = true
	// UNOFFICIAL 2026-11-04 ParticleSize (core/ParticleSize.h): each particle's size on its vertices, for soft particles; lit particles on at startup only.
	if (TheSettingManager->GetSettingI("Shaders.Particles.Status", "Enabled")) { ParticleSize::Install(); InstallSoftParticleHook(); }
	// UNOFFICIAL 2026-10-19 cloth round 5 (core/ClothWind.h DrawPartition): the game's two per-partition draw calls in
	// BSBatchRenderer::RenderPassImmediately_Skinned, so worn cloth can have its per-vertex weights in vertex stream 1. Only when the
	// cloth code is compiled in ([Shaders.Cloth.Main] Enabled at startup) and both calls are the game's; otherwise mask 4 stays.
	// UNOFFICIAL 2026-11-03 PipboyFit (core/PipboyFit.h) sends its constants from the same partition draw, so either one hooks it.
	const bool clothOn = TheSettingManager->GetSettingI("Shaders.Cloth.Main", "Enabled") != 0;
	if (!clothOn && TheSettingManager->GetSettingI("Shaders.PipboyFit.Main", "Enabled")) {
		auto isDrawPartition = [](UInt32 site) { return *(UInt8*)site == 0xE8 && site + 5 + *(SInt32*)(site + 1) == 0x00E6D310; };
		if (isDrawPartition(0x00B992E3) && isDrawPartition(0x00B99339)) {
			WriteRelCall(0x00B992E3, (UInt32)&ClothWind::DrawPartitionHook);
			WriteRelCall(0x00B99339, (UInt32)&ClothWind::DrawPartitionHook);
			PipboyFit::HookOn = true;
			Logger::Log("UNOFFICIAL PipboyFit: the skinned partition draws are hooked");
		}
		else Logger::Log("UNOFFICIAL PipboyFit: the calls at 00B992E3 / 00B99339 are not the game's partition draw (another plugin?); sleeves are not fitted");
	}
	if (clothOn) {
		auto isDrawPartition = [](UInt32 site) { return *(UInt8*)site == 0xE8 && site + 5 + *(SInt32*)(site + 1) == 0x00E6D310; };
		if (isDrawPartition(0x00B992E3) && isDrawPartition(0x00B99339)) {
			WriteRelCall(0x00B992E3, (UInt32)&ClothWind::DrawPartitionHook);
			WriteRelCall(0x00B99339, (UInt32)&ClothWind::DrawPartitionHook);
			ClothWind::DrawHookOn = true;
			PipboyFit::HookOn = true;
			Logger::Log("UNOFFICIAL cloth wind: the skinned partition draws are hooked (per-vertex weights of worn cloth, [Shaders.Cloth.Main] UseWeights)");
		}
		else Logger::Log("UNOFFICIAL cloth wind: the calls at 00B992E3 / 00B99339 are not the game's partition draw (another plugin?); worn cloth keeps the skeleton mask");
		// UNOFFICIAL 2026-10-20 still cloth (core/ClothWind.h OnlyRender): the renderer's still-object draws (NiDX9Renderer vtable 0x10EE4BC
		// + 0x1B4 Do_OnlyRenderShape, + 0x1B8 Do_OnlyRenderTristrips; what the geometry's OnlyRenderImmediate calls after the pass set its
		// shaders, streams and declaration), so still cloth can have its per-vertex weights in vertex stream 1. Whatever sits in the slots
		// is called through (another plugin's patch keeps working).
		const UInt32 shape = *(UInt32*)0x010EE670, strips = *(UInt32*)0x010EE674;
		if (shape && strips) {
			ClothWind::OnlyRenderShapeOriginal = (ClothWind::OnlyRenderFn)shape;
			ClothWind::OnlyRenderStripsOriginal = (ClothWind::OnlyRenderFn)strips;
			SafeWrite32(0x010EE670, (UInt32)&ClothWind::OnlyRenderShapeHook);
			SafeWrite32(0x010EE674, (UInt32)&ClothWind::OnlyRenderStripsHook);
			ClothWind::StillHookOn = true;
			Logger::Log("UNOFFICIAL cloth wind: the still-object draws are hooked (%08X %08X; per-vertex weights of still cloth, [Shaders.Cloth.Main] StillWeights)", shape, strips);
		}
	}

	// UNOFFICIAL 2026-11-07 perf-mirrorfix2 (Render.cpp MaterialDraw): on top of the cloth / Pip-Boy fit hooks above, calling on them
	InstallMaterialDrawHooks();

	// --- engine patches ---
	PatchChecked({ Branch::Call, 0xBE0B73, (UInt32)&NiD3DVertexShaderEx::Free },
		{ 0xE8, 0xF8, 0x8F, 0x2A, 0x00 }, "vertex shader free (NiD3DVertexShader)", IfChanged::WriteAnyway);
	PatchChecked({ Branch::Call, 0xBE0AF3, (UInt32)&NiD3DPixelShaderEx::Free },
		{ 0xE8, 0x78, 0x8E, 0x2A, 0x00 }, "pixel shader free (NiD3DPixelShader)", IfChanged::WriteAnyway);
	// the game allocates NVR's bigger versions of these objects
	GrowAllocation(0x00E7624D, sizeof(RenderManager), "RenderManager (NiDX9Renderer::Create)");
	GrowAllocation(0x00466606, sizeof(TESWeatherEx), "TESWeather (CreateFormFromID)");
	GrowAllocation(0x0046CF9B, sizeof(TESWeatherEx), "TESWeather (GenerateDefaultObjects)");
	Write8Checked(0xB575AA, 0x74, 0x75, "shader package kept (BSShaderManager::Initialize)", IfChanged::WriteAnyway);	// the shader package is never destroyed

	// UNOFFICIAL MoreLampsPerObject (2026-10-13): BSShaderPPLightingProperty::AddPass_Opt (0x00BDE9B0) gives a specular
	// many-lamp pass (0xD0, 0xD1: most lit surfaces indoors) at most 3-4 entries, the first light and 2-3 lamps (ADD ECX,2 at
	// 0x00BDEA3D; the others get 5-6 or 6-7) and drops every lamp past that, with no extra pass for them. The list is sorted by
	// brightness and lamps go in and out of view, so which 2-3 an object kept changed as the player moved: a wall piece next
	// to a lamp lit by it, then not. ADD ECX,5: 6-7 entries, as the other many-lamp passes; the shaders take 6
	// (SPECULAR_LAMPS6, ShaderRecord.cpp). Written only if the bytes are the game's.
	if (TheSettingManager->GetSettingI("Main.Main.Misc", "MoreLampsPerObject")) {
		const UInt8* site = (const UInt8*)0x00BDEA3D;
		if (site[0] == 0x83 && site[1] == 0xC1 && site[2] == 0x02) {
			SafeWrite8(0x00BDEA3F, 0x05);
			Logger::Log("UNOFFICIAL MoreLampsPerObject: specular many-lamp passes take up to 6 lamps (the game: 3)");
		}
		else Logger::Log("UNOFFICIAL MoreLampsPerObject: the game's code at 00BDEA3D is not as expected (another plugin?); not changed");
	}
	Write8Checked(0x008751C0, 0x04, 0x00, "first-person depth keep (DrawWorld_MTRender1stPerson)", IfChanged::WriteAnyway);
	// no pause on ALT-TAB: WinProc's WA_INACTIVE branch (je) is skipped. Not the game's bAlwaysActive: that one still opens the pause menu.
	Write16Checked(0x0086A170, { 0x74, 0x57 }, 0x9090, "no pause on Alt-Tab (WinProc)", IfChanged::WriteAnyway);

	// the detector window (debug scene tree): debug only, so left out if another plugin changed these places
	PatchChecked({ Branch::Jump, Jumpers::DetectorWindow::CreateTreeViewHook, (UInt32)&DetectorWindowCreateTreeViewHook },
		{ 0x8B, 0x4D, 0xB8, 0x89, 0x41 }, "detector window (NiTreeCtrl::CreateWindows)", IfChanged::Skip);
	PatchChecked({ Branch::Jump, Jumpers::DetectorWindow::DumpAttributesHook, (UInt32)&DetectorWindowDumpAttributesHook },
		{ 0xFF, 0x15, 0x8C, 0xF2, 0xFD }, "detector window (NiTreeCtrl::CreateTreeRecursive)", IfChanged::Skip);
	PatchChecked({ Branch::Jump, Jumpers::DetectorWindow::ConsoleCommandHook, (UInt32)&DetectorWindowConsoleCommandHook },
		{ 0xC6, 0x05, 0xC1, 0x9F, 0x1F }, "detector window (DrawWorld_Init)", IfChanged::Skip);
	CallChecked(Jumpers::DetectorWindow::SetNodeName, (UInt32)&DetectorWindowSetNodeName, "detector window node names");
	kRenderInterfaceDetour.ReplaceCall(Jumpers::RenderInterface::Hook, RenderInterfaceHook);
	InstallMenu3DGuardHook();
	InstallDoFDepthGuardHooks();
	BoneMatrixBatch::Enabled = settings->Main.BatchBoneUploads;
	BoneMatrixBatch::Install();
	ConstantMapSort::Enabled = settings->Main.FastConstantMapSort;
	ConstantMapSort::Install();
	PatchChecked({ Branch::Jump, Jumpers::Shadows::LeavesNodeNameHook, (UInt32)&LeavesNodeNameHook },
		{ 0x89, 0x45, 0x90, 0xEB, 0x07 }, "tree leaves node name (BSTreeModel::CreateArt)", IfChanged::WriteAnyway);
	PatchChecked({ Branch::Call, 0x00875B86, 0x00710AB0 },
		{ 0xE8, 0x75, 0xED, 0xFF, 0xFF }, "world FOV restore 1 (DrawWorld_MTRender1stPerson)", IfChanged::WriteAnyway);
	PatchChecked({ Branch::Call, 0x00875B9D, 0x00710AB0 },
		{ 0xE8, 0x5E, 0xED, 0xFF, 0xFF }, "world FOV restore 2 (DrawWorld_MTRender1stPerson)", IfChanged::WriteAnyway);
	PatchChecked({ Branch::Jump, 0x004E4C3B, 0x004E4C42 }, { 0x0F, 0xB6, 0xD0, 0x85, 0xD2 },	// reflections where the cell's water height differs from the worldspace's
		"water reflections (TESWaterSystem::AddPlaceableWater)", IfChanged::WriteAnyway);
	PatchChecked({ Branch::Jump, 0x004E4DA4, 0x004E4DAC }, { 0x0F, 0xB6, 0x4D, 0x18, 0x85 },	// reflections on distant water
		"distant water reflections (TESWaterSystem::AddLODWater)", IfChanged::WriteAnyway);
	// ImageSpaceShader::Render's own SetupPassShaders call is skipped: RenderPassImmediately binds the same shaders again right after
	// (always, for image space), and this early call is the only one that would hand SetShadersHook a stale BSShaderManager::
	// pCurrentRenderPass (xFNV ImageSpaceShader::Render; audit hooks.md C1). SetShadersHook relies on it: never drop.
	PatchChecked({ Branch::Jump, 0x00C03F49, 0x00C03F5A }, { 0x8B, 0x06, 0x8B, 0x90, 0xF4 }, "image space shader bind (ImageSpaceShader::Render)", IfChanged::WriteAnyway);
	// save screenshots corrupted under DXVK with the HDR mod: the save's texture is made without the format upgrade. 2026-11-25: the hook
	// calls what the call called before (BSRenderedTexture::Create, or another plugin's hook there).
	if (*(const UInt8*)0x00879061 == 0xE8) kSaveTextureDetour.ReplaceCall(0x00879061, CreateSaveTextureHook);
	else Logger::Log("[WARN] AttachHooks: 00879061 is not a call (byte %02X; another plugin?): save screenshot fix not hooked", *(const UInt8*)0x00879061);
	// Muzzle flash lights culled with the flash (MuzzleFlash::Update's call at 0x9BB158). 2026-11-24 hook cleanup: only if it is a
	// call, and the hook calls whatever it called before (the game's 0x9BB8A0 or another plugin's hook) instead of a fixed address.
	if (*(const UInt8*)0x009BB158 == 0xE8) kMuzzleFlashDetour.ReplaceCall(0x009BB158, MuzzleLightCullingFix);
	else Logger::Log("[WARN] AttachHooks: 009BB158 is not a call (byte %02X; another plugin?); muzzle flash light fix not hooked", *(const UInt8*)0x009BB158);

	// The old project's own intro, menu video and menu music only with ReplaceIntro (off by default): otherwise the game's own
	// (2026-11-02: the music redirect used to be patched in always, so the menu played NVRMainMenu.mp3 even with it off).
	if (settings->Main.ReplaceIntro) {
		PatchChecked({ Branch::Jump, Jumpers::SetTileShaderConstants::Hook, (UInt32)&SetTileShaderConstantsHook },
			{ 0x80, 0xBE, 0xAC, 0x00, 0x00 }, "ReplaceIntro menu video (TileShader::SetupGeometryConstants)", IfChanged::Skip);
		CallChecked(Jumpers::MainMenuMusic::Fix1, (UInt32)&MainMenuMusicFix, "ReplaceIntro menu music (1)");
		CallChecked(Jumpers::MainMenuMusic::Fix2, (UInt32)&MainMenuMusicFix, "ReplaceIntro menu music (2)");
	}

	if (settings->Main.RemovePrecipitations) {
		PatchChecked({ Branch::Jump, 0x0063AFC4, 0x0063AFD8 }, { 0x74, 0x12, 0x51, 0xD9, 0x45 }, "RemovePrecipitations (Sky::Update)", IfChanged::Skip);
		PatchChecked({ Branch::Jump, 0x0063A5CB, 0x0063A5DE }, { 0x74, 0x11, 0x51, 0xD9, 0xEE }, "RemovePrecipitations (Sky::SetMode)", IfChanged::Skip);
	}

	if (!AttachOne(Redirection{ "GetFogProperty", &(PVOID&)GetFogProperty, (PVOID)&ShadowSceneNode__GetFogPropertyEx })) {
		Logger::Log("[WARN] AttachHooks: GetFogProperty (00B55520) could not be hooked; patching its 14 calls instead");
		for (UInt32 caller : kFogPropertyCallers) WriteRelCall(caller, (UInt32)&ShadowSceneNode__GetFogPropertyEx);
	}

	// End of frame: queued effect reloads run here. 2026-11-24 hook cleanup: the slot's previous entry is kept and called (the game's
	// Do_EndFrame 0xE69660, or another plugin's hook) instead of a fixed address that would cut another plugin off.
	kEndFrameDetour.ReplaceVirtualFunc(0x10EE63C, NiDX9Renderer__Do_EndFrame);

	if (settings->Main.RemoveUnderwater)	// the underwater fog's NiPoint2 is made (0, 0): only over the game's call
		PatchChecked({ Branch::Call, 0x4EC8EE, (UInt32)&WaterFogRemover }, { 0xE8, 0xCD, 0x64, 0xF6, 0xFF }, "RemoveUnderwater", IfChanged::Skip);

	if (settings->FlyCam.Enabled) {	// tfc movement keys: optional, left out if another plugin changed the movement code
		PatchChecked({ Branch::Jump, Jumpers::FlyCam::UpdateForwardFlyCamHook, (UInt32)&UpdateForwardFlyCamHook },
			{ 0xD9, 0x45, 0x80, 0xDC, 0x05 }, "FlyCam forward", IfChanged::Skip);
		PatchChecked({ Branch::Jump, Jumpers::FlyCam::UpdateBackwardFlyCamHook, (UInt32)&UpdateBackwardFlyCamHook },
			{ 0xD9, 0x45, 0x80, 0xDC, 0x25 }, "FlyCam backward", IfChanged::Skip);
		PatchChecked({ Branch::Jump, Jumpers::FlyCam::UpdateRightFlyCamHook, (UInt32)&UpdateRightFlyCamHook },
			{ 0xD9, 0x85, 0x7C, 0xFF, 0xFF }, "FlyCam right", IfChanged::Skip);
		PatchChecked({ Branch::Jump, Jumpers::FlyCam::UpdateLeftFlyCamHook, (UInt32)&UpdateLeftFlyCamHook },
			{ 0xD9, 0x85, 0x7C, 0xFF, 0xFF }, "FlyCam left", IfChanged::Skip);
	}
}
