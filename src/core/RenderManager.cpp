#include "ShaderWarmUp2.h"	// UNOFFICIAL 2026-10-18: ShaderWarmUp2 device hooks go in with the render manager
#include "DxvkCompute.h"	// UNOFFICIAL 2026-10-23 (NVR DXVK)
#include "DxvkLoader.h"	// UNOFFICIAL 2026-11-04: the d3d9.dll in use may be a mod's (DxvkLoader.h)
#include "RtLampShadows.h"	// UNOFFICIAL 2026-10-28 RT lamp shadows
#include "RtSunShadows.h"	// UNOFFICIAL 2026-11-13 RT sun shadows' ForwardLookup
#include "MixCheck.h"	// UNOFFICIAL 2026-10-30 NvrDxvkMixCheck
#include "TestTools.h"
#if NVR_TEST_TOOLS
#include "SettingShots.h"	// UNOFFICIAL, test only (includes FrameRecorder.h)
#endif

// The point size value that tells an AMD/Intel driver (and DXVK) to resolve the bound depth texture (the RESZ trick).
static const DWORD kReszResolve = 0x7FA05000;

// Rows 0..2 of the result hold the rotation transposed and scaled; row 3 holds the position relative to the camera.
// Addressed as m[row][col], which is the same storage as _11.._44.
void RenderManager::CreateD3DMatrix(D3DMATRIX* out, NiTransform* xf) {
	const float scale = xf->scale;
	for (int row = 0; row < 3; row++) {
		for (int col = 0; col < 3; col++)
			out->m[row][col] = xf->rot.data[col][row] * scale;
		out->m[row][3] = 0.0f;
	}
	NiPoint3& at = xf->pos;
	out->m[3][0] = at.x - CameraPosition.x;
	out->m[3][1] = at.y - CameraPosition.y;
	out->m[3][2] = at.z - CameraPosition.z;
	out->m[3][3] = 1.0f;
}

// Vector from the camera's position to the centre of a bound.
static NiPoint3 CentreFromCamera(NiCamera* cam, NiBound* Bound) {
	NiPoint3& eye = cam->m_worldTransform.pos;
	return NiPoint3{ Bound->Center.x - eye.x, Bound->Center.y - eye.y, Bound->Center.z - eye.z };
}

void RenderManager::GetScreenSpaceBoundSize(NiPoint2* sizeOut, NiBound* sphere, float tolerance) {
	NiCamera* cam = WorldSceneGraph->camera;
	NiPoint3 offset = CentreFromCamera(cam, sphere);
	float depth = offset * offset;
	float radius = sphere->Radius;

	// A bound sitting on the camera fills the screen.
	if (depth < tolerance) {
		if (depth > -tolerance) {
			sizeOut->x = sizeOut->y = FLT_MAX;
			return;
		}
		depth = -depth;
	}

	if (!cam->Frustum.Ortho) radius /= depth;
	sizeOut->x = radius * 2.0f / (cam->Frustum.Right - cam->Frustum.Left);
	sizeOut->y = radius * 2.0f / (cam->Frustum.Top - cam->Frustum.Bottom);
}

float RenderManager::GetObjectDistance(NiBound* Bound) {
	NiPoint3 offset = CentreFromCamera(WorldSceneGraph->camera, Bound);
	return sqrt(offset * offset);
}

bool RenderManager::IsReversedDepth() {
	const float clearDepth = NiDX9Renderer::GetSingleton()->m_fZClear;
	return clearDepth < 1.0f;
}

void RenderManager::UpdateSceneCameraData() {
	NiCamera* cam = WorldSceneGraph->camera;
	if (!cam) return;

	NiMatrix33& axes = cam->m_worldTransform.rot;
	NiPoint3& place = cam->m_worldTransform.pos;
	CameraForward.x = axes.data[0][0];
	CameraForward.y = axes.data[1][0];
	CameraForward.z = axes.data[2][0];
	CameraPosition = place.toD3DXVEC4();
}

