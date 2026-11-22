/*
    SDL 1.2 / Xbox SDK compatibility for the nxdk (SDL2) port.

    The game was written against SDL 1.2 and the XDK. Rather than rewrite
    every call site, the handful of old APIs it uses are mapped here onto
    SDL2 with the same behaviour.
*/
#ifndef COMPAT_H
#define COMPAT_H

#include <SDL.h>
#include <stdio.h>
#include "platform.h"

// SDL 1.2 surface flags (ignored by SDL2's SDL_CreateRGBSurface).
#ifndef SDL_HWSURFACE
#define SDL_HWSURFACE 0
#endif
#ifndef SDL_SWSURFACE
#define SDL_SWSURFACE 0
#endif
#ifndef SDL_SRCALPHA
#define SDL_SRCALPHA 0x00010000
#endif

// The game's screen is 32-bit XRGB.
#define COMPAT_SCREEN_FORMAT SDL_PIXELFORMAT_RGB888

// SDL_DisplayFormat: convert to the screen format, dropping per-pixel alpha
// (SDL 1.2 behaviour). A colour key, if any, is kept.
static inline SDL_Surface* SDL_DisplayFormat(SDL_Surface* s)
{
    if (!s) return NULL;
    SDL_Surface* r = SDL_ConvertSurfaceFormat(s, COMPAT_SCREEN_FORMAT, 0);
    if (r) SDL_SetSurfaceBlendMode(r, SDL_BLENDMODE_NONE);
    return r;
}

// SDL_DisplayFormatAlpha: convert to 32-bit ARGB keeping per-pixel alpha.
static inline SDL_Surface* SDL_DisplayFormatAlpha(SDL_Surface* s)
{
    if (!s) return NULL;
    SDL_Surface* r = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ARGB8888, 0);
    if (r) SDL_SetSurfaceBlendMode(r, SDL_BLENDMODE_BLEND);
    return r;
}

// SDL_SetAlpha(surface, SDL_SRCALPHA, a): whole-surface transparency.
static inline int SDL_SetAlpha(SDL_Surface* s, Uint32 flags, Uint8 alpha)
{
    if (!s) return -1;
    if (flags & SDL_SRCALPHA)
    {
        SDL_SetSurfaceAlphaMod(s, alpha);
        return SDL_SetSurfaceBlendMode(s, SDL_BLENDMODE_BLEND);
    }
    SDL_SetSurfaceAlphaMod(s, 255);
    return SDL_SetSurfaceBlendMode(s, SDL_BLENDMODE_NONE);
}

// XDK: XLaunchNewImage(NULL, NULL) returned to the dashboard.
#define XLaunchNewImage(a, b) Platform_Exit(0)

// Host test build: translate the game's "D:\\data\\..." paths.
#ifndef NXDK
const char* Host_Path(const char* xboxPath);
#define IMG_Load(p)         IMG_Load(Host_Path(p))
#define TTF_OpenFont(p, s)  TTF_OpenFont(Host_Path(p), s)
#define fopen(p, m)         fopen(Host_Path(p), m)
#endif

#endif
