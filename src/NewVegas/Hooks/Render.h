#pragma once
// Render hooks (Render.cpp). For each detoured game function: a pointer to the original (the detour's trampoline after
// installation) and the replacement. The replacements of __thiscall functions are __fastcall with an unused edx slot.

// Main::Render, the whole frame.
using MainRenderFn = void(__thiscall*)(Main*, BSRenderedTexture*, int, int);
extern MainRenderFn Render;
void __fastcall RenderHook(Main*, UInt32, BSRenderedTexture*, int, int);

// BSShader::SetShaders, per pass.
using SetShadersFn = void(__thiscall*)(BSShader*, UInt32);
extern SetShadersFn SetShaders;
void __fastcall SetShadersHook(BSShader*, UInt32, UInt32 PassIndex);

using RenderPassFn = void(__cdecl*)(void*, UInt32, UInt32, UInt32);
extern RenderPassFn RenderPassStandard;
void __cdecl RenderPassStandardHook(void* Pass, UInt32, UInt32, UInt32);
extern RenderPassFn RenderPassSkinned;
void __cdecl RenderPassSkinnedHook(void* Pass, UInt32, UInt32, UInt32);

using SetSamplerStateFn = HRESULT(__thiscall*)(NiDX9RenderState*, UInt32, D3DSAMPLERSTATETYPE, UInt32, UInt8);
extern SetSamplerStateFn SetSamplerState;
HRESULT __fastcall SetSamplerStateHook(NiDX9RenderState*, UInt32, UInt32 Sampler, D3DSAMPLERSTATETYPE, UInt32 Value, UInt8 Save);
// UNOFFICIAL LampReachMargin: ShadowSceneLight::BoundWithinLightRadius (0x00B9DAE0) with a margin (Render.cpp).
extern bool(__thiscall* BoundWithinLightRadius)(void*, const float*, void*, float);
bool __fastcall BoundWithinLightRadiusHook(void* This, UInt32 edx, const float* Bound, void* Light, float Scale);
extern float LampReachMargin;

// The world scene, the blended objects, the first-person model.
using RenderWorldSceneGraphFn = void(__thiscall*)(Main*, Sun*, UInt8, UInt8, UInt8);
extern RenderWorldSceneGraphFn RenderWorldSceneGraph;
void __fastcall RenderWorldSceneGraphHook(Main*, UInt32, Sun*, UInt8 IsFirstPerson, UInt8 WireFrame, UInt8);
extern void(__thiscall* RenderBlended)(void*, UInt32);
void __fastcall RenderBlendedHook(void* This, UInt32 edx, UInt32 Arg);	// UNOFFICIAL sandport: Sandstorm ThroughBlended

using RenderFirstPersonFn = void(__thiscall*)(Main*, NiDX9Renderer*, NiGeometry*, Sun*, BSRenderedTexture*);
extern RenderFirstPersonFn RenderFirstPerson;
void __fastcall RenderFirstPersonHook(Main*, UInt32, NiDX9Renderer*, NiGeometry*, Sun*, BSRenderedTexture*);

// Water: the reflection render, the LOD water height, the visibility test of a water node.
using RenderReflectionsFn = void(__thiscall*)(WaterManager*, NiCamera*, ShadowSceneNode*);
extern RenderReflectionsFn RenderReflections;
void __fastcall RenderReflectionsHook(WaterManager*, UInt32, NiCamera*, ShadowSceneNode*);

using RenderPipboyFn = void(__thiscall*)(Main*, NiGeometry*, NiDX9Renderer*);
extern RenderPipboyFn RenderPipboy;
void __fastcall RenderPipboyHook(Main*, UInt32, NiGeometry*, NiDX9Renderer*);

using GetWaterHeightLODFn = float(__thiscall*)(TESWorldSpace*);
extern GetWaterHeightLODFn GetWaterHeightLOD;
float __fastcall GetWaterHeightLODHook(TESWorldSpace*, UInt32);

extern bool(__thiscall* IsWaterNodeVisible)(void*, void*, void*);
bool __fastcall IsWaterNodeVisibleHook(void* This, UInt32 edx, void* Ref, void* Camera);

// Image space (the game's post process) and the save-game screenshot texture.
using ProcessImageSpaceShadersFn = void(__cdecl*)(NiDX9Renderer*, BSRenderedTexture*, BSRenderedTexture*);
extern ProcessImageSpaceShadersFn ProcessImageSpaceShaders;
void __cdecl ProcessImageSpaceShadersHook(NiDX9Renderer*, BSRenderedTexture* Source, BSRenderedTexture* Destination);

using ShowDetectorWindowFn = void*(__thiscall*)(DetectorWindow*, HWND, HINSTANCE, NiNode*, char*, int, int, int, int);
extern ShowDetectorWindowFn ShowDetectorWindow;
void* __fastcall ShowDetectorWindowHook(DetectorWindow*, UInt32, HWND, HINSTANCE, NiNode* Root, char* Caption, int Left, int Top, int Width, int Height);

using CreateBSRenderedTextureFn = BSRenderedTexture*(__cdecl*)(BSString*, const UInt32, const UInt32, NiTexture::FormatPrefs*, UInt32, bool, NiDepthStencilBuffer*, UInt32, UInt32);
extern CreateBSRenderedTextureFn CreateBSRenderedTexture;
BSRenderedTexture* __cdecl CreateSaveTextureHook(BSString* Name, const UInt32 Width, const UInt32 Height, NiTexture::FormatPrefs* Prefs,
	UInt32 MSAA, bool UseDepthStencil, NiDepthStencilBuffer* DepthStencil, UInt32 Unknown7, UInt32 BackgroundColor);

// The HUD / menus pass.
extern CallDetour kRenderInterfaceDetour;
void __fastcall RenderInterfaceHook(void* Interface, void*, void* Culler, bool PipboyVisible);

// Naked mid-function patches (bodies in Render.cpp).
void SetTileShaderConstantsHook();
void DetectorWindowCreateTreeViewHook();
void DetectorWindowDumpAttributesHook();
void DetectorWindowConsoleCommandHook();

void __fastcall MuzzleLightCullingFix(MuzzleFlash*);

BSFogProperty* __cdecl ShadowSceneNode__GetFogPropertyEx(UInt32 FogType);
NiPoint2* __fastcall WaterFogRemover(NiPoint2* Out, void*, float X, float Y);

bool __fastcall NiDX9Renderer__Do_EndFrame(NiDX9Renderer* Renderer, void*);

void AttachGrassFirstRenderPassHook();	// UNOFFICIAL 2026-10-17 GrassFirst ([Shaders.Grass.Main] DrawFirst), inside the Detours transaction
void InstallGrassFirstHooks();			// after it
void InstallMaterialDrawHooks();		// UNOFFICIAL 2026-11-07 perf-mirrorfix2: MaterialAware's alpha + the RT hair tint at every object draw (after the cloth hooks)
void InstallSoftParticleHook();			// UNOFFICIAL 2026-11-05 soft particles: BSShaderNoLighting::SetupGeometryConstants

// Detector window (the scene graph browser) helpers.
void DetectorWindowDumpAttributes(HWND, UInt32 Message, WPARAM, LPTVINSERTSTRUCTA Item);
void DetectorWindowSetNodeName(char* Out, int OutSize, char* Format, char* ClassName, char* Name, float X, float Y, float Z);
