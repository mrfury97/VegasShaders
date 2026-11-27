#pragma once
#include <unordered_map>
#include <unordered_set>

// One kind of shadow-caster draw: geometry is collected during the scene walk (AccumObject) and drawn later in one go
// (RenderAccum) with the pass's own vertex/pixel shader pair.
class RenderPass {
public:
	RenderPass() {}
	virtual ~RenderPass() { VertexShader = nullptr; PixelShader = nullptr; }

	ShaderRecordVertex*	VertexShader;	// the pass's shaders (set by ShadowManager)
	ShaderRecordPixel*	PixelShader;

	// UNOFFICIAL 2026-10-13: draw every caster double-sided ([Shaders.ShadowsExteriors.Main] TwoSidedCasters, sun shadow maps
	// only; ShadowManager::RenderShadowMap sets it round its draws). Each object's own cull mode otherwise skips one-sided walls
	// seen from behind (the game uses them where the player never sees their other side), so the sun and the volumetric light
	// shone straight through them. Closed objects come out the same either way: the face nearest the sun wins the depth test.
	static inline bool TwoSided = false;
	// UNOFFICIAL 2026-10-14: while the light grid's sun map is drawn (ShadowManager, ShadowGridColor.pso), each caster's diffuse
	// texture is bound at s0 so the map records its colour; TESR_ShadowData.w says whether one was bound.
	static inline bool BindDiffuse = false;
	// UNOFFICIAL 2026-11-24: while the reflection world cubemap is drawn (ShadowManager::RenderReflectionCubeFace, ReflCube.pso),
	// each non-Hair/non-FaceGen caster's glow map (ppTextures[2]) is bound at s1; TESR_ShadowData.z says whether one was bound.
	static inline bool BindGlow = false;
	// UNOFFICIAL 2026-10-17 (TB-1, terrain blending shadow band): while Terrain Blending is on, the land is drawn into the sun
	// shadow maps LandDrop game units lower than it is (ShadowManager::RenderShadowMap sets it to BlendDistance * ShadowDrop;
	// 0 = off, nothing changes). The land fades away over the part of an object it covers by less than BlendDistance; with the
	// land at its true height that part sits in the land's own shadow, so the fade revealed a darkened strip. A plane lowered
	// by d moves its sun-map depth by d / sin(sun elevation), exactly what a point buried by d is behind the surface. The
	// shader is untouched: the offset is in the caster's world matrix (UpdateShadowWorld).
	static inline float LandDrop = 0.0f;
	// UNOFFICIAL 2026-10-21 (sunshadow) SunCascadeLeanDraws: set by ShadowManager around a sun cascade's draws. Each caster after a
	// pass's first uploads only the shader constants whose values changed (SetCTLean), and the cloth / plant wind calls are left
	// out for a caster they would leave as it is (WindLeavesStill). The same draws with the same values: see RenderPass.cpp.
	static inline bool Lean = false;
	static inline std::unordered_set<NiGeometry*> LandSet;	// this map's land geometry (filled while its cells' land is accumulated)
	// UNOFFICIAL 2026-10-31 SunLayers ([Main.Main.ReducedQuality], ShadowManager.cpp): set by ShadowManager round a middle / far sun
	// cascade's static layer (1) or its per-frame overlay (2), 0 elsewhere. In 1 a caster that moves by itself (skinned, a SpeedTree when
	// LayerTreePass is set, cloth or plant wind by its texture) is not drawn: its reference (LayerGeoRef, noted while accumulated) goes
	// into *LayerMovingRefs, which the overlay then draws every frame.
	static inline int LayerMode = 0;
	// UNOFFICIAL 2026-10-31 BackDepth (BackDepth.h): every caster's cull mode turned round, so only its back faces draw
	static inline bool FlipCull = false;
	static inline bool FirstPerson = false;	// UNOFFICIAL 2026-11-21: true during RenderViewModelShadowMap (skip cloth/plant displacement)
	static inline RenderPass* LayerTreePass = nullptr;
	static inline std::unordered_map<NiGeometry*, const void*> LayerGeoRef;
	static inline std::unordered_set<const void*>* LayerMovingRefs = nullptr;
	static inline unsigned LayerHeldBack = 0;	// casters left out of static layers (stats)
	static void UpdateShadowWorld(NiGeometry* Geo);
	static void BindDiffuseOf(NiGeometry* Geo);
	static void BindGlowOf(NiGeometry* Geo);

