#include "BounceLight.h"
#include "../../core/TemporalBackbone.h"	// UNOFFICIAL 2026-11-01 SharedNoise
#include "BounceLightMath.h"
#include "../../core/GpuProfiler.h"
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-25 (NVR DXVK)
#include "../../core/RenderRect.h"	// UNOFFICIAL 2026-10-27 RenderScale v2

void BounceLightEffect::UpdateConstants() {
}

void BounceLightEffect::UpdateSettings() {
	Constants.Data.x = TheSettingManager->GetSettingF("Shaders.BounceLight.Main", "Strength");
	strength = Constants.Data.x;
	Constants.Data.y = TheSettingManager->GetSettingF("Shaders.BounceLight.Main", "Radius");
	Constants.Data.z = TheSettingManager->GetSettingF("Shaders.BounceLight.Main", "DrawDistance");
	Constants.Data.w = TheSettingManager->GetSettingF("Shaders.BounceLight.Main", "BlurRadius");
	quarterResolution = TheSettingManager->GetSettingI("Shaders.BounceLight.Main", "QuarterResolution") != 0;
	// UNOFFICIAL (a key missing from an older settings file reads 0: off, as before).
	offScreenLight = std::clamp(TheSettingManager->GetSettingF("Shaders.BounceLight.Main", "OffScreenLight"), 0.0f, 4.0f);
	multiBounce = std::clamp(TheSettingManager->GetSettingF("Shaders.BounceLight.Main", "MultiBounce"), 0.0f, 2.0f);
	temporal = TheSettingManager->GetSettingI("Shaders.BounceLight.Main", "Temporal") != 0;
	halfSamples = TheSettingManager->GetSettingI("Shaders.BounceLight.Main", "HalfSamples") != 0;
	// UNOFFICIAL 2026-11-09 TemporalJitterFix (core/TemporalBackbone.h ShiftReprojection; missing key: on)
	{ SettingManager::Configuration::ConfigNode node; jitterFix = !TheSettingManager->Config.FillNode(&node, "Shaders.BounceLight.Main", "TemporalJitterFix") || node.BoolValue; }
	// UNOFFICIAL NeonSpill (0 or missing: off; reach and light level missing: their defaults).
	// NeonSpillOn: the spill's own switch, so it can be turned off and on without losing its strength. Off, the bounce light runs
	// exactly as with NeonSpill 0 (the plain bounce, no pyramid, no spill buffers drawn).
	const bool spillOn = TheSettingManager->GetSettingI("Shaders.BounceLight.Main", "NeonSpillOn") != 0;
	neonSpill = spillOn ? std::clamp(TheSettingManager->GetSettingF("Shaders.BounceLight.Main", "NeonSpill"), 0.0f, 20.0f) : 0.0f;
	spillReach = TheSettingManager->GetSettingF("Shaders.BounceLight.Main", "NeonSpillReach");
	spillReach = spillReach > 0.0f ? std::clamp(spillReach, 0.25f, 4.0f) : 1.0f;
	spillLightLevel = TheSettingManager->GetSettingF("Shaders.BounceLight.Main", "NeonSpillLightLevel");
	spillLightLevel = spillLightLevel > 0.0f ? std::clamp(spillLightLevel, 0.25f, 50.0f) : 1.5f;
	spillDebug = std::clamp(TheSettingManager->GetSettingI("Shaders.BounceLight.Main", "NeonSpillDebug"), 0, 2);
	spillMemorySeconds = std::clamp(TheSettingManager->GetSettingF("Shaders.BounceLight.Main", "NeonSpillMemory"), 0.0f, 5.0f);
	spillNear = std::clamp(TheSettingManager->GetSettingF("Shaders.BounceLight.Main", "NeonSpillNear"), 0.0f, 4.0f);
	spillFar = std::clamp(TheSettingManager->GetSettingF("Shaders.BounceLight.Main", "NeonSpillFar"), 0.0f, 4.0f);
	spillCap = std::clamp(TheSettingManager->GetSettingF("Shaders.BounceLight.Main", "NeonSpillBrightCap"), 0.0f, 100.0f);
	// UNOFFICIAL round 6 (0 or missing: round 5 exactly).
	spillHoldAfterTurn = std::clamp(TheSettingManager->GetSettingF("Shaders.BounceLight.Main", "NeonSpillHoldAfterTurn"), 0.0f, 5.0f);
	spillSeparateDepth = std::clamp(TheSettingManager->GetSettingF("Shaders.BounceLight.Main", "NeonSpillSeparateDepth"), 0.0f, 1.0f);
	spillCapSceneLight = std::clamp(TheSettingManager->GetSettingF("Shaders.BounceLight.Main", "NeonSpillCapSceneLight"), 0.0f, 1.0f);
	spillMaskSpillOnly = std::clamp(TheSettingManager->GetSettingF("Shaders.BounceLight.Main", "NeonSpillMaskSpillOnly"), 0.0f, 1.0f);
	spillHardEdges = std::clamp(TheSettingManager->GetSettingF("Shaders.BounceLight.Main", "NeonSpillHardEdges"), 0.0f, 1.0f);
	spillSteadySeconds = std::clamp(TheSettingManager->GetSettingF("Shaders.BounceLight.Main", "NeonSpillSteadySeconds"), 0.0f, 2.0f);
	// UNOFFICIAL 2026-10-12: the hard depth test for all bounce light, spill or not (the user saw sunlit walls' bounce drawn over
	// the Lucky 38's pillars in front of them: the soft weights were normalised, so pillars whose four low-resolution texels all
	// lay on the wall took the wall's light).
	hardEdges = TheSettingManager->GetSettingI("Shaders.BounceLight.Main", "HardEdges") != 0;
}

void BounceLightEffect::RegisterConstants() {
	TheShaderManager->RegisterConstant("TESR_BounceLightData", &Constants.Data);
}

// Nothing at startup: the buffers (about 37 MB at 1440p for both resolutions) are made the first time the effect
// renders at a resolution (EnsureBuffers), so an install that never turns it on pays no video memory.
void BounceLightEffect::RegisterTextures() {
}

void BounceLightEffect::EnsureBuffers(bool quarter) {
	if (quarter && !quarterTried) {
		quarterTried = true;
		const int width = MaxQuality::Reduced(TheRenderManager->width, 4);
		const int height = MaxQuality::Reduced(TheRenderManager->height, 4);
		TheTextureManager->InitTexture("NVR_BounceBufferQuarter0", &quarterTexture[0], &quarterSurface[0], width, height, D3DFMT_A16B16G16R16F);
		TheTextureManager->InitTexture("NVR_BounceBufferQuarter1", &quarterTexture[1], &quarterSurface[1], width, height, D3DFMT_A16B16G16R16F);
		TheTextureManager->InitTexture("NVR_BouncePrepQuarter", &quarterPrepTexture, &quarterPrepSurface, width, height, D3DFMT_A32B32G32R32F);
	}
	if (!quarter && !halfTried) {
		halfTried = true;
		const int width = MaxQuality::Reduced(TheRenderManager->width, 2);
		const int height = MaxQuality::Reduced(TheRenderManager->height, 2);
		TheTextureManager->InitTexture("NVR_BounceBuffer0", &halfTexture[0], &halfSurface[0], width, height, D3DFMT_A16B16G16R16F);
		TheTextureManager->InitTexture("NVR_BounceBuffer1", &halfTexture[1], &halfSurface[1], width, height, D3DFMT_A16B16G16R16F);
		TheTextureManager->InitTexture("NVR_BouncePrep", &halfPrepTexture, &halfPrepSurface, width, height, D3DFMT_A32B32G32R32F);
	}
}

