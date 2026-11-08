// UNOFFICIAL 2026-10-27 (NVR DXVK, [Shaders.TAA.FSR] RenderScale): real upscaling. The world scene is drawn at a lower internal
// resolution (e.g. 67 % of the screen's width and height = 45 % of the pixels) and AMD FSR 2.2.1 (core\Upscaler.cpp) upscales it
// to the screen right after, before anything else reads it. The first-person weapon, the HUD and menus, the game's image space and
// every NVR effect then run at full resolution on a full-resolution picture, exactly as without it.
//
// How (docs\next\FORK2-UPSCALE.md has the long version):
//  1. BeginJitter (RenderWorldSceneGraphHook, before the game draws the world): the world camera's viewport port (NiCamera::ViewPort,
//     normalised; NiDX9Renderer::Do_SetCameraData turns it into the D3D9 viewport of every SetCameraData of the scene) becomes the top-
//     left RenderWidth x RenderHeight of the full-size targets. Nothing is resized: the game's targets, NVR's buffers and the depth
//     buffer keep the screen's size, the world just fills their top-left corner. The projection is unchanged (same frustum), so
//     every clip-space lookup (shadow maps, water reflections, the light grid) reads what it read before. The jitter is FSR's own
//     sequence in render pixels, the texture mip bias adds log2 of the scale.
//  2. UpscaleWorld (right after the world scene, before the game's refraction pass and the first-person weapon):
//     - the world's depth buffer resolved into TESR_DepthBufferWorld (INTZ), the low-resolution picture copied out of the target;
//     - FSRUpPrep (NVR): camera motion vectors and FSR's depth from that depth buffer (no weapon in it yet);
//     - FSR 2 (HDR build when the world's target is a float target: the scene before tonemapping);
//     - a D3D9 pass writes FSR's output into the world's colour target (the alpha channel, which some NVR shaders use as a material
//       mark, from the nearest low-resolution pixel) and the depth buffer, nearest-upsampled from the low-resolution one, so the
//       hook's own depth resolve that follows gives every NVR effect a full-resolution depth (CombineDepth and all its readers).
//  3. TAAEffect::Render skips its resolve for that frame (FSR has anti-aliased it).
// What reads the world's target inside its own render: the water's refraction copy (made by the game mid-scene, BSShaderAccumulator
// FinishAccumulating_Standard_PostResolveDepth): NVR's water shaders scale that one lookup by TESR_RenderScale. NVR's own copies made
// inside the scene (terrain blending's depth, LampSums' run copy) are read at the same pixels they were drawn: nothing to change.
// Viewport checks of world draws (lamp scissor, forward lamp shadows, the B42 scope fix) accept the render viewport (IsWorldViewport).
//
// Not upscaled (a frame at native resolution instead, counted, each reason logged once): TAA or FSR off, no NVR DXVK, the game's MSAA
// on (FSR replaces it: turn it off), a scope's scissor, the Pip-Boy, menus/overlays, an unusual camera port. Skin scattering
// ([Shaders.Skin.NewSkin] ScreenSpace) is skipped in upscaled frames (its targets were drawn at the low resolution).

#include <algorithm>
#include <cmath>

#include "TAA.h"
#include "../../core/DxvkCompute.h"
#include "../../core/GpuProfiler.h"
#include "../../core/Upscaler.h"
#include "../../core/RenderRect.h"	// UNOFFICIAL 2026-10-27 RenderScale v2
#include "../../core/Diagnostics.h"	// UNOFFICIAL 2026-11-01 the history watch (HistoryProbe)
#include "../../core/ObjectMotion.h"	// UNOFFICIAL 2026-10-29 ObjectMotion
#include "../../core/ReactiveMask.h"	// UNOFFICIAL 2026-10-30 ReactiveMask
#include "../../core/RrRectPlan.h"	// UNOFFICIAL 2026-11-07 (perf-rr) the render size's maths, shared with RR's plan

D3DXVECTOR4 TAAEffect::RenderScaleConstant = D3DXVECTOR4(1.0f, 1.0f, 1.0f, 1.0f);

namespace {
	// UNOFFICIAL 2026-10-30 (tester report: with DLSS on, the render-scaled picture stayed small in the top-left corner): a RenderScale v2
	// frame whose upscale never ran (no image space after it; UpscaleBegin drops it) is shown as drawn: small, in its corner. Three in a
	// row and v2 is off for the session: the upscale runs right after the world scene (v1, UpscaleWorld), which every frame reaches.
	UInt32 DroppedInARow = 0;
	bool V2Off = false;

	enum UpscaleSkipReason {
		SkipSecondWorld, SkipTaaOff, SkipGame, SkipFailed, SkipExtension, SkipResz, SkipPipBoy, SkipCamera, SkipPort, SkipScissor,
		SkipTarget, SkipMsaa, SkipSize, SkipReasonCount
	};
	const char* const SkipNames[SkipReasonCount] = {
		"a second world scene in the same frame", "TAA is off ([Shaders.TAA.Status] Enabled)", "menu, overlay or loading (no player cell)",
		"RenderScale failed earlier this session", "no Vegas Shaders DXVK fork, or FSR failed", "no RESZ depth resolve (the Vegas Shaders DXVK fork with the RESZ patch, or dxvk.conf d3d9.customVendorId = 1002)",
		"the Pip-Boy is open", "no world camera, or an orthographic one", "the world camera's viewport is not the whole screen",
		"a scissor is set (a gun scope's view)", "no world render target", "the game's MSAA is on (turn it off: FSR anti-aliases)",
		"the world's targets are not the screen's size"
	};

	// Constants of FSRUpPrep.comp (std140), the same block as FSRPrep.comp's.
	struct UpPrepConstants {
		D3DXMATRIX	projection, view, prevView;
		D3DXVECTOR4	cameraData, prevProjection, cameraDelta, size;
		D3DXVECTOR4	extra;	// UNOFFICIAL 2026-10-30 x: 1 = ReactiveMask::Texture at binding 5, y: ReactiveMaxDistance
	};
#define IMG(b)      { 1, b, NVR_EXT_BIND_SAMPLED_IMAGE, b, NVR_EXT_VIEW_2D, FALSE, 0 }
#define UAV(b)      { 1, b, NVR_EXT_BIND_STORAGE_IMAGE, b, NVR_EXT_VIEW_2D, TRUE, 0 }
#define CB(s, b)    { s, b, NVR_EXT_BIND_UNIFORM_BUFFER, 0, NVR_EXT_VIEW_2D, FALSE, 0 }
#define POINTS      { 0, 0, NVR_EXT_BIND_SAMPLER, 0, NVR_EXT_VIEW_2D, FALSE, 0 }
	const NVR_EXT_BINDING_DESC UpPrepBindings[] = { IMG(0), IMG(1), UAV(2), UAV(3), CB(2, 0), POINTS, IMG(4), IMG(5) };	// IMG(1): v2's viewmodel depth; IMG(5): UNOFFICIAL 2026-10-30 ReactiveMask
#undef IMG
#undef UAV
#undef CB
#undef POINTS

	NVR_EXT_RESOURCE_BINDING Image(UINT slot, IUnknown* resource, bool storage = false) {
		NVR_EXT_RESOURCE_BINDING b = {};
		b.Slot = slot; b.Type = storage ? NVR_EXT_BIND_STORAGE_IMAGE : NVR_EXT_BIND_SAMPLED_IMAGE; b.pResource = resource;
		return b;
	}

