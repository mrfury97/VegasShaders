#pragma once
// UNOFFICIAL 2026-11-15 DeferredFirstPerson ([Main.Main.Quality] DeferredFirstPerson, restart, test only, on since 2026-11-24 (settings step 101); needs DeferredRenderer 1 or 2
// and GBufferV2): the first-person view model (arms, hands, gun, the Pip-Boy on the wrist) through the full deferred renderer
// (docs/next/FD-FIRSTPERSON-RESULTS.md; plan FULL-DEFERRED-RENDERER-PLAN.md 4.7 "first person"; FD-SKIN-RESULTS.md section 6).
//
// Where it draws: Main::RenderFirstPerson 0x00875110 runs after RenderWorldSceneGraph 0x00873200 (whose main list held the world's G-buffer span
// and light passes), after its own depth clear (Render.cpp RenderFirstPersonHook), with the view model posed by 0x00874C10 in a space moved by
// ShadowSceneNode::kLightingOffset (+0x1E4: the player node at the origin; the first-person camera at the world eye + that offset; the lamps
// given to its draws at their world position + the same offset), its own camera and field of view.
//
// What happens with the switch on, each first-person frame the renderer is live:
//   Begin (before the game's render): the checks below; if they pass, the view-model span of the G-buffer opens (CapturedNormals::
//     BeginViewModelSpan, VmDeferred: the targets bound over the world's G-buffer without a clear), and inside it the same records as the world
//     are chosen at every bind (objects, parallax, skin, alpha-tested hair: DeferredRenderer::Choose / DeferredSkin::Choose read the open span).
//     A draw whose geometry is not under the player's first-person node (the HUD's 3D objects the same render draws with another camera)
//     takes its forward record (DeferredRenderer::BeforeDraw, ViewModelRoot), as a blended draw does. The game's own lamp and highlight passes
//     of the deferred parts are skipped as in the world (Render.cpp's pass hooks see the span as a world one).
//   End (after the game's render, the span closed):
//     1. the view-model mark: one quad, depth-tested against the first-person depth just drawn (written = the view model's), sets stencil
//        bit 0x40 there and clears it everywhere else. The world's light passes ran before the view model was drawn, so they never touch it;
//        these passes run only where the mark is, so they never touch the world;
//     2. GearWeather (core/GearWeather.h), when on: the weather layers into the view model's G-buffer;
//     3. DeferredSun::Apply and DeferredLamps::Apply again (Vm.on: the first-person camera's ray mapping, its own projection for the depth
//        test, the lamps placed as the game places them for first person, the sun's shadow through ViewModelShadows' map, positions from the
//        G-buffer's camera distance, the stencil test on the mark) into a light target (fp16, cleared over the view model's box);
//     4. the resolve: the frame (the stored curve by now: LinearScene encoded it at the world's end) at the mark gets enc(dec(frame) + light)
//        (the deferred lamp pass's own copy-path formula), from a copy of the frame over the box;
//     5. the stencil bits 0x7F back to 0 (bit 0x80 stays WeaponMask's).
// The lamp lists: DeferredLamps' froxel lists were made for the world camera's tiles. The pixel's tile is the world camera's tile of the same
// direction: the first-person camera turns with the world camera (0x00874C10 copies its rotation) and sits at the world eye (checked, below),
// so the screen share is rescaled about the centre by tan(fp half fov) / tan(world half fov) through DL_View, and DL_Right.w / DL_Up.w take
// the world's tangents: the ray the pass rebuilds is exactly the first-person ray (tests/fd_firstperson_math.py proves the identity), and its
// tile is the world's. The lamps: DL_Rows' translation = kLightingOffset - the first-person eye (lamp + offset - eye, what the forward draws
// get); the atlas tiles and the sun cascades are world-camera-relative, which is the same frame while the eye check holds.
//
// Forward (the view model exactly as before), with ONE log line per reason when the reason changes:
//   - third person, the Pip-Boy or another menu rendered in 3D, VATS, the Bodycam effect on;
//   - iron sights / scopes: the player aiming (the game moves the first-person camera onto the weapon's sighting node, 0x00874C10; ScopeFix's
//     territory), or the first-person eye more than 2 units off the world eye for any other reason (a camera mod);
//   - an FOV mod or anything that gives the first-person camera an off-centre or differently shaped frustum (the tile mapping needs both
//     cameras centred and of one aspect), or a rotation different from the world camera's;
//   - the game's MSAA (the view-model span refuses multisampled targets), the flashlight on (the deferred flashlight lights the world's
//     pixels only, and the game's flashlight lamp is in no lamp pass of a deferred part), no directional light, the G-buffer not captured
//     this frame, or anything the passes need missing (they did not load, no stencil in the depth surface).
#include "SkinBones.h"	// 2026-11-18: a skin's bone slots may be stale
#include "DeferredRenderer.h"
#include "DeferredSkin.h"
#include "FlashlightDeferred.h"
#include "ViewModelLight.h"
#include "GearWeather.h"
#include "RtLightUnits.h"
#include "ScopeOverlay.h"

namespace DeferredFirstPerson {
	inline const char* kSection = "Main.Main.Quality";
	inline int Wanted = -1;
	inline bool WantedAtStartup() {
		if (Wanted < 0) {
			const bool key = TheSettingManager->GetSettingI(kSection, "DeferredFirstPerson") != 0;
			Wanted = key && CapturedNormals::V2CompiledIn() ? 1 : 0;
		}
		return Wanted != 0;
	}

	inline bool Decided = false;		// this frame's Begin passed its checks (the span may still refuse: Open says)
	inline bool Open = false;			// the view-model G-buffer span is open (DeferredRenderer::ViewModelRoot set)
	inline bool NativeLayerActive = false;	// FirstPersonNativeScale swapped full-size G-buffer targets into CapturedNormals
	inline const char* LastWhy = nullptr;
	inline unsigned WhyLines = 0;
	inline D3DXVECTOR3 Eye(0.0f, 0.0f, 0.0f), Offset(0.0f, 0.0f, 0.0f);
	inline NiCamera* Camera = nullptr;
	inline bool MapMade = false;		// ViewModelShadows drew the map this frame (Render.cpp)
	inline float EyeOff = 0.0f, Turn = 1.0f;	// (the diagnostic line) the eye's distance from the world eye + offset, the cameras' forward cosine
	inline double AimHoldUntil = 0.0;		// real-time hold past IsAiming() dropping during firing or ADS zoom-out interpolation
	inline unsigned DiagLines = 0;
	inline D3DXVECTOR4 Frame[6] = {};	// the first-person camera in Includes/DeferredDepth.hlsl's DD layout (End)
	inline IDirect3DTexture9* LightTex = nullptr;  inline IDirect3DSurface9* LightSurf = nullptr;
	inline IDirect3DTexture9* FrameTex = nullptr;  inline IDirect3DSurface9* FrameSurf = nullptr;
	inline D3DSURFACE_DESC LightMade = {}, FrameMade = {};
	inline IDirect3DPixelShader9* ResolvePS = nullptr;
	inline IDirect3DPixelShader9* NullPS = nullptr;
	inline IDirect3DStateBlock9* State = nullptr;
	inline bool Failed = false;			// the session's: a resource could not be made (logged)
	inline unsigned long long StatFrames = 0, StatSpans = 0, StatPasses = 0, StatNoBinds = 0, StatForward = 0, StatBoxPixels = 0;

