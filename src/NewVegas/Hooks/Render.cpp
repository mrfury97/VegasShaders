#pragma once

#include <cstdarg>
#include <unordered_set>
#include <fstream>
#include <sstream>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <thread>
#include "../../core/FrameWorker.h"	// UNOFFICIAL 2026-11-03 SkyOnWorker
#include "../../core/GpuProfiler.h"
#include "../../core/GpuTimeline.h"
#include "../../core/PixelInspector.h"	// UNOFFICIAL 2026-10-26 (debugging tool)
#include "../../core/Diagnostics.h"	// UNOFFICIAL 2026-11-01 flash detector + report key
#include "../../core/PointShadowForward.h"
#include "../../core/ShaderWarmUp.h"
#include "../../core/GlassMask.h"	// UNOFFICIAL 2026-10-29 glass reflections (RenderBlendedHook)
#include "../../core/ShaderWarmUp2.h"
#include "../../core/IOWorker2.h"	// UNOFFICIAL 2026-10-20: second loading thread (its once-a-minute log line)
#include "../../core/CpuSplit.h"	// UNOFFICIAL 2026-10-21: game update split (F10 profiling only)
#include "../../core/ConstantFilter.h"
#include "../../core/HiddenObjectCulling.h"
#include "../../core/LampScissor.h"
#include "../../core/LampPassTest.h"
#include "../../core/PassSums.h"
#include "../../core/LampsInOnePass.h"
#include "../../core/OnePassDepth.h"	// UNOFFICIAL 2026-10-26 LampsDepthFirst: the one many-lamp passes drawn depth first
#include "../../core/ClothWind.h"
#include "../../core/PlantWind.h"	// UNOFFICIAL 2026-10-20 plant wind ([Shaders.Plants.Wind])
#include "../../core/EffectSelfTest.h"
#include "../../core/MaterialRules.h"
#include "../../core/PbrPop.h"	// UNOFFICIAL 2026-10-18 PBR pop-in: SpecularFade, SpecularLODPasses (+ test diagnostic)
#include "../../core/PassFamilyView.h"	// UNOFFICIAL 2026-10-21, diagnostic: which pass family draws each lit object
#include "../../core/CasterView.h"	// UNOFFICIAL 2026-10-28, diagnostic: which objects cast into the sun cascade that covers them
#include "../../core/LinearSceneCensus.h"	// UNOFFICIAL 2026-10-28, diagnostic: what draws into the world and how (linear-scene plan)
#include "../../core/LinearScene.h"	// UNOFFICIAL 2026-10-28 LinearScene: the world's main list kept in linear light
#define MIXCHECK_IMPLEMENTATION	// UNOFFICIAL 2026-10-30 NvrDxvkMixCheck: the check's bodies are compiled here, once
#include "../../core/MixCheck.h"
#include "../../core/CapturedNormals.h"	// UNOFFICIAL 2026-10-28 CapturedNormals: the main list's surfaces write their smooth normals
#include "../../core/DeferredLamps.h"	// UNOFFICIAL 2026-10-31 DeferredLamps: the lamps of the deferred pixels in one pass
#include "../../core/FlashlightDeferred.h"	// UNOFFICIAL 2026-11-04 the flashlight as a real spot light on the G-buffer
#include "../../core/FlashlightLamp.h"	// UNOFFICIAL 2026-11-04 and as a real game lamp for what the G-buffer does not hold
#include "../../core/EnvLightFade.h"	// UNOFFICIAL 2026-11-04 the env-map light fade per pixel
#include "../../core/ObjectMotion.h"	// UNOFFICIAL 2026-10-29 ObjectMotion: what moved, noted as the main list draws it
#include "../../core/FoldHighlights.h"	// UNOFFICIAL 2026-10-21 FoldHighlights: several-pass objects draw their highlights with their lamps' light
#include "../../core/FoldFog.h"	// UNOFFICIAL 2026-10-27 FoldFog: env-mapped objects fog in their lit pass, no fog pass
#include "../../core/NewGrass.h"	// UNOFFICIAL 2026-10-18 NewGrass: blade geometry in place of the early grass card draws (GrassFirst)
#include "../../core/GrassShadows.h"	// UNOFFICIAL 2026-10-27 grass sun shadows: the early grass drawn again into the near sun shadow map (GrassFirst)
#include "../../core/RtCore.h"	// UNOFFICIAL 2026-10-30 RT grass: each grass pass's tufts into the ray tracing scene (GrassFirst::NoteRtGrass)
#include "../../effects/Grass/GpuGrass.h"	// UNOFFICIAL 2026-10-27 GPU grass (NVR DXVK 1.7-grass): strands placed and culled on the GPU from the early grass draws
#include "../../core/HdrOutput.h"	// UNOFFICIAL 2026-10-26 HDR output (needs NVR DXVK 1.5-hdr)
void HdrOutputNoteDesktop() { HdrOutput::NoteDesktop(); }	// UNOFFICIAL 2026-10-29: from NVSEPlugin_Load (see HdrOutput.h)
#include "../../core/ReflMaterial.h"	// UNOFFICIAL 2026-10-20 MaterialAware reflections
#include "../../core/RtHairTint.h"	// UNOFFICIAL 2026-11-07 perf-mirrorfix: the hair's tint for the RT scene
#include "../../core/AlphaHash.h"	// UNOFFICIAL 2026-11-03 SmoothCutouts
#include "../../core/CharacterObjectReach.h"	// UNOFFICIAL 2026-10-23 CharacterObjectReach: head/face/headgear object draws flagged (c204)
#include "../../core/DustDraw.h"	// 2026-11-04 dust on top surfaces: static world draws flagged (b14)
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-26: DXVK PROFILE lines (F10 profiling on NVR DXVK 1.5)
#include "../../core/DxvkLoader.h"
#include "../../core/ShadowAtlas.h"	// UNOFFICIAL 2026-10-30 ShadowAtlas (WorldStartConvert)
#include "../../core/LandLamps.h"	// UNOFFICIAL 2026-10-30 LandAllLamps: the land lit by all of its own lamps (plan C)
#include "../../core/ReactiveMask.h"	// UNOFFICIAL 2026-10-30 ReactiveMask: FSR's reactive mask from the world's alpha draws
#include "../../core/SkyCoverage.h"	// UNOFFICIAL 2026-11-04 SkyCoverage: the game's MSAA, exact sky shares
#include "../../core/ForwardCloudShadows.h"	// UNOFFICIAL 2026-11-04 ForwardCloudShadows: the clouds' shadow in the surface shaders
#include "../../core/BendContact.h"	// UNOFFICIAL 2026-11-06 forward Bend contact shadows (LOD only, per draw)

namespace RmaosDraw { static void Upload(); }	// UNOFFICIAL _rmaos maps (below)

// Everything from the start of the game's render call up to the world scene: NVR shadow maps,
// the game's own pre-scene work (water reflection/refraction/depth maps and anything else).
static GpuTimer PreSceneTimer("Pre-scene (to world scene)");
static bool PreSceneTimerActive = false;
static bool PreSceneLabelOpen = false;	// UNOFFICIAL 2026-10-26 GpuDebugLabels: the pre-scene group (GpuDebug.h)
static bool PreSceneMarker = false;	// UNOFFICIAL, test only: the frame recorder's marker for this manual timer
static void EndPreSceneTimer() {
	if (PreSceneMarker) { PreSceneMarker = false; if (GpuTimer::MarkerHook) GpuTimer::MarkerHook(PreSceneTimer.GetName(), false); }
	if (PreSceneLabelOpen) { PreSceneLabelOpen = false; GpuDebug::End(); }
	if (!PreSceneTimerActive) return;
	PreSceneTimer.End();
	PreSceneTimerActive = false;
}

// ---- F10: the game's own draws split by shader family; first shader uses; shader-bind cost ----
// The world scene, the water reflection map and the first-person model are drawn by the game's render
// loop, most families with NVR's replacement pixel shaders. SetShadersHook sees every geometry pass being
// set up. While F10 profiling is on it puts a GPU timestamp wherever the shader family changes inside one
// of those three passes, and the GpuTimeline adds up the GPU time per family ("GPU SPLIT" lines). It also
// counts binds per frame ("SHADER BINDS" lines) and remembers which D3D pixel shaders have already drawn in
// which pass, so a FRAME SPIKE line can name a shader used for the first time in that frame and the
// longest stall between two binds (the draws of a pass happen between its bind and the next one, so a
// driver compiling a shader at its first draw shows up there).
namespace ShaderSplit {
	enum Context : unsigned char { World, Reflections, FirstPerson, ContextCount, Outside = ContextCount };
	static const char* const ContextNames[ContextCount + 1] = { "world scene", "reflections", "first person", "other" };
	static const unsigned MaxLabels = 32; // label 0: from the start of the pass to its first shader bind
	static_assert(ContextCount * MaxLabels * 2 <= GpuTimeline::KeyCount, "split keys do not fit the timeline");

	static char LabelNames[MaxLabels][16] = { "(pass setup)" };
	static unsigned LabelCount = 1;
	static std::unordered_map<const void*, unsigned char> LabelOfShader;
	static GpuTimeline Timeline("World scene split");

	static unsigned char CurrentContext = Outside;
	static unsigned char CurrentKey = GpuTimeline::NoKey;
	static unsigned char SavedContext[8], SavedKey[8];
	static unsigned Depth = 0;

	// First use of a D3D pixel shader in a pass. Tracked all session, F10 or not, so "first" means first.
	static std::unordered_set<UInt64> Seen;
	static const void* LastHandle = nullptr;
	static unsigned char LastHandleContext = Outside;
	static unsigned FirstUsesSession = 0;

	struct Counters {
		unsigned Binds[ContextCount + 1];			// SetShaders calls (one per geometry pass)
		unsigned PixelChanges[ContextCount + 1];	// ... that changed the pixel shader
		unsigned NvrUploads[ContextCount + 1];		// ... to an NVR shader, so ShaderRecord::SetCT ran
		unsigned FamilyChanges[ContextCount + 1];	// timeline marks
		unsigned FirstUses;
		double HookMs;								// CPU time inside SetShadersHook
	};
	static Counters Frame = {}, Window = {};
	static unsigned WindowFrames = 0;
	static double WindowMaxHookMs = 0.0;
	static bool FrameProfiled = false;

	// This frame's first uses and its longest gap between two binds, for FRAME SPIKE lines.
	struct BindInfo { const char* Shader; unsigned char Context; bool Nvr; bool FirstUse; };
	static BindInfo FirstUseBinds[4];
	static BindInfo LastBind = {}, MaxGapBind = {};
	static double LastBindEnd = 0.0, MaxGapMs = 0.0;

	static unsigned char KeyOf(unsigned context, unsigned label, bool nvr) { return (unsigned char)((context * MaxLabels + label) * 2 + (nvr ? 1 : 0)); }

	// UNOFFICIAL 2026-10-17, test only: the same family changes as marks in a recorded frame (FrameRecorder.h), named like the
	// GPU SPLIT lines ("world scene/GRASS/vanilla"), tracked on their own so they do not depend on the timeline having a slot.
	static unsigned char RecKey = GpuTimeline::NoKey;
	static unsigned char RecSaved[8];
	static void RecMark(unsigned char key) {
#if NVR_TEST_TOOLS
		if (key == RecKey) return;
		RecKey = key;
		if (!FrameRecorder::Active()) return;
		char name[64];
		if (key == GpuTimeline::NoKey) strcpy_s(name, "(none)");
		else sprintf_s(name, "%s/%s/%s", ContextNames[(key / 2) / MaxLabels], LabelNames[(key / 2) % MaxLabels], (key & 1) ? "NVR" : "vanilla");
		FrameRecorder::Mark(FrameFormat::kMarkSplit, name);
#else
		(void)key;
#endif
	}

	static void Append(char* buffer, size_t size, size_t& used, const char* format, ...);

	// The lit-object pixel shaders (ObjectTemplate.hlsl, src/effects/PBR.h) split by variant group (P47, roadmap 3J):
	// 2000-2028 sun + up to 3 lights, 2029-2036 up to 6 lights in one pass, 2037-2044 the additive light passes that
	// redraw an object for more lights, 2045-2046 diffuse point lights, 2047-2056 the specular passes.
	static const char* SlsGroup(const char* name) {
		const int number = atoi(name + 3);
		if (number >= 2000 && number <= 2028) return "SLS 1-3 lights";
		if (number >= 2029 && number <= 2036) return "SLS 4+ lights";
		if (number >= 2037 && number <= 2044) return "SLS light pass";
		if (number >= 2045 && number <= 2046) return "SLS diffuse pt";
		if (number >= 2047 && number <= 2056) return "SLS specular";
		return "SLS other";
	}

	// Lights in use by the 4+ light shaders (roadmap 3J). They compute every light slot and multiply the unused ones
	// by 0; how many slots a draw really uses decides whether skipping them pays (tests/game_shaders.cpp: -11 to -29%
	// of their cost with 2 lights, +2 to +4% with all slots used). The game sets the count (EmittanceColor.a = c2.w,
	// or PSLightColor[0].a = c3.w in the OPT variants) after binding the shader, so it is read back from the device at
	// the next bind in the same pass, when that draw is done. Only while F10 runs.
	struct MultiLightShader { int Number; unsigned Slots; bool Opt; };
	static const MultiLightShader MultiLights[] = { { 2029, 6, false }, { 2030, 6, false }, { 2031, 4, false }, { 2032, 4, true },
		{ 2033, 4, false }, { 2034, 3, false }, { 2035, 3, true }, { 2036, 3, false } };
	static const unsigned MultiLightCount = sizeof(MultiLights) / sizeof(MultiLights[0]);
	static unsigned LightsInUse[MultiLightCount][8] = {};	// [shader][lights in use; 7 = 7 or more]
	static unsigned LightsReadFailures = 0;
	static int PendingMultiLight = -1;
	static unsigned char PendingMultiLightContext = Outside;

	static int MultiLightIndex(const char* name) {
		if (!name || strncmp(name, "SLS", 3)) return -1;
		const int number = atoi(name + 3);
		for (unsigned i = 0; i < MultiLightCount; ++i) if (MultiLights[i].Number == number) return (int)i;
		return -1;
	}

	// UNOFFICIAL 2026-10-16 (night cost study): lamps in use by the land shaders with point lights (TerrainTemplate.hlsl,
	// SLS2092-2146: every second number, in fours per texture count: none, 6, 12, 24 lamp slots). The game sets
	// PointLightCount (c88) after binding the shader; read back at the next bind in the same pass, like the objects' above.
	static unsigned TerrainLampSlots(const char* name) {
		if (!name || strncmp(name, "SLS", 3)) return 0;
		const int number = atoi(name + 3);
		if (number < 2092 || number > 2146 || (number & 1)) return 0;
		static const unsigned slots[4] = { 0, 6, 12, 24 };
		return slots[((number - 2092) / 2) % 4];
	}
	static unsigned TerrainLamps[3][26] = {};	// [6 / 12 / 24 slots][lamps in use; 25 = more than 24]
	static unsigned TerrainLampsFailures = 0;
	static int PendingTerrainSlots = -1;
	static unsigned char PendingTerrainContext = Outside;

	static void SampleTerrainLamps(unsigned char context) {
		const int pending = PendingTerrainSlots;
		PendingTerrainSlots = -1;
		if (pending < 0 || context != PendingTerrainContext) return;
		float count[4];
		if (FAILED(TheRenderManager->device->GetPixelShaderConstantF(88, count, 1))) { TerrainLampsFailures++; return; }
		const unsigned used = count[0] < 0.5f ? 0 : count[0] > 24.5f ? 25 : (unsigned)(count[0] + 0.5f);
		TerrainLamps[pending][used]++;
	}

	static void ReportTerrainLamps() {
		static const char* const names[3] = { "6", "12", "24" };
		for (unsigned v = 0; v < 3; ++v) {
			char line[512] = {};
			size_t used = 0;
			unsigned draws = 0;
			for (unsigned n = 0; n < 26; ++n) {
				if (!TerrainLamps[v][n]) continue;
				draws += TerrainLamps[v][n];
				Append(line, sizeof(line), used, " %u%s x%u", n, n == 25 ? "+" : "", TerrainLamps[v][n]);
			}
			if (draws) Logger::Log("TERRAIN LAMPS in use per draw, %s-lamp land shaders, %u draws (lamps x draws):%s", names[v], draws, line);
		}
		if (TerrainLampsFailures) Logger::Log("TERRAIN LAMPS: the device did not return shader constants %u times", TerrainLampsFailures);
		memset(TerrainLamps, 0, sizeof(TerrainLamps));
		TerrainLampsFailures = 0;
	}

	static void SampleLightsInUse(unsigned char context) {
		SampleTerrainLamps(context);
		const int pending = PendingMultiLight;
		PendingMultiLight = -1;
		if (pending < 0 || context != PendingMultiLightContext) return;
		float constants[8];
		if (FAILED(TheRenderManager->device->GetPixelShaderConstantF(2, constants, 2))) { LightsReadFailures++; return; }
		const float used = MultiLights[pending].Opt ? constants[7] : constants[3];
		const unsigned bucket = used >= 0.5f && used < 6.5f ? (unsigned)(used + 0.5f) : used >= 6.5f ? 7 : 0;
		LightsInUse[pending][bucket]++;
	}

	// Light slots the old code computes for a draw with `used` lights, of which a skipping version would compute
	// only these: slot 1 always, slot 2 if used > 1, slot 3 if used >= 2 (sic, the shader's own condition), slot 4 if
	// used >= 3, slot 5 if used >= 4, slot 6 if used >= 5.
	static unsigned NeededSlots(unsigned slots, unsigned used) {
		unsigned needed = 1 + (used > 1) + (used >= 2);
		if (slots > 3) needed += used >= 3;
		if (slots > 4) needed += (used >= 4) + (used >= 5);
		return needed < slots ? needed : slots;
	}

	static void ReportLightsInUse() {
		char line[1024] = {};
		size_t used = 0;
		unsigned draws = 0;
		double computed = 0.0, needed = 0.0;
		for (unsigned i = 0; i < MultiLightCount; ++i) {
			unsigned total = 0;
			for (unsigned b = 0; b < 8; ++b) total += LightsInUse[i][b];
			if (!total) continue;
			draws += total;
			Append(line, sizeof(line), used, " | SLS%d (%u slots):", MultiLights[i].Number, MultiLights[i].Slots);
			for (unsigned b = 0; b < 8; ++b) {
				if (!LightsInUse[i][b]) continue;
				Append(line, sizeof(line), used, " %u%s x%u", b, b == 7 ? "+" : "", LightsInUse[i][b]);
				computed += (double)MultiLights[i].Slots * LightsInUse[i][b];
				needed += (double)NeededSlots(MultiLights[i].Slots, b) * LightsInUse[i][b];
			}
		}
		if (draws)
			Logger::Log("SLS 4+ LIGHTS in use per draw, %u draws: light slots computed for nothing %.0f%%%s", draws,
				100.0 * (computed - needed) / computed, line);
		if (LightsReadFailures) Logger::Log("SLS 4+ LIGHTS: the device did not return shader constants %u times", LightsReadFailures);
		memset(LightsInUse, 0, sizeof(LightsInUse));
		LightsReadFailures = 0;
	}

	// Family of a game pixel shader: the terrain templates by name, the lit-object variant groups (SlsGroup),
	// otherwise the leading letters of the shader name (PAR = parallax objects, SKIN, SM3 = hair and eyes, STLEAF =
	// tree leaves, GRASS, SKY, WATER, ...). Worked out once per shader.
	static unsigned char LabelFor(const NiD3DPixelShader* shader) {
		auto found = LabelOfShader.find(shader);
		if (found != LabelOfShader.end()) return found->second;
		char name[16] = {};
		const char* source = shader ? shader->Name : nullptr;
		const char* terrain = source && TheShaderManager->Shaders.Terrain ? TheShaderManager->Shaders.Terrain->GetTemplate(source).Name : nullptr;
		if (!source) strcpy_s(name, "(no shader)");
		else if (terrain && !strcmp(terrain, "TerrainTemplate")) {
			// UNOFFICIAL 2026-10-16: the land shaders by lamp slots (night cost study), "TERRAIN" = no point lights.
			const unsigned slots = TerrainLampSlots(source);
			if (slots) sprintf_s(name, "TERRAIN %uL", slots);
			else strcpy_s(name, "TERRAIN");
		}
		else if (terrain) strcpy_s(name, !strcmp(terrain, "TerrainLODTemplate") ? "TERRAIN LOD" : "TERRAIN FADE");
		else if (!strncmp(source, "SLS", 3) && source[3] >= '0' && source[3] <= '9') strcpy_s(name, SlsGroup(source));
		else if (!strncmp(source, "SM3", 3)) strcpy_s(name, "SM3");
		else if (!strncmp(source, "SKY", 3)) strcpy_s(name, "SKY");
		else if (!strncmp(source, "WATER", 5)) strcpy_s(name, "WATER");
		else if (!strncmp(source, "ISHDR", 5) || !strncmp(source, "HDR", 3)) strcpy_s(name, "HDR");
		else {
			size_t n = 0;
			while (n < sizeof(name) - 1 && source[n] >= 'A' && source[n] <= 'Z') { name[n] = source[n]; n++; }
			if (!n) strcpy_s(name, "(other)");
		}
		unsigned char label = 0;
		for (unsigned i = 1; i < LabelCount; ++i)
			if (!strcmp(LabelNames[i], name)) { label = (unsigned char)i; break; }
		if (!label) {
			if (LabelCount < MaxLabels - 1) { strcpy_s(LabelNames[LabelCount], name); label = (unsigned char)LabelCount++; }
			else { label = MaxLabels - 1; strcpy_s(LabelNames[label], "(more)"); }
		}
		LabelOfShader[shader] = label;
		return label;
	}

	static void Append(char* buffer, size_t size, size_t& used, const char* format, ...) {
		if (used >= size) return;
		va_list args;
		va_start(args, format);
		const int written = _vsnprintf_s(buffer + used, size - used, _TRUNCATE, format, args);
		va_end(args);
		used = written < 0 ? size : used + written;
	}

	static void SpikeDetail(char* buffer, size_t size) {
		if (!FrameProfiled) return;
		unsigned binds = 0;
		for (unsigned c = 0; c <= ContextCount; ++c) binds += Frame.Binds[c];
		size_t used = 0;
		Append(buffer, size, used, " | shader binds %u (%.2f ms CPU), first uses %u", binds, Frame.HookMs, Frame.FirstUses);
		for (unsigned i = 0; i < Frame.FirstUses && i < 4; ++i)
			Append(buffer, size, used, "%s%s (%s, %s)", i ? ", " : ": ", FirstUseBinds[i].Shader, FirstUseBinds[i].Nvr ? "NVR" : "vanilla", ContextNames[FirstUseBinds[i].Context]);
		if (MaxGapBind.Shader)
			Append(buffer, size, used, " | longest gap between binds %.1f ms, after %s (%s, %s%s)", MaxGapMs, MaxGapBind.Shader,
				MaxGapBind.Nvr ? "NVR" : "vanilla", ContextNames[MaxGapBind.Context], MaxGapBind.FirstUse ? ", first use" : "");
	}

	static void ReportSplit(const GpuTimeline& timeline) {
		struct Row { double Avg, Max; unsigned Label; bool Nvr; };
		for (unsigned c = 0; c < ContextCount; ++c) {
			Row rows[MaxLabels * 2];
			unsigned count = 0;
			double total = 0.0;
			for (unsigned label = 0; label < MaxLabels; ++label) {
				for (unsigned nvr = 0; nvr < 2; ++nvr) {
					const unsigned key = KeyOf(c, label, nvr != 0);
					const double avg = timeline.AverageMs(key);
					if (avg <= 0.0) continue;
					rows[count++] = { avg, timeline.MaxMs(key), label, nvr != 0 };
					total += avg;
				}
			}
			if (!count) continue;
			std::sort(rows, rows + count, [](const Row& a, const Row& b) { return a.Avg > b.Avg; });
			Logger::Log("GPU SPLIT %s, %u frames: %.4f ms per frame by shader family (NVR = Vegas Shaders' replacement shader, vanilla = the game's own)",
				ContextNames[c], timeline.WindowFrames(), total);
			for (unsigned i = 0; i < count; ++i)
				Logger::Log("GPU SPLIT   %-12s %-14s %-7s avg %.4f ms  max %.4f  (%4.1f%%)", ContextNames[c], LabelNames[rows[i].Label],
					rows[i].Nvr ? "NVR" : "vanilla", rows[i].Avg, rows[i].Max, 100.0 * rows[i].Avg / total);
		}
		Logger::Log("GPU SPLIT   most family changes in one frame %u; frames dropped (over %u changes) %u, rejected (clock) %u",
			timeline.MostMarks(), GpuTimeline::MaxMarks, timeline.DroppedFrames(), timeline.RejectedFrames());
	}

	static void ReportCounters() {
		const double n = WindowFrames;
		char line[768] = {};
		size_t used = 0;
		Append(line, sizeof(line), used, "SHADER BINDS per frame (%u frames): binds / pixel shader changes / of those to NVR shaders / family changes:", WindowFrames);
		for (unsigned c = 0; c <= ContextCount; ++c)
			Append(line, sizeof(line), used, "%s %s %.0f / %.0f / %.0f / %.0f", c ? " |" : "", ContextNames[c], Window.Binds[c] / n, Window.PixelChanges[c] / n,
				Window.NvrUploads[c] / n, Window.FamilyChanges[c] / n);
		Logger::Log("%s", line);
		Logger::Log("SHADER BINDS   CPU inside SetShaders avg %.3f ms max %.3f per frame | first shader uses %u in these frames, %u this session",
			Window.HookMs / n, WindowMaxHookMs, Window.FirstUses, FirstUsesSession);
		ReportLightsInUse();
		ReportTerrainLamps();
	}

	static void BeginFrame(IDirect3DDevice9* device) {
		static bool connected = false;
		if (!connected) {
			FrameTimeMonitor::SpikeDetail = &SpikeDetail;
			Timeline.OnReport = &ReportSplit;
			connected = true;
		}
		Depth = 0;
		CurrentContext = Outside;
		CurrentKey = GpuTimeline::NoKey;
		RecKey = GpuTimeline::NoKey;
		Frame = {};
		PendingMultiLight = -1;
		PendingTerrainSlots = -1;
		LastBind = { "(frame start)", Outside, false, false };
		MaxGapBind = {};
		MaxGapMs = 0.0;
		FrameProfiled = GpuTimer::Enabled && !InterfaceManager->IsActive(Menu::kMenuType_Loading);
		LastBindEnd = FrameProfiled ? CpuTimer::NowMs() : 0.0;
		if (FrameProfiled) Timeline.BeginFrame(device);
	}

	static void EndFrame() {
		Timeline.EndFrame();
		if (!FrameProfiled || !GpuTimer::Enabled) return;
		for (unsigned c = 0; c <= ContextCount; ++c) {
			Window.Binds[c] += Frame.Binds[c];
			Window.PixelChanges[c] += Frame.PixelChanges[c];
			Window.NvrUploads[c] += Frame.NvrUploads[c];
			Window.FamilyChanges[c] += Frame.FamilyChanges[c];
		}
		Window.FirstUses += Frame.FirstUses;
		Window.HookMs += Frame.HookMs;
		if (Frame.HookMs > WindowMaxHookMs) WindowMaxHookMs = Frame.HookMs;
		if (++WindowFrames >= 120) {
			ReportCounters();
			Window = {};
			WindowFrames = 0;
			WindowMaxHookMs = 0.0;
		}
	}

	// The game pass being drawn (nests: the stack restores the outer pass and its current family).
	static void BeginContext(Context context) {
		if (Depth < 8) { SavedContext[Depth] = CurrentContext; SavedKey[Depth] = CurrentKey; RecSaved[Depth] = RecKey; }
		Depth++;
		CurrentContext = context;
		CurrentKey = Timeline.InFrame() ? KeyOf(context, 0, false) : GpuTimeline::NoKey;
		Timeline.Mark(CurrentKey);
		RecMark(KeyOf(context, 0, false));
	}

	static void EndContext() {
		if (!Depth) return;
		Depth--;
		CurrentContext = Depth < 8 ? SavedContext[Depth] : (unsigned char)Outside;
		CurrentKey = Depth < 8 ? SavedKey[Depth] : GpuTimeline::NoKey;
		Timeline.Mark(CurrentKey);
		RecMark(Depth < 8 ? RecSaved[Depth] : GpuTimeline::NoKey);
	}

	// Called from SetShadersHook once the pixel shader for the pass is chosen. Start: when the hook began.
	static void OnBind(const NiD3DPixelShader* shader, const IDirect3DPixelShader9* previous, double start) {
		const unsigned char context = CurrentContext;
		const IDirect3DPixelShader9* handle = shader ? shader->ShaderHandle : nullptr;
		const bool nvr = handle && handle != (const IDirect3DPixelShader9*)shader->ShaderHandleBackup;
		bool firstUse = false;
		if (handle && (handle != LastHandle || context != LastHandleContext)) {
			LastHandle = handle;
			LastHandleContext = context;
			firstUse = Seen.insert(((UInt64)(uintptr_t)handle << 2) | (context & 3)).second;
			if (firstUse) FirstUsesSession++;
		}
#if NVR_TEST_TOOLS
		if (context != Outside && FrameRecorder::Active()) RecMark(KeyOf(context, LabelFor(shader), nvr));	// UNOFFICIAL, test only
#endif
		if (!FrameProfiled) return;

		// The previous pass drew between the end of its bind and the start of this one.
		if (LastBindEnd > 0.0 && start - LastBindEnd > MaxGapMs) { MaxGapMs = start - LastBindEnd; MaxGapBind = LastBind; }
		Frame.Binds[context]++;
		if (handle != previous) {
			Frame.PixelChanges[context]++;
			if (nvr) Frame.NvrUploads[context]++;
		}
		LastBind = { shader ? shader->Name : "(no pixel shader)", context, nvr, firstUse };
		if (firstUse && Frame.FirstUses++ < 4) FirstUseBinds[Frame.FirstUses - 1] = LastBind;

		// The previous pass's draw is done: read its lights in use if it was a 4+ light shader, then note this one.
		SampleLightsInUse(context);
		const int multiLight = context != Outside ? MultiLightIndex(shader ? shader->Name : nullptr) : -1;
		if (multiLight >= 0) { PendingMultiLight = multiLight; PendingMultiLightContext = context; }
		const unsigned terrainSlots = context != Outside && nvr ? TerrainLampSlots(shader ? shader->Name : nullptr) : 0;
		if (terrainSlots) { PendingTerrainSlots = terrainSlots == 6 ? 0 : terrainSlots == 12 ? 1 : 2; PendingTerrainContext = context; }

		if (context == Outside || !Timeline.InFrame()) return;
		const unsigned char key = KeyOf(context, LabelFor(shader), nvr);
		if (key != CurrentKey) {
			Timeline.Mark(key);
			CurrentKey = key;
			Frame.FamilyChanges[context]++;
		}
	}

	static void EndBind(double start) {
		const double now = CpuTimer::NowMs();
		Frame.HookMs += now - start;
		LastBindEnd = now;
	}
}

// ---- Which of the game's render entry points ran this frame ----
// UNOFFICIAL 2026-10-17 terrain study, DIAGNOSTIC (only while the F10 GPU profile is on): how many land passes each piece of
// ground gets in the world scene (each pass re-runs the whole terrain shader: parallax, sun shadow, lighting) and which land
// passes they are. Land pass IDs 0x93-0x94 and 0x1EF-0x230 (research\grass-draw-order.md, from WallSoGB's map). Logged every
// 240 frames as TERRAIN PROBE.
namespace TerrainProbe {
	static std::unordered_map<const void*, unsigned> PassesThisFrame;
	static std::unordered_map<UInt16, unsigned long long> ByPass;
	static std::unordered_map<UInt16, std::string> ShadersOfPass;	// pass -> "pixel shader / vertex shader" first seen with it
	static const char* LastPixel = nullptr;	// set in SetShadersHook
	static const char* LastVertex = nullptr;
	static unsigned long long PerGeometry[9] = {}, Geometries = 0, Passes = 0;
	static unsigned Frames = 0;

	static void Note(const PointShadowForward::RenderPassView* pass) {
		if (!GpuTimer::Enabled || ShaderSplit::CurrentContext != ShaderSplit::World || !pass) return;
		const UInt16 e = pass->passEnum;
		if (!((e >= 0x93 && e <= 0x94) || (e >= 0x1EF && e <= 0x230))) return;
		PassesThisFrame[pass->geometry]++;
		ByPass[e]++;
		if (ShadersOfPass.find(e) == ShadersOfPass.end()) ShadersOfPass[e] = std::string(LastPixel ? LastPixel : "?") + "/" + (LastVertex ? LastVertex : "?");
		Passes++;
	}

	static void NewFrame() {
		if (!GpuTimer::Enabled) { PassesThisFrame.clear(); return; }
		for (auto& g : PassesThisFrame) { PerGeometry[g.second < 8 ? g.second : 8]++; Geometries++; }
		PassesThisFrame.clear();
		if (++Frames < 240) return;
		if (Geometries) {
			char line[512]; int n = 0;
			for (int k = 1; k <= 8; k++) n += sprintf_s(line + n, sizeof(line) - n, " %d%s:%.1f%%", k, k == 8 ? "+" : "", 100.0 * PerGeometry[k] / Geometries);
			Logger::Log("TERRAIN PROBE %u frames: %.1f land pieces and %.1f land passes a frame; pieces by pass count:%s",
				Frames, (double)Geometries / Frames, (double)Passes / Frames, line);
			std::vector<std::pair<unsigned long long, UInt16>> top;
			for (auto& b : ByPass) top.push_back({ b.second, b.first });
			std::sort(top.rbegin(), top.rend());
			n = 0;
			for (size_t i = 0; i < top.size() && i < 10; i++)
				n += sprintf_s(line + n, sizeof(line) - n, " %s:%.1f", Pointers::Functions::GetPassDescription(top[i].second), (double)top[i].first / Frames);
			Logger::Log("TERRAIN PROBE passes a frame by kind:%s", line);
			for (size_t i = 0; i < top.size() && i < 10; i++)
				Logger::Log("TERRAIN PROBE   %s drawn with %s", Pointers::Functions::GetPassDescription(top[i].second), ShadersOfPass[top[i].second].c_str());
		}
		Frames = 0; Geometries = Passes = 0; ByPass.clear();
		for (auto& c : PerGeometry) c = 0;
	}
}

// The depth buffers NVR's effects read are refreshed only inside RenderWorldSceneGraphHook. After a save is
// loaded straight into an interior the game has been seen to render frames without entering it at all
// (the F10 log then shows no 'World scene (game)' / 'Depth resolves' samples and 'Pre-scene' equal to the
// whole frame), and the effects run on a stale depth buffer: the scene comes out almost black. This
// records what happened on the first frames after each cell change, and lets ProcessImageSpaceShadersHook
// skip NVR's effects while the world scene keeps going missing (the WorldSceneGuard switch).
static bool WorldRenderedThisFrame = false;
static unsigned WorldMissStreak = 0;        // consecutive earlier frames without a world scene render
static unsigned FrameWorldCalls = 0, FrameFirstPersonCalls = 0, FrameImageSpaceCalls = 0;
static bool FrameWorldArgsKnown = false;
static int FrameWorldArgs[3] = {};          // IsFirstPerson, WireFrame, Arg4 of the first call this frame
static const unsigned WorldGuardFrames = 10;

// The world is not drawn behind the main menu or a loading screen, so those frames say nothing about it.
static bool WorldRenderExpected() {
	return Player && Player->parentCell && !InterfaceManager->IsActive(Menu::MenuType::kMenuType_Main) &&
		!InterfaceManager->IsActive(Menu::MenuType::kMenuType_Loading);
}

// UNOFFICIAL 2026-10-20 MenuSceneGuard (P82 tester's Pip-Boy screenshot: a blurred green and orange glow on the right of the screen
// glass, in daylight): with the Pip-Boy open the game draws only the Pip-Boy over a still, already finished picture of the world, and
// NVR's effects ran on that picture for the guard's first 10 frames. The surroundings cube (Reflections, CubeMap) took in the Pip-Boy's
// own green screen and orange buttons (the stale depth buffers let them through its weapon and distance tests), and the glass, an
// env-mapped surface ([Shaders.PBR.EnvMaps] CubeReflections), reflected them blurred for as long as the Pip-Boy stayed open. Such a
// frame (the menu's background ready, no world drawn) is now skipped at once; the cube keeps the last world frame's surroundings.
static bool WorldSceneGuardActive() {
	if (TheSettingManager->SettingsMain.Main.DisableWorldSceneGuard || WorldRenderedThisFrame || !WorldRenderExpected()) return false;
	return WorldMissStreak >= WorldGuardFrames || (TheSettingManager->SettingsMain.Main.MenuSceneGuard && TESMain::IsMenuBackgroundReady());
}

static void ReportWorldRender(BSRenderedTexture* RenderedTexture, int Arg2, int Arg3) {
	static TESObjectCELL* lastCell = nullptr;
	static unsigned frame = 0, logged = 0, tracePending = 0;
	static bool guardLogged = false;
	frame++;

	TESObjectCELL* cell = Player ? Player->parentCell : nullptr;
	if (!cell) { WorldMissStreak = 0; lastCell = nullptr; return; } // no cell: nothing to report
	if (!WorldRenderExpected()) { WorldMissStreak = 0; return; }     // main menu / loading screen
	if (cell != lastCell) { lastCell = cell; tracePending = 3; }

	const bool missed = !WorldRenderedThisFrame;
	const bool started = missed && WorldMissStreak == 0;
	const bool recovered = !missed && WorldMissStreak >= 3;
	const bool guarded = missed && WorldMissStreak >= WorldGuardFrames;

	if (logged < 80 && (tracePending || started || recovered || (guarded && !guardLogged))) {
		const char* name = cell->GetEditorName();
		Logger::Log("WORLD TRACE frame %u: cell %08X '%s' interior=%d behaveLikeExterior=%d | world scene calls %u%s | first person %u, image space %u | "
			"render args rt=%p %d %d | menuBackgroundReady=%d mainMenu=%d loading=%d | miss streak %u%s%s",
			frame, cell->refID, name ? name : "?", cell->IsInterior() ? 1 : 0, (cell->flags0 & TESObjectCELL::kFlags0_BehaveLikeExterior) ? 1 : 0,
			FrameWorldCalls, FrameWorldArgsKnown ? (FrameWorldArgs[0] ? " (first-person pass)" : " (world pass)") : " (none)",
			FrameFirstPersonCalls, FrameImageSpaceCalls, RenderedTexture, Arg2, Arg3,
			TESMain::IsMenuBackgroundReady() ? 1 : 0, InterfaceManager->IsActive(Menu::MenuType::kMenuType_Main) ? 1 : 0,
			InterfaceManager->IsActive(Menu::MenuType::kMenuType_Loading) ? 1 : 0, WorldMissStreak,
			started ? " [WORLD SCENE NOT RENDERED]" : "", recovered ? " [world scene rendering again]" : "");
		logged++;
		if (tracePending) tracePending--;
		if (guarded) guardLogged = true;
	}
	if (recovered) guardLogged = false;

	WorldMissStreak = missed ? WorldMissStreak + 1 : 0;
}

