#pragma once
// Tiny profiler: accumulates time per subsystem and prints a summary to SkyrimMario.log every 5 s.
#include <windows.h>
#include <stdio.h>
enum ProfId { P_FRAME, P_TICK, P_BUILD, P_DRAW, P_COLLUPD, P_SURFLOAD, P_POSSESS, P_MOVEREFR, P_HIDE, P_HARVEST, P_AUDIO, P_SCENES, P_COUNT };
static const char* const kProfNames[P_COUNT] = { "overlayFrame(total)", "marioTick", "buildGeom", "d3dDraw", "collisionUpdate", "surfaceLoad", "possessTask(total)", "  moveRefr", "  hideBody", "harvestTask", "audioTick", "backbufferScenes/frame(count)" };
struct ProfAcc { double sum = 0, mx = 0; long n = 0; };
inline ProfAcc g_profAcc[P_COUNT]; inline CRITICAL_SECTION g_profCs; inline bool g_profInit = false; inline double g_profLast = 0; inline int g_profDumps = 0;
inline void ProfInit() { if (!g_profInit) { InitializeCriticalSection(&g_profCs); g_profInit = true; } }
inline double ProfNow() { static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }(); LARGE_INTEGER c; QueryPerformanceCounter(&c); return c.QuadPart * 1000.0 / f.QuadPart; }
inline void ProfAdd(int id, double v) { if (!g_profInit) return; EnterCriticalSection(&g_profCs); ProfAcc& a = g_profAcc[id]; a.sum += v; if (v > a.mx) a.mx = v; ++a.n; LeaveCriticalSection(&g_profCs); }
struct ProfScope { int id; double t0; ProfScope(int i) : id(i), t0(ProfNow()) {} ~ProfScope() { ProfAdd(id, ProfNow() - t0); } };
inline void ProfDumpIfDue(FILE* log) {
    if (!g_profInit || !log || g_profDumps >= 24) return;
    double now = ProfNow(); if (g_profLast == 0) { g_profLast = now; return; } if (now - g_profLast < 5000.0) return;
    EnterCriticalSection(&g_profCs); g_profLast = now; ++g_profDumps;
    fprintf(log, "perf (last 5 s)  [avg ms / max ms / calls]\n");
    for (int i = 0; i < P_COUNT; ++i) { ProfAcc& a = g_profAcc[i]; if (a.n) fprintf(log, "  %-30s %7.3f / %7.3f / %ld\n", kProfNames[i], a.sum / a.n, a.mx, a.n); a = ProfAcc(); }
    fflush(log); LeaveCriticalSection(&g_profCs);
}
