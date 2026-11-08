#pragma once

// UNOFFICIAL 2026-10-23 (NVR DXVK): the NVR extension on an NVR-built DXVK d3d9.dll (tools\nvr-dxvk,
// docs\next\NVR-DXVK-EXTENSION.md). Through it NVR records Vulkan compute work into DXVK's own command
// stream, in order with the D3D9 work around it: no 16-sampler, 224-constant, 16-bool or instruction
// limits for the passes that get a compute version. Absent (native D3D9, stock DXVK): Available() is
// false and every effect keeps its D3D9 pass. Nothing here changes a setting's name or meaning.
//
// Shader files: Data\Shaders\VegasShaders\Compute\<Name>.spv (built from the .comp next to them
// in src\hlsl\NewVegas\Compute with glslangValidator -V --target-env vulkan1.1).
//
// Fail once: the first failure of any step (no extension, a file missing, a refused pipeline) is
// logged once and the compute paths stay off for the rest of the session.

#include "NvrDxvkExt.h"

class DxvkCompute {
public:
	static void					Initialize(IDirect3DDevice9* device);	// RenderManager::Initialize; logs the outcome
	static bool					Available() { return Ext != nullptr && !Failed; }
	static ID3D9NvrExt*			Extension() { return Ext; }
	static const NVR_EXT_INFO&	Info() { return ExtInfo; }
	static const char*			Status();	// one line for the log and the menu

	// Reads Data\Shaders\VegasShaders\Compute\<Name>.spv; empty on failure (logged once per name).
	static bool					LoadSpirv(const char* Name, std::vector<char>& Words);
	// UNOFFICIAL 2026-10-26: whether Compute\<Name>.spv exists (a variant an older package may lack: no log, no failure).
	static bool					HasSpirv(const char* Name);

	// Helpers; null on failure (logged). The sampler heap is set 0, binding 0 in every NVR .comp and .frag.
	static ID3D9NvrExtPipeline*	CreatePipeline(const char* Name, const NVR_EXT_BINDING_DESC* Bindings, UINT Count, UINT PushSize, bool UsesSamplers);
	// A full-screen pass: <Name>.spv is the fragment shader (built-in vertex shader, uv at location 0).
	// UNOFFICIAL 2026-10-26 FailAll false (debugging tools): a missing file or refused pipeline is only logged; the other paths stay on.
	static ID3D9NvrExtPipeline*	CreateGraphicsPipeline(const char* Name, const NVR_EXT_BINDING_DESC* Bindings, UINT Count, UINT PushSize, bool UsesSamplers, UINT TargetCount = 1, NVR_EXT_BLEND Blend = NVR_EXT_BLEND_NONE, bool FailAll = true);
	// The samplers every pass uses (made once): the effects' LINEAR/CLAMP and POINT/CLAMP states.
	static ID3D9NvrExtSampler*	LinearClamp();
	static ID3D9NvrExtSampler*	PointClamp();
	static ID3D9NvrExtSampler*	LinearWrap();	// the file textures' (dirt, blood, water noise) WRAP state
	static ID3D9NvrExtSampler*	PointWrap();	// UNOFFICIAL 2026-10-25 (Cinema): POINT/WRAP, the blue noise file texture (MIN/MAG NONE)
	// UNOFFICIAL 2026-10-25 (extension 1.3): trilinear over all mips, or the same with 16x anisotropy (D3D9 ANISOTROPIC), for
	// file textures sampled with mips (Underwater's caustics, ...). Null on an older NVR DXVK: keep the D3D9 pass then.
	static ID3D9NvrExtSampler*	MipSampler(bool Anisotropic, NVR_EXT_ADDRESS Address);
	// [Main.Main.Performance] NvrDxvkPasses: the ported full-screen effects run as Vulkan passes (false, or no extension: their D3D9 passes).
	static bool					PassesOn();
	// [Main.Main.Performance] NvrDxvkVariants (UNOFFICIAL 2026-10-26): Vulkan passes use their compile-time variants
	// (Compute\variants.txt, the D3D9 techniques' fixed switches) where the .spv exists; off: the run-time switched shaders (A/B).
	static bool					VariantsOn();
	// [Main.Main.Performance] NvrDxvkStencil (UNOFFICIAL 2026-10-26): Vulkan passes test a stencil where their D3D9 technique does
	// (extension 1.6 DrawDepthStencil; SMAA's weight pass only on the edge pixels); off, or an older NVR DXVK: without (A/B).
	static bool					StencilOn();
	// [Main.Main.Performance] NvrDxvkKeepD3D9 (UNOFFICIAL 2026-10-26, test tool): whether this effect (its EffectRecord name, e.g.
	// PointShadows; names separated by commas or spaces) keeps its D3D9 passes while the others run as Vulkan passes, so one pass's
	// real cost shows in the frame total. EffectRecord::Render, ExtFullScreen and ExtDrawTo check it (so effects with their own
	// Render, AmbientLight, SMAA, AO, ..., fall back to D3D9 for the frame too).
	static bool					KeptOnD3D9(const char* Name);
	static ID3D9NvrExtBuffer*	CreateBuffer(UINT Size, UINT Flags, const char* Name);
	// UNOFFICIAL 2026-10-26 (debugging tools): as CreateBuffer, but a failure is only logged (the other compute paths stay on).
	static ID3D9NvrExtBuffer*	CreateBufferNoFail(UINT Size, UINT Flags, const char* Name);
	// UNOFFICIAL 2026-10-26 (alflicker): DXVK copies a D3DPOOL_MANAGED texture (the file textures: blue noise, SMAA's tables, noise
	// volumes, ...) to the GPU only when a D3D9 draw binds it, and the extension reads the GPU image as it is: a file texture only
	// Vulkan passes read stayed empty (AmbientLight's blue noise read 0 everywhere, so its rotating pattern moved the whole screen at
	// once: the every-4th-frame flicker; a few D3D9 frames cured it for the session). PreLoad (IDirect3DResource9) uploads what is
	// pending; nothing for the render targets. Every Draw/Dispatch call passes its bindings through this first.
	static void					PreLoad(const NVR_EXT_RESOURCE_BINDING* Bindings, UINT Count);

