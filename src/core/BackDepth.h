#pragma once
// UNOFFICIAL 2026-10-31 BackDepth ([Main.Main.Quality] BackDepth, live, off; docs/next/ROOT-ASSUMPTIONS-PLAN.md B3 "thickness").
// The screen-space effects read only the nearest surface of every pixel (the depth buffer) and have to guess how thick it is: the contact
// shadows take every surface as SSS_THICKNESS (20 units) deep, AmbientLight's horizon search as solid all the way back. Behind a thin
// thing (a pole, a rail, a person's arm) that guess casts contact shadows and ambient darkening that are not there (rings and halos).
// This pass draws the BACK faces of the objects within BackDepthDistance of the camera (no land: it has no back) into a screen-sized R32F
// target, the nearest back face per pixel, with the sun-shadow casters' own shaders and passes (ShadowMap.vso/.pso: skinning, plant and
// cloth wind, alpha test) and the camera's matrix, each object's cull mode turned round (RenderPass::FlipCull). Readers
// (Effects/Includes/BackDepth.hlsl BackViewZ) turn the stored depth into view distance with the projection (TESR_BackDepthData): an
// object is then as thick as its back face says; where no object drew (land, past the distance) the old guess stays.
// Drawn after the world, before the effects (ShaderManager, next to the capsule pass), inside the render rect (RenderScale), as the depth.
#include <d3dx9.h>
#include "RenderPass.h"
#include "RenderRect.h"
#include "GpuProfiler.h"
#include "ClothWind.h"

namespace BackDepth {
	inline IDirect3DTexture9* Texture = nullptr;
	inline IDirect3DSurface9* Surface = nullptr;
	inline IDirect3DSurface9* Depth = nullptr;
	inline UINT Width = 0, Height = 0;
	inline D3DXVECTOR4 Data = { 0.0f, 0.0f, 0.0f, 0.0f };	// TESR_BackDepthData: x 1 = drawn this frame, y projection _33, z projection _43
	inline unsigned Objects = 0, Frames = 0;

	inline void Release() {
		if (Surface) { Surface->Release(); Surface = nullptr; }
		if (Texture) { Texture->Release(); Texture = nullptr; }
		if (Depth) { Depth->Release(); Depth = nullptr; }
		Width = Height = 0;
	}

	inline void Render(IDirect3DDevice9* Device) {
		Data.x = 0.0f;
		if (!TheSettingManager->GetSettingI("Main.Main.Quality", "BackDepth")) return;
		if (!TheTextureManager->DepthTexture || !Player || !WorldSceneGraph || !WorldSceneGraph->camera) return;
		D3DSURFACE_DESC desc;
		if (FAILED(TheTextureManager->DepthTexture->GetLevelDesc(0, &desc))) return;
		if (desc.Width != Width || desc.Height != Height) {
			Release();
			if (FAILED(Device->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &Texture, NULL)) ||
				FAILED(Texture->GetSurfaceLevel(0, &Surface)) ||
				FAILED(Device->CreateDepthStencilSurface(desc.Width, desc.Height, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &Depth, NULL))) {
				Logger::Log("[ERROR] BackDepth: could not make the %ux%u targets", desc.Width, desc.Height);
				Release();
				return;
			}
			Width = desc.Width; Height = desc.Height;
			Logger::Log("BackDepth: %ux%u R32F back-face depth made", Width, Height);
		}
		static GpuTimer timer("Back-face depth (BackDepth)");
		GpuProfileScope gpu(timer, Device);
		static CpuTimer cpuTimer("Back-face depth (CPU)");
		CpuProfileScope cpu(cpuTimer);

		ShadowManager* sm = TheShadowManager;
		ShadowsExteriorEffect* shadows = TheShaderManager->Effects.ShadowsExteriors;
		NiDX9RenderState* state = TheRenderManager->renderState;
		const float reach = max(TheSettingManager->GetSettingF("Main.Main.Quality", "BackDepthDistance"), 0.0f);

		IDirect3DSurface9* savedTarget = nullptr;
		IDirect3DSurface9* savedDepth = nullptr;
		Device->GetRenderTarget(0, &savedTarget);
		Device->GetDepthStencilSurface(&savedDepth);
		IDirect3DSurface9* savedMrt[3] = {};
		for (DWORD i = 1; i < 4; i++) { Device->GetRenderTarget(i, &savedMrt[i - 1]); if (savedMrt[i - 1]) Device->SetRenderTarget(i, nullptr); }
		D3DVIEWPORT9 savedPort;
		Device->GetViewport(&savedPort);
		DWORD savedZFunc = D3DCMP_LESSEQUAL;
		Device->GetRenderState(D3DRS_ZFUNC, &savedZFunc);

