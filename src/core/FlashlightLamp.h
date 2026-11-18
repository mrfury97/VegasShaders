#pragma once
// UNOFFICIAL 2026-11-04 the flashlight as a real game light (user: "make it otherwise behave like a regular light in terms of what it lights up,
// things like env shader flag assets should react the way they do when they get touched by a light at night"). While the deferred light
// (core/FlashlightDeferred.h) is on, the Pip-Boy light -- a real light in the game's own lists, there exactly while the flashlight is on --
// is moved onto the flashlight and given its reach and colour (FlashlightEffect::UpdateConstants), so the game hands it to the objects in
// reach like any lamp: their lamp passes, the leaves, the first-person view, and the env-map fade of BSS2_Envmap_Light_Fade assets
// (ShadowLightShader::SetupGeometryConstants_AmbientEmittance: the env reflection follows the first non-shadow lamp's light at the object).
// Per draw (ForPass, before the game sets the pass up; AfterPass puts it back) its dimmer is:
//  - 0 for the passes of an object the G-buffer holds (it wrote it this frame or the last, NoteWrite from CapturedNormals::BeforeDraw):
//    the light pass lights that already, with its shadow; except its env-map passes (0x244-0x24B), which the light pass does not touch;
//  - else its own times the cone at the object's bound (the share of the bound inside the beam), so an object outside the beam gets none.
// VegasShaders' own light lists (ShaderManager::GetNearbyLights, ManyLamps' table, the RT reflections' lamps, and since 2026-11-07
// perf-flashlight the RT lamp shadows' ManyLights / ExteriorManyLights table, PointShadowForward::NoteManyLamp) leave it out (Is): the light
// pass and the grass's own beam are the flashlight there. So no RT lamp-shadow pass ever traces it (only the path tracer, as its lamp 0).
#include <unordered_set>
#include <unordered_map>
#include <string>
#include <cmath>
#include <algorithm>
#include <d3dx9.h>

namespace FlashlightLamp {
	inline NiPointLight* Lamp = nullptr;	// the Pip-Boy light while it is the flashlight's (nullptr: none)
	inline float Dimmer = 0.0f;				// its dimmer outside the draws
	inline D3DXVECTOR3 Position = {}, Axis = {};	// the light (world) and the beam's axis
	inline float CosOuter = 0.0f, CosInner = 1.0f;
	inline bool Changed = false;
	inline bool SlotSet = false;	// c28 holds this draw's slot (AfterPass clears it)
	// 2026-11-04 (user: hair black in the beam): c28-c30 only for a pixel shader that reads them (declares NVR_FlashLamp: ObjectTemplate). Others
	// use those registers for their own (the hair shaders' LightData[8] runs c21-c28: a light's place and reach). The values are made in
	// ForPass, before the game binds the draw's shaders, and sent there or at the bind (OnBind).
	inline float Pending[3][4] = {};
	inline bool HasPending = false, BoundWants = false;
	inline std::unordered_map<const void*, bool> Declares;	// pixel shader -> declares NVR_FlashLamp (from its constant table, once each)