	// UNOFFICIAL 2026-10-26 (extension 1.5, diagnostic): while On (F10 profiling), a "DXVK PROFILE" line every 120 frames: where a frame
	// goes between the game thread (the game and NVR outside D3D9 calls; inside them DXVK's own work and its waits), DXVK's command
	// thread and the GPU. RenderHook calls it once a frame; an older NVR DXVK, native D3D9 or stock DXVK: nothing.
	static void					ProfileTick(bool On);

	// Phase 2 (extension 1.2): [Main.Main.Performance] NvrDxvkReplacements. Called by ShaderRecord::LoadShader before the device
	// sees a pixel shader's bytecode: if Data\Shaders\VegasShaders\Compute\Replace\<Name>@<hash>.spv (FNV-1a of the bytecode,
	// 8 hex digits; a Replace\dump folder collects every shader as <Name>@<hash>.pso) or <Name>.spv exists, DXVK binds that SPIR-V
	// fragment shader wherever the game binds this shader. The .txt next to it says what the replacement reads from the game:
	//   textures=0,1,2        the D3D9 samplers it declares (set 1, binding i); must be ones the original uses
	//   floats=0-3,8-15       the float registers it wants in its constant block (set 2, binding 6), packed in that order;
	//                         absent: the original's own compacted register list (DXVK's layout, harder to write against)
	// True when registered. A problem leaves the shader as it is (logged); it does not turn the other compute paths off.
	static bool					ReplacementsOn();
	// UNOFFICIAL 2026-10-26 (FORK-1026-LIMITS.md): with the Replace\dump folder, the shader's source file (template) and its defines
	// are written next to the .pso as <Name>@<hash>.defines, so tools\nvr-dxvk\replace\nolimits.py can build the no-limits version
	// (NVR_NO_SM3_LIMITS) of exactly that variant. Template/Defines may be null (nothing extra written).
	static bool					RegisterReplacement(const char* Name, const void* Bytecode, UINT Size, const char* Template = nullptr, const D3DXMACRO* Defines = nullptr);
	static ID3D9NvrExtSampler*	CreateSampler(NVR_EXT_FILTER Filter, NVR_EXT_ADDRESS Address);

