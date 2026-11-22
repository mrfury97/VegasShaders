#include "RenderPass.h"
#include "ClothWind.h"
#include "PlantWind.h"	// UNOFFICIAL 2026-10-20 plant wind: a static plant's shadow moves with it
#include "ShadowBoneUpload.h"
#include "OrthoProbe.h"	// UNOFFICIAL 2026-10-17 test tools: ortho map caster dump
#include "SunShadowStats.h"	// UNOFFICIAL 2026-10-21 test tools: where the sun cascades' CPU time goes

// Sets the diffuse texture of a shadow caster at s0 for the light grid's colour pass. Data.w tells the shader whether one was bound.
void RenderPass::BindDiffuseOf(NiGeometry* caster) {
	ShadowsExteriorEffect::ShadowStruct& shadow = TheShaderManager->Effects.ShadowsExteriors->Constants;
	shadow.Data.w = 0.0f;
	if (!BindDiffuse) return;
	BSShaderProperty* shade = (BSShaderProperty*)caster->GetProperty(NiProperty::PropertyType::kType_Shade);
	if (!shade || !shade->IsLightingProperty()) return;
	NiSourceTexture** slots = ((BSShaderPPLightingProperty*)shade)->ppTextures[0];
	NiTexture* tex = slots ? *slots : nullptr;
	if (!tex || !tex->rendererData || !tex->rendererData->dTexture) return;
	NiDX9RenderState* state = TheRenderManager->renderState;
	state->SetTexture(0, tex->rendererData->dTexture);
	state->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP, false);
	state->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP, false);
	state->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR, false);
	state->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR, false);
	state->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR, false);
	shadow.Data.w = 1.0f;
}


// The caster's world matrix for the sun map (camera relative, row vectors: _43 is the height). Land is lowered by LandDrop.
// The caster's own world matrix, camera relative, into the shadow passes' world constant.
static void LoadCasterWorld(NiGeometry* caster) {
	D3DXMATRIX& world = TheShaderManager->ShaderConst.ShadowWorld;
	TheRenderManager->CreateD3DMatrix(&world, &caster->m_worldTransform);
}

void RenderPass::UpdateShadowWorld(NiGeometry* caster) {
	LoadCasterWorld(caster);
	if (LandDrop > 0.0f && LandSet.count(caster)) TheShaderManager->ShaderConst.ShadowWorld._43 -= LandDrop;
}


// UNOFFICIAL 2026-10-21 (sunshadow) SunCascadeLeanDraws, the shader constants. Measured (evidence\sunshadow\LOG.md): both SetCT
// were ~15% of a sun-shadow caster's CPU time; every caster uploaded every constant of both shaders again (world matrix, the
// cascade's view-projection, the shadow data, the plant wind's leaf constants, the camera position), though only the world
// matrix and the shadow data change between casters. SetCTLean uploads a constant only when its value differs from what this
// pass last uploaded for it; the first caster of each pass does the full SetCT. Exact: device constants keep their values
// between draws, and while a pass draws nothing else writes the registers of these TESR_ constants (vertex c0-c8, c131-c134;
// pixel c0-c1): bones go to c9 up, SpeedTree to c63-c130, terrain LOD to c140-c145, cloth and plant wind to c200 up, and the
// game's draw calls (OnlyRender, DrawPartition) only draw. Records that bind textures or buffers in SetCT are not trimmed.
struct ConstantRun { const ShaderRecord* record = nullptr; std::vector<D3DXVECTOR4> sent; bool valid = false; };
static ConstantRun VertexRun, PixelRun;

