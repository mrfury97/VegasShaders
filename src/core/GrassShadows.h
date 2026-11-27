#pragma once

// UNOFFICIAL 2026-10-27 grass sun shadows ([Shaders.Grass.Main] CastShadows). User: "no grass shadows are broken ... billboard bushes
// have shadows as they should". The sun's shadow maps are drawn from the scene graph (ShadowManager::WalkSunCell: the cells' land and
// references), and the grass tufts are not in it -- the game places them per draw from an instance table (TallGrassShader) -- so they
// never cast anything; only the shading ON the grass took the shadows of others (GRASS23x000TMS.pso).
//
// Now, in GrassFirst's early grass call (Render.cpp namespace GrassFirst; needs [Shaders.Grass.Main] DrawFirst, on by default), once
// the near grass is drawn the same grass passes are drawn again into the NEAR sun cascade of the shadow atlas, before the land and
// everything after it read the atlas. For that span the device's DrawIndexedPrimitive is ours (as NewGrass / GpuGrass do it): each
// grass draw gets the caster shaders (the grass VS compiled with GRASS_SHADOW_CASTER, GrassShadowCaster.pso), its depth MIN-blended
// into the map's (the shadow map's depth test where the format does not blend), and the camera's view -> shadow map transform in
// c252-c255 (xyz; the grass wind lanes in w are kept), all put back after it.
// The tufts sway, fade and stand exactly where they are drawn (same VS code, same constants); the collision push is left out.
//
// 2026-10-28: with terrain blending on GrassFirst stands aside (its depth copy would hold the grass); then Render.cpp ShadowOnlyEarly
// takes the grass passes out before the land without drawing them, they come here, and the late part draws them whole on screen.
//
// Limits: the PCF shadow maps (Mode 3, the default) only -- the variance modes filter their maps after drawing;
// the grass drawn early does not take these shadows itself (it is drawn before them), the ground and everything after it does.
//
// 2026-10-28 (Conor: "grass shadows fade out like 5 feet in front me, needs a slider"): the near cascade ends about 2-3 m out at
// CascadeLambda 0.9, so the grass shadows faded there. [Shaders.Grass.Main] CastShadowsReach: the tufts within this many world units
// of the camera are also drawn into the middle and far cascades (only those redrawn this frame: a cached cascade keeps what it
// has, so swaying tufts never pile up in it). 0 = the near cascade only, as before.

#include <d3dx9.h>
#include <vector>
#include "NewGrass.h"	// LastVariant: the grass VS bound last

extern unsigned SunCascadesDrawnMask;	// ShadowManager.cpp: the sun cascades (bit per ShadowMapTypeEnum) drawn this frame

namespace GrassShadows {
	struct Record { void* pass; UInt32 passId, a3, a4, a5; };

