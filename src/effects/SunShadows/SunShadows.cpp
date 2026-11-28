#include "../../core/GpuProfiler.h"
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-25 (NVR DXVK)
#include "../../core/RenderRect.h"	// UNOFFICIAL 2026-10-27 RenderScale v2

// Per-pass GPU timers nested in the caller's "Sun contact shadows": march, horizontal blur,
// vertical blur (+ intensity), and the deferred shadow apply of the four-pass technique.
// (Shared by the D3D9 passes and the Vulkan ones.)
static GpuTimer* ContactPassTimers() {
	static GpuTimer passTimers[4] = { GpuTimer("  Contact march"), GpuTimer("  Contact blur H"),
		GpuTimer("  Contact blur V"), GpuTimer("  Contact shadow apply") };
	return passTimers;
}

void SunShadowsEffect::SetCT() {
	EffectRecord::SetCT();

	// Change shadow atlas sampler state if anisotropy is supposed to be used.
	int anisotropy = TheShaderManager->Effects.ShadowsExteriors->Settings.ShadowMaps.Anisotropy;
	if (anisotropy) {
		TheRenderManager->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_ANISOTROPIC);
		TheRenderManager->SetSamplerState(1, D3DSAMP_MAXANISOTROPY, anisotropy);
	}
}

void SunShadowsEffect::UpdateSettings() {
	fastPaths = TheSettingManager->GetSettingI("Shaders.ShadowsExteriors.ScreenSpace", "FastPaths") != 0;
}

void SunShadowsEffect::RegisterTextures() {
	TheTextureManager->InitTexture("NVR_ShadowPassScratch0", &scratchTexture[0], &scratchSurface[0],
		TheRenderManager->width, TheRenderManager->height, D3DFMT_G16R16);
	TheTextureManager->InitTexture("NVR_ShadowPassScratch1", &scratchTexture[1], &scratchSurface[1],
		TheRenderManager->width, TheRenderManager->height, D3DFMT_G16R16);
}

bool SunShadowsEffect::TargetsWanted() {
	ShadowsExteriorEffect* ext = TheShaderManager ? TheShaderManager->Effects.ShadowsExteriors : nullptr;
	return Enabled && TheShaderManager && TheShaderManager->GameState.isExterior && ext && ext->Enabled &&
		(ext->Settings.Exteriors.Enabled || ext->Constants.ShadowFade.z > 0.0f);
}

