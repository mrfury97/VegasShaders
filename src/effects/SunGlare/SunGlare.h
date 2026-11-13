#pragma once

// UNOFFICIAL 2026-11-13 (modern lighting): the sun's lens glare. A tight halo, a many-pointed starburst and a long horizontal
// streak drawn round the sun's place on screen, as a camera lens (and the eye's lashes) makes them. It shows only as much as the
// sun's disc itself shows: hidden behind a tree, a wall, the first-person weapon or a thick cloud, it is gone. See README.md.
class SunGlareEffect : public EffectRecord
{
public:
	SunGlareEffect() : EffectRecord("SunGlare") {};

	struct SettingsStruct {
		float strength, haloSize, haloStrength, spikes, spikeLength, spikeWidth, spikeStrength, spikeRotation;
		float streakLength, streakWidth, streakStrength, chromatic, sunSize, cloudLow, cloudHigh, sunsetStrength, easeFrames;
		int debugView;
	};
	SettingsStruct	Settings{};

	// TESR_SunGlareData   x strength x day x horizon fade (0: nothing drawn), y halo size, z halo strength, w the debug view
	// TESR_SunGlareSpikes x lines through the sun (spikes / 2), y spike length, z spike width, w spike strength
	// TESR_SunGlareStreak x streak length, y streak width, z streak strength, w chromatic
	// TESR_SunGlareSun    x the disc radius the visibility is measured over, y z the cloud contrast ramp, w the share one frame adds (1 / EaseFrames)
	// TESR_SunGlareTurn   x y cos / sin of the first line's angle, z w cos / sin of the step between lines
	struct ConstantsStruct { D3DXVECTOR4 Data, Spikes, Streak, Sun, Turn; };
	ConstantsStruct	Constants{};

	void	UpdateConstants();
	void	RegisterConstants();
	void	RegisterTextures();
	void	UpdateSettings();
	bool	ShouldRender();
	// The cover pre-pass (technique SunGlareCover), then the picture pass through EffectRecord::Render.
	void	Render(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) override;

private:
	// How much of the sun shows, measured once a frame into one texel and eased over a few frames there (the pass blends into
	// what the texel held: no read-back, no second texture). A sun flickering through moving leaves, and the sub-pixel shift
	// TAA / DLSS give every frame, would otherwise switch the whole glare on and off from frame to frame.
	IDirect3DTexture9*	coverTexture = nullptr;
	IDirect3DSurface9*	coverSurface = nullptr;
	CachedHandle		techCover;
	DWORD				coverFrame = 0;		// the frame the texel was last written (older than the last frame: cleared first)
	bool	RunCover(IDirect3DDevice9* Device);
};
