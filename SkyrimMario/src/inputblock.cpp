#define DIRECTINPUT_VERSION 0x0800
#include "inputblock.h"
#include <windows.h>
#include <dinput.h>
#include <map>
#include <string.h>

static FILE* g_log = nullptr;
#define LOG(...) do { if (g_log) { fprintf(g_log, __VA_ARGS__); fputc('\n', g_log); fflush(g_log); } } while (0)

static volatile bool g_block = false;
void InputBlockSet(bool b) { g_block = b; }

// DirectInput keyboard scan codes that drive Mario in possess mode
static bool Blocked(DWORD sc) { return sc == 0x11 /*W*/ || sc == 0x1E /*A*/ || sc == 0x1F /*S*/ || sc == 0x20 /*D*/ || sc == 0x39 /*Space*/ || sc == 0x1D /*LCtrl*/ || sc == 0x21 /*F*/; }

static const GUID kIID_DI8A = { 0xBF798030, 0x483A, 0x4DA2, { 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00 } };
static const GUID kIID_DI8W = { 0xBF798031, 0x483A, 0x4DA2, { 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00 } };
static const GUID kGUID_SysKeyboard = { 0x6F1D2B61, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };

typedef HRESULT (WINAPI *GetState_t)(void*, DWORD, LPVOID);
typedef HRESULT (WINAPI *GetData_t)(void*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
typedef HRESULT (WINAPI *GetInfoA_t)(void*, LPDIDEVICEINSTANCEA);
typedef HRESULT (WINAPI *GetInfoW_t)(void*, LPDIDEVICEINSTANCEW);

static std::map<void*, bool> g_isKbd;
static CRITICAL_SECTION g_cs; static bool g_csInit = false;
static bool IsKeyboard(void* self, bool wide) {
    EnterCriticalSection(&g_cs);
    auto it = g_isKbd.find(self); if (it != g_isKbd.end()) { bool r = it->second; LeaveCriticalSection(&g_cs); return r; }
    LeaveCriticalSection(&g_cs);
    bool kb = false; void** vt = *(void***)self;
    if (!wide) { DIDEVICEINSTANCEA di = {}; di.dwSize = sizeof di; if (SUCCEEDED(((GetInfoA_t)vt[15])(self, &di))) kb = (di.dwDevType & 0xFF) == DI8DEVTYPE_KEYBOARD; }
    else       { DIDEVICEINSTANCEW di = {}; di.dwSize = sizeof di; if (SUCCEEDED(((GetInfoW_t)vt[15])(self, &di))) kb = (di.dwDevType & 0xFF) == DI8DEVTYPE_KEYBOARD; }
    EnterCriticalSection(&g_cs); g_isKbd[self] = kb; LeaveCriticalSection(&g_cs);
    return kb;
}

static void FilterState(BYTE* st) { for (int sc = 0; sc < 256; ++sc) if (Blocked(sc)) st[sc] = 0; }
static void FilterData(LPDIDEVICEOBJECTDATA d, DWORD cb, LPDWORD n) {
    if (!d || !n || !*n) return;
    BYTE* base = (BYTE*)d; DWORD out = 0;
    for (DWORD i = 0; i < *n; ++i) {
        DIDEVICEOBJECTDATA* e = (DIDEVICEOBJECTDATA*)(base + i * cb);
        bool press = (e->dwData & 0x80) != 0;
        if (Blocked(e->dwOfs) && press) continue;               // drop presses; releases still pass (no stuck keys)
        if (out != i) memcpy(base + out * cb, e, cb);
        ++out;
    }
    *n = out;
}

// one hook set per vtable flavour (A / W)
static GetState_t oSA = nullptr, oSW = nullptr; static GetData_t oDA = nullptr, oDW = nullptr;
#define MAKE_HOOKS(SUF, WIDE) \
static HRESULT WINAPI HookState##SUF(void* self, DWORD cb, LPVOID p) { \
    HRESULT hr = o##S##SUF(self, cb, p); \
    if (SUCCEEDED(hr) && g_block && cb == 256 && p && IsKeyboard(self, WIDE)) FilterState((BYTE*)p); return hr; } \
static HRESULT WINAPI HookData##SUF(void* self, DWORD cb, LPDIDEVICEOBJECTDATA d, LPDWORD n, DWORD fl) { \
    HRESULT hr = o##D##SUF(self, cb, d, n, fl); \
    if (SUCCEEDED(hr) && g_block && d && n && cb >= 16 && IsKeyboard(self, WIDE)) FilterData(d, cb, n); return hr; }
MAKE_HOOKS(A, false)
MAKE_HOOKS(W, true)

static bool Patch(void** vt, int idx, void* hook, void** orig) {
    DWORD old; if (!VirtualProtect(&vt[idx], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
    *orig = vt[idx]; vt[idx] = hook; VirtualProtect(&vt[idx], sizeof(void*), old, &old); return true;
}

bool InputBlockInstall(FILE* log) {
    g_log = log; if (!g_csInit) { InitializeCriticalSection(&g_cs); g_csInit = true; }
    HMODULE m = GetModuleHandleA("dinput8.dll"); if (!m) m = LoadLibraryA("dinput8.dll");
    typedef HRESULT (WINAPI *Create_t)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    Create_t create = m ? (Create_t)GetProcAddress(m, "DirectInput8Create") : nullptr;
    if (!create) { LOG("inputblock: dinput8.dll / DirectInput8Create not found"); return false; }
    bool okA = false, okW = false;
    {   // ANSI flavour
        void* di = nullptr;
        if (SUCCEEDED(create(GetModuleHandleA(nullptr), 0x0800, kIID_DI8A, &di, nullptr)) && di) {
            typedef HRESULT (WINAPI *CreateDev_t)(void*, REFGUID, void**, LPUNKNOWN);
            void* dev = nullptr; if (SUCCEEDED(((CreateDev_t)(*(void***)di)[3])(di, kGUID_SysKeyboard, &dev, nullptr)) && dev) {
                void** vt = *(void***)dev;
                okA = Patch(vt, 9, (void*)HookStateA, (void**)&oSA) && Patch(vt, 10, (void*)HookDataA, (void**)&oDA);
            }
        }
    }
    {   // wide flavour (separate vtable)
        void* di = nullptr;
        if (SUCCEEDED(create(GetModuleHandleA(nullptr), 0x0800, kIID_DI8W, &di, nullptr)) && di) {
            typedef HRESULT (WINAPI *CreateDev_t)(void*, REFGUID, void**, LPUNKNOWN);
            void* dev = nullptr; if (SUCCEEDED(((CreateDev_t)(*(void***)di)[3])(di, kGUID_SysKeyboard, &dev, nullptr)) && dev) {
                void** vt = *(void***)dev;
                okW = Patch(vt, 9, (void*)HookStateW, (void**)&oSW) && Patch(vt, 10, (void*)HookDataW, (void**)&oDW);
            }
        }
    }
    LOG("inputblock: keyboard hooks installed (ANSI=%d, wide=%d)", (int)okA, (int)okW);
    return okA || okW;
}
