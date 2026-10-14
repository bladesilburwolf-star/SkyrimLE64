// SkyrimMario milestone 2: Mario drawn as a Direct3D 9 overlay inside Skyrim LE 1.9.32.
// Mario's physics run in sm64.dll; his triangles are projected with the game's own
// camera (NiCamera::WorldPtToScreenPt3) and drawn with pre-transformed vertices at the
// end of the frame. No SKSE source is needed: the handful of game addresses used here
// come from the SKSE 1.7.3 headers (Skyrim 1.9.32.0).
//
// Keys (while the game window is focused):
//   F9            spawn / remove Mario in front of the player
//   F10           flip the screen Y axis (use if Mario appears upside-down)
//   Arrow keys    move Mario (relative to the camera)
//   Right Ctrl    A button (jump)       Right Shift  Z button (crouch / ground pound)
//   Right Alt     B button (punch / dive)
#include "sm64_api.h"
#include "collision.h"
#include "possess.h"
#include "inputblock.h"
#include "xinput.h"
#include "audio.h"
#include "prof.h"
#include <windows.h>
#include <d3d9.h>
#include <vector>
#include <algorithm>
#include <math.h>
#include <string.h>

// ---- Skyrim LE 1.9.32.0 addresses (from SKSE 1.7.3 source) ----
static const uintptr_t ADDR_PLAYER_PTR   = 0x01B2E8E4; // PlayerCharacter**
static const uintptr_t ADDR_PLAYERCAM    = 0x012E7288; // PlayerCamera**
static const uintptr_t ADDR_RENDERER     = 0x01BA76FC; // NiDX9Renderer**
static const uintptr_t ADDR_WORLD2SCREEN = 0x00AB84C0; // WorldPtToScreenPt3_Internal
struct NiPort { float left, right, top, bottom; };
typedef bool (*WorldPtToScreen_t)(float* matrix, NiPort* port, float* pIn, float* xOut, float* yOut, float* zOut, float tol);

static FILE* g_log = nullptr;
#define LOG(...) do { if (g_log) { fprintf(g_log, __VA_ARGS__); fputc('\n', g_log); fflush(g_log); } } while (0)

static bool Readable(const void* p, size_t n) { return p && !IsBadReadPtr(p, n); }
template <class T> static bool Read(uintptr_t addr, T& out) {
    if (!Readable((void*)addr, sizeof(T))) return false; out = *(T*)addr; return true;
}

// ---------------- sm64 state ----------------
static SM64Api g_api;
static std::vector<uint8_t> g_atlas;           // 704x64 RGBA from the ROM
static std::vector<float> g_pos, g_nrm, g_col, g_uv;
static SM64MarioGeometryBuffers g_gb;
static SM64MarioState g_st;
static int32_t g_mario = -1;
static float g_origin[3];                      // Skyrim-space point that SM64 (0,0,0) maps to
static const float S = 0.8f;                   // SM64 units -> Skyrim units (Mario 160 -> 128)
static bool g_flipY = false;
static LARGE_INTEGER g_freq, g_last;
static double g_accum = 0;
static int g_loggedFrames = 0;
static bool g_spawnPending = false;
static int g_diagTicks = 0;
static bool g_possess = false, g_f8 = false, g_f7 = false;
static float g_stuckRef[3]; static int g_stuckTicks = 0, g_stuckEvents = 0;
// render-time interpolation between the last two 30 Hz physics ticks (removes stepping / camera shake)
static float g_renderDelta[3] = { 0, 0, 0 };   // possess mode: shift Mario onto the player actor so he sits exactly where the camera looks
static std::vector<float> g_prevPos, g_posI;
static int g_prevTris = 0;
static float g_prevM[3], g_alpha = 1.0f;
static bool g_hadTick = false;
static LARGE_INTEGER g_lastHarvest, g_spawnTime;
static std::vector<SM64Surface> g_surf;
static const float HARVEST_RADIUS = 1000.0f;   // Skyrim units around Mario (smaller = faster surface reloads)
static void LoadFlatFloor();