static void SetCTLean(ShaderRecord* record, ConstantRun& run) {
	if (run.valid && run.record == record) {
		size_t k = 0;
		for (UInt32 c = 0; c < record->FloatShaderValuesCount; c++) {
			ShaderFloatValue& constant = record->FloatShaderValues[c];
			const size_t bytes = constant.RegisterCount * sizeof(D3DXVECTOR4);
			if (memcmp(constant.Value, &run.sent[k], bytes)) {
				record->SetShaderConstantF(constant.RegisterIndex, constant.Value, constant.RegisterCount);
				memcpy(&run.sent[k], constant.Value, bytes);
			}
			k += constant.RegisterCount;
		}
		return;
	}
	record->SetCT();
	run.record = record;
	run.valid = !record->TextureShaderValuesCount && !record->HasRenderedBuffer && !record->HasDepthBuffer && !record->HasTerrainBlendDepth && !record->HasDisplaceDepth && !record->ClearSamplers;
	size_t registers = 0;
	for (UInt32 c = 0; c < record->FloatShaderValuesCount; c++) {
		if (!record->FloatShaderValues[c].Value) run.valid = false;	// (SetCT could not find it: it keeps trying, so do we)
		registers += record->FloatShaderValues[c].RegisterCount;
	}
	if (!run.valid) return;
	run.sent.resize(registers);
	size_t k = 0;
	for (UInt32 c = 0; c < record->FloatShaderValuesCount; c++) {
		const ShaderFloatValue& constant = record->FloatShaderValues[c];
		memcpy(&run.sent[k], constant.Value, constant.RegisterCount * sizeof(D3DXVECTOR4));
		k += constant.RegisterCount;
	}
}

// UNOFFICIAL 2026-10-21 (sunshadow) SunCascadeLeanDraws, the wind calls. ClothWind::ForGeometry and PlantWind::ForGeometry were
// ~20% of a caster's CPU time (property walks, hash lookups and path compares per caster, twice). In a shadow pass both leave a
// caster exactly as it is -- ClothWind::Zero with nothing to clean, PlantWind returning -- when the caster is not skinned (the
// cloth AutoDetect and worn-cloth paths are for skinned meshes) and either has no lighting property or diffuse texture, or its
// diffuse texture is neither cloth (ClothWind::ByTexture) nor plant (PlantWind::ByTexture): both decide per texture, by path.
// Those verdicts are learnt from their own caches right after a normal call, remembered here per texture object and path (the
// path compared by content, as theirs), and forgotten at every settings load (SkySettingsGeneration: their keyword lists can
// change only then, and they clear their caches when they do). Nothing to clean must be pending (the previous caster's AfterDraw
// leaves it so; else the normal calls run).
struct StillTexture { const NiSourceTexture* texture = nullptr; std::string path; bool still = false; };
static StillTexture StillTextures[4096];
static int StillGeneration = -1;

static inline size_t StillSlot(const void* texture) { return ((uintptr_t)texture >> 4) * 2654435761u >> 20 & 4095; }

static const NiSourceTexture* WindTexture(NiGeometry* caster, bool& noTexture) {
	noTexture = false;
	BSShaderProperty* property = (BSShaderProperty*)caster->GetProperty(NiProperty::PropertyType::kType_Shade);
	const UInt32 vtable = property ? *(UInt32*)property : 0;
	if (vtable != 0x10AE0D0 && vtable != 0x10B9910) { noTexture = true; return nullptr; }	// BSShaderPPLightingProperty, Lighting30ShaderProperty
	NiSourceTexture** textures = ((BSShaderPPLightingProperty*)property)->ppTextures[0];
	const NiSourceTexture* diffuse = textures ? *textures : nullptr;
	if (!diffuse || !diffuse->ddsPath1) { noTexture = true; return nullptr; }
	return diffuse;
}

static bool WindLeavesStill(NiGeometry* caster) {
	if (caster->skinInstance || ClothWind::NonZeroSent || ClothWind::ActiveWeights || ClothWind::ActiveWalk || ClothWind::ActiveSkin || ClothWind::ActiveStill)
		return false;
	const int generation = TheShaderManager->Effects.ShadowsExteriors->Settings.OrthoMap.SkySettingsGeneration;
	if (generation != StillGeneration) {
		for (StillTexture& entry : StillTextures) { entry.texture = nullptr; entry.path.clear(); }
		StillGeneration = generation;
	}
	bool noTexture;
	const NiSourceTexture* diffuse = WindTexture(caster, noTexture);
	if (noTexture) return true;
	const StillTexture& entry = StillTextures[StillSlot(diffuse)];
	return entry.texture == diffuse && entry.still && entry.path == diffuse->ddsPath1;
}

