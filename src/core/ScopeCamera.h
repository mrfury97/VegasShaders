#pragma once

// UNOFFICIAL 2026-11-12 the picture-in-picture scope ([Main.Main.Scope] Detect, OwnCamera, Clouds, Skin, Shaders, Water, PostEffects,
// FogStrength). User: the old fix "just disabled the effects outright instead of trying to account for the pip scope".
//
// What draws the picture: the mod B42 Optics calls JIP LN's `ProjectExtraCamera "ISCM" "PRJ" fov size` every frame while aiming. JIP
// draws the world's own scene graph again from a second camera (ISCM, on the weapon) into a small square texture (512 or 1024 a side,
// A16B16G16R16F with the game's HDR) and shows that texture on the lens shape PRJ of the first-person weapon. It does so with the game's
// own functions, called from the plugin: BSShaderUtil::AccumulateScene (0x00B6BEE0), then BSShaderUtil::RenderScene (0x00B6C0D0), before
// the game's own frame render (TESMain::DrawWorld, where this mod's frame work starts). No image-space pass runs on that texture.
// So every draw of the picture passes SetShadersHook and the object draw hooks with this mod's shaders bound, outside the world /
// first-person / reflection passes, and with the MAIN camera's constants from the frame before: the shaders rebuild each pixel's
// position with the main camera's inverse projection (Includes/Shadow.hlsl GetShadowWorldPos) and the sky its view direction from the
// SCREEN's size (Includes/Sky.hlsl SkyViewDirection). Sun and lamp shadows landed in the wrong place, so the old ScopeFix switched them
// off for the picture, recognised by a guess: "a viewport that is not the screen's while a B42 lens was drawn lately".
//
// Detect (default true): the picture is recognised exactly. Both game functions are hooked (Render.cpp ScopeScene). A call is an extra
// camera's scene when it comes from outside the game's own code (a plugin: the return address is not in FalloutNV.exe), its camera is
// not the world's and the scene it walks is the world's own (BSShaderManager's world ShadowSceneNode, or a node above or below it). The
// game's own calls of the two functions (screen splatter, menus' 3D views, cube maps) end at one pointer compare. Round the scene's draws
// the pass context is ShaderSplit::ExtraCamera, and InScene below is true: ScopeSunShadows (Render.cpp) and PointShadowForward::
// OtherCamera ask that instead of the viewport guess. false at startup = the hooks are not installed and the guess is used, as before.
//
// OwnCamera (default true): for the length of the scene the constants this mod's shaders read are that camera's (ShaderManager::
// BeginExtraCamera: the view / projection / inverse matrices, camera position and direction, the reciprocal resolution of the picture,
// the view-space sun direction, the render scale 1, and the sun shadow cascades moved from the camera position they were drawn for to
// this camera's, as ShadowManager moves a kept cascade), uploaded again at the scene's first bind and put back after it, with the next
// bind forced to upload again (the constants reach a game shader only when its handle changes). With that the sun shadows and the
// interior lamp shadows (cube maps / atlas tiles; not the traced ones, whose result is the main view's screen) are ON inside the
// picture, and the sky's gradient and horizon fade follow the picture's own view. false = the main camera's constants and no shadows in
// the picture, as before. The lens shape itself in the main frame never takes the weapon's shadow (ScopeFix, unchanged).
//
// Clouds (default true): the game's own cloud layers are not hidden in the picture while the volumetric clouds replace them in the
// main view (those are a post effect: they are never drawn into the picture, which had a cloudless sky).
// Skin (default true, needs OwnCamera): skin in the picture takes this mod's skin shaders, as the world's does (without the screen-
// space blur, as the first-person arms). false = the game's own skin shaders there, as before (the rule for menus' 3D views).
// Shaders: 0 = the picture drawn as the world is; 1 (default) = cheaper: no terrain parallax and plain sun shadow filtering (no contact
// hardening search) in the picture; 2 = everything in the picture with the game's own shaders (cheapest; the game's look in the lens).
// Water (default 0): 1 = water in the picture with the game's own water shaders (this mod's read the main view's depth and reflection
// by screen position, which is not the picture's). A WATER line in the log counts the water passes and reflection renders either way.
// PostEffects (default 0): 1 = distance fog on the picture from its own depth (core/ScopeFog.h; the weather's fog, not VolumetricFog's
// look). 2 is reserved for fog + ambient occlusion (not built). FogStrength scales the fog.
namespace ScopeCamera {
	// ---- settings (ReadSettings; a missing key gives the default) ----
	inline bool Detect = true, OwnCamera = true, Clouds = true, Skin = true;
	inline int Shaders = 1, Water = 0, PostEffects = 0;
	inline float FogStrength = 1.0f;
	inline UInt32 SettingsFrame = 0xFFFFFFFFu;	// FrameNumber of the last read

