#pragma once
// UNOFFICIAL 2026-11-04 (user: "lets fade the way lights interact with envmap_light_fade at night"). The env-map shine of assets with the
// BSS2_Envmap_Light_Fade flag follows the light round them: ShadowLightShader::SetupGeometryConstants_AmbientEmittance multiplies the env
// pass's c1.w by F = max(the sun's luminance, the first active non-shadow lamp's luminance x (1 - d / r)), d from the lamp to the OBJECT's
// bound centre. At night (no sun) a lamp makes a whole object gleam at one strength, which jumps as the lamp or the player moves.
// Per env pass (0x244-0x24B, before the game sets it up; Render.cpp ForwardShadowsForPass) the same F is worked out here with the same lamp
// (the game's GetFirstActiveNonShadowLight) and sent with the lamp to EnvTemplate.hlsl (c28-c31), which shades per pixel instead:
// P = max(sun, lamp x (1 - (d/r)^2)^2 at the pixel, times the beam's cone for the flashlight's game lamp), the shine scaled by P / F.
// After the draw c28 goes back to 0 (AfterPass): no other env draw takes it.
#include <cmath>
#include <algorithm>
#include <d3dx9.h>
#include "FlashlightLamp.h"

namespace EnvLightFade {
	inline bool Set = false;
	inline unsigned long long StatPasses = 0;

	inline float Luminance(const NiColor& c) { return c.g * 0.36f + c.r * 0.31f + c.b * 0.32f; }	// NiColor::Luminance 0xB719E0