// UNOFFICIAL, Temporal: two more low-resolution buffers (last frame's gather is read from one while this frame's goes to the
// other; about 1.8 MB each at a quarter of 1440p, 7.4 MB at half), cleared so nothing undefined is ever read back.
void BounceLightEffect::EnsureHistory(bool quarter) {
	bool& tried = quarter ? quarterHistoryTried : halfHistoryTried;
	if (tried) return;
	tried = true;
	const int width = quarter ? MaxQuality::Reduced(TheRenderManager->width, 4) : MaxQuality::Reduced(TheRenderManager->width, 2);
	const int height = quarter ? MaxQuality::Reduced(TheRenderManager->height, 4) : MaxQuality::Reduced(TheRenderManager->height, 2);
	IDirect3DTexture9** textures = quarter ? quarterHistory : halfHistory;
	IDirect3DSurface9** surfaces = quarter ? quarterHistorySurface : halfHistorySurface;
	TheTextureManager->InitTexture(quarter ? "NVR_BounceHistoryQuarter0" : "NVR_BounceHistory0", &textures[0], &surfaces[0], width, height, D3DFMT_A16B16G16R16F);
	TheTextureManager->InitTexture(quarter ? "NVR_BounceHistoryQuarter1" : "NVR_BounceHistory1", &textures[1], &surfaces[1], width, height, D3DFMT_A16B16G16R16F);
	for (int i = 0; i < 2; i++) if (surfaces[i]) TheRenderManager->device->ColorFill(surfaces[i], NULL, D3DCOLOR_ARGB(0, 0, 0, 0));
}

// The game's colours are display-encoded (sRGB); the bounce works in linear light.
static float BounceLinear(float c) {
	c = std::clamp(c, 0.0f, 1.0f);
	return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

// UNOFFICIAL NeonSpill: the pyramid's buffers, level 0 320 texels wide (BounceLight.fx.hlsl's SpillBaseWidth) and as tall as
// the screen's shape gives, each next one half the one before; and the spill's memory buffers. Made once.
bool BounceLightEffect::EnsureSpill() {
	if (!spillTried) {
		spillTried = true;
		static const char* names[SpillLevels] = { "NVR_BounceSpill0", "NVR_BounceSpill1", "NVR_BounceSpill2", "NVR_BounceSpill3",
			"NVR_BounceSpill4", "NVR_BounceSpill5", "NVR_BounceSpill6" };
		int width = 320, height = (std::max)(1, (int)ceilf(320.0f * TheRenderManager->height / (float)TheRenderManager->width));
		for (int i = 0; i < SpillLevels; i++) {
			TheTextureManager->InitTexture(names[i], &spillTexture[i], &spillSurface[i], width, height, D3DFMT_A16B16G16R16F);
			if (i == 0) {	// NeonSpillSteadySeconds: level 0's size
				static const char* steady[2] = { "NVR_BounceSpillSteady0", "NVR_BounceSpillSteady1" };
				for (int k = 0; k < 2; k++) {
					TheTextureManager->InitTexture(steady[k], &spillSteadyTexture[k], &spillSteadySurface[k], width, height, D3DFMT_A16B16G16R16F);
					if (spillSteadySurface[k]) TheRenderManager->device->ColorFill(spillSteadySurface[k], NULL, D3DCOLOR_ARGB(0, 0, 0, 0));
				}
			}
			width = (width + 1) / 2;
			height = (height + 1) / 2;
		}
		// The spill's memory, per bounce resolution (RenderSpillGather checks them).
		static const char* memories[2][2] = { { "NVR_BounceSpillMemory0", "NVR_BounceSpillMemory1" },
			{ "NVR_BounceSpillMemoryQuarter0", "NVR_BounceSpillMemoryQuarter1" } };
		for (int q = 0; q < 2; q++) {
			const int memoryWidth = q ? MaxQuality::Reduced(TheRenderManager->width, 4) : MaxQuality::Reduced(TheRenderManager->width, 2);
			const int memoryHeight = q ? MaxQuality::Reduced(TheRenderManager->height, 4) : MaxQuality::Reduced(TheRenderManager->height, 2);
			for (int i = 0; i < 2; i++) {
				TheTextureManager->InitTexture(memories[q][i], &spillMemoryTexture[q][i], &spillMemorySurface[q][i], memoryWidth, memoryHeight, D3DFMT_A16B16G16R16F);
				if (spillMemorySurface[q][i]) TheRenderManager->device->ColorFill(spillMemorySurface[q][i], NULL, D3DCOLOR_ARGB(0, 0, 0, 0));
			}
		}
	}
	for (int i = 0; i < SpillLevels; i++) if (!spillTexture[i] || !spillSurface[i]) return false;
	return true;
}

// UNOFFICIAL NeonSpill: how bright (linear) a pixel must be to count as a light: NeonSpillLightLevel times the brightness of a
// white surface in the scene's own light, the sky's ambient plus the sun's or moon's light outdoors, the cell's ambient plus
// its directional light indoors (display-encoded colours added, as the game lights a surface, then made linear). Logged when
// it changes by more than a fifth (a door, the time of day, the weather).
float BounceLightEffect::SpillThreshold() {
	D3DXVECTOR4 ambient = TheShaderManager->ShaderConst.sunAmbient, direct = TheShaderManager->ShaderConst.sunColor;
	if (!TheShaderManager->GameState.isExterior && Player && Player->parentCell && Player->parentCell->IsInterior() &&
		Player->parentCell->lighting) {
		ambient = Player->parentCell->lighting->ambient.toD3DXVECTOR4();
		direct = Player->parentCell->lighting->directional.toD3DXVECTOR4();
	}
	auto linear = [](float c) { c = (std::max)(c, 0.0f); return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); };
	const float light = 0.2126f * linear(ambient.x + direct.x) + 0.7152f * linear(ambient.y + direct.y) + 0.0722f * linear(ambient.z + direct.z);
	if (spillReportedLight < 0.0f || fabsf(light - spillReportedLight) > 0.2f * spillReportedLight) {
		spillReportedLight = light;
		Logger::Log("UNOFFICIAL BounceLight: neon spill: the scene's own light is %.3f (%s ambient %.2f %.2f %.2f + %s %.2f %.2f %.2f); lights brighter than %.3f count (x %.2f).",
			light, TheShaderManager->GameState.isExterior ? "sky" : "cell", ambient.x, ambient.y, ambient.z,
			TheShaderManager->GameState.isExterior ? "sun/moon" : "directional", direct.x, direct.y, direct.z, light * spillLightLevel, spillLightLevel);
	}
	return light * spillLightLevel;
}

