// nxdk / original Xbox implementation of platform.h
#ifdef NXDK

#include "platform.h"
#include "scaler.h"
#include "mixer.h"

#include <hal/video.h>
#include <hal/debug.h>
#include <nxdk/mount.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Saves go to E:\UDATA\<title id>\, which is where dashboards look for them.
// The title ID is written into the XBE by tools/set_titleid.py (same ID as the
// original 2H4UX release, so existing saves show up under the same game).
#define SAVE_DIR "E:\\UDATA\\32483455\\"

#define PCRTC_START       0x00600800
#define PCRTC_INTR        0x00600100
#define PCRTC_INTR_EN     0x00600140
#define NV_PMC_INTR_EN_0  0x00000140

// AC97 (MCPX audio) bus-master control registers, see nxdk hal/audio.c.
#define AC97_MMIO         ((volatile unsigned char*)0xFEC00000)
#define AC97_PCM_OUT_CR   0x11B
#define AC97_PCM_OUT_SR   0x116
#define AC97_SPDIF_CR     0x17B
#define AC97_SPDIF_SR     0x176

extern "C" void usbh_core_deinit(void);

static uint32_t* g_buffers[2] = { NULL, NULL };
// Physical addresses for the CRTC. The CPU sees these buffers through the
// write-combined mapping (0xF0000000+), which the GPU knows nothing about,
// so the scan-out register must be given the real physical address.
static ULONG     g_physical[2] = { 0, 0 };
// False if the scan-out address isn't what we expect: then draw straight into
// the kernel-programmed framebuffer instead of flipping.
static bool      g_flip = false;
static int       g_back = 1;
static int       g_width = 0, g_height = 0;
static char      g_modeName[64] = "none";

#ifdef DIAG
// Diagnostic build (make DIAG=1): the way frames reach the screen can be
// switched at runtime (left-stick click) to compare behaviour in emulators.
//   0 FLIP        - draw into the back buffer, then point PCRTC_START at it
//   1 SINGLE      - wait for vblank, draw into nxdk's framebuffer, never
//                   touch PCRTC_START
//   2 SINGLE+CRTC - as SINGLE, but also write PCRTC_START = nxdk's buffer
//                   every frame
static int        g_presentMode = 0;
static ULONG      g_initPcrtc = 0;       // PCRTC_START right after XVideoSetMode
static ULONG      g_preModePcrtc = 0;    // PCRTC_START before XVideoSetMode
static DWORD      g_encoder = 0;
static void       diagLog(const char* fmt, ...);
static void       diagStartup();
static const char* presentName(int m)
{
    return m == 0 ? "FLIP" : (m == 1 ? "SINGLE" : "SINGLE+CRTC");
}
#endif

void Platform_Init()
{
    if (!nxIsDriveMounted('E'))
        nxMountDrive('E', "\\Device\\Harddisk0\\Partition1\\");
    CreateDirectoryA("E:\\UDATA", NULL);
    CreateDirectoryA("E:\\UDATA\\32483455", NULL);
}

static bool trySetMode(int w, int h)
{
    if (!XVideoSetMode(w, h, 32, REFRESH_DEFAULT))
        return false;
    VIDEO_MODE vm = XVideoGetMode();
    g_width = vm.width;
    g_height = vm.height;
    return g_width > 0 && g_height > 0;
}

bool Platform_VideoInit(int forcedHeight)
{
    // XVideoSetMode only accepts modes that match the AV cable and the
    // HDTV modes enabled in the dashboard, so trying from best to worst
    // gives us "whatever the console is set up for".
    static const int autoModes[][2] = {
        { 1280, 720 },   // 720p  (component, 720p enabled)
        { 1920, 1080 },  // 1080i (component, 1080i enabled)
        { 800,  600 },   // VGA
        { 640,  480 },   // 480p on component if enabled, otherwise SD (480i / PAL)
    };

#ifdef DIAG
    g_preModePcrtc = VIDEOREG(PCRTC_START);
#endif
    bool ok = false;
    if (forcedHeight == 720)  ok = trySetMode(1280, 720);
    if (forcedHeight == 1080) ok = trySetMode(1920, 1080);
    if (forcedHeight == 480)  ok = trySetMode(640, 480);

    for (unsigned i = 0; !ok && i < sizeof(autoModes) / sizeof(autoModes[0]); i++)
        ok = trySetMode(autoModes[i][0], autoModes[i][1]);

    if (!ok)
        return false;

    // Double buffering: nxdk's framebuffer plus one more of the same size.
    const size_t size = (size_t)g_width * g_height * 4;
    g_buffers[0] = (uint32_t*)XVideoGetFB();
    g_buffers[1] = (uint32_t*)MmAllocateContiguousMemoryEx(size, 0, 0x7FFFFFFF, 0x1000,
                                                           PAGE_READWRITE | PAGE_WRITECOMBINE);
    if (g_buffers[1] == NULL)
        return false;
    g_physical[0] = MmGetPhysicalAddress(g_buffers[0]);
    g_physical[1] = MmGetPhysicalAddress(g_buffers[1]);
    // AvSetDisplayMode has just pointed the CRTC at nxdk's framebuffer.
    // Only flip if our idea of its address matches the hardware's.
    g_flip = (VIDEOREG(PCRTC_START) == g_physical[0]);
#ifdef DIAG
    g_initPcrtc = VIDEOREG(PCRTC_START);
    g_encoder = XVideoGetEncoderSettings();
    g_presentMode = g_flip ? 0 : 1;
#endif
    memset(g_buffers[0], 0, size);
    memset(g_buffers[1], 0, size);
    XVideoFlushFB();
    g_back = g_flip ? 1 : 0;

    ScaleRect r = Scaler_Configure(GAME_W, GAME_H, g_width, g_height);
    snprintf(g_modeName, sizeof(g_modeName), "%dx%d (picture %dx%d, %s)",
             g_width, g_height, r.w, r.h, g_flip ? "double-buffered" : "single buffer");
#ifdef DIAG
    diagStartup();
#endif
    return true;
}

