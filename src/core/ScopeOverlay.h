#pragma once

// UNOFFICIAL 2026-11-12 the game's own rifle scope overlay ([Main.Main.Scope] KeepOverlayBlack, FullFrame).
// User: "the game's base scope texture seems to get lightened by our effects on the edges and it looks kind of strange. It's supposed to
// be black but it looks almost grey."
//
// What the game does (FalloutNV 1.4.0.525; names from the community symbol map, the order checked in the machine code):
//  - The overlay is a 3D model, the weapon's scope NIF (meshes\interface\hud\*.nif), hung under HUDMainMenu::spScopeModel (0x011D9770),
//    a node of the menu scene (ShadowSceneNode 1). HUDMainMenu::Draw3DElements (0x0077F0D0) draws it with its own camera
//    (spScopeCamera 0x011D975C) while the HUD's scope flag is set (+0x1FC: HUDMainMenu::IsGunScopeVisible 0x00709CC0). It clears the depth
//    buffer first and draws through the game's usual accumulator and batch renderer, so every draw passes SetShadersHook.
//  - TESMain::Swap (0x0086FF70) calls it at 0x00870403 (Interface::DrawHUDMenu3D), AFTER TESMain::DrawWorld: after the game's image
//    space and the whole of this mod's chain (ProcessImageSpaceShadersHook), before the menus. So no post effect ever sees the overlay:
//    bloom, fog, the exposure meter, the tone curve, the grade, anti-aliasing and grain are finished when it is drawn on top.
//  - New Vegas' seven reticle scopes (MilDot, Varmit, Duplex, German, BigGame, Laser, scope3) are a black plate with a rectangular
//    opening (unlit: BSShaderNoLightingProperty) and, filling that opening, ONE QUAD WITH A LIT MATERIAL (BSShaderPPLightingProperty,
//    specular flag, alpha blended, material specular colour black). Its texture <name>_o.dds is the black mask round the lens circle
//    plus the reticle; its normal map scope03_n.dds is flat, with a gloss channel that rises from 0 at the lens to 0.7 - 1 at the
//    quad's edges. The game's lit shader gives black there: a black texture times the light, and a highlight scaled by the material's
//    black specular colour. (Fallout 3's scope01.nif and the Gauss rifle's are unlit all through.)
// What went wrong: that quad was drawn with this mod's object shaders (SLS2xxx = ObjectTemplate.hlsl) like any lit object, in the
// 'Outside' context. They add light that does not come from the texture: highlights and the sky's / surroundings' reflection, whose
// strength follows the gloss channel and not the material's black specular colour (PBR.hlsl: a fixed dielectric reflectance; Object.hlsl:
// a grey or black surface with a high mask is taken for polished metal), all made from the WORLD camera's data, which does not belong
// to this picture. The black round the lens came out grey, most towards the quad's edges, where the gloss is highest.
//
// KeepOverlayBlack (default true, live): every draw of the overlay model takes the game's own shader pair, as the character creation
// menu's 3D view does (GameShadersInMenus, SetShadersHook). A draw is the overlay's when its shape hangs under spScopeModel. Asked only
// at shader binds outside the world / first-person / reflection passes, and only while the game says the scope is up: nothing otherwise.
// B42 Optics' lens shapes (B42Lens*, PRJ) hang on the first-person weapon and are drawn in the first-person pass: never matched.
// false = the mod's shaders on the overlay, as before.
//
// FullFrame (default false, live): while the scope is up the game draws the world through a scissor rectangle
// (TESMain::DrawWorldStandard 0x00870BD0: left = width x fScopeScissorAmount (0.3) x the interface's aspect ratio ratio x 0.5,
// BSShaderManager::EnableFullScreenScissor 0x00B4F540), because the plate hides the sides. The strips left and right of it keep what the
// scene's clear left: no world, far depth. This mod's chain draws full screen (a render target change puts the scissor rectangle back to
// the whole target) and takes those strips for sky: fog and clouds are drawn there, they are part of the average the exposure meters,
// and wide blurs can carry them to the lens's rim. On, the game's setting reads 0 for the length of the world draw (its value is put back
// right after), which makes the scissor the whole screen: the frame under the overlay is then an ordinary one. It costs the world's
// pixels in the strips while scoped. The same as fScopeScissorAmount=0 under [Display] in the game's INI. Off: as before.
namespace ScopeOverlay {
	inline bool KeepBlack = true;		// [Main.Main.Scope] KeepOverlayBlack
	inline bool FullFrame = false;		// [Main.Main.Scope] FullFrame
	// UNOFFICIAL 2026-11-12 KeepRenderScale (user, FSR at RenderScale 0.33 on a 2560x1440 screen, a rifle with the game's own scope: "drops
	// the fps in half when i scope"; the profiler log: TAA 3.1 -> 5.9 ms, Ambient light 2.9 -> 5.2 ms, every screen effect ~1.8x). The game
	// draws the scoped world through a scissor rectangle, and the upscaler refuses a frame with a scissor set (TAAUpscale.cpp UpscaleBegin:
	// "a frame stays at native resolution: a scissor is set (a gun scope's view)"), so while scoped the WHOLE frame and every effect ran at
	// native resolution. On (default): TAAEffect::UpscaleBegin accepts the scoped frame and moves the game's scissor rectangle into the
	// render rectangle (each edge x the render scale), so the world is clipped as the game meant and the frame stays at the render
	// scale. (First try the same day: reading the game's scissor amount as 0 for the world draw. The game then still switched the
	// scissor TEST on, with a whole-screen rectangle, and the upscaler still refused the frame: no gain on the bench.)
	// false = as before (native resolution while scoped).
	inline bool KeepRenderScale = true;	// [Main.Main.Scope] KeepRenderScale
	inline bool UpscalerSmall = false;	// this frame: FSR / DLSS at a RenderScale below 1 (set by ShaderManager::UpdateConstants)
	inline unsigned Logged = 0;			// bit 0: an unlit overlay draw given the game's shaders, bit 1: the lit quad, bit 2: FullFrame
	inline bool ScissorHeld = false;	// the game's fScopeScissorAmount is 0 for this world draw
	// UNOFFICIAL 2026-11-12 (the same day; user: "the vanilla overlay one now drops the fps in half when i scope", a ported rifle with a
	// detailed scope model): the first version asked the game "is the scope up" and WALKED the scope model at EVERY shader bind outside
	// the world passes (shadow casters and all), up to 96 nodes with a virtual call each. A small scope NIF (the binoculars: a handful
	// of shapes) cost nothing; a big one cost the walk's cap thousands of times a frame. Now the model is walked ONCE a frame
	// (ReadSettings) into this list, and a bind only compares its shape with the list; nothing at all while the scope is down.
	inline bool ScopeUp = false;			// this frame: KeepBlack and the game says the scope is up
	inline const void* Shapes[192] = {};	// this frame's shapes of the scope model (not its nodes)
	inline int ShapeCount = 0;
	inline unsigned AskedThisFrame = 0, AskedLogged = 0;	// binds that compared with the list (for one log line)
	inline float ScissorKept = 0.0f;	// its own value, put back in EndWorld

