#include <algorithm>
#include <cstddef>

#include "Bloom.h"
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-25 (NVR DXVK)
#include "../../core/RenderRect.h"	// UNOFFICIAL 2026-10-27 RenderScale v2

void BloomEffect::RegisterConstants() {
	ShaderManager* sm = TheShaderManager;
	sm->RegisterConstant("TESR_BloomData", &Constants.Data);
	sm->RegisterConstant("TESR_BloomExtraData", &Constants.ExtraData);
	sm->RegisterConstant("TESR_BloomResolution", &Constants.Resolution);
	sm->RegisterConstant("TESR_BloomGlow", &Constants.Glow);
	sm->RegisterConstant("TESR_BloomGlowExtra", &Constants.GlowExtra);
};

void BloomEffect::RegisterTextures() {
	// Level i is half the size of level i - 1 and is registered as TESR_BloomBuffer, then TESR_BloomBuffer2, 4, 8 ...
	int levelWidth = TheRenderManager->width;
	int levelHeight = TheRenderManager->height;
	for (int level = 0; level < MaxPasses; level++) {
		std::string name = "TESR_BloomBuffer";
		if (level > 0) name += std::to_string(1 << level);

		levelWidth /= 2;
		levelHeight /= 2;

		Settings.Resolution[level] = D3DXVECTOR4((float)levelWidth, (float)levelHeight, 1.0f / (float)levelWidth, 1.0f / (float)levelHeight);

		TheTextureManager->InitTexture(name.c_str(), &Textures.BloomTexture[level], &Textures.BloomSurface[level],
			Settings.Resolution[level].x, Settings.Resolution[level].y, D3DFMT_A16B16G16R16F);
		TheShaderManager->CreateFrameVertex(levelWidth, levelHeight, &Textures.BloomVertexBuffer[level]);
	}
};

void BloomEffect::UpdateSettings() {
	// the three day periods: settings section and the group each fills
	const char* sections[3] = { "Shaders.Bloom.Main", "Shaders.Bloom.Night", "Shaders.Bloom.Interiors" };
	BloomSettings* periods[3] = { &Settings.Main, &Settings.Night, &Settings.Interiors };
	for (int i = 0; i < 3; i++) {
		periods[i]->Passes = TheSettingManager->GetSettingF(sections[i], "Passes");
		periods[i]->PassBlending = TheSettingManager->GetSettingF(sections[i], "PassBlending");
	}

	// UNOFFICIAL (a key missing from an older settings file reads 0: as before).
	for (int i = 0; i < 3; i++) {
		periods[i]->Spread = std::clamp(TheSettingManager->GetSettingF(sections[i], "Spread"), -0.75f, 2.0f);
		periods[i]->GlowBoost = std::clamp(TheSettingManager->GetSettingF(sections[i], "GlowBoost"), 0.0f, 20.0f);
	}
	Settings.GlowLightLevel = TheSettingManager->GetSettingF("Shaders.Bloom.Main", "GlowLightLevel");
	Settings.GlowLightLevel = Settings.GlowLightLevel > 0.0f ? std::clamp(Settings.GlowLightLevel, 0.25f, 50.0f) : 1.5f;
	Settings.LinearLight = TheSettingManager->GetSettingI("Shaders.Bloom.Main", "LinearLight") != 0;
	Settings.AntiFlicker = TheSettingManager->GetSettingI("Shaders.Bloom.Main", "AntiFlicker") != 0;
};




// UNOFFICIAL GlowBoost: the brightness (linear) of a white surface in the scene's own light: the sky's ambient plus the sun's
// or moon's light outdoors, the cell's ambient plus its directional light indoors (as BounceLightEffect::SpillThreshold).
static float BloomSceneLight() {
	D3DXVECTOR4 ambient = TheShaderManager->ShaderConst.sunAmbient, direct = TheShaderManager->ShaderConst.sunColor;
	if (!TheShaderManager->GameState.isExterior && Player && Player->parentCell && Player->parentCell->IsInterior() &&
		Player->parentCell->lighting) {
		ambient = Player->parentCell->lighting->ambient.toD3DXVECTOR4();
		direct = Player->parentCell->lighting->directional.toD3DXVECTOR4();
	}
	auto linear = [](float c) { c = (std::max)(c, 0.0f); return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); };
	return 0.2126f * linear(ambient.x + direct.x) + 0.7152f * linear(ambient.y + direct.y) + 0.0722f * linear(ambient.z + direct.z);
}

