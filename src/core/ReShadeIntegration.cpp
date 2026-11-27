// UNOFFICIAL: NVR as a ReShade add-on. Each job has its own switch in [Main.Main.Performance]:
//
//   ReShadeNVRDepth: ReShade's DEPTH texture becomes a copy of NVR's combined depth. ReShade's Generic Depth
//   only sees the game's depth buffer, which the game clears before it draws the first-person model, so the gun
//   and arms are missing from it (depth effects shade or blur across them). NVR's combined depth already holds
//   the world and the first-person model in one buffer, in main-camera space. It is written back in the
//   direction of the game's own depth buffer, so the RESHADE_DEPTH_INPUT_* settings that fit the game still fit.
//
//   ReShadeDepthNearRemap (2026-10-18): the first-person model is drawn with its own, closer near plane, so in the
//   world camera's projection its depth lies past the near plane (reversed projected depth g > 1). ReShade.fxh-style
//   decoders then get a negative linear depth. The copy folds that band back below 1 with a monotone map: identity
//   for g <= 0.5 (everything farther than about twice the world near plane: every normal world and LOD pixel comes
//   out bit-identical), 1 - 1/(4g) above (value and slope continuous at 0.5, ordered, < 1); negative or non-finite
//   values become sky. The map is adapted from NVR Depth Bridge (MIT, NVR Depth Bridge contributors), notice below.
//
//   ReShadeBeforeHUD: ReShade normally draws its effects when the frame is presented, after the HUD and menus.
//   effect_runtime::render_effects called right after NVR's effects draws them there instead, and ReShade then
//   skips its own draw for that frame. Frames where NVR's image space does not run (main menu, loading screens)
//   keep ReShade's usual draw at present. D3D9 runtime only.
//
//   ReShadeMenuGuard (2026-10-18, off by default): the effect files named in ReShadeMenuGuardEffects are paused while
//   a menu, the Pip-Boy (and with ReShadeMenuGuardVATS, VATS) is open. Only techniques that are on when ReShade
//   starts its effects are switched off, and exactly those are switched on again right after the effects, before
//   ReShade's overlay or preset can see the change (nothing is ever saved). Menu state from the game's own
//   InterfaceManager. Design after NVR Depth Bridge's MenuGuard.
//
//   ReShadeGameValues (2026-10-25, on by default; only effects that ask for them see a difference): uniforms with
//   source = "ModLoaded" / "Weather" / "WorldspaceID" / "InteriorID" get the game's values, the names FNV ReShade
//   Helper (doodlum and Wall_SoGB, github.com/doodlum/fnv-reshade-helper) uses, so presets written for it work with NVR.
//   Weather = (current weather form ID, previous weather form ID, transition 0..1, game hour); IDs are the low 24
//   bits of the form ID (no load-order byte). Written from scratch: that repository carries no licence.
//
// Two ReShade runtimes are served (the game has one swap chain, so one of them):
//   D3D9 (ReShade installed as d3d9.dll, also when DXVK sits behind it): view handles are the D3D9 objects themselves
//   (the low bit marks the sRGB variant). DEPTH is bound in reshade_begin_effects and unbound after the effects.
//   Vulkan (2026-10-18, ReShade as a Vulkan layer under DXVK): NVR's copy texture is a VkImage inside DXVK. Its handle
//   and DXVK's resting layout come from DXVK's ID3D9VkInteropTexture (asked on the render thread right after the copy
//   and published with a generation number); ReShade's events run on DXVK's presenting thread, where a resource view
//   of that image is made through the ReShade API and bound to DEPTH. The image is moved to the shader-read layout in
//   reshade_begin_effects and back to DXVK's layout in reshade_finish_effects, inside ReShade's own present command
//   list (no submissions of our own, no DXVK flush). ReShade waits for the GPU in update_texture_bindings on Vulkan,
//   so DEPTH stays bound across frames there: it is rebound only after an effect reload, when something else rebound
//   DEPTH, or for a new image, and unbound when NVR stops providing it (switch off, no new copy for a few presents).
//   The Vulkan side never holds a D3D9 object: only the VkImage handle and its own view. NVR's texture lives for the
//   whole session (NVR does not recreate its targets on a device reset).
//
// The events used (init/destroy_effect_runtime and the reshade_* ones) are allowed in ReShade's normal build,
// not only the one with full add-on support. The register call is made directly (not reshade::register_addon)
// to accept a ReShade older than these headers: it is tried from RESHADE_API_VERSION down; only interface
// methods that exist since API version 1 are used. ReShade's exports use the default (cdecl) convention.
//
// ---- Notice for the parts adapted from NVR Depth Bridge (near remap, Vulkan interop/layout handling, menu guard) ----
// MIT License
//
// Copyright (c) 2026 NVR Depth Bridge contributors
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
// ---------------------------------------------------------------------------------------------------------------------

#ifdef assert
#undef assert	// Framework.h's own assert; reshade.hpp's headers use the CRT one
#endif
#include <cassert>
#ifdef IMGUI_VERSION_NUM
#undef IMGUI_VERSION_NUM	// keep reshade_overlay.hpp's ImGui function table out; NVR has its own ImGui
#endif
#include <reshade.hpp>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>
#include "TestTools.h"
#include "ImGuiManager.h"	// BeforeNVRMenu: is the menu open
#if NVR_TEST_TOOLS
#include "SettingShots.h"	// test only: the auto-test's 'final' pictures
#endif

using namespace reshade::api;

namespace {

	// DXVK's D3D9 interop interface for textures: the subset of DXVK's public header src/d3d9/d3d9_interfaces.h
	// (zlib licence) that is needed here, as NVR Depth Bridge declares it. VkImage is a 64-bit handle also in a 32-bit
	// process, VkImageLayout a 32-bit enum; the create-info pointer is passed as null (DXVK then leaves it out).
	struct DxvkImageCreateInfo;
	MIDL_INTERFACE("d56344f5-8d35-46fd-806d-94c351b472c1")
	ID3D9VkInteropTexture : public IUnknown {
		virtual HRESULT STDMETHODCALLTYPE GetVulkanImageInfo(uint64_t* pHandle, uint32_t* pLayout, DxvkImageCreateInfo* pInfo) = 0;
	};

	HMODULE NvrModule = nullptr;
	HMODULE ReShadeModule = nullptr;
	bool Registered = false;
	UInt32 RegisterTries = 0;
	UInt32 FramesSeen = 0;

	effect_runtime* Runtime = nullptr;					// ReShade's D3D9 runtime of the game's swap chain (render thread)
	std::atomic<effect_runtime*> VkRuntime{ nullptr };	// ReShade's Vulkan runtime under DXVK (its events come on DXVK's presenting thread)
	bool ReportedOther = false;
	UInt32 PresentCount = 0;			// presents of Runtime, to tell this frame's depth copy from an older one
	UInt32 DepthCopiedAt = ~0u;
	UInt32 EffectsDrawnAt = ~0u;		// PresentCount of the last frame whose effects NVR had ReShade draw (render_effects)
	bool DepthBound = false;			// DEPTH points at NVR's copy for the effects running now (D3D9)

