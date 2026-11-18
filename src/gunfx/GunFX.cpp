// GunFX: an xNVSE plugin for Fallout: New Vegas. Smoke from the gun barrel, with the game's own
// temporary-effect spawner (the one impact effects use):
//   - a small puff at the muzzle on each shot (optional);
//   - after you stop firing, a thin wisp rising from the barrel like a lit cigarette, longer the more you fired.
//   - a soft orange glow at the muzzle while the barrel is hot.
// Settings: until 2026-11-16 GunFX.ini next to the DLL; now Vegas Shaders' own settings, [Main.GunFX.<section>] (the menu's Gun
// effects page), read through GunFXHost (GunFXKeys.h lists every key). Per-gun files still override them (see UpdateGunSettings).
// 2026-11-16 (gunfx-builtin): built into Vegas Shaders (VegasShaders.dll) instead of a plugin of its own. Its own translation
// unit: everything below lives in an anonymous namespace (its NiPoint3, UInt32, Look ... never meet Vegas Shaders' types), and
// Vegas Shaders calls the functions of GunFX.h (Load from its NVSEPlugin_Load, MainGameLoop from its xNVSE message handler).
//
// Game addresses (FalloutNV.exe 1.4.0.525):
//   TESObjectWEAP::Fire                0x00523150  thiscall (weapon, actor); called from 0x008BADE9 (attacks),
//                                      0x0087BA71 (queued fire task) and 0x005DA5F8 (FireWeapon script command)
//   Actor::GetProjectileNode           0x008BE0A0  thiscall (actor): the gun's ProjectileNode, where its bullets start
//   BSTempEffectParticle::Spawn        0x006890B0  cdecl (cell, lifetime, model, up direction, position, scale, flags, parent)
//   PlayerCharacter singleton pointer  0x011DEA3C
#include <windows.h>
#include <cstdio>
#include <share.h>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <cctype>
#include <string>
#include <vector>
#include <cstdint>

extern "C" IMAGE_DOS_HEADER __ImageBase;
#include "GunFX.h"
#include "GunFXHost.h"
#include "GunFXKeys.h"