// After the normal calls for a caster WindLeavesStill did not clear: what their caches now say about its texture.
static void LearnWindVerdict(NiGeometry* caster) {
	if (caster->skinInstance) return;
	bool noTexture;
	const NiSourceTexture* diffuse = WindTexture(caster, noTexture);
	if (!diffuse) return;
	auto cloth = ClothWind::ByTexture.find(diffuse);
	auto plant = PlantWind::ByTexture.find(diffuse);
	if (cloth == ClothWind::ByTexture.end() || plant == PlantWind::ByTexture.end()) return;	// a call returned before its verdict
	if (cloth->second.first != diffuse->ddsPath1 || plant->second.first != diffuse->ddsPath1) return;
	StillTexture& entry = StillTextures[StillSlot(diffuse)];
	entry.texture = diffuse;
	entry.path = diffuse->ddsPath1;
	entry.still = !cloth->second.second && !plant->second.second;
}

// UNOFFICIAL 2026-10-31 SunLayers: true when the cloth or plant wind moves this caster's texture (their own verdicts, filled by
// ForGeometry; an unknown texture counts as still).
static bool WindMoves(NiGeometry* caster) {
	bool noTexture;
	const NiSourceTexture* diffuse = WindTexture(caster, noTexture);
	if (!diffuse) return false;
	auto cloth = ClothWind::ByTexture.find(diffuse);
	if (cloth != ClothWind::ByTexture.end() && cloth->second.second && cloth->second.first == diffuse->ddsPath1) return true;
	auto plant = PlantWind::ByTexture.find(diffuse);
	return plant != PlantWind::ByTexture.end() && plant->second.second && plant->second.first == diffuse->ddsPath1;
}

void RenderPass::RenderAccum() {
	if (GeometryList.size() == 0) return;	// nothing queued: not even the shaders are set

	// Shaders for the whole pass are set once, here.
	NiDX9RenderState* state = TheRenderManager->renderState;
	state->SetPixelShader(PixelShader->ShaderHandle, false);
	state->SetVertexShader(VertexShader->ShaderHandle, false);
	const bool lean = Lean;
	VertexRun.valid = PixelRun.valid = false;	// a new pass: its first caster does the full SetCT

	for (;;) {
		if (GeometryList.size() == 0) break;
		// UNOFFICIAL (Fable): taken off the list before the null test (a null entry left on it never ended the loop)
		NiGeometry* const caster = GeometryList.top();
		GeometryList.pop();
		if (caster == nullptr) continue;
#if NVR_TEST_TOOLS
		SunShadowStats::Cascade* stats = SunShadowStats::Current;	// UNOFFICIAL 2026-10-21: null unless a sun cascade is measured
		unsigned long long t0 = 0, t1 = 0, t2 = 0, t3 = 0;
		if (stats) { SunShadowStats::NoteCaster(caster); t0 = SunShadowStats::Now(); }
#endif

		UpdateConstants(caster);
#if NVR_TEST_TOOLS
		if (stats) t1 = SunShadowStats::Now();
#endif
		if (lean) { SetCTLean(PixelShader, PixelRun); SetCTLean(VertexShader, VertexRun); }
		else { PixelShader->SetCT(); VertexShader->SetCT(); }	// every constant of both shaders
#if NVR_TEST_TOOLS
		if (stats) t2 = SunShadowStats::Now();
#endif
		if (!lean || !WindLeavesStill(caster)) {
			ClothWind::ForGeometry(TheRenderManager->device, caster, true, FirstPerson);	// UNOFFICIAL cloth wind: the cloth's shadow moves with it
			PlantWind::ForGeometry(TheRenderManager->device, caster, true, FirstPerson);	// UNOFFICIAL 2026-10-20 plant wind: so does a static plant's
			if (lean) LearnWindVerdict(caster);
		}
		if (LayerMode == 1 && (caster->skinInstance || this == LayerTreePass || WindMoves(caster))) {	// UNOFFICIAL 2026-10-31 SunLayers
			auto owner = LayerGeoRef.find(caster);
			if (owner != LayerGeoRef.end() && owner->second) {	// (land and LOD have no reference: drawn, as still)
				if (LayerMovingRefs) LayerMovingRefs->insert(owner->second);
				LayerHeldBack++;
				ClothWind::AfterDraw(TheRenderManager->device);
				continue;
			}
		}
#if NVR_TEST_TOOLS
		if (OrthoProbe::Recording) OrthoProbe::OnDraw(this, caster);	// UNOFFICIAL 2026-10-17: only while an ortho dump is recorded
		if (stats) t3 = SunShadowStats::Now();
#endif

		RenderGeometry(caster);
		ClothWind::AfterDraw(TheRenderManager->device);	// UNOFFICIAL 2026-10-19: no cloth values left behind for other draws
#if NVR_TEST_TOOLS
		if (stats) {
			const unsigned long long t4 = SunShadowStats::Now();
			stats->tUpdate += t1 - t0; stats->tSetCT += t2 - t1; stats->tWind += t3 - t2; stats->tRender += t4 - t3;
		}
#endif
	}
}


