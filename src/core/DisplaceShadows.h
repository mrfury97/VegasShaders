#pragma once

// UNOFFICIAL 2026-10-27 displacement shadows ([Shaders.Terrain.Displacement] CastShadows, on by default, live; plan step 2 of
// docs/next/FORK2-DISPLACE.md). With NvrDxvkDisplacement the camera sees the land's bumps (Vulkan tessellation through the NVR DXVK
// fork's 1.7-displace), but the sun's shadow cascades were drawn with NVR's caster shaders (ShadowMap.vso / ShadowMapOpaque.pso),
// which have nothing to tessellate with: the maps held the flat land. So the bumps cast no shadows, and the land's own shadow lookup
// had to stay at or above the flat land (TerrainDisplace.tese).
//
// Now the cell land near the camera goes into the sun cascades through the land's own tessellation: Shadows\ShadowMapLand.vso with
// the land's TCS (family 2 of the fork's registrations, DxvkCompute::RegisterDisplace) and ShadowMapLand-t<N>.pso with the
// SHADOW_CASTER variants of TerrainDisplace.tese. The displacement is the camera's, point for point:
//  - the splitting, the distance fade and the quadrant borders are measured from the MAIN camera (its eye in object space goes to the
//    TCS), not from the light, so the same points move by the same amounts;
//  - the heights come from the land's own base maps (alpha) at s0.., blended by the same vertex weights, filtered with the land's
//    own sampler states; which layers have a height map (LandHeight, c34-c35) is what Vanilla Plus Terrain set for that land piece:
//    it fills them per draw through the game's constant map, nowhere NVR could read them, so they are taken from the land's own draws
//    in the world scene (a DrawIndexedPrimitive hook for the world scene's span only: land pixel shader bound -> its vertex buffer
//    and base vertex as the key, the base map addresses, c34-c35 and s0's sampler states), and used for the cascades of the next
//    frames. A piece the camera has not drawn yet (never on screen this session) keeps the flat caster;
//  - ObjectBlend (the bumps fading where the land meets rocks, feet, buildings) reads LAST frame's depth copy, with last frame's
//    camera: the shadow maps are drawn before the world scene, so this frame's copy does not exist yet. A frame late, the same for
//    anything standing still; a moving object's fade trails it by a frame.
// The pixel shader writes the depth from the displaced clip position, as ShadowMapOpaque.pso does (PCF maps, always now).
//
// Mismatches left (all small, within the maps' bias): ObjectBlend a frame late (above); the cascades drawn every few frames
// (CachedDistantShadows / NearCascadeInterval) keep the bumps as they were when drawn; land outside FadeEnd keeps the flat caster
// (the bumps are gone there anyway). The ortho map (rain, skylighting), the light grid's sun map and the lamps' cube maps keep the
// flat land.
//
// Cost: the land within FadeEnd is tessellated again for every cascade it reaches (F10 "Sun cascade: displaced land"); land farther
// away is drawn as before.

#include <unordered_map>
#include "DxvkCompute.h"
#include "GpuProfiler.h"

namespace DisplaceShadows {

	// ---- what the land's own draws used (taken in the world scene, read by the cascades of the next frames) ----
	struct Key {
		IDirect3DVertexBuffer9* vb;
		INT base;
		bool operator==(const Key& other) const { return vb == other.vb && base == other.base; }
	};
	struct KeyHash {
		size_t operator()(const Key& key) const { return (((size_t)key.vb >> 4) * 2654435761u) ^ (size_t)key.base; }
	};
	// s0's sampler states, given to every base map of the shadow draw (the game sets the same on all of them)
	static const D3DSAMPLERSTATETYPE kSamplerStates[7] = { D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER,
		D3DSAMP_MIPFILTER, D3DSAMP_MIPMAPLODBIAS, D3DSAMP_MAXANISOTROPY };
	struct Capture {
		unsigned scene = 0;							// the world scene it was taken in (WorldScenes)
		int layers = 0;								// the land pixel shader's layer count (TEX_COUNT)
		IDirect3DBaseTexture9* textures[7] = {};	// its base maps s0.. (addresses only: checked against the geometry's own textures)
		float height[8] = {};						// LandHeight, c34-c35
		DWORD sampler[7] = {};						// kSamplerStates of s0
	};

