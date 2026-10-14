#pragma once
#include "sm64_api.h"
// Plays Mario's real N64 sound engine (sm64_audio_tick, 32 kHz stereo) through waveOut on its own thread.
bool AudioStart(SM64Api* api, FILE* log);
void AudioSetActive(bool on);   // false = silence (and flush queued audio)
