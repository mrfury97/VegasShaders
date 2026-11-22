#include "CombineDepth.h"
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-25 (NVR DXVK)
#include "../../core/RenderRect.h"	// UNOFFICIAL 2026-10-27 RenderScale v2
#include "../../core/CapturedNormals.h"

// The constant's registered memory: the same one SetCT uploads from (a matrix is the D3DXMATRIX as stored).
static const D3DXVECTOR4* Registered(const char* name) {
	auto found = TheShaderManager->ConstantsTable.find(name);
	return found == TheShaderManager->ConstantsTable.end() ? nullptr : found->second;
}

void CombineDepthEffect::UpdateConstants() {
	NiCamera* Camera = WorldSceneGraph->camera;

	if (!johnnyguitar || !JG_GetClipDist) {
		Constants.viewNearZ = Camera->Frustum.Near;
	}
	else {
		float nearZ = JG_GetClipDist();

		Constants.viewNearZ = nearZ ? max(nearZ, 0.3)  : Camera->Frustum.Near;
	}
}

void CombineDepthEffect::RegisterTextures() {
	// Effects recompute the post-projection depth from the linear channel (Depth.hlsl
	// projectedDepthFromLinear), so a single 32-bit channel carries everything they read.
	const D3DFORMAT format = TheSettingManager->SettingsMain.Main.SlimDepthBuffer ? D3DFMT_R32F : D3DFMT_G32R32F;
	TheTextureManager->InitTexture("TESR_DepthBuffer", &Textures.CombinedDepthTexture, &Textures.CombinedDepthSurface, TheRenderManager->width, TheRenderManager->height, format);
	Logger::Log("UNOFFICIAL depth buffer: %s.", format == D3DFMT_R32F ? "R32F (single channel)" : "G32R32F");
}

// ---- One-off check of the merged draw (roadmap I3) ----
// Every call in the merged path returns success, yet on a GTX 1070 (native D3D9) its targets stayed stale:
// the retail runtime returns S_OK even for a draw the driver rejects. So the first time the merged path
// runs in a session, the normals target is filled with a marker colour, and its centre pixel is read back
// before and after the draw (a one-off stall of a few milliseconds). Marker still there afterwards = the
// draw never ran; the merged path then switches itself off (separate passes until restart). The render
// states the draw ran with are logged too, to show which D3D9 rule it may have broken.
static const char* SurfaceFormatName(D3DFORMAT format) {
	switch (format) {
	case D3DFMT_R32F: return "R32F";
	case D3DFMT_G32R32F: return "G32R32F";
	case D3DFMT_A16B16G16R16F: return "A16B16G16R16F";
	case D3DFMT_A32B32G32R32F: return "A32B32G32R32F";
	case D3DFMT_A8R8G8B8: return "A8R8G8B8";
	case D3DFMT_X8R8G8B8: return "X8R8G8B8";
	case D3DFMT_D24S8: return "D24S8";
	case D3DFMT_D24X8: return "D24X8";
	case D3DFMT_D16: return "D16";
	case (D3DFORMAT)MAKEFOURCC('I', 'N', 'T', 'Z'): return "INTZ";
	default: return "other";
	}
}

