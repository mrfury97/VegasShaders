#pragma once
// UNOFFICIAL 2026-11-04 deferred flashlight ([Shaders.Flashlight.Deferred]; user: "what I really want is an actual working real spotlight for
// the flashlight"). The Flashlight effect's spot light (FlashlightEffect: its position on the weapon or the camera, cone, reach, colour,
// shadow map) lights the world's surfaces in ONE full-screen pass (Shaders/FlashlightDeferred.hlsl) after the main list's G-buffer is
// resolved and before LinearScene encodes the frame (Render.cpp DeferredSpan, after DeferredLamps::Apply): each pixel's own base colour,
// roughness, metal and normal through the object shaders' lamp maths, the cone, the light's shadow map and its cookie, added in linear
// light. Needs the G-buffer ([Main.Main.Quality] CapturedNormals, MaterialBuffer, AlbedoBuffer). 2026-11-04: the flashlight's only light
// on the world's surfaces (the old screen-space one is gone, effects/Flashlight/Flashlight.h); without the G-buffer, or if the pass did not
// load, nothing but the grass's beam and the game lamp (core/FlashlightLamp.h) light with it.
#include "CapturedNormals.h"
#include "LinearScene.h"
#include "ShaderRecord.h"
namespace FlashlightDeferred {
	inline ShaderRecordPixel* Pass = nullptr;
	inline bool PassFailed = false;
	inline unsigned DrewFrame = ~0u;	// TheFrameRateManager->FrameNumber of the last frame the pass drew
	inline IDirect3DTexture9* CopyTex = nullptr;
	inline IDirect3DSurface9* Copy = nullptr;
	inline D3DSURFACE_DESC CopyMade = {};
	inline unsigned long long StatPasses = 0, StatSkipped = 0;

	// Set up to draw: the effect and its deferred mode on, the G-buffer compiled in, the pass not failed.
	inline bool Usable() {
		const FlashlightEffect* f = TheShaderManager->Effects.Flashlight;
		return f && f->Enabled && CapturedNormals::AlbedoCompiledIn() && !PassFailed;
	}
	// The G-buffer capture's reader (Render.cpp CapturedNormals::BeginFrame): while the light is on.
	inline bool Wanted() { return Usable() && TheShaderManager->Effects.Flashlight->spotLightActive; }
	inline bool DrewThisFrame() { return DrewFrame == (unsigned)TheFrameRateManager->FrameNumber; }

	inline bool EnsurePass() {
		if (Pass || PassFailed) return Pass != nullptr;
		static D3DXMACRO overrides[] = { { "INTERIOR_SHADOWS", "0" }, { "FORWARD_SHADOWS", "0" }, { "SUN_CROSSFADE", "0" }, { "CONTACT_HARDENING", "0" }, { NULL, NULL } };
		ShaderTemplate tmpl = {};
		tmpl.Name = "FlashlightDeferred";
		Pass = (ShaderRecordPixel*)ShaderRecord::LoadShader("FlashlightDeferred.pso", NULL, tmpl, "Deferred\\Flashlight\\", overrides);
		if (Pass) Logger::Log("UNOFFICIAL FlashlightDeferred: the flashlight's light pass loaded");
		else { PassFailed = true; Logger::Log("[ERROR] UNOFFICIAL FlashlightDeferred: the light pass (Shaders\\FlashlightDeferred.hlsl) did not load: the flashlight lights only the grass (and with GameLight what the game lamp reaches) from now on"); }
		return Pass != nullptr;
	}

