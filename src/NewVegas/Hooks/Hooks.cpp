#include "Hooks.h"
#include "../../core/PointShadowForward.h"
#include "../../core/ClothWind.h"	// UNOFFICIAL 2026-10-19 cloth round 5: the partition draw hook

namespace {
	// One Detours redirection: the game function pointer variable and the function that replaces it.
	struct Redirection {
		PVOID*	original;
		PVOID	replacement;
	};
	#define NVR_REDIRECT(name) Redirection{ &(PVOID&)name, (PVOID)&name##Hook }

	void Redirect(std::initializer_list<Redirection> list) {
		for (const Redirection& r : list) DetourAttach(r.original, r.replacement);
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

	// Every place the renderer asks the shadow scene node for its fog: NVR's version answers instead (removes the vanilla fog).
	const UInt32 kFogPropertyCallers[] = {
		0x6335EE, 0xB795FA, 0xB7AE86, 0xB7B539, 0xB7C3AB, 0xB86738, 0xBAA43B,
		0xBB1B5B, 0xBB1FA5, 0xBB670E, 0xBBDF26, 0xBBE3EC, 0xBC6E33, 0xBD4BED,
	};
}

void AttachHooks() {
	SettingsMainStruct* settings = &TheSettingManager->SettingsMain;

	// --- function detours (one transaction) ---
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	Redirect({
		NVR_REDIRECT(ReadSetting), NVR_REDIRECT(WriteSetting),			// INI reads / writes
		NVR_REDIRECT(NewMain), NVR_REDIRECT(InitializeRenderer),		// startup: main object, renderer
		NVR_REDIRECT(NewTES), NVR_REDIRECT(NewPlayerCharacter), NVR_REDIRECT(NewSceneGraph),
		NVR_REDIRECT(NewMainDataHandler), NVR_REDIRECT(NewMenuInterfaceManager), NVR_REDIRECT(NewQueuedModelLoader),
		NVR_REDIRECT(CreateVertexShader), NVR_REDIRECT(CreatePixelShader), NVR_REDIRECT(SetShaderPackage),	// shader loading
		NVR_REDIRECT(Render), NVR_REDIRECT(ProcessImageSpaceShaders), NVR_REDIRECT(RenderWorldSceneGraph),	// the frame
		NVR_REDIRECT(RenderBlended),   // UNOFFICIAL sandport: glass writes no depth in a sandstorm (ThroughBlended)
		NVR_REDIRECT(RenderFirstPerson), NVR_REDIRECT(SetShaders), NVR_REDIRECT(SetSamplerState),
	});
	AttachGrassFirstRenderPassHook();	// UNOFFICIAL 2026-10-17 GrassFirst (Render.cpp): pass-through unless DrawFirst is on
	LampReachMargin = (std::max)(0.0f, TheSettingManager->GetSettingF("Main.Main.Misc", "LampReachMargin"));
	if (LampReachMargin > 0.0f) {	// UNOFFICIAL, Render.cpp
		const LONG attached = DetourAttach(&(PVOID&)BoundWithinLightRadius, &BoundWithinLightRadiusHook);
		Logger::Log("UNOFFICIAL LampReachMargin %.0f: hook %s (%ld), first bytes %02X %02X %02X", LampReachMargin, attached == NO_ERROR ? "attached" : "FAILED", attached,
			((const UInt8*)0x00B9DAE0)[0], ((const UInt8*)0x00B9DAE0)[1], ((const UInt8*)0x00B9DAE0)[2]);
	}
	// UNOFFICIAL interior forward shadows: each object's own lamps (PointShadowForward.h).
	// (Also for ScopeFix outdoors, HiddenObjectCulling, LampScissor and the material rules, which work on the same per-object call; their
	// switches are live only while this hook is in, i.e. with any of these on at startup.)
	if (TheSettingManager->GetSettingI("Shaders.ShadowsInteriors.Forward", "Enabled") ||
		TheSettingManager->GetSettingI("Shaders.ShadowsInteriors.Forward", "ScopeFix") ||
		settings->Main.HiddenObjectCulling || settings->Main.LampScissor ||
		TheSettingManager->GetSettingI("Shaders.PBR.MaterialRules", "Enabled") ||
		TheSettingManager->GetSettingI("Main.Main.Misc", "LampsDepthFirst")) {	// UNOFFICIAL 2026-10-26 LampsDepthFirst draws its depth step there
		Redirect({ NVR_REDIRECT(RenderPassStandard), NVR_REDIRECT(RenderPassSkinned) });
	}
	Redirect({
		NVR_REDIRECT(GetWaterHeightLOD),
		NVR_REDIRECT(IsWaterNodeVisible),   // always: water with no child nodes (crash in the reflection pass; ported from Balebandro/Physically-Based-Reloaded 1ce5d62)
		// in even when ForceReflections is off, so the reflection pass can be profiled (RenderReflectionsHook decides what it changes)
		NVR_REDIRECT(RenderReflections),
		NVR_REDIRECT(RenderPipboy), NVR_REDIRECT(ShowDetectorWindow), NVR_REDIRECT(LoadForm),
	});
	if (settings->FlyCam.Enabled) Redirect({ NVR_REDIRECT(UpdateFlyCam) });
	DetourTransactionCommit();

	// --- the game's own shaders: per-draw constant updates ---
	kSkyShaderConstantsDetour.ReplaceVirtualFunc(0x10AFE94, SkyShader__UpdateConstants);
	// UNOFFICIAL the skin shader (Skin.h, Shaders.cpp): SkinShader's per-draw slots bind the FaceGen maps to the light-only face
	// passes, mute the engine's own face highlight passes and bind the skin blur's targets; interior faces take SkinShader's own
	// passes. Always installed: each does nothing while Vanilla Plus Skin's shaders are in use (SkinShaders::Active, decided
	// when the SKIN shaders load, after the VPS check in GameCommon.cpp).
	kSkinPrepareGeometryDetour.ReplaceVirtualFunc(0x10BB980 + 27 * 4, SkinShader__PrepareGeometryForRendering);
	kSkinPostGeometryDetour.ReplaceVirtualFunc(0x10BB980 + 35 * 4, SkinShader__PostGeometry);
	InstallFaceGenInteriorPatch();

	// UNOFFICIAL KeepLampsInView (PointShadowForward.h): the game's one lamp view test call, with forward shadows, unless another
	// plugin has taken it.
	if (TheSettingManager->GetSettingI("Shaders.ShadowsInteriors.Forward", "Enabled")) {
		const UInt32 site = PointShadowForward::kTestFrustumCallSite;
		if (*(UInt8*)site == 0xE8 && site + 5 + *(SInt32*)(site + 1) == PointShadowForward::kTestFrustumCull) {
			WriteRelCall(site, (UInt32)&PointShadowForward::TestFrustumCullHook);
			Logger::Log("UNOFFICIAL KeepLampsInView: the game's lamp view test (ShadowSceneNode::PreOnVisible) is checked against the render camera");
		}
		else Logger::Log("UNOFFICIAL KeepLampsInView: the call at %08X is not the game's lamp view test (another plugin?); not hooked", site);
	}

	InstallGrassFirstHooks();	// UNOFFICIAL 2026-10-17 GrassFirst (Render.cpp): pass-through unless [Shaders.Grass.Main] DrawFirst = true
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

	// --- engine patches ---
	Patch({
		{ Branch::Call, 0xBE0B73, (UInt32)&NiD3DVertexShaderEx::Free },	// NVR's shader records are freed with the game's shaders
		{ Branch::Call, 0xBE0AF3, (UInt32)&NiD3DPixelShaderEx::Free },
	});
	// the game allocates NVR's bigger versions of these objects
	SafeWrite32(0x00E7624D, sizeof(RenderManager));
	SafeWrite32(0x00466606, sizeof(TESWeatherEx));
	SafeWrite32(0x0046CF9B, sizeof(TESWeatherEx));
	SafeWrite8(0xB575AA, 0x75);			// the shader package is never destroyed

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
	SafeWrite8(0x008751C0, 0);				// first-person rendering keeps the depth buffer (no clear)
	SafeWrite16(0x0086A170, 0x9090);		// no pause on ALT-TAB

	Patch({
		{ Branch::Jump, Jumpers::DetectorWindow::CreateTreeViewHook, (UInt32)&DetectorWindowCreateTreeViewHook },	// the detector window (debug scene tree)
		{ Branch::Jump, Jumpers::DetectorWindow::DumpAttributesHook, (UInt32)&DetectorWindowDumpAttributesHook },
		{ Branch::Jump, Jumpers::DetectorWindow::ConsoleCommandHook, (UInt32)&DetectorWindowConsoleCommandHook },
		{ Branch::Call, Jumpers::DetectorWindow::SetNodeName, (UInt32)&DetectorWindowSetNodeName },
	});
	kRenderInterfaceDetour.ReplaceCall(Jumpers::RenderInterface::Hook, RenderInterfaceHook);
	Patch({
		{ Branch::Jump, 0x871290, (UInt32)&RenderShadowMapHook },
		{ Branch::Jump, Jumpers::Shadows::LeavesNodeNameHook, (UInt32)&LeavesNodeNameHook },
		{ Branch::Jump, 0x004E4C3B, 0x004E4C42 },	// reflections where the cell's water height differs from the worldspace's
		{ Branch::Jump, 0x004E4DA4, 0x004E4DAC },	// reflections on distant water
		{ Branch::Call, 0x00875B86, 0x00710AB0 },	// the world field of view is set again after first-person rendering (2 sites)
		{ Branch::Call, 0x00875B9D, 0x00710AB0 },
		{ Branch::Jump, 0x00C03F49, 0x00C03F5A },	// image space effects rendered wrong
		{ Branch::Call, 0x9BB158, (UInt32)&MuzzleLightCullingFix },
		{ Branch::Call, 0x879061, (UInt32)&CreateSaveTextureHook },	// save screenshots corrupted under DXVK with the HDR mod
	});

	// The old project's own intro, menu video and menu music only with ReplaceIntro (off by default): otherwise the game's own
	// (2026-11-02: the music redirect used to be patched in always, so the menu played NVRMainMenu.mp3 even with it off).
	if (settings->Main.ReplaceIntro)
		Patch({ { Branch::Jump, Jumpers::SetTileShaderConstants::Hook, (UInt32)&SetTileShaderConstantsHook },
			{ Branch::Call, Jumpers::MainMenuMusic::Fix1, (UInt32)&MainMenuMusicFix },
			{ Branch::Call, Jumpers::MainMenuMusic::Fix2, (UInt32)&MainMenuMusicFix } });

	if (settings->Main.RemovePrecipitations)
		Patch({ { Branch::Jump, 0x0063AFC4, 0x0063AFD8 }, { Branch::Jump, 0x0063A5CB, 0x0063A5DE } });

	for (UInt32 caller : kFogPropertyCallers) WriteRelCall(caller, (UInt32)&ShadowSceneNode__GetFogPropertyEx);

	SafeWrite32(0x10EE63C, reinterpret_cast<UInt32>(&NiDX9Renderer__Do_EndFrame));	// end of frame: queued effect reloads run here

	if (settings->Main.RemoveUnderwater)
		WriteRelCall(0x4EC8EE, (UInt32)&WaterFogRemover);

	if (settings->FlyCam.Enabled) {
		Patch({
			{ Branch::Jump, Jumpers::FlyCam::UpdateForwardFlyCamHook, (UInt32)&UpdateForwardFlyCamHook },
			{ Branch::Jump, Jumpers::FlyCam::UpdateBackwardFlyCamHook, (UInt32)&UpdateBackwardFlyCamHook },
			{ Branch::Jump, Jumpers::FlyCam::UpdateRightFlyCamHook, (UInt32)&UpdateRightFlyCamHook },
			{ Branch::Jump, Jumpers::FlyCam::UpdateLeftFlyCamHook, (UInt32)&UpdateLeftFlyCamHook },
		});
	}
}