void RenderManager::SetupSceneCamera() {
	NiCamera* cam = WorldSceneGraph->camera;
	if (!cam) return;

	NiPoint3 eye = *Pointers::Generic::CameraLocation;
	NiMatrix33& axes = cam->m_worldTransform.rot;
	NiPoint3& place = cam->m_worldTransform.pos;
	NiFrustum& frustum = cam->Frustum;

	const float width = frustum.Right - frustum.Left;
	const float height = frustum.Top - frustum.Bottom;
	const float aspect = width / height;
	const float zNear = frustum.Near;
	const float zFar = frustum.Far;
	const float zStep = 1.0f / (zFar - zNear);
	const float sumX = frustum.Right + frustum.Left;	// 0 for a viewport centred on 0
	const float sumY = frustum.Top + frustum.Bottom;	// 0 for a viewport centred on 0

	// FOVData: x and y are the field of view in degrees (x horizontal, y the game's vertical value); z and w in radians.
	const float verticalDeg = WorldSceneGraph->cameraFOV;
	const float verticalRad = D3DXToRadian(verticalDeg);
	const float horizontalRad = 2 * atan(tan(verticalRad * 0.5) * aspect);
	FOVData = D3DXVECTOR4(D3DXToDegree(horizontalRad), verticalDeg, horizontalRad, verticalRad);

	memcpy(Pointers::Generic::CameraWorldTranslate, &place, 0x0C);

	// The camera's axes are the columns of its rotation.
	NiPoint3 fwd = { axes.data[0][0], axes.data[1][0], axes.data[2][0] };
	NiPoint3 up = { axes.data[0][1], axes.data[1][1], axes.data[2][1] };
	NiPoint3 side = { axes.data[0][2], axes.data[1][2], axes.data[2][2] };

	// World: only a translation, by minus the camera position.
	for (int r = 0; r < 4; r++)
		for (int c = 0; c < 4; c++)
			worldMatrix.m[r][c] = (r == c) ? 1.0f : 0.0f;
	worldMatrix._41 = -place.x;
	worldMatrix._42 = -place.y;
	worldMatrix._43 = -place.z;

	// View: the axes as columns in the order side, up, fwd. The translation row is minus each axis dotted with the eye.
	NiPoint3 axisOf[3] = { side, up, fwd };
	for (int c = 0; c < 3; c++) {
		viewMatrix.m[0][c] = axisOf[c].x;
		viewMatrix.m[1][c] = axisOf[c].y;
		viewMatrix.m[2][c] = axisOf[c].z;
	}
	for (int r = 0; r < 3; r++) viewMatrix.m[r][3] = 0.0f;
	for (int c = 0; c < 3; c++) viewMatrix.m[3][c] = -(axisOf[c] * eye);
	viewMatrix.m[3][3] = 1.0f;

	// Inverse view: the rotation's transpose, with the eye as the translation.
	for (int r = 0; r < 3; r++) {
		invViewMatrix.m[r][0] = axisOf[r].x;
		invViewMatrix.m[r][1] = axisOf[r].y;
		invViewMatrix.m[r][2] = axisOf[r].z;
		invViewMatrix.m[r][3] = 0.0f;
	}
	invViewMatrix.m[3][0] = eye.x;
	invViewMatrix.m[3][1] = eye.y;
	invViewMatrix.m[3][2] = eye.z;
	invViewMatrix.m[3][3] = 1.0f;

	// NVR's own copies of the two
	memcpy(&InvViewMatrix, &invViewMatrix, sizeof(D3DXMATRIX));
	memcpy(&ViewMatrix, &viewMatrix, sizeof(D3DXMATRIX));

	// Projection (see the Microsoft projection-transform page). A reversed depth buffer flips the z terms.
	const bool reversed = IsReversedDepth();
	const float zTerm = reversed ? -(zNear * zStep) : zFar * zStep;
	const float wTerm = reversed ? zNear * zFar * zStep : -(zNear * zFar * zStep);
	projMatrix = D3DXMATRIX(2.0f / width, 0.0f, 0.0f, 0.0f,
		0.0f, 2.0f / height, 0.0f, 0.0f,
		-sumX / width, -sumY / height, zTerm, 1.0f,
		0.0f, 0.0f, wTerm, 0.0f);

	// products as D3DX's operator* does them (D3DXMatrixMultiply, left to right)
	D3DXMATRIX worldView;
	D3DXMatrixMultiply(&worldView, &worldMatrix, &viewMatrix);
	D3DXMatrixMultiply(&WorldViewProjMatrix, &worldView, &projMatrix);
	D3DXMatrixMultiply(&ViewProjMatrix, &viewMatrix, &projMatrix);
	D3DXMatrixInverse(&InvProjMatrix, nullptr, &projMatrix);
	D3DXMatrixMultiply(&InvViewProjMatrix, &InvProjMatrix, &invViewMatrix);

	CameraForward = fwd.toD3DXVEC4();
	CameraPosition = place.toD3DXVEC4();

	// Depth reconstruction: x is the near plane (from CombineDepth), z is 1 when the depth buffer is reversed.
	DepthConstants = D3DXVECTOR4(TheShaderManager->Effects.CombineDepth->Constants.viewNearZ, 0.0f, reversed ? 1.0f : 0.0f, 1.0f);
	// camera: near, far, aspect, horizontal field of view in degrees
	CameraData = D3DXVECTOR4(zNear, zFar, width / height, FOVData.x);
}