// SM64 (x right, y up, z toward viewer) -> Skyrim (x east, y north, z up); proper rotation, keeps winding.
static void ToSky(const float* m, float* s) { s[0] = g_origin[0] + S * m[0]; s[1] = g_origin[1] - S * m[2]; s[2] = g_origin[2] + S * m[1]; }

// ---------------- game access ----------------
struct CamInfo { uintptr_t cam; float pos[3]; };
static bool GetCamera(CamInfo& ci) {
    uintptr_t pc, node, data, cam;
    if (!Read(ADDR_PLAYERCAM, pc) || !pc) return false;
    if (!Read(pc + 0x1C, node) || !node) return false;
    if (!Read(node + 0xAC, data) || !data) return false;   // NiNode::m_children.m_data
    if (!Read(data, cam) || !cam) return false;            // first child = NiCamera
    float nearP, farP; NiPort port;
    if (!Read(cam + 0xE8 + 0x10, nearP) || !Read(cam + 0xE8 + 0x14, farP) || !Read(cam + 0x10C, port)) return false;
    if (!(nearP > 0 && farP > nearP && port.right > port.left)) return false;
    ci.cam = cam;
    return Read(cam + 0x78, ci.pos[0]) && Read(cam + 0x7C, ci.pos[1]) && Read(cam + 0x80, ci.pos[2]);
}
static bool GetPlayer(float* pos, float* heading) {
    uintptr_t p; if (!Read(ADDR_PLAYER_PTR, p) || !p) return false;
    return Read(p + 0x34, pos[0]) && Read(p + 0x38, pos[1]) && Read(p + 0x3C, pos[2]) && Read(p + 0x30, *heading);
}

// ---------------- D3D resources ----------------
static IDirect3DTexture9* g_tex = nullptr;
static IDirect3DDevice9* g_texDev = nullptr;
static const int TEXW = 1024;
static bool EnsureTexture(IDirect3DDevice9* dev) {
    if (g_tex && g_texDev == dev) return true;
    if (g_tex) { g_tex->Release(); g_tex = nullptr; }
    if (FAILED(dev->CreateTexture(TEXW, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_tex, nullptr))) { LOG("CreateTexture failed"); g_tex = nullptr; return false; }
    D3DLOCKED_RECT lr;
    if (FAILED(g_tex->LockRect(0, &lr, nullptr, 0))) { LOG("LockRect failed"); return false; }
    for (int y = 0; y < 64; ++y) {
        uint8_t* row = (uint8_t*)lr.pBits + y * lr.Pitch; memset(row, 0, TEXW * 4);
        for (int x = 0; x < SM64_TEXTURE_WIDTH; ++x) {
            const uint8_t* p = &g_atlas[(y * SM64_TEXTURE_WIDTH + x) * 4];
            row[x*4+0] = p[2]; row[x*4+1] = p[1]; row[x*4+2] = p[0]; row[x*4+3] = p[3];
        }
    }
    g_tex->UnlockRect(0); g_texDev = dev; LOG("Mario texture uploaded"); return true;
}

struct Vtx { float x, y, z, rhw; DWORD color; float u, v; };
static const DWORD FVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

// ---------------- Mario control / tick ----------------
static bool Focused() { DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid); return pid == GetCurrentProcessId(); }
static bool Key(int vk) { return Focused() && (GetAsyncKeyState(vk) & 0x8000); }

