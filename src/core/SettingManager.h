#pragma once

#define TOML11_PRESERVE_COMMENTS_BY_DEFAULT
#include "../lib/toml11/toml.hpp"


// Every [Main.*] setting in one place, filled by SettingManager::LoadSettings. The members are read all over the code by name;
// their order is kept.
struct SettingsMainStruct
{
	struct MainStruct {	// [Main.Main.*]
		bool	RemoveUnderwater, ForceMSAA, ForceReflections;	// [Main.Main.Water] / [Main.Main.Misc]
		bool	BlockGameMSAA;	// [Main.Main.Misc] UNOFFICIAL 2026-11-04: the INIs' iMultiSample / iWaterMultiSamples read as 0
		bool	GameShadersInReflections; // [Main.Main.Water] diagnostic: the reflection pass uses the game's own shaders
		bool	ReflectionClipFix; // [Main.Main.Water] UNOFFICIAL: the water reflection's clip plane rebuilt by NVR at every draw of the reflection pass
		bool	RemovePrecipitations;	// [Main.Main.Precipitations]
		bool	MemoryHeapManagement, MemoryTextureManagement;	// [Main.Main.Memory] HeapManagement, TextureManagement
		bool	ReplaceIntro, SkipFog, RenderEffects, RenderPreTonemapping;	// [Main.Main.Misc]
        bool    DisableFrameChain; // ![Main.Main.Performance] FrameChain (copy-free effect chain)
        bool    DisableWorldSceneGuard; // ![Main.Main.Performance] WorldSceneGuard (skip NVR effects while the game renders no world scene)
        bool    MenuSceneGuard; // [Main.Main.Performance] MenuSceneGuard (UNOFFICIAL 2026-10-20: skip them at once over a menu's still background: the Pip-Boy)
        bool    DisableCompositeApply; // ![Main.Main.Performance] CompositeApply (shadow/AO apply folded into fog)
        bool    DisableChainGameTexture; // ![Main.Main.Performance] ChainUsesGameTexture
        bool    DisableMergedNormals; // ![Main.Main.Performance] MergedDepthNormals
        bool    SkipIdentityExposure; // [Main.Main.Performance] SkipIdentityExposure (no exposure pass while MaxBrightness is 0: it then leaves the picture as it is), live
        bool    AvgLumaCompute; // [Main.Main.Performance] AvgLumaCompute (UNOFFICIAL 2026-10-23: the average-luma pass as a compute reduction; needs NVR DXVK, otherwise the D3D9 pass runs as before), live
        int     AvgLumaComputeCheck; // [Main.Main.Performance] AvgLumaComputeCheck (test tool: every N frames both paths run and are logged side by side; 0 off), live
        bool    NvrDxvkPasses; // [Main.Main.Performance] NvrDxvkPasses (UNOFFICIAL 2026-10-23: the ported full-screen effects as Vulkan passes; needs NVR DXVK), live
        bool    NvrDxvkVariants; // [Main.Main.Performance] NvrDxvkVariants (UNOFFICIAL 2026-10-26: Vulkan passes use their compile-time variants; DxvkCompute::VariantsOn), live
        bool    NvrDxvkAmbientLight; // [Main.Main.Performance] NvrDxvkAmbientLight (UNOFFICIAL 2026-11-01: AmbientLight's passes as Vulkan passes too; off = D3D9, AmbientLightEffect::ExtOn), live
        bool    NvrDxvkClouds; // [Main.Main.Performance] NvrDxvkClouds (UNOFFICIAL 2026-11-01: VolumetricClouds' passes as Vulkan passes too; off = D3D9, VolumetricCloudsEffect::ExtRender), live
        bool    NvrDxvkStencil; // [Main.Main.Performance] NvrDxvkStencil (UNOFFICIAL 2026-10-26: Vulkan passes test a stencil where their D3D9 technique does; needs NVR DXVK 1.6; DxvkCompute::StencilOn), live
        char    NvrDxvkTestSuffix[16]; // [Main.Main.Performance] NvrDxvkTestSuffix (UNOFFICIAL 2026-10-26, test: Vulkan passes use Compute\<name>-<variant><this>.spv where it exists; EffectRecord::ExtVariant), live
        char    NvrDxvkKeepD3D9[128]; // [Main.Main.Performance] NvrDxvkKeepD3D9 (UNOFFICIAL 2026-10-26, test: effect names that keep their D3D9 passes while NvrDxvkPasses is on; DxvkCompute::KeptOnD3D9), live
        bool    AmbientLightSmoothCompute; // [Main.Main.Performance] AmbientLightSmoothCompute (UNOFFICIAL 2026-10-25: AmbientLight's NormalSmoothing as one compute dispatch; needs NVR DXVK, otherwise the two D3D9 passes run as before), live
        bool    NvrDxvkReplacements; // [Main.Main.Performance] NvrDxvkReplacements (UNOFFICIAL 2026-10-24: pixel shaders with a Compute\Replace\<name>.spv run as that SPIR-V through the NVR DXVK extension; needs NVR DXVK 1.2), at load
        bool    AmbientLightBlurCompute; // [Main.Main.Performance] AmbientLightBlurCompute (UNOFFICIAL 2026-10-25: AmbientLight's BlurX + BlurY as one compute dispatch; needs NVR DXVK), live
        bool    NvrSm6Shaders; // [Main.Main.Performance] NvrSm6Shaders (UNOFFICIAL 2026-10-26: NVR's own effect passes with an SM6 build in Compute\Sm6 run from that SPIR-V, compiled from the same .hlsl without ps_3_0's limits; needs NVR DXVK; core\Sm6Pass.h), live
        int     SmoothNormals; // [Main.Main.Quality] SmoothNormals (UNOFFICIAL 2026-10-23: reach in pixels of the shared normal smoothing after the normals pass; 0 off), live
        bool    LampMaskScissor; // [Main.Main.Performance] LampMaskScissor (the lamp-mask pass drawn only inside its lamps' screen rectangle, not at all with none on screen; exact), live
        bool    InteriorShadowFold; // [Main.Main.Performance] InteriorShadowFold (indoors the unblurred lamp darkening applied in the fog composite, not its own pass), live
        int     InteriorShadowFoldRound; // [Main.Main.Performance] InteriorShadowFoldRound (DIAGNOSTIC: 0 as this GPU stores 16-bit floats (HalfStoreMode), 1 none, 2 toward zero, 3 nearest even), live
        bool    CubeFaceViewCull; // [Main.Main.Performance] CubeFaceViewCull (lamp cube faces no on-screen pixel can look up are not drawn while the lamp is redrawn every frame anyway), live
        bool    CubeFaceCopySkip; // [Main.Main.Performance] CubeFaceCopySkip (RedrawActorsOnly: a lamp cube face nobody moves in is not copied again while it still holds its static copy), live
        bool    SlimDepthBuffer; // [Main.Main.Performance] SlimDepthBuffer (read once at startup)
        bool    SkipRedundantConstants; // [Main.Main.Performance] SkipRedundantConstants (ConstantFilter.h), live
        bool    ShaderWarmUp; // [Main.Main.Performance] ShaderWarmUp (ShaderWarmUp.h: the driver compiles NVR shaders at the main menu)
        bool    SkinnedShadowFaceTest; // [Main.Main.Performance] SkinnedShadowFaceTest (ShadowManager.cpp: skeleton bounds for animated casters in lamp cube maps), live
        bool    HiddenObjectCulling; // [Main.Main.Performance] HiddenObjectCulling (HiddenObjectCulling.h: skip the extra passes of objects hidden a few frames ago), live
        bool    PreDecodeCache; // [Main.Main.Performance] PreDecodeCache (LinearPreDecode.h: powf results kept, decoded copies sent per call; same bits), live
        bool    FastCasterHash; // [Main.Main.Performance] FastCasterHash (ShadowManager.cpp: lamp caster hash over 4-byte words, once per object per frame), live
        bool    SkyOnWorker; // [Main.Main.Performance] SkyOnWorker (Sky.cpp + FrameWorker.h: the sky light projection on a worker thread, joined before its readers; same values), live
        bool    CasterListCache; // [Main.Main.Performance] CasterListCache (ShadowManager.cpp PointShadowCasterState: each lamp keeps its casters' motion entries, no map lookup per caster per lamp; with FastCasterHash), live
        int     HiddenObjectCullingMaxAge; // [Main.Main.Performance] HiddenObjectCullingMaxAge (oldest visibility answer used, 1-7 frames), live
        bool    ReShadeDepthCompat; // [Main.Main.Performance] ReShadeDepthCompat (keep the game's depth-stencil bound through NVR's passes, for ReShade; also on while ReShade is loaded, ReShadeIntegration::KeepDepthBound), live
        bool    ReShadeNVRDepth; // [Main.Main.Performance] ReShadeNVRDepth (ReShadeIntegration.cpp: ReShade's DEPTH = NVR's combined depth, world + first-person model), live
        bool    ReShadeBeforeHUD; // [Main.Main.Performance] ReShadeBeforeHUD (ReShadeIntegration.cpp: ReShade's effects drawn after NVR's, before the HUD), live
        bool    ReShadeUnderNVRMenu; // [Main.Main.Performance] ReShadeUnderNVRMenu (ReShadeIntegration::BeforeNVRMenu: ReShade drawn before NVR's menu when it has not drawn yet), live
        bool    ReShadeDepthNearRemap; // [Main.Main.Performance] ReShadeDepthNearRemap (ReShadeIntegration.cpp: first-person depths past the world near plane folded below 1; identity for g <= 0.5), live
        bool    ReShadeMenuGuard; // [Main.Main.Performance] ReShadeMenuGuard (ReShadeIntegration.cpp: the ReShadeMenuGuardEffects files paused while menus are open), live
        bool    ReShadeMenuGuardVATS; // [Main.Main.Performance] ReShadeMenuGuardVATS (the menu guard also in VATS), live
        bool    ReShadeGameValues; // [Main.Main.Performance] ReShadeGameValues (ReShadeIntegration.cpp: game weather/hour/worldspace/interior to ReShade uniforms with a source annotation, FNV ReShade Helper names), live
        bool    LampScissor; // [Main.Main.Performance] LampScissor (LampScissor.h: lamp-only passes drawn only inside their lamps' screen rectangle), live
        bool    LampScissorExact; // [Main.Main.Performance] LampScissorExact (LampScissor.h, 2026-10-21: the exact rectangle, also for lamps near the camera), live
        bool    NeonSpillSelfTest;    // [Shaders.BounceLight.Main] NeonSpillSelfTest (EffectSelfTest.h, DIAGNOSTIC: ticking it runs one in-game A/B test of the neon spill), live
        int     LampPassTestSkip; // [Main.Main.Performance] LampPassTestSkip (LampPassTest.h, DIAGNOSTIC: skip the passes a merged light pass would replace; 0 off), live
        bool    TrimLampPasses; // [Main.Main.Performance] TrimLampPasses (ObjectTemplate.hlsl: lamp passes drop pixels outside every lamp's reach), live
        bool    GodRaysLowRes; // [Main.Main.ReducedQuality] GodRaysLowRes (quarter instead of half resolution)
        bool    AOLowRes; // [Main.Main.ReducedQuality] AOLowRes (quarter instead of half resolution)
        bool    FXAA; // [Main.Main.ReducedQuality] FXAA (lean FXAA pass after SMAA)
        float   LampShadowMoveTolerance; // [Main.Main.ReducedQuality] LampShadowMoveTolerance (CHANGES THE IMAGE above 0: a lamp moving less than this many units from where its shadow cube was drawn keeps that cube and position), live
        int     PointShadowInterval; // [Main.Main.ReducedQuality] PointShadowInterval (redraw each point-light shadow cubemap every N frames, 1-4)
        bool    CheapReflections; // [Main.Main.ReducedQuality] CheapReflections (water reflection map drawn without sun shadows and terrain parallax)
        bool    CheapUnderwaterTerrain; // [Main.Main.ReducedQuality] CheapUnderwaterTerrain (ground below the water surface drawn without parallax)
        bool    ParallaxLite; // [Main.Main.ReducedQuality] ParallaxLite (8-step parallax with a secant step for terrain and parallax objects; terrain also two shadow taps). P60's TerrainParallaxLite.
        bool    StaggeredSunShadows; // [Main.Main.ReducedQuality] StaggeredSunShadows (middle sun-shadow cascade every 4th frame, far/Lod every 8th)
        bool    CachedDistantShadows; // [Main.Main.ReducedQuality] CachedDistantShadows (the stagger, but cascades with characters in them redraw every frame)
        int     NearCascadeInterval; // [Main.Main.ReducedQuality] NearCascadeInterval (redraw the near sun-shadow cascade every N frames, 1-2)
		bool	PresetManagerEnabled;	// [Main.Main.Misc]: location presets switch by themselves (docs/preset-manager-design.md)
		UInt8	AnisotropicFilter;		// [Main.Main.Misc] from here on
		UInt16	ScreenshotKey;			// a DirectInput key code
		bool	HDRScreenshot;			// the screenshot key also saves the HDR picture
		float	FarPlaneDistance;		// the camera's far plane
	};
	