namespace {   // closed before the GunFX:: functions at the end
#include "IniDefaults.h"
#include "GunShape.h"

typedef unsigned int UInt32;
typedef unsigned char UInt8;


struct NiPoint3 { float x, y, z; };

struct MuzzleFlash {
	bool bEnabled, bMPSEnabled, bUpdateLight, gap03;
	float fEnableTimer, fDurationTimer;
	UInt8* node;          // NiNode at the muzzle (NiRefObject: ref count at +4); world translation at +0x8C
	void* light;
	void* projectile;
	void* sourceWeap;
	UInt8* sourceActor;   // TESObjectREFR; parent cell at +0x40
};

// Look of one volumetric smoke chain (read from [Wisp] / [Trail], the same keys as the sprite smoke).
struct VolLook { float size, sizeVar, life, lifeVar, speed, speedVar, startSize, grow, opacity, fadeIn, fadeStart, fadeEnd, rise, curl, spread; };
// The volumetric smoke's chains: heat smoke, after-fire trail, muzzle puff, ejection smoke.
enum { ChainHeat, ChainTrail, ChainPuff, ChainEject, ChainCount };
static struct {
	// Puff on each shot.
	int puff;
	char puffModel[MAX_PATH];
	float puffLifetime, puffScale, puffInterval;
	int puffPlayerOnly;
	// Wisp driven by barrel heat (your own gun).
	int trail;
	char trailModel[MAX_PATH];
	float trailScale, heatPerShot, coolPerSecond, maxHeat, heatStart, heatFull, maxRate, fileRate, effectSeconds, tailSeconds;
	int trailFlags, trailSmokeOnly, trailFollow;
	float constantRate, sway, swaySpeed, coolDelay, wispStopDelay;
	int wispEmitters, afterEmitters;   // copies of the heat smoke / trail spread along the barrel's path (1-4)
	// Volumetric smoke (prototype, [Volume]).
	int volume;
	float volumeSize, volumeDensity, volumeRise, volumeCurl, volumeDrag, volumeSpacing, volumeNoise, volumeBrightness, volumeShade;
	int volumeDebug;
	int volumeKinds[ChainCount];          // which smoke is drawn volumetric ([Volume] bHeatSmoke, bTrail, bPuff, bEjection)
	float volumeBurstDrag, volumePuffForward, volumeBurstSize;
	int volumeBurstPoints;
	int volumeGunInFront;                 // the first-person gun is always drawn in front of the smoke
	float volumeExpand, volumeThinning;   // how big the smoke ends up; how much it thins out as it widens
	float volumeFlash, volumeFlashRadius, volumeFlashSeconds;   // the muzzle flash's light on the smoke
	float volumeWind, volumeWindPickup, volumeWindOffset;        // drift with the weather's wind
	int volumePlayer;                                            // your body pushes and thins the smoke
	float volumePlayerRadius, volumePlayerPush, volumeStir, volumePlayerHeight;
	float volumeDiffuse;                                         // stirred smoke spreads out, thins and frays
	float volumeBurst;                                           // smoke you run or swing through is thrown outward in a puff
	float volumeBullet, volumeBulletReach;                       // each bullet punches a tunnel through the smoke: how hard, how far from its path
	float volumeBulletOpen, volumeBulletSeconds;                 // ... how long its hole takes to open, and how long until the smoke has filled it again
	float volumePillows, volumeStreaks;                          // texture: billowy puffs and ejection smoke, fibrous heat smoke and trail
	float volumeHaze, volumeHazeLife, volumeHazeSize;            // gun-smoke haze: how much per shot, how long it lingers, how big
	float volumeTint;                                            // colour by thickness and age
	float volumeSmooth;                                          // how quickly kinks in a strand even out
	float volumeDepth;                                           // shading for a sense of depth (0 = flat)
	float volumeDriftRight, volumeDriftAway;                     // heat smoke / trail drift (units per second)
	int volumeWrapGun;                                           // smoke flows around the gun
	int volumeGunShape;                                          // ... around its real shape (from its model), not a tube
	float volumeGunLength, volumeGunRadius, volumeBarrelSpread, volumeGunSwirl, volumeBodySwirl, volumeSwirlLife;
	float volumeTendrils, volumeTendrilAngle, volumeTendrilSize, volumeTendrilSpeed;   // child strands
	int volumeUnzip;                                             // the heat smoke and trail split around you as you walk in
	float volumeFanSpeed;
	VolLook volLook[ChainCount];
	// After-fire trail ([Trail]): a thin wisp that rises only once you stop firing.
	int after;
	char afterModel[MAX_PATH];
	float afterScale, afterDelay, afterMinHeat, afterFullHeat, afterMaxRate, afterSway, afterSwaySpeed;
	int glow, glowPreview;
	float glowScale, glowLength, glowStart, glowHeatSpan, glowRiseSeconds;
	float glowRadius, glowForwardLength, glowMinLength, glowSpreadShots, glowSpreadCoolSeconds;
	float glowOffsetX, glowOffsetY, glowOffsetZ;
	char puffNode[128], wispNode[128], ejectNode[128], ejectModel[MAX_PATH];
	NiPoint3 puffOffset, wispOffset, ejectOffset;
	int eject, haze, hazePreview, hazeMask, logWeaponNodes, blast;
	float blastPixels, blastRadius, blastSeconds;
	float ejectScale, ejectLifetime, ejectInterval, ejectHotScale, ejectBurst, puffBurst, ejectBurstRate, puffBurstRate;
	float hazeStart, hazeFull, hazeRise, hazePixels, hazeHeight, hazeWidth, hazeSpeed, hazeLength, hazeMinSize;
	int flags, logEvents;
	int debugLog;                         // [Main.GunFX.General] DebugLog: the chatty lines too
} Settings;

static FILE* Log = nullptr;   // GUNFX_REPLAY only (the replay rig's own log file); the game build logs into the Vegas Shaders log
static void LogText(const char* line) {
#ifndef GUNFX_REPLAY
	GunFXHost::Log(line);
#else
	if (!Log) return;
	// The clock time on every line, so it can be lined up with NVR's log and with what you did in the game.
	SYSTEMTIME t;
	GetLocalTime(&t);
	fprintf(Log, "%02d:%02d:%02d.%03d  %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, line);
	fflush(Log);
#endif
}
static void LogLine(const char* format, ...) {
	char line[4096];
	va_list args; va_start(args, format); _vsnprintf_s(line, sizeof(line), _TRUNCATE, format, args); va_end(args);
	LogText(line);
}
// 2026-11-16: the chatty lines (per shot, per burst, per gun, timing) only with [Main.GunFX.General] DebugLog on.
static void DebugLine(const char* format, ...) {
	if (!Settings.debugLog) return;
	char line[4096];
	va_list args; va_start(args, format); _vsnprintf_s(line, sizeof(line), _TRUNCATE, format, args); va_end(args);
	LogText(line);
}

// Seconds on the high-resolution counter. GetTickCount64 moves in 15.6 ms steps, too coarse for anything that changes
// from one frame to the next: at 75-80 frames a second, one frame in six saw no time pass and the others 15.6 ms.
static double PreciseSeconds() {
	static LARGE_INTEGER frequency = {};
	if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
	LARGE_INTEGER counter;
	QueryPerformanceCounter(&counter);
	return (double)counter.QuadPart / (double)frequency.QuadPart;
}
static bool LogEvent() { if (Settings.logEvents <= 0) return false; Settings.logEvents--; return true; }

// Master switches from NVR's menu (Main > GunFX), set every frame through GunFX_SetSwitches: bit 0 puff, 1 heat
// smoke strand, 2 ejection smoke, 3 barrel glow, 4 heat haze, 5 muzzle blast, 6 energy weapons too, 7 after-fire trail,
// 8 volumetric heat smoke / trail. All on until NVR says otherwise, so
// GunFX also works on its own (then only the INI decides).
static volatile LONG NvrSwitches = -1;
static bool Switch(int bit) { return (NvrSwitches >> bit) & 1; }
// Energy weapons (lasers, plasma, ...) get no GunFX unless the menu switch EnergyWeapons (bit 6) is on. Decided from
// the weapon's own data, so modded weapons count too: it uses the Energy Weapons skill (TESObjectWEAP actor value at
// +0x15C, 34) or its type is energy pistol/rifle (+0xF4: 4, 7).
static bool IsEnergyWeapon(void* weapon) {
	if (!weapon) return false;
	const UInt8 type = *((UInt8*)weapon + 0xF4);
	return *(UInt32*)((UInt8*)weapon + 0x15C) == 34 || type == 4 || type == 7;
}
static bool GunAllowed(void* weapon) { return !IsEnergyWeapon(weapon) || Switch(6); }

typedef void* (__cdecl* SpawnFn)(void* cell, float lifetime, const char* model, NiPoint3 up, NiPoint3 position,
	float scale, UInt32 flags, void* parent);
static const SpawnFn SpawnEffect = (SpawnFn)0x006890B0;
#ifndef GUNFX_REPLAY
static void* Player() { return *(void**)0x011DEA3C; }
#else
// The replay rig (work\gunfx_replay.cpp) compiles this file with GUNFX_REPLAY and plays the game's part itself: there is
// no game, so no player object; your body and the room come from ReplayBody instead (UpdateAirAndPlayer).
static void* Player() { return nullptr; }
struct ReplayBodyState { bool on, indoors; NiPoint3 feet; };
static ReplayBodyState ReplayBody = {};
#endif

// Keeps a game node alive while it is remembered between frames (released with the game's own DeleteThis at zero).
static void NodeAddRef(UInt8* node) { if (node) InterlockedIncrement((volatile LONG*)(node + 4)); }
static void NodeRelease(UInt8* node) {
	if (node && InterlockedDecrement((volatile LONG*)(node + 4)) == 0) {
		void** vtable = *(void***)node;
		((void(__thiscall*)(void*))vtable[1])(node);
	}
}

// Hides the solid parts of a spawned effect (meshes such as the ashtray under the cigarette smoke), leaving its
// particle systems. Uses the game's own type checks: NiObject vtable 8 IsTriStrips, 9 IsTriShape, 12 IsParticlesGeom,
// 3 IsNiNode; NiNode children at +0x9C (NiTArray: data +4, end +0xA); NiAVObject flags at +0x30 (bit 0 = culled).
static int HideSolidMeshes(UInt8* object, int depth) {
	if (!object || depth > 8) return 0;
	void** vtable = *(void***)object;
	typedef void* (__thiscall* IsFn)(void*);
	if (((IsFn)vtable[12])(object)) return 0; // particles: keep
	if (((IsFn)vtable[8])(object) || ((IsFn)vtable[9])(object)) { *(UInt32*)(object + 0x30) |= 1; return 1; }
	int hidden = 0;
	if (((IsFn)vtable[3])(object)) {
		UInt8** children = *(UInt8***)(object + 0x9C + 4);
		const unsigned short count = *(unsigned short*)(object + 0x9C + 0xA);
		for (unsigned short i = 0; children && i < count; i++) hidden += HideSolidMeshes(children[i], depth + 1);
	}
	return hidden;
}

// Switches off the emitters of a spawned effect, so no new smoke appears; the smoke already in the air keeps rising and
// fading. NiObjectNET controller at +0x0C; NiTimeController flags at +0x08 (0x8 = active), next controller at +0x30;
// NiObject vtable 2 GetRTTI -> NiRTTI { const char* name }. Only a controller named NiPSysEmitterCtlr is touched.
static bool LoggedControllers = false;
static const char* RttiName(UInt8* object) {
	typedef const char** (__thiscall* RttiFn)(void*);
	const char** rtti = ((RttiFn)(*(void***)object)[2])(object);
	return rtti && *rtti ? *rtti : "?";
}
static int StopEmitters(UInt8* object, int depth) {
	if (!object || depth > 8) return 0;
	void** vtable = *(void***)object;
	typedef void* (__thiscall* IsFn)(void*);
	int stopped = 0;
	if (((IsFn)vtable[12])(object)) {
		char names[256] = "";
		for (UInt8* ctrl = *(UInt8**)(object + 0x0C); ctrl; ctrl = *(UInt8**)(ctrl + 0x30)) {
			const char* name = RttiName(ctrl);
			if (!LoggedControllers && strlen(names) + strlen(name) + 2 < sizeof(names)) { strcat_s(names, name); strcat_s(names, " "); }
			if (!strcmp(name, "NiPSysEmitterCtlr")) { *(unsigned short*)(ctrl + 0x08) &= ~0x8; stopped++; }
		}
		if (!LoggedControllers) { LoggedControllers = true; DebugLine("particle system controllers: %s", names); }
		return stopped;
	}
	if (((IsFn)vtable[3])(object)) {
		UInt8** children = *(UInt8***)(object + 0x9C + 4);
		const unsigned short count = *(unsigned short*)(object + 0x9C + 0xA);
		for (unsigned short i = 0; children && i < count; i++) stopped += StopEmitters(children[i], depth + 1);
	}
	return stopped;
}

// Borrowed nodes are used only synchronously while the visible model is alive.
static UInt8* FindNamedNode(UInt8* object, const char* wanted, int depth = 0) {
	if (!object || !wanted || !*wanted || depth > 24) return nullptr;
	const char* name = *(const char**)(object + 8);
	if (name && (!_stricmp(name, wanted) || (!strncmp(name, "##", 2) && !_stricmp(name + 2, wanted)))) return object;
	typedef void* (__thiscall* IsFn)(void*);
	if (!((IsFn)(*(void***)object)[3])(object)) return nullptr;
	UInt8** children = *(UInt8***)(object + 0xA0);
	const unsigned short count = *(unsigned short*)(object + 0xA6);
	for (unsigned short i = 0; children && i < count; ++i)
		if (UInt8* found = FindNamedNode(children[i], wanted, depth + 1)) return found;
	return nullptr;
}
static NiPoint3 NodePosition(UInt8* node, NiPoint3 offset) {
	NiPoint3 at = *(NiPoint3*)(node + 0x8C);
	const float* r = (const float*)(node + 0x68);
	const float scale = *(float*)(node + 0x98);
	at.x += scale * (r[0]*offset.x + r[1]*offset.y + r[2]*offset.z);
	at.y += scale * (r[3]*offset.x + r[4]*offset.y + r[5]*offset.z);
	at.z += scale * (r[6]*offset.x + r[7]*offset.y + r[8]*offset.z);
	return at;
}
static void LogNodeNames(UInt8* node, int& budget, int depth = 0) {
	if (!node || budget <= 0 || depth > 24) return;
	--budget;
	const char* name = *(const char**)(node + 8);
	if (name && *name) DebugLine("weapon node: %s", name);
	typedef void* (__thiscall* IsFn)(void*);
	if (!((IsFn)(*(void***)node)[3])(node)) return;
	UInt8** children = *(UInt8***)(node + 0xA0);
	const unsigned short count = *(unsigned short*)(node + 0xA6);
	for (unsigned short i = 0; children && i < count && budget > 0; ++i) LogNodeNames(children[i], budget, depth + 1);
}
static UInt8* VisibleRoot();
static UInt8* SmokeAnchor(const char* name, UInt8* fallback) {
	UInt8* named = FindNamedNode(VisibleRoot(), name);
	return named ? named : fallback;
}

// ---- Look tuning from the INI ------------------------------------------------------------------------------------
// The smoke's look lives in the effect file; after spawning, these values are rewritten in the effect's own copy.
// Nothing about the modifiers' memory layout is assumed: their list is found in the particle system by checking
// that every entry is a NiPSys/BSPSys object, and each value is located by searching the modifier for the exact
// numbers the file was built with (tools\make-effects.py). Only places that match are written; misses are logged.
struct Look {             // file values (what to look for) and INI values (what to write)
	float speed, radius, radiusVar, life, lifeVar;   // NiPSysEmitter: radius, radius variation, life, life variation
	float grow, fade;                                // NiPSysGrowFadeModifier
	float strength, turbulence, turbulenceScale;     // NiPSysGravityModifier (negative strength rises)
	float percents[6];                               // BSPSysSimpleColorModifier: fade in/out, colour 1 end, 2 start, 2 end, 3 start
	float opacity;                                   // middle colour's alpha
};
static const Look WispFile = { 12.0f, 5.0f, 1.5f, 2.5f, 0.6f, 2.2f, 0.0f, -20.0f, 0.03f, 5.0f, { 0.08f, 0.15f, 0.2f, 0.5f, 0.51f, 0.8f }, 0.35f };
static const Look PuffFile = { 5.0f, 3.0f, 1.0f, 1.3f, 0.3f, 1.2f, 0.0f, -6.0f, 0.035f, 5.0f, { 0.03f, 0.15f, 0.0f, 0.06f, 0.51f, 0.8f }, 0.35f };
// The strand: a dense stream of small faint puffs that blend into one thin strand (barrelstrand.nif, emits 40/s).
static const Look StrandFile = { 10.0f, 3.0f, 0.7f, 2.6f, 0.3f, 2.6f, 0.0f, -9.0f, 0.02f, 5.0f, { 0.05f, 0.15f, 0.02f, 0.12f, 0.45f, 0.92f }, 0.15f };
static const Look* WispBuilt = &WispFile;   // the numbers the chosen wisp model was built with (set from sModel)
static Look WispLook = WispFile, PuffLook = PuffFile;

static bool InImage(const void* p, UInt32 lo, UInt32 hi) { return (UInt32)p >= lo && (UInt32)p < hi; }
// Name of a game object's class, or null when the pointer does not look like one (vtable in .rdata, code in .text).
static const char* SafeRtti(UInt8* object) {
	if (!object || IsBadReadPtr(object, 8)) return nullptr;
	void** vtable = *(void***)object;
	if (!InImage(vtable, 0x00FDF000, 0x01183000) || !InImage(vtable[2], 0x00401000, 0x00FDF000)) return nullptr;
	return RttiName(object);
}
static bool IsModifier(UInt8* object) {
	const char* n = SafeRtti(object);
	return n && (!strncmp(n, "NiPSys", 6) || !strncmp(n, "BSPSys", 6)) && strstr(n, "Ctlr") == nullptr;
}
static bool Near(float a, float b) { return fabsf(a - b) < 0.0005f; }
static float* FindFloats(UInt8* object, const float* want, const int* gaps, int count) {
	// want[k] at byte offset gaps[k] from the match; searched in the first 0x80 bytes of the object.
	for (int at = 8; at <= 0x80; at += 4) {
		bool ok = true;
		for (int k = 0; k < count && ok; k++) ok = Near(*(float*)(object + at + gaps[k]), want[k]);
		if (ok) return (float*)(object + at);
	}
	return nullptr;
}

static int ApplyLookToSystem(UInt8* psys, const Look& file, const Look& look, char* report, size_t reportSize) {
	// The modifiers: a pointer field of the particle system that leads either to an array of modifiers or to a linked
	// list of them (NiTListItem: next +0, previous +4, item +8). Every entry must be a NiPSys/BSPSys object.
	UInt8* list[32]; int count = 0;
	const char* kind = "";
	for (int off = 0x20; off < 0x200 && !count; off += 4) {
		UInt8** data = *(UInt8***)(psys + off);
		if (!data || IsBadReadPtr(data, 16)) continue;
		int n = 0;
		while (n < 32 && !IsBadReadPtr(data + n, 4) && IsModifier(data[n])) { list[n] = data[n]; n++; }
		if (n >= 3) { count = n; kind = "array"; break; }
		n = 0;
		for (UInt8** node = data; node && n < 32 && !IsBadReadPtr(node, 12) && IsModifier((UInt8*)node[2]); node = (UInt8**)node[0]) list[n++] = (UInt8*)node[2];
		if (n >= 3) { count = n; kind = "list"; break; }
	}
	if (!count) { strcat_s(report, reportSize, "modifier list not found"); return 0; }
	char found[48]; sprintf_s(found, "(%d modifiers in a %s) ", count, kind); strcat_s(report, reportSize, found);
	int written = 0;
	// Copies of one effect share some modifier data with each other, so a later copy may already hold the values
	// written into an earlier one; the place found the first time (by the file's numbers) is remembered per type.
	static int emitterAt[2] = {}, growAt = 0, gravityAt = 0, colourAt = 0;
	auto locate = [](UInt8* m, int& learned, const float* want, const int* gaps, int n) -> float* {
		if (float* f = FindFloats(m, want, gaps, n)) { learned = (int)((UInt8*)f - m); return f; }
		return learned ? (float*)(m + learned) : nullptr;
	};
	for (int i = 0; i < count; i++) {
		UInt8* m = list[i]; const char* name = SafeRtti(m);
		if (!strcmp(name, "NiPSysCylinderEmitter") || !strcmp(name, "NiPSysSphereEmitter")) {
			const float want[] = { file.radius, file.radiusVar, file.life, file.lifeVar }; const int gaps[] = { 0, 4, 8, 12 };
			if (float* f = locate(m, emitterAt[name[6] == 'S'], want, gaps, 4)) { f[0] = look.radius; f[1] = look.radiusVar; f[2] = look.life; f[3] = look.lifeVar; written++; strcat_s(report, reportSize, "size+life "); }
		}
		else if (!strcmp(name, "NiPSysGrowFadeModifier")) {
			const float want[] = { file.grow, file.fade }; const int gaps[] = { 0, 8 };
			if (float* f = locate(m, growAt, want, gaps, 2)) { f[0] = look.grow; f[2] = look.fade; written++; strcat_s(report, reportSize, "grow "); }
		}
		else if (!strcmp(name, "NiPSysGravityModifier")) {
			const float want[] = { file.strength, file.turbulence, file.turbulenceScale }; const int gaps[] = { 0, 8, 12 };
			if (float* f = locate(m, gravityAt, want, gaps, 3)) { f[0] = look.strength; f[2] = look.turbulence; f[3] = look.turbulenceScale; written++; strcat_s(report, reportSize, "rise+curl "); }
		}
		else if (!strcmp(name, "BSPSysSimpleColorModifier")) {
			const int gaps[] = { 0, 4, 8, 12, 16, 20 };
			if (float* f = locate(m, colourAt, file.percents, gaps, 6)) {
				for (int k = 0; k < 6; k++) f[k] = look.percents[k];
				f[6 + 4 + 3] = look.opacity;   // colour 2 alpha (6 percents, then three RGBA colours)
				written++; strcat_s(report, reportSize, "fade+opacity ");
			}
		}
	}
	return written;
}

// Applies a look to every particle system of a spawned effect (root at effect + 0x18). Returns the parts written.
static int ApplyLook(UInt8* effect, const Look& file, const Look& look, char* report, size_t reportSize) {
	int written = 0;
	__try {
		UInt8* stack[16]; int top = 0;
		if (UInt8* root = *(UInt8**)(effect + 0x18)) stack[top++] = root;
		while (top) {
			UInt8* object = stack[--top];
			void** vtable = *(void***)object;
			typedef void* (__thiscall* IsFn)(void*);
			if (((IsFn)vtable[12])(object)) written += ApplyLookToSystem(object, file, look, report, reportSize);
			else if (((IsFn)vtable[3])(object)) {
				UInt8** children = *(UInt8***)(object + 0x9C + 4);
				const unsigned short count = *(unsigned short*)(object + 0x9C + 0xA);
				for (unsigned short i = 0; children && i < count && top < 16; i++) if (children[i]) stack[top++] = children[i];
			}
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { strcat_s(report, reportSize, "(stopped by a fault)"); }
	return written;
}

static void ReadLook(const char* section, const Look& file, Look& look);

// ---- Puff on each shot -----------------------------------------------------------------------------------------
static struct { void* actor; ULONGLONG tick; } LastPuff[16] = {};

static void* SpawnBurst(void* cell, float lifetime, const char* model, NiPoint3 at, float scale, float seconds, float rate, ULONGLONG now,
	UInt8* follow, NiPoint3 offset);
static bool VolumeWanted(int chain);
static void StartVolumeBurst(int chain, UInt8* node, const NiPoint3& offset, float seconds, float size);
static void Puff(MuzzleFlash* flash, ULONGLONG now) {
	if (!Settings.puff || !Switch(0)) return;
	if (Settings.puffPlayerOnly && flash->sourceActor != Player()) return;
	void* cell = *(void**)(flash->sourceActor + 0x40);
	if (!cell) return;
	int slot = -1, oldest = 0;
	for (int i = 0; i < 16; i++) {
		if (LastPuff[i].actor == flash->sourceActor) { slot = i; break; }
		if (LastPuff[i].tick < LastPuff[oldest].tick) oldest = i;
	}
	// Automatic guns (TESObjectWEAP flags at +0x100, 0x2 = automatic): at most one puff every fMinInterval, so a burst
	// builds a haze; other guns: a puff on every shot.
	const bool automatic = flash->sourceWeap && (*((UInt8*)flash->sourceWeap + 0x100) & 0x2);
	if (automatic && slot >= 0 && now - LastPuff[slot].tick < (ULONGLONG)(Settings.puffInterval * 1000.0f)) return;
	if (slot < 0) slot = oldest;
	LastPuff[slot].actor = flash->sourceActor;
	LastPuff[slot].tick = now;
	const NiPoint3 position = NodePosition(flash->node, Settings.puffOffset);
	if (flash->sourceActor == Player() && VolumeWanted(ChainPuff)) {
		StartVolumeBurst(ChainPuff, flash->node, Settings.puffOffset, Settings.puffBurst, 1.0f);
		if (LogEvent()) DebugLine("puff at (%.1f, %.1f, %.1f) -> volumetric", position.x, position.y, position.z);
		return;
	}
	void* effect = SpawnBurst(cell, Settings.puffLifetime, Settings.puffModel, position, Settings.puffScale, Settings.puffBurst, Settings.puffBurstRate, now,
		flash->sourceActor == Player() ? flash->node : nullptr, Settings.puffOffset);
	// v24: the NIF already contains this look. Avoid heuristic writes into a live particle object.
	if (LogEvent()) DebugLine("puff at (%.1f, %.1f, %.1f) -> %s (NIF look)", position.x, position.y, position.z,
		effect ? "created" : "NOT created");
}

// ---- Wisp driven by barrel heat ---------------------------------------------------------------------------------
// Each shot heats the barrel by heatPerShot; it cools by coolPerSecond. One wisp effect follows the muzzle, and its
// smoke rate is set every frame from the heat: nothing below heatStart, rising smoothly to maxRate at heatFull, and
// falling the same way as the barrel cools; the smoke already in the air always rises and fades out on its own.
// The rate is the value of the emitter controller's float interpolator (NiSingleInterpController interpolator at
// +0x34, NiFloatInterpolator value at +0x0C); it is only written after checking that it holds the file's rate.
static float Heat = 0.0f;
static float VisualHeat = 0.0f;
static float VisualHaze = 0.0f;
static float SpreadHeat = 0.0f;       // slow, separate spread toward the rear of the gun
static ULONGLONG LastShot = 0;          // the last shot of the current gun
static double LastShotSeconds = 0.0;   // the same on the precise clock (PreciseSeconds), for fades that change every frame
static ULONGLONG LastMuzzleSeen = 0;
static double LastLoopSeconds = 0.0;   // the last game-loop frame (PreciseSeconds)
static UInt8* TrailNode = nullptr;     // the muzzle of the last shot (kept alive with a reference)
static void* TrailActor = nullptr;
static CRITICAL_SECTION HeatLock;
static bool HeatLockReady = false;
static UInt8* HeatNode = nullptr;       // retained for NVR's render-time sample
static float HeatStrength = 0.0f, HeatLength = 0.0f, HeatRadius = 0.0f, HeatForwardLength = 0.0f;
static float HeatOffsetX = 0.0f, HeatOffsetY = 0.0f, HeatOffsetZ = 0.0f;
static float HazePublished[8] = {};
static float HazeBlast[2] = {};       // muzzle blast: displacement in pixels now (fades after each shot), radius in pixels
static float HazeWorld[6] = {};       // muzzle (with the glow offsets) and rearward axis, world space, game-loop time

static UInt8* FollowEffect = nullptr;  // the running wisp (kept alive with a reference)
static ULONGLONG FollowUntil = 0;      // when it stops following (just before the game removes it)
static float* RateValue = nullptr;     // its emitter's smoke rate, when found
static UInt8* RateController = nullptr; // its emitter controller (kept switched on while it is ours)
static Look FollowLook;                // the look written into the running wisp
static bool FollowHeld = false;        // the game held its own reference to the running wisp when it was spawned
// The after-fire trail: a second following effect, like the wisp, that only smokes after the last shot.
static UInt8* AfterEffect = nullptr;
static ULONGLONG AfterUntil = 0;
static float* AfterRate = nullptr;
static UInt8* AfterController = nullptr;
static bool AfterHeld = false;
static const int GlowPoints = 4;
static NiPoint3 GlowBack = { 0.0f, -1.0f, 0.0f };
static bool GlowUnavailable = false;   // fail closed if the glow mesh is missing
static ULONGLONG NextGlowPulse = 0;

// Each gun keeps its own heat: switching guns puts the current one's heat aside (it keeps cooling while away) and
// brings back the new one's, cooled by the time it was not in use.
struct GunHeat { void* weapon; float heat, spread; ULONGLONG since; };
static GunHeat Guns[16] = {};
static void* CurrentWeapon = nullptr;
static void ReleaseWisp(const char* why);
static void SwitchGun(void* weapon, ULONGLONG now) {
	if (weapon == CurrentWeapon) return;
	int slot = -1, oldest = 0;
	for (int i = 0; i < 16; i++) {
		if (Guns[i].weapon == CurrentWeapon && CurrentWeapon) { Guns[i].heat = Heat; Guns[i].spread = SpreadHeat; Guns[i].since = now; }
		if (Guns[i].since < Guns[oldest].since) oldest = i;
	}
	if (CurrentWeapon) {
		bool stored = false;
		for (int i = 0; i < 16; i++) stored |= Guns[i].weapon == CurrentWeapon;
		if (!stored) Guns[oldest] = GunHeat{ CurrentWeapon, Heat, SpreadHeat, now };
	}
	Heat = 0.0f;
	SpreadHeat = 0.0f;
	for (int i = 0; i < 16; i++) if (Guns[i].weapon == weapon) slot = i;
	if (slot >= 0) {
		const float awaySeconds = (now - Guns[slot].since) / 1000.0f;
		const float cooled = Guns[slot].heat - awaySeconds * Settings.coolPerSecond;
		Heat = cooled > 0.0f ? cooled : 0.0f;
		const float spread = Guns[slot].spread - awaySeconds / Settings.glowSpreadCoolSeconds;
		SpreadHeat = spread > 0.0f ? spread : 0.0f;
	}
	if (CurrentWeapon) ReleaseWisp("switched guns");
	NextGlowPulse = 0;
	VisualHeat = VisualHaze = 0.0f;
	CurrentWeapon = weapon;
}

static void OnPlayerShot(MuzzleFlash* flash, ULONGLONG now) {
	SwitchGun(flash->sourceWeap, now);
	LastShot = now;
	LastShotSeconds = PreciseSeconds();
	LastMuzzleSeen = now;
	Heat = Heat + Settings.heatPerShot > Settings.maxHeat ? Settings.maxHeat : Heat + Settings.heatPerShot;
	SpreadHeat += 1.0f / Settings.glowSpreadShots;
	if (SpreadHeat > 1.0f) SpreadHeat = 1.0f;
	if (TrailNode != flash->node) {
		NodeAddRef(flash->node);
		NodeRelease(TrailNode);
		TrailNode = flash->node;
	}
	TrailActor = flash->sourceActor;
}

static void LoadSettings();
static char IniPath[MAX_PATH];
static FILETIME IniWritten = {};
static char WeaponIniPath[MAX_PATH] = "";   // the equipped gun's settings file ("" = none), see UpdateGunSettings
static FILETIME WeaponIniWritten = {};
static int OwnGunValues(const char* path);
static bool MainValue(const char* section, const char* key, char* out, DWORD size);   // (the settings layer, below)
#ifndef GUNFX_REPLAY
static std::string SettingsPrint();
static std::string LoadedPrint;   // the GunFX values LoadSettings last read (SettingsPrint)
#endif
static ULONGLONG LastIniCheck = 0;

// Reads the settings again when they change while the game runs (the menu; the replay rig: GunFX.ini saved), and this gun's
// settings file when it is saved.
static void ReloadIniIfChanged(ULONGLONG now) {
	if (now - LastIniCheck < 250) return;   // (2026-11-16: 4 times a second, was 1: the menu)
	LastIniCheck = now;
	WIN32_FILE_ATTRIBUTE_DATA info;
	if (WeaponIniPath[0] && GetFileAttributesExA(WeaponIniPath, GetFileExInfoStandard, &info) &&
		CompareFileTime(&info.ftLastWriteTime, &WeaponIniWritten) != 0) {
		WeaponIniWritten = info.ftLastWriteTime;
		LogLine("this gun's settings file changed, reloading (%d own value(s))", OwnGunValues(WeaponIniPath));
		LoadSettings();
	}
#ifndef GUNFX_REPLAY
	// 2026-11-16: a menu change applies live. The settings revision changes with any setting (and with every step of a slider
	// drag), so the GunFX values are compared, and read again only once they have stood still for half a second: each new smoke
	// look writes a NIF of its own (BakeLook), and a drag would leave one per step on the disk.
	static unsigned seenRevision = 0xFFFFFFFFu;
	static std::string pending;
	static ULONGLONG pendingSince = 0;
	const unsigned revision = GunFXHost::Revision();
	if (revision == seenRevision && pending.empty()) return;
	seenRevision = revision;
	const std::string print = SettingsPrint();
	if (print == LoadedPrint) { pending.clear(); return; }
	if (print != pending) { pending = print; pendingSince = now; return; }
	if (now - pendingSince < 500) return;
	pending.clear();
	LogLine("settings changed in the menu, reading them again");
	LoadSettings();
#else
	if (!GetFileAttributesExA(IniPath, GetFileExInfoStandard, &info)) return;
	if (CompareFileTime(&info.ftLastWriteTime, &IniWritten) == 0) return;
	const bool first = IniWritten.dwLowDateTime == 0 && IniWritten.dwHighDateTime == 0;
	IniWritten = info.ftLastWriteTime;
	if (first) return;
	LogLine("GunFX.ini changed, reloading");
	LoadSettings();
#endif
	GlowUnavailable = false;
	if (FollowEffect) LogLine("running wisp keeps its baked NIF look");
}


// Our own effect files (Meshes\GunFX, or the older Meshes\BarrelSmoke paths).
static bool OwnModel(const char* model) {
	return _strnicmp(model, "GunFX", 5) == 0 || _strnicmp(model, "BarrelSmoke", 11) == 0;
}

static float* FindRate(UInt8* object, int depth, bool ownModel, UInt8** controller) {
	if (!object || depth > 8) return nullptr;
	void** vtable = *(void***)object;
	typedef void* (__thiscall* IsFn)(void*);
	if (((IsFn)vtable[12])(object)) {
		for (UInt8* ctrl = *(UInt8**)(object + 0x0C); ctrl; ctrl = *(UInt8**)(ctrl + 0x30)) {
			if (strcmp(RttiName(ctrl), "NiPSysEmitterCtlr")) continue;
			UInt8* interp = *(UInt8**)(ctrl + 0x34);
			if (!interp || strcmp(RttiName(interp), "NiFloatInterpolator")) return nullptr;
			float* value = (float*)(interp + 0x0C);
			if (!ownModel && fabsf(*value - Settings.fileRate) >= 0.01f) return nullptr;
			*controller = ctrl;
			return value;
		}
		return nullptr;
	}
	if (((IsFn)vtable[3])(object)) {
		UInt8** children = *(UInt8***)(object + 0x9C + 4);
		const unsigned short count = *(unsigned short*)(object + 0x9C + 0xA);
		for (unsigned short i = 0; children && i < count; i++)
			if (float* found = FindRate(children[i], depth + 1, ownModel, controller)) return found;
	}
	return nullptr;
}

// ---- Bursts (puff, ejection port) ----------------------------------------------------------------------------------
// A burst's emitter must start when it is spawned. A NIF switch "on for 0.06 s, then off" follows the controller's
// cycle on the game clock, not the spawn, so most bursts stayed silent and then several emitted together when the
// window came round. These effects keep the switch always on (as the strand does), and the plugin sets the emitter's
// rate at the spawn and to 0 after fBurstSeconds, the same rate control the wisp uses.
// While a burst emits, it follows its node (the muzzle or the ejection port), so a moving gun leaves a short soft smear
// along its path instead of a dense blob at the spot where the shot happened.
struct Burst { UInt8* effect; float* rate; UInt8* controller; ULONGLONG offAt; UInt8* follow; NiPoint3 offset; };
static NiPoint3 WorldPoint(UInt8* node, NiPoint3 at);
static Burst Bursts[64] = {};
static float BurstRate(const char* model) {
	char lower[MAX_PATH];
	strcpy_s(lower, model);
	_strlwr_s(lower);
	if (strstr(lower, "barrelport")) return 160.0f;   // the rates the files were built with
	if (strstr(lower, "barrelpuff")) return 60.0f;
	return 40.0f;
}
static void EndBurst(Burst& b) {
	if (b.rate) *b.rate = 0.0f;
	NodeRelease(b.follow);
	NodeRelease(b.effect);
	b = Burst{};
}
static void* SpawnBurst(void* cell, float lifetime, const char* model, NiPoint3 at, float scale, float seconds, float burstRate, ULONGLONG now,
	UInt8* follow, NiPoint3 offset) {
	void* effect = SpawnEffect(cell, lifetime, model, NiPoint3{ 0.0f, 0.0f, 1.0f }, at, scale, (UInt32)Settings.flags, nullptr);
	if (!effect) return nullptr;
	UInt8* root = *(UInt8**)((UInt8*)effect + 0x18);
	UInt8* controller = nullptr;
	float* rate = root ? FindRate(root, 0, OwnModel(model), &controller) : nullptr;
	if (!rate || !controller) return effect;   // not ours: the file decides
	*rate = burstRate > 0.0f ? burstRate : BurstRate(model);   // puffs of smoke per second while the burst lasts
	*(unsigned short*)(controller + 0x08) |= 0x8;
	int slot = 0;
	for (int i = 0; i < 64; i++) {
		if (!Bursts[i].effect) { slot = i; break; }
		if (Bursts[i].offAt < Bursts[slot].offAt) slot = i;
	}
	if (Bursts[slot].effect) EndBurst(Bursts[slot]);
	NodeAddRef((UInt8*)effect);
	NodeAddRef(follow);
	Bursts[slot] = Burst{ (UInt8*)effect, rate, controller, now + (ULONGLONG)(seconds * 1000.0f), follow, offset };
	return effect;
}
static void UpdateBursts(ULONGLONG now) {
	for (int i = 0; i < 64; i++) {
		Burst& b = Bursts[i];
		if (!b.effect) continue;
		if (now >= b.offAt) { EndBurst(b); continue; }
		// Still emitting: move it to where its node is now (only while the node is still in the scene).
		UInt8* root = *(UInt8**)(b.effect + 0x18);
		if (root && b.follow && *(volatile LONG*)(b.follow + 4) > 1)
			*(NiPoint3*)(root + 0x58) = WorldPoint(b.follow, NodePosition(b.follow, b.offset));
	}
}

// ---- Emitter copies along the barrel's path ------------------------------------------------------------------------
// An emitter moves once per frame, so every puff born during a frame starts at the same spot. A moving gun therefore
// leaves a dotted line of separate blobs, spaced by how far the barrel travelled that frame. The heat smoke and the
// trail run several copies instead (iEmitters): each frame the copies are spread evenly along the line from last
// frame's source position to this frame's, and the smoke rate is split between them, so the strand stays continuous
// while moving. Standing still they all sit at the same spot and look like one emitter.
struct Copies { UInt8* effect[3]; float* rate[3]; UInt8* ctrl[3]; int count; NiPoint3 last; bool hasLast; };
static Copies WispCopies = {}, AfterCopies = {};
static void SpawnCopies(Copies& c, int total, void* cell, float lifetime, const char* model, float scale, const NiPoint3& at) {
	c = Copies{};
	for (int k = 0; k < total - 1 && k < 3; k++) {
		void* effect = SpawnEffect(cell, lifetime, model, NiPoint3{ 0.0f, 0.0f, 1.0f }, at, scale, (UInt32)Settings.trailFlags, nullptr);
		if (!effect) break;
		UInt8* root = *(UInt8**)((UInt8*)effect + 0x18);
		UInt8* ctrl = nullptr;
		float* rate = root ? FindRate(root, 0, OwnModel(model), &ctrl) : nullptr;
		if (rate) { *rate = 0.0f; *(unsigned short*)(ctrl + 0x08) |= 0x8; }
		NodeAddRef((UInt8*)effect);
		c.effect[c.count] = (UInt8*)effect; c.rate[c.count] = rate; c.ctrl[c.count] = ctrl; c.count++;
	}
}
static void ReleaseCopies(Copies& c) {
	for (int k = 0; k < c.count; k++) {
		if (c.rate[k]) *c.rate[k] = 0.0f;
		NodeRelease(c.effect[k]);
	}
	c = Copies{};
}
static void PlaceCopies(Copies& c, UInt8* root, float* rate, UInt8* ctrl, const NiPoint3& at, float smoke) {
	NiPoint3 from = c.hasLast ? c.last : at;
	const float dx = at.x - from.x, dy = at.y - from.y, dz = at.z - from.z;
	if (dx * dx + dy * dy + dz * dz > 200.0f * 200.0f) from = at;   // a jump (teleport, load): no path to fill
	const int total = c.count + 1;
	for (int k = 0; k < c.count; k++) {
		const float f = (float)(k + 1) / total;
		UInt8* copyRoot = *(UInt8**)(c.effect[k] + 0x18);
		if (copyRoot) {
			NiPoint3& p = *(NiPoint3*)(copyRoot + 0x58);
			p.x = from.x + (at.x - from.x) * f; p.y = from.y + (at.y - from.y) * f; p.z = from.z + (at.z - from.z) * f;
		}
		if (c.rate[k]) { *c.rate[k] = smoke / total; *(unsigned short*)(c.ctrl[k] + 0x08) |= 0x8; }
	}
	if (root) *(NiPoint3*)(root + 0x58) = at;
	if (rate) { *rate = smoke / total; *(unsigned short*)(ctrl + 0x08) |= 0x8; }
	c.last = at;
	c.hasLast = true;
}

// Lets the running wisp go: its emitter is switched off and the smoke in the air rises and fades where it is.
static void ReleaseWisp(const char* why) {
	if (!FollowEffect) return;
	ReleaseCopies(WispCopies);
	UInt8* root = *(UInt8**)(FollowEffect + 0x18);
	if (root && FollowUntil > GetTickCount64()) StopEmitters(root, 0);
	if (why) DebugLine("wisp released: %s (heat %.1f)", why, Heat);
	NodeRelease(FollowEffect);
	FollowEffect = nullptr;
	RateValue = nullptr;
	RateController = nullptr;
}

// Lets the after-fire trail go (its smoke in the air keeps rising and fading).
static void ReleaseAfter(const char* why) {
	if (!AfterEffect) return;
	ReleaseCopies(AfterCopies);
	if (AfterRate) *AfterRate = 0.0f;
	if (why) DebugLine("after-fire trail released: %s (heat %.1f)", why, Heat);
	NodeRelease(AfterEffect);
	AfterEffect = nullptr;
	AfterRate = nullptr;
	AfterController = nullptr;
}

static NiPoint3 GlowDirection(UInt8* node) {
	if (!node) return GlowBack;
	const NiPoint3 muzzle = *(NiPoint3*)(node + 0x8C);
	UInt8* parent = *(UInt8**)(node + 0x18);
	NiPoint3 back = parent ? *(NiPoint3*)(parent + 0x8C) : muzzle;
	back.x -= muzzle.x; back.y -= muzzle.y; back.z -= muzzle.z;
	float length = sqrtf(back.x * back.x + back.y * back.y + back.z * back.z);
	if (length < 2.0f || length > 100.0f) {
		// Fallback to the node's local -Y direction when its parent is at the muzzle too.
		const float* rot = (float*)(node + 0x68);
		back = NiPoint3{ -rot[1], -rot[4], -rot[7] };
		length = sqrtf(back.x * back.x + back.y * back.y + back.z * back.z);
	}
	return length > 0.01f ? NiPoint3{ back.x / length, back.y / length, back.z / length } : GlowBack;
}

// The barrel's rearward direction for the volumetric smoke: the muzzle node's own forward axis (a weapon model points
// along its +Y), which lies along the barrel itself. GlowDirection's line to the node's parent is used only when that
// axis is far off it (over 45 degrees: a model whose muzzle node is turned); on some models the parent sits off the
// barrel line (by the sights or the grip), which tilted the barrel the smoke flows around toward it. `offDegrees`: how
// far apart the two are (for the log).
static NiPoint3 BarrelBack(UInt8* node, float* offDegrees = nullptr) {
	const NiPoint3 toParent = GlowDirection(node);
	if (!node) return toParent;
	const float* rot = (const float*)(node + 0x68);
	NiPoint3 axis = { -rot[1], -rot[4], -rot[7] };
	const float length = sqrtf(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
	if (!(length > 0.5f && length < 2.0f)) return toParent;   // not a sane rotation
	axis = NiPoint3{ axis.x / length, axis.y / length, axis.z / length };
	float c = axis.x * toParent.x + axis.y * toParent.y + axis.z * toParent.z;
	c = c < -1.0f ? -1.0f : c > 1.0f ? 1.0f : c;
	if (offDegrees) *offDegrees = acosf(c) * 57.29578f;
	return c > 0.7071f ? axis : toParent;
}

// ---- Gun shape probe (test) ---------------------------------------------------------------------------------------
// Whether the game keeps a gun model's mesh (vertex positions and triangles) in memory once it is loaded, so the smoke
// could flow around the gun's real shape instead of a tube. Logged once per gun (first person): each mesh part with its
// vertices and triangles, and the whole gun's extent along its own axes. NiObject vtable 8 IsTriStrips, 9 IsTriShape,
// 3 IsNiNode; NiGeometry model data at +0xB8, skin at +0xBC; NiGeometryData vertex count +0x08, positions +0x20, keep
// flags +0x38; NiTriBasedGeomData triangles +0x40; NiTriShapeData index list +0x48; NiTriStripsData strip indices +0x4C.
struct ShapeProbe { int parts, readable, skinned, vertices, triangles, lines; NiPoint3 lo, hi; };
static NiPoint3 IntoNode(UInt8* node, const NiPoint3& w) {      // a world point in a node's own space
	const float* R = (const float*)(node + 0x68);
	const NiPoint3 T = *(NiPoint3*)(node + 0x8C);
	const float S = *(float*)(node + 0x98) > 0.0001f ? *(float*)(node + 0x98) : 1.0f;
	const NiPoint3 d = { w.x - T.x, w.y - T.y, w.z - T.z };
	return NiPoint3{ (R[0] * d.x + R[3] * d.y + R[6] * d.z) / S, (R[1] * d.x + R[4] * d.y + R[7] * d.z) / S, (R[2] * d.x + R[5] * d.y + R[8] * d.z) / S };
}
static void ProbeShapes(UInt8* object, UInt8* root, ShapeProbe& p, int depth) {
	if (!object || depth > 24) return;
	void** vtable = *(void***)object;
	typedef void* (__thiscall* IsFn)(void*);
	const bool strips = ((IsFn)vtable[8])(object) != nullptr, shape = ((IsFn)vtable[9])(object) != nullptr;
	if (strips || shape) {
		p.parts++;
		UInt8* data = *(UInt8**)(object + 0xB8);
		const bool skinned = *(UInt8**)(object + 0xBC) != nullptr;
		p.skinned += skinned ? 1 : 0;
		const unsigned short verts = data ? *(unsigned short*)(data + 0x08) : 0;
		const NiPoint3* pos = data ? *(NiPoint3**)(data + 0x20) : nullptr;
		const unsigned short tris = data ? *(unsigned short*)(data + 0x40) : 0;
		const void* index = data ? *(void**)(data + (strips ? 0x4C : 0x48)) : nullptr;
		const UInt8 keep = data ? *(data + 0x38) : 0;
		const char* name = *(const char**)(object + 8);
		if (p.lines++ < 48)
			DebugLine("gun shape probe: %s '%s': %u vertices%s, %u triangles%s, keep flags 0x%02X%s", RttiName(object), name ? name : "",
				verts, pos ? "" : " (positions not kept)", tris, index ? "" : " (indices not kept)", keep, skinned ? ", skinned" : "");
		if (!pos || !verts) return;
		p.readable++;
		p.vertices += verts;
		p.triangles += tris;
		if (skinned) return;                                       // skinned positions are not in the part's own space
		const float* r = (const float*)(object + 0x68);
		const NiPoint3 t = *(NiPoint3*)(object + 0x8C);
		const float s = *(float*)(object + 0x98);
		for (unsigned short v = 0; v < verts; v++) {
			const NiPoint3& q = pos[v];
			const NiPoint3 w = { t.x + s * (r[0] * q.x + r[1] * q.y + r[2] * q.z), t.y + s * (r[3] * q.x + r[4] * q.y + r[5] * q.z),
				t.z + s * (r[6] * q.x + r[7] * q.y + r[8] * q.z) };
			const NiPoint3 l = IntoNode(root, w);
			p.lo = NiPoint3{ l.x < p.lo.x ? l.x : p.lo.x, l.y < p.lo.y ? l.y : p.lo.y, l.z < p.lo.z ? l.z : p.lo.z };
			p.hi = NiPoint3{ l.x > p.hi.x ? l.x : p.hi.x, l.y > p.hi.y ? l.y : p.hi.y, l.z > p.hi.z ? l.z : p.hi.z };
		}
		return;
	}
	if (!((IsFn)vtable[3])(object)) return;
	UInt8** children = *(UInt8***)(object + 0xA0);
	const unsigned short count = *(unsigned short*)(object + 0xA6);
	for (unsigned short i = 0; children && i < count; ++i) ProbeShapes(children[i], root, p, depth + 1);
}
static void ProbeGunShape(UInt8* root, UInt8* muzzle) {
	ShapeProbe p = {};
	p.lo = NiPoint3{ 1e9f, 1e9f, 1e9f };
	p.hi = NiPoint3{ -1e9f, -1e9f, -1e9f };
	__try { ProbeShapes(root, root, p, 0); }
	__except (EXCEPTION_EXECUTE_HANDLER) { LogLine("gun shape probe: stopped by a fault after %d part(s)", p.parts); }
	const NiPoint3 m = IntoNode(root, *(NiPoint3*)(muzzle + 0x8C));
	DebugLine("gun shape probe: %d mesh part(s), %d with readable positions (%d skinned): %d vertices, %d triangles", p.parts, p.readable,
		p.skinned, p.vertices, p.triangles);
	if (p.lo.x <= p.hi.x)
		DebugLine("gun shape probe: extent in the gun's own space x %.1f..%.1f, y %.1f..%.1f, z %.1f..%.1f; muzzle at (%.1f, %.1f, %.1f)",
			p.lo.x, p.hi.x, p.lo.y, p.hi.y, p.lo.z, p.hi.z, m.x, m.y, m.z);
}

// ---- The gun's real shape ------------------------------------------------------------------------------------------
// The volumetric smoke flows round the gun you hold as it really is (its sights, magazine, scope, bipod), not a tube:
// when a gun is first seen, its mesh parts' triangles are read from the game (on the game's thread, a few milliseconds)
// and turned into a distance map (GunShape.h) on a background thread, a fraction of a second; the tube stands in until it
// is ready. Parts the game hides and bending (skinned) parts are left out. [Volume] bGunShape (0 = always the tube).
// NiAVObject flags +0x30 (bit 0 hidden); NiGeometry model data +0xB8, skin +0xBC; NiGeometryData vertex count +0x08,
// positions +0x20; NiTriBasedGeomData triangles +0x40; NiTriShapeData index count +0x44, indices +0x48; NiTriStripsData
// strips +0x44, strip lengths +0x48, strip indices +0x4C.
struct ShapeJob { std::vector<float> tris; UInt8* root; void* weapon; GunShape* result; int parts, skipped, triangles; double ms; };
static const size_t ShapeMaxTriangles = 600000;
static GunShape* Shape = nullptr;                  // the shape of the gun you hold (game thread only)
static UInt8* ShapeRoot = nullptr;                 // the gun model it is built (or being built) for
static void* ShapeWeapon = nullptr;
static bool ShapeBuilding = false;
static ShapeJob* volatile ShapeDone = nullptr;     // a finished build, handed over by the background thread
static NiPoint3 ShapeMuzzle = {};                  // the muzzle in the gun's own space (when its build started)
static bool ShapeAlongX = false;                   // the barrel lies along the gun's own X axis (the muzzle at its far end)
// This frame: whether the smoke uses the shape, and the gun model's world transform.
static bool ShapeOn = false;
static float ShapeRot[9] = {}, ShapeScale = 1.0f;
static NiPoint3 ShapePos = {};

static void CollectShape(UInt8* object, UInt8* root, ShapeJob& job, int depth) {
	if (!object || depth > 24) return;
	if (*(UInt32*)(object + 0x30) & 1) return;                       // hidden, with everything under it
	void** vtable = *(void***)object;
	typedef void* (__thiscall* IsFn)(void*);
	const bool strips = ((IsFn)vtable[8])(object) != nullptr, shape = ((IsFn)vtable[9])(object) != nullptr;
	if (strips || shape) {
		UInt8* data = *(UInt8**)(object + 0xB8);
		const unsigned short nv = data ? *(unsigned short*)(data + 0x08) : 0;
		const NiPoint3* pos = data ? *(NiPoint3**)(data + 0x20) : nullptr;
		if (!pos || !nv || *(UInt8**)(object + 0xBC)) { job.skipped++; return; }   // no positions kept, or it bends (skinned)
		// From the part's own space to the gun's, through the world (both as they are now): M = R^T r s / S.
		const float* r = (const float*)(object + 0x68);
		const NiPoint3 t = *(NiPoint3*)(object + 0x8C);
		const float s = *(float*)(object + 0x98);
		const float* R = (const float*)(root + 0x68);
		const NiPoint3 T = *(NiPoint3*)(root + 0x8C);
		const float S = *(float*)(root + 0x98) > 0.0001f ? *(float*)(root + 0x98) : 1.0f;
		float M[9];
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++) M[i * 3 + j] = (R[i] * r[j] + R[3 + i] * r[3 + j] + R[6 + i] * r[6 + j]) * s / S;
		const NiPoint3 dt = { t.x - T.x, t.y - T.y, t.z - T.z };
		const float o[3] = { (R[0] * dt.x + R[3] * dt.y + R[6] * dt.z) / S, (R[1] * dt.x + R[4] * dt.y + R[7] * dt.z) / S,
			(R[2] * dt.x + R[5] * dt.y + R[8] * dt.z) / S };
		auto put = [&](unsigned short a) {
			const NiPoint3& q = pos[a];
			job.tris.push_back(M[0] * q.x + M[1] * q.y + M[2] * q.z + o[0]);
			job.tris.push_back(M[3] * q.x + M[4] * q.y + M[5] * q.z + o[1]);
			job.tris.push_back(M[6] * q.x + M[7] * q.y + M[8] * q.z + o[2]);
		};
		if (shape) {
			const unsigned short count = *(unsigned short*)(data + 0x40);
			const UInt32 len = *(UInt32*)(data + 0x44);
			const unsigned short* idx = *(unsigned short**)(data + 0x48);
			if (!idx || len != 3u * count || job.tris.size() / 9 + count > ShapeMaxTriangles) { job.skipped++; return; }
			for (UInt32 k = 0; k < len; k++) if (idx[k] >= nv) { job.skipped++; return; }
			for (UInt32 k = 0; k < len; k++) put(idx[k]);
		}
		else {
			const unsigned short ns = *(unsigned short*)(data + 0x44);
			const unsigned short* lens = *(unsigned short**)(data + 0x48);
			const unsigned short* idx = *(unsigned short**)(data + 0x4C);
			if (!lens || !idx) { job.skipped++; return; }
			UInt32 total = 0;
			for (unsigned short k = 0; k < ns; k++) total += lens[k];
			if (job.tris.size() / 9 + total > ShapeMaxTriangles) { job.skipped++; return; }
			for (UInt32 k = 0; k < total; k++) if (idx[k] >= nv) { job.skipped++; return; }
			const unsigned short* q = idx;
			for (unsigned short k = 0; k < ns; k++) {
				const int L = lens[k];
				for (int j = 0; j + 2 < L; j++) {
					const unsigned short a = q[j], b = q[j + 1], c = q[j + 2];
					if (a == b || b == c || a == c) continue;                // a strip's joining (empty) triangle
					put(a); put(b); put(c);
				}
				q += L;
			}
		}
		job.parts++;
		return;
	}
	if (!((IsFn)vtable[3])(object)) return;
	UInt8** children = *(UInt8***)(object + 0xA0);
	const unsigned short count = *(unsigned short*)(object + 0xA6);
	for (unsigned short i = 0; children && i < count; ++i) CollectShape(children[i], root, job, depth + 1);
}
static bool CollectShapeSafe(UInt8* root, ShapeJob& job) {
	__try { CollectShape(root, root, job, 0); return true; }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The background build: a map 0.5 units fine around the gun, 5 units beyond it, its skin 0.3 units out from the
// drawn surface; at most 2 million grid points (coarser for a huge gun).
static DWORD WINAPI ShapeWorker(LPVOID arg) {
	ShapeJob* job = (ShapeJob*)arg;
	LARGE_INTEGER f, a, b;
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&a);
	try {
		GunShape* shape = new GunShape();
		if (shape->Build(job->tris.data(), job->triangles, 0.5f, 5.0f, 0.3f, 2000000)) job->result = shape;
		else delete shape;
	}
	catch (...) { job->result = nullptr; }
	QueryPerformanceCounter(&b);
	job->ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
	std::vector<float>().swap(job->tris);
	InterlockedExchangePointer((PVOID volatile*)&ShapeDone, job);
	return 0;
}

// Once a frame with the gun you hold (its model's root node, and its muzzle node): takes a finished build, and starts
// one for a gun that has none yet (one at a time).
static void UpdateGunShape(UInt8* root, UInt8* muzzleNode) {
	if (ShapeJob* done = (ShapeJob*)InterlockedExchangePointer((PVOID volatile*)&ShapeDone, nullptr)) {
		ShapeBuilding = false;
		if (done->result && done->root == root && done->root == ShapeRoot && done->weapon == CurrentWeapon) {
			delete Shape;
			Shape = done->result;
			done->result = nullptr;
			DebugLine("gun shape: ready - %d part(s) (%d left out), %d triangles -> map %d x %d x %d at %.2f units (%.1f MB) in %.0f ms; "
				"barrel along the gun's own axis: %s", done->parts, done->skipped, done->triangles, Shape->nx, Shape->ny, Shape->nz,
				Shape->voxel, Shape->d.size() * 4.0 / 1048576.0, done->ms, ShapeAlongX ? "yes" : "no");
		}
		else if (!done->result) LogLine("gun shape: no map could be built from the gun's model; the tube stands in");
		delete done->result;
		delete done;
	}
	if (!root || !Settings.volume || !Settings.volumeWrapGun || !Settings.volumeGunShape || ShapeBuilding) return;
	// A gun whose model could not be read is tried again after 3 s, up to 3 times.
	static int tries = 0;
	static ULONGLONG triedAt = 0;
	const ULONGLONG now = GetTickCount64();
	if (root == ShapeRoot && CurrentWeapon == ShapeWeapon) {
		if (Shape || tries >= 3 || now - triedAt < 3000) return;
	}
	else tries = 0;
	tries++;
	triedAt = now;
	delete Shape;                                                       // the tube stands in until the new map is ready
	Shape = nullptr;
	ShapeRoot = root;
	ShapeWeapon = CurrentWeapon;
	ShapeJob* job = new ShapeJob();
	job->root = root; job->weapon = CurrentWeapon; job->result = nullptr; job->parts = job->skipped = 0; job->ms = 0.0;
	const bool read = CollectShapeSafe(root, *job);
	job->triangles = (int)(job->tris.size() / 9);
	if (!read || !job->triangles) {
		LogLine("gun shape: %s; the tube stands in", read ? "the gun's model has no readable mesh" : "reading the gun's model stopped by a fault");
		delete job;
		return;
	}
	// Where the muzzle is in the gun's own space, and whether the barrel lies along its X axis (the muzzle near its far end).
	ShapeMuzzle = IntoNode(root, *(NiPoint3*)(muzzleNode + 0x8C));
	float far_x = -1e9f;
	for (size_t i = 0; i < job->tris.size(); i += 3) far_x = job->tris[i] > far_x ? job->tris[i] : far_x;
	ShapeAlongX = ShapeMuzzle.x > 0.0f && ShapeMuzzle.x > 0.85f * far_x && fabsf(ShapeMuzzle.y) < 0.3f * ShapeMuzzle.x &&
		fabsf(ShapeMuzzle.z) < 0.3f * ShapeMuzzle.x;
	ShapeBuilding = true;
	HANDLE thread = CreateThread(nullptr, 0, ShapeWorker, job, 0, nullptr);
	if (thread) CloseHandle(thread);
	else { ShapeBuilding = false; delete job; }
}

// The distance (world units) from a point to the surface of the gun you hold (negative inside it), and the outward
// direction there. Only while ShapeOn.
static float GunSurface(const NiPoint3& p, NiPoint3* n) {
	const float* R = ShapeRot;
	const NiPoint3 d = { p.x - ShapePos.x, p.y - ShapePos.y, p.z - ShapePos.z };
	const float l[3] = { (R[0] * d.x + R[3] * d.y + R[6] * d.z) / ShapeScale, (R[1] * d.x + R[4] * d.y + R[7] * d.z) / ShapeScale,
		(R[2] * d.x + R[5] * d.y + R[8] * d.z) / ShapeScale };
	float ln[3];
	const float dist = Shape->Sample(l, ln);
	*n = NiPoint3{ R[0] * ln[0] + R[1] * ln[1] + R[2] * ln[2], R[3] * ln[0] + R[4] * ln[1] + R[5] * ln[2], R[6] * ln[0] + R[7] * ln[1] + R[8] * ln[2] };
	return dist * ShapeScale;
}

static NiPoint3 GlowAt(int index, int count) {
	NiPoint3 at = *(NiPoint3*)(TrailNode + 0x8C);
	const float distance = count > 1 ? Settings.glowLength * index / (count - 1) : 0.0f;
	at.x += GlowBack.x * distance;
	at.y += GlowBack.y * distance;
	at.z += GlowBack.z * distance;
	return at;
}

static bool InFirstPersonModel(UInt8* node);
static NiPoint3 WorldPoint(UInt8* node, NiPoint3 at) {
	if (!node || !InFirstPersonModel(node)) return at;
	const float* pos = (const float*)((UInt8*)Player() + 0x30);
	const float dx = at.x - pos[0], dy = at.y - pos[1], dz = at.z - pos[2];
	const bool local = dx * dx + dy * dy + dz * dz > 1000.0f * 1000.0f;
	static int logged = 0;
	if (logged < 3) {
		logged++;
		DebugLine("first-person point (%.1f, %.1f, %.1f), player at (%.1f, %.1f, %.1f): %s", at.x, at.y, at.z, pos[0], pos[1], pos[2],
			local ? "player-relative, converted to world" : "world space");
	}
	if (local) { at.x += pos[0]; at.y += pos[1]; at.z += pos[2]; }
	return at;
}

static void PublishHeat(bool muzzleAlive, float strength) {
	const float haze = Settings.haze && Switch(4) ? (Settings.hazePreview ? 1.0f : VisualHaze) : 0.0f;
	if (!Settings.glow || !Switch(3)) strength = 0.0f;
	// Muzzle blast: a quick wide shimmer on every shot, independent of the barrel heat; full at the shot, fading over
	// fSeconds (an exponential fade, gone after about three times that).
	const float since = LastShotSeconds > 0.0 ? (float)(PreciseSeconds() - LastShotSeconds) : 1e9f;   // precise: a short fade
	float pulse = Settings.blast && Switch(5) && since < Settings.blastSeconds * 4.0f ? expf(-since / Settings.blastSeconds) : 0.0f;
	if (pulse < 0.02f) pulse = 0.0f;
	UInt8* next = muzzleAlive && (strength > 0.01f || haze > 0.001f || pulse > 0.0f) ? TrailNode : nullptr;
	NodeAddRef(next);
	EnterCriticalSection(&HeatLock);
	UInt8* old = HeatNode;
	HeatNode = next;
	HeatStrength = next ? strength : 0.0f;
	HeatLength = Settings.glowPreview ? Settings.glowLength :
		Settings.glowMinLength + (Settings.glowLength - Settings.glowMinLength) * SpreadHeat;
	HeatRadius = Settings.glowRadius;
	HeatForwardLength = Settings.glowForwardLength;
	HeatOffsetX = Settings.glowOffsetX;
	HeatOffsetY = Settings.glowOffsetY;
	HeatOffsetZ = Settings.glowOffsetZ;
	HazePublished[0] = next ? haze : 0.0f;
	HazePublished[1] = Settings.hazePixels;
	// Size grows with the heat too: fMinSize of full size when the haze first shows, full size at full heat (the
	// strength already follows the heat in the shader).
	const float hazeSize = Settings.hazeMinSize + (1.0f - Settings.hazeMinSize) * (haze < 0.0f ? 0.0f : haze > 1.0f ? 1.0f : haze);
	HazePublished[2] = Settings.hazeHeight * hazeSize;
	HazePublished[3] = Settings.hazeWidth * hazeSize;
	HazePublished[4] = Settings.hazeSpeed;
	HazePublished[5] = Settings.hazeLength * hazeSize;
	HazePublished[6] = (float)Settings.hazeMask;
	HazeBlast[0] = next ? Settings.blastPixels * pulse : 0.0f;
	HazeBlast[1] = Settings.blastRadius;
	if (next) {
		NiPoint3 muzzle = *(NiPoint3*)(next + 0x8C);
		const float* rot = (const float*)(next + 0x68);
		muzzle.x += rot[0] * Settings.glowOffsetX + rot[1] * Settings.glowOffsetY + rot[2] * Settings.glowOffsetZ;
		muzzle.y += rot[3] * Settings.glowOffsetX + rot[4] * Settings.glowOffsetY + rot[5] * Settings.glowOffsetZ;
		muzzle.z += rot[6] * Settings.glowOffsetX + rot[7] * Settings.glowOffsetY + rot[8] * Settings.glowOffsetZ;
		muzzle = WorldPoint(next, muzzle);
		const NiPoint3 back = GlowDirection(next);
		HazeWorld[0] = muzzle.x; HazeWorld[1] = muzzle.y; HazeWorld[2] = muzzle.z;
		HazeWorld[3] = back.x; HazeWorld[4] = back.y; HazeWorld[5] = back.z;
	}
	LeaveCriticalSection(&HeatLock);
	NodeRelease(old);
}

static bool SpawnGlow(float heatFraction) {
	void* cell = TrailActor ? *(void**)((UInt8*)TrailActor + 0x40) : nullptr;
	if (!cell) return false;
	const int points = 1 + (int)(heatFraction * (GlowPoints - 1));
	for (int i = 0; i < points; i++) {
		const NiPoint3 position = GlowAt(i, points);
		void* effect = SpawnEffect(cell, 0.65f, "GunFX\\barrelglow.nif",
			NiPoint3{ 0.0f, 0.0f, 1.0f }, position, Settings.glowScale * (1.0f - 0.12f * i),
			(UInt32)Settings.trailFlags, nullptr);
		if (!effect) { LogLine("barrel glow NOT created; disabled until INI reload"); GlowUnavailable = true; return false; }
	}
	if (LogEvent()) DebugLine("barrel glow pulse: %d point(s) across %.1f units", points, Settings.glowLength);
	return true;
}

static bool SpawnWisp(ULONGLONG now, const NiPoint3& position) {
	void* cell = TrailActor ? *(void**)((UInt8*)TrailActor + 0x40) : nullptr;
	if (!cell) return false;
	void* effect = SpawnEffect(cell, Settings.effectSeconds, Settings.trailModel, NiPoint3{ 0.0f, 0.0f, 1.0f }, position,
		Settings.trailScale, (UInt32)Settings.trailFlags, nullptr);
	if (!effect) { LogLine("wisp %s NOT created", Settings.trailModel); return false; }
	UInt8* root = *(UInt8**)((UInt8*)effect + 0x18);   // the cloned model (BSTempEffectParticle::Init loads it while spawning)
	if (Settings.trailSmokeOnly && root) HideSolidMeshes(root, 0);
	FollowEffect = (UInt8*)effect;
	NodeAddRef(FollowEffect);
	FollowHeld = *(volatile LONG*)(FollowEffect + 4) > 1;
	// v24: use the NIF's baked look; do not scan and modify the live modifier list.
	if (LogEvent()) DebugLine("wisp uses baked NIF look");
	FollowUntil = now + (ULONGLONG)(Settings.effectSeconds * 1000.0f) - 150;
	// Our own effects (in Meshes\GunFX) are always taken over, whatever rate they start with; other models only
	// when their rate matches fFileRate.
	const bool ownModel = OwnModel(Settings.trailModel);
	RateController = nullptr;
	RateValue = root ? FindRate(root, 0, ownModel, &RateController) : nullptr;
	const float startRate = RateValue ? *RateValue : -1.0f;
	const unsigned short startFlags = RateController ? *(unsigned short*)(RateController + 0x08) : 0;
	if (RateValue) { *RateValue = 0.0f; *(unsigned short*)(RateController + 0x08) |= 0x8; }
	SpawnCopies(WispCopies, Settings.wispEmitters, cell, Settings.effectSeconds, Settings.trailModel, Settings.trailScale, position);
	DebugLine("wisp %s at (%.1f, %.1f, %.1f) for up to %.0f s, heat %.1f (game reference %s); smoke rate %s (started at %.1f, emitter %s; controller %p, rate %p)",
		Settings.trailModel, position.x, position.y, position.z, Settings.effectSeconds, Heat, FollowHeld ? "yes" : "no",
		RateValue ? "under control" : "NOT found (on/off only; check fFileRate matches the file)", startRate,
		(startFlags & 0x8) ? "on" : "OFF", RateController, RateValue);
	return true;
}

// Spawns the after-fire trail at the barrel (same source node and offsets as the wisp); its rate starts at 0.
static bool SpawnAfter(ULONGLONG now, const NiPoint3& position) {
	void* cell = TrailActor ? *(void**)((UInt8*)TrailActor + 0x40) : nullptr;
	if (!cell) return false;
	void* effect = SpawnEffect(cell, Settings.effectSeconds, Settings.afterModel, NiPoint3{ 0.0f, 0.0f, 1.0f }, position,
		Settings.afterScale, (UInt32)Settings.trailFlags, nullptr);
	if (!effect) { LogLine("after-fire trail %s NOT created", Settings.afterModel); return false; }
	UInt8* root = *(UInt8**)((UInt8*)effect + 0x18);
	AfterEffect = (UInt8*)effect;
	NodeAddRef(AfterEffect);
	AfterHeld = *(volatile LONG*)(AfterEffect + 4) > 1;
	AfterUntil = now + (ULONGLONG)(Settings.effectSeconds * 1000.0f) - 150;
	AfterController = nullptr;
	AfterRate = root ? FindRate(root, 0, OwnModel(Settings.afterModel), &AfterController) : nullptr;
	if (AfterRate) { *AfterRate = 0.0f; *(unsigned short*)(AfterController + 0x08) |= 0x8; }
	SpawnCopies(AfterCopies, Settings.afterEmitters, cell, Settings.effectSeconds, Settings.afterModel, Settings.afterScale, position);
	DebugLine("after-fire trail %s, heat %.1f, %d emitter(s); smoke rate %s", Settings.afterModel, Heat, AfterCopies.count + 1,
		AfterRate ? "under control" : "NOT found");
	return true;
}

// ---- Ribbon report (test) -----------------------------------------------------------------------------------------
// Logs what the running ribbon holds, once a second, iRibbonReports times. The game's ribbon data (BSStripPSysData,
// read from BSStripPSysData::UpdatePoints 0xC26120 and NiDX9Renderer::Do_RenderParticleStrips 0xE70720): positions
// +0x20, colours +0x28, radii +0x4C, active particles +0x50 (ushort), sizes +0x54, particle info +0x70 (0x1C each: age
// +0xC, life span +0x10), ribbon per particle +0x7C (0x20 each: points +0, count +4, ring size +0x10, oldest +0x14,
// next write +0x18), max points +0x80; a point is 0x84 bytes: position +0, colour +0x6C, size +0x7C, radius +0x80.
static int RibbonReports = 0;
static ULONGLONG LastRibbonReport = 0;
static UInt8* FindStripSystem(UInt8* object, int depth) {
	if (!object || depth > 8) return nullptr;
	void** vtable = *(void***)object;
	typedef void* (__thiscall* IsFn)(void*);
	if (((IsFn)vtable[12])(object)) return strcmp(RttiName(object), "BSStripParticleSystem") ? nullptr : object;
	if (((IsFn)vtable[3])(object)) {
		UInt8** children = *(UInt8***)(object + 0x9C + 4);
		const unsigned short count = *(unsigned short*)(object + 0x9C + 0xA);
		for (unsigned short i = 0; children && i < count; i++) if (UInt8* found = FindStripSystem(children[i], depth + 1)) return found;
	}
	return nullptr;
}
static void ReportRibbon(UInt8* root, ULONGLONG now) {
	if (RibbonReports <= 0 || now - LastRibbonReport < 1000) return;
	LastRibbonReport = now;
	RibbonReports--;
	__try {
		UInt8* psys = FindStripSystem(root, 0);
		if (!psys) { LogLine("ribbon: no ribbon particle system in this effect"); return; }
		UInt8* data = nullptr;
		for (int off = 0x9C; off <= 0xD0 && !data; off += 4) {
			UInt8* p = *(UInt8**)(psys + off);
			const char* n = SafeRtti(p);
			if (n && !strcmp(n, "BSStripPSysData")) data = p;
		}
		if (!data) { LogLine("ribbon: data not found"); return; }
		const NiPoint3 muzzle = TrailNode ? *(NiPoint3*)(TrailNode + 0x8C) : NiPoint3{};
		const NiPoint3 world = *(NiPoint3*)(psys + 0x8C);
		const float* bound = *(float**)(psys + 0x20);
		const unsigned short active = *(unsigned short*)(data + 0x50), maxPoints = *(unsigned short*)(data + 0x80);
		DebugLine("ribbon @%llu: %u strands (max points %u); system at (%.0f, %.0f, %.0f) scale %.2f flags %#x; muzzle (%.0f, %.0f, %.0f); bound %s",
			now % 100000, active, maxPoints, world.x, world.y, world.z, *(float*)(psys + 0x98), *(UInt32*)(psys + 0x30),
			muzzle.x, muzzle.y, muzzle.z, bound && !IsBadReadPtr(bound, 16) ? "" : "none");
		if (bound && !IsBadReadPtr(bound, 16))
			DebugLine("  bound centre (%.0f, %.0f, %.0f) radius %.1f", bound[0], bound[1], bound[2], bound[3]);
		UInt8* info = *(UInt8**)(data + 0x70);
		UInt8* strips = *(UInt8**)(data + 0x7C);
		NiPoint3* positions = *(NiPoint3**)(data + 0x20);
		float* colours = *(float**)(data + 0x28);
		int totalPoints = 0;
		for (unsigned short i = 0; strips && i < active; i++) totalPoints += *(unsigned short*)(strips + i * 0x20 + 4);
		DebugLine("  points in all strands: %d", totalPoints);
		for (unsigned short i = 0; strips && info && i < active && i < 4; i++) {
			UInt8* s = strips + i * 0x20;
			UInt8* points = *(UInt8**)s;
			const int count = *(unsigned short*)(s + 4), ring = *(int*)(s + 0x10), oldest = *(int*)(s + 0x14), next = *(int*)(s + 0x18);
			const float age = *(float*)(info + i * 0x1C + 0xC), life = *(float*)(info + i * 0x1C + 0x10);
			const NiPoint3 head = positions ? positions[i] : NiPoint3{};
			const float headAlpha = colours ? colours[i * 4 + 3] : -1.0f;
			DebugLine("  strand %u: age %.2f of %.2f s, head (%.0f, %.0f, %.0f) alpha %.2f, %d points (ring %d, oldest %d, next %d)",
				i, age, life, head.x, head.y, head.z, headAlpha, count, ring, oldest, next);
			if (!points || ring <= 0 || count <= 0) continue;
			const int idx[3] = { oldest % ring, (oldest + count / 2) % ring, ((next - 1) % ring + ring) % ring };
			const char* label[3] = { "oldest", "middle", "newest" };
			for (int k = 0; k < 3; k++) {
				UInt8* pt = points + idx[k] * 0x84;
				const NiPoint3 at = *(NiPoint3*)pt;
				const float* c = (float*)(pt + 0x6C);
				DebugLine("    %s point (%.0f, %.0f, %.0f) [%.0f above muzzle, %.0f away] width %.2f (size %.2f x radius %.2f) colour %.2f %.2f %.2f alpha %.2f",
					label[k], at.x, at.y, at.z, at.z - muzzle.z,
					sqrtf((at.x - muzzle.x) * (at.x - muzzle.x) + (at.y - muzzle.y) * (at.y - muzzle.y) + (at.z - muzzle.z) * (at.z - muzzle.z)),
					*(float*)(pt + 0x7C) * *(float*)(pt + 0x80), *(float*)(pt + 0x7C), *(float*)(pt + 0x80), c[0], c[1], c[2], c[3]);
			}
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { LogLine("ribbon: report stopped by a fault"); }
}

// The current visible model is either the first-person model (PlayerCharacter +0x694) or the player's 3D
// (renderData +0x64 -> niNode +0x14).
static UInt8* VisibleRoot() {
	UInt8* player = (UInt8*)Player();
	if (!player) return nullptr;
	if (*(player + 0x64C)) {
		UInt8* renderData = *(UInt8**)(player + 0x64);
		return renderData ? *(UInt8**)(renderData + 0x14) : nullptr;
	}
	return *(UInt8**)(player + 0x694);
}
typedef UInt8* (__thiscall* GetFireNodeFn)(void* weapon, UInt8* root);
static const GetFireNodeFn GetFireNode = (GetFireNodeFn)0x00525700;
typedef bool(__thiscall* IsWeaponDrawnFn)(void* actor);
static const IsWeaponDrawnFn IsWeaponDrawn = (IsWeaponDrawnFn)0x008A16D0;   // Actor::IsWeaponDrawn (IsWeaponOut condition)


static void HandleShots(ULONGLONG now);   // below, with the hooks

// Actor::GetCurrentWeapon 0x008A1710 (thiscall actor) -> TESObjectWEAP (as in Actor::GetProjectileNode). TESObjectWEAP:
// form ID +0x0C, name (TESFullName at +0x30, BSString data) +0x34, weapon type +0xF4.
typedef void* (__thiscall* GetCurrentWeaponFn)(void* actor);
static const GetCurrentWeaponFn GetEquippedWeapon = (GetCurrentWeaponFn)0x008A1710;
static void* SettingsWeapon = nullptr;
static const char* const GunSections[] = { "Muzzle", "Puff", "Wisp", "Trail", "Volume", "Glow", "Haze", "Blast", "Ejection" };
static bool SkippedGunKey(const char* key) {   // test switches, not meant per gun
	return !_stricmp(key, "iRibbonReports") || !_stricmp(key, "bFollowMuzzle") || !_stricmp(key, "fConstantRate") ||
		!_stricmp(key, "iFlags") || !_stricmp(key, "fFileRate") || !_stricmp(key, "bSmokeOnly");
}
static void WriteGunIni(const char* path, const char* name, UInt32 formId) {
	FILE* f = nullptr;
	if (fopen_s(&f, path, "w") || !f) { LogLine("could not create %s", path); return; }
	fprintf(f, "; GunFX settings for %s (form %08X).\n", name, formId);
	fprintf(f, "; Every line starting with ; uses the main value (Vegas Shaders' menu, Gun effects; shown here as it was when this\n");
	fprintf(f, "; file was made). To give this gun its own value, delete the ; at the start of the line and change the value.\n");
	fprintf(f, "; Put the ; back to return to the main setting. Changes apply while the game runs. Delete this file to start over.\n");
	char value[512];
	for (const char* section : GunSections) {
		fprintf(f, "\n[%s]\n", section);
		for (const GunFXKeys::Key& k : GunFXKeys::kKeys) {   // (2026-11-16: the keys of the old GunFX.ini, in its order)
			if (_stricmp(k.Section, section) || SkippedGunKey(k.Ini)) continue;
			if (!MainValue(section, k.Ini, value, sizeof(value))) strcpy_s(value, k.Default);
			fprintf(f, ";%s=%s\n", k.Ini, value);
		}
	}
	fclose(f);
}
static int OwnGunValues(const char* path) {
	int own = 0;
	char keys[8192];
	for (const char* section : GunSections) {
		GetPrivateProfileStringA(section, NULL, "", keys, sizeof(keys), path);
		for (const char* k = keys; *k; k += strlen(k) + 1) own++;
	}
	return own;
}
static void UpdateGunSettings() {
	void* player = Player();
	void* weapon = player ? GetEquippedWeapon(player) : nullptr;
	if (weapon == SettingsWeapon) return;
	SettingsWeapon = weapon;
	if (weapon && *((UInt8*)weapon + 0xF4) >= 3 && *((UInt8*)weapon + 0xF4) <= 9) SwitchGun(weapon, GetTickCount64());
	WeaponIniPath[0] = 0;
	WeaponIniWritten = FILETIME{};
	const UInt8 type = weapon ? *((UInt8*)weapon + 0xF4) : 0;
	if (weapon && type >= 3 && type <= 9) {
		const UInt32 formId = *(UInt32*)((UInt8*)weapon + 0x0C);
		const char* raw = *(const char**)((UInt8*)weapon + 0x34);
		char name[96];
		if (raw && *raw) strncpy_s(name, raw, _TRUNCATE); else sprintf_s(name, "Weapon %08X", formId);
		char file[96];
		int n = 0;
		for (const char* c = name; *c && n < 80; c++) file[n++] = strchr("\\/:*?\"<>|", *c) || (unsigned char)*c < 32 ? '_' : *c;
		while (n > 0 && (file[n - 1] == ' ' || file[n - 1] == '.')) n--;
		file[n] = 0;
		if (!n) sprintf_s(file, "Weapon %08X", formId);
		char folder[MAX_PATH];
		strcpy_s(folder, IniPath);
		char* slash = strrchr(folder, 0x5C);
		if (slash) slash[1] = 0;
		strcat_s(folder, "GunFX");
		CreateDirectoryA(folder, NULL);
		strcat_s(folder, "\\Weapons");
		CreateDirectoryA(folder, NULL);
		sprintf_s(WeaponIniPath, "%s\\%s.ini", folder, file);
		if (GetFileAttributesA(WeaponIniPath) == INVALID_FILE_ATTRIBUTES) {
			char older[MAX_PATH];
			strcpy_s(older, IniPath);
			char* cut = strrchr(older, 0x5C);
			if (cut) cut[1] = 0;
			sprintf_s(older + strlen(older), MAX_PATH - strlen(older), "BarrelSmoke\\Weapons\\%s.ini", file);
			if (CopyFileA(older, WeaponIniPath, TRUE)) LogLine("took over the BarrelSmoke settings of %s", name);
			else WriteGunIni(WeaponIniPath, name, formId);
			LogLine("created per-gun settings: GunFX\\Weapons\\%s.ini", file);
		}
		WIN32_FILE_ATTRIBUTE_DATA info;
		if (GetFileAttributesExA(WeaponIniPath, GetFileExInfoStandard, &info)) WeaponIniWritten = info.ftLastWriteTime;
		DebugLine("equipped %s: settings from GunFX\\Weapons\\%s.ini (%d own value(s)), the rest from the menu",
			name, file, OwnGunValues(WeaponIniPath));
	}
	LoadSettings();
}

// ---- Volumetric smoke (prototype) ---------------------------------------------------------------------------------
// Instead of the game's particles, GunFX's smoke can be kept here as chains of points: the heat smoke and the
// after-fire trail (the path the barrel left behind), and bursts for the muzzle puff and the ejection smoke. Each point
// rises, curls, slows down, grows and fades with age; NVR draws every pair of neighbouring points as a soft tube of smoke
// (VolumetricSmoke.fx), so the smoke is continuous however fast the gun moves. Menu switch Main > GunFX >
// VolumetricSmoke; settings in [Volume] (which smoke turns volumetric, and how); the look of each comes from its own
// section ([Wisp], [Trail], [Puff], [Ejection]). The sprite version of a smoke is not spawned while its volumetric
// version is on. Points are in world space (game-loop positions). Player only.
// side: which side of you it went to when your body pushed it (-1 left, 1 right, 0 not yet); frayed: it has had its
// chance to fray into a tendril (once), or is part of one.
// unzipped: you walked into it and it split into two halves (this one and a twin in a second strand), each keeping its side.
// gunDir: the direction (across the barrel) it went round the barrel when the gun swept into it (zero: not yet). Kept as
// a direction, so it stays on that side even when the swing reverses.
// clear: it has been outside your body since it was made (smoke you walk into, not smoke made within your reach).
// stir: how stirred up it is now (0 calm .. about 1), by your body, the gun and the whirlpools; spread: how much wider it has
// become from being stirred (1 = not at all), see Diffuse. kick: the burst it was thrown out with when you or the gun hit it
// (game units per second; it dies away), see WantBurst.
struct SmokePoint { NiPoint3 pos, vel, gunDir, kick; float born, life, r0, r1, grow, strength, opacity, fadeIn, fadeStart, fadeEnd, seed, tex, side, stir, spread; bool link, frayed, unzipped, clear; };
static const int ChainMax = 96;
static const char* const ChainNames[ChainCount] = { "heat smoke", "trail", "puff", "ejection" };
struct SmokeChain { SmokePoint p[ChainMax]; int count; float lastEmit; bool emitting; };
static SmokeChain Chains[ChainCount] = {};
// Where a chain emits this frame, and how.
struct SmokeSource {
	bool on;           // emitting
	NiPoint3 at;       // from here (world)
	float strength;    // density factor (the heat smoke and trail follow the barrel heat)
	float size;        // size factor: [Volume] fSizeScale (strands) or fBurstSizeScale (bursts; the ejection smoke also grows with the heat)
	NiPoint3 push;     // added to each new point's speed (the puff blows forward out of the barrel)
	NiPoint3 carry;    // each new point starts with this speed too: the gun's own movement (gas leaving a moving gun)
	NiPoint3 bore;     // the source is inside the barrel: new points slide out along this (to the muzzle) over their first 0.1 s
	float maxGap;      // a new point also once the source has moved this far from the last one (0 = by time only)
	float points;      // points over the smoke's life: strands 30, bursts 62 (closer together for short bursts)
	float drag;        // air drag per second
	int cluster;       // points to emit at once now (each puff / ejection burst starts with a cluster of them)
	bool drift;        // drifts toward the right of where you aim and away from you (the heat smoke and trail)
	bool frays;        // frays into tendrils where your body splits it (not the tendrils themselves)
	int zip;           // unzips into two arms as you walk into it: 1 the heat smoke, 2 the trail (their twin strands), 0 not
	bool haze;         // the gun-smoke haze (not hit by bullets: its blobs are too big to show a tunnel)
};
// A muzzle puff or ejection burst: emits until the clock passes `until` (at least one frame), from `node` plus
// `offset` (the node's own axes), following the node while it lasts.
struct SmokeBurst { float until; UInt8* node; NiPoint3 offset; float size; bool fresh; int pending; };
static SmokeBurst VolumeBursts[ChainCount] = {};   // used for ChainPuff and ChainEject
static float SmokeClock = 0.0f;
static UInt32 SmokeRandom = 12345u;
static float Random01() { SmokeRandom = SmokeRandom * 1664525u + 1013904223u; return (SmokeRandom >> 8) * (1.0f / 16777216.0f); }
// What NVR reads: one record of 16 floats per tube segment between neighbouring points (or per lone point): end A
// (x, y, z, radius), end B (x, y, z, radius), (density at A, density at B, age 0..1, how much of each rounded end is drawn
// (A 0..15 + 16 * B 0..15, see EndWeight) + 256 * seed), (noise coordinate at A and at B, age 0..1 at A and at B).
static const int RecordFloats = 16;
static float VolumePublished[((ChainCount + 2) * ChainMax + 8 * 4 + 40) * RecordFloats] = {};   // the chains, 2 twin arms, 8 tendrils of 4 points, 40 haze blobs
// The muzzle flash of the player's last shot (world position, time), for the light it throws on the smoke.
static NiPoint3 FlashWorld = {};
static double FlashAt = 0.0;   // PreciseSeconds (its fade lasts only a few frames)
static int VolumePublishedCount = 0;
static float VolumeParams[8] = {};
// The haze blobs, for NVR's haze pass (GunFX_GetSmokeHaze): 8 floats each, centre x, y, z (world), radius, optical depth
// straight through the middle, 3 spare.
static const int HazeFloats = 8;
static float SmokeHazePublished[40 * HazeFloats] = {};
static int SmokeHazePublishedCount = 0;
static float VolAfterRate = 0.0f;           // the after-fire trail's rate this frame (for the volumetric trail)

// The air the smoke drifts in (the weather's wind outdoors, still air indoors) and your body, which pushes smoke aside,
// drags it along as you move and thins it where it is stirred. Worked out once a frame by UpdateAirAndPlayer.
static NiPoint3 VolumeWind = {};          // game units per second
static bool VolumeIndoors = false;        // in an interior (no wind; smoke gathers under the ceiling)
// The gun as the smoke sees it: a capsule from the muzzle back along the barrel ([Volume] fGunLength, fGunRadius), and
// the direction you aim (flat) with its right-hand side, for the heat smoke and trail's drift. Set by the main loop.
static bool VolumeGunOn = false;
static NiPoint3 VolumeGunMuzzle = {}, VolumeGunBack = {}, VolumeAimForward = {}, VolumeAimRight = {};
// How far inside the barrel (behind the muzzle node) the heat smoke and trail start, so they flow out of its end.
static float VolumeMuzzleDepth = 0.0f;
// How the gun moves, smoothed over about 0.3 s: firing kicks the gun model back and up and springs it forward several
// times a second; those kicks cancel out here, so the smoke does not flick from side to side on every shot, while a
// real swing of the gun or your walking still shows.
static NiPoint3 VolumeGunVel = {};
static float VolumeWindKeep = 1.0f;       // this frame's share of horizontal motion not yet turned into the wind's
static bool VolumePlayerOn = false;
static NiPoint3 VolumePlayerFeet = {}, VolumePlayerVel = {};
static float VolumeMoving = 0.0f;         // how much you are moving, 0..1 (eased), for NVR (GunFX_GetMoving)

// The smoke point's radius now (as UpdateVolumeSmoke draws it).
static float PointRadius(const SmokePoint& s, float age) {
	float g = s.grow > 0.01f ? age / s.grow : 1.0f;
	g = g < 0.0f ? 0.0f : g > 1.0f ? 1.0f : g;
	g = 1.0f - (1.0f - g) * (1.0f - g);
	return (s.r0 + (s.r1 - s.r0) * g) * s.spread;
}

// The size your body and the gun treat a smoke point as: its radius, at most 6 units. Big, diffuse smoke (a haze blob, a
// billowed puff) is pushed by its middle instead of being kept a whole radius away, so you and the gun stay inside it.
static float ReachRadius(const SmokePoint& s, float age) {
	const float r = PointRadius(s, age);
	return r < 6.0f ? r : 6.0f;
}

static void UpdateAirAndPlayer(float dt) {
	UInt8* player = (UInt8*)Player();
	UInt8* cell = player ? *(UInt8**)(player + 0x40) : nullptr;
	// Interiors (TESObjectCELL flags at +0x24: 1 interior, 0x80 behaves like an exterior) have no wind.
#ifndef GUNFX_REPLAY
	const bool indoors = cell && (cell[0x24] & 0x01) && !(cell[0x24] & 0x80);
	UInt8* sky = *(UInt8**)0x011DEA20;                    // Sky: wind speed 0..1 at +0xCC, direction at +0xD0
#else
	(void)cell;
	const bool indoors = ReplayBody.indoors;
	UInt8* sky = nullptr;                                 // the replay rig has no weather: still air
#endif
	VolumeIndoors = indoors;
	float speed = 0.0f, raw = 0.0f;
	if (sky && !indoors && Settings.volumeWind > 0.0f) {
		speed = *(float*)(sky + 0xCC);
		raw = *(float*)(sky + 0xD0);
		if (!(speed >= 0.0f && speed <= 4.0f)) speed = 0.0f;   // not a sane value
		if (!(raw > -1000.0f && raw < 1000.0f)) raw = 0.0f;
	}
	// The direction may be in degrees or radians: above a full turn in radians it is taken as degrees. The heading the
	// wind blows toward, 0 = north (+Y), clockwise; [Volume] fWindDirectionOffset turns it (180 = the other way).
	const float radians = (fabsf(raw) > 6.3f ? raw * 0.01745329f : raw) + Settings.volumeWindOffset * 0.01745329f;
	VolumeWind = NiPoint3{ sinf(radians) * speed * Settings.volumeWind, cosf(radians) * speed * Settings.volumeWind, 0.0f };
	const float fadeIn = speed * 4.0f < 1.0f ? speed * 4.0f : 1.0f;
	VolumeWindKeep = expf(-Settings.volumeWindPickup * fadeIn * dt);
	static int logged = 0;
	static float loggedSpeed = -1.0f, loggedRaw = 0.0f;
	static bool loggedIndoors = false;
	if (logged < 10 && (fabsf(speed - loggedSpeed) > 0.1f || fabsf(raw - loggedRaw) > 0.5f || indoors != loggedIndoors)) {
		logged++;
		loggedSpeed = speed; loggedRaw = raw; loggedIndoors = indoors;
		DebugLine("volumetric smoke wind: %s, speed %.2f, direction %.2f (raw) -> drift (%.1f, %.1f) units/s", indoors ? "indoors (none)" : "outdoors",
			speed, raw, VolumeWind.x, VolumeWind.y);
	}
	// Your body: where you stand and how fast you move (smoothed; a jump of over 25 units per frame is a teleport).
	VolumePlayerOn = false;
	static NiPoint3 last = {};
	static bool haveLast = false;
#ifndef GUNFX_REPLAY
	if (!player || !Settings.volumePlayer) { haveLast = false; VolumeMoving = 0.0f; return; }
	const NiPoint3 feet = *(NiPoint3*)(player + 0x30);
#else
	if (!ReplayBody.on || !Settings.volumePlayer) { haveLast = false; VolumeMoving = 0.0f; return; }
	const NiPoint3 feet = ReplayBody.feet;
#endif
	if (haveLast && dt > 0.0001f) {
		NiPoint3 v = { (feet.x - last.x) / dt, (feet.y - last.y) / dt, (feet.z - last.z) / dt };
		if (v.x * v.x + v.y * v.y + v.z * v.z > 1500.0f * 1500.0f) v = NiPoint3{};
		const float k = 1.0f - expf(-10.0f * dt);
		VolumePlayerVel.x += (v.x - VolumePlayerVel.x) * k;
		VolumePlayerVel.y += (v.y - VolumePlayerVel.y) * k;
		VolumePlayerVel.z += (v.z - VolumePlayerVel.z) * k;
		// How much you are moving (walking or running: 20..80 units per second, as BodyShare), eased: up at once, down over
		// about half a second after you stop. NVR's shader then leaves your own smoke close to you uncut (GunFX_GetMoving).
		const float pace = sqrtf(VolumePlayerVel.x * VolumePlayerVel.x + VolumePlayerVel.y * VolumePlayerVel.y);
		const float target = pace < 20.0f ? 0.0f : pace > 80.0f ? 1.0f : (pace - 20.0f) / 60.0f;
		VolumeMoving += (target - VolumeMoving) * (1.0f - expf((target > VolumeMoving ? -12.0f : -2.0f) * dt));
	}
	last = feet;
	haveLast = true;
	VolumePlayerFeet = feet;
	VolumePlayerOn = true;
}

// ---- Whirlpools behind the gun and your body ---------------------------------------------------------------------
// A pole moved through air leaves a pair of small whirlpools behind it, spinning opposite ways, that keep stirring for a
// moment after it has passed. While the gun sweeps across smoke (faster than 40 units per second across the barrel) it
// sheds such a pair every 0.08 s just behind the barrel's middle ([Volume] fGunSwirl); while you walk through smoke your
// body sheds upright ones every 0.12 s behind you ([Volume] fBodySwirl, lasting fSwirlLife). Each whirls the smoke around
// it (a line vortex along the barrel or your body, softened in its core) and fades out; they also carry each other
// along, so a wake rolls up into eddies by itself. At most 24 at once.
struct Vortex { NiPoint3 pos, axis; float strength, born, life, core, half; };
static const int VortexMax = 24;
static Vortex Vortices[VortexMax] = {};
static int VortexCount = 0;
static float LastShed = -10.0f, LastBodyShed = -10.0f;

// The swirl the whirlpools give a point (game units per second); `skip`: a whirlpool to leave out (its own).
static NiPoint3 VortexFlow(const NiPoint3& p, int skip = -1) {
	NiPoint3 v = {};
	for (int i = 0; i < VortexCount; i++) {
		if (i == skip) continue;
		const Vortex& w = Vortices[i];
		float fade = 1.0f - (SmokeClock - w.born) / w.life;
		if (fade <= 0.0f) continue;
		fade = fade * fade * (3.0f - 2.0f * fade);                                // eases out
		const NiPoint3 r = { p.x - w.pos.x, p.y - w.pos.y, p.z - w.pos.z };
		const float ra = r.x * w.axis.x + r.y * w.axis.y + r.z * w.axis.z;
		const float half = w.half, endFade = 0.5f * w.core + 4.0f;
		if (fabsf(ra) > half + endFade) continue;                                 // beside its length only
		const NiPoint3 rp = { r.x - w.axis.x * ra, r.y - w.axis.y * ra, r.z - w.axis.z * ra };
		const float core = w.core;
		const float d2 = rp.x * rp.x + rp.y * rp.y + rp.z * rp.z;
		if (d2 > 36.0f * core * core) continue;                                   // six core radii around it
		const float ends = fabsf(ra) > half ? 1.0f - (fabsf(ra) - half) / endFade : 1.0f;
		const float k = w.strength * fade * ends / (6.2831853f * (d2 + core * core));
		v.x += (w.axis.y * rp.z - w.axis.z * rp.y) * k;                             // axis x r: around the axis
		v.y += (w.axis.z * rp.x - w.axis.x * rp.z) * k;
		v.z += (w.axis.x * rp.y - w.axis.y * rp.x) * k;
	}
	return v;
}

// Sheds pairs behind the gun and your body when they move through smoke; moves every whirlpool with the wind and the
// others' swirl, and drops faded ones.
static bool SmokeNearGun();
static bool SmokeNearBody();
static void ShedBodyVortices();
static void ShedVortices(float dt) {
	static NiPoint3 moves[VortexMax];
	for (int i = 0; i < VortexCount; i++) moves[i] = VortexFlow(Vortices[i].pos, i);   // carried by the others
	int keep = 0;
	for (int i = 0; i < VortexCount; i++) {
		Vortex w = Vortices[i];
		if (SmokeClock - w.born >= w.life) continue;
		w.pos.x += (VolumeWind.x + moves[i].x) * dt; w.pos.y += (VolumeWind.y + moves[i].y) * dt; w.pos.z += moves[i].z * dt;
		Vortices[keep++] = w;
	}
	VortexCount = keep;
	ShedBodyVortices();
	if (!VolumeGunOn || !Settings.volumeWrapGun || Settings.volumeGunSwirl <= 0.0f) return;
	const NiPoint3& b = VolumeGunBack;
	NiPoint3 U = { VolumeWind.x - VolumeGunVel.x, VolumeWind.y - VolumeGunVel.y, -VolumeGunVel.z };   // the air past the gun
	const float ub = U.x * b.x + U.y * b.y + U.z * b.z;
	U = NiPoint3{ U.x - b.x * ub, U.y - b.y * ub, U.z - b.z * ub };                                    // across the barrel
	const float u = sqrtf(U.x * U.x + U.y * U.y + U.z * U.z);
	if (u < 40.0f || SmokeClock - LastShed < 0.08f || VortexCount + 2 > VortexMax || !SmokeNearGun()) return;
	LastShed = SmokeClock;
	const NiPoint3 uh = { U.x / u, U.y / u, U.z / u };
	NiPoint3 side = { b.y * uh.z - b.z * uh.y, b.z * uh.x - b.x * uh.z, b.x * uh.y - b.y * uh.x };      // across both
	const float Rg = Settings.volumeGunRadius, half = 0.5f * Settings.volumeGunLength;
	const NiPoint3 behind = { VolumeGunMuzzle.x + b.x * half + uh.x * Rg * 2.0f, VolumeGunMuzzle.y + b.y * half + uh.y * Rg * 2.0f,
		VolumeGunMuzzle.z + b.z * half + uh.z * Rg * 2.0f };
	// Spinning so that between them the air follows the gun (as the wake behind a moving pole does).
	const float strength = Settings.volumeGunSwirl * u * Rg * 2.0f;
	for (int e = -1; e <= 1; e += 2) {
		Vortex& w = Vortices[VortexCount++];
		w.pos = NiPoint3{ behind.x + side.x * Rg * 1.2f * e, behind.y + side.y * Rg * 1.2f * e, behind.z + side.z * Rg * 1.2f * e };
		w.axis = b;
		w.strength = -strength * e;
		w.born = SmokeClock;
		w.life = 0.9f;
		w.core = Rg * 1.5f;
		w.half = half;
	}
}

// Your body: as you walk through smoke (faster than 40 units per second), a pair of upright whirlpools behind you every
// 0.12 s, spinning so that between them the air follows you; your wake rolls up into eddies as they carry each other.
static void ShedBodyVortices() {
	if (!VolumePlayerOn || Settings.volumeBodySwirl <= 0.0f) return;
	const NiPoint3 U = { VolumeWind.x - VolumePlayerVel.x, VolumeWind.y - VolumePlayerVel.y, 0.0f };   // the air past you
	const float u = sqrtf(U.x * U.x + U.y * U.y);
	if (u < 40.0f || SmokeClock - LastBodyShed < 0.12f || VortexCount + 2 > VortexMax || !SmokeNearBody()) return;
	LastBodyShed = SmokeClock;
	const NiPoint3 uh = { U.x / u, U.y / u, 0.0f };
	const NiPoint3 side = { -uh.y, uh.x, 0.0f };                                                  // up x uh
	const float R = Settings.volumePlayerRadius, H = Settings.volumePlayerHeight;
	const NiPoint3 behind = { VolumePlayerFeet.x + uh.x * R * 1.1f, VolumePlayerFeet.y + uh.y * R * 1.1f, VolumePlayerFeet.z + 0.5f * H };
	const float strength = Settings.volumeBodySwirl * u * R;
	for (int e = -1; e <= 1; e += 2) {
		Vortex& w = Vortices[VortexCount++];
		w.pos = NiPoint3{ behind.x + side.x * R * 0.8f * e, behind.y + side.y * R * 0.8f * e, behind.z };
		w.axis = NiPoint3{ 0.0f, 0.0f, 1.0f };
		w.strength = -strength * e;
		w.born = SmokeClock;
		w.life = Settings.volumeSwirlLife;
		w.core = R * 0.5f;
		w.half = 0.5f * H;
	}
}

// Whether a smoke is drawn volumetric now: [Volume] bEnabled, the menu switch and that smoke's own [Volume] switch.
static bool VolumeWanted(int chain) { return Settings.volume && Switch(8) && Settings.volumeKinds[chain]; }

static int HazeShots = 0;                     // shots since the haze was last updated
// Starts (or extends) the volumetric puff / ejection burst from a node.
static void StartVolumeBurst(int chain, UInt8* node, const NiPoint3& offset, float seconds, float size) {
	SmokeBurst& b = VolumeBursts[chain];
	if (b.node != node) { NodeAddRef(node); NodeRelease(b.node); b.node = node; }
	b.offset = offset;
	b.size = size;
	const float until = SmokeClock + seconds;
	if (until > b.until) b.until = until;
	b.fresh = true;
	b.pending += Settings.volumeBurstPoints;   // every shot / casing adds its own cluster
	if (chain == ChainPuff) HazeShots++;       // and every shot leaves some haze (UpdateHaze)
}

static void EmitSmoke(SmokeChain& c, const SmokeSource& src, const VolLook& v) {
	if (c.count == ChainMax) {                 // full: drop the oldest point
		memmove(&c.p[0], &c.p[1], sizeof(SmokePoint) * (ChainMax - 1));
		c.count--;
		c.p[0].link = false;
	}
	SmokePoint& s = c.p[c.count];
	const float size = v.size * src.size;
	const float life = v.life + v.lifeVar * (Random01() * 2.0f - 1.0f);
	const float speed = v.speed + v.speedVar * (Random01() * 2.0f - 1.0f);
	// Upwards, within fSpread (radians) of straight up, like the sprite emitters.
	const float tilt = v.spread * sqrtf(Random01()), turn = 6.2831853f * Random01();
	s.pos = src.at;
	s.vel = NiPoint3{ speed * sinf(tilt) * cosf(turn) + src.carry.x, speed * sinf(tilt) * sinf(turn) + src.carry.y, speed * cosf(tilt) + src.carry.z };
	// Plus the source's push (the puff's jet out of the barrel): within fSpread of its direction, each point at its own
	// share of its speed, so the jet spreads into an uneven cone instead of one round ball.
	const float pushSpeed = sqrtf(src.push.x * src.push.x + src.push.y * src.push.y + src.push.z * src.push.z);
	if (pushSpeed > 0.01f) {
		const NiPoint3 d = { src.push.x / pushSpeed, src.push.y / pushSpeed, src.push.z / pushSpeed };
		NiPoint3 u = fabsf(d.z) < 0.9f ? NiPoint3{ -d.y, d.x, 0.0f } : NiPoint3{ 0.0f, -d.z, d.y };   // across the jet
		const float ul = sqrtf(u.x * u.x + u.y * u.y + u.z * u.z);
		u = NiPoint3{ u.x / ul, u.y / ul, u.z / ul };
		const NiPoint3 w = { d.y * u.z - d.z * u.y, d.z * u.x - d.x * u.z, d.x * u.y - d.y * u.x };
		const float jt = v.spread * sqrtf(Random01()), jr = 6.2831853f * Random01();
		const float share = pushSpeed * (0.35f + 0.8f * Random01());
		const float c = cosf(jt), sn = sinf(jt) * cosf(jr), sw = sinf(jt) * sinf(jr);
		s.vel.x += share * (c * d.x + sn * u.x + sw * w.x);
		s.vel.y += share * (c * d.y + sn * u.y + sw * w.y);
		s.vel.z += share * (c * d.z + sn * u.z + sw * w.z);
	}
	s.born = SmokeClock;
	s.life = life > 0.2f ? life : 0.2f;
	s.r0 = size * v.startSize;
	s.r1 = size * Settings.volumeExpand * (1.0f + v.sizeVar * (Random01() - 0.5f) * 0.5f);   // [Volume] fExpand: how big it ends up
	s.grow = v.grow;
	s.strength = src.strength;
	s.opacity = v.opacity * Settings.volumeDensity;
	s.fadeIn = v.fadeIn;
	s.fadeStart = v.fadeStart;
	s.fadeEnd = v.fadeEnd;
	s.seed = Random01() * 100.0f;
	// The point's noise coordinate along the smoke: by birth time, so neighbours are close and the detail flows along
	// a strand, plus a little of its own, so a burst's points differ.
	s.tex = SmokeClock * 1.5f + (s.seed - floorf(s.seed)) * 0.5f;
	s.link = c.emitting && c.count > 0;
	s.side = 0.0f;
	s.gunDir = NiPoint3{};
	s.frayed = false;
	s.unzipped = false;
	s.clear = false;
	s.stir = 0.0f;
	s.spread = 1.0f;
	s.kick = NiPoint3{};
	c.count++;
	c.lastEmit = SmokeClock;
}

// ---- Tendrils: thin, short-lived child strands --------------------------------------------------------------------
// Where a strand bends sharply (a quick change of direction of the gun), the smoke at the bend keeps going a little
// (momentum) while the strand turns, so a thin filament peels off outward; where your body splits a strand, its torn ends
// fray the same way. A tendril is 4 points leaving the parent point at spreading speeds (a thin line growing out of it),
// a share of its thickness ([Volume] fTendrilSize), fainter, about half as long-lived and curling more. Each strand point
// has one chance ([Volume] fTendrils) and at most 8 tendrils exist at once, so the cost stays tiny. They drift, flow
// around you and the gun, and are drawn like the rest of the smoke.
static const int TendrilMax = 8, TendrilPoints = 4;
static SmokeChain Tendrils[TendrilMax] = {};
static VolLook TendrilLook[TendrilMax] = {};
static bool TendrilDrift[TendrilMax] = {};
static void SpawnTendril(const SmokePoint& parent, float parentRadius, NiPoint3 dir, const VolLook& look, bool drift) {
	if (Settings.volumeTendrils <= 0.0f) return;
	int slot = -1;
	for (int t = 0; t < TendrilMax && slot < 0; t++) if (!Tendrils[t].count) slot = t;
	if (slot < 0) return;                                 // all busy: skip rather than cut one short
	const float dl = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
	if (dl < 0.001f) return;
	dir = NiPoint3{ dir.x / dl, dir.y / dl, dir.z / dl };
	SmokeChain& c = Tendrils[slot];
	c.count = 0;
	c.emitting = false;
	c.lastEmit = SmokeClock;
	VolLook& v = TendrilLook[slot];
	v = look;
	v.curl *= 2.0f;
	v.life *= 0.45f;
	v.lifeVar *= 0.45f;
	TendrilDrift[slot] = drift;
	const float speed = Settings.volumeTendrilSpeed;
	const float r0 = parentRadius * Settings.volumeTendrilSize;
	for (int k = 0; k < TendrilPoints; k++) {
		SmokePoint& s = c.p[c.count++];
		s = parent;
		// Each point a little further out (so they do not pile up into a bright dot) and leaving at its own speed and a
		// slightly different angle (so the tendril curls instead of shooting out straight).
		s.pos.x += dir.x * r0 * 0.6f * k; s.pos.y += dir.y * r0 * 0.6f * k; s.pos.z += dir.z * r0 * 0.6f * k;
		const float share = speed * (0.3f + 0.35f * k);
		const float jx = (Random01() - 0.5f) * 0.9f, jy = (Random01() - 0.5f) * 0.9f, jz = (Random01() - 0.3f) * 0.9f;
		s.vel.x = parent.vel.x + (dir.x + jx) * share;
		s.vel.y = parent.vel.y + (dir.y + jy) * share;
		s.vel.z = parent.vel.z + (dir.z + jz) * share;
		s.born = SmokeClock;
		s.life = v.life * (0.8f + 0.4f * Random01());
		if (s.life < 0.3f) s.life = 0.3f;
		s.r0 = r0 * (k ? 0.85f : 1.0f);
		s.r1 = s.r0 * 2.5f;                                // widens as it goes, dissolving
		s.grow = s.life;
		s.opacity = parent.opacity * 0.35f;                 // thin and faint, not a solid line
		s.stir = parent.stir * 0.5f;
		s.spread = 1.0f;
		s.kick = NiPoint3{ parent.kick.x * 0.5f, parent.kick.y * 0.5f, parent.kick.z * 0.5f };
		s.fadeIn = 0.15f; s.fadeStart = 0.2f; s.fadeEnd = 0.9f;   // eases in, then fades
		s.seed = Random01() * 100.0f;
		s.tex = parent.tex + 0.3f * k;
		s.side = 0.0f;
		s.gunDir = NiPoint3{};
		s.link = k > 0;
		s.frayed = true;                                  // tendrils do not fray again
		s.unzipped = false;
	}
}

// Your body as a hard rule, after everything else has moved: no smoke point stays inside it (an upright capsule from
// your feet to fPlayerHeight, fPlayerRadius wide, plus a little of the smoke's own radius): a point inside moves straight
// out (the air flowing round you has already carried smoke ahead of you to your sides, so it is not shoved ahead and
// carried along). A strand whose piece between two points would pass through you is split there, as walking through smoke splits
// it, so no smoke tube runs through you. Smoke younger than 0.1 s is left alone, and the rule grows in until 0.6 s,
// easing points out, so it still leaves the barrel and the ejection port cleanly.
// ---- Unzipping: the heat smoke and trail split around you as you walk into them --------------------------------------
// Walking along your own trail, it lies straight along your path; pushed aside bit by bit, its points would land on
// either side of you in turn (a zigzag, chopped up). Instead it unzips like a bow wave: just ahead of you each point splits
// into two halves of half the opacity, this one and a twin in a second strand. The original goes to one side of you (your
// gun's, unless the strand is clearly on the other side already), the twins to the other, and each keeps its side, so the
// trail opens into two arms that pass either side of you and trail behind in a V. The split happens where the air ahead
// of you starts to part (three times your reach ahead), before the air carries the strand aside, so each half goes its
// own way round you. [Volume] bUnzip turns it off.
// Only a trail already hanging in the air (older than this, seconds) unzips. The gun's own fresh smoke, which you run into
// as you move forward (the muzzle is ahead of you), flows back past the gun's side as one stream instead: split, one arm
// had to cross the middle of your view and the two arms kept restarting (a flicker).
static const float UnzipAge = 1.0f;
static SmokeChain Twins[2] = {};                  // the second arms of the heat smoke and the trail
static float ZipSide[2] = { 1.0f, 1.0f };         // the side the original strand goes to in this walk-through
static float ZipLast[2] = { -10.0f, -10.0f };     // when a point last unzipped (a new walk-through after 0.5 s)
static float TwinParent[2] = { -1.0f, -1.0f };    // birth time of the last twin's original (to link the next twin to it)

static void Unzip(SmokeChain& c, int i, int zip, float across, float here, float rightX, float rightY, float rightZ = 0.0f) {
	SmokePoint& s = c.p[i];
	const int z = zip - 1;
	if (SmokeClock - ZipLast[z] > 0.5f) ZipSide[z] = fabsf(across) > 0.3f * here ? (across > 0.0f ? 1.0f : -1.0f) : 1.0f;
	ZipLast[z] = SmokeClock;
	s.unzipped = true;
	s.side = ZipSide[z];
	s.opacity *= 0.5f;
	if (s.stir < 0.5f) s.stir = 0.5f;                  // splitting stirs it up (the twin copies this)
	// The two halves fan out at once, each toward its side ([Volume] fFanSpeed), so the arms open right where it splits
	// instead of lying on top of each other until you reach them.
	const float fan = Settings.volumeFanSpeed * ZipSide[z];
	s.vel.x += rightX * fan;
	s.vel.y += rightY * fan;
	s.vel.z += rightZ * fan;
	SmokeChain& t = Twins[z];
	if (t.count == ChainMax) {                    // full: drop the oldest twin
		memmove(&t.p[0], &t.p[1], sizeof(SmokePoint) * (ChainMax - 1));
		t.count--;
		t.p[0].link = false;
	}
	SmokePoint& w = t.p[t.count];
	w = s;
	w.side = -ZipSide[z];
	w.vel.x -= rightX * fan * 2.0f;                  // the twin fans out the other way (it copied this one's push)
	w.vel.y -= rightY * fan * 2.0f;
	w.vel.z -= rightZ * fan * 2.0f;
	// Linked to the previous twin when that one's original is this point's neighbour in the strand (and linked to it).
	const bool afterOlder = i > 0 && s.link && c.p[i - 1].born == TwinParent[z];
	const bool afterNewer = i + 1 < c.count && c.p[i + 1].link && c.p[i + 1].born == TwinParent[z];
	w.link = t.count > 0 && (afterOlder || afterNewer);
	t.count++;
	TwinParent[z] = s.born;
}

// The side of you a point goes to: the side it is clearly on (`clear`); near your middle, the side it chose before, or
// else the side its neighbours in the strand chose (so a strand goes round one way together instead of its points
// scattering left and right, which drew pieces straight across your view), or else the side it leans to.
static float ChooseSide(const SmokeChain& c, int i, float across, bool clear) {
	const SmokePoint& s = c.p[i];
	if (clear) return across > 0.0f ? 1.0f : -1.0f;
	if (s.side != 0.0f) return s.side;
	if (i > 0 && s.link && c.p[i - 1].side != 0.0f) return c.p[i - 1].side;
	if (i + 1 < c.count && c.p[i + 1].link && c.p[i + 1].side != 0.0f) return c.p[i + 1].side;
	return across < 0.0f ? -1.0f : 1.0f;
}

static void SpawnTendril(const SmokePoint& parent, float parentRadius, NiPoint3 dir, const VolLook& look, bool drift);
// How much of your body's flow and keep-out rule a point gets while you move. Smoke hugging the gun is carried by the air
// round the gun, not by your body's bow wave: the gun and your arms are out in front of you, so smoke leaving the muzzle
// streams back along the gun to your shoulder and face (as smoke from a cigarette in your hand does when you walk). Your
// body's model is an upright pole 30 units round, and its bow wave reached the muzzle 74 units ahead: it threw the fresh
// trail sideways at about 60 units per second, a streak angling off the muzzle. Within 4 units of the gun's surface none,
// full from 16 units; standing still (under 20 units per second, all of it from 80) everything is as before.
// UpdateChain fills BodyShareOf for the chain it updates; its KeepOutOfBody reads it.
static float BodyShareOf[ChainMax];
static float BodyShare(const NiPoint3& p) {
	if (!ShapeOn || !VolumeGunOn || !VolumePlayerOn) return 1.0f;
#ifdef GUNFX_REPLAY
	if (getenv("OLD_BODY")) return 1.0f;           // the replay rig's before/after switch
#endif
	const float speed = sqrtf(VolumePlayerVel.x * VolumePlayerVel.x + VolumePlayerVel.y * VolumePlayerVel.y);
	const float moving = speed < 20.0f ? 0.0f : speed > 80.0f ? 1.0f : (speed - 20.0f) / 60.0f;
	if (moving <= 0.0f) return 1.0f;
	NiPoint3 n;
	const float d = GunSurface(p, &n);
	const float nearGun = d <= 4.0f ? 1.0f : d >= 16.0f ? 0.0f : (16.0f - d) / 12.0f;
	return 1.0f - moving * nearGun;
}
static void KeepOutOfBody(SmokeChain& c, const VolLook* frayLook, bool drift, int zip, float dt) {
	if (!VolumePlayerOn) return;
	const NiPoint3& feet = VolumePlayerFeet;
	const float R = Settings.volumePlayerRadius;
	const float bottom = feet.z, top = feet.z + Settings.volumePlayerHeight;
	// Straight out while you stand, sliding aside while you walk: gradually between 20 and 80 units per second (no
	// switch to flip back and forth on), and each point keeps the side of you it first went to.
	const float speed = sqrtf(VolumePlayerVel.x * VolumePlayerVel.x + VolumePlayerVel.y * VolumePlayerVel.y);
	const float flow = speed < 20.0f ? 0.0f : speed > 80.0f ? 1.0f : (speed - 20.0f) / 60.0f;
	const float hx = speed > 1.0f ? VolumePlayerVel.x / speed : 0.0f, hy = speed > 1.0f ? VolumePlayerVel.y / speed : 0.0f;   // heading
	const float rx = hy, ry = -hx;                                                                                          // its right
	// Smoke born inside you (the ejection port sits within your reach) grows into the rule over 0.5 s and, while you stand,
	// eases out over a few frames, so it drifts clear instead of shooting out (walking, the air flowing round you carries
	// it aside).
	const float span = 0.5f;
	const float easeTime = 0.05f * (1.0f - flow);
	const float ease = easeTime > 0.001f ? 1.0f - expf(-dt / easeTime) : 1.0f;
	// But never faster than you move plus 120 units a second. Running, the ease is instant, and smoke the air had not yet
	// carried aside was snapped 4-15 units to your edge in one frame: it popped into view (inside you, the shader's near
	// fade hides it). Now it slides out, and what is held back inside you thins out, stirred into the air you push through.
	const float maxMove = (speed + 120.0f) * dt;
	for (int i = 0; i < c.count; i++) {
		SmokePoint& s = c.p[i];
		const float age = SmokeClock - s.born;
		const float full = R + 0.7f * ReachRadius(s, age);
		// Below the feet or above the head the capsule rounds off.
		const float dz = s.pos.z < bottom ? bottom - s.pos.z : s.pos.z > top ? s.pos.z - top : 0.0f;
		if (dz >= full) { s.clear = true; continue; }
		const float ox = s.pos.x - feet.x, oy = s.pos.y - feet.y;
		const float dist = sqrtf(ox * ox + oy * oy);
		const float hereFull = sqrtf(full * full - dz * dz);
		if (dist >= hereFull) s.clear = true;                           // it has been outside you
		const float share = BodyShareOf[i];                             // (along the gun while you move: less or none)
		// Only smoke made inside your reach grows into the rule; smoke you walk into meets all of it at once.
		if (!s.clear && age <= 0.1f) continue;
		float grown = s.clear ? 1.0f : (age - 0.1f) / span;
		grown = grown > 1.0f ? 1.0f : grown;
		const float reach = full * grown;
		const float ahead = ox * hx + oy * hy;
		const float across = ox * rx + oy * ry;
		// Walking into it: unzip where the air ahead of you starts to part (three times your reach ahead).
		if (zip && Settings.volumeUnzip && age >= UnzipAge && flow > 0.5f && !s.unzipped && ahead > 0.0f && dist < hereFull * 3.0f && share > 0.5f)
			Unzip(c, i, zip, across, hereFull, rx, ry);
		if (dz >= reach) continue;
		const float here = sqrtf(reach * reach - dz * dz);   // the body's radius at this height
		if (dist >= here) continue;
		if (!s.unzipped) s.side = ChooseSide(c, i, across, fabsf(across) > 0.25f * here);
		// Straight out (right on your middle, to its side): the shortest way, so a point only just inside moves only a
		// little. Sliding aside as you walk is the air's job (PoleFlow, before the smoke reaches you); a sideways shove here
		// threw smoke that had just touched you 10 to 20 units in one frame.
		float nx, ny;
		if (dist > 0.01f) { nx = feet.x + ox / dist * here; ny = feet.y + oy / dist * here; }
		else { nx = feet.x + (speed > 1.0f ? rx : 1.0f) * s.side * here; ny = feet.y + (speed > 1.0f ? ry : 0.0f) * s.side * here; }
		float mx = (nx - s.pos.x) * ease * share, my = (ny - s.pos.y) * ease * share;
		const float want = sqrtf(mx * mx + my * my);
		if (want > maxMove && want > 0.0001f) {
			const float keep = maxMove / want;
			mx *= keep;
			my *= keep;
			s.opacity *= expf(-(1.0f - keep) * dt / 0.1f);
		}
		s.pos.x += mx;
		s.pos.y += my;
	}
	// Split the strand where a piece would pass through you (its closest point to your axis, seen from above, within
	// fPlayerRadius at a height you occupy).
	for (int i = 1; i < c.count; i++) {
		if (!c.p[i].link) continue;
		if (c.p[i].unzipped && c.p[i - 1].unzipped && c.p[i].side == c.p[i - 1].side) continue;   // an arm on one side of you
		if (BodyShareOf[i] < 0.5f || BodyShareOf[i - 1] < 0.5f) continue;   // streaming along the gun as you move: not cut by you
		if ((!c.p[i].clear && SmokeClock - c.p[i].born < 0.1f + span) || (!c.p[i - 1].clear && SmokeClock - c.p[i - 1].born < 0.1f + span))
			continue;                                                    // made within your reach, still leaving the gun or the port
		const NiPoint3& a = c.p[i - 1].pos;
		const NiPoint3& b = c.p[i].pos;
		const float ex = b.x - a.x, ey = b.y - a.y;
		const float len2 = ex * ex + ey * ey;
		float u = len2 > 0.0001f ? ((feet.x - a.x) * ex + (feet.y - a.y) * ey) / len2 : 0.0f;
		u = u < 0.0f ? 0.0f : u > 1.0f ? 1.0f : u;
		const float px = a.x + ex * u - feet.x, py = a.y + ey * u - feet.y, pz = a.z + (b.z - a.z) * u;
		if (px * px + py * py < R * R && pz > bottom - R && pz < top + R) {
			c.p[i].link = false;
			// The torn ends fray into tendrils, away from you.
			if (frayLook) for (int e = i - 1; e <= i; e++) {
				SmokePoint& t = c.p[e];
				if (t.frayed) continue;
				t.frayed = true;
				if (Random01() > Settings.volumeTendrils) continue;
				SpawnTendril(t, PointRadius(t, SmokeClock - t.born), NiPoint3{ t.pos.x - feet.x, t.pos.y - feet.y, 0.0f }, *frayLook, drift);
			}
		}
	}
}

// The gun as a hard rule, after everything else has moved: no smoke point stays inside the barrel (a capsule from the
// muzzle back fGunLength, fGunRadius thick plus a little of the smoke's own radius). Moving into the heat smoke or trail
// (walking forward, or swinging the gun through it), the gun unzips it like your body does: one arm along each side of
// the barrel, and they keep those sides when they reach you. A point is pushed straight out from the barrel; near its
// middle it leans toward its own way out (the way it went round before, its side, or over the top), smoothly, so the
// points of a strand all go the same way instead of scattering at random angles. A strand whose piece would pass through
// the barrel is split there (unless both ends went the same way), its torn ends fraying. Smoke younger than 0.1 s at the
// muzzle's tip is left alone, so it still leaves the muzzle cleanly.
// While the gun is (nearly) still, the rule is gentle: young smoke grows into it over its first half second and is not
// split, and points ease out over a few frames. Firing kicks the gun model back, up and forward into the smoke it has just
// made, and smoke rising off a barrel pointed down runs up along it; pushed out at once, those points flicked out at
// random angles (and frayed into tendrils). Moving the gun (faster than 25 units per second across the barrel or walking
// forward, fully at 50) makes it firm again, so a swing or a walk never passes through smoke: everything the gun has
// moved past is pushed out fully, at once while it sweeps across, eased over a few frames while you walk forward (the
// barrel then slides along its own length, and a one-frame shove snapped fresh smoke sideways).
static float SegmentDistance2(const NiPoint3& p0, const NiPoint3& p1, const NiPoint3& q0, const NiPoint3& q1) {
	const NiPoint3 d1 = { p1.x - p0.x, p1.y - p0.y, p1.z - p0.z }, d2 = { q1.x - q0.x, q1.y - q0.y, q1.z - q0.z };
	const NiPoint3 r = { p0.x - q0.x, p0.y - q0.y, p0.z - q0.z };
	const float a = d1.x * d1.x + d1.y * d1.y + d1.z * d1.z, e = d2.x * d2.x + d2.y * d2.y + d2.z * d2.z;
	const float f = d2.x * r.x + d2.y * r.y + d2.z * r.z;
	float sp = 0.0f, tq = 0.0f;
	if (a <= 0.0001f && e <= 0.0001f) { sp = tq = 0.0f; }
	else if (a <= 0.0001f) { tq = f / e; tq = tq < 0.0f ? 0.0f : tq > 1.0f ? 1.0f : tq; }
	else {
		const float c = d1.x * r.x + d1.y * r.y + d1.z * r.z;
		if (e <= 0.0001f) { sp = -c / a; sp = sp < 0.0f ? 0.0f : sp > 1.0f ? 1.0f : sp; }
		else {
			const float bb = d1.x * d2.x + d1.y * d2.y + d1.z * d2.z, den = a * e - bb * bb;
			sp = den > 0.0001f ? (bb * f - c * e) / den : 0.0f;
			sp = sp < 0.0f ? 0.0f : sp > 1.0f ? 1.0f : sp;
			tq = (bb * sp + f) / e;
			if (tq < 0.0f) { tq = 0.0f; sp = -c / a; sp = sp < 0.0f ? 0.0f : sp > 1.0f ? 1.0f : sp; }
			else if (tq > 1.0f) { tq = 1.0f; sp = (bb - c) / a; sp = sp < 0.0f ? 0.0f : sp > 1.0f ? 1.0f : sp; }
		}
	}
	const NiPoint3 cp = { p0.x + d1.x * sp - (q0.x + d2.x * tq), p0.y + d1.y * sp - (q0.y + d2.y * tq), p0.z + d1.z * sp - (q0.z + d2.z * tq) };
	return cp.x * cp.x + cp.y * cp.y + cp.z * cp.z;
}

// How fast the gun really moves into the smoke (game units per second): across the barrel (swinging it, strafing) or
// forward along it (walking forward); both smoothed, so firing's kicks back and forth along the barrel do not count.
// `moving`: 0 while it is (nearly) still, 1 from 50 units per second, gradually from 25; `sweeping`: the same for the
// movement across the barrel only.
static float GunSpeed(float* moving, float* sweeping = nullptr) {
	*moving = 0.0f;
	if (sweeping) *sweeping = 0.0f;
	if (!VolumeGunOn) return 0.0f;
	const NiPoint3& b = VolumeGunBack;
	const float gb = VolumeGunVel.x * b.x + VolumeGunVel.y * b.y + VolumeGunVel.z * b.z;
	const NiPoint3 gunCross = { VolumeGunVel.x - b.x * gb, VolumeGunVel.y - b.y * gb, VolumeGunVel.z - b.z * gb };
	const float crossSpeed = sqrtf(gunCross.x * gunCross.x + gunCross.y * gunCross.y + gunCross.z * gunCross.z);
	const float forward = VolumePlayerOn ? -(VolumePlayerVel.x * b.x + VolumePlayerVel.y * b.y + VolumePlayerVel.z * b.z) : 0.0f;
	const float speed = crossSpeed > forward ? crossSpeed : forward;
	const float m = (speed - 25.0f) / 25.0f, s = (crossSpeed - 25.0f) / 25.0f;
	*moving = m < 0.0f ? 0.0f : m > 1.0f ? 1.0f : m;
	if (sweeping) *sweeping = s < 0.0f ? 0.0f : s > 1.0f ? 1.0f : s;
	return speed;
}

static bool HasWay(const NiPoint3& g);
static NiPoint3 WayRound(SmokeChain& c, int i, const NiPoint3& b, const NiPoint3& side, float across);
static void KeepOutOfGun(SmokeChain& c, const VolLook* frayLook, bool drift, int zip, float dt) {
	if (!VolumeGunOn || !Settings.volumeWrapGun) return;
	const NiPoint3& m = VolumeGunMuzzle;
	const NiPoint3& b = VolumeGunBack;
	const float L = Settings.volumeGunLength, Rg = Settings.volumeGunRadius;
	float moving, sweeping;
	const float gunSpeed = GunSpeed(&moving, &sweeping);
	// Points ease out over a few frames unless the barrel sweeps across them (then at once, or it would pass through);
	// walking forward, the barrel slides along its own length over smoke, so easing it out loses nothing.
	const float easeTime = 0.04f * (1.0f - sweeping);
	const float ease = easeTime > 0.001f ? 1.0f - expf(-dt / easeTime) : 1.0f;
	// "Its side" across the barrel: your aim's right, made square to the barrel; over the top: up, square to it.
	const float rb = VolumeAimRight.x * b.x + VolumeAimRight.y * b.y + VolumeAimRight.z * b.z;
	NiPoint3 right = { VolumeAimRight.x - b.x * rb, VolumeAimRight.y - b.y * rb, VolumeAimRight.z - b.z * rb };
	const float rl = sqrtf(right.x * right.x + right.y * right.y + right.z * right.z);
	right = rl > 0.01f ? NiPoint3{ right.x / rl, right.y / rl, right.z / rl } : NiPoint3{ 1.0f, 0.0f, 0.0f };
	NiPoint3 up = { -b.x * b.z, -b.y * b.z, 1.0f - b.z * b.z };
	const float ul = sqrtf(up.x * up.x + up.y * up.y + up.z * up.z);
	up = ul > 0.01f ? NiPoint3{ up.x / ul, up.y / ul, up.z / ul } : NiPoint3{ 0.0f, 0.0f, 1.0f };
	const NiPoint3 back = { m.x + b.x * L, m.y + b.y * L, m.z + b.z * L };
	// The sweep: the gun's movement through the air, across the barrel (its direction, and the direction across both).
	NiPoint3 sweepDir = { VolumeGunVel.x - VolumeWind.x, VolumeGunVel.y - VolumeWind.y, VolumeGunVel.z };
	const float sb = sweepDir.x * b.x + sweepDir.y * b.y + sweepDir.z * b.z;
	sweepDir = NiPoint3{ sweepDir.x - b.x * sb, sweepDir.y - b.y * sb, sweepDir.z - b.z * sb };
	const float sweep = sqrtf(sweepDir.x * sweepDir.x + sweepDir.y * sweepDir.y + sweepDir.z * sweepDir.z);
	if (sweep > 0.01f) sweepDir = NiPoint3{ sweepDir.x / sweep, sweepDir.y / sweep, sweepDir.z / sweep };
	const NiPoint3 sweepSide = { b.y * sweepDir.z - b.z * sweepDir.y, b.z * sweepDir.x - b.x * sweepDir.z, b.x * sweepDir.y - b.y * sweepDir.x };
	for (int i = 0; i < c.count; i++) {
		SmokePoint& s = c.p[i];
		const float age = SmokeClock - s.born;
		if (ShapeOn) {
			// The gun's real shape: a point closer to its surface than 0.6 of its own radius moves out along the way out
			// there; the same growing in, easing and leaning as with the tube.
			const float r = ReachRadius(s, age);
			const NiPoint3 fm = { s.pos.x - m.x, s.pos.y - m.y, s.pos.z - m.z };
			// Just leaving the muzzle, or still inside the barrel's bore (the heat smoke and trail start in it).
			const float fb = fm.x * b.x + fm.y * b.y + fm.z * b.z;
			const NiPoint3 fr = { fm.x - b.x * fb, fm.y - b.y * fb, fm.z - b.z * fb };
			const bool inBore = fb > 0.0f && fb < VolumeMuzzleDepth + 0.5f && fr.x * fr.x + fr.y * fr.y + fr.z * fr.z < 2.25f;
			const bool tip = inBore || fm.x * fm.x + fm.y * fm.y + fm.z * fm.z < (2.0f + r) * (2.0f + r);
			if (tip && age <= 0.1f) continue;
			NiPoint3 n;
			const float sd = GunSurface(s.pos, &n);
			const float keep = 0.6f * r;
			if (zip && Settings.volumeUnzip && age >= UnzipAge && gunSpeed > 40.0f && !s.unzipped && sd < 1.3f * keep + 1.0f)
				Unzip(c, i, zip, fm.x * right.x + fm.y * right.y + fm.z * right.z, keep + 1.0f, right.x, right.y, right.z);
			if (sd >= keep || (n.x == 0.0f && n.y == 0.0f && n.z == 0.0f)) continue;
			float grown = (age - 0.1f) / 0.4f;
			if (!tip && grown < moving) grown = moving;
			grown = grown < 0.0f ? 0.0f : grown > 1.0f ? 1.0f : grown;
			// Young smoke by a still gun may stay up to 3 units inside it while it grows into the rule.
			const float allowed = keep - (1.0f - grown) * (keep + 3.0f);
			if (sd >= allowed) continue;
			// Deep inside, the way out leans toward the point's own way (the way it went round before, its side, or up),
			// so the points of a strand leave together.
			NiPoint3 dir = n;
			if (sd < 0.0f) {
				NiPoint3 own = up;
				if (HasWay(s.gunDir)) {
					const float gb2 = s.gunDir.x * b.x + s.gunDir.y * b.y + s.gunDir.z * b.z;
					const NiPoint3 g = { s.gunDir.x - b.x * gb2, s.gunDir.y - b.y * gb2, s.gunDir.z - b.z * gb2 };
					const float gl = sqrtf(g.x * g.x + g.y * g.y + g.z * g.z);
					if (gl > 0.05f) own = NiPoint3{ g.x / gl, g.y / gl, g.z / gl };
				}
				else if (s.side != 0.0f) own = NiPoint3{ right.x * s.side, right.y * s.side, right.z * s.side };
				const float lean = -sd < 2.0f ? -sd * 0.25f : 0.5f;
				dir = NiPoint3{ n.x + own.x * lean, n.y + own.y * lean, n.z + own.z * lean };
				const float dl = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
				dir = dl > 0.001f ? NiPoint3{ dir.x / dl, dir.y / dl, dir.z / dl } : n;
			}
			const float step = (allowed - sd) * ease;
			s.pos.x += dir.x * step; s.pos.y += dir.y * step; s.pos.z += dir.z * step;
			continue;
		}
		const float alongRaw = (s.pos.x - m.x) * b.x + (s.pos.y - m.y) * b.y + (s.pos.z - m.z) * b.z;
		// At the muzzle, or still inside the barrel's bore (the heat smoke and trail start in it).
		const NiPoint3 tr = { s.pos.x - m.x - b.x * alongRaw, s.pos.y - m.y - b.y * alongRaw, s.pos.z - m.z - b.z * alongRaw };
		const bool atTip = alongRaw < 0.5f || (alongRaw < 0.5f + VolumeMuzzleDepth && tr.x * tr.x + tr.y * tr.y + tr.z * tr.z < 2.25f);
		if (atTip && age <= 0.1f) continue;
		const float along = alongRaw < 0.0f ? 0.0f : alongRaw > L ? L : alongRaw;
		const NiPoint3 axisPoint = { m.x + b.x * along, m.y + b.y * along, m.z + b.z * along };
		NiPoint3 o = { s.pos.x - axisPoint.x, s.pos.y - axisPoint.y, s.pos.z - axisPoint.z };
		const float d = sqrtf(o.x * o.x + o.y * o.y + o.z * o.z);
		const float full = Rg + 0.6f * ReachRadius(s, age);
		// Moving into it: the gun unzips the heat smoke and trail (one arm to each side of the barrel).
		if (zip && Settings.volumeUnzip && age >= UnzipAge && gunSpeed > 40.0f && !s.unzipped && d < full * 1.3f) {
			const float across = o.x * right.x + o.y * right.y + o.z * right.z;
			Unzip(c, i, zip, across, full, right.x, right.y, right.z);
		}
		if (d >= full) continue;
		// Young smoke grows into the rule from 0.1 s to 0.5 s old while the gun is still (it is then pushed out bit by bit,
		// each point along its own way out); smoke the moving gun has moved past is pushed out at once, young or not
		// (walking forward, the gun overtakes the smoke it has just made). At the tip, always by age.
		float grown = (age - 0.1f) / 0.4f;
		if (!atTip && grown < moving) grown = moving;
		grown = grown < 0.0f ? 0.0f : grown > 1.0f ? 1.0f : grown;
		const float reach = full * grown;
		if (d >= reach) continue;
		// Out from the barrel, leaning near its middle toward the point's own way out: the way it went round before, its
		// side, or over the top. Gradual (none from 0.35 of the reach out), so there is no sudden switch of direction.
		NiPoint3 own = up;
		if (HasWay(s.gunDir)) {
			const float gb2 = s.gunDir.x * b.x + s.gunDir.y * b.y + s.gunDir.z * b.z;
			const NiPoint3 g = { s.gunDir.x - b.x * gb2, s.gunDir.y - b.y * gb2, s.gunDir.z - b.z * gb2 };
			const float gl = sqrtf(g.x * g.x + g.y * g.y + g.z * g.z);
			if (gl > 0.05f) own = NiPoint3{ g.x / gl, g.y / gl, g.z / gl };
		}
		else if (s.side != 0.0f) own = NiPoint3{ right.x * s.side, right.y * s.side, right.z * s.side };
		const float lean = 0.35f * reach - d;
		NiPoint3 dir = o;
		if (lean > 0.0f) { dir.x += own.x * lean; dir.y += own.y * lean; dir.z += own.z * lean; }
		const float dl = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
		dir = dl > 0.001f ? NiPoint3{ dir.x / dl, dir.y / dl, dir.z / dl } : own;
		NiPoint3 to = { axisPoint.x + dir.x * reach, axisPoint.y + dir.y * reach, axisPoint.z + dir.z * reach };
		// In front of the barrel as it sweeps across (faster than 20 units per second across it): round it to the side
		// the point is on instead (shoved straight ahead it would ride along with the gun).
		if (sweep > 40.0f && d > 0.01f) {
			const float ahead = (o.x * -sweepDir.x + o.y * -sweepDir.y + o.z * -sweepDir.z) / d;   // 1 right in front
			if (ahead > 0.0f) {
				const float across = (o.x * sweepSide.x + o.y * sweepSide.y + o.z * sweepSide.z) / d;
				const NiPoint3 g = WayRound(c, i, b, sweepSide, across);
				const float w = ahead * (sweep < 80.0f ? (sweep - 40.0f) / 40.0f : 1.0f);
				const NiPoint3 round = { axisPoint.x + g.x * reach, axisPoint.y + g.y * reach, axisPoint.z + g.z * reach };
				to.x += (round.x - to.x) * w; to.y += (round.y - to.y) * w; to.z += (round.z - to.z) * w;
				// A blend of two places on the barrel's edge can fall just inside it: out to the edge again.
				NiPoint3 e = { to.x - axisPoint.x, to.y - axisPoint.y, to.z - axisPoint.z };
				const float el = sqrtf(e.x * e.x + e.y * e.y + e.z * e.z);
				if (el > 0.01f && el < reach) { to.x = axisPoint.x + e.x / el * reach; to.y = axisPoint.y + e.y / el * reach; to.z = axisPoint.z + e.z / el * reach; }
			}
		}
		s.pos.x += (to.x - s.pos.x) * ease; s.pos.y += (to.y - s.pos.y) * ease; s.pos.z += (to.z - s.pos.z) * ease;
	}
	// A piece passing through the barrel is split (unless both ends went the same way, or it is just leaving the muzzle,
	// or it is young smoke by a still gun).
	for (int i = 1; i < c.count; i++) {
		if (!c.p[i].link) continue;
		if (c.p[i].unzipped && c.p[i - 1].unzipped && c.p[i].side == c.p[i - 1].side) continue;
		if (SmokeClock - c.p[i].born < 0.1f || SmokeClock - c.p[i - 1].born < 0.1f) continue;
		if (moving < 1.0f && (SmokeClock - c.p[i].born < 0.5f || SmokeClock - c.p[i - 1].born < 0.5f)) continue;
		{   // both went the same way round
			const NiPoint3& g0 = c.p[i - 1].gunDir;
			const NiPoint3& g1 = c.p[i].gunDir;
			if (g0.x * g1.x + g0.y * g1.y + g0.z * g1.z > 0.5f) continue;
		}
		if (ShapeOn) {                                                 // the real shape: inside it a quarter, half or three quarters along
			const NiPoint3& p0 = c.p[i - 1].pos;
			const NiPoint3& p1 = c.p[i].pos;
			bool through = false;
			for (int k = 1; k <= 3 && !through; k++) {
				const float w = 0.25f * k;
				NiPoint3 nn;
				through = GunSurface(NiPoint3{ p0.x + (p1.x - p0.x) * w, p0.y + (p1.y - p0.y) * w, p0.z + (p1.z - p0.z) * w }, &nn) < 0.0f;
			}
			if (!through) continue;
		}
		else if (SegmentDistance2(c.p[i - 1].pos, c.p[i].pos, m, back) >= Rg * Rg) continue;
		c.p[i].link = false;
		if (frayLook) for (int e = i - 1; e <= i; e++) {
			SmokePoint& t = c.p[e];
			if (t.frayed) continue;
			t.frayed = true;
			if (Random01() > Settings.volumeTendrils) continue;
			NiPoint3 out;                                                   // away from the gun
			if (ShapeOn) GunSurface(t.pos, &out);
			else {
				float al = (t.pos.x - m.x) * b.x + (t.pos.y - m.y) * b.y + (t.pos.z - m.z) * b.z;
				al = al < 0.0f ? 0.0f : al > L ? L : al;
				out = NiPoint3{ t.pos.x - (m.x + b.x * al), t.pos.y - (m.y + b.y * al), t.pos.z - (m.z + b.z * al) };
			}
			SpawnTendril(t, PointRadius(t, SmokeClock - t.born), out, *frayLook, drift);
		}
	}
}

// The way round the barrel a point goes (across the barrel, unit length): the direction it chose before, made square to
// the barrel as it is now; or, the first time, the side of the sweep it is clearly on (across, along `side`); right in
// front of the barrel, the way its neighbours in the strand went (so the strand goes round together), or else the side it
// leans to.
static bool HasWay(const NiPoint3& g) { return g.x != 0.0f || g.y != 0.0f || g.z != 0.0f; }
static NiPoint3 WayRound(SmokeChain& c, int i, const NiPoint3& b, const NiPoint3& side, float across) {
	SmokePoint& s = c.p[i];
	if (!HasWay(s.gunDir)) {
		const bool clear = fabsf(across) > 0.3f;
		if (!clear && i > 0 && s.link && HasWay(c.p[i - 1].gunDir)) s.gunDir = c.p[i - 1].gunDir;
		else if (!clear && i + 1 < c.count && c.p[i + 1].link && HasWay(c.p[i + 1].gunDir)) s.gunDir = c.p[i + 1].gunDir;
		else {
			const float sign = across < 0.0f ? -1.0f : 1.0f;
			s.gunDir = NiPoint3{ side.x * sign, side.y * sign, side.z * sign };
		}
	}
	const float gb = s.gunDir.x * b.x + s.gunDir.y * b.y + s.gunDir.z * b.z;
	NiPoint3 g = { s.gunDir.x - b.x * gb, s.gunDir.y - b.y * gb, s.gunDir.z - b.z * gb };
	const float gl = sqrtf(g.x * g.x + g.y * g.y + g.z * g.z);
	return gl > 0.05f ? NiPoint3{ g.x / gl, g.y / gl, g.z / gl } : side;
}

// For the log: a rule that moves a smoke point more than 4 units in one frame, and more than half again what the gun or
// you (pace, units per second) moved that frame, makes a visible jump (less is smoke carried along at the speed of what
// pushes it); the first 40 of the session are written down (which smoke, how old, how far, by which rule, how fast the gun
// and you were moving, and the frame's length).
static int JumpsLogged = 0;
static void NoteJumps(const SmokeChain& c, const NiPoint3* before, const char* rule, float pace, float dt) {
	if (JumpsLogged >= 40) return;
	const float carried = 1.5f * pace * dt + 1.0f;
	const float limit = carried > 4.0f ? carried : 4.0f;
	for (int i = 0; i < c.count; i++) {
		const NiPoint3& p = c.p[i].pos;
		const float dx = p.x - before[i].x, dy = p.y - before[i].y, dz = p.z - before[i].z;
		const float moved2 = dx * dx + dy * dy + dz * dz;
		if (moved2 <= limit * limit) continue;
		const char* name = "tendril";
		for (int k = 0; k < ChainCount; k++) if (&c == &Chains[k]) name = ChainNames[k];
		if (&c == &Twins[0] || &c == &Twins[1]) name = "twin arm";
		JumpsLogged++;
		DebugLine("volumetric smoke jump: %s point %.2f s old moved %.1f units in one frame by %s (gun moving %.0f units/s, you %.0f; frame %.0f ms)",
			name, SmokeClock - c.p[i].born, sqrtf(moved2), rule,
			sqrtf(VolumeGunVel.x * VolumeGunVel.x + VolumeGunVel.y * VolumeGunVel.y + VolumeGunVel.z * VolumeGunVel.z),
			sqrtf(VolumePlayerVel.x * VolumePlayerVel.x + VolumePlayerVel.y * VolumePlayerVel.y), dt * 1000.0f);
		return;
	}
}

// ---- Air flowing round a pole (the gun's barrel, your body) --------------------------------------------------------
// The air streaming past a solid pole parts in front of it, speeds up past its sides and closes in behind it. For smoke
// at `o` from the pole's axis (square to it, length d) where the pole reaches out to a, with the air streaming past the
// pole at U (square to its axis, speed u): how much the air there moves differently from the free stream,
// (a/d)^2 (U - 2 (U.er) er), so none goes into the pole; plus, in front of it (where that air would stop dead against the
// pole and ride along with it), a turn round it the way the smoke goes (`way`, unit, square to the axis), so it slides
// past instead of being pushed ahead. Added to the smoke's movement (not its own speed): it acts at once, starts well
// before the pole arrives and stops when the pole has passed. Fades out between 3 and 4 times the reach; inside the reach
// only the sliding round is left (no air into or out of the pole there).
static NiPoint3 PoleFlow(const NiPoint3& o, float d, float a, const NiPoint3& U, float u, const NiPoint3& way) {
	if (d < 0.01f || d >= 4.0f * a || u < 1.0f) return NiPoint3{};
	const NiPoint3 er = { o.x / d, o.y / d, o.z / d };
	const float q = d > a ? a * a / (d * d) : 1.0f, ue = U.x * er.x + U.y * er.y + U.z * er.z;
	NiPoint3 v = { q * (U.x - 2.0f * ue * er.x), q * (U.y - 2.0f * ue * er.y), q * (U.z - 2.0f * ue * er.z) };
	if (ue < 0.0f) {
		const float across = er.x * way.x + er.y * way.y + er.z * way.z;
		const float turn = u * (-ue / u) * (1.0f - fabsf(across)) * (d > a ? a / d : 1.0f);   // 1 right in front, 0 at the sides
		v.x += way.x * turn; v.y += way.y * turn; v.z += way.z * turn;
	}
	if (d > 3.0f * a) {
		const float f = 1.0f - (d - 3.0f * a) / a;
		const float s = f * f * (3.0f - 2.0f * f);
		v.x *= s; v.y *= s; v.z *= s;
	}
	return v;
}

// ---- Stirred smoke spreads out ------------------------------------------------------------------------------------
// Real smoke that is stirred up (walked into, cut by the gun, caught in a whirlpool) mixes with the air round it: it
// billows out wider and thinner (the same smoke over more air), its points wander apart so a strand frays into a loose
// cloud, and its edges break up into wisps. Each point's stir follows how hard the air round it is being moved: the flow
// round your body and the gun, the whirlpools, being inside your reach, the pushes that keep it out of you and the gun
// (80 units per second counts as fully stirred; splitting in two stirs it too). It rises quickly and settles over about a
// second. While stirred a point widens (up to 3 times as wide) and thins faster still, curls and wanders more (UpdateChain),
// is drawn wispier (counted as older for the edge detail), and when strongly stirred may peel off a tendril along the way
// it is pushed. [Volume] fDiffuse: how much (0 = none, 1 = normal, higher = more).

// The keep-out rules' pushes this frame stir the points they moved (`before`: the positions before the rule).
static void NotePushes(const SmokeChain& c, const NiPoint3* before, float* stirIn, NiPoint3* stirDir, float dt) {
	if (dt <= 0.0001f) return;
	for (int i = 0; i < c.count; i++) {
		const NiPoint3 m = { (c.p[i].pos.x - before[i].x) / dt, (c.p[i].pos.y - before[i].y) / dt, (c.p[i].pos.z - before[i].z) / dt };
		const float speed = sqrtf(m.x * m.x + m.y * m.y + m.z * m.z);
		if (speed < 1.0f) continue;
		stirIn[i] += speed / 80.0f;
		stirDir[i] = NiPoint3{ stirDir[i].x + m.x, stirDir[i].y + m.y, stirDir[i].z + m.z };
	}
}

static void Diffuse(SmokeChain& c, const float* stirIn, const NiPoint3* stirDir, float dt, const VolLook* frayLook, bool drift) {
	const float k = Settings.volumeDiffuse;
	const float rise = 1.0f - expf(-dt / 0.08f), settle = expf(-dt / 0.8f);
	for (int i = 0; i < c.count; i++) {
		SmokePoint& s = c.p[i];
		// A burst billows the smoke out as it flies: it swells (up to 3.5 times as wide) into a puff, thinning less than its
		// widening (it still reads as a puff), then settles and thins as the stir dies down.
		const float kick = sqrtf(s.kick.x * s.kick.x + s.kick.y * s.kick.y + s.kick.z * s.kick.z);
		if (kick > 1.0f && s.spread < 3.5f) {
			float b = expf(3.0f * kick / 80.0f * dt);
			if (s.spread * b > 3.5f) b = 3.5f / s.spread;
			s.spread *= b;
			s.opacity /= powf(b, 0.4f);
		}
		if (k <= 0.0f) continue;
		const float in = stirIn[i] < 1.5f ? stirIn[i] : 1.5f;
		s.stir = in > s.stir ? s.stir + (in - s.stir) * rise : s.stir * settle;
		if (s.stir < 0.01f) continue;
		// Wider (up to 3 times as wide), and thinner than the widening alone (it mixes into the air it spreads through, so it
		// fades into a ragged veil rather than swelling into a puff).
		float f = expf(1.2f * k * s.stir * dt);
		if (s.spread * f > 3.0f) f = 3.0f / s.spread;
		if (f > 1.0f) { s.spread *= f; s.opacity /= powf(f, 1.6f); }
		// Strongly stirred: a wisp may peel off, the way it is being pushed.
		if (frayLook && !s.frayed && s.stir > 0.7f && Random01() < Settings.volumeTendrils * s.stir * k * dt * 1.5f) {
			s.frayed = true;
			SpawnTendril(s, PointRadius(s, SmokeClock - s.born), stirDir[i], *frayLook, drift);
		}
	}
}

// ---- Bursts: smoke you run or swing through is thrown outward --------------------------------------------------------
// Running (or swinging the gun) through smoke hits it: the air you shove carries it outward and a little up in a quick
// puff, and drags what is just behind you along a little (your wake); it billows as it flies (Diffuse) and settles as
// the burst dies away over about a third of a second (UpdateChain). out: away from you (or the gun's surface) there;
// wd: the way you or the gun move (unit length); u: how fast; hit: 1 right against it .. 0 at the edge of its
// reach. Keeps the stronger burst wanted this frame. [Volume] fBurst: how strong (0 = none).
static void WantBurst(NiPoint3& want, const NiPoint3& out, const NiPoint3& fwd, float u, float hit) {
	const float k = Settings.volumeBurst * u * (hit > 1.0f ? 1.0f : hit);
	if (k <= 0.0f) return;
	const float facing = out.x * fwd.x + out.y * fwd.y + out.z * fwd.z;      // 1 in front of it, 0 beside it, -1 behind it
	float outward = facing + 0.6f;                                            // in front and beside: out; behind: hardly
	outward = outward < 0.0f ? 0.0f : outward > 1.0f ? 1.0f : outward;
	const float along = 0.3f + 0.3f * (facing < 0.0f ? -facing : 0.0f);       // dragged along, more so behind (the wake)
	const NiPoint3 v = { (out.x * 0.85f * outward + fwd.x * along) * k, (out.y * 0.85f * outward + fwd.y * along) * k,
		(out.z * 0.85f * outward + fwd.z * along + 0.25f) * k };
	if (v.x * v.x + v.y * v.y + v.z * v.z > want.x * want.x + want.y * want.y + want.z * want.z) want = v;
}

// ---- Bullets: every shot punches through the smoke ------------------------------------------------------------------
// A bullet flies from the muzzle to where you aim (the camera, handed over by NVR every frame: GunFX_SetView; without it,
// along the barrel) and shoves the air round its path aside: smoke within reach of the path is thrown outward from it and
// a little along it (the bullet's wake), so a tunnel opens through the smoke, billows out (Diffuse) and fills in again as
// the burst dies away (UpdateChain). The smoke of the shot itself is left alone: the next shot cuts through it. The haze's
// blobs are too big to show a tunnel by moving them: NVR clears a tunnel through the haze itself along each recent
// bullet's path (GunFX_GetBullets). [Volume] fBullet: how hard (0 = off), fBulletReach: how far from the path (game
// units; a big puff is reached by half its radius more).
struct Bullet { NiPoint3 from, dir; float at; };
static const int BulletMax = 16;
static Bullet Bullets[BulletMax] = {};
static int BulletNext = 0;
static NiPoint3 ViewEye = {}, ViewForward = {};
static ULONGLONG ViewAt = 0;
static void NoteBullet(const NiPoint3& muzzle) {
	if (Settings.volumeBullet <= 0.0f) return;
	NiPoint3 dir = { -VolumeGunBack.x, -VolumeGunBack.y, -VolumeGunBack.z };
	bool view = false;
	NiPoint3 eye = {}, fwd = {};
	if (HeatLockReady) {
		EnterCriticalSection(&HeatLock);
		view = ViewAt && GetTickCount64() - ViewAt < 1000;
		eye = ViewEye; fwd = ViewForward;
		LeaveCriticalSection(&HeatLock);
	}
	if (view) {                                                // toward the point you aim at, 1500 units ahead
		const NiPoint3 aim = { eye.x + fwd.x * 1500.0f - muzzle.x, eye.y + fwd.y * 1500.0f - muzzle.y, eye.z + fwd.z * 1500.0f - muzzle.z };
		const float l = sqrtf(aim.x * aim.x + aim.y * aim.y + aim.z * aim.z);
		if (l > 1.0f) dir = NiPoint3{ aim.x / l, aim.y / l, aim.z / l };
	}
	const float dl = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
	if (dl < 0.5f) return;
	Bullets[BulletNext] = Bullet{ muzzle, NiPoint3{ dir.x / dl, dir.y / dl, dir.z / dl }, SmokeClock };
	BulletNext = (BulletNext + 1) % BulletMax;
}
// The burst the bullets fired in the last 0.08 s throw a smoke point out with (kept if stronger than want).
static void BulletBurst(NiPoint3& want, const SmokePoint& s, float age) {
	if (Settings.volumeBullet <= 0.0f) return;
	for (const Bullet& b : Bullets) {
		const float since = SmokeClock - b.at;
		if (b.at <= 0.0f || since < 0.0f || since > 0.08f || s.born >= b.at) continue;
		const NiPoint3 rel = { s.pos.x - b.from.x, s.pos.y - b.from.y, s.pos.z - b.from.z };
		const float along = rel.x * b.dir.x + rel.y * b.dir.y + rel.z * b.dir.z;
		if (along < 0.0f || along > 1500.0f) continue;
		NiPoint3 off = { rel.x - b.dir.x * along, rel.y - b.dir.y * along, rel.z - b.dir.z * along };
		const float d = sqrtf(off.x * off.x + off.y * off.y + off.z * off.z);
		const float reach = Settings.volumeBulletReach + 0.5f * PointRadius(s, age);
		if (d >= reach) continue;
		if (d < 0.01f) {                                       // right on the path: out to any side
			off = fabsf(b.dir.z) < 0.9f ? NiPoint3{ -b.dir.y, b.dir.x, 0.0f } : NiPoint3{ 1.0f, 0.0f, 0.0f };
			const float ol = sqrtf(off.x * off.x + off.y * off.y + off.z * off.z);
			off = NiPoint3{ off.x / ol * 0.01f, off.y / ol * 0.01f, off.z / ol * 0.01f };
		}
		const float dd = d > 0.01f ? d : 0.01f;
		float hit = 1.0f - d / reach;
		hit = hit * hit * (3.0f - 2.0f * hit);
		const float k = Settings.volumeBullet * 90.0f * hit;
		const NiPoint3 v = { off.x / dd * k + b.dir.x * 0.4f * k, off.y / dd * k + b.dir.y * 0.4f * k, off.z / dd * k + b.dir.z * 0.4f * k };
		if (v.x * v.x + v.y * v.y + v.z * v.z > want.x * want.x + want.y * want.y + want.z * want.z) want = v;
	}
}

// Air shoved aside has momentum: smoke the air round you or the gun carried away keeps a share of that motion (and lifts
// a little), so it flies on outward after you pass instead of stopping dead (low: what the parting air moved it with).
static void KeepMomentum(NiPoint3& want, const NiPoint3& flow) {
	const float k = Settings.volumeBurst * 1.6f;
	if (k <= 0.0f) return;
	const float fl = sqrtf(flow.x * flow.x + flow.y * flow.y + flow.z * flow.z);
	const NiPoint3 v = { flow.x * k, flow.y * k, flow.z * k + fl * k * 0.3f };
	if (v.x * v.x + v.y * v.y + v.z * v.z > want.x * want.x + want.y * want.y + want.z * want.z) want = v;
}

// Moves and ages one chain; emits a new point while its source is on.
static void UpdateChain(SmokeChain& c, const SmokeSource& src, float dt, const VolLook& v) {
	const float rise = v.rise * Settings.volumeRise;
	const float curl = v.curl * Settings.volumeCurl;
	const float keep = expf(-src.drag * dt);
	// How hard the air round each point is being moved this frame (see Diffuse), and which way.
	static float stirIn[ChainMax];
	static NiPoint3 stirDir[ChainMax];
	for (int i = 0; i < c.count; i++) {
		SmokePoint& s = c.p[i];
		const float age = SmokeClock - s.born;
		stirIn[i] = 0.0f;
		stirDir[i] = NiPoint3{};
		const float bodyShare = BodyShare(s.pos);
		BodyShareOf[i] = bodyShare;
		// Slowly changing sideways push, stronger with age (the strand bends gently low and drifts more higher up), and
		// stronger and up and down too where the smoke is stirred, so its points wander apart (it frays and spreads).
		const float w = s.seed + SmokeClock * 0.6f;
		const float stirred = Settings.volumeDiffuse * s.stir;
		const float push = curl * (0.3f + age) * (1.0f + 3.0f * stirred);
		s.vel.x += (sinf(w + s.pos.z * 0.05f) + 0.5f * sinf(w * 2.3f + s.pos.z * 0.11f)) * push * dt;
		s.vel.y += (cosf(w * 0.8f + s.pos.z * 0.06f) + 0.5f * sinf(w * 1.7f + 1.3f)) * push * dt;
		s.vel.z += rise * dt + (sinf(w * 1.3f + 2.1f) + 0.5f * sinf(w * 2.9f + s.pos.x * 0.07f)) * push * stirred * 0.6f * dt;
		// Drag toward the moving air: horizontally the smoke takes on the wind (still air: it just slows down).
		const float windKeep = keep * VolumeWindKeep;
		s.vel.x = VolumeWind.x + (s.vel.x - VolumeWind.x) * windKeep;
		s.vel.y = VolumeWind.y + (s.vel.y - VolumeWind.y) * windKeep;
		s.vel.z *= keep;
		// Your body (an upright capsule from your feet to the top of your head): smoke inside it is pushed out, and
		// thins where it is stirred, so it never cuts through you. While you move it flows around you, as air does:
		// smoke ahead of you slides aside (rather than being shoved ahead and carried along), smoke behind you is pulled
		// into your wake. Smoke made within your reach is left alone until 0.1 s old (and the push grows in until 0.4 s), so it still leaves
		// the barrel and the port cleanly.
		if (VolumePlayerOn && (s.clear || age > 0.1f)) {
			const float R = Settings.volumePlayerRadius;
			float cz = s.pos.z;
			const float lo = VolumePlayerFeet.z + R, hi = VolumePlayerFeet.z + Settings.volumePlayerHeight - R;
			cz = cz < lo ? lo : cz > hi ? hi : cz;
			const float dx = s.pos.x - VolumePlayerFeet.x, dy = s.pos.y - VolumePlayerFeet.y, dz = s.pos.z - cz;
			const float reach = R + 0.7f * ReachRadius(s, age);
			const float d2 = dx * dx + dy * dy + dz * dz;
			if (d2 < reach * reach) {
				const float d = sqrtf(d2);
				float overlap = (1.0f - d / reach) * bodyShare;
				if (!s.clear && age < 0.4f) overlap *= (age - 0.1f) / 0.3f;
				const NiPoint3 out = d > 0.01f ? NiPoint3{ dx / d, dy / d, dz / d } : NiPoint3{ 1.0f, 0.0f, 0.0f };
				const float shove = Settings.volumePlayerPush * overlap;
				NiPoint3 target = { out.x * shove, out.y * shove, out.z * shove };
				// From pushed straight out (standing) to flowing around you (walking), gradually between 20 and 80 units
				// per second, so a shuffle or a still stance does not flip it back and forth.
				const float speed = sqrtf(VolumePlayerVel.x * VolumePlayerVel.x + VolumePlayerVel.y * VolumePlayerVel.y);
				const float flow = speed < 20.0f ? 0.0f : speed > 80.0f ? 1.0f : (speed - 20.0f) / 60.0f;
				if (flow > 0.0f) {
					const float fx = VolumePlayerVel.x / speed, fy = VolumePlayerVel.y / speed;   // your heading
					const float rx = fy, ry = -fx;                                                // its right
					const float ahead = out.x * fx + out.y * fy;
					const float across = out.x * rx + out.y * ry;
					if (!s.unzipped) s.side = ChooseSide(c, i, across, fabsf(across) > 0.25f);   // the side it is on
					const float aside = shove + speed * 0.5f;
					NiPoint3 flowing = { rx * s.side * aside, ry * s.side * aside, target.z };
					if (ahead < 0.0f) { flowing.x += fx * speed * 0.5f; flowing.y += fy * speed * 0.5f; }   // the wake
					target.x += (flowing.x - target.x) * flow;
					target.y += (flowing.y - target.y) * flow;
				}
				const float k = 1.0f - expf(-10.0f * overlap * dt);
				s.vel.x += (target.x - s.vel.x) * k;
				s.vel.y += (target.y - s.vel.y) * k;
				s.vel.z += (target.z - s.vel.z) * k;
				s.opacity *= expf(-Settings.volumeStir * overlap * dt);
				stirIn[i] += overlap;                                  // inside your reach: stirred
			}
		}
		// Drift: the heat smoke and trail lean toward the right of where you aim and a little away from you, so they
		// keep out of the middle of your view (and away from your body as you walk); it grows in over the first 0.6 s,
		// so the smoke still leaves the barrel straight. Only adds speed that way, relative to the wind.
		if (src.drift && VolumeGunOn) {
			float grown = age / 0.6f;
			grown = grown < 0.0f ? 0.0f : grown > 1.0f ? 1.0f : grown;
			const float k = 1.0f - expf(-1.5f * grown * dt);
			const float rx = s.vel.x - VolumeWind.x, ry = s.vel.y - VolumeWind.y;
			const float right = rx * VolumeAimRight.x + ry * VolumeAimRight.y;
			if (right < Settings.volumeDriftRight) {
				s.vel.x += VolumeAimRight.x * (Settings.volumeDriftRight - right) * k;
				s.vel.y += VolumeAimRight.y * (Settings.volumeDriftRight - right) * k;
			}
			const float away = rx * VolumeAimForward.x + ry * VolumeAimForward.y;
			if (away < Settings.volumeDriftAway) {
				s.vel.x += VolumeAimForward.x * (Settings.volumeDriftAway - away) * k;
				s.vel.y += VolumeAimForward.y * (Settings.volumeDriftAway - away) * k;
			}
		}
		// The air flowing round the gun and your body as they move (PoleFlow): the smoke near them is carried round them
		// the way air is, starting before they arrive, so it parts in front of them and slides past their sides instead of
		// being shoved aside at the last moment (KeepOutOfGun and KeepOutOfBody stay as the last guard). Smoke younger
		// than 0.1 s is left alone, so it still leaves the muzzle and the port cleanly. Swirls behind them are the
		// whirlpools of ShedVortices.
		NiPoint3 around = {};
		NiPoint3 burst = {};                                           // the burst the gun or you would throw it out with (WantBurst)
		if (!src.haze) BulletBurst(burst, s, age);                     // and the bullets fired through it
		if (ShapeOn && VolumeGunOn && Settings.volumeWrapGun && age > 0.1f) {
			// The gun's real shape: air meeting its surface stops going into it and turns along it (nearer the surface,
			// more; out to about four times fGunRadius), and in front of a sweep it goes round the way the smoke goes
			// (WayRound), so smoke parts round the barrel, the sights and the magazine before they reach it.
			NiPoint3 n;
			const float dist = GunSurface(s.pos, &n);
			const float L = Settings.volumeGunRadius > 0.5f ? Settings.volumeGunRadius : 0.5f;
			if (dist < 4.0f * L && (n.x != 0.0f || n.y != 0.0f || n.z != 0.0f)) {
				const NiPoint3 U = { VolumeWind.x - VolumeGunVel.x, VolumeWind.y - VolumeGunVel.y, -VolumeGunVel.z };   // the air past the gun
				const float u = sqrtf(U.x * U.x + U.y * U.y + U.z * U.z);
				const float un = U.x * n.x + U.y * n.y + U.z * n.z;
				// Sliding along the gun (walking or running with it): the air rubbing past its surface churns the smoke there,
				// so smoke streaming back along the barrel spreads into a thin veil over the gun and peels off it in wisps
				// (Diffuse), instead of staying a thin line hidden behind the barrel. Within 2 x fGunRadius of the surface, from
				// 30 units per second of air along the barrel, full at 150 (a sideways sweep stirs it through `around`).
				{
					const NiPoint3& gb = VolumeGunBack;
					const float slide = fabsf(U.x * gb.x + U.y * gb.y + U.z * gb.z);
					const float nearGun = dist < 2.0f * L ? 1.0f - (dist > 0.0f ? dist : 0.0f) / (2.0f * L) : 0.0f;
					const float rub = slide < 30.0f ? 0.0f : slide > 150.0f ? 1.0f : (slide - 30.0f) / 120.0f;
#ifdef GUNFX_REPLAY
					if (!getenv("OLD_BODY") && !getenv("NO_RUB"))
#endif
					stirIn[i] += 0.8f * nearGun * rub;
				}
				if (u > 40.0f && dist < 1.5f * L) {                        // a real sweep: the gun hits the smoke
					const float hit = 1.0f - (dist > 0.0f ? dist : 0.0f) / (1.5f * L);
					WantBurst(burst, n, NiPoint3{ -U.x / u, -U.y / u, -U.z / u }, u, hit);
				}
				if (u > 15.0f && un < 0.0f) {
					const float close = L / (L + (dist > 0.0f ? dist : 0.0f));
					NiPoint3 f = { -un * n.x * close * close, -un * n.y * close * close, -un * n.z * close * close };
					if (u > 40.0f) {
						const NiPoint3& b = VolumeGunBack;
						const float ub = U.x * b.x + U.y * b.y + U.z * b.z;
						const NiPoint3 Uc = { U.x - b.x * ub, U.y - b.y * ub, U.z - b.z * ub };          // across the barrel
						const float uc = sqrtf(Uc.x * Uc.x + Uc.y * Uc.y + Uc.z * Uc.z);
						if (uc > 1.0f) {
							const NiPoint3 S = { (b.y * Uc.z - b.z * Uc.y) / uc, (b.z * Uc.x - b.x * Uc.z) / uc, (b.x * Uc.y - b.y * Uc.x) / uc };
							const float across = n.x * S.x + n.y * S.y + n.z * S.z;
							NiPoint3 g = WayRound(c, i, b, S, across);
							const float gn = g.x * n.x + g.y * n.y + g.z * n.z;
							g = NiPoint3{ g.x - n.x * gn, g.y - n.y * gn, g.z - n.z * gn };              // along the surface
							const float gl = sqrtf(g.x * g.x + g.y * g.y + g.z * g.z);
							if (gl > 0.05f) {
								const float turn = (-un) * (1.0f - fabsf(across)) * close / gl;
								f.x += g.x * turn; f.y += g.y * turn; f.z += g.z * turn;
							}
						}
					}
					float k = u < 30.0f ? (u - 15.0f) / 15.0f : 1.0f;
					if (dist > 3.0f * L) { const float e = 1.0f - (dist - 3.0f * L) / L; k *= e * e * (3.0f - 2.0f * e); }
					around.x += f.x * k; around.y += f.y * k; around.z += f.z * k;
					if (u > 40.0f) KeepMomentum(burst, NiPoint3{ f.x * k, f.y * k, f.z * k });
				}
			}
		}
		else if (VolumeGunOn && Settings.volumeWrapGun && age > 0.1f) {
			// The gun as a tube (until its real shape is ready, or with [Volume] bGunShape 0): across the barrel only (air
			// along it just slides along it), within its length.
			const NiPoint3& m = VolumeGunMuzzle;
			const NiPoint3& b = VolumeGunBack;
			const float along = (s.pos.x - m.x) * b.x + (s.pos.y - m.y) * b.y + (s.pos.z - m.z) * b.z;
			if (along > 0.0f && along < Settings.volumeGunLength) {
				const NiPoint3 o = { s.pos.x - (m.x + b.x * along), s.pos.y - (m.y + b.y * along), s.pos.z - (m.z + b.z * along) };
				const float d = sqrtf(o.x * o.x + o.y * o.y + o.z * o.z);
				const float a = Settings.volumeGunRadius + 0.6f * ReachRadius(s, age);
				NiPoint3 U = { VolumeWind.x - VolumeGunVel.x, VolumeWind.y - VolumeGunVel.y, -VolumeGunVel.z };
				const float ub = U.x * b.x + U.y * b.y + U.z * b.z;
				U = NiPoint3{ U.x - b.x * ub, U.y - b.y * ub, U.z - b.z * ub };
				const float u = sqrtf(U.x * U.x + U.y * U.y + U.z * U.z);
				if (u > 15.0f && d > 0.01f && d < 4.0f * a) {
					// The way round: across the sweep, to the side the smoke is on (remembered, see WayRound); only for a
					// real sweep (faster than 40), not the gun's small drift.
					NiPoint3 way = {};
					const NiPoint3 uh = { U.x / u, U.y / u, U.z / u };
					if (u > 40.0f) {
						const NiPoint3 S = { b.y * uh.z - b.z * uh.y, b.z * uh.x - b.x * uh.z, b.x * uh.y - b.y * uh.x };
						way = WayRound(c, i, b, S, (o.x * S.x + o.y * S.y + o.z * S.z) / d);
					}
					const float ramp = u < 30.0f ? (u - 15.0f) / 15.0f : 1.0f;   // eases in from 15 to 30 units per second
					const NiPoint3 f = PoleFlow(o, d, a, U, u, way);
					around.x += f.x * ramp; around.y += f.y * ramp; around.z += f.z * ramp;
					if (u > 40.0f) KeepMomentum(burst, NiPoint3{ f.x * ramp, f.y * ramp, f.z * ramp });
					const float gap = d - a, reachOut = 1.5f * Settings.volumeGunRadius;
					if (u > 40.0f && gap < reachOut)
						WantBurst(burst, NiPoint3{ o.x / d, o.y / d, o.z / d }, NiPoint3{ -uh.x, -uh.y, -uh.z }, u, 1.0f - (gap > 0.0f ? gap : 0.0f) / reachOut);
				}
			}
		}
		if (VolumePlayerOn && Settings.volumePlayerPush > 0.0f && (s.clear || age > 0.1f)) {
			// Your body: an upright pole (rounding off above your head and below your feet), the air streaming past it
			// as you walk; smoke made within your reach (the ejection port's) takes it in gradually until 0.4 s old.
			const float R = Settings.volumePlayerRadius;
			const float bottom = VolumePlayerFeet.z, top = VolumePlayerFeet.z + Settings.volumePlayerHeight;
			const float a = R + 0.7f * ReachRadius(s, age);
			const float dz = s.pos.z < bottom ? bottom - s.pos.z : s.pos.z > top ? s.pos.z - top : 0.0f;
			const NiPoint3 U = { VolumeWind.x - VolumePlayerVel.x, VolumeWind.y - VolumePlayerVel.y, 0.0f };
			const float u = sqrtf(U.x * U.x + U.y * U.y);
			const NiPoint3 o = { s.pos.x - VolumePlayerFeet.x, s.pos.y - VolumePlayerFeet.y, 0.0f };
			const float d = sqrtf(o.x * o.x + o.y * o.y);
			if (u > 15.0f && dz < a && d > 0.01f && d < 4.0f * a) {
				const NiPoint3 S = { -U.y / u, U.x / u, 0.0f };                       // the right of where you walk
				const float across = (o.x * S.x + o.y * S.y) / d;
				if (!s.unzipped) s.side = ChooseSide(c, i, across, fabsf(across) > 0.25f);
				const NiPoint3 way = { S.x * s.side, S.y * s.side, 0.0f };
				float k = u < 30.0f ? (u - 15.0f) / 15.0f : 1.0f;
				k *= (1.0f - dz / a) * bodyShare;
				if (!s.clear && age < 0.4f) k *= (age - 0.1f) / 0.3f;   // made within your reach: takes it in gradually
				NiPoint3 f = PoleFlow(o, d, a, U, u, way);
				// Behind you the flow would pull smoke back in to your rear edge, where it piled up (2026-10-07): only the
				// sideways slide is kept there, so the trail drifts off behind you instead of collecting at your back.
				const float ue = (U.x * o.x + U.y * o.y) / d;          // > 0: behind you (downstream)
				const float radial = (f.x * o.x + f.y * o.y) / d;      // < 0: pulled in toward your edge
				if (ue > 0.0f && radial < 0.0f) { f.x -= radial * o.x / d; f.y -= radial * o.y / d; }
				around.x += f.x * k; around.y += f.y * k;
				const float gap = d - a;                                     // running into it: you hit the smoke
				if (u > 40.0f && gap < 1.2f * a)
					WantBurst(burst, NiPoint3{ o.x / d, o.y / d, 0.0f }, NiPoint3{ -U.x / u, -U.y / u, 0.0f }, u, k * (1.0f - (gap > 0.0f ? gap : 0.0f) / (1.2f * a)));
				if (u > 40.0f) KeepMomentum(burst, NiPoint3{ f.x * k, f.y * k, 0.0f });
			}
		}
		{   // the burst: up to what the hit gives it within 0.05 s, dying away over about a third of a second
			const float want2 = burst.x * burst.x + burst.y * burst.y + burst.z * burst.z;
			const float have2 = s.kick.x * s.kick.x + s.kick.y * s.kick.y + s.kick.z * s.kick.z;
			if (want2 > have2) {
				const float k = 1.0f - expf(-dt / 0.05f);
				s.kick.x += (burst.x - s.kick.x) * k; s.kick.y += (burst.y - s.kick.y) * k; s.kick.z += (burst.z - s.kick.z) * k;
			}
			const float fade = expf(-dt / 0.35f);
			s.kick.x *= fade; s.kick.y *= fade; s.kick.z *= fade;
			around.x += s.kick.x; around.y += s.kick.y; around.z += s.kick.z;   // (stirs it too, below)
		}
		const NiPoint3 swirl = VortexCount ? VortexFlow(s.pos) : NiPoint3{};
		{   // the air moved round it (by your body, the gun and the whirlpools) stirs it
			const NiPoint3 m = { around.x + swirl.x, around.y + swirl.y, around.z + swirl.z };
			stirIn[i] += sqrtf(m.x * m.x + m.y * m.y + m.z * m.z) / 80.0f;
			stirDir[i] = NiPoint3{ stirDir[i].x + m.x, stirDir[i].y + m.y, stirDir[i].z + m.z };
		}
		s.pos.x += (s.vel.x + swirl.x + around.x) * dt; s.pos.y += (s.vel.y + swirl.y + around.y) * dt; s.pos.z += (s.vel.z + swirl.z + around.z) * dt;
	}
	// Smoothing: each point inside a strand drifts toward the middle of its neighbours, so the sharp kinks a quick or
	// jittery gun movement leaves in the strand even out within a fraction of a second, as turbulence does to real smoke.
	// It grows in over the first 0.4 s, so the smoke still leaves the barrel straight; the newest point stays on the
	// barrel and a strand's ends stay put. [Volume] fSmooth: how quickly (per second; 0 = off).
	if (Settings.volumeSmooth > 0.0f && c.count > 2) {
		static NiPoint3 mid[ChainMax];
		for (int i = 1; i + 1 < c.count; i++) {
			const NiPoint3& p0 = c.p[i - 1].pos;
			const NiPoint3& p2 = c.p[i + 1].pos;
			mid[i] = NiPoint3{ (p0.x + p2.x) * 0.5f, (p0.y + p2.y) * 0.5f, (p0.z + p2.z) * 0.5f };
		}
		for (int i = 1; i + 1 < c.count; i++) {
			if (!c.p[i].link || !c.p[i + 1].link) continue;           // only between two neighbours of the same strand
			float grown = (SmokeClock - c.p[i].born) / 0.4f;
			grown = grown < 0.0f ? 0.0f : grown > 1.0f ? 1.0f : grown;
			const float k = 1.0f - expf(-Settings.volumeSmooth * grown * dt);
			SmokePoint& s = c.p[i];
			s.pos.x += (mid[i].x - s.pos.x) * k;
			s.pos.y += (mid[i].y - s.pos.y) * k;
			s.pos.z += (mid[i].z - s.pos.z) * k;
		}
	}
	static NiPoint3 before[ChainMax];
	for (int i = 0; i < c.count; i++) before[i] = c.p[i].pos;
	KeepOutOfBody(c, src.frays ? &v : nullptr, src.drift, src.zip, dt);
	NoteJumps(c, before, "your body's rule", sqrtf(VolumePlayerVel.x * VolumePlayerVel.x + VolumePlayerVel.y * VolumePlayerVel.y), dt);
	NotePushes(c, before, stirIn, stirDir, dt);
	for (int i = 0; i < c.count; i++) before[i] = c.p[i].pos;
	KeepOutOfGun(c, src.frays ? &v : nullptr, src.drift, src.zip, dt);
	NoteJumps(c, before, "the gun's rule", sqrtf(VolumeGunVel.x * VolumeGunVel.x + VolumeGunVel.y * VolumeGunVel.y + VolumeGunVel.z * VolumeGunVel.z), dt);
	NotePushes(c, before, stirIn, stirDir, dt);
	Diffuse(c, stirIn, stirDir, dt, src.frays ? &v : nullptr, src.drift);
	while (c.count && SmokeClock - c.p[0].born > c.p[0].life) {   // expired at the old end
		memmove(&c.p[0], &c.p[1], sizeof(SmokePoint) * (c.count - 1));
		c.count--;
		if (c.count) c.p[0].link = false;
	}
	if (src.on) {
		float spacing = (v.life + v.lifeVar) / (src.points > 2.0f ? src.points : 2.0f);
		if (spacing < Settings.volumeSpacing) spacing = Settings.volumeSpacing;
		const bool jumped = c.count && c.emitting && [&]() {
			const NiPoint3& q = c.p[c.count - 1].pos;
			const float dx = src.at.x - q.x, dy = src.at.y - q.y, dz = src.at.z - q.z;
			return dx * dx + dy * dy + dz * dz > 200.0f * 200.0f;
		}();
		if (jumped) c.emitting = false;           // a jump (teleport, load): start a new strand
		// A burst's cluster: points leaving the same spot at different speeds and angles, linked one after the other,
		// so they open up into one irregular cloud; otherwise one point every `spacing` seconds.
		int emit = src.cluster;
		if ((!c.emitting || SmokeClock - c.lastEmit >= spacing) && emit < 1) emit = 1;
		// A strand from a fast-moving gun: a point every maxGap units too, so it stays smooth instead of a few long pieces
		// whipping round (the newest point rides on the source; the one before it lies where the gun was).
		if (emit < 1 && src.maxGap > 0.0f && c.emitting && c.count >= 2) {
			const NiPoint3& q = c.p[c.count - 2].pos;
			const float dx = src.at.x - q.x, dy = src.at.y - q.y, dz = src.at.z - q.z;
			if (dx * dx + dy * dy + dz * dz > src.maxGap * src.maxGap) emit = 1;
		}
		for (int e = 0; e < emit; e++) { EmitSmoke(c, src, v); c.emitting = true; }
		// Points that have just left a source inside the barrel slide out along it over their first 0.1 s (following the gun),
		// so the strand starts hidden in the barrel and fresh smoke flows out of its end; then they are on their own.
		if (src.bore.x != 0.0f || src.bore.y != 0.0f || src.bore.z != 0.0f)
			for (int i = 0; i < c.count; i++) {
				const float age = SmokeClock - c.p[i].born;
				if (age >= 0.1f) continue;
				const float k = age / 0.1f;
				c.p[i].pos = NiPoint3{ src.at.x + src.bore.x * k, src.at.y + src.bore.y * k, src.at.z + src.bore.z * k };
			}
		// Keep the newest point on the source, so the strand stays attached to the barrel between emissions.
		if (c.count) c.p[c.count - 1].pos = src.at;
	}
	c.emitting = src.on;
}

// How much of a segment's rounded end is drawn past a joint (0..1). A strand's free end is fully rounded; at a joint the
// neighbouring segment's body already covers the straight continuation, so only the wedge on the outside of a bend needs
// filling (more the sharper the bend). A neighbour shorter than the tube's radius covers little, so the end stays round;
// a segment shorter than its radius lies inside its neighbour, so its end adds nothing.
// old -> nw is the segment; other is the neighbour's far point (past nw, or before old when atOld).
static float EndWeight(const NiPoint3& old, const NiPoint3& nw, const NiPoint3& other, float r, bool atOld = false) {
	const NiPoint3 self = { nw.x - old.x, nw.y - old.y, nw.z - old.z };
	const NiPoint3 next = atOld ? NiPoint3{ old.x - other.x, old.y - other.y, old.z - other.z }
	                            : NiPoint3{ other.x - nw.x, other.y - nw.y, other.z - nw.z };
	const float ls = sqrtf(self.x * self.x + self.y * self.y + self.z * self.z);
	const float ln = sqrtf(next.x * next.x + next.y * next.y + next.z * next.z);
	r = r > 0.05f ? r : 0.05f;
	const float cover = ln / r < 1.0f ? ln / r : 1.0f;
	const float inside = ls / r < 1.0f ? ls / r : 1.0f;
	float fill = 0.0f;
	if (ls > 0.001f && ln > 0.001f) fill = 0.5f * (1.0f - (self.x * next.x + self.y * next.y + self.z * next.z) / (ls * ln));
	return 1.0f - cover * (1.0f - fill * inside);
}

// Whether any smoke is near your body (within 40 units of its edge, at a height you occupy, give or take 20).
static bool SmokeNearBody() {
	const NiPoint3& f = VolumePlayerFeet;
	const float reach = Settings.volumePlayerRadius + 40.0f;
	for (int k = 0; k < ChainCount + 2; k++) {
		const SmokeChain& c = k < ChainCount ? Chains[k] : Twins[k - ChainCount];
		for (int i = 0; i < c.count; i++) {
			const NiPoint3& p = c.p[i].pos;
			if (p.z < f.z - 20.0f || p.z > f.z + Settings.volumePlayerHeight + 20.0f) continue;
			const float dx = p.x - f.x, dy = p.y - f.y;
			if (dx * dx + dy * dy < reach * reach) return true;
		}
	}
	return false;
}

// Whether any smoke is within 15 units of the barrel (whirlpools are only shed into smoke).
static bool SmokeNearGun() {
	const NiPoint3& m = VolumeGunMuzzle;
	const NiPoint3& b = VolumeGunBack;
	const float L = Settings.volumeGunLength;
	for (int k = 0; k < ChainCount + 2; k++) {
		const SmokeChain& c = k < ChainCount ? Chains[k] : Twins[k - ChainCount];
		for (int i = 0; i < c.count; i++) {
			const NiPoint3& p = c.p[i].pos;
			float al = (p.x - m.x) * b.x + (p.y - m.y) * b.y + (p.z - m.z) * b.z;
			al = al < 0.0f ? 0.0f : al > L ? L : al;
			const float dx = p.x - (m.x + b.x * al), dy = p.y - (m.y + b.y * al), dz = p.z - (m.z + b.z * al);
			if (dx * dx + dy * dy + dz * dz < 225.0f) return true;
		}
	}
	return false;
}

// ---- Gun-smoke haze: a room fills with smoke as you keep firing -------------------------------------------------------
// Every shot leaves some haze where its puff dies out: a big, thin blob of smoke that grows, rises slowly and lingers.
// Sustained fire builds up a cloud; indoors it gathers under the ceiling and hangs for fHazeLife seconds, outdoors the
// wind carries it off and it clears in under half that. A shot thickens the nearest young blob it lands in, up to four
// shots' worth; once that blob is full the next shot starts a new one, so firing from one spot keeps stacking haze there.
// At most HazeMax blobs (NVR draws them all in one pass at quarter resolution, GunFX_GetSmokeHaze: a room full costs about
// 0.2 ms); with that many, a shot thickens the nearest blob up to twice as far. They move like the rest of the smoke (your
// body and the gun stir and part them). [Volume] fHaze: how much each shot leaves (0 = none), fHazeLife (seconds indoors),
// fHazeSize (a blob's radius when grown, game units).
static const int HazeMax = 40;
static SmokeChain Haze = {};
static void UpdateHaze(float dt) {
	VolLook look = {};
	look.size = Settings.volumeHazeSize / 1.6f;               // EmitSmoke widens by fExpand (1.6 by default) to its full size
	look.sizeVar = 0.5f;
	look.life = Settings.volumeHazeLife * (VolumeIndoors ? 1.0f : 0.4f);
	look.life = look.life > 3.0f ? look.life : 3.0f;
	look.lifeVar = 0.25f * look.life;
	look.speed = 3.0f; look.speedVar = 2.0f; look.spread = 3.1416f;   // a little, any way
	look.startSize = 0.25f;
	look.grow = 0.4f * look.life;
	look.opacity = 0.012f * Settings.volumeHaze;
	look.fadeIn = 3.0f / look.life;                            // builds up over about 3 s as the puff dies out
	look.fadeStart = 0.5f; look.fadeEnd = 1.0f;
	look.rise = 1.5f;
	look.curl = 0.0003f;
	SmokeSource src = {};
	src.strength = 1.0f; src.size = 1.0f; src.points = 1.0f; src.drag = 1.0f; src.haze = true;
	const float density = Settings.volumeDensity > 0.0f ? Settings.volumeDensity : 1.0f;
	while (HazeShots > 0) {
		HazeShots--;
		if (Settings.volumeHaze <= 0.0f || !VolumeGunOn) continue;
		// Where the puff dies out: 20 units ahead of the muzzle, a little higher.
		const NiPoint3& b = VolumeGunBack;
		const NiPoint3 at = { VolumeGunMuzzle.x - b.x * 20.0f, VolumeGunMuzzle.y - b.y * 20.0f, VolumeGunMuzzle.z - b.z * 20.0f + 5.0f };
		// The nearest blob it lands in that is young enough and not yet full, else a new one if there is room, else (a full
		// room) the nearest blob at all.
		const float add = look.opacity * density, cap = 4.0f * add;
		int best = -1, nearest = -1;
		float bestDist = 1e9f, nearestDist = 1e9f;
		for (int i = 0; i < Haze.count; i++) {
			const SmokePoint& s = Haze.p[i];
			const float age = SmokeClock - s.born;
			const float dx = s.pos.x - at.x, dy = s.pos.y - at.y, dz = s.pos.z - at.z, d = sqrtf(dx * dx + dy * dy + dz * dz);
			if (d < nearestDist) { nearest = i; nearestDist = d; }
			if (age > 0.6f * s.life || s.opacity >= cap * 0.999f) continue;
			if (d < bestDist && d < 0.7f * PointRadius(s, age)) { best = i; bestDist = d; }
		}
		if (best >= 0) {                                       // thicker, up to four shots' worth
			SmokePoint& s = Haze.p[best];
			s.opacity = s.opacity + add < cap ? s.opacity + add : cap;
		}
		else if (Haze.count >= HazeMax && nearest >= 0) {      // a full room: thicker still where you fire
			SmokePoint& s = Haze.p[nearest];
			s.opacity = s.opacity + add < 2.0f * cap ? s.opacity + add : 2.0f * cap;
		}
		else if (Haze.count < HazeMax) {
			src.at = at;
			src.push = NiPoint3{ -b.x * 6.0f, -b.y * 6.0f, -b.z * 6.0f };   // drifts on forward a little
			Haze.emitting = false;                                       // lone blobs, not a strand
			EmitSmoke(Haze, src, look);
			Haze.p[Haze.count - 1].link = false;
		}
	}
	if (!Haze.count) return;
	UpdateChain(Haze, src, dt, look);                          // src.on is false: moves them, makes none
	for (int i = 0; i < Haze.count; i++) {
		SmokePoint& s = Haze.p[i];
		s.link = false;
		// Stirring spreads a blob less (it is already wide; it thins instead).
		if (s.spread > 1.6f) s.spread = 1.6f;
		// Indoors it gathers under the ceiling: rising slows from 160 units above your feet and stops at 250.
		if (VolumeIndoors && VolumePlayerOn && s.vel.z > 0.0f) {
			float k = (s.pos.z - VolumePlayerFeet.z - 160.0f) / 90.0f;
			k = k < 0.0f ? 0.0f : k > 1.0f ? 1.0f : k;
			s.vel.z *= 1.0f - k;
		}
	}
	// Gone when their time is up, wherever they are in the list (a thickened blob can outlast older ones).
	int keep = 0;
	for (int i = 0; i < Haze.count; i++)
		if (SmokeClock - Haze.p[i].born <= Haze.p[i].life) Haze.p[keep++] = Haze.p[i];
	Haze.count = keep;
}

static void UpdateVolumeSmoke(float dt, const SmokeSource sources[ChainCount]) {
	SmokeClock += dt;
	UpdateAirAndPlayer(dt);
	ShedVortices(dt);
	for (int k = 0; k < ChainCount; k++) UpdateChain(Chains[k], sources[k], dt, Settings.volLook[k]);
	for (int z = 0; z < 2; z++) {                 // the second arms move like their strands
		if (!Twins[z].count) continue;
		SmokeSource ts = {};
		ts.strength = 1.0f; ts.size = 1.0f; ts.points = 30.0f; ts.drag = Settings.volumeDrag; ts.drift = sources[z].drift; ts.frays = true;
		UpdateChain(Twins[z], ts, dt, Settings.volLook[z]);
	}
	for (int t = 0; t < TendrilMax; t++) {
		if (!Tendrils[t].count) continue;
		SmokeSource ts = {};
		ts.strength = 1.0f; ts.size = 1.0f; ts.points = 30.0f; ts.drag = 1.5f; ts.drift = TendrilDrift[t];
		UpdateChain(Tendrils[t], ts, dt, TendrilLook[t]);
	}
	UpdateHaze(dt);
	// Sharp bends in the heat smoke and trail fray into tendrils (each point once, while young enough to look fresh), while
	// the gun really moves (a swing, a turn, walking): firing in place, its kicks lay the strand in a small zigzag that
	// would otherwise throw tendrils out at random angles. Only bends longer on both sides than the smoke is thick count
	// (a smaller kink does not show in the drawn tube).
	float gunMoving;
	GunSpeed(&gunMoving);
	if (Settings.volumeTendrils > 0.0f && gunMoving > 0.5f) {
		const float cosLimit = cosf(Settings.volumeTendrilAngle * 0.01745329f);
		for (int k = 0; k <= ChainTrail; k++) {
			SmokeChain& c = Chains[k];
			for (int i = 1; i + 1 < c.count; i++) {
				SmokePoint& s = c.p[i];
				if (s.frayed || !s.link || !c.p[i + 1].link) continue;
				const float age = SmokeClock - s.born;
				if (age < 0.15f || age > 0.6f * s.life) continue;
				const NiPoint3& p0 = c.p[i - 1].pos;
				const NiPoint3& p2 = c.p[i + 1].pos;
				const NiPoint3 a = { s.pos.x - p0.x, s.pos.y - p0.y, s.pos.z - p0.z };
				const NiPoint3 b = { p2.x - s.pos.x, p2.y - s.pos.y, p2.z - s.pos.z };
				const float la = sqrtf(a.x * a.x + a.y * a.y + a.z * a.z), lb = sqrtf(b.x * b.x + b.y * b.y + b.z * b.z);
				const float thick = PointRadius(s, age) > 0.5f ? PointRadius(s, age) : 0.5f;
				if (la < thick || lb < thick) continue;
				if ((a.x * b.x + a.y * b.y + a.z * b.z) / (la * lb) > cosLimit) continue;
				s.frayed = true;
				if (Random01() > Settings.volumeTendrils) continue;
				// Out of the bend: the old direction minus the new one.
				SpawnTendril(s, PointRadius(s, age), NiPoint3{ a.x / la - b.x / lb, a.y / la - b.y / lb, a.z / la - b.z / lb },
					Settings.volLook[k], sources[k].drift);
			}
		}
	}
	// Current radius and density of every point, then one record per segment (or lone point).
	static float out[((ChainCount + 2) * ChainMax + TendrilMax * TendrilPoints + HazeMax) * RecordFloats];
	static_assert(sizeof(out) == sizeof(VolumePublished), "the published records and their copy must match");
	static float hazeOut[HazeMax * HazeFloats];
	static_assert(sizeof(hazeOut) == sizeof(SmokeHazePublished), "the published haze and its copy must match");
	int n = 0, hazeN = 0;
	for (int k = 0; k < ChainCount + 2 + TendrilMax + 1; k++) {
		const SmokeChain& c = k < ChainCount ? Chains[k] : k < ChainCount + 2 ? Twins[k - ChainCount] : k < ChainCount + 2 + TendrilMax ? Tendrils[k - ChainCount - 2] : Haze;
		// Its texture (NVR draws it): the bursts (puff, ejection smoke) billowy, the strands (heat smoke, trail, their
		// second arms and the tendrils) fibrous. 0..15 each, packed above the end weights.
		const bool burst = k == ChainPuff || k == ChainEject;
		const float pillows = burst ? Settings.volumePillows : 0.0f, streaks = burst ? 0.0f : Settings.volumeStreaks;
		const float style = 16.0f * floorf((pillows < 0.0f ? 0.0f : pillows > 1.0f ? 1.0f : pillows) * 15.0f + 0.5f) +
			floorf((streaks < 0.0f ? 0.0f : streaks > 1.0f ? 1.0f : streaks) * 15.0f + 0.5f);
		float radius[ChainMax], density[ChainMax], age01[ChainMax];
		for (int i = 0; i < c.count; i++) {
			const SmokePoint& s = c.p[i];
			const float age = SmokeClock - s.born;
			const float a = age / s.life;
			float g = s.grow > 0.01f ? age / s.grow : 1.0f;
			g = g < 0.0f ? 0.0f : g > 1.0f ? 1.0f : g;
			g = 1.0f - (1.0f - g) * (1.0f - g);           // grows fast at first and slower later, like a puff billowing out
			float fade = 1.0f;
			if (a < s.fadeIn && s.fadeIn > 0.001f) fade = a / s.fadeIn;
			else if (a > s.fadeStart) fade = 1.0f - (a - s.fadeStart) / (s.fadeEnd - s.fadeStart > 0.001f ? s.fadeEnd - s.fadeStart : 0.001f);
			fade = fade < 0.0f ? 0.0f : fade > 1.0f ? 1.0f : fade;
			fade = fade * fade * (3.0f - 2.0f * fade);   // smooth in and out
			const float grown = s.r0 + (s.r1 - s.r0) * g;     // as it grows by itself
			const float r = grown * s.spread;                  // and wider where it was stirred (its opacity thinned to match, see Diffuse)
			radius[i] = r;
			// fOpacity x fDensityScale is about how much of the background the strand covers looking straight across
			// it where it starts (optical depth). As it widens it keeps that opacity, like the sprites, or thins out by
			// [Volume] fThinning (0 = not at all, 1 = the same smoke spread wider). The shader multiplies the density by
			// the path length through the tube (~1.77 r).
			const float spread = s.r0 > 0.01f && grown > s.r0 ? powf(s.r0 / grown, Settings.volumeThinning) : 1.0f;
			density[i] = s.opacity * s.strength * fade * spread / (1.7725f * (r > 0.05f ? r : 0.05f));
			// For the look, stirred and spread smoke counts as older: the shader eats into its edges more (wispier).
			const float look = a + 0.5f * (s.spread - 1.0f) + 0.4f * s.stir;
			age01[i] = look < 1.0f ? look : 1.0f;
		}
		if (&c == &Haze) {                                // the haze: lone blobs, drawn by NVR's haze pass
			for (int i = 0; i < c.count && hazeN < HazeMax; i++) {
				float* o = hazeOut + hazeN * HazeFloats;
				o[0] = c.p[i].pos.x; o[1] = c.p[i].pos.y; o[2] = c.p[i].pos.z; o[3] = radius[i];
				o[4] = density[i] * 1.7725f * (radius[i] > 0.05f ? radius[i] : 0.05f);   // back to the optical depth through it
				o[5] = o[6] = o[7] = 0.0f;
				if (o[4] > 0.0f) hazeN++;
			}
			continue;
		}
		for (int i = 0; i < c.count; i++) {
			const bool linked = c.p[i].link && i > 0;
			const bool followed = i + 1 < c.count && c.p[i + 1].link;
			if (!linked && followed) continue;            // the old end of the next segment
			const int j = linked ? i - 1 : i;
			const bool capB = linked && c.p[j].link && j > 0;
			float* o = out + n * RecordFloats;
			o[0] = c.p[i].pos.x; o[1] = c.p[i].pos.y; o[2] = c.p[i].pos.z; o[3] = radius[i];
			o[4] = c.p[j].pos.x; o[5] = c.p[j].pos.y; o[6] = c.p[j].pos.z; o[7] = radius[j];
			o[8] = density[i]; o[9] = density[j]; o[10] = 0.5f * (age01[i] + age01[j]);
			const float wA = followed ? EndWeight(c.p[j].pos, c.p[i].pos, c.p[i + 1].pos, radius[i]) : 1.0f;
			const float wB = capB ? EndWeight(c.p[j].pos, c.p[i].pos, c.p[j - 1].pos, radius[j], true) : 1.0f;
			o[11] = floorf(wA * 15.0f + 0.5f) + 16.0f * floorf(wB * 15.0f + 0.5f) + 256.0f * style;
			o[12] = c.p[i].tex; o[13] = c.p[j].tex; o[14] = age01[i]; o[15] = age01[j];
			n++;
		}
	}
	EnterCriticalSection(&HeatLock);
	memcpy(VolumePublished, out, n * RecordFloats * sizeof(float));
	VolumePublishedCount = n;
	memcpy(SmokeHazePublished, hazeOut, hazeN * HazeFloats * sizeof(float));
	SmokeHazePublishedCount = hazeN;
	VolumeParams[0] = Settings.volumeNoise;
	VolumeParams[1] = Settings.volumeBrightness;
	VolumeParams[2] = Settings.volumeShade;
	VolumeParams[3] = SmokeClock;
	VolumeParams[4] = Settings.volumeDebug ? 1.0f : 0.0f;
	VolumeParams[5] = Settings.volumeGunInFront ? 1.0f : 0.0f;
	VolumeParams[6] = Settings.volumeTint;
	VolumeParams[7] = Settings.volumeDepth;
	LeaveCriticalSection(&HeatLock);
	// Where the smoke is (every 2 s while there is any, 20 times per session), to compare with NVR's log.
	static float loggedAt = -10.0f;
	static int logged = 0;
	if (n && Player() && logged < 20 && SmokeClock - loggedAt >= 2.0f) {
		loggedAt = SmokeClock;
		logged++;
		const float* player = (const float*)((UInt8*)Player() + 0x30);
		char counts[200] = "";
		for (int k = 0; k < ChainCount; k++) {
			char one[48];
			sprintf_s(one, "%s%s %d%s", k ? ", " : "", ChainNames[k], Chains[k].count, sources[k].on ? " (smoking)" : "");
			strcat_s(counts, one);
		}
		int tendrils = 0;
		for (int t = 0; t < TendrilMax; t++) tendrils += Tendrils[t].count ? 1 : 0;
		char one[64];
		sprintf_s(one, ", twin arms %d + %d, tendrils %d", Twins[0].count, Twins[1].count, tendrils);
		strcat_s(counts, one);
		DebugLine("volumetric smoke: %d segment(s); points: %s; first segment end (%.1f, %.1f, %.1f) radius %.2f, density %.4f; "
			"player at (%.1f, %.1f, %.1f)", n, counts, out[0], out[1], out[2], out[3], out[8], player[0], player[1], player[2]);
	}
}

// For the log: what the volumetric smoke costs on the game's thread (everything above, per frame), averaged over 5 s
// while there is smoke, with the worst frame and how much smoke there was; 40 lines a session.
static void NoteVolumeTime(const LARGE_INTEGER& start) {
	static LARGE_INTEGER frequency = {};
	static double total = 0.0, worst = 0.0;
	static int frames = 0, logged = 0, mostPoints = 0, mostSegments = 0;
	static ULONGLONG since = 0;
	if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
	LARGE_INTEGER end;
	QueryPerformanceCounter(&end);
	int points = 0;
	for (int k = 0; k < ChainCount; k++) points += Chains[k].count;
	for (int z = 0; z < 2; z++) points += Twins[z].count;
	for (int t = 0; t < TendrilMax; t++) points += Tendrils[t].count;
	if (!points) { frames = 0; total = worst = 0.0; since = 0; return; }
	const double ms = (double)(end.QuadPart - start.QuadPart) * 1000.0 / (double)frequency.QuadPart;
	const ULONGLONG now = GetTickCount64();
	if (!since) since = now;
	total += ms; frames++;
	worst = ms > worst ? ms : worst;
	mostPoints = points > mostPoints ? points : mostPoints;
	mostSegments = VolumePublishedCount > mostSegments ? VolumePublishedCount : mostSegments;
	if (now - since < 5000) return;
	if (logged < 40) {
		logged++;
		DebugLine("volumetric smoke CPU: %.3f ms a frame on average, worst %.3f ms, over %d frames (up to %d points, %d segments)",
			total / frames, worst, frames, mostPoints, mostSegments);
	}
	total = worst = 0.0; frames = 0; mostPoints = mostSegments = 0; since = now;
}

static void MainLoop() {
	const ULONGLONG now = GetTickCount64();
	ReloadIniIfChanged(now);
	UpdateGunSettings();
	HandleShots(now);
	UpdateBursts(now);
	const bool heatVolume = VolumeWanted(ChainHeat), trailVolume = VolumeWanted(ChainTrail);   // instead of sprites
	// The frame's length from the precise clock (see PreciseSeconds): with the 15.6 ms tick the smoke stood still on one
	// frame in six (the newest point, pinned to the gun, stretched the strand) and moved 20-25 % too far on the others.
	// `now` stays for the coarse timers (delays after a shot, sprite lifetimes).
	const double nowSeconds = PreciseSeconds();
	const float dt = LastLoopSeconds > 0.0 ? (float)(nowSeconds - LastLoopSeconds) : 0.0f;
	LastLoopSeconds = nowSeconds;
	if (!Settings.trail || !Switch(1)) ReleaseWisp(nullptr);

	const float span = Settings.heatFull - Settings.heatStart > 0.01f ? Settings.heatFull - Settings.heatStart : 0.01f;
	float t = (Heat - Settings.heatStart) / span;
	t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
	float rate = Settings.maxRate * t * t * (3.0f - 2.0f * t);   // smooth start and finish
	if (Settings.constantRate > 0.0f) rate = Settings.constantRate;  // test switch: fixed rate, no heat
	// The heat smoke only runs while you fire: fStopDelay seconds after the last shot it stops (the smoke already in
	// the air drifts off) and the after-fire trail takes over. fStopDelay 0 = it keeps going until the barrel cools.
	const bool firing = Settings.wispStopDelay <= 0.0f || (LastShot && now - LastShot < (ULONGLONG)(Settings.wispStopDelay * 1000.0f));
	if (!firing && Settings.constantRate <= 0.0f) {
		rate = 0.0f;
		if (FollowEffect) ReleaseWisp("stopped firing");
	}
	// The shader rises over a sustained burst, independently of the smoke's heatFull.
	float glowT = (Heat - Settings.glowStart) / Settings.glowHeatSpan;
	glowT = glowT < 0.0f ? 0.0f : glowT > 1.0f ? 1.0f : glowT;
	glowT = glowT * glowT * (3.0f - 2.0f * glowT);

	const bool muzzleExists = TrailNode && TrailActor == Player();
	const bool drawn = muzzleExists && IsWeaponDrawn(Player());
	UInt8* root = drawn ? VisibleRoot() : nullptr;
	UInt8* liveNode = root && CurrentWeapon ? GetFireNode(CurrentWeapon, root) : nullptr;
	if (liveNode && liveNode != TrailNode) {
		NodeAddRef(liveNode);
		NodeRelease(TrailNode);
		TrailNode = liveNode;
	}
	if (liveNode) LastMuzzleSeen = now;
	if (liveNode) GlowBack = GlowDirection(liveNode);
	// An animation may briefly remove ProjectileNode; keep the held node through that gap.
	const bool muzzleAlive = drawn && TrailNode && (liveNode || now - LastMuzzleSeen < 250);
	// The game can end a temporary effect early (cell change, loading, too many effects); then it drops its reference.
	const bool dropped = FollowEffect && FollowHeld && *(volatile LONG*)(FollowEffect + 4) <= 1;
	if (FollowEffect && (!muzzleAlive || dropped || now > FollowUntil))
		ReleaseWisp(!muzzleExists ? "the muzzle left the scene" : !drawn ? "gun put away" : !muzzleAlive ? "the muzzle is not on the gun you see"
			: dropped ? "the game removed it" : "its time is up");
	if (TrailNode && !muzzleAlive) {
		NodeRelease(TrailNode);
		TrailNode = nullptr;
		TrailActor = nullptr;
	}
	if (heatVolume && FollowEffect) ReleaseWisp("volumetric smoke is on");
	if (Settings.trail && Switch(1) && rate > 0.01f && muzzleAlive && !heatVolume) {
		// A wisp close to its end hands over to a new one, so the smoke never stops mid-stream.
		if (FollowEffect && now + (ULONGLONG)(Settings.tailSeconds * 1000.0f) > FollowUntil) ReleaseWisp("handing over to a new one");
		if (!FollowEffect) SpawnWisp(now, NodePosition(SmokeAnchor(Settings.wispNode, TrailNode), Settings.wispOffset));
	}
	// The P66 object shader draws the heat on the gun itself; skip the old free-floating glow particles.
	if (FollowEffect) {
		UInt8* root = *(UInt8**)(FollowEffect + 0x18);
		if (root && Settings.trailFollow) {
			// A slow, smooth sideways sway of the source (two mixed waves per axis); the rising smoke keeps the path
			// it was born on, so the strand snakes as it climbs.
			NiPoint3 at = NodePosition(SmokeAnchor(Settings.wispNode, TrailNode), Settings.wispOffset);
			if (Settings.sway > 0.0f) {
				const float t = (float)(now % 3600000) / 1000.0f * Settings.swaySpeed * 6.2831853f;
				at.x += Settings.sway * (sinf(t) + 0.5f * sinf(t * 2.3f + 1.0f)) / 1.5f;
				at.y += Settings.sway * (sinf(t * 0.8f + 2.0f) + 0.5f * sinf(t * 1.9f)) / 1.5f;
			}
			PlaceCopies(WispCopies, root, RateValue, RateController, at, rate);
		}
		else if (RateValue) { *RateValue = rate; *(unsigned short*)(RateController + 0x08) |= 0x8; }
		if (root) ReportRibbon(root, now);
		else if (rate <= 0.01f) ReleaseWisp("cooled down (no rate control)");
	}
	// After-fire trail: from fStartDelay seconds after the last shot, while the barrel is still warm. Its rate follows
	// the heat that is left (full at fFullHeat, none below fMinHeat), so it thins out as the gun cools; firing again
	// stops it at once (the smoke already in the air drifts away).
	{
		const bool resting = LastShot && now - LastShot >= (ULONGLONG)(Settings.afterDelay * 1000.0f);
		const float afterSpan = Settings.afterFullHeat - Settings.afterMinHeat > 0.01f ? Settings.afterFullHeat - Settings.afterMinHeat : 0.01f;
		float a = (Heat - Settings.afterMinHeat) / afterSpan;
		a = a < 0.0f ? 0.0f : a > 1.0f ? 1.0f : a;
		const float afterRate = resting ? Settings.afterMaxRate * a * a * (3.0f - 2.0f * a) : 0.0f;
		const bool afterWanted = Settings.after && Switch(7) && muzzleAlive && GunAllowed(CurrentWeapon);
		const bool afterOn = afterWanted && !trailVolume;
		VolAfterRate = afterWanted ? afterRate : 0.0f;
		const bool afterDropped = AfterEffect && AfterHeld && *(volatile LONG*)(AfterEffect + 4) <= 1;
		if (AfterEffect && (!afterOn || afterDropped || now > AfterUntil || !resting))
			ReleaseAfter(!afterOn ? nullptr : afterDropped ? "the game removed it" : !resting ? "firing again" : "its time is up");
		if (afterOn && afterRate > 0.01f) {
			if (AfterEffect && now + (ULONGLONG)(Settings.tailSeconds * 1000.0f) > AfterUntil) ReleaseAfter("handing over to a new one");
			if (!AfterEffect) SpawnAfter(now, NodePosition(SmokeAnchor(Settings.wispNode, TrailNode), Settings.wispOffset));
		}
		if (AfterEffect) {
			UInt8* afterRoot = *(UInt8**)(AfterEffect + 0x18);
			if (afterRoot && TrailNode) {
				NiPoint3 at = NodePosition(SmokeAnchor(Settings.wispNode, TrailNode), Settings.wispOffset);
				if (Settings.afterSway > 0.0f) {
					const float w = (float)(now % 3600000) / 1000.0f * Settings.afterSwaySpeed * 6.2831853f;
					at.x += Settings.afterSway * (sinf(w * 1.1f + 0.7f) + 0.5f * sinf(w * 2.7f)) / 1.5f;
					at.y += Settings.afterSway * (sinf(w * 0.9f + 1.3f) + 0.5f * sinf(w * 2.1f + 0.4f)) / 1.5f;
				}
				PlaceCopies(AfterCopies, afterRoot, AfterRate, AfterController, at, afterRate);
			}
			else if (AfterRate) { *AfterRate = afterRate; *(unsigned short*)(AfterController + 0x08) |= 0x8; }
		}
	}
	// Volumetric smoke: the same decisions as the sprite smoke, drawn by NVR as continuous tubes.
	{
		LARGE_INTEGER volumeStart;
		QueryPerformanceCounter(&volumeStart);
		SmokeSource sources[ChainCount] = {};
		VolumeGunOn = false;
		ShapeOn = false;
		VolumeMuzzleDepth = 0.0f;
		if (muzzleAlive && TrailNode) {
			UInt8* anchor = SmokeAnchor(Settings.wispNode, TrailNode);
			const NiPoint3 muzzle = WorldPoint(anchor, NodePosition(anchor, Settings.wispOffset));
			// How the gun moves (a jump of over 25 units in a frame is a teleport or a view change: no speed from it).
			static NiPoint3 lastMuzzle = {};
			static bool haveMuzzle = false;
			if (haveMuzzle && dt > 0.0001f) {
				NiPoint3 v = { (muzzle.x - lastMuzzle.x) / dt, (muzzle.y - lastMuzzle.y) / dt, (muzzle.z - lastMuzzle.z) / dt };
				if (v.x * v.x + v.y * v.y + v.z * v.z > 1500.0f * 1500.0f) v = NiPoint3{};
				const float kv = 1.0f - expf(-dt / 0.3f);
				VolumeGunVel.x += (v.x - VolumeGunVel.x) * kv; VolumeGunVel.y += (v.y - VolumeGunVel.y) * kv; VolumeGunVel.z += (v.z - VolumeGunVel.z) * kv;
			}
			lastMuzzle = muzzle;
			haveMuzzle = true;
			VolumeGunMuzzle = muzzle;
			float off = 0.0f;
			VolumeGunBack = BarrelBack(TrailNode, &off);                     // toward the stock (unit length)
			static void* loggedGun = nullptr;
			if (CurrentWeapon != loggedGun) {
				loggedGun = CurrentWeapon;
				DebugLine("volumetric smoke: the barrel's direction from %s (the muzzle node's own axis and the line to its parent are %.0f degrees apart)",
					off <= 45.0f ? "the muzzle node's own axis" : "the line to the muzzle node's parent", off);
			}
			// The gun shape probe, once per gun, on the gun model GunFX follows (the one you see), naming which it is.
			static void* probedGun = nullptr;
			if (CurrentWeapon != probedGun) {
				probedGun = CurrentWeapon;
				UInt8* player = (UInt8*)Player();
				DebugLine("gun shape probe: the %s model's gun (the game's own third-person switch: %d)",
					InFirstPersonModel(TrailNode) ? "first-person" : "third-person", player ? (int)*(player + 0x64C) : -1);
				if (UInt8* gunRoot = *(UInt8**)(TrailNode + 0x18)) ProbeGunShape(gunRoot, TrailNode);
			}
			// The gun's real shape (built in the background when a gun is first seen), and where its model is this frame.
			UInt8* gunRoot = *(UInt8**)(TrailNode + 0x18);
			UpdateGunShape(gunRoot, TrailNode);
			ShapeOn = Shape && gunRoot && gunRoot == ShapeRoot;
			if (gunRoot && gunRoot == ShapeRoot) {
				memcpy(ShapeRot, gunRoot + 0x68, sizeof(ShapeRot));
				ShapePos = WorldPoint(TrailNode, *(NiPoint3*)(gunRoot + 0x8C));
				ShapeScale = *(float*)(gunRoot + 0x98) > 0.0001f ? *(float*)(gunRoot + 0x98) : 1.0f;
				// With the model read, its own long axis is the barrel's direction (every gun model tested lies along its
				// X axis, the muzzle at the far end; the muzzle node itself can be turned).
				if (ShapeAlongX) VolumeGunBack = NiPoint3{ -ShapeRot[0], -ShapeRot[3], -ShapeRot[6] };
			}
			// Where you aim, flat; looking (nearly) straight up or down it has no clear heading, so the last one is kept.
			static bool haveAim = false;
			const float fx = -VolumeGunBack.x, fy = -VolumeGunBack.y, fl = sqrtf(fx * fx + fy * fy);
			if (fl > 0.05f) {
				VolumeAimForward = NiPoint3{ fx / fl, fy / fl, 0.0f };
				VolumeAimRight = NiPoint3{ fy / fl, -fx / fl, 0.0f };        // 90 degrees clockwise seen from above
				haveAim = true;
			}
			VolumeGunOn = haveAim && (Settings.volumeWrapGun || Settings.volumeDriftRight != 0.0f || Settings.volumeDriftAway != 0.0f);
		}
		for (int k = 0; k < ChainCount; k++) {
			sources[k].drift = k <= ChainTrail && (Settings.volumeDriftRight != 0.0f || Settings.volumeDriftAway != 0.0f);
			sources[k].frays = true;
			sources[k].zip = k == ChainHeat ? 1 : k == ChainTrail ? 2 : 0;
			sources[k].strength = 1.0f;
			sources[k].size = k <= ChainTrail ? Settings.volumeSize : Settings.volumeBurstSize;
			sources[k].points = k <= ChainTrail ? 30.0f : 62.0f;
			sources[k].drag = k <= ChainTrail ? Settings.volumeDrag : Settings.volumeBurstDrag;
		}
		if ((heatVolume || trailVolume) && muzzleAlive && TrailNode) {
			UInt8* anchor = SmokeAnchor(Settings.wispNode, TrailNode);
			const NiPoint3 base = WorldPoint(anchor, NodePosition(anchor, Settings.wispOffset));
			NiPoint3 wispAt = base, trailAt = base;
			// From inside the barrel: the heat smoke and trail start a little way back inside it and slide out (UpdateChain),
			// so they flow out of the barrel's end instead of appearing in front of it (the muzzle node often sits a little
			// ahead of the barrel). With the gun's real shape, the end is found by feeling back along the barrel until points
			// 0.8 units off its axis are inside the gun (its wall), then 1.5 units further in; otherwise 1.5 units in.
			NiPoint3 boreOut = {};
			float atMuzzle = 1.0f;                                       // how much of the heat smoke still starts at the muzzle
			if (VolumeGunOn) {
				const NiPoint3& b = VolumeGunBack;
				float depth = 1.5f;
				if (ShapeOn) {
					NiPoint3 up = { -b.x * b.z, -b.y * b.z, 1.0f - b.z * b.z };
					const float ul = sqrtf(up.x * up.x + up.y * up.y + up.z * up.z);
					up = ul > 0.01f ? NiPoint3{ up.x / ul, up.y / ul, up.z / ul } : NiPoint3{ 1.0f, 0.0f, 0.0f };
					const NiPoint3 side = { b.y * up.z - b.z * up.y, b.z * up.x - b.x * up.z, b.x * up.y - b.y * up.x };
					for (float s = 0.0f; s <= 6.0f; s += 0.25f) {
						bool wall = false;
						for (int k = 0; k < 4 && !wall; k++) {
							const float u = k == 0 ? 0.8f : k == 1 ? -0.8f : 0.0f, w = k == 2 ? 0.8f : k == 3 ? -0.8f : 0.0f;
							NiPoint3 nn;
							wall = GunSurface(NiPoint3{ base.x + b.x * s + up.x * u + side.x * w, base.y + b.y * s + up.y * u + side.y * w,
								base.z + b.z * s + up.z * u + side.z * w }, &nn) < 0.0f;
						}
						if (wall) { depth = s + 1.5f; break; }
					}
				}
				VolumeMuzzleDepth = depth;
				boreOut = NiPoint3{ -b.x * depth, -b.y * depth, -b.z * depth };
				wispAt = trailAt = NiPoint3{ base.x + b.x * depth, base.y + b.y * depth, base.z + b.z * depth };
			}
			// The heat smoke also rises off the hot barrel: its source wanders slowly a share of the way back along it
			// ([Volume] fBarrelSpread of fGunLength; 0 = the muzzle only).
			if (VolumeGunOn && Settings.volumeBarrelSpread > 0.0f) {
				const float tb = (float)fmod(nowSeconds, 3600.0);
				const float wander = 0.5f + 0.5f * (0.6f * sinf(tb * 1.3f) + 0.4f * sinf(tb * 2.9f + 1.0f));
				const float back = Settings.volumeBarrelSpread * Settings.volumeGunLength * wander;
				wispAt.x += VolumeGunBack.x * back; wispAt.y += VolumeGunBack.y * back; wispAt.z += VolumeGunBack.z * back;
				// Off the barrel's top, not from inside it (where the gun's rule would shove it out as soon as the gun
				// moves): lifted to the edge of the gun's shape, gradually over the first 3 units back from the muzzle.
				if (Settings.volumeWrapGun) {
					const NiPoint3& b = VolumeGunBack;
					NiPoint3 up = { -b.x * b.z, -b.y * b.z, 1.0f - b.z * b.z };
					const float ul = sqrtf(up.x * up.x + up.y * up.y + up.z * up.z);
					if (ul > 0.01f) {
						const VolLook& hv = Settings.volLook[ChainHeat];
						const float r0 = hv.size * Settings.volumeSize * hv.startSize;
						float g = back / 3.0f;
						g = g > 1.0f ? 1.0f : g;
						float lift = Settings.volumeGunRadius + 0.6f * r0;
						if (ShapeOn) {
							// The real barrel's top: from the tube's edge down to the gun's surface (or up, where something
							// stands taller), in steps of 0.2 units.
							const float target = 0.6f * r0 + 0.1f;
							auto clear = [&](float h) {
								NiPoint3 nn;
								const float k = h / ul;
								return GunSurface(NiPoint3{ wispAt.x + up.x * k, wispAt.y + up.y * k, wispAt.z + up.z * k }, &nn) >= target;
							};
							if (clear(lift)) { while (lift > 0.2f && clear(lift - 0.2f)) lift -= 0.2f; }
							else for (int k = 0; k < 30 && !clear(lift); k++) lift += 0.2f;
						}
						atMuzzle = 1.0f - g * g * (3.0f - 2.0f * g);
						lift *= (1.0f - atMuzzle) / ul;
						wispAt.x += up.x * lift; wispAt.y += up.y * lift; wispAt.z += up.z * lift;
					}
				}
			}
			// The sprite smoke's sway of its source; not while it flows out of the barrel (it would come out through the wall).
			const float swayHeat = boreOut.x != 0.0f || boreOut.y != 0.0f || boreOut.z != 0.0f ? 1.0f - atMuzzle : 1.0f;
			const float swayTrail = boreOut.x != 0.0f || boreOut.y != 0.0f || boreOut.z != 0.0f ? 0.0f : 1.0f;
			const float t1 = (float)fmod(nowSeconds, 3600.0) * Settings.swaySpeed * 6.2831853f;
			wispAt.x += swayHeat * Settings.sway * (sinf(t1) + 0.5f * sinf(t1 * 2.3f + 1.0f)) / 1.5f;
			wispAt.y += swayHeat * Settings.sway * (sinf(t1 * 0.8f + 2.0f) + 0.5f * sinf(t1 * 1.9f)) / 1.5f;
			const float t2 = (float)fmod(nowSeconds, 3600.0) * Settings.afterSwaySpeed * 6.2831853f;
			trailAt.x += swayTrail * Settings.afterSway * (sinf(t2 * 1.1f + 0.7f) + 0.5f * sinf(t2 * 2.7f)) / 1.5f;
			trailAt.y += swayTrail * Settings.afterSway * (sinf(t2 * 0.9f + 1.3f) + 0.5f * sinf(t2 * 2.1f + 0.4f)) / 1.5f;
			sources[ChainHeat].on = heatVolume && Settings.trail && Switch(1) && rate > 0.01f;
			sources[ChainHeat].at = wispAt;
			sources[ChainHeat].bore = NiPoint3{ boreOut.x * atMuzzle, boreOut.y * atMuzzle, boreOut.z * atMuzzle };
			sources[ChainHeat].strength = Settings.maxRate > 0.01f ? rate / Settings.maxRate : 0.0f;
			sources[ChainTrail].on = trailVolume && VolAfterRate > 0.01f;
			sources[ChainTrail].at = trailAt;
			sources[ChainTrail].bore = boreOut;
			sources[ChainHeat].maxGap = sources[ChainTrail].maxGap = 8.0f;
			sources[ChainTrail].strength = Settings.afterMaxRate > 0.01f ? VolAfterRate / Settings.afterMaxRate : 0.0f;
		}
		// Puff and ejection bursts: from their node while they last (at least one frame), following it.
		for (int k = ChainPuff; k <= ChainEject; k++) {
			SmokeBurst& b = VolumeBursts[k];
			if (!b.node) continue;
			if (VolumeWanted(k) && (b.fresh || SmokeClock < b.until)) {
				sources[k].on = true;
				sources[k].at = WorldPoint(b.node, NodePosition(b.node, b.offset));
				sources[k].size = Settings.volumeBurstSize * b.size;
				sources[k].cluster = b.pending < ChainMax / 2 ? b.pending : ChainMax / 2;
				b.pending = 0;
				// The gas leaves with the gun's own movement (smoothed, so firing's kicks do not fling it) and then slows
				// in the air, so a gun you walk or run with does not run straight into its own fresh puff.
				if (VolumeGunOn) sources[k].carry = VolumeGunVel;
				if (k == ChainPuff) {   // forward out of the barrel (GlowDirection gives the rearward axis)
					const bool alongModel = ShapeAlongX && TrailNode && b.node == TrailNode && *(UInt8**)(TrailNode + 0x18) == ShapeRoot;
					const NiPoint3 back = alongModel ? VolumeGunBack : BarrelBack(b.node);
					const float f = Settings.volumePuffForward;
					sources[k].push = NiPoint3{ -back.x * f, -back.y * f, -back.z * f };
				}
				b.fresh = false;
			}
			else { NodeRelease(b.node); b.node = nullptr; b.pending = 0; }
		}
		UpdateVolumeSmoke(dt < 0.1f ? dt : 0.1f, sources);   // one step of at most 0.1 s (after a menu or a hitch)
		NoteVolumeTime(volumeStart);
	}
	if (now - LastShot >= (ULONGLONG)(Settings.coolDelay * 1000.0f)) Heat -= dt * Settings.coolPerSecond;
	if (Heat < 0.0f) Heat = 0.0f;
	// The slow footprint counts sustained fire; cooling it during a long burst
	// would make low-rate weapons unable to grow at all.
	if (now - LastShot >= 5000) SpreadHeat -= dt / Settings.glowSpreadCoolSeconds;
	if (SpreadHeat < 0.0f) SpreadHeat = 0.0f;
	if (!muzzleAlive) VisualHeat = 0.0f;
	else {
		const float step = dt < 0.05f ? dt : 0.05f;
		const float seconds = glowT > VisualHeat ? Settings.glowRiseSeconds : 2.0f;
		VisualHeat += (glowT - VisualHeat) * step / (seconds + step);
	}
	float hazeT = (Heat - Settings.hazeStart) / (Settings.hazeFull - Settings.hazeStart);
	hazeT = hazeT < 0 ? 0 : hazeT > 1 ? 1 : hazeT;
	hazeT = hazeT*hazeT*(3.0f-2.0f*hazeT);
	if (!muzzleAlive) VisualHaze = 0;
	else {
		const float step = dt < 0.05f ? dt : 0.05f;
		VisualHaze += (hazeT-VisualHaze)*step/(Settings.hazeRise+step);
	}
	PublishHeat(muzzleAlive, Settings.glowPreview ? 1.0f : VisualHeat);
}

// Diagnostic for hitches: the whole game-loop step's time (the volumetric part alone is in "volumetric smoke CPU"). A
// step over 4 ms is written down (the first 40), and every 5 s the average and worst (the first 60 times), so a long
// frame in NVR's log (FRAME SPIKE, "Game update (CPU)") can be checked against GunFX's own share of it.
static void TimedMainLoop() {
	const double start = PreciseSeconds();
	MainLoop();
	const double end = PreciseSeconds();
	const double ms = (end - start) * 1000.0;
	static double total = 0.0, worst = 0.0, windowStart = 0.0;
	static int frames = 0, slowLogged = 0, reports = 0;
	if (ms > 4.0 && slowLogged < 40) {
		slowLogged++;
		DebugLine("slow game-loop step: GunFX took %.1f ms this frame", ms);
	}
	total += ms;
	frames++;
	if (ms > worst) worst = ms;
	if (windowStart <= 0.0) windowStart = end;
	if (end - windowStart < 5.0) return;
	if (reports < 60) {
		reports++;
		DebugLine("GunFX per frame (whole step): %.3f ms on average, worst %.3f ms, over %d frames", total / frames, worst, frames);
	}
	total = worst = 0.0;
	frames = 0;
	windowStart = end;
}

// ---- Hooks -----------------------------------------------------------------------------------------------------
// Every shot of a ranged weapon goes through TESObjectWEAP::Fire, with or without a muzzle flash. The hook only notes
// the shot (one call comes from the game's task queue, possibly on another thread); the game loop handles it.
typedef void (__thiscall* FireFn)(void* weapon, void* actor);
static FireFn OriginalFire[3] = {};
static const UInt32 CallSites[3] = { 0x008BADE9, 0x0087BA71, 0x005DA5F8 };
struct PendingShot { UInt8* actor; void* weapon; bool eject; };
static PendingShot Pending[32];
static int PendingCount = 0;
static CRITICAL_SECTION PendingLock;
static void NoteShot(void* weapon, void* actor) {
	if (!weapon || !actor) return;
	EnterCriticalSection(&PendingLock);
	if (PendingCount < 32) Pending[PendingCount++] = PendingShot{ (UInt8*)actor, weapon, false };
	LeaveCriticalSection(&PendingLock);
}
// TESObjectWEAP::EjectShellCasing 0x00524DB0, thiscall (weapon, actor): the game throws out a casing. Called from
// TESObjectWEAP::Fire for automatic guns (0x00524A93, at the shot) and from the actor's animation when it reaches its
// eject point for other guns (0x008BB4C5: later than the shot on bolt-action, lever and pump guns).
static FireFn OriginalEject[2] = {};
static const UInt32 EjectSites[2] = { 0x00524A93, 0x008BB4C5 };
static void NoteEject(void* weapon, void* actor) {
	if (!weapon || !actor) return;
	EnterCriticalSection(&PendingLock);
	if (PendingCount < 32) Pending[PendingCount++] = PendingShot{ (UInt8*)actor, weapon, true };
	LeaveCriticalSection(&PendingLock);
}
static void __fastcall EjectHook0(void* weapon, void*, void* actor) { OriginalEject[0](weapon, actor); NoteEject(weapon, actor); }
static void __fastcall EjectHook1(void* weapon, void*, void* actor) { OriginalEject[1](weapon, actor); NoteEject(weapon, actor); }
static void __fastcall FireHook0(void* weapon, void*, void* actor) { OriginalFire[0](weapon, actor); NoteShot(weapon, actor); }
static void __fastcall FireHook1(void* weapon, void*, void* actor) { OriginalFire[1](weapon, actor); NoteShot(weapon, actor); }
static void __fastcall FireHook2(void* weapon, void*, void* actor) { OriginalFire[2](weapon, actor); NoteShot(weapon, actor); }
typedef UInt8* (__thiscall* GetProjectileNodeFn)(void* actor);
static const GetProjectileNodeFn GetProjectileNode = (GetProjectileNodeFn)0x008BE0A0;
// TESObjectWEAP::GetFireNode finds the weapon's fire node (its ProjectileNode) by name under a model.
static int LoggedShots = 0;

// True when a node belongs to the player's first-person model (PlayerCharacter firstPersonNiNode at +0x694;
// NiAVObject parent at +0x18).
static bool InFirstPersonModel(UInt8* node) {
	UInt8* firstPerson = *(UInt8**)((UInt8*)Player() + 0x694);
	for (int depth = 0; node && depth < 32; depth++, node = *(UInt8**)(node + 0x18)) if (node == firstPerson) return true;
	return false;
}

// A shot: smoke at the gun's ProjectileNode (for the player, in the model you see: first or third person). Guns only:
// TESObjectWEAP weapon type at +0xF4, 3..9 = pistols, rifles, automatics, energy weapons, handle weapons, launchers
// (not grenades, mines or thrown weapons).
static void OnShot(UInt8* actor, void* weapon, ULONGLONG now) {
	const UInt8 type = *((UInt8*)weapon + 0xF4);
	if (Settings.puffPlayerOnly && actor != Player()) return;
	if (!GunAllowed(weapon)) return;   // energy weapon with the EnergyWeapons switch off: no smoke, no heat
	if (type < 3 || type > 9) {
		static void* loggedSkipped[16] = {};
		static int skipped = 0;
		bool seen = false;
		for (int i = 0; i < skipped; i++) seen |= loggedSkipped[i] == weapon;
		if (!seen && skipped < 16) { loggedSkipped[skipped++] = weapon; LogLine("shot with weapon type %u: not a gun, no smoke", type); }
		return;
	}
	// The player: the gun's ProjectileNode in the model you see (the game's own answer is the third-person one, also
	// in first person). Others: the game's projectile node.
	UInt8* node = actor == Player() ? (VisibleRoot() ? GetFireNode(weapon, VisibleRoot()) : nullptr) : GetProjectileNode(actor);
	static void* loggedWeapons[64] = {};
	bool seen = false;
	for (int i = 0; i < LoggedShots; i++) seen |= loggedWeapons[i] == weapon;
	if (!seen && LoggedShots < 64) {
		if (actor == Player() && node && Settings.logWeaponNodes) {
			int budget = 64;
			UInt8* parent = *(UInt8**)(node + 0x18);
			LogNodeNames(parent ? parent : node, budget);
		}
		loggedWeapons[LoggedShots++] = weapon;
		const char* name = node ? *(const char**)(node + 0x08) : nullptr;
		DebugLine("first shot of a gun: weapon type %u%s, projectile node %s%s", type, (*((UInt8*)weapon + 0x100) & 0x2) ? " automatic" : "",
			node ? (name ? name : "(no name)") : "NOT found (no smoke)",
			!node || actor != Player() ? "" : InFirstPersonModel(node) ? " (first-person model)" : " (third-person model)");
	}
	if (!node) return;
	MuzzleFlash shot = {};
	shot.node = node;
	shot.sourceActor = actor;
	shot.sourceWeap = weapon;
	if (actor == Player()) {
		OnPlayerShot(&shot, now);
		MuzzleFlash smoke = shot;
		smoke.node = SmokeAnchor(Settings.puffNode, node);
		{   // its muzzle flash lights the volumetric smoke for a moment
			const NiPoint3 at = WorldPoint(smoke.node, NodePosition(smoke.node, Settings.puffOffset));
			EnterCriticalSection(&HeatLock);
			FlashWorld = at;
			FlashAt = PreciseSeconds();
			LeaveCriticalSection(&HeatLock);
			NoteBullet(at);                                    // and its bullet punches through the smoke
		}
		// Puff() handles the configured local offset.
		Puff(&smoke, now);
	} else Puff(&shot, now);
}

// A casing thrown out by the game: a small puff of smoke at the ejection port of the gun you see. The first ejections
// and every failure are written to the log (with the position next to the muzzle's, for comparison).
static int LoggedEjects = 0, LoggedEjectsView[2] = {};
// The game's own lookup (as in TESObjectWEAP::EjectShellCasing): BSUtilities::GetObjectByName (cdecl root, name) on
// PlayerCharacter::GetCurrentNode, the model in use (first or third person). The game passes its literal
// "ShellCasingNode" at 0x0102CB00; another name from the INI is passed as written.
typedef UInt8* (__cdecl* GetObjectByNameFn)(UInt8* root, const char* name);
static const GetObjectByNameFn GetObjectByName = (GetObjectByNameFn)0x004AAE30;
typedef UInt8* (__thiscall* GetCurrentNodeFn)(void* player);
static const GetCurrentNodeFn GetCurrentNode = (GetCurrentNodeFn)0x00950BE0;
static void OnEject(UInt8* actor, void* weapon, ULONGLONG now) {
	if (!Settings.eject || !Switch(2) || actor != Player() || !GunAllowed(weapon)) return;
	const UInt8 type = *((UInt8*)weapon + 0xF4);
	if (type < 3 || type > 9) return;
	static ULONGLONG lastEject = 0;
	if (now - lastEject < (ULONGLONG)(Settings.ejectInterval * 1000)) return;
	const char* name = !_stricmp(Settings.ejectNode, "ShellCasingNode") ? (const char*)0x0102CB00 : Settings.ejectNode;
	char hashed[136];
	sprintf_s(hashed, "##%s", Settings.ejectNode);
	UInt8* root = VisibleRoot();
	UInt8* port = root ? GetObjectByName(root, name) : nullptr;
	if (!port && root) port = GetObjectByName(root, hashed);
	if (!port) port = FindNamedNode(root, Settings.ejectNode);
	if (!port) {
		UInt8* current = GetCurrentNode(Player());
		if (current && current != root) { root = current; port = GetObjectByName(root, name); }
	}
	if (!port) {
		if (LoggedEjects < 40) { LoggedEjects++; LogLine("ejection: node %s NOT found on the %s model (no smoke)", Settings.ejectNode,
			*((UInt8*)Player() + 0x64C) ? "third-person" : "first-person"); }
		return;
	}
	lastEject = now;
	void* cell = *(void**)(actor + 0x40);
	const NiPoint3 at = WorldPoint(port, NodePosition(port, Settings.ejectOffset));
	// A hot gun vents more smoke: the burst grows from fScale (cold) to fScale x fHotScale at full barrel heat.
	float hot = Heat / (Settings.heatFull > 0.01f ? Settings.heatFull : 0.01f);
	hot = hot < 0.0f ? 0.0f : hot > 1.0f ? 1.0f : hot;
	const float scale = Settings.ejectScale * (1.0f + (Settings.ejectHotScale - 1.0f) * hot);
	const bool volume = VolumeWanted(ChainEject);
	if (volume) StartVolumeBurst(ChainEject, port, Settings.ejectOffset, Settings.ejectBurst, 1.0f + (Settings.ejectHotScale - 1.0f) * hot);
	void* effect = !volume && cell ? SpawnBurst(cell, Settings.ejectLifetime, Settings.ejectModel, at, scale, Settings.ejectBurst, Settings.ejectBurstRate, now,
		port, Settings.ejectOffset) : nullptr;
	const int view = InFirstPersonModel(port) ? 0 : 1;
	if (LoggedEjectsView[view] < 8 || (!effect && !volume)) {
		LoggedEjectsView[view]++;
		UInt8* muzzle = root ? GetFireNode(weapon, root) : nullptr;
		const NiPoint3 m = muzzle ? *(NiPoint3*)(muzzle + 0x8C) : NiPoint3{};
		DebugLine("ejection smoke at (%.1f, %.1f, %.1f) on the %s model, %.1f units from the muzzle; %s, scale %.2f, %.1f s -> %s",
			at.x, at.y, at.z, InFirstPersonModel(port) ? "first-person" : "third-person",
			muzzle ? sqrtf((at.x - m.x) * (at.x - m.x) + (at.y - m.y) * (at.y - m.y) + (at.z - m.z) * (at.z - m.z)) : -1.0f,
			Settings.ejectModel, scale, Settings.ejectLifetime, volume ? "volumetric" : effect ? "created" : (cell ? "NOT created" : "no cell"));
	}
}
static void HandleShots(ULONGLONG now) {
	PendingShot shots[32];
	EnterCriticalSection(&PendingLock);
	const int count = PendingCount;
	memcpy(shots, Pending, count * sizeof(PendingShot));
	PendingCount = 0;
	LeaveCriticalSection(&PendingLock);
	for (int i = 0; i < count; i++) {
		if (shots[i].eject) OnEject(shots[i].actor, shots[i].weapon, now);
		else OnShot(shots[i].actor, shots[i].weapon, now);
	}
}

// Replaces the call at a call site, keeping whatever it called before (another plugin's hook included).
static bool HookCall(UInt32 site, void* hook, FireFn* original) {
	UInt8* p = (UInt8*)site;
	if (p[0] != 0xE8) { LogLine("call site %08X is not a call (byte %02X); skipped", site, p[0]); return false; }
	*original = (FireFn)(site + 5 + *(int*)(p + 1));
	DWORD old;
	VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
	*(int*)(p + 1) = (int)((UInt32)hook - (site + 5));
	VirtualProtect(p, 5, old, &old);
	LogLine("hooked the weapon fire call at %08X (was %08X)", site, (UInt32)*original);
	return true;
}

// ---- Settings --------------------------------------------------------------------------------------------------
// ---- Per-gun settings ------------------------------------------------------------------------------------------
// Each gun gets Data\NVSE\Plugins\GunFX\Weapons\<gun name>.ini the first time it is equipped. Its lines start
// with ';' (= use the main GunFX.ini); a line without ';' gives that gun its own value. While a gun is equipped,
// every setting is read from its file first, then from the main INI. [Main] always comes from the main INI.
// 2026-11-16: the main value of a GunFX.ini key now comes from Vegas Shaders' settings ([Main.GunFX.<section>], GunFXKeys.h maps
// the names); an effect's bEnabled reads 1 there, since its menu switch decides (a per-gun file can still turn it off for one gun).
// False: no such setting (the code's own fallback stands). The replay rig (GUNFX_REPLAY) still reads GunFX.ini.
static bool MainValue(const char* section, const char* key, char* out, DWORD size) {
#ifndef GUNFX_REPLAY
	if (GunFXKeys::SwitchOf(section, key)) { strcpy_s(out, size, "1"); return true; }
	return GunFXHost::GetValue(section, key, out, size);
#else
	GetPrivateProfileStringA(section, key, "\x01", out, size, IniPath);
	return out[0] != 1;
#endif
}
static UINT LayerInt(const char* section, const char* key, INT fallback, const char* path) {
	if (WeaponIniPath[0] && path == IniPath && _stricmp(section, "Main")) {
		char value[64];
		GetPrivateProfileStringA(section, key, "\x01", value, sizeof(value), WeaponIniPath);
		if (value[0] != 1) return GetPrivateProfileIntA(section, key, fallback, WeaponIniPath);
	}
	char value[64];
	return MainValue(section, key, value, sizeof(value)) ? (UINT)atoi(value) : (UINT)fallback;   // (GetPrivateProfileInt reads the same way)
}
static DWORD LayerStr(const char* section, const char* key, const char* fallback, char* out, DWORD size, const char* path) {
	if (WeaponIniPath[0] && path == IniPath && key && _stricmp(section, "Main")) {
		const DWORD n = GetPrivateProfileStringA(section, key, "\x01", out, size, WeaponIniPath);
		if (out[0] != 1) return n;
	}
	if (!key || !MainValue(section, key, out, size)) strcpy_s(out, size, fallback);
	return (DWORD)strlen(out);
}
#ifndef GUNFX_REPLAY
// Every main value in one string, to see whether the GunFX settings changed (the settings revision changes with any setting).
static std::string SettingsPrint() {
	std::string print;
	char value[MAX_PATH];
	for (const GunFXKeys::Key& k : GunFXKeys::kKeys) {
		if (!MainValue(k.Section, k.Ini, value, sizeof(value))) value[0] = 0;
		print += value;
		print += '\x1f';
	}
	return print;
}
#endif

static float IniFloat(const char* section, const char* key, const char* fallback) {
	char buffer[64];
	LayerStr(section, key, fallback, buffer, sizeof(buffer), IniPath);
	return (float)atof(buffer);
}
static void IniModel(const char* section, const char* key, const char* fallback, char* out) {
	LayerStr(section, key, fallback, out, MAX_PATH, IniPath);
	for (char* c = out; *c; c++) if (*c == '/') *c = 0x5C; // the game's archives expect backslashes
	if (!_strnicmp(out, "BarrelSmoke\\", 12)) {      // older setting: the effects now live in Meshes\GunFX
		char renamed[MAX_PATH];
		sprintf_s(renamed, "GunFX\\%s", out + 12);
		strcpy_s(out, MAX_PATH, renamed);
	}
}

static void ReadLook(const char* section, const Look& file, Look& look) {
	char fallback[32];
	auto get = [&](const char* key, float value) { sprintf_s(fallback, "%g", value); return IniFloat(section, key, fallback); };
	look = file;
	look.strength = -get("fRise", -file.strength);            // upward push (positive rises)
	look.turbulence = get("fCurl", file.turbulence);
	look.turbulenceScale = get("fCurlScale", file.turbulenceScale);
	look.radius = get("fSize", file.radius);
	look.radiusVar = get("fSizeVariation", file.radiusVar);
	look.life = get("fSmokeLife", file.life);
	look.lifeVar = get("fSmokeLifeVariation", file.lifeVar);
	look.grow = get("fGrowSeconds", file.grow);
	look.fade = get("fShrinkSeconds", file.fade);
	look.opacity = get("fOpacity", file.opacity);
	look.percents[3] = get("fFadeIn", file.percents[3]);       // share of its life when a puff is fully visible
	if (look.percents[2] > look.percents[3]) look.percents[2] = look.percents[3];
	look.percents[4] = get("fFadeStart", file.percents[4]);    // share of its life when a puff starts fading out
	look.percents[5] = get("fFadeEnd", file.percents[5]);      // ...and is fully gone
}

// ---- Smoke look from the INI, baked into a copy of the effect file ------------------------------------------------
// Writing into live particle modifiers crashed the game, so the look is never changed at run time. Instead, when the
// settings load, the effect file is copied to Data\Meshes\GunFX\Generated\<name>_<hash>.nif with the INI's look values
// written into it, and that copy is spawned. The hash covers every value, so each look gets its own file and the
// game's model cache never hands back an older one. Keys that are missing keep the file's own values; when nothing
// differs, the original file is used. NIF 20.2.0.7 layout (as in tools\niftweak.py): emitter radius/variation/life/
// variation at +53/+57/+61/+65, speed/variation +13/+17, declination variation +25; grow/fade modifier grow +13,
// fade +19, base (start) scale +25; gravity strength +33, turbulence
// +41, turbulence scale +45; simple colour modifier six shares of life at +13, middle colour's alpha at +65.
struct SmokeLook { float size, sizeVar, life, lifeVar, grow, shrink, rise, curl, curlScale, opacity, fadeIn, fadeStart, fadeEnd,
	startSize, speed, speedVar, spread; };   // grow/fade base scale; emitter speed, its variation, declination variation
static bool ReadWholeFile(const char* path, std::vector<UInt8>& out) {
	FILE* f = nullptr;
	if (fopen_s(&f, path, "rb") || !f) return false;
	fseek(f, 0, SEEK_END);
	const long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	out.resize(size > 0 ? size : 0);
	const bool ok = size > 0 && fread(out.data(), 1, size, f) == (size_t)size;
	fclose(f);
	return ok;
}
// Block offsets and type names of a NIF file. False when the layout is not the expected one.
static bool NifBlocks(const std::vector<UInt8>& d, std::vector<std::string>& types, std::vector<size_t>& at, std::vector<UInt32>& sizes) {
	size_t p = 0;
	while (p < d.size() && d[p] != '\n') p++;
	p++;
	auto need = [&](size_t n) { return p + n <= d.size(); };
	if (!need(4 + 1 + 4 + 4 + 4)) return false;
	p += 4 + 1;
	p += 4;                                   // user version
	const UInt32 blocks = *(const UInt32*)&d[p]; p += 4;
	p += 4;                                   // user version 2
	for (int i = 0; i < 3; i++) { if (!need(1)) return false; p += 1 + d[p]; }
	if (!need(2)) return false;
	const unsigned short typeCount = *(const unsigned short*)&d[p]; p += 2;
	std::vector<std::string> names;
	for (unsigned short i = 0; i < typeCount; i++) {
		if (!need(4)) return false;
		const UInt32 n = *(const UInt32*)&d[p]; p += 4;
		if (!need(n)) return false;
		names.emplace_back((const char*)&d[p], n); p += n;
	}
	if (blocks > 4096 || !need(blocks * 6)) return false;
	std::vector<unsigned short> index(blocks);
	for (UInt32 i = 0; i < blocks; i++) { index[i] = *(const unsigned short*)&d[p]; p += 2; }
	sizes.resize(blocks);
	for (UInt32 i = 0; i < blocks; i++) { sizes[i] = *(const UInt32*)&d[p]; p += 4; }
	if (!need(8)) return false;
	const UInt32 strings = *(const UInt32*)&d[p]; p += 8;
	for (UInt32 i = 0; i < strings; i++) {
		if (!need(4)) return false;
		const UInt32 n = *(const UInt32*)&d[p]; p += 4 + n;
	}
	if (!need(4)) return false;
	const UInt32 groups = *(const UInt32*)&d[p]; p += 4 + 4 * groups;
	types.clear(); at.clear();
	for (UInt32 i = 0; i < blocks; i++) {
		if (index[i] >= names.size() || p + sizes[i] > d.size()) return false;
		types.push_back(names[index[i]]);
		at.push_back(p);
		p += sizes[i];
	}
	return true;
}
static float& NifFloat(std::vector<UInt8>& d, size_t at) { return *(float*)&d[at]; }
// Reads (get=true) or writes the look of every particle system in the file. Returns how many parts were found.
static int NifLook(std::vector<UInt8>& d, SmokeLook& look, bool get) {
	std::vector<std::string> types; std::vector<size_t> at; std::vector<UInt32> sizes;
	if (!NifBlocks(d, types, at, sizes)) return 0;
	int parts = 0;
	auto field = [&](size_t offset, float& value) { if (get) value = NifFloat(d, offset); else NifFloat(d, offset) = value; };
	for (size_t i = 0; i < types.size(); i++) {
		const std::string& t = types[i];
		const size_t q = at[i];
		if ((t == "NiPSysCylinderEmitter" || t == "NiPSysSphereEmitter" || t == "NiPSysBoxEmitter") && sizes[i] >= 69) {
			field(q + 53, look.size); field(q + 57, look.sizeVar); field(q + 61, look.life); field(q + 65, look.lifeVar);
			field(q + 13, look.speed); field(q + 17, look.speedVar); field(q + 25, look.spread); parts++;
		}
		else if (t == "NiPSysGrowFadeModifier" && sizes[i] >= 29) { field(q + 13, look.grow); field(q + 19, look.shrink); field(q + 25, look.startSize); parts++; }
		else if (t == "NiPSysGravityModifier" && sizes[i] >= 49) {
			float strength = -look.rise;
			field(q + 33, strength);
			if (get) look.rise = -strength;
			field(q + 41, look.curl); field(q + 45, look.curlScale); parts++;
		}
		else if (t == "BSPSysSimpleColorModifier" && sizes[i] >= 85) {
			float shares[6];
			for (int k = 0; k < 6; k++) shares[k] = NifFloat(d, q + 13 + 4 * k);
			if (get) { look.fadeIn = shares[3]; look.fadeStart = shares[4]; look.fadeEnd = shares[5]; look.opacity = NifFloat(d, q + 65); }
			else {
				NifFloat(d, q + 13 + 4 * 3) = look.fadeIn;
				if (shares[2] > look.fadeIn) NifFloat(d, q + 13 + 4 * 2) = look.fadeIn;   // colour 1 must end before colour 2 starts
				NifFloat(d, q + 13 + 4 * 4) = look.fadeStart;
				NifFloat(d, q + 13 + 4 * 5) = look.fadeEnd;
				NifFloat(d, q + 65) = look.opacity;
			}
			parts++;
		}
	}
	return parts;
}
// The model for a section: the file itself, or a generated copy with the INI's look. `model` is updated in place.
static void BakeLook(const char* section, char* model) {
	char data[MAX_PATH];                                       // ...\Data\ from ...\Data\NVSE\Plugins\GunFX.ini
	strcpy_s(data, IniPath);
	for (int up = 0; up < 3; up++) { char* cut = strrchr(data, 0x5C); if (cut) *cut = 0; }
	char source[MAX_PATH];
	sprintf_s(source, "%s\\Meshes\\%s", data, model);
	std::vector<UInt8> file;
	if (!ReadWholeFile(source, file)) { LogLine("%s look: %s is not a loose file, its own look is used", section, model); return; }
	SmokeLook built = {};
	if (NifLook(file, built, true) < 4) { LogLine("%s look: %s has an unexpected layout, its own look is used", section, model); return; }
	SmokeLook look = built;
	char fallback[32];
	auto get = [&](const char* key, float value) { sprintf_s(fallback, "%g", value); return IniFloat(section, key, fallback); };
	look.size = get("fSize", built.size);              look.sizeVar = get("fSizeVariation", built.sizeVar);
	look.life = get("fSmokeLife", built.life);         look.lifeVar = get("fSmokeLifeVariation", built.lifeVar);
	look.grow = get("fGrowSeconds", built.grow);       look.shrink = get("fShrinkSeconds", built.shrink);
	look.rise = get("fRise", built.rise);              look.curl = get("fCurl", built.curl);
	look.curlScale = get("fCurlScale", built.curlScale);
	look.opacity = get("fOpacity", built.opacity);     look.fadeIn = get("fFadeIn", built.fadeIn);
	look.fadeStart = get("fFadeStart", built.fadeStart); look.fadeEnd = get("fFadeEnd", built.fadeEnd);
	look.startSize = get("fStartSize", built.startSize);
	look.speed = get("fSpeed", built.speed);           look.speedVar = get("fSpeedVariation", built.speedVar);
	look.spread = get("fSpread", built.spread);
	const float* a = (const float*)&built; const float* b = (const float*)&look;
	int changed = 0;
	for (size_t k = 0; k < sizeof(SmokeLook) / sizeof(float); k++) changed += fabsf(a[k] - b[k]) > 1e-5f;
	if (!changed) { DebugLine("%s look: %s as built", section, model); return; }
	UInt32 hash = 2166136261u;                                 // FNV-1a over the model name and the values
	for (const char* c = model; *c; c++) hash = (hash ^ (UInt8)tolower(*c)) * 16777619u;
	for (size_t k = 0; k < sizeof(SmokeLook); k++) hash = (hash ^ ((const UInt8*)&look)[k]) * 16777619u;
	char base[MAX_PATH];
	const char* slash = strrchr(model, 0x5C);
	strcpy_s(base, slash ? slash + 1 : model);
	if (char* dot = strrchr(base, '.')) *dot = 0;
	char generated[MAX_PATH], folder[MAX_PATH], target[MAX_PATH];
	sprintf_s(generated, "GunFX\\Generated\\%s_%08x.nif", base, hash);
	sprintf_s(folder, "%s\\Meshes\\GunFX\\Generated", data);
	sprintf_s(target, "%s\\Meshes\\%s", data, generated);
	if (GetFileAttributesA(target) == INVALID_FILE_ATTRIBUTES) {
		NifLook(file, look, false);
		CreateDirectoryA(folder, NULL);
		FILE* f = nullptr;
		if (fopen_s(&f, target, "wb") || !f) { LogLine("%s look: could not write %s, its own look is used", section, generated); return; }
		fwrite(file.data(), 1, file.size(), f);
		fclose(f);
	}
	DebugLine("%s look: %d value(s) from the settings -> %s", section, changed, generated);
	strcpy_s(model, MAX_PATH, generated);
}

static void LoadSettings() {
	GetModuleFileNameA((HMODULE)&__ImageBase, IniPath, MAX_PATH);
	char* slash = strrchr(IniPath, 0x5C);
	strcpy_s(slash ? slash + 1 : IniPath, MAX_PATH - (slash ? (slash + 1 - IniPath) : 0), "GunFX.ini");

	// (IniPath stays: the per-gun folder and Data/Meshes are found from it.)
#ifdef GUNFX_REPLAY
	char defaults[MAX_PATH];
	sprintf_s(defaults, "%s.defaults", IniPath);
	int migrated = MergeIniDefaults(defaults, IniPath);
	if (migrated > 0) LogLine("bundled INI defaults applied (created file or added %d missing keys)", migrated);
	if (migrated < 0) LogLine("INI defaults migration incomplete; check file permissions or configuration size");
#else
	LoadedPrint = SettingsPrint();
#endif
	Settings.debugLog = LayerInt("Main", "bDebugLog", 0, IniPath);
	Settings.puff = LayerInt("Puff", "bEnabled", 1, IniPath);
	IniModel("Puff", "sModel", "GunFX/barrelpuff.nif", Settings.puffModel);
	Settings.puffLifetime = IniFloat("Puff", "fLifetime", "1.8");
	Settings.puffScale = IniFloat("Puff", "fScale", "1.0");
	Settings.puffInterval = IniFloat("Puff", "fMinInterval", "0.05");
	Settings.puffPlayerOnly = LayerInt("Puff", "bPlayerOnly", 1, IniPath);

	Settings.trail = LayerInt("Wisp", "bEnabled", 1, IniPath);
	IniModel("Wisp", "sModel", "GunFX/barrelstrand.nif", Settings.trailModel);
	Settings.trailScale = IniFloat("Wisp", "fScale", "2.7");
	Settings.heatPerShot = IniFloat("Wisp", "fHeatPerShot", "0.5");
	Settings.coolPerSecond = IniFloat("Wisp", "fCoolPerSecond", "3.0");
	Settings.coolDelay = IniFloat("Wisp", "fCoolDelay", "1.5");
	Settings.wispStopDelay = IniFloat("Wisp", "fStopDelay", "0.5");
	Settings.wispEmitters = (int)LayerInt("Wisp", "iEmitters", 4, IniPath);
	Settings.wispEmitters = Settings.wispEmitters < 1 ? 1 : Settings.wispEmitters > 4 ? 4 : Settings.wispEmitters;
	Settings.maxHeat = IniFloat("Wisp", "fMaxHeat", "15");
	Settings.heatStart = IniFloat("Wisp", "fHeatStart", "3");
	Settings.heatFull = IniFloat("Wisp", "fHeatFull", "12");
	Settings.maxRate = IniFloat("Wisp", "fMaxRate", "150");
	Settings.fileRate = IniFloat("Wisp", "fFileRate", "20");
	{
		// A strand model carries its own build numbers (the INI's fFileRate is for the other models).
		char lower[MAX_PATH];
		strcpy_s(lower, Settings.trailModel);
		_strlwr_s(lower);
		const bool strand = strstr(lower, "barrelstrand") != nullptr;
		WispBuilt = strand ? &StrandFile : &WispFile;
		if (strand) Settings.fileRate = 40.0f;
	}
	Settings.sway = IniFloat("Wisp", "fSway", "1");
	Settings.swaySpeed = IniFloat("Wisp", "fSwaySpeed", "0.1");
	Settings.effectSeconds = IniFloat("Wisp", "fEffectSeconds", "30");
	Settings.tailSeconds = IniFloat("Wisp", "fTailSeconds", "3.5");
	// Flag 4 makes the game take the lifetime from the model's own animation.
	Settings.trailFlags = LayerInt("Wisp", "iFlags", 3, IniPath);
	Settings.trailSmokeOnly = LayerInt("Wisp", "bSmokeOnly", 0, IniPath);
	Settings.trailFollow = LayerInt("Wisp", "bFollowMuzzle", 1, IniPath);      // test switch
	Settings.constantRate = IniFloat("Wisp", "fConstantRate", "0");                       // test switch
	RibbonReports = LayerInt("Wisp", "iRibbonReports", 0, IniPath);          // test switch
	Settings.glow = LayerInt("Glow", "bEnabled", 1, IniPath);
	Settings.glowPreview = LayerInt("Glow", "bPreview", 0, IniPath);
	Settings.glowScale = IniFloat("Glow", "fScale", "1.3");
	Settings.glowLength = IniFloat("Glow", "fLength", "9");
	if (Settings.glowLength < 0.0f) Settings.glowLength = 0.0f;
	if (Settings.glowLength > 40.0f) Settings.glowLength = 40.0f;
	Settings.glowStart = IniFloat("Glow", "fStartHeat", "1.0");
	Settings.glowHeatSpan = IniFloat("Glow", "fHeatSpan", "12.0");
	if (!(Settings.glowHeatSpan >= 1.0f && Settings.glowHeatSpan <= 30.0f)) Settings.glowHeatSpan = 12.0f;
	Settings.glowRiseSeconds = IniFloat("Glow", "fRiseSeconds", "2.8");
	if (!(Settings.glowRiseSeconds >= 0.1f && Settings.glowRiseSeconds <= 10.0f)) Settings.glowRiseSeconds = 2.8f;
	Settings.glowRadius = IniFloat("Glow", "fRadius", "3.2");
	if (!(Settings.glowRadius >= 1.0f && Settings.glowRadius <= 20.0f)) Settings.glowRadius = 3.2f;
	Settings.glowForwardLength = IniFloat("Glow", "fForwardLength", "7.0");
	if (!(Settings.glowForwardLength >= 1.0f && Settings.glowForwardLength <= 30.0f)) Settings.glowForwardLength = 7.0f;
	Settings.glowMinLength = IniFloat("Glow", "fMinLength", "2.5");
	if (!(Settings.glowMinLength >= 0.0f && Settings.glowMinLength <= Settings.glowLength)) Settings.glowMinLength = Settings.glowLength < 2.5f ? Settings.glowLength : 2.5f;
	Settings.glowSpreadShots = IniFloat("Glow", "fSpreadShots", "1000.0");
	if (!(Settings.glowSpreadShots >= 1.0f && Settings.glowSpreadShots <= 100000.0f)) Settings.glowSpreadShots = 1000.0f;
	Settings.glowSpreadCoolSeconds = IniFloat("Glow", "fSpreadCoolSeconds", "900.0");
	if (!(Settings.glowSpreadCoolSeconds >= 1.0f && Settings.glowSpreadCoolSeconds <= 86400.0f)) Settings.glowSpreadCoolSeconds = 900.0f;
	Settings.glowOffsetX = IniFloat("Glow", "fOffsetX", "0.0");
	Settings.glowOffsetY = IniFloat("Glow", "fOffsetY", "0.0");
	Settings.glowOffsetZ = IniFloat("Glow", "fOffsetZ", "0.0");
	if (!(Settings.glowOffsetX >= -30.0f && Settings.glowOffsetX <= 30.0f)) Settings.glowOffsetX = 0.0f;
	if (!(Settings.glowOffsetY >= -30.0f && Settings.glowOffsetY <= 30.0f)) Settings.glowOffsetY = 0.0f;
	if (!(Settings.glowOffsetZ >= -30.0f && Settings.glowOffsetZ <= 30.0f)) Settings.glowOffsetZ = 0.0f;

	LayerStr("Puff", "sNode", "", Settings.puffNode, sizeof(Settings.puffNode), IniPath);
	LayerStr("Wisp", "sNode", "", Settings.wispNode, sizeof(Settings.wispNode), IniPath);
	LayerStr("Ejection", "sNode", "ShellCasingNode", Settings.ejectNode, sizeof(Settings.ejectNode), IniPath);
	auto offset = [](const char* section) {
		NiPoint3 v = {IniFloat(section,"fOffsetX","0"), IniFloat(section,"fOffsetY","0"), IniFloat(section,"fOffsetZ","0")};
		if (!std::isfinite(v.x) || fabsf(v.x)>100) v.x=0;
		if (!std::isfinite(v.y) || fabsf(v.y)>100) v.y=0;
		if (!std::isfinite(v.z) || fabsf(v.z)>100) v.z=0;
		return v;
	};
	// One offset for everything at the barrel tip (puff, heat smoke, after-fire trail), so they always line up.
	Settings.puffOffset=offset("Muzzle"); Settings.wispOffset=Settings.puffOffset; Settings.ejectOffset=offset("Ejection");
	auto bounded = [](const char* section, const char* key, const char* fallback, float low, float high) {
		float v=IniFloat(section,key,fallback);
		return std::isfinite(v) && v>=low && v<=high ? v : (float)atof(fallback);
	};
	Settings.logWeaponNodes=LayerInt("Main","bLogWeaponNodes",1,IniPath);
	Settings.eject = LayerInt("Ejection","bEnabled",1,IniPath);
	IniModel("Ejection","sModel","GunFX/barrelport.nif",Settings.ejectModel);
	Settings.after = LayerInt("Trail", "bEnabled", 1, IniPath);
	IniModel("Trail", "sModel", "GunFX/barreltrail.nif", Settings.afterModel);
	Settings.afterScale = IniFloat("Trail", "fScale", "2.0");
	Settings.afterDelay = IniFloat("Trail", "fStartDelay", "0.5");
	Settings.afterEmitters = (int)LayerInt("Trail", "iEmitters", 4, IniPath);
	Settings.afterEmitters = Settings.afterEmitters < 1 ? 1 : Settings.afterEmitters > 4 ? 4 : Settings.afterEmitters;
	Settings.afterMinHeat = IniFloat("Trail", "fMinHeat", "2");
	Settings.afterFullHeat = IniFloat("Trail", "fFullHeat", "10");
	Settings.afterMaxRate = IniFloat("Trail", "fMaxRate", "60");
	Settings.afterSway = IniFloat("Trail", "fSway", "1.5");
	Settings.afterSwaySpeed = IniFloat("Trail", "fSwaySpeed", "0.15");
	BakeLook("Trail", Settings.afterModel);
	Settings.volume = LayerInt("Volume", "bEnabled", 1, IniPath);
	Settings.volumeSize = IniFloat("Volume", "fSizeScale", "0.25");
	Settings.volumeDensity = IniFloat("Volume", "fDensityScale", "4.0");
	Settings.volumeRise = IniFloat("Volume", "fRiseScale", "1.0");
	Settings.volumeCurl = IniFloat("Volume", "fCurlScale", "400");
	Settings.volumeDrag = IniFloat("Volume", "fDrag", "0.4");
	Settings.volumeSpacing = IniFloat("Volume", "fSpacing", "0.04");
	Settings.volumeNoise = IniFloat("Volume", "fNoise", "0.6");
	Settings.volumeBrightness = IniFloat("Volume", "fBrightness", "1.0");
	Settings.volumeShade = IniFloat("Volume", "fSelfShadow", "0.5");
	Settings.volumeDebug = LayerInt("Volume", "bDebug", 0, IniPath);
	Settings.volumeKinds[ChainHeat] = LayerInt("Volume", "bHeatSmoke", 1, IniPath);
	Settings.volumeKinds[ChainTrail] = LayerInt("Volume", "bTrail", 1, IniPath);
	Settings.volumeKinds[ChainPuff] = LayerInt("Volume", "bPuff", 1, IniPath);
	Settings.volumeKinds[ChainEject] = LayerInt("Volume", "bEjection", 1, IniPath);
	Settings.volumeBurstDrag = IniFloat("Volume", "fBurstDrag", "3.0");
	Settings.volumeBurstSize = IniFloat("Volume", "fBurstSizeScale", "1.0");
	Settings.volumeGunInFront = LayerInt("Volume", "bGunInFront", 1, IniPath);
	Settings.volumeExpand = IniFloat("Volume", "fExpand", "1.6");
	Settings.volumeFlash = IniFloat("Volume", "fFlash", "1.0");
	Settings.volumeFlashRadius = IniFloat("Volume", "fFlashRadius", "15");
	Settings.volumeFlashSeconds = IniFloat("Volume", "fFlashSeconds", "0.05");
	Settings.volumeWind = IniFloat("Volume", "fWind", "40");
	Settings.volumeWindPickup = IniFloat("Volume", "fWindPickup", "0.8");
	Settings.volumeWindOffset = IniFloat("Volume", "fWindDirectionOffset", "0");
	Settings.volumePlayer = LayerInt("Volume", "bPlayerPush", 1, IniPath);
	Settings.volumePlayerRadius = IniFloat("Volume", "fPlayerRadius", "30");
	Settings.volumePlayerHeight = IniFloat("Volume", "fPlayerHeight", "140");
	Settings.volumePlayerPush = IniFloat("Volume", "fPlayerPush", "40");
	Settings.volumeStir = IniFloat("Volume", "fStir", "1.5");
	Settings.volumeDiffuse = IniFloat("Volume", "fDiffuse", "1.0");
	Settings.volumeBurst = IniFloat("Volume", "fBurst", "0.5");
	Settings.volumeBullet = IniFloat("Volume", "fBullet", "2");
	Settings.volumeBulletReach = IniFloat("Volume", "fBulletReach", "8");
	Settings.volumeBulletOpen = IniFloat("Volume", "fBulletOpen", "0.12");
	Settings.volumeBulletSeconds = IniFloat("Volume", "fBulletSeconds", "1.4");
	if (Settings.volumeBulletOpen < 0.01f) Settings.volumeBulletOpen = 0.01f;
	if (Settings.volumeBulletSeconds < 0.2f) Settings.volumeBulletSeconds = 0.2f;
	if (Settings.volumeBulletSeconds > 3.0f) Settings.volumeBulletSeconds = 3.0f;
	Settings.volumePillows = IniFloat("Volume", "fPillows", "1");
	Settings.volumeStreaks = IniFloat("Volume", "fStreaks", "0");
	Settings.volumeHaze = IniFloat("Volume", "fHaze", "3");
	Settings.volumeHazeLife = IniFloat("Volume", "fHazeLife", "45");
	Settings.volumeHazeSize = IniFloat("Volume", "fHazeSize", "90");
	Settings.volumeTint = IniFloat("Volume", "fTint", "1.0");
	Settings.volumeSmooth = IniFloat("Volume", "fSmooth", "3.0");
	Settings.volumeDepth = IniFloat("Volume", "fDepth", "1.0");
	Settings.volumeDriftRight = IniFloat("Volume", "fDriftRight", "8");
	Settings.volumeDriftAway = IniFloat("Volume", "fDriftAway", "3");
	Settings.volumeWrapGun = LayerInt("Volume", "bWrapGun", 1, IniPath);
	Settings.volumeGunShape = LayerInt("Volume", "bGunShape", 1, IniPath);
	Settings.volumeGunLength = IniFloat("Volume", "fGunLength", "30");
	Settings.volumeGunRadius = IniFloat("Volume", "fGunRadius", "3.5");
	Settings.volumeGunSwirl = IniFloat("Volume", "fGunSwirl", "0.6");
	Settings.volumeBodySwirl = IniFloat("Volume", "fBodySwirl", "0.5");
	Settings.volumeSwirlLife = IniFloat("Volume", "fSwirlLife", "1.2");
	if (Settings.volumeSwirlLife < 0.2f) Settings.volumeSwirlLife = 0.2f;
	Settings.volumeBarrelSpread = IniFloat("Volume", "fBarrelSpread", "0.3");
	Settings.volumeTendrils = IniFloat("Volume", "fTendrils", "0.6");
	Settings.volumeTendrilAngle = IniFloat("Volume", "fTendrilAngle", "40");
	Settings.volumeTendrilSize = IniFloat("Volume", "fTendrilSize", "0.35");
	Settings.volumeTendrilSpeed = IniFloat("Volume", "fTendrilSpeed", "12");
	Settings.volumeUnzip = LayerInt("Volume", "bUnzip", 1, IniPath);
	Settings.volumeFanSpeed = IniFloat("Volume", "fFanSpeed", "30");
	if (Settings.volumeBarrelSpread < 0.0f) Settings.volumeBarrelSpread = 0.0f;
	if (Settings.volumeBarrelSpread > 1.0f) Settings.volumeBarrelSpread = 1.0f;
	Settings.volumeThinning = IniFloat("Volume", "fThinning", "0.2");
	if (Settings.volumeThinning < 0.0f) Settings.volumeThinning = 0.0f;
	if (Settings.volumeThinning > 1.0f) Settings.volumeThinning = 1.0f;
	Settings.volumeBurstPoints = (int)LayerInt("Volume", "iBurstPoints", 4, IniPath);
	if (Settings.volumeBurstPoints < 1) Settings.volumeBurstPoints = 1;
	if (Settings.volumeBurstPoints > 12) Settings.volumeBurstPoints = 12;
	Settings.volumePuffForward = IniFloat("Volume", "fPuffForward", "15");
	{
		const char* sections[ChainCount] = { "Wisp", "Trail", "Puff", "Ejection" };
		for (int k = 0; k < ChainCount; k++) {
			VolLook& v = Settings.volLook[k];
			const char* sec = sections[k];
			v.size = IniFloat(sec, "fSize", "3");            v.sizeVar = IniFloat(sec, "fSizeVariation", "0.5");
			v.life = IniFloat(sec, "fSmokeLife", "2.6");     v.lifeVar = IniFloat(sec, "fSmokeLifeVariation", "0.3");
			v.speed = IniFloat(sec, "fSpeed", "10");         v.speedVar = IniFloat(sec, "fSpeedVariation", "1");
			v.startSize = IniFloat(sec, "fStartSize", "0.3"); v.grow = IniFloat(sec, "fGrowSeconds", "2.6");
			v.opacity = IniFloat(sec, "fOpacity", "0.1");    v.fadeIn = IniFloat(sec, "fFadeIn", "0.05");
			v.fadeStart = IniFloat(sec, "fFadeStart", "0.2"); v.fadeEnd = IniFloat(sec, "fFadeEnd", "0.95");
			v.rise = IniFloat(sec, "fRise", "7");            v.curl = IniFloat(sec, "fCurl", "0.012");
			v.spread = IniFloat(sec, "fSpread", k < 2 ? "0.03" : "0.8");
			v.size *= k == ChainHeat ? Settings.trailScale : k == ChainTrail ? Settings.afterScale : IniFloat(sec, "fScale", "1.0");   // the effects' fScale
		}
	}
	BakeLook("Puff", Settings.puffModel);
	BakeLook("Wisp", Settings.trailModel);
	BakeLook("Ejection", Settings.ejectModel);
	// A changed look applies at once: the running heat smoke / trail hands over to a new effect with it (the smoke
	// already in the air drifts off). Each keeps its own look; they never share settings.
	static char lastWisp[MAX_PATH] = "", lastAfter[MAX_PATH] = "";
	static int lastWispCount = 0, lastAfterCount = 0;
	if ((lastWisp[0] && _stricmp(lastWisp, Settings.trailModel)) || (lastWispCount && lastWispCount != Settings.wispEmitters))
		ReleaseWisp("its look changed");
	if ((lastAfter[0] && _stricmp(lastAfter, Settings.afterModel)) || (lastAfterCount && lastAfterCount != Settings.afterEmitters))
		ReleaseAfter("its look changed");
	lastWispCount = Settings.wispEmitters;
	lastAfterCount = Settings.afterEmitters;
	strcpy_s(lastWisp, Settings.trailModel);
	strcpy_s(lastAfter, Settings.afterModel);
	Settings.ejectScale=bounded("Ejection","fScale","1.0",0.01f,10);
	Settings.ejectLifetime=bounded("Ejection","fLifetime","1.5",0.1f,5);
	Settings.ejectHotScale=bounded("Ejection","fHotScale","1.8",0.1f,10);
	Settings.ejectBurst=bounded("Ejection","fBurstSeconds","0.06",0.01f,2);
	Settings.puffBurst=bounded("Puff","fBurstSeconds","0.12",0.01f,2);
	Settings.puffBurstRate=bounded("Puff","fBurstRate","60",1,600);
	Settings.ejectBurstRate=bounded("Ejection","fBurstRate","160",1,600);
	Settings.ejectInterval=bounded("Ejection","fMinInterval","0",0,2);
	Settings.haze=LayerInt("Haze","bEnabled",1,IniPath);
	Settings.hazePreview=LayerInt("Haze","bPreview",0,IniPath);
	Settings.hazeMask=LayerInt("Haze","bShowMask",0,IniPath);
	Settings.blast=LayerInt("Blast","bEnabled",1,IniPath);
	Settings.blastPixels=bounded("Blast","fStrengthPixels","6",0,30);
	Settings.blastRadius=bounded("Blast","fRadiusPixels","70",5,400);
	Settings.blastSeconds=bounded("Blast","fSeconds","0.06",0.01f,1);
	Settings.hazeStart=bounded("Haze","fStartHeat","3",0,100);
	Settings.hazeFull=bounded("Haze","fFullHeat","12",0.1f,100);
	if (Settings.hazeFull <= Settings.hazeStart) Settings.hazeFull=Settings.hazeStart+1;
	Settings.hazeRise=bounded("Haze","fRiseSeconds","1.5",0.05f,20);
	Settings.hazePixels=bounded("Haze","fStrengthPixels","3",0,8);
	Settings.hazeHeight=bounded("Haze","fHeightPixels","90",1,200);
	Settings.hazeWidth=bounded("Haze","fWidthPixels","20",1,100);
	Settings.hazeSpeed=bounded("Haze","fSpeed","1",0.01f,10);
	Settings.hazeLength=bounded("Haze","fLength","8",0.1f,40);
	Settings.hazeMinSize=bounded("Haze","fMinSize","0.35",0,1);
	DebugLine("haze %s: %.2f pixels; ejection %s: node %s; smoke anchors puff '%s', wisp '%s'", Settings.haze?"on":"off",
		Settings.hazePixels,Settings.eject?"on":"off",Settings.ejectNode,Settings.puffNode,Settings.wispNode);
	Settings.flags = LayerInt("Main", "iFlags", 3, IniPath);
	ReadLook("Wisp", *WispBuilt, WispLook);
	ReadLook("Puff", PuffFile, PuffLook);
	Settings.logEvents = LayerInt("Main", "iLogEvents", 20, IniPath);

	DebugLine("puff: %s, model %s, %.2f s, scale %.2f, every %.2f s at most, player only %d", Settings.puff ? "on" : "off",
		Settings.puffModel, Settings.puffLifetime, Settings.puffScale, Settings.puffInterval, Settings.puffPlayerOnly);
	DebugLine("wisp: %s, model %s, scale %.2f, heat +%.2f per shot, cools %.2f/s, max %.1f; smoke from heat %.1f, full (%.1f/s) at %.1f",
		Settings.trail ? "on" : "off", Settings.trailModel, Settings.trailScale, Settings.heatPerShot, Settings.coolPerSecond,
		Settings.maxHeat, Settings.heatStart, Settings.maxRate, Settings.heatFull);
	DebugLine("barrel shader heat: %s, preview %s, length %.1f..%.1f, radius %.1f, forward %.1f, offset (%.1f, %.1f, %.1f), spread %.0f shots, cool %.0f s; starts at heat %.1f, full over %.1f heat, rise %.1f s", Settings.glow ? "on" : "off", Settings.glowPreview ? "on" : "off",
		Settings.glowMinLength, Settings.glowLength, Settings.glowRadius, Settings.glowForwardLength,
		Settings.glowOffsetX, Settings.glowOffsetY, Settings.glowOffsetZ,
		Settings.glowSpreadShots, Settings.glowSpreadCoolSeconds, Settings.glowStart, Settings.glowHeatSpan, Settings.glowRiseSeconds);
}

static bool ReadHeatForNVR(float* out, const void* weaponNode, bool extended) {
	if (!out || !weaponNode || !HeatLockReady) return false;
	EnterCriticalSection(&HeatLock);
	UInt8* node = HeatNode;
	NodeAddRef(node);
	const float strength = HeatStrength, span = HeatLength;
	const float radius = HeatRadius, forward = HeatForwardLength;
	const float offsetX = HeatOffsetX, offsetY = HeatOffsetY, offsetZ = HeatOffsetZ;
	LeaveCriticalSection(&HeatLock);
	if (!node) return false;
	bool belongs = false;
	UInt8* parent = node;
	for (int depth = 0; parent && depth < 32; depth++, parent = *(UInt8**)(parent + 0x18)) {
		if (parent == weaponNode) { belongs = true; break; }
	}
	if (belongs) {
		NiPoint3 muzzle = *(NiPoint3*)(node + 0x8C);
		if (extended) {
			// Offset in ProjectileNode's own axes so it follows the gun's animation.
			const float* rot = (const float*)(node + 0x68);
			muzzle.x += rot[0] * offsetX + rot[1] * offsetY + rot[2] * offsetZ;
			muzzle.y += rot[3] * offsetX + rot[4] * offsetY + rot[5] * offsetZ;
			muzzle.z += rot[6] * offsetX + rot[7] * offsetY + rot[8] * offsetZ;
		}
		const NiPoint3 back = GlowDirection(node);
		out[0] = muzzle.x; out[1] = muzzle.y; out[2] = muzzle.z; out[3] = strength;
		out[4] = back.x; out[5] = back.y; out[6] = back.z; out[7] = span;
		if (extended) { out[8] = radius; out[9] = forward; }
	}
	NodeRelease(node);
	return belongs;
}

}   // the anonymous namespace

// ---- What Vegas Shaders calls (GunFX.h; until 2026-11-16 the GunFX_* exports of GunFX.dll) ----------------------------
namespace GunFX {
bool GetHeatV2(float out[8], const void* weaponNode) {
	return ReadHeatForNVR(out, weaponNode, false);
}

bool GetHeatV3(float out[10], const void* weaponNode) {
	return ReadHeatForNVR(out, weaponNode, true);
}

bool GetHaze(float out[8], const void* weaponNode) {
	float heat[10];
	if (!out || !ReadHeatForNVR(heat, weaponNode, true)) return false;
	EnterCriticalSection(&HeatLock);
	memcpy(out, HazePublished, sizeof(HazePublished));
	LeaveCriticalSection(&HeatLock);
	return out[0] > 0.001f;
}

// As GunFX_GetHaze, plus out[8..10] the muzzle and out[11..13] the rearward axis in world space, from the game
// loop (for the first-person model, whose render-time positions are in another space).
bool GetHazeV2(float out[14], const void* weaponNode) {
	float heat[10];
	if (!out || !ReadHeatForNVR(heat, weaponNode, true)) return false;
	EnterCriticalSection(&HeatLock);
	memcpy(out, HazePublished, sizeof(HazePublished));
	memcpy(out + 8, HazeWorld, sizeof(HazeWorld));
	LeaveCriticalSection(&HeatLock);
	return out[0] > 0.001f;
}

// As V2, plus out[14] the muzzle blast displacement in pixels right now and out[15] its radius in pixels. True while
// either the barrel haze or the blast is showing.
bool GetHazeV3(float out[16], const void* weaponNode) {
	float heat[10];
	if (!out || !ReadHeatForNVR(heat, weaponNode, true)) return false;
	EnterCriticalSection(&HeatLock);
	memcpy(out, HazePublished, sizeof(HazePublished));
	memcpy(out + 8, HazeWorld, sizeof(HazeWorld));
	memcpy(out + 14, HazeBlast, sizeof(HazeBlast));
	LeaveCriticalSection(&HeatLock);
	return out[0] > 0.001f || out[14] > 0.01f;
}

// NVR's menu switches (see Switch()); called by NVR every frame.
void SetSwitches(UInt32 bits) {
	if ((LONG)bits != NvrSwitches) {
		InterlockedExchange(&NvrSwitches, (LONG)bits);
		LogLine("menu switches: puff %d, heat smoke %d, ejection smoke %d, glow %d, haze %d, muzzle blast %d, energy weapons %d, after-fire trail %d, volumetric %d",
			bits & 1, (bits >> 1) & 1, (bits >> 2) & 1, (bits >> 3) & 1, (bits >> 4) & 1, (bits >> 5) & 1, (bits >> 6) & 1, (bits >> 7) & 1, (bits >> 8) & 1);
	}
}

// Volumetric smoke for NVR: up to maxRecords records of 12 floats (see VolumePublished) and params[8] (noise,
// brightness, self-shadow, clock, debug view, gun in front, colour, depth shading). Returns the number of records.
int GetVolumeSmoke(float* out, int maxRecords, float* params) {
	if (!out || !params || !HeatLockReady) return 0;
	EnterCriticalSection(&HeatLock);
	const int n = VolumePublishedCount < maxRecords ? VolumePublishedCount : maxRecords;
	for (int i = 0; i < n; i++) memcpy(out + i * 12, VolumePublished + i * RecordFloats, 12 * sizeof(float));   // the first 12 floats
	memcpy(params, VolumeParams, sizeof(VolumeParams));
	LeaveCriticalSection(&HeatLock);
	return n;
}
// The same with records of 16 floats (see VolumePublished): NVR P70 and newer.
int GetVolumeSmoke2(float* out, int maxRecords, float* params) {
	if (!out || !params || !HeatLockReady) return 0;
	EnterCriticalSection(&HeatLock);
	const int n = VolumePublishedCount < maxRecords ? VolumePublishedCount : maxRecords;
	memcpy(out, VolumePublished, n * RecordFloats * sizeof(float));
	memcpy(params, VolumeParams, sizeof(VolumeParams));
	LeaveCriticalSection(&HeatLock);
	return n;
}
// The camera, from NVR every frame: eye (world) x, y, z and the way it looks x, y, z (unit length). Bullets fly to where
// you aim.
void SetView(const float* v) {
	if (!v || !HeatLockReady) return;
	EnterCriticalSection(&HeatLock);
	ViewEye = NiPoint3{ v[0], v[1], v[2] };
	ViewForward = NiPoint3{ v[3], v[4], v[5] };
	ViewAt = GetTickCount64();
	LeaveCriticalSection(&HeatLock);
}
// The bullets whose holes are still open ([Volume] fBulletSeconds), for the tunnels NVR clears through the smoke: up to
// maxBullets of 8 floats, where it was fired from (world) x, y, z, seconds since, the way it flew x, y, z, how hard
// ([Volume] fBullet). Returns how many.
int GetBullets(float* out, int maxBullets) {
	if (!out || Settings.volumeBullet <= 0.0f) return 0;
	int n = 0;
	for (const Bullet& b : Bullets) {
		const float since = SmokeClock - b.at;
		if (b.at <= 0.0f || since < 0.0f || since >= Settings.volumeBulletSeconds || n >= maxBullets) continue;
		float* o = out + n++ * 8;
		o[0] = b.from.x; o[1] = b.from.y; o[2] = b.from.z; o[3] = since;
		o[4] = b.dir.x; o[5] = b.dir.y; o[6] = b.dir.z; o[7] = Settings.volumeBullet;
	}
	return n;
}
// How a bullet's hole runs, for NVR's shader: out[0] = seconds it takes to open ([Volume] fBulletOpen), out[1] = seconds until
// the smoke has filled it again ([Volume] fBulletSeconds).
void GetBulletLook(float* out) {
	if (!out) return;
	out[0] = Settings.volumeBulletOpen;
	out[1] = Settings.volumeBulletSeconds;
}
// How much you are moving (0 standing .. 1 walking or running, eased), for NVR's shader: while you move, bullets leave your
// own smoke close to you alone (it streams past their tunnels, which stay where they were fired, and flickered).
float GetMoving() {
	return VolumeMoving;
}
// The gun-smoke haze for NVR's haze pass (NVR P71 and newer): up to maxBlobs blobs of 8 floats (see SmokeHazePublished).
// Returns how many.
int GetSmokeHaze(float* out, int maxBlobs) {
	if (!out || !HeatLockReady) return 0;
	EnterCriticalSection(&HeatLock);
	const int n = SmokeHazePublishedCount < maxBlobs ? SmokeHazePublishedCount : maxBlobs;
	memcpy(out, SmokeHazePublished, n * HazeFloats * sizeof(float));
	LeaveCriticalSection(&HeatLock);
	return n;
}
// The light the player's last muzzle flash throws on the volumetric smoke: out[8] = x, y, z (world), light (0 = none;
// [Volume] fFlash at the shot, gone after about fFlashSeconds x 4), colour r, g, b, reach (fFlashRadius).
void GetSmokeFlash(float* out) {
	if (!out) return;
	memset(out, 0, 8 * sizeof(float));
	if (!HeatLockReady) return;
	EnterCriticalSection(&HeatLock);
	const NiPoint3 at = FlashWorld;
	const double shot = FlashAt;
	LeaveCriticalSection(&HeatLock);
	const float seconds = Settings.volumeFlashSeconds > 0.005f ? Settings.volumeFlashSeconds : 0.005f;
	const float since = shot > 0.0 ? (float)(PreciseSeconds() - shot) : 1e9f;
	if (since > seconds * 4.0f || Settings.volumeFlash <= 0.0f) return;
	out[0] = at.x; out[1] = at.y; out[2] = at.z;
	out[3] = Settings.volumeFlash * expf(-since / seconds);
	out[4] = 1.0f; out[5] = 0.55f; out[6] = 0.25f;   // warm orange
	out[7] = Settings.volumeFlashRadius;
}

// From Vegas Shaders' NVSEPlugin_Load (game only, not the editor), after its settings are read. False: GunFX stays off (another
// game version: its addresses are FalloutNV.exe 1.4.0.525's, which GunFX.dll's own NVSEPlugin_Query required too).
static bool Loaded = false;
bool Load(unsigned runtimeVersion) {
	// (2026-11-16: no GunFX.log of its own any more: its lines go into the Vegas Shaders log, "GunFX:" in front)
	LogLine("GunFX 1.39 (volumetric smoke prototype: heat smoke, trail, puff and ejection smoke; edge detail, sun and muzzle flash light, colour, wind, pushed by your body, smoothed strands, depth shading, drift aside, flows around the gun, tendrils, unzips and fans out around you and the gun, swirls behind the barrel and your body, recoil smoothed out, barrel direction from the muzzle node, no random flicks, gun shape probe, puff carried with the gun, smoke eased out of the gun and your body, air flows round your body and the gun, the gun's real shape, smoke you run into parts at once, stirred smoke spreads out and frays, heat smoke and trail flow out of the barrel, bursts when you run or swing through smoke, only a hanging trail unzips, CPU time logged, gun-smoke haze, haze in one pass of its own and stacking where you fire, bullets punch through the smoke and the haze, billowy puffs and fibrous strands, smoke timed on the precise clock, smoke inside you slides out instead of popping, clock time on each log line, hitch diagnostic: whole-step timing, smoke streams back along the gun as you move instead of being thrown aside by your body, and spreads over it, bullet holes open and fill in at your pace, and leave your own smoke near you alone while you move)");
	if (runtimeVersion != 0x040020D0) { LogLine("not FalloutNV.exe 1.4.0.525 (runtime %08X): GunFX stays off", runtimeVersion); return false; }
	LoadSettings();
	// (GunFX.dll stood down when the old BarrelSmoke.dll was installed; dropped 2026-11-16: that mod hardly circulated)
	InitializeCriticalSection(&PendingLock);
	InitializeCriticalSection(&HeatLock);
	HeatLockReady = true;
	HookCall(CallSites[0], (void*)FireHook0, &OriginalFire[0]);
	HookCall(CallSites[1], (void*)FireHook1, &OriginalFire[1]);
	HookCall(CallSites[2], (void*)FireHook2, &OriginalFire[2]);
	HookCall(EjectSites[0], (void*)EjectHook0, &OriginalEject[0]);
	HookCall(EjectSites[1], (void*)EjectHook1, &OriginalEject[1]);
	LogLine("built into Vegas Shaders: settings in [Main.GunFX.*] (the menu's Gun effects page), the game loop from its xNVSE message handler");
	Loaded = true;
	return true;
}

// xNVSE's kMessage_MainGameLoop, forwarded by Vegas Shaders' message handler (no listener of its own).
// 2026-11-16: with every effect switched off (the default) and nothing left to fade out, GunFX does no work at all: the shots are
// dropped, so no heat, gun-shape builds or smoke steps (before, every player who had GunFX.dll installed paid for those even with
// everything off; a player without it paid nothing, which is what built-in GunFX must keep).
static bool Idle() {
	if (NvrSwitches & 0x1BF) return false;   // an effect is on (bit 6, energy weapons, is no effect of its own)
	// (the glow and haze ease toward zero and never quite reach it: below what Vegas Shaders draws counts as gone)
	return Heat <= 0.0f && VisualHeat < 1e-4f && VisualHaze < 1e-4f && HeatStrength < 1e-4f && HazePublished[0] <= 0.001f &&
		HazeBlast[0] <= 0.01f && !FollowEffect && !AfterEffect && !VolumePublishedCount && !SmokeHazePublishedCount;
}
void MainGameLoop() {
	if (!Loaded) return;
	if (Idle()) { EnterCriticalSection(&PendingLock); PendingCount = 0; LeaveCriticalSection(&PendingLock); return; }
	TimedMainLoop();
}
}   // namespace GunFX
