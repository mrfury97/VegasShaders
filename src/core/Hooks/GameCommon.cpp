// More of the game's singleton constructors: NVR keeps the objects they return, and the renderer's is where NVR starts.
namespace {
	typedef NiDX9Renderer* (__thiscall* RendererInitFn)(NiDX9Renderer*);
	typedef PlayerCharacter* (__thiscall* PlayerCtorFn)(PlayerCharacter*);
	typedef SceneGraph* (__thiscall* SceneGraphCtorFn)(SceneGraph*, char*, UInt8, NiCamera*);
	typedef MenuInterfaceManager* (__thiscall* InterfaceCtorFn)(MenuInterfaceManager*);
	typedef QueuedModelLoader* (__thiscall* ModelLoaderCtorFn)(QueuedModelLoader*);
}

RendererInitFn InitializeRenderer = (RendererInitFn)Hooks::InitializeRenderer;
void BlockGameMSAABeforeRenderer();	// NewVegas/Hooks/Settings.cpp (UNOFFICIAL 2026-11-04)
NiDX9Renderer* __fastcall InitializeRendererHook(NiDX9Renderer* self, UInt32 edx) {
	BlockGameMSAABeforeRenderer();	// every INI is read by now; TESRenderer::Init reads the MSAA settings next
	// the game allocated the renderer at NVR's size (AttachHooks), so it is NVR's RenderManager
	RenderManager* renderer = (RenderManager*)InitializeRenderer(self);
	TheRenderManager = renderer;
	renderer->Initialize();
	InitializeManagers();
	ReShadeIntegration::Initialize();	// UNOFFICIAL: NVR as a ReShade add-on, when ReShade is loaded

	// Vanilla Plus Skin has to be known here: the game makes its SKIN20xx shaders (through the CreateVertexShader /
	// CreatePixelShader hooks) during this same startup, long before xNVSE's DeferredInit message, so a check there came too
	// late and every SKIN shader was loaded from the standalone files whether VPS was installed or not.
	const bool vpsPresent = GetModuleHandle(L"VanillaPlusSkin.dll") != NULL;
	if (vpsPresent) TheShaderManager->Shaders.Skin->bVPSLoaded = true;
	// (2026-11-03: which skin shader is used is decided and logged later, SkinShaders::Templates: OverrideVanillaPlusSkin, on by default, keeps ours)
	Logger::Log(vpsPresent ? "Vanilla Plus Skin found (the skin shader in use is logged as the SKIN shaders load)" : "Vanilla Plus Skin not found");

	return renderer;
}

PlayerCtorFn NewPlayerCharacter = (PlayerCtorFn)Hooks::NewPlayerCharacter;
PlayerCharacter* __fastcall NewPlayerCharacterHook(PlayerCharacter* self, UInt32 edx) {
	return Player = NewPlayerCharacter(self);
}

SceneGraphCtorFn NewSceneGraph = (SceneGraphCtorFn)Hooks::NewSceneGraph;
SceneGraph* __fastcall NewSceneGraphHook(SceneGraph* self, UInt32 edx, char* graphName, UInt8 minFarPlane, NiCamera* camera) {
	SceneGraph* made = NewSceneGraph(self, graphName, minFarPlane, camera);
	if (strcmp(graphName, "World") == 0) WorldSceneGraph = made;	// the game makes several; NVR follows the world's
	return made;
}

InterfaceCtorFn NewMenuInterfaceManager = (InterfaceCtorFn)Hooks::NewMenuInterfaceManager;
MenuInterfaceManager* __fastcall NewMenuInterfaceManagerHook(MenuInterfaceManager* self, UInt32 edx) {
	return InterfaceManager = NewMenuInterfaceManager(self);
}

ModelLoaderCtorFn NewQueuedModelLoader = (ModelLoaderCtorFn)Hooks::NewQueuedModelLoader;
QueuedModelLoader* __fastcall NewQueuedModelLoaderHook(QueuedModelLoader* self, UInt32 edx) {
	return ModelLoader = NewQueuedModelLoader(self);
}
