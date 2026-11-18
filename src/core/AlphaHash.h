#pragma once
// UNOFFICIAL 2026-11-03 SmoothCutouts ([Shaders.TAA.Cutouts]; shaders: Includes/AlphaHash.hlsl).
//
// Alpha-tested cut-outs (leaves, grass, fences, chain-link, hair cards) were made for the game's transparency multisampling, alpha to
// coverage, which needs the game's MSAA. TAA, FSR and DLSS run with MSAA off, so every texel is drawn fully or not at all: jagged cut
// edges, and the see-through fringe of a texture made for alpha to coverage drawn solid (Discord 2026-11-03: "all alpha cutout assets
// look chopped"; a tester's hair port drew a dark outline round the head). The object, parallax and grass shaders hash the alpha test
// instead (keep the texel with a probability, against noise that changes per pixel and frame); the jittered frames average it.
//
// Per object pass (Render.cpp RenderPassStandardHook / RenderPassSkinnedHook): b15 is 1 for an object of the world scene whose alpha
// property tests (GREATER / GREATEREQUAL) without blending, while hashing (a jittered scene) or FarCoverage is live; the shaders read it
// (NVR_AlphaHashDraw). The hash itself also needs TESR_AlphaHash.x > 0, which is 0 while the scene is not jittered. Back to 0 after the
// pass, so no other draw (blended glass and hair, the sorted blended list, NVR's own redraws, anything outside the world scene) is touched.
// Every pass of one object takes the same decision for a pixel (same alpha, same noise), so its lamp passes land on what it drew.
// Skin (SkinTemplate) is left out: its light-only passes test alpha in the fixed-function stage only and could not agree.
// Per frame (TESR_AlphaHash): x the mode (0 off or not jittered), y the noise offset, z the edge width, w FarCoverage (below).
// Off (Enabled false at startup): ALPHA_HASH_ON is not defined, the shaders are exactly as before and nothing here runs.
//
// UNOFFICIAL 2026-11-16 FarCoverage ([Shaders.TAA.Cutouts] FarCoverage, restart; FarCoverageScale, live; docs/next/FAR-FOLIAGE-AA.md):
// far cut-outs keep their coverage, with or without TAA: the alpha the cut sees is raised with the mip the hardware reads
// (Includes/AlphaHash.hlsl AlphaCutCoverage). The shader needs the base texture's size: b11 / b12 carry a size class per pass (256 to 2048
// texels, the geometric mean of width and height, from the property's NiDX9TextureData). w = scale + 2 * q, q the world's mip bias in
// quarter mips below 0. Off (FarCoverage false at startup): ALPHA_COVERAGE_ON is not defined, and the shaders are as before.
#include <d3d9.h>
#include <cmath>
#include <algorithm>

namespace AlphaHash {
	inline const char* kSection = "Shaders.TAA.Cutouts";
	inline int Startup = -1;				// Enabled at startup (the shaders are compiled with it)
	inline int CoverageStartup = -1;		// FarCoverage at startup (likewise)
	inline bool Live = false;				// this world scene marks alpha-tested passes (hashing or FarCoverage on)
	inline bool HashLive = false;			// ... and hashes them (jittered, Mode > 0)
	inline bool CoverLive = false;			// ... and raises their far alpha (FarCoverageScale > 0)
	inline bool Sent = false;				// b15 as last sent
	inline int SentSize = -1;				// b11 / b12 as last sent (size class 0-3; -1 unknown)
	inline UInt32 Frame = 0;				// world scenes begun (the noise moves once per frame's first one)
	inline UInt32 Hashed = 0, HashedShown = 0;	// passes marked this frame / last frame (the log line)
	inline D3DXVECTOR4 Constant = { 0.0f, 0.0f, 1.0f, 0.0f };	// TESR_AlphaHash

	inline bool CompiledIn() {
		if (Startup < 0) Startup = TheSettingManager->GetSettingI(kSection, "Enabled") ? 1 : 0;
		return Startup == 1;
	}

	inline bool CoverageCompiledIn() {
		if (CoverageStartup < 0) CoverageStartup = TheSettingManager->GetSettingI(kSection, "FarCoverage") ? 1 : 0;
		return CoverageStartup == 1;
	}

	inline void Send(IDirect3DDevice9* device, bool on) {
		if (on == Sent || !device) return;
		const BOOL value = on ? TRUE : FALSE;
		device->SetPixelShaderConstantB(15, &value, 1);
		Sent = on;
	}

	inline void SendSize(IDirect3DDevice9* device, int size) {
		if (size == SentSize || !device) return;
		const BOOL values[2] = { (size & 1) ? TRUE : FALSE, (size & 2) ? TRUE : FALSE };
		device->SetPixelShaderConstantB(11, values, 2);
		SentSize = size;
	}

