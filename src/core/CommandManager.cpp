// Script command table. The command names, help texts, parameter lists, the
// registration order (= opcode order) and the results are public: other mods call them.

#include "Branding.h"
#include "StalePresetValues.h"	// UNOFFICIAL 2026-11-10

namespace {

	CommandParam MakeArg(const char* typeName, CommandParam::ParamType kind) {
		CommandParam arg = { typeName, (UInt32)kind, 0 };
		return arg;
	}

	CommandInfo MakeInfo(const char* longName, const char* helpText, UInt16 numParams, CommandParam* params, void* execute) {
		CommandInfo info = { longName, "", 0, helpText, 0, numParams, params, execute, NULL, NULL, 0 };
		return info;
	}

}

static CommandParam ConstantArgs[5] = {
	MakeArg("string", CommandParam::kParamType_String),
	MakeArg("float", CommandParam::kParamType_Float),
	MakeArg("float", CommandParam::kParamType_Float),
	MakeArg("float", CommandParam::kParamType_Float),
	MakeArg("float", CommandParam::kParamType_Float),
};

static CommandParam EffectArgs[2] = {
	MakeArg("string", CommandParam::kParamType_String),
	MakeArg("int", CommandParam::kParamType_Integer),
};

static CommandParam ReadSettingArgs[2] = {
	MakeArg("string", CommandParam::kParamType_String),
	MakeArg("string", CommandParam::kParamType_String),
};

static CommandParam WriteSettingArgs[3] = {
	MakeArg("string", CommandParam::kParamType_String),
	MakeArg("string", CommandParam::kParamType_String),
	MakeArg("float", CommandParam::kParamType_Float),
};

static CommandParam WriteSettingStringArgs[3] = {
	MakeArg("string", CommandParam::kParamType_String),
	MakeArg("string", CommandParam::kParamType_String),
	MakeArg("string", CommandParam::kParamType_String),
};

static CommandParam RefOffsetArgs[4] = {
	MakeArg("objectRef", CommandParam::kParamType_ObjectRef),
	MakeArg("float", CommandParam::kParamType_Float),
	MakeArg("float", CommandParam::kParamType_Float),
	MakeArg("float", CommandParam::kParamType_Float),
};

static CommandParam PositionArgs[3] = {
	MakeArg("float", CommandParam::kParamType_Float),
	MakeArg("float", CommandParam::kParamType_Float),
	MakeArg("float", CommandParam::kParamType_Float),
};

static bool RunIsThirdPerson(CommandArgs args) {
	TheCommandManager->Commands.IsThirdPerson(args.result);
	return true;
}

static bool RunGetVersion(CommandArgs args) {
	TheCommandManager->Commands.GetVersion(args.result);
	return true;
}

static bool RunGetLocationName(CommandArgs args) {
	TheCommandManager->Commands.GetLocationName(args.result);
	return true;
}

static bool RunSetExtraEffectEnabled(CommandArgs args) {
	char effect[80];
	int flag;
	if (Pointers::Functions::ExtractArgs(args.paramInfo, args.arg1, args.opcodeOffset, args.thisObj, args.Obj, args.scriptObj, args.eventList, &effect, &flag))
		TheCommandManager->Commands.SetExtraEffectEnabled(args.result, effect, flag);
	return true;
}

static bool RunSetCustomConstant(CommandArgs args) {
	char name[80];
	D3DXVECTOR4 vec;
	if (Pointers::Functions::ExtractArgs(args.paramInfo, args.arg1, args.opcodeOffset, args.thisObj, args.Obj, args.scriptObj, args.eventList, &name, &vec.x, &vec.y, &vec.z, &vec.w))
		TheCommandManager->Commands.SetCustomConstant(args.result, name, &vec);
	return true;
}

static bool RunGetWeatherName(CommandArgs args) {
	TheCommandManager->Commands.GetWeatherName(args.result);
	return true;
}