	struct FrameRateStruct {	// [Main.FrameRate.SmartControl] and [Main.FrameRate.Stuttering]
		bool	SmartControl, SmartBackgroundProcess;
		UInt32	SmartControlFPS, BackgroundThreadPriority;
		float	FlowControl;	// SmartControl's frame-time slack
	};

	struct CullingProcessStruct {	// [Main.CullingProcess.Main]
		bool	EnableCulling, EnableReflectionCulling;
		float	CullMinSize, CullMinSizeReflection;
	};

	struct CameraModeStruct {	// [Main.CameraMode.Main] and [Main.CameraMode.Positioning]
		bool		Enabled, ChasingFirst, ChasingThird;
		UInt8		Crosshair;	// the crosshair mode
		NiPoint3	Offset, AimingOffset, DialogOffset;
		float		FoV;		// degrees
	};
	
	struct WeatherModeStruct {	// [Main.WeatherMode.*]: the weathers' night, fog and sun colours are scaled by these
		bool		Enabled;
		NiPoint3	CoeffNight, CoeffFog, CoeffSun;
	};


	struct MenuStruct {	// [Main.Menu.Style] and [Main.Menu.Keys]: what the ImGui menu reads from them (2026-11-05: the old text
		// menu's fonts, colours, layout and navigation keys are gone; the menu skin reads its own keys)
		UInt8	TextSize;				// the menu's text size, percent
		UInt8	KeyEnable;				// DirectInput key code that opens the menu
		UInt8	MasterSwitchModifier;	// with KeyEnable, switches every effect on or off
	};