void BloomEffect::UpdateConstants() {
	// .x and .y updated dynamically to reflect filterRadius.
	ShaderManager* sm = TheShaderManager;
	const BloomSettingsStruct& s = Settings;
	const float aspect = (float)TheRenderManager->width / TheRenderManager->height;
	Constants.Data = D3DXVECTOR4(
		0.005f,
		0.005f * aspect,	// scaled for non-square resolutions
		std::clamp(sm->GetTransitionValue(s.Main.PassBlending, s.Night.PassBlending, s.Interiors.PassBlending), 0.0f, 1.0f),
		1.0f / std::clamp(sm->GetTransitionValue(s.Main.Passes, s.Night.Passes, s.Interiors.Passes), 2.0f, 8.0f));

	// UNOFFICIAL Spread: the last upsample divides by the sum of the levels' weights (exactly .w as before at weight 1).
	const float spread = 1.0f + std::clamp(TheShaderManager->GetTransitionValue(Settings.Main.Spread, Settings.Night.Spread, Settings.Interiors.Spread), -0.75f, 2.0f);
	Constants.Glow.x = spread;
	Constants.Glow.y = Constants.Data.w;
	if (spread != 1.0f) {
		const int passes = std::clamp((int)TheShaderManager->GetTransitionValue(Settings.Main.Passes, Settings.Night.Passes, Settings.Interiors.Passes), 2, 8);
		float total = 0.0f, weight = 1.0f;
		for (int i = 0; i < passes; i++) { total += weight; weight *= spread; }
		Constants.Glow.y = 1.0f / total;
	}
	Constants.Glow.z = (std::max)(0.0f, TheShaderManager->GetTransitionValue(Settings.Main.GlowBoost, Settings.Night.GlowBoost, Settings.Interiors.GlowBoost));
	Constants.Glow.w = Settings.GlowLightLevel * BloomSceneLight();
	Constants.GlowExtra.x = Settings.AntiFlicker ? 1.0f : 0.0f;
	Constants.GlowExtra.y = Settings.LinearLight ? 1.0f : 0.0f;

	// Flag for tonemapping.
	Constants.ExtraData.x = Enabled && (!TheShaderManager->GameState.OverlayIsOn);
};


bool BloomEffect::SwitchEffect() {
	EffectRecord::SwitchEffect();

	// switched off: leave the top level black so nothing stale is read
	if (!Enabled)
		TheRenderManager->device->ColorFill(Textures.BloomSurface[0], NULL, D3DCOLOR_ARGB(255, 0, 0, 0));

	Constants.ExtraData.x = Enabled;

	return Enabled;
}

/*
* Renders a single bloom pass to current render target, using the passed in technique. Clears target if specified.
*/
void BloomEffect::RenderPass(IDirect3DDevice9* Device, UINT techniqueIndex, bool ClearRenderTarget) {
	try {
		Effect->SetTechnique(Effect->GetTechnique(techniqueIndex));
		SetCT(); // refresh the constant table
		UINT passCount = 0;
		Effect->Begin(&passCount, NULL);
		for (UINT pass = 0; pass < passCount; pass++) {
			if (ClearRenderTarget) Device->Clear(0L, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 0, 0, 0), 1.0f, 0L);
			Effect->BeginPass(pass);
			Device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
			Effect->EndPass();
		}
		Effect->End();
	}
	catch (const std::exception& e) {
		Logger::Log("Error during rendering of effect %s: %s", Name, e.what());
	}
}

// UNOFFICIAL 2026-10-25 (NVR DXVK): the push block of every Bloom*.frag (Bloom.glsl), the same layout.
// UNOFFICIAL 2026-11-19 RenderRect: renderScale (.xy src0 active scale, .zw src1 active scale) & clampBounds (.xy src0 max UV, .zw src1 max UV).
struct BloomExtPush {
	D3DXVECTOR4	data;			// TESR_BloomData
	D3DXVECTOR4	resolution;		// TESR_BloomResolution
	D3DXVECTOR4	glow;			// TESR_BloomGlow
	D3DXVECTOR4	glowExtra;		// TESR_BloomGlowExtra
	UInt32		samplerIndex;	// LINEAR, CLAMP: the extension writes the sampler heap index here (PushOffset)
	UInt32		pad[3];
	D3DXVECTOR4	renderScale;	// 80: (src0ScaleX, src0ScaleY, src1ScaleX, src1ScaleY)
	D3DXVECTOR4	clampBounds;	// 96: (src0MaxU, src0MaxV, src1MaxU, src1MaxV)
};