	IDirect3DTexture9* DepthTexture = nullptr;
	IDirect3DSurface9* DepthSurface = nullptr;
	ID3DXEffect* DepthEffect = nullptr;
	D3DXHANDLE ParamsHandle = NULL;
	D3DXHANDLE OptionsHandle = NULL;
	D3DXHANDLE SourceHandle = NULL;
	bool DepthFailed = false;
	IDirect3DStateBlock9* SavedState = nullptr;

	// Render thread -> Vulkan side (DXVK's presenting thread).
	std::atomic<uint64_t> SharedImage{ 0 };		// VkImage of DepthTexture
	std::atomic<uint32_t> SharedLayout{ 0 };	// DXVK's resting VkImageLayout of it
	std::atomic<uint32_t> SharedGeneration{ 0 };	// changes whenever the two above do (written last, read first)
	std::atomic<uint32_t> CopySerial{ 0 };		// one more after every copy
	bool InteropFailed = false;

	// Vulkan side only (DXVK's presenting thread).
	const UInt32 StalePresents = 4;				// presents without a new copy (menus, loading screens) before DEPTH is unbound
	struct VulkanState {
		resource_view view = { 0 };
		uint64_t image = 0;
		uint32_t generation = 0;
		resource_usage resting = resource_usage::undefined;
		uint32_t lastSerial = 0;
		UInt32 stale = 0;
		bool bound = false, rebind = false, transitioned = false, reportedDrift = false;
		std::vector<effect_texture_variable> depthVariables;	// texture variables that showed our view right after binding
		UInt32 timedCalls = 0;
	} Vk;

	// Menu guard. The list text is set on the render thread (SettingManager) and read where ReShade runs its effects.
	std::mutex GuardMutex;
	std::string GuardText;
	std::atomic<UInt32> GuardTextVersion{ 0 };
	struct GuardState {
		UInt32 version = ~0u;
		bool dirty = true;
		std::vector<std::string> files;
		std::vector<effect_technique> techniques;
		std::vector<effect_technique> paused;
		bool reportedPause = false;
	} Guard;

	// Game values. Read from the game on the render thread (RenderEffects), handed to ReShade where it runs its effects.
	struct GameValueSnapshot {
		float weather[4] = {};		// current weather form ID, previous (fading) weather form ID, transition 0..1, game hour
		int32_t worldspace = 0;		// form ID (low 24 bits) of the player's worldspace, 0 in interiors
		int32_t interior = 0;		// form ID (low 24 bits) of the player's interior cell, 0 outside
	};
	std::mutex ValuesMutex;
	GameValueSnapshot Values;
	enum class ValueSource { ModLoaded, Weather, WorldspaceID, InteriorID };
	struct ValueState {
		bool dirty = true;			// variable handles are found again after an effect reload or a runtime change
		bool wasOn = false;
		std::vector<std::pair<effect_uniform_variable, ValueSource>> variables;
	} GameValues;