static bool RunGetSetting(CommandArgs args) {
	char section[40];
	char key[40];
	if (Pointers::Functions::ExtractArgs(args.paramInfo, args.arg1, args.opcodeOffset, args.thisObj, args.Obj, args.scriptObj, args.eventList, &section, &key))
		TheCommandManager->Commands.GetSetting(args.result, section, key);
	return true;
}

static bool RunSetSetting(CommandArgs args) {
	char section[40];
	char key[40];
	float value;
	if (Pointers::Functions::ExtractArgs(args.paramInfo, args.arg1, args.opcodeOffset, args.thisObj, args.Obj, args.scriptObj, args.eventList, &section, &key, &value))
		TheCommandManager->Commands.SetSetting(args.result, section, key, value);
	return true;
}

static bool RunSetSettingS(CommandArgs args) {
	char section[40];
	char key[40];
	char text[80];
	if (Pointers::Functions::ExtractArgs(args.paramInfo, args.arg1, args.opcodeOffset, args.thisObj, args.Obj, args.scriptObj, args.eventList, &section, &key, &text))
		TheCommandManager->Commands.SetSettingS(args.result, section, key, text);
	return true;
}

static bool RunCameraTranslate(CommandArgs args) {
	TESObjectREFR* target;
	NiPoint3 offset;
	if (Pointers::Functions::ExtractArgs(args.paramInfo, args.arg1, args.opcodeOffset, args.thisObj, args.Obj, args.scriptObj, args.eventList, &target, &offset.x, &offset.y, &offset.z))
		TheCommandManager->Commands.CameraTranslate(args.result, target, &offset);
	return true;
}

static bool RunCameraRotate(CommandArgs args) {
	TESObjectREFR* target;
	NiPoint3 angles;
	// Script argument order is (yaw, pitch, roll); the vector stores them as (z, x, y).
	if (Pointers::Functions::ExtractArgs(args.paramInfo, args.arg1, args.opcodeOffset, args.thisObj, args.Obj, args.scriptObj, args.eventList, &target, &angles.z, &angles.x, &angles.y))
		TheCommandManager->Commands.CameraRotate(args.result, target, &angles);
	return true;
}

static bool RunCameraTranslateToPosition(CommandArgs args) {
	NiPoint3 point;
	if (Pointers::Functions::ExtractArgs(args.paramInfo, args.arg1, args.opcodeOffset, args.thisObj, args.Obj, args.scriptObj, args.eventList, &point.x, &point.y, &point.z))
		TheCommandManager->Commands.CameraTranslateToPosition(args.result, &point);
	return true;
}

static bool RunCameraRotateToPosition(CommandArgs args) {
	NiPoint3 angles;
	if (Pointers::Functions::ExtractArgs(args.paramInfo, args.arg1, args.opcodeOffset, args.thisObj, args.Obj, args.scriptObj, args.eventList, &angles.z, &angles.x, &angles.y))
		TheCommandManager->Commands.CameraRotateToPosition(args.result, &angles);
	return true;
}

static bool RunCameraLookAt(CommandArgs args) {
	TESObjectREFR* target;
	NiPoint3 offset;
	if (Pointers::Functions::ExtractArgs(args.paramInfo, args.arg1, args.opcodeOffset, args.thisObj, args.Obj, args.scriptObj, args.eventList, &target, &offset.x, &offset.y, &offset.z))
		TheCommandManager->Commands.CameraLookAt(args.result, target, &offset);
	return true;
}

static bool RunCameraLookAtPosition(CommandArgs args) {
	NiPoint3 point;
	if (Pointers::Functions::ExtractArgs(args.paramInfo, args.arg1, args.opcodeOffset, args.thisObj, args.Obj, args.scriptObj, args.eventList, &point.x, &point.y, &point.z))
		TheCommandManager->Commands.CameraLookAtPosition(args.result, &point);
	return true;
}

static bool RunCameraReset(CommandArgs args) {
	TheCommandManager->Commands.CameraReset(args.result);
	return true;
}