static void Spawn() {
    float pp[3], hd; if (!GetPlayer(pp, &hd)) { LOG("spawn: player not available"); return; }
    g_origin[0] = pp[0] + sinf(hd) * 150.0f; g_origin[1] = pp[1] + cosf(hd) * 150.0f; g_origin[2] = pp[2];
    if (g_mario >= 0) { { SM64Lock lk; g_api.mario_delete(g_mario); } g_mario = -1; }
    g_diagTicks = 0; g_spawnPending = true; QueryPerformanceCounter(&g_spawnTime); g_lastHarvest.QuadPart = 0;
    LOG("Spawn requested at Skyrim (%.0f, %.0f, %.0f) - waiting for collision harvest", g_origin[0], g_origin[1], g_origin[2]);
}
static void Despawn() { if (g_possess) { g_possess = false; PossessSet(false); InputBlockSet(false); XInputBlockSet(false); } if (g_mario >= 0) { SM64Lock lk; g_api.mario_delete(g_mario); } g_mario = -1; g_spawnPending = false; LOG("Mario removed"); }
static void FinishSpawn() {
    { SM64Lock lk; g_mario = g_api.mario_create(0, 125, 0); }                    // ~100 Skyrim units above the player's feet
    if (g_mario < 0) { LOG("mario_create failed with real collision; using flat floor"); LoadFlatFloor(); { SM64Lock lk; g_mario = g_api.mario_create(0, 50, 0); } }
    g_spawnPending = false; g_accum = 0; QueryPerformanceCounter(&g_last); g_loggedFrames = 0; g_hadTick = false;
    LOG("Mario spawned id=%d", g_mario);
}
// Every 0.5 s: queue a harvest around Mario, and load any finished harvest into sm64.dll.
static void UpdateCollision() {
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    {
        float c[3]; if (g_mario >= 0) ToSky(g_st.position, c); else { c[0] = g_origin[0]; c[1] = g_origin[1]; c[2] = g_origin[2]; }
        static float s_lastC[3]; float dx = c[0]-s_lastC[0], dy = c[1]-s_lastC[1], dz = c[2]-s_lastC[2];
        double since = g_lastHarvest.QuadPart ? double(now.QuadPart - g_lastHarvest.QuadPart) / g_freq.QuadPart : 1e9;
        // The harvest covers a 1000-unit radius. Keep a generous movement margin
        // while avoiding a full scene walk every few seconds in stationary play.
        if (g_lastHarvest.QuadPart == 0 || dx*dx + dy*dy + dz*dz > 500.f * 500.f || since > 8.0) {
            CollisionRequest(c, HARVEST_RADIUS, g_origin, S); g_lastHarvest = now; s_lastC[0] = c[0]; s_lastC[1] = c[1]; s_lastC[2] = c[2];
        }
    }
    if (CollisionFetch(g_surf)) {
        if (!g_surf.empty()) {
            uint64_t h = 1469598103934665603ULL; const uint8_t* b = (const uint8_t*)g_surf.data(); size_t nb = g_surf.size() * sizeof(SM64Surface);
            for (size_t i = 0; i < nb; ++i) { h ^= b[i]; h *= 1099511628211ULL; }
            static uint64_t s_lastHash = 0;
            if (h != s_lastHash) { s_lastHash = h; ProfScope ps(P_SURFLOAD); SM64Lock lk; g_api.static_surfaces_load(g_surf.data(), (uint32_t)g_surf.size()); }
            if (g_spawnPending) FinishSpawn(); }
        else if (g_spawnPending && double(now.QuadPart - g_spawnTime.QuadPart) / g_freq.QuadPart > 3.0) { LOG("no collision found nearby; using flat floor"); LoadFlatFloor(); FinishSpawn(); }
    }
    if (g_spawnPending && double(now.QuadPart - g_spawnTime.QuadPart) / g_freq.QuadPart > 8.0) { LOG("collision harvest never returned; using flat floor"); LoadFlatFloor(); FinishSpawn(); }
}

