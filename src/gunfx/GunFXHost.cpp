// The Vegas Shaders side of the built-in GunFX (2026-11-16, gunfx-builtin; see GunFXHost.h).
#include "GunFXHost.h"
#include "GunFX.h"
#include "GunFXKeys.h"
#include "../core/Paths.h"
#include <tlhelp32.h>

namespace GunFXHost {

static UInt32 SwitchRevision = 0xFFFFFFFFu;

void SyncSwitches() {
	// Bits: 0 puff, 1 heat smoke, 2 ejection smoke, 3 barrel glow, 4 heat haze, 5 muzzle blast, 6 energy weapons too,
	// 7 after-fire trail, 8 volumetric smoke (the order GunFX.dll's GunFX_SetSwitches always had).
	static const char* const keys[] = { "Puff", "HeatSmoke", "EjectionSmoke", "BarrelGlow", "HeatHaze", "MuzzleBlast", "EnergyWeapons", "AfterFireTrail", "VolumetricSmoke" };
	SwitchRevision = TheSettingManager->SettingsRevision;
	UInt32 bits = 0;
	for (UInt32 i = 0; i < 9; ++i)
		if (TheSettingManager->GetSettingI("Main.GunFX.Main", keys[i])) bits |= 1u << i;
	GunFX::SetSwitches(bits);
}

// An old GunFX.dll a player left in Data\NVSE\Plugins (a zip install cannot delete it) still loads: xNVSE loads every plugin there. It
// would draw its own smoke on top of ours (its switches start all on and only Vegas Shaders ever set them). Switched off for good
// with one call, GunFX_SetSwitches(0): the copy keeps that value, and nothing else is ever read from it. Every GunFX.dll since P67
// has that export; a copy without it (older) is named in the log. Found by its export, so a renamed copy counts too.
// At the first frame, not at the plugin's load: the order xNVSE loads plugins in is not guaranteed.
static void HoldOffOldCopies() {
	HMODULE self = nullptr;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&HoldOffOldCopies, &self);
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
	if (snap == INVALID_HANDLE_VALUE) { Logger::Log("GunFX: could not look for an old GunFX.dll (module list unavailable)"); return; }
	MODULEENTRY32W entry = {};
	entry.dwSize = sizeof(entry);
	int found = 0;
	for (BOOL more = Module32FirstW(snap, &entry); more; more = Module32NextW(snap, &entry)) {
		if (entry.hModule == self) continue;
		typedef void (__cdecl* SetFn)(UInt32);
		const SetFn set = (SetFn)GetProcAddress(entry.hModule, "GunFX_SetSwitches");
		if (set) {
			set(0);
			found++;
			Logger::Log("GunFX is built into Vegas Shaders now; the old %ls is switched off and can be deleted with GunFX.ini / GunFX.ini.defaults / GunFX.pdb (%ls).",
				entry.szModule, entry.szExePath);
		}
		else if (!_wcsnicmp(entry.szModule, L"GunFX", 5) && GetProcAddress(entry.hModule, "NVSEPlugin_Load")) {
			found++;
			Logger::Log("[ERROR] GunFX is built into Vegas Shaders now, but an old %ls (%ls) is too old to be switched off: it may draw its own smoke on top. Delete it with GunFX.ini / GunFX.ini.defaults / GunFX.pdb.",
				entry.szModule, entry.szExePath);
		}
	}
	CloseHandle(snap);
	if (!found) Logger::Log("GunFX: built in; no old GunFX.dll loaded");
}

void FrameStart() {
	static bool heldOff = false;
	if (!heldOff) { heldOff = true; HoldOffOldCopies(); }
	if (SwitchRevision != TheSettingManager->SettingsRevision) SyncSwitches();
}

bool GetValue(const char* iniSection, const char* iniKey, char* out, unsigned size) {
	if (!out || !size) return false;
	out[0] = 0;
	char section[64];
	GunFXKeys::TomlSection(iniSection, section, sizeof(section));
	const char* key = GunFXKeys::TomlKey(iniSection, iniKey);
	char text[256];	// GetSettingS copies at most a node's Value (80 bytes)
	TheSettingManager->GetSettingS(section, key, text);
	if (!strcmp(text, "\"")) return false;	// (a setting that does not exist reads as a lone quote)
	switch (iniKey[0]) {
	case 'b': case 'i': sprintf_s(out, size, "%d", TheSettingManager->GetSettingI(section, key)); break;
	case 'f': sprintf_s(out, size, "%.9g", TheSettingManager->GetSettingF(section, key)); break;
	default: strncpy_s(out, size, text, _TRUNCATE); break;
	}
	return true;
}