	// One line per reason, when it changes (at most 64 a session); null = deferred.
	inline void Note(const char* why) {
		if (why == LastWhy) return;
		LastWhy = why;
		if (WhyLines >= 64) return;
		WhyLines++;
		Logger::Log(why ? "UNOFFICIAL DeferredFirstPerson: first person FORWARD: %s (logged when the reason changes)" : "UNOFFICIAL DeferredFirstPerson: first person DEFERRED%s",
			why ? why : " (the view model writes the G-buffer; DeferredSun / DeferredLamps light it again with the first-person camera)");
	}

	// ShaderManager::Initialize (never on the render thread): the resolve and mark shaders, and GearWeather's pass.
	inline void Load() {
		if (!WantedAtStartup() || ResolvePS || Failed) return;
		IDirect3DDevice9* d = TheRenderManager ? TheRenderManager->device : nullptr;
		if (!d) return;
		// c0: x gamma, y 1/gamma, z 1=linear, w 1=GearWeather ambient update; c1: flat ambient + sky scale; c2..c10: sky SH; c11: CopyTex box UVs.
		static const char* kResolve =
			"sampler2D F : register(s0); sampler2D L : register(s1);"
			"sampler2D OA : register(s2); sampler2D OM : register(s3);"
			"sampler2D NA : register(s4); sampler2D NM : register(s5); sampler2D GN : register(s6);"
			"float4 G : register(c0); float4 A : register(c1); float4 SH[9] : register(c2); float4 B : register(c11);"
			"float3 DecN(float2 e) {"
			"  float3 n = float3(e, 1.0 - abs(e.x) - abs(e.y));"
			"  float t = saturate(-n.z);"
			"  n.xy += float2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);"
			"  return normalize(n);"
			"}"
			"float4 main(float2 uv : TEXCOORD0) : COLOR0 {"
			"  float4 u = float4(uv, 0, 0);"
			"  float4 fSample = tex2Dlod(F, u);"
			"  float3 s = max(fSample.rgb, 0.0);"
			"  float3 l = max(tex2Dlod(L, u).rgb, 0.0);"
			"  float3 sLin = G.z > 0.5 ? s : pow(s, G.x);"
			"  [branch] if (G.w > 0.5 && uv.x >= B.x && uv.y >= B.y && uv.x <= B.z && uv.y <= B.w) {"
			"    float4 oa = tex2Dlod(OA, u); float4 om = tex2Dlod(OM, u);"
			"    float4 na = tex2Dlod(NA, u); float4 nm = tex2Dlod(NM, u);"
			"    float oMet = saturate((om.r - floor(om.r + 0.004)) / 0.49);"
			"    float nMet = saturate((nm.r - floor(nm.r + 0.004)) / 0.49);"
			"    float oRgh = om.b < -1.5 ? -2.0 - om.b : (om.b < 0.0 ? 1.0 : om.b);"
			"    float nRgh = nm.b < -1.5 ? -2.0 - nm.b : (nm.b < 0.0 ? 1.0 : nm.b);"
			"    float chg = dot(abs(na.rgb - oa.rgb), 1.0) + abs(nMet - oMet) + abs(nRgh - oRgh);"
			"    [branch] if (om.r > 0.5 && chg > 1e-4) {"
			"      float3 N = DecN(tex2Dlod(GN, u).rg);"
			"      float3 shE = SH[0].rgb + SH[1].rgb * N.y + SH[2].rgb * N.z + SH[3].rgb * N.x"
			"        + SH[4].rgb * (N.x * N.y) + SH[5].rgb * (N.y * N.z) + SH[6].rgb * (3.0 * N.z * N.z - 1.0)"
			"        + SH[7].rgb * (N.x * N.z) + SH[8].rgb * (N.x * N.x - N.y * N.y);"
			"      float3 skyTerm = pow(max(shE, 0.0), G.x * 0.5) * A.w;"
			"      float3 ambE = A.rgb + skyTerm;"
			"      float3 oDiff = oa.rgb * (1.0 - oMet);"
			"      float3 nDiff = na.rgb * (1.0 - nMet);"
			"      if (max(ambE.r, max(ambE.g, ambE.b)) < 1e-4) {"
			"        float3 est = sLin / max(oDiff, 0.06);"
			"        ambE = clamp(est, 0.015, 0.35);"
			"      }"
			"      float metFade = oMet > 0.02 ? lerp(1.0, saturate(nMet / oMet), 0.65 * oMet) : 1.0;"
			"      float glossGain = saturate(oRgh - nRgh) * (1.0 - nRgh);"
			"      float3 skySheen = skyTerm * (0.14 * glossGain * (0.4 + 0.6 * saturate(N.z)));"
			"      sLin = max(sLin * metFade + ambE * (nDiff - oDiff), 0.0) + skySheen;"
			"    }"
			"  }"
			"  float3 outLin = sLin + l;"
			"  return float4(G.z > 0.5 ? outLin : pow(max(outLin, 0.0), G.y), fSample.a);"
			"}";
		static const char* kNull = "float4 main() : COLOR0 { return 0.0; }";
		ResolvePS = CapturedNormals::FullScreenShader(d, kResolve, "DeferredFirstPerson resolve", Failed);
		NullPS = ResolvePS ? CapturedNormals::FullScreenShader(d, kNull, "DeferredFirstPerson mark", Failed) : nullptr;
		if (!ResolvePS || !NullPS) { Failed = true; Logger::Log("[ERROR] UNOFFICIAL DeferredFirstPerson: its shaders did not compile: first person stays forward"); return; }
		Logger::Log("UNOFFICIAL DeferredFirstPerson: loaded (the view model through the deferred renderer, test only)%s", GearWeather::WantedAtStartup() ? "; GearWeather on" : "");
		GearWeather::Load();
	}

