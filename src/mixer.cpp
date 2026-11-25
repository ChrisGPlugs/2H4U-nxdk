#include "mixer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#define DR_MP3_NO_SIMD      // its SIMD detection misfires for the Xbox target
#include "../third_party/dr_mp3.h"

#ifndef NXDK
extern const char* Host_Path(const char* xboxPath);
#define MIXER_PATH(p) Host_Path(p)
#else
#define MIXER_PATH(p) (p)
#endif

struct Mix_Music
{
    Uint8*          fileData;
    size_t          fileSize;
    drmp3           mp3;
    SDL_AudioStream* stream;   // MP3 rate/channels -> device format
};

namespace {

struct Channel
{
    Mix_Chunk* chunk;
    Uint32     pos;      // byte offset
    int        loops;    // -1 = forever
};

SDL_AudioDeviceID g_device = 0;     // nonzero while audio is open
SDL_AudioSpec     g_spec;
Channel           g_channels[MIX_CHANNELS];

Mix_Music*        g_music = NULL;
int               g_musicLoops = 0;
bool              g_musicPaused = false;

Sint32*           g_accum = NULL;
Sint16*           g_musicBuf = NULL;
int               g_accumSamples = 0;

const int MP3_DECODE_FRAMES = 1152;
Sint16            g_decodeBuf[MP3_DECODE_FRAMES * 2];

bool readFile(const char* path, Uint8** data, size_t* size)
{
    SDL_RWops* rw = SDL_RWFromFile(path, "rb");
    if (!rw) return false;
    Sint64 len = SDL_RWsize(rw);
    if (len <= 0) { SDL_RWclose(rw); return false; }
    *data = (Uint8*)malloc((size_t)len);
    *size = (size_t)SDL_RWread(rw, *data, 1, (size_t)len);
    SDL_RWclose(rw);
    return *size == (size_t)len;
}

// Pull `samples` S16 samples (interleaved) of music into out. Called with the device locked.
void readMusic(Sint16* out, int samples)
{
    Mix_Music* m = g_music;
    int bytesWanted = samples * (int)sizeof(Sint16);

    while (SDL_AudioStreamAvailable(m->stream) < bytesWanted)
    {
        drmp3_uint64 got = drmp3_read_pcm_frames_s16(&m->mp3, MP3_DECODE_FRAMES, g_decodeBuf);
        if (got == 0)
        {
            if (g_musicLoops == 0) { SDL_AudioStreamFlush(m->stream); break; }
            if (g_musicLoops > 0) g_musicLoops--;
            drmp3_seek_to_pcm_frame(&m->mp3, 0);
            continue;
        }
        SDL_AudioStreamPut(m->stream, g_decodeBuf, (int)(got * m->mp3.channels * sizeof(Sint16)));
    }

    int got = SDL_AudioStreamGet(m->stream, out, bytesWanted);
    if (got < 0) got = 0;
    if (got < bytesWanted)
    {
        memset((Uint8*)out + got, 0, bytesWanted - got);
        if (g_musicLoops == 0 && SDL_AudioStreamAvailable(m->stream) == 0)
            g_music = NULL;                 // finished
    }
}

volatile unsigned g_callbacks = 0;

void SDLCALL audioCallback(void*, Uint8* stream, int len)
{
    g_callbacks++;
    const int samples = len / (int)sizeof(Sint16);
    if (samples > g_accumSamples)
    {
        g_accum = (Sint32*)realloc(g_accum, samples * sizeof(Sint32));
        g_musicBuf = (Sint16*)realloc(g_musicBuf, samples * sizeof(Sint16));
        g_accumSamples = samples;
    }
    memset(g_accum, 0, samples * sizeof(Sint32));

    if (g_music && !g_musicPaused)
    {
        readMusic(g_musicBuf, samples);
        for (int i = 0; i < samples; i++) g_accum[i] += g_musicBuf[i];
    }

    for (int c = 0; c < MIX_CHANNELS; c++)
    {
        Channel& ch = g_channels[c];
        if (!ch.chunk) continue;
        const int vol = ch.chunk->volume;
        int i = 0;
        while (i < samples && ch.chunk)
        {
            const Sint16* src = (const Sint16*)(ch.chunk->abuf + ch.pos);
            int avail = (int)((ch.chunk->alen - ch.pos) / sizeof(Sint16));
            int n = samples - i < avail ? samples - i : avail;
            for (int k = 0; k < n; k++) g_accum[i + k] += (src[k] * vol) >> 7;
            i += n;
            ch.pos += n * sizeof(Sint16);
            if (ch.pos >= ch.chunk->alen)
            {
                if (ch.loops == 0) ch.chunk = NULL;
                else { if (ch.loops > 0) ch.loops--; ch.pos = 0; }
            }
        }
    }

    Sint16* out = (Sint16*)stream;
    for (int i = 0; i < samples; i++)
    {
        Sint32 v = g_accum[i];
        out[i] = (Sint16)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
    }
}

#if defined(NXDK) && defined(AUDIO_POLL)
// ---------------------------------------------------------------------------
// Polled AC97 output (make AUDIO_POLL=1).
//
// nxdk's SDL audio driver only sends more sound when the AC97 "buffer done"
// interrupt fires. xemu never delivers that interrupt for nxdk programs
// (xemu issue #1529), so audio stalls there. Instead, a thread checks the
// DMA engine's position every couple of milliseconds and keeps a few
// buffers queued with XAudioProvideSamples(). No interrupt is needed, so
// the same code works on hardware and in xemu.
// ---------------------------------------------------------------------------
}  // namespace
#include <hal/audio.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>
namespace {

const int POLL_FRAMES  = 1024;          // per buffer: ~21 ms at 48 kHz
const int POLL_BUFFERS = 6;             // ring of DMA buffers
const int POLL_AHEAD   = 3;             // buffers kept queued after the playing one
#define AC97_REG8(off) (*(volatile unsigned char*)(0xFEC00000 + (off)))

SDL_mutex*      g_lock = NULL;
SDL_Thread*     g_pollThread = NULL;
volatile bool   g_pollRun = false;
unsigned char*  g_dmaBuf[POLL_BUFFERS];
int             g_nextBuf = 0;

void lockAudio()   { if (g_lock) SDL_LockMutex(g_lock); }
void unlockAudio() { if (g_lock) SDL_UnlockMutex(g_lock); }

void SDLCALL audioCallback(void*, Uint8* stream, int len);

void queueOne()
{
    unsigned char* buf = g_dmaBuf[g_nextBuf];
    g_nextBuf = (g_nextBuf + 1) % POLL_BUFFERS;
    lockAudio();
    audioCallback(NULL, buf, POLL_FRAMES * 4);
    unlockAudio();
    XAudioProvideSamples(buf, POLL_FRAMES * 4, 0);
}

int SDLCALL pollThread(void*)
{
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);
    while (g_pollRun)
    {
        const unsigned char civ = AC97_REG8(0x114);
        const unsigned char lvi = AC97_REG8(0x115);
        const bool halted = (AC97_REG8(0x116) & 1) != 0;   // DMA controller halted
        int ahead = (lvi - civ) & 31;
        if (halted)
        {
            // Ran dry (or not started): refill and restart the engine.
            for (int i = 0; i < POLL_AHEAD; i++) queueOne();
            XAudioPlay();
        }
        else
        {
            while (ahead < POLL_AHEAD) { queueOne(); ahead++; }
        }
        Sleep(2);
    }
    return 0;
}
#else
void lockAudio()   { SDL_LockAudioDevice(g_device); }
void unlockAudio() { SDL_UnlockAudioDevice(g_device); }
#endif

} // namespace