static CommandInfo InfoGetVersion = MakeInfo(CommandPrefix"GetVersion", "Returns the plugin version (console command only)", 0, NULL, RunGetVersion);
static CommandInfo InfoGetLocationName = MakeInfo(CommandPrefix"GetLocationName", "Gets the name of the current worldspace and current cell(console command only)", 0, NULL, RunGetLocationName);
static CommandInfo InfoSetExtraEffectEnabled = MakeInfo(CommandPrefix"SetExtraEffectEnabled", "Enables or disables a custom effect", 2, EffectArgs, RunSetExtraEffectEnabled);
static CommandInfo InfoSetCustomConstant = MakeInfo(CommandPrefix"SetCustomConstant", "Sets the value of a custom shader constant", 5, ConstantArgs, RunSetCustomConstant);
static CommandInfo InfoGetWeatherName = MakeInfo(CommandPrefix"GetWeatherName", "Returns the plugin version (console command only)", 0, NULL, RunGetWeatherName);
static CommandInfo InfoGetSetting = MakeInfo(CommandPrefix"GetSetting", "Gets a configuration setting", 2, ReadSettingArgs, RunGetSetting);
static CommandInfo InfoSetSetting = MakeInfo(CommandPrefix"SetSetting", "Sets a configuration setting", 3, WriteSettingArgs, RunSetSetting);
static CommandInfo InfoSetSettingS = MakeInfo(CommandPrefix"SetSettingS", "Sets a string configuration setting and applies it immediately", 3, WriteSettingStringArgs, RunSetSettingS);
static CommandInfo InfoCameraTranslate = MakeInfo(CommandPrefix"CameraTranslate", "Translates the camera position in relation of the ref", 4, RefOffsetArgs, RunCameraTranslate);
static CommandInfo InfoCameraRotate = MakeInfo(CommandPrefix"CameraRotate", "Rotates the camera in relation of the ref (yaw, pitch, roll)", 4, RefOffsetArgs, RunCameraRotate);
static CommandInfo InfoCameraTranslateToPosition = MakeInfo(CommandPrefix"CameraTranslateToPosition", "Translates the camera position in the world", 3, PositionArgs, RunCameraTranslateToPosition);
static CommandInfo InfoCameraRotateToPosition = MakeInfo(CommandPrefix"CameraRotateToPosition", "Rotates the camera in the world (yaw, pitch, roll)", 3, PositionArgs, RunCameraRotateToPosition);
static CommandInfo InfoCameraLookAt = MakeInfo(CommandPrefix"CameraLookAt", "Looks at the ref with an offset", 4, RefOffsetArgs, RunCameraLookAt);
static CommandInfo InfoCameraLookAtPosition = MakeInfo(CommandPrefix"CameraLookAtPosition", "Looks at the position in the world", 3, PositionArgs, RunCameraLookAtPosition);
static CommandInfo InfoCameraReset = MakeInfo(CommandPrefix"CameraReset", "Resets the camera to the normal state", 0, NULL, RunCameraReset);

// Papyrus entry points. Not registered with the game today; kept so the table below stays complete.
static bool Papyrus_SetExtraEffectEnabled(BSFixedString Name, bool Value) {

	double out;
	TheCommandManager->Commands.SetExtraEffectEnabled(&out, Name.m_data, Value);
	return out;

}

static bool Papyrus_SetCustomConstant(BSFixedString Name, float Value1, float Value2, float Value3, float Value4) {

	double out;
	D3DXVECTOR4 vec = { Value1, Value2, Value3, Value4 };
	TheCommandManager->Commands.SetCustomConstant(&out, Name.m_data, &vec);
	return out;

}