#ifdef DIAG
static Uint64 g_scaleTicks = 0;      // time spent scaling the last frame
static Uint64 g_lastPresent = 0;
static double g_frameMsAvg = 0, g_scaleMsAvg = 0;
#endif

void Platform_Present(SDL_Surface* frame)
{
#ifdef DIAG
    const Uint64 freq = SDL_GetPerformanceFrequency();
    const Uint64 now = SDL_GetPerformanceCounter();
    if (g_lastPresent)
        g_frameMsAvg = g_frameMsAvg * 0.95 + 0.05 * (double)(now - g_lastPresent) * 1000.0 / freq;
    g_lastPresent = now;

    if (g_presentMode != 0)
    {
        XVideoWaitForVBlank();
        if (g_presentMode == 2)
            VIDEOREG(PCRTC_START) = g_physical[0];
        const Uint64 t0 = SDL_GetPerformanceCounter();
        if (SDL_MUSTLOCK(frame)) SDL_LockSurface(frame);
        Scaler_Run((const uint32_t*)frame->pixels, frame->pitch / 4, g_buffers[0], g_width);
        if (SDL_MUSTLOCK(frame)) SDL_UnlockSurface(frame);
        XVideoFlushFB();
        g_scaleMsAvg = g_scaleMsAvg * 0.95 + 0.05 * (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / freq;
        return;
    }
    const Uint64 t0 = SDL_GetPerformanceCounter();
#endif
    if (SDL_MUSTLOCK(frame)) SDL_LockSurface(frame);
    Scaler_Run((const uint32_t*)frame->pixels, frame->pitch / 4,
               g_buffers[g_back], g_width);
    if (SDL_MUSTLOCK(frame)) SDL_UnlockSurface(frame);
#ifdef DIAG
    g_scaleMsAvg = g_scaleMsAvg * 0.95 + 0.05 * (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / freq;
#endif

    XVideoFlushFB();          // drain write-combine buffers
    XVideoWaitForVBlank();
    if (g_flip)
    {
        VIDEOREG(PCRTC_START) = g_physical[g_back];
        g_back ^= 1;
    }
}

const char* Platform_VideoModeName()
{
    return g_modeName;
}

const char* Platform_SavePath(const char* fileName)
{
    // Callers keep the pointer (CParam/CHighscore store it), so each call
    // gets its own copy. Only called a couple of times at startup.
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s%s", SAVE_DIR, fileName);
    return strdup(path);
}

// Quiet every device that can raise an interrupt into this program's code.
// A quick reboot loads the dashboard over our memory; an interrupt arriving
// at one of our handlers during that hangs the console (audio keeps looping
// its last buffers, which is the popping heard on exit).
static void quiesceHardware()
{
    // Audio: stop both AC97 output DMA engines with interrupts disabled,
    // then clear any pending status. nxdk has no call that disconnects its
    // audio ISR, but with the engines stopped it never fires.
    Mix_HaltOutput();   // polled backend: stop the feeder thread first
    {
        AC97_MMIO[AC97_PCM_OUT_CR] = 0;
        AC97_MMIO[AC97_SPDIF_CR] = 0;
        AC97_MMIO[AC97_PCM_OUT_SR] = 0xFF;
        AC97_MMIO[AC97_SPDIF_SR] = 0xFF;
    }

    // USB: close the pads, then shut down nxdk's OHCI stack, which
    // disconnects its interrupt (SDL deliberately leaves this to the app).
    if (SDL_WasInit(SDL_INIT_JOYSTICK))
        SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK);
    usbh_core_deinit();

    // Video: no vblank interrupts, and show nxdk's own framebuffer again.
    VIDEOREG(PCRTC_INTR_EN) = 0;
    VIDEOREG(PCRTC_INTR) = 1;
    VIDEOREG(NV_PMC_INTR_EN_0) = 0;
    if (g_flip)
        VIDEOREG(PCRTC_START) = g_physical[0];
}

void Platform_Exit(int code)
{
    // Go straight back to the dashboard. exit() would get there too, but
    // only after running static destructors and SDL_Quit; SDL's Xbox audio
    // shutdown re-runs XAudioInit and waits on the audio thread.
    // Config and high scores are already written and closed by now.
    (void)code;
    quiesceHardware();
    HalReturnToFirmware(HalQuickRebootRoutine);
}

#ifdef DIAG
// ---------------------------------------------------------------------------
// Diagnostic overlay, controls and log (make DIAG=1)
// ---------------------------------------------------------------------------
#include <stdarg.h>
#include <SDL_ttf.h>

extern unsigned Mix_DiagCallbacks();
extern int      Mix_DiagFreq();
extern int      Mix_DiagSamples();

#define DIAG_LOG SAVE_DIR "diag.txt"
#define AC97_NAM16(reg) (*(volatile unsigned short*)(0xFEC00000 + (reg)))
#define AC97_NABM16(reg) (*(volatile unsigned short*)(0xFEC00000 + (reg)))

static bool      g_audioUnmuted = false;
static int       g_logLines = 0;

static void diagLog(const char* fmt, ...)
{
    if (g_logLines > 2000) return;
    FILE* f = fopen(DIAG_LOG, "a");
    if (!f) return;
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    // No %f: nxdk's printf (PDCLib) does not support floating point and
    // misreads every argument after one.
    const Uint32 t = SDL_GetTicks();
    fprintf(f, "[%5lu.%03lu] %s\n", (unsigned long)(t / 1000), (unsigned long)(t % 1000), line);
    fclose(f);
    g_logLines++;
}

static const char* avPackName(DWORD s)
{
    switch (s & VIDEO_ADAPTER_MASK)
    {
    case AV_PACK_SCART:    return "SCART";
    case AV_PACK_HDTV:     return "HDTV(component)";
    case AV_PACK_VGA:      return "VGA";
    case AV_PACK_RFU:      return "RF";
    case AV_PACK_SVIDEO:   return "S-Video";
    case AV_PACK_STANDARD: return "Composite";
    default:               return "none/unknown";
    }
}

static void diagStartup()
{
    DeleteFileA(DIAG_LOG);
    diagLog("2H4UX diagnostic build " __DATE__ " " __TIME__);
    VIDEO_MODE vm = XVideoGetMode();
    diagLog("video: %dx%d bpp %d refresh %d", vm.width, vm.height, vm.bpp, vm.refresh);
    diagLog("encoder settings 0x%08lX  AV pack %s  flags:%s%s%s%s%s",
            g_encoder, avPackName(g_encoder),
            (g_encoder & VIDEO_WIDESCREEN) ? " widescreen" : "",
            (g_encoder & VIDEO_MODE_480P) ? " 480p" : "",
            (g_encoder & VIDEO_MODE_720P) ? " 720p" : "",
            (g_encoder & VIDEO_MODE_1080I) ? " 1080i" : "",
            (g_encoder & VIDEO_60Hz) ? " 60Hz" : "");
    diagLog("PCRTC_START before SetMode 0x%08lX  after 0x%08lX", g_preModePcrtc, g_initPcrtc);
    diagLog("fb0 virt %p phys 0x%08lX   fb1 virt %p phys 0x%08lX",
            g_buffers[0], g_physical[0], g_buffers[1], g_physical[1]);
    diagLog("AvGetSavedDataAddress %p", AvGetSavedDataAddress());
    diagLog("flip %s -> start mode %s", g_flip ? "enabled" : "DISABLED (PCRTC mismatch)",
            presentName(g_presentMode));
}

void Platform_DiagCycleVideo()
{
    g_presentMode = (g_presentMode + 1) % 3;
    if (g_presentMode == 1)
    {
        // Entering SINGLE: show nxdk's buffer once (we may be on buffer 1),
        // then never touch the register again.
        VIDEOREG(PCRTC_START) = g_physical[0];
    }
    if (g_presentMode == 0)
        g_back = 1;
    diagLog("present mode -> %s", presentName(g_presentMode));
}

void Platform_DiagToggleAudio()
{
    g_audioUnmuted = !g_audioUnmuted;
    if (g_audioUnmuted)
    {
        AC97_NAM16(0x02) = 0x0000;   // master volume 0 dB, unmuted
        AC97_NAM16(0x18) = 0x0808;   // PCM out 0 dB, unmuted
    }
    else
    {
        AC97_NAM16(0x02) = 0x8000;   // muted (AC97 reset default)
        AC97_NAM16(0x18) = 0x8808;
    }
    diagLog("codec volumes -> %s (master 0x%04X pcm 0x%04X)",
            g_audioUnmuted ? "UNMUTED" : "MUTED", AC97_NAM16(0x02), AC97_NAM16(0x18));
}

// Text overlay at the top of the 800x600 game frame.
void Platform_DiagOverlay(SDL_Surface* screen)
{
    static TTF_Font*    font = NULL;
    static SDL_Surface* lines[6] = { 0 };
    static Uint32       nextUpdate = 0, nextLog = 0;
    static unsigned     lastCallbacks = 0;
    static int          lastCiv = -1, civChanges = 0;
    static Uint32       rateStart = 0;
    static float        callbacksPerSec = 0, civPerSec = 0;

    if (!font)
    {
        if (!TTF_WasInit()) TTF_Init();
        font = TTF_OpenFont("D:\\data\\bgothm.ttf", 13);
        if (!font) return;
        rateStart = SDL_GetTicks();
    }

    // Sample the AC97 current-buffer index often to see whether DMA moves.
    const int civ = AC97_MMIO[0x114];
    if (lastCiv >= 0 && civ != lastCiv) civChanges++;
    lastCiv = civ;

    const Uint32 now = SDL_GetTicks();
    if (now - rateStart >= 1000)
    {
        const unsigned cb = Mix_DiagCallbacks();
        callbacksPerSec = (cb - lastCallbacks) * 1000.0f / (now - rateStart);
        civPerSec = civChanges * 1000.0f / (now - rateStart);
        lastCallbacks = cb;
        civChanges = 0;
        rateStart = now;
    }

    char text[6][200];
    snprintf(text[0], 200, "DIAG  %s  AV %s  enc 0x%08lX   L3=video mode  R3=codec mute",
             g_modeName, avPackName(g_encoder), g_encoder);
    snprintf(text[1], 200, "present %s   PCRTC now 0x%08lX  init 0x%08lX  pre 0x%08lX  fb0 0x%08lX fb1 0x%08lX",
             presentName(g_presentMode), (ULONG)VIDEOREG(PCRTC_START), g_initPcrtc, g_preModePcrtc,
             g_physical[0], g_physical[1]);
    // Integers only (see diagLog): tenths of a millisecond.
    const int frame10 = (int)(g_frameMsAvg * 10.0), scale10 = (int)(g_scaleMsAvg * 10.0);
    const int fps = g_frameMsAvg > 0 ? (int)(1000.0 / g_frameMsAvg + 0.5) : 0;
    snprintf(text[2], 200, "frame %d.%d ms (%d fps)   scale %d.%d ms",
             frame10 / 10, frame10 % 10, fps, scale10 / 10, scale10 % 10);
    snprintf(text[3], 200, "audio %d Hz x%d  mixer callbacks %u (%d/s)",
             Mix_DiagFreq(), Mix_DiagSamples(), Mix_DiagCallbacks(), (int)callbacksPerSec);
    snprintf(text[4], 200, "AC97 CR 0x%02X SR 0x%04X CIV %d LVI %d PICB %d  CIV moves %d/s",
             AC97_MMIO[0x11B], AC97_NABM16(0x116), civ, AC97_MMIO[0x115], AC97_NABM16(0x118), (int)civPerSec);
    snprintf(text[5], 200, "codec master 0x%04X  pcm 0x%04X  pwr 0x%04X  vendor %04X%04X  unmute %s",
             AC97_NAM16(0x02), AC97_NAM16(0x18), AC97_NAM16(0x26), AC97_NAM16(0x7C), AC97_NAM16(0x7E),
             g_audioUnmuted ? "ON" : "off");

    if (now >= nextUpdate)
    {
        nextUpdate = now + 250;
        SDL_Color white = { 255, 255, 255, 255 }, black = { 0, 0, 0, 255 };
        for (int i = 0; i < 6; i++)
        {
            if (lines[i]) SDL_FreeSurface(lines[i]);
            lines[i] = TTF_RenderText_Shaded(font, text[i], white, black);
        }
    }
    if (now >= nextLog)
    {
        nextLog = now + 2000;
        for (int i = 1; i < 6; i++) diagLog("%s", text[i]);
    }

    SDL_Rect pos = { 4, 4, 0, 0 };
    for (int i = 0; i < 6; i++)
    {
        if (!lines[i]) continue;
        SDL_BlitSurface(lines[i], NULL, screen, &pos);
        pos.y += lines[i]->h;
    }
}
#endif // DIAG

#endif // NXDK
