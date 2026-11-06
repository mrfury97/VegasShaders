// UNOFFICIAL 2026-10-28 (NVR DXVK) RT sun shadows: RtSunShadows.h; the trace shader is Compute\Rt\RtSunShadow.comp (run by the helper),
// the combine pass Compute\RtSunCombine.frag (a fork pass in the game).

#include "RtSunShadows.h"
#include "RtCore.h"
#include "RtDenoise.h"
#include "DxvkCompute.h"
#include "RenderRect.h"	// UNOFFICIAL 2026-11-03 RenderScale < 1: the trace over the frame's top-left rect (docs/next/RT-RENDERSCALE.md)
#include "GpuProfiler.h"

namespace {
	const char* const kSection = "Shaders.ShadowsExteriors.RT";

	struct Settings {
		bool	enabled = false;
		int		mode = 0;				// 0 combine, 1 replace
		float	strength = 1.0f;
		float	range = 6000.0f;
		float	normalBias = 2.0f;
		float	normalBiasPerUnit = 0.0005f;
		float	startBias = 1.0f;
		float	rayLength = 30000.0f;
		float	sunAngle = 0.0f;		// degrees across the sun's disc: 0 = one hard ray, > 0 = rays jittered inside it + the denoiser
		bool	denoise = true;
		bool	halfResolution = false;
		int		denoiseKernel = 5;			// the shared denoiser's a-trous kernel: 5 (5 x 5) or 3 (3 x 3, cheaper)		// trace a quarter of the pixels (2 x 2 blocks), denoise and upsample
		bool	cloudShadows = false;	// 2026-10-30: the volumetric clouds' shadow map in the traced visibility
		bool	penumbra = false;		// 2026-10-31: contact-hardening soft shadows (RtSunPenumbra.frag; SunAngle 0 = the real sun, 0.53 deg)
		float	penumbraMaxRadius = 32.0f;	// the widest penumbra filter (full-size pixels)
		bool	keepForward = false;	// 2026-11-04 Mode 0: true = the object shaders' own cascade lookups stay (the old way: darker where both shadow)
		int		debugView = 0;			// 1 RT only, 2 traced not applied, 3 hit distance, 4 the raw visibility over the picture, 5 state codes, 6 primary rays (4-6: grey over the picture)
	} S;

	// The trace pass's uniform block (RtSunShadow.comp, std140)
	struct PassConstants {
		float size[4], forward[4], right[4], up[4], origin[4], sun[4], bias[4], range[4], alpha[4], full[4];
		float cloud[4], cloudCentre[4], cloudLight[4], camera[4];	// 2026-10-30 CloudShadows (cloud.x 0: none); camera: its world position
		float prevForward[4], prevRight[4], prevUp[4], prevCamera[4];	// 2026-11-04 the blocker history: last traced frame's camera (w 1: valid)
	};

	int			imageDepth = -1, imageNormals = -1, imageVisibility = -1, shader = -1, hitDebugShader = -1;
	// 2026-10-30 CloudShadows: the clouds' shadow map in the helper (registered at the helper's start, while CloudShadows is on), and
	// whether this frame's trace carried it / its result went into the sun shadow buffer (CloudsApplied)
	int			imageCloud = -1;
	UINT		imageCloudSize = 0;
	bool		cloudsInTrace = false;
	UInt32		cloudsAppliedFrame = 0;
	IDirect3DTexture9*	visibilityTexture = nullptr;
	IDirect3DTexture9*	denoisedTexture = nullptr;		// SunAngle > 0: the denoised visibility (R16F)
	IDirect3DSurface9*	denoisedSurface = nullptr;
	IDirect3DTexture9*	signalTexture = nullptr;		// what Apply / RenderDebug use this frame (the denoised one when it ran)
	RtDenoise::Denoiser* denoiser = nullptr;
	IDirect3DSurface9*	visibilitySurface = nullptr;
	UINT		textureWidth = 0, textureHeight = 0;
	bool		tracedThisFrame = false;
	UInt32		frames = 0;
	UInt32		lastTracedFrame = 0xFFFFFFFF;	// UNOFFICIAL 2026-11-01 (StatusText)
	D3DXVECTOR4	replaceConstant = { 0.0f, 0.0f, 0.0f, 0.0f };
	// 2026-10-31 Penumbra: the traced blocker distances (helper image + its copy here) and the contact-hardening filter's output
	int			imageHit = -1;
	// UNOFFICIAL 2026-11-04 the blockers over frames (RtSunShadow.comp bindings 8, 9; ping-pong) and the camera they were traced with
	int			imageBlocker[2] = { -1, -1 };
	int			blockerParity = 0;
	UInt32		blockerFrame = 0xFFFFFFFF;		// the frame whose trace wrote imageBlocker[blockerParity]
	UINT		blockerWidth = 0, blockerHeight = 0;
	float		blockerCamera[4][4] = {};		// forward, right, up, world position of that frame
	UInt32		sunUpFrame = 0;					// 2026-11-04 the last frame the sun's shadows were not fully faded
	IDirect3DTexture9*	hitTexture = nullptr;
	IDirect3DSurface9*	hitSurface = nullptr;
	bool		hitThisFrame = false;
	IDirect3DTexture9*	penumbraTexture = nullptr;
	IDirect3DSurface9*	penumbraSurface = nullptr;
	ID3D9NvrExtPipeline* penumbraPipeline = nullptr;
	bool		penumbraTried = false;
	// UNOFFICIAL 2026-11-04 RenderScale < 1 (docs/next/RT-RENDERSCALE.md): the traced part of the traced-size textures this frame (the
	// RenderRect's top-left tw x th; the whole of them in a native frame), for Penumbra's footprint and its taps
	UINT		tracedRectW = 0, tracedRectH = 0;
	ID3D9NvrExtPipeline* combine[3] = {};	// [0] multiply, [1] write red (DebugView 1 / 3), [2] write all (DebugView 4, grey)
	bool		combineTried[3] = {};
	bool		tracedForDebug = false;		// this frame's visibility is there for RenderDebug

