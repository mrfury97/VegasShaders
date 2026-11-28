// ---- Menu groups -----------------------------------------------------------------------------------------------------
// UNOFFICIAL 2026-11-01 (user: the three fog types "separated with their settings under each collapsing menu", a toggle at the
// top of each that turns the others off; the DXVK settings under their own header in the performance page; "the enabled switch
// at the top"; "tessellation in particular would be pretty confusing to someone outside of our loop"). Only the menu changes:
// every key keeps its name and its section, so saved settings files read as before. On a page, the settings in no group come
// first, then each group under its own collapsing header with its keys in the page's order.

// The fog types: VolumetricFog draws one of them (VolumetricFog.cpp: LegacyFog wins over ClassicLook, ClassicLook 1-2 over the
// current fog). A group's switch picks its type and turns the shader on; unticking the type that is on turns the shader off.
enum class FogType { None = -1, Current, Classic, Legacy };
static const char* const kFogMain = "Shaders.VolumetricFog.Main";
static int s_classicMode = 1;	// the ClassicLook (1 or 2) the Classic switch brings back after another type was picked

static FogType ActiveFog() {
	if (!TheSettingManager->GetMenuShaderEnabled("VolumetricFog")) return FogType::None;
	if (TheSettingManager->GetSettingI(kFogMain, "LegacyFog")) return FogType::Legacy;
	if (TheSettingManager->GetSettingI(kFogMain, "ClassicLook") > 0) return FogType::Classic;
	return FogType::Current;
}

// 2026-11-26 (unsaved marks): the fog picker's mode, and a fog type's own row (on now, or on when saved)
static int FogMode() { return (int)ActiveFog(); }
static bool FogTypeMarked(FogType type) {
	return UnsavedSettings::Any() && (ActiveFog() == type) != ((FogType)SavedModeCached("fog", FogMode) == type);
}

static void SetFogType(FogType type, bool on) {
	const int classic = TheSettingManager->GetSettingI(kFogMain, "ClassicLook");
	if (classic > 0) s_classicMode = classic > 2 ? 2 : classic;
	const bool shaderOn = TheSettingManager->GetMenuShaderEnabled("VolumetricFog");
	if (on) {
		TheSettingManager->SetSetting(kFogMain, "LegacyFog", type == FogType::Legacy);
		TheSettingManager->SetSetting(kFogMain, "ClassicLook", type == FogType::Classic ? s_classicMode : 0);
		if (!shaderOn) TheShaderManager->SwitchShaderStatus("VolumetricFog");
	}
	else if (shaderOn && ActiveFog() == type)
		TheShaderManager->SwitchShaderStatus("VolumetricFog");
	TheSettingManager->LoadSettings();	// the fog reads its type again
}

static void FogSwitch(FogType type) {
	static const char* const labels[] = { "Use the current fog", "Use the classic fog", "Use the legacy fog" };
	bool on = ActiveFog() == type;
	ImGui::BeginDisabled(TheSettingManager->IsShaderForced("VolumetricFog"));
	MenuWidgets::RowBegin(labels[(int)type], MenuWidgets::ResetSlotWidth(), FogTypeMarked(type));	// 2026-11-25 (unsaved marks)
	if (MenuWidgets::Toggle("##fogtype", &on)) {
		PickerWrites writes("fog", FogMode);	// 2026-11-26 (unsaved marks)
		SetFogType(type, on);
	}
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) || MenuWidgets::RowLabelHovered())
		ImGui::SetTooltip("One fog type draws at a time: switching this one on turns the other two off.\n"
			"Switching it off turns the mod's fog off (the VolumetricFog switch in the sidebar).");
}

static void DxvkStatus() {
	if (DxvkCompute::Available())
		ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1.0f), "The DXVK fork is loaded in this game: these settings work.");
	else
		ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "The DXVK fork is not loaded (native D3D9 or stock DXVK): these settings do nothing here.");
}

// 2026-11-04: a group's own on/off switch at the top of it (lamp haze, the GT7 modes), like a fog type's.
struct GroupSwitch {
	const char* Label;
	const char* Tip;
	bool (*Get)();
	void (*Set)(bool on);
};

static bool LampHazeGet() { return TheSettingManager->GetSettingI("Shaders.VolumetricFog.Interiors", "LampHazeEnabled") != 0; }
static void LampHazeSet(bool on) { TheSettingManager->SetSetting("Shaders.VolumetricFog.Interiors", "LampHazeEnabled", on); }
static const GroupSwitch kLampHazeSwitch = { "Lamp haze",
	"A soft glow of the room's lamps in the indoor fog's air (the settings below). Needs the current fog.", LampHazeGet, LampHazeSet };

static bool GT7ExposureGet() { return TheSettingManager->GetSettingI("Shaders.Exposure.Main", "Mode") == 1; }
static bool NightShiftGet() { return TheSettingManager->GetSettingI("Shaders.Tonemapping.Main", "NightShiftEnabled") != 0; }
static void NightShiftSet(bool on) { TheSettingManager->SetSetting("Shaders.Tonemapping.Main", "NightShiftEnabled", on); }
static const GroupSwitch kNightShiftSwitch = { "Night colour shift",
	"In the dark the eye loses colour and sees blues best: dim parts of the picture turn paler and cooler, lamps keep their colour. "
	"The same switch on the Main, Night and Interiors pages.", NightShiftGet, NightShiftSet };

static void GT7ExposureSet(bool on) { TheSettingManager->SetSetting("Shaders.Exposure.Main", "Mode", on ? 1 : 0); }
static const GroupSwitch kGT7ExposureSwitch = { "Use eye adaptation",
	"Exposure Mode 1: auto-exposure and white balance as the eye adapts, in place of the classic exposure (Mode 0). The same switch "
	"on the Main, Night and Interiors pages. Made to go with the Perceptual tone mapper (Tonemapping: TonemappingMode 11). After "
	"Polyphony Digital's SIGGRAPH 2025 paper for Gran Turismo 7.", GT7ExposureGet, GT7ExposureSet };

// 2026-11-25 (unsaved marks): the key behind each group switch (its Get and Set above)
static const char* GroupSwitchScope(const GroupSwitch& sw) {
	if (&sw == &kLampHazeSwitch) return "Shaders.VolumetricFog.Interiors:LampHazeEnabled";
	if (&sw == &kNightShiftSwitch) return "Shaders.Tonemapping.Main:NightShiftEnabled";
	if (&sw == &kGT7ExposureSwitch) return "Shaders.Exposure.Main:Mode";
	return "";
}

static void DrawGroupSwitch(const GroupSwitch& sw) {
	bool on = sw.Get();
	MenuWidgets::RowBegin(sw.Label, MenuWidgets::ResetSlotWidth(), ScopeMarked(GroupSwitchScope(sw)));	// 2026-11-25 (unsaved marks)
	if (MenuWidgets::Toggle("##groupswitch", &on)) {
		sw.Set(on);
		TheSettingManager->LoadSettings();
	}
	if (ImGui::IsItemHovered() || MenuWidgets::RowLabelHovered()) ImGui::SetTooltip("%s", sw.Tip);
}

struct MenuGroup {
	const char* Section;
	const char* Header;
	const char* Keys;	// comma-separated
	FogType     Fog;	// a fog type's switch at the top of the group, or None
	bool        Open;	// open the first time the page is shown (a fog type's group also opens while it is the one on)
	const char* Note;	// a line under the header, or nullptr
	void      (*Status)();	// a live status line under the note, or nullptr
	const GroupSwitch* Switch = nullptr;	// 2026-11-04: the group's own on/off switch at its top, or nullptr
};

static const MenuGroup kMenuGroups[] = {
	{ kFogMain, "Shared by every fog type",
		"Amount, StartDistance, FogSaturation, WeatherImpact, Extinction, Inscattering, DistantFogStart, DistantFogRange, DistantFogBlend, DistantFogHeight",
		FogType::None, true, "How strong the fog is, its colour and the distant haze: the current, classic and legacy fog all use these.", nullptr },
	{ kFogMain, "Current fog",
		"FogVolume, BaseDensity, GameFogFloor, GameFogStart, WeatherFogColor, MorningFogDip, SunriseSunsetBoost, HeightLayer, LayerBase, LayerThickness, LayerFollowsCamera, "
		"HeightFalloff, MaxHeight, HeightInfluence, "
		"WindAngle, WindSpeed, NoiseScale, NoiseStrength, PhaseAsymmetry, SunGlow, ShadowStrength, RainyDisablesSkyFilter, CloudyDisablesSkyFilter, "
		"AerialStrength, AerialRangeStart, AerialStartDistance, AerialTintBlend, AerialDayFadeStart, AerialTintR, AerialTintG, AerialTintB, EdgeAA, MinDensityFloor",
		FogType::Current, false, "This build's fog: drifting patches that sit in the air, darker where the sun's shadow falls. The classic fog uses "
		"these settings too, except FogVolume, SunGlow, GameFogFloor, GameFogStart, WeatherFogColor and the height layer.", nullptr },
	{ kFogMain, "Classic fog (the original look)", "ClassicLook",
		FogType::Classic, false, "The fog as the original mod drew it: patches painted on the surfaces and the game's own sky colour. "
		"ClassicLook 1 = at half resolution (faster), 2 = every pixel at full resolution, as the original did (slower). Its density, "
		"wind and height come from the current fog's settings.", nullptr },
	{ kFogMain, "Legacy fog (for older presets)",
		"LegacyFog, LegacyAs441, MinimumBaseFog, SunPower, HeightFogDensity, HeightFogFalloff, HeightFogDist, HeightFogSkyColor, HeightFogHeight, HeightFogBlend, "
		"HeightFogRolloff, HeightFogSaturation, HeightFogInscattering, FogNight, SimpleFogSkyColor, SimpleFogHeight, SimpleFogBlend",
		FogType::Legacy, false, "The fog from before this build's fog rewrite (a simple fog plus a height fog), for presets made for it "
		"(Uranium Haze and others). It also uses the shared settings above.", nullptr },

	{ "Shaders.VolumetricFog.Interiors", "Shared by every fog type", "Amount, StartDistance, FogSaturation, WeatherImpact, Extinction, Inscattering",
		FogType::None, true, "Indoors: how strong the fog is and its colour, for all three fog types.", nullptr },
	{ "Shaders.VolumetricFog.Interiors", "Current fog",
		"BaseDensity, GameFogFloor, GameFogStart, HeightFalloff, MaxHeight, HeightInfluence, WindAngle, WindSpeed, NoiseScale, NoiseStrength, RainyDisablesSkyFilter, CloudyDisablesSkyFilter",
		FogType::Current, false, "Indoors, for the current fog (the classic fog uses them too).", nullptr },
	{ "Shaders.VolumetricFog.Interiors", "Classic fog (the original look)", "",
		FogType::Classic, false, "Indoors the classic fog uses the current fog's settings above; its mode (ClassicLook) is on the Main page.", nullptr },
	{ "Shaders.VolumetricFog.Interiors", "Legacy fog (for older presets)",
		"MinimumBaseFog, HeightFogDensity, HeightFogFalloff, HeightFogDist, HeightFogHeight, HeightFogBlend, HeightFogRolloff, HeightFogSaturation, "
		"HeightFogInscattering, SimpleFogHeight, SimpleFogBlend",
		FogType::Legacy, false, "Indoors, for the legacy fog (it also uses the shared settings above).", nullptr },
	{ "Shaders.VolumetricFog.Interiors", "Lamp haze", "LampHaze, LampHazeAnisotropy, LampHazeCore, LampHazeMaxLamps",
		FogType::None, false, "The room's lamps light the indoor fog's air: a soft glow round each lamp. Off by default.", nullptr, &kLampHazeSwitch },

	{ "Shaders.Tonemapping.Main", "Night colour shift", "NightShift, NightShiftLevel",
		FogType::None, false, "Dim parts of the picture lose colour and turn cooler, as the eye sees in the dark. Off by default.", nullptr, &kNightShiftSwitch },
	{ "Shaders.Tonemapping.Night", "Night colour shift", "NightShift, NightShiftLevel",
		FogType::None, false, "At night: the strength most people will want to change.", nullptr, &kNightShiftSwitch },
	{ "Shaders.Tonemapping.Interiors", "Night colour shift", "NightShift, NightShiftLevel",
		FogType::None, false, "Indoors (dark rooms).", nullptr, &kNightShiftSwitch },

	{ "Shaders.Exposure.Main", "Eye adaptation", "AdaptExposure, AdaptWhiteBalance, AdaptWeatherLight",
		FogType::None, false, "Auto-exposure and white balance that adapt as the eye does, metering the whole view. While it is on, MinBrightness, "
		"MaxBrightness and the adapt speeds above do nothing. Off by default.", nullptr, &kGT7ExposureSwitch },
	{ "Shaders.Exposure.Night", "Eye adaptation", "AdaptExposure, AdaptWhiteBalance, AdaptWeatherLight",
		FogType::None, false, "At night, with eye adaptation on.", nullptr, &kGT7ExposureSwitch },
	{ "Shaders.Exposure.Interiors", "Eye adaptation", "AdaptExposure, AdaptWhiteBalance, AdaptWeatherLight",
		FogType::None, false, "Indoors, with eye adaptation on.", nullptr, &kGT7ExposureSwitch },

	{ "Main.Main.Performance", "DXVK (Vulkan fork)",
		"NvrDxvkPasses, NvrDxvkVariants, NvrDxvkStencil, NvrDxvkAmbientLight, NvrDxvkClouds, NvrDxvkKeepD3D9, NvrDxvkTestSuffix, NvrDxvkMixCheck, "
		"NvrDxvkReplacements, NvrSm6Shaders, NvrDxvkDisplacement, AvgLumaCompute, AmbientLightSmoothCompute, AmbientLightBlurCompute",
		FogType::None, false, "Settings for the DXVK fork's d3d9.dll (the fork this build ships): the mod's effects as Vulkan passes, tessellation and the like. "
		"Native D3D9 and stock DXVK ignore them.", DxvkStatus },

	{ "Shaders.Terrain.Displacement", "Bump shape", "Strength, Center, Contrast, BumpSize, LodBias",
		FogType::None, true, "How tall the bumps are and how big the smallest one is.", nullptr },
	{ "Shaders.Terrain.Displacement", "Detail and reach", "EdgeLength, EdgeGrowth, MaxFactor, FadeStart, FadeEnd, BorderGrid, FarUntessellated, FarHysteresis",
		FogType::None, false, "How finely the ground near you is cut up (finer = more work for the graphics card) and how far from you the bumps reach.", nullptr },
	{ "Shaders.Terrain.Displacement", "Meeting objects", "ObjectBlend, ObjectBlendWidth, ObjectBlendKeep, ObjectBlendShadows",
		FogType::None, false, "Flatten the bumps where the ground meets rocks, buildings and feet, so those do not sink into it (experimental).", nullptr },
	{ "Shaders.Terrain.Displacement", "Shadows and lighting", "CastShadows, ShadowSlack, BendNormals, ContactShadowsOutside, IgnoreMipBias",
		FogType::None, false, "Whether the bumps cast sun shadows and catch the light as real slopes.", nullptr },
	{ "Shaders.Terrain.Displacement", "Test tools", "FixedFactor, DebugView, Winding",
		FogType::None, false, nullptr, nullptr },

	{ "Shaders.POM.Displacement", "Bump shape", "Strength, Center, Contrast, LodBias",
		FogType::None, true, "How far rock and cliff surfaces move in and out.", nullptr },
	{ "Shaders.POM.Displacement", "Detail and reach", "EdgePixels, EdgeFade, MaxFactor, FadeStart, FadeEnd",
		FogType::None, false, "How finely the objects near you are cut up (finer = more work for the graphics card) and how far from you the bumps reach.", nullptr },
	{ "Shaders.POM.Displacement", "Test tools", "SameHeight, FixedFactor, DebugView, Winding",
		FogType::None, false, nullptr, nullptr },

	// UNOFFICIAL 2026-11-01 (user: "group appropriate options together under collapsing menus" on the other pages)
	// Shaders.Cloth.Main
	{ "Shaders.Cloth.Main", "Which clothes move", "Keywords, Exclude, ExcludeParts, AutoDetect, AutoShare, ForceOff",
		FogType::None, false, "Which garments, flags and banners count as cloth, by texture name or by the shape of their mesh.", nullptr },
	{ "Shaders.Cloth.Main", "Which parts of a garment move",
		"MaskMode, SkinMask, PinFraction, SkinSpace, HeightTop, HeightBottom, LegCut, HeightFromFeet, UseSkeleton, LimbGap, LimbFade, "
		"UseWeights, StillWeights",
		FogType::None, false, "Where a coat is pinned (shoulders, chest) and where it hangs loose (the hem), and what stays still on tents and flags.", nullptr },
	{ "Shaders.Cloth.Main", "Wind",
		"UseGameWind, CalmFactor, WindDirX, WindDirY, WindDirZ, WindSpeed, WindIntensity, Wavelength, FlutterSpeed, FlutterAmount, "
		"RippleAmount, RippleScale",
		FogType::None, true, "How strongly and which way the wind blows the cloth, and the size of its billows and flutter.", nullptr },
	{ "Shaders.Cloth.Main", "Coats follow the wearer",
		"MotionDrag, MotionMax, MotionSpring, MotionDamping, StrideSwing, StrideLength, TurnLag, MotionIndoors",
		FogType::None, false, "The hem trails behind as you walk, swishes with your steps and flares out when you turn.", nullptr },
	{ "Shaders.Cloth.Main", "Walking coats",
		"WalkSim, WalkHang, WalkSpring, WalkDamping, WalkDrag, WalkLegMargin, WalkMaxSwing, WalkMaxAngle, WalkDragCap, WalkHemSmooth",
		FogType::None, false, "A small walking simulation: the coat hangs from the hips and swings instead of riding your legs.", nullptr },
	{ "Shaders.Cloth.Main", "Test tools", "LogAuto, LogMatches, TimeOffset",
		FogType::None, false, nullptr, nullptr },

	// Shaders.VolumetricClouds.Main
	{ "Shaders.VolumetricClouds.Main", "Cover and weather",
		"CoveragePleasant, CoverageCloudy, CoverageRainy, CoverageSnow, WeatherSize, FromWeatherTextures, WeatherTint",
		FogType::None, true, "How much of the sky the clouds fill in each kind of weather, and how closely they follow the weather's own clouds.", nullptr },
	{ "Shaders.VolumetricClouds.Main", "Cloud shape",
		"ShapeSize, DetailSize, Erosion, EdgeSharpness, ShapeSmoothing, Turbulence, DetailSmoothness, FineDetail, FineDetailSize, CumulusShape",
		FogType::None, false, "The size of the clouds, how wispy or crisp their edges are and how much fine detail they keep close up.", nullptr },
	{ "Shaders.VolumetricClouds.Main", "Storm clouds", "Towering, TowerHeight, TowerAnvil",
		FogType::None, false, "A few huge storm clouds towering over the desert, with flat anvil tops.", nullptr },
	{ "Shaders.VolumetricClouds.Main", "Cloud lighting", "SunStrength, AmbientStrength, ForwardScattering",
		FogType::None, false, "How brightly the sun and the sky light the clouds, and their silver linings toward the sun.", nullptr },
	{ "Shaders.VolumetricClouds.Main", "Movement", "WindSpeed, Evolution, GameTime",
		FogType::None, false, "How fast the clouds drift and change shape.", nullptr },
	{ "Shaders.VolumetricClouds.Main", "Cloud shadows",
		"Shadows, ShadowRange, ShadowResolution, ShadowPrefilter, ShadowSoftness, PhysicalShadows, ShadowDebug",
		FogType::None, false, "Drifting cloud shadows on the ground: how dark, how far round you and how soft.", nullptr },
	{ "Shaders.VolumetricClouds.Main", "Game clouds and water", "HideGameClouds, WaterReflections, ReflectionDebug",
		FogType::None, false, "Whether the game's own flat clouds stay, and these clouds in the water's reflection.", nullptr },
	{ "Shaders.VolumetricClouds.Main", "Quality and steadiness",
		"HalfResolution, Resolution, Steps, Sharpness, FixedMarchExit, History, Stable, JitterWrap",
		FogType::None, false, "The resolution and sample count the clouds are worked out at (the main cost), and how steady they stay as you move.", nullptr },

	// Shaders.ShadowsInteriors.Main
	{ "Shaders.ShadowsInteriors.Main", "Lamps",
		"LightPoints, ExteriorLightPoints, LightRadiusMult, DrawDistance, ShadowCubeMapSize, NearFade, EdgeFade",
		FogType::None, true, "How many lamps cast shadows at once, how far away and how detailed their shadows are.", nullptr },
	{ "Shaders.ShadowsInteriors.Main", "What casts shadows",
		"UseCastShadowFlag, Activators, Actors, AlphaEnabled, Apparatus, Books, Containers, Doors, Furniture, Misc, Statics, MinRadius, "
		"PlayerShadowThirdPerson, PlayerShadowFirstPerson, TwoSidedCasters, SkinnedAlphaTest",
		FogType::None, false, "Which kinds of objects are drawn into the lamp shadows.", nullptr },
	{ "Shaders.ShadowsInteriors.Main", "Contact shadows", "ContactShadows, ContactLength, ContactThickness, ContactDistance",
		FogType::None, false, "Small dark lines where a prop meets the floor or a wall.", nullptr },
	{ "Shaders.ShadowsInteriors.Main", "Fixes and speed-ups",
		"MaskSkipFar, LampReachMask, ForwardReachMask, PlayerInsideLamp, PlayerLampMargin, RedrawActorsOnly",
		FogType::None, false, "Bug fixes for lamp reach and lamps inside your body, and fewer shadow redraws when people move.", nullptr },

	// Shaders.ShadowsInteriors.Forward
	{ "Shaders.ShadowsInteriors.Forward", "Soft shadows",
		"ContactHardening, ContactHardeningStrength, ContactHardeningMinSoftness, LampSize, MaxSoftness, SmoothEdges, EvenTaps, "
		"MagnifiedSoftness, TexelFilter",
		FogType::None, true, "Shadows sharp where an object touches what it shadows and softer further away, as from a real lamp.", nullptr },
	{ "Shaders.ShadowsInteriors.Forward", "Which lamps get shadows",
		"LampSwitchMargin, LampRanking, KeepLampsInView, CameraIndependent, VisibleRanking, PoolLamps, PoolDrawsPerFrame",
		FogType::None, false, "How the lamps that cast shadows are picked, and extra lamps beyond the usual limit.", nullptr },
	{ "Shaders.ShadowsInteriors.Forward", "Shadow pop-in", "ShadowHold, SteadyLampShadows, FadeIn, FadeOut",
		FogType::None, false, "Lamp shadows fade in and out instead of popping when a lamp gains or loses its shadow.", nullptr },
	{ "Shaders.ShadowsInteriors.Forward", "Nearby lamps share a shadow",
		"ShadowClusterEnabled, ShadowClusterDistance, ShadowClusterInvisibleOnly, ShadowClusterMinLights",
		FogType::None, false, "Lamps close together share the brightest one's shadow (cheaper in rooms full of lights).", nullptr },
	{ "Shaders.ShadowsInteriors.Forward", "Lamps' own models",
		"SkipOwnFixture, OwnFixtureDistance, LampGlassShadows, OwnGlassDistance, LampModelShadows, LampModelDistance, LampModelOwnDistance, NoShadowLampModels, FixtureDistance",
		FogType::None, false, "Whether a lamp's glass, bulb and fixture shadow its own light.", nullptr },
	{ "Shaders.ShadowsInteriors.Forward", "Invisible and fill lights",
		"InvisibleLightShadowStrength, InvisibleShadowFromLamp, InvisibleLightSize, FillLightRadius, FillLightShadowStrength",
		FogType::None, false, "Shadows from the level designers' invisible lights and big room-filling lights.", nullptr },
	{ "Shaders.ShadowsInteriors.Forward", "Speckles", "Bias, NormalOffset, AutoNormalOffset",
		FogType::None, false, "Keeps lit walls from showing dark speckles where a surface shadows itself.", nullptr },
	{ "Shaders.ShadowsInteriors.Forward", "Room darkening", "KeepDarkening, DarkeningBlur, MaskHalfRes",
		FogType::None, false, "The old darkening of what no shadowed lamp reaches, so rooms keep their mood.", nullptr },

	// Shaders.ShadowsInteriors.Capsules
	{ "Shaders.ShadowsInteriors.Capsules", "People's shadows",
		"DropActors, MaxActors, MaxDistance, PlayerFirstPerson, RadiusScale, Strength, SelfMargin, LampSize, InvisibleLightSize, RankBy",
		FogType::None, true, "Soft lamp shadows of people from simple capsule shapes round their bones.", nullptr },
	{ "Shaders.ShadowsInteriors.Capsules", "Contact shadows",
		"ContactLength, ContactSteps, ContactThickness, ContactStrength, ContactBias, ContactJitter, StartOffset, StartOffsetPerUnit",
		FogType::None, false, "A short ray toward each lamp for the small shadows where things touch.", nullptr },

	// Shaders.ShadowsInteriors.RT
	{ "Shaders.ShadowsInteriors.RT", "Soft shadows", "Rays, LampRadius, InvisibleLightRadius, Jitter",
		FogType::None, true, "The size of the lamp the rays aim at, for soft shadow edges.", nullptr },
	{ "Shaders.ShadowsInteriors.RT", "Smoothing the noise",
		"Denoise, DenoiseHistory, Filter, FilterStep, FilterDepthSigma, FilterNormalPower",
		FogType::None, false, "Smooths the grain of soft traced shadows over frames and neighbouring pixels.", nullptr },
	{ "Shaders.ShadowsInteriors.RT", "Every lamp on screen",
		"ManyLights, ManyLightsMax, ManyLightsRays, ManyLightsSpatial, ManyLightsHistory, ManyLightsRestStride, ManyLightsOneRay",
		FogType::None, false, "Every lamp that lights something on screen gets a traced shadow, not only the ones with shadow maps.", nullptr },
	{ "Shaders.ShadowsInteriors.RT", "Where rays start and stop",
		"StartOffset, StartOffsetPerUnit, LampStop, SkipLampModels, SkipLampGlass, DepthTolerance",
		FogType::None, false, "Keeps surfaces, bulbs and lamp fixtures from shadowing their own light.", nullptr },
	{ "Shaders.ShadowsInteriors.RT", "Outdoors at night", "Exteriors, ExteriorForward, ExteriorLampStop, ExteriorDebug",
		FogType::None, false, "Traced shadows from street lamps and other lights outdoors at night.", nullptr },
	{ "Shaders.ShadowsInteriors.RT", "Test tools", "DebugView, OneSided, DebugLamp",
		FogType::None, false, nullptr, nullptr },

	// Shaders.Bodycam.Main
	{ "Shaders.Bodycam.Main", "Lens",
		"Fisheye, FitEdges, Border, Fringe, EdgeBlur, LensBlur, Vignette, LensDirt, Autofocus, WideView",
		FogType::None, true, "The cheap wide lens: bent edges, colour fringes, blur, dirt and a wide field of view.", nullptr },
	{ "Shaders.Bodycam.Main", "Colour", "Exposure, Contrast, Saturation, Lift, Cool, Tint",
		FogType::None, false, "Brightness, contrast and colour of the footage.", nullptr },
	{ "Shaders.Bodycam.Main", "Bright and low light", "Glow, AutoExposure, ExposureSpeed, Overload, LowLight, LightSmear",
		FogType::None, false, "How the camera copes with bright light, muzzle flashes and the dark.", nullptr },
	{ "Shaders.Bodycam.Main", "Video and compression", "Sharpen, Grain, Blocks, VideoFps, Breakup, Hiccups",
		FogType::None, false, "Sensor noise, compression blocks and the video's frame rate.", nullptr },
	{ "Shaders.Bodycam.Main", "Camera movement", "MotionBlur, RollingShutter, Bounce, GunJolt",
		FogType::None, false, "The chest-mounted camera bobs with your steps, smears and wobbles in fast turns and kicks with each shot.", nullptr },
	{ "Shaders.Bodycam.Main", "Microphone", "Mic, MicTone, MicCompression, MicGain, MicBits, MicSampleRate, MicHiss",
		FogType::None, false, "All the game's sound as if picked up by the camera's own cheap microphone.", nullptr },

	// Shaders.Bodycam.Night
	{ "Shaders.Bodycam.Night", "Lens",
		"Fisheye, FitEdges, Border, Fringe, EdgeBlur, LensBlur, Vignette, LensDirt, Autofocus",
		FogType::None, true, "At night outdoors: the cheap wide lens.", nullptr },
	{ "Shaders.Bodycam.Night", "Colour", "Exposure, Contrast, Saturation, Lift, Cool, Tint",
		FogType::None, false, "At night outdoors: brightness, contrast and colour.", nullptr },
	{ "Shaders.Bodycam.Night", "Bright and low light", "Glow, AutoExposure, ExposureSpeed, Overload, LowLight, LightSmear",
		FogType::None, false, "At night outdoors: bright lights, muzzle flashes and low-light mode.", nullptr },
	{ "Shaders.Bodycam.Night", "Video and compression", "Sharpen, Grain, Blocks, VideoFps, Breakup, Hiccups",
		FogType::None, false, "At night outdoors: noise, compression blocks and frame rate.", nullptr },
	{ "Shaders.Bodycam.Night", "Camera movement", "MotionBlur, RollingShutter, Bounce, GunJolt",
		FogType::None, false, "At night outdoors: bob, smear, wobble and shot kick.", nullptr },

	// Shaders.Bodycam.Interiors
	{ "Shaders.Bodycam.Interiors", "Lens",
		"Fisheye, FitEdges, Border, Fringe, EdgeBlur, LensBlur, Vignette, LensDirt, Autofocus",
		FogType::None, true, "Indoors: the cheap wide lens.", nullptr },
	{ "Shaders.Bodycam.Interiors", "Colour", "Exposure, Contrast, Saturation, Lift, Cool, Tint",
		FogType::None, false, "Indoors: brightness, contrast and colour.", nullptr },
	{ "Shaders.Bodycam.Interiors", "Bright and low light", "Glow, AutoExposure, ExposureSpeed, Overload, LowLight, LightSmear",
		FogType::None, false, "Indoors: bright lights, muzzle flashes and low-light mode.", nullptr },
	{ "Shaders.Bodycam.Interiors", "Video and compression", "Sharpen, Grain, Blocks, VideoFps, Breakup, Hiccups",
		FogType::None, false, "Indoors: noise, compression blocks and frame rate.", nullptr },
	{ "Shaders.Bodycam.Interiors", "Camera movement", "MotionBlur, RollingShutter, Bounce, GunJolt",
		FogType::None, false, "Indoors: bob, smear, wobble and shot kick.", nullptr },

	// Shaders.AmbientOcclusion.RT
	{ "Shaders.AmbientOcclusion.RT", "Sky visibility", "Sky, SkyRays, SkyDistance",
		FogType::None, false, "How much of the sky each spot sees, from rays instead of a top-down map.", nullptr },
	{ "Shaders.AmbientOcclusion.RT", "Bounce light",
		"GI, GIDistance, GIStrength, GIOffscreen, GIScreenLight, GIGameLamps, GIClamp, GIAmbient, GISky, SkyMissSH, GIStrengthNight, GISkyNight, SkyMissNightAmbient, GIScreenAmbientOut, GIDenoiseSunGI, GIDenoiseRoomGI, RoomGIGlow, LegacyTilt, GIStrengthInterior, GIAmbientInterior, GIClampInterior, GIDistanceInterior, GlowGIInterior, GlowDirectInterior, LegacyTiltInterior, GISpatial, GIReuse, GIInterleave, GIMovingHistory, GlowGI, GlowDirect",
		FogType::None, true, "One bounce of light from walls and floors, and glowing signs and windows lighting what is near them.", nullptr },
	{ "Shaders.AmbientOcclusion.RT", "Light that bounces more than once",
		"RadianceCache, CacheStrength, CacheCellSize, CacheRays, CacheFrames, CacheExteriors, CacheSpread, CacheRaysExterior, CacheSpreadExterior",
		FogType::None, false, "A grid of light round you, for light that bounces more than once.", nullptr },

	// Shaders.Reflections.Main
	{ "Shaders.Reflections.Main", "Quality and reach", "MaxDistance, Steps, Thickness, Temporal, BlurRadius, DrawDistance",
		FogType::None, true, "How far the reflections reach and how steady and smooth they look (more steps cost more).", nullptr },
	{ "Shaders.Reflections.Main", "When rays find nothing",
		"SkyFallback, CubeMap, CubeCapture, CubeFade, GridFallback, GridBrightness, CloudSky",
		FogType::None, false, "What a reflection shows where the screen has nothing to mirror: the sky, a live cube of the surroundings or the light grid.", nullptr },
	{ "Shaders.Reflections.Main", "Materials",
		"MaterialAware, MaterialReflect, RoughnessOverrides, MaterialMaxRoughness, MaterialBufferRoughness, GlassReflect",
		FogType::None, false, "Reflections that follow each surface's own material (polished, rough, metal, glass).", nullptr },
	{ "Shaders.Reflections.Main", "Blending with the surface", "ReflectAdd, SpecularOcclusion, SpecularOcclusionLevel, ReplaceForward",
		FogType::None, false, "Whether the reflection replaces or adds to the surface, and darker reflections where the room is dark.", nullptr },

	// Shaders.Reflections.RT
	{ "Shaders.Reflections.RT", "What reflections show off screen",
		"ShadeOffscreen, ScreenTolerance, EstimateBrightness, SunShadows, HitLamps, HitAmbientGrid, HitTextures, GlowBrightness, SkyPanorama",
		FogType::None, true, "How things behind you or hidden from view are lit and coloured when they show in a reflection.", nullptr },
	{ "Shaders.Reflections.RT", "Glossy surfaces",
		"VisibleNormals, RayReuse, ReuseRoughness, ContactSharpness, ContactSpread, RoughShare, RoughReach",
		FogType::None, false, "Blurred reflections on worn tiles, satin metal and wet stone: how sharp they stay and how cheap they are.", nullptr },
	{ "Shaders.Reflections.RT", "Weapon", "Weapon, WeaponHands, WeaponMetal, WeaponRoughness",
		FogType::None, false, "Your first-person weapon reflects the live surroundings cube (or the ray-traced scene when RT is on).", nullptr },
	{ "Shaders.Reflections.RT", "Water", "Water, WaterSkipPlanar, WaterPlanarRefresh, WaterRoughness",
		FogType::None, false, "Water reflects the ray traced scene instead of the game's own water reflection.", nullptr },

	// Shaders.Sandstorm.Main
	{ "Shaders.Sandstorm.Main", "Dust shape", "HeightFalloff, Billowing, NoiseScale, StreakStrength",
		FogType::None, true, "How the dust gathers into rolling clouds and streaks of blown sand.", nullptr },
	{ "Shaders.Sandstorm.Main", "Dust colour and light",
		"DustColorR, DustColorG, DustColorB, Brightness, SunScattering, LightScattering",
		FogType::None, false, "The dust's colour and how it glows toward the sun and round lamps and fires.", nullptr },
	{ "Shaders.Sandstorm.Main", "Wind", "WindSpeed, MinWindSpeed, GrassWind, TreeWind",
		FogType::None, false, "How fast the dust blows and how much harder grass and trees sway in the storm.", nullptr },
	{ "Shaders.Sandstorm.Main", "Fire and glass", "EmissiveThreshold, EmissiveDistance, ThroughBlended, CharactersKeepDepth",
		FogType::None, false, "Keeps fire, sparks and glass from showing through the dust as if it were not there.", nullptr },
	{ "Shaders.Sandstorm.Main", "Speed", "Steps, Octaves, SoftEdges, FineShelter, FastPath",
		FogType::None, false, "Cheaper or finer dust.", nullptr },

	// Shaders.Sky.Main
	{ "Shaders.Sky.Main", "Sky colour",
		"ColorLook, TintDayR, TintDayG, TintDayB, TintNightR, TintNightG, TintNightB, TintKeepsBrightness, SaturationDay, SaturationNight",
		FogType::None, true, "The colour and colourfulness of the sky by day and at night.", nullptr },
	{ "Shaders.Sky.Main", "Horizon colour",
		"TintHorizonDayR, TintHorizonDayG, TintHorizonDayB, TintHorizonNightR, TintHorizonNightG, TintHorizonNightB, TintGradientHeight",
		FogType::None, false, "An extra colour for the sky near the horizon, and how high it reaches.", nullptr },
	{ "Shaders.Sky.Main", "Sunrise and sunset", "UseSunDiskColor, SunsetR, SunsetG, SunsetB",
		FogType::None, false, "The colour the sun gives the sky at sunrise and sunset.", nullptr },
	{ "Shaders.Sky.Main", "Stars and moon", "StarStrength, StarTwinkle, MoonBrightness",
		FogType::None, false, nullptr, nullptr },

	// Shaders.AmbientLight.Main
	{ "Shaders.AmbientLight.Main", "Edges and outlines", "NoEdgeOutline, EdgeReject, LowScaleFullRes, TemporalJitterFix, MinReach, NormalSmoothing, NormalSmoothingWorld, HorizonBias",
		FogType::None, true, "Fixes for thin light outlines round things and dark facets on low-poly surfaces.", nullptr },
	{ "Shaders.AmbientLight.Main", "Light grid bounce",
		"GridBounce, GridCellSize, GridSteps, GridSpread, GridScreenStrength, GridBlocking, GridWallMemory, GridLamps, GridFine, GridColour, AlbedoBuffer, AlbedoFallbackCap",
		FogType::None, false, "Bounce light from a 3D grid of light round you, from the sun by day and the lamps indoors and at night.", nullptr },
	{ "Shaders.AmbientLight.Main", "Screen bounce light",
		"ComputeGI, ComputeGIStrength, ComputeGIThickness, ComputeGIThinOcclusion, ComputeGIQuality, ComputeGITest",
		FogType::None, false, "Light bounced from what is on screen (needs the DXVK fork).", nullptr },

	// Shaders.WetWorld.Main
	{ "Shaders.WetWorld.Main", "Wet ground that lasts",
		"Persistent, PersistentWetTime, PersistentDryTime, PersistentPuddleFill, PersistentPuddleDry, PersistentShade, PersistentMapSize, "
		"PersistentMapResolution, PersistentDebugView",
		FogType::None, false, "Wet ground and puddles are kept spot by spot round you and dry slowly, faster in the sun.", nullptr },
	{ "Shaders.WetWorld.Main", "Not used", "PuddleCoeff_R, PuddleCoeff_G, PuddleCoeff_B, PuddleSpecularMultiplier",
		FogType::None, false, "These settings do nothing.", nullptr },

	// Shaders.Grass.GPUGrass
	{ "Shaders.Grass.GPUGrass", "Distance and detail",
		"BladeDistance, Blades, BladesFar, LodNear, LodFar, DetailDistance, MinPixelWidth, MaxBlades",
		FogType::None, true, "How far the strands reach and how many each clump has near and far (more costs more).", nullptr },
	{ "Shaders.Grass.GPUGrass", "Strand shape", "LengthMin, TipReach, Spread, Bend, Kink, Droop, Flutter",
		FogType::None, false, "How long, bent and spread out the strands are, and their flutter in the wind.", nullptr },
	{ "Shaders.Grass.GPUGrass", "Colour", "Variation, TipLift, Saturation",
		FogType::None, false, nullptr, nullptr },

	// Shaders.PBR.Main
	{ "Shaders.PBR.Main", "Sky light", "SkylightingScale, SkylightingMode, SkylightingDirectionality, SkylightingSaturation",
		FogType::None, true, "How much the sky's colour lights objects from above and around.", nullptr },
	{ "Shaders.PBR.Main", "Hair shine", "HairHighlight, HairHighlightGameLook",
		FogType::None, true, "The hair has its own shader: Reflectivity and Roughness do not reach it. These set its shine.", nullptr },
	{ "Shaders.PBR.Main", "Material maps", "MetalnessScale, AOScale, RoughnessScale",
		FogType::None, false, "For materials with an _rmaos map: how much of their metal, occlusion and roughness applies.", nullptr },
	{ "Shaders.PBR.Main", "Highlights",
		"SmithCorrelated, SmithCorrelatedTest, EnergySplit, SpecularDistance, SpecularFade, FoldHighlights, HighlightSums",
		FogType::None, false, "How the shiny highlights on objects are worked out and how far they reach.", nullptr },
	{ "Shaders.PBR.Main", "Lamp light", "LampDistanceFix, LampSums, LampReachSkip, DxvkLampBranches",
		FogType::None, false, "Fixes and speed-ups for objects lit by several lamps.", nullptr },
	{ "Shaders.PBR.Main", "Shader marking (test)",
		"MarkShadersFrom, MarkShadersTo, MarkShadersVanilla, MarkShadersVsFrom, MarkShadersVsTo, MarkShadersFamily",
		FogType::None, false, nullptr, nullptr },

	// Main.Main.Performance (the DXVK group above stays as it is)
	{ "Main.Main.Performance", "Shader compiling and warm-up",
		"ParallelShaderCompile, NewEffectCompiler, ShaderWarmUp2, ShaderWarmUp2BudgetMs, ShaderWarmUp2MinMs",
		FogType::None, true, "How the mod's shaders are compiled at game start and preloaded against stutter.", nullptr },
	{ "Main.Main.Performance", "ReShade",
		"ReShadeDepthCompat, ReShadeNVRDepth, ReShadeBeforeHUD, ReShadeUnderNVRMenu, ReShadeDepthNearRemap, ReShadeMenuGuard, "
		"ReShadeMenuGuardEffects, ReShadeMenuGuardVATS, ReShadeGameValues, ReShadeDepthCheck",
		FogType::None, false, "For ReShade users: depth for ReShade's effects, ReShade under the HUD and menus, and pausing effects in menus.", nullptr },
	{ "Main.Main.Performance", "CPU savings", "SkipRedundantConstants, SettingsCache, PreDecodeCache, MenuHiddenLean",
		FogType::None, false, "Small savings on the processor: work skipped when nothing changed.", nullptr },
	{ "Main.Main.Performance", "Full-screen effect passes",
		"FrameChain, CompositeApply, ChainUsesGameTexture, MergedDepthNormals, SkipIdentityExposure, SlimDepthBuffer",
		FogType::None, false, "Fewer whole-frame copies and passes between the mod's effects.", nullptr },
	{ "Main.Main.Performance", "Lamp light passes",
		"LampScissor, LampScissorExact, TrimLampPasses, LampMaskScissor, LampMaskScissorCheck, LampPassOrderLog",
		FogType::None, false, "Indoors, each extra lamp's light is drawn only where the lamp reaches.", nullptr },
	{ "Main.Main.Performance", "Indoor lamp shadows",
		"SkinnedShadowFaceTest, FastCasterHash, CasterListCache, InteriorShadowFold, InteriorShadowFoldRound, InteriorShadowFoldCheck, "
		"CubeFaceCopySkip, CubeFaceCopySkipCheck, CubeFaceViewCull, CubeFaceViewCullCheck",
		FogType::None, false, "Less work redrawing the lamp shadows indoors.", nullptr },

	// Main.Main.RayTracing
	{ "Main.Main.RayTracing", "People", "Actors, ActorUpdateInterval, ActorRadius, ActorStillSkip, ActorStillThreshold, ActorRebuildInterval, FaceMorphs, FaceMorphRadius",
		FogType::None, true, "NPCs, creatures and your own body in the ray traced scene, and how often they are updated.", nullptr },
	{ "Main.Main.RayTracing", "Plants and grass", "LeafCards, LeafCardsOpaque, PlantSway, Grass, GrassRadius, GrassMaxTufts",
		FogType::None, false, "Tree leaves, bushes and the grass near you in the ray traced scene.", nullptr },
	{ "Main.Main.RayTracing", "Textures and see-through meshes", "AlphaTested, HitTextures, GlowTextures, Decals",
		FogType::None, false, "Fences and foliage cards with their holes, and each mesh's texture and glow where a ray hits it.", nullptr },
	{ "Main.Main.RayTracing", "Far distance", "FarObjectLOD, FarLandLOD, FarRadius",
		FogType::None, false, "Distant objects and terrain in the ray traced scene (for RT reflections).", nullptr },
	{ "Main.Main.RayTracing", "Building the scene", "BuildTrianglesPerFrame, BuildNearRadius, CullSmallAngle, Compact, ArenaMB",
		FogType::None, false, "How new objects are added to the ray traced scene without stutter, and its memory.", nullptr },
};