static void TickMario(const CamInfo& ci) {
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    g_accum += double(now.QuadPart - g_last.QuadPart) / double(g_freq.QuadPart); g_last = now;
    if (g_accum > 0.2) g_accum = 0.2;
    SM64MarioInputs in = {};
    // camera position in SM64 space (inverse of ToSky)
    float cx = (ci.pos[0] - g_origin[0]) / S, cz = -(ci.pos[1] - g_origin[1]) / S;
    in.camLookX = g_st.position[0] - cx; in.camLookZ = g_st.position[2] - cz;
    bool L = Key(VK_LEFT) || (g_possess && Key('A')), Rr = Key(VK_RIGHT) || (g_possess && Key('D'));
    bool U = Key(VK_UP) || (g_possess && Key('W')), D = Key(VK_DOWN) || (g_possess && Key('S'));
    float sx = (Rr ? 1.f : 0.f) - (L ? 1.f : 0.f);
    float sy = (D ? 1.f : 0.f) - (U ? 1.f : 0.f);   // up = -1 (joystick convention)
    MarioPad pad = {}; if (g_possess) pad = XInputReadPad();
    sx += pad.sx; sy += pad.sy;
    float mag = sqrtf(sx*sx + sy*sy); if (mag > 1) { sx /= mag; sy /= mag; }
    in.stickX = sx; in.stickY = sy;
    in.buttonA = Key(VK_RCONTROL) || (g_possess && Key(VK_SPACE)) || pad.a;
    in.buttonZ = Key(VK_RSHIFT) || (g_possess && Key(VK_LCONTROL)) || pad.z;
    in.buttonB = Key(VK_RMENU) || (g_possess && Key('F')) || pad.b;
    while (g_accum >= 1.0 / 30.0) {
        if (g_hadTick) {                                   // remember the previous tick for interpolation
            if (g_prevPos.size() != g_pos.size()) g_prevPos.assign(g_pos.size(), 0.f);
            memcpy(g_prevPos.data(), g_pos.data(), sizeof(float) * 9 * g_gb.numTrianglesUsed);
            g_prevTris = g_gb.numTrianglesUsed; memcpy(g_prevM, g_st.position, sizeof g_prevM);
        }
        { SM64Lock lk; g_api.mario_tick(g_mario, &in, &g_st, &g_gb); } g_accum -= 1.0 / 30.0;
        if (!g_hadTick) { g_hadTick = true; g_prevPos = g_pos; g_prevTris = g_gb.numTrianglesUsed; memcpy(g_prevM, g_st.position, sizeof g_prevM); }
        // stuck detector: pushing the stick but barely moving for half a second -> log it and dump nearby collision once
        { float mag2 = in.stickX*in.stickX + in.stickY*in.stickY;
          if (mag2 > 0.25f && !(g_st.action & 0x00000800u /*airborne-ish*/)) {
              if (++g_stuckTicks == 1) memcpy(g_stuckRef, g_st.position, sizeof g_stuckRef);
              if (g_stuckTicks >= 15) {
                  float dx = g_st.position[0]-g_stuckRef[0], dz = g_st.position[2]-g_stuckRef[2];
                  if (dx*dx + dz*dz < 8.f * 8.f && g_stuckEvents < 6) { ++g_stuckEvents;
                      LOG("STUCK? mario at SM64 (%.0f, %.0f, %.0f) action=0x%08X fwdVel=%.1f stick=(%.1f,%.1f)", g_st.position[0], g_st.position[1], g_st.position[2], g_st.action, g_st.forwardVelocity, in.stickX, in.stickY);
                      float c[3]; ToSky(g_st.position, c); CollisionDebugNext(); CollisionRequest(c, HARVEST_RADIUS, g_origin, S); }
                  g_stuckTicks = 0;
              }
          } else g_stuckTicks = 0; }
        if (++g_diagTicks % 30 == 0 && g_diagTicks <= 30 * 20) {   // once a second for the first 20 s
            float fh; { SM64Lock lk; fh = g_api.surface_find_floor_height(g_st.position[0], g_st.position[1] + 100.f, g_st.position[2]); }
            LOG("mario: pos=(%.0f, %.0f, %.0f) action=0x%08X fwdVel=%.1f stick=(%.1f,%.1f) A=%d floorHeightBelow=%.0f (SM64 units; 0 = player's feet level)", g_st.position[0], g_st.position[1], g_st.position[2], g_st.action, g_st.forwardVelocity, in.stickX, in.stickY, (int)in.buttonA, fh);
        }
    }
    g_alpha = (float)(g_accum * 30.0); if (g_alpha < 0) g_alpha = 0; if (g_alpha > 1) g_alpha = 1;
}

