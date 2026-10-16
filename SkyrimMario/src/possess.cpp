#include "possess.h"
#include "prof.h"
#include <windows.h>
#include <math.h>
#include <string.h>
#include <vector>
#include <algorithm>

extern void* g_skseTaskInterface;
struct SKSETaskInterfaceLite { uint32_t interfaceVersion; void (*AddTask)(void* task); void (*AddUITask)(void* task); };

static const uintptr_t ADDR_PLAYER_PTR = 0x01B2E8E4;
static const uintptr_t ADDR_MOVEREFR   = 0x0090C9F0;   // MoveRefrToPosition (SKSE 1.7.3 GameReferences.cpp)
static const uintptr_t ADDR_GETWORLDSP = 0x004D5EB0;   // TESObjectREFR::GetWorldspace
static const uintptr_t ADDR_INVALIDHND = 0x01310630;   // *g_invalidRefHandle

typedef void (*MoveRefr_t)(void* refr, uint32_t* targetHandle, void* parentCell, void* worldSpace, float* pos, float* rot);
typedef void* (__attribute__((thiscall)) *GetWorldspace_t)(void*);

static FILE* g_log = nullptr;
#define LOG(...) do { if (g_log) { fprintf(g_log, __VA_ARGS__); fputc('\n', g_log); fflush(g_log); } } while (0)

static CRITICAL_SECTION g_cs; static bool g_csInit = false;
static volatile LONG g_pending = 0;
static bool g_on = false, g_wantHidden = false, g_isHidden = false;
static float g_target[3]; static bool g_haveTarget = false;
static int g_moves = 0;

static bool Ok(const void* p, size_t n) { return p && !IsBadReadPtr(p, n); }
typedef void* (__attribute__((thiscall)) *GetRTTI_t)(void*);
static bool IsA(uintptr_t obj, const char* name) {
    void** vt = (void**)*(uintptr_t*)obj; if (!Ok(vt, 12)) return false;
    void* rtti = ((GetRTTI_t)vt[2])((void*)obj);
    for (int i = 0; i < 16 && rtti && Ok(rtti, 8); ++i) {
        const char* nm = *(const char**)rtti; if (Ok(nm, 4) && strcmp(nm, name) == 0) return true;
        rtti = *((void**)rtti + 1);
    }
    return false;
}
static std::vector<uintptr_t> g_hiddenShapes;     // geometry we hid (so we only un-hide what we hid)
// Walk the player's skeleton; hide (or restore) every geometry. Node-level flags get reset by the game,
// geometry-level flags stick.
static int SetBodyHidden(uintptr_t root, bool hide) {
    std::vector<uintptr_t> stack(1, root), seen; int guard = 0, changed = 0;
    while (!stack.empty() && ++guard < 4000) {
        uintptr_t o = stack.back(); stack.pop_back();
        if (!o || !Ok((void*)o, 0xB8)) continue;
        if (IsA(o, "NiNode")) {
            uintptr_t data = *(uintptr_t*)(o + 0xAC); uint16_t cnt = *(uint16_t*)(o + 0xB2);
            if (!data || cnt > 1024 || !Ok((void*)data, cnt * 4)) continue;
            for (int i = 0; i < cnt; ++i) if (((uintptr_t*)data)[i]) stack.push_back(((uintptr_t*)data)[i]);
        } else if (IsA(o, "NiGeometry")) {
            seen.push_back(o); uint32_t* fl = (uint32_t*)(o + 0x98);
            bool ours = std::find(g_hiddenShapes.begin(), g_hiddenShapes.end(), o) != g_hiddenShapes.end();
            if (hide) { if (!(*fl & 1u)) { *fl |= 1u; g_hiddenShapes.push_back(o); ++changed; } }
            else if (ours) { *fl &= ~1u; ++changed; }
        }
    }
    // forget shapes that no longer exist in the skeleton (equipment changed) - never touch freed memory
    g_hiddenShapes.erase(std::remove_if(g_hiddenShapes.begin(), g_hiddenShapes.end(), [&](uintptr_t s){ return !hide || std::find(seen.begin(), seen.end(), s) == seen.end(); }), g_hiddenShapes.end());
    return changed;
}