// Centre pixel of an A16B16G16R16F render target, through a 1x1 copy (the GPU finishes the frame so far).
static bool ReadCentreTexel(IDirect3DDevice9* Device, IDirect3DSurface9* surface, const D3DSURFACE_DESC& desc, float texel[4]) {
	if (desc.Format != D3DFMT_A16B16G16R16F) return false;
	IDirect3DSurface9* probe = nullptr;
	IDirect3DSurface9* readable = nullptr;
	bool read = false;
	if (SUCCEEDED(Device->CreateRenderTarget(1, 1, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &probe, NULL)) &&
		SUCCEEDED(Device->CreateOffscreenPlainSurface(1, 1, desc.Format, D3DPOOL_SYSTEMMEM, &readable, NULL))) {
		const LONG cx = (LONG)RenderRect::Width(desc.Width) / 2, cy = (LONG)RenderRect::Height(desc.Height) / 2;
		const RECT centre = { cx, cy, cx + 1, cy + 1 };
		D3DLOCKED_RECT locked = {};
		if (SUCCEEDED(Device->StretchRect(surface, &centre, probe, NULL, D3DTEXF_NONE)) &&
			SUCCEEDED(Device->GetRenderTargetData(probe, readable)) &&
			SUCCEEDED(readable->LockRect(&locked, NULL, D3DLOCK_READONLY))) {
			D3DXFloat16To32Array(texel, (const D3DXFLOAT16*)locked.pBits, 4);
			readable->UnlockRect();
			read = true;
		}
	}
	if (probe) probe->Release();
	if (readable) readable->Release();
	return read;
}

static bool IsMarker(const float texel[4]) {
	// D3DCOLOR_ARGB(0x40, 0x20, 0x10, 0x08) converted to floats; the half-float rounding is far below 0.002.
	const float marker[4] = { 0x20 / 255.0f, 0x10 / 255.0f, 0x08 / 255.0f, 0x40 / 255.0f };
	for (int i = 0; i < 4; ++i)
		if (fabsf(texel[i] - marker[i]) > 0.002f) return false;
	return true;
}

static void LogMergedState(IDirect3DDevice9* Device, const D3DCAPS9& caps, const D3DSURFACE_DESC& depthDesc, const D3DSURFACE_DESC& normalsDesc, IDirect3DSurface9* depthStencil) {
	const D3DRENDERSTATETYPE states[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_STENCILENABLE, D3DRS_ALPHATESTENABLE, D3DRS_ALPHABLENDENABLE,
		D3DRS_FOGENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_COLORWRITEENABLE, D3DRS_COLORWRITEENABLE1 };
	DWORD value[10] = {};
	for (int i = 0; i < 10; ++i)
		if (FAILED(Device->GetRenderState(states[i], &value[i]))) value[i] = 0xFFFFFFFF;
	D3DVIEWPORT9 viewport = {};
	Device->GetViewport(&viewport);
	D3DSURFACE_DESC ds = {};
	if (depthStencil) depthStencil->GetDesc(&ds);
	Logger::Log("UNOFFICIAL merged depth/normals state: RT0 %ux%u %s, RT1 %ux%u %s, game depth-stencil (unbound for the draw) %ux%u %s multisample %u | "
		"caps: %lu targets, independent bit depths %d, post-shader blending %d, independent write masks %d",
		depthDesc.Width, depthDesc.Height, SurfaceFormatName(depthDesc.Format), normalsDesc.Width, normalsDesc.Height, SurfaceFormatName(normalsDesc.Format),
		ds.Width, ds.Height, depthStencil ? SurfaceFormatName(ds.Format) : "none", (unsigned)ds.MultiSampleType, caps.NumSimultaneousRTs,
		(caps.PrimitiveMiscCaps & D3DPMISCCAPS_MRTINDEPENDENTBITDEPTHS) ? 1 : 0, (caps.PrimitiveMiscCaps & D3DPMISCCAPS_MRTPOSTPIXELSHADERBLENDING) ? 1 : 0,
		(caps.PrimitiveMiscCaps & D3DPMISCCAPS_INDEPENDENTWRITEMASKS) ? 1 : 0);
	Logger::Log("UNOFFICIAL merged depth/normals state: z %lu, z write %lu, stencil %lu, alpha test %lu, blend %lu, fog %lu, sRGB write %lu, scissor %lu, "
		"write mask RT0 %lx RT1 %lx, viewport %lu,%lu %lux%lu",
		value[0], value[1], value[2], value[3], value[4], value[5], value[6], value[7], value[8], value[9],
		viewport.X, viewport.Y, viewport.Width, viewport.Height);
}