void CommandManager::Initialize(const PluginInterface* iface) {

	Logger::Log("Initializing the command manager...");
	TheCommandManager = new CommandManager();

	void* executers[] = { RunIsThirdPerson };
	// Registration order defines the opcode numbers (base 0x3000). Do not reorder, remove or insert: other mods' compiled scripts
	// store these numbers, not the names (Cartographer calls NVRSetSetting 0x3006, NVRSetSettingS 0x3007, NVRGetSetting 0x3005, as
	// in the original New Vegas Reloaded). New commands go at the end only. The static_assert below holds the count.
	CommandInfo* infos[] = {
		&InfoGetVersion,
		&InfoGetLocationName,
		&InfoSetExtraEffectEnabled,
		&InfoSetCustomConstant,
		&InfoGetWeatherName,
		&InfoGetSetting,
		&InfoSetSetting,
		&InfoSetSettingS,
		&InfoCameraTranslate,
		&InfoCameraRotate,
		&InfoCameraTranslateToPosition,
		&InfoCameraRotateToPosition,
		&InfoCameraLookAt,
		&InfoCameraLookAtPosition,
		&InfoCameraReset,
	};
	static_assert(sizeof(infos) / sizeof(infos[0]) == 15, "the original mod's 15 commands, in its order (opcodes 0x3000-0x300E): append new ones and raise this");
	void* papyrus[] = { Papyrus_SetExtraEffectEnabled, Papyrus_SetCustomConstant };
	TheCommandManager->RegisterCommands(iface, executers, infos, sizeof(infos) / sizeof(infos[0]), papyrus);

}

void CommandManager::PluginCommands::IsThirdPerson(double* out) {

	*out = TheCameraManager->IsFirstPerson() ? 0.0 : 1.0;

}

void CommandManager::PluginCommands::GetVersion(double* out) {

	if (InterfaceManager->IsActive(Menu::MenuType::kMenuType_Console)) Pointers::Functions::PrintToConsole("%s %s", Branding::ProductName, Branding::Version);	// ours, not the game's (the log keeps both)
	*out = 1;

}

void CommandManager::PluginCommands::GetLocationName(double* out) {

	if (InterfaceManager->IsActive(Menu::MenuType::kMenuType_Console)) {
		// Same lookups and the same order as before: cell, worldspace, region.
		const char* cell = Player->parentCell->GetEditorName();
		const char* world = NULL;
		const char* region = NULL;
		if (TESWorldSpace* ws = Player->GetWorldSpace()) world = ws->GetEditorName();
		if (TESRegion* rg = Player->GetRegion()) region = rg->GetEditorName();

		std::string line = "Worldspace: ";
		line += world ? world : "(no worldspace)";
		line += ", Region: ";
		line += region ? region : "(no region)";
		line += ", Cell: ";
		line += cell;
		Pointers::Functions::PrintToConsole(line.c_str());
	}
	*out = 1;

}

void CommandManager::PluginCommands::SetExtraEffectEnabled(double* out, const char* effectName, bool value) {

	*out = 0;

}

void CommandManager::PluginCommands::SetCustomConstant(double* out, const char* effectName, D3DXVECTOR4* value) {

	TheShaderManager->SetCustomConstant(effectName, *value);
	*out = 1;

}

void CommandManager::PluginCommands::GetWeatherName(double* out) {

	if (InterfaceManager->IsActive(Menu::MenuType::kMenuType_Console)) {
		const char* label = NULL;
		TESWeatherEx* weather = (TESWeatherEx*)TheShaderManager->ShaderConst.pWeather;
		if (weather) label = weather->GetEditorName();

		std::string line = "Weather: ";
		line += label ? label : "(no weather)";
		Pointers::Functions::PrintToConsole(line.c_str());
	}
	*out = 1;

}

void CommandManager::PluginCommands::GetSetting(double* out, const char* section, const char* key) {

	*out = TheSettingManager->GetSettingF(section, key);

}

void CommandManager::PluginCommands::SetSetting(double* out, const char* section, const char* key, float value) {

	if (StalePresetValues::Blocked(section, key, value, "a script (NVRSetSetting)")) { *out = 1; return; }	// UNOFFICIAL 2026-11-10
	TheSettingManager->SetSettingF(section, key, value);
	*out = 1;

}

