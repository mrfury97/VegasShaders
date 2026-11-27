#pragma once

// UNOFFICIAL Phase 3 Distant Land (Task 1, docs/next/DISTANT-LAND-PLAN.md):
// Lightweight NIF (Gamebryo 20.2.0.7) LOD block scanner running on the background worker thread.
// Reads Data\Meshes\Landscape\LOD\<world>\Blocks\*.Level4*.nif vertex clouds directly to calibrate
// true Strip/Mojave roof, saucer, and spire Z heights + horizontal rim radii, then merges with
// outdoor ESM-extracted LIGH REFRs. Pure STL + Win32 file I/O (zero game globals).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "LandHeights.h"

namespace LodNifScanner {

	struct BlockFile {
		int			cellX = 0;
		int			cellY = 0;
		bool		isHigh = false;
		std::string	path;
	};

	struct LandmarkProbe {
		const char*	name;
		float		cx, cy, scanR;
		float		defaultMinZ, defaultMaxZ, defaultRimZ, defaultRimR;
		bool		found = false;
		float		minZ = 1.0e9f, maxZ = -1.0e9f;
		float		rimZ = 0.0f, rimR = 0.0f;
		std::vector<std::pair<float, float>> samples; // (z, rXY)
	};

	inline std::string LowerAscii(std::string s) {
		for (char& c : s) c = (char)tolower((unsigned char)c);
		return s;
	}

	inline bool ParseCellFromFilename(const std::string& name, int& outX, int& outY, bool& outHigh) {
		const std::string low = LowerAscii(name);
		outHigh = (low.find(".high.") != std::string::npos);
		// Expect ".x<int>.y<int>.nif"
		const size_t xPos = low.find(".x");
		if (xPos == std::string::npos) return false;
		const size_t yPos = low.find(".y", xPos + 2);
		if (yPos == std::string::npos) return false;
		const char* xp = low.c_str() + xPos + 2;
		const char* yp = low.c_str() + yPos + 2;
		char* endX = nullptr;
		char* endY = nullptr;
		const long cx = strtol(xp, &endX, 10);
		const long cy = strtol(yp, &endY, 10);
		if (endX == xp || *endX != '.' || endY == yp) return false;
		outX = (int)cx;
		outY = (int)cy;
		return true;
	}

	inline std::vector<BlockFile> FindLevel4Blocks(const std::string& worldName) {
		std::vector<BlockFile> out;
		if (worldName.empty()) return out;
		const std::string dir = "Data\\Meshes\\Landscape\\LOD\\" + worldName + "\\Blocks\\";
		WIN32_FIND_DATAA fd = {};
		HANDLE h = FindFirstFileA((dir + "*.Level4*.nif").c_str(), &fd);
		if (h == INVALID_HANDLE_VALUE) return out;

		std::map<std::pair<int, int>, BlockFile> bestByCell;
		do {
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
			int cx = 0, cy = 0;
			bool high = false;
			if (!ParseCellFromFilename(fd.cFileName, cx, cy, high)) continue;
			const std::pair<int, int> key(cx, cy);
			auto it = bestByCell.find(key);
			if (it == bestByCell.end() || (!it->second.isHigh && high)) {
				bestByCell[key] = BlockFile{ cx, cy, high, dir + fd.cFileName };
			}
		} while (FindNextFileA(h, &fd));
		FindClose(h);

		out.reserve(bestByCell.size());
		for (auto& kv : bestByCell) out.push_back(std::move(kv.second));
		return out;
	}

