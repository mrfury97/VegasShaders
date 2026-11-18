#include "../../core/GameShaderPairs.h"	// UNOFFICIAL 2026-11-13 (the game's own shaders, noted as they are made)
// The game creates its NiD3D shader objects here; NVR prepares the extra fields of each one,
// gives the two noise shaders the water height-map names, and loads its own replacement.
typedef NiD3DVertexShader* (__thiscall* CreateVertexShaderFn)(BSShader*, char*, char*, char*, char*);
typedef NiD3DPixelShader* (__thiscall* CreatePixelShaderFn)(BSShader*, char*, char*, char*, char*);
typedef void (__cdecl* SetShaderPackageFn)(int, int, UInt8, int, char*, int);

CreateVertexShaderFn CreateVertexShader = (CreateVertexShaderFn)Hooks::CreateVertexShader;
CreatePixelShaderFn CreatePixelShader = (CreatePixelShaderFn)Hooks::CreatePixelShader;
SetShaderPackageFn SetShaderPackage = (SetShaderPackageFn)Hooks::SetShaderPackage;

// Common set-up of a freshly made vertex or pixel shader. NoiseA/NoiseB are the game names that
// become WATERHEIGHTMAP0/1 (Suffix ".vso"/".pso"); WaterSlots receives them.
template <typename ShaderEx, typename Slot>
static void PrepareNewShader(ShaderEx* Shader, const char* GameName, const char* Kind, const char* NoiseA, const char* NoiseB,
	const char* AliasA, const char* AliasB, Slot* WaterSlots) {
	for (int variant = ShaderRecordType::Default; variant <= ShaderRecordType::Interior; variant++)
		Shader->ShaderProg[variant] = NULL;
	Shader->ShaderHandleBackup = Shader->ShaderHandle;
	GameShaderPairs::NoteMade(GameName, (IUnknown*)Shader->ShaderHandle, Kind[0] == 'p');	// UNOFFICIAL 2026-11-13 (core/GameShaderPairs.h: which shader package the game took them from)
	ShaderOwnership::Mark(Shader);	// 2026-11-03: NVR's fields in this object are now its own (ShaderRecord.h ShaderOwnership)
	Shader->SetName(GameName);

	if (!strcmp(GameName, NoiseA)) {
		Shader->SetName(AliasA);
		WaterSlots[0] = Shader;
	}
	else if (!strcmp(GameName, NoiseB)) {
		Shader->SetName(AliasB);
		WaterSlots[1] = Shader;
	}
	// UNOFFICIAL 2026-10-27 diagnostic (grass shaders silently missing from the 20:10 launch on): each grass shader the game makes
	if (!memcmp(GameName, "GRASS", 5)) Logger::Log("UNOFFICIAL the game makes the grass %s shader %s", Kind, GameName);
	TheShaderManager->LoadShader(Shader);
}

NiD3DVertexShader* __fastcall CreateVertexShaderHook(BSShader* owner, UInt32 unusedEdx, char* fileName, char* arg2, char* shaderType, char* shaderName) {
	auto* made = (NiD3DVertexShaderEx*)CreateVertexShader(owner, fileName, arg2, shaderType, shaderName);
	if (made) PrepareNewShader(made, shaderName, "vertex", "ISNOISESCROLLANDBLEND.vso", "ISNOISENORMALMAP.vso",
		"WATERHEIGHTMAP0.vso", "WATERHEIGHTMAP1.vso", TheShaderManager->WaterVertexShaders);
	return made;
}

NiD3DPixelShader* __fastcall CreatePixelShaderHook(BSShader* owner, UInt32 unusedEdx, char* fileName, char* arg2, char* shaderType, char* shaderName) {
	auto* made = (NiD3DPixelShaderEx*)CreatePixelShader(owner, fileName, arg2, shaderType, shaderName);
	if (made) PrepareNewShader(made, shaderName, "pixel", "ISNOISESCROLLANDBLEND.pso", "ISNOISENORMALMAP.pso",
		"WATERHEIGHTMAP0.pso", "WATERHEIGHTMAP1.pso", TheShaderManager->WaterPixelShaders);
	return made;
}

// After the game picks its shader package from the graphics card, force package 7 (SM3) as current and highest.
void __cdecl SetShaderPackageHook(int a1, int a2, UInt8 force1x, int a4, char* graphicsName, int a6) {
	SetShaderPackage(a1, a2, force1x, a4, graphicsName, a6);
	*(UInt32*)0x011F91C0 = 7;	// current package
	*(UInt32*)0x011F91BC = 7;	// highest package
}