	inline std::unordered_map<Key, Capture, KeyHash> Captures;
	inline unsigned WorldScenes = 0;
	inline bool Capturing = false;		// BeginWorld .. EndWorld, while wanted
	inline bool Hooked = false;			// our DrawIndexedPrimitive is in the device's table (or below another hook that went on top)
	inline DWORD Thread = 0;
	inline bool LoadTried = false, LoadFailed = false;	// the caster shaders (LoadShaders)

	typedef HRESULT(STDMETHODCALLTYPE* DrawIndexedPrimitiveFn)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
	inline DrawIndexedPrimitiveFn Next = nullptr;

	// A land draw of the world scene: the first land pass of this scene for that piece (more than 7 layers: the later passes are left).
	inline void Take(IDirect3DDevice9* device, int layers, INT base) {
		IDirect3DVertexBuffer9* vb = nullptr;
		UINT offset = 0, stride = 0;
		if (FAILED(device->GetStreamSource(0, &vb, &offset, &stride)) || !vb) return;
		vb->Release();	// only the address is kept, as the key
		Capture& capture = Captures[Key{ vb, base }];
		if (capture.scene == WorldScenes && capture.layers) return;
		capture.scene = WorldScenes;
		capture.layers = layers;
		for (int i = 0; i < 7; i++) {
			capture.textures[i] = nullptr;
			IDirect3DBaseTexture9* texture = nullptr;
			if (i < layers && SUCCEEDED(device->GetTexture(i, &texture)) && texture) {
				capture.textures[i] = texture;
				texture->Release();
			}
		}
		if (FAILED(device->GetPixelShaderConstantF(34, capture.height, 2))) memset(capture.height, 0, sizeof(capture.height));
		for (int k = 0; k < 7; k++) device->GetSamplerState(0, kSamplerStates[k], &capture.sampler[k]);
		static unsigned logged = 0;	// the first few, to compare with the shadow side's keys (Find)
		if (logged < 4) {
			logged++;
			Logger::Log("NVR TESS shadows: land draw noted: vertex buffer %p, base vertex %d, %d layers, s0 %p, heights %.2f %.2f %.2f %.2f %.2f %.2f %.2f",
				(void*)vb, base, layers, (void*)capture.textures[0], capture.height[0], capture.height[1], capture.height[2], capture.height[3],
				capture.height[4], capture.height[5], capture.height[6]);
		}
	}

	inline HRESULT STDMETHODCALLTYPE DrawHook(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, INT base, UINT minIndex, UINT numVertices, UINT start, UINT count) {
		if (Capturing && GetCurrentThreadId() == Thread) {
			const int layers = DxvkCompute::LandLayersOf(TheRenderManager->renderState->GetPixelShader());
			if (layers) Take(device, layers, base);
		}
		return Next(device, type, base, minIndex, numVertices, start, count);
	}