// Cull mode for a caster: the TwoSided and FlipCull switches (UNOFFICIAL) apply on top of the caster's own stencil property.
static void ApplyCasterCull(NiGeometry* caster) {
	NiProperty* stencil = caster->GetProperty(NiProperty::PropertyType::kType_Stencil);

	NiDX9RenderState* state = TheRenderManager->renderState;
	if (RenderPass::TwoSided) {
		state->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE, RenderStateArgs);	// UNOFFICIAL TwoSidedCasters
	}
	else if (stencil) {
		NiStencilProperty* stencilProp = static_cast<NiStencilProperty*>(stencil);
		UInt32 mode = stencilProp->GetDrawMode();
		if (RenderPass::FlipCull) mode = mode == 3 ? 3 : mode == 2 ? 1 : 2;	// UNOFFICIAL BackDepth: both stays, one side turns round
		state->SetCullMode((NiStencilProperty::DrawMode)mode);
	}
	else {
		state->SetRenderState(D3DRS_CULLMODE, RenderPass::FlipCull ? D3DCULL_CW : D3DCULL_CCW, RenderStateArgs);
	}
}

// Stream sources, index buffer and vertex format of a buffer, shared by the plain and the skinned draws.
static void BindBufferStreams(NiGeometryBufferData* buffers) {
	NiDX9RenderState* state = TheRenderManager->renderState;
	for (UInt32 i = 0; i < buffers->StreamCount; i++)
		TheRenderManager->device->SetStreamSource(i, buffers->VBChip[i]->VB, 0, buffers->VertexStride[i]);
	if (buffers->IB)
		TheRenderManager->device->SetIndices(buffers->IB);
	if (buffers->FVF)
		state->SetFVF(buffers->FVF, false);
	else
		state->SetVertexDeclaration(buffers->VertexDeclaration, false);
}


void RenderPass::RenderGeometry(NiGeometry* caster) {
	NiGeometryData* data = caster->geomData;
	NiGeometryBufferData* buffer = data->m_pkBuffData;
	NiD3DShaderDeclaration* declaration = caster->shader->ShaderDeclaration;

	ApplyCasterCull(caster);	// cull from the stencil property

	TheRenderManager->PackGeometryBuffer(buffer, data, NULL, declaration);
	if (buffer && buffer->VertCount) DrawGeometryBuffer(caster, buffer);
}


// draws the geo data from the Geometry buffer
void RenderPass::DrawGeometryBuffer(NiGeometry* caster, NiGeometryBufferData* buffers) {
	BindBufferStreams(buffers);

	uint16_t dirtyFlags = caster->geomData->m_usDirtyFlags;
	NiTriStrips* strips = caster->IsTriStrips();
	// RenderTriStripsAlt (0xE74840) / RenderTriShapeAlt (0xE745A0), through ClothWind::OnlyRender (UNOFFICIAL 2026-10-20: with still
	// cloth's per-vertex weights in stream 1 when ForGeometry chose them; else exactly these calls)
	ClothWind::OnlyRender(strips != nullptr, NiDX9Renderer::GetSingleton(), caster);
	// Add back the dirty flag that are reset with vanilla render functions.
	caster->geomData->m_usDirtyFlags = dirtyFlags;
}

