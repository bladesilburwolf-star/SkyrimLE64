#include "xinput.h"
#include <windows.h>
#include <xinput.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

static FILE* g_log = nullptr;
#define LOG(...) do { if (g_log) { fprintf(g_log, __VA_ARGS__); fputc('\n', g_log); fflush(g_log); } } while (0)
typedef DWORD (WINAPI *GetState_t)(DWORD, XINPUT_STATE*);
static GetState_t g_orig = nullptr;
static volatile bool g_block = false;
void XInputBlockSet(bool b) { g_block = b; }

static DWORD WINAPI HookGetState(DWORD idx, XINPUT_STATE* st) {
    DWORD r = g_orig(idx, st);
    if (r == ERROR_SUCCESS && idx == 0 && st && g_block) {
        st->Gamepad.sThumbLX = 0; st->Gamepad.sThumbLY = 0; st->Gamepad.bLeftTrigger = 0;
        st->Gamepad.wButtons &= ~(XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_X);
    }
    return r;
}

static bool PatchIAT(HMODULE mod, void* hook) {
    BYTE* base = (BYTE*)mod; IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base; IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]; if (!dir.VirtualAddress) return false;
    for (IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); d->Name; ++d) {
        if (_strnicmp((const char*)(base + d->Name), "xinput", 6) != 0) continue;
        IMAGE_THUNK_DATA* names = (IMAGE_THUNK_DATA*)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        IMAGE_THUNK_DATA* thunk = (IMAGE_THUNK_DATA*)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++thunk) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME* ibn = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            if (strcmp((const char*)ibn->Name, "XInputGetState") != 0) continue;
            DWORD old; if (!VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) return false;
            g_orig = (GetState_t)thunk->u1.Function; thunk->u1.Function = (DWORD_PTR)hook; VirtualProtect(&thunk->u1.Function, sizeof(void*), old, &old);
            LOG("xinput: patched import table (%s)", (const char*)(base + d->Name)); return true;
        }
    }
    return false;
}
// fallback: Windows "hot-patchable" function (mov edi,edi preceded by 5 padding bytes)
static bool HotPatch(HMODULE m, void* hook) {
    BYTE* f = (BYTE*)GetProcAddress(m, "XInputGetState"); if (!f || IsBadReadPtr(f - 5, 7)) return false;
    if (!(f[0] == 0x8B && f[1] == 0xFF)) return false;
    for (int i = 1; i <= 5; ++i) if (f[-i] != 0xCC && f[-i] != 0x90) return false;
    DWORD old; if (!VirtualProtect(f - 5, 7, PAGE_EXECUTE_READWRITE, &old)) return false;
    f[-5] = 0xE9; *(int32_t*)(f - 4) = (int32_t)((BYTE*)hook - f);     // jmp hook  (rel to f-5+5 = f)
    f[0] = 0xEB; f[1] = 0xF9;                                          // jmp short -7 -> f-5
    g_orig = (GetState_t)(f + 2); VirtualProtect(f - 5, 7, old, &old); FlushInstructionCache(GetCurrentProcess(), f - 5, 7);
    LOG("xinput: hot-patched XInputGetState"); return true;
}

bool XInputInstall(FILE* log) {
    g_log = log;
    bool ok = PatchIAT(GetModuleHandleA(nullptr), (void*)HookGetState);
    if (!ok) { const char* dlls[] = { "xinput1_3.dll", "xinput1_4.dll", "xinput9_1_0.dll" };
        for (auto n : dlls) { HMODULE m = GetModuleHandleA(n); if (m && HotPatch(m, (void*)HookGetState)) { ok = true; break; } } }
    if (!g_orig) {      // no hook: still allow reading the pad directly
        const char* dlls[] = { "xinput1_3.dll", "xinput1_4.dll", "xinput9_1_0.dll" };
        for (auto n : dlls) { HMODULE m = LoadLibraryA(n); if (m) { g_orig = (GetState_t)GetProcAddress(m, "XInputGetState"); if (g_orig) break; } }
    }
    LOG(ok ? "xinput: hook installed (Skyrim will not see Mario's gamepad controls while possessing)" : "xinput: WARNING hook NOT installed - the game will also react to the gamepad (reading works: %d)", (int)(g_orig != nullptr));
    return ok;
}

MarioPad XInputReadPad() {
    MarioPad p = {}; if (!g_orig) return p;
    XINPUT_STATE st = {}; if (g_orig(0, &st) != ERROR_SUCCESS) return p;
    p.connected = true;
    float x = st.Gamepad.sThumbLX / 32768.f, y = st.Gamepad.sThumbLY / 32768.f, m = sqrtf(x*x + y*y), dz = 7849.f / 32768.f;
    if (m > dz) { float s = (m - dz) / (1.f - dz); if (s > 1) s = 1; p.sx = x / m * s; p.sy = -(y / m * s); }
    p.a = (st.Gamepad.wButtons & XINPUT_GAMEPAD_A) != 0;
    p.b = (st.Gamepad.wButtons & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_X)) != 0;
    p.z = st.Gamepad.bLeftTrigger > 100;
    return p;
}
