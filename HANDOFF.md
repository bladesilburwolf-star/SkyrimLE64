# SkyrimMario - handoff notes (state at milestone 5.2)

Skyrim LE 1.9.32.0 + SKSE 1.7.3 plugin. Mario's physics/animation come from a 32-bit libsm64 `sm64.dll`
(built from the user's libsm64 fork; needs `baserom.us.z64`), both in `<Skyrim>\mario64\`.
Build: `make` in an MSYS2 **MinGW 32-bit** shell (or `make CXX=i686-w64-mingw32-g++` on Linux). Output in `bin/`.
The plugin is header-free: SKSE structs/game addresses are declared by hand (taken from the SKSE 1.7.3 source).

## Files (src/)
- `plugin.cpp`     SKSEPlugin_Query/Load. SKSE 1.7.3 reports runtime version **0x09200000** for 1.9.32 (not 0x01090200).
                   Saves the SKSE task interface (`QueryInterface(4)`), starts a worker thread -> `OverlayRun`.
- `overlay.cpp`    Loads sm64, hooks D3D9 `Present`(17)/`EndScene`(42) via the device vtable, 30 Hz Mario tick,
                   projects Mario's triangles with the game's NiCamera (`WorldPtToScreenPt3_Internal` @0x00AB84C0),
                   draws pre-transformed (XYZRHW) triangles, painter-sorted, 2 passes (vertex colour, then atlas texture).
                   Keys: F9 spawn/remove, F8 possess, F10 flip Y. Interpolates between ticks.
- `collision.cpp`  SKSE *task* (runs on the game's main thread) walks the WorldRoot scene graph and converts nearby
                   NiTriShape triangles to `SM64Surface`s (Skyrim->SM64: m=((x-ox)/S,(z-oz)/S,-(y-oy)/S), S=0.8).
                   Filters sky/fog/cloud domes (`_CD`, Atmosphere...), water, BSLODTriShape, skinned meshes, hidden nodes,
                   common non-solid foliage names (Flower/Grass/Plant/etc.), triangles with an edge > 2500 units.
                   Winding is fixed up using the mesh's own vertex normals. Steep faces use the default material;
                   walkable ground uses NOT_SLIPPERY.
- `possess.cpp`    Task: hides every NiGeometry in the player skeleton (flag bit0 at +0x98) and calls
                   `MoveRefrToPosition` (@0x90C9F0) every frame to put the player actor on Mario so the stock 3rd-person
                   camera follows.
- `inputblock.cpp` Hooks GetDeviceState/GetDeviceData on the real dinput8.dll keyboard vtable (works under SKSE's wrapper) and
                   drops presses of W A S D Space LCtrl F while possessing.
- `xinput.cpp`     Reads pad 0 directly; hooks XInputGetState (IAT patch, hot-patch fallback) to zero Mario's controls for the game.
- `audio.cpp`      waveOut 32 kHz stereo thread pumping `sm64_audio_tick` (2 x 544/528 frames per call). Shares `SM64Lock` with physics.
- `runner.cpp`, `test_main.cpp`  loader helpers + standalone `sm64_test.exe` self-test.

## Important gotchas
- Game objects are MSVC C++: virtual calls into the game need `__attribute__((thiscall))`; task objects handed to SKSE
  (`TaskDelegate`: Run, Dispose) must be thiscall too. Tasks run inside the game's task pump - never queue from inside Run.
- NiObject vtable: slot 2 = GetRTTI (used for class-name checks). NiNode children: array data ptr @+0xAC, count @+0xB2.
  NiAVObject flags @+0x98 (bit0 = hidden), world transform @+0x54 (rot3x3, pos @+0x78, scale @+0x84).
- Addresses (1.9.32): player ptr 0x01B2E8E4 (pos @+0x34, rot @+0x28, loaded 3D node = *(*(p+0x44)+0x20)),
  PlayerCamera ptr 0x012E7288 (cameraNode @+0x1C), NiDX9Renderer ptr 0x01BA76FC (D3D device @+0x38).
- SM64 input: stickY = -1 is "forward"; camLookX/Z = Mario - camera (horizontal).

## Known issues / next steps
1. **Camera shake in possess mode.** Teleporting the player every frame with MoveRefrToPosition fights Havok / the
   camera's own smoothing. Log has `possess: drift` lines. Better options: hook `PlayerCamera` vtable Update (slot 2 of
   TESCamera: SetNode, Update) and set the camera position/rotation ourselves (orbit around Mario, keep mouse look), leaving
   the player actor alone; or move the player's Havok character controller instead of calling MoveTo.
2. (Addressed in 4.2 via MenuManager::IsMenuOpen; HUD still drawn over.) **Mario draws over menus / HUD.** The overlay draws at every back-buffer EndScene so the last one (over the UI) wins.
   Skip drawing while any menu is open (SKSE `MenuManager::IsMenuOpen`, see skse/GameMenus.h) or while the game is paused,
   and/or find the scene that draws the HUD and draw just before it.
3. **Not in the 3D scene**: no occlusion, no world lighting, no shadows. Proper fix = create/clone an NiTriShape in the scene
   graph (skse/NiGeometry.h has `NiTriShape::Create`) and refresh its vertex data every frame.
4. Possession still lets the game receive WASD/Space (teleport overrides it). Could block via the DirectInput wrapper.
5. Collision is re-harvested after 500 units of movement or 8 s (radius 1000); SM64 collision is limited to about +-8192 SM64 units
   from the spawn origin - re-origin when Mario travels far.
6. Mario's own body parts: no animation hooks into Skyrim (sounds/music/audio not wired up; libsm64 audio API is unused).

## Do NOT
- Use made-up addresses. Menu manager is 0x012E3548 (not 0x01B2E860); BSFixedString ctor 0x00A511C0.
- Only teleport the player when far from Mario: it makes the camera lag. Move every frame, or hook the camera instead.

## Performance (5.1)
- User video (possess mode): ~10 fps, stalls to 0.5 s. `prof.h` writes a 'perf' block to the log every 5 s. Suspects: per-frame
  `MoveRefrToPosition` (possess.cpp), D3D state block per back-buffer scene (overlay.cpp), collision harvest + sm64 surface reload.
- If MoveRefr dominates: stop moving the player every frame. Hook the camera update instead (TESCamera vtable slot 2 `Update`,
  PlayerCamera singleton 0x012E7288, cameraNode @+0x1C, camera states array @+0x6C) and offset the camera by (Mario - player),
  leashing the real actor (teleport only when far) so cells/AI still follow.

## 5.2 / 5.3 notes
- Harvest radius 1000, vertical band -450/+550 around Mario. Common foliage names are excluded after a stuck report
  identified `MountainFlower02Red` collision at Mario's position. Harvest logs report `foliage=` rejected counts.
- Full harvest interval is 500 units of movement or 8 seconds. Each harvest still walks the scene graph on the main thread;
  the supplied log showed roughly 11-18 ms per harvest, but sustained render-side averages were under 1 ms per scene.
- F7 / stuck detector -> `CollisionDebugNext()` makes the next harvest log the nearest triangles ('near:' lines).
- Potential remaining collision offenders: foliage with generic names, tiny clutter, meshes without normals (winding guess).