	struct FlyCamStruct {	// [Main.FlyCam.Main]
		bool	Enabled;
		UInt8	KeyAdd, KeySubtract;
		float	ScrollMultiplier, StepValue;
	};

	struct DevelopStruct {	// [Main.Develop.Main]
		bool	DebugMode;		// hotkeys that dump textures
		UInt8	TraceShaders;	// logs the shaders drawn
	};

	struct TransitionsStruct { float TransitionCurvePower; };	// [Main.Transitions.Main]

	// the groups, in their original order
	MainStruct				Main;				// [Main.Main.*]
	CullingProcessStruct	CullingProcess;		// [Main.CullingProcess.*]
	CameraModeStruct		CameraMode;			// [Main.CameraMode.*]
	FrameRateStruct			FrameRate;			// [Main.FrameRate.*]
	WeatherModeStruct		WeatherMode;		// [Main.WeatherMode.*]
	MenuStruct				Menu;				// [Main.Menu.*]
	FlyCamStruct			FlyCam;				// [Main.FlyCam.*]
	DevelopStruct			Develop;			// [Main.Develop.*]
	TransitionsStruct		Transitions;		// [Main.Transitions.*]
};

using FormsList = std::vector<UInt32>;	// FormIDs (GetFormList)

// One [Shaders.Water.<name>] set (LoadSettings reads the keys of the same names).
struct SettingsWaterStruct {	// one water set
	float	choppiness, waveWidth, waveSpeed;
	float	reflectivity;	// of the surface
	float	causticsStrength, causticsStrengthS;
	float	shoreFactor, shoreMovement;
	float	turbidity, inScattCoeff;
	float	inExtCoeff_R, inExtCoeff_G, inExtCoeff_B;
	float	depthDarkness;	// how fast deep water darkens
};