// UNOFFICIAL 2026-11-12 [Main.Main.Scope] OwnCamera (core/ScopeCamera.h; ShaderManager::BeginExtraCamera / EndExtraCamera use these).
// The main camera's values of this object's own members, kept while another camera's scene is drawn and put back after it.
void RenderManager::SaveCameraState(CameraState* state) {
	state->WorldViewProj = WorldViewProjMatrix;
	state->ViewProj = ViewProjMatrix;
	state->InvViewProj = InvViewProjMatrix;
	state->InvProj = InvProjMatrix;
	state->InvView = InvViewMatrix;
	state->View = ViewMatrix;
	state->CameraData = CameraData;
	state->FOVData = FOVData;
	state->CameraForward = CameraForward;
	state->CameraPosition = CameraPosition;
}

void RenderManager::RestoreCameraState(const CameraState* state) {
	WorldViewProjMatrix = state->WorldViewProj;
	ViewProjMatrix = state->ViewProj;
	InvViewProjMatrix = state->InvViewProj;
	InvProjMatrix = state->InvProj;
	InvViewMatrix = state->InvView;
	ViewMatrix = state->View;
	CameraData = state->CameraData;
	FOVData = state->FOVData;
	CameraForward = state->CameraForward;
	CameraPosition = state->CameraPosition;
}