	// Combined depth -> projected depth in the game's direction. The source is NVR's TESR_DepthBuffer: R32F with
	// SlimDepthBuffer (view Z / far), otherwise G32R32F (.x view Z / far, .y reversed projected depth).
	const char DepthCopyFX[] = R"(
float4 Params;	// x near, y far, z 1 = the game's depth is reversed, w 1 = single-channel (linear) source
float4 Options;	// x 1 = near remap (ReShadeDepthNearRemap)
texture CombinedDepth;
sampler2D Source = sampler_state { Texture = <CombinedDepth>; ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };

struct VSOUT { float4 pos : POSITION; float2 uv : TEXCOORD0; };

VSOUT VS(float4 pos : POSITION0, float2 uv : TEXCOORD0) {
	VSOUT OUT;
	OUT.pos = pos;
	OUT.uv = uv;
	return OUT;
}

float4 PS(float2 uv : TEXCOORD0) : COLOR0 {
	float4 texel = tex2D(Source, uv);
	float reversed = texel.y;
	if (Params.w > 0.5) {
		float viewZ = max(texel.x * Params.y, 1.0e-6);
		reversed = Params.x * (Params.y - viewZ) / (viewZ * (Params.y - Params.x));
	}
	if (Options.x > 0.5) {
		// Near remap, adapted from NVR Depth Bridge (MIT, NVR Depth Bridge contributors): identity up to 0.5 (bit-exact),
		// 1 - 1/(4g) above (first-person depths past the world near plane stay ordered and below 1); negative, NaN or
		// infinite -> sky.
		reversed = (reversed >= 0.0 && reversed < 3.0e38) ? reversed : 0.0;
		reversed = reversed <= 0.5 ? reversed : min(1.0 - 0.25 / reversed, 0.99999994);
	}
	float depth = Params.z > 0.5 ? reversed : 1.0 - reversed;
	return float4(depth, depth, depth, 1.0);
}

technique Copy {
	pass P0 {
		ZEnable = false; ZWriteEnable = false; StencilEnable = false; AlphaBlendEnable = false; AlphaTestEnable = false;
		ColorWriteEnable = 15; CullMode = None; ScissorTestEnable = false; SRGBWriteEnable = false; FogEnable = false;
		VertexShader = compile vs_3_0 VS();
		PixelShader = compile ps_3_0 PS();
	}
}
)";

	HMODULE FindReShade() {
		HMODULE modules[1024];
		DWORD bytes = 0;
		if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes)) return nullptr;
		const DWORD count = min(bytes, (DWORD)sizeof(modules)) / sizeof(HMODULE);
		for (DWORD i = 0; i < count; ++i)
			if (GetProcAddress(modules[i], "ReShadeRegisterAddon") && GetProcAddress(modules[i], "ReShadeUnregisterAddon"))
				return modules[i];
		return nullptr;
	}

	void TakeRuntime(effect_runtime* runtime) {
		if (!runtime || Runtime || VkRuntime.load()) return;	// one runtime: the game's swap chain
		const device_api api = runtime->get_device()->get_api();
		if (api == device_api::d3d9) {
			Runtime = runtime;
			Logger::Log("UNOFFICIAL ReShade: D3D9 runtime found.");
			return;
		}
		if (api == device_api::vulkan) {
			effect_runtime* none = nullptr;
			if (VkRuntime.compare_exchange_strong(none, runtime))
				Logger::Log("UNOFFICIAL ReShade: Vulkan runtime found (ReShade as a Vulkan layer under DXVK): Vegas Shaders depth goes over through DXVK's interop (ReShadeNVRDepth); drawing before the HUD needs ReShade's D3D9 runtime and stays off.");
			return;
		}
		if (!ReportedOther) {
			Logger::Log("UNOFFICIAL ReShade: its runtime is neither D3D9 nor Vulkan (API %u): Vegas Shaders depth and drawing before the HUD are off, only ReShadeDepthCompat applies.", (unsigned)api);
			ReportedOther = true;
		}
	}

	// ---- Vulkan side (DXVK's presenting thread) ----

	// DXVK's resting layout of the image as a ReShade state; VkImageLayout values of Vulkan core (as NVR Depth Bridge
	// maps them): 1 GENERAL, 2 COLOR_ATTACHMENT_OPTIMAL, 5 SHADER_READ_ONLY_OPTIMAL (the one DXVK picks for a sampled
	// render target).
	bool UsageFromLayout(uint32_t layout, resource_usage& usage) {
		switch (layout) {
		case 1: usage = resource_usage::general; return true;
		case 2: usage = resource_usage::render_target; return true;
		case 5: usage = resource_usage::shader_resource; return true;
		default: return false;
		}
	}

	// update_texture_bindings waits for the GPU on Vulkan; the first calls are timed in the log to show what it costs.
	void BindDepth(effect_runtime* runtime, resource_view view, const char* what) {
		LARGE_INTEGER start, end, frequency;
		QueryPerformanceCounter(&start);
		runtime->update_texture_bindings("DEPTH", view, view);
		QueryPerformanceCounter(&end);
		QueryPerformanceFrequency(&frequency);
		if (Vk.timedCalls < 8) {
			Vk.timedCalls++;
			Logger::Log("UNOFFICIAL ReShade (Vulkan): DEPTH %s (update_texture_bindings %.2f ms).", what, (double)(end.QuadPart - start.QuadPart) * 1000.0 / (double)frequency.QuadPart);
		}
	}

	// The texture variables that show our view right after binding it: the effects' DEPTH textures, watched for a
	// rebind by something else (ReShade's Generic Depth binds when its own pick changes).
	void FindDepthVariables(effect_runtime* runtime) {
		Vk.depthVariables.clear();
		runtime->enumerate_texture_variables(nullptr, [](effect_runtime* rt, effect_texture_variable variable) {
			resource_view srv = { 0 }, srgb = { 0 };
			rt->get_texture_binding(variable, &srv, &srgb);
			if (srv == Vk.view) Vk.depthVariables.push_back(variable);
		});
	}

	void ReleaseView(effect_runtime* runtime, const char* why) {
		if (Vk.view == 0) return;
		if (Vk.bound) { BindDepth(runtime, resource_view{ 0 }, why); Vk.bound = false; }
		runtime->get_command_queue()->wait_idle();	// only here (a new image or the runtime going away), never per frame
		runtime->get_device()->destroy_resource_view(Vk.view);
		Vk.view = { 0 };
		Vk.image = 0;
		Vk.depthVariables.clear();
		Vk.transitioned = false;
	}

	void VulkanBegin(effect_runtime* runtime, command_list* cmd) {
		device* const dev = runtime->get_device();
		// A copy since the last present? (approximate: the render thread may already be a frame ahead; only used to
		// unbind DEPTH after a few presents without one, as in menus and loading screens)
		const uint32_t serial = CopySerial.load(std::memory_order_acquire);
		if (serial != Vk.lastSerial) { Vk.lastSerial = serial; Vk.stale = 0; }
		else if (Vk.stale < 100000) Vk.stale++;

		// A new image: the first copy, or DXVK gave the texture another VkImage.
		const uint32_t generation = SharedGeneration.load(std::memory_order_acquire);
		if (generation != Vk.generation) {
			ReleaseView(runtime, "unbound (Vegas Shaders' depth copy has a new image)");
			Vk.generation = generation;
			const uint64_t image = SharedImage.load(std::memory_order_relaxed);
			const uint32_t layout = SharedLayout.load(std::memory_order_relaxed);
			resource_usage resting = resource_usage::undefined;
			if (image && UsageFromLayout(layout, resting)) {
				const resource_desc desc = dev->get_resource_desc(resource{ image });
				if (desc.type == resource_type::texture_2d && desc.texture.format == format::r32_float && desc.texture.samples == 1 &&
					dev->create_resource_view(resource{ image }, resource_usage::shader_resource, resource_view_desc(format::r32_float, 0, 1, 0, 1), &Vk.view)) {
					Vk.image = image;
					Vk.resting = resting;
					Logger::Log("UNOFFICIAL ReShade (Vulkan): view made of Vegas Shaders' depth copy (DXVK image %08x%08x, %ux%u, resting layout %u).",
						(unsigned)(image >> 32), (unsigned)image, desc.texture.width, desc.texture.height, layout);
				}
				else {
					Vk.view = { 0 };
					Logger::Log("UNOFFICIAL ReShade (Vulkan): no view could be made of Vegas Shaders' depth copy (type %u, format %u, samples %u): Vegas Shaders depth not handed over.",
						(unsigned)desc.type, (unsigned)desc.texture.format, (unsigned)desc.texture.samples);
				}
			}
			else if (image) Logger::Log("UNOFFICIAL ReShade (Vulkan): DXVK keeps Vegas Shaders' depth copy in layout %u, which is not handled here: Vegas Shaders depth not handed over.", layout);
		}

		const bool want = TheSettingManager->SettingsMain.Main.ReShadeNVRDepth && Vk.view != 0 && Vk.stale <= StalePresents;
		if (!want) {
			if (Vk.bound) {
				BindDepth(runtime, resource_view{ 0 }, Vk.stale > StalePresents ? "unbound (no new Vegas Shaders depth for a few frames)" : "unbound");
				Vk.bound = false;
				Vk.depthVariables.clear();
			}
			return;
		}
		if (Vk.bound && !Vk.rebind && !Vk.depthVariables.empty()) {
			resource_view srv = { 0 }, srgb = { 0 };
			runtime->get_texture_binding(Vk.depthVariables[0], &srv, &srgb);
			if (srv != Vk.view) {
				Vk.rebind = true;
				if (!Vk.reportedDrift) {
					Logger::Log("UNOFFICIAL ReShade (Vulkan): DEPTH was rebound by something else (Generic Depth? best switched off), taken back; each time costs a GPU wait.");
					Vk.reportedDrift = true;
				}
			}
		}
		if (!Vk.bound || Vk.rebind) {
			BindDepth(runtime, Vk.view, Vk.bound ? "bound again to Vegas Shaders' combined depth" : "bound to Vegas Shaders' combined depth");
			Vk.bound = true;
			Vk.rebind = false;
			FindDepthVariables(runtime);
		}
		// Into ReShade's present command list, after DXVK's submissions of the frame: DXVK's layout -> shader read.
		cmd->barrier(resource{ Vk.image }, Vk.resting, resource_usage::shader_resource);
		Vk.transitioned = true;
	}

	void VulkanFinish(command_list* cmd) {
		if (!Vk.transitioned) return;
		cmd->barrier(resource{ Vk.image }, resource_usage::shader_resource, Vk.resting);	// back to the layout DXVK expects
		Vk.transitioned = false;
	}