// One [Shaders.Coloring.<name>] set.
struct SettingsColoringStruct {	// one colour-grading set
	float	Strength, BaseGamma, Fade, Contrast, Saturation;
	float	Bleach, BleachLuma;
	float	ColorCurve, ColorCurveR, ColorCurveG, ColorCurveB;
	float	EffectGamma, EffectGammaR, EffectGammaG, EffectGammaB;
	float	Linearization;	// the gamma the grading works in
};


// One weather's values from [Weathers.<name>.*], pushed into the game's weather by SetSettingsWeather.
struct SettingsWeatherStruct {	// one weather
	char	LowerLayer[80], UpperLayer[80];	// cloud textures
	UInt8	WindSpeed, CloudSpeedLower, CloudSpeedUpper, TransDelta, SunGlare, SunDamage;
	float	FogNearDay, FogNearNight, FogFarDay, FogFarNight;
	TESWeather::ColorData	Colors[TESWeather::kNumColorTypes];	// per colour type, per time of day
	float	HDR[14];	// in WeatherHDRTypes order
};

using SettingsWaterMap		= std::map<std::string, SettingsWaterStruct>;		// by set name
using SettingsColoringMap	= std::map<std::string, SettingsColoringStruct>;	// by set name
using SettingsWeatherMap	= std::map<std::string, SettingsWeatherStruct>;		// by weather EditorID
using StringList			= std::vector<std::string>;
using tomlValue				= toml::basic_value<toml::preserve_comments, std::map, std::vector>;	// a toml tree that keeps comments, tables sorted