// SetupSceneCamera's values for ANOTHER camera (a picture-in-picture scope's), into this object's own members only: what
// TESR_InvViewTransform, TESR_InvProjectionTransform, TESR_ViewProjectionTransform, TESR_InvViewProjectionTransform,
// TESR_WorldViewProjectionTransform, TESR_CameraPosition, TESR_CameraForward and TESR_CameraData read. The same formulas as above, from
// the camera's own frustum and world transform. NOT written: the renderer's worldMatrix / viewMatrix / projMatrix / invViewMatrix and
// the game's camera globals (the game fills those itself when it makes this camera current for the scene: BSShaderUtil::RenderScene calls
// NiRenderer::SetCurrentCamera first), and DepthConstants (it describes the frame's depth texture, not this picture's). The field of
// view comes from the frustum (WorldSceneGraph->cameraFOV is the main camera's). projection: the matrix made here, for the caller.
bool RenderManager::SetupExtraCamera(NiCamera* cam, D3DXMATRIX* projection) {
	if (!cam || !projection) return false;
	const NiFrustum& frustum = cam->Frustum;
	const float width = frustum.Right - frustum.Left;
	const float height = frustum.Top - frustum.Bottom;
	const float zNear = frustum.Near;
	const float zFar = frustum.Far;
	// a perspective frustum with room between its planes: anything else is not a scope's camera (nothing is changed)
	if (frustum.Ortho || !(width > 1.0e-6f) || !(height > 1.0e-6f) || !(zNear > 0.0f) || !(zFar > zNear)) return false;
	const float zStep = 1.0f / (zFar - zNear);
	const float sumX = frustum.Right + frustum.Left;
	const float sumY = frustum.Top + frustum.Bottom;

	const NiMatrix33& axes = cam->m_worldTransform.rot;
	const NiPoint3& place = cam->m_worldTransform.pos;
	// The same eye as SetupSceneCamera's view (the game's camera location, about 0: every draw is relative to its camera).
	const NiPoint3 gameEye = *Pointers::Generic::CameraLocation;
	const D3DXVECTOR3 eye(gameEye.x, gameEye.y, gameEye.z);
	// The camera's axes are the columns of its rotation.
	const D3DXVECTOR3 fwd(axes.data[0][0], axes.data[1][0], axes.data[2][0]);
	const D3DXVECTOR3 up(axes.data[0][1], axes.data[1][1], axes.data[2][1]);
	const D3DXVECTOR3 side(axes.data[0][2], axes.data[1][2], axes.data[2][2]);
	const D3DXVECTOR3 axisOf[3] = { side, up, fwd };

	D3DXMATRIX world, view, invView;
	D3DXMatrixIdentity(&world);
	world._41 = -place.x;
	world._42 = -place.y;
	world._43 = -place.z;
	D3DXMatrixIdentity(&view);
	D3DXMatrixIdentity(&invView);
	for (int c = 0; c < 3; c++) {
		view.m[0][c] = axisOf[c].x;
		view.m[1][c] = axisOf[c].y;
		view.m[2][c] = axisOf[c].z;
		view.m[3][c] = -D3DXVec3Dot(&axisOf[c], &eye);
	}
	for (int r = 0; r < 3; r++) {
		invView.m[r][0] = axisOf[r].x;
		invView.m[r][1] = axisOf[r].y;
		invView.m[r][2] = axisOf[r].z;
	}
	invView.m[3][0] = eye.x;
	invView.m[3][1] = eye.y;
	invView.m[3][2] = eye.z;

	const bool reversed = IsReversedDepth();
	const float zTerm = reversed ? -(zNear * zStep) : zFar * zStep;
	const float wTerm = reversed ? zNear * zFar * zStep : -(zNear * zFar * zStep);
	*projection = D3DXMATRIX(2.0f / width, 0.0f, 0.0f, 0.0f,
		0.0f, 2.0f / height, 0.0f, 0.0f,
		-sumX / width, -sumY / height, zTerm, 1.0f,
		0.0f, 0.0f, wTerm, 0.0f);

	D3DXMATRIX worldView;
	D3DXMatrixMultiply(&worldView, &world, &view);
	D3DXMatrixMultiply(&WorldViewProjMatrix, &worldView, projection);
	D3DXMatrixMultiply(&ViewProjMatrix, &view, projection);
	D3DXMatrixInverse(&InvProjMatrix, nullptr, projection);
	D3DXMatrixMultiply(&InvViewProjMatrix, &InvProjMatrix, &invView);
	InvViewMatrix = invView;
	ViewMatrix = view;

	CameraForward = D3DXVECTOR4(fwd.x, fwd.y, fwd.z, 1.0f);
	CameraPosition = D3DXVECTOR4(place.x, place.y, place.z, 1.0f);
	// The frustum's sides are tangents (at distance 1): the angles follow from them. x, y in degrees (x horizontal), z, w in radians.
	const float horizontalRad = 2.0f * atanf(0.5f * width);
	const float verticalRad = 2.0f * atanf(0.5f * height);
	FOVData = D3DXVECTOR4(D3DXToDegree(horizontalRad), D3DXToDegree(verticalRad), horizontalRad, verticalRad);
	CameraData = D3DXVECTOR4(zNear, zFar, width / height, FOVData.x);
	return true;
}

// detect if dxvk is present
// UNOFFICIAL 2026-10-27: see RenderManager::Initialize (stock DXVK on NVIDIA: no world depth). Shown once on screen, the first
// frame the player is in the world (the interface does not exist yet while the render manager starts).
static bool NvrDepthMissingUnderDXVK = false;
void NvrDepthWarningTick() {
	static bool shown = false;
	if (!NvrDepthMissingUnderDXVK || shown || !Player || !Player->parentCell || !InterfaceManager) return;
	shown = true;
	InterfaceManager->ShowMessage("Vegas Shaders: no depth under this DXVK on NVIDIA. Add d3d9.customVendorId = 1002 to dxvk.conf (see the Vegas Shaders log).");
}

