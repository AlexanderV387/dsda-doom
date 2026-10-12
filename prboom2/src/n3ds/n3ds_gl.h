//
// [3DS] DSDA-Doom's OpenGL renderer on pgl (n3ds/pgl.h): the SDL OpenGL
// calls the 3DS does not have, and what DSDA-Doom's shaders did.
//

#ifndef __N3DS_GL__
#define __N3DS_GL__

#include "n3ds/pgl.h"

// SDL on the 3DS has no OpenGL: these are pgl's. Included after SDL.h.
#define SDL_GL_CreateContext(window) N3DS_GLCreateContext()
#define SDL_GL_DeleteContext(context) pglDeleteContext()
#define SDL_GL_SwapWindow(window) N3DS_GLSwap()
#define SDL_GL_SetSwapInterval(interval) N3DS_GLNothing()
#define SDL_GL_GetProcAddress(name) pglGetProcAddress(name)
#define SDL_GL_GetDrawableSize(window, w, h) N3DS_GLDrawableSize(w, h)
#define SDL_GL_SetAttribute(attribute, value) N3DS_GLNothing()
#define SDL_GL_GetAttribute(attribute, value) N3DS_GLGetAttribute(value)

static inline int N3DS_GLNothing(void)
{
  return 0;
}

static inline void N3DS_GLDrawableSize(int *width, int *height)
{
  *width = 400;
  *height = 240;
}

static inline int N3DS_GLGetAttribute(int *value)
{
  *value = 0;
  return 0;
}

void *N3DS_GLCreateContext(void);

// The frame to the top screen, and the bottom screen.
void N3DS_GLSwap(void);

// The palette of the moment (pain, items, radiation suit): textures keep
// the normal one, the frame gets this one's tint on top.
void N3DS_GLSetPalette(int palette);

// An indexed texture (palette index and alpha per pixel, as DSDA-Doom's
// shader takes them) in RGBA, with the normal palette. To free.
unsigned char *N3DS_GLIndexedToRGBA(const unsigned char *indexed, int width, int height);

#endif
