#include "collision.h"
#include "prof.h"
#include <windows.h>
#include <math.h>
#include <string.h>
#include <string>

extern void* g_skseTaskInterface;   // SKSETaskInterface* (set by plugin.cpp)
struct SKSETaskInterfaceLite { uint32_t interfaceVersion; void (*AddTask)(void* task); void (*AddUITask)(void* task); };

static const uintptr_t ADDR_PLAYER_PTR = 0x01B2E8E4;
static FILE* g_log = nullptr;
#define LOG(...) do { if (g_log) { fprintf(g_log, __VA_ARGS__); fputc('\n', g_log); fflush(g_log); } } while (0)

static CRITICAL_SECTION g_cs;
static bool g_csInit = false;
static volatile LONG g_taskPending = 0;
static std::vector<SM64Surface> g_ready;
static bool g_haveReady = false;
static int g_harvestsLogged = 0;
static int g_colLogged = 0;   // lines of 'triangles under Mario' diagnostics written this harvest
static int g_colHarvests = 0;
static volatile LONG g_debugNext = 0;      // set by CollisionDebugNext()
static bool g_dbgThisRun = false; static int g_dbgLines = 0;
static const float V_BELOW = 450.0f, V_ABOVE = 550.0f;   // vertical band around Mario that matters for his collision

// request parameters (written by render thread, read by main thread under lock)
static float g_qc[3], g_qr, g_qo[3], g_qS;

// ---- safe memory helpers ----
static bool Ok(const void* p, size_t n) { return p && !IsBadReadPtr(p, n); }
template <class T> static T Rd(uintptr_t a) { return *(T*)a; }

typedef void* (__attribute__((thiscall)) *GetRTTI_t)(void*);
static bool IsA(uintptr_t obj, const char* name) {
    void** vt = (void**)Rd<uintptr_t>(obj); if (!Ok(vt, 12)) return false;
    void* rtti = ((GetRTTI_t)vt[2])((void*)obj);            // NiObject::GetRTTI (vtbl slot 2)
    for (int i = 0; i < 16 && rtti && Ok(rtti, 8); ++i) {
        const char* nm = *(const char**)rtti; if (Ok(nm, 4) && strcmp(nm, name) == 0) return true;
        rtti = *((void**)rtti + 1);
    }
    return false;
}

static const char* ClassName(uintptr_t obj) {
    void** vt = (void**)Rd<uintptr_t>(obj); if (!Ok(vt, 12)) return "?";
    void* rtti = ((GetRTTI_t)vt[2])((void*)obj); if (!rtti || !Ok(rtti, 8)) return "?";
    const char* nm = *(const char**)rtti; return Ok(nm, 4) ? nm : "?";
}
static std::string NodeName(uintptr_t obj) {
    uintptr_t p = Rd<uintptr_t>(obj + 0x08); if (!p || !Ok((void*)p, 16)) return "";
    const char* c = (const char*)p; std::string r;
    for (int i = 0; i < 40 && Ok(c + i, 1) && c[i] >= 32 && c[i] < 127; ++i) r += c[i];
    return r;
}
static void DumpTree(uintptr_t root, uintptr_t pnode) {
    LOG("scene dump: player node %p (%s '%s')", (void*)pnode, ClassName(pnode), NodeName(pnode).c_str());
    uintptr_t o = pnode; int d = 0;
    while (o && d < 12 && Ok((void*)o, 0xB8)) { LOG("  ancestor[%d] %p %s '%s'", d, (void*)o, ClassName(o), NodeName(o).c_str()); o = Rd<uintptr_t>(o + 0x18); ++d; }
    LOG("  root %p %s '%s'", (void*)root, ClassName(root), NodeName(root).c_str());
    if (IsA(root, "NiNode")) {
        uintptr_t data = Rd<uintptr_t>(root + 0xAC); uint16_t cnt = Rd<uint16_t>(root + 0xB2);
        LOG("  root children: %u", cnt);
        for (int i = 0; i < cnt && i < 24 && data && Ok((void*)data, (i+1)*4); ++i) { uintptr_t ch = ((uintptr_t*)data)[i];
            if (ch && Ok((void*)ch, 0xB8)) LOG("    child[%d] %s '%s' flags=%08X", i, ClassName(ch), NodeName(ch).c_str(), Rd<uint32_t>(ch + 0x98)); }
    }
}