	std::stack<NiGeometry*>	GeometryList;	// collected casters, drawn and emptied by RenderAccum

	// per-pass hooks: collect a geometry (true when this pass takes it), set its shader constants, draw it, name its constants
	virtual bool AccumObject(NiGeometry* geometry) { return true; }
	// UNOFFICIAL: the test behind AccumObject without the push, so a caller can sort geometry once and push it several
	// times (ShadowManager::RenderShadowCubeMap, one walk for six faces): 0 = not this pass, 1 = this pass draws it,
	// 2 = this pass claims it but has nothing to draw (a skinned geometry with no renderable partition).
	virtual int TestObject(NiGeometry* Geo) { return 1; };
	virtual void UpdateConstants(NiGeometry* geometry) {}
	virtual void RenderGeometry(NiGeometry* geometry);
	virtual void RegisterConstants() {}

	// the game's own buffers of a geometry (plain or one skin partition) drawn with whatever shaders are bound
	void DrawGeometryBuffer(NiGeometry* geometry, NiGeometryBufferData* buffers);
	void DrawSkinnedGeometryBuffer(NiGeometry* geometry, NiGeometryBufferData* buffers, NiSkinPartition::Partition* part);
	void RenderAccum();
};


// Opaque static casters: depth only.
class ShadowRenderPass : public RenderPass {
public:
	ShadowRenderPass();

	struct ConstantsStruct {};
	ConstantsStruct Constants;

	bool AccumObject(NiGeometry* geometry);
	int TestObject(NiGeometry* geometry);
	void RegisterConstants();
	void UpdateConstants(NiGeometry* geometry);
};

// Alpha-tested casters: the diffuse texture's alpha cuts the shadow.
class AlphaShadowRenderPass : public RenderPass {
public:
	AlphaShadowRenderPass();

	struct ConstantsStruct { IDirect3DBaseTexture9* DiffuseMap; };
	ConstantsStruct Constants;

	bool AccumObject(NiGeometry* geometry);
	int TestObject(NiGeometry* geometry);
	void RegisterConstants();
	void UpdateConstants(NiGeometry* geometry);
};


// Skinned casters (actors): drawn partition by partition with their bone matrices.
class SkinnedGeoShadowRenderPass : public RenderPass {
public:
	SkinnedGeoShadowRenderPass();

	struct ConstantsStruct { D3DXVECTOR4* BoneMatrices; };
	ConstantsStruct Constants;

	bool AccumObject(NiGeometry* geometry);
	int TestObject(NiGeometry* geometry);
	void RegisterConstants();
	void UpdateConstants(NiGeometry* geometry);
	void RenderGeometry(NiGeometry* geometry);

	// UNOFFICIAL 2026-11-02 SkinnedAlphaTest: skinned casters with an alpha property (hair, fringes, ragged cloth) get the alpha
	// pass's cut-out, so only their opaque texels cast. Set by the shadow manager per map kind, with the clip pixel shader.
	static bool AlphaTest;
};


// SpeedTree branches and leaves: billboard axes and leaf corners from the game's tree shader.
class SpeedTreeShadowRenderPass : public RenderPass {
public:
	SpeedTreeShadowRenderPass();

	struct ConstantsStruct {
		D3DXVECTOR4		*BillboardRight, *BillboardUp, *LeafBase;
		IDirect3DBaseTexture9*	DiffuseMap;
	};
	ConstantsStruct Constants;

	bool AccumObject(NiGeometry* geometry);
	int TestObject(NiGeometry* geometry);
	void RegisterConstants();
	void UpdateConstants(NiGeometry* geometry);
};


// Distant land (LOD): morphs like the game's LOD land shader.
class TerrainLODPass : public RenderPass {
public:
	TerrainLODPass();

	struct ConstantsStruct {
		D3DXMATRIX		WorldTranspose;
		D3DXVECTOR4		LODLandParams, HighDetailRange;
	};
	ConstantsStruct Constants;

	bool AccumObject(NiGeometry* geometry);
	int TestObject(NiGeometry* geometry);
	void UpdateConstants(NiGeometry* geometry);
};
