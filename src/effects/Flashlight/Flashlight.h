#pragma once

// The player's flashlight (the Pip-Boy light's key). UNOFFICIAL 2026-11-04 restructure (user: "get rid of flashlight beam shader, lets
// restructure the flashlight shader, get rid of the old post process code, the deferred variant should be on by default, dimmer and
// distance, as well as attach to weapon should be the only things in main, the color of the flashlight should be dictated by the color of
// your pipboy hud"). This effect draws nothing itself (its Flashlight.fx is a stub so the effect loads and keeps its menu switch); each
// frame it places the light (on the camera, the head in third person, or the weapon), colours it with the Pip-Boy's own HUD colour and
// hands it to what lights the world with it:
//  - core/FlashlightDeferred.h: the spot light on the G-buffer (with its shadow map, ShadowManager::RenderShadowSpotlight);
//  - effects/Grass: the grass's own beam (card and GPU grass), shadowed too;
//  - core/FlashlightLamp.h: the Pip-Boy light moved onto the beam, so the game lights what the G-buffer does not hold.
// The old screen-space light, its Vulkan passes, the FlashlightBeam shaft and the MaterialLight relight are in
// VegasShaders-backups/before_flashlight_restructure_20261104.
class FlashlightEffect : public EffectRecord
{
public:
	FlashlightEffect() : EffectRecord("Flashlight"), SpotLight(nullptr), spotLightActive(false), aimingHoldUntil(0.0) {};

	struct FlashlightSettingsStruct {	// [Shaders.Flashlight.Main]
		float		Dimmer;			// how bright the beam is at its centre, in the lamps' units (linear light)
		float		Distance;		// its reach
		bool		attachToWeapon;	// on the weapon instead of the camera (it rides the weapon while aiming either way)
		NiColor		Color;			// the Pip-Boy's HUD colour this frame (SystemColorManager "Pipboy"), or ColorR/G/B with UseOwnColour
		float		ConeAngle;		// the cone's outer half-angle, degrees (Angle; 30 = the fixed value of 2026-11-04)
		NiPoint3	Offset;			// from the weapon or camera, in its frame (OffsetX = along the beam, Y / Z the node's other two axes)
		// UNOFFICIAL 2026-11-11 the old flashlight's look keys back on this one (each a no-op at its default)
		bool		useOwnColour;	// ColorR/G/B instead of the Pip-Boy colour (its hue: brightness stays Dimmer's)
		float		NearFade;		// > 0: the light fades out on surfaces nearer the light than this (units)
		float		HotspotLimit;	// > 0: a soft roll-off of the added light above 1 / HotspotLimit (luma)
		bool		softEdges;		// wrap lighting instead of the clamped N.L (the raster pass only)
	};
	FlashlightSettingsStruct	Settings = {};
	// UNOFFICIAL 2026-11-11 the look outdoors ([0], Main) and indoors ([1], Main with Interiors' values >= 0 over it); ApplyLook picks one
	// each frame into Settings (as the old flashlight did, by TheShaderManager->GameState.isExterior).
	struct LookStruct { NiColor Color; float Dimmer, Distance, ConeAngle, NearFade, HotspotLimit; };
	LookStruct	Looks[2] = {};
	void	ApplyLook(bool exterior);
	// [Shaders.Flashlight.Deferred] (core/FlashlightDeferred.h): how the light pass draws it.
	struct DeferredSettingsStruct {
		bool	Shadows;		// the light's shadow map (the old Main RenderShadows' successor)
		float	ShadowSoftness;	// PCF radius, in texels of a 512 map (FlashlightDeferred::SoftnessTexels)
		float	ShadowBias;		// share of the reach
		float	Cookie;			// the cookie texture's strength (0 none; the old Main CookieStrength's successor)
		float	Falloff;		// > 0 a windowed inverse square, (1 - d^2/r^2)^2 / (1 + F d^2/r^2); 0 the lamps' 1 - d^2/r^2
		bool	GameLight;		// the Pip-Boy light as the beam's real game lamp for what the light pass cannot see (core/FlashlightLamp.h)
		int		DebugView;		// 1 the light alone, 2 the shadow, 3 the normal
		bool	TestForceOn;	// test only: on as if the Pip-Boy light were (and the game's Pip-Boy light switched on)
	};
	DeferredSettingsStruct	Deferred = {};
	bool	WantsShadowMap() const { return Deferred.Shadows; }

	// The Pip-Boy light while the flashlight is on: the flashlight's game lamp (GameLight) or, with GameLight off, darkened (else the game
	// could still hand it to the objects round the player as one of their few lamps). Its own values, put back when the flashlight is off.
	NiPointLight*	hiddenPipBoyLight = nullptr;
	float			hiddenPipBoyDimmer = 0.0f;
	NiColor			hiddenPipBoySpec = NiColor(0, 0, 0);
	bool			drivenPipBoyLight = false;
	NiColor			hiddenPipBoyDiff = NiColor(0, 0, 0);
	NiPoint3		hiddenPipBoyLocal = {};
	void			DrivePipBoyLight(NiPointLight* PipLight, const NiNode* PlayerNode, const NiPoint3& Pos, const NiMatrix33& Rot);
	void			RestorePipBoyLight();

	NiSpotLight* SpotLight;
	bool	spotLightActive;	// true while the cone is lit
	double	aimingHoldUntil;	// real time the aiming latch holds until, see UpdateConstants

	void	UpdateConstants();
	void	UpdateSettings();
	bool	ShouldRender() { return false; }	// nothing drawn here (see above)
	void	PublishLightConstants(bool abActive);
};
