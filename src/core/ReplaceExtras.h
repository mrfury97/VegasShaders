#pragma once

// UNOFFICIAL 2026-11-14 (rt-shadows, no-limits replacements; F:\Claude Brain\RTShadowAudit\AUDIT.md). The DXVK fork runs a SPIR-V
// replacement in place of a game pixel shader (DxvkCompute::RegisterReplacement, fork patch 0003). Replacements built by
// tools/nvr-dxvk/replace/nolimits.py from shaders that had no sampler left in ps_3_0 (6-7 layer land; Shaders/Includes/Shadow.hlsl
// SHADOW_SUN_CONTACT_EXTRA) read the forward RT sun from NVR's OWN resources instead of a D3D9 register: their manifest says
// "extras=ubo,image", and they get
//   set 4 binding 0: a uniform buffer = cbuffer NvrExtra { row_major float4x4 TESR_ContactReproject; float4 TESR_ContactForwardData;
//                    float4 TESR_ContactForwardSize; } (BendContact's values, this frame's)
//   set 4 binding 1: TESR_ContactForward (core/BendContact.h's texture; Load, no sampler)
// both bound once a frame (Bind, from ShadowsExteriorEffect::UpdateConstants after BendContact::UpdateConstants) into the extension's
// replacement slots kSlot, which no NVR pass uses (passes bind 0 ..~16 for their own draws).

#include "DxvkCompute.h"
#include "BendContact.h"

namespace ReplaceExtras {
	inline constexpr UINT kSlot = 63;
	inline ID3D9NvrExtBuffer* Buffer = nullptr;
	inline bool Failed = false;
	inline UINT Binds = 0;

	struct Block { float reproject[16]; float data[4]; float size[4]; float pad[8]; };
	static_assert(sizeof(Block) == 128, "NvrExtra block");

	inline void Bind() {
		if (!DxvkCompute::ReplacementExtrasUsed() || Failed || !BendContact::Texture || !DxvkCompute::Available()) return;
		if (!Buffer) {
			Buffer = DxvkCompute::CreateBuffer(sizeof(Block), NVR_EXT_BUFFER_UNIFORM | NVR_EXT_BUFFER_TRANSFER_DST, "NVR replacement extras");
			if (!Buffer) { Failed = true; Logger::Log("UNOFFICIAL Vegas Shaders DXVK: replacement extras: the uniform buffer could not be made: the no-limits replacements read no RT sun"); return; }
		}
		Block b = {};
		memcpy(b.reproject, &BendContact::Reproject, sizeof(b.reproject));
		b.data[0] = BendContact::Data.x; b.data[1] = BendContact::Data.y; b.data[2] = BendContact::Data.z; b.data[3] = BendContact::Data.w;
		D3DSURFACE_DESC desc = {};
		if (FAILED(BendContact::Texture->GetLevelDesc(0, &desc))) return;
		b.size[0] = (float)desc.Width; b.size[1] = (float)desc.Height; b.size[2] = 1.0f / (float)desc.Width; b.size[3] = 1.0f / (float)desc.Height;
		ID3D9NvrExt* ext = DxvkCompute::Extension();
		if (FAILED(ext->UpdateBuffer(Buffer, 0, sizeof(b), &b))) return;
		NVR_EXT_RESOURCE_BINDING bindings[2] = {};
		bindings[0].Slot = kSlot; bindings[0].Type = NVR_EXT_BIND_UNIFORM_BUFFER; bindings[0].pResource = Buffer;
		bindings[1].Slot = kSlot; bindings[1].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[1].pResource = BendContact::Texture;
		const HRESULT hr = ext->SetReplacementResources(2, bindings);
		if (Binds++ % 7200 == 0)
			Logger::Log("UNOFFICIAL Vegas Shaders DXVK: replacement extras bound (%u): RT sun forward data x %.0f, %ux%u: %s", Binds, b.data[0], desc.Width, desc.Height,
				SUCCEEDED(hr) ? "ok" : "SetReplacementResources FAILED");
	}
}