	// The D3D9 composite (ps_3_0 with pre-transformed vertices, as PassSums' fold-in): FSR's output into the world's colour target and
	// the low-resolution depth, upsampled, into its depth buffer. c0 xy: the render size as a fraction of the textures' size
	// (where the low-resolution picture ends in uv), z: [Shaders.TAA.FSR] DebugView, w: 1 = FSR's output in s0, 0 = the fallback
	// (the low-resolution picture stretched bilinearly, for a frame FSR could not do). c1 xy: the screen size. c2 xy: this frame's
	// jitter as FSR takes it (render pixels; the low-resolution sample at pixel p shows the unjittered point p - jitter), zw: near, far.
	// UNOFFICIAL 2026-10-27 (user: "fsr seems a bit bugged with ambient light enabled - leaves lines on terrain and darkens it a bit"):
	// the depth was the nearest low-resolution sample, jittered: on slopes a staircase of render-pixel steps that moved every frame, and
	// AmbientLight (and the normals made from depth) shaded every step as a small crease. Now the depth is read at the unjittered point
	// (as FSR's picture is) and blended between the four nearest samples where they lie on one surface (within 2 % in distance, or on one
	// plane within 10 %: device depth is linear across a plane on screen); at an object's edge the nearest sample, as before. The alpha
	// (a material mark some NVR shaders read) is the nearest unjittered sample.
	const char* const CompositeSource =
		"sampler2D FsrS : register(s0);\n"		// FSR's output (or the low-resolution picture, fallback)
		"sampler2D LowS : register(s1);\n"		// the low-resolution picture, POINT
		"sampler2D DepthS : register(s2);\n"		// the world's depth (INTZ), POINT
		"sampler2D MotionS : register(s3);\n"	// FSR's motion input, POINT
		"sampler2D LowLinearS : register(s4);\n"	// the low-resolution picture, LINEAR
		"float4 Scale : register(c0);\n"
		"float4 Screen : register(c1);\n"
		"float4 Jitter : register(c2);\n"
		"struct PSOut { float4 color : COLOR0; float depth : DEPTH; };\n"
		"float ViewZ(float d) { return Jitter.z * Jitter.w / (Jitter.w - d * (Jitter.w - Jitter.z)); }\n"
		"PSOut main(float2 uv : TEXCOORD0) {\n"
		"  PSOut o;\n"
		"  float2 lowUv = uv * Scale.xy;\n"		// the low-resolution picture at the mapped position (the stretched fallback, debug views)
		"  float2 inv = 1.0 / Screen.xy;\n"
		"  float2 last = Scale.xy * Screen.xy - 1.0;\n"	// the last low-resolution pixel
		"  float2 t = uv * Scale.xy * Screen.xy + Jitter.xy - 0.5;\n"	// in low-resolution pixel centres, where this pixel's point was sampled
		"  float2 i0 = floor(t);\n"
		"  float2 f = t - i0;\n"
		"  float2 a = clamp(i0, 0.0, last), b = clamp(i0 + 1.0, 0.0, last);\n"
		"  float d00 = tex2D(DepthS, (a + 0.5) * inv).x, d10 = tex2D(DepthS, (float2(b.x, a.y) + 0.5) * inv).x;\n"
		"  float d01 = tex2D(DepthS, (float2(a.x, b.y) + 0.5) * inv).x, d11 = tex2D(DepthS, (b + 0.5) * inv).x;\n"
		"  float z00 = ViewZ(d00), z10 = ViewZ(d10), z01 = ViewZ(d01), z11 = ViewZ(d11);\n"
		"  float zMin = min(min(z00, z10), min(z01, z11)), spread = (max(max(z00, z10), max(z01, z11)) - zMin) / max(zMin, 1e-3);\n"
		"  float bend = abs(d00 - d10 - d01 + d11), slope = max(max(abs(d10 - d00), abs(d01 - d00)), 1e-7);\n"
		"  bool oneSurface = spread < 0.02 || (spread < 0.1 && bend <= 0.25 * slope);\n"
		"  float2 n = float2(f.x < 0.5 ? a.x : b.x, f.y < 0.5 ? a.y : b.y);\n"	// the nearest sample
		"  float4 low = tex2D(LowS, (n + 0.5) * inv);\n"
		"  o.depth = oneSurface ? lerp(lerp(d00, d10, f.x), lerp(d01, d11, f.x), f.y) : tex2D(DepthS, (n + 0.5) * inv).x;\n"
		"  float3 rgb = Scale.w > 0.5 ? tex2D(FsrS, uv).rgb : tex2D(LowLinearS, lowUv).rgb;\n"
		"  if (Scale.z > 0.5 && Scale.z < 1.5) rgb = (uv.x < Scale.x && uv.y < Scale.y) ? tex2D(LowS, uv).rgb : float3(0.0, 0.0, 0.0);\n"
		"  else if (Scale.z > 1.5 && Scale.z < 2.5) rgb = low.rgb;\n"
		"  else if (Scale.z > 2.5) { float2 m = tex2D(MotionS, lowUv).xy * Screen.xy / 16.0; rgb = float3(abs(m), 0.0); }\n"
		"  o.color = float4(rgb, low.a);\n"
		"  return o;\n"
		"}\n";

	const D3DRENDERSTATETYPE CompositeRenderStates[][1] = {
		{ D3DRS_ZENABLE }, { D3DRS_ZWRITEENABLE }, { D3DRS_ZFUNC }, { D3DRS_STENCILENABLE }, { D3DRS_TWOSIDEDSTENCILMODE },
		{ D3DRS_ALPHABLENDENABLE }, { D3DRS_SEPARATEALPHABLENDENABLE }, { D3DRS_ALPHATESTENABLE }, { D3DRS_CULLMODE },
		{ D3DRS_COLORWRITEENABLE }, { D3DRS_SCISSORTESTENABLE }, { D3DRS_SRGBWRITEENABLE }, { D3DRS_FOGENABLE }, { D3DRS_FILLMODE },
		{ D3DRS_CLIPPLANEENABLE }, { D3DRS_DEPTHBIAS }, { D3DRS_SLOPESCALEDEPTHBIAS }, { D3DRS_MULTISAMPLEANTIALIAS },
	};
	const DWORD CompositeRenderValues[] = {
		D3DZB_TRUE, TRUE, D3DCMP_ALWAYS, FALSE, FALSE,
		FALSE, FALSE, FALSE, D3DCULL_NONE,
		0xF, FALSE, FALSE, FALSE, D3DFILL_SOLID,
		0, 0, 0, FALSE,
	};
	static_assert(_countof(CompositeRenderStates) == _countof(CompositeRenderValues), "composite render states");
	const DWORD CompositeSamplers = 5;

	void SetCompositeStates(IDirect3DDevice9* device, IDirect3DPixelShader9* shader, IDirect3DBaseTexture9* const* textures, const float* constants) {
		device->SetVertexShader(NULL);
		device->SetPixelShader(shader);
		device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
		for (UINT i = 0; i < _countof(CompositeRenderValues); i++) device->SetRenderState(CompositeRenderStates[i][0], CompositeRenderValues[i]);
		for (DWORD s = 0; s < CompositeSamplers; s++) {
			const DWORD filter = (s == 0 || s == 4) ? D3DTEXF_LINEAR : D3DTEXF_POINT;	// s0: FSR's output is read at its own pixels (same result)
			device->SetTexture(s, textures ? textures[s] : nullptr);
			device->SetSamplerState(s, D3DSAMP_MINFILTER, filter);
			device->SetSamplerState(s, D3DSAMP_MAGFILTER, filter);
			device->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
			device->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
			device->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
			device->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
			device->SetSamplerState(s, D3DSAMP_MIPMAPLODBIAS, 0);
			device->SetSamplerState(s, D3DSAMP_MAXMIPLEVEL, 0);
		}
		const float zero[12] = {};
		device->SetPixelShaderConstantF(0, constants ? constants : zero, 3);
	}