static bool KeyInList(const char* list, const char* key) {
	const size_t n = strlen(key);
	for (const char* p = list; *p; ) {
		while (*p == ' ' || *p == ',') p++;
		const char* end = p;
		while (*end && *end != ',') end++;
		const char* last = end;
		while (last > p && last[-1] == ' ') last--;
		if ((size_t)(last - p) == n && strncmp(p, key, n) == 0) return true;
		p = end;
	}
	return false;
}

static const MenuGroup* FindMenuGroup(const std::string& section, const char* key) {
	for (const MenuGroup& g : kMenuGroups)
		if (section == g.Section && KeyInList(g.Keys, key)) return &g;
	return nullptr;
}

// A key its group's switch draws instead (LegacyFog: the legacy fog's switch).
static bool IsGroupSwitchKey(const std::string& section, const char* key) {
	return (section == kFogMain && strcmp(key, "LegacyFog") == 0) ||
		(section == "Shaders.VolumetricFog.Interiors" && strcmp(key, "LampHazeEnabled") == 0) ||	// 2026-11-04: the Lamp haze group's switch
		(section == "Shaders.Exposure.Main" && strcmp(key, "Mode") == 0) ||	// 2026-11-04: the Eye adaptation group's switch
		(section == "Shaders.Tonemapping.Main" && strcmp(key, "NightShiftEnabled") == 0);	// 2026-11-04: the Night colour shift group's switch
}

// What a page is for, in plain words, under its title.
static void TessellationStatus() {
	const RestartSetting* r = FindRestartSetting("Main.Main.Performance", "NvrDxvkDisplacement");
	const bool atStart = r && RestartState(*r, true) != 0.0f;
	const ImVec4 orange(1.0f, 0.6f, 0.2f, 1.0f), green(0.45f, 0.9f, 0.45f, 1.0f);
	if (!DxvkCompute::Available())
		ImGui::TextColored(orange, "Not available in this game: the DXVK fork's d3d9.dll is not loaded (native D3D9 or stock DXVK).");
	else if (!atStart)
		ImGui::TextColored(orange, "Not set up: NvrDxvkDisplacement (Main > Performance > DXVK) was off when the game started. Tick it, press Save, restart the game.");
	else if (!DxvkCompute::DisplaceOn())
		ImGui::TextColored(orange, "NvrDxvkDisplacement is on, but no land shader took the tessellation: this DXVK fork may be older than 1.7-displace.");
	else
		ImGui::TextColored(green, "Set up: Enabled below turns the bumps on and off at once.");
}

struct SectionHelp { const char* Section; const char* Text; void (*Status)(); };
static const SectionHelp kSectionHelp[] = {
	{ "Shaders.Terrain.Displacement",
		"Tessellation gives the ground real 3D bumps (stones, ruts, mounds) instead of only painted-on depth: near you the land is cut "
		"into many small triangles and each point is raised or lowered by the ground texture's height map, so the bumps show against the "
		"sky, cast shadows and catch the light. Two switches: NvrDxvkDisplacement (Main > Performance > DXVK) sets it up when the game "
		"starts, and Enabled below turns the bumps on and off at once. Needs the DXVK fork's d3d9.dll. Off by default while it is being "
		"finished: the bumps can still flicker round objects.", TessellationStatus },
	{ "Shaders.POM.Displacement",
		"The same tessellation as the ground's (Terrain > Tessellation) for rocks, cliffs and the other objects with a parallax height "
		"map: their outlines get real bumps. NvrDxvkDisplacement (Main > Performance > DXVK) sets it up when the game starts; Enabled "
		"below turns the object bumps on and off at once, apart from the ground's. Needs the DXVK fork's d3d9.dll. Experimental.", TessellationStatus },
	{ kFogMain,
		"Three fog types, one at a time: the current fog (this build's), the classic fog (the original look) and the legacy fog (for "
		"older presets). Tick \"Use ...\" at the top of the one you want: the other two turn off. The shared settings apply to all three.", nullptr },
	{ "Shaders.VolumetricFog.Interiors",
		"The fog indoors. The fog type (current, classic or legacy) is the same indoors and out: the switches here and on the Main page "
		"are the same ones.", nullptr },
};

// A page's help text (kSectionHelp), wrapped at the page's edge, with its live status line under it.
static void RenderSectionHelp(const std::string& section) {
	for (const SectionHelp& help : kSectionHelp) {
		if (section != help.Section) continue;
		ImGui::PushTextWrapPos(0.0f);	// (TextColored, used by the status lines, does not wrap by itself)
		ImGui::TextUnformatted(help.Text);
		if (help.Status) help.Status();
		ImGui::PopTextWrapPos();
		ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.5f));
	}
}

// What ImGui shows of a label: the part before "##".
static std::string ShownText(const std::string& label) {
	const size_t hidden = label.find("##");
	return hidden == std::string::npos ? label : label.substr(0, hidden);
}

static std::string MenuEntryLabel(const std::string& name);

// The shader a Shaders.<Name>.<Page> section belongs to, or "" for any other section.
static std::string ShaderOfPage(const std::string& section) {
	if (section.find("Shaders") != 0) return std::string();
	const size_t first = section.find('.');
	const size_t second = section.find('.', first + 1);
	if (first == std::string::npos || second == std::string::npos) return std::string();
	return section.substr(first + 1, second - first - 1);
}

// The shader's own switch at the right end of a shader page's title line: the whole shader (every section of it), not just
// the page shown. Insert does the same while the page is open.
static void RenderShaderHeaderSwitch(const std::string& shaderName) {
	const char* shader = shaderName.c_str();
	const SettingManager::MenuSwitch kind = TheSettingManager->GetMenuShaderSwitch(shader);
	if (kind == SettingManager::MenuSwitch::None) return;	// UNOFFICIAL 2026-10-19: an entry without a switch gets none
	bool on = TheSettingManager->GetMenuShaderEnabled(shader);
	const bool forced = TheSettingManager->IsShaderForced(shader);

	// Effects read at start (Main.Enabled or Status.Enabled): show the restart note left of the switch
	RestartNote note;
	const std::string switchSec = "Shaders." + shaderName + (kind == SettingManager::MenuSwitch::MainEnabled ? ".Main" : ".Status");
	const bool noted = FindRestartNote(switchSec.c_str(), "Enabled", note);
	const float gap = ImGui::GetStyle().ItemSpacing.x;
	const float wanted = MenuWidgets::ToggleWidth() + (noted ? ImGui::CalcTextSize(note.text).x + gap : 0.0f);
	ImGui::SameLine();
	const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - wanted;
	if (right > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(right);
	if (noted) {
		DrawRestartNote(note);
		ImGui::SameLine();
	}
	ImGui::BeginDisabled(forced);
	if (MenuWidgets::Toggle("##headerSwitch", &on) && (!on || ConfirmEffectOn(shaderName))) {	// (2026-11-05: conflicts ask first)
		TheShaderManager->SwitchShaderStatus(shader);
		TheSettingManager->LoadSettings();
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Turns all of %s on or off (every %s section), not just this section (key: Insert).\n"
			"This section's own switches are in the list below.", shader, shader);
	ImGui::EndDisabled();
}

// ---- Path tracing notes (UNOFFICIAL 2026-11-12, core/PtApplies.h) ----------------------------------------------------------
// A tester: with path tracing on "a whole swath of shaders and settings do not appear to work". While the path traced picture
// replaces the scene, the menu says what each effect does meanwhile, from one table: a line at the top of the effect's pages
// (DrawPtNote), a short tag beside its name in the effects list (DrawPtTag) and the whole table on the Ray tracing page
// (DrawPtOverview). View > Path tracing notes ([Main.Menu.Developer] PathTracingNotes) hides them; path tracing off: nothing shows.
static bool PtNotesOn() { return gMenu.ptNotes && RtPathTrace::Replacing(); }

// name: an effect's entry ("Bloom") or a page's section ("Shaders.Bloom.Main")
static void DrawPtNote(const std::string& name) {
	if (!PtNotesOn()) return;
	const PtApplies::Row* row = PtApplies::Find(name);
	if (!row) return;
	const PtApplies::Now now = PtApplies::Resolve(*row);
	ImGui::PushTextWrapPos(0.0f);
	if (now.St == PtApplies::Applies) ImGui::TextDisabled("Path tracing is on. %s", now.Line);
	else ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Path tracing is on. %s", now.Line);
	ImGui::PopTextWrapPos();
	ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.5f));
}

static void DrawPtTag(const std::string& effect) {
	if (!PtNotesOn()) return;
	const PtApplies::Row* row = PtApplies::Find(effect);
	if (!row) return;
	const PtApplies::Now now = PtApplies::Resolve(*row);
	if (now.St == PtApplies::Applies) return;	// works as always: no tag
	ImGui::SameLine();
	ImGui::TextDisabled("%s", PtApplies::kTags[now.St]);
	if (ImGui::IsItemHovered()) WrappedTooltip(now.Line);
}

// The Ray tracing page's block: every row of the table by state. Shown while path tracing is switched on or replaces the scene.
static void DrawPtOverview() {
	if (!gMenu.ptNotes || !(RtPathTrace::Replacing() || TheSettingManager->GetSettingI("Main.Main.RayTracing", "PTRealtime") != 0)) return;
	if (!MenuWidgets::SubHeader("What still applies with path tracing###ptapplies", false)) return;
	ImGui::PushTextWrapPos(0.0f);
	ImGui::TextDisabled("What each effect does while the path traced picture replaces the scene. Its own page says the same at the top.");
	static const PtApplies::State kOrder[] = { PtApplies::Replaced, PtApplies::Off, PtApplies::Missing, PtApplies::Twice, PtApplies::Partly,
		PtApplies::Feeds, PtApplies::Applies };
	for (const PtApplies::State state : kOrder) {
		bool headed = false;
		auto line = [&](const std::string& text) {
			if (!headed) {
				headed = true;
				ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.5f));
				ImGui::PushStyleColor(ImGuiCol_Text, MenuSkin::Heading(0.25f));
				ImGui::TextUnformatted(PtApplies::kHeads[state]);
				ImGui::PopStyleColor();
			}
			ImGui::Bullet();
			ImGui::SameLine();
			ImGui::TextUnformatted(text.c_str());
		};
		for (const PtApplies::Row& r : PtApplies::kRows) {
			const PtApplies::Now now = PtApplies::Resolve(r);
			if (now.St == state) line((r.Label ? std::string(r.Label) : ShownText(MenuEntryLabel(r.Name))) + ": " + now.Line);
		}
		if (state == PtApplies::Applies) {
			std::string names;
			PtApplies::EachName(PtApplies::kOnTop, [&](const std::string& n) { names += (names.empty() ? "" : ", ") + ShownText(MenuEntryLabel(n)); });
			line("Everything drawn on the finished picture: " + names + ".");
		}
	}
	ImGui::PopTextWrapPos();
	ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y));
}

// A page's title area: the shader's (or the section's) name, large, the shader's switch at the right, and the section's full
// path as a dim line under it.
static void RenderPageHeader(const std::string& section) {
	const std::string shader = ShaderOfPage(section);
	const std::string title = shader.empty() ? section.substr(section.rfind('.') + 1) : ShownText(MenuEntryLabel(shader));
	std::string path;
	for (char c : section) path += c == '.' ? std::string(" > ") : std::string(1, c);

	ImGui::PushStyleColor(ImGuiCol_Text, MenuSkin::Heading(0.25f));
	MenuWidgets::Title(title.c_str(), 1.3f, !shader.empty() && EffectSwitchMarked(shader));	// 2026-11-25 (unsaved marks): the title names the switch beside it
	ImGui::PopStyleColor();
	if (!shader.empty()) RenderShaderHeaderSwitch(shader);
	ImGui::TextDisabled("%s", path.c_str());
	ImGui::Separator();
	ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.5f));
	if (section == "Shaders.HDR.Main") {
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextColored(kRestartOrange, "Warning: HDR output currently causes freezes for some players. A fix is being researched.");
		ImGui::PopTextWrapPos();
		ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.4f));
	}
	DrawPtNote(section);	// UNOFFICIAL 2026-11-12: what this effect does while path tracing replaces the scene (nothing otherwise)
}

// ---- The LUT page --------------------------------------------------------------------------------------------------------
// DayLUT, NightLUT and InteriorLUT are not rows (ShouldHideKey): each is a picker that steps through the LUT files and loads
// the one shown. Their values at menu open are kept here, as the rows keep theirs.

static void RenderLutPicker(LUTEffect* lut, const char* label, const char* key, int& index, int slot) {
	const std::string& atOpen = SessionValue("Shaders.LUT.Main", key, lut->LUTFiles[index].c_str());
	const bool changed = lut->LUTFiles[index] != atOpen;
	ImGui::PushID(key);
	const float width = MenuWidgets::RowBegin(label, MenuWidgets::ResetSlotWidth(), UnsavedSettings::Key("Shaders.LUT.Main", key));	// 2026-11-25 (unsaved marks)
	int step = 0;
	ImGui::BeginGroup();
	if (ImGui::ArrowButton("##prev", ImGuiDir_Left)) step = -1;
	ImGui::SameLine();
	const float arrowW = ImGui::GetFrameHeight();
	const float nameW = ImMax(width - arrowW * 2.0f - ImGui::GetStyle().ItemSpacing.x * 2.0f, arrowW);
	// UNOFFICIAL 2026-11-11: the name is a drop-down list of every LUT file (with a filter box: LUT packs hold a hundred and more);
	// the arrows still step to the neighbour, to flick through looks.
	int pick = -1;
	ImGui::SetNextItemWidth(nameW);
	if (ImGui::BeginCombo("##list", lut->LUTFiles[index].c_str(), ImGuiComboFlags_HeightLarge)) {
		static char filter[64] = "";
		if (ImGui::IsWindowAppearing()) { filter[0] = 0; ImGui::SetKeyboardFocusHere(); }
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::InputTextWithHint("##lutfilter", "Type to filter...", filter, sizeof(filter));
		for (int i = 0; i < (int)lut->LUTFiles.size(); i++) {
			const std::string& name = lut->LUTFiles[i];
			if (filter[0]) {	// case-insensitive "contains"
				bool hit = false;
				const size_t n = strlen(filter);
				for (size_t at = 0; at + n <= name.size() && !hit; at++) hit = _strnicmp(name.c_str() + at, filter, n) == 0;
				if (!hit) continue;
			}
			const bool selected = i == index;
			if (ImGui::Selectable(name.c_str(), selected)) pick = i;
			if (selected && ImGui::IsWindowAppearing()) ImGui::SetScrollHereY(0.5f);
		}
		ImGui::EndCombo();
	}
	ImGui::SameLine();
	if (ImGui::ArrowButton("##next", ImGuiDir_Right)) step = 1;
	ImGui::EndGroup();
	if (ImGui::IsItemHovered() && (gMenu.plusKey || gMenu.minusKey)) step = gMenu.plusKey ? 1 : -1;
	if (MenuWidgets::ResetButton("~", changed, "Revert to value at session start"))
		lut->LoadLUT(slot, atOpen.c_str());	// LoadLUT puts index and the texture in step (AssignLUTSlot)
	if (pick >= 0 && pick != index) {
		index = pick;
		lut->LoadLUT(slot, lut->LUTFiles[index].c_str());
	}
	else if (step) {
		const int count = (int)lut->LUTFiles.size();
		index = ((index + step) % count + count) % count;
		lut->LoadLUT(slot, lut->LUTFiles[index].c_str());
	}
	ImGui::PopID();
}