	// Fast NIF 20.2.0.7 reader: seeks directly to NiTriShapeData / NiTriStripsData blocks and appends world-space vertices into matching landmark probes.
	inline void ScanNifBlocksForLandmarks(const BlockFile& blk, std::vector<LandmarkProbe>& probes, size_t& outVertCount) {
		const float ox = (float)blk.cellX * 4096.0f;
		const float oy = (float)blk.cellY * 4096.0f;

		// Quick cell proximity reject before reading potentially 500KB+ NIF files from disk
		bool nearAny = false;
		for (const auto& p : probes) {
			if (p.cx >= ox - 2048.0f && p.cx <= ox + 20480.0f && p.cy >= oy - 2048.0f && p.cy <= oy + 20480.0f) {
				nearAny = true;
				break;
			}
		}
		if (!nearAny) return;

		FILE* f = nullptr;
		if (fopen_s(&f, blk.path.c_str(), "rb") || !f) return;
		fseek(f, 0, SEEK_END);
		const long fsz = ftell(f);
		if (fsz < 64 || fsz > 64L * 1024L * 1024L) { fclose(f); return; }
		fseek(f, 0, SEEK_SET);
		std::vector<uint8_t> buf((size_t)fsz);
		if (fread(buf.data(), 1, buf.size(), f) != buf.size()) { fclose(f); return; }
		fclose(f);

		size_t pos = 0;
		while (pos < buf.size() && buf[pos] != '\n') ++pos;
		if (pos >= buf.size() || pos < 16) return;
		++pos; // skip '\n'

		auto readU8 = [&](uint8_t& v) -> bool { if (pos + 1 > buf.size()) return false; v = buf[pos++]; return true; };
		auto readU16 = [&](uint16_t& v) -> bool { if (pos + 2 > buf.size()) return false; memcpy(&v, &buf[pos], 2); pos += 2; return true; };
		auto readU32 = [&](uint32_t& v) -> bool { if (pos + 4 > buf.size()) return false; memcpy(&v, &buf[pos], 4); pos += 4; return true; };

		uint32_t ver = 0, userVer = 0, numBlocks = 0, userVer2 = 0;
		uint8_t endian = 1;
		if (!readU32(ver) || ver != 0x14020007u || !readU8(endian) || !readU32(userVer) || !readU32(numBlocks) || !readU32(userVer2)) return;
		if (numBlocks == 0 || numBlocks > 65536) return;

		if (userVer >= 11) {
			for (int k = 0; k < 3; ++k) {
				uint8_t sl = 0;
				if (!readU8(sl) || pos + sl > buf.size()) return;
				pos += sl;
			}
		}

		uint16_t numTypes = 0;
		if (!readU16(numTypes) || numTypes == 0 || numTypes > 2048) return;
		std::vector<std::string> types(numTypes);
		for (uint16_t i = 0; i < numTypes; ++i) {
			uint32_t sl = 0;
			if (!readU32(sl) || sl > 512 || pos + sl > buf.size()) return;
			types[i].assign((const char*)&buf[pos], sl);
			pos += sl;
		}

		if (pos + (size_t)numBlocks * 6 > buf.size()) return;
		std::vector<uint16_t> typeIdx(numBlocks);
		std::vector<uint32_t> blockSizes(numBlocks);
		memcpy(typeIdx.data(), &buf[pos], (size_t)numBlocks * 2); pos += (size_t)numBlocks * 2;
		memcpy(blockSizes.data(), &buf[pos], (size_t)numBlocks * 4); pos += (size_t)numBlocks * 4;

		uint32_t numStrings = 0, maxStr = 0;
		if (!readU32(numStrings) || !readU32(maxStr)) return;
		for (uint32_t i = 0; i < numStrings; ++i) {
			uint32_t sl = 0;
			if (!readU32(sl) || sl > 4096 || pos + sl > buf.size()) return;
			pos += sl;
		}

		uint32_t numGroups = 0;
		if (!readU32(numGroups) || pos + (size_t)numGroups * 4 > buf.size()) return;
		pos += (size_t)numGroups * 4;

		for (uint32_t bi = 0; bi < numBlocks; ++bi) {
			const uint32_t bsz = blockSizes[bi];
			if (pos + bsz > buf.size()) break;
			if (typeIdx[bi] < types.size()) {
				const std::string& tname = types[typeIdx[bi]];
				if ((tname == "NiTriShapeData" || tname == "NiTriStripsData") && bsz >= 9) {
					uint16_t nv = 0;
					memcpy(&nv, &buf[pos + 4], 2);
					const uint8_t hasVerts = buf[pos + 8];
					if (hasVerts && nv > 0 && (size_t)9 + (size_t)nv * 12 <= bsz) {
						const float* v = (const float*)&buf[pos + 9];
						outVertCount += nv;
						for (uint16_t vi = 0; vi < nv; ++vi) {
							const float wx = ox + v[vi * 3 + 0];
							const float wy = oy + v[vi * 3 + 1];
							const float wz = v[vi * 3 + 2];
							for (auto& p : probes) {
								const float dx = wx - p.cx;
								const float dy = wy - p.cy;
								const float r2 = dx * dx + dy * dy;
								if (r2 <= p.scanR * p.scanR) {
									p.found = true;
									p.minZ = (std::min)(p.minZ, wz);
									p.maxZ = (std::max)(p.maxZ, wz);
									p.samples.emplace_back(wz, sqrtf(r2));
								}
							}
						}
					}
				}
			}
			pos += bsz;
		}
	}