// ---------------- menu detection ----------------
// Real addresses from SKSE 1.7.3 source: MenuManager singleton 0x012E3548, MenuManager::IsMenuOpen 0x00A5CE90,
// BSFixedString ctor 0x00A511C0. Any "blocking" menu (journal, inventory, map, console, loading, dialogue...) hides Mario.
static const uintptr_t ADDR_MENUMGR = 0x012E3548, ADDR_ISMENUOPEN = 0x00A5CE90, ADDR_BSFIXED_CTOR = 0x00A511C0;
typedef void* (__attribute__((thiscall)) *BSFixedCtor_t)(void* self, const char* s);
typedef bool  (__attribute__((thiscall)) *IsMenuOpen_t)(void* mm, void* name);
static bool CheckMenuOpen() {
    static const char* names[] = { "Cursor Menu", "Loading Menu", "MainMenu", "Journal Menu", "TweenMenu", "InventoryMenu", "MagicMenu",
        "MapMenu", "Console", "ContainerMenu", "BarterMenu", "GiftMenu", "Crafting Menu", "Lockpicking Menu", "Book Menu", "Dialogue Menu",
        "Sleep/Wait Menu", "MessageBoxMenu", "Training Menu", "LevelUp Menu", "StatsMenu", "Favorites Menu", "RaceSex Menu", "Fader Menu" };
    const int N = sizeof(names) / sizeof(names[0]);
    static void* cache[24]; static bool init = false;
    uintptr_t mm = 0; if (!Read(ADDR_MENUMGR, mm) || !mm) return false;
    if (!init) { init = true; for (int i = 0; i < N; ++i) { cache[i] = nullptr; ((BSFixedCtor_t)ADDR_BSFIXED_CTOR)(&cache[i], names[i]); } LOG("menu detection initialised (%d menu names)", N); }
    static int s_last = -2, s_logs = 0; int hit = -1;
    for (int i = 0; i < N; ++i) if (cache[i] && ((IsMenuOpen_t)ADDR_ISMENUOPEN)((void*)mm, &cache[i])) { hit = i; break; }
    if (hit != s_last && s_logs < 40) { ++s_logs; s_last = hit; LOG("menu state: %s", hit >= 0 ? names[hit] : "(none - gameplay)"); }
    return hit >= 0;
}
static bool g_menuOpen = false;

// ---------------- drawing ----------------
static WorldPtToScreen_t Project = (WorldPtToScreen_t)ADDR_WORLD2SCREEN;