	void ReadSettings() {
		S.enabled = TheSettingManager->GetSettingI(kSection, "Enabled") != 0;
		S.mode = TheSettingManager->GetSettingI(kSection, "Mode");
		S.strength = TheSettingManager->GetSettingF(kSection, "Strength");
		S.range = (std::max)(500.0f, TheSettingManager->GetSettingF(kSection, "Range"));
		S.normalBias = TheSettingManager->GetSettingF(kSection, "NormalBias");
		S.normalBiasPerUnit = TheSettingManager->GetSettingF(kSection, "NormalBiasPerUnit");
		S.startBias = TheSettingManager->GetSettingF(kSection, "StartBias");
		S.rayLength = (std::max)(100.0f, TheSettingManager->GetSettingF(kSection, "RayLength"));
		S.debugView = TheSettingManager->GetSettingI(kSection, "DebugView");
		S.sunAngle = (std::min)((std::max)(TheSettingManager->GetSettingF(kSection, "SunAngle"), 0.0f), 10.0f);
		S.denoise = TheSettingManager->GetSettingI(kSection, "Denoise") != 0;
		S.halfResolution = TheSettingManager->GetSettingI(kSection, "HalfResolution") != 0;
		S.denoiseKernel = TheSettingManager->GetSettingI(kSection, "DenoiseKernel") == 3 ? 3 : 5;
		S.cloudShadows = TheSettingManager->GetSettingI(kSection, "CloudShadows") != 0;
		S.penumbra = TheSettingManager->GetSettingI(kSection, "Penumbra") != 0;
		S.penumbraMaxRadius = (std::min)((std::max)(TheSettingManager->GetSettingF(kSection, "PenumbraMaxRadius"), 2.0f), 64.0f);
		if (S.penumbra && S.sunAngle <= 0.0f) S.sunAngle = 0.53f;	// the sun's real angular diameter
		S.keepForward = TheSettingManager->GetSettingI(kSection, "KeepForward") != 0;
	}

	const D3DXVECTOR4* Constant(const char* name) {
		auto found = TheShaderManager->ConstantsTable.find(name);
		return found == TheShaderManager->ConstantsTable.end() ? nullptr : found->second;
	}
	IDirect3DBaseTexture9* Texture(const char* name) {
		auto found = TheTextureManager->TextureNames.find(name);
		return found == TheTextureManager->TextureNames.end() || !found->second ? nullptr : *found->second;
	}

	bool HitView() { return S.debugView == 7 || S.debugView == 8; }	// RtHitDebug.comp: camera rays, no sun

	bool Wants() {
		ReadSettings();
		ShadowsExteriorEffect* shadows = TheShaderManager->Effects.ShadowsExteriors;
		if (S.enabled && HitView()) return true;	// 2026-10-30: the hit texture / glow checks run indoors too (the neon is in casinos)
		if (!(S.enabled && TheShaderManager->GameState.isExterior && shadows && shadows->Enabled && shadows->Settings.Exteriors.Enabled)) return false;
		// UNOFFICIAL 2026-11-04: not while the sun's shadows are fully faded (night, the turn of dusk and dawn: every pixel came out free)
		const UInt32 now = TheFrameRateManager ? TheFrameRateManager->FrameNumber : 0;
		if (shadows->Constants.ShadowFade.x < 1.0f) sunUpFrame = now;
		return now - sunUpFrame < 300 || S.debugView != 0;
	}

	void Setup() {
		const UINT w = TheRenderManager->width, h = TheRenderManager->height;
		imageDepth = RtCore::AddImage("NVR RT sun depth", w, h, NVR_EXT_SHARED_R32F);
		imageNormals = RtCore::AddImage("NVR RT sun normals", w, h, NVR_EXT_SHARED_RGBA16F);
		imageVisibility = RtCore::AddImage("NVR RT sun visibility", w, h, NVR_EXT_SHARED_R16F);
		imageHit = RtCore::AddImage("NVR RT sun hit distance", w, h, NVR_EXT_SHARED_R16F);	// 2026-10-31 Penumbra (always: the switch is live)
		imageBlocker[0] = RtCore::AddImage("NVR RT sun blockers 0", w, h, NVR_EXT_SHARED_RGBA16F);	// 2026-11-04 the blocker history
		imageBlocker[1] = RtCore::AddImage("NVR RT sun blockers 1", w, h, NVR_EXT_SHARED_RGBA16F);
		blockerFrame = 0xFFFFFFFF;
		// 2026-10-30 CloudShadows: the clouds' shadow map (A16B16G16R16F, ShadowResolution x ShadowResolution), if it exists by now
		imageCloud = -1;
		imageCloudSize = 0;
		VolumetricCloudsEffect* clouds = TheShaderManager->Effects.VolumetricClouds;
		D3DSURFACE_DESC desc = {};
		if (S.cloudShadows && clouds && clouds->Textures.ShadowTexture && SUCCEEDED(clouds->Textures.ShadowTexture->GetLevelDesc(0, &desc)) &&
			desc.Format == D3DFMT_A16B16G16R16F && desc.Width == desc.Height) {
			imageCloud = RtCore::AddImage("NVR RT sun cloud shadow", desc.Width, desc.Height, NVR_EXT_SHARED_RGBA16F);
			imageCloudSize = desc.Width;
		}
		else if (S.cloudShadows) Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RT sun CloudShadows: no volumetric cloud shadow map at the helper's start (Volumetric Clouds and their Shadows on?); off until the next start.");
	}

