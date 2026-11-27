#pragma once
// UNOFFICIAL 2026-11-04 (user: "lets fade the way lights interact with envmap_light_fade at night"). The env-map shine of assets with the
// BSS2_Envmap_Light_Fade flag follows the light round them: ShadowLightShader::SetupGeometryConstants_AmbientEmittance multiplies the env
// pass's c1.w by F = max(the sun's luminance, the first active non-shadow lamp's luminance x (1 - d / r)), d from the lamp to the OBJECT's
// bound centre. At night (no sun) a lamp makes a whole object gleam at one strength, which jumps as the lamp or the player moves.
// Per env pass (0x244-0x24B, before the game sets it up; Render.cpp ForwardShadowsForPass) the same F is worked out here with the same lamp
// (the game's GetFirstActiveNonShadowLight) and sent with the lamp to EnvTemplate.hlsl (c28-c31), which shades per pixel instead:
// P = max(sun, lamp x (1 - (d/r)^2)^2 at the pixel, times the beam's cone for the flashlight's game lamp), the shine scaled by P / F.
// After the draw c28 goes back to 0 (AfterPass): no other env draw takes it.
// UNOFFICIAL 2026-11-25 fd-complete batch 5 ([Main.Main.Quality] DeferredEnvFade, live, default true): while the deferred renderer is live
// and this frame's lamp table is made (ManyLamps::ActiveLamps: every lamp the deferred passes light, built at the frame's first world draw,
// before the main list), a WORLD env pass takes its lamps from that list, not from the game's per-object light list: the three strongest
// at the object's bound (luminance x (1 - d/(r + bound radius))^2, as the extra lamps were ranked), and the shine is set to the material's
// alpha x P directly (mode -2), so the game's own fade F (from its light list, in c1.w) is not used. The flashlight's game lamp (left out
// of the table) keeps slot 0 when the game made it the object's first lamp. Off, first person, reflections, or no table this frame: as before.
#include <cmath>
#include <algorithm>
#include <vector>
#include <d3dx9.h>
#include "FlashlightLamp.h"
#include "ManyLamps.h"
#include "DeferredRenderer.h"

namespace EnvLightFade {
	inline bool Set = false;
	inline unsigned long long StatPasses = 0, StatTablePasses = 0;

	inline float Luminance(const NiColor& c) { return c.g * 0.36f + c.r * 0.31f + c.b * 0.32f; }	// NiColor::Luminance 0xB719E0