// UNOFFICIAL NeonSpill: BounceSpillPyramid's passes, each into its level's buffer, reading the level before (stage 8 + its
// index); level 0 reads the image and the depth, and counts what is brighter than threshold (linear) as light. False if
// anything is missing or fails.
bool BounceLightEffect::RenderSpillPyramid(IDirect3DDevice9* Device, IDirect3DTexture9* scene, float threshold) {
	D3DXHANDLE technique = TechniqueHandle(techSpillPyramid, "BounceSpillPyramid");
	D3DXHANDLE targetHandle = ParameterHandle(paramSpillTarget, "NVR_SpillTarget");
	D3DXHANDLE memoryHandle = ParameterHandle(paramSpillMemory, "NVR_SpillMemory");
	D3DXTECHNIQUE_DESC description = {};
	if (!technique || !targetHandle || !memoryHandle || !EnsureSpill() ||
		FAILED(Effect->GetTechniqueDesc(technique, &description)) || description.Passes != SpillLevels)
		return false;
	static GpuTimer pyramidTimer("  Neon spill pyramid");
	GpuProfileScope gpu(pyramidTimer, Device);
	UINT passes = 0;
	D3DXVECTOR4 memoryData(0.0f, 0.0f, 0.0f, threshold);
	Effect->SetVector(memoryHandle, &memoryData);
	// NeonSpillNear / Far / BrightCap (the cap is used here, the weights by the gather).
	if (D3DXHANDLE shapeHandle = ParameterHandle(paramSpillShape, "NVR_SpillShape")) {
		// UNOFFICIAL NeonSpillCapSceneLight: the cap in multiples of the scene's own light (threshold / NeonSpillLightLevel) instead
		// of the light test, so it does not grow when the light test is raised.
		D3DXVECTOR4 shape(spillNear, spillFar, spillCapSceneLight > 0.0f ? spillCap / spillLightLevel : spillCap, 0.0f);
		Effect->SetVector(shapeHandle, &shape);
	}
	// UNOFFICIAL NeonSpillSteadySeconds: this frame's share of the running average (frame time over the seconds), where last
	// frame's average is (the pyramid's own last camera; NVR_BounceReproject is set again for the bounce's passes later), and
	// whether it can be used (y 0: no usable last average, the light counts at once as before the average existed).
	const bool steady = spillSteadySeconds > 0.0f && spillSteadyTexture[0] && spillSteadyTexture[1];
	D3DXHANDLE steadyHandle = steady ? ParameterHandle(paramSpillSteady, "NVR_SpillSteady") : NULL;
	D3DXHANDLE reprojectHandle = steady ? ParameterHandle(paramReproject, "NVR_BounceReproject") : NULL;
	const bool steadyOn = steadyHandle && reprojectHandle;
	if (steadyOn) {
		const double now = TheFrameRateManager->Time;
		const double gap = now - spillSteadyTime;
		const bool usable = spillSteadyValid && gap > 0.0 && gap < 0.5 && !TheShaderManager->GameState.isCameraCut;
		D3DXMATRIX reproject;
		D3DXMatrixIdentity(&reproject);
		if (usable) BounceLightReprojection(TheRenderManager->viewMatrix, TheRenderManager->InvViewMatrix, spillSteadyView, spillSteadyInvView, spillSteadyProj, &reproject);
		// w: the camera has not moved since the last average (as NeonSpillHoldAfterTurn's test): the shader reads it at the same
		// texel, exactly, instead of through the reprojection (whose round trip is a hair off, which averaged neighbours in).
		bool moved = false;
		for (int i = 0; i < 4 && usable && !moved; i++) for (int j = 0; j < 4 && !moved; j++)
			moved = fabsf(TheRenderManager->viewMatrix.m[i][j] - spillSteadyView.m[i][j]) >= (i < 3 ? 1.0e-5f : 0.01f) ||
				fabsf(TheRenderManager->projMatrix.m[i][j] - spillSteadyProj.m[i][j]) >= 1.0e-6f;
		Effect->SetMatrix(reprojectHandle, &reproject);
		D3DXVECTOR4 steadyData(usable ? std::clamp((float)(1.0 - exp(-gap / spillSteadySeconds)), 0.01f, 1.0f) : 1.0f, usable ? 1.0f : 0.0f,
			spillDebug == 1 ? 1.0f : 0.0f, usable && !moved ? 1.0f : 0.0f);	// z: NeonSpillDebug 1 shows the average; w: still
		static int logged = 0, frames = 0, stills = 0;
		static float largest = 0.0f;
		if (usable) {
			frames++;
			stills += moved ? 0 : 1;
			for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) largest = (std::max)(largest, fabsf(TheRenderManager->viewMatrix.m[i][j] - spillSteadyView.m[i][j]));
			if (frames % 300 == 0 && logged < 4) {
				logged++;
				Logger::Log("UNOFFICIAL BounceLight: NeonSpillSteadySeconds %.2f: share %.3f (frame %.4f s); of the last 300 frames %d had a still camera; largest view change %g.",
					spillSteadySeconds, steadyData.x, (float)gap, stills, largest);
				stills = 0;
				largest = 0.0f;
			}
		}
		Effect->SetVector(steadyHandle, &steadyData);
	}
	else if (D3DXHANDLE offHandle = ParameterHandle(paramSpillSteady, "NVR_SpillSteady")) {
		D3DXVECTOR4 off(0.0f, 0.0f, 0.0f, 0.0f);
		Effect->SetVector(offHandle, &off);
	}
	Effect->SetTechnique(technique);
	SetCT();
	HRESULT result = Effect->Begin(&passes, 0);
	if (FAILED(result)) return false;
	for (int i = 0; i < SpillLevels && SUCCEEDED(result); i++) {
		D3DSURFACE_DESC target = {}, read = {};
		spillSurface[i]->GetDesc(&target);
		if (i) spillSurface[i - 1]->GetDesc(&read);
		D3DXVECTOR4 data(1.0f / target.Width, 1.0f / target.Height, i ? 1.0f / read.Width : 0.0f, i ? 1.0f / read.Height : 0.0f);
		Effect->SetVector(targetHandle, &data);
		for (int s = 0; s < SpillLevels; s++) Device->SetTexture(8 + s, nullptr); // the level written is never bound
		D3DVIEWPORT9 viewport = { 0, 0, target.Width, target.Height, 0.0f, 1.0f };
		result = Device->SetRenderTarget(0, spillSurface[i]);
		if (SUCCEEDED(result)) result = Device->SetViewport(&viewport);
		if (SUCCEEDED(result)) result = Effect->BeginPass(i);
		if (FAILED(result)) break;
		if (i) Device->SetTexture(8 + i - 1, spillTexture[i - 1]);
		Device->SetTexture(2, scene); // TESR_SourceBuffer slot: the current image
		DWORD writeMask1 = 0xF;
		if (i == 0 && steadyOn) {	// NeonSpillSteadySeconds: last average read at stage 15, this frame's written as target 1
			// Target 1 has its own colour write mask (D3DRS_COLORWRITEENABLE1); whatever the game left there, all four channels.
			Device->GetRenderState(D3DRS_COLORWRITEENABLE1, &writeMask1);
			Device->SetRenderState(D3DRS_COLORWRITEENABLE1, 0xF);
			Device->SetTexture(15, spillSteadyTexture[spillSteadyIndex]);
			const HRESULT second = Device->SetRenderTarget(1, spillSteadySurface[spillSteadyIndex ^ 1]);
			static bool reportedSecond = false;
			if (!reportedSecond) {
				reportedSecond = true;
				D3DCAPS9 caps = {};
				Device->GetDeviceCaps(&caps);
				Logger::Log("UNOFFICIAL BounceLight: NeonSpillSteadySeconds: second target %s (0x%08X), device takes %u targets at once, "
					"its write mask was 0x%X, independent masks %s.", SUCCEEDED(second) ? "set" : "FAILED", (unsigned)second,
					(unsigned)caps.NumSimultaneousRTs, (unsigned)writeMask1, (caps.PrimitiveMiscCaps & D3DPMISCCAPS_INDEPENDENTWRITEMASKS) ? "yes" : "no");
			}
		}
		result = Device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
		if (i == 0 && steadyOn) {
			Device->SetRenderTarget(1, nullptr);
			Device->SetTexture(15, nullptr);
			Device->SetRenderState(D3DRS_COLORWRITEENABLE1, writeMask1);
		}
		HRESULT endResult = Effect->EndPass();
		if (SUCCEEDED(result)) result = endResult;
	}
	Effect->End();
	for (int s = 0; s < SpillLevels; s++) Device->SetTexture(8 + s, nullptr);
	if (steadyOn) {
		if (SUCCEEDED(result)) {
			spillSteadyIndex ^= 1;
			spillSteadyValid = true;
			spillSteadyTime = TheFrameRateManager->Time;
			spillSteadyView = TheRenderManager->viewMatrix;
			spillSteadyInvView = TheRenderManager->InvViewMatrix;
			spillSteadyProj = TheRenderManager->projMatrix;
		}
		else spillSteadyValid = false;
	}
	return SUCCEEDED(result);
}