	inline bool Enabled = true;			// [Shaders.Grass.Main] CastShadows (live)
	inline float Reach = 1050.0f;		// [Shaders.Grass.Main] CastShadowsReach (live): past the near cascade, tufts this close also go into the middle and far ones
	inline int Cascade = 0;				// Begin .. End: the cascade drawn into (ShadowsExteriorEffect::MapNear .. MapFar)
	inline const char* const CascadeNames[3] = { "near", "middle", "far" };
	inline std::vector<Record> Records;	// this frame's early grass passes drawn with NVR's grass VS (GrassFirst)
	inline bool Active = false;			// between Begin and End: the grass draws go into the shadow map
	inline bool Hooked = false;
	inline ShaderRecordVertex* VS[4] = {};	// GRASS23x000-003.vso as casters (the 1POINT VS 004-007 are 000-003 plus the lamp)
	inline ShaderRecordPixel* PS = nullptr;
	inline bool LoadTried = false, LoadFailed = false;
	// UNOFFICIAL 2026-11-04 the flashlight's shadow map too (BeginSpot; user: "make the flashlight cast shadows from grass too"): the same
	// passes into its spot map with the grass VS compiled as GRASS_SHADOW_CASTER 2 (its perspective, the distance over the reach in z,
	// MIN-blended: the nearer stays), the matrix in c13-c16 and the light in c18; c252 keeps the camera's inverse view.
	inline bool SpotMode = false;
	inline ShaderRecordVertex* SpotVS[4] = {};
	inline bool SpotLoadTried = false, SpotLoadFailed = false;
	inline float SpotMatrix[4][4] = {};
	inline D3DXVECTOR4 SpotLight = {};	// camera-relative, w the reach
	inline unsigned SpotFrames = 0;
	inline bool SpotSavedMinBlend = false;
	inline const D3DXMATRIX* CameraToLight = nullptr;	// the near cascade's camera-relative world -> shadow map space (Begin)
	inline D3DXVECTOR3 SunDir = { 0.0f, 0.0f, 1.0f };
	inline DWORD ZFunc = D3DCMP_LESSEQUAL;
	inline IDirect3DSurface9* SavedTarget[4] = {};
	inline IDirect3DSurface9* SavedDepth = nullptr;
	inline D3DVIEWPORT9 SavedViewport = {};
	inline D3DXVECTOR4 NearSphere;		// the cascade's sphere (centre camera-relative, w radius); the near one's unless Cascade > 0
	inline unsigned Frames = 0, StatPasses = 0, StatOutside = 0, StatDraws = 0, StatSkipped = 0, StatRetarget = 0;
	// 2026-10-27 (user, at a low sun: "there are still no shadows", the log saying the passes were drawn): the draws no longer depend
	// on the atlas's depth buffer (its contents after the cascades are not something to count on: it is created discardable, and
	// whatever ran after the cascades may have used or cleared it). Where the format blends (R32F does on every card that runs
	// this), each grass pixel is MIN-blended into the depth the map holds: the nearer of the two stays, as a depth test would keep.
	inline bool MinBlend = false, MinBlendChecked = false;
	inline bool Black = false;			// [Shaders.Grass.Main] CastShadowsBlack (live, diagnostic): the casters write depth 0, so their shadow is solid black
	inline IDirect3DSurface9* AtlasTarget = nullptr;	// Begin .. End: what every grass draw must go into (DrawHook puts it back if a pass changed it)
	inline IDirect3DSurface9* AtlasDepth = nullptr;
	inline D3DVIEWPORT9 AtlasViewport = {};
	// An occlusion query round the replay: how many shadow map pixels the grass really wrote (logged with the frame lines).
	inline IDirect3DQuery9* Query = nullptr;
	inline bool QueryOpen = false, QueryPending = false;
	inline DWORD LastPixels = 0;
	inline bool PixelsKnown = false;
	inline D3DXVECTOR3 ProbeLight = { 0.0f, 0.0f, 0.0f };	// the first near pass's bound centre in the near map's space (x, y -1..1 and z 0..1 on the map)
	inline bool ProbeSet = false;

	typedef HRESULT(STDMETHODCALLTYPE* DrawIndexedPrimitiveFn)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
	inline DrawIndexedPrimitiveFn Next = nullptr;

	inline void ReadSettings() {
		Enabled = TheSettingManager->GetSettingI("Shaders.Grass.Main", "CastShadows") != 0;
		Black = TheSettingManager->GetSettingI("Shaders.Grass.Main", "CastShadowsBlack") != 0;
		Reach = max(0.0f, TheSettingManager->GetSettingF("Shaders.Grass.Main", "CastShadowsReach"));
	}

	// GrassFirst: an early grass pass just drawn with NVR's grass VS.
	inline void Note(void* pass, UInt32 passId, UInt32 a3, UInt32 a4, UInt32 a5) {
		if (Enabled) Records.push_back({ pass, passId, a3, a4, a5 });
	}