	struct CompositeVertex { float x, y, z, rhw, u, v; };
}

bool TAAEffect::IsWorldViewport(const D3DVIEWPORT9& port) {
	if (port.Width == TheRenderManager->width && port.Height == TheRenderManager->height) return true;
	TAAEffect* taa = TheShaderManager ? TheShaderManager->Effects.TAA : nullptr;
	return taa && (taa->Up.Active || taa->Up.Deferred) && port.Width == taa->Up.RenderWidth && port.Height == taa->Up.RenderHeight;
}

bool TAAEffect::UpscaledBeforeEffects() {
	TAAEffect* taa = TheShaderManager ? TheShaderManager->Effects.TAA : nullptr;
	return taa && taa->Up.Upscaled;
}

bool TAAEffect::UpscaledThisFrame() {
	TAAEffect* taa = TheShaderManager ? TheShaderManager->Effects.TAA : nullptr;
	return taa && (taa->Up.Upscaled || taa->Up.Deferred);	// UNOFFICIAL 2026-10-27 v2: also while the frame waits for its upscale
}

void TAAEffect::NoteWorldDraw(bool firstPerson) {
	TAAEffect* taa = TheShaderManager ? TheShaderManager->Effects.TAA : nullptr;
	// UNOFFICIAL 2026-10-27 RenderScale v2: first-person passes while the frame waits for its upscale (the weapon into the same rect).
	if (!taa || !(firstPerson ? taa->Up.Deferred : taa->Up.Active)) return;
	D3DVIEWPORT9 port = {};
	if (FAILED(TheRenderManager->device->GetViewport(&port))) return;
	UpscaleStruct& up = taa->Up;
	if (port.X == 0 && port.Y == 0 && port.Width == up.RenderWidth && port.Height == up.RenderHeight) up.DrawsSmall++;
	else if (port.X == 0 && port.Y == 0 && port.Width == up.Width && port.Height == up.Height) {
		// Something reset the viewport to the whole target mid-scene (a SetRenderTarget without the game's SetCameraData after it):
		// this world pass is put back into the render viewport, where the rest of the scene is.
		up.DrawsFull++;
		D3DVIEWPORT9 render = port;
		render.Width = up.RenderWidth;
		render.Height = up.RenderHeight;
		TheRenderManager->device->SetViewport(&render);
	}
	else up.DrawsOther++;
}

void TAAEffect::UpscaleReleaseTargets() {
	if (Up.Target) { Up.Target->Release(); Up.Target = nullptr; }
	if (Up.DepthStencil) { Up.DepthStencil->Release(); Up.DepthStencil = nullptr; }
}

void TAAEffect::UpscaleRestorePort() {
	if (!Up.PortSet) return;
	if (Up.Camera) Up.Camera->ViewPort = Up.SavedPort;
	Up.PortSet = false;
	RenderScaleConstant = D3DXVECTOR4(1.0f, 1.0f, 1.0f, 1.0f);
}

void TAAEffect::UpscaleSkip(int reason) {
	Up.HistoryValid = false;
	Up.SkippedFrames++;
	if (reason >= 0 && reason < SkipReasonCount && !(Up.LoggedSkips & (1u << reason))) {
		Up.LoggedSkips |= 1u << reason;
		Logger::Log("UNOFFICIAL RenderScale %.2f: a frame stays at native resolution: %s (logged once per reason).", FSRSettings.RenderScale, SkipNames[reason]);
	}
	Up.LastSkip = reason;
}

