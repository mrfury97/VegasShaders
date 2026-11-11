#include "PixelInspector.h"
#include "ShaderCounters.h"
#include "Diagnostics.h"	// UNOFFICIAL 2026-11-01: the stage probes share AfterFrame's call sites
#include "imgui.h"

// UNOFFICIAL 2026-10-26 (debugging tool): see PixelInspector.h and docs\next\FORK-DEBUG-PIXEL.md.

namespace PixelInspector {

bool Active = false;
int InOwnTarget = 0;

namespace {
	const char* const Section = "Main.Develop.PixelInspector";
	const UINT MaxStages = 64;
	const UINT RecordVec4 = 10;							// PixelInspect.comp: 9 texels + (size, centre)
	const UINT RecordBytes = RecordVec4 * 16;

	struct Stage { char label[64]; Kind kind; };
	struct FrameMeta { UINT count = 0; int x = 0, y = 0; UINT width = 0, height = 0; UINT frame = 0; float farZ = 0.0f; Stage stages[MaxStages]; };

	// PixelInspect.comp: set 1 binding 0 the stage's texture, set 2 binding 0 the records (written); no samplers (texelFetch).
	const NVR_EXT_BINDING_DESC Bindings[] = {
		{ 1, 0, NVR_EXT_BIND_SAMPLED_IMAGE,  0, NVR_EXT_VIEW_2D, FALSE, 0 },
		{ 2, 0, NVR_EXT_BIND_STORAGE_BUFFER, 0, NVR_EXT_VIEW_2D, TRUE,  0 },
	};
	struct Push { float u, v; UINT stage; UINT pad; };

	ID3D9NvrExtPipeline*	Pipeline = nullptr;
	ID3D9NvrExtBuffer*		Records = nullptr;
	DebugReadbackRing		Ring;
	bool					SetupFailed = false;
	bool					Enabled = false;		// the setting, read each frame in EndFrame
	bool					NeedsDxvk = false;		// enabled, but no NVR DXVK 1.5-debug: the window says so
	int						PixelX = -1, PixelY = -1;
	int						LogEvery = 0;
	UINT					FrameNumber = 0;

	FrameMeta				Current;					// the frame being recorded
	FrameMeta				Meta[DebugReadbackRing::Slots];	// per ring slot
	FrameMeta				Latest;						// the newest finished frame
	float					LatestData[MaxStages][RecordVec4][4] = {};
	bool					HaveLatest = false;
	int						LoggedX = -2, LoggedY = -2;
	UINT					LoggedFrame = 0;
	bool					Picking = false;
	UINT					ActiveSince = 0;			// results of frames before the latest switch-on are dropped
	UINT					LastWrites = 0xFFFFFFFFu;	// FrameChain::WriteCount at the last frame stage
	EffectRecord*			LastFrameEffect = nullptr;	// (without the chain) the last frame stage's effect
	UINT					LastFrameStage = 0;
	UINT					Captured = 0, Skipped = 0;	// stages recorded / refused (bound as the frame target, multisampled ...)

	bool Setup() {
		if (Pipeline && Records && Ring.Ready()) return true;
		if (SetupFailed) return false;
		SetupFailed = true;	// until everything below works
		if (!DebugReadback::Interface()) return false;
		if (!DxvkCompute::HasSpirv("PixelInspect")) {
			Logger::Log("UNOFFICIAL PIXEL INSPECTOR: %sCompute\\PixelInspect.spv is missing; off for the session.", Paths::ShaderRootShort());
			return false;
		}
		std::vector<char> words;
		if (!DxvkCompute::LoadSpirv("PixelInspect", words)) return false;
		NVR_EXT_COMPUTE_PIPELINE_DESC desc = {};
		desc.pSpirv = words.data();
		desc.SpirvSize = (UINT)words.size();
		desc.BindingCount = _countof(Bindings);
		desc.pBindings = Bindings;
		desc.PushConstantSize = sizeof(Push);
		desc.UsesSamplerHeap = FALSE;
		desc.pDebugName = "PixelInspect";
		if (FAILED(DxvkCompute::Extension()->CreateComputePipeline(&desc, &Pipeline)) || !Pipeline) {
			Pipeline = nullptr;
			Logger::Log("UNOFFICIAL PIXEL INSPECTOR: the compute pipeline was refused (DXVK's log says why); off for the session.");
			return false;
		}
		Records = DxvkCompute::CreateBufferNoFail(MaxStages * RecordBytes, NVR_EXT_BUFFER_STORAGE | NVR_EXT_BUFFER_TRANSFER_SRC, "NVR pixel inspector");
		if (!Records || !Ring.Init(MaxStages * RecordBytes, "NVR pixel inspector readback")) {
			Logger::Log("UNOFFICIAL PIXEL INSPECTOR: its buffers could not be made; off for the session.");
			return false;
		}
		SetupFailed = false;
		Logger::Log("UNOFFICIAL PIXEL INSPECTOR: ready (one 3x3 copy per stage, read back without waiting).");
		return true;
	}