	// Does a grass pass's world bound reach the cascade's sphere (and, past the near cascade, come within Reach of the camera)?
	// (Unknown bounds: yes in the near cascade, no in the others.)
	inline bool InNear(void* pass) {
		NiAVObject* geometry = *(NiAVObject**)pass;
		if (SpotMode) {	// the flashlight: the piece within its reach
			const NiBound* bound = geometry ? geometry->GetWorldBound() : nullptr;
			if (!bound || bound == NiBound::GetGlobalWorldBound() || !(bound->Radius > 0.0f)) return true;
			const D3DXVECTOR4& cam = TheRenderManager->CameraPosition;
			const float dx = bound->Center.x - cam.x - SpotLight.x, dy = bound->Center.y - cam.y - SpotLight.y, dz = bound->Center.z - cam.z - SpotLight.z;
			const float reach = bound->Radius + SpotLight.w;
			return dx * dx + dy * dy + dz * dz <= reach * reach;
		}
		if (!geometry) return Cascade == 0;
		const NiBound* bound = geometry->GetWorldBound();
		if (!bound || bound == NiBound::GetGlobalWorldBound() || !(bound->Radius > 0.0f)) return Cascade == 0;
		const D3DXVECTOR4& cam = TheRenderManager->CameraPosition;
		if (Cascade > 0) {
			const float cx = bound->Center.x - cam.x, cy = bound->Center.y - cam.y, cz = bound->Center.z - cam.z;
			const float limit = Reach + bound->Radius + 256.0f;
			if (cx * cx + cy * cy + cz * cz > limit * limit) return false;
		}
		const float dx = bound->Center.x - cam.x - NearSphere.x, dy = bound->Center.y - cam.y - NearSphere.y, dz = bound->Center.z - cam.z - NearSphere.z;
		const float reach = bound->Radius + NearSphere.w;
		const float along = dx * SunDir.x + dy * SunDir.y + dz * SunDir.z;
		if (along < -reach || along > reach + 1024.0f) return false;
		const float px = dx - along * SunDir.x, py = dy - along * SunDir.y, pz = dz - along * SunDir.z;
		const bool inside = px * px + py * py + pz * pz <= reach * reach;
		if (inside && !ProbeSet && CameraToLight) {	// for the log: where the map puts this piece (CPU side, the same transform as the VS)
			const D3DXVECTOR3 rel(bound->Center.x - cam.x, bound->Center.y - cam.y, bound->Center.z - cam.z);
			D3DXVec3TransformCoord(&ProbeLight, &rel, CameraToLight);
			ProbeSet = true;
		}
		return inside;
	}

	inline bool LoadShaders() {
		if (LoadTried) return !LoadFailed;
		LoadTried = true;
		static const D3DXMACRO caster[] = { { "GRASS_SHADOW_CASTER", "1" }, { NULL, NULL } };
		static const char* const sources[4] = { "GRASS23x000.vso", "GRASS23x001.vso", "GRASS23x002.vso", "GRASS23x003.vso" };
		static const char* const names[4] = { "GrassShadowCaster0.vso", "GrassShadowCaster1.vso", "GrassShadowCaster2.vso", "GrassShadowCaster3.vso" };
		for (int i = 0; i < 4; i++) {
			ShaderTemplate t = {};
			t.Name = sources[i];
			VS[i] = (ShaderRecordVertex*)ShaderRecord::LoadShader(names[i], NULL, t, NULL, caster);
		}
		PS = (ShaderRecordPixel*)ShaderRecord::LoadShader("GrassShadowCaster.pso", NULL);
		LoadFailed = !PS || (!VS[0] && !VS[1] && !VS[2] && !VS[3]);
		Logger::Log("UNOFFICIAL grass shadows: caster shaders %s (VS %p %p %p %p, PS %p)", LoadFailed ? "FAILED to load: off" : "loaded", VS[0], VS[1], VS[2], VS[3], PS);
		return !LoadFailed;
	}
	inline bool LoadSpotShaders() {
		if (SpotLoadTried) return !SpotLoadFailed;
		SpotLoadTried = true;
		static const D3DXMACRO caster[] = { { "GRASS_SHADOW_CASTER", "2" }, { NULL, NULL } };
		static const char* const sources[4] = { "GRASS23x000.vso", "GRASS23x001.vso", "GRASS23x002.vso", "GRASS23x003.vso" };
		static const char* const names[4] = { "GrassSpotCaster0.vso", "GrassSpotCaster1.vso", "GrassSpotCaster2.vso", "GrassSpotCaster3.vso" };
		for (int i = 0; i < 4; i++) {
			ShaderTemplate t = {};
			t.Name = sources[i];
			SpotVS[i] = (ShaderRecordVertex*)ShaderRecord::LoadShader(names[i], NULL, t, NULL, caster);
		}
		SpotLoadFailed = !SpotVS[0] && !SpotVS[1] && !SpotVS[2] && !SpotVS[3];
		Logger::Log("UNOFFICIAL grass shadows: flashlight caster shaders %s (VS %p %p %p %p)", SpotLoadFailed ? "FAILED to load: no grass in the flashlight's shadow" : "loaded", SpotVS[0], SpotVS[1], SpotVS[2], SpotVS[3]);
		return !SpotLoadFailed;
	}

