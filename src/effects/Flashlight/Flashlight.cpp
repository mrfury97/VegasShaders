#include "Flashlight.h"
#include "../../core/FlashlightDeferred.h"	// UNOFFICIAL 2026-11-04 the deferred spot light (Usable)
#include "../../core/FlashlightLamp.h"	// UNOFFICIAL 2026-11-04 the Pip-Boy light as its real game lamp


// Settings readers for this file: the section and key exactly as the settings file names them.
static int FlashSettingI(const char* section, const char* key) { return TheSettingManager->GetSettingI(section, key); }
static float FlashSettingF(const char* section, const char* key) { return TheSettingManager->GetSettingF(section, key); }
// UNOFFICIAL 2026-11-11 a key's value, or `fallback` when the settings do not have it (an older saved file): the keys brought back below must
// be no-ops when missing too, and a missing key reads 0 through FlashSettingF (Angle 0, Interiors 0 = "replace Main with 0").
static float FlashSettingOr(const char* section, const char* key, float fallback) {
	SettingManager::Configuration::ConfigNode node;
	return TheSettingManager->Config.FillNode(&node, section, key) ? node.FloatValue : fallback;
}

void FlashlightEffect::UpdateSettings() {
	const char* const mainSection = "Shaders.Flashlight.Main";
	Settings.attachToWeapon = FlashSettingI(mainSection, "AttachToWeapon") != 0;
	// UNOFFICIAL 2026-11-11 (players asked for the old flashlight's settings back): its look keys on this light, each a no-op at its default.
	// Angle: the cone's outer HALF-angle in degrees, as OuterSpotAngle, the old Angle and the 2026-11-04 fixed 30 (default 30: today's beam);
	// the full-strength edge is half of it, the shadow map's FOV twice it (ShadowManager::RenderShadowSpotlight, so the map and the cookie
	// follow), at most 63 because the path tracer packs it in whole degrees below 64 (RtFlashlightCode.h). Offset: in the frame of the node
	// the light rides (weapon, head or camera: weaponRot below, as the old one), X along the beam.
	Settings.useOwnColour = FlashSettingI(mainSection, "UseOwnColour") != 0;
	Settings.softEdges = FlashSettingI(mainSection, "SoftEdges") != 0;
	Settings.Offset = NiPoint3(FlashSettingOr(mainSection, "OffsetX", 0.0f), FlashSettingOr(mainSection, "OffsetY", 0.0f), FlashSettingOr(mainSection, "OffsetZ", 0.0f));
	LookStruct& outdoors = Looks[0];
	auto colour = [&](const char* key, float fallback) { return (std::min)((std::max)(FlashSettingOr(mainSection, key, fallback), 0.0f), 10.0f); };
	outdoors.Color = NiColor(colour("ColorR", 2.0f), colour("ColorG", 2.0f), colour("ColorB", 1.0f));
	outdoors.Dimmer = (std::min)((std::max)(FlashSettingF(mainSection, "Dimmer"), 0.0f), 50.0f);
	outdoors.Distance = (std::max)(FlashSettingF(mainSection, "Distance"), 10.0f);
	outdoors.ConeAngle = (std::min)((std::max)(FlashSettingOr(mainSection, "Angle", 30.0f), 5.0f), 63.0f);
	outdoors.NearFade = (std::min)((std::max)(FlashSettingOr(mainSection, "NearFade", 0.0f), 0.0f), 1000.0f);
	outdoors.HotspotLimit = (std::min)((std::max)(FlashSettingOr(mainSection, "HotspotLimit", 0.0f), 0.0f), 10.0f);
	// Interiors: a value of 0 or more replaces Main's indoors; below 0 (default -1) or missing: Main's (as RtAmbient's <Key>Interior)
	const char* const interiorSection = "Shaders.Flashlight.Interiors";
	auto indoors = [&](const char* key, float main, float lo, float hi) {
		const float v = FlashSettingOr(interiorSection, key, -1.0f);
		return v >= 0.0f ? (std::min)((std::max)(v, lo), hi) : main;
	};
	LookStruct& inside = Looks[1];
	inside.Color = NiColor(indoors("ColorR", outdoors.Color.r, 0.0f, 10.0f), indoors("ColorG", outdoors.Color.g, 0.0f, 10.0f), indoors("ColorB", outdoors.Color.b, 0.0f, 10.0f));
	inside.Dimmer = indoors("Dimmer", outdoors.Dimmer, 0.0f, 50.0f);
	inside.Distance = indoors("Distance", outdoors.Distance, 10.0f, 1.0e6f);
	inside.ConeAngle = indoors("Angle", outdoors.ConeAngle, 5.0f, 63.0f);
	inside.NearFade = indoors("NearFade", outdoors.NearFade, 0.0f, 1000.0f);
	inside.HotspotLimit = indoors("HotspotLimit", outdoors.HotspotLimit, 0.0f, 10.0f);
	ApplyLook(TheShaderManager->GameState.isExterior);

	const char* const deferredSection = "Shaders.Flashlight.Deferred";
	Deferred.Shadows = FlashSettingI(deferredSection, "Shadows") != 0;
	Deferred.ShadowSoftness = (std::min)((std::max)(FlashSettingF(deferredSection, "ShadowSoftness"), 0.0f), 16.0f);
	Deferred.ShadowBias = (std::min)((std::max)(FlashSettingF(deferredSection, "ShadowBias"), 0.0f), 0.1f);
	Deferred.Cookie = (std::min)((std::max)(FlashSettingF(deferredSection, "Cookie"), 0.0f), 1.0f);
	Deferred.Falloff = (std::min)((std::max)(FlashSettingF(deferredSection, "Falloff"), 0.0f), 32.0f);
	Deferred.GameLight = FlashSettingI(deferredSection, "GameLight") != 0;
	Deferred.DebugView = (std::max)(0, (std::min)(3, (int)FlashSettingI(deferredSection, "DebugView")));
	Deferred.TestForceOn = FlashSettingI(deferredSection, "TestForceOn") != 0;

	if (!SpotLight) SpotLight = NiSpotLight::CreateObject();
};