	// Once a frame (Render.cpp, after DeferredSun::BeginFrame).
	inline void BeginFrame() {
		if (NativeLayerActive) { CapturedNormals::PopVmNativeGBuffer(); NativeLayerActive = false; }
		Decided = false;
		GearWeather::BeginFrame(WantedAtStartup() && !Failed && DeferredRenderer::Live);
		if (!WantedAtStartup() || !GpuTimer::Enabled) { StatFrames = 0; return; }
		if (++StatFrames < 240) return;
		const double f = (double)StatFrames;
		Logger::Log("DEFERRED FIRST PERSON %u frames: view-model spans %.2f a frame, passes %.2f (spans with no deferred bind %.2f), forward frames %.2f (last reason: %s), box pixels %.0f",
			(unsigned)StatFrames, StatSpans / f, StatPasses / f, StatNoBinds / f, StatForward / f, LastWhy ? LastWhy : "none", StatBoxPixels / f);
		StatFrames = 0; StatSpans = StatPasses = StatNoBinds = StatForward = StatBoxPixels = 0;
	}

	inline bool HasStencil(D3DFORMAT f) { return f == (D3DFORMAT)MAKEFOURCC('I', 'N', 'T', 'Z') || f == D3DFMT_D24S8 || f == D3DFMT_D24FS8; }

	inline void Quad(IDirect3DDevice9* d, const RECT& r, UINT w, UINT h, float z, DWORD zFunc, DWORD stencilFunc, DWORD ref, DWORD readMask, DWORD writeMask,
		DWORD passOp, DWORD zFailOp, DWORD colour, IDirect3DPixelShader9* ps);	// (below)

	// Why a first-person frustum cannot be used (null: it can). Begin checks the camera's frustum as it stands before the render (the render
	// sets the first-person field of view again; End checks once more and latches a refusal: see Latch).
	inline const char* FrustumWhy(const NiFrustum& f) {
		const float width = f.Right - f.Left, height = f.Top - f.Bottom;
		if (f.Ortho || !(width > 1.0e-6f) || !(height > 1.0e-6f) || !(f.Near > 0.0f) || !(f.Far > f.Near)) return "the first-person camera's frustum is not a perspective one";
		if (fabsf(f.Right + f.Left) > 1.0e-3f * width || fabsf(f.Top + f.Bottom) > 1.0e-3f * height) return "the first-person frustum is off-centre (an FOV or camera mod)";
		const float tanWX = fabsf(TheRenderManager->InvProjMatrix._11), tanWY = fabsf(TheRenderManager->InvProjMatrix._22);
		if (!(tanWX > 1.0e-6f) || !(tanWY > 1.0e-6f) || fabsf((width / height) / (tanWX / tanWY) - 1.0f) > 0.02f) return "the first-person frustum's shape differs from the world camera's (an FOV mod)";
		return nullptr;
	}
	// A refusal found in End, after the view model drew its deferred records (its frustum changed inside the render): that frame's view model
	// stays without its passes, and Begin refuses for the next 300 frames, then tries again (one log line a change, as every reason).
	inline const char* Latched = nullptr;
	inline UInt32 LatchedUntil = 0;


