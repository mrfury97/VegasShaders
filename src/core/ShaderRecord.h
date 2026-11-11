#pragma once

#include "ShaderTemplate.h"
#include <mutex>
#include <unordered_set>

// How LoadShader treats the compiled-shader cache.
enum ShaderCompileType { AlwaysOff, AlwaysOn, RecompileChanged, RecompileInMenu, RecompileAbsent };

// One constant or sampler a shader reads, as found in its constant table.
class ShaderValue {
public:
	ShaderValue() {}
	virtual ~ShaderValue() {}

	const char*	Name;
	UInt32		RegisterIndex, RegisterCount;
};

// A float/vector constant; Value points at the CPU-side data it is fed from.
class ShaderFloatValue : public ShaderValue {
public:
	ShaderFloatValue() : Value(nullptr) {}
	~ShaderFloatValue() override {}

	void				GetValueFromConstantTable();	// fills Value from the current effect/shader constants

	D3DXVECTOR4*		Value;	D3DXPARAMETER_TYPE Type;
};

// A sampler; Texture is resolved from TexturePath or the TESR_ name.
class ShaderTextureValue : public ShaderValue {
public:
	ShaderTextureValue() : Texture(nullptr) {}
	~ShaderTextureValue() override {}

	void				GetSamplerStateString(ID3DXBuffer* Source, UINT32 SamplerIndex);	void GetTextureRecord();

	std::string							SamplerString, TexturePath;
	TextureRecord*						Texture;	TextureRecord::TextureRecordType Type;
};

// Shared part of game-shader records and effect records: their constant/sampler tables.
class ShaderProgram {
public:
	ShaderProgram();
	virtual ~ShaderProgram();

	virtual void			SetCT() = 0;
	virtual void			CreateCT(ID3DXBuffer* Source, ID3DXConstantTable* Table) = 0;

	static void				ReportError(HRESULT Hr);
	static bool				FileExists(const char* FilePath);
	static bool				CheckPreprocessResult(const char* CachedPath, ID3DXBuffer* Source);
	static std::string		PortableSource(ID3DXBuffer* Source);
	// UNOFFICIAL 2026-11-10 shipped shader cache (ShaderRecord.cpp): a stale or missing cache entry is filled from the release's own
	// read-only copy (...\CacheShipped\) when that one was made from exactly this source. True = the cache entry is now fresh.
	static bool				SeedFromShipped(const char* PreprocessedPath, const char* CompiledPath, const std::string& Text);
	static long				ShippedSeeds();

	// Drops the cached IDirect3DTexture9* for a named sampler so that SetCT re-resolves it
	// on the next draw. MUST be called on every consumer of a texture that is released and
	// recreated -- see ShaderManager::ClearShaderSamplers. Lives on ShaderProgram rather
	// than EffectRecord because game shaders need it too.
	void					ClearSampler(const char* SamplerName, size_t NameLength);

	// Tables built by CreateCT: an array and its length for each kind.
	ShaderFloatValue*		FloatShaderValues;		UInt32 FloatShaderValuesCount;
	ShaderTextureValue*		TextureShaderValues;	UInt32 TextureShaderValuesCount;
};

// A replacement for one of the game's own vertex/pixel shaders.
class ShaderRecord : public ShaderProgram {
public:
	ShaderRecord();
	~ShaderRecord() override;

	void					SetCT() override;
	void					CreateCT(ID3DXBuffer* Source, ID3DXConstantTable* Table) override;
	virtual void			SetShaderConstantF(UInt32 FirstRegister, D3DXVECTOR4* Data, UInt32 Count) = 0;

	// CacheSubPath: the cache subfolder when it is not SubPath (an Interior record compiled from the main source);
	// Overrides: defines that win over the global ones ShaderRecord adds (NULL-terminated).
	static void				BuildDefines(ShaderTemplate& Template, const D3DXMACRO* Overrides);	// UNOFFICIAL: the defines LoadShader compiles with
	static ShaderRecord*	LoadShader(const char* Name, const char* SubPath, ShaderTemplate Template = ShaderTemplate{},
		const char* CacheSubPath = nullptr, const D3DXMACRO* Overrides = nullptr);

	const char*				Name;
	bool					HasRenderedBuffer, HasDepthBuffer;
	bool					HasTerrainBlendDepth = false;	// UNOFFICIAL: the land shaders with terrain blending compiled in (Terrain.h)
	bool					HasDisplaceDepth = false;		// UNOFFICIAL 2026-10-27: land pixel shaders with displacement ObjectBlend's s6 (Terrain.h)
	bool					ClearSamplers;
	bool					PointShadowForward = false;	// UNOFFICIAL: compiled with INTERIOR_SHADOWS (PointShadowForward.h)
	bool					UsedOnce = false;			// UNOFFICIAL: SetCT ran once this session (ShaderWarmUp.h)
	bool					IsLandLight = false;			// UNOFFICIAL 2026-10-25: a land pixel shader reading the game's SunDir in c18 (LandLight.h)
	bool					CloudSunReg = false;			// UNOFFICIAL 2026-11-04 ForwardCloudShadows: declares NVR_CloudSun (c169, pixel) or NVR_CloudSunVS (c240-c241, vertex)
	unsigned char			DisplaceFamily = 0;			// UNOFFICIAL 2026-10-28: registered for tessellation under this family (DxvkCompute::DisplaceBind)
};

