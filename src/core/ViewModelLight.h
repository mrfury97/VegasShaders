#pragma once
// UNOFFICIAL 2026-11-15 DeferredFirstPerson (core/DeferredFirstPerson.h): the view model's sun shadow map transform as ShadowManager::
// RenderViewModelShadowMap made it this frame, kept for the first-person deferred sun pass. The forward shaders read the map through
// CharacterCameraToLight, which also undoes their rebuilt position's projection (see that function); the deferred pass has the true
// camera-relative position, so it takes the light's view x projection alone. Frame: TheFrameRateManager->FrameNumber when it was made.
#include <d3dx9.h>

namespace ViewModelLight {
	inline D3DXMATRIX ViewProj;
	inline unsigned Frame = 0xFFFFFFFFu;
}