unsigned Revision() { return TheSettingManager->SettingsRevision; }

void Log(const char* line) { Logger::Log("GunFX: %s", line); }

// ---- The one-time import of an old GunFX.ini ------------------------------------------------------------------------------------
static bool SameValue(char type, const char* a, const char* b) {
	if (type == 's') {
		for (; *a && *b; a++, b++) {
			const char x = *a == '/' ? '\\' : (char)tolower((unsigned char)*a), y = *b == '/' ? '\\' : (char)tolower((unsigned char)*b);
			if (x != y) return false;
		}
		return *a == *b;
	}
	const double x = atof(a), y = atof(b);
	return fabs(x - y) <= 1e-6 * (fabs(x) > 1.0 ? fabs(x) : 1.0);
}

void ImportIniOnce() {
	if (TheSettingManager->GetSettingI("Main.GunFX.General", "IniImported")) return;
	const std::string folder = Paths::ModuleDir();
	const std::string ini = folder + "GunFX.ini", defaults = folder + "GunFX.ini.defaults";
	if (GetFileAttributesA(ini.c_str()) == INVALID_FILE_ATTRIBUTES) return;	// (no old INI: nothing to do; looked for again next start)
	const bool haveDefaults = GetFileAttributesA(defaults.c_str()) != INVALID_FILE_ATTRIBUTES;
	Logger::Log("GunFX: reading the old %s into the settings once (the values that differ from %s)", ini.c_str(),
		haveDefaults ? "its GunFX.ini.defaults" : "the defaults GunFX shipped with");
	int imported = 0;
	for (const GunFXKeys::Key& k : GunFXKeys::kKeys) {
		char value[512], reference[512];
		GetPrivateProfileStringA(k.Section, k.Ini, "\x01", value, sizeof(value), ini.c_str());
		if (value[0] == 1) continue;	// not in the player's INI
		strcpy_s(reference, k.Default);
		if (haveDefaults) {
			GetPrivateProfileStringA(k.Section, k.Ini, "\x01", reference, sizeof(reference), defaults.c_str());
			if (reference[0] == 1) strcpy_s(reference, k.Default);
		}
		if (SameValue(k.Ini[0], value, reference)) continue;
		if (const char* sw = GunFXKeys::SwitchOf(k.Section, k.Ini)) {
			if (atoi(value) != 0) continue;	// switched on in the INI: the menu switch decides, as before
			TheSettingManager->SetSetting("Main.GunFX.Main", sw, false);
			Logger::Log("GunFX: imported [%s] %s = %s: the menu switch Main.GunFX.Main.%s is off", k.Section, k.Ini, value, sw);
			imported++;
			continue;
		}
		char section[64];
		GunFXKeys::TomlSection(k.Section, section, sizeof(section));
		const char* key = GunFXKeys::TomlKey(k.Section, k.Ini);
		if (k.Ini[0] == 's') for (char* c = value; *c; c++) if (*c == '\\') *c = '/';	// (paths are kept with forward slashes)
		TheSettingManager->SetSettingS(section, key, value);
		Logger::Log("GunFX: imported [%s] %s = %s (default %s) -> %s.%s", k.Section, k.Ini, value, reference, section, key);
		imported++;
	}
	// Marked and saved only when something was imported. An INI equal to its defaults (most players) leaves the settings untouched,
	// with nothing unsaved in the menu, and is compared again at the next start (223 values, well under a millisecond).
	if (imported) {
		TheSettingManager->SetSetting("Main.GunFX.General", "IniImported", true);
		TheSettingManager->SaveSettings();
	}
	Logger::Log("GunFX: %d value(s) imported from GunFX.ini; it is no longer read and can be deleted with GunFX.ini.defaults (and an old GunFX.dll / GunFX.pdb)", imported);
}

}