// Above the LUT page's settings: what the pre-tonemapping mode expects, then the three pickers.
static void RenderLutPage() {
	LUTEffect* lut = TheShaderManager->Effects.LUT;
	if (!lut) return;
	if (lut->Settings.PreTonemapping) {
		const bool compat = lut->Settings.HDRCompat;
		ImGui::TextColored(compat ? ImVec4(1.0f, 0.85f, 0.2f, 1.0f) : ImVec4(1.0f, 0.5f, 0.2f, 1.0f), "%s", compat
			? "HDR Compat: SDR LUTs normalized to HDR range (approximate)."
			: "Pre-tonemapping mode: use HDR LUTs (float16 DDS converted from .CUBE).");
	}
	if (lut->LUTFiles.empty()) {
		ImGui::TextDisabled("No LUTs found in %s", LUTEffect::LUTFolder());
	}
	else {
		RenderLutPicker(lut, "Day", "DayLUT", lut->DayIdx, 0);
		RenderLutPicker(lut, "Night", "NightLUT", lut->NightIdx, 1);
		RenderLutPicker(lut, "Interior", "InteriorLUT", lut->InteriorIdx, 2);
	}
	ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y));
}

// Puts the pinned settings first, then the page's own Enabled switch (both orderings are stable).
static void PageOrder(SettingManager::Configuration::SettingList& settings) {
	// The settings players look for first go to the top of their section; the rest stay alphabetical.
	static const char* const kPinnedFirst[][2] = {
		{ "Shaders.PBR.Main", "ReflectivityExterior" },
		{ "Shaders.PBR.Main", "ReflectivityInterior" },
	};
	std::stable_partition(settings.begin(), settings.end(), [](const SettingManager::Configuration::ConfigNode& n) {
		for (const auto& pin : kPinnedFirst)
			if (gMenu.page == pin[0] && strcmp(n.Key, pin[1]) == 0) return true;
		return false;
	});
	// A page's own on/off switch comes before everything else.
	std::stable_partition(settings.begin(), settings.end(), [](const SettingManager::Configuration::ConfigNode& n) {
		return strcmp(n.Key, "Enabled") == 0;
	});
}

// What the page needs to know about one of its settings. Worked out once per frame (see RenderSettingsList).
struct PageEntry {
	const MenuGroup* group;	// the menu group it is drawn under, or nullptr for the ungrouped part
	bool             hidden;	// never drawn as a row (ShouldHideKey; 2026-11-12: an Internal setting, InternalHidden)
	bool             test;	// a test/developer setting (IsTestSetting)
};

// Draws every setting of the page: the ungrouped ones first, then each menu group under its own header.
// The group, hidden and test facts of each setting are computed once per frame, not in every pass over the list.
// The simple view of a page (kSimpleRows): its rows in the table's order, and how many more Advanced shows.
// True while a folder shown again on the Ray tracing page draws only some of a section's keys: groups with none of them are left out,
// the fog-type switches are not drawn and the simple view's multi-key choices are left to the
// section's own page.
static bool s_keySubset = false;
static bool s_inCategoryPage = false;

static void RenderSimpleList(SettingManager::Configuration::SettingList& settings, bool isShader) {
	int shown = 0, rows = 0;
	for (const auto& s : settings) if (!ShouldHideKey(s.Key) && !IsTestSetting(s)) rows++;
	for (const SimpleRow& r : kSimpleRows) {
		if (gMenu.page != r.Section) continue;
		if (!r.Key) {
			if (!s_keySubset) DrawSimpleChoice(r);
			continue;
		}
		for (size_t i = 0; i < settings.size(); i++) {
			if (strcmp(settings[i].Key, r.Key)) continue;
			if (InternalHidden(settings[i].Section, settings[i].Key) || IsDeadSetting(settings[i].Section, settings[i].Key)) continue;	// UNOFFICIAL 2026-11-12: not in a simple view either
			// A colour (its R key listed): the page's colour wheel for R, G and B, under the plain name
			const std::string key(r.Key), prefix = key.substr(0, key.size() - 1);
			SettingManager::Configuration::ConfigNode *g = nullptr, *b = nullptr;
			if (isShader && key.size() > 1 && key.back() == 'R')
				for (auto& other : settings) {
					if (prefix + "G" == other.Key) g = &other;
					if (prefix + "B" == other.Key) b = &other;
				}
			if (g && b) {
				RenderColorTriple(settings[i], *g, *b, prefix, r.Label);
				shown += 3;
			}
			else {
				RenderSetting(settings[i], isShader);
				shown++;
			}
		}
	}
	if (!s_inCategoryPage) {
		ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y));
		if (rows > shown)
			ImGui::TextDisabled("%d more setting%s here with Advanced on (top row)", rows - shown, rows - shown == 1 ? "" : "s");
	}
}

static void RenderSettingsList(SettingManager::Configuration::SettingList& settings) {
	const bool isShader = (gMenu.page.find("Shaders") == 0);
	const bool devShown = DevOptionsShown();
	if (!AdvancedShown() && HasSimpleView(gMenu.page)) return RenderSimpleList(settings, isShader);	// 2026-11-05

	std::vector<PageEntry> facts;
	facts.reserve(settings.size());
	for (const auto& s : settings)
		facts.push_back({ FindMenuGroup(gMenu.page, s.Key), ShouldHideKey(s.Key) || InternalHidden(s.Section, s.Key), IsTestSetting(s) });	// (2026-11-12: Internal)

	// Where each key is in the list, to find the R/G/B siblings of a colour triple
	std::unordered_map<std::string, size_t> position;
	for (size_t i = 0; i < settings.size(); i++) position[settings[i].Key] = i;

	std::unordered_set<std::string> handled;
	int testHidden = 0, shown = 0;	// UNOFFICIAL 2026-10-19 menu honesty

	auto drawOne = [&](size_t i) {
		SettingManager::Configuration::ConfigNode& s = settings[i];
		const std::string key(s.Key);
		if (handled.count(key)) return;
		if (facts[i].hidden) return;
		if (IsGroupSwitchKey(gMenu.page, s.Key)) return;	// drawn as its group's switch
		if (!devShown && facts[i].test) { testHidden++; return; }	// UNOFFICIAL 2026-10-19: test switches, dead keys (2026-10-25: + DevOnly)
		shown++;

		// HDRCompat only means something with pre-tonemapping on
		const LUTEffect* lut = TheShaderManager->Effects.LUT;
		if (lut && !lut->Settings.PreTonemapping && gMenu.page == "Shaders.LUT.Main" && !strcmp(s.Key, "HDRCompat")) return;

		// <prefix>R/G/B on a shader page: one colour wheel. The list is alphabetical (B, G, R), so whichever channel comes
		// first starts it.
		const char last = key.empty() ? '\0' : key.back();
		const bool channel = key.size() > 1 && (last == 'R' || last == 'G' || last == 'B');
		if (isShader && channel) {
			const std::string prefix = key.substr(0, key.size() - 1);
			const std::string names[3] = { prefix + "R", prefix + "G", prefix + "B" };
			const bool curve = prefix == "ColorCurve" && strstr(s.Section, "Shaders.Coloring");	// ColorCurveR/G/B are three curves
			const bool complete = position.count(names[0]) && position.count(names[1]) && position.count(names[2]);
			if (!curve && complete && !handled.count(names[0])) {
				for (const std::string& n : names) handled.insert(n);
				RenderColorTriple(settings[position[names[0]]], settings[position[names[1]]], settings[position[names[2]]], prefix);
				return;
			}
		}
		RenderSetting(s, isShader);
	};

	for (size_t i = 0; i < settings.size(); i++)
		if (!facts[i].group) drawOne(i);

	for (const MenuGroup& g : kMenuGroups) {
		if (gMenu.page != g.Section) continue;
		// A group with nothing to show (all its keys developer-only) is left out, unless it carries a fog switch.
		bool any = !s_keySubset && (g.Fog != FogType::None || g.Switch);
		for (size_t i = 0; i < settings.size(); i++)
			if (!any && facts[i].group == &g && !facts[i].hidden && (devShown || !facts[i].test)) any = true;
		if (!any) {
			for (size_t i = 0; i < settings.size(); i++)
				if (facts[i].group == &g && !facts[i].hidden) testHidden++;
			continue;
		}
		const bool active = (g.Fog != FogType::None && ActiveFog() == g.Fog) || (g.Switch && g.Switch->Get());
		const std::string header = std::string(g.Header) + (active ? "   (on)" : "") + "###group" + g.Header;
		// 2026-11-25 (unsaved marks): one of the group's rows, or its switch, is unsaved
		bool unsaved = (g.Switch && ScopeMarked(GroupSwitchScope(*g.Switch))) || (g.Fog != FogType::None && !s_keySubset && FogTypeMarked(g.Fog));
		for (size_t i = 0; i < settings.size() && !unsaved; i++)
			unsaved = facts[i].group == &g && !facts[i].hidden && UnsavedSettings::Key(settings[i].Section, settings[i].Key);
		if (!MenuWidgets::SubHeader(header.c_str(), g.Open || active, unsaved)) {
			for (size_t i = 0; i < settings.size(); i++)
				if (facts[i].group == &g && !facts[i].hidden && !devShown && facts[i].test) testHidden++;
			continue;
		}
		ImGui::PushID(g.Header);
		if (g.Fog != FogType::None && !s_keySubset) FogSwitch(g.Fog);
		if (g.Switch) DrawGroupSwitch(*g.Switch);
		ImGui::PushTextWrapPos(0.0f);	// the note and the status line wrap at the page's edge
		if (g.Note) ImGui::TextDisabled("%s", g.Note);
		if (g.Status) g.Status();
		ImGui::PopTextWrapPos();
		for (size_t i = 0; i < settings.size(); i++)
			if (facts[i].group == &g) drawOne(i);
		ImGui::PopID();
	}

	if (!testHidden) return;
	ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y));
	if (shown) ImGui::TextDisabled("%d developer setting%s hidden (View > Developer options)", testHidden, testHidden == 1 ? "" : "s");
	else ImGui::TextDisabled("Only developer settings here: %d hidden (View > Developer options)", testHidden);
}

// UNOFFICIAL 2026-10-25: settings-only Shaders entries shown under the entry they belong with, so the list does not hold pages that
// read like effects of their own (a tester could not tell the flat clouds' shadows from the volumetric clouds' ones, and Foliage
// stood out as the one entry with no switch). Only the menu moves: their settings keep their own sections ([Shaders.<name>.Main]),
// so saved settings and the toml are unchanged; each page keeps its header (Cloud Shadows its on/off button).
struct MenuMove { const char* name; const char* under; const char* label; };
static const MenuMove kMenuMoves[] = {
	{ "CloudShadows", "ShadowsExteriors", "Cloud Shadows (flat clouds)" },
	{ "ContactHardening", "ShadowsExteriors", "Contact hardening (soft sun edges)" },
	{ "Foliage", "Plants", "Foliage glow" },
	{ "DustMotes", "VolumetricFog", "Dust motes" },	// 2026-11-04 (user: "make motes live under vol fog")
	{ "LocalExposure", "Exposure", "Local exposure" },	// 2026-11-04 (it works with the exposure)
};
static const char* const kDeferredTabId = "@DeferredRenderer";
static const char* const kDebugTabId = "@DebugSettings";

// Test shots (gMenu.revealPage): whether this node leads to the page shown, so the shot's sidebar shows it.
static bool LeadsToSelected(const std::string& path) {
	if (gMenu.page.compare(0, path.size() + 1, path + ".") == 0) return true;
	if ((gMenu.page == kDeferredTabId || gMenu.page == kDebugTabId) && (path == "Main" || path == "Main.Main")) return true;
	for (const MenuMove& m : kMenuMoves)
		if (gMenu.page == std::string("Shaders.") + m.name + ".Main" && (path == "Shaders" || path == std::string("Shaders.") + m.under)) return true;
	return false;
}
// UNOFFICIAL 2026-10-27 (user: "why does sun far have its own ticker box in the list of shaders?" / "put sun far under shadows ext"):
// whole shader entries, their own on/off box included, shown inside another entry instead of at the top of the list. SunFar has no
// settings page (only [Shaders.SunFar.Status] Enabled): it is the pass that draws the mountain and flat-cloud shadows.
struct MenuNest { const char* name; const char* under; };
static const MenuNest kMenuNests[] = {
	{ "SunFar", "ShadowsExteriors" },
};
static bool IsMovedShaderEntry(const std::string& name) {
	for (const MenuMove& m : kMenuMoves) if (name == m.name) return true;
	for (const MenuNest& n : kMenuNests) if (name == n.name) return true;
	return false;
}

// Recursive sidebar tree — path is the full dotted section path, name is the display label.
// UNOFFICIAL 2026-10-27: the menu name of a Shaders entry where its internal name (its settings sections, kept for the users'
// settings files) says less than it does (user: "change the taa menu to say taa / fsr / dlss"). The ## part keeps ImGui's ID the name.
static std::string MenuEntryLabel(const std::string& name) {
	if (name == "HDR") return "HDR (freeze warning)##HDR";
	if (name == "TAA") return "TAA / FSR / DLSS##TAA";
	if (name == "SunFar") return "SunFar (mountain and flat-cloud shadow pass)##SunFar";
	// UNOFFICIAL 2026-11-07 (user: "why is the exterior setting underneath interior"): the two shadow effects are named after the game's
	// old split, but ShadowsInteriors is the LAMPS' shadows (indoors and, with its RT section, street lamps outdoors at night) and
	// ShadowsExteriors the sun's; the sections keep their names in the settings files
	if (name == "ShadowsInteriors") return "Lamp shadows (ShadowsInteriors)##ShadowsInteriors";
	if (name == "ShadowsExteriors") return "Sun shadows (ShadowsExteriors)##ShadowsExteriors";
	if (name == "WorldSurface") return "World surface detail##WorldSurface";
	return name;
}
// The same for a page in the sidebar (by its full path; the section keeps its name in the settings files).
static std::string MenuSectionLabel(const std::string& path, const std::string& name) {
	if (path == "Shaders.Terrain.Displacement") return "Tessellation (displacement)##" + name;	// UNOFFICIAL 2026-10-27 (user)
	if (path == "Shaders.POM.Displacement") return "Tessellation (displacement)##" + name;	// UNOFFICIAL 2026-10-28 rocks and cliffs
	return name;
}

// ---- The Ray tracing page ------------------------------------------------------------------------------------------------
// 2026-11-05 (docs/next/SETTINGS-GAME-MENU-PLAN.md phase 1; user: "the rt effects should live in their own menu all together for
// easier testing"): every ray-traced effect on one page: a status line each (running, or what stops it: testers' "RT does nothing"
// reports were nearly all another setting in the way), one switch for all of them (A/B), one picker for their debug views, then the
// five RT sections' own settings. Only the menu moves: the settings keep their sections in the toml, and those pages leave the sidebar.

static const char* const kRtTabId = "@RayTracing";

struct RtSection { const char* Section; const char* Title; const char* Note; };
static const RtSection kRtSections[] = {
	{ "Shaders.ShadowsExteriors.RT", "Sun shadows", "Traced sun shadows outdoors, combined with the shadow maps or replacing them near you (Mode)." },
	{ "Shaders.ShadowsInteriors.RT", "Lamp shadows", "Traced shadows from the lamps instead of cube maps. Enabled needs a restart; Live switches back and forth at once." },
	{ "Shaders.AmbientOcclusion.RT", "Ambient occlusion and bounce light",
		"Traced occlusion in place of the screen estimate, and with GI one bounce of light (more with RadianceCache)." },
	{ "Shaders.Reflections.RT", "Reflections", "Rays for what the screen-space reflections miss: things off screen or hidden. Needs Reflections on." },
	// UNOFFICIAL 2026-11-07 (ptaaa-fog)
	{ "Shaders.VolumetricLight.RT", "Light shafts and fog", "Outdoors only: the sun's shadow in the air (the light shafts and the fog's sunlight) traced with the same scene as "
		"the other ray-traced effects. It uses ray tracing only while another ray-traced effect is on, and changes only where the shafts' shadows fall: "
		"their strength is Volumetric light's. Indoors the shafts are Volumetric fog's lamp haze (LampHazeEnabled, LampHazeShadows)." },
	{ "Main.Main.RayTracing", "Scene and path tracing", "What goes into the traced scene (people, plants, see-through meshes, far distance), and the path tracer." },
};

static bool IsRtSection(const std::string& path) {
	for (const RtSection& s : kRtSections) if (path == s.Section) return true;
	return false;
}

static bool RtOn(const char* section, const char* key) { return TheSettingManager->GetSettingI(section, key) != 0; }

// One effect's line: running (green), off (dim), or on but something stops it or makes it weak (amber), with the reason.
enum class RtState { Off, Running, Blocked };
struct RtStatusLine { RtState State; std::string Text; std::string Detail; };

static RtStatusLine RtCoreLine() {
	const char* status = RtCore::StatusText();
	const std::string text = status && status[0] ? status : "not started (no ray-traced effect is on yet)";
	return { RtCore::Running() ? RtState::Running : RtState::Off, text, std::string() };
}

// UNOFFICIAL 2026-11-15 (RT master switches) an effect that is on but has no running core: the master switch for this space, or the core
static RtStatusLine RtNotRunningLine(const std::string& detail) {
	if (RtCore::Gated())
		return { RtState::Off, TheShaderManager->GameState.isExterior ? "off here: Ray tracing outdoors is off (the normal effect is used)" :
			"off here: Ray tracing indoors is off (the normal effect is used)", detail };
	return { RtState::Blocked, "on, but the ray tracing core is not running (first line)", detail };
}

static RtStatusLine RtSunLine() {
	const char* s = "Shaders.ShadowsExteriors.RT";
	const std::string detail = RtSunShadows::StatusText();
	if (!RtOn(s, "Enabled")) return { RtState::Off, "off", detail };
	if (!RtCore::Running()) return RtNotRunningLine(detail);
	if (!TheShaderManager->GameState.isExterior) return { RtState::Off, "indoors: the sun is traced outdoors only", detail };
	ShadowsExteriorEffect* shadows = TheShaderManager->Effects.ShadowsExteriors;
	if (!shadows || !shadows->Enabled || !shadows->Settings.Exteriors.Enabled)
		return { RtState::Off, "off: sun shadows are disabled", detail };
	return { RtState::Running, TheSettingManager->GetSettingI(s, "Mode") == 1 ? "running (replaces the shadow maps near you)" :
		"running (combined with the shadow maps)", detail };
}

static RtStatusLine RtLampLine() {
	const char* s = "Shaders.ShadowsInteriors.RT";
	const std::string detail = RtLampShadows::Status;
	const bool on = RtOn(s, "Enabled");
	if (on != RtLampShadows::CompiledIn)
		return { on ? RtState::Blocked : RtState::Off, on ? "switched on: needs a restart (the lamp shaders are built at start)" :
			"switched off: not traced, the cube maps again (on again without a restart)", detail };	// (2026-11-11: off is live, RtLampShadows::EnabledNow)
	if (!on) return { RtState::Off, "off (Enabled; needs a restart)", detail };
	if (!RtOn(s, "Live")) return { RtState::Off, "Live is off: the cube maps again (A/B)", detail };
	if (!RtCore::Running()) return RtNotRunningLine(detail);
	if (TheShaderManager->GameState.isExterior && !RtOn(s, "Exteriors")) return { RtState::Off, "outdoors: traced indoors only (Exteriors off; needs a restart)", detail };
	// UNOFFICIAL 2026-11-13 (a tester on an RTX card: "RT lamp shadows = no worky"): with path tracing on these are not traced at all
	// (RtLampShadows::Wants, PTStandDownRT, which every path tracing preset sets), and this line still said "running".
	if (RtPathTrace::StandDownRT())
		return { RtState::Blocked, "not traced while path tracing is on: the path tracer has its own lamp shadows (PTStandDownRT). Switch path tracing off to see these", detail };
	if (RtPathTrace::Replacing())
		return { RtState::Blocked, "traced, but path tracing is on: they show only on what stays the game's own picture (PTStandDownRT is off)", detail };
	return { RtState::Running, "running", detail };
}

// Ambient light uses the RT occlusion and bounce light unless its own ComputeGI is actually active (needs DXVK fork).
static bool RtComputeGIInWay() {
	if (!TheSettingManager->GetMenuShaderEnabled("AmbientLight")) return false;
	AmbientLightEffect* al = TheShaderManager->Effects.AmbientLight;
	return al ? al->GIActive() : (DxvkCompute::Available() && RtOn("Shaders.AmbientLight.Main", "ComputeGI"));
}

static RtStatusLine RtAOLine() {
	const char* s = "Shaders.AmbientOcclusion.RT";
	const std::string detail = AmbientOcclusionEffect::RtStatusText();
	if (!RtOn(s, "Enabled")) return { RtState::Off, "off", detail };
	if (!RtCore::Running()) return RtNotRunningLine(detail);
	if (!RtAmbient::PassesWanted()) return { RtState::Blocked, "on, but the Vulkan passes are off ([Main.Main.Performance] NvrDxvkPasses)", detail };
	const bool exterior = TheShaderManager->GameState.isExterior;
	const bool aoRuns = TheSettingManager->GetMenuShaderEnabled("AmbientOcclusion") &&
		RtOn(exterior ? "Shaders.AmbientOcclusion.Exteriors" : "Shaders.AmbientOcclusion.Interiors", "Enabled");
	const bool alUses = TheSettingManager->GetMenuShaderEnabled("AmbientLight") && !RtComputeGIInWay();
	if (!aoRuns && !alUses)
		return { RtState::Blocked, std::string("on, but nothing uses it here: Ambient occlusion is off ") + (exterior ? "outdoors (Exteriors" : "indoors (Interiors") +
			" Enabled) and Ambient light " + (RtComputeGIInWay() ? "has ComputeGI on" : "is off"), detail };
	// UNOFFICIAL 2026-11-13: Ambient occlusion and Ambient light step aside while the path traced picture replaces the scene
	if (RtPathTrace::Replacing())
		return { RtState::Blocked, "not shown while path tracing is on: the path tracer shades corners itself. Switch path tracing off to see it", detail };
	if (RtComputeGIInWay()) return { RtState::Blocked, "running, but Ambient light ignores it while its ComputeGI is on (only Ambient occlusion uses it)", detail };
	// UNOFFICIAL 2026-11-13: Ambient light's D3D9 passes (NvrDxvkAmbientLight off, the default) take the traced occlusion only with OcclusionOnD3D9
	if (alUses && !TheSettingManager->SettingsMain.Main.NvrDxvkAmbientLight && !RtOn(s, "OcclusionOnD3D9"))
		return { RtState::Blocked, "traced, but Ambient light keeps its own screen occlusion (OcclusionOnD3D9 is off): only the bounce light uses the rays", detail };
	return { RtState::Running, "running", detail };
}

static RtStatusLine RtGILine() {
	const char* s = "Shaders.AmbientOcclusion.RT";
	const std::string detail = AmbientOcclusionEffect::RtStatusText();
	if (!RtOn(s, "GI")) return { RtState::Off, "off (GI)", detail };
	if (!RtCore::Running()) return RtNotRunningLine(detail);
	if (!RtAmbient::PassesWanted()) return { RtState::Blocked, "on, but the Vulkan passes are off ([Main.Main.Performance] NvrDxvkPasses)", detail };
	if (!TheSettingManager->GetMenuShaderEnabled("AmbientLight")) return { RtState::Blocked, "on, but Ambient light is off: the bounce light is added there", detail };
	if (RtComputeGIInWay()) return { RtState::Blocked, "on, but Ambient light's ComputeGI is on: it replaces the RT bounce light", detail };
	if (RtPathTrace::Replacing())	// UNOFFICIAL 2026-11-13
		return { RtState::Blocked, "not shown while path tracing is on: the path tracer has its own bounce light. Switch path tracing off to see it", detail };
	return { RtState::Running, RtOn(s, "RadianceCache") ? "running (more than one bounce: RadianceCache)" : "running (one bounce)", detail };
}

static RtStatusLine RtSkyLine() {
	const std::string detail = AmbientOcclusionEffect::RtStatusText();
	if (!RtOn("Shaders.AmbientOcclusion.Skylighting", "Enabled") || !RtOn("Shaders.AmbientOcclusion.RT", "Sky"))
		return { RtState::Off, "off", detail };
	if (!TheSettingManager->GetMenuShaderEnabled("AmbientLight") && !TheSettingManager->GetMenuShaderEnabled("AmbientOcclusion"))
		return { RtState::Blocked, "on, but both Ambient light and Ambient occlusion are off", detail };
	if (!TheShaderManager->GameState.isExterior)
		return { RtState::Off, "indoors: sky occlusion runs outdoors only", detail };
	if (!RtCore::Running()) return RtNotRunningLine(detail);
	if (!RtAmbient::PassesWanted()) return { RtState::Blocked, "on, but the Vulkan passes are off ([Main.Main.Performance] NvrDxvkPasses)", detail };
	if (RtPathTrace::Replacing())
		return { RtState::Blocked, "not shown while path tracing is on: the path tracer traces its own sky light", detail };
	return { RtState::Running, "running", detail };
}

static RtStatusLine RtReflectionsLine() {
	const char* s = "Shaders.Reflections.RT";
	ReflectionsEffect* r = TheShaderManager->Effects.Reflections;
	const std::string detail = r ? r->RtStatusText() : std::string("no Reflections effect");
	if (!RtOn(s, "Enabled")) return { RtState::Off, "off", detail };
	if (!TheSettingManager->GetMenuShaderEnabled("Reflections"))
		return { RtState::Blocked, "on, but Reflections is off: the rays only fill in what its screen-space reflections miss", detail };
	if (!RtCore::Running()) return RtNotRunningLine(detail);
	// UNOFFICIAL 2026-11-13: with path tracing on the Reflections effect is not drawn (PTReflectionsOff) and its rays are not traced
	// (PTStandDownRT); every path tracing preset sets both, and this line still said "running"
	if (RtPathTrace::Replacing() && RtOn("Main.Main.RayTracing", "PTReflectionsOff"))
		return { RtState::Blocked, "not drawn while path tracing is on: the path tracer has its own reflections (PTReflectionsOff). Switch path tracing off to see these", detail };
	if (RtPathTrace::StandDownRT())
		return { RtState::Blocked, "not traced while path tracing is on (PTStandDownRT): Reflections shows its screen-space answer only", detail };
	const float wet = TheSettingManager->GetSettingF("Shaders.Reflections.Main", "WetRoughness");
	const float maxRough = TheSettingManager->GetSettingF(s, "MaxRoughness");
	char t[200];
	if (wet >= maxRough) {
		sprintf_s(t, "running, but wet ground (WetRoughness %.2f) is rougher than MaxRoughness %.2f: no rays on wet ground", wet, maxRough);
		return { RtState::Blocked, t, detail };
	}
	if (r && r->NothingReflects())
		return { RtState::Blocked, TheSettingManager->GetSettingF(s, "Weapon") > 0.0f ? "running for the weapon only: nothing else on screen reflects here (dry, GroundSheen 0)" :
			"on, but nothing on screen reflects here (dry, GroundSheen and InteriorFloors 0): nothing to trace", detail };
	return { RtState::Running, "running", detail };
}

// UNOFFICIAL 2026-11-13 (a tester: "rt vol light = no worky"): the traced light shafts had no line here, and they are the RT effect
// with the most silent ways to do nothing (indoors, no other RT effect on, the sun's shadows faded, the shafts too faint to see).
static RtStatusLine RtShaftsLine() {
	std::string text;
	const int state = RtFogVisibility::MenuState(text);
	return { state == 1 ? RtState::Running : state == 2 ? RtState::Blocked : RtState::Off, text, RtFogVisibility::StatusText() + "\n" + RtFogLamps::StatusText() };
}

static RtStatusLine RtPathTraceLine() {
	const char* s = "Main.Main.RayTracing";
	const std::string detail = RtPathTrace::StatusText();
	const bool realtime = RtOn(s, "PTRealtime");
	if (!realtime && !RtPathTrace::ReferenceOn()) return { RtState::Off, "off", detail };	// (2026-11-11: ReferencePT only as honoured)
	if (!RtCore::Running()) return RtNotRunningLine(detail);
	return { RtState::Running, realtime ? "running: the path-traced game (experimental)" : "running: the reference picture (a comparison, not for play)", detail };
}

static void DrawRtStatusLine(const char* name, const RtStatusLine& line) {
	static const ImVec4 kRunning(0.45f, 0.85f, 0.45f, 1.0f), kBlocked(1.0f, 0.75f, 0.2f, 1.0f);
	ImGui::TextUnformatted(name);
	ImGui::SameLine(ImGui::GetFontSize() * 9.0f);
	ImGui::PushTextWrapPos(0.0f);
	if (line.State == RtState::Off) ImGui::TextDisabled("%s", line.Text.c_str());
	else ImGui::TextColored(line.State == RtState::Running ? kRunning : kBlocked, "%s", line.Text.c_str());
	ImGui::PopTextWrapPos();
	if (!line.Detail.empty() && ImGui::IsItemHovered()) WrappedTooltip(line.Detail.c_str());
}