#if NVR_TEST_TOOLS
	// Test only: a 'final' picture from ReShade's Vulkan runtime (after its effects), finished on the render thread.
	void CaptureFinalVulkan(effect_runtime* runtime) {
		if (!SettingShots::BeginFinalCapture()) return;
		static UInt32 failures = 0;
		uint32_t width = 0, height = 0;
		runtime->get_screenshot_width_and_height(&width, &height);
		SettingShots::Job* job = new (std::nothrow) SettingShots::Job();
		bool ok = false;
		if (job && width && height) {
			std::vector<uint8_t> pixels((size_t)width * height * 8);	// room for up to 8 bytes per pixel (the back buffer's own format)
			ok = runtime->capture_screenshot(pixels.data());
			if (ok) {
				job->width = width;
				job->height = height;
				job->bgra.resize((size_t)width * height);
				memcpy(job->bgra.data(), pixels.data(), (size_t)width * height * 4);	// 8-bit BGRA (DXVK's swap chain for X8R8G8B8)
				for (unsigned& pixel : job->bgra) pixel |= 0xFF000000u;
			}
		}
		if (!ok) {
			failures++;
			Logger::Log("SETTING SHOTS: no final picture from ReShade's Vulkan runtime (%ux%u, try %u)", width, height, failures);
			if (failures < 3) { delete job; job = nullptr; }		// tried again at the next present
			else if (job) { job->bgra.clear(); job->width = job->height = 1; failures = 0; }	// an empty picture: logged as not saved, the run moves on
		}
		SettingShots::EndFinalCapture(job);
	}
#endif

	// ---- Menu guard ----

#if NVR_TEST_TOOLS
	std::atomic<bool> ForceMenuForTest{ false };	// DIAGNOSTIC ReShadeDepthCheck bit 2: the guard acts as if a menu were open (the auto-test
												// cannot take pictures in real menus: they stop NVR's image space, where it counts frames)