	// fd-complete batch 5: the frame's table lamps (the deferred passes' own) in place of the game's light list. false: not this pass.
	inline bool FromTable(NiGeometry* geometry, BSShaderProperty* property, ShadowSceneLight* gameFirst) {
		if (!DeferredRenderer::Live || ManyLamps::TableFailed || ManyLamps::TableFrame != ManyLamps::FrameId) return false;
		static UInt32 keyFrame = 0xFFFFFFFFu;
		static bool keyOn = true;
		if (keyFrame != ManyLamps::FrameId) { keyFrame = ManyLamps::FrameId; keyOn = TheSettingManager->GetSettingI("Main.Main.Quality", "DeferredEnvFade") != 0; }
		if (!keyOn) return false;
		const NiBound* bound = geometry->GetWorldBound();
		if (!bound) return false;
		const D3DXVECTOR4& cam = TheRenderManager->CameraPosition;
		// The table's lamps with their gamma-space luminance, made once a frame (the table's colours are linear when linear lighting is compiled
		// in: back to the game's values, as the sun's below; three powf a lamp, so never per pass: Vault 21 has ~540 lamps and ~90 env passes)
		struct Lamp { float x, y, z, reach, lum; };
		static std::vector<Lamp> lamps;
		static UInt32 lampsFrame = 0xFFFFFFFFu;
		static size_t lampsCount = ~(size_t)0;	// (the table may be made twice in a frame: its first draw, then the world's; ActiveLamps is refilled)
		if (lampsFrame != ManyLamps::FrameId || lampsCount != ManyLamps::ActiveLamps.size()) {
			lampsFrame = ManyLamps::FrameId;
			lampsCount = ManyLamps::ActiveLamps.size();
			lamps.clear();
			const bool linear = ManyLamps::LinearCompiled && LinearPreDecode::Gamma > 1.0f;
			const float inv = linear ? 1.0f / LinearPreDecode::Gamma : 1.0f;
			for (const ManyLamps::ActiveLamp& l : ManyLamps::ActiveLamps) {
				if (!(l.reach > 1.0f)) continue;
				const float* c = l.colour;
				const float lum = linear ? powf((std::max)(c[1], 0.0f), inv) * 0.36f + powf((std::max)(c[0], 0.0f), inv) * 0.31f + powf((std::max)(c[2], 0.0f), inv) * 0.32f
					: c[1] * 0.36f + c[0] * 0.31f + c[2] * 0.32f;
				if (lum > 0.0f) lamps.push_back({ l.pos[0], l.pos[1], l.pos[2], l.reach, lum });
			}
		}
		int best[3] = { -1, -1, -1 };
		float score[3] = {}, lum[3] = {};
		const int n = (int)lamps.size();
		for (int k = 0; k < n; k++) {
			const Lamp& l = lamps[k];
			const float dx = l.x - bound->Center.x, dy = l.y - bound->Center.y, dz = l.z - bound->Center.z;
			const float reach = l.reach + bound->Radius;
			const float d2 = dx * dx + dy * dy + dz * dz;
			if (d2 >= reach * reach) continue;
			const float att = 1.0f - sqrtf(d2) / reach;
			const float sc = l.lum * att * att;
			if (sc <= score[2]) continue;
			const int slot = sc > score[0] ? 0 : (sc > score[1] ? 1 : 2);
			for (int m = 2; m > slot; m--) { best[m] = best[m - 1]; score[m] = score[m - 1]; lum[m] = lum[m - 1]; }
			best[slot] = k; score[slot] = sc; lum[slot] = l.lum;
		}
		float sunLum = 0.0f;
		if (ShadowSceneNode* node = BSShaderManager::GetShadowSceneNode(0))
			if (node->directionalLight && node->directionalLight->sourceLight) sunLum = Luminance(node->directionalLight->sourceLight->Diff);
		const bool flash = gameFirst && gameFirst->sourceLight && FlashlightLamp::Is(gameFirst->sourceLight);
		float c[4][4] = {};
		float extra[2][4] = {};
		auto lampAt = [&](int k, float* out) {
			const Lamp& l = lamps[k];
			out[0] = l.x - cam.x; out[1] = l.y - cam.y; out[2] = l.z - cam.z; out[3] = l.reach;
		};
		int next = 0;	// the next table lamp to place
		if (flash) {	// slot 0: the flashlight's game lamp, as before
			const NiPointLight* light = gameFirst->sourceLight;
			c[0][3] = Luminance(light->Diff) * FlashlightLamp::Dimmer;
			c[1][0] = light->m_worldTransform.pos.x - cam.x; c[1][1] = light->m_worldTransform.pos.y - cam.y; c[1][2] = light->m_worldTransform.pos.z - cam.z;
			c[1][3] = (std::max)(light->Spec.r, 1.0f);
			c[2][0] = FlashlightLamp::Axis.x; c[2][1] = FlashlightLamp::Axis.y; c[2][2] = FlashlightLamp::Axis.z; c[2][3] = FlashlightLamp::CosOuter;
			c[3][0] = FlashlightLamp::CosInner; c[3][1] = 1.0f;
		}
		else if (best[0] >= 0) { lampAt(best[0], c[1]); c[0][3] = lum[0]; next = 1; }
		else c[1][3] = 1.0f;	// no lamp: the sun alone (lampFade 0)
		for (int e = 0; e < 2 && next < 3; e++, next++) {
			if (best[next] < 0) break;
			lampAt(best[next], extra[e]);
			c[3][2 + e] = lum[next];
		}
		c[0][0] = -2.0f;	// replace: the shine = material alpha x P (EnvTemplate.hlsl), the game's fade F not used
		c[0][1] = (std::max)(property->fAlpha * property->fFadeAlpha, 2.0e-4f);
		c[0][2] = sunLum;
		TheRenderManager->device->SetPixelShaderConstantF(28, &c[0][0], 4);
		TheRenderManager->device->SetPixelShaderConstantF(37, &extra[0][0], 2);
		Set = true;
		StatPasses++;
		StatTablePasses++;
		if (StatTablePasses <= 3 || StatTablePasses % 50000 == 0)
			Logger::Log("UNOFFICIAL EnvLightFade (DeferredEnvFade): env pass %llu from the lamp table ('%s': sun %.3f, lamps %.3f %.3f %.3f of %d%s)", StatTablePasses,
				geometry->m_pcName ? geometry->m_pcName : "?", sunLum, flash ? c[0][3] : lum[0], c[3][2], c[3][3], n, flash ? ", slot 0 the flashlight's" : "");
		return true;
	}