	void Record(IDirect3DDevice9* Device) {
		tracedThisFrame = false;
		if (imageDepth < 0 || imageNormals < 0 || imageVisibility < 0 || shader < 0) return;
		const UINT fw = TheRenderManager->width, fh = TheRenderManager->height;
		const UINT scale = S.halfResolution ? 2u : 1u;
		const UINT w = (std::max)(1u, fw / scale), h = (std::max)(1u, fh / scale);	// as RtDenoise's Downscale	// the traced size (the images stay full size: the top-left part)
		if (!visibilityTexture || textureWidth != w || textureHeight != h) {
			if (visibilitySurface) { visibilitySurface->Release(); visibilitySurface = nullptr; }
			if (visibilityTexture) { visibilityTexture->Release(); visibilityTexture = nullptr; }
			TheTextureManager->InitTexture("TESR_RtSunShadow", &visibilityTexture, &visibilitySurface, w, h, D3DFMT_R16F);
			textureWidth = w;
			textureHeight = h;
			if (!visibilityTexture) return;
			if (hitSurface) { hitSurface->Release(); hitSurface = nullptr; }
			if (hitTexture) { hitTexture->Release(); hitTexture = nullptr; }
		}
		hitThisFrame = false;
		const bool wantHit = S.penumbra && imageHit >= 0 && imageBlocker[0] >= 0 && imageBlocker[1] >= 0 && S.debugView != 7 && S.debugView != 8;
		if (wantHit && !hitTexture) TheTextureManager->InitTexture("TESR_RtSunBlocker", &hitTexture, &hitSurface, w, h, D3DFMT_R16F);
		IDirect3DBaseTexture9* depth = Texture("TESR_DepthBuffer");
		IDirect3DBaseTexture9* normals = Texture("TESR_NormalsBuffer");
		const D3DXVECTOR4* cameraData = Constant("TESR_CameraData");
		ShadowsExteriorEffect* shadows = TheShaderManager->Effects.ShadowsExteriors;
		if (!depth || !normals || !cameraData || !shadows) return;

		const D3DXMATRIX& view = TheRenderManager->viewMatrix;
		const D3DXMATRIX& proj = TheRenderManager->projMatrix;
		// (2026-11-03) RenderScale < 1 (FSR / DLSS with LowResEffects): the frame is the top-left RenderRect part of the screen-size
		// depth, normals and shadow buffer. The trace covers only that part: the shader's size / full are the rect's (its uv = pixel /
		// rect size is then the screen's, the depth is read in the rect), the result goes to the same top-left part of the full-size
		// visibility texture, which the combine copies texel for texel into the rect-layout shadow buffer. (Before: the full grid's
		// uv read rect pixels as screen points: wrong positions, round 20's "RT barely changes the picture at RenderScale 0.67".)
		// Off: the rect is the whole screen, exactly as before. (SunAngle's denoiser: RtDenoise, rect-aware 2026-11-03; Penumbra:
		// RunPenumbra, 2026-11-04.)
		const UINT rw = RenderRect::Width(fw), rh = RenderRect::Height(fh);
		static int stateLogs = 0;
		if (stateLogs < 3 && (frames % 120) == 5) {
			stateLogs++;
			Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RT sun shadows Record: RenderRect %s (scale %.2f x %.2f, in use %d), traced grid %ux%u of %ux%u.", RenderRect::On ? "ON" : "off",
				RenderRect::ScaleX, RenderRect::ScaleY, RenderRect::InUse(TheFrameRateManager->FrameNumber) ? 1 : 0, (std::max)(1u, rw / scale), (std::max)(1u, rh / scale), w, h);
		}
		const UINT tw = (std::max)(1u, rw / scale), th = (std::max)(1u, rh / scale);
		tracedRectW = tw; tracedRectH = th;	// (2026-11-04) Penumbra's
		PassConstants c = {};
		c.size[0] = (float)tw; c.size[1] = (float)th; c.size[2] = 1.0f / tw; c.size[3] = 1.0f / th;
		c.full[0] = (float)rw; c.full[1] = (float)rh;	// depth / normals are read at full size (in the rect)
		if (TAAEffect* taa = TheShaderManager->Effects.TAA) { const D3DXVECTOR2 j = taa->DepthJitterUV(); c.full[2] = j.x; c.full[3] = j.y; }	// (2026-11-03)
		// SunShadows.glsl toWorld(): the view matrix's columns are right (0), up (1) and forward (2)
		c.forward[0] = view._13; c.forward[1] = view._23; c.forward[2] = view._33; c.forward[3] = cameraData->y;
		c.right[0] = view._11 / proj._11; c.right[1] = view._21 / proj._11; c.right[2] = view._31 / proj._11;
		c.up[0] = view._12 / proj._22; c.up[1] = view._22 / proj._22; c.up[2] = view._32 / proj._22;
		const D3DXVECTOR3 eye = RtCore::Eye();	// the scene is relative to the eye (not Pointers::Generic::CameraLocation)
		c.origin[0] = eye.x; c.origin[1] = eye.y; c.origin[2] = eye.z;
		const D3DXVECTOR4& sun = shadows->Constants.SmoothedSunDir;	// toward the sun, as the cascades use it
		c.sun[0] = sun.x; c.sun[1] = sun.y; c.sun[2] = sun.z;
		c.sun[3] = shadows->Constants.ShadowFade.x < 1.0f ? 1.0f : 0.0f;
		c.bias[0] = S.normalBias; c.bias[1] = S.normalBiasPerUnit; c.bias[2] = S.startBias; c.bias[3] = S.rayLength;
		c.alpha[0] = RtCore::AlphaTested() ? 1.0f : 0.0f;
		c.alpha[1] = (float)(frames & 1023);								// the jitter's frame
		c.alpha[2] = tanf(0.5f * S.sunAngle * 0.0174532925f);				// the sun cone's half-angle tangent (0: hard)
		c.alpha[3] = wantHit && hitSurface ? 1.0f : 0.0f;					// 2026-10-31 Penumbra: write the blocker distances (binding 7)
		c.range[0] = S.range; c.range[1] = 0.075f * S.range;	// 2026-11-04 the last 7.5% (Mode 1's cascades come in over the 7.5% before)
		 c.range[2] = 255.0f; c.range[3] = S.debugView == 3 ? 1.0f : S.debugView == 5 ? 2.0f : S.debugView == 6 ? 3.0f :
			S.debugView == 9 ? 5.0f : S.debugView == 10 ? 6.0f : 0.0f;	// 9, 10 (2026-10-30): CloudShadows checks

