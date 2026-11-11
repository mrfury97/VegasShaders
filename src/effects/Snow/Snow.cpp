#include "Snow.h"

void SnowEffect::UpdateConstants() {
	Animator& fall = Constants.SnowAnimator;
	const bool snowing = TheShaderManager->GameState.isSnow;

	if (snowing && !fall.switched) {
		// snow has just begun: puddles fade out, the flakes ramp up from nothing
		TheShaderManager->Effects.WetWorld->Constants.PuddlesAnimator.Start(0.3f, 0.f);
		fall.switched = true;
		fall.Initialize(0.f);
		fall.Start(0.5f, 1.f);
	}
	else if (!snowing && fall.switched) {
		// snow has just ended: fade the flakes out
		fall.switched = false;
		fall.Start(0.2f, 0.f);
	}
	Constants.Data.x = fall.GetValue();

	// the snow needs the top-down (ortho) shadow map while it is visible
	if (Constants.Data.x) TheShaderManager->orthoRequired = true;
}

void SnowEffect::UpdateSettings() {
	const char* section = "Shaders.Snow.Main";
	Constants.Data.z = TheSettingManager->GetSettingF(section, "Speed");

	// UNOFFICIAL 2026-11-09 WorldSpace (macintroll's world-space snow; Snow.fx.hlsl techniques 1 and 2). All live.
	// The limits are the shader's: the flake radius is capped at 4.5 % of the 64-unit cell (2.88) after its 0.65-1.35 spread, and the
	// ray walks at most 40 cells, which covers (40 - 3) * 64 / 1.733 = 1366 units.
	WorldSpace = TheSettingManager->GetSettingI(section, "WorldSpace") != 0;
	WorldSurfaceMarks = TheSettingManager->GetSettingI(section, "WorldSurfaceMarks") != 0;
	Constants.WorldData.x = std::clamp(TheSettingManager->GetSettingF(section, "WorldFlakeSize"), 0.5f, 3.0f);
	Constants.WorldData.y = std::clamp(TheSettingManager->GetSettingF(section, "WorldDensity"), 0.0f, 1.0f);
	Constants.WorldData.z = std::clamp(TheSettingManager->GetSettingF(section, "WorldDistance"), 200.0f, 1300.0f);
	Constants.WorldData.w = std::clamp(TheSettingManager->GetSettingF(section, "WorldOpacity"), 0.0f, 1.0f);
	const float cloudWind = std::clamp(TheSettingManager->GetSettingF(section, "WorldCloudWind"), 0.0f, 2.0f);
	Constants.WorldWind.x = cloudWind;
	// The flake grid repeats every this many cells, so that one whole wrap of the clouds' offset (900000 units) moves the snow by
	// a whole number of repeats (his WS_CloudRepeatCells, here because the strength is no longer a compile-time constant).
	Constants.WorldWind.y = (std::max)(1.0f, floorf(8192.0f * cloudWind + 0.5f));
	Constants.WorldWind.z = std::clamp(TheSettingManager->GetSettingF(section, "WorldPlantWind"), 0.0f, 3.0f);
	Constants.WorldWind.w = 0.0f;
}

UINT SnowEffect::Technique() {
	if (!WorldSpace || !Effect) return 0;
	// The marks read the Normals effect's buffer: without it, the flakes alone.
	const NormalsEffect* normals = TheShaderManager->Effects.Normals;
	const UINT wanted = WorldSurfaceMarks && normals && normals->Enabled && normals->Effect ? 2 : 1;
	return Effect->GetTechnique(wanted) ? wanted : 0;	// (an older Snow.fx.hlsl in the game folder: the old snow)
}

void SnowEffect::RegisterConstants() {
	ShaderManager* sm = TheShaderManager;
	sm->RegisterConstant("TESR_SnowData", &Constants.Data);
	sm->RegisterConstant("TESR_SnowWorldData", &Constants.WorldData);	// UNOFFICIAL 2026-11-09 WorldSpace
	sm->RegisterConstant("TESR_SnowWorldWind", &Constants.WorldWind);
}

bool SnowEffect::ShouldRender() {
	const auto& game = TheShaderManager->GameState;
	if (game.isUnderwater || !game.isExterior) return false;
	return Constants.Data.x > 0.0f;
};