// Diagnostic (F10 profiling only): where a long frame goes outside NVR's other timers. "Game render (CPU)" is this whole
// hook (the game's render call, NVR's work included), "Present (CPU)" the time the driver keeps the game in Present (a
// wait for the graphics card to catch up shows there), and "Game update (CPU)" the rest between two renders: the game's
// own update (AI, physics, scripts, xNVSE plugins such as GunFX). FRAME SPIKE lines name them when over 0.75 ms.
// Present is hooked in the device's function table the first time profiling is switched on, so a normal run is untouched.
namespace FrameSplit {
	typedef HRESULT (__stdcall* PresentFn)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
	static PresentFn OriginalPresent = nullptr;
	static double RenderEnd = 0.0, PresentMs = 0.0;
	static HRESULT __stdcall PresentHook(IDirect3DDevice9* device, const RECT* source, const RECT* dest, HWND window, const RGNDATA* dirty) {
		CpuSplit::MarkNow(CpuSplit::kPresentBegin);	// UNOFFICIAL 2026-10-21 game update split
		const double start = CpuTimer::NowMs();
		const HRESULT result = OriginalPresent(device, source, dest, window, dirty);
		PresentMs += CpuTimer::NowMs() - start;
		CpuSplit::MarkNow(CpuSplit::kPresentEnd);
		return result;
	}
	static void Install(IDirect3DDevice9* device) {
		static bool tried = false;
		if (tried || !device) return;
		tried = true;
		void** table = *(void***)device;
		DWORD old = 0;
		if (!VirtualProtect(&table[17], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
			Logger::Log("FRAME SPLIT: could not hook Present; Present and Game update times are not available");
			return;
		}
		OriginalPresent = (PresentFn)table[17];
		table[17] = (void*)&PresentHook;
		VirtualProtect(&table[17], sizeof(void*), old, &old);
		Logger::Log("FRAME SPLIT: Present hooked (diagnostic: Present, Game update and Game render CPU times while F10 profiling is on)");
	}
}

void NvrDepthWarningTick();	// RenderManager.cpp
// Camera and shader constants for the frame, in the order the scene and image space need them.
static void PrepareSceneCameras() {
	auto* renderer = TheRenderManager;
	renderer->UpdateSceneCameraData();
	renderer->SetupSceneCamera();
	TheShaderManager->UpdateConstants();
}

using RenderMethod = void(__thiscall*)(Main*, BSRenderedTexture*, int, int);
RenderMethod Render = reinterpret_cast<RenderMethod>(Hooks::Render);
void __fastcall RenderHook(Main* This, UInt32 edx, BSRenderedTexture* RenderedTexture, int Arg2, int Arg3) {
	SessionLog::StageScope stage("game frame");	// 2026-11-04: the CRASH lines' stage (SessionLog.h)
	SessionLog::StallWatch::FrameScope stallFrame;	// UNOFFICIAL 2026-11-07 (perf-ptcost): the STALL watchdog's frame begin / end (SessionLog.h)
	static CpuTimer gameRenderCpuTimer("Game render (CPU)"), presentCpuTimer("Present (CPU)"), gameUpdateCpuTimer("Game update (CPU)");
	const double renderStart = CpuTimer::NowMs();
	if (GpuTimer::Enabled) {
		FrameSplit::Install(TheRenderManager->device);
		CpuSplit::Install();	// UNOFFICIAL 2026-10-21: the game update split ('Upd', 'Rnd', 'AI' lines), patched the first time F10 is on
		if (FrameSplit::RenderEnd > 0.0 && FrameSplit::OriginalPresent) {
			const double between = renderStart - FrameSplit::RenderEnd;
			presentCpuTimer.Add(FrameSplit::PresentMs);
			gameUpdateCpuTimer.Add(between > FrameSplit::PresentMs ? between - FrameSplit::PresentMs : 0.0);
		}
	}
	FrameSplit::PresentMs = 0.0;
	CpuSplit::MarkNow(CpuSplit::kRenderBegin);
	DxvkCompute::ProfileTick(GpuTimer::Enabled);	// UNOFFICIAL 2026-10-26: game thread vs DXVK thread vs GPU (NVR DXVK 1.5)
	NvrDepthWarningTick();	// UNOFFICIAL 2026-10-27: stock DXVK on NVIDIA gives NVR no depth: said once on screen (RenderManager.cpp)
#if NVR_TEST_TOOLS
	ShaderWarmUp2::FrameTick(SettingShots::Running ? (int)SettingShots::Index : -1);	// UNOFFICIAL ShaderWarmUp2: frame times + new setups (shot index: picture frames)
#else
	ShaderWarmUp2::FrameTick(-1);	// UNOFFICIAL ShaderWarmUp2: frame times + new setups
#endif
	IOWorker2::Tick();	// UNOFFICIAL IOWorker2 (off by default): what the second loading thread did, once a minute
	// UNOFFICIAL 2026-10-26 (GpuDebug.h): [Main.Develop.Main] RenderDocCaptureKey asks an already-loaded RenderDoc for a capture
	// of the next frame(s); Tick logs the file it wrote. 0 (the default) = no key, nothing runs.
	if (GpuDebug::CaptureKey > 0 && !InterfaceManager->IsActive(Menu::MenuType::kMenuType_Console) && Global->OnKeyDown((UInt16)GpuDebug::CaptureKey))
		GpuDebug::RequestCapture(GpuDebug::CaptureFrames, "hotkey");
	GpuDebug::Tick();



	TheFrameRateManager->UpdatePerformance();
	TheCameraManager->SetSceneGraph();
	PrepareSceneCameras();
	TerrainErosion::Update();   // [Shaders.Terrain.Erosion]

	RmaosDraw::Upload();	// UNOFFICIAL _rmaos maps: the files read since last frame, made into textures (a few a frame)

	//if (SettingsMain->Develop.TraceShaders && InterfaceManager->IsActive(Menu::MenuType::kMenuType_None) && Global->OnKeyDown(SettingsMain->Develop.TraceShaders) && DWNode::Get() == NULL) DWNode::Create();
	// Whole game frame on the GPU (scene, reflections, NVR effects, image space). Comparing it
	// with the individual buckets shows how much GPU time is not attributed to any of them.
	static GpuTimer frameTimer("Game frame total");
	GpuProfileScope gpu(frameTimer, TheRenderManager->device);
	PreSceneLabelOpen = GpuDebug::Labels && GpuDebug::Begin(PreSceneTimer.GetName());
	PreSceneTimerActive = PreSceneTimer.Begin(TheRenderManager->device);
	if (GpuTimer::MarkerHook) { GpuTimer::MarkerHook(PreSceneTimer.GetName(), true); PreSceneMarker = true; }	// UNOFFICIAL, test only
	WorldRenderedThisFrame = false;
	FrameWorldCalls = FrameFirstPersonCalls = FrameImageSpaceCalls = 0;
	TerrainProbe::NewFrame();	// UNOFFICIAL 2026-10-17 terrain study, diagnostic
	FrameWorldArgsKnown = false;
	ConstantFilter::BeginFrame(TheRenderManager->device);	// UNOFFICIAL: SkipRedundantConstants
	HiddenObjectCulling::BeginFrame(TheRenderManager->device);	// UNOFFICIAL: HiddenObjectCulling
	LampScissor::BeginFrame();	// UNOFFICIAL: LampScissor
	OnePassDepth::BeginFrame(TheRenderManager->device);	// UNOFFICIAL 2026-10-26 LampsDepthFirst
	PassSums::BeginFrame(PassSums::ModeFromSettings());	// UNOFFICIAL ExactPassSums, and 2026-10-21 LampSums (the extra lamp passes summed in linear light)
	ClothWind::BeginFrame(Tes ? Tes->sky : nullptr, TheShaderManager->GameState.isExterior);	// UNOFFICIAL cloth wind
	PipboyFit::BeginFrame();	// UNOFFICIAL 2026-11-03 PipboyFit
	PlantWind::BeginFrame(Tes ? Tes->sky : nullptr, TheShaderManager->GameState.isExterior);	// UNOFFICIAL 2026-10-20 plant wind (after the cloth: it uses its bookkeeping)
#if NVR_TEST_TOOLS
	LampPassTest::BeginFrame();	// UNOFFICIAL, diagnostic: LampPassTestSkip
#endif
	LampsInOnePass::BeginFrame();	// UNOFFICIAL 2026-10-18 LampsInOnePass (before the scene walk builds the objects' passes)
	ManyLamps::BeginFrame(ConstantFilter::Hooked);	// UNOFFICIAL 2026-10-27 ManyLampsOnePass (F10 statistics)
	DeferredLamps::BeginFrame();	// UNOFFICIAL 2026-10-31 DeferredLamps
	LandLamps::BeginFrame();	// UNOFFICIAL 2026-10-30 LandAllLamps
	// UNOFFICIAL ManyLampsOnePass: the block back after the game's writes; 2026-10-30 LandAllLamps: the land's other lamps after VPT's
	struct AfterWrite { static void Both(UINT start, UINT count) { ManyLamps::AfterPixelWrite(start, count); LandLamps::AfterPixelWrite(start, count); } };
	ConstantFilter::PixelWriteAfter = ManyLamps::State == 1 && LandLamps::CompiledIn() ? &AfterWrite::Both : ManyLamps::State == 1 ? &ManyLamps::AfterPixelWrite :
		LandLamps::CompiledIn() ? &LandLamps::AfterPixelWrite : nullptr;
	if (LandLamps::CompiledIn() && !ConstantFilter::Hooked) { static bool warned = false; if (!warned) { warned = true; Logger::Log("[WARNING] LandAllLamps: the pixel-constant hook (ConstantFilter) is not in: nothing added"); } }	// UNOFFICIAL ManyLampsOnePass: the block back after the game's writes
	PbrPop::LodPasses::BeginFrame();	// UNOFFICIAL 2026-10-18 SpecularLODPasses (likewise before the scene walk)
	PbrPop::SpecFade::BeginFrame();	// UNOFFICIAL 2026-10-18 SpecularFade
	PbrPop::SpecDistance::BeginFrame();	// UNOFFICIAL 2026-10-27 SpecularDistance (before the scene walk builds the objects' passes)
	CharacterObjectReach::BeginFrame();	// UNOFFICIAL 2026-10-23 CharacterObjectReach
	DustDraw::BeginFrame();	// 2026-11-04 dust: which object draws may take it (core/DustDraw.h)
	FoldHighlights::BeginFrame();	// UNOFFICIAL 2026-10-21 FoldHighlights
	LinearScene::BeginFrame(TheRenderManager->device);	// UNOFFICIAL 2026-10-28 LinearScene
	ForwardCloudShadows::BeginFrame(TheRenderManager->device);	// UNOFFICIAL 2026-11-04 ForwardCloudShadows (the clouds' last map, its constants)
	{	// UNOFFICIAL 2026-10-28 CapturedNormals: captured while a reader will use them this frame (SnowAccumulation's CapturedNormals, or
		// [Main.Main.Quality] CapturedNormals: merged into the normals buffer every effect reads)
		SnowAccumulationEffect* snow = TheShaderManager->Effects.SnowAccumulation;
		// UNOFFICIAL 2026-10-31 AlbedoBuffer: its readers (DLSS RR's RRAlbedo 2, AmbientLight's AlbedoBuffer) ask for the capture too (AlbedoWanted)
		CapturedNormals::BeginFrame(TheRenderManager->device, (snow && snow->Enabled && snow->CapturedNormals && snow->ShouldRender()) || CapturedNormals::MergeWanted() || CapturedNormals::AlbedoWanted() || FlashlightDeferred::Wanted());
		DisplaceHeight::BeginFrame();	// UNOFFICIAL 2026-11-01 SameHeight: it needs the draw hook (pass-through outside the main list)
		FlashlightLamp::BeginFrame();	// UNOFFICIAL 2026-11-04: the objects the G-buffer holds, this frame and the last
		if (DisplaceHeight::On) CapturedNormals::Hook(TheRenderManager->device);
	}
	ObjectMotion::BeginFrame();	// UNOFFICIAL 2026-10-29 ObjectMotion
	// UNOFFICIAL 2026-10-30 [Main.Main.Performance] NvrDxvkMixCheck (core/MixCheck.h): the setup line at the first frame; with the check on
	// (read at launch) its device hooks go in after LinearScene's and CapturedNormals' (theirs run inside them).
	MixCheck::BeginFrame(TheRenderManager->device, TheShaderManager->Effects.ShadowsExteriors && TheShaderManager->Effects.ShadowsExteriors->Settings.ShadowMaps.MSAA);
	FoldFog::BeginFrame();	// UNOFFICIAL 2026-10-27 FoldFog
	PbrPop::ActorPasses::BeginFrame();	// UNOFFICIAL 2026-10-18 ActorsInOnePass (before the scene walk)
#if NVR_TEST_TOOLS
	LampsInOnePass::Census::NewFrame();	// UNOFFICIAL, diagnostic (F10 only)
	PbrPop::SpecLodTest::BeginFrame();	// UNOFFICIAL, test: PbrPopSpecLODEnd
	PbrPop::PopLog::NewFrame();	// UNOFFICIAL, diagnostic: PbrPopLog
	PassFamilyView::BeginFrame();	// UNOFFICIAL 2026-10-21, diagnostic: PassFamilyView
	CasterView::BeginFrame();	// UNOFFICIAL 2026-10-28, diagnostic: CasterView
	if (!LinearSceneCensus::ContextOf) LinearSceneCensus::ContextOf = []() -> int {
		return ShaderSplit::CurrentContext == ShaderSplit::World ? 1 : ShaderSplit::CurrentContext == ShaderSplit::FirstPerson ? 2 : 0; };
	LinearSceneCensus::BeginFrame(TheRenderManager->device);	// UNOFFICIAL 2026-10-28, diagnostic: LinearSceneCensus
#endif
	// UNOFFICIAL the skin shader (Skin.h): the blur's targets are cleared on the frame's first skin draw; interior faces take
	// SkinShader's own passes while it is in use (Shaders.cpp UpdateFaceGenInteriorFlag: the game's own flag while Vanilla Plus
	// Skin's shaders are in use, so that patch is refreshed every frame whichever is in use).
	if (TheShaderManager->Effects.SkinScattering && TheShaderManager->Shaders.Skin && TheShaderManager->Shaders.Skin->Active())
		TheShaderManager->Effects.SkinScattering->BeginFrame();
	UpdateFaceGenInteriorFlag();
	ShaderSplit::BeginFrame(TheRenderManager->device);
	Render(This, RenderedTexture, Arg2, Arg3);
	SkyProjection::Join();	// UNOFFICIAL 2026-11-03 SkyOnWorker: never past the frame it was started in
	ShaderSplit::EndFrame();
	EndPreSceneTimer();
	ReportWorldRender(RenderedTexture, Arg2, Arg3);
	const double renderEnd = CpuTimer::NowMs();
	FrameSplit::RenderEnd = GpuTimer::Enabled ? renderEnd : 0.0;
	if (GpuTimer::Enabled) gameRenderCpuTimer.Add(renderEnd - renderStart);
	CpuSplit::MarkNow(CpuSplit::kRenderEnd);
}

// Set when NVR changes, mid-frame, a TESR_ constant that game shaders read (CheapReflections). Constants
// reach a game shader only in ShaderRecord::SetCT, which runs when the pixel shader CHANGES, so the next
// bind is told that no pixel shader is bound: an NVR shader then uploads even if the game keeps the one
// already bound. Nothing else changes (the game still sets the device shader itself).
static bool ForcePixelConstants = false;

// ReducedQuality CheapReflections: the water reflection map is drawn without the forward sun-shadow
// lookup (TESR_ShadowForwardData.x = 1, the forward path's own off switch) and without terrain parallax
// (TESR_TerrainParallaxData.x = 0), like the game's own reflections. Both are restored, and uploaded
// again on the next bind, when the pass ends.
// UNOFFICIAL: forward sun shadows are always OFF inside the water reflection pass. NVR's object and terrain shaders
// rebuild each pixel's world position from the MAIN camera's inverse matrices (Shadow.hlsl GetShadowWorldPos); the
// reflection is drawn with the mirrored camera, so that position is wrong there and the shadow lookup paints dark
// patches with ragged edges on the reflected carrier, shore and buildings that change with the view (2026-10-07,
// user's clip). Vanilla reflections carry no shadows either. Parallax stays unless CheapReflections is on.
class CheapReflectionScope {
public:
	CheapReflectionScope() {
		ShadowsExteriorEffect* shadows = TheShaderManager->Effects.ShadowsExteriors;
		TerrainShaders* terrain = TheShaderManager->Shaders.Terrain;
		if (!shadows || !terrain) return;
		Forward = &shadows->Constants.ForwardData.x;
		SavedForward = *Forward;
		*Forward = 1.0f;
		if (TheSettingManager->SettingsMain.Main.CheapReflections) {
			Parallax = &terrain->ParallaxConstants.Data.x;
			SavedParallax = *Parallax;
			*Parallax = 0.0f;
			static bool announced = false;
			if (!announced) {
				Logger::Log("UNOFFICIAL cheap reflections: water reflection map drawn without terrain parallax (forward sun shadows are always off there).");
				announced = true;
			}
		}
		ForcePixelConstants = true;
	}
	~CheapReflectionScope() {
		if (!Forward) return;
		*Forward = SavedForward;
		if (Parallax) *Parallax = SavedParallax;
		ForcePixelConstants = true;
	}

private:
	float* Forward = nullptr;
	float* Parallax = nullptr;
	float SavedForward = 0.0f;
	float SavedParallax = 0.0f;
};

// ReducedQuality CheapUnderwaterTerrain decides per pixel from the camera-relative world position, which the terrain
// shader reconstructs with the MAIN camera's matrices. In the water reflection pass (mirrored camera) that position
// is meaningless, so the water line test is switched off (TESR_TerrainParallaxExtraData.w = -FLT_MAX) for the pass.
class UnderwaterTerrainReflectionScope {
public:
	UnderwaterTerrainReflectionScope() {
		TerrainShaders* terrain = TheShaderManager->Shaders.Terrain;
		if (!TheSettingManager->SettingsMain.Main.CheapUnderwaterTerrain || !terrain) return;
		Waterline = &terrain->ParallaxConstants.ExtraData.w;
		Saved = *Waterline;
		*Waterline = -FLT_MAX;
		ForcePixelConstants = true;
	}
	~UnderwaterTerrainReflectionScope() {
		if (!Waterline) return;
		*Waterline = Saved;
		ForcePixelConstants = true;
	}

private:
	float* Waterline = nullptr;
	float Saved = 0.0f;
};

// Water reflection probe v5, the "reflection lab" (diagnostic, P54 -> v4 2026-10-06 -> v5 2026-10-07). The game draws the
// water reflection map with its own camera inside RenderReflections, and the water shaders look the map up as if that
// camera were an exact mirror of the main one. Looking level or up, the far LOD buildings drop out of the map and
// terrain that lies below the water (the LOD push-down walls) is drawn, so the water-height clip plane is not doing
// what it should for those draws. After an NVR screenshot (ScreenshotKey) the next three reflection passes are recorded,
// one per mode: m0 as the game draws it, m1 with the user clip plane forced OFF at every bind, m2 with the clip plane
// rebuilt by NVR at every bind from the device's own view/projection and the camera-translate global (the game's
// SetClipPlane formula). Each mode saves the final map ("<shot> reflection mK.png"), the map before every bind into it
// ("<shot> reflection mK stage NN <shader>.png") and, for m0, the water depth map. The log gets, per bind: shaders,
// object transform, render target, clip plane and enable, depth/stencil/blend state, c0-c19, the camera globals, the
// plane rebuilt by NVR against the game's, how far c0-c3 is from (object world * view' * projection), and the number of
// draw calls and primitives the run issued. Every SetClipPlane call and every change of D3DRS_CLIPPLANEENABLE in the
// recorded frames is logged with the camera globals and the device view translation at that moment.
//
// RESULT (2026-10-07 05:53, TTW shore, level view): m0 draws the LOD objects (8 calls, 30k triangles) and every pixel of
// them is clipped; m1 shows them all; m2 shows the correct reflection (buildings, far shore, no below-water walls) with a
// plane that differs from the game's by 3e-5. The game's plane value is right at every draw; it is SET once at the start
// of the pass (TESWaterSystem::SetClipPlane, before the pass binds any vertex shader) and the driver misapplies it to the
// later programmable draws. Re-issuing the same plane 0 once the pass's shaders are bound makes it take. That is the fix:
// [Main.Main.Water] ReflectionClipFix (default true) re-sets clip plane 0, unchanged, at every bind of the reflection
// context where it is enabled (before and after the game binds the pass's shaders). m2 now does the same.
extern char LastScreenshotBase[MAX_PATH];
extern char LastScreenshotName[80];
extern bool ScreenshotTakenThisFrame;

namespace ReflectionProbe {
	enum { ModeAsIs = 0, ModeNoClip = 1, ModeReplane = 2, ModeCount = 3 };
	static const int MaxDrawn = 96, MaxStages = 16, MaxPlaneEvents = 48;

	struct CameraShot {
		bool valid = false;
		NiPoint3 pos = {};
		float rot[3][3] = {};
		NiFrustum frustum;
	};
	struct PassShot {
		bool valid = false;
		D3DVIEWPORT9 viewport = {};
		D3DSURFACE_DESC target = {};
		D3DXMATRIX view, proj;
		HRESULT viewHr = E_FAIL, projHr = E_FAIL;
		CameraShot scene, hook;
		NiPoint3 worldTranslate = {}, location = {};  // the game's camera globals the pass renders relative to
		char geometry[64] = {};
		NiPoint3 geometryPos = {};
		DWORD clipEnable = 0, cullMode = 0, zFunc = 0, zEnable = 0;
		float clip[2][4] = {};
	};
	// Every bind in the recorded reflection context.
	struct DrawnObject {
		char name[48]; char shader[24]; char vshader[24];
		NiPoint3 pos; float rot[3][3]; float scale; float radius;
		bool intoMap;
		DWORD clipEnable; float clip0[4], clip1[4];
		DWORD stencil, zEnable, zFunc, zWrite, blend, cull, colorWrite;
		float vs[20][4];                 // vertex constants c0-c19 (c0-c3 ModelViewProj, c8-c11 ObjToCubeSpace, c12 HighDetailRange, c19 GeomorphParams)
		NiPoint3 worldTranslate, location;
		float viewRow4[3];               // the device view's translation row at the bind
		bool viewChanged, projChanged;   // against the pass's first bind
		unsigned long draws, prims;      // draw counters at the bind (the run's own count is the next bind's minus this)
		int stage;
		float rebuilt[4]; float rebuiltDiff; bool replaned, unclipped;
		float mvpDiff, mvpScale;         // c0-c3 against transpose(world * view' * proj): max abs difference, max abs entry
	};
	struct PlaneEvent {                  // a SetClipPlane call or a D3DRS_CLIPPLANEENABLE change in the recorded frames
		bool enableChange; DWORD index; DWORD value; float plane[4];
		NiPoint3 worldTranslate, location; float viewRow4[3]; float proj43; int bindsSoFar; unsigned char context;
	};
	static DrawnObject Drawn[MaxDrawn];
	static PlaneEvent Events[MaxPlaneEvents];
	static int DrawnCount = 0, DrawnTotal = 0, StageCount = 0, EventCount = 0, EventTotal = 0;
	// Every object drawn in the recorded reflection context (RenderPassStandard/Skinned hooks): count, skinned count,
	// and the first names tagged with the bind they were drawn under ("name@bind", "*" = skinned).
	static int ObjTotal = 0, ObjSkinnedTotal = 0, ObjListed = 0;
	static char ObjList[6144];
	static unsigned long EndDraws = 0, EndPrims = 0;
	static bool DrawnDone = false;  // one reflection pass only: set once the world pass follows a recorded one
	static PassShot Shots[2];  // 0: world scene, 1: reflections
	static IDirect3DSurface9* ReflectionTarget = nullptr;
	static NiCamera* HookCamera = nullptr;  // the camera the game passed to RenderReflections
	static int ArmedFrames = 0, Mode = 0;
	static char Base[MAX_PATH], Name[80], Suffix[16];
	static const UInt32 WorldReflectionMapPtr = 0x011C7AD4;  // PlaceableWaterGroup::spWorldReflectionMap
	static const UInt32 DepthMapPtr = 0x011C7B68;            // PlaceableWaterGroup::spDepthMap

	// ---- device vtable hooks: draw counters (always, cheap), SetClipPlane and clip-enable events (recorded frames) ----
	typedef HRESULT (STDMETHODCALLTYPE* DrawIndexedFn)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
	typedef HRESULT (STDMETHODCALLTYPE* DrawPrimFn)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
	typedef HRESULT (STDMETHODCALLTYPE* SetClipPlaneFn)(IDirect3DDevice9*, DWORD, CONST float*);
	typedef HRESULT (STDMETHODCALLTYPE* SetRenderStateFn)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
	static DrawIndexedFn OrigDrawIndexed = nullptr;
	static DrawPrimFn OrigDrawPrim = nullptr;
	static SetClipPlaneFn OrigSetClipPlane = nullptr;
	static SetRenderStateFn OrigSetRenderState = nullptr;
	static unsigned long DrawCalls = 0, DrawPrims = 0;
	static bool InsideOwnCall = false;

	static void NoteEvent(IDirect3DDevice9* device, bool enableChange, DWORD index, DWORD value, const float* plane) {
		EventTotal++;
		if (EventCount >= MaxPlaneEvents) return;
		PlaneEvent& e = Events[EventCount++];
		e.enableChange = enableChange; e.index = index; e.value = value;
		if (plane) memcpy(e.plane, plane, sizeof(e.plane)); else memset(e.plane, 0, sizeof(e.plane));
		e.worldTranslate = *Pointers::Generic::CameraWorldTranslate;
		e.location = *Pointers::Generic::CameraLocation;
		D3DXMATRIX view, proj;
		if (SUCCEEDED(device->GetTransform(D3DTS_VIEW, &view))) { e.viewRow4[0] = view._41; e.viewRow4[1] = view._42; e.viewRow4[2] = view._43; }
		else e.viewRow4[0] = e.viewRow4[1] = e.viewRow4[2] = 0.0f;
		e.proj43 = SUCCEEDED(device->GetTransform(D3DTS_PROJECTION, &proj)) ? proj._43 : 0.0f;
		e.bindsSoFar = DrawnTotal;
		e.context = ShaderSplit::CurrentContext;
	}
	static HRESULT STDMETHODCALLTYPE MyDrawIndexed(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, INT b, UINT mn, UINT nv, UINT si, UINT pc) {
		DrawCalls++; DrawPrims += pc;
		return OrigDrawIndexed(d, t, b, mn, nv, si, pc);
	}
	static HRESULT STDMETHODCALLTYPE MyDrawPrim(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT sv, UINT pc) {
		DrawCalls++; DrawPrims += pc;
		return OrigDrawPrim(d, t, sv, pc);
	}
	static HRESULT STDMETHODCALLTYPE MySetClipPlane(IDirect3DDevice9* d, DWORD index, CONST float* plane) {
		if (ArmedFrames && !InsideOwnCall) NoteEvent(d, false, index, 0, plane);
		return OrigSetClipPlane(d, index, plane);
	}
	static HRESULT STDMETHODCALLTYPE MySetRenderState(IDirect3DDevice9* d, D3DRENDERSTATETYPE state, DWORD value) {
		if (state == D3DRS_CLIPPLANEENABLE && ArmedFrames && !InsideOwnCall) NoteEvent(d, true, 0, value, nullptr);
		return OrigSetRenderState(d, state, value);
	}
	static void InstallDeviceHooks(IDirect3DDevice9* device) {
		if (OrigDrawIndexed || !device) return;
		void** vtable = *(void***)device;
		DWORD old;
		if (!VirtualProtect(&vtable[55], 28 * sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
			Logger::Log("UNOFFICIAL reflection probe: device vtable not writable, no draw counts or plane events");
			return;
		}
		OrigSetClipPlane = (SetClipPlaneFn)vtable[55];
		OrigSetRenderState = (SetRenderStateFn)vtable[57];
		OrigDrawPrim = (DrawPrimFn)vtable[81];
		OrigDrawIndexed = (DrawIndexedFn)vtable[82];
		vtable[55] = (void*)&MySetClipPlane;
		vtable[57] = (void*)&MySetRenderState;
		vtable[81] = (void*)&MyDrawPrim;
		vtable[82] = (void*)&MyDrawIndexed;
		VirtualProtect(&vtable[55], 28 * sizeof(void*), old, &old);
		Logger::Log("UNOFFICIAL reflection probe: device hooks installed (draw counters, clip plane events)");
	}

	// ---- the game's clip plane maths (TESWaterSystem::SetClipPlane 0x4ECEF0) ----
	// World -> clip as the game builds it: the device view (rotation only; the engine renders relative to the camera)
	// with its translation row rebuilt from the camera-translate global, times the device projection.
	static bool WorldToClip(IDirect3DDevice9* device, D3DXMATRIX& out, D3DXMATRIX* viewOut = nullptr, D3DXMATRIX* projOut = nullptr) {
		D3DXMATRIX view, proj;
		if (FAILED(device->GetTransform(D3DTS_VIEW, &view)) || FAILED(device->GetTransform(D3DTS_PROJECTION, &proj))) return false;
		const NiPoint3 t = *Pointers::Generic::CameraWorldTranslate;
		view._41 = -(view._11 * t.x + view._21 * t.y + view._31 * t.z);
		view._42 = -(view._12 * t.x + view._22 * t.y + view._32 * t.z);
		view._43 = -(view._13 * t.x + view._23 * t.y + view._33 * t.z);
		out = view * proj;
		if (viewOut) *viewOut = view;
		if (projOut) *projOut = proj;
		return true;
	}
	// plane_clip = normalize(plane_world) * transpose(inverse(worldToClip))   (D3DXPlaneTransform: out = p * M)
	static void PlaneToClip(const D3DXPLANE& world, const D3DXMATRIX& worldToClip, D3DXPLANE& clip) {
		D3DXMATRIX inv, m;
		D3DXMatrixInverse(&inv, NULL, &worldToClip);
		D3DXMatrixTranspose(&m, &inv);
		D3DXPLANE n;
		D3DXPlaneNormalize(&n, &world);
		D3DXPlaneTransform(&clip, &n, &m);
	}
	// the inverse: plane_world = plane_clip * transpose(worldToClip)
	static void PlaneToWorld(const D3DXPLANE& clip, const D3DXMATRIX& worldToClip, D3DXPLANE& world) {
		D3DXMATRIX tr;
		D3DXMatrixTranspose(&tr, &worldToClip);
		D3DXPlaneTransform(&world, &clip, &tr);
		D3DXPlaneNormalize(&world, &world);
	}
	static float PlaneDiff(const float* a, const float* b) {
		float d = 0.0f;
		for (int i = 0; i < 4; i++) d = max(d, fabsf(a[i] - b[i]));
		return d;
	}

	// The world plane behind the game's current clip plane 0, re-derived whenever the game sets a new plane
	// (world reflection, sky reflection and the depth pass each set their own).
	static float LastGamePlane[4] = {};
	static D3DXPLANE PassWorldPlane;
	static bool PassWorldPlaneValid = false;
	static int PassWorldPlaneDerivations = 0;

	// For the log only: the game's clip plane 0 as a world plane (derived when the game sets a new plane) and the same
	// plane rebuilt with the device's current transforms, against the game's. Only while plane 0 is enabled, so the
	// image-space binds (other transforms) never feed the derivation.
	static void Rebuild(IDirect3DDevice9* device, DWORD clipEnable, float* rebuilt, float* diff) {
		memset(rebuilt, 0, 4 * sizeof(float)); *diff = -1.0f;
		if (!(clipEnable & 1)) return;
		float gamePlane[4];
		if (FAILED(device->GetClipPlane(0, gamePlane))) return;
		D3DXMATRIX worldToClip;
		if (!WorldToClip(device, worldToClip)) return;
		if (!PassWorldPlaneValid || memcmp(gamePlane, LastGamePlane, sizeof(gamePlane)) != 0) {
			PlaneToWorld(*(const D3DXPLANE*)gamePlane, worldToClip, PassWorldPlane);
			memcpy(LastGamePlane, gamePlane, sizeof(gamePlane));
			PassWorldPlaneValid = true;
			PassWorldPlaneDerivations++;
		}
		D3DXPLANE clip;
		PlaneToClip(PassWorldPlane, worldToClip, clip);
		memcpy(rebuilt, &clip, sizeof(D3DXPLANE));
		*diff = PlaneDiff((const float*)&clip, gamePlane);
	}

	// THE FIX: re-issue clip plane 0, unchanged, while the pass's shaders are bound. Returns true when it was re-set.
	static bool TouchPlane(IDirect3DDevice9* device) {
		DWORD clipEnable = 0;
		if (FAILED(device->GetRenderState(D3DRS_CLIPPLANEENABLE, &clipEnable)) || !(clipEnable & 1)) return false;
		float plane[4];
		if (FAILED(device->GetClipPlane(0, plane))) return false;
		InsideOwnCall = true;
		const bool ok = SUCCEEDED(device->SetClipPlane(0, plane));
		InsideOwnCall = false;
		return ok;
	}

	static void Copy(CameraShot& shot, NiCamera* camera) {
		if (!camera) return;
		shot.pos = camera->m_worldTransform.pos;
		memcpy(shot.rot, camera->m_worldTransform.rot.data, sizeof(shot.rot));
		shot.frustum = camera->Frustum;
		shot.valid = true;
	}

	static void SafeName(char* out, size_t size, const char* in) {
		size_t i = 0;
		for (; in && in[i] && i + 1 < size; i++) out[i] = (isalnum((unsigned char)in[i]) || in[i] == '.' || in[i] == '_') ? in[i] : '_';
		out[i] = 0;
	}

	// c0-c3 against transpose(objectWorld * view' * proj), where view' carries the translation from the global.
	static void CheckModelViewProj(DrawnObject& d, IDirect3DDevice9* device, NiGeometry* geometry) {
		d.mvpDiff = d.mvpScale = -1.0f;
		if (!geometry) return;
		D3DXMATRIX worldToClip;
		if (!WorldToClip(device, worldToClip)) return;
		const NiTransform& w = geometry->m_worldTransform;
		D3DXMATRIX model;
		D3DXMatrixIdentity(&model);
		for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) model.m[i][j] = w.scale * w.rot.data[j][i];
		model._41 = w.pos.x; model._42 = w.pos.y; model._43 = w.pos.z;
		const D3DXMATRIX mvp = model * worldToClip;
		float diff = 0.0f, scale = 0.0f;
		for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) {
			const float expected = mvp.m[c][r];  // the game uploads the transpose (row_major float4x4, mul(M, v))
			diff = max(diff, fabsf(d.vs[r][c] - expected));
			scale = max(scale, fabsf(expected));
		}
		d.mvpDiff = diff; d.mvpScale = scale;
	}

	static bool FixOn() { return TheSettingManager->SettingsMain.Main.ReflectionClipFix; }

	// One object about to be drawn (BSBatchRenderer::RenderPassImmediately, through NVR's RenderPassStandard/Skinned hooks).
	static void OnObject(const void* pass, bool skinned) {
		if (!ArmedFrames || DrawnDone || !pass || ShaderSplit::CurrentContext != ShaderSplit::Reflections) return;
		ObjTotal++;
		if (skinned) ObjSkinnedTotal++;
		if (ObjListed >= 120) return;
		const NiGeometry* geometry = *(NiGeometry* const*)pass;
		const char* name = geometry && geometry->m_pcName ? geometry->m_pcName : "(none)";
		const size_t used = strlen(ObjList);
		if (used + strlen(name) + 16 >= sizeof(ObjList)) return;
		sprintf_s(ObjList + used, sizeof(ObjList) - used, "%s%s%s@%d", used ? ", " : "", skinned ? "*" : "", name, DrawnTotal - 1);
		ObjListed++;
	}

	// After the game bound the pass's shaders (SetShadersHook, after the original SetShaders): the fix's second touch.
	static void AfterBind() {
		if (ShaderSplit::CurrentContext != ShaderSplit::Reflections) return;
		if (!FixOn() && !(ArmedFrames && Mode == ModeReplane)) return;
		IDirect3DDevice9* device = TheRenderManager->device;
		if (device) TouchPlane(device);
	}