		if (!RtCore::CopyIn(imageDepth, depth) || !RtCore::CopyIn(imageNormals, normals)) return;
		// 2026-10-30 CloudShadows: the clouds' shadow map as last drawn (it is drawn later in the frame: last frame's, with its own centre)
		cloudsInTrace = false;
		VolumetricCloudsEffect* clouds = TheShaderManager->Effects.VolumetricClouds;
		static int cloudLogs = 0;
		if (S.cloudShadows && cloudLogs < 3 && (frames % 300) == 30) {
			cloudLogs++;
			Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RT sun CloudShadows: image %d (%u texels), map valid %d size %u range %.0f m base %.0f m centre %.0f %.0f m light %.3f %.3f %.3f, "
				"strength %.3f, camera %.0f %.0f %.0f m", imageCloud, imageCloudSize, clouds && clouds->rtMapValid ? 1 : 0, clouds ? clouds->shadowMapSize : 0u,
				clouds ? clouds->rtMapRange : 0.0f, clouds ? clouds->rtMapBase : 0.0f, clouds ? clouds->rtMapCentre[0] : 0.0f, clouds ? clouds->rtMapCentre[1] : 0.0f,
				clouds ? clouds->rtMapLight[0] : 0.0f, clouds ? clouds->rtMapLight[1] : 0.0f, clouds ? clouds->rtMapLight[2] : 0.0f,
				clouds ? clouds->ShadowStrengthForSunFar() : 0.0f, TheRenderManager->CameraPosition.x / 70.0f, TheRenderManager->CameraPosition.y / 70.0f,
				TheRenderManager->CameraPosition.z / 70.0f);
		}
		if (S.cloudShadows && imageCloud >= 0 && clouds && clouds->rtMapValid && clouds->Textures.ShadowTexture && TheShaderManager->GameState.isExterior &&
			clouds->shadowMapSize == imageCloudSize && S.debugView != 7 && S.debugView != 8) {
			const float strength = clouds->ShadowStrengthForSunFar();
			if (strength > 0.0f && RtCore::CopyIn(imageCloud, clouds->Textures.ShadowTexture)) {
				c.cloud[0] = strength; c.cloud[1] = clouds->rtMapRange; c.cloud[2] = (float)clouds->shadowMapSize; c.cloud[3] = clouds->rtMapBase;
				c.cloudCentre[0] = clouds->rtMapCentre[0]; c.cloudCentre[1] = clouds->rtMapCentre[1]; c.cloudCentre[2] = 1.0f / 70.0f;
				c.cloudLight[0] = clouds->rtMapLight[0]; c.cloudLight[1] = clouds->rtMapLight[1]; c.cloudLight[2] = clouds->rtMapLight[2];
				cloudsInTrace = true;
			}
		}
		// the camera's world position (the scene is camera relative): the clouds' map is in world metres
		c.camera[0] = TheRenderManager->CameraPosition.x; c.camera[1] = TheRenderManager->CameraPosition.y; c.camera[2] = TheRenderManager->CameraPosition.z;
		const int atlas = RtCore::AlphaAtlasImage();
		const int albedoAtlas = RtCore::AlbedoAtlasImage();
		const int glowAtlas = RtCore::GlowAtlasImage();
		if ((S.debugView == 7 || (S.debugView == 8 && glowAtlas >= 0)) && albedoAtlas >= 0 && atlas >= 0 && hitDebugShader >= 0) {	// the hit textures check (RtHitDebug.comp)
			if (S.debugView == 8) c.range[3] = 4.0f;	// 2026-10-30: the glow atlas (GlowTextures)
			const int images[4] = { imageVisibility, atlas, albedoAtlas, glowAtlas >= 0 ? glowAtlas : albedoAtlas };
			if (!RtCore::AddPass(hitDebugShader, images, 4, 0x1, &c, sizeof(c), (tw + 7) / 8, (th + 7) / 8)) return;
		}
		else {
			// 2026-11-04 the blocker history: last frame's camera when last frame's trace wrote the pair's other half at this size
			const UInt32 now = TheFrameRateManager ? TheFrameRateManager->FrameNumber : 0;
			const bool hit = c.alpha[3] > 0.0f;
			if (hit && blockerFrame + 1 == now && blockerWidth == tw && blockerHeight == th) {
				memcpy(c.prevForward, blockerCamera[0], sizeof(c.prevForward));
				memcpy(c.prevRight, blockerCamera[1], sizeof(c.prevRight));
				memcpy(c.prevUp, blockerCamera[2], sizeof(c.prevUp));
				memcpy(c.prevCamera, blockerCamera[3], sizeof(c.prevCamera));
				c.prevCamera[3] = 1.0f;
			}
			const int images[8] = { imageDepth, imageNormals, imageVisibility, c.alpha[0] > 0.0f && atlas >= 0 ? atlas : imageVisibility,	// [3]: binding 5
				cloudsInTrace ? imageCloud : imageNormals,	// [4]: binding 6, the clouds' shadow map (a stand-in with cloud.x 0)
				hit ? imageHit : imageVisibility,	// [5]: binding 7, the blocker distances (a stand-in, not written, with alpha.w 0)
				hit ? imageBlocker[blockerParity] : imageNormals, hit ? imageBlocker[1 - blockerParity] : imageNormals };	// [6], [7]: bindings 8, 9 (not used with alpha.w 0)
			if (!RtCore::AddPass(shader, images, 8, hit ? 0xA4 : 0x4, &c, sizeof(c), (tw + 7) / 8, (th + 7) / 8)) { cloudsInTrace = false; return; }
			if (hit) {
				blockerParity = 1 - blockerParity;	// the half just written is the next frame's input
				blockerFrame = now;
				blockerWidth = tw; blockerHeight = th;
				memcpy(blockerCamera[0], c.forward, sizeof(c.forward));
				memcpy(blockerCamera[1], c.right, sizeof(c.right));
				memcpy(blockerCamera[2], c.up, sizeof(c.up));
				memcpy(blockerCamera[3], c.camera, sizeof(c.camera));
			}
		}
		tracedThisFrame = RtCore::CopyOut(imageVisibility, visibilitySurface, tw, th);
		static int rectLogs = 0;
		if (tracedThisFrame && RenderRect::On && rectLogs < 2) {
			rectLogs++;
			Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RT sun shadows traced in the RenderScale rect: %ux%u of %ux%u (scale %.2f x %.2f).", tw, th, w, h, RenderRect::ScaleX, RenderRect::ScaleY);
		}
		if (tracedThisFrame) lastTracedFrame = TheFrameRateManager ? TheFrameRateManager->FrameNumber : 0;	// UNOFFICIAL 2026-11-01
		if (tracedThisFrame && c.alpha[3] > 0.0f) hitThisFrame = RtCore::CopyOut(imageHit, hitSurface, tw, th);
		if (tracedThisFrame && frames++ == 0)
			Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RT sun shadows traced (%ux%u, Mode %d: %s, range %.0f, DebugView %d).", w, h, S.mode,
				S.mode == 1 ? "replace the cascades inside the range" : "combine with the cascades", S.range, S.debugView);
		if (tracedThisFrame && frames == 31)
			Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RT sun constants: forward %.3f %.3f %.3f (farZ %.0f), right %.3f %.3f %.3f, up %.3f %.3f %.3f, origin %.2f %.2f %.2f, "
				"sun %.3f %.3f %.3f (up %.0f), camera position %.0f %.0f %.0f", c.forward[0], c.forward[1], c.forward[2], c.forward[3], c.right[0], c.right[1],
				c.right[2], c.up[0], c.up[1], c.up[2], c.origin[0], c.origin[1], c.origin[2], c.sun[0], c.sun[1], c.sun[2], c.sun[3],
				TheRenderManager->CameraPosition.x, TheRenderManager->CameraPosition.y, TheRenderManager->CameraPosition.z);
	}

	// 2026-10-31 Penumbra: RtSunPenumbra.frag (4 sampled images + the sampler; push 64 bytes, the sampler index in the last 16)
	ID3D9NvrExtPipeline* PenumbraPipeline() {
		if (penumbraTried) return penumbraPipeline;
		penumbraTried = true;
		std::vector<char> spirv;
		ID3D9NvrExt* ext = DxvkCompute::Extension();
		if (!ext || !DxvkCompute::LoadSpirv("RtSunPenumbra", spirv)) return nullptr;
		NVR_EXT_BINDING_DESC bindings[5] = {};
		for (UINT i = 0; i < 4; i++) bindings[i] = { 1, i, NVR_EXT_BIND_SAMPLED_IMAGE, i, NVR_EXT_VIEW_2D, FALSE, 0 };
		bindings[4] = { 0, 0, NVR_EXT_BIND_SAMPLER, 0, NVR_EXT_VIEW_2D, FALSE, 48 };
		NVR_EXT_GRAPHICS_PIPELINE_DESC desc = {};
		desc.pFragmentSpirv = spirv.data();
		desc.FragmentSpirvSize = (UINT)spirv.size();
		desc.BindingCount = 5;
		desc.pBindings = bindings;
		desc.PushConstantSize = 64;
		desc.UsesSamplerHeap = TRUE;
		desc.SamplerHeapSet = 0;
		desc.SamplerHeapBinding = 0;
		desc.ColorTargetCount = 1;
		desc.Blend = NVR_EXT_BLEND_NONE;
		desc.ColorWriteMask = 0xFu;
		desc.pDebugName = "RtSunPenumbra";
		if (FAILED(ext->CreateGraphicsPipeline(&desc, &penumbraPipeline))) {
			penumbraPipeline = nullptr;
			Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RT sun Penumbra: the pass could not be made (Compute\\RtSunPenumbra.spv; the DXVK log says why)");
		}
		return penumbraPipeline;
	}

	// The contact-hardening filter over the temporally accumulated visibility (Source) into penumbraTexture; false: not run
	bool RunPenumbra(IDirect3DDevice9* Device, IDirect3DTexture9* Source) {
		IDirect3DBaseTexture9* depth = Texture("TESR_DepthBuffer");
		IDirect3DBaseTexture9* normals = Texture("TESR_NormalsBuffer");
		const D3DXVECTOR4* cameraData = Constant("TESR_CameraData");
		ID3D9NvrExtPipeline* pipeline = PenumbraPipeline();
		static int told = 0;
		if (!pipeline || !depth || !normals || !cameraData || !hitThisFrame || !hitTexture || !DxvkCompute::PointClamp()) {
			if (told < 3) {
				told++;
				Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RT sun Penumbra not run: pipeline %d depth %d normals %d camera %d blockers this frame %d blocker texture %d sampler %d",
					pipeline ? 1 : 0, depth ? 1 : 0, normals ? 1 : 0, cameraData ? 1 : 0, hitThisFrame ? 1 : 0, hitTexture ? 1 : 0, DxvkCompute::PointClamp() ? 1 : 0);
			}
			return false;
		}
		const UINT w = textureWidth, h = textureHeight;
		static UINT pw = 0, ph = 0;
		if (!penumbraTexture || pw != w || ph != h) {
			pw = w; ph = h;
			if (penumbraSurface) { penumbraSurface->Release(); penumbraSurface = nullptr; }
			if (penumbraTexture) { penumbraTexture->Release(); penumbraTexture = nullptr; }
			Device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R16F, D3DPOOL_DEFAULT, &penumbraTexture, nullptr);
			if (penumbraTexture) penumbraTexture->GetSurfaceLevel(0, &penumbraSurface);
		}
		if (!penumbraSurface) return false;
		static GpuTimer timer("  RT sun penumbra");
		GpuProfileScope gpu(timer, Device);
		// UNOFFICIAL 2026-11-04 RenderScale < 1 (docs/next/RT-RENDERSCALE.md): in a rect frame the trace fills the top-left tw x th part of
		// the w x h textures. The footprint: a traced pixel's world size is the view height over th pixels (not h: the radius came out
		// 1 / scale too wide), and the largest radius (full-size pixels of the output) is that many x the scale in traced pixels, so the
		// penumbra covers the same part of the upscaled screen as at RenderScale 1. The taps stay inside the rect (extra.yz the last
		// texel centre; past it hold older frames' values) and texels past it write 0 (extra.w 1). Native: th = h, scale 1, no clamp.
		const UINT tw = (std::min)((std::max)(tracedRectW, 1u), w), th = (std::min)((std::max)(tracedRectH, 1u), h);
		const bool rect = tw < w || th < h;
		const float rectScale = rect ? (float)th / (float)h : 1.0f;
		struct { float params[4], size[4], extra[4]; UInt32 samplerIndex, pad[3]; } push = {};
		push.params[0] = tanf(0.5f * S.sunAngle * 0.0174532925f);
		push.params[1] = cameraData->y;
		push.params[2] = S.penumbraMaxRadius / (S.halfResolution ? 2.0f : 1.0f);	// the setting is in full-size pixels
		if (rect) push.params[2] *= rectScale;
		push.params[3] = S.debugView == 11 ? 1.0f : S.debugView == 12 ? 2.0f : 0.0f;	// 12: a flat 0.25 (the pass' output reaches the screen)
		push.size[0] = 1.0f / w; push.size[1] = 1.0f / h; push.size[2] = (float)th; push.size[3] = TheRenderManager->projMatrix._22;
		push.extra[0] = 0.75f;
		if (rect) { push.extra[1] = ((float)tw - 0.5f) / (float)w; push.extra[2] = ((float)th - 0.5f) / (float)h; push.extra[3] = 1.0f; }
		static int rectLogs = 0;
		if (rect && rectLogs < 2) {
			rectLogs++;
			Logger::Log("UNOFFICIAL NVR DXVK: RT sun Penumbra in the RenderScale rect: %ux%u of %ux%u, max radius %.1f traced px (scale %.3f).", tw, th, w, h, push.params[2], rectScale);
		}
		NVR_EXT_RESOURCE_BINDING bindings[5] = {};
		IUnknown* images[4] = { Source, hitTexture, depth, normals };
		for (UINT i = 0; i < 4; i++) { bindings[i].Slot = i; bindings[i].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[i].pResource = images[i]; }
		bindings[4].Slot = 0; bindings[4].Type = NVR_EXT_BIND_SAMPLER; bindings[4].pResource = DxvkCompute::PointClamp();
		NVR_EXT_DRAW_DESC draw = {};
		draw.pPipeline = pipeline;
		draw.TargetCount = 1;
		draw.Targets[0].pResource = penumbraSurface;
		draw.BindingCount = 5;
		draw.pBindings = bindings;
		draw.pPushData = &push;
		draw.PushDataSize = sizeof(push);
		const HRESULT hr = DxvkCompute::Extension()->Draw(&draw);
		if (told < 6) {
			told++;
			Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RT sun Penumbra: pass %ux%u, tan %.5f, farZ %.0f, max radius %.1f px, debug %.0f: %s (0x%08X)", w, h, push.params[0],
				push.params[1], push.params[2], push.params[3], SUCCEEDED(hr) ? "drawn" : "Draw FAILED", (UInt32)hr);
		}
		return SUCCEEDED(hr);
	}

	ID3D9NvrExtPipeline* CombinePipeline(int which) {
		if (combineTried[which]) return combine[which];
		combineTried[which] = true;
		std::vector<char> spirv;
		ID3D9NvrExt* ext = DxvkCompute::Extension();
		if (!ext || !DxvkCompute::LoadSpirv("RtSunCombine", spirv)) return nullptr;
		NVR_EXT_BINDING_DESC bindings[2] = {
			{ 1, 0, NVR_EXT_BIND_SAMPLED_IMAGE, 0, NVR_EXT_VIEW_2D, FALSE, 0 },
			{ 0, 0, NVR_EXT_BIND_SAMPLER, 0, NVR_EXT_VIEW_2D, FALSE, 4 },
		};
		NVR_EXT_GRAPHICS_PIPELINE_DESC desc = {};
		desc.pFragmentSpirv = spirv.data();
		desc.FragmentSpirvSize = (UINT)spirv.size();
		desc.BindingCount = 2;
		desc.pBindings = bindings;
		desc.PushConstantSize = 16;
		desc.UsesSamplerHeap = TRUE;
		desc.SamplerHeapSet = 0;
		desc.SamplerHeapBinding = 0;
		desc.ColorTargetCount = 1;
		desc.Blend = which == 0 ? NVR_EXT_BLEND_MULTIPLY : NVR_EXT_BLEND_NONE;
		desc.ColorWriteMask = which == 2 ? 0u : 0x1u;	// red only: the sun's shadow; green (the lamps) untouched. DebugView 4: all
		desc.pDebugName = which == 0 ? "RtSunCombine (multiply)" : which == 1 ? "RtSunCombine (write)" : "RtSunCombine (debug grey)";
		if (FAILED(ext->CreateGraphicsPipeline(&desc, &combine[which]))) {
			combine[which] = nullptr;
			Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RT sun shadows: the combine pass could not be made (the DXVK log says why)");
		}
		return combine[which];
	}
}