	inline void FinalizeProbe(LandmarkProbe& p) {
		if (!p.found || p.samples.size() < 24 || p.maxZ <= p.minZ + 50.0f) {
			p.minZ = p.defaultMinZ;
			p.maxZ = p.defaultMaxZ;
			p.rimZ = p.defaultRimZ;
			p.rimR = p.defaultRimR;
			std::vector<std::pair<float, float>>().swap(p.samples);
			return;
		}
		// Inspect 350-unit Z bands across upper half [minZ + 0.50*height .. maxZ] to locate widest upper saucer/roof rim
		const float height = p.maxZ - p.minZ;
		const float bandStart = p.minZ + 0.50f * height;
		const float step = (std::max)(height / 14.0f, 250.0f);
		float bestRimZ = p.defaultRimZ;
		float bestRimR = p.defaultRimR;
		float bestScore = -1.0f;

		std::vector<float> rs;
		for (float zb = bandStart; zb < p.maxZ; zb += step) {
			rs.clear();
			const float zTop = zb + step;
			for (const auto& s : p.samples) {
				if (s.first >= zb && s.first < zTop) rs.push_back(s.second);
			}
			if (rs.size() < 8) continue;
			std::sort(rs.begin(), rs.end());
			const float p90R = rs[(size_t)(rs.size() * 9 / 10)];
			// Slightly favor higher bands when radii tie cleanly
			const float score = p90R + 0.04f * (zb - bandStart);
			if (score > bestScore) {
				bestScore = score;
				bestRimR = p90R;
				bestRimZ = zb + 0.5f * step;
			}
		}
		p.rimZ = bestRimZ;
		p.rimR = (std::max)(bestRimR, 120.0f);
		std::vector<std::pair<float, float>>().swap(p.samples);
	}