void RenderPass::DrawSkinnedGeometryBuffer(NiGeometry* caster, NiGeometryBufferData* buffers, NiSkinPartition::Partition* slice) {
	BindBufferStreams(buffers);

#if NVR_TEST_TOOLS
	if (SunShadowStats::Current) SunShadowStats::Current->partitions++;
#endif
	ClothWind::DrawPartition(NiDX9Renderer::GetSingleton(), buffers, slice, nullptr);  // DrawSkinnedGeometry (0xE6D310; UNOFFICIAL 2026-10-19: with worn cloth's per-vertex weights in stream 1 when ForGeometry chose them)
}


// Every shadow pass draws the same shadow-map shader pair; then the pass names its constants (called from the constructor, so
// the class being built answers the virtual call).
static void SetUpShadowPass(RenderPass& pass) {
	pass.PixelShader = TheShadowManager->ShadowMapPixel;
	pass.VertexShader = TheShadowManager->ShadowMapVertex;
	pass.RegisterConstants();
}

// Passes that queue geometry: the test result 1 queues it, 0 and 2 do not (2 is claimed, with nothing to draw).
static bool QueueIfDrawn(RenderPass& pass, NiGeometry* caster, int verdict) {
	if (verdict == 1) pass.GeometryList.push(caster);
	return verdict != 0;
}


ShadowRenderPass::ShadowRenderPass() { SetUpShadowPass(*this); }


int ShadowRenderPass::TestObject(NiGeometry* caster) {
	if (!caster->geomData || !caster->geomData->m_pkBuffData) return 0; // discard objects without buffer data

	BSShaderProperty* shade = (BSShaderProperty*)caster->GetProperty(NiProperty::PropertyType::kType_Shade);
	if (!shade || !shade->IsLightingProperty()) return 0;
	return 1;
}

bool ShadowRenderPass::AccumObject(NiGeometry* caster) {
	return QueueIfDrawn(*this, caster, TestObject(caster));
}


void ShadowRenderPass::RegisterConstants() {
	D3DXMATRIX* world = &TheShaderManager->ShaderConst.ShadowWorld;
	TheShaderManager->RegisterConstant("TESR_ShadowWorldTransform", reinterpret_cast<D3DXVECTOR4*>(world));
	//TheTextureManager->RegisterTexture("TESR_DiffuseMap", &Constants.DiffuseMap);
}


void ShadowRenderPass::UpdateConstants(NiGeometry* caster) {
	ShadowsExteriorEffect::ShadowStruct& shadow = TheShaderManager->Effects.ShadowsExteriors->Constants;
	shadow.Data.x = 0.0f; // Type of geo (0 normal, 1 actors (skinned), 2 speedtree leaves)
	shadow.Data.y = 0.0f; // Alpha Control
	UpdateShadowWorld(caster);
	BindDiffuseOf(caster);	// UNOFFICIAL light grid colour (nothing unless RenderPass::BindDiffuse)
}



AlphaShadowRenderPass::AlphaShadowRenderPass() { SetUpShadowPass(*this); }


int AlphaShadowRenderPass::TestObject(NiGeometry* caster) {
	if (!caster->geomData || !caster->geomData->m_pkBuffData) return 0; // discard objects without buffer data

	BSShaderProperty* shade = (BSShaderProperty*)caster->GetProperty(NiProperty::PropertyType::kType_Shade);
	NiAlphaProperty* alpha = (NiAlphaProperty*)caster->GetProperty(NiProperty::PropertyType::kType_Alpha);

	if (!shade || !shade->IsLightingProperty()) return 0;
	if (!alpha) return 0;
	if (!(alpha->flags & NiAlphaProperty::AlphaFlags::ALPHA_BLEND_MASK) && !(alpha->flags & NiAlphaProperty::AlphaFlags::TEST_ENABLE_MASK)) return 0;
	return 1;
}

bool AlphaShadowRenderPass::AccumObject(NiGeometry* caster) {
	return QueueIfDrawn(*this, caster, TestObject(caster));
}


void AlphaShadowRenderPass::RegisterConstants() {
}


