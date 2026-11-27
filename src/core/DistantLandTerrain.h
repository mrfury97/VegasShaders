#pragma once

// UNOFFICIAL Phase 1 Distant Land prototype (Task 1, docs/next/DISTANT-LAND-PLAN.md):
// VS-drawn 7-ring Geo-Clipmap distant terrain writing GBufferV2 + hardware reversed depth,
// plus Tier-1 vanilla terrain LOD takeover (BGSTerrainManager::GetRootLandLODNode APP_CULLED).
// Gated behind [Main.Develop.Main] DebugVar1 >= 1.5 (2 = lit distant terrain, 3 = ring-LOD debug view, 4 = terrain + forced night lights).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <future>
#include <string>
#include <vector>

#include "../effects/Terrain/LandHeights.h"
#include "../effects/Terrain/LodNifScanner.h"
#include "DeferredSun.h"
#include "DistantLandLightStore.h"
#include "DistantLandRocks.h"
#include "DistantLandTrees.h"
#include "RenderRect.h"

namespace DistantLandTerrain {
	struct GridVertex {
		float gx, gy;
	};

	struct LoadedWorld {
		void*								WorldSpace = nullptr;
		std::string							Name;
		std::string							Message;
		std::string							CachePath;
		std::string							NifNote;
		bool								LoadedFromCache = false;
		double								CacheWriteMB = 0.0;
		UINT								Width = 0, Height = 0;
		float								West = 0.0f, North = 0.0f, Texel = 128.0f;
		float								MinZ = 0.0f, MaxZ = 1000.0f;
		double								Seconds = 0.0;
		std::vector<float>					Heights;
		std::vector<uint32_t>				NormalRGBA;
		UINT								MacroW = 0, MacroH = 0;
		std::vector<uint32_t>				MacroRGBA;
		std::vector<LandHeights::FoliageRef>	Foliage;
		std::vector<LandHeights::RockRef>		Rocks;
		std::vector<LandHeights::NightLightRef>	NightLights;
	};

	static constexpr int   kGridHalf  = 64;
	static constexpr int   kGridSide  = kGridHalf * 2 + 1; // 129
	static constexpr UINT  kVertCount = (UINT)(kGridSide * kGridSide); // 16641 (fits uint16_t)
	static constexpr int   kHoleHalf  = 31; // kGridHalf/2 - 1 -> 1*step_k guaranteed overlap margin on all 4 sides
	static constexpr int   kNumRings  = 6;  // steps 128, 256, 512, 1024, 2048, 4096 -> +/-262,144 units (+/-64 cells)

	inline bool Initialized = false;
	inline bool Failed      = false;

	inline IDirect3DVertexShader9*      TerrainVS = nullptr;
	inline IDirect3DPixelShader9*       TerrainPS = nullptr;
	inline IDirect3DVertexDeclaration9* GridDecl  = nullptr;
	inline IDirect3DVertexBuffer9*      GridVB    = nullptr;
	inline IDirect3DIndexBuffer9*       IBFull    = nullptr;
	inline IDirect3DIndexBuffer9*       IBRing    = nullptr;
	inline UINT                         FullPrimCount = 0;
	inline UINT                         RingPrimCount = 0;

	// Async worldspace loader & GPU height/normal textures (D3DPOOL_DEFAULT, 0 MB resident CPU VA)
	inline void*                   RequestedWorld = nullptr;
	inline void*                   UploadedWorld  = nullptr;
	inline bool                    Loading        = false;
	inline std::future<LoadedWorld> LoadJob;

	inline IDirect3DTexture9*      HeightTex      = nullptr; // D3DFMT_R32F (vertex shader height fetch)
	inline IDirect3DTexture9*      NormalTex      = nullptr; // D3DFMT_A8R8G8B8 (pixel shader Sobel normal + slope + ridge)
	inline IDirect3DTexture9*      MacroAlbedoTex = nullptr; // D3DFMT_A8R8G8B8 (VCLR * LTEX or xLODGen macro diffuse)
	inline UINT                    MapWidth = 0, MapHeight = 0;
	inline float                   MapWest = 0.0f, MapNorth = 0.0f, MapTexel = 128.0f;
	inline float                   MapMinZ = 0.0f, MapMaxZ = 1000.0f;
	inline float                   MacroBlendWeight = 0.92f;

	inline bool                    VanillaHidden      = false;
	inline bool                    VanillaTreesHidden = false;

	inline void ReleaseWorldTextures() {
		if (HeightTex) { HeightTex->Release(); HeightTex = nullptr; }
		if (NormalTex) { NormalTex->Release(); NormalTex = nullptr; }
		if (MacroAlbedoTex) { MacroAlbedoTex->Release(); MacroAlbedoTex = nullptr; }
		DistantLandTrees::ReleaseInstances();
		DistantLandRocks::ReleaseInstances();
		DistantLandLightStore::Clear();
		UploadedWorld = nullptr;
		MapWidth = MapHeight = 0;
	}

	inline void SetVanillaLodHidden(bool hide, bool hideTrees = false);

	inline void OnDeviceLost() {
		SetVanillaLodHidden(false, false);
		ReleaseWorldTextures();
		RequestedWorld = nullptr;
	}

	inline void* ResolveLodWorldPtr(TESWorldSpace* world) {
		while (world && world->parent && (world->parentFlags & 1)) world = world->parent;
		return (void*)world;
	}

	inline bool ShouldSuppressVanillaTrees(TESWorldSpace* world) {
		if (DistantLandLightStore::EffectiveSuppressLevel() < 1) return false;
		if (DistantLandLightStore::EffectiveDistantLandMode() < 2) return false;
		void* resolved = ResolveLodWorldPtr(world);
		return resolved && UploadedWorld == resolved && HeightTex && NormalTex &&
			!DistantLandTrees::Failed && DistantLandTrees::InstanceCount > 0;
	}

	inline bool ShouldSuppressVanillaTerrain(TESWorldSpace* world) {
		if (DistantLandLightStore::EffectiveSuppressLevel() < 2) return false;
		if (DistantLandLightStore::EffectiveDistantLandMode() < 2) return false;
		void* resolved = ResolveLodWorldPtr(world);
		return resolved && UploadedWorld == resolved && HeightTex && NormalTex;
	}

	// Binary disk cache (Data\VegasShaders\Cache\DistantLand_<World>_<Hash>.bin) keyed by plugins + LOD Blocks mtime/sizes.
	static constexpr uint32_t kCacheMagic   = 0x4C445356u; // 'VSDL'
	static constexpr uint32_t kCacheVersion = 4u;

	struct DiskCacheHeader {
		uint32_t	magic;
		uint32_t	version;
		uint64_t	fingerprint;
		uint32_t	width, height;
		float		west, north, texel, minZ, maxZ;
		uint32_t	macroW, macroH;
		uint32_t	foliageCount;
		uint32_t	rockCount;
		uint32_t	nightLightCount;
		uint32_t	msgLen;
		uint32_t	nifNoteLen;
	};

	inline uint64_t Fnv1aMix(uint64_t h, const void* ptr, size_t n) {
		const uint8_t* p = (const uint8_t*)ptr;
		for (size_t i = 0; i < n; ++i) {
			h ^= (uint64_t)p[i];
			h *= 1099511628211ULL;
		}
		return h;
	}

	inline uint64_t ComputeCacheFingerprint(const std::vector<std::string>& pluginNames, const std::string& definingFile, UInt32 worldObjID, const std::string& worldName) {
		uint64_t h = 14695981039346656037ULL;
		const uint32_t ver = kCacheVersion;
		h = Fnv1aMix(h, &ver, sizeof(ver));
		h = Fnv1aMix(h, &worldObjID, sizeof(worldObjID));
		const std::string defLow = LodNifScanner::LowerAscii(definingFile);
		const std::string wrldLow = LodNifScanner::LowerAscii(worldName);
		h = Fnv1aMix(h, defLow.data(), defLow.size());
		h = Fnv1aMix(h, wrldLow.data(), wrldLow.size());

		for (const std::string& p : pluginNames) {
			const std::string low = LodNifScanner::LowerAscii(p);
			h = Fnv1aMix(h, low.data(), low.size());
			WIN32_FILE_ATTRIBUTE_DATA fad = {};
			if (GetFileAttributesExA(("Data\\" + p).c_str(), GetFileExInfoStandard, &fad)) {
				h = Fnv1aMix(h, &fad.nFileSizeHigh, sizeof(fad.nFileSizeHigh));
				h = Fnv1aMix(h, &fad.nFileSizeLow, sizeof(fad.nFileSizeLow));
				h = Fnv1aMix(h, &fad.ftLastWriteTime, sizeof(fad.ftLastWriteTime));
			}
		}

		// Fold LOD Blocks .nif count, total bytes, and newest mtime so xLODGen updates invalidate cache automatically.
		uint64_t nifCount = 0, nifBytes = 0, maxWriteTime = 0;
		if (!worldName.empty()) {
			const std::string pat = "Data\\Meshes\\Landscape\\LOD\\" + worldName + "\\Blocks\\*.Level4*.nif";
			WIN32_FIND_DATAA fd = {};
			HANDLE fh = FindFirstFileA(pat.c_str(), &fd);
			if (fh != INVALID_HANDLE_VALUE) {
				do {
					if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
					++nifCount;
					nifBytes += ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
					const uint64_t wt = ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
					if (wt > maxWriteTime) maxWriteTime = wt;
				} while (FindNextFileA(fh, &fd));
				FindClose(fh);
			}
		}
		h = Fnv1aMix(h, &nifCount, sizeof(nifCount));
		h = Fnv1aMix(h, &nifBytes, sizeof(nifBytes));
		h = Fnv1aMix(h, &maxWriteTime, sizeof(maxWriteTime));
		return h;
	}