#endif

	bool MenuOpen() {
#if NVR_TEST_TOOLS
		if (ForceMenuForTest.load(std::memory_order_relaxed)) return true;
#endif
		if (!InterfaceManager) return false;
		if (InterfaceManager->IsPipBoyOpen()) return true;
		if (InterfaceManager->currentMode == 1) return false;	// InterfaceManager::IsInMenuMode (0x7023A0): currentMode != 1 (1 = GameMode)
		const bool vats = InterfaceManager->menuStack[0] == Menu::kMenuType_VATS;
		return !vats || TheSettingManager->SettingsMain.Main.ReShadeMenuGuardVATS;
	}

	void RefreshGuard(effect_runtime* runtime) {
		const UInt32 version = GuardTextVersion.load(std::memory_order_acquire);
		if (version != Guard.version) {
			std::string text;
			{
				std::lock_guard<std::mutex> lock(GuardMutex);
				text = GuardText;
			}
			Guard.version = version;
			Guard.files.clear();
			size_t start = 0;
			while (start <= text.size()) {
				size_t end = text.find_first_of(",;", start);
				if (end == std::string::npos) end = text.size();
				std::string name = text.substr(start, end - start);
				const size_t a = name.find_first_not_of(" \t\""), b = name.find_last_not_of(" \t\"");
				if (a != std::string::npos) Guard.files.push_back(name.substr(a, b - a + 1));
				start = end + 1;
			}
			Guard.dirty = true;
		}
		if (!Guard.dirty) return;
		Guard.dirty = false;
		Guard.techniques.clear();
		for (const std::string& file : Guard.files)
			runtime->enumerate_techniques(file.c_str(), [](effect_runtime*, effect_technique technique) { Guard.techniques.push_back(technique); });
		Logger::Log("UNOFFICIAL ReShade: menu guard covers %u techniques in %u effect files.", (unsigned)Guard.techniques.size(), (unsigned)Guard.files.size());
	}

	void GuardBegin(effect_runtime* runtime) {
		if (!TheSettingManager->SettingsMain.Main.ReShadeMenuGuard || !MenuOpen()) { Guard.reportedPause = false; return; }
		RefreshGuard(runtime);
		for (effect_technique technique : Guard.techniques)
			if (technique != 0 && runtime->get_technique_state(technique)) {
				runtime->set_technique_state(technique, false);
				Guard.paused.push_back(technique);
			}
		if (!Guard.reportedPause) {
			Logger::Log("UNOFFICIAL ReShade: menu open, menu guard pauses %u techniques (switched on again after each frame's effects).", (unsigned)Guard.paused.size());
			Guard.reportedPause = true;
		}
	}

	// Exactly the techniques paused above are switched on again, before ReShade's overlay or preset sees them.
	void GuardFinish(effect_runtime* runtime) {
		for (effect_technique technique : Guard.paused) runtime->set_technique_state(technique, true);
		Guard.paused.clear();
	}

	// ---- Game values (ReShadeGameValues) ----

	// Render thread: the values ReShade effects can ask for, taken from the frame NVR just drew.
	void SnapshotGameValues() {
		GameValueSnapshot now;
		if (Tes && Tes->sky) {
			const Sky* sky = Tes->sky;
			now.weather[0] = sky->firstWeather ? (float)(sky->firstWeather->refID & 0x00FFFFFF) : 0.0f;
			now.weather[1] = sky->secondWeather ? (float)(sky->secondWeather->refID & 0x00FFFFFF) : 0.0f;
			now.weather[2] = sky->weatherPercent;
		}
		now.weather[3] = TheShaderManager->ShaderConst.GameTime.y;	// hours 0..24 (ShaderManager::UpdateConstants)
		if (Player && Player->parentCell) {
			TESObjectCELL* cell = Player->parentCell;
			if (cell->IsInterior()) now.interior = (int32_t)(cell->refID & 0x00FFFFFF);
			else if (cell->worldSpace) now.worldspace = (int32_t)(cell->worldSpace->refID & 0x00FFFFFF);
		}
		std::lock_guard<std::mutex> lock(ValuesMutex);
		Values = now;
	}

	// Uniforms with a 'source' annotation of one of the names FNV ReShade Helper (doodlum, Wall_SoGB) gives its effects,
	// so presets made for it work here: ModLoaded (bool), Weather (float4), WorldspaceID and InteriorID (int).
	void FindValueVariables(effect_runtime* runtime) {
		GameValues.dirty = false;
		GameValues.variables.clear();
		runtime->enumerate_uniform_variables(nullptr, [](effect_runtime* runtime, effect_uniform_variable variable) {
			char source[32] = {};
			if (!runtime->get_annotation_string_from_uniform_variable(variable, "source", source)) return;
			ValueSource kind;
			if (!strcmp(source, "ModLoaded")) kind = ValueSource::ModLoaded;
			else if (!strcmp(source, "Weather")) kind = ValueSource::Weather;
			else if (!strcmp(source, "WorldspaceID")) kind = ValueSource::WorldspaceID;
			else if (!strcmp(source, "InteriorID")) kind = ValueSource::InteriorID;
			else return;
			GameValues.variables.push_back({ variable, kind });
		});
		if (!GameValues.variables.empty())
			Logger::Log("UNOFFICIAL ReShade: game values go to %u effect variables (ReShadeGameValues).", (unsigned)GameValues.variables.size());
	}

	void SetGameValues(effect_runtime* runtime) {
		const bool on = TheSettingManager->SettingsMain.Main.ReShadeGameValues;
		if (!on && !GameValues.wasOn) return;
		if (GameValues.dirty) FindValueVariables(runtime);
		GameValueSnapshot now;
		{
			std::lock_guard<std::mutex> lock(ValuesMutex);
			now = Values;
		}
		for (const auto& [variable, kind] : GameValues.variables) {
			if (!on) {	// switched off: effects see the mod as gone again, the other values are left as they were
				if (kind == ValueSource::ModLoaded) runtime->set_uniform_value_bool(variable, false);
				continue;
			}
			switch (kind) {
			case ValueSource::ModLoaded: runtime->set_uniform_value_bool(variable, true); break;
			case ValueSource::Weather: runtime->set_uniform_value_float(variable, now.weather[0], now.weather[1], now.weather[2], now.weather[3]); break;
			case ValueSource::WorldspaceID: runtime->set_uniform_value_int(variable, now.worldspace); break;
			case ValueSource::InteriorID: runtime->set_uniform_value_int(variable, now.interior); break;
			}
		}
		GameValues.wasOn = on;
	}

	// ---- ReShade events ----

	void OnInitRuntime(effect_runtime* runtime) { TakeRuntime(runtime); }

	void OnDestroyRuntime(effect_runtime* runtime) {
		if (runtime == Runtime) Runtime = nullptr;
		else if (runtime == VkRuntime.load()) {
			ReleaseView(runtime, "unbound (ReShade's runtime goes away)");
			Vk.generation = 0;	// a new runtime makes a new view of the current image
			VkRuntime.store(nullptr);
		}
		else return;
		Guard.paused.clear();
		Guard.dirty = true;
		GameValues.dirty = true;
	}

	// Every present; also how the runtime is found when NVR registered after ReShade created it (the usual case).
	void OnPresent(effect_runtime* runtime) {
		TakeRuntime(runtime);
		if (runtime == Runtime) {
			PresentCount++;
#if NVR_TEST_TOOLS
			SettingShots::CaptureFinal();
#endif
		}
#if NVR_TEST_TOOLS
		else if (runtime == VkRuntime.load()) CaptureFinalVulkan(runtime);
#endif
	}

	// Technique and variable handles are invalid after a reload; on Vulkan DEPTH is bound again once.
	void OnReloadedEffects(effect_runtime* runtime) {
		if (runtime != Runtime && runtime != VkRuntime.load()) return;
		Guard.paused.clear();
		Guard.dirty = true;
		GameValues.dirty = true;
		if (runtime == VkRuntime.load()) {
			Vk.rebind = true;
			Vk.depthVariables.clear();
		}
	}

	// ReShade's built-in Generic Depth binds its pick in this same event and registered first, so this binding
	// is the one the effects see. A copy from an earlier frame (NVR's image space skipped this one) is left out.
	void OnBeginEffects(effect_runtime* runtime, command_list* cmd, resource_view, resource_view) {
		if (runtime == Runtime) {
			if (DepthTexture && DepthCopiedAt == PresentCount && TheSettingManager->SettingsMain.Main.ReShadeNVRDepth) {
				const resource_view depth = { reinterpret_cast<uintptr_t>(DepthTexture) };
				runtime->update_texture_bindings("DEPTH", depth, depth);
				DepthBound = true;
			}
		}
		else if (runtime == VkRuntime.load()) VulkanBegin(runtime, cmd);
		else return;
		SetGameValues(runtime);
		GuardBegin(runtime);
	}

	// D3D9: unbound again after the effects, so a frame without NVR's copy, or with ReShadeNVRDepth off, gets Generic
	// Depth's pick (or nothing) instead of an old copy left bound. Vulkan: the image goes back to DXVK's layout.
	void OnFinishEffects(effect_runtime* runtime, command_list* cmd, resource_view, resource_view) {
		if (runtime == Runtime) {
			if (DepthBound) {
				runtime->update_texture_bindings("DEPTH", resource_view{ 0 }, resource_view{ 0 });
				DepthBound = false;
			}
		}
		else if (runtime == VkRuntime.load()) VulkanFinish(cmd);
		else return;
		GuardFinish(runtime);
	}

	bool TryRegister() {
		RegisterTries++;
		const auto registerAddon = reinterpret_cast<bool(*)(void*, uint32_t)>(GetProcAddress(ReShadeModule, "ReShadeRegisterAddon"));
		if (!registerAddon || !NvrModule) return false;
		uint32_t version = 0;
		for (uint32_t v = RESHADE_API_VERSION; v >= 1 && !version; --v)
			if (registerAddon(NvrModule, v)) version = v;
		if (!version) {
			Logger::Log("UNOFFICIAL ReShade: ReShade refused Vegas Shaders as an add-on (API versions %u..1, try %u); see ReShade.log.", RESHADE_API_VERSION, RegisterTries);
			return false;
		}
		reshade::register_event<reshade::addon_event::init_effect_runtime>(OnInitRuntime);
		reshade::register_event<reshade::addon_event::destroy_effect_runtime>(OnDestroyRuntime);
		reshade::register_event<reshade::addon_event::reshade_present>(OnPresent);
		reshade::register_event<reshade::addon_event::reshade_begin_effects>(OnBeginEffects);
		reshade::register_event<reshade::addon_event::reshade_finish_effects>(OnFinishEffects);
		reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(OnReloadedEffects);
		Registered = true;
		Logger::Log("UNOFFICIAL ReShade: Vegas Shaders registered as a ReShade add-on (API version %u, headers %u).", version, RESHADE_API_VERSION);
		return true;
	}

	// ---- D3D9 side (render thread) ----

	bool CreateDepthCopy(IDirect3DDevice9* Device) {
		ID3DXBuffer* errors = nullptr;
		HRESULT result = D3DXCreateEffect(Device, DepthCopyFX, sizeof(DepthCopyFX) - 1, NULL, NULL, D3DXFX_LARGEADDRESSAWARE, NULL, &DepthEffect, &errors);	// as EffectRecord: the game is large-address aware (E_OUTOFMEMORY without it)
		if (FAILED(result)) {
			Logger::Log("UNOFFICIAL ReShade: depth copy shader failed (%08lx): %s", result, errors ? (const char*)errors->GetBufferPointer() : "");
			if (errors) errors->Release();
			return false;
		}
		if (errors) errors->Release();
		ParamsHandle = DepthEffect->GetParameterByName(NULL, "Params");
		OptionsHandle = DepthEffect->GetParameterByName(NULL, "Options");
		SourceHandle = DepthEffect->GetParameterByName(NULL, "CombinedDepth");
		if (!ParamsHandle || !OptionsHandle || !SourceHandle) {
			Logger::Log("UNOFFICIAL ReShade: depth copy shader parameters missing.");
			return false;
		}
		result = Device->CreateTexture(TheRenderManager->width, TheRenderManager->height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &DepthTexture, NULL);
		if (FAILED(result) || FAILED(DepthTexture->GetSurfaceLevel(0, &DepthSurface))) {
			Logger::Log("UNOFFICIAL ReShade: depth texture (R32F %ux%u) failed (%08lx).", TheRenderManager->width, TheRenderManager->height, result);
			return false;
		}
		return true;
	}

	IDirect3DTexture9* CombinedDepthSource() {
		return TheShaderManager->Effects.CombineDepth ? TheShaderManager->Effects.CombineDepth->Textures.CombinedDepthTexture : nullptr;
	}

	// One copy pass into target: the combined depth in the game's direction, with or without the near remap.
	void DrawCopy(IDirect3DDevice9* Device, IDirect3DTexture9* source, IDirect3DSurface9* target, bool remap, D3DFORMAT& sourceFormat) {
		D3DSURFACE_DESC desc = {};
		source->GetLevelDesc(0, &desc);
		sourceFormat = desc.Format;
		const D3DXVECTOR4& camera = TheRenderManager->CameraData;	// x near, y far
		const D3DXVECTOR4 params(camera.x, camera.y, TheRenderManager->IsReversedDepth() ? 1.0f : 0.0f, desc.Format == D3DFMT_R32F ? 1.0f : 0.0f);
		const D3DXVECTOR4 options(remap ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
		DepthEffect->SetVector(ParamsHandle, &params);
		DepthEffect->SetVector(OptionsHandle, &options);
		DepthEffect->SetTexture(SourceHandle, source);
		// The game's depth-stencil stays bound (same size, Z off in the pass), as ReShadeDepthCompat keeps it,
		// except under Game MSAA where D3D9 requires single-sampled RTs not to be paired with an MSAA depth-stencil.
		ReShadeIntegration::MsaaDepthGuard msaaGuard(Device);
		Device->SetRenderTarget(0, target);
		for (DWORD i = 1; i < 4; ++i) Device->SetRenderTarget(i, nullptr);
		Device->SetStreamSource(0, TheShaderManager->FrameVertex, 0, sizeof(FrameVS));
		Device->SetFVF(FrameFVF);
		UINT passes = 0;
		if (SUCCEEDED(DepthEffect->Begin(&passes, 0))) {
			if (SUCCEEDED(DepthEffect->BeginPass(0))) {
				Device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
				DepthEffect->EndPass();
			}
			DepthEffect->End();
		}
		DepthEffect->SetTexture(SourceHandle, nullptr);
	}

	bool CopyDepth(IDirect3DDevice9* Device) {
		IDirect3DTexture9* source = CombinedDepthSource();
		if (!source || DepthFailed) return false;
		if (!DepthEffect && !CreateDepthCopy(Device)) {
			DepthFailed = true;
			return false;
		}
		const bool remap = TheSettingManager->SettingsMain.Main.ReShadeDepthNearRemap;
		D3DFORMAT format = D3DFMT_UNKNOWN;
		DrawCopy(Device, source, DepthSurface, remap, format);
		DepthCopiedAt = PresentCount;
		static bool reported = false;
		if (!reported) {
			Logger::Log("UNOFFICIAL ReShade: Vegas Shaders' combined depth handed to ReShade as DEPTH (%s source, %s depth, near remap %s).",
				format == D3DFMT_R32F ? "R32F" : "G32R32F", TheRenderManager->IsReversedDepth() ? "reversed" : "standard", remap ? "on" : "off");
			reported = true;
		}
		return true;
	}

	// Under ReShade's Vulkan runtime: the copy's VkImage and DXVK's resting layout, published for the presenting thread.
	// Asked after every copy (two cheap calls): DXVK could move the texture to another image.
	void PublishForVulkan() {
		if (InteropFailed || !DepthTexture) return;
		ID3D9VkInteropTexture* interop = nullptr;
		if (FAILED(DepthTexture->QueryInterface(__uuidof(ID3D9VkInteropTexture), reinterpret_cast<void**>(&interop))) || !interop) {
			Logger::Log("UNOFFICIAL ReShade: ReShade runs as a Vulkan layer, but Vegas Shaders' depth texture has no DXVK interop interface (not DXVK, or a very old one): Vegas Shaders depth is not handed over.");
			InteropFailed = true;
			return;
		}
		uint64_t image = 0;
		uint32_t layout = 0;
		const HRESULT result = interop->GetVulkanImageInfo(&image, &layout, nullptr);
		interop->Release();
		if (FAILED(result) || !image) {
			Logger::Log("UNOFFICIAL ReShade: DXVK gave no image for Vegas Shaders' depth texture (%08lx): Vegas Shaders depth is not handed over.", result);
			InteropFailed = true;
			return;
		}
		if (image != SharedImage.load(std::memory_order_relaxed) || layout != SharedLayout.load(std::memory_order_relaxed)) {
			SharedImage.store(image, std::memory_order_relaxed);
			SharedLayout.store(layout, std::memory_order_relaxed);
			const uint32_t generation = SharedGeneration.fetch_add(1, std::memory_order_release) + 1;
			Logger::Log("UNOFFICIAL ReShade: Vegas Shaders' depth copy is DXVK image %08x%08x, resting layout %u (generation %u).",
				(unsigned)(image >> 32), (unsigned)image, layout, generation);
		}
		CopySerial.fetch_add(1, std::memory_order_release);
	}

#if NVR_TEST_TOOLS
	// DIAGNOSTIC [Main.Main.Performance] ReShadeDepthCheck 1 (development builds only; 2 = the menu guard's test switch, see
	// ForceMenuForTest; 3 = both): every 64th frame the copy is drawn
	// twice more into scratch targets, with and without the near remap, and both (plus the exported copy) are read back
	// and compared on the CPU, one log line. Shows that the remap leaves every pixel with g <= 0.5 bit-identical, what it
	// does above, and that the exported texture is the variant the switch asks for. Stalls the GPU; also runs without
	// ReShade (the copy is then made for this check only).
	struct CheckTarget { IDirect3DTexture9* texture = nullptr; IDirect3DSurface9* surface = nullptr; IDirect3DSurface9* readback = nullptr; };
	CheckTarget CheckOn, CheckOff, CheckExport;
	bool CheckFailed = false;
	UInt32 CheckFrames = 0;

	bool MakeCheckTarget(IDirect3DDevice9* Device, CheckTarget& t, bool renderTarget) {
		const UINT w = TheRenderManager->width, h = TheRenderManager->height;
		if (renderTarget && (FAILED(Device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &t.texture, NULL)) ||
			FAILED(t.texture->GetSurfaceLevel(0, &t.surface)))) return false;
		return SUCCEEDED(Device->CreateOffscreenPlainSurface(w, h, D3DFMT_R32F, D3DPOOL_SYSTEMMEM, &t.readback, NULL));
	}

	bool ReadBack(IDirect3DDevice9* Device, IDirect3DSurface9* source, IDirect3DSurface9* readback, std::vector<float>& out) {
		if (FAILED(Device->GetRenderTargetData(source, readback))) return false;
		D3DLOCKED_RECT lock = {};
		if (FAILED(readback->LockRect(&lock, NULL, D3DLOCK_READONLY))) return false;
		const UINT w = TheRenderManager->width, h = TheRenderManager->height;
		out.resize((size_t)w * h);
		for (UINT y = 0; y < h; y++) memcpy(&out[(size_t)y * w], (const char*)lock.pBits + (size_t)y * lock.Pitch, (size_t)w * sizeof(float));
		readback->UnlockRect();
		return true;
	}

	void CheckDepth(IDirect3DDevice9* Device, bool exported) {
		if (CheckFailed || !DepthEffect || (CheckFrames++ & 63) != 0) return;
		IDirect3DTexture9* source = CombinedDepthSource();
		if (!source) return;
		if (!CheckOn.surface && (!MakeCheckTarget(Device, CheckOn, true) || !MakeCheckTarget(Device, CheckOff, true) || !MakeCheckTarget(Device, CheckExport, false))) {
			Logger::Log("UNOFFICIAL ReShade depth check: scratch targets failed, check off.");
			CheckFailed = true;
			return;
		}
		D3DFORMAT format = D3DFMT_UNKNOWN;
		DrawCopy(Device, source, CheckOn.surface, true, format);
		DrawCopy(Device, source, CheckOff.surface, false, format);
		std::vector<float> on, off, exp;
		if (!ReadBack(Device, CheckOn.surface, CheckOn.readback, on) || !ReadBack(Device, CheckOff.surface, CheckOff.readback, off) ||
			(exported && !ReadBack(Device, DepthSurface, CheckExport.readback, exp))) {
			Logger::Log("UNOFFICIAL ReShade depth check: read back failed.");
			return;
		}
		const bool reversed = TheRenderManager->IsReversedDepth();
		const bool remap = TheSettingManager->SettingsMain.Main.ReShadeDepthNearRemap;
		UInt32 below = 0, belowDiffer = 0, band = 0, past = 0, sky = 0, skyWrong = 0, outside = 0, changed = 0, exportDiffer = 0;
		float bandMin = FLT_MAX, bandMax = -FLT_MAX, outMin = FLT_MAX, outMax = -FLT_MAX, maxError = 0.0f;
		for (size_t i = 0; i < off.size(); i++) {
			const float g = reversed ? off[i] : 1.0f - off[i];		// reversed projected depth before the remap
			const float r = reversed ? on[i] : 1.0f - on[i];		// after
			if (memcmp(&on[i], &off[i], sizeof(float))) changed++;
			if (!(r >= 0.0f && r < 1.0f)) outside++;
			if (!(g >= 0.0f && g < 3.0e38f)) { sky++; if (r != 0.0f) skyWrong++; }
			else if (g <= 0.5f) { below++; if (memcmp(&on[i], &off[i], sizeof(float))) belowDiffer++; }
			else {
				band++;
				if (g > 1.0f) past++;
				const float expected = min(1.0f - 0.25f / g, 0.99999994f);
				maxError = max(maxError, fabsf(r - expected));
				bandMin = min(bandMin, g); bandMax = max(bandMax, g);
				outMin = min(outMin, r); outMax = max(outMax, r);
			}
			if (exported && memcmp(&exp[i], remap ? &on[i] : &off[i], sizeof(float))) exportDiffer++;
		}
		if (!band) bandMin = bandMax = outMin = outMax = 0.0f;
		const D3DXVECTOR4& camera = TheRenderManager->CameraData;
		Logger::Log("UNOFFICIAL ReShade depth check (near %.3f, far %.1f, %s depth, %s source): %u px; changed by the remap %u; g <= 0.5: %u px, %u differ (must be 0); "
			"band g > 0.5: %u px (%u past the near plane, g > 1), g %.4f..%.4f -> %.6f..%.6f, max error against 1 - 1/(4g) %.3g; sky (g < 0 or not finite) %u px, %u not 0; "
			"outside [0,1) after the remap %u; exported copy (remap %s) %s",
			camera.x, camera.y, reversed ? "reversed" : "standard", format == D3DFMT_R32F ? "R32F" : "G32R32F", (unsigned)off.size(), changed, below, belowDiffer,
			band, past, bandMin, bandMax, outMin, outMax, maxError, sky, skyWrong, outside, remap ? "on" : "off",
			exported ? (exportDiffer ? "DIFFERS from that variant" : "matches that variant bit for bit") : "not made (no ReShade runtime)");
		if (exported && exportDiffer) Logger::Log("UNOFFICIAL ReShade depth check: %u px of the exported copy differ.", exportDiffer);
	}