	static void OnBind(NiGeometry* geometry, const char* pixelShader, const char* vertexShader) {
		const unsigned char context = ShaderSplit::CurrentContext;
		IDirect3DDevice9* device = TheRenderManager->device;
		if (context != ShaderSplit::Reflections) PassWorldPlaneValid = false;
		if (!ArmedFrames) {
			if (context == ShaderSplit::Reflections && FixOn() && device) TouchPlane(device);
			return;
		}
		const int slot = context == ShaderSplit::World ? 0 : context == ShaderSplit::Reflections ? 1 : -1;
		if (slot == 0 && Shots[1].valid && !DrawnDone) { DrawnDone = true; EndDraws = DrawCalls; EndPrims = DrawPrims; }
		if (slot < 0) return;
		if (!Shots[slot].valid) {
			PassShot& s = Shots[slot];
			device->GetViewport(&s.viewport);
			IDirect3DSurface9* target = nullptr;
			if (SUCCEEDED(device->GetRenderTarget(0, &target)) && target) {
				target->GetDesc(&s.target);
				if (slot == 1) { if (ReflectionTarget) ReflectionTarget->Release(); ReflectionTarget = target; }
				else target->Release();
			}
			s.viewHr = device->GetTransform(D3DTS_VIEW, &s.view);
			s.projHr = device->GetTransform(D3DTS_PROJECTION, &s.proj);
			Copy(s.scene, WorldSceneGraph ? WorldSceneGraph->camera : nullptr);
			if (slot == 1) Copy(s.hook, HookCamera);
			device->GetRenderState(D3DRS_CLIPPLANEENABLE, &s.clipEnable);
			device->GetRenderState(D3DRS_CULLMODE, &s.cullMode);
			device->GetRenderState(D3DRS_ZFUNC, &s.zFunc);
			device->GetRenderState(D3DRS_ZENABLE, &s.zEnable);
			for (DWORD i = 0; i < 2; i++) if (FAILED(device->GetClipPlane(i, s.clip[i]))) memset(s.clip[i], 0, sizeof(s.clip[i]));
			s.worldTranslate = *Pointers::Generic::CameraWorldTranslate;
			s.location = *Pointers::Generic::CameraLocation;
			if (geometry) {
				strncpy_s(s.geometry, geometry->m_pcName ? geometry->m_pcName : "(no name)", _TRUNCATE);
				s.geometryPos = geometry->m_worldTransform.pos;
			}
			s.valid = true;
		}
		if (slot != 1 || DrawnDone) return;
		DrawnTotal++;
		if (DrawnCount >= MaxDrawn) return;
		DrawnObject& d = Drawn[DrawnCount++];
		memset(&d, 0, sizeof(d));
		strncpy_s(d.name, geometry && geometry->m_pcName ? geometry->m_pcName : "(none)", _TRUNCATE);
		strncpy_s(d.shader, pixelShader ? pixelShader : "(none)", _TRUNCATE);
		strncpy_s(d.vshader, vertexShader ? vertexShader : "(none)", _TRUNCATE);
		if (geometry) {
			d.pos = geometry->m_worldTransform.pos;
			memcpy(d.rot, geometry->m_worldTransform.rot.data, sizeof(d.rot));
			d.scale = geometry->m_worldTransform.scale;
			NiBound* bound = geometry->GetWorldBound();
			d.radius = bound ? bound->Radius : 0.0f;
		}
		device->GetRenderState(D3DRS_CLIPPLANEENABLE, &d.clipEnable);
		if (FAILED(device->GetClipPlane(0, d.clip0))) memset(d.clip0, 0, sizeof(d.clip0));
		if (FAILED(device->GetClipPlane(1, d.clip1))) memset(d.clip1, 0, sizeof(d.clip1));
		device->GetRenderState(D3DRS_STENCILENABLE, &d.stencil);
		device->GetRenderState(D3DRS_ZENABLE, &d.zEnable);
		device->GetRenderState(D3DRS_ZFUNC, &d.zFunc);
		device->GetRenderState(D3DRS_ZWRITEENABLE, &d.zWrite);
		device->GetRenderState(D3DRS_ALPHABLENDENABLE, &d.blend);
		device->GetRenderState(D3DRS_CULLMODE, &d.cull);
		device->GetRenderState(D3DRS_COLORWRITEENABLE, &d.colorWrite);
		if (FAILED(device->GetVertexShaderConstantF(0, &d.vs[0][0], 20))) memset(d.vs, 0, sizeof(d.vs));
		d.worldTranslate = *Pointers::Generic::CameraWorldTranslate;
		d.location = *Pointers::Generic::CameraLocation;
		D3DXMATRIX view, proj;
		if (SUCCEEDED(device->GetTransform(D3DTS_VIEW, &view))) {
			d.viewRow4[0] = view._41; d.viewRow4[1] = view._42; d.viewRow4[2] = view._43;
			d.viewChanged = memcmp(&view, &Shots[1].view, sizeof(view)) != 0;
		}
		if (SUCCEEDED(device->GetTransform(D3DTS_PROJECTION, &proj))) d.projChanged = memcmp(&proj, &Shots[1].proj, sizeof(proj)) != 0;
		d.draws = DrawCalls; d.prims = DrawPrims;
		d.stage = -1;
		IDirect3DSurface9* target = nullptr;
		if (SUCCEEDED(device->GetRenderTarget(0, &target)) && target) {
			d.intoMap = target == ReflectionTarget;
			target->Release();
		}
		CheckModelViewProj(d, device, geometry);
		Rebuild(device, d.clipEnable, d.rebuilt, &d.rebuiltDiff);  // for the log only
		if (Mode == ModeReplane || FixOn()) d.replaned = TouchPlane(device);
		if (Mode == ModeNoClip && (d.clipEnable & 1)) {
			InsideOwnCall = true;
			d.unclipped = SUCCEEDED(device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0));
			InsideOwnCall = false;
		}
		// What the reflection map holds before this bind's draws (stage 0: just cleared).
		if (d.intoMap && StageCount < MaxStages && ReflectionTarget) {
			char shader[24], file[MAX_PATH];
			SafeName(shader, sizeof(shader), d.shader);
			sprintf_s(file, "%s reflection%s stage %02d %s.png", Base, Suffix, StageCount, shader);
			if (SUCCEEDED(D3DXSaveSurfaceToFileA(file, D3DXIFF_PNG, ReflectionTarget, NULL, NULL))) d.stage = StageCount;
			StageCount++;
		}
	}

	static void LogCamera(const char* label, const CameraShot& c) {
		if (!c.valid) { Logger::Log("UNOFFICIAL reflection probe   %s: none", label); return; }
		// NVR's convention (RenderManager::SetupSceneCamera): columns 0, 1, 2 are forward, up, right.
		const float fx = c.rot[0][0], fy = c.rot[1][0], fz = c.rot[2][0];
		Logger::Log("UNOFFICIAL reflection probe   %s: pos %.1f %.1f %.1f | forward %.4f %.4f %.4f (pitch %.2f deg) | up %.4f %.4f %.4f | right %.4f %.4f %.4f | frustum L %.4f R %.4f T %.4f B %.4f N %.2f F %.0f",
			label, c.pos.x, c.pos.y, c.pos.z, fx, fy, fz, D3DXToDegree(asinf(max(-1.0f, min(1.0f, fz)))),
			c.rot[0][1], c.rot[1][1], c.rot[2][1], c.rot[0][2], c.rot[1][2], c.rot[2][2],
			c.frustum.Left, c.frustum.Right, c.frustum.Top, c.frustum.Bottom, c.frustum.Near, c.frustum.Far);
	}

	static void LogPass(const char* label, const PassShot& s) {
		if (!s.valid) { Logger::Log("UNOFFICIAL reflection probe %s: not drawn in the recorded frames", label); return; }
		Logger::Log("UNOFFICIAL reflection probe %s: target %ux%u format %u, viewport %lu %lu %lux%lu depth %.2f-%.2f",
			label, s.target.Width, s.target.Height, (unsigned)s.target.Format, s.viewport.X, s.viewport.Y, s.viewport.Width, s.viewport.Height, s.viewport.MinZ, s.viewport.MaxZ);
		if (SUCCEEDED(s.viewHr)) {
			const D3DXMATRIX& v = s.view;
			Logger::Log("UNOFFICIAL reflection probe   D3D view: %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.1f %.1f %.1f %.4f",
				v._11, v._12, v._13, v._14, v._21, v._22, v._23, v._24, v._31, v._32, v._33, v._34, v._41, v._42, v._43, v._44);
		}
		else Logger::Log("UNOFFICIAL reflection probe   D3D view: not available (hr %08lX)", (unsigned long)s.viewHr);
		if (SUCCEEDED(s.projHr)) {
			const D3DXMATRIX& p = s.proj;
			Logger::Log("UNOFFICIAL reflection probe   D3D projection: %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.6f %.4f | %.4f %.4f %.4f %.4f",
				p._11, p._12, p._13, p._14, p._21, p._22, p._23, p._24, p._31, p._32, p._33, p._34, p._41, p._42, p._43, p._44);
		}
		else Logger::Log("UNOFFICIAL reflection probe   D3D projection: not available (hr %08lX)", (unsigned long)s.projHr);
		LogCamera("scene camera", s.scene);
		if (&s == &Shots[1]) LogCamera("camera passed to RenderReflections", s.hook);
		Logger::Log("UNOFFICIAL reflection probe   game camera globals: world translate %.1f %.1f %.1f, location %.1f %.1f %.1f | first geometry '%s' at %.1f %.1f %.1f",
			s.worldTranslate.x, s.worldTranslate.y, s.worldTranslate.z, s.location.x, s.location.y, s.location.z,
			s.geometry, s.geometryPos.x, s.geometryPos.y, s.geometryPos.z);
		Logger::Log("UNOFFICIAL reflection probe   states: clip planes enabled 0x%lX, plane 0 %.6f %.6f %.6f %.6f, plane 1 %.6f %.6f %.6f %.6f | cull %lu, zfunc %lu, zenable %lu, engine Z clear %.1f",
			s.clipEnable, s.clip[0][0], s.clip[0][1], s.clip[0][2], s.clip[0][3], s.clip[1][0], s.clip[1][1], s.clip[1][2], s.clip[1][3],
			s.cullMode, s.zFunc, s.zEnable, NiDX9Renderer::GetSingleton()->m_fZClear);
	}

	static void LogDrawn() {
		Logger::Log("UNOFFICIAL reflection probe: %d binds in the recorded reflection context (first %d listed; 'map' = draws into the reflection map; draws/prims = this run's; mvp diff = c0-c3 against transpose(object world * view' * projection), scale = largest entry)",
			DrawnTotal, DrawnCount);
		for (int i = 0; i < DrawnCount; i++) {
			const DrawnObject& d = Drawn[i];
			const unsigned long nextDraws = i + 1 < DrawnCount ? Drawn[i + 1].draws : EndDraws;
			const unsigned long nextPrims = i + 1 < DrawnCount ? Drawn[i + 1].prims : EndPrims;
			Logger::Log("UNOFFICIAL reflection probe   drawn %2d: '%s' (ps %s, vs %s) at %.1f %.1f %.1f scale %.3f r %.0f | %s | clip 0x%lX plane0 %.6f %.6f %.6f %.6f%s%s | rebuilt %.6f %.6f %.6f %.6f (diff %.6f)%s | z en %lu func %lu write %lu, stencil %lu, blend %lu, cull %lu, cw 0x%lX | draws %lu prims %lu | stage %d | mvp diff %.4g of %.4g",
				i, d.name, d.shader, d.vshader, d.pos.x, d.pos.y, d.pos.z, d.scale, d.radius, d.intoMap ? "map" : "other target",
				d.clipEnable, d.clip0[0], d.clip0[1], d.clip0[2], d.clip0[3], d.unclipped ? " [FORCED OFF]" : "", d.replaned ? " [RE-SET]" : "",
				d.rebuilt[0], d.rebuilt[1], d.rebuilt[2], d.rebuilt[3], d.rebuiltDiff, (d.clip1[0] != 0.0f || d.clip1[1] != 0.0f || d.clip1[2] != 0.0f) ? " (plane1 set)" : "",
				d.zEnable, d.zFunc, d.zWrite, d.stencil, d.blend, d.cull, d.colorWrite,
				nextDraws >= d.draws ? nextDraws - d.draws : 0UL, nextPrims >= d.prims ? nextPrims - d.prims : 0UL, d.stage, d.mvpDiff, d.mvpScale);
			Logger::Log("UNOFFICIAL reflection probe              globals wt %.1f %.1f %.1f loc %.1f %.1f %.1f | view row4 %.1f %.1f %.1f%s%s | c0-c3 %.5g %.5g %.5g %.5g | %.5g %.5g %.5g %.5g | %.5g %.5g %.5g %.5g | %.5g %.5g %.5g %.5g",
				d.worldTranslate.x, d.worldTranslate.y, d.worldTranslate.z, d.location.x, d.location.y, d.location.z,
				d.viewRow4[0], d.viewRow4[1], d.viewRow4[2], d.viewChanged ? " VIEW CHANGED" : "", d.projChanged ? " PROJ CHANGED" : "",
				d.vs[0][0], d.vs[0][1], d.vs[0][2], d.vs[0][3], d.vs[1][0], d.vs[1][1], d.vs[1][2], d.vs[1][3],
				d.vs[2][0], d.vs[2][1], d.vs[2][2], d.vs[2][3], d.vs[3][0], d.vs[3][1], d.vs[3][2], d.vs[3][3]);
			if (!strncmp(d.shader, "SLS", 3) || !strncmp(d.vshader, "SLS", 3))
				Logger::Log("UNOFFICIAL reflection probe              c8-c11 %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g | c12 %.4g %.4g %.4g %.4g | c14 %.4g %.4g %.4g %.4g | c16 %.4g %.4g %.4g %.4g | c19 %.4g %.4g %.4g %.4g",
					d.vs[8][0], d.vs[8][1], d.vs[8][2], d.vs[8][3], d.vs[9][0], d.vs[9][1], d.vs[9][2], d.vs[9][3], d.vs[10][0], d.vs[10][1], d.vs[10][2], d.vs[10][3], d.vs[11][0], d.vs[11][1], d.vs[11][2], d.vs[11][3],
					d.vs[12][0], d.vs[12][1], d.vs[12][2], d.vs[12][3], d.vs[14][0], d.vs[14][1], d.vs[14][2], d.vs[14][3], d.vs[16][0], d.vs[16][1], d.vs[16][2], d.vs[16][3], d.vs[19][0], d.vs[19][1], d.vs[19][2], d.vs[19][3]);
		}
		Logger::Log("UNOFFICIAL reflection probe: world plane derived from the game's clip plane: %.6f %.6f %.6f %.3f (height %.2f if horizontal; %d derivations in this capture)",
			PassWorldPlane.a, PassWorldPlane.b, PassWorldPlane.c, PassWorldPlane.d, PassWorldPlane.c != 0.0f ? -PassWorldPlane.d / PassWorldPlane.c : 0.0f, PassWorldPlaneDerivations);
	}

	static void LogEvents() {
		Logger::Log("UNOFFICIAL reflection probe: %d clip plane events in the recorded frames (first %d listed; 'after N binds' counts binds of the reflection context)", EventTotal, EventCount);
		for (int i = 0; i < EventCount; i++) {
			const PlaneEvent& e = Events[i];
			if (e.enableChange)
				Logger::Log("UNOFFICIAL reflection probe   event %2d: CLIPPLANEENABLE = 0x%lX | context %u after %d binds | globals wt %.1f %.1f %.1f loc %.1f %.1f %.1f | view row4 %.1f %.1f %.1f | proj _43 %.4f",
					i, e.value, (unsigned)e.context, e.bindsSoFar, e.worldTranslate.x, e.worldTranslate.y, e.worldTranslate.z, e.location.x, e.location.y, e.location.z, e.viewRow4[0], e.viewRow4[1], e.viewRow4[2], e.proj43);
			else
				Logger::Log("UNOFFICIAL reflection probe   event %2d: SetClipPlane(%lu) %.6f %.6f %.6f %.6f | context %u after %d binds | globals wt %.1f %.1f %.1f loc %.1f %.1f %.1f | view row4 %.1f %.1f %.1f | proj _43 %.4f",
					i, e.index, e.plane[0], e.plane[1], e.plane[2], e.plane[3], (unsigned)e.context, e.bindsSoFar, e.worldTranslate.x, e.worldTranslate.y, e.worldTranslate.z, e.location.x, e.location.y, e.location.z, e.viewRow4[0], e.viewRow4[1], e.viewRow4[2], e.proj43);
		}
	}

	// Which water planes the game knows, and their heights: the reflection must be mirrored about the one in view.
	static void LogWater() {
		if (!Player || !Player->parentCell) return;
		TESWaterForm* form = nullptr;
		const float chosen = (Tes && WorldSceneGraph) ? Tes->GetWaterHeight(Player, WorldSceneGraph, &form) : 0.0f;
		Logger::Log("UNOFFICIAL reflection probe water: player z %.1f, cell water height %.1f, Vegas Shaders' GetWaterHeight %.1f",
			Player->pos.z, Player->parentCell->GetWaterHeight(), chosen);
		WaterManager* water = Tes ? Tes->waterManager : nullptr;
		if (!water) return;
		Logger::Log("UNOFFICIAL reflection probe water manager: groups %u, unk24 %.3f, unk38 %.3f, unk98 %.3f, unk34 %u, unk9C %u",
			water->waterGroups.count, water->unk24, water->unk38, water->unk98, (unsigned)water->unk34, (unsigned)water->unk9C);
		DNode<WaterGroup>* node = water->waterGroups.first;
		for (UInt32 i = 0; node && i < water->waterGroups.count && i < 8; i++, node = node->next) {
			WaterGroup* group = node->data;
			if (!group) continue;
			char planes[256] = "";
			DNode<TESObjectREFR>* plane = group->waterPlanes.first;
			for (UInt32 j = 0; plane && j < group->waterPlanes.count && j < 4; j++, plane = plane->next) {
				NiNode* planeNode = plane->data ? plane->data->GetNode() : nullptr;
				NiBound* bound = planeNode ? planeNode->GetWorldBound() : nullptr;
				char entry[64];
				if (bound) sprintf_s(entry, " [z %.1f r %.0f]", bound->Center.z, bound->Radius);
				else strcpy_s(entry, " [no node]");
				strcat_s(planes, entry);
			}
			Logger::Log("UNOFFICIAL reflection probe water group %u: vector04 %.3f %.3f %.3f %.1f | vector14 %.3f %.3f %.3f %.1f | %u planes%s | bytes %u %u %u %u %u",
				i, group->vector04.x, group->vector04.y, group->vector04.z, group->vector04.w,
				group->vector14.x, group->vector14.y, group->vector14.z, group->vector14.w,
				group->waterPlanes.count, planes, group->byte5C, group->byte5D, group->byte5E, group->byte5F, group->byte60);
		}
	}

	// The game's own reflection map and water depth (refraction) map: their D3D surfaces, whether either is the target
	// the reflection pass drew into (aliasing would let the depth pass draw over the reflection), and the depth map.
	static IDirect3DSurface9* MapSurface(UInt32 pointerAddress, IDirect3DTexture9** textureOut) {
		*textureOut = nullptr;
		BSRenderedTexture* rendered = *(BSRenderedTexture**)pointerAddress;
		if (!rendered || !rendered->RenderedTextures[0] || !rendered->RenderedTextures[0]->rendererData) return nullptr;
		IDirect3DTexture9* texture = rendered->GetD3DTexture(0);
		*textureOut = texture;
		IDirect3DSurface9* surface = nullptr;
		if (!texture || FAILED(texture->GetSurfaceLevel(0, &surface))) return nullptr;
		return surface;
	}

	static void LogMaps() {
		IDirect3DTexture9 *reflTex, *depthTex;
		IDirect3DSurface9* refl = MapSurface(WorldReflectionMapPtr, &reflTex);
		IDirect3DSurface9* depth = MapSurface(DepthMapPtr, &depthTex);
		D3DSURFACE_DESC dd = {};
		if (depth) depth->GetDesc(&dd);
		Logger::Log("UNOFFICIAL reflection probe maps: drawn-into target %p | spWorldReflectionMap texture %p surface %p (%s) | spDepthMap texture %p surface %p %ux%u format %u (%s)",
			ReflectionTarget, reflTex, refl, refl && refl == ReflectionTarget ? "= target" : "not the target",
			depthTex, depth, dd.Width, dd.Height, (unsigned)dd.Format, depth && depth == ReflectionTarget ? "= TARGET: ALIASED" : (depth && depth == refl ? "= reflection map: ALIASED" : "separate"));
		if (depth) {
			if (Mode == ModeAsIs) {
				char file[MAX_PATH];
				sprintf_s(file, "%s water depth map.png", Base);
				const HRESULT hr = D3DXSaveSurfaceToFileA(file, D3DXIFF_PNG, depth, NULL, NULL);
				Logger::Log("UNOFFICIAL reflection probe: water depth map %s (hr %08lX)", SUCCEEDED(hr) ? "saved" : "NOT saved", (unsigned long)hr);
			}
			depth->Release();
		}
		if (refl) refl->Release();
	}

	static void Report() {
		static const char* const modeNames[ModeCount] = { "m0 as the game draws it", "m1 clip plane forced OFF at every bind", "m2 clip plane re-set by Vegas Shaders at every bind" };
		Logger::Log("UNOFFICIAL reflection probe for screenshot %s, %s%s:", Name, modeNames[Mode], FixOn() ? " (ReflectionClipFix on: m0 includes it)" : "");
		LogPass("world pass", Shots[0]);
		LogPass("reflection pass", Shots[1]);
		LogDrawn();
		Logger::Log("UNOFFICIAL reflection probe objects: %d drawn in the reflection context (%d skinned); first %d (name@bind, * = skinned): %s",
			ObjTotal, ObjSkinnedTotal, ObjListed, ObjList[0] ? ObjList : "(none: the per-object hooks are off, or nothing was drawn)");
		LogEvents();
		if (Mode == ModeAsIs) LogWater();
		LogMaps();
		if (ReflectionTarget) {
			char file[MAX_PATH];
			sprintf_s(file, "%s reflection%s.png", Base, Suffix);
			const HRESULT hr = D3DXSaveSurfaceToFileA(file, D3DXIFF_PNG, ReflectionTarget, NULL, NULL);
			Logger::Log("UNOFFICIAL reflection probe: reflection map %s as \"%s reflection%s.png\" (hr %08lX)", SUCCEEDED(hr) ? "saved" : "NOT saved", Name, Suffix, (unsigned long)hr);
			ReflectionTarget->Release();
			ReflectionTarget = nullptr;
		}
	}

	static void Arm(int mode) {
		Mode = mode;
		sprintf_s(Suffix, " m%d", mode);
		Shots[0] = PassShot();
		Shots[1] = PassShot();
		DrawnCount = DrawnTotal = StageCount = EventCount = EventTotal = 0;
		ObjTotal = ObjSkinnedTotal = ObjListed = 0;
		ObjList[0] = 0;
		EndDraws = EndPrims = 0;
		DrawnDone = false;
		PassWorldPlaneValid = false;
		PassWorldPlaneDerivations = 0;
		if (ReflectionTarget) { ReflectionTarget->Release(); ReflectionTarget = nullptr; }
		InstallDeviceHooks(TheRenderManager->device);
		ArmedFrames = 30;  // the reflection pass only runs while water is in view
	}

	// Once per frame, after the screenshot check.
	static void EndFrame() {
		if (ScreenshotTakenThisFrame) {
			ScreenshotTakenThisFrame = false;
			strcpy_s(Base, LastScreenshotBase);
			strcpy_s(Name, LastScreenshotName);
			Arm(ModeAsIs);
			return;
		}
		if (!ArmedFrames) return;
		if ((Shots[0].valid && Shots[1].valid && DrawnDone) || --ArmedFrames == 0) {
			Report();
			ArmedFrames = 0;
			if (Mode + 1 < ModeCount) Arm(Mode + 1);
		}
	}
}

// GunFX master switches (menu Main > GunFX, all off by default): sent to GunFX.dll, which runs the effects, about five
// times a second. Bits: 0 puff, 1 heat smoke strand, 2 ejection smoke, 3 barrel glow, 4 heat haze, 5 muzzle blast,
// 6 energy weapons too (off = no GunFX on lasers, plasma, ...), 7 after-fire trail, 8 volumetric heat smoke/trail.
namespace GunFXSwitches {
	static void Update() {
		static ULONGLONG last = 0;
		const ULONGLONG now = GetTickCount64();
		if (now - last < 200) return;
		last = now;
		typedef void (__cdecl* SetFn)(UInt32);
		static SetFn set = nullptr;
		if (!set) {
			HMODULE gunfx = GetModuleHandleA("GunFX.dll");
			if (gunfx) set = (SetFn)GetProcAddress(gunfx, "GunFX_SetSwitches");
			if (!set) return;
		}
		static const char* const keys[] = { "Puff", "HeatSmoke", "EjectionSmoke", "BarrelGlow", "HeatHaze", "MuzzleBlast", "EnergyWeapons", "AfterFireTrail", "VolumetricSmoke" };
		UInt32 bits = 0;
		for (UInt32 i = 0; i < 9; ++i)
			if (TheSettingManager->GetSettingI("Main.GunFX.Main", keys[i])) bits |= 1u << i;
		set(bits);
	}
}

namespace BarrelHeat {
	typedef bool (__cdecl* GetHeatFn)(float out[10], const void* weaponNode);
	static GetHeatFn GetHeat = nullptr;
	static bool Active = false;
	static bool ProbeFrame = false;
	// UNOFFICIAL 2026-10-13 (user: the glow disappears in some lighting): the game binds an object shader once and then draws
	// every object that uses the same pass with it (no new bind), so the glow set at the bind followed the FIRST object of the
	// run: a gun piece drawn later in it (the barrel after the mag release, each with its own transform) got the glow placed in
	// the other piece's space, or none. Which pieces share a run depends on their lamps and shadows, hence "some lighting".
	// Now also set per object (ForObject, from the RenderPass hooks), with the shaders of the last bind.
	static NiD3DVertexShaderEx* BoundVertex = nullptr;
	static NiD3DPixelShaderEx* BoundPixel = nullptr;
	static float FrameHeat = 0.0f;		// the hottest of the player's two weapon nodes this frame (0: nothing to do)
	static float ProbeHeat[10] = {};
	static bool ProbeBridge = false;
	static unsigned ProbeDraws = 0, ProbeUploads = 0, ProbeSamples = 0;
	static ULONGLONG LastProbe = 0, LastProbeHeat = 0;

	static void Clear() {
		if (!Active) return;
		const float off[12] = {};
		TheRenderManager->device->SetVertexShaderConstantF(171, off, 3);
		TheRenderManager->device->SetPixelShaderConstantF(171, off, 3);
		Active = false;
	}

	static bool ToGunLocal(NiGeometry* geometry, float state[10], const char*& failure) {
		// Rigid weapon vertices are in this geometry's local space in ObjectTemplate.
		// Put the live muzzle there too, without consulting either camera projection.
		if (geometry->skinInstance || fabsf(geometry->m_worldTransform.scale) < 0.001f) { failure = "skin or scale"; return false; }
		const NiTransform& transform = geometry->m_worldTransform;
		auto toLocal = [&](const D3DXVECTOR3& point) {
			const float dx = point.x - transform.pos.x, dy = point.y - transform.pos.y, dz = point.z - transform.pos.z;
			const float inverseScale = 1.0f / transform.scale;
			return D3DXVECTOR3(
				(transform.rot.data[0][0] * dx + transform.rot.data[1][0] * dy + transform.rot.data[2][0] * dz) * inverseScale,
				(transform.rot.data[0][1] * dx + transform.rot.data[1][1] * dy + transform.rot.data[2][1] * dz) * inverseScale,
				(transform.rot.data[0][2] * dx + transform.rot.data[1][2] * dy + transform.rot.data[2][2] * dz) * inverseScale);
		};
		const D3DXVECTOR3 muzzle = toLocal(D3DXVECTOR3(state[0], state[1], state[2]));
		const D3DXVECTOR3 back = toLocal(D3DXVECTOR3(state[0] + state[4] * state[7], state[1] + state[5] * state[7],
			state[2] + state[6] * state[7]));
		D3DXVECTOR3 axis = back - muzzle;
		const float length = D3DXVec3Length(&axis);
		if (!std::isfinite(muzzle.x) || !std::isfinite(muzzle.y) || !std::isfinite(muzzle.z) ||
			!std::isfinite(length) || length < 1.0f || length > 100.0f) { failure = "local transform"; return false; }
		state[0] = muzzle.x; state[1] = muzzle.y; state[2] = muzzle.z;
		state[4] = axis.x / length; state[5] = axis.y / length; state[6] = axis.z / length;
		state[7] = length;
		return true;
	}

	// Temporary v40 diagnostics: sample one third-person frame per second.
	static void BeginWorldProbe() {
		ProbeFrame = false;
		if (!Player || !Player->isThirdPerson || ProbeSamples >= 120) return;
		const ULONGLONG now = GetTickCount64();
		if (now - LastProbe < 1000) return;
		LastProbe = now;
		if (!GetHeat) {
			HMODULE smoke = GetModuleHandleA("GunFX.dll");
			if (smoke) GetHeat = (GetHeatFn)GetProcAddress(smoke, "GunFX_GetHeatV3");
		}
		NiNode* weapon = Player->ActorSkinInfo ? Player->ActorSkinInfo->WeaponNode : nullptr;
		memset(ProbeHeat, 0, sizeof(ProbeHeat));
		ProbeBridge = weapon && GetHeat && GetHeat(ProbeHeat, weapon);
		if (ProbeBridge && ProbeHeat[3] > 0.01f) LastProbeHeat = now;
		if (!LastProbeHeat || now - LastProbeHeat > 15000) return;
		ProbeFrame = true;
		ProbeDraws = ProbeUploads = 0;
		Logger::Log("BarrelHeat probe BEGIN %u: bridge %s, heat %.3f, weapon %p", ++ProbeSamples,
			ProbeBridge ? "yes" : "no", ProbeHeat[3], weapon);
	}

	static void ProbeDraw(NiGeometry* geometry, NiD3DVertexShaderEx* vs, NiD3DPixelShaderEx* ps) {
		if (!ProbeFrame || !geometry || !Player->ActorSkinInfo) return;
		NiNode* weapon = Player->ActorSkinInfo->WeaponNode;
		NiAVObject* node = geometry;
		for (int depth = 0; node && node != weapon && depth < 32; ++depth) node = node->m_parent;
		if (!weapon || node != weapon) return;
		if (++ProbeDraws > 64) return;
		float local[10];
		memcpy(local, ProbeHeat, sizeof(local));
		const char* failure = ProbeBridge ? "none" : "no bridge";
		const bool aligned = ProbeBridge && ToGunLocal(geometry, local, failure);
		NiBound bound = {};
		if (geometry->geomData) bound = geometry->geomData->m_kBound;
		float ambient[4] = {};
		DWORD blending = 0, source = 0, destination = 0;
		auto device = TheRenderManager->device;
		device->GetPixelShaderConstantF(1, ambient, 1);
		device->GetRenderState(D3DRS_ALPHABLENDENABLE, &blending);
		device->GetRenderState(D3DRS_SRCBLEND, &source);
		device->GetRenderState(D3DRS_DESTBLEND, &destination);
		IDirect3DVertexShader9* boundVS = nullptr;
		IDirect3DPixelShader9* boundPS = nullptr;
		device->GetVertexShader(&boundVS);
		device->GetPixelShader(&boundPS);
		Logger::Log("BarrelHeat probe surface %.48s: VS %s NVR=%d bound=%d, PS %s NVR=%d bound=%d; local=%s (%s) muzzle (%.2f %.2f %.2f) axis (%.3f %.3f %.3f) bound (%.2f %.2f %.2f r%.2f) scale %.3f alpha %.3f blend %u/%u/%u",
			geometry->m_pcName ? geometry->m_pcName : "(unnamed)", vs && vs->Name ? vs->Name : "none",
			vs && vs->ShaderHandleBackup && vs->ShaderHandle != vs->ShaderHandleBackup, vs && boundVS == vs->ShaderHandle,
			ps && ps->Name ? ps->Name : "none", ps && ps->ShaderHandleBackup && ps->ShaderHandle != ps->ShaderHandleBackup,
			ps && boundPS == ps->ShaderHandle, aligned ? "yes" : "no", failure,
			local[0], local[1], local[2], local[4], local[5], local[6], bound.Center.x, bound.Center.y, bound.Center.z,
			bound.Radius, geometry->m_worldTransform.scale, ambient[3], blending, source, destination);
		if (boundVS) boundVS->Release();
		if (boundPS) boundPS->Release();
	}

	static void EndWorldProbe() {
		if (ProbeFrame) Logger::Log("BarrelHeat probe END: gun draws %u, heat uploads %u", ProbeDraws, ProbeUploads);
		ProbeFrame = false;
	}

	// Once a frame, before the world: the player's weapons' heat, so objects need no work while the guns are cold.
	static void BeginFrame() {
		FrameHeat = 0.0f;
		if (!Player) return;
		if (!GetHeat) {
			HMODULE smoke = GetModuleHandleA("GunFX.dll");
			if (smoke) GetHeat = (GetHeatFn)GetProcAddress(smoke, "GunFX_GetHeatV3");
			if (!GetHeat) return;
		}
		SkinInfo* skins[2] = { Player->firstPersonSkinInfo, Player->ActorSkinInfo };
		for (SkinInfo* skin : skins) {
			float state[10] = {};
			if (skin && skin->WeaponNode && GetHeat(state, skin->WeaponNode) && state[3] > FrameHeat) FrameHeat = state[3];
		}
	}

	static void Upload(NiGeometry* geometry, NiD3DVertexShaderEx* vertexShader, NiD3DPixelShaderEx* pixelShader) {
		const bool firstPerson = ShaderSplit::CurrentContext == ShaderSplit::FirstPerson;
		if (!firstPerson && ShaderSplit::CurrentContext != ShaderSplit::World) return;
		if (FrameHeat <= 0.0f) return;
		if (!pixelShader || !pixelShader->Name || !pixelShader->ShaderHandleBackup ||
			pixelShader->ShaderHandle == pixelShader->ShaderHandleBackup) return;
		const char* name = pixelShader->Name;
		if (strncmp(name, "SLS20", 5) || strlen(name) != 11 || strcmp(name + 7, ".pso") ||
			(name[5] - '0') * 10 + name[6] - '0' > 56) return;
		// Indoor lighting can use SLS2037-2044 for the only opaque draw.
		// Check the actual blend state instead of guessing the pass from its shader name.
		// Blended light/specular redraws must not apply the glow again.
		if (!firstPerson) {
			DWORD alphaBlend = FALSE;
			if (FAILED(TheRenderManager->device->GetRenderState(D3DRS_ALPHABLENDENABLE, &alphaBlend)) || alphaBlend) return;
		}
		// The local coordinates used by the heat pixel shader come from its matching
		// ObjectTemplate vertex shader. Shadow's interpolated sentinel is unrelated.
		const char* vertexName = vertexShader ? vertexShader->Name : nullptr;
		const bool paired = vertexName && vertexShader->ShaderHandleBackup &&
			vertexShader->ShaderHandle != vertexShader->ShaderHandleBackup &&
			!strncmp(vertexName, "SLS20", 5) && strlen(vertexName) == 11 && !strcmp(vertexName + 7, ".vso") &&
			(vertexName[5] - '0') * 10 + vertexName[6] - '0' <= 56;

		float state[12] = {};
		NiNode* weaponNode = nullptr;
		if (Player) {
			SkinInfo* skin = firstPerson ? Player->firstPersonSkinInfo : Player->ActorSkinInfo;
			if (skin) weaponNode = skin->WeaponNode;
		}
		if (geometry && weaponNode && paired) {
			NiAVObject* node = geometry;
			for (int depth = 0; node && depth < 32; ++depth, node = node->m_parent) {
				if (node == weaponNode) {
					if (!GetHeat) {
						HMODULE smoke = GetModuleHandleA("GunFX.dll");
						if (smoke) GetHeat = (GetHeatFn)GetProcAddress(smoke, "GunFX_GetHeatV3");
					}
					if (GetHeat && GetHeat(state, weaponNode)) {
						BarrelHaze::CaptureWorld(state, firstPerson, weaponNode);
						const char* failure = "none";
						const bool aligned = ToGunLocal(geometry, state, failure);
						if (!aligned) state[3] = 0.0f;
						else {
							const float scale = fabsf(geometry->m_worldTransform.scale);
							state[8] /= scale;
							state[9] /= scale;
							static unsigned hazeLogged[2] = {};
							static ULONGLONG hazeLoggedAt[2] = {};
							const unsigned hazeView = firstPerson ? 0 : 1;
							const ULONGLONG hazeNow = GetTickCount64();
							if (BarrelHaze::Captured == hazeNow && hazeLogged[hazeView] < 8 && hazeNow - hazeLoggedAt[hazeView] > 1000) {
								hazeLogged[hazeView]++;
								hazeLoggedAt[hazeView] = hazeNow;
								Logger::Log("BarrelHaze %s: UV (%.3f %.3f) -> (%.3f %.3f), view depth %.1f, heat %.2f, push %.2f px, plume %.0fx%.0f px%s",
									firstPerson ? "first-person" : "third-person", BarrelHaze::Line.x, BarrelHaze::Line.y,
									BarrelHaze::Line.z, BarrelHaze::Line.w, BarrelHaze::Animation.w, BarrelHaze::Data.x,
									BarrelHaze::Data.y, BarrelHaze::Data.z, BarrelHaze::Data.w,
									firstPerson ? " (first-person field of view)" : "");
							}
						}
						static bool logged[2] = {};
						const unsigned context = ShaderSplit::CurrentContext == ShaderSplit::FirstPerson ? 0 : 1;
						if (!logged[context] && (state[3] > 0.25f || !aligned)) {
							logged[context] = true;
							Logger::Log("BarrelHeat: %s weapon surface %s, heat %.2f, local alignment %s (%s), skinned %s", context ? "third-person" : "first-person",
								geometry->m_pcName ? geometry->m_pcName : "(unnamed)", state[3], aligned ? "yes" : "fallback", failure, geometry->skinInstance ? "yes" : "no");
						}
					}
					break;
				}
			}
		}
		if (state[3] <= 0.0f) return;
		TheRenderManager->device->SetVertexShaderConstantF(171, state, 3);
		TheRenderManager->device->SetPixelShaderConstantF(171, state, 3);
		Active = true;
		if (!firstPerson && ProbeFrame) ++ProbeUploads;
	}

	// A pass's shaders just bound (SetShadersHook), for the object current at the bind.
	static void SetForDraw(NiGeometry* geometry, NiD3DVertexShaderEx* vertexShader, NiD3DPixelShaderEx* pixelShader) {
		BoundVertex = vertexShader;
		BoundPixel = pixelShader;
		if (ShaderSplit::CurrentContext == ShaderSplit::World) ProbeDraw(geometry, vertexShader, pixelShader);
		Upload(geometry, vertexShader, pixelShader);
	}

	// Each object about to be drawn (RenderPassStandard/Skinned hooks): its own glow, or none, with the shaders still bound
	// from the last bind. (If the object's draw binds new shaders, SetShadersHook sets it again for them.)
	static void ForObject(const PointShadowForward::RenderPassView* pass) {
		if (!Active && FrameHeat <= 0.0f) return;
		Clear();
		if (pass && pass->geometry && BoundPixel) Upload(pass->geometry, BoundVertex, BoundPixel);
	}

	// Leaving the world or first-person pass: nothing carried into the next.
	static void EndContext() {
		Clear();
		BoundVertex = nullptr;
		BoundPixel = nullptr;
	}
}

// UNOFFICIAL material rules ([Shaders.PBR.MaterialRules], core/MaterialRules.h, Object.hlsl): before each object, the class of
// its diffuse texture (from VegasShaders.materials.txt next to the DLL, read once) goes to pixel c197-c199. Resolved
// once per texture and kept by the texture's address (with its path, as a freed texture's address can be reused). Only the
// object shaders (ObjectTemplate, MATERIAL_RULES) read these registers; everything else ignores them. Every object shader
// bind uploads the registered "no rule" values (PBR.cpp), so AfterBind sends this object's again.
namespace MaterialRulesDraw {
	static MaterialRules::Table Table;
	static bool Loaded = false;
	struct Entry { std::string Path; int Rule; unsigned Frame = ~0u; };	// Frame: UNOFFICIAL 2026-11-04, the frame its path was last compared in
	static std::unordered_map<const NiSourceTexture*, Entry> ByTexture;
	static float Current[12] = {};
	static bool Active = false;		// Current holds a rule's values (sent; resent after each bind)

	static void Load() {
		Loaded = true;
		// VegasShaders.materials.txt next to the DLL, else an older install's NewVegasReloaded.dll.materials.txt (Paths.h)
		const std::string pathText = Paths::ReadPath(Paths::ModuleDir(), Paths::File::Materials);
		const char* path = pathText.c_str();
		std::ifstream file(path, std::ios::binary);
		if (!file) { Logger::Log("MaterialRules: %s not found, no rules.", path); return; }
		std::stringstream text;
		text << file.rdbuf();
		std::string errors;
		if (!Table.Load(text.str(), errors)) Logger::Log("MaterialRules: lines skipped in %s:\n%s", path, errors.c_str());
		Logger::Log("MaterialRules: %d classes read from %s.", Table.Count() - 1, path);
	}

	static int RuleFor(NiGeometry* geometry, bool log) {
		BSShaderProperty* property = (BSShaderProperty*)geometry->GetProperty(NiProperty::PropertyType::kType_Shade);
		if (!property) return 0;
		const UInt32 vtable = *(UInt32*)property;
		if (vtable != 0x10AE0D0 && vtable != 0x10B9910) return 0;	// BSShaderPPLightingProperty, Lighting30ShaderProperty (nvse/class_vtbls.h)
		if (property->ulFlags[0] & BSShaderProperty::Landscape) return 0;
		BSShaderPPLightingProperty* lighting = (BSShaderPPLightingProperty*)property;
		const NiSourceTexture* diffuse = lighting->ppTextures[0] ? lighting->ppTextures[0][0] : nullptr;
		if (!diffuse || !diffuse->ddsPath1 || !diffuse->ddsPath1[0]) return 0;
		auto known = ByTexture.find(diffuse);
		// UNOFFICIAL 2026-11-04 (cpu-workers trims): the path is compared once per texture per frame, not at every pass of every object: the
		// compare guards against a freed texture's address being reused, and a texture an object is drawn with stays alive for the frame.
		const unsigned frame = TheFrameRateManager ? TheFrameRateManager->FrameNumber : 0;
		if (known != ByTexture.end() && known->second.Frame == frame) return known->second.Rule;
		if (known != ByTexture.end() && known->second.Path == diffuse->ddsPath1) { known->second.Frame = frame; return known->second.Rule; }
		const int rule = Table.Match(MaterialRules::Normalize(diffuse->ddsPath1));
		ByTexture[diffuse] = { std::string(diffuse->ddsPath1), rule, frame };
		if (log && rule) Logger::Log("[MaterialRules] %s -> %s", diffuse->ddsPath1, Table.Get(rule).Name.c_str());
		return rule;
	}