bool TAAEffect::UpscaleBegin(bool firstWorld) {
	// UNOFFICIAL 2026-10-27 RenderScale v2: a second world scene while the frame waits for its upscale is drawn as always (constant
	// 1 for it; UpscaleBeginEffects sets the scale again). A frame whose upscale never ran (no image space after it) is dropped here.
	if (Up.Deferred) {
		if (!firstWorld) { Up.SkippedFrames++; RenderScaleConstant = D3DXVECTOR4(1.0f, 1.0f, 1.0f, 1.0f); return false; }
		Up.Deferred = false;
		RenderRect::End();
		RenderRect::RescaleRegistered(TheRenderManager->device);
		Up.HistoryValid = false;
		UpscaleReleaseTargets();
		Logger::Log("UNOFFICIAL RenderScale v2: a frame drawn small was never upscaled (no image space after it); dropped.");
		if (++DroppedInARow >= 3 && !V2Off) {
			V2Off = true;
			Logger::Log("UNOFFICIAL RenderScale v2: %u frames in a row were never upscaled (each was shown small, in the top-left corner); the effects run after the upscale "
				"for this session: the world is upscaled right after it is drawn, before the weapon and Vegas Shaders' effects.", DroppedInARow);
		}
	}
	Up.Active = false;
	UpscaleReleaseTargets();
	RenderScaleConstant = D3DXVECTOR4(1.0f, 1.0f, 1.0f, 1.0f);
	Up.DrawsSmall = Up.DrawsFull = Up.DrawsOther = 0;
	// A second world scene in the same frame (rare: menus' 3D backgrounds) is drawn as it always was; the frame's upscale (and
	// TAA's skip of its resolve) belongs to the first one, so Upscaled is kept.
	if (!firstWorld) { if (FSRSettings.Enabled && FSRSettings.RenderScale < 0.999f) Up.SkippedFrames++; return false; }
	Up.Upscaled = false;
	if (!FSRSettings.Enabled || FSRSettings.RenderScale >= 0.999f) { Up.HistoryValid = false; return false; }
	auto skip = [this](int reason) { UpscaleSkip(reason); return false; };
	if (!Enabled || !Effect || !ShouldRender()) return skip(SkipTaaOff);
	if (!TheSettingManager->SettingsMain.Main.RenderEffects || !Player || !Player->parentCell || TheShaderManager->GameState.OverlayIsOn) return skip(SkipGame);
	if (Up.Failed) return skip(SkipFailed);
	if (!FSRWanted()) return skip(SkipExtension);
	if (!TheRenderManager->RESZ) return skip(SkipResz);
	if (InterfaceManager->IsPipBoyOpen()) return skip(SkipPipBoy);
	NiCamera* camera = WorldSceneGraph ? WorldSceneGraph->camera : nullptr;
	if (!camera || camera->Frustum.Ortho) return skip(SkipCamera);
	const NiViewport& port = camera->ViewPort;
	if (fabsf(port.l) > 1e-4f || fabsf(port.r - 1.0f) > 1e-4f || fabsf(port.t - 1.0f) > 1e-4f || fabsf(port.b) > 1e-4f) return skip(SkipPort);
	IDirect3DDevice9* device = TheRenderManager->device;
	DWORD scissor = FALSE;
	if (SUCCEEDED(device->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissor)) && scissor) return skip(SkipScissor);

	IDirect3DSurface9* target = nullptr;
	IDirect3DSurface9* depth = nullptr;
	if (FAILED(device->GetRenderTarget(0, &target)) || !target) return skip(SkipTarget);
	if (FAILED(device->GetDepthStencilSurface(&depth)) || !depth) { target->Release(); return skip(SkipTarget); }
	D3DSURFACE_DESC targetDesc = {}, depthDesc = {};
	target->GetDesc(&targetDesc);
	depth->GetDesc(&depthDesc);
	const UINT width = TheRenderManager->width, height = TheRenderManager->height;
	int problem = -1;
	if (targetDesc.MultiSampleType != D3DMULTISAMPLE_NONE || depthDesc.MultiSampleType != D3DMULTISAMPLE_NONE) problem = SkipMsaa;
	else if (targetDesc.Width != width || targetDesc.Height != height || depthDesc.Width < width || depthDesc.Height < height) problem = SkipSize;
	if (problem >= 0) { target->Release(); depth->Release(); return skip(problem); }

	const float scale = FSRSettings.RenderScale;
	// UNOFFICIAL 2026-10-27 (user: "the half screen ambient light darkening specifically on balanced fsr not the other settings"): the
	// internal size is a multiple of 8. At 2560 x 1440, Balanced (0.588) gave 1505 x 847, the only preset odd both ways: the effects'
	// half / quarter / eighth buffers then drew a part half a texel off the frame's (753 for 752.5), an offset growing across the
	// picture, and AmbientLight's depth-aware upsampling tipped over to the wrong texels past a line (the split). A multiple of 8 keeps
	// every one of them whole (at most 0.3 % off the chosen scale). The native size (scale 1) stays exact.
	// (2026-11-07 perf-rr: the same maths, now core/RrRectPlan.h's, so tests/rr_rect_plan.cpp checks the sizes RR is given)
	const UINT renderWidth = RrRect::SnapRenderSize(width, scale);
	const UINT renderHeight = RrRect::SnapRenderSize(height, scale);

	Up.Width = width;
	Up.Height = height;
	Up.RenderWidth = renderWidth;
	Up.RenderHeight = renderHeight;
	// ffxFsr2GetJitterPhaseCount: 8 x (display / render)^2 steps.
	Up.JitterPhases = RrRect::JitterPhases(width, renderWidth);
	Up.Target = target;
	Up.DepthStencil = depth;
	Up.TargetFormat = targetDesc.Format;
	Up.Hdr = targetDesc.Format == D3DFMT_A16B16G16R16F || targetDesc.Format == D3DFMT_A32B32G32R32F;
	// Do_SetCameraData: X = W * l, Y = H * (1 - t), width = W * (r - l), height = H * (t - b), each rounded.
	// UNOFFICIAL 2026-10-27 (user: "a strange blue line that grows and shrinks depending on render scale ... appears and disappears on
	// some render scale levels"; their log: "0 world passes drawn at that size" at those scales): W x (render / W) in float came out a
	// hair under the render size at some scales and the game's conversion dropped a whole pixel, so the world was drawn one column or
	// row short of the render rectangle; its last column / row kept stale pixels (sky blue) that FSR stretched into a line at the
	// screen's edge. A quarter pixel more lands on the render size whether the game rounds or cuts off.
	Up.SavedPort = port;
	camera->ViewPort.l = 0.0f;
	camera->ViewPort.r = ((float)renderWidth + 0.25f) / (float)width;
	camera->ViewPort.t = 1.0f;
	camera->ViewPort.b = 1.0f - ((float)renderHeight + 0.25f) / (float)height;
	Up.Camera = camera;
	Up.PortSet = true;
	Up.Active = true;
	// UNOFFICIAL 2026-10-27 RenderScale v2: zw = 1 / scale (Includes/RenderRect.hlsl; the water shaders read only xy).
	const float sx = (float)renderWidth / (float)width, sy = (float)renderHeight / (float)height;
	RenderScaleConstant = D3DXVECTOR4(sx, sy, 1.0f / sx, 1.0f / sy);
	return true;
}

bool TAAEffect::UpscaleEnsureResources() {
	const UINT width = Up.Width, height = Up.Height;
	IDirect3DDevice9* device = TheRenderManager->device;
	D3DSURFACE_DESC desc = {};
	if (Up.Input.Surface) Up.Input.Surface->GetDesc(&desc);
	if (!Up.Input.Surface || desc.Width != width || desc.Height != height || Up.InputFormat != Up.TargetFormat) {
		FSRTexture* textures[] = { &Up.Input, &Up.Motion, &Up.Depth };
		for (FSRTexture* t : textures) {
			if (t->Surface) { t->Surface->Release(); t->Surface = nullptr; }
			if (t->Texture) { t->Texture->Release(); t->Texture = nullptr; }
		}
		TheTextureManager->InitTexture("TESR_UpscaleInput", &Up.Input.Texture, &Up.Input.Surface, width, height, Up.TargetFormat);
		TheTextureManager->InitTexture("TESR_UpscaleMotion", &Up.Motion.Texture, &Up.Motion.Surface, width, height, D3DFMT_G16R16F);
		TheTextureManager->InitTexture("TESR_UpscaleDepth", &Up.Depth.Texture, &Up.Depth.Surface, width, height, D3DFMT_G32R32F);
		if (!Up.Input.Surface || !Up.Motion.Surface || !Up.Depth.Surface) {
			Logger::Log("[ERROR] UNOFFICIAL RenderScale: could not create its inputs (%ux%u, format %u)", width, height, (UInt32)Up.TargetFormat);
			return false;
		}
		Up.InputFormat = Up.TargetFormat;
		Up.HistoryValid = false;
		Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RenderScale inputs made (%ux%u, about %u MB)", width, height, (UInt32)((UInt64)width * height * 20 / (1024 * 1024)));
	}
	// The D3D9 composite first: it is also the fallback that makes a failed frame whole (UpscaleWorld).
	if (!Up.CompositeTried) {
		Up.CompositeTried = true;
		ID3DXBuffer* code = nullptr;
		ID3DXBuffer* errors = nullptr;
		if (FAILED(D3DXCompileShader(CompositeSource, (UINT)strlen(CompositeSource), NULL, NULL, "main", "ps_3_0", 0, &code, &errors, NULL)) || !code ||
			FAILED(device->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &Up.CompositePS))) {
			Logger::Log("[ERROR] UNOFFICIAL RenderScale: the composite shader failed: %s", errors ? (const char*)errors->GetBufferPointer() : "?");
			Up.CompositePS = nullptr;
		}
		if (errors) errors->Release();
		if (code) code->Release();
		// Every state the composite sets, recorded once: captured before each composite and applied after it, so the game's render
		// state cache never sees a change.
		if (Up.CompositePS && SUCCEEDED(device->BeginStateBlock())) {
			SetCompositeStates(device, Up.CompositePS, nullptr, nullptr);
			if (FAILED(device->EndStateBlock(&Up.CompositeStates))) Up.CompositeStates = nullptr;
		}
		if (Up.CompositePS && !Up.CompositeStates) Logger::Log("[ERROR] UNOFFICIAL RenderScale: the composite's state block could not be made");
	}
	if (!Up.PrepTried) {
		Up.PrepTried = true;
		// Checked first: a missing file would otherwise turn every Vulkan pass off for the session (DxvkCompute::CreatePipeline).
		if (DxvkCompute::HasSpirv("FSRUpPrep")) Up.Prep = DxvkCompute::CreatePipeline("FSRUpPrep", UpPrepBindings, _countof(UpPrepBindings), 4, true);
		if (!Up.Prep) Logger::Log("[ERROR] UNOFFICIAL RenderScale: Compute\\FSRUpPrep.spv missing or refused");
	}
	return Up.Prep && Up.CompositePS && Up.CompositeStates;
}