class SettingManager : public SettingManagerBase
{
public:
	// The settings files in memory: the defaults (VegasShaders.defaults.toml, which decide what exists and its type), the
	// user's own file over them, and the Max Quality layer.
	class Configuration
	{
	public:
		enum NodeType { Boolean = 0, Integer = 1, Float = 2, String = 3 };	// a ConfigNode's Type

		struct SectionPosition { char* Start; char* End; bool Found; };	// unused, kept for its name

		// One setting with its value in every form.
		class ConfigNode
		{
		public:
			char		Section[40];	// "Shaders.Bloom.Main"
			char		Key[40];		// "Strength"
			char		MidSection[80];	// between the first and second dot: "Bloom"
			char		Value[80];		// formatted text representation; still read directly by the UI layer (cut at 79 characters)
			std::string	LongValue;		// UNOFFICIAL 2026-10-17: a String setting's whole text (Value cuts it at 79: a longer one overflowed Value and crashed the game)
			std::string Description;	// the default's first comment
			bool		Reboot;			// takes effect after a restart
			UInt32		Type;			// a NodeType
			bool		MaxQuality = false;	// UNOFFICIAL 2026-10-15: this value is Max Quality's, not the user's own (shown red in the menu)

			// Typed value, kept in sync with Value above. Populated for every node
			// regardless of Type (e.g. a Boolean node also gets IntValue/FloatValue)
			// so callers that read a setting through a different accessor
			// (GetSettingI on a bool, GetSettingF on an int, etc.) get the right
			// native value without re-parsing the text form.
			bool		BoolValue;	// the value as bool,
			int			IntValue;	// as int,
			float		FloatValue;	// and as float

			bool operator < (const ConfigNode& other) const { return strcoll(Key, other.Key) < 0; }	// by key, in the locale's order
		};

		using SettingList = std::vector<ConfigNode>;

		void			Init();	// (re)reads the files
		bool			FillNode(ConfigNode* node, const char* section, const char* key);	// false: not a setting
		void			FillSections(StringList* names, const char* parent);	// parent NULL: the top level
		void			FillSettings(SettingList* nodes, const char* section);
		void			SetValue(ConfigNode* node);	// into the user's table (or Max Quality's layer)
		void			CreateWeatherSection(const char* weatherName, TESWeather* weather);	// does nothing now
		tomlValue*		FindSection(tomlValue* root, StringList* steps);	// uses up steps; NULL when missing
		void			SetValueIn(tomlValue& Target, ConfigNode* Node);	// UNOFFICIAL: SetValue into the given table
		bool			IsMaxQualityKey(const char* Section, const char* Key);	// UNOFFICIAL: Max Quality sets this one (while it is on)
		static void		MergeTables(tomlValue& Into, const tomlValue& From);	// UNOFFICIAL: From's values over Into's, tables merged
		tomlValue*		UserSection(const char* section);	// the user's own table for a section, or NULL
		void			UpdateOldDefaults();	// one-time moves of saved old defaults ([_Unofficial] SettingsVersion)