/*
* UNOFFICIAL 2026-10-25 (NVR DXVK): the chain as Vulkan passes (src\hlsl\NewVegas\Compute\Bloom*.frag). The same passes, the same
* constants (the Data.xy and Resolution changes included), each one a pass into its own target (ExtDrawTo; the frame chain is not
* touched). The D3D9 upsample reads a level and writes that same level (the sum over it), which a Vulkan pass cannot do: the
* downsample levels below the top one go to extLevels, and the upsamples write the chain's own levels, so nothing is read and
* written at once. The top level is the downsample's in both paths. Only BloomTexture[0] is read outside the chain (Lens, Rain,
* Precipitations, the game's HDR blend shaders, Debug). Returns false when a pass is refused; the D3D9 chain then does the frame
* from the same image.
*/
bool BloomEffect::RenderBloomExt(IDirect3DDevice9* Device, int passes) {
	// The pipelines are made on first use. Bindings: the textures in order (set 1, binding i, slot i), then the sampler.
	auto make = [](ExtPass& pass, const char* spvName, UINT textureCount) {
		if (pass.Tried) return;
		pass.Tried = true;
		NVR_EXT_BINDING_DESC bindings[3] = {};
		for (UINT i = 0; i < textureCount; i++) {
			bindings[i].Set = 1; bindings[i].Binding = i; bindings[i].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[i].Slot = i; bindings[i].ViewType = NVR_EXT_VIEW_2D;
		}
		bindings[textureCount].Type = NVR_EXT_BIND_SAMPLER; bindings[textureCount].Slot = 0; bindings[textureCount].PushOffset = (UINT)offsetof(BloomExtPush, samplerIndex);
		pass.Pipeline = DxvkCompute::CreateGraphicsPipeline(spvName, bindings, textureCount + 1, sizeof(BloomExtPush), true);
	};
	make(extDownFirst, "BloomDownFirst", 1);
	make(extDown, "BloomDown", 1);
	make(extUp, "BloomUp", 2);
	make(extUpLast, "BloomUpLast", 2);
	ID3D9NvrExtSampler* sampler = DxvkCompute::LinearClamp();
	if (!extDownFirst.Pipeline || !extDown.Pipeline || !extUp.Pipeline || !extUpLast.Pipeline || !sampler) return false;

	// The downsample levels below the top: the chain's own format and sizes, made once when this path first runs.
	if (!extLevelsTried) {
		extLevelsTried = true;
		for (int i = 0; i < MaxPasses - 1; i++) {
			std::string name = "NVR_BloomLevel" + std::to_string(i);
			TheTextureManager->InitTexture(name.c_str(), &extLevels.Texture[i], &extLevels.Surface[i], (int)Settings.Resolution[i].x, (int)Settings.Resolution[i].y, D3DFMT_A16B16G16R16F);
		}
	}
	for (int i = 0; i < passes - 1; i++) {
		if (!extLevels.Texture[i] || !extLevels.Surface[i]) return false;
	}

	// Level i as the chain holds it between its passes: the downsample's own texture below the top, the chain's at the top.
	auto levelTexture = [&](int i) -> IDirect3DBaseTexture9* { return i < passes - 1 ? extLevels.Texture[i] : Textures.BloomTexture[i]; };
	auto levelSurface = [&](int i) -> IDirect3DSurface9* { return i < passes - 1 ? extLevels.Surface[i] : Textures.BloomSurface[i]; };

	const bool rectOn = RenderRect::Active();
	auto levelActiveW = [&](int lvl) -> UINT {
		const UINT w = (UINT)Settings.Resolution[lvl].x;
		return rectOn ? (std::max)(1u, RenderRect::Width(w)) : w;
	};
	auto levelActiveH = [&](int lvl) -> UINT {
		const UINT h = (UINT)Settings.Resolution[lvl].y;
		return rectOn ? (std::max)(1u, RenderRect::Height(h)) : h;
	};
	auto srcBounds = [&](int srcLvl, float& sx, float& sy, float& hiX, float& hiY) {
		if (!rectOn) { sx = 1.0f; sy = 1.0f; hiX = 1.0f; hiY = 1.0f; return; }
		if (srcLvl < 0) {
			const UINT w = (UINT)TheRenderManager->width, h = (UINT)TheRenderManager->height;
			sx = (float)RenderRect::Width(w) / (float)w;
			sy = (float)RenderRect::Height(h) / (float)h;
			hiX = sx - 0.5f / (float)w;
			hiY = sy - 0.5f / (float)h;
		} else {
			const float w = Settings.Resolution[srcLvl].x, h = Settings.Resolution[srcLvl].y;
			sx = (float)levelActiveW(srcLvl) / w;
			sy = (float)levelActiveH(srcLvl) / h;
			hiX = sx - 0.5f / w;
			hiY = sy - 0.5f / h;
		}
	};

	// One pass into one target: the textures in binding order, then the LINEAR/CLAMP sampler.
	auto draw = [&](ExtPass& pass, IDirect3DBaseTexture9* const* textures, UINT textureCount, IDirect3DSurface9* target, int dstLvl, const BloomExtPush& push) {
		NVR_EXT_RESOURCE_BINDING resources[3] = {};
		for (UINT i = 0; i < textureCount; i++) { resources[i].Slot = i; resources[i].Type = NVR_EXT_BIND_SAMPLED_IMAGE; resources[i].pResource = textures[i]; }
		resources[textureCount].Slot = 0; resources[textureCount].Type = NVR_EXT_BIND_SAMPLER; resources[textureCount].pResource = sampler;
		const RECT vp = { 0, 0, (LONG)levelActiveW(dstLvl), (LONG)levelActiveH(dstLvl) };
		return ExtDrawTo(pass.Pipeline, &target, 1, resources, textureCount + 1, &push, sizeof(push), rectOn ? &vp : nullptr, ExtUv::TexelCentre);	// D3D9: a quad per level (BloomVertexBuffer)
	};
	auto pushConstants = [&](int src0Lvl, int src1Lvl) {
		BloomExtPush push = {};
		push.data = Constants.Data;
		push.resolution = Constants.Resolution;
		push.glow = Constants.Glow;
		push.glowExtra = Constants.GlowExtra;
		float sx0, sy0, hiX0, hiY0, sx1, sy1, hiX1, hiY1;
		srcBounds(src0Lvl, sx0, sy0, hiX0, hiY0);
		srcBounds(src1Lvl, sx1, sy1, hiX1, hiY1);
		push.renderScale = D3DXVECTOR4(sx0, sy0, sx1, sy1);
		push.clampBounds = D3DXVECTOR4(hiX0, hiY0, hiX1, hiY1);
		return push;
	};

	// Downsample: level 0 from the image, each further level from the one above (techniques 0 to passes - 1 of the D3D9 chain).
	for (int i = 0; i < passes; i++) {
		Constants.Resolution = Settings.Resolution[i];
		IDirect3DBaseTexture9* textures[1] = { i == 0 ? TheTextureManager->RenderedTexture : levelTexture(i - 1) };
		if (!draw(i == 0 ? extDownFirst : extDown, textures, 1, levelSurface(i), i, pushConstants(i - 1, i - 1))) return false;
	}

	// Upsample from the top down: level i gets the level above it (the chain's BloomTexture[i + 1]) with its own downsample added.
	// The last one (i = 0, technique 14 of the D3D9 chain) is the frame's bloom.
	for (int i = passes - 2; i >= 0; i--) {
		if (Constants.Data.z > 0.0f) {
			Constants.Data.x = Settings.Resolution[i + 1].z; // Pixel size x axis of the upsampled texture.
			Constants.Data.y = Settings.Resolution[i + 1].w; // Pixel size y axis of the upsampled texture.
		}
		Constants.Resolution = Settings.Resolution[i];
		IDirect3DBaseTexture9* textures[2] = { Textures.BloomTexture[i + 1], levelTexture(i) };
		if (!draw(i == 0 ? extUpLast : extUp, textures, 2, Textures.BloomSurface[i], i, pushConstants(i + 1, i))) return false;
	}
	return true;
}

