#pragma once
#include <functional>	// (vector/map/string arrive with Framework.h)
#include "NvrDxvkExt.h"

class Sm6Pass;		// UNOFFICIAL 2026-10-26 (core\Sm6Pass.h)
struct Sm6PassSet;

// One post-process effect: an ID3DXEffect compiled from the Effects folder's <Name>.fx.hlsl, its constant and
// sampler tables, and the full-screen Render that runs one of its techniques through the frame chain.
class EffectRecord : public ShaderProgram {
public:
	EffectRecord(const char* EffectFileName);
	~EffectRecord() override;

	void					SetCT() override;
	void					CreateCT(ID3DXBuffer* Source, ID3DXConstantTable* Table) override;

	// Per-effect hooks; the base versions do nothing.
	virtual void			UpdateConstants() {}	// each frame, before the effect renders
	virtual void			UpdateSettings() {}		// after a settings change
	virtual void			RegisterConstants() {}	// TESR_ constants into TheShaderManager's table
	virtual void			RegisterTextures() {}	// render targets/textures the effect owns
	virtual bool			ShouldRender() { return true; }	// false skips Render under the effect's own conditions
	// UNOFFICIAL 2026-11-12 (memleak) [Main.Main.Performance] TargetReleaseSeconds (core/TextureManager.h, the idle targets). An effect
	// answers true for a target of its own (made with TextureManager::InitTexture into one of its members) that NOTHING draws into or
	// needs the contents or size of while TargetsWanted() is false, and whose pointer nobody keeps past a frame: the texture manager
	// then keeps a 1 x 1 stand-in in its place while the effect is off and makes the real one again when it is switched on. The base
	// answer is no (kept as before). Before answering yes for a new effect: every reader and writer of the target outside the effect,
	// every method of the effect reachable while it is off, every copy of the pointer (docs: the effect's README, "Idle targets").
	virtual bool			TargetParkable(const char* SamplerName) { return false; }
	// ... and whether the effect needs its targets now. Enabled, unless it is on but stands aside (SMAA while TAA runs).
	virtual bool			TargetsWanted() { return Enabled; }
	virtual void			BeforeDraw(IDirect3DDevice9* Device) {};	// UNOFFICIAL 2026-10-22: right before each pass's draw (legacy path; SunFar's HazardLog)
	virtual bool			SwitchEffect();			// flips the menu switch, loads or disposes accordingly
	virtual void			Render(IDirect3DDevice9* Device, IDirect3DSurface9* Target, IDirect3DSurface9* Rendered, UINT techniqueIndex, bool ClearTarget, IDirect3DSurface9* Source);
	// UNOFFICIAL 2026-10-23 (NVR DXVK, core\DxvkCompute.h): an effect ported to the extension's full-screen passes overrides
	// ExtRender; Render calls it first and returns when it drew (true). It must draw the same picture as the D3D9 technique
	// (its GLSL in src\hlsl\NewVegas\Compute, kept beside the .fx.hlsl). ExtFullScreen does one pass through the frame
	// chain exactly as Render does (the chain's spare texture, the direct final write, or the legacy copy): the pipeline's
	// inputs are bound by the caller; TESR_RenderedBuffer is TheTextureManager->RenderedTexture at that moment.
	// ExtFailed: a failed pass turns the effect's ported path off for the session (logged once); Render then uses D3D9.
	virtual bool			ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) { return false; }
	// last: false for a pass that is not the effect's last (a multi-pass port): it never writes straight into the game target.
	bool					ExtFullScreen(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, IDirect3DSurface9* SourceBuffer,
								ID3D9NvrExtPipeline* Pipeline, const NVR_EXT_RESOURCE_BINDING* Bindings, UINT BindingCount, const void* Push, UINT PushSize, bool last = true);
	bool					ExtFailed = false;
	UInt32					ExtFrames = 0;
	// The common case: a pass whose fragment shader reads N textures (set 1, binding 0..N-1, slots 0..N-1) through one sampler
	// (slot 0, its heap index at SamplerPushOffset of the push block; WrapSamplerPushOffset > 0: a LINEAR/WRAP sampler at
	// slot 1 for file textures) and a push block of constants. The pipeline is made on first use from
	// Shaders\VegasShaders\Compute\<SpvName>.spv and kept in Pass.
	struct ExtPass { ID3D9NvrExtPipeline* Pipeline = nullptr; bool Tried = false; };
	bool					ExtTexturePass(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, IDirect3DSurface9* SourceBuffer,
								ExtPass& Pass, const char* SpvName, IDirect3DBaseTexture9* const* Textures, UINT TextureCount,
								const void* Push, UINT PushSize, UINT SamplerPushOffset, bool PointSampler = false, UINT WrapSamplerPushOffset = 0, bool PointWrapSampler = false, bool Last = true);
	// UNOFFICIAL 2026-10-26: a pass shader's compile-time variant, Compute\<SpvName>-<Suffix>.spv (Compute\variants.txt: the D3D9
	// technique's fixed switches, so unused paths are compiled out as fxc does), when [Main.Main.Performance] NvrDxvkVariants is
	// on and the file exists; else Generic (the run-time switched shader). Make creates a pipeline from a .spv name with the
	// caller's bindings; each variant's pipeline is kept in Variants. Null: Make failed (as for Generic alone).
	ID3D9NvrExtPipeline*	ExtVariant(std::map<std::string, ExtPass>& Variants, ExtPass& Generic, const char* SpvName, const char* Suffix,
								const std::function<ID3D9NvrExtPipeline*(const char*)>& Make);
	// UNOFFICIAL 2026-10-25: a pass into the effect's OWN targets (1-4 surfaces of one size, e.g. its half-resolution buffers),
	// optional viewport rectangle; the frame chain is not touched. A multi-pass port chains these and ends with ExtFullScreen
	// (or ExtTexturePass) for the pass that writes the frame. A failure sets ExtFailed (logged), as ExtFullScreen does.
	// UNOFFICIAL 2026-10-26 Uv: what the GLSL's built-in `uv` must equal. TexelCentre (default): the texel centre, (x + 0.5) / W --
	// right when the D3D9 pass used a vertex shader that puts it there (HalfVS, PackedVS), a quad made for this size (Bloom's), or
	// the GLSL adds the offset itself (AmbientLight's frameUV, SkyView's uvShift) or reads gl_FragCoord (VPOS). ScreenQuad: the
	// D3D9 pass drew the frame quad (FrameVS over ShaderManager::CreateFrameVertex) into a target smaller than the screen, so it
	// read at x / W + 0.5 / screen; the pass is moved by that sub-pixel amount (extension 1.4 DrawShifted; zero for a screen-size
	// target, so ScreenQuad is always safe for a FrameVS pass). Audit 2026-10-26: docs/next/NVR-DXVK-EXTENSION.md, half-pixel rule.
	// UNOFFICIAL 2026-10-26 DepthStencil (extension 1.6 DrawDepthStencil): a depth-stencil surface of the targets' size and its tests,
	// e.g. a pass that runs only where an earlier one left a stencil mark (SMAA's weights). False without 1.6 (no ExtFailed: the
	// caller keeps its path without the attachment).
	enum class ExtUv { ScreenQuad, TexelCentre };
	bool					ExtShiftLogged = false;
	bool					ExtDrawTo(ID3D9NvrExtPipeline* Pipeline, IDirect3DSurface9* const* Targets, UINT TargetCount, const NVR_EXT_RESOURCE_BINDING* Bindings,
								UINT BindingCount, const void* Push, UINT PushSize, const RECT* Viewport = nullptr, ExtUv Uv = ExtUv::TexelCentre,
								const NVR_EXT_DEPTH_STENCIL* DepthStencil = nullptr);
	// UNOFFICIAL 2026-10-25: constants over the 240 push bytes (big kernels, matrices): Data uploaded into a uniform buffer the
	// effect keeps (made on first use, remade when Size changes; recorded in order with the draws, no waits). Bind the returned
	// buffer as NVR_EXT_BIND_UNIFORM_BUFFER (GLSL: layout(set = 2, binding = 0, std140) uniform ...). Null: no extension or failed.
	struct ExtUniformBuffer { ID3D9NvrExtBuffer* Buffer = nullptr; UINT Size = 0; bool Tried = false; };
	ID3D9NvrExtBuffer*		ExtUniforms(ExtUniformBuffer& Uniforms, const void* Data, UINT Size);
	// The loaded file texture behind one of the effect's samplers (e.g. "TESR_BloodLensSampler"), null if none.
	IDirect3DBaseTexture9*	ExtFileTexture(const char* SamplerName);
	// ClearSampler is inherited from ShaderProgram -- game shaders need the same thing.
	void					DisposeEffect();	// releases the ID3DXEffect and the tables (LoadGeneration moves on)
	bool					LoadEffect();		// compiles/reads the effect file; false when it failed
	static int				BuildMacros(D3DXMACRO* EffectMacros);	// UNOFFICIAL 2026-10-26: the effects' compile-time defines (room for 10; core\Sm6Pass)
	// UNOFFICIAL 2026-10-26 (deep/fork-sm6, NvrSm6Shaders): a pass of this effect's technique with an SM6 build (core\Sm6Pass.h), ready;
	// null otherwise. Render draws such a pass from it when it is Plain (FrameVS, no render states); Sm6Covers: every pass of the
	// technique is, so the hand-ported ExtRender is skipped for it.
	struct Sm6PassSet*		Sm6Set = nullptr;
	class Sm6Pass*			Sm6For(UINT techniqueIndex, UINT pass);
	bool					Sm6Covers(UINT techniqueIndex);

	bool					IsLoaded();	// an ID3DXEffect is there
	bool					Enabled;	// the effect's menu switch
	bool					usesSourceBuffer = true; // false when the effect declares no TESR_SourceBuffer sampler
	// True when a pass can leave destination pixels unwritten or depends on their contents
	// (clip/discard, blending, stencil, colour write masks). Under the frame chain such passes
	// get their destination pre-filled with the current image, matching the old in-place result.
	bool					needsPrefill = true;
	// Per technique and pass, filled when only clip/discard set needsPrefill: whether that pass's
	// compiled pixel shader can kill pixels (texkill). A pass that cannot writes every pixel, so a
	// pre-filled destination would be overwritten entirely (the match was in code no pass of it
	// uses, such as a shared include's helpers). Empty: every pass follows needsPrefill.
	std::vector<std::vector<bool>> passPrefill;
	bool					PassNeedsPrefill(UINT techniqueIndex, UINT pass) const;
	// Set by an effect around one Render call whose passes will write every pixel of the target (no scissor, discard
	// or blending that frame): the frame chain then skips the pre-fill copy (VolumetricSmoke over most of the screen).
	bool					writesWholeTarget = false;
	void					RebindSlotTextures();
	float					renderTime, constantUpdateTime;	// ms of the last Render / UpdateConstants (menu timings)

	// UNOFFICIAL: D3DX finds a technique or parameter by name with a string scan on every call, and the chain did about
	// 25 of those a frame. A handle is fixed for the life of the ID3DXEffect, so each is looked up once per loaded effect
	// (LoadGeneration changes whenever the effect is loaded or disposed) and kept in a CachedHandle the caller owns.
	struct CachedHandle { unsigned Generation = 0; D3DXHANDLE Handle = NULL; };
	unsigned				LoadGeneration = 1;
	D3DXHANDLE				TechniqueHandle(CachedHandle& cache, const char* name);
	D3DXHANDLE				ParameterHandle(CachedHandle& cache, const char* name);
	// UNOFFICIAL 2026-10-19: textures a caller wants on stages for its non-TESR (NVR_) samplers during its next Render calls. Render
	// sets them again right after every BeginPass (as VolumetricFog and AmbientLight bind theirs by hand there): a texture set on the
	// device before the pass began did not reach the pass (SunFar's cloud layers, Specular's sky visibility, 2026-10-19 tests).
	// Null entries are left alone; ClearPassTextures unbinds the ones set and empties the list.
	IDirect3DBaseTexture9*	PassTextures[16] = {};
	void					SetPassTexture(UINT stage, IDirect3DBaseTexture9* texture) { if (stage < 16) PassTextures[stage] = texture; }
	void					BindPassTextures();
	void					ClearPassTextures();

	ID3DXEffect*			Effect;	// null while not loaded
	const char*				Name;	// file name without .fx.hlsl, also the settings name
	int					StatusSlot = -1;	// UNOFFICIAL 2026-11-01: its slot in core/EffectStatus.h (Render notes each call)
	// UNOFFICIAL 2026-11-01: a Render override's early return notes why (off / not loaded / ShouldRender false, or why when given)
	void					NoteSkip(const char* why = nullptr) { EffectStatus::Note(StatusSlot, Name, !Enabled ? EffectStatus::Disabled : !Effect ?
								EffectStatus::NotLoaded : why ? EffectStatus::Skipped : EffectStatus::ShouldRenderFalse, why); }
};