	// The light as the pass and the grass take it (camera-relative to TheRenderManager->CameraPosition): [0] xyz the light, w its reach;
	// [1] xyz the beam's axis, w cos of the cone's outer edge; [2] rgb the colour (linear, its hue at Intensity), w cos of the full-strength
	// edge (half the angle). False while the light is off or cannot draw (2026-11-04: the grass, which the G-buffer does not hold).
	inline bool LightParams(D3DXVECTOR4 out[3]) {
		const FlashlightEffect* f = TheShaderManager->Effects.Flashlight;
		if (!Usable() || !f->spotLightActive || !f->SpotLight) return false;
		const NiSpotLight& light = *f->SpotLight;
		const float reach = light.Spec.r;
		if (!(reach > 1.0f)) return false;
		const D3DXVECTOR4& cam = TheRenderManager->CameraPosition;
		const NiPoint3& lp = light.m_worldTransform.pos;
		D3DXVECTOR3 axis(light.m_worldTransform.rot.data[0][0], light.m_worldTransform.rot.data[1][0], light.m_worldTransform.rot.data[2][0]);
		D3DXVec3Normalize(&axis, &axis);
		const float outer = D3DXToRadian((std::min)((std::max)(light.OuterSpotAngle, 1.0f), 89.0f));
		const NiColor& c = f->Settings.Color;
		const float peak = (std::max)((std::max)(c.r, c.g), (std::max)(c.b, 1.0e-4f));
		float gammaLight = 1.0f;
		if (const PBRShaders* pbr = TheShaderManager->Shaders.PBR) if (pbr->Constants.Linear.x > 1.0f) gammaLight = pbr->Constants.Linear.x;
		const float intensity = (std::max)(f->Settings.Dimmer, 0.0f);	// ([Shaders.Flashlight.Main] Dimmer, 2026-11-04: was Deferred Intensity)
		out[0] = D3DXVECTOR4(lp.x - cam.x, lp.y - cam.y, lp.z - cam.z, reach);
		out[1] = D3DXVECTOR4(axis.x, axis.y, axis.z, cosf(outer));
		out[2] = D3DXVECTOR4(powf(c.r / peak, gammaLight) * intensity, powf(c.g / peak, gammaLight) * intensity, powf(c.b / peak, gammaLight) * intensity, cosf(outer * 0.5f));
		return true;
	}

	// The flashlight's shadow map when it was drawn this frame for its shadows (ShadowManager, FlashlightEffect::WantsShadowMap); nullptr
	// otherwise. The light pass and the grass (2026-11-04) read it with SpotLightWorldToLightMatrix[0].
	inline IDirect3DTexture9* ShadowMap() {
		const FlashlightEffect* f = TheShaderManager->Effects.Flashlight;
		ShadowsExteriorEffect* shadows = TheShaderManager->Effects.ShadowsExteriors;
		IDirect3DTexture9* map = shadows ? shadows->Textures.ShadowSpotlightTexture[0] : nullptr;
		return f && f->SpotLight && f->Deferred.Shadows && f->SpotLight->CastShadows && map ? map : nullptr;
	}

	// The PCF disc's radius in texels of a map Width wide: ShadowSoftness is in texels of a 512 map (2026-11-04), so the blur is as wide on
	// the beam whatever ShadowMapSize is.
	inline float SoftnessTexels(UINT Width) { return TheShaderManager->Effects.Flashlight->Deferred.ShadowSoftness * (float)Width / 512.0f; }