// True when the d3d9.dll the game's Direct3D comes from has the ProductName "DXVK".
static bool getDXVKPresent() {
	char path[MAX_PATH] = {};
	if (!GetModuleFileNameA(DxvkLoader::Runtime(), path, MAX_PATH)) return false;
	DWORD ignored = 0;
	const DWORD infoSize = GetFileVersionInfoSizeA(path, &ignored);
	if (infoSize == 0) return false;

	std::vector<char> info(infoSize);
	if (!GetFileVersionInfoA(path, ignored, infoSize, info.data())) {
		Logger::Log("couldn't find file version info");
		return false;
	}

	struct LangCodepage { WORD language, codePage; }* table = nullptr;

	UINT tableBytes = 0;
	if (!VerQueryValueA(info.data(), "\\VarFileInfo\\Translation", (LPVOID*)&table, &tableBytes)) return false;
	if (!tableBytes) return false;

	const UINT entries = tableBytes / sizeof(LangCodepage);
	for (UINT i = 0; i < entries; i++) {
		char key[256];
		StringCchPrintfA(key, 256, "\\StringFileInfo\\%04x%04x\\ProductName", table[i].language, table[i].codePage);

		char* product = NULL;
		UINT productBytes;
		VerQueryValueA(info.data(), key, (LPVOID*)&product, &productBytes);

		if (!memcmp(product, "DXVK", 4)) return true;
	}
	return false;
}

// Which Direct3D 9 implementation the game is running on, for the log. The DXVK check is the same one
// used below (ProductName of the d3d9.dll the process loads); the path shows where that file lives.
const char* RenderManager::D3D9RuntimeDescription() {
	static char text[MAX_PATH + 64] = {};
	if (!text[0]) {
		char path[MAX_PATH] = {};
		HMODULE module = DxvkLoader::Runtime();
		if (module) GetModuleFileNameA(module, path, MAX_PATH);
		_snprintf_s(text, sizeof(text), _TRUNCATE, "%s (%s)", getDXVKPresent() ? "DXVK" : "system Direct3D 9 or another wrapper", path[0] ? path : "path unknown");
	}
	return text;
}

// DXVK can report another GPU to the game than the one installed: its hideNvidiaGpu option shows an
// NVIDIA card as "AMD Radeon RX 6700 XT". Nothing in NVR depends on the name, but it confused a player
// reading the log, so the log also lists the display adapters Windows itself knows (user32, which DXVK
// does not replace).
static void LogWindowsDisplayAdapters(const char* reportedName) {
	std::string adapters;
	bool reportedSeen = false;
	DISPLAY_DEVICEA device = {};
	device.cb = sizeof(device);
	for (DWORD index = 0; EnumDisplayDevicesA(nullptr, index, &device, 0); ++index, device.cb = sizeof(device)) {
		if (device.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) continue;
		const std::string name = device.DeviceString;
		if (name.empty() || (" " + adapters + ",").find(" " + name + ",") != std::string::npos) continue;
		adapters += adapters.empty() ? name : ", " + name;
		if (name == reportedName) reportedSeen = true;
	}
	if (adapters.empty()) return;
	Logger::Log("Windows display adapters: %s", adapters.c_str());
	if (!reportedSeen)
		Logger::Log("The game is told about a different GPU than Windows lists. DXVK does this on purpose (its hideNvidiaGpu "
			"option shows NVIDIA cards as AMD Radeon RX 6700 XT); Vegas Shaders does not depend on the name.");
}