	// ---- state ----
	inline bool HooksIn = false;				// both game functions are detoured (Detect was on at startup)
	inline bool InScene = false;				// an extra camera's scene is being drawn now (ShaderSplit::ExtraCamera)
	inline bool Own = false;					// ... with that camera's constants in place (OwnCamera took effect)
	inline bool Shadows = false;				// ... so the sun and lamp shadow lookups are right in it
	inline bool GameShaders = false;			// ... every draw of it takes the game's own shaders (Shaders 2)
	inline bool OurSkin = false;				// ... skin takes this mod's shaders (Skin, with Own)
	inline bool GameWater = false;				// ... water takes the game's own shaders (Water 1)
	inline NiCamera* Camera = nullptr;			// the scene's camera while InScene
	inline const NiCamera* Pending = nullptr;	// a plugin just accumulated the world's scene for this camera: the next RenderScene with it is the picture
	inline UInt32 PendingCaller = 0;			// that call's return address (for the log)
	inline UInt32 LastSceneFrame = 0xFFFFFFFFu;	// FrameNumber when the last extra-camera scene ended (the frame it belongs to is the next one drawn)
	inline unsigned ScenesDrawn = 0;			// this session
	// Water (the Scope WATER log line, every 300 scenes)
	inline const void* LastCamera = nullptr;	// the last scene's camera (compared only, never read through)
	inline unsigned WaterScenes = 0;			// scenes since the last WATER line
	inline unsigned WaterBinds = 0, WaterBindsOurs = 0;	// water shader binds inside those scenes; of those with this mod's water shader
	inline unsigned ReflectionCalls = 0, ReflectionCallsExtra = 0;	// the game's water reflection renders (RenderReflectionsHook) meanwhile; of those for the scene's camera

	inline UInt32 FrameNumber() { return TheFrameRateManager ? TheFrameRateManager->FrameNumber : 0; }

	// The exact recognition is working NOW: the hooks are in and saw an extra camera's scene for this frame or the two before it.
	// Only then is the old viewport guess (ScopeSunShadows, PointShadowForward::OtherCamera) left out: if the hooks never see the
	// picture (a plugin that draws it another way), the guess goes on protecting it exactly as before.
	inline bool Exact() { return Detect && HooksIn && ScenesDrawn && FrameNumber() - LastSceneFrame <= 2; }

	inline bool ReadBool(const char* key, bool fallback) {
		SettingManager::Configuration::ConfigNode node;
		return TheSettingManager->Config.FillNode(&node, "Main.Main.Scope", key) ? node.BoolValue : fallback;
	}
	inline int ReadInt(const char* key, int fallback) {
		SettingManager::Configuration::ConfigNode node;
		return TheSettingManager->Config.FillNode(&node, "Main.Main.Scope", key) ? node.IntValue : fallback;
	}
	inline float ReadFloat(const char* key, float fallback) {
		SettingManager::Configuration::ConfigNode node;
		return TheSettingManager->Config.FillNode(&node, "Main.Main.Scope", key) ? node.FloatValue : fallback;
	}

	// ShaderManager::UpdateConstants (every frame) and the hooks' installation. Live, but read only every 30th frame: nothing here costs
	// a frame without a scope more than one compare.
	inline void ReadSettings(bool now = false) {
		const UInt32 frame = FrameNumber();
		if (!now && SettingsFrame != 0xFFFFFFFFu && frame - SettingsFrame < 30) return;
		SettingsFrame = frame;
		Detect = ReadBool("Detect", true);
		OwnCamera = ReadBool("OwnCamera", true);
		Clouds = ReadBool("Clouds", true);
		Skin = ReadBool("Skin", true);
		Shaders = (std::min)((std::max)(ReadInt("Shaders", 1), 0), 2);
		Water = (std::min)((std::max)(ReadInt("Water", 0), 0), 1);
		PostEffects = (std::min)((std::max)(ReadInt("PostEffects", 0), 0), 2);
		FogStrength = (std::min)((std::max)(ReadFloat("FogStrength", 1.0f), 0.0f), 4.0f);
	}
}
