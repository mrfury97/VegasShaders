#pragma once
#include "../../core/FailOnce.h"

class GodRaysEffect : public EffectRecord
{
public:
	// UNOFFICIAL 2026-11-12 (memleak) idle targets (core/TextureManager.h, EffectRecord::TargetParkable): the ray buffers and the reduction chains are used only from Render, behind the switch
	bool	TargetParkable(const char* SamplerName) override { return true; }
	GodRaysEffect() : EffectRecord("GodRays") {};

	struct GodRaysStruct { D3DXVECTOR4 Ray, RayColor, Data; };
	GodRaysStruct	Constants;

	void	UpdateConstants();
	void	RegisterConstants(), RegisterTextures();
	void	UpdateSettings();
	bool	ShouldRender();

	float dayMult, nightMult;
	bool sunGlareEnabled;
	float rayVisibility;
	// UNOFFICIAL 2026-11-02 RetryFailedEffects (core/FailOnce.h): were bools that held until restart
	FailOnce::Latch packedRenderFailed{ "GodRays packed pass" };
	FailOnce::Latch dedicatedRenderFailed{ "GodRays dedicated pass" };
	CachedHandle techDedicated, paramLayout;	// UNOFFICIAL: looked up once per loaded effect
	// UNOFFICIAL 2026-10-16 SkipEmpty: the light mask's maximum taken down to 1 x 1 in 8 x 8 steps, per ray size ([0] half, [1] quarter).
	bool skipEmpty = true;
	CachedHandle techReduce, paramReduce, paramSkip;
	IDirect3DTexture9* maxTexture[2][4] = {};
	IDirect3DSurface9* maxSurface[2][4] = {};
	UINT maxWidth[2][4] = {}, maxHeight[2][4] = {};
	int maxSteps[2] = {};
	bool RunReduce(IDirect3DDevice9* Device, IDirect3DTexture9* source, UINT width, UINT height, int set);
	// UNOFFICIAL 2026-10-18 SkipIdle (look option): see GodRays.cpp IdleGuard.
	bool skipIdle = false;
	bool idleEmptyKnown = false;	// the last finished check found the sky mask's maximum under idleLimit
	bool idleCheckFrame = false, idleCheckSkipped = false;
	unsigned idleIssueFrame = 0;	// fixes-a #54: FrameNumber when the pending query was issued (an answer more than 8 frames old is not trusted)
	int idleSinceCheck = 0, idleAge = 0, probeLevel = 0;	// frames since the last check was issued; age of the unanswered query; [Shaders.GodRays.Main] ProbeLog
	bool idlePending = false, idleQueryBroken = false;	// a query is out and not answered yet; the device has no occlusion queries
	float idleLimit = 0.0f, idleTruth = -1.0f, idleLimitAtIssue = 0.0f;
	DWORD idleLastPixels = 0;
	IDirect3DQuery9* idleQuery = nullptr;	// occlusion query around the 1 x 1 test draw (GodRaysIdleTest)
	CachedHandle techIdle;
	unsigned idleWindow = 0, idleSkipped = 0, idleChecks = 0, idleCheckOnly = 0, idleFull = 0, idleGuardRefused = 0, idleValidated = 0, idleValidateBad = 0, idleValidateBright = 0, idleValidateTick = 0;
	bool IdleGuard() const;
	void IdlePoll();
	bool IdleIssue(IDirect3DDevice9* Device, IDirect3DTexture9* skyMask, UINT width, UINT height, int set);
	void IdleStats();
	// DIAGNOSTIC (grnoon 2026-10-18, development builds only): [Shaders.GodRays.Main] ProbeLog 1 logs the largest sky-mask and light-mask values
	bool Probe(IDirect3DDevice9* Device, IDirect3DTexture9* const* textures, UINT width, UINT height, int set, bool lowRes);
	IDirect3DTexture9* raysTexture[2] = {};
	IDirect3DSurface9* raysSurface[2] = {};
	// Quarter-resolution pair, used instead when [Main.Main.ReducedQuality] GodRaysLowRes is on.
	IDirect3DTexture9* raysTextureLow[2] = {};
	IDirect3DSurface9* raysSurfaceLow[2] = {};

	// UNOFFICIAL 2026-10-25 (NVR DXVK): ext = the six passes and the SkipEmpty reduction as Vulkan passes (GodRays*.frag);
	// SkipIdle's 1 x 1 test inside the occlusion query stays a D3D9 draw.
	bool RenderDedicated(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget,
		IDirect3DSurface9* RenderedSurface, bool ext = false);
	bool ExtPrepare(const D3DXVECTOR4& layout);
	bool ExtDrawPass(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT pass,
		IDirect3DTexture9* const* textures, IDirect3DSurface9* const* surfaces, const D3DXVECTOR4& skip, IDirect3DTexture9* reduced);
	ExtPass extSkyMask, extLightMask, extBlur, extCombine, extReduce;
	ExtUniformBuffer extUniforms;
	bool extDrawing = false;	// RenderDedicated is on its Vulkan path: RunReduce draws through the extension too
	void Render(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget,
		IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget,
		IDirect3DSurface9* SourceBuffer) override;
};