void AlphaShadowRenderPass::UpdateConstants(NiGeometry* caster) {
	ShadowsExteriorEffect::ShadowStruct& shadow = TheShaderManager->Effects.ShadowsExteriors->Constants;
	shadow.Data.x = 0.0f; // Type of geo (0 normal, 1 actors (skinned), 2 speedtree leaves)
	shadow.Data.y = 0.0f; // Alpha Control
	UpdateShadowWorld(caster);

	BSShaderProperty* shade = (BSShaderProperty*)caster->GetProperty(NiProperty::PropertyType::kType_Shade);
	NiTexture* tex = *((BSShaderPPLightingProperty*)shade)->ppTextures[0];

	// UNOFFICIAL (Fable): rendererData can be null for a texture the renderer has not created yet.
	if (tex && tex->rendererData && tex->rendererData->dTexture) {

		shadow.Data.y = 1.0f; // Alpha Control
//			Constants.DiffuseMap = Texture->rendererData->dTexture;

		//// Set diffuse texture at register 0
		NiDX9RenderState* state = TheRenderManager->renderState;
		state->SetTexture(0, tex->rendererData->dTexture);
		state->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP, false);
		state->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP, false);
		state->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT, false);
		state->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT, false);
		state->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_POINT, false);
	}
}


SkinnedGeoShadowRenderPass::SkinnedGeoShadowRenderPass() { SetUpShadowPass(*this); }


int SkinnedGeoShadowRenderPass::TestObject(NiGeometry* caster) {
	// check data for rigged geometry
	if (!caster->skinInstance)
		return 0;

	NiSkinInstance* skin = caster->skinInstance;
	if (skin->IsKindOf<BSDismemberSkinInstance>() && !((BSDismemberSkinInstance*)skin)->IsRenderable)
		return 2; // claimed as skinned geometry, nothing to draw

	// only accum if valid data present; claimed in any case because we still found skinned geo either way
	if (caster->skinInstance->SkinPartition && caster->skinInstance->SkinPartition->Partitions)
		return 1;

	return 0;
}

bool SkinnedGeoShadowRenderPass::AccumObject(NiGeometry* caster) {
	return QueueIfDrawn(*this, caster, TestObject(caster));
}


void SkinnedGeoShadowRenderPass::RegisterConstants() {
	//TheShaderManager->RegisterConstant("TESR_Bones", NULL);
}


bool SkinnedGeoShadowRenderPass::AlphaTest = false;

void SkinnedGeoShadowRenderPass::UpdateConstants(NiGeometry* caster) {
	// Render skinned geometry
	ShadowsExteriorEffect::ShadowStruct& shadow = TheShaderManager->Effects.ShadowsExteriors->Constants;
	shadow.Data.x = 1.0f; // Type of geo (0 normal, 1 actors (skinned), 2 speedtree leaves)
	shadow.Data.y = 0.0f; // Alpha control
	LoadCasterWorld(caster);
	BindDiffuseOf(caster);	// UNOFFICIAL light grid colour

	// UNOFFICIAL 2026-11-02 SkinnedAlphaTest (tester report: hair black under interior lamps): a skinned caster always went in whole,
	// so the transparent part of alpha-tested hair cards shadowed the hair itself and whatever was under it. With an alpha
	// property (blended or tested, as AlphaShadowRenderPass takes) its diffuse is bound at s0 and the clip shader cuts it out.
	if (!AlphaTest) return;
	NiAlphaProperty* alpha = (NiAlphaProperty*)caster->GetProperty(NiProperty::PropertyType::kType_Alpha);
	if (!alpha || !(alpha->flags & (NiAlphaProperty::AlphaFlags::ALPHA_BLEND_MASK | NiAlphaProperty::AlphaFlags::TEST_ENABLE_MASK))) return;
	BSShaderProperty* shade = (BSShaderProperty*)caster->GetProperty(NiProperty::PropertyType::kType_Shade);
	if (!shade || !shade->IsLightingProperty()) return;
	NiSourceTexture** slots = ((BSShaderPPLightingProperty*)shade)->ppTextures[0];
	NiTexture* tex = slots ? *slots : nullptr;
	if (!tex || !tex->rendererData || !tex->rendererData->dTexture) return;
	shadow.Data.y = 1.0f; // Alpha control
	NiDX9RenderState* state = TheRenderManager->renderState;
	state->SetTexture(0, tex->rendererData->dTexture);
	state->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP, false);
	state->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP, false);
	state->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT, false);
	state->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT, false);
	state->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_POINT, false);
}