#if defined(NXDK) && defined(AUDIO_POLL)
int Mix_OpenAudio(int, Uint16, int, int)
{
    if (g_device) return 0;
    SDL_zero(g_spec);
    g_spec.freq = 48000;
    g_spec.format = AUDIO_S16SYS;
    g_spec.channels = 2;
    g_spec.samples = POLL_FRAMES;
    memset(g_channels, 0, sizeof(g_channels));

    for (int i = 0; i < POLL_BUFFERS; i++)
    {
        g_dmaBuf[i] = (unsigned char*)MmAllocateContiguousMemoryEx(POLL_FRAMES * 4, 0, 0xFFFFFFFF, 0,
                                                                   PAGE_READWRITE | PAGE_WRITECOMBINE);
        if (!g_dmaBuf[i]) return -1;
        memset(g_dmaBuf[i], 0, POLL_FRAMES * 4);
    }
    g_lock = SDL_CreateMutex();
    XAudioInit(16, 2, NULL, NULL);
    g_device = 1;
    for (int i = 0; i < POLL_AHEAD; i++) queueOne();
    XAudioPlay();

    g_pollRun = true;
    g_pollThread = SDL_CreateThread(pollThread, "audio-poll", NULL);
    return g_pollThread ? 0 : -1;
}

void Mix_CloseAudio(void)
{
    if (!g_device) return;
    g_pollRun = false;
    if (g_pollThread) SDL_WaitThread(g_pollThread, NULL);
    g_pollThread = NULL;
    XAudioPause();
    g_device = 0;
}

// Called on quit, before handing back to the dashboard.
void Mix_HaltOutput(void)
{
    g_pollRun = false;
    if (g_pollThread) SDL_WaitThread(g_pollThread, NULL);
    g_pollThread = NULL;
}
#else
void Mix_HaltOutput(void) {}

int Mix_OpenAudio(int frequency, Uint16, int, int chunksize)
{
    if (g_device) return 0;
    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) < 0)
        return -1;

    SDL_AudioSpec want;
    SDL_zero(want);
    want.freq = frequency > 0 ? frequency : 48000;
#ifdef NXDK
    want.freq = 48000;                       // the Xbox driver always runs at 48 kHz
#endif
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = (Uint16)(chunksize > 0 ? chunksize : 1024);
    want.callback = audioCallback;

    // Always mix in S16 stereo; let SDL convert if the device needs something else.
    g_device = SDL_OpenAudioDevice(NULL, 0, &want, &g_spec, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!g_device) return -1;
    memset(g_channels, 0, sizeof(g_channels));
    SDL_PauseAudioDevice(g_device, 0);
    return 0;
}