#endif

	// ReShade draws into a target of the back buffer's size without multisampling here; anything else is left to
	// its usual draw at present (older ReShade versions also need the back buffer's format).
	bool CanDrawInto(IDirect3DDevice9* Device, IDirect3DSurface9* target, bool& isBackBuffer) {
		isBackBuffer = false;
		D3DSURFACE_DESC desc = {}, backDesc = {};
		if (!target || FAILED(target->GetDesc(&desc)) || desc.MultiSampleType != D3DMULTISAMPLE_NONE) return false;
		IDirect3DSurface9* backBuffer = nullptr;
		if (FAILED(Device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) || !backBuffer) return false;
		isBackBuffer = backBuffer == target;
		const bool fits = isBackBuffer || (SUCCEEDED(backBuffer->GetDesc(&backDesc)) &&
			backDesc.Width == desc.Width && backDesc.Height == desc.Height && backDesc.Format == desc.Format);
		backBuffer->Release();
		return fits;
	}

	void DrawEffects(IDirect3DDevice9* Device, IDirect3DSurface9* OutputSurface) {
		bool isBackBuffer = false;
		static bool reportedDrawn = false, reportedSkipped = false;	// each case logged once (menus may alternate them)
		const bool fits = CanDrawInto(Device, OutputSurface, isBackBuffer);
		if (fits && !reportedDrawn) {
			Logger::Log("UNOFFICIAL ReShade: effects drawn before the HUD (into %s).", isBackBuffer ? "the back buffer" : "Vegas Shaders' output, back buffer size and format");
			reportedDrawn = true;
		}
		if (!fits && !reportedSkipped) {
			Logger::Log("UNOFFICIAL ReShade: Vegas Shaders' output does not match the back buffer (size, format or multisampling); ReShade draws those frames at present, over the HUD.");
			reportedSkipped = true;
		}
		if (!fits) return;
		Device->SetRenderTarget(0, OutputSurface);
		for (DWORD i = 1; i < 4; ++i) Device->SetRenderTarget(i, nullptr);
		const uintptr_t handle = reinterpret_cast<uintptr_t>(OutputSurface);
		Runtime->render_effects(Runtime->get_command_queue()->get_immediate_command_list(), resource_view{ handle }, resource_view{ handle | 1 });
		EffectsDrawnAt = PresentCount;
	}

}