	// One grass draw while Active: into the shadow map with the caster shaders, everything it changed put back after.
	inline HRESULT STDMETHODCALLTYPE DrawHook(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, INT base, UINT minIndex, UINT numVertices, UINT start, UINT count) {
		if (!Active) return Next(device, type, base, minIndex, numVertices, start, count);
		// The grass VS this pass bound (GrassShaders::SetForDraw): only NVR's carries the caster's twin; anything else draws nothing here.
		const char v = NewGrass::LastVariant;
		ShaderRecordVertex* vs = GrassShaders::LastBindNVR && v >= '0' && v <= '7' ? (SpotMode ? SpotVS : VS)[(v - '0') & 3] : nullptr;
		if (!vs || !vs->ShaderHandle || (!SpotMode && !CameraToLight)) { StatSkipped++; return D3D_OK; }

		// c252-c255: save whatever was bound, then upload view -> shadow map transform directly from TheRenderManager->InvViewMatrix
		// (preserving GrassWind::Lanes in column 4) and world-space GrassBend in c0-c1.
		float view[4][4] = {};
		const bool viewSaved = SUCCEEDED(device->GetVertexShaderConstantF(252, &view[0][0], 4));
		if (!viewSaved) { GrassShaders::PendingTstgRestore = false; StatSkipped++; return D3D_OK; }
		float bendSaved[2][4] = {};
		const bool bendSavedOk = SUCCEEDED(device->GetVertexShaderConstantF(0, &bendSaved[0][0], 2));
		float bendWorld[4] = { 0.0f, 0.0f, 0.0f, 0.0f }, bendParams[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		if (GrassShaders* grass = TheShaderManager ? TheShaderManager->Shaders.Grass : nullptr) {
			if (grass->Have && grass->BendPush > 0.0f) {
				bendWorld[0] = grass->BendCentre.x;
				bendWorld[1] = grass->BendCentre.y;
				bendWorld[2] = grass->BendCentre.z;
				bendWorld[3] = grass->Constants.Bend.w;
				bendParams[0] = grass->BendPush;
			}
		}
		device->SetVertexShaderConstantF(0, bendWorld, 1);
		device->SetVertexShaderConstantF(1, bendParams, 1);

		float spotSaved[6][4] = {};	// c13-c18 (the flashlight caster's, SpotMode)
		if (SpotMode) {
			device->GetVertexShaderConstantF(13, &spotSaved[0][0], 6);
			device->SetVertexShaderConstantF(13, &SpotMatrix[0][0], 4);
			device->SetVertexShaderConstantF(18, (const float*)&SpotLight, 1);
		}
		else {
			D3DXMATRIX invView;
			for (int r = 0; r < 4; r++) {
				for (int c = 0; c < 3; c++) invView.m[r][c] = TheRenderManager->InvViewMatrix.m[r][c];
				invView.m[r][3] = r == 3 ? 1.0f : 0.0f;
			}
			D3DXMATRIX viewToLight;
			D3DXMatrixMultiply(&viewToLight, &invView, CameraToLight);
			float light[4][4];
			for (int r = 0; r < 4; r++) {
				for (int c = 0; c < 3; c++) light[r][c] = viewToLight.m[r][c];
				light[r][3] = GrassWind::On ? GrassWind::Lanes[r] : (r == 3 ? 1.0f : 0.0f);
			}
			device->SetVertexShaderConstantF(252, &light[0][0], 4);
		}
		// 2026-10-28 (Conor, streaks with cloth on): c248-c251, the camera's inverse projection the caster places each tuft with, written
		// here as NVR's constant table sends it for the grass VS (TESR_InvProjectionTransform = RenderManager::InvProjMatrix), not
		// trusted from the device: other code may have touched c248 (ClothWind cleared it before every non-cloth pass), and NVR does not
		// send an unchanged constant again.
		float proj[4][4];
		const bool projSaved = SUCCEEDED(device->GetVertexShaderConstantF(248, &proj[0][0], 4));
		device->SetVertexShaderConstantF(248, (const float*)&TheRenderManager->InvProjMatrix, 4);

		// MinBlend: no depth test, the nearer depth kept by the blend (BLENDOP MIN ignores the factors). Else the shadow maps' own test.
		static const D3DRENDERSTATETYPE kStates[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_CULLMODE, D3DRS_ALPHATESTENABLE,
			D3DRS_ALPHABLENDENABLE, D3DRS_BLENDOP, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_SEPARATEALPHABLENDENABLE,
			D3DRS_COLORWRITEENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_STENCILENABLE,
			D3DRS_DEPTHBIAS, D3DRS_SLOPESCALEDEPTHBIAS, D3DRS_CLIPPLANEENABLE, D3DRS_ADAPTIVETESS_Y };
		const DWORD want[] = { MinBlend ? (DWORD)D3DZB_FALSE : (DWORD)D3DZB_TRUE, MinBlend ? 0u : 1u, ZFunc, D3DCULL_NONE, FALSE,
			MinBlend ? 1u : 0u, D3DBLENDOP_MIN, D3DBLEND_ONE, D3DBLEND_ONE, FALSE,
			0xF, FALSE, FALSE, FALSE, 0, 0, 0, 0 };
		static_assert(sizeof(kStates) / sizeof(kStates[0]) == sizeof(want) / sizeof(want[0]), "one value per state");
		DWORD saved[sizeof(kStates) / sizeof(kStates[0])];
		for (unsigned i = 0; i < sizeof(kStates) / sizeof(kStates[0]); i++) {
			if (FAILED(device->GetRenderState(kStates[i], &saved[i]))) saved[i] = want[i];
			if (saved[i] != want[i]) device->SetRenderState(kStates[i], want[i]);
		}
		IDirect3DVertexShader9* oldVS = nullptr;
		IDirect3DPixelShader9* oldPS = nullptr;
		device->GetVertexShader(&oldVS);
		device->GetPixelShader(&oldPS);
		device->SetVertexShader((IDirect3DVertexShader9*)vs->ShaderHandle);
		device->SetPixelShader((IDirect3DPixelShader9*)PS->ShaderHandle);
		float oldC0[4] = {};
		device->GetPixelShaderConstantF(0, oldC0, 1);
		const float casterParams[4] = { Black ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f };
		device->SetPixelShaderConstantF(0, casterParams, 1);

		// The near cascade whatever the pass bound (a pass could set its own targets or viewport): put back after the draw.
		IDirect3DSurface9* rt[4] = {};
		IDirect3DSurface9* ds = nullptr;
		D3DVIEWPORT9 vp = {};
		for (DWORD i = 0; i < 4; i++) if (FAILED(device->GetRenderTarget(i, &rt[i]))) rt[i] = nullptr;
		if (FAILED(device->GetDepthStencilSurface(&ds))) ds = nullptr;
		device->GetViewport(&vp);
		const bool retarget = rt[0] != AtlasTarget || rt[1] || rt[2] || rt[3] || ds != AtlasDepth || memcmp(&vp, &AtlasViewport, sizeof(vp)) != 0;
		if (retarget) {
			StatRetarget++;
			device->SetRenderTarget(0, AtlasTarget);
			for (DWORD i = 1; i < 4; i++) if (rt[i]) device->SetRenderTarget(i, nullptr);
			device->SetDepthStencilSurface(AtlasDepth);
			device->SetViewport(&AtlasViewport);
		}

		const HRESULT hr = Next(device, type, base, minIndex, numVertices, start, count);
		StatDraws++;

		if (retarget) {
			device->SetRenderTarget(0, rt[0]);
			for (DWORD i = 1; i < 4; i++) if (rt[i]) device->SetRenderTarget(i, rt[i]);
			device->SetDepthStencilSurface(ds);
			device->SetViewport(&vp);
		}
		for (DWORD i = 0; i < 4; i++) if (rt[i]) rt[i]->Release();
		if (ds) ds->Release();
		device->SetPixelShaderConstantF(0, oldC0, 1);
		device->SetVertexShader(oldVS);
		device->SetPixelShader(oldPS);
		if (oldVS) oldVS->Release();
		if (oldPS) oldPS->Release();
		for (unsigned i = 0; i < sizeof(kStates) / sizeof(kStates[0]); i++) if (saved[i] != want[i]) device->SetRenderState(kStates[i], saved[i]);
		if (viewSaved) device->SetVertexShaderConstantF(252, &view[0][0], 4);
		if (projSaved) device->SetVertexShaderConstantF(248, &proj[0][0], 4);
		if (SpotMode) device->SetVertexShaderConstantF(13, &spotSaved[0][0], 6);
		if (bendSavedOk) device->SetVertexShaderConstantF(0, &bendSaved[0][0], 2);
		return hr;
	}

	inline void Unhook(IDirect3DDevice9* device) {
		if (!Hooked || !device) return;
		void** table = *(void***)device;
		if (table[82] == (void*)&DrawHook) {
			DWORD old = 0;
			if (VirtualProtect(&table[82], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
				table[82] = (void*)Next;
				VirtualProtect(&table[82], sizeof(void*), old, &old);
			}
		}
		Hooked = false;
	}

	// After GrassFirst's early grass: a cascade of the shadow atlas (near, middle or far) becomes the target and the draws are ours.
	// False: nothing to do (off, no early grass, indoors, no sun shadow maps this frame, not PCF; past the near cascade also Reach 0,
	// the cascade starting past Reach or not redrawn this frame), and nothing was changed.
	inline bool Begin(IDirect3DDevice9* device, int cascade = ShadowsExteriorEffect::MapNear, bool oldAtlas = false) {
		Active = false;
		if (!Enabled || Records.empty() || !device || !TheShaderManager->GameState.isExterior) return false;
		if (cascade < ShadowsExteriorEffect::MapNear || cascade > ShadowsExteriorEffect::MapFar) return false;
		if (cascade > ShadowsExteriorEffect::MapNear && (!(Reach > 0.0f) || !(SunCascadesDrawnMask & (1u << cascade)))) return false;
		ShadowsExteriorEffect* shadows = TheShaderManager->Effects.ShadowsExteriors;
		if (!shadows || !shadows->Enabled || !shadows->ShadowAtlasSurface || !shadows->ShadowAtlasDepthSurface) return false;
		if (oldAtlas && (!shadows->DeferredCaptureReady || !shadows->ShadowAtlasOldSurface)) return false;
		IDirect3DSurface9* const target = oldAtlas ? shadows->ShadowAtlasOldSurface : shadows->ShadowAtlasSurface;
		if (shadows->Settings.ShadowMaps.Mode != 3 || shadows->ShadowAtlasSurfaceMSAA) {
			static bool told = false;
			if (!told) Logger::Log("UNOFFICIAL grass shadows: only with the PCF sun shadow maps ([Shaders.ShadowsExteriors.ShadowMaps] Mode 3, no MSAA): off");
			told = true;
			return false;
		}
		if (!(shadows->Constants.ShadowFade.y > 0.0f) || shadows->Constants.ShadowFade.x >= 1.0f) return false;	// no sun shadow maps this frame
		if (!LoadShaders()) return false;

		const ShadowsExteriorEffect::ShadowMapSettings& map = shadows->ShadowMaps[cascade];
		if (cascade > ShadowsExteriorEffect::MapNear) {
			// Skip only if the previous cascade's actual full-coverage split depth already reaches Reach.
			const D3DXVECTOR4& inner = shadows->ShadowMaps[cascade - 1].ShadowMapCascadeCenterRadius;
			const float innerSplit = ((const float*)&shadows->Constants.ShadowMapRadius)[cascade - 1];
			const float handover = (std::max)(0.1f * inner.w, (std::min)(shadows->Constants.ForwardData.w, inner.w));
			if (innerSplit - handover >= Reach) return false;
		}
		Cascade = cascade;
		CameraToLight = &map.ShadowCameraToLight;
		NearSphere = map.ShadowMapCascadeCenterRadius;
		const D3DXVECTOR4& sd = shadows->Constants.SmoothedSunDir;
		const float sLen = sqrtf(sd.x * sd.x + sd.y * sd.y + sd.z * sd.z);
		SunDir = sLen > 1.0e-4f ? D3DXVECTOR3(sd.x / sLen, sd.y / sLen, sd.z / sLen) : D3DXVECTOR3(0.0f, 0.0f, 1.0f);
		ProbeSet = false;
		// The depth test the shadow maps are drawn with (ShadowManager::RenderShadowMaps).
		ZFunc = (1.0 - NiDX9Renderer::GetSingleton()->m_fZClear) ? D3DCMP_GREATEREQUAL : D3DCMP_LESSEQUAL;
		if (!MinBlendChecked) {
			MinBlendChecked = true;
			IDirect3D9* d3d = nullptr;
			D3DSURFACE_DESC desc = {};
			if (SUCCEEDED(device->GetDirect3D(&d3d)) && d3d && SUCCEEDED(shadows->ShadowAtlasSurface->GetDesc(&desc)))
				MinBlend = SUCCEEDED(d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8,
					D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING, D3DRTYPE_TEXTURE, desc.Format));
			if (d3d) d3d->Release();
			Logger::Log("UNOFFICIAL grass shadows: %s (shadow map format %u, the game's depth clear %.1f: depth test %s)", MinBlend ?
				"the tufts are MIN-blended into the near map (no depth test)" : "the format does not blend: drawn with the depth test",
				(unsigned)desc.Format, NiDX9Renderer::GetSingleton()->m_fZClear, ZFunc == D3DCMP_LESSEQUAL ? "LESSEQUAL" : "GREATEREQUAL");
		}
		// Last frame's pixel count (never waits: read when the card has it).
		if (QueryPending && Query) {
			DWORD pixels = 0;
			const HRESULT q = Query->GetData(&pixels, sizeof(pixels), 0);
			if (q == S_OK) { LastPixels = pixels; PixelsKnown = true; QueryPending = false; }
			else if (q != S_FALSE) { Query->Release(); Query = nullptr; QueryPending = false; }	// device lost or the like: made again
		}

		void** table = *(void***)device;
		if (table[82] != (void*)&DrawHook) {
			DWORD old = 0;
			if (!VirtualProtect(&table[82], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
			Next = (DrawIndexedPrimitiveFn)table[82];
			table[82] = (void*)&DrawHook;
			VirtualProtect(&table[82], sizeof(void*), old, &old);
			Hooked = true;
		}

		device->GetViewport(&SavedViewport);
		for (DWORD i = 0; i < 4; i++) if (FAILED(device->GetRenderTarget(i, &SavedTarget[i]))) SavedTarget[i] = nullptr;
		if (FAILED(device->GetDepthStencilSurface(&SavedDepth))) SavedDepth = nullptr;
		device->SetRenderTarget(0, target);	// (resets the viewport: set after)
		for (DWORD i = 1; i < 4; i++) if (SavedTarget[i]) device->SetRenderTarget(i, nullptr);
		device->SetDepthStencilSurface(shadows->ShadowAtlasDepthSurface);
		device->SetViewport(&map.ShadowMapViewPort);
		AtlasTarget = target;
		AtlasDepth = shadows->ShadowAtlasDepthSurface;
		AtlasViewport = map.ShadowMapViewPort;
		if (!Query) device->CreateQuery(D3DQUERYTYPE_OCCLUSION, &Query);
		QueryOpen = Query && !QueryPending && SUCCEEDED(Query->Issue(D3DISSUE_BEGIN));
		Active = true;
		return true;
	}

	// UNOFFICIAL 2026-11-04 after the sun cascades: the flashlight's spot map (drawn this frame, Map: FlashlightDeferred::ShadowMap's)
	// becomes the target, its matrix (camera-relative world -> its clip space, rows) and the light (camera-relative, w the reach) go to
	// the caster. False: nothing to do (off, no grass passes, no map, the format does not blend), nothing changed.
	inline bool BeginSpot(IDirect3DDevice9* device, IDirect3DTexture9* Map, const D3DXMATRIX& Matrix, const D3DXVECTOR4& Light) {
		Active = false;
		SpotMode = false;
		if (!Enabled || Records.empty() || !device || !Map || !(Light.w > 1.0f)) return false;
		IDirect3DSurface9* surface = nullptr;
		if (FAILED(Map->GetSurfaceLevel(0, &surface)) || !surface) return false;
		surface->Release();	// (the texture keeps it)
		D3DSURFACE_DESC desc = {};
		surface->GetDesc(&desc);
		static int blends = -1;	// the spot map's format MIN-blends (it has no depth buffer of its own here)
		if (blends < 0) {
			IDirect3D9* d3d = nullptr;
			blends = SUCCEEDED(device->GetDirect3D(&d3d)) && d3d && SUCCEEDED(d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8,
				D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING, D3DRTYPE_TEXTURE, desc.Format)) ? 1 : 0;
			if (d3d) d3d->Release();
			Logger::Log("UNOFFICIAL grass shadows: the flashlight's map (format %u) %s", (unsigned)desc.Format, blends ? "MIN-blends: the tufts go into it" : "does not blend: no grass in the flashlight's shadow");
		}
		if (!blends || !LoadShaders() || !LoadSpotShaders()) return false;
		memcpy(SpotMatrix, &Matrix, sizeof(SpotMatrix));
		SpotLight = Light;

		void** table = *(void***)device;
		if (table[82] != (void*)&DrawHook) {
			DWORD old = 0;
			if (!VirtualProtect(&table[82], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
			Next = (DrawIndexedPrimitiveFn)table[82];
			table[82] = (void*)&DrawHook;
			VirtualProtect(&table[82], sizeof(void*), old, &old);
			Hooked = true;
		}
		device->GetViewport(&SavedViewport);
		for (DWORD i = 0; i < 4; i++) if (FAILED(device->GetRenderTarget(i, &SavedTarget[i]))) SavedTarget[i] = nullptr;
		if (FAILED(device->GetDepthStencilSurface(&SavedDepth))) SavedDepth = nullptr;
		device->SetRenderTarget(0, surface);
		for (DWORD i = 1; i < 4; i++) if (SavedTarget[i]) device->SetRenderTarget(i, nullptr);
		device->SetDepthStencilSurface(nullptr);	// (no depth test: MIN blend)
		const D3DVIEWPORT9 full = { 0, 0, desc.Width, desc.Height, 0.0f, 1.0f };
		device->SetViewport(&full);
		AtlasTarget = surface;
		AtlasDepth = nullptr;
		AtlasViewport = full;
		SpotSavedMinBlend = MinBlend;	// (the sun maps' own answer, back in End)
		MinBlend = true;
		QueryOpen = false;
		SpotMode = true;
		Active = true;
		return true;
	}

	inline void End(IDirect3DDevice9* device) {
		if (!Active) return;
		Active = false;
		if (SpotMode) {	// UNOFFICIAL 2026-11-04 the flashlight's map
			SpotMode = false;
			MinBlend = SpotSavedMinBlend;
			Unhook(device);
			device->SetRenderTarget(0, SavedTarget[0]);
			for (DWORD i = 1; i < 4; i++) if (SavedTarget[i]) device->SetRenderTarget(i, SavedTarget[i]);
			device->SetDepthStencilSurface(SavedDepth);
			device->SetViewport(&SavedViewport);
			for (DWORD i = 0; i < 4; i++) if (SavedTarget[i]) { SavedTarget[i]->Release(); SavedTarget[i] = nullptr; }
			if (SavedDepth) { SavedDepth->Release(); SavedDepth = nullptr; }
			if (++SpotFrames <= 3 || SpotFrames % 3600 == 0)
				Logger::Log("UNOFFICIAL grass shadows: frame %u: %u grass passes drawn into the flashlight's shadow map (%u draws; %u skipped: not NVR's grass VS, or the GPU grass strands cast them), %u out of its reach",
					SpotFrames, StatPasses, StatDraws, StatSkipped, StatOutside);
			StatPasses = StatOutside = StatDraws = StatSkipped = StatRetarget = 0;
			return;
		}
		if (QueryOpen) { QueryOpen = false; QueryPending = SUCCEEDED(Query->Issue(D3DISSUE_END)); }
		Unhook(device);
		device->SetRenderTarget(0, SavedTarget[0]);
		for (DWORD i = 1; i < 4; i++) if (SavedTarget[i]) device->SetRenderTarget(i, SavedTarget[i]);
		device->SetDepthStencilSurface(SavedDepth);
		device->SetViewport(&SavedViewport);
		for (DWORD i = 0; i < 4; i++) if (SavedTarget[i]) { SavedTarget[i]->Release(); SavedTarget[i] = nullptr; }
		if (SavedDepth) { SavedDepth->Release(); SavedDepth = nullptr; }
		if (Cascade == ShadowsExteriorEffect::MapNear) Frames++;
		if (Frames <= 3 || Frames == 60 || Frames == 600 || Frames % 3600 == 0) {
			char pixels[32] = "not known yet";
			if (PixelsKnown) sprintf_s(pixels, "%lu", (unsigned long)LastPixels);
			Logger::Log("UNOFFICIAL grass shadows: frame %u: %u early grass passes drawn into the %s sun shadow map (%u draws; %u skipped: not Vegas Shaders' grass vertex shader; %u put back on the map: a pass had changed the target or viewport), %u outside it%s; map pixels written (last read, near map) %s; first piece at %.3f %.3f %.3f in the map (x, y -1..1, z 0..1 to be on it)%s",
				Frames, StatPasses, CascadeNames[Cascade], StatDraws, StatSkipped, StatRetarget, StatOutside, Cascade ? " or past CastShadowsReach" : "", pixels, ProbeLight.x, ProbeLight.y, ProbeLight.z, Black ? "; CastShadowsBlack on" : "");
		}
		Cascade = ShadowsExteriorEffect::MapNear;
		StatPasses = StatOutside = StatDraws = StatSkipped = StatRetarget = 0;
	}
}