namespace RtSunShadows {
	void Register() {
		shader = RtCore::AddShader("RtSunShadow");
		hitDebugShader = RtCore::AddShader("RtHitDebug");
		RtCore::AddUser({ "RT sun shadows", Wants, Setup, Record });
	}

	void Apply(IDirect3DDevice9* Device) {
		const bool traced = tracedThisFrame;
		tracedThisFrame = false;
		tracedForDebug = traced;
		if (S.enabled) {	// UNOFFICIAL 2026-11-01 (core/EffectStatus.h)
			static int statusSlot = -1;
			EffectStatus::Note(statusSlot, "RT sun shadows", traced ? EffectStatus::Drawn : EffectStatus::Skipped, traced ? "traced" : "not traced this frame (helper not ready / no images)");
		}
		signalTexture = visibilityTexture;
		// Soft shadows: the jittered one-ray visibility through the shared denoiser (RtDenoise.h)
		if (traced && visibilityTexture && S.sunAngle > 0.0f && S.denoise) {
			const UINT w = textureWidth, h = textureHeight;	// the traced size (half with HalfResolution)
			static UINT denoisedWidth = 0, denoisedHeight = 0;
			if (!denoisedTexture || denoisedWidth != w || denoisedHeight != h) {
				denoisedWidth = w; denoisedHeight = h;
				if (denoisedSurface) { denoisedSurface->Release(); denoisedSurface = nullptr; }
				if (denoisedTexture) { denoisedTexture->Release(); denoisedTexture = nullptr; }
				Device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R16F, D3DPOOL_DEFAULT, &denoisedTexture, nullptr);
				if (denoisedTexture) denoisedTexture->GetSurfaceLevel(0, &denoisedSurface);
			}
			if (!denoiser) denoiser = RtDenoise::Create("RT sun shadows");
			RtDenoise::Settings settings;
			settings.Downscale = S.halfResolution ? 2 : 1;
			settings.Kernel = S.denoiseKernel;
			// UNOFFICIAL 2026-11-04: the history clamped to this frame's rays (a moved shadow does not trail); its own denoiser under DLSS
			// Ray Reconstruction too (else the raw one-ray signal went on and the Penumbra pass never ran); a sun on the move (its
			// glide between steps) keeps 4 frames, a jump or a gap in the tracing restarts the history
			settings.ClampHistory = true;
			settings.IgnoreRr = true;
			{
				static D3DXVECTOR4 lastSun = { 0.0f, 0.0f, 0.0f, 0.0f };
				static UInt32 lastFrame = 0;
				const D3DXVECTOR4& sun = TheShaderManager->Effects.ShadowsExteriors->Constants.SmoothedSunDir;
				const UInt32 now = TheFrameRateManager ? TheFrameRateManager->FrameNumber : 0;
				const float turn = sun.x * lastSun.x + sun.y * lastSun.y + sun.z * lastSun.z;
				if (now - lastFrame > 2 || turn < 0.99939f) settings.Reset = true;			// over 2 degrees in a frame, or not traced lately
				else if (turn < 0.9999999f) settings.MaxHistory = 4.0f;					// moving
				lastSun = sun;
				lastFrame = now;
			}
			const bool penumbra = S.penumbra && hitThisFrame;
			if (penumbra) settings.Iterations = 0;	// 2026-10-31 Penumbra: temporal only, the spatial part is RtSunPenumbra's (sized per pixel)
			if (denoisedSurface && RtDenoise::Run(denoiser, Device, visibilityTexture, denoisedSurface, settings)) {
				signalTexture = denoisedTexture;
				if (penumbra && RunPenumbra(Device, denoisedTexture)) signalTexture = penumbraTexture;
			}
		}
		if (!traced || S.debugView == 2 || S.debugView >= 4 || !signalTexture) return;
		ShadowsExteriorEffect* shadows = TheShaderManager->Effects.ShadowsExteriors;
		IDirect3DSurface9* target = shadows->Textures.ShadowPassSurface;
		ID3D9NvrExtPipeline* pipeline = CombinePipeline(S.debugView == 1 || S.debugView == 3 ? 1 : 0);
		ID3D9NvrExtSampler* point = S.halfResolution ? DxvkCompute::LinearClamp() : DxvkCompute::PointClamp();	// half: bilinear up
		if (!pipeline || !point || !target) return;

