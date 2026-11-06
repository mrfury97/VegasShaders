#pragma once

// Draws every shadow map NVR uses: the sun cascades in the shadow atlas, the ortho (sky / rain) map, the lamps' cube maps
// and the spot lights' maps. One instance, made by Initialize (value-initialised: members without an initialiser below
// start at zero) and kept for the whole session.
class ShadowManager {
public:
	static void Initialize();

	// The sun's maps, in atlas order (also the bit of each cascade in the cascade masks).
	enum ShadowMapTypeEnum { MapNear = 0, MapMiddle = 1, MapFar = 2, MapLod = 3, MapOrtho = 4 };

	// --- The caster walk: references and scene nodes into the passes' geometry lists ---
	NiNode*					GetRefNode(TESObjectREFR*, ShadowsExteriorEffect::FormsStruct*);	// null when the reference casts no shadow
	void					AccumChildren(NiAVObject* Root, ShadowsExteriorEffect::FormsStruct*, bool isLand, bool isLOD, NiFrustumPlanes* Planes = nullptr);
	bool					IsRefracting(TESObjectREFR*);
	void					AccumObject(NiAVObject* Geometry, ShadowsExteriorEffect::FormsStruct*, bool isLODLand);
	bool					OrthoSkipBlendOnly = false;	// UNOFFICIAL Skylighting v2 clean ortho map: leave out alpha-blended-only geometry (FX meshes, glass)
	void					RenderAccums();	// draws and empties every pass's list
	void					RenderShadowMap(ShadowsExteriorEffect::ShadowMapSettings* Map, D3DXMATRIX* LightViewProj);
	// UNOFFICIAL 2026-10-21 (sunshadow): RenderShadowMap in its three parts -- the state (matrices, render states, the land drop),
	// the walk (LOD roots and loaded cells into the passes' lists) and the draws (viewport, clear, the lists). keepLandSet: the sun
	// cascades' one walk filled RenderPass::LandSet for all of them, so the parts leave it alone.
	void					BeginShadowMap(ShadowsExteriorEffect::ShadowMapSettings* ShadowMap, D3DXMATRIX* ViewProj, bool keepLandSet = false);
	void					WalkShadowMap(ShadowsExteriorEffect::ShadowMapSettings* ShadowMap);
	void					DrawShadowMap(ShadowsExteriorEffect::ShadowMapSettings* ShadowMap, bool keepLandSet = false);
	// UNOFFICIAL 2026-10-21 (sunshadow) SunCascadeOneWalk: one walk of the LOD roots and loaded cells for every sun cascade drawn in a
	// frame, each object tested against each cascade (bit c of a mask = cascade c), each cascade's casters kept in the order its own
	// walk would have found them, then handed to the passes cascade by cascade (PushSunCasters).
	// kSunDisplaceLand (UNOFFICIAL 2026-10-27, core/DisplaceShadows.h): cell land drawn with its tessellation bumps
	enum SunPass : UInt8 { kSunOpaque, kSunLodLand, kSunAlpha, kSunSkinned, kSunTree, kSunDisplaceLand };
	struct SunCaster { NiGeometry* geometry; UInt8 pass; };
	std::vector<SunCaster>	SunCasters[4];
	std::vector<std::pair<NiAVObject*, unsigned>> SunContainers;
	ShadowsExteriorEffect::FormsStruct* SunForms[4] = {};
	NiFrustumPlanes*		SunPlanes[4] = {};
	void					WalkSunCascades(unsigned mask);
	void					WalkSunCell(TESObjectCELL* Cell, unsigned mask);
	void					WalkSunChildren(NiAVObject* Object, unsigned mask, bool isLand, bool isLOD);
	void					WalkSunObject(NiAVObject* Object, unsigned mask, bool isLODLand);
	RenderPass*				SunPassOf(UInt8 pass);
	void					PushSunCasters(int cascade);
	void					AccumExteriorCell(TESObjectCELL*, ShadowsExteriorEffect::ShadowMapSettings* Map);
	// UNOFFICIAL RedrawActorsOnly (PointShadowSchedule.h): which casters RenderShadowCubeMap draws, into which cube map.
	// All: every caster into the live cube map (as before). StaticOnly: those that never move, into the slot's static cube
	// map. MovingOnto: the static cube map copied into the live one, then only the moving casters, min-blended on top.
	enum class CubeLayer { All, StaticOnly, MovingOnto };
	bool					RenderShadowCubeMap(ShadowSceneLight** Lights, UInt32 LightIndex, CubeLayer Layer = CubeLayer::All);	// false: MovingOnto's copy failed, nothing drawn
	void					RenderShadowSpotlight(NiSpotLight** Spots, UInt32 Index);
	void					RenderShadowMaps();	// every map of the frame (the hook on the game's shadow render calls it)
	// UNOFFICIAL 2026-10-23 CharacterShadowMap (docs/next/CHARACTER-SHADOW-MAP-DESIGN.md): the player's own small sun shadow map, drawn
	// after the sun cascades from the same sun direction; actors only (the player, optionally other actors inside it). Sets
	// Constants.CharacterData.w = 1 when the map holds this frame's draw.
	void					RenderCharacterShadowMap(const D3DXVECTOR3& SunDir);
	bool					RenderViewModelShadowMap(NiCamera* FirstPersonCamera);	// UNOFFICIAL 2026-11-05 (Balebandro) ViewModelShadows: the first-person arms and weapon into the character map
	static void				ViewModelLogDraw(NiGeometry* Geometry);	// CharacterShadowMapLog: a first-person draw against this frame's viewmodel map
	void					ClearShadowCascade(D3DVIEWPORT9* Rect, D3DXVECTOR4* Color);	// fills a map's rectangle with a colour (ShadowMapClear.pso)
	void                    BlurShadowAtlas(unsigned cascadeMask);