	inline std::string SanitizeTag(const std::string& s, const char* fallback) {
		std::string r;
		for (char c : s) {
			if (isalnum((unsigned char)c) || c == '_' || c == '-') r.push_back(c);
			else r.push_back('_');
		}
		return r.empty() ? std::string(fallback) : r;
	}

	inline bool TryLoadBinaryCache(const std::string& path, uint64_t expectedHash, LoadedWorld& out) {
		FILE* f = nullptr;
		if (fopen_s(&f, path.c_str(), "rb") || !f) return false;
		DiskCacheHeader hdr = {};
		if (fread(&hdr, sizeof(hdr), 1, f) != 1 ||
			hdr.magic != kCacheMagic || hdr.version != kCacheVersion || hdr.fingerprint != expectedHash ||
			hdr.width < 2 || hdr.height < 2 || hdr.width > 8192 || hdr.height > 8192 ||
			hdr.macroW > 8192 || hdr.macroH > 8192 ||
			hdr.foliageCount > 262144 || hdr.rockCount > 65536 || hdr.nightLightCount > 4096 ||
			hdr.msgLen > 4096 || hdr.nifNoteLen > 4096) {
			fclose(f);
			return false;
		}

		const size_t nPix = (size_t)hdr.width * hdr.height;
		const size_t nMacro = (size_t)hdr.macroW * hdr.macroH;
		out.Width = hdr.width;
		out.Height = hdr.height;
		out.West = hdr.west;
		out.North = hdr.north;
		out.Texel = hdr.texel;
		out.MinZ = hdr.minZ;
		out.MaxZ = hdr.maxZ;
		out.MacroW = hdr.macroW;
		out.MacroH = hdr.macroH;

		out.Heights.resize(nPix);
		out.NormalRGBA.resize(nPix);
		out.MacroRGBA.resize(nMacro);
		out.Foliage.resize(hdr.foliageCount);
		out.Rocks.resize(hdr.rockCount);
		out.NightLights.resize(hdr.nightLightCount);
		out.Message.resize(hdr.msgLen);
		out.NifNote.resize(hdr.nifNoteLen);

		auto rd = [&](void* dst, size_t elemSz, size_t count) -> bool {
			return count == 0 || fread(dst, elemSz, count, f) == count;
		};
		const bool ok =
			rd(out.Heights.data(), sizeof(float), nPix) &&
			rd(out.NormalRGBA.data(), sizeof(uint32_t), nPix) &&
			rd(out.MacroRGBA.data(), sizeof(uint32_t), nMacro) &&
			rd(out.Foliage.data(), sizeof(LandHeights::FoliageRef), hdr.foliageCount) &&
			rd(out.Rocks.data(), sizeof(LandHeights::RockRef), hdr.rockCount) &&
			rd(out.NightLights.data(), sizeof(LandHeights::NightLightRef), hdr.nightLightCount) &&
			rd(&out.Message[0], 1, hdr.msgLen) &&
			rd(&out.NifNote[0], 1, hdr.nifNoteLen);
		fclose(f);

		if (!ok) {
			std::vector<float>().swap(out.Heights);
			std::vector<uint32_t>().swap(out.NormalRGBA);
			std::vector<uint32_t>().swap(out.MacroRGBA);
			std::vector<LandHeights::FoliageRef>().swap(out.Foliage);
			std::vector<LandHeights::RockRef>().swap(out.Rocks);
			std::vector<LandHeights::NightLightRef>().swap(out.NightLights);
			return false;
		}
		out.LoadedFromCache = true;
		out.CachePath = path;
		return true;
	}