static std::vector<Vtx> va, vb;
static bool g_haveGeom = false;
// Builds this frame's screen-space triangles (once per frame).
static void BuildGeom(const CamInfo& ci, float W, float H) {
    g_haveGeom = false;
    int n = g_gb.numTrianglesUsed; if (n <= 0) return;
    g_posI.resize(9 * n);
    if (g_hadTick && g_prevTris == n) for (int i = 0; i < 9 * n; ++i) g_posI[i] = g_prevPos[i] + (g_pos[i] - g_prevPos[i]) * g_alpha;
    else memcpy(g_posI.data(), g_pos.data(), sizeof(float) * 9 * n);
    NiPort port; float* mtx = (float*)(ci.cam + 0xA8); Read(ci.cam + 0x10C, port);
    struct T { float depth; int i; Vtx a[3], b[3]; bool textured; };
    static std::vector<T> tris; tris.clear(); tris.reserve(n);
    int projFail = 0;
    for (int t = 0; t < n; ++t) {
        T tr; tr.i = t; bool ok = true; float cd = 0;
        tr.textured = !(g_uv[t*6+0] == 1.f && g_uv[t*6+1] == 1.f && g_uv[t*6+2] == 1.f && g_uv[t*6+3] == 1.f);
        for (int v = 0; v < 3 && ok; ++v) {
            float sky[3]; ToSky(&g_posI[t*9 + v*3], sky); sky[0] += g_renderDelta[0]; sky[1] += g_renderDelta[1]; sky[2] += g_renderDelta[2];
            float x, y, z;
            if (!Project(mtx, &port, sky, &x, &y, &z, 1e-5f)) { ok = false; break; }
            float px = x * W, py = (g_flipY ? y : 1.0f - y) * H;
            float dx = sky[0]-ci.pos[0], dy = sky[1]-ci.pos[1], dz = sky[2]-ci.pos[2]; cd += dx*dx + dy*dy + dz*dz;
            const float* c = &g_col[t*9 + v*3]; const float* nm = &g_nrm[t*9 + v*3];
            float ndl = nm[0]*0.30f + nm[1]*0.80f + nm[2]*0.50f; if (ndl < 0) ndl = 0;
            int sh = (int)(255.f * std::min(1.f, 0.55f + 0.45f * ndl));
            auto B = [](float f){ int i = (int)(f * 255.f); return (DWORD)(i < 0 ? 0 : i > 255 ? 255 : i); };
            tr.a[v] = { px, py, 0.f, 1.f, 0xFF000000u | (B(c[0]) << 16) | (B(c[1]) << 8) | B(c[2]), 0.f, 0.f };
            tr.b[v] = { px, py, 0.f, 1.f, 0xFF000000u | (sh << 16) | (sh << 8) | sh, g_uv[t*6 + v*2] * (float(SM64_TEXTURE_WIDTH) / TEXW), g_uv[t*6 + v*2 + 1] };
        }
        if (!ok) { ++projFail; continue; }
        tr.depth = cd; tris.push_back(tr);
    }
    if (g_loggedFrames < 5) { LOG("frame: mario tris=%d drawn=%d projFail=%d pos=(%.0f,%.0f,%.0f) cam=(%.0f,%.0f,%.0f) screen=%.0fx%.0f",
        n, (int)tris.size(), projFail, g_st.position[0], g_st.position[1], g_st.position[2], ci.pos[0], ci.pos[1], ci.pos[2], W, H); ++g_loggedFrames; }
    if (tris.empty()) return;
    std::sort(tris.begin(), tris.end(), [](const T& a, const T& b){ return a.depth > b.depth; }); // far first (painter)

    va.clear(); vb.clear();
    for (auto& t : tris) { for (int v = 0; v < 3; ++v) va.push_back(t.a[v]); if (t.textured) for (int v = 0; v < 3; ++v) vb.push_back(t.b[v]); }
    g_haveGeom = !va.empty();
}

// Draws the cached triangles. Called at EVERY end-of-scene that targets the backbuffer, so the
// last one of the frame (on top of everything the game drew) is the one that stays visible.
static void IssueDraw(IDirect3DDevice9* dev) {
    if (!g_haveGeom || va.empty()) return;

    IDirect3DStateBlock9* sb = nullptr;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb)) || !sb) return;
    sb->Capture();
    dev->SetVertexShader(nullptr); dev->SetPixelShader(nullptr); dev->SetFVF(FVF);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE); dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE); dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE); dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE); dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF); dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE); dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA); dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE); dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT); dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);

    // pass 1: vertex-colored, untextured
    dev->SetTexture(0, nullptr);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1); dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1); dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)va.size() / 3, va.data(), sizeof(Vtx));
    // pass 2: textured parts blended on top
    if (!vb.empty() && EnsureTexture(dev)) {
        dev->SetTexture(0, g_tex);
        dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE); dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE); dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE); dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE); dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)vb.size() / 3, vb.data(), sizeof(Vtx));
    }
    sb->Apply(); sb->Release();
}