// The weapon types the light does not ride on: melee, grenades, mines and thrown weapons (there is no sight to look down).
static bool IsMeleeWeaponType(UInt8 type) {
	switch (type) {
	case TESObjectWEAP::WeaponType::kWeapType_HandToHandMelee:
	case TESObjectWEAP::WeaponType::kWeapType_OneHandMelee:
	case TESObjectWEAP::WeaponType::kWeapType_TwoHandMelee:
	case TESObjectWEAP::WeaponType::kWeapType_OneHandGrenade:
	case TESObjectWEAP::WeaponType::kWeapType_OneHandMine:
	case TESObjectWEAP::WeaponType::kWeapType_OneHandLunchboxMine:
	case TESObjectWEAP::WeaponType::kWeapType_OneHandThrown:
		return true;
	default:
		return false;
	}
}

// UNOFFICIAL 2026-11-04 (user: "the color of the flashlight should be dictated by the color of your pipboy hud"): the game's "Pipboy" system
// colour (SystemColorManager::GetColor 0x7190A0 on GetSingleton 0x718B60, colour 4: the Pip-Boy's own, set in the game's display settings;
// packed R << 24 | G << 16 | B << 8 | A, MakeRGBA), the game's default amber when there is none.
static NiColor PipboyColor() {
	void* manager = ((void* (__cdecl*)())0x718B60)();
	const UInt32 packed = manager ? ThisStdCall<UInt32>(0x7190A0, manager, 4) : 0;
	if (!packed) return NiColor(1.0f, 182.0f / 255.0f, 66.0f / 255.0f);
	return NiColor(((packed >> 24) & 0xFF) / 255.0f, ((packed >> 16) & 0xFF) / 255.0f, ((packed >> 8) & 0xFF) / 255.0f);
}

