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

	inline bool Is(const void* light) { return light && light == Lamp; }

	inline void BeginFrame() {
		static unsigned frames = 0;
		if (Lamp && ++frames >= 600) {	// every 600 frames with the lamp on
			Logger::Log("UNOFFICIAL FlashlightLamp: per frame %.1f lamp passes left to the light pass, %.1f lit in the beam, %.1f outside it; %u objects in the G-buffer",
				StatZero / (double)frames, StatCone / (double)frames, StatOut / (double)frames, (unsigned)Wrote[Current].size());
			frames = 0; StatZero = StatCone = StatOut = 0;
		}
		Current ^= 1; Wrote[Current].clear();
	}

	// CapturedNormals::BeforeDraw, for a draw that writes the G-buffer: the object of the pass being drawn (BSShaderManager::pCurrentRenderPass).
	inline void NoteWrite() {
		if (!Lamp) return;
		const void* pass = *(void* const*)0x011F91E0;
		if (pass) if (const void* geometry = *(void* const*)pass) Wrote[Current].insert(geometry);
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
		if (!Lamp || !pass) return;
		const UInt8* p = (const UInt8*)pass;
		const UInt8 count = p[9];
		ShadowSceneLight* const* lights = *(ShadowSceneLight* const* const*)(p + 0xC);
		int index = -1;
		for (UInt8 i = 0; lights && i < count && index < 0; i++) if (lights[i] && lights[i]->sourceLight == Lamp) index = i;
		if (index < 0) return;
		NiGeometry* geometry = *(NiGeometry* const*)p;
		const UInt16 type = *(const UInt16*)(p + 4);
		const bool envPass = type >= 0x244 && type <= 0x24B;
		float dimmer;
		if (inGBufferSpan && !envPass && geometry && (Wrote[0].count(geometry) || Wrote[1].count(geometry))) { dimmer = 0.0f; StatZero++; }
		else {
			const float cone = ConeAt(geometry ? geometry->m_kWorldBound : nullptr);
			dimmer = Dimmer * cone;
			if (cone > 0.0f) StatCone++; else StatOut++;
		}
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
		if (Changed) { if (Lamp) Lamp->Dimmer = Dimmer; Changed = false; }
		HasPending = false;
		if (SlotSet) { static const float none[4] = {}; TheRenderManager->device->SetPixelShaderConstantF(28, none, 1); SlotSet = false; }	// (no other draw takes it)
	}

	// The lamp let go (the flashlight off, or no deferred light): nothing else to do here, its own values are FlashlightEffect's.
	inline void Release() { AfterPass(); Lamp = nullptr; Wrote[0].clear(); Wrote[1].clear(); }
}