	// Render.cpp RenderFirstPersonHook, right before CapturedNormals::BeginViewModelSpan. camera: Main's first-person camera.
	// nativeLayer (fd-batch8 merge): FirstPersonNative::Begin returned true this frame ([Main.Main.Quality] FirstPersonNativeScale with
	// RenderScale < 1 and FSR / DLSS): the view model draws into that full-size layer, composited after the upscale, not into the world's
	// G-buffer at the world's size, so this frame's view model stays forward (FirstPersonNativeScale wins: it is a player setting).
	inline void Begin(IDirect3DDevice9* d, NiCamera* camera, bool viewModelMap, bool nativeLayer = false) {
		if (NativeLayerActive) { CapturedNormals::PopVmNativeGBuffer(); NativeLayerActive = false; }
		Decided = Open = false;
		CapturedNormals::VmDeferred = false;
		MapMade = viewModelMap;
		if (!WantedAtStartup() || Failed || !DeferredRenderer::Live || !d) return;
		const auto& gs = TheShaderManager->GameState;
		const char* why = nullptr;
		D3DSURFACE_DESC rtDesc = {};
		if (!Player || !Player->IsDrawnFirstPerson() || !Player->firstPersonNiNode || !camera) why = "third person (or no first-person model)";
		else if (nativeLayer) {
			IDirect3DSurface9* rt0 = nullptr;
			if (FAILED(d->GetRenderTarget(0, &rt0)) || !rt0) why = "FirstPersonNativeScale render target unavailable";
			else {
				if (FAILED(rt0->GetDesc(&rtDesc))) why = "FirstPersonNativeScale render target desc unavailable";
				else if (rtDesc.MultiSampleType != D3DMULTISAMPLE_NONE) why = "FirstPersonNativeScale with MSAA (set FirstPersonNativeSamples = 0 for deferred first person)";
				rt0->Release();
			}
		}
		// (2026-11-25 fd-complete, both tried and kept forward: the Pip-Boy open draws no world behind it, so there is no G-buffer, lamp table
		// or sun this frame to light the arm from (bench 20261125-085930-slotA: with this check off, first person fell back on "the world's
		// G-buffer was not captured this frame"); VATS targeting and its kill camera draw no view model at all (20261125-090315-slotA: no
		// view-model span after VATS opened), so there is nothing for the deferred passes to light)
		if (!why && (InterfaceManager->IsPipBoyOpen() || gs.PipBoyIsOn)) why = "the Pip-Boy is open";
		else if (gs.VATSIsOn) why = "VATS";
		// (2026-11-24 fd-complete batch 2: Bodycam no longer sends first person forward: it only widens the world camera's field of view, which
		// FrustumWhy / MakeFrame's tangent rescale already handle; [Main.Main.Quality] DeferredFirstPersonBodycam false = forward under it, as before)
		else if (TheShaderManager->Effects.Bodycam && TheShaderManager->Effects.Bodycam->Enabled && !TheSettingManager->GetSettingI(kSection, "DeferredFirstPersonBodycam")) why = "the Bodycam effect is on";
		else if (ScopeOverlay::Visible()) why = "the rifle scope overlay is up";
		// (2026-11-24 fd-complete batch 2: the deferred flashlight lights the view model too (End, FlashlightDeferred::Vm); [Main.Main.Quality]
		// DeferredFirstPersonFlashlight false = forward while it is on, as before)
		else if ((FlashlightDeferred::DrewThisFrame() || FlashlightDeferred::Wanted()) && !TheSettingManager->GetSettingI(kSection, "DeferredFirstPersonFlashlight"))
			why = "the flashlight is on (the deferred flashlight lights the world only; its game lamp is no lamp pass of a deferred part)";
		else if (!Tes || !Tes->directionalLight) why = "no directional light";
		else if (CapturedNormals::Data.x <= 0.0f || CapturedNormals::AlbedoData.x <= 0.0f) why = "the world's G-buffer was not captured this frame";
		else if (TheSettingManager->GetSettingI(kSection, "DeferredFirstPersonLive") == 0) why = "DeferredFirstPersonLive is off (the A/B key)";
		else if (Latched && (UInt32)TheFrameRateManager->FrameNumber < LatchedUntil) why = Latched;
		else if (const char* frustum = FrustumWhy(camera->Frustum)) why = frustum;
		if (!why) {
			// the first-person eye against the world eye: kLightingOffset (ShadowSceneNode +0x1E4, set by 0x00874C10 with the eye at +0x1F0)
			const ShadowSceneNode* node = BSShaderManager::GetShadowSceneNode(0);
			const NiPoint3 offset = node ? *(const NiPoint3*)((const char*)node + 0x1E4) : NiPoint3{ 0.0f, 0.0f, 0.0f };
			const NiPoint3& eye = camera->m_worldTransform.pos;
			const D3DXVECTOR4& world = TheRenderManager->CameraPosition;
			const float dx = eye.x - offset.x - world.x, dy = eye.y - offset.y - world.y, dz = eye.z - offset.z - world.z;
			const NiMatrix33& r = camera->m_worldTransform.rot;
			const float turn = r.data[0][0] * TheRenderManager->CameraForward.x + r.data[1][0] * TheRenderManager->CameraForward.y + r.data[2][0] * TheRenderManager->CameraForward.z;
			const double now = TheFrameRateManager ? TheFrameRateManager->Time : 0.0;
			if (now < AimHoldUntil - 5.0) AimHoldUntil = 0.0;
			const bool rawAiming = Player->process && Player->IsAiming();
			if (rawAiming) AimHoldUntil = now + 0.75;
			const bool aiming = rawAiming || now < AimHoldUntil;
			const float maxEyeOffSq = aiming ? 4096.0f : 4.0f;	// iron sights / red dot / optics move the first-person eye onto the sight
			const float minTurn = aiming ? 0.90f : 0.9999f;
			if (!_finite(dx) || dx * dx + dy * dy + dz * dz > maxEyeOffSq) why = aiming
				? "the first-person eye sits more than 64 units off the world eye while aiming"
				: "the first-person eye sits more than 2 units off the world eye (a sight, a scope or a camera mod)";
			else if (turn < minTurn) why = "the first-person camera is turned away from the world camera (a camera mod)";
			else { Eye = D3DXVECTOR3(eye.x, eye.y, eye.z); Offset = D3DXVECTOR3(offset.x, offset.y, offset.z); EyeOff = sqrtf(dx * dx + dy * dy + dz * dz); Turn = turn; }
		}
		if (!why) {
			IDirect3DSurface9* depth = nullptr;
			D3DSURFACE_DESC z = {};
			if (FAILED(d->GetDepthStencilSurface(&depth)) || !depth || FAILED(depth->GetDesc(&z)) || !HasStencil(z.Format)) why = "the depth surface has no stencil bits (the view-model mark needs one)";
			if (depth) depth->Release();
		}
		if (!why && ((!DeferredSun::Pass && !DeferredSun::ComputeEligible(false)) ||
			(!DeferredLamps::Pass && !DeferredLamps::ComputePossible()))) why = "a light pass did not load";
		if (why) { StatForward++; Note(why); return; }
		if (nativeLayer) {
			if (!CapturedNormals::PushVmNativeGBuffer(d, rtDesc.Width, rtDesc.Height)) {
				StatForward++;
				Note("FirstPersonNativeScale G-buffer could not be made");
				return;
			}
			NativeLayerActive = true;
		}
		Camera = camera;
		Decided = true;
		CapturedNormals::VmDeferred = true;	// (BeginViewModelSpan opens the span for it)
		if (GearWeather::Live && GearWeather::Pass && NullPS) {	// GearWeather: the ids' bits cleared over the screen first (a stencil-only quad), so a part's pixels hold its own id or none
			IDirect3DSurface9* rt = nullptr;
			D3DSURFACE_DESC t = {};
			if (SUCCEEDED(d->GetRenderTarget(0, &rt)) && rt && SUCCEEDED(rt->GetDesc(&t)) && (State || SUCCEEDED(d->CreateStateBlock(D3DSBT_ALL, &State)))) {
				D3DVIEWPORT9 vp = {}; d->GetViewport(&vp);
				State->Capture();
				const RECT screen = { 0, 0, (LONG)t.Width, (LONG)t.Height };
				Quad(d, screen, t.Width, t.Height, 0.0f, D3DCMP_ALWAYS, D3DCMP_ALWAYS, 0, 0xFF, GearWeather::kIdMask | GearWeather::kMark, D3DSTENCILOP_REPLACE, D3DSTENCILOP_REPLACE, 0, NullPS);
				d->SetViewport(&vp);
				State->Apply();
				if (PassSums::AfterStateBlock) PassSums::AfterStateBlock(d);
			}
			if (rt) rt->Release();
		}
	}

	// Render.cpp, right after CapturedNormals::BeginViewModelSpan.
	inline void SpanOpened(IDirect3DDevice9* d) {
		if (!Decided) return;
		if (!CapturedNormals::VmSpan) { CapturedNormals::VmDeferred = false; Decided = false; StatForward++; Note("the view-model span did not open (the first-person target is not the G-buffer's size)"); return; }
		Open = true;
		StatSpans++;
		DeferredRenderer::ViewModelRoot = Player->firstPersonNiNode;
		DeferredLamps::DeferredBindsThisSpan = 0;		// (the world's were used by its passes: these count the view model's)
		DeferredSun::LightFreeBindsThisSpan = DeferredSun::OlderBindsThisSpan = 0;
		SkinInfo* skin = Player->firstPersonSkinInfo;
		GearWeather::BeginTagging(skin ? skin->WeaponNode : nullptr);	// (the ids' bits were cleared in Begin)
	}