	inline double WriteBinaryCache(const std::string& dataRoot, const std::string& path, uint64_t hash, const LoadedWorld& w) {
		CreateDirectoryA(dataRoot.c_str(), nullptr);
		const std::string cacheDir = dataRoot + "Cache";
		CreateDirectoryA(cacheDir.c_str(), nullptr);

		const std::string tmpPath = path + ".tmp";
		FILE* f = nullptr;
		if (fopen_s(&f, tmpPath.c_str(), "wb") || !f) return 0.0;

		DiskCacheHeader hdr = {};
		hdr.magic = kCacheMagic;
		hdr.version = kCacheVersion;
		hdr.fingerprint = hash;
		hdr.width = w.Width;
		hdr.height = w.Height;
		hdr.west = w.West;
		hdr.north = w.North;
		hdr.texel = w.Texel;
		hdr.minZ = w.MinZ;
		hdr.maxZ = w.MaxZ;
		hdr.macroW = w.MacroW;
		hdr.macroH = w.MacroH;
		hdr.foliageCount = (uint32_t)w.Foliage.size();
		hdr.rockCount = (uint32_t)w.Rocks.size();
		hdr.nightLightCount = (uint32_t)w.NightLights.size();
		hdr.msgLen = (uint32_t)(std::min)(w.Message.size(), (size_t)4096);
		hdr.nifNoteLen = (uint32_t)(std::min)(w.NifNote.size(), (size_t)4096);

		auto wr = [&](const void* src, size_t elemSz, size_t count) -> bool {
			return count == 0 || fwrite(src, elemSz, count, f) == count;
		};
		const bool ok =
			wr(&hdr, sizeof(hdr), 1) &&
			wr(w.Heights.data(), sizeof(float), w.Heights.size()) &&
			wr(w.NormalRGBA.data(), sizeof(uint32_t), w.NormalRGBA.size()) &&
			wr(w.MacroRGBA.data(), sizeof(uint32_t), w.MacroRGBA.size()) &&
			wr(w.Foliage.data(), sizeof(LandHeights::FoliageRef), hdr.foliageCount) &&
			wr(w.Rocks.data(), sizeof(LandHeights::RockRef), hdr.rockCount) &&
			wr(w.NightLights.data(), sizeof(LandHeights::NightLightRef), hdr.nightLightCount) &&
			wr(w.Message.data(), 1, hdr.msgLen) &&
			wr(w.NifNote.data(), 1, hdr.nifNoteLen);

		const long bytes = ftell(f);
		fclose(f);
		if (!ok) { DeleteFileA(tmpPath.c_str()); return 0.0; }
		if (!MoveFileExA(tmpPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
			DeleteFileA(tmpPath.c_str());
			return 0.0;
		}
		return (double)bytes / (1024.0 * 1024.0);
	}

	inline void SetVanillaLodHidden(bool hide, bool hideTrees) {
		NiNode* landLodRoot = BGSTerrainManager::GetRootLandLODNode();
		if (landLodRoot) {
			if (hide) {
				if ((landLodRoot->m_flags & NiAVObject::APP_CULLED) == 0) {
					landLodRoot->m_flags |= NiAVObject::APP_CULLED;
				}
				VanillaHidden = true;
			} else if (VanillaHidden) {
				landLodRoot->m_flags &= ~NiAVObject::APP_CULLED;
				VanillaHidden = false;
			}
		} else if (!hide) {
			VanillaHidden = false;
		}

		NiNode* treeLodRoot = BGSTerrainManager::GetRootTreeLODNode();
		if (hide && hideTrees) {
			if (treeLodRoot) {
				if ((treeLodRoot->m_flags & NiAVObject::APP_CULLED) == 0) {
					treeLodRoot->m_flags |= NiAVObject::APP_CULLED;
				}
				if (!VanillaTreesHidden) {
					static bool loggedTreeTakeover = false;
					if (!loggedTreeTakeover) {
						loggedTreeTakeover = true;
						Logger::Log("UNOFFICIAL DistantLandTrees: Tier-1 vanilla DISTTREE takeover active (BGSTerrainManager::spTreeNode APP_CULLED, %u instanced far trees)",
							DistantLandTrees::InstanceCount);
					}
					VanillaTreesHidden = true;
				}
			}
		} else if (VanillaTreesHidden) {
			if (treeLodRoot) {
				treeLodRoot->m_flags &= ~NiAVObject::APP_CULLED;
			}
			VanillaTreesHidden = false;
		}
	}

	// Composite real LOD diffuse DDS tiles (Data\Textures\Landscape\LOD\<World>\diffuse\*.level16.*.dds or *.level8.*.dds)
	// into LoadedWorld::MacroRGBA so worlds like Zion Canyon, Big MT, Divide, and Mojave match their authored LOD palette.
	// Verified on Zion & WastelandNV: FNV LOD tiles store X East->West (u = 1 - px/w) and Y South->North (py=0 at cy*4096).
	inline void LoadLodDiffuseIntoMacro(LoadedWorld& out) {
		if (out.Name.empty() || out.Width < 2 || out.Height < 2 || out.MacroW < 2 || out.MacroH < 2 ||
			out.MacroRGBA.size() != (size_t)out.MacroW * out.MacroH || out.Texel <= 0.0f) {
			return;
		}
		const std::string dir = "Data\\Textures\\Landscape\\LOD\\" + out.Name + "\\diffuse\\";
		auto findTiles = [&](int wantLevel) {
			std::vector<std::string> names;
			char pat[512];
			sprintf_s(pat, "%s*.level%d.*.dds", dir.c_str(), wantLevel);
			WIN32_FIND_DATAA fd = {};
			HANDLE fh = FindFirstFileA(pat, &fd);
			if (fh != INVALID_HANDLE_VALUE) {
				do {
					if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) names.emplace_back(fd.cFileName);
				} while (FindNextFileA(fh, &fd));
				FindClose(fh);
			}
			return names;
		};

		int level = 16;
		std::vector<std::string> files = findTiles(16);
		if (files.empty()) { level = 8; files = findTiles(8); }
		if (files.empty()) return;

		auto parseTileCoords = [](const std::string& fn, int& outLv, int& outCx, int& outCy) -> bool {
			const std::string low = LodNifScanner::LowerAscii(fn);
			const size_t lp = low.find(".level");
			if (lp == std::string::npos) return false;
			char* end = nullptr;
			const long lv = strtol(low.c_str() + lp + 6, &end, 10);
			if (!end || lv <= 0) return false;
			const char* xp = strstr(end, ".x");
			if (!xp) return false;
			const long cx = strtol(xp + 2, &end, 10);
			if (!end) return false;
			const char* yp = strstr(end, ".y");
			if (!yp) return false;
			const long cy = strtol(yp + 2, &end, 10);
			outLv = (int)lv; outCx = (int)cx; outCy = (int)cy;
			return true;
		};

		auto decodeRgb565 = [](uint16_t c, uint8_t& r, uint8_t& g, uint8_t& b) {
			r = (uint8_t)(((c >> 11) & 31u) * 255u / 31u);
			g = (uint8_t)(((c >> 5) & 63u) * 255u / 63u);
			b = (uint8_t)((c & 31u) * 255u / 31u);
		};

		const float worldSpanW = (float)out.Width * out.Texel;
		const float worldSpanH = (float)out.Height * out.Texel;
		const double macroScaleX = (double)out.Width / (double)out.MacroW;
		const double macroScaleY = (double)out.Height / (double)out.MacroH;
		std::vector<uint8_t> decodedRGB;
		std::vector<uint8_t> rawBuf;
		int loadedTiles = 0;

		for (const std::string& fn : files) {
			int lv = 0, cx = 0, cy = 0;
			if (!parseTileCoords(fn, lv, cx, cy)) continue;
			const std::string fullPath = dir + fn;
			FILE* fp = nullptr;
			if (fopen_s(&fp, fullPath.c_str(), "rb") || !fp) continue;
			uint8_t hdr[128] = {};
			if (fread(hdr, 1, 128, fp) != 128 || memcmp(hdr, "DDS ", 4) != 0) {
				fclose(fp);
				continue;
			}
			uint32_t ddsH = 0, ddsW = 0, fourcc = 0;
			memcpy(&ddsH, hdr + 12, 4);
			memcpy(&ddsW, hdr + 16, 4);
			memcpy(&fourcc, hdr + 84, 4);
			if (ddsW < 4 || ddsH < 4 || ddsW > 4096 || ddsH > 4096) { fclose(fp); continue; }

			const bool isDxt1 = (fourcc == 0x31545844u); // 'DXT1'
			const bool isDxt5 = (fourcc == 0x35545844u); // 'DXT5'
			if (!isDxt1 && !isDxt5) { fclose(fp); continue; }

			const UINT bx = (ddsW + 3u) / 4u, by = (ddsH + 3u) / 4u;
			const size_t blockSize = isDxt1 ? 8u : 16u;
			const size_t topBytes = (size_t)bx * by * blockSize;
			rawBuf.resize(topBytes);
			if (fread(rawBuf.data(), 1, topBytes, fp) != topBytes) { fclose(fp); continue; }
			fclose(fp);

			decodedRGB.resize((size_t)ddsW * ddsH * 3u);
			const uint8_t* src = rawBuf.data();
			for (UINT ty = 0; ty < by; ++ty) {
				for (UINT tx = 0; tx < bx; ++tx) {
					const uint8_t* blk = src + (isDxt1 ? 0u : 8u);
					src += blockSize;
					uint16_t c0 = 0, c1 = 0;
					uint32_t bits = 0;
					memcpy(&c0, blk, 2);
					memcpy(&c1, blk + 2, 2);
					memcpy(&bits, blk + 4, 4);
					uint8_t pal[4][3] = {};
					decodeRgb565(c0, pal[0][0], pal[0][1], pal[0][2]);
					decodeRgb565(c1, pal[1][0], pal[1][1], pal[1][2]);
					if (!isDxt1 || c0 > c1) {
						for (int ch = 0; ch < 3; ++ch) {
							pal[2][ch] = (uint8_t)((2u * pal[0][ch] + pal[1][ch]) / 3u);
							pal[3][ch] = (uint8_t)((pal[0][ch] + 2u * pal[1][ch]) / 3u);
						}
					} else {
						for (int ch = 0; ch < 3; ++ch) {
							pal[2][ch] = (uint8_t)((pal[0][ch] + pal[1][ch]) / 2u);
							pal[3][ch] = 0u;
						}
					}
					for (UINT py = 0; py < 4u; ++py) {
						const UINT y = ty * 4u + py;
						if (y >= ddsH) continue;
						for (UINT px = 0; px < 4u; ++px) {
							const UINT x = tx * 4u + px;
							if (x >= ddsW) continue;
							const uint32_t code = (bits >> (2u * (py * 4u + px))) & 3u;
							const size_t di = ((size_t)y * ddsW + x) * 3u;
							decodedRGB[di + 0] = pal[code][0];
							decodedRGB[di + 1] = pal[code][1];
							decodedRGB[di + 2] = pal[code][2];
						}
					}
				}
			}

			const float tileSpan = (float)lv * 4096.0f;
			const float tileX0 = (float)cx * 4096.0f;
			const float tileY0 = (float)cy * 4096.0f;
			const float tileX1 = tileX0 + tileSpan;
			const float tileY1 = tileY0 + tileSpan;
			const double mcStartD = ((double)tileX0 - out.West) / out.Texel + 0.5;
			const double mcEndD   = ((double)tileX1 - out.West) / out.Texel + 0.5;
			const double mrStartD = ((double)out.North - tileY1) / out.Texel + 0.5;
			const double mrEndD   = ((double)out.North - tileY0) / out.Texel + 0.5;
			const int mc0 = (std::max)(0, (int)floor(mcStartD / macroScaleX) - 1);
			const int mc1 = (std::min)((int)out.MacroW - 1, (int)ceil(mcEndD / macroScaleX) + 1);
			const int mr0 = (std::max)(0, (int)floor(mrStartD / macroScaleY) - 1);
			const int mr1 = (std::min)((int)out.MacroH - 1, (int)ceil(mrEndD / macroScaleY) + 1);
			if (mc0 > mc1 || mr0 > mr1) continue;
			(void)worldSpanW; (void)worldSpanH;

			const float invTileSpan = 1.0f / tileSpan;
			for (int mr = mr0; mr <= mr1; ++mr) {
				const float rowCenter = (float)((((double)mr + 0.5) * macroScaleY) - 0.5);
				const float wy = out.North - rowCenter * out.Texel;
				if (wy < tileY0 || wy >= tileY1) continue;
				const float fyNrm = std::clamp((wy - tileY0) * invTileSpan, 0.0f, 0.99999f);
				const float sy = std::clamp(fyNrm * (float)ddsH - 0.5f, 0.0f, (float)(ddsH - 1u));
				const int y0 = (int)floorf(sy), y1 = (std::min)((int)ddsH - 1, y0 + 1);
				const float wyf = sy - (float)y0;

				for (int mc = mc0; mc <= mc1; ++mc) {
					const float colCenter = (float)((((double)mc + 0.5) * macroScaleX) - 0.5);
					const float wx = out.West + colCenter * out.Texel;
					if (wx < tileX0 || wx >= tileX1) continue;
					const float fxNrm = std::clamp(1.0f - (wx - tileX0) * invTileSpan, 0.0f, 0.99999f);
					const float sx = std::clamp(fxNrm * (float)ddsW - 0.5f, 0.0f, (float)(ddsW - 1u));
					const int x0 = (int)floorf(sx), x1 = (std::min)((int)ddsW - 1, x0 + 1);
					const float wxf = sx - (float)x0;

					const size_t i00 = ((size_t)y0 * ddsW + x0) * 3u;
					const size_t i10 = ((size_t)y0 * ddsW + x1) * 3u;
					const size_t i01 = ((size_t)y1 * ddsW + x0) * 3u;
					const size_t i11 = ((size_t)y1 * ddsW + x1) * 3u;
					float rgb[3];
					for (int ch = 0; ch < 3; ++ch) {
						const float top = (float)decodedRGB[i00 + ch] * (1.0f - wxf) + (float)decodedRGB[i10 + ch] * wxf;
						const float bot = (float)decodedRGB[i01 + ch] * (1.0f - wxf) + (float)decodedRGB[i11 + ch] * wxf;
						rgb[ch] = top * (1.0f - wyf) + bot * wyf;
					}
					const uint32_t R = (uint32_t)std::clamp((int)lroundf(rgb[0]), 0, 255);
					const uint32_t G = (uint32_t)std::clamp((int)lroundf(rgb[1]), 0, 255);
					const uint32_t B = (uint32_t)std::clamp((int)lroundf(rgb[2]), 0, 255);
					out.MacroRGBA[(size_t)mr * out.MacroW + mc] = (255u << 24) | (R << 16) | (G << 8) | B;
				}
			}
			++loadedTiles;
		}
		if (loadedTiles > 0) {
			char note[96];
			sprintf_s(note, "diffuse DDS: %d level%d tiles", loadedTiles, level);
			if (!out.NifNote.empty()) out.NifNote += " | ";
			out.NifNote += note;
		}
	}