static std::string NodeName(uintptr_t obj);
struct Stats { int nodes, shapes, noData, skinned, noNormals, hidden, tris, culled, rejLOD, rejSky, rejWater, rejFoliage, bigTris; };


static bool Has(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }
static bool HasNoCase(const std::string& s, const char* sub) {
    const size_t n = strlen(sub);
    if (n == 0 || s.size() < n) return false;
    for (size_t i = 0; i + n <= s.size(); ++i) {
        size_t j = 0;
        for (; j < n; ++j) {
            char a = s[i + j], b = sub[j];
            if (a >= 'A' && a <= 'Z') a = char(a + ('a' - 'A'));
            if (b >= 'A' && b <= 'Z') b = char(b + ('a' - 'A'));
            if (a != b) break;
        }
        if (j == n) return true;
    }
    return false;
}
static bool IsNonSolidFoliage(const std::string& n, const std::string& pn) {
    static const char* const tokens[] = {
        "flower", "grass", "foliage", "flora", "shrub", "bush", "fern", "plant", "weed",
        "leaves", "leaf", "canopy", "mushroom"
    };
    for (const char* t : tokens) if (HasNoCase(n, t) || HasNoCase(pn, t)) return true;
    return false;
}
// Skyrim's scene graph also holds the sky, clouds, fog layers, water planes and distant-LOD stand-ins.
// None of those are ground, so they must not become SM64 collision.
static bool RejectShape(uintptr_t shp, Stats& st) {
    if (strcmp(ClassName(shp), "BSLODTriShape") == 0) { ++st.rejLOD; return true; }
    std::string n = NodeName(shp), pn = NodeName(Rd<uintptr_t>(shp + 0x18));
    // Alpha-card foliage is rendered as broad triangles but should not block Mario.
    // The latest stuck report was MountainFlower02Red:0; skipping both shape and
    // parent names also catches the other common Skyrim flora meshes.
    if (IsNonSolidFoliage(n, pn)) { ++st.rejFoliage; return true; }
    if (Has(n, "_CD") || Has(n, "Atmosphere") || Has(n, "Cloud") || Has(n, "Stars") || Has(n, "Moon") || Has(n, "SunGlare") || Has(n, "SunBase") || Has(n, "Aurora") || Has(pn, "Sky")) { ++st.rejSky; return true; }
    if (Has(n, "Water") || pn == "Water" || Has(pn, "Water")) { ++st.rejWater; return true; }
    return false;
}
static const float MAX_EDGE = 2500.0f;   // sky domes / fog / LOD quads have enormous triangles; real geometry doesn't