/*
* The stock path renders every pass into TESR_PointShadowBuffer while the same passes sample it
* (the contact-shadow blurs read neighbouring texels), a read/write feedback loop with undefined
* results. Route the passes through two scratch targets instead -- buffer -> A -> B -> ... ->
* buffer -- so each pass reads a texture that is not bound for writing, and the last pass still
* lands in TESR_PointShadowBuffer without an extra copy.
*/
void SunShadowsEffect::Render(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface,
	UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	if (!Enabled || !Effect || !ShouldRender()) { NoteSkip(); renderTime = 0; return; }
	// UNOFFICIAL 2026-10-17 FastPaths: techniques 0/1 -> their copies 3/4 with the real early returns (when the file has them).
	// fixes-b #19: the copy is looked up by NAME (an older SunShadows.fx with other techniques at 3/4 is left alone).
	if (fastPaths && techniqueIndex <= 1) {
		static CachedHandle fastCache[2];
		D3DXHANDLE fast = TechniqueHandle(fastCache[techniqueIndex], techniqueIndex == 0 ? "ContactFast" : "ForwardContactShadowsFast");
		if (fast && Effect->GetTechnique(techniqueIndex + 3) == fast) techniqueIndex += 3;
	}

	IDirect3DTexture9* shadowTexture = TheShaderManager->Effects.ShadowsExteriors->Textures.ShadowPassTexture;
	IDirect3DSurface9* shadowSurface = TheShaderManager->Effects.ShadowsExteriors->Textures.ShadowPassSurface;
	D3DXHANDLE technique = Effect->GetTechnique(techniqueIndex);
	D3DXTECHNIQUE_DESC description = {};
	D3DSURFACE_DESC target = {}, scratch = {};
	if (pingPongFailed || !technique || RenderTarget != shadowSurface || !shadowTexture ||
		!scratchTexture[0] || !scratchTexture[1] || !scratchSurface[0] || !scratchSurface[1] ||
		FAILED(Effect->GetTechniqueDesc(technique, &description)) || description.Passes < 2 ||
		FAILED(RenderTarget->GetDesc(&target)) || FAILED(scratchSurface[0]->GetDesc(&scratch)) ||
		target.Width != scratch.Width || target.Height != scratch.Height || target.Format != scratch.Format) {
		EffectRecord::Render(Device, RenderTarget, RenderedSurface, techniqueIndex, ClearRenderTarget, SourceBuffer);
		return;
	}

	// UNOFFICIAL 2026-10-25 (NVR DXVK): the same passes as Vulkan passes when [Main.Main.Performance] NvrDxvkPasses is on.
	if (!ExtFailed && !ClearRenderTarget && DxvkCompute::PassesOnRect()) {
		auto extTimer = TimeLogger();
		if (ExtPingPong(Device, techniqueIndex, description, shadowTexture, shadowSurface)) {
			Device->SetRenderTarget(0, RenderTarget);
			renderTime = extTimer.LogTime("SunShadows::Extension");
			return;
		}
	}

	auto timer = TimeLogger();
	HRESULT result = Effect->SetTechnique(technique);
	if (SUCCEEDED(result)) SetCT();
	UINT passes = 0;
	if (SUCCEEDED(result)) result = Effect->Begin(&passes, 0);
	if (SUCCEEDED(result)) {
		GpuTimer* passTimers = ContactPassTimers();
		IDirect3DTexture9* source = shadowTexture;
		for (UINT p = 0; p < passes && SUCCEEDED(result); ++p) {
			GpuProfileScope gpuPass(passTimers[p < 4 ? p : 3], Device);
			const bool last = p == passes - 1;
			IDirect3DSurface9* destination = last ? shadowSurface : scratchSurface[p & 1];
			Device->SetTexture(3, nullptr); // TESR_PointShadowBuffer slot
			TheRenderManager->renderState->NoteDeviceTexture(3, nullptr);	// UNOFFICIAL 2026-10-29: keep the game's texture cache truthful (as SetCT)
			result = Device->SetRenderTarget(0, destination);
			if (FAILED(result)) break;
			if (ClearRenderTarget) Device->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 0, 0, 0), 1.0f, 0);
			result = Effect->BeginPass(p);
			if (FAILED(result)) break;
			Device->SetTexture(3, source);
			TheRenderManager->renderState->NoteDeviceTexture(3, source);
			RenderRect::ApplyViewport(Device);	// UNOFFICIAL 2026-10-27 RenderScale v2: the drawn part only in rect frames
			result = Device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
			HRESULT endResult = Effect->EndPass();
			if (SUCCEEDED(result)) result = endResult;
			source = last ? shadowTexture : scratchTexture[p & 1];
		}
		Effect->End();
	}
	Device->SetTexture(3, nullptr);
	TheRenderManager->renderState->NoteDeviceTexture(3, nullptr);
	Device->SetRenderTarget(0, RenderTarget);
	if (FAILED(result)) {
		pingPongFailed = true;
		Logger::Log("Sun shadow ping-pong failed (%08lx); using the in-place path until restart.", result);
		return;
	}
	static bool reported = false;
	if (!reported) {
		Logger::Log("UNOFFICIAL contact shadows use scratch ping-pong (%u passes, no feedback loop).", passes);
		reported = true;
	}
	renderTime = timer.LogTime("SunShadows::PingPong");
}