// D3D9: FSR's output (or, fsrOutput null, the low-resolution picture stretched: the fallback) into the world's colour target and the
// low-resolution depth, nearest-upsampled, into its depth buffer. Everything it changes is put back except the viewport, which is left
// at the whole screen (what the game's own next SetCameraData sets anyway).
bool TAAEffect::UpscaleComposite(IDirect3DDevice9* Device, IDirect3DTexture9* fsrOutput, IDirect3DTexture9* depthSource, bool colour, bool jittered) {
	if (!Up.CompositePS || !Up.CompositeStates || !Up.Target || !Up.DepthStencil || FAILED(Up.CompositeStates->Capture())) return false;
	IDirect3DSurface9* oldTargets[4] = {};
	for (DWORD i = 0; i < 4; i++) if (FAILED(Device->GetRenderTarget(i, &oldTargets[i]))) oldTargets[i] = nullptr;
	IDirect3DSurface9* oldDepth = nullptr;
	if (FAILED(Device->GetDepthStencilSurface(&oldDepth))) oldDepth = nullptr;
	IDirect3DVertexBuffer9* oldStream = nullptr;
	UINT oldOffset = 0, oldStride = 0;
	if (FAILED(Device->GetStreamSource(0, &oldStream, &oldOffset, &oldStride))) oldStream = nullptr;

	Device->SetRenderTarget(0, Up.Target);	// also sets the viewport to the whole target
	for (DWORD i = 1; i < 4; i++) if (oldTargets[i]) Device->SetRenderTarget(i, nullptr);
	Device->SetDepthStencilSurface(Up.DepthStencil);
	const D3DVIEWPORT9 full = { 0, 0, Up.Width, Up.Height, 0.0f, 1.0f };
	Device->SetViewport(&full);

	IDirect3DBaseTexture9* textures[CompositeSamplers] = { fsrOutput ? (IDirect3DBaseTexture9*)fsrOutput : (IDirect3DBaseTexture9*)Up.Input.Texture,
		Up.Input.Texture, depthSource ? depthSource : TheTextureManager->DepthTexture, Up.Motion.Texture, Up.Input.Texture };
	const bool shift = jittered && Up.Jittered;
	const float constants[12] = { (float)Up.RenderWidth / (float)Up.Width, (float)Up.RenderHeight / (float)Up.Height, (float)FSRSettings.DebugView,
		fsrOutput ? 1.0f : 0.0f, (float)Up.Width, (float)Up.Height, 0.0f, 0.0f,
		shift ? -Up.JitterX : 0.0f, shift ? Up.JitterY : 0.0f, TheRenderManager->CameraData.x, TheRenderManager->CameraData.y };
	SetCompositeStates(Device, Up.CompositePS, textures, constants);
	if (!colour) Device->SetRenderState(D3DRS_COLORWRITEENABLE, 0);	// UNOFFICIAL 2026-10-27 v2: depth only (the state block puts it back)
	const float w = (float)Up.Width - 0.5f, h = (float)Up.Height - 0.5f;
	const CompositeVertex quad[4] = { { -0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f }, { w, -0.5f, 0.0f, 1.0f, 1.0f, 0.0f },
		{ -0.5f, h, 0.0f, 1.0f, 0.0f, 1.0f }, { w, h, 0.0f, 1.0f, 1.0f, 1.0f } };
	const HRESULT drawn = Device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(CompositeVertex));

	Up.CompositeStates->Apply();
	Device->SetStreamSource(0, oldStream, oldOffset, oldStride);
	Device->SetRenderTarget(0, oldTargets[0] ? oldTargets[0] : Up.Target);
	for (DWORD i = 1; i < 4; i++) if (oldTargets[i]) Device->SetRenderTarget(i, oldTargets[i]);
	Device->SetDepthStencilSurface(oldDepth);
	Device->SetViewport(&full);
	for (IDirect3DSurface9* s : oldTargets) if (s) s->Release();
	if (oldDepth) oldDepth->Release();
	if (oldStream) oldStream->Release();
	return SUCCEEDED(drawn);
}

bool TAAEffect::UpscaleCheckDrawn(IDirect3DDevice9* Device) {
	auto fail = [this](const char* why) {
		Logger::Log("[ERROR] UNOFFICIAL Vegas Shaders DXVK: RenderScale: %s; the world is drawn at native resolution for the rest of this session.", why);
		Up.Failed = true;
		return false;
	};

	// Which viewport the world's passes were drawn with (NoteWorldDraw counts them in RenderPassStandard/SkinnedHook, and puts a pass
	// that found the whole-screen viewport back into the render one). None at the render size but some at the whole screen: nothing
	// was drawn small (off for the session). Some of each: something reset the viewport mid-scene (logged once; those passes were
	// put back). No pass counted at all: the viewport the scene ended with decides.
	const UInt32 drawsSmall = Up.DrawsSmall, drawsFull = Up.DrawsFull, drawsOther = Up.DrawsOther;
	D3DVIEWPORT9 drawn = {};
	Device->GetViewport(&drawn);
	const bool endedFull = drawn.X == 0 && drawn.Y == 0 && drawn.Width == Up.Width && drawn.Height == Up.Height;
	if ((drawsSmall == 0 && drawsFull > 0) || (drawsSmall == 0 && drawsFull == 0 && drawsOther == 0 && endedFull)) {
		fail("the world was drawn at full resolution anyway (the game did not use the camera's viewport port)");
		Up.Whole = true;	// nothing to put back: the picture is whole
		return false;
	}
	if ((drawsFull > 0 || drawsOther > 0) && !Up.MixedLogged) {
		Up.MixedLogged = true;
		Logger::Log("UNOFFICIAL RenderScale: of this world scene's passes %u used the render viewport (0,0 %ux%u), %u the whole screen and %u another "
			"(logged once; whole-screen ones found the viewport reset mid-scene and were put back into the render one). The scene ended with %u,%u %ux%u.",
			drawsSmall, Up.RenderWidth, Up.RenderHeight, drawsFull, drawsOther, (unsigned)drawn.X, (unsigned)drawn.Y, (unsigned)drawn.Width, (unsigned)drawn.Height);
	}
	return true;
}