	IDirect3DSurface9* SurfaceOf(IUnknown* resource, bool& release) {
		release = false;
		IDirect3DSurface9* surface = nullptr;
		if (SUCCEEDED(resource->QueryInterface(__uuidof(IDirect3DSurface9), (void**)&surface)) && surface) { release = true; return surface; }
		IDirect3DTexture9* texture = nullptr;
		if (SUCCEEDED(resource->QueryInterface(__uuidof(IDirect3DTexture9), (void**)&texture)) && texture) {
			texture->GetSurfaceLevel(0, &surface);
			texture->Release();
			if (surface) release = true;
			return surface;
		}
		return nullptr;
	}

	// UNOFFICIAL 2026-11-09 NVR_RTAOInput's y (RtAOInput.frag: Pack2(OctEncode(world normal)), 12 bits each) back to the normal.
	void RtInputNormal(float packed, float n[3]) {
		const float hi = floorf(packed / 4096.0f), lo = packed - hi * 4096.0f;
		const float ex = hi / 4095.0f * 2.0f - 1.0f, ey = lo / 4095.0f * 2.0f - 1.0f;
		n[0] = ex; n[1] = ey; n[2] = 1.0f - fabsf(ex) - fabsf(ey);
		if (n[2] < 0.0f) { const float x = n[0], y = n[1]; n[0] = (1.0f - fabsf(y)) * (x >= 0.0f ? 1.0f : -1.0f); n[1] = (1.0f - fabsf(x)) * (y >= 0.0f ? 1.0f : -1.0f); }
		const float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
		if (len > 0.0f) { n[0] /= len; n[1] /= len; n[2] /= len; }
	}

	const char* KindName(Kind kind) {
		switch (kind) { case Depth: return "depth"; case Normal: return "normal"; case Buffer: return "buffer"; case RtInput: return "rt input"; default: return "colour"; }
	}

	// One line of a stage's values (the log and the window's tooltip).
	void FormatStage(char* out, size_t size, const FrameMeta& meta, UINT i, const float (*rec)[4]) {
		const float* c = rec[4];
		float mean[4] = {};
		for (int t = 0; t < 9; t++) for (int k = 0; k < 4; k++) mean[k] += rec[t][k] / 9.0f;
		int n = _snprintf_s(out, size, _TRUNCATE, "[%02u] %-44s %4.0fx%-4.0f texel (%4.0f, %4.0f): %.5f %.5f %.5f %.5f | 3x3 mean %.5f %.5f %.5f %.5f",
			i, meta.stages[i].label, rec[9][0], rec[9][1], rec[9][2], rec[9][3], c[0], c[1], c[2], c[3], mean[0], mean[1], mean[2], mean[3]);
		if (n < 0 || n >= (int)size) return;
		if (meta.stages[i].kind == Depth) _snprintf_s(out + n, size - n, _TRUNCATE, " | %.1f units from the camera (x farZ %.0f)", c[0] * meta.farZ, meta.farZ);
		else if (meta.stages[i].kind == Normal) _snprintf_s(out + n, size - n, _TRUNCATE, " | view normal (%.3f, %.3f, %.3f)", c[0] * 2.0f - 1.0f, c[1] * 2.0f - 1.0f, c[2] * 2.0f - 1.0f);
		else if (meta.stages[i].kind == RtInput) {
			float w[3]; RtInputNormal(c[1], w);
			if (c[0] > 0.0f) _snprintf_s(out + n, size - n, _TRUNCATE, " | rays start %.1f units away, world normal (%.3f, %.3f, %.3f) (z up)", c[0], w[0], w[1], w[2]);
			else _snprintf_s(out + n, size - n, _TRUNCATE, " | no rays at this texel (x 0: sky, past the far end or outside the rect)");
		}
	}