		tomlValue		TomlConfig;		// the user's settings
		tomlValue		DefaultConfig;	// the defaults (with their comments)
		UInt32			DefaultsGeneration = 0;	// UNOFFICIAL 2026-10-18: counts the loads of the defaults file (Init); the menu's "New in this patch" list is rebuilt when it changes
		tomlValue		MaxQualityConfig;	// UNOFFICIAL 2026-10-15: Max Quality's settings (VegasShaders.maxquality.toml), read over the user's while it is on
		tomlValue		MaxQualityUser;		// UNOFFICIAL: the user's own changes to those (VegasShaders.maxquality.user.toml, also in MaxQualityConfig)
		bool			MaxQualityActive = false;
		bool			MaxQualityUserChanged = false;	// MaxQualityUser has changes the next Save writes
		bool			configLoaded;	// Init has run
		char			*Config, *ConfigB;	// unused, kept for their names
		UInt32			FileSize;		// unused
	};

	static void				Initialize();	// creates TheSettingManager
	void					LoadSettings();	// fills the structs below from the files in memory
	void					SaveSettings();	// writes the user's file (and Max Quality's changes)
	void					RevertSettings();	// back to what is on disk
	void					SaveSettingsTo(const char* path);
	int						GetSettingI(const char* section, const char* key);
	float					GetSettingF(const char* section, const char* key);
	char*					GetSettingS(const char* section, const char* key, char* out);	// out: at least 80 bytes
	float					GetSettingTransition(const char* section, const char* key, bool isExterior, float dayFactor);	// .Interiors, or .Night -> .Main
	void					SetSetting(const char* section, const char* key, float value);
	void					SetSetting(const char* section, const char* key, int value);
	void					SetSetting(const char* section, const char* key, bool value);
	void					SetSettingS(const char* section, const char* key, const char* value);	// from text, any type
	void					SetSettingF(const char* section, const char* key, float value);	// from a number, any type; reloads
	void					SetSetting(Configuration::ConfigNode* node);
	void					Increment(const char* section, const char* key);
	void					Decrement(const char* section, const char* key);
	void					SetSettingWeather(const char* section, const char* key, float value);
	void					FillMenuSections(StringList* names, const char* parent);
	void					FillMenuSettings(Configuration::SettingList* list, const char* section);
	void					GetFormList(const char* sectionName, FormsList* forms);
	void					CreateNode(Configuration::ConfigNode* node, const char* section, const char* key, float value, bool reboot);
	void					CreateNode(Configuration::ConfigNode* node, const char* section, const char* key, int value, bool reboot);
	void					CreateNode(Configuration::ConfigNode* node, const char* section, const char* key, bool value, bool reboot);
	void					CreateNodeS(Configuration::ConfigNode* node, const char* section, const char* key, const char* value, bool reboot);
	enum class MenuSwitch { Status, MainEnabled, None };	// UNOFFICIAL: which setting switches a Shaders list entry (GetMenuShaderSwitch)
	MenuSwitch				GetMenuShaderSwitch(const char* Name);
	bool					GetMenuShaderEnabled(const char* name);
	void					SetMenuShaderEnabled(const char* name, bool enabled);
	bool					GetMenuMiscEnabled(const char* key);	// [Main.Main.Misc]
	void					SetMenuMiscEnabled(const char* key, bool enabled);
	SettingsWaterStruct*	GetSettingsWater(const char* location);	// NULL when there is no such set
	SettingsColoringStruct* GetSettingsColoring(const char* location);
	SettingsWeatherStruct*	GetSettingsWeather(const char* weatherName);
	void					SetSettingsWeather(TESWeather* weather);
	bool					IsShaderForced(const char* name);	// always false

	template <class T> static std::string	ToString(const T number);	// through float, except bool and int
	template <class T> static T				FromString(const char* text);
	static void								SplitString(const char* text, const char* delimiters, StringList* parts);	// text < 256 characters
	template <class T> static void			FillFromString(const char* text, const char* delimiters, T* values);

	Configuration					Config;			// the files in memory
	UInt32							SettingsRevision = 0;	// UNOFFICIAL 2026-10-18: bumped by every SetSetting / LoadSettings; the menu's "New in this patch" list re-reads its values when it changes
	bool							GameLoading;		// a save is loading
	bool							SettingsChanged;	// the effects read their settings again
	bool							hasUnsavedChanges;	// the menu's Save has something to write
	SettingsMainStruct				SettingsMain;		// [Main.*]
	SettingsWaterMap				SettingsWater;		// [Shaders.Water.*]
	SettingsColoringMap				SettingsColoring;	// [Shaders.Coloring.*]
	SettingsWeatherMap				SettingsWeather;	// [Weathers.*]

private:
	void					FilterMenuSections(StringList* sections, const char* parent);	// hides advanced shadow pages below Custom quality
};
