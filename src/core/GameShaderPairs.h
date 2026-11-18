#pragma once
// UNOFFICIAL 2026-11-13 ([Main.Main.Misc] GameShaderPairCheck, GameShaderPairRepair; docs/next/FLASHLIGHT-AUDIT-2026-11-13.md section 4a).
// Found while asking why the flashlight's game lamp lit nothing on rocks drawn "by the game's own shaders" (POM off): they were not the
// game's shaders. The install had loose shader files from TWO other mods in Data\Shaders\Loose (loaded by Fallout Shader Loader in place
// of the shader package's): parallax pixel shaders built from an old New Vegas Reloaded (ps_3_0; the lamp's direction on TEXCOORD3, its
// falloff coordinates on TEXCOORD4, the view on TEXCOORD7) and, over that pack's own vertex shaders, PAR2000-2007.vso from a later
// vertex-shader pack with the GAME's layout (lamp direction TEXCOORD2, falloff TEXCOORD5, view TEXCOORD6). The pixel shader PAR2029
// then read its falloff coordinates from the vertex shader's half vector: always "out of reach", so NO lamp ever lit those rocks, with or
// without this mod. To this mod both halves are "the game's" (the handle the game made), so nothing said so.
//
// Check (default on): at a shader bind where at least one half is not this mod's, the pixel shader's TEXCOORD inputs are compared with
// the vertex shader's TEXCOORD outputs (D3DX reads both from the bytecode), once per pair. A pixel shader that reads a coordinate the
// vertex shader does not write is a pair that does not fit: one log line each (first 24), and, when neither half is this mod's and
// one differs from the shader package's own (another mod's replacement), a line in Help > Install check. Cost: two pointer compares at a bind of
// this mod's own pairs; one hash lookup at the others (binds happen once per run of a pass kind, not per draw).
// It cannot see two layouts that use the same coordinates for different things.
//
// Repair (default ON since 2026-11-13 b, the owner's call: such a pair is broken either way, with the repair its objects are lit again;
// it does override another mod's shader, so off = only named): a pair that does not fit, with neither half this mod's, is drawn with the
// first of these that does fit: the package's pixel shader, the package's vertex shader, the package's pair. The package is the game's
// own Data\Shaders\shaderpackageNNN.sdp, read once when first needed (which NNN: see ReadPackage). The replacement is on the shader
// object only for the length of the game's bind (SetShadersHook, Render.cpp), as the menu's 3D view takes the game's pair; everything
// else still sees the old handle. A pair with one half this mod's is named in the log and left as it is (this mod's fault to fix).
#include <unordered_map>
#include <string>
#include <vector>
#include <mutex>
#include <d3dx9.h>
#include "InstallCheck.h"
#include "Paths.h"

namespace GameShaderPairs {
	inline bool Check = true;		// [Main.Main.Misc] GameShaderPairCheck
	inline bool Repair = true;		// [Main.Main.Misc] GameShaderPairRepair (on by default since 2026-11-13 b)
	struct Verdict {
		bool fits;
		IDirect3DVertexShader9* vertex;	// what Repair binds instead (nullptr: the bound one stays)
		IDirect3DPixelShader9* pixel;
		const char* how;				// the repair in words ("" = none found)
	};
	inline std::unordered_map<unsigned long long, Verdict> Pairs;
	inline unsigned long long LastKey = 0;
	inline const Verdict* Last = nullptr;
	inline unsigned Logged = 0, Mismatched = 0, RepairedBinds = 0;
	inline const char* BoundNote = "";	// for the FLASHLAMP look (core/FlashlightLamp.h): the pair of the last bind
	// the handles the repair put on the shader objects for the game's bind (AfterBind takes them off again)
	inline IDirect3DVertexShader9* HeldVertex = nullptr; inline IDirect3DPixelShader9* HeldPixel = nullptr;
	inline bool Holding = false;

	// The shader package: every entry's place in the file's bytes, by upper-case name.
	inline int PackageState = 0;	// 0 not read, 1 read, -1 not there
	inline std::vector<char> PackageBytes;
	inline std::unordered_map<std::string, std::pair<size_t, size_t>> PackageEntries;
	inline std::unordered_map<std::string, IUnknown*> PackageMade;	// the D3D shaders made from it