		static GpuTimer timer("  RT sun shadows combine");
		GpuProfileScope gpu(timer, Device);
		// A pass may not draw into the device's current render target: park the device on another surface meanwhile
		IDirect3DSurface9* current = nullptr;
		Device->GetRenderTarget(0, &current);
		SunShadowsEffect* sunShadows = TheShaderManager->Effects.SunShadows;
		const bool park = current == target && sunShadows && sunShadows->scratchSurface[0];
		if (park) Device->SetRenderTarget(0, sunShadows->scratchSurface[0]);

		struct { float strength; UInt32 samplerIndex, grey, pad; } push = { S.debugView ? 1.0f : (std::min)((std::max)(S.strength, 0.0f), 1.0f), 0, 0, 0 };
		NVR_EXT_RESOURCE_BINDING bindings[2] = {};
		bindings[0].Slot = 0; bindings[0].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[0].pResource = signalTexture;
		bindings[1].Slot = 0; bindings[1].Type = NVR_EXT_BIND_SAMPLER; bindings[1].pResource = point;
		NVR_EXT_DRAW_DESC draw = {};
		draw.pPipeline = pipeline;
		draw.TargetCount = 1;
		draw.Targets[0].pResource = target;
		draw.BindingCount = 2;
		draw.pBindings = bindings;
		draw.pPushData = &push;
		draw.PushDataSize = sizeof(push);
		const bool drawn = SUCCEEDED(DxvkCompute::Extension()->Draw(&draw));
		if (drawn && cloudsInTrace && S.debugView == 0) cloudsAppliedFrame = TheFrameRateManager->FrameNumber + 1;	// 2026-10-30 CloudShadows