	void LogLatest(const char* why) {
		Logger::Log("PIXEL INSPECTOR pixel (%d, %d) of %ux%u, frame %u (%s): %u stages", Latest.x, Latest.y, Latest.width, Latest.height, Latest.frame, why, Latest.count);
		char line[512];
		for (UINT i = 0; i < Latest.count; i++) {
			FormatStage(line, sizeof(line), Latest, i, LatestData[i]);
			Logger::Log("PIXEL INSPECTOR   %s", line);
		}
		LoggedX = Latest.x; LoggedY = Latest.y; LoggedFrame = Latest.frame;
	}

	void SetPixel(int x, int y) {
		TheSettingManager->SetSetting(Section, "X", x);
		TheSettingManager->SetSetting(Section, "Y", y);
		PixelX = x; PixelY = y;
	}
}

void Capture(const char* Label, IUnknown* Resource, Kind kind) {
	if (!Active || !Resource || !Label) return;
	if (Current.count >= MaxStages) { Skipped++; return; }
	IDirect3DDevice9* device = TheRenderManager->device;
	bool release = false;
	IDirect3DSurface9* surface = SurfaceOf(Resource, release);
	if (!surface) { Skipped++; return; }
	D3DSURFACE_DESC desc = {};
	if (FAILED(surface->GetDesc(&desc)) || desc.MultiSampleType != D3DMULTISAMPLE_NONE) {	// a multisampled target cannot be sampled
		if (release) surface->Release();
		Skipped++;
		return;
	}

	// A texture must not be a render target of the device while a pass reads it: unbind it for the copy (render target 0 goes to
	// the spare TESR_SourceBuffer surface, which is never drawn into here), then put everything back. SetRenderTarget resets the
	// viewport and scissor rectangle, so those are saved and restored too.
	IDirect3DSurface9* bound[4] = {};
	bool unbound[4] = {};
	bool any = false;
	for (DWORD i = 0; i < 4; i++) {
		if (FAILED(device->GetRenderTarget(i, &bound[i]))) bound[i] = nullptr;
		if (bound[i] == surface) { unbound[i] = true; any = true; }
	}
	D3DVIEWPORT9 viewport = {};
	RECT scissor = {};
	bool parked = true;
	if (any) {
		device->GetViewport(&viewport);
		device->GetScissorRect(&scissor);
		IDirect3DSurface9* park = TheTextureManager->SourceSurface;
		if (unbound[0] && (!park || park == surface)) parked = false;	// nowhere to move render target 0: skip this stage
		else {
			for (DWORD i = 0; i < 4; i++) if (unbound[i]) device->SetRenderTarget(i, i == 0 ? park : nullptr);
		}
	}

	bool recorded = false;
	if (parked) {
		const UINT index = Current.count;
		NVR_EXT_RESOURCE_BINDING resources[2] = {};
		resources[0].Slot = 0; resources[0].Type = NVR_EXT_BIND_SAMPLED_IMAGE; resources[0].pResource = surface;
		resources[1].Slot = 0; resources[1].Type = NVR_EXT_BIND_STORAGE_BUFFER; resources[1].pResource = Records;	// whole buffer (no offset alignment rules)
		Push push = { (PixelX + 0.5f) / (float)TheRenderManager->width, (PixelY + 0.5f) / (float)TheRenderManager->height, index, 0 };
		NVR_EXT_DISPATCH_DESC dispatch = {};
		dispatch.pPipeline = Pipeline;
		dispatch.BindingCount = 2;
		dispatch.pBindings = resources;
		dispatch.pPushData = &push;
		dispatch.PushDataSize = sizeof(push);
		dispatch.GroupsX = dispatch.GroupsY = dispatch.GroupsZ = 1;
		DxvkCompute::PreLoad(resources, 2);	// UNOFFICIAL 2026-10-27 (merge with the alflicker fix): managed textures on the GPU
		if (SUCCEEDED(DxvkCompute::Extension()->Dispatch(&dispatch))) {
			Stage& s = Current.stages[index];
			strncpy_s(s.label, Label, _TRUNCATE);
			s.kind = kind;
			Current.count++;
			Captured++;
			recorded = true;
		}
	}
	if (!recorded) Skipped++;

	if (any && parked) {
		for (DWORD i = 0; i < 4; i++) if (unbound[i]) device->SetRenderTarget(i, bound[i]);
		device->SetViewport(&viewport);
		device->SetScissorRect(&scissor);
	}
	for (DWORD i = 0; i < 4; i++) if (bound[i]) bound[i]->Release();
	if (release) surface->Release();
}

void AfterOwnTarget(EffectRecord* Effect, IDirect3DSurface9* Target, UINT Technique) {
	if (!Active || !Effect || !Target || !Effect->Enabled || !Effect->Effect) return;
	auto& effects = TheShaderManager->Effects;
	// Depth and normals are recorded explicitly (ShaderManager), once; the game target is a frame stage, not an own target.
	if ((EffectRecord*)effects.CombineDepth == Effect || (EffectRecord*)effects.Normals == Effect) return;
	FrameChain& chain = TheShaderManager->Chain;
	if (chain.IsActive() && Target == chain.FinalSurface()) return;
	IDirect3DCubeTexture9* cube = nullptr;
	if (SUCCEEDED(Target->GetContainer(__uuidof(IDirect3DCubeTexture9), (void**)&cube)) && cube) { cube->Release(); return; }
	const bool shadow = effects.ShadowsExteriors && Target == effects.ShadowsExteriors->Textures.ShadowPassSurface;
	char label[64];
	_snprintf_s(label, sizeof(label), _TRUNCATE, "%s t%u -> %s", Effect->Name, Technique, shadow ? "shadow buffer" : "own target");
	Capture(label, Target, Buffer);
}

void AfterFrame(EffectRecord* Effect) {
	Diagnostics::StageAfterEffect(Effect);	// UNOFFICIAL 2026-11-01: the flash detector's stage probes, at every call site of this (a no-op unless they run)
	if (!Active || !Effect) return;
	FrameChain& chain = TheShaderManager->Chain;
	IDirect3DSurface9* image = TheTextureManager->RenderedSurface;
	if (chain.IsActive()) {
		// Only when the frame was written since the last stage (an effect that did not draw leaves the count alone). After a Commit the
		// picture is the chain's current buffer; after CommitFinal or Sync (the effect drew into the game target) it is the game target.
		if (chain.WriteCount() == LastWrites) return;
		LastWrites = chain.WriteCount();
		if (chain.LastWriteKind() != 1) image = chain.FinalSurface();
	}
	else {
		if (!Effect->Enabled || !Effect->Effect) return;
		if (Effect == LastFrameEffect && LastFrameStage + 1 == Current.count) return;
	}
	char label[64];
	_snprintf_s(label, sizeof(label), _TRUNCATE, "after %s", Effect->Name);
	Capture(label, image, Color);
	LastFrameEffect = Effect;
	LastFrameStage = Current.count - 1;
}

void EndFrame() {
	FrameNumber++;
	if (Active) {
		if (Current.count) {
			const int slot = Ring.Submit(Records, Current.count * RecordBytes);
			if (slot >= 0) Meta[slot] = Current;
		}
		// Every finished frame (oldest first); the newest wins.
		static float data[MaxStages][RecordVec4][4];
		int slot = 0;
		while (Ring.Poll(data, MaxStages * RecordBytes, slot)) {
			if (Meta[slot].frame < ActiveSince) continue;	// recorded before this switch-on (a ring slot left from the last time)
			Latest = Meta[slot];
			memcpy(LatestData, data, sizeof(float) * 4 * RecordVec4 * Latest.count);
			HaveLatest = true;
		}
		if (HaveLatest && Latest.frame != LoggedFrame) {
			if (Latest.x != LoggedX || Latest.y != LoggedY) LogLatest(LoggedX == -2 ? "first result" : "new pixel");
			else if (LogEvery > 0 && Latest.frame - LoggedFrame >= (UINT)LogEvery) LogLatest("every LogEvery frames");
		}
	}

	// The settings for the next frame.
	Enabled = TheSettingManager->GetSettingI(Section, "Enabled") != 0;
	PixelX = TheSettingManager->GetSettingI(Section, "X");
	PixelY = TheSettingManager->GetSettingI(Section, "Y");
	LogEvery = TheSettingManager->GetSettingI(Section, "LogEvery");
	// UNOFFICIAL 2026-11-09 (diagnostic, the RT GI "wedge": a screen-fixed 45-degree triangle in the lower left corner, edge x = y - 1036.6
	// at 3840 wide): LogEvery = -N is the WEDGE PROBE. X and Y are ignored; the pixel steps every N frames through 18 points, three rows
	// (y = 0.65, 0.79, 0.93 of the height), in each row the edge's x - 400, - 120, - 30 (inside the wedge) and + 30, + 120, + 400 (outside),
	// scaled by width / 3840. Every new pixel's first result is logged (the "new pixel" lines), so standing still for 30 s gives several
	// rounds with nobody touching the menu. tools: share\inspect-rt\wedge_report.py reads them.
	if (LogEvery < 0) {
		static const float kRows[3] = { 0.65f, 0.79f, 0.93f };
		static const int kOffsets[6] = { -400, -120, -30, 30, 120, 400 };
		const float scale = (float)TheRenderManager->width / 3840.0f;
		const UINT step = (FrameNumber / (UINT)(-LogEvery)) % 18u;
		const int y = (int)(kRows[step / 6u] * (float)TheRenderManager->height);
		PixelY = y;
		PixelX = (int)((float)y - 1036.6f * scale + (float)kOffsets[step % 6u] * scale);
		if (PixelX < 0) PixelX = 0;
		LogEvery = 0;
		static bool logged = false;
		if (!logged && Enabled) { logged = true; Logger::Log("PIXEL INSPECTOR wedge probe on: 18 points, a new one every %d frames (edge x = y - %.1f)", -TheSettingManager->GetSettingI(Section, "LogEvery"), 1036.6f * scale); }
	}
	const bool inside = PixelX >= 0 && PixelY >= 0 && PixelX < (int)TheRenderManager->width && PixelY < (int)TheRenderManager->height;
	const bool wasActive = Active;
	Active = Enabled && inside && Setup();
	NeedsDxvk = Enabled && !DebugReadback::Ext && DebugReadback::Tried;
	if (!Active && wasActive) { LoggedX = LoggedY = -2; LoggedFrame = 0; HaveLatest = false; }	// the next switch-on logs its first result again
	if (Active && !wasActive) ActiveSince = FrameNumber + 1;
	LastFrameEffect = nullptr;
	LastWrites = TheShaderManager->Chain.WriteCount();	// the next frame's first effect is a stage only if it writes the frame (was: one spurious stage after a switch-on)
	Current.count = 0;
	Current.x = PixelX; Current.y = PixelY;
	Current.width = TheRenderManager->width; Current.height = TheRenderManager->height;
	Current.frame = FrameNumber + 1;
	Current.farZ = TheRenderManager->CameraData.y;
}

bool PixelFor(UINT Width, UINT Height, int& x, int& y) {
	const int px = TheSettingManager->GetSettingI(Section, "X"), py = TheSettingManager->GetSettingI(Section, "Y");
	if (px < 0 || py < 0 || !TheRenderManager->width || !TheRenderManager->height) return false;
	x = (int)((px + 0.5f) * Width / TheRenderManager->width);
	y = (int)((py + 0.5f) * Height / TheRenderManager->height);
	return true;
}

void DrawUI() {
	bool countersOn = false;
	for (ShaderCounters* c : ShaderCounters::All()) if (!c->Summary().empty()) countersOn = true;
	if (!Enabled && !countersOn) { Picking = false; return; }

	ImGuiIO& io = ImGui::GetIO();
	const float toScreenX = io.DisplaySize.x / (float)max(1u, (UINT)TheRenderManager->width);
	const float toScreenY = io.DisplaySize.y / (float)max(1u, (UINT)TheRenderManager->height);

	ImGui::SetNextWindowSize(ImVec2(760.0f, 420.0f), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowPos(ImVec2(40.0f, 60.0f), ImGuiCond_FirstUseEver);
	if (ImGui::Begin("Vegas Shaders Pixel Inspector")) {
		if (Enabled) {
			if (NeedsDxvk) ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Needs Vegas Shaders DXVK (the fork's d3d9.dll with extension 1.5-debug). Nothing is recorded.");
			else if (SetupFailed) ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Could not be set up (see the log): nothing is recorded.");
			int xy[2] = { PixelX, PixelY };
			ImGui::SetNextItemWidth(200.0f);
			if (ImGui::InputInt2("Pixel (x, y)", xy)) SetPixel(max(0, xy[0]), max(0, xy[1]));
			ImGui::SameLine();
			if (ImGui::Button(Picking ? "Click the picture..." : "Pick with the mouse")) Picking = !Picking;
			ImGui::SameLine();
			if (ImGui::Button("Log now") && HaveLatest) LogLatest("Log now");
			ImGui::TextDisabled("%u stages last frame recorded, %u refused; readback %s", Captured, Skipped, Ring.Ready() ? "ready" : "not set up");
			Captured = Skipped = 0;
			if (HaveLatest && ImGui::BeginTable("stages", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV, ImVec2(0.0f, 280.0f))) {
				ImGui::TableSetupScrollFreeze(0, 1);
				ImGui::TableSetupColumn("Stage", ImGuiTableColumnFlags_WidthStretch, 2.0f);
				ImGui::TableSetupColumn("Centre texel (r g b a)", ImGuiTableColumnFlags_WidthStretch, 2.2f);
				ImGui::TableSetupColumn("Meaning", ImGuiTableColumnFlags_WidthStretch, 1.6f);
				ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80.0f);
				ImGui::TableHeadersRow();
				for (UINT i = 0; i < Latest.count; i++) {
					const float (*rec)[4] = LatestData[i];
					const float* c = rec[4];
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(Latest.stages[i].label);
					if (ImGui::IsItemHovered()) {
						char line[512];
						FormatStage(line, sizeof(line), Latest, i, rec);
						ImGui::SetTooltip("%s", line);
					}
					ImGui::TableNextColumn();
					ImGui::ColorButton("##swatch", ImVec4(min(c[0], 1.0f), min(c[1], 1.0f), min(c[2], 1.0f), 1.0f), ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoAlpha, ImVec2(12.0f, 12.0f));
					ImGui::SameLine();
					ImGui::Text("%.4f %.4f %.4f %.4f", c[0], c[1], c[2], c[3]);
					ImGui::TableNextColumn();
					if (Latest.stages[i].kind == Depth) ImGui::Text("%.1f units away", c[0] * Latest.farZ);
					else if (Latest.stages[i].kind == Normal) ImGui::Text("n (%.2f %.2f %.2f)", c[0] * 2.0f - 1.0f, c[1] * 2.0f - 1.0f, c[2] * 2.0f - 1.0f);
					else ImGui::TextDisabled("%s", KindName(Latest.stages[i].kind));
					ImGui::TableNextColumn();
					ImGui::Text("%.0fx%.0f", rec[9][0], rec[9][1]);
				}
				ImGui::EndTable();
			}
			else if (!HaveLatest && !NeedsDxvk) ImGui::TextDisabled("Waiting for the first result (a few frames)...");
		}
		else ImGui::TextDisabled("Pixel inspector off ([Main.Develop.PixelInspector] Enabled).");
		for (ShaderCounters* c : ShaderCounters::All()) {
			if (c->Summary().empty()) continue;
			ImGui::Separator();
			ImGui::Text("Shader counters: %s (per frame, last second)", c->Name());
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextDisabled("%s", c->Summary().c_str());
			ImGui::PopTextWrapPos();
		}
	}
	ImGui::End();

	if (!Enabled) { Picking = false; return; }
	// The picker: the next left click on the picture (not on one of NVR's windows) chooses the pixel.
	if (Picking && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.WantCaptureMouse) {
		SetPixel(max(0, (int)(io.MousePos.x / toScreenX)), max(0, (int)(io.MousePos.y / toScreenY)));
		Picking = false;
	}
	// A marker round the pixel.
	if (PixelX >= 0 && PixelY >= 0) {
		ImDrawList* draw = ImGui::GetForegroundDrawList();
		const ImVec2 p((PixelX + 0.5f) * toScreenX, (PixelY + 0.5f) * toScreenY);
		const ImU32 colour = IM_COL32(255, 60, 200, 255);
		draw->AddLine(ImVec2(p.x - 14.0f, p.y), ImVec2(p.x - 4.0f, p.y), colour, 2.0f);
		draw->AddLine(ImVec2(p.x + 4.0f, p.y), ImVec2(p.x + 14.0f, p.y), colour, 2.0f);
		draw->AddLine(ImVec2(p.x, p.y - 14.0f), ImVec2(p.x, p.y - 4.0f), colour, 2.0f);
		draw->AddLine(ImVec2(p.x, p.y + 4.0f), ImVec2(p.x, p.y + 14.0f), colour, 2.0f);
	}
}

}