	inline std::string Upper(const char* name) { std::string s(name ? name : ""); for (char& c : s) c = (char)toupper((unsigned char)c); return s; }

	// Every shader the game makes (ShaderIO.cpp PrepareNewShader): its name and the game's own handle. The package the game really took
	// its shaders from is the one most of a sample of them are found in, byte for byte (the mod sets the "current package" number to 7
	// after the game chose its own, so that number alone does not say; and the packages differ: 7's lit shaders clamp the light at 1).
	struct Made { std::string name; IUnknown* handle; bool pixel; };
	inline std::vector<Made> MadeByGame;
	inline std::mutex MadeLock;
	inline void NoteMade(const char* name, IUnknown* handle, bool pixel) {
		if (!name || !handle) return;
		std::lock_guard<std::mutex> hold(MadeLock);
		if (MadeByGame.size() < 4096) MadeByGame.push_back({ Upper(name), handle, pixel });
	}
	// The game frees a shader object (ShaderRecord.cpp Free): its handle is not asked for its bytes afterwards.
	inline void Forget(const IUnknown* handle) {
		if (!handle) return;
		std::lock_guard<std::mutex> hold(MadeLock);
		for (size_t i = 0; i < MadeByGame.size(); i++) if (MadeByGame[i].handle == handle) { MadeByGame[i] = MadeByGame.back(); MadeByGame.pop_back(); break; }
	}
	template <typename Handle> inline bool Bytes(Handle* shader, std::vector<DWORD>& code, UINT* bytes = nullptr) {
		UINT size = 0;
		if (!shader || FAILED(shader->GetFunction(nullptr, &size)) || size < 8) return false;
		code.assign((size + 3) / 4, 0);
		if (bytes) *bytes = size;
		return SUCCEEDED(shader->GetFunction(code.data(), &size));
	}
	inline bool SameBytes(const char* packed, size_t packedSize, const std::vector<DWORD>& bound, size_t boundSize) {
		const size_t common = (std::min)(packedSize, boundSize);
		return common >= 8 && (std::max)(packedSize, boundSize) - common <= 4 && common <= bound.size() * 4 && !memcmp(packed, bound.data(), common);
	}
	inline bool ParsePackage(unsigned number, std::vector<char>& bytes, std::unordered_map<std::string, std::pair<size_t, size_t>>& entries) {
		bytes.clear(); entries.clear();
		char path[MAX_PATH];
		sprintf_s(path, "%s\\Data\\Shaders\\shaderpackage%03u.sdp", Paths::GameRoot().c_str(), number);
		FILE* in = _fsopen(path, "rb", _SH_DENYNO);
		if (!in) return false;
		fseek(in, 0, SEEK_END);
		const long size = ftell(in);
		fseek(in, 0, SEEK_SET);
		if (size > 12 && size < (64 << 20)) { bytes.resize((size_t)size); bytes.resize(fread(bytes.data(), 1, (size_t)size, in)); }
		fclose(in);
		if (bytes.size() < 12) return false;
		// the file: magic, entry count, data size; then per entry a 256-byte name, a length and the bytecode
		const UInt32 count = *(const UInt32*)(bytes.data() + 4);
		size_t at = 12;
		for (UInt32 i = 0; i < count && at + 260 <= bytes.size(); i++) {
			char name[257]; memcpy(name, bytes.data() + at, 256); name[256] = 0;
			const UInt32 length = *(const UInt32*)(bytes.data() + at + 256);
			if (at + 260 + (size_t)length > bytes.size()) break;
			entries[Upper(name)] = { at + 260, (size_t)length };
			at += 260 + (size_t)length;
		}
		return !entries.empty();
	}
	inline bool ReadPackage() {
		if (PackageState) return PackageState > 0;
		PackageState = -1;
		// The game's shaders (every one it made; many are the same bytes in several packages, so a few do not tell the packages apart)
		struct Sample { std::string name; std::vector<DWORD> code; UINT size; };
		std::vector<Sample> sample;
		{
			std::lock_guard<std::mutex> hold(MadeLock);
			sample.reserve(MadeByGame.size());
			for (const Made& made : MadeByGame) {
				Sample one; one.name = made.name; one.size = 0;
				const bool ok = made.pixel ? Bytes((IDirect3DPixelShader9*)made.handle, one.code, &one.size) : Bytes((IDirect3DVertexShader9*)made.handle, one.code, &one.size);
				if (ok) sample.push_back(std::move(one));
			}
		}
		const unsigned current = *(const UInt32*)0x011F91C0;	// BSShaderManager's current package (7 with this mod: Hooks/ShaderIO.cpp)
		unsigned best = 0, bestNumber = 0;
		std::string scores;
		std::vector<char> bytes; std::unordered_map<std::string, std::pair<size_t, size_t>> entries;
		for (int t = 0; t <= 30; t++) {
			const unsigned number = t == 0 ? current : (unsigned)(31 - t);	// the current one first (it wins a tie), then from the highest down
			if ((t > 0 && number == current) || !ParsePackage(number, bytes, entries)) continue;
			unsigned score = 0;
			for (const Sample& one : sample) {
				auto entry = entries.find(one.name);
				if (entry != entries.end() && SameBytes(bytes.data() + entry->second.first, entry->second.second, one.code, one.size)) score++;
			}
			char text[24]; sprintf_s(text, "%s%u: %u", scores.empty() ? "" : ", ", number, score); scores += text;
			if (!bestNumber || score > best) { best = score; bestNumber = number; PackageBytes.swap(bytes); PackageEntries.swap(entries); }
		}
		if (bestNumber) {
			PackageState = 1;
			Logger::Log("UNOFFICIAL GameShaderPairs: the game's shader package is Data\\Shaders\\shaderpackage%03u.sdp (%u shaders; %u of the %u shaders the game made are in it byte for byte, the rest are other mods' replacements or not from a package; the 'current package' number is %u; per package: %s)",
				bestNumber, (unsigned)PackageEntries.size(), best, (unsigned)sample.size(), current, scores.c_str());
		}
		else Logger::Log("UNOFFICIAL GameShaderPairs: the game's shader package could not be read: pairs that do not fit are only named");
		return PackageState > 0;
	}