	// UNOFFICIAL 2026-10-27 tessellation displacement (fork patch 0021, extension "1.7-displace": the separate ID3D9NvrExtTess
	// interface; docs/next/FORK2-DISPLACE.md): [Main.Main.Performance] NvrDxvkDisplacement + [Shaders.Terrain.Displacement].
	// ShaderRecord::LoadShader calls RegisterDisplace for every shader it creates, before the device sees the bytecode: with the switch
	// on at load and the interface present, the land's vertex shader (TerrainTemplate VS, SLS2100.vso) is registered with
	// Compute\Tess\TerrainDisplace.tcs.spv and every land pixel shader (TerrainTemplate PS, SLS2092-2146) with
	// TerrainDisplace-t<layers>.tes.spv (+ -ccw), all under one family. DXVK then draws the land's triangle lists as patches through
	// them; everything else is untouched. DisplaceTick (once a frame, TerrainShaders::UpdateConstants) sends the live settings and
	// logs 'NVR TESS' counters every 600 frames. Off at load, no NVR DXVK, or an older one: nothing is registered, nothing changes.
	// 2026-10-28: returns the family the shader was registered under (0: none), kept on its ShaderRecord for DisplaceBind.
	static bool					DisplaceOn();
	static unsigned				RegisterDisplace(const char* Name, const char* TemplateName, const D3DXMACRO* Defines, bool Vertex, const void* Bytecode, UINT Size);
	static void					DisplaceTick();
	// 2026-10-28 objects ([Shaders.POM.Displacement]; docs/next/FORK2-DISPLACE.md "Objects"): the parallax objects (ParallaxTemplate.hlsl,
	// rocks and cliffs) are family 3: their vertex shaders with Compute\Tess\ObjectDisplace[-c].tcs, their pixel shaders with
	// ObjectDisplace-h<height slot>[-c].tes. They need settings of their own, and the fork has ONE tessellation state for every family, so
	// a registered pixel shader's bind (ShaderRecord::SetCT, kDisplaceLand / kDisplaceObjects) sends its family's state when the other's
	// was the last sent. DisplaceObjectsLayout: compile ParallaxTemplate with NVR_DISPLACE_OBJECTS (the switch on under NVR DXVK).
	enum : unsigned { kDisplaceLand = 1, kDisplaceLandShadow = 2, kDisplaceObjects = 3 };
	static void					DisplaceBind(unsigned Family);
	static bool					DisplaceObjectsLayout();
	// 2026-10-27 ObjectBlend ([Shaders.Terrain.Displacement] ObjectBlend / ObjectBlendWidth / ObjectBlendKeep): the bumps fade out
	// where the land meets anything drawn before it. DisplaceDepthWanted: the world scene should take terrain blending's depth copy
	// for it (Terrain.cpp BeginWorldScene). DisplaceSceneDepth: a land pixel shader carrying that copy at s6 binds (Terrain.cpp
	// PrepareDisplaceDepth); Valid = this draw is in the world scene and the copy holds it, Scale = its viewport's share of the copy.
	static bool					DisplaceDepthWanted();
	static void					DisplaceSceneDepth(bool Valid, float ScaleX, float ScaleY);
	// 2026-10-27 CastShadows ([Shaders.Terrain.Displacement] CastShadows; core/DisplaceShadows.h, plan step 2): the land drawn into the
	// sun's shadow cascades through the same tessellation. RegisterDisplace also registers Shadows\ShadowMapLand.vso (with the TCS) and
	// ShadowMapLand-t<N>.pso (with TerrainDisplace-t<N>-s[d].tes) as family 2. NoteLandPixelShader / LandLayersOf: which bound pixel
	// shader is a land one, and its layer count (ShaderRecord::LoadShader notes every TerrainTemplate pixel shader it creates).
	// DisplaceShadowsWanted: the setting on and displacement on; DisplaceShadowLayers: bit N - 1 set when ShadowMapLand-tN.pso is
	// registered (0 until the shadow vertex shader is). DisplaceShadowBegin / End: the tessellation state for the land's shadow draws
	// (ObjectBlend from last frame's depth copy, Scale its viewport share, no wireframe), and back. DisplaceShadowsDrawn: land pieces
	// drawn displaced into the cascades this frame; while there are any, the main view's projectionPosition follows the displaced land
	// all the way (TerrainDisplace.tese ShadowsHold).
	static void					NoteLandPixelShader(IDirect3DPixelShader9* Shader, const D3DXMACRO* Defines);
	static int					LandLayersOf(IDirect3DPixelShader9* Shader);
	static bool					DisplaceShadowsWanted();
	static unsigned				DisplaceShadowLayers();
	static float				DisplaceReach();	// FadeEnd: no bumps past it
	static bool					DisplaceBlendInShadows();	// 2026-10-28 [Shaders.Terrain.Displacement] ObjectBlendShadows
	static void					DisplaceShadowBegin(bool ObjectBlend, float ScaleX, float ScaleY);
	static void					DisplaceShadowEnd();
	static void					DisplaceShadowsDrawn(unsigned Pieces);
	// 2026-10-28 far land ([Shaders.Terrain.Displacement] FarUntessellated, on by default, live; docs/next/FORK2-DISPLACE.md "Far land
	// drawn untessellated"): RenderPassStandardHook, before every game pass. A land pass drawn with the land's tessellated shaders (pass
	// IDs 0x1EF-0x230, BSSM_LAND<n>O...: SLS2100.vso) of the world scene or the water's reflection whose piece lies wholly past FadeEnd
	// + 256 units of the camera (horizontally, its world bound) is drawn with the land's tessellation state switched off: there
	// TerrainDisplace.tesc sets level 1 and moves nothing, so these are the same triangles, drawn as they are without the TCS / TES
	// (each of the piece's strips one draw instead of three patch lists). Any other pass puts the land's state back as it was (so no
	// land draw is ever left switched off by the piece before it). Context: kDisplaceWorld / kDisplaceReflection / kDisplaceOther (the
	// last keeps the tessellation; all three are counted for the 'NVR TESS' line).
	enum : int { kDisplaceWorld = 0, kDisplaceReflection = 1, kDisplaceOther = 2 };
	static void					DisplaceLandPass(NiGeometry* Geometry, UInt16 PassEnum, int Context);
	// 2026-10-28 ContactShadowsOutside ([Shaders.Terrain.Displacement], live, missing key = on; tester: a crusty, darker ring round the
	// player on tessellated land): while the land's bumps are on, where the screen-space contact shadows should start and be full
	// (view depth; ShadowsExterior.cpp takes the farther of this and NearFade). False: the bumps are off, or the option is.
	static bool					DisplaceContactFade(float& Start, float& End);
	// UNOFFICIAL 2026-11-07 (rtgap-tess, docs/next/RT-DETAIL-GAPS.md 1a) the RT passes' start slack: the depth buffer has the bumps, the RT
	// scene is flat, so a ray rebuilt from the depth can start under the RT surface. Out: [0] the largest active Strength (land or objects,
	// game units), [1] FadeStart, [2] FadeEnd of the families on (the bumps fade out between them, by view depth); false and zeros while no
	// bumps are drawn, or [Main.Main.RayTracing] TessStartSlack is off (the A/B). Plain values for RtTessContact.h.
	static bool					DisplaceRtSlack(float Out[3]);
	// UNOFFICIAL 2026-11-07 (rtgap-tessblas) the RT scene's displaced copies (RtCore::Displace): the land's / the parallax objects'
	// tessellation is registered and switched on (as last sent: [Shaders.Terrain.Displacement] / [Shaders.POM.Displacement] Enabled)
	static bool					DisplaceLandActive();
	static bool					DisplaceObjectsActive();

	// Marks every compute path off for the session (logged once with the reason).
	static void					Fail(const char* Reason);

	template<class T> static void Drop(T*& Object) { if (Object) { Object->Release(); Object = nullptr; } }

private:
	static ID3D9NvrExt*			Ext;
	static NVR_EXT_INFO			ExtInfo;
	static bool					Failed;
	static char					StatusText[160];
	static ID3D9NvrExtSampler*	SamplerLinear;
	static ID3D9NvrExtSampler*	SamplerPoint;
	static ID3D9NvrExtSampler*	SamplerWrap;
	static ID3D9NvrExtSampler*	SamplerPointWrap;
};