		if (park) Device->SetRenderTarget(0, current);
		if (current) current->Release();
	}

	void RenderDebug(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget) {
		const bool hitIndoors = HitView() && tracedThisFrame && !TheShaderManager->GameState.isExterior;	// no Apply indoors
		if (hitIndoors) tracedThisFrame = false;
		const bool traced = tracedForDebug || hitIndoors;
		tracedForDebug = false;
		if (!traced || S.debugView < 4 || (!signalTexture && !hitIndoors) || !RenderTarget) return;
		IDirect3DTexture9* backbone = S.debugView == 13 || S.debugView == 14 ? RtDenoise::Backbone() : nullptr;	// 2026-10-31 DenoiseBackbone
		if ((S.debugView == 13 || S.debugView == 14) && !backbone) return;
		ID3D9NvrExtPipeline* pipeline = CombinePipeline(2);
		ID3D9NvrExtSampler* point = S.halfResolution ? DxvkCompute::LinearClamp() : DxvkCompute::PointClamp();	// half: bilinear up
		SunShadowsEffect* sunShadows = TheShaderManager->Effects.SunShadows;
		if (!pipeline || !point || !sunShadows || !sunShadows->scratchSurface[0]) return;
		IDirect3DSurface9* current = nullptr;
		Device->GetRenderTarget(0, &current);
		Device->SetRenderTarget(0, sunShadows->scratchSurface[0]);	// a pass may not draw into the device's current target
		struct { float strength; UInt32 samplerIndex, grey, channel; } push = { 1.0f, 0, 1, S.debugView == 13 ? 1u : S.debugView == 14 ? 2u : 0u };
		NVR_EXT_RESOURCE_BINDING bindings[2] = {};
		bindings[0].Slot = 0; bindings[0].Type = NVR_EXT_BIND_SAMPLED_IMAGE;
		bindings[0].pResource = backbone ? (IUnknown*)backbone : S.debugView == 4 || S.debugView >= 11 ? (IUnknown*)signalTexture : (IUnknown*)visibilityTexture;
		bindings[1].Slot = 0; bindings[1].Type = NVR_EXT_BIND_SAMPLER; bindings[1].pResource = point;
		NVR_EXT_DRAW_DESC draw = {};
		draw.pPipeline = pipeline;
		draw.TargetCount = 1;
		draw.Targets[0].pResource = RenderTarget;
		draw.BindingCount = 2;
		draw.pBindings = bindings;
		draw.pPushData = &push;
		draw.PushDataSize = sizeof(push);
		DxvkCompute::Extension()->Draw(&draw);
		Device->SetRenderTarget(0, current);
		if (current) current->Release();
	}

	bool CloudsApplied() { return cloudsAppliedFrame == TheFrameRateManager->FrameNumber + 1; }

	bool Replacing() { return S.enabled && S.mode == 1 && S.debugView != 2 && S.debugView < 4 && RtCore::Running() && TheShaderManager->GameState.isExterior; }

	// UNOFFICIAL 2026-11-04: the cascades in the deferred apply (the object shaders' own lookups off) whenever the traced shadow is applied:
	// Mode 1, and Mode 0 unless KeepForward. With the forward lookups the objects had already lost the sun in a cascade shadow and the
	// traced shadow, multiplied into the whole picture, darkened them a second time; now both go through the one buffer.
	bool Deferred() {
		return S.enabled && (S.mode == 1 || !S.keepForward) && S.debugView != 2 && S.debugView < 4 && RtCore::Running() && TheShaderManager->GameState.isExterior;
	}

	// UNOFFICIAL 2026-11-01 (the REPORT block's ray tracing line)
	std::string StatusText() {
		if (!S.enabled) return "RT sun shadows off ([Shaders.ShadowsExteriors.RT] Enabled false): the cascades alone";
		const unsigned frame = TheFrameRateManager ? TheFrameRateManager->FrameNumber : 0;
		char t[400];
		sprintf_s(t, "RT sun shadows on, Mode %d (%s)%s | %s | traced %ux%u, frames traced %u, last traced frame f%u%s | Range %.0f, SunAngle %.2f, Denoise %s, HalfResolution %s, "
			"DebugView %d (1 traced alone, 2 traced not applied, 4 traced visibility as grey: white = the sun reaches the pixel, black = blocked)",
			S.mode, S.mode == 1 ? "replace: the cascades off inside Range, the traced visibility alone" : "combine: the traced visibility multiplied into the cascades",
			Replacing() ? ", replacing now" : "", !RtCore::Running() ? "the RT core is not running: the cascades alone" : !TheShaderManager->GameState.isExterior ? "indoors: not traced" : "the core runs",
			textureWidth, textureHeight, frames, lastTracedFrame == 0xFFFFFFFF ? 0u : lastTracedFrame, lastTracedFrame == frame ? " (this frame)" : "", S.range, S.sunAngle,
			S.denoise ? "on" : "off", S.halfResolution ? "on" : "off", S.debugView);
		return t;
	}
	IDirect3DSurface9* VisibilitySurface() { return lastTracedFrame != 0xFFFFFFFF ? visibilitySurface : nullptr; }

	float ReplaceRange() {
		// 2026-11-04: only when this frame was traced (EffectChain calls this again right before the apply): a frame the helper fell
		// behind keeps its cascades instead of losing every sun shadow inside the range
		replaceConstant.x = Replacing() && tracedThisFrame ? S.range : 0.0f;
		return replaceConstant.x;
	}

	D3DXVECTOR4* ReplaceConstant() { return &replaceConstant; }

	int DepthImage() { return imageDepth; }
	int NormalsImage() { return imageNormals; }
}