	// The TEXCOORD indices a pixel shader reads / a vertex shader writes, as bits. False: D3DX could not say.
	inline bool Coords(const DWORD* code, bool pixel, unsigned& mask) {
		D3DXSEMANTIC list[MAXD3DDECLLENGTH];
		UINT count = 0;
		mask = 0;
		const HRESULT hr = pixel ? D3DXGetShaderInputSemantics(code, list, &count) : D3DXGetShaderOutputSemantics(code, list, &count);
		if (FAILED(hr)) return false;
		for (UINT i = 0; i < count && i < MAXD3DDECLLENGTH; i++) if (list[i].Usage == D3DDECLUSAGE_TEXCOORD && list[i].UsageIndex < 32) mask |= 1u << list[i].UsageIndex;
		return true;
	}
	// The package's own shader of this name: its coordinates, whether the handle's bytes are the same, and (made on demand) the D3D shader.
	struct FromPackage { bool found = false, same = false; unsigned mask = 0; const DWORD* code = nullptr; size_t size = 0; };
	inline FromPackage Look(const char* name, bool pixel, const std::vector<DWORD>& bound, size_t boundSize) {
		FromPackage r;
		if (!ReadPackage()) return r;
		auto entry = PackageEntries.find(Upper(name));
		if (entry == PackageEntries.end()) return r;
		r.code = (const DWORD*)(PackageBytes.data() + entry->second.first); r.size = entry->second.second;
		r.found = Coords(r.code, pixel, r.mask);
		r.same = SameBytes((const char*)r.code, r.size, bound, boundSize);
		return r;
	}
	inline IUnknown* Make(const char* name, bool pixel, const FromPackage& from) {
		const std::string key = Upper(name);
		auto made = PackageMade.find(key);
		if (made != PackageMade.end()) return made->second;
		IUnknown* shader = nullptr;
		if (pixel) { IDirect3DPixelShader9* p = nullptr; if (SUCCEEDED(TheRenderManager->device->CreatePixelShader(from.code, &p))) shader = p; }
		else { IDirect3DVertexShader9* v = nullptr; if (SUCCEEDED(TheRenderManager->device->CreateVertexShader(from.code, &v))) shader = v; }
		return PackageMade[key] = shader;
	}