// UNOFFICIAL 2026-10-25 (NVR DXVK): the constants are read from the same registered memory the D3D9 path uploads
// (ConstantsTable; SetPixelShaderConstantF copies it raw), so the pass sees the registers the technique sees.
static const D3DXVECTOR4* RegisteredConstant(const char* name) {
	auto found = TheShaderManager->ConstantsTable.find(name);
	return found == TheShaderManager->ConstantsTable.end() ? nullptr : found->second;
}

// UNOFFICIAL 2026-10-25 (NVR DXVK): Snow.frag; the three textures and the constants the technique reads (EffectRecord::ExtTexturePass).
// Push block (196 bytes, padded to 208 like the shader's block): the registers of the two matrices the shader reads (the
// shadow matrix's four, the view matrix's first three: toWorld reads no more), then the float4s, then the sampler index.
bool SnowEffect::ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	// UNOFFICIAL 2026-11-09: only technique 0 has a Vulkan twin. WorldSpace (techniques 1 and 2) has none yet, so on the DXVK fork
	// it falls back to the D3D9 effect here (false = Render draws the technique as D3D9 passes).
	if (techniqueIndex != 0) return false;
	const D3DXVECTOR4* shadowToLight = RegisteredConstant("TESR_ShadowCameraToLightTransformOrtho");
	const D3DXVECTOR4* view = RegisteredConstant("TESR_ViewTransform");
	const D3DXVECTOR4* projection = RegisteredConstant("TESR_ProjectionTransform");
	const D3DXVECTOR4* cameraData = RegisteredConstant("TESR_CameraData");
	const D3DXVECTOR4* gameTime = RegisteredConstant("TESR_GameTime");
	const D3DXVECTOR4* sunColor = RegisteredConstant("TESR_SunColor");
	const D3DXVECTOR4* sunAmbient = RegisteredConstant("TESR_SunAmbient");
	if (!shadowToLight || !view || !projection || !cameraData || !gameTime || !sunColor || !sunAmbient) return false;

	struct Push {
		D3DXVECTOR4 shadowToLight[4];	// 0: TESR_ShadowCameraToLightTransformOrtho, registers 0-3
		D3DXVECTOR4 view[3];			// 64: TESR_ViewTransform, columns 0-2 (HLSL M[i][j] is element (i+1, j+1): SetMatrix)
		D3DXVECTOR4 gameTime;			// 112: TESR_GameTime
		D3DXVECTOR4 snowData;			// 128: TESR_SnowData
		D3DXVECTOR4 sunColor;			// 144: TESR_SunColor
		D3DXVECTOR4 sunAmbient;			// 160: TESR_SunAmbient
		D3DXVECTOR4 projectionFar;		// 176: x TESR_ProjectionTransform[0][0], y [1][1], z TESR_CameraData.y
		UInt32		samplerIndex;		// 192: LINEAR, CLAMP (filled in by ExtTexturePass)
		UInt32		pad[3];
	} push = {};
	memcpy(push.shadowToLight, shadowToLight, sizeof(push.shadowToLight));
	for (int c = 0; c < 3; c++)	// 2026-10-25: the columns toWorld reads (the memory rows were a transposed view ray)
		push.view[c] = D3DXVECTOR4(((const float*)&view[0])[c], ((const float*)&view[1])[c], ((const float*)&view[2])[c], ((const float*)&view[3])[c]);
	push.gameTime = *gameTime;
	push.snowData = Constants.Data;
	push.sunColor = *sunColor;
	push.sunAmbient = *sunAmbient;
	push.projectionFar = D3DXVECTOR4(projection[0].x, projection[1].y, cameraData->y, 0.0f);

	IDirect3DBaseTexture9* textures[] = {
		TheTextureManager->RenderedTexture,
		TheShaderManager->Effects.CombineDepth ? TheShaderManager->Effects.CombineDepth->Textures.CombinedDepthTexture : nullptr,
		TheShaderManager->Effects.ShadowsExteriors ? TheShaderManager->Effects.ShadowsExteriors->ShadowMapOrthoTexture : nullptr };
	return ExtTexturePass(Device, RenderTarget, RenderedSurface, SourceBuffer, extPass, "Snow", textures, _countof(textures), &push, sizeof(push), 192);
}