void SkinnedGeoShadowRenderPass::RenderGeometry(NiGeometry* caster) {
	NiGeometryData* data = caster->geomData;
	NiGeometryBufferData* buffer = data->m_pkBuffData;
	NiSkinInstance* skin = caster->skinInstance;
	NiD3DShaderDeclaration* declaration = caster->shader->ShaderDeclaration;
	(void)declaration;	// read as before; skinned draws use the partitions' own buffers

	ApplyCasterCull(caster);	// cull from the stencil property

	NiSkinPartition* partitions = skin->SkinPartition;
	TheRenderManager->CalculateBoneMatrixes(skin, &caster->m_worldTransform);

	if (skin->SkinToWorldWorldToSkin)
		memcpy(&TheShaderManager->Effects.ShadowsExteriors->Constants.ShadowWorld, skin->SkinToWorldWorldToSkin, 0x40);

	DismemberPartition* dismember = NULL;
	if (skin->IsKindOf<BSDismemberSkinInstance>())
		dismember = ((BSDismemberSkinInstance*)skin)->partitions;

	for (UInt32 p = 0; p < partitions->PartitionsCount; p++) {
		if (dismember && !dismember[p].Enabled)
			continue;

		NiSkinPartition::Partition* part = &partitions->Partitions[p];
		if (!part)
			continue;

		buffer = part->BuffData;
		if (buffer) {
			UploadShadowBones(TheRenderManager->device, (const float*)skin->BoneMatrixes, part->pBones, part->Bones);

			//TheRenderManager->PackSkinnedGeometryBuffer(buffers, ModelData, SkinInstance, Partition, ShaderDeclaration);
			DrawSkinnedGeometryBuffer(caster, buffer, part);
		}

		if (!dismember)
			continue;

		for (UInt32 d = p + 1; d < partitions->PartitionsCount; d++) {
			if (dismember[p + 1].StartCap)
				break;

			++p;
			++part;
			if (dismember[p].Enabled) {
				buffer = part->BuffData;
				if (buffer) {
					UploadShadowBones(TheRenderManager->device, (const float*)skin->BoneMatrixes, part->pBones, part->Bones);


					DrawSkinnedGeometryBuffer(caster, buffer, part);
				}
			}

		}
	}
}


SpeedTreeShadowRenderPass::SpeedTreeShadowRenderPass() { SetUpShadowPass(*this); }


void SpeedTreeShadowRenderPass::RegisterConstants() {
	//TheShaderManager->RegisterConstant("TESR_BillboardRight", Constants.BillboardRight);
	//TheShaderManager->RegisterConstant("TESR_BillboardUp", Constants.BillboardUp);
	//TheShaderManager->RegisterConstant("TESR_RockParams", (D3DXVECTOR4*)Pointers::ShaderParams::RockParams);
	//TheShaderManager->RegisterConstant("TESR_RustleParams", (D3DXVECTOR4*)Pointers::ShaderParams::RustleParams);
	//TheShaderManager->RegisterConstant("TESR_WindMatrices", (D3DXVECTOR4*)Pointers::ShaderParams::WindMatrixes);
	//TheShaderManager->RegisterConstant("TESR_LeafBase", Constants.LeafBase);
	//TheTextureManager->RegisterTexture("TESR_LeafDiffuseMap", &Constants.DiffuseMap);
}


int SpeedTreeShadowRenderPass::TestObject(NiGeometry* caster) {
	NiShadeProperty* shadeProp = static_cast<NiShadeProperty*>(caster->GetProperty(NiProperty::kType_Shade));
	if (!shadeProp || shadeProp->m_eShaderType != NiShadeProperty::kProp_SpeedTreeLeaf) return 0;
	return 1;
}

bool SpeedTreeShadowRenderPass::AccumObject(NiGeometry* caster) {
	return QueueIfDrawn(*this, caster, TestObject(caster));
}