void RenderManager::Initialize() {
	Logger::Log("Extending the render manager...");
	CameraForward = CameraPosition = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);
	BackBuffer = nullptr;
	SetRect(&SaveGameScreenShotRECT, 0, 0, 256, 144);	// the save game picture's size
	IsSaveGameScreenShot = false;

	IDirect3D9* d3d = NULL;
	device->GetDirect3D(&d3d);
	D3DDISPLAYMODE displayMode;
	d3d->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &displayMode);
	RESZ = d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, displayMode.Format, D3DUSAGE_RENDERTARGET, D3DRTYPE_SURFACE, (D3DFORMAT)MAKEFOURCC('R','E','S','Z')) == D3D_OK;
	D3DADAPTER_IDENTIFIER9 adapter = {};
	if (SUCCEEDED(d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &adapter)))
		Logger::Log("Graphics adapter as reported to the game: %s (vendor %04lX, device %04lX)", adapter.Description, adapter.VendorId, adapter.DeviceId);
	LogWindowsDisplayAdapters(adapter.Description);
	d3d->Release();

	// The depth resolve path follows what the D3D9 layer offers, not the GPU brand: AMD and Intel drivers
	// offer RESZ; NVAPI works only with NVIDIA's own D3D9 driver. (This used to log "AMD/Intel detected" for
	// NVIDIA cards under DXVK, which confused users.) UNOFFICIAL 2026-10-26: stock DXVK offers RESZ only when
	// the adapter reports AMD (d3d9.customVendorId = 1002 on NVIDIA); NVR DXVK offers it on every GPU (fork
	// patch 0007). Without RESZ under DXVK, NVAPI initializes but cannot reach DXVK's device: no depth.
	DXVK = RESZ ? getDXVKPresent() : false;
	if (RESZ) {
		Logger::Log("Depth resolve: RESZ (%s).", DXVK ? "DXVK, any GPU brand" : "AMD/Intel driver");
		if (DXVK) { Logger::Log("DXVK found"); }
	}
	else if (NvAPI_Initialize() == NVAPI_OK) {
		Logger::Log("Depth resolve: NVAPI (NVIDIA driver).");
		// UNOFFICIAL 2026-10-27: stock DXVK on an NVIDIA card offers no RESZ (only when the adapter reports AMD), and NVAPI cannot reach
		// DXVK's device: NVR gets no world depth, so AO, fog, shadows, reflections and everything else that reads depth go wrong.
		// Said once in the log here and once on screen in game (NvrDepthWarningTick); the old path stays as it was.
		if (getDXVKPresent()) {
			NvrDepthMissingUnderDXVK = true;
			Logger::Log("WARNING: this d3d9.dll is DXVK without RESZ on an NVIDIA card: Vegas Shaders gets NO world depth (ambient occlusion, fog, "
				"shadows, reflections and other depth effects will look wrong). Fix: add the line  d3d9.customVendorId = 1002  to dxvk.conf "
				"next to d3d9.dll (DXVK then offers RESZ), or use Vegas Shaders DXVK, which offers it on every GPU.");
		}
	}
	else Logger::Log("ERROR: Cannot initialize the render manager. Graphics device not supported.");
	Logger::Log("D3D9 runtime: %s", D3D9RuntimeDescription());
	const UInt8 anisotropy = TheSettingManager->SettingsMain.Main.AnisotropicFilter;
	if (anisotropy >= 2) device->SetSamplerState(0, D3DSAMP_MAXANISOTROPY, anisotropy);
	BackBuffer = this->CreateHDRRenderTarget();	// the HDR back buffer NVR renders into
#if NVR_TEST_TOOLS
	FrameRecorder::EarlyStart(device, SettingShots::RequestFile);	// UNOFFICIAL, test only: 'record' in the shot list (upload journal)
#endif
	DxvkCompute::Initialize(device);	// UNOFFICIAL 2026-10-23 (NVR DXVK): is the NVR extension on this d3d9.dll? (logged)
	RtLampShadows::Initialize();	// UNOFFICIAL 2026-10-28: POINT_SHADOW_RT for the session, before any shader is loaded (logged)
	RtSunShadows::Initialize();	// UNOFFICIAL 2026-11-13: RT_SUN_FORWARD for the session, likewise
	ShaderWarmUp2::Install(device);	// UNOFFICIAL ShaderWarmUp2 (off unless [Main.Main.Performance] ShaderWarmUp2 is 1 or 2): before any shader is created
	SessionLog::OnDevice(device);	// UNOFFICIAL 2026-11-01: the session header's GPU, D3D9 runtime and screen lines (SessionLog.h)
}

