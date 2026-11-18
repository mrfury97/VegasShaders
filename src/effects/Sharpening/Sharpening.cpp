#include "Sharpening.h"
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-23 (NVR DXVK)

// UNOFFICIAL: outdoors ([Shaders.Sharpening.Main]) and indoors ([Shaders.Sharpening.Interiors]) each have their own
// values; the one for the current cell is published every frame.
void SharpeningEffect::UpdateConstants() {
	Constants.Data = TheShaderManager->GameState.isExterior ? Exterior : Interior;
}

void SharpeningEffect::UpdateSettings(){
	Exterior = D3DXVECTOR4(TheSettingManager->GetSettingF("Shaders.Sharpening.Main", "Strength"),
		TheSettingManager->GetSettingF("Shaders.Sharpening.Main", "Clamp"),
		TheSettingManager->GetSettingF("Shaders.Sharpening.Main", "Offset"), 0.0f);
	Interior = D3DXVECTOR4(TheSettingManager->GetSettingF("Shaders.Sharpening.Interiors", "Strength"),
		TheSettingManager->GetSettingF("Shaders.Sharpening.Interiors", "Clamp"),
		TheSettingManager->GetSettingF("Shaders.Sharpening.Interiors", "Offset"), 0.0f);
	Constants.Data = TheShaderManager->GameState.isExterior ? Exterior : Interior;
}

void SharpeningEffect::RegisterConstants(){
	TheShaderManager->RegisterConstant("TESR_SharpeningData", &Constants.Data);
}

// UNOFFICIAL 2026-10-23 (NVR DXVK): Sharpening.frag; the textures and constants the technique reads (EffectRecord::ExtTexturePass).
bool SharpeningEffect::ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	if (techniqueIndex != 0) return false;
	struct Push { D3DXVECTOR4 data, reciprocalResolution, cameraData; UInt32 samplerIndex; UInt32 pad[3]; } push = {};
	push.data = Constants.Data;
	push.reciprocalResolution = TheShaderManager->ShaderConst.ReciprocalResolution;
	push.cameraData = TheRenderManager->CameraData;
	IDirect3DBaseTexture9* textures[] = { TheTextureManager->RenderedTexture, TheShaderManager->Effects.CombineDepth ? TheShaderManager->Effects.CombineDepth->Textures.CombinedDepthTexture : nullptr };
	return ExtTexturePass(Device, RenderTarget, RenderedSurface, SourceBuffer, extPass, "Sharpening", textures, _countof(textures), &push, sizeof(push), 48);
}

