#pragma once

// Game addresses NVR hooks, and the small game-specific bases of NVR's managers.

// Functions NVR detours (Detours) or calls through the original pointer.
class Hooks {	// function entry points
public:
	// INISettingCollection
	static const UInt32 ReadSetting = 0x00C44280, WriteSetting = 0x00C43E90;
	// constructors of the game singletons NVR keeps pointers to
	static const UInt32 NewMain = 0x0086C160, NewTES = 0x0044FB20, NewPlayerCharacter = 0x00938180, NewSceneGraph = 0x00878610;
	static const UInt32 NewMainDataHandler = 0x0045D270, NewMenuInterfaceManager = 0x0070A130, NewQueuedModelLoader = 0x00442650;
	// renderer set-up and shader loading
	static const UInt32 InitializeRenderer = 0x0086D500, CreateVertexShader = 0x00BE0FE0, CreatePixelShader = 0x00BE1750;
	static const UInt32 SetShaderPackage = 0x00B4F710, SetShaders = 0x00BE1F90, SetSamplerState = 0x00E910A0;
	// the frame
	static const UInt32 Render = 0x008706B0, RenderWorldSceneGraph = 0x00873200, RenderFirstPerson = 0x00875110;
	static const UInt32 RenderReflections = 0x004E1BC0, RenderPipboy = 0x008761E0, ProcessImageSpaceShaders = 0x00B55AC0;
	static const UInt32 RenderPassStandard = 0x00B98E80, RenderPassSkinned = 0x00B99110;	// BSBatchRenderer::RenderPassImmediately_* (cdecl)
	static const UInt32 RenderBlended = 0x00B650C0;		// BSShaderAccumulator, blended stage (back to front, after the opaque groups)
	static const UInt32 CreateRenderedTexture = 0x00B6B610;	// BSTextureManager
	// water, forms, tools
	static const UInt32 GetWaterHeightLOD = 0x0045CD80, IsWaterNodeVisible = 0x004E62E0, LoadForm = 0x004601D0;
	static const UInt32 ShowDetectorWindow = 0x004D61B0, UpdateFlyCam = 0x0094A8C0;
};

// Mid-function patch sites: where NVR's code is entered (Hook) and where it jumps back to the game (Return).
class Jumpers {	// patch sites inside functions
public:
	struct CreateDevice { static const UInt32 Hook = 0x00E731FF, Return = 0x00E73204; };
	struct DetectorWindow {	// the debug scene graph viewer
		static const UInt32 CreateTreeViewHook = 0x004D6476, CreateTreeViewReturn = 0x004D647F;
		static const UInt32 DumpAttributesHook = 0x004D6A2E, DumpAttributesReturn = 0x004D69C5;
		static const UInt32 ConsoleCommandHook = 0x00872D09, ConsoleCommandReturn = 0x00872D10;
		static const UInt32 SetNodeName = 0x004D678A;	// call site
	};
	struct RenderInterface { static const UInt32 Hook = 0x007144D3; };
	struct Shadows {	// shadow caster selection
		static const UInt32 RenderShadowMap1Hook = 0x0087084E;	// (patch currently disabled)
		static const UInt32 LeavesNodeNameHook = 0x0066A112, LeavesNodeNameReturn = 0x0066A11E;
	};
	struct SetTileShaderConstants { static const UInt32 Hook = 0x00BCAAD7, Return = 0x00BCAADE; };
	struct MainMenuMusic { static const UInt32 Fix1 = 0x007D0B48, Fix2 = 0x007D6CE7; };
	struct FlyCam {	// tfc movement
		static const UInt32 UpdateForwardFlyCamHook = 0x0094ABDF, UpdateForwardFlyCamReturn = 0x0094ABEB;
		static const UInt32 UpdateBackwardFlyCamHook = 0x0094ABFB, UpdateBackwardFlyCamReturn = 0x0094AC07;
		static const UInt32 UpdateRightFlyCamHook = 0x0094AC17, UpdateRightFlyCamReturn = 0x0094AC29;
		static const UInt32 UpdateLeftFlyCamHook = 0x0094AC39, UpdateLeftFlyCamReturn = 0x0094AC4B;
	};
};

class CommandManagerBase {	// CommandManager
public:
	// NVR's console commands start at opcode 0x3000 (its xNVSE opcode range).
	void RegisterCommands(const PluginInterface* nvse, void** executers, CommandInfo** commands, int count, void** papyrus) {
		nvse->SetOpcodeBase(0x3000);
		for (CommandInfo** command = commands; command != commands + count; ++command)
			nvse->RegisterCommand(*command);
	}
};

class SettingManagerBase {	// SettingManager
public:
	// Cloud layers 0 and 1 of a weather (HDR is not applied here).
	void SetTextureAndHDR(TESWeatherEx* weather, char* upperLayer, char* lowerLayer, float* hdr) {
		TESTexture* layers = weather->textureLayers;
		layers[0].ddsPath.Set(upperLayer);
		layers[1].ddsPath.Set(lowerLayer);
	}

	// Windowed mode: the game's window gets a borderless popup style (patched into its CreateWindow calls).
	void SetWindowedMode(UInt8 fullscreen) {
		if (fullscreen) return;
		const UInt32 borderless = WS_POPUP;
		SafeWrite32(0x0086AF04, borderless);
		SafeWrite32(0x0086AF2F, borderless | WS_VISIBLE);
	}
};

class RenderManagerBase : public NiDX9Renderer {	// NVR's view of the game's renderer
public:
	IDirect3DSurface9* CreateHDRRenderTarget() { return nullptr; }	// no separate HDR target on New Vegas
};

class ShaderManagerBase {	// nothing game-specific yet
};