// One switch for every RT effect, for an A/B: off remembers which were on, on brings them back (or, with nothing remembered, turns on
// the sun shadows, the ambient occlusion and bounce light, and the reflections). Lamp shadows go through Live (Enabled needs a restart).
struct RtMasterKey { const char* Section; const char* Key; bool Default; };
static const RtMasterKey kRtMasterKeys[] = {
	{ "Shaders.ShadowsExteriors.RT", "Enabled", true },
	{ "Shaders.ShadowsInteriors.RT", "Live", true },
	{ "Shaders.AmbientOcclusion.RT", "Enabled", true },
	{ "Shaders.AmbientOcclusion.RT", "Sky", false },
	{ "Shaders.AmbientOcclusion.RT", "GI", false },
	{ "Shaders.Reflections.RT", "Enabled", true },
	{ "Main.Main.RayTracing", "PTRealtime", false },
	{ "Main.Main.RayTracing", "ReferencePT", false },
};
static bool s_rtRemembered = false;
static bool s_rtMemory[_countof(kRtMasterKeys)] = {};

static bool RtAnyOn() {
	for (const RtMasterKey& k : kRtMasterKeys) {
		if (!RtOn(k.Section, k.Key)) continue;
		if (!strcmp(k.Key, "Live") && !RtOn(k.Section, "Enabled")) continue;	// Live means nothing without the lamp shadows on
		return true;
	}
	return false;
}

// 2026-11-25 (unsaved marks): one of the keys the switch writes is unsaved
static bool RtMasterMarked() {
	for (const RtMasterKey& k : kRtMasterKeys) if (UnsavedSettings::Key(k.Section, k.Key)) return true;
	return false;
}

static void DrawRtMasterSwitch() {
	bool on = RtAnyOn();
	MenuWidgets::RowBegin("All ray tracing", MenuWidgets::ResetSlotWidth(), RtMasterMarked());	// 2026-11-25 (unsaved marks)
	if (MenuWidgets::Toggle("##rtmaster", &on)) {
		if (!on) {
			for (size_t i = 0; i < _countof(kRtMasterKeys); i++) {
				s_rtMemory[i] = RtOn(kRtMasterKeys[i].Section, kRtMasterKeys[i].Key);
				TheSettingManager->SetSetting(kRtMasterKeys[i].Section, kRtMasterKeys[i].Key, false);
			}
			s_rtRemembered = true;
		}
		else
			for (size_t i = 0; i < _countof(kRtMasterKeys); i++)
				TheSettingManager->SetSetting(kRtMasterKeys[i].Section, kRtMasterKeys[i].Key, s_rtRemembered ? s_rtMemory[i] : kRtMasterKeys[i].Default);
		TheSettingManager->LoadSettings();
	}
	if (ImGui::IsItemHovered() || MenuWidgets::RowLabelHovered())
		WrappedTooltip("Every ray-traced effect off, or back on as it was, to compare. With nothing to bring back it turns on the sun shadows, "
			"the ambient occlusion and the reflections. Lamp shadows switch through their Live setting (turning them on the first time needs "
			"a restart). The path tracer is only turned off, never on.");
}

// UNOFFICIAL 2026-11-15 RT master switches (docs/next/RT-MASTER-SWITCHES.md; owner: one switch for interiors, one for exteriors, e.g. keep
// ray tracing indoors where it is cheap and drop it outdoors). [Main.Main.RayTracing] RTInteriors / RTExteriors, live (a missing key: on).
// Off: in that kind of space every ray-traced effect stands down and its normal effect is used (RtCore::Gated); the effects' own settings
// stay as they are. The core logs one line per change of a switch and a GAME line each time the gate opens or shuts (RtCore::UpdateGate).
static bool RtSpaceSwitchOn(const char* key) {
	SettingManager::Configuration::ConfigNode node;
	return !TheSettingManager->Config.FillNode(&node, "Main.Main.RayTracing", key) || node.BoolValue;
}

static void DrawRtSpaceSwitch(const char* label, const char* key, const char* tip) {
	bool on = RtSpaceSwitchOn(key);
	ImGui::PushID(key);
	MenuWidgets::RowBegin(label, MenuWidgets::ResetSlotWidth(), UnsavedSettings::Key("Main.Main.RayTracing", key));	// 2026-11-25 (unsaved marks)
	if (MenuWidgets::Toggle("##rtspace", &on)) TheSettingManager->SetSetting("Main.Main.RayTracing", key, on);
	if (ImGui::IsItemHovered() || MenuWidgets::RowLabelHovered()) WrappedTooltip(tip);
	ImGui::PopID();
}

static void DrawRtSpaceSwitches() {
	DrawRtSpaceSwitch("Ray tracing indoors", "RTInteriors", "Off: inside buildings, vaults and caves no effect uses ray tracing; the normal "
		"shadows, ambient occlusion, reflections and light are used there instead. Your ray tracing settings are kept and come back outdoors "
		"(and here when you switch this on again). Applies at once.");
	DrawRtSpaceSwitch("Ray tracing outdoors", "RTExteriors", "Off: outdoors no effect uses ray tracing; the normal shadows, ambient occlusion, "
		"reflections and light shafts are used there instead. Outdoors is where ray tracing costs the most. Your ray tracing settings are kept "
		"and come back indoors (and here when you switch this on again). Applies at once.");
	ImGui::TextDisabled("Turning one off brings back the normal effects in that kind of place.");
}

// One picker for the RT debug views: picking one sets that DebugView and clears the other three, so only one shows.
struct RtDebugView { const char* Label; const char* Section; int Value; };
static const char* const kRtDebugSections[] = { "Shaders.ShadowsExteriors.RT", "Shaders.ShadowsInteriors.RT", "Shaders.AmbientOcclusion.RT", "Shaders.Reflections.RT",
	"Shaders.VolumetricLight.RT" };	// (2026-11-13: + the light shafts' A/B)
static const RtDebugView kRtDebugViews[] = {
	{ "Off (the normal picture)", nullptr, 0 },
	{ "Sun shadows: the traced shadow alone", "Shaders.ShadowsExteriors.RT", 1 },
	{ "Sun shadows: traced visibility (white lit, black blocked)", "Shaders.ShadowsExteriors.RT", 4 },
	{ "Lamp shadows: traced vs cube maps (red / green where they differ)", "Shaders.ShadowsInteriors.RT", 1 },
	{ "Lamp shadows: where the traced result is used", "Shaders.ShadowsInteriors.RT", 2 },
	{ "Ambient occlusion: the traced visibility", "Shaders.AmbientOcclusion.RT", 1 },
	{ "Bounce light alone (GI)", "Shaders.AmbientOcclusion.RT", 6 },
	{ "Reflections: where each reflection comes from", "Shaders.Reflections.RT", 1 },
	{ "Reflections: ray hit distance", "Shaders.Reflections.RT", 2 },
	{ "Reflections: ray tracing alone (screen trace off)", "Shaders.Reflections.RT", 3 },
	{ "Light shafts: the shadow maps again (traced, not used: an A/B)", "Shaders.VolumetricLight.RT", 1 },	// UNOFFICIAL 2026-11-13
};

static int RtCurrentDebugView() {	// the picker's entry, -1 when the DebugViews set match none (set by hand)
	int set = 0;
	for (const char* section : kRtDebugSections) if (TheSettingManager->GetSettingI(section, "DebugView") != 0) set++;
	if (set == 0) return 0;
	if (set > 1) return -1;
	for (int i = 1; i < (int)_countof(kRtDebugViews); i++)
		if (TheSettingManager->GetSettingI(kRtDebugViews[i].Section, "DebugView") == kRtDebugViews[i].Value) return i;
	return -1;
}

static void DrawRtDebugPicker() {
	const int current = RtCurrentDebugView();
	bool unsaved = false;	// 2026-11-25 (unsaved marks): one of the DebugView keys the picker writes
	for (const char* section : kRtDebugSections) unsaved = unsaved || UnsavedSettings::Key(section, "DebugView");
	MenuWidgets::RowBegin("Debug view", MenuWidgets::ResetSlotWidth(), unsaved);
	ImGui::SetNextItemWidth(-FLT_MIN);
	if (ImGui::BeginCombo("##rtdebug", current >= 0 ? kRtDebugViews[current].Label : "Other (set in the sections below)")) {
		for (int i = 0; i < (int)_countof(kRtDebugViews); i++) {
			if (!ImGui::Selectable(kRtDebugViews[i].Label, i == current)) continue;
			for (const char* section : kRtDebugSections) TheSettingManager->SetSetting(section, "DebugView", 0);
			if (kRtDebugViews[i].Section) TheSettingManager->SetSetting(kRtDebugViews[i].Section, "DebugView", kRtDebugViews[i].Value);
			TheSettingManager->LoadSettings();
		}
		ImGui::EndCombo();
	}
	if (ImGui::IsItemHovered() || MenuWidgets::RowLabelHovered())
		WrappedTooltip("Shows one ray-traced effect's own picture, to see what it does. It stays on until you pick Off (the normal picture).");
}

static void DrawRtSharedBlocksOnRtPage(const char* rtSection);	// 2026-11-09: below
static bool RtSharedBlocksMarked(const char* rtSection);	// 2026-11-25 (unsaved marks): below, with them

static void RenderRayTracingPage() {
	PageTitle("Ray tracing", "Every ray-traced effect in one place. Needs the DXVK fork, NvrRtHelper.exe and a GPU with ray tracing.");
	DrawRtSpaceSwitches();	// UNOFFICIAL 2026-11-15 the master switches for indoors and outdoors, first
	ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.5f));

	if (MenuWidgets::SubHeader("Status###rtstatus", true)) {
		DrawRtStatusLine("Core", RtCoreLine());
		DrawRtStatusLine("Sun shadows", RtSunLine());
		DrawRtStatusLine("Lamp shadows", RtLampLine());
		DrawRtStatusLine("Occlusion", RtAOLine());
		DrawRtStatusLine("Sky occlusion", RtSkyLine());
		DrawRtStatusLine("Bounce light", RtGILine());
		DrawRtStatusLine("Reflections", RtReflectionsLine());
		DrawRtStatusLine("Light shafts", RtShaftsLine());	// UNOFFICIAL 2026-11-13
		DrawRtStatusLine("Path tracing", RtPathTraceLine());
		ImGui::TextDisabled("Hover a line for the full state.");
		ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.5f));
	}
	DrawPtOverview();	// UNOFFICIAL 2026-11-12: "What still applies with path tracing" (only while path tracing is on)

	DrawRtMasterSwitch();
	if (DevOptionsShown()) DrawRtDebugPicker();
	ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y));

	// The five sections' own rows, each under its header, drawn as their pages draw them (the groups read the page shown)
	const std::string page = gMenu.page;
	for (const RtSection& section : kRtSections) {
		const std::string header = std::string(section.Title) + "###rtsection" + section.Section;
		const bool unsaved = SectionRowsMarked(section.Section) || RtSharedBlocksMarked(section.Section);	// 2026-11-25 (unsaved marks)
		if (!MenuWidgets::SubHeader(header.c_str(), false, unsaved)) continue;
		SettingManager::Configuration::SettingList rows;
		TheSettingManager->FillMenuSettings(&rows, section.Section);
		ImGui::PushID(section.Section);
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextDisabled("%s", section.Note);
		ImGui::PopTextWrapPos();
		DrawPtNote(section.Section);	// UNOFFICIAL 2026-11-12 (nothing while path tracing is off)
		gMenu.page = section.Section;
		PageOrder(rows);
		RenderSettingsList(rows);
		gMenu.page = page;
		DrawRtSharedBlocksOnRtPage(section.Section);	// 2026-11-09: the raster settings this effect shares, shown again
		ImGui::PopID();
	}
}

// 2026-11-05 (user: "cameramode, flycam can live under main -> misc"): small Main sections drawn inside another page under their own
// headers (as the Ray tracing page draws its sections), instead of as entries of their own in the sidebar. Only the menu moves.
struct PageAttach { const char* Host; const char* Section; const char* Title; };
static const PageAttach kPageAttaches[] = {
	{ "Main.Main.Misc", "Main.CameraMode.Main", "Camera mode" },
	{ "Main.Main.Misc", "Main.CameraMode.Positioning", "Camera mode position" },
	{ "Main.Main.Misc", "Main.FlyCam.Main", "Fly camera" },
	{ "Main.Main.Misc", "Main.Transitions.Main", "Day and night blending" },
};

static void RenderAttachedSections(const std::string& host) {
	const std::string page = gMenu.page;
	for (const PageAttach& a : kPageAttaches) {
		if (host != a.Host) continue;
		const std::string header = std::string(a.Title) + "###attached" + a.Section;
		if (!MenuWidgets::SubHeader(header.c_str(), false, UnsavedSettings::Section(a.Section))) continue;	// 2026-11-25 (unsaved marks)
		SettingManager::Configuration::SettingList rows;
		TheSettingManager->FillMenuSettings(&rows, a.Section);
		ImGui::PushID(a.Section);
		gMenu.page = a.Section;
		PageOrder(rows);
		RenderSettingsList(rows);
		gMenu.page = page;
		ImGui::PopID();
	}
}

static bool HiddenFromSidebar(const std::string& path, const std::string& name);

// ---- Simple view: the whole effect on one page -------------------------------------------------------------------------------
// 2026-11-05 (user, of a simple page with four rows: "can we make it so we utilize all this dead space with simple mode on better so it
// doesn't look so barren?"). While Advanced is off, a page of an effect with a simple view shows every page of that effect at once, each
// as a card (its plain name, its simple rows, how many more Advanced has), two cards a row when the page is wide enough: one screen per
// effect, as a game's graphics menu has, instead of a few rows per page. The page picked in the sidebar comes first. Pages with no simple
// view and the ray tracing pages (on their own page) are left out of the cards.
static std::string SimplePageTitle(const std::string& name) {
	static const char* const kNames[][2] = { { "Main", "General" }, { "Exteriors", "Outdoors" }, { "Exterior", "Outdoors" },
		{ "Interiors", "Indoors" }, { "Night", "At night" }, { "Rain", "In the rain" }, { "NightRain", "At night in the rain" },
		{ "Default", "General" }, { "FirstPersonView", "First person" }, { "ThirdPersonView", "Third person" }, { "VanityView", "Idle camera" } };
	for (const auto& n : kNames) if (name == n[0]) return n[1];
	return name;
}

static void RenderSimpleCard(const std::string& section) {
	SettingManager::Configuration::SettingList rows;
	TheSettingManager->FillMenuSettings(&rows, section.c_str());
	ImGui::PushID(section.c_str());
	ImGui::PushStyleColor(ImGuiCol_Text, MenuSkin::Heading(0.25f));
	MenuWidgets::Title(SimplePageTitle(section.substr(section.rfind('.') + 1)).c_str(), 1.1f, UnsavedSettings::Section(section.c_str()));	// 2026-11-25 (unsaved marks)
	ImGui::PopStyleColor();
	ImGui::Separator();
	const std::string page = gMenu.page;
	gMenu.page = section;
	PageOrder(rows);
	RenderSettingsList(rows);	// (Advanced off: RenderSimpleList)
	gMenu.page = page;
	ImGui::PopID();
}

// Whether this page is drawn as the whole effect's cards; draws them.
static bool RenderSimpleEffectPage() {
	if (AdvancedShown() || !HasSimpleView(gMenu.page) || IsRtSection(gMenu.page)) return false;
	const std::string effect = ShaderOfPage(gMenu.page);
	if (effect.empty()) return false;
	std::vector<std::string> cards = { gMenu.page };
	StringList children;
	TheSettingManager->FillMenuSections(&children, ("Shaders." + effect).c_str());
	for (const std::string& child : children) {
		const std::string section = "Shaders." + effect + "." + child;
		if (section != gMenu.page && HasSimpleView(section) && !IsRtSection(section) && !HiddenFromSidebar(section, child)) cards.push_back(section);
	}
	RenderPageHeader(gMenu.page);
	RenderSectionHelp(gMenu.page);
	const int columns = cards.size() > 1 && ImGui::GetContentRegionAvail().x > ImGui::GetFontSize() * 55.0f ? 2 : 1;
	if (!ImGui::BeginTable("##simplecards", columns, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_PadOuterX)) return true;
	for (const std::string& section : cards) {
		ImGui::TableNextColumn();
		RenderSimpleCard(section);
		ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 2.0f));
	}
	ImGui::EndTable();
	return true;
}

// ---- Folders of settings shown again on the Ray tracing page -----------------------------------------------------------------
// (2026-11-10: these were the folders of the "pages by what you see", an experiment of 2026-11-08 that put a second map of the same
// settings above the effects list. The pages are gone: the effects list is the one map, and the player arranges its folders, see "The
// effects list's folders" below. What stays is the part the Ray tracing page uses: the raster settings a ray-traced effect shares.)

// Keys: nullptr = every key of the section; "-a, b" = every key but these; "@group:<Header>" = that kMenuGroups group's keys;
// otherwise the keys listed. Section "@effect": Keys is an effect's name, drawn as its on/off switch.
struct VisualItem { const char* Section; const char* Keys; const char* Title; };
struct VisualFolder { const char* Title; const char* Note; std::vector<VisualItem> Items; };

static const VisualFolder kSunMapsFolder = { "Shadow maps", "Distance, sharpness and what casts. Used by both: ray tracing combines with the maps or "
	"replaces them only near you.", {
	{ "Shaders.ShadowsExteriors.Main", nullptr, "General" },
	{ "Shaders.ShadowsExteriors.ShadowMaps", nullptr, "Shadow maps" },
	{ "Shaders.ShadowsExteriors.Forms", nullptr, "What casts sun shadows" },
	{ "Shaders.ShadowsExteriors.SunSmoothing", nullptr, "Sun movement" },
	{ "Shaders.ShadowsExteriors.Ortho", nullptr, "Top-down map" } } };
static const VisualFolder kSunSoftFolder = { "Soft edges (contact hardening)", "Sharp where an object meets the ground and softer the further the "
	"shadow falls. It softens the shadow maps: with ray tracing on Replace it shows only past the traced range. Switching it on needs a restart.",
	{
	{ "@effect", "ContactHardening", "Soft edges" },
	{ "Shaders.ContactHardening.Main", nullptr, nullptr } } };
static const VisualFolder kSunContactFolder = { "Contact shadows (screen space)", "Small sun shadows where things touch, on top of either.",
	{ { "Shaders.ShadowsExteriors.ScreenSpace", nullptr, nullptr },
	{ "Shaders.ShadowsExteriors.SSShadows", nullptr, nullptr } } };
static const VisualFolder kLampCommonFolder = { "Lamps and what casts shadows", "How many lamps cast shadows, how far, and which objects. Used by both.",
	{ { "Shaders.ShadowsInteriors.Main", nullptr, nullptr } } };
static const VisualFolder kLampCapsuleFolder = { "People's lamp shadows (capsules)", "Soft lamp shadows of people from simple shapes round their bones, "
	"on top of either.", { { "Shaders.ShadowsInteriors.Capsules", nullptr, nullptr } } };
static const VisualFolder kAmbientLightFolder = { "Ambient light", "Reach, strength and the outline fixes of the all-in-one pass.",
	{ { "@effect", "AmbientLight", "Ambient light" },
	{ "Shaders.AmbientLight.Main", "-GridBounce, GridCellSize, GridSteps, GridSpread, GridScreenStrength, GridBlocking, GridWallMemory, GridLamps, GridFine, GridColour, "
		"AlbedoBuffer, AlbedoFallbackCap, ComputeGI, ComputeGIStrength, ComputeGIThickness, ComputeGIThinOcclusion, ComputeGIQuality, ComputeGITest", nullptr } } };
static const VisualFolder kGridFolder = { "Light grid bounce", "Sun and lamp light spread through a 3D grid round you (GridBounce above 0 turns it on). "
	"Adds to any extra bounce light.", { { "Shaders.AmbientLight.Main", "@group:Light grid bounce", nullptr } } };
static const VisualFolder kSsrFolder = { "Screen-space reflections", "Reach, steadiness, what shows where rays find nothing, and per-material reflections. "
	"Used by both.", { { "Shaders.Reflections.Main", nullptr, nullptr } } };

static bool VisualKeyWanted(const char* keys, const char* section, const char* key) {
	if (!keys) return true;
	if (keys[0] == '-') return !KeyInList(keys + 1, key);
	if (!strncmp(keys, "@group:", 7)) {
		for (const MenuGroup& g : kMenuGroups)
			if (!strcmp(g.Section, section) && !strcmp(g.Header, keys + 7)) return KeyInList(g.Keys, key);
		return false;
	}
	return KeyInList(keys, key);
}

// 2026-11-25 (unsaved marks): whether [section] key is one of the item's rows (its switch, for an "@effect" item), and whether
// one of them is unsaved; a folder is marked by its items.
static bool VisualItemHas(const VisualItem& item, const std::string& section, const std::string& key) {
	if (!strcmp(item.Section, "@effect")) return key == "Enabled" && section == EffectSwitchSection(item.Keys);
	return section == item.Section && VisualKeyWanted(item.Keys, item.Section, key.c_str());
}

static bool VisualItemMarked(const VisualItem& item) {
	for (const auto& section : UnsavedSettings::All())
		for (const auto& key : section.second)
			if (VisualItemHas(item, section.first, key.first)) return true;
	return false;
}

static bool VisualItemsMarked(const std::vector<VisualItem>& items) {
	for (const VisualItem& item : items) if (VisualItemMarked(item)) return true;
	return false;
}

// An effect's on/off switch as a row (the sidebar's switch: the same "Heads up" when it clashes with another).
static void DrawVisualEffectSwitch(const char* shader, const char* label) {
	const SettingManager::MenuSwitch kind = TheSettingManager->GetMenuShaderSwitch(shader);
	if (kind == SettingManager::MenuSwitch::None) return;
	bool on = TheSettingManager->GetMenuShaderEnabled(shader);
	const bool forced = TheSettingManager->IsShaderForced(shader);
	const std::string name = std::string(label ? label : shader) + " (effect on/off)";
	ImGui::PushID("@effect");
	ImGui::PushID(shader);
	MenuWidgets::RowBegin(name.c_str(), MenuWidgets::ResetSlotWidth(), EffectSwitchMarked(shader));	// 2026-11-25 (unsaved marks)
	ImGui::BeginDisabled(forced);
	if (MenuWidgets::Toggle("##fx", &on) && (!on || ConfirmEffectOn(shader))) {
		TheShaderManager->SwitchShaderStatus(shader);
		TheSettingManager->LoadSettings();
	}
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) || MenuWidgets::RowLabelHovered())
		WrappedTooltip(forced ? "Locked by a preset." : "The effect's own switch: the same one as its entry in the effects list.");
	if (kind == SettingManager::MenuSwitch::MainEnabled) RestartBadge(("Shaders." + std::string(shader) + ".Main").c_str(), "Enabled");
	else RestartBadge(("Shaders." + std::string(shader) + ".Status").c_str(), "Enabled");
	ImGui::PopID();
	ImGui::PopID();
}

// One section's rows (all, or the item's keys), drawn as the section's own page draws them (its groups, simple view, colour wheels).
static void DrawVisualItem(const VisualItem& item, bool titled) {
	if (!strcmp(item.Section, "@effect")) return DrawVisualEffectSwitch(item.Keys, item.Title);
	SettingManager::Configuration::SettingList rows;
	TheSettingManager->FillMenuSettings(&rows, item.Section);
	if (item.Keys) {
		SettingManager::Configuration::SettingList kept;
		for (const auto& r : rows) if (VisualKeyWanted(item.Keys, item.Section, r.Key)) kept.push_back(r);
		rows.swap(kept);
	}
	if (rows.empty()) return;
	ImGui::PushID(item.Section);
	ImGui::PushID(item.Keys ? item.Keys : "");
	if (titled && item.Title) {
		ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.25f));
		ImGui::PushStyleColor(ImGuiCol_Text, MenuSkin::Heading(0.4f));
		ImGui::TextUnformatted(item.Title);
		MenuWidgets::MarkLastItem(item.Title, VisualItemMarked(item));	// 2026-11-25 (unsaved marks)
		ImGui::PopStyleColor();
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("[%s]", item.Section);
	}
	const std::string page = gMenu.page;
	gMenu.page = item.Section;
	PageOrder(rows);
	s_keySubset = item.Keys != nullptr;
	RenderSettingsList(rows);
	s_keySubset = false;
	gMenu.page = page;
	ImGui::PopID();
	ImGui::PopID();
}

// ---- The raster settings an RT effect shares, shown again under it ----------------------------------------------------------
// 2026-11-09 (user: "make the settings for RT effects that live under raster menus also have duplicate copies under each RT effect so
// users do not have to swap between menus to configure them"): a ray-traced effect runs together with its raster twin and reads some of
// the twin's settings (the sun's shadow maps and smoothing, the lamps' reach and capsules, the screen-space reflections, Ambient light and
// the light grid with the surface colour it multiplies the bounce by). Each folder above shows under the RT effect's section on the Ray
// tracing page, folded, as the very same rows (the same keys: a change here is a change on the effect's own page, and the toml is the same).
struct RtSharedBlock {
	const char* Rt;				// the RT effect's section
	const VisualFolder* Source;	// the raster folder whose rows are shown again
};
static const VisualFolder kShaftsSharedFolder = { "Light shafts", "The shafts' own strength and look; the traced shadow only changes what blocks the sun.",
	{ { "Shaders.VolumetricLight.Main", nullptr, "General" }, { "Shaders.VolumetricLight.Night", nullptr, "At night" } } };
static const RtSharedBlock kRtSharedBlocks[] = {
	{ "Shaders.ShadowsExteriors.RT", &kSunMapsFolder },
	{ "Shaders.ShadowsExteriors.RT", &kSunSoftFolder },
	{ "Shaders.ShadowsExteriors.RT", &kSunContactFolder },
	{ "Shaders.ShadowsInteriors.RT", &kLampCommonFolder },
	{ "Shaders.ShadowsInteriors.RT", &kLampCapsuleFolder },
	{ "Shaders.AmbientOcclusion.RT", &kAmbientLightFolder },
	{ "Shaders.AmbientOcclusion.RT", &kGridFolder },
	{ "Shaders.Reflections.RT", &kSsrFolder },
	{ "Shaders.VolumetricLight.RT", &kShaftsSharedFolder },
};

static bool VisualFolderHasVisibleItems(const VisualFolder& f) {
	for (const VisualItem& item : f.Items) {
		if (!strcmp(item.Section, "@effect")) return true;
		if (ShelvedEffects::Section(item.Section)) continue;
		SettingManager::Configuration::SettingList rows;
		TheSettingManager->FillMenuSettings(&rows, item.Section);
		for (const auto& r : rows)
			if (VisualKeyWanted(item.Keys, item.Section, r.Key) && !ShouldHideKey(r.Key) && !InternalHidden(r.Section, r.Key))
				return true;
	}
	return false;
}

static void DrawRtSharedBlock(const RtSharedBlock& b) {
	const VisualFolder& f = *b.Source;
	if (!VisualFolderHasVisibleItems(f)) return;
	const std::string header = std::string("Also used here: ") + f.Title + "###rtshared/page/" + b.Rt + "/" + f.Title;
	if (!MenuWidgets::SubHeader(header.c_str(), false, VisualItemsMarked(f.Items))) return;	// 2026-11-25 (unsaved marks)
	ImGui::PushID(f.Title);
	ImGui::Indent(ImGui::GetFontSize() * 0.75f);
	ImGui::PushTextWrapPos(0.0f);
	ImGui::TextDisabled("The same rows as on the effect's own page: a change here is a change there.");
	if (f.Note) ImGui::TextDisabled("%s", f.Note);
	ImGui::PopTextWrapPos();
	int sections = 0;
	for (const VisualItem& item : f.Items) if (strcmp(item.Section, "@effect")) sections++;
	for (const VisualItem& item : f.Items) DrawVisualItem(item, sections > 1);
	ImGui::Unindent(ImGui::GetFontSize() * 0.75f);
	ImGui::PopID();
}

// Under an RT effect's section on the Ray tracing page.
static void DrawRtSharedBlocksOnRtPage(const char* rtSection) {
	for (const RtSharedBlock& b : kRtSharedBlocks)
		if (!strcmp(b.Rt, rtSection)) DrawRtSharedBlock(b);
}

// 2026-11-25 (unsaved marks): one of the rows shown again under that section is unsaved
static bool RtSharedBlocksMarked(const char* rtSection) {
	for (const RtSharedBlock& b : kRtSharedBlocks)
		if (!strcmp(b.Rt, rtSection) && VisualItemsMarked(b.Source->Items)) return true;
	return false;
}

// ---- The Gun effects page (2026-11-16, gunfx-builtin) ---------------------------------------------------------------------------
// User: "we could just have the most important gunfx cfg keys exist inside the menu under different sorted drop downs". GunFX is built
// in and its old GunFX.ini keys live in [Main.GunFX.<section>]. The page (the effects list's "Gun effects" entry, [Main.GunFX.Main]) is
// one drop-down per effect: its switch and the few keys a player would tune (kSimpleRows); with Advanced on, every key of its section.
// Where the smoke starts and the log switch only with Advanced on. The same rows as anywhere else (DrawVisualItem): same keys, same file.
struct GunFolder { const char* Title; const char* Id; std::vector<VisualItem> Items; bool AdvancedOnly; };
static const GunFolder kGunFolders[] = {
	{ "Smoke puff at the muzzle", "puff", { { "Main.GunFX.Main", "Puff", nullptr }, { "Main.GunFX.Puff", nullptr, nullptr } }, false },
	{ "Smoke from a hot barrel", "heat", { { "Main.GunFX.Main", "HeatSmoke", nullptr }, { "Main.GunFX.Wisp", nullptr, nullptr } }, false },
	{ "Smoke trail after firing", "trail", { { "Main.GunFX.Main", "AfterFireTrail", nullptr }, { "Main.GunFX.Trail", nullptr, nullptr } }, false },
	{ "Smoke from the ejection port", "eject", { { "Main.GunFX.Main", "EjectionSmoke", nullptr }, { "Main.GunFX.Ejection", nullptr, nullptr } }, false },
	{ "Glowing hot barrel", "glow", { { "Main.GunFX.Main", "BarrelGlow", nullptr }, { "Main.GunFX.Glow", nullptr, nullptr } }, false },
	{ "Heat shimmer over a hot barrel", "haze", { { "Main.GunFX.Main", "HeatHaze", nullptr }, { "Main.GunFX.Haze", nullptr, nullptr } }, false },
	{ "Heat ripple on every shot", "blast", { { "Main.GunFX.Main", "MuzzleBlast", nullptr }, { "Main.GunFX.Blast", nullptr, nullptr } }, false },
	{ "Real 3D smoke (experimental)", "volume", { { "Main.GunFX.Main", "VolumetricSmoke, VolumetricSmokeHalfRes, VolumetricSmokeLowRes, BulletTunnels", nullptr },
		{ "Main.GunFX.Volume", "-Haze, HazeLife, HazeSize", nullptr } }, false },
	{ "Smoke haze fills the room", "smokehaze", { { "Main.GunFX.Main", "SmokeHaze", nullptr }, { "Main.GunFX.Volume", "Haze, HazeLife, HazeSize", nullptr } }, false },
	{ "Where the smoke starts on the gun", "muzzle", { { "Main.GunFX.Muzzle", nullptr, "Barrel tip (puff, heat smoke, trail)" } }, true },
	{ "Log and general", "general", { { "Main.GunFX.General", nullptr, nullptr } }, true },
};