bool TAAEffect::UpscaleRun(IDirect3DDevice9* Device, bool resolveDepth) {
	auto fail = [this](const char* why) {
		Logger::Log("[ERROR] UNOFFICIAL Vegas Shaders DXVK: RenderScale: %s; the world is drawn at native resolution for the rest of this session.", why);
		Up.Failed = true;
		return false;
	};

	if (!UpscaleEnsureResources()) {
		fail("its textures or passes could not be made");
		return false;	// the caller stretches the low-resolution picture (if the composite exists) or leaves it
	}

	// 1. This frame's world depth, and the camera of this frame without jitter (EndJitter put the frustum back).
	// (v2: already resolved by RenderWorldSceneGraphHook; the depth-stencil holds the weapon's depth by now.)
	if (resolveDepth) TheRenderManager->ResolveDepthBuffer(TheTextureManager->DepthTexture);
	TheRenderManager->UpdateSceneCameraData();
	TheRenderManager->SetupSceneCamera();
	const UINT renderWidth = Up.RenderWidth, renderHeight = Up.RenderHeight;

	// The history goes on only from the last world scene if it was upscaled too, at the same size, in the same cell, recently.
	LARGE_INTEGER now, frequency;
	QueryPerformanceCounter(&now);
	QueryPerformanceFrequency(&frequency);
	const float deltaTime = Up.LastTime.QuadPart ? (float)(now.QuadPart - Up.LastTime.QuadPart) / (float)frequency.QuadPart : 0.0f;
	Up.LastTime = now;
	const bool reset = !Up.HistoryValid || TheShaderManager->GameState.isCameraCut || renderWidth != Up.LastRenderWidth ||
		renderHeight != Up.LastRenderHeight || deltaTime > 0.5f;
	if (reset) SessionLog::HistoryReset("RenderScale upscaler", !Up.HistoryValid ? "no history" : TheShaderManager->GameState.isCameraCut ? "camera cut" :
		deltaTime > 0.5f ? "frame gap over 0.5 s" : "render size");	// UNOFFICIAL 2026-11-01
	if (reset) {
		Up.PrevView = TheRenderManager->viewMatrix;
		Up.PrevProjection = D3DXVECTOR4(TheRenderManager->projMatrix._11, TheRenderManager->projMatrix._22, 0.0f, 0.0f);
		Up.PrevCameraPosition = TheRenderManager->CameraPosition;
	}

	// 2. The low-resolution picture out of the world's target (FSR reads it while the target is drawn into again).
	const RECT region = { 0, 0, (LONG)renderWidth, (LONG)renderHeight };
	{
		static GpuTimer copyTimer("  RenderScale copy");
		GpuProfileScope gpu(copyTimer, Device);
		if (FAILED(Device->StretchRect(Up.Target, &region, Up.Input.Surface, &region, D3DTEXF_NONE))) {
			fail("the low-resolution picture could not be copied");
			return false;
		}
	}

	// 3. FSR's inputs: camera motion vectors and FSR's depth from the world's depth buffer.
	UpPrepConstants prep = {};
	prep.projection = TheRenderManager->projMatrix;
	prep.view = TheRenderManager->viewMatrix;
	prep.prevView = Up.PrevView;
	prep.cameraData = TheRenderManager->CameraData;
	prep.prevProjection = Up.PrevProjection;
	const D3DXVECTOR4 delta = TheRenderManager->CameraPosition - Up.PrevCameraPosition;
	// UNOFFICIAL 2026-10-29 ObjectMotion: w = FSRUpPrep.comp's switch for t_objmotion (only once this frame's is made: an upscale before
	// the effects runs before it, and keeps camera motion only)
	const bool objectMotion = ObjectMotion::Ready();
	prep.cameraDelta = D3DXVECTOR4(delta.x, delta.y, delta.z, objectMotion ? 1.0f : 0.0f);
	// w: v2 (the weapon is drawn by now) = its reactive value and FSRPrep.comp's weapon rule; v1 = -1, no weapon test.
	prep.size = D3DXVECTOR4((float)renderWidth, (float)renderHeight, reset ? 1.0f : 0.0f, resolveDepth ? -1.0f : FSRSettings.WeaponReactive);
	const bool reactive = ReactiveMask::Ready();	// UNOFFICIAL 2026-10-30
	prep.extra = D3DXVECTOR4(reactive ? 1.0f : 0.0f, ReactiveMask::MaxDistance(), 0.0f, 0.0f);
	ID3D9NvrExtBuffer* prepBuffer = ExtUniforms(Up.PrepUniforms, &prep, sizeof(prep));
	ID3D9NvrExtSampler* point = DxvkCompute::PointClamp();
	if (!prepBuffer || !point) { fail("its constant buffer or sampler could not be made"); return false; }
	{
		NVR_EXT_RESOURCE_BINDING b[] = { Image(0, TheTextureManager->DepthTexture), Image(1, TheTextureManager->DepthTextureViewModel),
			Image(2, Up.Motion.Texture, true), Image(3, Up.Depth.Texture, true), {}, {},
			Image(4, objectMotion ? (IUnknown*)ObjectMotion::Texture : (IUnknown*)TheTextureManager->DepthTexture),
			Image(5, reactive ? (IUnknown*)ReactiveMask::Texture : (IUnknown*)TheTextureManager->DepthTexture) };
		b[4].Slot = 0; b[4].Type = NVR_EXT_BIND_UNIFORM_BUFFER; b[4].pResource = prepBuffer;
		b[5].Slot = 0; b[5].Type = NVR_EXT_BIND_SAMPLER; b[5].pResource = point;
		NVR_EXT_DISPATCH_DESC dispatch = {};
		UInt32 push = 0;
		dispatch.pPipeline = Up.Prep;
		dispatch.BindingCount = _countof(b);
		dispatch.pBindings = b;
		dispatch.pPushData = &push;
		dispatch.PushDataSize = 4;
		dispatch.GroupsX = (renderWidth + 7) / 8;
		dispatch.GroupsY = (renderHeight + 7) / 8;
		dispatch.GroupsZ = 1;
		static GpuTimer prepTimer("  FSR prep (RenderScale)");
		GpuProfileScope gpu(prepTimer, Device);
		DxvkCompute::PreLoad(b, _countof(b));	// UNOFFICIAL 2026-10-27 (fork2-combo, the alflicker rule): managed textures on the GPU
		if (FAILED(DxvkCompute::Extension()->Dispatch(&dispatch))) { fail("FSRUpPrep failed to dispatch"); return false; }
	}

	// 4. The upscaler.
	Upscaler::Inputs in;
	in.Color = Up.Input.Texture;
	in.Depth = Up.Depth.Texture;
	in.Motion = Up.Motion.Texture;
	// UNOFFICIAL 2026-11-01 v2: the weapon is in this picture (FSRUpPrep's weapon rule above): DLSS's bias current colour mask on it
	// ([Shaders.TAA.DLSS] WeaponMask). v1: none (the weapon is drawn after the upscale, at full resolution).
	in.ViewModel = resolveDepth ? nullptr : TheTextureManager->DepthTextureViewModel;
	in.RenderWidth = renderWidth;
	in.RenderHeight = renderHeight;
	in.InputWidth = in.DisplayWidth = Up.Width;
	in.InputHeight = in.DisplayHeight = Up.Height;
	// As at native resolution: BeginJitter moved the frustum right/up by (x, y) render pixels, the picture left/down: FSR's (-x, +y).
	in.JitterX = Up.Jittered ? -Up.JitterX : 0.0f;
	in.JitterY = Up.Jittered ? Up.JitterY : 0.0f;
	in.JitterPhases = Up.JitterPhases;
	in.NearZ = TheRenderManager->CameraData.x;
	in.FarZ = TheRenderManager->CameraData.y;
	in.TanHalfFovX = 1.0f / TheRenderManager->projMatrix._11;
	in.TanHalfFovY = 1.0f / TheRenderManager->projMatrix._22;
	in.DeltaSeconds = deltaTime;
	in.Sharpness = FSRSettings.Sharpness;
	in.Hdr = Up.Hdr;
	in.Reset = reset || lastPathFSR;	// after FSR at native resolution the shared history is that path's
	Upscaler::IUpscaler* upscaler = Upscaler::Choose(FSRSettings.Upscaler);
	bool evaluated = upscaler->Evaluate(Device, in);
	if (!evaluated && upscaler == Upscaler::Fsr31() && upscaler->Failed()) {
		// UNOFFICIAL 2026-10-28: FSR 3.1 failing (logged by it) hands this frame and the rest of the session to FSR 2.
		upscaler = Upscaler::Choose(FSRSettings.Upscaler);
		in.Reset = true;
		evaluated = upscaler->Evaluate(Device, in);
	}
	if (!evaluated) { fail("the upscaler failed"); return false; }

	// 5. Into the world's targets.
	{
		static GpuTimer compositeTimer("  RenderScale composite + depth");
		GpuProfileScope gpu(compositeTimer, Device);
		if (!UpscaleComposite(Device, upscaler->Output())) { fail("the composite pass failed"); return false; }
	}

	// UNOFFICIAL 2026-11-01 (Diagnostics.h part 5): the state for the log lines, and the history watch's probe of the small picture
	// (the top-left renderWidth x renderHeight of Up.Input) against the upscaled one
	Diag.path = resolveDepth ? "RenderScale upscale of the world (v1: before the weapon and the effects)" : "RenderScale upscale after the pre-tonemap effects (v2)";
	Diag.upscaler = std::string(upscaler->Name()) + ": " + upscaler->DiagText();
	Diag.reset = in.Reset;
	Diag.sinceReset = in.Reset ? 0 : Diag.sinceReset + 1;
	Diag.jitter = Up.JitterIndex;
	Diag.jitterX = in.JitterX;
	Diag.jitterY = in.JitterY;
	Diag.deltaMs = deltaTime * 1000.0f;
	Diag.inW = renderWidth; Diag.inH = renderHeight; Diag.outW = Up.Width; Diag.outH = Up.Height;
	Diag.hdr = in.Hdr;
	Diag.output = upscaler->Output();
	Diag.frame = TheFrameRateManager ? TheFrameRateManager->FrameNumber : 0;
	if (Diag.output) {
		IDirect3DSurface9* probeOut = nullptr;
		if (SUCCEEDED(Diag.output->GetSurfaceLevel(0, &probeOut)) && probeOut) {
			Diagnostics::HistoryProbe("RenderScale upscaler", Up.Input.Surface, renderWidth, renderHeight, probeOut, &TAAEffect::DiagTextStatic);
			probeOut->Release();
		}
	}

	Up.PrevView = TheRenderManager->viewMatrix;
	Up.PrevProjection = D3DXVECTOR4(TheRenderManager->projMatrix._11, TheRenderManager->projMatrix._22, 0.0f, 0.0f);
	Up.PrevCameraPosition = TheRenderManager->CameraPosition;
	Up.LastRenderWidth = renderWidth;
	Up.LastRenderHeight = renderHeight;
	Up.HistoryValid = true;
	Up.JitterIndex = (Up.JitterIndex + 1) % (std::max)(1, (int)Up.JitterPhases);
	Up.Upscaled = true;
	return true;
}