void CommandManager::PluginCommands::SetSettingS(double* out, const char* section, const char* key, const char* value) {

	TheSettingManager->SetSettingS(section, key, value);

	// The value is persisted above. For the three LUT slots, also load the texture and
	// assign it, but do not write the setting back a second time.
	if (strcmp(section, "Shaders.LUT.Main") == 0 && TheShaderManager->Effects.LUT) {
		int slot;
		if (strcmp(key, "DayLUT") == 0) slot = 0;
		else if (strcmp(key, "NightLUT") == 0) slot = 1;
		else if (strcmp(key, "InteriorLUT") == 0) slot = 2;
		else slot = -1;
		if (slot >= 0) {
			LUTEffect* lut = TheShaderManager->Effects.LUT;
			std::string texturePath = std::string(LUTEffect::LUTFolder()) + value;
			IDirect3DBaseTexture9* texture = TheTextureManager->GetFileTexture(texturePath, TextureRecord::PlanarBuffer);
			if (texture) lut->AssignLUTSlot(slot, texture, value);
		}
	}

	*out = 1;

}

void CommandManager::PluginCommands::CameraTranslate(double* out, TESObjectREFR* target, NiPoint3* value) {

	CameraManager::CameraManagerCommands* cmd = TheCameraManager->CameraCommands;

	cmd->IsTranslating = true;
	cmd->Ref = target;
	// Script (x, y, z) maps to the camera's (z, y, x).
	cmd->Position.x = value->z;
	cmd->Position.y = value->y;
	cmd->Position.z = value->x;
	*out = 1;

}

void CommandManager::PluginCommands::CameraRotate(double* out, TESObjectREFR* target, NiPoint3* value) {

	CameraManager::CameraManagerCommands* cmd = TheCameraManager->CameraCommands;

	cmd->IsRotating = true;
	cmd->Ref = target;
	cmd->Rotation.x = value->z;
	cmd->Rotation.y = value->x;
	cmd->Rotation.z = value->y;
	*out = 1;

}

void CommandManager::PluginCommands::CameraTranslateToPosition(double* out, NiPoint3* value) {

	CameraManager::CameraManagerCommands* cmd = TheCameraManager->CameraCommands;

	cmd->IsTranslatingToPosition = true;
	cmd->Position.x = value->x;
	cmd->Position.y = value->y;
	cmd->Position.z = value->z;
	*out = 1;

}

void CommandManager::PluginCommands::CameraRotateToPosition(double* out, NiPoint3* value) {

	CameraManager::CameraManagerCommands* cmd = TheCameraManager->CameraCommands;

	cmd->IsRotatingToPosition = true;
	cmd->Rotation.x = value->z;
	cmd->Rotation.y = value->x;
	cmd->Rotation.z = value->y;
	*out = 1;

}

void CommandManager::PluginCommands::CameraLookAt(double* out, TESObjectREFR* target, NiPoint3* value) {

	CameraManager::CameraManagerCommands* cmd = TheCameraManager->CameraCommands;

	cmd->IsLookingAt = true;
	cmd->Ref = target;
	cmd->LookingAtOffset.x = value->z;
	cmd->LookingAtOffset.y = value->y;
	cmd->LookingAtOffset.z = value->x;
	*out = 1;

}

void CommandManager::PluginCommands::CameraLookAtPosition(double* out, NiPoint3* value) {

	CameraManager::CameraManagerCommands* cmd = TheCameraManager->CameraCommands;

	cmd->IsLookingAtPosition = true;
	cmd->LookingAtPosition.x = value->x;
	cmd->LookingAtPosition.y = value->y;
	cmd->LookingAtPosition.z = value->z;
	*out = 1;

}

void CommandManager::PluginCommands::CameraReset(double* out) {

	CameraManager::CameraManagerCommands* cmd = TheCameraManager->CameraCommands;

	cmd->IsTranslating = false;
	cmd->IsRotating = false;
	cmd->IsTranslatingToPosition = false;
	cmd->IsRotatingToPosition = false;
	cmd->IsLookingAt = false;
	cmd->IsLookingAtPosition = false;
	*out = 1;

}
