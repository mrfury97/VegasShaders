#include "Normals.h"
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-25 (NVR DXVK)
#include "../../core/RenderRect.h"	// UNOFFICIAL 2026-10-27 RenderScale v2

// The constant's registered memory: the same one SetCT uploads from (a matrix is the D3DXMATRIX as stored).
static const D3DXVECTOR4* Registered(const char* name) {
	auto found = TheShaderManager->ConstantsTable.find(name);
	return found == TheShaderManager->ConstantsTable.end() ? nullptr : found->second;
}

void NormalsEffect::RegisterConstants() {
	TheShaderManager->RegisterConstant("TESR_NormalsData", &Constants.Data);
	TheShaderManager->RegisterConstant("TESR_NormalsSmoothData", &SmoothData);	// UNOFFICIAL 2026-10-23 (sharednormals)
}

void NormalsEffect::UpdateConstants() {
	SmoothData.x = (float)max(0, min(TheSettingManager->SettingsMain.Main.SmoothNormals, 256));	// UNOFFICIAL 2026-10-23 (sharednormals), live
}

// UNOFFICIAL 2026-10-23 (sharednormals): see Normals.h. Pass X reads TESR_NormalsBuffer (stage 1) into the spare texture, pass Y reads
// the spare (stage 2) back into TESR_NormalsBuffer; the stage that is also the pass's target is unbound first. Nothing drawn (false)
// when off, when the effect is missing, or when a draw fails (logged once).
bool NormalsEffect::RenderSmooth(IDirect3DDevice9* Device) {
	smoothed = false;
	const int reach = TheSettingManager->SettingsMain.Main.SmoothNormals;
	if (reach <= 0 || !Enabled || !Effect || !ShouldRender() || !Textures.NormalsTexture || !Textures.NormalsSurface) return false;
	if (!smoothTried) {
		smoothTried = true;
		TheTextureManager->InitTexture("TESR_NormalsSmoothTemp", &SmoothTempTexture, &SmoothTempSurface, TheRenderManager->width, TheRenderManager->height, D3DFMT_A16B16G16R16F);
	}
	D3DXHANDLE technique = Effect->GetTechnique(0);
	D3DXHANDLE smooth = TechniqueHandle(techSmooth, "NormalsSmooth");
	D3DXTECHNIQUE_DESC description = {};
	if (!technique || !smooth || !SmoothTempTexture || !SmoothTempSurface || FAILED(Effect->GetTechniqueDesc(smooth, &description)) || description.Passes != 2) return false;

	static GpuTimer timer("  Normal smoothing (shared)");
	GpuProfileScope gpu(timer, Device);
	// UNOFFICIAL 2026-10-25 (NVR DXVK): both passes as Vulkan passes (ExtSmooth); the D3D9 passes below when that is off or fails.
	bool drawn = DxvkCompute::PassesOn() && !ExtFailed && ExtSmooth(Device);
	if (!drawn) {
		IDirect3DSurface9* target = nullptr;
		Device->GetRenderTarget(0, &target);
		Effect->SetTechnique(smooth);
		SetCT();
		UINT passes = 0;
		HRESULT result = Effect->Begin(&passes, 0);
		if (SUCCEEDED(result)) {
			for (UINT p = 0; p < 2 && SUCCEEDED(result); ++p) {
				IDirect3DTexture9* read = p == 0 ? Textures.NormalsTexture : SmoothTempTexture;
				IDirect3DSurface9* write = p == 0 ? SmoothTempSurface : Textures.NormalsSurface;
				Device->SetTexture(1, p == 0 ? read : nullptr);
				Device->SetTexture(2, p == 1 ? read : nullptr);
				TheRenderManager->renderState->NoteDeviceTexture(1, p == 0 ? read : nullptr);
				TheRenderManager->renderState->NoteDeviceTexture(2, p == 1 ? read : nullptr);
				result = Device->SetRenderTarget(0, write);
				if (SUCCEEDED(result)) result = Effect->BeginPass(p);
				if (FAILED(result)) break;
				RenderRect::ApplyViewport(Device);	// UNOFFICIAL 2026-10-27 RenderScale v2
				result = Device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
				Effect->EndPass();
			}
			Effect->End();
		}
		Effect->SetTechnique(technique);
		Device->SetTexture(1, nullptr);
		Device->SetTexture(2, nullptr);
		TheRenderManager->renderState->NoteDeviceTexture(1, nullptr);
		TheRenderManager->renderState->NoteDeviceTexture(2, nullptr);
		Device->SetRenderTarget(0, target);
		if (target) target->Release();
		drawn = SUCCEEDED(result);
	}
	smoothed = drawn;
	static int reported = -1;
	if ((smoothed ? 1 : 0) != reported) {
		reported = smoothed ? 1 : 0;
		Logger::Log("UNOFFICIAL SmoothNormals: %s", smoothed ? "on (TESR_NormalsBuffer smoothed in place after the normals pass; AmbientLight's own NormalSmoothing skipped)" : "NOT drawn (a draw failed)");
	}
	return smoothed;
}