	inline bool Wants(IDirect3DPixelShader9* shader) {
		if (!shader) return false;
		auto found = Declares.find(shader);
		if (found != Declares.end()) return found->second;
		bool declares = false;
		UINT size = 0;
		if (SUCCEEDED(shader->GetFunction(nullptr, &size)) && size) {
			std::string code(size, '\0');
			ID3DXConstantTable* table = nullptr;
			if (SUCCEEDED(shader->GetFunction(&code[0], &size)) && SUCCEEDED(D3DXGetShaderConstantTable((const DWORD*)code.data(), &table)) && table) {
				declares = table->GetConstantByName(NULL, "NVR_FlashLamp") != NULL;
				table->Release();
			}
		}
		return Declares[shader] = declares;
	}
	inline void Send() { TheRenderManager->device->SetPixelShaderConstantF(28, &Pending[0][0], 3); SlotSet = true; }
	inline std::unordered_set<const void*> Wrote[2];	// geometry that wrote the G-buffer: this frame, the last
	inline int Current = 0;
	inline unsigned long long StatZero = 0, StatCone = 0, StatOut = 0;
	// UNOFFICIAL 2026-11-13 (docs/next/FLASHLIGHT-AUDIT-2026-11-13.md, raster: objects drawn without records got no flashlight) the pass
	// the hooks are drawing now (ForPass .. AfterPass), the pixel shader bound for it, and a look at what the lamp does per pass:
	// with GameLightTest 10 or more, the 60th frame after it was set is logged in full ("FLASHLAMP" lines).
	inline const NiGeometry* PassGeometry = nullptr;
	inline const char* BoundName = "";
	inline bool BoundGame = false;	// the bound pixel shader is the game's own (no record of the mod's)
	inline const char* BoundVertexName = "";
	inline bool BoundVertexGame = false;	// ... and the vertex shader bound with it
	inline int TestMode = 0;		// [Shaders.Flashlight.Deferred] GameLightTest (test only), its ones: 2 = the lamp is never zeroed for an object with records
	inline int DiagRaw = 0, DiagSeen = 0;	// ... the whole value: 10 or more = the look at one frame, 60 frames after each change of it
	inline int DiagNamed = 0, DiagDumps = 0;
	inline char PendLights[160] = "";
	inline unsigned DiagFrames = 0;
	inline bool DiagNow = false;
	inline int DiagLines = 0, DiagOwnLines = 0;
	struct DiagPass { bool valid; const NiGeometry* geometry; UInt16 type; UInt8 lights, maxLights; int index; float dimmer, cone, dist, radius; bool wrote0, wrote1, span; };
	inline DiagPass Pend = {};
	inline unsigned DPasses = 0, DListed = 0, DZero = 0, DCone = 0, DOut = 0, DUnlistedInBeam = 0, DNotes = 0, DNoteNoHook = 0, DNoteOther = 0, DGameInBeam = 0, DGameLit = 0;
	inline const char* BoundPairNote = "";	// (core/GameShaderPairs.h: the bound pair does not fit / was repaired)
	inline unsigned DGameMisfit = 0;
	inline void NoteBind(const char* name, bool game, const char* vertexName, bool vertexGame, const char* pairNote = "") { BoundName = name ? name : ""; BoundGame = game; BoundVertexName = vertexName ? vertexName : ""; BoundVertexGame = vertexGame; BoundPairNote = pairNote ? pairNote : ""; }

	inline bool Is(const void* light) { return light && light == Lamp; }

	inline void BeginFrame() {
		static unsigned frames = 0;
		if (Lamp && ++frames >= 600) {	// every 600 frames with the lamp on
			Logger::Log("UNOFFICIAL FlashlightLamp: per frame %.1f lamp passes left to the light pass, %.1f lit in the beam, %.1f outside it; %u objects in the G-buffer",
				StatZero / (double)frames, StatCone / (double)frames, StatOut / (double)frames, (unsigned)Wrote[Current].size());
			frames = 0; StatZero = StatCone = StatOut = 0;
		}
		Current ^= 1; Wrote[Current].clear();
		if (DiagNow) {
			Logger::Log("FLASHLAMP frame: %u hooked passes; the lamp in the pass's list %u (dimmer 0 because the object wrote records %u, lit by its cone %u, cone 0 %u); in the beam and reach but the lamp NOT in the list %u; the game's own pixel shader on a pass in the beam %u, of them lit by the lamp %u, of those with a shader pair that does not fit (GameShaderPairs) and was not repaired %u; record notes %u (outside a hooked pass %u, the engine's current pass was another object than the hooked one %u); test mode %d",
				DPasses, DListed, DZero, DCone, DOut, DUnlistedInBeam, DGameInBeam, DGameLit, DGameMisfit, DNotes, DNoteNoHook, DNoteOther, TestMode);
			DiagNow = false;
		}
		DPasses = DListed = DZero = DCone = DOut = DUnlistedInBeam = DNotes = DNoteNoHook = DNoteOther = DGameInBeam = DGameLit = DGameMisfit = 0;
		if (DiagRaw != DiagSeen) { DiagSeen = DiagRaw; DiagFrames = 0; }
		if (Lamp && DiagRaw >= 10) {
			if (++DiagFrames == 60) {
				DiagNow = true; DiagLines = DiagOwnLines = DiagNamed = DiagDumps = 0;
				// the lamp as the game's scene holds it: its entry, and the objects the game says it lights
				const ShadowSceneLight* entry = nullptr;
				for (NiTList<ShadowSceneLight>::Entry* e = SceneNode ? SceneNode->lights.start : nullptr; e && !entry; e = e->next) if (e->data && e->data->sourceLight == Lamp) entry = e->data;
				if (!entry) Logger::Log("FLASHLAMP lamp: NOT in the scene's light list (reach %.0f, dimmer %.2f)", Lamp->Spec.r, Lamp->Dimmer);
				else {
					std::string names; int n = 0, rocks = 0;
					for (NiTList<NiGeometry>::Entry* g = entry->kGeometryList.start; g && n < 4000; g = g->next, n++) {
						const char* nm = g->data && g->data->m_pcName ? g->data->m_pcName : "?";
						if (strstr(nm, "Rock") || strstr(nm, "rock")) rocks++;
						if (n < 14) { names += nm; names += ", "; }
					}
					Logger::Log("FLASHLAMP lamp: in the scene's list: enabled %u, point %d, LOD dimmer %.2f, fade %.2f, reach %.0f, dimmer %.2f, at %.0f %.0f %.0f; its own geometry list holds %d objects (%u counted by the game), %d with 'rock' in the name; first: %s",
						(unsigned)entry->bIsEnabled, entry->bPointLight ? 1 : 0, entry->fLODDimmer, entry->fFade, Lamp->Spec.r, Lamp->Dimmer, Lamp->m_worldTransform.pos.x, Lamp->m_worldTransform.pos.y, Lamp->m_worldTransform.pos.z,
						n, (unsigned)entry->kGeometryList.numItems, rocks, names.c_str());
				}
			}
		}
		else DiagFrames = 0;
	}