	// The world scene starts (RenderWorldSceneGraphHook, after TAA's BeginJitter). jittered: TAA's jitter is on for it. mipBias: the world
	// textures' LOD bias this scene (TAAEffect::WorldMipBias: MipBias and RenderScale's), 0 or below. upscaler: an upscaler (FSR / DLSS) resolves
	// this scene, not TAA's own resolve (TAAEffect::WorldDrawnSmall; picks HashStyle's auto value, below).
	inline void BeginWorld(IDirect3DDevice9* device, bool firstWorld, bool jittered, float mipBias, bool upscaler = false) {
		Sent = true; Send(device, false);	// whatever the device holds: 0 until a pass says otherwise
		SentSize = -1;
		const bool hashIn = CompiledIn(), coverIn = CoverageCompiledIn();
		if (!hashIn && !coverIn) { Live = HashLive = CoverLive = false; return; }
		const int mode = hashIn ? TheSettingManager->GetSettingI(kSection, "Mode") : 0;
		const float scale = coverIn ? (std::clamp)(TheSettingManager->GetSettingF(kSection, "FarCoverageScale"), 0.0f, 0.5f) : 0.0f;
		HashLive = hashIn && jittered && mode > 0;
		CoverLive = scale > 0.0f;
		Live = HashLive || CoverLive;
		if (firstWorld) {
			Frame++;
			HashedShown = Hashed;
			Hashed = 0;
			// once a minute or so, and at most every 600 frames when the count moves by a quarter (a new place: the bench checks it there)
			static UInt32 lastFrame = 0, lastCount = 0;
			const UInt32 slack = lastCount / 4 + 8;
			const bool moved = HashedShown > lastCount + slack || HashedShown + slack < lastCount;
			if (Live && (lastFrame == 0 || Frame - lastFrame >= 3600 || (moved && Frame - lastFrame >= 600))) {
				lastFrame = Frame;
				lastCount = HashedShown;
				Logger::Log("SmoothCutouts: mode %d (hashing %s), FarCoverage %.2f, %u alpha-tested passes marked last frame",
					mode, HashLive ? "on" : "off: not jittered or mode 0", scale, HashedShown);
			}
		}
		// Interleaved gradient noise moves 5.588238 pixels a frame (Jimenez 2014): 64 frames before it repeats.
		Constant.x = HashLive ? (float)(std::min)(mode, 2) : 0.0f;
		// 2026-11-16 HashStyle (Includes/AlphaHash.hlsl): 0 the slide over 64 frames; 1 / 3 a still pattern whose threshold steps
		// through 8 stratified values (bit-reversed eighths: the jitter cycle's length); 2 still. z carries the style above the edge width.
		// (2026-11-16 round 3 decision) -1 = auto: 3 under TAA's own resolve (native D3D9, or the fork without FSR / DLSS: its exponential history
		// never settles a per-frame hash, the still-camera fizz), 0 under an upscaler (FSR 3.1 / DLSS accumulate it steadily, and 0 is the softest).
		const int asked = hashIn ? TheSettingManager->GetSettingI(kSection, "HashStyle") : 0;
		const int style = asked < 0 ? (upscaler ? 0 : 3) : (std::min)(asked, 6);
		static int loggedStyle = -2;
		if (HashLive && style != loggedStyle) {
			loggedStyle = style;
			Logger::Log("SmoothCutouts: HashStyle %d (%s)", style, asked >= 0 ? "set" : upscaler ? "auto: FSR / DLSS resolves" : "auto: TAA's own resolve");
		}
		static const float kEighths[8] = { 0.0f, 4.0f, 2.0f, 6.0f, 1.0f, 5.0f, 3.0f, 7.0f };
		Constant.y = style == 0 ? 5.588238f * (float)(Frame % 64) : (style == 2 || style == 4) ? 0.0f : (kEighths[Frame % 8] + 0.5f) / 8.0f;
		Constant.z = (hashIn ? (std::clamp)(TheSettingManager->GetSettingF(kSection, "EdgeWidth"), 0.25f, 4.0f) : 1.0f) + 16.0f * (float)style;
		const float q = std::floor((std::clamp)(-mipBias, 0.0f, 3.0f) * 4.0f + 0.5f);	// quarter mips, 0-12
		Constant.w = CoverLive ? scale + 2.0f * q : 0.0f;
	}

	inline void EndWorld(IDirect3DDevice9* device) {
		Live = HashLive = CoverLive = false;
		Send(device, false);
	}

	// The size class of a geometry's base texture (0: 256 or less, 1: 512, 2: 1024, 3: 2048 or more; 1 when unknown).
	inline int SizeClassOf(NiGeometry* geometry) {
		BSShaderProperty* shade = (BSShaderProperty*)geometry->GetProperty(NiProperty::PropertyType::kType_Shade);
		if (!shade || !shade->IsLightingProperty()) return 1;
		NiSourceTexture** slots = ((BSShaderPPLightingProperty*)shade)->ppTextures[0];
		const NiTexture* base = slots ? *slots : nullptr;
		const NiDX9TextureData* data = base ? base->rendererData : nullptr;
		if (!data || !data->width || !data->height) return 1;
		const float log2Size = 0.5f * (std::log2((float)data->width) + std::log2((float)data->height));
		return (std::clamp)((int)std::floor(log2Size + 0.5f) - 8, 0, 3);
	}

	// Before each object pass. world: the pass belongs to the world scene (ShaderSplit::World).
	inline void ForPass(NiGeometry* geometry, bool world, IDirect3DDevice9* device) {
		bool mark = Live && world && geometry;
		if (mark) {
			const NiAlphaProperty* alpha = geometry->propertyState.m_spAlphaProperty;
			const UInt16 func = alpha ? (UInt16)((alpha->flags >> 10) & 7) : 0;	// NiAlphaProperty::TestFunction: 4 GREATER, 6 GREATEREQUAL
			mark = alpha && (alpha->flags & NiAlphaProperty::TEST_ENABLE_MASK) && !(alpha->flags & NiAlphaProperty::ALPHA_BLEND_MASK) &&
				(func == 4 || func == 6);
		}
		if (mark) {
			Hashed++;
			if (CoverLive) SendSize(device, SizeClassOf(geometry));
		}
		Send(device, mark);
	}

	inline void AfterPass(IDirect3DDevice9* device) { Send(device, false); }
}