void TAAEffect::UpscaleWorld() {
	if (!Up.Active) return;
	IDirect3DDevice9* device = TheRenderManager->device;
	bool ok = false;
	Up.Whole = false;
	{
		static GpuTimer timer("FSR upscale (RenderScale)");
		GpuProfileScope gpu(timer, device);
		ok = UpscaleCheckDrawn(device);
		// UNOFFICIAL 2026-10-27 RenderScale v2 (LowResEffects): no upscale here; the weapon and NVR's pre-tonemap effects run in the
		// same top-left rect and UpscaleAfterEffects upscales after them (the targets stay held until then). Needs the pre-tonemap
		// chain (RenderPreTonemapping); else, or without its resources, v1 below.
		if (ok && FSRSettings.LowResEffects && !V2Off && TheSettingManager->SettingsMain.Main.RenderPreTonemapping && UpscaleEnsureResources()) {
			Up.Active = false;
			Up.Deferred = true;
			return;
		}
		if (ok) ok = UpscaleRun(device);
		// A frame that failed after the world was drawn small: the low-resolution picture stretched (bilinear) and the depth upsampled,
		// so the frame is whole; the next frames are at native resolution (Failed).
		if (!ok && !Up.Whole && Up.CompositePS && Up.CompositeStates && Up.Input.Surface) {
			const RECT region = { 0, 0, (LONG)Up.RenderWidth, (LONG)Up.RenderHeight };
			if (SUCCEEDED(device->StretchRect(Up.Target, &region, Up.Input.Surface, &region, D3DTEXF_NONE))) {
				TheRenderManager->ResolveDepthBuffer(TheTextureManager->DepthTexture);
				UpscaleComposite(device, nullptr);
			}
		}
	}
	UpscaleFinish(ok, false);
}

void TAAEffect::UpscaleBeginEffects() {
	if (!Up.Deferred) return;
	const float sx = (float)Up.RenderWidth / (float)Up.Width, sy = (float)Up.RenderHeight / (float)Up.Height;
	RenderScaleConstant = D3DXVECTOR4(sx, sy, 1.0f / sx, 1.0f / sy);
	RenderRect::Begin(sx, sy);
	RenderRect::LastRectFrame = TheFrameRateManager->FrameNumber;
}

// UNOFFICIAL 2026-10-27 RenderScale v2: the frame's upscale after NVR's pre-tonemap effects (or, if they did not run, after the weapon).
void TAAEffect::UpscaleAfterEffects(IDirect3DSurface9* GameSurface) {
	if (!Up.Deferred) return;
	Up.Deferred = false;
	DroppedInARow = 0;	// UNOFFICIAL 2026-10-30: this frame's upscale runs
	RenderRect::End();
	IDirect3DDevice9* device = TheRenderManager->device;
	RenderRect::RescaleRegistered(device);	// Bloom's top level, read by the game's tonemap and the rain at full size
	bool ok = false;
	{
		static GpuTimer timer("FSR upscale (RenderScale v2)");
		GpuProfileScope gpu(timer, device);
		// The picture to upscale is where the chain left it: GameSurface (normally the world's own target).
		if (GameSurface && GameSurface != Up.Target) {
			D3DSURFACE_DESC desc = {};
			if (SUCCEEDED(GameSurface->GetDesc(&desc)) && desc.Width == Up.Width && desc.Height == Up.Height && desc.MultiSampleType == D3DMULTISAMPLE_NONE) {
				if (Up.Target) Up.Target->Release();
				Up.Target = GameSurface;
				Up.Target->AddRef();
				Up.TargetFormat = desc.Format;
				Up.Hdr = desc.Format == D3DFMT_A16B16G16R16F || desc.Format == D3DFMT_A32B32G32R32F;
			}
		}
		ok = UpscaleRun(device, false);
		Up.AfterEffects = true;	// UNOFFICIAL 2026-11-01: the effects already ran in the small frame (TAA's SmoothEffects skips)
		if (!ok && Up.CompositePS && Up.CompositeStates && Up.Input.Surface) {
			const RECT region = { 0, 0, (LONG)Up.RenderWidth, (LONG)Up.RenderHeight };
			if (SUCCEEDED(device->StretchRect(Up.Target, &region, Up.Input.Surface, &region, D3DTEXF_NONE))) UpscaleComposite(device, nullptr);
		}
		// Every depth NVR's later effects read, full size again: the world's (the composite wrote it into the depth-stencil) and the
		// weapon's (upsampled at its own, unjittered pixels), each resolved; then TESR_DepthBuffer and TESR_NormalsBuffer redrawn.
		if (Up.DepthStencil && Up.CompositePS && Up.CompositeStates) {
			IDirect3DSurface9* oldDepth = nullptr;
			if (FAILED(device->GetDepthStencilSurface(&oldDepth))) oldDepth = nullptr;
			device->SetDepthStencilSurface(Up.DepthStencil);
			TheRenderManager->ResolveDepthBuffer(TheTextureManager->DepthTexture);
			if (UpscaleComposite(device, nullptr, TheTextureManager->DepthTextureViewModel, false, false)) {
				device->SetDepthStencilSurface(Up.DepthStencil);
				TheRenderManager->ResolveDepthBuffer(TheTextureManager->DepthTextureViewModel);
			}
			device->SetDepthStencilSurface(oldDepth);
			if (oldDepth) oldDepth->Release();
		}
	}
	RenderScaleConstant = D3DXVECTOR4(1.0f, 1.0f, 1.0f, 1.0f);
	TheShaderManager->RenderDepthAndNormals(GameSurface ? GameSurface : Up.Target);
	if (ok && !Up.V2Logged) {
		Up.V2Logged = true;
		Logger::Log("UNOFFICIAL RenderScale v2: Vegas Shaders' effects ran at %ux%u, upscaled after them (to %ux%u by %s).", Up.RenderWidth, Up.RenderHeight,
			Up.Width, Up.Height, Upscaler::Choose(FSRSettings.Upscaler)->Name());
	}
	UpscaleFinish(ok, true);
}