	// --- The passes, one per kind of caster ---
	ShadowRenderPass*				geometryPass = nullptr;		// opaque
	AlphaShadowRenderPass*			alphaPass = nullptr;		// alpha tested
	SkinnedGeoShadowRenderPass*		skinnedGeoPass = nullptr;	// skinned (people, creatures)
	SpeedTreeShadowRenderPass*		speedTreePass = nullptr;	// SpeedTree branches and leaves
	TerrainLODPass*					terrainLODPass = nullptr;	// distant land

	// Leaf billboard axes facing the sun (RecalculateBillboardVectors; vertex constants c63 / c64 of the sun maps).
	NiVector4				BillboardRight;
	NiVector4				BillboardUp;

	// --- Shaders (Shadows\ folder), and the pair the passes currently use ---
	ShaderRecordVertex*		ShadowMapVertex = nullptr;
	ShaderRecordPixel*		ShadowMapPixel = nullptr;
	ShaderRecordVertex*		ShadowCubeMapVertex = nullptr;
	ShaderRecordPixel*		ShadowCubeMapPixel = nullptr;
	// UNOFFICIAL: ShadowMap.pso / ShadowCubeMap.pso without the alpha-test discard, for opaque casters (early-Z).
	ShaderRecordPixel*		ShadowMapOpaquePixel;
	ShaderRecordPixel*		ShadowGridColorPixel = nullptr;	// UNOFFICIAL 2026-10-14: light grid sun map, depth + colour (optional)
	ShaderRecordPixel*		ShadowCubeMapOpaquePixel;
	ShaderRecordVertex*		ShadowMapBlurVertex = nullptr;
	ShaderRecordPixel*		ShadowMapBlurPixel = nullptr;
	ShaderRecordPixel*		ShadowMapClearPixel = nullptr;
	D3DVIEWPORT9			ShadowCubeMapViewPort = {};
	ShaderRecordVertex*		CurrentVertex = nullptr;
	ShaderRecordPixel*		CurrentPixel = nullptr;
	bool					AlphaEnabled = false;
	// Reused by AccumChildren. That runs per reference, per cell, per cascade, and a fresh
	// std::stack allocated a deque block on every call; clear() keeps the capacity.
	std::vector<NiAVObject*>	containersScratch;
	// UNOFFICIAL: the cube map form-based fallback's references in range of the light, collected once per light.
	std::vector<NiNode*>		fallbackRefsScratch;
	// UNOFFICIAL: RenderShadowCubeMap's casters, sorted once per light for all six faces (kept to avoid an allocation).
	struct CubeCaster { NiGeometry* geometry; RenderPass* pass; unsigned char faces; };
	std::vector<CubeCaster>		cubeCasters;
	int						PointLightsNum = 0;
	float					shadowMapsRenderTime = 0.0f;	// milliseconds, the last RenderShadowMaps
	bool					ShadowShadersLoaded = false;
	int						FrameCounter = 0;
	bool					ForceAllCascades = true; // next frame refreshes every cascade (fresh or recreated atlas)
	bool					SunCascadesSkipped = false; // UNOFFICIAL 2026-10-16 SkipWhenFaded: the cascades were not drawn last frame
	bool					SunAtlasDrawn = false; // UNOFFICIAL 2026-10-16 SkipWhenFaded: the atlas holds a full draw since it was (re)made
	// ReducedQuality CachedDistantShadows: characters near the player this frame (bound centre, radius), and per sun
	// cascade where the camera was at its last redraw and whether a character was inside it then.
	std::vector<D3DXVECTOR4>	FrameMovers;
	D3DXVECTOR3				CascadeRefreshCamera[4] = {};
	bool					CascadeHadMover[4] = {};
	// UNOFFICIAL 2026-10-31 SunCacheByChange (root plan A1): per cascade at its last redraw, the camera's forward, the sun and the
	// frames since; a cached cascade is redrawn when one of them (or a character in it) says the kept map is no longer right.
	D3DXVECTOR3				CascadeRefreshForward[4] = {};
	D3DXVECTOR3				CascadeRefreshSun[4] = {};
	unsigned				CascadeAge[4] = {};
	unsigned				SunCacheReasons[4][5] = {};	// per cascade: age, sun, moved, character, kept
	float					MoverShadowStretch = 0.0f;
	// UNOFFICIAL optional contact-hardening sun shadows (Shaders/Includes/Shadow.hlsl, SampleShadowAtlasContact; compiled
	// in only when on at startup). [Shaders.ContactHardening.Main/.Status]. x: penumbra width per world unit between
	// caster and receiver (0 = off), y: widest penumbra radius in atlas texels.
	D3DXVECTOR4				ContactHardeningData = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);
	bool					ContactHardeningCompiled = false;
	// F10 statistics for CachedDistantShadows, logged every 240 frames by LogSunShadowStats.
	struct {
		unsigned Frames, Scheduled[4], ForCharacters[4], Characters;
	}						SunStats = {};
	void					LogSunShadowStats(bool cachedDistant);
	void					CollectMovers(const D3DXVECTOR3& SunDir);
	bool					MoversInCascade(int cascade, ShadowsExteriorEffect::ShadowMapSettings* ShadowMap, float InnerDepth);

private:
	bool					CheckShaderFlags(NiGeometry*);	// false: no shader property, or one that casts no shadow (refraction, decals)
	void					RecalculateBillboardVectors(D3DXVECTOR3* SunDir);
};
