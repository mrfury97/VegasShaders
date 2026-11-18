#pragma once
#include <windows.h>
#include <cstring>

// Copy a fresh configuration, or add only missing keys to an existing one.
inline int MergeIniDefaults(const char* defaults, const char* active) {
    if (GetFileAttributesA(defaults) == INVALID_FILE_ATTRIBUTES) return 0;
    if (CopyFileA(defaults, active, TRUE)) return 1;
    char sections[16384], keys[16384], existing[16384], value[4096];
    DWORD n = GetPrivateProfileSectionNamesA(sections, sizeof(sections), defaults);
    if (n >= sizeof(sections) - 2) return -1;
    int added = 0;
    for (const char* s = sections; *s; s += strlen(s) + 1) {
        n = GetPrivateProfileStringA(s, NULL, "", keys, sizeof(keys), defaults);
        if (n >= sizeof(keys) - 2) return -1;
        n = GetPrivateProfileStringA(s, NULL, "", existing, sizeof(existing), active);
        if (n >= sizeof(existing) - 2) return -1;
        for (const char* k = keys; *k; k += strlen(k) + 1) {
            bool found = false;
            for (const char* e = existing; *e; e += strlen(e) + 1)
                if (!_stricmp(k, e)) { found = true; break; }
            if (found) continue;
            n = GetPrivateProfileStringA(s, k, "", value, sizeof(value), defaults);
            if (n >= sizeof(value) - 1 || !WritePrivateProfileStringA(s, k, value, active)) return -1;
            ++added;
        }
    }
    return added;
}