void Mix_CloseAudio(void)
{
    if (!g_device) return;
    SDL_CloseAudioDevice(g_device);
    g_device = 0;
    free(g_accum);    g_accum = NULL;
    free(g_musicBuf); g_musicBuf = NULL;
    g_accumSamples = 0;
}
#endif

Mix_Chunk* Mix_LoadWAV(const char* file)
{
    if (!g_device) return NULL;

    SDL_AudioSpec spec;
    Uint8* buf = NULL;
    Uint32 len = 0;
    if (!SDL_LoadWAV(MIXER_PATH(file), &spec, &buf, &len))
        return NULL;

    SDL_AudioCVT cvt;
    if (SDL_BuildAudioCVT(&cvt, spec.format, spec.channels, spec.freq,
                          AUDIO_S16SYS, 2, g_spec.freq) < 0)
    {
        SDL_FreeWAV(buf);
        return NULL;
    }
    cvt.len = (int)len;
    cvt.buf = (Uint8*)malloc(len * cvt.len_mult);
    memcpy(cvt.buf, buf, len);
    SDL_FreeWAV(buf);
    if (cvt.needed) SDL_ConvertAudio(&cvt);

    Mix_Chunk* chunk = (Mix_Chunk*)malloc(sizeof(Mix_Chunk));
    chunk->allocated = 1;
    chunk->abuf = cvt.buf;
    chunk->alen = (Uint32)(cvt.needed ? cvt.len_cvt : cvt.len) & ~3u;
    chunk->volume = MIX_MAX_VOLUME;
    return chunk;
}

void Mix_FreeChunk(Mix_Chunk* chunk)
{
    if (!chunk) return;
    if (g_device) lockAudio();
    for (int c = 0; c < MIX_CHANNELS; c++)
        if (g_channels[c].chunk == chunk) g_channels[c].chunk = NULL;
    if (g_device) unlockAudio();
    free(chunk->abuf);
    free(chunk);
}

int Mix_PlayChannel(int channel, Mix_Chunk* chunk, int loops)
{
    if (!g_device || !chunk || chunk->alen == 0) return -1;
    lockAudio();
    if (channel < 0 || channel >= MIX_CHANNELS)
    {
        channel = -1;
        for (int c = 0; c < MIX_CHANNELS; c++)
            if (!g_channels[c].chunk) { channel = c; break; }
    }
    if (channel >= 0)
    {
        g_channels[channel].chunk = chunk;
        g_channels[channel].pos = 0;
        g_channels[channel].loops = loops;
    }
    unlockAudio();
    return channel;
}

Mix_Music* Mix_LoadMUS(const char* file)
{
    if (!g_device) return NULL;
    Mix_Music* m = (Mix_Music*)calloc(1, sizeof(Mix_Music));
    if (!readFile(MIXER_PATH(file), &m->fileData, &m->fileSize) ||
        !drmp3_init_memory(&m->mp3, m->fileData, m->fileSize, NULL))
    {
        free(m->fileData);
        free(m);
        return NULL;
    }
    m->stream = SDL_NewAudioStream(AUDIO_S16SYS, (Uint8)m->mp3.channels, (int)m->mp3.sampleRate,
                                   AUDIO_S16SYS, 2, g_spec.freq);
    if (!m->stream)
    {
        drmp3_uninit(&m->mp3);
        free(m->fileData);
        free(m);
        return NULL;
    }
    return m;
}

void Mix_FreeMusic(Mix_Music* music)
{
    if (!music) return;
    if (g_device) lockAudio();
    if (g_music == music) g_music = NULL;
    if (g_device) unlockAudio();
    SDL_FreeAudioStream(music->stream);
    drmp3_uninit(&music->mp3);
    free(music->fileData);
    free(music);
}

int Mix_PlayMusic(Mix_Music* music, int loops)
{
    if (!g_device || !music) return -1;
    lockAudio();
    drmp3_seek_to_pcm_frame(&music->mp3, 0);
    SDL_AudioStreamClear(music->stream);
    g_music = music;
    g_musicLoops = loops;   // -1 = forever, like SDL_mixer
    g_musicPaused = false;
    unlockAudio();
    return 0;
}

void Mix_PauseMusic(void)  { g_musicPaused = true; }
void Mix_ResumeMusic(void) { g_musicPaused = false; }

void Mix_HaltMusic(void)
{
    if (!g_device) return;
    lockAudio();
    g_music = NULL;
    g_musicPaused = false;
    unlockAudio();
}

int Mix_PlayingMusic(void)
{
    return g_music != NULL ? 1 : 0;
}

#ifdef DIAG
unsigned Mix_DiagCallbacks() { return g_callbacks; }
int      Mix_DiagFreq()      { return g_device ? g_spec.freq : 0; }
int      Mix_DiagSamples()   { return g_device ? g_spec.samples : 0; }
#endif