	// The game's own answer: Interface::IsGunScopeVisible (0x00709CB0; false while there is no HUD).
	inline bool Visible() { return CdeclCall<bool>(0x00709CB0); }

	// Once a frame: the shapes hanging under HUDMainMenu::spScopeModel (0x011D9770), walked down from the model's root (alive as long as
	// the HUD is). Only addresses are kept and compared: a listed shape is never read at a bind.
	inline void Collect() {
		ShapeCount = 0;
		NiAVObject* root = *(NiAVObject**)0x011D9770;
		if (!root) return;
		NiAVObject* stack[128];
		int top = 0, seen = 0;
		stack[top++] = root;
		while (top && seen < 4096 && ShapeCount < 192) {
			NiAVObject* object = stack[--top];
			seen++;
			NiNode* node = object->IsNiNode();
			if (!node) { Shapes[ShapeCount++] = object; continue; }
			for (UInt32 i = 0; i < node->m_children.end && top < 128; i++)
				if (node->m_children.data[i]) stack[top++] = node->m_children.data[i];
		}
	}

	// ShaderManager::UpdateConstants, every frame. A missing key gives the default (FillNode is false until the defaults file lists it).
	inline void ReadSettings() {
		SettingManager::Configuration::ConfigNode node;
		KeepBlack = !TheSettingManager->Config.FillNode(&node, "Main.Main.Scope", "KeepOverlayBlack") || node.BoolValue;
		FullFrame = TheSettingManager->Config.FillNode(&node, "Main.Main.Scope", "FullFrame") && node.BoolValue;
		KeepRenderScale = !TheSettingManager->Config.FillNode(&node, "Main.Main.Scope", "KeepRenderScale") || node.BoolValue;
		if (ScopeUp && AskedThisFrame && !AskedLogged) {
			AskedLogged = 1;
			Logger::Log("UNOFFICIAL KeepOverlayBlack: scope up: the scope model has %d shape(s); %u shader binds outside the world passes compared with that list last frame (one walk of the model a frame, none per bind).", ShapeCount, AskedThisFrame);
		}
		AskedThisFrame = 0;
		ScopeUp = KeepBlack && Visible();
		if (ScopeUp) Collect(); else ShapeCount = 0;
	}