bool CombineDepthEffect::RenderWithNormals(IDirect3DDevice9* Device, IDirect3DSurface9* NormalsSurface, bool allowInlineMerge, bool* outInlineMerged) {
	if (outInlineMerged) *outInlineMerged = false;
	if (ReShadeIntegration::KeepDepthBound()) return false;	// UNOFFICIAL ReShadeDepthCompat: the two-target pass unbinds the depth-stencil; separate passes instead
	if (!Enabled || !Effect || !ShouldRender() || !NormalsSurface || !Textures.CombinedDepthSurface)
		return false;
	// UNOFFICIAL 2026-10-25 (NVR DXVK) / 2026-11-19 RenderRect: the same two targets in one Vulkan pass (also at RenderScale < 1.0).
	// Safe even while [Main.Main.Performance] MergedDepthNormals is false (that switch avoids a native D3D9 GTX 1070 MRT bug).
	if (DxvkCompute::PassesOnRect() && !ExtFailed && ExtDepthNormals(Device, NormalsSurface, allowInlineMerge, outInlineMerged)) return true;
	if (mergedNormalsFailed || TheSettingManager->SettingsMain.Main.DisableMergedNormals)
		return false;
	D3DXHANDLE technique = TechniqueHandle(techDepthNormals, "DepthNormals");
	D3DSURFACE_DESC depthDesc = {}, normalsDesc = {};
	const D3DCAPS9* caps = TheRenderManager->DeviceCaps();	// UNOFFICIAL: fetched once, not per frame
	if (!technique || !caps || caps->NumSimultaneousRTs < 2 ||
		FAILED(Textures.CombinedDepthSurface->GetDesc(&depthDesc)) || FAILED(NormalsSurface->GetDesc(&normalsDesc)) ||
		depthDesc.Width != normalsDesc.Width || depthDesc.Height != normalsDesc.Height) {
		mergedNormalsFailed = true;
		return false;
	}
	// R32F depth is 32 bits per pixel, the normals are 64: mixing sizes needs this cap.
	const bool sameBitDepth = (depthDesc.Format == D3DFMT_G32R32F) == (normalsDesc.Format == D3DFMT_A16B16G16R16F);
	if (!sameBitDepth && !(caps->PrimitiveMiscCaps & D3DPMISCCAPS_MRTINDEPENDENTBITDEPTHS)) {
		mergedNormalsFailed = true;
		Logger::Log("Merged depth/normals unavailable: MRT with different bit depths not supported.");
		return false;
	}

	auto timer = TimeLogger();
	static bool checked = false;
	const bool check = !checked;
	checked = true;
	float before[4] = {}, after[4] = {};
	bool haveBefore = false;
	if (check && SUCCEEDED(Device->ColorFill(NormalsSurface, NULL, D3DCOLOR_ARGB(0x40, 0x20, 0x10, 0x08))))
		haveBefore = ReadCentreTexel(Device, NormalsSurface, normalsDesc, before);

	DWORD oldWriteMask1 = 0xF;
	Device->GetRenderState(D3DRS_COLORWRITEENABLE1, &oldWriteMask1);
	// With the game's depth-stencil still bound, the two-target draw was silently dropped on a
	// GTX 1070 (native D3D9). The fog MRT pass, which works, unbinds it first; do the same.
	IDirect3DSurface9* depthStencil = nullptr;
	Device->GetDepthStencilSurface(&depthStencil);
	Device->SetDepthStencilSurface(nullptr);
	HRESULT result = Device->SetRenderTarget(0, Textures.CombinedDepthSurface);
	if (SUCCEEDED(result)) result = Device->SetRenderTarget(1, NormalsSurface);
	if (SUCCEEDED(result)) result = Device->SetRenderState(D3DRS_COLORWRITEENABLE1, 0xF);
	if (SUCCEEDED(result)) result = Effect->SetTechnique(technique);
	if (SUCCEEDED(result)) {
		SetCT();
		UINT passes = 0;
		result = Effect->Begin(&passes, 0);
		if (SUCCEEDED(result)) {
			result = Effect->BeginPass(0);
			if (SUCCEEDED(result)) {
				if (check) LogMergedState(Device, *caps, depthDesc, normalsDesc, depthStencil);
				RenderRect::ApplyViewport(Device);	// UNOFFICIAL 2026-10-27 RenderScale v2
				result = Device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
				Effect->EndPass();
			}
			Effect->End();
		}
	}
	Device->SetRenderTarget(1, nullptr);
	Device->SetRenderState(D3DRS_COLORWRITEENABLE1, oldWriteMask1);
	Device->SetDepthStencilSurface(depthStencil);
	if (depthStencil) depthStencil->Release();
	if (FAILED(result)) {
		mergedNormalsFailed = true;
		Logger::Log("Merged depth/normals failed (%08lx); using separate passes until restart.", result);
		return false;
	}
	if (check) {
		const bool haveAfter = haveBefore && ReadCentreTexel(Device, NormalsSurface, normalsDesc, after);
		if (!haveAfter || !IsMarker(before)) {
			Logger::Log("UNOFFICIAL merged depth/normals check: inconclusive (marker %s, read back %s).",
				haveBefore && IsMarker(before) ? "written" : "not written", haveAfter ? "ok" : "failed");
		}
		else {
			const bool ran = !IsMarker(after);
			Logger::Log("UNOFFICIAL merged depth/normals check: normals centre pixel %.4f %.4f %.4f %.4f after the draw (marker %.4f %.4f %.4f %.4f): %s",
				after[0], after[1], after[2], after[3], before[0], before[1], before[2], before[3],
				ran ? "the two-target draw RAN." : "the two-target draw was DROPPED by the driver; using separate passes until restart.");
			if (!ran) {
				mergedNormalsFailed = true;
				return false; // the caller renders depth and normals the usual way this frame
			}
		}
	}
	static bool reported = false;
	if (!reported) {
		Logger::Log("UNOFFICIAL merged depth/normals active: one pass, two render targets.");
		reported = true;
	}
	renderTime = timer.LogTime("CombineDepth::RenderWithNormals");
	return true;
}