static bool RenderGunPage() {
	if (gMenu.page != "Main.GunFX.Main") return false;
	RenderPageHeader(gMenu.page);
	RenderSectionHelp(gMenu.page);
	DrawVisualItem({ "Main.GunFX.Main", "EnergyWeapons", nullptr }, false);
	ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y));
	for (const GunFolder& f : kGunFolders) {
		if (f.AdvancedOnly && !AdvancedShown()) continue;
		const std::string header = std::string(f.Title) + "###gunfx/" + f.Id;
		if (!MenuWidgets::SubHeader(header.c_str(), false, VisualItemsMarked(f.Items))) continue;	// 2026-11-25 (unsaved marks)
		ImGui::PushID(f.Id);
		ImGui::Indent(ImGui::GetFontSize() * 0.75f);
		for (const VisualItem& item : f.Items) DrawVisualItem(item, item.Title != nullptr);
		ImGui::Unindent(ImGui::GetFontSize() * 0.75f);
		ImGui::PopID();
	}
	if (!AdvancedShown()) {
		ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y));
		ImGui::TextDisabled("Advanced (top row) shows every GunFX setting under its own name, where the smoke starts and the log switch.");
	}
	ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y));
	ImGui::TextDisabled("Per-gun settings: Data/NVSE/Plugins/GunFX/Weapons/<gun>.ini (made when you first equip a gun).");
	return true;
}

// ---- What is hiding effects right now ---------------------------------------------------------------------------------------
// 2026-11-05 (user, after an hour of "the rays show in the debug view but I see no reflection": the master switch, a debug view left
// on, specular occlusion and physically faint floors each hid it in turn): "add a note for each setting that could override and make
// other effects virtually invisible if they're on ... with the dead space at the bottom". Every page ends with the notes that apply
// NOW, each with a button that undoes it. kHidingNotes holds the hand-written ones; the Heads up rules (kConflictRules) whose setting is
// at its value and whose other side is on are listed too. tools/check_menu_notes.py checks that every debug view in the defaults is
// in kDebugViews.
struct DebugViewKey { const char* Section; const char* Key; const char* Name; };
static const DebugViewKey kDebugViews[] = {
	{ "Main.Main.RayTracing", "PTDebugView", "Path tracing" },
	{ "Main.Main.ReducedQuality", "SunLayersDebug", "Sun layers" },
	{ "Shaders.AmbientOcclusion.Skylighting", "DebugView", "Skylighting" },
	{ "Shaders.AmbientOcclusion.RT", "DebugView", "Ray-traced ambient occlusion" },
	{ "Shaders.AmbientLight.Main", "DebugView", "Ambient light" },
	{ "Shaders.Flashlight.Deferred", "DebugView", "Flashlight" },
	{ "Shaders.Grass.GPUGrass", "DebugView", "GPU grass" },
	{ "Shaders.Grass.Interaction", "Debug", "Grass interaction" },
	{ "Shaders.DitherBuster.Main", "DebugView", "Dither buster" },
	{ "Shaders.TAA.Main", "DebugView", "TAA" },
	{ "Shaders.TAA.FSR", "ReactiveDebug", "FSR reactive mask" },
	{ "Shaders.TAA.FSR", "DebugView", "FSR" },
	{ "Shaders.PBR.Metal", "DebugView", "Metal" },
	{ "Shaders.PBR.MaterialRules", "DebugView", "Material rules" },
	{ "Shaders.PBR.Rmaos", "DebugView", "Material maps" },
	{ "Shaders.POM.Displacement", "DebugView", "Object displacement" },
	{ "Shaders.ShadowsExteriors.Main", "FitToDepthDebug", "Sun shadow fit" },
	{ "Shaders.ShadowsExteriors.RT", "DebugView", "Ray-traced sun shadows" },
	{ "Shaders.ShadowsExteriors.ShadowMaps", "CharacterShadowMapDebug", "Character shadow map" },
	{ "Shaders.ShadowsExteriors.TerrainShadows", "DebugView", "Terrain shadows" },
	{ "Shaders.CloudShadows.Main", "DebugView", "Cloud shadows" },
	{ "Shaders.ShadowsInteriors.Forward", "DebugView", "Lamp shadows (forward)" },
	{ "Shaders.ShadowsInteriors.Deferred", "DebugView", "Lamp shadows (deferred)" },
	{ "Shaders.ShadowsInteriors.RT", "DebugView", "Ray-traced lamp shadows" },
	{ "Shaders.ShadowsInteriors.RT", "ExteriorDebug", "Ray-traced lamp shadows outdoors" },
	{ "Shaders.ShadowsInteriors.RT", "DebugLamp", "Ray-traced lamp shadows (one lamp)" },
	{ "Shaders.DustMotes.Main", "DebugView", "Dust motes" },
	{ "Shaders.LocalExposure.Main", "DebugView", "Local exposure" },
	{ "Shaders.Reflections.Main", "DebugView", "Reflections" },
	{ "Shaders.Reflections.RT", "DebugView", "Ray-traced reflections" },
	{ "Shaders.VolumetricClouds.Main", "ShadowDebug", "Cloud shadow" },
	{ "Shaders.VolumetricClouds.Main", "ReflectionDebug", "Cloud reflections" },
	{ "Shaders.HandBlood.Main", "DebugView", "Hand blood" },
	{ "Shaders.Skin.Debug", "DebugView", "Skin" },
	{ "Shaders.SnowAccumulation.Main", "CapturedNormalsDebugView", "Snow normals" },
	{ "Shaders.Terrain.Blending", "DebugView", "Terrain blending" },
	{ "Shaders.Terrain.Blending", "Debug", "Terrain blending (debug)" },
	{ "Shaders.Terrain.Displacement", "DebugView", "Terrain displacement" },
	{ "Shaders.WetWorld.Main", "PersistentDebugView", "Wet world" },
	{ "Shaders.WetnessEffects.HudRaindrops", "DebugView", "Raindrops on screen" },
	{ "Shaders.WetnessEffects.Reflections", "DebugView", "Wet reflections" },
	{ "Main.Main.Quality", "MsaaEdgeDebugView", "MSAA edge fix" },
	{ "Main.Main.Quality", "GBufferView", "G-buffer" },
	{ "Main.Main.Quality", "FirstPersonNativeDebug", "First-person native scale" },
	{ "Main.Main.Quality", "GearWeatherDebug", "Gear weather" },
	{ "Shaders.WorldSurface.Main", "DebugView", "World surface detail" },
	{ "Shaders.ShadowsExteriors.Deferred", "DebugView", "Deferred sun" },
	{ "Shaders.Skin.SSS", "DebugView", "Skin SSS" },
	{ "Shaders.Puddles.Debug", "DebugView", "Puddles" },
	{ "Shaders.WetnessEffects.Debug", "ShowCover", "Wetness cover" },
	{ "Shaders.Debug.Main", "ClipView", "Clip view" },
	{ "Shaders.HDR.Main", "ShowHudMask", "HDR HUD mask" },
	{ "Shaders.ShadowsExteriors.RT", "DenoiserDebugView", "RT sun denoiser" },
	{ "Shaders.ShadowsInteriors.RT", "DenoiserDebugView", "RT lamp denoiser" },
	{ "Shaders.Reflections.RT", "DenoiserDebugView", "RT reflections denoiser" },
	{ "Shaders.VolumetricLight.RT", "DebugView", "RT light shafts" },
};

static float SettingF(const char* section, const char* key) { return TheSettingManager->GetSettingF(section, key); }
static bool ReflectionsShown() { return TheSettingManager->SettingsMain.Main.RenderEffects && EffectOn("Reflections"); }
static bool RtReflectionsOn() { return ReflectionsShown() && SettingOn("Shaders.Reflections.RT", "Enabled"); }
// The user asked for clear reflections: a strong or sharp setting.
static bool WantsClearReflections() {
	return SettingF("Shaders.Reflections.Main", "Roughness") <= 0.15f || SettingF("Shaders.Reflections.Main", "InteriorFloors") >= 1.0f ||
		SettingF("Shaders.Reflections.Main", "GroundSheen") >= 1.0f || SettingF("Shaders.Reflections.Main", "Mirror") > 0.0f;
}

struct HidingNote {
	bool (*Active)();
	const char* Text;
	const char* Fix;		// the button
	const char* Sets;		// what the button writes: "Section/Key=Value Section/Key=Value"; "!effect:Name" switches that effect on
};
static const HidingNote kHidingNotes[] = {
	{ []() { return !TheSettingManager->SettingsMain.Main.RenderEffects; },
		"ALL effects are switched off (the master switch: the modifier key + the menu key, Alt+O by default). Changing settings shows "
		"nothing until they are on again.", "Turn effects on", "Main.Main.Misc/RenderEffects=true" },
	{ []() { return ReflectionsShown() && SettingF("Shaders.Reflections.Main", "SpecularOcclusion") > 0.0f &&
			SettingF("Shaders.Reflections.Main", "Mirror") < 1.0f && WantsClearReflections(); },
		"Specular occlusion dims reflections on floors darker than what they reflect: on dark polished floors people and objects "
		"vanish from the reflection and only lamps remain.", "Turn specular occlusion off", "Shaders.Reflections.Main/SpecularOcclusion=0.0" },
	{ []() { return ReflectionsShown() && SettingF("Shaders.Reflections.Main", "Mirror") <= 0.0f && WantsClearReflections(); },
		"Floors reflect like real stone and tile: about 4 % looking down (more toward the horizon), so mostly bright lamps show in "
		"them. For a clear mirror that shows people at any angle, raise Mirror floors.", "Make floors mirrors",
		"Shaders.Reflections.Main/Mirror=1.0 Shaders.Reflections.Main/Roughness=0.02" },
	{ []() { return ReflectionsShown() && !TheShaderManager->GameState.isExterior && SettingF("Shaders.Reflections.Main", "InteriorFloors") <= 0.0f &&
			SettingF("Shaders.Reflections.Main", "MaterialReflect") <= 0.0f; },
		"Indoors nothing reflects: Floor reflections indoors and Smooth surface reflections are both 0.", "Floors reflect indoors",
		"Shaders.Reflections.Main/InteriorFloors=1.0" },
	{ []() { return TheSettingManager->SettingsMain.Main.RenderEffects && SettingOn("Shaders.Reflections.RT", "Enabled") && !EffectOn("Reflections"); },
		"Ray-traced reflections are on but the Reflections effect is off: they do nothing until it is on.", "Turn Reflections on", "!effect:Reflections" },
	{ []() { return TheSettingManager->SettingsMain.Main.RenderEffects && AnyRtOn() && !SettingOn("Main.Main.RayTracing", "Actors"); },
		"People are left out of the ray-traced scene: no people in ray-traced reflections, and no ray-traced shadows from them.",
		"Put people in", "Main.Main.RayTracing/Actors=true" },
	{ []() { return RtReflectionsOn() && !SettingOn("Main.Main.RayTracing", "HitTextures"); },
		"Ray-traced reflections show each object as one flat colour (real textures are off).", "Use real textures",
		"Main.Main.RayTracing/HitTextures=true" },
	{ []() { return RtReflectionsOn() && !SettingOn("Main.Main.RayTracing", "AlphaTested"); },
		"Fences, leaves, grates and other see-through meshes are missing from ray-traced reflections and shadows.", "Put them in",
		"Main.Main.RayTracing/AlphaTested=true" },
	// (2026-11-05 rtrefl-lake: without it the lake mirrored only the sky; the cliffs across it stand past SceneRadius)
	{ []() { return RtReflectionsOn() && TheShaderManager->GameState.isExterior && !SettingOn("Main.Main.RayTracing", "FarObjectLOD"); },
		"Outdoors, ray-traced reflections only reach what is near you (SceneRadius): distant cliffs, hills and buildings are missing, so "
		"a lake shows only the sky where it should mirror the far shore.", "Put the distance in",
		"Main.Main.RayTracing/FarObjectLOD=true Main.Main.RayTracing/FarLandLOD=true" },
	{ []() { return TheSettingManager->SettingsMain.Main.RenderEffects && RtCore::LeftOut("RT reference path tracer") &&
			(RtPathTrace::ReferenceOn() || SettingOn("Main.Main.RayTracing", "PTRealtime")); },
		"Path tracing is on but it was left out at start: the other ray-traced effects took all the shared images it needs. Turn one "
		"of them off (or PTRestirGI) and restart the game.", nullptr, nullptr },
	{ []() { return TheSettingManager->SettingsMain.Main.RenderEffects && AnyRtOn() && !RtCore::Running() && !RtCore::Gated(); },	// (2026-11-15: not while a master switch is off here)
		"A ray-traced effect is on but ray tracing is not running (see the Ray tracing page's first line for why): it shows nothing.",
		nullptr, nullptr },
};

static void ApplySets(const char* sets) {
	if (!sets) return;
	const std::string all(sets);
	for (const bool on : { true, false }) {	// "!effect:Name" switches an effect on, "!effectoff:Name" off (its own switch, as the sidebar)
		const std::string tag = on ? "!effect:" : "!effectoff:";
		if (all.rfind(tag, 0) != 0) continue;
		const std::string name = all.substr(tag.size());
		if (TheSettingManager->GetMenuShaderEnabled(name.c_str()) != on) TheShaderManager->SwitchShaderStatus(name.c_str());
		return;
	}
	std::stringstream parts(all);
	std::string kv;
	while (parts >> kv) {
		const size_t slash = kv.find('/'), eq = kv.find('=');
		if (slash == std::string::npos || eq == std::string::npos || eq < slash) continue;
		TheSettingManager->SetSettingS(kv.substr(0, slash).c_str(), kv.substr(slash + 1, eq - slash - 1).c_str(), kv.substr(eq + 1).c_str());
	}
	TheSettingManager->LoadSettings();
}

static void RenderHidingNotes() {
	struct Line { std::string Text, Fix, Sets; };
	std::vector<Line> lines;
	for (const HidingNote& n : kHidingNotes)
		if (n.Active()) lines.push_back({ n.Text, n.Fix ? n.Fix : "", n.Sets ? n.Sets : "" });
	std::string views;
	std::string reset;
	for (const DebugViewKey& d : kDebugViews)
		if (TheSettingManager->GetSettingF(d.Section, d.Key) != 0.0f) {
			views += (views.empty() ? "" : ", ") + std::string(d.Name);
			reset += std::string(d.Section) + "/" + d.Key + "=0 ";
		}
	if (!views.empty())
		lines.push_back({ "A debug view is on (" + views + "): it replaces or tints the normal picture.", "Turn debug views off", reset });
	for (const ConflictRule& r : kConflictRules) {
		if (!r.Applies || !r.Applies()) continue;	// (the always-on ones are warnings for the moment of switching, not hiders)
		// When AmbientLight and AmbientOcclusion/BounceLight are both on, the AmbientOcclusion/BounceLight rules already offer to turn the unused one off; don't emit a second contradictory button for AmbientLight
		if (!strcmp(r.Section, "Shaders.AmbientLight.Status") && !strcmp(r.Key, "Enabled")) continue;
		const bool boolean = !strcmp(r.Value, "true");
		const bool at = boolean ? SettingOn(r.Section, r.Key) : TheSettingManager->GetSettingI(r.Section, r.Key) == atoi(r.Value);
		if (!at) continue;
		const std::string section(r.Section);
		const bool status = boolean && !strcmp(r.Key, "Enabled") && section.size() > 15 && section.rfind("Shaders.", 0) == 0 &&
			section.compare(section.size() - 7, 7, ".Status") == 0;
		const std::string fxName = status ? section.substr(8, section.size() - 15) : std::string();
		const std::string btn = status ? ("Turn " + fxName + " off") : boolean ? "Turn it off" : "";
		lines.push_back({ r.Text, btn,
			status ? "!effectoff:" + fxName : boolean ? section + "/" + r.Key + "=false" : std::string() });
	}
	if (lines.empty()) return;

	// In the page's empty space: pushed to the bottom when there is room, else after the rows.
	const float lineH = ImGui::GetTextLineHeightWithSpacing();
	float need = ImGui::GetFrameHeightWithSpacing() * 2.0f;
	for (const Line& l : lines) need += lineH * (1.0f + (float)l.Text.size() / 110.0f) + (l.Fix.empty() ? 0.0f : ImGui::GetFrameHeightWithSpacing());
	const float room = ImGui::GetContentRegionAvail().y;
	if (room > need) ImGui::Dummy(ImVec2(0.0f, room - need));
	ImGui::Separator();
	ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", lines.size() == 1 ? "1 thing may be hiding effects right now" :
		(std::to_string(lines.size()) + " things may be hiding effects right now").c_str());
	for (size_t i = 0; i < lines.size(); i++) {
		ImGui::PushID((int)i + 7100);
		ImGui::Bullet();
		ImGui::SameLine();
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextUnformatted(lines[i].Text.c_str());
		ImGui::PopTextWrapPos();
		if (!lines[i].Fix.empty() && ImGui::SmallButton(lines[i].Fix.c_str())) ApplySets(lines[i].Sets.c_str());
		ImGui::PopID();
	}
}

// ---- Unified Ambient Occlusion & Bounce Light page -----------------------------------------------------------------------
// One clean place to pick your AO, Sky Occlusion, and Bounce Light (GI) modes without colliding effects. Writes canonical
// TOML keys underneath so existing presets and user configs remain 100% compatible.
static const char* const kAoTabId = "@IndirectLighting";
static const char* const kQuickTabId = "@QuickSettings";

static void SetShaderEnabled(const char* shader, bool wantOn) {
	if (TheSettingManager->GetMenuShaderEnabled(shader) != wantOn)
		TheShaderManager->SwitchShaderStatus(shader);
}

static int DetectAoMode() {
	const bool al = EffectOn("AmbientLight");
	const bool ao = EffectOn("AmbientOcclusion");
	if (!al && !ao) return 0;
	const bool rtOn = RtOn("Shaders.AmbientOcclusion.RT", "Enabled");
	const bool rtAoActive = rtOn && (!al || RtOn("Shaders.AmbientOcclusion.RT", "OcclusionOnD3D9") ||
		TheSettingManager->SettingsMain.Main.NvrDxvkAmbientLight) && !(al && RtComputeGIInWay());
	if (rtAoActive) return 4;
	if (al) return 1;
	if (RtOn("Shaders.AmbientOcclusion.GTAO", "Enabled")) return 2;
	return 3;
}

static int DetectSkyOcclusionMode() {
	if (!RtOn("Shaders.AmbientOcclusion.Skylighting", "Enabled")) return 0;
	if (RtOn("Shaders.AmbientOcclusion.RT", "Sky")) return 2;
	return 1;
}

static int DetectBounceLightMode() {
	const bool al = EffectOn("AmbientLight");
	if (al && RtOn("Shaders.AmbientLight.Main", "ComputeGI")) return 1;
	if (al && RtOn("Shaders.AmbientOcclusion.RT", "GI")) return 2;
	if (al && (SettingF("Shaders.AmbientLight.Main", "GridBounce") > 0.0f || SettingF("Shaders.AmbientLight.Main", "GridLamps") > 0.0f)) return 3;
	if (!al && EffectOn("BounceLight")) return 4;
	return 0;
}

static const char* const kAoModeNames[] = {
	"Off",
	"Ambient Light (all-in-one, recommended)",
	"Standalone GTAO (screen-space)",
	"Standalone Legacy AO (HBAO)",
	"Ray-traced AO (RTX + DXVK)"
};
static const char* const kSkyModeNames[] = {
	"Off",
	"Top-down sky map (fast)",
	"Ray-traced sky occlusion (RTX + DXVK)"
};
static const char* const kBounceModeNames[] = {
	"Off",
	"Screen-space GI (ComputeGI, needs DXVK fork)",
	"Ray-traced GI (RTX + DXVK)",
	"3D Light Grid (sun + lamp grid)",
	"Standalone BounceLight (legacy)"
};

// 2026-11-26 (unsaved marks): the three pickers write into each other's settings (AO's Ambient Light hosts the bounce light): one group
static int IndirectModes() { return DetectAoMode() | DetectSkyOcclusionMode() << 4 | DetectBounceLightMode() << 8; }

static void DrawIndirectLightingPickers() {
	const int aoMode = DetectAoMode();
	const int skyMode = DetectSkyOcclusionMode();
	const int giMode = DetectBounceLightMode();

	// 1. Ambient Occlusion mode
	ImGui::PushID("ao_mode");
	const float w1 = MenuWidgets::RowBegin("Ambient occlusion", MenuWidgets::ResetSlotWidth(), ModeMarked("ao", DetectAoMode));	// 2026-11-25 (unsaved marks)
	ImGui::SetNextItemWidth(w1);
	const int pickedAo = NamePicker("##aomode", kAoModeNames[aoMode], kAoModeNames, 5, aoMode);
	if (ImGui::IsItemHovered() || MenuWidgets::RowLabelHovered())
		WrappedTooltip("Controls corner and crevice shading.\n\n"
			"Ambient Light: fast all-in-one GTAO pass that also hosts ComputeGI, Light Grid, and RT GI.\n"
			"Standalone GTAO / Legacy AO: separate AmbientOcclusion effect (disables AmbientLight).\n"
			"Ray-traced AO: traced occlusion with zero screen-edge halos (requires RTX + DXVK fork).");
	ImGui::PopID();
	if (pickedAo >= 0 && pickedAo != aoMode) {
		PickerWrites writes("il", IndirectModes);	// 2026-11-26 (unsaved marks)
		if (pickedAo == 0) {
			SetShaderEnabled("AmbientLight", false);
			SetShaderEnabled("AmbientOcclusion", false);
			TheSettingManager->SetSetting("Shaders.AmbientOcclusion.RT", "Enabled", false);
		}
		else if (pickedAo == 1) {
			SetShaderEnabled("AmbientLight", true);
			SetShaderEnabled("AmbientOcclusion", false);
			TheSettingManager->SetSetting("Shaders.AmbientOcclusion.RT", "Enabled", false);
			TheSettingManager->SetSetting("Shaders.AmbientOcclusion.RT", "OcclusionOnD3D9", true);
		}
		else if (pickedAo == 2 || pickedAo == 3) {
			SetShaderEnabled("AmbientLight", false);
			SetShaderEnabled("AmbientOcclusion", true);
			TheSettingManager->SetSetting("Shaders.AmbientOcclusion.Exteriors", "Enabled", true);
			TheSettingManager->SetSetting("Shaders.AmbientOcclusion.Interiors", "Enabled", true);
			TheSettingManager->SetSetting("Shaders.AmbientOcclusion.GTAO", "Enabled", pickedAo == 2);
			TheSettingManager->SetSetting("Shaders.AmbientOcclusion.RT", "Enabled", false);
		}
		else if (pickedAo == 4) {
			SetShaderEnabled("AmbientLight", true);
			SetShaderEnabled("AmbientOcclusion", false);
			TheSettingManager->SetSetting("Shaders.AmbientLight.Main", "ComputeGI", false);
			TheSettingManager->SetSetting("Shaders.AmbientOcclusion.RT", "Enabled", true);
			TheSettingManager->SetSetting("Shaders.AmbientOcclusion.RT", "OcclusionOnD3D9", true);
		}
		TheSettingManager->LoadSettings();
	}

	// 2. Sky Occlusion mode
	ImGui::PushID("sky_mode");
	const float w2 = MenuWidgets::RowBegin("Sky occlusion", MenuWidgets::ResetSlotWidth(), ModeMarked("sky", DetectSkyOcclusionMode));	// 2026-11-25 (unsaved marks)
	ImGui::SetNextItemWidth(w2);
	const int pickedSky = NamePicker("##skymode", kSkyModeNames[skyMode], kSkyModeNames, 3, skyMode);
	if (ImGui::IsItemHovered() || MenuWidgets::RowLabelHovered())
		WrappedTooltip("Outdoors, darkens ambient sky light under porches, roofs, bridges and overhangs.\n"
			"Top-down sky map is fast and works on every GPU. Ray-traced sky occlusion uses rays for exact multi-story cover.");
	ImGui::PopID();
	if (pickedSky >= 0 && pickedSky != skyMode) {
		PickerWrites writes("il", IndirectModes);	// 2026-11-26 (unsaved marks)
		if (pickedSky == 0) {
			TheSettingManager->SetSetting("Shaders.AmbientOcclusion.Skylighting", "Enabled", false);
			TheSettingManager->SetSetting("Shaders.AmbientOcclusion.RT", "Sky", false);
		}
		else if (pickedSky == 1) {
			TheSettingManager->SetSetting("Shaders.AmbientOcclusion.Skylighting", "Enabled", true);
			TheSettingManager->SetSetting("Shaders.AmbientOcclusion.RT", "Sky", false);
			if (!EffectOn("AmbientLight") && !EffectOn("AmbientOcclusion")) SetShaderEnabled("AmbientLight", true);
		}
		else if (pickedSky == 2) {
			TheSettingManager->SetSetting("Shaders.AmbientOcclusion.Skylighting", "Enabled", true);
			TheSettingManager->SetSetting("Shaders.AmbientOcclusion.RT", "Sky", true);
			if (!EffectOn("AmbientLight") && !EffectOn("AmbientOcclusion")) SetShaderEnabled("AmbientLight", true);
		}
		TheSettingManager->LoadSettings();
	}

	// 3. Bounce Light (GI) mode
	ImGui::PushID("gi_mode");
	const float w3 = MenuWidgets::RowBegin("Bounce light (GI)", MenuWidgets::ResetSlotWidth(), ModeMarked("gi", DetectBounceLightMode));	// 2026-11-25 (unsaved marks)
	ImGui::SetNextItemWidth(w3);
	const int pickedGi = NamePicker("##gimode", kBounceModeNames[giMode], kBounceModeNames, 5, giMode);
	if (ImGui::IsItemHovered() || MenuWidgets::RowLabelHovered())
		WrappedTooltip("Indirect light bouncing off lit surfaces onto nearby ones.\n\n"
			"Screen-space GI (ComputeGI): fast screen-space bounce light on the DXVK fork.\n"
			"Ray-traced GI: off-screen accurate bounce light and glowing signs (RTX + DXVK fork).\n"
			"3D Light Grid: soft sun and lamp bounce spread through a 3D grid around you.\n"
			"Standalone BounceLight: legacy separate effect (disables AmbientLight).");
	ImGui::PopID();
	if (pickedGi >= 0 && pickedGi != giMode) {
		PickerWrites writes("il", IndirectModes);	// 2026-11-26 (unsaved marks)
		if (pickedGi == 0) {
			TheSettingManager->SetSetting("Shaders.AmbientLight.Main", "ComputeGI", false);
			TheSettingManager->SetSetting("Shaders.AmbientOcclusion.RT", "GI", false);
			TheSettingManager->SetSetting("Shaders.AmbientLight.Main", "GridBounce", 0.0f);
			TheSettingManager->SetSetting("Shaders.AmbientLight.Main", "GridLamps", 0.0f);
			SetShaderEnabled("BounceLight", false);
		}
		else if (pickedGi == 1) {
			SetShaderEnabled("AmbientLight", true);
			SetShaderEnabled("BounceLight", false);
			TheSettingManager->SetSetting("Shaders.AmbientLight.Main", "ComputeGI", true);
			TheSettingManager->SetSetting("Shaders.AmbientOcclusion.RT", "GI", false);
		}
		else if (pickedGi == 2) {
			SetShaderEnabled("AmbientLight", true);
			SetShaderEnabled("BounceLight", false);
			TheSettingManager->SetSetting("Shaders.AmbientLight.Main", "ComputeGI", false);
			TheSettingManager->SetSetting("Shaders.AmbientOcclusion.RT", "GI", true);
		}
		else if (pickedGi == 3) {
			SetShaderEnabled("AmbientLight", true);
			SetShaderEnabled("BounceLight", false);
			TheSettingManager->SetSetting("Shaders.AmbientLight.Main", "ComputeGI", false);
			TheSettingManager->SetSetting("Shaders.AmbientOcclusion.RT", "GI", false);
			if (SettingF("Shaders.AmbientLight.Main", "GridBounce") <= 0.0f)
				TheSettingManager->SetSetting("Shaders.AmbientLight.Main", "GridBounce", 0.7f);
			if (SettingF("Shaders.AmbientLight.Main", "GridLamps") <= 0.0f)
				TheSettingManager->SetSetting("Shaders.AmbientLight.Main", "GridLamps", 0.5f);
		}
		else if (pickedGi == 4) {
			SetShaderEnabled("AmbientLight", false);
			TheSettingManager->SetSetting("Shaders.AmbientLight.Main", "ComputeGI", false);
			TheSettingManager->SetSetting("Shaders.AmbientOcclusion.RT", "GI", false);
			SetShaderEnabled("BounceLight", true);
		}
		TheSettingManager->LoadSettings();
	}
}