class ShaderRecordVertex : public ShaderRecord {
public:
	ShaderRecordVertex(const char* RecordName);
	~ShaderRecordVertex() override;

	void					SetShaderConstantF(UInt32 FirstRegister, D3DXVECTOR4* Data, UInt32 Count) override;

	IDirect3DVertexShader9*	ShaderHandle;
};

class ShaderRecordPixel : public ShaderRecord {
public:
	ShaderRecordPixel(const char* RecordName);
	~ShaderRecordPixel() override;

	void					SetShaderConstantF(UInt32 FirstRegister, D3DXVECTOR4* Data, UInt32 Count) override;

	IDirect3DPixelShader9*	ShaderHandle;
};

// Index into NiD3D*ShaderEx::ShaderProg: which variant of a replaced game shader.
enum ShaderRecordType { Default, Exterior, Interior };

// The game's shader objects, extended with our replacement records (members live in GameNi.h).
class NiD3DVertexShaderEx : public NiD3DVertexShader {
public:
	void					SetupShader(IDirect3DVertexShader9* BoundHandle);
	void					DisposeShader();
	ShaderRecordVertex*		GetShaderRecord(ShaderRecordType Variant);
	static void __fastcall	Free(NiD3DVertexShaderEx* Shader);
};

// UNOFFICIAL 2026-10-22 MarkShaders (test only, ShaderRecord.cpp NiD3DPixelShaderEx::SetupShader): SLS pixel shader range drawn magenta; 0 = off.
inline int MarkShadersFrom = 0, MarkShadersTo = 0, MarkShadersFamily = 0, MarkShadersVanilla = 0, MarkShadersVsFrom = 0, MarkShadersVsTo = 0;
// UNOFFICIAL 2026-10-27 [Shaders.PBR.Main] GameShaderEyes (SetShadersHook, Render.cpp): eyeballs drawn with the game's own shaders.
inline bool GameShaderEyes = true;
// UNOFFICIAL 2026-11-09 [Shaders.PBR.Main] EyeShaderOld (A/B, Render.cpp GameEyes): true = the eyes as before 11-09 (the game's own shaders
// while LinearScene is off, the mod's ordinary object shader while it is on); false (default) = the eye variant (ObjectTemplate.hlsl NVR_EYE).
inline bool EyeShaderOld = false;
// UNOFFICIAL 2026-11-09 [Shaders.PBR.Main] GameShadersInMenus (SetShadersHook, Render.cpp): the character creation menu's head (hair, eyes,
// teeth, clothes: everything but the skin, which has had this rule since 10-27) drawn with the game's own shaders.
inline bool GameShadersInMenus = true;

// 2026-11-03 (white env-mapped objects with Real Time Reflections, three testers): NVR keeps its own fields INSIDE the game's shader
// objects (Enabled, ShaderProg[3], ShaderHandleBackup: GameNi.h NiD3DShaderProgram), filled in by the CreateVertexShader /
// CreatePixelShader hooks (ShaderIO.cpp PrepareNewShader). A shader another plugin makes some other way -- Real Time Reflections
// swaps its own env-map pixel shaders (made through Fallout Shader Loader) into the game's passes -- never went through them, so
// those slots hold whatever the game keeps there: SetupShader copied that "backup" over the real handle on every draw (NVR on or
// off), and the env pass drew with no pixel shader (flat white). Only shaders this plugin prepared are touched now; any other one
// is left exactly as its owner set it, and named once in the log.
namespace ShaderOwnership {
	inline std::unordered_set<const void*> Prepared;
	inline std::mutex Lock;
	inline void Mark(const void* shader) { std::lock_guard<std::mutex> hold(Lock); Prepared.insert(shader); }
	inline void Forget(const void* shader) { std::lock_guard<std::mutex> hold(Lock); Prepared.erase(shader); }
	inline bool Ours(const void* shader) { std::lock_guard<std::mutex> hold(Lock); return Prepared.count(shader) != 0; }
	// true when NVR may set this shader up; false (named in the log the first time) for another plugin's
	inline bool MaySetUp(const void* shader, const char* kind) {
		if (Ours(shader)) return true;
		static std::unordered_set<const void*> named;
		if (named.size() < 64 && named.insert(shader).second)
			Logger::Log("UNOFFICIAL shaders: a %s shader Vegas Shaders did not make is in a game pass (another plugin's, e.g. Real Time Reflections): left as its owner set it", kind);
		return false;
	}
}

class NiD3DPixelShaderEx : public NiD3DPixelShader {
public:
	void					SetupShader(IDirect3DPixelShader9* BoundHandle);
	void					DisposeShader();
	ShaderRecordPixel*		GetShaderRecord(ShaderRecordType Variant);
	static void __fastcall	Free(NiD3DPixelShaderEx* Shader);
};