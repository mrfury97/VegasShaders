#include "SafeWrite.h"
#include <memoryapi.h>	// VirtualProtect
#include <mutex>
#include <unordered_set>

void NoteHookSite(SIZE_T address, SIZE_T count, const char* label) {
	// The game's .text and .rdata (vtables); .data (from 0x01183000) holds settings and globals, written often: not logged.
	if (address < 0x00401000 || address >= 0x01183000) return;
	static std::mutex lock;
	static std::unordered_set<SIZE_T> seen;
	{
		std::lock_guard<std::mutex> guard(lock);
		if (!seen.insert(address).second) return;
	}
	const UInt8* b = (const UInt8*)address;
	char hex[3 * 8 + 1] = {};
	const SIZE_T shown = count < 8 ? (count ? count : 1) : 8;
	for (SIZE_T i = 0; i < shown; ++i) sprintf_s(hex + 3 * i, 4, "%02X ", b[i]);
	char found[MAX_PATH + 48] = "";
	if (address >= 0x00FDF000) {	// .rdata: a vtable slot (or a constant)
		if (count == 4) sprintf_s(found, " = %08X", *(const UInt32*)address);
	}
	else if (b[0] == 0xE8 || b[0] == 0xE9) {	// a call/jmp: where does it go?
		const SIZE_T target = address + 5 + *(const SInt32*)(address + 1);
		if (target >= 0x00401000 && target < 0x01183000) sprintf_s(found, " -> %08X (game)", (UInt32)target);
		else {
			HMODULE module = nullptr;
			char path[MAX_PATH] = "?";
			if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)target, &module) && module)
				GetModuleFileNameA(module, path, MAX_PATH);
			const char* name = strrchr(path, '\\');
			sprintf_s(found, " -> %08X (%s)", (UInt32)target, name ? name + 1 : path);
		}
	}
	Logger::Log("HOOKSITE %08X %-24s before: %s%s", (UInt32)address, label ? label : "", hex, found);
}

#pragma optimize("y", on)	// frame pointers omitted in the patch helpers

namespace {
	// Makes [address, address + bytes) writable (and executable) for the lifetime of the object.
	class WritableScope {
	public:
		WritableScope(SIZE_T address, SIZE_T bytes = sizeof(SIZE_T)) : start(address), length(bytes) {
			VirtualProtect((void*)start, length, PAGE_EXECUTE_READWRITE, &previous);
		}
		~WritableScope() {
			if (start >= 0x00401000 && start < 0x00FDF000)
				FlushInstructionCache(GetCurrentProcess(), (LPCVOID)start, length);
			VirtualProtect((void*)start, length, previous, &previous);
		}

	private:
		const SIZE_T start;
		const SIZE_T length;
		SIZE_T previous;
	};

	// rel32 operand of a jump / call whose rel32 sits at `operand` (the instruction ends 4 bytes later).
	inline UInt32 RelativeTo(SIZE_T operand, SIZE_T target) { return target - (operand + 4); }
}

void __fastcall SafeWrite8(SIZE_T address, SIZE_T value) {
	NoteHookSite(address, 1);
	WritableScope scope(address);
	*(uint8_t*)address = value;
}

void __fastcall SafeWrite16(SIZE_T address, SIZE_T value) {
	NoteHookSite(address, 2);
	WritableScope scope(address);
	*(uint16_t*)address = value;
}

void __fastcall SafeWrite32(SIZE_T address, SIZE_T value) {
	NoteHookSite(address, 4);
	WritableScope scope(address);
	*(uint32_t*)address = value;
}

void __fastcall SafeWriteBuf(SIZE_T address, const void* bytes, SIZE_T count) {
	NoteHookSite(address, count);
	WritableScope scope(address, count);
	memcpy((void*)address, bytes, count);
}

void __fastcall WriteRelJump(SIZE_T site, SIZE_T target) {
	NoteHookSite(site, 5);
	WritableScope scope(site, 5);
	*(uint8_t*)site = 0xE9;
	*(uint32_t*)(site + 1) = RelativeTo(site + 1, target);
}

void __fastcall WriteRelCall(SIZE_T site, SIZE_T target) {
	NoteHookSite(site, 5);
	WritableScope scope(site, 5);
	*(uint8_t*)site = 0xE8;
	*(uint32_t*)(site + 1) = RelativeTo(site + 1, target);
}

void __fastcall ReplaceCall(SIZE_T site, SIZE_T target) {
	NoteHookSite(site, 5);
	WritableScope scope(site + 1, 4);
	*(uint32_t*)(site + 1) = RelativeTo(site + 1, target);
}

#pragma optimize("y", off)	// back to the project setting