	// The pass: after CapturedNormals::EndSpan (G-buffer unbound and resolved), before LinearScene::EndSpan (Render.cpp DeferredSpan).
	inline void Apply(IDirect3DDevice9* d) {
		FlashlightEffect* f = TheShaderManager->Effects.Flashlight;
		if (!Usable() || !f->spotLightActive || !f->SpotLight) return;
		if (!CapturedNormals::Drawn || !CapturedNormals::AlbedoTexture || !CapturedNormals::MaterialTexture || !CapturedNormals::Texture || !EnsurePass()) {
			// UNOFFICIAL 2026-11-06 (tester log, native D3D9 + iMultiSample 4: Cookie 0 -> 1 changed nothing, "0 objects in the G-buffer"): say once
			// why the beam has no shadow or cookie, so a log shows it.
			static bool told = false;
			if (!told && CapturedNormals::MsaaWarned) {
				told = true;
				Logger::Log("[WARNING] UNOFFICIAL FlashlightDeferred: the flashlight's light pass needs the G-buffer, which the game's MSAA turns off on native D3D9 (see the CapturedNormals warning): the beam lights through the game lamp only, with no shadow and no cookie. For the full beam: Vegas Shaders DXVK, or iMultiSample=0 in the game's INIs");
			}
			StatSkipped++; return;
		}
		D3DXVECTOR4 params[3];
		if (!LightParams(params)) { StatSkipped++; return; }
		IDirect3DSurface9* target = nullptr;
		d->GetRenderTarget(0, &target);
		if (!target) { StatSkipped++; return; }
		D3DSURFACE_DESC t = {};
		target->GetDesc(&t);
		const bool linear = LinearScene::Linear;
		const int debugView = f->Deferred.DebugView;
		if (!linear || debugView) {	// the copy path: the frame's copy for enc(dec(v) + L) (and the debug views, written over the frame)
			if (!CopyTex || CopyMade.Width != t.Width || CopyMade.Height != t.Height || CopyMade.Format != t.Format) {
				if (Copy) { Copy->Release(); Copy = nullptr; }
				if (CopyTex) { CopyTex->Release(); CopyTex = nullptr; }
				if (FAILED(d->CreateTexture(t.Width, t.Height, 1, D3DUSAGE_RENDERTARGET, t.Format, D3DPOOL_DEFAULT, &CopyTex, NULL)) || FAILED(CopyTex->GetSurfaceLevel(0, &Copy))) {
					if (Copy) { Copy->Release(); Copy = nullptr; }
					if (CopyTex) { CopyTex->Release(); CopyTex = nullptr; }
					target->Release(); StatSkipped++; return;
				}
				CopyMade = t;
			}
			if (FAILED(d->StretchRect(target, NULL, Copy, NULL, D3DTEXF_NONE))) { target->Release(); StatSkipped++; return; }
		}
		static GpuTimer timer("Deferred flashlight");
		GpuProfileScope gpu(timer, d);

		D3DVIEWPORT9 vp = {}; d->GetViewport(&vp);
		IDirect3DSurface9* rt[4] = {};
		for (DWORD i = 1; i < 4; i++) if (SUCCEEDED(d->GetRenderTarget(i, &rt[i])) && rt[i]) d->SetRenderTarget(i, nullptr);
		IDirect3DVertexShader9* vs = nullptr; IDirect3DPixelShader9* ps = nullptr; IDirect3DVertexDeclaration9* decl = nullptr;
		IDirect3DVertexBuffer9* vb = nullptr; UINT vbOffset = 0, vbStride = 0; DWORD fvf = 0;
		d->GetVertexShader(&vs); d->GetPixelShader(&ps); d->GetVertexDeclaration(&decl); d->GetFVF(&fvf); d->GetStreamSource(0, &vb, &vbOffset, &vbStride);
		static const DWORD kStages[] = { 0, 1, 4, 5, 6, 7 };
		IDirect3DBaseTexture9* tex[6] = {};
		for (int s = 0; s < 6; s++) d->GetTexture(kStages[s], &tex[s]);
		// every constant the pass sets, saved and put back
		struct Range { UINT reg, count; float v[16][4]; } saved[] = { { 0, 15 }, { 32, 5 }, { 135, 1 }, { 168, 1 }, { 191, 1 }, { 205, 1 }, { 211, 1 }, { 223, 1 } };
		for (Range& r : saved) d->GetPixelShaderConstantF(r.reg, &r.v[0][0], r.count);
		// 2026-11-04 depth-tested (not written) at each pixel's G-buffer surface, the shader's own depth (FlashlightDeferred.hlsl): what was
		// drawn over that surface without writing the G-buffer (hair) is not lit as the surface behind it. The game's depth runs either way
		// (reversed while it clears to 0, as ShadowManager draws its maps).
		const DWORD zFunc = (1.0 - NiDX9Renderer::GetSingleton()->m_fZClear) ? D3DCMP_GREATEREQUAL : D3DCMP_LESSEQUAL;
		static const D3DRENDERSTATETYPE kRS[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_ALPHATESTENABLE, D3DRS_CULLMODE, D3DRS_STENCILENABLE,
			D3DRS_SCISSORTESTENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_COLORWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND,
			D3DRS_BLENDOP, D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_FOGENABLE };
		const bool add = !debugView;	// 2026-11-04 the copy path adds too (the difference its light makes: FlashlightDeferred.hlsl); the debug views write over
		const DWORD want[] = { D3DZB_TRUE, FALSE, zFunc, FALSE, D3DCULL_NONE, FALSE, FALSE, FALSE, 0x7, add ? (DWORD)TRUE : (DWORD)FALSE, D3DBLEND_ONE, D3DBLEND_ONE,
			D3DBLENDOP_ADD, FALSE, FALSE };
		static_assert(sizeof(kRS) / sizeof(kRS[0]) == sizeof(want) / sizeof(want[0]), "one value per state");
		DWORD savedRS[sizeof(kRS) / sizeof(kRS[0])];
		for (unsigned i = 0; i < sizeof(kRS) / sizeof(kRS[0]); i++) { if (FAILED(d->GetRenderState(kRS[i], &savedRS[i]))) savedRS[i] = want[i]; if (savedRS[i] != want[i]) d->SetRenderState(kRS[i], want[i]); }
		static const D3DSAMPLERSTATETYPE kSS[] = { D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE };
		DWORD savedSS[6][sizeof(kSS) / sizeof(kSS[0])];
		for (int s = 0; s < 6; s++) {
			const bool linearFilter = kStages[s] == 7;	// the cookie
			const DWORD wantS[] = { linearFilter ? (DWORD)D3DTEXF_LINEAR : (DWORD)D3DTEXF_POINT, linearFilter ? (DWORD)D3DTEXF_LINEAR : (DWORD)D3DTEXF_POINT, D3DTEXF_NONE, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, FALSE };
			for (unsigned i = 0; i < sizeof(kSS) / sizeof(kSS[0]); i++) { d->GetSamplerState(kStages[s], kSS[i], &savedSS[s][i]); if (savedSS[s][i] != wantS[i]) d->SetSamplerState(kStages[s], kSS[i], wantS[i]); }
		}

		// the object shaders' frame globals in their own registers (DeferredLamps::Apply's list, less the lamp shadows')
		static const struct { const char* name; UINT reg; } kGlobals[] = { { "TESR_PBRData", 32 }, { "TESR_PBRExtraData", 33 }, { "TESR_PBRMetal", 34 },
			{ "TESR_PBRMetalLook", 36 }, { "TESR_DebugVar", 135 }, { "TESR_LinearScales", 168 }, { "TESR_LinearLighting", 191 }, { "TESR_LampFalloff", 205 },
			{ "TESR_PBRMath", 223 } };
		for (const auto& g : kGlobals) {
			auto found = TheShaderManager->ConstantsTable.find(g.name);
			if (found != TheShaderManager->ConstantsTable.end() && found->second) d->SetPixelShaderConstantF(g.reg, (const float*)found->second, 1);
		}
		const float one[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
		d->SetPixelShaderConstantF(211, one, 1);	// NVR_SpecFade: full

		// the light: camera-relative, its axis, the cone (outer edge, full strength from half its angle in), the colour (LightParams)
		const float gamma = LinearScene::Gamma > 1.0f ? LinearScene::Gamma : 2.2f;
		// the shadow map: drawn this frame by ShadowManager (FlashlightEffect::WantsShadowMap)
		IDirect3DTexture9* map = ShadowMap();
		const bool shadowed = map != nullptr;
		D3DSURFACE_DESC mapDesc = {};
		if (shadowed) map->GetLevelDesc(0, &mapDesc);
		const D3DXMATRIX& iv = TheRenderManager->InvViewMatrix;
		const float tanX = fabsf(TheRenderManager->InvProjMatrix._11), tanY = fabsf(TheRenderManager->InvProjMatrix._22);
		const D3DXMATRIX& proj = NiDX9Renderer::GetSingleton()->projMatrix;
		const float own[11][4] = {
			{ proj._33, proj._43, 0.01f, 2.0f },	// FL_Depth: the depth of a view z, and the pull toward the camera (1 % and 2 units)
			{ 1.0f / (float)vp.Width, 1.0f / (float)vp.Height, (float)vp.X, (float)vp.Y },
			{ iv._11, iv._12, iv._13, tanX },
			{ iv._21, iv._22, iv._23, tanY },
			{ iv._31, iv._32, iv._33, linear ? 1.0f : 0.0f },	// w: the frame is linear (the light added ONE / ONE unless a debug view)
			{ params[0].x, params[0].y, params[0].z, params[0].w },
			{ params[1].x, params[1].y, params[1].z, params[1].w },
			{ params[2].x, params[2].y, params[2].z, params[2].w },
			{ 0.0f, 0.0f, gamma, 1.0f / gamma },
			{ shadowed ? 1.0f : 0.0f, mapDesc.Width ? 1.0f / (float)mapDesc.Width : 0.0f, f->Deferred.ShadowBias, SoftnessTexels(mapDesc.Width) },
			{ f->Deferred.Cookie, (float)debugView, f->Deferred.Falloff, 0.0f } };
		d->SetPixelShaderConstantF(0, own[0], 11);
		d->SetPixelShaderConstantF(11, (const float*)&TheShaderManager->SpotLightWorldToLightMatrix[0], 4);	// row_major in the pass

		d->SetVertexShader(nullptr);
		d->SetPixelShader(Pass->ShaderHandle);
		d->SetTexture(0, linear && !debugView ? nullptr : CopyTex);
		d->SetTexture(1, CapturedNormals::AlbedoTexture);
		d->SetTexture(4, CapturedNormals::MaterialTexture);
		d->SetTexture(5, CapturedNormals::Texture);
		d->SetTexture(6, shadowed ? map : nullptr);
		d->SetTexture(7, f->ExtFileTexture("TESR_SpotLightTexture"));
		d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
		const float x0 = (float)vp.X - 0.5f, y0 = (float)vp.Y - 0.5f, x1 = (float)(vp.X + vp.Width) - 0.5f, y1 = (float)(vp.Y + vp.Height) - 0.5f;
		const float u0 = (float)vp.X / t.Width, v0 = (float)vp.Y / t.Height, u1 = (float)(vp.X + vp.Width) / t.Width, v1 = (float)(vp.Y + vp.Height) / t.Height;
		const float quad[4][6] = { { x0, y0, 0, 1, u0, v0 }, { x1, y0, 0, 1, u1, v0 }, { x0, y1, 0, 1, u0, v1 }, { x1, y1, 0, 1, u1, v1 } };
		d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
		StatPasses++;
		DrewFrame = (unsigned)TheFrameRateManager->FrameNumber;

		for (int s = 0; s < 6; s++) {
			for (unsigned i = 0; i < sizeof(kSS) / sizeof(kSS[0]); i++) d->SetSamplerState(kStages[s], kSS[i], savedSS[s][i]);
			d->SetTexture(kStages[s], tex[s]); if (tex[s]) tex[s]->Release();
		}
		for (unsigned i = 0; i < sizeof(kRS) / sizeof(kRS[0]); i++) if (savedRS[i] != want[i]) d->SetRenderState(kRS[i], savedRS[i]);
		for (Range& r : saved) d->SetPixelShaderConstantF(r.reg, &r.v[0][0], r.count);
		for (DWORD i = 1; i < 4; i++) if (rt[i]) { d->SetRenderTarget(i, rt[i]); rt[i]->Release(); }
		d->SetViewport(&vp);
		if (decl) { d->SetVertexDeclaration(decl); decl->Release(); } else d->SetFVF(fvf);
		d->SetStreamSource(0, vb, vbOffset, vbStride); if (vb) vb->Release();
		d->SetVertexShader(vs); if (vs) vs->Release();
		d->SetPixelShader(ps); if (ps) ps->Release();
		target->Release();
	}
}
