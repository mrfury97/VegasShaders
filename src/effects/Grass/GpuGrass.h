#pragma once

// UNOFFICIAL 2026-10-27 GPU grass ([Shaders.Grass.GPUGrass], OFF by default; NVR DXVK 1.7-grass, fork patch 0024; docs\next\FORK2-GRASS.md).
// Grass strands placed, culled and counted on the graphics card, then drawn with ONE indirect draw per level of detail, into the game's
// own scene with its depth buffer, instead of the game's alpha-tested grass cards.
//
// Placement is the game's: during GrassFirst's early grass call (Render.cpp namespace GrassFirst, the near tufts before the land) every
// card draw is caught at its DrawIndexedPrimitive (as NewGrass does), its instance table (the tufts the game placed for that grass type
// on that piece of land: position, slope, size step, brightness) and the card's own colour and silhouette per height (read once from
// its texture, NewGrass::GetRamp) are recorded, and the card draw is skipped (KeepCards false) or kept (KeepCards true: the strands are
// added round it). Right after that call one compute pass turns every recorded tuft into a clump of strands (Density: more clumps of the
// same plant beside each tuft), culls the clumps against the view, gives each its strand count by distance and writes the visible strands
// and the draw counts; two indirect draws (near strands 5 triangles, far 1) then put them into the frame with the game's depth test, lit
// exactly as the game's own grass pixel shader lights the cards, in NVR's grass wind ([Shaders.Grass.Wind]) or the game's sway.
// The fading tufts past the fade start stay the game's cards (drawn after the land as before), as does every piece farther than
// BladeDistance, any grass that is not a cut-out card (rocks and debris placed as grass) and all grass while GrassFirst does not run.
// UNOFFICIAL 2026-11-05 (from Balebandro's fork): with terrain blending on (GrassFirst draws no grass before the land then) the same is
// done in GrassFirst's late replay after the land (Render.cpp GrassListHook), while grass shadows recorded the passes (ShadowOnlyEarly):
// the card draws caught there, a mixed pass's fading tufts drawn again as cards, and the strands drawn right after the grass.
//
// Needs the NVR DXVK d3d9.dll with ID3D9NvrExtGeometryScene (1.7-grass); without it (native D3D9, stock DXVK, an older NVR DXVK)
// nothing is caught or skipped and the grass is the game's. Off: no code path touches the device.
namespace GpuGrass {
	void	ReadSettings();								// GrassShaders::UpdateSettings (live)
	// GrassFirst's early grass call: true when this call's card draws are ours (the device's DrawIndexedPrimitive is hooked for it;
	// NewGrass then stays out of it).
	bool	BeginEarly(IDirect3DDevice9* Device, bool GrassEnabled);
	void	Arm(void* Pass, int Part, float Nearest);	// one early grass pass about to be drawn (part 0 every tuft, 1 the near ones)
	void	Disarm();
	bool	Took();									// UNOFFICIAL 2026-11-05 (Balebandro): the armed pass's card draw was caught (before Disarm)
	bool	KeepsCards();								// KeepCards: the caught cards are drawn too
	void	EndEarly(IDirect3DDevice9* Device);			// unhooks; the GPU work (cull + draws) for what was caught
}