// UNOFFICIAL 2026-11-11 the outdoor or indoor look into Settings (everything that reads the light reads it from there: the light below,
// FlashlightDeferred::LightParams for the light pass, the grass, RT reflections and the path tracer, FlashlightLamp).
void FlashlightEffect::ApplyLook(bool exterior) {
	const LookStruct& look = Looks[exterior ? 0 : 1];
	Settings.Dimmer = look.Dimmer;
	Settings.Distance = look.Distance;
	Settings.ConeAngle = look.ConeAngle;
	Settings.NearFade = look.NearFade;
	Settings.HotspotLimit = look.HotspotLimit;
}

void FlashlightEffect::UpdateConstants() {
	if (SpotLight == nullptr) return;
	ApplyLook(TheShaderManager->GameState.isExterior);	// (UNOFFICIAL 2026-11-11: per frame, so a door switches it)
	Settings.Color = Settings.useOwnColour ? Looks[TheShaderManager->GameState.isExterior ? 0 : 1].Color : PipboyColor();

	// Whether the game has the flashlight on: the test JIP uses (functions_jip/jip_fn_ui.h), with TestForceOn for the auto tests.
	uint8_t* const magicBase = ThisStdCall<uint8_t*>(0x93CCD0, NULL);
	void* const magicSlot = (void*)&(*(MagicItem**)(magicBase + 0x18));
	UInt32 lightIsOn = ThisStdCall<bool>(0x822B90, &Player->magicTarget, magicSlot, 1);
	// UNOFFICIAL 2026-11-04 TestForceOn also switches the game's own Pip-Boy light on, as its key does (PlayerCharacter's input handling: both
	// light effects shown, then the spPipBoyLight ability cast), a few tries in the world: the game light (core/FlashlightLamp.h) is that light
	if (Deferred.TestForceOn && !lightIsOn && Player->parentCell) {
		static int tries = 0;
		static unsigned nextFrame = 0;
		if (tries < 5 && (unsigned)TheFrameRateManager->FrameNumber >= nextFrame) {
			tries++;
			nextFrame = (unsigned)TheFrameRateManager->FrameNumber + 120;
			void* pipboy = ((void* (__cdecl*)())0x705990)();	// Interface::GetPipboy
			void* spell = ThisStdCall<void*>(0x93CCD0, NULL);	// MagicSystem::GetPipBoyLightSpell
			if (pipboy && spell) {
				ThisStdCall<void>(0x7FA310, pipboy, true, true, true);	// FOPipboyManager::ShowPipboyLightEffect (third person)
				ThisStdCall<void>(0x7FA310, pipboy, false, true, true);	// (first person)
				void* caster = (UInt8*)Player + 0x88;	// Actor's MagicCaster
				ThisStdCall<void>(0x815600, caster, spell, false);	// MagicCaster::CastAbility
				Logger::Log("UNOFFICIAL TestForceOn: the Pip-Boy light switched on (try %d)", tries);
			}
		}
	}
	if (Deferred.TestForceOn) lightIsOn = 1;	// UNOFFICIAL 2026-10-28 TestForceOn (auto tests)

	const bool active = Enabled && lightIsOn != 0;
	spotLightActive = active;
	if (!active) {
		RestorePipBoyLight();
		// off: the light goes to zero dimmer and zero reach
		SpotLight->Dimmer = 0.0f;
		SpotLight->Spec = NiColor(0.0f, 0.0f, 0.0f);
		SpotLight->CastShadows = false;
		PublishLightConstants(false);
		return;
	}

	NiPoint3 weaponPos;
	NiMatrix33 weaponRot;
	bool meleeWeapon = false;
	if (Player->process->IsWeaponOut() && Player->ActorSkinInfo) {
		const TESObjectWEAP* weapon = Player->ActorSkinInfo->WeaponForm;
		meleeWeapon = !weapon || IsMeleeWeaponType(weapon->weaponType);
	}

	// Down the sights the light belongs on the gun, whatever AttachToWeapon says: the weapon is lined up with the view there, so
	// riding the head puts the beam behind the thing the player is looking past.
	//
	// IsAiming() cannot be used raw: it is false during the attack and follow-through states, so the light alternated between two
	// bones while firing. So it is latched: aiming takes hold at once, and releasing waits for the signal to stay false for a
	// little while (in real seconds, so the hold does not shorten as the frame rate rises). Melee is excluded, as for AttachToWeapon.
	static const double aimHoldSeconds = 0.35;
	const double frameTime = TheFrameRateManager->Time;
	if (Player->IsAiming()) aimingHoldUntil = frameTime + aimHoldSeconds;
	const bool aiming = frameTime < aimingHoldUntil;
	const bool rideWeapon = (Settings.attachToWeapon || aiming) && !meleeWeapon && Player->process->IsWeaponOut();

	if (Player->isThirdPerson) {
		if (rideWeapon && Player->ActorSkinInfo && Player->ActorSkinInfo->WeaponNode) {
			weaponPos = Player->ActorSkinInfo->WeaponNode->m_worldTransform.pos;
			weaponRot = Player->ActorSkinInfo->WeaponNode->m_worldTransform.rot;
		}
		else if (Player->ActorSkinInfo && Player->ActorSkinInfo->HeadNode) {
			// A turn of 90 degrees about Z, then X, applied in the head bone's own frame.
			static const float turnData[3][3] = { { 0.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { -1.0f, 0.0f, 0.0f } };
			NiMatrix33 turn = NiMatrix33();
			for (int row = 0; row < 3; row++)
				for (int column = 0; column < 3; column++) turn.data[row][column] = turnData[row][column];

			const NiAVObject* head = Player->ActorSkinInfo->HeadNode;
			weaponPos = head->m_worldTransform.pos;
			const NiMatrix33 headRot = head->m_worldTransform.rot;
			turn = turn * headRot;	// the turn in the bone's frame
			weaponRot = headRot * turn;
		}
		else {
			weaponPos = WorldSceneGraph->camera->m_worldTransform.pos;
			weaponRot = WorldSceneGraph->camera->m_worldTransform.rot;
		}
	}
	else {
		if (rideWeapon && Player->firstPersonSkinInfo && Player->firstPersonSkinInfo->WeaponNode) {
			weaponPos = Player->firstPersonSkinInfo->WeaponNode->m_worldTransform.pos;
			weaponRot = Player->firstPersonSkinInfo->WeaponNode->m_worldTransform.rot;
		}
		else {
			weaponPos = WorldSceneGraph->camera->m_worldTransform.pos;
			weaponRot = WorldSceneGraph->camera->m_worldTransform.rot;
		}
	}

	// The offset, turned into the direction of the cone.
	const NiPoint3 offset = weaponRot * Settings.Offset;
	weaponPos.x += offset.x;
	weaponPos.y += offset.y;
	weaponPos.z += offset.z;

	// The Pip-Boy light (the game's, a child of the player's node): the flashlight's game lamp with GameLight (core/FlashlightLamp.h; not
	// culled then: the game hands no culled light to the objects), else culled and darkened while the flashlight is on.
	NiNode* playerNode = Player->GetNode();
	const bool drive = FlashlightDeferred::Usable() && Deferred.GameLight;
	for (UINT32 i = 0; playerNode && i < playerNode->m_children.capacity; i++) {
		NiAVObject* child = playerNode->m_children.data[i];
		if (!child || child->GetRTTI() != (void*)0x11F4A98) continue;
		NiPointLight* pipLight = (NiPointLight*)child;
		if (drive) {
			child->m_flags &= ~child->APP_CULLED;
			if (hiddenPipBoyLight != pipLight || !drivenPipBoyLight) {
				if (hiddenPipBoyLight != pipLight || pipLight->Dimmer != 0.0f || pipLight->Spec.r != 0.0f) { hiddenPipBoyDimmer = pipLight->Dimmer; hiddenPipBoySpec = pipLight->Spec; }
				hiddenPipBoyDiff = pipLight->Diff;
				hiddenPipBoyLocal = pipLight->m_localTransform.pos;
				hiddenPipBoyLight = pipLight;
				drivenPipBoyLight = true;
				static bool told = false;
				if (!told) { told = true; Logger::Log("UNOFFICIAL FlashlightLamp: the Pip-Boy light (dimmer %.2f, reach %.0f) is the flashlight's game light while it is on", hiddenPipBoyDimmer, hiddenPipBoySpec.r); }
			}
			DrivePipBoyLight(pipLight, playerNode, weaponPos, weaponRot);
		}
		else {
			child->m_flags |= child->APP_CULLED;
			if (drivenPipBoyLight && hiddenPipBoyLight == pipLight) {	// (GameLight switched off while lit: its own colour and place back first)
				pipLight->Diff = hiddenPipBoyDiff;
				pipLight->m_localTransform.pos = hiddenPipBoyLocal;
				drivenPipBoyLight = false;
				FlashlightLamp::Release();
			}
			if (hiddenPipBoyLight != pipLight || pipLight->Dimmer != 0.0f || pipLight->Spec.r != 0.0f) {
				if (pipLight->Dimmer != 0.0f || pipLight->Spec.r != 0.0f) { hiddenPipBoyDimmer = pipLight->Dimmer; hiddenPipBoySpec = pipLight->Spec; }
				hiddenPipBoyLight = pipLight;
				pipLight->Dimmer = 0.0f;
				pipLight->Spec = NiColor(0, 0, 0);
			}
		}
	}

	NiSpotLight& light = *SpotLight;
	light.CastShadows = WantsShadowMap();
	light.Diff = Settings.Color;
	light.Dimmer = Settings.Dimmer;
	light.m_worldTransform.pos = weaponPos;
	light.m_worldTransform.rot = weaponRot;
	light.m_worldTransform.scale = 1.0f;
	light.OuterSpotAngle = Settings.ConeAngle;
	light.Spec = NiColor(Settings.Distance, 0, 0);	// radius in the r channel

	PublishLightConstants(true);
};

// The Pip-Boy light's own values back once the flashlight is off (only if it is still the player's: the game may have let it go since).
void FlashlightEffect::RestorePipBoyLight() {
	if (!hiddenPipBoyLight) return;
	NiNode* playerNode = Player->GetNode();
	for (UINT32 i = 0; playerNode && i < playerNode->m_children.capacity; i++) {
		if (playerNode->m_children.data[i] != hiddenPipBoyLight) continue;
		hiddenPipBoyLight->Dimmer = hiddenPipBoyDimmer;
		hiddenPipBoyLight->Spec = hiddenPipBoySpec;
		if (drivenPipBoyLight) {
			hiddenPipBoyLight->Diff = hiddenPipBoyDiff;
			hiddenPipBoyLight->m_localTransform.pos = hiddenPipBoyLocal;
		}
		break;
	}
	hiddenPipBoyLight = nullptr;
	drivenPipBoyLight = false;
	FlashlightLamp::Release();
}

// The shared TESR_SpotLight* constants (the spot list ShadowManager draws the shadow map for, and the shaders that read them), from the
// light as placed this call: UpdateConstants runs more than once a frame (frame start, ShaderManager::GetNearbyLights in the shadow pass,
// after the world), each reading the bones again.
void FlashlightEffect::PublishLightConstants(bool abActive) {
	if (!abActive) {
		const D3DXVECTOR4 zero(0.0f, 0.0f, 0.0f, 0.0f);
		TheShaderManager->SpotLightPosition[0] = zero;
		TheShaderManager->SpotLightDirection[0] = zero;
		TheShaderManager->SpotLightColor[0] = zero;
		return;
	}
	D3DXVECTOR4& position = TheShaderManager->SpotLightPosition[0];
	position = SpotLight->m_worldTransform.pos.toD3DXVEC4();
	position.w = SpotLight->Spec.r;	// radius
	TheShaderManager->SpotLightDirection[0] = D3DXVECTOR4(
		SpotLight->m_worldTransform.rot.data[0][0],
		SpotLight->m_worldTransform.rot.data[1][0],
		SpotLight->m_worldTransform.rot.data[2][0],
		SpotLight->OuterSpotAngle);	// outside angle of the cone
	TheShaderManager->SpotLightColor[0] = D3DXVECTOR4(SpotLight->Diff.r, SpotLight->Diff.g, SpotLight->Diff.b, SpotLight->Dimmer);
}

// UNOFFICIAL 2026-11-04 (core/FlashlightLamp.h): the Pip-Boy light as the flashlight's real lamp. At the flashlight (its place under the
// player's node worked back from the world position, and the world position itself for this frame), with the beam's reach, its colour's hue
// and Dimmer as the light pass has them (the game decodes a lamp's colour x dimmer through the same gamma), and no shadow of its own
// (the game's env-map fade takes the first lamp WITHOUT one). The cone is FlashlightLamp's, per draw.
void FlashlightEffect::DrivePipBoyLight(NiPointLight* PipLight, const NiNode* PlayerNode, const NiPoint3& Pos, const NiMatrix33& Rot) {
	const NiTransform& parent = PlayerNode->m_worldTransform;
	const float dx = Pos.x - parent.pos.x, dy = Pos.y - parent.pos.y, dz = Pos.z - parent.pos.z;
	const float inv = parent.scale > 1.0e-4f ? 1.0f / parent.scale : 1.0f;
	PipLight->m_localTransform.pos.x = (parent.rot.data[0][0] * dx + parent.rot.data[1][0] * dy + parent.rot.data[2][0] * dz) * inv;
	PipLight->m_localTransform.pos.y = (parent.rot.data[0][1] * dx + parent.rot.data[1][1] * dy + parent.rot.data[2][1] * dz) * inv;
	PipLight->m_localTransform.pos.z = (parent.rot.data[0][2] * dx + parent.rot.data[1][2] * dy + parent.rot.data[2][2] * dz) * inv;
	PipLight->m_worldTransform.pos = Pos;

	const NiColor& c = Settings.Color;
	const float peak = (std::max)((std::max)(c.r, c.g), (std::max)(c.b, 1.0e-4f));
	float gammaLight = 1.0f;
	if (const PBRShaders* pbr = TheShaderManager->Shaders.PBR) if (pbr->Constants.Linear.x > 1.0f) gammaLight = pbr->Constants.Linear.x;
	const float dimmer = powf(Settings.Dimmer, 1.0f / gammaLight);
	PipLight->Diff = NiColor(c.r / peak, c.g / peak, c.b / peak);
	// the reach (all three, as the game's lights), shortened by the beam's Falloff: the game's lamp falloff is the gentle 1 - d^2/r^2
	const float lampReach = Settings.Distance / sqrtf(1.0f + 0.5f * Deferred.Falloff);
	PipLight->Spec = NiColor(lampReach, lampReach, lampReach);
	PipLight->CastShadows = 0;
	PipLight->Dimmer = dimmer;

	const float outer = D3DXToRadian((std::min)((std::max)(Settings.ConeAngle, 1.0f), 89.0f));
	FlashlightLamp::Lamp = PipLight;
	FlashlightLamp::Dimmer = dimmer;
	FlashlightLamp::Position = D3DXVECTOR3(Pos.x, Pos.y, Pos.z);
	D3DXVECTOR3 axis(Rot.data[0][0], Rot.data[1][0], Rot.data[2][0]);
	D3DXVec3Normalize(&FlashlightLamp::Axis, &axis);
	FlashlightLamp::CosOuter = cosf(outer);
	FlashlightLamp::CosInner = cosf(outer * 0.5f);
}