	// One object about to be drawn (RenderPassStandard/Skinned hooks).
	static void ForObject(const PointShadowForward::RenderPassView* pass) {
		PBRShaders* pbr = TheShaderManager->Shaders.PBR;
		if (!pbr || !pbr->MaterialRulesSettings.Enabled) {
			if (Active && pbr) {	// switched off: the "no rule" values back for the object shader still bound
				TheRenderManager->device->SetPixelShaderConstantF(197, (const float*)&pbr->MaterialRule, 1);
				TheRenderManager->device->SetPixelShaderConstantF(198, (const float*)&pbr->MaterialRough, 1);
			}
			Active = false;
			return;
		}
		if (!Loaded) Load();
		const int index = pass && pass->geometry ? RuleFor(pass->geometry, pbr->MaterialRulesSettings.LogMatches) : 0;
		float values[12] = {
			0.0f, 0.0f, 0.0f, 1.0f,
			-1.0f, -1.0f, pbr->MaterialRough.z, 0.0f,
			0.0f, 0.0f, 0.0f, 0.0f };
		if (index) {
			const MaterialRules::Rule& rule = Table.Get(index);
			const bool metal = pbr->MaterialRulesSettings.MetalOn && rule.Metal != MaterialRules::MetalOff;
			values[0] = 1.0f;
			values[1] = metal ? rule.Strength : 0.0f;
			values[2] = rule.Metal == MaterialRules::MetalFull ? 2.0f : 0.0f;	// "mask" is the guess until the _m mask is bound
			values[3] = rule.Reflections;
			values[4] = rule.RoughMin;
			values[5] = rule.RoughMax;
			values[7] = rule.ReplaceAuthored ? 1.0f : 0.0f;	// UNOFFICIAL 2026-11-05 authored=replace (Object.hlsl BridgeSetup: TESR_MaterialRough.w)
			values[8] = rule.Tint[0];
			values[9] = rule.Tint[1];
			values[10] = rule.Tint[2];
		}
		if (Active && !memcmp(values, Current, sizeof(values))) return;
		memcpy(Current, values, sizeof(values));
		Active = true;
		TheRenderManager->device->SetPixelShaderConstantF(197, Current, 3);
	}

	// After an object shader's bind uploaded the registered "no rule" values.
	static void AfterBind() {
		if (Active) TheRenderManager->device->SetPixelShaderConstantF(197, Current, 3);
	}
}

// UNOFFICIAL 2026-11-03 (core/RtCore.cpp: the RT scene's materials, the reference path tracer's off-screen hits): the material rule's
// roughness (the middle of its range; < 0 = the rule gives none) and metal (full = Strength, off = 0, guess / mask = -1: not known
// without the texture) for a mesh, from the same table the object shaders use. False: no rule.
bool RtMaterialRuleOf(NiGeometry* geometry, float* roughness, float* metal) {
	*roughness = -1.0f; *metal = -1.0f;
	if (!MaterialRulesDraw::Loaded) MaterialRulesDraw::Load();
	const int index = geometry ? MaterialRulesDraw::RuleFor(geometry, false) : 0;
	if (!index) return false;
	const MaterialRules::Rule& rule = MaterialRulesDraw::Table.Get(index);
	if (rule.RoughMin >= 0.0f && rule.RoughMax >= 0.0f) *roughness = 0.5f * (rule.RoughMin + rule.RoughMax);
	if (rule.Metal == MaterialRules::MetalFull) *metal = rule.Strength;
	else if (rule.Metal == MaterialRules::MetalOff) *metal = 0.0f;
	return true;
}

// UNOFFICIAL 2026-11-07 (ptaaa-materials) the material rule's class name for a geometry's diffuse texture ("" when no rule covers it):
// RtCore's glass panes ("glass") and foliage cards ("plant") for the path tracer
const char* RtMaterialClassOf(NiGeometry* geometry) {
	if (!MaterialRulesDraw::Loaded) MaterialRulesDraw::Load();
	const int index = geometry ? MaterialRulesDraw::RuleFor(geometry, false) : 0;
	return index ? MaterialRulesDraw::Table.Get(index).Name.c_str() : "";
}

// UNOFFICIAL 2026-10-14 _rmaos maps ([Shaders.PBR.Rmaos], Object.hlsl RMAOS): before each object, its diffuse texture's
// <name>_rmaos.dds (beside it, or the same path under textures\pbr\; a diffuse named *_d.dds also tries the name without _d)
// is bound on texture stage 2, and pixel c220 tells the object shader whether it has one. Loose files only (not BSAs). Each
// diffuse path is looked up once; the files are read on a worker thread and made into textures a few per frame, so a new
// area's maps arrive over a few frames instead of stalling one (until then those objects draw as they did). The textures are
// kept (GPU memory only, D3DPOOL_DEFAULT, so nothing of them sits in the game's 4 GB) for the rest of the session.
namespace RmaosDraw {
	struct Map { std::string File; IDirect3DTexture9* Texture = nullptr; bool Missing = false; bool Queued = false; double ArrivedMs = 0.0;
		float ReflRough = -1.0f; unsigned ReflGeneration = 0; };	// UNOFFICIAL 2026-10-21 RoughnessOverrides (core/ReflMaterial.h), cached per map
	static std::unordered_map<std::string, Map> ByPath;			// normalised diffuse path -> its map
	struct Entry { std::string Path; Map* Found; };
	static std::unordered_map<const NiSourceTexture*, Entry> ByTexture;
	struct Job { Map* Target; std::string File; std::vector<char> Bytes; };
	static std::mutex Lock;
	static std::condition_variable Wake;
	static std::deque<Job> ToRead, Read;
	static bool WorkerStarted = false, Checked = false, Compiled = false;
	static float Current[4] = { -1.0f, 0.0f, 0.0f, 0.0f };	// c220 as last sent this frame
	static bool ThisDraw = false;	// UNOFFICIAL 2026-10-20: the object being drawn has its map on (MaterialAware reflections, core/ReflMaterial.h)
	// UNOFFICIAL 2026-10-18 FadeIn (pbrpop, [Shaders.PBR.Rmaos] FadeIn, seconds, live): a map that has just arrived comes in over this time
	// (c220.x = its share, Object.hlsl blends its values in from the object's look without it) instead of at once. 0 = at once, as before.
	static float FadeSeconds = 0.0f;
	static double FrameNowMs = 0.0;
	static int Loaded = 0;

	static void Worker() {
		for (;;) {
			Job job;
			{
				std::unique_lock<std::mutex> hold(Lock);
				Wake.wait(hold, [] { return !ToRead.empty(); });
				job = std::move(ToRead.front());
				ToRead.pop_front();
			}
			std::ifstream file(job.File, std::ios::binary);
			if (file) job.Bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
			std::lock_guard<std::mutex> hold(Lock);
			Read.push_back(std::move(job));
		}
	}

	static bool Exists(const std::string& file) {
		const DWORD attributes = GetFileAttributesA(file.c_str());
		return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
	}

	// The map for a normalised diffuse path ("textures\...\x.dds"), looked up on disk the first time.
	static Map* Find(const std::string& path, bool log) {
		auto known = ByPath.find(path);
		if (known != ByPath.end()) return &known->second;
		Map& map = ByPath[path];
		map.Missing = true;
		if (path.size() < 13 || path.compare(path.size() - 4, 4, ".dds") != 0) return &map;
		const std::string base = path.substr(0, path.size() - 4);
		std::vector<std::string> stems = { base };
		if (base.size() > 2 && base.compare(base.size() - 2, 2, "_d") == 0) stems.push_back(base.substr(0, base.size() - 2));
		std::vector<std::string> candidates;
		for (const std::string& stem : stems) candidates.push_back(stem + "_rmaos.dds");
		if (base.compare(0, 13, "textures\\pbr\\") != 0)
			for (const std::string& stem : stems) candidates.push_back("textures\\pbr\\" + stem.substr(9) + "_rmaos.dds");
		for (const std::string& c : candidates) {
			const std::string file = "Data\\" + c;
			if (!Exists(file)) continue;
			map.File = file;
			map.Missing = false;
			break;
		}
		if (log) Logger::Log("[Rmaos] %s -> %s", path.c_str(), map.Missing ? "none" : map.File.c_str());
		return &map;
	}

	// Once a frame: the files the worker has read, made into textures (at most two a frame), and c220 resent with the first object
	// (the effects drawn between frames use the high constant registers too).
#if NVR_TEST_TOOLS
	static void ReloadTest();
#endif
	// UNOFFICIAL 2026-10-18 UploadBudget (pbrpop, [Shaders.PBR.Rmaos], live): with a value above 0, as many maps a frame as fit in that many
	// milliseconds (at least one) instead of two: a new place's maps (hundreds indoors) arrive in a fraction of the frames, so fewer
	// objects are seen changing their look when theirs comes in. 0 = two a frame, as before.
	static void Upload() {
		Current[0] = -1.0f;
#if NVR_TEST_TOOLS
		ReloadTest();
#endif
		const float budget = TheSettingManager->GetSettingF("Shaders.PBR.Rmaos", "UploadBudget");
		FadeSeconds = (std::max)(TheSettingManager->GetSettingF("Shaders.PBR.Rmaos", "FadeIn"), 0.0f);
		const double start = CpuTimer::NowMs();
		FrameNowMs = start;
		int uploaded = 0;
		for (int n = 0; budget > 0.0f ? (n == 0 || CpuTimer::NowMs() - start < budget) : n < 2; n++) {
			Job job;
			{
				std::lock_guard<std::mutex> hold(Lock);
				if (Read.empty()) break;
				job = std::move(Read.front());
				Read.pop_front();
			}
			IDirect3DTexture9* texture = nullptr;
			if (!job.Bytes.empty())
				D3DXCreateTextureFromFileInMemoryEx(TheRenderManager->device, job.Bytes.data(), (UINT)job.Bytes.size(), D3DX_DEFAULT_NONPOW2,
					D3DX_DEFAULT_NONPOW2, D3DX_DEFAULT, 0, D3DFMT_UNKNOWN, D3DPOOL_DEFAULT, D3DX_DEFAULT, D3DX_DEFAULT, 0, NULL, NULL, &texture);
			if (texture) { job.Target->Texture = texture; job.Target->ArrivedMs = CpuTimer::NowMs(); Loaded++; }
			else { job.Target->Missing = true; Logger::Log("[Rmaos] could not load %s", job.File.c_str()); }
			uploaded++;
		}
#if NVR_TEST_TOOLS
		// DIAGNOSTIC (pbrpop, with [Main.Main.Misc] PbrPopLog on): how fast a place's maps arrive.
		static unsigned lines = 0;
		if (TheSettingManager->GetSettingI("Main.Main.Misc", "PbrPopLog") && lines < 3000) {
			size_t toRead = 0, toUpload = 0;
			{ std::lock_guard<std::mutex> hold(Lock); toRead = ToRead.size(); toUpload = Read.size(); }
			if (uploaded || toRead || toUpload) {
				lines++;
				Logger::Log("RMAOS STREAM: uploaded %d in %.2f ms (loaded so far %d); waiting: %u to read, %u to upload", uploaded, CpuTimer::NowMs() - start, Loaded,
					(unsigned)toRead, (unsigned)toUpload);
			}
		}
#endif
	}

#if NVR_TEST_TOOLS
	// TEST (pbrpop, [Shaders.PBR.Rmaos] ReloadTest): a new nonzero value drops every loaded map, so they arrive again as in a new place
	// (the arrival seen with a still camera). Maps still on their way are left alone.
	static void ReloadTest() {
		static int last = 0;
		const int want = TheSettingManager->GetSettingI("Shaders.PBR.Rmaos", "ReloadTest");
		if (want == last) return;
		last = want;
		if (!want) return;
		TheRenderManager->device->SetTexture(2, nullptr);
		TheRenderManager->renderState->NoteDeviceTexture(2, nullptr);
		int dropped = 0;
		for (auto& entry : ByPath) {
			Map& map = entry.second;
			if (!map.Texture) continue;
			map.Texture->Release();
			map.Texture = nullptr;
			map.Queued = false;
			dropped++;
		}
		Loaded -= dropped;
		Logger::Log("RMAOS STREAM: ReloadTest %d: %d maps dropped (they load again as they are drawn)", want, dropped);
	}
#endif

	// Every draw with a map binds it (stage 2 is the effects' too between the passes); without one, stage 2 is left as it is:
	// the shader does not read it then.
	static void Send(const float values[4], IDirect3DTexture9* texture) {
		if (texture) {
			TheRenderManager->device->SetTexture(2, texture);
			TheRenderManager->renderState->NoteDeviceTexture(2, texture);
			const DWORD states[][2] = { {D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP}, {D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP},
				{D3DSAMP_MAGFILTER, D3DTEXF_LINEAR}, {D3DSAMP_MINFILTER, D3DTEXF_LINEAR}, {D3DSAMP_MIPFILTER, D3DTEXF_LINEAR} };
			for (const auto& s : states) TheRenderManager->SetSamplerState(2, (D3DSAMPLERSTATETYPE)s[0], s[1]);
		}
		if (!memcmp(values, Current, sizeof(Current))) return;
		memcpy(Current, values, sizeof(Current));
		TheRenderManager->device->SetPixelShaderConstantF(220, Current, 1);
	}

	// One object about to be drawn (RenderPassStandard/Skinned hooks).
	static void ForObject(const PointShadowForward::RenderPassView* pass) {
		ThisDraw = false;
		if (!Checked) {
			Checked = true;
			Compiled = TheSettingManager->GetSettingI("Shaders.PBR.Rmaos", "Enabled") != 0;	// as ShaderRecord compiled it
		}
		if (!Compiled) return;
		PBRShaders* pbr = TheShaderManager->Shaders.PBR;
		float values[4] = { 0.0f, pbr ? pbr->RmaosSettings.OcclusionNow : 1.0f, pbr ? (float)pbr->RmaosSettings.DebugView : 0.0f,
			pbr && pbr->RmaosSettings.MetalFromMap ? 1.0f : 0.0f };	// UNOFFICIAL 2026-10-28 w: MetalFromMap (Object.hlsl getDerivedMetallic)	// UNOFFICIAL 2026-10-18: Occlusion x the weather's AOScale
		Map* map = nullptr;
		NiGeometry* geometry = pass ? pass->geometry : nullptr;
		BSShaderProperty* property = geometry ? (BSShaderProperty*)geometry->GetProperty(NiProperty::PropertyType::kType_Shade) : nullptr;
		const UInt32 vtable = property ? *(UInt32*)property : 0;
		if ((vtable == 0x10AE0D0 || vtable == 0x10B9910) && !(property->ulFlags[0] & BSShaderProperty::Landscape)) {	// BSShaderPPLightingProperty, Lighting30ShaderProperty
			BSShaderPPLightingProperty* lighting = (BSShaderPPLightingProperty*)property;
			const NiSourceTexture* diffuse = lighting->ppTextures[0] ? lighting->ppTextures[0][0] : nullptr;
			if (diffuse && diffuse->ddsPath1 && diffuse->ddsPath1[0]) {
				auto known = ByTexture.find(diffuse);
				if (known != ByTexture.end() && known->second.Path == diffuse->ddsPath1) map = known->second.Found;
				else {
					map = Find(MaterialRules::Normalize(diffuse->ddsPath1), pbr && pbr->RmaosSettings.LogMatches);
					ByTexture[diffuse] = { std::string(diffuse->ddsPath1), map };
				}
			}
		}
		if (map && !map->Missing && !map->Texture && !map->Queued) {
			map->Queued = true;
			std::lock_guard<std::mutex> hold(Lock);
			if (!WorkerStarted) { WorkerStarted = true; std::thread(Worker).detach(); }
			ToRead.push_back({ map, map->File, {} });
			Wake.notify_one();
		}
		const bool on = map && map->Texture && pbr && pbr->RmaosSettings.On;
		float share = 1.0f;	// UNOFFICIAL FadeIn: the map's share while it comes in
		if (on && FadeSeconds > 0.0f) share = std::clamp((float)((FrameNowMs - map->ArrivedMs) / (1000.0 * FadeSeconds)), 0.001f, 1.0f);
		values[0] = on ? share : 0.0f;
		ThisDraw = on;
		if (on && ReflMaterial::Writing && map->ReflGeneration != ReflMaterial::OverrideGeneration) {
			map->ReflRough = ReflMaterial::RoughFor(map->File);
			map->ReflGeneration = ReflMaterial::OverrideGeneration;
		}
		ReflMaterial::DrawRough = on ? map->ReflRough : -1.0f;
		Send(values, on ? map->Texture : nullptr);
	}

	int LoadedCount() { return Loaded; }
}

using SetShadersMethod = void(__thiscall*)(BSShader*, UInt32);
SetShadersMethod SetShaders = reinterpret_cast<SetShadersMethod>(Hooks::SetShaders);
namespace ScopeSunShadows { static void ForDraw(const PointShadowForward::RenderPassView* pass); }

// UNOFFICIAL 2026-10-13: blood on the first-person weapon and gloves (Shaders/Includes/HandBlood.hlsl; the idea and its wiring
// are mrfury97's HUD raindrops in the Physically-Based-Reloaded testbed). Fresh blood splats appearing near the camera (an NPC
// hit up close: BloodFlowNewBloodNear, BloodFlow.cpp) add blood, more the nearer; it dries over DryTime and fades over FadeTime,
// and goes underwater. The object shaders draw it only when compiled in ([Shaders.HandBlood.Status] Enabled at startup,
// ShaderRecord.cpp HAND_BLOOD) and only for draws whose c218.x is above 0: set here for every first-person pass, cleared for
// the first other one.
float BloodFlowNewBloodNear(const D3DXVECTOR3& eye, float reach);
bool BloodFlowLastTint(float out[3]);
namespace HandBlood {
	static bool Checked = false, Compiled = false, Set = false;
	static float Amount = 0.0f, Wet = 0.0f, Seed = 0.0f, Spray = 0.0f;
	static float SinceHit = 1e6f;	// seconds since blood was last added (the drops run for a few seconds after)
	static float Draw[8] = {};
	static ULONGLONG LastTick = 0;
	static int Logged = 0;

	// Once a frame, before the scene.
	static void Update() {
		if (!Checked) {
			Checked = true;
			Compiled = TheSettingManager->GetSettingI("Shaders.HandBlood.Status", "Enabled") != 0;
			Logger::Log("UNOFFICIAL HandBlood: %s", Compiled ? "on (blood on the first-person weapon and gloves)" : "off (not compiled into the shaders)");
		}
		if (!Compiled) return;
		const char* section = "Shaders.HandBlood.Main";
		const ULONGLONG now = GetTickCount64();
		const float dt = LastTick ? std::clamp((now - LastTick) / 1000.0f, 0.0f, 0.25f) : 0.0f;
		LastTick = now;
		const float reach = std::clamp(TheSettingManager->GetSettingF(section, "Reach"), 10.0f, 1000.0f);
		const D3DXVECTOR3 eye(TheRenderManager->CameraPosition.x, TheRenderManager->CameraPosition.y, TheRenderManager->CameraPosition.z);
		const float nearest = BloodFlowNewBloodNear(eye, reach);
		if (nearest < reach) {
			if (Amount < 0.02f) { Seed = (float)(rand() % 997); Spray = (rand() % 6283) / 1000.0f; }	// clean: a new spatter
			const float closeness = 1.0f - nearest / reach;
			Amount = std::clamp(Amount + std::clamp(TheSettingManager->GetSettingF(section, "PerHit"), 0.0f, 1.0f) * (0.35f + 0.65f * closeness), 0.0f, 1.0f);
			Wet = 1.0f;
			SinceHit = 0.0f;
			if (Logged < 20) { Logged++; Logger::Log("UNOFFICIAL HandBlood: fresh blood %.0f units away: amount %.2f", nearest, Amount); }
		}
		const float dryTime = std::clamp(TheSettingManager->GetSettingF(section, "DryTime"), 1.0f, 3600.0f);
		const float fadeTime = std::clamp(TheSettingManager->GetSettingF(section, "FadeTime"), 0.0f, 36000.0f);
		Wet = std::clamp(Wet - dt / dryTime, 0.0f, 1.0f);
		if (fadeTime > 0.0f) Amount *= expf(-dt / fadeTime);
		if (TheShaderManager->GameState.isUnderwater) Amount = 0.0f;	// washed off
		if (Amount < 0.003f) Amount = 0.0f;
		if (TheSettingManager->GetSettingI(section, "DebugView")) { Amount = 1.0f; Wet = 1.0f; }
		// The colour: the game's own blood decals' (their colour times the splatter texture's grey, BloodFlow's MatchShade), as a
		// texture colour; Color until one has been seen.
		float tint[3];
		const float shade = std::clamp(TheSettingManager->GetSettingF("Shaders.BloodFlow.Main", "MatchShade"), 0.01f, 1.0f);
		if (BloodFlowLastTint(tint)) for (int c = 0; c < 3; c++) Draw[4 + c] = std::clamp(tint[c] * shade, 0.0f, 1.0f);
		else {
			Draw[4] = TheSettingManager->GetSettingF(section, "ColorR");
			Draw[5] = TheSettingManager->GetSettingF(section, "ColorG");
			Draw[6] = TheSettingManager->GetSettingF(section, "ColorB");
		}
		// Darker than the decals' own colour by Brightness (the blood is thick on the gun); the drops run over RunTime seconds
		// after each hit, as far as Run says.
		const float brightness = std::clamp(TheSettingManager->GetSettingF(section, "Brightness"), 0.05f, 2.0f);
		for (int c = 4; c < 7; c++) Draw[c] = std::clamp(Draw[c] * brightness, 0.0f, 1.0f);
		SinceHit = std::clamp(SinceHit + dt, 0.0f, 1e6f);
		const float runTime = std::clamp(TheSettingManager->GetSettingF(section, "RunTime"), 0.1f, 60.0f);
		Draw[7] = std::clamp(TheSettingManager->GetSettingF(section, "Run"), 0.0f, 2.0f) * (1.0f - expf(-SinceHit / runTime));
		if (TheSettingManager->GetSettingI(section, "DebugView")) Draw[7] = std::clamp(TheSettingManager->GetSettingF(section, "Run"), 0.0f, 2.0f);
		Draw[0] = Amount;
		Draw[1] = Wet;
		Draw[2] = std::clamp(TheSettingManager->GetSettingF(section, "CellSize"), 0.2f, 20.0f);
		Draw[3] = Seed;
	}

	// Every geometry pass, after its shaders are bound.
	static void SetForDraw() {
		if (!Compiled) return;
		if (Draw[0] > 0.0f && ShaderSplit::CurrentContext == ShaderSplit::FirstPerson) {
			TheRenderManager->device->SetPixelShaderConstantF(218, Draw, 2);
			Set = true;
		}
		else if (Set) {
			const float off[8] = {};
			TheRenderManager->device->SetPixelShaderConstantF(218, off, 2);
			Set = false;
		}
	}
}

// UNOFFICIAL 2026-10-17 GrassFirst ([Shaders.Grass.Main] DrawFirst, live, ON by default since grassfirst3): the grass drawn BEFORE
// the land, so the land's pixel shader (parallax, 4-7 layers: the heaviest pass of an outdoor frame) is skipped by the depth test
// under the grass.
// Measured first in the frame replay (bench\deep-roadmap\evidence\grassfirst-LOG.md): noon, moving the grass before the land
// cut the land 4.03 -> 3.04 ms. Engine facts (WallSoGB's map of FalloutNV.exe + the Ghidra dump; credit WallSoGB):
//  - BSShaderAccumulator::FinishAccumulating_Standard_PreResolveDepth 0x00B65AE0 draws the main opaque list with
//    RenderBatches(1, 0x25B) at 0x00B65BED (objects and land, by pass ID) and the HDR grass later with RenderBatches(0x25C, 0x287)
//    at 0x00B65C2B.
//  - RenderBatches 0x00B63770 -> BSBatchRenderer::PrepareNextRenderPass_SetNextPass 0x00B99900 walks each renderer's sorted list
//    of pass IDs (BSSimpleList<UInt16> at renderer +0x2C; renderers at accumulator +0x174, count +0x18C) to the first ID in range
//    but then removes the list's HEAD: it assumes the ranges come in ascending order. A plain early grass call (the 'DrawFirst'
//    test of the morning, run 20261017-071126) therefore threw away one land/object ID per grass pass, and those never came back
//    (BSBatchRenderer::RegisterPass 0x00B99C90 re-inserts an ID only while its group is empty) = land and buildings gone.
//    Here the ID values inside the list are rewritten so the grass IDs come first (no node is added or freed), the grass range
//    is drawn, and the values are sorted back into ascending order; any grass ID not drawn goes back to its place and the game's
//    own later call draws it.
//  - Grass blends (no transparency multisampling, the INI default) and every pixel past its alpha test (ref 10/255) writes depth.
//    Drawn first, the tufts in the distance fade blended over the empty background and hid the land behind them: halos along
//    the far edge of each field (replay 'move'). So the grass is drawn in two parts, each tuft exactly once: NVR's grass vertex
//    shaders keep, by c19.w (GrassShaders::DrawPart), only the tufts before the fade start (fully opaque) in the early part and
//    only the fading tufts in the late part, at the game's place after the land. The late part re-issues the same RenderPass
//    objects through the engine's own BSBatchRenderer::RenderPassImmediately 0x00B994F0 (cdecl, recorded during the early call)
//    with the cull / alpha-test state of their bucket. A grass pass whose vertex shader is the game's (it cannot split) is not
//    drawn early at all: it is drawn whole at the late place, as before.
//  - Only in the world scene (not the water reflection or water depth: accumulator +0x164 / +0x165, mode +0x19C = 0), and not with
//    terrain blending on (its depth copy at the first land draw would then contain the grass).
//  - Picture check (grassfirst3, bench\deep-roadmap\evidence\grassfirst3-LOG.md): the 'far power line' and blade-edge 'speckle'
//    left after the c19 and sampler restores were the TEST, not this code: two shots next to each other in a shot list differ at
//    alpha-tested edges (the far wire 12-20/255, grass blades ~0.1% of the frame) whatever the settings (two states alternating
//    with the shot's place), and the old lists put every on shot next to its off shot. With on and off two places apart (list
//    grassfirst-p) on/off = off/off: wire max 4-8 vs 4-8, speckle +0.001 pt (d30 noon x2, night, dusk), frame -1.07 ms at d30.
namespace GrassFirst {
	static const UInt32 kMainListCallSite = 0x00B65BED, kGrassCallSite = 0x00B65C2B, kRenderBatches = 0x00B63770;
	static const UInt32 kRenderPassImmediately = 0x00B994F0, kEndPass = 0x00B988E0;
	static const UInt16 kGrassFirstId = 0x25C, kGrassLastId = 0x287;	// GRASS_DIRONLY 0x25C-0x26F, GRASS_1POINT 0x270-0x283, GRASS_SHADOW 0x284-0x287
	static const UInt16 kSplitLastId = 0x283;	// GRASS_SHADOW passes are never drawn early

	struct PassIdNode { UInt16 id; UInt16 pad; PassIdNode* next; };	// BSSimpleList<unsigned short>
	struct Record { void* pass; UInt32 passId, a3, a4, a5, cull, alphaTest; bool drawnEarly; };

	static bool Installed = false;
	static bool Recording = false;
	static void* ActiveAccumulator = nullptr;	// set by the early part, consumed by the late part of the same accumulator call
	static std::vector<Record> Records;
	static bool GameVertexShader[kGrassLastId - kGrassFirstId + 1] = {};	// learned: this grass pass binds the game's VS (no split)
	// UNOFFICIAL 2026-10-28 (tester: "i think grass shadows go away after fast traveling"): frames in a row a learned pass bound NVR's VS
	// again in the late part; at UnlearnFrames it is split (and so drawn early, with its sun shadows) again. See GrassListHook.
	static unsigned short NvrAgainFrames[kGrassLastId - kGrassFirstId + 1] = {};
	static unsigned NvrAgainLastFrame[kGrassLastId - kGrassFirstId + 1] = {};
	static const unsigned short UnlearnFrames = 30;
	static unsigned StatLearned = 0, StatUnlearned = 0;	// whole session (the first few are logged)
	static unsigned Frames = 0, StatEarly = 0, StatLate = 0, StatWhole = 0, StatUnsplit = 0;
	static unsigned RecordedFrame = ~0u;	// TheFrameRateManager->FrameNumber when Records was filled
	// UNOFFICIAL 2026-10-28 grass shadows with terrain blending on (ShadowOnlyEarly): the early call records the grass passes without
	// drawing them; GrassShadows draws them into the near sun cascade, and the late part draws them whole at the game's place.
	static bool ShadowOnly = false;
	static bool ShadowOnlyRecords = false;	// UNOFFICIAL 2026-11-04: Records came from ShadowOnlyEarly (terrain blending): GPU grass takes them in the late replay
	static unsigned ShadowOnlyFrames = 0;
	// UNOFFICIAL 2026-10-28 (tester: "i think grass shadows go away after fast traveling"; his log: after a trip every grass pass
	// went "whole after the land", none early, for the rest of the session, and grass shadows with them, since only the passes drawn
	// early go into the sun's shadow maps). A pass was learned as the game's VS from GrassShaders::LastBindNVR after its draw, which is
	// the LAST grass bind seen anywhere: a pass that bound nothing (skipped, or met mid-load) took a stale value, e.g. from last frame's
	// lamp-lit grass (GRASS23x004-007, the game's own VS), and what was learned stayed for the session. Now a pass is learned only
	// from a grass bind of this early run (EarlySerial), and everything learned is forgotten whenever the world was not drawn for a
	// while (a loading screen: fast travel, doors; a menu) or NVR's shaders were switched (RenderEffects, the grass set).
	static unsigned EarlySerial = 0;		// GrassShaders::BindSerial as this frame's early run began
	static unsigned LastEarlyFrame = 0;		// TheFrameRateManager->FrameNumber of the last early run
	static bool LastShadersOn = true;
	static unsigned StatUnknown = 0, LearnLogs = 0, ForgetLogs = 0;
	static void ForgetIfStale(GrassShaders* grass) {
		const unsigned frame = TheFrameRateManager->FrameNumber;
		const bool shadersOn = TheSettingManager->SettingsMain.Main.RenderEffects && grass->Enabled;
		const bool gap = LastEarlyFrame && frame - LastEarlyFrame > 30;	// half a second or more without the world's early grass
		if (gap || shadersOn != LastShadersOn) {
			unsigned learned = 0;
			for (bool& g : GameVertexShader) { if (g) learned++; g = false; }
			if (learned && ForgetLogs < 20) {
				ForgetLogs++;
				Logger::Log("UNOFFICIAL GrassFirst: %u grass passes learned as the game's VS are tried again (%s)", learned,
					gap ? "the world was not drawn for a while: a loading screen or a menu" : "Vegas Shaders' own shaders were switched");
			}
		}
		LastEarlyFrame = frame;
		LastShadersOn = shadersOn;
	}

	static void(__cdecl* RenderPassImmediately)(void*, UInt32, UInt32, UInt32, UInt32) = (void(__cdecl*)(void*, UInt32, UInt32, UInt32, UInt32))kRenderPassImmediately;

	static inline bool IsGrass(UInt32 id) { return id >= kGrassFirstId && id <= kGrassLastId; }

	// UNOFFICIAL 2026-10-30 RT grass ([Main.Main.RayTracing] Grass; RtCore::NoteGrass): a world grass pass just drawn: TallGrassShader's
	// instance table still holds its tufts, ScaleMask (c2), the card texture (stage 0) and the alpha reference are its own. Nothing is
	// read unless the RT core runs with Grass on. The game's grass shadow passes (GRASS_SHADOW) are left out.
	static void NoteRtGrass(void* pass, UInt32 passId) {
		if (passId > kSplitLastId || ShaderSplit::CurrentContext != ShaderSplit::World || !RtCore::GrassWanted()) return;
		NiGeometry* geometry = pass ? *(NiGeometry**)pass : nullptr;
		TallGrassShader* shader = TallGrassShader::GetShader();
		if (!geometry || !geometry->geomData || !shader || !shader->pInstanceData || !shader->ucInstanceCount) return;
		float scale[4] = {};
		if (FAILED(TheRenderManager->device->GetVertexShaderConstantF(2, scale, 1))) return;
		NiDX9RenderState* state = TheRenderManager->renderState;
		RtCore::NoteGrass(geometry, state->GetTexture(0), (const float*)shader->pInstanceData, (std::min)((UINT)shader->ucInstanceCount, (UINT)NewGrass::kTufts),
			NewGrass::GetModel(geometry).VertsPerModel, scale, state->GetRenderState(D3DRS_ALPHAREF));
	}

	// Each renderer's pass-ID list, or false if anything is not as expected (then nothing is touched).
	static bool Lists(void* accumulator, PassIdNode** lists, unsigned& count) {
		const UInt8* a = (const UInt8*)accumulator;
		count = *(const UInt32*)(a + 0x18C);
		if (count > 2) return false;	// the inline array at +0x174 holds 2
		for (unsigned i = 0; i < count; i++) {
			UInt8* renderer = *(UInt8* const*)(a + 0x174 + 4 * i);
			lists[i] = renderer ? (PassIdNode*)(renderer + 0x2C) : nullptr;
		}
		return true;
	}

	// fixes-b #29: true if any renderer's pass-ID list holds a grass ID (a grass pass is queued in this accumulator).
	static bool AnyGrass(PassIdNode* const* lists, unsigned count) {
		for (unsigned i = 0; i < count; i++)
			for (const PassIdNode* p = lists[i]; p; p = p->next)
				if (IsGrass(p->id)) return true;
		return false;
	}

	// Rewrites the values of one list in place: grass IDs first (keeping their order), or everything ascending again.
	static bool Rewrite(PassIdNode* head, bool grassFirst) {
		if (!head || (!head->next && !head->id)) return true;	// empty
		UInt16 values[1024];
		unsigned n = 0;
		for (PassIdNode* p = head; p; p = p->next) {
			if (n == 1024) return false;	// cannot happen (IDs are unique, < 0x300); then leave the list alone
			values[n++] = p->id;
		}
		if (grassFirst) std::stable_partition(values, values + n, [](UInt16 id) { return IsGrass(id); });
		else std::sort(values, values + n);
		unsigned i = 0;
		for (PassIdNode* p = head; p; p = p->next) p->id = values[i++];
		return true;
	}

	// The device state the draws after the early part must see as before it (round 2, 2026-10-17 evening): in game a far power
	// line drawn in the main list changed at its edges (up to 25/255) with DrawFirst on; the shader constants alone did not
	// explain it. Everything the grass passes set is put back: float constants (device), render states, sampler states, texture
	// stage states and textures (through the game's state cache, so its idea of the device stays right). Streams, shaders and
	// declarations are set by every pass itself. [Shaders.Grass.Main] DrawFirstRestore picks the parts (diagnostic bit mask:
	// 1 constants, 2 render states, 4 sampler states, 8 texture stage states, 16 textures; default 5). The single-shot mask tests
	// of 21:00-21:14 flipped between 4 and 15/255 at the power line for the same mask; most of that was the shot-parity effect of
	// the test (see above), but the cache-only restores did leave a real wobble (ON shots differing among themselves by 7-10/255 in
	// runs 20261017-203716 / -211133). With samplers also written to the device (mask 5) every same-parity pair is at the floor.
	static const unsigned char kRenderStates[] = { 7, 8, 9, 14, 15, 16, 19, 20, 22, 23, 24, 25, 26, 27, 28, 29, 34, 35, 36, 37, 38, 48,
		52, 53, 54, 55, 56, 57, 58, 59, 60, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 139, 140, 141, 142, 143, 145, 146, 147, 148,
		151, 152, 154, 155, 156, 157, 158, 159, 160, 161, 162, 163, 165, 166, 167, 168, 170, 171, 172, 173, 174, 175, 176, 178, 179, 180,
		181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191, 192, 193, 194, 195, 198, 199, 200, 201, 202, 203, 204, 205, 206, 207,
		208, 209 };	// the render states D3D9 defines (d3d9types.h), as the frame recorder's list
	static const unsigned char kStageStates[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 22, 23, 24, 26, 27, 28, 32 };
	struct Snapshot {
		bool constants = false;
		float vs[256][4], ps[224][4];
		DWORD rs[sizeof(kRenderStates)];
		DWORD ss[16][14];
		DWORD tss[8][sizeof(kStageStates)];
		IDirect3DBaseTexture9* tex[16];
	};
	static Snapshot Saved;
	static unsigned RestoreMask = 5;
	static unsigned StatRestored = 0;

	static void Save() {
		IDirect3DDevice9* device = TheRenderManager->device;
		NiDX9RenderState* state = TheRenderManager->renderState;
		Saved.constants = (RestoreMask & 1) && SUCCEEDED(device->GetVertexShaderConstantF(0, &Saved.vs[0][0], 256)) &&
			SUCCEEDED(device->GetPixelShaderConstantF(0, &Saved.ps[0][0], 224));
		if (RestoreMask & 2) for (unsigned i = 0; i < sizeof(kRenderStates); i++) Saved.rs[i] = state->GetRenderState((D3DRENDERSTATETYPE)kRenderStates[i]);
		if (RestoreMask & 4) for (DWORD s = 0; s < 16; s++) for (DWORD t = 1; t < 14; t++) {
			DWORD v = 0;
			device->GetSamplerState(s, (D3DSAMPLERSTATETYPE)t, &v);	// the device: some sampler states bypass the cache (SetSamplerStateHook)
			Saved.ss[s][t] = v;
		}
		if (RestoreMask & 8) for (UInt32 s = 0; s < 8; s++) for (unsigned i = 0; i < sizeof(kStageStates); i++)
			Saved.tss[s][i] = state->GetTextureStageState(s, (D3DTEXTURESTAGESTATETYPE)kStageStates[i]);
		if (RestoreMask & 16) for (UInt32 s = 0; s < 16; s++) Saved.tex[s] = state->GetTexture(s);
	}

