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
#include "LightFreeRecords.h"
#include "DxvkCompute.h"
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
		// (UNOFFICIAL 2026-11-14 DeferredSun: FD_LIGHT_FREE while the lighting-free records are made: the keep decoded from their COLOR1.a code)
		static D3DXMACRO overrides[] = { { "INTERIOR_SHADOWS", "0" }, { "FORWARD_SHADOWS", "0" }, { "SUN_CROSSFADE", "0" }, { "CONTACT_HARDENING", "0" }, { NULL, NULL }, { NULL, NULL }, { NULL, NULL } };
		int next = 4;
		if (LightFreeRecords::WantedAtStartup()) overrides[next++] = { "FD_LIGHT_FREE", "1" };
		// UNOFFICIAL 2026-11-14 foliage (core/DeferredFoliage.h): a leaf / grass deferred record's COLOR2.b is its sun weight (2 + w): taken as rough
		if (TheSettingManager->GetSettingI("Main.Main.Quality", "DeferredFoliage") != 0 && CapturedNormals::V2CompiledIn()) overrides[next++] = { "FD_FOLIAGE", "1" };
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
	// UNOFFICIAL 2026-11-11 the Flashlight's Angle, Offset, Distance, colour (UseOwnColour) and the Interiors look are already in the NiSpotLight
	// and Settings (FlashlightEffect::UpdateConstants / ApplyLook), so every reader of this gets them; NearFade / HotspotLimit are not in
	// here (the pass's c10.w / c8.x, the path tracer's frame row texel 31).
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
		// (UNOFFICIAL 2026-11-11 BUG FIX: + drawn THIS frame, as the comment above always said. The map stayed "there" in frames
		// ShadowManager did not draw it, and the pass and the grass read the last drawn one through the last matrix.)
		return f && f->SpotLight && f->Deferred.Shadows && f->SpotLight->CastShadows && map && f->ShadowMapFrame == (unsigned)TheFrameRateManager->FrameNumber ? map : nullptr;
	}

	// UNOFFICIAL 2026-11-11 (BUG FIX; a tester on the DXVK build: "flashlight does not appear to load flashlight.png ... Cookie just brightens
	// and darkens the light as its value goes from 0 to 1; at 1 it is invisible"). The cookie was read from the Flashlight EFFECT's sampler list
	// (ExtFileTexture("TESR_SpotLightTexture")), but the 2026-11-04 restructure left that effect a stub with no samplers: the file was never
	// loaded, stage 7 was empty (an empty stage reads black) and lerp(1, black, Cookie) faded the whole beam out. The pass loads its own file
	// now, the first time Cookie is above 0 (TextureManager::GetFileTexture: "Loaded texture file Data\Textures\Effects\Flashlight.png" in
	// the log, or its "[ERROR] : Couldn't load texture file" line and the one below). No file = no cookie (strength 0 to the shader: the
	// clean beam), never a darker one.
	inline IDirect3DBaseTexture9* CookieSlot = nullptr;	// UNOFFICIAL 2026-11-12 the same texture by name, TESR_FlashBeamCookie (the hair shaders' beam, Includes/FlashBeam.hlsl; FlashlightEffect::RegisterTextures)
	inline IDirect3DBaseTexture9* CookieTexture() {
		static bool tried = false;
		static IDirect3DBaseTexture9* texture = nullptr;	// (TextureManager's cache owns it; never freed while the game runs)
		if (!tried) {
			tried = true;
			texture = TheTextureManager->GetFileTexture("Data\\Textures\\Effects\\Flashlight.png",TextureRecord::TextureRecordType::PlanarBuffer);
			CookieSlot = texture;
			if (texture) Logger::Log("UNOFFICIAL FlashlightDeferred: the lens pattern (Data\\Textures\\Effects\\Flashlight.png) loaded for [Shaders.Flashlight.Deferred] Cookie");
			else Logger::Log("[WARNING] UNOFFICIAL FlashlightDeferred: the lens pattern Data\\Textures\\Effects\\Flashlight.png was not found: [Shaders.Flashlight.Deferred] Cookie does nothing (the beam stays clean) until the file is there and the game is restarted");
		}
		return texture;
	}

	// The PCF disc's radius in texels of a map Width wide: ShadowSoftness is in texels of a 512 map (2026-11-04), so the blur is as wide on
	// the beam whatever ShadowMapSize is.
	inline float SoftnessTexels(UINT Width) { return TheShaderManager->Effects.Flashlight->Deferred.ShadowSoftness * (float)Width / 512.0f; }

	// UNOFFICIAL 2026-11-12 HairAndEyes ([Shaders.Flashlight.Deferred] HairAndEyes, default on, live; BUG FIX, report: in the beam "hair
	// transparent / invisible on the forehead", the rest of the hair dark, "eyes discolored"). What the bench showed (Prospector Saloon, a
	// woman's head in the beam, this key off / on, the pass's debug views; log line "CapturedNormals: HairAndEyes: ..."):
	//  - HAIR. The game's own hair, brows and beards (SM3002 / SM3003.pso) are blended with no alpha test, so the game draws them in its
	//    sorted blended list: AFTER the main list, so after this pass, through none of the per-object hooks. The pass never touched them
	//    (the 2026-11-04 depth test had nothing to reject there). Their only flashlight light was the game lamp (core/FlashlightLamp.h),
	//    which those hooks never shaped for them: full strength, no cone, no shadow, on every head in the lamp's reach; and none at all
	//    with GameLight off, with the lamp not among the hair's three lamps, or under path tracing (GameLightUnderPT off): dark hair
	//    blended over a brightly lit scalp.
	//  - EYES. An eyeball writes its records (the eye variant, ObjectTemplate.hlsl NVR_EYE), and the pass lit them like any object's: the
	//    full lamp term with the highlight of a smooth surface, a flat veil in the light's colour over iris and pupil. Its own shader
	//    never does that (a wrapped diffuse; the game's own env pass gives the shine).
	// With the key on, the hair and eye shaders take the beam themselves (Includes/FlashBeam.hlsl; TESR_FlashBeam below, as the grass
	// takes TESR_GrassSpot): the pass's reach, Falloff, cone, NearFade, HotspotLimit and shadow map, on their own colour and normal.
	// The hair leaves the flashlight's game lamp out of its lamps while it does (in its shader: no hook sees those draws). The eye
	// marks its record "far away" in the distance, which the pass skips by its reach test; hair drawn in the main list (a hair mod's
	// alpha-tested hair) writes the same mark over the record under it (core/CapturedNormals.h BeamMarkers), so the pass does not
	// light the scalp through it.
	// The pass's depth margin (FL_Depth below: 1 % of the distance + 2 units) is NOT made smaller. The recorded distance is a half
	// float: 1 part in 1024 (0.5 unit at 1000, 0.03 at 50), so the number itself would allow a twentieth of it. But the margin also
	// covers what the number does not: the per-sample depth of a sloped surface under the game's MSAA against a record taken at the
	// pixel's centre (a few units a pixel at a grazing angle far out), and surfaces the game draws with a depth offset (decals). A
	// tighter test would also still light a scalp through hair lying closer than it; the mark has no such distance.
	// The beam is there from the main list's start (BeamBegin) to the end of that world pass (Render.cpp ShaderSplit: BeamShow), in a
	// frame the pass lights. Any other view (reflections, the first person, a scope's camera), the light or the key off, no records
	// (native Direct3D 9 with the game's MSAA), path tracing replacing the picture: [0].w = 0 and the shaders do what they did before.
	// [0] xyz the light (camera-relative), w its reach; [1] the beam's axis, w cos of the cone's outer edge; [2] rgb linear light x the
	// lamps' light scale, w cos of the full-strength edge; [3] the shadow map (x there, y texel size, z bias, w PCF radius);
	// [4] x Falloff, y NearFade, z HotspotLimit, w Cookie (0 without its file).
	inline D3DXVECTOR4 Beam[5] = {};	// TESR_FlashBeam (FlashlightEffect::RegisterConstants): what a shader bound NOW gets
	inline D3DXVECTOR4 BeamScene[5] = {};	// this world scene's beam, from its main list's start ([0].w 0: none)
	inline unsigned long long StatBeamSpans = 0;
	// UNOFFICIAL 2026-11-13 HairUnderPT ([Shaders.Flashlight.Deferred] HairUnderPT, default on, live; docs/next/FLASHLIGHT-AUDIT-2026-11-13.md).
	// Under path tracing the whole beam above was off ("the path tracer lights the picture"), but the game's blended hair, brows and
	// beards are not always the tracer's: where the traced scene has no hair at the pixel, and everywhere with PTBlendedLayer 1, the
	// GAME's hair is on screen, and it had no flashlight light at all (no beam; the game lamp steps aside under path tracing): dark hair
	// on a head the tracer's flashlight lights. With the key the beam is there under path tracing too, but only AFTER the main list
	// (BeamAfterSpan): for the sorted blended list, where that hair is drawn. Inside the main list nothing changes (no beam, no marks:
	// eyes and alpha-tested hair keep their records as the tracer reads them, and are lit by its lamp 0).
	inline bool BeamAfterSpan = false, BeamSpanOpen = false;
	// Render.cpp ShaderSplit: the shaders get the beam only inside the world's own pass (true), never in a pass nested in it or after it.
	inline void BeamShow(bool world) {
		if (world && BeamScene[0].w > 0.0f && !(BeamAfterSpan && BeamSpanOpen)) memcpy(Beam, BeamScene, sizeof(Beam));
		else Beam[0].w = 0.0f;
	}
	// A new world pass (or a new frame): no beam until its main list starts.
	inline void BeamNewScene() { BeamScene[0].w = 0.0f; Beam[0].w = 0.0f; CapturedNormals::BeamNow = false; CapturedNormals::BeamHeld = false; BeamAfterSpan = false; BeamSpanOpen = false; }
	// The main list's end: no more marks (the light pass draws next); the beam itself stays for the hair the game draws after it.
	inline void BeamSpanEnd() { CapturedNormals::BeamNow = false; BeamSpanOpen = false; if (BeamAfterSpan) BeamShow(true); }	// (2026-11-13 HairUnderPT: the beam from here on)
	inline void BeamBegin() {
		BeamNewScene();
		const FlashlightEffect* f = TheShaderManager->Effects.Flashlight;
		// what Apply needs to light this frame (its own tests, as far as they are known before the list is drawn)
		if (!f || !f->Deferred.HairAndEyes || !Usable() || !f->spotLightActive || !f->SpotLight) return;
		if (!CapturedNormals::InSpan || CapturedNormals::VmSpan || !CapturedNormals::AlbedoTexture || !CapturedNormals::MaterialTexture || !CapturedNormals::Texture) return;
		// the path tracer lights the picture with the flashlight as its own lamp: nothing of this in the main list then; with
		// HairUnderPT (2026-11-13) the beam alone after it, for the game's blended hair
		const bool traced = RtPathTrace::Replacing();
		if (traced && !f->Deferred.HairUnderPT) return;
		D3DXVECTOR4 params[3];
		// (a reach the mark's "far" would not be far for: no such beam in practice; then as before)
		if (!LightParams(params) || params[0].w > 12000.0f || !EnsurePass()) return;
		float scale = 1.0f;	// the lamps' light scale, as the pass's lamp term takes it (Object.hlsl LIN_SCALE_LIGHT; as the grass's beam)
		if (const PBRShaders* pbr = TheShaderManager->Shaders.PBR) if (pbr->Constants.LinearScales.x > 0.0f) scale = pbr->Constants.LinearScales.x;
		BeamScene[0] = params[0];
		BeamScene[1] = params[1];
		BeamScene[2] = D3DXVECTOR4(params[2].x * scale, params[2].y * scale, params[2].z * scale, params[2].w);
		BeamScene[3] = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);
		if (IDirect3DTexture9* map = ShadowMap()) {
			D3DSURFACE_DESC desc = {};
			map->GetLevelDesc(0, &desc);
			if (desc.Width) BeamScene[3] = D3DXVECTOR4(1.0f, 1.0f / (float)desc.Width, f->Deferred.ShadowBias, SoftnessTexels(desc.Width));
		}
		BeamScene[4] = D3DXVECTOR4(f->Deferred.Falloff, f->Settings.NearFade, f->Settings.HotspotLimit, f->Deferred.Cookie > 0.0f && CookieTexture() ? f->Deferred.Cookie : 0.0f);
		BeamAfterSpan = traced; BeamSpanOpen = true;
		BeamShow(true);	// (under path tracing: nothing yet, BeamSpanEnd shows it)
		CapturedNormals::BeamNow = !traced; CapturedNormals::BeamHeld = true;
		CapturedNormals::BeamFrames++;
		if (StatBeamSpans == 40) CapturedNormals::BeamLog();	// (once early: what the first frames with the beam drew)
		if (StatBeamSpans++ == 0) Logger::Log("UNOFFICIAL FlashlightDeferred: HairAndEyes on: hair and eyes take the flashlight's beam in their own shaders; where they are drawn before the light pass they mark their pixels, so the pass does not light them a second time (reach %.0f, shadow map %s)",
			BeamScene[0].w, BeamScene[3].x > 0.5f ? "read" : "not drawn");
		{	// (2026-11-13 HairUnderPT) once per change: which of the two ways the beam is in
			static int told = -1;
			if (told != (traced ? 1 : 0)) { told = traced ? 1 : 0; if (traced) Logger::Log("UNOFFICIAL FlashlightDeferred: HairUnderPT on: under path tracing the game's blended hair (drawn after the main list) takes the flashlight's beam in its own shader; eyes and everything in the main list are the path tracer's"); }
		}
	}

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
		struct Range { UINT reg, count; float v[16][4]; } saved[] = { { 0, 16 }, { 32, 5 }, { 135, 1 }, { 168, 1 }, { 191, 1 }, { 205, 1 }, { 211, 1 }, { 223, 1 } };	// (c15: fd-batchA FL_Model, 2026-11-16)
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
		{	// UNOFFICIAL 2026-11-11 (a tester: "flashlight not casting shadows", no log line said which case): each change, a few times a session
			static int told = 0, last = -1;
			const int state = shadowed ? 1 : 0;
			if (told < 8 && state != last) {
				told++; last = state;
				Logger::Log("UNOFFICIAL FlashlightDeferred: the light pass draws %s its shadow map ([Shaders.Flashlight.Deferred] Shadows %s; the map %s drawn this frame; %s). The player's own body and weapon never cast in it; under path tracing (PTRealtime) the beam on screen is the path tracer's lamp 0 with traced shadows, not this pass",
					shadowed ? "WITH" : "WITHOUT", f->Deferred.Shadows ? "on" : "OFF", f->ShadowMapFrame == (unsigned)TheFrameRateManager->FrameNumber ? "was" : "was NOT",
					TheShaderManager->GameState.isExterior ? "outdoors: casters are the player's own cell's objects, not the land" : "indoors: casters are the cell's objects by [Shaders.ShadowsInteriors.Forms]");
			}
		}
		D3DSURFACE_DESC mapDesc = {};
		if (shadowed) map->GetLevelDesc(0, &mapDesc);
		// (2026-11-11) the cookie: loaded on first use; without the file its strength is 0 (CookieTexture)
		IDirect3DBaseTexture9* cookieTexture = f->Deferred.Cookie > 0.0f ? CookieTexture() : nullptr;
		const float cookieStrength = cookieTexture ? f->Deferred.Cookie : 0.0f;
		const D3DXMATRIX& iv = TheRenderManager->InvViewMatrix;
		const float tanX = fabsf(TheRenderManager->InvProjMatrix._11), tanY = fabsf(TheRenderManager->InvProjMatrix._22);
		const D3DXMATRIX& proj = NiDX9Renderer::GetSingleton()->projMatrix;
		// UNOFFICIAL 2026-11-16 fd-batchA item 7 ([Shaders.Flashlight.Deferred] JitterExact, live): the view ray through the projection the list was
		// drawn with, TAA's jitter (_31 / _32) included, as Includes/DeferredDepth.hlsl DDRay and DeferredLamps::Apply: the shader's ray is
		// Fwd + Right (ndc.x tanX) + Up (ndc.y tanY), so the jitter goes into the forward row (c4); the shader is unchanged. (The depth test's
		// dot(position, FL_Fwd) moves by under 0.1 % of the view z with it, far inside its 1 % + 2 unit pull.) Off: the unjittered ray, as before.
		const bool jitterExact = TheSettingManager->GetSettingI("Shaders.Flashlight.Deferred", "JitterExact") != 0;
		const float jx = jitterExact ? proj._31 * tanX : 0.0f, jy = jitterExact ? proj._32 * tanY : 0.0f;
		float dispSlack[3] = {};
		DxvkCompute::DisplaceRtSlack(dispSlack);
		const float depthPull = 2.0f + (dispSlack[0] > 0.0f ? dispSlack[0] : 0.0f);
		const float own[11][4] = {
			{ proj._33, proj._43, 0.01f, depthPull },	// FL_Depth: the depth of a view z, and the pull toward the camera (1 % and 2 + displace slack units)
			{ 1.0f / (float)vp.Width, 1.0f / (float)vp.Height, (float)vp.X, (float)vp.Y },
			{ iv._11, iv._12, iv._13, tanX },
			{ iv._21, iv._22, iv._23, tanY },
			{ iv._31 - iv._11 * jx - iv._21 * jy, iv._32 - iv._12 * jx - iv._22 * jy, iv._33 - iv._13 * jx - iv._23 * jy, linear ? 1.0f : 0.0f },	// w: the frame is linear (the light added ONE / ONE unless a debug view)
			{ params[0].x, params[0].y, params[0].z, params[0].w },
			{ params[1].x, params[1].y, params[1].z, params[1].w },
			{ params[2].x, params[2].y, params[2].z, params[2].w },
			// (UNOFFICIAL 2026-11-11 xy, were 0: HotspotLimit and SoftEdges, the old flashlight's keys; 0 = today's light)
			{ f->Settings.HotspotLimit, f->Settings.softEdges ? 1.0f : 0.0f, gamma, 1.0f / gamma },
			{ shadowed ? 1.0f : 0.0f, mapDesc.Width ? 1.0f / (float)mapDesc.Width : 0.0f, f->Deferred.ShadowBias, SoftnessTexels(mapDesc.Width) },
			{ cookieStrength, (float)debugView, f->Deferred.Falloff, f->Settings.NearFade } };	// (w: UNOFFICIAL 2026-11-11 NearFade, was 0)
		d->SetPixelShaderConstantF(0, own[0], 11);
		d->SetPixelShaderConstantF(11, (const float*)&TheShaderManager->SpotLightWorldToLightMatrix[0], 4);	// row_major in the pass
		// UNOFFICIAL 2026-11-16 fd-batchA item 6 ([Shaders.Flashlight.Deferred] SpecularBit, live; FlashlightDeferred.hlsl FL_Model): the record's
		// specular bit picks the beam's model (PBRDiffuse on the records the game draws without a highlight), as the lamp pass does
		const float model[4] = { TheSettingManager->GetSettingI("Shaders.Flashlight.Deferred", "SpecularBit") != 0 ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f };
		d->SetPixelShaderConstantF(15, model, 1);

		d->SetVertexShader(nullptr);
		d->SetPixelShader(Pass->ShaderHandle);
		d->SetTexture(0, linear && !debugView ? nullptr : CopyTex);
		d->SetTexture(1, CapturedNormals::AlbedoTexture);
		d->SetTexture(4, CapturedNormals::MaterialTexture);
		d->SetTexture(5, CapturedNormals::Texture);
		d->SetTexture(6, shadowed ? map : nullptr);
		d->SetTexture(7, cookieTexture);	// (2026-11-11: was the stub effect's sampler, always null)
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