// UNOFFICIAL 2026-10-25 (NVR DXVK): CombineDepth.frag, technique 0. Drawn into the combined depth target, as the D3D9 pass; the two
// game depth textures are sampled as they are (POINT, CLAMP, as the .fx declares them).
bool CombineDepthEffect::ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	if (techniqueIndex != 0 || ClearRenderTarget) return false;
	const D3DXVECTOR4* depthConstants = Registered("TESR_DepthConstants");
	const D3DXVECTOR4* cameraData = Registered("TESR_CameraData");
	if (!depthConstants || !cameraData || !TheTextureManager->DepthTexture || !TheTextureManager->DepthTextureViewModel) return false;

	struct Push { D3DXVECTOR4 depthConstants; D3DXVECTOR4 cameraData; UInt32 samplerIndex; UInt32 pad[3]; D3DXVECTOR4 upscaleRect; D3DXVECTOR4 upscaleVm; } push = {};
	static_assert(sizeof(Push) == 80, "CombineDepth.frag push block");
	push.depthConstants = *depthConstants;
	push.depthConstants.w = 0.0f;
	push.cameraData = *cameraData;
	IDirect3DBaseTexture9* textures[] = { TheTextureManager->DepthTexture, TheTextureManager->DepthTextureViewModel };
	return ExtTexturePass(Device, RenderTarget, RenderedSurface, SourceBuffer, extPass, "CombineDepth", textures, _countof(textures), &push, sizeof(push), 32, true);
}