	static void Restore() {
		IDirect3DDevice9* device = TheRenderManager->device;
		NiDX9RenderState* state = TheRenderManager->renderState;
		if (Saved.constants) {
			device->SetVertexShaderConstantF(0, &Saved.vs[0][0], 256);
			device->SetPixelShaderConstantF(0, &Saved.ps[0][0], 224);
		}
		if (RestoreMask & 2) for (unsigned i = 0; i < sizeof(kRenderStates); i++) {
			const D3DRENDERSTATETYPE rs = (D3DRENDERSTATETYPE)kRenderStates[i];
			if (state->GetRenderState(rs) != Saved.rs[i]) { state->SetRenderState(rs, Saved.rs[i], RenderStateArgs); StatRestored++; }
		}
		if (RestoreMask & 4) for (DWORD s = 0; s < 16; s++) for (DWORD t = 1; t < 14; t++) {
			DWORD v = 0;
			device->GetSamplerState(s, (D3DSAMPLERSTATETYPE)t, &v);
			if (v != Saved.ss[s][t]) {
				// Through the game's cache (so it knows) AND straight to the device: some sampler states reach the device past the
				// cache (SetSamplerStateHook), so the cache can already hold the old value and skip the write. Seen in game 21:11:
				// sampler 0's filter left by the grass (linear, not anisotropic) reached the far power line's alpha-tested draws
				// (replay: MINFILTER LINEAR or MAXANISOTROPY 1 on actions 755-818 of the noon capture = the same 14/255 edge change).
				state->SetSamplerState(s, (D3DSAMPLERSTATETYPE)t, Saved.ss[s][t], 0);
				device->SetSamplerState(s, (D3DSAMPLERSTATETYPE)t, Saved.ss[s][t]);
				StatRestored++;
			}
		}
		if (RestoreMask & 8) for (UInt32 s = 0; s < 8; s++) for (unsigned i = 0; i < sizeof(kStageStates); i++) {
			const D3DTEXTURESTAGESTATETYPE t = (D3DTEXTURESTAGESTATETYPE)kStageStates[i];
			if (state->GetTextureStageState(s, t) != Saved.tss[s][i]) { state->SetTextureStageState(s, t, Saved.tss[s][i], 0); StatRestored++; }
		}
		if (RestoreMask & 16) for (UInt32 s = 0; s < 16; s++)
			if (state->GetTexture(s) != Saved.tex[s]) { state->SetTexture(s, Saved.tex[s]); StatRestored++; }
	}

	// Round 2: whole grass draws that are all before the fade start or all in it need only one part. From the draw's world bound
	// (centre C, radius r; NiAVObject world bound of the pass's geometry) and the camera: the VS fade distance of a tuft is
	// length(clip.x, clip.y, clip.w) (REVERSED_DEPTH), which is at least its view depth w >= dot(C - cam, forward) - r, and at most
	// K x its distance <= K (|C - cam| + r), K = max(1, 1 / tan(smaller FOV / 2)). The fade start (AlphaParam.z, c5) is read from
	// the device after an early grass draw. A margin of 64 units and 3% keeps it on the safe side; misjudged 'all fading' only
	// draws that draw whole after the land (the game's own way); 'all near' is the side that must be right.
	enum { kMixed = 0, kAllNear = 1, kAllFading = 2 };
	static bool CullDraws = true;
	static float FadeStart = 0.0f, FadeSeenMin = 0.0f, FadeSeenMax = 0.0f;
	static unsigned StatAllNear = 0, StatAllFading = 0;
	static int Classify(void* pass) {
		if (!CullDraws || FadeStart <= 0.0f) return kMixed;
		NiAVObject* geometry = *(NiAVObject**)pass;
		if (!geometry) return kMixed;
		const NiBound* bound = geometry->GetWorldBound();
		if (!bound || bound == NiBound::GetGlobalWorldBound() || !(bound->Radius > 0.0f)) return kMixed;
		const D3DXVECTOR4& cam = TheRenderManager->CameraPosition;
		const D3DXVECTOR4& fwd = TheRenderManager->CameraForward;
		const float dx = bound->Center.x - cam.x, dy = bound->Center.y - cam.y, dz = bound->Center.z - cam.z;
		const float r = bound->Radius + 64.0f;
		const float depth = dx * fwd.x + dy * fwd.y + dz * fwd.z;
		if (depth - r > FadeStart * 1.03f) return kAllFading;
		// UNOFFICIAL 2026-10-18: the VS fade metric length(clip.x, clip.y, clip.w) is <= max(1, P11, P22) x distance with P11 = 2 / (R - L),
		// P22 = 2 / (T - B) of the camera frustum. FOVData assumed a vertical FOV, but the engine's FOV is the width of a 4:3 picture:
		// at FOV 90 / 16:9 that gave K = 1.03 where the top/bottom edge needs 1.13 (fading tufts drawn whole before the land = halos).
		NiCamera* camera = WorldSceneGraph ? WorldSceneGraph->camera : nullptr;
		if (!camera || camera->Frustum.Ortho) return kMixed;
		const float fw = camera->Frustum.Right - camera->Frustum.Left, fh = camera->Frustum.Top - camera->Frustum.Bottom;
		if (!(fw > 1e-4f && fh > 1e-4f)) return kMixed;
		const float k = (std::max)(1.0f, (std::max)(2.0f / fw, 2.0f / fh)) * 1.03f;
		if (k * (sqrtf(dx * dx + dy * dy + dz * dz) + r) <= FadeStart) return kAllNear;
		return kMixed;
	}

	// UNOFFICIAL 2026-10-18 NewGrass: how far the nearest point of a draw's world bound is from the camera (0 if unknown).
	static float NearestDistance(void* pass) {
		NiAVObject* geometry = *(NiAVObject**)pass;
		if (!geometry) return 0.0f;
		const NiBound* bound = geometry->GetWorldBound();
		if (!bound || bound == NiBound::GetGlobalWorldBound() || !(bound->Radius > 0.0f)) return 0.0f;
		const D3DXVECTOR4& cam = TheRenderManager->CameraPosition;
		const float dx = bound->Center.x - cam.x, dy = bound->Center.y - cam.y, dz = bound->Center.z - cam.z;
		const float d = sqrtf(dx * dx + dy * dy + dz * dz) - bound->Radius;
		return d > 0.0f ? d : 0.0f;
	}

	static bool Allowed(void* accumulator) {
		GrassShaders* grass = TheShaderManager->Shaders.Grass;
		if (!Installed || !grass || !grass->Enabled || !grass->DrawFirst) return false;
		if (ShaderSplit::CurrentContext != ShaderSplit::World) return false;
		const UInt8* a = (const UInt8*)accumulator;
		if (*(const UInt32*)(a + 0x19C) != 0 || a[0x164] || a[0x165]) return false;
		TerrainShaders* terrain = TheShaderManager->Shaders.Terrain;
		if (terrain && terrain->BlendingSettings.Enabled) {
			static bool told = false;
			if (!told) { told = true; Logger::Log("UNOFFICIAL GrassFirst: off while terrain blending is on (its depth copy would contain the grass); the grass still casts its sun shadows (drawn into the shadow map only, before the land)"); }
			return false;
		}
		return true;
	}

	// UNOFFICIAL 2026-10-28 (user: a tester "not getting grass shadows", with terrain blending on): where GrassFirst stands aside for
	// terrain blending, the grass is still put into the near sun cascade before the land (ShadowOnlyEarly). Nothing reaches the
	// screen or its depth there, so the blend's depth copy at the first land draw is as it was.
	static bool ShadowOnlyAllowed(void* accumulator) {
		GrassShaders* grass = TheShaderManager->Shaders.Grass;
		if (!Installed || !grass || !grass->Enabled || !grass->DrawFirst || !GrassShadows::Enabled) return false;
		if (ShaderSplit::CurrentContext != ShaderSplit::World || !TheShaderManager->GameState.isExterior) return false;
		const UInt8* a = (const UInt8*)accumulator;
		if (*(const UInt32*)(a + 0x19C) != 0 || a[0x164] || a[0x165]) return false;
		TerrainShaders* terrain = TheShaderManager->Shaders.Terrain;
		return terrain && terrain->BlendingSettings.Enabled;
	}

	// UNOFFICIAL 2026-10-26 LampsDepthFirst (core/OnePassDepth.h): the main list's one many-lamp (OPT) passes first, depth only, then
	// shaded once per visible pixel; then the rest of the list as the game does it.
	static bool RewriteOptFirst(PassIdNode* head) {
		if (!head || (!head->next && !head->id)) return true;	// empty
		UInt16 values[1024];
		unsigned n = 0;
		for (PassIdNode* p = head; p; p = p->next) {
			if (n == 1024) return false;
			values[n++] = p->id;
		}
		std::stable_partition(values, values + n, [](UInt16 id) { return OnePassDepth::IsOpt(id); });
		unsigned i = 0;
		for (PassIdNode* p = head; p; p = p->next) p->id = values[i++];
		return true;
	}

	// Is an OPT ID queued, and is every queued OPT pass an object's first pass? (BSBatchRenderer: pass groups at +8, 0x68 each, found
	// through the NiTMap at +0x0C; 5 buckets of 0x14 with the head at +4; a node holds the RenderPass at [2]; RenderPass +7 = first pass.)
	static int OptQueued(void* accumulator) {
		const UInt8* a = (const UInt8*)accumulator;
		const UInt32 count = *(const UInt32*)(a + 0x18C);
		bool any = false;
		for (UInt32 r = 0; r < count && r < 2; r++) {
			UInt8* renderer = *(UInt8* const*)(a + 0x174 + 4 * r);
			if (!renderer) continue;
			PassIdNode* list = (PassIdNode*)(renderer + 0x2C);
			if (!list->next && !list->id) continue;
			for (PassIdNode* p = list; p; p = p->next) {
				if (!OnePassDepth::IsOpt(p->id)) continue;
				UInt32 index = 0;
				if (!(UInt8)ThisCall(0x00853130, renderer + 0x0C, (UInt32)p->id, &index)) continue;	// NiTMapBase<UInt32, UInt32>::GetAt
				const UInt8* passGroup = *(UInt8**)(renderer + 8) + index * 0x68;
				for (unsigned b = 0; b < 5; b++)
					for (const UInt32* node = *(const UInt32* const*)(passGroup + b * 0x14 + 4); node; node = (const UInt32*)node[0]) {
						const UInt8* pass = (const UInt8*)node[2];
						if (!pass) continue;
						if (!pass[7]) return -1;	// an add-on OPT pass: its object's first pass may come later in the list
						any = true;
					}
			}
		}
		return any ? 1 : 0;
	}

	static void DepthFirst(void* accumulator, UInt32 arg3, SInt32 group) {
		if (!OnePassDepth::Mode || !OnePassDepth::HookSeen || ShaderSplit::CurrentContext != ShaderSplit::World) return;
		const UInt8* a = (const UInt8*)accumulator;
		if (*(const UInt32*)(a + 0x19C) != 0 || a[0x164] || a[0x165]) return;	// standard render mode, not the water passes
		if (!OnePassDepth::MakeShader(TheRenderManager->device)) return;
		const int queued = OptQueued(accumulator);
		if (queued < 0 && GpuTimer::Enabled) OnePassDepth::StatBailNotFirst += 1;
		if (queued <= 0) return;
		PassIdNode* lists[2] = {};
		unsigned count = 0;
		if (!Lists(accumulator, lists, count)) return;
		const double start = GpuTimer::Enabled ? CpuTimer::NowMs() : 0.0;
		bool ok = true;
		for (unsigned i = 0; ok && i < count; i++) ok = RewriteOptFirst(lists[i]);
		if (ok) {
			const unsigned mask = RestoreMask;
			RestoreMask = 31;	// everything the passes set is put back for the rest of the list
			Save();
			OnePassDepth::Records.clear();
			// Step 1: every OPT pass recorded; the eligible ones drawn depth-only (RenderPassImmediatelyHook -> DepthFirstPass).
			OnePassDepth::Recording = true;
			ThisCall(kRenderBatches, accumulator, 0xC3u, 0xD6u, arg3, group);
			OnePassDepth::Recording = false;
			((void(__cdecl*)(char, int))0x00B98540)(0, 0);	// BSRenderState::SetAlphaToCoverageEnable(0, 0), as after a bucket
			((void(__cdecl*)())kEndPass)();
			// Step 2: the same passes with the game's shaders, in the recorded order.
			NiDX9RenderState* state = TheRenderManager->renderState;
			for (const OnePassDepth::Record& r : OnePassDepth::Records) {
				state->SetRenderState(D3DRS_CULLMODE, r.cull, RenderStateArgs);
				state->SetRenderState(D3DRS_ALPHATESTENABLE, r.alphaTest, RenderStateArgs);
				ReflMaterial::NeutralScope neutral(r.pass, r.pass ? ((const PointShadowForward::RenderPassView*)r.pass)->geometry : nullptr, ShaderSplit::CurrentContext == ShaderSplit::World, ShaderSplit::CurrentContext == ShaderSplit::FirstPerson, TheRenderManager->device);	// (perf-mirrorfix)
				RenderPassImmediately(r.pass, r.passId, r.a3, r.a4, r.a5);
			}
			((void(__cdecl*)(char, int))0x00B98540)(0, 0);
			((void(__cdecl*)())kEndPass)();
			Restore();
			RestoreMask = mask;
			if (GpuTimer::Enabled) {
				OnePassDepth::StatLists += 1;
				for (const OnePassDepth::Record& r : OnePassDepth::Records) if (r.depth) OnePassDepth::StatDepth += 1;
			}
			OnePassDepth::Records.clear();
		}
		for (unsigned i = 0; i < count; i++) Rewrite(lists[i], false);	// ascending again, whatever happened
		if (GpuTimer::Enabled) OnePassDepth::StatCpuMs += CpuTimer::NowMs() - start;
	}

	// Step 1 of LampsDepthFirst: one OPT pass met by the early call: recorded, and drawn depth-only if it may be.
	static void DepthFirstPass(void* pass, UInt32 passId, UInt32 a3, UInt32 a4, UInt32 a5) {
		NiDX9RenderState* state = TheRenderManager->renderState;
		OnePassDepth::Record r = { pass, passId, a3, a4, a5, state->GetRenderState(D3DRS_CULLMODE), state->GetRenderState(D3DRS_ALPHATESTENABLE), false };
		if (OnePassDepth::Eligible(pass, r.alphaTest, state->GetRenderState(D3DRS_ALPHABLENDENABLE))) {
			OnePassDepth::InDepth = true;
			OnePassDepth::DepthDrawn = false;
			RenderPassImmediately(pass, passId, a3, a4, a5);
			OnePassDepth::InDepth = false;
			r.depth = OnePassDepth::DepthDrawn;
		}
		OnePassDepth::Records.push_back(r);
	}

	// GrassShadows::Records (the passes noted for this frame) into the near sun cascade, every tuft (c19.w = 0); 2026-10-28 also into
	// the middle and far cascades for the tufts within CastShadowsReach (GrassShadows::Begin decides which cascades need them).
	static void DrawGrassShadows() {
		static GpuTimer grassShadowTimer("Grass shadows");
		for (int cascade = ShadowsExteriorEffect::MapNear; cascade <= ShadowsExteriorEffect::MapFar; cascade++) {
			if (!GrassShadows::Begin(TheRenderManager->device, cascade)) {
				if (cascade == ShadowsExteriorEffect::MapNear) break;	// off, indoors, no sun maps, not PCF: none of them
				continue;
			}
			GpuProfileScope gpu(grassShadowTimer, TheRenderManager->device);
			GrassShaders::DrawPart = 0;
			((void(__cdecl*)())kEndPass)();	// the first pass binds again (SetShadersHook: c19 with w = 0)
			for (const GrassShadows::Record& r : GrassShadows::Records) {
				if (!GrassShadows::InNear(r.pass)) { GrassShadows::StatOutside++; continue; }
				RenderPassImmediately(r.pass, r.passId, r.a3, r.a4, r.a5);
				GrassShadows::StatPasses++;
			}
			((void(__cdecl*)(char, int))0x00B98540)(0, 0);	// BSRenderState::SetAlphaToCoverageEnable(0, 0), as after a bucket
			((void(__cdecl*)())kEndPass)();
			GrassShadows::End(TheRenderManager->device);
		}
		// UNOFFICIAL 2026-11-04 the same passes into the flashlight's shadow map (drawn this frame, before the world), for the light pass
		// at the main list's end and the grass drawn after this
		if (IDirect3DTexture9* map = FlashlightDeferred::ShadowMap()) {
			D3DXVECTOR4 light[3];
			if (FlashlightDeferred::LightParams(light) && GrassShadows::BeginSpot(TheRenderManager->device, map, TheShaderManager->SpotLightWorldToLightMatrix[0], light[0])) {
				GpuProfileScope gpu(grassShadowTimer, TheRenderManager->device);
				GrassShaders::DrawPart = 0;
				((void(__cdecl*)())kEndPass)();
				// (the cards the GPU grass strands stand in for stay out: the strands cast the flashlight's shadow themselves, GpuGrass.cpp)
				const float strandReach = GpuGrass::FlashlightCasterReach();
				for (const GrassShadows::Record& r : GrassShadows::Records) {
					if (!GrassShadows::InNear(r.pass)) { GrassShadows::StatOutside++; continue; }
					if (strandReach > 0.0f && NearestDistance(r.pass) <= strandReach) { GrassShadows::StatSkipped++; continue; }
					RenderPassImmediately(r.pass, r.passId, r.a3, r.a4, r.a5);
					GrassShadows::StatPasses++;
				}
				((void(__cdecl*)(char, int))0x00B98540)(0, 0);
				((void(__cdecl*)())kEndPass)();
				GrassShadows::End(TheRenderManager->device);
			}
			GpuGrass::DrawFlashlightCasters(TheRenderManager->device);	// and the GPU grass strands, before the light pass reads the map
		}
		GrassShadows::Records.clear();
	}

	// UNOFFICIAL 2026-10-28 terrain blending on: the grass range is taken out of the lists before the land as GrassFirst does it, but
	// every pass is only recorded (RenderPassImmediatelyHook, ShadowOnly), drawn into the near sun cascade, and then drawn whole by
	// the late part at the game's place after the land (GrassListHook: drawnEarly false = part 0, every tuft, with the cull and alpha
	// test state of its bucket), as the passes GrassFirst does not split always were. The game's grass call then finds nothing left.
	static void ShadowOnlyEarly(void* accumulator, UInt32 arg3, SInt32 group) {
		PassIdNode* lists[2] = {};
		unsigned count = 0;
		bool ok = Lists(accumulator, lists, count);
		if (!ok || !AnyGrass(lists, count)) return;
		for (unsigned i = 0; ok && i < count; i++) ok = Rewrite(lists[i], true);
		if (ok) {
			Records.clear();
			GrassShadows::Records.clear();
			RestoreMask = (unsigned)TheShaderManager->Shaders.Grass->DrawFirstRestore;
			Save();
			GrassShaders::DrawPart = 0;
			Recording = ShadowOnly = true;
			ThisCall(kRenderBatches, accumulator, (UInt32)kGrassFirstId, (UInt32)kGrassLastId, arg3, group);
			Recording = ShadowOnly = false;
			for (const Record& r : Records) GrassShadows::Note(r.pass, r.passId, r.a3, r.a4, r.a5);	// the caster draw skips any not on NVR's VS
			const size_t recorded = Records.size();
			DrawGrassShadows();
			GrassShaders::DrawPart = 0;
			((void(__cdecl*)())kEndPass)();
			Restore();
			ActiveAccumulator = accumulator;
			RecordedFrame = TheFrameRateManager->FrameNumber;
			ShadowOnlyRecords = true;
			if (++ShadowOnlyFrames <= 3 || ShadowOnlyFrames % 3600 == 0)
				Logger::Log("UNOFFICIAL grass shadows (terrain blending on): frame %u: %u grass passes taken before the land into the sun shadow map only, drawn on screen after the land as the game does",
					ShadowOnlyFrames, (unsigned)recorded);
		}
		for (unsigned i = 0; i < count; i++) Rewrite(lists[i], false);	// ascending again, whatever happened
	}

	// The game's RenderBatches(1, 0x25B) call: the near grass first, then the main list as the game does it.
	static void __fastcall MainListHook(void* accumulator, void* edx, UInt32 first, UInt32 last, UInt32 arg3, SInt32 group) {
		// UNOFFICIAL 2026-10-30 ReactiveMask (core/ReactiveMask.h): the colour as the main opaque list leaves it. Declared first so it runs
		// last, after LinearScene has encoded the target back (its span ends at this function's end too).
		struct ReactiveSpan { bool on; ReactiveSpan(bool main) : on(main && ShaderSplit::CurrentContext == ShaderSplit::World) {}
			~ReactiveSpan() { if (on) ReactiveMask::NoteOpaque(TheRenderManager->device); } } reactiveSpan(first == 1 && last == 0x25B && group < 0);
#if NVR_TEST_TOOLS
		struct CensusMain { bool was; CensusMain() : was(LinearSceneCensus::InMain) { LinearSceneCensus::InMain = true; } ~CensusMain() { LinearSceneCensus::InMain = was; } } censusMain;	// UNOFFICIAL 2026-10-28, diagnostic
#endif
		// UNOFFICIAL 2026-10-28 LinearScene: the world's main list in linear light, left encoded at its end (core/LinearScene.h).
		struct LinearSpan { bool on; LinearSpan() : on(ShaderSplit::CurrentContext == ShaderSplit::World) { if (on) LinearScene::BeginSpan(TheRenderManager->device); }
			~LinearSpan() { if (on) LinearScene::EndSpan(TheRenderManager->device); } } linearSpan;
		// UNOFFICIAL 2026-10-31 DeferredLamps (core/DeferredLamps.h): declared between LinearSpan and the CaptureSpans, so its end runs after
		// CapturedNormals::EndSpan (the G-buffer resolved) and before LinearScene::EndSpan (the encode): the lamps of the deferred pixels.
		struct DeferredSpan { bool on; DeferredSpan() : on(ShaderSplit::CurrentContext == ShaderSplit::World) {}
			~DeferredSpan() { if (on) { DeferredLamps::Apply(TheRenderManager->device); FlashlightDeferred::Apply(TheRenderManager->device); } } } deferredSpan;	// UNOFFICIAL 2026-11-04 + the deferred flashlight
		ActiveAccumulator = nullptr;
		// UNOFFICIAL 2026-10-28 CapturedNormals (core/CapturedNormals.h): the normal target bound around the game's own call only (the early
		// grass part above it draws grass and the grass shadows, which write none).
		struct CaptureSpan { bool on; CaptureSpan() : on(ShaderSplit::CurrentContext == ShaderSplit::World) { if (on) CapturedNormals::BeginSpan(TheRenderManager->device); }
			~CaptureSpan() { if (on) CapturedNormals::EndSpan(TheRenderManager->device); } };
		if (first == 1 && last == 0x25B && group < 0) DepthFirst(accumulator, arg3, group);	// UNOFFICIAL 2026-10-26 LampsDepthFirst
		if (first == 1 && last == 0x25B && group < 0 && Allowed(accumulator)) {
			PassIdNode* lists[2] = {};
			unsigned count = 0;
			bool ok = Lists(accumulator, lists, count);
			// fixes-b #29: no grass pass queued in this accumulator (interiors, bare land): nothing to draw first, so no device
			// work at all (the state save / restore reads back ~500 constants and ~250 states) -- the game's own calls run as is.
			if (ok && !AnyGrass(lists, count)) { CaptureSpan capture; ThisCall(kRenderBatches, accumulator, first, last, arg3, group); return; }
			for (unsigned i = 0; ok && i < count; i++) ok = Rewrite(lists[i], true);
			if (ok) {
				ForgetIfStale(TheShaderManager->Shaders.Grass);	// UNOFFICIAL 2026-10-28
				Records.clear();
				GrassShadows::Records.clear();
				// The draws after this early part see the device state they saw before it (Save / Restore above).
				RestoreMask = (unsigned)TheShaderManager->Shaders.Grass->DrawFirstRestore;
				CullDraws = TheShaderManager->Shaders.Grass->DrawFirstCull;
				Save();
				GrassShaders::DrawPart = 1;
				Recording = true;
				// UNOFFICIAL 2026-10-27 GPU grass (effects/GpuGrass.h): when it takes this call, NewGrass stays out of it (both hook the same draw call)
				const bool gpuGrass = GpuGrass::BeginEarly(TheRenderManager->device, TheShaderManager->Shaders.Grass->Enabled);
				if (!gpuGrass) NewGrass::BeginEarly(TheRenderManager->device, TheShaderManager->Shaders.Grass->Enabled);	// UNOFFICIAL 2026-10-18 NewGrass (core/NewGrass.h)
				EarlySerial = GrassShaders::BindSerial;	// UNOFFICIAL 2026-10-28: only this run's grass binds teach RenderPassImmediatelyHook
				ThisCall(kRenderBatches, accumulator, (UInt32)kGrassFirstId, (UInt32)kGrassLastId, arg3, group);
				if (!gpuGrass) NewGrass::EndEarly(TheRenderManager->device, StatEarly);
				GpuGrass::EndEarly(TheRenderManager->device);	// the strands: one cull pass + two indirect draws, before the land
				Recording = false;
				// UNOFFICIAL 2026-10-27 grass sun shadows (core/GrassShadows.h): the early grass passes drawn with NVR's grass VS, drawn again
				// into the near sun cascade (every tuft, c19.w = 0), before the land and everything after it read the shadow atlas.
				DrawGrassShadows();
				GrassShaders::DrawPart = 0;
				((void(__cdecl*)())kEndPass)();
				Restore();
				ActiveAccumulator = accumulator;
				RecordedFrame = TheFrameRateManager->FrameNumber;	// fixes-b #29: the late part replays these pass pointers in this frame only
				ShadowOnlyRecords = false;
			}
			for (unsigned i = 0; i < count; i++) Rewrite(lists[i], false);	// ascending again, whatever happened
		}
		else if (first == 1 && last == 0x25B && group < 0 && ShadowOnlyAllowed(accumulator)) ShadowOnlyEarly(accumulator, arg3, group);	// UNOFFICIAL 2026-10-28
		CaptureSpan capture;
		ThisCall(kRenderBatches, accumulator, first, last, arg3, group);
	}

	// The game's RenderBatches(0x25C, 0x287) call: the fading tufts of the grass drawn early, and whole the grass passes that were
	// not drawn early, in their recorded order; then the game's call (which normally finds nothing left).
	static void __fastcall GrassListHook(void* accumulator, void* edx, UInt32 first, UInt32 last, UInt32 arg3, SInt32 group) {
		if (ActiveAccumulator && ActiveAccumulator == accumulator && first == kGrassFirstId && last == kGrassLastId &&
			RecordedFrame != TheFrameRateManager->FrameNumber) {
			// fixes-b #29: records from an earlier frame (the pass objects may be gone): never replayed
			ActiveAccumulator = nullptr;
			Records.clear();
		}
		if (ActiveAccumulator && ActiveAccumulator == accumulator && first == kGrassFirstId && last == kGrassLastId) {
			ActiveAccumulator = nullptr;
			NiDX9RenderState* state = TheRenderManager->renderState;
			((void(__cdecl*)())kEndPass)();
			// UNOFFICIAL 2026-11-04 GPU grass with terrain blending on (user: "make gpu grass work with terrain blending"): no grass is
			// drawn before the land then (Allowed), so the card draws are caught here, in this replay after the land, as the early part
			// catches them otherwise. A caught pass is drawn whole (c19.w = 0) and its cards skipped; one with fading tufts too (not all
			// near, Classify) is drawn again with only those (c19.w = 2), as the early part leaves them to this replay. The strands are drawn
			// right after the grass (EndEarly), the device state put back around them as around the early part.
			const bool gpuGrass = ShadowOnlyRecords && !Records.empty() &&
				GpuGrass::BeginEarly(TheRenderManager->device, TheShaderManager->Shaders.Grass->Enabled);
			ShadowOnlyRecords = false;
			int part = -1;
			for (const Record& r : Records) {
				const int want = r.drawnEarly ? 2 : 0;
				if (want != part) { ((void(__cdecl*)())kEndPass)(); part = want; GrassShaders::DrawPart = want; }	// re-bind: c19.w
				state->SetRenderState(D3DRS_CULLMODE, r.cull, RenderStateArgs);
				state->SetRenderState(D3DRS_ALPHATESTENABLE, r.alphaTest, RenderStateArgs);
				int kind = kMixed;
				const bool arm = gpuGrass && !r.drawnEarly && r.passId <= kSplitLastId && !GameVertexShader[r.passId - kGrassFirstId] &&
					(kind = Classify(r.pass)) != kAllFading;
				if (arm) GpuGrass::Arm(r.pass, kind == kAllNear ? 0 : 1, NearestDistance(r.pass));
				// fixes-b #28: alpha-to-coverage (transparency multisampling) needs no restore here: BSBatchRenderer::RenderPassImmediately
				// 0x00B994F0 itself calls BSRenderState::SetAlphaToCoverageEnable(pass wants it, 0) for every pass when the game's
				// TMS flag (0x011F9421) is set (Ghidra dump of the exe, psycho-master graphics_fnv_atmosphere_alpha_coverage_state_followup).
				ReflMaterial::NeutralScope neutral(r.pass, r.pass ? ((const PointShadowForward::RenderPassView*)r.pass)->geometry : nullptr, ShaderSplit::CurrentContext == ShaderSplit::World, ShaderSplit::CurrentContext == ShaderSplit::FirstPerson, TheRenderManager->device);	// (perf-mirrorfix: the grass replay's own alpha)
				RenderPassImmediately(r.pass, r.passId, r.a3, r.a4, r.a5);
				NoteRtGrass(r.pass, r.passId);	// UNOFFICIAL 2026-10-30 RT grass (a pass drawn early is noted once: RtCore skips the repeat)
				if (gpuGrass) {
					const bool took = arm && GpuGrass::Took();
					GpuGrass::Disarm();
					float alpha[4] = {};	// AlphaParam (c5) of NVR's grass VS: z = the fade start, for Classify (learned early otherwise)
					if (GrassShaders::LastBindNVR && SUCCEEDED(TheRenderManager->device->GetVertexShaderConstantF(5, alpha, 1)) && alpha[2] > 0.0f)
						FadeStart = alpha[2];
					if (took && kind != kAllNear && !GpuGrass::KeepsCards() && GrassShaders::LastBindNVR) {
						((void(__cdecl*)())kEndPass)();
						GrassShaders::DrawPart = 2;
						RenderPassImmediately(r.pass, r.passId, r.a3, r.a4, r.a5);
						((void(__cdecl*)())kEndPass)();
						GrassShaders::DrawPart = part;
					}
				}
				if (r.drawnEarly) StatLate++; else StatWhole++;
				// UNOFFICIAL 2026-10-28 (tester: grass shadows gone after a fast travel; log: from then on every grass draw "whole after
				// it", none early): right after the load a grass pass was drawn with the game's vertex shader, which was learned for the
				// session, so the grass was never split again and never cast its shadows. A learned pass that binds NVR's VS here for
				// UnlearnFrames frames running (SetForDraw sets LastBindNVR as it binds) is split again from the next frame.
				if (!r.drawnEarly && r.passId <= kSplitLastId && GameVertexShader[r.passId - kGrassFirstId]) {
					const unsigned i = r.passId - kGrassFirstId;
					const unsigned frame = TheFrameRateManager->FrameNumber;
					if (!GrassShaders::LastBindNVR) NvrAgainFrames[i] = 0;
					else if (NvrAgainLastFrame[i] != frame) {
						NvrAgainLastFrame[i] = frame;
						if (++NvrAgainFrames[i] >= UnlearnFrames) {
							GameVertexShader[i] = false;
							NvrAgainFrames[i] = 0;
							if (++StatUnlearned <= 10)
								Logger::Log("UNOFFICIAL GrassFirst: grass pass %03X binds Vegas Shaders' vertex shader again (%u frames running): split and drawn before the land again (with its sun shadows)",
									r.passId, (unsigned)UnlearnFrames);
						}
					}
				}
			}
			((void(__cdecl*)(char, int))0x00B98540)(0, 0);	// BSRenderState::SetAlphaToCoverageEnable(0, 0), as RenderNextRenderPass after a bucket
			((void(__cdecl*)())kEndPass)();
			GrassShaders::DrawPart = 0;
			if (gpuGrass) {	// UNOFFICIAL 2026-11-04 the strands of the passes caught above (terrain blending on)
				RestoreMask = (unsigned)TheShaderManager->Shaders.Grass->DrawFirstRestore;
				Save();
				GpuGrass::EndEarly(TheRenderManager->device);
				Restore();
			}
			// What the game's grass call leaves in c19 without GrassFirst: the last grass bind's value, w = 0.
			if (!Records.empty()) {
				const float leave[4] = { GrassShaders::LastPairing[0], GrassShaders::LastPairing[1], GrassShaders::LastPairing[2], 0.0f };
				TheRenderManager->device->SetVertexShaderConstantF(19, leave, 1);
			}
			Records.clear();
			if (++Frames <= 3 || (Frames % 3600 == 0 && Frames <= 3600 * 17))	// UNOFFICIAL fixes-b #53: at most 20 lines per session
				Logger::Log("UNOFFICIAL GrassFirst: frame %u: %u grass draws before the land (%u of them all near: not again), %u after it (fading tufts), %u whole after it (%u all fading, the rest no split), %u drawn whole early (game VS, learned), %u met before any grass bind of the run; fade start %.1f (seen %.1f-%.1f); %u states put back (mask %u)",
					Frames, StatEarly, StatAllNear, StatLate, StatWhole, StatAllFading, StatUnsplit, StatUnknown, FadeStart, FadeSeenMin, FadeSeenMax, StatRestored, RestoreMask);
			StatEarly = StatLate = StatWhole = StatUnsplit = StatAllNear = StatAllFading = StatRestored = StatUnknown = 0;
			FadeSeenMin = FadeSeenMax = 0.0f;
		}
		ThisCall(kRenderBatches, accumulator, first, last, arg3, group);
	}

	// BSBatchRenderer::RenderPassImmediately: while the early grass call runs, each grass pass is recorded, and drawn now (near
	// tufts) only if its vertex shader is NVR's.
	static void __cdecl RenderPassImmediatelyHook(void* pass, UInt32 passId, UInt32 a3, UInt32 a4, UInt32 a5) {
		// UNOFFICIAL 2026-10-28 (native D3D9): the runtime puts its own device-table entries back; the main list's draw hooks are put in
		// again at every pass (in the order they were first put in: LinearScene, then CapturedNormals).
		if (LinearScene::InSpan) LinearScene::EnsureHooked(TheRenderManager->device);
		if (CapturedNormals::InSpan) CapturedNormals::EnsureHooked(TheRenderManager->device);
		if (OnePassDepth::Recording && OnePassDepth::IsOpt(passId)) { DepthFirstPass(pass, passId, a3, a4, a5); return; }	// UNOFFICIAL LampsDepthFirst
		// UNOFFICIAL 2026-11-07 perf-mirrorfix: every world / first-person pass writes its own alpha (core/ReflMaterial.h; grass comes only through here)
		ReflMaterial::NeutralScope neutral(pass, pass ? ((const PointShadowForward::RenderPassView*)pass)->geometry : nullptr, ShaderSplit::CurrentContext == ShaderSplit::World, ShaderSplit::CurrentContext == ShaderSplit::FirstPerson, TheRenderManager->device);
		if (!Recording || !IsGrass(passId)) {
			RenderPassImmediately(pass, passId, a3, a4, a5);
			if (IsGrass(passId)) NoteRtGrass(pass, passId);	// UNOFFICIAL 2026-10-30 RT grass (GrassFirst off: the game's own grass call)
			return;
		}
		NiDX9RenderState* state = TheRenderManager->renderState;
		Record r = { pass, passId, a3, a4, a5, state->GetRenderState(D3DRS_CULLMODE), state->GetRenderState(D3DRS_ALPHATESTENABLE), false };
		if (ShadowOnly) { Records.push_back(r); return; }	// UNOFFICIAL 2026-10-28 terrain blending on: not drawn now (ShadowOnlyEarly)
		bool& gameVS = GameVertexShader[passId - kGrassFirstId];
		if (passId <= kSplitLastId && !gameVS) {
			const int kind = Classify(pass);
			if (kind == kAllFading) { StatAllFading++; Records.push_back(r); return; }	// drawn whole after the land (as the game)
			if (kind == kAllNear) {	// every tuft, now (c19.w = 0): if the bound test were ever wrong, a tuft is drawn early, never lost
				GrassShaders::DrawPart = 0;
				const float all[4] = { GrassShaders::LastPairing[0], GrassShaders::LastPairing[1], GrassShaders::LastPairing[2], 0.0f };
				TheRenderManager->device->SetVertexShaderConstantF(19, all, 1);
			}
			// UNOFFICIAL 2026-10-18 NewGrass: this pass's card draw may be replaced by blades (its first DrawIndexedPrimitive, core/NewGrass.h).
			NewGrass::Arm(pass, kind == kAllNear ? 0 : 1, NearestDistance(pass));
			GpuGrass::Arm(pass, kind == kAllNear ? 0 : 1, NearestDistance(pass));	// UNOFFICIAL 2026-10-27 GPU grass (does nothing unless it took this call)
			RenderPassImmediately(pass, passId, a3, a4, a5);
			NoteRtGrass(pass, passId);	// UNOFFICIAL 2026-10-30 RT grass
			NewGrass::Disarm();
			GpuGrass::Disarm();
			if (kind == kAllNear) {
				GrassShaders::DrawPart = 1;
				const float nearOnly[4] = { GrassShaders::LastPairing[0], GrassShaders::LastPairing[1], GrassShaders::LastPairing[2], 1.0f };
				TheRenderManager->device->SetVertexShaderConstantF(19, nearOnly, 1);
			}
			if (GrassShaders::BindSerial == EarlySerial) {
				// UNOFFICIAL 2026-10-28: no grass bind in this run yet (nothing drawn: a pass skipped, or met mid-load): nothing is
				// learned or noted from a stale bind; the pass is drawn whole after the land, as the game draws it.
				StatUnknown++;
				r.drawnEarly = false;
			}
			else if (GrassShaders::LastBindNVR) {
				GrassShadows::Note(pass, passId, a3, a4, a5);	// UNOFFICIAL 2026-10-27 grass sun shadows: drawn again into the near cascade (MainListHook)
				float alpha[4] = {};	// AlphaParam (c5) of NVR's grass VS: z = fade start, for the next draws' Classify
				if (SUCCEEDED(TheRenderManager->device->GetVertexShaderConstantF(5, alpha, 1)) && alpha[2] > 0.0f) {
					FadeStart = alpha[2];
					FadeSeenMin = FadeSeenMin > 0.0f ? (std::min)(FadeSeenMin, alpha[2]) : alpha[2];
					FadeSeenMax = (std::max)(FadeSeenMax, alpha[2]);
				}
				StatEarly++;
				if (kind == kAllNear) { StatAllNear++; return; }	// every tuft drawn now: no late part
				r.drawnEarly = true;
			}
			else {	// drawn whole with the game's VS (first time only): not again (until ForgetIfStale)
				gameVS = true;
				StatUnsplit++;
				NvrAgainFrames[passId - kGrassFirstId] = 0;	// UNOFFICIAL 2026-10-28: until it binds NVR's VS again (GrassListHook)
				if (LearnLogs < 40) {
					LearnLogs++;
					Logger::Log("UNOFFICIAL GrassFirst: grass pass %03X draws with the game's vertex shader %s: drawn whole after the land from now on (no grass shadows from it) until the next loading screen",
						passId, GrassShaders::LastBindName ? GrassShaders::LastBindName : "?");
				}
				return;
			}
		}
		Records.push_back(r);
	}
}