	// The world scene's span (TerrainShaders::BeginWorldScene / EndWorldScene). GrassFirst's own DrawIndexedPrimitive hooks go on and off
	// inside it, on top of this one, and put it back when they leave.
	inline void BeginWorld(IDirect3DDevice9* device) {
		WorldScenes++;
		Capturing = false;
		if (!device || !TheShaderManager->GameState.isExterior || !DxvkCompute::DisplaceShadowsWanted() || !LoadTried || LoadFailed) return;
		void** table = *(void***)device;
		if (!Hooked) {
			DWORD old = 0;
			if (!VirtualProtect(&table[82], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return;
			Next = (DrawIndexedPrimitiveFn)table[82];
			table[82] = (void*)&DrawHook;
			VirtualProtect(&table[82], sizeof(void*), old, &old);
			Hooked = true;
		}
		if (Captures.size() > 8192) Captures.clear();	// cells come and go: pieces long gone (their buffers are reused, the check catches them)
		Thread = GetCurrentThreadId();
		Capturing = true;
	}

	inline void EndWorld(IDirect3DDevice9* device) {
		Capturing = false;
		if (!Hooked || !device) return;
		void** table = *(void***)device;
		if (table[82] != (void*)&DrawHook) return;	// another hook stayed on top: ours stays under it, passing every draw on (never put in twice)
		DWORD old = 0;
		if (VirtualProtect(&table[82], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
			table[82] = (void*)Next;
			VirtualProtect(&table[82], sizeof(void*), old, &old);
			Hooked = false;
		}
	}

	// ---- the camera of the newest depth copy (ObjectBlend for the shadow draws) ----
	inline unsigned CascadeFrames = 0;		// sun cascade blocks begun (BeginCascades)
	inline unsigned CopyCascadeFrame = ~0u;	// CascadeFrames when the newest depth copy was made
	inline double CopyViewProj[4][4] = {};	// its camera's camera-relative view-projection (row vectors: translation(camera) x ViewProj)
	inline D3DXVECTOR3 CopyCamera = { 0.0f, 0.0f, 0.0f };
	inline float CopyScale[2] = { 1.0f, 1.0f };	// its viewport's share of the copy (RenderScale)

	// Terrain.cpp CopySceneDepth, right after the copy (terrain blending or ObjectBlend): the world camera it was drawn with.
	inline void NoteDepthCopy() {
		CopyCascadeFrame = CascadeFrames;
		const D3DXMATRIX& vp = TheRenderManager->ViewProjMatrix;
		CopyCamera = D3DXVECTOR3(TheRenderManager->CameraPosition.x, TheRenderManager->CameraPosition.y, TheRenderManager->CameraPosition.z);
		for (int c = 0; c < 4; c++) {
			for (int r = 0; r < 3; r++) CopyViewProj[r][c] = vp.m[r][c];
			CopyViewProj[3][c] = (double)CopyCamera.x * vp.m[0][c] + (double)CopyCamera.y * vp.m[1][c] + (double)CopyCamera.z * vp.m[2][c] + vp.m[3][c];
		}
		CopyScale[0] = CopyScale[1] = 1.0f;
		D3DVIEWPORT9 port = {};
		if (SUCCEEDED(TheRenderManager->device->GetViewport(&port)) && TheRenderManager->width && TheRenderManager->height) {
			CopyScale[0] = std::clamp((float)(port.X + port.Width) / (float)TheRenderManager->width, 0.0f, 1.0f);
			CopyScale[1] = std::clamp((float)(port.Y + port.Height) / (float)TheRenderManager->height, 0.0f, 1.0f);
		}
	}

	// ---- the land's shadow pass ----
	inline bool CascadeWalk = false;	// the sun cascades' casters are being sorted (BeginCascades .. EndCascades)
	inline bool BlendFromCopy = false;	// this frame's cascades: ObjectBlend from last frame's depth copy
	inline float Reach = 0.0f;			// FadeEnd: land pieces farther than this from the camera keep the flat caster
	inline ShaderRecordVertex* VS = nullptr;
	inline ShaderRecordPixel* PS[7] = {};
	inline unsigned FramePieces = 0, StatFrames = 0, StatBumpy = 0, StatNoCapture = 0, StatTexturesDiffer = 0, StatNoShader = 0;

	// The caster shaders, once (with the switch on at startup they are registered with the fork as they are created).
	inline bool LoadShaders() {
		if (LoadTried) return !LoadFailed;
		LoadTried = true;
		static const char* const names[7] = { "ShadowMapLand-t1.pso", "ShadowMapLand-t2.pso", "ShadowMapLand-t3.pso", "ShadowMapLand-t4.pso",
			"ShadowMapLand-t5.pso", "ShadowMapLand-t6.pso", "ShadowMapLand-t7.pso" };
		static const char* const counts[7] = { "1", "2", "3", "4", "5", "6", "7" };
		VS = (ShaderRecordVertex*)ShaderRecord::LoadShader("ShadowMapLand.vso", "Shadows\\");
		for (int i = 0; i < 7; i++) PS[i] = (ShaderRecordPixel*)ShaderRecord::LoadShader(names[i], "Shadows\\", ShaderTemplate{ "ShadowMapLand.pso", {{"TEX_COUNT", counts[i]}} });
		const unsigned layers = DxvkCompute::DisplaceShadowLayers();
		LoadFailed = !VS || !layers;
		Logger::Log("UNOFFICIAL displacement shadows: %s (vertex shader %s, pixel shaders registered for layer counts mask 0x%02X)", LoadFailed ?
			"OFF: the land keeps casting flat shadows (copy Shaders\\Shadows\\ShadowMapLand.* and the Compute\\Tess *-s*.tes.spv files)" :
			"the land near the camera is drawn with its bumps into the sun's shadow cascades", VS ? "loaded" : "MISSING", layers);
		return !LoadFailed;
	}

	// The sun cascade block of ShadowManager::RenderShadowMaps.
	inline void BeginCascades() {
		CascadeFrames++;
		FramePieces = 0;
		CascadeWalk = DxvkCompute::DisplaceShadowsWanted() && TheShaderManager->GameState.isExterior && LoadShaders();
		Reach = DxvkCompute::DisplaceReach();
		// The copy made in the world scene after the previous cascades, i.e. last frame's (else none for ObjectBlend this frame)
		// 2026-10-28: only with ObjectBlendShadows on (off: the maps hold the full bumps, DxvkCompute::DisplaceBlendInShadows)
		BlendFromCopy = CopyCascadeFrame + 1 == CascadeFrames && TheTextureManager->TerrainBlendDepthTexture && DxvkCompute::DisplaceBlendInShadows();
	}

	inline void EndCascades() {
		CascadeWalk = false;
		DxvkCompute::DisplaceShadowsDrawn(FramePieces);
		if (!LoadTried || LoadFailed) return;
		if (++StatFrames >= 600) {
			const double f = (double)StatFrames;
			Logger::Log("NVR TESS shadows: per frame %.1f land pieces drawn with their bumps into the sun cascades, flat: %.1f not drawn by the camera yet, %.1f textures differ, %.1f no shader | ObjectBlend from last frame's depth copy %s | %u land pieces known",
				StatBumpy / f, StatNoCapture / f, StatTexturesDiffer / f, StatNoShader / f, BlendFromCopy ? "on" : "off", (unsigned)Captures.size());
			StatFrames = StatBumpy = StatNoCapture = StatTexturesDiffer = StatNoShader = 0;
		}
	}

	// ShadowManager (AccumObject / WalkSunObject, cell land only): does this land piece go through the land's shadow pass?
	inline bool Takes(NiGeometry* geo) {
		if (!CascadeWalk || !geo) return false;
		const NiBound* bound = geo->m_kWorldBound;
		if (!bound) return false;
		const D3DXVECTOR4& cam = TheRenderManager->CameraPosition;
		const float dx = bound->Center.x - cam.x, dy = bound->Center.y - cam.y, dz = bound->Center.z - cam.z;
		const float reach = Reach + bound->Radius;
		if (dx * dx + dy * dy + dz * dz > reach * reach) return false;	// past FadeEnd: no bumps, the flat caster as before
		BSShaderProperty* property = (BSShaderProperty*)geo->GetProperty(NiProperty::PropertyType::kType_Shade);
		return property && property->IsLightingProperty() && ((BSShaderPPLightingProperty*)property)->GetLandscapeLayerCount() > 0;
	}

	// This land piece's capture, when it is still the same land: the base maps it was drawn with are the piece's own layers, in order.
	inline const Capture* Find(NiGeometry* geo) {
		NiGeometryBufferData* data = geo->geomData ? geo->geomData->m_pkBuffData : nullptr;
		if (!data || !data->VBChip || !data->VBChip[0] || !data->VBChip[0]->VB) { StatNoCapture++; return nullptr; }
		auto found = Captures.find(Key{ data->VBChip[0]->VB, (INT)data->BaseVertexIndex });
		if (found == Captures.end()) {
			StatNoCapture++;
			static unsigned logged = 0;	// a land piece never on screen is normal; one the camera did draw would show its buffer above
			if (logged < 4 && !Captures.empty()) {
				logged++;
				unsigned sameBuffer = 0;
				for (const auto& entry : Captures) if (entry.first.vb == data->VBChip[0]->VB) sameBuffer++;
				Logger::Log("NVR TESS shadows: land piece without a noted draw: vertex buffer %p, base vertex %u (chip offset %u), %u noted draws with that buffer",
					(void*)data->VBChip[0]->VB, data->BaseVertexIndex, data->VBChip[0]->Offset, sameBuffer);
			}
			return nullptr;
		}
		const Capture& capture = found->second;
		BSShaderPPLightingProperty* property = (BSShaderPPLightingProperty*)geo->GetProperty(NiProperty::PropertyType::kType_Shade);
		const bool isLighting = property && property->IsLightingProperty();
		NiSourceTexture** textures = isLighting ? property->ppTextures[0] : nullptr;
		const UInt32 propLayers = isLighting ? property->GetLandscapeLayerCount() : 0u;
		bool same = textures && capture.layers >= 1 && capture.layers <= 7 && propLayers >= (UInt32)capture.layers;
		for (int i = 0; same && i < capture.layers; i++) {
			NiSourceTexture* texture = textures[i];
			IDirect3DBaseTexture9* d3d = texture && texture->rendererData ? texture->rendererData->dTexture : nullptr;
			same = d3d && d3d == capture.textures[i];
		}
		if (!same) {
			StatTexturesDiffer++;
			static unsigned logged = 0;
			if (logged < 4) {
				logged++;
				NiSourceTexture* first = textures ? textures[0] : nullptr;
				Logger::Log("NVR TESS shadows: land piece whose textures differ from its noted draw: %d layers noted, %u on the piece, s0 noted %p, the piece's first %p",
					capture.layers, (unsigned)propLayers, (void*)capture.textures[0],
					first && first->rendererData ? (void*)first->rendererData->dTexture : nullptr);
			}
			return nullptr;
		}
		return &capture;
	}

	// c0-c3: last frame's main-camera ModelViewProj of this piece (rows = the clip position's components; ObjectBlend)
	inline void MainRows(NiGeometry* geo, float* rows) {
		const NiTransform& t = geo->m_worldTransform;
		const double s = t.scale;
		const double world[4][4] = {
			{ t.rot.data[0][0] * s, t.rot.data[1][0] * s, t.rot.data[2][0] * s, 0.0 },
			{ t.rot.data[0][1] * s, t.rot.data[1][1] * s, t.rot.data[2][1] * s, 0.0 },
			{ t.rot.data[0][2] * s, t.rot.data[1][2] * s, t.rot.data[2][2] * s, 0.0 },
			{ (double)t.pos.x - CopyCamera.x, (double)t.pos.y - CopyCamera.y, (double)t.pos.z - CopyCamera.z, 1.0 } };
		for (int k = 0; k < 4; k++)
			for (int r = 0; r < 4; r++)
				rows[k * 4 + r] = (float)(world[r][0] * CopyViewProj[0][k] + world[r][1] * CopyViewProj[1][k] + world[r][2] * CopyViewProj[2][k] + world[r][3] * CopyViewProj[3][k]);
	}

	// c4-c7: this cascade's ModelViewProj of this piece (the land dropped for terrain blending as ShadowMap.vso's world matrix is)
	inline void ShadowRows(NiGeometry* geo, const D3DXMATRIX& viewProj, float* rows) {
		RenderPass::UpdateShadowWorld(geo);
		D3DXMATRIX m;
		D3DXMatrixMultiply(&m, &TheShaderManager->ShaderConst.ShadowWorld, &viewProj);
		for (int k = 0; k < 4; k++)
			for (int r = 0; r < 4; r++) rows[k * 4 + r] = m.m[r][k];
	}

	// c8: the main camera in the piece's object space (the game's EyePosition for the land: the TCS measures from it)
	inline void EyeOf(NiGeometry* geo, float* eye) {
		const NiTransform& t = geo->m_worldTransform;
		const D3DXVECTOR4& cam = TheRenderManager->CameraPosition;
		const float dx = cam.x - t.pos.x, dy = cam.y - t.pos.y, dz = cam.z - t.pos.z;
		const float inverse = t.scale != 0.0f ? 1.0f / t.scale : 1.0f;
		for (int i = 0; i < 3; i++) eye[i] = (t.rot.data[0][i] * dx + t.rot.data[1][i] * dy + t.rot.data[2][i] * dz) * inverse;
		eye[3] = 1.0f;
	}

	class LandPass : public RenderPass {
	public:
		LandPass() { VertexShader = nullptr; PixelShader = nullptr; }

		// Before the opaque pass (ShadowManager::RenderAccums): the land pieces sorted here, with their bumps; a piece that cannot be
		// (not drawn by the camera yet, its land changed, no shader for its layer count) goes to 'flat', the opaque pass, as before.
		void Draw(RenderPass* flat) {
			if (GeometryList.empty()) return;
			static GpuTimer timer("Sun cascade: displaced land");	// F10
			IDirect3DDevice9* device = TheRenderManager->device;
			GpuProfileScope gpu(timer, device);
			NiDX9RenderState* renderState = TheRenderManager->renderState;
			const D3DXMATRIX& viewProj = TheShaderManager->Effects.ShadowsExteriors->Constants.ShadowViewProj;
			const unsigned layerMask = DxvkCompute::DisplaceShadowLayers();
			IDirect3DTexture9* depth = TheTextureManager->TerrainBlendDepthTexture;
			DxvkCompute::DisplaceShadowBegin(BlendFromCopy, CopyScale[0], CopyScale[1]);
			// UNOFFICIAL 2026-10-31 ShadowSlack ([Shaders.Terrain.Displacement], live, 0 = as before; user: shadows flicker with FSR / TAA, save
			// NVRAutoTest32). The bumps are finer than a cascade's texel, so the bumpy land's own depth in the map is a few units above or below
			// the bumpy land looked up there: fine "fingerprint" stripes of self-shadow over the sunlit ground, which every jitter step of TAA /
			// FSR samples somewhere else (a still camera: 20 % of the picture flickering at FSR 0.5, 2 % with the sun's shadows off, 8 % with
			// CastShadows off). The displaced land goes into the map that many world units farther from the sun (its depth only), so it no
			// longer shades itself within that height; real bumps taller than it, rocks and everything else cast as before.
			{
				const float slack = std::clamp(TheSettingManager->GetSettingF("Shaders.Terrain.Displacement", "ShadowSlack"), 0.0f, 64.0f);
				const float perUnit = sqrtf(viewProj._13 * viewProj._13 + viewProj._23 * viewProj._23 + viewProj._33 * viewProj._33);	// the map's depth per world unit
				const float c0[4] = { slack * perUnit, 0.0f, 0.0f, 0.0f };
				device->SetPixelShaderConstantF(0, c0, 1);
			}
			renderState->SetVertexShader(VS->ShaderHandle, false);
			IDirect3DPixelShader9* boundPs = nullptr;
			bool depthAtS6 = false;
			while (!GeometryList.empty()) {
				NiGeometry* geo = GeometryList.top();
				GeometryList.pop();
				if (!geo) continue;
				const Capture* capture = Find(geo);
				if (!capture) { flat->GeometryList.push(geo); continue; }
				const int layers = capture->layers;
				if (!(layerMask & (1u << (layers - 1))) || !PS[layers - 1]) { StatNoShader++; flat->GeometryList.push(geo); continue; }
				if (PS[layers - 1]->ShaderHandle != boundPs) {
					boundPs = PS[layers - 1]->ShaderHandle;
					renderState->SetPixelShader(boundPs, false);
				}

				float constants[9][4];
				MainRows(geo, constants[0]);
				ShadowRows(geo, viewProj, constants[4]);
				EyeOf(geo, constants[8]);
				device->SetVertexShaderConstantF(0, &constants[0][0], 9);
				device->SetPixelShaderConstantF(34, capture->height, 2);	// the fork reads which layers have heights from the device's c34-c35
				for (int i = 0; i < layers; i++) {
					renderState->SetTexture(i, capture->textures[i]);
					for (int k = 0; k < 7; k++) renderState->SetSamplerState(i, kSamplerStates[k], capture->sampler[k], false);
				}
				if (layers == 7) depthAtS6 = false;
				else if (!depthAtS6) {	// last frame's depth copy, as TerrainTemplate's TESR_DisplaceDepth (read only while BlendFromCopy)
					renderState->SetTexture(6, depth);
					renderState->SetSamplerState(6, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP, false);
					renderState->SetSamplerState(6, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP, false);
					renderState->SetSamplerState(6, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR, false);
					renderState->SetSamplerState(6, D3DSAMP_MINFILTER, D3DTEXF_LINEAR, false);
					renderState->SetSamplerState(6, D3DSAMP_MIPFILTER, D3DTEXF_NONE, false);
					depthAtS6 = true;
				}
				RenderGeometry(geo);
				StatBumpy++;
				FramePieces++;
			}
			DxvkCompute::DisplaceShadowEnd();
			for (int i = 0; i < 7; i++) renderState->SetTexture(i, nullptr);	// nothing of ours left bound (the depth copy is written again this frame)
		}
	};
	inline LandPass Pass;
}