	// A stencil / colour quad over a box (pixels) of the bound targets: ALWAYS, ref, write mask, the pass and depth-fail ops; z: the quad's
	// depth with the depth test func (D3DCMP_ALWAYS = no test); ps the shader, colour: the colour write mask.
	inline void Quad(IDirect3DDevice9* d, const RECT& r, UINT w, UINT h, float z, DWORD zFunc, DWORD stencilFunc, DWORD ref, DWORD readMask, DWORD writeMask,
		DWORD passOp, DWORD zFailOp, DWORD colour, IDirect3DPixelShader9* ps) {
		const struct { D3DRENDERSTATETYPE s; DWORD v; } kStates[] = {
			{ D3DRS_ZENABLE, zFunc == D3DCMP_ALWAYS ? (DWORD)D3DZB_FALSE : (DWORD)D3DZB_TRUE }, { D3DRS_ZFUNC, zFunc }, { D3DRS_ZWRITEENABLE, FALSE },
			{ D3DRS_STENCILENABLE, TRUE }, { D3DRS_TWOSIDEDSTENCILMODE, FALSE }, { D3DRS_STENCILFUNC, stencilFunc }, { D3DRS_STENCILREF, ref },
			{ D3DRS_STENCILMASK, readMask }, { D3DRS_STENCILWRITEMASK, writeMask }, { D3DRS_STENCILPASS, passOp }, { D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP },
			{ D3DRS_STENCILZFAIL, zFailOp }, { D3DRS_ALPHATESTENABLE, FALSE }, { D3DRS_ALPHABLENDENABLE, FALSE }, { D3DRS_SEPARATEALPHABLENDENABLE, FALSE },
			{ D3DRS_CULLMODE, D3DCULL_NONE }, { D3DRS_COLORWRITEENABLE, colour }, { D3DRS_SCISSORTESTENABLE, FALSE }, { D3DRS_CLIPPLANEENABLE, 0 },
			{ D3DRS_FOGENABLE, FALSE }, { D3DRS_SRGBWRITEENABLE, FALSE } };
		for (const auto& k : kStates) d->SetRenderState(k.s, k.v);
		const D3DVIEWPORT9 full = { 0, 0, w, h, 0.0f, 1.0f };
		d->SetViewport(&full);
		d->SetVertexShader(nullptr);
		d->SetPixelShader(ps);
		d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
		const float x0 = (float)r.left - 0.5f, y0 = (float)r.top - 0.5f, x1 = (float)r.right - 0.5f, y1 = (float)r.bottom - 0.5f;
		const float u0 = (float)r.left / w, v0 = (float)r.top / h, u1 = (float)r.right / w, v1 = (float)r.bottom / h;
		const float quad[4][6] = { { x0, y0, z, 1, u0, v0 }, { x1, y0, z, 1, u1, v0 }, { x0, y1, z, 1, u0, v1 }, { x1, y1, z, 1, u1, v1 } };
		d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
	}

	// The first-person camera's frame (DD layout), its projection's depth terms, and the passes' constants. Non-null: an unusable frustum.
	inline const char* MakeFrame(const D3DVIEWPORT9& vp, UINT w, UINT h, float proj[2]) {
		const NiFrustum& f = Camera->Frustum;
		if (const char* why = FrustumWhy(f)) return why;
		const NiPoint3& curEye = Camera->m_worldTransform.pos;
		if (_finite(curEye.x) && _finite(curEye.y) && _finite(curEye.z))
			Eye = D3DXVECTOR3(curEye.x, curEye.y, curEye.z);
		const float width = f.Right - f.Left, height = f.Top - f.Bottom;
		const float tanWX = fabsf(TheRenderManager->InvProjMatrix._11), tanWY = fabsf(TheRenderManager->InvProjMatrix._22);
		const bool reversed = NiDX9Renderer::GetSingleton()->m_fZClear < 1.0f;
		const float step = 1.0f / (f.Far - f.Near);
		proj[0] = reversed ? -(f.Near * step) : f.Far * step;
		proj[1] = reversed ? f.Near * f.Far * step : -(f.Near * f.Far * step);
		const NiMatrix33& r = Camera->m_worldTransform.rot;	// the axes are its columns: forward, up, side (RenderManager::SetupSceneCamera)
		Frame[0] = D3DXVECTOR4(r.data[0][2], r.data[1][2], r.data[2][2], width * 0.5f);
		Frame[1] = D3DXVECTOR4(r.data[0][1], r.data[1][1], r.data[2][1], height * 0.5f);
		Frame[2] = D3DXVECTOR4(r.data[0][0], r.data[1][0], r.data[2][0], proj[0]);
		Frame[3] = D3DXVECTOR4(vp.Width ? 1.0f / (float)vp.Width : 0.0f, vp.Height ? 1.0f / (float)vp.Height : 0.0f, (float)vp.X, (float)vp.Y);
		Frame[4] = D3DXVECTOR4(Eye.x, Eye.y, Eye.z, proj[1]);
		Frame[5] = D3DXVECTOR4(-(f.Right + f.Left) / width, -(f.Top + f.Bottom) / height, 1.0f / (float)w, 1.0f / (float)h);
		// DeferredLamps (c1-c7): the screen share rescaled about the centre (k = tan fp / tan world), the world's tangents, the first-person axes,
		// a linear target (the light target), the lamps' rows (identity, translation kLightingOffset - the eye)
		const float kx = Frame[0].w / tanWX, ky = Frame[1].w / tanWY;
		float (&c)[7][4] = DeferredLamps::Vm.c;
		c[0][0] = kx / (float)vp.Width; c[0][1] = ky / (float)vp.Height;
		c[0][2] = (float)vp.X - 0.5f * (1.0f - kx) * (float)vp.Width / kx; c[0][3] = (float)vp.Y - 0.5f * (1.0f - ky) * (float)vp.Height / ky;
		c[1][0] = Frame[0].x; c[1][1] = Frame[0].y; c[1][2] = Frame[0].z; c[1][3] = tanWX;
		c[2][0] = Frame[1].x; c[2][1] = Frame[1].y; c[2][2] = Frame[1].z; c[2][3] = tanWY;
		c[3][0] = Frame[2].x; c[3][1] = Frame[2].y; c[3][2] = Frame[2].z; c[3][3] = 1.0f;
		const D3DXVECTOR3 shift = Offset - Eye;
		const float rows[3][4] = { { 1.0f, 0.0f, 0.0f, shift.x }, { 0.0f, 1.0f, 0.0f, shift.y }, { 0.0f, 0.0f, 1.0f, shift.z } };
		memcpy(c[4], rows, sizeof(rows));
		DeferredLamps::Vm.depth[0] = proj[0]; DeferredLamps::Vm.depth[1] = proj[1];
		// DeferredSun: the DD block, the depth terms, the view model's sun map (the light's view x projection: the true point; w 0.5 = this map)
		memcpy(DeferredSun::Vm.frame, Frame, sizeof(Frame));
		DeferredSun::Vm.depth[0] = proj[0]; DeferredSun::Vm.depth[1] = proj[1];
		const ShadowsExteriorEffect* shadows = TheShaderManager->Effects.ShadowsExteriors;
		const bool map = MapMade && shadows && ViewModelLight::Frame == (unsigned)TheFrameRateManager->FrameNumber;
		memcpy(DeferredSun::Vm.charM, &ViewModelLight::ViewProj, sizeof(DeferredSun::Vm.charM));
		if (shadows) { memcpy(DeferredSun::Vm.charCenter, &shadows->Constants.CharacterCenter, 16); memcpy(DeferredSun::Vm.charData, &shadows->Constants.CharacterData, 16); }
		if (!map) DeferredSun::Vm.charData[3] = 0.0f;
		return nullptr;
	}