		DWORD savedStates[6];
		static const D3DRENDERSTATETYPE kStates[6] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_ALPHATESTENABLE, D3DRS_CULLMODE, D3DRS_ALPHAREF };
		for (int k = 0; k < 6; k++) Device->GetRenderState(kStates[k], &savedStates[k]);
		// Everything the effects' own quads rely on: the second round showed the effects drawing the casters' last vertex buffer
		// (the picture smeared into streaks) without this. Streams, indices, declaration, shaders, textures and both constant files.
		IDirect3DVertexBuffer9* savedStream = nullptr; UINT savedOffset = 0, savedStride = 0;
		Device->GetStreamSource(0, &savedStream, &savedOffset, &savedStride);
		IDirect3DIndexBuffer9* savedIndices = nullptr;
		Device->GetIndices(&savedIndices);
		IDirect3DVertexDeclaration9* savedDecl = nullptr;
		Device->GetVertexDeclaration(&savedDecl);
		DWORD savedFvf = 0;
		Device->GetFVF(&savedFvf);
		IDirect3DVertexShader9* savedVSh = nullptr;
		Device->GetVertexShader(&savedVSh);
		IDirect3DPixelShader9* savedPSh = nullptr;
		Device->GetPixelShader(&savedPSh);
		IDirect3DBaseTexture9* savedTex[16] = {};
		for (DWORD t = 0; t < 16; t++) Device->GetTexture(t, &savedTex[t]);
		static float savedVC[256][4], savedPC[224][4];
		Device->GetVertexShaderConstantF(0, &savedVC[0][0], 256);
		Device->GetPixelShaderConstantF(0, &savedPC[0][0], 224);
		const bool savedClothNonZero = ClothWind::NonZeroSent, savedClothGrass = ClothWind::GrassInC248;
		const bool reversed = TheRenderManager->IsReversedDepth();
		Device->SetRenderTarget(0, Surface);
		Device->SetDepthStencilSurface(Depth);
		D3DVIEWPORT9 port = { 0, 0, RenderRect::Width(Width), RenderRect::Height(Height), 0.0f, 1.0f };
		Device->SetViewport(&port);
		// 0 in the colour = no back face here (the near plane, or infinitely far with reversed depth: neither is a back face)
		Device->Clear(0L, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_ARGB(0, 0, 0, 0), reversed ? 0.0f : 1.0f, 0L);
		state->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE, RenderStateArgs);
		state->SetRenderState(D3DRS_ZWRITEENABLE, TRUE, RenderStateArgs);
		state->SetRenderState(D3DRS_ZFUNC, reversed ? D3DCMP_GREATER : D3DCMP_LESS, RenderStateArgs);	// the nearest back face
		state->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE, RenderStateArgs);
		state->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE, RenderStateArgs);

		// the casters' passes with the sun-shadow shaders (indoors they may hold the cube-map ones: put back after)
		RenderPass* passes[5] = { sm->geometryPass, sm->alphaPass, sm->skinnedGeoPass, sm->speedTreePass, sm->terrainLODPass };
		ShaderRecordVertex* savedVS[5];
		ShaderRecordPixel* savedPS[5];
		for (int p = 0; p < 5; p++) { savedVS[p] = passes[p]->VertexShader; savedPS[p] = passes[p]->PixelShader; passes[p]->VertexShader = sm->ShadowMapVertex; }
		sm->geometryPass->PixelShader = sm->ShadowMapOpaquePixel;
		sm->alphaPass->PixelShader = sm->ShadowMapPixel;
		sm->skinnedGeoPass->PixelShader = sm->ShadowMapOpaquePixel;
		sm->speedTreePass->PixelShader = sm->ShadowMapPixel;
		sm->terrainLODPass->PixelShader = sm->ShadowMapOpaquePixel;
		// the casters' passes write the shadow constants (TESR_ShadowData x / y: caster kind, alpha) the effects read afterwards (y = the
		// shadows' darkness): all of them put back after (first run: the whole picture went 26-40 levels darker without this)
		const auto savedConstants = shadows->Constants;
		const D3DXMATRIX savedShadowWorld = TheShaderManager->ShaderConst.ShadowWorld;
		shadows->Constants.ShadowViewProj = TheRenderManager->ViewProjMatrix;	// camera-relative, as the casters' world matrices

		// the objects: every reference of the loaded cells within reach of the camera and not wholly behind it
		ShadowsExteriorEffect::FormsStruct forms = shadows->ShadowMaps[ShadowsExteriorEffect::MapNear].Forms;
		forms.MinRadius = 0.0f;
		forms.AlphaEnabled = true;
		forms.Lod = false;
		const NiPoint3 eye = WorldSceneGraph->camera->m_worldTransform.pos;
		const D3DXVECTOR3 forward(TheRenderManager->CameraForward.x, TheRenderManager->CameraForward.y, TheRenderManager->CameraForward.z);
		Objects = 0;
		auto walkCell = [&](TESObjectCELL* cell) {
			if (!cell) return;
			for (TList<TESObjectREFR>::Entry* entry = &cell->objectList.First; entry; entry = entry->next) {
				TESObjectREFR* ref = entry->item;
				if (!ref || !ref->baseForm) continue;
				NiNode* node = sm->GetRefNode(ref, &forms);
				if (!node || !node->m_kWorldBound) continue;
				const NiPoint3& c = node->m_kWorldBound->Center;
				const float r = node->m_kWorldBound->Radius;
				const D3DXVECTOR3 to(c.x - eye.x, c.y - eye.y, c.z - eye.z);
				if (D3DXVec3Length(&to) - r > reach || D3DXVec3Dot(&to, &forward) < -r) continue;
				if (sm->IsRefracting(ref)) continue;
				sm->AccumChildren(node, &forms, false, false, nullptr);
				Objects++;
			}
		};
		if (Player->GetWorldSpace()) {
			GridCellArray* cells = Tes->gridCellArray;
			const UInt32 count = cells->size * cells->size;
			for (UInt32 i = 0; i < count; i++) walkCell(cells->GetCell(i));
		}
		else walkCell(Player->parentCell);
		RenderPass::FlipCull = true;
		sm->RenderAccums();
		RenderPass::FlipCull = false;

		shadows->Constants = savedConstants;
		TheShaderManager->ShaderConst.ShadowWorld = savedShadowWorld;
		for (int k = 0; k < 6; k++) state->SetRenderState(kStates[k], savedStates[k], RenderStateArgs);
		Device->SetStreamSource(0, savedStream, savedOffset, savedStride);
		if (savedStream) savedStream->Release();
		Device->SetIndices(savedIndices);
		if (savedIndices) savedIndices->Release();
		if (savedDecl) { state->SetVertexDeclaration(savedDecl, false); savedDecl->Release(); }
		else state->SetFVF(savedFvf, false);
		state->SetVertexShader(savedVSh, false); if (savedVSh) savedVSh->Release();
		state->SetPixelShader(savedPSh, false); if (savedPSh) savedPSh->Release();
		for (DWORD t = 0; t < 16; t++) { state->SetTexture(t, savedTex[t]); if (savedTex[t]) savedTex[t]->Release(); }
		Device->SetVertexShaderConstantF(0, &savedVC[0][0], 256);
		ClothWind::OnVSConstantsRestored(savedVC[248], savedClothNonZero, savedClothGrass);
		Device->SetPixelShaderConstantF(0, &savedPC[0][0], 224);
		for (int p = 0; p < 5; p++) { passes[p]->VertexShader = savedVS[p]; passes[p]->PixelShader = savedPS[p]; }
		state->SetRenderState(D3DRS_ZFUNC, savedZFunc, RenderStateArgs);
		Device->SetRenderTarget(0, savedTarget);
		for (DWORD i = 1; i < 4; i++) if (savedMrt[i - 1]) { Device->SetRenderTarget(i, savedMrt[i - 1]); savedMrt[i - 1]->Release(); }
		Device->SetDepthStencilSurface(savedDepth);
		Device->SetViewport(&savedPort);
		if (savedTarget) savedTarget->Release();
		if (savedDepth) savedDepth->Release();

		const D3DXMATRIX& proj = TheRenderManager->projMatrix;
		Data = D3DXVECTOR4(1.0f, proj._33, proj._43, 0.0f);
		if (++Frames >= 240) {
			Frames = 0;
			if (GpuTimer::Enabled) Logger::Log("BACK DEPTH: %u objects drawn (within %.0f units), %ux%u", Objects, reach, port.Width, port.Height);
		}
	}
}