	inline void ForPass(const void* pass, bool firstPerson = false) {
		if (!pass) return;
		const UInt8* p = (const UInt8*)pass;
		const UInt16 type = *(const UInt16*)(p + 4);
		if (type < 0x244 || type > 0x24B) return;
		NiGeometry* geometry = *(NiGeometry* const*)p;
		if (!geometry) return;
		BSShaderProperty* property = (BSShaderProperty*)geometry->GetProperty(NiProperty::PropertyType::kType_Shade);
		if (!property || !property->IsLightingProperty()) return;
		if (!(property->ulFlags[1] & 0x8000) || (property->ulFlags[0] & 0x20000)) return;	// BSS2_Envmap_Light_Fade, not BSS1_Eye_Environment_Mapping

		void* iter = nullptr;
		ShadowSceneLight* lamp = ThisStdCall<ShadowSceneLight*>(0xB70600, property, &iter);	// BSShaderLightingProperty::GetFirstActiveNonShadowLight
		if (!lamp || !lamp->sourceLight) return;	// the sun alone: one value is right
		const NiPointLight* light = lamp->sourceLight;
		const float radius = light->Spec.r;
		if (!(radius > 1.0f)) return;

		float sunLum = 0.0f;
		NiPoint3 fpOffset = { 0.0f, 0.0f, 0.0f };
		if (ShadowSceneNode* node = BSShaderManager::GetShadowSceneNode(0)) {
			if (node->directionalLight && node->directionalLight->sourceLight) sunLum = Luminance(node->directionalLight->sourceLight->Diff);
			// UNOFFICIAL 2026-11-20: in 1st-person, bound->Center is in 1st-person space (shifted by kLightingOffset at +0x1E4);
			// without adding fpOffset, d was ~20,000 units so gameF evaluated to 0 at night and EnvLightFade skipped the weapon completely.
			if (firstPerson) {
				const NiPoint3& o = *(const NiPoint3*)((const char*)node + 0x1E4);
				if (_finite(o.x) && _finite(o.y) && _finite(o.z)) fpOffset = o;
			}
		}
		const NiBound* bound = geometry->GetWorldBound();
		const float dx = (light->m_worldTransform.pos.x + fpOffset.x) - bound->Center.x;
		const float dy = (light->m_worldTransform.pos.y + fpOffset.y) - bound->Center.y;
		const float dz = (light->m_worldTransform.pos.z + fpOffset.z) - bound->Center.z;
		const float lampLum = Luminance(light->Diff) * light->Dimmer;
		const float gameF = (std::max)(sunLum, lampLum * (1.0f - (std::min)((std::max)(sqrtf(dx * dx + dy * dy + dz * dz) / radius, 0.0f), 1.0f)));

		const bool flash = FlashlightLamp::Is(light);
		const D3DXVECTOR4& cam = TheRenderManager->CameraPosition;
		// the flashlight's lamp: its full brightness (its dimmer here is already its cone at the object, FlashlightLamp::ForPass)
		const float pixelLum = flash ? Luminance(light->Diff) * FlashlightLamp::Dimmer : lampLum;
		const float c[4][4] = {
			{ -1.0f, gameF, sunLum, pixelLum },	// (x -1: on; FlashlightLamp puts its slot + 1, positive, in c28.x for the object shaders)
			{ light->m_worldTransform.pos.x - cam.x, light->m_worldTransform.pos.y - cam.y, light->m_worldTransform.pos.z - cam.z, radius },
			{ FlashlightLamp::Axis.x, FlashlightLamp::Axis.y, FlashlightLamp::Axis.z, FlashlightLamp::CosOuter },
			{ FlashlightLamp::CosInner, flash ? 1.0f : 0.0f, 0.0f, 0.0f } };

		// UNOFFICIAL 2026-11-20: gather up to 2 additional strongest active point lights on this property (c37-c39 in EnvTemplate.hlsl)
		// so walking between lights at night smoothly crossblends instead of snapping when FNV swaps lamp 0.
		float extra[3][4] = {};
		float bestScore[2] = { 0.0f, 0.0f };
		const UInt8* propBytes = (const UInt8*)property;
		for (const UInt32* node = *(const UInt32* const*)(propBytes + 0x60); node; node = (const UInt32*)node[0]) {
			const UInt8* sl = (const UInt8*)node[2];
			if (!sl || sl == (const UInt8*)lamp || *(const UInt16*)(sl + 0x110) == 0xFF) continue;
			const UInt8* niL = *(const UInt8* const*)(sl + 0xF8);
			if (!niL || (niL[0x30] & 1) || sl[0xEC]) continue;
			const ShadowSceneLight* ssl = (const ShadowSceneLight*)sl;
			if (!ssl->sourceLight || !ssl->bPointLight) continue;
			const NiPointLight* l = ssl->sourceLight;
			if (FlashlightLamp::Is(l)) continue;
			const float r = l->Spec.r;
			if (!(r > 1.0f)) continue;
			const float ldx = (l->m_worldTransform.pos.x + fpOffset.x) - bound->Center.x;
			const float ldy = (l->m_worldTransform.pos.y + fpOffset.y) - bound->Center.y;
			const float ldz = (l->m_worldTransform.pos.z + fpOffset.z) - bound->Center.z;
			const float dist = sqrtf(ldx * ldx + ldy * ldy + ldz * ldz);
			if (dist >= r) continue;
			const float lum = Luminance(l->Diff) * l->Dimmer;
			const float att = 1.0f - (dist / r);
			const float score = lum * att * att;
			if (score <= bestScore[1]) continue;
			const int slot = score > bestScore[0] ? 0 : 1;
			if (slot == 0) {
				bestScore[1] = bestScore[0];
				memcpy(extra[1], extra[0], sizeof(extra[0]));
				extra[2][1] = extra[2][0];
			}
			bestScore[slot] = score;
			extra[slot][0] = l->m_worldTransform.pos.x - cam.x;
			extra[slot][1] = l->m_worldTransform.pos.y - cam.y;
			extra[slot][2] = l->m_worldTransform.pos.z - cam.z;
			extra[slot][3] = r;
			extra[2][slot] = lum;
		}

		TheRenderManager->device->SetPixelShaderConstantF(28, &c[0][0], 4);
		TheRenderManager->device->SetPixelShaderConstantF(37, &extra[0][0], 3);
		Set = true;
		StatPasses++;
		if (StatPasses <= 3 || StatPasses % 50000 == 0)
			Logger::Log("UNOFFICIAL EnvLightFade: env pass %llu shaped per pixel (0x%X '%s': the game's fade %.3f, sun %.3f, lamp %.3f%s)", StatPasses, (unsigned)type,
				geometry->m_pcName ? geometry->m_pcName : "?", gameF, sunLum, pixelLum, flash ? ", the flashlight's" : "");
	}
	inline void AfterPass() {
		if (!Set) return;
		static const float none[4] = {};
		TheRenderManager->device->SetPixelShaderConstantF(28, none, 1);
		TheRenderManager->device->SetPixelShaderConstantF(39, none, 1);
		Set = false;
	}
}