bool CombineDepthEffect::RenderUpscaled(IDirect3DDevice9* Device, UINT renderWidth, UINT renderHeight, float worldJx, float worldJy, float vmJx, float vmJy) {
	if (!DxvkCompute::PassesOn() || ExtFailed || !Enabled || !Effect || !ShouldRender() ||
		!Textures.CombinedDepthSurface || !TheTextureManager->DepthTexture || !TheTextureManager->DepthTextureViewModel)
		return false;
	if (RenderRect::Active()) return false;
	const D3DXVECTOR4* depthConstants = Registered("TESR_DepthConstants");
	const D3DXVECTOR4* cameraData = Registered("TESR_CameraData");
	if (!depthConstants || !cameraData) return false;

	static GpuTimer timer("Depth combine (upscaled)");
	GpuProfileScope gpu(timer, Device);
	struct Push { D3DXVECTOR4 depthConstants; D3DXVECTOR4 cameraData; UInt32 samplerIndex; UInt32 pad[3]; D3DXVECTOR4 upscaleRect; D3DXVECTOR4 upscaleVm; } push = {};
	static_assert(sizeof(Push) == 80, "CombineDepth.frag push block");
	push.depthConstants = *depthConstants;
	push.depthConstants.w = 1.0f;
	push.cameraData = *cameraData;
	push.upscaleRect = D3DXVECTOR4((float)renderWidth, (float)renderHeight, worldJx, worldJy);
	push.upscaleVm = D3DXVECTOR4(vmJx, vmJy, 0.0f, 0.0f);
	IDirect3DBaseTexture9* textures[] = { TheTextureManager->DepthTexture, TheTextureManager->DepthTextureViewModel };
	return ExtTexturePass(Device, Textures.CombinedDepthSurface, nullptr, nullptr, extPass, "CombineDepth", textures, _countof(textures), &push, sizeof(push), 32, true);
}