// UNOFFICIAL NeonSpill: BounceSpillGather into this frame's memory buffer at the bounce's low resolution (the pyramid at
// stages 8-14, last frame's memory at stage 7). NVR_SpillMemory, NVR_BounceLayout, NVR_BounceTemporal (frame number) and
// NVR_BounceReproject are set by Render first. Returns the buffer written, or null if anything is missing or fails.
IDirect3DTexture9* BounceLightEffect::RenderSpillGather(IDirect3DDevice9* Device, bool quarter, const D3DVIEWPORT9& reduced) {
	D3DXHANDLE technique = TechniqueHandle(techSpillGather, "BounceSpillGather");
	// UNOFFICIAL NeonSpillSeparateDepth: the gather that keeps a light's depth apart from what stands in the way (it reads the
	// pyramid's level 0 size from NVR_SpillOptions, set by Render). Missing from an older BounceLight.fx.hlsl: the gather as before.
	if (spillSeparateDepth > 0.0f && ParameterHandle(paramSpillOptions, "NVR_SpillOptions"))
		if (D3DXHANDLE separate = TechniqueHandle(techSpillGatherSeparate, "BounceSpillGatherSeparate")) technique = separate;
	IDirect3DTexture9* const* textures = spillMemoryTexture[quarter ? 1 : 0];
	IDirect3DSurface9* const* surfaces = spillMemorySurface[quarter ? 1 : 0];
	if (!technique || !textures[0] || !textures[1] || !surfaces[0] || !surfaces[1]) return nullptr;
	static GpuTimer spillTimer("  Neon spill gather");
	GpuProfileScope gpu(spillTimer, Device);
	const int write = spillMemoryIndex ^ 1;
	Effect->SetTechnique(technique);
	SetCT();
	UINT passes = 0;
	HRESULT result = Effect->Begin(&passes, 0);
	if (FAILED(result)) return nullptr;
	result = Device->SetRenderTarget(0, surfaces[write]);
	if (SUCCEEDED(result)) result = Device->SetViewport(&reduced);
	if (SUCCEEDED(result)) result = Effect->BeginPass(0);
	if (SUCCEEDED(result)) {
		for (int s = 0; s < SpillLevels; s++) Device->SetTexture(8 + s, spillTexture[s]); // NVR_Spill0-6
		Device->SetTexture(7, textures[spillMemoryIndex]); // NVR_BounceHistory: last frame's spill
		result = Device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
		HRESULT endResult = Effect->EndPass();
		if (SUCCEEDED(result)) result = endResult;
	}
	Effect->End();
	for (int s = 0; s < SpillLevels; s++) Device->SetTexture(8 + s, nullptr);
	Device->SetTexture(7, nullptr);
	return SUCCEEDED(result) ? textures[write] : nullptr;
}

// ---- UNOFFICIAL 2026-10-25 (NVR DXVK): the five passes as Vulkan passes (see BounceLight.h) ----
namespace {
	struct BouncePush {
		UInt32 pointSampler;			// 0: POINT, CLAMP (filled in by DXVK)
		UInt32 linearSampler;			// 4: LINEAR, CLAMP
		UInt32 wrapSampler;				// 8: POINT, WRAP (the blue noise)
		UInt32 pad;
		D3DXVECTOR4 pass;				// 16: the pass's uniform arguments (BounceLight*.frag)
	};
	struct BounceUniforms {
		D3DXMATRIX projection, invProjection, reproject;	// TESR_ProjectionTransform, TESR_InvProjectionTransform, NVR_BounceReproject (as stored)
		D3DXVECTOR4 data, reciprocalResolution, layout, temporal, ambient, cameraData;
	};

	// The constants from the same registered memory the D3D9 path uploads (SetCT).
	const D3DXVECTOR4* RegisteredConstant(const char* name) {
		auto found = TheShaderManager->ConstantsTable.find(name);
		return found == TheShaderManager->ConstantsTable.end() ? nullptr : found->second;
	}

	IDirect3DBaseTexture9* SlotTexture(const char* name) {
		std::string slotName = name;
		IDirect3DBaseTexture9** slot = TheTextureManager->GetTextureSlotByName(slotName);
		return slot ? *slot : nullptr;
	}

	// Textures in set 1 (binding i, slot i), the constants in set 2 binding 0, the three samplers' heap indices at push offsets 0, 4, 8.
	void MakePipeline(EffectRecord::ExtPass& pass, const char* spvName, UINT textureCount) {
		if (pass.Tried) return;
		pass.Tried = true;
		NVR_EXT_BINDING_DESC bindings[8] = {};
		UINT count = 0;
		for (UINT i = 0; i < textureCount; i++, count++) {
			bindings[count].Set = 1; bindings[count].Binding = i; bindings[count].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[count].Slot = i; bindings[count].ViewType = NVR_EXT_VIEW_2D;
		}
		bindings[count].Set = 2; bindings[count].Binding = 0; bindings[count].Type = NVR_EXT_BIND_UNIFORM_BUFFER; bindings[count].Slot = 0; count++;
		for (UINT s = 0; s < 3; s++, count++) { bindings[count].Type = NVR_EXT_BIND_SAMPLER; bindings[count].Slot = s; bindings[count].PushOffset = 4 * s; }
		pass.Pipeline = DxvkCompute::CreateGraphicsPipeline(spvName, bindings, count, sizeof(BouncePush), true);
	}

	// The resources of one draw, in MakePipeline's order.
	UINT Resources(NVR_EXT_RESOURCE_BINDING* resources, IDirect3DBaseTexture9* const* textures, UINT textureCount, ID3D9NvrExtBuffer* uniforms) {
		UINT count = 0;
		for (UINT i = 0; i < textureCount; i++, count++) { resources[count].Slot = i; resources[count].Type = NVR_EXT_BIND_SAMPLED_IMAGE; resources[count].pResource = textures[i]; }
		resources[count].Slot = 0; resources[count].Type = NVR_EXT_BIND_UNIFORM_BUFFER; resources[count].pResource = uniforms; count++;
		ID3D9NvrExtSampler* samplers[3] = { DxvkCompute::PointClamp(), DxvkCompute::LinearClamp(), DxvkCompute::PointWrap() };
		for (UINT s = 0; s < 3; s++, count++) { resources[count].Slot = s; resources[count].Type = NVR_EXT_BIND_SAMPLER; resources[count].pResource = samplers[s]; }
		return count;
	}
}

