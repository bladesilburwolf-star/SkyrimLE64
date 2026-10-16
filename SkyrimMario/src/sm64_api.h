// Minimal libsm64 API (matches libsm64 src/libsm64.h), loaded dynamically.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string>

struct SM64Surface { int16_t type; int16_t force; uint16_t terrain; int32_t vertices[3][3]; };
struct SM64MarioInputs { float camLookX, camLookZ; float stickX, stickY; uint8_t buttonA, buttonB, buttonZ; };
struct SM64MarioState {
    float position[3]; float velocity[3]; float faceAngle; float forwardVelocity;
    int16_t health; uint32_t action; int32_t animID; int16_t animFrame;
    uint32_t flags; uint32_t particleFlags; int16_t invincTimer;
};
struct SM64MarioGeometryBuffers { float *position; float *normal; float *color; float *uv; uint16_t numTrianglesUsed; };

enum { SM64_TEXTURE_WIDTH = 64 * 11, SM64_TEXTURE_HEIGHT = 64, SM64_GEO_MAX_TRIANGLES = 1024 };

struct SM64Api {
    void    (*register_debug_print_function)(void (*)(const char*));
    void    (*global_init)(const uint8_t* rom, uint8_t* outTexture);
    void    (*global_terminate)(void);
    void    (*static_surfaces_load)(const SM64Surface*, uint32_t);
    int32_t (*mario_create)(float x, float y, float z);
    void    (*mario_tick)(int32_t id, const SM64MarioInputs*, SM64MarioState*, SM64MarioGeometryBuffers*);
    void    (*mario_delete)(int32_t id);
    float   (*surface_find_floor_height)(float x, float y, float z);
    void    (*audio_init)(const uint8_t* rom);
    uint32_t (*audio_tick)(uint32_t numQueued, uint32_t numDesired, int16_t* buffer);
    void    (*set_sound_volume)(float vol);
    void    (*set_mario_position)(int32_t marioId, float x, float y, float z);
};

bool LoadSM64(const std::string& dllPath, SM64Api& api, FILE* log);
// Loads sm64.dll + baserom.us.z64 from `dir`, runs a scripted headless Mario
// (fall, walk, jump) for `frames` ticks at 30 Hz and logs the result.
bool RunSelfTest(const std::string& dir, FILE* log, int frames);

// Milestone 2: installs the Direct3D 9 overlay (F9 spawns Mario). Returns once hooks are in.
bool OverlayRun(const std::string& dir, FILE* log);

// sm64.dll keeps global state: physics (render thread) and audio (audio thread) must not run at the same time.
void SM64LockEnter(); void SM64LockLeave();
struct SM64Lock { SM64Lock() { SM64LockEnter(); } ~SM64Lock() { SM64LockLeave(); } };
