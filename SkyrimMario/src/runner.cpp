#include "sm64_api.h"
#include <windows.h>
#include <vector>
#include <math.h>
#include <string.h>

static CRITICAL_SECTION g_lockCs; static bool g_lockInit = false;
void SM64LockEnter() { if (!g_lockInit) { InitializeCriticalSection(&g_lockCs); g_lockInit = true; } EnterCriticalSection(&g_lockCs); }
void SM64LockLeave() { LeaveCriticalSection(&g_lockCs); }
static FILE* g_log = nullptr;
static void DebugPrint(const char* s) { if (g_log) { fprintf(g_log, "[sm64] %s\n", s); fflush(g_log); } }

#define LOADFN(name) do { api.name = (decltype(api.name))GetProcAddress(h, "sm64_" #name); \
    if (!api.name) { fprintf(log, "ERROR: sm64.dll is missing export sm64_%s\n", #name); ok = false; } } while (0)

bool LoadSM64(const std::string& dllPath, SM64Api& api, FILE* log)
{
    HMODULE h = LoadLibraryA(dllPath.c_str());
    if (!h) { fprintf(log, "ERROR: LoadLibrary failed for %s (Win32 error %lu). 32-bit DLL required.\n", dllPath.c_str(), GetLastError()); return false; }
    bool ok = true;
    LOADFN(register_debug_print_function); LOADFN(global_init); LOADFN(global_terminate);
    LOADFN(static_surfaces_load); LOADFN(mario_create); LOADFN(mario_tick); LOADFN(mario_delete); LOADFN(surface_find_floor_height); LOADFN(audio_init); LOADFN(audio_tick); LOADFN(set_sound_volume); LOADFN(set_mario_position);
    return ok;
}

static bool WriteBMP(const std::string& path, const uint8_t* rgba, int w, int h)
{
    FILE* f = fopen(path.c_str(), "wb"); if (!f) return false;
    uint32_t imgSize = w * h * 4, off = 54, fileSize = off + imgSize;
    uint8_t hdr[54] = {0}; hdr[0]='B'; hdr[1]='M';
    memcpy(hdr+2,&fileSize,4); memcpy(hdr+10,&off,4);
    uint32_t dib = 40; memcpy(hdr+14,&dib,4); int32_t W=w,H=h; memcpy(hdr+18,&W,4); memcpy(hdr+22,&H,4);
    hdr[26]=1; hdr[28]=32; memcpy(hdr+34,&imgSize,4);
    fwrite(hdr,1,54,f);
    std::vector<uint8_t> row(w*4);
    for (int y = h-1; y >= 0; --y) {
        for (int x = 0; x < w; ++x) { const uint8_t* p = rgba+(y*w+x)*4; row[x*4]=p[2]; row[x*4+1]=p[1]; row[x*4+2]=p[0]; row[x*4+3]=p[3]; }
        fwrite(row.data(),1,row.size(),f);
    }
    fclose(f); return true;
}

bool RunSelfTest(const std::string& dir, FILE* log, int frames)
{
    g_log = log;
    SM64Api api = {};
    if (!LoadSM64(dir + "\\sm64.dll", api, log)) return false;
    api.register_debug_print_function(DebugPrint);
    fprintf(log, "sm64.dll loaded OK (all required exports found)\n");

    std::string romPath = dir + "\\baserom.us.z64";
    FILE* rf = fopen(romPath.c_str(), "rb");
    if (!rf) { fprintf(log, "ERROR: cannot open %s\n", romPath.c_str()); return false; }
    std::vector<uint8_t> rom(8 * 1024 * 1024 + 16);
    size_t n = fread(rom.data(), 1, rom.size(), rf); fclose(rf);
    fprintf(log, "ROM read: %lu bytes (expected 8388608), first bytes %02X %02X %02X %02X (expected 80 37 12 40)\n",
            (unsigned long)n, rom[0], rom[1], rom[2], rom[3]);
    if (n != 8388608 || rom[0] != 0x80) { fprintf(log, "ERROR: ROM must be a big-endian .z64 US ROM (8 MB)\n"); return false; }

    std::vector<uint8_t> tex(4 * SM64_TEXTURE_WIDTH * SM64_TEXTURE_HEIGHT);
    api.global_init(rom.data(), tex.data());
    WriteBMP(dir + "\\mario_texture_dump.bmp", tex.data(), SM64_TEXTURE_WIDTH, SM64_TEXTURE_HEIGHT);
    fprintf(log, "global_init done; texture atlas dumped to mario_texture_dump.bmp\n");

    const int32_t S = 1000;
    SM64Surface floor[2] = {};
    int32_t a[3][3] = {{-S,0,S},{S,0,S},{S,0,-S}}, b[3][3] = {{-S,0,S},{S,0,-S},{-S,0,-S}};
    memcpy(floor[0].vertices, a, sizeof a); memcpy(floor[1].vertices, b, sizeof b);
    api.static_surfaces_load(floor, 2);

    int32_t id = api.mario_create(0, 100, 0);
    if (id < 0) { fprintf(log, "ERROR: mario_create failed (%d)\n", id); return false; }
    fprintf(log, "Mario created, id=%d\n", id);

    std::vector<float> pos(9*SM64_GEO_MAX_TRIANGLES), nrm(9*SM64_GEO_MAX_TRIANGLES), col(9*SM64_GEO_MAX_TRIANGLES), uv(6*SM64_GEO_MAX_TRIANGLES);
    SM64MarioGeometryBuffers gb = { pos.data(), nrm.data(), col.data(), uv.data(), 0 };
    SM64MarioState st = {};
    float maxY = -1e9f, landedY = 1e9f, walked = 0, startX = 0, startZ = 0; bool grounded = false; int tris = 0;

    for (int f = 0; f < frames; ++f) {
        SM64MarioInputs in = {}; in.camLookX = 0; in.camLookZ = -1;
        if (f >= 60 && f < 150) in.stickY = 1.0f;          // walk
        if (f >= 150 && f < 165) { in.stickY = 1.0f; in.buttonA = 1; } // jump
        api.mario_tick(id, &in, &st, &gb);
        tris = gb.numTrianglesUsed;
        if (f == 59) { landedY = st.position[1]; startX = st.position[0]; startZ = st.position[2]; grounded = true; }
        if (f > 59) { if (st.position[1] > maxY) maxY = st.position[1]; walked = hypotf(st.position[0]-startX, st.position[2]-startZ); }
        if (f % 15 == 0) fprintf(log, "f=%3d pos=(%.1f, %.1f, %.1f) vel=(%.1f, %.1f, %.1f) action=0x%08X tris=%d\n", f,
            st.position[0], st.position[1], st.position[2], st.velocity[0], st.velocity[1], st.velocity[2], st.action, tris);
        fflush(log);
        Sleep(33);
    }
    api.mario_delete(id);

    bool landed = grounded && landedY < 5.0f, moved = walked > 100.0f, jumped = maxY > landedY + 20.0f, geo = tris > 100;
    fprintf(log, "RESULT landed=%s walked=%s (%.0f units) jumped=%s geometry=%s (%d tris)\n",
        landed?"PASS":"FAIL", moved?"PASS":"FAIL", walked, jumped?"PASS":"FAIL", geo?"PASS":"FAIL", tris);
    bool pass = landed && moved && jumped && geo;
    fprintf(log, pass ? "SELF-TEST PASSED\n" : "SELF-TEST FAILED\n");
    return pass;
}