	inline Verdict Judge(NiD3DVertexShaderEx* vs, NiD3DPixelShaderEx* ps) {
		Verdict verdict = { true, nullptr, nullptr, "" };
		std::vector<DWORD> vcode, pcode;
		unsigned vmask = 0, pmask = 0;
		UINT vsize = 0, psize = 0;
		if (!Bytes(vs->ShaderHandle, vcode, &vsize) || !Bytes(ps->ShaderHandle, pcode, &psize) || !Coords(vcode.data(), false, vmask) || !Coords(pcode.data(), true, pmask)) return verdict;
		if (!(pmask & ~vmask)) return verdict;
		verdict.fits = false;
		Mismatched++;
		const bool modVertex = (void*)vs->ShaderHandle != (void*)vs->ShaderHandleBackup, modPixel = (void*)ps->ShaderHandle != (void*)ps->ShaderHandleBackup;
		char missing[64] = "";
		for (unsigned i = 0; i < 16; i++) if ((pmask & ~vmask) & (1u << i)) { char one[8]; sprintf_s(one, "%s%u", missing[0] ? ", " : "", i); strcat_s(missing, one); }
		// One half is this mod's (its pixel shader behind a vertex shader it does not replace: SLS2000.pso behind the tree branches'
		// STB2004.vso, seen 2026-11-13; or its vertex shader left with a game pixel shader the driver refused): named once, as a note for
		// this mod's own upkeep. No repair, and the shader package is not read for it.
		if (modVertex || modPixel) {
			if (Logged < 24) {
				Logged++;
				Logger::Log("UNOFFICIAL GameShaderPairs (a note for this mod's upkeep, nothing to do for the player): the pixel shader %s (%s) reads TEXCOORD %s, which the vertex shader %s (%s) it is drawn with does not write.",
					ps->Name ? ps->Name : "?", modPixel ? "this mod's" : "the game's or another mod's", missing, vs->Name ? vs->Name : "?", modVertex ? "this mod's" : "the game's or another mod's");
			}
			return verdict;
		}
		// Neither half is this mod's. What the game made of each name: the package's own, or another mod's replacement of it
		IDirect3DVertexShader9* gameVertex = vs->ShaderHandle; IDirect3DPixelShader9* gamePixel = ps->ShaderHandle;
		const FromPackage pv = Look(vs->Name, false, vcode, vsize), pp = Look(ps->Name, true, pcode, psize);
		{	// the pair to draw with instead, the first that fits: the package's pixel shader, its vertex shader, both
			IDirect3DVertexShader9* vertex = nullptr; IDirect3DPixelShader9* pixel = nullptr;
			const char* how = "";
			if (pp.found && !pp.same && !(pp.mask & ~vmask)) { vertex = gameVertex; pixel = (IDirect3DPixelShader9*)Make(ps->Name, true, pp); how = "the shader package's pixel shader"; }
			else if (pv.found && !pv.same && !(pmask & ~pv.mask)) { vertex = (IDirect3DVertexShader9*)Make(vs->Name, false, pv); pixel = gamePixel; how = "the shader package's vertex shader"; }
			else if (pp.found && pv.found && !pp.same && !pv.same && !(pp.mask & ~pv.mask)) { vertex = (IDirect3DVertexShader9*)Make(vs->Name, false, pv); pixel = (IDirect3DPixelShader9*)Make(ps->Name, true, pp); how = "the shader package's pair"; }
			if (how[0] && vertex && pixel) {	// (not when the device would not make a package shader); only the halves that change
				verdict.vertex = vertex != vs->ShaderHandle ? vertex : nullptr;
				verdict.pixel = pixel != ps->ShaderHandle ? pixel : nullptr;
				verdict.how = how;
			}
		}
		auto whose = [](const FromPackage& from) { return !from.found ? "the game's, not found in its shader package" : from.same ? "the game's own" : "ANOTHER MOD'S replacement of the game's"; };
		if (Logged < 24) {
			Logged++;
			Logger::Log("[WARNING] UNOFFICIAL GameShaderPairs: the pixel shader %s (%s) reads TEXCOORD %s, which the vertex shader %s (%s) it is drawn with does not write: lamp light, highlights or parallax can be wrong or missing on what this pair draws. %s%s%s",
				ps->Name ? ps->Name : "?", whose(pp), missing, vs->Name ? vs->Name : "?", whose(pv),
				verdict.how[0] ? "[Main.Main.Misc] GameShaderPairRepair draws it with " : "No fitting pair found for a repair", verdict.how, verdict.how[0] ? " instead." : ".");
		}
		// Help > Install check: one of the halves is another mod's replacement (two mods' loose shader files mixed)
		if ((pp.found && !pp.same) || (pv.found && !pv.same)) {
			static unsigned told = 0;
			if (told < 6) {
				told++;
				InstallCheck::Note(InstallCheck::Level::Warning, "shaderpairs", std::string("Shader files of two mods that do not fit each other: ") + (ps->Name ? ps->Name : "?") + " is drawn with " + (vs->Name ? vs->Name : "?"),
					"Replacement shader files from different mods are mixed (Data\\Shaders\\Loose, loaded by Fallout Shader Loader): one mod's pixel shader is drawn with another's vertex shader, so lamps do not light what they draw while this mod's own shader for it is off (POM, PBR). Install the mod that pixel shader came from last, so that its own vertex shaders are back, or remove its shader files. Until then [Main.Main.Misc] GameShaderPairRepair (on by default) draws such a pair with the game's own shader, so lamps light it again.");
			}
		}
		return verdict;
	}