void RenderManager::ResolveDepthBuffer(IDirect3DTexture9* depthTarget) {
	MixCheck::NoteWrite(depthTarget, MixCheck::kD3D9, "DepthResolve");	// UNOFFICIAL 2026-10-30 NvrDxvkMixCheck (RESZ writes it without a draw)

	if (RESZ) {
		// RESZ: one point drawn with the depth texture bound, and the point-size code after it, make the driver copy the depth
		// buffer into the texture. Everything the draw changes is read first and put back after.
		static const D3DRENDERSTATETYPE kTouched[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_COLORWRITEENABLE };
		DWORD keptStates[3];
		DWORD keptFVF;
		UINT32 keptOffset, keptStride;
		IDirect3DVertexDeclaration9* keptDecl = nullptr;
		IDirect3DBaseTexture9* keptTexture = nullptr;
		IDirect3DVertexShader9* keptVS = nullptr;
		IDirect3DPixelShader9* keptPS = nullptr;
		IDirect3DVertexBuffer9* keptStream = nullptr;

		const D3DXVECTOR3 onePoint(0.0f, 0.0f, 0.0f);
		device->GetFVF(&keptFVF);
		device->GetVertexDeclaration(&keptDecl);
		device->GetTexture(0, &keptTexture);
		device->GetVertexShader(&keptVS);
		device->GetPixelShader(&keptPS);
		device->GetStreamSource(0, &keptStream, &keptOffset, &keptStride);
		for (int i = 0; i < 3; i++) device->GetRenderState(kTouched[i], &keptStates[i]);

		device->SetVertexShader(nullptr);
		device->SetPixelShader(nullptr);
		device->SetFVF(D3DFVF_XYZ);
		device->SetTexture(0, depthTarget);
		for (int i = 0; i < 3; i++) device->SetRenderState(kTouched[i], 0);	// no depth test, no depth or colour writes
		device->DrawPrimitiveUP(D3DPT_POINTLIST, 1, &onePoint, sizeof(D3DXVECTOR3));
		for (int i = 0; i < 3; i++) device->SetRenderState(kTouched[i], keptStates[i]);
		device->SetRenderState(D3DRS_POINTSIZE, kReszResolve);
		device->SetRenderState(D3DRS_POINTSIZE, 0);

		device->SetFVF(keptFVF);
		device->SetVertexDeclaration(keptDecl);
		device->SetTexture(0, keptTexture);
		device->SetVertexShader(keptVS);
		device->SetPixelShader(keptPS);
		device->SetStreamSource(0, keptStream, keptOffset, keptStride);

		IUnknown* references[] = { keptTexture, keptStream, keptVS, keptPS, keptDecl };	// the Get calls above added these
		for (IUnknown* held : references) if (held) held->Release();
	}
	else {
		// NVAPI path: the game's depth surface (or its INTZ texture) is stretched into the depth texture.
		TextureManager* textures = TheTextureManager;
		if (textures->DepthSurface == nullptr) {
			// first call: find the game's depth surface; an INTZ one is read through its texture
			device->GetDepthStencilSurface(&textures->DepthSurface);
			D3DSURFACE_DESC gameDepth;
			textures->DepthSurface->GetDesc(&gameDepth);
			const bool isIntz = gameDepth.Format == (D3DFORMAT)MAKEFOURCC('I', 'N', 'T', 'Z');
			if (isIntz) {
				void* owner = nullptr;
				textures->DepthSurface->GetContainer(IID_IDirect3DTexture9, &owner);
				textures->DepthTextureINTZ = static_cast<IDirect3DTexture9*>(owner);
			}
			NvAPI_D3D9_RegisterResource(isIntz ? (IDirect3DResource9*)textures->DepthTextureINTZ : (IDirect3DResource9*)textures->DepthSurface);
		}

		// UNOFFICIAL 2026-10-30 (terrain flicker deep dive): the game's depth surface can be larger than the picture (seen: 1024 lines
		// tall with a 720-line picture, native D3D9 on NVIDIA with the game's MSAA off). A copy of the whole surface squashed it into the
		// depth texture: below 720/1024 of the screen every pixel read far (the near ground fogged white). Only the picture's own area
		// (top left, the size of the depth texture) is copied.
		RECT source = {}, *sourceRect = NULL;
		D3DSURFACE_DESC from = {}, to = {};
		if (SUCCEEDED(textures->DepthSurface->GetDesc(&from)) && SUCCEEDED(depthTarget->GetLevelDesc(0, &to)) &&
			(from.Width != to.Width || from.Height != to.Height)) {
			source.right = (LONG)(std::min)(from.Width, to.Width);
			source.bottom = (LONG)(std::min)(from.Height, to.Height);
			sourceRect = &source;
			static bool told = false;
			if (!told) { told = true; Logger::Log("UNOFFICIAL depth (NVAPI): the game's depth surface is %ux%u, the depth texture %ux%u: the top-left %ix%i is copied.", from.Width, from.Height, to.Width, to.Height, source.right, source.bottom); }
		}
		RECT target = { 0, 0, source.right, source.bottom };
		IDirect3DResource9* copyFrom = textures->DepthTextureINTZ ? (IDirect3DResource9*)textures->DepthTextureINTZ : (IDirect3DResource9*)textures->DepthSurface;
#if NVR_TEST_TOOLS
		// UNOFFICIAL, test only: NvAPI bypasses the device, so the frame recorder notes the copy itself (FrameRecorder.h; whole surface).
		FrameRecorder::NvStretchRect(copyFrom, depthTarget);
#endif
		NvAPI_D3D9_StretchRectEx(device, copyFrom, sourceRect, depthTarget, sourceRect ? &target : NULL, D3DTEXF_NONE);
	}
}