void InstallGrassFirstHooks() {
	auto isCall = [](UInt32 site) { return *(UInt8*)site == 0xE8 && site + 5 + *(SInt32*)(site + 1) == GrassFirst::kRenderBatches; };
	if (!GrassFirst::Installed) {
		Logger::Log("UNOFFICIAL GrassFirst: the render pass hook is not in (another plugin?); [Shaders.Grass.Main] DrawFirst does nothing");
		return;
	}
	if (isCall(GrassFirst::kMainListCallSite) && isCall(GrassFirst::kGrassCallSite)) {
		WriteRelCall(GrassFirst::kMainListCallSite, (UInt32)&GrassFirst::MainListHook);
		WriteRelCall(GrassFirst::kGrassCallSite, (UInt32)&GrassFirst::GrassListHook);
		Logger::Log("UNOFFICIAL GrassFirst: hooked (inactive unless [Shaders.Grass.Main] DrawFirst = true)");
	}
	else {
		GrassFirst::Installed = false;
		Logger::Log("UNOFFICIAL GrassFirst: the calls at %08X / %08X are not the game's (another plugin?); not hooked", GrassFirst::kMainListCallSite, GrassFirst::kGrassCallSite);
	}
}

// Inside AttachHooks' Detours transaction.
void AttachGrassFirstRenderPassHook() {
	const UInt8* code = (const UInt8*)GrassFirst::kRenderPassImmediately;
	if (code[0] == 0x56 && code[1] == 0x8B && code[2] == 0x74 && code[3] == 0x24 && code[4] == 0x08)	// PUSH ESI; MOV ESI,[ESP+8]
		GrassFirst::Installed = DetourAttach(&(PVOID&)GrassFirst::RenderPassImmediately, &GrassFirst::RenderPassImmediatelyHook) == NO_ERROR;
}

// UNOFFICIAL 2026-10-27 / 2026-10-28 [Shaders.PBR.Main] GameShaderEyes: an eyeball's draw uses the game's own shaders (see SetShadersHook).
// Per draw on the device only: Before (draw hooks) notes an eye draw and puts the bound pass's game shaders on the device; OnBind does it
// again when the game binds inside that draw; After puts NVR's back. Off, or not an eye: nothing changes.
namespace GameEyes {
	static bool Active = false;
	static NiD3DPixelShaderEx* PS = nullptr;
	static NiD3DVertexShaderEx* VS = nullptr;

	static void Apply(NiD3DPixelShaderEx* ps, NiD3DVertexShaderEx* vs) {
		IDirect3DDevice9* device = TheRenderManager->device;
		const bool nvrPixel = ps && ps->ShaderHandleBackup && ps->ShaderHandle != ps->ShaderHandleBackup;
		const bool nvrVertex = vs && vs->ShaderHandleBackup && vs->ShaderHandle != vs->ShaderHandleBackup;
		PS = nvrPixel ? ps : nullptr;
		VS = nvrVertex ? vs : nullptr;
		if (PS) device->SetPixelShader((IDirect3DPixelShader9*)PS->ShaderHandleBackup);
		if (VS) device->SetVertexShader((IDirect3DVertexShader9*)VS->ShaderHandleBackup);
		static bool announced = false;
		if ((PS || VS) && !announced) {
			announced = true;
			Logger::Log("UNOFFICIAL GameShaderEyes: eyeballs drawn with the game's own shaders, per draw (first: %s + %s)",
				vs && vs->Name ? vs->Name : "?", ps && ps->Name ? ps->Name : "?");
		}
	}

	static void Before(const PointShadowForward::RenderPassView* pass) {
		Active = false;
		// UNOFFICIAL 2026-10-28: with LinearScene on, the eyes keep NVR's shader (a game shader there would turn the whole frame back to the
		// stored curve and forth again for every eye).
		if (!GameShaderEyes || LinearScene::Live || !pass || !pass->geometry) return;
		const BSShaderProperty* prop = static_cast<const BSShaderProperty*>(((NiGeometry*)pass->geometry)->GetProperty(NiProperty::kType_Shade));
		if (!prop || !prop->GetFlag(BSSP_EYE_ENVIRONMENT_MAPPING)) return;
		Active = true;
		NiD3DPass* bound = *(NiD3DPass**)0x0126F74C;
		if (bound) Apply((NiD3DPixelShaderEx*)bound->PixelShader, (NiD3DVertexShaderEx*)bound->VertexShader);
	}

	static void OnBind(NiD3DPixelShaderEx* ps, NiD3DVertexShaderEx* vs) { if (Active) Apply(ps, vs); }

	static void After() {
		if (!Active) return;
		Active = false;
		IDirect3DDevice9* device = TheRenderManager->device;
		if (PS) device->SetPixelShader(PS->ShaderHandle);
		if (VS) device->SetVertexShader(VS->ShaderHandle);
		PS = nullptr; VS = nullptr;
	}
}

void __fastcall SetShadersHook(BSShader* This, UInt32 edx, UInt32 PassIndex) {
	BarrelHeat::Clear();
	const bool profiling = ShaderSplit::FrameProfiled;
	const double bindStart = profiling ? CpuTimer::NowMs() : 0.0;
	void* geometrySlot = *(void**)0x011F91E0;
	NiGeometry* Geometry = *(NiGeometry**)geometrySlot;
	NiD3DPass* Pass = *reinterpret_cast<NiD3DPass**>(0x0126F74C);
	NiD3DVertexShaderEx* VertexShader = static_cast<NiD3DVertexShaderEx*>(Pass->VertexShader);
	NiD3DPixelShaderEx* PixelShader = static_cast<NiD3DPixelShaderEx*>(Pass->PixelShader);
	NiDX9RenderState* renderState = TheRenderManager->renderState;
	IDirect3DVertexShader9* VertexShader2 = renderState->GetVertexShader();
	IDirect3DPixelShader9* PixelShader2 = renderState->GetPixelShader();

	// UNOFFICIAL volumetric clouds: the game's cloud layers (SKYTEX.pso) stay hidden while they draw; in the water's reflection
	// too when the clouds are drawn into it (WaterReflections), else they stay there. Set before the shader's constants are
	// uploaded below. In the reflection pass, the clouds note its target, depth buffer and camera (DrawIntoReflection).
	if (VolumetricCloudsEffect* clouds = TheShaderManager->Effects.VolumetricClouds) {
		const bool reflections = ShaderSplit::CurrentContext == ShaderSplit::Reflections;
		clouds->Constants.Hide.x = reflections ? clouds->HideGameCloudsInReflection : clouds->HideGameClouds;
		if (reflections && clouds->HideGameCloudsInReflection > 0.0f) clouds->CaptureReflection(TheRenderManager->device);
	}

	// 2026-11-03: only shaders NVR made are set up (ShaderRecord.h ShaderOwnership: Real Time Reflections' own env shaders stay its own)
	if (VertexShader) {
		if (ShaderOwnership::MaySetUp(VertexShader, "vertex")) VertexShader->SetupShader(VertexShader2);
	}
	else
		Logger::Log("Error getting vertex shader for pass %s", Pointers::Functions::GetPassDescription(PassIndex));
	// UNOFFICIAL cloth wind: which objects drew with a vanilla vertex shader (they must not get cloth: see ClothWind.h SAFETY)
	ClothWind::NoteVertexShader(VertexShader ? VertexShader->Name : nullptr);
	ClothWind::NoteShaders(ClothWind::PassGeometry ? ClothWind::PassGeometry : Geometry, VertexShader && VertexShader->ShaderHandleBackup && VertexShader->ShaderHandle != VertexShader->ShaderHandleBackup &&
		ClothWind::CapableName(VertexShader->Name), VertexShader ? VertexShader->Name : nullptr);
	if (PixelShader) {
		if (ShaderOwnership::MaySetUp(PixelShader, "pixel")) PixelShader->SetupShader(ForcePixelConstants ? nullptr : PixelShader2);
		ForcePixelConstants = false;
	} else
		Logger::Log("Error getting pixel shader for pass %s", Pointers::Functions::GetPassDescription(PassIndex));
	// UNOFFICIAL 2026-10-22 MarkShaders (test only, [Shaders.PBR.Main] MarkShadersTo > 0): the vertex + pixel shader pairs the game draws with,
	// each pair once (first 120), with whether NVR replaced each.
	if (MarkShadersTo > 0 && VertexShader && PixelShader) {
		static std::unordered_set<std::string> pairs;
		if (pairs.size() < 120) {
			std::string key = std::string(VertexShader->Name ? VertexShader->Name : "?") + " + " + (PixelShader->Name ? PixelShader->Name : "?");
			if (pairs.insert(key).second)
				Logger::Log("MarkShaders: pair %s (pass %s) vs %s ps %s", key.c_str(), Pointers::Functions::GetPassDescription(PassIndex),
					VertexShader->ShaderHandle != VertexShader->ShaderHandleBackup ? "NVR" : "game", PixelShader->ShaderHandle != PixelShader->ShaderHandleBackup ? "NVR" : "game");
		}
	}
	ReflMaterial::OnShadersBound(TheRenderManager->device);	// UNOFFICIAL 2026-10-21 MaterialAware reflections: alpha writes for a flagged pass
	RtHairTint::NoteBind(PixelShader ? PixelShader->Name : nullptr);	// UNOFFICIAL 2026-11-07 perf-mirrorfix: a hair shader bound (its tint for the RT scene)
	if (TheShaderManager->Shaders.Grass) TheShaderManager->Shaders.Grass->SetForDraw(VertexShader, PixelShader);	// UNOFFICIAL 2026-10-17 grass lighting fix
	TerrainProbe::LastPixel = PixelShader ? PixelShader->Name : nullptr;	// UNOFFICIAL 2026-10-17 terrain study, diagnostic
	TerrainProbe::LastVertex = VertexShader ? VertexShader->Name : nullptr;
#if NVR_TEST_TOOLS
	LampsInOnePass::Census::OnBind(PassIndex, PixelShader ? PixelShader->Name : nullptr, VertexShader ? VertexShader->Name : nullptr);	// UNOFFICIAL, diagnostic
#endif
	// Diagnostic (P57): the water reflection with the game's own shaders. NVR's replacements take constants made for
	// the main camera; the reflection camera sits mirrored below the water. SetupShader picks the handle the game
	// binds next, so switching it here keeps the game's state tracking right; the next bind picks NVR's again.
	if (TheSettingManager->SettingsMain.Main.GameShadersInReflections && ShaderSplit::CurrentContext == ShaderSplit::Reflections) {
		if (VertexShader && VertexShader->ShaderHandleBackup) VertexShader->ShaderHandle = (IDirect3DVertexShader9*)VertexShader->ShaderHandleBackup;
		if (PixelShader && PixelShader->ShaderHandleBackup) PixelShader->ShaderHandle = (IDirect3DPixelShader9*)PixelShader->ShaderHandleBackup;
	}
	// UNOFFICIAL 2026-10-27 race menu face (tester report): skin drawn outside the world scene, first person and reflections --
	// the character creation menu's head (its own camera, lights and picture; no world scene is drawn while it is open) and
	// other 3D menu views -- gets the game's own skin shaders. NVR's skin shaders take the world camera's shadow, sky and lamp
	// data, which do not belong to that picture: the face came out invisible outdoors (Novac) and yellow indoors (Doc Mitchell's).
	if (ShaderSplit::CurrentContext == ShaderSplit::Outside && PixelShader && PixelShader->Name && !strncmp(PixelShader->Name, "SKIN", 4)) {
		static bool announced = false;
		const bool nvrPixel = PixelShader->ShaderHandleBackup && PixelShader->ShaderHandle != PixelShader->ShaderHandleBackup;
		const bool nvrVertex = VertexShader && VertexShader->Name && !strncmp(VertexShader->Name, "SKIN", 4) &&
			VertexShader->ShaderHandleBackup && VertexShader->ShaderHandle != VertexShader->ShaderHandleBackup;
		if (nvrPixel) PixelShader->ShaderHandle = (IDirect3DPixelShader9*)PixelShader->ShaderHandleBackup;
		if (nvrVertex) VertexShader->ShaderHandle = (IDirect3DVertexShader9*)VertexShader->ShaderHandleBackup;
		if ((nvrPixel || nvrVertex) && !announced) {
			announced = true;
			Logger::Log("UNOFFICIAL skin outside the world scene (%s + %s, e.g. the character creation menu): drawn with the game's own skin shaders",
				VertexShader && VertexShader->Name ? VertexShader->Name : "?", PixelShader->Name);
		}
	}
	// UNOFFICIAL 2026-10-27 [Shaders.PBR.Main] GameShaderEyes (user: "pbr seems to apply to eyeballs and can darken them in some areas. it
	// looks a bit odd. can we skip eyeballs?"): an eyeball is a small sphere, and NVR's object shaders light it as one -- the sun's and the
	// lamps' falloff across it, its own sun shadow and the eyelids' -- so half of each eye went dark, where the game's own shader keeps it
	// evenly bright with its environment map. The game marks its eye meshes (Eye_Environment_Mapping, BSSP flag 17): those draw with the
	// game's own shaders, as the race menu's face above (the next bind picks NVR's again). Off: NVR's shaders, as before.
	// 2026-10-28 (user: "pbr will pop in and out on a couple objects when im turning while standing still"; PbrPopLog + LinearSceneCensus,
	// Freeside: road pieces and walls switching 0 <-> 1 lamp were drawn with the GAME's SLS2011 / SLS2000 ~30 times a frame): the swap above
	// changed the shared shader object, and the game binds once for a whole run of one pass kind, so every object after an eye in that run
	// (roads, walls, windows) was drawn with the game's flat shader too, depending on what was on screen. Now only the eye's own draw gets the
	// game's shaders on the device (GameEyes::Before / OnBind / After, the draw hooks below); the shader objects keep NVR's.
	// UNOFFICIAL lit particles (Shaders.Particles): the NOLIGHT replacements are shader model 3, and D3D9 cannot pair a
	// 3.0 shader with the game's 2.x one. Where only one side of a NOLIGHT draw has a replacement -- NOLIGHTTEXVC.pso
	// behind the game's glow/muzzle-flash vertex shaders, or NOLIGHT016/017.vso in front of the flame/spark pixel
	// shader -- the draw gets the game's own pair. Skipped entirely while the collection is off.
	// (2026-11-05 soft particles: every NOLIGHT shader has a replacement but the skinned NOLIGHT008-015.vso, so those are the draws left here.)
	if (VertexShader && PixelShader && TheShaderManager->Shaders.Particles && TheShaderManager->Shaders.Particles->Enabled) {
		const bool nvrVertex = VertexShader->ShaderHandleBackup && VertexShader->ShaderHandle != VertexShader->ShaderHandleBackup;
		const bool nvrPixel = PixelShader->ShaderHandleBackup && PixelShader->ShaderHandle != PixelShader->ShaderHandleBackup;
		auto particleFamily = [](const char* name) { return name && (!strncmp(name, "NOLIGHT", 7) || !strncmp(name, "GDECAL", 6)); };
		const bool family = particleFamily(VertexShader->Name) || particleFamily(PixelShader->Name);
		// The first-person pass (the player's own muzzle flash, Pip-Boy glow) always gets the game's pair.
		if (family && ShaderSplit::CurrentContext == ShaderSplit::FirstPerson) {
			if (nvrVertex) VertexShader->ShaderHandle = (IDirect3DVertexShader9*)VertexShader->ShaderHandleBackup;
			if (nvrPixel) PixelShader->ShaderHandle = (IDirect3DPixelShader9*)PixelShader->ShaderHandleBackup;
		}
		else if (nvrVertex != nvrPixel && family) {
			if (nvrVertex) VertexShader->ShaderHandle = (IDirect3DVertexShader9*)VertexShader->ShaderHandleBackup;
			else PixelShader->ShaderHandle = (IDirect3DPixelShader9*)PixelShader->ShaderHandleBackup;
		}
	}
#if NVR_TEST_TOOLS
	if (FrameRecorder::Active()) {	// UNOFFICIAL, test only: the recorded frame's shader names (FrameRecorder.h)
		char name[160];
		if (VertexShader && VertexShader->ShaderHandle) {
			sprintf_s(name, "%s %s", VertexShader->Name ? VertexShader->Name : "?", (void*)VertexShader->ShaderHandle != (void*)VertexShader->ShaderHandleBackup ? "(NVR)" : "(vanilla)");
			FrameRecorder::NameShader(VertexShader->ShaderHandle, name);
		}
		if (PixelShader && PixelShader->ShaderHandle) {
			sprintf_s(name, "%s %s", PixelShader->Name ? PixelShader->Name : "?", (void*)PixelShader->ShaderHandle != (void*)PixelShader->ShaderHandleBackup ? "(NVR)" : "(vanilla)");
			FrameRecorder::NameShader(PixelShader->ShaderHandle, name);
		}
	}
#endif
	ShaderSplit::OnBind(PixelShader, PixelShader2, bindStart);
	if (ShaderSplit::CurrentContext == ShaderSplit::World) TerrainShaders::NoteWorldBind();	// UNOFFICIAL terrain blending diagnostic
	ReflectionProbe::OnBind(Geometry, PixelShader ? PixelShader->Name : nullptr, VertexShader ? VertexShader->Name : nullptr);

	// trace pipeline active shaders
	const auto& develop = TheSettingManager->SettingsMain.Develop;
	if (develop.DebugMode && !InterfaceManager->IsActive(Menu::MenuType::kMenuType_Console) && Global->OnKeyDown(develop.TraceShaders)) {
		char label[256];
		sprintf(label, "Pass %i %s, %s (%s %s)", PassIndex, Pointers::Functions::GetPassDescription(PassIndex), Geometry->m_pcName, VertexShader->Name, PixelShader->Name);
		if (VertexShader->ShaderHandle == VertexShader->ShaderHandleBackup) strcat(label, " - Vertex: vanilla");
		if (PixelShader->ShaderHandle == PixelShader->ShaderHandleBackup) strcat(label, " - Pixel: vanilla");
		Logger::Log("%s", label);
		InterfaceManager->ShowMessage("Shaders Traced");
		//DWNode::AddNode(Name, Geometry->m_parent, Geometry);
	}
	FlashlightLamp::OnBind(PixelShader ? PixelShader->ShaderHandle : nullptr);	// UNOFFICIAL 2026-11-04 its c28-c30 only for a shader that reads them
	SetShaders(This, PassIndex);
	GameEyes::OnBind(PixelShader, VertexShader);	// UNOFFICIAL GameShaderEyes: inside an eye's draw, the game's shaders on the device (see above)
	MaterialRulesDraw::AfterBind();	// UNOFFICIAL material rules: this object's class again over the bind's "no rule"
	ReflectionProbe::AfterBind();  // UNOFFICIAL ReflectionClipFix: the clip plane re-issued once the pass's shaders are bound
	BarrelHeat::SetForDraw(Geometry, VertexShader, PixelShader);
	HandBlood::SetForDraw();	// UNOFFICIAL: blood on the first-person weapon and gloves
	if (TheShaderManager->Effects.HudRaindrops) TheShaderManager->Effects.HudRaindrops->SetDrawConstants(ShaderSplit::CurrentContext == ShaderSplit::FirstPerson);	// UNOFFICIAL 2026-11-04: rain drops (Wetness Effects HudRaindrops)
	LampScissor::OnBind(PixelShader, ShaderSplit::CurrentContext == ShaderSplit::World);	// UNOFFICIAL LampScissor
	HiddenObjectCulling::OnBind(PixelShader);	// UNOFFICIAL HiddenObjectCulling: only lamp-only passes may be skipped
	// UNOFFICIAL interior forward point-light shadows: the draw's lamps and their cube maps (PointShadowForward.h).
	PointShadowForward::LastVertexShaderName = VertexShader ? VertexShader->Name : nullptr;	// AimProbe (LogLamps)
	PointShadowForward::LastVertexShaderNvr = VertexShader && VertexShader->ShaderHandleBackup && VertexShader->ShaderHandle != VertexShader->ShaderHandleBackup;
	PointShadowForward::LastPixelShaderNvr = PixelShader && PixelShader->ShaderHandleBackup && PixelShader->ShaderHandle != PixelShader->ShaderHandleBackup;
	FoldHighlights::OnBind(PixelShader ? PixelShader->Name : nullptr, PointShadowForward::LastPixelShaderNvr, PassIndex);	// UNOFFICIAL FoldHighlights
#if NVR_TEST_TOOLS
	PassFamilyView::OnBind(TheRenderManager->device, PixelShader ? PixelShader->Name : nullptr, PointShadowForward::LastPixelShaderNvr,
		ShaderSplit::CurrentContext == ShaderSplit::World, PassIndex);	// UNOFFICIAL, diagnostic: PassFamilyView
	CasterView::OnBind(TheRenderManager->device);	// UNOFFICIAL 2026-10-28, diagnostic
	LinearSceneCensus::OnBind(PixelShader ? PixelShader->ShaderHandle : nullptr, PixelShader ? PixelShader->Name : nullptr, PointShadowForward::LastPixelShaderNvr, PassIndex);	// UNOFFICIAL 2026-10-28, diagnostic
#endif
	PointShadowForward::SetForDraw(PixelShader, ShaderSplit::CurrentContext == ShaderSplit::World,
		ShaderSplit::CurrentContext == ShaderSplit::FirstPerson,
		PixelShader && PointShadowForward::LastPixelShaderNvr && ManyLamps::IsOurs(PixelShader->Name) && ManyLamps::Active() ?
			ManyLamps::Steady(*(const PointShadowForward::RenderPassView**)0x011F91E0) : nullptr);	// UNOFFICIAL 2026-10-31 SteadyLampShadows
	LandLamps::OnBind(PixelShader ? PixelShader->Name : nullptr, PointShadowForward::LastPixelShaderNvr);	// UNOFFICIAL 2026-10-30 LandAllLamps
	ManyLamps::OnBind(PixelShader ? PixelShader->Name : nullptr, PointShadowForward::LastPixelShaderNvr, ShaderSplit::CurrentContext == ShaderSplit::FirstPerson, ShaderSplit::CurrentContext == ShaderSplit::World);	// UNOFFICIAL 2026-10-27 ManyLampsOnePass: the run's first object
	DeferredLamps::OnBind(PixelShader ? PixelShader->ShaderHandle : nullptr);	// UNOFFICIAL 2026-10-31 DeferredLamps: counts the deferred binds, reads the frame globals once
	ScopeSunShadows::ForDraw(*(const PointShadowForward::RenderPassView**)0x011F91E0);	// UNOFFICIAL ScopeFix outdoors
	OnePassDepth::Reapply(TheRenderManager->device);	// UNOFFICIAL LampsDepthFirst: a bind inside a depth-only draw keeps the depth-only shader
#if NVR_TEST_TOOLS
	SettingShots::Land::OnBind(TheRenderManager->device, PixelShader ? PixelShader->Name : nullptr, ShaderSplit::CurrentContext == ShaderSplit::World);	// UNOFFICIAL 2026-10-30, test only: 'landflicker' mask
#endif
	if (profiling) ShaderSplit::EndBind(bindStart);

}

// UNOFFICIAL interior forward shadows: called once per object, only from BSBatchRenderer::RenderPassImmediately, after it
// bound the shaders (when the pass type or shader changed). Each object gets its own lamps (PointShadowForward.h).
// Hooked only while [Shaders.ShadowsInteriors.Forward] Enabled was on at startup (Hooks.cpp).
void(__cdecl* RenderPassStandard)(void*, UInt32, UInt32, UInt32) = (void(__cdecl*)(void*, UInt32, UInt32, UInt32))Hooks::RenderPassStandard;
void(__cdecl* RenderPassSkinned)(void*, UInt32, UInt32, UInt32) = (void(__cdecl*)(void*, UInt32, UInt32, UInt32))Hooks::RenderPassSkinned;

// UNOFFICIAL ScopeFix outdoors ([Shaders.ShadowsInteriors.Forward] ScopeFix): the object and terrain shaders' sun shadows
// (GetSunShadow) rebuild positions with the main camera's matrices, so B42 Optics' lens and its scope picture (the world
// drawn by another camera into a small square texture, JIP's ProjectExtraCamera, outside the game's passes) got the main
// view's shadow pattern laid over them. For those draws the forward path's own off switch, TESR_ShadowForwardData.x (c133,
// pinned in Shadow.hlsl, which only NVR's shaders read: the game's use nothing above c97), is written straight to the
// device; the next other draw gets the real value back. The picture is recognised by its size (not the screen's, not the
// water reflection), and only while a B42 lens was drawn in the last two frames: without B42 Optics nothing changes.
namespace ScopeSunShadows {
	static bool Overridden = false;
	static unsigned LensFrame = 0;		// PointShadowForward::Frame when a B42 lens was last drawn
	static bool Announced = false;

	static void ForDraw(const PointShadowForward::RenderPassView* pass) {
		ShadowsExteriorEffect* shadows = TheShaderManager->Effects.ShadowsExteriors;
		if (!shadows) return;
		bool suppress = false;
		if (PointShadowForward::ScopeFix && TheShaderManager->GameState.isExterior) {
			const unsigned char context = ShaderSplit::CurrentContext;
			if (context == ShaderSplit::FirstPerson) {
				suppress = PointShadowForward::IsLens(pass);
				if (suppress) LensFrame = PointShadowForward::Frame;
			}
			else if (context != ShaderSplit::Reflections && LensFrame && PointShadowForward::Frame - LensFrame <= 2) {
				const D3DVIEWPORT9& port = TheRenderManager->m_kD3DPort;
				if (!TAAEffect::IsWorldViewport(port)) {	// UNOFFICIAL 2026-10-27: RenderScale's world viewport is the world's too
					suppress = true;
					if (!Announced) {
						Announced = true;
						Logger::Log("UNOFFICIAL ScopeFix: the B42 Optics scope picture (%ux%u) is drawn without sun shadows.",
							(unsigned)port.Width, (unsigned)port.Height);
					}
				}
			}
		}
		if (suppress) {
			D3DXVECTOR4 value = shadows->Constants.ForwardData;
			value.x = 1.0f;
			TheRenderManager->device->SetPixelShaderConstantF(133, (const float*)&value, 1);
			Overridden = true;
		}
		else if (Overridden) {
			TheRenderManager->device->SetPixelShaderConstantF(133, (const float*)&shadows->Constants.ForwardData, 1);
			Overridden = false;
		}
	}
}

// UNOFFICIAL, diagnostic (2026-10-13, with [Shaders.ShadowsInteriors.Forward] LogLamps): how many lamps the game gives each
// object's draw, per pass type, and the largest per-pass cap it set (RenderPass +0x0A), logged every 3 seconds. Asked for by
// "increase the per object point light limit": a wall piece next to a lamp stayed unlit by it while its neighbour was lit.
namespace LampCountStats {
	static unsigned Draws[256][16] = {};
	static unsigned MaxCap[256] = {};
	static DWORD Last = 0;
	// Every lamp that reached a draw in the interval: its position and reach (its NiLight: +0xF8; world position +0x8C, radius
	// +0xE0, as ShadowLightShader::SetupGeometryOpt_LightsSpecular reads them) and in how many draws.
	struct Seen { const void* light; float x, y, z, radius; unsigned draws; };
	static Seen Lamps[128] = {};
	static int LampCount = 0;
	// UNOFFICIAL 2026-10-30 LAMP CAPS (F10 profile, world scene; ROOT-ASSUMPTIONS-PLAN C: which shader family loses lamps): per pass type
	// (named by the first pixel shader seen drawing it), its objects a frame, and against each object's own active lamps (its lighting
	// property's list +0x60, filtered as GetNumberOfActiveNonShadowLights, as ManyLamps reads it) the lamps none of its passes this frame
	// carried: objects short of lamps and the lamps left out.
	struct Obj { UInt8 type; std::vector<const void*> own, seen; bool templateBase = false; unsigned skippable = 0, skippableLamps = 0; };	// (own: listed when first drawn; the geometry is not read again)
	struct Cap { unsigned long long objects = 0, draws = 0, own = 0, short_ = 0, leftOut = 0; char shader[16] = ""; };
	static Cap Caps[256];
	// UNOFFICIAL 2026-10-31 LAMP CAPS, stage 0b of docs/next/DEFERRED-RENDERING-PLAN.md: the game's extra lamp and highlight passes (PassSums::KindOf
	// Lamps / Highlights) a frame whose object's base pass is one of NVR's templates (a deferred record could skip them), and the objects they belong to.
	static unsigned long long SkipPasses = 0, SkipLamps = 0, SkipObjects = 0, ExtraObjects = 0, ExtraPasses = 0;
	static std::unordered_map<NiGeometry*, Obj> Objects;
	static unsigned CapFrames = 0;
	static UInt32 CapFrame = 0;
	static int OwnLamps(NiGeometry* geometry, std::vector<const void*>* list) {
		// only a lighting property has the light list at +0x60 (run 20261030-193412-slotA crashed reading it from another kind)
		BSShaderProperty* shade = (BSShaderProperty*)geometry->GetProperty(NiProperty::PropertyType::kType_Shade);
		const UInt8* property = shade && shade->IsLightingProperty() ? (const UInt8*)shade : nullptr;
		int n = 0;
		for (const UInt32* node = property ? *(const UInt32* const*)(property + 0x60) : nullptr; node; node = (const UInt32*)node[0]) {
			const UInt8* light = (const UInt8*)node[2];
			if (!light || *(const UInt16*)(light + 0x110) == 0xFF) continue;
			const UInt8* niLight = *(const UInt8* const*)(light + 0xF8);
			if (!niLight || (niLight[0x30] & 1) || light[0xEC]) continue;
			n++;
			if (list) list->push_back(light);
		}
		return n;
	}
	static void EndCapFrame() {
		for (auto& e : Objects) {
			Cap& cap = Caps[e.second.type];
			cap.objects++;
			cap.own += e.second.own.size();
			int missing = 0;
			for (const void* l : e.second.own) if (std::find(e.second.seen.begin(), e.second.seen.end(), l) == e.second.seen.end()) missing++;
			if (missing) { cap.short_++; cap.leftOut += missing; }
			if (e.second.skippable) {	// UNOFFICIAL 2026-10-31 (stage 0b)
				ExtraObjects++; ExtraPasses += e.second.skippable;
				if (e.second.templateBase) { SkipObjects++; SkipPasses += e.second.skippable; SkipLamps += e.second.skippableLamps; }
			}
		}
		Objects.clear();
		if (++CapFrames < 240) return;
		const double f = 1.0 / CapFrames;
		Logger::Log("LAMP CAPS skippable: %.1f extra lamp + highlight passes a frame (%.1f lamps in them) on %.1f objects whose base pass is a Vegas Shaders template; %.1f such passes on %.1f objects in all",
			SkipPasses * f, SkipLamps * f, SkipObjects * f, ExtraPasses * f, ExtraObjects * f);
		SkipPasses = SkipLamps = SkipObjects = ExtraObjects = ExtraPasses = 0;
		for (unsigned t = 0; t < 256; t++) {
			Cap& k = Caps[t];
			if (k.objects) Logger::Log("LAMP CAPS pass 0x%02X (%s): %.1f objects a frame (%.1f draws), %.2f own lamps each; %.1f objects a frame short of lamps, %.1f lamps left out",
				t, k.shader[0] ? k.shader : "?", k.objects * f, k.draws * f, (double)k.own / k.objects, k.short_ * f, k.leftOut * f);
			k = Cap();
		}
		CapFrames = 0;
	}
	// UNOFFICIAL 2026-11-04 (cpu-workers trims): with the F10 profile AND [Main.Main.Misc] LampCapsStats on (a diagnostic, off by default):
	// with F10 alone it ran on every world pass (a map lookup, a vector per object, ~0.7 % of the Ultra-Luxe's main thread), so every F10 log
	// read slower than the game runs.
	static bool CapsWanted = false;
	static UInt32 CapsWantedFrame = ~0u;
	static void CountCaps(const PointShadowForward::RenderPassView* pass) {
		if (!GpuTimer::Enabled || !pass || !pass->geometry || ShaderSplit::CurrentContext != ShaderSplit::World) return;
		const UInt32 frame = TheFrameRateManager ? TheFrameRateManager->FrameNumber : 0;
		if (frame != CapsWantedFrame) { CapsWantedFrame = frame; CapsWanted = TheSettingManager->GetSettingI("Main.Main.Misc", "LampCapsStats") != 0; }
		if (!CapsWanted) return;
		if (frame != CapFrame) { if (CapFrame) EndCapFrame(); CapFrame = frame; }
		const UInt8 type = pass->passEnum & 0xFF;
		Cap& cap = Caps[type];
		cap.draws++;
		if (!cap.shader[0] && PointShadowForward::LastPixelShaderName) strncpy_s(cap.shader, PointShadowForward::LastPixelShaderName, _TRUNCATE);
		auto found = Objects.find(pass->geometry);
		if (found == Objects.end()) { found = Objects.emplace(pass->geometry, Obj{ type, {}, {} }).first; OwnLamps(pass->geometry, &found->second.own); }
		for (int k = 0; pass->lights && k < pass->numLights; k++) if (pass->lights[k]) found->second.seen.push_back(pass->lights[k]);
		{	// UNOFFICIAL 2026-10-31 (stage 0b): an extra lamp / highlight pass, or a base pass drawn with one of NVR's pixel shaders
			const PassSums::Kind kind = PassSums::KindOf(pass->passEnum);
			if (kind == PassSums::Lamps || kind == PassSums::Highlights) { found->second.skippable++; found->second.skippableLamps += pass->numLights > 1 ? pass->numLights - 1 : 0; }
			else if (PointShadowForward::LastPixelShaderNvr) found->second.templateBase = true;
		}
	}
	static void Count(const PointShadowForward::RenderPassView* pass) {
		CountCaps(pass);
		if (!PointShadowForward::LogObjectLamps || !pass) return;
		const unsigned type = pass->passEnum & 0xFF;
		Draws[type][(std::min)((unsigned)pass->numLights, 15u)]++;
		MaxCap[type] = (std::max)(MaxCap[type], (unsigned)pass->maxLights);
		for (int k = 0; pass->lights && k < pass->numLights; k++) {
			const ShadowSceneLight* light = pass->lights[k];
			if (!light) continue;
			int i = 0;
			while (i < LampCount && Lamps[i].light != light) i++;
			if (i == LampCount) {
				if (LampCount == 128) continue;
				const char* source = *(const char* const*)((const char*)light + 0xF8);
				if (!source) continue;
				const float* p = (const float*)(source + 0x8C);
				Lamps[LampCount++] = { light, p[0], p[1], p[2], *(const float*)(source + 0xE0), 0 };
			}
			Lamps[i].draws++;
		}
		const DWORD now = GetTickCount();
		if (now - Last < 3000) return;
		Last = now;
		for (int i = 0; i < LampCount; i++)
			Logger::Log("LAMP SEEN %p at %.0f %.0f %.0f, reach %.0f, in %u draws", Lamps[i].light, Lamps[i].x, Lamps[i].y, Lamps[i].z, Lamps[i].radius, Lamps[i].draws);
		LampCount = 0;
		for (unsigned t = 0; t < 256; t++) {
			unsigned total = 0;
			for (unsigned n = 0; n < 16; n++) total += Draws[t][n];
			if (!total) continue;
			char text[256] = ""; size_t used = 0;
			for (unsigned n = 0; n < 16 && used < sizeof(text) - 16; n++)
				if (Draws[t][n]) used += snprintf(text + used, sizeof(text) - used, " %u:%u", n, Draws[t][n]);
			Logger::Log("LAMP COUNTS pass 0x%02X: %u draws, lamps per draw (count:draws)%s, largest cap %u", t, total, text, MaxCap[t]);
		}
		memset(Draws, 0, sizeof(Draws)); memset(MaxCap, 0, sizeof(MaxCap));
	}
}

// UNOFFICIAL 2026-10-29 MaterialBuffer (core/CapturedNormals.h, Includes/CapturedNormals.hlsl; c211.w since 2026-10-31): the drawn object's
// material class and world bound radius for the templates' COLOR2. Class: 2 skinned (worn gear), 3 cloth / 5 plant by ClothWind's / PlantWind's
// own per-texture verdicts (cached; nothing new decided), else 1. The land's shader writes 4 itself.
// UNOFFICIAL 2026-11-04: whether the draw is a plant (the plant wind's Keywords, by its diffuse texture). With the plant wind off nothing
// decides the plants (PlantWind::ForGeometry returns first): decided here, by the same lists, into the same cache (MaterialBuffer's plant
// class, SunFar's and ForwardCloudShadows' PlantShadows).
static bool PlantPass(NiGeometry* geometry) {
	if (!geometry || geometry->skinInstance) return false;
	BSShaderProperty* property = (BSShaderProperty*)geometry->GetProperty(NiProperty::PropertyType::kType_Shade);
	if (!property || !property->IsLightingProperty()) return false;
	NiSourceTexture** textures = ((BSShaderPPLightingProperty*)property)->ppTextures[0];
	const NiSourceTexture* diffuse = textures ? *textures : nullptr;
	if (!diffuse) return false;
	auto plant = PlantWind::ByTexture.find(diffuse);
	if (plant == PlantWind::ByTexture.end()) {
		if (!diffuse->ddsPath1) return false;
		if (PlantWind::ByTexture.size() >= 4096) PlantWind::ByTexture.clear();
		plant = PlantWind::ByTexture.emplace(diffuse, std::make_pair(std::string(diffuse->ddsPath1), PlantWind::IsPlantTexture(diffuse->ddsPath1))).first;
	}
	return plant->second.second;
}

static void MaterialForPass(void* Pass) {
	if (!CapturedNormals::InSpan || !CapturedNormals::MaterialCompiled || !CapturedNormals::MaterialTexture) return;
	NiGeometry* geometry = ((const PointShadowForward::RenderPassView*)Pass)->geometry;
	if (!geometry) return;
	float materialClass = geometry->skinInstance ? 2.0f : 1.0f;
	BSShaderProperty* property = (BSShaderProperty*)geometry->GetProperty(NiProperty::PropertyType::kType_Shade);
	if (property && property->IsLightingProperty()) {
		NiSourceTexture** textures = ((BSShaderPPLightingProperty*)property)->ppTextures[0];
		const NiSourceTexture* diffuse = textures ? *textures : nullptr;
		if (diffuse) {
			auto cloth = ClothWind::ByTexture.find(diffuse);
			if (cloth != ClothWind::ByTexture.end() && cloth->second.second) materialClass = 3.0f;
			else if (PlantPass(geometry)) materialClass = 5.0f;
		}
	}
	const float radius = geometry->m_kWorldBound ? std::clamp(geometry->m_kWorldBound->Radius, 0.0f, 1.0e6f) : 0.0f;
	PbrPop::SpecFade::SendMaterial(TheRenderManager->device, materialClass + 8.0f * floorf(radius));	// c211.w (c217 is the RT lamp shadows')
}