	// CapturedNormals::BeforeDraw, for a draw that writes the G-buffer: the object of the pass being drawn (BSShaderManager::pCurrentRenderPass).
	inline void NoteWrite() {
		if (!Lamp) return;
		const void* pass = *(void* const*)0x011F91E0;
		const void* engine = pass ? *(void* const*)pass : nullptr;
		DNotes++;
		if (!PassGeometry) DNoteNoHook++; else if (engine != (const void*)PassGeometry) DNoteOther++;
		if (engine) Wrote[Current].insert(engine);
	}

	// The share of a bound inside the beam: the cone (smooth from the outer edge to the full-strength edge, squared as the light pass's)
	// at the bound's nearest angle to the axis.
	inline float ConeAt(const NiBound* bound) {
		if (!bound) return 1.0f;
		const D3DXVECTOR3 v(bound->Center.x - Position.x, bound->Center.y - Position.y, bound->Center.z - Position.z);
		const float dist = D3DXVec3Length(&v);
		if (dist <= bound->Radius + 1.0f) return 1.0f;	// the light is inside it
		const float cosA = (std::clamp)(D3DXVec3Dot(&v, &Axis) / dist, -1.0f, 1.0f);
		const float angle = (std::max)(acosf(cosA) - asinf((std::min)(bound->Radius / dist, 1.0f)), 0.0f);
		const float c = cosf(angle);
		const float t = (std::clamp)((c - CosOuter) / (std::max)(CosInner - CosOuter, 1.0e-4f), 0.0f, 1.0f);
		const float s = t * t * (3.0f - 2.0f * t);
		return s * s;
	}