	// SetShadersHook, a bind outside the world / first-person / reflection passes: is this pass's shape one of the scope overlay's?
	// (this frame's list; while the scope is down: one test of a bool)
	inline bool InDraw(const void* shape) {
		if (!ScopeUp || !ShapeCount || !shape) return false;
		AskedThisFrame++;
		for (int i = 0; i < ShapeCount; i++) if (Shapes[i] == shape) return true;
		return false;
	}

	// The log's proof that the rule ran: once for the lit quad (the cause of the grey), once for an unlit shape (only with the lit
	// particles collection on: otherwise those draw with the game's shaders anyway).
	inline void Note(bool swapped, const char* vertexShader, const char* pixelShader) {
		if (!swapped) return;
		const bool lit = pixelShader && !strncmp(pixelShader, "SLS", 3);
		const unsigned bit = lit ? 2u : 1u;
		if (Logged & bit) return;
		Logged |= bit;
		Logger::Log("UNOFFICIAL KeepOverlayBlack: the game's scope overlay (%s) is drawn with the game's own shaders (%s + %s), not the mod's.",
			lit ? "its lit quad: the black mask round the lens and the reticle" : "an unlit shape", vertexShader ? vertexShader : "?", pixelShader ? pixelShader : "?");
	}

	// RenderHook, round the game's world draw (TESMain::DrawWorld reads the setting once, in DrawWorldStandard, when the scope is up).
	inline float* ScissorAmount() { return (float*)0x011DEA90; }	// fScopeScissorAmount:Display (the Setting at 0x011DEA8C, its value at +4)
	inline void BeginWorld() {
		if (!FullFrame || !Visible()) return;
		ScissorKept = *ScissorAmount();
		*ScissorAmount() = 0.0f;
		ScissorHeld = true;
		if (!(Logged & 4u)) {
			Logged |= 4u;
			Logger::Log("UNOFFICIAL Scope FullFrame: while scoped the world is drawn on the whole screen (the game's fScopeScissorAmount %.2f read as 0 for the world draw).", ScissorKept);
		}
	}
	inline void EndWorld() {
		if (!ScissorHeld) return;
		*ScissorAmount() = ScissorKept;
		ScissorHeld = false;
	}
}