static void ForwardShadowsForPass(void* Pass) {
	// UNOFFICIAL 2026-11-04 the flashlight's game lamp for this draw (core/FlashlightLamp.h), first: everything below reads the lamps' dimmers
	FlashlightLamp::ForPass(Pass, ShaderSplit::CurrentContext == ShaderSplit::World && CapturedNormals::InSpan);
	EnvLightFade::ForPass(Pass);	// UNOFFICIAL 2026-11-04 (after it: the game reads that lamp's dimmer for the fade too)
	if (ShaderSplit::CurrentContext == ShaderSplit::FirstPerson) ShadowManager::ViewModelLogDraw(((const PointShadowForward::RenderPassView*)Pass)->geometry);	// UNOFFICIAL 2026-11-04 CharacterShadowMapLog
	if (ShaderSplit::CurrentContext == ShaderSplit::World) MaterialForPass(Pass);	// UNOFFICIAL 2026-10-29 MaterialBuffer
	if (ForwardCloudShadows::Compiled)	// UNOFFICIAL 2026-11-04 ForwardCloudShadows: this draw's strength (0 for plants with PlantShadows off)
		ForwardCloudShadows::ForPass(TheRenderManager->device, !ForwardCloudShadows::PlantShadows && PlantPass(((const PointShadowForward::RenderPassView*)Pass)->geometry));
	LampCountStats::Count((const PointShadowForward::RenderPassView*)Pass);
	LandLamps::ForPass((const PointShadowForward::RenderPassView*)Pass);	// UNOFFICIAL 2026-10-30 LandAllLamps: the piece's own lamps, for after VPT's write
	// UNOFFICIAL 2026-10-31 SteadyLampShadows: while a many-lamp shader is bound, its lamps (and the six with cube-map shadows) in a steady order
	const PointShadowForward::RenderPassView* lamps = ManyLamps::Bound ? ManyLamps::Steady((const PointShadowForward::RenderPassView*)Pass) : (const PointShadowForward::RenderPassView*)Pass;
	ManyLamps::ForPass(lamps, ShaderSplit::CurrentContext == ShaderSplit::FirstPerson, ShaderSplit::CurrentContext == ShaderSplit::World);	// UNOFFICIAL 2026-10-27 ManyLampsOnePass: the object's lamps in c37-c63
	LampsInOnePass::GlowForPass((const PointShadowForward::RenderPassView*)Pass);	// UNOFFICIAL ManyLampsGlow: the glow map on s4
	PointShadowForward::OnRenderPass(lamps, ShaderSplit::CurrentContext == ShaderSplit::World,
		ShaderSplit::CurrentContext == ShaderSplit::FirstPerson);
	ManyLamps::ParityCheck((const PointShadowForward::RenderPassView*)Pass, ShaderSplit::CurrentContext == ShaderSplit::World);	// UNOFFICIAL 2026-10-31 TableLampsParity (a test)
	ScopeSunShadows::ForDraw((const PointShadowForward::RenderPassView*)Pass);
}

// UNOFFICIAL 2026-10-26 LampsDepthFirst, step 1 (core/OnePassDepth.h): the pass drawn with depth only. Only what moves vertices is
// set up as for the real draw (cloth and plant wind: same constants as step 2's draw); the game's own setup runs as always.
static void DepthOnlyStandard(void* Pass, UInt32 Arg2, UInt32 Arg3, UInt32 Arg4) {
	IDirect3DDevice9* device = TheRenderManager->device;
	NiGeometry* geometry = ((const PointShadowForward::RenderPassView*)Pass)->geometry;
	ClothWind::ForGeometry(device, geometry, false, false);
	PlantWind::ForGeometry(device, geometry, false, false);
	ClothWind::PassGeometry = geometry;
	OnePassDepth::BeginDepth(device);
	RenderPassStandard(Pass, Arg2, Arg3, Arg4);
	OnePassDepth::EndDepth(device);
	OnePassDepth::DepthDrawn = true;
	ClothWind::PassGeometry = nullptr;
	ClothWind::AfterDraw(device);
}

// UNOFFICIAL 2026-11-05 soft particles: BSShaderNoLighting::SetupGeometryConstants 0xBC9A50 (vtable slot 0x10BB2AC), run for every
// NoLighting geometry between its shader bind and its draw. The blended ones (particles, glows, the dust storm walls) are drawn past
// RenderPassStandard/SkinnedHook, so this is where each gets its soft size.
static VirtFuncDetour NoLightingConstantsDetour;
static void __fastcall NoLightingConstantsHook(void* shader, void*, void* properties) {
	ThisStdCall(NoLightingConstantsDetour.GetOverwrittenAddr(), shader, properties);
	const PointShadowForward::RenderPassView* pass = *(const PointShadowForward::RenderPassView* const*)0x011F91E0;	// BSShaderManager::pCurrentRenderPass
	if (pass) ParticleShaders::ForDraw(pass->geometry, pass->passEnum, ShaderSplit::CurrentContext == ShaderSplit::World);
}

void InstallSoftParticleHook() {
	if (*(UInt32*)0x010BB2AC != 0x00BC9A50) {
		Logger::Log("Soft particles: BSShaderNoLighting::SetupGeometryConstants is already replaced (another plugin?); no fade");
		return;
	}
	NoLightingConstantsDetour.ReplaceVirtualFunc(0x010BB2AC, &NoLightingConstantsHook);
}

void __cdecl RenderPassStandardHook(void* Pass, UInt32 Arg2, UInt32 Arg3, UInt32 Arg4) {
	OnePassDepth::HookSeen = true;
	if (OnePassDepth::InDepth) { DepthOnlyStandard(Pass, Arg2, Arg3, Arg4); return; }	// UNOFFICIAL LampsDepthFirst, step 1
	TerrainProbe::Note((const PointShadowForward::RenderPassView*)Pass);	// UNOFFICIAL 2026-10-17 terrain study, diagnostic
#if NVR_TEST_TOOLS
	LampsInOnePass::Census::Note(Pass, ShaderSplit::CurrentContext == ShaderSplit::World);	// UNOFFICIAL, diagnostic (F10 only)
	if (ShaderSplit::CurrentContext == ShaderSplit::World || ShaderSplit::CurrentContext == ShaderSplit::FirstPerson)	// UNOFFICIAL, diagnostic: PbrPopLog
		PbrPop::PopLog::Note((const PointShadowForward::RenderPassView*)Pass, ShaderSplit::CurrentContext == ShaderSplit::FirstPerson ? 1 : 0);
	if (LampPassTest::Skip(Pass, ShaderSplit::CurrentContext == ShaderSplit::World)) return;	// UNOFFICIAL, diagnostic
	if (PassFamilyView::Skip(Pass, ShaderSplit::CurrentContext == ShaderSplit::World)) return;	// UNOFFICIAL, diagnostic: PassFamilyView
	if (CasterView::Skip(Pass, ShaderSplit::CurrentContext == ShaderSplit::World)) return;	// UNOFFICIAL 2026-10-28, diagnostic: CasterView
#endif
	if (FoldFog::Skip(Pass, ShaderSplit::CurrentContext == ShaderSplit::World)) return;	// UNOFFICIAL FoldFog: the object's lit pass fogged
	if (FoldHighlights::Skip(Pass, ShaderSplit::CurrentContext == ShaderSplit::World)) {	// UNOFFICIAL FoldHighlights: drawn with the object's lamps
#if NVR_TEST_TOOLS
		LampsInOnePass::Census::NoteSkipped(Pass);	// diagnostic (F10 only)
#endif
		return;
	}
	PassSums::BeforePass(Pass, ShaderSplit::CurrentContext == ShaderSplit::World, TheRenderManager->device);	// UNOFFICIAL ExactPassSums
	ClothWind::ForGeometry(TheRenderManager->device, ((const PointShadowForward::RenderPassView*)Pass)->geometry, false, ShaderSplit::CurrentContext == ShaderSplit::FirstPerson);	// UNOFFICIAL cloth wind
	PlantWind::ForGeometry(TheRenderManager->device, ((const PointShadowForward::RenderPassView*)Pass)->geometry, false, ShaderSplit::CurrentContext == ShaderSplit::FirstPerson);	// UNOFFICIAL 2026-10-20 plant wind (cleaned by ClothWind::AfterDraw)
	if (HiddenObjectCulling::Before(Pass, ShaderSplit::CurrentContext == ShaderSplit::World)) { ClothWind::AfterDraw(TheRenderManager->device); return; }	// UNOFFICIAL HiddenObjectCulling
	ReflectionProbe::OnObject(Pass, false);
	MaterialRulesDraw::ForObject((const PointShadowForward::RenderPassView*)Pass);	// UNOFFICIAL material rules
	RmaosDraw::ForObject((const PointShadowForward::RenderPassView*)Pass);	// UNOFFICIAL _rmaos maps
	{	// UNOFFICIAL SpecularFade, and FoldHighlights' switches for this draw (same constant)
		float fold = 0.0f, foldVertexColor = 0.0f;
		FoldHighlights::ForDraw(TheRenderManager->device, Pass, ShaderSplit::CurrentContext == ShaderSplit::World, fold, foldVertexColor);
		PbrPop::SpecFade::ForDraw(TheRenderManager->device, ((const PointShadowForward::RenderPassView*)Pass)->geometry, fold, foldVertexColor);
	}
	CharacterObjectReach::ForDraw(TheRenderManager->device, ((const PointShadowForward::RenderPassView*)Pass)->geometry, ShaderSplit::CurrentContext == ShaderSplit::World);	// UNOFFICIAL 2026-10-23
	DustDraw::ForDraw(TheRenderManager->device, ShaderSplit::CurrentContext == ShaderSplit::World);	// 2026-11-04: a static mesh in the world scene takes dust
	BarrelHeat::ForObject((const PointShadowForward::RenderPassView*)Pass);	// UNOFFICIAL GunFX barrel glow, per object
	BendContact::ForDraw(TheRenderManager->device, ((const PointShadowForward::RenderPassView*)Pass)->geometry, ShaderSplit::CurrentContext == ShaderSplit::World);	// UNOFFICIAL 2026-11-06 LodOnly
	ForwardShadowsForPass(Pass);
	if (LampScissor::Before((const PointShadowForward::RenderPassView*)Pass, ShaderSplit::CurrentContext == ShaderSplit::World, BarrelHeat::Active) == LampScissor::Skip) {
		HiddenObjectCulling::Abandon();	// (a lamp-only pass is never an object's first pass in practice; if it was, its query is not used)
		ClothWind::AfterDraw(TheRenderManager->device);
		FlashlightLamp::AfterPass();	// UNOFFICIAL 2026-11-04
		EnvLightFade::AfterPass();
		return;
	}
	ClothWind::PassGeometry = ((const PointShadowForward::RenderPassView*)Pass)->geometry;	// UNOFFICIAL 2026-10-19 cloth: SetShadersHook notes this object
#if NVR_TEST_TOOLS
	PassFamilyView::BeforeDraw(TheRenderManager->device, Pass, ShaderSplit::CurrentContext == ShaderSplit::World);	// UNOFFICIAL, diagnostic
	CasterView::BeforeDraw(TheRenderManager->device, Pass, ShaderSplit::CurrentContext == ShaderSplit::World);	// UNOFFICIAL 2026-10-28, diagnostic
#endif
	ReflMaterial::NeutralBegin(Pass, ((const PointShadowForward::RenderPassView*)Pass)->geometry, ShaderSplit::CurrentContext == ShaderSplit::World,
		ShaderSplit::CurrentContext == ShaderSplit::FirstPerson, RmaosDraw::ThisDraw, TheRenderManager->device);	// UNOFFICIAL 2026-11-07 perf-mirrorfix: every pass's own alpha
	ReflMaterial::ForPass(Pass, ((const PointShadowForward::RenderPassView*)Pass)->geometry, ShaderSplit::CurrentContext == ShaderSplit::World, RmaosDraw::ThisDraw, TheRenderManager->device);	// UNOFFICIAL MaterialAware reflections
	const bool countOpt = OnePassDepth::Count && GpuTimer::Enabled && ShaderSplit::CurrentContext == ShaderSplit::World &&
		OnePassDepth::IsOpt(((const PointShadowForward::RenderPassView*)Pass)->passEnum);	// UNOFFICIAL LampsDepthFirstCount (test aid)
	if (countOpt) OnePassDepth::Counts::Before(TheRenderManager->device);
	if (ShaderSplit::CurrentContext == ShaderSplit::World) TAAEffect::NoteWorldDraw();	// UNOFFICIAL 2026-10-27 RenderScale: a check of the viewport
	else if (ShaderSplit::CurrentContext == ShaderSplit::FirstPerson) TAAEffect::NoteWorldDraw(true);	// v2: the weapon into the world's rect
	// UNOFFICIAL 2026-10-28 far land: a land piece wholly past the tessellation's FadeEnd is drawn untessellated (before its bind)
	DxvkCompute::DisplaceLandPass(((const PointShadowForward::RenderPassView*)Pass)->geometry, ((const PointShadowForward::RenderPassView*)Pass)->passEnum,
		ShaderSplit::CurrentContext == ShaderSplit::World ? DxvkCompute::kDisplaceWorld :
		ShaderSplit::CurrentContext == ShaderSplit::Reflections ? DxvkCompute::kDisplaceReflection : DxvkCompute::kDisplaceOther);
	GameEyes::Before((const PointShadowForward::RenderPassView*)Pass);	// UNOFFICIAL GameShaderEyes, per draw
	DisplaceHeight::Geometry = ((const PointShadowForward::RenderPassView*)Pass)->geometry;	// UNOFFICIAL 2026-11-01 SameHeight
	AlphaHash::ForPass(((const PointShadowForward::RenderPassView*)Pass)->geometry, ShaderSplit::CurrentContext == ShaderSplit::World, TheRenderManager->device);	// UNOFFICIAL 2026-11-03 SmoothCutouts
	RenderPassStandard(Pass, Arg2, Arg3, Arg4);
	AlphaHash::AfterPass(TheRenderManager->device);
	FlashlightLamp::AfterPass();	// UNOFFICIAL 2026-11-04 the flashlight's game lamp back to its own dimmer
	EnvLightFade::AfterPass();
	DisplaceHeight::Geometry = nullptr;
	GameEyes::After();
	if (ShaderSplit::CurrentContext == ShaderSplit::World) ObjectMotion::NoteRigid(((const PointShadowForward::RenderPassView*)Pass)->geometry);	// UNOFFICIAL 2026-10-29 ObjectMotion
	if (countOpt) OnePassDepth::Counts::After();
	ManyLamps::AfterDraw((const PointShadowForward::RenderPassView*)Pass);	// UNOFFICIAL ManyLampsOnePass: the first person's lighting offset
	LampsInOnePass::GlowAfterDraw(Pass);	// UNOFFICIAL ManyLampsGlow (statistics)
	ReflMaterial::AfterPass(TheRenderManager->device);
	ReflMaterial::NeutralEnd(TheRenderManager->device);	// (perf-mirrorfix)
#if NVR_TEST_TOOLS
	FoldHighlights::AfterDraw(TheRenderManager->device);	// UNOFFICIAL, diagnostic
	PassFamilyView::After(TheRenderManager->device, ShaderSplit::CurrentContext == ShaderSplit::World);	// UNOFFICIAL, diagnostic
	CasterView::After(TheRenderManager->device);	// UNOFFICIAL 2026-10-28, diagnostic
#endif
	ClothWind::PassGeometry = nullptr;
	ClothWind::AfterDraw(TheRenderManager->device);	// UNOFFICIAL 2026-10-19: no cloth values left behind for other draws
#if NVR_TEST_TOOLS
	LampPassTest::AfterDraw(Pass, ShaderSplit::CurrentContext == ShaderSplit::World, TheRenderManager->device);	// UNOFFICIAL, diagnostic
#endif
	LampScissor::After();
	HiddenObjectCulling::After();
	PointShadowForward::NoteDraw((const PointShadowForward::RenderPassView*)Pass, ShaderSplit::CurrentContext == ShaderSplit::World);	// LogLamps diagnostic
}

void __cdecl RenderPassSkinnedHook(void* Pass, UInt32 Arg2, UInt32 Arg3, UInt32 Arg4) {
	if (OnePassDepth::InDepth) { if (GpuTimer::Enabled) OnePassDepth::StatSkinned += 1; return; }	// UNOFFICIAL LampsDepthFirst: no depth step (step 2 draws it)
#if NVR_TEST_TOOLS
	LampsInOnePass::Census::Note(Pass, ShaderSplit::CurrentContext == ShaderSplit::World);	// UNOFFICIAL, diagnostic (F10 only)
	if (ShaderSplit::CurrentContext == ShaderSplit::World || ShaderSplit::CurrentContext == ShaderSplit::FirstPerson)	// UNOFFICIAL, diagnostic: PbrPopLog
		PbrPop::PopLog::Note((const PointShadowForward::RenderPassView*)Pass, ShaderSplit::CurrentContext == ShaderSplit::FirstPerson ? 1 : 0);
	if (LampPassTest::Skip(Pass, ShaderSplit::CurrentContext == ShaderSplit::World)) return;	// UNOFFICIAL, diagnostic
	if (PassFamilyView::Skip(Pass, ShaderSplit::CurrentContext == ShaderSplit::World)) return;	// UNOFFICIAL, diagnostic: PassFamilyView
	if (CasterView::Skip(Pass, ShaderSplit::CurrentContext == ShaderSplit::World)) return;	// UNOFFICIAL 2026-10-28, diagnostic: CasterView
#endif
	if (FoldFog::Skip(Pass, ShaderSplit::CurrentContext == ShaderSplit::World)) return;	// UNOFFICIAL FoldFog: the object's lit pass fogged
	if (FoldHighlights::Skip(Pass, ShaderSplit::CurrentContext == ShaderSplit::World)) {	// UNOFFICIAL FoldHighlights: drawn with the object's lamps
#if NVR_TEST_TOOLS
		LampsInOnePass::Census::NoteSkipped(Pass);	// diagnostic (F10 only)
#endif
		return;
	}
	PassSums::BeforePass(Pass, ShaderSplit::CurrentContext == ShaderSplit::World, TheRenderManager->device);	// UNOFFICIAL ExactPassSums
	ClothWind::ForGeometry(TheRenderManager->device, ((const PointShadowForward::RenderPassView*)Pass)->geometry, false, ShaderSplit::CurrentContext == ShaderSplit::FirstPerson);	// UNOFFICIAL cloth wind
	PipboyFit::ForGeometry(((const PointShadowForward::RenderPassView*)Pass)->geometry, ShaderSplit::CurrentContext == ShaderSplit::FirstPerson);	// UNOFFICIAL 2026-11-03 PipboyFit
	if (HiddenObjectCulling::Before(Pass, ShaderSplit::CurrentContext == ShaderSplit::World)) { PipboyFit::AfterDraw(); ClothWind::AfterDraw(TheRenderManager->device); return; }	// UNOFFICIAL HiddenObjectCulling
	ReflectionProbe::OnObject(Pass, true);
	MaterialRulesDraw::ForObject((const PointShadowForward::RenderPassView*)Pass);	// UNOFFICIAL material rules
	RmaosDraw::ForObject((const PointShadowForward::RenderPassView*)Pass);	// UNOFFICIAL _rmaos maps
	{	// UNOFFICIAL SpecularFade, and FoldHighlights' switches for this draw (same constant)
		float fold = 0.0f, foldVertexColor = 0.0f;
		FoldHighlights::ForDraw(TheRenderManager->device, Pass, ShaderSplit::CurrentContext == ShaderSplit::World, fold, foldVertexColor);
		PbrPop::SpecFade::ForDraw(TheRenderManager->device, ((const PointShadowForward::RenderPassView*)Pass)->geometry, fold, foldVertexColor);
	}
	CharacterObjectReach::ForDraw(TheRenderManager->device, ((const PointShadowForward::RenderPassView*)Pass)->geometry, ShaderSplit::CurrentContext == ShaderSplit::World);	// UNOFFICIAL 2026-10-23
	DustDraw::ForDraw(TheRenderManager->device, false);	// 2026-11-04: skinned (people, creatures, armour, arms): no dust
	BarrelHeat::ForObject((const PointShadowForward::RenderPassView*)Pass);	// UNOFFICIAL GunFX barrel glow, per object
	BendContact::ForDraw(TheRenderManager->device, ((const PointShadowForward::RenderPassView*)Pass)->geometry, ShaderSplit::CurrentContext == ShaderSplit::World);	// UNOFFICIAL 2026-11-06 LodOnly
	ForwardShadowsForPass(Pass);
	ClothWind::PassGeometry = ((const PointShadowForward::RenderPassView*)Pass)->geometry;	// UNOFFICIAL 2026-10-19 cloth: SetShadersHook notes this object
#if NVR_TEST_TOOLS
	PassFamilyView::BeforeDraw(TheRenderManager->device, Pass, ShaderSplit::CurrentContext == ShaderSplit::World);	// UNOFFICIAL, diagnostic
	CasterView::BeforeDraw(TheRenderManager->device, Pass, ShaderSplit::CurrentContext == ShaderSplit::World);	// UNOFFICIAL 2026-10-28, diagnostic
#endif
	ReflMaterial::NeutralBegin(Pass, ((const PointShadowForward::RenderPassView*)Pass)->geometry, ShaderSplit::CurrentContext == ShaderSplit::World,
		ShaderSplit::CurrentContext == ShaderSplit::FirstPerson, RmaosDraw::ThisDraw, TheRenderManager->device);	// UNOFFICIAL 2026-11-07 perf-mirrorfix: every pass's own alpha
	ReflMaterial::ForPass(Pass, ((const PointShadowForward::RenderPassView*)Pass)->geometry, ShaderSplit::CurrentContext == ShaderSplit::World, RmaosDraw::ThisDraw, TheRenderManager->device);	// UNOFFICIAL MaterialAware reflections
	const bool countOpt = OnePassDepth::Count && GpuTimer::Enabled && ShaderSplit::CurrentContext == ShaderSplit::World &&
		OnePassDepth::IsOpt(((const PointShadowForward::RenderPassView*)Pass)->passEnum);	// UNOFFICIAL LampsDepthFirstCount (test aid)
	if (countOpt) OnePassDepth::Counts::Before(TheRenderManager->device);
	if (ShaderSplit::CurrentContext == ShaderSplit::World) TAAEffect::NoteWorldDraw();	// UNOFFICIAL 2026-10-27 RenderScale: a check of the viewport
	else if (ShaderSplit::CurrentContext == ShaderSplit::FirstPerson) TAAEffect::NoteWorldDraw(true);	// v2: the weapon into the world's rect
	GameEyes::Before((const PointShadowForward::RenderPassView*)Pass);	// UNOFFICIAL GameShaderEyes, per draw
	AlphaHash::ForPass(((const PointShadowForward::RenderPassView*)Pass)->geometry, ShaderSplit::CurrentContext == ShaderSplit::World, TheRenderManager->device);	// UNOFFICIAL 2026-11-03 SmoothCutouts
	RenderPassSkinned(Pass, Arg2, Arg3, Arg4);
	if (ShaderSplit::CurrentContext == ShaderSplit::World)	// UNOFFICIAL 2026-11-07 perf-mirrorfix: the hair's tint for the RT scene (core/RtHairTint.h)
		RtHairTint::NoteDraw(((const PointShadowForward::RenderPassView*)Pass)->geometry, TheRenderManager->device);
	AlphaHash::AfterPass(TheRenderManager->device);
	FlashlightLamp::AfterPass();	// UNOFFICIAL 2026-11-04 the flashlight's game lamp back to its own dimmer
	EnvLightFade::AfterPass();
	if (ShaderSplit::CurrentContext == ShaderSplit::World) ObjectMotion::NoteSkinned(((const PointShadowForward::RenderPassView*)Pass)->geometry);	// UNOFFICIAL 2026-10-29 ObjectMotion: the bones the game just made
	GameEyes::After();
	if (countOpt) OnePassDepth::Counts::After();
	ManyLamps::AfterDraw((const PointShadowForward::RenderPassView*)Pass);	// UNOFFICIAL ManyLampsOnePass: the first person's lighting offset
	LampsInOnePass::GlowAfterDraw(Pass);	// UNOFFICIAL ManyLampsGlow (statistics)
	ReflMaterial::AfterPass(TheRenderManager->device);
	ReflMaterial::NeutralEnd(TheRenderManager->device);	// (perf-mirrorfix)
#if NVR_TEST_TOOLS
	FoldHighlights::AfterDraw(TheRenderManager->device);	// UNOFFICIAL, diagnostic
	PassFamilyView::After(TheRenderManager->device, ShaderSplit::CurrentContext == ShaderSplit::World);	// UNOFFICIAL, diagnostic
	CasterView::After(TheRenderManager->device);	// UNOFFICIAL 2026-10-28, diagnostic
#endif
	ClothWind::PassGeometry = nullptr;
	ClothWind::AfterDraw(TheRenderManager->device);	// UNOFFICIAL 2026-10-19: no cloth values left behind for other draws
	HiddenObjectCulling::After();
	PointShadowForward::NoteDraw((const PointShadowForward::RenderPassView*)Pass, ShaderSplit::CurrentContext == ShaderSplit::World);	// LogLamps diagnostic
}

using SetSamplerStateMethod = HRESULT(__thiscall*)(NiDX9RenderState*, UInt32, D3DSAMPLERSTATETYPE, UInt32, UInt8);
SetSamplerStateMethod SetSamplerState = reinterpret_cast<SetSamplerStateMethod>(Hooks::SetSamplerState);

// UNOFFICIAL LampReachMargin (2026-10-13, [Main.Main.Misc]): the game attaches a lamp to an object when the lamp's reach
// overlaps the object's bound sphere (distance - Scale x bound radius < reach). Fire lights flicker: their position jitters
// by a few units every frame, so a lamp whose reach ended right at an object's bound attached and detached frame after
// frame, and its light and shadow flicked on and off over the whole object (a Gomorrah wall: gap -1 to +2 units, every
// frame, LogLamps). The bound is taken this much larger: a lamp just past the edge stays attached. Its light has faded to
// nothing at its reach, so nothing changes but the flicker (and an object may keep one more lamp, at no visible cost).
bool(__thiscall* BoundWithinLightRadius)(void*, const float*, void*, float) = (bool(__thiscall*)(void*, const float*, void*, float))0x00B9DAE0;
float LampReachMargin = 0.0f;
bool __fastcall BoundWithinLightRadiusHook(void* This, UInt32 edx, const float* Bound, void* Light, float Scale) {
	if (LampReachMargin > 0.0f && Bound && Bound[3] > 1.0f) Scale += LampReachMargin / Bound[3];
	return BoundWithinLightRadius(This, Bound, Light, Scale);
}
HRESULT __fastcall SetSamplerStateHook(NiDX9RenderState* This, UInt32 edx, UInt32 Sampler, D3DSAMPLERSTATETYPE Type, UInt32 Value, UInt8 Save) {

	auto* typeMap = reinterpret_cast<UInt16*>(0x126F92C);
	if (typeMap[Type] < 5)
		return SetSamplerState(This, Sampler, Type, Value, Save);
	return TheRenderManager->device->SetSamplerState(Sampler, Type, Value);

}

// UNOFFICIAL: in third person RenderWorldSceneGraphHook clears the depth buffer and resolves it into the view-model depth
// texture, the same empty picture every frame. It is resolved on the first such frame and once every 64 after (in case
// the texture's contents were lost); RenderFirstPersonHook, which writes real data into that texture, resets the count.
static unsigned ViewModelDepthClearedFrames = 0;

// UNOFFICIAL 2026-10-18 sandport: glass in a sandstorm ([Shaders.Sandstorm.Main] ThroughBlended). Ported from
// Physically-Based-Reloaded (Balebandro, Alejandro Martinez), GPL-3.0: their RenderBlendedHook (commit ebcf3c03). The sandstorm
// measures each pixel's dust to the depth there; glass wrote its own, so the world seen through a window (blended in before the
// storm is drawn) kept only the dust between the camera and the glass, and stayed clear. While the storm blows, in the world
// scene's blended stage (0xB650C0, BSShaderAccumulator's back-to-front lists after every opaque group), see-through surfaces draw
// without writing depth: alpha blended, and not cut out by an alpha test of any strength (fences, wire and torn sheets keep their
// depth). Water keeps its own (the water effects read its surface), except seen from underwater.
// Changed here: the world's depth surface is noted by this code itself (theirs used terrain blending's), and DrawIndexedPrimitive
// (device table slot 82) is hooked only for the length of the stage and put back after it: the native runtime has been seen to
// put its own entries back into the table (VolumetricClouds), and nothing stays in the table while no storm blows.
namespace SandstormGlass {
	static bool InWorldScene = false;
	static IDirect3DSurface9* WorldDepth = nullptr;	// the depth surface bound as the world scene starts, while the storm hides glass depth (compared only)
	static IDirect3DSurface9* GlassDepth = nullptr;	// UNOFFICIAL 2026-10-29 GlassMask: the same, while glass reflections are on (compared only)
	static bool InBlendedStage = false;
	static bool HideDepth = false, GlassOn = false;	// this stage: the storm hides glass depth / GlassMask redraws glass into its mask
	typedef HRESULT (STDMETHODCALLTYPE* DrawIndexedPrimitiveFn)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
	static DrawIndexedPrimitiveFn Next = nullptr;
	static unsigned HiddenDraws = 0;	// see-through draws that wrote no depth (logged)

	static bool IsSeeThroughPass(IDirect3DDevice9* Device, bool glassMask = false) {
		DWORD blend = FALSE, zwrite = FALSE, test = FALSE, reference = 0;
		Device->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
		Device->GetRenderState(D3DRS_ZWRITEENABLE, &zwrite);
		if (!blend || !zwrite) return false;
		Device->GetRenderState(D3DRS_ALPHATESTENABLE, &test);
		Device->GetRenderState(D3DRS_ALPHAREF, &reference);
		if (test && reference >= 32) return false;   // a cut-out (some glass tests at a near-zero reference)
		void* pass = *(void**)0x011F91E0;   // BSShaderManager::pCurrentRenderPass; its first member is the geometry
		NiGeometry* geometry = pass ? *(NiGeometry**)pass : nullptr;
		SandstormEffect* sandstorm = TheShaderManager->Effects.Sandstorm;
		// UNOFFICIAL 2026-10-25 (CharactersKeepDepth): ported from Physically-Based-Reloaded (Balebandro, Alejandro Martinez),
		// GPL-3.0, commit 432ed1e6. Characters and creatures keep their depth, or they get the dust behind them and look
		// see-through: skinned meshes (hair, blended clothing and armour), and unskinned ones hung on a skeleton's bones (hats,
		// helmets, goggles). Changed here: behind its own switch, for A/B.
		// UNOFFICIAL 2026-10-29 GlassMask: characters are never glass, whatever the storm's switch.
		if (geometry && (glassMask || (sandstorm && sandstorm->Settings.CharactersKeepDepth))) {
			if (geometry->skinInstance) return false;
			NiNode* node = geometry->m_parent;
			for (int depth = 0; node && depth < 32; ++depth, node = node->m_parent)
				if (node->m_pcName && !_strnicmp(node->m_pcName, "Bip01", 5)) return false;
		}
		// Water keeps its depth, the water effects read its surface; except from underwater, where the world above is seen
		// through it as through glass and must get the dust of its own distance.
		if (geometry && !(sandstorm && sandstorm->Underwater))
			if (NiShadeProperty* shade = (NiShadeProperty*)geometry->GetProperty(NiProperty::kType_Shade))
				if (shade->m_eShaderType == NiShadeProperty::kProp_Water) return false;
		return true;
	}

	static HRESULT STDMETHODCALLTYPE DrawHook(IDirect3DDevice9* Device, D3DPRIMITIVETYPE Type, INT Base, UINT MinIndex, UINT Vertices, UINT Start, UINT Count) {
		// UNOFFICIAL 2026-10-29 GlassMask (core/GlassMask.h): no storm hiding glass depth, glass reflections on: the see-through draw as
		// the game does it, then once more into the glass mask.
		if (InBlendedStage && !HideDepth && GlassOn && IsSeeThroughPass(Device, true)) {
			const HRESULT result = Next(Device, Type, Base, MinIndex, Vertices, Start, Count);
			if (SUCCEEDED(result)) GlassMask::Redraw(Device, [&]() { return Next(Device, Type, Base, MinIndex, Vertices, Start, Count); });
			return result;
		}
		if (InBlendedStage && HideDepth && IsSeeThroughPass(Device)) {
			HiddenDraws++;
			Device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);   // set back at once: the render state cache never sees it
			const HRESULT result = Next(Device, Type, Base, MinIndex, Vertices, Start, Count);
			Device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
			return result;
		}
		return Next(Device, Type, Base, MinIndex, Vertices, Start, Count);
	}

	static void BeginWorld() {
		InWorldScene = true;
		WorldDepth = nullptr;
		GlassDepth = nullptr;
		GlassMask::BeginWorld();	// UNOFFICIAL 2026-10-29
		SandstormEffect* sandstorm = TheShaderManager->Effects.Sandstorm;
		const bool storm = sandstorm && sandstorm->HidesBlendedDepth(), glass = GlassMask::Wanted();
		if (!storm && !glass) return;
		IDirect3DSurface9* depth = nullptr;
		TheRenderManager->device->GetDepthStencilSurface(&depth);
		if (storm) WorldDepth = depth;
		if (glass) GlassDepth = depth;
		if (depth) depth->Release();
	}

	static void EndWorld() {
		InWorldScene = false;
		WorldDepth = nullptr;
		GlassDepth = nullptr;
	}
}