/*
* Renders the final bloom buffer by iteratively downsampling and upsampling with intermediary blur.
*/
// UNOFFICIAL 2026-10-27 RenderScale v2: in a rect frame each level draws into its top-left Width x Height part, with a quad of its own
// (uv 0..Scale plus this level's half texel, as BloomVertexBuffer has); call after SetRenderTarget. Otherwise the level's usual quad.
IDirect3DVertexBuffer9* BloomEffect::LevelQuad(int i) {
	if (!RenderRect::Active()) return Textures.BloomVertexBuffer[i];
	const UINT w = (UINT)Settings.Resolution[i].x, h = (UINT)Settings.Resolution[i].y;
	const D3DXVECTOR3 key(RenderRect::ScaleX, RenderRect::ScaleY, (float)w);
	if (!RectVertexBuffer[i] || RectVertexKey[i] != key) {
		if (RectVertexBuffer[i]) { RectVertexBuffer[i]->Release(); RectVertexBuffer[i] = nullptr; }
		TheShaderManager->CreateFrameVertex(w, h, &RectVertexBuffer[i], RenderRect::ScaleX, RenderRect::ScaleY);
		RectVertexKey[i] = key;
	}
	const D3DVIEWPORT9 port = { 0, 0, (std::max)(1u, RenderRect::Width(w)), (std::max)(1u, RenderRect::Height(h)), 0.0f, 1.0f };
	TheRenderManager->device->SetViewport(&port);
	return RectVertexBuffer[i] ? RectVertexBuffer[i] : Textures.BloomVertexBuffer[i];
}