	// SetShadersHook, right before the game binds the pass's shaders.
	inline void BeforeBind(NiD3DVertexShaderEx* vs, NiD3DPixelShaderEx* ps) {
		BoundNote = "";
		if (!Check || !vs || !ps || !vs->ShaderHandle || !ps->ShaderHandle || !vs->ShaderHandleBackup || !ps->ShaderHandleBackup) return;
		if ((void*)vs->ShaderHandle != (void*)vs->ShaderHandleBackup && (void*)ps->ShaderHandle != (void*)ps->ShaderHandleBackup) return;	// both this mod's
		const unsigned long long key = ((unsigned long long)(UInt32)(uintptr_t)vs->ShaderHandle << 32) | (UInt32)(uintptr_t)ps->ShaderHandle;
		if (key != LastKey || !Last) {
			auto found = Pairs.find(key);
			if (found == Pairs.end()) {
				if (Pairs.size() >= 4096) return;
				// (the handles are kept alive while their verdict is: a freed shader's address could be handed out again)
				vs->ShaderHandle->AddRef(); ps->ShaderHandle->AddRef();
				found = Pairs.emplace(key, Judge(vs, ps)).first;
			}
			LastKey = key; Last = &found->second;
		}
		if (Last->fits) return;
		BoundNote = " [this pair does NOT fit]";
		if (!Repair || (!Last->vertex && !Last->pixel)) return;
		HeldVertex = vs->ShaderHandle; HeldPixel = ps->ShaderHandle; Holding = true;
		if (Last->vertex) vs->ShaderHandle = Last->vertex;
		if (Last->pixel) ps->ShaderHandle = Last->pixel;
		BoundNote = " [this pair did not fit: REPAIRED]";
		RepairedBinds++;
	}
	inline void AfterBind(NiD3DVertexShaderEx* vs, NiD3DPixelShaderEx* ps) {
		if (!Holding) return;
		Holding = false;
		if (vs) vs->ShaderHandle = HeldVertex;
		if (ps) ps->ShaderHandle = HeldPixel;
	}

	// ShaderManager::UpdateConstants (every frame; the keys are live)
	inline void ReadSettings() {
		SettingManager::Configuration::ConfigNode node;
		Check = !TheSettingManager->Config.FillNode(&node, "Main.Main.Misc", "GameShaderPairCheck") || node.BoolValue;	// a missing key: on
		Repair = !TheSettingManager->Config.FillNode(&node, "Main.Main.Misc", "GameShaderPairRepair") || node.BoolValue;	// (2026-11-13 b) a missing key: on
	}
}
