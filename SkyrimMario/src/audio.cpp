#include "audio.h"
#include "prof.h"
#include <windows.h>
#include <mmsystem.h>
#include <string.h>

static const int NBUF = 6, FRAMES_PER_TICK_MAX = 2 * 544, BUF_BYTES = FRAMES_PER_TICK_MAX * 4;
static HWAVEOUT g_hwo = nullptr; static WAVEHDR g_hdr[NBUF]; static int16_t* g_buf[NBUF];
static volatile LONG g_active = 0; static SM64Api* g_api = nullptr; static FILE* g_log = nullptr;

static DWORD WINAPI AudioThread(LPVOID) {
    bool was = false;
    for (;;) {
        if (!g_active) { if (was) { waveOutReset(g_hwo); was = false; } Sleep(20); continue; }
        was = true;
        int queued = 0, freeIdx = -1;
        for (int i = 0; i < NBUF; ++i) { if (g_hdr[i].dwFlags & WHDR_INQUEUE) queued += g_hdr[i].dwBufferLength / 4; else if (freeIdx < 0) freeIdx = i; }
        if (freeIdx < 0 || queued >= 3 * 1088) { Sleep(4); continue; }       // keep ~100 ms buffered
        uint32_t n; { ProfScope pa(P_AUDIO); SM64Lock l; n = g_api->audio_tick((uint32_t)queued, 1100, g_buf[freeIdx]); }
        if (!n || n > 544) { Sleep(10); continue; }
        g_hdr[freeIdx].dwBufferLength = n * 2 * 4;                             // two sub-buffers of n stereo frames
        waveOutWrite(g_hwo, &g_hdr[freeIdx], sizeof(WAVEHDR));
    }
    return 0;
}

bool AudioStart(SM64Api* api, FILE* log) {
    g_api = api; g_log = log;
    WAVEFORMATEX wf = {}; wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = 2; wf.nSamplesPerSec = 32000; wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4; wf.nAvgBytesPerSec = 32000 * 4;
    if (waveOutOpen(&g_hwo, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) { fprintf(log, "audio: waveOutOpen failed\n"); fflush(log); return false; }
    for (int i = 0; i < NBUF; ++i) {
        g_buf[i] = (int16_t*)calloc(1, BUF_BYTES);
        memset(&g_hdr[i], 0, sizeof(WAVEHDR)); g_hdr[i].lpData = (LPSTR)g_buf[i]; g_hdr[i].dwBufferLength = BUF_BYTES;
        waveOutPrepareHeader(g_hwo, &g_hdr[i], sizeof(WAVEHDR)); g_hdr[i].dwFlags |= WHDR_DONE;
    }
    CreateThread(nullptr, 0, AudioThread, nullptr, 0, nullptr);
    fprintf(log, "audio: waveOut started (32 kHz stereo)\n"); fflush(log);
    return true;
}
void AudioSetActive(bool on) { InterlockedExchange(&g_active, on ? 1 : 0); }