// Render's D3D9 loop as Vulkan passes: pass p of the technique Render set, from the same textures into the same targets.
bool BounceLightEffect::ExtPasses(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, D3DXHANDLE technique,
	UINT firstPass, UINT endPass, IDirect3DTexture9* scene, IDirect3DTexture9* const* lowTexture, IDirect3DSurface9* const* lowSurface,
	IDirect3DTexture9* prepTexture, IDirect3DSurface9* prepSurface, IDirect3DTexture9* const* historyTexture,
	IDirect3DSurface9* const* historySurface, int writeIndex, const D3DXVECTOR4& layout, const D3DXVECTOR4& temporalData,
	const D3DXVECTOR4& ambientData, const D3DXMATRIX& reproject, GpuTimer* passTimers) {
	if (!DxvkCompute::Available()) return false;
	// The technique's Prepare (x: PreparePlus), Gather (Gather, or GatherPlus(samples, temporal, motionPixels)) and Combine (hard).
	const bool plus = technique != Effect->GetTechnique(0);
	D3DXVECTOR4 gather(0.0f, 8.0f, 0.0f, 1.0f);
	bool hard = false;
	if (plus) {
		if (technique == TechniqueHandle(techPlus, "BounceLightPlus") || (hard = technique == TechniqueHandle(techPlusHard, "BounceLightPlusHardEdges")))
			gather = D3DXVECTOR4(1.0f, 8.0f, 0.0f, 1.0f);
		else if (technique == TechniqueHandle(techTemporal, "BounceLightTemporal") || (hard = technique == TechniqueHandle(techTemporalHard, "BounceLightTemporalHardEdges")))
			gather = D3DXVECTOR4(1.0f, 8.0f, 1.0f, 4.0f);
		else if (technique == TechniqueHandle(techTemporalHalf, "BounceLightTemporalHalf") || (hard = technique == TechniqueHandle(techTemporalHalfHard, "BounceLightTemporalHalfHardEdges")))
			gather = D3DXVECTOR4(1.0f, 4.0f, 1.0f, 8.0f);
		else return false;
	}
	MakePipeline(extPrepare, "BounceLightPrepare", 4);
	MakePipeline(extGather, "BounceLightGather", 3);
	MakePipeline(extBlur, "BounceLightBlur", 1);
	MakePipeline(extCombine, "BounceLightCombine", 3);
	IDirect3DBaseTexture9* depth = SlotTexture("TESR_DepthBuffer");
	IDirect3DBaseTexture9* normals = SlotTexture("TESR_NormalsBuffer");
	IDirect3DBaseTexture9* noise = ExtFileTexture("TESR_BlueNoiseSampler");
	const D3DXVECTOR4* projection = RegisteredConstant("TESR_ProjectionTransform");
	const D3DXVECTOR4* invProjection = RegisteredConstant("TESR_InvProjectionTransform");
	const D3DXVECTOR4* reciprocalResolution = RegisteredConstant("TESR_ReciprocalResolution");
	const D3DXVECTOR4* cameraData = RegisteredConstant("TESR_CameraData");
	if (!extPrepare.Pipeline || !extGather.Pipeline || !extBlur.Pipeline || !extCombine.Pipeline || !DxvkCompute::PointClamp() ||
		!DxvkCompute::LinearClamp() || !DxvkCompute::PointWrap() || !depth || !normals || !noise || !projection || !invProjection ||
		!reciprocalResolution || !cameraData) return false;
	BounceUniforms uniforms = {};
	memcpy(&uniforms.projection, projection, sizeof(D3DXMATRIX));
	memcpy(&uniforms.invProjection, invProjection, sizeof(D3DXMATRIX));
	uniforms.reproject = reproject;
	uniforms.data = Constants.Data;
	uniforms.reciprocalResolution = *reciprocalResolution;
	uniforms.layout = layout;
	uniforms.temporal = temporalData;
	uniforms.ambient = ambientData;
	uniforms.cameraData = *cameraData;
	ID3D9NvrExtBuffer* buffer = ExtUniforms(extUniforms, &uniforms, sizeof(uniforms));
	if (!buffer) return false;
	const bool useHistory = historyTexture != nullptr;
	NVR_EXT_RESOURCE_BINDING resources[8] = {};
	BouncePush push = {};
	for (UINT p = firstPass; p < endPass; ++p) {
		GpuProfileScope gpu(passTimers[p], Device);
		bool drawn = false;
		if (p == 0) {	// Prepare: into the packed buffer (PreparePlus also reads last frame's result, low buffer 0)
			push.pass = D3DXVECTOR4(plus ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
			IDirect3DBaseTexture9* inputs[4] = { depth, normals, scene, lowTexture[0] };
			drawn = ExtDrawTo(extPrepare.Pipeline, &prepSurface, 1, resources, Resources(resources, inputs, 4, buffer), &push, sizeof(push));
		}
		else if (p == 1) {	// Gather: into low buffer 0, or with Temporal this frame's history buffer (last frame's read; the packed buffer stands in otherwise)
			push.pass = gather;
			IDirect3DBaseTexture9* inputs[3] = { prepTexture, noise, useHistory ? historyTexture[writeIndex ^ 1] : prepTexture };
			IDirect3DSurface9* target = useHistory ? historySurface[writeIndex] : lowSurface[0];
			drawn = ExtDrawTo(extGather.Pipeline, &target, 1, resources, Resources(resources, inputs, 3, buffer), &push, sizeof(push));
		}
		else if (p == 2 || p == 3) {	// BlurX: (history or) low 0 -> low 1; BlurY: low 1 -> low 0
			push.pass = p == 2 ? D3DXVECTOR4(1.0f, 0.0f, 0.0f, 0.0f) : D3DXVECTOR4(0.0f, 1.0f, 0.0f, 0.0f);
			IDirect3DBaseTexture9* inputs[1] = { p == 3 ? lowTexture[1] : useHistory ? historyTexture[writeIndex] : lowTexture[0] };
			drawn = ExtDrawTo(extBlur.Pipeline, &lowSurface[p == 2 ? 1 : 0], 1, resources, Resources(resources, inputs, 1, buffer), &push, sizeof(push));
		}
		else {	// Combine: the frame chain's next image (Commit), as the D3D9 pass into chain.Output()
			push.pass = D3DXVECTOR4(hard ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
			IDirect3DBaseTexture9* inputs[3] = { scene, depth, lowTexture[0] };
			drawn = ExtFullScreen(Device, RenderTarget, RenderedSurface, nullptr, extCombine.Pipeline, resources, Resources(resources, inputs, 3, buffer),
				&push, sizeof(push), false);
		}
		if (!drawn) return false;
	}
	if (endPass < 5 && ++ExtFrames == 1) Logger::Log("UNOFFICIAL Vegas Shaders DXVK: %s runs as Vulkan passes.", Name);
	return true;
}

// Prepare, gather, blur X and blur Y at half or quarter resolution, then the combine at full resolution into the
// frame, the same way the dedicated ambient occlusion runs.
void BounceLightEffect::Render(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget,
	IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget,
	IDirect3DSurface9* SourceBuffer) {
	// UNOFFICIAL 2026-10-13: the neon spill is RETIRED (the user: it looks drawn over the screen, not like a light in the scene, and
	// bled through roofs; a world-space replacement may come later). Off every frame, whatever a settings file or the diagnostic
	// self-test sets: the bounce light runs exactly as with NeonSpill 0. Its code (RenderSpillPyramid, RenderSpillGather and the
	// shader's spill parts) is unreachable from here and can be deleted.
	neonSpill = 0.0f;
	if (!Enabled || !Effect || !ShouldRender()) { NoteSkip(); renderTime = 0; lastValid = historyValid = false; return; }
	EnsureBuffers(quarterResolution);
	if (quarterResolution && !quarterTexture[0]) EnsureBuffers(false); // quarter failed: half resolution instead
	D3DXHANDLE layoutHandle = ParameterHandle(paramLayout, "NVR_BounceLayout");
	D3DXTECHNIQUE_DESC description = {};
	IDirect3DTexture9* scene = TheTextureManager->RenderedTexture;
	const bool quarter = quarterResolution && quarterTexture[0] && quarterTexture[1] && quarterSurface[0] && quarterSurface[1] &&
		quarterPrepTexture && quarterPrepSurface;
	IDirect3DTexture9* const* lowTexture = quarter ? quarterTexture : halfTexture;
	IDirect3DSurface9* const* lowSurface = quarter ? quarterSurface : halfSurface;
	IDirect3DTexture9* prepTexture = quarter ? quarterPrepTexture : halfPrepTexture;
	IDirect3DSurface9* prepSurface = quarter ? quarterPrepSurface : halfPrepSurface;
	// UNOFFICIAL: the technique for the options on (the first one, as before, when all are off), and the history buffers
	// for Temporal. Anything missing (an older BounceLight.fx.hlsl, a buffer that could not be made): a step back, logged once.
	D3DXHANDLE technique = Effect->GetTechnique(0);
	IDirect3DTexture9* const* historyTexture = nullptr;
	IDirect3DSurface9* const* historySurface = nullptr;
	const bool extras = offScreenLight > 0.0f || multiBounce > 0.0f || temporal || neonSpill > 0.0f;
	D3DXHANDLE temporalHandle = NULL, ambientHandle = NULL, reprojectHandle = NULL, spillHandle = NULL;
	if (extras) {
		temporalHandle = ParameterHandle(paramTemporal, "NVR_BounceTemporal");
		ambientHandle = ParameterHandle(paramAmbient, "NVR_BounceAmbient");
		reprojectHandle = ParameterHandle(paramReproject, "NVR_BounceReproject");
		spillHandle = ParameterHandle(paramSpill, "NVR_BounceSpill");
		D3DXHANDLE wanted = NULL;
		if (temporal) {
			EnsureHistory(quarter);
			IDirect3DTexture9* const* textures = quarter ? quarterHistory : halfHistory;
			IDirect3DSurface9* const* surfaces = quarter ? quarterHistorySurface : halfHistorySurface;
			wanted = halfSamples ? TechniqueHandle(techTemporalHalf, "BounceLightTemporalHalf") : TechniqueHandle(techTemporal, "BounceLightTemporal");
			if (wanted && textures[0] && textures[1] && surfaces[0] && surfaces[1]) { historyTexture = textures; historySurface = surfaces; }
			else wanted = NULL;
		}
		if (!wanted) wanted = TechniqueHandle(techPlus, "BounceLightPlus");
		if (wanted && temporalHandle && ambientHandle && reprojectHandle && (spillHandle || neonSpill <= 0.0f)) technique = wanted;
		else historyTexture = nullptr;
		static D3DXHANDLE reportedTechnique = NULL;
		static float reportedSpill = -1.0f, reportedLevel = -1.0f, reportedMemory = -1.0f, reportedShape = -1.0f, reportedRound6 = -1.0f;
		static int reportedDebug = -1;
		if (technique != reportedTechnique || neonSpill != reportedSpill || spillLightLevel != reportedLevel || spillDebug != reportedDebug ||
			spillMemorySeconds != reportedMemory || spillNear + 7.0f * spillFar + 49.0f * spillCap != reportedShape ||
			spillHoldAfterTurn + 7.0f * spillSeparateDepth + 49.0f * spillCapSceneLight + 343.0f * spillMaskSpillOnly + 2401.0f * spillHardEdges != reportedRound6) {
			reportedMemory = spillMemorySeconds;
			reportedShape = spillNear + 7.0f * spillFar + 49.0f * spillCap;
			reportedRound6 = spillHoldAfterTurn + 7.0f * spillSeparateDepth + 49.0f * spillCapSceneLight + 343.0f * spillMaskSpillOnly + 2401.0f * spillHardEdges;
			reportedTechnique = technique;
			reportedSpill = neonSpill;
			reportedLevel = spillLightLevel;
			reportedDebug = spillDebug;
			if (technique == Effect->GetTechnique(0))
				Logger::Log("UNOFFICIAL BounceLight: OffScreenLight, MultiBounce, Temporal and NeonSpill need the BounceLight.fx.hlsl that came with this DLL; running without them.");
			else Logger::Log("UNOFFICIAL BounceLight: off-screen light %.2f, extra bounces %.2f, last frame's bounce reused: %s, neon spill %.2f (reach %.2f, light level %.2f x the scene's own light, near %.2f, far %.2f, bright cap %.1f, memory %.1f s, hold after turn %.1f s, separate depth %s, cap x scene light %s, light-pixel test on the spill only %s, hard edges %s, debug view %d).",
				offScreenLight, multiBounce, historyTexture ? (halfSamples ? "yes, 4 samples a frame" : "yes, 8 samples a frame") : "no",
				neonSpill, spillReach, spillLightLevel, spillNear, spillFar, spillCap, spillMemorySeconds, spillHoldAfterTurn,
				spillSeparateDepth > 0.0f ? "on" : "off", spillCapSceneLight > 0.0f ? "on" : "off", spillMaskSpillOnly > 0.0f ? "on" : "off", spillHardEdges > 0.0f ? "on" : "off", spillDebug);
		}
	}
	const bool plus = technique != Effect->GetTechnique(0);
	D3DVIEWPORT9 original = {};
	D3DSURFACE_DESC half = {};
	if (!layoutHandle || !scene || !lowTexture[0] || !lowTexture[1] || !lowSurface[0] || !lowSurface[1] ||
		!prepTexture || !prepSurface || FAILED(Effect->GetTechniqueDesc(technique, &description)) ||
		description.Passes != 5 || FAILED(Device->GetViewport(&original)) || FAILED(lowSurface[0]->GetDesc(&half))) {
		static bool reported = false;
		if (!reported) Logger::Log("UNOFFICIAL BounceLight: buffers missing or BounceLight.fx.hlsl is not the matching version (5 passes), effect skipped.");
		reported = true;
		renderTime = 0;
		lastValid = historyValid = false;
		return;
	}
	RenderRect::WholeViewport(Device, original);	// UNOFFICIAL 2026-10-27 RenderScale v2: the layout is for the whole target
	auto timer = TimeLogger();
	FrameChain& chain = TheShaderManager->Chain;
	const bool chained = chain.Owns(RenderTarget, RenderedSurface);
	IDirect3DSurface9* finalTarget = chained ? chain.Output() : RenderTarget;
	D3DVIEWPORT9 reduced = original;
	reduced.Width = half.Width;
	reduced.Height = half.Height;
	D3DXVECTOR4 layout((float)half.Width / original.Width, (float)half.Height / original.Height,
		1.0f / half.Width, 1.0f / half.Height);
	IDirect3DSurface9* depthSurface = nullptr;
	Device->GetDepthStencilSurface(&depthSurface);
	// UNOFFICIAL ReShadeDepthCompat: leave the game's depth-stencil bound (single smaller targets are legal with it).
	if (!ReShadeIntegration::KeepDepthBound()) Device->SetDepthStencilSurface(nullptr);

	// deferCombine: passes 0-3 only (the combine is SpecularEffect::RenderWithBounce's); combineOnly: pass 4 only, from them.
	const bool onlyCombine = combineOnly && deferredReady && deferredTexture == lowTexture[0];
	// UNOFFICIAL: what last frame left can be used if its low-resolution passes ran at this resolution a moment ago, in
	// the same cell (a load or a door means another place); the depth test in the shader catches the rest.
	const double now = TheFrameRateManager->Time;
	// UNOFFICIAL 2026-10-27 RenderScale v2: last frame's buffers drawn at another scale (or full vs rect) hold other texels.
	static bool lastRect = false;
	const bool rectSame = RenderRect::Active() == lastRect && !RenderRect::ScaleChanged();
	if (!onlyCombine) lastRect = RenderRect::Active();
	// UNOFFICIAL 2026-11-01 SharedNoise: and the shared history rule (core/TemporalBackbone.h)
	const bool sharedNoise = TemporalBackbone::Shared();
	const bool continuous = lastValid && rectSame && lastQuarter == quarter && now - lastTime >= 0.0 && now - lastTime < 0.5 &&
		!TheShaderManager->GameState.isCameraCut && (!sharedNoise || TemporalBackbone::HistoryValid());
	const float frameSeconds = continuous ? (float)(now - lastTime) : 0.0f;
	EffectStatus::History(StatusSlot, Name, continuous ? "kept" : "reset (no history, rect / quarter change, gap, cut or shared rule)");	// UNOFFICIAL 2026-11-01
	// UNOFFICIAL NeonSpillHoldAfterTurn: how long the view has stood still. Turning or moving restarts it (a hair of float noise in
	// the matrices does not count as moving); a break in continuity (the gate above: quarter switch, 0.5 s gap, cell change, or a
	// failed frame, which clears lastValid) ends any hold.
	if (!onlyCombine) {
		bool moved = false;
		for (int i = 0; i < 4 && continuous && !moved; i++) for (int j = 0; j < 3 && !moved; j++)
			moved = fabsf(TheRenderManager->viewMatrix.m[i][j] - lastView.m[i][j]) >= (i < 3 ? 1.0e-5f : 0.01f);
		spillStillSeconds = !continuous ? 1.0e9f : moved ? 0.0f : spillStillSeconds + frameSeconds;
	}
	// UNOFFICIAL NeonSpill: the light pyramid first (the spill's gather reads it). With the spill, the gather applies Strength
	// to the short bounce and the combine's strength is 1; without it (off, or the pyramid failed), as before.
	bool spill = false;
	const float spillThreshold = plus && neonSpill > 0.0f && !onlyCombine ? SpillThreshold() : 0.0f;
	if (plus && neonSpill > 0.0f && !onlyCombine) {
		spill = RenderSpillPyramid(Device, scene, spillThreshold);
		if (!spill) {
			static bool reported = false;
			if (!reported) Logger::Log("UNOFFICIAL BounceLight: the neon spill's buffers or techniques are missing, or it failed to render; running without it.");
			reported = true;
		}
	}
	if (!onlyCombine) Constants.Data.x = spill ? 1.0f : strength;
	// UNOFFICIAL NeonSpillHardEdges: with the spill running, the technique's HardEdges variant (same passes and buffers; Blur and
	// Combine with the hard depth test). Missing from an older BounceLight.fx.hlsl: as before.
	D3DXHANDLE spillTechnique = technique;
	const bool hard = hardEdges || (spill && spillHardEdges > 0.0f);
	if (hard) {
		if (technique == TechniqueHandle(techPlus, "BounceLightPlus")) spillTechnique = TechniqueHandle(techPlusHard, "BounceLightPlusHardEdges");
		else if (technique == TechniqueHandle(techTemporal, "BounceLightTemporal")) spillTechnique = TechniqueHandle(techTemporalHard, "BounceLightTemporalHardEdges");
		else if (technique == TechniqueHandle(techTemporalHalf, "BounceLightTemporalHalf")) spillTechnique = TechniqueHandle(techTemporalHalfHard, "BounceLightTemporalHalfHardEdges");
		if (!spillTechnique) spillTechnique = technique;
	}

	Effect->SetTechnique(hardEdges || (spill && spillHardEdges > 0.0f) ? spillTechnique : technique);
	SetCT();
	Effect->SetVector(layoutHandle, &layout);
	const bool useHistory = historyTexture != nullptr;
	const int writeIndex = historyIndex ^ 1; // this frame's gather goes to the history buffer last frame's did not
	// (Kept for ExtPasses: the Vulkan passes take the same values.)
	D3DXVECTOR4 temporalData(0.0f, 0.0f, 0.0f, 0.0f), ambientData(0.0f, 0.0f, 0.0f, 0.0f);
	D3DXMATRIX reproject;
	D3DXMatrixIdentity(&reproject);
	if (plus && !onlyCombine) {
		static const float HistoryShare = 0.9f; // last frame's share for a spot that has not moved (less as it moves)
		temporalData = D3DXVECTOR4(useHistory && continuous && historyValid ? HistoryShare : 0.0f, continuous ? multiBounce : 0.0f,
			(float)(sharedNoise ? TemporalBackbone::EffectIndex(TemporalBackbone::OffsetBounceLight) : frameNumber % 1024), 0.0f);	// (SharedNoise: shared index, own offset)
		// The ambient light: the weather's outdoors, the cell's own indoors (as the lit particles take it); as a mid-grey
		// surface (0.25 in linear light) would send it.
		D3DXVECTOR4 ambient = TheShaderManager->ShaderConst.sunAmbient;
		if (!TheShaderManager->GameState.isExterior && Player && Player->parentCell && Player->parentCell->IsInterior() &&
			Player->parentCell->lighting)
			ambient = Player->parentCell->lighting->ambient.toD3DXVECTOR4();
		ambientData = D3DXVECTOR4(0.25f * BounceLinear(ambient.x), 0.25f * BounceLinear(ambient.y), 0.25f * BounceLinear(ambient.z), offScreenLight);
		if (continuous) BounceLightReprojection(TheRenderManager->viewMatrix, TheRenderManager->InvViewMatrix, lastView, lastInvView, lastProj, &reproject);
		// UNOFFICIAL 2026-11-09 TemporalJitterFix: the gather was drawn from this frame's jittered depth, the history from last frame's
		// (LastFrame() in BounceLight.fx.hlsl and Compute/BounceLight.glsl read the history where the jitter's change puts it)
		if (continuous && jitterFix) TemporalBackbone::ShiftReprojection(reproject, TemporalBackbone::DepthJitter() - lastJitter);
		Effect->SetVector(temporalHandle, &temporalData);
		Effect->SetVector(ambientHandle, &ambientData);
		Effect->SetMatrix(reprojectHandle, &reproject);
		// UNOFFICIAL NeonSpill: w = how many times the screen's average brightness a light must reach (BounceLight.fx.hlsl).
		D3DXVECTOR4 spillData(spill ? neonSpill : 0.0f, spill ? strength : 1.0f, spillReach, spillLightLevel);
		if (spillHandle) Effect->SetVector(spillHandle, &spillData);
	}
	// UNOFFICIAL NeonSpill (B-15): every other frame (the first technique, every extra off; or the combine alone) the spill does not
	// run, so NVR_BounceSpill gets its "off" value. It was left as the last spill-on frame set it, which switched on Combine's and
	// Blur's spill-only parts in the first technique.
	else if (D3DXHANDLE offHandle = ParameterHandle(paramSpill, "NVR_BounceSpill")) {
		D3DXVECTOR4 spillOff(0.0f, 1.0f, spillReach, spillLightLevel);
		Effect->SetVector(offHandle, &spillOff);
	}
	// UNOFFICIAL NeonSpill: the spill's own gather with its memory: last frame's kept (0.85 of it where the spot is found
	// again), and while the view moves, no faster fall than over NeonSpillMemory seconds. If it fails, the frame runs
	// without the spill, as before.
	IDirect3DTexture9* spillResult = nullptr;
	if (spill) {
		D3DXHANDLE memoryHandle = ParameterHandle(paramSpillMemory, "NVR_SpillMemory");
		const bool memory = continuous && spillMemoryValid;
		// z: NeonSpillHoldAfterTurn, 1 until the view has stood still that long (the hold then also works on still pixels).
		const float holdAfterTurn = memory && spillHoldAfterTurn > 0.0f && spillStillSeconds < spillHoldAfterTurn ? 1.0f : 0.0f;
		D3DXVECTOR4 memoryData(memory ? 0.85f : 0.0f, memory && spillMemorySeconds > 0.0f ? expf(-frameSeconds / spillMemorySeconds) : 0.0f, holdAfterTurn, spillThreshold);
		if (memoryHandle) Effect->SetVector(memoryHandle, &memoryData);
		// UNOFFICIAL round 6: the pyramid's level 0 size (NeonSpillSeparateDepth) and NeonSpillMaskSpillOnly, for the spill's gather,
		// GatherPlus and Combine (BounceLight.fx.hlsl's NVR_SpillOptions; only read with the spill on).
		D3DSURFACE_DESC level0 = {};
		if (D3DXHANDLE optionsHandle = ParameterHandle(paramSpillOptions, "NVR_SpillOptions")) if (SUCCEEDED(spillSurface[0]->GetDesc(&level0))) {
			// w: the light-pixel test's level for NeonSpillMaskSpillOnly, 1.5 x the scene's own light (the default light test).
			D3DXVECTOR4 options((float)level0.Width, (float)level0.Height, spillMaskSpillOnly > 0.0f ? 1.0f : 0.0f,
				spillMaskSpillOnly > 0.0f ? 1.5f * spillThreshold / spillLightLevel : 0.0f);
			Effect->SetVector(optionsHandle, &options);
		}
		spillResult = memoryHandle ? RenderSpillGather(Device, quarter, reduced) : nullptr;
		if (!spillResult) {
			static bool reported = false;
			if (!reported) Logger::Log("UNOFFICIAL BounceLight: the neon spill's gather failed; running without it.");
			reported = true;
			spill = false;
			spillMemoryValid = false;
			Constants.Data.x = strength;
			D3DXVECTOR4 spillData(0.0f, 1.0f, spillReach, spillLightLevel);
			if (spillHandle) Effect->SetVector(spillHandle, &spillData);
		}
		Effect->SetTechnique(hardEdges || (spill && spillHardEdges > 0.0f) ? spillTechnique : technique);
		SetCT();
	}
	else spillMemoryValid = false;
	// UNOFFICIAL NeonSpill debug view (in the combine; 0 whenever the spill did not run, as before).
	const int debugView = spill || onlyCombine ? spillDebug * (neonSpill > 0.0f) : 0;
	if (D3DXHANDLE debugHandle = ParameterHandle(paramSpillDebug, "NVR_SpillDebug")) {
		D3DXVECTOR4 debugData((float)debugView, 0.0f, 0.0f, 0.0f);
		Effect->SetVector(debugHandle, &debugData);
	}
	// Separate timers per resolution, so the log tells the two apart.
	static GpuTimer halfTimers[5] = { GpuTimer("  Bounce prepare (half)"), GpuTimer("  Bounce gather (half)"),
		GpuTimer("  Bounce blur X (half)"), GpuTimer("  Bounce blur Y (half)"), GpuTimer("  Bounce combine (full)") };
	static GpuTimer quarterTimers[5] = { GpuTimer("  Bounce prepare (quarter)"), GpuTimer("  Bounce gather (quarter)"),
		GpuTimer("  Bounce blur X (quarter)"), GpuTimer("  Bounce blur Y (quarter)"), GpuTimer("  Bounce combine (full, from quarter)") };
	GpuTimer* passTimers = quarter ? quarterTimers : halfTimers;
	const UINT firstPass = onlyCombine ? 4 : 0, endPass = deferCombine ? 4 : 5;
	UINT passes = 0;
	// UNOFFICIAL 2026-10-25 (NVR DXVK): the same passes as Vulkan passes when NvrDxvkPasses is on (not with the retired spill's buffers);
	// if they cannot draw, this D3D9 loop draws the frame as before.
	const D3DXHANDLE drawnTechnique = hardEdges || (spill && spillHardEdges > 0.0f) ? spillTechnique : technique;
	const bool extDrawn = !spill && !debugView && !ExtFailed && DxvkCompute::PassesOn() &&
		ExtPasses(Device, RenderTarget, RenderedSurface, drawnTechnique, firstPass, endPass, scene, lowTexture, lowSurface, prepTexture, prepSurface,
			historyTexture, historySurface, writeIndex, layout, temporalData, ambientData, reproject, passTimers);
	HRESULT result = extDrawn ? S_OK : Effect->Begin(&passes, 0);
	if (!extDrawn && SUCCEEDED(result)) {
		for (UINT p = firstPass; p < endPass && SUCCEEDED(result); ++p) {
			const bool combine = p == 4;
			// Prepare writes the packed buffer; the gather reads it and writes 0, blur X reads 0 and writes 1,
			// blur Y reads 1 and writes 0, the combine reads 0.
			// UNOFFICIAL: with the other techniques Prepare also reads 0 (last frame's result: the extra bounces); with
			// Temporal the gather also reads last frame's history buffer and writes the other one, which blur X reads.
			IDirect3DSurface9* destination = combine ? finalTarget : p == 0 ? prepSurface :
				p == 1 && useHistory ? historySurface[writeIndex] : lowSurface[p == 2 ? 1 : 0];
			Device->SetTexture(5, nullptr);
			Device->SetTexture(6, nullptr);
			if (plus) Device->SetTexture(7, nullptr);
			result = Device->SetRenderTarget(0, destination);
			if (SUCCEEDED(result)) result = Device->SetViewport(combine ? &original : &reduced);
			if (SUCCEEDED(result)) result = Effect->BeginPass(p);
			if (FAILED(result)) break;
			if (p == 0 && plus) Device->SetTexture(7, lowTexture[0]); // NVR_BounceHistory: last frame's result
			if (p == 1) {
				Device->SetTexture(6, prepTexture);
				if (useHistory) Device->SetTexture(7, historyTexture[historyIndex]); // NVR_BounceHistory: last frame's gather
			}
			else if (p == 2 && useHistory) Device->SetTexture(5, historyTexture[writeIndex]);
			else if (p == 2 || p == 4) Device->SetTexture(5, lowTexture[0]);
			else if (p == 3) Device->SetTexture(5, lowTexture[1]);
			// UNOFFICIAL NeonSpill, at NVR_Spill0's stage: the spill's buffer for the gather; the pyramid's first level for
			// the combine's debug view 1; nothing otherwise.
			Device->SetTexture(8, p == 1 && spill ? spillResult : combine && debugView == 1 ? spillTexture[0] : nullptr);
			Device->SetTexture(2, scene); // TESR_SourceBuffer slot: the current image
			RenderRect::ApplyViewport(Device);	// UNOFFICIAL 2026-10-27 RenderScale v2: the low buffers and the frame keep to their rect
			{
				GpuProfileScope gpu(passTimers[p], Device);
				result = Device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
			}
			HRESULT endResult = Effect->EndPass();
			if (SUCCEEDED(result)) result = endResult;
		}
		Effect->End();
	}
	Device->SetTexture(5, nullptr);
	Device->SetTexture(6, nullptr);
	if (plus) Device->SetTexture(7, nullptr);
	if (spill || debugView) for (int s = 0; s < SpillLevels; s++) Device->SetTexture(8 + s, nullptr);
	Device->SetTexture(2, TheTextureManager->SourceTexture);
	// UNOFFICIAL: what this frame's low-resolution passes left is next frame's "last frame".
	if (SUCCEEDED(result) && firstPass == 0) {
		lastView = TheRenderManager->viewMatrix;
		lastInvView = TheRenderManager->InvViewMatrix;
		lastProj = TheRenderManager->projMatrix;
		lastJitter = TemporalBackbone::DepthJitter();	// UNOFFICIAL 2026-11-09 TemporalJitterFix
		lastValid = true;
		lastQuarter = quarter;
		lastTime = now;
		frameNumber++;
		historyValid = useHistory;
		if (useHistory) historyIndex = writeIndex;
		// UNOFFICIAL NeonSpill: this frame's spill buffer is next frame's memory.
		if (spillResult) { spillMemoryIndex ^= 1; spillMemoryValid = true; }
	}
	else if (FAILED(result)) lastValid = historyValid = spillMemoryValid = false;
	Device->SetRenderTarget(0, RenderTarget);
	Device->SetViewport(&original);
	Device->SetDepthStencilSurface(depthSurface);
	if (depthSurface) depthSurface->Release();
	if (SUCCEEDED(result)) {
		if (endPass == 4) {
			deferredReady = true;
			deferredTexture = lowTexture[0];
			deferredLayout = layout;
		}
		else if (extDrawn) {}	// ExtFullScreen committed the combine (or made the copy) itself
		else if (chained) chain.Commit();
		else Device->StretchRect(RenderTarget, NULL, RenderedSurface, NULL, D3DTEXF_NONE);
	}
	else {
		static bool reported = false;
		if (!reported) Logger::Log("UNOFFICIAL BounceLight: render failed (%08lx).", result);
		reported = true;
	}
	renderTime = timer.LogTime("BounceLight::Render");
}