static void RenderIndirectLightingPage() {
	PageTitle("Ambient occlusion & bounce light",
		"Choose one mode for corner shading (AO), sky occlusion under roofs, and bounce light (GI). All modes write standard settings underneath.");

	if (RtPathTrace::Replacing()) {
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
			"Path tracing is active: the path tracer computes its own occlusion and bounce light. Turn path tracing off to see these modes.");
		ImGui::PopTextWrapPos();
		ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.5f));
	}

	DrawIndirectLightingPickers();

	const int aoMode = DetectAoMode();
	const int skyMode = DetectSkyOcclusionMode();
	const int giMode = DetectBounceLightMode();

	// Live status hints when requirements are missing
	if (giMode == 1 && !DxvkCompute::Available()) {
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextColored(kRestartOrange, "Screen-space GI (ComputeGI) needs the DXVK fork's d3d9.dll (not loaded in this session).");
		ImGui::PopTextWrapPos();
	}
	if ((aoMode == 4 || skyMode == 2 || giMode == 2) && !RtCore::Running()) {
		DrawRtStatusLine("Ray tracing", RtCoreLine());
	}
	if (giMode == 2) DrawRtStatusLine("RT Bounce", RtGILine());
	if (skyMode == 2) DrawRtStatusLine("RT Sky", RtSkyLine());
	if (aoMode == 4) DrawRtStatusLine("RT AO", RtAOLine());

	ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.5f));
	ImGui::Separator();

	// Active tuning blocks in 2 columns when wide enough (zero dead space)
	const int cols = ImGui::GetContentRegionAvail().x > ImGui::GetFontSize() * 55.0f ? 2 : 1;
	if (ImGui::BeginTable("##il_tuning", cols, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_PadOuterX)) {
		ImGui::TableNextColumn();
		if (aoMode == 1 && MenuWidgets::SubHeader("Ambient occlusion tuning (Ambient Light)###il_ao_al", true)) {
			DrawVisualItem({ "Shaders.AmbientLight.Main", "OcclusionStrength, Radius, DrawDistance, DarkestOcclusion, BrightnessFade", nullptr }, false);
		}
		else if ((aoMode == 2 || aoMode == 3) && MenuWidgets::SubHeader("Ambient occlusion tuning (Standalone)###il_ao_sa", true)) {
			DrawVisualItem({ "Shaders.AmbientOcclusion.Exteriors", "Enabled, StrengthMultiplier, Range, ClampStrength, LumThreshold", "Outdoors" }, true);
			DrawVisualItem({ "Shaders.AmbientOcclusion.Interiors", "Enabled, StrengthMultiplier, Range, ClampStrength, LumThreshold", "Indoors" }, true);
			if (aoMode == 2) DrawVisualItem({ "Shaders.AmbientOcclusion.GTAO", "Enabled, MaxRadius", "GTAO" }, true);
		}
		else if (aoMode == 4 && MenuWidgets::SubHeader("Ambient occlusion tuning (Ray-traced)###il_ao_rt", true)) {
			DrawVisualItem({ "Shaders.AmbientOcclusion.RT", "RayCount, Distance, OcclusionOnD3D9", nullptr }, false);
			if (EffectOn("AmbientLight"))
				DrawVisualItem({ "Shaders.AmbientLight.Main", "OcclusionStrength, Radius, DarkestOcclusion, BrightnessFade", "Strength & darkness" }, true);
			else if (EffectOn("AmbientOcclusion")) {
				DrawVisualItem({ "Shaders.AmbientOcclusion.Exteriors", "StrengthMultiplier, ClampStrength, LumThreshold", "Outdoors" }, true);
				DrawVisualItem({ "Shaders.AmbientOcclusion.Interiors", "StrengthMultiplier, ClampStrength, LumThreshold", "Indoors" }, true);
			}
		}

		if (skyMode > 0 && MenuWidgets::SubHeader("Sky occlusion tuning###il_sky", true)) {
			DrawVisualItem({ "Shaders.AmbientOcclusion.Skylighting", "Strength, Distance, Reflections", nullptr }, false);
			if (skyMode == 2) DrawVisualItem({ "Shaders.AmbientOcclusion.RT", "SkyRays, SkyDistance", "Ray-traced sky" }, true);
		}

		ImGui::TableNextColumn();
		if (giMode == 1 && MenuWidgets::SubHeader("Bounce light tuning (ComputeGI)###il_gi_ssgi", true)) {
			DrawVisualItem({ "Shaders.AmbientLight.Main", "ComputeGIStrength, ComputeGIQuality, ComputeGIThickness, ComputeGIThinOcclusion", nullptr }, false);
		}
		else if (giMode == 2 && MenuWidgets::SubHeader("Bounce light tuning (Ray-traced GI)###il_gi_rt", true)) {
			DrawVisualItem({ "Shaders.AmbientLight.Main", "SunGI, RoomGI, AmbientReplace, AmbientFloor, AmbientReplaceInterior, AmbientFloorInterior", "Ambient replacement" }, false);
			DrawVisualItem({ "Shaders.AmbientOcclusion.RT", "GIStrength, GIDistance, GISky, SkyMissSH, GIAmbient, RadianceCache, CacheExteriors, GIStrengthNight, GIStrengthInterior, GlowGI, GlowDirect", "Ray-traced bounce" }, true);
		}
		else if (giMode == 3 && MenuWidgets::SubHeader("Bounce light tuning (3D Light Grid)###il_gi_grid", true)) {
			DrawVisualItem({ "Shaders.AmbientLight.Main", "GridBounce, GridLamps, GridSpread, GridColour", nullptr }, false);
		}
		else if (giMode == 4 && MenuWidgets::SubHeader("Bounce light tuning (Standalone)###il_gi_sa", true)) {
			DrawVisualItem({ "Shaders.BounceLight.Main", "Strength, Radius, BlurRadius, QuarterResolution, Temporal", nullptr }, false);
		}
		else if (giMode == 0) {
			ImGui::TextDisabled("Bounce light is currently off. Pick a Bounce light (GI) mode above to tune it.");
		}
		ImGui::EndTable();
	}

	ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.5f));
	ImGui::TextDisabled("Tip: Turn on Advanced (top right) to access every fine-tuning parameter in AmbientLight / AmbientOcclusion / BounceLight.");
}

// ---- Dedicated Deferred Renderer & Unified Debug Views pages --------------------------------------------------------------
static bool IsDeferredQualityKey(const char* key) {
	if (!strncmp(key, "GearWeather", 11)) return true;
	static const char* const kDeferredKeys[] = {
		"DeferredRendererLive", "DeferredStandalone",
		"DeferredTransparents", "DeferredFoliageLamps",
		"DeferredSkin", "DeferredSkinLive", "DeferredSkinBlur", "DeferredSkinSSS",
		"DeferredHair", "DeferredHairLive", "DeferredFirstPerson", "DeferredFirstPersonLive",
		"DeferredReflections", "DeferredBumpedNormals",
		"DeferredALAlbedo", "DeferredALAlbedoGain", "DeferredRRGuides",
		"DeferredRtSunFirst", "DeferredTiledLight", "DeferredADFog",
		"DeferredDepthProbe", "GBufferView"
	};
	for (const char* k : kDeferredKeys) if (!strcmp(key, k)) return true;
	return false;
}

static void DrawSettingKey(const char* section, const char* key, bool skipInternal = true, const char* labelOverride = nullptr) {
	if (skipInternal && InternalHidden(section, key)) return;
	SettingManager::Configuration::ConfigNode node;
	if (!TheSettingManager->Config.FillNode(&node, section, key)) return;
	ImGui::PushID(section);
	RenderSetting(node, strncmp(section, "Shaders.", 8) == 0, labelOverride);
	ImGui::PopID();
}

static void RenderDeferredRendererPage() {
	PageTitle("Deferred renderer", "Unified screen-space sun and lamp lighting, G-buffer materials, skin/hair/glass deferred paths, and developer controls.");

	const ImVec4 green(0.45f, 0.85f, 0.45f, 1.0f);

	ImGui::TextUnformatted("Active state:");
	ImGui::SameLine();
	ImGui::TextColored(green, "Deferred (Full: outdoors + indoors)");

	if (ImGui::Button("Apply recommended settings##def_recommended")) {
		TheSettingManager->SetSetting("Main.Main.Quality", "DeferredSkin", true);
		TheSettingManager->SetSetting("Main.Main.Quality", "DeferredSkinSSS", 2);
		TheSettingManager->SetSetting("Main.Main.Quality", "DeferredHair", true);
		TheSettingManager->SetSetting("Main.Main.Quality", "DeferredTransparents", 1);
		TheSettingManager->SetSetting("Main.Main.Quality", "DeferredBumpedNormals", true);
		TheSettingManager->LoadSettings();
	}
	if (ImGui::IsItemHovered())
		WrappedTooltip("Enables the recommended deferred sub-features (Skin, Burley SSS, Hair, Transparents, Bumped Normals). Press Save and restart if startup settings changed.");

	ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.5f));
	if (MenuWidgets::SubHeader("Deferred features###def_features", true)) {
		static const char* const kFeatures[] = {
			"DeferredSkin", "DeferredSkinSSS", "DeferredHair", "DeferredTransparents", "DeferredBumpedNormals"
		};
		for (const char* k : kFeatures) DrawSettingKey("Main.Main.Quality", k);
	}

	if (!DevOptionsShown()) {
		ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.5f));
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextDisabled("First-person deferred and experimental switches are hidden (View > Developer options).");
		ImGui::PopTextWrapPos();
		return;
	}

	if (MenuWidgets::SubHeader("First-person & GearWeather (Dev)###def_fp", false)) {
		static const char* const kFpKeys[] = {
			"DeferredFirstPerson", "GearWeather", "GearWeatherStrength", "GearWeatherWet",
			"GearWeatherFrost", "GearWeatherSand", "GearWeatherMud", "GearWeatherSpeed",
			"GearWeatherSkin", "GearWeatherScale", "GearWeatherForce"
		};
		for (const char* k : kFpKeys) DrawSettingKey("Main.Main.Quality", k);
	}

	if (MenuWidgets::SubHeader("Experimental & tuning (Dev)###def_exp", false)) {
		static const char* const kExpKeys[] = {
			"DeferredFoliageLamps", "DeferredReflections", "DeferredALAlbedo", "DeferredALAlbedoGain",
			"DeferredRRGuides", "DeferredRtSunFirst", "DeferredTiledLight", "DeferredADFog",
			"DeferredSkinBlur", "GBufferView"
		};
		for (const char* k : kExpKeys) DrawSettingKey("Main.Main.Quality", k);

		ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.25f));
		ImGui::PushStyleColor(ImGuiCol_Text, MenuSkin::Heading(0.4f));
		ImGui::TextUnformatted("Deferred sun pass (Shaders.ShadowsExteriors.Deferred)");
		ImGui::PopStyleColor();
		static const char* const kSunKeys[] = {
			"MatchSunDisc", "CanopyShadow", "ParallaxShadow", "LandCrackShadow",
			"MicroShadow", "AuthoredHighlight", "HighlightFade", "Shadows"
		};
		for (const char* k : kSunKeys) DrawSettingKey("Shaders.ShadowsExteriors.Deferred", k);
	}
}

static bool ContainsNoCase(const char* haystack, const char* needle) {
	if (!needle || !*needle) return true;
	if (!haystack) return false;
	for (const char* h = haystack; *h; h++) {
		const char* a = h, *b = needle;
		while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
		if (!*b) return true;
	}
	return false;
}

static void RenderDebugSettingsPage() {
	PageTitle("Debug views & diagnostics", "All shader and renderer debug views in one place (including internal debug views).");

	enum class DbgKind { IntZero, BoolFalse, FloatZero, FloatNegOne, FloatOne, PixelCoord, EffectDebug };
	struct DbgItem { const char* Section; const char* Key; const char* Label; DbgKind Kind; };
	struct DbgGroup { const char* Title; const DbgItem* Items; size_t Count; };

	static const DbgItem kDbgDeferred[] = {
		{ "Main.Main.Quality", "GBufferView", "G-buffer (GBufferView)", DbgKind::IntZero },
		{ "Main.Main.Quality", "MsaaEdgeDebugView", "MSAA edge fix (MsaaEdgeDebugView)", DbgKind::IntZero },
		{ "Main.Main.Quality", "FirstPersonNativeDebug", "1st-person native (FirstPersonNativeDebug)", DbgKind::IntZero },
		{ "Main.Main.Quality", "GearWeatherDebug", "Gear weather (GearWeatherDebug)", DbgKind::IntZero },
		{ "Shaders.WorldSurface.Main", "DebugView", "World surface detail (DebugView)", DbgKind::IntZero },
		{ "Main.Main.ReducedQuality", "SunLayersDebug", "Sun layers (SunLayersDebug)", DbgKind::IntZero },
	};
	static const DbgItem kDbgLighting[] = {
		{ "Shaders.AmbientLight.Main", "DebugView", "Ambient light (DebugView)", DbgKind::IntZero },
		{ "Shaders.AmbientLight.Main", "DebugGIExposure", "Ambient light GI exposure (DebugGIExposure)", DbgKind::FloatOne },
		{ "Shaders.AmbientOcclusion.Skylighting", "DebugView", "Skylighting (DebugView)", DbgKind::IntZero },
		{ "Shaders.AmbientOcclusion.RT", "DebugView", "RT AO / GI (DebugView)", DbgKind::IntZero },
		{ "Shaders.Flashlight.Deferred", "DebugView", "Flashlight (DebugView)", DbgKind::IntZero },
		{ "Shaders.LocalExposure.Main", "DebugView", "Local exposure (DebugView)", DbgKind::IntZero },
		{ "Shaders.DustMotes.Main", "DebugView", "Dust motes (DebugView)", DbgKind::IntZero },
	};
	static const DbgItem kDbgShadows[] = {
		{ "Shaders.ShadowsExteriors.Deferred", "DebugView", "Deferred sun (DebugView)", DbgKind::IntZero },
		{ "Shaders.ShadowsExteriors.Main", "FitToDepthDebug", "Sun fit-to-depth (FitToDepthDebug)", DbgKind::IntZero },
		{ "Shaders.ShadowsExteriors.ShadowMaps", "CharacterShadowMapDebug", "Character shadow map (CharacterShadowMapDebug)", DbgKind::IntZero },
		{ "Shaders.ShadowsExteriors.TerrainShadows", "DebugView", "Terrain shadows (DebugView)", DbgKind::IntZero },
		{ "Shaders.ShadowsExteriors.RT", "DebugView", "RT sun shadows (DebugView)", DbgKind::IntZero },
		{ "Shaders.ShadowsExteriors.RT", "DenoiserDebugView", "RT sun denoiser (DenoiserDebugView)", DbgKind::IntZero },
		{ "Shaders.CloudShadows.Main", "DebugView", "Cloud shadows (DebugView)", DbgKind::IntZero },
		{ "Shaders.ShadowsInteriors.Forward", "DebugView", "Lamp shadows forward (DebugView)", DbgKind::IntZero },
		{ "Shaders.ShadowsInteriors.Deferred", "DebugView", "Lamp shadows deferred (DebugView)", DbgKind::IntZero },
		{ "Shaders.ShadowsInteriors.RT", "DebugView", "RT lamp shadows (DebugView)", DbgKind::IntZero },
		{ "Shaders.ShadowsInteriors.RT", "DenoiserDebugView", "RT lamp denoiser (DenoiserDebugView)", DbgKind::IntZero },
		{ "Shaders.ShadowsInteriors.RT", "ExteriorDebug", "RT lamp exteriors (ExteriorDebug)", DbgKind::IntZero },
		{ "Shaders.ShadowsInteriors.RT", "DebugLamp", "RT debug lamp slot (DebugLamp)", DbgKind::IntZero },
		{ "Shaders.VolumetricClouds.Main", "ShadowDebug", "Volumetric clouds shadow (ShadowDebug)", DbgKind::BoolFalse },
		{ "Shaders.VolumetricClouds.Main", "ReflectionDebug", "Volumetric clouds refl. (ReflectionDebug)", DbgKind::IntZero },
		{ "Shaders.VolumetricLight.RT", "DebugView", "RT light shafts (DebugView)", DbgKind::IntZero },
	};
	static const DbgItem kDbgReflPbr[] = {
		{ "Shaders.Reflections.Main", "DebugView", "Reflections SSR (DebugView)", DbgKind::IntZero },
		{ "Shaders.Reflections.RT", "DebugView", "RT reflections (DebugView)", DbgKind::IntZero },
		{ "Shaders.Reflections.RT", "DenoiserDebugView", "RT reflections denoiser (DenoiserDebugView)", DbgKind::IntZero },
		{ "Shaders.PBR.Metal", "DebugView", "PBR metal (DebugView)", DbgKind::IntZero },
		{ "Shaders.PBR.MaterialRules", "DebugView", "PBR material rules (DebugView)", DbgKind::BoolFalse },
		{ "Shaders.PBR.Rmaos", "DebugView", "PBR RMAOS maps (DebugView)", DbgKind::IntZero },
		{ "Shaders.POM.Displacement", "DebugView", "Object displacement (DebugView)", DbgKind::IntZero },
	};
	static const DbgItem kDbgRt[] = {
		{ "Main.Main.RayTracing", "PTDebugView", "Path tracing (PTDebugView)", DbgKind::IntZero },
		{ "Main.Develop.Main", "DebugMode", "Develop DebugMode", DbgKind::BoolFalse },
		{ "Main.Develop.Main", "GpuDebugLabels", "GPU debug labels (GpuDebugLabels)", DbgKind::BoolFalse },
		{ "Main.Develop.Main", "DebugVar1", "Develop DebugVar1", DbgKind::FloatZero },
		{ "Main.Develop.Main", "DebugVar2", "Develop DebugVar2", DbgKind::FloatZero },
		{ "Main.Develop.Main", "DebugVar3", "Develop DebugVar3", DbgKind::FloatZero },
		{ "Main.Develop.Main", "DebugVar4", "Develop DebugVar4", DbgKind::FloatZero },
		{ "Main.Develop.PixelInspector", "Enabled", "Pixel inspector Enabled", DbgKind::BoolFalse },
		{ "Main.Develop.PixelInspector", "X", "Pixel inspector X", DbgKind::PixelCoord },
		{ "Main.Develop.PixelInspector", "Y", "Pixel inspector Y", DbgKind::PixelCoord },
		{ "Main.Develop.PixelInspector", "LogEvery", "Pixel inspector LogEvery", DbgKind::IntZero },
		{ "Shaders.Debug.Main", "ClipView", "Clip view (ClipView)", DbgKind::IntZero },
		{ "@effect", "Debug", "Debug overlay", DbgKind::EffectDebug },
	};
	static const DbgItem kDbgTerrain[] = {
		{ "Shaders.Terrain.Blending", "DebugView", "Terrain blending (DebugView)", DbgKind::IntZero },
		{ "Shaders.Terrain.Displacement", "DebugView", "Terrain displacement (DebugView)", DbgKind::IntZero },
		{ "Shaders.Grass.GPUGrass", "DebugView", "GPU grass (DebugView)", DbgKind::IntZero },
		{ "Shaders.Grass.Interaction", "Debug", "Grass interaction (Debug)", DbgKind::BoolFalse },
		{ "Shaders.SnowAccumulation.Main", "CapturedNormalsDebugView", "Snow normals (CapturedNormalsDebugView)", DbgKind::IntZero },
		{ "Shaders.WetWorld.Main", "PersistentDebugView", "Wet world persistent (PersistentDebugView)", DbgKind::IntZero },
		{ "Shaders.WetnessEffects.Debug", "ShowCover", "Wetness cover (ShowCover)", DbgKind::BoolFalse },
		{ "Shaders.WetnessEffects.Debug", "WetnessOverride", "Wetness override", DbgKind::FloatNegOne },
		{ "Shaders.WetnessEffects.Debug", "PuddleOverride", "Puddle override", DbgKind::FloatNegOne },
		{ "Shaders.WetnessEffects.Debug", "RainOverride", "Rain override", DbgKind::FloatNegOne },
		{ "Shaders.WetnessEffects.HudRaindrops", "DebugView", "HUD raindrops (DebugView)", DbgKind::IntZero },
		{ "Shaders.WetnessEffects.Reflections", "DebugView", "Wetness reflections (DebugView)", DbgKind::BoolFalse },
		{ "Shaders.Puddles.Debug", "DebugView", "Puddles (DebugView)", DbgKind::IntZero },
	};
	static const DbgItem kDbgMisc[] = {
		{ "Shaders.Skin.Debug", "DebugView", "Skin (DebugView)", DbgKind::IntZero },
		{ "Shaders.Skin.SSS", "DebugView", "Skin SSS (DebugView)", DbgKind::IntZero },
		{ "Shaders.HandBlood.Main", "DebugView", "Hand blood (DebugView)", DbgKind::IntZero },
		{ "Shaders.TAA.Main", "DebugView", "TAA (DebugView)", DbgKind::IntZero },
		{ "Shaders.TAA.FSR", "DebugView", "FSR (DebugView)", DbgKind::IntZero },
		{ "Shaders.TAA.FSR", "ReactiveDebug", "FSR reactive mask (ReactiveDebug)", DbgKind::BoolFalse },
		{ "Shaders.DitherBuster.Main", "DebugView", "Dither buster (DebugView)", DbgKind::IntZero },
		{ "Shaders.HDR.Main", "ShowHudMask", "HDR HUD mask (ShowHudMask)", DbgKind::BoolFalse },
		{ "Main.GunFX.Volume", "Debug", "GunFX volume smoke (Debug)", DbgKind::BoolFalse },
	};
	static const DbgGroup kDbgGroups[] = {
		{ "Deferred & G-Buffer", kDbgDeferred, _countof(kDbgDeferred) },
		{ "Lighting, AO & GI", kDbgLighting, _countof(kDbgLighting) },
		{ "Shadows (Sun, Lamps & Clouds)", kDbgShadows, _countof(kDbgShadows) },
		{ "Reflections, PBR & Materials", kDbgReflPbr, _countof(kDbgReflPbr) },
		{ "Ray Tracing & Path Tracing", kDbgRt, _countof(kDbgRt) },
		{ "Terrain, Grass & Weather", kDbgTerrain, _countof(kDbgTerrain) },
		{ "Characters, AA, HDR & Misc", kDbgMisc, _countof(kDbgMisc) },
	};

	auto isActive = [](const DbgItem& item, bool countOnly) -> bool {
		switch (item.Kind) {
		case DbgKind::IntZero:
		case DbgKind::BoolFalse:
			return TheSettingManager->GetSettingI(item.Section, item.Key) != 0;
		case DbgKind::FloatZero:
			return fabsf(TheSettingManager->GetSettingF(item.Section, item.Key)) > 1e-4f;
		case DbgKind::FloatNegOne:
			return TheSettingManager->GetSettingF(item.Section, item.Key) >= 0.0f;
		case DbgKind::FloatOne:
			return fabsf(TheSettingManager->GetSettingF(item.Section, item.Key) - 1.0f) > 1e-3f;
		case DbgKind::PixelCoord:
			return !countOnly && TheSettingManager->GetSettingI("Main.Develop.PixelInspector", "Enabled") != 0;
		case DbgKind::EffectDebug:
			return EffectOn("Debug");
		}
		return false;
	};

	int activeCount = 0;
	for (const DbgGroup& g : kDbgGroups) {
		for (size_t i = 0; i < g.Count; i++) {
			const DbgItem& e = g.Items[i];
			if (strcmp(e.Section, "@effect") != 0 && ShelvedEffects::Section(e.Section)) continue;
			if (isActive(e, true)) activeCount++;
		}
	}

	static char dbgFilter[64] = "";
	static bool dbgActiveOnly = false;
	ImGui::SetNextItemWidth(ImMin(ImGui::GetFontSize() * 16.0f, ImGui::GetContentRegionAvail().x * 0.45f));
	ImGui::InputTextWithHint("##dbgfilter", "Filter debug views...", dbgFilter, sizeof(dbgFilter));
	ImGui::SameLine();
	ImGui::Checkbox("Active only##dbgactive", &dbgActiveOnly);
	ImGui::SameLine();
	if (ImGui::Button("Reset all debug views##dbgreset")) {
		for (const DbgGroup& g : kDbgGroups) {
			for (size_t i = 0; i < g.Count; i++) {
				const DbgItem& e = g.Items[i];
				if (strcmp(e.Section, "@effect") != 0 && ShelvedEffects::Section(e.Section)) continue;
				switch (e.Kind) {
				case DbgKind::IntZero:     TheSettingManager->SetSetting(e.Section, e.Key, 0); break;
				case DbgKind::BoolFalse:   TheSettingManager->SetSetting(e.Section, e.Key, false); break;
				case DbgKind::FloatZero:   TheSettingManager->SetSetting(e.Section, e.Key, 0.0f); break;
				case DbgKind::FloatNegOne: TheSettingManager->SetSetting(e.Section, e.Key, -1.0f); break;
				case DbgKind::FloatOne:    TheSettingManager->SetSetting(e.Section, e.Key, 1.0f); break;
				case DbgKind::EffectDebug: SetShaderEnabled("Debug", false); break;
				case DbgKind::PixelCoord:  break;
				}
			}
		}
		TheSettingManager->LoadSettings();
	}
	if (activeCount > 0) {
		ImGui::SameLine();
		ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "(%d active)", activeCount);
	}
	ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.5f));

	const bool filtering = dbgFilter[0] != '\0' || dbgActiveOnly;
	int shownGroups = 0;
	for (const DbgGroup& g : kDbgGroups) {
		std::vector<const DbgItem*> matched;
		bool hasActive = false;
		for (size_t i = 0; i < g.Count; i++) {
			const DbgItem& e = g.Items[i];
			if (strcmp(e.Section, "@effect") != 0 && ShelvedEffects::Section(e.Section)) continue;
			const bool active = isActive(e, false);
			if (active) hasActive = true;
			if (dbgActiveOnly && !active) continue;
			if (dbgFilter[0] && !ContainsNoCase(e.Label, dbgFilter) && !ContainsNoCase(e.Section, dbgFilter) &&
				!ContainsNoCase(e.Key, dbgFilter) && !ContainsNoCase(g.Title, dbgFilter))
				continue;
			matched.push_back(&e);
		}
		if (matched.empty()) continue;
		shownGroups++;
		const std::string header = std::string(g.Title) + "###dbggrp_" + g.Title;
		bool unsaved = false;	// 2026-11-25 (unsaved marks): one of the views listed under it
		for (const DbgItem* e : matched)
			unsaved = unsaved || (!strcmp(e->Section, "@effect") ? EffectSwitchMarked(e->Key) : UnsavedSettings::Key(e->Section, e->Key));
		if (!MenuWidgets::SubHeader(header.c_str(), filtering || hasActive, unsaved)) continue;
		ImGui::PushID(g.Title);
		for (const DbgItem* e : matched) {
			if (!strcmp(e->Section, "@effect")) {
				DrawVisualEffectSwitch(e->Key, e->Label);
				continue;
			}
			DrawSettingKey(e->Section, e->Key, false, e->Label);
		}
		ImGui::PopID();
	}
	if (shownGroups == 0)
		ImGui::TextDisabled(dbgActiveOnly ? "No debug views are currently active." : "No debug views match the current filter.");
}

// ---- Under Development & First-person Gear Weather pages -------------------------------------------------------------------
static const char* const kUnderDevTabId = "@UnderDevelopment";
static const char* const kGearWeatherTabId = "@GearWeather";
static const char* const kScopeGearWeather = "Main.Main.Quality:GearWeather, Main.Main.Quality:GearWeatherStrength, "
	"Main.Main.Quality:GearWeatherFrost, Main.Main.Quality:GearWeatherSand, Main.Main.Quality:GearWeatherWet, "
	"Main.Main.Quality:GearWeatherMud, Main.Main.Quality:GearWeatherSpeed, Main.Main.Quality:GearWeatherSkin, "
	"Main.Main.Quality:GearWeatherScale, Main.Main.Quality:GearWeatherForce";
static const char* const kScopeUnderDev = "Main.Main.Quality:GearWeather, Main.Main.Quality:GearWeatherStrength, "
	"Main.Main.Quality:GearWeatherFrost, Main.Main.Quality:GearWeatherSand, Main.Main.Quality:GearWeatherWet, "
	"Main.Main.Quality:GearWeatherMud, Main.Main.Quality:GearWeatherSpeed, Main.Main.Quality:GearWeatherSkin, "
	"Main.Main.Quality:GearWeatherScale, Main.Main.Quality:GearWeatherForce, "
	"Shaders.WorldSurface.Status:Enabled, Shaders.WorldSurface.Main, Shaders.WorldSurface.Dust, "
	"Shaders.WorldSurface.Rust, Shaders.WorldSurface.Waterline, Shaders.WorldSurface.Moss, "
	"Main.Main.Quality:MaterialSSS, "
	"Shaders.ShadowsExteriors.Deferred:RealSunPath, Shaders.ShadowsExteriors.Deferred:SunNoonElevation, "
	"Shaders.ShadowsExteriors.Deferred:SunAzimuthOffset, Shaders.ShadowsExteriors.Deferred:SunSheen, "
	"Shaders.ShadowsExteriors.Deferred:SunSheenRoughness";

