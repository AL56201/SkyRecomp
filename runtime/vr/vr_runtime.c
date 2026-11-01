// Check SteamVR without loading OpenXR: some drivers crash if only vrserver is running.
#include "vr.h"
#include <stdio.h>
#include <SDL3/SDL.h>
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <string.h>

static bool steam_runtime(void) {
    char path[2048] = {0};
    DWORD size = sizeof path;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Khronos\\OpenXR\\1", "ActiveRuntime",
                     RRF_RT_REG_SZ, NULL, path, &size) != ERROR_SUCCESS) return true;
    for (char *p = path; *p; ++p) if (*p >= 'A' && *p <= 'Z') *p += 'a' - 'A';
    return strstr(path, "steamxr") != NULL;
}

bool vr_runtime_ready(void) {
    if (!steam_runtime()) return true;  // other OpenXR runtimes report readiness through their API
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32 entry = {.dwSize = sizeof entry};
    bool server = false, compositor = false;
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    if (Process32First(snapshot, &entry)) do {
        DWORD other_session;
        if (!ProcessIdToSessionId(entry.th32ProcessID, &other_session) || other_session != session) continue;
        if (!_stricmp(entry.szExeFile, "vrserver.exe")) server = true;
        if (!_stricmp(entry.szExeFile, "vrcompositor.exe")) compositor = true;
    } while (Process32Next(snapshot, &entry));
    CloseHandle(snapshot);
    return server && compositor;
}

void vr_runtime_start(void) {
    if (steam_runtime()) {
        fprintf(stderr, "[vr] requesting SteamVR startup; desktop play continues while waiting\n");
        if (!SDL_OpenURL("steam://run/250820")) fprintf(stderr, "[vr] SteamVR launch failed: %s\n", SDL_GetError());
    }
}
#else
bool vr_runtime_ready(void) { return true; }
void vr_runtime_start(void) {}
#endif