	inline std::vector<LandHeights::NightLightRef> ScanAndBuildNightLights(
		const std::string& worldName,
		const std::vector<LandHeights::NightLightRef>& esmLights,
		std::string& noteOut)
	{
		std::vector<LandHeights::NightLightRef> out;
		out.reserve(960);

		const bool isWastelandNV = LowerAscii(worldName) == "wastelandnv";
		size_t scannedBlocks = 0, scannedVerts = 0;

		if (isWastelandNV) {
			std::vector<LandmarkProbe> probes = {
				{ "Lucky38",     -18888.0f,  96472.0f, 1150.0f, 4056.0f, 12912.0f, 11650.0f, 960.0f },
				{ "TheTops",     -22764.0f,  94080.0f, 1050.0f, 4056.0f,  6850.0f,  6550.0f, 460.0f },
				{ "Gomorrah",    -22504.0f,  99096.0f, 1050.0f, 3916.0f,  6250.0f,  5900.0f, 420.0f },
				{ "UltraLuxe",   -18440.6f,  88153.1f, 1100.0f, 4086.6f,  5750.0f,  5050.0f, 520.0f },
				{ "McCarran",    -10088.0f,  62474.0f, 1400.0f, 4592.0f,  5500.0f,  5350.0f, 350.0f },
				{ "HELIOSOne",    21081.4f,  -1265.5f, 1100.0f, 5235.1f,  8950.0f,  8350.0f, 220.0f },
				{ "Novac",        24789.1f, -32500.8f, 1200.0f, 6980.2f,  7750.0f,  7550.0f, 260.0f },
				{ "Goodsprings", -67970.2f,   3904.5f, 1400.0f, 8368.1f,  8800.0f,  8680.0f, 320.0f },
			};

			const std::vector<BlockFile> blocks = FindLevel4Blocks(worldName);
			for (const auto& blk : blocks) {
				size_t before = scannedVerts;
				ScanNifBlocksForLandmarks(blk, probes, scannedVerts);
				if (scannedVerts != before) ++scannedBlocks;
			}
			for (auto& p : probes) FinalizeProbe(p);

			const auto& l38  = probes[0];
			const auto& tops = probes[1];
			const auto& gom  = probes[2];
			const auto& ulux = probes[3];
			const auto& mcc  = probes[4];
			const auto& hel  = probes[5];
			const auto& nov  = probes[6];
			const auto& gs   = probes[7];

			auto add = [&](float x, float y, float z, float radius, float r, float g, float b, float bias = 1100.0f) {
				out.push_back(LandHeights::NightLightRef{ x, y, z, radius, r, g, b, bias });
			};
			constexpr float kTwoPi = 6.2831853f;

			// 1. Lucky 38 calibrated from actual Level4 NIF vertices (spire apex + widest saucer rim + cocktail deck + column ribs)
			const float l38SpireTop = (std::max)(l38.maxZ - 40.0f, l38.rimZ + 500.0f);
			const float l38SaucerZ  = l38.rimZ + 60.0f;
			const float l38SaucerR  = (std::clamp)(l38.rimR * 0.94f, 450.0f, 1080.0f);
			const float l38DeckZ    = l38SaucerZ - 360.0f;
			const float l38BaseZ    = (std::max)(l38.minZ + 350.0f, 4800.0f);

			add(l38.cx, l38.cy, l38SpireTop, 195.0f, 9.0f, 1.5f, 0.5f, 1200.0f);           // aircraft warning ruby spire beacon
			add(l38.cx, l38.cy, l38SpireTop - 520.0f, 155.0f, 6.8f, 3.8f, 1.1f, 1200.0f);  // upper mast amber strobe
			for (int i = 0; i < 24; ++i) {
				const float a = kTwoPi * (float)i / 24.0f;
				const float c = cosf(a), s = sinf(a);
				if (i % 2 == 0)
					add(l38.cx + c * l38SaucerR, l38.cy + s * l38SaucerR, l38SaucerZ, 110.0f, 1.8f, 5.4f, 6.8f, 1200.0f); // iconic cyan saucer rim
				else
					add(l38.cx + c * (l38SaucerR * 0.90f), l38.cy + s * (l38SaucerR * 0.90f), l38DeckZ, 95.0f, 6.2f, 3.9f, 1.2f, 1200.0f); // gold lounge windows
			}
			for (int rib = 0; rib < 4; ++rib) {
				const float a = kTwoPi * (float)rib / 4.0f + 0.35f;
				const float ribSpan = (std::max)(l38DeckZ - 900.0f - l38BaseZ, 1200.0f);
				for (int k = 0; k < 7; ++k) {
					const float z = l38BaseZ + (ribSpan * (float)k) / 6.0f;
					add(l38.cx + cosf(a) * 320.0f, l38.cy + sinf(a) * 320.0f, z, 95.0f, 5.0f, 2.7f, 0.9f, 1200.0f);
				}
			}

			// 2. The Tops Hotel & Casino
			const float topsRoofZ = (std::clamp)(tops.maxZ - 110.0f, tops.minZ + 900.0f, 9200.0f);
			const float topsMarqR = (std::clamp)(tops.rimR * 0.85f, 360.0f, 620.0f);
			const float topsSpanZ = (std::max)(topsRoofZ - (tops.minZ + 350.0f), 900.0f);
			for (int s = 0; s < 3; ++s) {
				const float ox = (float)(s - 1) * 400.0f;
				for (int k = 0; k < 5; ++k) {
					const float z = tops.minZ + 350.0f + (topsSpanZ * (float)k) / 4.0f;
					add(tops.cx + ox, tops.cy - 280.0f, z, 110.0f, 1.4f, 5.6f, 5.2f, 1300.0f);
					add(tops.cx + 340.0f, tops.cy + ox, z + 110.0f, 105.0f, 5.5f, 4.1f, 1.3f, 1300.0f);
				}
			}
			for (int i = 0; i < 8; ++i) {
				const float a = kTwoPi * (float)i / 8.0f;
				add(tops.cx + cosf(a) * topsMarqR, tops.cy + sinf(a) * topsMarqR, topsRoofZ, 125.0f, 1.6f, 6.2f, 5.4f, 1300.0f);
			}

			// 3. Gomorrah
			const float gomRoofZ = (std::clamp)(gom.maxZ - 140.0f, gom.minZ + 800.0f, 8200.0f);
			const float gomR = (std::clamp)(gom.rimR * 0.82f, 340.0f, 560.0f);
			for (int i = 0; i < 10; ++i) {
				const float a = kTwoPi * (float)i / 10.0f;
				const bool hot = (i % 2) == 0;
				add(gom.cx + cosf(a) * gomR, gom.cy + sinf(a) * gomR, gomRoofZ, 130.0f,
					hot ? 6.8f : 6.0f, hot ? 1.5f : 3.2f, hot ? 3.8f : 0.9f, 1300.0f);
			}
			for (int k = 0; k < 6; ++k) {
				const float z = (gom.minZ + 260.0f) + ((gomRoofZ - gom.minZ - 400.0f) * (float)k) / 5.0f;
				add(gom.cx + 380.0f, gom.cy - 320.0f + (float)(k % 3) * 280.0f, z, 115.0f, 6.5f, 1.9f, 3.2f, 1300.0f);
			}

			// 4. Ultra-Luxe
			const float uluxCrownZ = (std::clamp)(ulux.maxZ - 120.0f, ulux.minZ + 700.0f, 7500.0f);
			const float uluxEaveZ  = (std::clamp)(ulux.rimZ, ulux.minZ + 450.0f, uluxCrownZ - 260.0f);
			const float uluxR = (std::clamp)(ulux.rimR * 0.88f, 400.0f, 640.0f);
			for (int i = 0; i < 12; ++i) {
				const float a = kTwoPi * (float)i / 12.0f;
				add(ulux.cx + cosf(a) * uluxR, ulux.cy + sinf(a) * uluxR, uluxEaveZ, 120.0f, 5.8f, 4.3f, 1.9f, 1250.0f);
			}
			add(ulux.cx, ulux.cy, uluxCrownZ, 185.0f, 6.2f, 4.7f, 2.2f, 1250.0f);
			add(ulux.cx + 580.0f, ulux.cy, ulux.minZ + 380.0f, 145.0f, 4.5f, 3.9f, 1.8f, 1250.0f);

			// 5. Strip Promenade & Freeside neon corridor
			struct StripNode { float x, y, z, rad, r, g, b; };
			static const StripNode kCorridor[] = {
				{ -19800.0f,  92200.0f, 4520.0f, 130.0f, 5.2f, 3.1f, 0.9f },
				{ -20600.0f,  95400.0f, 4560.0f, 130.0f, 2.1f, 4.9f, 5.4f },
				{ -21200.0f,  97600.0f, 4480.0f, 125.0f, 6.0f, 2.2f, 3.2f },
				{ -21700.0f, 100500.0f, 4550.0f, 130.0f, 5.6f, 3.8f, 1.1f },
				{ -17900.0f,  90200.0f, 4460.0f, 115.0f, 5.0f, 3.4f, 1.2f },
				{ -17400.0f,  94500.0f, 4500.0f, 115.0f, 1.8f, 4.8f, 4.6f },
				{ -13800.0f, 117595.0f, 4650.0f, 135.0f, 5.5f, 2.5f, 0.8f },
				{ -13216.0f, 117603.0f, 4720.0f, 125.0f, 2.0f, 4.5f, 5.0f },
				{ -17502.0f, 116201.0f, 4800.0f, 130.0f, 5.8f, 1.8f, 2.2f },
			};
			for (const auto& n : kCorridor) add(n.x, n.y, n.z, n.rad, n.r, n.g, n.b, 1250.0f);
			for (int i = 0; i < 20; ++i) {
				const float t = (float)i / 19.0f;
				const float x = -18200.0f - t * 5000.0f + sinf((float)i * 1.7f) * 320.0f;
				const float y = 88200.0f + t * 14500.0f;
				const float z = 4280.0f + (float)(i % 3) * 160.0f;
				const bool cool = (i % 4) == 1;
				add(x, y, z, 95.0f, cool ? 1.8f : 4.6f, cool ? 4.4f : 2.9f, cool ? 4.8f : 0.9f, 1250.0f);
			}

			// 6. Camp McCarran, HELIOS One, Novac & Goodsprings calibrated beacons
			add(mcc.cx, mcc.cy, (std::max)(mcc.maxZ + 60.0f, 5400.0f), 125.0f, 5.4f, 1.1f, 0.4f, 1100.0f);
			add(-8670.0f, 63473.0f, 4900.0f, 105.0f, 4.0f, 2.7f, 1.1f, 1100.0f);
			add(-12424.0f, 60455.0f, 4850.0f, 105.0f, 4.0f, 2.7f, 1.1f, 1100.0f);

			const float helTop = (std::max)(hel.maxZ, 8850.0f);
			add(hel.cx, hel.cy, helTop - 500.0f, 165.0f, 3.2f, 4.8f, 6.2f, 1200.0f);
			add(hel.cx, hel.cy, helTop + 40.0f, 140.0f, 6.4f, 1.4f, 0.5f, 1200.0f);

			add(nov.cx, nov.cy, (std::max)(nov.rimZ, 7650.0f), 115.0f, 4.6f, 2.2f, 0.8f, 900.0f);
			add(23258.3f, -32546.1f, 7450.0f, 110.0f, 5.4f, 1.5f, 0.7f, 900.0f);

			add(gs.cx, gs.cy, (std::max)(gs.rimZ, 8720.0f), 95.0f, 4.4f, 2.6f, 0.9f, 900.0f);
			add(-65385.9f, -1683.7f, 8620.0f, 85.0f, 3.8f, 2.4f, 0.9f, 900.0f);

			char buf[320];
			sprintf_s(buf, "NIF %zu blocks (%zu verts): Lucky38 Z[%.0f..%.0f] saucer rimZ=%.0f R=%.0f; Tops maxZ=%.0f; Gom maxZ=%.0f; UltraLuxe maxZ=%.0f",
				scannedBlocks, scannedVerts, l38.minZ, l38.maxZ, l38SaucerZ, l38SaucerR, tops.maxZ, gom.maxZ, ulux.maxZ);
			noteOut = buf;
		} else {
			noteOut = "non-WastelandNV worldspace (ESM lights only)";
		}

		// Merge ESM-extracted exterior town/outpost LIGH REFRs (avoid double-stacking right on top of Strip landmark rig)
		size_t mergedEsm = 0;
		for (const auto& l : esmLights) {
			if (out.size() >= 960) break;
			if (isWastelandNV && l.x > -24500.0f && l.x < -16500.0f && l.y > 87000.0f && l.y < 101500.0f) continue;
			out.push_back(l);
			++mergedEsm;
		}
		char tail[96];
		sprintf_s(tail, " + %zu ESM lights (%zu total)", mergedEsm, out.size());
		noteOut += tail;
		return out;
	}
}