static void DrawGearWeatherControls() {
	DrawSettingKey("Main.Main.Quality", "GearWeather", false);
	{
		const bool fpOn = TheSettingManager->GetSettingI("Main.Main.Quality", "DeferredFirstPerson") != 0;
		if (!fpOn) {
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextColored(kRestartOrange, "Requires DeferredFirstPerson (restart).");
			ImGui::PopTextWrapPos();
			DrawSettingKey("Main.Main.Quality", "DeferredFirstPerson", false);
		}
	}
	if (TheSettingManager->GetSettingI("Main.Main.Quality", "GearWeather") != 0) {
		static const char* const kGwKeys[] = {
			"GearWeatherStrength", "GearWeatherFrost", "GearWeatherSand",
			"GearWeatherWet", "GearWeatherMud", "GearWeatherSpeed",
			"GearWeatherSkin", "GearWeatherScale"
		};
		for (const char* k : kGwKeys) DrawSettingKey("Main.Main.Quality", k, false);
		static const char* const kGwPreview[] = {
			"Auto (live weather)", "Wet (rain)", "Frost & snowflakes", "Sandstorm dust & grit", "Mud splash", "All layers"
		};
		const int forceVal = std::clamp(TheSettingManager->GetSettingI("Main.Main.Quality", "GearWeatherForce"), 0, 5);
		ImGui::PushID("gw_preview");
		const float pw = MenuWidgets::RowBegin("Preview mode", MenuWidgets::ResetSlotWidth(), UnsavedSettings::Key("Main.Main.Quality", "GearWeatherForce"));	// 2026-11-25 (unsaved marks)
		ImGui::SetNextItemWidth(pw);
		const int pickedPreview = NamePicker("##gw_prev", kGwPreview[forceVal], kGwPreview, 6, forceVal);
		if (ImGui::IsItemHovered() || MenuWidgets::RowLabelHovered())
			WrappedTooltip("Live preview override for inspecting first-person frost/snowflakes, sandstorm grit, rain wetness, or mud in any weather.");
		ImGui::PopID();
		if (pickedPreview >= 0 && pickedPreview != forceVal) {
			TheSettingManager->SetSetting("Main.Main.Quality", "GearWeatherForce", pickedPreview);
			TheSettingManager->LoadSettings();
		}
	}
}

static void RenderGearWeatherPage() {
	PageTitle("Gear weather (1st-person)", "Snow/frost rime, sandstorm grit, rain wetness, and mud splashes on first-person weapons, hands, and sleeves.");
	ImGui::PushID("gw_page");
	DrawGearWeatherControls();
	ImGui::PopID();
}

static void RenderUnderDevelopmentPage() {
	PageTitle("Under Development", "Experimental features currently in active development. Changes here edit the same settings as their own sections.");

	if (MenuWidgets::SubHeader("First-person gear weather###underdev_gw", true, ScopeMarked(kScopeGearWeather))) {
		ImGui::PushID("ud_gw");
		DrawGearWeatherControls();
		ImGui::PopID();
		ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.35f));
	}

	const bool wsMarked = ScopeMarked("Shaders.WorldSurface.Status:Enabled, Shaders.WorldSurface.Main, Shaders.WorldSurface.Dust, Shaders.WorldSurface.Rust, Shaders.WorldSurface.Waterline, Shaders.WorldSurface.Moss");
	if (MenuWidgets::SubHeader("World surface detail & weathering###underdev_ws", true, wsMarked)) {
		DrawVisualEffectSwitch("WorldSurface", "World surface detail & weathering");
		DrawVisualItem({ "Shaders.WorldSurface.Main", nullptr, nullptr }, false);
		DrawVisualItem({ "Shaders.WorldSurface.Dust", nullptr, "Crevice & seam dust" }, true);
		DrawVisualItem({ "Shaders.WorldSurface.Rust", nullptr, "Metal oxidation & rust" }, true);
		DrawVisualItem({ "Shaders.WorldSurface.Waterline", nullptr, "Waterline dampness" }, true);
		DrawVisualItem({ "Shaders.WorldSurface.Moss", nullptr, "Shaded stone moss" }, true);
		ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.35f));
	}

	if (MenuWidgets::SubHeader("Material subsurface scattering (MaterialSSS)###underdev_sss", true, UnsavedSettings::Key("Main.Main.Quality", "MaterialSSS"))) {
		DrawSettingKey("Main.Main.Quality", "MaterialSSS", false, "Material SSS mode");
		ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.35f));
	}

	const bool sunMarked = ScopeMarked("Shaders.ShadowsExteriors.Deferred:RealSunPath, Shaders.ShadowsExteriors.Deferred:SunNoonElevation, Shaders.ShadowsExteriors.Deferred:SunAzimuthOffset, Shaders.ShadowsExteriors.Deferred:SunSheen, Shaders.ShadowsExteriors.Deferred:SunSheenRoughness");
	if (MenuWidgets::SubHeader("Natural outdoor sunlight (RealSunPath & SunSheen)###underdev_sun", true, sunMarked)) {
		DrawSettingKey("Shaders.ShadowsExteriors.Deferred", "RealSunPath", false, "Physical sun arc");
		DrawSettingKey("Shaders.ShadowsExteriors.Deferred", "SunNoonElevation", false, "Solar noon elevation (deg)");
		DrawSettingKey("Shaders.ShadowsExteriors.Deferred", "SunAzimuthOffset", false, "Compass azimuth offset (deg)");
		DrawSettingKey("Shaders.ShadowsExteriors.Deferred", "SunSheen", false, "Sunlight micro-rough sheen");
		DrawSettingKey("Shaders.ShadowsExteriors.Deferred", "SunSheenRoughness", false, "Sun sheen lobe width");
	}
}

// ---- Quick Settings & Status landing page --------------------------------------------------------------------------------
static int DetectAaUpscalerMode() {
	if (EffectOn("TAA")) {
		if (RtOn("Shaders.TAA.DLSS", "Enabled")) return 5;
		if (RtOn("Shaders.TAA.FSR", "Enabled")) return 4;
		return 3;
	}
	if (EffectOn("SMAA")) return 2;
	if (RtOn("Main.Main.ReducedQuality", "FXAA")) return 1;
	return 0;
}

static void RenderQuickSettingsPage() {
	PageTitle("Quick settings", "Runtime status and main graphics switches in one place. Pick any folder on the left for full effect controls.");

	const ImVec4 green(0.45f, 0.85f, 0.45f, 1.0f);
	const ImVec4 amber(1.0f, 0.75f, 0.2f, 1.0f);
	const float statusCol = ImGui::GetFontSize() * 9.0f;	// matches DrawRtStatusLine exactly

	auto statusLine = [&](const char* label, const ImVec4* col, const char* text) {
		ImGui::TextUnformatted(label);
		ImGui::SameLine(statusCol);
		ImGui::PushTextWrapPos(0.0f);
		if (col) ImGui::TextColored(*col, "%s", text);
		else ImGui::TextDisabled("%s", text);
		ImGui::PopTextWrapPos();
	};

	// Two balanced columns when wide enough (same threshold as RenderSimpleEffectPage) so there is no dead space
	const int cols = ImGui::GetContentRegionAvail().x > ImGui::GetFontSize() * 55.0f ? 2 : 1;
	if (!ImGui::BeginTable("##quickcards", cols, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_PadOuterX)) return;

	// ---- Left column: Runtime status, Core graphics, Screenshot mode ----
	ImGui::TableNextColumn();
	ImGui::PushStyleColor(ImGuiCol_Text, MenuSkin::Heading(0.25f));
	MenuWidgets::Title("Runtime status", 1.1f);
	ImGui::PopStyleColor();
	ImGui::Separator();
	statusLine("DXVK fork", DxvkCompute::Available() ? &green : &kRestartOrange,
		DxvkCompute::Available() ? "Loaded (Vulkan compute, tessellation & RT)" : "Not loaded (D3D9 / stock DXVK - no Vulkan or RT)");
	statusLine("Renderer", &green, "Deferred (Full: outdoors + indoors)");
	statusLine("Location", nullptr, TheShaderManager->GameState.isExterior ? "Outdoors (uses outdoor settings)" : "Indoors (uses indoor settings)");
	if (DxvkCompute::Available()) {
		DrawRtStatusLine("Ray tracing", RtCoreLine());
		if (RtPathTrace::Replacing()) DrawRtStatusLine("Path tracing", RtPathTraceLine());
	}

	ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 1.25f));
	ImGui::PushStyleColor(ImGuiCol_Text, MenuSkin::Heading(0.25f));
	MenuWidgets::Title("Core graphics", 1.1f);
	ImGui::PopStyleColor();
	ImGui::Separator();
	{
		bool effects = TheSettingManager->SettingsMain.Main.RenderEffects;
		MenuWidgets::RowBegin("All effects", MenuWidgets::ResetSlotWidth(), ScopeMarked(kScopeAllEffects));	// 2026-11-25 (unsaved marks)
		if (MenuWidgets::Toggle("##qs_fx", &effects)) {
			TheSettingManager->SetSetting("Main.Main.Misc", "RenderEffects", effects);
			TheSettingManager->LoadSettings();
		}
		if (ImGui::IsItemHovered() || MenuWidgets::RowLabelHovered())
			WrappedTooltip("Master switch: turns all Vegas Shaders effects on or off at once (hotkey: modifier + menu key).");

		if (ImGui::Button("Open Deferred Renderer...##qs_jump_deferred")) gMenu.page = kDeferredTabId;

		static const char* const kAaNames[] = {
			"Off",
			"FXAA (fast)",
			"SMAA (sharp, low cost)",
			"TAA (temporal anti-aliasing)",
			"FSR (AMD upscaler / AA)",
			"DLSS (NVIDIA upscaler / AA)"
		};
		const int aaCount = DxvkCompute::Available() ? 6 : 4;
		const int aaMode = (std::min)(DetectAaUpscalerMode(), aaCount - 1);
		ImGui::PushID("qs_aa");
		const float aaW = MenuWidgets::RowBegin("Anti-aliasing", MenuWidgets::ResetSlotWidth(), ModeMarked("aa", DetectAaUpscalerMode));	// 2026-11-25 (unsaved marks)
		ImGui::SetNextItemWidth(aaW);
		const int pickedAa = NamePicker("##aa", kAaNames[aaMode], kAaNames, aaCount, aaMode);
		if (ImGui::IsItemHovered() || MenuWidgets::RowLabelHovered())
			WrappedTooltip(DxvkCompute::Available()
				? "Picks one anti-aliasing or upscaling method and turns conflicting ones off cleanly."
				: "Picks one anti-aliasing method. FSR and DLSS are disabled because the Vegas Shaders DXVK fork is not loaded.");
		ImGui::PopID();
		if (pickedAa >= 0 && pickedAa != aaMode) {
			PickerWrites writes("aa", DetectAaUpscalerMode);	// 2026-11-26 (unsaved marks)
			TheSettingManager->SetSetting("Main.Main.ReducedQuality", "FXAA", pickedAa == 1);
			SetShaderEnabled("SMAA", pickedAa == 2);
			SetShaderEnabled("TAA", pickedAa >= 3);
			if (DxvkCompute::Available()) {
				TheSettingManager->SetSetting("Shaders.TAA.FSR", "Enabled", pickedAa == 4);
				TheSettingManager->SetSetting("Shaders.TAA.DLSS", "Enabled", pickedAa == 5);
			}
			TheSettingManager->LoadSettings();
		}
		if (DxvkCompute::Available()) {
			if (aaMode == 4) DrawVisualItem({ "Shaders.TAA.FSR", "RenderScale", nullptr }, false);
			else if (aaMode == 5) DrawVisualItem({ "Shaders.TAA.DLSS", "RenderScale", nullptr }, false);
		}
	}

	ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 1.25f));
	ImGui::PushStyleColor(ImGuiCol_Text, MenuSkin::Heading(0.25f));
	MenuWidgets::Title("Screenshot mode", 1.1f);
	ImGui::PopStyleColor();
	ImGui::Separator();
	{
		const bool asked = TheSettingManager->GetSettingI("Main.Main.Quality", "MaxQuality") != 0;
		bool maxQuality = asked;
		if (asked) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.45f, 1.0f));
		MenuWidgets::RowBegin("Max Quality", MenuWidgets::ResetSlotWidth(), ScopeMarked(kScopeMaxQuality));	// 2026-11-25 (unsaved marks)
		const bool flipped = MenuWidgets::Toggle("##qs_maxquality", &maxQuality);
		if (asked) ImGui::PopStyleColor();
		if (flipped) {
			TheSettingManager->SetSetting("Main.Main.Quality", "MaxQuality", maxQuality);
			TheSettingManager->LoadSettings();
		}
		if (ImGui::IsItemHovered() || MenuWidgets::RowLabelHovered())
			WrappedTooltip("For screenshots, not for normal play: forces full resolution and highest quality across effects (often half frame rate or less). "
				"Your own settings are preserved. Press Save and restart the game to apply.");
		ImGui::SameLine();
		if (asked != MaxQuality::Active) ImGui::TextColored(kRestartOrange, "restart to apply");
		else ImGui::TextDisabled("(screenshots, restart)");
	}

	// ---- Right column: Ambient occlusion & bounce light, Ray tracing ----
	ImGui::TableNextColumn();
	ImGui::PushStyleColor(ImGuiCol_Text, MenuSkin::Heading(0.25f));
	MenuWidgets::Title("Ambient occlusion & bounce light", 1.1f);
	ImGui::PopStyleColor();
	ImGui::Separator();
	DrawIndirectLightingPickers();
	if (ImGui::Button("Tune AO & Bounce Light...##qs_jump_ao")) gMenu.page = kAoTabId;

	if (DxvkCompute::Available()) {
		ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 1.25f));
		ImGui::PushStyleColor(ImGuiCol_Text, MenuSkin::Heading(0.25f));
		MenuWidgets::Title("Ray tracing", 1.1f);
		ImGui::PopStyleColor();
		ImGui::Separator();
		DrawRtSpaceSwitches();
		for (const SimpleRow& r : kSimpleRows) {
			if (!r.Key && !strcmp(r.Section, "Main.Main.RayTracing") && !strcmp(r.Label, "Path tracing preset")) {
				DrawSimpleChoice(r);
				break;
			}
		}
		if (ImGui::Button("Open Ray Tracing page...##qs_jump_rt")) gMenu.page = kRtTabId;
	}

	ImGui::EndTable();
}

// ---- Simple Category pages (merged per-category view when Advanced is off) ------------------------------------------------
// In Simple mode, each graphics category in the sidebar opens one unified page combining its mode pickers (where effects compete)
// and per-effect cards with inline on/off toggles and plain-English settings in a 2-column layout. Flipping Advanced on brings
// back the full per-shader folder tree in the sidebar (and expands every card's raw settings if kept open).
static const char* const kCatPrefix = "@Cat.";

static bool DrawCategoryEffectHeader(const char* shader, const char* title, bool& enabledOut, bool marked = false) {	// marked: 2026-11-25 (unsaved marks)
	const SettingManager::MenuSwitch kind = TheSettingManager->GetMenuShaderSwitch(shader);
	const bool hasSwitch = kind != SettingManager::MenuSwitch::None;
	bool on = hasSwitch ? TheSettingManager->GetMenuShaderEnabled(shader) : true;
	const bool forced = hasSwitch && TheSettingManager->IsShaderForced(shader);

	ImGui::PushID(shader);
	ImGui::PushStyleColor(ImGuiCol_Text, on ? MenuSkin::Heading(0.25f) : MenuWidgets::MutedText(0.45f));
	MenuWidgets::Title(title, 1.08f, marked || EffectSwitchMarked(shader));
	ImGui::PopStyleColor();
	if (hasSwitch) {
		RestartNote note;
		const std::string switchSec = std::string("Shaders.") + shader + (kind == SettingManager::MenuSwitch::MainEnabled ? ".Main" : ".Status");
		const bool noted = FindRestartNote(switchSec.c_str(), "Enabled", note);
		const float gap = ImGui::GetStyle().ItemSpacing.x;
		const float wanted = MenuWidgets::ToggleWidth() + (noted ? ImGui::CalcTextSize(note.text).x + gap : 0.0f);
		ImGui::SameLine();
		const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - wanted;
		if (right > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(right);
		if (noted) { DrawRestartNote(note); ImGui::SameLine(); }
		ImGui::BeginDisabled(forced);
		if (MenuWidgets::Toggle("##catfx", &on) && (!on || ConfirmEffectOn(shader))) {
			TheShaderManager->SwitchShaderStatus(shader);
			TheSettingManager->LoadSettings();
		}
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip(forced ? "Locked by a preset." : "Turn %s on or off.", title);
	}
	ImGui::Separator();
	ImGui::PopID();
	enabledOut = on;
	return on;
}

static void DrawSimpleEffectCard(const char* shader, const char* title, std::initializer_list<const char*> subSections,
	const char* noteWhenOff = nullptr, bool embedLut = false)
{
	if (ShelvedEffects::Section(std::string("Shaders.") + shader)) return;
	ImGui::PushID(shader);
	bool on = false;
	bool cardMarked = false;	// 2026-11-25 (unsaved marks): the card's title, for any of the sections it shows (its switch: the header's own)
	for (const char* sub : subSections) cardMarked = cardMarked || UnsavedSettings::Section((std::string("Shaders.") + shader + "." + sub).c_str());
	DrawCategoryEffectHeader(shader, title, on, cardMarked);
	if (!strcmp(shader, "HDR")) {
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextColored(kRestartOrange, "Warning: HDR currently causes freezes for some players. A fix is being researched.");
		ImGui::PopTextWrapPos();
	}
	if (on) {
		DrawPtNote(shader);
		if (embedLut) RenderLutPage();
		const bool multi = subSections.size() > 1;
		s_inCategoryPage = true;
		for (const char* sub : subSections) {
			const std::string sec = std::string("Shaders.") + shader + "." + sub;
			if (ShelvedEffects::Section(sec)) continue;
			if (multi) {
				const std::string subTitle = SimplePageTitle(sub) + "###cat_" + shader + "_" + sub;
				const bool defOpen = !strcmp(sub, "Main") || !strcmp(sub, "Default") || !strcmp(sub, "Exterior") || !strcmp(sub, "Exteriors") ||
					!strcmp(sub, "Interiors") || !strcmp(sub, "Scattering") || !strcmp(sub, "Forward") || !strcmp(sub, "GPUGrass") ||
					!strcmp(sub, "Physical") || !strcmp(sub, "Blending") || !strcmp(sub, "FirstPersonView") || !strcmp(sub, "HudRaindrops");
				if (!MenuWidgets::SubHeader(subTitle.c_str(), defOpen, UnsavedSettings::Section(sec.c_str()))) continue;	// 2026-11-25 (unsaved marks)
			}
			if (sec == "Shaders.Terrain.Displacement" || sec == "Shaders.POM.Displacement") TessellationStatus();
			DrawVisualItem({ sec.c_str(), nullptr, nullptr }, false);
		}
		s_inCategoryPage = false;
	}
	else if (noteWhenOff && noteWhenOff[0]) {
		ImGui::TextDisabled("%s", noteWhenOff);
	}
	ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.9f));
	ImGui::PopID();
}

static bool BeginCategoryColumns() {
	const int cols = ImGui::GetContentRegionAvail().x > ImGui::GetFontSize() * 55.0f ? 2 : 1;
	return ImGui::BeginTable("##catcards", cols, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_PadOuterX);
}

static void CategoryFooter() {
	ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.5f));
	ImGui::TextDisabled("Tip: Switch Advanced on (top right) for individual shader folders and all raw settings.");
}

// 1. Lighting & AO
static int ReflectionsMode() { return !EffectOn("Reflections") ? 0 : RtOn("Shaders.Reflections.RT", "Enabled") ? 2 : 1; }	// 2026-11-26 (unsaved marks): the picker's mode

static void RenderCatLighting() {
	PageTitle("Lighting & AO", "Ambient occlusion, sky occlusion, bounce light (GI), reflections, PBR materials, specular highlights, and flashlight.");
	DrawPtNote("AmbientLight");

	// Competing mode pickers across the top
	if (MenuWidgets::SubHeader("Ambient occlusion, sky occlusion & bounce light###cat_il_modes", true,
			ModeMarked("ao", DetectAoMode) || ModeMarked("sky", DetectSkyOcclusionMode) || ModeMarked("gi", DetectBounceLightMode))) {	// 2026-11-25 (unsaved marks)
		DrawIndirectLightingPickers();
		const int aoMode = DetectAoMode(), skyMode = DetectSkyOcclusionMode(), giMode = DetectBounceLightMode();
		if (giMode == 1 && !DxvkCompute::Available())
			ImGui::TextColored(kRestartOrange, "Screen-space GI (ComputeGI) needs the DXVK fork's d3d9.dll (not loaded in this session).");
		if (giMode == 2) DrawRtStatusLine("RT Bounce", RtGILine());
		if (skyMode == 2) DrawRtStatusLine("RT Sky", RtSkyLine());
		if (aoMode == 4) DrawRtStatusLine("RT AO", RtAOLine());
		if (ImGui::Button("Open full AO & Bounce Light tuning...##cat_jump_ao")) gMenu.page = kAoTabId;
		ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.5f));
	}

	if (BeginCategoryColumns()) {
		ImGui::TableNextColumn();
		// Active AO & GI quick tuning
		const int aoMode = DetectAoMode(), skyMode = DetectSkyOcclusionMode(), giMode = DetectBounceLightMode();
		s_inCategoryPage = true;
		if (aoMode == 1) {
			ImGui::PushStyleColor(ImGuiCol_Text, MenuSkin::Heading(0.25f));
			MenuWidgets::Title("Ambient Light tuning", 1.08f);
			ImGui::PopStyleColor(); ImGui::Separator();
			DrawVisualItem({ "Shaders.AmbientLight.Main", "OcclusionStrength, Radius, DrawDistance, DarkestOcclusion, BrightnessFade", nullptr }, false);
			ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.9f));
		}
		else if (aoMode == 2 || aoMode == 3) {
			DrawSimpleEffectCard("AmbientOcclusion", "Ambient occlusion", { "Exteriors", "Interiors", "GTAO" });
		}
		else if (aoMode == 4) {
			ImGui::PushStyleColor(ImGuiCol_Text, MenuSkin::Heading(0.25f));
			MenuWidgets::Title("Ray-traced Ambient Occlusion", 1.08f);
			ImGui::PopStyleColor(); ImGui::Separator();
			DrawVisualItem({ "Shaders.AmbientOcclusion.RT", "RayCount, Distance", nullptr }, false);
			if (EffectOn("AmbientLight"))
				DrawVisualItem({ "Shaders.AmbientLight.Main", "OcclusionStrength, DarkestOcclusion, BrightnessFade", nullptr }, false);
			else if (EffectOn("AmbientOcclusion")) {
				DrawVisualItem({ "Shaders.AmbientOcclusion.Exteriors", "ClampStrength, LumThreshold", "Outdoors" }, false);
				DrawVisualItem({ "Shaders.AmbientOcclusion.Interiors", "ClampStrength, LumThreshold", "Indoors" }, false);
			}
			ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.9f));
		}
		if (skyMode > 0) {
			ImGui::PushStyleColor(ImGuiCol_Text, MenuSkin::Heading(0.25f));
			MenuWidgets::Title("Sky occlusion (under roofs)", 1.08f);
			ImGui::PopStyleColor(); ImGui::Separator();
			DrawVisualItem({ "Shaders.AmbientOcclusion.Skylighting", "Strength, Distance, Reflections", nullptr }, false);
			ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.9f));
		}
		if (giMode == 1) {
			ImGui::PushStyleColor(ImGuiCol_Text, MenuSkin::Heading(0.25f));
			MenuWidgets::Title("Screen-space bounce light (ComputeGI)", 1.08f);
			ImGui::PopStyleColor(); ImGui::Separator();
			DrawVisualItem({ "Shaders.AmbientLight.Main", "ComputeGIStrength, ComputeGIQuality", nullptr }, false);
			ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.9f));
		}
		else if (giMode == 2) {
			ImGui::PushStyleColor(ImGuiCol_Text, MenuSkin::Heading(0.25f));
			MenuWidgets::Title("Ray-traced bounce light (GI)", 1.08f);
			ImGui::PopStyleColor(); ImGui::Separator();
			DrawVisualItem({ "Shaders.AmbientLight.Main", "SunGI, RoomGI, AmbientReplace, AmbientFloor", nullptr }, false);
			DrawVisualItem({ "Shaders.AmbientOcclusion.RT", "GIStrength, GIDistance, GISky, SkyMissSH, RadianceCache, GIStrengthInterior, GlowGI", nullptr }, false);
			ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.9f));
		}
		else if (giMode == 3) {
			ImGui::PushStyleColor(ImGuiCol_Text, MenuSkin::Heading(0.25f));
			MenuWidgets::Title("3D Light Grid bounce", 1.08f);
			ImGui::PopStyleColor(); ImGui::Separator();
			DrawVisualItem({ "Shaders.AmbientLight.Main", "GridBounce, GridLamps", nullptr }, false);
			ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.9f));
		}
		else if (giMode == 4) {
			DrawSimpleEffectCard("BounceLight", "Bounce light", { "Main" });
		}
		s_inCategoryPage = false;

		DrawSimpleEffectCard("PBR", "PBR materials & highlights", { "Main", "Metal", "Rmaos", "Linear" }, "Physically based roughness, metalness and linear lighting.");
		DrawSimpleEffectCard("Specular", "Specular highlights", { "Exterior", "Rain" }, "Sun glints and sky sheen on surfaces.");

		ImGui::TableNextColumn();
		// Reflections mode picker + card
		{
			static const char* const kReflModes[] = { "Off", "Screen-space reflections (SSR)", "Screen-space + Ray-traced (RTX + DXVK)" };
			const int reflMode = ReflectionsMode();	// 2026-11-26 (unsaved marks)
			ImGui::PushStyleColor(ImGuiCol_Text, MenuSkin::Heading(0.25f));
			MenuWidgets::Title("Reflections", 1.08f);
			ImGui::PopStyleColor(); ImGui::Separator();
			ImGui::PushID("cat_refl_mode");
			const float rw = MenuWidgets::RowBegin("Mode", MenuWidgets::ResetSlotWidth(), ModeMarked("refl", ReflectionsMode));	// 2026-11-25 (unsaved marks)
			ImGui::SetNextItemWidth(rw);
			const int picked = NamePicker("##refl", kReflModes[reflMode], kReflModes, 3, reflMode);
			ImGui::PopID();
			if (picked >= 0 && picked != reflMode) {
				PickerWrites writes("refl", ReflectionsMode);	// 2026-11-26 (unsaved marks)
				SetShaderEnabled("Reflections", picked > 0);
				TheSettingManager->SetSetting("Shaders.Reflections.RT", "Enabled", picked == 2);
				TheSettingManager->LoadSettings();
			}
			if (reflMode > 0) {
				s_inCategoryPage = true;
				DrawVisualItem({ "Shaders.Reflections.Main", nullptr, nullptr }, false);
				if (reflMode == 1 && MenuWidgets::SubHeader("Weapon reflections###cat_refl_weapon", true, ScopeMarked("Shaders.Reflections.RT:Weapon, Shaders.Reflections.RT:WeaponHands, Shaders.Reflections.RT:WeaponMetal, Shaders.Reflections.RT:WeaponRoughness"))) {	// 2026-11-25 (unsaved marks)
					DrawVisualItem({ "Shaders.Reflections.RT", "Weapon, WeaponHands, WeaponMetal, WeaponRoughness", nullptr }, false);
				}
				if (reflMode == 2 && MenuWidgets::SubHeader("Ray-traced reflections###cat_refl_rt", true, UnsavedSettings::Section("Shaders.Reflections.RT"))) {
					DrawRtStatusLine("Status", RtReflectionsLine());
					DrawVisualItem({ "Shaders.Reflections.RT", nullptr, nullptr }, false);
				}
				s_inCategoryPage = false;
			}
			ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.9f));
		}
		DrawSimpleEffectCard("Flashlight", "Flashlight / Pip-Boy light", { "Main", "Deferred" }, "Real shadow-casting flashlight beam.");
		ImGui::EndTable();
	}
	CategoryFooter();
}

// 2. Shadows
// 2026-11-26 (unsaved marks): the three shadow pickers' modes
static int SunShadowsMode() { return !EffectOn("ShadowsExteriors") ? 0 : RtOn("Shaders.ShadowsExteriors.RT", "Enabled") ? 2 : 1; }
static int CloudShadowsMode() { return CloudsCastShadows() ? 2 : SettingOn("Shaders.CloudShadows.Main", "Enabled") ? 1 : 0; }
static int LampShadowsMode() { return !EffectOn("ShadowsInteriors") ? 0 : RtOn("Shaders.ShadowsInteriors.RT", "Enabled") ? 2 : 1; }

