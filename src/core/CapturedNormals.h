#pragma once
#include "DxvkLoader.h"

// UNOFFICIAL 2026-10-28 CapturedNormals (docs/next/CAPTURED-NORMALS.md; macintroll's proposal, first reader: SnowAccumulation,
// [Shaders.SnowAccumulation.Main] CapturedNormals). Every screen-space effect reads TESR_NormalsBuffer, rebuilt from the depth buffer:
// one flat normal per triangle, noisy at edges and under MSAA / FSR (triangle-shaped snow on rocks, cloud shadow crust, striped facing
// terms). Here the surfaces' own smooth normals are written while they draw:
//   - while the world's main opaque list draws (RenderBatches(1, 0x25B), GrassFirst::MainListHook), a screen-sized fp16 target is bound as
//     render target 3, cleared to "nothing" first; NVR's lit templates (ObjectTemplate, ParallaxTemplate, SkinTemplate, TerrainTemplate,
//     compiled with CAPTURED_NORMALS_ON) write COLOR3 = the smooth interpolated vertex normal (world, after skinning and tessellation,
//     BEFORE the normal map; octahedral in rg), the camera distance (b) and 1 (a): Shaders/Includes/CapturedNormals.hlsl;
//   - a draw hook keeps everything else out: the target is written only by those templates drawn opaque (or with the land's
//     SRCALPHA / INVSRCALPHA fade: they write alpha 1, a replace) into the scene target; the game's own shaders, additive lamp and
//     highlight passes, other blends and depth-only draws have its write mask off (COLORWRITEENABLE3 = 0); a draw into any other target,
//     and any Clear, has it unbound (MRT targets must match in size; Clear clears every bound target). LinearScene's full-screen
//     conversions unbind targets 1-3 themselves;
//   - after the list the multisampled copy (the game's MSAA) is resolved; RenderScale v2 frames need nothing: the world draws into the
//     target's top-left part exactly as into the scene's, so the effects' rect coordinates match.
// Readers (Effects/Includes/CapturedNormals.hlsl) compare the stored distance with the depth buffer's at the pixel and use the normal
// only where they agree; anywhere else (the game's own shaders, about 9 % of the list's draws in the LinearScene census, alpha-blended
// and see-through things, water, grass, decals and particles drawn later, the first-person view) they keep the depth-rebuilt normal.
// Off (default): not compiled into the shaders, nothing hooked, no target.
#include <d3d9.h>
#include <unordered_set>
#include "DxvkCompute.h"
#include "GBufferCensus.h"	// UNOFFICIAL 2026-11-14 GBufferCensus (test only): every draw of a census frame, after BeforeDraw
#include "MergeCopies.h"	// UNOFFICIAL 2026-11-14 MergeCopies (test only): its merged draw / checks in the innermost draw hook
#include "DisplaceHeight.h"	// UNOFFICIAL 2026-11-01 SameHeight: the draw hook gives it each draw
#include "DrawState.h"	// UNOFFICIAL 2026-10-29 SharedDrawState: the tracked state lives there
#include "PassSums.h"
#include "FailOnce.h"	// UNOFFICIAL 2026-11-02 RetryFailedEffects
#include "FdTestFault.h"	// UNOFFICIAL 2026-11-16 fd-batchB (test only: DebugVar4 904 / 905, the self-check)
#include "SettingForce.h"	// UNOFFICIAL 2026-11-16 fd-batchB: ShadowContrast refused while the deferred renderer is on (item 23)
#include "FlashlightLamp.h"	// UNOFFICIAL 2026-11-04 the flashlight's game lamp: which objects the G-buffer holds
#include "WaterMask.h"	// UNOFFICIAL 2026-11-12 PTGrassOwnPixel: grass without records goes into the path tracer's mask of the game's own pixels
#include "GpuProfiler.h"	// UNOFFICIAL ShadowContrast step timer	// UNOFFICIAL 2026-10-29 lean hook: AfterStateBlock
#include "RtCore.h"	// UNOFFICIAL 2026-11-15 RT master switches (RtCore::Gated: the grass records for the RT GI)

namespace RtPathTrace { bool Replacing(); }	// UNOFFICIAL 2026-11-12 (core/RtPathTrace.h): the foliage records are written only while it is true

namespace CapturedNormals {

	inline bool Checked = false, Compiled = false;	// on at startup (the templates write COLOR3)
	inline bool Live = false;					// this frame: compiled, wanted by a reader, not failed
	inline bool Failed = false, Hooked = false, MsaaWarned = false;
	inline bool InSpan = false, Bound = false, Drawn = false;
	inline IDirect3DSurface9* SpanTarget = nullptr;	// the scene target the list began with
	inline IDirect3DTexture9* Texture = nullptr;  inline IDirect3DSurface9* TextureSurface = nullptr;	// what the readers sample (TESR_CapturedNormalsBuffer)
	inline IDirect3DSurface9* MsaaSurface = nullptr;	// with the game's MSAA: what the draws write, resolved into Texture after the list
	inline D3DSURFACE_DESC Made = {};
	inline int Recreations = 0;
	inline UInt32 RecreationsSince = 0;	// UNOFFICIAL 2026-11-02 RetryFailedEffects: LiveFrames when the Recreations window began
	inline FailOnce::Latch MakeFail{ "CapturedNormals target" };	// a target that could not be made, or the thrash stop below (not the self-check)
	inline D3DXVECTOR4 Data = { 0.0f, 0.004f, 2.0f, 0.5f };	// TESR_CapturedNormalsData: x 1 = holds this frame's, y z the distance match's share and units, w the least cosine to the depth normal (CapturedNormalAgrees)
	inline std::unordered_set<IDirect3DPixelShader9*> Writers;	// the templates' pixel shaders compiled with CAPTURED_NORMALS_ON (ShaderRecord)
	inline IDirect3DPixelShader9* LastPS = nullptr;  inline bool LastWriter = false;
	inline UInt32 Frames = 0, Writes = 0, Masked = 0, Unbinds = 0;

	inline void NoteWriter(IDirect3DPixelShader9* handle) { if (handle) Writers.insert(handle); }

	// UNOFFICIAL 2026-11-12 foliage records (docs/next/PT-COMPAT-AUDIT-2026-11-12.md items 1 and 9; [Main.Main.Quality] CapturedGrass /
	// CapturedLeaves, live, a missing key = on). The grass (GRASS23x000 / 000TMS.pso) and SpeedTree leaf (STLEAF2000 / 2001 (TMS).pso) pixel
	// shaders are no templates and wrote depth without any record: under the path tracer such a pixel was shaded with the record of
	// whatever was drawn behind it, or shown as the game's own pixel, blade by blade. Compiled with the AlbedoBuffer they write all three
	// records (Includes/CapturedNormals.hlsl CAPTURED_FOLIAGE; ShaderRecord notes them here), and the draw hook takes them as writers:
	//  - leaves: only inside the span (the world's main list), like any template. A leaf pass the game draws outside it writes nothing
	//    (LeafWrites in the log line says which it is);
	//  - grass is never drawn in the main list. The near tufts GrassFirst draws BEFORE it (GrassShaders::DrawPart 1): the span opens at
	//    their first shader bind (GrassBind, from GrassShaders::SetForDraw), so the clear comes first and MainListHook's own BeginSpan
	//    finds the span open. Every grass draw AFTER the list (the fading tufts; everything with terrain blending on or DrawFirst off):
	//    the three targets are bound again, not cleared, for the grass writer draws alone into the span's own scene target (GrassLateDraw),
	//    and taken off (and resolved again under the game's MSAA) at the first other draw or Clear (GrassLateEnd).
	// Only while the path tracer replaces the picture (RtPathTrace::Replacing, read once a frame): the raster frame's readers of these
	// buffers (the deferred flashlight, which would light the grass a second time next to the grass's own beam; snow; AmbientLight's
	// bounce) see exactly what they saw before. (The same day, second pass: the grass's also for the RT GI; see GrassOwnBeam.) Not written: the game's own 1POINT grass shaders (grass by a lamp), GPU grass strands,
	// NewGrass blades, and anything on native D3D9 with the game's MSAA (no span there).
	inline std::unordered_set<IDirect3DPixelShader9*> GrassWriters, LeafWriters;
	inline void NoteGrassWriter(IDirect3DPixelShader9* handle) { if (handle) GrassWriters.insert(handle); }
	inline void NoteLeafWriter(IDirect3DPixelShader9* handle) { if (handle) LeafWriters.insert(handle); }
	inline bool GrassKey = true, LeafKey = true, GrassNow = false, LeafNow = false;	// the keys; this frame: key, writers loaded, path tracing replaces
	// UNOFFICIAL 2026-11-14 full deferred renderer, foliage (core/DeferredFoliage.h, [Main.Main.Quality] DeferredFoliage): FoliageDeferred, set by
	// DeferredFoliage::BeginFrame before BeginFrame here, writes the leaf and grass records this frame whatever the keys (the deferred records
	// need the G-buffer: this is why the census found the leaves "foliageidle"); GrassLateWanted: the late grass's records keep their old readers
	// only (path tracing, the RT GI); the deferred records' handles (the flashlight's game lamp is taken off the objects they draw).
	inline bool FoliageDeferred = false, GrassLateWanted = false, LastFoliageDeferred = false;
	inline std::unordered_set<IDirect3DPixelShader9*> FoliageDeferredHandles;
	// (2026-11-14 fog) the grass DEFERRED record writes its keep and highlight in COLOR2.g [32, 1024) instead of the mark 4.5 (Includes/DeferredPixel.hlsl
	// FDFoliageGrassCode): the grass-mark readers here take that range as grass too once such a record is made (DeferredFoliage::Load)
	inline bool FoliageGrassRange = false;
	inline bool LastGrassWriter = false, LastLeafWriter = false;
	inline bool GrassArmed = false;			// a grass shader was bound after the span: the draws are looked at until the first that is not grass
	inline bool GrassLateOk = false, GrassLateBound = false;	// this frame's span ended (its targets hold the frame); ours are bound outside it
	inline IDirect3DSurface9* GrassLateTarget = nullptr;	// the scene target of that span (not held: compared only)
	inline UInt32 GrassWrites = 0, GrassLateWrites = 0, GrassLateBusy = 0, GrassEarlySpans = 0, LeafWrites = 0;
	// UNOFFICIAL 2026-11-12 PTGrassOwnPixel ([Main.Main.RayTracing] PTGrassOwnPixel, on; core/WaterMask.h has the reasoning). A grass draw of the
	// world scene AFTER the main list (GrassPass: Render.cpp GrassFirst sets it round each grass pass) that writes no record (not a grass
	// writer shader, or the records are not written this frame, or their targets could not be bound) is drawn once more into the path
	// tracer's mask of the game's own pixels (GrassOwnDraw, from the draw hook). Only while the path tracer replaces the picture and this
	// frame's main list left its records (GrassOwnOk, EndSpan): one flag test a draw otherwise. Grass drawn BEFORE the land needs nothing:
	// the land fails the depth test behind it, so no record lies there and the pixel is the game's own already.
	inline bool GrassPass = false, GrassOwnOn = false, GrassOwnOk = false;
	inline UInt32 GrassPassFrame = 0;		// TheFrameRateManager->FrameNumber of that pass (the mask is cleared once a frame)
	inline IDirect3DSurface9* GrassOwnTarget = nullptr;	// the scene target of the frame's span (not held: compared only)
	// UNOFFICIAL 2026-11-12 CapturedGrass, second pass ("grass records that are right"; an RTX run: bright orange-yellow speckles on the
	// tufts that changed every frame). Read from the code, the largest fault first:
	//  1. NOBODY READ THE GRASS'S NORMAL. The path tracer's normals image is TESR_NormalsBuffer (core/RtViewModel.h kPrepPS: `nb.rgb` for
	//     every world pixel), and the RT AO / GI rays start from the same buffer (Compute/RtAOInput.frag). A captured normal gets into it
	//     only through MergeIntoNormals, which wants it within 60 degrees of the depth buffer's own facet. The grass records its tuft's
	//     axis (about straight up), a card's facet is about level: refused on nearly every grass pixel. What stayed is the normal rebuilt
	//     from depth (Effects/Normals.fx: differences to the next texel or two), which on blades one to three pixels wide, with the ground
	//     behind them and the wind moving them, is a different direction per pixel and per frame. Before the records those pixels were
	//     shown as the game's own (PTScreenOwn), so it did not matter; with the records each became a lit surface with a random normal
	//     under the noon sun: the bright speckles. The same normals make the RT GI "sparkle" on grass without any path tracing.
	//     Now MergeIntoNormals takes the record's normal on the pixels the grass shader marks (MaterialBuffer g = 0.5; GrassMergeNow,
	//     [Main.Main.Quality] CapturedGrassNormals, live, a missing key = on), whatever the facet says.
	//  2. The material, the colour's missing tuft shade, the coverage rule and the normal's flip: GRASS23x000TMS.pso.hlsl, main's end.
	//  3. NOT in these files (the tracer's side; described in the hand-over): the traced scene's own grass tufts stand still, are not
	//     turned as the game turns them, and shadow the screen's blades from an 8-unit-wide choice of start points.
	// The gate: the records are also written without path tracing while the RT GI or SunGI reads them (GrassForGI: the bounce's surface
	// colour from the AlbedoBuffer, every screen-space reader's normal through the merge), but not while the deferred flashlight's beam is
	// on the grass (GrassOwnBeam, from GrassShaders::UpdateLights): its light pass lights everything the records hold, and the grass
	// lights itself by the same beam (TESR_GrassSpot): twice. Under the path tracer the raster flashlight is not shown, so there it stays.
	constexpr float kGrassMark = 0.5f;	// MaterialBuffer g of a grass record (GRASS23x000TMS.pso.hlsl)
	constexpr float kGrassMarkV2 = 4.5f;	// UNOFFICIAL 2026-11-14 the same with GBufferV2 (g 4 and up: no shading normal there)
	inline bool GrassOwnBeam = false;	// the grass shaders light the grass by the deferred flashlight's beam this frame
	inline bool GrassForGI = false;		// this frame's grass records are written for the raster frame's RT GI (no path tracing)
	inline bool GrassMergeNow = false;	// this frame: the grass's own normal goes into TESR_NormalsBuffer (MergeIntoNormals)
	// UNOFFICIAL 2026-11-12 CapturedGrassTint: the grass's colour keys as GrassShaders::UpdateSettings read them (tint, colour, saturation),
	// for the self-check's line only (AlbedoCheck): the log then says which settings its numbers were measured with.
	inline float GrassColourKeys[3] = { 1.0f, 1.0f, 1.0f };
	inline int GrassState = -1;			// the last state logged (BeginFrame)
	inline UInt32 GrassStateLogs = 0, GrassBeamFrames = 0;
	// UNOFFICIAL 2026-11-12 CapturedCoverage (audit item 7; see CoverageBegin)
	inline bool CoverageOn = true, CoverageActive = false;
	// Writers whose record alpha is a CODE, not 1 (skinned gear: the normals' 2; the DeferredLamps records: the material's 1.01-1.99 and
	// the albedo's 1 - fog): a code cannot ride the "alpha 0 keeps the texel" blend (see BlendedAlphaBegin), so their blended draws are
	// left exactly as the 2026-11-11 BlendedAlpha fix made them (the whole polygon recorded). ShaderRecord notes them.
	inline std::unordered_set<IDirect3DPixelShader9*> CodeWriters;
	inline void NoteCodeWriter(IDirect3DPixelShader9* handle) { if (handle) CodeWriters.insert(handle); }
	inline bool LastCodeWriter = false;
	inline float CoverageSaved[4] = {};
	inline UInt32 CoverageDraws = 0;
	// UNOFFICIAL 2026-11-12 HairAndEyes ([Shaders.Flashlight.Deferred] HairAndEyes; core/FlashlightDeferred.h BeamBegin, Includes/FlashBeam.hlsl).
	// Hair (SM3002 / SM3003.pso) writes no record. Hair drawn in the main list (alpha-tested hair, as hair mods have it; the game's own
	// is blended and comes after the list) leaves the scalp's record under it, and the deferred flashlight lights that record through
	// hair lying within its depth margin. While the hair takes the beam itself (BeamNow: this span, set by BeamBegin) such a draw
	// writes ONE thing into the captured normals' target: "far away" in the distance (blue; the other channels and the other two
	// records are masked), which the light pass skips by its own reach test. Under the hair's SRCALPHA / INVSRCALPHA blend the shader's
	// alpha there is 1 or 0 (its own alpha against a dither), so a texel is marked or left as it was; drawn without a blend, marked.
	// Any other blend: no mark (the old depth test decides). A writer drawn over the hair later puts its own record back, as always.
	// The eye variant is a writer and marks its own record in its shader. ShaderRecord notes the hair shaders built with the output.
	inline std::unordered_set<IDirect3DPixelShader9*> BeamMarkers;
	inline void NoteBeamMarker(IDirect3DPixelShader9* handle) { if (handle) BeamMarkers.insert(handle); }
	// The game's own hair (blended, no alpha test) is drawn AFTER the main list, after the light pass: nothing to mark there, the beam
	// alone (BeamHeld: this world scene has it for the shaders, from the main list's start; Render.cpp HairBeamBind counts both kinds).
	inline bool BeamNow = false, BeamHeld = false, LastBeamMarker = false;
	inline bool EyeDrawNow = false;	// an eye record is on the device for this draw (Render.cpp GameEyes), for the counts below
	// since the last log line: frames with the beam; hair draws that wrote the mark / could not (another blend); hair shader binds in
	// the main list / after it; eye draws with the beam / of them recorded (writers)
	inline UInt32 BeamFrames = 0, BeamMarks = 0, BeamUnmarked = 0, BeamHairInList = 0, BeamHairAfter = 0, BeamEyes = 0, BeamEyesRecorded = 0;
	// One line of those counts (with the 600-frame lines, and once early after the beam first comes on), then they start again.
	inline void BeamLog() {
		if (BeamFrames) {
			const double n = (double)BeamFrames;
			Logger::Log("CapturedNormals: HairAndEyes: %u frames with the flashlight's beam in the hair and eye shaders. A frame: %.1f hair shader binds in the main list, before the light pass (%.1f draws marked their pixels for it, %.1f could not: another blend than the hair's usual one, the pass's depth test alone decides there), %.1f hair shader binds after the pass (the game's sorted blended list: nothing to mark, the hair blends over what the pass lit); %.1f eye draws took the beam, %.1f of them recorded (the mark is in their record); %u hair shaders have the mark output",
				BeamFrames, BeamHairInList / n, BeamMarks / n, BeamUnmarked / n, BeamHairAfter / n, BeamEyes / n, BeamEyesRecorded / n, (unsigned)BeamMarkers.size());
		}
		BeamFrames = BeamMarks = BeamUnmarked = BeamHairInList = BeamHairAfter = BeamEyes = BeamEyesRecorded = 0;
	}
	// a [Main.Main.Quality] switch, live; a missing key = on
	inline bool QualityKeyOn(const char* key) {
		SettingManager::Configuration::ConfigNode node;
		return !TheSettingManager->Config.FillNode(&node, "Main.Main.Quality", key) || node.BoolValue;
	}

	// UNOFFICIAL 2026-11-07 (ptaaa-firstperson; docs/next/RT-PATH-TRACING.md "ptaaa-firstperson") the first-person view model's G-buffer for
	// the path tracer (RtPathTrace.cpp PTViewModel): the same span round the game's first-person render (Render.cpp RenderFirstPersonHook,
	// BeginViewModelSpan / EndViewModelSpan), without the clears: the arms, gun and Pip-Boy write their smooth normal + distance, colour and
	// material over the world's where they are drawn (the readers' distance test keeps the depth-rebuilt normal there for every other
	// effect, as before: the view model's distance is its own FOV's). The hands' bare skin (the skin template: no albedo / material output
	// of its own) gets its colour through VmSkin (BeforeDraw) and VmSkinFill, its material through SkinFill. Only while the path tracer asks
	// (VmWanted, set by RtPathTrace each frame), after a world span that captured, with no MSAA, into a target of the G-buffer's size.
	inline bool VmWanted = false, VmSpan = false, VmSkinDrawn = false, VmSkinFillFailed = false, VmRefusedLogged = false;
	// UNOFFICIAL 2026-11-15 DeferredFirstPerson (core/DeferredFirstPerson.h): the same span for the full deferred renderer's first person (set by
	// DeferredFirstPerson::Begin for this frame's first-person render only; false without the switch: the span as before, the path tracer's)
	inline bool VmDeferred = false;
	inline UInt32 VmFrame = 0xFFFFFFFFu;	// the frame (TheFrameRateManager->FrameNumber, from Render.cpp) whose view model was captured
	inline UInt32 VmWritesBefore = 0, VmCaptures = 0, VmSkinDraws = 0;
	inline float VmContrastSaved = 0.0f;
	inline IDirect3DTexture9* VmSkinTex = nullptr;  inline IDirect3DSurface9* VmSkinSurface = nullptr;
	inline IDirect3DPixelShader9* VmSkinFillPS = nullptr;