void ReShadeIntegration::Initialize() {
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(&ReShadeIntegration::Initialize), &NvrModule);
	ReShadeModule = FindReShade();
	Loaded = ReShadeModule != nullptr;
	if (!Loaded) return;
	char path[MAX_PATH] = {};
	GetModuleFileNameA(ReShadeModule, path, MAX_PATH);
	Logger::Log("UNOFFICIAL ReShade: found %s; ReShadeDepthCompat on while it is loaded.", path);
	TryRegister();
}

void ReShadeIntegration::BeforeNVRMenu() {
	// UNOFFICIAL 2026-10-25 (ReShadeUnderNVRMenu): frames where ReShade has not drawn yet (ReShadeBeforeHUD off, or NVR's
	// image space did not run: main menu, loading screens, some game menus) would get ReShade at present, over NVR's own
	// menu (bloom and the like on the menu). With the menu open, ReShade's effects are drawn here instead, after the HUD
	// and the game's menus but before NVR's menu; ReShade then skips its own draw for that frame.
	if (!Loaded || !Registered || !Runtime || !ImGuiManager::IsVisible()) return;
	if (!TheSettingManager->SettingsMain.Main.ReShadeUnderNVRMenu || EffectsDrawnAt == PresentCount) return;
	IDirect3DDevice9* Device = TheRenderManager->device;
	if (!SavedState && FAILED(Device->CreateStateBlock(D3DSBT_ALL, &SavedState))) return;
	SavedState->Capture();
	IDirect3DSurface9* targets[4] = {};
	for (DWORD i = 0; i < 4; ++i)
		if (FAILED(Device->GetRenderTarget(i, &targets[i]))) targets[i] = nullptr;
	if (targets[0]) DrawEffects(Device, targets[0]);
	for (DWORD i = 0; i < 4; ++i) {
		Device->SetRenderTarget(i, targets[i]);
		if (targets[i]) targets[i]->Release();
	}
	SavedState->Apply();
}