class PossessTask {
public:
    virtual void __attribute__((thiscall)) Run() {
        ProfScope psAll(P_POSSESS);
        static int s_run = 0; ++s_run;
        EnterCriticalSection(&g_cs);
        bool wantHidden = g_wantHidden, haveTarget = g_haveTarget; float t[3] = { g_target[0], g_target[1], g_target[2] };
        LeaveCriticalSection(&g_cs);
        uintptr_t player = Ok((void*)ADDR_PLAYER_PTR, 4) ? *(uintptr_t*)ADDR_PLAYER_PTR : 0;
        if (player && Ok((void*)player, 0x60)) {
            uintptr_t ls = *(uintptr_t*)(player + 0x44);
            uintptr_t node = (ls && Ok((void*)ls, 0x24)) ? *(uintptr_t*)(ls + 0x20) : 0;
            if (node && Ok((void*)node, 0xB8)) {                      // hide / show the player's own body
                if ((wantHidden && (s_run % 10 == 1 || !g_isHidden)) || (!wantHidden && g_isHidden)) { ProfScope ph(P_HIDE); int ch = SetBodyHidden(node, wantHidden); if (ch && g_moves < 6) LOG("possess: %s %d body shapes", wantHidden ? "hid" : "restored", ch); }
                g_isHidden = wantHidden;
            }
            if (wantHidden && haveTarget) {                            // keep the player actor on Mario
                float* pos = (float*)(player + 0x34); float* rot = (float*)(player + 0x28);
                float dx = pos[0] - t[0], dy = pos[1] - t[1], dz = pos[2] - t[2];
                static int s_drift = 0; if (s_drift < 40) { ++s_drift; LOG("possess: drift player->Mario = %.1f units", sqrtf(dx*dx + dy*dy + dz*dz)); }
                if (dx*dx + dy*dy + dz*dz > 16.0f) {          // ignore sub-4-unit differences (each move is expensive)
                    ProfScope pm(P_MOVEREFR);
                    void* cell = *(void**)(player + 0x40);
                    void* ws = ((GetWorldspace_t)ADDR_GETWORLDSP)((void*)player);
                    uint32_t handle = *(uint32_t*)ADDR_INVALIDHND;
                    float np[3] = { t[0], t[1], t[2] }, nr[3] = { rot[0], rot[1], rot[2] };
                    ((MoveRefr_t)ADDR_MOVEREFR)((void*)player, &handle, cell, ws, np, nr);
                    if (++g_moves <= 3) LOG("possess: moved player to (%.0f, %.0f, %.0f)", t[0], t[1], t[2]);
                }
            }
        }
        InterlockedExchange(&g_pending, 0);
    }
    virtual void __attribute__((thiscall)) Dispose() { delete this; }
};

static void Queue() {
    if (!g_skseTaskInterface || InterlockedCompareExchange(&g_pending, 1, 0) != 0) return;
    ((SKSETaskInterfaceLite*)g_skseTaskInterface)->AddTask(new PossessTask());
}

void PossessInit(FILE* log) { g_log = log; if (!g_csInit) { InitializeCriticalSection(&g_cs); g_csInit = true; } }

void PossessSet(bool on) {
    EnterCriticalSection(&g_cs); g_on = on; g_wantHidden = on; if (!on) g_haveTarget = false; LeaveCriticalSection(&g_cs);
    LOG("possess mode %s", on ? "ON" : "OFF"); g_moves = 0;
    Queue();
}

void PossessClearTarget() { EnterCriticalSection(&g_cs); g_haveTarget = false; LeaveCriticalSection(&g_cs); }

void PossessFollow(const float p[3]) {
    EnterCriticalSection(&g_cs); bool on = g_on; if (on) { g_target[0] = p[0]; g_target[1] = p[1]; g_target[2] = p[2]; g_haveTarget = true; } LeaveCriticalSection(&g_cs);
    if (on) Queue();
}
