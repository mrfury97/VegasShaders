#pragma once

// UNOFFICIAL 2026-10-19: the land heights of one worldspace, read straight from the game's plugin files in load order, for
// the terrain shadows (TerrainShadows.h) when no xLODGen height map is installed. NVR's own code, no game calls: it runs on a
// worker thread and only reads the files.
//
// The plugins are walked group by group (WRLD > the world's children > exterior cell blocks > sub-blocks > a CELL and its
// children > temporary children), skipping everything else by its size, so only the CELL and LAND records of that world are
// read. A cell's LAND from a later plugin replaces an earlier one's (load order). Each LAND's VHGT holds 33 x 33 heights
// 128 units apart (the outer row and column shared with the next cell), as a start height and signed steps of 8 units; the
// first 32 x 32 of each cell are kept, as xLODGen's height maps keep them, and averaged down to the size asked for.
//
// Compressed records (zlib) are unpacked by the small inflate in LandHeights.cpp, written for this from RFC 1950/1951.
#include <cstdint>
#include <string>
#include <vector>

namespace LandHeights {

	struct Plugin {
		std::string	Name;   // as the game lists it ("FalloutNV.esm"), read from Data\<Name>
	};

	struct Result {
		std::vector<float>	Heights;   // row 0 north, column 0 west: each texel's average height (game units)
		std::vector<float>	Peaks;     // ...and its highest point
		uint32_t	Width = 0, Height = 0;
		float		West = 0.0f, North = 0.0f;   // the centre of texel (0, 0), world units
		float		Texel = 128.0f;              // world units between texel centres
		float		Low = 0.0f, High = 0.0f;     // lowest and highest land height
		int			CellWest = 0, CellSouth = 0, CellEast = 0, CellNorth = 0;
		uint32_t	Cells = 0;                   // cells with land
		std::string	Message;                     // what happened, for the log
	};

	struct FoliageRef {
		float		x = 0.0f, y = 0.0f, z = 0.0f, rotZ = 0.0f, scale = 1.0f;
		uint8_t		style = 0; // 0=Joshua, 1=Pine, 2=DeadTree, 3=Yucca/Bush, 4=Cottonwood/Palm
	};

	struct RockRef {
		float		x = 0.0f, y = 0.0f, z = 0.0f, rotZ = 0.0f, scale = 1.0f;
		uint8_t		style = 0; // 0=Boulder, 1=Outcrop/CanyonRock, 2=Cliff/Mesa
	};

	struct NightLightRef {
		float		x = 0.0f, y = 0.0f, z = 0.0f, radius = 100.0f;
		float		r = 1.0f, g = 1.0f, b = 1.0f, depthBias = 1000.0f;
	};

	struct ExtraOut {
		std::vector<uint32_t>		MacroAlbedoRGBA; // half-res (Width+1)/2 x (Height+1)/2 D3DCOLOR_ARGB
		uint32_t					MacroW = 0, MacroH = 0;
		std::vector<FoliageRef>		Foliage;
		std::vector<RockRef>		Rocks;
		std::vector<NightLightRef>	NightLights;
	};

	// Plugins in load order; the worldspace as the plugin that defines it (by name) and its object id (the low 24 bits of its
	// form id). MaxResolution: texels on the longer side at most. Returns false (Message says why) when nothing was found.
	bool	Read(const std::vector<Plugin>& Plugins, const std::string& WorldFile, uint32_t WorldObjectID, uint32_t MaxResolution, Result& Out);
	bool	ReadEx(const std::vector<Plugin>& Plugins, const std::string& WorldFile, uint32_t WorldObjectID, uint32_t MaxResolution, Result& Out, ExtraOut* Extra);

	// zlib (RFC 1950) inflate into Out, stopping once Out holds Limit bytes (0: all). False on a damaged stream.
	bool	Inflate(const uint8_t* In, size_t InSize, std::vector<uint8_t>& Out, size_t Limit);
}