void ReShadeIntegration::SetMenuGuardEffects(const char* list) {
	std::lock_guard<std::mutex> lock(GuardMutex);
	if (GuardText == list) return;
	GuardText = list;
	GuardTextVersion.fetch_add(1, std::memory_order_release);
}

void ReShadeIntegration::RenderEffects(IDirect3DSurface9* OutputSurface) {
	bool check = false;
#if NVR_TEST_TOOLS
	static int checkSetting = 0;
	static UInt32 checkRead = 0;
	if ((checkRead++ % 60) == 0) checkSetting = TheSettingManager->GetSettingI("Main.Main.Performance", "ReShadeDepthCheck");
	check = (checkSetting & 1) != 0;
	ForceMenuForTest.store((checkSetting & 2) != 0, std::memory_order_relaxed);
#endif
	if (!Loaded && !check) return;
	FramesSeen++;
	if (Loaded && !Registered) {
		// Registration failed at renderer start: a few more tries over the first frames, then leave it.
		if (RegisterTries < 5 && FramesSeen % 120 == 0) TryRegister();
		if (!check) return;
	}
	const bool vulkan = VkRuntime.load() != nullptr;
	if (!Runtime && !vulkan && !check) return;
	if (Runtime || vulkan) SnapshotGameValues();
	const auto& Main = TheSettingManager->SettingsMain.Main;
	const bool copy = (Main.ReShadeNVRDepth && (Runtime || vulkan)) || check;
	const bool draw = Runtime && Main.ReShadeBeforeHUD;
	if (!copy && !draw) return;

	IDirect3DDevice9* Device = TheRenderManager->device;
	// ReShade's draw changes device state the game's render state cache does not know about: everything is
	// restored afterwards (render targets first, since setting one resets the viewport the state block restores).
	if (!SavedState && FAILED(Device->CreateStateBlock(D3DSBT_ALL, &SavedState))) return;
	SavedState->Capture();
	IDirect3DSurface9* targets[4] = {};
	IDirect3DSurface9* depthStencil = nullptr;
	for (DWORD i = 0; i < 4; ++i)
		if (FAILED(Device->GetRenderTarget(i, &targets[i]))) targets[i] = nullptr;
	Device->GetDepthStencilSurface(&depthStencil);

	if (copy && CopyDepth(Device)) {
		if (vulkan && Main.ReShadeNVRDepth) PublishForVulkan();
#if NVR_TEST_TOOLS
		if (check) CheckDepth(Device, Main.ReShadeNVRDepth && (Runtime || vulkan));
#endif
	}
	if (draw) DrawEffects(Device, OutputSurface);

	for (DWORD i = 0; i < 4; ++i) {
		Device->SetRenderTarget(i, targets[i]);
		if (targets[i]) targets[i]->Release();
	}
	Device->SetDepthStencilSurface(depthStencil);
	if (depthStencil) depthStencil->Release();
	SavedState->Apply();
}

static bool SurfaceIsMsaa(IDirect3DDevice9* device, IDirect3DSurface9** outSurface = nullptr) {
	if (!device) return false;
	IDirect3DSurface9* ds = nullptr;
	if (FAILED(device->GetDepthStencilSurface(&ds)) || !ds) return false;
	D3DSURFACE_DESC desc = {};
	const bool msaa = SUCCEEDED(ds->GetDesc(&desc)) && desc.MultiSampleType != D3DMULTISAMPLE_NONE;
	if (msaa && outSurface) *outSurface = ds;
	else ds->Release();
	return msaa;
}

bool ReShadeIntegration::KeepDepthBoundFor(IDirect3DDevice9* device) {
	if (!KeepDepthBound()) return false;
	return !SurfaceIsMsaa(device);
}

ReShadeIntegration::MsaaDepthGuard::MsaaDepthGuard(IDirect3DDevice9* d) : device(d) {
	if (SurfaceIsMsaa(d, &saved)) {
		d->SetDepthStencilSurface(nullptr);
		detached = true;
	}
}

ReShadeIntegration::MsaaDepthGuard::~MsaaDepthGuard() {
	if (detached && device) device->SetDepthStencilSurface(saved);
	if (saved) saved->Release();
}