// UNOFFICIAL 2026-10-25 (NVR DXVK): NormalsSmooth.frag, pass X (alongY 0) reads TESR_NormalsBuffer into the spare texture, pass Y
// (alongY 1) reads it back into TESR_NormalsBuffer. The same two draws as the D3D9 passes; false when one fails (the D3D9 passes
// then run and write both again). Sampler and depth as the .fx declares them (POINT, CLAMP; the depth is CombineDepth's).
bool NormalsEffect::ExtSmooth(IDirect3DDevice9* Device) {
	IDirect3DTexture9* depth = TheShaderManager->Effects.CombineDepth ? TheShaderManager->Effects.CombineDepth->Textures.CombinedDepthTexture : nullptr;
	const D3DXVECTOR4* reciprocal = Registered("TESR_ReciprocalResolution");
	const D3DXVECTOR4* cameraData = Registered("TESR_CameraData");
	const D3DXVECTOR4* projection = Registered("TESR_ProjectionTransform");
	if (!depth || !Textures.NormalsTexture || !Textures.NormalsSurface || !SmoothTempTexture || !SmoothTempSurface || !reciprocal || !cameraData || !projection) return false;

	struct Push { D3DXVECTOR4 reciprocalResolution; D3DXVECTOR4 smoothData; D3DXVECTOR4 cameraData; D3DXMATRIX projection; UInt32 samplerIndex; UInt32 alongY; } push = {};
	push.reciprocalResolution = *reciprocal;
	push.smoothData = SmoothData;
	push.cameraData = *cameraData;
	memcpy(&push.projection, projection, sizeof(D3DXMATRIX));
	for (UINT pass = 0; pass < 2; pass++) {
		push.alongY = pass;
		IDirect3DBaseTexture9* textures[] = { pass == 0 ? Textures.NormalsTexture : SmoothTempTexture, depth };
		IDirect3DSurface9* target = pass == 0 ? SmoothTempSurface : Textures.NormalsSurface;
		if (!ExtTexturePass(Device, target, nullptr, nullptr, extPassSmooth, "NormalsSmooth", textures, _countof(textures), &push, sizeof(push), 112, true)) return false;
	}
	return true;
}

// UNOFFICIAL 2026-10-25 (NVR DXVK): Normals.frag, technique 0. Drawn into the normals buffer (the caller's target), as the D3D9 pass.
// A failed draw sets ExtFailed, which turns the smoothing's Vulkan passes off too (the same effect object).
bool NormalsEffect::ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	if (techniqueIndex != 0 || ClearRenderTarget) return false;
	IDirect3DBaseTexture9* depth = TheShaderManager->Effects.CombineDepth ? TheShaderManager->Effects.CombineDepth->Textures.CombinedDepthTexture : nullptr;
	const D3DXVECTOR4* reciprocal = Registered("TESR_ReciprocalResolution");
	const D3DXVECTOR4* cameraData = Registered("TESR_CameraData");
	const D3DXVECTOR4* invProjection = Registered("TESR_InvProjectionTransform");
	if (!depth || !reciprocal || !cameraData || !invProjection) return false;

	struct Push { D3DXVECTOR4 reciprocalResolution; D3DXVECTOR4 cameraData; D3DXMATRIX invProjection; UInt32 samplerIndex; UInt32 pad[3]; } push = {};
	push.reciprocalResolution = *reciprocal;
	push.cameraData = *cameraData;
	memcpy(&push.invProjection, invProjection, sizeof(D3DXMATRIX));
	IDirect3DBaseTexture9* textures[] = { depth };
	return ExtTexturePass(Device, RenderTarget, RenderedSurface, SourceBuffer, extPass, "Normals", textures, _countof(textures), &push, sizeof(push), 96, true);
}
	
void NormalsEffect::RegisterTextures() {
	TheTextureManager->InitTexture("TESR_NormalsBuffer", &Textures.NormalsTexture, &Textures.NormalsSurface, TheRenderManager->width, TheRenderManager->height, D3DFMT_A16B16G16R16F);
	// UNOFFICIAL 2026-11-12: the spare texture is made on first use (RenderSmooth), but the effect file names its sampler at load, which
	// logged "[ERROR] Texture TESR_NormalsSmoothTemp not found" in every session. The name is known from the start now (an empty slot).
	TheTextureManager->RegisterTexture("TESR_NormalsSmoothTemp", (IDirect3DBaseTexture9**)&SmoothTempTexture);
}