// --- The Vulkan passes (NVR DXVK) -----------------------------------------------------------------------------------
// UNOFFICIAL 2026-10-25 (NVR DXVK): every technique of SunShadows.fx as Compute\SunShadows<Pass>.frag (SunShadows.glsl holds what
// they share), line for line, through the same ping-pong as Render: buffer -> scratch A -> scratch B -> ... -> buffer, each pass
// into its own target with ExtDrawTo (the frame chain is not involved). The constants (eight matrices: 736 bytes) go to a uniform
// buffer, as the effect's SetCT uploads them: the matrices with ID3DXEffect::SetMatrix, so the memory as stored makes GLSL's M[i][j]
// HLSL's (see SunShadows.glsl). Sampler states as the .fx declares them: LINEAR/CLAMP for the depth, the atlas, the normals and the
// shadow buffer (one mip each; the atlas only ever read at explicit LOD 0, where SetCT's optional anisotropy does nothing), LINEAR/WRAP
// with linear mips for the blue noise (MipSampler, extension 1.3; an older fork keeps the D3D9 passes).
namespace {
	enum SunPass { SunMarch, SunBlurH, SunBlurV, SunApply, SunFinalBlur, SunSharp, SunSharpFinish };

	// The Constants block of SunShadows.glsl (std140).
	struct SunUniforms {
		D3DXMATRIX	cascade[4], projection, invProjection, view, invView;
		D3DXVECTOR4	reciprocalResolution, smoothedSunDir, viewSpaceLightDir, formatData, screenSpaceData, shadowFade, shadowBlur,
					forwardData, center[4], cameraData, flags;
		D3DXMATRIX	charToLight;		// UNOFFICIAL 2026-10-26 CharacterShadowMap: TESR_CharShadowCameraToLight, TESR_CharShadowCenter, TESR_CharShadowData
		D3DXVECTOR4	charCenter, charData;
		D3DXVECTOR4	renderScale;		// TESR_RenderScale (xy scale, zw 1/scale)
	};
	struct SunPush { UInt32 linearIndex, noiseIndex, pass, pad; };

	// Set 1: the textures in the .fx's sampler order (binding = slot); set 2: the constants; the samplers' heap indices at 0 and 4.
	const NVR_EXT_BINDING_DESC SunBindings[] = {
		{ 1, 0, NVR_EXT_BIND_SAMPLED_IMAGE,  0, NVR_EXT_VIEW_2D, FALSE, 0 },	// TESR_DepthBuffer
		{ 1, 1, NVR_EXT_BIND_SAMPLED_IMAGE,  1, NVR_EXT_VIEW_2D, FALSE, 0 },	// TESR_ShadowAtlas
		{ 1, 2, NVR_EXT_BIND_SAMPLED_IMAGE,  2, NVR_EXT_VIEW_2D, FALSE, 0 },	// TESR_NormalsBuffer
		{ 1, 3, NVR_EXT_BIND_SAMPLED_IMAGE,  3, NVR_EXT_VIEW_2D, FALSE, 0 },	// TESR_PointShadowBuffer (the pass's source)
		{ 1, 4, NVR_EXT_BIND_SAMPLED_IMAGE,  4, NVR_EXT_VIEW_2D, FALSE, 0 },	// TESR_NoiseSampler
		{ 1, 5, NVR_EXT_BIND_SAMPLED_IMAGE,  5, NVR_EXT_VIEW_2D, FALSE, 0 },	// TESR_CharShadowMap (UNOFFICIAL 2026-10-26; a stand-in without the map)
		{ 2, 0, NVR_EXT_BIND_UNIFORM_BUFFER, 0, NVR_EXT_VIEW_2D, FALSE, 0 },	// Constants
		{ 0, 0, NVR_EXT_BIND_SAMPLER,        0, NVR_EXT_VIEW_2D, FALSE, 0 },	// LINEAR, CLAMP
		{ 0, 0, NVR_EXT_BIND_SAMPLER,        1, NVR_EXT_VIEW_2D, FALSE, 4 },	// LINEAR, WRAP, MIPFILTER LINEAR
	};

	// The constant registered under a name: the memory the D3D9 path uploads (SetCT reads it at each draw). Null if absent.
	const D3DXVECTOR4* SunConstant(const char* name) {
		auto found = TheShaderManager->ConstantsTable.find(name);
		return found == TheShaderManager->ConstantsTable.end() ? nullptr : found->second;
	}
	// The texture registered under a sampler name (InitTexture / RegisterTexture). Null if absent.
	IDirect3DBaseTexture9* SunTexture(const char* name) {
		auto found = TheTextureManager->TextureNames.find(name);
		return found == TheTextureManager->TextureNames.end() || !found->second ? nullptr : *found->second;
	}
}