static void ProcessShape(uintptr_t shp, std::vector<SM64Surface>& out, Stats& st) {
    if (Rd<uintptr_t>(shp + 0xB4)) { ++st.skinned; return; }          // skinned (actors): bind pose, skip
    if (RejectShape(shp, st)) return;
    uintptr_t d = Rd<uintptr_t>(shp + 0xB0); if (!Ok((void*)d, 0x54)) { ++st.noData; return; }
    uint16_t nv = Rd<uint16_t>(d + 0x08), nt = Rd<uint16_t>(d + 0x48);
    const float* V = (const float*)Rd<uintptr_t>(d + 0x20); const float* N = (const float*)Rd<uintptr_t>(d + 0x24);
    const uint16_t* T = (const uint16_t*)Rd<uintptr_t>(d + 0x50);
    if (!nv || !nt || !V || !T || !Ok(V, nv * 12) || !Ok(T, nt * 6)) { ++st.noData; return; }
    bool hasN = N && Ok(N, nv * 12); if (!hasN) ++st.noNormals;
    ++st.shapes;
    const float* R = (const float*)(shp + 0x54); const float* P = (const float*)(shp + 0x78); float sc = Rd<float>(shp + 0x84);
    // shape-level cull with the geometry's local bound
    const float* B = (const float*)(d + 0x10);
    float bc[3] = { sc * (R[0]*B[0] + R[1]*B[1] + R[2]*B[2]) + P[0], sc * (R[3]*B[0] + R[4]*B[1] + R[5]*B[2]) + P[1], sc * (R[6]*B[0] + R[7]*B[1] + R[8]*B[2]) + P[2] };
    float br = B[3] * sc;
    if (br > 0 && (fabsf(bc[0]-g_qc[0]) > g_qr + br || fabsf(bc[1]-g_qc[1]) > g_qr + br || fabsf(bc[2]-g_qc[2]) > g_qr + br)) { ++st.culled; return; }

    // transform vertices to Skyrim world space
    static std::vector<float> W; W.resize(nv * 3);
    for (int i = 0; i < nv; ++i) {
        const float* v = V + i*3;
        W[i*3+0] = sc * (R[0]*v[0] + R[1]*v[1] + R[2]*v[2]) + P[0];
        W[i*3+1] = sc * (R[3]*v[0] + R[4]*v[1] + R[5]*v[2]) + P[1];
        W[i*3+2] = sc * (R[6]*v[0] + R[7]*v[1] + R[8]*v[2]) + P[2];
    }
    for (int t = 0; t < nt; ++t) {
        int i0 = T[t*3], i1 = T[t*3+1], i2 = T[t*3+2]; if (i0 >= nv || i1 >= nv || i2 >= nv) continue;
        const float *a = &W[i0*3], *b = &W[i1*3], *c = &W[i2*3];
        float mnx = fminf(a[0], fminf(b[0], c[0])), mxx = fmaxf(a[0], fmaxf(b[0], c[0]));
        float mny = fminf(a[1], fminf(b[1], c[1])), mxy = fmaxf(a[1], fmaxf(b[1], c[1]));
        float mnz = fminf(a[2], fminf(b[2], c[2])), mxz = fmaxf(a[2], fmaxf(b[2], c[2]));
        if (mxx < g_qc[0]-g_qr || mnx > g_qc[0]+g_qr || mxy < g_qc[1]-g_qr || mny > g_qc[1]+g_qr || mxz < g_qc[2]-V_BELOW || mnz > g_qc[2]+V_ABOVE) continue;
        float u[3] = { b[0]-a[0], b[1]-a[1], b[2]-a[2] }, w[3] = { c[0]-a[0], c[1]-a[1], c[2]-a[2] };
        float n[3] = { u[1]*w[2]-u[2]*w[1], u[2]*w[0]-u[0]*w[2], u[0]*w[1]-u[1]*w[0] };
        if (n[0]*n[0] + n[1]*n[1] + n[2]*n[2] < 1e-3f) continue;      // degenerate
        { float e0 = u[0]*u[0]+u[1]*u[1]+u[2]*u[2], e1 = w[0]*w[0]+w[1]*w[1]+w[2]*w[2], dx = c[0]-b[0], dy = c[1]-b[1], dz = c[2]-b[2], e2 = dx*dx+dy*dy+dz*dz;
          if (fmaxf(e0, fmaxf(e1, e2)) > MAX_EDGE * MAX_EDGE) { ++st.bigTris; continue; } }
        if (hasN) {                                                   // make winding agree with the mesh's own normals
            const float* n0 = N + i0*3; const float* n1 = N + i1*3; const float* n2 = N + i2*3;
            float sx = n0[0]+n1[0]+n2[0], sy = n0[1]+n1[1]+n2[1], sz = n0[2]+n1[2]+n2[2];
            float wx = sc * (R[0]*sx + R[1]*sy + R[2]*sz), wy = sc * (R[3]*sx + R[4]*sy + R[5]*sz), wz = sc * (R[6]*sx + R[7]*sy + R[8]*sz);
            if (n[0]*wx + n[1]*wy + n[2]*wz < 0) { std::swap(i1, i2); b = &W[i1*3]; c = &W[i2*3]; }
        }
        if (g_colHarvests <= 3 && g_colLogged < 24) {   // diagnostics: which triangles lie in the vertical column through the query point?
            float qx = g_qc[0], qy = g_qc[1];
            float d0 = (b[0]-a[0])*(qy-a[1]) - (b[1]-a[1])*(qx-a[0]);
            float d1 = (c[0]-b[0])*(qy-b[1]) - (c[1]-b[1])*(qx-b[0]);
            float d2 = (a[0]-c[0])*(qy-c[1]) - (a[1]-c[1])*(qx-c[0]);
            bool inside = (d0 >= 0 && d1 >= 0 && d2 >= 0) || (d0 <= 0 && d1 <= 0 && d2 <= 0);
            if (inside) {
                float nn[3] = { (b[1]-a[1])*(c[2]-a[2]) - (b[2]-a[2])*(c[1]-a[1]), (b[2]-a[2])*(c[0]-a[0]) - (b[0]-a[0])*(c[2]-a[2]), (b[0]-a[0])*(c[1]-a[1]) - (b[1]-a[1])*(c[0]-a[0]) };
                float len = sqrtf(nn[0]*nn[0] + nn[1]*nn[1] + nn[2]*nn[2]);
                if (fabsf(nn[2]) > 1e-4f && len > 0) {
                    float z = a[2] - (nn[0]*(qx-a[0]) + nn[1]*(qy-a[1])) / nn[2];
                    LOG("  column: z=%+.0f (feet-relative %+.0f) normalZ=%+.2f hasNormals=%d shape='%s' parent='%s' class=%s",
                        z, z - g_qo[2], nn[2]/len, (int)hasN, NodeName(shp).c_str(), NodeName(Rd<uintptr_t>(shp + 0x18)).c_str(), ClassName(shp));
                    ++g_colLogged;
                }
            }
        }
        if (g_dbgThisRun && g_dbgLines < 60 && mxx > g_qc[0]-90 && mnx < g_qc[0]+90 && mxy > g_qc[1]-90 && mny < g_qc[1]+90 && mxz > g_qc[2]-30 && mnz < g_qc[2]+200) {
            float nl = sqrtf(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
            float cx = (a[0]+b[0]+c[0])/3 - g_qc[0], cy = (a[1]+b[1]+c[1])/3 - g_qc[1], cz = (a[2]+b[2]+c[2])/3 - g_qc[2];
            LOG("  near: shape='%s' class=%s parent='%s' dist=%.0f normalZ=%+.2f hasNormals=%d triZ(feet-rel)=%+.0f", NodeName(shp).c_str(), ClassName(shp),
                NodeName(Rd<uintptr_t>(shp + 0x18)).c_str(), sqrtf(cx*cx + cy*cy + cz*cz), n[2]/nl, (int)hasN, (a[2]+b[2]+c[2])/3 - g_qo[2]);
            ++g_dbgLines;
        }
        SM64Surface s = {};
        // Keep the custom no-slip material on walkable ground, but let vertical
        // faces behave like walls instead of giving Mario climbable foliage/props.
        float normalLen = sqrtf(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
        float up = normalLen > 0 ? fabsf(n[2]) / normalLen : 0;
        s.type = up >= 0.65f ? 0x0015 : 0x0000;
        s.force = 0; s.terrain = 0;
        const float* pts[3] = { a, b, c };
        for (int k = 0; k < 3; ++k) {   // Skyrim -> SM64:  m = ((x-ox)/S, (z-oz)/S, -(y-oy)/S)
            s.vertices[k][0] = (int32_t)lrintf((pts[k][0] - g_qo[0]) / g_qS);
            s.vertices[k][1] = (int32_t)lrintf((pts[k][2] - g_qo[2]) / g_qS);
            s.vertices[k][2] = (int32_t)lrintf(-(pts[k][1] - g_qo[1]) / g_qS);
        }
        out.push_back(s); ++st.tris;
        if (out.size() >= 80000) return;
    }
}

static void Harvest(std::vector<SM64Surface>& out, Stats& st) {
    uintptr_t player = Ok((void*)ADDR_PLAYER_PTR, 4) ? Rd<uintptr_t>(ADDR_PLAYER_PTR) : 0; if (!player) return;
    uintptr_t ls = Rd<uintptr_t>(player + 0x44); if (!Ok((void*)ls, 0x24)) return;
    uintptr_t pnode = Rd<uintptr_t>(ls + 0x20); if (!Ok((void*)pnode, 0xB8)) return;
    uintptr_t root = pnode; for (int i = 0; i < 64; ++i) { uintptr_t p = Rd<uintptr_t>(root + 0x18); if (!p || !Ok((void*)p, 0xB8)) break; root = p; }
    static bool dumped = false; if (!dumped) { dumped = true; DumpTree(root, pnode); }
    struct E { uintptr_t o; int depth; int br; };
    struct Br { std::string name, cls; int tris; };
    static std::vector<E> stack; stack.clear(); stack.push_back({ root, 0, -1 });
    std::vector<Br> brs;
    int guard = 0;
    while (!stack.empty() && ++guard < 200000 && out.size() < 80000) {
        E e = stack.back(); stack.pop_back(); uintptr_t o = e.o;
        if (!o || o == pnode || !Ok((void*)o, 0xB8)) continue;
        if (Rd<uint32_t>(o + 0x98) & 1) { ++st.hidden; continue; }                 // app-culled / hidden
        if (e.depth == 2) { e.br = (int)brs.size(); brs.push_back({ NodeName(o), ClassName(o), 0 }); }   // child of ShadowSceneNode
        if (IsA(o, "NiNode")) {
            ++st.nodes;
            uintptr_t data = Rd<uintptr_t>(o + 0xAC); uint16_t cnt = Rd<uint16_t>(o + 0xB2);
            if (!data || cnt > 8192 || !Ok((void*)data, cnt * 4)) continue;
            for (int i = 0; i < cnt; ++i) { uintptr_t ch = ((uintptr_t*)data)[i]; if (ch) stack.push_back({ ch, e.depth + 1, e.br }); }
        } else if (IsA(o, "NiTriShape")) {
            size_t before = out.size(); ProcessShape(o, out, st);
            if (e.br >= 0) brs[e.br].tris += (int)(out.size() - before);
        }
    }
    if (g_colHarvests <= 2) for (auto& b : brs) LOG("  branch '%s' (%s): %d tris", b.name.c_str(), b.cls.c_str(), b.tris);
}

class HarvestTask {
public:
    virtual void __attribute__((thiscall)) Run() {
        ProfScope psAll(P_HARVEST);
        std::vector<SM64Surface> tmp; Stats st = {}; DWORD t0 = GetTickCount();
        EnterCriticalSection(&g_cs); float qc[3] = { g_qc[0], g_qc[1], g_qc[2] }; LeaveCriticalSection(&g_cs); (void)qc;
        g_dbgThisRun = InterlockedExchange(&g_debugNext, 0) != 0; g_dbgLines = 0;
        if (g_dbgThisRun) LOG("debug harvest at Skyrim (%.0f, %.0f, %.0f), feet z=%.0f:", g_qc[0], g_qc[1], g_qc[2], g_qo[2]);
        g_colLogged = 0; ++g_colHarvests;
        if (g_colHarvests <= 3) LOG("column dump for harvest %d at Skyrim (%.0f, %.0f) feet z=%.0f:", g_colHarvests, g_qc[0], g_qc[1], g_qo[2]);
        Harvest(tmp, st);
        EnterCriticalSection(&g_cs); g_ready.swap(tmp); g_haveReady = true; LeaveCriticalSection(&g_cs);
        if (g_harvestsLogged < 8) { ++g_harvestsLogged;
            LOG("harvest: %d tris | nodes=%d shapes=%d (noData=%d skinned=%d noNormals=%d) hidden=%d culled=%d rejected: LOD=%d sky=%d water=%d foliage=%d bigTris=%d | %lu ms",
                st.tris, st.nodes, st.shapes, st.noData, st.skinned, st.noNormals, st.hidden, st.culled, st.rejLOD, st.rejSky, st.rejWater, st.rejFoliage, st.bigTris, GetTickCount() - t0); }
        InterlockedExchange(&g_taskPending, 0);
    }
    virtual void __attribute__((thiscall)) Dispose() { delete this; }
};

void CollisionDebugNext() { InterlockedExchange(&g_debugNext, 1); }

void CollisionInit(FILE* log) { g_log = log; if (!g_csInit) { InitializeCriticalSection(&g_cs); g_csInit = true; } }

void CollisionRequest(const float c[3], float r, const float o[3], float S) {
    if (!g_skseTaskInterface || InterlockedCompareExchange(&g_taskPending, 1, 0) != 0) return;
    EnterCriticalSection(&g_cs);
    memcpy(g_qc, c, 12); g_qr = r; memcpy(g_qo, o, 12); g_qS = S;
    LeaveCriticalSection(&g_cs);
    ((SKSETaskInterfaceLite*)g_skseTaskInterface)->AddTask(new HarvestTask());
}

bool CollisionFetch(std::vector<SM64Surface>& out) {
    EnterCriticalSection(&g_cs); bool have = g_haveReady; if (have) { out.swap(g_ready); g_ready.clear(); g_haveReady = false; } LeaveCriticalSection(&g_cs);
    return have;
}