	// UNOFFICIAL 2026-10-29 MaterialBuffer ([Main.Main.Quality] MaterialBuffer, restart, with CapturedNormals; CAPTURED_NORMALS_ON 2; ROOT-ASSUMPTIONS-
	// PLAN B2, format sent to the RT sessions): the same span, a second fp16 target on stage 2 (TESR_MaterialBuffer), written by the
	// Object / Parallax / Terrain templates' spare COLOR2 (Includes/CapturedNormals.hlsl CapturedMaterialOut): r the material class, g the
	// drawn object's bound radius (thickness), b roughness (-1 none), a 1. Stage 2 is taken only while it is free: the skin blur binds its own
	// target there round skin draws (SkinScattering), and the skin templates are no material writers.
	inline bool MaterialChecked = false, MaterialCompiled = false;
	inline IDirect3DTexture9* MaterialTexture = nullptr;  inline IDirect3DSurface9* MaterialSurface = nullptr;  inline IDirect3DSurface9* MaterialMsaa = nullptr;
	inline D3DXVECTOR4 MaterialData = { 0.0f, 0.0f, 0.0f, 0.0f };	// TESR_MaterialBufferData: x 1 = holds this frame's
	inline std::unordered_set<IDirect3DPixelShader9*> MaterialWriters;
	inline bool LastMaterialWriter = false, MaterialBound = false, MaterialDrawn = false;
	inline UInt32 MaterialWrites = 0, MaterialBusy = 0;	// draws that wrote it; writer draws that found stage 2 taken
	// UNOFFICIAL 2026-10-29 ShadowContrast for the game's passes (ROOTPLAN round 6 step 17; ObjectTemplate.hlsl ShadowContrast): the added
	// lamp passes (DIFFUSE) of the object shader add the light their shadows took to MaterialBuffer's alpha (written 1 by the opaque
	// draws, so a = 1 + that light); after the main list ApplyContrast darkens the rest of each pixel's light by that share. Needs
	// MaterialBuffer. ContrastStrength: [Shaders.ShadowsInteriors.Forward] ShadowContrast indoors, 0 elsewhere or not compiled in (PBR.cpp).
	inline float ContrastStrength = 0.0f;
	inline std::unordered_set<IDirect3DPixelShader9*> ContrastWriters;
	inline void NoteContrastWriter(IDirect3DPixelShader9* handle) { if (handle) ContrastWriters.insert(handle); }
	inline bool LastContrastWriter = false, ContrastDrawn = false, ContrastFailed = false;
	inline UInt32 ContrastWrites = 0, ContrastOtherBlend = 0, ContrastApplied = 0;
	inline IDirect3DTexture9* ContrastCopyTex = nullptr;  inline IDirect3DSurface9* ContrastCopy = nullptr;  inline D3DSURFACE_DESC ContrastMade = {};
	inline IDirect3DPixelShader9* ContrastPS = nullptr;
	inline void NoteMaterialWriter(IDirect3DPixelShader9* handle) { if (handle) MaterialWriters.insert(handle); }
	inline bool CompiledIn();
	inline bool MaterialCompiledIn() {
		if (!MaterialChecked) { MaterialChecked = true; MaterialCompiled = CompiledIn() && TheSettingManager->GetSettingI("Main.Main.Quality", "MaterialBuffer") != 0; }
		return MaterialCompiled;
	}
	inline IDirect3DSurface9* MaterialDrawSurface() { return MaterialMsaa ? MaterialMsaa : MaterialSurface; }

	// UNOFFICIAL 2026-10-31 AlbedoBuffer ([Main.Main.Quality] AlbedoBuffer, restart, with CapturedNormals + MaterialBuffer; CAPTURED_NORMALS_ON 3;
	// docs/next/DEFERRED-RENDERING-PLAN.md rung A, results in DEFERRED-RESULTS.md): the same span, a third fp16 target on stage 1 (TESR_AlbedoBuffer),
	// written by the Object / Parallax / Terrain templates' spare COLOR1 (Includes/CapturedNormals.hlsl CapturedAlbedoOut): rgb the LINEAR base
	// colour the light is multiplied by (texture x vertex colour, before metal), a 1 = written. Stage 1 is taken only while it is free or ours:
	// the skin blur binds its own target there round skin draws (SkinScattering), and the ONLY_SPECULAR records keep their highlightCopy on
	// COLOR1 (they are no writers: masked off while ours is bound). The DIFFUSE lamp passes have no texture of their own and write a 0 unless
	// FoldHighlights bound the base map on s7; the ONLY_LIGHT base passes write the texture the game's TEXTURE pass multiplies in (s0).
	// Skin: SkinAlbedoFill copies the skin blur's own albedo target (stored curve, decoded with the linear lighting's gamma) into it where the
	// normals' a marks an actor, after the list (needs SkinScattering's screen-space blur; otherwise skin stays unwritten).
	// Readers: DlssRRGuides (RRAlbedo 2), AmbientLight's bounce (AlbedoBuffer); they ask for the capture through AlbedoWanted (Render.cpp).
	inline bool AlbedoChecked = false, AlbedoCompiled = false;
	inline IDirect3DTexture9* AlbedoTexture = nullptr;  inline IDirect3DSurface9* AlbedoSurface = nullptr;  inline IDirect3DSurface9* AlbedoMsaa = nullptr;
	inline D3DXVECTOR4 AlbedoData = { 0.0f, 0.0f, 0.0f, 0.0f };	// TESR_AlbedoBufferData: x 1 = holds this frame's
	inline std::unordered_set<IDirect3DPixelShader9*> AlbedoWriters, AlbedoLightOnlyWriters;	// the latter: the ONLY_LIGHT / DIFFUSE records (their albedo is the texture a later pass multiplies in)
	inline bool LastAlbedoWriter = false, LastAlbedoLightOnly = false, AlbedoBound = false, AlbedoDrawn = false;
	inline UInt32 AlbedoWrites = 0, AlbedoBusy = 0, AlbedoLightOnlyDraws = 0, AlbedoSkinFills = 0;
	inline IDirect3DTexture9* SkinAlbedoTex = nullptr;  inline IDirect3DSurface9* SkinAlbedoMsaa = nullptr;  inline IDirect3DSurface9* SkinAlbedoSurface = nullptr;	// the skin blur's albedo target, when skin drew this span (SkinScattering::BindForSkinDraw)
	inline IDirect3DPixelShader9* SkinAlbedoFillPS = nullptr;  inline bool SkinAlbedoFillFailed = false, SkinAlbedoLogged = false, AlbedoWantLogged = false;
	inline void NoteAlbedoWriter(IDirect3DPixelShader9* handle, bool lightOnly) { if (!handle) return; AlbedoWriters.insert(handle); if (lightOnly) AlbedoLightOnlyWriters.insert(handle); }
	inline bool SunGIWantsAlbedo = false;	// UNOFFICIAL 2026-11-12 (sungi p4) set by AmbientLight (SunGI on, outdoors): AlbedoBuffer's reader asks for the capture without the AL key
	inline bool BloodWantsAlbedo = false;	// UNOFFICIAL 2026-11-12 set by BloodFlow ([Shaders.BloodFlow.Surface] on with a patch of blood): its film takes the light from the picture over this record
	inline void NoteSkinAlbedo(IDirect3DTexture9* tex, IDirect3DSurface9* msaa, IDirect3DSurface9* surface) { SkinAlbedoTex = tex; SkinAlbedoMsaa = msaa; SkinAlbedoSurface = surface; }
	inline bool AlbedoCompiledIn() {
		if (!AlbedoChecked) { AlbedoChecked = true; AlbedoCompiled = MaterialCompiledIn() && TheSettingManager->GetSettingI("Main.Main.Quality", "AlbedoBuffer") != 0; }
		return AlbedoCompiled;
	}
	inline IDirect3DSurface9* AlbedoDrawSurface() { return AlbedoMsaa ? AlbedoMsaa : AlbedoSurface; }
	// UNOFFICIAL 2026-11-14 GBufferV2 ([Main.Main.Quality] GBufferV2, restart, off by default; CAPTURED_NORMALS_ON 4; Includes/CapturedNormals.hlsl;
	// docs/next/FD-GBUFFER-RESULTS.md): the MaterialBuffer's layout v2 of the full deferred renderer (FULL-DEFERRED-RENDERER-PLAN.md 4.2). r the
	// shading model (+16 specular) + 0.49 metal, g / a the normal-mapped normal (octahedral; g 4 and up = none here), b the roughness; written =
	// r > 0.5. Needs the AlbedoBuffer level (CapturedNormals + MaterialBuffer + AlbedoBuffer): refused with a log line without it.
	inline bool V2Checked = false, V2Compiled = false;
	inline bool V2CompiledIn() {
		if (!V2Checked) {
			V2Checked = true;
			const bool wanted = TheSettingManager->GetSettingI("Main.Main.Quality", "GBufferV2") != 0;
			V2Compiled = wanted && AlbedoCompiledIn();
			if (wanted && !V2Compiled) Logger::Log("[WARNING] UNOFFICIAL GBufferV2: needs [Main.Main.Quality] CapturedNormals + MaterialBuffer + AlbedoBuffer on (restart): the G-buffer stays in its old layout");
			else if (V2Compiled) Logger::Log("UNOFFICIAL GBufferV2: the G-buffer's material target in layout v2 (shading model + specular bit + metal, the normal-mapped normal, roughness)");
		}
		return V2Compiled;
	}
	// UNOFFICIAL 2026-11-16 fd-batchB (deferred gaps audit item 23): ShadowContrast's game-pass route (ContrastStrength above) needs the
	// DIFFUSE lamp passes to add their lost light into MaterialBuffer's alpha. With the deferred renderer on ([Main.Main.Quality]
	// DeferredRenderer 1 or 2 at startup) those passes are skipped for every deferred object, and its G-buffer layout v2 (forced on by it)
	// keeps the shading normal's y in that alpha: the passes still drawn would bend the normal and ApplyContrast would darken by a normal.
	// Refused while either holds (a startup state, so it does not come and go with DeferredRendererLive), with one log line. The
	// in-shader half (TESR_ShadowContrast in the one-pass lamp shader, effects/PBR/PBR.cpp) is not touched here: it darkens only the
	// draws still lit forward, each by its own shadows. Off (ShadowContrast 0, the default): nothing changes.
	inline bool ContrastLive() {
		if (ContrastStrength <= 0.0f) return false;
		const int mode = SettingForce::Mode();
		if (mode < 1 && !V2Compiled) return true;
		static bool told = false;
		if (!told) {
			told = true;
			Logger::Log("[WARNING] UNOFFICIAL ShadowContrast ([Shaders.ShadowsInteriors.Forward] ShadowContrast %.2f): refused while %s: its game-pass step needs the lamp passes the "
				"deferred renderer skips, and writes the alpha that holds the G-buffer's shading normal there. The rest of the look is unchanged (logged once)",
				ContrastStrength, mode >= 1 ? "the deferred renderer is on (DeferredRenderer 1 or 2)" : "the G-buffer layout v2 is on (GBufferV2)");
		}
		return false;
	}
	// A MaterialBuffer texel, either layout: written, and its class / shading model (0-15).
	inline bool MaterialWritten(const float* v) { return V2Compiled ? v[0] > 0.5f : v[3] > 0.5f; }
	inline int MaterialClass(const float* v) { int c = (int)floorf(v[0] + 0.004f); if (V2Compiled && c >= 16) c -= 16; return c; }
	// UNOFFICIAL 2026-11-12 (ptbump; a tester under the path tracer: "textures are missing their normals/bumpmaps", "a gross sheen over
	// everything"). Two additions to the MaterialBuffer's record, both only read by the path tracer (RtPathTrace.comp ScreenHit):
	//  - the LAND's bumped normal ([Main.Main.Quality] CapturedBumpNormals, restart; TerrainTemplate.hlsl CAPTURED_PT 2, compiled in only
	//    with the AlbedoBuffer level under the DXVK fork, where path tracing runs: ShaderRecord::BuildDefines' CAPTURED_PT 2, BumpCompiled). A land
	//    texel (class 4) held no radius and no roughness worth reading (g the last object's radius, b -1): now g = 3 + e.x, b = -7 + e.y,
	//    e the blended layers' normal-mapped normal in world axes, hemi-octahedral about +z (-1 .. 1 each). Every reader of g / b tests
	//    the class or b >= 0 first (WetWorld: land is ground whatever g; the flashlight / RR / Reflections: b < 0 = no roughness, as -1).
	//    The 6- and 7-layer land shaders keep the old values (no temp register left: see CapturedCoverage's note there);
	//  - "the game draws no highlight on this mesh" (NoHighlightFlag, set by RtPathTrace while the path tracer runs with
	//    PTMatteNoHighlight; CAPTURED_PT 1 and up, PtCodesCompiled): Render.cpp MaterialForPass sends the draw's class + 8 x radius NEGATIVE for a mesh without the specular
	//    shader flag, and the object / parallax records then hold b = -2 - roughness (Includes/CapturedNormals.hlsl). The other readers
	//    of b decode it back to the same roughness (FlashlightDeferred.hlsl, MergeIntoReflMaterial, DlssRRGuides.comp): unchanged.
	inline bool NoHighlightFlag = false;
	inline bool PtCodesCompiled = false, BumpCompiled = false;	// ShaderRecord::BuildDefines (CAPTURED_PT 1 / 2)
	inline bool BumpCompiledIn() { return BumpCompiled; }
	// Render.cpp's per-frame "wanted" for the whole capture: a reader of the albedo asks for it (else, with CapturedNormals' merge off and no snow,
	// the G-buffer would never be made). Logged once.
	inline bool AlbedoWanted() {
		const bool rr = TheSettingManager->GetSettingI("Shaders.TAA.DLSS", "RayReconstruction") != 0 && TheSettingManager->GetSettingI("Shaders.TAA.DLSS", "RRAlbedo") == 2;
		const bool al = TheSettingManager->GetSettingI("Shaders.AmbientLight.Main", "AlbedoBuffer") != 0 || SunGIWantsAlbedo || BloodWantsAlbedo;	// UNOFFICIAL 2026-11-12 (sungi p4): SunGI outdoors asks for it too; BloodFlow Surface too
		if (!AlbedoCompiledIn()) {
			// UNOFFICIAL 2026-10-31 (round 1's AlbedoBuffer-off twin run: the AmbientLight switch went on and off without a word): a reader
			// switched on without the capture gets its one line, so the silence is explained.
			static bool missingLogged = false;
			if (!missingLogged && (rr || al)) {
				missingLogged = true;
				Logger::Log("AlbedoBuffer: %s asked for it, but [Main.Main.Quality] AlbedoBuffer is off (restart; needs CapturedNormals + MaterialBuffer): the guessed colour stays (logged once)",
					rr && al ? "DLSS RR albedo and AmbientLight / BloodFlow Surface" : rr ? "DLSS RR albedo" : "AmbientLight / BloodFlow Surface");
			}
			return false;
		}
		if (!AlbedoWantLogged && (rr || al)) { AlbedoWantLogged = true; Logger::Log("AlbedoBuffer: captured every frame for its readers (%s%s%s)", rr ? "DLSS RR albedo" : "", rr && al ? ", " : "", al ? "AmbientLight bounce / BloodFlow Surface" : ""); }
		return rr || al;
	}