void BloomEffect::RenderBloomBuffer(IDirect3DSurface9* RenderTarget) {
	if (!Enabled) {
		renderTime = 0.0f;
		return; // skip rendering if the effect is disabled
	}

	auto timer = TimeLogger();
	BufferFrame = TheFrameRateManager->FrameNumber;	// UNOFFICIAL 2026-11-15 fpnative-fix (Bloom.h)

	IDirect3DDevice9* Device = TheRenderManager->device;
	NiDX9RenderState* RenderState = TheRenderManager->renderState;

	const int passes = std::clamp((int) TheShaderManager->GetTransitionValue(Settings.Main.Passes, Settings.Night.Passes, Settings.Interiors.Passes), 2, 8);

	// UNOFFICIAL 2026-10-25 (NVR DXVK) / 2026-11-19 RenderRect: Vulkan bloom runs under RenderScale < 1.0 too.
	if (!ExtFailed && DxvkCompute::PassesOnRect() && RenderBloomExt(Device, passes)) {
		Device->SetStreamSource(0, TheShaderManager->FrameVertex, 0, sizeof(FrameVS)); // as the D3D9 chain leaves it
		Device->SetRenderTarget(0, RenderTarget);
		RenderRect::RescaleAfterUpscale(Textures.BloomSurface[0]);
		renderTime = timer.LogTime("EffectRecord::ExtRender Bloom");
		return;
	}

	// Points the device at one bloom level (UNOFFICIAL RenderScale v2: LevelQuad sets the viewport, so it comes after the target).
	auto aimAtLevel = [&](int level) {
		Device->SetRenderTarget(0, Textures.BloomSurface[level]);
		Device->SetStreamSource(0, LevelQuad(level), 0, sizeof(FrameVS));
		Constants.Resolution = Settings.Resolution[level];
	};

	// The effect file lists the downsample techniques first, then the upsample ones for MaxPasses levels.
	UINT technique = 0;

	// down: each level is blurred from the one above it, the first level clears its target
	for (int level = 0; level < passes; level++) {
		aimAtLevel(level);
		RenderPass(Device, technique++, true);
	}

	technique += 2 * (MaxPasses - passes);

	// up: from the deepest level back to the top, combining as it goes
	for (int level = passes - 2; level >= 0; level--) {
		aimAtLevel(level);
		if (Constants.Data.z > 0.0f) {
			// pixel size of the texture being upsampled
			Constants.Data.x = Settings.Resolution[level + 1].z;
			Constants.Data.y = Settings.Resolution[level + 1].w;
		}
		RenderPass(Device, technique++, false);
	}

	Device->SetStreamSource(0, TheShaderManager->FrameVertex, 0, sizeof(FrameVS)); // hand the full-screen quad back to the other effects
	Device->SetRenderTarget(0, RenderTarget);
	// UNOFFICIAL 2026-10-27 RenderScale v2: the top level is also read after the upscale, at full size (the game's tonemap shader,
	// the rain): stretched over the whole buffer then (user: "a ghosted image" over the screen with tonemapping on).
	RenderRect::RescaleAfterUpscale(Textures.BloomSurface[0]);

	renderTime = timer.LogTime("EffectRecord::Render Bloom");
}
