#include "Game.h"

// The game's constructors of its singletons, wrapped so NVR keeps a pointer to each object they build.
namespace {
	typedef Main* (__thiscall* MainCtorFn)(Main*, HWND, HINSTANCE);
	typedef TES* (__thiscall* TesCtorFn)(TES*, char*, NiNode*, NiNode*, Sky*, NiNode*);
	typedef MainDataHandler* (__thiscall* DataHandlerCtorFn)(MainDataHandler*);

	ShadowSceneNode** const kShadowSceneNodeGlobal = (ShadowSceneNode**)0x011F91C8;	// the game's scene root, set while TES is built
}

MainCtorFn NewMain = (MainCtorFn)Hooks::NewMain;
Main* __fastcall NewMainHook(Main* self, UInt32 edx, HWND window, HINSTANCE instance) {
	return Global = NewMain(self, window, instance);
}

TesCtorFn NewTES = (TesCtorFn)Hooks::NewTES;
TES* __fastcall NewTESHook(TES* self, UInt32 edx, char* rootData, NiNode* objectLod, NiNode* landLod, Sky* sky, NiNode* waterLod) {
	Tes = NewTES(self, rootData, objectLod, landLod, sky, waterLod);
	SceneNode = *kShadowSceneNodeGlobal;
	return Tes;
}

DataHandlerCtorFn NewMainDataHandler = (DataHandlerCtorFn)Hooks::NewMainDataHandler;
MainDataHandler* __fastcall NewMainDataHandlerHook(MainDataHandler* self, UInt32 edx) {
	return DataHandler = NewMainDataHandler(self);
}

// The cell's root node holds one child node per kind (land, statics, ...); null when the cell has no node yet.
NiNode* TESObjectCELL::GetChildNode(CellNodes which) {
	if (NiNode* root = GetNode()) {
		if (root->m_children.data && static_cast<UInt32>(which) < root->m_children.capacity &&
			static_cast<UInt32>(which) < root->m_children.end)
			return static_cast<NiNode*>(root->m_children.data[which]);
	}
#if _DEBUG
	Logger::Log("[ TESObjectCELL::GetChildNode ] %s has no master node", fullName.name.m_data);
#endif
	return nullptr;
}