	// The view model's screen box from its parts' bounds (as ShadowManager's ViewModelSpheres gathers them), the whole view if none.
	inline void Spheres(NiAVObject* o, RECT& box, bool& any, int depth) {
		// (2026-11-16) not the root's APP_CULLED: the game clears it only inside the first-person render (Render.cpp's ViewModelShadows note) and
		// sets it again after, so read here (after the render) the walk stopped at the root and the box was always the whole screen (batch 8:
		// "box pixels 2,073,600" in every window)
		if (!o || depth > 48 || (depth > 0 && (o->m_flags & NiAVObject::NiFlags::APP_CULLED))) return;
		if (o->IsGeometry()) {
			NiGeometry* g = static_cast<NiGeometry*>(o);
			NiSkinInstance* skin = g->skinInstance;
			RECT r;
			if (skin && skin->SkinData && skin->SkinData->BoneData && skin->BoneObjects) {
				for (UInt32 i = 0; i < skin->SkinData->Bones; i++)
					if (SkinBones::Live(skin->BoneObjects[i]) && skin->SkinData->BoneData[i].Verts) {
						const D3DXVECTOR4 s = GearWeather::Sphere(skin->BoneObjects[i]->m_worldTransform, skin->SkinData->BoneData[i].Bound, Eye);
						if (GearWeather::Near(s) && GearWeather::SphereBox(s, Frame, r)) GearWeather::Grow(box, r, any);
					}
			}
			else if (g->geomData) { const D3DXVECTOR4 s = GearWeather::Sphere(g->m_worldTransform, g->geomData->m_kBound, Eye); if (GearWeather::Near(s) && GearWeather::SphereBox(s, Frame, r)) GearWeather::Grow(box, r, any); }
			return;
		}
		NiNode* n = o->IsNiNode();
		if (!n) return;
		if (n->IsKindOf<NiSwitchNode>()) {
			NiSwitchNode* s = static_cast<NiSwitchNode*>(n);
			if (s->m_iIndex >= 0 && s->m_iIndex < n->m_children.end) Spheres(n->m_children.data[s->m_iIndex], box, any, depth + 1);
			return;
		}
		for (int i = 0; i < n->m_children.end; i++) Spheres(n->m_children.data[i], box, any, depth + 1);
	}