static bool g_builtThisFrame = false, g_inHook = false, g_f9 = false, g_f10 = false;
// The game ends several scenes on the back buffer per frame. Drawing (and the heavy D3D state capture) at every one of
// them was wasteful; draw only from the scene index that was the last one in each of the recent frames.
static int g_sceneIdx = 0, g_sceneHist[4] = { 0, 0, 0, 0 }, g_sceneHistPos = 0;
static int DrawFromScene() { int m = 1 << 30; for (int i = 0; i < 4; ++i) if (g_sceneHist[i] > 0 && g_sceneHist[i] < m) m = g_sceneHist[i]; return m == (1 << 30) ? 1 : m; }
static int g_drawCount = 0;
static void OverlayFrame(IDirect3DDevice9* dev) {
    IDirect3DSurface9 *rt = nullptr, *bb = nullptr;
    dev->GetRenderTarget(0, &rt); dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    bool isBack = rt && bb && rt == bb;
    D3DSURFACE_DESC d = {}; if (isBack) bb->GetDesc(&d);
    if (rt) rt->Release();
    if (bb) bb->Release();
    if (!isBack) return;
    ++g_sceneIdx;
    ProfScope pfFrame(P_FRAME);

    if (!g_builtThisFrame) {                 // once per frame: input, physics tick, projection
        g_builtThisFrame = true; g_haveGeom = false; g_drawCount = 0;
        g_menuOpen = CheckMenuOpen();
        InputBlockSet(g_possess && !g_menuOpen); XInputBlockSet(g_possess && !g_menuOpen);       // hide Mario's keys from the game while possessing
        bool f9 = !g_menuOpen && Key(VK_F9), f10 = !g_menuOpen && Key(VK_F10);
        if (f9 && !g_f9) { if (g_mario >= 0) Despawn(); else Spawn(); }
        if (f10 && !g_f10) { g_flipY = !g_flipY; LOG("flipY=%d", (int)g_flipY); }
        bool f7 = !g_menuOpen && Key(VK_F7);
        if (f7 && !g_f7 && g_mario >= 0) { float c[3]; ToSky(g_st.position, c); CollisionDebugNext(); CollisionRequest(c, HARVEST_RADIUS, g_origin, S); LOG("F7: collision debug dump requested"); }
        g_f7 = f7;
        bool f8 = !g_menuOpen && Key(VK_F8);
        if (f8 && !g_f8 && g_mario >= 0) { g_possess = !g_possess; PossessSet(g_possess); }
        g_f8 = f8;
        g_f9 = f9; g_f10 = f10;
        CamInfo ci;
        if (g_menuOpen) QueryPerformanceCounter(&g_last);   // paused: don't accumulate time
        if ((g_mario >= 0 || g_spawnPending) && !g_menuOpen) { ProfScope ps(P_COLLUPD); UpdateCollision(); }
        if (g_mario >= 0 && !g_menuOpen && GetCamera(ci)) {
            { ProfScope ps(P_TICK); TickMario(ci); }
            float mi[3] = { g_prevM[0] + (g_st.position[0]-g_prevM[0])*g_alpha, g_prevM[1] + (g_st.position[1]-g_prevM[1])*g_alpha, g_prevM[2] + (g_st.position[2]-g_prevM[2])*g_alpha };
            float ms[3]; ToSky(mi, ms);
            g_renderDelta[0] = g_renderDelta[1] = g_renderDelta[2] = 0;
            if (g_possess) {                       // camera lag fix: Skyrim's camera follows the player actor, so draw Mario ON the actor
                float pp[3], hd;
                if (GetPlayer(pp, &hd)) { float dx = pp[0]-ms[0], dy = pp[1]-ms[1], dz = pp[2]-ms[2];
                    if (dx*dx + dy*dy + dz*dz < 150.f * 150.f) { g_renderDelta[0] = dx; g_renderDelta[1] = dy; g_renderDelta[2] = dz; } }
            }
            { ProfScope ps(P_BUILD); BuildGeom(ci, (float)d.Width, (float)d.Height); }
            if (g_possess) PossessFollow(ms);
        }
        AudioSetActive(g_mario >= 0 && !g_menuOpen);
    }
    if (g_haveGeom && !g_menuOpen && g_sceneIdx >= DrawFromScene()) { ProfScope ps(P_DRAW); IssueDraw(dev); if (g_loggedFrames < 6 && ++g_drawCount > 1) LOG("  (backbuffer end-of-scene #%d this frame)", g_drawCount); }
}

