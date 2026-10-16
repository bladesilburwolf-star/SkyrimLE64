// SkyrimMario - SKSE 1.9.32 plugin (Skyrim LE 1.9.32.0.8). Milestone 2:
// Mario overlay (F9 to spawn) drawn through a Direct3D 9 hook.
// SKSE structs are declared here directly so no SKSE source tree is needed yet.
#include "sm64_api.h"
#include <windows.h>

typedef uint32_t UInt32;
struct PluginInfo { UInt32 infoVersion; const char* name; UInt32 version; };
struct SKSEInterface {
    UInt32 skseVersion, runtimeVersion, editorVersion, isEditor;
    void* (*QueryInterface)(UInt32);
    UInt32 (*GetPluginHandle)(void);
    UInt32 (*GetReleaseIndex)(void);
};
void* g_skseTaskInterface = nullptr;
static const UInt32 RUNTIME_1_9_32_0 = 0x09200000; // value SKSE 1.7.3 reports for Skyrim 1.9.32.0 (seen in skse.log)

static std::string ModuleDir(HMODULE m) {
    char p[MAX_PATH]; GetModuleFileNameA(m, p, MAX_PATH);
    std::string s(p); return s.substr(0, s.find_last_of("\\/"));
}
static HMODULE ThisModule() {
    HMODULE m = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&ThisModule, &m);
    return m;
}
static DWORD WINAPI Worker(LPVOID) {
    std::string pluginDir = ModuleDir(ThisModule());
    std::string gameDir = ModuleDir(nullptr);
    FILE* log = fopen((pluginDir + "\\SkyrimMario.log").c_str(), "w");
    if (!log) return 1;
    fprintf(log, "SkyrimMario milestone 5.3 (plugin dir: %s)\n", pluginDir.c_str());
    fprintf(log, "Looking for sm64.dll + baserom.us.z64 in: %s\\mario64\n", gameDir.c_str());
    OverlayRun(gameDir + "\\mario64", log);
    return 0; // log stays open for the overlay
}

extern "C" {
__declspec(dllexport) bool SKSEPlugin_Query(const SKSEInterface* skse, PluginInfo* info) {
    info->infoVersion = 1; info->name = "SkyrimMario"; info->version = 1;
    if (skse->isEditor) return false;
    if (skse->runtimeVersion != RUNTIME_1_9_32_0 && skse->runtimeVersion != 0x01090200) return false;
    return true;
}
__declspec(dllexport) bool SKSEPlugin_Load(const SKSEInterface* skse) {
    g_skseTaskInterface = skse->QueryInterface(4); // kInterface_Task
    CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    return true;
}
}