	inline bool Ensure(IDirect3DDevice9* d, IDirect3DTexture9*& tex, IDirect3DSurface9*& surf, D3DSURFACE_DESC& made, UINT w, UINT h, D3DFORMAT format, const char* what) {
		if (tex && made.Width == w && made.Height == h && made.Format == format) return true;
		if (surf) { surf->Release(); surf = nullptr; }
		if (tex) { tex->Release(); tex = nullptr; }
		if (FAILED(d->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, format, D3DPOOL_DEFAULT, &tex, NULL)) || FAILED(tex->GetSurfaceLevel(0, &surf))) {
			if (tex) { tex->Release(); tex = nullptr; }
			Logger::Log("[ERROR] UNOFFICIAL DeferredFirstPerson: could not make %s (%ux%u): first person forward from now on", what, w, h);
			Failed = true;
			return false;
		}
		made.Width = w; made.Height = h; made.Format = format;
		return true;
	}

	// Render.cpp, right after CapturedNormals::EndViewModelSpan (the G-buffer unbound), before the view model's shadow constants are put back.
	inline void End(IDirect3DDevice9* d) {
		struct NativePopGuard {
			~NativePopGuard() {
				if (NativeLayerActive) {
					CapturedNormals::PopVmNativeGBuffer();
					NativeLayerActive = false;
				}
			}
		} nativePopGuard;
		GearWeather::EndTagging();
		CapturedNormals::VmDeferred = false;
		if (!Open) return;
		Open = false;
		DeferredRenderer::ViewModelRoot = nullptr;
		DeferredRenderer::AtSpanEnd(d);	// a deferred record still bound goes back to its forward one (later draws light their own lamps)
		DeferredSkin::DrawnThisSpan = false;	// (the hands' skin is not the next world span's: DeferredSkinBlur fills from the world's span only)
		if (DeferredLamps::DeferredBindsThisSpan == 0 && DeferredSun::LightFreeBindsThisSpan == 0) { StatNoBinds++; return; }
		IDirect3DSurface9* target = nullptr;
		IDirect3DSurface9* depth = nullptr;
		d->GetRenderTarget(0, &target);
		d->GetDepthStencilSurface(&depth);
		D3DSURFACE_DESC t = {};
		if (!target || !depth || FAILED(target->GetDesc(&t)) || (!State && FAILED(d->CreateStateBlock(D3DSBT_ALL, &State)))) {
			if (target) target->Release(); if (depth) depth->Release();
			DeferredLamps::DeferredBindsThisSpan = 0; DeferredSun::LightFreeBindsThisSpan = DeferredSun::OlderBindsThisSpan = 0;
			return;
		}
		D3DVIEWPORT9 vp = {}; d->GetViewport(&vp);
		float proj[2] = {};
		const char* why = MakeFrame(vp, t.Width, t.Height, proj);
		RECT box = {}; bool any = false;
		if (!why) {	// the parts' screen box: GearWeather's copies only (the light passes, their clear and the resolve go by the stencil mark)
			Spheres(Player->firstPersonNiNode, box, any, 0);
			GearWeather::ClipTo(box, t.Width, t.Height);
			if (!any || box.right <= box.left || box.bottom <= box.top) box = { 0, 0, (LONG)t.Width, (LONG)t.Height };
		}
		if (!why && (!Ensure(d, LightTex, LightSurf, LightMade, t.Width, t.Height, D3DFMT_A16B16G16R16F, "the light target") ||
			!Ensure(d, FrameTex, FrameSurf, FrameMade, t.Width, t.Height, t.Format, "the frame's copy"))) why = "a target could not be made";
		if (why) {
			// the view model drew its deferred records already: they are unlit this frame. Rare (Begin checks the same frustum before the
			// render); latched so Begin keeps it forward for the next 300 frames (Failed covers a target that could not be made)
			Note(why);
			Latched = why; LatchedUntil = (UInt32)TheFrameRateManager->FrameNumber + 300;
			target->Release(); depth->Release();
			DeferredLamps::DeferredBindsThisSpan = 0; DeferredSun::LightFreeBindsThisSpan = DeferredSun::OlderBindsThisSpan = 0;
			return;
		}
		Note(nullptr);
		StatPasses++;
		// UNOFFICIAL 2026-11-15 the diagnostic line (the first 4 deferred frames, then every 3600th): everything a dark or wrong gun would be
		// explained by from a bench log alone
		if (DiagLines < 4 || StatPasses % 3600 == 0) {
			DiagLines++;
			const D3DXMATRIX& rp = NiDX9Renderer::GetSingleton()->projMatrix;
			Logger::Log("UNOFFICIAL DeferredFirstPerson: frame %u: eye off the world eye + offset %.3f units, forward cosine %.6f, offset (%.1f %.1f %.1f) | tan fp %.4f / %.4f, world %.4f / %.4f | "
				"depth terms %.6g %.6g, the renderer's projection now %.6g %.6g | deferred binds: lamps %u, sun %u | box %ld,%ld-%ld,%ld of %ux%u | parts %d (nearest bound %.1f units from the eye), weather %s (%.2f %.2f %.2f %.2f) | sun map %s",
				(unsigned)TheFrameRateManager->FrameNumber, EyeOff, Turn, Offset.x, Offset.y, Offset.z, Frame[0].w, Frame[1].w, fabsf(TheRenderManager->InvProjMatrix._11), fabsf(TheRenderManager->InvProjMatrix._22),
				proj[0], proj[1], rp._33, rp._43, DeferredLamps::DeferredBindsThisSpan, DeferredSun::LightFreeBindsThisSpan, box.left, box.top, box.right, box.bottom, t.Width, t.Height,
				GearWeather::PartCount, GearWeather::NearestPart, GearWeather::Live ? "live" : "off", GearWeather::Sent[0], GearWeather::Sent[1], GearWeather::Sent[2], GearWeather::Sent[3],
				DeferredSun::Vm.charData[3] > 0.0f ? "this frame's" : "none");
		}
		StatBoxPixels += (unsigned long long)(box.right - box.left) * (unsigned long long)(box.bottom - box.top);
		static GpuTimer allTimer("First person: deferred (all)");
		GpuProfileScope gpuAll(allTimer, d);
		State->Capture();
		IDirect3DSurface9* extra[3] = {};
		for (DWORD i = 1; i < 4; i++) if (SUCCEEDED(d->GetRenderTarget(i, &extra[i - 1])) && extra[i - 1]) d->SetRenderTarget(i, nullptr);
		IDirect3DTexture9* vmDepth = nullptr;
		if (DxvkCompute::Available() && TheTextureManager && TheTextureManager->DepthTextureViewModel) {
			D3DSURFACE_DESC zDesc = {};
			if (SUCCEEDED(TheTextureManager->DepthTextureViewModel->GetLevelDesc(0, &zDesc)) &&
				zDesc.Width == t.Width && zDesc.Height == t.Height) {
				TheRenderManager->ResolveDepthBuffer(TheTextureManager->DepthTextureViewModel);
				vmDepth = TheTextureManager->DepthTextureViewModel;
			}
		}
		const bool reversed = NiDX9Renderer::GetSingleton()->m_fZClear < 1.0f;
		const RECT screen = { 0, 0, (LONG)t.Width, (LONG)t.Height };
		{	// 1. the mark: bit 0x40 where the first-person depth was written (the quad just inside the clear value, the test "nearer than it")
			static GpuTimer markTimer("First person: view-model mark");
			GpuProfileScope gpu(markTimer, d);
			Quad(d, screen, t.Width, t.Height, reversed ? 1.0e-5f : 1.0f - 1.0e-5f, reversed ? D3DCMP_LESS : D3DCMP_GREATER, D3DCMP_ALWAYS, GearWeather::kMark, 0xFF,
				GearWeather::kMark, D3DSTENCILOP_REPLACE, D3DSTENCILOP_ZERO, 0, NullPS);
		}
		// 2. the weather on the gear (its own targets: the G-buffer's albedo and material)
		const bool gwApplied = GearWeather::Apply(d, Frame, Eye, box);
		d->SetRenderTarget(1, nullptr);
		// 3. the light passes into the light target, at the mark only. The target is cleared at the mark (a stencil-tested quad, not the box:
		// a marked pixel outside the parts' bounds must not keep an old frame's light)
		d->SetRenderTarget(0, LightSurf);
		Quad(d, screen, t.Width, t.Height, 0.0f, D3DCMP_ALWAYS, D3DCMP_EQUAL, GearWeather::kMark, GearWeather::kMark, 0, D3DSTENCILOP_KEEP, D3DSTENCILOP_KEEP, 0xF, NullPS);
		d->SetViewport(&vp);
		d->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_EQUAL);
		d->SetRenderState(D3DRS_STENCILREF, GearWeather::kMark);
		d->SetRenderState(D3DRS_STENCILMASK, GearWeather::kMark);
		d->SetRenderState(D3DRS_STENCILWRITEMASK, 0);
		d->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_KEEP); d->SetRenderState(D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP); d->SetRenderState(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
		d->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE, FALSE);
		DeferredSun::Vm.on = DeferredLamps::Vm.on = true;
		DeferredSun::Vm.depthTex = vmDepth;
		DeferredLamps::Vm.depthTex = vmDepth;
		// (fd-batch8 merge) SSS v2's skin highlight target (DeferredLamps::SkinSpecSurface) belongs to the world's passes, which used it already
		// (DeferredSkin::AfterPasses): off for the view model's run, so its skin pixels add nothing to it and no second target is bound
		IDirect3DSurface9* const skinSpec = DeferredLamps::SkinSpecSurface;
		DeferredLamps::SkinSpecSurface = nullptr;
		DeferredSun::Apply(d);
		DeferredLamps::Apply(d);
		// (2026-11-24 fd-complete batch 2) the flashlight's beam on the view model: the same pass, the first-person camera's rays, the light
		// moved into the view model's space (FlashlightDeferred::ViewModelRun), at the mark, into the light target
		if (FlashlightDeferred::Wanted() && TheSettingManager->GetSettingI(kSection, "DeferredFirstPersonFlashlight")) {
			FlashlightDeferred::ViewModelRun& fv = FlashlightDeferred::Vm;
			for (int k = 0; k < 3; k++) { fv.rows[k][0] = Frame[k].x; fv.rows[k][1] = Frame[k].y; fv.rows[k][2] = Frame[k].z; }
			fv.rows[0][3] = Frame[0].w; fv.rows[1][3] = Frame[1].w;	// tan(half fov x / y) of the first-person frustum
			const D3DXVECTOR4& world = TheRenderManager->CameraPosition;
			fv.shift = D3DXVECTOR3(Eye.x - Offset.x - world.x, Eye.y - Offset.y - world.y, Eye.z - Offset.z - world.z);
			fv.depthTex = vmDepth;
			fv.on = true;
			FlashlightDeferred::Apply(d);
			fv.on = false;
			fv.depthTex = nullptr;
		}
		DeferredLamps::SkinSpecSurface = skinSpec;
		DeferredSun::Vm.depthTex = nullptr;
		DeferredLamps::Vm.depthTex = nullptr;
		DeferredSun::Vm.on = DeferredLamps::Vm.on = false;
		{	// 4. the resolve over the frame (its copy over the box: a target is not read while bound)
			static GpuTimer resolveTimer("First person: deferred resolve");
			GpuProfileScope gpu(resolveTimer, d);
			d->StretchRect(target, NULL, FrameSurf, NULL, D3DTEXF_NONE);	// (the whole frame, as the world lamp pass's copy path: no pixel left to the bounds)
			d->SetRenderTarget(0, target);
			const bool gwAmb = gwApplied && GearWeather::CopyTex[0] && GearWeather::CopyTex[1]
				&& CapturedNormals::AlbedoTexture && CapturedNormals::MaterialTexture && CapturedNormals::Texture;
			d->SetTexture(0, FrameTex); d->SetTexture(1, LightTex);
			if (gwAmb) {
				d->SetTexture(2, GearWeather::CopyTex[0]);
				d->SetTexture(3, GearWeather::CopyTex[1]);
				d->SetTexture(4, CapturedNormals::AlbedoTexture);
				d->SetTexture(5, CapturedNormals::MaterialTexture);
				d->SetTexture(6, CapturedNormals::Texture);
			}
			const DWORD sampCount = gwAmb ? 7u : 2u;
			for (DWORD s = 0; s < sampCount; s++) {
				d->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT); d->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT); d->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
				d->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); d->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP); d->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
			}
			const float gamma = LinearScene::Gamma > 1.0f ? LinearScene::Gamma : 2.2f;
			const float g[4] = { gamma, 1.0f / gamma, LinearScene::Linear ? 1.0f : 0.0f, gwAmb ? 1.0f : 0.0f };
			d->SetPixelShaderConstantF(0, g, 1);
			if (gwAmb) {
				D3DXVECTOR4 sh[9] = {};
				D3DXVECTOR4 amb = { 0.0f, 0.0f, 0.0f, 0.0f };
				if (TheShaderManager && TheShaderManager->GameState.isExterior) {
					const D3DXVECTOR4& sa = TheShaderManager->ShaderConst.sunAmbient;
					const D3DXVECTOR3 flat = RtLightUnits::DecodeColor(sa.x, sa.y, sa.z, RtLightUnits::AmbientScale());
					amb = D3DXVECTOR4(flat.x, flat.y, flat.z, RtLightUnits::SkyScale());
					auto found = TheShaderManager->ConstantsTable.find("TESR_SkyIrradianceObjects");
					if (found != TheShaderManager->ConstantsTable.end() && found->second) memcpy(sh, found->second, sizeof(sh));
				}
				d->SetPixelShaderConstantF(1, &amb.x, 1);
				d->SetPixelShaderConstantF(2, &sh[0].x, 9);
				const float b[4] = { (float)box.left / t.Width, (float)box.top / t.Height, (float)box.right / t.Width, (float)box.bottom / t.Height };
				d->SetPixelShaderConstantF(11, b, 1);
			}
			Quad(d, screen, t.Width, t.Height, 0.0f, D3DCMP_ALWAYS, D3DCMP_EQUAL, GearWeather::kMark, GearWeather::kMark, 0, D3DSTENCILOP_KEEP, D3DSTENCILOP_KEEP, 0x7, ResolvePS);
		}
		// 5. our stencil bits back to 0 (0x80 is WeaponMask's)
		Quad(d, screen, t.Width, t.Height, 0.0f, D3DCMP_ALWAYS, D3DCMP_ALWAYS, 0, 0xFF, GearWeather::kMark | GearWeather::kIdMask, D3DSTENCILOP_REPLACE, D3DSTENCILOP_REPLACE, 0, NullPS);
		d->SetRenderTarget(0, target);
		for (DWORD i = 1; i < 4; i++) if (extra[i - 1]) { d->SetRenderTarget(i, extra[i - 1]); extra[i - 1]->Release(); }
		d->SetDepthStencilSurface(depth);
		d->SetViewport(&vp);
		State->Apply();
		if (PassSums::AfterStateBlock) PassSums::AfterStateBlock(d);	// (the lean hooks re-read the device's state, as WeaponMask / SkyCoverage)
		target->Release(); depth->Release();
		DeferredLamps::DeferredBindsThisSpan = 0; DeferredSun::LightFreeBindsThisSpan = DeferredSun::OlderBindsThisSpan = 0;
	}
}