	inline LoadedWorld LoadWorker(std::vector<std::string> pluginNames, std::string definingFile, UInt32 worldObjID, void* worldPtr, std::string worldName, std::string dataRoot) {
		LoadedWorld out;
		out.WorldSpace = worldPtr;
		out.Name = std::move(worldName);

		const auto t0 = std::chrono::steady_clock::now();
		const uint64_t fp = ComputeCacheFingerprint(pluginNames, definingFile, worldObjID, out.Name);
		char cacheBuf[512];
		sprintf_s(cacheBuf, "%sCache\\DistantLand_%s_%06X_%016llX.bin",
			dataRoot.c_str(), SanitizeTag(out.Name, "World").c_str(), (unsigned)(worldObjID & 0xFFFFFFu), (unsigned long long)fp);
		const std::string cachePath = cacheBuf;

		if (TryLoadBinaryCache(cachePath, fp, out)) {
			out.Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
			return out;
		}

		std::vector<LandHeights::Plugin> plugins;
		plugins.reserve(pluginNames.size());
		for (const std::string& p : pluginNames) plugins.push_back({ p });

		LandHeights::Result land;
		LandHeights::ExtraOut extra;
		if (!LandHeights::ReadEx(plugins, definingFile, worldObjID, 8192, land, &extra) || land.Width < 2 || land.Height < 2 || land.Heights.size() != (size_t)land.Width * land.Height) {
			out.Message = land.Message.empty() ? "no landscape records found" : land.Message;
			out.Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
			return out;
		}
		std::vector<float>().swap(land.Peaks);

		out.Width  = land.Width;
		out.Height = land.Height;
		out.West   = land.West;
		out.North  = land.North;
		out.Texel  = land.Texel;
		out.MinZ   = land.Low;
		out.MaxZ   = land.High;
		out.Message = std::move(land.Message);
		out.Heights = std::move(land.Heights);
		out.MacroW = extra.MacroW;
		out.MacroH = extra.MacroH;
		out.MacroRGBA = std::move(extra.MacroAlbedoRGBA);
		out.Foliage = std::move(extra.Foliage);
		out.Rocks = std::move(extra.Rocks);
		out.NightLights = LodNifScanner::ScanAndBuildNightLights(out.Name, extra.NightLights, out.NifNote);
		std::vector<LandHeights::NightLightRef>().swap(extra.NightLights);
		LoadLodDiffuseIntoMacro(out);

		const UINT w = out.Width, h = out.Height;
		out.NormalRGBA.resize((size_t)w * h);

		auto sampleH = [&](int r, int c) -> float {
			r = std::clamp(r, 0, (int)h - 1);
			c = std::clamp(c, 0, (int)w - 1);
			return out.Heights[(size_t)r * w + c];
		};

		const float inv8Texel = out.Texel > 1.0f ? 1.0f / (8.0f * out.Texel) : (1.0f / (8.0f * 128.0f));
		for (UINT r = 0; r < h; ++r) {
			for (UINT c = 0; c < w; ++c) {
				const int ri = (int)r, ci = (int)c;
				const float h00 = sampleH(ri - 1, ci - 1), h01 = sampleH(ri - 1, ci), h02 = sampleH(ri - 1, ci + 1);
				const float h10 = sampleH(ri,     ci - 1), h11 = sampleH(ri,     ci), h12 = sampleH(ri,     ci + 1);
				const float h20 = sampleH(ri + 1, ci - 1), h21 = sampleH(ri + 1, ci), h22 = sampleH(ri + 1, ci + 1);

				// Row 0 = north (+Y), Col 0 = west (-X): dh/dx east, dh/dy north.
				const float dhdx = ((h02 + 2.0f * h12 + h22) - (h00 + 2.0f * h10 + h20)) * inv8Texel;
				const float dhdy = ((h00 + 2.0f * h01 + h02) - (h20 + 2.0f * h21 + h22)) * inv8Texel;

				float nx = -dhdx, ny = -dhdy, nz = 1.0f;
				const float invLen = 1.0f / sqrtf(nx * nx + ny * ny + nz * nz);
				nx *= invLen; ny *= invLen; nz *= invLen;

				const float avg4 = 0.25f * (h01 + h21 + h10 + h12);
				const float ridge = std::clamp((h11 - avg4) / 180.0f, -1.0f, 1.0f);
				const float slope = std::clamp(1.0f - nz, 0.0f, 1.0f);

				const uint32_t R = (uint32_t)std::clamp((int)lroundf((nx * 0.5f + 0.5f) * 255.0f), 0, 255);
				const uint32_t G = (uint32_t)std::clamp((int)lroundf((ny * 0.5f + 0.5f) * 255.0f), 0, 255);
				const uint32_t B = (uint32_t)std::clamp((int)lroundf(slope * 255.0f), 0, 255);
				const uint32_t A = (uint32_t)std::clamp((int)lroundf((ridge * 0.5f + 0.5f) * 255.0f), 0, 255);
				out.NormalRGBA[(size_t)r * w + c] = (A << 24) | (R << 16) | (G << 8) | B;
			}
		}

		out.CachePath = cachePath;
		out.CacheWriteMB = WriteBinaryCache(dataRoot, cachePath, fp, out);
		out.Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		return out;
	}

	inline bool UploadLoadedWorld(IDirect3DDevice9* d, LoadedWorld& loaded) {
		ReleaseWorldTextures();
		if (loaded.Width < 2 || loaded.Height < 2 || loaded.Heights.empty() || loaded.NormalRGBA.empty()) return false;

		const UINT w = loaded.Width, h = loaded.Height;
		if (FAILED(d->CreateTexture(w, h, 1, 0, D3DFMT_R32F, D3DPOOL_DEFAULT, &HeightTex, nullptr)) ||
			FAILED(d->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &NormalTex, nullptr))) {
			ReleaseWorldTextures();
			std::vector<float>().swap(loaded.Heights);
			std::vector<uint32_t>().swap(loaded.NormalRGBA);
			Logger::Log("[ERROR] UNOFFICIAL DistantLandTerrain: failed to allocate %ux%u R32F/A8R8G8B8 GPU textures", w, h);
			return false;
		}

		// Upload one staging texture at a time and release CPU buffers immediately to minimize transient 32-bit VA peak.
		HRESULT hrH = E_FAIL;
		{
			IDirect3DTexture9* sysH = nullptr;
			if (SUCCEEDED(d->CreateTexture(w, h, 1, 0, D3DFMT_R32F, D3DPOOL_SYSTEMMEM, &sysH, nullptr)) && sysH) {
				D3DLOCKED_RECT lr = {};
				if (SUCCEEDED(sysH->LockRect(0, &lr, nullptr, 0))) {
					for (UINT r = 0; r < h; ++r) {
						memcpy((uint8_t*)lr.pBits + (size_t)r * lr.Pitch, &loaded.Heights[(size_t)r * w], (size_t)w * sizeof(float));
					}
					sysH->UnlockRect(0);
				}
				hrH = d->UpdateTexture(sysH, HeightTex);
				sysH->Release();
			}
			std::vector<float>().swap(loaded.Heights);
		}