void SpeedTreeShadowRenderPass::UpdateConstants(NiGeometry* caster) {

	ShadowsExteriorEffect::ShadowStruct& shadow = TheShaderManager->Effects.ShadowsExteriors->Constants;
	IDirect3DDevice9* dev = TheRenderManager->device;
	NiDX9RenderState* state = TheRenderManager->renderState;

	shadow.Data.x = 2.0f; // Type of geo (0 normal, 1 actors (skinned), 2 speedtree leaves)
	shadow.Data.y = 0.0f; // Alpha control
	LoadCasterWorld(caster);

	// Bind constant values for leaf transformation
	dev->SetVertexShaderConstantF(63, (float*)&TheShadowManager->BillboardRight, 1);
	dev->SetVertexShaderConstantF(64, (float*)&TheShadowManager->BillboardUp, 1);
	dev->SetVertexShaderConstantF(65, Pointers::ShaderParams::RockParams, 1);
	dev->SetVertexShaderConstantF(66, Pointers::ShaderParams::RustleParams, 1);
	dev->SetVertexShaderConstantF(67, Pointers::ShaderParams::WindMatrixes, 16);

	SpeedTreeLeafShaderProperty* leafProp = (SpeedTreeLeafShaderProperty*)caster->GetProperty(NiProperty::PropertyType::kType_Shade);
	BSTreeNode* node = (BSTreeNode*)caster->m_parent->m_parent;
	NiDX9SourceTextureData* tex = (NiDX9SourceTextureData*)node->TreeModel->LeavesTexture->rendererData;

	if (tex) shadow.Data.y = 1.0f;

	// Bind constant values for leaf transformation
	dev->SetVertexShaderConstantF(83, leafProp->leafData->leafBase, 48);

	// Set diffuse texture at register 0
	// UNOFFICIAL (Fable): the leaves texture can have no renderer data yet; bind nothing then instead of crashing.
	state->SetTexture(0, tex ? tex->dTexture : nullptr);
	state->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP, false);
	state->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP, false);
	state->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT, false);
	state->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT, false);
	state->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_POINT, false);
}


TerrainLODPass::TerrainLODPass() { SetUpShadowPass(*this); }


int TerrainLODPass::TestObject(NiGeometry* caster) {
	if (!caster->geomData || !caster->geomData->m_pkBuffData) return 0; // discard objects without buffer data

	BSShaderProperty* shade = (BSShaderProperty*)caster->GetProperty(NiProperty::PropertyType::kType_Shade);
	if (!shade || !shade->IsLightingProperty()) return 0;
	return 1;
}

bool TerrainLODPass::AccumObject(NiGeometry* caster) {
	return QueueIfDrawn(*this, caster, TestObject(caster));
}


void TerrainLODPass::UpdateConstants(NiGeometry* caster) {
	ShadowsExteriorEffect::ShadowStruct& shadow = TheShaderManager->Effects.ShadowsExteriors->Constants;
	shadow.Data.x = 3.0f; // Type of geo (0 normal, 1 actors (skinned), 2 speedtree leaves, 3 terrain LOD)
	shadow.Data.y = 0.0f; // Alpha Control
	LoadCasterWorld(caster);
	BindDiffuseOf(caster);	// UNOFFICIAL light grid colour
	const D3DXMATRIX& world = TheShaderManager->ShaderConst.ShadowWorld;
	D3DXMatrixTranspose(&Constants.WorldTranspose, &world);

	// the LOD land shader's morph inputs: morph distance and land drop, then the loaded area (x/y camera relative, z/w less 15)
	const BSShaderPPLightingProperty* lodShade = static_cast<BSShaderPPLightingProperty*>(caster->GetProperty(NiProperty::PropertyType::kType_Shade));
	D3DXVECTOR4& morph = Constants.LODLandParams;
	morph.x = lodShade->fMorphDistance;
	morph.y = *BSShaderManager::fLODLandDrop;

	const auto* loaded = BSShaderManager::kLoadedRange;
	const auto* eye = BSShaderManager::kCameraPos;
	D3DXVECTOR4& range = Constants.HighDetailRange;
	range.x = loaded->x - eye->x;
	range.y = loaded->y - eye->y;
	range.z = loaded->z - 15;
	range.w = loaded->w - 15;

	IDirect3DDevice9* dev = TheRenderManager->device;
	dev->SetVertexShaderConstantF(140, (float*)&Constants.WorldTranspose, 4);
	dev->SetVertexShaderConstantF(144, (float*)&Constants.HighDetailRange, 1);
	dev->SetVertexShaderConstantF(145, (float*)&Constants.LODLandParams, 1);
}