void (__thiscall* RenderBlended)(void*, UInt32) = (void (__thiscall*)(void*, UInt32))Hooks::RenderBlended;
void __fastcall RenderBlendedHook(void* This, UInt32 edx, UInt32 Arg) {
	using namespace SandstormGlass;
	bool hide = false, glass = false;
	if (InWorldScene && (WorldDepth || GlassDepth)) {
		// The world's own depth surface, not a reflection's or cube map's.
		IDirect3DSurface9* depth = nullptr;
		TheRenderManager->device->GetDepthStencilSurface(&depth);
		hide = WorldDepth && depth == WorldDepth;
		// UNOFFICIAL 2026-10-29 GlassMask: glass reflections need the glass depth, so not while the storm hides it.
		glass = !hide && GlassDepth && depth == GlassDepth && GlassMask::BeforeStage(TheRenderManager->device);
		if (depth) depth->Release();
	}
	if (!hide && !glass) { RenderBlended(This, Arg); return; }
	HideDepth = hide;
	GlassOn = glass;
	void** table = *(void***)TheRenderManager->device;
	bool hooked = false;
	if (table[82] != (void*)&DrawHook) {
		DWORD old = 0;
		if (VirtualProtect(&table[82], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
			Next = (DrawIndexedPrimitiveFn)table[82];
			table[82] = (void*)&DrawHook;
			VirtualProtect(&table[82], sizeof(void*), old, &old);
			hooked = true;
		}
	}
	const unsigned before = HiddenDraws;
	InBlendedStage = table[82] == (void*)&DrawHook;
	RenderBlended(This, Arg);
	InBlendedStage = false;
	if (GlassOn) GlassMask::AfterStage(TheRenderManager->device);	// UNOFFICIAL 2026-10-29
	HideDepth = GlassOn = false;
	if (hooked && table[82] == (void*)&DrawHook) {
		DWORD old = 0;
		if (VirtualProtect(&table[82], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
			table[82] = (void*)Next;
			VirtualProtect(&table[82], sizeof(void*), old, &old);
		}
	}
	static unsigned logged = 0;
	if (logged < 3 && HiddenDraws > before) {
		logged++;
		Logger::Log("UNOFFICIAL Sandstorm ThroughBlended: %u see-through blended draws wrote no depth in this world scene's blended stage", HiddenDraws - before);
	}
}

using RenderWorldSceneGraphMethod = void(__thiscall*)(Main*, Sun*, UInt8, UInt8, UInt8);
RenderWorldSceneGraphMethod RenderWorldSceneGraph = reinterpret_cast<RenderWorldSceneGraphMethod>(Hooks::RenderWorldSceneGraph);

// Resolves the viewmodel depth buffer into its own texture (third person, and the end of the first-person pass).
static void ResolveViewModelDepth() {
	TheRenderManager->ResolveDepthBuffer(TheTextureManager->DepthTextureViewModel);
}
void __fastcall RenderWorldSceneGraphHook(Main* This, UInt32 edx, Sun* SkySun, UInt8 IsFirstPerson, UInt8 WireFrame, UInt8 Arg4) {
	SessionLog::StageScope stage("the world");
	EndPreSceneTimer();
	WorldRenderedThisFrame = true;
	if (!FrameWorldArgsKnown) { FrameWorldArgsKnown = true; FrameWorldArgs[0] = IsFirstPerson; FrameWorldArgs[1] = WireFrame; FrameWorldArgs[2] = Arg4; }
	FrameWorldCalls++;
	// UNOFFICIAL TAA (off by default): the sub-pixel jitter covers the world scene and nothing else -- see
	// TAAEffect::BeginJitter. A no-op unless TAA resolves this frame.
	TAAEffect* TAA = TheShaderManager->Effects.TAA;
	// UNOFFICIAL 2026-10-27 RenderScale ([Shaders.TAA.FSR], TAAUpscale.cpp): also sets the world camera's viewport to the lower internal
	// resolution when that is on (the frame's first world scene only). A no-op otherwise.
	if (TAA) TAA->BeginJitter(FrameWorldCalls == 1);
	// UNOFFICIAL terrain blending (off by default): the world's depth is copied as the first land shader of this scene binds.
	TerrainShaders::BeginWorldScene();
	ParticleShaders::BeginWorldScene();	// UNOFFICIAL 2026-11-04 soft particles: the same, at the first alpha-blended NoLighting draw
	if (FrameWorldCalls == 1) ShadowAtlas::WorldStart(TheRenderManager->device);	// UNOFFICIAL 2026-10-30 ShadowAtlas WorldStartConvert (test)
	// UNOFFICIAL 2026-11-07 SameFrame RT lamp shadows: this frame's trace before the world's first draw (after the jitter: the rays use it)
	if (FrameWorldCalls == 1) RtLampShadows::TraceBeforeWorld(TheRenderManager->device);
	{
		// Game geometry drawn with NVR's replacement shaders, including per-object sun shadows.
		static GpuTimer worldTimer("World scene (game)");
		GpuProfileScope gpu(worldTimer, TheRenderManager->device);
		ShaderSplit::BeginContext(ShaderSplit::World);
		HiddenObjectCulling::BeginWorld();
		GunFXSwitches::Update();
		HandBlood::Update();	// UNOFFICIAL: blood on the first-person weapon and gloves (once a frame)
		BarrelHaze::Reset();
		BarrelHeat::BeginFrame();
		BarrelHeat::BeginWorldProbe();
		// UNOFFICIAL the skin blur: its targets are bound only for skin drawn inside the world scene.
		if (TheShaderManager->Effects.SkinScattering) TheShaderManager->Effects.SkinScattering->InWorldScene = true;
		SandstormGlass::BeginWorld();	// UNOFFICIAL sandport: ThroughBlended (RenderBlendedHook)
		// UNOFFICIAL 2026-10-20 MaterialAware reflections (core/ReflMaterial.h): blended passes keep off the scene's alpha.
		ReflMaterial::BeginWorld(TheRenderManager->renderState, TheRenderManager->device, ReflMaterial::CompiledIn() && TheSettingManager->GetSettingI("Shaders.Reflections.Main", "MaterialAware") > 0);
		// UNOFFICIAL 2026-11-03 SmoothCutouts (core/AlphaHash.h): alpha-tested objects hashed while TAA / FSR / DLSS jitter this scene.
		AlphaHash::BeginWorld(TheRenderManager->device, FrameWorldCalls == 1, TAA && TAA->IsJitterActive());
		RenderWorldSceneGraph(This, SkySun, IsFirstPerson, WireFrame, Arg4);
		AlphaHash::EndWorld(TheRenderManager->device);
		ReflMaterial::EndWorld(TheRenderManager->renderState, TheRenderManager->device);
		SandstormGlass::EndWorld();
		PassSums::EndWorld(TheRenderManager->device);	// UNOFFICIAL ExactPassSums: a run still open at the world's end is folded in here
		// UNOFFICIAL the skin blur: the SkinShader hooks unbind the targets after every skin draw; this only guarantees
		// nothing after the world scene ever draws into it.
		if (TheShaderManager->Effects.SkinScattering) {
			TheShaderManager->Effects.SkinScattering->InWorldScene = false;
			TheShaderManager->Effects.SkinScattering->Unbind();
		}
		BarrelHeat::EndWorldProbe();
		BarrelHeat::EndContext();
		HiddenObjectCulling::EndWorld();
		ShaderSplit::EndContext();
	}

	TerrainShaders::EndWorldScene();
	ParticleShaders::EndWorldScene();
	// After the material pass, not before: it redraws world geometry over the scene, and must land on exactly the same
	// jittered pixels the world did.
	if (TAA) TAA->EndJitter();
	// UNOFFICIAL 2026-10-27 RenderScale: a world drawn at the lower internal resolution is upscaled by FSR 2 into its own targets
	// (colour + depth buffer) here, before the game's refraction pass, the first-person weapon and the depth resolve below, so
	// everything after sees a full-resolution frame. A no-op unless RenderScale drew this world scene small.
	if (TAA) TAA->UpscaleWorld();

	const bool pipboyOpen = InterfaceManager->IsPipBoyOpen();
	const bool pipboyLive = TheGameMenuManager->IsLiveMenu && (TheGameMenuManager->IsLiveMenu(Menu::kMenuType_BigFour, false, false) == GameMenuManager::MenuPauseState::MENU_LIVE);

	static GpuTimer depthResolveTimer("Depth resolves");
	GpuProfileScope gpuResolve(depthResolveTimer, TheRenderManager->device);
	if (!pipboyOpen || pipboyLive) {
		// UNOFFICIAL 2026-11-04 SkyCoverage (core/SkyCoverage.h): with the game's MSAA, each pixel's sky share from the world's depth samples,
		// after every world draw (the distant land and objects come after the main list) and before the resolve below.
		if (FrameWorldCalls == 1) SkyCoverage::Draw(TheRenderManager->device);
		TheRenderManager->ResolveDepthBuffer(TheTextureManager->DepthTexture);	// the world buffer is left alone while the Pip-Boy is out
		// UNOFFICIAL 2026-10-30 ReactiveMask (core/ReactiveMask.h): what was drawn over the main list, now that this frame's world depth is
		// readable (before the first-person weapon and the RenderScale v2 upscale; a v1 upscale has read FSR's inputs already)
		if (FrameWorldCalls == 1) ReactiveMask::Build(TheRenderManager->device);
	}

	if (IsFirstPerson == 0) {
		// clear the viewmodel depth buffer
		TheRenderManager->Clear(nullptr, NiRenderer::kClear_ZBUFFER);
		if (ViewModelDepthClearedFrames == 0 || (ViewModelDepthClearedFrames & 63) == 0)	// UNOFFICIAL, see above
				ResolveViewModelDepth();
			ViewModelDepthClearedFrames++;
	}
}

using RenderFirstPersonMethod = void(__thiscall*)(Main*, NiDX9Renderer*, NiGeometry*, Sun*, BSRenderedTexture*);
RenderFirstPersonMethod RenderFirstPerson = reinterpret_cast<RenderFirstPersonMethod>(Hooks::RenderFirstPerson);
void __fastcall RenderFirstPersonHook(Main* This, UInt32 edx, NiDX9Renderer* Renderer, NiGeometry* Geo, Sun* SkySun, BSRenderedTexture* RenderedTexture) {
	SessionLog::StageScope stage("first person");
	// Clear the depth buffer before rendering first person model to prevent clipping with world objects & other artefacts
	static GpuTimer firstPersonTimer("First person (game)");
	GpuProfileScope gpu(firstPersonTimer, TheRenderManager->device);
	FrameFirstPersonCalls++;
	TheRenderManager->Clear(nullptr, NiRenderer::kClear_ZBUFFER);
	//ThisCall(0x00874C10, Global);
	ShaderSplit::BeginContext(ShaderSplit::FirstPerson);
	// UNOFFICIAL: the first-person model's metal strength ([Shaders.PBR.Metal] FirstPerson) for this pass only.
	PBRShaders* pbr = TheShaderManager->Shaders.PBR;
	if (pbr) pbr->BeginFirstPerson(TheRenderManager->device);
	// UNOFFICIAL 2026-11-04 ViewModelShadows (ShadowManager::RenderViewModelShadowMap): the arms and weapon alone into the character map,
	// between draws, before the game's first-person render. They are posed for the frame by now (moved to the origin and updated in
	// 0x874C10, which also queued their cull task); the game clears the root's APP_CULLED only inside that render, so it is cleared here
	// for the map's walk and put back. The map's constants go to the device at once (a shader the game keeps bound from the world render
	// would not upload them; c206-c209 and c213-c214, CharacterMapShadow's, outdoors only) and are put back after the render.
	ShadowsExteriorEffect* shadows = TheShaderManager->Effects.ShadowsExteriors;
	bool viewModelShadows = false;
	D3DXMATRIX savedCharacterCameraToLight;
	D3DXVECTOR4 savedCharacterCenter, savedCharacterData;
	if (shadows && Player && Player->firstPersonNiNode && Player->IsFirstPerson()) {
		savedCharacterCameraToLight = shadows->Constants.CharacterCameraToLight;
		savedCharacterCenter = shadows->Constants.CharacterCenter;
		savedCharacterData = shadows->Constants.CharacterData;
		NiNode* viewModel = Player->firstPersonNiNode;
		const UInt32 culled = viewModel->m_flags & NiAVObject::NiFlags::APP_CULLED;
		viewModel->m_flags &= ~NiAVObject::NiFlags::APP_CULLED;
		viewModelShadows = TheShadowManager->RenderViewModelShadowMap(This->camera);
		viewModel->m_flags |= culled;
		if (viewModelShadows) {
			IDirect3DDevice9* device = TheRenderManager->device;
			device->SetPixelShaderConstantF(206, (const float*)&shadows->Constants.CharacterCameraToLight, 4);
			device->SetPixelShaderConstantF(213, (const float*)&shadows->Constants.CharacterCenter, 1);
			device->SetPixelShaderConstantF(214, (const float*)&shadows->Constants.CharacterData, 1);
		}
	}
	PbrPop::ActorPasses::FirstPersonNow = 1;	// UNOFFICIAL ActorsInOnePass: the first-person model's passes are built in here
	// UNOFFICIAL 2026-11-07 (ptaaa-firstperson) PTViewModel: the model's colour, normals and material into the G-buffer, for the path
	// tracer to light it (core/CapturedNormals.h BeginViewModelSpan; nothing unless the path tracer asked for it this frame)
	CapturedNormals::BeginViewModelSpan(TheRenderManager->device);
	// UNOFFICIAL 2026-11-07 perf-mirrorfix: the arms and gun write their own alpha over the world's materials (core/ReflMaterial.h)
	ReflMaterial::BeginFirstPerson(TheRenderManager->renderState, TheRenderManager->device, ReflMaterial::CompiledIn() && TheSettingManager->GetSettingI("Shaders.Reflections.Main", "MaterialAware") > 0);
	RenderFirstPerson(This, Renderer, Geo, SkySun, RenderedTexture);
	ReflMaterial::EndFirstPerson(TheRenderManager->renderState, TheRenderManager->device);
	CapturedNormals::EndViewModelSpan(TheRenderManager->device, TheFrameRateManager->FrameNumber);
	PbrPop::ActorPasses::FirstPersonNow = 0;
	SkyCoverage::DrawViewModel(TheRenderManager->device);	// UNOFFICIAL 2026-11-04: the weapon out of the sky share (core/SkyCoverage.h)
	if (viewModelShadows) {
		// The map holds the viewmodel now, not what the world's draws read: nothing later this frame reads it (w 0).
		shadows->Constants.CharacterCameraToLight = savedCharacterCameraToLight;
		shadows->Constants.CharacterCenter = savedCharacterCenter;
		shadows->Constants.CharacterData = savedCharacterData;
		shadows->Constants.CharacterData.w = 0.0f;
	}
	if (TheShaderManager->Effects.SkinScattering) TheShaderManager->Effects.SkinScattering->Unbind();	// UNOFFICIAL the skin blur
	if (pbr) pbr->EndFirstPerson(TheRenderManager->device);
	BarrelHeat::EndContext();
	ShaderSplit::EndContext();
	ResolveViewModelDepth();
	ViewModelDepthClearedFrames = 0;	// UNOFFICIAL: the next third-person frame resolves the cleared buffer again
}

using RenderReflectionsMethod = void(__thiscall*)(WaterManager*, NiCamera*, ShadowSceneNode*);
RenderReflectionsMethod RenderReflections = reinterpret_cast<RenderReflectionsMethod>(Hooks::RenderReflections);
void __fastcall RenderReflectionsHook(WaterManager* This, UInt32 edx, NiCamera* Camera, ShadowSceneNode* SceneNode) {
	SessionLog::StageScope stage("the water reflection");
	ReflectionProbe::HookCamera = Camera;
	// UNOFFICIAL 2026-10-31 RT water ([Shaders.Reflections.RT] WaterSkipPlanar): the water shaders read the RT reflection this frame,
	// not the planar map, so the planar pass is not drawn (ReflectionsEffect::UpdateWaterConstants decides, once a frame).
	// (2026-11-05 rtfull-lake3) but not on every frame: the pass also draws maps the water shaders read besides the reflection (the
	// water's depth: its see-through depth and shore), and skipped from the first frame after a load they were never drawn: streaky,
	// see-through water with the lake floor showing (og-lake). Drawn on every WaterPlanarRefresh-th frame (and the first), else skipped.
	if (ReflectionsEffect* reflections = TheShaderManager->Effects.Reflections) {
		if (reflections->WaterSkipsPlanar()) {
			static UInt32 lastDrawn = 0, refreshFrame = 0xFFFFFFFFu;
			static int refresh = 8;
			const UInt32 frame = TheFrameRateManager->FrameNumber;
			if (refreshFrame != frame) { refreshFrame = frame; refresh = TheSettingManager->GetSettingI("Shaders.Reflections.RT", "WaterPlanarRefresh"); }
			if (refresh <= 0 || frame - lastDrawn < (UInt32)refresh) {
				static bool logged = false;
				if (!logged) { logged = true; Logger::Log("UNOFFICIAL Reflections: RT water: the planar water reflection pass is skipped while RT holds the water (drawn every %d frames for the water's other maps).", refresh); }
				return;
			}
			lastDrawn = frame;
		}
	}
	if (!TheSettingManager->SettingsMain.Main.ForceReflections) {
		// Hooked for profiling in this mode: the game's reflection pass, unchanged unless CheapReflections is on.
		static GpuTimer reflectionsTimer("Water reflections (game)");
		static CpuTimer reflectionsCpuTimer("Water reflections (CPU)");
		CpuProfileScope cpu(reflectionsCpuTimer);
		GpuProfileScope gpu(reflectionsTimer, TheRenderManager->device);
		CheapReflectionScope cheap;
		UnderwaterTerrainReflectionScope underwaterTerrain;
		ShaderSplit::BeginContext(ShaderSplit::Reflections);
		RenderReflections(This, Camera, SceneNode);
		ShaderSplit::EndContext();
		if (VolumetricCloudsEffect* clouds = TheShaderManager->Effects.VolumetricClouds) clouds->EndReflectionPass(TheRenderManager->device);	// UNOFFICIAL
		return;
	}
	
	D3DXVECTOR4* shadowParams = &TheShaderManager->Effects.ShadowsExteriors->Constants.Data;
	const float savedShadow = shadowParams->x;

	D3DXVECTOR4* parallaxParams = &TheShaderManager->Shaders.Terrain->ParallaxConstants.Data;
	const float savedParallax = parallaxParams->x;

	if (DWNode::Get()) DWNode::AddNode("BEGIN REFLECTIONS RENDERING", NULL, NULL);
	shadowParams->x = -1.0f;	// no sun shadows in the water reflection: its geometry is drawn with the scene's own shaders
	parallaxParams->x = 0;
	{
		// ForceReflections renders the full world again into the water reflection map.
		static GpuTimer reflectionsTimer("Water reflections (game)");
		static CpuTimer reflectionsCpuTimer("Water reflections (CPU)");
		CpuProfileScope cpu(reflectionsCpuTimer);
		GpuProfileScope gpu(reflectionsTimer, TheRenderManager->device);
		CheapReflectionScope cheap;
		UnderwaterTerrainReflectionScope underwaterTerrain;
		ShaderSplit::BeginContext(ShaderSplit::Reflections);
		RenderReflections(This, Camera, SceneNode);
		ShaderSplit::EndContext();
		if (VolumetricCloudsEffect* clouds = TheShaderManager->Effects.VolumetricClouds) clouds->EndReflectionPass(TheRenderManager->device);	// UNOFFICIAL
	}
	shadowParams->x = savedShadow;
	parallaxParams->x = savedParallax;
	if (DWNode::Get()) DWNode::AddNode("END REFLECTIONS RENDERING", NULL, NULL);
}

using RenderPipboyMethod = void(__thiscall*)(Main*, NiGeometry*, NiDX9Renderer*);
RenderPipboyMethod RenderPipboy = reinterpret_cast<RenderPipboyMethod>(Hooks::RenderPipboy);
void __fastcall RenderPipboyHook(Main* This, UInt32 edx, NiGeometry* Geo, NiDX9Renderer* Renderer) {
	const float fov = Player->firstPersonFoV;
	WorldSceneGraph->UpdateParticleShaderFoV(fov);
//	Player->SetFoV(Player->firstPersonFoV);
	RenderPipboy(This, Geo, Renderer);
}

using GetWaterHeightLODMethod = float(__thiscall*)(TESWorldSpace*);
GetWaterHeightLODMethod GetWaterHeightLOD = reinterpret_cast<GetWaterHeightLODMethod>(Hooks::GetWaterHeightLOD);
float __fastcall GetWaterHeightLODHook(TESWorldSpace* This, UInt32 edx) {
	
	if (*(void**)This == reinterpret_cast<void*>(0x0103195C))
		return TheShaderManager->Shaders.Water->Constants.Default.waterSettings.x;
	return This->waterHeight;

}

// UNOFFICIAL 2026-10-30, ported from Balebandro/Physically-Based-Reloaded 1ce5d62 (Alejandro Martinez):
// TESWaterSystem::IsWaterNodeVisible takes the first child of a water reference's 3D (NiNode::GetAt(0)) without
// checking it has any. A LOD water reference whose 3D is a node with no children array yet (NiNode +0xA0 null)
// crashed the game there, from UpdateLODWater in the water reflection pass.
// Such water is not visible: false, as for one with no 3D at all.
bool (__thiscall* IsWaterNodeVisible)(void*, void*, void*) = (bool (__thiscall*)(void*, void*, void*))Hooks::IsWaterNodeVisible;
bool __fastcall IsWaterNodeVisibleHook(void* This, UInt32 edx, void* Ref, void* Camera) {

	if (Ref) {
		void* Node = ((void* (__thiscall*)(void*))(*(void***)Ref)[0x1D0 / 4])(Ref);   // TESObjectREFR::Get3D
		if (Node) {
			const bool IsNode = ((void* (__thiscall*)(void*))(*(void***)Node)[3])(Node) != NULL;   // NiObject::IsNode
			if (!IsNode || !*(void**)((UInt8*)Node + 0xA0)) return false;
		}
	}
	return (*IsWaterNodeVisible)(This, Ref, Camera);

}

bool renderedSkipToggle = false;
bool lockpickSkipToggle = false;

// UNOFFICIAL 2026-10-27 RenderScale v2: a frame drawn small (world + weapon) is upscaled before the game's image space, also on the
// paths below that skip NVR's effects. Surface: the scene target when the caller has it, else the game's source target's.
static void FlushRenderScale(BSRenderedTexture* SourceTarget, IDirect3DSurface9* Surface = nullptr) {
	TAAEffect* taa = TheShaderManager->Effects.TAA;
	if (!taa || !taa->UpscalePending()) return;
	IDirect3DSurface9* own = nullptr;
	if (!Surface && SourceTarget && SourceTarget->GetD3DTexture(0)) SourceTarget->GetD3DTexture(0)->GetSurfaceLevel(0, &own);
	taa->UpscaleAfterEffects(Surface ? Surface : own);
	if (own) own->Release();
}

using ProcessImageSpaceShadersMethod = void(__cdecl*)(NiDX9Renderer*, BSRenderedTexture*, BSRenderedTexture*);
ProcessImageSpaceShadersMethod ProcessImageSpaceShaders = reinterpret_cast<ProcessImageSpaceShadersMethod>(Hooks::ProcessImageSpaceShaders);
void __cdecl ProcessImageSpaceShadersHook(NiDX9Renderer* Renderer, BSRenderedTexture* SourceTarget, BSRenderedTexture* DestinationTarget) {
	SessionLog::StageScope stage("effects");
	if (TheShaderManager->Effects.SkinScattering) TheShaderManager->Effects.SkinScattering->Unbind();	// UNOFFICIAL the skin blur
	bool liveRendered = false;	// FORenderedMenu, FOPipBoyManager
	bool live3D = false;	// normal menus, but 3D (lockpick etc)
	if (TESMain::IsMenuBackgroundReady() && TheGameMenuManager->IsLiveMenu && InterfaceManager->currentMode != 1) {
		const bool lockpickLive = LockPickMenu::GetSingleton() && TheGameMenuManager->IsLiveMenu(Menu::kMenuType_LockPick, false, false) == GameMenuManager::MENU_LIVE;
		const bool pipboyLive = InterfaceManager->IsPipBoyOpen() && TheGameMenuManager->IsLiveMenu(Menu::kMenuType_BigFour, false, false) == GameMenuManager::MenuPauseState::MENU_LIVE;
		const bool renderedLive = InterfaceManager->pRenderedMenu && TheGameMenuManager->IsLiveMenu(InterfaceManager->menuStack[0], false, false) == GameMenuManager::MenuPauseState::MENU_LIVE;
		liveRendered = pipboyLive || renderedLive;
		live3D = lockpickLive;
	}

	// UNOFFICIAL 2026-11-01 LockpickEffects (tester report: strange visuals in the lockpick view): while the lockpick screen is open, live
	// (a real-time-menus mod: the world keeps drawing behind the lock) or paused, the frame gets the game's own image space only.
	// true = as before (NVR's effects on the world pass behind a live lock).
	// The game's image space alone, after the RenderScale upscale (UNOFFICIAL 2026-10-27 RenderScale v2)
	auto gameImageSpaceOnly = [&]() {
		FlushRenderScale(SourceTarget);
		ProcessImageSpaceShaders(Renderer, SourceTarget, DestinationTarget);
	};
	if (LockPickMenu::GetSingleton() && InterfaceManager->IsActive(Menu::kMenuType_LockPick) && !TheSettingManager->GetSettingI("Main.Main.Misc", "LockpickEffects")) {
		lockpickSkipToggle = false;
		gameImageSpaceOnly();
		return;
	}

	// A live 3D menu draws every other frame: the frame between gets the game's image space alone.
	if (live3D) {
		if (lockpickSkipToggle) {
			lockpickSkipToggle = false;
			gameImageSpaceOnly();
			return;
		}
		lockpickSkipToggle = true;
	}
	else
		lockpickSkipToggle = false;

	// The same for a live rendered menu (the Pip-Boy and the like).
	if (liveRendered) {
		if (!renderedSkipToggle) {
			renderedSkipToggle = true;
			gameImageSpaceOnly();
			return;
		}
		renderedSkipToggle = false;
	}
	else
		renderedSkipToggle = false;
	FrameImageSpaceCalls++;
	if (WorldSceneGuardActive()) {
		// No world scene has been rendered for several frames, so the depth buffers NVR's effects need are
		// stale and they would darken the image. Let the game's own image space run on its own.
		static bool announced = false;
		if (!announced) { Logger::Log("UNOFFICIAL world scene guard: no world scene render for %u frames, Vegas Shaders effects skipped until it returns.", WorldMissStreak); announced = true; }
#if NVR_TEST_TOOLS
		SettingShots::BeginFrame();	// UNOFFICIAL 2026-10-20, test only: the shots go on over a menu's still background (Pip-Boy pictures)
#endif
		// UNOFFICIAL 2026-10-20 MenuSceneGuard: NVR's bloom is not drawn in these frames, so the HDR blend (ISHDRBLENDINSHADERCIN) took
		// the last one it made, frozen (in the Pip-Boy: of the world before it opened, or of the Pip-Boy part way through opening). It
		// takes the game's own bloom of this frame instead, as with NVR's bloom off; NVR's flag comes back with its next frame.
		BloomEffect* bloom = TheShaderManager->Effects.Bloom;
		const float bloomFlag = bloom ? bloom->Constants.ExtraData.x : 0.0f;
		if (bloom && TheSettingManager->SettingsMain.Main.MenuSceneGuard) bloom->Constants.ExtraData.x = 0.0f;
		FlushRenderScale(SourceTarget);	// UNOFFICIAL 2026-10-27 RenderScale v2
		ProcessImageSpaceShaders(Renderer, SourceTarget, DestinationTarget);
		if (bloom) bloom->Constants.ExtraData.x = bloomFlag;
#if NVR_TEST_TOOLS
		if (!DestinationTarget && TheRenderManager->currentRTGroup) SettingShots::Capture(TheRenderManager->currentRTGroup->RenderTargets[0]->data->Surface);
#endif
		return;
	}

	IDirect3DDevice9* device = TheRenderManager->device;
	NiDX9RenderState* renderState = TheRenderManager->renderState;
	IDirect3DSurface9* gameSurface = nullptr;
	IDirect3DSurface9* outputSurface = nullptr;
	const auto& mainSettings = TheSettingManager->SettingsMain.Main;

	PrepareSceneCameras();
#if NVR_TEST_TOOLS
	EffectSelfTest::BeginFrame();	// UNOFFICIAL, diagnostic: NeonSpillSelfTest (sets its configuration after the settings are read)
	SettingShots::BeginFrame();		// UNOFFICIAL, test only: the auto-test's A/B shots (SettingShots.h)
	if (SettingShots::WorldCapture && SourceTarget) {	// UNOFFICIAL, test only: 'world' shots, the game's scene buffer before NVR's effects
		IDirect3DSurface9* scene = nullptr;
		if (SUCCEEDED(SourceTarget->GetD3DTexture(0)->GetSurfaceLevel(0, &scene)) && scene) { SettingShots::CaptureWorld(device, scene); scene->Release(); }
	}
#endif

	if (SourceTarget && mainSettings.RenderPreTonemapping) {
		SourceTarget->GetD3DTexture(0)->GetSurfaceLevel(0, &gameSurface);	// the game's render target, as a surface

		// Disable render state settings that create artefacts
		// Render states that create artefacts, set in this order (POINTSIZE last: it is the flicker fix)
		static const struct { D3DRENDERSTATETYPE state; UInt32 value; } preTonemapStates[] = {
			{ D3DRS_ZENABLE, D3DZB_FALSE }, { D3DRS_ZWRITEENABLE, D3DZB_FALSE }, { D3DRS_STENCILENABLE, D3DZB_FALSE },
			{ D3DRS_STENCILMASK, D3DZB_FALSE }, { D3DRS_STENCILWRITEMASK, 255 }, { D3DRS_STENCILREF, D3DZB_FALSE },
			{ D3DRS_STENCILPASS, D3DSTENCILOP_KEEP }, { D3DRS_STENCILFUNC, D3DCMP_ALWAYS }, { D3DRS_COLORWRITEENABLE, 15 },
			{ D3DRS_ALPHATESTENABLE, D3DZB_FALSE }, { D3DRS_ALPHABLENDENABLE, D3DZB_FALSE }, { D3DRS_ALPHAREF, 0 },
			{ D3DRS_SRCBLEND, D3DBLEND_ONE }, { D3DRS_DESTBLEND, D3DBLEND_ONE }, { D3DRS_NORMALIZENORMALS, D3DZB_FALSE },
			{ D3DRS_POINTSIZE, 810365505 },	// fix flickering linked to alpha somehow
		};
		for (const auto& entry : preTonemapStates) renderState->SetRenderState(entry.state, entry.value, RenderStateArgs);

		// UNOFFICIAL 2026-10-27 RenderScale v2 (TAAUpscale.cpp): in a frame drawn small, NVR's effects run in its rect.
		if (TheShaderManager->Effects.TAA) TheShaderManager->Effects.TAA->UpscaleBeginEffects();
		TheShaderManager->RenderEffectsPreTonemapping(gameSurface);

	}
	// UNOFFICIAL 2026-10-27 RenderScale v2: the upscale after them (also when the chain did not run); a no-op unless the frame waits for it.
	FlushRenderScale(SourceTarget, gameSurface);
#if NVR_TEST_TOOLS
	if (SettingShots::WorldCapture && SourceTarget) {	// UNOFFICIAL 2026-11-02, test only: the scene buffer as the game's image space gets it
		IDirect3DSurface9* scene = nullptr;
		if (SUCCEEDED(SourceTarget->GetD3DTexture(0)->GetSurfaceLevel(0, &scene)) && scene) { SettingShots::CapturePreImageSpace(TheRenderManager->device, scene); scene->Release(); }
	}
#endif

	{
		static GpuTimer imageSpaceTimer("Game image space");
		GpuProfileScope gpu(imageSpaceTimer, device);
		ProcessImageSpaceShaders(Renderer, SourceTarget, DestinationTarget);
	}

	if (DestinationTarget == nullptr && TheRenderManager->currentRTGroup) {
		outputSurface = TheRenderManager->currentRTGroup->RenderTargets[0]->data->Surface;
		if (!mainSettings.RenderPreTonemapping) TheShaderManager->RenderEffectsPreTonemapping(outputSurface);
		// UNOFFICIAL 2026-10-26 HDR output (core\HdrOutput.h; off by default, needs NVR DXVK): settings, then the highlight gain from
		// the scene before tonemapping and the tonemapper's SDR result (outputSurface still holds it here).
		HdrOutput::BeginFrame(device, SourceTarget ? SourceTarget->GetD3DTexture(0) : nullptr);
		if (HdrOutput::S.Active) {
			static GpuTimer hdrGainTimer("HDR gain pass");
			GpuProfileScope gpu(hdrGainTimer, device);
			HdrOutput::RenderGain(device, outputSurface);
		}
		TheShaderManager->RenderEffects(outputSurface);
		// UNOFFICIAL 2026-10-26 pixel inspector ([Main.Develop.PixelInspector], off by default, needs NVR DXVK 1.5-debug): the final
		// picture before the HUD, then this frame's records go to the non-waiting readback and the settings are read for the next one.
		PixelInspector::CaptureIf("Final (before the HUD)", outputSurface);
		PixelInspector::EndFrame();
#if NVR_TEST_TOOLS
		EffectSelfTest::Capture(EffectSelfTest::Final, outputSurface);	// UNOFFICIAL, diagnostic
		SettingShots::Land::OnPicture(device, outputSurface);	// UNOFFICIAL 2026-10-30, test only: 'landflicker' (before Capture, which may end the shot)
		SettingShots::Capture(outputSurface);	// UNOFFICIAL, test only
#endif
		ReShadeIntegration::RenderEffects(outputSurface);	// UNOFFICIAL: with ReShade loaded, its effects here (before the HUD) on NVR's depth
		if (HdrOutput::S.Active) {
			static GpuTimer hdrMarkTimer("HDR scene copy (mark)");
			GpuProfileScope gpu(hdrMarkTimer, device);
			HdrOutput::MarkScene(outputSurface);	// UNOFFICIAL 2026-10-26: the finished picture before the HUD (HUD pixels stay at paper white)
		}
		HdrOutput::Capture();					// UNOFFICIAL 2026-10-26 test tool ([Shaders.HDR.Main] CaptureFrame)
		TheRenderManager->CheckAndTakeScreenShot(outputSurface, mainSettings.HDRScreenshot);
		Diagnostics::OnFinalPicture(device, outputSurface);	// UNOFFICIAL 2026-11-01: the flash detector and the report key (core/Diagnostics.h)
		ReflectionProbe::EndFrame();
	}

#if NVR_TEST_TOOLS
	EffectSelfTest::EndFrame(outputSurface != NULL);	// UNOFFICIAL, diagnostic: the settings back, the run moves on
#endif
	if (gameSurface) gameSurface->Release();
}

static void RenderMainMenuMovie() {
	auto* bink = TheBinkManager;
	if (TheSettingManager->SettingsMain.Main.ReplaceIntro && InterfaceManager->IsActive(Menu::MenuType::kMenuType_Main))
		bink->Render(MainMenuMovie);
	else
		bink->Close();

}

CallDetour kRenderInterfaceDetour;
void __fastcall RenderInterfaceHook(void* apThis, void*, void* apCuller, bool abPipboyVisible) {
	SessionLog::StageScope stage("the interface");
	{ SessionLog::StageScope warm("shader warm-up"); ShaderWarmUp::OnInterfaceFrame(); }	// UNOFFICIAL: at the main menu, draw the shaders used in play once (ShaderWarmUp.h)
	{ SessionLog::StageScope warm("shader warm-up 2"); ShaderWarmUp2::OnInterfaceFrame(); }	// UNOFFICIAL: at the main menu, draw the recorded drawing setups once (ShaderWarmUp2.h)
	{ SessionLog::StageScope movie("the main menu movie"); RenderMainMenuMovie(); }
	ImGuiManager::NewFrame();
	const auto original = kRenderInterfaceDetour.GetOverwrittenAddr();
	ThisCall(original, apThis, apCuller, abPipboyVisible);
	ReShadeIntegration::BeforeNVRMenu();	// UNOFFICIAL 2026-10-25: ReShade's effects under NVR's menu, never on it
	ImGuiManager::Render();
#if NVR_TEST_TOOLS
	SettingShots::CaptureMenu();	// UNOFFICIAL 2026-10-19, test only: 'menu' shots (the menu drawn on the picture)
#endif
}

static void SetTileShaderConstants() {
	if (!InterfaceManager->IsActive(Menu::MenuType::kMenuType_Main)) return;
	float matrix[16];
	const NiVector4 tint = { 1.0f, 1.0f, 1.0f, 0.0f };
	IDirect3DDevice9* dev = TheRenderManager->device;
	dev->GetVertexShaderConstantF(0, matrix, 4);
	if ((int)matrix[3] == -1 && (int)matrix[7] == 1 && (int)matrix[15] == 1) dev->SetPixelShaderConstantF(0, (const float*)&tint, 1);
}

__declspec(naked) void SetTileShaderConstantsHook() {

	__asm {
		pushad
		call	SetTileShaderConstants
		popad
		cmp		byte ptr [esi + 0xAC], 0
		jmp		Jumpers::SetTileShaderConstants::Return
	}

}

using ShowDetectorWindowMethod = void*(__thiscall*)(DetectorWindow*, HWND, HINSTANCE, NiNode*, char*, int, int, int, int);
ShowDetectorWindowMethod ShowDetectorWindow = reinterpret_cast<ShowDetectorWindowMethod>(::Hooks::ShowDetectorWindow);
void* __fastcall ShowDetectorWindowHook(DetectorWindow* This, UInt32 edx, HWND Handle, HINSTANCE Instance, NiNode* RootNode, char* FormCaption, int X, int Y, int Width, int Height) {
	
	void* result = ShowDetectorWindow(This, Handle, Instance, RootNode, (char*)"Pipeline detector by Alenet", X, Y, 1280, 1024);
	auto& top = RootNode->m_children;
	for (int i = 0; i < top.end; i++) {
		NiNode* node = (NiNode*)top.data[i];
		auto& kids = node->m_children;
		kids.data[0] = NULL;
		kids.data[1] = NULL;
		kids.end = 0;
		kids.numObjs = 0;
	}
	return result;

}

void DetectorWindowSetNodeName(char* Buffer, int Size, char* Format, char* ClassName, char* Name, float LPosX, float LPosY, float LPosZ) {

	strcpy(Buffer, Name);

}

static void DetectorWindowCreateTreeView(HWND TreeView) {
	HFONT font = CreateFontA(14, 0, 0, 0, FW_DONTCARE, NULL, NULL, NULL, ANSI_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
		DEFAULT_PITCH | FF_DONTCARE, "Consolas");
	const LPARAM background = 0x001E1E1E, foreground = 0x00DCDCDC;
	SendMessageA(TreeView, WM_SETFONT, (WPARAM)font, TRUE);
	SendMessageA(TreeView, TVM_SETBKCOLOR, NULL, background);
	SendMessageA(TreeView, TVM_SETTEXTCOLOR, NULL, foreground);
}

__declspec(naked) void DetectorWindowCreateTreeViewHook() {

	__asm {
		pushad
		push	eax
		call	DetectorWindowCreateTreeView
		pop		eax
		popad
		mov     ecx, [ebp - 0x48]
		mov		[ecx + 0x0C], eax
		mov     esp, ebp
		pop     ebp
		jmp		Jumpers::DetectorWindow::CreateTreeViewReturn
	}

}

void DetectorWindowDumpAttributes(HWND TreeView, UInt32 Msg, WPARAM wParam, LPTVINSERTSTRUCTA lParam) {

	TVITEMEXA info = { NULL };
	char text[260] = { '\0' };
	info.pszText = text;
	info.mask = TVIF_TEXT;
	info.hItem = (HTREEITEM)SendMessageA(TreeView, TVM_GETNEXTITEM, TVGN_PARENT, (LPARAM)lParam->hParent);
	info.cchTextMax = 260;
	SendMessageA(TreeView, TVM_GETITEMA, 0, (LPARAM)&info);
	if (!memcmp(info.pszText, "Pass", 4))
		SendMessageA(TreeView, TVM_DELETEITEM, 0, (LPARAM)lParam->hParent);
	else if (strlen(info.pszText))
		SendMessageA(TreeView, Msg, wParam, (LPARAM)lParam);
}

__declspec(naked) void DetectorWindowDumpAttributesHook() {

	__asm {
		call	DetectorWindowDumpAttributes
		add		esp, 16
		jmp		Jumpers::DetectorWindow::DumpAttributesReturn
	}

}

__declspec(naked) void DetectorWindowConsoleCommandHook() {

	__asm {
		call	DWNode::Create
		jmp		Jumpers::DetectorWindow::ConsoleCommandReturn
	}

}

// Enables culling of muzzle flashes so they don't stay after firing
void __fastcall MuzzleLightCullingFix(MuzzleFlash* This) {
	if (auto* light = This->light) {
		if (This->bEnabled) light->m_flags &= ~1;
		else light->m_flags |= 1;
	}
	ThisCall(0x9BB8A0, This);
}

BSFogProperty* __cdecl ShadowSceneNode__GetFogPropertyEx(UInt32 aeType) {
	// The master switch counts too: with RenderEffects off NVR's fog is not drawn, so the game's must stay (2026-10-16 vanilla
	// check: "NVR all off" showed no distance fog at all).
	if (TheSettingManager->SettingsMain.Main.RenderEffects && TheShaderManager->Effects.VolumetricFog->Enabled && !TheShaderManager->GameState.isUnderwater) {
		aeType = 1;  // Use UI scene node and thus render no fog.
	}

	auto* sceneNode = BSShaderManager::GetShadowSceneNode(static_cast<BSShaderManager::SceneGraphType>(aeType));
	return sceneNode->fogProperty;
}

NiPoint2* __fastcall WaterFogRemover(NiPoint2* point, void*, float x, float y)
{
	point->x = point->y = 0.f;
	return point;
}

// Compatibility patch for DXVK 16bits buffer upgrade.
using FormatUpgradeSwitch = bool(__cdecl*)();

BSRenderedTexture* (__cdecl* CreateBSRenderedTexture)(BSString*, const UInt32, const UInt32, NiTexture::FormatPrefs*, UInt32, bool, NiDepthStencilBuffer*, UInt32, UInt32) = (BSRenderedTexture * (__cdecl*)(BSString*, const UInt32, const UInt32, NiTexture::FormatPrefs*, UInt32, bool, NiDepthStencilBuffer*, UInt32, UInt32))Hooks::CreateRenderedTexture;
BSRenderedTexture* __cdecl CreateSaveTextureHook(BSString* apName, const UInt32 uiWidth, const UInt32 uiHeight, NiTexture::FormatPrefs* kPrefs, 
	UInt32 eMSAAPref, bool bUseDepthStencil, NiDepthStencilBuffer* pkDSBuffer, UInt32 a7, UInt32 uiBackgroundColor) {
		HMODULE d3d9 = DxvkLoader::Runtime();
	FormatUpgradeSwitch disable = reinterpret_cast<FormatUpgradeSwitch>(GetProcAddress(d3d9, "DXVK_D3D9_HDR_DisableRenderTargetUpgrade"));
	FormatUpgradeSwitch enable = reinterpret_cast<FormatUpgradeSwitch>(GetProcAddress(d3d9, "DXVK_D3D9_HDR_EnableRenderTargetUpgrade"));

	if (disable)
		disable();	// the texture is made without the format upgrade
	BSRenderedTexture* texture = CreateBSRenderedTexture(apName, uiWidth, uiHeight, kPrefs, eMSAAPref, bUseDepthStencil, pkDSBuffer, a7, uiBackgroundColor);
	if (enable)
		enable();	// the format upgrade is back on

	return texture;
}

// 0xE69660
bool __fastcall NiDX9Renderer__Do_EndFrame(NiDX9Renderer* apThis, void*) {
	const bool result = ThisStdCall<bool>(0xE69660, apThis);

	// Reload effects if queued.
	ShaderManager* shaders = TheShaderManager;
	if (shaders && shaders->ShaderReloadQueued) {	// UNOFFICIAL 2026-10-25 'reloadshaders' (SettingShots.h)
		shaders->ShaderReloadQueued = false;
		shaders->ReloadShaders();
	}
	if (shaders && shaders->EffectReloadQueued) {
		shaders->ReloadEffects();
		shaders->EffectReloadQueued = false;
	}

	return result;
}


// Code to increase all lights strength
//__forceinline NiVector4* GetConstant(int index) {
//	return &((NiVector4*)0x11FA0C0)[index];
//}
//
//__forceinline NiColorAlpha* GetLightConstant(int index) {
//	return reinterpret_cast<NiColorAlpha*>(GetConstant(index));
//}
//
//__forceinline void ScaleColor(NiColorAlpha* Color, float scale) {
//	Color->r *= scale;
//	Color->g *= scale;
//	Color->b *= scale;
//}
//
//void __fastcall ShadowLightShader__UpdateLights(void* apThis, void*, void* apShaderProp, void* apRenderPass, D3DXMATRIX aMatrix, void* apTransform, UInt32 aeRenderPassType, void* apSkinInstance) {
//	ThisCall(0xB78A90, apThis, apShaderProp, apRenderPass, aMatrix, apTransform, aeRenderPassType, apSkinInstance);
	//Logger::Log("scaling light by %f", TheShaderManager->ShaderConst.HDR.PointLightMult);
	//NiColorAlpha* pColor;

	// ambient light is constant 0
	//ScaleColor(GetLightConstant(0), TheShaderManager->ShaderConst.HDR.PointLightMult);

	// pointlight registers go from 0 to 10
	//for (UInt32 i = 0; i < 12; i++) {
	//	ScaleColor(GetLightConstant(i), TheShaderManager->ShaderConst.HDR.PointLightMult);
	//}

	// emittance color is index 27
	//ScaleColor(GetLightConstant(27), TheShaderManager->ShaderConst.HDR.PointLightMult);
//}