static void RenderCatShadows() {
	PageTitle("Shadows", "Sun shadows, contact hardening, cloud shadows, and indoor/night lamp shadows.");

	// Competing mode pickers across the top
	if (MenuWidgets::SubHeader("Shadow modes###cat_sh_modes", true,
			ModeMarked("sun", SunShadowsMode) || ModeMarked("cloud", CloudShadowsMode) || ModeMarked("lamp", LampShadowsMode))) {	// 2026-11-25 (unsaved marks)
		// Sun shadows mode
		static const char* const kSunModes[] = { "Off", "Shadow maps", "Shadow maps + Ray-traced (RTX + DXVK)" };
		const int sunMode = SunShadowsMode();	// 2026-11-26 (unsaved marks)
		ImGui::PushID("cat_sun");
		const float sw = MenuWidgets::RowBegin("Sun shadows", MenuWidgets::ResetSlotWidth(), ModeMarked("sun", SunShadowsMode));	// 2026-11-25 (unsaved marks)
		ImGui::SetNextItemWidth(sw);
		const int pSun = NamePicker("##sun", kSunModes[sunMode], kSunModes, 3, sunMode);
		ImGui::PopID();
		if (pSun >= 0 && pSun != sunMode) {
			PickerWrites writes("sun", SunShadowsMode);	// 2026-11-26 (unsaved marks)
			SetShaderEnabled("ShadowsExteriors", pSun > 0);
			TheSettingManager->SetSetting("Shaders.ShadowsExteriors.RT", "Enabled", pSun == 2);
			TheSettingManager->LoadSettings();
		}

		// Cloud shadows mode
		static const char* const kCloudModes[] = { "Off", "Flat cloud shadows (cheap)", "Volumetric cloud shadows" };
		const int cloudMode = CloudShadowsMode();	// 2026-11-26 (unsaved marks)
		ImGui::PushID("cat_cloudsh");
		const float cw = MenuWidgets::RowBegin("Cloud shadows", MenuWidgets::ResetSlotWidth(), ModeMarked("cloud", CloudShadowsMode));	// 2026-11-25 (unsaved marks)
		ImGui::SetNextItemWidth(cw);
		const int pCloud = NamePicker("##cloudsh", kCloudModes[cloudMode], kCloudModes, 3, cloudMode);
		ImGui::PopID();
		if (pCloud >= 0 && pCloud != cloudMode) {
			PickerWrites writes("cloud", CloudShadowsMode);	// 2026-11-26 (unsaved marks)
			if (pCloud == 0) {
				TheSettingManager->SetSetting("Shaders.CloudShadows.Main", "Enabled", false);
				TheSettingManager->SetSetting("Shaders.VolumetricClouds.Main", "Shadows", 0.0f);
			}
			else if (pCloud == 1) {
				TheSettingManager->SetSetting("Shaders.CloudShadows.Main", "Enabled", true);
				TheSettingManager->SetSetting("Shaders.VolumetricClouds.Main", "Shadows", 0.0f);
			}
			else {
				SetShaderEnabled("VolumetricClouds", true);
				if (SettingF("Shaders.VolumetricClouds.Main", "Shadows") <= 0.0f)
					TheSettingManager->SetSetting("Shaders.VolumetricClouds.Main", "Shadows", 0.6f);
				TheSettingManager->SetSetting("Shaders.CloudShadows.Main", "Enabled", false);
			}
			TheSettingManager->LoadSettings();
		}

		// Lamp shadows mode
		static const char* const kLampModes[] = { "Off", "Shadow maps (cube maps)", "Ray-traced lamp shadows (RTX + DXVK, restart)" };
		const int lampMode = LampShadowsMode();	// 2026-11-26 (unsaved marks)
		ImGui::PushID("cat_lamp");
		const float lw = MenuWidgets::RowBegin("Lamp shadows", MenuWidgets::ResetSlotWidth(), ModeMarked("lamp", LampShadowsMode));	// 2026-11-25 (unsaved marks)
		ImGui::SetNextItemWidth(lw);
		const int pLamp = NamePicker("##lamp", kLampModes[lampMode], kLampModes, 3, lampMode);
		ImGui::PopID();
		if (pLamp >= 0 && pLamp != lampMode) {
			PickerWrites writes("lamp", LampShadowsMode);	// 2026-11-26 (unsaved marks)
			SetShaderEnabled("ShadowsInteriors", pLamp > 0);
			TheSettingManager->SetSetting("Shaders.ShadowsInteriors.RT", "Enabled", pLamp == 2);
			TheSettingManager->LoadSettings();
		}
		ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.5f));
	}

	if (BeginCategoryColumns()) {
		ImGui::TableNextColumn();
		DrawSimpleEffectCard("ShadowsExteriors", "Sun shadows (outdoors)", { "Main", "ScreenSpace", "SSShadows", "TerrainShadows" }, "Sun shadow maps, contact shadows and mountain shadows.");
		if (RtOn("Shaders.ShadowsExteriors.RT", "Enabled")) {
			s_inCategoryPage = true;
			if (MenuWidgets::SubHeader("Ray-traced sun shadows###cat_sun_rt", true, UnsavedSettings::Section("Shaders.ShadowsExteriors.RT"))) {
				DrawRtStatusLine("Status", RtSunLine());
				DrawVisualItem({ "Shaders.ShadowsExteriors.RT", nullptr, nullptr }, false);
			}
			s_inCategoryPage = false;
			ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.9f));
		}
		DrawSimpleEffectCard("ContactHardening", "Contact hardening (soft sun edges)", { "Main" }, "Softens sun shadow edges with distance from the caster (restart to turn on).");
		if (SettingOn("Shaders.CloudShadows.Main", "Enabled") && !CloudsCastShadows())
			DrawSimpleEffectCard("CloudShadows", "Flat cloud shadows", { "Main" });

		ImGui::TableNextColumn();
		DrawSimpleEffectCard("ShadowsInteriors", "Lamp shadows (indoors & night)", { "Main", "Forward", "Atlas" }, "Real-time shadows cast by lamps, fires and lights.");
		if (RtOn("Shaders.ShadowsInteriors.RT", "Enabled")) {
			s_inCategoryPage = true;
			if (MenuWidgets::SubHeader("Ray-traced lamp shadows###cat_lamp_rt", true, UnsavedSettings::Section("Shaders.ShadowsInteriors.RT"))) {
				DrawRtStatusLine("Status", RtLampLine());
				DrawVisualItem({ "Shaders.ShadowsInteriors.RT", nullptr, nullptr }, false);
			}
			s_inCategoryPage = false;
		}
		ImGui::EndTable();
	}
	CategoryFooter();
}

// 3. Sky & atmosphere
static void RenderCatSky() {
	PageTitle("Sky & atmosphere", "Sky, volumetric clouds, fog, sunbeams, god rays, sandstorms, and heat haze.");

	// Competing fog style picker
	if (MenuWidgets::SubHeader("Fog mode###cat_fog_mode", true, ModeMarked("fog", FogMode))) {	// 2026-11-25 (unsaved marks)
		static const char* const kFogModes[] = { "Off", "Current fog (this build)", "Classic fog (original look)", "Legacy fog (older presets)" };
		const FogType cur = ActiveFog();
		const int idx = cur == FogType::None ? 0 : cur == FogType::Current ? 1 : cur == FogType::Classic ? 2 : 3;
		ImGui::PushID("cat_fog");
		const float fw = MenuWidgets::RowBegin("Volumetric fog style", MenuWidgets::ResetSlotWidth(), ModeMarked("fog", FogMode));
		ImGui::SetNextItemWidth(fw);
		const int picked = NamePicker("##fog", kFogModes[idx], kFogModes, 4, idx);
		ImGui::PopID();
		if (picked >= 0 && picked != idx) {
			PickerWrites writes("fog", FogMode);	// 2026-11-26 (unsaved marks)
			if (picked == 0) SetFogType(cur, false);
			else SetFogType(picked == 1 ? FogType::Current : picked == 2 ? FogType::Classic : FogType::Legacy, true);
		}
		ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.5f));
	}

	if (BeginCategoryColumns()) {
		ImGui::TableNextColumn();
		DrawSimpleEffectCard("Sky", "Sky & realistic atmosphere", { "Main", "Physical", "Clouds" }, "Sky tinting, stars, moon, and physically simulated sky.");
		DrawSimpleEffectCard("VolumetricClouds", "Volumetric clouds", { "Main" }, "3D drifting volumetric clouds and cloud shadows.");
		DrawSimpleEffectCard("VolumetricFog", "Volumetric fog", { "Main", "Night", "Interiors" }, "Atmospheric distance haze and indoor fog.");

		ImGui::TableNextColumn();
		DrawSimpleEffectCard("VolumetricLight", "Light shafts (volumetric)", { "Main", "Night" }, "Sunbeams and moonlight shafts through trees and buildings.");
		DrawSimpleEffectCard("GodRays", "God rays (screen-space)", { "Main", "Coloring" }, "Classic screen-space sun shafts.");
		DrawSimpleEffectCard("SunGlare", "Sun glare", { "Main" }, "Soft halo and glare around the sun.");
		DrawSimpleEffectCard("Sandstorm", "Sandstorms", { "Main" }, "Rolling dust clouds and blowing sand streaks.");
		DrawSimpleEffectCard("HeatHaze", "Desert heat haze", { "Main" }, "Distant shimmering air during hot Mojave afternoons.");
		DrawSimpleEffectCard("DustMotes", "Dust motes", { "Main" }, "Floating lit dust specks in the air.");
		ImGui::EndTable();
	}
	CategoryFooter();
}

// 4. Weather & water
static int WetnessMode() { return EffectOn("WetnessEffects") ? 1 : EffectOn("WetWorld") ? 2 : 0; }	// 2026-11-26 (unsaved marks): the picker's mode

static void RenderCatWeather() {
	PageTitle("Weather & water", "Rain, snow, surface wetness, puddles, and water shaders.");

	// Competing wetness picker
	if (MenuWidgets::SubHeader("Wetness system###cat_wet_mode", true, ModeMarked("wet", WetnessMode))) {	// 2026-11-25 (unsaved marks)
		static const char* const kWetModes[] = { "Off", "WetnessEffects + Puddles (recommended)", "Legacy WetWorld" };
		const int wetMode = WetnessMode();	// 2026-11-26 (unsaved marks)
		ImGui::PushID("cat_wet");
		const float ww = MenuWidgets::RowBegin("Rain wetness", MenuWidgets::ResetSlotWidth(), ModeMarked("wet", WetnessMode));
		ImGui::SetNextItemWidth(ww);
		const int picked = NamePicker("##wet", kWetModes[wetMode], kWetModes, 3, wetMode);
		ImGui::PopID();
		if (picked >= 0 && picked != wetMode) {
			PickerWrites writes("wet", WetnessMode);	// 2026-11-26 (unsaved marks)
			SetShaderEnabled("WetnessEffects", picked == 1);
			SetShaderEnabled("WetWorld", picked == 2);
			TheSettingManager->LoadSettings();
		}
		RestartBadge("Shaders.WetnessEffects.Status", "Enabled");
		ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.5f));
	}

	if (BeginCategoryColumns()) {
		ImGui::TableNextColumn();
		DrawSimpleEffectCard("Precipitations", "Rain", { "Main" }, "Rain streak visibility, speed and refraction.");
		DrawSimpleEffectCard("Snow", "Snowfall", { "Main" }, "Falling snow (world-space and classic).");
		DrawSimpleEffectCard("SnowAccumulation", "Snow on ground", { "Main" }, "Snow build-up, melting and footprints.");
		if (EffectOn("WetnessEffects")) {
			DrawSimpleEffectCard("WetnessEffects", "Wetness & raindrops", { "Main", "Raindrops", "HudRaindrops" });
			DrawSimpleEffectCard("Puddles", "Puddles", { "Main" }, "Dynamic rain puddles with ripples (needs restart to toggle).");
		}
		else if (EffectOn("WetWorld")) {
			DrawSimpleEffectCard("WetWorld", "Legacy WetWorld", { "Main" });
		}

		ImGui::TableNextColumn();
		DrawSimpleEffectCard("Water", "Water", { "Default", "Interiors", "Placed" }, "Water clarity, waves, caustics and reflections.");
		DrawSimpleEffectCard("Underwater", "Underwater", {}, "Underwater depth tint and distortion.");
		DrawSimpleEffectCard("WaterLens", "Water drops on screen", { "Main" }, "Dripping water on the camera when leaving water.");

		ImGui::PushID("GearWeatherCard");
		ImGui::PushStyleColor(ImGuiCol_Text, MenuSkin::Heading(0.25f));
		MenuWidgets::Title("First-person gear weather", 1.08f, ScopeMarked(kScopeGearWeather));
		ImGui::PopStyleColor();
		ImGui::Separator();
		DrawGearWeatherControls();
		ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.9f));
		ImGui::PopID();

		ImGui::EndTable();
	}
	CategoryFooter();
}

// 5. Ground & plants
static void RenderCatGround() {
	PageTitle("Ground & plants", "Terrain blending, tessellation, parallax occlusion, strand grass, and wind in plants.");
	if (BeginCategoryColumns()) {
		ImGui::TableNextColumn();
		DrawSimpleEffectCard("Terrain", "Terrain", { "Blending", "Variation", "Macro", "Parallax", "Displacement" }, "Ground blending, anti-tiling and tessellation.");
		DrawSimpleEffectCard("WorldSurface", "World surface detail & weathering", { "Main", "Dust", "Rust", "Waterline", "Moss" }, "World-space micro-detail grain, crevice dust, metal rust, waterline dampness, and moss.");
		DrawSimpleEffectCard("POM", "Parallax occlusion & rock tessellation", { "Main", "Displacement" }, "3D depth on stone, brick and cliff textures.");

		ImGui::TableNextColumn();
		DrawSimpleEffectCard("Grass", "Grass & strand grass", { "Main", "GPUGrass", "Wind", "Interaction" }, "3D GPU grass blades, wind sway and player bending.");
		DrawSimpleEffectCard("Plants", "Plant wind", { "Wind" }, "Bushes and tree leaves swaying in the wind.");
		DrawSimpleEffectCard("Foliage", "Foliage glow", { "Main" }, "Backlit sunlight glowing through leaves.");
		ImGui::EndTable();
	}
	CategoryFooter();
}

// 6. Characters
static void RenderCatCharacters() {
	PageTitle("Characters", "Skin shading (outdoors and indoors), moving cloth, Pip-Boy sleeve fitting, and blood on hands/weapons.");
	if (BeginCategoryColumns()) {
		ImGui::TableNextColumn();
		DrawSimpleEffectCard("Skin", "Skin shading & subsurface scattering", { "Main", "Scattering", "Interiors" }, "Realistic skin highlights, softness and backlit glow.");

		ImGui::TableNextColumn();
		DrawSimpleEffectCard("Cloth", "Moving cloth & coats", { "Main", "Interiors" }, "Swinging coats, capes, flags and banners (needs restart to enable).");
		DrawSimpleEffectCard("PipboyFit", "Pip-Boy sleeve fit", { "Main" }, "Pulls armour sleeves under the Pip-Boy (needs restart).");
		DrawSimpleEffectCard("HandBlood", "Blood on weapon & hands", { "Main" }, "Close-range blood spatter on your weapon and gloves (needs restart).");
		ImGui::EndTable();
	}
	CategoryFooter();
}

// 7. Camera & colour
static int ExposureMode() { return !EffectOn("Exposure") ? 0 : TheSettingManager->GetSettingI("Shaders.Exposure.Main", "Mode") == 1 ? 2 : 1; }	// 2026-11-26 (unsaved marks): the picker's mode

static void RenderCatCamera() {
	PageTitle("Camera & colour", "Tonemapping, exposure, HDR, bloom, LUTs, colour grading, depth of field, motion blur, and camera looks.");

	// Competing exposure picker
	if (MenuWidgets::SubHeader("Exposure mode###cat_exp_mode", true, ModeMarked("exp", ExposureMode))) {	// 2026-11-25 (unsaved marks)
		static const char* const kExpModes[] = { "Off", "Classic (screen average)", "Eye adaptation (whole view + white balance)" };
		const int expMode = ExposureMode();	// 2026-11-26 (unsaved marks)
		ImGui::PushID("cat_exp");
		const float ew = MenuWidgets::RowBegin("Exposure", MenuWidgets::ResetSlotWidth(), ModeMarked("exp", ExposureMode));
		ImGui::SetNextItemWidth(ew);
		const int picked = NamePicker("##exp", kExpModes[expMode], kExpModes, 3, expMode);
		ImGui::PopID();
		if (picked >= 0 && picked != expMode) {
			PickerWrites writes("exp", ExposureMode);	// 2026-11-26 (unsaved marks)
			SetShaderEnabled("Exposure", picked > 0);
			if (picked > 0) TheSettingManager->SetSetting("Shaders.Exposure.Main", "Mode", picked == 2 ? 1 : 0);
			TheSettingManager->LoadSettings();
		}
		ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.5f));
	}

	if (BeginCategoryColumns()) {
		ImGui::TableNextColumn();
		DrawSimpleEffectCard("Tonemapping", "Tone mapping", { "Main", "Interiors", "Night" }, "Filmic tone curve, contrast and night colour shift.");
		DrawSimpleEffectCard("Exposure", "Exposure", { "Main", "Interiors", "Night" }, "Auto-exposure brightness adaptation.");
		DrawSimpleEffectCard("LocalExposure", "Local exposure", { "Main" }, "Keeps bright skies and dark corners balanced together.");
		if (DxvkCompute::Available())
			DrawSimpleEffectCard("HDR", "HDR output (freeze warning)", { "Main" }, "High dynamic range output settings.");
		else if (MenuWidgets::SubHeader("HDR output (requires Vegas Shaders DXVK)###cat_hdr_nodxvk", false))
			ImGui::TextDisabled("HDR output is inactive on native D3D9 / stock DXVK (Windows Auto HDR is controlled in Windows Display settings).");
		DrawSimpleEffectCard("Bloom", "Bloom", { "Main", "Night", "Interiors" }, "Soft glow around bright lights and skies.");
		DrawSimpleEffectCard("BloomLegacy", "Legacy bloom", { "Exteriors", "Interiors" }, "Older bloom for legacy presets.");
		DrawSimpleEffectCard("LUT", "Colour lookup table (LUT)", { "Main" }, "Custom colour grading LUT files.", true);

		ImGui::TableNextColumn();
		DrawSimpleEffectCard("Coloring", "Colour grading", { "Default" }, "Saturation, contrast, gamma and RGB tint.");
		DrawSimpleEffectCard("ImageAdjust", "Image adjust", { "Main", "Night", "Interiors" }, "Fine brightness, contrast and colour adjustments.");
		DrawSimpleEffectCard("Lens", "Lens flare & dirt", { "Main", "Night", "Interiors" }, "Anamorphic flare and lens dust.");
		DrawSimpleEffectCard("DepthOfField", "Depth of field", { "FirstPersonView", "ThirdPersonView" }, "Background focus blur.");
		DrawSimpleEffectCard("MotionBlur", "Motion blur", { "FirstPersonView", "ThirdPersonView" }, "Camera turning blur.");
		DrawSimpleEffectCard("Cinema", "Cinema bars & grain", { "Main" }, "Letterbox bars, vignette and film grain.");
		DrawSimpleEffectCard("Bodycam", "Bodycam look", { "Main", "Night", "Interiors" }, "Body-worn camera lens and video look.");
		DrawSimpleEffectCard("LowHF", "Low health effect", { "Main" }, "Red vignette and desaturation at low health.");
		DrawSimpleEffectCard("Dream", "Dream look (extra)", { "Main" }, "Stylized soft dream filter.");
		ImGui::EndTable();
	}
	CategoryFooter();
}

// 8. Anti-aliasing & sharpening
static void RenderCatAA() {
	PageTitle("Anti-aliasing & sharpening", "Pick one anti-aliasing or upscaling method (FSR / DLSS / TAA / SMAA / FXAA), plus sharpening and de-banding.");

	if (MenuWidgets::SubHeader("Anti-aliasing / Upscaler###cat_aa_mode", true,
			ModeMarked("aa", DetectAaUpscalerMode) || ScopeMarked("Shaders.TAA.FSR:RenderScale, Shaders.TAA.DLSS:RenderScale"))) {	// 2026-11-25 (unsaved marks)
		static const char* const kAaNames[] = {
			"Off", "FXAA (fast)", "SMAA (sharp, low cost)",
			"TAA (temporal anti-aliasing)", "FSR (AMD upscaler / AA)", "DLSS (NVIDIA upscaler / AA)"
		};
		const int aaCount = DxvkCompute::Available() ? 6 : 4;
		const int aaMode = (std::min)(DetectAaUpscalerMode(), aaCount - 1);
		ImGui::PushID("cat_aa");
		const float aw = MenuWidgets::RowBegin("Method", MenuWidgets::ResetSlotWidth(), ModeMarked("aa", DetectAaUpscalerMode));	// 2026-11-25 (unsaved marks)
		ImGui::SetNextItemWidth(aw);
		const int picked = NamePicker("##aa", kAaNames[aaMode], kAaNames, aaCount, aaMode);
		ImGui::PopID();
		if (picked >= 0 && picked != aaMode) {
			PickerWrites writes("aa", DetectAaUpscalerMode);	// 2026-11-26 (unsaved marks)
			TheSettingManager->SetSetting("Main.Main.ReducedQuality", "FXAA", picked == 1);
			SetShaderEnabled("SMAA", picked == 2);
			SetShaderEnabled("TAA", picked >= 3);
			if (DxvkCompute::Available()) {
				TheSettingManager->SetSetting("Shaders.TAA.FSR", "Enabled", picked == 4);
				TheSettingManager->SetSetting("Shaders.TAA.DLSS", "Enabled", picked == 5);
			}
			TheSettingManager->LoadSettings();
		}
		if (!DxvkCompute::Available())
			ImGui::TextDisabled("FSR and DLSS are disabled automatically when the Vegas Shaders DXVK fork is not loaded.");
		s_inCategoryPage = true;
		if (DxvkCompute::Available()) {
			if (aaMode == 4) DrawVisualItem({ "Shaders.TAA.FSR", "RenderScale", nullptr }, false);
			else if (aaMode == 5) DrawVisualItem({ "Shaders.TAA.DLSS", "RenderScale", nullptr }, false);
		}
		s_inCategoryPage = false;
		ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.5f));
	}

	if (BeginCategoryColumns()) {
		ImGui::TableNextColumn();
		const int aaMode = DetectAaUpscalerMode();
		s_inCategoryPage = true;
		if (aaMode >= 3) {
			ImGui::PushStyleColor(ImGuiCol_Text, MenuSkin::Heading(0.25f));
			MenuWidgets::Title(aaMode == 5 ? "DLSS & TAA" : aaMode == 4 ? "FSR & TAA" : "TAA", 1.08f);
			ImGui::PopStyleColor(); ImGui::Separator();
			if (aaMode == 4) DrawVisualItem({ "Shaders.TAA.FSR", "-Enabled, RenderScale", nullptr }, false);
			else if (aaMode == 5) DrawVisualItem({ "Shaders.TAA.DLSS", "-Enabled, RenderScale", nullptr }, false);
			DrawVisualItem({ "Shaders.TAA.Main", nullptr, nullptr }, false);
			if (MenuWidgets::SubHeader("Smooth foliage & fence cutouts###cat_aa_cut", false, UnsavedSettings::Section("Shaders.TAA.Cutouts")))
				DrawVisualItem({ "Shaders.TAA.Cutouts", nullptr, nullptr }, false);
		}
		else if (aaMode == 2) {
			ImGui::PushStyleColor(ImGuiCol_Text, MenuSkin::Heading(0.25f));
			MenuWidgets::Title("SMAA", 1.08f);
			ImGui::PopStyleColor(); ImGui::Separator();
			DrawVisualItem({ "Shaders.SMAA.Main", nullptr, nullptr }, false);
		}
		else {
			ImGui::TextDisabled(aaMode == 1 ? "FXAA is active (no extra settings)." : "No anti-aliasing is active. Pick a method above.");
		}
		s_inCategoryPage = false;

		ImGui::TableNextColumn();
		DrawSimpleEffectCard("Sharpening", "Sharpening", { "Main", "Interiors" }, "Post-process image sharpening.");
		DrawSimpleEffectCard("DitherBuster", "Dither buster", { "Main" }, "Smooths dithered transparency patterns.");
		DrawSimpleEffectCard("SkyDither", "Sky de-banding", { "Main" }, "Removes colour banding in smooth sky gradients.");
		ImGui::EndTable();
	}
	CategoryFooter();
}

// 9. Particles & gun effects
static void RenderCatParticles() {
	PageTitle("Particles & gun effects", "Particle lighting, GPU particles, and built-in GunFX muzzle smoke, barrel glow and heat shimmer.");
	if (BeginCategoryColumns()) {
		ImGui::TableNextColumn();
		DrawSimpleEffectCard("Particles", "Particles", { "Main" }, "Soft blending and sun/lamp lighting on smoke and fire.");
		DrawSimpleEffectCard("GPUParticles", "GPU particles", { "Main" }, "GPU-accelerated particle effects.");

		ImGui::TableNextColumn();
		ImGui::PushStyleColor(ImGuiCol_Text, MenuSkin::Heading(0.25f));
		MenuWidgets::Title("Gun effects (GunFX)", 1.08f);
		ImGui::PopStyleColor(); ImGui::Separator();
		s_inCategoryPage = true;
		DrawVisualItem({ "Main.GunFX.Main", "EnergyWeapons", nullptr }, false);
		for (const GunFolder& f : kGunFolders) {
			if (f.AdvancedOnly && !AdvancedShown()) continue;
			const std::string header = std::string(f.Title) + "###catgun/" + f.Id;
			if (!MenuWidgets::SubHeader(header.c_str(), false, VisualItemsMarked(f.Items))) continue;	// 2026-11-25 (unsaved marks)
			ImGui::PushID(f.Id);
			ImGui::Indent(ImGui::GetFontSize() * 0.75f);
			for (const VisualItem& item : f.Items) DrawVisualItem(item, item.Title != nullptr);
			ImGui::Unindent(ImGui::GetFontSize() * 0.75f);
			ImGui::PopID();
		}
		s_inCategoryPage = false;
		ImGui::EndTable();
	}
	CategoryFooter();
}

struct SimpleCategory {
	const char* Id;
	const char* Label;
	const char* Tip;
	void (*Render)();
	const char* CountEffects;	// comma-separated shader names for the "X of Y on" badge (nullptr = special)
};
static const SimpleCategory kSimpleCategories[] = {
	{ "@Cat.Lighting",   "Lighting & AO",              "AO, sky occlusion, bounce light (GI), PBR, specular, reflections, flashlight.", RenderCatLighting,  "AmbientLight, Specular, PBR, Reflections, Flashlight" },
	{ "@Cat.Shadows",    "Shadows",                    "Sun shadows, contact hardening, cloud shadows, and lamp shadows.",              RenderCatShadows,   "ShadowsExteriors, ShadowsInteriors, ContactHardening" },
	{ kRtTabId,          "Ray tracing",                "Every ray-traced effect and path tracing in one place.",                        nullptr,            nullptr },
	{ "@Cat.Sky",        "Sky & atmosphere",           "Sky, volumetric clouds, fog, light shafts, god rays, sandstorms, heat haze.",   RenderCatSky,       "Sky, VolumetricClouds, VolumetricFog, VolumetricLight, GodRays, SunGlare, Sandstorm, HeatHaze" },
	{ "@Cat.Weather",    "Weather & water",            "Rain, snow, wetness, puddles, and water.",                                      RenderCatWeather,   "Precipitations, Snow, SnowAccumulation, WetnessEffects, Puddles, Water, Underwater, WaterLens" },
	{ "@Cat.Ground",     "Ground & plants",            "Terrain blending, tessellation, POM, strand grass, plants.",                    RenderCatGround,    "Terrain, POM, Grass, Plants" },
	{ "@Cat.Characters", "Characters",                 "Skin shading, moving cloth, Pip-Boy sleeve fit, hand blood.",                   RenderCatCharacters,"Skin, Cloth, PipboyFit, HandBlood" },
	{ "@Cat.Camera",     "Camera & colour",            "Tonemapping, exposure, HDR, bloom, LUTs, grading, DoF, motion blur, looks.",    RenderCatCamera,    "Tonemapping, Exposure, HDR, Bloom, Lens, LUT, Coloring, ImageAdjust, Cinema, Bodycam, DepthOfField, MotionBlur, LowHF" },
	{ "@Cat.AA",         "Anti-aliasing & sharpening", "FSR, DLSS, TAA, SMAA, FXAA, sharpening, de-banding.",                           RenderCatAA,        "TAA, SMAA, Sharpening, DitherBuster, SkyDither" },
	{ "@Cat.Particles",  "Particles & gun effects",    "Particle lighting, GPU particles, and GunFX smoke/barrel glow.",                RenderCatParticles, "Particles, GPUParticles" },
};

static bool RenderSimpleCategoryPage() {
	for (const SimpleCategory& c : kSimpleCategories) {
		if (c.Render && gMenu.page == c.Id) {
			c.Render();
			return true;
		}
	}
	return false;
}

static void RtSwitchCount(int& on, int& switches);
static bool SidebarMarked(const std::string& row);	// 2026-11-25 (unsaved marks): with the sidebar, below

static void DrawSimpleCategoriesSidebar() {
	bool anyMarked = false;	// 2026-11-25 (unsaved marks)
	for (const SimpleCategory& c : kSimpleCategories) anyMarked = anyMarked || SidebarMarked(c.Id);
	MenuWidgets::GroupLabel("Graphics", true, anyMarked);
	for (const SimpleCategory& c : kSimpleCategories) {
		const bool shown = gMenu.page == c.Id;
		ImGui::PushID(c.Id);
		ImGui::PushStyleColor(ImGuiCol_Text, shown ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : MenuWidgets::MutedText(0.35f));
		if (ImGui::Selectable(c.Label, shown)) gMenu.page = c.Id;
		MenuWidgets::MarkLastItem(c.Label, SidebarMarked(c.Id));	// 2026-11-25 (unsaved marks)
		ImGui::PopStyleColor();
		if (ImGui::IsItemHovered() && c.Tip) WrappedTooltip(c.Tip);
		int on = 0, total = 0;
		if (!c.CountEffects) RtSwitchCount(on, total);
		else {
			for (const char* p = c.CountEffects; *p; ) {
				while (*p == ' ' || *p == ',') p++;
				const char* end = p;
				while (*end && *end != ',') end++;
				if (end > p) {
					const std::string name(p, end);
					if (!ShelvedEffects::Section("Shaders." + name) && TheSettingManager->GetMenuShaderSwitch(name.c_str()) != SettingManager::MenuSwitch::None) {
						total++;
						if (TheSettingManager->GetMenuShaderEnabled(name.c_str())) on++;
					}
				}
				p = end;
			}
		}
		if (total > 0) {
			ImGui::SameLine();
			ImGui::TextDisabled("%d/%d", on, total);
		}
		ImGui::PopID();
	}
	ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.5f));
	ImGui::PushTextWrapPos(0.0f);
	ImGui::TextDisabled("Turn on Advanced (top right) for individual shader folders and all settings.");
	ImGui::PopTextWrapPos();
}

