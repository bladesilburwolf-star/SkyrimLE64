#pragma once
#include "sm64_api.h"
#include <vector>
// Collision harvesting: a SKSE task (runs on Skyrim's main thread, so the scene graph is
// stable) walks the world scene graph and copies nearby static triangles, converted to
// SM64 coordinates, into a buffer that the render thread loads into sm64.dll.
void CollisionInit(FILE* log);
// Ask for a fresh harvest centred on a Skyrim-space point; origin/scale describe the
// Skyrim->SM64 mapping (sky = origin + S * (mx, -mz, my)).
void CollisionRequest(const float centerSky[3], float radiusSky, const float originSky[3], float S);
// If a finished harvest is waiting, moves it into `out` and returns true.
bool CollisionFetch(std::vector<SM64Surface>& out);
// Debug: the NEXT harvest also logs the triangles closest to the query point (name, parent, normal, distance).
void CollisionDebugNext();