// UNOFFICIAL 2026-11-07 (perf-rr) DLSS Ray Reconstruction at RenderScale < 1: the RR point (TAAFSR.cpp RayReconstruct) denoised AND upscaled
// this rect frame's picture into the frame chain (the whole screen now). What UpscaleAfterEffects does after its colour upscale is done here,
// right after RR, so the chain's last steps (luma, exposure, bloom, lens, the pre-tonemap LUT) run at full size on the full picture and the
// image-space hook's UpscaleAfterEffects finds nothing pending (no second upscale): the rect ends (RenderRect::End, buffers registered for
// after the upscale stretched), the world's and the weapon's depths are upsampled into the depth-stencil and resolved, TESR_DepthBuffer and
// TESR_NormalsBuffer redrawn at full size. DLSS SR's history goes stale (it did not see this frame); its jitter index steps on, as UpscaleRun's.
void TAAEffect::UpscaleAfterRayReconstruction(IDirect3DDevice9* Device, IDirect3DSurface9* ChainTarget) {
	if (!Up.Deferred) return;
	Up.Deferred = false;
	DroppedInARow = 0;	// (this frame's upscale ran: RR's)
	RenderRect::End();
	RenderRect::RescaleRegistered(Device);
	{
		static GpuTimer timer("  RR: RenderScale depths to full size");
		GpuProfileScope gpu(timer, Device);
		if (Up.DepthStencil && Up.CompositePS && Up.CompositeStates) {
			IDirect3DSurface9* oldDepth = nullptr;
			if (FAILED(Device->GetDepthStencilSurface(&oldDepth))) oldDepth = nullptr;
			// the world's depth (TESR_DepthBufferWorld, the rect, jittered) upsampled into the depth-stencil (no colour: RR wrote the picture)
			// as UpscaleRun's composite writes it, then resolved; then the weapon's (unjittered pixels), as UpscaleAfterEffects
			if (UpscaleComposite(Device, nullptr, nullptr, false, true)) {
				Device->SetDepthStencilSurface(Up.DepthStencil);
				TheRenderManager->ResolveDepthBuffer(TheTextureManager->DepthTexture);
				if (UpscaleComposite(Device, nullptr, TheTextureManager->DepthTextureViewModel, false, false)) {
					Device->SetDepthStencilSurface(Up.DepthStencil);
					TheRenderManager->ResolveDepthBuffer(TheTextureManager->DepthTextureViewModel);
				}
			}
			Device->SetDepthStencilSurface(oldDepth);
			if (oldDepth) oldDepth->Release();
		}
	}
	RenderScaleConstant = D3DXVECTOR4(1.0f, 1.0f, 1.0f, 1.0f);
	// As after UpscaleRun: the frame counts as upscaled after the effects (TAA's resolve skips it; CapturedNormals does not merge into the
	// normals redrawn from an upsampled depth; SmoothEffects skips it).
	Up.Upscaled = true;
	Up.AfterEffects = true;
	Up.HistoryValid = false;	// DLSS SR / FSR's RenderScale history did not see this frame
	Up.JitterIndex = (Up.JitterIndex + 1) % (std::max)(1, (int)Up.JitterPhases);
	TheShaderManager->RenderDepthAndNormals(ChainTarget ? ChainTarget : Up.Target);
	UpscaleFinish(true, true);
}

void TAAEffect::UpscaleFinish(bool ok, bool v2) {
	Up.Active = false;
	RenderScaleConstant = D3DXVECTOR4(1.0f, 1.0f, 1.0f, 1.0f);
	UpscaleReleaseTargets();
	// UNOFFICIAL 2026-10-30 (top-left picture report): [Shaders.TAA.FSR] DebugView is a test view of the upscaled frame for FSR and DLSS alike,
	// and 1 is exactly a small picture in the top-left corner: said in the log whenever it changes.
	static int loggedDebugView = 0;
	if (FSRSettings.DebugView != loggedDebugView) {
		loggedDebugView = FSRSettings.DebugView;
		if (loggedDebugView) Logger::Log("UNOFFICIAL RenderScale: [Shaders.TAA.FSR] DebugView %d is on, for FSR and DLSS alike (a test view: 1 the world as drawn, small "
			"in the top-left corner, the rest black; 2 stretched without the upscaler; 3 the motion input). 0 = the real picture.", loggedDebugView);
	}
	if (!ok) { Up.HistoryValid = false; return; }

	Up.Frames++;
	const bool changed = Up.LoggedScale != FSRSettings.RenderScale || Up.LoggedWidth != Up.RenderWidth || Up.LoggedHeight != Up.RenderHeight;
	if (changed) {
		Up.LoggedScale = FSRSettings.RenderScale;
		Up.LoggedWidth = Up.RenderWidth;
		Up.LoggedHeight = Up.RenderHeight;
		if (v2) Logger::Log("UNOFFICIAL RenderScale v2: the first-person weapon and Vegas Shaders' effects before tonemapping are drawn at that size too.");
		Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RenderScale %.2f: the world scene is drawn at %ux%u (%.0f%% of the pixels) and upscaled by %s to %ux%u (%s input, jitter %d steps; "
			"%u world passes drawn at that size this frame, %u at the whole screen).",
			FSRSettings.RenderScale, Up.RenderWidth, Up.RenderHeight, 100.0 * Up.RenderWidth * Up.RenderHeight / ((double)Up.Width * Up.Height),
			Upscaler::Choose(FSRSettings.Upscaler)->Name(), Up.Width, Up.Height, Up.Hdr ? "HDR" : "LDR", (int)Up.JitterPhases, Up.DrawsSmall, Up.DrawsFull);
	}
	if (GpuTimer::Enabled && (Up.Frames % 240) == 0) {
		Logger::Log("RENDERSCALE: %u frames upscaled from %ux%u to %ux%u, %u at native resolution (last reason %d); this frame's world passes: %u at the render size, "
			"%u whole screen, %u other", Up.Frames, Up.RenderWidth, Up.RenderHeight, Up.Width, Up.Height, Up.SkippedFrames, Up.LastSkip, Up.DrawsSmall, Up.DrawsFull, Up.DrawsOther);
	}
}