// UNOFFICIAL 2026-10-25 (NVR DXVK): DepthNormals.frag, the merged pass: the combined depth (target 0) and the normals (target 1) in one
// draw. Both targets are the caller's and of one size, so the same check as the D3D9 merged pass; false (the D3D9 merged pass then) otherwise.
bool CombineDepthEffect::ExtDepthNormals(IDirect3DDevice9* Device, IDirect3DSurface9* NormalsSurface, bool allowInlineMerge, bool* outInlineMerged) {
	if (outInlineMerged) *outInlineMerged = false;
	auto timer = TimeLogger();
	D3DSURFACE_DESC depthDesc = {}, normalsDesc = {};
	if (FAILED(Textures.CombinedDepthSurface->GetDesc(&depthDesc)) || FAILED(NormalsSurface->GetDesc(&normalsDesc)) ||
		depthDesc.Width != normalsDesc.Width || depthDesc.Height != normalsDesc.Height)
		return false;
	const D3DXVECTOR4* depthConstants = Registered("TESR_DepthConstants");
	const D3DXVECTOR4* cameraData = Registered("TESR_CameraData");
	const D3DXVECTOR4* reciprocal = Registered("TESR_ReciprocalResolution");
	const D3DXVECTOR4* invProjection = Registered("TESR_InvProjectionTransform");
	if (!depthConstants || !cameraData || !reciprocal || !invProjection || !TheTextureManager->DepthTexture || !TheTextureManager->DepthTextureViewModel) return false;

	CapturedNormals::MergeParams mp;
	const bool hasMerge = CapturedNormals::FillMergeParams(mp, allowInlineMerge);
	struct Push {
		D3DXVECTOR4 reciprocalResolution;
		D3DXVECTOR4 cameraData;
		D3DXVECTOR4 depthConstants;
		D3DXMATRIX invProjection;
		UInt32 samplerIndex;
		UInt32 pad[3];
		D3DXVECTOR4 renderScale;
		D3DXVECTOR4 mergeTol;
		D3DXVECTOR4 mergeView;
		D3DXVECTOR4 mergeRow0;
		D3DXVECTOR4 mergeRow1;
		D3DXVECTOR4 mergeRow2;
	} push = {};
	static_assert(sizeof(Push) == 224, "DepthNormals.frag push block");
	push.reciprocalResolution = *reciprocal;
	push.cameraData = *cameraData;
	push.depthConstants = *depthConstants;
	memcpy(&push.invProjection, invProjection, sizeof(D3DXMATRIX));
	const bool rectOn = RenderRect::Active();
	const int rw = rectOn ? RenderRect::Width((int)depthDesc.Width) : (int)depthDesc.Width;
	const int rh = rectOn ? RenderRect::Height((int)depthDesc.Height) : (int)depthDesc.Height;
	const float sx = (float)rw / (float)depthDesc.Width;
	const float sy = (float)rh / (float)depthDesc.Height;
	push.renderScale = D3DXVECTOR4(sx, sy, 1.0f / sx, 1.0f / sy);
	push.mergeTol = mp.tol;
	push.mergeView = mp.view;
	push.mergeRow0 = mp.row0;
	push.mergeRow1 = mp.row1;
	push.mergeRow2 = mp.row2;

	if (!extPassDepthNormals.Tried) {
		extPassDepthNormals.Tried = true;
		const NVR_EXT_BINDING_DESC bindings[] = {
			{ 1, 0, NVR_EXT_BIND_SAMPLED_IMAGE, 0, NVR_EXT_VIEW_2D, FALSE, 0 },		// TESR_DepthBufferWorld
			{ 1, 1, NVR_EXT_BIND_SAMPLED_IMAGE, 1, NVR_EXT_VIEW_2D, FALSE, 0 },		// TESR_DepthBufferViewModel
			{ 1, 2, NVR_EXT_BIND_SAMPLED_IMAGE, 2, NVR_EXT_VIEW_2D, FALSE, 0 },		// TESR_CapturedNormalsBuffer
			{ 1, 3, NVR_EXT_BIND_SAMPLED_IMAGE, 3, NVR_EXT_VIEW_2D, FALSE, 0 },		// MaterialBuffer
			{ 0, 0, NVR_EXT_BIND_SAMPLER,       0, NVR_EXT_VIEW_2D, FALSE, 112 },	// POINT, CLAMP
		};
		extPassDepthNormals.Pipeline = DxvkCompute::CreateGraphicsPipeline("DepthNormals", bindings, _countof(bindings), sizeof(push), true, 2);
	}
	ID3D9NvrExtSampler* point = DxvkCompute::PointClamp();
	if (!extPassDepthNormals.Pipeline || !point) return false;

	IDirect3DBaseTexture9* fallbackTex = TheTextureManager->DepthTexture;
	NVR_EXT_RESOURCE_BINDING resources[5] = {};
	resources[0].Slot = 0; resources[0].Type = NVR_EXT_BIND_SAMPLED_IMAGE; resources[0].pResource = TheTextureManager->DepthTexture;
	resources[1].Slot = 1; resources[1].Type = NVR_EXT_BIND_SAMPLED_IMAGE; resources[1].pResource = TheTextureManager->DepthTextureViewModel;
	resources[2].Slot = 2; resources[2].Type = NVR_EXT_BIND_SAMPLED_IMAGE; resources[2].pResource = mp.captured ? mp.captured : fallbackTex;
	resources[3].Slot = 3; resources[3].Type = NVR_EXT_BIND_SAMPLED_IMAGE; resources[3].pResource = mp.material ? mp.material : fallbackTex;
	resources[4].Slot = 0; resources[4].Type = NVR_EXT_BIND_SAMPLER; resources[4].pResource = point;
	IDirect3DSurface9* targets[2] = { Textures.CombinedDepthSurface, NormalsSurface };
	const RECT vp = { 0, 0, rw, rh };
	if (!ExtDrawTo(extPassDepthNormals.Pipeline, targets, 2, resources, _countof(resources), &push, sizeof(push), rectOn ? &vp : nullptr, ExtUv::ScreenQuad)) return false;	// FrameVS into a smaller target
	if (hasMerge) {
		if (outInlineMerged) *outInlineMerged = true;
		CapturedNormals::NoteExternalMerge((UInt32)rw, (UInt32)rh);
	}
	renderTime = timer.LogTime("CombineDepth::RenderWithNormals (NVR DXVK)");
	return true;
}