	// Before the game sets a pass up (Render.cpp ForwardShadowsForPass). pass: BSShaderProperty::RenderPass (+0 geometry, +4 the pass
	// type, +9 the light count, +0xC the ShadowSceneLight list); inGBufferSpan: the world's main list, while the G-buffer is captured.
	inline void ForPass(const void* pass, bool inGBufferSpan) {
		Pend.valid = false;
		PassGeometry = pass ? *(NiGeometry* const*)pass : nullptr;
		if (!Lamp || !pass) return;
		const UInt8* p = (const UInt8*)pass;
		const UInt8 count = p[9];
		ShadowSceneLight* const* lights = *(ShadowSceneLight* const* const*)(p + 0xC);
		int index = -1;
		for (UInt8 i = 0; lights && i < count && index < 0; i++) if (lights[i] && lights[i]->sourceLight == Lamp) index = i;
		NiGeometry* geometry = *(NiGeometry* const*)p;
		const UInt16 type = *(const UInt16*)(p + 4);
		if (DiagNow && geometry) {	// (the look at one frame: this pass, for AfterPass's line once its shader is known)
			const NiBound* b = geometry->m_kWorldBound;
			const float cone = ConeAt(b);
			float dist = 0.0f, radius = 0.0f;
			if (b) { const D3DXVECTOR3 v(b->Center.x - Position.x, b->Center.y - Position.y, b->Center.z - Position.z); dist = D3DXVec3Length(&v); radius = b->Radius; }
			Pend = { true, geometry, type, count, p[0xA], index, 0.0f, cone, dist, radius, Wrote[0].count(geometry) != 0, Wrote[1].count(geometry) != 0, inGBufferSpan };
			PendLights[0] = 0;
			for (UInt8 i = 0; lights && i < count && i < 5; i++) {
				char one[40];
				const ShadowSceneLight* l = lights[i];
				if (!l) snprintf(one, sizeof(one), "[%u empty] ", (unsigned)i);
				else snprintf(one, sizeof(one), "[%u %s r %.0f d %.2f] ", (unsigned)i, l->sourceLight == Lamp ? "FLASH" : l->bPointLight ? "lamp" : "sun", l->sourceLight ? l->sourceLight->Spec.r : 0.0f, l->sourceLight ? l->sourceLight->Dimmer : 0.0f);
				strncat(PendLights, one, sizeof(PendLights) - strlen(PendLights) - 1);
			}
			DPasses++;
			if (index >= 0) DListed++;
			else if (cone > 0.0f && dist - radius < Lamp->Spec.r) DUnlistedInBeam++;
		}
		if (index < 0) return;
		const bool envPass = type >= 0x244 && type <= 0x24B;
		float dimmer;
		if (TestMode != 2 && inGBufferSpan && !envPass && geometry && (Wrote[0].count(geometry) || Wrote[1].count(geometry))) { dimmer = 0.0f; StatZero++; if (DiagNow) DZero++; }
		else {
			const float cone = ConeAt(geometry ? geometry->m_kWorldBound : nullptr);
			dimmer = Dimmer * cone;
			if (cone > 0.0f) { StatCone++; if (DiagNow) DCone++; } else { StatOut++; if (DiagNow) DOut++; }
		}
		Pend.dimmer = dimmer;
		Lamp->Dimmer = dimmer;
		Changed = true;
		// 2026-11-04 the per-pixel cone (ObjectTemplate.hlsl NVR_FlashLamp, c28-c30): which slot of the shader's lamps it is (light k of the
		// pass is PSLightColor[k], one later in the two many-lamp OPT passes: PointShadowForward::FirstShaderLight), and the beam
		const int slot = index - ((type == 0xCA || type == 0xD1) ? 1 : 0);
		if (dimmer > 0.0f && slot >= 0 && Dimmer > 0.0f) {
			const D3DXVECTOR4& cam = TheRenderManager->CameraPosition;
			const float c[3][4] = { { (float)slot + 1.0f, (std::min)(Dimmer / dimmer, 1.0e4f), CosInner, 0.0f },	// (1 / the cone: capped, as it nears 0)
				{ Position.x - cam.x, Position.y - cam.y, Position.z - cam.z, 0.0f }, { Axis.x, Axis.y, Axis.z, CosOuter } };
			memcpy(Pending, c, sizeof(Pending));
			HasPending = true;
			if (BoundWants) Send();
		}
	}
	// SetShadersHook, the pixel shader about to be bound: this draw's values go to it if it reads them; ones sent for the last shader are
	// taken back from one that does not.
	inline void OnBind(IDirect3DPixelShader9* shader) {
		BoundWants = Wants(shader);
		if (BoundWants && HasPending && !SlotSet) Send();
		else if (!BoundWants && SlotSet) { static const float none[3][4] = {}; TheRenderManager->device->SetPixelShaderConstantF(28, &none[0][0], 3); SlotSet = false; }
	}
	inline void AfterPass() {
		if (DiagNow && Pend.valid && Lamp) {	// one frame's look: passes in the beam drawn by the game's own shader, and a few of the mod's the lamp lights
			const bool inBeam = Pend.cone > 0.0f && Pend.dist - Pend.radius < Lamp->Spec.r;
			if (BoundGame && inBeam) { DGameInBeam++; if (Pend.index >= 0 && Pend.dimmer > 0.0f) { DGameLit++; if (strstr(BoundPairNote, "NOT")) DGameMisfit++; } }
			const bool own = !BoundGame && Pend.index >= 0 && Pend.dimmer > 0.0f;
			const char* name = Pend.geometry->m_pcName ? Pend.geometry->m_pcName : "(unnamed)";
			const NiAVObject* parent = (const NiAVObject*)Pend.geometry->m_parent;
			const char* parentName = parent && parent->m_pcName ? parent->m_pcName : "?";
			const bool named = strstr(name, "Rock") || strstr(parentName, "Rock") || strstr(name, "rock") || strstr(parentName, "rock");
			if ((named && DiagNamed < 30 && ++DiagNamed) || (BoundGame && inBeam && Pend.dist < 3000.0f && DiagLines < 16 && ++DiagLines) || (own && DiagOwnLines < 8 && ++DiagOwnLines)) {
				Logger::Log("FLASHLAMP pass 0x%03X %s (%s) with %s (%s)%s '%s' < '%s': lights %u of %u, the lamp %s (index %d), dimmer %.2f of %.2f, cone at its bound %.2f, bound %.0f from the light, radius %.0f, lamp reach %.0f; records this frame %d, last %d; main list %d",
					Pend.type, BoundName, BoundGame ? "the game's shader" : "the mod's shader", BoundVertexName, BoundVertexGame ? "the game's" : "the mod's", BoundPairNote, name, parentName, Pend.lights, Pend.maxLights,
					Pend.index >= 0 ? "IN its list" : "NOT in its list", Pend.index, Pend.dimmer, Dimmer, Pend.cone, Pend.dist, Pend.radius, Lamp->Spec.r, Pend.wrote0 ? 1 : 0, Pend.wrote1 ? 1 : 0, Pend.span ? 1 : 0);
				Logger::Log("FLASHLAMP      its lights: %s", PendLights);
				if (named && Pend.index >= 0 && DiagDumps < 3) {	// what the device holds right after this pass's draw
					DiagDumps++;
					IDirect3DDevice9* d = TheRenderManager->device;
					float ps[5][4] = {}, vs[4][4] = {};
					d->GetPixelShaderConstantF(1, ps[0], 1); d->GetPixelShaderConstantF(3, ps[1], 3); d->GetPixelShaderConstantF(27, ps[4], 1);
					d->GetVertexShaderConstantF(16, vs[0], 1); d->GetVertexShaderConstantF(25, vs[1], 3);
					Logger::Log("FLASHLAMP      device: PS c1 ambient %.3f %.3f %.3f %.3f | c3 light0 %.3f %.3f %.3f %.3f | c4 light1 %.3f %.3f %.3f %.3f | c5 light2 %.3f %.3f %.3f %.3f | c27 toggles %.2f %.2f %.2f %.2f",
						ps[0][0], ps[0][1], ps[0][2], ps[0][3], ps[1][0], ps[1][1], ps[1][2], ps[1][3], ps[2][0], ps[2][1], ps[2][2], ps[2][3], ps[3][0], ps[3][1], ps[3][2], ps[3][3], ps[4][0], ps[4][1], ps[4][2], ps[4][3]);
					Logger::Log("FLASHLAMP      device: VS c16 eye %.1f %.1f %.1f %.2f | c25 light0 %.3f %.3f %.3f %.2f | c26 light1 %.1f %.1f %.1f r %.1f | c27 light2 %.1f %.1f %.1f r %.1f",
						vs[0][0], vs[0][1], vs[0][2], vs[0][3], vs[1][0], vs[1][1], vs[1][2], vs[1][3], vs[2][0], vs[2][1], vs[2][2], vs[2][3], vs[3][0], vs[3][1], vs[3][2], vs[3][3]);
					const NiTexture* attXY = *(NiTexture**)0x011F9534; const NiTexture* attZ = *(NiTexture**)0x011F9538;
					const void* dXY = attXY && attXY->rendererData ? (const void*)attXY->rendererData->dTexture : nullptr;
					const void* dZ = attZ && attZ->rendererData ? (const void*)attZ->rendererData->dTexture : nullptr;
					std::string stages;
					for (DWORD s = 0; s < 8; s++) {
						IDirect3DBaseTexture9* t = nullptr; d->GetTexture(s, &t);
						D3DSURFACE_DESC desc = {};
						if (t && t->GetType() == D3DRTYPE_TEXTURE) ((IDirect3DTexture9*)t)->GetLevelDesc(0, &desc);
						DWORD au = 0, mag = 0, mip = 0, srgb = 0; d->GetSamplerState(s, D3DSAMP_ADDRESSU, &au); d->GetSamplerState(s, D3DSAMP_MAGFILTER, &mag); d->GetSamplerState(s, D3DSAMP_MIPFILTER, &mip); d->GetSamplerState(s, D3DSAMP_SRGBTEXTURE, &srgb);
						char one[120]; snprintf(one, sizeof(one), "s%u %p%s %ux%u f%d addr %u mag %u mip %u srgb %u | ", (unsigned)s, (void*)t, (const void*)t == dXY ? " =attXY" : (const void*)t == dZ ? " =attZ" : "", desc.Width, desc.Height, (int)desc.Format, (unsigned)au, (unsigned)mag, (unsigned)mip, (unsigned)srgb);
						stages += one;
						if (t) t->Release();
					}
					{	// the shaders on the device (their bytecode's length and version: a replaced or repaired shader shows here)
						IDirect3DPixelShader9* dp = nullptr; IDirect3DVertexShader9* dv = nullptr; d->GetPixelShader(&dp); d->GetVertexShader(&dv);
						UINT pn = 0, vn = 0; DWORD pt[2] = {}, vt[2] = {};
						if (dp) { dp->GetFunction(nullptr, &pn); if (pn >= 4) { std::string code(pn, '\0'); if (SUCCEEDED(dp->GetFunction(&code[0], &pn))) memcpy(pt, code.data(), 4); } dp->Release(); }
						if (dv) { dv->GetFunction(nullptr, &vn); if (vn >= 4) { std::string code(vn, '\0'); if (SUCCEEDED(dv->GetFunction(&code[0], &vn))) memcpy(vt, code.data(), 4); } dv->Release(); }
						Logger::Log("FLASHLAMP      device: pixel shader %p, %u bytes, ps_%u_%u | vertex shader %p, %u bytes, vs_%u_%u", (void*)dp, (unsigned)pn, (unsigned)((pt[0] >> 8) & 0xFF), (unsigned)(pt[0] & 0xFF), (void*)dv, (unsigned)vn, (unsigned)((vt[0] >> 8) & 0xFF), (unsigned)(vt[0] & 0xFF));
					}
					DWORD ab = 0, sb = 0, db = 0, cw = 0, zf = 0, zw = 0; d->GetRenderState(D3DRS_ALPHABLENDENABLE, &ab); d->GetRenderState(D3DRS_SRCBLEND, &sb); d->GetRenderState(D3DRS_DESTBLEND, &db); d->GetRenderState(D3DRS_COLORWRITEENABLE, &cw); d->GetRenderState(D3DRS_ZFUNC, &zf); d->GetRenderState(D3DRS_ZWRITEENABLE, &zw);
					Logger::Log("FLASHLAMP      device: the game's falloff textures XY %p Z %p; stages: %s blend %u src %u dst %u, colour write %X, z func %u write %u", dXY, dZ, stages.c_str(), (unsigned)ab, (unsigned)sb, (unsigned)db, (unsigned)cw, (unsigned)zf, (unsigned)zw);
				}
			}
		}
		Pend.valid = false;
		PassGeometry = nullptr;
		if (Changed) { if (Lamp) Lamp->Dimmer = Dimmer; Changed = false; }
		HasPending = false;
		if (SlotSet) { static const float none[4] = {}; TheRenderManager->device->SetPixelShaderConstantF(28, none, 1); SlotSet = false; }	// (no other draw takes it)
	}

	// The lamp let go (the flashlight off, or no deferred light): nothing else to do here, its own values are FlashlightEffect's.
	inline void Release() { AfterPass(); Lamp = nullptr; Wrote[0].clear(); Wrote[1].clear(); }
}