	// UNOFFICIAL 2026-10-31 MaterialBuffer gaps (ROOT-ASSUMPTIONS-PLAN B2): metal (r = class + 0.49 x metal, Includes/CapturedNormals.hlsl),
	// the parallax draws' roughness, and skin. The skin templates have no target left (COLOR1 / COLOR2 are the skin blur's, COLOR3 the
	// normals), so with MaterialBuffer they put their roughness in the captured normal's alpha (2.05 + 0.4 x roughness; gear stays 2,
	// every reader's a > 0.99 / a > 1.5 test still holds), and after the list SkinFill copies those pixels into the buffer as class 6
	// (skin, thickness 0, that roughness, metal 0). [Main.Main.Quality] MaterialBufferSkin (live, on). Vanilla Plus Skin's template
	// (SkinVPSTemplate, one target) writes neither buffer: its pixels stay class 0.
	inline IDirect3DPixelShader9* SkinFillPS = nullptr;  inline bool SkinFillFailed = false;
	inline UInt32 SkinFills = 0;
	// UNOFFICIAL 2026-11-15 DeferredSkin (core/DeferredSkin.h, Includes/SkinDeferred.hlsl): set at record load when the deferred skin records are
	// made. Their pixels hold the whole G-buffer and a COLOR3.a code of 4 and up: the two skin fills below then take 2.02 .. 3.5 / 1.5 .. 3.5 only
	inline bool SkinDeferredCodes = false;
	inline IDirect3DPixelShader9* FullScreenShader(IDirect3DDevice9* d, const char* src, const char* what, bool& failed) {
		IDirect3DPixelShader9* out = nullptr;
		ID3DXBuffer* code = nullptr; ID3DXBuffer* errors = nullptr;
		if (FAILED(D3DXCompileShader(src, (UINT)strlen(src), NULL, NULL, "main", "ps_3_0", 0, &code, &errors, NULL)) || !code ||
			FAILED(d->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &out))) {
			Logger::Log("[ERROR] %s: its shader failed (%s)", what, errors ? (const char*)errors->GetBufferPointer() : "?");
			failed = true; out = nullptr;
		}
		if (errors) errors->Release();
		if (code) code->Release();
		return out;
	}
	// One full-screen draw of ps into target (the whole surface, uv 0-1) reading source on stage 0, optionally blended
	// (src x srcBlend + dest x destBlend); everything it changes is put back.
	inline void FullScreenPass(IDirect3DDevice9* d, IDirect3DSurface9* target, IDirect3DPixelShader9* psNew, IDirect3DBaseTexture9* source,
		bool blendOn, DWORD srcBlend, DWORD destBlend, IDirect3DBaseTexture9* source2 = nullptr,	// source2: UNOFFICIAL 2026-10-31 AlbedoBuffer, on stage 1
		IDirect3DBaseTexture9* source3 = nullptr) {	// source3: UNOFFICIAL 2026-11-14 GBufferView, on stage 2 (with source2)
		D3DSURFACE_DESC t = {};
		if (!target || !psNew || FAILED(target->GetDesc(&t))) return;
		D3DVIEWPORT9 vp = {}; d->GetViewport(&vp);
		IDirect3DSurface9* rt[4] = {};
		for (DWORD i = 0; i < 4; i++) d->GetRenderTarget(i, &rt[i]);
		for (DWORD i = 1; i < 4; i++) if (rt[i]) d->SetRenderTarget(i, nullptr);
		d->SetRenderTarget(0, target);
		const D3DVIEWPORT9 full = { 0, 0, t.Width, t.Height, 0.0f, 1.0f };
		d->SetViewport(&full);
		IDirect3DVertexShader9* vs = nullptr; IDirect3DPixelShader9* ps = nullptr; IDirect3DVertexDeclaration9* decl = nullptr;
		IDirect3DVertexBuffer9* vb = nullptr; UINT vbOffset = 0, vbStride = 0; DWORD fvf = 0;
		IDirect3DBaseTexture9* tex0 = nullptr; IDirect3DBaseTexture9* tex1 = nullptr; IDirect3DBaseTexture9* tex2 = nullptr;
		if (!source2) source3 = nullptr;
		d->GetVertexShader(&vs); d->GetPixelShader(&ps); d->GetVertexDeclaration(&decl); d->GetFVF(&fvf);
		d->GetStreamSource(0, &vb, &vbOffset, &vbStride); d->GetTexture(0, &tex0); if (source2) d->GetTexture(1, &tex1); if (source3) d->GetTexture(2, &tex2);
		static const D3DRENDERSTATETYPE kRS[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE, D3DRS_CULLMODE, D3DRS_STENCILENABLE,
			D3DRS_SCISSORTESTENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_COLORWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND,
			D3DRS_BLENDOP, D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_FOGENABLE };
		const DWORD want[] = { D3DZB_FALSE, FALSE, FALSE, D3DCULL_NONE, FALSE, FALSE, FALSE, 0xF, (DWORD)(blendOn ? TRUE : FALSE),
			blendOn ? srcBlend : D3DBLEND_ONE, blendOn ? destBlend : D3DBLEND_ZERO, D3DBLENDOP_ADD, FALSE, FALSE };
		DWORD saved[sizeof(kRS) / sizeof(kRS[0])];
		for (unsigned i = 0; i < sizeof(kRS) / sizeof(kRS[0]); i++) { if (FAILED(d->GetRenderState(kRS[i], &saved[i]))) saved[i] = want[i]; if (saved[i] != want[i]) d->SetRenderState(kRS[i], want[i]); }
		static const D3DSAMPLERSTATETYPE kSS[] = { D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE };
		const DWORD wantS[] = { D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, FALSE };
		const DWORD stages = source3 ? 3 : source2 ? 2 : 1;
		DWORD savedS[3][sizeof(kSS) / sizeof(kSS[0])];
		for (DWORD s = 0; s < stages; s++)
			for (unsigned i = 0; i < sizeof(kSS) / sizeof(kSS[0]); i++) { if (FAILED(d->GetSamplerState(s, kSS[i], &savedS[s][i]))) savedS[s][i] = wantS[i]; if (savedS[s][i] != wantS[i]) d->SetSamplerState(s, kSS[i], wantS[i]); }
		d->SetVertexShader(nullptr);
		d->SetPixelShader(psNew);
		d->SetTexture(0, source);
		if (source2) d->SetTexture(1, source2);
		if (source3) d->SetTexture(2, source3);
		d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
		const float x1 = (float)t.Width - 0.5f, y1 = (float)t.Height - 0.5f;
		const float quad[4][6] = { { -0.5f, -0.5f, 0, 1, 0, 0 }, { x1, -0.5f, 0, 1, 1, 0 }, { -0.5f, y1, 0, 1, 0, 1 }, { x1, y1, 0, 1, 1, 1 } };
		d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
		for (DWORD s = 0; s < stages; s++)
			for (unsigned i = 0; i < sizeof(kSS) / sizeof(kSS[0]); i++) if (savedS[s][i] != wantS[i]) d->SetSamplerState(s, kSS[i], savedS[s][i]);
		for (unsigned i = 0; i < sizeof(kRS) / sizeof(kRS[0]); i++) if (saved[i] != want[i]) d->SetRenderState(kRS[i], saved[i]);
		d->SetRenderTarget(0, rt[0]);
		for (DWORD i = 1; i < 4; i++) if (rt[i]) d->SetRenderTarget(i, rt[i]);
		for (DWORD i = 0; i < 4; i++) if (rt[i]) rt[i]->Release();
		d->SetViewport(&vp);
		d->SetTexture(0, tex0); if (tex0) tex0->Release();
		if (source2) { d->SetTexture(1, tex1); if (tex1) tex1->Release(); }
		if (source3) { d->SetTexture(2, tex2); if (tex2) tex2->Release(); }
		if (decl) { d->SetVertexDeclaration(decl); decl->Release(); } else d->SetFVF(fvf);
		d->SetStreamSource(0, vb, vbOffset, vbStride); if (vb) vb->Release();
		d->SetVertexShader(vs); if (vs) vs->Release();
		d->SetPixelShader(ps); if (ps) ps->Release();
	}
	inline void SkinFill(IDirect3DDevice9* d) {
		if (SkinFillFailed || !MaterialSurface || !Texture) return;
		if (TheSettingManager->GetSettingI("Main.Main.Quality", "MaterialBufferSkin") == 0) return;
		if (!SkinFillPS) {
			static const char* kPS =
				"sampler2D N : register(s0);"
				"float4 main(float2 uv : TEXCOORD0) : COLOR0 {"
				"  float a = tex2D(N, uv).a; clip(a - 2.02);"
				"  return float4(6.0, 0.0, saturate((a - 2.05) / 0.4), 1.0); }";
			// UNOFFICIAL 2026-11-14 GBufferV2: the same in layout v2: skin's shading model 6, no shading normal (g 4: the skin template has no
			// target left for it; readers take the smooth normal), written by the class
			static const char* kPSV2 =
				"sampler2D N : register(s0);"
				"float4 main(float2 uv : TEXCOORD0) : COLOR0 {"
				"  float a = tex2D(N, uv).a; clip(a - 2.02);"
				"  return float4(6.0, 4.0, saturate((a - 2.05) / 0.4), 1.0); }";
			static const char* kPSV2Deferred =	// (DeferredSkin: the deferred skin's own record stays)
				"sampler2D N : register(s0);"
				"float4 main(float2 uv : TEXCOORD0) : COLOR0 {"
				"  float a = tex2D(N, uv).a; clip(a - 2.02); clip(3.5 - a);"
				"  return float4(6.0, 4.0, saturate((a - 2.05) / 0.4), 1.0); }";
			SkinFillPS = FullScreenShader(d, V2Compiled ? (SkinDeferredCodes ? kPSV2Deferred : kPSV2) : kPS, "MaterialBufferSkin", SkinFillFailed);
			if (!SkinFillPS) return;
		}
		static GpuTimer skinTimer("MaterialBuffer skin fill");
		GpuProfileScope gpu(skinTimer, d);
		FullScreenPass(d, MaterialSurface, SkinFillPS, Texture, false, D3DBLEND_ONE, D3DBLEND_ZERO);
		SkinFills++;
	}
	// UNOFFICIAL 2026-10-31 AlbedoBuffer: the skin blur's own albedo target (SkinTemplate.hlsl COLOR2: saturate(baseColor) in the stored curve, its
	// luma in a) copied into TESR_AlbedoBuffer where the normals mark an actor (a >= 1.5: skin 2, or 2.05 + 0.4 x roughness with MaterialBufferSkin),
	// decoded with the linear lighting's gamma (LinColor: x^gamma above 1, as is at 1). Only when skin drew this span with the screen-space blur
	// on (SkinScattering::BindForSkinDraw notes its target); otherwise skin stays unwritten (a 0: the readers keep their stand-ins there).
	inline void SkinAlbedoFill(IDirect3DDevice9* d) {
		if (SkinAlbedoFillFailed || !AlbedoSurface || !Texture || !SkinAlbedoTex) return;
		if (!SkinAlbedoFillPS) {
			static const char* kPS =
				"sampler2D N : register(s0); sampler2D S : register(s1); float4 G : register(c0);"
				"float4 main(float2 uv : TEXCOORD0) : COLOR0 {"
				"  float a = tex2D(N, uv).a; clip(a - 1.5);"
				"  float3 c = max(tex2D(S, uv).rgb, 0.0); c = G.x > 1.0 ? pow(c, G.x) : c;"
				"  return float4(c, 1.0); }";
			static const char* kPSDeferred =	// (DeferredSkin: the deferred skin's own albedo stays)
				"sampler2D N : register(s0); sampler2D S : register(s1); float4 G : register(c0);"
				"float4 main(float2 uv : TEXCOORD0) : COLOR0 {"
				"  float a = tex2D(N, uv).a; clip(a - 1.5); clip(3.5 - a);"
				"  float3 c = max(tex2D(S, uv).rgb, 0.0); c = G.x > 1.0 ? pow(c, G.x) : c;"
				"  return float4(c, 1.0); }";
			SkinAlbedoFillPS = FullScreenShader(d, SkinDeferredCodes ? kPSDeferred : kPS, "AlbedoBuffer skin fill", SkinAlbedoFillFailed);
			if (!SkinAlbedoFillPS) return;
		}
		if (SkinAlbedoMsaa && SkinAlbedoSurface) d->StretchRect(SkinAlbedoMsaa, NULL, SkinAlbedoSurface, NULL, D3DTEXF_NONE);	// (the blur resolves it again later: the same bytes)
		static GpuTimer timer("AlbedoBuffer skin fill");
		GpuProfileScope gpu(timer, d);
		float saved[4] = {};
		d->GetPixelShaderConstantF(0, saved, 1);
		const float g[4] = { PassSums::Gamma, 0.0f, 0.0f, 0.0f };
		d->SetPixelShaderConstantF(0, g, 1);
		FullScreenPass(d, AlbedoSurface, SkinAlbedoFillPS, Texture, false, D3DBLEND_ONE, D3DBLEND_ZERO, SkinAlbedoTex);
		d->SetPixelShaderConstantF(0, saved, 1);
		AlbedoSkinFills++;
		if (!SkinAlbedoLogged) { SkinAlbedoLogged = true; Logger::Log("AlbedoBuffer: skin albedo copied from the skin blur's target (gamma %.2f)", PassSums::Gamma); }
	}
	// UNOFFICIAL 2026-10-31 Reflections MaterialBufferRoughness, D3D9 effect: the reflections' own material copy (8 bits: r roughness,
	// g metal, b 1 = an _rmaos map, alpha 1 there, 0 elsewhere) takes this buffer's roughness and metal where it has no map: b 0.5 there
	// ("the game's shader's", read by the floor guess only). Blended INVDESTALPHA / DESTALPHA so the map's pixels stay as they are.
	inline IDirect3DPixelShader9* ReflMergePS = nullptr;  inline bool ReflMergeFailed = false;
	inline bool MergeIntoReflMaterial(IDirect3DDevice9* d, IDirect3DSurface9* target) {
		if (ReflMergeFailed || !MaterialTexture || MaterialData.x < 0.5f || !target) return false;
		if (!ReflMergePS) {
			static const char* kPS =
				"sampler2D M : register(s0);"
				"float4 main(float2 uv : TEXCOORD0) : COLOR0 {"
				"  float4 m = tex2D(M, uv); clip(m.r - 0.5);"
				"  float b = m.b < -1.99 && m.b > -3.01 ? -2.0 - m.b : m.b; clip(b);"	// (2026-11-12 ptbump: a no-highlight record's roughness, as before the code)
				"  return float4(saturate(b), saturate(frac(m.r) / 0.49), 0.5, 0.0); }";
			ReflMergePS = FullScreenShader(d, kPS, "Reflections MaterialBufferRoughness", ReflMergeFailed);
			if (!ReflMergePS) return false;
		}
		FullScreenPass(d, target, ReflMergePS, MaterialTexture, true, D3DBLEND_INVDESTALPHA, D3DBLEND_DESTALPHA);
		return true;
	}

	// The self-check (main session, 2026-10-28: P31's MergedDepthNormals wrote two targets in one draw, and on native D3D9 the GTX 1070
	// driver silently dropped that draw: the buffers stopped updating and every effect read stale data). On a few frames (the 60th, 300th
	// and 1200th captured frame, then every 18,000th) the target is read back, shrunk to 64 x 64 texels: right after the clear it must hold
	// no written texel (the clear really happened), after the list at least one (the draws really wrote it) when templates drew. A
	// failure switched the capture off for the session (logged): the readers keep the depth-rebuilt normal. A read-back waits for the GPU
	// once, on those frames only. (Before 2026-11-16 one failure did it at once; now three in a row do, see below.)
	// UNOFFICIAL 2026-11-16 fd-batchB (deferred gaps audit item 8): one WRONG check switched the capture off for the session, and with it the
	// deferred renderer, whatever the frame held (the after-list count takes record codes only: a frame of nothing the check can count reads
	// as "not written"). Now a WRONG check is checked again kRecheckFrames later; kStrikes WRONG in a row (each half counted apart, a right
	// one starts it over) is a broken target: off as before, but through a FailOnce latch (CheckFail): tried again after the next cell change
	// with a check soon after, 3 tries in all ([Main.Main.Misc] RetryFailedEffects false: off for the session, as before). A target the driver
	// really drops fails every check, so it is still switched off within kStrikes x kRecheckFrames frames.
	inline IDirect3DSurface9* CheckTarget = nullptr;  inline IDirect3DSurface9* CheckCopy = nullptr;
	inline UInt32 LiveFrames = 0, CheckFailures = 0;
	inline bool CheckThisFrame = false, CheckReported = false;
	constexpr int kStrikes = 3;
	constexpr UInt32 kRecheckFrames = 30;
	inline int ClearStrikes = 0, ListStrikes = 0;	// WRONG in a row, per half
	inline UInt32 RecheckAt = 0;					// a LiveFrames to check again on (0 none)
	inline FailOnce::Latch CheckFail{ "CapturedNormals self-check" };

	inline bool IsCheckFrame(UInt32 f) { return f == 60 || f == 300 || f == 1200 || (f > 1200 && f % 18000 == 0) || (RecheckAt && f == RecheckAt); }

	// Texels written by a record (alpha 1 .. 2: the world's and the actors' / gear's codes) in a 64 x 64 point-sampled copy of the target;
	// -1 if it could not be read.
	inline int CountWritten(IDirect3DDevice9* d) {
		if (!CheckTarget && (FAILED(d->CreateRenderTarget(64, 64, D3DFMT_A16B16G16R16F, D3DMULTISAMPLE_NONE, 0, FALSE, &CheckTarget, NULL)) ||
			FAILED(d->CreateOffscreenPlainSurface(64, 64, D3DFMT_A16B16G16R16F, D3DPOOL_SYSTEMMEM, &CheckCopy, NULL)))) {
			if (CheckTarget) { CheckTarget->Release(); CheckTarget = nullptr; }
			return -1;
		}
		if (FAILED(d->StretchRect(TextureSurface, NULL, CheckTarget, NULL, D3DTEXF_POINT)) || FAILED(d->GetRenderTargetData(CheckTarget, CheckCopy))) return -1;
		D3DLOCKED_RECT r = {};
		if (FAILED(CheckCopy->LockRect(&r, NULL, D3DLOCK_READONLY))) return -1;
		int written = 0;
		for (int y = 0; y < 64; y++) {
			const UInt16* row = (const UInt16*)((const char*)r.pBits + y * r.Pitch);
			// a world record's code: 1.0, or (2026-11-16 fd-gaps) 1 + q / 1024 up to 1.468 where a GBufferV2 lighting-free record packs fd-sunvis's
			// sun share and F0 into the fraction (Includes/DeferredPixel.hlsl FDSunVisCode): half floats 1.0 <= a < 1.5. Counting only exact 1.0
			// read every such record as unwritten and switched CapturedNormals (and so the deferred renderer) off for the session.
			// (2026-11-16 fd-batchB) and the actors' / gear's code, 2.0 or 2 - q / 1024 down to 1.53 (FDSunVisCode again): 1.5 <= a <= 2.0, so a
			// frame that shows only the player's or an NPC's gear is not read as "not written". Half floats 1.0 .. 2.0 = 0x3C00 .. 0x4000.
			for (int x = 0; x < 64; x++) { const UInt16 a = row[x * 4 + 3]; if (a >= 0x3C00 && a <= 0x4000) written++; }
		}
		CheckCopy->UnlockRect();
		return written;
	}

	// afterList: the second half (templates drew, so something must be written), else the first (the clear must have emptied it)
	inline void CheckResult(bool ok, const char* what, int count, bool afterList) {
		// (2026-11-16 fd-batchB, test only) DebugVar4 904: this after-list check reads WRONG once; 905: every one while it stays set
		if (afterList && ok && (FdTestFault::Broken || FdTestFault::Take(904))) {
			Logger::Log("CapturedNormals: self-check at captured frame %u, %s: %d of 4096 texels written, read as WRONG by test fault %d (DebugVar4)", LiveFrames, what, count, FdTestFault::Broken ? 905 : 904);
			ok = false;
		}
		else Logger::Log("CapturedNormals: self-check at captured frame %u, %s: %d of 4096 texels written -> %s", LiveFrames, what, count, ok ? "ok" : "WRONG");
		int& strikes = afterList ? ListStrikes : ClearStrikes;
		if (ok) {
			if (strikes) Logger::Log("CapturedNormals: self-check %s right again after %d WRONG: the capture stays on", what, strikes);
			strikes = 0;
			if (!ClearStrikes && !ListStrikes) RecheckAt = 0;
			return;
		}
		CheckFailures++;
		if (++strikes < kStrikes) {
			RecheckAt = LiveFrames + kRecheckFrames;
			Logger::Log("[WARNING] CapturedNormals: self-check %s WRONG (%d in a row): checked again at captured frame %u; %d in a row switch the capture off", what, strikes, RecheckAt, kStrikes);
			return;
		}
		Logger::Log("[WARNING] CapturedNormals: the normal target does not update (the driver drops the draws or the clear?): %d self-checks %s in a row read WRONG; the effects keep the depth-rebuilt normals", strikes, what);
		ClearStrikes = ListStrikes = 0;
		RecheckAt = 0;
		Failed = true;
		CheckFail.Set(afterList ? "the target holds nothing after the main list (3 checks in a row)" : "the target is not cleared (3 checks in a row)");
	}

	// 64 x 64 point copy of a target as floats (4096 x 4); false if it could not be read (CheckTarget is CountWritten's).
	inline bool Read64(IDirect3DDevice9* d, IDirect3DSurface9* source, std::vector<float>& out) {
		if (!CheckTarget || !CheckCopy || !source) return false;
		if (FAILED(d->StretchRect(source, NULL, CheckTarget, NULL, D3DTEXF_POINT)) || FAILED(d->GetRenderTargetData(CheckTarget, CheckCopy))) return false;
		D3DLOCKED_RECT r = {};
		if (FAILED(CheckCopy->LockRect(&r, NULL, D3DLOCK_READONLY))) return false;
		out.resize(4096 * 4);
		for (int y = 0; y < 64; y++) D3DXFloat16To32Array(&out[y * 64 * 4], (const D3DXFLOAT16*)((const char*)r.pBits + y * r.Pitch), 64 * 4);
		CheckCopy->UnlockRect();
		return true;
	}
	// The albedo self-check (the check frames; a log line only): albedo texels written against the normals' texels (the plan's "albedo texels =
	// normal texels"), the normal texels left without an albedo (the game's own shaders' pixels are in neither; a DIFFUSE pass owns no pixel),
	// written texels by material class, the skin texels filled, and the mean written albedo.
	inline void AlbedoCheck(IDirect3DDevice9* d) {
		std::vector<float> n, a, m;
		if (!Read64(d, TextureSurface, n) || !Read64(d, AlbedoSurface, a)) return;
		const bool material = MaterialDrawn && MaterialSurface && Read64(d, MaterialSurface, m);
		int normals = 0, albedo = 0, both = 0, normalsOnly = 0, skinNormals = 0, skinAlbedo = 0, byClass[8] = {};
		double mean[3] = {};
		// UNOFFICIAL 2026-11-12 CapturedGrass: the grass's records ([1]: MaterialBuffer g = kGrassMark) against the land's ([0]: class 4):
		// texels, the mean colour and the mean world normal (z up), so one launch tells a colour that is too bright from a normal that is
		// off. Here (the main list's end) only the near tufts are in (GrassFirst's early run); the fading ones are drawn after it.
		int count[2] = {}; double colour[2][3] = {}, up[2] = {};
		for (int i = 0; i < 4096; i++) {
			const bool hasN = n[i * 4 + 3] > 0.99f, hasA = a[i * 4 + 3] > 0.99f, skin = n[i * 4 + 3] >= 1.5f;
			normals += hasN; albedo += hasA; both += hasN && hasA; normalsOnly += hasN && !hasA; skinNormals += skin; skinAlbedo += skin && hasA;
			if (hasA) { for (int c = 0; c < 3; c++) mean[c] += a[i * 4 + c]; if (material && MaterialWritten(&m[i * 4])) { const int cls = MaterialClass(&m[i * 4]); if (cls <= 7) byClass[(std::max)(cls, 0)]++; } }	// (2026-11-14 GBufferV2: either layout)
			if (GrassNow && material && hasA && hasN && MaterialWritten(&m[i * 4])) {
				const bool isGrass = m[i * 4] > 0.5f && (fabsf(m[i * 4 + 1] - (V2Compiled ? kGrassMarkV2 : kGrassMark)) < 0.1f ||
					(FoliageGrassRange && m[i * 4 + 1] > 31.99f && m[i * 4 + 1] < 1024.0f));	// (2026-11-14 the grass deferred record's range)
				const int w = isGrass ? 1 : MaterialClass(&m[i * 4]) == 4 ? 0 : -1;
				if (w >= 0) {
					// the octahedral normal's z (Includes/Shadow.hlsl ShadowEncodeNormal, as the merge shader decodes it)
					float x = n[i * 4], y = n[i * 4 + 1], z = 1.0f - fabsf(x) - fabsf(y);
					const float fold = (std::min)((std::max)(-z, 0.0f), 1.0f);
					x += x >= 0.0f ? -fold : fold; y += y >= 0.0f ? -fold : fold;
					const float len = sqrtf(x * x + y * y + z * z);
					count[w]++; up[w] += len > 1.0e-6f ? z / len : 0.0f;
					for (int c = 0; c < 3; c++) colour[w][c] += a[i * 4 + c];
				}
			}
		}
		const double k = albedo ? 1.0 / albedo : 0.0;
		Logger::Log("AlbedoBuffer: self-check at captured frame %u: albedo texels %d of 4096 (normals %d, both %d, normals without albedo %d = %.1f %%); skin texels %d, filled %d; written by class 0:%d object:%d gear:%d cloth:%d land:%d plant:%d skin:%d grass:%d; mean albedo %.3f %.3f %.3f",
			LiveFrames, albedo, normals, both, normalsOnly, normals ? 100.0 * normalsOnly / normals : 0.0, skinNormals, skinAlbedo,
			byClass[0], byClass[1], byClass[2], byClass[3], byClass[4], byClass[5], byClass[6], byClass[7], mean[0] * k, mean[1] * k, mean[2] * k);
		if (GrassNow && material) {
			const double g = count[1] ? 1.0 / count[1] : 0.0, l = count[0] ? 1.0 / count[0] : 0.0;
			// UNOFFICIAL 2026-11-12 CapturedGrassTint: the two numbers the grass's colour is judged by, so a run reads from the log alone: each
			// one's luma (Rec. 709 weights on the linear mean) and red over blue (how orange it is), and the grass's over the land's. The
			// second pass's record (the texture alone) read 1.18 and 2.10 at Goodsprings at noon (grass 0.181 0.121 0.056, land 0.125 0.107
			// 0.081): tufts too bright and too orange under the path tracer. The keys are the ones the grass shader had (GrassColourKeys).
			double luma[2], redBlue[2];
			for (int w = 0; w < 2; w++) {
				const double perTexel = w ? g : l;
				luma[w] = (0.2126 * colour[w][0] + 0.7152 * colour[w][1] + 0.0722 * colour[w][2]) * perTexel;
				redBlue[w] = colour[w][2] > 0.0 ? colour[w][0] / colour[w][2] : 0.0;
			}
			Logger::Log("CapturedGrass: self-check at captured frame %u (the near tufts only; linear colours): grass %d texels, mean colour %.3f %.3f %.3f (luma %.3f, R/B %.2f), mean normal up %.2f; land %d texels, mean colour %.3f %.3f %.3f (luma %.3f, R/B %.2f), mean normal up %.2f; GRASS / LAND: luma %.2f, R/B %.2f, with CapturedGrassTint %.2f, CapturedGrassColour %.2f, CapturedGrassSaturation %.2f (the texture alone read luma 1.18 and R/B 2.10 at Goodsprings at noon: too bright, too orange; a normal far from the land's: the normal is at fault)",
				LiveFrames, count[1], colour[1][0] * g, colour[1][1] * g, colour[1][2] * g, luma[1], redBlue[1], up[1] * g,
				count[0], colour[0][0] * l, colour[0][1] * l, colour[0][2] * l, luma[0], redBlue[0], up[0] * l,
				luma[0] > 0.0 ? luma[1] / luma[0] : 0.0, redBlue[0] > 0.0 ? redBlue[1] / redBlue[0] : 0.0,
				GrassColourKeys[0], GrassColourKeys[1], GrassColourKeys[2]);
		}
	}

	// Read once (the shaders are compiled with it): any reader's switch at startup.
	inline bool CompiledIn() {
		if (!Checked) {
			Checked = true;
			Compiled = TheSettingManager->GetSettingI("Shaders.SnowAccumulation.Main", "CapturedNormals") != 0 ||
				TheSettingManager->GetSettingI("Main.Main.Quality", "CapturedNormals") != 0;
		}
		return Compiled;
	}

	// UNOFFICIAL 2026-10-28 [Main.Main.Quality] CapturedNormals: the captured normals written INTO TESR_NormalsBuffer (the normal rebuilt
	// from depth that every effect reads: AmbientOcclusion, AmbientLight, the sun / cloud / far shadows, Specular, point-shadow darkening,
	// SnowAccumulation...), right after it is drawn and smoothed (ShaderManager::RenderDepthAndNormals). One full-screen pass: where this
	// pixel's surface wrote its smooth normal, the distance matches the depth buffer and the normal is within ~60 degrees of the depth
	// buffer's own facet there (CapturedNormalAgrees' rule), the buffer takes it (view space, as Normals.fx writes it); every other pixel
	// is clipped and keeps the depth-rebuilt normal. Live (the templates need the switch at startup).
	inline bool MergeWanted() { return CompiledIn() && TheSettingManager->GetSettingI("Main.Main.Quality", "CapturedNormals") != 0; }
	inline IDirect3DPixelShader9* MergePS = nullptr;
	inline bool MergeFailed = false, MergeLogged = false;
	inline UInt32 Merges = 0;

	inline bool MergeIntoNormals(IDirect3DDevice9* d, IDirect3DSurface9* normals, IDirect3DBaseTexture9* depth) {
		if (MergeFailed || Failed || Data.x <= 0.0f || !Texture || !normals || !depth) return false;
		auto constant = [](const char* name) -> const D3DXVECTOR4* {
			auto found = TheShaderManager->ConstantsTable.find(name);
			return found == TheShaderManager->ConstantsTable.end() ? nullptr : found->second;
		};
		const D3DXMATRIX* view = (const D3DXMATRIX*)constant("TESR_ViewTransform");
		const D3DXMATRIX* proj = (const D3DXMATRIX*)constant("TESR_ProjectionTransform");
		const D3DXVECTOR4* camera = constant("TESR_CameraData");
		const D3DXVECTOR4* scale = constant("TESR_RenderScale");
		if (!view || !proj || !camera) return false;
		if (!MergePS) {
			// c0: 1 / proj _11, 1 / proj _22, far, 0; c1: TESR_RenderScale (xy drawn share, zw its reciprocal); c2: distance share, units,
			// least cosine; c3-c6: TESR_ViewTransform as the effects see it (row_major: rows uploaded as stored).
			// UNOFFICIAL 2026-11-12 CapturedGrass: s2 the MaterialBuffer, read only with c2.w = 1 (grass records written this frame and
			// CapturedGrassNormals on): a pixel the grass shader marked (g = kGrassMark, written) takes the record's normal without the
			// facet test (a card's facet is level, the tuft's lighting normal about straight up: see GrassOwnBeam's comment, 1). c2.w 0:
			// the same result as before on every pixel.
			static const char* kPS =
				"sampler2D C : register(s0); sampler2D D : register(s1); sampler2D M : register(s2);"
				"float4 P : register(c0); float4 R : register(c1); float4 G : register(c2); row_major float4x4 V : register(c3);"
				"float4 main(float2 uv : TEXCOORD0) : COLOR0 {"
				"  float4 c = tex2D(C, uv); float z = tex2D(D, uv).x * P.z;"
				"  float grass = 0.0f;"
				"  if (G.w > 0.5f) { float4 m = tex2Dlod(M, float4(uv, 0.0f, 0.0f)); grass = m.a > 0.5f && m.r > 0.5f && abs(m.g - 0.5f) < 0.1f ? 1.0f : 0.0f; }"
				"  float2 s = uv * R.zw;"
				"  float3 vp = float3((s.x * 2.0f - 1.0f) * P.x, (1.0f - s.y * 2.0f) * P.y, 1.0f) * z;"
				"  float3 fn = cross(ddy(vp), ddx(vp)); fn = normalize(fn + float3(0.0f, 0.0f, 1.0e-20f)); fn *= dot(fn, vp) > 0.0f ? -1.0f : 1.0f;"
				"  float3 n = float3(c.rg, 1.0f - abs(c.r) - abs(c.g)); float t = saturate(-n.z);"
				"  n.xy += float2(n.x >= 0.0f ? -t : t, n.y >= 0.0f ? -t : t); n = normalize(n);"
				"  float3 nv = normalize(mul(n, (float3x3)V));"
				"  float d = length(vp);"
				"  clip(c.a > 0.99f && abs(c.b - d) <= d * G.x + G.y && (dot(nv, fn) > G.z || grass > 0.5f) && z < P.z * 0.99f ? 1.0f : -1.0f);"
				"  return float4(nv * 0.5f + 0.5f, 1.0f); }";
			// UNOFFICIAL 2026-11-14 GBufferV2: there the grass's mark is g = 4.5 (kGrassMarkV2) and "written" is the class (r), a being a normal's y
			std::string source = kPS;
			if (V2Compiled) {
				const std::string oldTest = "m.a > 0.5f && m.r > 0.5f && abs(m.g - 0.5f) < 0.1f";
				// (2026-11-14 DeferredFoliage: + the grass deferred record's COLOR2.g range [32, 1024), only while such records are made)
				const std::string newTest = FoliageGrassRange ? "m.r > 0.5f && (abs(m.g - 4.5f) < 0.1f || (m.g > 31.99f && m.g < 1024.0f))" : "m.r > 0.5f && abs(m.g - 4.5f) < 0.1f";
				const size_t at = source.find(oldTest);
				if (at != std::string::npos) source.replace(at, oldTest.size(), newTest);
			}
			ID3DXBuffer* code = nullptr; ID3DXBuffer* errors = nullptr;
			if (FAILED(D3DXCompileShader(source.c_str(), (UINT)source.size(), NULL, NULL, "main", "ps_3_0", 0, &code, &errors, NULL)) || !code ||
				FAILED(d->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &MergePS))) {
				Logger::Log("[ERROR] CapturedNormals: the merge shader failed (%s): the normals buffer stays depth-rebuilt", errors ? (const char*)errors->GetBufferPointer() : "?");
				MergeFailed = true;
			}
			if (errors) errors->Release();
			if (code) code->Release();
			if (MergeFailed) return false;
		}
		D3DSURFACE_DESC t = {};
		normals->GetDesc(&t);

		// Save what the chain relies on (as LinearScene::Convert does).
		IDirect3DVertexShader9* vs = nullptr; IDirect3DPixelShader9* ps = nullptr; IDirect3DVertexDeclaration9* decl = nullptr;
		IDirect3DVertexBuffer9* vb = nullptr; UINT vbOffset = 0, vbStride = 0; IDirect3DBaseTexture9* tex[3] = {};
		IDirect3DSurface9* rt0 = nullptr; D3DVIEWPORT9 vp = {};
		DWORD fvf = 0; float saved[7][4] = {};
		// UNOFFICIAL 2026-11-12 CapturedGrass: the grass's own normal on its marked pixels (stage 2: the MaterialBuffer), only in a frame
		// whose grass records are written and that holds the material buffer; otherwise two stages and c2.w 0, as before
		const bool grass = GrassMergeNow && MaterialTexture && MaterialData.x > 0.5f;
		const DWORD stages = grass ? 3 : 2;
		d->GetVertexShader(&vs); d->GetPixelShader(&ps); d->GetVertexDeclaration(&decl); d->GetFVF(&fvf);
		d->GetStreamSource(0, &vb, &vbOffset, &vbStride); d->GetTexture(0, &tex[0]); d->GetTexture(1, &tex[1]); if (grass) d->GetTexture(2, &tex[2]);
		d->GetRenderTarget(0, &rt0); d->GetViewport(&vp); d->GetPixelShaderConstantF(0, &saved[0][0], 7);
		static const D3DRENDERSTATETYPE kRS[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_ALPHATESTENABLE, D3DRS_STENCILENABLE,
			D3DRS_SCISSORTESTENABLE, D3DRS_CULLMODE, D3DRS_COLORWRITEENABLE, D3DRS_FILLMODE, D3DRS_SRGBWRITEENABLE, D3DRS_FOGENABLE, D3DRS_CLIPPLANEENABLE };
		const DWORD want[] = { FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, D3DCULL_NONE, 0xFu, D3DFILL_SOLID, FALSE, FALSE, 0 };
		static_assert(sizeof(kRS) / sizeof(kRS[0]) == sizeof(want) / sizeof(want[0]), "one value per state");
		DWORD savedRS[sizeof(kRS) / sizeof(kRS[0])];
		for (unsigned i = 0; i < sizeof(kRS) / sizeof(kRS[0]); i++) { if (FAILED(d->GetRenderState(kRS[i], &savedRS[i]))) savedRS[i] = want[i]; if (savedRS[i] != want[i]) d->SetRenderState(kRS[i], want[i]); }
		static const D3DSAMPLERSTATETYPE kSS[] = { D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE };
		const DWORD wantS[] = { D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, FALSE };
		DWORD savedS[3][sizeof(kSS) / sizeof(kSS[0])];
		for (DWORD s = 0; s < stages; s++)
			for (unsigned i = 0; i < sizeof(kSS) / sizeof(kSS[0]); i++) { if (FAILED(d->GetSamplerState(s, kSS[i], &savedS[s][i]))) savedS[s][i] = wantS[i]; if (savedS[s][i] != wantS[i]) d->SetSamplerState(s, kSS[i], wantS[i]); }

		d->SetRenderTarget(0, normals);
		D3DVIEWPORT9 full = { 0, 0, t.Width, t.Height, 0.0f, 1.0f };
		d->SetViewport(&full);
		d->SetVertexShader(nullptr);
		d->SetPixelShader(MergePS);
		const float c[7][4] = {
			{ 1.0f / proj->_11, 1.0f / proj->_22, camera->y, 0.0f },
			{ scale ? scale->x : 1.0f, scale ? scale->y : 1.0f, scale ? scale->z : 1.0f, scale ? scale->w : 1.0f },
			{ Data.y, Data.z, Data.w, grass ? 1.0f : 0.0f },	// (w: 2026-11-12 CapturedGrass, the marked grass pixels take their record's normal)
			{ view->_11, view->_12, view->_13, view->_14 }, { view->_21, view->_22, view->_23, view->_24 },
			{ view->_31, view->_32, view->_33, view->_34 }, { view->_41, view->_42, view->_43, view->_44 } };
		d->SetPixelShaderConstantF(0, &c[0][0], 7);
		d->SetTexture(0, Texture);
		d->SetTexture(1, depth);
		if (grass) d->SetTexture(2, MaterialTexture);
		d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
		const float x1 = (float)t.Width - 0.5f, y1 = (float)t.Height - 0.5f;
		const float quad[4][6] = { { -0.5f, -0.5f, 0, 1, 0, 0 }, { x1, -0.5f, 0, 1, 1, 0 }, { -0.5f, y1, 0, 1, 0, 1 }, { x1, y1, 0, 1, 1, 1 } };
		d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));

		// Put it all back.
		for (DWORD s = 0; s < stages; s++)
			for (unsigned i = 0; i < sizeof(kSS) / sizeof(kSS[0]); i++) if (savedS[s][i] != wantS[i]) d->SetSamplerState(s, kSS[i], savedS[s][i]);
		for (unsigned i = 0; i < sizeof(kRS) / sizeof(kRS[0]); i++) if (savedRS[i] != want[i]) d->SetRenderState(kRS[i], savedRS[i]);
		if (rt0) { d->SetRenderTarget(0, rt0); rt0->Release(); }
		d->SetViewport(&vp);
		for (DWORD s = 0; s < stages; s++) { d->SetTexture(s, tex[s]); if (tex[s]) tex[s]->Release(); }
		d->SetPixelShaderConstantF(0, &saved[0][0], 7);
		if (decl) { d->SetVertexDeclaration(decl); decl->Release(); } else d->SetFVF(fvf);
		d->SetStreamSource(0, vb, vbOffset, vbStride); if (vb) vb->Release();
		d->SetVertexShader(vs); if (vs) vs->Release();
		d->SetPixelShader(ps); if (ps) ps->Release();
		if (!MergeLogged) { MergeLogged = true; Logger::Log("CapturedNormals: merged into the normals buffer (%ux%u) for every effect that reads it", t.Width, t.Height); }
		Merges++;
		return true;
	}

	typedef HRESULT(STDMETHODCALLTYPE* DIPFn)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
	typedef HRESULT(STDMETHODCALLTYPE* DPFn)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
	typedef HRESULT(STDMETHODCALLTYPE* DPUPFn)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, CONST void*, UINT);
	typedef HRESULT(STDMETHODCALLTYPE* DIPUPFn)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, CONST void*, D3DFORMAT, CONST void*, UINT);
	typedef HRESULT(STDMETHODCALLTYPE* ClearFn)(IDirect3DDevice9*, DWORD, CONST D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
	inline DIPFn NextDIP = nullptr;  inline DPFn NextDP = nullptr;  inline DPUPFn NextDPUP = nullptr;  inline DIPUPFn NextDIPUP = nullptr;
	inline ClearFn NextClear = nullptr;

	inline void Release() {
		if (MsaaSurface) { MsaaSurface->Release(); MsaaSurface = nullptr; }
		if (TextureSurface) { TextureSurface->Release(); TextureSurface = nullptr; }
		if (Texture) { Texture->Release(); Texture = nullptr; }
		if (MaterialMsaa) { MaterialMsaa->Release(); MaterialMsaa = nullptr; }
		if (MaterialSurface) { MaterialSurface->Release(); MaterialSurface = nullptr; }
		if (MaterialTexture) { MaterialTexture->Release(); MaterialTexture = nullptr; }
		if (AlbedoMsaa) { AlbedoMsaa->Release(); AlbedoMsaa = nullptr; }	// UNOFFICIAL 2026-10-31 AlbedoBuffer
		if (AlbedoSurface) { AlbedoSurface->Release(); AlbedoSurface = nullptr; }
		if (AlbedoTexture) { AlbedoTexture->Release(); AlbedoTexture = nullptr; }
		Made = {};
		GrassArmed = GrassLateOk = GrassLateBound = false; GrassLateTarget = nullptr;	// UNOFFICIAL 2026-11-12 (the late grass binds: gone with the targets)
		GrassOwnOk = false; GrassOwnTarget = nullptr;	// (PTGrassOwnPixel)
	}

	inline IDirect3DSurface9* DrawSurface() { return MsaaSurface ? MsaaSurface : TextureSurface; }

	inline bool Ensure(IDirect3DDevice9* d, const D3DSURFACE_DESC& t) {
		if (Texture && Made.Width == t.Width && Made.Height == t.Height && Made.MultiSampleType == t.MultiSampleType && Made.MultiSampleQuality == t.MultiSampleQuality) return true;
		// UNOFFICIAL 2026-11-02 RetryFailedEffects: the stop counted every size change of the session, so the 5th render-scale or upscaler
		// change switched the normals off for good. With RetryFailedEffects on it counts within 600 captured frames (a real thrash is
		// several a second), and the stop is a FailOnce latch: tried again after the next cell change. Off: as before.
		if (Texture && FailOnce::RetryOn() && LiveFrames - RecreationsSince > 600) { RecreationsSince = LiveFrames; Recreations = 0; }
		if (Texture && ++Recreations > 4) {	// the world's target never changes; something else draws its list into another full-screen target
			Logger::Log("[WARNING] CapturedNormals: the world's main list draws into changing targets");
			Release();
			Failed = true;
			MakeFail.Set("the world's main list draws into changing targets");
			return false;
		}
		Release();
		bool ok = SUCCEEDED(d->CreateTexture(t.Width, t.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &Texture, NULL)) &&
			SUCCEEDED(Texture->GetSurfaceLevel(0, &TextureSurface));
		// MRT targets must share the scene's multisampling: a multisampled copy, resolved into the texture after the list.
		if (ok && t.MultiSampleType != D3DMULTISAMPLE_NONE)
			ok = SUCCEEDED(d->CreateRenderTarget(t.Width, t.Height, D3DFMT_A16B16G16R16F, t.MultiSampleType, t.MultiSampleQuality, FALSE, &MsaaSurface, NULL));
		if (ok && MaterialCompiledIn()) {	// UNOFFICIAL 2026-10-29 MaterialBuffer: its target, alike (failing it leaves the normals working)
			bool made = SUCCEEDED(d->CreateTexture(t.Width, t.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &MaterialTexture, NULL)) &&
				SUCCEEDED(MaterialTexture->GetSurfaceLevel(0, &MaterialSurface));
			if (made && t.MultiSampleType != D3DMULTISAMPLE_NONE)
				made = SUCCEEDED(d->CreateRenderTarget(t.Width, t.Height, D3DFMT_A16B16G16R16F, t.MultiSampleType, t.MultiSampleQuality, FALSE, &MaterialMsaa, NULL));
			if (!made) {
				Logger::Log("[WARNING] MaterialBuffer: could not make its %ux%u target: off for the session (the captured normals keep working)", t.Width, t.Height);
				if (MaterialMsaa) { MaterialMsaa->Release(); MaterialMsaa = nullptr; }
				if (MaterialSurface) { MaterialSurface->Release(); MaterialSurface = nullptr; }
				if (MaterialTexture) { MaterialTexture->Release(); MaterialTexture = nullptr; }
				MaterialCompiled = false;
			}
		}
		if (ok && MaterialCompiled && AlbedoCompiledIn()) {	// UNOFFICIAL 2026-10-31 AlbedoBuffer: its target, alike (failing it leaves the other two working)
			bool made = SUCCEEDED(d->CreateTexture(t.Width, t.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &AlbedoTexture, NULL)) &&
				SUCCEEDED(AlbedoTexture->GetSurfaceLevel(0, &AlbedoSurface));
			if (made && t.MultiSampleType != D3DMULTISAMPLE_NONE)
				made = SUCCEEDED(d->CreateRenderTarget(t.Width, t.Height, D3DFMT_A16B16G16R16F, t.MultiSampleType, t.MultiSampleQuality, FALSE, &AlbedoMsaa, NULL));
			if (!made) {
				Logger::Log("[WARNING] AlbedoBuffer: could not make its %ux%u target: off for the session (the normals and the material buffer keep working)", t.Width, t.Height);
				if (AlbedoMsaa) { AlbedoMsaa->Release(); AlbedoMsaa = nullptr; }
				if (AlbedoSurface) { AlbedoSurface->Release(); AlbedoSurface = nullptr; }
				if (AlbedoTexture) { AlbedoTexture->Release(); AlbedoTexture = nullptr; }
				AlbedoCompiled = false;
			}
		}
		if (!ok) {
			Logger::Log("[ERROR] CapturedNormals: could not make the %ux%u target (msaa %u)", t.Width, t.Height, (unsigned)t.MultiSampleType);
			Release();
			Failed = true;
			MakeFail.Set("could not make its target");
			return false;
		}
		Made = t;
		Logger::Log("CapturedNormals: target %ux%u, msaa %u", t.Width, t.Height, (unsigned)t.MultiSampleType);
		return true;
	}

	// Target 3 on or off. Binding a target past 0 leaves the viewport alone in D3D9; it is put back anyway (cheap, and some layers reset it).
	inline void Bind(IDirect3DDevice9* d) {
		D3DVIEWPORT9 vp; d->GetViewport(&vp);
		d->SetRenderTarget(3, DrawSurface());
		d->SetViewport(&vp);
		Bound = true;
	}
	inline void Unbind(IDirect3DDevice9* d) {
		D3DVIEWPORT9 vp; d->GetViewport(&vp);
		d->SetRenderTarget(3, nullptr);
		if (MaterialBound) {	// UNOFFICIAL 2026-10-29 MaterialBuffer: ours on stage 2 goes too
			IDirect3DSurface9* rt2 = nullptr;
			if (SUCCEEDED(d->GetRenderTarget(2, &rt2)) && rt2) rt2->Release();
			if (rt2 && rt2 == MaterialDrawSurface()) d->SetRenderTarget(2, nullptr);
			d->SetRenderState(D3DRS_COLORWRITEENABLE2, 0xF);
			MaterialBound = false;
		}
		if (AlbedoBound) {	// UNOFFICIAL 2026-10-31 AlbedoBuffer: ours on stage 1 goes too
			IDirect3DSurface9* rt1 = nullptr;
			if (SUCCEEDED(d->GetRenderTarget(1, &rt1)) && rt1) rt1->Release();
			if (rt1 && rt1 == AlbedoDrawSurface()) d->SetRenderTarget(1, nullptr);
			d->SetRenderState(D3DRS_COLORWRITEENABLE1, 0xF);
			AlbedoBound = false;
		}
		d->SetViewport(&vp);
		d->SetRenderState(D3DRS_COLORWRITEENABLE3, 0xF);
		Bound = false;
		Unbinds++;
	}

	// UNOFFICIAL 2026-10-29 CapturedNormalsLeanHook ([Main.Main.Quality], live; ROOTPLAN round 3 step 8, work\NVR-p84cpu docs\next\
	// CPU-P79-VS-P84.md: BeforeDraw cost 1.5 ms of the Ultra-Luxe's main thread): the hook asked the device for render targets 0 / 2 / 3,
	// the pixel shader and up to six render states on every draw (each a call through DXVK's device lock) and set COLORWRITEENABLE3 on
	// every draw. Lean: those values are kept from the device's own Set calls (SetRenderTarget / SetRenderState / SetPixelShader hooks,
	// installed with the setting), read from the device once at the span's start (Resync) and again after a state block's Apply
	// (PassSums' fold-in, the one place in the list that changes state without them), and the write masks are set only when they change.
	// The same decisions from the same values: the picture is unchanged. Off: the per-draw device queries as before.
	using TrackedState = SharedDrawState::TrackedState;
	inline TrackedState& T = SharedDrawState::T;
	inline bool LeanWanted = false, LeanHooked = false, Tracked = false;
	inline UInt32 LeanFrames = 0, LeanSkippedSets = 0;
	typedef HRESULT(STDMETHODCALLTYPE* SetRTFn)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
	typedef HRESULT(STDMETHODCALLTYPE* SetRSFn)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
	typedef HRESULT(STDMETHODCALLTYPE* SetPSFn)(IDirect3DDevice9*, IDirect3DPixelShader9*);
	inline SetRTFn NextSetRT = nullptr;  inline SetRSFn NextSetRS = nullptr;  inline SetPSFn NextSetPS = nullptr;
	inline HRESULT STDMETHODCALLTYPE SetRTHook(IDirect3DDevice9* d, DWORD i, IDirect3DSurface9* s) {
		const HRESULT r = NextSetRT(d, i, s);
		if (i < 4 && SUCCEEDED(r)) T.rt[i] = s;
		return r;
	}
	inline HRESULT STDMETHODCALLTYPE SetRSHook(IDirect3DDevice9* d, D3DRENDERSTATETYPE st, DWORD v) {
		switch (st) {
		case D3DRS_COLORWRITEENABLE: T.colorWrite = v; break;
		case D3DRS_ALPHABLENDENABLE: T.blend = v; break;
		case D3DRS_SRCBLEND: T.src = v; break;
		case D3DRS_DESTBLEND: T.dst = v; break;
		case D3DRS_COLORWRITEENABLE2: T.colorWrite2 = v; break;
		case D3DRS_COLORWRITEENABLE3: T.colorWrite3 = v; break;
		case D3DRS_COLORWRITEENABLE1: T.colorWrite1 = v; break;	// UNOFFICIAL 2026-10-31 AlbedoBuffer
		default: break;
		}
		return NextSetRS(d, st, v);
	}
	inline HRESULT STDMETHODCALLTYPE SetPSHook(IDirect3DDevice9* d, IDirect3DPixelShader9* ps) {
		const HRESULT r = NextSetPS(d, ps);
		if (SUCCEEDED(r)) T.ps = ps;
		return r;
	}
	// Everything tracked, from the device itself (the span's start; after a state block).
	inline void Resync(IDirect3DDevice9* d) {
		for (DWORD i = 0; i < 4; i++) { IDirect3DSurface9* s = nullptr; if (SUCCEEDED(d->GetRenderTarget(i, &s)) && s) s->Release(); T.rt[i] = s; }
		IDirect3DPixelShader9* ps = nullptr; d->GetPixelShader(&ps); if (ps) ps->Release(); T.ps = ps;
		d->GetRenderState(D3DRS_COLORWRITEENABLE, &T.colorWrite); d->GetRenderState(D3DRS_ALPHABLENDENABLE, &T.blend);
		d->GetRenderState(D3DRS_SRCBLEND, &T.src); d->GetRenderState(D3DRS_DESTBLEND, &T.dst);
		d->GetRenderState(D3DRS_COLORWRITEENABLE2, &T.colorWrite2); d->GetRenderState(D3DRS_COLORWRITEENABLE3, &T.colorWrite3);
		d->GetRenderState(D3DRS_COLORWRITEENABLE1, &T.colorWrite1);	// UNOFFICIAL 2026-10-31 AlbedoBuffer
		Tracked = true;
	}
	inline void AfterStateBlock(IDirect3DDevice9* d) { if (InSpan && Tracked) Resync(d); }

	// UNOFFICIAL 2026-11-04 [Main.Main.Performance] DrawStateTracking (live; cpu-workers follow-up, bench\deep-roadmap\cpuworkers\REPORT.md):
	// the same lean tracking on native D3D9 and stock DXVK, where the device-table hooks above cannot be trusted (the native runtime puts its
	// own entries back between our reinstalls, and a Set call made meanwhile left a stale record). Here the device's own SetRenderTarget and
	// SetRenderState BODIES are detoured (the code every table entry ends in, so a restored entry still lands in it), and the pixel shader,
	// state-block Apply / EndStateBlock and Reset come from ConstantFilter's detours of those bodies (SharedDrawState::PixelShaderSet /
	// StateLost). A lost state makes the record stale: it is read from the device again at the next draw (never inside those calls: the
	// native runtime crashed on device calls made there, ConstantFilter 2026-10-27). Every CheckEvery-th draw the record is compared with
	// the device (DrawStateTrackingCheck, 0 = every 1024th): a difference is logged and the tracking stays off for the session (the per-draw
	// device queries, as before). Measured before: the queries cost ~5 % of the Ultra-Luxe's main thread on native D3D9 (~1 ms).
	inline bool BodyTracked = false, BodyFailed = false, BodyWanted = false, BodyDisabled = false;
	inline SetRTFn OrigBodySetRT = nullptr;
	inline SetRSFn OrigBodySetRS = nullptr;
	inline UInt32 BodyChecks = 0, BodyMismatches = 0, BodyDrawCounter = 0, BodyResyncs = 0, BodyCheckEvery = 1024;
	inline HRESULT STDMETHODCALLTYPE BodySetRT(IDirect3DDevice9* d, DWORD i, IDirect3DSurface9* s) {
		const HRESULT r = OrigBodySetRT(d, i, s);
		if (i < 4 && SUCCEEDED(r)) T.rt[i] = s;
		return r;
	}
	inline HRESULT STDMETHODCALLTYPE BodySetRS(IDirect3DDevice9* d, D3DRENDERSTATETYPE st, DWORD v) {
		switch (st) {
		case D3DRS_COLORWRITEENABLE: T.colorWrite = v; break;
		case D3DRS_ALPHABLENDENABLE: T.blend = v; break;
		case D3DRS_SRCBLEND: T.src = v; break;
		case D3DRS_DESTBLEND: T.dst = v; break;
		case D3DRS_COLORWRITEENABLE2: T.colorWrite2 = v; break;
		case D3DRS_COLORWRITEENABLE3: T.colorWrite3 = v; break;
		case D3DRS_COLORWRITEENABLE1: T.colorWrite1 = v; break;
		default: break;
		}
		return OrigBodySetRS(d, st, v);
	}
	inline void BodyPixelShader(IDirect3DPixelShader9* ps) { T.ps = ps; }
	inline void BodyStateLost() { if (BodyTracked) { Tracked = false; SharedDrawState::Valid = false; } }
	inline bool BodyLean() { return BodyTracked && BodyWanted && !BodyDisabled; }
	// Once, with the device: the bodies the table points at now, only when they are the graphics runtime's own code (d3d9.dll, native or
	// DXVK's); anything else (a wrapper on top of it) is left alone and the tracking stays off.
	inline void InstallBodyTracking(IDirect3DDevice9* d) {
		if (BodyTracked || BodyFailed || !d) return;
		BodyFailed = true;	// (until it worked)
		if (!SharedDrawState::ObserversLive) { Logger::Log("DrawStateTracking: ConstantFilter's device detours are not in: off (the per-draw device queries, as before)"); return; }
		void** table = *(void***)d;
		HMODULE runtime = DxvkLoader::Runtime(), owner37 = nullptr, owner57 = nullptr;
		GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)table[37], &owner37);
		GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)table[57], &owner57);
		if (!runtime || owner37 != runtime || owner57 != runtime) {
			Logger::Log("DrawStateTracking: SetRenderTarget / SetRenderState are not d3d9.dll's own here (%p %p, d3d9.dll %p): off (the per-draw device queries, as before)",
				table[37], table[57], (void*)runtime);
			return;
		}
		OrigBodySetRT = (SetRTFn)table[37];
		OrigBodySetRS = (SetRSFn)table[57];
		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		DetourAttach(&(PVOID&)OrigBodySetRT, (PVOID)&BodySetRT);
		DetourAttach(&(PVOID&)OrigBodySetRS, (PVOID)&BodySetRS);
		const LONG result = DetourTransactionCommit();
		if (result != NO_ERROR) { Logger::Log("DrawStateTracking: detours failed (%ld): off (the per-draw device queries, as before)", result); return; }
		SharedDrawState::PixelShaderSet = &BodyPixelShader;
		SharedDrawState::StateLost = &BodyStateLost;
		BodyFailed = false;
		BodyTracked = true;
		Logger::Log("DrawStateTracking: on (the device's SetRenderTarget / SetRenderState bodies detoured; render targets, pixel shader and blend states kept from them)");
	}
	// The record against the device (BeforeDraw, every CheckEvery-th draw while the record is used): false on any difference.
	inline bool BodyVerify(IDirect3DDevice9* d) {
		TrackedState now = {};
		for (DWORD i = 0; i < 4; i++) { IDirect3DSurface9* s = nullptr; if (SUCCEEDED(d->GetRenderTarget(i, &s)) && s) s->Release(); now.rt[i] = s; }
		IDirect3DPixelShader9* ps = nullptr; d->GetPixelShader(&ps); if (ps) ps->Release(); now.ps = ps;
		d->GetRenderState(D3DRS_COLORWRITEENABLE, &now.colorWrite); d->GetRenderState(D3DRS_ALPHABLENDENABLE, &now.blend);
		d->GetRenderState(D3DRS_SRCBLEND, &now.src); d->GetRenderState(D3DRS_DESTBLEND, &now.dst);
		d->GetRenderState(D3DRS_COLORWRITEENABLE2, &now.colorWrite2); d->GetRenderState(D3DRS_COLORWRITEENABLE3, &now.colorWrite3);
		d->GetRenderState(D3DRS_COLORWRITEENABLE1, &now.colorWrite1);
		BodyChecks++;
		const char* which = nullptr;
		for (DWORD i = 0; i < 4 && !which; i++) if (now.rt[i] != T.rt[i]) which = "a render target";
		if (!which && now.ps != T.ps) which = "the pixel shader";
		if (!which && (now.colorWrite != T.colorWrite || now.colorWrite1 != T.colorWrite1 || now.colorWrite2 != T.colorWrite2 || now.colorWrite3 != T.colorWrite3)) which = "a write mask";
		if (!which && (now.blend != T.blend || now.src != T.src || now.dst != T.dst)) which = "the blend";
		if (!which) return true;
		BodyMismatches++;
		if (BodyMismatches <= 5) Logger::Log("[WARNING] DrawStateTracking: the kept record differs from the device (%s) after %u checks: off for this session (the per-draw device queries, as before)", which, BodyChecks);
		return false;
	}
	inline void HookTracking(IDirect3DDevice9* d) {
		if (LeanHooked || !d) return;
		void** table = *(void***)d;
		DWORD old = 0;
		const int slots[3] = { 37, 57, 107 };	// SetRenderTarget, SetRenderState, SetPixelShader
		for (int k = 0; k < 3; k++) if (!VirtualProtect(&table[slots[k]], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return;
		NextSetRT = (SetRTFn)table[37]; table[37] = (void*)&SetRTHook;
		NextSetRS = (SetRSFn)table[57]; table[57] = (void*)&SetRSHook;
		NextSetPS = (SetPSFn)table[107]; table[107] = (void*)&SetPSHook;
		LeanHooked = true;
		PassSums::AfterStateBlock = &AfterStateBlock;
		Logger::Log("CapturedNormals: lean draw hook on (render targets, pixel shader and blend states tracked from the device's Set calls)");
	}
	// DXVK only: the native runtime puts its own device-table entries back now and then (EnsureHooked), and a Set call that slipped past
	// the tracking would leave a stale record; there the hook keeps asking the device.
	inline bool Lean() { return Tracked && ((LeanHooked && LeanWanted && DxvkCompute::Available()) || BodyLean()); }	// (BodyLean: DrawStateTracking, below)
	inline IDirect3DSurface9* TargetNow(IDirect3DDevice9* d, DWORD i) {
		if (Lean()) return T.rt[i];
		IDirect3DSurface9* s = nullptr;
		if (SUCCEEDED(d->GetRenderTarget(i, &s)) && s) s->Release();
		return s;
	}
	inline DWORD StateNow(IDirect3DDevice9* d, D3DRENDERSTATETYPE st, DWORD fallback) {
		if (Lean()) switch (st) {
			case D3DRS_COLORWRITEENABLE: return T.colorWrite;
			case D3DRS_ALPHABLENDENABLE: return T.blend;
			case D3DRS_SRCBLEND: return T.src;
			case D3DRS_DESTBLEND: return T.dst;
			default: break;
		}
		DWORD v = fallback;
		if (FAILED(d->GetRenderState(st, &v))) v = fallback;
		return v;
	}
	// A write mask: lean, only when the tracked value differs (our own Set call keeps the record right).
	inline void SetMask(IDirect3DDevice9* d, D3DRENDERSTATETYPE st, DWORD v) {
		if (Lean() && (st == D3DRS_COLORWRITEENABLE3 ? T.colorWrite3 : st == D3DRS_COLORWRITEENABLE1 ? T.colorWrite1 : T.colorWrite2) == v) { LeanSkippedSets++; return; }	// (COLORWRITEENABLE1: 2026-10-31 AlbedoBuffer)
		d->SetRenderState(st, v);
	}

	// UNOFFICIAL 2026-11-11 BlendedAlpha ([Main.Main.Quality] CapturedBlendedAlpha; a tester's video, Tale of Two Wastelands under the path
	// tracer: rubble piles as the game's bright raster pixels next to path-traced ground, the boundaries along triangles). A writer drawn
	// with the SRCALPHA / INVSRCALPHA blend (Fallout 3's rubble piles and roads fade into the ground by vertex alpha; objects fading in) was
	// taken as "the templates write alpha 1 there, a plain replace" (BeforeDraw). True for the targets' rgb. But MaterialAware reflections
	// (core/ReflMaterial.h SetSceneStates, on by default) run the whole world scene with SEPARATEALPHABLENDENABLE and the alpha blend
	// ZERO / ONE, and D3D9 has ONE blend state for every render target: the blended writer replaced the normal / albedo / material rgb and
	// KEPT each target's alpha. Over a texel nothing had written yet (objects draw before the land) the alpha stayed 0 = "unwritten": every
	// reader took the pixel as not captured, and the path tracer showed the game's own pixel there (RtPathTrace.comp gScreenDrawnElse);
	// over a texel another object had written, the same rubble came out path traced. For such a draw the alpha blend is now ONE / ZERO (the
	// targets' alpha is the shader's: written) and the scene target's alpha write is masked off (it keeps its alpha, as ZERO / ONE did);
	// AfterDraw puts the three states back. Only the exact ZERO / ONE case (a draw ReflMaterial gave another alpha blend keeps it).
	inline bool BlendedAlphaOn = true, BlendedAlphaActive = false;
	inline DWORD BlendedAlphaSaved[3] = {};	// SRCBLENDALPHA, DESTBLENDALPHA, COLORWRITEENABLE
	inline UInt32 BlendedAlphaDraws = 0;
	// UNOFFICIAL 2026-11-12 CapturedCoverage (keep): the targets' alpha blend is ONE / INVSRCALPHA instead of ONE / ZERO: a record written
	// with alpha 1 gives 1 x 1 + dest x 0 = 1 exactly as before, and one written with alpha 0 (a blended draw below half coverage, a grass
	// texel that is no grass) leaves the texel's alpha as it was, as SRCALPHA / INVSRCALPHA leaves its rgb. Only for writers whose record
	// alpha is 1 or 0: an alpha CODE (actors' 2, the deferred records' 1.01-1.99) would come out as code + dest x (1 - code), so the
	// CodeWriters and the skin template keep ONE / ZERO (keep false).
	inline void BlendedAlphaBegin(IDirect3DDevice9* d, DWORD mask, bool keep) {
		DWORD separate = FALSE, srcA = D3DBLEND_ONE, dstA = D3DBLEND_ZERO;
		if (FAILED(d->GetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, &separate)) || !separate) return;
		if (FAILED(d->GetRenderState(D3DRS_SRCBLENDALPHA, &srcA)) || FAILED(d->GetRenderState(D3DRS_DESTBLENDALPHA, &dstA))) return;
		if (srcA != D3DBLEND_ZERO || dstA != D3DBLEND_ONE) return;
		BlendedAlphaSaved[0] = srcA; BlendedAlphaSaved[1] = dstA; BlendedAlphaSaved[2] = mask;
		d->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
		d->SetRenderState(D3DRS_DESTBLENDALPHA, keep ? D3DBLEND_INVSRCALPHA : D3DBLEND_ZERO);
		if (mask & D3DCOLORWRITEENABLE_ALPHA) d->SetRenderState(D3DRS_COLORWRITEENABLE, mask & 7);
		BlendedAlphaActive = true;
		BlendedAlphaDraws++;
	}

	// UNOFFICIAL 2026-11-12 CapturedCoverage ([Main.Main.Quality] CapturedCoverage, live, a missing key = on; audit item 7: "fading objects
	// recorded as solid"). A template writer drawn with the SRCALPHA / INVSRCALPHA blend wrote its records with alpha 1 on every pixel of
	// the polygon, whatever the draw's own alpha: a Fallout 3 rubble skirt or road edge (vertex alpha down to 0), an object fading in,
	// terrain blending's Smooth seam became solid, hard-edged shapes under the path tracer. The templates now write record alpha 0 where
	// the draw's own alpha is below 0.5 (Includes/CapturedNormals.hlsl CapturedCoverage), which under this blend leaves the texel as it
	// was -- but only for a draw marked as blended, since an OPAQUE draw's alpha can be anything (a texture's alpha that means something
	// else) and must record as before. The mark rides in NVR_SpecFade.z (pixel c211, core/PbrPop.h: FoldHighlights' vertex-colour switch,
	// 0 or 1): for this one draw it is -1 (was 0) or 3 (was 1), which every z > 0.5 test in the shaders reads as before; AfterDraw puts
	// the register back exactly, so PbrPop's "already sent" record stays right. No new register (c180-c223 are taken) and no new define.
	// Opaque draws: nothing is set, the shaders multiply their record alpha by 1.
	inline void CoverageBegin(IDirect3DDevice9* d) {
		if (FAILED(d->GetPixelShaderConstantF(211, CoverageSaved, 1))) return;
		if (CoverageSaved[2] > 1.5f) CoverageSaved[2] = 1.0f; else if (CoverageSaved[2] < -0.5f) CoverageSaved[2] = 0.0f;	// (never a mark left behind)
		const float marked[4] = { CoverageSaved[0], CoverageSaved[1], CoverageSaved[2] > 0.5f ? 3.0f : -1.0f, CoverageSaved[3] };
		d->SetPixelShaderConstantF(211, marked, 1);
		CoverageActive = true;
		CoverageDraws++;
	}

	// UNOFFICIAL 2026-11-12 grass records after the main list (see GrassWriters). GrassLateEnd: our targets off the device (only where
	// they are still ours), the write masks as Unbind leaves them, and under the game's MSAA the three resolved again for the readers.
	inline void GrassLateEnd(IDirect3DDevice9* d) {
		GrassArmed = false;
		if (!GrassLateBound) return;
		GrassLateBound = false;
		D3DVIEWPORT9 vp; d->GetViewport(&vp);
		IDirect3DSurface9* const ours[3] = { AlbedoDrawSurface(), MaterialDrawSurface(), DrawSurface() };
		for (DWORD i = 1; i <= 3; i++) {
			IDirect3DSurface9* rt = nullptr;
			if (SUCCEEDED(d->GetRenderTarget(i, &rt)) && rt) rt->Release();
			if (rt && rt == ours[i - 1]) d->SetRenderTarget(i, nullptr);
		}
		d->SetViewport(&vp);
		d->SetRenderState(D3DRS_COLORWRITEENABLE1, 0xF);
		d->SetRenderState(D3DRS_COLORWRITEENABLE2, 0xF);
		d->SetRenderState(D3DRS_COLORWRITEENABLE3, 0xF);
		if (MsaaSurface && TextureSurface) d->StretchRect(MsaaSurface, NULL, TextureSurface, NULL, D3DTEXF_NONE);
		if (MaterialMsaa && MaterialSurface) d->StretchRect(MaterialMsaa, NULL, MaterialSurface, NULL, D3DTEXF_NONE);
		if (AlbedoMsaa && AlbedoSurface) d->StretchRect(AlbedoMsaa, NULL, AlbedoSurface, NULL, D3DTEXF_NONE);
	}
	// A draw outside the span while a grass shader was the last thing bound (GrassArmed). The device is asked (nothing is tracked here);
	// the first draw that is not a grass writer's ends it, so the rest of the frame's draws pay one flag test.
	inline void GrassLateDraw(IDirect3DDevice9* d) {
		IDirect3DPixelShader9* ps = nullptr;
		d->GetPixelShader(&ps); if (ps) ps->Release();
		if (!ps || !GrassLateOk || !GrassWriters.count(ps) || !TextureSurface || !MaterialSurface || !AlbedoSurface) { GrassLateEnd(d); return; }
		IDirect3DSurface9* rt0 = nullptr;
		if (SUCCEEDED(d->GetRenderTarget(0, &rt0)) && rt0) rt0->Release();
		if (rt0 != GrassLateTarget) { GrassLateEnd(d); GrassArmed = true; return; }	// another target (never with ours bound); the grass may go on after it
		DWORD mask = 0xF, blend = FALSE, src = D3DBLEND_ONE, dst = D3DBLEND_ZERO;
		d->GetRenderState(D3DRS_COLORWRITEENABLE, &mask);
		d->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
		if (blend) { d->GetRenderState(D3DRS_SRCBLEND, &src); d->GetRenderState(D3DRS_DESTBLEND, &dst); }
		// as BeforeDraw: opaque, or the SRCALPHA / INVSRCALPHA blend (grass: its record alpha is 1 or 0, a replace or nothing)
		if (!(mask & 7) || (blend && (src != D3DBLEND_SRCALPHA || dst != D3DBLEND_INVSRCALPHA))) {
			if (GrassLateBound) { d->SetRenderState(D3DRS_COLORWRITEENABLE1, 0); d->SetRenderState(D3DRS_COLORWRITEENABLE2, 0); d->SetRenderState(D3DRS_COLORWRITEENABLE3, 0); }
			return;
		}
		if (!GrassLateBound) {
			IDirect3DSurface9* const ours[3] = { AlbedoDrawSurface(), MaterialDrawSurface(), DrawSurface() };
			for (DWORD i = 1; i <= 3; i++) {	// every stage free (or ours already), or none is touched
				IDirect3DSurface9* rt = nullptr;
				if (SUCCEEDED(d->GetRenderTarget(i, &rt)) && rt) rt->Release();
				if (rt && rt != ours[i - 1]) { GrassLateBusy++; return; }
			}
			D3DVIEWPORT9 vp; d->GetViewport(&vp);
			for (DWORD i = 1; i <= 3; i++) d->SetRenderTarget(i, ours[i - 1]);
			d->SetViewport(&vp);
			GrassLateBound = true;
		}
		d->SetRenderState(D3DRS_COLORWRITEENABLE1, 0xF);
		d->SetRenderState(D3DRS_COLORWRITEENABLE2, 0xF);
		d->SetRenderState(D3DRS_COLORWRITEENABLE3, 0xF);
		if (blend && BlendedAlphaOn) BlendedAlphaBegin(d, mask, true);	// (AfterDraw puts the alpha blend back)
		GrassLateWrites++;
	}
	inline void BeginSpan(IDirect3DDevice9* d);
	// GrassShaders::SetForDraw, at every grass shader bind. part: GrassShaders::DrawPart (1 = GrassFirst's early run, which only
	// GrassFirst::MainListHook makes, in the world scene, right before its own BeginSpan / EndSpan pair round the main list).
	inline void GrassBind(IDirect3DDevice9* d, int part) {
		if (!GrassNow || !Live || InSpan) return;
		if (part == 1) { BeginSpan(d); if (InSpan) GrassEarlySpans++; }
		else if (GrassLateOk) GrassArmed = true;
	}
	// A draw this hook does not see is coming (GPU grass strands through the DXVK extension): nothing of ours may be written by it.
	inline void BeforeForeignDraw(IDirect3DDevice9* d) {
		if (GrassLateBound) { GrassLateEnd(d); return; }
		if (!InSpan) return;
		IDirect3DSurface9* rt3 = nullptr;
		if (SUCCEEDED(d->GetRenderTarget(3, &rt3)) && rt3) rt3->Release();
		if ((rt3 && rt3 == DrawSurface()) || MaterialBound || AlbedoBound) Unbind(d);	// (the next writing draw binds them again)
	}

	// (PTGrassOwnPixel) after a grass draw of a pass Render.cpp marked: no record written by it -> once more into the mask (see GrassPass)
	template <typename DrawFn> inline void GrassOwnDraw(IDirect3DDevice9* d, DrawFn draw) {
		if (InSpan) return;
		IDirect3DPixelShader9* ps = nullptr;
		d->GetPixelShader(&ps); if (ps) ps->Release();
		if (GrassNow && GrassLateBound && ps && GrassWriters.count(ps)) return;	// this draw wrote its own records (GrassLateDraw)
		IDirect3DSurface9* rt0 = nullptr;
		if (SUCCEEDED(d->GetRenderTarget(0, &rt0)) && rt0) rt0->Release();
		if (!rt0 || rt0 != GrassOwnTarget) return;	// a shadow map, a reflection, another scene: not the frame the records describe
		DWORD colour = 0, ztest = D3DZB_FALSE, zwrite = FALSE;
		d->GetRenderState(D3DRS_COLORWRITEENABLE, &colour); d->GetRenderState(D3DRS_ZENABLE, &ztest); d->GetRenderState(D3DRS_ZWRITEENABLE, &zwrite);
		if (!(colour & 7) || ztest == D3DZB_FALSE || !zwrite) return;	// (EQUAL finds only a draw that wrote its depth)
		WaterMask::RedrawGrass(d, GrassPassFrame, draw);
	}

	inline void AfterDraw(IDirect3DDevice9* d) {
		if (CoverageActive) { CoverageActive = false; d->SetPixelShaderConstantF(211, CoverageSaved, 1); }	// UNOFFICIAL 2026-11-12 CapturedCoverage
		if (!BlendedAlphaActive) return;
		BlendedAlphaActive = false;
		d->SetRenderState(D3DRS_SRCBLENDALPHA, BlendedAlphaSaved[0]);
		d->SetRenderState(D3DRS_DESTBLENDALPHA, BlendedAlphaSaved[1]);
		if (BlendedAlphaSaved[2] & D3DCOLORWRITEENABLE_ALPHA) d->SetRenderState(D3DRS_COLORWRITEENABLE, BlendedAlphaSaved[2]);
	}

	inline void BeforeDraw(IDirect3DDevice9* d) {
		if (!InSpan) { if (GrassArmed) GrassLateDraw(d); return; }	// (2026-11-12 grass records after the main list)
		if (BodyLean()) {	// UNOFFICIAL 2026-11-04 DrawStateTracking: a lost record read again; every CheckEvery-th draw compared
			if (!Tracked) { Resync(d); BodyResyncs++; SharedDrawState::Valid = true; }
			else if (++BodyDrawCounter >= BodyCheckEvery) {
				BodyDrawCounter = 0;
				if (!BodyVerify(d)) { BodyDisabled = true; Tracked = false; SharedDrawState::Valid = false; }
			}
		}
		// Is it bound now? Read every draw: LinearScene's conversion takes targets 1-3 off and puts them back itself.
		IDirect3DSurface9* rt3 = TargetNow(d, 3);
		Bound = rt3 && rt3 == DrawSurface();
		IDirect3DSurface9* rt0 = TargetNow(d, 0);	// a key only
		if (rt0 != SpanTarget) { if (Bound) Unbind(d); return; }	// a copy, a shadow map, a mask: never with ours bound
		IDirect3DPixelShader9* ps = nullptr;
		if (Lean()) ps = T.ps;
		else { d->GetPixelShader(&ps); if (ps) ps->Release(); }
		if (ps != LastPS) { LastPS = ps; LastWriter = ps && Writers.count(ps) != 0; LastMaterialWriter = ps && MaterialWriters.count(ps) != 0;
			LastContrastWriter = ps && ContrastWriters.count(ps) != 0;
			LastAlbedoWriter = ps && AlbedoWriters.count(ps) != 0; LastAlbedoLightOnly = ps && AlbedoLightOnlyWriters.count(ps) != 0;	// UNOFFICIAL 2026-10-31 AlbedoBuffer
			// UNOFFICIAL 2026-11-12 foliage records: the grass and leaf shaders write all three (while path tracing replaces the picture)
			LastGrassWriter = GrassNow && ps && GrassWriters.count(ps) != 0; LastLeafWriter = LeafNow && ps && LeafWriters.count(ps) != 0;
			LastFoliageDeferred = FoliageDeferred && ps && FoliageDeferredHandles.count(ps) != 0;	// UNOFFICIAL 2026-11-14 DeferredFoliage
			LastCodeWriter = ps && CodeWriters.count(ps) != 0;	// (2026-11-12 CapturedCoverage: not for these)
			LastBeamMarker = ps && BeamMarkers.count(ps) != 0;	// UNOFFICIAL 2026-11-12 HairAndEyes: a hair shader with the mark output
			if (LastGrassWriter || LastLeafWriter) LastWriter = LastMaterialWriter = LastAlbedoWriter = true; }
		bool write = LastWriter;
		if (EyeDrawNow && BeamNow) { BeamEyes++; if (write) BeamEyesRecorded++; }	// UNOFFICIAL 2026-11-12 HairAndEyes (the log's counts)
		bool contrast = false;	// UNOFFICIAL ShadowContrast: an added lamp pass that gives its lost light to MaterialBuffer's alpha
		if (write) {
			const DWORD mask = StateNow(d, D3DRS_COLORWRITEENABLE, 0xF), blend = StateNow(d, D3DRS_ALPHABLENDENABLE, FALSE);
			const DWORD src = blend ? StateNow(d, D3DRS_SRCBLEND, D3DBLEND_ONE) : D3DBLEND_ONE, dst = blend ? StateNow(d, D3DRS_DESTBLEND, D3DBLEND_ZERO) : D3DBLEND_ZERO;
			// opaque, or a SRCALPHA / INVSRCALPHA fade (the land over objects): the templates write alpha 1 there, a plain replace
			write = (mask & 7) && (!blend || (src == D3DBLEND_SRCALPHA && dst == D3DBLEND_INVSRCALPHA));
			// (2026-11-11 BlendedAlpha) ... of the targets' alpha too, under the scene's separate alpha blend (see BlendedAlphaBegin)
			// (2026-11-12 CapturedCoverage) a blended template writer is marked for its one draw (CoverageBegin), and a record alpha of 0
			// keeps the texel's alpha (the foliage shaders write 1 or 0 by themselves)
			const bool foliage = LastGrassWriter || LastLeafWriter;
			const bool coverage = write && blend && CoverageOn && AlbedoCompiled && LastAlbedoWriter && !LastCodeWriter && !LastAlbedoLightOnly;
			// UNOFFICIAL 2026-11-14 GBufferV2: every blended template writer is marked (also with CapturedCoverage off, also the code writers and the
			// light-only records): a v2 record's alpha is a normal's y, which under this blend would weight its rgb; marked, the shaders write
			// alpha 1 (or CapturedCoverage's 0) and no shading normal (Includes/CapturedNormals.hlsl CapturedShadingNormal)
			const bool v2Mark = V2Compiled && write && blend && LastAlbedoWriter && !foliage;
			if ((coverage || v2Mark) && !foliage) CoverageBegin(d);
			if (write && blend && BlendedAlphaOn) BlendedAlphaBegin(d, mask, coverage || foliage || (v2Mark && !LastCodeWriter));
			// added (ONE / ONE): the alpha adds up across the lamp passes; any other added blend would scale it, so it is left out (counted)
			if (!write && LastContrastWriter && ContrastLive() && (mask & 7) && blend && dst == D3DBLEND_ONE) {	// (2026-11-16 ContrastLive: item 23)
				if (src == D3DBLEND_ONE) contrast = true; else ContrastOtherBlend++;
			}
		}
		if (write) {
			if (!Bound) Bind(d);
			SetMask(d, D3DRS_COLORWRITEENABLE3, 0xF);
			Writes++;
			Drawn = true;
			// UNOFFICIAL 2026-11-04: the light pass lights this object (core/FlashlightLamp.h). (2026-11-07 ptaaa-firstperson) not the view
			// model's draws (VmSpan): the deferred flashlight ran before them, so marking them "lit already" would take the light off the gun
			// (2026-11-12) nor the grass and the leaves: they keep their own beam / the game lamp (their records are only for the path tracer)
			// (2026-11-14 DeferredFoliage) a foliage DEFERRED record's draw: the light pass lights it (its grass beam is compiled out), so the
			// game's flashlight lamp leaves the leaves as it leaves any object the G-buffer holds
			if (!VmSpan && ((!LastGrassWriter && !LastLeafWriter) || LastFoliageDeferred)) FlashlightLamp::NoteWrite();
			if (LastGrassWriter) GrassWrites++; else if (LastLeafWriter) LeafWrites++;
		}
		else if (LastBeamMarker && BeamNow && !VmSpan) {
			// UNOFFICIAL 2026-11-12 HairAndEyes: hair that takes the flashlight's beam itself, drawn in the main list (before the light
			// pass). Its mark: the distance alone (blue), under no blend or the hair's own (see BeamMarkers); the other two records stay
			// masked below (the hair is no writer of theirs).
			const DWORD mask = StateNow(d, D3DRS_COLORWRITEENABLE, 0xF), blend = StateNow(d, D3DRS_ALPHABLENDENABLE, FALSE);
			const DWORD src = blend ? StateNow(d, D3DRS_SRCBLEND, D3DBLEND_ONE) : D3DBLEND_ONE, dst = blend ? StateNow(d, D3DRS_DESTBLEND, D3DBLEND_ZERO) : D3DBLEND_ZERO;
			if ((mask & 7) && (!blend || (src == D3DBLEND_SRCALPHA && dst == D3DBLEND_INVSRCALPHA))) {
				if (!Bound) Bind(d);
				SetMask(d, D3DRS_COLORWRITEENABLE3, D3DCOLORWRITEENABLE_BLUE);
				BeamMarks++;
			}
			else {
				if (Bound) SetMask(d, D3DRS_COLORWRITEENABLE3, 0);
				BeamUnmarked++;
			}
		}
		else if (Bound) {
			SetMask(d, D3DRS_COLORWRITEENABLE3, 0);
			Masked++;
		}
		// UNOFFICIAL 2026-11-07 (ptaaa-firstperson) the first-person span (BeginViewModelSpan): the skin template's draws (normal writers that
		// write no albedo or material: the hands' bare skin) give their own albedo output (COLOR2: saturate(base colour) in the stored curve,
		// which in the world list goes to the skin blur's target; the blur never binds in first person) to VmSkin; every other draw finds
		// stage 2 free of it again, so the material buffer below binds itself there as always
		if (VmSpan && VmSkinSurface) {
			IDirect3DSurface9* rt2 = TargetNow(d, 2);
			if (write && !LastAlbedoWriter && !LastMaterialWriter) {
				if (rt2 != VmSkinSurface && (!rt2 || rt2 == MaterialDrawSurface())) {
					D3DVIEWPORT9 vp; d->GetViewport(&vp); d->SetRenderTarget(2, VmSkinSurface); d->SetViewport(&vp);
					MaterialBound = false;
					rt2 = VmSkinSurface;
				}
				if (rt2 == VmSkinSurface) { SetMask(d, D3DRS_COLORWRITEENABLE2, 0xF); VmSkinDrawn = true; VmSkinDraws++; }
			}
			else if (rt2 == VmSkinSurface) { D3DVIEWPORT9 vp; d->GetViewport(&vp); d->SetRenderTarget(2, nullptr); d->SetViewport(&vp); }
		}
		// UNOFFICIAL 2026-10-29 MaterialBuffer: stage 2 when it is free (or ours), for the material writers' opaque draws; masked otherwise
		if (MaterialTexture && MaterialCompiled) {
			IDirect3DSurface9* rt2 = TargetNow(d, 2);
			const bool ours = rt2 && rt2 == MaterialDrawSurface();
			if (write && LastMaterialWriter) {
				if (!rt2) { D3DVIEWPORT9 vp; d->GetViewport(&vp); d->SetRenderTarget(2, MaterialDrawSurface()); d->SetViewport(&vp); }
				if (!rt2 || ours) { SetMask(d, D3DRS_COLORWRITEENABLE2, 0xF); MaterialBound = true; MaterialDrawn = true; MaterialWrites++; }
				else MaterialBusy++;
			}
			else if (contrast) {	// UNOFFICIAL ShadowContrast: alpha only (the opaque draws' rgb stays)
				if (!rt2) { D3DVIEWPORT9 vp; d->GetViewport(&vp); d->SetRenderTarget(2, MaterialDrawSurface()); d->SetViewport(&vp); }
				if (!rt2 || ours) { SetMask(d, D3DRS_COLORWRITEENABLE2, D3DCOLORWRITEENABLE_ALPHA); MaterialBound = true; ContrastDrawn = true; ContrastWrites++; }
				else MaterialBusy++;
			}
			else if (ours) SetMask(d, D3DRS_COLORWRITEENABLE2, 0);
		}
		// UNOFFICIAL 2026-10-31 AlbedoBuffer: stage 1 when it is free (or ours), for the albedo writers' opaque draws (the skin blur's target there
		// round skin draws is left alone, counted); every other draw with ours bound is masked (the ONLY_SPECULAR highlightCopy above all)
		if (AlbedoTexture && AlbedoCompiled) {
			IDirect3DSurface9* rt1 = TargetNow(d, 1);
			const bool ours = rt1 && rt1 == AlbedoDrawSurface();
			if (write && LastAlbedoWriter) {
				if (!rt1) { D3DVIEWPORT9 vp; d->GetViewport(&vp); d->SetRenderTarget(1, AlbedoDrawSurface()); d->SetViewport(&vp); }
				if (!rt1 || ours) { SetMask(d, D3DRS_COLORWRITEENABLE1, 0xF); AlbedoBound = true; AlbedoDrawn = true; AlbedoWrites++; if (LastAlbedoLightOnly) AlbedoLightOnlyDraws++; }
				else AlbedoBusy++;
			}
			else if (ours) SetMask(d, D3DRS_COLORWRITEENABLE1, 0);
		}
	}

	// UNOFFICIAL 2026-10-29 ShadowContrast (see ContrastStrength): after the main list, the scene x 1 - strength x lost / (lit + lost), lost =
	// MaterialBuffer's alpha - 1 where an opaque draw wrote it. A multiply blend (dest x the factor), so every sample of a multisampled
	// scene is darkened alike; the factor from a copy of the frame (lit) in the space the frame is in now (LinearScene keeps it linear
	// inside the list; otherwise it is decoded with the gamma). Everything the next draw relies on is put back.
	inline void ApplyContrast(IDirect3DDevice9* d, IDirect3DSurface9* target) {
		if (ContrastFailed || !target || !MaterialTexture) return;
		D3DSURFACE_DESC t = {};
		if (FAILED(target->GetDesc(&t))) return;
		if (!ContrastCopyTex || ContrastMade.Width != t.Width || ContrastMade.Height != t.Height || ContrastMade.Format != t.Format) {
			if (ContrastCopy) { ContrastCopy->Release(); ContrastCopy = nullptr; }
			if (ContrastCopyTex) { ContrastCopyTex->Release(); ContrastCopyTex = nullptr; }
			if (FAILED(d->CreateTexture(t.Width, t.Height, 1, D3DUSAGE_RENDERTARGET, t.Format, D3DPOOL_DEFAULT, &ContrastCopyTex, NULL)) ||
				FAILED(ContrastCopyTex->GetSurfaceLevel(0, &ContrastCopy))) {
				Logger::Log("[ERROR] ShadowContrast: could not make the frame's copy (%ux%u format %u): the game's passes keep their shadows as before", t.Width, t.Height, (unsigned)t.Format);
				ContrastFailed = true;
				return;
			}
			ContrastMade = t;
		}
		if (!ContrastPS) {
			// c0: x strength, y 1 = the frame is linear now, z the gamma, w 1 / the gamma
			static const char* kPS =
				"sampler2D S : register(s0); sampler2D M : register(s1); float4 E : register(c0);"
				"float4 main(float2 uv : TEXCOORD0) : COLOR0 {"
				"  float3 c = max(tex2D(S, uv).rgb, 0.0); float a = tex2D(M, uv).a; float lost = a > 0.5 ? max(a - 1.0, 0.0) : 0.0;"
				"  float3 lin = E.y > 0.5 ? c : pow(c, E.z); float l = dot(lin, float3(0.2126, 0.7152, 0.0722));"
				"  float f = saturate(1.0 - E.x * lost / max(l + lost, 1.0e-5));"
				"  return float4((E.y > 0.5 ? f : pow(f, E.w)).xxx, 1.0); }";
			ID3DXBuffer* code = nullptr; ID3DXBuffer* errors = nullptr;
			if (FAILED(D3DXCompileShader(kPS, (UINT)strlen(kPS), NULL, NULL, "main", "ps_3_0", 0, &code, &errors, NULL)) || !code ||
				FAILED(d->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &ContrastPS))) {
				Logger::Log("[ERROR] ShadowContrast: its shader failed (%s)", errors ? (const char*)errors->GetBufferPointer() : "?");
				ContrastFailed = true;
			}
			if (errors) errors->Release();
			if (code) code->Release();
			if (ContrastFailed) return;
		}
		static GpuTimer contrastTimer("ShadowContrast step");	// UNOFFICIAL: the copy + the full-screen step, in the F10 GPU profile
		GpuProfileScope gpu(contrastTimer, d);
		if (FAILED(d->StretchRect(target, NULL, ContrastCopy, NULL, D3DTEXF_NONE))) { ContrastFailed = true; Logger::Log("[ERROR] ShadowContrast: frame copy failed"); return; }

		D3DVIEWPORT9 vp = {}; d->GetViewport(&vp);
		IDirect3DSurface9* rt0 = nullptr; d->GetRenderTarget(0, &rt0);
		IDirect3DSurface9* rt[4] = {};
		for (DWORD i = 1; i < 4; i++) if (SUCCEEDED(d->GetRenderTarget(i, &rt[i])) && rt[i]) d->SetRenderTarget(i, nullptr);
		if (rt0 != target) d->SetRenderTarget(0, target);
		d->SetViewport(&vp);
		IDirect3DVertexShader9* vs = nullptr; IDirect3DPixelShader9* ps = nullptr; IDirect3DVertexDeclaration9* decl = nullptr;
		IDirect3DVertexBuffer9* vb = nullptr; UINT vbOffset = 0, vbStride = 0; DWORD fvf = 0; float c0[4] = {};
		IDirect3DBaseTexture9* tex[2] = {};
		d->GetVertexShader(&vs); d->GetPixelShader(&ps); d->GetVertexDeclaration(&decl); d->GetFVF(&fvf);
		d->GetStreamSource(0, &vb, &vbOffset, &vbStride); d->GetTexture(0, &tex[0]); d->GetTexture(1, &tex[1]); d->GetPixelShaderConstantF(0, c0, 1);
		static const D3DRENDERSTATETYPE kRS[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE, D3DRS_CULLMODE, D3DRS_STENCILENABLE,
			D3DRS_SCISSORTESTENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_COLORWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND,
			D3DRS_BLENDOP, D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_FOGENABLE };
		const DWORD want[] = { D3DZB_FALSE, FALSE, FALSE, D3DCULL_NONE, FALSE, FALSE, FALSE, 0x7, TRUE, D3DBLEND_ZERO, D3DBLEND_SRCCOLOR,
			D3DBLENDOP_ADD, FALSE, FALSE };
		DWORD saved[sizeof(kRS) / sizeof(kRS[0])];
		for (unsigned i = 0; i < sizeof(kRS) / sizeof(kRS[0]); i++) { if (FAILED(d->GetRenderState(kRS[i], &saved[i]))) saved[i] = want[i]; if (saved[i] != want[i]) d->SetRenderState(kRS[i], want[i]); }
		static const D3DSAMPLERSTATETYPE kSS[] = { D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE };
		const DWORD wantS[] = { D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, FALSE };
		DWORD savedS[2][sizeof(kSS) / sizeof(kSS[0])];
		for (DWORD s = 0; s < 2; s++)
			for (unsigned i = 0; i < sizeof(kSS) / sizeof(kSS[0]); i++) { if (FAILED(d->GetSamplerState(s, kSS[i], &savedS[s][i]))) savedS[s][i] = wantS[i]; if (savedS[s][i] != wantS[i]) d->SetSamplerState(s, kSS[i], wantS[i]); }

		const bool linear = LinearScene::Linear;
		const float gamma = LinearScene::Gamma > 1.0f ? LinearScene::Gamma : 2.2f;
		const float e[4] = { ContrastStrength, linear ? 1.0f : 0.0f, gamma, 1.0f / gamma };
		d->SetVertexShader(nullptr);
		d->SetPixelShader(ContrastPS);
		d->SetPixelShaderConstantF(0, e, 1);
		d->SetTexture(0, ContrastCopyTex);
		d->SetTexture(1, MaterialTexture);
		d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
		const float x0 = (float)vp.X - 0.5f, y0 = (float)vp.Y - 0.5f, x1 = (float)(vp.X + vp.Width) - 0.5f, y1 = (float)(vp.Y + vp.Height) - 0.5f;
		const float u0 = (float)vp.X / t.Width, v0 = (float)vp.Y / t.Height, u1 = (float)(vp.X + vp.Width) / t.Width, v1 = (float)(vp.Y + vp.Height) / t.Height;
		const float quad[4][6] = { { x0, y0, 0, 1, u0, v0 }, { x1, y0, 0, 1, u1, v0 }, { x0, y1, 0, 1, u0, v1 }, { x1, y1, 0, 1, u1, v1 } };
		d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
		ContrastApplied++;

		for (DWORD s = 0; s < 2; s++)
			for (unsigned i = 0; i < sizeof(kSS) / sizeof(kSS[0]); i++) if (savedS[s][i] != wantS[i]) d->SetSamplerState(s, kSS[i], savedS[s][i]);
		for (unsigned i = 0; i < sizeof(kRS) / sizeof(kRS[0]); i++) if (saved[i] != want[i]) d->SetRenderState(kRS[i], saved[i]);
		if (rt0 != target) d->SetRenderTarget(0, rt0);
		for (DWORD i = 1; i < 4; i++) if (rt[i]) { d->SetRenderTarget(i, rt[i]); rt[i]->Release(); }
		if (rt0) rt0->Release();
		d->SetViewport(&vp);
		d->SetTexture(0, tex[0]); if (tex[0]) tex[0]->Release();
		d->SetTexture(1, tex[1]); if (tex[1]) tex[1]->Release();
		d->SetPixelShaderConstantF(0, c0, 1);
		if (decl) { d->SetVertexDeclaration(decl); decl->Release(); } else d->SetFVF(fvf);
		d->SetStreamSource(0, vb, vbOffset, vbStride); if (vb) vb->Release();
		d->SetVertexShader(vs); if (vs) vs->Release();
		d->SetPixelShader(ps); if (ps) ps->Release();
	}

	inline HRESULT STDMETHODCALLTYPE DIPHook(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, INT b, UINT mi, UINT nv, UINT s, UINT pc) {
		DisplaceHeight::BeforeDraw(d);	// UNOFFICIAL 2026-11-01 SameHeight (displaced parallax objects)
		BeforeDraw(d);
		const bool census = GBufferCensus::Active;	// UNOFFICIAL 2026-11-14 GBufferCensus (test only): the draw as it is made
		if (census) GBufferCensus::Before(d, GBufferCensus::KindDIP, (UInt32)t, (UInt32)b, mi, nv, s, pc);
		// (UNOFFICIAL 2026-11-14 MergeCopies: when this is the innermost of NVR's hooks, after every per-draw step above)
		const HRESULT r = MergeCopies::DrawAction && MergeCopies::Inner((void*)NextDIP) ? MergeCopies::AtDraw(d, NextDIP, t, b, mi, nv, s, pc) : NextDIP(d, t, b, mi, nv, s, pc);
		if (census) GBufferCensus::After(d);
		AfterDraw(d);	// (2026-11-11 BlendedAlpha)
		if (GrassPass && GrassOwnOk && SUCCEEDED(r)) GrassOwnDraw(d, [&]() { return NextDIP(d, t, b, mi, nv, s, pc); });	// UNOFFICIAL 2026-11-12 PTGrassOwnPixel
		return r;
	}
	inline HRESULT STDMETHODCALLTYPE DPHook(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT s, UINT pc) {
		BeforeDraw(d);
		const bool census = GBufferCensus::Active;
		if (census) GBufferCensus::Before(d, GBufferCensus::KindDP, (UInt32)t, s, pc);
		const HRESULT r = NextDP(d, t, s, pc);
		if (census) GBufferCensus::After(d);
		AfterDraw(d);
		return r;
	}
	inline HRESULT STDMETHODCALLTYPE DPUPHook(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT pc, CONST void* v, UINT stride) {
		BeforeDraw(d);
		const bool census = GBufferCensus::Active;
		if (census) GBufferCensus::Before(d, GBufferCensus::KindDPUP, (UInt32)t, pc, stride);
		const HRESULT r = NextDPUP(d, t, pc, v, stride);
		if (census) GBufferCensus::After(d);
		AfterDraw(d);
		return r;
	}
	inline HRESULT STDMETHODCALLTYPE DIPUPHook(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT mi, UINT nv, UINT pc, CONST void* i, D3DFORMAT f, CONST void* v, UINT stride) {
		BeforeDraw(d);
		const bool census = GBufferCensus::Active;
		if (census) GBufferCensus::Before(d, GBufferCensus::KindDIPUP, (UInt32)t, mi, nv, pc, (UInt32)f, stride);
		const HRESULT r = NextDIPUP(d, t, mi, nv, pc, i, f, v, stride);
		if (census) GBufferCensus::After(d);
		AfterDraw(d);
		return r;
	}
	inline HRESULT STDMETHODCALLTYPE ClearHook(IDirect3DDevice9* d, DWORD count, CONST D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) {
		if (InSpan) {	// D3D9's Clear clears every bound target; the next writing draw binds it again
			IDirect3DSurface9* rt3 = nullptr;
			if (SUCCEEDED(d->GetRenderTarget(3, &rt3)) && rt3) rt3->Release();
			if ((rt3 && rt3 == DrawSurface()) || MaterialBound || AlbedoBound) Unbind(d);	// (AlbedoBound: 2026-10-31 AlbedoBuffer)
		}
		else if (GrassLateBound) { const bool armed = GrassArmed; GrassLateEnd(d); GrassArmed = armed; }	// UNOFFICIAL 2026-11-12 (the late grass binds)
		return NextClear(d, count, rects, flags, color, z, stencil);
	}

	inline void Hook(IDirect3DDevice9* d) {
		if (Hooked || !d) return;
		void** table = *(void***)d;
		DWORD old = 0;
		if (!VirtualProtect(&table[43], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return;
		NextClear = (ClearFn)table[43];  table[43] = (void*)&ClearHook;
		VirtualProtect(&table[43], sizeof(void*), old, &old);
		if (!VirtualProtect(&table[81], 4 * sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return;
		NextDP = (DPFn)table[81];  table[81] = (void*)&DPHook;
		NextDIP = (DIPFn)table[82];  table[82] = (void*)&DIPHook;
		MergeCopies::OurHooks[0] = (void*)&DIPHook;	// UNOFFICIAL 2026-11-14 MergeCopies (which hook is the innermost)
		NextDPUP = (DPUPFn)table[83];  table[83] = (void*)&DPUPHook;
		NextDIPUP = (DIPUPFn)table[84];  table[84] = (void*)&DIPUPHook;
		VirtualProtect(&table[81], 4 * sizeof(void*), old, &old);
		Hooked = true;	// stays for the session (other hooks chain through it); outside the list it only passes the call on
	}

	// UNOFFICIAL 2026-10-28 (native D3D9): the native runtime writes its own entries back into the device's table (VolumetricClouds saw
	// it for StretchRect right after a draw; Render.cpp SandstormGlass), so a hook put in once is gone for the rest of the session: the
	// first native run saw 0.5 draws a frame here (LinearScene 2.4) against ~370 under NVR DXVK. Called inside the list at every pass
	// (GrassFirst::RenderPassImmediatelyHook, an engine detour the runtime does not touch) and at BeginSpan: a slot holding exactly
	// what this hook passes calls on to (the runtime's entry, or the hook below ours) gets ours back; a slot holding anything else (another
	// program's hook on top) is left alone. LinearScene::EnsureHooked runs first, as it was put in first, so the chain comes back the same.
	inline UInt32 Reinstalls = 0;
	inline void EnsureHooked(IDirect3DDevice9* d) {
		if (!Hooked || !d) return;
		void** table = *(void***)d;
		struct Slot { int i; void* ours; void* next; };
		const Slot slots[5] = { { 43, (void*)&ClearHook, (void*)NextClear }, { 81, (void*)&DPHook, (void*)NextDP }, { 82, (void*)&DIPHook, (void*)NextDIP },
			{ 83, (void*)&DPUPHook, (void*)NextDPUP }, { 84, (void*)&DIPUPHook, (void*)NextDIPUP } };
		const Slot tracking[3] = { { 37, (void*)&SetRTHook, (void*)NextSetRT }, { 57, (void*)&SetRSHook, (void*)NextSetRS }, { 107, (void*)&SetPSHook, (void*)NextSetPS } };
		for (int k = 0; LeanHooked && k < 3; k++) {	// UNOFFICIAL 2026-10-29 lean hook (DXVK only, see Lean; kept in step anyway)
			const Slot& s = tracking[k];
			if (table[s.i] == s.ours || table[s.i] != s.next) continue;
			DWORD old = 0;
			if (!VirtualProtect(&table[s.i], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) continue;
			table[s.i] = s.ours;
			VirtualProtect(&table[s.i], sizeof(void*), old, &old);
		}
		for (const Slot& s : slots) {
			if (table[s.i] == s.ours || table[s.i] != s.next) continue;
			DWORD old = 0;
			if (!VirtualProtect(&table[s.i], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) continue;
			table[s.i] = s.ours;
			VirtualProtect(&table[s.i], sizeof(void*), old, &old);
			if (++Reinstalls <= 3) Logger::Log("CapturedNormals: device table slot %d had been put back to the runtime's entry: hook put in again (logged 3 times)", s.i);
		}
	}

	// Render.cpp, once a frame. wanted: a reader will use this frame's normals (SnowAccumulation with CapturedNormals on).
	inline void BeginFrame(IDirect3DDevice9* d, bool wanted) {
		Data.x = 0.0f;	// nothing captured yet this frame
		{	// UNOFFICIAL 2026-11-12 foliage records + CapturedCoverage: the keys (live), and whether the path tracer replaces the picture
			if (GrassLateBound) GrassLateEnd(d);
			GrassArmed = GrassLateOk = false; GrassLateTarget = nullptr;
			GrassOwnOk = false; GrassOwnTarget = nullptr; GrassPass = false;	// (PTGrassOwnPixel)
			{ SettingManager::Configuration::ConfigNode gk; GrassKey = TheSettingManager->Config.FillNode(&gk, "Main.Main.Quality", "CapturedGrass") && gk.BoolValue; }	/* UNOFFICIAL 2026-11-12: a missing key (an older defaults file): off. The defaults file's value was false for a day (an RTX run of the first records: bright speckles that changed every frame; cause and fix in GrassOwnBeam's comment above, the colour in GRASS23x000TMS.pso), true since the same evening (ptgrass: without the records what a blade's pixel becomes depends on the view angle, see RtViewModel.h "grass") */ LeafKey = QualityKeyOn("CapturedLeaves"); CoverageOn = QualityKeyOn("CapturedCoverage");
			const bool traced = AlbedoCompiledIn() && RtPathTrace::Replacing();
			// UNOFFICIAL 2026-11-12 CapturedGrass, second pass: also for the raster frame's RT GI (the SunGI switch, which sets SunGIWantsAlbedo,
			// or [Shaders.AmbientOcclusion.RT] Enabled + GI): its bounce takes the grass's own colour, and its rays (and every other
			// reader of TESR_NormalsBuffer) the tuft's normal instead of the depth-rebuilt one. Not while the grass lights itself by the
			// deferred flashlight's beam (the light pass would light its records too). The keys are read only with CapturedGrass on.
			bool forGI = false, beam = false;
			if (GrassKey && !traced && AlbedoCompiledIn() && !GrassWriters.empty()) {
				// (2026-11-15 RT master switches: the RT AO + GI keys count as off while RtCore::Gated; SunGI's switch keeps its own fallback rule)
				forGI = SunGIWantsAlbedo || (!RtCore::Gated() && TheSettingManager->GetSettingI("Shaders.AmbientOcclusion.RT", "Enabled") != 0 && TheSettingManager->GetSettingI("Shaders.AmbientOcclusion.RT", "GI") != 0);
				if (forGI && GrassOwnBeam) { forGI = false; beam = true; GrassBeamFrames++; }
			}
			// UNOFFICIAL 2026-11-13 PTAlbedoCoverage ([Main.Main.RayTracing], a missing key: on; RtPathTrace.comp gAlbedoCover has the capture's
			// numbers): with the game's MSAA the grass is drawn with transparency multisampling, a card covering 1-3 of a pixel's 4 samples far
			// round the tuft. With grass records every such pixel's record is the MEAN of the grass's and the land's (class 5-6 from 7 and 4,
			// a mixed distance and normal, alpha 1): a third of the picture seen from above came out as dark half-grass surfaces. No grass
			// records for the path tracer then: the land leaves the covered samples unwritten, the trace takes every partly written pixel
			// as the game's own (its anti-aliased blend), and the grass drawn after the land goes into the mask (PTGrassOwnPixel).
			bool msaaOwn = false;
			if (traced && AlbedoMsaa) { SettingManager::Configuration::ConfigNode ck; msaaOwn = !TheSettingManager->Config.FillNode(&ck, "Main.Main.RayTracing", "PTAlbedoCoverage") || ck.BoolValue; }
			GrassNow = ((traced && !msaaOwn) || forGI) && GrassKey && !GrassWriters.empty();
			GrassLateWanted = GrassNow;	// (2026-11-14 DeferredFoliage: the late grass's records for these readers only)
			if (FoliageDeferred && !GrassWriters.empty()) GrassNow = true;	// UNOFFICIAL 2026-11-14 DeferredFoliage (never while traced: DeferredFoliage::BeginFrame)
			GrassOwnOn = traced && WaterMask::GrassOn && !WaterMask::Failed;	// UNOFFICIAL 2026-11-12 PTGrassOwnPixel (the key: RtPathTrace.cpp's settings read)
			GrassForGI = GrassNow && !traced;
			GrassMergeNow = GrassNow && QualityKeyOn("CapturedGrassNormals");
			LeafNow = (traced && LeafKey && !LeafWriters.empty()) || (FoliageDeferred && !LeafWriters.empty());	// (2026-11-14 DeferredFoliage)
			{	// one line when the grass records' state changes (at most 24 a session: the flashlight can switch it often)
				const int state = !GrassKey ? 0 : GrassWriters.empty() || !AlbedoCompiledIn() ? 1 : msaaOwn ? 6 : traced ? 2 : forGI ? 3 : beam ? 4 : 5;
				if (state != GrassState && (state != 0 || GrassState > 0) && GrassStateLogs < 24) {
					GrassStateLogs++;
					static const char* const text[] = { "off (the key)",
						"on, but cannot run: no grass shader was compiled with the records (needs [Main.Main.Quality] CapturedNormals, MaterialBuffer and AlbedoBuffer at startup, and Vegas Shaders' grass shaders)",
						"WRITTEN for the path tracer", "WRITTEN for the RT GI (no path tracing: the bounce's colour and the rays' normal on grass)",
						"on, but not written now: the deferred flashlight lights the grass itself (its light pass would light the records too)",
						"on, but not written now: nothing reads them (no path tracing, no RT GI / SunGI)",
						"on, but NOT written under path tracing with the game's MSAA (PTAlbedoCoverage): a grass card covers a share of a pixel's samples far round the tuft, and a record averaged with the land's is neither. The grass is the game's own anti-aliased pixel there" };
					Logger::Log("CapturedGrass: grass records %s%s", text[state], state == 2 || state == 3 ? (GrassMergeNow ? "; the grass's own normal goes into the normals buffer (CapturedGrassNormals on)" : "; CapturedGrassNormals is OFF: grass pixels keep the normal rebuilt from depth (noisy)") : "");
				}
				GrassState = state;
			}
		}
		// UNOFFICIAL 2026-11-02 RetryFailedEffects (2026-11-16 fd-batchB: the self-check's latch too, with a check soon after the retry). Each
		// latch decides for itself (no short cut: a released one is cleared even while the other still holds).
		if (Failed && (MakeFail.failed || CheckFail.failed)) {
			const bool fromCheck = CheckFail.failed;
			const bool makeHeld = MakeFail.failed && MakeFail.Blocked(), checkHeld = CheckFail.failed && CheckFail.Blocked();
			if (!makeHeld && !checkHeld) {
				Failed = false; Recreations = 0; RecreationsSince = LiveFrames;
				if (fromCheck) { ClearStrikes = ListStrikes = 0; RecheckAt = LiveFrames + 2 * kRecheckFrames; }
			}
		}
		Live = CompiledIn() && !Failed && wanted;
		if (Live && !Hooked) {
			Hook(d);
			Logger::Log("CapturedNormals: on (%u template pixel shaders write the normal target)", (unsigned)Writers.size());
		}
		if (Live && ++Frames >= 600) {
			Logger::Log("CapturedNormals: per frame %.1f draws wrote normals, %.1f masked, %.2f unbinds (other targets, clears); %u hook reinstalls so far%s",
				Writes / (double)Frames, Masked / (double)Frames, Unbinds / (double)Frames, Reinstalls, MaterialCompiled ? "" : " (MaterialBuffer off)");
			if (MaterialCompiled)	// UNOFFICIAL 2026-10-29 MaterialBuffer
				Logger::Log("MaterialBuffer: per frame %.1f draws wrote it, %.1f writer draws found stage 2 taken (the skin blur's); %u material writer shaders",
					MaterialWrites / (double)Frames, MaterialBusy / (double)Frames, (unsigned)MaterialWriters.size());
			if (AlbedoCompiled)	// UNOFFICIAL 2026-10-31 AlbedoBuffer
				Logger::Log("AlbedoBuffer: per frame %.1f draws wrote it (%.1f of them ONLY_LIGHT base passes: the albedo from s0), %.1f writer draws found stage 1 taken (the skin blur's), skin fills %.2f; %u writer shaders (%u light-only)",
					AlbedoWrites / (double)Frames, AlbedoLightOnlyDraws / (double)Frames, AlbedoBusy / (double)Frames, AlbedoSkinFills / (double)Frames, (unsigned)AlbedoWriters.size(), (unsigned)AlbedoLightOnlyWriters.size());
			if (MaterialCompiled && !ContrastWriters.empty())	// UNOFFICIAL ShadowContrast (the game's passes)
				Logger::Log("ShadowContrast: per frame %.1f lamp passes gave their shadows' light to MaterialBuffer (%.1f with another added blend, left out), applied %.2f times; strength %.2f; %u lamp-pass shaders",
					ContrastWrites / (double)Frames, ContrastOtherBlend / (double)Frames, ContrastApplied / (double)Frames, ContrastStrength, (unsigned)ContrastWriters.size());
			if (BodyTracked)	// UNOFFICIAL 2026-11-04 DrawStateTracking
				Logger::Log("DrawStateTracking: %s; %u checks against the device so far, %u differed; %u records read again after a state block or reset",
					BodyDisabled ? "OFF after a difference (the per-draw device queries)" : BodyWanted ? "in use" : "switched off", BodyChecks, BodyMismatches, BodyResyncs);
			// UNOFFICIAL 2026-11-12 foliage records + CapturedCoverage
			if (AlbedoCompiled)
				Logger::Log("CapturedNormals: foliage records %s (CapturedGrass %s, %u grass shaders; CapturedLeaves %s, %u leaf shaders; written while path tracing replaces the picture, the grass's also for the RT GI: %s): per frame %.1f grass draws wrote them before / in the main list (the span opened early on %.2f of the frames), %.1f after it (%.2f found a target stage taken), %.1f leaf draws in the main list. CapturedCoverage %s: %.1f blended writer draws a frame recorded only where their own alpha is 0.5 or more",
					GrassNow || LeafNow ? "ACTIVE" : "idle", GrassKey ? "on" : "off", (unsigned)GrassWriters.size(), LeafKey ? "on" : "off", (unsigned)LeafWriters.size(),
					GrassForGI ? "now" : "not now",
					GrassWrites / (double)Frames, GrassEarlySpans / (double)Frames, GrassLateWrites / (double)Frames, GrassLateBusy / (double)Frames, LeafWrites / (double)Frames,
					CoverageOn ? "on" : "off", CoverageDraws / (double)Frames);
			// UNOFFICIAL 2026-11-12 HairAndEyes (only when it did something)
			BeamLog();
			GrassWrites = GrassLateWrites = GrassLateBusy = GrassEarlySpans = LeafWrites = CoverageDraws = 0;
			Frames = Writes = Masked = Unbinds = 0;
			MaterialWrites = MaterialBusy = 0;
			AlbedoWrites = AlbedoBusy = AlbedoLightOnlyDraws = AlbedoSkinFills = 0;	// UNOFFICIAL 2026-10-31 AlbedoBuffer
			ContrastWrites = ContrastOtherBlend = ContrastApplied = 0;
		}
	}

	// GrassFirst::MainListHook, around the game's RenderBatches(1, 0x25B) call (world context only).
	// (2026-11-12: also from GrassBind, at the early grass's first shader bind; MainListHook's call then finds the span open)
	inline void BeginSpan(IDirect3DDevice9* d) {
		if (!Live || InSpan) return;
		if (GrassLateBound) GrassLateEnd(d);	// UNOFFICIAL 2026-11-12 (a second world scene in the frame: the first one's late grass is over)
		GrassArmed = GrassLateOk = false; GrassLateTarget = nullptr;
		GrassOwnOk = false; GrassOwnTarget = nullptr;	// (PTGrassOwnPixel)
		IDirect3DSurface9* rt0 = nullptr;
		if (FAILED(d->GetRenderTarget(0, &rt0)) || !rt0) return;
		D3DSURFACE_DESC t = {};
		rt0->GetDesc(&t);
		// Reflections, menus and other views draw the list into smaller targets: the main view only.
		if (t.Width != TheRenderManager->width || t.Height != TheRenderManager->height) { rt0->Release(); return; }
		// Measured by SkinScattering on a GTX 1070 (native D3D9): with multisampled extra targets bound a draw writes nothing at all to
		// target 0. Under DXVK it works; on native D3D9 the game's MSAA keeps the depth-rebuilt normals.
		if (t.MultiSampleType != D3DMULTISAMPLE_NONE && !DxvkCompute::Available()) {
			if (!MsaaWarned) { MsaaWarned = true; Logger::Log("[WARNING] CapturedNormals: the scene is multisampled (the game's MSAA) on native D3D9, where extra multisampled targets draw nothing: off while MSAA is on (fine under Vegas Shaders DXVK)"); }
			rt0->Release();
			return;
		}
		if (!Ensure(d, t)) { rt0->Release(); return; }
		EnsureHooked(d);	// (native D3D9 puts its own table entries back; see EnsureHooked)
		// UNOFFICIAL 2026-10-29 CapturedNormalsLeanHook: the tracked state from the device, once per span
		LeanWanted = TheSettingManager->GetSettingI("Main.Main.Quality", "CapturedNormalsLeanHook") != 0;
		// UNOFFICIAL 2026-11-04 DrawStateTracking: elsewhere (native D3D9, stock DXVK) the detoured bodies keep the record (see BodySetRT)
		BodyWanted = TheSettingManager->GetSettingI("Main.Main.Performance", "DrawStateTracking") != 0;
		{ const int every = TheSettingManager->GetSettingI("Main.Main.Performance", "DrawStateTrackingCheck"); BodyCheckEvery = every > 0 ? (UInt32)every : 1024; }
		if (LeanWanted && DxvkCompute::Available()) { HookTracking(d); Resync(d); }
		else if (BodyWanted && !DxvkCompute::Available()) { InstallBodyTracking(d); if (BodyLean()) Resync(d); else Tracked = false; }
		else Tracked = false;
		SpanTarget = rt0;	// (holds the reference until EndSpan)
		d->ColorFill(DrawSurface(), NULL, D3DCOLOR_ARGB(0, 0, 0, 0));	// "nothing here": readers keep the depth-rebuilt normal
		if (MaterialTexture && MaterialCompiled) d->ColorFill(MaterialDrawSurface(), NULL, D3DCOLOR_ARGB(0, 0, 0, 0));	// UNOFFICIAL MaterialBuffer: class 0
		MaterialDrawn = MaterialBound = false;
		if (AlbedoTexture && AlbedoCompiled) d->ColorFill(AlbedoDrawSurface(), NULL, D3DCOLOR_ARGB(0, 0, 0, 0));	// UNOFFICIAL 2026-10-31 AlbedoBuffer: unwritten
		AlbedoDrawn = AlbedoBound = false;
		SkinAlbedoTex = nullptr; SkinAlbedoMsaa = nullptr; SkinAlbedoSurface = nullptr;	// (noted again by the skin draws of this span)
		{	// (2026-11-11) [Main.Main.Quality] CapturedBlendedAlpha, live (a missing key: on); a log line of its work now and then
			SettingManager::Configuration::ConfigNode node;
			BlendedAlphaOn = !TheSettingManager->Config.FillNode(&node, "Main.Main.Quality", "CapturedBlendedAlpha") || node.BoolValue;
			if (LiveFrames % 1800 == 2) { Logger::Log("CapturedNormals: CapturedBlendedAlpha %s: %u blended writer draws since the last line had the targets' alpha written (the scene's alpha blend ZERO / ONE kept it before: such pixels counted as unwritten)", BlendedAlphaOn ? "on" : "off", BlendedAlphaDraws); BlendedAlphaDraws = 0; }
		}
		CheckThisFrame = IsCheckFrame(++LiveFrames);
		{	// UNOFFICIAL 2026-10-31 [Main.Main.Quality] MaterialBufferCheckEvery (live, 0 off): the self-check every that many frames too (a test aid)
			const int every = TheSettingManager->GetSettingI("Main.Main.Quality", "MaterialBufferCheckEvery");
			if (every > 0 && LiveFrames % (UInt32)every == 0) CheckThisFrame = true;
		}
		if (CheckThisFrame) {	// the self-check's first half: the clear must have emptied it
			if (MsaaSurface) d->StretchRect(MsaaSurface, NULL, TextureSurface, NULL, D3DTEXF_NONE);
			const int n = CountWritten(d);
			if (n < 0) { CheckThisFrame = false; if (!CheckReported) { CheckReported = true; Logger::Log("CapturedNormals: self-check could not read the target back (skipped)"); } }
			else CheckResult(n == 0, "after the clear", n, false);
			if (Failed) { SpanTarget = nullptr; rt0->Release(); return; }
		}
		InSpan = true;
		Bound = false;
		Drawn = false;
		LastPS = nullptr;
		SharedDrawState::Valid = Lean();	// UNOFFICIAL SharedDrawState: what LinearScene may read until EndSpan
	}

	inline void EndSpan(IDirect3DDevice9* d) {
		if (!InSpan) return;
		IDirect3DSurface9* rt3 = nullptr;
		if (SUCCEEDED(d->GetRenderTarget(3, &rt3)) && rt3) rt3->Release();
		if ((rt3 && rt3 == DrawSurface()) || MaterialBound || AlbedoBound) Unbind(d);	// (AlbedoBound: 2026-10-31 AlbedoBuffer)
		InSpan = false;
		Tracked = false;	// UNOFFICIAL lean hook: resynced at the next span
		SharedDrawState::Valid = false;
		if (MsaaSurface) d->StretchRect(MsaaSurface, NULL, TextureSurface, NULL, D3DTEXF_NONE);	// the game's MSAA: resolved for the readers
		if (MaterialMsaa && MaterialSurface) d->StretchRect(MaterialMsaa, NULL, MaterialSurface, NULL, D3DTEXF_NONE);	// UNOFFICIAL MaterialBuffer
		if (AlbedoMsaa && AlbedoSurface) d->StretchRect(AlbedoMsaa, NULL, AlbedoSurface, NULL, D3DTEXF_NONE);	// UNOFFICIAL 2026-10-31 AlbedoBuffer
		if (ContrastDrawn && ContrastLive()) ApplyContrast(d, SpanTarget);	// UNOFFICIAL ShadowContrast (the game's lamp passes; 2026-11-16 ContrastLive: item 23)
		ContrastDrawn = false;
		if (MaterialDrawn && !Failed && Drawn) SkinFill(d);	// UNOFFICIAL 2026-10-31 MaterialBufferSkin (after the contrast step: it reads the alpha)
		if (AlbedoDrawn && !Failed && Drawn) SkinAlbedoFill(d);	// UNOFFICIAL 2026-10-31 AlbedoBuffer: the skin's albedo from the skin blur's target
		// UNOFFICIAL 2026-11-12 grass records: the grass drawn after this list writes into the same targets (GrassLateDraw), this scene target only
		GrassLateTarget = SpanTarget;
		GrassLateOk = GrassLateWanted && !Failed && SpanTarget && AlbedoDrawn && MaterialDrawn && Drawn;	// (2026-11-14: GrassNow before DeferredFoliage; the same value without it)
		GrassOwnTarget = SpanTarget;	// (PTGrassOwnPixel: the grass drawn after this list without records, this scene target only)
		GrassOwnOk = GrassOwnOn && !Failed && SpanTarget && AlbedoDrawn && Drawn;
		if (SpanTarget) { SpanTarget->Release(); SpanTarget = nullptr; }
		MaterialData.x = MaterialDrawn && !Failed ? 1.0f : 0.0f;
		AlbedoData.x = AlbedoDrawn && !Failed ? 1.0f : 0.0f;	// UNOFFICIAL 2026-10-31 AlbedoBuffer
		if (CheckThisFrame && Drawn) {	// the self-check's second half: templates drew, so something must be written
			const int n = CountWritten(d);
			if (n >= 0) CheckResult(n > 0, "after the main list", n, true);
			// UNOFFICIAL 2026-10-29 MaterialBuffer: what it holds on these frames too (64 x 64 point copy): written texels by class, and the
			// thickness / roughness ranges seen (a log line only; nothing is switched off by it)
			D3DLOCKED_RECT r = {};
			if (MaterialDrawn && MaterialSurface && CheckTarget && SUCCEEDED(d->StretchRect(MaterialSurface, NULL, CheckTarget, NULL, D3DTEXF_POINT)) &&
				SUCCEEDED(d->GetRenderTargetData(CheckTarget, CheckCopy)) && SUCCEEDED(CheckCopy->LockRect(&r, NULL, D3DLOCK_READONLY))) {
				int byClass[8] = {}, metalTexels = 0; float thickMax = 0.0f, roughMin = 2.0f, roughMax = -2.0f, metalMax = 0.0f;	// (7: 2026-11-12 CapturedGrass, grass)
				int v2Specular = 0, v2Normals = 0, v2NoNormal = 0, v2ByModel[8] = {};	// UNOFFICIAL 2026-11-14 GBufferV2 (8 hair, 9 eye, 10 unlit, the rest)
				for (int y = 0; y < 64; y++) {
					const D3DXFLOAT16* row = (const D3DXFLOAT16*)((const char*)r.pBits + y * r.Pitch);
					for (int x = 0; x < 64; x++) {
						float v[4]; D3DXFloat16To32Array(v, row + x * 4, 4);
						if (!MaterialWritten(v)) continue;	// (2026-11-14 GBufferV2: either layout)
						{ const int cls = MaterialClass(v); if (cls <= 7) byClass[(std::max)(cls, 0)]++; }	// 2026-10-31: r = class + 0.49 x metal
						const float metal = (std::max)(v[0] - floorf(v[0] + 0.004f), 0.0f) / 0.49f;
						if (metal > 0.02f) { metalTexels++; metalMax = (std::max)(metalMax, metal); }
						if (V2Compiled) { if (v[0] >= 15.99f) v2Specular++; if (fabsf(v[1]) <= 1.001f) v2Normals++; else v2NoNormal++; if (MaterialClass(v) > 7) v2ByModel[(std::min)(MaterialClass(v), 15) - 8]++; }
						else thickMax = (std::max)(thickMax, v[1]);
						if (v[2] >= 0.0f) { roughMin = (std::min)(roughMin, v[2]); roughMax = (std::max)(roughMax, v[2]); }
					}
				}
				CheckCopy->UnlockRect();
				Logger::Log("MaterialBuffer: self-check at captured frame %u: texels by class 0:%d object:%d gear:%d cloth:%d land:%d plant:%d skin:%d grass:%d of 4096; largest thickness %.0f; roughness %.2f-%.2f; metal on %d texels (up to %.2f); skin fills so far %u",
					LiveFrames, byClass[0], byClass[1], byClass[2], byClass[3], byClass[4], byClass[5], byClass[6], byClass[7], thickMax, roughMin, roughMax, metalTexels, metalMax, SkinFills);
				// UNOFFICIAL 2026-11-14 GBufferV2: the layout's own channels (the thickness above is not in it: 0)
				if (V2Compiled)
					Logger::Log("GBufferV2: self-check at captured frame %u: of the written texels %d with a shading normal, %d without (g 4+: skin fill, foliage, blended draws, light passes), %d with the specular bit; hair:%d eye:%d unlit:%d other:%d",
						LiveFrames, v2Normals, v2NoNormal, v2Specular, v2ByModel[0], v2ByModel[1], v2ByModel[2], v2ByModel[3] + v2ByModel[4] + v2ByModel[5] + v2ByModel[6] + v2ByModel[7]);
			}
			if (AlbedoDrawn && AlbedoSurface && CheckTarget) AlbedoCheck(d);	// UNOFFICIAL 2026-10-31 AlbedoBuffer
		}
		if (CheckThisFrame && !Drawn && ListStrikes) RecheckAt = LiveFrames + kRecheckFrames;	// (2026-11-16 a recheck with no template draw: again later)
		CheckThisFrame = false;
		Data.x = Drawn && !Failed ? 1.0f : 0.0f;
	}

	// UNOFFICIAL 2026-11-07 (ptaaa-firstperson) the hands' bare skin colour (VmSkin: the skin template's COLOR2, the stored curve) into the
	// AlbedoBuffer where the captured normals mark skin (a >= 1.5) and VmSkin was written this span (cleared to 0: any colour or luma there;
	// a pure-black texel in the dark is the one miss and keeps the world's colour), decoded as SkinAlbedoFill decodes the skin blur's
	inline void VmSkinFill(IDirect3DDevice9* d) {
		if (VmSkinFillFailed || !AlbedoSurface || !Texture || !VmSkinTex) return;
		if (!VmSkinFillPS) {
			static const char* kPS =
				"sampler2D N : register(s0); sampler2D S : register(s1); float4 G : register(c0);"
				"float4 main(float2 uv : TEXCOORD0) : COLOR0 {"
				"  float a = tex2D(N, uv).a; clip(a - 1.5);"
				"  float4 s = tex2D(S, uv); clip(dot(s, 1.0) - 0.002);"
				"  float3 c = max(s.rgb, 0.0); c = G.x > 1.0 ? pow(c, G.x) : c;"
				"  return float4(c, 1.0); }";
			VmSkinFillPS = FullScreenShader(d, kPS, "AlbedoBuffer view-model skin fill", VmSkinFillFailed);
			if (!VmSkinFillPS) return;
		}
		float saved[4] = {};
		d->GetPixelShaderConstantF(0, saved, 1);
		const float g[4] = { PassSums::Gamma, 0.0f, 0.0f, 0.0f };
		d->SetPixelShaderConstantF(0, g, 1);
		FullScreenPass(d, AlbedoSurface, VmSkinFillPS, Texture, false, D3DBLEND_ONE, D3DBLEND_ZERO, VmSkinTex);
		d->SetPixelShaderConstantF(0, saved, 1);
	}

	// Render.cpp RenderFirstPersonHook, right round the game's first-person render
	inline void BeginViewModelSpan(IDirect3DDevice9* d) {
		if (GrassLateBound) GrassLateEnd(d);	// UNOFFICIAL 2026-11-12 (the world's late grass is over)
		GrassArmed = GrassLateOk = false;
		GrassOwnOk = false;	// (PTGrassOwnPixel)
		if (!(VmWanted || VmDeferred) || !Live || InSpan || Failed || Data.x <= 0.0f || AlbedoData.x <= 0.0f || !Texture || !AlbedoSurface || MsaaSurface || AlbedoMsaa) return;
		IDirect3DSurface9* rt0 = nullptr;
		if (FAILED(d->GetRenderTarget(0, &rt0)) || !rt0) return;
		D3DSURFACE_DESC t = {};
		rt0->GetDesc(&t);
		if (t.Width != Made.Width || t.Height != Made.Height || t.MultiSampleType != D3DMULTISAMPLE_NONE) {
			if (!VmRefusedLogged) { VmRefusedLogged = true; Logger::Log("UNOFFICIAL RT path trace: the first-person model draws into a %ux%u target (msaa %u), the G-buffer is %ux%u: its colour and normals are not captured, it keeps the game's own pixels (logged once)", t.Width, t.Height, (unsigned)t.MultiSampleType, Made.Width, Made.Height); }
			rt0->Release();
			return;
		}
		if (VmSkinTex) {	// (the screen's size changed)
			D3DSURFACE_DESC s = {};
			if (FAILED(VmSkinTex->GetLevelDesc(0, &s)) || s.Width != t.Width || s.Height != t.Height) { VmSkinSurface->Release(); VmSkinSurface = nullptr; VmSkinTex->Release(); VmSkinTex = nullptr; }
		}
		if (!VmSkinTex && (FAILED(d->CreateTexture(t.Width, t.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &VmSkinTex, NULL)) || !VmSkinTex ||
			FAILED(VmSkinTex->GetSurfaceLevel(0, &VmSkinSurface)))) {
			if (VmSkinTex) { VmSkinTex->Release(); VmSkinTex = nullptr; }
			VmSkinSurface = nullptr;	// (the hands then keep the world's colour under them: VmSkinFill needs it; the gun is still captured)
		}
		if (VmSkinSurface) d->ColorFill(VmSkinSurface, NULL, D3DCOLOR_ARGB(0, 0, 0, 0));
		EnsureHooked(d);
		if (LeanWanted && DxvkCompute::Available()) { HookTracking(d); Resync(d); }
		else if (BodyLean()) Resync(d);
		else Tracked = false;
		SpanTarget = rt0;	// (holds the reference until EndViewModelSpan)
		VmContrastSaved = ContrastStrength;
		ContrastStrength = 0.0f;	// (ShadowContrast was applied after the world's list: the first person's lamp passes add nothing to it)
		InSpan = true;
		VmSpan = true;
		Bound = false;
		LastPS = nullptr;
		VmSkinDrawn = false;
		VmWritesBefore = Writes;
		SharedDrawState::Valid = Lean();
	}

	inline void EndViewModelSpan(IDirect3DDevice9* d, UInt32 frame) {
		if (!VmSpan) return;
		IDirect3DSurface9* rt3 = nullptr;
		if (SUCCEEDED(d->GetRenderTarget(3, &rt3)) && rt3) rt3->Release();
		if ((rt3 && rt3 == DrawSurface()) || MaterialBound || AlbedoBound) Unbind(d);
		IDirect3DSurface9* rt2 = nullptr;
		if (SUCCEEDED(d->GetRenderTarget(2, &rt2)) && rt2) rt2->Release();
		if (rt2 && rt2 == VmSkinSurface) { D3DVIEWPORT9 vp; d->GetViewport(&vp); d->SetRenderTarget(2, nullptr); d->SetViewport(&vp); }
		d->SetRenderState(D3DRS_COLORWRITEENABLE2, 0xF);
		InSpan = false;
		VmSpan = false;
		Tracked = false;
		SharedDrawState::Valid = false;
		ContrastStrength = VmContrastSaved;
		ContrastDrawn = false;
		if (Writes != VmWritesBefore && !Failed) {
			if (MaterialTexture && MaterialCompiled) SkinFill(d);	// the hands' material (class 6) from their captured normals' alpha (the world's again: the same values)
			if (VmSkinDrawn) VmSkinFill(d);
			VmFrame = frame;
			if (VmWanted && VmCaptures++ % 3600 == 0)	// (2026-11-15: the path tracer's line, not DeferredFirstPerson's)
				Logger::Log("UNOFFICIAL RT path trace: the first-person model's colour, normals and material captured for the path tracer (%u draws this frame, %u bare-skin draws so far)", Writes - VmWritesBefore, VmSkinDraws);
		}
		if (SpanTarget) { SpanTarget->Release(); SpanTarget = nullptr; }
	}
	// UNOFFICIAL 2026-11-14 GBufferView ([Main.Main.Quality] GBufferView, live, test only; needs GBufferV2): the G-buffer one channel at a time
	// over the finished picture (after the post chain, EffectChain PostStep_RtSunDebug). 0 off; 1 albedo (linear, shown with a 1 / 2.2 curve);
	// 2 shading normal (world axes as rgb; where the record has none, the smooth normal at a third of its brightness); 3 smooth normal
	// (COLOR3); 4 roughness (grey; blue = none); 5 metal (orange over grey); 6 shading model as colours (1 object grey, 2 gear blue, 3 cloth
	// purple, 4 land brown, 5 plants green, 6 skin pink, 7 grass light green, 8 hair yellow, 9 eye cyan, 10 unlit white); 7 specular bit
	// (white = lamps with a highlight, grey = diffuse only); 8 fog factor (COLOR1.a: white = no fog); 9 camera distance (log2, white at
	// 65,536 units); 10 relief: the shading normal lit from a fixed direction (bumps that read inverted next to a neighbour = a frame sign
	// fault); 11 how far the shading normal bends from the smooth one (black 0, white 90 degrees). Black everywhere: nothing written there.
	inline IDirect3DPixelShader9* ViewPS = nullptr;  inline bool ViewFailed = false;
	inline void ViewEnsure(IDirect3DDevice9* d) {	// at record load (ShaderRecord::LoadShader), never in a frame
		if (ViewPS || ViewFailed || !d || !V2Compiled) return;
		static const char* kPS =
			"sampler2D A : register(s0); sampler2D M : register(s1); sampler2D N : register(s2);"
			"float4 V : register(c0); float4 R : register(c1);"
			"float3 Oct(float2 e) { float3 n = float3(e, 1.0f - abs(e.x) - abs(e.y)); float t = saturate(-n.z);"
			"  n.xy += float2(n.x >= 0.0f ? -t : t, n.y >= 0.0f ? -t : t); return normalize(n); }"
			"float4 main(float2 uv : TEXCOORD0) : COLOR0 {"
			"  float4 g = float4(uv * R.xy, 0.0f, 0.0f);"
			"  float4 a = tex2Dlod(A, g), m = tex2Dlod(M, g), n = tex2Dlod(N, g);"
			"  float written = m.r > 0.5f ? 1.0f : 0.0f;"
			"  float c = floor(m.r + 0.01f); float spec = c >= 16.0f ? 1.0f : 0.0f; c -= 16.0f * spec;"
			"  float hasN = written > 0.5f && abs(m.g) <= 1.001f ? 1.0f : 0.0f;"
			"  float3 sm = n.a > 0.5f ? Oct(n.rg) : 0.0f;"
			"  float3 sn = hasN > 0.5f ? Oct(float2(m.g, m.a)) : sm;"
			"  float metal = saturate(frac(m.r) / 0.49f);"
			"  float rough = m.b >= 0.0f ? m.b : (m.b < -1.99f && m.b > -3.01f ? -2.0f - m.b : -1.0f);"
			"  float v = V.x; float3 o = 0.0f;"
			"  if (v < 1.5f) o = pow(saturate(a.rgb), 1.0f / 2.2f) * (a.a > 0.0f ? 1.0f : 0.0f);"
			"  else if (v < 2.5f) o = written * (sn * 0.5f + 0.5f) * (hasN > 0.5f ? 1.0f : 0.33f);"
			"  else if (v < 3.5f) o = n.a > 0.5f ? sm * 0.5f + 0.5f : 0.0f;"
			"  else if (v < 4.5f) o = written * (rough >= 0.0f ? saturate(rough).xxx : float3(0.1f, 0.2f, 0.9f));"
			"  else if (v < 5.5f) o = written * lerp(float3(0.25f, 0.25f, 0.25f), float3(1.0f, 0.45f, 0.05f), metal);"
			"  else if (v < 6.5f) {"
			"    float3 k = c < 1.5f ? float3(0.6f, 0.6f, 0.6f) : c < 2.5f ? float3(0.2f, 0.4f, 1.0f) : c < 3.5f ? float3(0.6f, 0.2f, 0.8f) :"
			"      c < 4.5f ? float3(0.55f, 0.35f, 0.15f) : c < 5.5f ? float3(0.1f, 0.6f, 0.1f) : c < 6.5f ? float3(1.0f, 0.55f, 0.65f) :"
			"      c < 7.5f ? float3(0.5f, 1.0f, 0.4f) : c < 8.5f ? float3(1.0f, 0.9f, 0.1f) : c < 9.5f ? float3(0.1f, 0.9f, 0.9f) : float3(1.0f, 1.0f, 1.0f);"
			"    o = written * k; }"
			"  else if (v < 7.5f) o = written * (spec > 0.5f ? 1.0f : 0.35f).xxx;"
			"  else if (v < 8.5f) o = saturate(a.a).xxx;"
			"  else if (v < 9.5f) o = n.a > 0.5f ? saturate(log2(1.0f + n.b) / 16.0f).xxx : 0.0f;"
			"  else if (v < 10.5f) o = written * saturate(dot(sn, normalize(float3(0.5f, 0.4f, 0.75f)))).xxx;"
			"  else o = written * hasN * saturate(acos(saturate(dot(sn, sm))) / 1.5708f).xxx;"
			"  return float4(o, 1.0f); }";
		ViewPS = FullScreenShader(d, kPS, "GBufferView", ViewFailed);
		if (ViewPS) Logger::Log("UNOFFICIAL GBufferView: the G-buffer viewer loaded ([Main.Main.Quality] GBufferView 1-11, test only)");
	}
	// After the post chain (EffectChain PostStep_RtSunDebug): over the whole target while [Main.Main.Quality] GBufferView is 1-11.
	inline void ViewRender(IDirect3DDevice9* d, IDirect3DSurface9* target) {
		if (!ViewPS || !target || !Texture || Data.x <= 0.0f) return;
		const int view = TheSettingManager->GetSettingI("Main.Main.Quality", "GBufferView");
		if (view <= 0) return;
		auto found = TheShaderManager->ConstantsTable.find("TESR_RenderScale");
		const D3DXVECTOR4* scale = found == TheShaderManager->ConstantsTable.end() ? nullptr : found->second;
		const float c[2][4] = { { (float)view, 0.0f, 0.0f, 0.0f }, { scale ? scale->x : 1.0f, scale ? scale->y : 1.0f, 0.0f, 0.0f } };
		float saved[2][4] = {};
		d->GetPixelShaderConstantF(0, &saved[0][0], 2);
		d->SetPixelShaderConstantF(0, &c[0][0], 2);
		IDirect3DBaseTexture9* albedo = AlbedoTexture && AlbedoData.x > 0.5f ? (IDirect3DBaseTexture9*)AlbedoTexture : (IDirect3DBaseTexture9*)Texture;
		IDirect3DBaseTexture9* material = MaterialTexture && MaterialData.x > 0.5f ? (IDirect3DBaseTexture9*)MaterialTexture : (IDirect3DBaseTexture9*)Texture;
		FullScreenPass(d, target, ViewPS, albedo, false, D3DBLEND_ONE, D3DBLEND_ZERO, material, Texture);
		d->SetPixelShaderConstantF(0, &saved[0][0], 2);
	}
}