		HRESULT hrN = E_FAIL;
		if (SUCCEEDED(hrH)) {
			IDirect3DTexture9* sysN = nullptr;
			if (SUCCEEDED(d->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sysN, nullptr)) && sysN) {
				D3DLOCKED_RECT lr = {};
				if (SUCCEEDED(sysN->LockRect(0, &lr, nullptr, 0))) {
					for (UINT r = 0; r < h; ++r) {
						memcpy((uint8_t*)lr.pBits + (size_t)r * lr.Pitch, &loaded.NormalRGBA[(size_t)r * w], (size_t)w * sizeof(uint32_t));
					}
					sysN->UnlockRect(0);
				}
				hrN = d->UpdateTexture(sysN, NormalTex);
				sysN->Release();
			}
		}
		std::vector<uint32_t>().swap(loaded.NormalRGBA);

		// Low-VA guardrail: keep HeightTex + NormalTex lean if free 32-bit virtual address space drops below 500 MB.
		MEMORYSTATUSEX vaStat = {};
		vaStat.dwLength = sizeof(vaStat);
		bool lowVa = false;
		double usedVaMB = 0.0, freeVaMB = 0.0;
		if (GlobalMemoryStatusEx(&vaStat)) {
			usedVaMB = (double)(vaStat.ullTotalVirtual - vaStat.ullAvailVirtual) / (1024.0 * 1024.0);
			freeVaMB = (double)vaStat.ullAvailVirtual / (1024.0 * 1024.0);
			lowVa = vaStat.ullAvailVirtual < (500ULL * 1024ULL * 1024ULL);
			if (lowVa) {
				static bool loggedLowVa = false;
				if (!loggedLowVa) {
					loggedLowVa = true;
					Logger::Log("[WARNING] UNOFFICIAL DistantLandTerrain: low 32-bit free VA (%.0f MB free, %.0f MB used); skipping MacroAlbedoTex", freeVaMB, usedVaMB);
				}
			}
		}

		// Upload MacroAlbedoTex (half-res VCLR * LTEX or fallback) and GPU-instanced distant foliage
		const UINT mw = (!lowVa && loaded.MacroW >= 2 && loaded.MacroH >= 2 && !loaded.MacroRGBA.empty()) ? loaded.MacroW : 2u;
		const UINT mh = (!lowVa && loaded.MacroW >= 2 && loaded.MacroH >= 2 && !loaded.MacroRGBA.empty()) ? loaded.MacroH : 2u;
		HRESULT hrM = E_FAIL;
		if (SUCCEEDED(hrH) && SUCCEEDED(hrN) && SUCCEEDED(d->CreateTexture(mw, mh, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &MacroAlbedoTex, nullptr)) && MacroAlbedoTex) {
			IDirect3DTexture9* sysM = nullptr;
			if (SUCCEEDED(d->CreateTexture(mw, mh, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sysM, nullptr)) && sysM) {
				D3DLOCKED_RECT lr = {};
				if (SUCCEEDED(sysM->LockRect(0, &lr, nullptr, 0))) {
					if (!lowVa && !loaded.MacroRGBA.empty()) {
						for (UINT r = 0; r < mh; ++r) {
							memcpy((uint8_t*)lr.pBits + (size_t)r * lr.Pitch, &loaded.MacroRGBA[(size_t)r * mw], (size_t)mw * sizeof(uint32_t));
						}
					} else {
						const uint32_t defCol = (0u << 24) | (132u << 16) | (114u << 8) | 88u;
						for (UINT r = 0; r < mh; ++r) {
							uint32_t* rowPtr = (uint32_t*)((uint8_t*)lr.pBits + (size_t)r * lr.Pitch);
							for (UINT c = 0; c < mw; ++c) rowPtr[c] = defCol;
						}
					}
					sysM->UnlockRect(0);
				}
				hrM = d->UpdateTexture(sysM, MacroAlbedoTex);
				sysM->Release();
			}
		}
		MacroBlendWeight = (!lowVa && !loaded.MacroRGBA.empty() && SUCCEEDED(hrM)) ? 0.92f : 0.0f;
		std::vector<uint32_t>().swap(loaded.MacroRGBA);

		if (FAILED(hrH) || FAILED(hrN)) {
			std::vector<LandHeights::FoliageRef>().swap(loaded.Foliage);
			std::vector<LandHeights::RockRef>().swap(loaded.Rocks);
			std::vector<LandHeights::NightLightRef>().swap(loaded.NightLights);
			ReleaseWorldTextures();
			Logger::Log("[ERROR] UNOFFICIAL DistantLandTerrain: UpdateTexture failed (%08lX / %08lX)", hrH, hrN);
			return false;
		}

		UploadedWorld = loaded.WorldSpace;
		MapWidth  = w;
		MapHeight = h;
		MapWest   = loaded.West;
		MapNorth  = loaded.North;
		MapTexel  = loaded.Texel;
		MapMinZ   = loaded.MinZ;
		MapMaxZ   = loaded.MaxZ;

		const UINT folCount = (UINT)loaded.Foliage.size();
		const UINT rockCount = (UINT)loaded.Rocks.size();
		const UINT lightCount = (UINT)loaded.NightLights.size();
		DistantLandTrees::UploadInstances(d, loaded.Foliage);
		DistantLandRocks::UploadInstances(d, loaded.Rocks);
		DistantLandLightStore::Set(loaded.WorldSpace, std::move(loaded.NightLights));

		if (loaded.LoadedFromCache) {
			Logger::Log("UNOFFICIAL DistantLandTerrain: loaded from disk cache %s in %.3f s -> %ux%u height+normal + %ux%u macro albedo + %u trees + %u rocks + %u night lights for %s (texel %.0f, Z [%.0f..%.0f], VA %.0f used / %.0f MB free)%s%s",
				loaded.CachePath.c_str(), loaded.Seconds, w, h, mw, mh, folCount, rockCount, lightCount,
				loaded.Name.empty() ? "worldspace" : loaded.Name.c_str(), loaded.Texel, loaded.MinZ, loaded.MaxZ, usedVaMB, freeVaMB,
				loaded.NifNote.empty() ? "" : " | ", loaded.NifNote.c_str());
		} else {
			Logger::Log("UNOFFICIAL DistantLandTerrain: baked in %.2f s%s (%s) -> %ux%u height+normal + %ux%u macro albedo + %u trees + %u rocks + %u night lights for %s (texel %.0f, Z [%.0f..%.0f], VA %.0f used / %.0f MB free): %s | %s",
				loaded.Seconds, loaded.CacheWriteMB > 0.0 ? ", wrote disk cache" : "",
				loaded.CachePath.c_str(), w, h, mw, mh, folCount, rockCount, lightCount,
				loaded.Name.empty() ? "worldspace" : loaded.Name.c_str(), loaded.Texel, loaded.MinZ, loaded.MaxZ, usedVaMB, freeVaMB,
				loaded.Message.c_str(), loaded.NifNote.c_str());
		}
		return true;
	}

	inline void EnsureWorldHeightMap(IDirect3DDevice9* d) {
		TESWorldSpace* world = Player ? Player->GetWorldSpace() : nullptr;
		if (!world) return;
		while (world->parent && (world->parentFlags & 1)) world = world->parent;

		if (Loading && LoadJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
			LoadedWorld loaded = LoadJob.get();
			Loading = false;
			if (!UploadLoadedWorld(d, loaded)) {
				Logger::Log("[WARNING] UNOFFICIAL DistantLandTerrain: %s: %s",
					loaded.Name.empty() ? "worldspace" : loaded.Name.c_str(), loaded.Message.c_str());
			}
		}

		if (!Loading && RequestedWorld != world) {
			RequestedWorld = world;
			std::vector<std::string> plugins;
			std::string definingFile;
			if (DataHandler) {
				const ModList& mods = DataHandler->modList;
				const UInt32 count = (std::min)(mods.loadedModCount, (UInt32)0xFF);
				for (UInt32 i = 0; i < count; ++i) {
					if (mods.loadedMods[i]) plugins.emplace_back(mods.loadedMods[i]->name);
				}
				const UInt32 idx = world->refID >> 24;
				if (idx < count && mods.loadedMods[idx]) definingFile = mods.loadedMods[idx]->name;
			}
			const char* edName = world->GetEditorName();
			std::string worldName = edName ? edName : "";
			std::string dataRoot = Paths::DataRoot();
			try {
				LoadJob = std::async(std::launch::async, LoadWorker, std::move(plugins), std::move(definingFile), world->refID & 0xFFFFFFu, (void*)world, std::move(worldName), std::move(dataRoot));
				Loading = true;
			} catch (const std::exception& e) {
				Logger::Log("[ERROR] UNOFFICIAL DistantLandTerrain: could not start heightmap loader (%s)", e.what());
			}
		}
	}

	inline bool CompileShaders(IDirect3DDevice9* d) {
		const std::string path = std::string(Paths::ShadersDir()) + "DistantLandTerrain.hlsl";
		auto compile = [&](const char* entry, const char* target, ID3DXBuffer** code) -> bool {
			ID3DXBuffer* err = nullptr;
			const HRESULT hr = D3DXCompileShaderFromFileA(path.c_str(), nullptr, nullptr, entry, target, 0, code, &err, nullptr);
			if (FAILED(hr) || !*code) {
				Logger::Log("[ERROR] UNOFFICIAL DistantLandTerrain: %s (%s) compile failed in %s (%s)",
					entry, target, path.c_str(), err ? (const char*)err->GetBufferPointer() : "no compiler output");
				if (err) err->Release();
				return false;
			}
			if (err) err->Release();
			return true;
		};
		ID3DXBuffer* vsBuf = nullptr;
		ID3DXBuffer* psBuf = nullptr;
		if (!compile("TerrainVS", "vs_3_0", &vsBuf) || !compile("TerrainPS", "ps_3_0", &psBuf)) {
			if (vsBuf) vsBuf->Release();
			if (psBuf) psBuf->Release();
			return false;
		}
		const HRESULT hr1 = d->CreateVertexShader((const DWORD*)vsBuf->GetBufferPointer(), &TerrainVS);
		const HRESULT hr2 = d->CreatePixelShader((const DWORD*)psBuf->GetBufferPointer(), &TerrainPS);
		vsBuf->Release();
		psBuf->Release();
		if (FAILED(hr1) || FAILED(hr2) || !TerrainVS || !TerrainPS) {
			Logger::Log("[ERROR] UNOFFICIAL DistantLandTerrain: CreateVertexShader/PixelShader failed (%08lX %08lX)", hr1, hr2);
			return false;
		}
		return true;
	}

	inline bool InitResources(IDirect3DDevice9* d) {
		if (Initialized) return !Failed;
		Initialized = true;
		if (!CompileShaders(d)) { Failed = true; return false; }

		static const D3DVERTEXELEMENT9 kDecl[] = {
			{ 0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
			D3DDECL_END()
		};
		if (FAILED(d->CreateVertexDeclaration(kDecl, &GridDecl)) || !GridDecl) {
			Logger::Log("[ERROR] UNOFFICIAL DistantLandTerrain: CreateVertexDeclaration failed");
			Failed = true; return false;
		}

		std::vector<GridVertex> verts(kVertCount);
		for (int j = 0; j < kGridSide; ++j) {
			for (int i = 0; i < kGridSide; ++i) {
				verts[(size_t)j * kGridSide + i] = { (float)(i - kGridHalf), (float)(j - kGridHalf) };
			}
		}
		const UINT vbBytes = kVertCount * (UINT)sizeof(GridVertex);
		void* ptr = nullptr;
		if (FAILED(d->CreateVertexBuffer(vbBytes, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &GridVB, nullptr)) || !GridVB ||
			FAILED(GridVB->Lock(0, vbBytes, &ptr, 0))) {
			Logger::Log("[ERROR] UNOFFICIAL DistantLandTerrain: CreateVertexBuffer failed");
			Failed = true; return false;
		}
		memcpy(ptr, verts.data(), vbBytes);
		GridVB->Unlock();

		std::vector<uint16_t> idxFull, idxRing;
		idxFull.reserve(64 * 64 * 6);
		idxRing.reserve(64 * 64 * 6);
		for (int qy = -kGridHalf; qy < kGridHalf; ++qy) {
			for (int qx = -kGridHalf; qx < kGridHalf; ++qx) {
				const uint16_t v00 = (uint16_t)((qy + kGridHalf) * kGridSide + (qx + kGridHalf));
				const uint16_t v10 = (uint16_t)(v00 + 1);
				const uint16_t v01 = (uint16_t)(v00 + kGridSide);
				const uint16_t v11 = (uint16_t)(v01 + 1);

				auto emit = [&](std::vector<uint16_t>& dst) {
					// D3D9 left-handed screen space with D3DCULL_CCW keeps clockwise (+det) front faces.
					if (((qx + kGridHalf) ^ (qy + kGridHalf)) & 1) {
						dst.push_back(v00); dst.push_back(v11); dst.push_back(v10);
						dst.push_back(v00); dst.push_back(v01); dst.push_back(v11);
					} else {
						dst.push_back(v00); dst.push_back(v01); dst.push_back(v10);
						dst.push_back(v10); dst.push_back(v01); dst.push_back(v11);
					}
				};
				emit(idxFull);
				if (!(qx >= -kHoleHalf && qx < kHoleHalf && qy >= -kHoleHalf && qy < kHoleHalf)) {
					emit(idxRing);
				}
			}
		}

		FullPrimCount = (UINT)(idxFull.size() / 3);
		RingPrimCount = (UINT)(idxRing.size() / 3);
		auto makeIB = [&](const std::vector<uint16_t>& src, IDirect3DIndexBuffer9*& dst) -> bool {
			const UINT bytes = (UINT)(src.size() * sizeof(uint16_t));
			void* ip = nullptr;
			if (FAILED(d->CreateIndexBuffer(bytes, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &dst, nullptr)) || !dst ||
				FAILED(dst->Lock(0, bytes, &ip, 0))) return false;
			memcpy(ip, src.data(), bytes);
			dst->Unlock();
			return true;
		};
		if (!makeIB(idxFull, IBFull) || !makeIB(idxRing, IBRing)) {
			Logger::Log("[ERROR] UNOFFICIAL DistantLandTerrain: CreateIndexBuffer failed");
			Failed = true; return false;
		}

		Logger::Log("UNOFFICIAL DistantLandTerrain: Geo-Clipmap ready (%d rings, %u full / %u ring tris)", kNumRings, FullPrimCount, RingPrimCount);
		return true;
	}

	inline const D3DXVECTOR4* FindConstant(const char* name) {
		if (!TheShaderManager) return nullptr;
		const auto it = TheShaderManager->ConstantsTable.find(name);
		return (it != TheShaderManager->ConstantsTable.end()) ? it->second : nullptr;
	}

	inline void Render(IDirect3DDevice9* d) {
		if (!d || Failed) { SetVanillaLodHidden(false); return; }

		const int mode = DistantLandLightStore::EffectiveDistantLandMode();
		if (mode >= 1 && !CapturedNormals::VmSpan && TheShaderManager && TheShaderManager->GameState.isExterior) {
			EnsureWorldHeightMap(d);
		}
		const bool wanted =
			mode >= 2 &&
			!CapturedNormals::VmSpan &&
			TheShaderManager && TheShaderManager->GameState.isExterior &&
			DeferredSun::Wanted() &&
			CapturedNormals::AlbedoSurface && CapturedNormals::MaterialSurface && CapturedNormals::TextureSurface;

		if (!wanted) {
			SetVanillaLodHidden(false);
			return;
		}
		if (!HeightTex || !NormalTex || !InitResources(d)) {
			SetVanillaLodHidden(false);
			return;
		}

		IDirect3DSurface9* target = nullptr;
		if (FAILED(d->GetRenderTarget(0, &target)) || !target) {
			SetVanillaLodHidden(false);
			return;
		}
		D3DSURFACE_DESC desc = {};
		if (FAILED(target->GetDesc(&desc)) || desc.MultiSampleType != D3DMULTISAMPLE_NONE) {
			target->Release();
			SetVanillaLodHidden(false);
			return;
		}

		if (desc.Width == 0 || desc.Height == 0) {
			target->Release();
			SetVanillaLodHidden(false);
			return;
		}

		// Hide vanilla distant terrain LOD (SLS2003) and distant trees (DISTTREE) cleanly while keeping buildings/water intact.
		const bool hideTrees = !DistantLandTrees::Failed && DistantLandTrees::InstVB && DistantLandTrees::InstanceCount > 0 && DistantLandTrees::InitResources(d);
		SetVanillaLodHidden(true, hideTrees);

		{
			static GpuTimer timer("DistantLand terrain", 12);
			GpuProfileScope gpu(timer, d);

			// Save full D3D9 device state
			D3DVIEWPORT9 savedVP = {};
			d->GetViewport(&savedVP);
			IDirect3DSurface9* savedRT[4] = {};
			for (DWORD i = 0; i < 4; ++i) d->GetRenderTarget(i, &savedRT[i]);
			IDirect3DVertexShader9* savedVS = nullptr; d->GetVertexShader(&savedVS);
			IDirect3DPixelShader9* savedPS = nullptr; d->GetPixelShader(&savedPS);
			IDirect3DVertexDeclaration9* savedDecl = nullptr; d->GetVertexDeclaration(&savedDecl);
			DWORD savedFVF = 0; d->GetFVF(&savedFVF);
			IDirect3DVertexBuffer9* savedVB = nullptr; UINT savedOff = 0, savedStr = 0;
			d->GetStreamSource(0, &savedVB, &savedOff, &savedStr);
			IDirect3DIndexBuffer9* savedIB = nullptr; d->GetIndices(&savedIB);

			IDirect3DBaseTexture9* savedTex0 = nullptr; d->GetTexture(0, &savedTex0);
			IDirect3DBaseTexture9* savedTex1 = nullptr; d->GetTexture(1, &savedTex1);
			IDirect3DBaseTexture9* savedVTF  = nullptr; d->GetTexture(D3DVERTEXTEXTURESAMPLER0, &savedVTF);
			static const D3DSAMPLERSTATETYPE kSS[] = { D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE };
			DWORD savedSS0[6] = {}, savedSS1[6] = {};
			for (int i = 0; i < 6; ++i) {
				d->GetSamplerState(0, kSS[i], &savedSS0[i]);
				d->GetSamplerState(1, kSS[i], &savedSS1[i]);
			}
			DWORD savedSSV[6] = {};
			bool haveSSV[6] = {};
			for (int i = 0; i < 6; ++i) haveSSV[i] = SUCCEEDED(d->GetSamplerState(D3DVERTEXTEXTURESAMPLER0, kSS[i], &savedSSV[i]));

			float savedVSC[13][4] = {};
			d->GetVertexShaderConstantF(0, &savedVSC[0][0], 13);
			float savedPSC[19][4] = {};
			d->GetPixelShaderConstantF(0, &savedPSC[0][0], 19);

			static const D3DRENDERSTATETYPE kRS[] = {
				D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC,
				D3DRS_ALPHABLENDENABLE, D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_ALPHATESTENABLE,
				D3DRS_STENCILENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_CULLMODE,
				D3DRS_COLORWRITEENABLE, D3DRS_COLORWRITEENABLE1, D3DRS_COLORWRITEENABLE2, D3DRS_COLORWRITEENABLE3,
				D3DRS_FILLMODE, D3DRS_SRGBWRITEENABLE, D3DRS_FOGENABLE
			};
			DWORD savedRS[sizeof(kRS) / sizeof(kRS[0])] = {};
			for (size_t i = 0; i < sizeof(kRS) / sizeof(kRS[0]); ++i) d->GetRenderState(kRS[i], &savedRS[i]);

			// Bind GBufferV2 targets (keep hardware depth-stencil bound for early-Z + reversed depth write)
			d->SetRenderTarget(0, target);
			d->SetRenderTarget(1, CapturedNormals::AlbedoSurface);
			d->SetRenderTarget(2, CapturedNormals::MaterialSurface);
			d->SetRenderTarget(3, CapturedNormals::TextureSurface);

			const D3DVIEWPORT9 vp = {
				savedVP.X,
				savedVP.Y,
				savedVP.Width ? savedVP.Width : desc.Width,
				savedVP.Height ? savedVP.Height : desc.Height,
				0.0f,
				1.0f
			};
			d->SetViewport(&vp);

			const DWORD zFunc = (NiDX9Renderer::GetSingleton()->m_fZClear < 0.5f) ? D3DCMP_GREATEREQUAL : D3DCMP_LESSEQUAL;
			d->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
			d->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
			d->SetRenderState(D3DRS_ZFUNC, zFunc);
			d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
			d->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
			d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
			d->SetRenderState(D3DRS_STENCILENABLE, FALSE);
			d->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
			d->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
			d->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0Fu);
			d->SetRenderState(D3DRS_COLORWRITEENABLE1, 0x0Fu);
			d->SetRenderState(D3DRS_COLORWRITEENABLE2, 0x0Fu);
			d->SetRenderState(D3DRS_COLORWRITEENABLE3, 0x0Fu);
			d->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
			d->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
			d->SetRenderState(D3DRS_FOGENABLE, FALSE);

			const D3DXMATRIX& iv = TheRenderManager->InvViewMatrix;
			const D3DXMATRIX& p = NiDX9Renderer::GetSingleton()->projMatrix;
			const D3DXVECTOR4& cam = TheRenderManager->CameraPosition;

			float minX = cam.x - 10240.0f, maxX = cam.x + 10240.0f;
			float minY = cam.y - 10240.0f, maxY = cam.y + 10240.0f;
			uint32_t loadedBits = 0x1FFFFFFu;
			float maskValid = 0.0f;
			if (Tes && Tes->gridCellArray && Tes->gridCellArray->size > 0) {
				const int half = (int)(Tes->gridCellArray->size / 2);
				const int gx = Tes->gridCellArray->worldX;
				const int gy = Tes->gridCellArray->worldY;
				minX = (float)(gx - half) * 4096.0f;
				maxX = (float)(gx + half + 1) * 4096.0f;
				minY = (float)(gy - half) * 4096.0f;
				maxY = (float)(gy + half + 1) * 4096.0f;
				if (Tes->gridCellArray->size == 5 && Tes->gridCellArray->gridCells) {
					loadedBits = 0;
					for (int i = 0; i < 25; ++i) {
						TESObjectCELL* cell = Tes->gridCellArray->gridCells[i];
						if (!cell || !cell->coords || cell->GetNode() == nullptr) continue;
						const int cx = cell->coords->x - (gx - half);
						const int cy = cell->coords->y - (gy - half);
						if (cx >= 0 && cx < 5 && cy >= 0 && cy < 5) {
							loadedBits |= (1u << (cy * 5 + cx));
						}
					}
					maskValid = 1.0f;
				}
			}

			const float vsConst[13][4] = {
				{ iv._11, iv._12, iv._13, 0.0f },
				{ iv._21, iv._22, iv._23, 0.0f },
				{ iv._31, iv._32, iv._33, 0.0f },
				{ cam.x,  cam.y,  cam.z,  0.0f },
				{ p._11,  p._22,  p._33,  p._43 },
				{ p._31,  p._32,  (float)(kNumRings - 1), 0.0f },
				{ 0.0f,   0.0f,   0.0f,   0.0f }, // filled per ring below
				{ MapWest, MapNorth, MapTexel > 0.0f ? 1.0f / MapTexel : (1.0f / 128.0f), 0.0f },
				{ (float)MapWidth, (float)MapHeight, 1.0f / (float)MapWidth, 1.0f / (float)MapHeight },
				{ minX, minY, maxX, maxY },
				{ (float)(kGridHalf - 10), 0.1f, 18.0f, 512.0f },
				{ -1.0e9f, -1.0e9f, -1.0e9f, -1.0e9f }, // prevRect (updated per ring)
				{ (float)(loadedBits & 1023u), (float)((loadedBits >> 10) & 1023u), (float)((loadedBits >> 20) & 31u), maskValid },
			};
			d->SetVertexShaderConstantF(0, &vsConst[0][0], 13);

			const D3DXVECTOR4* terrainData  = FindConstant("TESR_TerrainData");
			const D3DXVECTOR4* terrainExtra = FindConstant("TESR_TerrainExtraData");
			const D3DXVECTOR4* terrainSky   = FindConstant("TESR_TerrainSkyData");
			const D3DXVECTOR4* skyIrrad     = FindConstant("TESR_SkyIrradianceTerrain");

			const float roughScale = (terrainData && terrainData->y > 0.0f) ? terrainData->y : 1.0f;
			const float ambScale   = terrainData ? terrainData->w : 1.0f;
			const float satScale   = terrainExtra ? terrainExtra->y : 1.0f;
			const float skyScale   = terrainSky ? terrainSky->x : 0.0f;
			const float gamma      = (std::max)(LinearScene::Gamma, 1.0f);

			float fogParams[4] = {};
			const bool gameFog = DeferredFog::Params(fogParams);
			const D3DXVECTOR4& fogDist = TheShaderManager->ShaderConst.fogData;
			const float fogPower = gameFog ? (std::max)(fogDist.w, 0.01f) : -1.0f;
			const float dbgRingView = (mode >= 2.5f && mode < 3.5f) ? 1.0f : 0.0f;

			float psConst[19][4] = {};
			psConst[0][0] = cam.x; psConst[0][1] = cam.y; psConst[0][2] = cam.z;
			psConst[1][0] = TheShaderManager->ShaderConst.sunAmbient.x;
			psConst[1][1] = TheShaderManager->ShaderConst.sunAmbient.y;
			psConst[1][2] = TheShaderManager->ShaderConst.sunAmbient.z;
			psConst[1][3] = ambScale;
			psConst[2][0] = skyScale; psConst[2][1] = satScale; psConst[2][2] = roughScale; psConst[2][3] = gamma;
			psConst[3][0] = fogParams[0]; psConst[3][1] = fogParams[1]; psConst[3][2] = fogParams[2];
			psConst[3][3] = LinearScene::Linear ? 1.0f : 0.0f;
			psConst[4][0] = fogDist.x; psConst[4][1] = fogDist.y; psConst[4][2] = fogPower; psConst[4][3] = dbgRingView;
			psConst[5][0] = minX; psConst[5][1] = minY; psConst[5][2] = maxX; psConst[5][3] = maxY;
			psConst[7][0] = MapMinZ;
			psConst[7][1] = 1.0f / (std::max)(MapMaxZ - MapMinZ, 1.0f);
			psConst[7][2] = MacroBlendWeight;
			psConst[8][0] = TheShaderManager->ShaderConst.horizonColor.x;
			psConst[8][1] = TheShaderManager->ShaderConst.horizonColor.y;
			psConst[8][2] = TheShaderManager->ShaderConst.horizonColor.z;
			psConst[8][3] = skyIrrad ? 1.0f : 0.0f;
			psConst[9][0] = TheShaderManager->ShaderConst.skyColor.x;
			psConst[9][1] = TheShaderManager->ShaderConst.skyColor.y;
			psConst[9][2] = TheShaderManager->ShaderConst.skyColor.z;
			if (skyIrrad) {
				for (int i = 0; i < 9; ++i) {
					psConst[10 + i][0] = skyIrrad[i].x;
					psConst[10 + i][1] = skyIrrad[i].y;
					psConst[10 + i][2] = skyIrrad[i].z;
					psConst[10 + i][3] = skyIrrad[i].w;
				}
			}
			d->SetPixelShaderConstantF(0, &psConst[0][0], 19);

			// Bind heightmap on VTF sampler 0 (POINT + CLAMP) and normal map on PS stage 0 (LINEAR + CLAMP)
			d->SetTexture(D3DVERTEXTEXTURESAMPLER0, HeightTex);
			const DWORD ptClamp[6]  = { D3DTEXF_POINT,  D3DTEXF_POINT,  D3DTEXF_NONE, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, FALSE };
			const DWORD linClamp[6] = { D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_NONE, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, FALSE };
			for (int i = 0; i < 6; ++i) d->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, kSS[i], ptClamp[i]);
			d->SetTexture(0, NormalTex);
			for (int i = 0; i < 6; ++i) d->SetSamplerState(0, kSS[i], linClamp[i]);
			d->SetTexture(1, MacroAlbedoTex);
			for (int i = 0; i < 6; ++i) d->SetSamplerState(1, kSS[i], linClamp[i]);

			d->SetVertexShader(TerrainVS);
			d->SetPixelShader(TerrainPS);
			d->SetVertexDeclaration(GridDecl);
			d->SetStreamSource(0, GridVB, 0, sizeof(GridVertex));

			float prevRect[4] = { -1.0e9f, -1.0e9f, -1.0e9f, -1.0e9f };
			bool drawnAny = false;
			const bool allLoaded = (maskValid > 0.5f && loadedBits == 0x1FFFFFFu);
			for (int k = 0; k < kNumRings; ++k) {
				const float step = 128.0f * (float)(1u << k);
				const float snapUnit = 2.0f * step;
				const float snapX = floorf(cam.x / snapUnit + 0.5f) * snapUnit;
				const float snapY = floorf(cam.y / snapUnit + 0.5f) * snapUnit;
				const float halfExt = (float)kGridHalf * step;
				if (allLoaded &&
					(snapX - halfExt) > (minX + 64.0f) && (snapX + halfExt) < (maxX - 64.0f) &&
					(snapY - halfExt) > (minY + 64.0f) && (snapY + halfExt) < (maxY - 64.0f)) {
					continue;
				}
				const float ringConst[4] = { snapX, snapY, step, (float)k };
				d->SetVertexShaderConstantF(6, ringConst, 1);
				d->SetVertexShaderConstantF(11, prevRect, 1);
				d->SetPixelShaderConstantF(6, prevRect, 1);

				d->SetIndices(drawnAny ? IBRing : IBFull);
				d->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, kVertCount, 0, drawnAny ? RingPrimCount : FullPrimCount);
				drawnAny = true;

				prevRect[0] = snapX - halfExt;
				prevRect[1] = snapY - halfExt;
				prevRect[2] = snapX + halfExt;
				prevRect[3] = snapY + halfExt;
			}

			// Phase 2: GPU-instanced distant trees & shrubs in the same GBufferV2 + reversed depth pass
			{
				const float treeVsConst[7][4] = {
					{ iv._11, iv._12, iv._13, 0.0f },
					{ iv._21, iv._22, iv._23, 0.0f },
					{ iv._31, iv._32, iv._33, 0.0f },
					{ cam.x,  cam.y,  cam.z,  0.0f },
					{ p._11,  p._22,  p._33,  p._43 },
					{ p._31,  p._32,  0.0f,   0.0f },
					{ minX,   minY,   maxX,   maxY },
				};
				float treePsConst[15][4] = {};
				memcpy(treePsConst[0], psConst[1], sizeof(float) * 4); // ambient
				memcpy(treePsConst[1], psConst[2], sizeof(float) * 4); // skyData
				memcpy(treePsConst[2], psConst[3], sizeof(float) * 4); // fogColor
				memcpy(treePsConst[3], psConst[4], sizeof(float) * 4); // fogData
				memcpy(treePsConst[4], psConst[8], sizeof(float) * 4); // horizonFallback + hasSH
				memcpy(treePsConst[5], psConst[9], sizeof(float) * 4); // skyFallback
				for (int i = 0; i < 9; ++i) memcpy(treePsConst[6 + i], psConst[10 + i], sizeof(float) * 4);
				d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
				DistantLandTrees::RenderInGBuffer(d, treeVsConst, treePsConst);

				// Phase 3: Procedural boulders omitted in standard mode >= 2 because vanilla Object LOD (*.Level4*.nif)
				// already draws real rock/cliff meshes; keep available in mode >= 4 for prototype rock debugging.
				if (mode >= 4) {
					const float invSpanX = (MapWidth > 0 && MapTexel > 0.0f) ? (1.0f / ((float)MapWidth * MapTexel)) : 0.0f;
					const float invSpanY = (MapHeight > 0 && MapTexel > 0.0f) ? (1.0f / ((float)MapHeight * MapTexel)) : 0.0f;
					const float rockVsConst[8][4] = {
						{ iv._11, iv._12, iv._13, 0.0f },
						{ iv._21, iv._22, iv._23, 0.0f },
						{ iv._31, iv._32, iv._33, 0.0f },
						{ cam.x,  cam.y,  cam.z,  0.0f },
						{ p._11,  p._22,  p._33,  p._43 },
						{ p._31,  p._32,  0.0f,   0.0f },
						{ minX,   minY,   maxX,   maxY },
						{ MapWest, MapNorth, invSpanX, invSpanY },
					};
					DistantLandRocks::RenderInGBuffer(d, rockVsConst, treePsConst);
				}
			}

			// Restore D3D9 device state completely
			d->SetTexture(D3DVERTEXTEXTURESAMPLER0, savedVTF);
			if (savedVTF) savedVTF->Release();
			for (int i = 0; i < 6; ++i) if (haveSSV[i]) d->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, kSS[i], savedSSV[i]);
			d->SetTexture(0, savedTex0);
			if (savedTex0) savedTex0->Release();
			for (int i = 0; i < 6; ++i) d->SetSamplerState(0, kSS[i], savedSS0[i]);
			d->SetTexture(1, savedTex1);
			if (savedTex1) savedTex1->Release();
			for (int i = 0; i < 6; ++i) d->SetSamplerState(1, kSS[i], savedSS1[i]);

			d->SetVertexShaderConstantF(0, &savedVSC[0][0], 13);
			d->SetPixelShaderConstantF(0, &savedPSC[0][0], 19);
			for (size_t i = 0; i < sizeof(kRS) / sizeof(kRS[0]); ++i) d->SetRenderState(kRS[i], savedRS[i]);

			for (DWORD i = 0; i < 4; ++i) {
				d->SetRenderTarget(i, savedRT[i]);
				if (savedRT[i]) savedRT[i]->Release();
			}
			d->SetViewport(&savedVP);
			if (savedDecl) { d->SetVertexDeclaration(savedDecl); savedDecl->Release(); }
			else if (savedFVF) d->SetFVF(savedFVF);
			d->SetStreamSource(0, savedVB, savedOff, savedStr); if (savedVB) savedVB->Release();
			d->SetIndices(savedIB); if (savedIB) savedIB->Release();
			d->SetVertexShader(savedVS); if (savedVS) savedVS->Release();
			d->SetPixelShader(savedPS); if (savedPS) savedPS->Release();
		}

		target->Release();
	}
}