// ---------------- vtable hooks ----------------
typedef HRESULT (WINAPI *EndScene_t)(IDirect3DDevice9*);
typedef HRESULT (WINAPI *Present_t)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
static EndScene_t g_origEndScene = nullptr; static Present_t g_origPresent = nullptr;
static HRESULT WINAPI HookEndScene(IDirect3DDevice9* dev) {
    if (!g_inHook) { g_inHook = true; OverlayFrame(dev); g_inHook = false; }
    return g_origEndScene(dev);
}
static HRESULT WINAPI HookPresent(IDirect3DDevice9* dev, const RECT* a, const RECT* b, HWND c, const RGNDATA* d) {
    ProfAdd(P_SCENES, (double)g_sceneIdx); g_sceneHist[g_sceneHistPos++ & 3] = g_sceneIdx; g_sceneIdx = 0;
    ProfDumpIfDue(g_log);
    g_builtThisFrame = false;
    return g_origPresent(dev, a, b, c, d);
}
static bool PatchVtable(void** vt, int idx, void* hook, void** orig) {
    DWORD old; if (!VirtualProtect(&vt[idx], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
    *orig = vt[idx]; vt[idx] = hook; VirtualProtect(&vt[idx], sizeof(void*), old, &old); return true;
}

static void LoadFlatFloor() {
    const int32_t F = 8000; static SM64Surface floor[2] = {};
    int32_t a[3][3] = {{-F,0,F},{F,0,F},{F,0,-F}}, b[3][3] = {{-F,0,F},{F,0,-F},{-F,0,-F}};
    memcpy(floor[0].vertices, a, sizeof a); memcpy(floor[1].vertices, b, sizeof b);
    SM64Lock lk; g_api.static_surfaces_load(floor, 2);
}

// ---------------- entry point ----------------
bool OverlayRun(const std::string& dir, FILE* log) {
    g_log = log; QueryPerformanceFrequency(&g_freq); ProfInit();
    if (!LoadSM64(dir + "\\sm64.dll", g_api, log)) return false;
    FILE* rf = fopen((dir + "\\baserom.us.z64").c_str(), "rb");
    if (!rf) { LOG("ERROR: cannot open baserom.us.z64 in %s", dir.c_str()); return false; }
    std::vector<uint8_t> rom(8 * 1024 * 1024 + 16); size_t n = fread(rom.data(), 1, rom.size(), rf); fclose(rf);
    if (n != 8388608 || rom[0] != 0x80) { LOG("ERROR: bad ROM (need big-endian 8MB .z64)"); return false; }
    g_atlas.assign(4 * SM64_TEXTURE_WIDTH * SM64_TEXTURE_HEIGHT, 0);
    g_api.global_init(rom.data(), g_atlas.data());
    g_api.audio_init(rom.data()); g_api.set_sound_volume(0.8f); AudioStart(&g_api, log);
    LoadFlatFloor();
    CollisionInit(log); PossessInit(log); InputBlockInstall(log); XInputInstall(log);
    g_pos.assign(9 * SM64_GEO_MAX_TRIANGLES, 0); g_nrm = g_pos; g_col = g_pos; g_uv.assign(6 * SM64_GEO_MAX_TRIANGLES, 0);
    g_gb = { g_pos.data(), g_nrm.data(), g_col.data(), g_uv.data(), 0 };
    LOG("SM64 initialised. Waiting for the Direct3D device...");

    IDirect3DDevice9* dev = nullptr;
    for (int i = 0; i < 1200 && !dev; ++i) {            // up to ~5 minutes
        uintptr_t r = 0, d = 0;
        if (Read(ADDR_RENDERER, r) && r && Read(r + 0x38, d) && d) dev = (IDirect3DDevice9*)d; else Sleep(250);
    }
    if (!dev) { LOG("ERROR: renderer/device never became available"); return false; }
    void** vt = *(void***)dev;
    bool ok = PatchVtable(vt, 17, (void*)HookPresent, (void**)&g_origPresent) &&
              PatchVtable(vt, 42, (void*)HookEndScene, (void**)&g_origEndScene);
    LOG(ok ? "D3D9 hooks installed (device %p). Press F9 in-game to spawn Mario." : "ERROR: failed to patch D3D9 vtable", (void*)dev);
    return ok;
}
