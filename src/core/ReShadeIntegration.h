#pragma once

// UNOFFICIAL: NVR as a ReShade add-on (ReShadeIntegration.cpp). With ReShade loaded, NVR hands ReShade its
// combined depth (the world and the first-person model in one buffer, in the game's own depth direction; with
// ReShade's D3D9 runtime, or since 2026-10-18 its Vulkan runtime under DXVK), draws ReShade's effects right after
// NVR's own, before the HUD, instead of ReShade drawing them over it (D3D9 runtime), and can pause chosen ReShade
// effects while menus are open (ReShadeMenuGuard).
class ReShadeIntegration {
public:
	static void Initialize();
	static void RenderEffects(IDirect3DSurface9* OutputSurface);
	static void BeforeNVRMenu();	// ReShadeUnderNVRMenu: right before NVR's menu is drawn (RenderInterfaceHook)
	static void SetMenuGuardEffects(const char* list);	// ReShadeMenuGuardEffects (SettingManager::LoadSettings)

	// ReShadeDepthCompat, also on by itself while ReShade is loaded: NVR's passes leave the game's depth buffer bound.
	static bool KeepDepthBound() { return Loaded || TheSettingManager->SettingsMain.Main.ReShadeDepthCompat; }
	// Same as KeepDepthBound(), except returns false when the bound depth-stencil is multisampled
	// (D3D9 requires RT and depth-stencil MultiSampleType to match; NVR offscreen RTs are D3DMULTISAMPLE_NONE).
	static bool KeepDepthBoundFor(IDirect3DDevice9* device);
	struct MsaaDepthGuard {
		IDirect3DDevice9* device = nullptr;
		IDirect3DSurface9* saved = nullptr;
		bool detached = false;
		explicit MsaaDepthGuard(IDirect3DDevice9* d);
		~MsaaDepthGuard();
	};

	static inline bool Loaded = false;	// a ReShade module is in the process (found at renderer start)
};