bool SunShadowsEffect::ExtPingPong(IDirect3DDevice9* Device, UINT techniqueIndex, const D3DXTECHNIQUE_DESC& description,
	IDirect3DTexture9* shadowTexture, IDirect3DSurface9* shadowSurface) {
	// The techniques of this SunShadows.fx, recognised by name and pass count (another file's keep the D3D9 passes).
	static const char* const names[5] = { nullptr, "ForwardContactShadows", "ForwardContactSharp", "ContactFast", "ForwardContactShadowsFast" };
	static const SunPass layouts[5][4] = {
		{ SunMarch, SunBlurH, SunBlurV, SunApply },		// technique 0
		{ SunMarch, SunBlurH, SunFinalBlur },			// ForwardContactShadows
		{ SunSharp, SunSharpFinish },					// ForwardContactSharp
		{ SunMarch, SunBlurH, SunBlurV, SunApply },		// ContactFast (ScreenSpaceShadowFast: the same march, SunShadowsMarch.frag)
		{ SunMarch, SunBlurH, SunFinalBlur },			// ForwardContactShadowsFast
	};
	static const UINT passCounts[5] = { 4, 3, 2, 4, 3 };
	if (techniqueIndex > 4 || description.Passes != passCounts[techniqueIndex]) return false;
	const char* name = description.Name;
	if (names[techniqueIndex] ? (!name || strcmp(name, names[techniqueIndex])) : (name && *name)) return false;

	// FORWARD_SHADOWS as the loaded effect was compiled with (EffectRecord::LoadEffect reads the setting at load).
	if (extForwardGeneration != LoadGeneration) {
		extForwardGeneration = LoadGeneration;
		extForwardCompiled = TheSettingManager->GetSettingI("Shaders.ShadowsExteriors.Main", "ForwardShadows") != 0;
		extCharCompiled = TheSettingManager->GetSettingI("Shaders.ShadowsExteriors.ShadowMaps", "CharacterShadowMap") != 0;	// (EffectRecord::LoadEffect, the same read)
	}

	static const char* const spvNames[5] = { "SunShadowsMarch", "SunShadowsBlur", "SunShadowsApply", "SunShadowsSharp", "SunShadowsSharpFinish" };
	static const UINT pipelineOf[7] = { 0, 1, 1, 2, 1, 3, 4 };	// SunPass -> extPasses / spvNames
	// UNOFFICIAL 2026-10-26: the Apply pass with CHARACTER_SHADOW_MAP compiled in is its own variant (Compute\variants.txt), as the .fx.
	ID3D9NvrExtPipeline* pipelines[5] = {};
	for (UINT p = 0; p < description.Passes; p++) {
		const UINT index = pipelineOf[layouts[techniqueIndex][p]];
		pipelines[index] = ExtVariant(extVariants, extPasses[index], spvNames[index], index == 2 && extCharCompiled ? "c1" : "", [](const char* name) {
			return DxvkCompute::CreateGraphicsPipeline(name, SunBindings, _countof(SunBindings), sizeof(SunPush), true);
		});
		if (!pipelines[index]) return false;
	}
	ID3D9NvrExtSampler* linear = DxvkCompute::LinearClamp();
	ID3D9NvrExtSampler* noiseSampler = DxvkCompute::MipSampler(false, NVR_EXT_ADDRESS_WRAP);
	if (!linear || !noiseSampler) return false;

	static const char* const cascadeNames[4] = { "TESR_ShadowCameraToLightTransformNear", "TESR_ShadowCameraToLightTransformMiddle",
		"TESR_ShadowCameraToLightTransformFar", "TESR_ShadowCameraToLightTransformLod" };
	static const char* const centerNames[4] = { "TESR_ShadowNearCenter", "TESR_ShadowMiddleCenter", "TESR_ShadowFarCenter", "TESR_ShadowLodCenter" };
	const D3DXVECTOR4* cascade[4];
	const D3DXVECTOR4* center[4];
	for (int i = 0; i < 4; i++) {
		cascade[i] = SunConstant(cascadeNames[i]);
		center[i] = SunConstant(centerNames[i]);
		if (!cascade[i] || !center[i]) return false;
	}
	const D3DXVECTOR4* projection = SunConstant("TESR_ProjectionTransform");
	const D3DXVECTOR4* invProjection = SunConstant("TESR_InvProjectionTransform");
	const D3DXVECTOR4* view = SunConstant("TESR_ViewTransform");
	const D3DXVECTOR4* invView = SunConstant("TESR_InvViewTransform");
	const D3DXVECTOR4* reciprocal = SunConstant("TESR_ReciprocalResolution");
	const D3DXVECTOR4* smoothedSunDir = SunConstant("TESR_SmoothedSunDir");
	const D3DXVECTOR4* viewSpaceLightDir = SunConstant("TESR_ViewSpaceLightDir");
	const D3DXVECTOR4* formatData = SunConstant("TESR_ShadowFormatData");
	const D3DXVECTOR4* screenSpaceData = SunConstant("TESR_ShadowScreenSpaceData");
	const D3DXVECTOR4* shadowFade = SunConstant("TESR_ShadowFade");
	const D3DXVECTOR4* shadowBlur = SunConstant("TESR_ShadowBlur");
	const D3DXVECTOR4* forwardData = SunConstant("TESR_ShadowForwardData");
	const D3DXVECTOR4* cameraData = SunConstant("TESR_CameraData");
	if (!projection || !invProjection || !view || !invView || !reciprocal || !smoothedSunDir || !viewSpaceLightDir || !formatData ||
		!screenSpaceData || !shadowFade || !shadowBlur || !forwardData || !cameraData)
		return false;

	// The textures as SetCT binds them (the shadow buffer's slot takes each pass's source below).
	IDirect3DBaseTexture9* textures[6] = { SunTexture("TESR_DepthBuffer"), SunTexture("TESR_ShadowAtlas"), SunTexture("TESR_NormalsBuffer"),
		shadowTexture, ExtFileTexture("TESR_NoiseSampler"), SunTexture("TESR_CharShadowMap") };
	if (!textures[5]) textures[5] = textures[0];	// no map (CharacterShadowMap off): never read (the generic Apply shader)
	for (IDirect3DBaseTexture9* texture : textures) if (!texture) return false;

	SunUniforms uniforms = {};
	for (int i = 0; i < 4; i++) {
		memcpy(&uniforms.cascade[i], cascade[i], sizeof(D3DXMATRIX));	// as stored, as SetMatrix takes them
		uniforms.center[i] = *center[i];
	}
	memcpy(&uniforms.projection, projection, sizeof(D3DXMATRIX));
	memcpy(&uniforms.invProjection, invProjection, sizeof(D3DXMATRIX));
	memcpy(&uniforms.view, view, sizeof(D3DXMATRIX));
	memcpy(&uniforms.invView, invView, sizeof(D3DXMATRIX));
	uniforms.reciprocalResolution = *reciprocal;
	uniforms.smoothedSunDir = *smoothedSunDir;
	uniforms.viewSpaceLightDir = *viewSpaceLightDir;
	uniforms.formatData = *formatData;
	uniforms.screenSpaceData = *screenSpaceData;
	uniforms.shadowFade = *shadowFade;
	uniforms.shadowBlur = *shadowBlur;
	uniforms.forwardData = *forwardData;
	uniforms.cameraData = *cameraData;
	uniforms.flags = D3DXVECTOR4(extForwardCompiled ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
	if (const D3DXVECTOR4* nearFade = SunConstant("TESR_ShadowScreenSpaceFade")) { uniforms.flags.y = nearFade->x; uniforms.flags.z = nearFade->y; }	// UNOFFICIAL 2026-10-27 NearFade
	if (const D3DXVECTOR4* rtReplace = SunConstant("TESR_RtSunReplace")) uniforms.flags.w = rtReplace->x;	// UNOFFICIAL 2026-10-28 RT sun shadows Mode 1
	if (const D3DXVECTOR4* charToLight = SunConstant("TESR_CharShadowCameraToLight")) memcpy(&uniforms.charToLight, charToLight, sizeof(D3DXMATRIX));
	if (const D3DXVECTOR4* charCenter = SunConstant("TESR_CharShadowCenter")) uniforms.charCenter = *charCenter;
	if (const D3DXVECTOR4* charData = SunConstant("TESR_CharShadowData")) uniforms.charData = *charData;
	const bool rectOn = RenderRect::Active();
	uniforms.renderScale = rectOn
		? D3DXVECTOR4(RenderRect::ScaleX, RenderRect::ScaleY, 1.0f / RenderRect::ScaleX, 1.0f / RenderRect::ScaleY)
		: D3DXVECTOR4(1.0f, 1.0f, 1.0f, 1.0f);
	ID3D9NvrExtBuffer* buffer = ExtUniforms(extUniforms, &uniforms, sizeof(uniforms));
	if (!buffer) return false;

	NVR_EXT_RESOURCE_BINDING resources[_countof(SunBindings)] = {};
	for (UINT i = 0; i < 6; i++) {
		resources[i].Slot = i; resources[i].Type = NVR_EXT_BIND_SAMPLED_IMAGE; resources[i].pResource = textures[i];
	}
	resources[6].Slot = 0; resources[6].Type = NVR_EXT_BIND_UNIFORM_BUFFER; resources[6].pResource = buffer;
	resources[7].Slot = 0; resources[7].Type = NVR_EXT_BIND_SAMPLER; resources[7].pResource = linear;
	resources[8].Slot = 1; resources[8].Type = NVR_EXT_BIND_SAMPLER; resources[8].pResource = noiseSampler;

	D3DSURFACE_DESC desc = {};
	if (rectOn && FAILED(shadowSurface->GetDesc(&desc))) return false;
	const RECT vp = { 0, 0, (LONG)RenderRect::Width(desc.Width), (LONG)RenderRect::Height(desc.Height) };
	if (rectOn && (vp.right <= 0 || vp.bottom <= 0)) return false;

	// The passes as Render draws them. The device's own target is moved to each pass's destination first, so a pass never reads the
	// device's current render target (pass 0 reads the shadow buffer, the caller's target). A failure before the last pass leaves the
	// shadow buffer untouched (only the last pass writes it), so the D3D9 passes can still do this frame from the start.
	GpuTimer* passTimers = ContactPassTimers();
	Device->SetTexture(3, nullptr); // TESR_PointShadowBuffer slot
	// UNOFFICIAL 2026-10-29: the game's texture cache told too. Before, it kept the last texture a world draw had bound on stage 3 while the
	// device held none, so the next game bind of that texture there was skipped and sampled nothing (SetCT, which notes, does not run first here).
	TheRenderManager->renderState->NoteDeviceTexture(3, nullptr);
	IDirect3DTexture9* source = shadowTexture;
	for (UINT p = 0; p < description.Passes; p++) {
		GpuProfileScope gpuPass(passTimers[p < 4 ? p : 3], Device);
		const bool last = p == description.Passes - 1;
		const SunPass kind = layouts[techniqueIndex][p];
		IDirect3DSurface9* destination = last ? shadowSurface : scratchSurface[p & 1];
		if (FAILED(Device->SetRenderTarget(0, destination))) return false;
		resources[3].pResource = source;
		SunPush push = {};
		push.pass = kind == SunBlurV ? 1 : kind == SunFinalBlur ? 2 : 0;	// SunShadowsBlur.frag: 0 H, 1 V, 2 FinalContactBlur
		if (!ExtDrawTo(pipelines[pipelineOf[kind]], &destination, 1, resources, _countof(resources), &push, sizeof(push),
			rectOn ? &vp : nullptr, rectOn ? ExtUv::TexelCentre : ExtUv::ScreenQuad)) return false;	// FrameVS into a smaller target
		source = last ? shadowTexture : scratchTexture[p & 1];
	}
	if (++ExtFrames == 1) Logger::Log("UNOFFICIAL Vegas Shaders DXVK: %s runs as Vulkan passes (%u passes).", Name, description.Passes);
	return true;
}