	inline void ForPass(const void* pass, bool firstPerson = false, bool world = false) {
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
		if (world && !firstPerson && FromTable(geometry, property, lamp)) return;	// (2026-11-25 fd-complete batch 5)
		if (!lamp || !lamp->sourceLight) return;	// the sun alone: one value is right
		const NiPointLight* light = lamp->sourceLight;
		const float radius = light->Spec.r;
		if (!(radius > 1.0f)) return;

		float sunLum = 0.0f;
		NiPoint3 fpOffset = { 0.0f, 0.0f, 0.0f };
		if (ShadowSceneNode* node = BSShaderManager::GetShadowSceneNode(0)) {
			if (node->directionalLight && node->directionalLight->sourceLight) sunLum = Luminance(node->directionalLight->sourceLight->Diff);
			// In 1st person, ShadowSceneNode +0x1E4 holds kLightingOffset for the extra-light ranking in 1st-person bound space.
			if (firstPerson) {
				const NiPoint3& o = *(const NiPoint3*)((const char*)node + 0x1E4);
				if (_finite(o.x) && _finite(o.y) && _finite(o.z)) fpOffset = o;
			}
		}
		// Vanilla SetupGeometryConstants_AmbientEmittance computes F_game from raw light->pos - bound->Center (no kLightingOffset),
		// so gameF must match that exact distance or dividing by gameF in EnvTemplate.hlsl crushes 1st-person shine on moonlit nights.
		const NiBound* bound = geometry->GetWorldBound();
		const float dx = light->m_worldTransform.pos.x - bound->Center.x;
		const float dy = light->m_worldTransform.pos.y - bound->Center.y;
		const float dz = light->m_worldTransform.pos.z - bound->Center.z;
		const float lampLum = Luminance(light->Diff) * light->Dimmer;
		const float gameF = (std::max)(sunLum, lampLum * (1.0f - (std::min)((std::max)(sqrtf(dx * dx + dy * dy + dz * dz) / radius, 0.0f), 1.0f)));

		const bool flash = FlashlightLamp::Is(light);
		const D3DXVECTOR4& cam = TheRenderManager->CameraPosition;
		// the flashlight's lamp: its full brightness (its dimmer here is already its cone at the object, FlashlightLamp::ForPass)
		const float pixelLum = flash ? Luminance(light->Diff) * FlashlightLamp::Dimmer : lampLum;
		// When F_game is ~0 in 1st person at night, vanilla zeroes c1.w; -2.0f mode supplies material alpha so the shader replaces F_game directly.
		const bool directReplace = firstPerson && gameF <= 1.0e-4f;
		const float modeX = directReplace ? -2.0f : -1.0f;
		const float factorOrAlpha = directReplace ? (property->fAlpha * property->fFadeAlpha) : gameF;
		float c[4][4] = {
			{ modeX, factorOrAlpha, sunLum, pixelLum },	// (x < -0.5: on; -2: replace zeroed c1.w; FlashlightLamp puts slot+1 > 0 in c28.x)
			{ light->m_worldTransform.pos.x - cam.x, light->m_worldTransform.pos.y - cam.y, light->m_worldTransform.pos.z - cam.z, radius },
			{ FlashlightLamp::Axis.x, FlashlightLamp::Axis.y, FlashlightLamp::Axis.z, FlashlightLamp::CosOuter },
			{ FlashlightLamp::CosInner, flash ? 1.0f : 0.0f, 0.0f, 0.0f } };

		// UNOFFICIAL 2026-11-20: gather up to 2 additional strongest active point lights on this property (c37-c38 + c31.zw in EnvTemplate.hlsl)
		// so walking between lights at night smoothly crossblends instead of snapping when FNV swaps lamp 0 (FNV zeroes c39 every draw).
		float extra[2][4] = {}, extraLum[2] = {};
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
				extraLum[1] = extraLum[0];
			}
			bestScore[slot] = score;
			extra[slot][0] = l->m_worldTransform.pos.x - cam.x;
			extra[slot][1] = l->m_worldTransform.pos.y - cam.y;
			extra[slot][2] = l->m_worldTransform.pos.z - cam.z;
			extra[slot][3] = r;
			extraLum[slot] = lum;
		}

		c[3][2] = extraLum[0];
		c[3][3] = extraLum[1];
		TheRenderManager->device->SetPixelShaderConstantF(28, &c[0][0], 4);
		TheRenderManager->device->SetPixelShaderConstantF(37, &extra[0][0], 2);
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
		Set = false;
	}
}