// Set when the NVR screenshot key saved a screenshot this frame (plain globals: the game allocates RenderManager
// itself, so members it adds are not constructed).
char LastScreenshotBase[MAX_PATH];
char LastScreenshotName[80];
bool ScreenshotTakenThisFrame = false;

void RenderManager::CheckAndTakeScreenShot(IDirect3DSurface9* RenderTarget, bool HDR) {
	if (!Global->OnKeyDown(TheSettingManager->SettingsMain.Main.ScreenshotKey)) return;

	char folder[MAX_PATH];
	char stamp[80];
	const time_t now = time(NULL);

	GetCurrentDirectoryA(MAX_PATH, folder);
	strcat(folder, "\\Screenshots");
	if (GetFileAttributesA(folder) == INVALID_FILE_ATTRIBUTES) CreateDirectoryA(folder, NULL);
	strftime(stamp, 80, "\\%Y%m%d %H.%M.%S", localtime(&now));
	strcat(folder, stamp);
	strcpy_s(LastScreenshotBase, folder);  // for the reflection probe (Hooks/Render.cpp)
	strcpy_s(LastScreenshotName, stamp + 1);
	// UNOFFICIAL 2026-10-13 (a tester: one screenshot saved a pile of pictures): the old reflection probe (P54, Hooks/Render.cpp)
	// is no longer started by a screenshot. And one extension (it saved "<time>.jpg.png").
	strcat(folder, HDR ? ".hdr" : ".png");
	if (HDR)
		D3DXSaveSurfaceToFileA(folder, D3DXIFF_HDR, RenderTarget, NULL, NULL);
	else
		D3DXSaveSurfaceToFileA(folder, D3DXIFF_PNG, RenderTarget, NULL, NULL);

	InterfaceManager->ShowMessage("Screenshot taken!");
}


// The detector-window node: a 2048-slot node the render manager publishes through the game's global.
void DWNode::Create() {
	DWNode* node = (DWNode*)Pointers::Functions::MemoryAlloc(sizeof(DWNode));
	node->New(2048);
	node->SetName("Passes...");
	*Pointers::Generic::DetectorWindowNode = node;
}

DWNode* DWNode::Get() {
	NiNode* published = *Pointers::Generic::DetectorWindowNode;
	return static_cast<DWNode*>(published);
}

void DWNode::AddNode(const char* label, NiAVObject* first, NiAVObject* second) {
	NiNode* holder = (NiNode*)Pointers::Functions::MemoryAlloc(sizeof(NiNode));
	holder->New(2);

	holder->SetName(label);
	// Both parameters' addresses are stored, as the original code does (the children are not re-parented here).
	holder->m_children.Add(&first);
	holder->m_children.Add(&second);
	(*Pointers::Generic::DetectorWindowNode)->AddObject(holder, 1);
}

// UNOFFICIAL: the caps do not change for a device; VolumetricFog::RenderDedicated and CombineDepth::RenderWithNormals
// used to ask the runtime for them every frame. Plain statics: the game allocates RenderManager itself, so members
// added to it are not constructed.
const D3DCAPS9* RenderManager::DeviceCaps() {
	static D3DCAPS9 caps = {};
	static IDirect3DDevice9* capsDevice = nullptr;
	static bool valid = false;
	if (!valid || capsDevice != device) {
		capsDevice = device;
		valid = device && SUCCEEDED(device->GetDeviceCaps(&caps));
	}
	return valid ? &caps : nullptr;
}
